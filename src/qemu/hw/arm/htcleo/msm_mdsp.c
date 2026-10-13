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
#define GSM_RX_ACQ      3               /* (the BCCH list's) */
#define GSM_RX_ACQ_CAMP 1               /* (cell selection's, to camp) */
#define GSM_CMD_STOP    8               /* (seen ending searches) */
/* the acquisition results: rings of 3, the DSP's write index, AMSS's read */
#define B_FCCH_RING     (MEM_B + 0x7B4)     /* 8 halfwords each */
#define B_FCCH_WR       (MEM_B + 0xADC)
#define B_SCH_RING      (MEM_B + 0xA7C)     /* 11 halfwords each */
#define B_SCH_WR        (MEM_B + 0xAE0)
#define GSM_BSIC        0x0A                /* NCC 1, BCC 2 */
#define GSM_CMD_ACCCH   29              /* receive a CCCH/BCCH block */
/*
 * transmit (command 30): the uplink entry's word address at byte 16, an
 * entry [length, type, data...]; a RACH's is [4, 6, RA]
 */
#define GSM_CMD_TX      30
#define GSM_ARFCN       133             /* the cell's, in its allocation */
#define GSM_TSC         2               /* = BCC */
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
    uint32_t l1_fn;             /* L1's frame number (its frame timer's) */
    int gsm_acq;                /* 0 idle, 1 searching, 2 tone posted */
    uint16_t gsm_acq_id;
    uint32_t gsm_acq_l1fn;      /* L1's frame number when it started */
    uint32_t rach_fn;           /* L1's frame number of the last RACH */
    uint8_t rach_ra;            /* and its content */
    uint32_t ia_bfn;            /* the CCCH block that answers it, or 0 */
    /* the network's side of the SDCCH it assigned (see gsm_net_ul) */
    bool ded;                   /* the MS is on it */
    bool l2_up;                 /* SAPI 0 is established */
    bool ua_pending, rr_pending, release;
    uint8_t ua[23];
    uint8_t vs, va, vr;         /* LAPDm V(S), V(A), V(R) */
    uint8_t last_i[23];         /* the unacknowledged I frame */
    unsigned t200;              /* SDCCH blocks it has been unacknowledged */
    unsigned idle;              /* SDCCH blocks since the MS sent anything */
    uint8_t l3q[4][20];         /* L3 messages to send */
    uint8_t l3q_len[4];
    unsigned l3q_n;
    unsigned sacch_n;
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

/*
 * The modelled cell's broadcast (3GPP 44.018): system information for MCC
 * 310, MNC 260 (the SIM's network), LAC 1, cell 1, in the BCCH block by
 * TC (SI1, SI2, SI3, SI4, then SI3/SI4 in the optional slots); the other
 * CCCH blocks page nobody, or assign a channel to a RACH.
 */
static const uint8_t gsm_si3[23] = {
    0x49, 0x06, 0x1b,                   /* L2 pseudo length 18, RR, SI3 */
    0x00, 0x01,                         /* cell identity */
    0x13, 0x00, 0x62, 0x00, 0x01,       /* LAI: 310 260, LAC 1 */
    0x48, 0x00, 0x0a,                   /* ATT, BS_AG_BLKS_RES 1, T3212 1 h */
    0x2f,                               /* cell options: RLT 64 */
    0x45, 0x40,                         /* reselect hysteresis, NECI */
    0x70, 0x00, 0x00,                   /* RACH: max retrans, tx-integer */
    0x2b, 0x2b, 0x2b, 0x2b,             /* rest octets */
};
/* SI1: cell allocation (variable bit map, ARFCN 133), RACH control */
static const uint8_t gsm_si1[23] = {
    0x55, 0x06, 0x19,
    0x8e, 0x42, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x70, 0x00, 0x00,
    0x2b,
};
/* SI2: BA list (variable bit map, ARFCN 133), NCC permitted, RACH */
static const uint8_t gsm_si2[23] = {
    0x59, 0x06, 0x1a,
    0x8e, 0x42, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0xff,
    0x70, 0x00, 0x00,
};
/* SI4: LAI, cell selection parameters, RACH control */
static const uint8_t gsm_si4[23] = {
    0x31, 0x06, 0x1c,
    0x13, 0x00, 0x62, 0x00, 0x01,
    0x45, 0x40,
    0x70, 0x00, 0x00,
    0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b,
};
/* idle CCCH blocks: PAGING REQUEST TYPE 1 paging nobody */
static const uint8_t gsm_fill[23] = {
    0x15, 0x06, 0x21, 0x00, 0x01, 0xf0, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b,
    0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b,
};

