/*
 * The GPRS side of the modelled GSM network (gsm_net.c)
 *
 * One PDCH on timeslot GSM_PDCH_TN carries the MS's TBFs (44.060):
 *   - uplink: one phase access from a packet RACH (gsm_net.c assigns it on
 *     the CCCH), dynamic allocation with USF GSM_USF, acknowledged mode;
 *     the network resolves contention and acknowledges with PACKET UPLINK
 *     ACK/NACK.  The TBF is extended (SI13's NW_EXT_UTBF): it outlives the
 *     MS's countdown, the MS sending dummy blocks until it has data again,
 *     until UL_HOLD blocks without data; then the final ACK/NACK, polled
 *     for a PACKET CONTROL ACK, ends it
 *   - downlink: assigned on the CCCH (IMMEDIATE ASSIGNMENT with a packet
 *     downlink assignment, by TLLI), unacknowledged mode, CS-1, the last
 *     block polled
 * Above it the SGSN and GGSN: LLC UI frames (44.064, no ciphering), GMM
 * (attach, detach and routing area updates are accepted), SM (one PDP
 * context, the MS getting GPRS_MS_IP and DNS GPRS_DNS_IP through IPCP in
 * the PCO), SNDCP unit data (44.065, no compression); the MS's IP packets
 * go to the host (gsm_net's owner, a NIC).
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "gsm_gprs.h"

#define GPRS_MS_IP      0x0A00030F      /* 10.0.3.15, as -nic user gives */
#define GPRS_DNS_IP     0x0A000303
#define GPRS_DNS2_IP    0x0A000303
#define GPRS_DL_TFI     0
/* RLC/MAC data blocks by coding scheme: octets, and of them data */
static const uint8_t cs_len[4] = { 23, 33, 39, 53 };
static const uint8_t cs_data[4] = { 20, 30, 36, 50 };
#define LLC_MAX         1560
#define N201_U          500             /* LLC information, SAPI 3 */
#define DL_QUEUE_MAX    128
#define CCCH_ASSIGN     200             /* CCCH blocks a downlink IA is sent */

/*
 * the downlink TBF: assigned on the CCCH (the MS in packet idle) or on the
 * PACCH (it has an uplink TBF), kept for DL_HOLD blocks without data (LLC
 * dummy frames every DL_KEEP blocks hold the MS's T3190 off), polled every
 * DL_POLL blocks so that the MS can ask for an uplink TBF
 */
enum { DL_IDLE, DL_ASSIGNING, DL_PACCH, DL_ACTIVE, DL_FINAL };
#define DL_HOLD         160             /* ~3 s */
#define DL_KEEP         8
#define DL_POLL         8
#define UL_HOLD         100             /* ~2 s */

struct Gprs {
    bool trace;
    int cs;                             /* data blocks' coding scheme */
    uint32_t fn;                        /* the PDCH's last block */
    GprsToHost *to_host;
    void *opaque;
    uint32_t tlli;
    /* the uplink TBF */
    bool ul_active, ul_cr_done, ul_final, ul_puan, ul_poll_sent;
    /*
     * the receive window (44.060 9.1.5): V(Q) the first BSN not received
     * yet, V(R) one past the highest received; ul_rx for the 64 BSNs
     * before V(R)
     */
    uint8_t ul_vq, ul_vr;
    bool ul_rx[128];
    uint8_t ul_blk[128][GSM_PDCH_BLOCK];
    uint8_t ul_cs[128];
    bool ul_realloc;                    /* the MS asked for resources */
    unsigned ul_idle, ul_since_puan;
    unsigned ul_quiet;                  /* blocks since the last data */
    uint8_t ul_pdu[LLC_MAX];
    unsigned ul_pdu_len;
    /* the downlink TBF */
    int dl;
    GQueue dl_q;                        /* LLC frames (GByteArray) */
    unsigned dl_off;                    /* in the head frame */
    uint8_t dl_bsn;
    unsigned dl_wait, dl_idle, dl_since_poll;
    bool dl_ia_sent;
    unsigned dl_polls;                  /* polls not answered */
    bool ul_wanted;                     /* the MS asked for an uplink TBF */
    /* LLC: V(U) to send, by SAPI */
    uint16_t vu[16];
    /* SM, SNDCP */
    bool pdp;
    uint8_t nsapi, llc_sapi;
    uint16_t npdu;
    uint8_t sn_buf[LLC_MAX];
    unsigned sn_len;
};

static void G_GNUC_PRINTF(1, 2) gprs_event(const char *fmt, ...)
{
    va_list ap;
    g_autofree char *s = NULL;

    va_start(ap, fmt);
    s = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    qemu_log_mask(LOG_UNIMP, "gsm-gprs: %s\n", s);
}

static void G_GNUC_PRINTF(3, 4) gtrace(Gprs *g, uint32_t fn,
                                       const char *fmt, ...)
{
    va_list ap;
    g_autofree char *s = NULL;

    if (!g->trace) {
        return;
    }
    va_start(ap, fmt);
    s = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    qemu_log("gsm-gprs %u: %s\n", fn, s);
}

/* ---- CSN.1 bits ---- */

