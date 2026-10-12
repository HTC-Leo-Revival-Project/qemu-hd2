/*
 * QSD8250 WCDMA system timer (STMR) of the modem core, at 0xB0005D00
 *
 * WCDMA L1 schedules its work on the 10 ms radio frame: the STMR counts
 * chips (3.84 Mcps) and raises three interrupts (modem VIC lines 12-14,
 * tramps 19-21) at programmable positions in the frame.  What AMSS
 * 2.15.50.14 does with it (tlm.c, the STMR driver at 0x6973EA-0x697800),
 * reverse-engineered:
 *
 *   0x24  write 1: latch the counters
 *   0x2C  latched counter (bits 23:0), same layout as 0x30
 *   0x30  latched frame counter: bits 18:0 the position in the frame in
 *         chip x 8 (0-307199), bits 24:19 the frame number modulo 64
 *   0x90  interrupt 0 status, one bit per event (16 events)
 *   0x94  interrupt 1 status (8 events)
 *   0x98  interrupt 2 status (8 events)
 *   0xA0, 0xA4, 0xA8  write 1 to clear status bits (the ISRs write back
 *         what they read)
 *   0xB0, 0xB4, 0xB8  event enables of interrupts 0, 1 and 2
 *   0xC0 + 4k  interrupt 0 event k position in the frame, in chips
 *   0x100 + 4k interrupt 1 event k
 *   0x120 + 4k interrupt 2 event k
 * The rest keeps what was written.  An interrupt line is high while one of
 * its enabled events is pending.
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

OBJECT_DECLARE_SIMPLE_TYPE(MsmWstmrState, MSM_WSTMR)

#define WSTMR_SIZE      0x200
#define R_LATCH         0x24
#define R_COUNT2        0x2C
#define R_COUNT         0x30
#define R_STATUS(i)     (0x90 + 4 * (i))
#define R_CLEAR(i)      (0xA0 + 4 * (i))
#define R_ENABLE(i)     (0xB0 + 4 * (i))

#define NUM_INTS        3
#define CHIPS_PER_FRAME 38400
#define FRAME_NS        10000000

static const struct {
    hwaddr base;
    int events;
} wstmr_int[NUM_INTS] = {
    { 0xC0, 16 }, { 0x100, 8 }, { 0x120, 8 },
};

struct MsmWstmrState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq[NUM_INTS];
    QEMUTimer *timer;

    uint32_t regs[WSTMR_SIZE / 4];
    uint32_t status[NUM_INTS];
    uint32_t latched;
    int64_t t0;
    uint64_t next_chip;         /* chip (since t0) of the next event check */
};

static uint64_t wstmr_chips(MsmWstmrState *s, int64_t now)
{
    return muldiv64(now - s->t0, CHIPS_PER_FRAME, FRAME_NS);
}

static int64_t wstmr_time(MsmWstmrState *s, uint64_t chip)
{
    return s->t0 + muldiv64(chip, FRAME_NS, CHIPS_PER_FRAME) + 1;
}

static uint32_t wstmr_count(MsmWstmrState *s)
{
    uint64_t cx8 = muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->t0,
                            CHIPS_PER_FRAME * 8, FRAME_NS);

    return ((cx8 / (CHIPS_PER_FRAME * 8)) & 0x3F) << 19 |
           cx8 % (CHIPS_PER_FRAME * 8);
}

static uint32_t wstmr_event_pos(MsmWstmrState *s, int i, int k)
{
    return s->regs[(wstmr_int[i].base >> 2) + k] % CHIPS_PER_FRAME;
}

static void wstmr_update(MsmWstmrState *s)
{
    for (int i = 0; i < NUM_INTS; i++) {
        qemu_set_irq(s->irq[i],
                     !!(s->status[i] & s->regs[R_ENABLE(i) >> 2]));
    }
}

