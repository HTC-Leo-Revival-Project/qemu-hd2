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
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"
#include "htcleo.h"
#include "gsm_net.h"
#include "net/net.h"
#include "net/eth.h"
#include "hw/core/qdev-properties.h"

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
#define GSM_CMD_ACCCH   29              /* receive a CCCH/BCCH block */
/*
 * transmit (command 30): the uplink entry's word address at byte 16, an
 * entry [length, type, data...]; a RACH's is [4, 6, RA]
 */
#define GSM_CMD_TX      30
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
    bool gprs_trace;
    uint32_t gsm_fn;            /* the cell's frame number (= L1's) */
    uint32_t l1_fn;             /* L1's frame number (its frame timer's) */
    int gsm_acq;                /* 0 idle, 1 searching, 2 tone posted */
    uint16_t gsm_acq_id;
    uint32_t gsm_acq_l1fn;      /* L1's frame number when it started */
    NICConf nic_conf;           /* GPRS: the MS's IP packets */
    NICState *nic;
    GsmNet *net;                /* the cell and network (gsm_net.c) */
    GHashTable *seen;
};

/* the commands' lengths in bytes, by id (AMSS's table at 0x1E1EAC4) */
/* a packet data block's halfwords in the mDSP, CS-1 to CS-4 */
static const uint8_t gsm_pdch_hw[4] = { 12, 17, 20, 27 };

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
 * A PDCH burst's decode results besides the data (AMSS's 0xC85C66), at the
 * word addresses in the command, each [ready, the command's sequence
 * number, then per timeslot of the assignment a word]:
 *   byte 12  the bursts scheduled: 0x8000 when a block was decoded (AMSS
 *            reads it two frames on, for every burst)
 *   byte 14  the decoded USFs: bit 15 valid, bits 2-0 the USF
 */
/* the PDCH's slots a receive command reads (byte 20), up to 4 */
#define GSM_PDCH_SLOTS  4

/*
 * a packet data receive result record (its word address at byte at):
 * [ready, seq, a word for each slot]
 */
static void gsm_pdch_result(MsmMdspState *s, const uint8_t *cmd, int at,
                            uint16_t seq, const uint16_t *val, unsigned n)
{
    uint32_t r = MEM_B + 2 * lduw_le_p(cmd + at);

    if (r + 20 <= MEM_C) {
        memset(s->mem + r, 0, 20);
        stw_le_p(s->mem + r + 2, seq);
        for (unsigned i = 0; i < n; i++) {
            stw_le_p(s->mem + r + 4 + 2 * i, val[i]);
        }
        stw_le_p(s->mem + r, 1);
    }
}

/*
 * a decoded block for L1: [length in halfwords with this header, seq,
 * frame type, status, ?, ?] then the block.  The status: bit 1 CRC OK,
 * bits 6-3 a packet data block's coding scheme (L1 takes its length from
 * it, AMSS's 0x1071A9C: CS-1 to 4, then MCS-1 to 9), bits 15-13 its slot
 * of a multislot allocation (slot).  The frame type on a traffic channel
 * (AMSS's 0xC8C7E4): bit 0 a FACCH frame, else speech (bits 2-5 other
 * kinds: SID, ...); on signalling channels bit 0 is not set.
 */
static void gsm_rx_data(MsmMdspState *s, uint32_t dat, uint16_t seq,
                        int kind, int cs, unsigned slot, const uint8_t *b)
{
    if (cs >= 0) {
        memset(s->mem + dat, 0, 12 + 2 * gsm_pdch_hw[3]);
        memcpy(s->mem + dat + 12, b, 2 * gsm_pdch_hw[cs]);
        stw_le_p(s->mem + dat + 2, seq);
        stw_le_p(s->mem + dat + 6, 2 | cs << 3 | slot << 13);
        stw_le_p(s->mem + dat, 6 + gsm_pdch_hw[cs]);
        return;
    }
    memset(s->mem + dat, 0, 12 + 24);
    if (kind != GSM_DL_SPEECH) {
        memcpy(s->mem + dat + 12, b, GSM_BLOCK);
    }
    stw_le_p(s->mem + dat + 2, seq);
    stw_le_p(s->mem + dat + 4, kind == GSM_DL_FACCH);
    stw_le_p(s->mem + dat + 6, 2);
    stw_le_p(s->mem + dat, GSM_BLOCK);
}

