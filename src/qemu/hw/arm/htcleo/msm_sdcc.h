/*
 * Qualcomm MSM SD card controller: SDIO card hook
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#ifndef HW_ARM_HTCLEO_MSM_SDCC_H
#define HW_ARM_HTCLEO_MSM_SDCC_H

#include "hw/core/qdev.h"

/*
 * An SDIO card wired straight to a controller (QEMU's SD bus only knows
 * memory cards).  command() returns the response length in bytes: 0 for
 * no response (timeout), 4 for a short and 16 for a long response.  A data
 * command's bytes then move through read()/write(); data_end() marks the
 * end of the data phase.
 */
typedef struct MsmSdioCardOps {
    size_t (*command)(void *opaque, uint8_t cmd, uint32_t arg,
                      uint32_t resp[4]);
    bool (*data_ready)(void *opaque);
    void (*read)(void *opaque, uint8_t *buf, size_t len);
    void (*write)(void *opaque, const uint8_t *buf, size_t len);
    void (*data_end)(void *opaque);
} MsmSdioCardOps;

void msm_sdcc_attach_sdio(DeviceState *sdcc, const MsmSdioCardOps *ops,
                          void *opaque);

/*
 * The data mover moving the controller's FIFO (mapped at base): it then
 * waits for the controller's CRCI, so it may be started before the card
 * has been sent its data command.
 */
void msm_sdcc_connect_adm(DeviceState *sdcc, DeviceState *adm, hwaddr base);

#endif
