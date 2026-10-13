/*
 * QSD8250 modem DSP (mDSP, a QDSP4) as seen by the modem core, high level
 *
 * The mDSP does the baseband signal processing for GSM/WCDMA.  AMSS loads
 * its firmware into the DSP's memories and talks to it through words in
 * those memories; the DSP's own code is not run here.  What AMSS
 * 2.15.50.14 does (mdsptask.c, mdspfunc.c), reverse-engineered:
 *
 *   0xB1000000  memory A, 16-bit words (DSP word n at +2n)
 *   0xB1200000  memory B, 16-bit words
 *   0xB1400000  memory C, 32-bit words
 *   0xB1800000  program memory, 32-bit words
 *   0xB1C00000  control: 3 holds the DSP in reset, 2 lets it run
 *   0xB1C00008  doorbell: the modem core interrupts the DSP; bit 31 with
 *               the bit of the DSP interrupt to raise
 *
 * Bringing the DSP up, AMSS holds it in reset, downloads the image's
 * sections, writes 0x10000 to C+0x24 and 0xFFFF to A+0xF0, releases it and
 * 500 us later expects the "Master Controller" to have posted a nonzero
 * word at B+0x24.  The DSP interrupts the modem core on VIC line 27
 * (tramp 41).  The image (0x1406 for radio 2.15.50.14) is one kernel with
 * an overlay per application (GSM, WCDMA, W2G ...) that AMSS downloads
 * while the DSP runs; each overlay has its own variables, which AMSS finds
 * through a symbol table it switches with the overlay ([0x1350B0C]).  AMSS
 * sees memory A at 0xC4F00000, B at 0xC5100000 and C at 0xC5300000.
 *
 * Doorbell interrupts and what the DSP does with them here:
 *   bit 19  master controller command: AMSS writes a command code to
 *           B+0xB4; the MC clears it when done and interrupts back.  An
 *           asynchronous command also carries a value in C+0x470 that the
 *           MC acknowledges by copying it to C+0x46C.  Every command
 *           completes at once, and the applications report ready:
 *             B+0x33A4  GSM ready (GSM overlay), polled by GSM L1
 *             A+0x2148  WCDMA ready for commands (low byte), polled by WL1
 *                       for 100 ms after it enables WCDMA and W2G
 *   bit 15  WCDMA synchronous command (mdspsync.c, WCDMA overlay): the
 *           command code and its arguments are at B+0x4958; the DSP
 *           overwrites the code with a status, 0 = done, and sets B+0x4E5E
 *           (event 0, "synchronous command done") which WL1's ISR clears
 *           before it runs the command's callback
 *           Command 0x13 starts the receive AGC: with no RF signal the AGC
 *           sits at its floor, -512 (dBm = (AGC + 512) / 12 - 106, so the
 *           -106 dBm noise floor), in A+0x1F3C (chain 0) and B+0x4E96/98
 *           (the filtered values), and WL1's frequency scan finds no carrier
 *           The searcher (srchcmd.c) issues its searches the same way;
 *           after a search the DSP sets "HPQ done" (event 2, B+0x4E56) and
 *           "LPQ done" (event 1, B+0x4E54) and interrupts, and the results
 *           it leaves (no peaks: there is no signal) send acquisition on to
 *           the next frequency.  Handlers ignore a done event for a queue
 *           with no search pending.
 *   bit 26  GSM sleep/wake handshake (not modelled)
 *
 * GSM L1 (mdsp_intf.c, mdsp_gprs.c) talks to the DSP through the GSM
 * overlay's variables, found through AMSS's overlay symbol table (slot k
 * at [[0x1350B0C] + 4k]): slots 44 and 45 are two per-frame command
 * buffers at B+0x5F2C and B+0x6024 ([count, buffer sequence number, then
 * the commands, each its id and fixed-length fields]), slot 47 (B+0xD18)
 * the base of the interface's other buffers.  The DSP runs a buffer's
 * commands at the frame tick and zeroes its count.  Commands handled:
 *   20 (0x14)  power measurements: [id, flags, result address (B word),
 *              n <= 7, then per measurement a gain byte, the ARFCN and two
 *              RF words]; the results at that address: [0x8000 (ready)
 *              | 3 + 2n, the buffer's sequence number, 0, then each
 *              measurement's energy, high half first]
 *   4          receive; with type 3 (word 8) it starts an acquisition: the
 *              DSP looks for the cell's tone (FCCH, frames 0/10/20/30/40 of
 *              the 51-multiframe) and posts it to a ring of three 8-halfword
 *              entries at B+0x7B4 (write index B+0xADC, AMSS's read index
 *              B+0xADA): [id, timing (32 bits), ?, frequency error, ?, ?,
 *              SNR >= 8]; the SCH of the next frame goes to a ring of three
 *              11-halfword entries at B+0xA7C (write B+0xAE0, read B+0xADE):
 *              [id (the tone's), status (bit 1 CRC OK), ?, ?, payload
 *              length 2, the 25 SCH bits as 4 bytes (BSIC, T1, T2, T3'),
 *              timing (32 bits), ?, ?]
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"
#include "htcleo.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmMdspState, MSM_MDSP)

#define MDSP_SIZE       0x00C01000
#define MEM_A           0x000000
#define MEM_B           0x200000
#define MEM_C           0x400000
#define MEM_P           0x800000
#define MDSP_CTL        0xC00000

#define CTL_RESET       3
#define CTL_RUN         2

#define B_MC_STATUS     (MEM_B + 0x24)
#define B_MC_CMD        (MEM_B + 0xB4)
#define C_MC_ACK        (MEM_C + 0x46C)
#define C_MC_ARG        (MEM_C + 0x470)
#define IMG_1406        0x1406
#define B_GSM_READY     (MEM_B + 0x33A4)
#define A_WCDMA_READY   (MEM_A + 0x2148)
#define B_SYNC_CMD      (MEM_B + 0x4958)
#define B_SYNC_DONE     (MEM_B + 0x4E5E)
#define A_RXAGC0        (MEM_A + 0x1F3C)
#define B_RXAGC_FILT    (MEM_B + 0x4E96)
#define SYNC_START_AGC  0x13
#define RXAGC_FLOOR     (-512)
#define B_EVT_LPQ_DONE  (MEM_B + 0x4E54)
#define B_EVT_HPQ_DONE  (MEM_B + 0x4E56)
#define SEARCH_NS       2000000         /* a search takes a couple of ms */
#define B_GSM_CMDBUF0   (MEM_B + 0x5F2C)
#define B_GSM_CMDBUF1   (MEM_B + 0x6024)
#define GSM_CMDBUF_SIZE 0xF8
#define GSM_CMD_PWR     20
#define GSM_CMD_RX      4               /* receive; type 3: acquisition */
#define GSM_RX_ACQ      3
#define GSM_CMD_STOP    8               /* (seen ending searches) */
/* the acquisition results: rings of 3, the DSP's write index, AMSS's read */
#define B_FCCH_RING     (MEM_B + 0x7B4)     /* 8 halfwords each */
#define B_FCCH_WR       (MEM_B + 0xADC)
#define B_SCH_RING      (MEM_B + 0xA7C)     /* 11 halfwords each */
#define B_SCH_WR        (MEM_B + 0xAE0)
#define GSM_BSIC        0x0A                /* NCC 1, BCC 2 */
#define GSM_FCCH_SNR    0x400
#define GSM_NOISE       0x40            /* energy of an empty channel */
#define GSM_CELL        0x186a0         /* energy of the cell */
#define DB_MC           (1u << 19)
#define DB_SYNC         (1u << 15)
#define CTL_DOORBELL    0x08