typedef struct Bits {
    uint8_t *b;
    unsigned n, max;
} Bits;

static void put(Bits *w, unsigned v, int len)
{
    while (len--) {
        if (w->n < 8 * w->max) {
            uint8_t m = 0x80 >> (w->n % 8);

            w->b[w->n / 8] = v >> len & 1 ? w->b[w->n / 8] | m :
                                            w->b[w->n / 8] & ~m;
            w->n++;
        }
    }
}

/* L/H: the padding's bit at this position (0x2B repeated), or its inverse */
static void put_lh(Bits *w, bool h)
{
    put(w, (0x2b >> (7 - w->n % 8) & 1) ^ h, 1);
}

static void pad(Bits *w)
{
    while (w->n < 8 * w->max) {
        put_lh(w, false);
    }
}

/* ---- LLC (44.064) ---- */

static uint32_t crc24_tab[256];

static uint32_t crc24(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffff;

    while (n--) {
        c = (c >> 8) ^ crc24_tab[(c ^ *p++) & 0xff];
    }
    return ~c & 0xffffff;
}

static void crc24_init(void)
{
    /* D^24 + D^23 + D^21 + D^20 + D^19 + D^17 + D^16 + D^15 + D^13 + D^8
     * + D^7 + D^5 + D^4 + D^2 + 1, bits reflected */
    uint32_t poly = 0, normal = 0xbba1b5;

    for (int i = 0; i < 24; i++) {
        if (normal & 1u << i) {
            poly |= 1u << (23 - i);
        }
    }
    for (int i = 0; i < 256; i++) {
        uint32_t c = i;

        for (int k = 0; k < 8; k++) {
            c = c & 1 ? (c >> 1) ^ poly : c >> 1;
        }
        crc24_tab[i] = c;
    }
}

/* data for the MS: a downlink TBF, on the PACCH if it is on the PDCH */
static void dl_wanted(Gprs *g)
{
    if (g->dl == DL_IDLE) {
        g->dl = g->ul_active ? DL_PACCH : DL_ASSIGNING;
        g->dl_wait = 0;
        g->dl_ia_sent = false;
    }
}

/* an LLC frame to the MS: UI, protected mode, on sapi */
static void llc_send(Gprs *g, unsigned sapi, const uint8_t *info, size_t n)
{
    GByteArray *f;
    uint16_t nu = g->vu[sapi & 15]++ & 511;
    uint8_t h[3] = { 0x40 | sapi, 0xc0 | (nu >> 6 & 7), (nu & 63) << 2 | 1 };
    uint32_t fcs;
    uint8_t t[3];

    if (g_queue_get_length(&g->dl_q) >= DL_QUEUE_MAX) {
        return;
    }
    f = g_byte_array_sized_new(n + 6);
    g_byte_array_append(f, h, 3);
    g_byte_array_append(f, info, n);
    fcs = crc24(f->data, f->len);
    t[0] = fcs;
    t[1] = fcs >> 8;
    t[2] = fcs >> 16;
    g_byte_array_append(f, t, 3);
    g_queue_push_tail(&g->dl_q, f);
    dl_wanted(g);
}

static void gmm_sm(Gprs *g, const uint8_t *m, size_t n);
static void sndcp_ul(Gprs *g, const uint8_t *p, size_t n);

static void llc_ul(Gprs *g, const uint8_t *f, size_t n)
{
    unsigned sapi;
    uint32_t fcs;

    if (n < 6) {
        return;
    }
    fcs = f[n - 3] | f[n - 2] << 8 | f[n - 1] << 16;
    if (crc24(f, n - 3) != fcs) {
        gprs_event("LLC: bad FCS %06x (computed %06x), %zu octets", fcs,
                   crc24(f, n - 3), n);
        return;
    }
    sapi = f[0] & 15;
    if ((f[1] & 0xe0) == 0xc0) {                /* UI */
        const uint8_t *info = f + 3;
        size_t len = n - 6;

        if (g->trace) {
            qemu_log("gsm-gprs: LLC UI SAPI %u, %zu octets\n", sapi, len);
        }
        if (sapi == 1) {
            gmm_sm(g, info, len);
        } else {
            sndcp_ul(g, info, len);
        }
    } else if ((f[1] & 0xef) == 0xeb && !(f[0] & 0x40)) {
        /*
         * an XID command (C/R 0 from the MS): the response (C/R 0 from
         * the network) accepts the values offered
         */
        GByteArray *r = g_byte_array_sized_new(n);
        uint32_t c;
        uint8_t t[3];

        gprs_event("LLC: XID on SAPI %u", sapi);
        g_byte_array_append(r, f, n - 3);
        r->data[0] = sapi;
        c = crc24(r->data, r->len);
        t[0] = c;
        t[1] = c >> 8;
        t[2] = c >> 16;
        g_byte_array_append(r, t, 3);
        g_queue_push_tail(&g->dl_q, r);
        dl_wanted(g);
    }
}

/* ---- GMM and SM (24.008) ---- */

static const uint8_t rai[6] = { 0x13, 0x00, 0x62, 0x00, 0x01, GSM_RAC };