/*
 * a slot's metrics (byte 4: the first's word address, the others' 0x28
 * words apart): 14 halfwords set here (traffic channels have 18; AMSS
 * keeps values of its own in the rest, which L1 tracking relies on)
 */
static void gsm_metrics(MsmMdspState *s, uint16_t seq, const uint8_t *cmd,
                        unsigned slot)
{
    uint32_t met = MEM_B + 2 * (lduw_le_p(cmd + 4) + 0x28 * slot);

    if (met + 28 > MEM_C) {
        return;
    }
    memset(s->mem + met, 0, 28);
    stw_le_p(s->mem + met + 2, seq);
    stw_le_p(s->mem + met + 6, 0x100);
    stw_le_p(s->mem + met + 8, 0x100);
    stw_le_p(s->mem + met, 1);
}

/*
 * a PDCH read (byte 2: 0x0A40; 0x0C40 on other channels): byte 20 the
 * number of slots (a multislot allocation's, from its lowest), then an
 * entry of 14 octets for each: [flags as on other channels, data word
 * address (0xFFFF but on a block's last burst), 0x0C00 | the slot's
 * index, its timing (625 quarter symbols a slot), ...].  The results:
 * byte 12's record a word for each slot, 0x8000 if a block was decoded
 * (L1 reads it two frames later), byte 14's the USFs.
 */
static void gsm_pdch(MsmMdspState *s, uint16_t seq, const uint8_t *cmd)
{
    unsigned n = MAX(MIN(lduw_le_p(cmd + 20), GSM_PDCH_SLOTS), 1);
    uint16_t sched[GSM_PDCH_SLOTS] = {}, usf[GSM_PDCH_SLOTS] = {};
    bool any = false;

    for (unsigned i = 0; i < n; i++) {
        const uint8_t *e = cmd + 22 + 14 * i;
        uint32_t dat = MEM_B + 2 * lduw_le_p(e + 2);
        uint8_t b[GSM_PDCH_BLOCK + 1] = {};
        int cs;

        if (i) {
            gsm_metrics(s, seq, cmd, i);
        }
        if (lduw_le_p(e + 2) == 0xffff ||
            dat + 12 + 2 * gsm_pdch_hw[3] > MEM_C) {
            continue;
        }
        cs = gsm_net_pdch_dl(s->net, s->gsm_fn - 3, lduw_le_p(e + 4) & 7, n,
                             b);
        gsm_rx_data(s, dat, seq, GSM_DL_SIGNAL, cs, i, b);
        sched[i] = 0x8000;
        usf[i] = 0x8000 | (b[0] & 7);
        any = true;
    }
    gsm_pdch_result(s, cmd, 12, seq, sched, n);
    if (any) {
        gsm_pdch_result(s, cmd, 14, seq, usf, n);
    }
}

static void gsm_accch(MsmMdspState *s, uint16_t seq, const uint8_t *cmd)
{
    uint32_t dat = MEM_B + 2 * lduw_le_p(cmd + 24);
    int kind = GSM_DL_SIGNAL;
    uint8_t b[GSM_BLOCK];

    gsm_metrics(s, seq, cmd, 0);
    if ((lduw_le_p(cmd + 2) >> 8) == 0x0a) {
        gsm_pdch(s, seq, cmd);
        return;
    }
    if (lduw_le_p(cmd + 24) == 0xffff || dat + 12 + 24 > MEM_C) {
        return;
    }
    if (gsm_net_dedicated(s->net)) {
        /*
         * byte 22: bit 0 decode, bit 4 a traffic channel's SACCH, bit 12
         * first burst; the block's four reads end at this frame
         */
        kind = gsm_net_ded_dl(s->net, s->gsm_fn - 3,
                              lduw_le_p(cmd + 22) & 0x10, b);
    } else {
        gsm_net_dl(s->net, gsm_net_block_fn(s->gsm_fn), b);
    }
    gsm_rx_data(s, dat, seq, kind, -1, 0, b);
}

