/*
 * HTC HD2 baseband, low level: the modem processor (ARM926EJ-S) running the
 * real AMSS firmware from the radio ROM.
 *
 * On a QSD8250 the modem core boots first (PBL -> DBL -> FSBL -> OSBL ->
 * AMSS), owns the clocks, the PMIC and the shared memory, and only then
 * releases the apps processor.  Here the secure boot stages are skipped:
 * the AMSS ELF (extracted from the RUU's radio NBH) is loaded at its
 * physical addresses and the modem core starts at its entry point, which is
 * where OSBL hands over.
 *
 * Address space of the modem core:
 *   0x00000000 - 0x3fffffff  the shared DRAM (the same RAM the apps see)
 *   0x40000000 - 0xffffffff  the shared peripheral bus, reached through a
 *                            tap that can log every access (trace=on)
 * Modem-private registers nobody models yet keep what was written (a sparse
 * register store behind the tap), with the behaviours learnt so far in
 * lle_reg_read():
 *   0xB0007200  sleep controllers, one 0x3C bank per air interface; +0x24
 *               is the state, 1 once the controller runs; bit 0 of +0x10
 *               and +0x18 says a write to +0x0C / +0x14 has synchronised
 *
 * Modem-private peripherals are mapped over the tap as they are modelled:
 *   0xB8000000  timers and watchdog (msm_mtmr.c)
 *   0xB8001000  the modem VIC, same block as the apps one (L4 flavour)
 *               -> ARM926 IRQ/FIQ; source 32 is the L4 tick
 *   0x9C000000  1 MiB of modem-side RAM (L4 maps it at 0xF9700000)
 *   0xB1000000  the modem DSP's memories and control (msm_mdsp.c)
 *   0xFFFC0000  internal SRAM; AMSS puts its TLB-locked sleep code at
 *               0xFFFEF800 (L4 maps 0xFFF00000 at 0xF9600000)
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/cpu/cluster.h"
#include "hw/core/sysbus.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "target/arm/cpu.h"
#include "hw/core/cpu.h"
#include "htcleo.h"
#include "msm_modem_lle.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmModemLleState, MSM_MODEM_LLE)

#define MODEM_TMR_PHYS  0xB8000000
#define MODEM_VIC_PHYS  0xB8001000
#define MIRQ_TICK       32
#define MODEM_RAM9C_PHYS 0x9C000000
#define MODEM_RAM9C_SIZE 0x00100000
#define MODEM_IMEM_PHYS 0xFFFC0000
#define MODEM_IMEM_SIZE 0x00040000
#define MODEM_MDSP_PHYS 0xB1000000
#define MIRQ_MDSP       27

#define TAP_BASE        0x40000000ULL
#define TAP_SIZE        0xC0000000ULL
#define TRACE_REPEATS   4       /* accesses logged per address and direction */

struct MsmModemLleState {
    DeviceState parent_obj;

    CPUClusterState cluster;
    ARMCPU cpu;
    MemoryRegion mem;           /* the modem core's view of the bus */
    MemoryRegion ram_alias;
    MemoryRegion tap;
    MemoryRegion ram9c, imem;
    DeviceState *vic, *tmr, *mdsp;

    MemoryRegion *ram;
    char *firmware;
    bool trace;

    GByteArray *elf;
    uint32_t entry;
    GHashTable *seen;
    GHashTable *regs;           /* unbacked modem registers: addr -> value */
    uint32_t dump_on;
    bool dumped;
};

/* ---------------------------------------------------------------- */
/* Bus tap                                                           */
/* ---------------------------------------------------------------- */