static void sm_activate(Gprs *g, const uint8_t *m, size_t n)
{
    uint8_t r[256];
    size_t k = 0, i;
    unsigned qlen, alen, ipcp_id = 0;
    bool want_ipcp = false;

    if (n < 6) {
        return;
    }
    g->nsapi = m[2] & 15;
    g->llc_sapi = m[3] & 15;
    qlen = m[4];
    if (5 + qlen >= n) {
        return;
    }
    i = 5 + qlen;
    alen = m[i];
    i += 1 + alen;
    /* optional: APN (0x28), PCO (0x27) */
    while (i + 1 < n) {
        unsigned iei = m[i], l = m[i + 1];

        if (iei == 0x27 && i + 2 + l <= n && l >= 1) {
            for (size_t p = i + 3; p + 3 <= i + 2 + l;) {
                unsigned proto = m[p] << 8 | m[p + 1], cl = m[p + 2];

                if (proto == 0x8021 && cl >= 4 && p + 3 + cl <= i + 2 + l) {
                    want_ipcp = true;
                    ipcp_id = m[p + 4];
                }
                p += 3 + cl;
            }
        }
        i += 2 + l;
    }
    r[k++] = ((m[0] ^ 0x80) & 0xf0) | 0x0a;
    r[k++] = 0x42;                              /* ACCEPT */
    r[k++] = g->llc_sapi;
    r[k++] = qlen;
    memcpy(r + k, m + 5, qlen);
    if (qlen >= 3) {
        /* the subscribed (0) values: best effort, reliability class 3 */
        if (!(r[k] & 0x3f)) {
            r[k] = 0x23;
        }
        if (!r[k + 1]) {
            r[k + 1] = 0x92;
        }
        if (!(r[k + 2] & 0x1f)) {
            r[k + 2] |= 0x1f;
        }
    }
    k += qlen;
    r[k++] = 0x04;                              /* radio priority 4 */
    r[k++] = 0x2b;                              /* PDP address: IPv4 */
    r[k++] = 6;
    r[k++] = 0x01;
    r[k++] = 0x21;
    stl_be_p(r + k, GPRS_MS_IP);
    k += 4;
    if (want_ipcp) {
        /* PCO: IPCP Configure-Nak with the DNS servers */
        static const uint8_t opt_len = 2 * 6;

        r[k++] = 0x27;
        r[k++] = 1 + 3 + 4 + opt_len;
        r[k++] = 0x80;
        r[k++] = 0x80;
        r[k++] = 0x21;
        r[k++] = 4 + opt_len;
        r[k++] = 3;
        r[k++] = ipcp_id;
        r[k++] = 0;
        r[k++] = 4 + opt_len;
        r[k++] = 0x81;
        r[k++] = 6;
        stl_be_p(r + k, GPRS_DNS_IP);
        k += 4;
        r[k++] = 0x83;
        r[k++] = 6;
        stl_be_p(r + k, GPRS_DNS2_IP);
        k += 4;
    }
    g->pdp = true;
    g->npdu = 0;
    g->sn_len = 0;
    gprs_event("PDP context: NSAPI %u, LLC SAPI %u, IPCP %d", g->nsapi,
               g->llc_sapi, want_ipcp);
    llc_send(g, 1, r, k);
}

static void gmm_sm(Gprs *g, const uint8_t *m, size_t n)
{
    unsigned pd = m[0] & 15, mt = n > 1 ? m[1] : 0;
    uint8_t r[32];
    size_t k = 0;

    if (n < 2) {
        return;
    }
    if (pd == 8) {
        switch (mt) {
        case 0x01:                              /* ATTACH REQUEST */
            r[k++] = 0x08;
            r[k++] = 0x02;                      /* ATTACH ACCEPT */
            r[k++] = 0x01;                      /* GPRS only attached */
            r[k++] = 0x49;                      /* periodic RAU: 54 min */
            r[k++] = 0x04;                      /* radio priority for SMS */
            memcpy(r + k, rai, 6);
            k += 6;
            gprs_event("GMM: attach accepted (TLLI %08x)", g->tlli);
            break;
        case 0x05:                              /* DETACH REQUEST */
            r[k++] = 0x08;
            r[k++] = 0x06;                      /* DETACH ACCEPT */
            r[k++] = 0x00;
            g->pdp = false;
            gprs_event("GMM: detach");
            break;
        case 0x08:                              /* ROUTING AREA UPDATE */
            r[k++] = 0x08;
            r[k++] = 0x09;                      /* RAU ACCEPT */
            r[k++] = 0x00;                      /* RA updated */
            r[k++] = 0x49;
            memcpy(r + k, rai, 6);
            k += 6;
            gprs_event("GMM: routing area update accepted");
            break;
        default:
            gprs_event("GMM: message %02x", mt);
            return;
        }
        llc_send(g, 1, r, k);
    } else if (pd == 10) {
        switch ((mt & 0x3f) | 0x40) {
        case 0x41:                              /* ACTIVATE PDP CONTEXT REQ */
            sm_activate(g, m, n);
            break;
        case 0x46:                              /* DEACTIVATE PDP CONTEXT */
            r[k++] = ((m[0] ^ 0x80) & 0xf0) | 0x0a;
            r[k++] = 0x47;
            g->pdp = false;
            gprs_event("PDP context deactivated");
            llc_send(g, 1, r, k);
            break;
        default:
            gprs_event("SM: message %02x", mt);
        }
    }
}

