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
#include "hw/core/qdev-properties-system.h"
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
#include "exec/cputlb.h"
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
    bool q6_lle;                /* the QDSP6 runs its firmware (qdsp6=lle) */
    char *q6_link_id;           /* chardev to the qemu-system-hexagon one */
    DeviceState *q6_link;
    bool usb_lle;               /* the gadget goes on this host's bus (usb=lle) */
    char *usb_ip_socket;        /* ... over this unix socket, if given */
    CPUClusterState apps_cluster;
    DeviceState *lle;
    gchar *fw_data;             /* -bios image, copied to RAM on reset */
    gsize fw_len;
    char *spl_path;             /* -M spl=<file>: run the real HSPL */
    char *keys_held;            /* -M keys=...: held down from reset       */
    uint8_t *hspl;              /* the unwrapped HSPL image */
    gsize hspl_len;
    bool hspl_run;              /* the HSPL is booting (no QEMU hand-off) */

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
        { .name = "SCORPION_C11", .cp = 15, .crn = 11, .crm = CP_ANY,
          .opc1 = CP_ANY, .opc2 = CP_ANY, .access = PL1_RW,
          .type = ARM_CP_CONST | ARM_CP_OVERRIDE, .resetvalue = 0 },
        /*
         * DACR (c3).  From ARMv6 the access permissions live in the L1
         * descriptor and DACR is RAZ/WI: every domain is a client.  The HSPL
         * still zeroes it while it is setting the MMU up
         * (hspl.bin 0x9500117c, right before it installs VBAR), and on a
         * Scorpion that write goes nowhere; in a model that honours DACR it
         * takes effect at once, denies every domain and the very next
         * instruction fetch aborts, so the board never gets off the ground.
         */
        { .name = "SCORPION_DACR", .cp = 15, .crn = 3, .crm = 0,
          .opc1 = 0, .opc2 = 0, .access = PL1_RW,
          .type = ARM_CP_CONST | ARM_CP_OVERRIDE, .resetvalue = 0 },
        { .name = "SCORPION_DBGPRSR", .cp = 14, .crn = 1, .crm = 5,
          .opc1 = 0, .opc2 = 4, .access = PL1_RW,
          .type = ARM_CP_CONST | ARM_CP_OVERRIDE, .resetvalue = 0 },
    };
    for (size_t i = 0; i < ARRAY_SIZE(regs); i++) {
        define_one_arm_cp_reg(cpu, &regs[i]);
    }
    /*
     * The whole of CP15 c9 is implementation-defined (ARMv7 moved every
     * architectural cache operation to c7), and on Scorpion Qualcomm filled
     * it with its cache and DMA controls.  The HSPL walks all of it while
     * it brings the caches up, and an unassigned encoding there is an
     * undefined-instruction exception, which it has no vector for: it takes
     * the one at 0x4, runs into the zeros next to its own code and dies.
     * So claim all of c9, which is what a real Scorpion would answer.
     *
     * c9 is opc1 0 with CRm selecting the control, so CRn stays 9.  Varying
     * CRn here instead would shadow c0..c15 - c1, the SCTLR, among them -
     * with a constant zero, and the HSPL's MMU-off trampoline
     * (mrc/mcr p15,0,r1,c1,c0,0) would read zero and the write be dropped.
     */
    for (int crm = 0; crm < 16; crm++) {
        ARMCPRegInfo c9 = {
            .name = g_strdup_printf("SCORPION_C9_%d", crm), .cp = 15,
            .crn = 9, .crm = crm, .opc1 = CP_ANY, .opc2 = CP_ANY,
            .access = PL1_RW, .type = ARM_CP_CONST | ARM_CP_OVERRIDE,
            .resetvalue = 0,
        };
        define_one_arm_cp_reg(cpu, &c9);
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

#define NAND_PAGE_RAW        2112
#define NAND_PAGES_PER_BLOCK 64
#define HTCLEO_SPL_BLOCK    0x20a  /* HSPL, one 2K sector per page */

static bool nand_read_page_data(BlockBackend *blk, uint32_t page,
                               uint8_t *out);

/* ------------------------------------------------------------------ */
/* HSPL, the HTC secure bootloader                                    */
/* ------------------------------------------------------------------ */

/*
 * A phone starts at the boot ROM, which loads the HSPL out of NAND into the
 * APPSBL window at physical 0 and jumps to it.  The ROM is masked ROM we do
 * not have, so this machine takes the HSPL straight from a file and drops it
 * where the ROM would (spl=<file>).  The HSPL then does what it does on the
 * phone: bring the clocks up, identify the NAND, build its bad block table,
 * read the panel and the EEPROM over I2C, DMOV the OS image from NAND into
 * RAM and jump to it.  Nothing of that is faked any more.
 *
 * The HSPL is entered at physical 0 with the MMU off.  It reads the NAND,
 * builds its own page tables from the section map at 0x950012f8 (VA
 * 0x95000000 -> PA 0x0, 1 MB, among the rest), installs them as TTBR0, turns
 * the MMU on and carries on - all without help from us.
 *
 * spl=nand takes the HSPL out of the NAND instead of a file, so the whole
 * chain comes from the NAND image; hd2_mknand.py --spl flashes it at block
 * 0x20a, one 2048-byte sector per page, which is where the boot ROM looks.
 */

#define HSPL_R000FF_MARK   "\x00\x01\x00\x80\x00\x00\x00\x01"
#define HSPL_R000FF_HDR    0x20            /* header, ends with the marker */
#define HSPL_R000FF_BLOCK  0x10000         /* payload block */
#define HSPL_R000FF_SIG    0x80            /* signature blob at each seam */
#define HSPL_R000FF_SEAM   (HSPL_R000FF_SIG + 1 + 8)

/*
 * HTC ships its bootloaders wrapped in an "R000FF" container: the 0x20-byte
 * header, then the payload in 64 KiB blocks.  Every seam between blocks is
 * marked by a 0x89-byte signature blob (128 bytes of signature, one length
 * byte, and the same 8-byte marker the header ends with).  Strip it and the
 * flat image is what the boot ROM copies to physical 0.
 *
 * Returns a g_malloc'd copy of the payload, or NULL if this is not a wrapped
 * image (a plain dump of the running phone's memory, for instance).
 */
static uint8_t *htcleo_hspl_unwrap(const uint8_t *img, size_t len,
                                   size_t *out_len)
{
    gsize pos, plain_len = 0;
    uint8_t *out;
    int seams = 0;

    if (len < HSPL_R000FF_HDR ||
        memcmp(img + 0x18, HSPL_R000FF_MARK, 8) != 0) {
        return NULL;
    }

    for (pos = HSPL_R000FF_HDR; pos < len; pos += HSPL_R000FF_BLOCK) {
        plain_len += MIN(HSPL_R000FF_BLOCK, len - pos);
        if (pos + HSPL_R000FF_BLOCK + HSPL_R000FF_SEAM >= len ||
            memcmp(img + pos + HSPL_R000FF_BLOCK + HSPL_R000FF_SIG + 1,
                   HSPL_R000FF_MARK, 8) != 0) {
            break;                          /* last block: no seam follows */
        }
        seams++;
        pos += HSPL_R000FF_SEAM;
    }

    out = g_malloc(plain_len);
    for (gsize src = HSPL_R000FF_HDR, dst = 0; dst < plain_len; ) {
        gsize n = MIN(HSPL_R000FF_BLOCK, plain_len - dst);

        memcpy(out + dst, img + src, n);
        src += n;
        dst += n;
        pos += HSPL_R000FF_SEAM;
    }
    info_report("htcleo: HSPL: unwrapped R000FF image (%d signature blobs, "
                "%zu bytes)", seams, plain_len);
    *out_len = plain_len;
    return out;
}

static bool htcleo_load_hspl(HtcleoMachineState *s)
{
    gchar *raw = NULL;
    gsize raw_len = 0;
    GError *err = NULL;

    if (!s->spl_path) {
        return false;
    }
    if (!g_file_get_contents(s->spl_path, &raw, &raw_len, &err)) {
        error_report("htcleo: cannot read the HSPL image '%s': %s",
                     s->spl_path, err->message);
        g_error_free(err);
        exit(1);
    }
    s->hspl = htcleo_hspl_unwrap((const uint8_t *)raw, raw_len, &s->hspl_len);
    if (!s->hspl) {
        s->hspl = (uint8_t *)raw;            /* already a flat image */
        s->hspl_len = raw_len;
        raw = NULL;
    }
    g_free(raw);
    info_report("htcleo: HSPL: %zu bytes at physical 0x%x", s->hspl_len,
                HTCLEO_HSPL_PHYS);
    return true;
}

/* ------------------------------------------------------------------ */
/* SPL hand-off                                                        */
/* ------------------------------------------------------------------ */

/*
 * Read the HSPL the way the boot ROM does: out of the NAND, one 2048-byte
 * sector per page, from HTCLEO_SPL_BLOCK up to the first erased one.  This
 * is what a flashed phone has -- hd2_mknand.py --spl puts it there -- so with
 * spl=nand the whole chain, PBL included, comes out of the NAND image rather
 * than a side-car file.  Returns false when nothing is flashed there.
 */
static bool htcleo_load_hspl_from_nand(HtcleoMachineState *s, BlockBackend *blk)
{
    gsize len = 0;
    uint32_t base = HTCLEO_SPL_BLOCK * NAND_PAGES_PER_BLOCK;
    uint8_t *img;

    if (!blk) {
        return false;
    }
    for (uint32_t i = 0; i < 1024; i++) {   /* up to 2 MiB of HSPL */
        uint8_t page[2048];
        bool erased = true;

        if (!nand_read_page_data(blk, base + i, page)) {
            break;
        }
        for (int b = 0; b < 2048; b++) {
            if (page[b] != 0xff) {         /* end of the flashed image */
                erased = false;
                break;
            }
        }
        if (erased) {
            break;
        }
        len += 2048;
    }
    if (len < 4) {
        return false;
    }
    img = g_malloc(len);
    for (uint32_t i = 0; i < len / 2048; i++) {
        if (!nand_read_page_data(blk, base + i, img + i * 2048)) {
            g_free(img);
            return false;
        }
    }
    s->hspl = htcleo_hspl_unwrap(img, len, &s->hspl_len);
    if (!s->hspl) {
        s->hspl = img;                      /* already a flat image */
        s->hspl_len = len;
    }
    info_report("htcleo: HSPL: %zu bytes from NAND block 0x%x -> "
                "physical 0x%x", s->hspl_len, HTCLEO_SPL_BLOCK,
                HTCLEO_HSPL_PHYS);
    return true;
}


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
 * the image is linked to run (MAGLDR 1.13: 0x28000000; Windows Mobile:
 * virtual 0x80000000).
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
    if ((pa & 0xE0000000) == 0x80000000 && !(pa & 0xFFF)) {
        /*
         * A stock Windows Mobile kernel is linked at cached virtual
         * addresses; its OEMAddressTable maps 0x80000000 to the start of
         * EBI1 bank 0, where the SPL copies it.
         */
        return HTCLEO_KERNEL_BASE + (pa - 0x80000000);
    }
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
    int p;

    if (!blk || !nand_read_page_data(blk, base, page)) {
        return false;
    }
    /*
     * WinCE MBR: the XIP partition (type 0x23) holds the OS image.  It is
     * partition 0 for cLK/MAGLDR; a stock ROM puts ULDR (0x20) first.
     */
    if (page[0] != 0xE9 || page[1] != 0xFD || page[2] != 0xFF ||
        page[510] != 0x55 || page[511] != 0xAA) {
        return false;
    }
    for (p = 0; p < 4 && page[0x1BE + 16 * p + 4] != 0x23; p++) {
    }
    if (p == 4) {
        return false;
    }
    start = ldl_le_p(page + 0x1BE + 16 * p + 8);
    count = ldl_le_p(page + 0x1BE + 16 * p + 12);
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

static void htcleo_sd_detect(void *opaque, int n, int level)
{
    HtcleoMachineState *s = opaque;

    if (s->gpio) {
        msm_gpio_set_input(s->gpio, HTCLEO_GPIO_SD_STATUS, level);
    }
}

/*
 * Hand the CPU to the HSPL.  This is what the boot ROM does, and it is more
 * than "drop the image at 0 and jump": the ROM leaves the core running with
 * the MMU on, with the secure alias of the APPSBL window mapped so the HSPL
 * executes at VA 0x95000000 out of physical 0.  The HSPL needs that, because
 * its very first act after the reset stub is to read one of its own literals
 * through VA 0x9501df3c (the address it jumps to once it has rebuilt the
 * page tables for real), before it enables the MMU itself.
 *
 * The map below is the one the ROM would leave: RAM identity-mapped for the
 * low window the HSPL keeps its globals and stack in (PA 0x80000, sp 0xff000),
 * plus the 4 MB secure alias at 0x95000000 that the HSPL itself later maps as
 * a 1 MB section on physical 0.  The HSPL then replaces all of it.
 */
/* 16 KiB aligned: TTBR0's low 14 bits are RES0 and get masked off */
#define HSPL_PBL_TTBR  0x002e8000           /* below the HSPL's own tables */
#define HTCLEO_MSC_ALIAS_PA 0x3a000000     /* RAM for the modem alias */
#define HSPL_GLOBALS_L2 0x00000400         /* where the HSPL expects its
                                         * coarse table for 0xb5080000 */
#define HSPL_GLOBALS_VA 0xb5080000          /* the boot ROM's globals window */
#define HSPL_GLOBALS_PA 0x00080000
#define HSPL_GLOBALS_PAGES 256              /* 256 pages: the whole 1 MB */

/*
 * The map the boot ROM leaves behind, and the one the HSPL itself builds when
 * it gets that far: its section table, the records at 0x950012f8 in the
 * image -- virtual, physical, size in MB, terminated by a zero record.  Rather
 * than hard-code the eight-odd RAM windows and the forty-odd peripheral
 * windows it lists, read them back out of the image and write each one as a
 * 4 MB short-format section descriptor, which is what its own page table
 * builder (hspl.c, 0x9501dd2c) does with the very same records.
 *
 * The PMEM window is the one that catches people out: the HSPL's stack top is
 * the constant 0x97a35ffc (hspl.c, 0x9505e60c), 10.7 MB into that window, so
 * it lands on physical 0x2f35ffc.  Map that window identity and VA
 * 0x97a35ffc falls past the end of RAM, where the writes are quietly dropped
 * and the stack reads back as zeroes.
 */
#define HSPL_SECTIONS   0x12f8               /* the table, inside the image */

#define HSPL_SECTION_LEN 12                  /* three little-endian words */

static void htcleo_hspl_boot(HtcleoMachineState *s, CPUARMState *env)
{
    uint32_t *l1 = g_new0(uint32_t, 4096);
    uint32_t sec, off;

    if (s->hspl_len > HTCLEO_RAM_SIZE) {
        error_report("htcleo: HSPL image is %zu bytes, larger than RAM",
                     s->hspl_len);
        exit(1);
    }

    physical_memory_write(HTCLEO_HSPL_PHYS, s->hspl, s->hspl_len);

    /*
     * The low RAM window, identity: the HSPL's stack pointer, its own
     * globals and the vectors it installs all sit below 4 MB, long before it
     * reaches the section table.
     */
    for (sec = 0; sec < 0x100; sec++) {
        l1[sec] = cpu_to_le32((sec << 20) | 0xC02);
    }
    /*
     * The peripheral windows at their own addresses: the HSPL reads the
     * hardware out of its discovery table (TCSR at 0xa8700000, I2C at
     * 0xa9900000, ...) through the MMU before it installs its own window.
     */
    for (sec = 0xa00; sec < 0xb00; sec++) {
        l1[sec] = cpu_to_le32((sec << 20) | 0xC02);
    }

    for (off = HSPL_SECTIONS; off + HSPL_SECTION_LEN <= s->hspl_len;
         off += HSPL_SECTION_LEN) {
        uint32_t va = ldl_le_p(s->hspl + off);
        uint32_t pa = ldl_le_p(s->hspl + off + 4);
        uint32_t mb = ldl_le_p(s->hspl + off + 8);

        if (!va && !pa && !mb) {
            break;                              /* the terminator */
        }
        if (!mb || ((va >> 20) + mb) >= 4096) {
            continue;                           /* not one of ours */
        }
        for (sec = 0; sec < mb; sec++) {
            /*
             * Short-format section descriptor: bits[1:0] = 0b10 says
             * "section", bits[11:10] = 0b11 gives full access, domain 0.
             * Without the 0b10 the walk faults at the first fetch.
             */
            l1[(va >> 20) + sec] =
                cpu_to_le32((pa & 0xfff00000) + sec * 0x100000 | 0xC02);
        }
    }
    /*
     * The window the HSPL ends up running from: VA 0x95000000 -> physical 0.
     * It builds this entry itself (hspl.bin 0x9501deb0) once it turns the MMU
     * on; it is here too so that the table describes the same machine the HSPL
     * builds one instruction later.
     */
    l1[HTCLEO_HSPL_ALIAS_VA >> 20] = cpu_to_le32(HTCLEO_HSPL_PHYS | 0xC02);

    /*
     * The globals window, VA 0xb5080000, which lands on the board's globals
     * at PA 0x80000: the HSPL reads and writes the board ID and the panel
     * type there, and every reference it makes carries the same +0x1030
     * offset the Linux board file reads them back from
     * (board-htcleo-panel.c).
     *
     * Put it exactly where the HSPL's own page table builder expects it: it
     * installs L1[b50] = 0x402, a coarse table based at PA 0x400 (hspl.c,
     * FUN_9501dd2c), and then leaves the entries alone, because on the phone
     * the boot ROM had already filled them in.  Writing them there means our
     * table and the HSPL's own walk the very same descriptors.
     *
     * Only the two 4 KB pages the HSPL actually names are filled: PA 0x400
     * is the HSPL image's own zero padding, and 0x1000 onwards is its code.
     */
    {
        static const uint32_t pages[] = { 0xb5081000, 0xb5082000 };

        for (size_t n = 0; n < ARRAY_SIZE(pages); n++) {
            uint32_t pa = HSPL_GLOBALS_PA + (pages[n] - HSPL_GLOBALS_VA);
            uint32_t desc = cpu_to_le32((pa & 0xfffff000) | 0xC02);

            physical_memory_write(HSPL_GLOBALS_L2 +
                                  (((pages[n] >> 12) & 0x3ff) * 4),
                                  &desc, sizeof(desc));
        }
        l1[HSPL_GLOBALS_VA >> 20] = cpu_to_le32(HSPL_GLOBALS_L2 | 0x402);
    }

    /*
     * The RAM alias, VA 0xa0000000..0xbfffffff: a second copy of the whole
     * 0x80000000..0x9fffffff window, one bit of the L1 index apart.  hspl.c's
     * page table builder walks the section list twice for it (FUN_9501dd2c,
     * the do/while that runs puVar5 from puVar1 + 0x800 to puVar1 + 0xa00),
     * and the table it builds at 0x2ec0000 shows the result: L1[a00 + n] is
     * L1[800 + n] with the access bits changed from 0x40e to 0x402.
     *
     * It matters, because the HSPL addresses its own data through the alias.
     * The DMOV descriptor list it builds for its first NAND transfer lives at
     * VA 0xbc1fe38, which the alias puts at physical 0x2e1fe38 -- and that is
     * exactly the pointer it then hands to the ADM in CMD_PTR.  Map it to
     * anything else and the descriptor is written to one place and read from
     * another, so the data mover sees an empty list and reports the transfer
     * as failed, over and over.
     */
    for (sec = 0x800; sec < 0xa00; sec++) {
        if (l1[sec]) {
            l1[sec + 0x200] = cpu_to_le32(ldl_le_p((uint8_t *)l1 + sec * 4) |
                                          0x4000);
        }
    }

    physical_memory_write(HSPL_PBL_TTBR, l1, 4096 * sizeof(uint32_t));
    g_free(l1);

    /*
     * SVC mode, IRQ/FIQ masked, and - the part that matters - entered at
     * *physical* 0 with the MMU off, which is what the boot ROM does.
     *
     * The HSPL rebuilds the tables at 0x2ec0000 itself and, at
     * hspl.bin 0x9501deb0, installs its own window with
     *
     *     mov r1, pc ; lsr r1,#20 ; lsl r1,#20 ; orr r1,r1,#0x40e
     *     str r1, [l1 + ((r1>>18) & ~0xfff)]
     *
     * i.e. L1[the MB its own PC is in] = that MB | 0x40e.  It only comes
     * out right if the core is executing at physical 0 when it does that,
     * which makes the HSPL reachable at both VA 0x00000000 and
     * VA 0x95000000 (the latter is its own section-table entry) and leaves
     * VA 0x1e200 - the form FUN_9505e5ec() produces by adding 0x6b000000 to
     * a code address - pointing at the same instructions.  Enter it at
     * VA 0x95000000 with the MMU on, as this used to, and that instruction
     * stores L1[0x950] = 0x9500040e: the HSPL maps itself onto physical
     * 0x95000000, which is not RAM, and the very next branch walks into
     * zeroes.
     */
    env->regs[15] = HTCLEO_HSPL_PHYS;
    env->thumb = false;
    env->cp15.cpacr_el1 &= ~0xF00000;

    env->cp15.ttbr0_el[1] = HSPL_PBL_TTBR;
    env->cp15.ttbr1_el[1] = 0;
    env->cp15.tcr_el[1] = 0;               /* 4 KB pages, one translation reg */
    env->cp15.sctlr_el[1] &= ~(SCTLR_M | 0x3FFFFF);
    env->cp15.sctlr_el[1] |= SCTLR_C;     /* caches on, MMU off            */
    /* every domain a client; from ARMv6 the DACR is RAZ/WI anyway */
    env->cp15.dacr_ns = 0x55555555;
    env->cp15.vbar_ns = 0;
    arm_rebuild_hflags(env);

    /*
     * The TLB and the translated blocks still hold the state the core came
     * out of reset in (no MMU), so they have to go before the first fetch
     * at 0x95000000 goes through the table we just built.
     */
    tlb_flush(CPU(s->cpu));
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
        if (s->q6_link) {
            /* OSBL starts the QDSP6 once the shared memory is set up */
            msm_q6_link_boot(s->q6_link);
        }
        if (!s->q6_lle) {
            msm_modem_qdsp6_boot(MSM_MODEM(s->modem));
        }
    } else {
        /* AMSS has been running since power-on and owns shared memory */
        msm_modem_boot(MSM_MODEM(s->modem));
    }
    htcleo_set_vbus(s, true);

    /*
     * The keys have to be on the matrix before anything runs: the HSPL reads
     * it as one of its first acts after reset, and the running system's own
     * input handler is not even installed yet.
     */
    htcleo_update_keys(s);

    if (s->spl_path && !strcmp(s->spl_path, "nand")) {
        /* spl=nand: the boot ROM reads the HSPL out of the NAND */
        g_free(s->hspl);
        s->hspl = NULL;
        s->hspl_len = 0;
        s->hspl_run = htcleo_load_hspl_from_nand(s, blk);
        if (!s->hspl_run) {
            error_report("htcleo: spl=nand but no HSPL is flashed at NAND "
                         "block 0x%x", HTCLEO_SPL_BLOCK);
            exit(1);
        }
    }

    if (s->hspl_run) {
        /*
         * spl=<file>|nand: the real HSPL boots.  It gets the SoC exactly as
         * the boot ROM leaves it and does the rest itself -- clocks, NAND
         * identify and bad block table, panel and EEPROM over I2C, and the
         * DMOV of the OS image out of NAND into RAM, which it ends by
         * jumping to.  None of the hand-off below is needed, so it does not
         * run.
         */
        htcleo_hspl_boot(s, env);
        goto done;
    }

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
        /*
         * Windows Mobile reads these as its driver globals (0x81000).  The
         * SPL also records why the phone powered on at +0x5c: "BUTT"
         * (power button) or "REST" (reset).  Anything else makes the
         * kernel treat the boot as off-mode charging and, with no charger
         * and the power key up, power off again.
         */
        v = cpu_to_le32(0x42555454);        /* 'TTUB' */
        physical_memory_write(0x80000 + 0x105c, &v, 4);
    }

    /* what the SPL leaves configured */
    msm_mdp_setup_scanout(s->mdp, HTCLEO_SPL_FB_BASE, 480, 800, 480 * 2,
                          true);
    htcleo_update_keys(s);
    /*
     * The SPL grants full access to cp10/cp11 (VFP) before it jumps:
     * Windows Mobile writes FPEXC right away and expects that to work.
     */
    env->cp15.cpacr_el1 |= 0xF00000;

    /* SVC mode, IRQ/FIQ masked, MMU off: jump to the OS image */
    env->regs[15] = entry;
    env->thumb = false;

 done:
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

    /*
 * The catch-all for the peripheral windows we do not model.  Reading 0 is what
 * a real device with an idle register does, so a guest can go a long way on
 * it; logging who touches it says which register the model is missing.
 */
