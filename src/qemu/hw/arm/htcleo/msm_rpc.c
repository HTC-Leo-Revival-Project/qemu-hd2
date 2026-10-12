/*
 * ONC RPC router running on the modem side of the "RPCCALL" SMD channel.
 *
 * Wire format from arch/arm/mach-msm/smd_rpcrouter.[ch]:
 *   every SMD packet = struct rr_header (8 x u32, little endian)
 *                      + payload of hdr.size bytes
 *   control payload  = union rr_control_msg
 *   data payload     = pacmark (u32: len | mid << 16 | first << 30 |
 *                      last << 31) + ONC RPC message fragment (big endian)
 *
 * On a NAND boot AMSS opens the conversation with HELLO; the kernel answers
 * HELLO and lists its own servers, after which the modem announces the
 * programs it implements with NEW_SERVER.  The kernel creates a platform
 * device "rsPPPPPPPP:VVVVVVVV" per announced server, which is what binds
 * drivers such as the RTC.
 *
 * Calls to modem programs are answered here.  Programs with real semantics
 * (time of day) are implemented; the rest accept the call and return
 * zero-filled results, which is how AMSS behaves for requests that only
 * switch hardware we do not have.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/bswap.h"
#include "system/rtc.h"
#include "htcleo.h"
#include "msm_modem.h"

#define RR_VERSION      1
#define RR_DATA         1
#define RR_HELLO        2
#define RR_BYE          3
#define RR_NEW_SERVER   4
#define RR_REMOVE_SERVER 5
#define RR_REMOVE_CLIENT 6
#define RR_RESUME_TX    7
#define RR_EXIT         8
#define RR_PING         9

#define RR_ROUTER_ADDRESS 0xFFFFFFFE
#define RR_PID_MODEM    0       /* RPCROUTER_PID_REMOTE */
#define RR_PID_APPS     1

#define RPC_CALL        0
#define RPC_REPLY       1
#define RR_FRAG_MAX     500     /* RPCROUTER_DATASIZE_MAX */

#define PACMARK(len, mid, first, last) \
    (((len) & 0xFFFF) | (((mid) & 0xFF) << 16) | ((!!(first)) << 30) | \
     ((uint32_t)(!!(last)) << 31))

/* Modem programs announced to the kernel (AMSS 1550 versions) */
static const struct {
    uint32_t prog, vers;
    const char *what;
} modem_programs[] = {
    { 0x30000048, 0x00010000, "time_remote (RTC)" },
    { 0x30000061, 0x00030001, "pm_lib" },
    { 0x30000061, 0x00010001, "pm_lib (pmic.c)" },
    { 0x30000060, 0x00010001, "pm" },
    { 0x30100001, 0x00000000, "htc battery" },
    { 0x30100003, 0x00000000, "htc acoustic" },
    { 0x30000064, 0x00010001, "hsusb api" },
    { 0x30000013, 0x00000000, "audmgr" },
    { 0x3000000a, 0x00000000, "adsp rtos" },
    { 0x30000002, 0x00010001, "cm (call manager)" },
    { 0x30000003, 0x00010001, "wms" },
    { 0x3000000e, 0x00010001, "nv" },
    { 0x30000016, 0x00010001, "pdsm (gps)" },
    { 0x30000019, 0x00010001, "snd" },
    { 0x3000001d, 0x00010001, "pdsm_atl (gps)" },
    { 0x3000005b, 0x00010001, "pdapi (gps)" },
};

typedef struct RpcMsg {
    uint32_t xid, prog, vers, proc;
    const uint8_t *args;
    uint32_t args_len;
} RpcMsg;

static uint64_t rr_key(uint32_t pid, uint32_t cid)
{
    return ((uint64_t)pid << 32) | cid;
}

static SmdChan *rpc_channel(MsmModemState *s, SmdChan *c)
{
    return msm_modem_find_channel(s, "RPCCALL", 0, c) ? c : NULL;
}

static void rr_send(MsmModemState *s, uint32_t type, uint32_t src_cid,
                    uint32_t dst_pid, uint32_t dst_cid,
                    const void *payload, uint32_t len)
{
    SmdChan c;
    g_autofree uint8_t *pkt = g_malloc(32 + len);

    if (!rpc_channel(s, &c)) {
        return;
    }
    stl_le_p(pkt + 0, RR_VERSION);
    stl_le_p(pkt + 4, type);
    stl_le_p(pkt + 8, RR_PID_MODEM);
    stl_le_p(pkt + 12, src_cid);
    stl_le_p(pkt + 16, 0);              /* confirm_rx */
    stl_le_p(pkt + 20, len);
    stl_le_p(pkt + 24, dst_pid);
    stl_le_p(pkt + 28, dst_cid);
    memcpy(pkt + 32, payload, len);
    if ((c.packet ? smd_modem_send_packet(s, &c, pkt, 32 + len)
                  : smd_modem_write(s, &c, pkt, 32 + len)) < 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "modem rpc: SMD fifo full\n");
    }
    smd_kick_apps(s, &c);
}

