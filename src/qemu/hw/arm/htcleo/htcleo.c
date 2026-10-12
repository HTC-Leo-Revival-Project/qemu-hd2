/*
 * HTC HD2 (HTC Leo, "htcleo") - Qualcomm QSD8250 machine
 *
 * Boot flow is the real one: the SPL's last act on hardware is to load the
 * OS image (cLK, flashed by HSPL from LEOIMG.nbh) from the NAND ROM area at
 * block 0x212 to 0x11800000 and jump to it ("OEMLaunchImage 80000000 /
 * Jump to Physical Address 11800000" in the SPL log).  This machine
 * performs that step itself, leaving the SoC configured the way the SPL
 * does (NAND controller, LCDC scanning out of 0x02A00000, clocks running),
 * then cLK takes over and boots Android from NAND exactly as on a phone.
 *
 * Usage:
 *   qemu-system-arm -M htcleo -drive if=mtd,format=raw,file=nand.img \
 *       [-drive if=sd,format=raw,file=sdcard.img] [-bios lk.bin]
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/core/boards.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/misc/unimp.h"
#include "hw/sd/sd.h"
#include "net/net.h"
#include "bcm4329.h"
#include "bcm4329_bt.h"
#include "msm_modem_lle.h"
#include "msm_sdcc.h"
#include "hw/cpu/cluster.h"
#include "hw/core/loader.h"
#include "system/address-spaces.h"
#include "system/physmem.h"
#include "system/system.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "chardev/char.h"
#include "chardev/char-fe.h"
#include "ui/input.h"
#include "standard-headers/linux/input-event-codes.h"
#include "target/arm/cpu.h"
#include "target/arm/cpregs.h"
#include "htcleo.h"
#include "msm_regfile.h"
#include "msm_mdp.h"
#include "msm_adreno.h"
#include "msm_modem.h"
#include "htcleo_i2cdev.h"
#include "hw/i2c/i2c.h"

#define TYPE_HTCLEO_MACHINE MACHINE_TYPE_NAME("htcleo")
OBJECT_DECLARE_SIMPLE_TYPE(HtcleoMachineState, HTCLEO_MACHINE)

/* Keypad matrix (board-htcleo-keypad.c): key = (output col, input row) */
static const int kp_cols[3] = {
    HTCLEO_GPIO_KP_MKOUT0, HTCLEO_GPIO_KP_MKOUT1, HTCLEO_GPIO_KP_MKOUT2
};
static const int kp_rows[3] = {
    HTCLEO_GPIO_KP_MPIN0, HTCLEO_GPIO_KP_MPIN1, HTCLEO_GPIO_KP_MPIN2
};

enum {
    HD2_KEY_VOLUP, HD2_KEY_VOLDOWN, HD2_KEY_WINDOWS, HD2_KEY_CALL,
    HD2_KEY_ENDCALL, HD2_KEY_BACK, HD2_KEY_HOME, HD2_KEY_POWER, HD2_NKEYS
};

/* matrix position of each key; -1 = direct GPIO */
static const struct { int col, row; } hd2_key_pos[HD2_NKEYS] = {
    [HD2_KEY_VOLUP]   = { 0, 0 },
    [HD2_KEY_VOLDOWN] = { 0, 1 },
    [HD2_KEY_WINDOWS] = { 1, 0 },
    [HD2_KEY_CALL]    = { 1, 1 },
    [HD2_KEY_ENDCALL] = { 1, 2 },
    [HD2_KEY_BACK]    = { 2, 0 },
    [HD2_KEY_HOME]    = { 2, 1 },
    [HD2_KEY_POWER]   = { -1, -1 },
};

struct HtcleoMachineState {
    MachineState parent;
    ARMCPU *cpu;
    DeviceState *vic, *sirc, *csr, *modem;
    MsmGpioState *gpio;
    MsmMdpState *mdp;
    MsmRegfileState *clkctl, *scpll, *tcsr;
    DeviceState *nand;
    DeviceState *wifi;
    DeviceState *adm, *bt;
    I2CSlave *gauge;
    int cable;                  /* CABLE_NONE / CABLE_USB / CABLE_AC */
    bool keys[HD2_NKEYS];
    QemuInputHandlerState *kbd;
    QemuInputHandlerState *touch;
    I2CSlave *ts;
    int touch_x, touch_y;
    bool touch_down;
    uint32_t panel_type;        /* 10 = Sharp, 1 = AUO */
    bool modem_lle;             /* run AMSS on a modem core (baseband=lle) */
    char *modem_fw;
    bool modem_trace;
    CPUClusterState apps_cluster;
    DeviceState *lle;
    gchar *fw_data;             /* -bios image, copied to RAM on reset */
    gsize fw_len;

    /* RAM console tap */
    Chardev *ramcon_chr;
    CharFrontend ramcon_fe;
    QEMUTimer *ramcon_timer;
    uint32_t ramcon_pos;
    bool ramcon_valid;
};

/* ------------------------------------------------------------------ */
/* CPU: Scorpion = ARMv7 Cortex-A8 class core with Qualcomm CP15 extras */
/* ------------------------------------------------------------------ */

static void scorpion_add_cpregs(ARMCPU *cpu)
{
    /*
     * Implementation-defined CP15/CP14 space the htcleo kernel and cLK
     * touch: c15 (PVR, L2 control, EFSR, CPMR), c9 opc1 0..3 (cache
     * VIC/floor and DCIALL), c11 (DMA), and the ETM/debug CP14 accesses in
     * arch-init-scorpion.S.  All read as zero and ignore writes.
     */
    static const ARMCPRegInfo regs[] = {
        { .name = "SCORPION_C9_0", .cp = 15, .crn = 9, .crm = 0,
          .opc1 = CP_ANY, .opc2 = 6, .access = PL1_RW,
          .type = ARM_CP_CONST | ARM_CP_OVERRIDE, .resetvalue = 0 },
        { .name = "SCORPION_C9_1", .cp = 15, .crn = 9, .crm = 1,
          .opc1 = CP_ANY, .opc2 = CP_ANY, .access = PL1_RW,
          .type = ARM_CP_CONST | ARM_CP_OVERRIDE, .resetvalue = 0 },
        { .name = "SCORPION_C9_7", .cp = 15, .crn = 9, .crm = 7,
          .opc1 = 0, .opc2 = 0, .access = PL1_RW,
          .type = ARM_CP_CONST | ARM_CP_OVERRIDE, .resetvalue = 0 },
        { .name = "SCORPION_C11", .cp = 15, .crn = 11, .crm = CP_ANY,
          .opc1 = CP_ANY, .opc2 = CP_ANY, .access = PL1_RW,
          .type = ARM_CP_CONST | ARM_CP_OVERRIDE, .resetvalue = 0 },
        { .name = "SCORPION_DBGPRSR", .cp = 14, .crn = 1, .crm = 5,
          .opc1 = 0, .opc2 = 4, .access = PL1_RW,
          .type = ARM_CP_CONST | ARM_CP_OVERRIDE, .resetvalue = 0 },
    };
    for (size_t i = 0; i < ARRAY_SIZE(regs); i++) {
        define_one_arm_cp_reg(cpu, &regs[i]);
    }
    /*
     * ETM (cp14, opc1 1): MAGLDR's entry code writes c1/c0/4 and reads
     * c1/c5/4.  CP_ANY only expands crm/opc1/opc2, so one entry per crn.
     */
    for (int crn = 0; crn < 16; crn++) {
        ARMCPRegInfo etm = {
            .name = g_strdup_printf("SCORPION_ETM_C%d", crn), .cp = 14,
            .crn = crn, .crm = CP_ANY, .opc1 = 1, .opc2 = CP_ANY,
            .access = PL1_RW, .type = ARM_CP_CONST | ARM_CP_OVERRIDE,
            .resetvalue = 0,
        };
        define_one_arm_cp_reg(cpu, &etm);
    }
}

