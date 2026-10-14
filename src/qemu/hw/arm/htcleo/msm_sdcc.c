/*
 * Qualcomm MSM SD card controller (SDCC), as driven by
 * drivers/mmc/host/msm_sdcc.c.  It is an ARM MMCI derivative:
 *
 *   0x00 POWER  0x04 CLOCK  0x08 ARGUMENT  0x0C COMMAND  0x10 RESPCMD
 *   0x14..0x20 RESPONSE0-3  0x24 DATATIMER  0x28 DATALENGTH
 *   0x2C DATACTRL (bit0 enable, bit1 read, bit3 DMA, 15:4 block size)
 *   0x30 DATACNT  0x34 STATUS  0x38 CLEAR  0x3C MASK0 (cmd_irq)
 *   0x40 MASK1 (pio_irq)  0x44 FIFOCNT  0x80..0xBC FIFO
 *
 * The data path is FIFO based.  Bulk transfers go through the data mover
 * (box commands reading or writing the FIFO window with the SDCC CRCI), so
 * the FIFO is "bottomless": every 32-bit read pulls the next bytes from
 * the card and every write pushes them, and the status flags describe a
 * FIFO that is always ready.  Both IRQ lines are levels:
 * STATUS & MASK0 and STATUS & MASK1.
 *
 * An SDIO card's interrupt (DAT1, in-band) shows in STATUS bit 22 (SDIOINTR)
 * and bit 25 (SDIOINTOPER, the one Linux's msm_sdcc unmasks) for as long as
 * the card holds it; Windows Mobile's SDIO stack takes the WLAN chip's
 * interrupts this way (Android's driver uses the chip's separate pin).
 *
 * accurate=on (with the hd2perf plugin's accurate mode) paces an SD card
 * like a real one: a read's data starts read-latency-us after the command
 * and flows at read-kbps, a write's at write-kbps, and the card is busy
 * programming for write-latency-us after the data before a busy-waiting
 * command (PROGENA) completes.  The data mover and the FIFO flags wait.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qemu/timer.h"
#include "hw/sd/sd.h"
#include "htcleo.h"
#include "msm_sdcc.h"
#include "../trace.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmSdccState, MSM_SDCC)

#define SDCC_POWER          0x00
#define SDCC_CLOCK          0x04
#define SDCC_ARGUMENT       0x08
#define SDCC_COMMAND        0x0C
#define SDCC_RESPCMD        0x10
#define SDCC_RESPONSE0      0x14
#define SDCC_RESPONSE3      0x20
#define SDCC_DATATIMER      0x24
#define SDCC_DATALENGTH     0x28
#define SDCC_DATACTRL       0x2C
#define SDCC_DATACNT        0x30
#define SDCC_STATUS         0x34
#define SDCC_CLEAR          0x38
#define SDCC_MASK0          0x3C
#define SDCC_MASK1          0x40
#define SDCC_FIFOCNT        0x44
#define SDCC_FIFO           0x80
#define SDCC_FIFO_END       0xC0

#define CMD_INDEX           0x3F
#define CMD_RESPONSE        (1u << 6)
#define CMD_LONGRSP         (1u << 7)
#define CMD_ENABLE          (1u << 10)
#define CMD_PROGENA         (1u << 11)

#define DC_ENABLE           (1u << 0)
#define DC_READ             (1u << 1)
#define DC_DMA_ENABLE       (1u << 3)

#define ST_CMDCRCFAIL       (1u << 0)
#define ST_CMDTIMEOUT       (1u << 2)
#define ST_CMDRESPEND       (1u << 6)
#define ST_CMDSENT          (1u << 7)
#define ST_DATAEND          (1u << 8)
#define ST_DATABLOCKEND     (1u << 10)
#define ST_TXACTIVE         (1u << 12)
#define ST_RXACTIVE         (1u << 13)
#define ST_TXFIFOHALFEMPTY  (1u << 14)
#define ST_RXFIFOHALFFULL   (1u << 15)
#define ST_RXFIFOFULL       (1u << 17)
#define ST_TXFIFOEMPTY      (1u << 18)
#define ST_RXFIFOEMPTY      (1u << 19)
#define ST_TXDATAAVLBL      (1u << 20)
#define ST_RXDATAAVLBL      (1u << 21)
#define ST_SDIOINTR         (1u << 22)
#define ST_PROGDONE         (1u << 23)
#define ST_SDIOINTOPER      (1u << 25)
/* bits the CLEAR register acknowledges; the rest describe live state */
#define ST_LATCHED          0x07C007FFu

