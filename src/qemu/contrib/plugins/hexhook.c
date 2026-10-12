/*
 * hexhook: print a vCPU's registers whenever chosen addresses run
 *
 * For reverse-engineering firmware run in system mode (written for the
 * QDSP6 firmware on qemu-system-hexagon, works for any target whose
 * registers are named r0.. / r00..):
 *   -plugin libhexhook.so,hook=0x108c0bb0,hook=...,dump=0x108c0bb0:2:8
 *     hook=VA        print r0-r5 and r31 (lr) of the vCPU when VA runs
 *     dump=VA:R:N    also print N words of memory at the address in rR
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
} Hook;

static GArray *hooks;
static struct qemu_plugin_register *regs[NREGS];

static uint32_t reg(int n)
{
    g_autoptr(GByteArray) buf = g_byte_array_new();
    uint32_t v = 0;

    if (!regs[n] || !qemu_plugin_read_register(regs[n], buf) || buf->len < 4) {
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
        uint32_t a = reg(h->dump_reg);

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
    g_string_append_c(s, '\n');
    qemu_plugin_outs(s->str);
}

static void on_tb(struct qemu_plugin_tb *tb, void *userdata)
{
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
        } else if (!strcmp(kv[0], "dump")) {
            g_auto(GStrv) f = g_strsplit(kv[1], ":", 3);
            Hook h;

            if (!f[0] || !f[1] || !f[2]) {
                fprintf(stderr, "hexhook: dump=VA:R:N\n");
                return -1;
            }
            h.va = strtoull(f[0], NULL, 0);
            h.dump_reg = atoi(f[1]);
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
