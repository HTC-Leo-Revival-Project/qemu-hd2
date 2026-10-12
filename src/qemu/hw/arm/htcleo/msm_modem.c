/*
 * HTC HD2 baseband (AMSS on the ARM9) as seen from the apps processor.
 *
 * The QSD8250's modem processor is not emulated instruction by
 * instruction.  Instead this device implements the modem side of every
 * shared-memory protocol the htcleo kernel and cLK use:
 *
 *  - SMEM: seeded from a dump of a real HD2's shared RAM (AMSS 15.42.50.11U)
 *    so the heap table, SMD channel table and version strings are exactly
 *    what the kernel finds on hardware.
 *  - DEX (WinCE proc_comm at SMEM+0xEFE00, doorbell A2M 4): PMIC, vibrator,
 *    audio path and power-off requests.
 *  - proc_comm (SMEM+0x0, doorbell A2M 6).
 *  - SMSM shared state (doorbell A2M 5): boot states plus the N-way DEM
 *    power-collapse and time-sync handshakes from arch/arm/mach-msm/pm.c
 *    and timer.c.
 *  - SMD channels (doorbell A2M 0 for the modem, A2M 8 for the DSP):
 *    channel open/close and data transfer; payload goes to the RPC router
 *    (msm_rpc.c) or is consumed.
 *
 * Modem -> apps interrupts are INT_A9_M2A_0..6.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "system/runstate.h"
#include "system/address-spaces.h"
#include "system/physmem.h"
#include "htcleo.h"
#include "msm_modem.h"

/* SMEM item ids (arch/arm/mach-msm/smd_private.h) */
#define SMEM_CHANNEL_ALLOC_TBL      13
#define SMEM_SMD_BASE_ID            14
#define SMEM_SMEM_SLOW_CLOCK_VALUE  83
#define SMEM_SMSM_SHARED_STATE      85
#define SMEM_SMSM_SLEEP_DELAY       87
#define SMEM_APPS_DEM_SLAVE_DATA    334
#define SMEM_SMD_FIFO_BASE_ID       338

/* proc_comm */
#define PCOM_CMD_IDLE       0
#define PCOM_CMD_DONE       1
#define PCOM_READY          1
#define PCOM_CMD_SUCCESS    3
#define PCOM_CMD_FAIL       4
#define PCOM_RESET_APPS     2
#define PCOM_RESET_CHIP     3
#define PCOM_GET_POWER_ON_STATUS 6
#define PCOM_GET_BATT_LEVEL 8
#define PCOM_CHG_IS_CHARGING 9
#define PCOM_POWER_DOWN     10
#define PCOM_GET_UUID_HIGH  16
#define PCOM_GET_UUID_LOW   17
#define PCOM_GET_HW_ENTROPY 18
#define PCOM_CLKCTL_RPC_RATE 27
#define PCOM_CLKCTL_RPC_ENABLED 29
#define PCOM_GPIO_IN        39
#define PCOM_RESET_CHIP_IMM 42
#define PCOM_GET_MODEM_VERSION 45

/* DEX */
#define DEX_HAS_DATA        0x100
#define DEX_PC_COMMAND      0x00
#define DEX_PC_STATUS       0x04
#define DEX_PC_SERIAL       0x08
#define DEX_PC_SERIAL_CHECK 0x0C
#define DEX_PC_DATA         0x20
#define DEX_PC_DATA_RESULT  0x24
#define DEX_PC_NOTIFY       0x28
#define DEX_PC_DATA2        0x30
#define DEX_PC_DATA_RESULT2 0x34
#define DEX_PC_READY        0x3C
#define DEX_BASE            0xEFE00
#define DEX_POWER_OFF       0x14
#define DEX_NOTIFY_ARM9_REBOOT 0x8E
#define DEX_GET_BATTERY_DATA 0x8A
#define DEX_GET_BATTERY_ID  0x8B
#define DEX_READ_RTC        0x81
#define DEX_AUDIO_CALL      0x1C
#define DEX_AUDIO_AUX_ON    0x51    /* enable_aux_loopback(1): FM radio */
#define DEX_AUDIO_AUX_OFF   0x52

/* SMSM */
#define SMSM_INIT           0x00000001
#define SMSM_OSENTERED      0x00000002
#define SMSM_SMDINIT        0x00000008
#define SMSM_RPCINIT        0x00000020
#define SMSM_RESET          0x00000040
#define SMSM_RUN            0x00000100
#define SMSM_SYSTEM_REBOOT  0x00080000
#define SMSM_SYSTEM_POWER_DOWN 0x00040000

