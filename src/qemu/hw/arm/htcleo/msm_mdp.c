/*
 * Qualcomm MDP 3.1 display processor (0xAA200000) driving the HD2's
 * 480x800 LCDC panel.
 *
 * Register offsets from drivers/video/msm/mdp_hw.h (CONFIG_MSM_MDP31):
 *   0x20 INTR_ENABLE  0x24 INTR_STATUS  0x28 INTR_CLEAR
 *   0x30 DISPLAY0_START (PPP kick)  0x44 DMA_P_START  0x48 DMA_S_START
 *   0x90000 DMA_P_CONFIG  0x90004 SIZE  0x90008 IBUF_ADDR
 *   0x9000C IBUF_Y_STRIDE 0x90010 OUT_XY
 *   0xE0000 LCDC_EN ...
 *   interrupts: bit0 DL0_ROI_DONE (PPP), bit2 DMA_S_DONE,
 *               bit14 DMA_P_DONE, bit15 LCDC_FRAME_START
 *
 * While the LCDC is enabled the panel continuously scans the DMA_P input
 * buffer; a frame-start interrupt fires every vsync (60 Hz).  The PPP blit
 * engine is in msm_mdp_ppp.c.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/module.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "htcleo.h"
#include "msm_mdp.h"

#define MDP_SIZE            0xF0000

#define MDP_INTR_ENABLE     0x00020
#define MDP_INTR_STATUS     0x00024
#define MDP_INTR_CLEAR      0x00028
#define MDP_DISPLAY0_START  0x00030
#define MDP_DISPLAY1_START  0x00034
#define MDP_DMA_P_START     0x00044
#define MDP_DMA_S_START     0x00048
#define MDP_HW_VERSION      0x00070
#define MDP_DMA_P_CONFIG    0x90000
#define MDP_DMA_P_SIZE      0x90004
#define MDP_DMA_P_IBUF_ADDR 0x90008
#define MDP_DMA_P_IBUF_Y_STRIDE 0x9000C
#define MDP_DMA_P_OUT_XY    0x90010
#define MDP_LCDC_EN         0xE0000
#define MDP_PPP_SCALE_STATUS 0x50000
#define MDP_PPP_BLEND_STATUS 0x70000

#define INT_DL0_ROI_DONE    (1u << 0)
#define INT_DMA_S_DONE      (1u << 2)
#define INT_DMA_P_DONE      (1u << 14)
#define INT_LCDC_FRAME_START (1u << 15)

#define PANEL_W 480
#define PANEL_H 800

static void mdp_update_irq(MsmMdpState *s)
{
    qemu_set_irq(s->irq, !!(s->intr_status & s->intr_enable));
}

void msm_mdp_raise(MsmMdpState *s, uint32_t bits)
{
    s->intr_status |= bits;
    mdp_update_irq(s);
}

static void mdp_vsync(void *opaque)
{
    MsmMdpState *s = opaque;

    if (s->regs[MDP_LCDC_EN >> 2] & 1) {
        /* every frame the LCDC re-fetches the DMA_P buffer */
        msm_mdp_raise(s, INT_LCDC_FRAME_START | INT_DMA_P_DONE);
        timer_mod(s->vsync, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  NANOSECONDS_PER_SECOND / 60);
    }
}

static uint64_t mdp_read(void *opaque, hwaddr off, unsigned size)
{
    MsmMdpState *s = opaque;

    switch (off) {
    case MDP_INTR_ENABLE:
        return s->intr_enable;
    case MDP_INTR_STATUS:
        return s->intr_status;
    case MDP_HW_VERSION:
        return 0x03010000;
    case MDP_PPP_SCALE_STATUS:
    case MDP_PPP_BLEND_STATUS:
        return 0;
    }
    if (off < MDP_SIZE) {
        return s->regs[off >> 2];
    }
    return 0;
}

static void mdp_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MsmMdpState *s = opaque;

    if (off >= MDP_SIZE) {
        return;
    }
    switch (off) {
    case MDP_INTR_ENABLE:
        s->intr_enable = val;
        mdp_update_irq(s);
        return;
    case MDP_INTR_CLEAR:
        s->intr_status &= ~val;
        mdp_update_irq(s);
        return;
    case MDP_DISPLAY0_START:
        s->regs[off >> 2] = val;
        msm_mdp_ppp_run(s);
        msm_mdp_raise(s, INT_DL0_ROI_DONE);
        return;
    case MDP_DMA_P_START:
        s->invalidate = true;
        msm_mdp_raise(s, INT_DMA_P_DONE);
        return;
    case MDP_DMA_S_START:
        msm_mdp_raise(s, INT_DMA_S_DONE);
        return;
    case MDP_LCDC_EN:
        s->regs[off >> 2] = val;
        if (val & 1) {
            if (!timer_pending(s->vsync)) {
                timer_mod(s->vsync, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                          NANOSECONDS_PER_SECOND / 60);
            }
        } else {
            timer_del(s->vsync);
        }
        s->invalidate = true;
        return;
    case MDP_DMA_P_IBUF_ADDR:
    case MDP_DMA_P_CONFIG:
    case MDP_DMA_P_SIZE:
    case MDP_DMA_P_IBUF_Y_STRIDE:
        s->invalidate = true;
        break;
    }
    s->regs[off >> 2] = val;
}

