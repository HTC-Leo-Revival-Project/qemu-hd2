/*
 * Qualcomm MSM (Scorpion) VIC and SIRC interrupt controllers
 *
 * Register layout from arch/arm/mach-msm/irq.c (CONFIG_ARCH_MSM_SCORPION),
 * arch/arm/mach-msm/sirc.c and cLK platform/qsd8k/interrupts.c.
 *
 * Linux's entry macro reads VIC_IRQ_VEC_RD (0xD0) and then
 * VIC_IRQ_VEC_PEND_RD (0xD4), expecting the pending source number or -1.
 * cLK reads the same pair and writes VIC_IRQ_VEC_WR when done.
 *
 * The modem core's VIC (baseband=lle, at 0xB8001000) is the same block.
 * L4/AMSS reads VIC_IRQ_VEC_RD alone and takes NO_PEND_VAL (reset value
 * 0xFFFFFFFF) to mean "nothing pending"; set the "l4" property for that.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "htcleo.h"

#define VIC_NUM_IRQS 64

OBJECT_DECLARE_SIMPLE_TYPE(MsmVicState, MSM_VIC)

struct MsmVicState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    qemu_irq fiq;

    uint32_t select[2];
    uint32_t enable[2];
    uint32_t type[2];       /* 1 = edge */
    uint32_t polarity[2];   /* 1 = active low / falling */
    uint32_t level[2];      /* raw input line levels */
    uint32_t latch[2];      /* latched edges */
    uint32_t soft[2];
    uint32_t masteren;
    uint32_t config;
    uint32_t no_pend_val;
    uint32_t vecprio[VIC_NUM_IRQS];
    uint32_t vecaddr[VIC_NUM_IRQS];
    uint32_t security[2];
    int32_t last_irq;
    bool l4;
    bool trace;             /* log the lines software enables */
    /* the last events of one line, kept quietly for "watch-log" */
    int32_t watch;
    struct { int64_t ns; char kind; uint32_t arg, pc; } wlog[256];
    unsigned wlog_n;                /* VEC_RD returns NO_PEND_VAL when idle */
};

static void vic_pending(MsmVicState *s, uint32_t pend[2])
{
    for (int b = 0; b < 2; b++) {
        uint32_t lvl = (s->level[b] ^ s->polarity[b]) & ~s->type[b];
        pend[b] = lvl | (s->latch[b] & s->type[b]) | s->soft[b];
    }
}

static void vic_update(MsmVicState *s)
{
    uint32_t pend[2];
    bool irq = false, fiq = false;

    vic_pending(s, pend);
    for (int b = 0; b < 2; b++) {
        uint32_t st = pend[b] & s->enable[b];
        if (st & ~s->select[b]) {
            irq = true;
        }
        if (st & s->select[b]) {
            fiq = true;
        }
    }
    qemu_set_irq(s->irq, irq && (s->masteren & 1));
    qemu_set_irq(s->fiq, fiq && (s->masteren & 2));
}

static int vic_first_pending(MsmVicState *s, bool want_fiq)
{
    uint32_t pend[2];

    vic_pending(s, pend);
    for (int b = 0; b < 2; b++) {
        uint32_t st = pend[b] & s->enable[b];
        st &= want_fiq ? s->select[b] : ~s->select[b];
        if (st) {
            return b * 32 + ctz32(st);
        }
    }
    return -1;
}

static void vic_wlog(MsmVicState *s, char kind, uint32_t arg)
{
    unsigned i = s->wlog_n++ % ARRAY_SIZE(s->wlog);

    s->wlog[i].ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    s->wlog[i].kind = kind;
    s->wlog[i].arg = arg;
    s->wlog[i].pc = current_cpu ?
        ((CPUARMState *)cpu_env(current_cpu))->regs[15] : 0;
}