/* ------------------------------------------------------------------ */
/* Keypad                                                              */
/* ------------------------------------------------------------------ */

static void htcleo_update_keys(HtcleoMachineState *s)
{
    for (int r = 0; r < 3; r++) {
        int level = 1;
        for (int k = 0; k < HD2_NKEYS; k++) {
            if (!s->keys[k] || hd2_key_pos[k].row != r) {
                continue;
            }
            /* a pressed key connects its row to a column driven low */
            if (msm_gpio_get_output(s->gpio, kp_cols[hd2_key_pos[k].col]) == 0) {
                level = 0;
            }
        }
        msm_gpio_set_input(s->gpio, kp_rows[r], level);
    }
    msm_gpio_set_input(s->gpio, HTCLEO_GPIO_POWER_KEY, !s->keys[HD2_KEY_POWER]);
    /* the button also drives the PMIC's KPD_PWR input (baseband=lle) */
    if (s->lle) {
        msm_ssbi_pmic_set_power_key(msm_modem_lle_pmic(s->lle),
                                    s->keys[HD2_KEY_POWER]);
    }
}

static void htcleo_wifi_irq(void *opaque, int n, int level)
{
    HtcleoMachineState *s = opaque;

    msm_gpio_set_input(s->gpio, HTCLEO_GPIO_WIFI_IRQ, level);
}

static void htcleo_bt_host_wake(void *opaque, int n, int level)
{
    HtcleoMachineState *s = opaque;

    msm_gpio_set_input(s->gpio, HTCLEO_GPIO_BT_HOST_WAKE, level);
}

static void htcleo_fm_audio(void *opaque, int16_t *buf, int frames)
{
    HtcleoMachineState *s = opaque;

    bcm4329_fm_read_audio(s->bt, buf, frames);
}

static void htcleo_backlight(void *opaque, int n, int level)
{
    HtcleoMachineState *s = opaque;

    msm_mdp_set_backlight(s->mdp, level);
}

/*
 * Charging: the external charger runs while a cable is in and the apps
 * core drives MCHG_EN_N (GPIO 22) low; ISET (GPIO 16) high asks for the
 * fast rate, which a USB port caps at 500 mA.
 */
enum { CABLE_NONE, CABLE_USB, CABLE_AC };
static const char *const cable_names[] = { "none", "usb", "ac" };

static void htcleo_update_charger(HtcleoMachineState *s)
{
    int mA = 0;

    if (!s->gauge) {
        return;
    }
    if (s->cable != CABLE_NONE &&
        msm_gpio_get_output(s->gpio, HTCLEO_GPIO_BATTERY_CHARGER_ENABLE) == 0) {
        bool fast = msm_gpio_get_output(s->gpio,
                                        HTCLEO_GPIO_BATTERY_CHARGER_CURRENT) == 1;
        mA = !fast ? 450 : s->cable == CABLE_AC ? 800 : 500;
    }
    htcleo_battery_set_charge(s->gauge, mA);
}

static void htcleo_set_vbus(HtcleoMachineState *s, bool notify)
{
    bool vbus = s->cable != CABLE_NONE;

    if (s->lle) {
        msm_ssbi_pmic_set_vbus(msm_modem_lle_pmic(s->lle), vbus);
    } else if (notify) {
        msm_modem_set_vbus(MSM_MODEM(s->modem), vbus);
    }
    htcleo_update_charger(s);
}

static void htcleo_gpio_out(void *opaque, int pin, int level)
{
    HtcleoMachineState *s = opaque;

    if (pin == -1 || pin == kp_cols[0] || pin == kp_cols[1] ||
        pin == kp_cols[2]) {
        htcleo_update_keys(s);
    }
    if ((pin == HTCLEO_GPIO_WIFI_SHUTDOWN_N || pin == -1) && s->wifi) {
        /* configured as an output by the modem (TLMM), so use the latch */
        bcm4329_set_power(s->wifi, msm_gpio_get_output_latch(s->gpio,
                                         HTCLEO_GPIO_WIFI_SHUTDOWN_N) == 1);
    }
    if ((pin == HTCLEO_GPIO_BT_SHUTDOWN_N || pin == HTCLEO_GPIO_BT_RESET_N ||
         pin == -1) && s->bt) {
        bcm4329_bt_set_power(s->bt,
            msm_gpio_get_output_latch(s->gpio, HTCLEO_GPIO_BT_SHUTDOWN_N) == 1 &&
            msm_gpio_get_output_latch(s->gpio, HTCLEO_GPIO_BT_RESET_N) == 1);
    }
    if (pin == HTCLEO_GPIO_BATTERY_CHARGER_ENABLE ||
        pin == HTCLEO_GPIO_BATTERY_CHARGER_CURRENT || pin == -1) {
        htcleo_update_charger(s);
    }
    if (pin == HTCLEO_GPIO_TS_POWER && s->ts) {
        htcleo_ts_set_power(s->ts, msm_gpio_get_output(s->gpio, pin) == 1);
    }
    if (pin == HTCLEO_GPIO_PS_HOLD && level == 0 &&
        msm_gpio_get_output(s->gpio, HTCLEO_GPIO_PS_HOLD) == 0) {
        /* PS_HOLD released: the PMIC cuts power */
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    }
}

