/*
 * Broadcom BCM4329 WLAN chip (SDIO), as driven by the dhd/bcm4329 driver of
 * the HTC HD2 kernels (drivers/net/wireless/bcm4329, DHD 4.218.x).
 *
 * The SDIO side is modelled at register level: CCCR/FBR/CIS, function 1
 * with its backplane window (Sonics SB backbone with chipcommon, d11, SDIO
 * device, ARM CM3 and SOCRAM cores), function 2 frame FIFO, out-of-band
 * interrupt pin.  The driver downloads the firmware and NVRAM into SOCRAM
 * and starts the ARM core exactly as on hardware.
 *
 * The firmware itself is replaced by a model of its host protocol: SDPCM
 * framing with credits, CDC ioctls (WLC_* commands and iovars), BDC data
 * frames and Broadcom event packets.  It offers one open access point,
 * "HD2-QEMU" on channel 6, and once the host associates, the data frames
 * are bridged to a QEMU network backend: with the default user-mode
 * network the phone gets 10.0.2.15 by DHCP and real internet access.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "qemu/cutils.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "net/net.h"
#include "htcleo.h"
#include "msm_sdcc.h"
#include "bcm4329.h"

OBJECT_DECLARE_SIMPLE_TYPE(Bcm4329State, BCM4329)

#define DPRINTF(fmt, ...) \
    qemu_log_mask(LOG_TRACE, "bcm4329: " fmt, ## __VA_ARGS__)

/* ---- chip constants ------------------------------------------------------ */

#define RAM_SIZE            0x48000         /* 288 KiB SOCRAM */
#define SI_ENUM_BASE        0x18000000u
#define SI_CORE_SIZE        0x1000u
#define NCORES              5

enum { CORE_CC, CORE_D11, CORE_SDIOD, CORE_ARM, CORE_SOCRAM };

static const struct { uint16_t id; uint8_t rev; } core_info[NCORES] = {
    [CORE_CC]     = { 0x800, 20 },
    [CORE_D11]    = { 0x812, 16 },
    [CORE_SDIOD]  = { 0x829, 7 },
    [CORE_ARM]    = { 0x82a, 2 },
    [CORE_SOCRAM] = { 0x80e, 3 },
};

#define CHIPID              (0x4329 | (3u << 16) | (NCORES << 24))
#define CC_CAP_PMU          0x10000000u

/* SB config space (at 0xf00 in every core) */
#define SB_TMSTATELOW       0xf98
#define SB_TMSTATEHIGH      0xf9c
#define SB_ADMATCH0         0xfb0
#define SB_IDLOW            0xff8
#define SB_IDHIGH           0xffc
#define SBTML_RESET         0x1
#define SBTML_REJ           0x6
#define SICF_CLOCK_EN       0x10000

/* SDIO device core */
#define SD_CORECONTROL      0x000
#define SD_INTSTATUS        0x020
#define SD_HOSTINTMASK      0x024
#define SD_TOSBMAILBOX      0x040
#define SD_TOHOSTMAILBOX    0x044
#define SD_TOSBMAILBOXDATA  0x048
#define SD_TOHOSTMAILBOXDATA 0x04c

#define I_HMB_FC_STATE      (1u << 4)
#define I_HMB_FRAME_IND     (1u << 6)
#define I_HMB_HOST_INT      (1u << 7)
#define HMB_DATA_DEVREADY   2
#define HMB_DATA_FWREADY    8
#define SDPCM_PROT_VERSION  4

/* function 1 registers */
#define F1_SBADDRLOW        0x1000A
#define F1_SBADDRMID        0x1000B
#define F1_SBADDRHIGH       0x1000C
#define F1_FRAMECTRL        0x1000D
#define F1_CHIPCLKCSR       0x1000E
#define SFC_RF_TERM         (1u << 0)
#define CLK_AVBITS          0xC0

/* CCCR */
#define CCCR_IOEN           0x02
#define CCCR_IORDY          0x03
#define CCCR_INTEN          0x04
#define CCCR_INTPEND        0x05
#define CCCR_BUSIF          0x07
#define CCCR_SPEED          0x13
#define CCCR_BRCM_SEPINT    0xF2
#define SEPINT_ACT_HI       0x04

/* SDPCM */
#define SDPCM_HDRLEN        12
#define CHAN_CONTROL        0
#define CHAN_EVENT          1
#define CHAN_DATA           2
#define BDC_HDRLEN          4

/* WLC ioctls */
#define WLC_GET_MAGIC       0
#define WLC_GET_VERSION     1
#define WLC_UP              2
#define WLC_DOWN            3
#define WLC_GET_RATE        12
#define WLC_GET_INFRA       19
#define WLC_GET_BSSID       23
#define WLC_GET_SSID        25
#define WLC_SET_SSID        26
#define WLC_GET_CHANNEL     29
#define WLC_GET_RADIO       37
#define WLC_GET_PHYTYPE     39
#define WLC_SCAN            50
#define WLC_SCAN_RESULTS    51
#define WLC_DISASSOC        52
#define WLC_REASSOC         53
#define WLC_GET_CURR_RATESET 114
#define WLC_GET_RSSI        127
#define WLC_GET_PHY_NOISE   135
#define WLC_GET_BSS_INFO    136
#define WLC_GET_BAND        141
#define WLC_GET_ASSOCLIST   159
#define WLC_GET_VALID_CHANNELS 217
#define WLC_GET_VAR         262
#define WLC_SET_VAR         263
#define WLC_IOCTL_MAGIC     0x14e46c77

#define CDCF_IOC_ERROR      0x01
#define CDCF_IOC_SET        0x02
#define BCME_NOTASSOCIATED  (-17)
#define BCME_UNSUPPORTED    (-23)

/* events */
#define WLC_E_SET_SSID      0
#define WLC_E_AUTH          3
#define WLC_E_ASSOC         7
#define WLC_E_DISASSOC      11
#define WLC_E_LINK          16
#define WLC_E_NDIS_LINK     18
#define WLC_E_SCAN_COMPLETE 26
#define WLC_E_STATUS_NO_NETWORKS 3

#define BSS_INFO_LEN        124
#define AP_CHANNEL          6
#define AP_RSSI             (-45)
#define RX_QUEUE_MAX        256
/* real scans visit 13 channels; association takes a few frames */
#define SCAN_MS             1500
#define JOIN_MS             300

static const uint8_t ap_bssid[6] = { 0x02, 0x48, 0x44, 0x32, 0x51, 0x45 };
static const char ap_ssid[] = "HD2-QEMU";
static const uint8_t ap_rates[] = {
    0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6c,
};

struct Bcm4329State {
    DeviceState parent_obj;
    qemu_irq oob;
    DeviceState *sdcc;          /* the host controller (in-band interrupt) */
    NICState *nic;
    NICConf conf;
    QEMUTimer *timer;
    QEMUBH *flush_bh;           /* restart the backend after a stall */

    bool powered;
    uint16_t rca;
    /* CCCR / FBR */
    uint8_t ioen, ien, busif, speed, sepint;
    uint16_t blksize[3];
    /* function 1 */
    uint32_t sbwindow;
    uint8_t clkcsr, f1reg[0x20];
    /* backplane */
    uint8_t *ram;
    uint32_t core[NCORES][SI_CORE_SIZE / 4];
    uint32_t intstatus, hostintmask, tohostmbdata;
    bool fw_running;

    /* current CMD53 */
    bool xfer_active, xfer_write, xfer_incr;
    int xfer_fn;
    uint32_t xfer_addr, xfer_len, xfer_pos;
    GByteArray *xfer_buf;
    GByteArray *f2_tx;          /* host frame spanning several CMD53s */

