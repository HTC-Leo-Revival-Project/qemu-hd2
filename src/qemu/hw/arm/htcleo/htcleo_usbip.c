/*
 * Exposing the emulated HD2's USB gadget to this host's own USB stack.
 *
 * msm_hsusb.c models the device end: setup packets, descriptor enumeration,
 * the guest's queues.  What it is missing in HLE is a *host* on the other end
 * of the cable, so the model carries a small one that enumerates the gadget
 * itself and pipes adb to a chardev.  With usb=lle that host is this machine:
 * the gadget is plugged into vhci_hcd, the kernel's virtual host controller,
 * and the host's real USB stack on top of it does the enumeration, so lsusb,
 * adb and fastboot see an actual USB device instead of a private protocol.
 *
 * How the attach works
 * --------------------
 * vhci_hcd is the classic USB/IP virtual host controller.  It exports its
 * ports through sysfs on its platform device, not through the /dev/vhci-hcd
 * char device (which this kernel does not build):
 *
 *     echo "<port> <sockfd> <devid> <speed>" > \
 *          /sys/bus/platform/devices/vhci_hcd.0/attach
 *
 * with `port` the port number as /sys/.../status prints it (0 is the first
 * high-speed root hub port) and `speed` 1 = low, 2 = full, 3 = high.  Two
 * details are easy to get wrong and both look like "USB never enumerates":
 *
 *  - the fd is resolved with sockfd_lookup() against the *writing* task's fd
 *    table, so the bridge has to own the socket and write the file itself:
 *    it must run as root, and no `sudo sh -c 'echo ... > attach'` works;
 *  - the kernel's own messages arrive on the *other* end of the connection.
 *    Reading the socket that was handed in shows nothing at all.
 *
 * So the bridge makes a socketpair, hands one end over and speaks USB/IP on
 * the other.  Once the kernel's stack drives enumeration by submitting a URB
 * per transfer, the wire protocol is four fixed-size messages
 * (drivers/usb/usbip/usbip-common.h), all big-endian:
 *
 *     USBIP_CMD_SUBMIT   1  the host stack submitted a URB
 *     USBIP_CMD_UNLINK   2  ... and now wants it back
 *     USBIP_RET_SUBMIT   3  our answer, with the data of an IN transfer
 *     USBIP_RET_UNLINK   4
 *
 *     struct usbip_header_basic {  20 bytes
 *         __u32 command, seqnum, devid, direction, ep;
 *     };
 *     union {                       28 bytes
 *         struct { __u32 transfer_flags;
 *                  __s32 transfer_buffer_length;
 *                  __s32 start_frame, number_of_packets, interval;
 *                  __u8  setup[8]; } cmd_submit;
 *         struct { __s32 status, actual_length, start_frame,
 *                  number_of_packets, error_count; } ret_submit;
 *         struct { __u32 seqnum; } cmd_unlink;
 *         struct { __s32 status; } ret_unlink;
 *     } u;
 *
 * vhci_send_cmd_submit() sends all 48 bytes for every message, zero-padding
 * the union arms it does not use, so a frame is always 48 bytes plus its
 * data.  The data rides along only for OUT transfers -- vhci_tx.c adds the
 * transfer buffer to the iovec under !usb_pipein(urb->pipe) -- so an IN
 * answer is a bare header followed by actual_length bytes, while a zero-length
 * OUT (a control transfer's status stage) is the header alone.  Getting that
 * wrong leaves the kernel's byte stream permanently misaligned and it drops
 * the connection with "unknown pdu".
 *
 * What the host never asks us
 * --------------------------
 * vhci_hcd answers the first two requests of every enumeration itself: the
 * GET_DESCRIPTOR(DEVICE) to address 0 that only learns the ep0 max packet
 * size, and SET_ADDRESS, which it completes locally and uses to turn the
 * port into vdev->udev.  Everything after that -- the real descriptors, the
 * configuration, the class requests and all bulk traffic -- is forwarded to
 * us, so the gadget still has to be a complete USB device.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/sockets.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"

#include <sys/un.h>

#include "hw/arm/htcleo/htcleo_usbip.h"

#define USBIP_ATTACH     "/sys/bus/platform/devices/vhci_hcd.0/attach"
#define USBIP_DETACH     "/sys/bus/platform/devices/vhci_hcd.0/detach"

#define CMD_SUBMIT       1
#define CMD_UNLINK       2
#define RET_SUBMIT       3
#define RET_UNLINK       4

#define DIR_OUT          0
#define DIR_IN           1

#define EPIPE_           (-32)
#define ETIMEDOUT_       (-110)
#define ECONNRESET_      (-104)

/* how often to look for a gadget that may have become ready */
#define POLL_NS          (SCALE_MS / 10)
/* ... and when to give up on a URB the gadget never answers */
#define TIMEOUT_NS       (2000 * SCALE_MS)
/* ... and when a transfer that was moving has stopped, to say so */
#define STUCK_NS         (5000 * SCALE_MS)