/* ---- SNDCP (44.065), unit data ---- */

static void sndcp_ul(Gprs *g, const uint8_t *p, size_t n)
{
    size_t h;
    bool first, more;

    if (n < 3 || !g->pdp) {
        return;
    }
    first = p[0] & 0x40;
    more = p[0] & 0x10;
    if (p[0] & 0x20) {                          /* SN-UNITDATA */
        h = first ? 4 : 3;
    } else {                                    /* SN-DATA */
        h = first ? 3 : 1;
    }
    if (n < h) {
        return;
    }
    if (first) {
        g->sn_len = 0;
    }
    if (g->sn_len + n - h <= sizeof(g->sn_buf)) {
        memcpy(g->sn_buf + g->sn_len, p + h, n - h);
        g->sn_len += n - h;
    }
    if (!more && g->sn_len) {
        gtrace(g, g->fn, "to host: %u octets", g->sn_len);
        g->to_host(g->opaque, g->sn_buf, g->sn_len);
        g->sn_len = 0;
    }
}

void gprs_from_host(Gprs *g, const uint8_t *ip, size_t len)
{
    size_t off = 0;
    unsigned seg = 0;
    uint16_t npdu = g->npdu++ & 0xfff;

    if (!g->pdp) {
        return;
    }
    gtrace(g, g->fn, "from host: %zu octets", len);
    while (off < len) {
        uint8_t f[N201_U];
        size_t h = 0, chunk;
        bool first = off == 0;

        f[h++] = (first ? 0x40 : 0) | 0x20 | g->nsapi;
        if (first) {
            f[h++] = 0x00;                      /* no compression */
        }
        f[h++] = seg << 4 | npdu >> 8;
        f[h++] = npdu;
        chunk = MIN(len - off, sizeof(f) - h);
        if (off + chunk < len) {
            f[0] |= 0x10;                       /* more */
        }
        memcpy(f + h, ip + off, chunk);
        llc_send(g, g->llc_sapi, f, h + chunk);
        off += chunk;
        seg++;
    }
}

/* ---- RLC/MAC ---- */

/* the uplink blocks have all come */
static bool ul_complete(Gprs *g)
{
    return g->ul_vq == g->ul_vr;
}

/* PACKET UPLINK ACK/NACK; final: all received, and poll */
static void puan(Gprs *g, uint8_t b[GSM_BLOCK], bool final)
{
    Bits w = { b + 1, 0, GSM_BLOCK - 1 };

    b[0] = 1 << 6 | (final ? 1 << 3 : 0) | GSM_USF;   /* RRBP 0, S/P */
    put(&w, 9, 6);                              /* PACKET UPLINK ACK/NACK */
    put(&w, 0, 2);                              /* PAGE_MODE */
    put(&w, 0, 2);
    put(&w, GSM_UL_TFI, 5);
    put(&w, 0, 1);                              /* GPRS */
    put(&w, g->cs, 2);                          /* CHANNEL_CODING_COMMAND */
    put(&w, final, 1);                          /* FINAL_ACK_INDICATION */
    put(&w, g->ul_vr, 7);                       /* STARTING_SEQUENCE_NUMBER */
    for (int bn = 64; bn >= 1; bn--) {          /* RECEIVED_BLOCK_BITMAP */
        put(&w, g->ul_rx[(g->ul_vr - bn) & 127], 1);
    }
    if (!g->ul_cr_done) {
        put(&w, 1, 1);
        put(&w, g->tlli >> 16, 16);             /* CONTENTION_RESOLUTION_TLLI */
        put(&w, g->tlli & 0xffff, 16);
        g->ul_cr_done = true;
    } else {
        put(&w, 0, 1);
    }
    put(&w, 0, 1);                              /* no timing advance */
    put(&w, 0, 1);                              /* no power control */
    put(&w, 0, 1);                              /* no extension bits */
    put(&w, 0, 1);                              /* no fixed allocation */
    put(&w, 0, 1);                              /* no release 99 additions */
    pad(&w);
}

/*
 * the next downlink data block: the queue's next octets, or (dummy) an
 * LLC dummy frame (44.064 UI dummy command, its FCS invalid: the MS's LLC
 * drops it); final: the last block of the TBF; poll: for an ACK/NACK
 */