    /* SDPCM */
    GQueue rxq;                 /* frames to the host (GByteArray) */
    uint32_t rx_pos;            /* read position in the head frame */
    uint8_t tx_seq;             /* our sequence to the host */
    uint8_t host_seq;           /* next sequence expected from the host */
    uint8_t adv_window;         /* credit window last advertised */

    /* 802.11 state */
    bool up, associated, scanning;
    int pending_join;
    uint8_t mac[6];
    GHashTable *iovars;         /* name -> GByteArray */
    int32_t pm;
};

/* ---- interrupts ------------------------------------------------------------ */

static bool bcm_irq_pending(Bcm4329State *s)
{
    return s->powered && (s->intstatus & s->hostintmask);
}

/*
 * The SDIO device core's interrupt goes out on the separate pin (the OOB
 * GPIO Android's driver uses) and, unless that pin is enabled in CCCR
 * 0xF2, in-band as function 1's interrupt when the host has enabled it
 * (CCCR 0x04: master and function 1 or 2): Windows Mobile's driver takes
 * it through the SDIO stack.
 */
static void bcm_update_irq(Bcm4329State *s)
{
    bool pending = bcm_irq_pending(s);
    bool hi = !(s->sepint & 1) || (s->sepint & SEPINT_ACT_HI);

    qemu_set_irq(s->oob, hi ? pending : !pending);
    if (s->sdcc) {
        msm_sdcc_sdio_irq(s->sdcc, pending && !(s->sepint & 1) &&
                          (s->ien & 1) && (s->ien & 0x06));
    }
}

/* ---- frames to the host -------------------------------------------------- */

static void bcm_queue_frame(Bcm4329State *s, int chan, const uint8_t *pre,
                            size_t prelen, const uint8_t *data, size_t len)
{
    GByteArray *f = g_byte_array_sized_new(SDPCM_HDRLEN + prelen + len);
    uint16_t flen = SDPCM_HDRLEN + prelen + len;
    uint8_t hdr[SDPCM_HDRLEN] = { 0 };

    if (g_queue_get_length(&s->rxq) >= RX_QUEUE_MAX) {
        g_byte_array_unref(f);
        return;
    }
    stw_le_p(hdr, flen);
    stw_le_p(hdr + 2, ~flen);
    hdr[4] = s->tx_seq++;
    hdr[5] = chan;
    hdr[6] = 0;                         /* no read-ahead length */
    hdr[7] = SDPCM_HDRLEN;              /* data offset */
    hdr[8] = 0;                         /* flow control: all on */
    hdr[9] = s->adv_window = s->host_seq + 16;  /* credit window */
    hdr[10] = SDPCM_PROT_VERSION;
    g_byte_array_append(f, hdr, sizeof(hdr));
    if (prelen) {
        g_byte_array_append(f, pre, prelen);
    }
    g_byte_array_append(f, data, len);
    g_queue_push_tail(&s->rxq, f);
    s->intstatus |= I_HMB_FRAME_IND;
    bcm_update_irq(s);
}

static void bcm_send_event(Bcm4329State *s, uint32_t type, uint32_t status,
                           uint16_t flags, const uint8_t *addr)
{
    uint8_t ev[14 + 10 + 48];
    uint8_t bdc[BDC_HDRLEN] = { 0x10, 0, 0, 0 };

    memset(ev, 0, sizeof(ev));
    memcpy(ev, s->mac, 6);                  /* ether dst */
    memcpy(ev + 6, s->mac, 6);              /* ether src */
    stw_be_p(ev + 12, 0x886c);              /* ETHER_TYPE_BRCM */
    stw_be_p(ev + 14, 0x8001);              /* BCMILCP_SUBTYPE_VENDOR_LONG */
    stw_be_p(ev + 16, 8 + 48);
    ev[18] = 0;
    ev[19] = 0x00;                          /* BRCM OUI 00:10:18 */
    ev[20] = 0x10;
    ev[21] = 0x18;
    stw_be_p(ev + 22, 1);                   /* BCMILCP_BCM_SUBTYPE_EVENT */
    stw_be_p(ev + 24, 1);                   /* wl_event_msg_t version */
    stw_be_p(ev + 26, flags);
    stl_be_p(ev + 28, type);
    stl_be_p(ev + 32, status);
    stl_be_p(ev + 36, 0);                   /* reason */
    stl_be_p(ev + 40, 0);                   /* auth type: open */
    stl_be_p(ev + 44, 0);                   /* datalen */
    if (addr) {
        memcpy(ev + 48, addr, 6);
    }
    memcpy(ev + 54, "eth0", 4);
    DPRINTF("event %u status %u flags %u\n", type, status, flags);
    bcm_queue_frame(s, CHAN_EVENT, bdc, sizeof(bdc), ev, sizeof(ev));
}

/* is event type enabled in the host's "event_msgs" mask? */
static bool bcm_event_enabled(Bcm4329State *s, uint32_t type)
{
    GByteArray *m = g_hash_table_lookup(s->iovars, "event_msgs");

    return m && type / 8 < m->len && (m->data[type / 8] >> (type % 8)) & 1;
}

/*
 * The link came up or went down.  The WinCE firmware also reports it as
 * WLC_E_NDIS_LINK, which is what Windows Mobile's driver waits for before
 * it indicates media connect to NDIS.
 */
static void bcm_link_event(Bcm4329State *s, bool up)
{
    bcm_send_event(s, WLC_E_LINK, 0, up, ap_bssid);
    if (bcm_event_enabled(s, WLC_E_NDIS_LINK)) {
        bcm_send_event(s, WLC_E_NDIS_LINK, 0, up, ap_bssid);
    }
}

/* ---- the access point -------------------------------------------------------- */

static void bcm_fill_bss_info(uint8_t *b)
{
    memset(b, 0, BSS_INFO_LEN);
    stl_le_p(b + 0, 108);                   /* WL_BSS_INFO_VERSION */
    stl_le_p(b + 4, BSS_INFO_LEN);
    memcpy(b + 8, ap_bssid, 6);
    stw_le_p(b + 14, 100);                  /* beacon period */
    stw_le_p(b + 16, 0x0401);               /* ESS, short slot: open */
    b[18] = strlen(ap_ssid);
    memcpy(b + 19, ap_ssid, strlen(ap_ssid));
    stl_le_p(b + 52, sizeof(ap_rates));
    memcpy(b + 56, ap_rates, sizeof(ap_rates));
    stw_le_p(b + 72, 0x2B00 | AP_CHANNEL);  /* 2.4 GHz, 20 MHz */
    b[76] = 1;                              /* DTIM period */
    stw_le_p(b + 78, (uint16_t)AP_RSSI);
    b[80] = (uint8_t)-92;                   /* noise */
    b[88] = AP_CHANNEL;                     /* ctl_ch */
    stw_le_p(b + 116, BSS_INFO_LEN);        /* ie_offset */
    stl_le_p(b + 120, 0);                   /* ie_length */
}

/* wl_scan_results_t with our one BSS; returns the length */
static size_t bcm_scan_results(uint8_t *b, size_t max)
{
    if (max < 12 + BSS_INFO_LEN) {
        return 0;
    }
    stl_le_p(b + 0, 12 + BSS_INFO_LEN);
    stl_le_p(b + 4, 108);
    stl_le_p(b + 8, 1);
    bcm_fill_bss_info(b + 12);
    return 12 + BSS_INFO_LEN;
}

static void bcm_disassociate(Bcm4329State *s)
{
    if (s->associated) {
        s->associated = false;
        bcm_send_event(s, WLC_E_DISASSOC, 0, 0, ap_bssid);
        bcm_link_event(s, false);
    }
}