static void vic_set_irq(void *opaque, int n, int level)
{
    MsmVicState *s = opaque;
    int b = n >> 5;
    uint32_t bit = 1u << (n & 31);
    uint32_t old = (s->level[b] ^ s->polarity[b]) & bit;

    if (level) {
        s->level[b] |= bit;
    } else {
        s->level[b] &= ~bit;
    }
    if (((s->level[b] ^ s->polarity[b]) & bit) && !old) {
        if (n == s->watch) {
            vic_wlog(s, 'E', !!(s->latch[b] & bit));  /* edge (was latched) */
        }
        s->latch[b] |= bit;
    }
    vic_update(s);
}

static uint64_t vic_read(void *opaque, hwaddr off, unsigned size)
{
    MsmVicState *s = opaque;
    uint32_t pend[2];
    int b = (off >> 2) & 1;
    int n;

    switch (off) {
    case 0x00: case 0x04: return s->select[b];
    case 0x10: case 0x14: return s->enable[b];
    case 0x40: case 0x44: return s->type[b];
    case 0x50: case 0x54: return s->polarity[b];
    case 0x60: return s->no_pend_val;
    case 0x68: return s->masteren;
    case 0x6C: return s->config;
    case 0x70: case 0x74: return s->security[b];
    case 0x80: case 0x84:
        vic_pending(s, pend);
        return pend[b] & s->enable[b] & ~s->select[b];
    case 0x90: case 0x94:
        vic_pending(s, pend);
        return pend[b] & s->enable[b] & s->select[b];
    case 0xA0: case 0xA4:
        vic_pending(s, pend);
        return pend[b];
    case 0xC0: case 0xC4: return s->soft[b];
    case 0xD0:              /* IRQ_VEC_RD */
        n = vic_first_pending(s, false);
        if (n >= 0 && n == s->watch) {
            vic_wlog(s, 'R', n);
        }
        if (n >= 0) {
            s->last_irq = n;
        } else if (s->l4) {
            return s->no_pend_val;
        }
        return s->last_irq;
    case 0xD4:              /* IRQ_VEC_PEND_RD */
        n = vic_first_pending(s, false);
        return n >= 0 ? n : 0xFFFFFFFF;
    case 0xDC:
    case 0xE0:
        n = vic_first_pending(s, true);
        return n >= 0 ? n : 0xFFFFFFFF;
    case 0xE8: case 0xEC: case 0xF0: case 0xF4: case 0xF8: case 0xFC:
        return 0;
    }
    if (off >= 0x200 && off < 0x300) {
        return s->vecprio[(off - 0x200) >> 2];
    }
    if (off >= 0x400 && off < 0x500) {
        return s->vecaddr[(off - 0x400) >> 2];
    }
    qemu_log_mask(LOG_UNIMP, "msm-vic: read 0x%03" HWADDR_PRIx "\n", off);
    return 0;
}

