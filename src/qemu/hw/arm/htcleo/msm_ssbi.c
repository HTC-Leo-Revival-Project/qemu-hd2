/*
 * QSD8250 SSBI 1.0 controllers and the PM7540 PMIC behind them
 *
 * The modem core reaches the PMIC and other chips through single-wire
 * serial bus (SSBI) controllers.  AMSS 2.15.50.14 picks one with a bus word
 * (bit 4 selects the second bank, bits 3:0 the controller) and drives it:
 *   0x00 CTL        mode bits (0x8001, 0x8005 ...)
 *   0x04 RESET      1 resets the controller
 *   0x08 CMD        write: addr << 16 | data;  read: 1 << 24 | addr << 16
 *   0x10 RD         data of the last read in bits 7:0
 *   0x14 STATUS     bit 0 busy, bit 1 ready for a command, bit 2 read ready
 *   0x18 PRIORITIES
 * Transfers complete at once, so a controller is always ready.
 *
 * Controllers (MMIO region n of this device, 0x20 bytes each):
 *   0  0xB8000300  bus 0x00: the PM7540 (most PMIC traffic)
 *   1  0xA8100000  bus 0x01
 *   2  0xB8000400  bus 0x02
 *   3  0xB0007580  bus 0x10: the PM7540 too (AMSS reads its ID here)
 *   4  0xB00075A0  bus 0x11
 *   5  0xB00075C0  bus 0x12
 * The other chips are plain register files.  The PM7540 is a register
 * file with these parts modelled (register map from AMSS's PMIC driver):
 *   0x02         model << 4 | revision (PM7500 family: AMSS model 4)
 *   0x25         RTC control: bit 7 runs the clock; a 1 written to bit 6
 *                moves the count, bit 5 = 1 from the RTC into 0x26-0x29
 *                (latch for reading), 0 from them into the RTC (set); bit 3
 *                picks the alarm instead of the time
 *   0x26-0x29    seconds since 1980-01-06 (GPS epoch), 0x26 most significant
 *   interrupts   eleven groups of eight: latched status 0x31-0x37 and
 *                0xB1-0xB4 (a source's rising edge sets it, writing 1
 *                clears it), mask 0x38-0x3E and 0xB8-0xBB (1 = masked, all
 *                masked at reset), real-time status 0x3F-0x44 and
 *                0xBF-0xC2 (groups 5 and 6 share 0x44).  The ISR reads
 *                the summary first: 0x30 bit g+1 for group g with an
 *                unmasked latched interrupt, bit 0 if a paged group has
 *                one, and then 0xB0 bit k+1 for paged group k.  The PMIC's interrupt output (active low,
 *                MSM GPIO 24 to the modem) is asserted while an unmasked
 *                latched bit is set.
 * Sources (AMSS IRQ ids): 2 USB/charger power valid (0x31 bit 5), 3 its
 * absence (bit 4), 12 power key pressed (0x33 bit 3), 13 released (bit 2).
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "qemu/timer.h"
#include "qemu/cutils.h"
#include "system/rtc.h"
#include "htcleo.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmSsbiState, MSM_SSBI)

#define SSBI_CTL        0x00
#define SSBI_RESET      0x04
#define SSBI_CMD        0x08
#define SSBI_RD         0x10
#define SSBI_STATUS     0x14
#define SSBI_SIZE       0x20

#define CMD_READ        (1u << 24)
#define STATUS_READY    (1u << 1)
#define STATUS_RD_READY (1u << 2)

#define CHIP_PMIC       0
#define NUM_CHIPS       5

/* which chip each controller talks to */
static const int ssbi_chip[MSM_SSBI_NUM] = { CHIP_PMIC, 1, 2, CHIP_PMIC, 3, 4 };

typedef struct SsbiCtl {
    MsmSsbiState *s;
    int n;
    MemoryRegion iomem;
    uint32_t regs[SSBI_SIZE / 4];
    uint32_t rd;
} SsbiCtl;

struct MsmSsbiState {
    SysBusDevice parent_obj;
    SsbiCtl ctl[MSM_SSBI_NUM];
    bool trace;
    uint8_t chip[NUM_CHIPS][256];   /* chip 0 is the PM7540 */

    /* PM7540 */
    qemu_irq pmic_irq;
    uint8_t pm_rt[11], pm_latch[11];
    uint32_t rtc_base;              /* RTC seconds at rtc_base_ns */
    int64_t rtc_base_ns;
    uint32_t rtc_alarm;
    bool vbus;
};

#define PM_REV          0x02    /* model << 4 | revision */
#define PM7540_MODEL    0x3     /* AMSS pm_model 4, the PM7500 family */
#define PM_RTC_CTL      0x25
#define PM_RTC_DATA     0x26    /* 0x26-0x29, most significant first */
#define RTC_RUN         0x80
#define RTC_XFER        0x40
#define RTC_TO_REGS     0x20
#define RTC_ALARM       0x08
#define GPS_EPOCH       315964800   /* 1980-01-06 in Unix time */

