/*
 * Broadcom BCM4329 Bluetooth core: an HCI controller on UART1DM (H4)
 *
 * The phone side is the ROM's own stack:
 *   - brcm_patchram_plus (the "hciattach" service) resets the core, sends
 *     Download Minidriver, streams bcm4329.hcd as Write RAM / Launch RAM
 *     commands, switches the baud rate and enables sleep mode (LPM), then
 *     hands the tty to the kernel's H4 line discipline;
 *   - the kernel's HCI core and bluetoothd bring the adapter up.
 * The controller answers those commands like a BCM4329 with BT 2.1 + EDR
 * firmware, scans (inquiry) without finding anyone and fails pages with a
 * page timeout: there is no radio.
 *
 * Low power mode: the chip raises BT_HOST_WAKE while it has bytes for the
 * host, so bcm_bt_lpm keeps the UART clock (and its receive DMA) running.
 *
 * Broadcom vendor command 0xFC15 reaches the FM receiver (bcm4329_fm.c).
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/cutils.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "htcleo.h"
#include "bcm4329_bt.h"

OBJECT_DECLARE_SIMPLE_TYPE(Bcm4329BtState, BCM4329_BT)

#define H4_CMD      0x01
#define H4_ACL      0x02
#define H4_SCO      0x03
#define H4_EVT      0x04

#define EV_INQUIRY_COMPLETE     0x01
#define EV_CONN_COMPLETE        0x03
#define EV_REMOTE_NAME          0x07
#define EV_CMD_COMPLETE         0x0E
#define EV_CMD_STATUS           0x0F
#define EV_VENDOR               0xFF

#define ST_OK                   0x00
#define ST_UNKNOWN_CMD          0x01
#define ST_PAGE_TIMEOUT         0x04

#define OP(ogf, ocf)    (((ogf) << 10) | (ocf))

#define HOST_WAKE_HOLD_NS   (30 * SCALE_MS)
#define CMD_LATENCY_NS      (50 * SCALE_US)

struct Bcm4329BtState {
    DeviceState parent_obj;
    DeviceState *uart;
    qemu_irq host_wake;
    char *bdaddr_prop;
    char *fm_stations;

    bool powered;
    uint8_t bdaddr[6];
    GByteArray *in;             /* host -> chip bytes not yet parsed */
    QEMUTimer *cmd_timer;
    QEMUTimer *wake_timer;
    QEMUTimer *inquiry_timer;
    QEMUTimer *page_timer;
    uint8_t page_addr[6];

    /* controller settings the host reads back */
    uint8_t name[248];
    uint8_t cod[3];
    uint8_t voice[2];
    uint8_t scan_enable;
    uint8_t auth_enable;
    uint8_t inquiry_mode;
    uint8_t ssp_mode;
    uint8_t page_scan_type, inq_scan_type;
    uint8_t eir[241];
    uint16_t page_timeout, ca_timeout, link_policy;
    uint8_t page_scan_act[4], inq_scan_act[4];

    Bcm4329Fm *fm;
};

/* ---------------------------------------------------------------- */
/* Output to the host                                                */
/* ---------------------------------------------------------------- */

static void bt_wake_release(void *opaque)
{
    Bcm4329BtState *s = opaque;

    qemu_set_irq(s->host_wake, 0);
}

static void bt_send_raw(Bcm4329BtState *s, const uint8_t *buf, int len)
{
    if (!s->powered || !s->uart) {
        return;
    }
    qemu_set_irq(s->host_wake, 1);
    timer_mod(s->wake_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              HOST_WAKE_HOLD_NS);
    msm_uartdm_receive(s->uart, buf, len);
}

static void bt_event(Bcm4329BtState *s, uint8_t evt, const uint8_t *p,
                     int len)
{
    uint8_t pkt[3 + 255];

    pkt[0] = H4_EVT;
    pkt[1] = evt;
    pkt[2] = len;
    memcpy(pkt + 3, p, len);
    bt_send_raw(s, pkt, 3 + len);
}