/*
 * a burst goes out (command 30): byte 12 the number of entries (up to 2),
 * then for each [flags, word address of the entry].  The flags: bit 8 an
 * entry, the low byte the channel (GSM_UL_*; packet data GSM_UL_PDTCH plus
 * the coding scheme), bits 14-12 the uplink slot
 * of a multislot allocation, bit 15 a poll's answer (it goes before a
 * block the slot's USF asked for).  The entry is [length, type, data]: a
 * RACH's [4, 6, RA], a signalling block's [15, 1, 23 octets], a speech
 * frame's [20, 1, ...].  L1 learns which packet data entries went out from
 * their length being cleared: those it finds still there it sends again.
 */
static void gsm_tx_entry(MsmMdspState *s, unsigned flags, uint32_t e)
{
    unsigned chan = flags & 0xff, len = lduw_le_p(s->mem + e);

    if (len == 4) {
        gsm_net_rach(s->net, s->mem[e + 4], s->l1_fn, s->gsm_fn);
        return;
    }
    if (chan >= GSM_UL_PDTCH && chan < GSM_UL_PDTCH + 4) {
        /*
         * a packet data block, the channel GSM_UL_PDTCH plus its coding
         * scheme (CS-1 to CS-4): [3 + its halfwords, ...]
         */
        gsm_net_pdch_ul(s->net, s->gsm_fn, s->mem + e + 4,
                        chan - GSM_UL_PDTCH);
        stw_le_p(s->mem + e, 0);                /* sent */
        return;
    }
    gsm_net_ul(s->net, s->gsm_fn, chan, s->mem + e + 4);
}

