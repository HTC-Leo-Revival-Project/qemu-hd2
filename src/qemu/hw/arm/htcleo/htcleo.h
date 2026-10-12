/*
 * HTC HD2 (htcleo) / Qualcomm QSD8250 emulation - shared definitions
 *
 * Every address and interrupt number in this file comes from the htcleo
 * kernel (arch/arm/mach-msm/include/mach/msm_iomap-8x50.h, irqs.h and
 * board-htcleo.h) or from cLK (platform/qsd8k).
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#ifndef HW_ARM_HTCLEO_H
#define HW_ARM_HTCLEO_H

#include "hw/core/sysbus.h"
#include "hw/core/irq.h"

/* ------------------------------------------------------------------ */
/* Physical memory map                                                 */
/* ------------------------------------------------------------------ */
#define HTCLEO_RAM_BASE         0x00000000
#define HTCLEO_RAM_SIZE         0x40000000      /* SMI + EBI1 + PMEM pools */

#define MSM_SHARED_RAM_PHYS     0x00100000      /* SMEM, 1 MiB */
#define MSM_SHARED_RAM_SIZE     0x00100000

#define HTCLEO_KERNEL_BASE      0x11800000      /* MSM_EBI1_BANK0_BASE */
#define HTCLEO_SPL_FB_BASE      0x02A00000      /* framebuffer the SPL sets up */
#define HTCLEO_RAM_CONSOLE      0x2FFC0000
#define HTCLEO_RAM_CONSOLE_SIZE 0x00040000

#define MSM_GPU_PHYS            0xA0000000
#define MSM_UARTDM1_PHYS        0xA0200000
#define MSM_SDC1_PHYS           0xA0300000
#define MSM_SDC2_PHYS           0xA0400000
#define MSM_SDC3_PHYS           0xA0500000
#define MSM_SDC4_PHYS           0xA0600000
#define MSM_HSUSB_PHYS          0xA0800000
#define MSM_UARTDM2_PHYS        0xA0900000
#define MSM_NAND_PHYS           0xA0A00000
#define MSM_SPI_PHYS            0xA1200000
#define MSM_SSBI_PHYS           0xA8100000
#define MSM_CLK_CTL_PHYS        0xA8600000
#define MSM_TCSR_PHYS           0xA8700000
#define MSM_SCPLL_PHYS          0xA8800000
#define MSM_GPIOCFG1_PHYS       0xA8E00000
#define MSM_GPIOCFG2_PHYS       0xA8F00000
#define MSM_GPIO1_PHYS          0xA9000000
#define MSM_GPIO2_PHYS          0xA9100000
#define MSM_DMOV_PHYS           0xA9700000
#define MSM_I2C_PHYS            0xA9900000
#define MSM_UART1_PHYS          0xA9A00000
#define MSM_UART2_PHYS          0xA9B00000
#define MSM_UART3_PHYS          0xA9C00000
#define MSM_MDP_PHYS            0xAA200000
#define MSM_TSSC_PHYS           0xAA300000
#define MSM_MDC_PHYS            0xAA500000
#define MSM_PMDH_PHYS           0xAA600000
#define MSM_EMDH_PHYS           0xAA700000
#define MSM_VIC_PHYS            0xAC000000
#define MSM_CSR_PHYS            0xAC100000
#define MSM_SIRC_PHYS           0xAC200000
#define MSM_CLK_CTL_SH2_PHYS    0xABA01000