static void bt_cmd_complete(Bcm4329BtState *s, uint16_t op,
                            const uint8_t *ret, int len)
{
    uint8_t p[255];

    p[0] = 1;                       /* Num_HCI_Command_Packets */
    p[1] = op;
    p[2] = op >> 8;
    memcpy(p + 3, ret, MIN(len, 252));
    bt_event(s, EV_CMD_COMPLETE, p, 3 + MIN(len, 252));
}

static void bt_cmd_status(Bcm4329BtState *s, uint16_t op, uint8_t status)
{
    uint8_t p[4] = { status, 1, op, op >> 8 };

    bt_event(s, EV_CMD_STATUS, p, 4);
}

static void bt_fm_irq(void *opaque)
{
    Bcm4329BtState *s = opaque;
    /* vendor-specific event, sub-code 0x08: FM interrupt (read the flags) */
    uint8_t p[1] = { 0x08 };

    bt_event(s, EV_VENDOR, p, 1);
}

/* ---------------------------------------------------------------- */
/* Controller state                                                  */
/* ---------------------------------------------------------------- */

static void bt_controller_reset(Bcm4329BtState *s)
{
    timer_del(s->inquiry_timer);
    timer_del(s->page_timer);
    memset(s->name, 0, sizeof(s->name));
    pstrcpy((char *)s->name, sizeof(s->name), "BCM4329B1");
    memset(s->cod, 0, sizeof(s->cod));
    s->voice[0] = 0x60;             /* CVSD, 16-bit, 2's complement */
    s->voice[1] = 0x00;
    s->scan_enable = 0;
    s->auth_enable = 0;
    s->inquiry_mode = 0;
    s->ssp_mode = 0;
    s->page_scan_type = s->inq_scan_type = 0;
    memset(s->eir, 0, sizeof(s->eir));
    s->page_timeout = 0x2000;
    s->ca_timeout = 0x1FA0;
    s->link_policy = 0;
    memcpy(s->page_scan_act, "\x00\x08\x12\x00", 4);
    memcpy(s->inq_scan_act, "\x00\x10\x12\x00", 4);
}

static void bt_inquiry_done(void *opaque)
{
    Bcm4329BtState *s = opaque;
    uint8_t st = ST_OK;

    bt_event(s, EV_INQUIRY_COMPLETE, &st, 1);
}

static void bt_page_done(void *opaque)
{
    Bcm4329BtState *s = opaque;
    uint8_t p[11] = { ST_PAGE_TIMEOUT, 0, 0 };

    memcpy(p + 3, s->page_addr, 6);
    p[9] = 0x01;                    /* ACL */
    p[10] = 0x00;
    bt_event(s, EV_CONN_COMPLETE, p, sizeof(p));
}

/* ---------------------------------------------------------------- */
/* Commands                                                          */
/* ---------------------------------------------------------------- */

/* supported commands bitmap (octets 0..21: everything up to BT 2.1) */
static void bt_supported_commands(uint8_t *bm)
{
    memset(bm, 0, 64);
    memset(bm, 0xFF, 22);
}