#define FIFO_BYTES          64

#define TYPE_MSM_SDCC_BUS "msm-sdcc-bus"

struct MsmSdccState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    SDBus sdbus;
    qemu_irq irq[2];            /* cmd_irq, pio_irq */
    qemu_irq card_detect;       /* the slot's switch: low with a card in */

    uint32_t power, clock, arg, cmd, respcmd;
    uint32_t resp[4];
    uint32_t datatimer, datalength, datactrl;
    uint32_t mask[2];
    uint32_t latched;
    uint32_t remain;            /* bytes left in the data phase */
    DeviceState *adm;           /* data mover serving the FIFO */
    bool data_active;
    bool sdio_irq;              /* the SDIO card's interrupt */

    const MsmSdioCardOps *sdio;
    void *sdio_opaque;

    /* accurate mode: the card's speed */
    bool accurate;
    uint32_t read_kbps, write_kbps, read_latency_us, write_latency_us;
    int64_t xfer_t0;            /* when this data phase's bytes start */
    uint32_t xfer_bytes;        /* moved so far in it */
    int64_t prog_until;         /* the card programs the last write */
    bool prog_pending;          /* PROGDONE waits for it */
    QEMUTimer *timer;
};

#define PACE_CHUNK          512     /* the card delivers whole blocks */

static bool sdcc_paced(MsmSdccState *s)
{
    return s->accurate && !s->sdio;
}

/*
 * The card has not caught up with the bytes already moved: hold the data
 * mover and the FIFO, and come back when the next block is due.
 */
static bool sdcc_throttled(MsmSdccState *s)
{
    int64_t now, due;
    uint32_t kbps;

    if (!sdcc_paced(s) || !s->data_active) {
        return false;
    }
    kbps = MAX(s->datactrl & DC_READ ? s->read_kbps : s->write_kbps, 1);
    due = s->xfer_t0 + (int64_t)(s->xfer_bytes / PACE_CHUNK * PACE_CHUNK +
                                 PACE_CHUNK) * 1000000 / kbps;
    /* the first block arrives whole too */
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (now >= due || s->xfer_bytes % PACE_CHUNK) {
        return false;
    }
    timer_mod(s->timer, due);
    return true;
}

static bool sdcc_card_ready(MsmSdccState *s)
{
    if (sdcc_throttled(s)) {
        return false;
    }
    return s->sdio ? s->sdio->data_ready(s->sdio_opaque)
                   : sdbus_data_ready(&s->sdbus);
}

static uint32_t sdcc_status(MsmSdccState *s)
{
    uint32_t st = s->latched;

    if (s->sdio_irq) {
        st |= ST_SDIOINTR | ST_SDIOINTOPER;
    }
    if (s->data_active && s->remain) {
        if (s->datactrl & DC_READ) {
            st |= ST_RXACTIVE;
            if (sdcc_card_ready(s)) {
                st |= ST_RXDATAAVLBL;
                if (s->remain >= FIFO_BYTES / 2) {
                    st |= ST_RXFIFOHALFFULL;
                }
                if (s->remain >= FIFO_BYTES) {
                    st |= ST_RXFIFOFULL;
                }
            } else {
                st |= ST_RXFIFOEMPTY;
            }
        } else {
            st |= ST_TXACTIVE;
            if (!sdcc_throttled(s)) {
                st |= ST_TXFIFOHALFEMPTY | ST_TXFIFOEMPTY;
            }
        }
    } else {
        st |= ST_RXFIFOEMPTY | ST_TXFIFOEMPTY;
    }
    return st;
}

