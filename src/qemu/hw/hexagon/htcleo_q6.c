/*
 * HTC HD2 (QSD8250) QDSP6: the multimedia DSP running its own firmware
 *
 * The QSD8250's QDSP6 is a Hexagon V2 core with six hardware threads.  It
 * does audio, voice and video for the apps processor, which talks to it
 * through DAL calls over SMD channels in the shared memory.  Its firmware
 * is the DSP1 image of the radio ROM (an EM_HEXAGON ELF, e_flags 1 = V2,
 * loaded at 0x10000000 and entered at 0x10002000).
 *
 * This machine runs that firmware in its own QEMU process next to the
 * qemu-system-arm -M htcleo one: both map the same DRAM (a shared memory
 * backend with id "htcleo.ram", 1 GiB at 0), so the DSP sees the shared
 * memory, its own carve-out and the apps' pmem buffers where the real one
 * does.  Peripherals that are not modelled yet sit behind a tap that logs
 * accesses and keeps what was written (trace=on).
 *
 * The QDSP6 subsystem registers are at 0xAB000000 (Qube maps them at
 * 0xFB000000 for the kernel):
 *   0xAB000000  timer, the MSM GPT block: +0 match, +4 count, +8 enable
 *               (bit 0 enable, bit 1 clear on match), +C clear.  It
 *               interrupts on line 3 ("Qube Timer INT Number: 3").
 *   0xAB005000  outgoing interrupts, write 1: +0 to the modem, +4 to the
 *               apps processor (the SMSM interrupt table's second byte
 *               picks the register)
 *   0xA0700000  audio interface DMA (htcleo_q6_audif.c), interrupt 12
 *   0xA1100000  video decoder core (htcleo_q6_vdc.c), interrupt 16
 *   0xABF00000  "TCM IO": interrupt routing the kernel sets up at boot
 *
 * The six hardware threads share one pipeline and take turns every cycle,
 * so a thread another one wakes runs at once.  Run with -icount: each
 * vCPU then gets smt-slice packets (default 64) per turn instead of a
 * whole host timeslice.
 *
 * The interrupt lines between the two processes go over a chardev
 * (link=<chardev id>), one byte per edge carrying the line number, the
 * other end being msm-q6-link in the htcleo machine:
 *   in  0  apps -> DSP, raises interrupt apps-irq
 *   in  1  modem -> DSP, raises interrupt modem-irq
 *   out 0  DSP -> apps, out 1 DSP -> modem
 *
 *   qemu-system-hexagon -M htcleo-q6,link=q6 -kernel dsp1.elf \
 *     -object memory-backend-file,id=htcleo.ram,size=1G,mem-path=...,share=on
 *     -chardev socket,id=q6,path=...
 *
 * This code is licensed under the GNU GPL v2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/host-utils.h"
#include "hw/core/irq.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "chardev/char.h"
#include "chardev/char-fe.h"
#include "hw/core/sysbus.h"
#include "hw/hexagon/hexagon.h"
#include "hw/hexagon/hexagon_globalreg.h"
#include "hw/hexagon/hexagon_tlb.h"
#include "hw/hexagon/htcleo_q6_vdc.h"
#include "hw/hexagon/htcleo_q6_audif.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "system/system.h"
#include "exec/icount.h"
#include "system/physmem.h"
#include "elf.h"
#include "cpu.h"

#define HTCLEO_Q6_RAM_SIZE      (1 * GiB)
#define HTCLEO_Q6_THREADS       6
#define HTCLEO_Q6_TLB_ENTRIES   64

#define Q6_TIMER_BASE           0xAB000000
#define Q6_TIMER_IRQ            3
#define Q6_TIMER_HZ             32768       /* sleep clock */
#define Q6_IPC_BASE             0xAB005000
#define AUDIF_BASE              0xA0700000
#define AUDIF_IRQ               12
#define VDC_BASE                0xA1100000
#define VDC_SIZE                0x5000
#define VDC_STATUS              0x0010      /* bits 0-1: AXI/CRIF errors */
#define VDC_STATUS_CLR          0x0018
#define VDC_CP_ENABLE           0x4088
#define VDC_CP_BUSY             0x408C      /* bit n: slot n queued */
#define VDC_CP_ADDR0            0x4090
#define VDC_CP_ADDR1            0x4094
#define VDC_CP_DONE             0x4098      /* bit n: slot n done */
#define VDC_CP_DONE_CLR         0x409C

