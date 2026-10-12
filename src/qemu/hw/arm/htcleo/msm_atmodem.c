/*
 * HTC HD2 baseband: AT command channel, SIM and packet data
 *
 * The HTC RIL (libhtc_ril.so) and the cLK PPP wrapper drive the modem with
 * AT commands on SMD channel "DS" (/dev/smd0).  A data call is set up with
 * AT+CGDCONT / ATD*99***1#, after which pppd talks PPP on SMD "DATA1"
 * (/dev/smd1).  This file is the modem side of both:
 *
 *  - an AT interpreter with the SIM card of the AOSP emulator (T-Mobile US,
 *    310/260, IMSI 310260000000000, no PIN) registered on UMTS (3G), so the
 *    stock APN list ("epc.tmobile.com") applies without changes to the ROM;
 *  - a PPP peer (RFC 1661/1662: LCP, IPCP with address and DNS assignment);
 *  - the IP packets of the PPP link bridged to a QEMU network backend (an
 *    Ethernet NIC, model "msm-modem"), answering ARP for the phone.  With
 *
 *      -nic user,model=msm-modem,net=10.0.3.0/24
 *
 *    the phone gets 10.0.3.15 with gateway 10.0.3.2 and DNS 10.0.3.3.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/cutils.h"
#include "net/net.h"
#include "net/eth.h"
#include "qemu/bswap.h"
#include "htcleo.h"
#include "msm_modem.h"

#define ATLOG(fmt, ...) \
    qemu_log_mask(LOG_TRACE, "modem at: " fmt, ## __VA_ARGS__)

/* the SIM and the network */
#define SIM_IMSI        "310260000000000"
#define SIM_ICCID       "89014103211118510720"
#define SIM_MSISDN      "+15555215554"
#define DEV_IMEI        "000000000000000"
#define NET_NAME_LONG   "Android"
#define NET_NAME_SHORT  "Android"
#define NET_PLMN        "310260"
#define NET_LAC         "2A3B"
#define NET_CI          "0001C4D5"
#define ACT_UTRAN       2

/* PPP */
#define PPP_LCP         0xC021
#define PPP_IPCP        0x8021
#define PPP_IP          0x0021
#define CONF_REQ        1
#define CONF_ACK        2
#define CONF_NAK        3
#define CONF_REJ        4
#define TERM_REQ        5
#define TERM_ACK        6
#define PROTO_REJ       8
#define ECHO_REQ        9
#define ECHO_REP        10
#define PPP_MAX         2048

enum { RC_OK = 0, RC_CONNECT = 1, RC_NO_CARRIER = 3, RC_ERROR = 4 };

typedef struct MsmAtModem {
    GByteArray *line;
    bool echo, verbose;
    int creg_n, cgreg_n, cops_fmt, cfun, cmee;
    bool pdp_active;
    bool sim_announced;
    char apn[64];
    QEMUTimer *reg_timer;

    /* PPP on DATA1 */
    GByteArray *rx;
    bool rx_esc;
    bool online;                /* a data call is up */
    uint8_t id;
    uint32_t magic;
    bool lcp_ours_acked, lcp_theirs_acked;
    bool ipcp_ours_acked, ipcp_theirs_acked;
    uint32_t phone_ip, gw_ip, dns_ip;
} MsmAtModem;

/* ------------------------------------------------------------------------ */
/* AT output                                                                 */
/* ------------------------------------------------------------------------ */

static void at_send(MsmModemState *s, const char *str)
{
    msm_modem_chan_send(s, "DS", str, strlen(str));
}

/* an information line (or an unsolicited result) */
static void at_info(MsmModemState *s, GString *out, const char *text)
{
    if (s->at->verbose) {
        g_string_append_printf(out, "\r\n%s\r\n", text);
    } else {
        g_string_append_printf(out, "%s\r\n", text);
    }
}

static void at_final(MsmModemState *s, GString *out, int rc)
{
    static const char *const v[] = {
        [RC_OK] = "OK", [RC_CONNECT] = "CONNECT",
        [RC_NO_CARRIER] = "NO CARRIER", [RC_ERROR] = "ERROR",
    };

    if (s->at->verbose) {
        g_string_append_printf(out, "\r\n%s\r\n", v[rc]);
    } else {
        g_string_append_printf(out, "%d\r", rc);
    }
}