/*
 * The block a receive command for frame fn is for: the block whose four
 * frames include fn (idle reads come burst by burst), else the next one
 * (the BCCH list queues its reads a couple of frames ahead).  Blocks start
 * on frames 2 (BCCH), 6, 12, 16, 22, 26, 32, 36, 42, 46 of the multiframe.
 */
static uint32_t gsm_block_fn(uint32_t fn)
{
    static const uint8_t starts[] = { 2, 6, 12, 16, 22, 26, 32, 36, 42, 46 };
    unsigned t3 = fn % 51;

    for (unsigned i = 0; i < ARRAY_SIZE(starts); i++) {
        if (t3 < starts[i] + 4) {
            return fn - t3 + starts[i];
        }
    }
    return fn - t3 + 51 + 2;
}

/* the first CCCH (not BCCH) block that starts after frame fn */
static uint32_t gsm_ccch_after(uint32_t fn)
{
    uint32_t b = gsm_block_fn(fn);

    while (b <= fn || b % 51 == 2) {
        b = gsm_block_fn(b + 4);
    }
    return b;
}

/*
 * IMMEDIATE ASSIGNMENT (44.018 9.1.18) answering the last RACH: SDCCH/8
 * subchannel 0 on timeslot 1 of the cell's carrier, the request reference
 * (RA and the RACH's frame number as T1', T3, T2), timing advance 0
 */
static void gsm_imm_ass(MsmMdspState *s, uint8_t b[23])
{
    uint32_t fn = s->rach_fn;
    unsigned t1p = fn / 1326 % 32, t2 = fn % 26, t3 = fn % 51;
    const uint8_t m[] = {
        0x2d, 0x06, 0x3f,                   /* L2 pseudo length 11, RR, IA */
        0x00,                               /* dedicated, normal paging */
        0x08 << 3 | 1,                      /* SDCCH/8 subchannel 0, TN 1 */
        GSM_TSC << 5 | (GSM_ARFCN >> 8 & 3), GSM_ARFCN & 0xff,
        s->rach_ra, t1p << 3 | t3 >> 3, (t3 & 7) << 5 | t2,
        0x00,                               /* timing advance */
        0x00,                               /* mobile allocation: none */
    };

    memset(b, 0x2b, 23);
    memcpy(b, m, sizeof(m));
}

static const uint8_t *gsm_block(MsmMdspState *s, uint32_t bfn)
{
    unsigned t3 = bfn % 51, tc = (bfn / 51) % 8;
    static uint8_t ia[23];

    static const uint8_t *const bcch[8] = {
        gsm_si1, gsm_si2, gsm_si3, gsm_si4, gsm_si3, gsm_si4, gsm_si3, gsm_si4,
    };

    if (t3 == 2) {
        return bcch[tc];
    }
    if (s->ia_bfn && bfn == s->ia_bfn) {
        gsm_imm_ass(s, ia);
        return ia;
    }
    return gsm_fill;
}

/*
 * The network behind the cell, on the SDCCH an IMMEDIATE ASSIGNMENT gives
 * (SDCCH/8 subchannel 0 on timeslot 1): LAPDm (44.006) SAPI 0 with window
 * 1, and the network side of MM/RR (24.008, 44.018): a location update is
 * accepted without authentication and the channel released.  The SACCH
 * carries SI5 and SI6.
 */
