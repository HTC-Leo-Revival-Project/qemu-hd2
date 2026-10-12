/*
 * Qualcomm MSM high speed USB controller (ChipIdea core, "msm72k"), device
 * mode, as driven by drivers/usb/gadget/msm72k_udc.c, plus the USB host on
 * the other end of the cable.
 *
 * Device controller: the standard ChipIdea/EHCI-style register set at
 * +0x100 (USBCMD, USBSTS, USBINTR, DEVICEADDR, ENDPOINTLISTADDR, PORTSC,
 * OTGSC, USBMODE, ENDPTSETUPSTAT/PRIME/FLUSH/STAT/COMPLETE, ENDPTCTRLn)
 * and the ULPI viewport.  Transfers use queue heads (64 bytes each, two
 * per endpoint: OUT then IN) at ENDPOINTLISTADDR and chained transfer
 * descriptors (next, token, 5 page pointers).
 *
 * Host: once the gadget enables its D+ pull-up with VBUS present, a small
 * built-in host resets the bus, enumerates the device (device and
 * configuration descriptors, address, configuration), finds the adb
 * interface (class 0xFF, subclass 0x42, protocol 1) and then pipes its
 * bulk endpoints to the "adb" chardev.  adb uses the same packets over
 * USB and TCP, so
 *
 *   -chardev socket,id=adb,host=127.0.0.1,port=5555,server=on,wait=off
 *   -global msm-hsusb.adb=adb
 *
 * followed by "adb connect 127.0.0.1:5555" gives a working adb.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/fifo8.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "system/physmem.h"
#include "htcleo.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmHsusbState, MSM_HSUSB)

#define R_ID                0x000
#define R_HWGENERAL         0x004
#define R_HWHOST            0x008
#define R_HWDEVICE          0x00C
#define R_HWTXBUF           0x010
#define R_HWRXBUF           0x014
#define R_CAPLENGTH         0x100
#define R_HCSPARAMS         0x104
#define R_DCIVERSION        0x120
#define R_DCCPARAMS         0x124
#define R_USBCMD            0x140
#define R_USBSTS            0x144
#define R_USBINTR           0x148
#define R_FRINDEX           0x14C
#define R_DEVICEADDR        0x154
#define R_EPLISTADDR        0x158
#define R_ULPI              0x170
#define R_PORTSC            0x184
#define R_OTGSC             0x1A4
#define R_USBMODE           0x1A8
#define R_SETUPSTAT         0x1AC
#define R_PRIME             0x1B0
#define R_FLUSH             0x1B4
#define R_STAT              0x1B8
#define R_COMPLETE          0x1BC
#define R_EPCTRL0           0x1C0
#define NUM_EP              16

#define CMD_RS              (1u << 0)
#define CMD_RST             (1u << 1)

#define STS_UI              (1u << 0)
#define STS_PCI             (1u << 2)
#define STS_URI             (1u << 6)
#define STS_SLI             (1u << 8)

#define PORTSC_CCS          (1u << 0)
#define PORTSC_PE           (1u << 2)
#define PORTSC_PHCD         (1u << 23)
#define PORTSC_HS           (2u << 26)

#define OTGSC_ID            (1u << 8)
#define OTGSC_AVV           (1u << 9)
#define OTGSC_ASV           (1u << 10)
#define OTGSC_BSV           (1u << 11)

#define ULPI_RUN            (1u << 30)
#define ULPI_WRITE          (1u << 29)

#define TD_TERMINATE        1u
#define TD_ACTIVE           (1u << 7)
#define TD_IOC              (1u << 15)

#define EPCTRL_RXS          (1u << 0)
#define EPCTRL_TXS          (1u << 16)

#define BIT_OUT(n)          (1u << (n))
#define BIT_IN(n)           (1u << ((n) + 16))

#define RX_FIFO             65536
#define CTL_BUF             1024

/* the built-in host */
enum {
    H_DETACHED,
    H_RESET,
    H_GET_DEV,
    H_SET_ADDR,
    H_GET_CFG9,
    H_GET_CFG,
    H_SET_CFG,
    H_ONLINE,
    H_FAILED,
};