enum {
    SMSM_STATE_APPS,
    SMSM_STATE_MODEM,
    SMSM_STATE_HEXAGON,
    SMSM_STATE_APPS_DEM,
    SMSM_STATE_MODEM_DEM,
    SMSM_STATE_QDSP6_DEM,
    SMSM_STATE_POWER_MASTER_DEM,
    SMSM_STATE_TIME_MASTER_DEM,
    SMSM_STATE_COUNT,
};

#define DEM_MASTER_RUN          0x01
#define DEM_MASTER_RSA          0x02
#define DEM_MASTER_PWRC_EARLY_EXIT 0x04
#define DEM_MASTER_SLEEP_EXIT   0x08
#define DEM_MASTER_READY        0x10
#define DEM_MASTER_SLEEP        0x20

#define DEM_SLAVE_RUN           0x0001
#define DEM_SLAVE_PWRC          0x0002
#define DEM_SLAVE_WFPI          0x0010
#define DEM_SLAVE_SLEEP         0x0020
#define DEM_SLAVE_SLEEP_EXIT    0x0040
#define DEM_SLAVE_PWRC_SUSPEND  0x0200
#define DEM_TIME_SLAVE_TIME_REQUEST 0x0400
#define DEM_TIME_SLAVE_TIME_POLL    0x0800
#define DEM_TIME_SLAVE_TIME_INIT    0x1000
#define DEM_TIME_MASTER_TIME_PENDING_APPS 0x01

/* SMD */
#define SMD_SS_CLOSED       0
#define SMD_SS_OPENING      1
#define SMD_SS_OPENED       2
#define SMD_SS_FLUSHING     3
#define SMD_SS_CLOSING      4
#define SMD_TYPE_MASK       0x0FF
#define SMD_TYPE_APPS_MODEM 0
#define SMD_TYPE_APPS_DSP   1
#define SMD_KIND_MASK       0xF00
#define SMD_KIND_STREAM     0x100
#define SMD_KIND_PACKET     0x200

/* half channel: state, fDSR fCTS fCD fRI fHEAD fTAIL fSTATE fUNUSED, tail, head */
#define HC_STATE    0
#define HC_FDSR     4
#define HC_FCTS     5
#define HC_FCD      6
#define HC_FRI      7
#define HC_FHEAD    8
#define HC_FTAIL    9
#define HC_FSTATE   10
#define HC_TAIL     12
#define HC_HEAD     16
#define HC_SIZE     20

/* ------------------------------------------------------------------ */
/* SMEM accessors                                                      */
/* ------------------------------------------------------------------ */

uint32_t smem_rd(MsmModemState *s, uint32_t off)
{
    return ldl_le_p(s->smem + off);
}

void smem_wr(MsmModemState *s, uint32_t off, uint32_t v)
{
    stl_le_p(s->smem + off, v);
}

static uint8_t smem_rdb(MsmModemState *s, uint32_t off)
{
    return s->smem[off];
}

static void smem_wrb(MsmModemState *s, uint32_t off, uint8_t v)
{
    s->smem[off] = v;
}

/* Offset of SMEM item `id`, or 0 if it has not been allocated */
uint32_t smem_item(MsmModemState *s, unsigned id, uint32_t *size)
{
    uint32_t e = 0xD0 + id * 16;

    if (id >= 512 || !smem_rd(s, e)) {
        return 0;
    }
    if (size) {
        *size = smem_rd(s, e + 8);
    }
    return smem_rd(s, e + 4);
}

static void m2a(MsmModemState *s, int n)
{
    qemu_irq_pulse(s->m2a[n]);
}

/* ------------------------------------------------------------------ */
/* SMSM                                                                */
/* ------------------------------------------------------------------ */

static uint32_t smsm_off(MsmModemState *s, int item)
{
    uint32_t size, off = smem_item(s, SMEM_SMSM_SHARED_STATE, &size);
    if (!off || (unsigned)item * 4 >= size) {
        return 0;
    }
    return off + item * 4;
}

static uint32_t smsm_get(MsmModemState *s, int item)
{
    uint32_t o = smsm_off(s, item);
    return o ? smem_rd(s, o) : 0;
}

static void smsm_set(MsmModemState *s, int item, uint32_t v)
{
    uint32_t o = smsm_off(s, item);
    if (o) {
        smem_wr(s, o, v);
    }
}

static void modem_wake_apps(void *opaque)
{
    MsmModemState *s = opaque;

    /* The apps CPU waits for INT_A9_M2A_6 to come out of power collapse */
    m2a(s, 6);
}