static void bt_command(Bcm4329BtState *s, uint16_t op, const uint8_t *p,
                       int plen)
{
    uint8_t r[252] = { ST_OK };
    int rl = 1;

    switch (op) {
    /* ---- link control ---- */
    case OP(0x01, 0x0001):              /* Inquiry */
        bt_cmd_status(s, op, ST_OK);
        timer_mod(s->inquiry_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  MIN(plen >= 4 ? p[3] : 8, 4) * 1280 * SCALE_MS);
        return;
    case OP(0x01, 0x0002):              /* Inquiry Cancel */
        timer_del(s->inquiry_timer);
        break;
    case OP(0x01, 0x0005):              /* Create Connection */
        bt_cmd_status(s, op, ST_OK);
        if (plen >= 6) {
            memcpy(s->page_addr, p, 6);
        }
        timer_mod(s->page_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  5 * NANOSECONDS_PER_SECOND);
        return;
    case OP(0x01, 0x0008):              /* Create Connection Cancel */
    case OP(0x01, 0x000B):              /* Link Key Request Reply */
    case OP(0x01, 0x000C):              /* Link Key Request Negative Reply */
    case OP(0x01, 0x000D):              /* PIN Code Request Reply */
    case OP(0x01, 0x000E):              /* PIN Code Request Negative Reply */
    case OP(0x01, 0x001A):              /* Remote Name Request Cancel */
        if (plen >= 6) {
            memcpy(r + 1, p, 6);
        }
        rl = 7;
        break;
    case OP(0x01, 0x0019): {            /* Remote Name Request */
        uint8_t ev[255] = { ST_PAGE_TIMEOUT };
        bt_cmd_status(s, op, ST_OK);
        if (plen >= 6) {
            memcpy(ev + 1, p, 6);
        }
        bt_event(s, EV_REMOTE_NAME, ev, 255);
        return;
    }
    case OP(0x01, 0x0006):              /* Disconnect */
    case OP(0x01, 0x0009):              /* Accept Connection Request */
    case OP(0x01, 0x000A):              /* Reject Connection Request */
        bt_cmd_status(s, op, ST_OK);
        return;

    /* ---- link policy ---- */
    case OP(0x02, 0x000E):              /* Read Default Link Policy */
        r[1] = s->link_policy;
        r[2] = s->link_policy >> 8;
        rl = 3;
        break;
    case OP(0x02, 0x000F):              /* Write Default Link Policy */
        if (plen >= 2) {
            s->link_policy = lduw_le_p(p);
        }
        break;

    /* ---- controller and baseband ---- */
    case OP(0x03, 0x0003):              /* Reset */
        bt_controller_reset(s);
        bcm4329_fm_reset(s->fm);
        break;
    case OP(0x03, 0x000D):              /* Read Stored Link Key */
        r[1] = 0x10;                    /* Max_Num_Keys */
        rl = 5;
        break;
    case OP(0x03, 0x0012):              /* Delete Stored Link Key */
        rl = 3;
        break;
    case OP(0x03, 0x0013):              /* Write Local Name */
        memcpy(s->name, p, MIN(plen, (int)sizeof(s->name)));
        break;
    case OP(0x03, 0x0014):              /* Read Local Name */
        memcpy(r + 1, s->name, sizeof(s->name));
        rl = 1 + sizeof(s->name);
        break;
    case OP(0x03, 0x0015):              /* Read Connection Accept Timeout */
        stw_le_p(r + 1, s->ca_timeout);
        rl = 3;
        break;
    case OP(0x03, 0x0016):              /* Write Connection Accept Timeout */
        if (plen >= 2) {
            s->ca_timeout = lduw_le_p(p);
        }
        break;
    case OP(0x03, 0x0017):              /* Read Page Timeout */
        stw_le_p(r + 1, s->page_timeout);
        rl = 3;
        break;
    case OP(0x03, 0x0018):              /* Write Page Timeout */
        if (plen >= 2) {
            s->page_timeout = lduw_le_p(p);
        }
        break;
    case OP(0x03, 0x0019):              /* Read Scan Enable */
        r[1] = s->scan_enable;
        rl = 2;
        break;
    case OP(0x03, 0x001A):              /* Write Scan Enable */
        s->scan_enable = plen ? p[0] : 0;
        break;
    case OP(0x03, 0x001B):              /* Read Page Scan Activity */
        memcpy(r + 1, s->page_scan_act, 4);
        rl = 5;
        break;
    case OP(0x03, 0x001C):              /* Write Page Scan Activity */
        memcpy(s->page_scan_act, p, MIN(plen, 4));
        break;
    case OP(0x03, 0x001D):              /* Read Inquiry Scan Activity */
        memcpy(r + 1, s->inq_scan_act, 4);
        rl = 5;
        break;
    case OP(0x03, 0x001E):              /* Write Inquiry Scan Activity */
        memcpy(s->inq_scan_act, p, MIN(plen, 4));
        break;
    case OP(0x03, 0x001F):              /* Read Authentication Enable */
        r[1] = s->auth_enable;
        rl = 2;
        break;
    case OP(0x03, 0x0020):              /* Write Authentication Enable */
        s->auth_enable = plen ? p[0] : 0;
        break;
    case OP(0x03, 0x0023):              /* Read Class of Device */
        memcpy(r + 1, s->cod, 3);
        rl = 4;
        break;
    case OP(0x03, 0x0024):              /* Write Class of Device */
        memcpy(s->cod, p, MIN(plen, 3));
        break;
    case OP(0x03, 0x0025):              /* Read Voice Setting */
        memcpy(r + 1, s->voice, 2);
        rl = 3;
        break;
    case OP(0x03, 0x0026):              /* Write Voice Setting */
        memcpy(s->voice, p, MIN(plen, 2));
        break;
    case OP(0x03, 0x0035):              /* Host Number Of Completed Packets */
        return;                         /* no event */
    case OP(0x03, 0x0038):              /* Read Number Of Supported IAC */
        r[1] = 1;
        rl = 2;
        break;
    case OP(0x03, 0x0039):              /* Read Current IAC LAP */
        r[1] = 1;
        r[2] = 0x33;                    /* GIAC 0x9E8B33 */
        r[3] = 0x8B;
        r[4] = 0x9E;
        rl = 5;
        break;
    case OP(0x03, 0x0042):              /* Read Inquiry Scan Type */
        r[1] = s->inq_scan_type;
        rl = 2;
        break;
    case OP(0x03, 0x0043):              /* Write Inquiry Scan Type */
        s->inq_scan_type = plen ? p[0] : 0;
        break;
    case OP(0x03, 0x0044):              /* Read Inquiry Mode */
        r[1] = s->inquiry_mode;
        rl = 2;
        break;
    case OP(0x03, 0x0045):              /* Write Inquiry Mode */
        s->inquiry_mode = plen ? p[0] : 0;
        break;
    case OP(0x03, 0x0046):              /* Read Page Scan Type */
        r[1] = s->page_scan_type;
        rl = 2;
        break;
    case OP(0x03, 0x0047):              /* Write Page Scan Type */
        s->page_scan_type = plen ? p[0] : 0;
        break;
    case OP(0x03, 0x0051):              /* Read Extended Inquiry Response */
        memcpy(r + 1, s->eir, sizeof(s->eir));
        rl = 1 + sizeof(s->eir);
        break;
    case OP(0x03, 0x0052):              /* Write Extended Inquiry Response */
        memcpy(s->eir, p, MIN(plen, (int)sizeof(s->eir)));
        break;
    case OP(0x03, 0x0055):              /* Read Simple Pairing Mode */
        r[1] = s->ssp_mode;
        rl = 2;
        break;
    case OP(0x03, 0x0056):              /* Write Simple Pairing Mode */
        s->ssp_mode = plen ? p[0] : 0;
        break;
    case OP(0x03, 0x0058):              /* Read Inquiry Response TX Power */
        r[1] = 4;                       /* dBm */
        rl = 2;
        break;
    case OP(0x03, 0x005A):              /* Read Default Erroneous Data Rep. */
        rl = 2;
        break;
    case OP(0x03, 0x0001):              /* Set Event Mask */
    case OP(0x03, 0x0005):              /* Set Event Filter */
    case OP(0x03, 0x0033):              /* Host Buffer Size */
    case OP(0x03, 0x003A):              /* Write Current IAC LAP */
    case OP(0x03, 0x005B):              /* Write Default Erroneous Data Rep. */
        break;

    /* ---- informational ---- */
    case OP(0x04, 0x0001):              /* Read Local Version Information */
        r[1] = 4;                       /* HCI 2.1 */
        stw_le_p(r + 2, 0x0000);
        r[4] = 4;                       /* LMP 2.1 */
        stw_le_p(r + 5, 15);            /* Broadcom Corporation */
        stw_le_p(r + 7, 0x4185);
        rl = 9;
        break;
    case OP(0x04, 0x0002):              /* Read Local Supported Commands */
        bt_supported_commands(r + 1);
        rl = 65;
        break;
    case OP(0x04, 0x0003):              /* Read Local Supported Features */
        memcpy(r + 1, "\xff\xfe\x8f\xfe\x9b\xff\x79\x83", 8);
        rl = 9;
        break;
    case OP(0x04, 0x0004):              /* Read Local Extended Features */
        r[1] = plen ? p[0] : 0;
        r[2] = 0;                       /* maximum page */
        if (r[1] == 0) {
            memcpy(r + 3, "\xff\xfe\x8f\xfe\x9b\xff\x79\x83", 8);
        }
        rl = 11;
        break;
    case OP(0x04, 0x0005):              /* Read Buffer Size */
        stw_le_p(r + 1, 1021);          /* ACL MTU */
        r[3] = 64;                      /* SCO MTU */
        stw_le_p(r + 4, 8);             /* ACL packets */
        stw_le_p(r + 6, 1);             /* SCO packets */
        rl = 8;
        break;
    case OP(0x04, 0x0009):              /* Read BD_ADDR */
        memcpy(r + 1, s->bdaddr, 6);
        rl = 7;
        break;

    /* ---- Broadcom vendor commands ---- */
    case OP(0x3F, 0x0001):              /* Write BD_ADDR */
        if (plen >= 6) {
            memcpy(s->bdaddr, p, 6);
        }
        break;
    case OP(0x3F, 0x0015):              /* FM register access */
        if (plen < 2) {
            r[0] = 0x12;                /* invalid parameters */
            break;
        }
        r[1] = p[0];
        r[2] = p[1];
        rl = 3 + bcm4329_fm_access(s->fm, p[0], p[1] == 0, p + 2, plen - 2,
                                   r + 3);
        break;
    case OP(0x3F, 0x002E): {            /* Download Minidriver */
        /* the patchram tool waits for two more bytes after the event */
        static const uint8_t two[2] = { 0, 0 };
        bt_cmd_complete(s, op, r, 1);
        bt_send_raw(s, two, 2);
        return;
    }
    case OP(0x3F, 0x004E):              /* Launch RAM: the patch takes over */
        bt_cmd_complete(s, op, r, 1);
        bt_controller_reset(s);
        return;
    case OP(0x3F, 0x0079):              /* Read Verbose Config Version */
        r[1] = 0x01;                    /* chip id */
        r[2] = 0x02;
        stw_le_p(r + 3, 0x0101);        /* build */
        rl = 5;
        break;
    default:
        if ((op >> 10) == 0x3F) {
            /*
             * 0xFC4C Write RAM, 0xFC18 Update Baud Rate, 0xFC27 Sleep Mode,
             * 0xFC1C/0xFC1E SCO PCM, 0xFC6D I2S and the rest: accepted
             */
            break;
        }
        qemu_log_mask(LOG_UNIMP, "bcm4329-bt: unknown HCI command %04x\n",
                      op);
        r[0] = ST_UNKNOWN_CMD;
        break;
    }
    bt_cmd_complete(s, op, r, rl);
}