/* control transfer stages */
enum { CTL_IDLE, CTL_DATA_IN, CTL_STATUS_OUT, CTL_STATUS_IN };

struct MsmHsusbState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer *timer;
    CharFrontend adb;
    Fifo8 rx;

    uint32_t usbcmd, usbsts, usbintr, deviceaddr, eplist;
    uint32_t portsc, otgsc, usbmode, ulpi_vp;
    uint32_t setupstat, stat, complete;
    uint32_t epctrl[NUM_EP];
    uint8_t ulpi[256];
    uint32_t ahb[0x40];             /* 0x000..0x0FC misc readback */

    int hstate;
    int64_t hdelay_until;
    int ctl;
    uint8_t ctl_buf[CTL_BUF];
    uint32_t ctl_len, ctl_want;
    int adb_in, adb_out;            /* endpoint numbers, -1 if none */
    uint8_t cfg_value;
    bool adb_connected;
};

static uint32_t ld32(hwaddr a)
{
    uint32_t v;

    physical_memory_read(a, &v, 4);
    return le32_to_cpu(v);
}

static void st32(hwaddr a, uint32_t v)
{
    v = cpu_to_le32(v);
    physical_memory_write(a, &v, 4);
}

static void hsusb_update(MsmHsusbState *s)
{
    qemu_set_irq(s->irq, !!(s->usbsts & s->usbintr));
}

static void hsusb_kick(MsmHsusbState *s, int64_t delay_ns)
{
    int64_t when = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay_ns;

    if (!timer_pending(s->timer) || timer_expire_time_ns(s->timer) > when) {
        timer_mod(s->timer, when);
    }
}

static bool hsusb_running(MsmHsusbState *s)
{
    return (s->usbcmd & CMD_RS) && !(s->portsc & PORTSC_PHCD);
}

/* queue head of endpoint bit (0..15 OUT, 16..31 IN) */
static hwaddr qh_addr(MsmHsusbState *s, int bit)
{
    int num = bit & 15, in = bit >> 4;

    return (s->eplist & ~0x7FFu) + (num * 2 + in) * 64;
}

static hwaddr td_byte_addr(hwaddr td, uint32_t off)
{
    uint32_t page0 = ld32(td + 8);
    uint32_t abs = (page0 & 0xFFF) + off;
    int i = abs >> 12;

    if (i == 0) {
        return page0 + off;
    }
    if (i > 4) {
        return 0;
    }
    return (ld32(td + 8 + 4 * i) & ~0xFFFu) + (abs & 0xFFF);
}

/*
 * Move data through the current transfer descriptor of endpoint bit.
 * IN (device -> host): copy up to max bytes into buf.  OUT: copy len bytes
 * from buf into the descriptor (a short packet ends it).  Returns the byte
 * count, or -1 if nothing is queued on the endpoint.
 */
static int hsusb_td_xfer(MsmHsusbState *s, int bit, uint8_t *buf,
                         uint32_t len)
{
    hwaddr qh = qh_addr(s, bit), td;
    uint32_t next, token, total, n, o;
    bool in = bit >= 16;

    if (!(s->stat & (1u << bit))) {
        return -1;
    }
    td = ld32(qh + 8);
    if (td & TD_TERMINATE) {
        s->stat &= ~(1u << bit);
        return -1;
    }
    td &= ~0x1Fu;
    token = ld32(td + 4);
    total = (token >> 16) & 0x7FFF;
    n = MIN(total, len);
    for (o = 0; o < n; ) {
        hwaddr a = td_byte_addr(td, o);
        uint32_t chunk = MIN(n - o, 0x1000 - (a & 0xFFF));

        if (!a) {
            break;
        }
        if (in) {
            physical_memory_read(a, buf + o, chunk);
        } else {
            physical_memory_write(a, buf + o, chunk);
        }
        o += chunk;
    }
    token = (token & ~((0x7FFFu << 16) | TD_ACTIVE | 0x6F)) |
            ((total - n) << 16);
    st32(td + 4, token);

    /* advance the queue head */
    next = ld32(td);
    st32(qh + 4, td);
    st32(qh + 8, next);
    st32(qh + 12, token);
    if (token & TD_IOC) {
        s->complete |= 1u << bit;
        s->usbsts |= STS_UI;
    }
    if (next & TD_TERMINATE) {
        s->stat &= ~(1u << bit);
    }
    hsusb_update(s);
    return n;
}