static void smsm_handle(MsmModemState *s)
{
    uint32_t apps = smsm_get(s, SMSM_STATE_APPS);
    uint32_t dem = smsm_get(s, SMSM_STATE_APPS_DEM);
    uint32_t master = smsm_get(s, SMSM_STATE_POWER_MASTER_DEM);
    uint32_t tmaster = smsm_get(s, SMSM_STATE_TIME_MASTER_DEM);
    uint32_t old_master = master, old_tmaster = tmaster;

    if ((apps & (SMSM_SYSTEM_REBOOT | SMSM_SYSTEM_POWER_DOWN)) !=
        (s->last_apps & (SMSM_SYSTEM_REBOOT | SMSM_SYSTEM_POWER_DOWN))) {
        if (apps & SMSM_SYSTEM_REBOOT) {
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        } else if (apps & SMSM_SYSTEM_POWER_DOWN) {
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        }
    }
    s->last_apps = apps;

    /* Power master: answer the apps power-collapse handshake */
    if (dem & (DEM_SLAVE_PWRC | DEM_SLAVE_PWRC_SUSPEND)) {
        if (!(master & DEM_MASTER_RSA)) {
            uint32_t sd_off = smem_item(s, SMEM_APPS_DEM_SLAVE_DATA, NULL);
            uint32_t ticks = sd_off ? smem_rd(s, sd_off) : 0;
            int64_t ns;

            master |= DEM_MASTER_RSA;
            master &= ~DEM_MASTER_RUN;
            /* sleep_time is in 32.768 kHz slow-clock ticks; 0 = forever */
            ns = ticks ? muldiv64(ticks, NANOSECONDS_PER_SECOND, 32768)
                       : 20 * SCALE_MS;
            /*
             * Hardware wakes the apps CPU on any enabled interrupt; this
             * model only wakes it on a timeout, so keep naps short to stay
             * responsive to keys and the touchscreen.
             */
            ns = MIN(ns, 20 * SCALE_MS);
            timer_mod(s->wake_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns);
        }
    } else if (dem & DEM_SLAVE_WFPI) {
        master &= ~DEM_MASTER_RSA;
        master |= DEM_MASTER_RUN;
        timer_del(s->wake_timer);
    } else if (dem & DEM_SLAVE_SLEEP) {
        master |= DEM_MASTER_SLEEP;
        master &= ~DEM_MASTER_SLEEP_EXIT;
    } else if (dem & DEM_SLAVE_SLEEP_EXIT) {
        master |= DEM_MASTER_SLEEP_EXIT;
        master &= ~DEM_MASTER_SLEEP;
    } else if (dem & DEM_SLAVE_RUN) {
        master |= DEM_MASTER_RUN;
        master &= ~(DEM_MASTER_RSA | DEM_MASTER_SLEEP | DEM_MASTER_SLEEP_EXIT);
        timer_del(s->wake_timer);
    }

    /* Time master: serve slow-clock requests (timer.c, N-way variant) */
    if (dem & DEM_TIME_SLAVE_TIME_REQUEST) {
        uint32_t clk = smem_item(s, SMEM_SMEM_SLOW_CLOCK_VALUE, NULL);
        if (clk) {
            uint64_t sclk = muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                     32768, NANOSECONDS_PER_SECOND);
            smem_wr(s, clk, (uint32_t)sclk | 1);
        }
        tmaster |= DEM_TIME_MASTER_TIME_PENDING_APPS;
    } else if (dem & DEM_TIME_SLAVE_TIME_INIT) {
        uint32_t clk = smem_item(s, SMEM_SMEM_SLOW_CLOCK_VALUE, NULL);
        if (clk) {
            smem_wr(s, clk, 0);
        }
        tmaster &= ~DEM_TIME_MASTER_TIME_PENDING_APPS;
    }

    if (master != old_master) {
        smsm_set(s, SMSM_STATE_POWER_MASTER_DEM, master);
    }
    if (tmaster != old_tmaster) {
        smsm_set(s, SMSM_STATE_TIME_MASTER_DEM, tmaster);
    }
    if (master != old_master || tmaster != old_tmaster) {
        m2a(s, 5);
    }
}

/* ------------------------------------------------------------------ */
/* proc_comm                                                           */
/* ------------------------------------------------------------------ */

/*
 * MAGLDR takes the modem core over before it boots an OS from the SD card
 * ("AD SD"): it looks for this Thumb function in AMSS (2.15.50.14 keeps it
 * at 0x9C1742), overwrites its first instructions with a jump to code it
 * copied to 0x1F0000 and has AMSS run it with proc_comm command 88.  That
 * code reports through 0x1E0000 (0x50000: done), turns the modem MMU off
 * and parks the core in a loop in MAGLDR's RAM.  The HLE modem keeps the
 * function where AMSS has it and plays AMSS's part.
 */
#define AMSS_HOOK_PA        0x009C1742
#define AMSS_HOOK_STATUS_PA 0x001E0000
#define PCOM_AMSS_HOOK      88

