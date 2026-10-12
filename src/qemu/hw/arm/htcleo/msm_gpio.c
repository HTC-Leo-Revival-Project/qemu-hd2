/*
 * Qualcomm QSD8x50 GPIO shadow registers (GPIO1 at 0xA9000000, GPIO2 at
 * 0xA9100000).
 *
 * Layout from arch/arm/mach-msm/gpio_hw.h (CONFIG_ARCH_QSD8X50): the banks
 * live at GPIO1+0x800 (banks 0,2..7) and GPIO2+0xC00 (bank 1).  Banks with
 * an enabled pending interrupt raise INT_GPIO_GROUP1 (GPIO1) or
 * INT_GPIO_GROUP2 (GPIO2), both routed through the SIRC.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "htcleo.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmGpioState, MSM_GPIO)

#define NUM_BANKS 8

typedef struct GpioBank {
    int first, last;        /* pin range */
    int block;              /* 0 = GPIO1, 1 = GPIO2 */
    hwaddr out, oe, in, edge, pos, en, clr, status;
} GpioBank;

static const GpioBank banks[NUM_BANKS] = {
    {   0,  15, 0, 0x00, 0x20, 0x50, 0x70, 0x90, 0xB0, 0xD0, 0xF0 },
    {  16,  42, 1, 0x00, 0x08, 0x20, 0x50, 0x58, 0x60, 0x68, 0x70 },
    {  43,  67, 0, 0x04, 0x24, 0x54, 0x74, 0x94, 0xB4, 0xD4, 0xF4 },
    {  68,  94, 0, 0x08, 0x28, 0x58, 0x78, 0x98, 0xB8, 0xD8, 0xF8 },
    {  95, 103, 0, 0x0C, 0x2C, 0x5C, 0x7C, 0x9C, 0xBC, 0xDC, 0xFC },
    { 104, 121, 0, 0x10, 0x30, 0x60, 0x80, 0xA0, 0xC0, 0xE0, 0x100 },
    { 122, 152, 0, 0x14, 0x34, 0x64, 0x84, 0xA4, 0xC4, 0xE4, 0x104 },
    { 153, 164, 0, 0x18, 0x38, 0x68, 0x88, 0xA8, 0xC8, 0xE8, 0x108 },
};

struct MsmGpioState {
    SysBusDevice parent_obj;
    MemoryRegion iomem[2];
    qemu_irq irq[2];        /* group1, group2 */

    uint32_t out[NUM_BANKS], oe[NUM_BANKS];
    uint32_t edge[NUM_BANKS], pos[NUM_BANKS], en[NUM_BANKS];
    uint32_t latch[NUM_BANKS];
    uint32_t ext[NUM_BANKS];        /* externally driven input levels */
    uint32_t last_in[NUM_BANKS];
    uint32_t other[2][0x400];       /* TLMM/config words: readback only */

    MsmGpioOutputNotify notify;
    void *notify_opaque;
};

static uint32_t bank_in(MsmGpioState *s, int b)
{
    return (s->out[b] & s->oe[b]) | (s->ext[b] & ~s->oe[b]);
}

static uint32_t bank_status(MsmGpioState *s, int b)
{
    uint32_t in = bank_in(s, b);
    uint32_t lvl = ~(in ^ s->pos[b]) & ~s->edge[b];   /* level active */
    return (s->latch[b] & s->edge[b]) | lvl;
}

static void gpio_update(MsmGpioState *s)
{
    bool grp[2] = { false, false };

    for (int b = 0; b < NUM_BANKS; b++) {
        uint32_t in = bank_in(s, b);
        uint32_t chg = in ^ s->last_in[b];
        uint32_t rise = chg & in, fall = chg & ~in;
        /* edge interrupts: pos=1 rising, pos=0 falling */
        s->latch[b] |= ((rise & s->pos[b]) | (fall & ~s->pos[b])) & s->edge[b];
        s->last_in[b] = in;
        if (bank_status(s, b) & s->en[b]) {
            grp[banks[b].block] = true;
        }
    }
    qemu_set_irq(s->irq[0], grp[0]);
    qemu_set_irq(s->irq[1], grp[1]);
}

static int pin_bank(int pin, int *bit)
{
    for (int b = 0; b < NUM_BANKS; b++) {
        if (pin >= banks[b].first && pin <= banks[b].last) {
            *bit = pin - banks[b].first;
            return b;
        }
    }
    return -1;
}

void msm_gpio_set_input(MsmGpioState *s, int pin, int level)
{
    int bit, b = pin_bank(pin, &bit);

    if (b < 0) {
        return;
    }
    if (level) {
        s->ext[b] |= 1u << bit;
    } else {
        s->ext[b] &= ~(1u << bit);
    }
    gpio_update(s);
}

int msm_gpio_get_output(MsmGpioState *s, int pin)
{
    int bit, b = pin_bank(pin, &bit);

    if (b < 0) {
        return -1;
    }
    if (!(s->oe[b] & (1u << bit))) {
        return -1;          /* not driven */
    }
    return (s->out[b] >> bit) & 1;
}

