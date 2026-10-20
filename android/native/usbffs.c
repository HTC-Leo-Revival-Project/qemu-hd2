/*
 * The emulated HD2's USB, on this phone's own USB port.
 *
 * The app cannot use its own USB port for the emulated phone on its own: the
 * UDC driver and the configfs tree under it are root-only, and no app has that.
 * This runs as root, and while it runs the emulated HD2 is what a PC plugged
 * into this phone sees.  Stop it and the S25 comes back.
 *
 * How it fits together
 * -------------------
 * QEMU models the HD2's USB gadget (hw/arm/htcleo/msm_hsusb.c) and, with
 * usb-ip-socket, speaks USB/IP on a unix socket -- the same wire format it
 * speaks to vhci_hcd on a PC.  This is the host at the other end: URBs go in,
 * answers come out, which is what a host controller does with a device.
 *
 * The far end is a real USB *device*, though: the phone's own UDC, through a
 * FunctionFS gadget set up in configfs.  The phone's kernel has no vhci_hcd
 * (nothing to modprobe, CONFIG_USB_VHCI_HCD is unset), so there is no way to
 * attach a device to the phone's host stack; going the other way -- making the
 * phone a gadget and handing it the emulated phone's descriptors -- is the
 * whole trick.  The UDC is hardware, so the PC talks to the emulated phone at
 * full speed with nothing in the data path.
 *
 * Order matters, and it is what leaves the phone alone if any step fails.  ep0
 * is opened and the descriptors written *before* the UDC is taken: ffs only
 * binds while it is FFS_ACTIVE, and FFS_ACTIVE only holds while ep0 is open, so
 * a gadget bound without it comes back -ENODEV from usb_gadget_start() and the
 * PC never sees anything.  Everything before the bind can be undone with an
 * rmdir; after it, Android's own gadget is off the bus and the only way back is
 * to let its HAL bind it again.
 *
 * The descriptors
 * ---------------
 * FunctionFS wants the descriptors up front, because the kernel answers the
 * host's standard requests from them.  They come from the emulated gadget, which
 * only produces them when a host asks, over the queue heads its driver primes --
 * so this walks the gadget as a host would (GET_DESCRIPTOR(DEVICE), then
 * GET_DESCRIPTOR(CONFIGURATION), then the three strings) and writes what comes
 * back into configfs.
 *
 * Every later standard request is answered by the phone's own UDC from that
 * copy, which is what the host would have read off the gadget anyway.  The
 * requests that matter still go to the emulated gadget: SET_CONFIGURATION,
 * class requests, vendor requests.  That is what makes it think it is plugged
 * into something -- cLK's fastboot serves nothing until it has been configured.
 *
 * Bulk traffic is the rest of it, and is all that adb and fastboot send: a read
 * on an OUT endpoint is the PC's bytes, a write on an IN endpoint is the
 * emulated gadget's, and both are URBs here.
 *
 * The bulk endpoints each get a thread of their own, blocking on their own
 * file: a read on an OUT waits for the PC to send something and a write on an IN
 * waits for it to read, so neither can be done on the thread that also has to
 * answer ep0 -- adb is full duplex.  ep0 stays on that thread, because a control
 * transfer is short and the emulated gadget answers it at once.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* ---- the phone, as found ------------------------------------------------ */

#define CONFIGFS_GADGETS   "/config/usb_gadget"
#define UDC_CLASS          "/sys/class/udc"
#define FFS_PARENT         "/data/local/tmp"
#define USB_HAL            "vendor.usb-hal"

/* ---- USB/IP ------------------------------------------------------------- */

/*
 * USB/IP (drivers/usb/usbip/usbip-common.h), all fields big-endian:
 *
 *     CMD_SUBMIT   1  we want a transfer done
 *     CMD_UNLINK   2  ... and now we want it back
 *     RET_SUBMIT   3  here it is
 *     RET_UNLINK   4
 *
 * An OUT transfer's data rides along in the submit; an IN's comes back after the
 * 48-byte header in the RET_SUBMIT.  The same rules as vhci_hcd, including the
 * one that matters for control transfers: the host stack folds a SETUP packet
 * and its data stage into a single URB, and QEMU synthesises the status stage
 * itself (usbip_status_stage() in htcleo_usbip.c) because the gadget is a UDC
 * and waits for it.  So one setup packet from the phone is one submit here.
 */
#define HDR_SIZE       48
#define CMD_SUBMIT     1
#define CMD_UNLINK     2
#define RET_SUBMIT     3
#define RET_UNLINK     4

#define DIR_OUT        0
#define DIR_IN         1

#define USBIP_EPIPE    (-32)        /* -EPIPE: the gadget stalled the endpoint */
#define USBIP_TIMEOUT  (-110)       /* -ETIMEDOUT: nobody answered in time */

#define DEV_ID         1            /* the kernel looks URBs up by seqnum anyway */

#ifndef MIN
#define MIN(a, b)     (((a) < (b)) ? (a) : (b))
#endif

/* one read asks for at most this much; a bulk IN ends at the first short packet */
#define XFER_MAX       65536

/*
 * How much of what the host sent we hold while the emulator catches up.
 *
 * A megabyte: the emulator moves the guest 4 KB at a time and a poll round is
 * up to 100 ms of its own clock, so a host sending a flash image hands over far
 * more than the emulator can take in the time the host is willing to wait.  With
 * nowhere to put the difference it piles up inside the phone's UDC, and that
 * endpoint stops accepting data -- which shows up as fastboot hung on
 * "Sending" with no error anywhere.
 */
#define RING_BYTES     (1024 * 1024)

/* How much of an OUT transfer the emulated gadget asks for at a time: cLK's
 * fastboot reads a download in 4096-byte pieces (usb_read(), "xfer = 4096").
 * Handing the emulator anything else ends one of those pieces short, and a short
 * request is what fastboot calls an error.  See ring_whole_pages(). */
#define GUEST_PAGE     4096

/*
 * How long the phone has to be quiet before the ring gives up waiting for a
 * whole guest-sized piece and sends what it has.
 *
 * A fastboot command is 17-30 bytes, and the host is waiting for the reply the
 * moment it has sent one, so this cannot be long or every command costs it.
 * A host that pauses this long in the middle of a megabyte is not going to send
 * the rest anyway, and the log says when it happens.
 */
/*
 * How long the phone has to be quiet before what it gave us goes out as it is.
 *
 * Long enough that a command has arrived whole: fastboot writes
 * "download:004be000\n" as its own transfer, but the UDC hands over whatever
 * fits, which was eleven bytes of it here, and a quiet period shorter than this
 * splits the command into two transfers -- the guest then reads "download:4" as
 * the command and the rest as the next one.  It also has to stay well inside the
 * host's own patience: fastboot waits 60 s for a reply, so a few hundred
 * milliseconds per command is invisible and the whole boot script still runs at
 * a sensible speed.
 */
#define QUIET_MS       400

/*
 * How long the phone has to be quiet before a *bulk* remainder goes out
 * unaligned, as opposed to a command's.  See pump(): a partial page in the
 * middle of a download must not be sent because the phone paused, or the
 * guest's request completes short and fastboot gives up with "oops".
 */
#define QUIET_BULK_MS  4000

/* The guest's command buffer (fastboot's usb_read(buffer, 64)). */
#define COMMAND_MAX    64

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

/*
 * Host order, for the FunctionFS blobs: the driver reads their header with
 * get_unaligned_le32() and rejects anything that does not match, so the same
 * values written the USB/IP way (big endian) come back as a bare EINVAL.
 */