/* ------------------------------------------------------------------ */
/* VIC interrupt numbers (QSD8x50)                                     */
/* ------------------------------------------------------------------ */
#define INT_A9_M2A_0            0
#define INT_A9_M2A_1            1
#define INT_A9_M2A_2            2
#define INT_A9_M2A_3            3
#define INT_A9_M2A_4            4
#define INT_A9_M2A_5            5
#define INT_A9_M2A_6            6
#define INT_GP_TIMER_EXP        7
#define INT_DEBUG_TIMER_EXP     8
#define INT_SIRC_0              9
#define INT_SDC3_0              10
#define INT_SDC3_1              11
#define INT_SDC4_0              12
#define INT_SDC4_1              13
#define INT_USB_OTG             15
#define INT_MDDI_PRI            16
#define INT_MDDI_EXT            17
#define INT_MDDI_CLIENT         18
#define INT_MDP                 19
#define INT_GRAPHICS            20
#define INT_ADM_AARM            21
#define INT_ADSP_A11            22
#define INT_ADSP_A9_A11         23
#define INT_SDC1_0              24
#define INT_SDC1_1              25
#define INT_SDC2_0              26
#define INT_SDC2_1              27
#define INT_KEYSENSE            28
#define INT_TCHSCRN_SSBI        29
#define INT_TCHSCRN1            30
#define INT_TCHSCRN2            31
#define INT_PWB_I2C             (32 + 2)
#define INT_NAND_WR_ER_DONE     (32 + 4)
#define INT_NAND_OP_DONE        (32 + 5)
#define INT_UART1DM_IRQ         (32 + 12)
#define INT_UART1DM_RX          (32 + 13)
#define INT_SCPLL_CTL_DONE      (32 + 19)
#define INT_UART2DM_IRQ         (32 + 20)
#define INT_UART2DM_RX          (32 + 21)
#define INT_USB_HS              (32 + 26)
#define INT_SIRC_1              (32 + 31)

/* SIRC sources (Linux numbers them FIRST_SIRC_IRQ + n) */
#define SIRC_UART1              0
#define SIRC_UART2              1
#define SIRC_UART3              2
#define SIRC_UART1_RX           3
#define SIRC_UART2_RX           4
#define SIRC_UART3_RX           5
#define SIRC_SPI_INPUT          6
#define SIRC_SPI_OUTPUT         7
#define SIRC_SPI_ERROR          8
#define SIRC_GPIO_GROUP1        9
#define SIRC_GPIO_GROUP2        10
#define SIRC_NUM                23

#define MSM_NUM_GPIOS           165

/* ------------------------------------------------------------------ */
/* htcleo board GPIOs (board-htcleo.h)                                 */
/* ------------------------------------------------------------------ */
#define HTCLEO_GPIO_PS_HOLD             25
#define HTCLEO_GPIO_KP_MKOUT0           33
#define HTCLEO_GPIO_KP_MKOUT1           32
#define HTCLEO_GPIO_KP_MKOUT2           31
#define HTCLEO_GPIO_KP_MPIN0            42
#define HTCLEO_GPIO_KP_MPIN1            41
#define HTCLEO_GPIO_KP_MPIN2            40
#define HTCLEO_GPIO_UP_INT_N            90
#define HTCLEO_GPIO_UP_RESET_N          91
#define HTCLEO_GPIO_TS_IRQ              92
#define HTCLEO_GPIO_COMPASS_INT         39  /* AKM8973 INT, level high */
#define HTCLEO_GPIO_PMIC_INT            24  /* PM7540 interrupt to the modem, low */
#define HTCLEO_GPIO_POWER_KEY           94
#define HTCLEO_GPIO_POWER_USB           109
#define HTCLEO_GPIO_SD_STATUS           153
#define HTCLEO_GPIO_TS_POWER            160
#define HTCLEO_GPIO_WIFI_SHUTDOWN_N     129
#define HTCLEO_GPIO_BT_SHUTDOWN_N       128
#define HTCLEO_GPIO_BT_RESET_N          146
#define HTCLEO_GPIO_BT_HOST_WAKE        37
#define HTCLEO_GPIO_BT_CHIP_WAKE        57
#define HTCLEO_GPIO_WIFI_IRQ            152
#define HTCLEO_GPIO_HDS_DET             145
#define HTCLEO_GPIO_COMPASS_INT_N       39
#define HTCLEO_GPIO_BATTERY_OVER_CHG    147
#define HTCLEO_GPIO_BATTERY_CHARGER_ENABLE  22  /* MCHG_EN_N, low = charge */
#define HTCLEO_GPIO_BATTERY_CHARGER_CURRENT 16  /* ISET, high = fast */