static const uint8_t amss_hook_fn[16] = {
    0x01, 0x00, 0x01, 0x20, 0x09, 0x1d, 0x09, 0x68,
    0x00, 0x29, 0x00, 0xd1, 0x00, 0x20, 0x70, 0x47,
};

/* movs r0, #imm8; lsls r0, r0, #16; bx r0 */
static bool amss_hook_patched(uint32_t *target)
{
    uint16_t op[3];

    physical_memory_read(AMSS_HOOK_PA, op, sizeof(op));
    op[0] = le16_to_cpu(op[0]);
    op[1] = le16_to_cpu(op[1]);
    op[2] = le16_to_cpu(op[2]);
    if ((op[0] & 0xFF00) != 0x2000 || op[1] != 0x0400 || op[2] != 0x4700) {
        return false;
    }
    *target = (uint32_t)(op[0] & 0xFF) << 16;
    return true;
}

/*
 * The parked core waits in MAGLDR's RAM until the mailbox reads 0x300d300d,
 * stores the entry point MAGLDR left next to it to 0x1E0000 and jumps
 * there: into the AMSS MAGLDR has just loaded from NAND.  The HLE modem
 * plays that cold start by bringing its state back to "AMSS booted".
 */
#define MAGLDR_MAILBOX_PA   0x280b7a70
#define MAGLDR_MAILBOX_GO   0x300d300d

static void modem_park_poll(void *opaque)
{
    MsmModemState *s = opaque;
    uint32_t mb[2];

    if (!s->taken_over) {
        return;
    }
    physical_memory_read(MAGLDR_MAILBOX_PA, mb, sizeof(mb));
    if (le32_to_cpu(mb[0]) != MAGLDR_MAILBOX_GO) {
        timer_mod(s->park_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  SCALE_MS);
        return;
    }
    physical_memory_write(AMSS_HOOK_STATUS_PA, &mb[1], sizeof(mb[1]));
    qemu_log_mask(LOG_UNIMP, "msm-modem: modem core released to 0x%x: "
                  "AMSS restarts\n", le32_to_cpu(mb[1]));
    msm_modem_boot(s);
}

static void modem_take_over(MsmModemState *s, uint32_t target)
{
    uint32_t done = cpu_to_le32(0x50000);

    qemu_log_mask(LOG_UNIMP, "msm-modem: AMSS hijacked, jump to 0x%x: "
                  "the modem core is parked\n", target);
    s->taken_over = true;
    physical_memory_write(AMSS_HOOK_STATUS_PA, &done, sizeof(done));
    timer_mod(s->park_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              SCALE_MS);
}

static void proc_comm_handle(MsmModemState *s)
{
    uint32_t cmd = smem_rd(s, 0x00);
    uint32_t d1 = smem_rd(s, 0x08), d2 = smem_rd(s, 0x0C);
    uint32_t status = PCOM_CMD_SUCCESS;

    if (cmd == PCOM_CMD_IDLE || cmd == PCOM_CMD_DONE) {
        return;
    }
    if (cmd == PCOM_AMSS_HOOK) {
        uint32_t target;

        if (amss_hook_patched(&target)) {
            /* the hooked function never returns to proc_comm */
            modem_take_over(s, target);
            return;
        }
    }
    switch (cmd) {
    case PCOM_RESET_APPS:
    case PCOM_RESET_CHIP:
    case PCOM_RESET_CHIP_IMM:
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        break;
    case PCOM_POWER_DOWN:
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        break;
    case PCOM_GET_POWER_ON_STATUS:
        d1 = 0x2;               /* power key */
        break;
    case PCOM_GET_BATT_LEVEL:
        d1 = 100;
        break;
    case PCOM_CHG_IS_CHARGING:
        d1 = 0;
        break;
    case PCOM_GET_UUID_HIGH:
        d1 = 0x48544320;
        break;
    case PCOM_GET_UUID_LOW:
        d1 = 0x4C454F31;
        break;
    case PCOM_GET_HW_ENTROPY:
        d1 = (uint32_t)g_random_int();
        d2 = (uint32_t)g_random_int();
        break;
    case PCOM_CLKCTL_RPC_ENABLED:
        d1 = 1;
        break;
    case PCOM_CLKCTL_RPC_RATE:
        d1 = 19200000;
        break;
    case PCOM_GET_MODEM_VERSION:
        d1 = 0x1550;
        break;
    default:
        break;
    }
    if (s->trace) {
        qemu_log("modem: proc_comm cmd %u d1 %x d2 %x\n", cmd,
                 smem_rd(s, 0x08), smem_rd(s, 0x0C));
    }
    smem_wr(s, 0x08, d1);
    smem_wr(s, 0x0C, d2);
    smem_wr(s, 0x04, status);
    smem_wr(s, 0x00, PCOM_CMD_DONE);
}