static void at_unsolicited(MsmModemState *s, const char *text)
{
    g_autoptr(GString) out = g_string_new(NULL);

    at_info(s, out, text);
    ATLOG("<- %s\n", text);
    at_send(s, out->str);
}

/* ------------------------------------------------------------------------ */
/* Network registration                                                      */
/* ------------------------------------------------------------------------ */

static int reg_stat(MsmModemState *s)
{
    return s->at->cfun == 1 ? 1 : 0;        /* registered, home network */
}

static void fmt_reg(MsmModemState *s, GString *out, const char *cmd, int n,
                    bool unsolicited)
{
    g_autofree char *t = NULL;
    int stat = reg_stat(s);

    if (unsolicited) {
        t = n >= 2 && stat ?
            g_strdup_printf("%s: %d,\"%s\",\"%s\",%d", cmd, stat, NET_LAC,
                            NET_CI, ACT_UTRAN) :
            g_strdup_printf("%s: %d", cmd, stat);
    } else {
        t = n >= 2 && stat ?
            g_strdup_printf("%s: %d,%d,\"%s\",\"%s\",%d", cmd, n, stat,
                            NET_LAC, NET_CI, ACT_UTRAN) :
            g_strdup_printf("%s: %d,%d", cmd, n, stat);
    }
    at_info(s, out, t);
}

static void reg_timer_cb(void *opaque)
{
    MsmModemState *s = opaque;
    MsmAtModem *a = s->at;
    g_autoptr(GString) out = g_string_new(NULL);

    if (a->cfun == 1 && !a->sim_announced) {
        /* the SIM came up with the radio */
        a->sim_announced = true;
        at_unsolicited(s, "+CPIN: READY");
    }
    if (a->creg_n) {
        fmt_reg(s, out, "+CREG", a->creg_n, true);
    }
    if (a->cgreg_n) {
        fmt_reg(s, out, "+CGREG", a->cgreg_n, true);
    }
    if (out->len) {
        ATLOG("<- registration %d\n", reg_stat(s));
        at_send(s, out->str);
    }
}

/* ------------------------------------------------------------------------ */
/* SIM files (AT+CRSM)                                                       */
/* ------------------------------------------------------------------------ */

static void bcd_swap(const char *digits, uint8_t *out, int n)
{
    int len = strlen(digits);

    for (int i = 0; i < n; i++) {
        int lo = 2 * i < len ? digits[2 * i] - '0' : 0xF;
        int hi = 2 * i + 1 < len ? digits[2 * i + 1] - '0' : 0xF;

        out[i] = (hi << 4) | lo;
    }
}

/* returns the file contents, or NULL if the SIM has no such file */
static GByteArray *sim_file(int fileid)
{
    GByteArray *f = g_byte_array_new();
    uint8_t b[32];

    switch (fileid) {
    case 0x2FE2:                            /* EF_ICCID */
        bcd_swap(SIM_ICCID, b, 10);
        g_byte_array_append(f, b, 10);
        break;
    case 0x6FAD:                            /* EF_AD: normal, MNC length 3 */
        g_byte_array_append(f, (const uint8_t *)"\x00\x00\x00\x03", 4);
        break;
    case 0x6F46: {                          /* EF_SPN */
        memset(b, 0xFF, 17);
        b[0] = 0x01;                        /* display condition */
        memcpy(b + 1, NET_NAME_LONG, strlen(NET_NAME_LONG));
        g_byte_array_append(f, b, 17);
        break;
    }
    case 0x6F40: {                          /* EF_MSISDN (one record) */
        const char *num = SIM_MSISDN + 1;
        memset(b, 0xFF, 28);
        b[14] = 1 + (strlen(num) + 1) / 2;  /* BCD length */
        b[15] = 0x91;                       /* international */
        bcd_swap(num, b + 16, 10);
        g_byte_array_append(f, b, 28);
        break;
    }
    case 0x6F07: {                          /* EF_IMSI */
        uint8_t imsi[9];
        char tmp[17];
        snprintf(tmp, sizeof(tmp), "9%s", SIM_IMSI);    /* parity nibble */
        imsi[0] = 8;
        bcd_swap(tmp, imsi + 1, 8);
        g_byte_array_append(f, imsi, 9);
        break;
    }
    default:
        g_byte_array_unref(f);
        return NULL;
    }
    return f;
}