/* ---- host side: control transfers ---------------------------------- */

static void host_setup(MsmHsusbState *s, uint8_t type, uint8_t req,
                       uint16_t value, uint16_t index, uint16_t length)
{
    uint8_t pkt[8] = { type, req, value, value >> 8, index, index >> 8,
                       length, length >> 8 };

    physical_memory_write(qh_addr(s, 0) + 0x28, pkt, 8);
    s->ctl_len = 0;
    s->ctl_want = MIN(length, CTL_BUF);
    s->ctl = (type & 0x80) && length ? CTL_DATA_IN : CTL_STATUS_IN;
    s->epctrl[0] &= ~(EPCTRL_RXS | EPCTRL_TXS);
    s->setupstat |= 1;
    s->usbsts |= STS_UI;
    hsusb_update(s);
}

/* advance the control transfer; true once it has finished */
static bool host_ctl_step(MsmHsusbState *s)
{
    int n;

    switch (s->ctl) {
    case CTL_DATA_IN:
        n = hsusb_td_xfer(s, 16, s->ctl_buf + s->ctl_len,
                          s->ctl_want - s->ctl_len);
        if (n < 0) {
            return false;
        }
        s->ctl_len += n;
        /* a short packet or the full length ends the data stage */
        s->ctl = CTL_STATUS_OUT;
        return false;
    case CTL_STATUS_OUT:
        if (hsusb_td_xfer(s, 0, NULL, 0) < 0) {
            return false;
        }
        s->ctl = CTL_IDLE;
        return true;
    case CTL_STATUS_IN:
        if (hsusb_td_xfer(s, 16, NULL, 0) < 0) {
            return false;
        }
        s->ctl = CTL_IDLE;
        return true;
    }
    return false;
}

static void host_parse_config(MsmHsusbState *s)
{
    uint32_t i = 0;
    bool adb = false;

    s->adb_in = s->adb_out = -1;
    s->cfg_value = s->ctl_len >= 6 ? s->ctl_buf[5] : 1;
    while (i + 2 <= s->ctl_len && s->ctl_buf[i] >= 2) {
        const uint8_t *d = s->ctl_buf + i;

        if (d[1] == 4 && i + 9 <= s->ctl_len) {
            adb = d[5] == 0xFF && d[6] == 0x42 && d[7] == 0x01;
        } else if (d[1] == 5 && adb && (d[3] & 3) == 2) {
            if (d[2] & 0x80) {
                s->adb_in = d[2] & 15;
            } else {
                s->adb_out = d[2] & 15;
            }
        }
        i += d[0];
    }
    qemu_log_mask(LOG_TRACE, "msm-hsusb: config %u, adb in %d out %d\n",
                  s->cfg_value, s->adb_in, s->adb_out);
}

static void host_detach(MsmHsusbState *s)
{
    s->hstate = H_DETACHED;
    s->ctl = CTL_IDLE;
    s->portsc &= ~(PORTSC_CCS | PORTSC_PE);
}

/* ---- adb pipe -------------------------------------------------------- */

static bool adb_ready(MsmHsusbState *s)
{
    return s->hstate == H_ONLINE && s->adb_in > 0 && s->adb_out > 0;
}

