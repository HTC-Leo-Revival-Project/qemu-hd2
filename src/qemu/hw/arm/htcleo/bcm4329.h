/*
 * Broadcom BCM4329 WLAN chip (SDIO)
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#ifndef HW_ARM_HTCLEO_BCM4329_H
#define HW_ARM_HTCLEO_BCM4329_H

#include "hw/core/qdev.h"

#define TYPE_BCM4329 "bcm4329"

/* the chip's power/reset line (WIFI_SHUTDOWN_N) */
void bcm4329_set_power(DeviceState *dev, bool on);
/* wire the chip to an SDCC slot */
void bcm4329_attach(DeviceState *dev, DeviceState *sdcc);

#endif
