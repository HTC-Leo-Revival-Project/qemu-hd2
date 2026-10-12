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
 * The PMIC is a 256-byte register file with the behaviour AMSS needs
 * added in pm7540_read()/pm7540_write(); the other chips are plain
 * register files.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
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
};

#define PM_REV          0x02    /* model << 4 | revision */
#define PM7540_MODEL    0xA     /* AMSS's pm_model 10 */

static void pm7540_reset(uint8_t *pm)
{
    pm[PM_REV] = PM7540_MODEL << 4 | 2;
}

static uint8_t pm7540_read(uint8_t *pm, uint8_t a)
{
    return pm[a];
}

static void pm7540_write(uint8_t *pm, uint8_t a, uint8_t v)
{
    pm[a] = v;
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
        c->rd = chip == CHIP_PMIC ? pm7540_read(mem, a) : mem[a];
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
        pm7540_write(mem, a, val);
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
    pm7540_reset(s->chip[CHIP_PMIC]);
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
}

static const Property ssbi_props[] = {
    DEFINE_PROP_BOOL("trace", MsmSsbiState, trace, false),
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