#define PM_GROUPS       11
static const uint8_t pm_status_reg[PM_GROUPS] = {
    0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0xB1, 0xB2, 0xB3, 0xB4 };
static const uint8_t pm_mask_reg[PM_GROUPS] = {
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0xB8, 0xB9, 0xBA, 0xBB };
static const uint8_t pm_rt_reg[PM_GROUPS] = {
    0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x44, 0xBF, 0xC0, 0xC1, 0xC2 };

/* interrupt sources: group, bit */
#define PM_SRC(g, b)    ((g) << 8 | (b))
#define SRC_VALID_CHG   PM_SRC(0, 0x20)     /* AMSS irq 2 */
#define SRC_INVALID_CHG PM_SRC(0, 0x10)     /* irq 3 */
#define SRC_PWR_KEY_ON  PM_SRC(2, 0x08)     /* irq 12 */
#define SRC_PWR_KEY_OFF PM_SRC(2, 0x04)     /* irq 13 */

static int pm_group_of(const uint8_t *tbl, uint8_t a)
{
    for (int g = 0; g < PM_GROUPS; g++) {
        if (tbl[g] == a) {
            return g;
        }
    }
    return -1;
}

static void pm7540_update_irq(MsmSsbiState *s)
{
    bool on = false;

    for (int g = 0; g < PM_GROUPS; g++) {
        if (s->pm_latch[g] & ~s->chip[CHIP_PMIC][pm_mask_reg[g]]) {
            on = true;
        }
    }
    qemu_set_irq(s->pmic_irq, !on);     /* active low */
}

static void pm7540_set_src(MsmSsbiState *s, int src, bool level)
{
    int g = src >> 8;
    uint8_t bit = src & 0xff;

    if (level && !(s->pm_rt[g] & bit)) {
        s->pm_latch[g] |= bit;
    }
    s->pm_rt[g] = level ? s->pm_rt[g] | bit : s->pm_rt[g] & ~bit;
    pm7540_update_irq(s);
}

void msm_ssbi_pmic_set_vbus(DeviceState *dev, bool present)
{
    MsmSsbiState *s = MSM_SSBI(dev);

    s->vbus = present;
    pm7540_set_src(s, SRC_VALID_CHG, present);
    pm7540_set_src(s, SRC_INVALID_CHG, !present);
}

void msm_ssbi_pmic_set_power_key(DeviceState *dev, bool pressed)
{
    MsmSsbiState *s = MSM_SSBI(dev);

    pm7540_set_src(s, SRC_PWR_KEY_ON, pressed);
    pm7540_set_src(s, SRC_PWR_KEY_OFF, !pressed);
}

/* RTC seconds now */
static uint32_t pm_rtc_now(MsmSsbiState *s)
{
    int64_t ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->rtc_base_ns;

    return s->rtc_base + (uint32_t)(ns / NANOSECONDS_PER_SECOND);
}

static void pm7540_reset(MsmSsbiState *s)
{
    uint8_t *pm = s->chip[CHIP_PMIC];
    struct tm tm;

    pm[PM_REV] = PM7540_MODEL << 4 | 2;
    /* the coin cell kept the clock running: start from QEMU's RTC time */
    qemu_get_timedate(&tm, 0);
    s->rtc_base = mktimegm(&tm) - GPS_EPOCH;
    s->rtc_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->rtc_alarm = 0;
    pm[PM_RTC_CTL] = RTC_RUN;
    for (int g = 0; g < PM_GROUPS; g++) {
        pm[pm_mask_reg[g]] = 0xFF;
    }
    memset(s->pm_rt, 0, sizeof(s->pm_rt));
    memset(s->pm_latch, 0, sizeof(s->pm_latch));
    s->pm_rt[SRC_PWR_KEY_OFF >> 8] |= SRC_PWR_KEY_OFF & 0xff;
    if (s->vbus) {
        s->pm_rt[SRC_VALID_CHG >> 8] |= SRC_VALID_CHG & 0xff;
    } else {
        s->pm_rt[SRC_INVALID_CHG >> 8] |= SRC_INVALID_CHG & 0xff;
    }
    pm7540_update_irq(s);
}

static uint8_t pm7540_read(MsmSsbiState *s, uint8_t a)
{
    uint8_t *pm = s->chip[CHIP_PMIC];
    int g = pm_group_of(pm_status_reg, a);
    uint8_t v = 0;

    if (g >= 0) {
        return s->pm_latch[g];
    }
    if (a == 0x30 || a == 0xB0) {
        for (g = 0; g < PM_GROUPS; g++) {
            if (!(s->pm_latch[g] & ~pm[pm_mask_reg[g]])) {
                continue;
            }
            if (g < 7) {
                v |= a == 0x30 ? 2 << g : 0;
            } else {
                v |= a == 0x30 ? 1 : 2 << (g - 7);
            }
        }
        return v;
    }
    if (pm_group_of(pm_rt_reg, a) >= 0) {
        for (g = 0; g < PM_GROUPS; g++) {
            if (pm_rt_reg[g] == a) {
                v |= s->pm_rt[g];
            }
        }
        return v;
    }
    return pm[a];
}