static void htcleo_key_event(DeviceState *dev, QemuConsole *src,
                             QemuInputEvent *evt)
{
    HtcleoMachineState *s = HTCLEO_MACHINE(qdev_get_machine());
    int k = -1;

    if (evt->type != INPUT_EVENT_KIND_KEY) {
        return;
    }
    /* key events carry Linux key codes */
    switch (evt->key.key) {
    case KEY_PAGEUP:    k = HD2_KEY_VOLUP; break;
    case KEY_PAGEDOWN:  k = HD2_KEY_VOLDOWN; break;
    case KEY_F1:        k = HD2_KEY_WINDOWS; break;
    case KEY_COMPOSE:   k = HD2_KEY_WINDOWS; break;
    case KEY_F2:        k = HD2_KEY_CALL; break;
    case KEY_F3:        k = HD2_KEY_ENDCALL; break;
    case KEY_ESC:       k = HD2_KEY_BACK; break;
    case KEY_HOME:      k = HD2_KEY_HOME; break;
    case KEY_F12:       k = HD2_KEY_POWER; break;
    case KEY_END:       k = HD2_KEY_POWER; break;
    }
    if (k < 0) {
        return;
    }
    s->keys[k] = evt->key.down;
    htcleo_update_keys(s);
}

/* Touchscreen: absolute pointer -> controller units (6..576 x 6..952) */
static void htcleo_touch_event(DeviceState *dev, QemuConsole *src,
                               QemuInputEvent *evt)
{
    HtcleoMachineState *s = HTCLEO_MACHINE(qdev_get_machine());

    switch (evt->type) {
    case INPUT_EVENT_KIND_ABS:
        if (evt->abs.axis == INPUT_AXIS_X) {
            s->touch_x = 6 + (int64_t)evt->abs.value * 570 /
                         INPUT_EVENT_ABS_MAX;
        } else if (evt->abs.axis == INPUT_AXIS_Y) {
            s->touch_y = 6 + (int64_t)evt->abs.value * 946 /
                         INPUT_EVENT_ABS_MAX;
        }
        break;
    case INPUT_EVENT_KIND_BTN:
        if (evt->btn.button == INPUT_BUTTON_LEFT) {
            s->touch_down = evt->btn.down;
        }
        break;
    default:
        break;
    }
}

static void htcleo_touch_sync(DeviceState *dev)
{
    HtcleoMachineState *s = HTCLEO_MACHINE(qdev_get_machine());

    if (s->ts && (s->touch_down || s->touch_x || s->touch_y)) {
        htcleo_ts_touch(s->ts, s->touch_down, s->touch_x, s->touch_y);
    }
}

static const QemuInputHandler htcleo_touchscreen = {
    .name  = "HTC HD2 touchscreen",
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = htcleo_touch_event,
    .sync  = htcleo_touch_sync,
};

static void htcleo_ts_irq(void *opaque, int n, int level)
{
    HtcleoMachineState *s = opaque;
    msm_gpio_set_input(s->gpio, HTCLEO_GPIO_TS_IRQ, level);
}

static void htcleo_pmic_irq(void *opaque, int n, int level)
{
    HtcleoMachineState *s = opaque;
    msm_gpio_set_input(s->gpio, HTCLEO_GPIO_PMIC_INT, level);
}

static void htcleo_akm_irq(void *opaque, int n, int level)
{
    HtcleoMachineState *s = opaque;
    msm_gpio_set_input(s->gpio, HTCLEO_GPIO_COMPASS_INT, level);
}

static const QemuInputHandler htcleo_keyboard = {
    .name  = "HTC HD2 buttons",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = htcleo_key_event,
};

/* ------------------------------------------------------------------ */
/* RAM console tap: copy the kernel's ram_console ring to a chardev    */
/* ------------------------------------------------------------------ */

#define RAMCON_SIG 0x43474244   /* "DBGC" */

static void htcleo_ramcon_poll(void *opaque)
{
    HtcleoMachineState *s = opaque;
    uint32_t hdr[3];
    uint32_t start, size, cap = HTCLEO_RAM_CONSOLE_SIZE - 12;

    physical_memory_read(HTCLEO_RAM_CONSOLE, hdr, sizeof(hdr));
    if (le32_to_cpu(hdr[0]) == RAMCON_SIG) {
        start = le32_to_cpu(hdr[1]);
        size = le32_to_cpu(hdr[2]);
        if (start < cap && size <= cap) {
            if (!s->ramcon_valid || start < s->ramcon_pos) {
                /* first sighting, or wrapped: dump from the oldest byte */
                if (!s->ramcon_valid) {
                    s->ramcon_pos = (size == cap) ? start : 0;
                    if (size < cap) {
                        s->ramcon_pos = 0;
                    }
                } else if (start < s->ramcon_pos) {
                    uint32_t n = cap - s->ramcon_pos;
                    g_autofree uint8_t *b = g_malloc(n);
                    physical_memory_read(HTCLEO_RAM_CONSOLE + 12 +
                                             s->ramcon_pos, b, n);
                    qemu_chr_fe_write_all(&s->ramcon_fe, b, n);
                    s->ramcon_pos = 0;
                }
                s->ramcon_valid = true;
            }
            if (start > s->ramcon_pos) {
                uint32_t n = start - s->ramcon_pos;
                g_autofree uint8_t *b = g_malloc(n);
                physical_memory_read(HTCLEO_RAM_CONSOLE + 12 +
                                         s->ramcon_pos, b, n);
                qemu_chr_fe_write_all(&s->ramcon_fe, b, n);
                s->ramcon_pos = start;
            }
        }
    }
    timer_mod(s->ramcon_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 20 * SCALE_MS);
}

/* ------------------------------------------------------------------ */
/* SPL hand-off                                                        */
/* ------------------------------------------------------------------ */

#define NAND_PAGE_RAW   2112
#define NAND_ROM_BLOCK  0x212

/* Logical (ECC-mode) data of one NAND page, as cLK/Linux read it */
static bool nand_read_page_data(BlockBackend *blk, uint32_t page, uint8_t *out)
{
    uint8_t raw[NAND_PAGE_RAW];

    if (blk_pread(blk, (uint64_t)page * NAND_PAGE_RAW, NAND_PAGE_RAW, raw,
                  0) < 0) {
        return false;
    }
    for (int cw = 0; cw < 4; cw++) {
        memcpy(out + cw * 512, raw + cw * 528, 512);
    }
    /* user bytes displaced by the x16 bad-block marker (CFG1 BB 465) */
    out[3 * 512 + 464] = raw[3 * 528 + 526];
    out[3 * 512 + 465] = raw[3 * 528 + 527];
    return true;
}

/*
 * Where the SPL puts the OS image.  A raw image (cLK) goes to 0x11800000
 * ("OEMLaunchImage 80000000 / Jump to Physical Address 11800000" in a real
 * SPL log).  A WinCE image (MAGLDR) carries an "ECEC" record at +0x40 whose
 * second word is its ROMHDR's file offset; the ROMHDR's physfirst is where
 * the image is linked to run (MAGLDR 1.13: 0x28000000).
 */
