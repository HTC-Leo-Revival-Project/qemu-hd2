/*
 * A GSM cell and the network behind it, for the mDSP model (msm_mdsp.c)
 *
 * The cell (MCC 310, MNC 260, LAC 1, cell 1, RAC 1) broadcasts SI1-4 and
 * SI13 (GPRS) on its BCCH
 * and pages, or assigns a channel to a RACH, on its CCCH (one CCCH
 * timeslot, not combined).  The channel it assigns is SDCCH/8 subchannel
 * 0 on timeslot 1, with SI5/SI6 on its SACCH.  On it the network runs
 * LAPDm (44.006: SAPI 0 for RR/MM/CC, SAPI 3 for SMS, window 1,
 * segmentation) and the network side of (24.008, 24.011, 44.018):
 *   - location updating: accepted, no authentication or ciphering
 *   - CM service requests and paging responses
 *   - calls (CC): an MS's call is answered after a few seconds; a call to
 *     the MS (gsm_net_mt_call) is paged for and set up; either side hangs
 *     up.  Calls get a TCH/F on timeslot 2 (early assignment); FACCH
 *     frames go in the place of speech, which is silence for now.
 *   - SMS (CP/RP/TP): texts from the MS are acknowledged and logged,
 *     texts to it (gsm_net_mt_sms) are paged for and delivered
 * The channel is released when nothing is left to do.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "gsm_net.h"
#include "gsm_gprs.h"

#define L3_MAX          251             /* an L3 message, at most */
#define L2_INFO         20              /* octets of L3 in an SDCCH frame */
#define PAGE_BLOCKS     400             /* CCCH blocks paged (~10 s) */
#define ANSWER_BLOCKS   12              /* a call is answered (~3 s) */
#define CM_WAIT_BLOCKS  80              /* for the service after accept */
#define SMSC            "+12063130004"

enum { SMS_IDLE, SMS_MT_EST, SMS_MT_WAIT_CPACK, SMS_MT_WAIT_RP, SMS_MO_WAIT_CPACK };
enum { CC_IDLE, CC_MO_PROC, CC_MO_ALERT, CC_MO_CONNECT, CC_ACTIVE,
       CC_MT_SETUP, CC_MT_RING, CC_DISC, CC_REL };
static const char *const cc_names[] = {
    "idle", "dialing", "alerting", "connecting", "active", "incoming",
    "ringing", "disconnecting", "releasing",
};

typedef struct GsmMsg {
    uint8_t m[L3_MAX];
    unsigned len, off;
} GsmMsg;

typedef struct GsmLapdm {
    unsigned sapi;
    bool up;
    bool sabm_pending, est_pending;     /* the network establishes */
    bool ua_pending, rr_pending;
    uint8_t ua[GSM_BLOCK];
    uint8_t vs, va, vr;
    uint8_t last_i[GSM_BLOCK];
    unsigned t200;
    GsmMsg q[8];
    unsigned qn;
    uint8_t rx[L3_MAX];                 /* a segmented message from the MS */
    unsigned rxlen;
} GsmLapdm;

typedef struct GsmSms {
    char *from, *text;
} GsmSms;

struct GsmNet {
    bool trace;
    /* CCCH */
    uint8_t rach_ra;
    bool rach_pkt;                      /* a packet access */
    uint32_t rach_fn;
    uint32_t ia_bfn;
    uint32_t page_bfn;
    unsigned page_left;
    uint8_t mi[9];                      /* the MS's identity (IMSI), LV */
    /* the dedicated channel */
    bool ded, releasing;
    bool tch;                           /* on the traffic channel */
    bool assigning;                     /* until ASSIGNMENT COMPLETE */
    unsigned idle, cm_wait;
    unsigned sacch_n;
    GsmLapdm l2[2];                     /* SAPI 0, SAPI 3 */
    /* SMS */
    GQueue mt_sms;
    int sms;
    uint8_t sms_ti, rp_mr;
    /* the call */
    int cc;
    uint8_t cc_ti;                      /* the network's TI octet, flag set */
    unsigned cc_timer;
    char *mt_call;                      /* calling number, to page for */
    bool hangup;
    GQueue events;
    Gprs *gprs;
};

/*
 * The broadcast, by TC in the BCCH block (t3 = 2): SI1, SI2, SI3, SI4,
 * then SI3/SI4 in the optional slots
 */
static const uint8_t si3[GSM_BLOCK] = {
    0x49, 0x06, 0x1b,                   /* L2 pseudo length 18, RR, SI3 */
    0x00, 0x01,                         /* cell identity */
    0x13, 0x00, 0x62, 0x00, 0x01,       /* LAI: 310 260, LAC 1 */
    0x48, 0x00, 0x0a,                   /* ATT, BS_AG_BLKS_RES 1, T3212 1 h */
    0x2f,                               /* cell options: RLT 64 */
    0x45, 0x40,                         /* reselect hysteresis, NECI */
    0x70, 0x00, 0x00,                   /* RACH: max retrans, tx-integer */
    0x2c, 0x2b, 0x2b, 0x2b,             /* rest octets: GPRS, SI13 on BCCH */
};
/* SI1: cell allocation (variable bit map, ARFCN 133), RACH control */
static const uint8_t si1[GSM_BLOCK] = {
    0x55, 0x06, 0x19,
    0x8e, 0x42, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x70, 0x00, 0x00,
    0x2b,
};
/* SI2: BA list (variable bit map, ARFCN 133), NCC permitted, RACH */
static const uint8_t si2[GSM_BLOCK] = {
    0x59, 0x06, 0x1a,
    0x8e, 0x42, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0xff,
    0x70, 0x00, 0x00,
};
/* SI4: LAI, cell selection parameters, RACH control */
static const uint8_t si4[GSM_BLOCK] = {
    0x31, 0x06, 0x1c,
    0x13, 0x00, 0x62, 0x00, 0x01,
    0x45, 0x40,
    0x70, 0x00, 0x00,
    0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b,
};
/* idle CCCH blocks: PAGING REQUEST TYPE 1 paging nobody */
static const uint8_t page_none[GSM_BLOCK] = {
    0x15, 0x06, 0x21, 0x00, 0x01, 0xf0, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b,
    0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b,
};
/*
 * CSN.1 rest octets: bits MSB first; L and H are the padding's bit at
 * that position (0x2B repeated) and its inverse
 */