/* ------------------------------------------------------------------ */
/* DEX (WinCE proc_comm flavour)                                       */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Codec aux input loopback                                            */
/* ------------------------------------------------------------------ */

#define AUX_RATE    48000

static void aux_callback(void *opaque, int avail)
{
    MsmModemState *s = opaque;
    int16_t buf[2 * 1024];

    while (avail >= 4 && s->aux_src) {
        int frames = MIN(avail / 4, 1024);
        size_t n;

        s->aux_src(s->aux_opaque, buf, frames);
        n = audio_be_write(s->audio_be, s->aux_voice, buf, frames * 4);
        if (n < (size_t)frames * 4) {
            break;
        }
        avail -= n;
    }
}

static void aux_loopback(MsmModemState *s, bool on)
{
    struct audsettings as = {
        .freq = AUX_RATE,
        .nchannels = 2,
        .fmt = AUDIO_FORMAT_S16,
        .big_endian = false,
    };

    if (!s->audio_be) {
        return;
    }
    if (on && !s->aux_voice) {
        s->aux_voice = audio_be_open_out(s->audio_be, NULL, "htcleo.aux", s,
                                         aux_callback, &as);
    }
    if (s->aux_voice) {
        audio_be_set_active_out(s->audio_be, s->aux_voice, on);
    }
}

void msm_modem_set_aux_source(MsmModemState *s,
                              void (*src)(void *opaque, int16_t *buf,
                                          int frames),
                              void *opaque)
{
    s->aux_src = src;
    s->aux_opaque = opaque;
}

static void dex_handle(MsmModemState *s)
{
    uint32_t b = DEX_BASE;
    uint32_t cmdw = smem_rd(s, b + DEX_PC_COMMAND);
    uint32_t cmd = cmdw & 0xFF;
    uint32_t data = smem_rd(s, b + DEX_PC_DATA);
    uint32_t status = cmd;
    uint32_t r1 = 0, r2 = 0;
    bool has_result = false;

    switch (cmd) {
    case DEX_POWER_OFF:
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        break;
    case DEX_NOTIFY_ARM9_REBOOT:
        break;
    case DEX_READ_RTC:
        r1 = (uint32_t)(qemu_clock_get_ns(QEMU_CLOCK_REALTIME) /
                        NANOSECONDS_PER_SECOND);
        has_result = true;
        break;
    case DEX_GET_BATTERY_DATA:
        r1 = 100;
        has_result = true;
        break;
    case DEX_GET_BATTERY_ID:
        r1 = 1;
        has_result = true;
        break;
    case DEX_AUDIO_CALL:
        if (data == DEX_AUDIO_AUX_ON || data == DEX_AUDIO_AUX_OFF) {
            aux_loopback(s, data == DEX_AUDIO_AUX_ON);
        }
        break;
    default:
        break;
    }
    if (s->trace) {
        qemu_log("modem: dex cmd 0x%02x data %x\n", cmd, data);
    }
    if (has_result) {
        status |= DEX_HAS_DATA;
        smem_wr(s, b + DEX_PC_DATA_RESULT, r1);
        smem_wr(s, b + DEX_PC_DATA_RESULT2, r2);
    }
    smem_wr(s, b + DEX_PC_STATUS, status);
    smem_wr(s, b + DEX_PC_SERIAL_CHECK, smem_rd(s, b + DEX_PC_SERIAL));
}

/* ------------------------------------------------------------------ */
/* SMD                                                                 */
/* ------------------------------------------------------------------ */

static bool smd_channel(MsmModemState *s, int n, SmdChan *c)
{
    uint32_t tbl = smem_item(s, SMEM_CHANNEL_ALLOC_TBL, NULL);
    uint32_t e, fsize;

    if (!tbl || n >= SMD_NUM_CH) {
        return false;
    }
    e = tbl + n * 32;
    if (!smem_rdb(s, e) || !smem_rd(s, e + 28)) {
        return false;
    }
    memcpy(c->name, s->smem + e, 20);
    c->name[20] = 0;
    c->cid = smem_rd(s, e + 20);
    c->ctype = smem_rd(s, e + 24);
    c->shared = smem_item(s, SMEM_SMD_BASE_ID + c->cid, NULL);
    c->fifo = smem_item(s, SMEM_SMD_FIFO_BASE_ID + c->cid, &fsize);
    if (!c->shared || !c->fifo || (fsize & (fsize - 1))) {
        return false;
    }
    c->fifo_size = fsize / 2;
    /* apps writes ch0 / data[0..size), modem writes ch1 / data[size..) */
    c->apps = c->shared;
    c->modem = c->shared + HC_SIZE;
    c->apps_data = c->fifo;
    c->modem_data = c->fifo + c->fifo_size;
    /* same rule as smd_is_packet(): unknown kind -> packet if cid>4 or 1 */
    switch (c->ctype & SMD_KIND_MASK) {
    case SMD_KIND_PACKET:
        c->packet = true;
        break;
    case SMD_KIND_STREAM:
        c->packet = false;
        break;
    default:
        c->packet = c->cid > 4 || c->cid == 1;
        break;
    }
    if (!memcmp(c->name, "DAL", 3)) {
        c->packet = false;
    }
    c->n = n;
    return true;
}

