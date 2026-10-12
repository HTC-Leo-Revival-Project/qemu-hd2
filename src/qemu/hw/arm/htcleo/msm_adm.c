/*
 * Qualcomm ADM / DMOV data mover (0xA9700000)
 *
 * Register map from arch/arm/mach-msm/include/mach/dma.h.  The apps
 * processor owns security domain 3 (+0xC00) but the other domain windows
 * behave the same here.  Per channel:
 *   CMD_PTR 0x000  RSLT 0x040  FLUSH0..5 0x080..0x1C0  STATUS 0x200
 *   CONFIG  0x300  ISR 0x380 (domain-wide)
 *
 * Writing CMD_PTR runs the whole command list at once: the peripherals the
 * data mover talks to (NAND, SDCC, ...) complete their side synchronously,
 * so CRCI flow control never has to stall.  One result (0x80000002) is
 * queued per command and INT_ADM_AARM is raised while any domain ISR bit
 * is pending.
 *
 * The exceptions are paced peripherals.  A paced receive (UART DM RX FIFO
 * -> memory) stays on its channel until the peripheral pushes bytes with
 * msm_adm_rx_push(), and completes when it is full or the driver flushes.
 * A CRCI window (the SD controllers' FIFOs, msm_adm_add_crci()) holds the
 * channel until its peripheral says the transfer can go: a driver may
 * start the data mover before it sends the card its read command, as
 * MAGLDR does.  The peripheral calls msm_adm_crci_kick() when that
 * changes, and the channel carries on from the waiting descriptor.
 *
 * The four security domains also have a page each, at 0xA9400000 (SD0),
 * 0xA9500000 (SD1), 0xA9600000 (SD2) and 0xA9700000 (SD3), with the
 * domain's registers at its usual offset in the page: AMSS drives SD0
 * through 0xA9400000.  Interrupt 0 is raised for any domain; interrupts
 * 1-4 are the per-domain lines, SD3 being the apps processor's
 * INT_ADM_AARM and SD0 the modem's.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "htcleo.h"
#include "../trace.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmAdmState, MSM_ADM)

#define ADM_CHANNELS    16
#define ADM_DOMAINS     4
#define RSLT_FIFO       8

#define CMD_PTR_LP      (1u << 31)
#define CMD_LC          (1u << 31)
#define CMD_MODE_MASK   3
#define CMD_MODE_SINGLE 0
#define CMD_MODE_SG     1
#define CMD_MODE_BOX    3
#define CMD_SRC_SWAP_BYTES  (1u << 11)
#define CMD_SRC_SWAP_SHORTS (1u << 12)
#define CMD_SRC_SWAP_WORDS  (1u << 13)
#define CMD_DST_SWAP_BYTES  (1u << 14)
#define CMD_DST_SWAP_SHORTS (1u << 15)
#define CMD_DST_SWAP_WORDS  (1u << 16)

#define RSLT_VALID      (1u << 31)
#define RSLT_DONE       (1u << 1)

#define STATUS_CMD_PTR_RDY  (1u << 0)
#define STATUS_RSLT_VALID   (1u << 1)

typedef struct AdmChannel {
    uint32_t rslt[RSLT_FIFO];
    int rslt_head, rslt_count;
    uint32_t config;
    bool stalled;               /* waiting on its CRCI */
    uint32_t stall_desc;        /* the descriptor it waits on */
    hwaddr stall_list;          /* its pointer list, 0 for a plain array */
    int stall_idx;              /* and the entry being run */
    uint32_t fill;              /* bytes a paced source has pushed so far */
} AdmChannel;

#define ADM_RX_SOURCES  2

typedef struct AdmRxSource {
    hwaddr base, size;
    MsmAdmRxReady ready;
    void *opaque;
} AdmRxSource;

#define ADM_CRCI_SOURCES    4

typedef struct AdmCrci {
    hwaddr base, size;
    MsmAdmCrciReady ready;
    void *opaque;
} AdmCrci;

#define RSLT_FLUSH      (1u << 2)

