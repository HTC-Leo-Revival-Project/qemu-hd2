/*
 * BCM4329 FM receiver core
 *
 * The host reaches it through Broadcom HCI vendor command 0xFC15:
 *   params  reg, op (0 write / 1 read), data...
 *   result  status, reg, op, data (reads return two bytes: reg, reg + 1)
 * The register map is Broadcom's FM block (the one the BCM2048 exposes on
 * I2C):
 *   0x00 FM_RDS_SYSTEM   bit 0 FM on, bit 1 RDS on
 *   0x01 FM_CTRL         bit 0 band: 0 = 87.5-108 MHz, 1 = 76-90 MHz
 *   0x05 AUDIO_CTRL0     bit 0 RF mute, bit 1 manual mute, bits 2/3 DAC
 *                        left/right, bit 4 route to DAC, bit 5 route to I2S
 *   0x07 SEARCH_CTRL0    bit 7 search up, bits 6:0 RSSI threshold
 *   0x09 SEARCH_TUNE_MODE 0 stop, 1 tune to FREQ, 2 seek, 3 AF jump
 *   0x0a/0x0b FREQ       frequency in kHz - 64000 (little endian)
 *   0x0c/0x0d AF_FREQ    the same, for AF jumps
 *   0x0f RSSI            signal strength of the tuned channel (0..127)
 *   0x10/0x11 MASK0/1    interrupt enables for FLAG0/1
 *   0x12/0x13 FLAG0/1    FLAG0: 0x01 search/tune finished, 0x02 failed,
 *                        0x20 stereo detected; cleared when read
 *   0xf8/0xf9 VOLUME     audio volume, 0..0x100 (little endian)
 * When a flag enabled in MASK0/1 is set, the chip sends the HCI vendor event
 * 0xFF with sub-code 0x08.
 *
 * The audio output (the chip's analog line out, which the HD2 feeds into
 * the codec's aux input) is what is on air at the tuned frequency: a
 * station's programme, or static between stations.  Stations come from the
 * fm-stations property, "MHZ=SOURCE;MHZ=SOURCE;...", where SOURCE is
 * "tone:HZ" or anything ffmpeg can decode (a file, which loops, or a stream
 * URL).  Without the property a few test stations are on air.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "qemu/bswap.h"
#include <math.h>
#include <glib-unix.h>
#include "bcm4329_bt.h"

#define REG_SYSTEM      0x00
#define REG_CTRL        0x01
#define REG_AUDIO_CTRL0 0x05
#define REG_SEARCH_CTRL0 0x07
#define REG_TUNE_MODE   0x09
#define REG_FREQ        0x0a
#define REG_AF_FREQ     0x0c
#define REG_RSSI        0x0f
#define REG_MASK0       0x10
#define REG_FLAG0       0x12
#define REG_FLAG1       0x13
#define REG_VOLUME      0xf8

#define SYS_FM_ON       0x01
#define CTRL_JAPAN      0x01
#define AUDIO_RF_MUTE   0x01
#define AUDIO_MUTE      0x02
#define AUDIO_DAC       0x10

#define FLAG_DONE       0x01
#define FLAG_FAIL       0x02
#define FLAG_STEREO     0x20

#define MODE_STOP       0
#define MODE_TUNE       1
#define MODE_SEEK       2
#define MODE_AF_JUMP    3

#define FREQ_BASE       64000   /* kHz */
#define STEP_KHZ        100
#define STATION_BW      100     /* kHz either side with some signal */
#define RSSI_STATION    75
#define RSSI_NOISE      12

#define RATE            48000
#define TUNE_NS         (20 * SCALE_MS)
#define SEEK_STEP_NS    (8 * SCALE_MS)

typedef struct FmStation {
    int khz;
    double tone_hz;             /* > 0: synthesized */
    char *source;               /* else decoded by ffmpeg */
    GPid pid;
    int fd;
    GByteArray *pcm;
    double phase, env_phase;
} FmStation;

struct Bcm4329Fm {
    uint8_t reg[256];
    int tuned_khz;              /* what the receiver is on, 0 = nothing yet */
    QEMUTimer *op_timer;
    Bcm4329FmIrq irq;
    void *opaque;
    GArray *stations;           /* FmStation */
    FmStation *playing;
    uint32_t noise;
};

/* ---------------------------------------------------------------- */
/* Stations                                                          */
/* ---------------------------------------------------------------- */

static const char *default_stations =
    "88.1=tone:440;94.7=tone:660;101.5=tone:523.25;105.9=tone:349.23";