static void dl_data(Gprs *g, uint8_t b[GSM_PDCH_BLOCK], unsigned usf,
                    bool dummy, bool final, bool poll)
{
    static const uint8_t llc_dummy[] = { 0x43, 0xc0, 0x01 };
    size_t k = 3, room = cs_data[g->cs];

    memset(b, 0x2b, GSM_PDCH_BLOCK);
    if (dummy) {
        b[k++] = sizeof(llc_dummy) << 2 | 1;
        memcpy(b + k, llc_dummy, sizeof(llc_dummy));
    } else {
        GByteArray *f = g_queue_peek_head(&g->dl_q);
        size_t left = f->len - g->dl_off;

        if (left < room) {
            /* the frame ends here: a length indicator, the rest filler */
            b[k++] = left << 2 | 1;             /* LI, M 0, E 1 */
            room--;
        } else if (left == room) {
            /*
             * it would end with the block, which needs a length indicator
             * too (44.060 10.4.14): its last octet goes in the next one
             */
            room--;
        }
        memcpy(b + k, f->data + g->dl_off, MIN(left, room));
        g->dl_off += MIN(left, room);
        if (g->dl_off >= f->len) {
            g_byte_array_free(g_queue_pop_head(&g->dl_q), true);
            g->dl_off = 0;
        }
    }
    b[0] = (poll ? 1 << 3 : 0) | usf;           /* data; RRBP 0 */
    b[1] = GPRS_DL_TFI << 1 | final;
    b[2] = g->dl_bsn << 1 | (k == 3);           /* E: no length indicator */
    g->dl_bsn = (g->dl_bsn + 1) & 127;
    if (poll) {
        g->dl_since_poll = 0;
        /* the MS answers polls: if it stops, it has left the TBF */
        if (++g->dl_polls > 3 && g->dl == DL_ACTIVE) {
            gprs_event("downlink TBF: no answer to polls, reassigning");
            g->dl = DL_ASSIGNING;
            g->dl_ia_sent = false;
            g->dl_wait = 0;
        }
    }
}

/* the Packet Timing Advance IE: TA 0 */
static void put_ta(Bits *w)
{
    put(w, 1, 1);
    put(w, 0, 6);
    put(w, 0, 1);
}

/*
 * PACKET UPLINK ASSIGNMENT on the PACCH, to the MS of the downlink TBF or
 * (ul) of the uplink one (asking for resources: the same are reassigned)
 */
static void pua(Gprs *g, uint8_t b[GSM_BLOCK], unsigned usf, bool ul)
{
    Bits w = { b + 1, 0, GSM_BLOCK - 1 };

    b[0] = 1 << 6 | usf;
    put(&w, 10, 6);                             /* PACKET UPLINK ASSIGNMENT */
    put(&w, 0, 2);                              /* PAGE_MODE */
    put(&w, 0, 1);                              /* no persistence levels */
    put(&w, 0, 1);                              /* Global TFI: */
    put(&w, !ul, 1);
    put(&w, ul ? GSM_UL_TFI : GPRS_DL_TFI, 5);
    put(&w, 0, 1);                              /* GPRS */
    put(&w, g->cs, 2);                          /* CHANNEL_CODING_COMMAND */
    put(&w, 0, 1);                              /* TLLI block CS-1 */
    put_ta(&w);
    put(&w, 0, 1);                              /* no frequency parameters */
    put(&w, 1, 2);                              /* dynamic allocation: */
    put(&w, 0, 1);                              /* not extended */
    put(&w, 0, 1);                              /* no P0 */
    put(&w, 0, 1);                              /* USF granularity 1 block */
    put(&w, 1, 1);
    put(&w, GSM_UL_TFI, 5);                     /* UPLINK_TFI_ASSIGNMENT */
    put(&w, 0, 1);                              /* no blocks granted */
    put(&w, 0, 1);                              /* no starting time */
    put(&w, 0, 1);                              /* USFs, no power control: */
    for (int tn = 0; tn < 8; tn++) {
        put(&w, tn == GSM_PDCH_TN, 1);
        if (tn == GSM_PDCH_TN) {
            put(&w, GSM_USF, 3);
        }
    }
    put(&w, 0, 1);                              /* no release 99 additions */
    pad(&w);
}

/* PACKET DOWNLINK ASSIGNMENT to the MS of the uplink TBF, on the PACCH */
static void pda(Gprs *g, uint8_t b[GSM_BLOCK], unsigned usf)
{
    Bits w = { b + 1, 0, GSM_BLOCK - 1 };

    b[0] = 1 << 6 | usf;
    put(&w, 2, 6);                              /* PACKET DOWNLINK ASSIGNMENT */
    put(&w, 0, 2);
    put(&w, 0, 1);
    put(&w, 0, 1);                              /* Global TFI: */
    put(&w, 0, 1);
    put(&w, GSM_UL_TFI, 5);                     /* its uplink TFI */
    put(&w, 0, 1);                              /* GPRS */
    put(&w, 0, 2);                              /* MAC mode: dynamic */
    put(&w, 1, 1);                              /* RLC unacknowledged */
    put(&w, 0, 1);                              /* CONTROL_ACK */
    put(&w, 0x80 >> GSM_PDCH_TN, 8);            /* TIMESLOT_ALLOCATION */
    put_ta(&w);
    put(&w, 0, 1);                              /* no P0 */
    put(&w, 0, 1);                              /* no frequency parameters */
    put(&w, 1, 1);
    put(&w, GPRS_DL_TFI, 5);                    /* DOWNLINK_TFI_ASSIGNMENT */
    put(&w, 0, 1);                              /* no power control */
    put(&w, 0, 1);                              /* no starting time */
    put(&w, 0, 1);                              /* no measurement mapping */
    put(&w, 0, 1);                              /* no release 99 additions */
    pad(&w);
}