typedef struct BitWriter {
    uint8_t *b;
    unsigned n, max;
} BitWriter;

static void bw_bit(BitWriter *w, int v)
{
    if (w->n < 8 * w->max) {
        uint8_t m = 0x80 >> (w->n % 8);

        w->b[w->n / 8] = v ? w->b[w->n / 8] | m : w->b[w->n / 8] & ~m;
        w->n++;
    }
}

static void bw_bits(BitWriter *w, unsigned v, int len)
{
    while (len--) {
        bw_bit(w, v >> len & 1);
    }
}

static void bw_lh(BitWriter *w, bool h)
{
    bw_bit(w, (0x2b >> (7 - w->n % 8) & 1) ^ h);
}

/* SI13: GPRS on the BCCH's channel, RAC 1, network mode II */
static void si13_build(uint8_t b[GSM_BLOCK])
{
    BitWriter w = { b + 3, 0, GSM_BLOCK - 3 };

    memset(b, 0x2b, GSM_BLOCK);
    b[0] = 0x01;                        /* L2 pseudo length 0 */
    b[1] = 0x06;
    b[2] = 0x00;                        /* SI13 */
    bw_lh(&w, true);
    bw_bits(&w, 1, 3);                  /* BCCH_CHANGE_MARK */
    bw_bits(&w, 0, 4);                  /* SI_CHANGE_FIELD */
    bw_bit(&w, 0);                      /* no SI13 change mark, GPRS MA */
    bw_bit(&w, 0);                      /* no PBCCH: */
    bw_bits(&w, GSM_RAC, 8);
    bw_bit(&w, 0);                      /* SPGC_CCCH_SUP */
    bw_bits(&w, 6, 3);                  /* PRIORITY_ACCESS_THR: all */
    bw_bits(&w, 0, 2);                  /* NETWORK_CONTROL_ORDER NC0 */
    /* GPRS cell options */
    bw_bits(&w, 1, 2);                  /* NMO II */
    bw_bits(&w, 3, 3);                  /* T3168 2 s */
    bw_bits(&w, 2, 3);                  /* T3192 1.5 s */
    bw_bits(&w, 3, 3);                  /* DRX_TIMER_MAX */
    bw_bit(&w, 0);                      /* ACCESS_BURST_TYPE 8 bit */
    bw_bit(&w, 1);                      /* CONTROL_ACK_TYPE: RLC/MAC block */
    bw_bits(&w, 15, 4);                 /* BS_CV_MAX */
    bw_bit(&w, 0);                      /* no PAN */
    bw_bit(&w, 0);                      /* no extension */
    /* GPRS power control parameters */
    bw_bits(&w, 0, 4);                  /* ALPHA */
    bw_bits(&w, 16, 5);                 /* T_AVG_W */
    bw_bits(&w, 16, 5);                 /* T_AVG_T */
    bw_bit(&w, 0);                      /* PC_MEAS_CHAN: BCCH */
    bw_bits(&w, 15, 4);                 /* N_AVG_I */
    bw_lh(&w, false);                   /* no release 99 additions */
    while (w.n % 8) {
        bw_lh(&w, false);
    }
}