static uint64_t htcleo_unclaimed_read(void *opaque, hwaddr off, unsigned size)
{
    uint32_t pc = current_cpu ?
        ((CPUARMState *)cpu_env(current_cpu))->regs[15] : 0;

    qemu_log_mask(LOG_UNIMP, "msm-periph: read 0x%08" HWADDR_PRIx "\n",
                  0xA0000000ULL + off);
    qemu_log_mask(LOG_UNIMP,
                  "msm-periph: read 0x%08" HWADDR_PRIx " at pc %08x\n",
                  0xA0000000ULL + off, pc);
    return 0;
}

static void htcleo_unclaimed_write(void *opaque, hwaddr off, uint64_t val,
                                   unsigned size)
{
    uint32_t pc = current_cpu ?
        ((CPUARMState *)cpu_env(current_cpu))->regs[15] : 0;

    qemu_log_mask(LOG_UNIMP,
                  "msm-periph: write 0x%08" HWADDR_PRIx " = 0x%08" PRIx64 "\n",
                  0xA0000000ULL + off, val);
    qemu_log_mask(LOG_UNIMP,
                  "msm-periph: write 0x%08" HWADDR_PRIx " at pc %08x\n",
                  0xA0000000ULL + off, pc);
}

static const MemoryRegionOps htcleo_unclaimed_ops = {
    .read = htcleo_unclaimed_read,
    .write = htcleo_unclaimed_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

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

    /*
     * spl=<file>|nand: run the phone's own HSPL rather than handing off
     * ourselves.  "nand" means the boot ROM pulls it out of the NAND, which
     * only the reset handler can do (it needs the drive), so it is left for
     * there; any other value is a file we can read now.
     */
    s->hspl_run = s->spl_path && strcmp(s->spl_path, "nand")
                  ? htcleo_load_hspl(s) : (s->spl_path != NULL);
    if (s->hspl_run && s->fw_data) {
        error_report("htcleo: spl= and -bios are both given; the HSPL loads "
                     "the OS image from NAND itself");
        exit(1);
    }

    /* CPU: the apps core (Scorpion), cluster 0 */
    object_initialize_child(OBJECT(machine), "apps-cluster", &s->apps_cluster,
                            TYPE_CPU_CLUSTER);
    qdev_prop_set_uint32(DEVICE(&s->apps_cluster), "cluster-id", 0);
    cpuobj = object_new(machine->cpu_type);
    object_property_add_child(OBJECT(&s->apps_cluster), "cpu", cpuobj);
    object_unref(cpuobj);
    object_property_set_int(cpuobj, "midr", 0x510F00F2, &error_fatal);
    /*
     * Scorpion's caches, not Cortex-A8's: 32-byte lines (CTR) and a 32 KiB
     * 4-way L1 data cache (CCSIDR).  Linux strides its maintenance by the
     * CTR line, and the htcleo kernel aligns DMA buffers to 32 bytes.
     */
    ARM_CPU(cpuobj)->ctr = 0x8003c003;
    ARM_CPU(cpuobj)->ccsidr[0] = 0x701fe019;
    if (object_property_find(cpuobj, "has_el3")) {
        object_property_set_bool(cpuobj, "has_el3", false, &error_fatal);
    }
    qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
    qdev_realize(DEVICE(&s->apps_cluster), NULL, &error_fatal);
    s->cpu = ARM_CPU(cpuobj);
    scorpion_add_cpregs(s->cpu);

    /* Memory: SMI + EBI1 + PMEM pools are one flat 1 GiB space */
    memory_region_add_subregion(sysmem, HTCLEO_RAM_BASE, machine->ram);

    /* qdsp6=lle: the QDSP6 runs in qemu-system-hexagon -M htcleo-q6 */
    if (s->q6_lle) {
        Chardev *chr = s->q6_link_id ? qemu_chr_find(s->q6_link_id) : NULL;

        if (!chr) {
            error_report("qdsp6=lle needs qdsp6-link=<chardev id>");
            exit(1);
        }
        s->q6_link = qdev_new(TYPE_MSM_Q6_LINK);
        object_property_add_child(OBJECT(machine), "q6-link",
                                  OBJECT(s->q6_link));
        qdev_prop_set_chr(s->q6_link, "chardev", chr);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(s->q6_link), &error_fatal);
    }

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
    {
        MemoryRegion *mr = g_new(MemoryRegion, 1);

        memory_region_init_io(mr, OBJECT(s), &htcleo_unclaimed_ops, s,
                              "msm-periph", 0x10000000);
        memory_region_add_subregion(sysmem, 0xA0000000, mr);
    }

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
    msm_csr_set_modem(DEVICE(s->csr), DEVICE(s->modem));
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
            } else if (i == 8 && s->q6_link) {
                /* A2M 8 rings the QDSP6, which runs on its own */
                qdev_connect_gpio_out_named(s->csr, "a2m", i,
                        qdev_get_gpio_in_named(s->q6_link, "to-q6", 0));
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
        if (s->lle && s->q6_link && i == M2A_LLE_Q6) {
            continue;
        }
        qdev_connect_gpio_out_named(s->lle ? s->lle : s->modem, "m2a", i,
                                    vic_in[INT_A9_M2A_0 + i]);
    }
    if (s->q6_link) {
        qdev_connect_gpio_out_named(s->q6_link, "from-q6", 0,
                                    vic_in[INT_ADSP_A11]);
        if (s->lle) {
            /* AMSS rings the QDSP6 ("SPSS2ADSP_INT") with outgoing 3 */
            qdev_connect_gpio_out_named(s->lle, "m2a", M2A_LLE_Q6,
                    qdev_get_gpio_in_named(s->q6_link, "to-q6", 1));
            qdev_connect_gpio_out_named(s->q6_link, "from-q6", 1,
                    qdev_get_gpio_in_named(s->lle, "sub0", MIRQ_SUB0_Q6));
        }
    } else {
        qdev_connect_gpio_out_named(s->modem, "m2a", M2A_DSP,
                                    vic_in[INT_ADSP_A11]);
    }

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
        if (i == 1) {
            /* the microSD slot's card switch, GPIO 153, low with a card */
            qdev_connect_gpio_out_named(dev, "card-detect", 0,
                                        qemu_allocate_irq(htcleo_sd_detect,
                                                          s, 0));
        }
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
    /*
     * usb=lle: the gadget is exposed to this machine through vhci_hcd, or
     * to the app's bridge over a unix socket when usb-ip-socket is given.
     */
    if (s->usb_lle) {
        qdev_prop_set_string(dev, "mode", "lle");
        if (s->usb_ip_socket) {
            qdev_prop_set_string(dev, "usb-ip-socket", s->usb_ip_socket);
        }
    }
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
        if (s->hspl_run) {
            /*
             * Two more chips the HSPL talks to while it brings the panel up
             * and reads the boot configuration, which Linux never probes:
             * without them the controller NAKs the address and the HSPL sits
             * in its retry loop instead of loading the OS.
             */
            htcleo_regdev_create(bus, 0x34, NULL, 0);
            htcleo_regdev_create(bus, 0x5C, NULL, 0);
        }
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