#define TAP_BASE                0x40000000ULL
#define TAP_SIZE                0xC0000000ULL
#define TRACE_REPEATS           4

#define TYPE_HTCLEO_Q6_MACHINE MACHINE_TYPE_NAME("htcleo-q6")
OBJECT_DECLARE_SIMPLE_TYPE(HtcleoQ6MachineState, HTCLEO_Q6_MACHINE)

struct HtcleoQ6MachineState {
    MachineState parent_obj;

    MemoryRegion tap;
    GHashTable *regs;           /* unmodelled registers: addr -> value */
    GHashTable *seen;
    bool trace;
    HexagonCPU *cpu[HTCLEO_Q6_THREADS];

    /* timer */
    MemoryRegion timer_io;
    QEMUTimer *timer;
    uint32_t tmr_match, tmr_enable;
    int64_t tmr_base;           /* virtual time of count 0 */

    /* interrupt lines to and from the htcleo machine */
    char *link_id;
    CharFrontend link;
    int32_t apps_irq, modem_irq;
    uint32_t smt_slice;
    MemoryRegion ipc_io;

    /* the video decoder core ("Janus" VDC) */
    MemoryRegion vdc_io;
    uint32_t vdc_regs[VDC_SIZE / 4];
    QEMUTimer *vdc_timer;
    int32_t vdc_irq;
    char *vdc_dump;             /* directory: one file per command buffer */
    unsigned vdc_frames;
};

/* ---------------------------------------------------------------- */
/* Tap over the peripheral space                                     */
/* ---------------------------------------------------------------- */

static uint32_t q6_pc(void)
{
    return current_cpu ? cpu_env(current_cpu)->gpr[HEX_REG_PC] : 0;
}

static void tap_log(HtcleoQ6MachineState *s, hwaddr a, bool wr, uint64_t v,
                    unsigned size)
{
    gpointer key = (gpointer)(uintptr_t)((a << 1) | wr);
    uintptr_t n;

    if (!s->trace) {
        return;
    }
    n = GPOINTER_TO_UINT(g_hash_table_lookup(s->seen, key));
    if (n >= TRACE_REPEATS && (a & ~0xfffULL) != 0xABF00000ULL) {
        return;
    }
    g_hash_table_insert(s->seen, key, GUINT_TO_POINTER(n + 1));
    qemu_log("q6-io: %c %08" HWADDR_PRIx " %s %0*" PRIx64 " pc %08x\n",
             wr ? 'W' : 'R', a, wr ? "<-" : "->", size * 2, v, q6_pc());
}

static uint64_t tap_read(void *opaque, hwaddr off, unsigned size)
{
    HtcleoQ6MachineState *s = opaque;
    hwaddr a = TAP_BASE + off;
    uint32_t w = GPOINTER_TO_UINT(g_hash_table_lookup(s->regs,
                                  (gpointer)(uintptr_t)(a & ~3ULL)));
    uint64_t v = extract32(w, (a & 3) * 8, MIN(size * 8, 32));

    tap_log(s, a, false, v, size);
    return v;
}

static void tap_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    HtcleoQ6MachineState *s = opaque;
    hwaddr a = TAP_BASE + off;
    gpointer key = (gpointer)(uintptr_t)(a & ~3ULL);
    uint32_t w = GPOINTER_TO_UINT(g_hash_table_lookup(s->regs, key));

    w = deposit32(w, (a & 3) * 8, MIN(size * 8, 32), val);
    g_hash_table_insert(s->regs, key, GUINT_TO_POINTER(w));
    tap_log(s, a, true, val, size);
}