static void fm_parse_stations(Bcm4329Fm *fm, const char *spec)
{
    g_auto(GStrv) items = g_strsplit(spec, ";", -1);

    for (int i = 0; items[i]; i++) {
        char *eq = strchr(items[i], '=');
        FmStation st = { .fd = -1 };
        double mhz;

        if (!eq) {
            continue;
        }
        *eq = 0;
        mhz = g_ascii_strtod(g_strstrip(items[i]), NULL);
        if (mhz < 64 || mhz > 108.1) {
            continue;
        }
        st.khz = (int)lround(mhz * 1000);
        eq = g_strstrip(eq + 1);
        if (g_str_has_prefix(eq, "tone:")) {
            st.tone_hz = g_ascii_strtod(eq + 5, NULL);
        } else {
            st.source = g_strdup(eq);
        }
        st.pcm = g_byte_array_new();
        g_array_append_val(fm->stations, st);
    }
}

static FmStation *fm_station_near(Bcm4329Fm *fm, int khz, int *off)
{
    FmStation *best = NULL;
    int best_off = INT_MAX;

    for (guint i = 0; i < fm->stations->len; i++) {
        FmStation *st = &g_array_index(fm->stations, FmStation, i);
        int d = abs(st->khz - khz);
        if (d < STATION_BW && d < best_off) {
            best = st;
            best_off = d;
        }
    }
    if (off) {
        *off = best ? best_off : STATION_BW;
    }
    return best;
}

static int fm_rssi_at(Bcm4329Fm *fm, int khz)
{
    int off;

    if (!fm_station_near(fm, khz, &off)) {
        return RSSI_NOISE;
    }
    /* falls off linearly to the noise floor at STATION_BW */
    return RSSI_NOISE + (RSSI_STATION - RSSI_NOISE) * (STATION_BW - off) /
           STATION_BW;
}

static void fm_source_stop(FmStation *st)
{
    if (st->fd >= 0) {
        qemu_close(st->fd);
        st->fd = -1;
    }
    if (st->pid) {
        kill(st->pid, SIGTERM);
        g_spawn_close_pid(st->pid);
        st->pid = 0;
    }
    g_byte_array_set_size(st->pcm, 0);
}

static void fm_child_exited(GPid pid, gint status, gpointer data)
{
    g_spawn_close_pid(pid);
}

static void fm_source_start(FmStation *st)
{
    g_autoptr(GError) err = NULL;
    bool remote = strstr(st->source, "://") != NULL;
    const char *argv[] = {
        "ffmpeg", "-nostdin", "-loglevel", "error",
        remote ? "-reconnect" : "-stream_loop", remote ? "1" : "-1",
        "-i", st->source, "-vn", "-f", "s16le", "-ac", "2",
        "-ar", G_STRINGIFY(RATE), "pipe:1", NULL
    };
    GPid pid;
    int out;

    if (!g_spawn_async_with_pipes(NULL, (char **)argv, NULL,
                                  G_SPAWN_SEARCH_PATH |
                                  G_SPAWN_DO_NOT_REAP_CHILD |
                                  G_SPAWN_STDERR_TO_DEV_NULL,
                                  NULL, NULL, &pid, NULL, &out, NULL, &err)) {
        qemu_log_mask(LOG_UNIMP, "bcm4329-fm: cannot run ffmpeg for %s: %s\n",
                      st->source, err->message);
        return;
    }
    g_unix_set_fd_nonblocking(out, true, NULL);
    g_child_watch_add(pid, fm_child_exited, NULL);
    st->pid = pid;
    st->fd = out;
}

/* switch the station whose programme is heard */
static void fm_select(Bcm4329Fm *fm, FmStation *st)
{
    if (fm->playing == st) {
        return;
    }
    if (fm->playing && fm->playing->source) {
        fm_source_stop(fm->playing);
    }
    fm->playing = st;
    if (st && st->source) {
        fm_source_start(st);
    }
}

/* ---------------------------------------------------------------- */
/* Tuning                                                            */
/* ---------------------------------------------------------------- */

static void fm_band(Bcm4329Fm *fm, int *lo, int *hi)
{
    if (fm->reg[REG_CTRL] & CTRL_JAPAN) {
        *lo = 76000;
        *hi = 90000;
    } else {
        *lo = 87500;
        *hi = 108000;
    }
}

static int fm_reg_khz(Bcm4329Fm *fm, int reg)
{
    return FREQ_BASE + lduw_le_p(&fm->reg[reg]);
}

static void fm_set_tuned(Bcm4329Fm *fm, int khz)
{
    int off;
    FmStation *st = fm_station_near(fm, khz, &off);

    fm->tuned_khz = khz;
    stw_le_p(&fm->reg[REG_FREQ], khz - FREQ_BASE);
    fm->reg[REG_RSSI] = fm_rssi_at(fm, khz);
    fm_select(fm, st);
}