static void dl_start(Gprs *g)
{
    g->dl = DL_ACTIVE;
    g->dl_bsn = 0;
    g->dl_off = 0;
    g->dl_idle = g->dl_since_poll = g->dl_polls = 0;
}

int gprs_pdch_dl(Gprs *g, uint32_t fn, uint8_t b[GSM_PDCH_BLOCK])
{
    unsigned usf = g->ul_active ? GSM_USF : 0;
    Bits w = { b + 1, 0, GSM_BLOCK - 1 };

    memset(b, 0x2b, GSM_PDCH_BLOCK);
    g->fn = fn;
    /* the MS reads the PDCH after a CCCH assignment: the downlink TBF is on */
    if (g->dl == DL_ASSIGNING && g->dl_ia_sent && !g->ul_active) {
        dl_start(g);
        gtrace(g, fn, "DL TBF on (CCCH)");
    }
    /* an uplink TBF came first: assign the downlink one on the PACCH */
    if (g->dl == DL_ASSIGNING && g->ul_active) {
        g->dl = DL_PACCH;
    }
    if (g->ul_active && ++g->ul_idle > 60) {
        gprs_event("uplink TBF: the MS has gone");
        g->ul_active = false;
    }
    /* the extended uplink TBF has been idle long enough: end it */
    if (g->ul_active && !g->ul_final && ++g->ul_quiet >= UL_HOLD &&
        ul_complete(g)) {
        g->ul_final = g->ul_puan = true;
    }
    /* a downlink TBF for the MS of an uplink one, before that one ends */
    if (g->dl == DL_PACCH && g->ul_active && !g->ul_poll_sent) {
        pda(g, b, usf);
        dl_start(g);
        gtrace(g, fn, "PDA");
        return 0;
    }
    if (g->ul_active && g->ul_puan) {
        puan(g, b, g->ul_final);
        gtrace(g, fn, "PUAN V(Q) %u V(R) %u%s", g->ul_vq, g->ul_vr,
               g->ul_final ? " final" : "");
        g->ul_puan = false;
        g->ul_since_puan = 0;
        if (g->ul_final) {
            g->ul_poll_sent = true;
        }
        return 0;
    }
    if (g->ul_active && g->ul_realloc) {
        pua(g, b, usf, true);
        gtrace(g, fn, "PUA (resources asked for)");
        g->ul_realloc = false;
        return 0;
    }
    if (g->ul_wanted && !g->ul_active && g->dl == DL_ACTIVE) {
        pua(g, b, usf, false);
        gtrace(g, fn, "PUA");
        g->ul_wanted = false;
        gprs_ul_assigned(g);
        g->ul_cr_done = true;                   /* the MS is known */
        return 0;
    }
    /* the uplink TBF is ending: assign on the CCCH once the MS is idle */
    if (g->dl == DL_PACCH && !g->ul_active) {
        g->dl = DL_ASSIGNING;
        g->dl_ia_sent = false;
        g->dl_wait = 0;
    }
    if (g->dl == DL_FINAL && ++g->dl_wait > 20) {
        g->dl = g_queue_is_empty(&g->dl_q) ? DL_IDLE : DL_ASSIGNING;
        g->dl_wait = 0;
        g->dl_ia_sent = false;
    }
    if (g->dl == DL_ACTIVE) {
        bool more = !g_queue_is_empty(&g->dl_q);

        if (more) {
            bool poll = ++g->dl_since_poll >= DL_POLL ||
                        (g_queue_get_length(&g->dl_q) == 1 &&
                         ((GByteArray *)g_queue_peek_head(&g->dl_q))->len -
                         g->dl_off <= cs_data[g->cs]);

            g->dl_idle = 0;
            gtrace(g, fn, "DL BSN %u%s, %u frames", g->dl_bsn,
                   poll ? " polled" : "", g_queue_get_length(&g->dl_q));
            dl_data(g, b, usf, false, false, poll);
            return g->cs;
        }
        if (++g->dl_idle >= DL_HOLD && !g->ul_active) {
            gtrace(g, fn, "DL release");
            dl_data(g, b, usf, true, true, true);  /* release */
            g->dl = DL_FINAL;
            g->dl_wait = 0;
            return g->cs;
        }
        if (g->dl_idle % DL_KEEP == 0) {
            gtrace(g, fn, "DL keepalive");
            dl_data(g, b, usf, true, false, true);
            return g->cs;
        }
    }
    /* PACKET DUMMY CONTROL BLOCK, carrying the USF */
    b[0] = 1 << 6 | usf;
    put(&w, 0x25, 6);
    put(&w, 0, 2);
    put(&w, 0, 1);
    pad(&w);
    return 0;
}

/* MSB-first bit field of an uplink control message (from its octet 1) */
static unsigned ul_bits(const uint8_t *b, unsigned at, unsigned len)
{
    unsigned v = 0;

    for (unsigned i = 0; i < len; i++) {
        unsigned bit = at + i;

        v = v << 1 | (b[1 + bit / 8] >> (7 - bit % 8) & 1);
    }
    return v;
}

