/*
 * HTC HD2 I2C peripherals
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#ifndef HW_ARM_HTCLEO_I2CDEV_H
#define HW_ARM_HTCLEO_I2CDEV_H

#include "hw/i2c/i2c.h"

/* register-file chip: init is (reg, value) pairs */
I2CSlave *htcleo_regdev_create(I2CBus *bus, uint8_t addr,
                               const uint8_t *init, int n);

void htcleo_ts_set_power(I2CSlave *ts, bool on);
/* x, y in controller units (6..576, 6..952) */
void htcleo_ts_touch(I2CSlave *ts, bool down, int x, int y);

#endif