static void fm_flags(Bcm4329Fm *fm, uint8_t f0)
{
    fm->reg[REG_FLAG0] |= f0;
    if ((fm->reg[REG_FLAG0] & fm->reg[REG_MASK0]) ||
        (fm->reg[REG_FLAG1] & fm->reg[REG_MASK0 + 1])) {
        fm->irq(fm->opaque);
    }
}

static void fm_op_done(void *opaque)
{
    Bcm4329Fm *fm = opaque;
    uint8_t mode = fm->reg[REG_TUNE_MODE];
    int lo, hi;

    fm->reg[REG_TUNE_MODE] = MODE_STOP;
    fm_band(fm, &lo, &hi);
    switch (mode) {
    case MODE_TUNE:
    case MODE_AF_JUMP: {
        int khz = fm_reg_khz(fm, mode == MODE_TUNE ? REG_FREQ : REG_AF_FREQ);
        if (khz < lo || khz > hi) {
            fm_flags(fm, FLAG_DONE | FLAG_FAIL);
            return;
        }
        fm_set_tuned(fm, khz);
        fm_flags(fm, FLAG_DONE |
                 (fm->reg[REG_RSSI] > RSSI_NOISE ? FLAG_STEREO : 0));
        break;
    }
    case MODE_SEEK: {
        bool up = fm->reg[REG_SEARCH_CTRL0] & 0x80;
        int thr = fm->reg[REG_SEARCH_CTRL0] & 0x7F;
        int start = fm->tuned_khz ? fm->tuned_khz : fm_reg_khz(fm, REG_FREQ);
        int khz = start;

        for (int n = (hi - lo) / STEP_KHZ + 1; n > 0; n--) {
            khz += up ? STEP_KHZ : -STEP_KHZ;
            if (khz > hi) {
                khz = lo;
            } else if (khz < lo) {
                khz = hi;
            }
            if (fm_rssi_at(fm, khz) >= MAX(thr, RSSI_NOISE + 1)) {
                fm_set_tuned(fm, khz);
                fm_flags(fm, FLAG_DONE | FLAG_STEREO);
                return;
            }
        }
        fm_flags(fm, FLAG_DONE | FLAG_FAIL);
        break;
    }
    }
}

static void fm_start_op(Bcm4329Fm *fm)
{
    uint8_t mode = fm->reg[REG_TUNE_MODE];
    int64_t delay = TUNE_NS;

    timer_del(fm->op_timer);
    if (!(fm->reg[REG_SYSTEM] & SYS_FM_ON) || mode == MODE_STOP) {
        fm->reg[REG_TUNE_MODE] = MODE_STOP;
        return;
    }
    if (mode == MODE_SEEK) {
        /* the receiver dwells on each channel it passes */
        int lo, hi, from = fm->tuned_khz ? fm->tuned_khz : 87500;
        int to = from, steps = 0;
        bool up = fm->reg[REG_SEARCH_CTRL0] & 0x80;
        int thr = fm->reg[REG_SEARCH_CTRL0] & 0x7F;

        fm_band(fm, &lo, &hi);
        do {
            to += up ? STEP_KHZ : -STEP_KHZ;
            to = to > hi ? lo : to < lo ? hi : to;
            steps++;
        } while (to != from && fm_rssi_at(fm, to) < MAX(thr, RSSI_NOISE + 1) &&
                 steps < 300);
        delay = steps * SEEK_STEP_NS;
    }
    timer_mod(fm->op_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay);
}

/* ---------------------------------------------------------------- */
/* Register interface                                                */
/* ---------------------------------------------------------------- */

static void fm_power(Bcm4329Fm *fm, bool on)
{
    timer_del(fm->op_timer);
    if (!on) {
        fm_select(fm, NULL);
        fm->tuned_khz = 0;
    }
}

int bcm4329_fm_access(Bcm4329Fm *fm, uint8_t reg, bool write,
                      const uint8_t *data, int len, uint8_t *out)
{
    if (!write) {
        out[0] = fm->reg[reg];
        out[1] = fm->reg[(uint8_t)(reg + 1)];
        if (reg == REG_FLAG0 || reg == REG_FLAG1) {
            fm->reg[reg] = 0;
            if (reg == REG_FLAG0) {
                fm->reg[REG_FLAG1] = 0;
            }
        }
        return 2;
    }
    for (int i = 0; i < len; i++) {
        uint8_t r = reg + i;
        uint8_t old = fm->reg[r];

        if (r == REG_FLAG0 || r == REG_FLAG1 || r == REG_RSSI) {
            continue;                   /* read-only */
        }
        fm->reg[r] = data[i];
        if (r == REG_SYSTEM && ((old ^ data[i]) & SYS_FM_ON)) {
            fm_power(fm, data[i] & SYS_FM_ON);
        }
    }
    if (reg <= REG_TUNE_MODE && reg + len > REG_TUNE_MODE) {
        fm_start_op(fm);
    }
    return 0;
}