/* an uplink data block, in BSN order: its LLC frames */
/* (n: its octets, by its coding scheme) */
static void ul_block(Gprs *g, const uint8_t *b, unsigned n)
{
    unsigned cv = b[0] >> 2 & 15, k = 3;
    bool ti = b[1] & 1, e = b[2] & 1;
    struct { unsigned len; bool m; } li[8];
    unsigned nli = 0;

    while (!e && k < n && nli < ARRAY_SIZE(li)) {
        li[nli].len = b[k] >> 2;
        li[nli].m = b[k] >> 1 & 1;
        e = b[k] & 1;
        /*
         * LI 0: no frame ends here (one that would have ended with the
         * block, its last octet going in the next one with an LI)
         */
        nli += li[nli].len != 0;
        k++;
    }
    if (ti && k + 4 <= n) {
        g->tlli = ldl_be_p(b + k);
        k += 4;
    }
    if (b[1] & 0x40) {                          /* PI: PFI */
        k++;
    }
    for (unsigned i = 0; i <= nli && k <= n; i++) {
        unsigned seg = i < nli ? li[i].len : n - k;

        seg = MIN(seg, n - k);
        if (g->ul_pdu_len + seg <= sizeof(g->ul_pdu)) {
            memcpy(g->ul_pdu + g->ul_pdu_len, b + k, seg);
            g->ul_pdu_len += seg;
        }
        k += seg;
        if (i < nli || cv == 0) {
            /* a length indicator ends the frame (so does the last block) */
            if (g->ul_pdu_len) {
                llc_ul(g, g->ul_pdu, g->ul_pdu_len);
            }
            g->ul_pdu_len = 0;
            if (i < nli && !li[i].m) {
                break;                          /* the rest is filler */
            }
        }
    }
    if (cv == 0) {
        g->ul_puan = true;                      /* (the TBF goes on) */
    }
}

/*
 * acknowledged mode: blocks are kept until those before them have come;
 * a repeat means the MS wants an acknowledgement, which it gets, as it
 * does every 4 new blocks (the bitmap nacks those missing)
 */
static void ul_data(Gprs *g, const uint8_t *b, int cs)
{
    unsigned bsn = b[2] >> 1;

    g->ul_quiet = 0;
    if (((bsn - g->ul_vq) & 127) >= 64 ||
        (((bsn - g->ul_vq) & 127) < ((g->ul_vr - g->ul_vq) & 127) &&
         g->ul_rx[bsn])) {
        g->ul_puan = true;                      /* a repeat */
        return;
    }
    if (((bsn - g->ul_vr) & 127) < 64) {        /* beyond V(R): advance it */
        while (g->ul_vr != ((bsn + 1) & 127)) {
            g->ul_rx[g->ul_vr] = false;
            g->ul_vr = (g->ul_vr + 1) & 127;
        }
    }
    g->ul_rx[bsn] = true;
    memcpy(g->ul_blk[bsn], b, cs_len[cs]);
    g->ul_cs[bsn] = cs;
    while (g->ul_vq != g->ul_vr && g->ul_rx[g->ul_vq]) {
        ul_block(g, g->ul_blk[g->ul_vq], cs_len[g->ul_cs[g->ul_vq]]);
        g->ul_vq = (g->ul_vq + 1) & 127;
    }
    if (!g->ul_cr_done || ++g->ul_since_puan >= 4) {
        g->ul_puan = true;
    }
}

void gprs_pdch_ul(Gprs *g, uint32_t fn, const uint8_t *b, int cs)
{
    g->ul_idle = 0;
    if (b[0] >> 6 == 0) {
        if (g->trace) {
            g_autofree char *hex = g_malloc0(2 * cs_len[cs & 3] + 1);

            for (unsigned i = 0; i < cs_len[cs & 3]; i++) {
                sprintf(hex + 2 * i, "%02x", b[i]);
            }
            gtrace(g, fn, "UL BSN %u CV %u TI %u CS-%d %s", b[2] >> 1,
                   b[0] >> 2 & 15, b[1] & 1, cs + 1, hex);
        }
        if (g->ul_active) {
            ul_data(g, b, cs & 3);
        }
        return;
    }
    if (b[0] >> 6 == 1) {
        unsigned type = b[1] >> 2;

        if (type == 2) {
            unsigned miss = 0, ssn = ul_bits(b, 6 + 5 + 1, 7);

            for (unsigned i = 0; i < 64; i++) {
                miss += !ul_bits(b, 6 + 5 + 8 + i, 1);
            }
            gtrace(g, fn, "PDAN SSN %u, %u of 64 missing%s", ssn, miss,
                   ul_bits(b, 6 + 5 + 72, 1) ? ", UL request" : "");
        } else if (type != 3) {                 /* (not a dummy) */
            gtrace(g, fn, "UL control %u", type);
        }
        if (type == 5 && g->ul_active) {        /* PACKET RESOURCE REQUEST */
            g->ul_realloc = true;
        }
        if (type == 1 && g->ul_poll_sent) {     /* PACKET CONTROL ACK */
            g->ul_active = false;
            gtrace(g, fn, "uplink TBF released");
        } else if (type == 2) {
            /*
             * PACKET DOWNLINK ACK/NACK: TFI (5), Ack/Nack Description (FAI,
             * SSN, 64 bits), then a Channel Request Description if the MS
             * has data to send
             */
            g->dl_polls = 0;
            if (ul_bits(b, 6 + 5 + 72, 1) && !g->ul_active) {
                g->ul_wanted = true;
            }
            if (g->dl == DL_FINAL) {
                g->dl = g_queue_is_empty(&g->dl_q) ? DL_IDLE : DL_ASSIGNING;
                g->dl_wait = 0;
                g->dl_ia_sent = false;
            }
        }
    }
}

