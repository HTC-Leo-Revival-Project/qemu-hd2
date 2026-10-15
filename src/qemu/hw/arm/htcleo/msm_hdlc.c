/*
 * QSD8250 modem HDLC accelerator (0xB8100000)
 *
 * AMSS's PPP (ps_hdlc_lib.c, hdlc_drv.c) frames and unframes async HDLC
 * (RFC 1662) with this block once LCP has set the ACCM to 0 ("switched
 * HDLC mode to HW"): packet data calls run their PPP through it.  Two
 * engines, polled, reverse-engineered from AMSS 2.15.50.14:
 *
 *   framer     +0x100 command: 0 reset, 2 begin a frame (FCS = 0xFFFF,
 *                     opening flag), 3 end it (FCS, closing flag), 4/1
 *                     before the output is read
 *              +0x104 ACCM (control characters escaped besides 7D/7E)
 *              +0x108 framed octets available (9 bits)
 *              +0x10C overflow
 *              +0x300 input: packet octets (byte or word writes)
 *              +0x400 output: framed octets (byte or word reads)
 *   unframer   +0x200 command: 0 reset (FIFOs), 1 process the input
 *              +0x204 units available
 *              +0x20C overflow
 *              +0x210 state: bit 31 escape pending, bits 30-29 in a frame
 *                     (11) or looking for a flag (00); bits 25-24 and 23-0
 *                     residual octets (never used here)
 *              +0x214 the running FCS
 *              +0x500 input: framed octets
 *              +0x600 output: units, each a header word (bit 31 FCS good,
 *                     bit 30 end of frame, bits 7-0 the octet count) and
 *                     the octets packed in words, little endian; a frame's
 *                     octets end with its 2 FCS octets
 * AMSS saves the unframer's state and FCS after each call and loads them
 * back before the next, so frames can span calls.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/fifo8.h"
#include "hw/core/sysbus.h"
#include "htcleo.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmHdlcState, MSM_HDLC)

#define FIFO_SIZE       4096
#define UNIT_MAX        128
#define FCS_GOOD        0xF0B8

struct MsmHdlcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    /* framer */
    uint32_t accm;
    uint16_t f_fcs;
    bool f_open;                /* a frame begun, its flag not yet out */
    Fifo8 f_out;
    /* unframer */
    bool in_frame, escape;
    uint16_t u_fcs;
    uint8_t unit[UNIT_MAX];
    unsigned unit_len, frame_len;
    Fifo8 u_out;                /* words, as octets */
    unsigned units;
};

static uint16_t fcs16(uint16_t fcs, uint8_t b)
{
    fcs ^= b;
    for (int i = 0; i < 8; i++) {
        fcs = fcs & 1 ? (fcs >> 1) ^ 0x8408 : fcs >> 1;
    }
    return fcs;
}

/* ---- framer ---- */

static void f_put(MsmHdlcState *s, uint8_t b)
{
    if (!fifo8_is_full(&s->f_out)) {
        fifo8_push(&s->f_out, b);
    }
}

static void f_esc(MsmHdlcState *s, uint8_t b)
{
    if (b == 0x7e || b == 0x7d || (b < 0x20 && (s->accm >> b & 1))) {
        f_put(s, 0x7d);
        f_put(s, b ^ 0x20);
    } else {
        f_put(s, b);
    }
}

static void f_flag(MsmHdlcState *s)
{
    if (s->f_open) {
        f_put(s, 0x7e);
        s->f_open = false;
    }
}

static void f_data(MsmHdlcState *s, uint8_t b)
{
    f_flag(s);
    s->f_fcs = fcs16(s->f_fcs, b);
    f_esc(s, b);
}

static void f_command(MsmHdlcState *s, unsigned cmd)
{
    switch (cmd) {
    case 0:
        fifo8_reset(&s->f_out);
        s->f_open = false;
        break;
    case 2:
        s->f_fcs = 0xffff;
        s->f_open = true;
        break;
    case 3: {
        uint16_t fcs = ~s->f_fcs;

        f_flag(s);
        f_esc(s, fcs & 0xff);
        f_esc(s, fcs >> 8);
        f_put(s, 0x7e);
        break;
    }
    }
}

/* ---- unframer ---- */

static void u_word(MsmHdlcState *s, uint32_t w)
{
    if (fifo8_num_free(&s->u_out) >= 4) {
        for (int i = 0; i < 4; i++) {
            fifo8_push(&s->u_out, w >> (8 * i));
        }
    }
}

/* the unit so far, at the end of a frame (eof) or not */
static void u_emit(MsmHdlcState *s, bool eof)
{
    if (!s->unit_len && !eof) {
        return;
    }
    u_word(s, (eof && s->u_fcs == FCS_GOOD ? 1u << 31 : 0) |
              (eof ? 1u << 30 : 0) | s->unit_len);
    for (unsigned i = 0; i < s->unit_len; i += 4) {
        uint32_t w = 0;

        for (unsigned k = 0; k < 4 && i + k < s->unit_len; k++) {
            w |= s->unit[i + k] << (8 * k);
        }
        u_word(s, w);
    }
    s->unit_len = 0;
    s->units++;
}

