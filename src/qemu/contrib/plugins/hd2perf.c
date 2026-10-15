/*
 * hd2perf: run an ARM guest at the speed of a real HTC HD2 (Qualcomm
 * Scorpion at ~1 GHz, 32 KiB L1D, 256 KiB L2, LPDDR1).
 *
 * Every instruction has a cost, by class (from its disassembly), and every
 * data access goes through a model of the L1 and L2 caches; misses cost
 * what the memory system would.  The costs drive virtual time through
 * qemu_plugin_icount_cost(), so the guest's clocks (and so its benchmarks)
 * see an HD2.  Run with -icount shift=0,sleep=on.
 *
 * Costs are in units of 1/UNITS ns.  Options (name=value):
 *   pace=on|off      keep virtual time from running ahead of real time (on)
 *   skip=N           a vCPU whose time does not count (1: the modem core,
 *                    which runs in parallel on the real machine); -1 none
 *   skipmips=M       its instruction rate, in MIPS (400); its instructions
 *                    move time only while the main core sleeps
 *   timing=off       only the caches and DMA coherency, at normal speed
 *                    (no -icount, no cost model)
 *   start=on|off     model the costs from the start (on), or run at the
 *                    plain -icount rate until switched on
 *   ctl=FILE         switch with FILE: "on" or "off" (checked 5 times a
 *                    second); off, nothing is instrumented, so a guest can
 *                    boot at the usual speed and be measured afterwards
 *   profile=FILE     every second, append per-class counts and cache
 *                    statistics to FILE (for calibration)
 *   <class>=U        the cost of an instruction class (see cost_names)
 *   acc=U l2hit=U mem=U memseq=U   the cost of each data access, of an L1
 *                    miss that hits L2, of a random L2 miss, of an L2 miss
 *                    that continues a sequential stream
 *   io=U ustore=U    an access to a device, a store to uncached memory
 *
 * The caches follow the page tables' memory types (write-back, write-
 * through, non-cacheable, device) and the CP15 c7 maintenance operations,
 * and keep DRAM's view of the lines where it differs from the CPU's: the
 * data mover (DMA) reads and writes DRAM through the plugin's DMA snoop.
 * Firmware that does not clean its command lists and buffers before DMA,
 * or invalidate them after, then fails as on the phone, and a line in the
 * log (and on stderr) names the address and the code that stored it.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define UNITS       4           /* cost units per ns */
#define MAX_CPUS    16

enum {
    C_ALU, C_BRANCH, C_LOAD, C_STORE, C_MUL, C_MULL, C_DIV,
    C_VMOV, C_VFPS, C_VFPD, C_VMACD, C_VDIV, C_NEON, C_NEONMAC,
    C_VLOAD, C_VSTORE, C_SYS, C_COUNT
};

static const char *cost_names[C_COUNT] = {
    "alu", "branch", "load", "store", "mul", "mull", "div",
    "vmov", "vfps", "vfpd", "vmacd", "vdiv", "neon", "neonmac",
    "vload", "vstore", "sys",
};

/* calibrated against a real HD2 (hd2bench) */
static uint64_t cost[C_COUNT] = {
    [C_ALU] = 2, [C_BRANCH] = 3, [C_LOAD] = 3, [C_STORE] = 2,
    [C_MUL] = 6, [C_MULL] = 8, [C_DIV] = 40,
    [C_VMOV] = 4, [C_VFPS] = 4, [C_VFPD] = 8, [C_VMACD] = 12,
    [C_VDIV] = 80, [C_NEON] = 4, [C_NEONMAC] = 6,
    [C_VLOAD] = 3, [C_VSTORE] = 2, [C_SYS] = 20,
};
static uint64_t cost_acc = 1, cost_l2hit = 40, cost_mem = 1000,
                cost_memseq = 200, cost_io = 400, cost_ustore = 20;

/* ---- caches ----------------------------------------------------------- */

/*
 * Scorpion's 32-byte lines, which the htcleo machine also reports in CTR,
 * so that the guest's maintenance by MVA covers what the model holds.
 */
#define LINE_SHIFT  5
#define LINE        (1u << LINE_SHIFT)

enum { F_VALID = 1, F_DIRTY = 2 };

typedef struct {
    unsigned sets, ways;
    uint64_t *tag;              /* sets * ways: the line number */
    uint8_t *flag;              /* F_VALID, F_DIRTY */
    uint8_t *age;               /* LRU age per way */
} Cache;

typedef struct {
    Cache l1, l2;
    uint64_t last_miss[4];      /* recent L2-miss lines, for streams */
    unsigned next_slot;
    uint64_t l1miss, l2miss, seqmiss, accesses;
} CpuState;