static void bcm_timer(void *opaque)
{
    Bcm4329State *s = opaque;

    if (!s->fw_running) {
        return;
    }
    if (s->scanning) {
        s->scanning = false;
        bcm_send_event(s, WLC_E_SCAN_COMPLETE, 0, 0, NULL);
    }
    if (s->pending_join > 0) {
        s->associated = true;
        bcm_send_event(s, WLC_E_AUTH, 0, 0, ap_bssid);
        bcm_send_event(s, WLC_E_ASSOC, 0, 0, ap_bssid);
        bcm_link_event(s, true);
        bcm_send_event(s, WLC_E_SET_SSID, 0, 0, ap_bssid);
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    } else if (s->pending_join < 0) {
        bcm_send_event(s, WLC_E_SET_SSID, WLC_E_STATUS_NO_NETWORKS, 0, NULL);
    }
    s->pending_join = 0;
}

static void bcm_kick(Bcm4329State *s, int ms)
{
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ms * SCALE_MS);
}

/* ---- firmware: ioctls ----------------------------------------------------- */

static void iovar_set(Bcm4329State *s, const char *name, const uint8_t *v,
                      size_t len)
{
    GByteArray *a = g_byte_array_new();

    g_byte_array_append(a, v, len);
    g_hash_table_replace(s->iovars, g_strdup(name), a);
}

static int bcm_get_var(Bcm4329State *s, const char *name, uint8_t *buf,
                       size_t len)
{
    GByteArray *a;

    if (!strcmp(name, "cur_etheraddr")) {
        memcpy(buf, s->mac, MIN(len, 6));
    } else if (!strcmp(name, "ver")) {
        pstrcpy((char *)buf, len,
                "wl0: Nov 17 2012 15:53:34 version 4.218.248.23\n");
    } else if (!strcmp(name, "iscanresults")) {
        /* wl_iscan_results_t: status, then wl_scan_results_t */
        memset(buf, 0, len);
        if (len >= 4) {
            stl_le_p(buf, 0);           /* WL_SCAN_RESULTS_SUCCESS */
            bcm_scan_results(buf + 4, len - 4);
        }
    } else if (!strcmp(name, "country")) {
        memset(buf, 0, len);
        pstrcpy((char *)buf, MIN(len, 4), "US");
    } else if (!strcmp(name, "nmode")) {
        memset(buf, 0, len);
        stl_le_p(buf, 1);
    } else if ((a = g_hash_table_lookup(s->iovars, name))) {
        memset(buf, 0, len);
        memcpy(buf, a->data, MIN(len, a->len));
    } else {
        memset(buf, 0, len);
    }
    return 0;
}

static int bcm_set_var(Bcm4329State *s, const char *name, const uint8_t *v,
                       size_t len)
{
    if (!strcmp(name, "cur_etheraddr") && len >= 6) {
        memcpy(s->mac, v, 6);
    } else if (!strcmp(name, "iscan")) {
        /* wl_iscan_params_t: version, action (start/continue/abort) */
        if (len >= 6 && lduw_le_p(v + 4) != 3) {
            s->scanning = true;
            bcm_kick(s, SCAN_MS);
        }
        return 0;
    }
    iovar_set(s, name, v, len);
    return 0;
}

static int bcm_ioctl(Bcm4329State *s, uint32_t cmd, bool set, uint8_t *buf,
                     size_t len)
{
    char key[16];
    GByteArray *a;

    switch (cmd) {
    case WLC_GET_MAGIC:
        stl_le_p(buf, WLC_IOCTL_MAGIC);
        return 0;
    case WLC_GET_VERSION:
        stl_le_p(buf, 1);
        return 0;
    case WLC_UP:
        s->up = true;
        return 0;
    case WLC_DOWN:
        bcm_disassociate(s);
        s->up = false;
        return 0;
    case WLC_GET_RATE:
        stl_le_p(buf, s->associated ? 108 : 0);
        return 0;
    case WLC_GET_INFRA:
        stl_le_p(buf, 1);
        return 0;
    case WLC_GET_BSSID:
        memset(buf, 0, MIN(len, 6));
        if (!s->associated) {
            return BCME_NOTASSOCIATED;
        }
        memcpy(buf, ap_bssid, MIN(len, 6));
        return 0;
    case WLC_GET_SSID:
        memset(buf, 0, len);
        if (s->associated && len >= 36) {
            stl_le_p(buf, strlen(ap_ssid));
            memcpy(buf + 4, ap_ssid, strlen(ap_ssid));
        }
        return 0;
    case WLC_SET_SSID:
    case WLC_REASSOC:
        if (len >= 4) {
            uint32_t sl = MIN(ldl_le_p(buf), 32);

            if (sl == 0) {
                bcm_disassociate(s);
                return 0;
            }
            if (len >= 4 + sl && sl == strlen(ap_ssid) &&
                !memcmp(buf + 4, ap_ssid, sl)) {
                s->pending_join = 1;
            } else {
                bcm_disassociate(s);
                s->pending_join = -1;
            }
            bcm_kick(s, JOIN_MS);
        }
        return 0;
    case WLC_GET_CHANNEL:
        memset(buf, 0, len);
        if (len >= 12) {
            stl_le_p(buf, AP_CHANNEL);
            stl_le_p(buf + 4, AP_CHANNEL);
        }
        return 0;
    case WLC_GET_RADIO:
        stl_le_p(buf, 0);
        return 0;
    case WLC_GET_PHYTYPE:
        stl_le_p(buf, 4);                       /* WLC_PHY_TYPE_N */
        return 0;
    case WLC_SCAN:
        s->scanning = true;
        bcm_kick(s, SCAN_MS);
        return 0;
    case WLC_SCAN_RESULTS:
        memset(buf, 0, len);
        bcm_scan_results(buf, len);
        return 0;
    case WLC_DISASSOC:
        bcm_disassociate(s);
        return 0;
    case WLC_GET_CURR_RATESET:
        memset(buf, 0, len);
        if (len >= 4 + sizeof(ap_rates)) {
            stl_le_p(buf, sizeof(ap_rates));
            memcpy(buf + 4, ap_rates, sizeof(ap_rates));
        }
        return 0;
    case WLC_GET_RSSI:
        stl_le_p(buf, s->associated ? AP_RSSI : 0);
        return 0;
    case WLC_GET_PHY_NOISE:
        stl_le_p(buf, (uint32_t)-92);
        return 0;
    case WLC_GET_BSS_INFO:
        memset(buf, 0, len);
        if (!s->associated) {
            return BCME_NOTASSOCIATED;
        }
        if (len >= 4 + BSS_INFO_LEN) {
            stl_le_p(buf, BSS_INFO_LEN);
            bcm_fill_bss_info(buf + 4);
        }
        return 0;
    case WLC_GET_BAND:
    case WLC_GET_ASSOCLIST:
        memset(buf, 0, MIN(len, 4));
        return 0;
    case WLC_GET_VALID_CHANNELS:
        memset(buf, 0, len);
        if (len >= 4 + 4 * 11) {
            stl_le_p(buf, 11);
            for (int i = 0; i < 11; i++) {
                stl_le_p(buf + 4 + 4 * i, i + 1);
            }
        }
        return 0;
    case WLC_GET_VAR:
    case WLC_SET_VAR: {
        size_t nl = strnlen((char *)buf, len);
        char *name;
        int ret;

        if (nl >= len) {
            return BCME_UNSUPPORTED;
        }
        name = g_strndup((char *)buf, nl);
        if (cmd == WLC_SET_VAR) {
            ret = bcm_set_var(s, name, buf + nl + 1, len - nl - 1);
        } else {
            /* the reply replaces the request (name and parameters) */
            ret = bcm_get_var(s, name, buf, len);
        }
        DPRINTF("%s \"%s\" -> %d\n", cmd == WLC_SET_VAR ? "set" : "get",
                name, ret);
        g_free(name);
        return ret;
    }
    }

    /* everything else: remember sets, echo them back on gets (GET = SET-1) */
    snprintf(key, sizeof(key), "#%u", set ? cmd : cmd + 1);
    if (set) {
        iovar_set(s, key, buf, MIN(len, 64));
        DPRINTF("ioctl set %u len %zu\n", cmd, len);
        return 0;
    }
    a = g_hash_table_lookup(s->iovars, key);
    memset(buf, 0, len);
    if (a) {
        memcpy(buf, a->data, MIN(len, a->len));
    }
    DPRINTF("ioctl get %u len %zu\n", cmd, len);
    return 0;
}