static void gsm_tx(MsmMdspState *s, const uint8_t *cmd)
{
    unsigned n = MAX(MIN(lduw_le_p(cmd + 12), 2), 1);
    uint8_t slots = 0;

    if (s->trace || (s->gprs_trace &&
                     (lduw_le_p(cmd + 14) & 0xff) >= GSM_UL_PDTCH)) {
        GString *g = g_string_new(NULL);

        for (unsigned k = 0; k < 24; k += 2) {
            g_string_append_printf(g, " %04x", lduw_le_p(cmd + k));
        }
        for (unsigned i = 0; i < n; i++) {
            uint32_t x = MEM_B + 2 * lduw_le_p(cmd + 16 + 4 * i);

            if (x + 8 <= MEM_C) {
                g_string_append_printf(g, " | %04x %04x %04x %04x",
                                       lduw_le_p(s->mem + x),
                                       lduw_le_p(s->mem + x + 2),
                                       lduw_le_p(s->mem + x + 4),
                                       lduw_le_p(s->mem + x + 6));
            }
        }
        qemu_log("msm-mdsp: gsm tx fn %u l1 fn %u:%s\n", s->gsm_fn, s->l1_fn,
                 g->str);
        g_string_free(g, true);
    }
    /* polls' answers first, then the rest, one entry a slot */
    for (int poll = 1; poll >= 0; poll--) {
        for (unsigned i = 0; i < n; i++) {
            unsigned flags = lduw_le_p(cmd + 14 + 4 * i);
            unsigned slot = flags >> 12 & 7;
            uint32_t e = MEM_B + 2 * lduw_le_p(cmd + 16 + 4 * i);

            if (!(flags >> 15) != !poll || slots & 1 << slot ||
                e + 4 + GSM_PDCH_BLOCK + 1 > MEM_C ||
                !lduw_le_p(s->mem + e)) {
                continue;
            }
            slots |= 1 << slot;
            gsm_tx_entry(s, flags, e);
        }
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
        qemu_log_mask(LOG_UNIMP, "msm-mdsp: gsm sch id %u fn %u"
                      " %02x%02x%02x%02x l1 %u start %u\n", s->gsm_acq_id,
                      s->gsm_fn, b[0], b[1], b[2], b[3], s->l1_fn,
                      s->gsm_acq_l1fn);
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
    /*
     * The cell keeps L1's frame numbers.  A cell of its own, on virtual
     * time, drifts from them: on the phone the sleep controller restarts
     * the frame timer at an exact slow-clock tick, here AMSS's register
     * write does, a variable time late, and L1 then lags the cell by tens
     * of milliseconds after each sleep.
     */
    s->l1_fn = level - 1;
    s->gsm_fn = s->l1_fn;
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
                qemu_log_mask(LOG_UNIMP, "msm-mdsp: gsm acquisition (%u) l1 fn"
                              " %u\n", lduw_le_p(buf + off + 16), s->l1_fn);
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
    gsm_net_reset(s->net);
}

static void mdsp_set_gprs_cs(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    uint8_t cs;

    if (!visit_type_uint8(v, name, &cs, errp)) {
        return;
    }
    if (cs < 1 || cs > 4) {
        error_setg(errp, "gprs-cs: 1 to 4 (CS-1 to CS-4)");
        return;
    }
    gsm_net_set_gprs_cs(MSM_MDSP(obj)->net, cs - 1);
}

static void mdsp_set_gprs_slots(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    uint8_t n;

    if (!visit_type_uint8(v, name, &n, errp)) {
        return;
    }
    if (n < 1 || n > 4) {
        error_setg(errp, "gprs-slots: 1 to 4");
        return;
    }
    gsm_net_set_gprs_slots(MSM_MDSP(obj)->net, n);
}

static void mdsp_set_gprs_trace(Object *obj, bool value, Error **errp)
{
    MSM_MDSP(obj)->gprs_trace = value;
    gsm_net_set_gprs_trace(MSM_MDSP(obj)->net, value);
}

static bool mdsp_get_trace(Object *obj, Error **errp)
{
    return MSM_MDSP(obj)->trace;
}

static void mdsp_set_trace(Object *obj, bool value, Error **errp)
{
    MSM_MDSP(obj)->trace = value;
    gsm_net_set_trace(MSM_MDSP(obj)->net, value);
}

/*
 * The network's user, through QOM (tools/qmon.py ... "qom-set ..."):
 *   sms     "FROM:TEXT" sends the MS a text
 *   call    "FROM" calls it; reading gives the call's state
 *   hangup  true ends the call
 *   events  the network's last events (texts from the MS ...)
 */
static void mdsp_set_sms(Object *obj, const char *value, Error **errp)
{
    const char *colon = strchr(value, ':');
    g_autofree char *from = NULL;

    if (!colon) {
        error_setg(errp, "sms: expected FROM:TEXT");
        return;
    }
    from = g_strndup(value, colon - value);
    gsm_net_mt_sms(MSM_MDSP(obj)->net, from, colon + 1);
}

static char *mdsp_get_sms(Object *obj, Error **errp)
{
    return g_strdup("");
}

static void mdsp_set_call(Object *obj, const char *value, Error **errp)
{
    gsm_net_mt_call(MSM_MDSP(obj)->net, value);
}

static char *mdsp_get_call(Object *obj, Error **errp)
{
    return g_strdup(gsm_net_call_state(MSM_MDSP(obj)->net));
}

static void mdsp_set_hangup(Object *obj, bool value, Error **errp)
{
    if (value) {
        gsm_net_hangup(MSM_MDSP(obj)->net);
    }
}

static bool mdsp_get_hangup(Object *obj, Error **errp)
{
    return false;
}

static char *mdsp_get_events(Object *obj, Error **errp)
{
    return gsm_net_events(MSM_MDSP(obj)->net);
}

/* ---- GPRS: the MS's IP packets, Ethernet to the host (-nic model=msm-modem) */

static const uint8_t gw_mac[6] = { 0x52, 0x55, 0x0a, 0, 3, 2 };

static void mdsp_to_host(void *opaque, const uint8_t *ip, size_t len)
{
    MsmMdspState *s = opaque;
    g_autofree uint8_t *eth = g_malloc(14 + len);

    if (!s->nic) {
        return;
    }
    memcpy(eth, gw_mac, 6);
    memcpy(eth + 6, s->nic_conf.macaddr.a, 6);
    stw_be_p(eth + 12, ETH_P_IP);
    memcpy(eth + 14, ip, len);
    qemu_send_packet(qemu_get_queue(s->nic), eth, 14 + len);
}

static ssize_t mdsp_nic_receive(NetClientState *nc, const uint8_t *buf,
                                size_t len)
{
    MsmMdspState *s = qemu_get_nic_opaque(nc);
    uint32_t ip = gsm_net_ms_ip(s->net);

    if (len < 14 || !ip) {
        return len;
    }
    if (lduw_be_p(buf + 12) == ETH_P_ARP && len >= 42 &&
        lduw_be_p(buf + 20) == 1 && ldl_be_p(buf + 38) == ip) {
        /* who has the MS's address: we do */
        uint8_t f[42];

        memcpy(f, buf + 22, 6);
        memcpy(f + 6, s->nic_conf.macaddr.a, 6);
        stw_be_p(f + 12, ETH_P_ARP);
        stw_be_p(f + 14, 1);
        stw_be_p(f + 16, ETH_P_IP);
        f[18] = 6;
        f[19] = 4;
        stw_be_p(f + 20, 2);
        memcpy(f + 22, s->nic_conf.macaddr.a, 6);
        stl_be_p(f + 28, ip);
        memcpy(f + 32, buf + 22, 6);
        memcpy(f + 38, buf + 28, 4);
        qemu_send_packet(qemu_get_queue(s->nic), f, sizeof(f));
    } else if (lduw_be_p(buf + 12) == ETH_P_IP) {
        gsm_net_from_host(s->net, buf + 14, len - 14);
    }
    return len;
}

static NetClientInfo mdsp_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .receive = mdsp_nic_receive,
};