static void u_byte(MsmHdlcState *s, uint8_t b)
{
    if (b == 0x7e) {
        if (s->in_frame && s->frame_len) {
            u_emit(s, true);            /* the end of a (non-empty) frame */
        }
        s->in_frame = true;
        s->escape = false;
        s->u_fcs = 0xffff;
        s->unit_len = s->frame_len = 0;
        return;
    }
    if (!s->in_frame) {
        return;                         /* looking for a flag */
    }
    if (b == 0x7d) {
        s->escape = true;
        return;
    }
    if (s->escape) {
        b ^= 0x20;
        s->escape = false;
    }
    s->u_fcs = fcs16(s->u_fcs, b);
    s->unit[s->unit_len++] = b;
    s->frame_len++;
    if (s->unit_len == UNIT_MAX) {
        u_emit(s, false);
    }
}

/* ---- registers ---- */

static uint64_t hdlc_read(void *opaque, hwaddr off, unsigned size)
{
    MsmHdlcState *s = opaque;
    uint64_t v = 0;

    switch (off) {
    case 0x104:
        return s->accm;
    case 0x108:
        return MIN(fifo8_num_used(&s->f_out), 0x1ff);
    case 0x204:
        return MIN(s->units, 0xff);
    case 0x210:
        return (s->escape ? 1u << 31 : 0) | (s->in_frame ? 3u << 29 : 0);
    case 0x214:
        return s->u_fcs;
    case 0x10c:
    case 0x20c:
        return 0;                       /* no overflow */
    }
    if (off >= 0x400 && off < 0x500) {
        for (unsigned i = 0; i < size && !fifo8_is_empty(&s->f_out); i++) {
            v |= (uint64_t)fifo8_pop(&s->f_out) << (8 * i);
        }
        return v;
    }
    if (off >= 0x600 && off < 0x700) {
        for (unsigned i = 0; i < size && !fifo8_is_empty(&s->u_out); i++) {
            v |= (uint64_t)fifo8_pop(&s->u_out) << (8 * i);
        }
        return v;
    }
    qemu_log_mask(LOG_UNIMP, "msm-hdlc: read 0x%03x\n", (unsigned)off);
    return 0;
}

static void hdlc_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MsmHdlcState *s = opaque;

    switch (off) {
    case 0x100:
        f_command(s, val & 0xff);
        return;
    case 0x104:
        s->accm = val;
        return;
    case 0x200:
        if ((val & 0xff) == 0) {
            fifo8_reset(&s->u_out);
            s->units = 0;
            s->unit_len = 0;
        } else {
            u_emit(s, false);           /* what the input left unfinished */
        }
        return;
    case 0x210:
        s->escape = val >> 31 & 1;
        s->in_frame = (val >> 29 & 3) == 3;
        /* (a frame in progress across calls has had octets) */
        s->frame_len = s->in_frame;
        return;
    case 0x214:
        s->u_fcs = val;
        return;
    }
    if (off >= 0x300 && off < 0x400) {
        for (unsigned i = 0; i < size; i++) {
            f_data(s, val >> (8 * i));
        }
        return;
    }
    if (off >= 0x500 && off < 0x600) {
        for (unsigned i = 0; i < size; i++) {
            u_byte(s, val >> (8 * i));
        }
        return;
    }
    qemu_log_mask(LOG_UNIMP, "msm-hdlc: write 0x%03x = %08x\n", (unsigned)off,
                  (unsigned)val);
}

static const MemoryRegionOps hdlc_ops = {
    .read = hdlc_read,
    .write = hdlc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void hdlc_reset(DeviceState *dev)
{
    MsmHdlcState *s = MSM_HDLC(dev);

    fifo8_reset(&s->f_out);
    fifo8_reset(&s->u_out);
    s->accm = 0xffffffff;
    s->f_fcs = s->u_fcs = 0xffff;
    s->f_open = s->in_frame = s->escape = false;
    s->unit_len = s->units = s->frame_len = 0;
}

static void hdlc_init(Object *obj)
{
    MsmHdlcState *s = MSM_HDLC(obj);

    memory_region_init_io(&s->iomem, obj, &hdlc_ops, s, "msm-hdlc", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    fifo8_create(&s->f_out, FIFO_SIZE);
    fifo8_create(&s->u_out, FIFO_SIZE);
}

static void hdlc_class_init(ObjectClass *oc, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(oc), hdlc_reset);
}

static const TypeInfo msm_hdlc_info = {
    .name = TYPE_MSM_HDLC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmHdlcState),
    .instance_init = hdlc_init,
    .class_init = hdlc_class_init,
};

static void msm_hdlc_register(void)
{
    type_register_static(&msm_hdlc_info);
}
type_init(msm_hdlc_register)