static void adb_pump(MsmHsusbState *s)
{
    uint8_t buf[16384];
    int n;

    if (!adb_ready(s)) {
        return;
    }
    /* device -> host */
    while ((n = hsusb_td_xfer(s, 16 + s->adb_in, buf, sizeof(buf))) >= 0) {
        if (n && s->adb_connected) {
            qemu_chr_fe_write_all(&s->adb, buf, n);
        }
    }
    /* host -> device */
    while (!fifo8_is_empty(&s->rx) && (s->stat & BIT_OUT(s->adb_out))) {
        uint32_t td = ld32(qh_addr(s, s->adb_out) + 8);
        uint32_t want, avail = fifo8_num_used(&s->rx), got = 0;

        if (td & TD_TERMINATE) {
            s->stat &= ~BIT_OUT(s->adb_out);
            break;
        }
        want = (ld32((td & ~0x1Fu) + 4) >> 16) & 0x7FFF;
        want = MIN(MIN(want, avail), sizeof(buf));
        while (got < want) {
            uint32_t chunk;
            const uint8_t *p = fifo8_pop_bufptr(&s->rx, want - got, &chunk);

            memcpy(buf + got, p, chunk);
            got += chunk;
        }
        if (hsusb_td_xfer(s, s->adb_out, buf, got) < 0) {
            break;
        }
    }
    qemu_chr_fe_accept_input(&s->adb);
}

static int adb_can_receive(void *opaque)
{
    MsmHsusbState *s = opaque;

    return fifo8_num_free(&s->rx);
}

static void adb_receive(void *opaque, const uint8_t *buf, int size)
{
    MsmHsusbState *s = opaque;

    fifo8_push_all(&s->rx, buf, MIN(size, fifo8_num_free(&s->rx)));
    hsusb_kick(s, 0);
}

static void adb_event(void *opaque, QEMUChrEvent event)
{
    MsmHsusbState *s = opaque;

    if (event == CHR_EVENT_OPENED) {
        s->adb_connected = true;
    } else if (event == CHR_EVENT_CLOSED) {
        s->adb_connected = false;
        fifo8_reset(&s->rx);
    }
}

/* ---- the host state machine (timer) ---------------------------------- */

static void hsusb_timer(void *opaque)
{
    MsmHsusbState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!hsusb_running(s)) {
        if (s->hstate != H_DETACHED) {
            host_detach(s);
        }
        return;
    }
    if (now < s->hdelay_until) {
        timer_mod(s->timer, s->hdelay_until);
        return;
    }

    switch (s->hstate) {
    case H_DETACHED:
        /* connect, then reset the bus */
        s->portsc |= PORTSC_CCS | PORTSC_PE | PORTSC_HS;
        s->usbsts |= STS_PCI | STS_URI;
        hsusb_update(s);
        s->hstate = H_RESET;
        s->hdelay_until = now + 20 * SCALE_MS;
        break;
    case H_RESET:
        host_setup(s, 0x80, 6, 0x0100, 0, 64);      /* GET_DESCRIPTOR dev */
        s->hstate = H_GET_DEV;
        break;
    case H_GET_DEV:
        if (host_ctl_step(s)) {
            host_setup(s, 0x00, 5, 7, 0, 0);        /* SET_ADDRESS 7 */
            s->hstate = H_SET_ADDR;
        }
        break;
    case H_SET_ADDR:
        if (host_ctl_step(s)) {
            host_setup(s, 0x80, 6, 0x0200, 0, 9);   /* config header */
            s->hstate = H_GET_CFG9;
        }
        break;
    case H_GET_CFG9:
        if (host_ctl_step(s)) {
            uint16_t total = s->ctl_len >= 4 ?
                             lduw_le_p(s->ctl_buf + 2) : 9;

            host_setup(s, 0x80, 6, 0x0200, 0, MIN(total, CTL_BUF));
            s->hstate = H_GET_CFG;
        }
        break;
    case H_GET_CFG:
        if (host_ctl_step(s)) {
            host_parse_config(s);
            host_setup(s, 0x00, 9, s->cfg_value, 0, 0);
            s->hstate = H_SET_CFG;
        }
        break;
    case H_SET_CFG:
        if (host_ctl_step(s)) {
            s->hstate = H_ONLINE;
        }
        break;
    case H_ONLINE:
        adb_pump(s);
        break;
    }
    if (s->hstate != H_ONLINE && s->hstate != H_FAILED) {
        /* poll the control pipe until the gadget answers */
        hsusb_kick(s, SCALE_MS);
    }
}