static void vic_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MsmVicState *s = opaque;
    int b = (off >> 2) & 1;

    switch (off) {
    case 0x00: case 0x04: s->select[b] = val; break;
    case 0x10: case 0x14: case 0x30: case 0x34:
        if (s->trace && (val & ~s->enable[b])) {
            uint32_t on = val & ~s->enable[b];

            for (int i = 0; i < 32; i++) {
                if (on & (1u << i)) {
                    qemu_log("msm-vic: line %d enabled\n", b * 32 + i);
                }
            }
        }
        if (s->watch >= 0 && (s->watch >> 5) == b) {
            uint32_t wb = 1u << (s->watch & 31);

            /* writes of this line alone (not the kernel's mask-all) */
            if (val == wb) {
                vic_wlog(s, 'N', !!(s->latch[b] & wb));  /* enable */
            }
        }
        if (off & 0x20) {
            s->enable[b] |= val;
        } else {
            s->enable[b] = val;
        }
        break;
    case 0x20: case 0x24:
        if (s->watch >= 0 && (s->watch >> 5) == b &&
            val == (1u << (s->watch & 31))) {
            vic_wlog(s, 'D', !!(s->latch[b] & (1u << (s->watch & 31))));
        }
        s->enable[b] &= ~val;
        break;
    case 0x40: case 0x44: s->type[b] = val; break;
    case 0x50: case 0x54: s->polarity[b] = val; break;
    case 0x60: s->no_pend_val = val; break;
    case 0x64: break;
    case 0x68: s->masteren = val; break;
    case 0x6C: s->config = val; break;
    case 0x70: case 0x74: s->security[b] = val; break;
    case 0xB0: case 0xB4:   /* INT_CLEAR */
        if (s->watch >= 0 && (s->watch >> 5) == b &&
            (val & (1u << (s->watch & 31)))) {
            vic_wlog(s, 'C', !!(s->latch[b] & (1u << (s->watch & 31))));
        }
        s->latch[b] &= ~val;
        s->soft[b] &= ~val;
        break;
    case 0xC0: case 0xC4: s->soft[b] |= val; break;
    case 0xD8: case 0xE4:               /* VEC_WR: end of interrupt */
        if (s->watch >= 0 && s->last_irq == s->watch) {
            vic_wlog(s, 'W', val);
        }
        break;
    case 0xE8: case 0xEC: case 0xF0: case 0xF4: case 0xF8: case 0xFC:
        break;
    default:
        if (off >= 0x200 && off < 0x300) {
            s->vecprio[(off - 0x200) >> 2] = val;
        } else if (off >= 0x400 && off < 0x500) {
            s->vecaddr[(off - 0x400) >> 2] = val;
        } else {
            qemu_log_mask(LOG_UNIMP, "msm-vic: write 0x%03" HWADDR_PRIx
                          " = 0x%" PRIx64 "\n", off, val);
        }
    }
    vic_update(s);
}

static const MemoryRegionOps vic_ops = {
    .read = vic_read,
    .write = vic_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void vic_reset(DeviceState *dev)
{
    MsmVicState *s = MSM_VIC(dev);

    memset(s->select, 0, sizeof(s->select));
    memset(s->enable, 0, sizeof(s->enable));
    memset(s->type, 0, sizeof(s->type));
    memset(s->polarity, 0, sizeof(s->polarity));
    memset(s->latch, 0, sizeof(s->latch));
    memset(s->soft, 0, sizeof(s->soft));
    s->masteren = s->l4 ? 3 : 0;
    s->no_pend_val = s->l4 ? 0xFFFFFFFF : 0;
    s->last_irq = 0;
    vic_update(s);
}

/* qom-get .../watch-log: the watched line's recent events, oldest first */
static char *vic_get_wlog(Object *obj, Error **errp)
{
    MsmVicState *s = MSM_VIC(obj);
    GString *g = g_string_new(NULL);
    unsigned n = MIN(s->wlog_n, ARRAY_SIZE(s->wlog));
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    for (unsigned k = s->wlog_n - n; k != s->wlog_n; k++) {
        unsigned i = k % ARRAY_SIZE(s->wlog);

        g_string_append_printf(g, "%c%u@-%.6f/%x ", s->wlog[i].kind,
                               s->wlog[i].arg,
                               (now - s->wlog[i].ns) / 1e9, s->wlog[i].pc);
    }
    g_string_append_printf(g, "(%u events)", s->wlog_n);
    return g_string_free(g, false);
}

static void vic_init(Object *obj)
{
    MsmVicState *s = MSM_VIC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &vic_ops, s, "msm-vic", 0x1000);
    object_property_add_str(obj, "watch-log", vic_get_wlog, NULL);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    sysbus_init_irq(sbd, &s->fiq);
    qdev_init_gpio_in(DEVICE(obj), vic_set_irq, VIC_NUM_IRQS);
}

static const Property vic_props[] = {
    DEFINE_PROP_BOOL("l4", MsmVicState, l4, false),
    DEFINE_PROP_BOOL("trace", MsmVicState, trace, false),
    DEFINE_PROP_INT32("watch", MsmVicState, watch, -1),
};

static void vic_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    device_class_set_legacy_reset(dc, vic_reset);
    device_class_set_props(dc, vic_props);
}