/*
 * ---- firmware: NDIS OIDs ----------------------------------------------------
 *
 * The Windows Mobile driver (bcmsddhd) hands NDIS OIDs to the dongle as CDC
 * commands: the WinCE firmware answers the 802.11 OIDs itself, and runs
 * OIDs WL_OID_BASE + n as wl ioctl n.  A query's reply carries the bytes
 * written; a failure sets CDCF_IOC_ERROR with the NDIS status in the status
 * word and the bytes needed in the length.
 */
#define WL_OID_BASE             0xFFE41420u

#define OID_802_3_PERMANENT_ADDRESS     0x01010101u
#define OID_802_3_CURRENT_ADDRESS       0x01010102u
#define OID_802_11_BSSID                0x0D010101u
#define OID_802_11_SSID                 0x0D010102u
#define OID_802_11_INFRASTRUCTURE_MODE  0x0D010108u
#define OID_802_11_ADD_WEP              0x0D010113u
#define OID_802_11_REMOVE_WEP           0x0D010114u
#define OID_802_11_DISASSOCIATE         0x0D010115u
#define OID_802_11_AUTHENTICATION_MODE  0x0D010118u
#define OID_802_11_PRIVACY_FILTER       0x0D010119u
#define OID_802_11_BSSID_LIST_SCAN      0x0D01011Au
#define OID_802_11_ENCRYPTION_STATUS    0x0D01011Bu
#define OID_802_11_RELOAD_DEFAULTS      0x0D01011Cu
#define OID_802_11_ADD_KEY              0x0D01011Du
#define OID_802_11_REMOVE_KEY           0x0D01011Eu
#define OID_802_11_ASSOCIATION_INFORMATION 0x0D01011Fu
#define OID_802_11_NETWORK_TYPES_SUPPORTED 0x0D010203u
#define OID_802_11_NETWORK_TYPE_IN_USE  0x0D010204u
#define OID_802_11_TX_POWER_LEVEL       0x0D010205u
#define OID_802_11_RSSI                 0x0D010206u
#define OID_802_11_FRAGMENTATION_THRESHOLD 0x0D010209u
#define OID_802_11_RTS_THRESHOLD        0x0D01020Au
#define OID_802_11_SUPPORTED_RATES      0x0D01020Eu
#define OID_802_11_CONFIGURATION        0x0D010211u
#define OID_802_11_POWER_MODE           0x0D010216u
#define OID_802_11_BSSID_LIST           0x0D010217u

#define NDIS_STATUS_SUCCESS             0u
#define NDIS_STATUS_NOT_SUPPORTED       0xC00000BBu
#define NDIS_STATUS_ADAPTER_NOT_READY   0xC0010011u
#define NDIS_STATUS_INVALID_LENGTH      0xC0010014u

#define NDIS_CONFIG_LEN         32
#define NDIS_BSSID_EX_FIXED     116     /* NDIS_WLAN_BSSID_EX up to its IEs */

/* NDIS_802_11_CONFIGURATION for the AP's channel */
static void oid_fill_config(uint8_t *b)
{
    memset(b, 0, NDIS_CONFIG_LEN);
    stl_le_p(b + 0, NDIS_CONFIG_LEN);
    stl_le_p(b + 4, 100);                       /* beacon period (TU) */
    stl_le_p(b + 12, (2407 + 5 * AP_CHANNEL) * 1000);  /* DSConfig, kHz */
    stl_le_p(b + 16, 16);                       /* FHConfig.Length */
}

/* NDIS_WLAN_BSSID_EX for the AP; returns its (4-aligned) length */
static size_t oid_fill_bssid_ex(uint8_t *b)
{
    size_t sl = strlen(ap_ssid), ie = NDIS_BSSID_EX_FIXED + 12, len;

    memcpy(b + 4, ap_bssid, 6);
    stl_le_p(b + 12, sl);
    memcpy(b + 16, ap_ssid, sl);
    stl_le_p(b + 48, 0);                        /* privacy: open */
    stl_le_p(b + 52, (uint32_t)AP_RSSI);
    stl_le_p(b + 56, 3);                        /* Ndis802_11OFDM24 */
    oid_fill_config(b + 60);
    stl_le_p(b + 92, 1);                        /* Ndis802_11Infrastructure */
    memcpy(b + 96, ap_rates, sizeof(ap_rates));
    /* fixed IEs: timestamp, beacon interval, capabilities (ESS) */
    stw_le_p(b + NDIS_BSSID_EX_FIXED + 8, 100);
    stw_le_p(b + NDIS_BSSID_EX_FIXED + 10, 0x0401);
    b[ie++] = 0;                                /* SSID */
    b[ie++] = sl;
    memcpy(b + ie, ap_ssid, sl);
    ie += sl;
    b[ie++] = 1;                                /* supported rates */
    b[ie++] = 8;
    memcpy(b + ie, ap_rates, 8);
    ie += 8;
    b[ie++] = 3;                                /* DS parameter set */
    b[ie++] = 1;
    b[ie++] = AP_CHANNEL;
    b[ie++] = 50;                               /* extended rates */
    b[ie++] = sizeof(ap_rates) - 8;
    memcpy(b + ie, ap_rates + 8, sizeof(ap_rates) - 8);
    ie += sizeof(ap_rates) - 8;
    stl_le_p(b + 112, ie - NDIS_BSSID_EX_FIXED);
    len = ROUND_UP(ie, 4);
    stl_le_p(b + 0, len);
    return len;
}

/*
 * Run an OID on buf (len bytes, the request's data).  Returns the NDIS
 * status; *n is the bytes written, or the bytes needed on failure.
 */