struct MsmMdspState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer *search;
    uint8_t *mem;
    uint32_t ctl;
    uint32_t ctl_regs[0x400];
    bool trace;                 /* log doorbells and first writes (+PC) */
    uint32_t gsm_fn;            /* the cell's frame number */
    int gsm_acq;                /* 0 idle, 1 searching, 2 tone posted */
    uint16_t gsm_acq_id;
    GHashTable *seen;
};

/* the commands' lengths in bytes, by id (AMSS's table at 0x1E1EAC4) */
static const uint8_t gsm_cmd_len[31] = {
    [2] = 22, [4] = 22, [5] = 8, [6] = 24, [7] = 2, [8] = 4, [11] = 8,
    [12] = 14, [13] = 8, [14] = 8, [16] = 4, [17] = 10, [20] = 64,
    [24] = 8, [25] = 14, [26] = 12, [28] = 8, [29] = 78, [30] = 48,
};

static void gsm_power(MsmMdspState *s, uint16_t seq, const uint8_t *cmd)
{
    uint32_t res = MEM_B + 2 * lduw_le_p(cmd + 4);
    unsigned n = MIN(lduw_le_p(cmd + 6), 7);

    if (res + 2 * (3 + 2 * n) > MEM_C) {
        return;
    }
    stw_le_p(s->mem + res + 2, seq);
    stw_le_p(s->mem + res + 4, 0);
    for (unsigned i = 0; i < n; i++) {
        uint16_t arfcn = lduw_le_p(cmd + 10 + 8 * i);
        uint32_t e = GSM_CELL;   /* XXX every ARFCN, until the RF tuning is decoded */

        if (s->trace) {
            qemu_log("msm-mdsp: gsm power seq %04x res %04x n %u #%u pos %04x"
                     " gain %02x rf %04x %04x\n", seq, lduw_le_p(cmd + 4), n,
                     i, arfcn, cmd[8 + 8 * i], lduw_le_p(cmd + 12 + 8 * i),
                     lduw_le_p(cmd + 14 + 8 * i));
        }
        stw_le_p(s->mem + res + 6 + 4 * i, e >> 16);
        stw_le_p(s->mem + res + 8 + 4 * i, e & 0xffff);
    }
    /* the length last, bit 15 "ready": AMSS polls it */
    stw_le_p(s->mem + res, 0x8000 | (3 + 2 * n));
}

