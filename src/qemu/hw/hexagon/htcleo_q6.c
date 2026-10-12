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
 *   0xABF00000  "TCM IO": interrupt routing the kernel sets up at boot
 *
 *   qemu-system-hexagon -M htcleo-q6 -kernel dsp1.elf \
 *     -object memory-backend-file,id=htcleo.ram,size=1G,mem-path=...,share=on
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
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/hexagon/hexagon.h"
#include "hw/hexagon/hexagon_globalreg.h"
#include "hw/hexagon/hexagon_tlb.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "system/system.h"
#include "elf.h"
#include "cpu.h"

#define HTCLEO_Q6_RAM_SIZE      (1 * GiB)
#define HTCLEO_Q6_THREADS       6
#define HTCLEO_Q6_TLB_ENTRIES   64

#define Q6_TIMER_BASE           0xAB000000
#define Q6_TIMER_IRQ            3
#define Q6_TIMER_HZ             32768       /* sleep clock */

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
    if (n >= TRACE_REPEATS) {
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

    s->regs = g_hash_table_new(g_direct_hash, g_direct_equal);
    s->seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    memory_region_init_io(&s->tap, OBJECT(machine), &tap_ops, s, "q6-tap",
                          TAP_SIZE);
    memory_region_add_subregion_overlap(sysmem, TAP_BASE, &s->tap, -1);
    memory_region_init_io(&s->timer_io, OBJECT(machine), &q6_timer_ops, s,
                          "q6-timer", 0x10);
    memory_region_add_subregion(sysmem, Q6_TIMER_BASE, &s->timer_io);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, q6_timer_fire, s);

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
    HTCLEO_Q6_MACHINE(obj)->trace = true;
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