static void rr_send_ctrl(MsmModemState *s, const uint32_t *words, int n)
{
    uint32_t msg[5] = { 0 };

    for (int i = 0; i < n && i < 5; i++) {
        stl_le_p(&msg[i], words[i]);
    }
    rr_send(s, words[0], RR_ROUTER_ADDRESS, RR_PID_APPS, RR_ROUTER_ADDRESS,
            msg, sizeof(msg));
}

static void rpc_announce_servers(MsmModemState *s)
{
    for (guint i = 0; i < s->rpc_servers->len; i++) {
        RpcServer *sv = &g_array_index(s->rpc_servers, RpcServer, i);
        uint32_t w[5] = { RR_NEW_SERVER, sv->prog, sv->vers, RR_PID_MODEM,
                          sv->cid };
        rr_send_ctrl(s, w, 5);
    }
}

static void rpc_hello_timer(void *opaque)
{
    MsmModemState *s = opaque;
    uint32_t w[1] = { RR_HELLO };

    rr_send_ctrl(s, w, 1);
}

void msm_modem_smd_opened(MsmModemState *s, SmdChan *c)
{
    if (!strcmp(c->name, "RPCCALL") && (c->ctype & 0xFF) == 0) {
        s->rpc_hello_done = false;
        /* give the kernel a moment to finish smd_open() and start reading */
        timer_mod(s->rpc_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  5 * SCALE_MS);
    }
}

/* ---------------------------------------------------------------- */
/* Program implementations                                           */
/* ---------------------------------------------------------------- */

typedef struct RpcReply {
    uint8_t buf[RR_FRAG_MAX * 4];
    uint32_t len;
} RpcReply;

static void put_be32(RpcReply *r, uint32_t v)
{
    if (r->len + 4 <= sizeof(r->buf)) {
        stl_be_p(r->buf + r->len, v);
        r->len += 4;
    }
}

static uint32_t arg_be32(const RpcMsg *m, int i)
{
    if ((uint32_t)(i + 1) * 4 > m->args_len) {
        return 0;
    }
    return ldl_be_p(m->args + 4 * i);
}

static int64_t rtc_offset;      /* guest RTC - host time, seconds */

static void prog_time_remote(MsmModemState *s, const RpcMsg *m, RpcReply *r)
{
    struct tm tm;
    time_t t;

    switch (m->proc) {
    case 7: /* TIMEREMOTE_PROCEEDURE_GET_JULIAN */
        t = time(NULL) + rtc_offset;
        gmtime_r(&t, &tm);
        put_be32(r, 1);                 /* opt_arg: data present */
        put_be32(r, tm.tm_year + 1900);
        put_be32(r, tm.tm_mon + 1);
        put_be32(r, tm.tm_mday);
        put_be32(r, tm.tm_hour);
        put_be32(r, tm.tm_min);
        put_be32(r, tm.tm_sec);
        put_be32(r, tm.tm_wday);
        break;
    case 6: /* TIMEREMOTE_PROCEEDURE_SET_JULIAN */
        if (arg_be32(m, 0)) {
            memset(&tm, 0, sizeof(tm));
            tm.tm_year = arg_be32(m, 1) - 1900;
            tm.tm_mon = arg_be32(m, 2) - 1;
            tm.tm_mday = arg_be32(m, 3);
            tm.tm_hour = arg_be32(m, 4);
            tm.tm_min = arg_be32(m, 5);
            tm.tm_sec = arg_be32(m, 6);
            rtc_offset = (int64_t)timegm(&tm) - (int64_t)time(NULL);
        }
        break;
    default:
        put_be32(r, 0);
        break;
    }
}

static void prog_default(MsmModemState *s, const RpcMsg *m, RpcReply *r)
{
    /* success with a zeroed result block */
    for (int i = 0; i < 8; i++) {
        put_be32(r, 0);
    }
}

static void rpc_dispatch(MsmModemState *s, const RpcMsg *m, RpcReply *r)
{
    switch (m->prog) {
    case 0x30000048:
        prog_time_remote(s, m, r);
        break;
    case 0x3000005b:
        put_be32(r, msm_gps_pdapi_call(s, m->proc, m->args, m->args_len));
        break;
    default:
        prog_default(s, m, r);
        break;
    }
}

