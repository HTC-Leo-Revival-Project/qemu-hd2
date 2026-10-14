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
 * With mode=lle (the machine's usb=lle) that host is this machine instead:
 * the same gadget is plugged into vhci_hcd and the host's real USB stack
 * enumerates it, so lsusb, adb and fastboot see an actual USB device.  See
 * htcleo_usbip.c; the register model below is shared by both.
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
#include "qapi/error.h"
#include "system/physmem.h"
#include "htcleo.h"
#include "hw/arm/htcleo/htcleo_usbip.h"

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

/* usb=lle: the root hub port and the speed the gadget advertises */
#define USBIP_PORT           1
#define USBIP_SPEED_HIGH     3

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
    /* max packet size per endpoint, from the descriptors the gadget sent */
    uint16_t mps[NUM_EP];
    /* how far through the configuration descriptor we have walked */
    uint32_t desc_at;
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

    bool usb_lle;                   /* mode=lle: the host is this machine */
    UsbipLink usbip;
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

/*
 * Hand an 8-byte SETUP packet to the gadget's ep0.  The controller's whole
 * job here is to copy it into the queue head's setup_data[8] and set the ep0
 * bit of ENDPTSETUPSTAT; the gadget then decodes it in handle_setup and
 * answers through the normal queues.
 */
static void hsusb_setup_packet(MsmHsusbState *s, const uint8_t *pkt)
{
    qemu_log_mask(LOG_TRACE,
                  "msm-hsusb: setup %02x %02x %04x %04x %04x at %08x "
                  "(eplist %08x)\n",
                  pkt[0], pkt[1], pkt[2] | pkt[3] << 8, pkt[4] | pkt[5] << 8,
                  pkt[6] | pkt[7] << 8, (uint32_t)(qh_addr(s, 0) + 0x28),
                  s->eplist);
    physical_memory_write(qh_addr(s, 0) + 0x28, pkt, 8);
    s->ctl = CTL_IDLE;
    /* a new SETUP packet clears whatever stall the last transfer left */
    s->epctrl[0] &= ~(EPCTRL_RXS | EPCTRL_TXS);
    s->setupstat |= 1;
    s->usbsts |= STS_UI;
    hsusb_update(s);
}

static void host_setup(MsmHsusbState *s, uint8_t type, uint8_t req,
                       uint16_t value, uint16_t index, uint16_t length)
{
    uint8_t pkt[8] = { type, req, value, value >> 8, index, index >> 8,
                       length, length >> 8 };

    hsusb_setup_packet(s, pkt);
    s->ctl_len = 0;
    s->ctl_want = MIN(length, CTL_BUF);
    s->ctl = (type & 0x80) && length ? CTL_DATA_IN : CTL_STATUS_IN;
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

/* ---- mode=lle: this machine is the host ------------------------------ */

/*
 * What a host controller does when it finds a device: connect the port and
 * reset the bus.  LK's udc_interrupt takes the STS_URI branch, flushes every
 * endpoint and comes up offline (UDC_EVENT_OFFLINE), which is exactly what it
 * does on hardware when a host is plugged in.
 */
static void hsusb_bus_connect(MsmHsusbState *s)
{
    s->setupstat = s->stat = s->complete = 0;
    s->deviceaddr = 0;
    s->ctl = CTL_IDLE;
    s->epctrl[0] &= ~(EPCTRL_RXS | EPCTRL_TXS);
    s->portsc |= PORTSC_CCS | PORTSC_PE | PORTSC_HS;
    s->usbsts |= STS_PCI | STS_URI;
    hsusb_update(s);
}

static void hsusb_lle_reset(void *opaque)
{
    hsusb_bus_connect(opaque);
}

static void hsusb_lle_setup(void *opaque, const uint8_t *pkt)
{
    MsmHsusbState *s = opaque;

    /*
     * SET_ADDRESS never gets here -- vhci_hcd completes the first one itself,
     * to turn the port into a real usb_device -- so the gadget's own address
     * stays at whatever the host stack was told, and only the host stack
     * knows it.
     */
    hsusb_setup_packet(s, pkt);
    /*
     * A fresh GET_DESCRIPTOR(CONFIGURATION) starts the descriptor walk over;
     * anything else leaves it, since the host reads the configuration in
     * 9-byte pieces and each one is its own request.
     */
    if (pkt[1] == 0x06 && pkt[2] == 0x00 && pkt[3] == 0x02) {
        s->desc_at = 0;
        s->desc_at = 9;             /* past the configuration header */
    }
}

/*
 * Pick the endpoints' max packet sizes out of the configuration descriptor as
 * the host reads it, so the transfer code can tell a short packet from a
 * whole one.  The stream is a 9 byte configuration header followed by, for
 * each interface, a 9 byte header and then zero or more 7 byte endpoint
 * descriptors -- so the walk only has to know whether it is at an interface
 * boundary (a multiple of 16 from the start of the interface).
 */
static void hsusb_scan_config(MsmHsusbState *s, const uint8_t *buf,
                              uint32_t len)
{
    uint32_t at = s->desc_at;

    while (len) {
        bool boundary = (at % 16) == 0;
        uint32_t need = boundary ? 9 : 7;

        if (len < need) {
            break;
        }
        if (boundary) {
            if (buf[1] != 0x04) {        /* not an interface descriptor */
                break;
            }
        } else {
            uint32_t epn, m;

            if (buf[1] != 0x05) {        /* not an endpoint descriptor */
                break;
            }
            epn = buf[2] & 0x0f;
            m = buf[4] | buf[5] << 8;
            if (epn < NUM_EP) {
                s->mps[epn] = m & 0x7ff;
            }
        }
        at += need;
        buf += need;
        len -= need;
    }
    s->desc_at = at;
}

static int hsusb_lle_poll(void *opaque, int ep, bool in,
                          const uint8_t *out, uint32_t out_len,
                          uint8_t *buf, uint32_t max, bool *more)
{
    MsmHsusbState *s = opaque;

    if (ep < 0 || ep >= NUM_EP) {
        return USBIP_NOPE;
    }
    *more = false;
    /*
     * LK rejects a request it does not implement by writing TXS|RXS into
     * ENDPTCTRL (hsusb.c:476) instead of queueing anything, so the bus has
     * no data to give.  The right answer is a stalled URB, not silence: the
     * host stack reads a silent ep0 as a dead device.
     */
    if (s->epctrl[ep] & (EPCTRL_RXS | EPCTRL_TXS)) {
        return USBIP_STALL;
    }
    /*
     * One round on this endpoint: USBIP_NOPE (-1) is what hsusb_td_xfer()
     * returns when nothing is primed, which is the bus's NAK -- the host
     * retries and the gadget gets another chance as soon as it queues.
     * `max` is the host's wLength and caps the answer, so an IN never comes
     * back longer than the buffer the host stack allocated for it.
     *
     * hsusb_td_xfer() numbers the queue heads 0..15 OUT and 16..31 IN (it
     * indexes ENDPTSTAT and the queue head array with them, so BIT_OUT() and
     * BIT_IN() are the wrong thing to pass -- only its own callers, such as
     * adb_pump() with "16 + s->adb_in", know this).
     */
    {
        int bit = in ? 16 + ep : ep;
        uint32_t left = in ? max : out_len;
        uint32_t done = 0;

        /*
         * NAK unless there is something to move, and decide that *here*.
         * hsusb_td_xfer() clears the primed bit when it finds an empty queue,
         * which is right for the built-in host polling once per packet but
         * fatal here: this runs again and again while the gadget is thinking,
         * so one unlucky look at an endpoint the gadget has just primed and
         * not filled yet would take the prime away and the gadget, which
         * never primes twice, would leave the request unanswered for good.
         */
        if (!(s->stat & (1u << bit)) ||
            ld32(qh_addr(s, bit) + 8) == TD_TERMINATE) {
            return USBIP_NOPE;
        }

        /*
         * One host transfer is several bus packets: fastboot hands over 16 KB
         * at a time while the gadget queues 4096-byte descriptors, so walk
         * the chain until the host's buffer is spent or the gadget runs dry.
         *
         * The first round happens even when `left` is zero: a control
         * transfer's status stage is exactly that, a zero-length packet whose
         * only job is to retire the descriptor the gadget primed for it.
         */
        for (;;) {
            int n = hsusb_td_xfer(s, bit,
                                  in ? buf + done : (uint8_t *)out + done,
                                  left);

            if (n < 0) {
                break;
            }
            /*
             * A primed transfer descriptor of zero length is the gadget
             * saying "that was the last packet": a zero-length packet ends
             * the transfer just as a short one does.
             */
            if (!n) {
                break;
            }
            done += n;
            left -= n;
            if (!left) {
                break;
            }
        }
        /*
         * Running out of primed descriptors with `left` to go is not the end
         * of the transfer: the gadget simply has not queued the rest yet.  A
         * bulk IN ends at the first packet shorter than the endpoint's max
         * packet size, so as long as what we have moved is a whole number of
         * packets the host has seen nothing that would end it, and finishing
         * here would strand the transfer short.
         *
         * Control reads are the exception: a descriptor that is shorter than
         * wLength ends the data stage, and that is how the host is told a
         * descriptor ran out.
         */
        if (in && ep == 0 && done) {
            hsusb_scan_config(s, buf, done);
        }
        if (in && left && done) {
            uint32_t mps = s->mps[ep] ? s->mps[ep] : 64;

            if (ep && !(done % mps)) {
                *more = true;
            }
        }
        return done;
    }
}

static const UsbipOps hsusb_lle_ops = {
    .reset = hsusb_lle_reset,
    .setup = hsusb_lle_setup,
    .poll = hsusb_lle_poll,
};

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

    if (s->usb_lle) {
        /* the host is this machine, so there is no built-in one to run */
        return;
    }
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
    if (!s->usb_lle) {
        host_detach(s);
    }
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
            if (s->usb_lle) {
                /*
                 * The gadget restarting its controller is a device falling
                 * off the bus and coming back: the host has to enumerate it
                 * again, or it keeps talking to the instance that is gone.
                 * cLK does this on every boot (udc_init() resets the core
                 * before udc_start()), so without this the emulated phone
                 * would only be usable before its first reboot.
                 */
                usbip_link_reset(&s->usbip);
                hsusb_bus_connect(s);
            }
            return;
        }
        if (!(s->usbcmd & CMD_RS) && (val & CMD_RS) &&
            s->hstate == H_DETACHED) {
            /* hosts debounce a new connection for ~100 ms */
            s->hdelay_until = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                              100 * SCALE_MS;
        }
        s->usbcmd = val;
        if (s->usb_lle) {
            /* the real host drives the bus; there is nothing to kick */
        } else if (hsusb_running(s)) {
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
        if (!s->usb_lle && hsusb_running(s)) {
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
        if (s->usb_lle) {
            qemu_log_mask(LOG_TRACE,
                          "msm-hsusb: prime %08x -> %08x (qh0 %08x qh1o %08x "
                          "qh1i %08x)\n", (uint32_t)val, (uint32_t)(s->stat | val),
                          ld32(qh_addr(s, 0) + 8), ld32(qh_addr(s, 1) + 8),
                          ld32(qh_addr(s, 17) + 8));
        }
        s->stat |= val;
        if (s->usb_lle) {
            /* the gadget is ready: the host may be waiting for exactly this */
            usbip_link_kick(&s->usbip);
        } else {
            hsusb_kick(s, 10 * SCALE_US);
        }
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
            if (s->usb_lle) {
                /*
                 * A stall is an answer in itself -- the gadget has given up
                 * on the request -- so hand it to the host now rather than
                 * waiting for a transfer that will never be queued.
                 */
                usbip_link_kick(&s->usbip);
            } else if (n == 0 && (val & (EPCTRL_RXS | EPCTRL_TXS)) &&
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
    if (s->usb_lle && s->usbip.attached) {
        /*
         * The bridge already put us on the host's bus and reset it at
         * realize; this reset came after and dropped the connect, so put it
         * back.  A machine reset is a bus reset for the gadget either way.
         */
        hsusb_bus_connect(s);
    }
}

static void msm_hsusb_realize(DeviceState *dev, Error **errp)
{
    MsmHsusbState *s = MSM_HSUSB(dev);

    fifo8_create(&s->rx, RX_FIFO);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, hsusb_timer, s);
    if (s->usb_lle) {
        /*
         * Plug the gadget into this machine's own USB stack.  This has to
         * happen now, while we are still the process that owns the socket:
         * vhci_hcd resolves the fd against the writing task's fd table, and
         * the attribute is root-only.
         */
        usbip_link_init(&s->usbip, &hsusb_lle_ops, s, USBIP_PORT,
                        USBIP_SPEED_HIGH, errp);
        if (*errp) {
            return;
        }
        return;
    }
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

static char *hsusb_get_mode(Object *obj, Error **errp)
{
    MsmHsusbState *s = MSM_HSUSB(obj);

    return g_strdup(s->usb_lle ? "lle" : "hle");
}

static void hsusb_set_mode(Object *obj, const char *v, Error **errp)
{
    MsmHsusbState *s = MSM_HSUSB(obj);

    if (!strcmp(v, "lle")) {
        s->usb_lle = true;
    } else if (!strcmp(v, "hle")) {
        s->usb_lle = false;
    } else {
        error_setg(errp, "mode must be 'hle' or 'lle'");
    }
}

static void msm_hsusb_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = msm_hsusb_realize;
    device_class_set_legacy_reset(dc, msm_hsusb_reset);
    device_class_set_props(dc, hsusb_props);
    object_class_property_add_str(oc, "mode", hsusb_get_mode, hsusb_set_mode);
    object_class_property_set_description(oc, "mode",
        "who is the USB host: hle (built in, adb goes to the 'adb' chardev) "
        "or lle (this machine's own, through vhci_hcd; needs root)");
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