static void mdsp_realize(DeviceState *dev, Error **errp)
{
    MsmMdspState *s = MSM_MDSP(dev);

    qemu_macaddr_default_if_unset(&s->nic_conf.macaddr);
    s->nic = qemu_new_nic(&mdsp_net_info, &s->nic_conf, TYPE_MSM_MDSP, NULL,
                          &dev->mem_reentrancy_guard, s);
}

static const Property mdsp_props[] = {
    DEFINE_NIC_PROPERTIES(MsmMdspState, nic_conf),
};

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
    s->net = gsm_net_new(mdsp_to_host, s);
    object_property_add_bool(obj, "trace", mdsp_get_trace, mdsp_set_trace);
    object_property_add_bool(obj, "gprs-trace", NULL, mdsp_set_gprs_trace);
    object_property_add(obj, "gprs-cs", "uint8", NULL, mdsp_set_gprs_cs,
                        NULL, NULL);
    object_property_add(obj, "gprs-slots", "uint8", NULL,
                        mdsp_set_gprs_slots, NULL, NULL);
    object_property_add_str(obj, "sms", mdsp_get_sms, mdsp_set_sms);
    object_property_add_str(obj, "call", mdsp_get_call, mdsp_set_call);
    object_property_add_bool(obj, "hangup", mdsp_get_hangup, mdsp_set_hangup);
    object_property_add_str(obj, "events", mdsp_get_events, NULL);
}

static void mdsp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, mdsp_reset);
    dc->realize = mdsp_realize;
    device_class_set_props(dc, mdsp_props);
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