/* ---------------------------------------------------------------- */
/* Message handling                                                  */
/* ---------------------------------------------------------------- */

static void rpc_send_reply(MsmModemState *s, uint32_t src_cid,
                           uint32_t dst_pid, uint32_t dst_cid,
                           uint32_t xid, RpcReply *res)
{
    uint8_t msg[24 + sizeof(res->buf)];
    uint32_t len = 0, off = 0;
    uint8_t mid = (uint8_t)(s->rpc_calls & 0xFF);

    stl_be_p(msg + 0, xid);
    stl_be_p(msg + 4, RPC_REPLY);
    stl_be_p(msg + 8, 0);               /* MSG_ACCEPTED */
    stl_be_p(msg + 12, 0);              /* verf flavor */
    stl_be_p(msg + 16, 0);              /* verf length */
    stl_be_p(msg + 20, 0);              /* SUCCESS */
    memcpy(msg + 24, res->buf, res->len);
    len = 24 + res->len;

    while (off < len) {
        uint32_t chunk = MIN(len - off, RR_FRAG_MAX);
        uint8_t frag[4 + RR_FRAG_MAX];
        stl_le_p(frag, PACMARK(chunk, mid, off == 0, off + chunk == len));
        memcpy(frag + 4, msg + off, chunk);
        rr_send(s, RR_DATA, src_cid, dst_pid, dst_cid, frag, 4 + chunk);
        off += chunk;
    }
}

static RpcServer *rpc_server_by_cid(MsmModemState *s, uint32_t cid)
{
    for (guint i = 0; i < s->rpc_servers->len; i++) {
        RpcServer *sv = &g_array_index(s->rpc_servers, RpcServer, i);
        if (sv->cid == cid) {
            return sv;
        }
    }
    return NULL;
}

/*
 * librpc does not register callback programs (0x31xxxxxx) with the router:
 * AMSS sends a callback to the endpoint that called the matching program
 * (0x30xxxxxx), and librpc dispatches it to the svc registered on it.
 */
#define RPC_CB_BIT      0x01000000

static void rpc_note_client(MsmModemState *s, uint32_t prog, uint32_t vers,
                            uint32_t pid, uint32_t cid)
{
    RpcServer cl = { .prog = prog, .vers = vers, .pid = pid, .cid = cid };

    for (guint i = 0; i < s->rpc_clients->len; i++) {
        RpcServer *o = &g_array_index(s->rpc_clients, RpcServer, i);
        if (o->prog == prog) {
            *o = cl;
            return;
        }
    }
    g_array_append_val(s->rpc_clients, cl);
}

static void rpc_handle_message(MsmModemState *s, uint32_t src_pid,
                               uint32_t src_cid, uint32_t dst_cid,
                               const uint8_t *msg, uint32_t len)
{
    RpcMsg m;
    RpcReply r = { .len = 0 };
    uint32_t cred_len, verf_len, off;

    if (len < 8) {
        return;
    }
    m.xid = ldl_be_p(msg);
    if (ldl_be_p(msg + 4) != RPC_CALL) {
        return;     /* replies to modem-originated calls: none issued */
    }
    if (len < 32) {
        return;
    }
    m.prog = ldl_be_p(msg + 12);
    m.vers = ldl_be_p(msg + 16);
    m.proc = ldl_be_p(msg + 20);
    cred_len = ldl_be_p(msg + 28);
    off = 32 + ((cred_len + 3) & ~3);
    if (off + 8 > len) {
        return;
    }
    verf_len = ldl_be_p(msg + off + 4);
    off += 8 + ((verf_len + 3) & ~3);
    m.args = msg + MIN(off, len);
    m.args_len = len > off ? len - off : 0;

    {
        RpcServer *sv = rpc_server_by_cid(s, dst_cid);
        if (sv && sv->prog != m.prog) {
            m.prog = sv->prog;
        }
    }
    rpc_note_client(s, m.prog, m.vers, src_pid, src_cid);
    s->rpc_calls++;
    if (s->trace) {
        qemu_log("modem rpc: call prog %08x vers %08x proc %u (%u arg bytes)\n",
                 m.prog, m.vers, m.proc, m.args_len);
    }
    rpc_dispatch(s, &m, &r);
    rpc_send_reply(s, dst_cid, src_pid, src_cid, m.xid, &r);
}