static CpuState cpus[MAX_CPUS];
static int skip_cpu = 1;
static uint64_t skip_mips = 400;
static bool pace = true;
static struct qemu_plugin_scoreboard *score;
static qemu_plugin_u64 cost_counter;
static qemu_plugin_u64 class_count[C_COUNT];
static char *profile_path, *ctl_path;
static qemu_plugin_id_t plugin_id;
static gint active_i = 1;     /* modelling costs (a gint for g_atomic) */
static bool timing = true;      /* timing=off: the caches only */
static bool no_mem, no_disas;    /* debugging */
static GMutex lock;

static void cache_init(Cache *c, unsigned size, unsigned ways)
{
    c->ways = ways;
    c->sets = size / LINE / ways;
    c->tag = g_new0(uint64_t, c->sets * ways);
    c->flag = g_new0(uint8_t, c->sets * ways);
    c->age = g_new0(uint8_t, c->sets * ways);
}

/* the way holding line, or -1 */
static int cache_find(Cache *c, uint64_t line)
{
    unsigned base = (line % c->sets) * c->ways;

    for (unsigned w = 0; w < c->ways; w++) {
        if ((c->flag[base + w] & F_VALID) && c->tag[base + w] == line) {
            return base + w;
        }
    }
    return -1;
}

/*
 * Look up (and fill on a miss) line; returns its slot, *hit, and the line
 * a fill evicted (*victim, with its flags in *vflags, 0 if none).
 */
static int cache_access(Cache *c, uint64_t line, bool *hit,
                        uint64_t *victim, uint8_t *vflags)
{
    unsigned base = (line % c->sets) * c->ways, slot = base;
    int found = cache_find(c, line);

    *vflags = 0;
    *hit = found >= 0;
    if (found < 0) {
        for (unsigned w = 0; w < c->ways; w++) {
            if (!(c->flag[base + w] & F_VALID)) {
                slot = base + w;
                break;
            }
            if (c->age[base + w] > c->age[slot]) {
                slot = base + w;
            }
        }
        *victim = c->tag[slot];
        *vflags = c->flag[slot];
        c->tag[slot] = line;
        c->flag[slot] = F_VALID;
        found = slot;
    }
    for (unsigned w = 0; w < c->ways; w++) {
        if (c->age[base + w] < 255) {
            c->age[base + w]++;
        }
    }
    c->age[found] = 0;
    return found;
}

/* ---- DMA coherency ------------------------------------------------------
 *
 * A write-back cache keeps the CPU's stores from DRAM until the line is
 * cleaned or evicted, and keeps serving a line it holds after a device
 * wrote DRAM under it, until the line is invalidated.  Guest memory (what
 * QEMU's RAM holds) is the CPU's view; for the lines where DRAM differs,
 * DRAM's contents are kept here, and the data mover is given those
 * (plugin DMA snoop).  So firmware that forgets its cache maintenance
 * fails here the way it fails on the phone, and the log says where.
 */
enum { L_DIRTY = 1, L_STALE = 2 };

typedef struct {
    uint8_t state;              /* L_DIRTY: the CPU's copy is newer;
                                   L_STALE: DRAM's is (a device wrote it) */
    bool warned;
    uint64_t pc;                /* the store that dirtied it */
    uint64_t va;                /* through this address */
    uint8_t attr;               /* and memory type */
    uint8_t dram[LINE];         /* what DRAM holds */
} CohLine;

static GHashTable *coh;         /* line number -> CohLine */
static uint64_t coh_warnings[4];