/* parse every complete H4 packet the host has sent */
static void bt_process(void *opaque)
{
    Bcm4329BtState *s = opaque;
    GByteArray *in = s->in;

    while (in->len) {
        uint8_t t = in->data[0];
        uint32_t need;

        switch (t) {
        case H4_CMD:
            if (in->len < 4) {
                return;
            }
            need = 4 + in->data[3];
            if (in->len < need) {
                return;
            }
            bt_command(s, lduw_le_p(in->data + 1), in->data + 4,
                       in->data[3]);
            break;
        case H4_ACL:
            if (in->len < 5) {
                return;
            }
            need = 5 + lduw_le_p(in->data + 3);
            if (in->len < need) {
                return;
            }
            break;                      /* no links: dropped */
        case H4_SCO:
            if (in->len < 4) {
                return;
            }
            need = 4 + in->data[3];
            if (in->len < need) {
                return;
            }
            break;
        default:
            /* line noise (e.g. a baud change in flight): resync */
            need = 1;
            break;
        }
        g_byte_array_remove_range(in, 0, need);
    }
}

static void bt_uart_tx(void *opaque, const uint8_t *buf, int len)
{
    Bcm4329BtState *s = opaque;

    if (!s->powered) {
        return;
    }
    g_byte_array_append(s->in, buf, len);
    /* the firmware answers a little later, not inside the host's write */
    timer_mod(s->cmd_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              CMD_LATENCY_NS);
}

