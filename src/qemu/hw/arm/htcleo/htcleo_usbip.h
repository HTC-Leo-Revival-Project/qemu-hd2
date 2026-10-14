/*
 * HTC HD2 USB gadget, low level: the emulated device on the host's own USB
 * stack (usb=lle)
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#ifndef HW_ARM_HTCLEO_HTCLEO_USBIP_H
#define HW_ARM_HTCLEO_HTCLEO_USBIP_H

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qemu/typedefs.h"

/* results of UsbipOps::poll() */
#define USBIP_NOPE      (-1)   /* the gadget has not primed the endpoint yet */
#define USBIP_STALL     (-2)   /* ... and has raised a hardware STALL on it */

typedef struct UsbipLink UsbipLink;

/*
 * A bulk packet on the wire is 4096 bytes for adb but 16 KB for fastboot,
 * which asks for as much as the endpoint's max packet size allows, so allow
 * a whole 64 KB transfer.  Getting this wrong is not a short read but a
 * desynchronised stream: the URB header says how many bytes follow, so a
 * frame cut short leaves its tail to be read as the next header.
 */
#define USBIP_XFER_MAX  65536
#define USBIP_HDR_SIZE  48     /* sizeof(struct usbip_header), always */
/*
 * Room for one whole frame plus slack.  Frames are taken out of the buffer
 * before the next one is read, so this only has to hold the largest one.
 */
#define USBIP_BUF       (USBIP_HDR_SIZE + USBIP_XFER_MAX + 1024)
/*
 * The host stack submits a whole control transfer's worth of URBs back to
 * back (SETUP, data, status) without waiting, so more than one can be in
 * flight; the gadget gets whatever is at the front.
 */
#define USBIP_MAX_URBS  8

/* one submitted URB, waiting for the gadget to answer it */
typedef struct UsbipUrb {
    uint32_t seq;
    uint8_t ep;
    bool dir_in;
    bool control;               /* it carried a SETUP packet */
    bool synth;                 /* ours (a status stage), not the host's */
    uint32_t want;              /* the host's wLength */
    uint32_t len;               /* bytes of `data` the host asked to send */
    uint32_t done;              /* ... and how many the gadget has taken */
    int64_t since;
    uint8_t setup[8];
    uint8_t data[USBIP_XFER_MAX];
} UsbipUrb;

/*
 * What the bridge needs from the emulated gadget.  msm_hsusb.c implements
 * these over its queue heads; nothing here knows about USB registers.
 */
typedef struct UsbipOps {
    /* a bus reset: the host found us (again) */
    void (*reset)(void *opaque);
    /* an 8-byte SETUP packet arrived on ep0 */
    void (*setup)(void *opaque, const uint8_t *pkt);
    /*
     * One round of bus activity on the endpoint the host is waiting for.
     * Returns the number of bytes moved, USBIP_NOPE when the gadget has
     * nothing queued yet, or USBIP_STALL when it holds a hardware STALL on
     * the endpoint.  `max` is the host's wLength: the answer must never be
     * longer, because the kernel refuses a longer one outright.
     */
    int  (*poll)(void *opaque, int ep, bool in,
                 const uint8_t *out, uint32_t out_len,
                 uint8_t *buf, uint32_t max);
} UsbipOps;

struct UsbipLink {
    const UsbipOps *ops;
    void *opaque;

    int fd;                     /* the end vhci_hcd was handed */
    int peer;                   /* ... and the end we speak on */
    bool attached;
    uint8_t port;
    uint8_t speed;
    uint8_t devid;

    QEMUTimer *timer;
    uint8_t inbuf[USBIP_BUF];
    size_t inlen;

    /* the URBs the host has submitted and we have not answered yet */
    UsbipUrb urbs[USBIP_MAX_URBS];
    uint8_t scratch[USBIP_XFER_MAX];
    int head, nurb;

    uint32_t n_submit, n_done, n_stall, n_unlink, n_drop;

    /* the host has enumerated us at least once */
    bool seen_setup;
    /* re-attaching after the gadget reset itself */
    int retry;
};

void usbip_link_init(UsbipLink *l, const UsbipOps *ops, void *opaque,
                     uint8_t port, uint8_t speed, Error **errp);
void usbip_link_cleanup(UsbipLink *l);
/* the gadget just primed an endpoint or stalled one: look for an answer */
void usbip_link_kick(UsbipLink *l);
/* the gadget reset its controller: plug it back in so the host finds it */
void usbip_link_reset(UsbipLink *l);

#endif