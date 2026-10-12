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

/* load AMSS and start the modem core at its entry point */
void msm_modem_lle_reset(DeviceState *dev);
ARMCPU *msm_modem_lle_cpu(DeviceState *dev);
/* the modem core's bus, for mapping modem-private peripherals */
MemoryRegion *msm_modem_lle_bus(DeviceState *dev);
/* input n of the modem core's interrupt controller */
qemu_irq msm_modem_lle_irq(DeviceState *dev, int n);

#endif
