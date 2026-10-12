/*
 * QSD8250 QDSP6 audio DSP, as reached over the "DAL00" SMD channel.
 *
 * Wire protocol from arch/arm/mach-msm/qdsp6_1550/dal.c (htcleo kernel):
 *
 *   struct dal_hdr {             16 bytes, little endian
 *       u32 length:16 (header inclusive), version:8, priority:7, async:1;
 *       u32 ddi:16, prototype:8, msgid:8;
 *       u32 from, to;            client handles
 *   }
 *   msgid 0x01 ATTACH (payload: device_id, attach[64], service[32])
 *         0x02 DETACH, 0x00 DDI call, 0x80|msgid reply, 0xC0 async event
 *
 * Audio (dal_audio.h, CONFIG_MACH_HTCLEO layout), DDI numbers from
 * DAL_OP_FIRST_DEVICE_API = 9:
 *   9 OPEN  10 WRITE  11 READ  12 IOCTL  13 INIT  14 CLOSE  15 FLUSH
 * Every client is one session; INIT gives {cb_evt, loc_evt, session}.
 * OPEN completes with ADSP_AUDIO_EVT_STATUS_OPEN, every WRITE/READ buffer
 * with ADSP_AUDIO_EVT_STATUS_BUF_DONE, which is what paces playback.
 *
 * PCM written by Android goes to QEMU's audio backend; recording returns
 * silence.  ACDB and ADIE (calibration / analog codec) calls succeed.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/audio.h"
#include "qemu/cutils.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "htcleo.h"
#include "msm_modem.h"

#define DAL_HDR_SIZE        16
#define DAL_VERSION         0x11
#define DAL_MSGID_DDI       0x00
#define DAL_MSGID_ATTACH    0x01
#define DAL_MSGID_DETACH    0x02
#define DAL_MSGID_REPLY     0x80
#define DAL_MSGID_ASYNCH    0xC0

#define AUDIO_OP_OPEN       9
#define AUDIO_OP_WRITE      10
#define AUDIO_OP_READ       11
#define AUDIO_OP_IOCTL      12
#define AUDIO_OP_INIT       13
#define AUDIO_OP_CLOSE      14
#define AUDIO_OP_FLUSH      15

#define ADSP_AUDIO_OPCODE_OPEN_READ     0x01
#define ADSP_AUDIO_OPCODE_OPEN_WRITE    0x02
#define ADSP_AUDIO_EVT_STATUS_OPEN      0x0108c0d6
#define ADSP_AUDIO_EVT_STATUS_CLOSE     0x0108c0d7
#define ADSP_AUDIO_EVT_STATUS_BUF_DONE  0x0108c0d8

#define DAL_MAX_HANDLES     32
#define DAL_EVENT_SIZE      96

typedef struct DalBuf {
    uint32_t addr, size;
} DalBuf;

typedef struct DalHandle {
    bool used;
    uint32_t client;            /* far-side (kernel) client pointer */
    uint32_t device;
    char chan[24];              /* SMD channel this client talks on */
    int chan_type;
    uint32_t cb_evt, loc_evt, session;
    int mode;                   /* 0 none, 1 playback, 2 capture */
    uint32_t rate, channels;
    GQueue *bufs;               /* DalBuf, oldest first */
    QEMUTimer *timer;
    SWVoiceOut *voice;
    struct MsmModemState *modem;
} DalHandle;

static DalHandle handles[DAL_MAX_HANDLES];

static uint32_t handle_id(int i)
{
    return 0xD5000000 | (i << 4);
}

static DalHandle *handle_by_id(uint32_t id)
{
    int i = (id >> 4) & 0xFF;

    if ((id & 0xFFFFF00F) != 0xD5000000 || i >= DAL_MAX_HANDLES ||
        !handles[i].used) {
        return NULL;
    }
    return &handles[i];
}

static void dal_send(MsmModemState *s, const char *chan, int type,
                     uint8_t msgid,
                     uint16_t ddi, uint8_t proto, uint32_t from, uint32_t to,
                     const void *payload, int len)
{
    SmdChan c;
    uint8_t hdr[DAL_HDR_SIZE];

    if (!msm_modem_find_channel(s, chan, type, &c)) {
        return;
    }
    stl_le_p(hdr, (DAL_HDR_SIZE + len) | (DAL_VERSION << 16));
    stl_le_p(hdr + 4, ddi | (proto << 16) | ((uint32_t)msgid << 24));
    stl_le_p(hdr + 8, from);
    stl_le_p(hdr + 12, to);
    smd_modem_write(s, &c, hdr, sizeof(hdr));
    if (len) {
        smd_modem_write(s, &c, payload, len);
    }
    smd_kick_apps(s, &c);
}