static inline uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static inline void put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

/* ---- the wire ------------------------------------------------------- */

static void usbip_send(UsbipLink *l, const uint8_t *buf, size_t len)
{
    size_t done = 0;

    while (done < len) {
        ssize_t n = write(l->peer, buf + done, len - done);

        if (n > 0) {
            done += n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        /*
         * A short write would leave the kernel's stream misaligned, which it
         * reports as a broken connection -- exactly what it is.  The kernel's
         * rx thread does nothing but a blocking recv, so this is not expected.
         */
        qemu_log_mask(LOG_TRACE,
                      "msm-usbip: short write %zu/%zu: %s\n",
                      done, len, strerror(errno));
        return;
    }
}

/* the 20-byte basic header every message starts with */
static void usbip_send_head(UsbipLink *l, uint8_t *buf, uint32_t command,
                            uint32_t seq, uint32_t dir, uint32_t ep)
{
    put32(buf + 0, command);
    put32(buf + 4, seq);
    put32(buf + 8, l->devid);
    put32(buf + 12, dir);
    put32(buf + 16, ep);
    memset(buf + 20, 0, USBIP_HDR_SIZE - 20);
}

static void usbip_ret_submit(UsbipLink *l, uint32_t seq, uint32_t ep,
                             bool in, const uint8_t *data, uint32_t len,
                             int32_t status)
{
    /*
     * actual_length is the byte count of the transfer in both directions:
     * for an OUT it is how much the device received, and handing that back
     * is not a detail -- USBDEVFS_BULK returns it as `idx`, which the host's
     * fastboot reads as "Write to device failed" when it comes up short.
     */
    uint8_t buf[USBIP_HDR_SIZE];

    usbip_send_head(l, buf, RET_SUBMIT, seq, in ? DIR_IN : DIR_OUT, ep);
    put32(buf + 20, status);
    put32(buf + 24, len);
    usbip_send(l, buf, sizeof(buf));
    if (in && len) {
        usbip_send(l, data, len);
    }
    qemu_log_mask(LOG_TRACE,
                  "msm-usbip: -> RET_SUBMIT seq %u ep %u %s len %u "
                  "status %d\n", seq, ep, in ? "in" : "out", len, status);
}

static void usbip_ret_unlink(UsbipLink *l, uint32_t seq, int32_t status)
{
    uint8_t buf[USBIP_HDR_SIZE];

    usbip_send_head(l, buf, RET_UNLINK, seq, DIR_OUT, 0);
    put32(buf + 20, status);
    usbip_send(l, buf, sizeof(buf));
    qemu_log_mask(LOG_TRACE,
                  "msm-usbip: -> RET_UNLINK seq %u status %d\n", seq, status);
}

/* ---- the bus --------------------------------------------------------- */

/* usbip_link_open() lives with the rest of the attach code, further down */
static int usbip_link_open(UsbipLink *l, Error **errp);

static void usbip_pop(UsbipLink *l)
{
    l->head = (l->head + 1) % USBIP_MAX_URBS;
    l->nurb--;
}

/* drop the URB at offset `i` from the queue, keeping the rest in order */
static void usbip_pop_at(UsbipLink *l, int i)
{
    for (int j = i; j + 1 < l->nurb; j++) {
        l->urbs[(l->head + j) % USBIP_MAX_URBS] =
            l->urbs[(l->head + j + 1) % USBIP_MAX_URBS];
    }
    l->nurb--;
}

/*
 * Queue the status stage of a control transfer we have just answered: the
 * zero-length packet in the direction opposite the data stage.
 *
 * The host stack folds the SETUP and the data stage of a control transfer
 * into a single URB and so never sends one -- but the gadget is a UDC, and a
 * UDC waits for it.  cLK's ep0in_complete() re-primes ep0-OUT with a length
 * of zero and spins on INFO_ACTIVE until it is retired, so without this the
 * USB thread never comes back to serve the bulk endpoints and every command
 * after enumeration goes unanswered.
 *
 * It goes in front of whatever is already queued: the host may have submitted
 * the next control transfer while we were busy with this one, and a SETUP
 * that overtakes its own status stage would be answered from the wrong queue
 * head.
 */
static void usbip_status_stage(UsbipLink *l, bool data_in)
{
    UsbipUrb *u;

    if (l->nurb >= USBIP_MAX_URBS - 1) {
        qemu_log_mask(LOG_UNIMP, "msm-usbip: no room for the status stage\n");
        return;
    }
    for (int i = l->nurb; i > 0; i--) {
        l->urbs[(l->head + i) % USBIP_MAX_URBS] =
            l->urbs[(l->head + i - 1) % USBIP_MAX_URBS];
    }
    u = &l->urbs[l->head];
    memset(u, 0, sizeof(*u));
    u->ep = 0;
    u->dir_in = !data_in;
    u->synth = true;                /* ours, so the host is never told */
    u->since = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    l->nurb++;
}

/*
 * Is the URB at offset `i` the one at the head of its own endpoint?
 *
 * Endpoints are independent queues, so a read adbd has not answered yet must
 * not hold up the mass storage endpoint behind it -- they are polled in
 * lockstep and either one can be waiting at any moment.  Order only matters
 * within a single endpoint, where the host sends one transfer at a time.
 */
static bool usbip_is_head(UsbipLink *l, int i)
{
    UsbipUrb *u = &l->urbs[(l->head + i) % USBIP_MAX_URBS];

    for (int j = 0; j < i; j++) {
        UsbipUrb *q = &l->urbs[(l->head + j) % USBIP_MAX_URBS];

        if (q->ep == u->ep && q->dir_in == u->dir_in) {
            return false;
        }
    }
    return true;
}

/*
 * Give the URB at offset `i` one round on the bus.  Returns true when it has
 * left the queue, false when it is still waiting for the gadget.
 */
static bool usbip_serve_one(UsbipLink *l, int i)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    UsbipUrb *u = &l->urbs[(l->head + i) % USBIP_MAX_URBS];
    bool control = u->control;
    bool dir_in = u->dir_in;
    uint32_t seq = u->seq;
    bool more = false;
    int n;

    /*
     * A control transfer with no data stage -- SET_CONFIGURATION and most
     * class requests -- is the SETUP packet and nothing else, and the
     * gadget has been given the packet already.
     */
    if (control && (dir_in ? !u->want : !u->len)) {
        n = 0;
    } else {
        /*
         * A control *read* comes as a single URB carrying both the SETUP
         * packet and the data stage: vudc_transfer.c handles ep0's setup
         * packet and then "treat_control_like_bulk" the same URB, so the
         * descriptor bytes the gadget queues have to come back here as
         * this URB's answer rather than as one of their own.
         */
        n = l->ops->poll(l->opaque, u->ep, dir_in,
                         u->data + u->done, u->len - u->done,
                         l->scratch + u->done,
                         dir_in ? u->want - u->done : 0, &more);
        if (n == USBIP_NOPE) {
            /*
             * NAK is not an error.  A bulk endpoint the gadget has not primed
             * yet is simply not ready, and a real controller keeps the URB
             * pending and retries until the gadget queues something -- telling
             * the host ETIMEDOUT instead makes it fail the transfer, which is
             * what drops adb to "offline" whenever adbd is a moment late.
             *
             * Only ep0 has a deadline: the host gives up on a control transfer
             * nobody answers, and a status stage the gadget never takes would
             * otherwise sit on the pipe forever.
             */
            if (u->ep != 0 || now - u->since <= TIMEOUT_NS) {
                return false;
            }
            if (u->synth) {
                qemu_log_mask(LOG_UNIMP,
                              "msm-usbip: the gadget never took the "
                              "status stage\n");
            } else {
                usbip_ret_submit(l, seq, u->ep, dir_in, NULL, 0,
                                 ETIMEDOUT_);
                l->n_drop++;
            }
            usbip_pop_at(l, i);
            return true;
        }
        if (n == USBIP_STALL) {
            /*
             * A gadget that does not implement a request stalls ep0
             * rather than queueing anything, so the bus has no data to
             * give and the only right answer is a stalled URB: the host
             * stack reads a silent ep0 as a dead device.
             */
            if (!u->synth) {
                usbip_ret_submit(l, seq, u->ep, dir_in, NULL, 0, EPIPE_);
                l->n_stall++;
            }
            usbip_pop_at(l, i);
            return true;
        }
        u->done += n;
        u->moved = now;
        if (!n && u->ep) {
            /*
             * The gadget was asked and gave nothing back.  On the way in,
             * hsusb_td_xfer() has already retired whatever the gadget had queued
             * for this endpoint -- with zero bytes -- so a request that was
             * primed and waiting has just been completed short, and cLK's
             * usb_read() reads that as the end of the transfer it asked for.  It
             * is the one way this path can end a download with "fastboot: oops!"
             * and no error anywhere else, so say which transfer it was.
             */
            qemu_log_mask(LOG_UNIMP,
                          "msm-usbip: seq %u ep %u %s moved nothing: "
                          "%u of %u, queued descriptors?\n",
                          seq, u->ep, dir_in ? "in" : "out", u->done, u->len);
        }
        if (more) {
            /*
             * The gadget is part-way through: it has queued some of what the
             * host asked for and will queue the rest when it can.  Completing
             * the URB now would send a short packet, and a bulk IN ends at
             * the first short packet -- so mass storage would see residue on
             * every read and reset the device.  Keep the URB and let the
             * next round add to what is already in the scratch buffer.
             */
            return false;
        }
        if (!dir_in && u->done < u->len) {
            /*
             * An OUT transfer is as long as the host said it is: fastboot
             * hands over 16 KB where the gadget queues 4096 at a time, and
             * a real controller keeps pulling packets until the URB is
             * full.  Answering with what it has so far is a short write,
             * which fastboot refuses outright ("SendBuffer() (Invalid
             * argument)"), so wait for the rest.
             *
             * An IN is the other way round: the gadget chooses how much it
             * sends, and a short packet is how it says "that is all".
             *
             * If it stops making progress for a while, the gadget has stopped
             * asking -- which is the one way this whole path can wedge, and it
             * shows up on the host as fastboot hung on "Sending" with no error
             * anywhere.  Say so here, where the numbers say how much of the
             * transfer is still sitting in the URB and which descriptor the
             * gadget has not primed.
             */
            if (now - u->since > STUCK_NS && now - u->moved > STUCK_NS) {
                qemu_log_mask(LOG_UNIMP,
                              "msm-usbip: seq %u ep %u stuck: %u of %u bytes "
                              "given, nothing for %d s\n",
                              u->seq, u->ep, u->done, u->len,
                              (int)((now - u->moved) / SCALE_MS));
                u->moved = now;
            }
            return false;
        }
    }

    if (!u->synth) {
        usbip_ret_submit(l, seq, u->ep, dir_in,
                         dir_in ? l->scratch : NULL, u->done, 0);
        l->n_done++;
    }
    usbip_pop_at(l, i);
    if (control) {
        usbip_status_stage(l, dir_in);
    }
    return true;
}