static const MemoryRegionOps mdp_ops = {
    .read = mdp_read,
    .write = mdp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

void msm_mdp_setup_scanout(MsmMdpState *s, uint32_t base, uint32_t w,
                           uint32_t h, uint32_t stride, bool rgb565)
{
    s->regs[MDP_DMA_P_IBUF_ADDR >> 2] = base;
    s->regs[MDP_DMA_P_SIZE >> 2] = (h << 16) | w;
    s->regs[MDP_DMA_P_IBUF_Y_STRIDE >> 2] = stride;
    /* DMA_OUT_SEL_LCDC | format */
    s->regs[MDP_DMA_P_CONFIG >> 2] = (2u << 19) | (rgb565 ? (1u << 25) : (2u << 25));
    s->regs[MDP_LCDC_EN >> 2] = 1;
    timer_mod(s->vsync, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / 60);
}

void msm_mdp_set_backlight(MsmMdpState *s, int level)
{
    level = MAX(0, MIN(level, 255));
    if (s->brightness != level) {
        s->brightness = level;
        s->invalidate = true;
    }
}

static bool mdp_gfx_update(void *opaque)
{
    MsmMdpState *s = opaque;
    DisplaySurface *surf = qemu_console_surface(s->con);
    uint32_t base = s->regs[MDP_DMA_P_IBUF_ADDR >> 2];
    uint32_t size = s->regs[MDP_DMA_P_SIZE >> 2];
    uint32_t stride = s->regs[MDP_DMA_P_IBUF_Y_STRIDE >> 2];
    uint32_t fmt = (s->regs[MDP_DMA_P_CONFIG >> 2] >> 25) & 3;
    uint32_t w = size & 0x7FF, h = (size >> 16) & 0x7FF;
    int bpp = fmt == 1 ? 2 : fmt == 2 ? 4 : 3;
    uint8_t *dst;
    int dstride;
    g_autofree uint8_t *line = NULL;
    bool on = (s->regs[MDP_LCDC_EN >> 2] & 1) && base && s->panel_on &&
              s->brightness;
    unsigned br = s->brightness;

    if (!w || w > PANEL_W) {
        w = PANEL_W;
    }
    if (!h || h > PANEL_H) {
        h = PANEL_H;
    }
    if (!stride) {
        stride = w * bpp;
    }
    if (surface_width(surf) != PANEL_W || surface_height(surf) != PANEL_H) {
        qemu_console_resize(s->con, PANEL_W, PANEL_H);
        surf = qemu_console_surface(s->con);
    }
    dst = surface_data(surf);
    dstride = surface_stride(surf);

    if (!on) {
        if (s->was_on || s->invalidate) {
            for (int y = 0; y < PANEL_H; y++) {
                memset(dst + y * dstride, 0, PANEL_W * 4);
            }
            qemu_console_update(s->con, 0, 0, PANEL_W, PANEL_H);
        }
        s->was_on = false;
        s->invalidate = false;
        return true;
    }
    s->was_on = true;
    line = g_malloc(stride);
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *d = (uint32_t *)(dst + y * dstride);
        address_space_read(&address_space_memory, base + y * stride,
                           MEMTXATTRS_UNSPECIFIED, line, w * bpp);
        for (uint32_t x = 0; x < w; x++) {
            uint8_t r, g, b;
            if (bpp == 2) {
                uint16_t p = line[2 * x] | (line[2 * x + 1] << 8);
                r = ((p >> 11) & 0x1F) << 3;
                g = ((p >> 5) & 0x3F) << 2;
                b = (p & 0x1F) << 3;
                r |= r >> 5;
                g |= g >> 6;
                b |= b >> 5;
            } else if (bpp == 4) {
                /* XRGB8888 in memory as B,G,R,X (little endian 0xXXRRGGBB) */
                b = line[4 * x];
                g = line[4 * x + 1];
                r = line[4 * x + 2];
            } else {
                r = line[3 * x];
                g = line[3 * x + 1];
                b = line[3 * x + 2];
            }
            if (br < 255) {
                r = r * br / 255;
                g = g * br / 255;
                b = b * br / 255;
            }
            d[x] = rgb_to_pixel32(r, g, b);
        }
    }
    qemu_console_update(s->con, 0, 0, PANEL_W, PANEL_H);
    s->invalidate = false;
    return true;
}

static void mdp_gfx_invalidate(void *opaque)
{
    MsmMdpState *s = opaque;
    s->invalidate = true;
}

static const GraphicHwOps mdp_gfx_ops = {
    .invalidate = mdp_gfx_invalidate,
    .gfx_update = mdp_gfx_update,
};

static void mdp_reset(DeviceState *dev)
{
    MsmMdpState *s = MSM_MDP(dev);

    memset(s->regs, 0, MDP_SIZE);
    s->intr_enable = 0;
    s->intr_status = 0;
    s->panel_on = true;
    s->brightness = 255;
    timer_del(s->vsync);
    mdp_update_irq(s);
}

static void mdp_realize(DeviceState *dev, Error **errp)
{
    MsmMdpState *s = MSM_MDP(dev);

    s->con = qemu_graphic_console_create(dev, 0, &mdp_gfx_ops, s);
    qemu_console_resize(s->con, PANEL_W, PANEL_H);
}

static void mdp_init(Object *obj)
{
    MsmMdpState *s = MSM_MDP(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->regs = g_malloc0(MDP_SIZE);
    memory_region_init_io(&s->iomem, obj, &mdp_ops, s, "msm-mdp", MDP_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->vsync = timer_new_ns(QEMU_CLOCK_VIRTUAL, mdp_vsync, s);
}

static void mdp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = mdp_realize;
    device_class_set_legacy_reset(dc, mdp_reset);
}

static const TypeInfo msm_mdp_info = {
    .name = TYPE_MSM_MDP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmMdpState),
    .instance_init = mdp_init,
    .class_init = mdp_class_init,
};

static void msm_mdp_register(void)
{
    type_register_static(&msm_mdp_info);
}
type_init(msm_mdp_register)