static void smd_set_modem_state(MsmModemState *s, SmdChan *c, uint32_t st)
{
    uint8_t on = (st == SMD_SS_OPENED);

    smem_wrb(s, c->modem + HC_FDSR, on);
    smem_wrb(s, c->modem + HC_FCTS, on);
    smem_wrb(s, c->modem + HC_FCD, on);
    smem_wr(s, c->modem + HC_STATE, st);
    smem_wrb(s, c->modem + HC_FSTATE, 1);
}

/* modem channels interrupt on INT_A9_M2A_0, DSP channels on INT_ADSP_A11 */
static int chan_irq(SmdChan *c)
{
    return (c->ctype & SMD_TYPE_MASK) == SMD_TYPE_APPS_DSP ? M2A_DSP : 0;
}

/* Queue `len` bytes from the modem to the apps; returns bytes written */
int smd_modem_write(MsmModemState *s, SmdChan *c, const void *buf, int len)
{
    uint32_t head = smem_rd(s, c->modem + HC_HEAD);
    uint32_t tail = smem_rd(s, c->modem + HC_TAIL);
    uint32_t mask = c->fifo_size - 1;
    uint32_t avail = mask - ((head - tail) & mask);
    const uint8_t *p = buf;
    int n;

    if ((uint32_t)len > avail) {
        return -1;
    }
    for (n = 0; n < len; n++) {
        s->smem[c->modem_data + ((head + n) & mask)] = p[n];
    }
    smem_wr(s, c->modem + HC_HEAD, (head + len) & mask);
    smem_wrb(s, c->modem + HC_FHEAD, 1);
    return len;
}

/* Send one SMD packet (20-byte header + payload) to the apps */
int smd_modem_send_packet(MsmModemState *s, SmdChan *c, const void *buf,
                          int len)
{
    uint8_t hdr[20] = { 0 };
    uint32_t head = smem_rd(s, c->modem + HC_HEAD);
    uint32_t tail = smem_rd(s, c->modem + HC_TAIL);
    uint32_t mask = c->fifo_size - 1;

    if ((uint32_t)len + 20 > mask - ((head - tail) & mask)) {
        return -1;
    }
    stl_le_p(hdr, len);
    smd_modem_write(s, c, hdr, 20);
    smd_modem_write(s, c, buf, len);
    return len;
}

static void chan_flush(MsmModemState *s, SmdChan *c)
{
    GByteArray *q = s->tx_backlog[c->n];
    uint32_t head, tail, mask, avail, n;

    if (!q->len || smem_rd(s, c->apps + HC_STATE) != SMD_SS_OPENED) {
        return;
    }
    head = smem_rd(s, c->modem + HC_HEAD);
    tail = smem_rd(s, c->modem + HC_TAIL);
    mask = c->fifo_size - 1;
    avail = mask - ((head - tail) & mask);
    n = MIN(avail, q->len);
    if (n) {
        smd_modem_write(s, c, q->data, n);
        g_byte_array_remove_range(q, 0, n);
        smd_kick_apps(s, c);
    }
}

void msm_modem_chan_send(MsmModemState *s, const char *name, const void *buf,
                         int len)
{
    SmdChan c;

    if (!msm_modem_find_channel(s, name, 0, &c)) {
        return;
    }
    g_byte_array_append(s->tx_backlog[c.n], buf, len);
    chan_flush(s, &c);
}

void smd_kick_apps(MsmModemState *s, SmdChan *c)
{
    m2a(s, chan_irq(c));
}

static void smd_consume(MsmModemState *s, SmdChan *c)
{
    uint32_t head = smem_rd(s, c->apps + HC_HEAD);
    uint32_t tail = smem_rd(s, c->apps + HC_TAIL);
    uint32_t mask = c->fifo_size - 1;
    uint32_t n = (head - tail) & mask;
    g_autofree uint8_t *buf = NULL;

    if (!n) {
        return;
    }
    buf = g_malloc(n);
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = s->smem[c->apps_data + ((tail + i) & mask)];
    }
    smem_wr(s, c->apps + HC_TAIL, head);
    smem_wrb(s, c->modem + HC_FTAIL, 1);

    if (c->packet) {
        /* reassemble the 20-byte-header packet stream */
        GByteArray *acc = s->pkt_acc[c->n];
        g_byte_array_append(acc, buf, n);
        while (acc->len >= 20) {
            uint32_t plen = ldl_le_p(acc->data);
            if (acc->len < 20 + plen) {
                break;
            }
            msm_modem_smd_packet(s, c, acc->data + 20, plen);
            g_byte_array_remove_range(acc, 0, 20 + plen);
        }
    } else {
        msm_modem_smd_stream(s, c, buf, n);
    }
}