static void usbip_service(UsbipLink *l)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool popped = true;

    /*
     * Give every endpoint a turn.  A pass that retires a URB has moved
     * everything behind it, so start over; a pass that retires nothing means
     * the whole queue is waiting on the gadget and the timer will wake us.
     */
    while (popped && l->nurb) {
        popped = false;
        for (int i = 0; i < l->nurb; i++) {
            if (!usbip_is_head(l, i)) {
                continue;
            }
            if (usbip_serve_one(l, i)) {
                popped = true;
                break;
            }
        }
    }

    /*
     * The poll timer and the re-attach timer are the same timer, so leave it
     * alone while a re-attach is pending: usbip_link_reset() has just armed it
     * to call usbip_link_open() again, and deleting it here (the URB queue is
     * empty, so this looks like the ordinary idle case) means we never listen
     * for the host again.
     */
    if (l->retry) {
        return;
    }

    if (l->nurb) {
        timer_mod(l->timer, now + POLL_NS);
    } else {
        timer_del(l->timer);
    }
}

/*
 * Eat every whole frame out of the input buffer and queue what they ask for.
 * A frame that has only partly arrived is left alone: its own header says how
 * long it is, so the rest of it can still be picked up next time.
 */
static void usbip_decode(UsbipLink *l)
{
    while (l->inlen >= USBIP_HDR_SIZE) {
        const uint8_t *p = l->inbuf;
        uint32_t command = get32(p), seq = get32(p + 4), ep = get32(p + 16);
        uint32_t dir = get32(p + 12), total = USBIP_HDR_SIZE;
        UsbipUrb *u;

        if (command == CMD_SUBMIT) {
            uint32_t want = (int32_t)get32(p + 24);

            if (want > 0 && dir == DIR_OUT) {
                total += MIN(want, USBIP_XFER_MAX);
            }
            if (l->inlen < total) {
                return;         /* the rest of the frame has not arrived */
            }
            if (total > sizeof(l->inbuf)) {
                qemu_log_mask(LOG_UNIMP,
                              "msm-usbip: a %u byte transfer does not fit, "
                              "dropping the link\n", want);
                l->inlen = 0;
                return;
            }
            if (l->nurb == USBIP_MAX_URBS) {
                /*
                 * Should not happen -- the host submits a URB per transfer
                 * and waits -- but an unanswerable one would wedge the whole
                 * bus, so drop the oldest rather than stop reading.
                 */
                qemu_log_mask(LOG_UNIMP,
                              "msm-usbip: too many URBs, dropping seq %u\n",
                              l->urbs[l->head].seq);
                l->n_drop++;
                usbip_pop(l);
            }
            u = &l->urbs[(l->head + l->nurb) % USBIP_MAX_URBS];
            memset(u, 0, sizeof(*u));
            u->seq = seq;
            u->ep = ep;
            u->dir_in = dir == DIR_IN;
            u->want = dir == DIR_IN ? MIN(want, USBIP_XFER_MAX) : want;
            u->since = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            u->moved = u->since;
            memcpy(u->setup, p + 40, 8);
            /*
             * A control transfer is a whole URB carrying the SETUP packet and
             * its data stage, in whichever direction it reads or writes, and
             * it only ever runs on endpoint 0.
             *
             * The setup field must NOT be used to tell control from bulk:
             * alloc_urb_from_cmd() copies the eight bytes out of every packet
             * (vudc_rx.c) and usbip_pack_pdu() puts back whatever the recycled
             * URB still holds, so a bulk URB that got a URB previously used by
             * a control transfer still carries those bytes.  Android's MSC
             * CBWs and adb's bulk reads both arrive that way.
             *
             * Believing them costs a synthesised status stage per bogus
             * transfer, queued on the head where it blocks everything behind it
             * until it times out: the adb IN endpoint then never gets serviced
             * and adb flaps offline, and the MSC data phase never runs so the
             * host sees a zero-capacity disk.
             */
            u->control = ep == 0;
            if (dir == DIR_OUT && total > USBIP_HDR_SIZE) {
                u->len = MIN(want, USBIP_XFER_MAX);
                memcpy(u->data, p + USBIP_HDR_SIZE, u->len);
            }
            l->nurb++;
            l->n_submit++;

            qemu_log_mask(LOG_TRACE,
                          "msm-usbip: <- CMD_SUBMIT seq %u ep %u %s len %u "
                          "setup %02x%02x%02x%02x%02x%02x%02x%02x\n",
                          seq, ep, u->dir_in ? "in" : "out", want,
                          u->setup[0], u->setup[1], u->setup[2], u->setup[3],
                          u->setup[4], u->setup[5], u->setup[6], u->setup[7]);
            if (u->control) {
                l->seen_setup = true;
                l->ops->setup(l->opaque, u->setup);
            }
        } else if (command == CMD_UNLINK) {
            uint32_t target = get32(p + 20);
            int status = 0;

            for (int i = 0; i < l->nurb; i++) {
                UsbipUrb *q = &l->urbs[(l->head + i) % USBIP_MAX_URBS];

                if (q->seq != target) {
                    continue;
                }
                /*
                 * Drop the request and report the kill: a real unlink ends the
                 * transfer without completing it, and saying "success" would
                 * hand the host a short write it never saw.
                 */
                status = ECONNRESET_;
                for (int j = l->nurb - 1; j > i; j--) {
                    l->urbs[(l->head + j - 1) % USBIP_MAX_URBS] =
                        l->urbs[(l->head + j) % USBIP_MAX_URBS];
                }
                l->nurb--;
                break;
            }
            l->n_unlink++;
            qemu_log_mask(LOG_TRACE,
                          "msm-usbip: <- CMD_UNLINK seq %u target %u%s\n",
                          seq, target, status ? " (dropped)" : "");
            usbip_ret_unlink(l, seq, status);
        } else {
            /*
             * The stream is out of step: the header says how long the next
             * frame is, and those bytes are gone, so nothing after this can
             * be trusted.  Dropping the connection is the only honest answer.
             */
            qemu_log_mask(LOG_UNIMP,
                          "msm-usbip: <- unknown pdu %u, dropping the link\n",
                          command);
            l->inlen = 0;
            return;
        }

        l->inlen -= total;
        if (l->inlen) {
            memmove(l->inbuf, l->inbuf + total, l->inlen);
        }
    }
}