static uint32_t bcm_oid(Bcm4329State *s, uint32_t oid, bool set, uint8_t *buf,
                        size_t len, size_t *n)
{
    uint8_t tmp[256];
    size_t want = 4;
    uint32_t v = 0;

    *n = len;
    if (oid >= WL_OID_BASE && oid < WL_OID_BASE + 0x400) {
        return bcm_ioctl(s, oid - WL_OID_BASE, set, buf, len) ?
               NDIS_STATUS_NOT_SUPPORTED : NDIS_STATUS_SUCCESS;
    }
    if (set) {
        switch (oid) {
        case OID_802_11_BSSID_LIST_SCAN:
            s->scanning = true;
            bcm_kick(s, SCAN_MS);
            break;
        case OID_802_11_SSID:
            /* NDIS_802_11_SSID is laid out like wlc_ssid_t */
            bcm_ioctl(s, WLC_SET_SSID, true, buf, len);
            break;
        case OID_802_11_DISASSOCIATE:
            bcm_disassociate(s);
            break;
        }
        return NDIS_STATUS_SUCCESS;
    }

    memset(tmp, 0, sizeof(tmp));
    switch (oid) {
    case OID_802_3_PERMANENT_ADDRESS:
    case OID_802_3_CURRENT_ADDRESS:
        memcpy(tmp, s->mac, 6);
        want = 6;
        break;
    case OID_802_11_BSSID:
        if (!s->associated) {
            *n = 0;
            return NDIS_STATUS_ADAPTER_NOT_READY;
        }
        memcpy(tmp, ap_bssid, 6);
        want = 6;
        break;
    case OID_802_11_SSID:
        if (s->associated) {
            stl_le_p(tmp, strlen(ap_ssid));
            memcpy(tmp + 4, ap_ssid, strlen(ap_ssid));
        }
        want = 36;
        break;
    case OID_802_11_INFRASTRUCTURE_MODE:
        v = 1;
        break;
    case OID_802_11_AUTHENTICATION_MODE:
        v = 0;                                  /* open */
        break;
    case OID_802_11_ENCRYPTION_STATUS:
        v = 1;                                  /* disabled */
        break;
    case OID_802_11_PRIVACY_FILTER:
    case OID_802_11_POWER_MODE:
        v = 0;
        break;
    case OID_802_11_NETWORK_TYPES_SUPPORTED:
        stl_le_p(tmp, 2);
        stl_le_p(tmp + 4, 1);                   /* DS */
        stl_le_p(tmp + 8, 3);                   /* OFDM 2.4 GHz */
        want = 12;
        break;
    case OID_802_11_NETWORK_TYPE_IN_USE:
        v = 3;
        break;
    case OID_802_11_TX_POWER_LEVEL:
        v = 100;                                /* mW */
        break;
    case OID_802_11_RSSI:
        if (!s->associated) {
            *n = 0;
            return NDIS_STATUS_ADAPTER_NOT_READY;
        }
        v = (uint32_t)AP_RSSI;
        break;
    case OID_802_11_FRAGMENTATION_THRESHOLD:
    case OID_802_11_RTS_THRESHOLD:
        v = 2346;
        break;
    case OID_802_11_SUPPORTED_RATES:
        memcpy(tmp, ap_rates, sizeof(ap_rates));
        want = 16;
        break;
    case OID_802_11_CONFIGURATION:
        oid_fill_config(tmp);
        want = NDIS_CONFIG_LEN;
        break;
    case OID_802_11_BSSID_LIST:
        /* NDIS_802_11_BSSID_LIST_EX: our one AP, always in range */
        stl_le_p(tmp, 1);
        want = 4 + oid_fill_bssid_ex(tmp + 4);
        break;
    default:
        if ((oid >> 16) == 0x0D01) {
            *n = 0;
            return NDIS_STATUS_NOT_SUPPORTED;
        }
        /* general and statistics OIDs: zeros */
        memset(buf, 0, len);
        return NDIS_STATUS_SUCCESS;
    }
    if (want == 4) {
        stl_le_p(tmp, v);
    }
    if (len < want) {
        *n = want;
        return NDIS_STATUS_INVALID_LENGTH;
    }
    memcpy(buf, tmp, want);
    *n = want;
    return NDIS_STATUS_SUCCESS;
}

static void bcm_control(Bcm4329State *s, const uint8_t *p, size_t len)
{
    uint32_t cmd, msglen, flags;
    size_t dlen;
    g_autofree uint8_t *rsp = NULL;
    int ret;

    if (len < 16) {
        return;
    }
    cmd = ldl_le_p(p);
    msglen = ldl_le_p(p + 4);
    flags = ldl_le_p(p + 8);
    dlen = MIN(msglen & 0xFFFF, 2048);
    rsp = g_malloc0(16 + dlen);
    memcpy(rsp + 16, p + 16, MIN(dlen, len - 16));
    if (cmd >= 0x10000) {
        size_t n;
        uint32_t st = bcm_oid(s, cmd, flags & CDCF_IOC_SET, rsp + 16, dlen,
                              &n);

        DPRINTF("oid %08x %s len %zu -> %08x (%zu)\n", cmd,
                flags & CDCF_IOC_SET ? "set" : "query", dlen, st, n);
        if (st) {
            flags |= CDCF_IOC_ERROR;
            dlen = 0;
        } else {
            dlen = MIN(n, dlen);
        }
        stl_le_p(rsp, cmd);
        stl_le_p(rsp + 4, (msglen & ~0xFFFFu) | (uint16_t)n);
        stl_le_p(rsp + 8, flags);
        stl_le_p(rsp + 12, st);
        bcm_queue_frame(s, CHAN_CONTROL, NULL, 0, rsp, 16 + dlen);
        return;
    }
    ret = bcm_ioctl(s, cmd, flags & CDCF_IOC_SET, rsp + 16, dlen);
    DPRINTF("cdc cmd %u flags %08x len %zu -> %d\n", cmd, flags, dlen, ret);
    if (ret) {
        flags |= CDCF_IOC_ERROR;
    }
    stl_le_p(rsp, cmd);
    stl_le_p(rsp + 4, msglen);
    stl_le_p(rsp + 8, flags);
    stl_le_p(rsp + 12, (uint32_t)ret);
    bcm_queue_frame(s, CHAN_CONTROL, NULL, 0, rsp, 16 + dlen);
}

/* a complete frame from the host on function 2 */
static void bcm_host_frame(Bcm4329State *s, const uint8_t *f, size_t len)
{
    uint16_t flen, chk;
    uint8_t chan, doff;

    if (len < SDPCM_HDRLEN) {
        return;
    }
    flen = lduw_le_p(f);
    chk = lduw_le_p(f + 2);
    if ((uint16_t)~(flen ^ chk) || flen < SDPCM_HDRLEN || flen > len) {
        DPRINTF("dropped host frame: tag %04x/%04x, %zu bytes\n", flen, chk,
                len);
        DPRINTF("bad host frame %04x/%04x len %zu\n", flen, chk, len);
        return;
    }
    s->host_seq = f[4] + 1;
    if ((uint8_t)(s->adv_window - s->host_seq) < 8) {
        /* running out of credit: a header-only frame refreshes it */
        bcm_queue_frame(s, CHAN_DATA, NULL, 0, NULL, 0);
    }
    chan = f[5] & 0x0F;
    doff = f[7];
    if (doff < SDPCM_HDRLEN || doff > flen) {
        return;
    }
    switch (chan) {
    case CHAN_CONTROL:
        bcm_control(s, f + doff, flen - doff);
        break;
    case CHAN_DATA:
        DPRINTF("tx frame seq %u len %u (xfer %zu)\n", f[4], flen, len);
        if (flen - doff > BDC_HDRLEN && s->associated && s->up) {
            qemu_send_packet(qemu_get_queue(s->nic), f + doff + BDC_HDRLEN,
                             flen - doff - BDC_HDRLEN);
        }
        break;
    }
}

/* ---- network backend -------------------------------------------------------- */

static bool bcm_can_receive(NetClientState *nc)
{
    Bcm4329State *s = qemu_get_nic_opaque(nc);

    return s->fw_running && s->associated &&
           g_queue_get_length(&s->rxq) < RX_QUEUE_MAX / 2;
}

static ssize_t bcm_receive(NetClientState *nc, const uint8_t *buf, size_t len)
{
    Bcm4329State *s = qemu_get_nic_opaque(nc);
    uint8_t bdc[BDC_HDRLEN] = { 0x10, 0, 0, (uint8_t)AP_RSSI };

    if (!s->fw_running || !s->associated || len > 1600) {
        return len;
    }
    bcm_queue_frame(s, CHAN_DATA, bdc, sizeof(bdc), buf, len);
    return len;
}