static void tap_log(MsmModemLleState *s, hwaddr a, bool wr, uint64_t v,
                    unsigned size, MemTxResult r)
{
    gpointer key = (gpointer)(uintptr_t)((a << 1) | wr);
    uintptr_t n;

    if (!s->trace) {
        return;
    }
    n = GPOINTER_TO_UINT(g_hash_table_lookup(s->seen, key));
    if (n >= TRACE_REPEATS && r == MEMTX_OK) {
        return;
    }
    g_hash_table_insert(s->seen, key, GUINT_TO_POINTER(n + 1));
    qemu_log("modem-io: %c %08" HWADDR_PRIx " %s %0*" PRIx64 " pc %08x%s\n",
             wr ? 'W' : 'R', a, wr ? "<-" : "->", size * 2, v,
             s->cpu.env.regs[15],
             r == MEMTX_OK ? "" : " (nothing there)");
}

/* behaviour of modem registers that exist only behind the tap */
static uint32_t lle_reg_read(MsmModemLleState *s, hwaddr a, uint32_t v)
{
    if (a >= 0xB0007200 && a < 0xB0007200 + 8 * 0x3C) {
        switch ((a - 0xB0007200) % 0x3C) {
        case 0x24:
            return 1;                   /* sleep controller: running */
        case 0x10:
        case 0x18:
            return v | 1;               /* +0x0C/+0x14 write synchronised */
        }
    }
    return v;
}

static MemTxResult tap_read(void *opaque, hwaddr off, uint64_t *data,
                            unsigned size, MemTxAttrs attrs)
{
    MsmModemLleState *s = opaque;
    hwaddr a = TAP_BASE + off;
    uint8_t buf[8] = { 0 };
    MemTxResult r;

    r = address_space_read(&address_space_memory, a, attrs, buf, size);
    *data = ldn_le_p(buf, size);
    if (r != MEMTX_OK) {
        uint32_t w = GPOINTER_TO_UINT(g_hash_table_lookup(s->regs,
                                      (gpointer)(uintptr_t)(a & ~3ULL)));
        w = lle_reg_read(s, a & ~3ULL, w);
        *data = extract32(w, (a & 3) * 8, MIN(size * 8, 32));
    }
    tap_log(s, a, false, *data, size, r);
    return MEMTX_OK;
}

/*
 * dump-on-write=ADDR: the first time the modem core writes ADDR (say the
 * UART1 transmit register, 0xA9A0000C, where AMSS prints its fatal errors),
 * log its registers and stack; return addresses in the stack lead to the
 * code that failed (tools/amss_dis.py).
 */
static void lle_dump_stack(MsmModemLleState *s)
{
    CPUARMState *env = &s->cpu.env;

    s->dumped = true;
    qemu_log("modem dump: pc %08x lr %08x sp %08x mode %x\n", env->regs[15],
             env->regs[14], env->regs[13], env->uncached_cpsr & 0x1f);
    for (int i = 0; i < 256; i++) {
        uint32_t w = 0;
        cpu_memory_rw_debug(CPU(&s->cpu), env->regs[13] + 4 * i, &w, 4, false);
        qemu_log("  sp+%03x %08x\n", 4 * i, le32_to_cpu(w));
    }
}

static MemTxResult tap_write(void *opaque, hwaddr off, uint64_t data,
                             unsigned size, MemTxAttrs attrs)
{
    MsmModemLleState *s = opaque;
    hwaddr a = TAP_BASE + off;
    uint8_t buf[8];
    MemTxResult r;

    stn_le_p(buf, size, data);
    r = address_space_write(&address_space_memory, a, attrs, buf, size);
    if (r != MEMTX_OK) {
        gpointer k = (gpointer)(uintptr_t)(a & ~3ULL);
        uint32_t w = GPOINTER_TO_UINT(g_hash_table_lookup(s->regs, k));
        w = deposit32(w, (a & 3) * 8, MIN(size * 8, 32), data);
        g_hash_table_insert(s->regs, k, GUINT_TO_POINTER(w));
    }
    tap_log(s, a, true, data, size, r);
    if (s->dump_on && a == s->dump_on && !s->dumped) {
        lle_dump_stack(s);
    }
    return MEMTX_OK;
}