int msm_gpio_get_output_latch(MsmGpioState *s, int pin)
{
    int bit, b = pin_bank(pin, &bit);

    return b < 0 ? -1 : (s->out[b] >> bit) & 1;
}

void msm_gpio_set_output_notify(MsmGpioState *s, MsmGpioOutputNotify cb,
                                void *opaque)
{
    s->notify = cb;
    s->notify_opaque = opaque;
}

static void notify_changes(MsmGpioState *s, int b, uint32_t before)
{
    uint32_t after = bank_in(s, b);
    uint32_t chg = (before ^ after);

    if (!s->notify) {
        return;
    }
    /* Report every pin whose driven state changed, including OE flips */
    for (int i = 0; i <= banks[b].last - banks[b].first; i++) {
        if (chg & (1u << i)) {
            s->notify(s->notify_opaque, banks[b].first + i,
                      (after >> i) & 1);
        }
    }
    /* the board may also need to see OE changes on matrix outputs */
    s->notify(s->notify_opaque, -1, 0);
}

static uint64_t gpio_read(MsmGpioState *s, int block, hwaddr off)
{
    hwaddr base = block ? 0xC00 : 0x800;

    if (off >= base && off < base + 0x110) {
        hwaddr r = off - base;
        for (int b = 0; b < NUM_BANKS; b++) {
            if (banks[b].block != block) {
                continue;
            }
            if (r == banks[b].out) {
                return s->out[b];
            } else if (r == banks[b].oe) {
                return s->oe[b];
            } else if (r == banks[b].in) {
                return bank_in(s, b);
            } else if (r == banks[b].edge) {
                return s->edge[b];
            } else if (r == banks[b].pos) {
                return s->pos[b];
            } else if (r == banks[b].en) {
                return s->en[b];
            } else if (r == banks[b].status) {
                return bank_status(s, b);
            } else if (r == banks[b].clr) {
                return 0;
            }
        }
    }
    return s->other[block][(off >> 2) & 0x3ff];
}

static void gpio_write(MsmGpioState *s, int block, hwaddr off, uint32_t val)
{
    hwaddr base = block ? 0xC00 : 0x800;

    if (off >= base && off < base + 0x110) {
        hwaddr r = off - base;
        for (int b = 0; b < NUM_BANKS; b++) {
            uint32_t before;
            if (banks[b].block != block) {
                continue;
            }
            before = bank_in(s, b);
            if (r == banks[b].out) {
                s->out[b] = val;
                notify_changes(s, b, before);
            } else if (r == banks[b].oe) {
                s->oe[b] = val;
                notify_changes(s, b, before);
            } else if (r == banks[b].edge) {
                s->edge[b] = val;
            } else if (r == banks[b].pos) {
                s->pos[b] = val;
            } else if (r == banks[b].en) {
                s->en[b] = val;
            } else if (r == banks[b].clr) {
                s->latch[b] &= ~val;
            } else {
                continue;
            }
            gpio_update(s);
            return;
        }
    }
    s->other[block][(off >> 2) & 0x3ff] = val;
}

static uint64_t gpio1_read(void *opaque, hwaddr off, unsigned size)
{
    return gpio_read(opaque, 0, off);
}

static void gpio1_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    gpio_write(opaque, 0, off, val);
}

static uint64_t gpio2_read(void *opaque, hwaddr off, unsigned size)
{
    return gpio_read(opaque, 1, off);
}

static void gpio2_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    gpio_write(opaque, 1, off, val);
}

static const MemoryRegionOps gpio1_ops = {
    .read = gpio1_read,
    .write = gpio1_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static const MemoryRegionOps gpio2_ops = {
    .read = gpio2_read,
    .write = gpio2_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void gpio_reset(DeviceState *dev)
{
    MsmGpioState *s = MSM_GPIO(dev);

    for (int b = 0; b < NUM_BANKS; b++) {
        s->out[b] = s->oe[b] = s->edge[b] = s->pos[b] = s->en[b] = 0;
        s->latch[b] = 0;
        s->last_in[b] = bank_in(s, b);
    }
    gpio_update(s);
}

static void gpio_init(Object *obj)
{
    MsmGpioState *s = MSM_GPIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem[0], obj, &gpio1_ops, s, "msm-gpio1",
                          0x1000);
    memory_region_init_io(&s->iomem[1], obj, &gpio2_ops, s, "msm-gpio2",
                          0x1000);
    sysbus_init_mmio(sbd, &s->iomem[0]);
    sysbus_init_mmio(sbd, &s->iomem[1]);
    sysbus_init_irq(sbd, &s->irq[0]);
    sysbus_init_irq(sbd, &s->irq[1]);
    /* undriven inputs float high (pull-ups on the board) */
    for (int b = 0; b < NUM_BANKS; b++) {
        s->ext[b] = 0xFFFFFFFF;
    }
}

static void gpio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    device_class_set_legacy_reset(dc, gpio_reset);
}

static const TypeInfo msm_gpio_info = {
    .name = TYPE_MSM_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmGpioState),
    .instance_init = gpio_init,
    .class_init = gpio_class_init,
};

static void msm_gpio_register(void)
{
    type_register_static(&msm_gpio_info);
}
type_init(msm_gpio_register)