static NetClientInfo bcm_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = bcm_can_receive,
    .receive = bcm_receive,
};

/* ---- backplane --------------------------------------------------------------- */

/* the NVRAM image sits below the length token in the last RAM word */
static void bcm_parse_nvram(Bcm4329State *s)
{
    uint32_t tok = ldl_le_p(s->ram + RAM_SIZE - 4);
    uint32_t words = tok & 0xFFFF;
    uint32_t start, i;

    if (((tok >> 16) ^ 0xFFFF) != words || !words ||
        words * 4 > RAM_SIZE - 4) {
        return;
    }
    start = RAM_SIZE - 4 - words * 4;
    for (i = start; i < RAM_SIZE - 4; ) {
        const char *v = (const char *)s->ram + i;
        size_t n = strnlen(v, RAM_SIZE - 4 - i);
        unsigned m[6];

        if (n > 8 && !strncmp(v, "macaddr=", 8) &&
            sscanf(v + 8, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3],
                   &m[4], &m[5]) == 6) {
            for (int k = 0; k < 6; k++) {
                s->mac[k] = m[k];
            }
            DPRINTF("NVRAM MAC %02x:%02x:%02x:%02x:%02x:%02x\n", m[0], m[1],
                    m[2], m[3], m[4], m[5]);
        }
        i += n + 1;
    }
}

static void bcm_fw_start(Bcm4329State *s)
{
    DPRINTF("firmware started\n");
    bcm_parse_nvram(s);
    s->fw_running = true;
    s->tohostmbdata = HMB_DATA_DEVREADY | HMB_DATA_FWREADY |
                      (SDPCM_PROT_VERSION << 16);
    s->intstatus |= I_HMB_HOST_INT;
    bcm_update_irq(s);
}

static void bcm_fw_stop(Bcm4329State *s)
{
    s->fw_running = false;
    s->up = s->associated = s->scanning = false;
    s->pending_join = 0;
    s->intstatus = 0;
    s->tohostmbdata = 0;
    s->tx_seq = 0;
    s->host_seq = 0;
    s->adv_window = 0;
    while (!g_queue_is_empty(&s->rxq)) {
        g_byte_array_unref(g_queue_pop_head(&s->rxq));
    }
    s->rx_pos = 0;
    g_byte_array_set_size(s->f2_tx, 0);
    timer_del(s->timer);
    bcm_update_irq(s);
}

static uint32_t bp_read32(Bcm4329State *s, uint32_t a)
{
    a &= ~3u;
    if (a < RAM_SIZE) {
        return ldl_le_p(s->ram + a);
    }
    if (a >= SI_ENUM_BASE && a < SI_ENUM_BASE + NCORES * SI_CORE_SIZE) {
        int c = (a - SI_ENUM_BASE) / SI_CORE_SIZE;
        uint32_t off = a & (SI_CORE_SIZE - 1);

        switch (off) {
        case SB_IDHIGH:
            return (0x4243u << 16) | ((core_info[c].id & 0xFFF) << 4) |
                   ((core_info[c].rev & 0x70) << 8) |
                   (core_info[c].rev & 0x0F);
        case SB_IDLOW:
            return 0;
        case SB_TMSTATEHIGH:
            return 0;                   /* never busy */
        case SB_ADMATCH0:
            return SI_ENUM_BASE + c * SI_CORE_SIZE;
        }
        if (c == CORE_CC) {
            switch (off) {
            case 0x00:
                return CHIPID;
            case 0x04:
                return CC_CAP_PMU | 0x00400000;
            case 0x604:
                return 2;               /* PMU rev 2 */
            }
        }
        if (c == CORE_SOCRAM && off == 0x00) {
            return (9 << 4) | 1;        /* 9 banks of 32 KiB */
        }
        if (c == CORE_SDIOD) {
            switch (off) {
            case SD_INTSTATUS:
                return s->intstatus;
            case SD_HOSTINTMASK:
                return s->hostintmask;
            case SD_TOHOSTMAILBOXDATA:
                return s->tohostmbdata;
            }
        }
        return s->core[c][off / 4];
    }
    return 0;
}

static void bp_write32(Bcm4329State *s, uint32_t a, uint32_t v)
{
    a &= ~3u;
    if (a < RAM_SIZE) {
        stl_le_p(s->ram + a, v);
        return;
    }
    if (a >= SI_ENUM_BASE && a < SI_ENUM_BASE + NCORES * SI_CORE_SIZE) {
        int c = (a - SI_ENUM_BASE) / SI_CORE_SIZE;
        uint32_t off = a & (SI_CORE_SIZE - 1);

        if (c == CORE_SDIOD) {
            switch (off) {
            case SD_INTSTATUS:
                s->intstatus &= ~v;
                if (!g_queue_is_empty(&s->rxq)) {
                    s->intstatus |= I_HMB_FRAME_IND;
                }
                bcm_update_irq(s);
                return;
            case SD_HOSTINTMASK:
                s->hostintmask = v;
                bcm_update_irq(s);
                return;
            case SD_TOSBMAILBOX:
                /* SMB_INT_ACK: the host read the mailbox data */
                return;
            }
        }
        s->core[c][off / 4] = v;
        if (c == CORE_ARM && off == SB_TMSTATELOW) {
            bool up = (v & (SBTML_RESET | SBTML_REJ | SICF_CLOCK_EN)) ==
                      SICF_CLOCK_EN;

            if (up && !s->fw_running) {
                bcm_fw_start(s);
            } else if (!up && s->fw_running) {
                bcm_fw_stop(s);
            }
        }
    }
}

/* function 1: backplane window (with the 4-byte access flag) and F1 regs */
static uint8_t f1_read8(Bcm4329State *s, uint32_t addr)
{
    if (addr < 0x10000) {
        uint32_t a = s->sbwindow | (addr & 0x7FFF);

        return bp_read32(s, a) >> ((a & 3) * 8);
    }
    switch (addr) {
    case F1_SBADDRLOW:
        return (s->sbwindow >> 8) & 0x80;
    case F1_SBADDRMID:
        return s->sbwindow >> 16;
    case F1_SBADDRHIGH:
        return s->sbwindow >> 24;
    case F1_CHIPCLKCSR:
        return s->clkcsr | CLK_AVBITS;
    }
    return addr < 0x10020 ? s->f1reg[addr - 0x10000] : 0;
}

static void f1_write8(Bcm4329State *s, uint32_t addr, uint8_t v)
{
    if (addr < 0x10000) {
        uint32_t a = s->sbwindow | (addr & 0x7FFF);
        uint32_t w = bp_read32(s, a), sh = (a & 3) * 8;

        bp_write32(s, a, (w & ~(0xFFu << sh)) | ((uint32_t)v << sh));
        return;
    }
    switch (addr) {
    case F1_SBADDRLOW:
        s->sbwindow = (s->sbwindow & ~0x8000u) | ((v & 0x80u) << 8);
        return;
    case F1_SBADDRMID:
        s->sbwindow = (s->sbwindow & ~0xFF0000u) | (v << 16);
        return;
    case F1_SBADDRHIGH:
        s->sbwindow = (s->sbwindow & ~0xFF000000u) | ((uint32_t)v << 24);
        return;
    case F1_CHIPCLKCSR:
        s->clkcsr = v & 0x3F;
        return;
    case F1_FRAMECTRL:
        if ((v & SFC_RF_TERM) && !g_queue_is_empty(&s->rxq)) {
            /* abandon the frame being read */
            g_byte_array_unref(g_queue_pop_head(&s->rxq));
            s->rx_pos = 0;
        }
        return;
    }
    if (addr < 0x10020) {
        s->f1reg[addr - 0x10000] = v;
    }
}