static void coh_warn(int kind, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void coh_warn(int kind, const char *fmt, ...)
{
    va_list ap;
    g_autofree char *msg = NULL;
    uint64_t n = ++coh_warnings[kind];

    if (n > 32 && n % 1000) {
        return;
    }
    va_start(ap, fmt);
    msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    qemu_plugin_outs(msg);
    fprintf(stderr, "%s", msg);
    if (n == 32) {
        qemu_plugin_outs("hd2perf: (more of these are counted, one in 1000 "
                         "shown)\n");
    }
}

static void ram_read(uint64_t pa, uint8_t *buf, size_t len)
{
    g_autoptr(GByteArray) data = g_byte_array_new();

    if (qemu_plugin_read_memory_hwaddr(pa, data, len) ==
            QEMU_PLUGIN_HWADDR_OPERATION_OK && data->len >= len) {
        memcpy(buf, data->data, len);
    } else {
        memset(buf, 0, len);
    }
}

static void ram_write(uint64_t pa, const uint8_t *buf, size_t len)
{
    g_autoptr(GByteArray) data = g_byte_array_new();

    g_byte_array_append(data, buf, len);
    qemu_plugin_write_memory_hwaddr(pa, data);
}

static CohLine *coh_get(uint64_t line)
{
    return coh ? g_hash_table_lookup(coh, &line) : NULL;
}

static CohLine *coh_new(uint64_t line)
{
    uint64_t *key = g_new(uint64_t, 1);
    CohLine *e = g_new0(CohLine, 1);

    *key = line;
    g_hash_table_insert(coh, key, e);
    return e;
}

/* the CPU drops its copy of a line (invalidate, or eviction of a clean one) */
static void coh_drop(uint64_t line, bool write_back)
{
    CohLine *e = coh_get(line);

    if (!e) {
        return;
    }
    /*
     * Written back, the CPU's copy is DRAM's; dropped without, the CPU
     * reads DRAM from now on (losing what it had not cleaned).
     */
    if (!(write_back && (e->state & L_DIRTY))) {
        ram_write(line << LINE_SHIFT, e->dram, LINE);
    }
    g_hash_table_remove(coh, &line);
}

/* the L2 evicted a line: a dirty one is written back */
static void coh_evicted(uint64_t line, uint8_t flags)
{
    if (flags & F_VALID) {
        coh_drop(line, flags & F_DIRTY);
    }
}

/* the first store to a clean write-back line: DRAM keeps the old data */
static void coh_dirty(uint64_t line, uint64_t pa, unsigned size, uint64_t pc,
                      uint64_t va, uint8_t attr)
{
    CohLine *e = coh_get(line);

    if (!e) {
        e = coh_new(line);
        /*
         * The store already happened; what DRAM holds for its bytes is not
         * known, so they read as zero.
         */
        ram_read(line << LINE_SHIFT, e->dram, LINE);
        memset(e->dram + (pa & (LINE - 1)), 0,
               MIN(size, LINE - (pa & (LINE - 1))));
    }
    e->state |= L_DIRTY;
    e->pc = pc;
    e->va = va;
    e->attr = attr;
    e->warned = false;
}

enum { OP_CLEAN = 1, OP_INVAL = 2 };

/* cache maintenance on one line of cpu's caches */
static void coh_line_op(CpuState *s, uint64_t line, int op)
{
    int slot = cache_find(&s->l2, line);
    int l1 = cache_find(&s->l1, line);

    if (slot >= 0 && (op & OP_CLEAN)) {
        CohLine *e = coh_get(line);

        if (e && (e->state & L_DIRTY)) {
            if (e->state & L_STALE) {
                /* the device's data is overwritten by the CPU's */
                g_hash_table_remove(coh, &line);
            } else {
                g_hash_table_remove(coh, &line);
            }
        }
        s->l2.flag[slot] &= ~F_DIRTY;
    }
    if (op & OP_INVAL) {
        coh_drop(line, false);
        if (slot >= 0) {
            s->l2.flag[slot] = 0;
        }
        if (l1 >= 0) {
            s->l1.flag[l1] = 0;
        }
    }
}

/* clean and/or invalidate the whole data cache */
static void coh_all_op(CpuState *s, int op)
{
    for (unsigned i = 0; i < s->l2.sets * s->l2.ways; i++) {
        if (s->l2.flag[i] & F_VALID) {
            coh_line_op(s, s->l2.tag[i], op);
        }
    }
    if (op & OP_INVAL) {
        memset(s->l1.flag, 0, s->l1.sets * s->l1.ways);
    }
}

/* the data mover reads or writes DRAM */
static bool dma_snoop(uint64_t addr, void *buf, uint64_t len, bool is_write)
{
    CpuState *s = &cpus[0];
    bool touched = false;

    if (!g_atomic_int_get(&active_i) || !s->l2.tag || !len) {
        return false;
    }
    g_mutex_lock(&lock);
    for (uint64_t off = 0; off < len;) {
        uint64_t pa = addr + off, line = pa >> LINE_SHIFT;
        unsigned in = pa & (LINE - 1), n = MIN(len - off, LINE - in);
        CohLine *e = coh_get(line);

        if (!is_write) {
            if (e) {
                memcpy((uint8_t *)buf + off, e->dram + in, n);
                if ((e->state & L_DIRTY) && !e->warned) {
                    e->warned = true;
                    coh_warn(0, "hd2perf: DMA read of 0x%08" PRIx64 ": the "
                             "line is dirty in the CPU cache (stored by pc "
                             "0x%08" PRIx64 ") and was not cleaned, so the "
                             "device reads stale DRAM\n", pa, e->pc);
                }
            }
        } else if (e || cache_find(&s->l2, line) >= 0) {
            /* the CPU holds the line: DRAM changes under its copy */
            if (!e) {
                e = coh_new(line);
                ram_read(line << LINE_SHIFT, e->dram, LINE);
            }
            if (e->state & L_DIRTY) {
                coh_warn(1, "hd2perf: DMA write to 0x%08" PRIx64 ": the line "
                         "is dirty in the CPU cache (pc 0x%08" PRIx64 ", va "
                         "0x%08" PRIx64 ", memory type 0x%02x), which will "
                         "overwrite the device's data unless it is "
                         "invalidated first\n", pa, e->pc, e->va, e->attr);
            }
            memcpy(e->dram + in, (uint8_t *)buf + off, n);
            e->state |= L_STALE;
            e->warned = false;
            touched = true;
        }
        off += n;
    }
    if (is_write && touched) {
        /* the lines nobody caches go straight to memory */
        for (uint64_t off = 0; off < len;) {
            uint64_t pa = addr + off, line = pa >> LINE_SHIFT;
            unsigned in = pa & (LINE - 1), n = MIN(len - off, LINE - in);

            if (!coh_get(line)) {
                ram_write(pa, (uint8_t *)buf + off, n);
            }
            off += n;
        }
    }
    g_mutex_unlock(&lock);
    return touched;
}

/* forget the caches (accurate mode off): DRAM and the CPU agree again */
static void coh_flush(void)
{
    GHashTableIter it;
    gpointer k, v;

    g_mutex_lock(&lock);
    if (coh) {
        g_hash_table_iter_init(&it, coh);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            CohLine *e = v;

            if (!(e->state & L_DIRTY)) {
                ram_write(*(uint64_t *)k << LINE_SHIFT, e->dram, LINE);
            }
        }
        g_hash_table_remove_all(coh);
    }
    for (int i = 0; i < MAX_CPUS; i++) {
        if (cpus[i].l2.flag) {
            memset(cpus[i].l2.flag, 0, cpus[i].l2.sets * cpus[i].l2.ways);
            memset(cpus[i].l1.flag, 0, cpus[i].l1.sets * cpus[i].l1.ways);
        }
    }
    g_mutex_unlock(&lock);
}

