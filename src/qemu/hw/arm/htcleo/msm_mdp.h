/*
 * Qualcomm MDP 3.1 display processor
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#ifndef HW_ARM_HTCLEO_MSM_MDP_H
#define HW_ARM_HTCLEO_MSM_MDP_H

#include "hw/core/sysbus.h"
#include "ui/console.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmMdpState, MSM_MDP)

struct MsmMdpState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    QemuConsole *con;
    QEMUTimer *vsync;
    uint32_t *regs;
    uint32_t intr_enable;
    uint32_t intr_status;
    bool invalidate;
    bool was_on;
    bool panel_on;
    uint64_t ppp_ops;
    uint8_t brightness;         /* LCD backlight, 0 = off */
};

void msm_mdp_raise(MsmMdpState *s, uint32_t bits);
void msm_mdp_ppp_run(MsmMdpState *s);
void msm_mdp_set_backlight(MsmMdpState *s, int level);
void msm_mdp_setup_scanout(MsmMdpState *s, uint32_t base, uint32_t w,
                           uint32_t h, uint32_t stride, bool rgb565);

#endif