static void rr_handle_control(MsmModemState *s, const uint8_t *p, uint32_t len)
{
    uint32_t cmd = len >= 4 ? ldl_le_p(p) : 0;

    switch (cmd) {
    case RR_HELLO:
        if (!s->rpc_hello_done) {
            s->rpc_hello_done = true;
            rpc_announce_servers(s);
        }
        break;
    case RR_NEW_SERVER:
        if (len >= 20) {
            RpcServer sv = {
                .prog = ldl_le_p(p + 4), .vers = ldl_le_p(p + 8),
                .pid = ldl_le_p(p + 12), .cid = ldl_le_p(p + 16),
            };
            for (guint i = 0; i < s->rpc_apps_servers->len; i++) {
                RpcServer *o = &g_array_index(s->rpc_apps_servers,
                                              RpcServer, i);
                if (o->prog == sv.prog && o->vers == sv.vers) {
                    *o = sv;
                    return;
                }
            }
            g_array_append_val(s->rpc_apps_servers, sv);
            if (s->trace) {
                qemu_log("modem rpc: apps server %08x:%08x at %u:%u\n",
                         sv.prog, sv.vers, sv.pid, sv.cid);
            }
        }
        break;
    case RR_REMOVE_SERVER:
    case RR_REMOVE_CLIENT:
    case RR_RESUME_TX:
    case RR_BYE:
    case RR_PING:
        break;
    default:
        if (s->trace) {
            qemu_log("modem rpc: control cmd %u\n", cmd);
        }
    }
}

static void rr_handle_msg(MsmModemState *s, const uint8_t *buf, uint32_t len)
{
    uint32_t type, src_pid, src_cid, confirm, size, dst_cid;

    if (len < 32) {
        return;
    }
    type = ldl_le_p(buf + 4);
    src_pid = ldl_le_p(buf + 8);
    src_cid = ldl_le_p(buf + 12);
    confirm = ldl_le_p(buf + 16);
    size = ldl_le_p(buf + 20);
    dst_cid = ldl_le_p(buf + 28);
    if (32 + size > len) {
        size = len - 32;
    }

    if (s->trace && type != RR_DATA) {
        qemu_log("modem rpc: rx ctrl type %u size %u cmd %u w1 %08x w2 %08x\n",
                 type, size, size >= 4 ? ldl_le_p(buf + 32) : 0,
                 size >= 8 ? ldl_le_p(buf + 36) : 0,
                 size >= 12 ? ldl_le_p(buf + 40) : 0);
    }
    if (type != RR_DATA) {
        rr_handle_control(s, buf + 32, size);
    } else if (size >= 4) {
        uint32_t pm = ldl_le_p(buf + 32);
        uint64_t key = rr_key(src_pid, src_cid);
        GByteArray *acc = g_hash_table_lookup(s->rpc_frag,
                                              (gpointer)(uintptr_t)key);
        if (!acc) {
            acc = g_byte_array_new();
            g_hash_table_insert(s->rpc_frag, (gpointer)(uintptr_t)key, acc);
        }
        if (pm & (1u << 30)) {
            g_byte_array_set_size(acc, 0);
        }
        g_byte_array_append(acc, buf + 36, MIN(pm & 0xFFFF, size - 4));
        if (pm & (1u << 31)) {
            rpc_handle_message(s, src_pid, src_cid, dst_cid, acc->data,
                               acc->len);
            g_byte_array_set_size(acc, 0);
        }
    }

    if (confirm) {
        /* tell the sender's router it may transmit again */
        uint32_t w[3] = { RR_RESUME_TX, RR_PID_MODEM, dst_cid };
        rr_send_ctrl(s, w, 3);
    }
}

void msm_modem_smd_packet(MsmModemState *s, SmdChan *c, const uint8_t *buf,
                          uint32_t len)
{
    if (!strcmp(c->name, "RPCCALL")) {
        rr_handle_msg(s, buf, len);
    }
}

void msm_modem_smd_stream(MsmModemState *s, SmdChan *c, const uint8_t *buf,
                          uint32_t len)
{
    GByteArray *acc;

    if (!strncmp(c->name, "DAL", 3)) {
        g_byte_array_append(s->pkt_acc[c->n], buf, len);
        msm_qdsp6_dal_stream(s, c, s->pkt_acc[c->n]);
        return;
    }
    if (!strcmp(c->name, "DS")) {
        msm_at_input(s, c, buf, len);
        return;
    }
    if (!strcmp(c->name, "DATA1")) {
        msm_ppp_input(s, c, buf, len);
        return;
    }
    if (strcmp(c->name, "RPCCALL")) {
        return;     /* DIAG and the rest: consumed */
    }
    /* the router frames its own messages: rr_header (32 B) + size */
    acc = s->pkt_acc[c->n];
    g_byte_array_append(acc, buf, len);
    while (acc->len >= 32) {
        uint32_t size = ldl_le_p(acc->data + 20);
        if (size > 0x10000) {
            g_byte_array_set_size(acc, 0);  /* lost sync */
            break;
        }
        if (acc->len < 32 + size) {
            break;
        }
        rr_handle_msg(s, acc->data, 32 + size);
        g_byte_array_remove_range(acc, 0, 32 + size);
    }
}