static int at_crsm(MsmModemState *s, GString *out, const char *args)
{
    int cmd = 0, fileid = 0, p1 = 0, p2 = 0, p3 = 0;
    g_autoptr(GByteArray) f = NULL;
    g_autoptr(GString) hex = g_string_new(NULL);
    g_autofree char *line = NULL;

    sscanf(args, "%d,%d,%d,%d,%d", &cmd, &fileid, &p1, &p2, &p3);
    f = sim_file(fileid);
    if (!f) {
        at_info(s, out, "+CRSM: 106,130");          /* 6A82: not found */
        return RC_OK;
    }
    switch (cmd) {
    case 192: {                                     /* GET RESPONSE */
        bool record = fileid == 0x6F40;
        uint8_t r[15] = {
            0, 0, f->len >> 8, f->len & 0xFF, fileid >> 8, fileid & 0xFF,
            0x04, 0, 0, 0, 0, 0x01, 0x02, record ? 0x01 : 0x00,
            record ? f->len : 0,
        };
        for (int i = 0; i < 15; i++) {
            g_string_append_printf(hex, "%02X", r[i]);
        }
        break;
    }
    case 176:                                       /* READ BINARY */
    case 178:                                       /* READ RECORD */
        for (guint i = 0; i < f->len; i++) {
            g_string_append_printf(hex, "%02X", f->data[i]);
        }
        break;
    default:
        at_info(s, out, "+CRSM: 105,130");
        return RC_OK;
    }
    line = g_strdup_printf("+CRSM: 144,0,\"%s\"", hex->str);
    at_info(s, out, line);
    return RC_OK;
}

/* ------------------------------------------------------------------------ */
/* Extended commands                                                         */
/* ------------------------------------------------------------------------ */

static int at_dial(MsmModemState *s, GString *out, const char *num);