/*
 * Flow control: an element with a CRCI waits until its peripheral raises
 * the request line.  The NAND controller always has its data ready, so
 * its transfers run at once; the SD controllers are CRCI windows.  Other paced peripherals (UART DM,
 * crypto, TSIF...) never raise their CRCI here, so such a command stays
 * queued until the driver flushes it.  Board files reassign CRCI numbers
 * (htcleo gives the BT UART CRCI 6, like SDC1), so decide by the peripheral
 * window the element touches rather than by number.
 */
static bool addr_serviced(uint32_t a)
{
    return (a >= MSM_NAND_PHYS && a < MSM_NAND_PHYS + 0x1000) ||
           (a >= MSM_SDC1_PHYS && a < MSM_SDC4_PHYS + 0x1000) ||
           a < HTCLEO_RAM_SIZE;
}

static bool uartdm_window(uint32_t a)
{
    return (a >= MSM_UARTDM1_PHYS && a < MSM_UARTDM1_PHYS + 0x1000) ||
           (a >= MSM_UARTDM2_PHYS && a < MSM_UARTDM2_PHYS + 0x1000);
}

struct MsmAdmState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;               /* any domain (one processor owns them all) */
    qemu_irq dom_irq[ADM_DOMAINS];
    AdmChannel ch[ADM_DOMAINS][ADM_CHANNELS];
    uint32_t isr[ADM_DOMAINS];
    uint32_t regs[0x400];
    uint64_t commands;
    AdmRxSource rx_src[ADM_RX_SOURCES];
    AdmCrci crci[ADM_CRCI_SOURCES];
    bool running;               /* inside a command list */
};

static AdmCrci *crci_window(MsmAdmState *s, uint32_t a)
{
    for (int i = 0; i < ADM_CRCI_SOURCES; i++) {
        AdmCrci *w = &s->crci[i];
        if (w->ready && a >= w->base && a < w->base + w->size) {
            return w;
        }
    }
    return NULL;
}

static bool crci_serviced(MsmAdmState *s, uint32_t cmd, uint32_t src,
                          uint32_t dst)
{
    AdmCrci *w;

    /* UART DM RX is paced by the UART (msm_adm_rx_push); TX never fills */
    if (uartdm_window(src)) {
        return false;
    }
    if (uartdm_window(dst)) {
        return true;
    }
    if (!(cmd & (0xFu << 3)) && !(cmd & (0xFu << 7))) {
        return true;
    }
    w = crci_window(s, src);
    if (w && !w->ready(w->opaque, false)) {
        return false;
    }
    w = crci_window(s, dst);
    if (w && !w->ready(w->opaque, true)) {
        return false;
    }
    return addr_serviced(src) && addr_serviced(dst);
}

static void adm_update(MsmAdmState *s)
{
    bool level = false;

    for (int d = 0; d < ADM_DOMAINS; d++) {
        if (s->isr[d]) {
            level = true;
        }
        qemu_set_irq(s->dom_irq[d], s->isr[d] != 0);
    }
    qemu_set_irq(s->irq, level);
}

static uint32_t ld32(hwaddr a)
{
    return address_space_ldl_le(&address_space_memory, a,
                                MEMTXATTRS_UNSPECIFIED, NULL);
}

static void swap_buf(uint8_t *b, uint32_t len, uint32_t cmd, bool src)
{
    uint32_t bytes = src ? CMD_SRC_SWAP_BYTES : CMD_DST_SWAP_BYTES;
    uint32_t shorts = src ? CMD_SRC_SWAP_SHORTS : CMD_DST_SWAP_SHORTS;
    uint32_t words = src ? CMD_SRC_SWAP_WORDS : CMD_DST_SWAP_WORDS;

    if (cmd & bytes) {
        for (uint32_t i = 0; i + 1 < len; i += 2) {
            uint8_t t = b[i]; b[i] = b[i + 1]; b[i + 1] = t;
        }
    }
    if (cmd & shorts) {
        for (uint32_t i = 0; i + 3 < len; i += 4) {
            uint16_t t;
            memcpy(&t, b + i, 2);
            memmove(b + i, b + i + 2, 2);
            memcpy(b + i + 2, &t, 2);
        }
    }
    if (cmd & words) {
        for (uint32_t i = 0; i + 7 < len; i += 8) {
            uint32_t t;
            memcpy(&t, b + i, 4);
            memmove(b + i, b + i + 4, 4);
            memcpy(b + i + 4, &t, 4);
        }
    }
}