/* the SCH's 25 bits for frame fn: BSIC, T1, T2, T3' (AMSS's 0xF9F41A) */
static void gsm_sch_bits(uint32_t fn, uint8_t b[4])
{
    unsigned t1 = fn / 1326, t2 = fn % 26, t3p = (fn % 51 - 1) / 10;

    b[0] = GSM_BSIC << 2 | (t1 >> 9 & 3);
    b[1] = t1 >> 1;
    b[2] = (t1 & 1) << 7 | t2 << 2 | (t3p >> 1 & 3);
    b[3] = t3p & 1;
}

/* acquisition: the tone (FCCH) of the cell, then its SCH a frame later */
static void gsm_acq_frame(MsmMdspState *s)
{
    unsigned t3 = s->gsm_fn % 51;

    if (s->gsm_acq == 1 && t3 % 10 == 0 && t3 != 50) {
        uint16_t wr = lduw_le_p(s->mem + B_FCCH_WR);
        uint8_t *e = s->mem + B_FCCH_RING + 16 * (wr % 3);

        memset(e, 0, 16);
        stw_le_p(e + 0, ++s->gsm_acq_id);
        stw_le_p(e + 14, GSM_FCCH_SNR);     /* >= 8 or it is rejected */
        stw_le_p(s->mem + B_FCCH_WR, wr + 1);
        s->gsm_acq = 2;
        if (s->trace) {
            qemu_log("msm-mdsp: gsm tone id %u fn %u\n", s->gsm_acq_id,
                     s->gsm_fn);
        }
    } else if (s->gsm_acq == 2 && t3 % 10 == 1) {
        uint16_t wr = lduw_le_p(s->mem + B_SCH_WR);
        uint8_t *e = s->mem + B_SCH_RING + 22 * (wr % 3);
        uint8_t b[4];

        gsm_sch_bits(s->gsm_fn, b);
        memset(e, 0, 22);
        stw_le_p(e + 0, s->gsm_acq_id);
        stw_le_p(e + 2, 2);             /* bit 1: CRC OK */
        stw_le_p(e + 8, 2);             /* payload halfwords */
        memcpy(e + 10, b, 4);
        stw_le_p(s->mem + B_SCH_WR, wr + 1);
        s->gsm_acq = 0;
        if (s->trace) {
            qemu_log("msm-mdsp: gsm sch id %u fn %u %02x%02x%02x%02x\n",
                     s->gsm_acq_id, s->gsm_fn, b[0], b[1], b[2], b[3]);
        }
    }
}