/* ---- data accesses -------------------------------------------------------- */

static void vcpu_mem(unsigned int vcpu, qemu_plugin_meminfo_t info,
                     uint64_t vaddr, void *udata)
{
    CpuState *s = &cpus[vcpu % MAX_CPUS];
    struct qemu_plugin_hwaddr *hw;
    bool store = qemu_plugin_mem_is_store(info), hit;
    unsigned size = 1u << qemu_plugin_mem_size_shift(info);
    uint64_t c = cost_acc, pa, line, victim;
    uint8_t attr, vflags;
    int slot;

    /* the modem core's time does not count; a vCPU can run before its init */
    if ((int)vcpu == skip_cpu || vcpu >= (unsigned)qemu_plugin_num_vcpus()) {
        return;
    }
    hw = qemu_plugin_get_hwaddr(info, vaddr);
    if (!hw || qemu_plugin_hwaddr_is_io(hw)) {
        qemu_plugin_u64_add(cost_counter, vcpu, c + cost_io);
        return;
    }
    pa = qemu_plugin_hwaddr_phys_addr(hw);
    line = pa >> LINE_SHIFT;
    attr = qemu_plugin_hwaddr_memattr(hw) & 0x0f;   /* inner policy */
    s->accesses++;

    if (attr < 0x8) {
        /* device or non-cacheable: straight to DRAM (stores are buffered) */
        if (store) {
            CohLine *e;

            g_mutex_lock(&lock);
            if ((e = coh_get(line))) {
                ram_read(pa & ~(uint64_t)(LINE - 1), e->dram, LINE);
            }
            g_mutex_unlock(&lock);
        }
        qemu_plugin_u64_add(cost_counter, vcpu,
                            c + (store ? cost_ustore : cost_mem));
        return;
    }

    cache_access(&s->l1, line, &hit, &victim, &vflags);
    if (!hit) {
        s->l1miss++;
        slot = cache_access(&s->l2, line, &hit, &victim, &vflags);
        if (hit) {
            c += cost_l2hit;
        } else {
            bool seq = false;

            for (int i = 0; i < 4; i++) {
                if (s->last_miss[i] && (line == s->last_miss[i] + 1 ||
                                        line + 1 == s->last_miss[i])) {
                    s->last_miss[i] = line;
                    seq = true;
                    break;
                }
            }
            if (!seq) {
                s->last_miss[s->next_slot++ % 4] = line;
            }
            s->l2miss++;
            s->seqmiss += seq;
            c += seq ? cost_memseq : cost_mem;
            if ((vflags & F_VALID) && (vflags & F_DIRTY || coh)) {
                g_mutex_lock(&lock);
                coh_evicted(victim, vflags);
                g_mutex_unlock(&lock);
            }
        }
    } else {
        slot = cache_find(&s->l2, line);
    }

    if (store && slot >= 0) {
        if (attr == 0xf) {
            /* write-back: the first store keeps the line from DRAM */
            if (!(s->l2.flag[slot] & F_DIRTY)) {
                s->l2.flag[slot] |= F_DIRTY;
                g_mutex_lock(&lock);
                coh_dirty(line, pa, size, (uint64_t)(uintptr_t)udata, vaddr,
                          qemu_plugin_hwaddr_memattr(hw));
                g_mutex_unlock(&lock);
            }
        } else {
            /* write-through: DRAM gets the store too */
            CohLine *e;

            g_mutex_lock(&lock);
            if ((e = coh_get(line)) && !(e->state & L_DIRTY)) {
                ram_read(pa, e->dram + (pa & (LINE - 1)),
                         MIN(size, LINE - (pa & (LINE - 1))));
            }
            g_mutex_unlock(&lock);
        }
    } else if (!store && slot >= 0 && coh) {
        CohLine *e = coh_get(line);

        if (e && (e->state & L_STALE) && !e->warned) {
            e->warned = true;
            coh_warn(2, "hd2perf: CPU read of 0x%08" PRIx64 " at pc 0x%08"
                     PRIx64 " hits a cached line a device has written since"
                     " (no invalidate): it reads the old data\n", pa,
                     (uint64_t)(uintptr_t)udata);
        }
    }
    qemu_plugin_u64_add(cost_counter, vcpu, c);
}