static uint32_t htcleo_os_load_address(BlockBackend *blk, uint32_t first_page,
                                       const uint8_t *first)
{
    uint8_t page[2048];
    uint32_t off, pa;

    if (memcmp(first + 0x40, "ECEC", 4)) {
        return HTCLEO_KERNEL_BASE;
    }
    off = ldl_le_p(first + 0x48);
    if ((off & 2047) > 2048 - 12 ||
        !nand_read_page_data(blk, first_page + off / 2048, page)) {
        return HTCLEO_KERNEL_BASE;
    }
    pa = ldl_le_p(page + (off & 2047) + 8);         /* ROMHDR.physfirst */
    if (pa >= HTCLEO_RAM_SIZE || pa & 0xFFF) {
        warn_report("htcleo: WinCE OS image physfirst 0x%08x is not in "
                    "RAM; loading it at 0x%x", pa, HTCLEO_KERNEL_BASE);
        return HTCLEO_KERNEL_BASE;
    }
    return pa;
}

static bool htcleo_load_os_from_nand(BlockBackend *blk, uint32_t *entry)
{
    uint8_t page[2048];
    uint32_t base = NAND_ROM_BLOCK * 64;
    uint32_t start, count, load = HTCLEO_KERNEL_BASE;

    if (!blk || !nand_read_page_data(blk, base, page)) {
        return false;
    }
    /* WinCE MBR: partition 0 type 0x23 holds the OS image (cLK) */
    if (page[0] != 0xE9 || page[1] != 0xFD || page[2] != 0xFF ||
        page[510] != 0x55 || page[511] != 0xAA || page[0x1BE + 4] != 0x23) {
        return false;
    }
    start = ldl_le_p(page + 0x1BE + 8);
    count = ldl_le_p(page + 0x1BE + 12);
    if (!count || count > 0x4000) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (!nand_read_page_data(blk, base + start + i, page)) {
            return false;
        }
        if (i == 0) {
            load = htcleo_os_load_address(blk, base + start, page);
        }
        physical_memory_write(load + i * 2048, page, 2048);
    }
    info_report("htcleo: SPL hand-off: loaded OS image from NAND "
                "(%u pages) to 0x%x", count, load);
    *entry = load;
    return true;
}

static void htcleo_reset(MachineState *machine, ResetType type)
{
    HtcleoMachineState *s = HTCLEO_MACHINE(machine);
    CPUARMState *env = &s->cpu->env;
    BlockBackend *blk = NULL;
    DriveInfo *dinfo = drive_get(IF_MTD, 0, 0);
    bool loaded = false;
    uint32_t entry = HTCLEO_KERNEL_BASE;

    qemu_devices_reset(type);
    if (s->wifi) {
        device_cold_reset(s->wifi);
    }
    cpu_reset(CPU(s->cpu));

    if (dinfo) {
        blk = blk_by_legacy_dinfo(dinfo);
    }
    if (s->lle) {
        /*
         * The modem core boots AMSS.  OSBL, which is skipped, has already
         * released the apps core with the OS loader ("Boot CE") before it
         * jumps to AMSS, so both run from here.
         */
        msm_modem_lle_reset(s->lle);
        msm_modem_qdsp6_boot(MSM_MODEM(s->modem));
    } else {
        /* AMSS has been running since power-on and owns shared memory */
        msm_modem_boot(MSM_MODEM(s->modem));
    }
    htcleo_set_vbus(s, true);

    if (s->fw_data) {
        physical_memory_write(HTCLEO_KERNEL_BASE, s->fw_data, s->fw_len);
        loaded = true;
    } else {
        loaded = htcleo_load_os_from_nand(blk, &entry);
    }
    if (!loaded) {
        error_report("htcleo: no OS image: give -bios lk.bin or a NAND image "
                     "with cLK flashed at block 0x%x", NAND_ROM_BLOCK);
        exit(1);
    }

    /*
     * SPL globals at MSM_PANEL_PHYS (0x80000): board ID and the panel type
     * Panel_TypeDetect() found.  A real HD2's SPL log reads "Board ID is 4"
     * and "Panel_TypeDetect: 10" (Sharp); board-htcleo-panel.c reads them
     * back from +0x1030/+0x1034.
     */
    {
        uint32_t v = cpu_to_le32(4);
        physical_memory_write(0x80000 + 0x1030, &v, 4);
        v = cpu_to_le32(s->panel_type);
        physical_memory_write(0x80000 + 0x1034, &v, 4);
    }

    /* what the SPL leaves configured */
    msm_mdp_setup_scanout(s->mdp, HTCLEO_SPL_FB_BASE, 480, 800, 480 * 2,
                          true);
    msm_gpio_set_input(s->gpio, HTCLEO_GPIO_SD_STATUS,
                       drive_get(IF_SD, 0, 0) ? 0 : 1);
    htcleo_update_keys(s);

    /* SVC mode, IRQ/FIQ masked, MMU off: jump to the OS image */
    env->regs[15] = entry;
    env->thumb = false;

    s->ramcon_valid = false;
    s->ramcon_pos = 0;
}

/* ------------------------------------------------------------------ */
/* Board construction                                                  */
/* ------------------------------------------------------------------ */

/*
 * UART DM (BT high-speed UART): nothing is attached, so MISR/ISR (read side
 * of 0x10/0x14, whose write sides are CR/IMR) never report an event and the status
 * register (0x08) says the transmitter is empty and ready.
 */
static uint32_t uartdm_read_hook(void *opaque, hwaddr off, uint32_t val)
{
    switch (off) {
    case 0x08:
        return 0x0C;        /* TXEMT | TXRDY */
    case 0x10:              /* MISR (write side: CR) */
    case 0x14:              /* ISR (write side: IMR) */
        return 0;
    case 0x38:
        return 0;           /* RX_TOTAL_SNAP */
    }
    return val;
}

/*
 * EBI1 / SMI memory controllers (0xA8300000 / 0xA8400000).  AMSS's sleep
 * code, running TLB-locked from internal SRAM, drives both:
 *   +0x20020  bit 0 requests self-refresh, bit 1 acknowledges it
 *   +0x80010  DDR command; +0x80004 bit 7 reports it done
 * DRAM never goes away here, so every request completes at once.
 */
static uint32_t memctl_read_hook(void *opaque, hwaddr off, uint32_t val)
{
    switch (off) {
    case 0x20020:
        return (val & ~2u) | ((val & 1) << 1);
    case 0x80004:
        return val | 0x80;
    }
    return val;
}

/*
 * TLMM (GPIO configuration, 0xA8E00000): +0x270 is HW_REVISION_NUMBER,
 * JTAG style: version [31:28], part number [27:12], JEDEC id [11:0].
 * AMSS maps part 0x240..0x246 to QSD8250 (0x280.. would be QSD8650).
 */
#define HTCLEO_HW_REVISION  0x102400E1