static const MemoryRegionOps tap_ops = {
    .read_with_attrs = tap_read,
    .write_with_attrs = tap_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------------------- */
/* Firmware                                                          */
/* ---------------------------------------------------------------- */

/* load every PT_LOAD segment of the AMSS ELF at its physical address */
static bool lle_load_elf(MsmModemLleState *s, Error **errp)
{
    const uint8_t *e = s->elf->data;
    uint32_t len = s->elf->len, phoff, phnum, phes;

    if (len < 52 || memcmp(e, "\x7f" "ELF\x01\x01", 6) ||
        lduw_le_p(e + 18) != 40 /* EM_ARM */) {
        error_setg(errp, "msm-modem-lle: %s is not a 32-bit ARM ELF",
                   s->firmware);
        return false;
    }
    s->entry = ldl_le_p(e + 24);
    phoff = ldl_le_p(e + 28);
    phes = lduw_le_p(e + 42);
    phnum = lduw_le_p(e + 44);
    for (uint32_t i = 0; i < phnum; i++) {
        const uint8_t *ph = e + phoff + i * phes;
        uint32_t type, off, pa, fsz, msz;
        g_autofree uint8_t *zero = NULL;

        if (phoff + (i + 1) * phes > len) {
            break;
        }
        type = ldl_le_p(ph);
        off = ldl_le_p(ph + 4);
        pa = ldl_le_p(ph + 12);
        fsz = ldl_le_p(ph + 16);
        msz = ldl_le_p(ph + 20);
        if (type != 1 /* PT_LOAD */ || off + fsz > len ||
            pa + msz > HTCLEO_RAM_SIZE) {
            continue;
        }
        address_space_write(&address_space_memory, pa,
                            MEMTXATTRS_UNSPECIFIED, e + off, fsz);
        if (msz > fsz) {
            zero = g_malloc0(msz - fsz);
            address_space_write(&address_space_memory, pa + fsz,
                                MEMTXATTRS_UNSPECIFIED, zero, msz - fsz);
        }
    }
    return true;
}

#define SMEM_HEAP_INFO          (MSM_SHARED_RAM_PHYS + 0xc0)
#define SMEM_TOC                (MSM_SHARED_RAM_PHYS + 0xd0)
#define SMEM_MARM_BOOT_SEGMENT_INFO 98

/* allocate a dynamic SMEM item the way smem_alloc() does */
static hwaddr lle_smem_alloc(unsigned id, uint32_t size)
{
    uint32_t h[4], e[4];

    size = ROUND_UP(size, 8);
    address_space_read(&address_space_memory, SMEM_HEAP_INFO,
                       MEMTXATTRS_UNSPECIFIED, h, sizeof(h));
    e[0] = cpu_to_le32(1);
    e[1] = h[1];
    e[2] = cpu_to_le32(size);
    e[3] = 0;
    address_space_write(&address_space_memory, SMEM_TOC + 16 * id,
                        MEMTXATTRS_UNSPECIFIED, e, sizeof(e));
    h[1] = cpu_to_le32(le32_to_cpu(h[1]) + size);
    h[2] = cpu_to_le32(le32_to_cpu(h[2]) - size);
    address_space_write(&address_space_memory, SMEM_HEAP_INFO,
                        MEMTXATTRS_UNSPECIFIED, h, sizeof(h));
    return MSM_SHARED_RAM_PHYS + le32_to_cpu(e[1]);
}

/*
 * What OSBL leaves in shared memory before it starts AMSS: the fixed items
 * (layout from AMSS's own table: proc_comm, version info, heap info,
 * allocation table, reset detect, warm boot, error message, spinlocks,
 * barrier) recorded in the allocation table, the heap marked initialized
 * with the rest of the 1 MiB free, and the modem boot segment info (item
 * 98): a 20-byte header with the segment count at +12, then the program
 * headers of the AMSS image it loaded.
 */
static void lle_osbl_smem_init(MsmModemLleState *s)
{
    static const struct { uint16_t id; uint32_t size; } fixed[] = {
        { 0, 0x40 }, { 3, 0x80 }, { 1, 0x10 }, { 2, 0x2000 }, { 4, 0x8 },
        { 5, 0x4 }, { 6, 0xc8 }, { 7, 0x20 }, { 8, 0x4 },
    };
    const hwaddr base = MSM_SHARED_RAM_PHYS;
    const hwaddr heap_info = base + 0xc0, toc = base + 0xd0;
    g_autofree uint8_t *zero = g_malloc0(MSM_SHARED_RAM_SIZE);
    uint32_t off = 0;

    address_space_write(&address_space_memory, base, MEMTXATTRS_UNSPECIFIED,
                        zero, MSM_SHARED_RAM_SIZE);
    for (size_t i = 0; i < ARRAY_SIZE(fixed); i++) {
        uint32_t e[4] = { cpu_to_le32(1), cpu_to_le32(off),
                          cpu_to_le32(fixed[i].size), 0 };
        address_space_write(&address_space_memory, toc + 16 * fixed[i].id,
                            MEMTXATTRS_UNSPECIFIED, e, sizeof(e));
        off += fixed[i].size;
    }
    {
        uint32_t h[4] = { cpu_to_le32(1), cpu_to_le32(off),
                          cpu_to_le32(MSM_SHARED_RAM_SIZE - off), 0 };
        address_space_write(&address_space_memory, heap_info,
                            MEMTXATTRS_UNSPECIFIED, h, sizeof(h));
    }
    {
        const uint8_t *e = s->elf->data;
        uint32_t phoff = ldl_le_p(e + 28), phnum = lduw_le_p(e + 44);
        uint32_t len = 20 + 32 * phnum;
        g_autofree uint8_t *info = g_malloc0(len);
        hwaddr a = lle_smem_alloc(SMEM_MARM_BOOT_SEGMENT_INFO, len);

        stl_le_p(info + 12, phnum);
        memcpy(info + 20, e + phoff, 32 * phnum);
        address_space_write(&address_space_memory, a,
                            MEMTXATTRS_UNSPECIFIED, info, len);
    }
}

void msm_modem_lle_reset(DeviceState *dev)
{
    MsmModemLleState *s = MSM_MODEM_LLE(dev);
    Error *err = NULL;

    g_hash_table_remove_all(s->seen);
    g_hash_table_remove_all(s->regs);
    s->dumped = false;
    cpu_reset(CPU(&s->cpu));
    if (!lle_load_elf(s, &err)) {
        error_report_err(err);
        exit(1);
    }
    lle_osbl_smem_init(s);
    /* OSBL jumps to AMSS in SVC mode with the MMU and caches off */
    cpu_set_pc(CPU(&s->cpu), s->entry);
    info_report("htcleo: modem core starts AMSS at 0x%08x", s->entry);
}

ARMCPU *msm_modem_lle_cpu(DeviceState *dev)
{
    return &MSM_MODEM_LLE(dev)->cpu;
}

MemoryRegion *msm_modem_lle_bus(DeviceState *dev)
{
    return &MSM_MODEM_LLE(dev)->mem;
}

qemu_irq msm_modem_lle_irq(DeviceState *dev, int n)
{
    return qdev_get_gpio_in(MSM_MODEM_LLE(dev)->vic, n);
}

/* a sysbus device whose registers only the modem core sees */
static DeviceState *lle_private_device(MsmModemLleState *s, const char *type,
                                       hwaddr base)
{
    DeviceState *d = qdev_new(type);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(d), &error_fatal);
    memory_region_add_subregion(&s->mem, base,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(d), 0));
    return d;
}