static const MemoryRegionOps tap_ops = {
    .read = tap_read,
    .write = tap_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------------------- */
/* Timer (the MSM GPT block)                                         */
/* ---------------------------------------------------------------- */

static uint32_t q6_timer_count(HtcleoQ6MachineState *s, int64_t now)
{
    return muldiv64(now - s->tmr_base, Q6_TIMER_HZ, NANOSECONDS_PER_SECOND);
}

static void q6_timer_schedule(HtcleoQ6MachineState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t delta;

    if (!(s->tmr_enable & 1)) {
        timer_del(s->timer);
        return;
    }
    delta = s->tmr_match - q6_timer_count(s, now);
    timer_mod(s->timer, now + muldiv64(delta ? delta : 1,
                                       NANOSECONDS_PER_SECOND, Q6_TIMER_HZ));
}

static void q6_timer_fire(void *opaque)
{
    HtcleoQ6MachineState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (s->trace) {
        qemu_log("q6-timer: match %08x, interrupt\n", s->tmr_match);
    }
    qemu_irq_pulse(qdev_get_gpio_in(DEVICE(s->cpu[0]), Q6_TIMER_IRQ));
    if (s->tmr_enable & 2) {
        s->tmr_base = now;      /* clear on match */
    }
    /* the count wraps before it matches again */
    timer_mod(s->timer, now + muldiv64(s->tmr_enable & 2 ? s->tmr_match :
                                       0x100000000ULL,
                                       NANOSECONDS_PER_SECOND, Q6_TIMER_HZ));
}

static uint64_t q6_timer_read(void *opaque, hwaddr off, unsigned size)
{
    HtcleoQ6MachineState *s = opaque;

    switch (off) {
    case 0x0:
        return s->tmr_match;
    case 0x4:
        return q6_timer_count(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    case 0x8:
        return s->tmr_enable;
    }
    return 0;
}

static void q6_timer_write(void *opaque, hwaddr off, uint64_t val,
                           unsigned size)
{
    HtcleoQ6MachineState *s = opaque;

    if (s->trace) {
        qemu_log("q6-timer: [%02x] <- %08x (count %08x) pc %08x\n",
                 (unsigned)off, (unsigned)val,
                 q6_timer_count(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)),
                 q6_pc());
    }

    switch (off) {
    case 0x0:
        s->tmr_match = val;
        break;
    case 0x8:
        s->tmr_enable = val & 3;
        break;
    case 0xC:
        s->tmr_base = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        break;
    default:
        return;
    }
    q6_timer_schedule(s);
}

static const MemoryRegionOps q6_timer_ops = {
    .read = q6_timer_read,
    .write = q6_timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ---------------------------------------------------------------- */
/* Interrupt link                                                    */
/* ---------------------------------------------------------------- */

static void q6_link_send(HtcleoQ6MachineState *s, int line)
{
    uint8_t b = line;

    if (s->trace) {
        qemu_log("q6-link: %.3f -> line %d pc %08x\n",
                 g_get_real_time() / 1e6, line, q6_pc());
    }
    qemu_chr_fe_write_all(&s->link, &b, 1);
}

static int q6_link_can_receive(void *opaque)
{
    return 64;
}

static void q6_link_receive(void *opaque, const uint8_t *buf, int size)
{
    HtcleoQ6MachineState *s = opaque;

    for (int i = 0; i < size; i++) {
        int line = buf[i] & 0x7F;
        int irq = line == 0 ? s->apps_irq : line == 1 ? s->modem_irq : -1;

        if (s->trace) {
            qemu_log("q6-link: <- line %d, interrupt %d\n", line, irq);
        }
        if (irq >= 0 && irq < 32) {
            qemu_irq_pulse(qdev_get_gpio_in(DEVICE(s->cpu[0]), irq));
        }
    }
}

static uint64_t q6_ipc_read(void *opaque, hwaddr off, unsigned size)
{
    return 0;
}

static void q6_ipc_write(void *opaque, hwaddr off, uint64_t val,
                         unsigned size)
{
    HtcleoQ6MachineState *s = opaque;

    if (!(val & 1)) {
        return;
    }
    switch (off) {
    case 0x0:
        q6_link_send(s, 1);     /* modem */
        break;
    case 0x4:
        q6_link_send(s, 0);     /* apps */
        break;
    }
}

static const MemoryRegionOps q6_ipc_ops = {
    .read = q6_ipc_read,
    .write = q6_ipc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ---------------------------------------------------------------- */
/* Video decoder core                                                */
/* ---------------------------------------------------------------- */

#define VDC_REG(s, off) ((s)->vdc_regs[(off) / 4])

#define VDC_CMD_MAX     0x40000     /* words */

/*
 * Command buffers are packet streams, header byte = opcode:
 *   04 rrrrrr v         write register r
 *   03 rrr sss v w      write registers r, s
 *   02 rr ss tt u v w   write registers r, s, t
 *   08 rrrrrr a n       feed n words from address a to register r
 *                       (n bit 22: flag, low bits: count)
 *   0a                  end of a pipeline step (one macroblock)
 *   00 xxxxxx           (seen with 0x50, after the MB header)
 * The first word is a header, the stream ends at a zero word.
 */
static void vdc_dump_buffer(HtcleoQ6MachineState *s, uint32_t addr)
{
    g_autofree uint32_t *w = g_new0(uint32_t, VDC_CMD_MAX);
    g_autofree char *path = g_strdup_printf("%s/vdc%04u.bin", s->vdc_dump,
                                            s->vdc_frames);
    FILE *f = fopen(path, "wb");
    unsigned n;

    if (!f) {
        return;
    }
    physical_memory_read(addr, w, VDC_CMD_MAX * 4);
    for (n = 1; n < VDC_CMD_MAX && w[n]; ) {
        uint32_t h = le32_to_cpu(w[n]);

        if (h >> 24 == 0x01) {
            n++;
            break;
        }

        switch (h >> 24) {
        case 0x04: n += 2; break;
        case 0x03: n += 3; break;
        case 0x02: n += 4; break;
        case 0x08: n += 3; break;
        default: n += 1; break;
        }
    }
    fwrite(&addr, 4, 1, f);
    fwrite(&n, 4, 1, f);
    fwrite(w, 4, n, f);
    /* then every block an 08 packet feeds: addr, words, data */
    for (unsigned i = 1; i + 2 < n; ) {
        uint32_t h = le32_to_cpu(w[i]);

        if (h >> 24 == 0x08) {
            uint32_t a = le32_to_cpu(w[i + 1]);
            uint32_t cnt = le32_to_cpu(w[i + 2]) & 0xffff;
            g_autofree uint32_t *blk = g_new(uint32_t, cnt);

            physical_memory_read(a, blk, cnt * 4);
            fwrite(&a, 4, 1, f);
            fwrite(&cnt, 4, 1, f);
            fwrite(blk, 4, cnt, f);
        }
        switch (h >> 24) {
        case 0x04: i += 2; break;
        case 0x03: i += 3; break;
        case 0x02: i += 4; break;
        case 0x08: i += 3; break;
        default: i += 1; break;
        }
    }
    fclose(f);
}

static void vdc_run(HtcleoQ6MachineState *s, int slot)
{
    uint32_t addr = VDC_REG(s, slot ? VDC_CP_ADDR1 : VDC_CP_ADDR0);

    if (s->vdc_dump) {
        vdc_dump_buffer(s, addr);
    }
    if (s->vdc_dump) {
        g_autofree char *yuv = g_strdup_printf("%s/out%04u.yuv", s->vdc_dump,
                                               s->vdc_frames);
        htcleo_vdc_decode(addr, s->trace, yuv);
    } else {
        htcleo_vdc_decode(addr, s->trace, NULL);
    }
    s->vdc_frames++;

    if (s->trace) {
        qemu_log("q6-vdc: %.3f slot %d command buffer %08x\n",
                 g_get_real_time() / 1e6, slot, addr);
    }
    VDC_REG(s, VDC_CP_DONE) |= 1u << slot;
}

static void vdc_kick(void *opaque)
{
    HtcleoQ6MachineState *s = opaque;
    uint32_t pending = VDC_REG(s, VDC_CP_BUSY) & ~VDC_REG(s, VDC_CP_DONE) & 3;

    if (!(VDC_REG(s, VDC_CP_ENABLE) & 1) || !pending) {
        return;
    }
    for (int slot = 0; slot < 2; slot++) {
        if (pending & (1u << slot)) {
            vdc_run(s, slot);
        }
    }
    if (s->vdc_irq >= 0) {
        qemu_irq_pulse(qdev_get_gpio_in(DEVICE(s->cpu[0]), s->vdc_irq));
    }
}

static uint64_t vdc_read(void *opaque, hwaddr off, unsigned size)
{
    HtcleoQ6MachineState *s = opaque;
    uint32_t v = VDC_REG(s, off & ~3);

    tap_log(s, VDC_BASE + off, false, v, size);
    return v;
}

static void vdc_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    HtcleoQ6MachineState *s = opaque;

    tap_log(s, VDC_BASE + off, true, val, size);
    switch (off) {
    case VDC_STATUS_CLR:
        VDC_REG(s, VDC_STATUS) &= ~val;
        return;
    case VDC_CP_DONE_CLR:
        VDC_REG(s, VDC_CP_DONE) &= ~val;
        return;
    case VDC_CP_BUSY:
        VDC_REG(s, off) = val;
        /* the hardware takes a while; let the driver finish queueing */
        timer_mod(s->vdc_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  3 * SCALE_MS);
        return;
    }
    VDC_REG(s, off) = val;
}

static const MemoryRegionOps vdc_ops = {
    .read = vdc_read,
    .write = vdc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ---------------------------------------------------------------- */
/* Machine                                                           */
/* ---------------------------------------------------------------- */

static void htcleo_q6_cpu_reset(void *opaque)
{
    cpu_reset(CPU(opaque));
}

static void htcleo_q6_init(MachineState *machine)
{
    HtcleoQ6MachineState *s = HTCLEO_Q6_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *glob, *tlb;
    uint64_t entry = 0;
    long size;

    if (machine->ram_size != HTCLEO_Q6_RAM_SIZE) {
        error_report("htcleo-q6: the DRAM is %u MiB",
                     (unsigned)(HTCLEO_Q6_RAM_SIZE / MiB));
        exit(1);
    }
    if (!machine->kernel_filename) {
        error_report("htcleo-q6: -kernel the DSP1 firmware ELF");
        exit(1);
    }
    memory_region_add_subregion(sysmem, 0, machine->ram);
    if (icount_enabled()) {
        icount_rr_slice = s->smt_slice;
    } else {
        warn_report("htcleo-q6: without -icount the hardware threads run "
                    "in long timeslices, not interleaved");
    }

    s->regs = g_hash_table_new(g_direct_hash, g_direct_equal);
    s->seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    memory_region_init_io(&s->tap, OBJECT(machine), &tap_ops, s, "q6-tap",
                          TAP_SIZE);
    memory_region_add_subregion_overlap(sysmem, TAP_BASE, &s->tap, -1);
    memory_region_init_io(&s->timer_io, OBJECT(machine), &q6_timer_ops, s,
                          "q6-timer", 0x10);
    memory_region_add_subregion(sysmem, Q6_TIMER_BASE, &s->timer_io);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, q6_timer_fire, s);
    memory_region_init_io(&s->ipc_io, OBJECT(machine), &q6_ipc_ops, s,
                          "q6-ipc", 0x8);
    memory_region_add_subregion(sysmem, Q6_IPC_BASE, &s->ipc_io);
    memory_region_init_io(&s->vdc_io, OBJECT(machine), &vdc_ops, s, "q6-vdc",
                          VDC_SIZE);
    memory_region_add_subregion(sysmem, VDC_BASE, &s->vdc_io);
    s->vdc_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, vdc_kick, s);

    glob = qdev_new(TYPE_HEXAGON_GLOBALREG);
    object_property_add_child(OBJECT(machine), "global-regs", OBJECT(glob));
    qdev_prop_set_bit(glob, "qdsp6-v2", true);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(glob), &error_fatal);

    tlb = qdev_new(TYPE_HEXAGON_TLB);
    object_property_add_child(OBJECT(machine), "tlb", OBJECT(tlb));
    qdev_prop_set_uint32(tlb, "num-entries", HTCLEO_Q6_TLB_ENTRIES);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(tlb), &error_fatal);

    size = load_elf(machine->kernel_filename, NULL, NULL, NULL, &entry, NULL,
                    NULL, NULL, 0, EM_HEXAGON, 0, 0);
    if (size <= 0) {
        error_report("htcleo-q6: cannot load %s", machine->kernel_filename);
        exit(1);
    }

    for (int i = 0; i < machine->smp.cpus; i++) {
        HexagonCPU *cpu = HEXAGON_CPU(object_new(machine->cpu_type));

        s->cpu[i] = cpu;
        qemu_register_reset(htcleo_q6_cpu_reset, cpu);
        qdev_prop_set_uint32(DEVICE(cpu), "htid", i);
        /* thread 0 runs from reset, start enables the others */
        qdev_prop_set_bit(DEVICE(cpu), "start-powered-off", i != 0);
        if (i == 0) {
            qdev_prop_set_uint32(DEVICE(cpu), "exec-start-addr", entry);
        }
        object_property_set_link(OBJECT(cpu), "global-regs", OBJECT(glob),
                                 &error_fatal);
        object_property_set_link(OBJECT(cpu), "tlb", OBJECT(tlb),
                                 &error_fatal);
        qdev_realize_and_unref(DEVICE(cpu), NULL, &error_fatal);
    }

    {
        /* audio interface DMA; its audiodev comes from -global */
        DeviceState *af = qdev_new(TYPE_HTCLEO_Q6_AUDIF);

        object_property_add_child(OBJECT(machine), "audif", OBJECT(af));
        qdev_prop_set_bit(af, "trace", s->trace);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(af), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(af), 0, AUDIF_BASE);
        sysbus_connect_irq(SYS_BUS_DEVICE(af), 0,
                           qdev_get_gpio_in(DEVICE(s->cpu[0]), AUDIF_IRQ));
    }

    if (s->link_id) {
        Chardev *chr = qemu_chr_find(s->link_id);

        if (!chr) {
            error_report("htcleo-q6: no chardev %s for link", s->link_id);
            exit(1);
        }
        qemu_chr_fe_init(&s->link, chr, &error_fatal);
        qemu_chr_fe_set_handlers(&s->link, q6_link_can_receive,
                                 q6_link_receive, NULL, NULL, s, NULL, true);
    }
}

