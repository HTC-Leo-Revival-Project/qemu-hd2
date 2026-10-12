/*
 * HTC HD2 baseband, low level: the modem core running AMSS
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#ifndef HW_ARM_HTCLEO_MSM_MODEM_LLE_H
#define HW_ARM_HTCLEO_MSM_MODEM_LLE_H

#include "hw/core/qdev.h"
#include "target/arm/cpu-qom.h"

#define TYPE_MSM_MODEM_LLE "msm-modem-lle"

/*
 * Modem VIC inputs.  AMSS names interrupts by tramp id; its tramp table maps
 * them to HAL ids and the HAL table to VIC lines (read at run time):
 * tramp 59 (DMOV dev 0) -> HAL 55 -> line 52, tramp 162 (watchdog bark,
 * FIQ) -> HAL 146 -> line 55, the L4 tick is line 32.
 */
#define MIRQ_ADM0           52
#define MIRQ_DOG_BARK       55

/* load AMSS and start the modem core at its entry point */
void msm_modem_lle_reset(DeviceState *dev);
ARMCPU *msm_modem_lle_cpu(DeviceState *dev);
/* the modem core's bus, for mapping modem-private peripherals */
MemoryRegion *msm_modem_lle_bus(DeviceState *dev);
/* input n of the modem core's interrupt controller */
qemu_irq msm_modem_lle_irq(DeviceState *dev, int n);

#endif