static void dal_event(DalHandle *h, uint32_t event_id, uint32_t status)
{
    uint8_t ev[DAL_EVENT_SIZE] = { 0 };

    stl_le_p(ev + 0, h->cb_evt);
    stl_le_p(ev + 4, h->loc_evt);
    stl_le_p(ev + 8, DAL_EVENT_SIZE - 12);
    /* struct adsp_audio_event: hdr{context, data}, session, event_id ... */
    stl_le_p(ev + 20, h->session);
    stl_le_p(ev + 24, event_id);
    stl_le_p(ev + 28, status);
    dal_send(h->modem, h->chan, h->chan_type, DAL_MSGID_ASYNCH, 0, 0,
             handle_id(h - handles), h->client, ev, sizeof(ev));
}

/* ---------------------------------------------------------------- */
/* Stream pacing                                                     */
/* ---------------------------------------------------------------- */

static void stream_arm(DalHandle *h)
{
    DalBuf *b = g_queue_peek_head(h->bufs);
    uint64_t bytes_per_sec;
    int64_t ns;

    if (!b || timer_pending(h->timer)) {
        return;
    }
    bytes_per_sec = (uint64_t)MAX(h->rate, 8000) * MAX(h->channels, 1) * 2;
    ns = muldiv64(MAX(b->size, 64), NANOSECONDS_PER_SECOND, bytes_per_sec);
    timer_mod(h->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns);

    if (h->mode == 1 && h->voice && h->modem->audio_be) {
        g_autofree uint8_t *pcm = g_malloc(b->size);
        address_space_read(&address_space_memory, b->addr,
                           MEMTXATTRS_UNSPECIFIED, pcm, b->size);
        audio_be_write(h->modem->audio_be, h->voice, pcm, b->size);
    }
}

static void stream_tick(void *opaque)
{
    DalHandle *h = opaque;
    DalBuf *b = g_queue_pop_head(h->bufs);

    if (!b) {
        return;
    }
    if (h->mode == 2) {
        /* capture: no microphone, deliver silence */
        g_autofree uint8_t *zero = g_malloc0(b->size);
        address_space_write(&address_space_memory, b->addr,
                            MEMTXATTRS_UNSPECIFIED, zero, b->size);
    }
    g_free(b);
    dal_event(h, ADSP_AUDIO_EVT_STATUS_BUF_DONE, 0);
    stream_arm(h);
}

static void voice_callback(void *opaque, int avail)
{
    /* pacing is done by the BUF_DONE timer; nothing to pull here */
}

static void stream_open(DalHandle *h, uint32_t opcode, uint32_t pblock)
{
    uint8_t fmt[8];

    h->mode = opcode == ADSP_AUDIO_OPCODE_OPEN_WRITE ? 1 :
              opcode == ADSP_AUDIO_OPCODE_OPEN_READ ? 2 : 0;
    h->rate = 44100;
    h->channels = 2;
    if (pblock) {
        address_space_read(&address_space_memory, pblock,
                           MEMTXATTRS_UNSPECIFIED, fmt, sizeof(fmt));
        h->channels = lduw_le_p(fmt) ? lduw_le_p(fmt) : 2;
        h->rate = ldl_le_p(fmt + 4) ? ldl_le_p(fmt + 4) : 44100;
    }
    if (h->mode == 1 && h->modem->audio_be) {
        struct audsettings as = {
            .freq = h->rate,
            .nchannels = h->channels,
            .fmt = AUDIO_FORMAT_S16,
            .big_endian = false,
        };
        h->voice = audio_be_open_out(h->modem->audio_be, h->voice,
                                     "htcleo.qdsp6", h, voice_callback, &as);
        if (h->voice) {
            audio_be_set_active_out(h->modem->audio_be, h->voice, true);
        }
    }
}

static void stream_close(DalHandle *h)
{
    DalBuf *b;

    timer_del(h->timer);
    while ((b = g_queue_pop_head(h->bufs))) {
        g_free(b);
    }
    if (h->voice && h->modem->audio_be) {
        audio_be_set_active_out(h->modem->audio_be, h->voice, false);
    }
    h->mode = 0;
}

/* ---------------------------------------------------------------- */
/* DAL message handling                                              */
/* ---------------------------------------------------------------- */

