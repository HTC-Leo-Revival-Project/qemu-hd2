/*
 * QSD8250 modem-side timers (PA 0xB8000000, seen only by the modem core)
 *
 * Reverse-engineered from the AMSS 2.15.50.14 image (L4 kernel and the
 * timetick/sleep drivers):
 *   0x000 TICK_EN     bit 0 runs the L4 tick timer; 0 -> 1 restarts it
 *   0x004 TICK_COUNT  slow-clock ticks since the restart
 *   0x008 TICK_MATCH  period in slow-clock ticks (L4 sets 0xa3 = 5 ms);
 *                     each period raises the "tick" line (modem VIC 32)
 *   0x010 SLEEP_SYNC  bit 0: the last SLEEP_MATCH write has taken effect
 *   0x014 SLEEP_MATCH slow-clock count at which the sleep timer fires
 *   0x100..0x10c      watchdog; 0x108 is the free-running 32.768 kHz
 *                     slow-clock count (timetick), 0x10c is the kick
 *   0x594 + 4n        writing 1 interrupts another processor ("m2a" out
 *                     n, AMSS's HAL out ids 72-79): the apps core's
 *                     INT_A9_M2A_n, except n = 3 which is the QDSP6's
 *                     ("SPSS2ADSP_INT")
 *   0x510..0x598      second-level interrupt controllers: +0 clear (write
 *                     1), +4 enable, +8 status (input level or latched
 *                     edge), each cascading into one modem VIC line.  AMSS
 *                     numbers them by its HAL descriptor table:
 *                       0 0x544 -> line 41    1 0x574 -> line 54
 *                       2 0x554 -> line 47    3 0x534 -> line 40
 *                       6 0x564 -> line 51    7 0x510 -> line 37
 *                       8 0x524 -> line 39    9 0x584 -> line 42
 *                     (4 and 5 share 0x500, not modelled).  Inputs "subN",
 *                     output "subN-irq".  Known inputs: sub7 bit 3 the UIM,
 *                     sub0 bit 0 the QDSP6's SMD/SMSM interrupt.
 * Everything else is kept as plain registers and logged as unimplemented.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "htcleo.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmMtmrState, MSM_MTMR)

#define SCLK_HZ         32768

#define R_TICK_EN       0x000
#define R_TICK_COUNT    0x004
#define R_TICK_MATCH    0x008
#define R_SLEEP_SYNC    0x010
#define R_SLEEP_MATCH   0x014
#define R_DOG_BASE      0x100
#define R_SCLK_COUNT    0x108
#define R_DOG_KICK      0x10c
#define R_IRQ_OUT       0x594
#define IRQ_OUT_NUM     8
#define SUB_NUM         10
#define SUB_INPUTS      16

/* clear register of each second-level controller, 0 = not modelled */
static const hwaddr sub_base[SUB_NUM] = {
    0x544, 0x574, 0x554, 0x534, 0, 0, 0x564, 0x510, 0x524, 0x584,
};

struct MsmMtmrState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq tick_irq;
    qemu_irq sleep_irq;
    qemu_irq irq_out[IRQ_OUT_NUM];
    qemu_irq sub_irq[SUB_NUM];
    uint32_t sub_level[SUB_NUM], sub_latch[SUB_NUM];
    QEMUTimer *tick_timer;
    QEMUTimer *sleep_timer;

    uint32_t regs[0x400];
    int64_t tick_start;         /* ns when TICK_COUNT was 0 */
};

static uint64_t sclk_now(void)
{
    return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), SCLK_HZ,
                    NANOSECONDS_PER_SECOND);
}

static int64_t sclk_to_ns(uint64_t ticks)
{
    return muldiv64(ticks, NANOSECONDS_PER_SECOND, SCLK_HZ);
}

static void mtmr_tick_arm(MsmMtmrState *s)
{
    uint32_t match = s->regs[R_TICK_MATCH / 4];

    if (!(s->regs[R_TICK_EN / 4] & 1) || !match) {
        timer_del(s->tick_timer);
        return;
    }
    timer_mod(s->tick_timer, s->tick_start + sclk_to_ns(match));
}

static void mtmr_tick(void *opaque)
{
    MsmMtmrState *s = opaque;

    /* periodic: count restarts from 0 at every match */
    s->tick_start += sclk_to_ns(s->regs[R_TICK_MATCH / 4]);
    qemu_irq_pulse(s->tick_irq);
    mtmr_tick_arm(s);
}

static void mtmr_sleep_arm(MsmMtmrState *s)
{
    uint32_t match = s->regs[R_SLEEP_MATCH / 4];
    uint32_t now = sclk_now();

    /* fire when the 32-bit count passes match (it may already have) */
    timer_mod(s->sleep_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              sclk_to_ns((uint32_t)(match - now) < 0x80000000u ?
                         (uint32_t)(match - now) : 0));
}

static void mtmr_sleep(void *opaque)
{
    MsmMtmrState *s = opaque;

    qemu_irq_pulse(s->sleep_irq);
}

static uint32_t sub_status(MsmMtmrState *s, int k)
{
    return s->sub_level[k] | s->sub_latch[k];
}

static void sub_update(MsmMtmrState *s, int k)
{
    qemu_set_irq(s->sub_irq[k], !!(sub_status(s, k) &
                                   s->regs[(sub_base[k] + 4) / 4]));
}