static void smd_scan(MsmModemState *s, bool dsp)
{
    for (int n = 0; n < SMD_NUM_CH; n++) {
        SmdChan c;
        uint32_t apps_st, modem_st;
        bool kick = false;

        if (!smd_channel(s, n, &c)) {
            continue;
        }
        if (((c.ctype & SMD_TYPE_MASK) == SMD_TYPE_APPS_DSP) != dsp) {
            continue;
        }
        apps_st = smem_rd(s, c.apps + HC_STATE);
        modem_st = smem_rd(s, c.modem + HC_STATE);

        switch (apps_st) {
        case SMD_SS_OPENING:
            if (modem_st != SMD_SS_OPENED) {
                smem_wr(s, c.modem + HC_TAIL, 0);
                smem_wr(s, c.modem + HC_HEAD, 0);
                g_byte_array_set_size(s->pkt_acc[n], 0);
                smd_set_modem_state(s, &c, SMD_SS_OPENED);
                kick = true;
                if (s->trace) {
                    qemu_log("modem: smd ch%d '%s' opened\n", n, c.name);
                }
                g_byte_array_set_size(s->tx_backlog[n], 0);
                msm_modem_smd_opened(s, &c);
                msm_atmodem_opened(s, &c);
            }
            break;
        case SMD_SS_OPENED:
            if (modem_st != SMD_SS_OPENED) {
                smd_set_modem_state(s, &c, SMD_SS_OPENED);
                kick = true;
            }
            break;
        case SMD_SS_CLOSING:
        case SMD_SS_CLOSED:
            if (modem_st == SMD_SS_OPENED) {
                smd_set_modem_state(s, &c, SMD_SS_CLOSED);
                kick = true;
            }
            break;
        }
        if (smem_rd(s, c.apps + HC_HEAD) != smem_rd(s, c.apps + HC_TAIL)) {
            smd_consume(s, &c);
            kick = true;
        }
        chan_flush(s, &c);
        if (smem_rdb(s, c.apps + HC_FTAIL)) {
            smem_wrb(s, c.apps + HC_FTAIL, 0);
        }
        if (smem_rdb(s, c.apps + HC_FHEAD)) {
            smem_wrb(s, c.apps + HC_FHEAD, 0);
        }
        if (kick) {
            smd_kick_apps(s, &c);
        }
    }
}