/* block access through the window: RAM bulk, registers as 32-bit words */
static void f1_read_block(Bcm4329State *s, uint32_t addr, uint8_t *buf,
                          uint32_t len, bool incr)
{
    for (uint32_t i = 0; i < len; ) {
        uint32_t a = incr ? addr + i : addr;

        if (a < 0x10000 && !((s->sbwindow | (a & 0x7FFF)) & 3) &&
            len - i >= 4) {
            stl_le_p(buf + i, bp_read32(s, s->sbwindow | (a & 0x7FFF)));
            i += 4;
        } else {
            buf[i++] = f1_read8(s, a);
        }
    }
}

static void f1_write_block(Bcm4329State *s, uint32_t addr, const uint8_t *buf,
                           uint32_t len, bool incr)
{
    for (uint32_t i = 0; i < len; ) {
        uint32_t a = incr ? addr + i : addr;

        if (a < 0x10000 && !((s->sbwindow | (a & 0x7FFF)) & 3) &&
            len - i >= 4) {
            bp_write32(s, s->sbwindow | (a & 0x7FFF), ldl_le_p(buf + i));
            i += 4;
        } else {
            f1_write8(s, a, buf[i++]);
        }
    }
}

/* ---- function 0: CCCR, FBR, CIS -------------------------------------------- */

static uint8_t cis_byte(uint32_t addr)
{
    static const uint8_t cis0[] = {
        0x20, 0x04, 0xd0, 0x02, 0x29, 0x43,     /* MANFID 02d0:4329 */
        0x22, 0x04, 0x00, 0x40, 0x00, 0x32,     /* FUNCE: 64 bytes, 25 MHz */
        0xff,
    };
    uint8_t funce[2 + 42] = { 0x22, 42, 0x01 };
    int fn = addr >> 12;
    uint32_t off = addr & 0xFFF;

    if (fn == 1) {
        if (off < sizeof(cis0)) {
            return cis0[off];
        }
        return 0xff;
    }
    if (fn == 2 || fn == 3) {
        uint16_t blk = fn == 2 ? 64 : 512;

        funce[2 + 12] = blk & 0xFF;
        funce[2 + 13] = blk >> 8;
        funce[2 + 28] = 100;                /* enable timeout: 1 s */
        if (off < sizeof(funce)) {
            return funce[off];
        }
        return 0xff;
    }
    return 0;
}

static uint8_t f0_read8(Bcm4329State *s, uint32_t addr)
{
    if (addr >= 0x1000) {
        return cis_byte(addr);
    }
    if (addr >= 0x100 && addr < 0x300) {
        int fn = addr >> 8;
        uint32_t off = addr & 0xFF;
        uint32_t cis = fn == 1 ? 0x2000 : 0x3000;

        switch (off) {
        case 0x09:
            return cis & 0xFF;
        case 0x0A:
            return (cis >> 8) & 0xFF;
        case 0x0B:
            return cis >> 16;
        case 0x10:
            return s->blksize[fn] & 0xFF;
        case 0x11:
            return s->blksize[fn] >> 8;
        }
        return 0;
    }
    switch (addr) {
    case 0x00:
        return 0x32;                        /* SDIO 2.00, CCCR 1.20 */
    case 0x01:
        return 0x02;
    case CCCR_IOEN:
        return s->ioen;
    case CCCR_IORDY:
        return (s->ioen & 0x02) | (s->fw_running ? (s->ioen & 0x04) : 0);
    case CCCR_INTEN:
        return s->ien;
    case CCCR_INTPEND:
        return bcm_irq_pending(s) ? 0x02 : 0;   /* function 1's */
    case CCCR_BUSIF:
        return s->busif;
    case 0x08:
        return 0x12;                        /* SMB, S4MI */
    case 0x09:
        return 0x00;
    case 0x0A:
        return 0x10;                        /* common CIS at 0x1000 */
    case 0x0B:
        return 0x00;
    case 0x10:
        return s->blksize[0] & 0xFF;
    case 0x11:
        return s->blksize[0] >> 8;
    case CCCR_SPEED:
        return s->speed | 0x01;
    case CCCR_BRCM_SEPINT:
        return s->sepint;
    }
    return 0;
}

static void f0_write8(Bcm4329State *s, uint32_t addr, uint8_t v)
{
    if (addr >= 0x100 && addr < 0x300) {
        int fn = addr >> 8;

        if ((addr & 0xFF) == 0x10) {
            s->blksize[fn] = (s->blksize[fn] & 0xFF00) | v;
        } else if ((addr & 0xFF) == 0x11) {
            s->blksize[fn] = (s->blksize[fn] & 0xFF) | (v << 8);
        }
        return;
    }
    switch (addr) {
    case CCCR_IOEN:
        s->ioen = v & 0x06;
        break;
    case CCCR_INTEN:
        s->ien = v;
        bcm_update_irq(s);
        break;
    case CCCR_BUSIF:
        s->busif = v;
        break;
    case 0x06:                              /* ABORT / RES */
        if (v & 0x08) {
            s->ioen = 0;
        }
        break;
    case 0x10:
        s->blksize[0] = (s->blksize[0] & 0xFF00) | v;
        break;
    case 0x11:
        s->blksize[0] = (s->blksize[0] & 0xFF) | (v << 8);
        break;
    case CCCR_SPEED:
        s->speed = v & 0x02;
        break;
    case CCCR_BRCM_SEPINT:
        s->sepint = v;
        bcm_update_irq(s);
        break;
    }
}

/* ---- SDIO card protocol ------------------------------------------------- */

static void bcm_f2_read(Bcm4329State *s, uint8_t *buf, uint32_t len)
{
    GByteArray *f = g_queue_peek_head(&s->rxq);

    memset(buf, 0, len);
    if (!f) {
        return;                             /* zero header: no more frames */
    }
    if (s->rx_pos < f->len) {
        memcpy(buf, f->data + s->rx_pos, MIN(len, f->len - s->rx_pos));
    }
    s->rx_pos += len;
    if (s->rx_pos >= f->len) {
        g_byte_array_unref(g_queue_pop_head(&s->rxq));
        s->rx_pos = 0;
        if (g_queue_get_length(&s->rxq) < RX_QUEUE_MAX / 4) {
            qemu_bh_schedule(s->flush_bh);
        }
    }
}