static void adm_copy(uint32_t cmd, hwaddr src, hwaddr dst, uint32_t len)
{
    g_autofree uint8_t *buf = NULL;

    if (!len) {
        return;
    }
    buf = g_malloc(len);
    address_space_read(&address_space_memory, src, MEMTXATTRS_UNSPECIFIED,
                       buf, len);
    swap_buf(buf, len, cmd, true);
    swap_buf(buf, len, cmd, false);
    address_space_write(&address_space_memory, dst, MEMTXATTRS_UNSPECIFIED,
                        buf, len);
}

/*
 * Run one array of command descriptors: 1 done, 0 bad list, -1 stalled
 * (*stall is then the descriptor that waits)
 */
static int adm_run_array(MsmAdmState *s, hwaddr a, uint32_t *stall)
{
    for (int guard = 0; guard < 4096; guard++) {
        uint32_t cmd = ld32(a);
        trace_msm_adm_desc(a, cmd, ld32(a + 4), ld32(a + 8), ld32(a + 12));
        if (!crci_serviced(s, cmd, ld32(a + 4), ld32(a + 8))) {
            *stall = a;
            return -1;
        }
        switch (cmd & CMD_MODE_MASK) {
        case CMD_MODE_SINGLE: {
            uint32_t src = ld32(a + 4), dst = ld32(a + 8), len = ld32(a + 12);
            adm_copy(cmd, src, dst, len & 0xFFFF);
            a += 16;
            break;
        }
        case CMD_MODE_BOX: {
            uint32_t src = ld32(a + 4), dst = ld32(a + 8);
            uint32_t lens = ld32(a + 12), rows = ld32(a + 16);
            uint32_t offs = ld32(a + 20);
            uint32_t src_len = lens >> 16, dst_len = lens & 0xFFFF;
            uint32_t src_rows = rows >> 16, dst_rows = rows & 0xFFFF;
            uint32_t src_off = offs >> 16, dst_off = offs & 0xFFFF;
            uint32_t len = MIN(src_len, dst_len);
            uint32_t n = MIN(src_rows, dst_rows);
            for (uint32_t r = 0; r < n; r++) {
                adm_copy(cmd, src + r * src_off, dst + r * dst_off, len);
            }
            a += 24;
            break;
        }
        case CMD_MODE_SG:
        default:
            qemu_log_mask(LOG_UNIMP, "msm-adm: unsupported cmd mode %u\n",
                          cmd & CMD_MODE_MASK);
            return 0;
        }
        if (cmd & CMD_LC) {
            return 1;
        }
    }
    return 0;
}

static void adm_post_result(MsmAdmState *s, int dom, int chn, uint32_t r)
{
    AdmChannel *c = &s->ch[dom][chn];

    if (c->rslt_count < RSLT_FIFO) {
        c->rslt[(c->rslt_head + c->rslt_count) % RSLT_FIFO] = r;
        c->rslt_count++;
    }
    /* only channels configured with IRQ_EN report to the interrupt line */
    if (c->config & 1) {
        s->isr[dom] |= 1u << chn;
        adm_update(s);
    }
}

static void adm_rx_wake(MsmAdmState *s, uint32_t desc)
{
    uint32_t src = ld32(desc + 4);

    for (int i = 0; i < ADM_RX_SOURCES; i++) {
        AdmRxSource *r = &s->rx_src[i];
        if (r->ready && src >= r->base && src < r->base + r->size) {
            r->ready(r->opaque);
        }
    }
}

/*
 * Run a channel from descriptor a to the end of its command.  list is the
 * pointer list being walked (0 for a single array) and idx its entry that
 * addresses a's array.
 */
static void adm_run(MsmAdmState *s, int dom, int chn, hwaddr a, hwaddr list,
                    int idx)
{
    AdmChannel *c = &s->ch[dom][chn];
    uint32_t stall = 0;

    s->running = true;
    for (;;) {
        if (adm_run_array(s, a, &stall) < 0) {
            c->stalled = true;
            c->stall_desc = stall;
            c->stall_list = list;
            c->stall_idx = idx;
            c->fill = 0;
            s->running = false;
            adm_rx_wake(s, stall);
            return;
        }
        if (!list || (ld32(list + 4 * idx) & CMD_PTR_LP) || ++idx >= 256) {
            break;
        }
        a = (hwaddr)(ld32(list + 4 * idx) & 0x1FFFFFFF) << 3;
    }
    s->running = false;
    adm_post_result(s, dom, chn, RSLT_VALID | RSLT_DONE);
}