/* ------------------------------------------------------------------ */
/* Device type names                                                   */
/* ------------------------------------------------------------------ */
#define TYPE_MSM_VIC        "msm-vic"
#define TYPE_MSM_SIRC       "msm-sirc"
#define TYPE_MSM_CSR        "msm-csr"
#define TYPE_MSM_GPIO       "msm-gpio"
#define TYPE_MSM_UART       "msm-uart"
#define TYPE_MSM_NAND       "msm-nand"
#define TYPE_MSM_ADM        "msm-adm"
#define TYPE_MSM_MDP        "msm-mdp"
#define TYPE_MSM_I2C        "msm-i2c"
#define TYPE_MSM_SDCC       "msm-sdcc"
#define TYPE_MSM_HSUSB      "msm-hsusb"
#define TYPE_MSM_MODEM      "msm-modem"
#define TYPE_MSM_CLKCTL     "msm-clkctl"
#define TYPE_MSM_REGFILE    "msm-regfile"
#define TYPE_MSM_SPI        "msm-spi"
#define TYPE_MSM_UARTDM     "msm-uartdm"
#define TYPE_MSM_MTMR       "msm-mtmr"
#define TYPE_MSM_MDSP       "msm-mdsp"
#define TYPE_MSM_GSTMR      "msm-gstmr"
#define TYPE_MSM_Q6_LINK    "msm-q6-link"
#define TYPE_MSM_WSTMR      "msm-wstmr"
#define TYPE_MSM_SSBI       "msm-ssbi"
#define TYPE_MSM_UIM        "msm-uim"
#define MSM_SSBI_NUM        6   /* SSBI controllers, see msm_ssbi.c */
void msm_ssbi_pmic_set_vbus(DeviceState *dev, bool present);
void msm_ssbi_pmic_set_power_key(DeviceState *dev, bool pressed);
#define TYPE_HTCLEO_MICROP  "htcleo-microp"
#define TYPE_HTCLEO_TS      "htcleo-ts"
#define TYPE_HTCLEO_AKM     "htcleo-akm8973"
#define TYPE_HTCLEO_GAUGE   "htcleo-ds2746"
#define TYPE_TPS65023       "tps65023"
#define TYPE_AKM8973        "akm8973"
#define TYPE_HTCLEO_RAMCON  "htcleo-ramconsole"

/* ------------------------------------------------------------------ */
/* Cross-device hooks                                                  */
/* ------------------------------------------------------------------ */
typedef struct MsmGpioState MsmGpioState;
typedef struct MsmModemState MsmModemState;

/* GPIO block: board-level input pins (pull/level) and output observers */
void msm_gpio_set_input(MsmGpioState *s, int pin, int level);
int msm_gpio_get_output(MsmGpioState *s, int pin);
/* output register bit even if the pin is not driven by the apps side */
int msm_gpio_get_output_latch(MsmGpioState *s, int pin);
/* Board callback invoked whenever the guest changes an output/OE bit. */
typedef void (*MsmGpioOutputNotify)(void *opaque, int pin, int level);
void msm_gpio_set_output_notify(MsmGpioState *s, MsmGpioOutputNotify cb,
                                void *opaque);

/* ADM data mover: paced receive from a peripheral FIFO (UART DM RX) */
typedef void (*MsmAdmRxReady)(void *opaque);
void msm_adm_set_rx_source(DeviceState *adm, hwaddr base, hwaddr size,
                           MsmAdmRxReady ready, void *opaque);
int msm_adm_rx_push(DeviceState *adm, hwaddr base, hwaddr size,
                    const uint8_t *buf, int len);
/*
 * ADM data mover: a peripheral FIFO window paced by its CRCI.  ready()
 * says whether a transfer from (to_periph false) or to the window can run
 * now; the peripheral calls msm_adm_crci_kick() when that may have changed.
 */
typedef bool (*MsmAdmCrciReady)(void *opaque, bool to_periph);
void msm_adm_add_crci(DeviceState *adm, hwaddr base, hwaddr size,
                      MsmAdmCrciReady ready, void *opaque);
void msm_adm_crci_kick(DeviceState *adm);

/* UART DM: byte stream to and from the device on the other end */
typedef void (*MsmUartDmTx)(void *opaque, const uint8_t *buf, int len);
void msm_uartdm_set_peer(DeviceState *uart, MsmUartDmTx tx, void *opaque);
void msm_uartdm_receive(DeviceState *uart, const uint8_t *buf, int len);

/* Modem: the CSR A2M doorbells are forwarded here */
void msm_modem_a2m_irq(MsmModemState *s, int n);

/* Keypad / power key / touchscreen inputs from the UI */
typedef struct HtcleoInput HtcleoInput;

#endif