static uint32_t tlmm_read_hook(void *opaque, hwaddr off, uint32_t val)
{
    return off == 0x270 ? HTCLEO_HW_REVISION : val;
}

/*
 * Bus master halt / clock control (0xA8200000; FSBL halts one master and
 * MAGLDR another before taking the modem over):
 *   +0x3300 + 4n  bit 0 requests a halt of master n, bit 1 acknowledges it
 *   +0x0100 + 4n  bit 5 enables its clock
 *   +0x3424       bit n reports master n's clock off
 * Halts and clock switches take effect at once.
 */
static uint32_t busctl_read_hook(void *opaque, hwaddr off, uint32_t val)
{
    MsmRegfileState *rf = opaque;

    if (off >= 0x3300 && off < 0x3380) {
        return (val & ~2u) | ((val & 1) << 1);
    }
    if (off == 0x3424) {
        for (int n = 0; n < 32; n++) {
            if (!(rf->regs[(0x100 >> 2) + n] & 0x20)) {
                val |= 1u << n;
            } else {
                val &= ~(1u << n);
            }
        }
    }
    return val;
}

static MsmRegfileState *htcleo_regfile(const char *name, hwaddr base,
                                       uint32_t size)
{
    DeviceState *d = qdev_new(TYPE_MSM_REGFILE);

    qdev_prop_set_string(d, "name", name);
    qdev_prop_set_uint32(d, "size", size);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(d), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(d), 0, base);
    return MSM_REGFILE(d);
}