/* CRCI: the data mover may move the next bytes through the FIFO */
static bool sdcc_crci_ready(void *opaque, bool to_card)
{
    MsmSdccState *s = opaque;

    if (!s->data_active || !s->remain || !(s->datactrl & DC_DMA_ENABLE) ||
        !(s->datactrl & DC_READ) == !to_card) {
        return false;
    }
    if (!to_card) {
        return sdcc_card_ready(s);
    }
    return !sdcc_throttled(s) && (s->sdio || sdbus_receive_ready(&s->sdbus));
}

static void sdcc_crci_kick(MsmSdccState *s)
{
    if (s->adm) {
        msm_adm_crci_kick(s->adm);
    }
}

static void sdcc_update(MsmSdccState *s)
{
    uint32_t st = sdcc_status(s);

    qemu_set_irq(s->irq[0], !!(st & s->mask[0]));
    qemu_set_irq(s->irq[1], !!(st & s->mask[1]));
}

static void sdcc_data_done(MsmSdccState *s)
{
    if (sdcc_paced(s) && !(s->datactrl & DC_READ)) {
        s->prog_until = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                        (int64_t)s->write_latency_us * SCALE_US;
    }
    s->data_active = false;
    s->latched |= ST_DATAEND | ST_DATABLOCKEND;
    if (s->sdio) {
        s->sdio->data_end(s->sdio_opaque);
    }
}

static void sdcc_do_command(MsmSdccState *s)
{
    SDRequest req = { .cmd = s->cmd & CMD_INDEX, .arg = s->arg };
    uint8_t r[16];
    size_t rlen;

    if (s->sdio) {
        uint32_t resp[4] = { 0 };

        rlen = s->sdio->command(s->sdio_opaque, req.cmd, req.arg, resp);
        for (int i = 0; i < 4; i++) {
            stl_be_p(r + 4 * i, resp[i]);
        }
    } else {
        rlen = sdbus_do_command(&s->sdbus, &req, r, sizeof(r));
    }
    s->respcmd = req.cmd;
    trace_msm_sdcc_command(req.cmd, req.arg, rlen);
    if (!(s->cmd & CMD_RESPONSE)) {
        s->latched |= ST_CMDSENT;
        return;
    }
    if (rlen == 4 && !(s->cmd & CMD_LONGRSP)) {
        s->resp[0] = ldl_be_p(r);
        s->resp[1] = s->resp[2] = s->resp[3] = 0;
    } else if (rlen == 16 && (s->cmd & CMD_LONGRSP)) {
        s->resp[0] = ldl_be_p(r);
        s->resp[1] = ldl_be_p(r + 4);
        s->resp[2] = ldl_be_p(r + 8);
        s->resp[3] = ldl_be_p(r + 12) & ~1u;
    } else {
        s->latched |= ST_CMDTIMEOUT;
        return;
    }
    s->latched |= ST_CMDRESPEND;
    if (s->cmd & CMD_PROGENA) {
        /* the card is busy until it has programmed the last write */
        if (sdcc_paced(s) &&
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < s->prog_until) {
            s->prog_pending = true;
            timer_mod(s->timer, s->prog_until);
        } else {
            s->latched |= ST_PROGDONE;
        }
    }
}

static uint32_t sdcc_fifo_read(MsmSdccState *s)
{
    uint8_t b[4] = { 0 };
    uint32_t n;

    if (!s->data_active || !(s->datactrl & DC_READ) || !s->remain ||
        !sdcc_card_ready(s)) {
        return 0;
    }
    n = MIN(s->remain, 4);
    if (s->sdio) {
        s->sdio->read(s->sdio_opaque, b, n);
    } else {
        sdbus_read_data(&s->sdbus, b, n);
    }
    s->remain -= n;
    s->xfer_bytes += n;
    if (!s->remain) {
        sdcc_data_done(s);
    }
    return ldl_le_p(b);
}

static void sdcc_fifo_write(MsmSdccState *s, uint32_t val)
{
    uint8_t b[4];
    uint32_t n;

    if (!s->data_active || (s->datactrl & DC_READ) || !s->remain) {
        return;
    }
    stl_le_p(b, val);
    n = MIN(s->remain, 4);
    if (s->sdio) {
        s->sdio->write(s->sdio_opaque, b, n);
    } else {
        sdbus_write_data(&s->sdbus, b, n);
    }
    s->remain -= n;
    s->xfer_bytes += n;
    if (!s->remain) {
        sdcc_data_done(s);
    }
}