static void usbip_read(void *opaque)
{
    UsbipLink *l = opaque;

    for (;;) {
        ssize_t n;

        if (l->inlen == sizeof(l->inbuf)) {
            /*
             * Only reachable if a frame is larger than the whole buffer, and
             * USBIP_BUF is built to hold two of the largest ones.  Refusing
             * to read is better than reading over a partial frame.
             */
            qemu_log_mask(LOG_UNIMP,
                          "msm-usbip: a frame is bigger than the input buffer\n");
            break;
        }
        n = recv(l->peer, l->inbuf + l->inlen, sizeof(l->inbuf) - l->inlen,
                 MSG_DONTWAIT);
        if (n > 0) {
            l->inlen += n;
            /*
             * Take the frames out before reading again: they are what make
             * room, and what tells us where the next one starts.
             */
            usbip_decode(l);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            break;
        }
        if (n == 0) {
            qemu_log_mask(LOG_TRACE, "msm-usbip: the host closed the port\n");
            /*
             * With usb-ip-socket the peer *is* the host and it is not coming
             * back, so leave the dead socket and listen again -- otherwise the
             * next bridge finds nothing to connect to.  The gadget reset that
             * goes with it is what the host would see anyway: the device fell
             * off the bus and comes back to be enumerated afresh.
             */
            if (l->path) {
                usbip_link_reset(l);
            }
        } else {
            qemu_log_mask(LOG_TRACE, "msm-usbip: recv: %s\n",
                          strerror(errno));
        }
        l->inlen = 0;
        break;
    }

    usbip_service(l);
}

