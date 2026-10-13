/*
 * Generic memory-backed MSM register block
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#ifndef HW_ARM_HTCLEO_MSM_REGFILE_H
#define HW_ARM_HTCLEO_MSM_REGFILE_H

#include "hw/core/sysbus.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmRegfileState, MSM_REGFILE)

struct MsmRegfileState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t size;
    char *name;
    uint32_t nwords;
    uint32_t *regs;
    uint32_t *reset_val;
    uint32_t *force_set;
    uint32_t *force_clr;
    uint32_t (*read_hook)(void *opaque, hwaddr off, uint32_t val);
    void (*write_hook)(void *opaque, hwaddr off, uint32_t val);
    void *hook_opaque;
};

void msm_regfile_preset(MsmRegfileState *s, hwaddr off, uint32_t val);
void msm_regfile_force(MsmRegfileState *s, hwaddr off, uint32_t set,
                       uint32_t clr);
uint32_t msm_regfile_get(MsmRegfileState *s, hwaddr off);

#endif