/* the first enabled event strictly after chip 'from' */
static void wstmr_schedule(MsmWstmrState *s, uint64_t from)
{
    uint64_t best = UINT64_MAX;
    uint64_t frame = from / CHIPS_PER_FRAME;

    for (int i = 0; i < NUM_INTS; i++) {
        uint32_t en = s->regs[R_ENABLE(i) >> 2];

        for (int k = 0; k < wstmr_int[i].events; k++) {
            uint64_t c;

            if (!(en & (1u << k))) {
                continue;
            }
            c = frame * CHIPS_PER_FRAME + wstmr_event_pos(s, i, k);
            if (c <= from) {
                c += CHIPS_PER_FRAME;
            }
            best = MIN(best, c);
        }
    }
    if (best == UINT64_MAX) {
        timer_del(s->timer);
        return;
    }
    s->next_chip = best;
    timer_mod(s->timer, wstmr_time(s, best));
}

static void wstmr_tick(void *opaque)
{
    MsmWstmrState *s = opaque;
    uint64_t now = wstmr_chips(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    uint64_t from = s->next_chip;

    /* every enabled event between the last check and now fires */
    for (int i = 0; i < NUM_INTS; i++) {
        uint32_t en = s->regs[R_ENABLE(i) >> 2];

        for (int k = 0; k < wstmr_int[i].events; k++) {
            uint32_t pos = wstmr_event_pos(s, i, k);
            uint64_t c = (from / CHIPS_PER_FRAME) * CHIPS_PER_FRAME + pos;

            if (!(en & (1u << k))) {
                continue;
            }
            if (c < from) {
                c += CHIPS_PER_FRAME;
            }
            if (c <= now) {
                s->status[i] |= 1u << k;
            }
        }
    }
    wstmr_update(s);
    wstmr_schedule(s, MAX(now, from));
}

static uint64_t wstmr_read(void *opaque, hwaddr off, unsigned size)
{
    MsmWstmrState *s = opaque;

    switch (off) {
    case R_COUNT:
        return s->latched;
    case R_COUNT2:
        return s->latched & 0xFFFFFF;
    case R_STATUS(0):
    case R_STATUS(1):
    case R_STATUS(2):
        return s->status[(off - R_STATUS(0)) / 4];
    }
    return s->regs[(off >> 2) & (WSTMR_SIZE / 4 - 1)];
}

static void wstmr_write(void *opaque, hwaddr off, uint64_t val,
                        unsigned size)
{
    MsmWstmrState *s = opaque;

    s->regs[(off >> 2) & (WSTMR_SIZE / 4 - 1)] = val;
    switch (off) {
    case R_LATCH:
        if (val & 1) {
            s->latched = wstmr_count(s);
        }
        return;
    case R_CLEAR(0):
    case R_CLEAR(1):
    case R_CLEAR(2):
        s->status[(off - R_CLEAR(0)) / 4] &= ~val;
        wstmr_update(s);
        return;
    case R_ENABLE(0):
    case R_ENABLE(1):
    case R_ENABLE(2):
        wstmr_update(s);
        break;
    default:
        if (off < 0xC0 || off >= 0x140) {
            return;
        }
    }
    /* enables or event positions changed */
    wstmr_schedule(s, wstmr_chips(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)));
}

static const MemoryRegionOps wstmr_ops = {
    .read = wstmr_read,
    .write = wstmr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void wstmr_reset(DeviceState *dev)
{
    MsmWstmrState *s = MSM_WSTMR(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->status, 0, sizeof(s->status));
    s->latched = 0;
    s->t0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->next_chip = 0;
    timer_del(s->timer);
    wstmr_update(s);
}

static void wstmr_init(Object *obj)
{
    MsmWstmrState *s = MSM_WSTMR(obj);

    memory_region_init_io(&s->iomem, obj, &wstmr_ops, s, "msm-wstmr",
                          WSTMR_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    for (int i = 0; i < NUM_INTS; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[i]);
    }
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, wstmr_tick, s);
}

static void wstmr_class_init(ObjectClass *oc, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(oc), wstmr_reset);
}

static const TypeInfo msm_wstmr_info = {
    .name = TYPE_MSM_WSTMR,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmWstmrState),
    .instance_init = wstmr_init,
    .class_init = wstmr_class_init,
};

static void msm_wstmr_register(void)
{
    type_register_static(&msm_wstmr_info);
}
type_init(msm_wstmr_register)