static void usbip_retry(void *opaque)
{
    UsbipLink *l = opaque;

    if (l->retry) {
        /* still waiting for the kernel to let go of the port */
        if (usbip_link_open(l, NULL) == 0) {
            l->retry = 0;
            l->ops->reset(l->opaque);
            return;
        }
        if (--l->retry == 0) {
            qemu_log_mask(LOG_UNIMP,
                          "msm-usbip: vhci_hcd kept the port busy\n");
            return;
        }
        timer_mod(l->timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 10 * SCALE_MS);
        return;
    }
    usbip_service(l);
}

void usbip_link_kick(UsbipLink *l)
{
    if (l->attached && l->nurb) {
        timer_mod(l->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
}

/* ---- attaching ------------------------------------------------------- */

/*
 * A sysfs attribute is not a file: it takes one write() and nothing else.
 * g_file_set_contents() would try to make a temporary file and rename it over
 * the target, which a sysfs attribute refuses, so write it by hand.
 */
static int usbip_sysfs(const char *path, const char *buf, size_t len,
                       Error **errp)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    ssize_t n;

    if (fd < 0) {
        error_setg(errp, "usb=lle: %s: %s", path, strerror(errno));
        return -1;
    }
    n = write(fd, buf, len);
    if (n != len) {
        error_setg(errp, "usb=lle: %s: %s", path,
                   n < 0 ? strerror(errno) : "short write");
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

/*
 * The bridge on the other end of the socket has arrived: the device appears on
 * the host's bus now, and the host enumerates it.
 */
static void usbip_accept(void *opaque)
{
    UsbipLink *l = opaque;
    int fd = accept(l->fd, NULL, NULL);

    if (fd < 0) {
        return;
    }
    qemu_set_fd_handler(l->fd, NULL, NULL, NULL);
    close(l->fd);
    l->fd = -1;
    l->peer = fd;
    l->attached = true;
    l->head = l->nurb = 0;
    l->inlen = 0;
    l->seen_setup = false;
    qemu_set_fd_handler(l->peer, usbip_read, NULL, l);
    qemu_log_mask(LOG_TRACE, "msm-usbip: the host connected\n");
}

/* claim a port and start speaking USB/IP on it */
static int usbip_link_open(UsbipLink *l, Error **errp)
{
    int sv[2];
    char buf[64];

    if (l->path) {
        /*
         * USB/IP on a unix socket (usb-ip-socket): the far end is the bridge,
         * which owns the host controller.  We listen and let it connect,
         * because it is started after us and there is nothing to attach to
         * until the gadget is ready anyway.
         */
        struct sockaddr_un un;
        int fd;

        if (l->fd >= 0 || l->attached) {
            return 0;            /* already listening, or the host is here */
        }
        if (strlen(l->path) >= sizeof(un.sun_path)) {
            error_setg(errp, "usb-ip-socket: the path is too long");
            return -1;
        }
        fd = qemu_socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            error_setg(errp, "usb-ip-socket: socket: %s", strerror(errno));
            return -1;
        }
        un.sun_family = AF_UNIX;
        strcpy(un.sun_path, l->path);
        unlink(l->path);            /* a stale socket from a killed run */
        if (bind(fd, (struct sockaddr *)&un, sizeof(un)) < 0 ||
            listen(fd, 1) < 0) {
            error_setg(errp, "usb-ip-socket: %s: %s", l->path,
                       strerror(errno));
            close(fd);
            unlink(l->path);
            return -1;
        }
        l->fd = fd;
        qemu_set_fd_handler(fd, usbip_accept, NULL, l);
        qemu_log_mask(LOG_TRACE,
                      "msm-usbip: listening on %s for the host\n", l->path);
        return 0;
    }

    if (geteuid() != 0) {
        error_setg(errp, "usb=lle needs root: vhci_hcd's attach attribute is "
                   "root-only and the socket fd has to be valid in the "
                   "process that writes it, so QEMU has to own the socket");
        return -1;
    }
    if (access(USBIP_ATTACH, W_OK) < 0) {
        error_setg(errp, "usb=lle: %s: %s (is vhci_hcd loaded? "
                   "modprobe vhci_hcd)", USBIP_ATTACH, strerror(errno));
        return -1;
    }
    if (qemu_socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        error_setg(errp, "usb=lle: socketpair: %s", strerror(errno));
        return -1;
    }
    l->fd = sv[0];
    l->peer = sv[1];

    /*
     * the port as vhci_hcd's `status` file numbers it, the fd the kernel has
     * to talk to us on, a device id (the kernel looks its URBs up by sequence
     * number anyway) and 3 = high speed, which is what the gadget advertises
     * in PORTSC.
     */
    snprintf(buf, sizeof(buf), "%u %d %u %u\n", l->port, l->fd, l->devid,
             l->speed);
    if (usbip_sysfs(USBIP_ATTACH, buf, strlen(buf), errp) < 0) {
        close(l->fd);
        close(l->peer);
        l->fd = l->peer = -1;
        return -1;
    }

    l->attached = true;
    qemu_set_fd_handler(l->peer, usbip_read, NULL, l);
    qemu_log_mask(LOG_TRACE,
                  "msm-usbip: attached to vhci_hcd port %u sockfd %u "
                  "speed %u\n", l->port, l->fd, l->speed);
    return 0;
}

/* give the port back */
static void usbip_link_close(UsbipLink *l)
{
    if (l->attached) {
        qemu_set_fd_handler(l->peer, NULL, NULL, NULL);
        /*
         * Closing our end is what really frees the port: vhci's rx thread is
         * parked in a blocking recv on it and only unwinds when the read
         * returns, so the attribute below and the close are both needed and
         * neither is enough on its own.
         */
        if (!l->path) {
            usbip_sysfs(USBIP_DETACH, "0", 1, NULL);
        }
        close(l->peer);
        close(l->fd);
        l->peer = l->fd = -1;
        l->attached = false;
    }
    if (l->fd >= 0) {
        /* still listening for the bridge (usb-ip-socket) */
        qemu_set_fd_handler(l->fd, NULL, NULL, NULL);
        close(l->fd);
        l->fd = -1;
    }
    if (l->path) {
        unlink(l->path);
    }
    timer_del(l->timer);
}

void usbip_link_set_path(UsbipLink *l, const char *path)
{
    g_free(l->path);
    l->path = path ? g_strdup(path) : NULL;
}

void usbip_link_init(UsbipLink *l, const UsbipOps *ops, void *opaque,
                     uint8_t port, uint8_t speed, Error **errp)
{
    l->ops = ops;
    l->opaque = opaque;
    l->port = port;
    l->speed = speed;
    l->devid = 1;
    l->fd = l->peer = -1;
    l->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, usbip_retry, l);

    /*
     * Do NOT attach yet.  The host starts enumerating the instant a device
     * appears, and a gadget that has not started cannot answer: the first
     * device descriptor read times out, the host parks the port as inactive,
     * and it never enumerates again ("vhci_device speed not set").  That is
     * what happens when cLK shares its ARM9 core with the AMSS and USB does
     * not come up until several seconds in.
     *
     * So wait for the gadget to tell us it is ready -- it resets its
     * controller once udc_start() has primed the endpoint list, which is
     * exactly the moment a real cable would see the device appear.
     */
}

/*
 * The gadget reset its controller, which on a real bus means the device fell
 * off and came back: the host unplugs it and enumerates it again.  Without
 * this the host keeps talking to the old instance, and a gadget that waits
 * for SET_CONFIGURATION before serving anything -- cLK's fastboot does --
 * never starts.
 *
 * Re-attaching is not immediate: the kernel frees the port from the thread
 * that owns the socket, so try again until it takes.
 */
void usbip_link_reset(UsbipLink *l)
{
    if (l->attached && !l->seen_setup && !l->path) {
        /*
         * Attached but the host has not asked us anything yet: it has already
         * started enumerating, so leave it alone.  Tearing the port down here
         * is what parks it as inactive for good.
         *
         * A socket has no such thing: nobody is enumerating anything, so a
         * closed peer is released however little was said on it.
         */
        return;
    }
    qemu_log_mask(LOG_TRACE, "msm-usbip: the gadget reset, re-attaching\n");
    usbip_link_close(l);
    l->head = l->nurb = 0;
    l->inlen = 0;
    l->seen_setup = false;
    l->retry = 100;
    timer_mod(l->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

void usbip_link_cleanup(UsbipLink *l)
{
    usbip_link_close(l);
    timer_free(l->timer);
    l->timer = NULL;
    g_free(l->path);
    l->path = NULL;
}