static void htcleo_init(MachineState *machine)
{
    HtcleoMachineState *s = HTCLEO_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    Object *cpuobj;
    DeviceState *dev;
    SysBusDevice *sbd;
    qemu_irq vic_in[64], sirc_in[SIRC_NUM];
    DriveInfo *dinfo;

    if (machine->ram_size != HTCLEO_RAM_SIZE) {
        error_report("htcleo: RAM size must be 1G (the board's physical map)");
        exit(1);
    }

    if (machine->firmware) {
        GError *err = NULL;
        if (!g_file_get_contents(machine->firmware, &s->fw_data, &s->fw_len,
                                 &err)) {
            error_report("htcleo: cannot read firmware '%s': %s",
                         machine->firmware, err->message);
            exit(1);
        }
    }

    /* CPU: the apps core (Scorpion), cluster 0 */
    object_initialize_child(OBJECT(machine), "apps-cluster", &s->apps_cluster,
                            TYPE_CPU_CLUSTER);
    qdev_prop_set_uint32(DEVICE(&s->apps_cluster), "cluster-id", 0);
    cpuobj = object_new(machine->cpu_type);
    object_property_add_child(OBJECT(&s->apps_cluster), "cpu", cpuobj);
    object_unref(cpuobj);
    object_property_set_int(cpuobj, "midr", 0x510F00F2, &error_fatal);
    if (object_property_find(cpuobj, "has_el3")) {
        object_property_set_bool(cpuobj, "has_el3", false, &error_fatal);
    }
    qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
    qdev_realize(DEVICE(&s->apps_cluster), NULL, &error_fatal);
    s->cpu = ARM_CPU(cpuobj);
    scorpion_add_cpregs(s->cpu);

    /* Memory: SMI + EBI1 + PMEM pools are one flat 1 GiB space */
    memory_region_add_subregion(sysmem, HTCLEO_RAM_BASE, machine->ram);

    /* baseband=lle: the modem core (ARM926, cluster 1) runs the real AMSS */
    if (s->modem_lle) {
        s->lle = qdev_new(TYPE_MSM_MODEM_LLE);
        object_property_add_child(OBJECT(machine), "modem-lle",
                                  OBJECT(s->lle));
        object_property_set_link(OBJECT(s->lle), "ram", OBJECT(machine->ram),
                                 &error_fatal);
        qdev_prop_set_string(s->lle, "firmware", s->modem_fw);
        qdev_prop_set_bit(s->lle, "trace", s->modem_trace);
        qdev_realize_and_unref(s->lle, NULL, &error_fatal);
    }

    /* Catch-all for every peripheral window we do not model, logged */
    create_unimplemented_device("msm-periph", 0xA0000000, 0x10000000);

    /* VIC */
    s->vic = qdev_new(TYPE_MSM_VIC);
    sbd = SYS_BUS_DEVICE(s->vic);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, MSM_VIC_PHYS);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_IRQ));
    sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_FIQ));
    for (int i = 0; i < 64; i++) {
        vic_in[i] = qdev_get_gpio_in(s->vic, i);
    }

    /* SIRC */
    s->sirc = qdev_new(TYPE_MSM_SIRC);
    sbd = SYS_BUS_DEVICE(s->sirc);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, MSM_SIRC_PHYS);
    sysbus_connect_irq(sbd, 0, vic_in[INT_SIRC_0]);
    sysbus_connect_irq(sbd, 1, vic_in[INT_SIRC_1]);
    for (int i = 0; i < SIRC_NUM; i++) {
        sirc_in[i] = qdev_get_gpio_in(s->sirc, i);
    }

    /* CSR: GPT/DGT + A2M doorbells */
    s->csr = qdev_new(TYPE_MSM_CSR);
    sbd = SYS_BUS_DEVICE(s->csr);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, MSM_CSR_PHYS);
    sysbus_connect_irq(sbd, 0, vic_in[INT_GP_TIMER_EXP]);
    sysbus_connect_irq(sbd, 1, vic_in[INT_DEBUG_TIMER_EXP]);

    /* Modem (AMSS shared-memory protocols) */
    s->modem = qdev_new(TYPE_MSM_MODEM);
    {
        g_autofree char *img = object_property_get_str(OBJECT(s->modem),
                                                       "smem-image", NULL);
        if ((!img || !*img) && g_file_test("smem.bin", G_FILE_TEST_EXISTS)) {
            qdev_prop_set_string(s->modem, "smem-image", "smem.bin");
        }
    }
    /* packet data (PPP) goes to -nic ...,model=msm-modem */
    qemu_configure_nic_device(s->modem, false, NULL);
    /* /machine/modem, for qom-set modem properties at run time */
    object_property_add_child(OBJECT(machine), "modem", OBJECT(s->modem));
    qdev_realize_and_unref(s->modem, NULL, &error_fatal);
    msm_modem_set_smem(MSM_MODEM(s->modem),
                       (uint8_t *)memory_region_get_ram_ptr(machine->ram) +
                       MSM_SHARED_RAM_PHYS);
    /* with baseband=lle the real AMSS answers the doorbells instead */
    for (int i = 0; i < 16; i++) {
        if (s->lle) {
            if (i < MIRQ_A2M_NUM) {
                qdev_connect_gpio_out_named(s->csr, "a2m", i,
                        msm_modem_lle_irq(s->lle, MIRQ_A2M_0 + i));
            } else if (i == 8) {
                /* A2M 8 rings the QDSP6, still modelled by msm_modem */
                qdev_connect_gpio_out_named(s->csr, "a2m", i,
                        qdev_get_gpio_in_named(s->modem, "a2m", i));
            }
            continue;
        }
        qdev_connect_gpio_out_named(s->csr, "a2m", i,
                                    qdev_get_gpio_in_named(s->modem, "a2m", i));
    }
    for (int i = 0; i < 7; i++) {
        qdev_connect_gpio_out_named(s->lle ? s->lle : s->modem, "m2a", i,
                                    vic_in[INT_A9_M2A_0 + i]);
    }
    qdev_connect_gpio_out_named(s->modem, "m2a", M2A_DSP,
                                vic_in[INT_ADSP_A11]);

    /* GPIO */
    dev = qdev_new(TYPE_MSM_GPIO);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, MSM_GPIO1_PHYS);
    sysbus_mmio_map(sbd, 1, MSM_GPIO2_PHYS);
    sysbus_connect_irq(sbd, 0, sirc_in[SIRC_GPIO_GROUP1]);
    sysbus_connect_irq(sbd, 1, sirc_in[SIRC_GPIO_GROUP2]);
    s->gpio = (MsmGpioState *)dev;
    msm_gpio_set_output_notify(s->gpio, htcleo_gpio_out, s);
    if (s->lle) {
        /* the modem's shadow of the GPIO registers and its group interrupts */
        sysbus_mmio_map_overlap(sbd, 2, 0xA8E00000, 1);
        sysbus_mmio_map_overlap(sbd, 3, 0xA8F00400, 1);
        sysbus_connect_irq(sbd, 2, msm_modem_lle_irq(s->lle, MIRQ_GPIO_GROUP1));
        sysbus_connect_irq(sbd, 3, msm_modem_lle_irq(s->lle, MIRQ_GPIO_GROUP2));
        /* the PM7540's interrupt output, active low, on GPIO 24 */
        qdev_connect_gpio_out_named(msm_modem_lle_pmic(s->lle), "pmic-irq", 0,
                                    qemu_allocate_irq(htcleo_pmic_irq, s, 0));
    }

    /* UART1 (SPL console, kernel debug) on serial0 */
    dev = qdev_new(TYPE_MSM_UART);
    qdev_prop_set_chr(dev, "chardev", serial_hd(0));
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, MSM_UART1_PHYS);
    sysbus_connect_irq(sbd, 0, sirc_in[SIRC_UART1]);
    for (int i = 1; i < 3; i++) {
        if (i == 1 && s->lle) {
            /* UART2 is AMSS's smart card interface: the UIM with the SIM */
            dev = qdev_new(TYPE_MSM_UIM);
            object_property_add_child(OBJECT(machine), "uim", OBJECT(dev));
            sbd = SYS_BUS_DEVICE(dev);
            sysbus_realize_and_unref(sbd, &error_fatal);
            sysbus_mmio_map(sbd, 0, MSM_UART2_PHYS);
            sysbus_connect_irq(sbd, 0, qdev_get_gpio_in_named(s->lle, "sub7",
                                                             MIRQ_SUB7_UIM));
            continue;
        }
        dev = qdev_new(TYPE_MSM_UART);
        sbd = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, i == 1 ? MSM_UART2_PHYS : MSM_UART3_PHYS);
        sysbus_connect_irq(sbd, 0, sirc_in[SIRC_UART1 + i]);
    }

    /* ADM data mover */
    dev = qdev_new(TYPE_MSM_ADM);
    s->adm = dev;
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, MSM_DMOV_PHYS);
    /* the SD0-SD2 pages 0xA9400000-0xA9600000 (AMSS uses SD0) */
    for (int d = 0; d < 3; d++) {
        MemoryRegion *alias = g_new(MemoryRegion, 1);
        memory_region_init_alias(alias, OBJECT(dev), "msm-adm-sd",
                                 sysbus_mmio_get_region(sbd, 0), 0, 0x1000);
        memory_region_add_subregion(sysmem, MSM_DMOV_PHYS - (3 - d) * 0x100000,
                                    alias);
    }
    if (s->lle) {
        /* SD3 interrupts the apps processor, SD0 the modem */
        sysbus_connect_irq(sbd, 1 + 3, vic_in[INT_ADM_AARM]);
        sysbus_connect_irq(sbd, 1 + 0, msm_modem_lle_irq(s->lle, MIRQ_ADM0));
    } else {
        sysbus_connect_irq(sbd, 0, vic_in[INT_ADM_AARM]);
    }

    /* NAND */
    s->nand = qdev_new(TYPE_MSM_NAND);
    dinfo = drive_get(IF_MTD, 0, 0);
    if (dinfo) {
        qdev_prop_set_drive_err(s->nand, "drive", blk_by_legacy_dinfo(dinfo),
                                &error_fatal);
    }
    sbd = SYS_BUS_DEVICE(s->nand);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, MSM_NAND_PHYS);
    sysbus_connect_irq(sbd, 0, vic_in[INT_NAND_OP_DONE]);
    sysbus_connect_irq(sbd, 1, vic_in[INT_NAND_WR_ER_DONE]);

    /*
     * SD controllers: SDC1 is the WiFi SDIO slot (no card: commands time
     * out), SDC2 the microSD slot.
     */
    for (int i = 0; i < 2; i++) {
        static const hwaddr base[2] = { MSM_SDC1_PHYS, MSM_SDC2_PHYS };
        static const int irq[2][2] = { { INT_SDC1_0, INT_SDC1_1 },
                                       { INT_SDC2_0, INT_SDC2_1 } };

        dev = qdev_new(TYPE_MSM_SDCC);
        sbd = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, base[i]);
        sysbus_connect_irq(sbd, 0, vic_in[irq[i][0]]);
        sysbus_connect_irq(sbd, 1, vic_in[irq[i][1]]);
        msm_sdcc_connect_adm(dev, s->adm, base[i]);
        dinfo = i == 1 ? drive_get(IF_SD, 0, 0) : NULL;
        if (dinfo) {
            DeviceState *card = qdev_new(TYPE_SD_CARD);

            qdev_prop_set_drive_err(card, "drive",
                                    blk_by_legacy_dinfo(dinfo), &error_fatal);
            qdev_realize_and_unref(card, qdev_get_child_bus(dev, "sd-bus"),
                                   &error_fatal);
        }
        if (i == 0) {
            /* BCM4329 WLAN on the SDIO slot, network from -nic */
            s->wifi = qdev_new(TYPE_BCM4329);
            qemu_configure_nic_device(s->wifi, true, NULL);
            qdev_realize_and_unref(s->wifi, NULL, &error_fatal);
            qdev_connect_gpio_out_named(s->wifi, "oob", 0,
                                        qemu_allocate_irq(htcleo_wifi_irq,
                                                          s, 0));
            bcm4329_attach(s->wifi, dev);
        }
    }

    /* MDP / LCDC display */
    dev = qdev_new(TYPE_MSM_MDP);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, MSM_MDP_PHYS);
    sysbus_connect_irq(sbd, 0, vic_in[INT_MDP]);
    s->mdp = MSM_MDP(dev);

    /* Clock controller: SPL leaves the PLLs running */
    s->clkctl = htcleo_regfile("msm-clk-ctl", MSM_CLK_CTL_PHYS, 0x1000);
    /* PLLn at 0x300 + 32n: MODE, L, M, N (freq = 19.2 MHz * (L + M/N)) */
    {
        static const struct { int n; uint32_t l; } plls[] = {
            { 0, 40 },      /* MPLL  768 MHz */
            { 1, 41 },      /* GPLL  787.2 MHz */
            { 2, 0 }, { 3, 0 },
            { 4, 20 },      /* 384 MHz */
            { 5, 50 },      /* 960 MHz */
        };
        for (size_t i = 0; i < ARRAY_SIZE(plls); i++) {
            hwaddr b = 0x300 + 32 * plls[i].n;
            msm_regfile_preset(s->clkctl, b, plls[i].l ? 0x7 : 0);
            msm_regfile_preset(s->clkctl, b + 4, plls[i].l);
            msm_regfile_preset(s->clkctl, b + 8, 0);
            msm_regfile_preset(s->clkctl, b + 12, 1);
        }
    }
    msm_regfile_force(s->clkctl, 0x84, 0x1, 0);        /* GRP_NS status */

    /* Scorpion PLL: running at 998.4 MHz (L = 26), never busy */
    s->scpll = htcleo_regfile("msm-scpll", 0xA8800000, 0x1000);
    msm_regfile_preset(s->scpll, 0x04, 0x00000002);    /* CTL: normal */
    msm_regfile_preset(s->scpll, 0x10, 26 << 3);       /* FSM_CTL_EXT */
    msm_regfile_force(s->scpll, 0x18, 0, 0x3);         /* STATUS idle */

    s->tcsr = htcleo_regfile("msm-tcsr", MSM_TCSR_PHYS, 0x1000);
    htcleo_regfile("msm-gpiocfg1", MSM_GPIOCFG1_PHYS, 0x1000)->read_hook =
        tlmm_read_hook;
    htcleo_regfile("msm-gpiocfg2", MSM_GPIOCFG2_PHYS, 0x1000);
    htcleo_regfile("msm-clk-ctl-sh2", MSM_CLK_CTL_SH2_PHYS, 0x1000);
    htcleo_regfile("msm-ssbi", MSM_SSBI_PHYS, 0x1000);
    htcleo_regfile("msm-ebi1-ctl", 0xA8300000, 0x100000)->read_hook =
        memctl_read_hook;
    htcleo_regfile("msm-smi-ctl", 0xA8400000, 0x100000)->read_hook =
        memctl_read_hook;
    {
        MsmRegfileState *rf = htcleo_regfile("msm-busctl", 0xA8200000,
                                             0x10000);
        rf->read_hook = busctl_read_hook;
        rf->hook_opaque = rf;
    }
    htcleo_regfile("msm-tssc", MSM_TSSC_PHYS, 0x1000);
    htcleo_regfile("msm-mdc", MSM_MDC_PHYS, 0x100000);
    htcleo_regfile("msm-pmdh", MSM_PMDH_PHYS, 0x1000);
    htcleo_regfile("msm-emdh", MSM_EMDH_PHYS, 0x1000);
    s->tcsr->read_hook = NULL;
    {
        MsmRegfileState *dm2 = htcleo_regfile("msm-uartdm2",
                                              MSM_UARTDM2_PHYS, 0x1000);
        DeviceState *dm1 = qdev_new(TYPE_MSM_UARTDM);

        dm2->read_hook = uartdm_read_hook;

        /* UART1DM: the BCM4329 Bluetooth core (and its FM receiver) */
        object_property_set_link(OBJECT(dm1), "adm", OBJECT(s->adm),
                                 &error_fatal);
        qdev_prop_set_uint32(dm1, "base", MSM_UARTDM1_PHYS);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dm1), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(dm1), 0, MSM_UARTDM1_PHYS);
        sysbus_connect_irq(SYS_BUS_DEVICE(dm1), 0, vic_in[INT_UART1DM_IRQ]);

        s->bt = qdev_new(TYPE_BCM4329_BT);
        object_property_add_child(OBJECT(machine), "bt", OBJECT(s->bt));
        object_property_set_link(OBJECT(s->bt), "uart", OBJECT(dm1),
                                 &error_fatal);
        qdev_realize_and_unref(s->bt, NULL, &error_fatal);
        qdev_connect_gpio_out_named(s->bt, "host-wake", 0,
                                    qemu_allocate_irq(htcleo_bt_host_wake,
                                                      s, 0));
        msm_modem_set_aux_source(MSM_MODEM(s->modem), htcleo_fm_audio, s);
    }
    htcleo_regfile("msm-spi", MSM_SPI_PHYS, 0x1000);
    htcleo_regfile("msm-sdc3", MSM_SDC3_PHYS, 0x1000);
    htcleo_regfile("msm-sdc4", MSM_SDC4_PHYS, 0x1000);
    dev = qdev_new(TYPE_MSM_HSUSB);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, MSM_HSUSB_PHYS);
    sysbus_connect_irq(sbd, 0, vic_in[INT_USB_HS]);
    /* Adreno 200 GPU */
    dev = qdev_new(TYPE_MSM_ADRENO);
    sbd = SYS_BUS_DEVICE(dev);
    object_property_add_child(OBJECT(machine), "gpu", OBJECT(dev));
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, MSM_GPU_PHYS);
    sysbus_connect_irq(sbd, 0, vic_in[INT_GRAPHICS]);

    /* I2C bus and the board's chips */
    {
        I2CBus *bus;
        I2CSlave *microp;
        static const uint8_t tps65023_init[] = {
            0x00, 0x2B,             /* VERSION */
            0x03, 0x7F,             /* REG_CTRL: all rails on */
            0x06, 0x14,             /* DEFCORE: 1.3 V */
        };

        dev = qdev_new(TYPE_MSM_I2C);
        sbd = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, MSM_I2C_PHYS);
        sysbus_connect_irq(sbd, 0, vic_in[INT_PWB_I2C]);
        bus = (I2CBus *)qdev_get_child_bus(dev, "i2c");

        s->ts = i2c_slave_create_simple(bus, TYPE_HTCLEO_TS, 0x2A >> 1);
        qdev_connect_gpio_out_named(DEVICE(s->ts), "irq", 0,
                                    qemu_allocate_irq(htcleo_ts_irq, s, 0));
        microp = i2c_slave_create_simple(bus, TYPE_HTCLEO_MICROP, 0xCC >> 1);
        qdev_connect_gpio_out_named(DEVICE(microp), "backlight", 0,
                                    qemu_allocate_irq(htcleo_backlight, s, 0));
        htcleo_regdev_create(bus, 0x48, tps65023_init,
                             ARRAY_SIZE(tps65023_init));
        s->gauge = i2c_slave_create_simple(bus, TYPE_HTCLEO_GAUGE, 0x26);
        object_property_add_alias(OBJECT(machine), "battery-capacity",
                                  OBJECT(s->gauge), "capacity");
        dev = DEVICE(i2c_slave_create_simple(bus, TYPE_HTCLEO_AKM, 0x1C));
        qdev_connect_gpio_out_named(dev, "irq", 0,
                                    qemu_allocate_irq(htcleo_akm_irq, s, 0));
    }

    /* Buttons and touchscreen */
    s->kbd = qemu_input_handler_register(NULL, &htcleo_keyboard);
    s->touch = qemu_input_handler_register(NULL, &htcleo_touchscreen);

    /* RAM console tap on serial1 (if given) */
    if (serial_hd(1)) {
        qemu_chr_fe_init(&s->ramcon_fe, serial_hd(1), &error_fatal);
        s->ramcon_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, htcleo_ramcon_poll,
                                       s);
        timer_mod(s->ramcon_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
}