static int at_ext(MsmModemState *s, GString *out, const char *cmd)
{
    MsmAtModem *a = s->at;
    char name[32];
    const char *rest;
    bool query = false, test = false, set = false;
    int n = 0;
    g_autofree char *t = NULL;

    for (n = 0; cmd[n] && cmd[n] != '=' && cmd[n] != '?' && n < 31; n++) {
        name[n] = g_ascii_toupper(cmd[n]);
    }
    name[n] = 0;
    rest = cmd + n;
    if (!strcmp(rest, "?")) {
        query = true;
    } else if (!strcmp(rest, "=?")) {
        test = true;
    } else if (*rest == '=') {
        set = true;
        rest++;
    }

    if (!strcmp(name, "+CPIN")) {
        if (query) {
            at_info(s, out, "+CPIN: READY");
        }
        return RC_OK;
    }
    if (!strcmp(name, "+SIMTYPE")) {
        at_info(s, out, "+SIMTYPE: 1");             /* USIM */
        return RC_OK;
    }
    if (!strcmp(name, "+CPBS")) {
        if (query) {
            at_info(s, out, "+CPBS: \"SM\",0,250");
        }
        return RC_OK;
    }
    if (!strcmp(name, "+CPMS")) {
        at_info(s, out, "+CPMS: 0,30,0,30,0,30");
        return RC_OK;
    }
    if (!strcmp(name, "+CIMI")) {
        at_info(s, out, SIM_IMSI);
        return RC_OK;
    }
    if (!strcmp(name, "+CGSN") || !strcmp(name, "+GSN")) {
        at_info(s, out, DEV_IMEI);
        return RC_OK;
    }
    if (!strcmp(name, "+CGMI") || !strcmp(name, "+GMI")) {
        at_info(s, out, "HTC");
        return RC_OK;
    }
    if (!strcmp(name, "+CGMM") || !strcmp(name, "+GMM")) {
        at_info(s, out, "HTC HD2 (QEMU)");
        return RC_OK;
    }
    if (!strcmp(name, "+CGMR") || !strcmp(name, "+GMR")) {
        at_info(s, out, "15.42.50.11U");
        return RC_OK;
    }
    if (!strcmp(name, "+CFUN")) {
        if (query) {
            t = g_strdup_printf("+CFUN: %d", a->cfun);
            at_info(s, out, t);
        } else if (set) {
            int v = atoi(rest);
            if (v != a->cfun) {
                a->cfun = v;
                timer_mod(a->reg_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                          (v == 1 ? 1500 : 10) * SCALE_MS);
            }
        }
        return RC_OK;
    }
    if (!strcmp(name, "+CREG") || !strcmp(name, "+CGREG")) {
        int *nn = name[2] == 'G' ? &a->cgreg_n : &a->creg_n;
        if (set) {
            *nn = atoi(rest);
        } else if (query) {
            fmt_reg(s, out, name, *nn, false);
        } else if (test) {
            t = g_strdup_printf("%s: (0-2)", name);
            at_info(s, out, t);
        }
        return RC_OK;
    }
    if (!strcmp(name, "+COPS")) {
        if (set) {
            int mode = 0, fmt = 0;
            if (sscanf(rest, "%d,%d", &mode, &fmt) == 2 && mode == 3) {
                a->cops_fmt = fmt;
            }
        } else if (query) {
            if (reg_stat(s)) {
                t = g_strdup_printf("+COPS: 0,%d,\"%s\",%d", a->cops_fmt,
                                    a->cops_fmt == 2 ? NET_PLMN :
                                    a->cops_fmt == 1 ? NET_NAME_SHORT :
                                    NET_NAME_LONG, ACT_UTRAN);
            } else {
                t = g_strdup("+COPS: 0");
            }
            at_info(s, out, t);
        } else if (test) {
            at_info(s, out, "+COPS: (2,\"" NET_NAME_LONG "\",\""
                    NET_NAME_SHORT "\",\"" NET_PLMN "\",2),,(0-4),(0-2)");
        }
        return RC_OK;
    }
    if (!strcmp(name, "+CSQ")) {
        at_info(s, out, a->cfun == 1 ? "+CSQ: 22,99" : "+CSQ: 99,99");
        return RC_OK;
    }
    if (!strcmp(name, "+CGDCONT")) {
        if (set && atoi(rest) == 1) {
            const char *q = strchr(rest, '"');
            q = q ? strchr(q + 1, '"') : NULL;     /* end of "IP" */
            q = q ? strchr(q + 1, '"') : NULL;     /* start of the APN */
            if (q) {
                const char *e = strchr(q + 1, '"');
                pstrcpy(a->apn, MIN(sizeof(a->apn),
                                    (size_t)(e ? e - q : 1)), q + 1);
            }
        } else if (query) {
            t = g_strdup_printf("+CGDCONT: 1,\"IP\",\"%s\",\"0.0.0.0\",0,0",
                                a->apn);
            at_info(s, out, t);
        }
        return RC_OK;
    }
    if (!strcmp(name, "+CGACT")) {
        if (set) {
            a->pdp_active = atoi(rest) == 1;
        } else if (query) {
            t = g_strdup_printf("+CGACT: 1,%d", a->pdp_active);
            at_info(s, out, t);
        }
        return RC_OK;
    }
    if (!strcmp(name, "+CGDATA") && set) {
        /* enter data state on the PDP context: same as ATD*99# */
        return at_dial(s, out, "*99#");
    }
    if (!strcmp(name, "+CGATT")) {
        if (query) {
            at_info(s, out, a->cfun == 1 ? "+CGATT: 1" : "+CGATT: 0");
        }
        return RC_OK;
    }
    if (!strcmp(name, "+CRSM")) {
        return at_crsm(s, out, rest);
    }
    if (!strcmp(name, "+CLCK")) {
        if (set && strchr(rest, ',') && atoi(strchr(rest, ',') + 1) == 2) {
            at_info(s, out, "+CLCK: 0");
        }
        return RC_OK;
    }
    if (!strcmp(name, "+CNUM")) {
        at_info(s, out, "+CNUM: ,\"" SIM_MSISDN "\",145");
        return RC_OK;
    }
    if (!strcmp(name, "+CSCA")) {
        if (query) {
            at_info(s, out, "+CSCA: \"+15555550000\",145");
        }
        return RC_OK;
    }
    if (!strcmp(name, "+CPAS")) {
        at_info(s, out, "+CPAS: 0");
        return RC_OK;
    }
    if (!strcmp(name, "+CMEE")) {
        if (set) {
            a->cmee = atoi(rest);
        }
        return RC_OK;
    }
    if (!strcmp(name, "+CLCC")) {
        return RC_OK;                       /* no calls */
    }

    qemu_log_mask(LOG_UNIMP, "modem at: unhandled AT%s\n", cmd);
    return RC_OK;
}

/* ------------------------------------------------------------------------ */
/* PPP                                                                       */
/* ------------------------------------------------------------------------ */