static void sub_set(MsmMtmrState *s, int k, int n, int level)
{
    uint32_t bit = 1u << n;

    if (level && !(s->sub_level[k] & bit)) {
        s->sub_latch[k] |= bit;
    }
    s->sub_level[k] = level ? s->sub_level[k] | bit : s->sub_level[k] & ~bit;
    sub_update(s, k);
}

#define SUB_SET_FN(k) \
    static void sub##k##_set(void *opaque, int n, int level) \
    { \
        sub_set(opaque, k, n, level); \
    }
SUB_SET_FN(0) SUB_SET_FN(1) SUB_SET_FN(2) SUB_SET_FN(3)
SUB_SET_FN(6) SUB_SET_FN(7) SUB_SET_FN(8) SUB_SET_FN(9)

static const qemu_irq_handler sub_set_fn[SUB_NUM] = {
    sub0_set, sub1_set, sub2_set, sub3_set, NULL, NULL,
    sub6_set, sub7_set, sub8_set, sub9_set,
};

/* the controller whose registers are at off, its register in *reg */
static int sub_find(hwaddr off, int *reg)
{
    for (int k = 0; k < SUB_NUM; k++) {
        if (sub_base[k] && off >= sub_base[k] && off < sub_base[k] + 12) {
            *reg = (off - sub_base[k]) / 4;
            return k;
        }
    }
    return -1;
}

static uint64_t mtmr_read(void *opaque, hwaddr off, unsigned size)
{
    MsmMtmrState *s = opaque;
    int reg, k = sub_find(off, &reg);

    if (k >= 0) {
        return reg == 2 ? sub_status(s, k) : s->regs[off / 4];
    }
    switch (off) {
    case R_TICK_COUNT:
        if (!(s->regs[R_TICK_EN / 4] & 1)) {
            return 0;
        }
        return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->tick_start,
                        SCLK_HZ, NANOSECONDS_PER_SECOND);
    case R_SLEEP_SYNC:
        return 1;
    case R_SCLK_COUNT:
        return (uint32_t)sclk_now();
    case R_TICK_EN:
    case R_TICK_MATCH:
    case R_SLEEP_MATCH:
    case R_DOG_BASE ... R_SCLK_COUNT - 4:
    case R_DOG_KICK:
        return s->regs[off / 4];
    }
    qemu_log_mask(LOG_UNIMP, "msm-mtmr: read 0x%03" HWADDR_PRIx " = %08x\n",
                  off, s->regs[off / 4]);
    return s->regs[off / 4];
}

static void mtmr_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MsmMtmrState *s = opaque;
    uint32_t old = s->regs[off / 4];
    int reg, k = sub_find(off, &reg);

    s->regs[off / 4] = val;
    if (k >= 0) {
        if (reg == 0) {
            s->sub_latch[k] &= ~val;
        }
        sub_update(s, k);
        return;
    }
    switch (off) {
    case R_TICK_EN:
        if ((val & 1) && !(old & 1)) {
            s->tick_start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }
        mtmr_tick_arm(s);
        return;
    case R_TICK_MATCH:
        mtmr_tick_arm(s);
        return;
    case R_SLEEP_MATCH:
        mtmr_sleep_arm(s);
        return;
    case R_DOG_BASE ... R_DOG_KICK:
        return;
    case R_IRQ_OUT ... R_IRQ_OUT + 4 * (IRQ_OUT_NUM - 1):
        if (val & 1) {
            qemu_irq_pulse(s->irq_out[(off - R_IRQ_OUT) / 4]);
        }
        return;
    }
    qemu_log_mask(LOG_UNIMP, "msm-mtmr: write 0x%03" HWADDR_PRIx
                  " = %08" PRIx64 "\n", off, val);
}

static const MemoryRegionOps mtmr_ops = {
    .read = mtmr_read,
    .write = mtmr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void mtmr_reset(DeviceState *dev)
{
    MsmMtmrState *s = MSM_MTMR(dev);

    memset(s->regs, 0, sizeof(s->regs));
    for (int k = 0; k < SUB_NUM; k++) {
        s->sub_latch[k] = 0;
        if (sub_base[k]) {
            sub_update(s, k);
        }
    }
    timer_del(s->tick_timer);
    timer_del(s->sleep_timer);
}

static void mtmr_init(Object *obj)
{
    MsmMtmrState *s = MSM_MTMR(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &mtmr_ops, s, "msm-mtmr", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->tick_irq);
    sysbus_init_irq(sbd, &s->sleep_irq);
    qdev_init_gpio_out_named(DEVICE(obj), s->irq_out, "m2a", IRQ_OUT_NUM);
    for (int k = 0; k < SUB_NUM; k++) {
        g_autofree char *in = g_strdup_printf("sub%d", k);
        g_autofree char *out = g_strdup_printf("sub%d-irq", k);

        if (!sub_base[k]) {
            continue;
        }
        qdev_init_gpio_out_named(DEVICE(obj), &s->sub_irq[k], out, 1);
        qdev_init_gpio_in_named(DEVICE(obj), sub_set_fn[k], in, SUB_INPUTS);
    }
    s->tick_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mtmr_tick, s);
    s->sleep_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mtmr_sleep, s);
}

static void mtmr_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, mtmr_reset);
}

static const TypeInfo msm_mtmr_info = {
    .name = TYPE_MSM_MTMR,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmMtmrState),
    .instance_init = mtmr_init,
    .class_init = mtmr_class_init,
};

static void msm_mtmr_register(void)
{
    type_register_static(&msm_mtmr_info);
}
type_init(msm_mtmr_register)