/* ---------------------------------------------------------------- */
/* Device                                                            */
/* ---------------------------------------------------------------- */

static void lle_realize(DeviceState *dev, Error **errp)
{
    MsmModemLleState *s = MSM_MODEM_LLE(dev);
    g_autoptr(GError) gerr = NULL;
    gchar *data;
    gsize len;

    if (!s->firmware || !s->ram) {
        error_setg(errp, "msm-modem-lle: needs firmware= (the AMSS ELF)");
        return;
    }
    if (!g_file_get_contents(s->firmware, &data, &len, &gerr)) {
        error_setg(errp, "msm-modem-lle: %s", gerr->message);
        return;
    }
    s->elf = g_byte_array_new_take((guint8 *)data, len);

    memory_region_init(&s->mem, OBJECT(dev), "modem-bus", UINT64_MAX);
    memory_region_init_alias(&s->ram_alias, OBJECT(dev), "modem-ram", s->ram,
                             0, memory_region_size(s->ram));
    memory_region_add_subregion(&s->mem, HTCLEO_RAM_BASE, &s->ram_alias);
    memory_region_init_io(&s->tap, OBJECT(dev), &tap_ops, s, "modem-tap",
                          TAP_SIZE);
    memory_region_add_subregion_overlap(&s->mem, TAP_BASE, &s->tap, -1);
    memory_region_init_ram(&s->ram9c, OBJECT(dev), "modem-ram9c",
                           MODEM_RAM9C_SIZE, &error_fatal);
    memory_region_add_subregion(&s->mem, MODEM_RAM9C_PHYS, &s->ram9c);
    memory_region_init_ram(&s->imem, OBJECT(dev), "modem-imem",
                           MODEM_IMEM_SIZE, &error_fatal);
    memory_region_add_subregion(&s->mem, MODEM_IMEM_PHYS, &s->imem);

    qdev_prop_set_uint32(DEVICE(&s->cluster), "cluster-id", 1);
    object_initialize_child(OBJECT(&s->cluster), "cpu", &s->cpu,
                            ARM_CPU_TYPE_NAME("arm926"));
    object_property_set_link(OBJECT(&s->cpu), "memory", OBJECT(&s->mem),
                             &error_abort);
    if (!qdev_realize(DEVICE(&s->cpu), NULL, errp)) {
        return;
    }
    qdev_realize(DEVICE(&s->cluster), NULL, errp);

    s->vic = qdev_new(TYPE_MSM_VIC);
    qdev_prop_set_bit(s->vic, "l4", true);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->vic), &error_fatal);
    memory_region_add_subregion(&s->mem, MODEM_VIC_PHYS,
                    sysbus_mmio_get_region(SYS_BUS_DEVICE(s->vic), 0));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->vic), 0,
                       qdev_get_gpio_in(DEVICE(&s->cpu), ARM_CPU_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->vic), 1,
                       qdev_get_gpio_in(DEVICE(&s->cpu), ARM_CPU_FIQ));

    s->tmr = lle_private_device(s, TYPE_MSM_MTMR, MODEM_TMR_PHYS);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->tmr), 0,
                       qdev_get_gpio_in(s->vic, MIRQ_TICK));

    s->mdsp = lle_private_device(s, TYPE_MSM_MDSP, MODEM_MDSP_PHYS);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->mdsp), 0,
                       qdev_get_gpio_in(s->vic, MIRQ_MDSP));
}

static void lle_init(Object *obj)
{
    MsmModemLleState *s = MSM_MODEM_LLE(obj);

    object_initialize_child(obj, "cluster", &s->cluster, TYPE_CPU_CLUSTER);
    s->seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    s->regs = g_hash_table_new(g_direct_hash, g_direct_equal);
}

static const Property lle_props[] = {
    DEFINE_PROP_LINK("ram", MsmModemLleState, ram, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_STRING("firmware", MsmModemLleState, firmware),
    DEFINE_PROP_BOOL("trace", MsmModemLleState, trace, false),
    DEFINE_PROP_UINT32("dump-on-write", MsmModemLleState, dump_on, 0),
};

static void lle_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = lle_realize;
    device_class_set_props(dc, lle_props);
}

static const TypeInfo msm_modem_lle_info = {
    .name = TYPE_MSM_MODEM_LLE,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(MsmModemLleState),
    .instance_init = lle_init,
    .class_init = lle_class_init,
};

static void msm_modem_lle_register(void)
{
    type_register_static(&msm_modem_lle_info);
}
type_init(msm_modem_lle_register)