/* ------------------------------------------------------------------ */
/* SIRC                                                                */
/* ------------------------------------------------------------------ */

OBJECT_DECLARE_SIMPLE_TYPE(MsmSircState, MSM_SIRC)

struct MsmSircState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;       /* -> VIC INT_SIRC_0 */
    qemu_irq fiq;       /* -> VIC INT_SIRC_1 */

    uint32_t select, enable, type, polarity, security;
    uint32_t level, latch, soft;
};

#define SIRC_MASK 0x007FFFFF

static uint32_t sirc_pending(MsmSircState *s)
{
    uint32_t lvl = (s->level ^ s->polarity) & ~s->type;
    return (lvl | (s->latch & s->type) | s->soft) & SIRC_MASK;
}

static void sirc_update(MsmSircState *s)
{
    uint32_t st = sirc_pending(s) & s->enable;

    qemu_set_irq(s->irq, !!(st & ~s->select));
    qemu_set_irq(s->fiq, !!(st & s->select));
}

static void sirc_set_irq(void *opaque, int n, int level)
{
    MsmSircState *s = opaque;
    uint32_t bit = 1u << n;
    uint32_t old = (s->level ^ s->polarity) & bit;

    if (level) {
        s->level |= bit;
    } else {
        s->level &= ~bit;
    }
    if (((s->level ^ s->polarity) & bit) && !old) {
        s->latch |= bit;
    }
    sirc_update(s);
}

static uint64_t sirc_read(void *opaque, hwaddr off, unsigned size)
{
    MsmSircState *s = opaque;

    switch (off) {
    case 0x00: return s->select;
    case 0x04: return s->enable;
    case 0x10: return s->type;
    case 0x14: return s->polarity;
    case 0x18: return s->security;
    case 0x1C: return sirc_pending(s) & s->enable & ~s->select;
    case 0x20: return sirc_pending(s) & s->enable & s->select;
    case 0x24: return sirc_pending(s);
    case 0x2C: return s->soft;
    }
    qemu_log_mask(LOG_UNIMP, "msm-sirc: read 0x%02" HWADDR_PRIx "\n", off);
    return 0;
}

static void sirc_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MsmSircState *s = opaque;

    switch (off) {
    case 0x00: s->select = val; break;
    case 0x04: s->enable = val; break;
    case 0x08: s->enable &= ~val; break;
    case 0x0C: s->enable |= val; break;
    case 0x10: s->type = val; break;
    case 0x14: s->polarity = val; break;
    case 0x18: s->security = val; break;
    case 0x28: s->latch &= ~val; s->soft &= ~val; break;
    case 0x2C: s->soft |= val; break;
    default:
        qemu_log_mask(LOG_UNIMP, "msm-sirc: write 0x%02" HWADDR_PRIx
                      " = 0x%" PRIx64 "\n", off, val);
    }
    sirc_update(s);
}

static const MemoryRegionOps sirc_ops = {
    .read = sirc_read,
    .write = sirc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void sirc_reset(DeviceState *dev)
{
    MsmSircState *s = MSM_SIRC(dev);

    s->select = s->enable = s->type = s->polarity = 0;
    s->latch = s->soft = 0;
    sirc_update(s);
}

static void sirc_init(Object *obj)
{
    MsmSircState *s = MSM_SIRC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &sirc_ops, s, "msm-sirc", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    sysbus_init_irq(sbd, &s->fiq);
    qdev_init_gpio_in(DEVICE(obj), sirc_set_irq, SIRC_NUM);
}

static void sirc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    device_class_set_legacy_reset(dc, sirc_reset);
}

static const TypeInfo msm_vic_types[] = {
    {
        .name = TYPE_MSM_VIC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MsmVicState),
        .instance_init = vic_init,
        .class_init = vic_class_init,
    }, {
        .name = TYPE_MSM_SIRC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MsmSircState),
        .instance_init = sirc_init,
        .class_init = sirc_class_init,
    },
};
DEFINE_TYPES(msm_vic_types)
