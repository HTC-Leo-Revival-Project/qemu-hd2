/*
 * HTC HD2 baseband: gpsOne position engine (PDAPI over ONC RPC)
 *
 * The GPS HAL (libgps "gps_leo", device/htc/leo/libgps/leo-gps-rpc.c) is a
 * client of the modem's PDAPI program 0x3000005b and serves the callback
 * program 0x3100005b.  Each pdsm_get_position() call (proc 0xb) is answered,
 * about a second later, with a position-determination event (callback
 * proc 1) carrying POSITION, VELOCITY, HEIGHT and DONE; DONE makes the HAL
 * ask for the next fix, so the phone sees a 1 Hz fix.
 *
 * The callback argument layout (32-bit big-endian words) follows the HAL:
 *   [2] event mask        [8] GPS time (s)        [60..63] lat/lon * 1e8
 *   [64] altitude * 10    [66] speed km/h * 10    [67] heading * 10
 *   [75] HDOP * 20        [77] SVs used (mask)    [82] SV count
 *   [83 + 3i] PRN, elevation, azimuth * 100 + SNR
 *
 * The position comes from the modem properties gps-latitude, gps-longitude
 * and gps-altitude (default: 37.4220 N, 122.0841 W, 10 m).  It can be moved
 * at run time, like the AOSP emulator's "geo fix":
 *   qom-set /machine/modem gps-fix LAT,LON[,ALT]
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "htcleo.h"
#include "msm_modem.h"

#define PDAPI_CB_PROG       0x3100005b
#define PDAPI_CB_VERS       0x00010001

#define EV_POSITION         0x1
#define EV_VELOCITY         0x2
#define EV_HEIGHT           0x4
#define EV_DONE             0x8

#define GPS_EPOCH           315964800       /* 1980-01-06 in Unix time */
#define HAL_LEAP_SECONDS    15              /* what the HAL subtracts */

typedef struct MsmGps {
    QEMUTimer *timer;
    bool session;
    double lat, lon, alt;
} MsmGps;

/* a constellation that looks plausible from anywhere: PRN, elev, azim, SNR */
static const uint16_t sats[][4] = {
    { 2, 62, 40, 42 }, { 5, 35, 110, 38 }, { 12, 48, 200, 40 },
    { 15, 22, 290, 33 }, { 21, 75, 330, 44 }, { 25, 15, 160, 30 },
    { 29, 40, 250, 37 }, { 31, 28, 70, 35 },
};

static void gps_send_fix(void *opaque)
{
    MsmModemState *s = opaque;
    MsmGps *g = s->gps;
    uint32_t a[83 + 3 * 12] = { 0 };
    int64_t lat = llround(g->lat * 1e8), lon = llround(g->lon * 1e8);
    int n = ARRAY_SIZE(sats);

    if (!g->session) {
        return;
    }
    g->session = false;
    a[2] = EV_POSITION | EV_VELOCITY | EV_HEIGHT | EV_DONE;
    a[8] = (uint32_t)(time(NULL) - GPS_EPOCH + HAL_LEAP_SECONDS);
    a[60] = (uint32_t)(lat >> 32);
    a[61] = (uint32_t)lat;
    a[62] = (uint32_t)(lon >> 32);
    a[63] = (uint32_t)lon;
    a[64] = (uint32_t)(int32_t)(g->alt * 10);
    a[66] = 0;                              /* standing still */
    a[67] = 0;
    a[75] = 18;                             /* HDOP 0.9 */
    a[77] = (1u << n) - 1;
    a[82] = n;
    for (int i = 0; i < n; i++) {
        a[83 + 3 * i] = sats[i][0];
        a[84 + 3 * i] = sats[i][1];
        a[85 + 3 * i] = sats[i][2] * 100 + sats[i][3];
    }
    for (size_t i = 0; i < ARRAY_SIZE(a); i++) {
        a[i] = cpu_to_be32(a[i]);
    }
    msm_rpc_apps_call(s, PDAPI_CB_PROG, PDAPI_CB_VERS, 1, a, ARRAY_SIZE(a));
}

uint32_t msm_gps_pdapi_call(MsmModemState *s, uint32_t proc,
                            const uint8_t *args, uint32_t len)
{
    MsmGps *g = s->gps;
    uint32_t a0 = len >= 4 ? ldl_be_p(args) : 0;

    switch (proc) {
    case 0x2:                               /* pdsm_client_init(type) */
        return 0x0A00 + a0;                 /* client handle */
    case 0xb:                               /* pdsm_get_position */
        g->session = true;
        timer_mod(g->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  NANOSECONDS_PER_SECOND);
        return 1;
    case 0xc:                               /* pdsm_client_end_session */
        g->session = false;
        timer_del(g->timer);
        return 1;
    }
    return 0;
}

static double prop_or(const char *v, double def)
{
    return v && *v ? g_ascii_strtod(v, NULL) : def;
}

static char *gps_fix_get(Object *obj, Error **errp)
{
    MsmGps *g = MSM_MODEM(obj)->gps;

    return g_strdup_printf("%.7f,%.7f,%.1f", g->lat, g->lon, g->alt);
}

static void gps_fix_set(Object *obj, const char *value, Error **errp)
{
    MsmGps *g = MSM_MODEM(obj)->gps;
    g_auto(GStrv) f = g_strsplit(value, ",", 3);
    double lat, lon;

    if (g_strv_length(f) < 2) {
        error_setg(errp, "gps-fix: expected LAT,LON[,ALT]");
        return;
    }
    lat = g_ascii_strtod(f[0], NULL);
    lon = g_ascii_strtod(f[1], NULL);
    if (fabs(lat) > 90 || fabs(lon) > 180) {
        error_setg(errp, "gps-fix: latitude or longitude out of range");
        return;
    }
    g->lat = lat;
    g->lon = lon;
    if (f[2]) {
        g->alt = g_ascii_strtod(f[2], NULL);
    }
}

void msm_gps_reset(MsmModemState *s)
{
    s->gps->session = false;
    timer_del(s->gps->timer);
}

void msm_gps_init(MsmModemState *s)
{
    MsmGps *g = g_new0(MsmGps, 1);

    s->gps = g;
    g->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gps_send_fix, s);
    g->lat = prop_or(s->gps_lat, 37.4220);
    g->lon = prop_or(s->gps_lon, -122.0841);
    g->alt = prop_or(s->gps_alt, 10.0);
    object_property_add_str(OBJECT(s), "gps-fix", gps_fix_get, gps_fix_set);
}
