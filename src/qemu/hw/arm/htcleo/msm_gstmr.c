/*
 * QSD8250 GSM frame timer (GSTMR) of the modem core, at 0xB0007B00
 *
 * GSM L1 runs off the frame tick: one interrupt per TDMA frame (120/26 ms)
 * on modem VIC line 18 (tramp 26).  What AMSS 2.15.50.14 (gstmr.c,
 * 0x54c0e4-0x54c6c4) does with the block, reverse-engineered:
 *
 *   0x00  count (read): bits 17:4 the position in the frame in quarter
 *         symbols (0-4999), bits 3:0 a fraction, bit 18 the frame parity
 *         (the ISR checks it toggles from tick to tick)
 *   0x04  configuration; 0x180000 when GSM L1 installs its ISR
 *   0x0C  frame interrupt position: same layout as the count (bit 18 the
 *         parity, bits 17:4 the position), AMSS writes 4999 + an offset so
 *         the interrupt comes at the end of the frame; bit 19 is set too
 *   0x1C  reset: 0xF then 0 restarts the counter
 *   0x28  frame number load: bit 22 strobes bits 21:0 in
 *   0x2C  frame number (bits 21:0), counting frames modulo the hyperframe;
 *         the ISR checks it against its own count
 *   0x30  more configuration (bitfields AMSS sets one by one)
 *
 * The counter runs from the TCXO, here from the virtual clock; the frame
 * interrupt comes once a frame from the first configuration write on, at
 * the frame boundary (the interrupt position is not modelled: the ISR
 * wants the frame number and parity of the frame just started).
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/host-utils.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "htcleo.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmGstmrState, MSM_GSTMR)

#define R_COUNT     0x00
#define R_CONFIG    0x04
#define R_INT_POS   0x0C
#define R_RESET     0x1C
#define R_FN_LOAD   0x28
#define R_FN        0x2C

#define FRAME_QS    5000                /* quarter symbols per frame */
#define FRAME_UNITS (FRAME_QS << 4)     /* counter units per frame */
#define FRAME_NUM   120000000           /* frame = 120/26 ms */
#define FRAME_DEN   26
#define COUNT_PARITY (1u << 18)
#define FN_LOAD     (1u << 22)
#define FN_MASK     0x3FFFFF
#define HYPERFRAME  2715648

struct MsmGstmrState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    qemu_irq frame[2];          /* to the modem DSP, by frame parity */
    QEMUTimer *timer;

    uint32_t regs[0x10];
    int64_t t0;                 /* virtual time of frame 0, position 0 */
    uint64_t next;              /* frame whose interrupt is due next */
    uint64_t fn_frame;          /* frame index at which fn_base was loaded */
    uint32_t fn_base;
    bool running;
};

/* counter units since t0 */
static uint64_t gstmr_units(MsmGstmrState *s, int64_t now)
{
    return muldiv64(now - s->t0, (uint64_t)FRAME_UNITS * FRAME_DEN,
                    FRAME_NUM);
}

static int64_t gstmr_time(MsmGstmrState *s, uint64_t units)
{
    return s->t0 + muldiv64(units, FRAME_NUM,
                            (uint64_t)FRAME_UNITS * FRAME_DEN) + 1;
}

static uint64_t gstmr_frame(MsmGstmrState *s)
{
    return gstmr_units(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) /
           FRAME_UNITS;
}

static void gstmr_schedule(MsmGstmrState *s)
{
    if (!s->running) {
        timer_del(s->timer);
        return;
    }
    timer_mod(s->timer, gstmr_time(s, s->next * FRAME_UNITS));
}

static void gstmr_tick(void *opaque)
{
    MsmGstmrState *s = opaque;
    /* the DSP runs the frame's commands as the frame starts */
    qemu_irq_pulse(s->frame[s->next & 1]);
    qemu_irq_pulse(s->irq);
    /* the next frame, skipping ticks the guest slept through */
    s->next = MAX(s->next + 1, gstmr_frame(s) + 1);
    gstmr_schedule(s);
}

static uint64_t gstmr_read(void *opaque, hwaddr off, unsigned size)
{
    MsmGstmrState *s = opaque;

    if (off == R_COUNT) {
        uint64_t u = gstmr_units(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        uint32_t v = u % FRAME_UNITS;

        if ((u / FRAME_UNITS) & 1) {
            v |= COUNT_PARITY;
        }
        return v;
    }
    if (off == R_FN) {
        return (s->fn_base + gstmr_frame(s) - s->fn_frame) % HYPERFRAME;
    }
    return s->regs[(off >> 2) & 0xF];
}

static void gstmr_write(void *opaque, hwaddr off, uint64_t val,
                        unsigned size)
{
    MsmGstmrState *s = opaque;
    uint32_t old = s->regs[(off >> 2) & 0xF];
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    s->regs[(off >> 2) & 0xF] = val;
    switch (off) {
    case R_RESET:
        if (old && !val) {
            s->t0 = now;
            s->next = 1;
            s->fn_frame = 0;
            gstmr_schedule(s);
        }
        break;
    case R_FN_LOAD:
        if (val & FN_LOAD) {
            s->fn_base = (val & FN_MASK) % HYPERFRAME;
            s->fn_frame = gstmr_frame(s);
        }
        break;
    case R_CONFIG:
        if (!s->running) {
            s->running = true;
            s->next = gstmr_units(s, now) / FRAME_UNITS + 1;
            gstmr_schedule(s);
        }
        break;
    }
}

static const MemoryRegionOps gstmr_ops = {
    .read = gstmr_read,
    .write = gstmr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void gstmr_reset(DeviceState *dev)
{
    MsmGstmrState *s = MSM_GSTMR(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->t0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->next = 0;
    s->fn_base = 0;
    s->fn_frame = 0;
    s->running = false;
    timer_del(s->timer);
}

static void gstmr_init(Object *obj)
{
    MsmGstmrState *s = MSM_GSTMR(obj);

    memory_region_init_io(&s->iomem, obj, &gstmr_ops, s, "msm-gstmr", 0x40);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_out_named(DEVICE(obj), s->frame, "frame", 2);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gstmr_tick, s);
}

static void gstmr_class_init(ObjectClass *oc, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(oc), gstmr_reset);
}

static const TypeInfo msm_gstmr_info = {
    .name = TYPE_MSM_GSTMR,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmGstmrState),
    .instance_init = gstmr_init,
    .class_init = gstmr_class_init,
};

static void msm_gstmr_register(void)
{
    type_register_static(&msm_gstmr_info);
}
type_init(msm_gstmr_register)