void bcm4329_fm_reset(Bcm4329Fm *fm)
{
    fm_power(fm, false);
    memset(fm->reg, 0, sizeof(fm->reg));
    stw_le_p(&fm->reg[REG_FREQ], 87500 - FREQ_BASE);
    stw_le_p(&fm->reg[REG_VOLUME], 0x100);
    fm->reg[REG_AUDIO_CTRL0] = AUDIO_DAC | 0x0C;
    fm->reg[REG_RSSI] = RSSI_NOISE;
}

/* ---------------------------------------------------------------- */
/* Audio                                                             */
/* ---------------------------------------------------------------- */

static int16_t fm_static(Bcm4329Fm *fm)
{
    /* xorshift white noise, low-passed a little by averaging */
    int32_t a, b;

    fm->noise ^= fm->noise << 13;
    fm->noise ^= fm->noise >> 17;
    fm->noise ^= fm->noise << 5;
    a = (int16_t)fm->noise;
    b = (int16_t)(fm->noise >> 16);
    return (a + b) / 6;
}

static void fm_station_pcm(FmStation *st, int16_t *buf, int frames)
{
    if (st->tone_hz > 0) {
        for (int i = 0; i < frames; i++) {
            /* a 2 Hz tremolo so a test station is easy to recognise */
            double env = 0.75 + 0.25 * sin(st->env_phase);
            int16_t v = (int16_t)(9000 * env * sin(st->phase));
            buf[2 * i] = buf[2 * i + 1] = v;
            st->phase = fmod(st->phase + 2 * M_PI * st->tone_hz / RATE,
                             2 * M_PI);
            st->env_phase = fmod(st->env_phase + 2 * M_PI * 2 / RATE,
                                 2 * M_PI);
        }
        return;
    }
    if (st->fd >= 0) {
        int want = frames * 4;
        while ((int)st->pcm->len < want) {
            uint8_t tmp[16384];
            ssize_t n = read(st->fd, tmp, sizeof(tmp));
            if (n <= 0) {
                break;
            }
            g_byte_array_append(st->pcm, tmp, n);
        }
    }
    {
        int have = MIN((int)st->pcm->len, frames * 4) & ~3;
        memcpy(buf, st->pcm->data, have);
        memset((uint8_t *)buf + have, 0, frames * 4 - have);
        g_byte_array_remove_range(st->pcm, 0, have);
    }
}

void bcm4329_fm_audio(Bcm4329Fm *fm, int16_t *buf, int frames)
{
    uint8_t ac = fm->reg[REG_AUDIO_CTRL0];
    int vol = MIN(lduw_le_p(&fm->reg[REG_VOLUME]), 0x100);
    int off = STATION_BW;
    FmStation *st = NULL;

    if (!(fm->reg[REG_SYSTEM] & SYS_FM_ON) || !fm->tuned_khz ||
        (ac & (AUDIO_MUTE | AUDIO_RF_MUTE)) || !(ac & AUDIO_DAC)) {
        memset(buf, 0, frames * 4);
        return;
    }
    st = fm_station_near(fm, fm->tuned_khz, &off);
    if (st) {
        fm_station_pcm(st, buf, frames);
    } else {
        memset(buf, 0, frames * 4);
    }
    /* signal fades into static away from the carrier */
    for (int i = 0; i < frames * 2; i++) {
        int32_t sig = st ? buf[i] * (STATION_BW - off) / STATION_BW : 0;
        int32_t hiss = fm_static(fm) * off / STATION_BW;
        int32_t v = (sig + hiss) * vol / 0x100;
        buf[i] = MAX(-32768, MIN(32767, v));
    }
}

Bcm4329Fm *bcm4329_fm_new(const char *stations, Bcm4329FmIrq irq,
                          void *opaque)
{
    Bcm4329Fm *fm = g_new0(Bcm4329Fm, 1);

    fm->irq = irq;
    fm->opaque = opaque;
    fm->noise = 0x12345678;
    fm->stations = g_array_new(false, true, sizeof(FmStation));
    fm_parse_stations(fm, stations && *stations ? stations
                                                : default_stations);
    fm->op_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, fm_op_done, fm);
    bcm4329_fm_reset(fm);
    return fm;
}
