/*
 * hexhook: print a vCPU's registers whenever chosen addresses run
 *
 * For reverse-engineering firmware run in system mode (written for the
 * QDSP6 firmware on qemu-system-hexagon, works for any target whose
 * registers are named r0.. / r00..):
 *   -plugin libhexhook.so,hook=0x108c0bb0,hook=...,dump=0x108c0bb0:2:8
 *     hook=VA        print r0-r5 and r31 (lr) of the vCPU when VA runs
 *     dump=VA:R:N    also print N words of memory at the address in rR
 *                    (R 0x... : at that fixed address)
 *     memr=LO-HI     print each instruction that reads data in [LO, HI)
 *     memrp=LO-HI    the same for a physical range
 *     pcr=LO-HI      only watch the instructions in that code range
 *     snap=VA:PA:LEN:FILE  the first time VA runs, save LEN bytes of
 *                    physical memory at PA to FILE
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define NREGS 32

typedef struct {
    uint64_t va;
    int dump_reg, dump_n;
    uint32_t dump_addr;             /* dump_reg < 0: fixed address */
    uint64_t snap_pa, snap_len;
    char *snap_file;                /* saved once, then NULL */
} Hook;

static GArray *hooks;
static uint64_t memr_lo, memr_hi;       /* watched data range */
static bool memr_phys;                  /* the range is physical */
static uint64_t pcr_lo, pcr_hi = ~0ULL;  /* only instrument code here */
static GHashTable *memr_seen;
static long memr_all;                   /* log every read, this many */
static int mem_stores;                  /* 1 stores instead of loads, 2 both */
static struct qemu_plugin_register *regs[NREGS];
static bool have_reg[NREGS];        /* r0's handle may be NULL */

static uint32_t reg(int n)
{
    g_autoptr(GByteArray) buf = g_byte_array_new();
    uint32_t v = 0;

    if (!have_reg[n] || !qemu_plugin_read_register(regs[n], buf) || buf->len < 4) {
        return 0;
    }
    memcpy(&v, buf->data, 4);
    return v;
}

static void on_exec(unsigned int vcpu, void *udata)
{
    Hook *h = &g_array_index(hooks, Hook, GPOINTER_TO_UINT(udata));
    g_autoptr(GString) s = g_string_new(NULL);

    g_string_printf(s, "hexhook: cpu%u %08" PRIx64 " r0 %08x r1 %08x r2 %08x"
                    " r3 %08x r4 %08x r5 %08x lr %08x", vcpu, h->va, reg(0),
                    reg(1), reg(2), reg(3), reg(4), reg(5), reg(31));
    if (h->dump_n) {
        g_autoptr(GByteArray) mem = g_byte_array_new();
        uint32_t a = h->dump_reg < 0 ? h->dump_addr : reg(h->dump_reg);

        g_string_append_printf(s, "\n  [r%d %08x]", h->dump_reg, a);
        if (qemu_plugin_read_memory_vaddr(a, mem, h->dump_n * 4)) {
            for (int i = 0; i < h->dump_n; i++) {
                uint32_t w;

                memcpy(&w, mem->data + i * 4, 4);
                g_string_append_printf(s, " %08x", w);
            }
        } else {
            g_string_append(s, " unreadable");
        }
    }
    if (h->snap_file) {
        FILE *f = fopen(h->snap_file, "wb");

        for (uint64_t o = 0; f && o < h->snap_len; o += 0x10000) {
            g_autoptr(GByteArray) b = g_byte_array_new();
            size_t n = MIN(0x10000, h->snap_len - o);

            if (qemu_plugin_read_memory_hwaddr(h->snap_pa + o, b, n) !=
                QEMU_PLUGIN_HWADDR_OPERATION_OK) {
                g_byte_array_set_size(b, n);
                memset(b->data, 0, n);
            }
            fwrite(b->data, 1, n, f);
        }
        if (f) {
            fclose(f);
        }
        g_string_append_printf(s, "\n  saved %s", h->snap_file);
        h->snap_file = NULL;
    }
    g_string_append_c(s, '\n');
    qemu_plugin_outs(s->str);
}

static void on_mem(unsigned int vcpu, qemu_plugin_meminfo_t info,
                   uint64_t vaddr, void *udata)
{
    uint64_t pc = (uint64_t)(uintptr_t)udata;
    gpointer key = (gpointer)(uintptr_t)pc;

    uint64_t a = vaddr;

    bool st = qemu_plugin_mem_is_store(info);

    if (mem_stores < 2 && st != mem_stores) {
        return;
    }
    if (memr_phys) {
        struct qemu_plugin_hwaddr *hw = qemu_plugin_get_hwaddr(info, vaddr);

        if (!hw || qemu_plugin_hwaddr_is_io(hw)) {
            return;
        }
        a = qemu_plugin_hwaddr_phys_addr(hw);
    }
    if (a < memr_lo || a >= memr_hi) {
        return;
    }
    if (memr_all > 0) {
        memr_all--;
    } else if (g_hash_table_contains(memr_seen, key)) {
        return;
    }
    g_hash_table_add(memr_seen, key);
    uint64_t pa = 0;
    struct qemu_plugin_hwaddr *hwp = qemu_plugin_get_hwaddr(info, vaddr);

    if (hwp && !qemu_plugin_hwaddr_is_io(hwp)) {
        pa = qemu_plugin_hwaddr_phys_addr(hwp);
    }
    qemu_plugin_mem_value v = qemu_plugin_mem_get_value(info);
    uint64_t val = v.type == QEMU_PLUGIN_MEM_VALUE_U8 ? v.data.u8 :
                   v.type == QEMU_PLUGIN_MEM_VALUE_U16 ? v.data.u16 :
                   v.type == QEMU_PLUGIN_MEM_VALUE_U32 ? v.data.u32 :
                   v.data.u64;
    g_autofree char *s = g_strdup_printf("hexhook: %s %08" PRIx64
                                         " (va %08" PRIx64 " pa %08" PRIx64
                                         ") from pc %08" PRIx64 " cpu%u"
                                         " = %" PRIx64 "\n",
                                         st ? "write" : "read", a,
                                         vaddr, pa, pc, vcpu, val);
    qemu_plugin_outs(s);
}