static void put32le(uint8_t *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

/* ---- the bridge --------------------------------------------------------- */

#define MAX_EPS        16
#define MAX_STRINGS    8
#define MAX_CFG        512

struct ep {
    int fd;                     /* the endpoint file in the functionfs mount */
    int num;                    /* the endpoint number, as the gadget numbered it */
    bool in;                    /* device -> host */
    char name[8];               /* "ep1", "ep2", in configuration order */
    uint8_t *buf;               /* IN: the transfer buffer, owned by its worker */

    /*
     * OUT: a ring of what has come off the phone but not gone to the emulator
     * yet.  It is there because the emulator is much slower than the phone's
     * UDC -- it hands the guest's 4 KB descriptors over one poll round at a
     * time -- and a bridge that stopped reading while a transfer was in flight
     * let that difference pile up in the UDC's own buffers until the endpoint
     * stopped accepting anything and the host's transfer hung for good.  The
     * ring keeps the phone drained while the emulator catches up.
     */
    uint8_t *ring;
    uint32_t ring_cap;
    uint32_t ring_head;         /* bytes the worker has read in */
    uint32_t ring_tail;         /* bytes handed to the emulator */
    bool flush;                 /* OUT: send what is left, page-aligned or not */
    uint32_t quiet;             /* OUT: how long the phone has given us nothing */
    uint32_t download_left;     /* OUT: the size the host announced, 0 if none */
    uint64_t download_seen;     /* OUT: bytes submitted since that command */
    bool download_done;         /* OUT: ... and whether that is all of them */
    char command[80];           /* OUT: the command being reassembled */
    uint32_t command_len;

    int slot;                   /* its urb slot; slot 0 is ep0 */
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cv;
    bool started;
    bool stop;
    bool done;                  /* its worker has returned */
    bool joined;                /* ...and so its fd and buffer are ours again */
    uint32_t len;               /* IN: how much the worker still has to write */
    bool writing;               /* the worker owns buf, or is inside write() */
};

struct bridge {
    int ip;                     /* the USB/IP socket to QEMU */
    uint32_t seq;
    uint64_t read_total;        /* bytes off the phone's OUT endpoint */
    uint64_t sent;              /* bytes handed to the emulator */
    uint64_t lost;              /* ... and how many could not be handed over */

    /* what the emulated gadget says it is */
    uint8_t dev[18];
    uint16_t dev_len;
    uint8_t cfg[MAX_CFG];       /* the configuration descriptor, whole */
    uint16_t cfg_len;
    unsigned fs_count;          /* descriptors per speed, as ffs wants them */
    unsigned hs_count;
    unsigned ss_count;
    uint8_t str[MAX_STRINGS][128];
    uint8_t str_len[MAX_STRINGS];
    uint8_t str_count;

    /* what we expose for it */
    char ffs_name[32];           /* functions/ffs.<name> and the mount source */
    char mount_dir[128];
    char gadget[32];
    char cfg_name[64];
    char cfg_dir[192];
    char func[192];
    char udc[64];

    struct ep *eps;                 /* one per endpoint, allocated once known */
    int nep;
    int ep0;
    uint8_t *ep0buf;           /* where a control read lands */

    bool bound;                 /* the UDC is ours */
    bool hal_stopped;           /* and Android's HAL is not going to fight us */
    bool running;
};

static struct bridge b;
static int verbose;
static volatile sig_atomic_t stop_now;

/* ---- plumbing ----------------------------------------------------------- */

static int64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void logline(const char *fmt, ...)
{
    va_list ap;

    /*
     * Milliseconds since start.  Without this the log says what happened and
     * not when, and "the transfer is slow" is a question about time: how long a
     * transfer takes to come back, how long the gaps between two reads are,
     * whether the emulator or the phone is the slow side.  Almost every line
     * here is a transfer going out or coming back, so the column reads as a
     * timeline of the whole run.
     */
    fprintf(stderr, "[%9lld] ", (long long)now_ms());
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

#define note(...)   do { if (verbose) { logline(__VA_ARGS__); } } while (0)
#define fail(...)   logline(__VA_ARGS__)

static void on_signal(int sig)
{
    stop_now = sig;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

    nanosleep(&ts, NULL);
}

static int write_all(int fd, const void *buf, size_t len, const char *what)
{
    const uint8_t *p = buf;

    while (len) {
        ssize_t n = write(fd, p, len);

        if (n > 0) {
            p += n;
            len -= n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        fail("%s: %s", what, strerror(errno));
        return -1;
    }
    return 0;
}

/*
 * A configfs attribute is one write() and nothing else: g_file_set_contents()
 * would make a temporary file and rename it over the target, which configfs
 * refuses, so write it by hand.
 */
static bool quiet;                 /* a write we expect to fail: say nothing */

static int put_attr(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    ssize_t n;
    size_t len = strlen(text);

    if (fd < 0) {
        if (!quiet) {
            fail("%s: %s", path, strerror(errno));
        }
        return -1;
    }
    n = write(fd, text, len);
    if (n != (ssize_t)len) {
        if (!quiet) {
            fail("%s: %s", path, n < 0 ? strerror(errno) : "short write");
        }
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int put_attr2(const char *gadget, const char *what, const char *text)
{
    char path[256];

    snprintf(path, sizeof(path), "%s/%s/%s", CONFIGFS_GADGETS, gadget, what);
    return put_attr(path, text);
}

/*
 * Put a gadget on the bus, or take it off.
 *
 * Coming off needs a newline, not an empty string: the kernel ignores a
 * zero-length write entirely (and write(2) reports EFAULT for it), so
 * `write(fd, "", 0)` leaves the gadget bound and the next bind gets EBUSY.
 * What `echo "" > .../UDC` does is what unbind_store() understands.
 */
static int set_udc(const char *gadget, const char *udc)
{
    return put_attr2(gadget, "UDC", udc[0] ? udc : "\n");
}

static int put_hex2(const char *gadget, const char *what, uint16_t v)
{
    char path[256], text[8];

    snprintf(path, sizeof(path), "%s/%s/%s", CONFIGFS_GADGETS, gadget, what);
    snprintf(text, sizeof(text), "0x%04x", v);
    return put_attr(path, text);
}

static int make_dir(const char *path)
{
    if (mkdir(path, 0755) == 0 || errno == EEXIST) {
        return 0;
    }
    fail("mkdir %s: %s", path, strerror(errno));
    return -1;
}

static int rm_dir(const char *path)
{
    if (rmdir(path) == 0 || errno == ENOENT) {
        return 0;
    }
    note("rmdir %s: %s", path, strerror(errno));
    return -1;
}

/* ---- the USB/IP end ----------------------------------------------------- */

/*
 * One URB in flight per endpoint, and the answer when it comes.  A bulk IN
 * keeps its request outstanding while the emulated gadget thinks about what to
 * send: that is what stops the endpoint timing out between packets, and it is
 * why "nothing yet" is not an error.  Nothing waits inline -- the answers come
 * back on one socket, interleaved, and ep0 has to keep flowing while a bulk
 * transfer is in flight -- so submitting is one write() and pump() moves
 * everything along.
 */
struct urb {
    bool used;                  /* submitted, waiting for its RET_SUBMIT */
    bool ready;                 /* the answer is here */
    uint32_t seq;
    bool in;
    uint8_t setup[8];
    uint32_t wlength;
    uint8_t *data;              /* where an IN's answer lands */
    uint32_t got;
    int status;
};

#define EP0_URB   (&pending[0])
static struct urb pending[MAX_EPS + 1];       /* slot 0 is ep0 */

static int submit_urb(struct urb *u, int ep, bool in, const uint8_t setup[8],
                      uint32_t wlength, uint8_t *out, uint32_t out_len,
                      uint8_t *into)
{
    uint8_t frame[HDR_SIZE];

    if (u->used || u->ready) {
        return -1;              /* one at a time, and only once it is collected */
    }
    memset(u, 0, sizeof(*u));
    u->seq = ++b.seq;
    u->used = true;
    u->in = in;
    u->data = into;
    u->wlength = in ? wlength : out_len;       /* what we asked for, either way */
    /*
     * Zero the whole header first.  The SETUP field is only meaningful on
     * ep0, and leaving the leftovers of whatever was on the stack in there is
     * not harmless: a peer that reads it as a SETUP packet on a bulk endpoint
     * waits for a status stage that this transfer does not have.
     */
    memset(frame, 0, sizeof(frame));

    put32(frame + 0, CMD_SUBMIT);
    put32(frame + 4, u->seq);
    put32(frame + 8, DEV_ID);
    put32(frame + 12, in ? DIR_IN : DIR_OUT);
    put32(frame + 16, ep);
    put32(frame + 20, 0);                       /* transfer_flags */
    put32(frame + 24, in ? wlength : (int32_t)out_len);
    if (ep == 0 && setup) {
        memcpy(u->setup, setup, 8);
        memcpy(frame + 40, setup, 8);           /* a control transfer is one URB */
    }

    if (out && out_len) {
        /*
         * The data of an OUT rides along in the same frame, and in one write():
         * the kernel's reader takes the payload from the frame, and a partial
         * header would leave its stream misaligned, which it drops as a broken
         * connection.
         */
        uint8_t *both = malloc(HDR_SIZE + out_len);

        if (!both) {
            u->used = false;
            return -1;
        }
        memcpy(both, frame, HDR_SIZE);
        memcpy(both + HDR_SIZE, out, out_len);
        if (write_all(b.ip, both, HDR_SIZE + out_len, "usb/ip submit") < 0) {
            free(both);
            u->used = false;
            return -1;
        }
        free(both);
    } else if (write_all(b.ip, frame, sizeof(frame), "usb/ip submit") < 0) {
        u->used = false;
        return -1;
    }
    note("emu: <- submit seq %u ep %d %s (%u)", u->seq, ep,
         in ? "in" : "out", in ? wlength : out_len);
    return 0;
}

/* take one RET_SUBMIT off the socket and file it under its endpoint */
/*
 * Replaying the configuration to the emulated guest.
 *
 * The phone's UDC answers the standard requests itself, out of the descriptor
 * copy we handed it, so the host's SET_CONFIGURATION never reaches the emulated
 * gadget.  It is not a harmless omission: in cLK, handle_setup()'s
 * SET_CONFIGURATION(1) is what enables the bulk endpoints and signals
 * UDC_EVENT_ONLINE, which is what wakes the fastboot thread.  Without it the
 * emulated phone enumerates and then sits there mute, and every bulk transfer
 * the host makes times out.
 *
 * The ffs ENABLE event is exactly the host selecting the configuration, so that
 * is where we send it on.  The answer goes nowhere: the UDC has already given
 * the host its status stage, and there is no SETUP waiting on ep0 for it.
 */
static struct urb replayed[2];        /* the two requests we send, in order */

static void replay_config(void)
{
    static const uint8_t cfg1[8] = {
        0x00,        /* host to device, standard, device */
        0x09,        /* SET_CONFIGURATION */
        0x01, 0x00,  /* wValue: configuration 1 */
        0x00, 0x00,  /* wIndex */
        0x00, 0x00,  /* wLength */
    };
    int i;

    for (i = 0; i < (int)(sizeof(replayed) / sizeof(replayed[0])); i++) {
        if (replayed[i].used) {
            return;                 /* still waiting for the last one */
        }
    }
    for (i = 0; i < (int)(sizeof(replayed) / sizeof(replayed[0])); i++) {
        if (submit_urb(&replayed[i], 0, false, cfg1, 0, NULL, 0, NULL) == 0) {
            note("emu: <- replay SET_CONFIGURATION as seq %u", replayed[i].seq);
        }
    }
}

static void on_answer(uint32_t seq, const uint8_t *reply)
{
    uint32_t actual = get32(reply + 24);
    int status = (int32_t)get32(reply + 20);

    for (int i = 0; i <= b.nep; i++) {
        struct urb *u = &pending[i];

        if (!u->used || u->seq != seq) {
            continue;
        }
        u->got = 0;
        if (u->in && actual && u->data) {
            uint32_t done = 0;

            while (done < actual) {
                ssize_t n = read(b.ip, u->data + done, actual - done);

                if (n <= 0) {
                    if (n < 0 && errno == EINTR) {
                        continue;
                    }
                    break;
                }
                done += n;
            }
            u->got = done;
        }
        u->used = false;
        u->ready = true;
        u->status = status;
        /*
         * An OUT's answer says how many of the bytes the emulated phone took.
         * Fewer than we sent is the worst thing that can happen here and nothing
         * else would say so: the host has already handed them over and will not
         * hand them over again, so the guest is left waiting for data that went
         * nowhere and the transfer hangs.
         */
        if (!u->in && actual != u->wlength) {
            fail("emu: took %u of the %u bytes on ep %d (seq %u), "
                 "the host sent those once",
                 actual, u->wlength, i ? b.eps[i - 1].num : 0, seq);
        }
        note("emu: -> seq %u ep %d %s %u bytes status %d", seq,
             i ? b.eps[i - 1].num : 0, u->in ? "in" : "out",
             u->in ? u->got : actual, status);
        return;
    }
    for (int i = 0; i < (int)(sizeof(replayed) / sizeof(replayed[0])); i++) {
        struct urb *u = &replayed[i];

        if (u->used && u->seq == seq) {
            u->used = false;
            note("emu: -> replayed seq %u status %d", seq, status);
            return;
        }
    }
    note("emu: an answer for seq %u nobody is waiting for", seq);
}

/* ---- asking the emulated gadget what it is ------------------------------ */

static int connect_usbip(const char *path)
{
    struct sockaddr_un un;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (fd < 0) {
        return -1;
    }
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof(un.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&un, sizeof(un)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/*
 * A control read, the way a host asks for a descriptor: a setup packet and the
 * bytes the gadget queues behind it.
 */
static int control_in(const uint8_t setup[8], uint8_t *out, uint32_t max,
                      uint32_t *got)
{
    struct urb u;
    uint8_t reply[HDR_SIZE];

    *got = 0;
    memset(&u, 0, sizeof(u));
    if (submit_urb(&u, 0, true, setup, (uint16_t)(setup[6] | setup[7] << 8),
                   NULL, 0, out) < 0) {
        return -1;
    }
    note("emu: <- %02x %02x value %04x length %u", setup[0], setup[1],
         setup[2] | setup[3] << 8, setup[6] | setup[7] << 8);

    for (;;) {
        ssize_t n = read(b.ip, reply, sizeof(reply));
        uint32_t cmd, seq;

        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            fail("usb/ip: the link closed (%s)", n ? strerror(errno) : "eof");
            u.used = false;
            return -1;
        }
        if ((size_t)n < HDR_SIZE) {
            fail("usb/ip: short frame");
            u.used = false;
            return -1;
        }
        cmd = get32(reply);
        seq = get32(reply + 4);
        if (cmd == RET_SUBMIT && seq == u.seq) {
            u.got = 0;
            if (get32(reply + 24) && max) {
                uint32_t want = get32(reply + 24);
                uint32_t done = 0;

                while (done < want && done < max) {
                    ssize_t r = read(b.ip, out + done, MIN(want - done, max - done));

                    if (r <= 0) {
                        if (r < 0 && errno == EINTR) {
                            continue;
                        }
                        break;
                    }
                    done += r;
                }
                u.got = done;
            }
            u.used = false;
            *got = u.got;
            note("emu: -> %u bytes, status %d", u.got, u.status);
            return (int32_t)get32(reply + 20);
        }
        if (cmd == CMD_UNLINK) {
            note("emu: unlinked seq %u", get32(reply + 20));
            u.used = false;
            return USBIP_EPIPE;
        }
        if (cmd == RET_UNLINK) {
            continue;
        }
        fail("usb/ip: unexpected frame, command %u", cmd);
        u.used = false;
        return -1;
    }
}

static int setup_request(uint8_t bmRequestType, uint8_t bRequest,
                         uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                         uint8_t *out, uint32_t max, uint32_t *got)
{
    uint8_t setup[8];

    setup[0] = bmRequestType;
    setup[1] = bRequest;
    setup[2] = wValue & 0xff;
    setup[3] = wValue >> 8;
    setup[4] = wIndex & 0xff;
    setup[5] = wIndex >> 8;
    setup[6] = wLength & 0xff;
    setup[7] = wLength >> 8;
    return control_in(setup, out, max, got);
}

/*
 * What the emulated phone is, as a host would find out.  Returns 0 when the
 * gadget has answered everything and -1 while it is not up yet, which is most
 * of the first half-minute of a boot: the gadget only primes its endpoints
 * once udc_start() has run.
 */
static int probe_gadget(void)
{
    uint8_t buf[MAX_CFG];
    uint32_t got;
    unsigned total;
    int status;

    b.dev_len = b.cfg_len = b.str_count = 0;

    status = setup_request(0x80, 0x06, 0x0100, 0, 64, buf, sizeof(buf), &got);
    if (status < 0 || got < 18) {
        return -1;
    }
    if (buf[0] != 18 || buf[1] != 1) {
        fail("emu: that is not a device descriptor (len %u type %u)",
             buf[0], buf[1]);
        return -1;
    }
    memcpy(b.dev, buf, 18);
    b.dev_len = 18;

    /* the 9-byte header first: it says how long the whole thing is */
    status = setup_request(0x80, 0x06, 0x0200, 0, 9, buf, sizeof(buf), &got);
    if (status < 0 || got < 9 || buf[1] != 2) {
        return -1;
    }
    total = le16(buf + 2);
    if (total < 9 || total > sizeof(b.cfg)) {
        return -1;
    }
    status = setup_request(0x80, 0x06, 0x0200, 0, (uint16_t)total,
                           buf, sizeof(buf), &got);
    if (status < 0 || got < 9) {
        return -1;
    }
    memcpy(b.cfg, buf, got);
    b.cfg_len = (uint16_t)got;

    /* iManufacturer, iProduct, iSerialNumber: what the PC shows for the device */
    for (unsigned i = 1; i <= 3; i++) {
        status = setup_request(0x80, 0x06, (uint16_t)(0x0300 | i), 0, 0xff,
                               buf, sizeof(buf), &got);
        if (status == 0 && got >= 2 && got < sizeof(b.str[0])) {
            memcpy(b.str[i], buf, got);
            b.str_len[i] = (uint8_t)got;
            if (i > b.str_count) {
                b.str_count = (uint8_t)i;
            }
        }
    }
    note("emu: %04x:%04x, %u bytes of configuration, %u strings",
         le16(b.dev + 8), le16(b.dev + 10), b.cfg_len, b.str_count);
    return 0;
}

static unsigned char utf16_to_utf8(const uint8_t *in, uint8_t len,
                                   uint8_t *out, size_t max)
{
    size_t n = 0;

    for (unsigned i = 0; i + 1 < len && n + 1 < max; i += 2) {
        unsigned c = in[i] | in[i + 1] << 8;

        if (c < 0x80) {
            out[n++] = (uint8_t)c;
        } else if (c < 0x800 && n + 2 < max) {
            out[n++] = (uint8_t)(0xc0 | c >> 6);
            out[n++] = (uint8_t)(0x80 | (c & 0x3f));
        } else if (n + 3 < max) {
            out[n++] = (uint8_t)(0xe0 | c >> 12);
            out[n++] = (uint8_t)(0x80 | ((c >> 6) & 0x3f));
            out[n++] = (uint8_t)(0x80 | (c & 0x3f));
        }
    }
    out[n] = 0;
    return (unsigned char)n;
}

/* ---- which gadget, which UDC -------------------------------------------- */

static int find_gadget(char *out, size_t max)
{
    char path[160];

    snprintf(path, sizeof(path), "%s/%s/UDC", CONFIGFS_GADGETS, out);
    return access(path, F_OK) == 0 ? 0 : -1;
}

/*
 * Any gadget this kernel has, as a last resort.  We would rather have one of
 * our own -- see main() -- but a kernel that will not give us a second gadget is
 * better served by borrowing the phone's than by not working, and the USB HAL
 * puts its own back on the way out.
 */
static int any_gadget(char *out, size_t max)
{
    char path[160];
    DIR *d = opendir(CONFIGFS_GADGETS);
    struct dirent *e;
    int found = -1;

    if (!d) {
        fail("%s: %s", CONFIGFS_GADGETS, strerror(errno));
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s/UDC", CONFIGFS_GADGETS, e->d_name);
        if (access(path, F_OK) == 0) {
            snprintf(out, max, "%s", e->d_name);
            found = 0;
            break;
        }
    }
    closedir(d);
    return found;
}

/* the contents of a small configfs/sysfs file, NUL terminated; -1 if unreadable */
static int read_file(const char *path, char *out, size_t max)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n;

    if (fd < 0) {
        return -1;
    }
    n = read(fd, out, max - 1);
    close(fd);
    if (n < 0) {
        return -1;
    }
    out[n] = 0;
    return (int)n;
}

static int find_udc(char *out, size_t max)
{
    DIR *d = opendir(UDC_CLASS);
    struct dirent *e;
    int found = -1;

    if (!d) {
        fail("%s: %s", UDC_CLASS, strerror(errno));
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        snprintf(out, max, "%s", e->d_name);
        found = 0;
        break;
    }
    closedir(d);
    if (found < 0) {
        fail("no UDC in %s: is a cable plugged in?", UDC_CLASS);
    }
    return found;
}

/* ---- the descriptors for the functionfs instance ------------------------ */

/*
 * Hand FunctionFS the descriptors it will answer the host's standard requests
 * from (drivers/usb/gadget/function/f_fs.c).  Four things about that format are
 * easy to get wrong, and all four come back as the same bare EINVAL:
 *
 *  - the per-speed field is a count of DESCRIPTORS, not a length in bytes;
 *  - all three speeds have to be there, each with its own blob.  A blob with
 *    only the high-speed one is rejected, even though the emulated controller
 *    is high-speed only;
 *  - the blobs hold interface and endpoint descriptors only.  The device and
 *    configuration descriptors are not in them and the driver rejects a blob
 *    that carries them: those two come from the configfs attributes;
 *  - each write() has to be at least 16 bytes, so a pipeline that dribbles the
 *    blob out in pieces fails the same way.
 *
 *      u32 magic   3, FUNCTIONFS_DESCRIPTORS_MAGIC_V2
 *      u32 length  the whole of this blob
 *      u32 flags   FS | HS | SS (1|2|4)
 *      u32 fs_count, hs_count, ss_count
 *      descriptors[] x3
 *
 * and then the strings the same way: magic 2, a language, and each string as
 * NUL-terminated UTF-8.  Only the strings the *interfaces* name; the device's
 * own three go into configfs, which is where the UDC reads them from.
 */
static int write_ep0(int fd, const uint8_t *buf, size_t len, const char *what)
{
    ssize_t n = write(fd, buf, len);

    if (n < 0) {
        fail("functionfs: %s: %s", what, strerror(errno));
        if (verbose) {
            char line[2 * 128 + 1];
            size_t n = len < 128 ? len : 128;

            for (size_t i = 0; i < n; i++) {
                snprintf(line + i * 2, 3, "%02x", buf[i]);
            }
            line[n * 2] = 0;
            fail("functionfs: %zu bytes, starting %s", len, line);
        }
        return -1;
    }
    if (n != (ssize_t)len) {
        fail("functionfs: %s: short write (%zd of %zu)", what, n, len);
        return -1;
    }
    note("functionfs: %s, %zu bytes", what, len);
    return 0;
}

/* walk the configuration: how many descriptors, and which are endpoints */
static int alloc_eps(void)
{
    unsigned count = 0;

    b.eps = calloc(MAX_EPS, sizeof(*b.eps));
    if (!b.eps) {
        fail("out of memory");
        return -1;
    }
    for (unsigned at = 9; at < b.cfg_len;) {
        unsigned l = b.cfg[at];

        if (!l || at + l > b.cfg_len) {
            fail("emu: the configuration descriptor is malformed at %u", at);
            return -1;
        }
        if (b.cfg[at + 1] == 5 && l >= 7 && b.nep < MAX_EPS) {  /* an endpoint */
            struct ep *e = &b.eps[b.nep];

            e->num = b.cfg[at + 2] & 0x0f;
            e->in = (b.cfg[at + 2] & 0x80) != 0;
            e->fd = -1;
            e->slot = b.nep + 1;              /* slot 0 is ep0 */
            snprintf(e->name, sizeof e->name, "ep%d", b.nep + 1);
            b.nep++;
        }
        count++;
        at += l;
    }
    b.fs_count = b.hs_count = b.ss_count = count;
    if (verbose) {
        char line[2 * 64 + 1];
        size_t n = b.cfg_len < 64 ? b.cfg_len : 64;

        for (size_t i = 0; i < n; i++) {
            snprintf(line + i * 2, 3, "%02x", b.cfg[i]);
        }
        line[n * 2] = 0;
        note("emu: %u bytes of configuration: %s", b.cfg_len, line);
    }
    note("emu: %u descriptors, %d endpoints", count, b.nep);
    return 0;
}

static int ffs_descriptors(uint8_t *out, size_t max, size_t *len)
{
    size_t body = b.cfg_len - 9;
    size_t n = 24 + 3 * body;      /* the header is 24 bytes, then three copies */

    if (body == 0 || n > max) {
        fail("the configuration descriptor does not fit");
        return -1;
    }
    put32le(out + 0, 3);
    put32le(out + 4, (uint32_t)n);
    put32le(out + 8, 1 | 2 | 4);               /* HAS_FS_DESC | HS | SS */
    put32le(out + 12, b.fs_count);
    put32le(out + 16, b.hs_count);
    put32le(out + 20, b.ss_count);
    /*
     * The same descriptors for all three speeds, as the gadget answered with.
     * The UDC is pinned to high speed (see max_speed above), so the middle copy
     * is the one the host reads; the other two are here because the driver
     * wants all three.  The super-speed copy has no endpoint companions -- the
     * driver rejects them -- so it would not survive a super-speed host, which
     * is exactly why the gadget is not allowed to negotiate one.
     */
    for (int copy = 0; copy < 3; copy++) {
        memcpy(out + 24 + copy * body, b.cfg + 9, body);
    }
    *len = n;
    return 0;
}

static int ffs_strings(uint8_t *out, size_t max, size_t *len)
{
    unsigned wanted = 1;             /* the driver wants at least one */
    size_t n = 18;

    for (unsigned at = 9; at < b.cfg_len;) {
        unsigned l = b.cfg[at];
        uint8_t i = b.cfg[at + 1] == 4 && l >= 9 ? b.cfg[at + 8] : 0;

        if (!l || at + l > b.cfg_len) {
            return -1;
        }
        if (i > wanted) {
            wanted = i;
        }
        at += l;
    }
    for (unsigned i = 1; i <= wanted; i++) {
        uint8_t text[128];
        size_t l;

        if (b.str_len[i] >= 2) {
            l = utf16_to_utf8(b.str[i] + 2, b.str_len[i] - 2, text, sizeof(text));
        } else {
            text[0] = 'I'; text[1] = 'i'; text[2] = 0;
            l = 2;
        }
        if (!l || n + l + 1 > max) {
            return -1;
        }
        memcpy(out + n, text, l + 1);
        n += l + 1;
    }
    put32le(out + 0, 2);                        /* FUNCTIONFS_STRINGS_MAGIC */
    put32le(out + 4, (uint32_t)n);
    put32le(out + 8, wanted);
    put32le(out + 12, 1);                       /* one language */
    out[16] = 0x04;
    out[17] = 0x09;                             /* 0x0409 */
    *len = n;
    return 0;
}

/* ---- setting the gadget up ---------------------------------------------- */

static int setup_configfs(void)
{
    static const char *dev_str[] = { "", "manufacturer", "product", "serialnumber" };
    char path[320], text[128];

    /* the device, as the emulated gadget describes it */
    put_hex2(b.gadget, "idVendor", le16(b.dev + 8));
    put_hex2(b.gadget, "idProduct", le16(b.dev + 10));
    put_hex2(b.gadget, "bcdUSB", le16(b.dev + 12));
    put_hex2(b.gadget, "bcdDevice", le16(b.dev + 14));
    put_hex2(b.gadget, "bDeviceClass", b.dev[4]);
    put_hex2(b.gadget, "bDeviceSubClass", b.dev[5]);
    put_hex2(b.gadget, "bDeviceProtocol", b.dev[6]);
    /*
     * max_speed is set in take_udc(), once the USB HAL is stopped: it puts
     * super-speed-plus back on every gadget it finds while it is still running.
     */

    /* and what the PC reads off it: the strings the gadget gave us */
    snprintf(path, sizeof(path), "%s/%s/strings/0x409", CONFIGFS_GADGETS, b.gadget);
    make_dir(path);
    for (unsigned i = 1; i <= 3; i++) {
        if (b.str_len[i] < 2) {
            continue;
        }
        utf16_to_utf8(b.str[i] + 2, b.str_len[i] - 2, (uint8_t *)text, sizeof(text));
        if (!text[0]) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s/strings/0x409/%s",
                 CONFIGFS_GADGETS, b.gadget, dev_str[i]);
        put_attr(path, text);
        note("configfs: %s = %s", dev_str[i], text);
    }

    /* the configuration.  It is not symlinked into the gadget: configfs refuses
     * that here (EPERM), and the association is implicit anyway */
    snprintf(path, sizeof(path), "%s/%s/configs", CONFIGFS_GADGETS, b.gadget);
    DIR *d = opendir(path);
    struct dirent *e;

    b.cfg_name[0] = 0;
    if (d) {
        while ((e = readdir(d)) != NULL) {
            char what[320];

            if (e->d_name[0] == '.') {
                continue;
            }
            snprintf(what, sizeof(what), "%s/%s", path, e->d_name);
            if (access(what, F_OK) == 0) {
                snprintf(b.cfg_name, sizeof(b.cfg_name), "%s", e->d_name);
                break;
            }
        }
        closedir(d);
    }
    if (!b.cfg_name[0]) {
        snprintf(b.cfg_name, sizeof(b.cfg_name), "c.1");
        snprintf(path, sizeof(path), "%s/%s/configs/%s", CONFIGFS_GADGETS,
                 b.gadget, b.cfg_name);
        if (make_dir(path) < 0) {
            return -1;
        }
    }
    snprintf(b.cfg_dir, sizeof(b.cfg_dir), "%s/%s/configs/%s",
             CONFIGFS_GADGETS, b.gadget, b.cfg_name);
    note("using configuration %s", b.cfg_dir);

    /*
     * The FunctionFS function.  The name has to be unique per run: this kernel
     * does not release a functionfs instance cleanly when the last ep0 goes
     * away, so a name that was used before is refused with EBUSY for good.
     */
    snprintf(b.func, sizeof(b.func), "%s/%s/functions/ffs.%s",
             CONFIGFS_GADGETS, b.gadget, b.ffs_name);
    if (make_dir(b.func) < 0) {
        return -1;
    }
    snprintf(b.mount_dir, sizeof(b.mount_dir), "%s/%s", FFS_PARENT, b.ffs_name);
    make_dir(b.mount_dir);
    /*
     * The instance name is the mount *source*, not the data string: functionfs
     * takes its mount options (mode, uid, gid, no_disconnect) from the data, and
     * anything it does not recognise there comes back as EINVAL.
     */
    if (mount(b.ffs_name, b.mount_dir, "functionfs", 0, "") < 0) {
        fail("mount -t functionfs %s: %s", b.ffs_name, strerror(errno));
        return -1;
    }
    note("mounted functionfs %s on %s", b.ffs_name, b.mount_dir);
    return 0;
}

/*
 * ep0, the descriptors, and then the endpoint files the driver creates once it
 * has them: numbered by position (ep1, ep2), one per endpoint in the order the
 * configuration lists them.
 */
static int setup_ffs(void)
{
    uint8_t buf[4096];
    char path[320];
    size_t len;

    snprintf(path, sizeof(path), "%s/ep0", b.mount_dir);
    b.ep0 = open(path, O_RDWR | O_CLOEXEC);
    if (b.ep0 < 0) {
        fail("%s: %s", path, strerror(errno));
        return -1;
    }
    if (ffs_descriptors(buf, sizeof(buf), &len) < 0 ||
        write_ep0(b.ep0, buf, len, "descriptors") < 0) {
        fail("the kernel would not take the emulated phone's descriptors");
        return -1;
    }
    if (ffs_strings(buf, sizeof(buf), &len) < 0 ||
        write_ep0(b.ep0, buf, len, "strings") < 0) {
        return -1;
    }

    for (int i = 0; i < b.nep; i++) {
        struct ep *e = &b.eps[i];

        snprintf(path, sizeof(path), "%s/ep%d", b.mount_dir, i + 1);
        e->fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (e->fd < 0) {
            fail("%s: %s", path, strerror(errno));
            continue;
        }
        note("functionfs: %s -> endpoint %d %s", path, e->num,
             e->in ? "in" : "out");
    }
    return 0;
}

/*
 * Take the port.  This is the moment the S25 stops being what the PC sees, so it
 * is last, once everything that can fail has worked -- and ffs is FFS_ACTIVE by
 * now, which is what makes it bind.
 */
static int take_udc(void)
{
    char path[320], link[320], target[320];
    DIR *d;
    struct dirent *e;

    /*
     * Android's USB HAL rebinds its own gadget to the UDC within milliseconds,
     * so it has to be out of the way first.  (Only the service: setting
     * sys.usb.config restarts adbd and takes wireless adb with it.)
     */
    if (system("stop " USB_HAL) == 0) {
        b.hal_stopped = true;
        note("stopped " USB_HAL);
        sleep_ms(300);
    }

    /*
     * Off the bus before anything else: max_speed can only be written while the
     * gadget is unbound, and a gadget left bound by a run that was killed (or
     * that failed later on) is exactly what is in the way.  The driver answers
     * EINVAL rather than saying so, which reads like a phone that cannot do it.
     */
    set_udc(b.gadget, "");

    /*
     * Now, with the HAL out of the way, pin the speed.  The emulated HD2 hangs
     * off an EHCI controller -- high speed, 512-byte bulk packets, no super
     * speed at all -- and it has to be said here because:
     *   - the driver only accepts "high-speed" here, not "high";
     *   - it can only be written while the gadget is unbound;
     *   - the HAL puts super-speed-plus back on every gadget while it runs;
     *   - and at super speed the host insists on SS endpoint companion
     *     descriptors that FunctionFS will not take, so SET_CONFIGURATION fails
     *     with EPIPE and the interface never comes up at all.
     */
    if (put_attr2(b.gadget, "max_speed", "high-speed") < 0) {
        fail("the gadget would not stay high-speed");
        return -1;
    }

    /*
     * Stopping the HAL is not enough: its gadget is still bound to the UDC, and
     * a bound UDC says EBUSY to anyone else.  Take it off every *other* gadget
     * as well -- and only from the UDC file, never from sys.usb.config, which
     * restarts adbd and takes wireless adb with it.
     */
    {
        char gadgets[320];

        snprintf(gadgets, sizeof gadgets, "%s/%s", CONFIGFS_GADGETS, "");
        d = opendir(gadgets);
        if (d) {
            while ((e = readdir(d)) != NULL) {
                char other[320], have[64];

                if (e->d_name[0] == '.' ||
                    !strcmp(e->d_name, b.gadget)) {
                    continue;
                }
                snprintf(other, sizeof other, "%s/%s/UDC", gadgets, e->d_name);
                if (read_file(other, have, sizeof have) <= 0 || !have[0]) {
                    continue;
                }
                note("configfs: taking the UDC off %s", e->d_name);
                put_attr(other, "\n");
            }
            closedir(d);
        }
    }

    /* off the bus before touching the configuration under it */
    set_udc(b.gadget, "");

    /* the phone's own functions out, ours in */
    d = opendir(b.cfg_dir);
    if (d) {
        while ((e = readdir(d)) != NULL) {
            ssize_t n;

            if (e->d_name[0] == '.' || !strcmp(e->d_name, "strings")) {
                continue;
            }
            snprintf(link, sizeof(link), "%s/%s", b.cfg_dir, e->d_name);
            n = readlink(link, target, sizeof(target) - 1);
            if (n <= 0) {
                continue;                    /* MaxPower, bmAttributes */
            }
            target[n] = 0;
            note("configfs: dropping %s", e->d_name);
            unlink(link);
        }
        closedir(d);
    }
    snprintf(path, sizeof(path), "%s/%s", b.cfg_dir, b.ffs_name);
    unlink(path);
    if (symlink(b.func, path) < 0) {
        fail("link %s: %s", path, strerror(errno));
        return -1;
    }

    if (set_udc(b.gadget, b.udc) < 0) {
        fail("the UDC would not take the gadget; the emulated phone needs ffs to "
             "be FFS_ACTIVE, which means ep0 open in this process");
        return -1;
    }
    b.bound = true;
    note("%s is on %s: the PC will see the emulated HD2", b.gadget, b.udc);
    return 0;
}

/* ---- the endpoints ------------------------------------------------------ */

static int wake_pipe[2] = { -1, -1 };

static void wake_the_loop(void)
{
    char c = 'x';
    ssize_t n = write(wake_pipe[1], &c, 1);

    (void)n;
}

/*
 * Wait for the endpoint file, but wake up now and then: the fds are
 * non-blocking (the loop touches them too), and a worker parked in read(2) for
 * good would not notice a stop request.
 *
 * 0 = ready, 1 = asked to stop, -1 = the descriptor is broken.
 */
static int wait_for_io(int fd, short events)
{
    for (;;) {
        struct pollfd p = { .fd = fd, .events = events };
        int n = poll(&p, 1, 100);

        if (n > 0 && (p.revents & events)) {
            return 0;
        }
        if (n < 0 && errno != EINTR) {
            return -1;
        }
        if (stop_now) {
            return 1;
        }
    }
}

/*
 * The same, but it says how long it waited.
 *
 * Worth having on the OUT side: when a transfer stops in the middle, the one
 * thing the log has to answer is whether the phone had the bytes for us and we
 * did not take them, or the phone had nothing because the host stopped sending.
 * Those are different faults with different fixes, and an infinite poll answers
 * neither -- the log just stops, which looks the same as either.
 */
static int wait_for_io_timed(int fd, short events, int ms, int *timed_out)
{
    int64_t until = now_ms() + ms;

    *timed_out = 0;
    for (;;) {
        struct pollfd p = { .fd = fd, .events = events };
        int64_t left = until - now_ms();
        int n;

        if (left <= 0) {
            *timed_out = 1;
            return 0;
        }
        n = poll(&p, 1, left > 100 ? 100 : (int)left);
        if (n > 0 && (p.revents & events)) {
            return 0;
        }
        if (n < 0 && errno != EINTR) {
            return -1;
        }
        if (stop_now) {
            return 1;
        }
    }
}

/*
 * OUT: the bytes arrive from the host at their own pace, so this reads as soon
 * as the endpoint has any and hands them to the loop, which passes them on to
 * the emulated gadget.  It must not wait for permission first: nothing has set
 * `have` yet on the first transfer, and waiting for it means the endpoint never
 * moves at all.
 *
 * The buffer is the worker's until the loop has copied it out, which is what
 * `writing` says here.
 */
/* what the OUT ring can still take, and what is waiting to go to the emulator */
static uint32_t ring_room(const struct ep *e)
{
    return e->ring_cap - (e->ring_head - e->ring_tail);
}

static uint32_t ring_held(const struct ep *e)
{
    return e->ring_head - e->ring_tail;
}

/* how much the worker could still read in without waiting for the emulator */
static uint32_t room_left(const struct ep *e)
{
    return e->ring_cap - ring_held(e);
}

static uint32_t ring_whole_pages(uint32_t held)
{
    return held - (held % GUEST_PAGE);
}

/*
 * How many bytes of the transfer this endpoint may send now.
 *
 * A download is the only transfer the host announces a size for, and that size
 * is what makes the end of it safe: cCLK's fastboot asks for 4096-byte pieces
 * while it downloads, and whatever the announced size has left over -- 4096 for
 * an image that is a whole number of pages, less for one that is not -- is
 * exactly the piece its last request wants.  Send that much and the guest's last
 * request is filled *completely*: no short completion, no "fastboot: oops!", and
 * no waiting on the phone going quiet, which this ffs does for seconds at a time
 * for reasons of its own.
 *
 * Until then: whole pages only, because the guest cannot be given a part of one.
 */
static uint32_t out_want(const struct ep *e, uint32_t held)
{
    uint32_t want;

    if (e->download_left && !e->download_done) {
        want = ring_whole_pages(held);
    } else {
        /*
         * No download running, or all of it has been sent: what is left goes as
         * it is.  For the tail that is the last page of the image and the guest's
         * last request wants exactly that much; for a command it is the whole
         * command.
         */
        want = held;
    }
    return want > held ? held : want;
}

/*
 * The `download:00102800` command the host sends before the bytes, read off the
 * wire to learn the size.  A command the bridge forwards is otherwise only ever
 * looked at by the guest.
 */
static void note_download_left(struct ep *e)
{
    static const char tag[] = "download:";
    const char *cmd = e->command;
    uint32_t len = e->command_len;
    const char *p;
    uint32_t v = 0, digits = 0;

    if (e->in) {
        e->command_len = 0;
        return;
    }
    /*
     * fastboot sends a command as its own short transfer and cLK's fastboot
     * takes it as it is -- there is no newline to wait for -- and the UDC may
     * hand the text over in pieces ("getvar:" and "version" separately).  So
     * this is called with whatever has arrived: a prefix that is not a download
     * clears the buffer, a prefix that is one keeps the digits it has until
     * there are enough to be a size.
     */
    if (len <= sizeof(tag) || len > 40 || memcmp(cmd, tag, sizeof(tag) - 1)) {
        e->command_len = 0;
        return;
    }
    for (p = cmd + sizeof(tag) - 1; p < cmd + len; p++) {
        uint32_t d;

        if (*p >= '0' && *p <= '9') {
            d = (uint32_t)(*p - '0');
        } else if ((*p | 32) >= 'a' && (*p | 32) <= 'f') {
            d = (uint32_t)((*p | 32) - 'a' + 10);
        } else {
            break;
        }
        v = v * 16 + d;
        digits++;
    }
    if (digits < 4 || !v) {
        note("ffs: %s: download command so far: \"%.*s\"", e->name, (int)len, cmd);
        return;                     /* more to come */
    }
    e->download_left = v;
    e->download_seen = 0;         /* the command is not part of the image */
    e->download_done = false;
    e->command_len = 0;
    note("ffs: %s: a download of %u bytes", e->name, v);
}

/*
 * Copy `len` bytes out of the ring at `at`, wrapping at the end of it.
 *
 * Without this the ring's end clips a transfer: a megabyte into a download the
 * tail sits near the wrap, the contiguous span left is 3,881 bytes, and a
 * transfer of 3,881 ends the guest's 4096-byte request short -- which is
 * "fastboot: oops!" in the middle of a flash.  The bytes are copied out into a
 * staging buffer anyway (submit_urb() wants one contiguous frame), so the copy
 * is where the wrap belongs.
 */
static void ring_copy_out(const struct ep *e, uint8_t *dst, uint32_t at, uint32_t len)
{
    uint32_t off = at % e->ring_cap;
    uint32_t first = e->ring_cap - off;

    if (first > len) {
        first = len;
    }
    memcpy(dst, e->ring + off, first);
    if (len > first) {
        memcpy(dst + first, e->ring, len - first);
    }
}

/*
 * How much of `want` can be taken from offset `at` in one go: the ring is not
 * linear memory, and a read that runs past the end of the buffer is EFAULT.
 */
static uint32_t ring_slice(const struct ep *e, uint32_t at, uint32_t want)
{
    uint32_t left = e->ring_cap - (at % e->ring_cap);

    return want > left ? left : want;
}

/*
 * Round a count down to the emulated gadget's request size.
 *
 * This is the whole reason the ring exists.  The phone's UDC hands us what it
 * has, not the host's transfers: one download arrives as 65,363 bytes and then
 * 65,536, because 65,363 is what the UDC had room for.  If each of those goes to
 * the emulator as its own transfer, the end of it looks like a USB short packet,
 * and a short packet completes the gadget's outstanding request short.  *That*
 * is what stops fastboot mid-download: cLK's usb_read() takes a short request as
 * the end of what it asked for, cmd_download() gets fewer bytes than it promised,
 * and fastboot prints "fastboot: oops!" and stops.  The guest asks for 4096 at a
 * time while it downloads, so handing it whole multiples of 4096 means it never
 * sees a request come up short.
 *
 * A fastboot command is 17-30 bytes and there is nothing to round it up to, so
 * what is left over goes out on `flush` -- see pump().
 */



static void *out_worker(void *arg)
{
    struct ep *e = arg;
    int quiet_rounds = 0;

    for (;;) {
        ssize_t n;
        int timed_out = 0;
        uint32_t room;

        /*
         * The only thing this waits for now is the phone.  It used to also wait
         * for the emulator to take the previous transfer, which is the whole
         * difference: a megabyte of flash image arrives in a few milliseconds
         * and the emulator takes it at its own pace, so the phone's UDC was
         * being asked to hold the difference, and holding it is what breaks.
         */
        pthread_mutex_lock(&e->lock);
        while (!e->stop && ring_room(e) == 0) {
            pthread_cond_wait(&e->cv, &e->lock);
        }
        if (e->stop) {
            pthread_mutex_unlock(&e->lock);
            break;
        }
        room = ring_room(e);
        /*
         * One read at a transfer's worth, whatever the ring has room for.  This
         * ffs blocks inside read(2) until it has data, so a count of a whole
         * megabyte is a request to sit there for a megabyte -- and asking for
         * more than the endpoint's packets add up to is what it answers EFAULT.
         */
        if (room > GUEST_PAGE) {
            room = GUEST_PAGE;
        }
        room = ring_slice(e, e->ring_head, room);
        pthread_mutex_unlock(&e->lock);

        if (wait_for_io_timed(e->fd, POLLIN, QUIET_MS, &timed_out) != 0) {
            break;
        }
        if (timed_out) {
            /*
             * The phone had nothing for us for a moment.  Nothing to decide here:
             * whether the ring holds a piece of the host's transfer that has to
             * go out unaligned is the loop's call, not this thread's, because a
             * read(2) on this ffs can sit in the driver for good and the loop has
             * to keep working either way.
             */
            quiet_rounds++;
            if (quiet_rounds == 40 || quiet_rounds == 240) {
                note("ffs: nothing from the host on %s for %d s", e->name,
                     quiet_rounds * QUIET_MS / 1000);
            }
            continue;
        }
        quiet_rounds = 0;
        n = read(e->fd, e->ring + (e->ring_head % e->ring_cap), room);
        if (verbose && n > 0) {
            note("ffs: read %zd bytes on %s", n, e->name);
        }
        if (n == 0) {
            /*
             * The endpoint has nothing: a zero-length packet the host sent, or
             * an endpoint the UDC has closed.  Either way there is nothing to
             * forward, and going straight back into read(2) turns "nothing" into
             * a spin that floods the emulator with empty transfers -- which the
             * guest reads as empty commands, all the way down its own loop.
             */
            sleep_ms(20);
        }

        pthread_mutex_lock(&e->lock);
        if (n > 0) {
            e->ring_head += (uint32_t)n;
            b.read_total += (uint64_t)n;
            wake_the_loop();
        }
        pthread_mutex_unlock(&e->lock);
        if (n < 0 && errno != EAGAIN && errno != EINTR) {
            note("ffs: %s read: %s", e->name, strerror(errno));
            break;
        }
    }
    /* tell stop_workers() this one is out, so it need not wait the full time */
    pthread_mutex_lock(&e->lock);
    e->done = true;
    pthread_cond_broadcast(&e->cv);
    pthread_mutex_unlock(&e->lock);
    return NULL;
}

static void *in_worker(void *arg)
{
    struct ep *e = arg;

    for (;;) {
        uint32_t len;

        pthread_mutex_lock(&e->lock);
        while (!e->stop && !e->len) {
            pthread_cond_wait(&e->cv, &e->lock);
        }
        if (e->stop) {
            pthread_mutex_unlock(&e->lock);
            break;
        }
        len = e->len;
        e->len = 0;
        e->writing = true;
        pthread_mutex_unlock(&e->lock);

        while (len) {
            ssize_t n;

            if (wait_for_io(e->fd, POLLOUT) != 0) {
                break;
            }
            n = write(e->fd, e->buf, len);
            if (n > 0) {
                if (verbose) {
                    note("ffs: %zd bytes on %s went to the host", n, e->name);
                }
                memmove(e->buf, e->buf + n, len - n);
                len -= (uint32_t)n;
            } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
                note("ffs: %s write: %s", e->name, strerror(errno));
                break;
            }
        }

        pthread_mutex_lock(&e->lock);
        e->writing = false;                  /* room for the next transfer */
        pthread_mutex_unlock(&e->lock);
        wake_the_loop();
    }
    /* tell stop_workers() this one is out, so it need not wait the full time */
    pthread_mutex_lock(&e->lock);
    e->done = true;
    pthread_cond_broadcast(&e->cv);
    pthread_mutex_unlock(&e->lock);
    return NULL;
}

static int start_workers(void)
{
    if (pipe(wake_pipe) < 0) {
        fail("pipe: %s", strerror(errno));
        return -1;
    }
    /*
     * Both ends non-blocking.  The loop drains the pipe until read(2) says
     * there is nothing left, which on a blocking pipe means it waits for a
     * write that never comes and the whole bridge stops there -- with both
     * workers parked on their condvars and nothing moving again.
     */
    for (int end = 0; end < 2; end++) {
        int fl = fcntl(wake_pipe[end], F_GETFL);

        if (fl < 0 || fcntl(wake_pipe[end], F_SETFL, fl | O_NONBLOCK) < 0) {
            fail("wake pipe: %s", strerror(errno));
            return -1;
        }
    }
    for (int i = 0; i < b.nep; i++) {
        struct ep *e = &b.eps[i];

        if (e->fd < 0) {
            continue;
        }
        if (e->in) {
            e->buf = malloc(XFER_MAX);
        } else {
            e->ring_cap = RING_BYTES;
            e->ring = malloc(e->ring_cap);
        }
        if (!e->in ? !e->ring : !e->buf) {
            fail("out of memory for %s", e->name);
            return -1;
        }
        pthread_mutex_init(&e->lock, NULL);
        pthread_cond_init(&e->cv, NULL);
        if (pthread_create(&e->thread, NULL, e->in ? in_worker : out_worker, e)) {
            fail("pthread_create: %s", strerror(errno));
            return -1;
        }
        e->started = true;
    }
    return 0;
}

static void stop_workers(void)
{
    for (int i = 0; i < b.nep; i++) {
        struct ep *e = &b.eps[i];

        if (!e->started) {
            continue;
        }
        pthread_mutex_lock(&e->lock);
        e->stop = true;
        pthread_cond_broadcast(&e->cv);
        pthread_mutex_unlock(&e->lock);
    }
    /*
     * Waiting for them has a deadline, because they cannot always be *asked* to
     * stop: this ffs blocks inside read(2) whether the fd is O_NONBLOCK or not
     * (it waits there for the UDC to queue more), so a worker caught by one
     * stays until the endpoint is closed -- which is what the unbind in
     * teardown(), before this, does.  Five seconds is generous for that, and a
     * worker still stuck after it is a thread this process is about to exit
     * anyway: a bridge that will not exit is one the phone's USB never gets
     * back.  Bionic has no timed join, so it is the worker saying it is done.
     */
    for (int i = 0; i < b.nep; i++) {
        struct ep *e = &b.eps[i];
        struct timespec until;

        if (!e->started) {
            continue;
        }
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += 5;
        pthread_mutex_lock(&e->lock);
        while (!e->done) {
            if (pthread_cond_timedwait(&e->cv, &e->lock, &until) == ETIMEDOUT) {
                fail("%s did not stop within 5 s; leaving it", e->name);
                break;
            }
        }
        e->joined = e->done;
        pthread_mutex_unlock(&e->lock);
    }
}

/* ---- ep0: one request from the phone at a time --------------------------- */

/*
 * ep0 is left on the loop's own thread: a control transfer is short and the
 * emulated gadget answers it at once, so there is nothing to wait long for and
 * keeping it here is what preserves the order the phone's requests arrive in.
 */
static void pump_ep0(void)
{
    struct usb_functionfs_event ev;
    ssize_t n = read(b.ep0, &ev, sizeof(ev));
    uint8_t setup[8];
    bool in;
    uint16_t wlength;

    if (n <= 0) {
        return;
    }
    note("ffs: event type %u", ev.type);
    if (ev.type == FUNCTIONFS_ENABLE) {
        replay_config();            /* the host configured us: tell the emulator */
        return;
    }
    if (ev.type != FUNCTIONFS_SETUP) {
        return;                   /* bind, unbind, disable, suspend, resume */
    }
    in = (ev.u.setup.bRequestType & USB_DIR_IN) != 0;
    wlength = le16((const uint8_t *)&ev.u.setup.wLength);
    note("ffs: <- %02x %02x value %04x", ev.u.setup.bRequestType,
         ev.u.setup.bRequest, le16((const uint8_t *)&ev.u.setup.wValue));

    setup[0] = ev.u.setup.bRequestType;
    setup[1] = ev.u.setup.bRequest;
    setup[2] = setup[3] = setup[4] = setup[5] = 0;
    setup[6] = wlength & 0xff;
    setup[7] = wlength >> 8;

    if (in) {
        /* a read: ask the gadget, and its answer is written back by pump() */
        submit_urb(EP0_URB, 0, true, setup, wlength, NULL, 0, b.ep0buf);
        return;
    }
    if (wlength) {
        /*
         * The PC is sending data.  One URB carries the setup packet and the data
         * stage together, the way vhci_hcd's host stack does it, because the
         * emulated gadget hands out one queue head per transfer and a SETUP that
         * overtook its own data would be answered from the wrong one.
         */
        static uint8_t data[4096];
        ssize_t got = read(b.ep0, data, MIN(wlength, (uint32_t)sizeof(data)));

        if (got < 0) {
            fail("ffs: ep0 read: %s", strerror(errno));
            return;
        }
        submit_urb(EP0_URB, 0, false, setup, 0, data, (uint32_t)got, NULL);
        return;
    }
    submit_urb(EP0_URB, 0, false, setup, 0, NULL, 0, NULL);
}

/* ---- what to do with what came back ------------------------------------- */

/* an ep0 answer goes straight back to the phone */
static void pump_ep0_answer(void)
{
    uint32_t got = EP0_URB->got;

    if (!EP0_URB->ready) {
        return;
    }
    EP0_URB->ready = false;
    if (got && write(b.ep0, b.ep0buf, got) < 0) {
        note("ffs: ep0 write: %s", strerror(errno));
    } else {
        note("ffs: -> %u bytes on ep0", got);
    }
}

/* a bulk transfer the emulated gadget has finished with */
static void pump_bulk(void)
{
    for (int i = 0; i < b.nep; i++) {
        struct ep *e = &b.eps[i];
        struct urb *u = &pending[i + 1];

        if (e->fd < 0 || !u->ready) {
            continue;
        }
        u->ready = false;

        /*
         * An OUT needs nothing done with its answer: the bytes went to the
         * emulator with the request, and the phone's endpoint was read before
         * that.  There is nothing to hand back, and pretending otherwise
         * (saying the endpoint has 0 bytes waiting) has the loop submit an
         * empty transfer to the emulator, which completes the gadget's
         * outstanding read as an empty command -- and then the next one, for as
         * long as the host stays plugged in.
         */
        if (!e->in) {
            continue;
        }
        if (u->status == USBIP_EPIPE) {
            /* the gadget gave up on this endpoint: let it try again */
            ioctl(e->fd, FUNCTIONFS_CLEAR_HALT);
            note("ffs: %s stalled, cleared", e->name);
            continue;
        }
        if (!u->got) {
            continue;
        }
        /* hand the bytes to the worker, which is blocked in write() on the UDC */
        pthread_mutex_lock(&e->lock);
        memcpy(e->buf, u->data, u->got);
        e->len = u->got;
        pthread_cond_signal(&e->cv);
        pthread_mutex_unlock(&e->lock);
        note("ffs: -> %s, %u bytes from the emulator", e->name, u->got);
    }
}

static void pump(void)
{
    struct pollfd fds[3];
    int usbip_slot = 0, ep0_slot = 1, wake_slot = 2;
    int woken;

    fds[usbip_slot].fd = b.ip;
    fds[usbip_slot].events = POLLIN;
    fds[ep0_slot].fd = b.ep0;
    fds[ep0_slot].events = POLLIN;
    fds[wake_slot].fd = wake_pipe[0];
    fds[wake_slot].events = POLLIN;

    /*
     * With a timeout, so the loop wakes on its own now and then.  That is what
     * ends a host transfer that did not finish on a guest-sized boundary: the
     * bytes that are left in the ring are the host's last few (a fastboot
     * command, the tail of a download) and there is nothing more coming to make
     * them a whole number of pages, so they go as they are.  See
     * ring_whole_pages().
     */
    woken = poll(fds, 3, QUIET_MS);
    if (woken < 0) {
        if (errno != EINTR) {
            fail("poll: %s", strerror(errno));
        }
        return;
    }
    if (!woken) {
        for (int i = 0; i < b.nep; i++) {
            struct ep *e = &b.eps[i];
            uint32_t held;

            if (e->in || e->fd < 0) {
                continue;
            }
            pthread_mutex_lock(&e->lock);
            held = ring_held(e);
            if (held && !e->flush && e->quiet == 0) {
                static int64_t last_tally;

                if (now_ms() - last_tally > 1000) {
                    last_tally = now_ms();
                    note("ffs: %s idle: read %llu, sent %llu, lost %llu, "
                         "ring %u", e->name,
                         (unsigned long long)b.read_total,
                         (unsigned long long)b.sent,
                         (unsigned long long)b.lost, held);
                }
            }
            if (held && !e->flush && e->quiet == 0) {
                /*
                 * The endpoint has gone quiet with something still in the ring.
                 * Say what, and how long it has been sitting there: this is the
                 * difference between the ring holding the host's data and the
                 * data having gone nowhere, and it is the one question a stalled
                 * download needs answered.
                 */
                note("ffs: %s quiet holding %u bytes (room %u, left %u)", e->name,
                     held, room_left(e), e->download_left);
            }
            if (!held || e->flush) {
                e->quiet = 0;
            } else {
                /*
                 * The loop hands over whole pages as they arrive, so what is left
                 * in the ring is a piece of one page, and the guest cannot finish
                 * its request without it -- nothing moves until it goes.
                 *
                 * Only a command-sized piece may go out unaligned, and only after
                 * the phone has gone quiet for a moment: a fastboot command is at
                 * most 64 bytes into the guest's 64-byte buffer, the host is
                 * waiting for the reply, and the guest needs the short completion
                 * to see the command at all.
                 *
                 * A piece of a *download* must never be sent like that, and no
                 * amount of waiting makes it safe: the phone goes quiet while our
                 * own worker is between reads (this ffs blocks inside read(2) for
                 * seconds at a time), and sending 3,909 of the guest's 4,096 then
                 * is a short request in the middle of a transfer -- fastboot calls
                 * that an error and stops.  What finishes a download instead is
                 * knowing how many bytes it has left, which the download: command
                 * says exactly.  See note_download_left().
                 */
                e->quiet += QUIET_MS;
                if (held <= COMMAND_MAX && room_left(e) > 0 &&
                    e->quiet >= QUIET_MS && !(e->download_left && !e->download_done)) {
                    note("ffs: %s quiet for %u ms with %u bytes held; "
                         "sending them", e->name, e->quiet, held);
                    e->flush = true;
                    e->quiet = 0;
                }
            }
            pthread_mutex_unlock(&e->lock);
        }
    }
    if (fds[wake_slot].revents) {
        char drain[64];

        while (read(wake_pipe[0], drain, sizeof(drain)) > 0) {
            /* one wake-up per transfer; the pipe says only that something moved */
        }
    }
    if (fds[usbip_slot].revents) {
        uint8_t reply[HDR_SIZE];
        ssize_t got = read(b.ip, reply, sizeof(reply));

        if (got <= 0) {
            if (got < 0 && errno != EINTR && errno != EAGAIN) {
                fail("usb/ip: the link closed");
                stop_now = 1;
                return;
            }
        } else if ((size_t)got >= HDR_SIZE) {
            uint32_t cmd = get32(reply);

            if (cmd == RET_SUBMIT) {
                on_answer(get32(reply + 4), reply);
            } else if (cmd == CMD_UNLINK) {
                uint32_t target = get32(reply + 20);

                for (int i = 0; i <= b.nep; i++) {
                    if (pending[i].used && pending[i].seq == target) {
                        pending[i].used = false;
                        note("emu: seq %u unlinked", target);
                    }
                }
            }
        }
    }
    if (fds[ep0_slot].revents) {
        pump_ep0();
    }

    /*
     * Hand over what came back *before* asking for anything new.  submit_urb()
     * clears the slot it reuses, and it reuses the slot of the endpoint it is
     * asking, so collecting afterwards means the new request wipes the answer
     * the worker was about to be given.
     */
    pump_ep0_answer();
    pump_bulk();

    /* What a worker has waiting for us, and what it still owes the endpoint. */
    for (int i = 0; i < b.nep; i++) {
        struct ep *e = &b.eps[i];
        struct urb *u = &pending[i + 1];
        uint32_t held;
        uint32_t len = 0;
        uint8_t out[XFER_MAX];

        if (e->fd < 0) {
            continue;
        }
        pthread_mutex_lock(&e->lock);
        held = e->in ? 0 : ring_held(e);
        if (held && !u->used && !u->ready) {
            /*
             * One transfer at a time per endpoint, but whatever else has come
             * off the phone stays in the ring meanwhile, so the UDC keeps being
             * drained at its own pace instead of ours.
             *
             * Whole guest-sized pieces unless the phone has gone quiet and
             * `flush` says this really is the end of the host's transfer: see
             * ring_whole_pages() for why splitting one up ends fastboot.  The
             * copy goes through ring_copy_out(), so the ring wrapping where it
             * likes does not shorten the piece.
             */
            uint32_t want = out_want(e, held);

            len = want > XFER_MAX ? XFER_MAX : want;
            if (len) {
                ring_copy_out(e, out, e->ring_tail, len);
                e->ring_tail += len;       /* submit_urb() writes it out itself */
                e->flush = false;
                if (!e->download_left) {
                    /*
                     * A command, and the only place the size of the transfer
                     * that follows can come from: fastboot sends
                     * "download:004be000\n" and then the image, and what is left
                     * over when the host stops is exactly the piece the guest's
                     * last request wants.
                     *
                     * It comes to us in pieces -- the UDC hands over what fits,
                     * which was eleven bytes of it -- so it is reassembled here
                     * until the newline, and only then read.
                     */
                    for (uint32_t i = 0; i < len && e->command_len + 1 < sizeof(e->command); i++) {
                        e->command[e->command_len++] = (char)out[i];
                    }
                    note_download_left(e);
                } else {
                    e->download_left -= len;
                    if (!e->download_left) {
                        note("ffs: %s: the download is complete", e->name);
                    }
                }
                pthread_cond_signal(&e->cv);  /* room in the ring again */
            }
        }
        pthread_mutex_unlock(&e->lock);
        if (len) {
            if (submit_urb(u, e->num, false, NULL, 0, out, len, NULL) < 0) {
                /*
                 * Cannot happen -- the slot was free a moment ago -- but if it
                 * does, the bytes are already out of the ring, so say so instead
                 * of leaving the guest waiting for a transfer nobody sent.
                 */
                fail("ffs: could not submit %u bytes on %s; they are lost", len,
                     e->name);
                b.lost += len;
            } else {
                b.sent += len;
            }
            note("emu: <- submit seq %u ep %d out (%u)", u->seq, e->num, len);
            continue;
        }
        /*
         * An IN keeps its request outstanding while the emulated gadget thinks
         * about what to send: that is what stops the endpoint timing out
         * between packets.  A worker inside write() is the one thing that
         * pauses it, since the UDC is busy with that transfer.
         */
        pthread_mutex_lock(&e->lock);
        bool free_to_ask = e->in && !e->writing;
        pthread_mutex_unlock(&e->lock);
        if (free_to_ask && !u->used) {
            submit_urb(u, e->num, true, NULL, XFER_MAX, NULL, 0, e->buf);
            note("emu: <- submit seq %u ep %d in (%u)", u->seq, e->num, XFER_MAX);
        }
    }
}

/*
 * Is there something an endpoint could send right now but did not?
 *
 * The loop submits one transfer per endpoint per pass and then goes back to
 * poll() for the next answer.  That is fine as long as something else wakes it,
 * and usually the worker's read does -- but a burst of reads can be drained out
 * of the wake pipe in one pass while the endpoint's transfer is still in flight,
 * and then the last wake has already been spent: the ring keeps half a megabyte
 * of a download, the emulator is idle, and the host waits for the rest.  This is
 * what that looks like, so serve() asks rather than assuming.
 */
static bool backlog(void)
{
    for (int i = 0; i < b.nep; i++) {
        struct urb *u = &pending[i + 1];
        uint32_t held;

        if (b.eps[i].in || b.eps[i].fd < 0 || u->used || u->ready) {
            continue;
        }
        pthread_mutex_lock(&b.eps[i].lock);
        held = ring_held(&b.eps[i]);
        pthread_mutex_unlock(&b.eps[i].lock);
        if (held && out_want(&b.eps[i], held)) {
            return true;
        }
    }
    return false;
}

static void serve(void)
{
    b.running = true;
    while (!stop_now) {
        pump();
        if (backlog()) {
            sleep_ms(1);
        }
    }
    note("stopping");
}

/* ---- putting the phone back --------------------------------------------- */

static void teardown(void)
{
    char path[320];

    /*
     * Off the bus first, and before the workers are waited for.
     *
     * Both halves matter.  Taking the UDC back is what we owe the phone anyway,
     * and it is also what ends this ffs's endpoints: a worker sitting in
     * read(2) when the port goes away is a worker that never comes back,
     * because this driver waits for the UDC inside the read whether the fd is
     * O_NONBLOCK or not.  Joining first and unbinding after is a bridge that
     * takes the phone's USB down with it and never gives it back -- which is
     * what stopping one used to do, silently, every time.
     */
    quiet = true;
    set_udc(b.gadget, "");
    quiet = false;
    if (b.bound) {
        b.bound = false;
        note("released the UDC");
    }
    stop_workers();
    if (b.ep0 >= 0) {
        close(b.ep0);            /* this is what takes ffs out of FFS_ACTIVE */
        b.ep0 = -1;
    }
    for (int i = 0; i < b.nep; i++) {
        /*
         * Only where the worker is out of it: closing a file another thread is
         * blocked in is its own kind of trouble, and this process is about to
         * exit anyway, which closes them just as well.
         */
        if (b.eps[i].started && !b.eps[i].joined) {
            continue;              /* the worker still owns all of this */
        }
        if (b.eps[i].fd >= 0) {
            close(b.eps[i].fd);
            b.eps[i].fd = -1;
        }
        free(b.eps[i].buf);
        b.eps[i].buf = NULL;
        free(b.eps[i].ring);
        b.eps[i].ring = NULL;
    }
    if (b.mount_dir[0]) {
        umount(b.mount_dir);
    }
    if (b.cfg_dir[0] && b.ffs_name[0]) {
        snprintf(path, sizeof(path), "%s/%s", b.cfg_dir, b.ffs_name);
        unlink(path);
    }
    if (b.func[0]) {
        rm_dir(b.func);
    }
    if (b.mount_dir[0]) {
        rm_dir(b.mount_dir);
    }
    /*
     * Android's own gadget is put back by its HAL.  sys.usb.config is left
     * alone: setting it restarts adbd, and taking the USB port away is not worth
     * losing the connection over -- the HAL binds g1 again on its own.
     */
    if (b.hal_stopped && system("start " USB_HAL) == 0) {
        note("started " USB_HAL);
    }
    if (b.ip >= 0) {
        close(b.ip);
        b.ip = -1;
    }
}

/* ---- main --------------------------------------------------------------- */

/*
 * --selftest: does the driver take this exact blob, and does it care how we
 * write it?  Useful when the bridge's own write is refused and the same bytes
 * from a shell are not.
 */
static int selftest(void)
{
    static const uint8_t blob[] = {
        0x03, 0x00, 0x00, 0x00,  0x5d, 0x00, 0x00, 0x00,  0x07, 0x00, 0x00, 0x00,
        0x03, 0x00, 0x00, 0x00,  0x03, 0x00, 0x00, 0x00,  0x03, 0x00, 0x00, 0x00,
        0x09, 0x04, 0x00, 0x00, 0x02, 0xff, 0x42, 0x03, 0x03,
        0x07, 0x05, 0x81, 0x02, 0x00, 0x02, 0x00,
        0x07, 0x05, 0x01, 0x02, 0x00, 0x02, 0x00,
        0x09, 0x04, 0x00, 0x00, 0x02, 0xff, 0x42, 0x03, 0x03,
        0x07, 0x05, 0x81, 0x02, 0x00, 0x02, 0x00,
        0x07, 0x05, 0x01, 0x02, 0x00, 0x02, 0x00,
        0x09, 0x04, 0x00, 0x00, 0x02, 0xff, 0x42, 0x01, 0x03,
        0x07, 0x05, 0x81, 0x02, 0x00, 0x02, 0x00,
        0x07, 0x05, 0x01, 0x02, 0x00, 0x02, 0x00,
    };
    const struct {
        const char *what;
        int flags;
        int split;
    } how[] = {
        { "rdwr, one write", O_RDWR, 0 },
        { "wronly, one write", O_WRONLY, 0 },
        { "rdwr, header then body", O_RDWR, 1 },
        { "rdwr nonblock, one write", O_RDWR | O_NONBLOCK, 0 },
    };
    unsigned n = 0;

    snprintf(b.gadget, sizeof(b.gadget), "g2");
    for (unsigned i = 0; i < sizeof(how) / sizeof(how[0]); i++) {
        char name[64], dir[320], path[400];
        int fd;
        ssize_t r;

        snprintf(name, sizeof name, "st%u", n++);
        snprintf(dir, sizeof dir, "%s/%s", FFS_PARENT, name);
        snprintf(path, sizeof path, "%s/%s/functions/ffs.%s",
                 CONFIGFS_GADGETS, b.gadget, name);
        if (make_dir(path) < 0) {
            note("selftest: %s: no function dir", how[i].what);
            continue;
        }
        make_dir(dir);
        if (mount(name, dir, "functionfs", 0, "") < 0) {
            note("selftest: %s: mount: %s", how[i].what, strerror(errno));
            continue;
        }
        snprintf(path, sizeof path, "%s/ep0", dir);
        fd = open(path, how[i].flags);
        if (fd < 0) {
            note("selftest: %s: open: %s", how[i].what, strerror(errno));
            continue;
        }
        errno = 0;
        if (how[i].split) {
            r = write(fd, blob, 24);
            r = r < 0 ? r : write(fd, blob + 24, sizeof(blob) - 24);
        } else {
            r = write(fd, blob, sizeof(blob));
        }
        note("selftest: %s: %zd bytes, %s", how[i].what, r,
             r < 0 ? strerror(errno) : "accepted");
        close(fd);
        umount2(dir, MNT_DETACH);
        rmdir(dir);
        snprintf(path, sizeof path, "%s/%s/functions/ffs.%s",
                 CONFIGFS_GADGETS, b.gadget, name);
        rmdir(path);
    }
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: usbffs --usbip PATH [options]\n"
            "  --usbip PATH     QEMU's USB/IP socket (usb-ip-socket); we connect\n"
            "  --ffs-name NAME  the FunctionFS instance name (default: usb<pid>)\n"
            "  --gadget NAME    the configfs gadget to take (default: hd2)\n"
            "  --udc NAME       the UDC to bind (default: the only one)\n"
            "  --pidfile PATH   where to write our pid, for whoever started us\n"
            "  --verbose        say what is going on\n");
}

/*
 * Our pid, where the app can find it.
 *
 * The app ships this as libhtcleo-usbffs.so and runs it out of the APK's
 * nativeLibraryDir, so its process name on the phone is *that*, not "usbffs":
 * pidof usbffs and pkill -x usbffs never match it, and the app's own stop
 * silently left every bridge it had ever started running -- still holding the
 * UDC, with the phone's USB HAL stopped.  A pidfile is not a workaround for
 * that, it is the only reliable handle on a process we do not spawn ourselves
 * (the app starts us through a root shell, and the shell is gone by the time
 * anything wants to stop us again).
 */
static char pidfile[320];

static void write_pidfile(void)
{
    FILE *f = fopen(pidfile, "w");

    if (!f) {
        fail("pidfile %s: %s", pidfile, strerror(errno));
        return;
    }
    fprintf(f, "%u\n", (unsigned)getpid());
    fclose(f);
    note("pid %u in %s", (unsigned)getpid(), pidfile);
}

/*
 * Stop a bridge left over from a run that ended badly, so that two of us are
 * never on the UDC at once: they take the port from each other, and the host's
 * data ends up in whichever one won the last round.
 */
static void stop_the_last_one(void)
{
    long pid;
    FILE *f = fopen(pidfile, "r");

    if (!f) {
        return;
    }
    if (fscanf(f, "%ld", &pid) != 1 || pid <= 0) {
        fclose(f);
        return;
    }
    fclose(f);
    if (pid == (long)getpid() || kill((pid_t)pid, 0) != 0) {
        return;                     /* gone already, or it is us */
    }
    note("the last bridge (pid %ld) is still running; stopping it first", pid);
    kill((pid_t)pid, SIGTERM);
    for (int i = 0; i < 50 && kill((pid_t)pid, 0) == 0; i++) {
        sleep_ms(100);
    }
    if (kill((pid_t)pid, 0) == 0) {
        note("pid %ld did not stop; killing it", pid);
        kill((pid_t)pid, SIGKILL);
        sleep_ms(200);
    }
}

int main(int argc, char **argv)
{
    const char *usbip = NULL;
    struct sigaction sa;

    snprintf(b.ffs_name, sizeof(b.ffs_name), "usb%u", (unsigned)getpid());
    snprintf(b.gadget, sizeof(b.gadget), "hd2");
    b.ip = -1;
    b.ep0 = -1;
    b.ep0buf = calloc(1, 4096);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--usbip") && i + 1 < argc) {
            usbip = argv[++i];
        } else if (!strcmp(argv[i], "--ffs-name") && i + 1 < argc) {
            snprintf(b.ffs_name, sizeof(b.ffs_name), "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--gadget") && i + 1 < argc) {
            snprintf(b.gadget, sizeof(b.gadget), "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--udc") && i + 1 < argc) {
            snprintf(b.udc, sizeof(b.udc), "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--pidfile") && i + 1 < argc) {
            snprintf(pidfile, sizeof(pidfile), "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v")) {
            verbose = 1;
        } else if (!strcmp(argv[i], "--selftest")) {
            return selftest();
        } else {
            usage();
            return 2;
        }
    }
    if (!usbip) {
        usage();
        return 2;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    if (geteuid() != 0) {
        fail("this has to run as root: the UDC and configfs are");
        return 1;
    }
    if (pidfile[0]) {
        stop_the_last_one();
        write_pidfile();
    }
    if (find_gadget(b.gadget, sizeof(b.gadget)) < 0) {
        /*
         * A gadget of our own, so the phone's is left alone rather than
         * rebuilt: we drop our function out of the configuration on the way out
         * and the USB HAL puts its own back, which is a far smaller thing to ask
         * of it than a second gadget appearing and disappearing underneath it.
         * A kernel that will not give us one (ENOMEM/EEXIST when a gadget was
         * removed earlier in this boot) gets the phone's instead, and says so.
         */
        char path[320];

        snprintf(path, sizeof(path), "%s/%s", CONFIGFS_GADGETS, b.gadget);
        if (make_dir(path) == 0) {
            note("created gadget %s", b.gadget);
        } else if (any_gadget(b.gadget, sizeof(b.gadget)) < 0) {
            fail("no USB gadget in %s, and %s could not be created",
                 CONFIGFS_GADGETS, b.gadget);
            return 1;
        } else {
            note("using the phone's own gadget %s", b.gadget);
        }
    }
    if (!b.udc[0] && find_udc(b.udc, sizeof(b.udc)) < 0) {
        return 1;
    }
    note("gadget %s, UDC %s, instance %s", b.gadget, b.udc, b.ffs_name);

    /* QEMU is the one that listens, so this waits for it */
    for (int tries = 0; tries < 300 && !stop_now; tries++) {
        b.ip = connect_usbip(usbip);
        if (b.ip >= 0) {
            break;
        }
        sleep_ms(100);
    }
    if (b.ip < 0) {
        fail("no USB/IP link on %s", usbip);
        return 1;
    }
    note("connected to the emulator on %s", usbip);

    /* the emulated gadget only answers once its driver has started */
    for (int tries = 0; tries < 900 && !stop_now; tries++) {
        if (probe_gadget() == 0) {
            break;
        }
        sleep_ms(100);
    }
    if (!b.dev_len) {
        fail("the emulated phone never answered: is it running?");
        return 1;
    }
    if (alloc_eps() < 0) {
        return 1;
    }
    if (setup_configfs() < 0 || setup_ffs() < 0 || take_udc() < 0) {
        teardown();
        return 1;
    }
    if (start_workers() < 0) {
        teardown();
        return 1;
    }
    logline("usbffs: the emulated HD2 is on the USB port");

    serve();
    teardown();
    if (pidfile[0]) {
        unlink(pidfile);
    }
    return 0;
}