/* a GSM frame starts: run the commands queued for it */
static void mdsp_gsm_frame(void *opaque, int n, int level)
{
    MsmMdspState *s = opaque;
    static const uint32_t bufs[2] = { B_GSM_CMDBUF0, B_GSM_CMDBUF1 };

    if (!level) {
        return;
    }
    s->gsm_fn = (s->gsm_fn + 1) % 2715648;
    if (s->ctl != CTL_RUN || lduw_le_p(s->mem + MEM_A) != IMG_1406 ||
        !lduw_le_p(s->mem + B_GSM_READY)) {
        return;
    }
    if (s->trace) {
        qemu_log("msm-mdsp: gsm frame parity %d: buf0 %u/%04x buf1 %u/%04x\n",
                 n, lduw_le_p(s->mem + bufs[0]), lduw_le_p(s->mem + bufs[0] + 2),
                 lduw_le_p(s->mem + bufs[1]), lduw_le_p(s->mem + bufs[1] + 2));
    }
    for (int b = 0; b < 2; b++) {
        uint8_t *buf = s->mem + bufs[b];
        unsigned count = lduw_le_p(buf), off = 4;
        uint16_t seq = lduw_le_p(buf + 2);

        for (unsigned c = 0; c < count && off + 2 <= GSM_CMDBUF_SIZE; c++) {
            uint16_t id = lduw_le_p(buf + off);
            unsigned len = id < ARRAY_SIZE(gsm_cmd_len) ? gsm_cmd_len[id] : 0;

            if (!len || off + len > GSM_CMDBUF_SIZE) {
                qemu_log_mask(LOG_UNIMP, "msm-mdsp: gsm command %u?\n", id);
                break;
            }
            if (id == GSM_CMD_PWR) {
                gsm_power(s, seq, buf + off);
            } else if (id == GSM_CMD_RX &&
                       lduw_le_p(buf + off + 16) == GSM_RX_ACQ) {
                s->gsm_acq = 1;
            }
            if (id != GSM_CMD_PWR && s->trace) {
                GString *g = g_string_new(NULL);

                for (unsigned k = 2; k < len; k += 2) {
                    g_string_append_printf(g, " %04x",
                                           lduw_le_p(buf + off + k));
                }
                qemu_log("msm-mdsp: gsm cmd %u seq %04x:%s\n", id, seq, g->str);
                g_string_free(g, true);
            }
            off += len;
        }
        stw_le_p(buf, 0);
    }
    gsm_acq_frame(s);
}

static void mdsp_boot(MsmMdspState *s)
{
    /* the master controller is up */
    stw_le_p(s->mem + B_MC_STATUS, 1);
    qemu_log_mask(LOG_UNIMP, "msm-mdsp: running image %04x\n",
                  lduw_le_p(s->mem + MEM_A));
}

/* a search finished, with nothing found */
static void mdsp_search_done(void *opaque)
{
    MsmMdspState *s = opaque;

    if (s->ctl != CTL_RUN) {
        return;
    }
    stw_le_p(s->mem + B_EVT_HPQ_DONE, 1);
    stw_le_p(s->mem + B_EVT_LPQ_DONE, 1);
    qemu_irq_pulse(s->irq);
}

static void mdsp_trace(MsmMdspState *s, const char *what, hwaddr off,
                       uint64_t val, unsigned size)
{
    uint32_t pc = current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0;
    gpointer key = (gpointer)(uintptr_t)(off | (uint64_t)(*what == 'R') << 40);
    uintptr_t n = GPOINTER_TO_UINT(g_hash_table_lookup(s->seen, key));

    if (n >= 2) {
        return;
    }
    g_hash_table_insert(s->seen, key, GUINT_TO_POINTER(n + 1));
    qemu_log("msm-mdsp: %s %c+%06x = %0*x pc %08x\n", what,
             off >= MEM_P ? 'P' : off >= MEM_C ? 'C' : off >= MEM_B ? 'B' : 'A',
             (unsigned)(off >= MEM_P ? off - MEM_P : off >= MEM_C ? off - MEM_C :
                        off >= MEM_B ? off - MEM_B : off),
             size * 2, (unsigned)val, pc);
}

static uint64_t mdsp_read(void *opaque, hwaddr off, unsigned size)
{
    MsmMdspState *s = opaque;

    if (off == MDSP_CTL) {
        return s->ctl;
    }
    if (off > MDSP_CTL) {
        return s->ctl_regs[((off - MDSP_CTL) >> 2) & 0x3FF];
    }
    if (s->trace) {
        mdsp_trace(s, "R", off, ldn_le_p(s->mem + off, size), size);
    }
    return ldn_le_p(s->mem + off, size);
}