static void adm_execute(MsmAdmState *s, int dom, int chn, uint32_t cmdptr)
{
    uint32_t mode = (cmdptr >> 29) & 7;
    hwaddr a = (hwaddr)(cmdptr & 0x1FFFFFFF) << 3;

    s->commands++;
    trace_msm_adm_execute(dom, chn, cmdptr);
    if (mode == 0 || mode == 1) {
        /*
         * pointer list: each entry addresses an array of descriptors.
         * Mode 0 ("CMD_LIST, does not work" in mach/dma.h) is walked the
         * same way: msm_serial_hs submits pointer lists with it.
         */
        adm_run(s, dom, chn, (hwaddr)(ld32(a) & 0x1FFFFFFF) << 3, a, 0);
    } else {
        adm_run(s, dom, chn, a, 0, 0);
    }
}

void msm_adm_add_crci(DeviceState *dev, hwaddr base, hwaddr size,
                      MsmAdmCrciReady ready, void *opaque)
{
    MsmAdmState *s = MSM_ADM(dev);

    for (int i = 0; i < ADM_CRCI_SOURCES; i++) {
        if (!s->crci[i].ready) {
            s->crci[i] = (AdmCrci) { base, size, ready, opaque };
            return;
        }
    }
    g_assert_not_reached();
}

/* A CRCI peripheral's state changed: resume the channels it can serve. */
void msm_adm_crci_kick(DeviceState *dev)
{
    MsmAdmState *s = MSM_ADM(dev);

    if (s->running) {
        return;
    }
    for (int d = 0; d < ADM_DOMAINS; d++) {
        for (int n = 0; n < ADM_CHANNELS; n++) {
            AdmChannel *c = &s->ch[d][n];
            uint32_t a = c->stall_desc;

            if (!c->stalled ||
                (!crci_window(s, ld32(a + 4)) &&
                 !crci_window(s, ld32(a + 8))) ||
                !crci_serviced(s, ld32(a), ld32(a + 4), ld32(a + 8))) {
                continue;
            }
            c->stalled = false;
            trace_msm_adm_resume(d, n, a);
            adm_run(s, d, n, a, c->stall_list, c->stall_idx);
        }
    }
}

void msm_adm_set_rx_source(DeviceState *dev, hwaddr base, hwaddr size,
                           MsmAdmRxReady ready, void *opaque)
{
    MsmAdmState *s = MSM_ADM(dev);

    for (int i = 0; i < ADM_RX_SOURCES; i++) {
        if (!s->rx_src[i].ready) {
            s->rx_src[i] = (AdmRxSource) { base, size, ready, opaque };
            return;
        }
    }
}

/*
 * Bytes from a paced peripheral FIFO in [base, base + size) for the box
 * command waiting on it.  Returns how many were taken, or -1 when no
 * transfer is waiting.  A full transfer completes with a DONE result.
 */
int msm_adm_rx_push(DeviceState *dev, hwaddr base, hwaddr size,
                    const uint8_t *buf, int len)
{
    MsmAdmState *s = MSM_ADM(dev);

    for (int d = 0; d < ADM_DOMAINS; d++) {
        for (int n = 0; n < ADM_CHANNELS; n++) {
            AdmChannel *c = &s->ch[d][n];
            uint32_t a = c->stall_desc, cmd, src, dst, lens, rows, offs;
            uint32_t row_len, nrows, dst_off, cap, take, done = 0;

            if (!c->stalled) {
                continue;
            }
            cmd = ld32(a);
            src = ld32(a + 4);
            if (src < base || src >= base + size ||
                (cmd & CMD_MODE_MASK) != CMD_MODE_BOX) {
                continue;
            }
            dst = ld32(a + 8);
            lens = ld32(a + 12);
            rows = ld32(a + 16);
            offs = ld32(a + 20);
            row_len = MIN(lens >> 16, lens & 0xFFFF);
            nrows = MIN(rows >> 16, rows & 0xFFFF);
            dst_off = offs & 0xFFFF;
            cap = row_len * nrows;
            take = MIN((uint32_t)len, cap - c->fill);
            while (done < take) {
                uint32_t r = (c->fill + done) / row_len;
                uint32_t col = (c->fill + done) % row_len;
                uint32_t chunk = MIN(take - done, row_len - col);
                address_space_write(&address_space_memory,
                                    dst + r * dst_off + col,
                                    MEMTXATTRS_UNSPECIFIED, buf + done, chunk);
                done += chunk;
            }
            c->fill += take;
            if (c->fill >= cap) {
                c->stalled = false;
                adm_post_result(s, d, n, RSLT_VALID | RSLT_DONE);
            }
            return take;
        }
    }
    return -1;
}

