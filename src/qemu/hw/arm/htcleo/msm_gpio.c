/*
 * Qualcomm QSD8x50 GPIO shadow registers (GPIO1 at 0xA9000000, GPIO2 at
 * 0xA9100000).
 *
 * Layout from arch/arm/mach-msm/gpio_hw.h (CONFIG_ARCH_QSD8X50): the banks
 * live at GPIO1+0x800 (banks 0,2..7) and GPIO2+0xC00 (bank 1).  Banks with
 * an enabled pending interrupt raise INT_GPIO_GROUP1 (GPIO1) or
 * INT_GPIO_GROUP2 (GPIO2), both routed through the SIRC.
 *
 * The modem core has its own shadow of the same registers: GPIO1's banks
 * at 0xA8E00000 + 0x000 and bank 1 at 0xA8F00000 + 0x400 (MMIO regions 2
 * and 3 here, seen through AMSS's GPIO interrupt table).  Both shadows
 * drive the same pins (out/oe/in are shared); interrupt enable, trigger
 * and pending state are per processor, and the modem's group interrupts
 * are outputs 2 and 3.
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

#define PROC_APPS   0
#define PROC_MODEM  1

typedef struct GpioIrqState {
    uint32_t edge[NUM_BANKS], pos[NUM_BANKS], en[NUM_BANKS];
    uint32_t latch[NUM_BANKS];
    uint32_t last_in[NUM_BANKS];
} GpioIrqState;

struct MsmGpioState {
    SysBusDevice parent_obj;
    MemoryRegion iomem[4];  /* apps GPIO1, GPIO2, modem GPIO1, GPIO2 */
    qemu_irq irq[4];        /* apps group1, group2, modem group1, group2 */

    uint32_t out[NUM_BANKS], oe[NUM_BANKS];
    uint32_t ext[NUM_BANKS];        /* externally driven input levels */
    GpioIrqState pi[2];             /* per processor */
    uint32_t other[2][0x400];       /* TLMM/config words: readback only */

    MsmGpioOutputNotify notify;
    void *notify_opaque;
};

static uint32_t bank_in(MsmGpioState *s, int b)
{
    return (s->out[b] & s->oe[b]) | (s->ext[b] & ~s->oe[b]);
}

static uint32_t bank_status(MsmGpioState *s, GpioIrqState *p, int b)
{
    uint32_t in = bank_in(s, b);
    uint32_t lvl = ~(in ^ p->pos[b]) & ~p->edge[b];   /* level active */
    return (p->latch[b] & p->edge[b]) | lvl;
}

static void gpio_update(MsmGpioState *s)
{
    for (int proc = 0; proc < 2; proc++) {
        GpioIrqState *p = &s->pi[proc];
        bool grp[2] = { false, false };

        for (int b = 0; b < NUM_BANKS; b++) {
            uint32_t in = bank_in(s, b);
            uint32_t chg = in ^ p->last_in[b];
            uint32_t rise = chg & in, fall = chg & ~in;
            /* edge interrupts: pos=1 rising, pos=0 falling */
            p->latch[b] |= ((rise & p->pos[b]) | (fall & ~p->pos[b])) &
                           p->edge[b];
            p->last_in[b] = in;
            if (bank_status(s, p, b) & p->en[b]) {
                grp[banks[b].block] = true;
            }
        }
        qemu_set_irq(s->irq[2 * proc], grp[0]);
        qemu_set_irq(s->irq[2 * proc + 1], grp[1]);
    }
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

/* bank registers of `proc` start at this offset in their MMIO window */
static hwaddr bank_base(int proc, int block)
{
    return proc == PROC_APPS ? (block ? 0xC00 : 0x800) : 0;
}

static uint64_t gpio_read(MsmGpioState *s, int proc, int block, hwaddr off)
{
    GpioIrqState *p = &s->pi[proc];
    hwaddr base = bank_base(proc, block);

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
                return p->edge[b];
            } else if (r == banks[b].pos) {
                return p->pos[b];
            } else if (r == banks[b].en) {
                return p->en[b];
            } else if (r == banks[b].status) {
                return bank_status(s, p, b);
            } else if (r == banks[b].clr) {
                return 0;
            }
        }
    }
    return proc == PROC_APPS ? s->other[block][(off >> 2) & 0x3ff] : 0;
}

static void gpio_write(MsmGpioState *s, int proc, int block, hwaddr off,
                       uint32_t val)
{
    GpioIrqState *p = &s->pi[proc];
    hwaddr base = bank_base(proc, block);

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
                p->edge[b] = val;
            } else if (r == banks[b].pos) {
                p->pos[b] = val;
            } else if (r == banks[b].en) {
                p->en[b] = val;
            } else if (r == banks[b].clr) {
                p->latch[b] &= ~val;
            } else {
                continue;
            }
            gpio_update(s);
            return;
        }
    }
    if (proc == PROC_APPS) {
        s->other[block][(off >> 2) & 0x3ff] = val;
    }
}

#define GPIO_OPS(name, proc, block)                                     \
static uint64_t name##_read(void *opaque, hwaddr off, unsigned size)      \
{                                                                         \
    return gpio_read(opaque, proc, block, off);                           \
}                                                                         \
static void name##_write(void *opaque, hwaddr off, uint64_t val,          \
                         unsigned size)                                   \
{                                                                         \
    gpio_write(opaque, proc, block, off, val);                            \
}                                                                         \
static const MemoryRegionOps name##_ops = {                               \
    .read = name##_read,                                                  \
    .write = name##_write,                                                \
    .endianness = DEVICE_LITTLE_ENDIAN,                                   \
    .valid = { .min_access_size = 4, .max_access_size = 4 },              \
};

GPIO_OPS(gpio1, PROC_APPS, 0)
GPIO_OPS(gpio2, PROC_APPS, 1)
GPIO_OPS(mgpio1, PROC_MODEM, 0)
GPIO_OPS(mgpio2, PROC_MODEM, 1)

static void gpio_reset(DeviceState *dev)
{
    MsmGpioState *s = MSM_GPIO(dev);

    for (int b = 0; b < NUM_BANKS; b++) {
        s->out[b] = s->oe[b] = 0;
    }
    memset(s->pi, 0, sizeof(s->pi));
    for (int proc = 0; proc < 2; proc++) {
        for (int b = 0; b < NUM_BANKS; b++) {
            s->pi[proc].last_in[b] = bank_in(s, b);
        }
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
    memory_region_init_io(&s->iomem[2], obj, &mgpio1_ops, s, "msm-gpio1-modem",
                          0x110);
    memory_region_init_io(&s->iomem[3], obj, &mgpio2_ops, s, "msm-gpio2-modem",
                          0x80);
    for (int i = 0; i < 4; i++) {
        sysbus_init_mmio(sbd, &s->iomem[i]);
        sysbus_init_irq(sbd, &s->irq[i]);
    }
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