static char *htcleo_q6_get_vdc_dump(Object *obj, Error **errp)
{
    return g_strdup(HTCLEO_Q6_MACHINE(obj)->vdc_dump);
}

static void htcleo_q6_set_vdc_dump(Object *obj, const char *v, Error **errp)
{
    HtcleoQ6MachineState *s = HTCLEO_Q6_MACHINE(obj);

    g_free(s->vdc_dump);
    s->vdc_dump = g_strdup(v);
}

static char *htcleo_q6_get_link(Object *obj, Error **errp)
{
    return g_strdup(HTCLEO_Q6_MACHINE(obj)->link_id);
}

static void htcleo_q6_set_link(Object *obj, const char *v, Error **errp)
{
    HtcleoQ6MachineState *s = HTCLEO_Q6_MACHINE(obj);

    g_free(s->link_id);
    s->link_id = g_strdup(v);
}

static void htcleo_q6_get_irq(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    visit_type_int32(v, name, (int32_t *)opaque, errp);
}

static void htcleo_q6_set_irq(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    visit_type_int32(v, name, (int32_t *)opaque, errp);
}

static bool htcleo_q6_get_trace(Object *obj, Error **errp)
{
    return HTCLEO_Q6_MACHINE(obj)->trace;
}

static void htcleo_q6_set_trace(Object *obj, bool value, Error **errp)
{
    HTCLEO_Q6_MACHINE(obj)->trace = value;
}