static void htcleo_get_panel(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    HtcleoMachineState *s = HTCLEO_MACHINE(obj);
    char *str = g_strdup(s->panel_type == 1 ? "auo" : "sharp");
    visit_type_str(v, name, &str, errp);
    g_free(str);
}

static void htcleo_set_panel(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    HtcleoMachineState *s = HTCLEO_MACHINE(obj);
    char *str;

    if (!visit_type_str(v, name, &str, errp)) {
        return;
    }
    if (!strcmp(str, "auo")) {
        s->panel_type = 1;
    } else if (!strcmp(str, "sharp")) {
        s->panel_type = 10;
    } else {
        error_setg(errp, "panel must be 'sharp' or 'auo'");
    }
    g_free(str);
}

static char *htcleo_get_cable(Object *obj, Error **errp)
{
    return g_strdup(cable_names[HTCLEO_MACHINE(obj)->cable]);
}

static void htcleo_set_cable(Object *obj, const char *v, Error **errp)
{
    HtcleoMachineState *s = HTCLEO_MACHINE(obj);

    for (int i = 0; i < ARRAY_SIZE(cable_names); i++) {
        if (!strcmp(v, cable_names[i])) {
            bool changed = s->cable != i;
            s->cable = i;
            /* at run time: the PMIC / the modem tell the OS */
            if (changed && s->gpio) {
                htcleo_set_vbus(s, true);
            }
            return;
        }
    }
    error_setg(errp, "cable must be none, usb or ac");
}

