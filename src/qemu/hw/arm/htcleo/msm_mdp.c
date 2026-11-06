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
#include "qemu/error-report.h"
#include "hw/core/qdev-properties.h"
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
        return 0x03010301;     /* MDP 3.1, what qdi.dll requires */
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
        s->st_dma++;
        msm_mdp_raise(s, INT_DMA_P_DONE);
        return;
    case MDP_DMA_S_START:
        msm_mdp_raise(s, INT_DMA_S_DONE);
        return;
    case MDP_LCDC_EN:
        s->regs[off >> 2] = val;
        s->redraw = true;
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
    /* DMA_OUT_SEL_LCDC | format | PACK_PATTERN_RGB (32 bpp: B,G,R,X) */
    s->regs[MDP_DMA_P_CONFIG >> 2] = (2u << 19) | (0x21u << 8) |
                                     (rgb565 ? (1u << 25) : (2u << 25));
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

static void mdp_build_lut565(MsmMdpState *s, unsigned br)
{
    if (!s->lut565) {
        s->lut565 = g_new(uint32_t, 65536);
    }
    for (uint32_t p = 0; p < 65536; p++) {
        uint8_t r = ((p >> 11) & 0x1F) << 3;
        uint8_t g = ((p >> 5) & 0x3F) << 2;
        uint8_t b = (p & 0x1F) << 3;

        r |= r >> 5;
        g |= g >> 6;
        b |= b >> 5;
        if (br < 255) {
            r = r * br / 255;
            g = g * br / 255;
            b = b * br / 255;
        }
        s->lut565[p] = rgb_to_pixel32(r, g, b);
    }
    s->lut_br = br;
    s->lut_valid = true;
}

/* the plain conversion of one pixel: what the surface must hold */
static uint32_t mdp_ref_pixel(const uint8_t *line, int bpp, uint32_t x,
                              uint32_t pack, unsigned br)
{
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
        uint8_t c[3] = { 0, 0, 0 };

        for (int k = 0; k < 3; k++) {
            c[((pack >> (2 * k)) & 3) % 3] = line[4 * x + k];
        }
        g = c[0];
        b = c[1];
        r = c[2];
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
    return rgb_to_pixel32(r, g, b);
}