static size_t bcm_command(void *opaque, uint8_t cmd, uint32_t arg,
                          uint32_t resp[4])
{
    Bcm4329State *s = opaque;

    if (!s->powered) {
        return 0;
    }
    switch (cmd) {
    case 0:                                 /* GO_IDLE */
        s->rca = 0;
        return 0;
    case 5:                                 /* IO_SEND_OP_COND: R4 */
        resp[0] = 0x80000000u | (2u << 28) | 0x00FF8000u;
        return 4;
    case 3:                                 /* SEND_RELATIVE_ADDR: R6 */
        s->rca = 0x0001;
        resp[0] = (s->rca << 16) | 0x0500;
        return 4;
    case 7:                                 /* SELECT_CARD: R1b */
        resp[0] = 0x00001E00;
        return 4;
    case 52: {                              /* IO_RW_DIRECT: R5 */
        int fn = (arg >> 28) & 7;
        uint32_t addr = (arg >> 9) & 0x1FFFF;
        bool wr = arg & 0x80000000u, raw = arg & 0x08000000u;
        uint8_t v = arg & 0xFF;

        if (wr) {
            if (fn == 0) {
                f0_write8(s, addr, v);
            } else if (fn == 1) {
                f1_write8(s, addr, v);
            }
        }
        if (!wr || raw) {
            v = fn == 0 ? f0_read8(s, addr) : fn == 1 ? f1_read8(s, addr) : 0;
        }
        resp[0] = 0x1000 | v;
        return 4;
    }
    case 53: {                              /* IO_RW_EXTENDED: R5 + data */
        int fn = (arg >> 28) & 7;
        uint32_t count = arg & 0x1FF;

        s->xfer_write = arg & 0x80000000u;
        s->xfer_fn = fn;
        s->xfer_incr = arg & 0x04000000u;
        s->xfer_addr = (arg >> 9) & 0x1FFFF;
        if (arg & 0x08000000u) {
            s->xfer_len = (count ? count : 512) * MAX(s->blksize[MIN(fn, 2)],
                                                      1);
        } else {
            s->xfer_len = count ? count : 512;
        }
        s->xfer_pos = 0;
        g_byte_array_set_size(s->xfer_buf, s->xfer_len);
        if (!s->xfer_write) {
            /* produce the data now: reads may have side effects */
            if (fn == 0) {
                for (uint32_t i = 0; i < s->xfer_len; i++) {
                    s->xfer_buf->data[i] =
                        f0_read8(s, s->xfer_addr + (s->xfer_incr ? i : 0));
                }
            } else if (fn == 1) {
                f1_read_block(s, s->xfer_addr, s->xfer_buf->data,
                              s->xfer_len, s->xfer_incr);
            } else {
                bcm_f2_read(s, s->xfer_buf->data, s->xfer_len);
            }
        }
        s->xfer_active = true;
        resp[0] = 0x1000;
        return 4;
    }
    }
    return 0;                               /* not an SDIO command */
}

static bool bcm_data_ready(void *opaque)
{
    Bcm4329State *s = opaque;

    return s->xfer_active && !s->xfer_write;
}

static void bcm_read(void *opaque, uint8_t *buf, size_t len)
{
    Bcm4329State *s = opaque;
    size_t n = s->xfer_active && s->xfer_pos < s->xfer_len ?
               MIN(len, s->xfer_len - s->xfer_pos) : 0;

    memcpy(buf, s->xfer_buf->data + s->xfer_pos, n);
    memset(buf + n, 0, len - n);
    s->xfer_pos += n;
}

static void bcm_write(void *opaque, const uint8_t *buf, size_t len)
{
    Bcm4329State *s = opaque;
    size_t n = s->xfer_active && s->xfer_pos < s->xfer_len ?
               MIN(len, s->xfer_len - s->xfer_pos) : 0;

    memcpy(s->xfer_buf->data + s->xfer_pos, buf, n);
    s->xfer_pos += n;
}

static void bcm_data_end(void *opaque)
{
    Bcm4329State *s = opaque;

    if (!s->xfer_active) {
        return;
    }
    s->xfer_active = false;
    if (!s->xfer_write) {
        return;
    }
    if (s->xfer_fn == 0) {
        for (uint32_t i = 0; i < s->xfer_pos; i++) {
            f0_write8(s, s->xfer_addr + (s->xfer_incr ? i : 0),
                      s->xfer_buf->data[i]);
        }
    } else if (s->xfer_fn == 1) {
        f1_write_block(s, s->xfer_addr, s->xfer_buf->data, s->xfer_pos,
                       s->xfer_incr);
    } else if (s->fw_running) {
        /*
         * One frame may take several CMD53s (blocks, then the rest in
         * byte mode); bytes past the frame length are padding.
         */
        g_byte_array_append(s->f2_tx, s->xfer_buf->data, s->xfer_pos);
        if (s->f2_tx->len >= 4) {
            uint16_t flen = lduw_le_p(s->f2_tx->data);
            uint16_t chk = lduw_le_p(s->f2_tx->data + 2);

            if ((uint16_t)~(flen ^ chk) || flen < SDPCM_HDRLEN) {
                DPRINTF("dropped host data: tag %04x/%04x\n", flen, chk);
                g_byte_array_set_size(s->f2_tx, 0);
            } else if (s->f2_tx->len >= flen) {
                bcm_host_frame(s, s->f2_tx->data, s->f2_tx->len);
                g_byte_array_set_size(s->f2_tx, 0);
            }
        }
    }
}

static const MsmSdioCardOps bcm_sdio_ops = {
    .command = bcm_command,
    .data_ready = bcm_data_ready,
    .read = bcm_read,
    .write = bcm_write,
    .data_end = bcm_data_end,
};

/* ---- device ------------------------------------------------------------------- */

static void bcm_chip_reset(Bcm4329State *s)
{
    bcm_fw_stop(s);
    s->rca = 0;
    s->ioen = s->ien = s->busif = s->speed = s->sepint = 0;
    s->blksize[0] = s->blksize[1] = s->blksize[2] = 0;
    s->sbwindow = SI_ENUM_BASE;
    s->clkcsr = 0;
    memset(s->f1reg, 0, sizeof(s->f1reg));
    memset(s->core, 0, sizeof(s->core));
    /* ARM held in reset until the host starts the firmware */
    s->core[CORE_ARM][SB_TMSTATELOW / 4] = SBTML_RESET;
    s->hostintmask = 0;
    s->xfer_active = false;
    g_hash_table_remove_all(s->iovars);
    memcpy(s->mac, s->conf.macaddr.a, 6);
    bcm_update_irq(s);
}

void bcm4329_set_power(DeviceState *dev, bool on)
{
    Bcm4329State *s = BCM4329(dev);

    if (s->powered != on) {
        s->powered = on;
        bcm_chip_reset(s);
    }
}

void bcm4329_attach(DeviceState *dev, DeviceState *sdcc)
{
    BCM4329(dev)->sdcc = sdcc;
    msm_sdcc_attach_sdio(sdcc, &bcm_sdio_ops, BCM4329(dev));
}

static void bcm4329_reset(DeviceState *dev)
{
    Bcm4329State *s = BCM4329(dev);

    s->powered = false;
    bcm_chip_reset(s);
}

static void bcm_flush_bh(void *opaque)
{
    Bcm4329State *s = opaque;

    qemu_flush_queued_packets(qemu_get_queue(s->nic));
}

static void bcm4329_realize(DeviceState *dev, Error **errp)
{
    Bcm4329State *s = BCM4329(dev);

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&bcm_net_info, &s->conf,
                          object_get_typename(OBJECT(dev)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
    s->flush_bh = qemu_bh_new_guarded(bcm_flush_bh, s,
                                      &dev->mem_reentrancy_guard);
}

static void bcm4329_init(Object *obj)
{
    Bcm4329State *s = BCM4329(obj);

    s->ram = g_malloc0(RAM_SIZE);
    s->xfer_buf = g_byte_array_new();
    s->f2_tx = g_byte_array_new();
    s->iovars = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                      (GDestroyNotify)g_byte_array_unref);
    g_queue_init(&s->rxq);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bcm_timer, s);
    qdev_init_gpio_out_named(DEVICE(obj), &s->oob, "oob", 1);
}

static const Property bcm4329_props[] = {
    DEFINE_NIC_PROPERTIES(Bcm4329State, conf),
};

static void bcm4329_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = bcm4329_realize;
    device_class_set_legacy_reset(dc, bcm4329_reset);
    device_class_set_props(dc, bcm4329_props);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo bcm4329_info = {
    .name = TYPE_BCM4329,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(Bcm4329State),
    .instance_init = bcm4329_init,
    .class_init = bcm4329_class_init,
};

static void bcm4329_register(void)
{
    type_register_static(&bcm4329_info);
}
type_init(bcm4329_register)