/* ---- cache maintenance (CP15 c7) ------------------------------------------- */

static GArray *regs;            /* the vCPU's registers, for the operand */
static struct qemu_plugin_register *reg_handle[16];
static bool reg_found[16];      /* a handle can be 0 (r0's is) */

static uint64_t read_reg(int r)
{
    g_autoptr(GByteArray) buf = g_byte_array_new();

    if (r < 0 || r > 14 || !reg_found[r] ||
        !qemu_plugin_read_register(reg_handle[r], buf) || buf->len < 4) {
        return 0;
    }
    return *(uint32_t *)buf->data;
}

/* udata: rt | crm << 8 | opc2 << 16 */
static void vcpu_cacheop(unsigned int vcpu, void *udata)
{
    CpuState *s = &cpus[vcpu % MAX_CPUS];
    unsigned v = (uintptr_t)udata, rt = v & 0xff, crm = (v >> 8) & 0xff;
    unsigned opc2 = v >> 16;
    int op;
    uint64_t pa, val;

    if ((int)vcpu == skip_cpu || !s->l2.tag) {
        return;
    }
    switch (crm) {
    case 6:  op = OP_INVAL; break;
    case 10: op = OP_CLEAN; break;
    case 14: op = OP_CLEAN | OP_INVAL; break;
    default: return;    /* c11 (to the PoU) does not reach DRAM */
    }
    val = read_reg(rt);
    g_mutex_lock(&lock);
    if (opc2 == 1) {
        /* by MVA */
        if (qemu_plugin_translate_vaddr(val, &pa)) {
            coh_line_op(s, pa >> LINE_SHIFT, op);
        }
    } else if (opc2 == 0 || (opc2 == 2 && (val & ~0xe) == 0)) {
        /*
         * The whole cache (ARMv6), or set/way (a loop over all of them:
         * done once, at set 0 way 0 of each level)
         */
        coh_all_op(s, op);
    }
    g_mutex_unlock(&lock);
}

/* "mcr p15, #0, r0, c7, c10, #1" (or "15, 0, r0, cr7, cr10, {1}") */
static bool parse_cacheop(const char *dis, unsigned *rt, unsigned *crm,
                          unsigned *opc2)
{
    g_auto(GStrv) f = NULL;
    const char *p = strstr(dis, "mcr");
    int n;

    if (!p || strstr(dis, "mcrr")) {
        return false;
    }
    p += 3;
    while (*p && *p != ' ' && *p != '\t') {
        p++;                    /* condition suffix */
    }
    f = g_strsplit_set(p, ",", -1);
    if (g_strv_length(f) < 6) {
        return false;
    }
    for (n = 0; n < 6; n++) {
        g_strstrip(f[n]);
    }
    if (atoi(f[0] + strspn(f[0], "p")) != 15 || atoi(f[1] + strspn(f[1], "#")) ||
        atoi(f[3] + strspn(f[3], "cr")) != 7) {
        return false;
    }
    if (!strcmp(f[2], "sp")) {
        *rt = 13;
    } else if (!strcmp(f[2], "lr")) {
        *rt = 14;
    } else if (f[2][0] == 'r') {
        *rt = atoi(f[2] + 1);
    } else {
        return false;
    }
    *crm = atoi(f[4] + strspn(f[4], "cr"));
    *opc2 = atoi(f[5] + strspn(f[5], "#{"));
    return true;
}

/* ---- instruction classes ---------------------------------------------- */

static bool starts(const char *m, const char *p)
{
    return !strncmp(m, p, strlen(p));
}