static uint64_t sdcc_read(void *opaque, hwaddr off, unsigned size)
{
    MsmSdccState *s = opaque;
    uint32_t v = 0;

    if (off >= SDCC_FIFO && off < SDCC_FIFO_END) {
        v = sdcc_fifo_read(s);
        sdcc_update(s);
        return v;
    }
    if (off == SDCC_STATUS) {
        trace_msm_sdcc_status(sdcc_status(s), s->remain);
    }
    switch (off) {
    case SDCC_POWER:
        return s->power;
    case SDCC_CLOCK:
        return s->clock;
    case SDCC_ARGUMENT:
        return s->arg;
    case SDCC_COMMAND:
        return s->cmd;
    case SDCC_RESPCMD:
        return s->respcmd;
    case SDCC_RESPONSE0 ... SDCC_RESPONSE3:
        return s->resp[(off - SDCC_RESPONSE0) >> 2];
    case SDCC_DATATIMER:
        return s->datatimer;
    case SDCC_DATALENGTH:
        return s->datalength;
    case SDCC_DATACTRL:
        return s->datactrl;
    case SDCC_DATACNT:
        return s->remain;
    case SDCC_STATUS:
        return sdcc_status(s);
    case SDCC_MASK0:
        return s->mask[0];
    case SDCC_MASK1:
        return s->mask[1];
    case SDCC_FIFOCNT:
        return (MIN(s->remain, FIFO_BYTES) + 3) >> 2;
    }
    qemu_log_mask(LOG_UNIMP, "msm-sdcc: read 0x%" HWADDR_PRIx "\n", off);
    return 0;
}

static void sdcc_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MsmSdccState *s = opaque;

    if (off >= SDCC_FIFO && off < SDCC_FIFO_END) {
        sdcc_fifo_write(s, val);
        sdcc_update(s);
        return;
    }
    trace_msm_sdcc_write(off, val);
    switch (off) {
    case SDCC_POWER:
        s->power = val;
        break;
    case SDCC_CLOCK:
        s->clock = val;
        break;
    case SDCC_ARGUMENT:
        s->arg = val;
        break;
    case SDCC_COMMAND:
        s->cmd = val;
        if (val & CMD_ENABLE) {
            sdcc_do_command(s);
            sdcc_update(s);
            sdcc_crci_kick(s);
        }
        break;
    case SDCC_DATATIMER:
        s->datatimer = val;
        break;
    case SDCC_DATALENGTH:
        s->datalength = val;
        break;
    case SDCC_DATACTRL:
        s->datactrl = val;
        if (val & DC_ENABLE) {
            s->remain = s->datalength;
            s->data_active = s->remain != 0;
            s->xfer_bytes = 0;
            s->xfer_t0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                         (val & DC_READ ? (int64_t)s->read_latency_us *
                                          SCALE_US : 0);
        } else {
            s->data_active = false;
            s->remain = 0;
        }
        sdcc_update(s);
        sdcc_crci_kick(s);
        break;
    case SDCC_CLEAR:
        s->latched &= ~(val & ST_LATCHED);
        break;
    case SDCC_MASK0:
        s->mask[0] = val;
        break;
    case SDCC_MASK1:
        s->mask[1] = val;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "msm-sdcc: write 0x%" HWADDR_PRIx
                      " = 0x%" PRIx64 "\n", off, val);
        break;
    }
    sdcc_update(s);
}

void msm_sdcc_sdio_irq(DeviceState *dev, bool level)
{
    MsmSdccState *s = MSM_SDCC(dev);

    if (s->sdio_irq != level) {
        s->sdio_irq = level;
        sdcc_update(s);
    }
}

void msm_sdcc_attach_sdio(DeviceState *dev, const MsmSdioCardOps *ops,
                          void *opaque)
{
    MsmSdccState *s = MSM_SDCC(dev);

    s->sdio = ops;
    s->sdio_opaque = opaque;
}