static void dal_handle_msg(MsmModemState *s, const char *chan, int type,
                           const uint8_t *m, uint32_t len)
{
    uint32_t w1 = ldl_le_p(m + 4);
    uint16_t ddi = w1 & 0xFFFF;
    uint8_t proto = (w1 >> 16) & 0xFF;
    uint8_t msgid = w1 >> 24;
    uint32_t from = ldl_le_p(m + 8), to = ldl_le_p(m + 12);
    const uint8_t *p = m + DAL_HDR_SIZE;
    uint32_t plen = len - DAL_HDR_SIZE;
    uint8_t reply[68] = { 0 };
    DalHandle *h;

    if (msgid == DAL_MSGID_ATTACH) {
        int i;
        for (i = 0; i < DAL_MAX_HANDLES && handles[i].used; i++) {
        }
        if (i == DAL_MAX_HANDLES) {
            stl_le_p(reply, 1);
            dal_send(s, chan, type, DAL_MSGID_ATTACH | DAL_MSGID_REPLY, ddi, proto,
                     0, from, reply, sizeof(reply));
            return;
        }
        h = &handles[i];
        if (!h->bufs) {
            h->bufs = g_queue_new();
            h->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, stream_tick, h);
        }
        h->used = true;
        h->client = from;
        h->device = plen >= 4 ? ldl_le_p(p) : 0;
        h->modem = s;
        h->mode = 0;
        pstrcpy(h->chan, sizeof(h->chan), chan);
        h->chan_type = type;
        stl_le_p(reply, 0);
        pstrcpy((char *)reply + 4, 64,
                plen > 4 && p[4] ? (const char *)p + 4 : "AudioQdsp");
        if (s->trace) {
            qemu_log("qdsp6: attach device 0x%x '%s' -> handle %d\n",
                     h->device, (char *)reply + 4, i);
        }
        dal_send(s, chan, type, DAL_MSGID_ATTACH | DAL_MSGID_REPLY, ddi, proto,
                 handle_id(i), from, reply, sizeof(reply));
        return;
    }

    h = handle_by_id(to);
    if (msgid == DAL_MSGID_DETACH) {
        if (h) {
            stream_close(h);
            h->used = false;
        }
        dal_send(s, chan, type, DAL_MSGID_DETACH | DAL_MSGID_REPLY, ddi, proto,
                 to, from, reply, 4);
        return;
    }
    if (msgid != DAL_MSGID_DDI) {
        return;
    }

    /* every call succeeds: status word 0 */
    dal_send(s, chan, type, DAL_MSGID_DDI | DAL_MSGID_REPLY, ddi, proto, to, from,
             reply, 4);
    if (!h) {
        return;
    }
    if (s->trace) {
        qemu_log("qdsp6: handle %d ddi %u proto %u len %u\n",
                 (int)(h - handles), ddi, proto, plen);
    }

    /* f5/f6 payloads start with the input length (f6: cmd first) */
    switch (ddi) {
    case AUDIO_OP_INIT:
        if (plen >= 4 + 12) {
            h->cb_evt = ldl_le_p(p + 4);
            h->loc_evt = ldl_le_p(p + 8);
            h->session = ldl_le_p(p + 12);
        }
        break;
    case AUDIO_OP_OPEN:
        if (plen >= 4 + 48) {
            stream_open(h, ldl_le_p(p + 12), ldl_le_p(p + 44));
        }
        dal_event(h, ADSP_AUDIO_EVT_STATUS_OPEN, 0);
        break;
    case AUDIO_OP_WRITE:
    case AUDIO_OP_READ:
        if (plen >= 4 + 24) {
            DalBuf *b = g_new0(DalBuf, 1);
            b->addr = ldl_le_p(p + 12);
            b->size = ddi == AUDIO_OP_WRITE ? ldl_le_p(p + 20)
                                            : ldl_le_p(p + 16);
            g_queue_push_tail(h->bufs, b);
            stream_arm(h);
        }
        break;
    case AUDIO_OP_CLOSE:
        stream_close(h);
        dal_event(h, ADSP_AUDIO_EVT_STATUS_CLOSE, 0);
        break;
    case AUDIO_OP_FLUSH:
        stream_close(h);
        break;
    default:
        break;
    }
}

/* Called with the bytes the apps processor wrote to a DSP DAL channel */
void msm_qdsp6_dal_stream(MsmModemState *s, SmdChan *c, GByteArray *acc)
{
    char chan[24];

    snprintf(chan, sizeof(chan), "%s", c->name);
    while (acc->len >= DAL_HDR_SIZE) {
        uint32_t len = ldl_le_p(acc->data) & 0xFFFF;
        if (len < DAL_HDR_SIZE) {
            g_byte_array_set_size(acc, 0);
            break;
        }
        if (acc->len < len) {
            break;
        }
        dal_handle_msg(s, chan, c->ctype & 0xFF, acc->data, len);
        g_byte_array_remove_range(acc, 0, len);
    }
}

void msm_qdsp6_reset(MsmModemState *s)
{
    for (int i = 0; i < DAL_MAX_HANDLES; i++) {
        if (handles[i].used) {
            stream_close(&handles[i]);
        }
        handles[i].used = false;
    }
}