void gprs_ul_assigned(Gprs *g)
{
    g->ul_active = true;
    g->ul_cr_done = g->ul_final = g->ul_puan = g->ul_poll_sent = false;
    g->ul_vq = g->ul_vr = 0;
    memset(g->ul_rx, 0, sizeof(g->ul_rx));
    g->ul_idle = g->ul_since_puan = g->ul_quiet = 0;
    g->ul_realloc = false;
    g->ul_pdu_len = 0;
}

/*
 * IMMEDIATE ASSIGNMENT of a downlink TBF (by TLLI) on the PDCH, while one
 * is wanted and the MS is not transferring (it reads the CCCH then)
 */
bool gprs_ccch_assignment(Gprs *g, uint32_t fn, uint8_t b[GSM_BLOCK])
{
    unsigned t1p = fn / 1326 % 32, t2 = fn % 26, t3 = fn % 51;
    Bits w;

    if (g->dl != DL_ASSIGNING || g->ul_active || !g->tlli) {
        return false;
    }
    if (++g->dl_wait > CCCH_ASSIGN) {
        gprs_event("downlink TBF: no answer, %u frames dropped",
                   g_queue_get_length(&g->dl_q));
        while (!g_queue_is_empty(&g->dl_q)) {
            g_byte_array_free(g_queue_pop_head(&g->dl_q), true);
        }
        g->dl = DL_IDLE;
        return false;
    }
    memset(b, 0x2b, GSM_BLOCK);
    b[0] = 0x2d;                                /* L2 pseudo length 11 */
    b[1] = 0x06;
    b[2] = 0x3f;
    b[3] = 0x30;                                /* TBF, downlink */
    b[4] = 0x01 << 3 | GSM_PDCH_TN;
    b[5] = GSM_TSC << 5 | (GSM_ARFCN >> 8 & 3);
    b[6] = GSM_ARFCN & 0xff;
    b[7] = 0x7f;                                /* (no request) */
    b[8] = t1p << 3 | t3 >> 3;
    b[9] = (t3 & 7) << 5 | t2;
    b[10] = 0;                                  /* timing advance */
    b[11] = 0;                                  /* mobile allocation */
    w = (Bits){ b + 12, 0, GSM_BLOCK - 12 };
    put_lh(&w, true);
    put_lh(&w, true);
    put(&w, 1, 2);                              /* Packet Downlink Assignment */
    put(&w, g->tlli >> 16, 16);
    put(&w, g->tlli & 0xffff, 16);
    put(&w, 1, 1);
    put(&w, GPRS_DL_TFI, 5);
    put(&w, 1, 1);                              /* RLC unacknowledged */
    put(&w, 0, 1);                              /* no ALPHA */
    put(&w, 0, 5);                              /* GAMMA */
    put(&w, 0, 1);                              /* no POLLING */
    put(&w, 1, 1);                              /* TA_VALID */
    put(&w, 0, 1);                              /* no TA index */
    put(&w, 0, 1);                              /* no starting time */
    put(&w, 0, 1);                              /* no P0 */
    put_lh(&w, false);                          /* no release 99 additions */
    pad(&w);
    g->dl_ia_sent = true;
    if (g->dl_wait == 1) {
        gprs_event("downlink TBF assigned to TLLI %08x (%u frames)", g->tlli,
                   g_queue_get_length(&g->dl_q));
    }
    return true;
}

void gprs_set_cs(Gprs *g, int cs)
{
    g->cs = cs & 3;
}

int gprs_cs(Gprs *g)
{
    return g->cs;
}

uint32_t gprs_ms_ip(Gprs *g)
{
    return g->pdp ? GPRS_MS_IP : 0;
}

void gprs_set_trace(Gprs *g, bool on)
{
    g->trace = on;
}

void gprs_reset(Gprs *g)
{
    while (!g_queue_is_empty(&g->dl_q)) {
        g_byte_array_free(g_queue_pop_head(&g->dl_q), true);
    }
    g->ul_active = false;
    g->dl = DL_IDLE;
    g->pdp = false;
    memset(g->vu, 0, sizeof(g->vu));
}

Gprs *gprs_new(GprsToHost *to_host, void *opaque)
{
    Gprs *g = g_new0(Gprs, 1);

    crc24_init();
    g->to_host = to_host;
    g->opaque = opaque;
    g->cs = 1;                                  /* CS-2 */
    g_queue_init(&g->dl_q);
    return g;
}