static char *htcleo_get_modem(Object *obj, Error **errp)
{
    return g_strdup(HTCLEO_MACHINE(obj)->modem_lle ? "lle" : "hle");
}

static void htcleo_set_modem(Object *obj, const char *v, Error **errp)
{
    HtcleoMachineState *s = HTCLEO_MACHINE(obj);

    if (!strcmp(v, "lle")) {
        s->modem_lle = true;
    } else if (!strcmp(v, "hle")) {
        s->modem_lle = false;
    } else {
        error_setg(errp, "baseband must be 'hle' or 'lle'");
    }
}

static char *htcleo_get_modem_fw(Object *obj, Error **errp)
{
    return g_strdup(HTCLEO_MACHINE(obj)->modem_fw);
}

static void htcleo_set_modem_fw(Object *obj, const char *v, Error **errp)
{
    HtcleoMachineState *s = HTCLEO_MACHINE(obj);

    g_free(s->modem_fw);
    s->modem_fw = g_strdup(v);
}

static bool htcleo_get_modem_trace(Object *obj, Error **errp)
{
    return HTCLEO_MACHINE(obj)->modem_trace;
}

static void htcleo_set_modem_trace(Object *obj, bool v, Error **errp)
{
    HTCLEO_MACHINE(obj)->modem_trace = v;
}

static void htcleo_instance_init(Object *obj)
{
    HTCLEO_MACHINE(obj)->panel_type = 10;
    HTCLEO_MACHINE(obj)->cable = CABLE_USB;     /* the adb cable */
}

static void htcleo_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "HTC HD2 / HTC Leo (Qualcomm QSD8250 Snapdragon)";
    object_class_property_add(oc, "panel", "str", htcleo_get_panel,
                              htcleo_set_panel, NULL, NULL);
    object_class_property_set_description(oc, "panel",
                                          "LCD panel fitted: sharp or auo");
    object_class_property_add_str(oc, "cable", htcleo_get_cable,
                                  htcleo_set_cable);
    object_class_property_set_description(oc, "cable",
        "what is plugged into the USB port: none, usb (a PC) or ac (a charger);"
        " can be changed at run time with qom-set /machine cable ...");
    object_class_property_add_str(oc, "baseband", htcleo_get_modem,
                                  htcleo_set_modem);
    object_class_property_set_description(oc, "baseband",
        "baseband: hle (modelled AMSS services) or lle (real AMSS firmware "
        "on an ARM926 modem core, see baseband-firmware)");
    object_class_property_add_str(oc, "baseband-firmware", htcleo_get_modem_fw,
                                  htcleo_set_modem_fw);
    object_class_property_set_description(oc, "baseband-firmware",
        "AMSS ELF for baseband=lle (tools/hd2_radio.py extracts it)");
    object_class_property_add_bool(oc, "baseband-trace", htcleo_get_modem_trace,
                                   htcleo_set_modem_trace);
    object_class_property_set_description(oc, "baseband-trace",
        "log the modem core's peripheral accesses (baseband=lle)");
    mc->init = htcleo_init;
    mc->reset = htcleo_reset;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a8");
    mc->default_ram_size = HTCLEO_RAM_SIZE;
    /* the apps core plus the modem core (baseband=lle) */
    mc->max_cpus = 2;
    mc->default_cpus = 2;
    mc->default_ram_id = "htcleo.ram";
    mc->ignore_memory_transaction_failures = true;
    mc->block_default_type = IF_MTD;
    mc->no_cdrom = true;
    mc->no_floppy = true;
    mc->no_parallel = true;
}

static const TypeInfo htcleo_machine_type = {
    .name = TYPE_HTCLEO_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(HtcleoMachineState),
    .instance_init = htcleo_instance_init,
    .class_init = htcleo_machine_class_init,
    .interfaces = arm_machine_interfaces,
};

static void htcleo_register(void)
{
    type_register_static(&htcleo_machine_type);
}
type_init(htcleo_register)