static void htcleo_q6_instance_init(Object *obj)
{
    HtcleoQ6MachineState *s = HTCLEO_Q6_MACHINE(obj);

    s->trace = true;
    s->smt_slice = 64;
    object_property_add_uint32_ptr(obj, "smt-slice", &s->smt_slice,
                                   OBJ_PROP_FLAG_READWRITE);
    s->apps_irq = -1;
    s->modem_irq = -1;
    s->vdc_irq = 16;
    object_property_add(obj, "vdc-irq", "int32", htcleo_q6_get_irq,
                        htcleo_q6_set_irq, NULL, &s->vdc_irq);
    object_property_add(obj, "apps-irq", "int32", htcleo_q6_get_irq,
                        htcleo_q6_set_irq, NULL, &s->apps_irq);
    object_property_add(obj, "modem-irq", "int32", htcleo_q6_get_irq,
                        htcleo_q6_set_irq, NULL, &s->modem_irq);
}

static void htcleo_q6_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "HTC HD2 (QSD8250) QDSP6, Hexagon V2";
    mc->init = htcleo_q6_init;
    mc->default_cpu_type = HEXAGON_CPU_TYPE_NAME("v2");
    mc->default_cpus = HTCLEO_Q6_THREADS;
    mc->max_cpus = HTCLEO_Q6_THREADS;
    mc->default_ram_size = HTCLEO_Q6_RAM_SIZE;
    mc->default_ram_id = "htcleo.ram";
    mc->no_parallel = 1;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_serial = 1;
    object_class_property_add_bool(oc, "trace", htcleo_q6_get_trace,
                                   htcleo_q6_set_trace);
    object_class_property_add_str(oc, "link", htcleo_q6_get_link,
                                  htcleo_q6_set_link);
    object_class_property_add_str(oc, "vdc-dump", htcleo_q6_get_vdc_dump,
                                  htcleo_q6_set_vdc_dump);
}

static const TypeInfo htcleo_q6_info = {
    .name = TYPE_HTCLEO_Q6_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(HtcleoQ6MachineState),
    .instance_init = htcleo_q6_instance_init,
    .class_init = htcleo_q6_class_init,
};

static void htcleo_q6_register(void)
{
    type_register_static(&htcleo_q6_info);
}
type_init(htcleo_q6_register)