static void mdsp_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MsmMdspState *s = opaque;

    if (off == MDSP_CTL + CTL_DOORBELL) {
        s->ctl_regs[CTL_DOORBELL >> 2] = val;
        if (s->trace) {
            qemu_log("msm-mdsp: doorbell %08x pc %08x\n", (unsigned)val,
                     current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0);
        }
        if (s->ctl != CTL_RUN || lduw_le_p(s->mem + MEM_A) != IMG_1406) {
            qemu_irq_pulse(s->irq);
            return;
        }
        if ((val & DB_MC) && lduw_le_p(s->mem + B_MC_CMD)) {
            qemu_log_mask(LOG_UNIMP, "msm-mdsp: MC command %04x\n",
                          lduw_le_p(s->mem + B_MC_CMD));
            stl_le_p(s->mem + C_MC_ACK, ldl_le_p(s->mem + C_MC_ARG));
            stw_le_p(s->mem + B_MC_CMD, 0);
            stw_le_p(s->mem + B_GSM_READY, 1);
            stw_le_p(s->mem + A_WCDMA_READY, 1);
        }
        if (val & DB_SYNC) {
            uint16_t cmd = lduw_le_p(s->mem + B_SYNC_CMD);

            qemu_log_mask(LOG_UNIMP, "msm-mdsp: WCDMA sync command %04x\n",
                          cmd);
            if (cmd == SYNC_START_AGC) {
                stw_le_p(s->mem + A_RXAGC0, RXAGC_FLOOR);
                stw_le_p(s->mem + B_RXAGC_FILT, RXAGC_FLOOR);
                stw_le_p(s->mem + B_RXAGC_FILT + 2, RXAGC_FLOOR);
            }
            timer_mod(s->search,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SEARCH_NS);
            stw_le_p(s->mem + B_SYNC_CMD, 0);
            stw_le_p(s->mem + B_SYNC_DONE, 1);
        }
        qemu_irq_pulse(s->irq);
        return;
    }
    if (off > MDSP_CTL) {
        s->ctl_regs[((off - MDSP_CTL) >> 2) & 0x3FF] = val;
        return;
    }
    if (off == MDSP_CTL) {
        uint32_t old = s->ctl;

        s->ctl = val;
        if (old == CTL_RESET && val == CTL_RUN) {
            mdsp_boot(s);
        }
        return;
    }
    if (s->trace) {
        mdsp_trace(s, "W", off, val, size);
    }
    stn_le_p(s->mem + off, size, val);
}

static const MemoryRegionOps mdsp_ops = {
    .read = mdsp_read,
    .write = mdsp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void mdsp_reset(DeviceState *dev)
{
    MsmMdspState *s = MSM_MDSP(dev);

    memset(s->mem, 0, MDSP_CTL);
    memset(s->ctl_regs, 0, sizeof(s->ctl_regs));
    s->ctl = 0;
    timer_del(s->search);
}

static bool mdsp_get_trace(Object *obj, Error **errp)
{
    return MSM_MDSP(obj)->trace;
}

static void mdsp_set_trace(Object *obj, bool value, Error **errp)
{
    MSM_MDSP(obj)->trace = value;
}

static void mdsp_init(Object *obj)
{
    MsmMdspState *s = MSM_MDSP(obj);

    s->mem = g_malloc0(MDSP_CTL);
    memory_region_init_io(&s->iomem, obj, &mdsp_ops, s, "msm-mdsp",
                          MDSP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_in_named(DEVICE(obj), mdsp_gsm_frame, "frame", 2);
    s->search = timer_new_ns(QEMU_CLOCK_VIRTUAL, mdsp_search_done, s);
    s->seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    object_property_add_bool(obj, "trace", mdsp_get_trace, mdsp_set_trace);
}

static void mdsp_class_init(ObjectClass *oc, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(oc), mdsp_reset);
}

static const TypeInfo msm_mdsp_info = {
    .name = TYPE_MSM_MDSP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmMdspState),
    .instance_init = mdsp_init,
    .class_init = mdsp_class_init,
};

static void msm_mdsp_register(void)
{
    type_register_static(&msm_mdsp_info);
}
type_init(msm_mdsp_register)