static int classify(const char *dis)
{
    char m[32];
    int n = 0;

    while (*dis == ' ' || *dis == '\t') {
        dis++;
    }
    while (dis[n] && dis[n] != ' ' && dis[n] != '\t' && n < 31) {
        m[n] = g_ascii_tolower(dis[n]);
        n++;
    }
    m[n] = 0;

    if (m[0] == 'v') {
        bool f64 = strstr(dis, ".f64") != NULL;
        bool q = strstr(dis, " q") || strstr(dis, ", q") || strstr(dis, "{q");

        if (starts(m, "vld") || starts(m, "vpop")) {
            return C_VLOAD;
        }
        if (starts(m, "vst") || starts(m, "vpush")) {
            return C_VSTORE;
        }
        if (starts(m, "vdiv") || starts(m, "vsqrt")) {
            return C_VDIV;
        }
        if (starts(m, "vmov") || starts(m, "vmrs") || starts(m, "vmsr") ||
            starts(m, "vdup") || starts(m, "vcvt")) {
            return f64 && starts(m, "vcvt") ? C_VFPD : C_VMOV;
        }
        if (starts(m, "vmla") || starts(m, "vmls") || starts(m, "vnmla") ||
            starts(m, "vnmls") || starts(m, "vfma") || starts(m, "vfms")) {
            return f64 ? C_VMACD : (q || !strstr(dis, ".f32") ? C_NEONMAC
                                                              : C_VFPS);
        }
        if (f64) {
            return C_VFPD;
        }
        /* single precision VFP works on s registers */
        if (strstr(dis, ".f32") && !q && !strstr(dis, " d")) {
            return C_VFPS;
        }
        return C_NEON;
    }
    if (starts(m, "ldr") || starts(m, "ldm") || starts(m, "pop") ||
        starts(m, "ldrex") || starts(m, "pld")) {
        return strstr(dis, "pc") && !starts(m, "pld") ? C_BRANCH : C_LOAD;
    }
    if (starts(m, "str") || starts(m, "stm") || starts(m, "push") ||
        starts(m, "strex")) {
        return C_STORE;
    }
    if (starts(m, "umull") || starts(m, "smull") || starts(m, "umlal") ||
        starts(m, "smlal") || starts(m, "smmul") || starts(m, "smmla")) {
        return C_MULL;
    }
    if (starts(m, "mul") || starts(m, "mla") || starts(m, "mls") ||
        starts(m, "smul") || starts(m, "smla") || starts(m, "smuad")) {
        return C_MUL;
    }
    if (starts(m, "sdiv") || starts(m, "udiv")) {
        return C_DIV;
    }
    if (starts(m, "mcr") || starts(m, "mrc") || starts(m, "svc") ||
        starts(m, "cps") || starts(m, "msr") || starts(m, "mrs") ||
        starts(m, "wfi") || starts(m, "dsb") || starts(m, "dmb") ||
        starts(m, "isb")) {
        return C_SYS;
    }
    if (m[0] == 'b' || starts(m, "cbz") || starts(m, "cbnz") ||
        starts(m, "tbb") || starts(m, "tbh")) {
        if (starts(m, "bic") || starts(m, "bfc") || starts(m, "bfi")) {
            return C_ALU;
        }
        return C_BRANCH;
    }
    if (strstr(dis, " pc,") || strstr(dis, "\tpc,")) {
        return C_BRANCH;
    }
    return C_ALU;
}

static bool mem_class(int c)
{
    return c == C_LOAD || c == C_STORE || c == C_VLOAD || c == C_VSTORE;
}

/*
 * Normal speed (timing=off): only stores are watched, and only the first
 * store to a clean line does any work.  The dirty lines live in the L2's
 * worth of lines, so the oldest are written back as a real cache would.
 */
static void vcpu_store(unsigned int vcpu, qemu_plugin_meminfo_t info,
                       uint64_t vaddr, void *udata)
{
    CpuState *s = &cpus[vcpu % MAX_CPUS];
    struct qemu_plugin_hwaddr *hw;
    uint64_t pa, line, victim;
    uint8_t attr, vflags;
    bool hit;
    int slot;

    if ((int)vcpu == skip_cpu || vcpu >= (unsigned)qemu_plugin_num_vcpus() ||
        !s->l2.tag) {
        return;
    }
    hw = qemu_plugin_get_hwaddr(info, vaddr);
    if (!hw || qemu_plugin_hwaddr_is_io(hw)) {
        return;
    }
    attr = qemu_plugin_hwaddr_memattr(hw) & 0x0f;
    pa = qemu_plugin_hwaddr_phys_addr(hw);
    line = pa >> LINE_SHIFT;
    if (attr != 0xf) {
        /* write-through, uncached: DRAM gets it (if DRAM's copy is kept) */
        if (g_hash_table_size(coh)) {
            CohLine *e;

            g_mutex_lock(&lock);
            if ((e = coh_get(line)) && !(e->state & L_DIRTY)) {
                ram_read(line << LINE_SHIFT, e->dram, LINE);
            }
            g_mutex_unlock(&lock);
        }
        return;
    }
    slot = cache_find(&s->l2, line);
    if (slot >= 0 && (s->l2.flag[slot] & F_DIRTY)) {
        return;
    }
    g_mutex_lock(&lock);
    slot = cache_access(&s->l2, line, &hit, &victim, &vflags);
    if (vflags & F_DIRTY) {
        coh_evicted(victim, vflags);
    }
    s->l2.flag[slot] |= F_DIRTY;
    coh_dirty(line, pa, 1u << qemu_plugin_mem_size_shift(info),
              (uint64_t)(uintptr_t)udata, vaddr, qemu_plugin_hwaddr_memattr(hw));
    g_mutex_unlock(&lock);
}

