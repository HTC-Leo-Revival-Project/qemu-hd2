/*
 * Generic memory-backed register block for MSM peripherals whose only job
 * here is to hold configuration (clock controller, SCPLL, TCSR, TLMM...).
 *
 * Each word reads back what was last written.  The board can preset
 * reset values and force bits to read as 1 or 0 (for example PLL lock /
 * busy flags the drivers poll).
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "htcleo.h"
#include "msm_regfile.h"

static uint64_t regfile_read(void *opaque, hwaddr off, unsigned size)
{
    MsmRegfileState *s = opaque;
    uint32_t idx = off >> 2;
    uint32_t v;

    if (idx >= s->nwords) {
        return 0;
    }
    v = (s->regs[idx] | s->force_set[idx]) & ~s->force_clr[idx];
    if (s->read_hook) {
        v = s->read_hook(s->hook_opaque, off & ~3, v);
    }
    v >>= (off & 3) * 8;
    if (size < 4) {
        v &= (1u << (size * 8)) - 1;
    }
    return v;
}

static void regfile_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MsmRegfileState *s = opaque;
    uint32_t idx = off >> 2;
    uint32_t shift = (off & 3) * 8;
    uint32_t mask = size == 4 ? 0xFFFFFFFF : ((1u << (size * 8)) - 1) << shift;

    if (idx >= s->nwords) {
        return;
    }
    s->regs[idx] = (s->regs[idx] & ~mask) | (((uint32_t)val << shift) & mask);
    if (s->write_hook) {
        s->write_hook(s->hook_opaque, off & ~3, s->regs[idx]);
    }
}

static const MemoryRegionOps regfile_ops = {
    .read = regfile_read,
    .write = regfile_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

void msm_regfile_preset(MsmRegfileState *s, hwaddr off, uint32_t val)
{
    s->reset_val[off >> 2] = val;
    s->regs[off >> 2] = val;
}

void msm_regfile_force(MsmRegfileState *s, hwaddr off, uint32_t set,
                       uint32_t clr)
{
    s->force_set[off >> 2] = set;
    s->force_clr[off >> 2] = clr;
}

uint32_t msm_regfile_get(MsmRegfileState *s, hwaddr off)
{
    return s->regs[off >> 2];
}

static void regfile_reset(DeviceState *dev)
{
    MsmRegfileState *s = MSM_REGFILE(dev);
    memcpy(s->regs, s->reset_val, s->nwords * 4);
}

static void regfile_realize(DeviceState *dev, Error **errp)
{
    MsmRegfileState *s = MSM_REGFILE(dev);

    s->nwords = s->size / 4;
    s->regs = g_new0(uint32_t, s->nwords);
    s->reset_val = g_new0(uint32_t, s->nwords);
    s->force_set = g_new0(uint32_t, s->nwords);
    s->force_clr = g_new0(uint32_t, s->nwords);
    memory_region_init_io(&s->iomem, OBJECT(s), &regfile_ops, s,
                          s->name ? s->name : "msm-regfile", s->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const Property regfile_props[] = {
    DEFINE_PROP_UINT32("size", MsmRegfileState, size, 0x1000),
    DEFINE_PROP_STRING("name", MsmRegfileState, name),
};

static void regfile_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = regfile_realize;
    device_class_set_legacy_reset(dc, regfile_reset);
    device_class_set_props(dc, regfile_props);
}

static const TypeInfo msm_regfile_info = {
    .name = TYPE_MSM_REGFILE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmRegfileState),
    .class_init = regfile_class_init,
};

static void msm_regfile_register(void)
{
    type_register_static(&msm_regfile_info);
}
type_init(msm_regfile_register)