/* the SACCH: L1 header (power, timing advance), UI, then SI5 or SI6 */
static const uint8_t si5[GSM_BLOCK] = {
    0x00, 0x00, 0x03, 0x03,
    0x49, 0x06, 0x1d,
    0x8e, 0x42, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
static const uint8_t si6[GSM_BLOCK] = {
    0x00, 0x00, 0x03, 0x03,
    0x2d, 0x06, 0x1e,
    0x00, 0x01,
    0x13, 0x00, 0x62, 0x00, 0x01,
    0x2f, 0xff,
    0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b,
};
static const uint8_t l2_fill[GSM_BLOCK] = {
    0x01, 0x03, 0x01, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b,
    0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b,
};

static void G_GNUC_PRINTF(2, 3) net_event(GsmNet *n, const char *fmt, ...)
{
    va_list ap;
    char *s;

    va_start(ap, fmt);
    s = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    qemu_log_mask(LOG_UNIMP, "gsm-net: %s\n", s);
    g_queue_push_tail(&n->events, s);
    while (g_queue_get_length(&n->events) > 32) {
        g_free(g_queue_pop_head(&n->events));
    }
}

/* ---- numbers and texts ---- */

/* "+1555..." as type of number and BCD; returns the BCD octets */
static unsigned bcd_encode(const char *num, uint8_t *toa, uint8_t *out,
                           unsigned *digits)
{
    unsigned nd = 0;

    *toa = *num == '+' ? 0x91 : 0x81;
    for (const char *p = num; *p; p++) {
        int d = *p == '*' ? 10 : *p == '#' ? 11 :
                g_ascii_isdigit(*p) ? *p - '0' : -1;

        if (d < 0 || nd >= 20) {
            continue;
        }
        if (nd & 1) {
            out[nd / 2] = (out[nd / 2] & 0x0f) | d << 4;
        } else {
            out[nd / 2] = 0xf0 | d;
        }
        nd++;
    }
    *digits = nd;
    return (nd + 1) / 2;
}

static char *bcd_decode(uint8_t toa, const uint8_t *b, unsigned n)
{
    GString *g = g_string_new(toa >> 4 == 0x9 || (toa & 0x70) == 0x10 ?
                              "+" : "");

    for (unsigned i = 0; i < 2 * n; i++) {
        unsigned d = i & 1 ? b[i / 2] >> 4 : b[i / 2] & 0xf;

        if (d == 0xf) {
            break;
        }
        g_string_append_c(g, "0123456789*#abc"[MIN(d, 14)]);
    }
    return g_string_free(g, false);
}

/* the GSM 7-bit default alphabet (23.038) */
static const char *const gsm7[128] = {
    "@", "£", "$", "¥", "è", "é", "ù", "ì", "ò", "Ç", "\n", "Ø", "ø", "\r",
    "Å", "å", "Δ", "_", "Φ", "Γ", "Λ", "Ω", "Π", "Ψ", "Σ", "Θ", "Ξ", "",
    "Æ", "æ", "ß", "É", " ", "!", "\"", "#", "¤", "%", "&", "'", "(", ")",
    "*", "+", ",", "-", ".", "/", "0", "1", "2", "3", "4", "5", "6", "7",
    "8", "9", ":", ";", "<", "=", ">", "?", "¡", "A", "B", "C", "D", "E",
    "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S",
    "T", "U", "V", "W", "X", "Y", "Z", "Ä", "Ö", "Ñ", "Ü", "§", "¿", "a",
    "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o",
    "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "ä", "ö", "ñ",
    "ü", "à",
};

/* a TP user data field as UTF-8 */
static char *sms_text(uint8_t dcs, bool udhi, const uint8_t *ud, unsigned udl,
                      unsigned avail)
{
    unsigned hdr = udhi && avail ? ud[0] + 1 : 0;
    GString *g = g_string_new(NULL);

    if ((dcs & 0x0c) == 0x08) {                 /* UCS2 */
        g_autofree gunichar2 *u = g_new0(gunichar2, udl / 2 + 1);
        unsigned k = 0;
        char *s;

        for (unsigned i = hdr; i + 1 < MIN(udl, avail); i += 2) {
            u[k++] = ud[i] << 8 | ud[i + 1];
        }
        s = g_utf16_to_utf8(u, k, NULL, NULL, NULL);
        g_string_append(g, s ? s : "?");
        g_free(s);
    } else if ((dcs & 0x0c) == 0x04) {          /* 8 bit */
        for (unsigned i = hdr; i < MIN(udl, avail); i++) {
            g_string_append_printf(g, "\\x%02x", ud[i]);
        }
    } else {
        unsigned skip = (hdr * 8 + 6) / 7;

        for (unsigned i = skip; i < udl; i++) {
            unsigned bit = i * 7, c;

            if (bit / 8 >= avail) {
                break;
            }
            c = ud[bit / 8] >> (bit % 8);
            if (bit % 8 > 1 && bit / 8 + 1 < avail) {
                c |= ud[bit / 8 + 1] << (8 - bit % 8);
            }
            g_string_append(g, gsm7[c & 0x7f]);
        }
    }
    return g_string_free(g, false);
}

/* text as TP-DCS and TP-UDL/UD (7 bit if it can, else UCS2) */
static unsigned sms_encode(const char *text, uint8_t *dcs, uint8_t *out,
                           unsigned max)
{
    uint8_t sept[160];
    unsigned ns = 0;
    const char *p;

    for (p = text; *p && ns < 160; p = g_utf8_next_char(p)) {
        unsigned len = g_utf8_next_char(p) - p, c;

        for (c = 0; c < 128; c++) {
            if (*gsm7[c] && strlen(gsm7[c]) == len && !memcmp(gsm7[c], p, len)) {
                break;
            }
        }
        if (c == 128) {
            break;
        }
        sept[ns++] = c;
    }
    if (!*p) {
        unsigned bytes = (ns * 7 + 7) / 8;

        *dcs = 0x00;
        memset(out + 1, 0, MIN(bytes, max - 1));
        for (unsigned i = 0; i < ns; i++) {
            unsigned bit = i * 7;

            out[1 + bit / 8] |= sept[i] << (bit % 8);
            if (bit % 8 > 1 && 1 + bit / 8 + 1 < max) {
                out[1 + bit / 8 + 1] |= sept[i] >> (8 - bit % 8);
            }
        }
        out[0] = ns;
        return 1 + bytes;
    } else {
        glong k = 0;
        g_autofree gunichar2 *u = g_utf8_to_utf16(text, -1, NULL, &k, NULL);
        unsigned n = 0;

        *dcs = 0x08;
        for (glong i = 0; u && i < k && 1 + n + 2 <= MIN(max, 141); i++) {
            out[1 + n++] = u[i] >> 8;
            out[1 + n++] = u[i];
        }
        out[0] = n;
        return 1 + n;
    }
}

/* ---- LAPDm ---- */

static GsmLapdm *l2(GsmNet *n, unsigned sapi)
{
    return &n->l2[sapi == 3];
}

/* an L3 message to the MS on SAPI 0 or 3 */
static void net_send(GsmNet *n, unsigned sapi, const uint8_t *m, unsigned len)
{
    GsmLapdm *l = l2(n, sapi);

    if (l->qn < ARRAY_SIZE(l->q) && len <= L3_MAX) {
        memcpy(l->q[l->qn].m, m, len);
        l->q[l->qn].len = len;
        l->q[l->qn].off = 0;
        l->qn++;
    }
}

static void lapdm_reset(GsmLapdm *l)
{
    unsigned sapi = l->sapi;

    memset(l, 0, sizeof(*l));
    l->sapi = sapi;
}

static void lapdm_ack(GsmLapdm *l, unsigned nr)
{
    if (((nr - l->va) & 7) <= ((l->vs - l->va) & 7)) {
        l->va = nr;
        if (l->va == l->vs) {
            l->t200 = 0;
        }
    }
}

static void net_l3(GsmNet *n, unsigned sapi, const uint8_t *m, unsigned len);

static void lapdm_ul(GsmNet *n, GsmLapdm *l, const uint8_t *f)
{
    uint8_t c = f[1];
    unsigned len = MIN(f[2] >> 2, L2_INFO);
    bool more = f[2] >> 1 & 1;

    if ((c & 3) == 3) {                         /* U frames */
        switch (c & ~0x10) {
        case 0x2f: {                            /* SABM: UA, information */
            /* (the messages queued go on the new link, after a handover) */
            GsmMsg q[ARRAY_SIZE(l->q)];
            unsigned qn = l->qn;

            memcpy(q, l->q, sizeof(q));
            lapdm_reset(l);
            memcpy(l->q, q, sizeof(q));
            l->qn = qn;
            if (qn) {
                l->q[0].off = 0;
            }
            l->up = true;
            memset(l->ua, 0x2b, GSM_BLOCK);
            l->ua[0] = l->sapi << 2 | 1;
            l->ua[1] = 0x63 | (c & 0x10);
            l->ua[2] = len << 2 | 1;
            memcpy(l->ua + 3, f + 3, len);
            l->ua_pending = true;
            if (len) {
                net_l3(n, l->sapi, f + 3, len);
            }
            break;
        }
        case 0x63:                              /* UA to our SABM */
            if (l->est_pending) {
                l->est_pending = false;
                l->up = true;
                l->vs = l->va = l->vr = 0;
            }
            break;
        case 0x43:                              /* DISC */
            memcpy(l->ua, l2_fill, GSM_BLOCK);
            l->ua[0] = l->sapi << 2 | 1;
            l->ua[1] = 0x63 | (c & 0x10);
            l->ua_pending = true;
            l->up = false;
            l->qn = 0;
            break;
        case 0x0f:                              /* DM */
            l->est_pending = l->up = false;
            break;
        }
    } else if (!(c & 1)) {                      /* I frames */
        lapdm_ack(l, c >> 5);
        if ((c >> 1 & 7) == l->vr) {
            l->vr = (l->vr + 1) & 7;
            if (l->rxlen + len <= L3_MAX) {
                memcpy(l->rx + l->rxlen, f + 3, len);
                l->rxlen += len;
            }
            if (!more) {
                net_l3(n, l->sapi, l->rx, l->rxlen);
                l->rxlen = 0;
            }
        }
        l->rr_pending = true;
    } else {                                    /* S frames */
        lapdm_ack(l, c >> 5);
    }
}

/* SAPI l's next frame to the MS, false if it has none */
static bool lapdm_dl(GsmLapdm *l, uint8_t b[GSM_BLOCK])
{
    memcpy(b, l2_fill, GSM_BLOCK);
    if (l->ua_pending) {
        memcpy(b, l->ua, GSM_BLOCK);
        l->ua_pending = false;
        return true;
    }
    if (l->sabm_pending || (l->est_pending && ++l->t200 >= 4)) {
        b[0] = l->sapi << 2 | 3;
        b[1] = 0x3f;
        l->sabm_pending = false;
        l->est_pending = true;
        l->t200 = 0;
        return true;
    }
    if (!l->up) {
        return false;
    }
    if (l->vs != l->va) {
        /* T200: the I frame is sent again after three blocks */
        if (++l->t200 >= 3) {
            memcpy(b, l->last_i, GSM_BLOCK);
            b[1] = l->vr << 5 | (b[1] & 0x1f);
            l->t200 = 0;
            l->rr_pending = false;
            return true;
        }
    } else if (l->qn) {
        GsmMsg *m = &l->q[0];
        unsigned seg = MIN(m->len - m->off, L2_INFO);
        bool more = m->off + seg < m->len;

        memset(b, 0x2b, GSM_BLOCK);
        b[0] = l->sapi << 2 | 3;
        b[1] = l->vr << 5 | l->vs << 1;
        b[2] = seg << 2 | more << 1 | 1;
        memcpy(b + 3, m->m + m->off, seg);
        m->off += seg;
        if (!more) {
            memmove(&l->q[0], &l->q[1], sizeof(l->q[0]) * --l->qn);
        }
        memcpy(l->last_i, b, GSM_BLOCK);
        l->vs = (l->vs + 1) & 7;
        l->t200 = 0;
        l->rr_pending = false;
        return true;
    }
    if (l->rr_pending) {
        b[0] = l->sapi << 2 | 1;
        b[1] = l->vr << 5 | 0x01;
        l->rr_pending = false;
        return true;
    }
    return false;
}

static bool lapdm_quiet(GsmLapdm *l)
{
    return !l->qn && l->vs == l->va && !l->ua_pending && !l->sabm_pending &&
           !l->est_pending;
}

/* ---- the network's procedures ---- */

static void cc_send(GsmNet *n, uint8_t type, const uint8_t *ie, unsigned len)
{
    uint8_t m[L3_MAX] = { n->cc_ti | 0x03, type };

    memcpy(m + 2, ie, len);
    net_send(n, 0, m, 2 + len);
}

static void cc_set(GsmNet *n, int st)
{
    if (n->cc != st) {
        net_event(n, "call: %s", cc_names[st]);
    }
    n->cc = st;
    n->cc_timer = 0;
}

/* ASSIGNMENT COMMAND: TCH/F on timeslot 2, speech full rate */
static void net_assign(GsmNet *n)
{
    static const uint8_t ass[] = {
        0x06, 0x2e, 0x01 << 3 | 2,
        GSM_TSC << 5 | (GSM_ARFCN >> 8 & 3), GSM_ARFCN & 0xff,
        0x00, 0x63, 0x01,
    };

    if (!n->tch && !n->assigning) {
        net_send(n, 0, ass, sizeof(ass));
        n->assigning = true;
        net_event(n, "assignment to TCH/F");
    }
}

static void cc_disconnect(GsmNet *n)
{
    static const uint8_t cause[] = { 0x02, 0xe0, 0x90 };    /* normal */

    cc_send(n, 0x25, cause, sizeof(cause));
    cc_set(n, CC_DISC);
}

static void sms_mt_start(GsmNet *n)
{
    GsmSms *sms = g_queue_peek_head(&n->mt_sms);
    uint8_t rp[L3_MAX], cp[L3_MAX], toa;
    unsigned k = 0, nd, len;

    /* RP-DATA (network to MS): RP-OA the SMSC, RP-UD an SMS-DELIVER */
    rp[k++] = 0x01;
    rp[k++] = ++n->rp_mr;
    len = bcd_encode(SMSC, &toa, rp + k + 2, &nd);
    rp[k] = 1 + len;
    rp[k + 1] = toa;
    k += 2 + len;
    rp[k++] = 0;                                /* RP-DA */
    {
        uint8_t *tp = rp + k + 1, dcs;
        unsigned t = 0, dcs_at;
        g_autoptr(GDateTime) now = g_date_time_new_now_local();
        int tz = g_date_time_get_utc_offset(now) / (15 * 60 * G_USEC_PER_SEC);
        unsigned ts[6] = {
            g_date_time_get_year(now) % 100, g_date_time_get_month(now),
            g_date_time_get_day_of_month(now), g_date_time_get_hour(now),
            g_date_time_get_minute(now), g_date_time_get_second(now),
        };

        tp[t++] = 0x04;                         /* SMS-DELIVER, no more */
        len = bcd_encode(sms->from, &toa, tp + t + 2, &nd);
        tp[t] = nd;
        tp[t + 1] = toa;
        t += 2 + len;
        tp[t++] = 0x00;                         /* PID */
        dcs_at = t++;                           /* DCS */
        for (unsigned i = 0; i < 6; i++) {
            tp[t++] = (ts[i] % 10) << 4 | ts[i] / 10;
        }
        tp[t++] = (abs(tz) % 10) << 4 | (abs(tz) / 10) | (tz < 0 ? 0x08 : 0);
        t += sms_encode(sms->text, &dcs, tp + t, L3_MAX - k - 4 - t);
        tp[dcs_at] = dcs;
        rp[k] = t;
        k += 1 + t;
    }
    /* CP-DATA, the network's transaction 0 */
    cp[0] = 0x09;
    cp[1] = 0x01;
    cp[2] = k;
    memcpy(cp + 3, rp, k);
    n->sms_ti = 0x09;
    net_send(n, 3, cp, 3 + k);
    n->sms = SMS_MT_WAIT_CPACK;
    net_event(n, "SMS from %s: delivering", sms->from);
}

/* what the network still has to do on the channel, if it is up */
static void net_service(GsmNet *n)
{
    GsmLapdm *s0 = l2(n, 0), *s3 = l2(n, 3);

    if (!n->ded || !s0->up || n->releasing) {
        return;
    }
    if (n->sms == SMS_IDLE && n->cc != CC_MT_SETUP &&
        !g_queue_is_empty(&n->mt_sms)) {
        if (s3->up) {
            sms_mt_start(n);
        } else if (!s3->est_pending && !s3->sabm_pending) {
            s3->sabm_pending = true;
            n->sms = SMS_MT_EST;
        }
    }
    if (n->sms == SMS_MT_EST && s3->up) {
        sms_mt_start(n);
    }
    if (n->cc == CC_IDLE && n->mt_call && n->sms == SMS_IDLE) {
        /* SETUP: speech, the calling number */
        uint8_t ie[32] = { 0x04, 0x01, 0xa0, 0x5c }, toa;
        unsigned nd, len = bcd_encode(n->mt_call, &toa, ie + 6, &nd);

        ie[4] = 1 + len;
        ie[5] = toa;
        n->cc_ti = 0x00;
        cc_send(n, 0x05, ie, 6 + len);
        cc_set(n, CC_MT_SETUP);
        g_free(n->mt_call);
        n->mt_call = NULL;
    }
    if (n->hangup) {
        n->hangup = false;
        if (n->cc != CC_IDLE && n->cc != CC_DISC && n->cc != CC_REL) {
            cc_disconnect(n);
        }
    }
    /* an MS's call: alerting, then answered */
    if (n->cc == CC_MO_PROC && !n->assigning &&
        ++n->cc_timer >= ANSWER_BLOCKS / 2) {
        cc_send(n, 0x01, NULL, 0);
        cc_set(n, CC_MO_ALERT);
    } else if (n->cc == CC_MO_ALERT && ++n->cc_timer >= ANSWER_BLOCKS * 2) {
        cc_send(n, 0x07, NULL, 0);
        cc_set(n, CC_MO_CONNECT);
    }
    if (n->cm_wait) {
        n->cm_wait--;
    }
    /* nothing left: release the channel */
    if (n->sms == SMS_IDLE && n->cc == CC_IDLE && !n->cm_wait &&
        g_queue_is_empty(&n->mt_sms) && !n->mt_call &&
        lapdm_quiet(s0) && lapdm_quiet(s3)) {
        static const uint8_t rel[] = { 0x06, 0x0d, 0x00 };

        net_send(n, 0, rel, sizeof(rel));
        n->releasing = true;
    }
}

static void net_identity(GsmNet *n, const uint8_t *mi, unsigned left)
{
    if (left >= 2 && mi[0] <= 8 && mi[0] + 1u <= left &&
        (mi[1] & 7) == 1) {                     /* an IMSI */
        memcpy(n->mi, mi, mi[0] + 1);
    }
}

static void net_mm(GsmNet *n, const uint8_t *m, unsigned len)
{
    unsigned mt = m[1] & 0x3f;

    switch (mt) {
    case 0x08: {                                /* LOCATION UPDATING REQUEST */
        static const uint8_t acc[] = { 0x05, 0x02, 0x13, 0x00, 0x62, 0x00, 0x01 };

        if (len > 9) {
            net_identity(n, m + 9, len - 9);
        }
        net_send(n, 0, acc, sizeof(acc));
        net_event(n, "location update accepted");
        break;
    }
    case 0x24: {                                /* CM SERVICE REQUEST */
        static const uint8_t acc[] = { 0x05, 0x21 };

        if (len > 4 && 4u + m[3] < len) {
            net_identity(n, m + 4 + m[3], len - 4 - m[3]);
        }
        net_send(n, 0, acc, sizeof(acc));
        n->cm_wait = CM_WAIT_BLOCKS;
        net_event(n, "CM service %u accepted", m[2] & 0xf);
        break;
    }
    case 0x01:                                  /* IMSI DETACH INDICATION */
        net_event(n, "IMSI detach");
        break;
    }
}

static void net_rr(GsmNet *n, const uint8_t *m, unsigned len)
{
    if ((m[1] & 0x3f) == 0x29) {                /* ASSIGNMENT COMPLETE */
        n->assigning = false;
        net_event(n, "on TCH/F");
        return;
    }
    if ((m[1] & 0x3f) == 0x2f) {                /* ASSIGNMENT FAILURE */
        n->assigning = n->tch = false;
        net_event(n, "assignment failed");
        return;
    }
    if ((m[1] & 0x3f) == 0x27) {                /* PAGING RESPONSE */
        if (len > 4 && 4u + m[3] < len) {
            net_identity(n, m + 4 + m[3], len - 4 - m[3]);
        }
        n->page_left = 0;
        net_event(n, "paging response");
    }
}

static void net_cc(GsmNet *n, const uint8_t *m, unsigned len)
{
    unsigned mt = m[1] & 0x3f;

    n->cm_wait = 0;
    /* the TI octet of our answers: the other side's flag */
    if (!(m[0] & 0x80)) {
        n->cc_ti = 0x80 | (m[0] & 0x70);
    }
    switch (mt) {
    case 0x05: {                                /* SETUP */
        g_autofree char *num = NULL;

        /* IEs: one octet if bit 8 is set, else type, length, value */
        for (unsigned i = 2; i < len;) {
            if (m[i] & 0x80) {
                i++;
                continue;
            }
            if (i + 1 >= len || i + 2 + m[i + 1] > len) {
                break;
            }
            if (m[i] == 0x5e && m[i + 1] >= 1) {
                num = bcd_decode(m[i + 2], m + i + 3, m[i + 1] - 1);
                break;
            }
            i += 2 + m[i + 1];
        }
        net_event(n, "call to %s", num ? num : "?");
        cc_send(n, 0x02, NULL, 0);              /* CALL PROCEEDING */
        cc_set(n, CC_MO_PROC);
        net_assign(n);
        break;
    }
    case 0x0f:                                  /* CONNECT ACKNOWLEDGE */
        cc_set(n, CC_ACTIVE);
        break;
    case 0x08:                                  /* CALL CONFIRMED */
        net_assign(n);                          /* early assignment */
        break;
    case 0x01:                                  /* ALERTING */
        cc_set(n, CC_MT_RING);
        break;
    case 0x07:                                  /* CONNECT */
        cc_send(n, 0x0f, NULL, 0);
        cc_set(n, CC_ACTIVE);
        break;
    case 0x25: {                                /* DISCONNECT */
        cc_send(n, 0x2d, NULL, 0);              /* RELEASE */
        cc_set(n, CC_REL);
        break;
    }
    case 0x2d:                                  /* RELEASE */
        cc_send(n, 0x2a, NULL, 0);              /* RELEASE COMPLETE */
        cc_set(n, CC_IDLE);
        break;
    case 0x2a:                                  /* RELEASE COMPLETE */
        cc_set(n, CC_IDLE);
        break;
    case 0x35: {                                /* START DTMF */
        uint8_t ie[2] = { 0x2c, len > 3 ? m[3] : '0' };

        net_event(n, "DTMF %c", ie[1]);
        cc_send(n, 0x36, ie, 2);
        break;
    }
    case 0x31:                                  /* STOP DTMF */
        cc_send(n, 0x32, NULL, 0);
        break;
    case 0x34: {                                /* STATUS ENQUIRY */
        static const uint8_t st[] = { 0x02, 0xe0, 0x9e, 0x14, 0x0a };
        uint8_t ie[sizeof(st)];

        memcpy(ie, st, sizeof(st));
        ie[4] = n->cc == CC_ACTIVE ? 0x0a : n->cc == CC_MT_RING ? 0x07 : 0x03;
        cc_send(n, 0x3d, ie, sizeof(ie));
        break;
    }
    }
}

static void net_sms(GsmNet *n, const uint8_t *m, unsigned len)
{
    unsigned mt = m[1];

    if (mt == 0x04) {                           /* CP-ACK */
        if (n->sms == SMS_MT_WAIT_CPACK) {
            n->sms = SMS_MT_WAIT_RP;
        } else if (n->sms == SMS_MO_WAIT_CPACK) {
            n->sms = SMS_IDLE;
        }
        return;
    }
    if (mt != 0x01 || len < 4) {                /* CP-DATA */
        return;
    }
    n->cm_wait = 0;
    {
        uint8_t ack[2] = { (uint8_t)((m[0] & 0x70) | 0x09 |
                                     (~m[0] & 0x80)), 0x04 };
        const uint8_t *rp = m + 3;
        unsigned rlen = MIN(m[2], len - 3);

        net_send(n, 3, ack, 2);                 /* CP-ACK */
        if (rlen >= 2 && (rp[0] & 7) == 0x00) {
            /* RP-DATA from the MS: the SMS-SUBMIT */
            unsigned k = 2;
            uint8_t rpack[5] = { ack[0], 0x01, 2, 0x03, rp[1] };
            g_autofree char *to = NULL, *text = NULL;

            k += 1 + rp[k];                     /* RP-OA */
            k += 1 + rp[k];                     /* RP-DA */
            if (k < rlen) {
                const uint8_t *tp = rp + k + 1;
                unsigned tl = MIN(rp[k], rlen - k - 1), t = 2, vpf;

                if (tl > 4) {
                    unsigned nd = tp[t];

                    to = bcd_decode(tp[t + 1], tp + t + 2, (nd + 1) / 2);
                    t += 2 + (nd + 1) / 2;
                    vpf = tp[0] >> 3 & 3;
                    if (t + 2 < tl) {
                        uint8_t dcs = tp[t + 1];

                        t += 2 + (vpf == 2 ? 1 : vpf ? 7 : 0);
                        if (t < tl) {
                            text = sms_text(dcs, tp[0] & 0x40, tp + t + 1,
                                            tp[t], tl - t - 1);
                        }
                    }
                }
            }
            net_event(n, "SMS to %s: %s", to ? to : "?", text ? text : "?");
            net_send(n, 3, rpack, sizeof(rpack));   /* CP-DATA: RP-ACK */
            n->sms = SMS_MO_WAIT_CPACK;
        } else if (rlen >= 2 && ((rp[0] & 7) == 0x02 || (rp[0] & 7) == 0x04)) {
            /* RP-ACK / RP-ERROR to our RP-DATA */
            GsmSms *sms = g_queue_pop_head(&n->mt_sms);

            if (sms) {
                net_event(n, "SMS from %s: %s", sms->from,
                          (rp[0] & 7) == 0x02 ? "delivered" : "rejected");
                g_free(sms->from);
                g_free(sms->text);
                g_free(sms);
            }
            n->sms = SMS_IDLE;
        }
    }
}

static void net_l3(GsmNet *n, unsigned sapi, const uint8_t *m, unsigned len)
{
    if (len < 2) {
        return;
    }
    if (n->trace) {
        qemu_log("gsm-net: L3 SAPI %u pd %u type %02x (%u octets)\n", sapi,
                 m[0] & 0xf, m[1], len);
    }
    switch (m[0] & 0xf) {
    case 5:
        net_mm(n, m, len);
        break;
    case 6:
        net_rr(n, m, len);
        break;
    case 3:
        net_cc(n, m, len);
        break;
    case 9:
        net_sms(n, m, len);
        break;
    }
}

static void net_free_channel(GsmNet *n)
{
    n->ded = n->releasing = n->tch = n->assigning = false;
    lapdm_reset(&n->l2[0]);
    lapdm_reset(&n->l2[1]);
    n->sms = SMS_IDLE;
    if (n->cc != CC_IDLE) {
        cc_set(n, CC_IDLE);
    }
    n->cm_wait = 0;
}

/* ---- the cell ---- */

uint32_t gsm_net_block_fn(uint32_t fn)
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
static uint32_t ccch_after(uint32_t fn)
{
    uint32_t b = gsm_net_block_fn(fn);

    while (b <= fn || b % 51 == 2) {
        b = gsm_net_block_fn(b + 4);
    }
    return b;
}

/*
 * IMMEDIATE ASSIGNMENT (44.018 9.1.18) answering the last RACH: SDCCH/8
 * subchannel 0 on timeslot 1, the request reference (RA and the RACH's
 * frame number as T1', T3, T2), timing advance 0
 */
static void imm_ass(GsmNet *n, uint8_t b[GSM_BLOCK])
{
    uint32_t fn = n->rach_fn;
    unsigned t1p = fn / 1326 % 32, t2 = fn % 26, t3 = fn % 51;
    const uint8_t m[] = {
        0x2d, 0x06, 0x3f,                   /* L2 pseudo length 11, RR, IA */
        0x00,                               /* dedicated, normal paging */
        0x08 << 3 | 1,                      /* SDCCH/8 subchannel 0, TN 1 */
        GSM_TSC << 5 | (GSM_ARFCN >> 8 & 3), GSM_ARFCN & 0xff,
        n->rach_ra, t1p << 3 | t3 >> 3, (t3 & 7) << 5 | t2,
        0x00,                               /* timing advance */
        0x00,                               /* mobile allocation: none */
    };

    memset(b, 0x2b, GSM_BLOCK);
    memcpy(b, m, sizeof(m));
}

/*
 * IMMEDIATE ASSIGNMENT of an uplink TBF (one phase packet access): a PDCH
 * on timeslot 3, dynamic allocation with USF GSM_USF, TFI 0, CS-1
 */
static void imm_ass_pkt(GsmNet *n, uint8_t b[GSM_BLOCK])
{
    uint32_t fn = n->rach_fn;
    unsigned t1p = fn / 1326 % 32, t2 = fn % 26, t3 = fn % 51;
    const uint8_t m[] = {
        0x2d, 0x06, 0x3f,                   /* L2 pseudo length 11, RR, IA */
        0x10,                               /* TBF, uplink; normal paging */
        0x01 << 3 | GSM_PDCH_TN,            /* packet channel description */
        GSM_TSC << 5 | (GSM_ARFCN >> 8 & 3), GSM_ARFCN & 0xff,
        n->rach_ra, t1p << 3 | t3 >> 3, (t3 & 7) << 5 | t2,
        0x00,                               /* timing advance */
        0x00,                               /* mobile allocation: none */
    };
    BitWriter w = { b + sizeof(m), 0, GSM_BLOCK - sizeof(m) };

    memset(b, 0x2b, GSM_BLOCK);
    memcpy(b, m, sizeof(m));
    /* IA rest octets: HH, Packet Uplink Assignment */
    bw_lh(&w, true);
    bw_lh(&w, true);
    bw_bits(&w, 0, 2);
    bw_bit(&w, 1);                      /* dynamic or fixed allocation */
    bw_bits(&w, GSM_UL_TFI, 5);
    bw_bit(&w, 0);                      /* POLLING */
    bw_bit(&w, 0);                      /* dynamic allocation */
    bw_bits(&w, GSM_USF, 3);
    bw_bit(&w, 0);                      /* USF_GRANULARITY: one block */
    bw_bit(&w, 0);                      /* no P0 */
    bw_bits(&w, 0, 2);                  /* CHANNEL_CODING_COMMAND: CS-1 */
    bw_bit(&w, 0);                      /* TLLI_BLOCK_CHANNEL_CODING: CS-1 */
    bw_bit(&w, 0);                      /* no ALPHA */
    bw_bits(&w, 0, 5);                  /* GAMMA */
    bw_bit(&w, 0);                      /* no TIMING_ADVANCE_INDEX */
    bw_bit(&w, 0);                      /* no TBF_STARTING_TIME */
    bw_lh(&w, false);                   /* no release 99 additions */
    while (w.n % 8) {
        bw_lh(&w, false);
    }
}

void gsm_net_rach(GsmNet *n, uint8_t ra, uint32_t l1_fn, uint32_t fn)
{
    /* 0111 1xxx one phase packet access, 0111 0xxx single block */
    n->rach_pkt = (ra & 0xf0) == 0x70;
    n->rach_ra = ra;
    n->rach_fn = l1_fn;
    n->ia_bfn = ccch_after(fn + 2);
    qemu_log_mask(LOG_UNIMP, "gsm-net: RACH ra %02x fn %u, answer in block"
                  " %u\n", ra, l1_fn, n->ia_bfn);
}

bool gsm_net_dedicated(GsmNet *n)
{
    return n->ded;
}

void gsm_net_dl(GsmNet *n, uint32_t bfn, uint8_t b[GSM_BLOCK])
{
    unsigned t3 = bfn % 51, tc = (bfn / 51) % 8;
    static const uint8_t *const bcch[8] = {
        si1, si2, si3, si4, NULL, si4, si3, si4,     /* TC 4: SI13 */
    };

    if (t3 == 2 && !bcch[tc]) {
        si13_build(b);
    } else if (t3 == 2) {
        memcpy(b, bcch[tc], GSM_BLOCK);
    } else if (n->ia_bfn && bfn == n->ia_bfn) {
        if (n->rach_pkt) {
            imm_ass_pkt(n, b);
            gprs_ul_assigned(n->gprs);
        } else {
            imm_ass(n, b);
        }
        qemu_log_mask(LOG_UNIMP, "gsm-net: IMMEDIATE ASSIGNMENT in block"
                      " %u\n", bfn);
    } else if (gprs_ccch_assignment(n->gprs, bfn, b)) {
        /* a downlink TBF */
    } else if (n->page_left && n->mi[0]) {
        /* PAGING REQUEST TYPE 1 for the MS's IMSI, in every CCCH block */
        unsigned l = 3 + 1 + n->mi[0];

        memset(b, 0x2b, GSM_BLOCK);
        b[0] = l << 2 | 1;
        b[1] = 0x06;
        b[2] = 0x21;
        b[3] = 0x00;
        memcpy(b + 4, n->mi, n->mi[0] + 1);
        if (bfn != n->page_bfn) {
            n->page_bfn = bfn;
            if (!--n->page_left) {
                net_event(n, "paging: no answer");
                if (n->mt_call) {
                    g_free(n->mt_call);
                    n->mt_call = NULL;
                }
            }
        }
    } else {
        memcpy(b, page_none, GSM_BLOCK);
    }
}

int gsm_net_ded_dl(GsmNet *n, uint32_t bfn, bool sacch, uint8_t b[GSM_BLOCK])
{
    int kind = GSM_DL_SIGNAL;
    bool tch = n->tch;

    /* SDCCH/8: subchannel 0 at frames 0-3, the SACCHs at 32-47 */
    if (!n->tch && bfn % 51 >= 32) {
        sacch = true;
    }

    if (sacch) {
        memcpy(b, n->sacch_n++ & 1 ? si6 : si5, GSM_BLOCK);
    } else {
        /* the MS sends something every block: if not, it has left */
        if (++n->idle > 24) {
            net_event(n, "the MS left the channel");
            net_free_channel(n);
            memcpy(b, l2_fill, GSM_BLOCK);
            return GSM_DL_SIGNAL;
        }
        net_service(n);
        if (!lapdm_dl(&n->l2[0], b) && !lapdm_dl(&n->l2[1], b)) {
            memcpy(b, l2_fill, GSM_BLOCK);
            kind = tch ? GSM_DL_SPEECH : GSM_DL_SIGNAL;
        } else if (tch) {
            kind = GSM_DL_FACCH;
        }
        if (n->releasing && !n->l2[0].up && !n->l2[0].ua_pending) {
            net_free_channel(n);
        }
    }
    if (n->trace && kind != GSM_DL_SPEECH) {
        qemu_log("gsm-net: DL %s: %02x %02x %02x %02x %02x %02x\n",
                 sacch ? "SACCH" : kind == GSM_DL_FACCH ? "FACCH" : "SDCCH",
                 b[0], b[1], b[2], b[3], b[4], b[5]);
    }
    return kind;
}

void gsm_net_ul(GsmNet *n, uint32_t fn, int chan, const uint8_t f[GSM_BLOCK])
{
    unsigned t3 = fn % 51, sapi = f[0] >> 2 & 7;

    if (n->ded) {
        n->idle = 0;
    }
    if (chan == GSM_UL_SPEECH) {
        return;
    }
    if (chan == GSM_UL_PDTCH) {
        gprs_pdch_ul(n->gprs, fn, f);
        return;
    }
    if (chan == GSM_UL_SDCCH && t3 >= 47) {
        return;                         /* the SACCH: measurement reports */
    }
    if (n->trace) {
        qemu_log("gsm-net: UL %s: %02x %02x %02x %02x %02x %02x\n",
                 chan == GSM_UL_FACCH ? "FACCH" : "SDCCH",
                 f[0], f[1], f[2], f[3], f[4], f[5]);
    }
    if (chan == GSM_UL_FACCH && n->ded && !n->tch) {
        n->tch = true;                  /* the MS is on its traffic channel */
    }
    if (sapi != 0 && sapi != 3) {
        return;
    }
    if (!n->ded) {
        if (sapi != 0 || (f[1] & ~0x10) != 0x2f) {
            return;                     /* only a SABM opens the channel */
        }
        n->ded = true;
        n->releasing = false;
        n->ia_bfn = 0;
    }
    lapdm_ul(n, l2(n, sapi), f);
}

/* ---- the network's user ---- */

void gsm_net_mt_sms(GsmNet *n, const char *from, const char *text)
{
    GsmSms *sms = g_new0(GsmSms, 1);

    sms->from = g_strdup(from);
    sms->text = g_strdup(text);
    g_queue_push_tail(&n->mt_sms, sms);
    if (!n->ded) {
        n->page_left = PAGE_BLOCKS;
    }
    net_event(n, "SMS from %s queued%s", from,
              n->mi[0] ? "" : " (the MS has not registered yet)");
}

void gsm_net_mt_call(GsmNet *n, const char *from)
{
    g_free(n->mt_call);
    n->mt_call = g_strdup(from);
    if (!n->ded) {
        n->page_left = PAGE_BLOCKS;
    }
    net_event(n, "call from %s", from);
}

void gsm_net_hangup(GsmNet *n)
{
    g_free(n->mt_call);
    n->mt_call = NULL;
    n->hangup = n->cc != CC_IDLE;
}

const char *gsm_net_call_state(GsmNet *n)
{
    return cc_names[n->cc];
}

char *gsm_net_events(GsmNet *n)
{
    GString *g = g_string_new(NULL);

    for (GList *e = n->events.head; e; e = e->next) {
        g_string_append_printf(g, "%s\n", (char *)e->data);
    }
    return g_string_free(g, false);
}

void gsm_net_pdch_dl(GsmNet *n, uint32_t bfn, uint8_t b[GSM_BLOCK])
{
    gprs_pdch_dl(n->gprs, bfn, b);
}

void gsm_net_from_host(GsmNet *n, const uint8_t *ip, size_t len)
{
    gprs_from_host(n->gprs, ip, len);
}

uint32_t gsm_net_ms_ip(GsmNet *n)
{
    return gprs_ms_ip(n->gprs);
}

void gsm_net_set_trace(GsmNet *n, bool on)
{
    n->trace = on;
    gprs_set_trace(n->gprs, on);
}

void gsm_net_reset(GsmNet *n)
{
    net_free_channel(n);
    gprs_reset(n->gprs);
    n->ia_bfn = n->page_left = 0;
}

GsmNet *gsm_net_new(void (*to_host)(void *opaque, const uint8_t *ip,
                                    size_t len), void *opaque)
{
    GsmNet *n = g_new0(GsmNet, 1);

    n->gprs = gprs_new(to_host, opaque);
    n->l2[0].sapi = 0;
    n->l2[1].sapi = 3;
    g_queue_init(&n->mt_sms);
    g_queue_init(&n->events);
    return n;
}
