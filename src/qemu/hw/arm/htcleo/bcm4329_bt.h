/*
 * Broadcom BCM4329 Bluetooth core (HCI over UART1DM) and its FM receiver
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#ifndef HW_ARM_HTCLEO_BCM4329_BT_H
#define HW_ARM_HTCLEO_BCM4329_BT_H

#include "hw/core/qdev.h"

#define TYPE_BCM4329_BT "bcm4329-bt"

/* BT_SHUTDOWN_N and BT_RESET_N both high: the core runs */
void bcm4329_bt_set_power(DeviceState *dev, bool on);
/* fill frames of 48 kHz stereo S16 from the FM receiver's audio output */
void bcm4329_fm_read_audio(DeviceState *dev, int16_t *buf, int frames);

/* bcm4329_fm.c: the FM core, reached through HCI vendor command 0xFC15 */
typedef struct Bcm4329Fm Bcm4329Fm;
typedef void (*Bcm4329FmIrq)(void *opaque);

Bcm4329Fm *bcm4329_fm_new(const char *stations, Bcm4329FmIrq irq,
                          void *opaque);
void bcm4329_fm_reset(Bcm4329Fm *fm);
/* register access; returns the bytes read back (reads: 2, writes: 0) */
int bcm4329_fm_access(Bcm4329Fm *fm, uint8_t reg, bool write,
                      const uint8_t *data, int len, uint8_t *out);
void bcm4329_fm_audio(Bcm4329Fm *fm, int16_t *buf, int frames);

#endif