void bcm4329_bt_set_power(DeviceState *dev, bool on)
{
    Bcm4329BtState *s = BCM4329_BT(dev);

    if (on == s->powered) {
        return;
    }
    s->powered = on;
    g_byte_array_set_size(s->in, 0);
    timer_del(s->cmd_timer);
    timer_del(s->wake_timer);
    qemu_set_irq(s->host_wake, 0);
    bt_controller_reset(s);
    bcm4329_fm_reset(s->fm);
}

void bcm4329_fm_read_audio(DeviceState *dev, int16_t *buf, int frames)
{
    Bcm4329BtState *s = BCM4329_BT(dev);

    if (!s->powered) {
        memset(buf, 0, frames * 4);
        return;
    }
    bcm4329_fm_audio(s->fm, buf, frames);
}

static void bt_reset(DeviceState *dev)
{
    Bcm4329BtState *s = BCM4329_BT(dev);

    s->powered = true;
    bcm4329_bt_set_power(dev, false);
}

static void bt_realize(DeviceState *dev, Error **errp)
{
    Bcm4329BtState *s = BCM4329_BT(dev);
    unsigned int a[6];

    if (!s->bdaddr_prop) {
        s->bdaddr_prop = g_strdup("00:23:76:48:44:33");
    }
    if (sscanf(s->bdaddr_prop, "%x:%x:%x:%x:%x:%x", &a[0], &a[1], &a[2],
               &a[3], &a[4], &a[5]) != 6) {
        error_setg(errp, "bcm4329-bt: bdaddr must be xx:xx:xx:xx:xx:xx");
        return;
    }
    for (int i = 0; i < 6; i++) {
        s->bdaddr[i] = a[5 - i];        /* HCI byte order */
    }
    msm_uartdm_set_peer(s->uart, bt_uart_tx, s);
    s->fm = bcm4329_fm_new(s->fm_stations, bt_fm_irq, s);
}

