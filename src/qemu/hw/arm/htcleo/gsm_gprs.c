/*
 * The GPRS side of the modelled GSM network (gsm_net.c)
 *
 * One PDCH on timeslot GSM_PDCH_TN carries the MS's TBFs (44.060):
 *   - uplink: one phase access from a packet RACH (gsm_net.c assigns it on
 *     the CCCH), dynamic allocation with USF GSM_USF, acknowledged mode;
 *     the network resolves contention and acknowledges with PACKET UPLINK
 *     ACK/NACK, the final one polled for a PACKET CONTROL ACK
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
#define RLC_DATA        20              /* CS-1 data octets */
#define LLC_MAX         1560
#define N201_U          500             /* LLC information, SAPI 3 */
#define DL_QUEUE_MAX    128
#define CCCH_ASSIGN     200             /* CCCH blocks a downlink IA is sent */

enum { DL_IDLE, DL_ASSIGNING, DL_ACTIVE, DL_FINAL };

struct Gprs {
    bool trace;
    GprsToHost *to_host;
    void *opaque;
    uint32_t tlli;
    /* the uplink TBF */
    bool ul_active, ul_cr_done, ul_final, ul_puan, ul_poll_sent;
    uint8_t ul_vr;                      /* V(R): the next BSN expected */
    bool ul_rx[128];
    unsigned ul_idle, ul_since_puan;
    uint8_t ul_pdu[LLC_MAX];
    unsigned ul_pdu_len;
    /* the downlink TBF */
    int dl;
    GQueue dl_q;                        /* LLC frames (GByteArray) */
    unsigned dl_off;                    /* in the head frame */
    uint8_t dl_bsn;
    unsigned dl_wait;
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
    if (g->dl == DL_IDLE) {
        g->dl = DL_ASSIGNING;
        g->dl_wait = 0;
    }
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
    } else if ((f[1] & 0xef) == 0xeb) {         /* XID: accept as offered */
        uint8_t x[LLC_MAX];
        size_t len = MIN(n - 5, sizeof(x) - 2);

        gprs_event("LLC: XID on SAPI %u", sapi);
        x[0] = 0x40 | sapi;
        x[1] = f[1];
        memcpy(x + 2, f + 2, len);
        {
            GByteArray *r = g_byte_array_sized_new(len + 5);
            uint32_t c;
            uint8_t t[3];

            g_byte_array_append(r, x, 2 + len);
            c = crc24(r->data, r->len);
            t[0] = c;
            t[1] = c >> 8;
            t[2] = c >> 16;
            g_byte_array_append(r, t, 3);
            g_queue_push_tail(&g->dl_q, r);
            if (g->dl == DL_IDLE) {
                g->dl = DL_ASSIGNING;
                g->dl_wait = 0;
            }
        }
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
    put(&w, 0, 2);                              /* CS-1 */
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

/* the next downlink data block of the downlink TBF */
static void dl_data(Gprs *g, uint8_t b[GSM_BLOCK], unsigned usf)
{
    GByteArray *f = g_queue_peek_head(&g->dl_q);
    size_t left = f->len - g->dl_off, k = 3, room = RLC_DATA;
    bool last_pdu, fbi = false;

    memset(b, 0x2b, GSM_BLOCK);
    if (left < room) {
        /* the frame ends here: a length indicator, the rest is filler */
        b[k++] = left << 2 | 1;                 /* LI, M 0, E 1 */
        room--;
    }
    memcpy(b + k, f->data + g->dl_off, MIN(left, room));
    g->dl_off += MIN(left, room);
    last_pdu = g->dl_off >= f->len;
    if (last_pdu) {
        g_byte_array_free(g_queue_pop_head(&g->dl_q), true);
        g->dl_off = 0;
        fbi = g_queue_is_empty(&g->dl_q);
    }
    b[0] = (fbi ? 1 << 3 : 0) | usf;            /* data; RRBP 0; poll last */
    b[1] = GPRS_DL_TFI << 1 | fbi;
    b[2] = g->dl_bsn << 1 | (k == 3);           /* E: no length indicator */
    g->dl_bsn = (g->dl_bsn + 1) & 127;
    if (fbi) {
        g->dl = DL_FINAL;
        g->dl_wait = 0;
    }
}

void gprs_pdch_dl(Gprs *g, uint32_t fn, uint8_t b[GSM_BLOCK])
{
    unsigned usf = g->ul_active ? GSM_USF : 0;
    Bits w = { b + 1, 0, GSM_BLOCK - 1 };

    memset(b, 0x2b, GSM_BLOCK);
    /* the MS reads the PDCH after its assignment: the downlink TBF is on */
    if (g->dl == DL_ASSIGNING && g->dl_wait && !g->ul_active) {
        g->dl = DL_ACTIVE;
    }
    if (g->ul_active && ++g->ul_idle > 60) {
        gprs_event("uplink TBF: the MS has gone");
        g->ul_active = false;
    }
    if (g->ul_active && g->ul_puan) {
        puan(g, b, g->ul_final);
        g->ul_puan = false;
        g->ul_since_puan = 0;
        if (g->ul_final) {
            g->ul_poll_sent = true;
        }
        return;
    }
    if (g->dl == DL_FINAL && ++g->dl_wait > 20) {
        g->dl = g_queue_is_empty(&g->dl_q) ? DL_IDLE : DL_ASSIGNING;
        g->dl_wait = 0;
    }
    if (g->dl == DL_ACTIVE && !g_queue_is_empty(&g->dl_q)) {
        dl_data(g, b, usf);
        return;
    }
    /* PACKET DUMMY CONTROL BLOCK, carrying the USF */
    b[0] = 1 << 6 | usf;
    put(&w, 0x25, 6);
    put(&w, 0, 2);
    put(&w, 0, 1);
    pad(&w);
}

static void ul_data(Gprs *g, const uint8_t *b)
{
    unsigned cv = b[0] >> 2 & 15, bsn = b[2] >> 1, k = 3;
    bool ti = b[1] & 1, e = b[2] & 1;
    struct { unsigned len; bool m; } li[8];
    unsigned nli = 0;

    while (!e && k < GSM_BLOCK && nli < ARRAY_SIZE(li)) {
        li[nli].len = b[k] >> 2;
        li[nli].m = b[k] >> 1 & 1;
        e = b[k] & 1;
        nli++;
        k++;
    }
    if (ti && k + 4 <= GSM_BLOCK) {
        uint32_t tlli = ldl_be_p(b + k);

        if (tlli != g->tlli) {
            g->tlli = tlli;
        }
        k += 4;
    }
    if (b[1] & 0x40) {                          /* PI: PFI */
        k++;
    }
    if (g->ul_rx[bsn] && bsn != g->ul_vr) {
        return;                                 /* a repeat */
    }
    g->ul_rx[bsn] = true;
    g->ul_rx[(bsn + 64) & 127] = false;
    if (bsn == g->ul_vr) {
        g->ul_vr = (g->ul_vr + 1) & 127;
    }
    /* reassemble LLC frames */
    for (unsigned i = 0; i <= nli && k <= GSM_BLOCK; i++) {
        unsigned seg = i < nli ? li[i].len : GSM_BLOCK - k;

        seg = MIN(seg, GSM_BLOCK - k);
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
        g->ul_final = true;
        g->ul_puan = true;
    } else if (!g->ul_cr_done || ++g->ul_since_puan >= 8) {
        g->ul_puan = true;
    }
}

void gprs_pdch_ul(Gprs *g, uint32_t fn, const uint8_t b[GSM_BLOCK])
{
    g->ul_idle = 0;
    if (b[0] >> 6 == 0) {
        if (g->trace) {
            qemu_log("gsm-gprs: UL data TFI %u BSN %u CV %u TI %u\n",
                     b[1] >> 1 & 31, b[2] >> 1, b[0] >> 2 & 15, b[1] & 1);
        }
        if (g->ul_active) {
            ul_data(g, b);
        }
        return;
    }
    if (b[0] >> 6 == 1) {
        unsigned type = b[1] >> 2;

        if (g->trace) {
            qemu_log("gsm-gprs: UL control, message type %u\n", type);
        }
        if (type == 1 && g->ul_poll_sent) {     /* PACKET CONTROL ACK */
            g->ul_active = false;
            gprs_event("uplink TBF released");
        } else if (type == 2 && g->dl == DL_FINAL) {
            /* PACKET DOWNLINK ACK/NACK to the last block */
            g->dl = g_queue_is_empty(&g->dl_q) ? DL_IDLE : DL_ASSIGNING;
            g->dl_wait = 0;
        }
    }
}

void gprs_ul_assigned(Gprs *g)
{
    g->ul_active = true;
    g->ul_cr_done = g->ul_final = g->ul_puan = g->ul_poll_sent = false;
    g->ul_vr = 0;
    memset(g->ul_rx, 0, sizeof(g->ul_rx));
    g->ul_idle = g->ul_since_puan = 0;
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
    g->dl_bsn = 0;
    g->dl_off = 0;
    if (g->dl_wait == 1) {
        gprs_event("downlink TBF assigned to TLLI %08x (%u frames)", g->tlli,
                   g_queue_get_length(&g->dl_q));
    }
    return true;
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
    g_queue_init(&g->dl_q);
    return g;
}