static uint16_t fcs16(const uint8_t *p, int len)
{
    uint16_t fcs = 0xFFFF;

    while (len--) {
        fcs ^= *p++;
        for (int i = 0; i < 8; i++) {
            fcs = (fcs & 1) ? (fcs >> 1) ^ 0x8408 : fcs >> 1;
        }
    }
    return fcs;
}

static void ppp_send(MsmModemState *s, uint16_t proto, const uint8_t *data,
                     int len)
{
    g_autoptr(GByteArray) f = g_byte_array_sized_new(len + 8);
    g_autoptr(GByteArray) o = g_byte_array_sized_new(2 * len + 16);
    uint8_t hdr[4] = { 0xFF, 0x03, proto >> 8, proto & 0xFF };
    uint16_t fcs;
    uint8_t flag = 0x7E, tail[2];

    g_byte_array_append(f, hdr, 4);
    g_byte_array_append(f, data, len);
    fcs = ~fcs16(f->data, f->len);
    tail[0] = fcs & 0xFF;
    tail[1] = fcs >> 8;
    g_byte_array_append(f, tail, 2);

    g_byte_array_append(o, &flag, 1);
    for (guint i = 0; i < f->len; i++) {
        uint8_t c = f->data[i];
        if (c < 0x20 || c == 0x7E || c == 0x7D) {
            uint8_t esc[2] = { 0x7D, c ^ 0x20 };
            g_byte_array_append(o, esc, 2);
        } else {
            g_byte_array_append(o, &c, 1);
        }
    }
    g_byte_array_append(o, &flag, 1);
    msm_modem_chan_send(s, "DATA1", o->data, o->len);
}

static void cp_send(MsmModemState *s, uint16_t proto, uint8_t code,
                    uint8_t id, const uint8_t *data, int len)
{
    uint8_t buf[PPP_MAX];

    len = MIN(len, PPP_MAX - 4);
    buf[0] = code;
    buf[1] = id;
    stw_be_p(buf + 2, len + 4);
    memcpy(buf + 4, data, len);
    ppp_send(s, proto, buf, len + 4);
}

static void lcp_send_req(MsmModemState *s)
{
    MsmAtModem *a = s->at;
    uint8_t req[12] = { 2, 6, 0, 0, 0, 0, 5, 6 };   /* ACCM 0, magic */

    stl_be_p(req + 8, a->magic);
    cp_send(s, PPP_LCP, CONF_REQ, ++a->id, req, sizeof(req));
}

static void ipcp_send_req(MsmModemState *s)
{
    MsmAtModem *a = s->at;
    uint8_t req[6] = { 3, 6 };

    stl_be_p(req + 2, a->gw_ip);
    cp_send(s, PPP_IPCP, CONF_REQ, ++a->id, req, 6);
}

static void ppp_link_reset(MsmAtModem *a)
{
    a->lcp_ours_acked = a->lcp_theirs_acked = false;
    a->ipcp_ours_acked = a->ipcp_theirs_acked = false;
    g_byte_array_set_size(a->rx, 0);
    a->rx_esc = false;
}

static bool ipcp_up(MsmAtModem *a)
{
    return a->ipcp_ours_acked && a->ipcp_theirs_acked;
}

static void arp_announce(MsmModemState *s);

static void lcp_input(MsmModemState *s, const uint8_t *p, int len)
{
    MsmAtModem *a = s->at;
    uint8_t code = p[0], id = p[1];
    uint8_t rej[PPP_MAX];
    int rl = 0;

    switch (code) {
    case CONF_REQ:
        for (int i = 4; i + 2 <= len && p[i + 1] >= 2; i += p[i + 1]) {
            uint8_t t = p[i], l = p[i + 1];
            /* MRU, ACCM, magic, PFC, ACFC are fine; nothing else */
            if (t != 1 && t != 2 && t != 5 && t != 7 && t != 8) {
                memcpy(rej + rl, p + i, MIN(l, len - i));
                rl += MIN(l, len - i);
            }
        }
        if (rl) {
            cp_send(s, PPP_LCP, CONF_REJ, id, rej, rl);
        } else {
            cp_send(s, PPP_LCP, CONF_ACK, id, p + 4, len - 4);
            a->lcp_theirs_acked = true;
            if (!a->lcp_ours_acked) {
                lcp_send_req(s);
            }
        }
        break;
    case CONF_ACK:
        a->lcp_ours_acked = true;
        break;
    case CONF_NAK:
    case CONF_REJ: {
        /* fall back to an empty request */
        cp_send(s, PPP_LCP, CONF_REQ, ++a->id, NULL, 0);
        break;
    }
    case TERM_REQ:
        cp_send(s, PPP_LCP, TERM_ACK, id, NULL, 0);
        ppp_link_reset(a);
        a->online = false;
        a->pdp_active = false;
        ATLOG("ppp terminated by the phone\n");
        break;
    case ECHO_REQ: {
        uint8_t m[4];
        stl_be_p(m, a->magic);
        cp_send(s, PPP_LCP, ECHO_REP, id, m, 4);
        break;
    }
    }
}

