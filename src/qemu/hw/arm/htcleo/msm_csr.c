/*
 * Qualcomm QSD8x50 CSR block (0xAC100000): GPT / DGT timers and the
 * apps -> modem (A2M) doorbells.
 *
 * Linux (arch/arm/mach-msm/timer.c):
 *   GPT  at +0x00: MATCH 0x0, COUNT 0x4, ENABLE 0x8, CLEAR 0xC   32768 Hz
 *   DGT  at +0x10: MATCH 0x0, COUNT 0x4, ENABLE 0x8, CLEAR 0xC   19.2 MHz/(div+1)
 *   +0x20 DGT clock divider (htcleo writes 3, i.e. /4)
 *   ENABLE bit0 = count, bit1 = clear on match
 *   A2M doorbells at +0x400 + 4*n (dex_comm n=4, smd n=0, smsm n=5,
 *   proc_comm n=6, qdsp n=8)
 *
 * The counters only advance while enabled; CLEAR resets them to zero.
 * Time is taken from QEMU_CLOCK_VIRTUAL so the guest sees real rates.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/module.h"
#include "htcleo.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmCsrState, MSM_CSR)

#define CSR_NUM_A2M 16

typedef struct MsmTimer {
    struct MsmCsrState *csr;
    QEMUTimer *timer;
    qemu_irq irq;
    uint32_t match;
    uint32_t enable;
    uint32_t count_base;    /* count at base_ns */
    int64_t base_ns;
    bool is_dgt;
} MsmTimer;

struct MsmCsrState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    MsmTimer t[2];          /* 0 = GPT, 1 = DGT */
    uint32_t dgt_div;
    uint32_t regs[0x400];   /* everything we do not model, readback only */
    qemu_irq a2m[CSR_NUM_A2M];
};

static uint64_t timer_freq(MsmTimer *t)
{
    if (!t->is_dgt) {
        return 32768;
    }
    return 19200000 / ((t->csr->dgt_div & 3) + 1);
}