/* MCR p15, 0, Rt, c7, CRm, opc2, from its encoding (ARM or Thumb-2) */
static bool raw_cacheop(struct qemu_plugin_insn *insn)
{
    uint8_t b[4];
    uint32_t w;
    uint16_t h1, h2;

    if (qemu_plugin_insn_size(insn) != 4 ||
        qemu_plugin_insn_data(insn, b, 4) != 4) {
        return false;
    }
    w = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
    h1 = b[0] | b[1] << 8;
    h2 = b[2] | b[3] << 8;
    return ((w & 0x0fef0f10) == 0x0e070f10) ||
           ((h1 & 0xefff) == 0xee07 && (h2 & 0x0f10) == 0x0f10);
}

static void vcpu_tb_trans_light(struct qemu_plugin_tb *tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);

    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        unsigned rt, crm, opc2;

        qemu_plugin_register_vcpu_mem_cb(
            insn, vcpu_store, QEMU_PLUGIN_CB_NO_REGS, QEMU_PLUGIN_MEM_W,
            (void *)(uintptr_t)qemu_plugin_insn_vaddr(insn));
        if (raw_cacheop(insn)) {
            g_autofree char *dis = qemu_plugin_insn_disas(insn);

            if (dis && parse_cacheop(dis, &rt, &crm, &opc2)) {
                qemu_plugin_register_vcpu_insn_exec_cb(
                    insn, vcpu_cacheop, QEMU_PLUGIN_CB_R_REGS,
                    (void *)(uintptr_t)(rt | crm << 8 | opc2 << 16));
            }
        }
    }
}

static void vcpu_tb_trans(struct qemu_plugin_tb *tb, void *userdata)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    uint64_t per[C_COUNT] = { 0 };

    if (!timing) {
        vcpu_tb_trans_light(tb);
        return;
    }
    if (!g_atomic_int_get((gint *)&active_i)) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        g_autofree char *dis = no_disas ? NULL : qemu_plugin_insn_disas(insn);
        int c = dis ? classify(dis) : C_ALU;

        per[c]++;
        /*
         * Per instruction, not per TB: a TB can stop part way (an icount
         * expiry, an exception) and must only cost what it ran.
         */
        if (timing) {
            qemu_plugin_register_vcpu_insn_exec_inline_per_vcpu(
                insn, QEMU_PLUGIN_INLINE_ADD_U64, cost_counter, cost[c]);
        }
        if (no_mem) {
            continue;
        }
        if (mem_class(c) || (c == C_BRANCH && dis && (strstr(dis, "ldr") ||
                                                       strstr(dis, "pop")))) {
            qemu_plugin_register_vcpu_mem_cb(
                insn, vcpu_mem, QEMU_PLUGIN_CB_NO_REGS, QEMU_PLUGIN_MEM_RW,
                (void *)(uintptr_t)qemu_plugin_insn_vaddr(insn));
        }
        if (c == C_SYS && dis) {
            unsigned rt, crm, opc2;

            if (parse_cacheop(dis, &rt, &crm, &opc2)) {
                qemu_plugin_register_vcpu_insn_exec_cb(
                    insn, vcpu_cacheop, QEMU_PLUGIN_CB_R_REGS,
                    (void *)(uintptr_t)(rt | crm << 8 | opc2 << 16));
            }
        }
    }
    if (profile_path) {
        for (int c = 0; c < C_COUNT; c++) {
            if (per[c]) {
                qemu_plugin_register_vcpu_tb_exec_inline_per_vcpu(
                    tb, QEMU_PLUGIN_INLINE_ADD_U64, class_count[c], per[c]);
            }
        }
    }
}

static void vcpu_init(unsigned int vcpu, void *userdata)
{
    CpuState *s = &cpus[vcpu % MAX_CPUS];

    cache_init(&s->l1, 32 * 1024, 4);
    cache_init(&s->l2, 256 * 1024, 8);
    if ((int)vcpu != skip_cpu && !regs) {
        /* the operands of the cache maintenance operations */
        regs = qemu_plugin_get_registers();
        for (unsigned i = 0; i < regs->len; i++) {
            qemu_plugin_reg_descriptor *d =
                &g_array_index(regs, qemu_plugin_reg_descriptor, i);
            static const char *names[15] = {
                "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9",
                "r10", "r11", "r12", "sp", "lr",
            };

            for (int r = 0; r < 15; r++) {
                if (!strcmp(d->name, names[r])) {
                    reg_handle[r] = d->handle;
                    reg_found[r] = true;
                }
            }
        }
    }
}

static void profile_dump(void)
{
    FILE *f;

    if (!profile_path || qemu_plugin_num_vcpus() < 1 ||
        !(f = fopen(profile_path, "a"))) {
        return;
    }
    fprintf(f, "%" PRId64, g_get_monotonic_time() / 1000);
    for (int c = 0; c < C_COUNT; c++) {
        fprintf(f, " %s=%" PRIu64, cost_names[c],
                qemu_plugin_u64_get(class_count[c], 0));
    }
    fprintf(f, " acc=%" PRIu64 " l1miss=%" PRIu64 " l2miss=%" PRIu64
            " seqmiss=%" PRIu64 " cost=%" PRIu64 "\n", cpus[0].accesses,
            cpus[0].l1miss, cpus[0].l2miss, cpus[0].seqmiss,
            qemu_plugin_u64_get(cost_counter, 0));
    fclose(f);
}