static void pm7540_write(MsmSsbiState *s, uint8_t a, uint8_t v)
{
    uint8_t *pm = s->chip[CHIP_PMIC];
    uint8_t old = pm[a];
    int g;

    if ((g = pm_group_of(pm_status_reg, a)) >= 0) {
        s->pm_latch[g] &= ~v;           /* write 1 to clear */
        pm7540_update_irq(s);
        return;
    }
    pm[a] = v;
    if (pm_group_of(pm_mask_reg, a) >= 0) {
        pm7540_update_irq(s);
    } else if (a == PM_RTC_CTL && (v & RTC_XFER) && !(old & RTC_XFER)) {
        uint32_t *t = (v & RTC_ALARM) ? &s->rtc_alarm : NULL;
        uint32_t now;

        if (v & RTC_TO_REGS) {
            now = t ? *t : pm_rtc_now(s);
            stl_be_p(&pm[PM_RTC_DATA], now);
        } else if (t) {
            *t = ldl_be_p(&pm[PM_RTC_DATA]);
        } else {
            s->rtc_base = ldl_be_p(&pm[PM_RTC_DATA]);
            s->rtc_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }
    }
}

static uint64_t ssbi_read(void *opaque, hwaddr off, unsigned size)
{
    SsbiCtl *c = opaque;

    switch (off) {
    case SSBI_RD:
        return c->rd;
    case SSBI_STATUS:
        return STATUS_READY | STATUS_RD_READY;
    }
    return c->regs[(off / 4) % (SSBI_SIZE / 4)];
}

static void ssbi_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    SsbiCtl *c = opaque;
    MsmSsbiState *s = c->s;
    int chip = ssbi_chip[c->n];
    uint8_t *mem = s->chip[chip];
    uint8_t a = val >> 16;

    if (off != SSBI_CMD) {
        c->regs[(off / 4) % (SSBI_SIZE / 4)] = val;
        return;
    }
    if (val & CMD_READ) {
        c->rd = chip == CHIP_PMIC ? pm7540_read(s, a) : mem[a];
        if (s->trace) {
            qemu_log("ssbi%d chip%d: read  [%02x] -> %02x\n", c->n, chip, a,
                     c->rd);
        }
        return;
    }
    if (s->trace) {
        qemu_log("ssbi%d chip%d: write [%02x] <- %02x\n", c->n, chip, a,
                 (unsigned)(val & 0xff));
    }
    if (chip == CHIP_PMIC) {
        pm7540_write(s, a, val);
    } else {
        mem[a] = val;
    }
}

static const MemoryRegionOps ssbi_ops = {
    .read = ssbi_read,
    .write = ssbi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void ssbi_reset(DeviceState *dev)
{
    MsmSsbiState *s = MSM_SSBI(dev);

    for (int i = 0; i < MSM_SSBI_NUM; i++) {
        memset(s->ctl[i].regs, 0, sizeof(s->ctl[i].regs));
        s->ctl[i].rd = 0;
    }
    memset(s->chip, 0, sizeof(s->chip));
    pm7540_reset(s);
}

static void ssbi_init(Object *obj)
{
    MsmSsbiState *s = MSM_SSBI(obj);

    for (int i = 0; i < MSM_SSBI_NUM; i++) {
        SsbiCtl *c = &s->ctl[i];
        c->s = s;
        c->n = i;
        memory_region_init_io(&c->iomem, obj, &ssbi_ops, c, "msm-ssbi",
                              SSBI_SIZE);
        sysbus_init_mmio(SYS_BUS_DEVICE(obj), &c->iomem);
    }
    qdev_init_gpio_out_named(DEVICE(obj), &s->pmic_irq, "pmic-irq", 1);
    s->vbus = true;
}

static const Property ssbi_props[] = {
    DEFINE_PROP_BOOL("trace", MsmSsbiState, trace, false),
    DEFINE_PROP_BOOL("vbus", MsmSsbiState, vbus, true),
};

static void ssbi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, ssbi_reset);
    device_class_set_props(dc, ssbi_props);
}

static const TypeInfo msm_ssbi_info = {
    .name = TYPE_MSM_SSBI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmSsbiState),
    .instance_init = ssbi_init,
    .class_init = ssbi_class_init,
};

static void msm_ssbi_register(void)
{
    type_register_static(&msm_ssbi_info);
}
type_init(msm_ssbi_register)