static uint32_t timer_count(MsmTimer *t)
{
    int64_t now;
    uint64_t ticks;
    uint32_t c;

    if (!(t->enable & 1)) {
        return t->count_base;
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    ticks = muldiv64(now - t->base_ns, timer_freq(t), NANOSECONDS_PER_SECOND);
    c = t->count_base + (uint32_t)ticks;
    if ((t->enable & 2) && t->match) {
        /* periodic mode: counter wraps back to 0 after reaching match */
        uint64_t period = (uint64_t)t->match + 1;
        c = (uint32_t)(((uint64_t)t->count_base + ticks) % period);
    }
    return c;
}

/* Re-anchor the running counter so later frequency/mode changes are exact */
static void timer_rebase(MsmTimer *t)
{
    t->count_base = timer_count(t);
    t->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void timer_schedule(MsmTimer *t)
{
    uint32_t now, delta;
    int64_t ns;

    if (!(t->enable & 1)) {
        timer_del(t->timer);
        return;
    }
    timer_rebase(t);
    now = t->count_base;
    if (t->enable & 2) {
        delta = (t->match >= now) ? t->match - now + 1 : 1;
    } else {
        delta = t->match - now;
        if (delta == 0) {
            delta = 0xFFFFFFFF;     /* full wrap */
        }
    }
    /* round up so the counter has really reached match on expiry */
    ns = muldiv64(delta, NANOSECONDS_PER_SECOND, timer_freq(t)) + 1;
    if (ns < 1000) {
        ns = 1000;
    }
    timer_mod(t->timer, t->base_ns + ns);
}

static void timer_expire(void *opaque)
{
    MsmTimer *t = opaque;

    /* The match interrupt is an edge into the VIC */
    qemu_irq_pulse(t->irq);
    if (t->enable & 2) {
        /* periodic: restart from zero */
        t->count_base = 0;
        t->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        timer_schedule(t);
    }
    /*
     * One-shot: the hardware would match again only after a full 32-bit
     * wrap; every user rewrites MATCH long before that, which re-arms.
     */
}

static uint64_t timer_read(MsmTimer *t, hwaddr off)
{
    switch (off) {
    case 0x0: return t->match;
    case 0x4: return timer_count(t);
    case 0x8: return t->enable;
    case 0xC: return 0;
    }
    return 0;
}

static void timer_write(MsmTimer *t, hwaddr off, uint32_t val)
{
    switch (off) {
    case 0x0:
        t->match = val;
        timer_schedule(t);
        break;
    case 0x4:
        timer_rebase(t);
        t->count_base = val;
        timer_schedule(t);
        break;
    case 0x8:
        timer_rebase(t);
        t->enable = val & 3;
        timer_schedule(t);
        break;
    case 0xC:
        t->count_base = 0;
        t->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        timer_schedule(t);
        break;
    }
}

static uint64_t csr_read(void *opaque, hwaddr off, unsigned size)
{
    MsmCsrState *s = opaque;

    if (off < 0x10) {
        return timer_read(&s->t[0], off);
    }
    if (off < 0x20) {
        return timer_read(&s->t[1], off - 0x10);
    }
    if (off == 0x20) {
        return s->dgt_div;
    }
    if (off >= 0x400 && off < 0x400 + 4 * CSR_NUM_A2M) {
        return 0;
    }
    return s->regs[(off >> 2) & 0x3ff];
}

static void csr_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MsmCsrState *s = opaque;

    if (off < 0x10) {
        timer_write(&s->t[0], off, val);
        return;
    }
    if (off < 0x20) {
        timer_write(&s->t[1], off - 0x10, val);
        return;
    }
    if (off == 0x20) {
        timer_rebase(&s->t[1]);
        s->dgt_div = val;
        timer_schedule(&s->t[1]);
        return;
    }
    if (off >= 0x400 && off < 0x400 + 4 * CSR_NUM_A2M) {
        int n = (off - 0x400) >> 2;
        if (val) {
            qemu_irq_pulse(s->a2m[n]);
        }
        return;
    }
    s->regs[(off >> 2) & 0x3ff] = val;
}

static const MemoryRegionOps csr_ops = {
    .read = csr_read,
    .write = csr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void csr_reset(DeviceState *dev)
{
    MsmCsrState *s = MSM_CSR(dev);

    for (int i = 0; i < 2; i++) {
        s->t[i].match = 0;
        s->t[i].enable = 0;
        s->t[i].count_base = 0;
        s->t[i].base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        timer_del(s->t[i].timer);
    }
    s->dgt_div = 3;     /* 19.2 MHz / 4 = 4.8 MHz, the boot default */
    memset(s->regs, 0, sizeof(s->regs));
    /*
     * SPSS_CLK_SEL (+0x104): the SPL leaves the Scorpion running from the
     * SCPLL (sel field = 1); acpuclock-scorpion.c BUG()s otherwise.
     */
    s->regs[0x104 >> 2] = 1 << 1;
}

static void csr_init(Object *obj)
{
    MsmCsrState *s = MSM_CSR(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &csr_ops, s, "msm-csr", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    for (int i = 0; i < 2; i++) {
        s->t[i].csr = s;
        s->t[i].is_dgt = (i == 1);
        s->t[i].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, timer_expire,
                                     &s->t[i]);
        sysbus_init_irq(sbd, &s->t[i].irq);
    }
    qdev_init_gpio_out_named(DEVICE(obj), s->a2m, "a2m", CSR_NUM_A2M);
}

static void csr_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    device_class_set_legacy_reset(dc, csr_reset);
}

static const TypeInfo msm_csr_info = {
    .name = TYPE_MSM_CSR,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmCsrState),
    .instance_init = csr_init,
    .class_init = csr_class_init,
};

static void msm_csr_register(void)
{
    type_register_static(&msm_csr_info);
}
type_init(msm_csr_register)