static uint64_t adm_read(void *opaque, hwaddr off, unsigned size)
{
    MsmAdmState *s = opaque;
    int dom = (off >> 10) & 3;
    hwaddr r = off & 0x3FF;
    int chn = (r >> 2) & 0xF;
    AdmChannel *c = &s->ch[dom][chn];
    uint32_t v;

    if (r < 0x40) {
        return 0;                               /* CMD_PTR */
    } else if (r < 0x80) {                      /* RSLT */
        if (!c->rslt_count) {
            return 0;
        }
        v = c->rslt[c->rslt_head];
        c->rslt_head = (c->rslt_head + 1) % RSLT_FIFO;
        c->rslt_count--;
        return v;
    } else if (r < 0x200) {
        return 0;                               /* FLUSH0..5 */
    } else if (r < 0x240) {                     /* STATUS */
        v = c->stalled ? 0 : STATUS_CMD_PTR_RDY;
        if (c->rslt_count) {
            v |= STATUS_RSLT_VALID | ((uint32_t)MIN(c->rslt_count, 7) << 29);
        }
        return v;
    } else if (r >= 0x300 && r < 0x340) {
        return c->config;
    } else if (r == 0x380) {                    /* ISR: read clears */
        v = s->isr[dom];
        s->isr[dom] = 0;
        adm_update(s);
        return v;
    }
    return s->regs[(off >> 2) & 0x3FF];
}

static void adm_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MsmAdmState *s = opaque;
    int dom = (off >> 10) & 3;
    hwaddr r = off & 0x3FF;
    int chn = (r >> 2) & 0xF;

    if (r < 0x40) {
        adm_execute(s, dom, chn, val);
    } else if (r >= 0x80 && r < 0x200) {
        /* FLUSH0..5: abort a command stalled on its CRCI */
        if (s->ch[dom][chn].stalled) {
            s->ch[dom][chn].stalled = false;
            adm_post_result(s, dom, chn, RSLT_VALID | RSLT_FLUSH);
        }
    } else if (r >= 0x300 && r < 0x340) {
        AdmChannel *c = &s->ch[dom][chn];
        c->config = val;
        if ((val & 1) && c->rslt_count) {
            s->isr[dom] |= 1u << chn;
            adm_update(s);
        }
    } else {
        s->regs[(off >> 2) & 0x3FF] = val;
    }
}

static const MemoryRegionOps adm_ops = {
    .read = adm_read,
    .write = adm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void adm_reset(DeviceState *dev)
{
    MsmAdmState *s = MSM_ADM(dev);

    memset(s->ch, 0, sizeof(s->ch));
    memset(s->isr, 0, sizeof(s->isr));
    adm_update(s);
}

static void adm_init(Object *obj)
{
    MsmAdmState *s = MSM_ADM(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &adm_ops, s, "msm-adm", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    for (int d = 0; d < ADM_DOMAINS; d++) {
        sysbus_init_irq(sbd, &s->dom_irq[d]);
    }
}

static void adm_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    device_class_set_legacy_reset(dc, adm_reset);
}

static const TypeInfo msm_adm_info = {
    .name = TYPE_MSM_ADM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmAdmState),
    .instance_init = adm_init,
    .class_init = adm_class_init,
};

static void msm_adm_register(void)
{
    type_register_static(&msm_adm_info);
}
type_init(msm_adm_register)