bool msm_modem_find_channel(MsmModemState *s, const char *name, int type,
                            SmdChan *out)
{
    for (int n = 0; n < SMD_NUM_CH; n++) {
        if (smd_channel(s, n, out) && !strcmp(out->name, name) &&
            (int)(out->ctype & SMD_TYPE_MASK) == type) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Doorbells                                                           */
/* ------------------------------------------------------------------ */

static void modem_a2m(void *opaque, int n, int level)
{
    MsmModemState *s = opaque;

    if (!level || !s->smem || s->taken_over) {
        return;
    }
    switch (n) {
    case 0:
        smd_scan(s, false);
        break;
    case 4:
        dex_handle(s);
        break;
    case 5:
        smsm_handle(s);
        break;
    case 6:
        proc_comm_handle(s);
        break;
    case 8:
        smd_scan(s, true);
        smsm_handle(s);
        break;
    default:
        if (s->trace) {
            qemu_log("modem: A2M %d\n", n);
        }
        break;
    }
}

void msm_modem_a2m_irq(MsmModemState *s, int n)
{
    modem_a2m(s, n, 1);
}

void msm_modem_set_smem(MsmModemState *s, uint8_t *smem)
{
    s->smem = smem;
}

/* Bring SMEM to the state AMSS leaves it in when it hands over to Linux */
void msm_modem_boot(MsmModemState *s)
{
    uint32_t tbl;

    if (!s->smem) {
        return;
    }
    if (s->smem_image) {
        g_autofree gchar *data = NULL;
        gsize len;
        GError *err = NULL;
        if (g_file_get_contents(s->smem_image, &data, &len, &err)) {
            memcpy(s->smem, data, MIN(len, MSM_SHARED_RAM_SIZE));
        } else {
            error_report("msm-modem: cannot read SMEM image %s: %s",
                         s->smem_image, err->message);
            g_error_free(err);
        }
    }

    s->taken_over = false;
    physical_memory_write(AMSS_HOOK_PA, amss_hook_fn, sizeof(amss_hook_fn));

    /* proc_comm: modem idle and ready */
    smem_wr(s, 0x00, PCOM_CMD_IDLE);
    smem_wr(s, 0x04, 0);
    smem_wr(s, 0x10, PCOM_CMD_IDLE);
    smem_wr(s, 0x14, PCOM_READY);

    /* DEX: clean mailbox */
    for (int i = 0; i < 0x40; i += 4) {
        smem_wr(s, DEX_BASE + i, 0);
    }

    /* boot reason: normal power-on (cLK checks 2 == offmode charging) */
    smem_wr(s, 0xEF244, s->boot_reason);

    /* SMSM: modem fully up, power master running, no time request */
    smsm_set(s, SMSM_STATE_APPS, 0);
    smsm_set(s, SMSM_STATE_MODEM, SMSM_INIT | SMSM_OSENTERED | SMSM_SMDINIT |
             SMSM_RPCINIT | SMSM_RUN);
    smsm_set(s, SMSM_STATE_APPS_DEM, 0);
    smsm_set(s, SMSM_STATE_POWER_MASTER_DEM, DEM_MASTER_RUN | DEM_MASTER_READY);
    smsm_set(s, SMSM_STATE_TIME_MASTER_DEM, 0);
    s->last_apps = 0;
    {
        uint32_t clk = smem_item(s, SMEM_SMEM_SLOW_CLOCK_VALUE, NULL);
        if (clk) {
            smem_wr(s, clk, 0);
        }
    }

    /* SMD: every channel closed on both sides, FIFOs empty */
    tbl = smem_item(s, SMEM_CHANNEL_ALLOC_TBL, NULL);
    for (int n = 0; tbl && n < SMD_NUM_CH; n++) {
        SmdChan c;
        if (!smd_channel(s, n, &c)) {
            continue;
        }
        memset(s->smem + c.shared, 0, 2 * HC_SIZE);
        g_byte_array_set_size(s->pkt_acc[n], 0);
    }
    for (int n = 0; n < SMD_NUM_CH; n++) {
        g_byte_array_set_size(s->tx_backlog[n], 0);
    }
    msm_modem_rpc_reset(s);
    msm_qdsp6_reset(s);
    msm_atmodem_reset(s);
    msm_gps_reset(s);
    aux_loopback(s, false);
}

static void modem_reset(DeviceState *dev)
{
    MsmModemState *s = MSM_MODEM(dev);
    timer_del(s->wake_timer);
    timer_del(s->park_timer);
}

static void modem_realize(DeviceState *dev, Error **errp)
{
    MsmModemState *s = MSM_MODEM(dev);

    for (int i = 0; i < SMD_NUM_CH; i++) {
        s->pkt_acc[i] = g_byte_array_new();
        s->tx_backlog[i] = g_byte_array_new();
    }
    s->wake_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, modem_wake_apps, s);
    s->park_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, modem_park_poll, s);
    msm_modem_rpc_init(s);
    msm_atmodem_init(s);
    msm_gps_init(s);
    /* audio output is optional: without a backend playback is silent */
    if (!audio_be_check(&s->audio_be, NULL)) {
        s->audio_be = NULL;
    }
}

static void modem_init(Object *obj)
{
    MsmModemState *s = MSM_MODEM(obj);

    qdev_init_gpio_in_named(DEVICE(obj), modem_a2m, "a2m", 16);
    qdev_init_gpio_out_named(DEVICE(obj), s->m2a, "m2a", M2A_NUM);
}

static const Property modem_props[] = {
    DEFINE_PROP_STRING("smem-image", MsmModemState, smem_image),
    DEFINE_PROP_UINT32("boot-reason", MsmModemState, boot_reason, 1),
    DEFINE_PROP_BOOL("trace", MsmModemState, trace, false),
    DEFINE_AUDIO_PROPERTIES(MsmModemState, audio_be),
    DEFINE_NIC_PROPERTIES(MsmModemState, nic_conf),
    /* where the emulated GPS receiver finds itself (degrees, metres) */
    DEFINE_PROP_STRING("gps-latitude", MsmModemState, gps_lat),
    DEFINE_PROP_STRING("gps-longitude", MsmModemState, gps_lon),
    DEFINE_PROP_STRING("gps-altitude", MsmModemState, gps_alt),
};

static void modem_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = modem_realize;
    device_class_set_legacy_reset(dc, modem_reset);
    device_class_set_props(dc, modem_props);
}

static const TypeInfo msm_modem_info = {
    .name = TYPE_MSM_MODEM,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(MsmModemState),
    .instance_init = modem_init,
    .class_init = modem_class_init,
};

static void msm_modem_register(void)
{
    type_register_static(&msm_modem_info);
}
type_init(msm_modem_register)