/*
 * Call a program the apps processor serves (callbacks such as the GPS
 * position events).  args are big-endian words; the reply is ignored.
 */
#define RPC_MODEM_CLIENT_CID    0x500

void msm_rpc_apps_call(MsmModemState *s, uint32_t prog, uint32_t vers,
                       uint32_t proc, const uint32_t *args, int nargs)
{
    RpcServer *sv = NULL;
    g_autofree uint8_t *msg = NULL;
    uint32_t len = 40 + 4 * nargs, off = 0, src_cid = RPC_MODEM_CLIENT_CID;
    uint8_t mid = (uint8_t)(s->rpc_calls & 0xFF);

    for (guint i = 0; i < s->rpc_apps_servers->len; i++) {
        RpcServer *o = &g_array_index(s->rpc_apps_servers, RpcServer, i);
        if (o->prog == prog && (o->vers & 0xFFFF0000) == (vers & 0xFFFF0000)) {
            sv = o;
            break;
        }
    }
    for (guint i = 0; !sv && (prog & RPC_CB_BIT) &&
                      i < s->rpc_clients->len; i++) {
        RpcServer *o = &g_array_index(s->rpc_clients, RpcServer, i);
        if (o->prog == (prog & ~RPC_CB_BIT)) {
            sv = o;
        }
    }
    if (!sv) {
        return;                     /* the apps side is not listening */
    }
    /*
     * Send from the matching modem server's address: the kernel only routes
     * the reply to endpoints the modem announced with NEW_SERVER.
     */
    for (guint i = 0; i < s->rpc_servers->len; i++) {
        RpcServer *o = &g_array_index(s->rpc_servers, RpcServer, i);
        if (o->prog == (prog & ~RPC_CB_BIT)) {
            src_cid = o->cid;
            break;
        }
    }
    msg = g_malloc(len);
    stl_be_p(msg + 0, ++s->rpc_xid);
    stl_be_p(msg + 4, RPC_CALL);
    stl_be_p(msg + 8, 2);           /* RPC version */
    stl_be_p(msg + 12, prog);
    stl_be_p(msg + 16, vers);
    stl_be_p(msg + 20, proc);
    memset(msg + 24, 0, 16);        /* AUTH_NULL credentials and verifier */
    memcpy(msg + 40, args, 4 * nargs);
    s->rpc_calls++;
    while (off < len) {
        uint32_t chunk = MIN(len - off, RR_FRAG_MAX);
        uint8_t frag[4 + RR_FRAG_MAX];
        stl_le_p(frag, PACMARK(chunk, mid, off == 0, off + chunk == len));
        memcpy(frag + 4, msg + off, chunk);
        rr_send(s, RR_DATA, src_cid, sv->pid, sv->cid, frag,
                4 + chunk);
        off += chunk;
    }
}

void msm_modem_rpc_reset(MsmModemState *s)
{
    g_array_set_size(s->rpc_apps_servers, 0);
    g_array_set_size(s->rpc_clients, 0);
    s->rpc_hello_done = false;
    s->rpc_calls = 0;
    g_hash_table_remove_all(s->rpc_frag);
    timer_del(s->rpc_timer);
}

void msm_modem_rpc_init(MsmModemState *s)
{
    s->rpc_servers = g_array_new(false, true, sizeof(RpcServer));
    s->rpc_apps_servers = g_array_new(false, true, sizeof(RpcServer));
    s->rpc_clients = g_array_new(false, true, sizeof(RpcServer));
    s->rpc_frag = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                        (GDestroyNotify)g_byte_array_unref);
    s->rpc_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rpc_hello_timer, s);
    for (size_t i = 0; i < ARRAY_SIZE(modem_programs); i++) {
        RpcServer sv = {
            .prog = modem_programs[i].prog,
            .vers = modem_programs[i].vers,
            .cid = 0x100 + i,
        };
        g_array_append_val(s->rpc_servers, sv);
    }
}