static void ipcp_input(MsmModemState *s, const uint8_t *p, int len)
{
    MsmAtModem *a = s->at;
    uint8_t code = p[0], id = p[1];
    uint8_t nak[PPP_MAX], rej[PPP_MAX];
    int nl = 0, rl = 0;
    bool was_up = ipcp_up(a);

    switch (code) {
    case CONF_REQ:
        for (int i = 4; i + 2 <= len && p[i + 1] >= 2; i += p[i + 1]) {
            uint8_t t = p[i], l = p[i + 1];
            uint32_t want = 0;

            if (t == 3 && l == 6) {
                want = a->phone_ip;
            } else if ((t == 129 || t == 131) && l == 6) {
                want = a->dns_ip;
            } else {
                memcpy(rej + rl, p + i, MIN(l, len - i));
                rl += MIN(l, len - i);
                continue;
            }
            if (ldl_be_p(p + i + 2) != want) {
                nak[nl] = t;
                nak[nl + 1] = 6;
                stl_be_p(nak + nl + 2, want);
                nl += 6;
            }
        }
        if (rl) {
            cp_send(s, PPP_IPCP, CONF_REJ, id, rej, rl);
        } else if (nl) {
            cp_send(s, PPP_IPCP, CONF_NAK, id, nak, nl);
        } else {
            cp_send(s, PPP_IPCP, CONF_ACK, id, p + 4, len - 4);
            a->ipcp_theirs_acked = true;
            if (!a->ipcp_ours_acked) {
                ipcp_send_req(s);
            }
        }
        break;
    case CONF_ACK:
        a->ipcp_ours_acked = true;
        break;
    case CONF_NAK:
    case CONF_REJ:
        ipcp_send_req(s);
        break;
    case TERM_REQ:
        cp_send(s, PPP_IPCP, TERM_ACK, id, NULL, 0);
        a->ipcp_ours_acked = a->ipcp_theirs_acked = false;
        break;
    }
    if (!was_up && ipcp_up(a)) {
        ATLOG("ppp: IP up\n");
        arp_announce(s);
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    }
}

static void ppp_frame(MsmModemState *s, const uint8_t *f, int len)
{
    MsmAtModem *a = s->at;
    uint16_t proto;

    if (len < 4 || fcs16(f, len) != 0xF0B8) {
        return;                             /* runt or bad FCS */
    }
    len -= 2;
    if (f[0] == 0xFF && f[1] == 0x03) {
        f += 2;
        len -= 2;
    }
    if (len < 1) {
        return;
    }
    if (f[0] & 1) {                         /* compressed protocol field */
        proto = f[0];
        f++;
        len--;
    } else {
        if (len < 2) {
            return;
        }
        proto = lduw_be_p(f);
        f += 2;
        len -= 2;
    }

    switch (proto) {
    case PPP_LCP:
        if (len >= 4) {
            lcp_input(s, f, len);
        }
        break;
    case PPP_IPCP:
        if (len >= 4) {
            ipcp_input(s, f, len);
        }
        break;
    case PPP_IP:
        if (ipcp_up(a) && len >= 20) {
            uint8_t eth[14 + PPP_MAX];
            static const uint8_t gw_mac[6] = { 0x52, 0x55, 0x0a, 0, 3, 2 };

            len = MIN(len, PPP_MAX);
            memcpy(eth, gw_mac, 6);
            memcpy(eth + 6, s->nic_conf.macaddr.a, 6);
            stw_be_p(eth + 12, ETH_P_IP);
            memcpy(eth + 14, f, len);
            qemu_send_packet(qemu_get_queue(s->nic), eth, 14 + len);
        }
        break;
    default: {
        /* IPv6CP, CCP, ...: not offered */
        uint8_t rej[PPP_MAX];
        int n = MIN(len, PPP_MAX - 2);
        stw_be_p(rej, proto);
        memcpy(rej + 2, f, n);
        cp_send(s, PPP_LCP, PROTO_REJ, ++a->id, rej, n + 2);
        break;
    }
    }
}