/* ---- registers --------------------------------------------------------- */

static void hsusb_ctrl_reset(MsmHsusbState *s)
{
    s->usbcmd = 0x00080000;
    s->usbsts = 0;
    s->usbintr = 0;
    s->deviceaddr = 0;
    s->eplist = 0;
    s->setupstat = s->stat = s->complete = 0;
    memset(s->epctrl, 0, sizeof(s->epctrl));
    s->epctrl[0] = (1u << 23) | (1u << 7);  /* ep0 always enabled */
    s->portsc &= PORTSC_PHCD;
    s->portsc |= 2u << 30;                  /* ULPI */
    host_detach(s);
    hsusb_update(s);
}

static uint64_t hsusb_read(void *opaque, hwaddr off, unsigned size)
{
    MsmHsusbState *s = opaque;
    uint32_t v;

    switch (off & ~3) {
    case R_ID:
        return 0x0042FA05;
    case R_HWGENERAL:
        return 0x00000035;
    case R_HWHOST:
        return 0x10020001;
    case R_HWDEVICE:
        return (NUM_EP << 1) | 1;
    case R_HWTXBUF:
        return 0x80060A10;
    case R_HWRXBUF:
        return 0x00000A10;
    case R_CAPLENGTH:
        v = 0x01000040;
        return size == 4 ? v : (v >> ((off & 3) * 8)) & 0xFFFF;
    case R_HCSPARAMS:
        return 0x00010011;
    case R_DCIVERSION:
        return 0x0001;
    case R_DCCPARAMS:
        return 0x00000180 | NUM_EP;
    case R_USBCMD:
        return s->usbcmd;
    case R_USBSTS:
        return s->usbsts;
    case R_USBINTR:
        return s->usbintr;
    case R_FRINDEX:
        return (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 125000) & 0x3FFF;
    case R_DEVICEADDR:
        return s->deviceaddr;
    case R_EPLISTADDR:
        return s->eplist;
    case R_ULPI:
        return s->ulpi_vp;
    case R_PORTSC:
        return s->portsc;
    case R_OTGSC:
        return s->otgsc | OTGSC_ID | OTGSC_AVV | OTGSC_ASV | OTGSC_BSV;
    case R_USBMODE:
        return s->usbmode;
    case R_SETUPSTAT:
        return s->setupstat;
    case R_PRIME:
    case R_FLUSH:
        return 0;
    case R_STAT:
        return s->stat;
    case R_COMPLETE:
        return s->complete;
    }
    if (off >= R_EPCTRL0 && off < R_EPCTRL0 + 4 * NUM_EP) {
        return s->epctrl[(off - R_EPCTRL0) >> 2];
    }
    if (off < 0x100) {
        return s->ahb[off >> 2];
    }
    return 0;
}