static const uint8_t gsm_si5[23] = {
    0x00, 0x00, 0x03, 0x03,             /* L1 header: power, TA; UI */
    0x49, 0x06, 0x1d,                   /* L2 pseudo length 18, RR, SI5 */
    0x8e, 0x42, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
static const uint8_t gsm_si6[23] = {
    0x00, 0x00, 0x03, 0x03,
    0x2d, 0x06, 0x1e,                   /* L2 pseudo length 11, RR, SI6 */
    0x00, 0x01,                         /* cell identity */
    0x13, 0x00, 0x62, 0x00, 0x01,       /* LAI */
    0x2f, 0xff,                         /* cell options, NCC permitted */
    0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b,
};
static const uint8_t gsm_l2_fill[23] = {
    0x01, 0x03, 0x01, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b,
    0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b,
};

static void gsm_net_send(MsmMdspState *s, const uint8_t *m, unsigned n)
{
    if (s->l3q_n < ARRAY_SIZE(s->l3q) && n <= sizeof(s->l3q[0])) {
        memcpy(s->l3q[s->l3q_n], m, n);
        s->l3q_len[s->l3q_n++] = n;
    }
}

/* an L3 message from the MS */
static void gsm_net_l3(MsmMdspState *s, const uint8_t *m, unsigned n)
{
    unsigned pd = m[0] & 0xf, mt = n > 1 ? m[1] & 0x3f : 0;

    if (s->trace) {
        qemu_log("msm-mdsp: gsm net: L3 pd %u type %02x (%u bytes)\n", pd, mt,
                 n);
    }
    if (pd == 5 && mt == 0x08) {
        /* LOCATION UPDATING REQUEST: ACCEPT (the LAI), then release */
        static const uint8_t acc[] = { 0x05, 0x02, 0x13, 0x00, 0x62, 0x00, 0x01 };
        static const uint8_t rel[] = { 0x06, 0x0d, 0x00 };

        gsm_net_send(s, acc, sizeof(acc));
        gsm_net_send(s, rel, sizeof(rel));
        s->release = true;
    }
}

static void gsm_net_ack(MsmMdspState *s, unsigned nr)
{
    if (((nr - s->va) & 7) <= ((s->vs - s->va) & 7)) {
        s->va = nr;
    }
}

/* an SDCCH frame from the MS (format B) */
static void gsm_net_ul(MsmMdspState *s, const uint8_t *f)
{
    uint8_t c = f[1];
    unsigned len = MIN(f[2] >> 2, 20);

    if ((f[0] >> 2 & 7) != 0) {         /* SAPI 3 (SMS) not yet */
        return;
    }
    s->idle = 0;
    if ((c & 3) == 3) {                 /* U frames */
        switch (c & ~0x10) {
        case 0x2f:                      /* SABM: UA with its information */
            s->ded = s->l2_up = true;
            s->vs = s->va = s->vr = 0;
            s->l3q_n = 0;
            s->release = false;
            memset(s->ua, 0x2b, 23);
            s->ua[0] = 0x01;
            s->ua[1] = 0x63 | (c & 0x10);
            s->ua[2] = len << 2 | 1;
            memcpy(s->ua + 3, f + 3, len);
            s->ua_pending = true;
            if (len) {
                gsm_net_l3(s, f + 3, len);
            }
            break;
        case 0x43:                      /* DISC: UA, the channel is free */
            memcpy(s->ua, gsm_l2_fill, 23);
            s->ua[1] = 0x63 | (c & 0x10);
            s->ua_pending = true;
            s->l2_up = false;
            break;
        }
    } else if (!(c & 1)) {              /* I frames */
        gsm_net_ack(s, c >> 5);
        if ((c >> 1 & 7) == s->vr) {
            s->vr = (s->vr + 1) & 7;
            gsm_net_l3(s, f + 3, len);
        }
        s->rr_pending = true;
    } else {                            /* S frames */
        gsm_net_ack(s, c >> 5);
    }
    if (s->trace) {
        qemu_log("msm-mdsp: gsm net: UL %02x %02x %02x %02x %02x %02x"
                 " V(S) %u V(A) %u V(R) %u\n", f[0], f[1], f[2], f[3], f[4],
                 f[5], s->vs, s->va, s->vr);
    }
}

/* the next SDCCH block to the MS */
static void gsm_net_dl(MsmMdspState *s, uint8_t b[23])
{
    memcpy(b, gsm_l2_fill, 23);
    if (s->ua_pending) {
        memcpy(b, s->ua, 23);
        s->ua_pending = false;
        if (!s->l2_up) {
            s->ded = false;             /* released */
        }
        return;
    }
    if (!s->l2_up) {
        if (s->ded && ++s->idle > 8) {
            s->ded = false;             /* the MS has left */
        }
        return;
    }
    if (s->vs != s->va) {
        /* T200: the I frame is resent after three blocks unacknowledged */
        if (++s->t200 < 3) {
            goto rr;
        }
        memcpy(b, s->last_i, 23);
        b[1] = s->vr << 5 | (b[1] & 0x1f);
        s->t200 = 0;
        s->rr_pending = false;
        return;
    }
    if (s->l3q_n) {
        unsigned n = s->l3q_len[0];

        memset(b, 0x2b, 23);
        b[0] = 0x03;
        b[1] = s->vr << 5 | s->vs << 1;
        b[2] = n << 2 | 1;
        memcpy(b + 3, s->l3q[0], n);
        memcpy(s->last_i, b, 23);
        memmove(s->l3q[0], s->l3q[1], sizeof(s->l3q[0]) * --s->l3q_n);
        memmove(s->l3q_len, s->l3q_len + 1, s->l3q_n);
        s->vs = (s->vs + 1) & 7;
        s->t200 = 0;
        s->rr_pending = false;
        return;
    }
    if (s->release && ++s->idle > 8) {
        s->ded = s->l2_up = false;      /* released, the MS did not say so */
        return;
    }
rr:
    if (s->rr_pending) {
        b[0] = 0x01;
        b[1] = s->vr << 5 | 0x01;
        s->rr_pending = false;
    }
}

/* a downlink block on the dedicated channel, by its first frame */
static const uint8_t *gsm_ded_block(MsmMdspState *s, unsigned t3,
                                    bool consume)
{
    static uint8_t b[23];

    if (t3 < 4) {
        if (consume) {
            gsm_net_dl(s, b);
        }
        return b;
    }
    if (t3 >= 32 && t3 < 48) {
        return s->sacch_n & 1 ? gsm_si6 : gsm_si5;
    }
    return gsm_l2_fill;
}

/* receive a CCCH/BCCH block: its metrics and its decoded 23 octets */
static void gsm_accch(MsmMdspState *s, uint16_t seq, const uint8_t *cmd)
{
    uint32_t met = MEM_B + 2 * lduw_le_p(cmd + 4);
    bool has_dat = lduw_le_p(cmd + 24) != 0xffff;
    uint32_t dat = MEM_B + 2 * lduw_le_p(cmd + 24);
    uint32_t bfn;
    const uint8_t *blk;

    if (s->ded) {
        /* the four reads of a block: the last one takes the data */
        bfn = s->gsm_fn - (has_dat ? 3 : 0);
        blk = gsm_ded_block(s, bfn % 51, has_dat);
        if (has_dat && bfn % 51 >= 32 && bfn % 51 < 48) {
            s->sacch_n++;
        }
    } else {
        bfn = gsm_block_fn(s->gsm_fn);
        blk = gsm_block(s, bfn);
    }
    if (met + 28 > MEM_C || dat + 12 + 24 > MEM_C) {
        return;
    }
    memset(s->mem + met, 0, 28);
    stw_le_p(s->mem + met + 2, seq);
    stw_le_p(s->mem + met + 6, 0x100);
    stw_le_p(s->mem + met + 8, 0x100);
    stw_le_p(s->mem + met, 1);
    if (has_dat) {
        memset(s->mem + dat, 0, 12 + 24);
        memcpy(s->mem + dat + 12, blk, 23);
        stw_le_p(s->mem + dat + 2, seq);
        stw_le_p(s->mem + dat + 6, 2);          /* bit 1: CRC OK */
        stw_le_p(s->mem + dat, 23);
    }
    if (s->trace) {
        qemu_log("msm-mdsp: gsm accch seq %04x fn %u block %u (t3 %u tc %u)"
                 " met %04x dat %04x %s\n", seq, s->gsm_fn, bfn, bfn % 51,
                 (bfn / 51) % 8, lduw_le_p(cmd + 4), lduw_le_p(cmd + 24),
                 blk == gsm_si1 ? "SI1" : blk == gsm_si2 ? "SI2" :
                 blk == gsm_si3 ? "SI3" : blk == gsm_si4 ? "SI4" :
                 blk == gsm_fill ? "fill" : s->ded ? "dedicated" :
                 "IMMEDIATE ASSIGNMENT");
        if (s->ded && has_dat) {
            qemu_log("msm-mdsp: gsm net: DL %02x %02x %02x %02x %02x %02x\n",
                     blk[0], blk[1], blk[2], blk[3], blk[4], blk[5]);
        }
    }
}

/* a burst goes out: a RACH is answered in the next CCCH block or so */
static void gsm_tx(MsmMdspState *s, const uint8_t *cmd)
{
    uint32_t e = MEM_B + 2 * lduw_le_p(cmd + 16);

    if (s->trace && e + 64 <= MEM_C && lduw_le_p(s->mem + e)) {
        GString *g = g_string_new(NULL);

        for (unsigned k = 0; k < 32; k += 2) {
            g_string_append_printf(g, " %04x", lduw_le_p(s->mem + e + k));
        }
        qemu_log("msm-mdsp: gsm tx l1 fn %u entry %04x:%s\n", s->l1_fn,
                 lduw_le_p(cmd + 16), g->str);
        g_string_free(g, true);
    }
    if (e + 6 > MEM_C || !lduw_le_p(s->mem + e)) {
        return;
    }
    if (lduw_le_p(s->mem + e) != 4) {
        /* an SDCCH frame: [15, 1, 23 octets]; not the SACCH's (t3 47-50) */
        unsigned t3 = s->gsm_fn % 51;

        if (e + 4 + 23 <= MEM_C && (t3 < 47 || !s->ded)) {
            gsm_net_ul(s, s->mem + e + 4);
        }
        return;
    }
    s->rach_fn = s->l1_fn;
    s->rach_ra = s->mem[e + 4];
    s->ia_bfn = gsm_ccch_after(s->gsm_fn + 2);
    if (s->trace) {
        qemu_log("msm-mdsp: gsm RACH ra %02x fn %u (cell %u), answer in"
                 " block %u\n", s->rach_ra, s->rach_fn, s->gsm_fn, s->ia_bfn);
    }
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
        /*
         * The burst's time from the acquisition start, in quarter symbols
         * (5000 a frame): L1 takes the cell's frame offset from it
         * (0xF9F91A: FN_diff = start + 1 + timing / 5000 - SCH FN)
         */
        {
            uint32_t qs = 5000 * ((s->l1_fn - s->gsm_acq_l1fn) % 2715648);

            stw_le_p(e + 14, qs >> 16);
            stw_le_p(e + 16, qs & 0xffff);
        }
        stw_le_p(s->mem + B_SCH_WR, wr + 1);
        s->gsm_acq = 0;
        if (s->trace) {
            qemu_log("msm-mdsp: gsm sch id %u fn %u %02x%02x%02x%02x l1 %u"
                     " start %u true diff %d\n", s->gsm_acq_id, s->gsm_fn,
                     b[0], b[1], b[2], b[3], s->l1_fn, s->gsm_acq_l1fn,
                     (int)(s->l1_fn - s->gsm_fn));
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
    s->l1_fn = level - 1;
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
            } else if (id == GSM_CMD_ACCCH) {
                gsm_accch(s, seq, buf + off);
            } else if (id == GSM_CMD_TX) {
                gsm_tx(s, buf + off);
            } else if (id == GSM_CMD_RX &&
                       (lduw_le_p(buf + off + 16) == GSM_RX_ACQ ||
                        lduw_le_p(buf + off + 16) == GSM_RX_ACQ_CAMP)) {
                s->gsm_acq = 1;
                s->gsm_acq_l1fn = s->l1_fn;
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