void msm_sdcc_connect_adm(DeviceState *dev, DeviceState *adm, hwaddr base)
{
    MsmSdccState *s = MSM_SDCC(dev);

    s->adm = adm;
    msm_adm_add_crci(adm, base + SDCC_FIFO, SDCC_FIFO_END - SDCC_FIFO,
                     sdcc_crci_ready, s);
}

/* the card caught up, or finished programming */
static void sdcc_timer(void *opaque)
{
    MsmSdccState *s = opaque;

    if (s->prog_pending &&
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >= s->prog_until) {
        s->prog_pending = false;
        s->latched |= ST_PROGDONE;
    }
    sdcc_update(s);
    sdcc_crci_kick(s);
}

static const MemoryRegionOps sdcc_ops = {
    .read = sdcc_read,
    .write = sdcc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void msm_sdcc_reset(DeviceState *dev)
{
    MsmSdccState *s = MSM_SDCC(dev);

    s->power = s->clock = s->arg = s->cmd = s->respcmd = 0;
    memset(s->resp, 0, sizeof(s->resp));
    s->datatimer = s->datalength = s->datactrl = 0;
    s->mask[0] = s->mask[1] = 0;
    s->latched = 0;
    s->remain = 0;
    s->data_active = false;
    s->prog_pending = false;
    s->prog_until = 0;
    timer_del(s->timer);
    sdcc_update(s);
    qemu_set_irq(s->card_detect, !sdbus_get_inserted(&s->sdbus));
}

/* a card inserted or removed (or a drive given media): the slot's switch */
static void msm_sdcc_set_inserted(DeviceState *dev, bool inserted)
{
    qemu_set_irq(MSM_SDCC(dev)->card_detect, !inserted);
}

static void msm_sdcc_init(Object *obj)
{
    MsmSdccState *s = MSM_SDCC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &sdcc_ops, s, "msm-sdcc", 0x1000);
    /*
     * A register write that lets a waiting data mover channel run reads
     * or writes the FIFO from inside that write (msm_adm_crci_kick)
     */
    s->iomem.disable_reentrancy_guard = true;
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq[0]);
    sysbus_init_irq(sbd, &s->irq[1]);
    qdev_init_gpio_out_named(DEVICE(obj), &s->card_detect, "card-detect", 1);
    qbus_init(&s->sdbus, sizeof(s->sdbus), TYPE_MSM_SDCC_BUS, DEVICE(obj),
              "sd-bus");
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sdcc_timer, s);
}

/*
 * The card's speed in accurate mode (KB/s, us); the defaults are a typical
 * class 4 microSD in the HD2's 4-bit slot.
 */
static const Property msm_sdcc_props[] = {
    DEFINE_PROP_BOOL("accurate", MsmSdccState, accurate, false),
    DEFINE_PROP_UINT32("read-kbps", MsmSdccState, read_kbps, 10000),
    DEFINE_PROP_UINT32("write-kbps", MsmSdccState, write_kbps, 5000),
    DEFINE_PROP_UINT32("read-latency-us", MsmSdccState, read_latency_us, 500),
    DEFINE_PROP_UINT32("write-latency-us", MsmSdccState, write_latency_us,
                       15000),
};

static void msm_sdcc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, msm_sdcc_reset);
    device_class_set_props(dc, msm_sdcc_props);
}

static void msm_sdcc_bus_class_init(ObjectClass *oc, const void *data)
{
    SD_BUS_CLASS(oc)->set_inserted = msm_sdcc_set_inserted;
}

static const TypeInfo msm_sdcc_bus_info = {
    .name = TYPE_MSM_SDCC_BUS,
    .parent = TYPE_SD_BUS,
    .instance_size = sizeof(SDBus),
    .class_init = msm_sdcc_bus_class_init,
};

static const TypeInfo msm_sdcc_info = {
    .name = TYPE_MSM_SDCC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmSdccState),
    .instance_init = msm_sdcc_init,
    .class_init = msm_sdcc_class_init,
};

static void msm_sdcc_register(void)
{
    type_register_static(&msm_sdcc_bus_info);
    type_register_static(&msm_sdcc_info);
}
type_init(msm_sdcc_register)