static char *htcleo_get_spl(Object *obj, Error **errp)
{
    return g_strdup(HTCLEO_MACHINE(obj)->spl_path);
}

static void htcleo_set_spl(Object *obj, const char *v, Error **errp)
{
    HtcleoMachineState *s = HTCLEO_MACHINE(obj);

    g_free(s->spl_path);
    s->spl_path = g_strdup(v);
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

static char *htcleo_get_usb(Object *obj, Error **errp)
{
    HtcleoMachineState *s = HTCLEO_MACHINE(obj);

    if (s->usb_ip_socket) {
        return g_strdup("socket");
    }
    return g_strdup(s->usb_lle ? "lle" : "hle");
}

static void htcleo_set_usb(Object *obj, const char *v, Error **errp)
{
    HtcleoMachineState *s = HTCLEO_MACHINE(obj);

    if (!strcmp(v, "lle")) {
        s->usb_lle = true;
    } else if (!strcmp(v, "hle")) {
        s->usb_lle = false;
    } else {
        error_setg(errp, "usb must be 'hle' or 'lle'");
    }
}

static char *htcleo_get_usb_ip(Object *obj, Error **errp)
{
    return g_strdup(HTCLEO_MACHINE(obj)->usb_ip_socket);
}

static void htcleo_set_usb_ip(Object *obj, const char *v, Error **errp)
{
    HTCLEO_MACHINE(obj)->usb_ip_socket = g_strdup(v);
}

static char *htcleo_get_q6(Object *obj, Error **errp)
{
    return g_strdup(HTCLEO_MACHINE(obj)->q6_lle ? "lle" : "hle");
}

static void htcleo_set_q6(Object *obj, const char *v, Error **errp)
{
    if (!strcmp(v, "lle")) {
        HTCLEO_MACHINE(obj)->q6_lle = true;
    } else if (!strcmp(v, "hle")) {
        HTCLEO_MACHINE(obj)->q6_lle = false;
    } else {
        error_setg(errp, "qdsp6 must be hle or lle");
    }
}

static char *htcleo_get_q6_link(Object *obj, Error **errp)
{
    return g_strdup(HTCLEO_MACHINE(obj)->q6_link_id);
}

static void htcleo_set_q6_link(Object *obj, const char *v, Error **errp)
{
    HtcleoMachineState *s = HTCLEO_MACHINE(obj);

    g_free(s->q6_link_id);
    s->q6_link_id = g_strdup(v);
}

static void htcleo_instance_init(Object *obj)
{

    HTCLEO_MACHINE(obj)->panel_type = 10;
    HTCLEO_MACHINE(obj)->cable = CABLE_USB;     /* the adb cable */
}

/*
 * Keys already down when the machine comes up, named the same way tools/keys.py
 * names them: volup, voldown, windows, call, endcall, back, home, power.
 *
 * This is for the bootloader's own key test.  The HSPL decides whether to be
 * the bootloader by reading the matrix a moment after the boot ROM hands it
 * the core, which is long before a key event sent from outside can arrive -
 * so the only way to try that path is for the key to be down at reset.
 */
static void htcleo_set_keys(Object *obj, const char *value, Error **errp)
{
    HtcleoMachineState *s = HTCLEO_MACHINE(obj);
    static const char *const names[HD2_NKEYS] = {
        "volup", "voldown", "windows", "call", "endcall", "back", "home",
        "power"
    };
    char *copy, *tok, *save = NULL;

    g_free(s->keys_held);
    s->keys_held = g_strdup(value);

    memset(s->keys, 0, sizeof(s->keys));
    if (!value || !*value) {
        return;
    }
    copy = g_strdup(value);
    for (tok = strtok_r(copy, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        for (int k = 0; k < HD2_NKEYS; k++) {
            if (!g_strcmp0(tok, names[k])) {
                s->keys[k] = true;
                break;
            }
        }
    }
    g_free(copy);
}

static char *htcleo_get_keys(Object *obj, Error **errp)
{
    return g_strdup(HTCLEO_MACHINE(obj)->keys_held);
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
    object_class_property_add_str(oc, "qdsp6", htcleo_get_q6, htcleo_set_q6);
    object_class_property_set_description(oc, "qdsp6",
        "hle: the multimedia DSP's DAL services modelled (default); lle: its "
        "firmware runs in qemu-system-hexagon -M htcleo-q6 (see qdsp6-link)");
    object_class_property_add_str(oc, "usb-host", htcleo_get_usb,
                                  htcleo_set_usb);
    object_class_property_set_description(oc, "usb-host",
        "who plays USB host: hle (built in, adb is bridged to TCP, the "
        "default), lle (this machine's own stack enumerates the emulated "
        "gadget over vhci_hcd, so lsusb/adb/fastboot see a real device; "
        "needs root and vhci_hcd) or socket (the same USB/IP stream on a "
        "unix socket, for a bridge that owns the host controller - see "
        "usb-ip-socket)");
    object_class_property_add_str(oc, "usb-ip-socket", htcleo_get_usb_ip,
                                  htcleo_set_usb_ip);
    object_class_property_set_description(oc, "usb-ip-socket",
        "a unix socket to speak USB/IP on instead of attaching to vhci_hcd: "
        "the Android app's bridge, which puts the emulated HD2 on the "
        "phone's own USB port through FunctionFS.  Needs usb-host=lle. "
        "QEMU listens, the bridge connects; no root needed here");
    object_class_property_add_str(oc, "qdsp6-link", htcleo_get_q6_link,
                                  htcleo_set_q6_link);
    object_class_property_add_str(oc, "baseband-firmware", htcleo_get_modem_fw,
                                  htcleo_set_modem_fw);
    object_class_property_set_description(oc, "baseband-firmware",
        "AMSS ELF for baseband=lle (tools/hd2_radio.py extracts it)");
    object_class_property_add_str(oc, "spl", htcleo_get_spl, htcleo_set_spl);
    object_class_property_set_description(oc, "spl",
        "the HTC HSPL bootloader to run in lle mode instead of this machine "
        "doing the SPL's last act itself: it is copied to physical 0 (where "
        "the boot ROM puts it) and booted, and it brings up the clocks, "
        "identifies the NAND, reads the panel over I2C and DMOVs the OS "
        "image from NAND before jumping to it.  R000FF-wrapped images are "
        "unwrapped; a raw dump works too.  Without it the machine keeps "
        "loading the OS image from NAND itself");
    object_class_property_add_str(oc, "keys", htcleo_get_keys,
                                  htcleo_set_keys);
    object_class_property_set_description(oc, "keys",
        "keys held down when the machine comes up, comma separated: volup, "
        "voldown, windows, call, endcall, back, home, power.  The HSPL's "
        "own test for becoming the bootloader runs in the first moments "
        "after the boot ROM hands it the core, so a key sent from outside "
        "arrives far too late to affect it");
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