static void plugin_exit(void *p);

static void set_active(bool on)
{
    if (on == !!g_atomic_int_get(&active_i)) {
        return;
    }
    g_atomic_int_set(&active_i, on);
    qemu_plugin_icount_cost_enable(on);
    /*
     * QEMU retranslates everything when it switches clocks (between slices,
     * on the vCPU's thread), and vcpu_tb_trans instruments the new code or
     * not; the caches start empty.
     */
    coh_flush();
}

static void ctl_poll(void)
{
    g_autofree char *buf = NULL;

    if (ctl_path && g_file_get_contents(ctl_path, &buf, NULL, NULL)) {
        g_strstrip(buf);
        if (!strcmp(buf, "on") || !strcmp(buf, "1")) {
            set_active(true);
        } else if (!strcmp(buf, "off") || !strcmp(buf, "0")) {
            set_active(false);
        }
    }
}

static gpointer control_thread(gpointer data)
{
    for (int tick = 1;; tick++) {
        g_usleep(G_USEC_PER_SEC / 5);
        ctl_poll();
        if (profile_path && tick % 5 == 0) {
            g_mutex_lock(&lock);
            profile_dump();
            g_mutex_unlock(&lock);
        }
    }
    return NULL;
}

static void plugin_exit(void *p)
{
    profile_dump();
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        g_auto(GStrv) kv = g_strsplit(argv[i], "=", 2);
        uint64_t v;
        int c;

        if (!kv[0] || !kv[1]) {
            fprintf(stderr, "hd2perf: bad option %s\n", argv[i]);
            return -1;
        }
        if (!strcmp(kv[0], "pace")) {
            if (!qemu_plugin_bool_parse(kv[0], kv[1], &pace)) {
                return -1;
            }
            continue;
        }
        if (!strcmp(kv[0], "timing")) {
            if (!qemu_plugin_bool_parse(kv[0], kv[1], &timing)) {
                return -1;
            }
            continue;
        }
        if (!strcmp(kv[0], "nomem")) {
            no_mem = true;
            continue;
        }
        if (!strcmp(kv[0], "nodisas")) {
            no_disas = true;
            continue;
        }
        if (!strcmp(kv[0], "start")) {
            bool on;

            if (!qemu_plugin_bool_parse(kv[0], kv[1], &on)) {
                return -1;
            }
            active_i = on;
            continue;
        }
        if (!strcmp(kv[0], "ctl")) {
            ctl_path = g_strdup(kv[1]);
            continue;
        }
        if (!strcmp(kv[0], "profile")) {
            profile_path = g_strdup(kv[1]);
            continue;
        }
        v = g_ascii_strtoull(kv[1], NULL, 0);
        if (!strcmp(kv[0], "skip")) {
            skip_cpu = (int)v;
        } else if (!strcmp(kv[0], "skipmips")) {
            skip_mips = v;
        } else if (!strcmp(kv[0], "io")) {
            cost_io = v;
        } else if (!strcmp(kv[0], "ustore")) {
            cost_ustore = v;
        } else if (!strcmp(kv[0], "acc")) {
            cost_acc = v;
        } else if (!strcmp(kv[0], "l2hit")) {
            cost_l2hit = v;
        } else if (!strcmp(kv[0], "mem")) {
            cost_mem = v;
        } else if (!strcmp(kv[0], "memseq")) {
            cost_memseq = v;
        } else {
            for (c = 0; c < C_COUNT && strcmp(kv[0], cost_names[c]); c++) {
            }
            if (c == C_COUNT) {
                fprintf(stderr, "hd2perf: unknown option %s\n", kv[0]);
                return -1;
            }
            cost[c] = v;
        }
    }

    score = qemu_plugin_scoreboard_new(sizeof(uint64_t) * (C_COUNT + 1));
    cost_counter = (qemu_plugin_u64){ score, 0 };
    for (int c = 0; c < C_COUNT; c++) {
        class_count[c] = (qemu_plugin_u64){ score, sizeof(uint64_t) * (c + 1) };
    }
    if (timing &&
        !qemu_plugin_icount_cost(cost_counter, UNITS, pace, skip_cpu, skip_mips)) {
        fprintf(stderr, "hd2perf: needs -icount (shift=0,sleep=on)\n");
        return -1;
    }
    plugin_id = id;
    coh = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
    qemu_plugin_register_dma_snoop(dma_snoop);
    if (timing) {
        qemu_plugin_icount_cost_enable(active_i);
    }
    qemu_plugin_register_vcpu_init_cb(id, vcpu_init, NULL);
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans, NULL);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    if (profile_path || ctl_path) {
        g_thread_new("hd2perf", control_thread, NULL);
    }
    return 0;
}