static bool mdp_gfx_update(void *opaque)
{
    MsmMdpState *s = opaque;
    DisplaySurface *surf = qemu_console_surface(s->con);
    uint32_t base = s->regs[MDP_DMA_P_IBUF_ADDR >> 2];
    uint32_t size = s->regs[MDP_DMA_P_SIZE >> 2];
    uint32_t stride = s->regs[MDP_DMA_P_IBUF_Y_STRIDE >> 2];
    uint32_t fmt = (s->regs[MDP_DMA_P_CONFIG >> 2] >> 25) & 3;
    uint32_t pack = (s->regs[MDP_DMA_P_CONFIG >> 2] >> 8) & 0x3f;
    uint32_t w = size & 0x7FF, h = (size >> 16) & 0x7FF;
    int bpp = fmt == 1 ? 2 : fmt == 2 ? 4 : 3;
    uint8_t *dst;
    int dstride;
    g_autofree uint8_t *line = NULL;
    bool on = (s->regs[MDP_LCDC_EN >> 2] & 1) && base && s->panel_on &&
              s->brightness;
    unsigned br = s->brightness;
    int64_t t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    uint32_t key[6], rowbytes, y0, y1, nrows = 0;
    bool full;

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
        s->prev_valid = false;
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
        s->prev_valid = false;
        return true;
    }
    s->was_on = true;

    rowbytes = w * bpp;
    key[0] = w; key[1] = h; key[2] = fmt; key[3] = pack; key[4] = br;
    key[5] = surface_stride(surf);
    if (memcmp(key, s->prev_key, sizeof(key))) {
        memcpy(s->prev_key, key, sizeof(key));
        s->prev_valid = false;
    }
    if (s->redraw) {
        s->redraw = false;
        s->prev_valid = false;
    }
    full = !s->diff || !s->prev_valid;
    if (s->diff && !s->prev) {
        s->prev = g_malloc(PANEL_W * PANEL_H * 4);
    }
    if (bpp == 2 && (!s->lut_valid || s->lut_br != br)) {
        mdp_build_lut565(s, br);
    }

    line = g_malloc(MAX(stride, rowbytes));
    y0 = h;
    y1 = 0;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *d = (uint32_t *)(dst + y * dstride);

        address_space_read(&address_space_memory, base + (hwaddr)y * stride,
                           MEMTXATTRS_UNSPECIFIED, line, rowbytes);
        if (!full && !memcmp(line, s->prev + (size_t)y * rowbytes, rowbytes)) {
            continue;
        }
        if (s->diff) {
            memcpy(s->prev + (size_t)y * rowbytes, line, rowbytes);
        }
        if (bpp == 2) {
            const uint16_t *src = (const uint16_t *)line;

            for (uint32_t x = 0; x < w; x++) {
                d[x] = s->lut565[lduw_le_p(src + x)];
            }
        } else if (bpp == 4) {
            /*
             * XRGB8888: the pack pattern names the component in each
             * byte, from byte 0 up (2 bits each: 0 G, 1 B, 2 R).  The
             * kernel's 32 bpp framebuffer is R,G,B,X with PATTERN_BGR.
             */
            int where[3] = { 0, 0, 0 };

            for (int k = 0; k < 3; k++) {
                where[((pack >> (2 * k)) & 3) % 3] = k;
            }
            for (uint32_t x = 0; x < w; x++) {
                const uint8_t *p = line + 4 * x;
                uint8_t g = p[where[0]], b = p[where[1]], r = p[where[2]];

                if (br < 255) {
                    r = r * br / 255;
                    g = g * br / 255;
                    b = b * br / 255;
                }
                d[x] = rgb_to_pixel32(r, g, b);
            }
        } else {
            for (uint32_t x = 0; x < w; x++) {
                uint8_t r = line[3 * x], g = line[3 * x + 1];
                uint8_t b = line[3 * x + 2];

                if (br < 255) {
                    r = r * br / 255;
                    g = g * br / 255;
                    b = b * br / 255;
                }
                d[x] = rgb_to_pixel32(r, g, b);
            }
        }
        y0 = MIN(y0, y);
        y1 = y;
        nrows++;
    }
    if (full) {
        y0 = 0;
        y1 = PANEL_H - 1;
    }
    if (nrows || full) {
        qemu_console_update(s->con, 0, y0, PANEL_W, y1 - y0 + 1);
    }
    if (s->verify) {
        for (uint32_t y = 0; y < h; y++) {
            const uint32_t *d = (const uint32_t *)(dst + y * dstride);

            address_space_read(&address_space_memory,
                               base + (hwaddr)y * stride,
                               MEMTXATTRS_UNSPECIFIED, line, rowbytes);
            for (uint32_t x = 0; x < w; x++) {
                if (d[x] != mdp_ref_pixel(line, bpp, x, pack, br)) {
                    if (s->st_bad++ < 20) {
                        error_report("msm-mdp: verify: row %u column %u is "
                                     "%08x, should be %08x (diff %d, "
                                     "full %d)", y, x, d[x],
                                     mdp_ref_pixel(line, bpp, x, pack, br),
                                     s->diff, full);
                    }
                    break;
                }
            }
        }
    }
    s->prev_valid = s->diff;
    s->invalidate = false;
    s->st_refresh++;
    s->st_rows += nrows;
    s->st_refresh_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t0;
    return true;
}

static void mdp_gfx_invalidate(void *opaque)
{
    MsmMdpState *s = opaque;
    s->invalidate = true;
    s->redraw = true;
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

static void mdp_stats(void *opaque)
{
    MsmMdpState *s = opaque;
    uint64_t ppp = s->ppp_ops - s->st_ppp0;

    if (s->st_dma || ppp || s->st_refresh) {
        info_report("msm-mdp: per second: %.1f frames flipped, %.1f blits, "
                    "%.1f refreshes of %.1f rows (%.2f ms each)",
                    s->st_dma / 2.0, ppp / 2.0, s->st_refresh / 2.0,
                    s->st_refresh ? (double)s->st_rows / s->st_refresh : 0,
                    s->st_refresh ?
                    s->st_refresh_ns / 1e6 / s->st_refresh : 0);
    }
    msm_mdp_ppp_dump();
    s->st_dma = s->st_refresh = s->st_rows = s->st_refresh_ns = 0;
    s->st_ppp0 = s->ppp_ops;
    timer_mod(s->stats, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 2000);
}

static void mdp_realize(DeviceState *dev, Error **errp)
{
    MsmMdpState *s = MSM_MDP(dev);

    s->stats = timer_new_ms(QEMU_CLOCK_REALTIME, mdp_stats, s);
    timer_mod(s->stats, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 2000);

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

static const Property mdp_props[] = {
    /* convert only the rows the guest changed (off: the whole frame, always) */
    DEFINE_PROP_BOOL("diff", MsmMdpState, diff, true),
    DEFINE_PROP_BOOL("verify", MsmMdpState, verify, false),
};

static void mdp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_props(dc, mdp_props);
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