static void bt_init(Object *obj)
{
    Bcm4329BtState *s = BCM4329_BT(obj);

    s->in = g_byte_array_new();
    s->cmd_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bt_process, s);
    s->wake_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bt_wake_release, s);
    s->inquiry_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bt_inquiry_done, s);
    s->page_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bt_page_done, s);
    qdev_init_gpio_out_named(DEVICE(obj), &s->host_wake, "host-wake", 1);
}

static const Property bt_props[] = {
    DEFINE_PROP_LINK("uart", Bcm4329BtState, uart, TYPE_DEVICE,
                     DeviceState *),
    /* the factory address hd2_mknand.py writes to NAND block 505 */
    DEFINE_PROP_STRING("bdaddr", Bcm4329BtState, bdaddr_prop),
    DEFINE_PROP_STRING("fm-stations", Bcm4329BtState, fm_stations),
};

static void bt_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = bt_realize;
    device_class_set_legacy_reset(dc, bt_reset);
    device_class_set_props(dc, bt_props);
}

static const TypeInfo bcm4329_bt_info = {
    .name = TYPE_BCM4329_BT,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(Bcm4329BtState),
    .instance_init = bt_init,
    .class_init = bt_class_init,
};

static void bcm4329_bt_register(void)
{
    type_register_static(&bcm4329_bt_info);
}
type_init(bcm4329_bt_register)