static void hsusb_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MsmHsusbState *s = opaque;

    switch (off) {
    case R_USBCMD:
        if (val & CMD_RST) {
            hsusb_ctrl_reset(s);
            return;
        }
        if (!(s->usbcmd & CMD_RS) && (val & CMD_RS) &&
            s->hstate == H_DETACHED) {
            /* hosts debounce a new connection for ~100 ms */
            s->hdelay_until = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                              100 * SCALE_MS;
        }
        s->usbcmd = val;
        if (hsusb_running(s)) {
            hsusb_kick(s, 10 * SCALE_MS);
        } else if (s->hstate != H_DETACHED) {
            host_detach(s);
        }
        break;
    case R_USBSTS:
        s->usbsts &= ~val;
        break;
    case R_USBINTR:
        s->usbintr = val;
        break;
    case R_DEVICEADDR:
        s->deviceaddr = val;
        break;
    case R_EPLISTADDR:
        s->eplist = val & ~0x7FFu;
        break;
    case R_ULPI:
        if (val & ULPI_RUN) {
            uint8_t reg = (val >> 16) & 0xFF;

            if (val & ULPI_WRITE) {
                s->ulpi[reg] = val & 0xFF;
                s->ulpi_vp = val & ~ULPI_RUN;
            } else {
                s->ulpi_vp = (val & ~ULPI_RUN & ~0xFF00u) |
                             (s->ulpi[reg] << 8);
            }
        } else {
            s->ulpi_vp = val;
        }
        break;
    case R_PORTSC:
        /* PHCD (PHY suspend) and the port test controls are writable */
        s->portsc = (s->portsc & ~(PORTSC_PHCD | (3u << 30))) |
                    (val & (PORTSC_PHCD | (3u << 30)));
        if (hsusb_running(s)) {
            hsusb_kick(s, 10 * SCALE_MS);
        }
        break;
    case R_OTGSC:
        s->otgsc = val & 0x7F00002F;
        break;
    case R_USBMODE:
        s->usbmode = val;
        break;
    case R_SETUPSTAT:
        s->setupstat &= ~val;
        break;
    case R_PRIME:
        /* the controller loads the queue heads at once */
        s->stat |= val;
        hsusb_kick(s, 10 * SCALE_US);
        break;
    case R_FLUSH:
        s->stat &= ~val;
        break;
    case R_COMPLETE:
        s->complete &= ~val;
        break;
    default:
        if (off >= R_EPCTRL0 && off < R_EPCTRL0 + 4 * NUM_EP) {
            int n = (off - R_EPCTRL0) >> 2;

            s->epctrl[n] = val;
            if (n == 0 && (val & (EPCTRL_RXS | EPCTRL_TXS)) &&
                s->ctl != CTL_IDLE) {
                /* the gadget stalled a request: carry on without it */
                qemu_log_mask(LOG_TRACE, "msm-hsusb: ep0 stall\n");
                s->ctl = CTL_IDLE;
                s->ctl_len = 0;
                if (s->hstate < H_ONLINE) {
                    s->hstate = s->hstate == H_SET_CFG ? H_ONLINE : H_FAILED;
                }
            }
        } else if (off < 0x100) {
            s->ahb[off >> 2] = val;
        }
        break;
    }
    hsusb_update(s);
}

static const MemoryRegionOps hsusb_ops = {
    .read = hsusb_read,
    .write = hsusb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void msm_hsusb_reset(DeviceState *dev)
{
    MsmHsusbState *s = MSM_HSUSB(dev);

    memset(s->ulpi, 0, sizeof(s->ulpi));
    s->ulpi[0] = 0x24;      /* vendor id: SMSC-like */
    s->ulpi[1] = 0x04;
    s->ulpi[2] = 0x04;
    s->ulpi[3] = 0x00;
    memset(s->ahb, 0, sizeof(s->ahb));
    s->ulpi_vp = 0;
    s->usbmode = 0;
    s->otgsc = 0;
    s->portsc = 0;
    hsusb_ctrl_reset(s);
    fifo8_reset(&s->rx);
}

static void msm_hsusb_realize(DeviceState *dev, Error **errp)
{
    MsmHsusbState *s = MSM_HSUSB(dev);

    fifo8_create(&s->rx, RX_FIFO);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, hsusb_timer, s);
    qemu_chr_fe_set_handlers(&s->adb, adb_can_receive, adb_receive,
                             adb_event, NULL, s, NULL, true);
}

static void msm_hsusb_init(Object *obj)
{
    MsmHsusbState *s = MSM_HSUSB(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &hsusb_ops, s, "msm-hsusb",
                          0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static const Property hsusb_props[] = {
    DEFINE_PROP_CHR("adb", MsmHsusbState, adb),
};

static void msm_hsusb_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = msm_hsusb_realize;
    device_class_set_legacy_reset(dc, msm_hsusb_reset);
    device_class_set_props(dc, hsusb_props);
}

static const TypeInfo msm_hsusb_info = {
    .name = TYPE_MSM_HSUSB,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmHsusbState),
    .instance_init = msm_hsusb_init,
    .class_init = msm_hsusb_class_init,
};

static void msm_hsusb_register(void)
{
    type_register_static(&msm_hsusb_info);
}
type_init(msm_hsusb_register)