static void on_tb(struct qemu_plugin_tb *tb, void *userdata)
{
    if (memr_hi) {
        for (size_t i = 0; i < qemu_plugin_tb_n_insns(tb); i++) {
            struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
            uint64_t va = qemu_plugin_insn_vaddr(insn);

            if (va < pcr_lo || va >= pcr_hi) {
                continue;
            }
            qemu_plugin_register_vcpu_mem_cb(insn, on_mem,
                    QEMU_PLUGIN_CB_NO_REGS, QEMU_PLUGIN_MEM_RW,
                    (void *)(uintptr_t)qemu_plugin_insn_vaddr(insn));
        }
    }
    for (size_t i = 0; i < qemu_plugin_tb_n_insns(tb); i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint64_t va = qemu_plugin_insn_vaddr(insn);

        for (guint k = 0; k < hooks->len; k++) {
            if (g_array_index(hooks, Hook, k).va == va) {
                qemu_plugin_register_vcpu_insn_exec_cb(insn, on_exec,
                        QEMU_PLUGIN_CB_R_REGS, GUINT_TO_POINTER(k));
            }
        }
    }
}

static void on_vcpu_init(unsigned int vcpu, void *userdata)
{
    g_autoptr(GArray) rl = qemu_plugin_get_registers();

    for (guint i = 0; i < rl->len; i++) {
        qemu_plugin_reg_descriptor *d =
            &g_array_index(rl, qemu_plugin_reg_descriptor, i);

        for (int n = 0; n < NREGS; n++) {
            char a[8], b[8];

            snprintf(a, sizeof(a), "r%d", n);
            snprintf(b, sizeof(b), "r%02d", n);
            if (!strcmp(d->name, a) || !strcmp(d->name, b)) {
                regs[n] = d->handle;
                have_reg[n] = true;
            }
        }
    }
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    hooks = g_array_new(false, true, sizeof(Hook));
    for (int i = 0; i < argc; i++) {
        g_auto(GStrv) kv = g_strsplit(argv[i], "=", 2);

        if (!kv[0] || !kv[1]) {
            fprintf(stderr, "hexhook: bad option %s\n", argv[i]);
            return -1;
        }
        if (!strcmp(kv[0], "hook")) {
            Hook h = { .va = strtoull(kv[1], NULL, 0) };

            g_array_append_val(hooks, h);
        } else if (!strcmp(kv[0], "memr") || !strcmp(kv[0], "memrp")) {
            char *end;

            memr_phys = !strcmp(kv[0], "memrp");
            memr_lo = strtoull(kv[1], &end, 0);
            memr_hi = *end == '-' ? strtoull(end + 1, NULL, 0) : memr_lo + 4;
            memr_seen = g_hash_table_new(g_direct_hash, g_direct_equal);
        } else if (!strcmp(kv[0], "stores")) {
            mem_stores = atoi(kv[1]);
        } else if (!strcmp(kv[0], "memall")) {
            memr_all = strtol(kv[1], NULL, 0);
        } else if (!strcmp(kv[0], "pcr")) {
            char *end;

            pcr_lo = strtoull(kv[1], &end, 0);
            pcr_hi = *end == '-' ? strtoull(end + 1, NULL, 0) : pcr_lo + 4;
        } else if (!strcmp(kv[0], "snap")) {
            g_auto(GStrv) f = g_strsplit(kv[1], ":", 4);
            Hook h = { 0 };

            if (!f[0] || !f[1] || !f[2] || !f[3]) {
                fprintf(stderr, "hexhook: snap=VA:PA:LEN:FILE\n");
                return -1;
            }
            h.va = strtoull(f[0], NULL, 0);
            h.snap_pa = strtoull(f[1], NULL, 0);
            h.snap_len = strtoull(f[2], NULL, 0);
            h.snap_file = g_strdup(f[3]);
            g_array_append_val(hooks, h);
        } else if (!strcmp(kv[0], "dump")) {
            g_auto(GStrv) f = g_strsplit(kv[1], ":", 3);
            Hook h = { 0 };

            if (!f[0] || !f[1] || !f[2]) {
                fprintf(stderr, "hexhook: dump=VA:R:N\n");
                return -1;
            }
            h.va = strtoull(f[0], NULL, 0);
            if (!strncmp(f[1], "0x", 2)) {
                h.dump_reg = -1;
                h.dump_addr = strtoul(f[1], NULL, 0);
            } else {
                h.dump_reg = atoi(f[1]);
            }
            h.dump_n = atoi(f[2]);
            g_array_append_val(hooks, h);
        } else {
            fprintf(stderr, "hexhook: unknown option %s\n", kv[0]);
            return -1;
        }
    }
    qemu_plugin_register_vcpu_init_cb(id, on_vcpu_init, NULL);
    qemu_plugin_register_vcpu_tb_trans_cb(id, on_tb, NULL);
    return 0;
}
