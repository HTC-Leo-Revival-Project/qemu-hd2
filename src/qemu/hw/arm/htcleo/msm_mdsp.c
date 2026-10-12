/*
 * QSD8250 modem DSP (mDSP, a QDSP4) as seen by the modem core, high level
 *
 * The mDSP does the baseband signal processing for GSM/WCDMA.  AMSS loads
 * its firmware into the DSP's memories and talks to it through words in
 * those memories; the DSP's own code is not run here.  What AMSS
 * 2.15.50.14 does (mdsptask.c, mdspfunc.c), reverse-engineered:
 *
 *   0xB1000000  memory A, 16-bit words (DSP word n at +2n)
 *   0xB1200000  memory B, 16-bit words
 *   0xB1400000  memory C, 32-bit words
 *   0xB1800000  program memory, 32-bit words
 *   0xB1C00000  control: 3 holds the DSP in reset, 2 lets it run
 *   0xB1C00008  doorbell: the modem core interrupts the DSP
 *
 * Bringing the DSP up, AMSS holds it in reset, downloads the image's
 * sections, writes 0x10000 to C+0x24 and 0xFFFF to A+0xF0, releases it and
 * 500 us later expects the "Master Controller" to have posted a nonzero
 * word at B+0x24.  The DSP interrupts the modem core on VIC line 27
 * (tramp 41).
 *
 * Master controller commands: AMSS writes a command code to B+0xB4 and
 * rings the doorbell; the MC clears B+0xB4 when done and interrupts back.
 * An asynchronous command also carries a value in C+0x470 that the MC
 * acknowledges by copying it to C+0x46C.  Here every command completes
 * at once.
 *
 * Image 0x1406 (radio 2.15.50.14) symbols the DSP itself maintains:
 *   B+0x33A4  GSM firmware ready; GSM L1 polls it after enabling the app
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/bswap.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "htcleo.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmMdspState, MSM_MDSP)

#define MDSP_SIZE       0x00C01000
#define MEM_A           0x000000
#define MEM_B           0x200000
#define MEM_C           0x400000
#define MEM_P           0x800000
#define MDSP_CTL        0xC00000

#define CTL_RESET       3
#define CTL_RUN         2

#define B_MC_STATUS     (MEM_B + 0x24)
#define B_MC_CMD        (MEM_B + 0xB4)
#define C_MC_ACK        (MEM_C + 0x46C)
#define C_MC_ARG        (MEM_C + 0x470)
#define IMG_1406        0x1406
#define B_GSM_READY     (MEM_B + 0x33A4)
#define CTL_DOORBELL    0x08

struct MsmMdspState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint8_t *mem;
    uint32_t ctl;
    uint32_t ctl_regs[0x400];
};

static void mdsp_boot(MsmMdspState *s)
{
    /* the master controller is up */
    stw_le_p(s->mem + B_MC_STATUS, 1);
    qemu_log_mask(LOG_UNIMP, "msm-mdsp: running image %04x\n",
                  lduw_le_p(s->mem + MEM_A));
}

static uint64_t mdsp_read(void *opaque, hwaddr off, unsigned size)
{
    MsmMdspState *s = opaque;

    if (off == MDSP_CTL) {
        return s->ctl;
    }
    if (off > MDSP_CTL) {
        return s->ctl_regs[((off - MDSP_CTL) >> 2) & 0x3FF];
    }
    return ldn_le_p(s->mem + off, size);
}

static void mdsp_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MsmMdspState *s = opaque;

    if (off == MDSP_CTL + CTL_DOORBELL) {
        s->ctl_regs[CTL_DOORBELL >> 2] = val;
        if (s->ctl == CTL_RUN && lduw_le_p(s->mem + B_MC_CMD)) {
            qemu_log_mask(LOG_UNIMP, "msm-mdsp: MC command %04x\n",
                          lduw_le_p(s->mem + B_MC_CMD));
            stl_le_p(s->mem + C_MC_ACK, ldl_le_p(s->mem + C_MC_ARG));
            stw_le_p(s->mem + B_MC_CMD, 0);
            if (lduw_le_p(s->mem + MEM_A) == IMG_1406) {
                stw_le_p(s->mem + B_GSM_READY, 1);
            }
        }
        qemu_irq_pulse(s->irq);
        return;
    }
    if (off > MDSP_CTL) {
        s->ctl_regs[((off - MDSP_CTL) >> 2) & 0x3FF] = val;
        return;
    }
    if (off == MDSP_CTL) {
        uint32_t old = s->ctl;

        s->ctl = val;
        if (old == CTL_RESET && val == CTL_RUN) {
            mdsp_boot(s);
        }
        return;
    }
    stn_le_p(s->mem + off, size, val);
}

static const MemoryRegionOps mdsp_ops = {
    .read = mdsp_read,
    .write = mdsp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void mdsp_reset(DeviceState *dev)
{
    MsmMdspState *s = MSM_MDSP(dev);

    memset(s->mem, 0, MDSP_CTL);
    memset(s->ctl_regs, 0, sizeof(s->ctl_regs));
    s->ctl = 0;
}

static void mdsp_init(Object *obj)
{
    MsmMdspState *s = MSM_MDSP(obj);

    s->mem = g_malloc0(MDSP_CTL);
    memory_region_init_io(&s->iomem, obj, &mdsp_ops, s, "msm-mdsp",
                          MDSP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void mdsp_class_init(ObjectClass *oc, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(oc), mdsp_reset);
}

static const TypeInfo msm_mdsp_info = {
    .name = TYPE_MSM_MDSP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmMdspState),
    .instance_init = mdsp_init,
    .class_init = mdsp_class_init,
};

static void msm_mdsp_register(void)
{
    type_register_static(&msm_mdsp_info);
}
type_init(msm_mdsp_register)