void msm_ppp_input(MsmModemState *s, SmdChan *c, const uint8_t *buf, int len)
{
    MsmAtModem *a = s->at;

    for (int i = 0; i < len; i++) {
        uint8_t b = buf[i];

        if (b == 0x7E) {
            if (a->rx->len) {
                ppp_frame(s, a->rx->data, a->rx->len);
            }
            g_byte_array_set_size(a->rx, 0);
            a->rx_esc = false;
            continue;
        }
        if (b == 0x7D) {
            a->rx_esc = true;
            continue;
        }
        if (a->rx_esc) {
            b ^= 0x20;
            a->rx_esc = false;
        }
        if (a->rx->len < PPP_MAX + 8) {
            g_byte_array_append(a->rx, &b, 1);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* The network side                                                          */
/* ------------------------------------------------------------------------ */

static void arp_send(MsmModemState *s, uint16_t op, const uint8_t *tha,
                     uint32_t tip)
{
    uint8_t f[42];

    memcpy(f, op == 1 ? (const uint8_t *)"\xff\xff\xff\xff\xff\xff" : tha, 6);
    memcpy(f + 6, s->nic_conf.macaddr.a, 6);
    stw_be_p(f + 12, ETH_P_ARP);
    stw_be_p(f + 14, 1);                    /* Ethernet */
    stw_be_p(f + 16, ETH_P_IP);
    f[18] = 6;
    f[19] = 4;
    stw_be_p(f + 20, op);
    memcpy(f + 22, s->nic_conf.macaddr.a, 6);
    stl_be_p(f + 28, s->at->phone_ip);
    memcpy(f + 32, op == 1 ? (const uint8_t *)"\0\0\0\0\0\0" : tha, 6);
    stl_be_p(f + 38, tip);
    qemu_send_packet(qemu_get_queue(s->nic), f, sizeof(f));
}

/* make the network learn our address: ask for the gateway */
static void arp_announce(MsmModemState *s)
{
    arp_send(s, 1, NULL, s->at->gw_ip);
}

static bool modem_can_receive(NetClientState *nc)
{
    MsmModemState *s = qemu_get_nic_opaque(nc);
    SmdChan c;

    if (!ipcp_up(s->at)) {
        return false;
    }
    /* don't pile up more than a few frames behind a slow reader */
    return !msm_modem_find_channel(s, "DATA1", 0, &c) ||
           s->tx_backlog[c.n]->len < 32 * 1024;
}

static ssize_t modem_receive(NetClientState *nc, const uint8_t *buf,
                             size_t len)
{
    MsmModemState *s = qemu_get_nic_opaque(nc);
    MsmAtModem *a = s->at;
    uint16_t type;

    if (len < 14) {
        return len;
    }
    type = lduw_be_p(buf + 12);
    if (type == ETH_P_ARP && len >= 42 && lduw_be_p(buf + 20) == 1 &&
        ldl_be_p(buf + 38) == a->phone_ip) {
        arp_send(s, 2, buf + 22, ldl_be_p(buf + 28));
    } else if (type == ETH_P_IP && ipcp_up(a)) {
        ppp_send(s, PPP_IP, buf + 14, MIN(len - 14, PPP_MAX));
    }
    return len;
}

static NetClientInfo modem_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = modem_can_receive,
    .receive = modem_receive,
};

/* ------------------------------------------------------------------------ */
/* Command line                                                              */
/* ------------------------------------------------------------------------ */

static int at_dial(MsmModemState *s, GString *out, const char *num)
{
    MsmAtModem *a = s->at;

    if (strstr(num, "*99")) {
        if (a->cfun != 1) {
            return RC_NO_CARRIER;
        }
        a->pdp_active = true;
        a->online = true;
        ppp_link_reset(a);
        a->magic = g_random_int();
        ATLOG("data call on \"%s\"\n", a->apn);
        return RC_CONNECT;
    }
    return RC_NO_CARRIER;                   /* no voice service */
}

static void at_command_line(MsmModemState *s, const char *line)
{
    MsmAtModem *a = s->at;
    g_autoptr(GString) out = g_string_new(NULL);
    const char *p = line;
    int rc = RC_OK;

    ATLOG("-> %s\n", line);
    while (*p == ' ') {
        p++;
    }
    if (g_ascii_strncasecmp(p, "AT", 2)) {
        return;                             /* not a command: ignore */
    }
    p += 2;
    while (*p && rc == RC_OK) {
        char ch = g_ascii_toupper(*p);

        if (strchr("+@$%^*#", ch)) {
            /* extended command up to an unquoted ';' */
            GString *cmd = g_string_new(NULL);
            bool q = false;
            while (*p && (q || *p != ';')) {
                if (*p == '"') {
                    q = !q;
                }
                g_string_append_c(cmd, *p++);
            }
            if (*p == ';') {
                p++;
            }
            rc = at_ext(s, out, cmd->str);
            g_string_free(cmd, true);
        } else if (ch == 'D') {
            rc = at_dial(s, out, p + 1);
            p += strlen(p);
        } else if (ch == '&') {
            p += 2;
            while (g_ascii_isdigit(*p)) {
                p++;
            }
        } else if (g_ascii_isalpha(ch)) {
            int v;
            p++;
            v = atoi(p);
            while (g_ascii_isdigit(*p)) {
                p++;
            }
            if (*p == '=') {                /* S-register style */
                p++;
                while (g_ascii_isdigit(*p)) {
                    p++;
                }
            } else if (*p == '?') {
                p++;
            }
            switch (ch) {
            case 'E':
                a->echo = v;
                break;
            case 'V':
                a->verbose = v;
                break;
            case 'Z':
                a->echo = a->verbose = true;
                break;
            case 'H':
                break;
            case 'A':
                rc = RC_NO_CARRIER;
                break;
            }
        } else {
            p++;
        }
    }
    at_final(s, out, rc);
    at_send(s, out->str);
}

void msm_at_input(MsmModemState *s, SmdChan *c, const uint8_t *buf, int len)
{
    MsmAtModem *a = s->at;

    if (a->echo) {
        msm_modem_chan_send(s, "DS", buf, len);
    }
    for (int i = 0; i < len; i++) {
        if (buf[i] == '\r' || buf[i] == '\n') {
            if (a->line->len) {
                g_byte_array_append(a->line, (const uint8_t *)"", 1);
                at_command_line(s, (const char *)a->line->data);
                g_byte_array_set_size(a->line, 0);
            }
        } else if (a->line->len < 1024) {
            g_byte_array_append(a->line, buf + i, 1);
        }
    }
}

void msm_atmodem_opened(MsmModemState *s, SmdChan *c)
{
    if (!strcmp(c->name, "DS")) {
        g_byte_array_set_size(s->at->line, 0);
        /* the baseband announces itself when the port opens */
        at_unsolicited(s, "AT-Command Interpreter ready");
    } else if (!strcmp(c->name, "DATA1")) {
        ppp_link_reset(s->at);
    }
}

void msm_atmodem_reset(MsmModemState *s)
{
    MsmAtModem *a = s->at;

    g_byte_array_set_size(a->line, 0);
    a->echo = true;
    a->verbose = true;
    a->creg_n = a->cgreg_n = a->cops_fmt = a->cmee = 0;
    a->cfun = 0;
    a->pdp_active = a->online = false;
    a->sim_announced = false;
    pstrcpy(a->apn, sizeof(a->apn), "internet");
    ppp_link_reset(a);
    timer_del(a->reg_timer);
}

void msm_atmodem_init(MsmModemState *s)
{
    MsmAtModem *a = g_new0(MsmAtModem, 1);

    s->at = a;
    a->line = g_byte_array_new();
    a->rx = g_byte_array_new();
    a->reg_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, reg_timer_cb, s);
    /* the backend is expected on 10.0.3.0/24 (-nic user,net=10.0.3.0/24) */
    a->phone_ip = 0x0A00030F;
    a->gw_ip = 0x0A000302;
    a->dns_ip = 0x0A000303;
    qemu_macaddr_default_if_unset(&s->nic_conf.macaddr);
    s->nic = qemu_new_nic(&modem_net_info, &s->nic_conf, TYPE_MSM_MODEM,
                          NULL, &DEVICE(s)->mem_reentrancy_guard, s);
    msm_atmodem_reset(s);
}
