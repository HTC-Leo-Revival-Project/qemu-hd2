/*
 * amsslog: print the debug ("F3") messages of Qualcomm AMSS running on a
 * modem core, and report when chosen functions run.
 *
 * AMSS logs through a few msg_send functions that take a pointer to a
 * constant descriptor in r0 and up to three arguments in r1-r3:
 *     +0  u16 line   +8  const char *format   +12  const char *file
 * Hooking those functions gives the firmware's own log, which is often the
 * quickest way to see why it is unhappy.  Only translation of the hooked
 * instructions costs anything, so the guest runs at full speed.
 *
 *   -plugin libamsslog.so,vcpu=1,msg=0x526604,msg=0xc55d88,hook=0x52cb9a
 *     vcpu=N    the vCPU index to watch (default 1: the -M htcleo modem)
 *     msg=VA    a msg_send entry point (any number of them)
 *     hook=VA   print r0-r3 and lr whenever VA runs
 * Output goes to the QEMU log (-D file).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

enum { KIND_MSG, KIND_HOOK };

typedef struct {
    uint64_t va;
    int kind;
} Watch;

static GArray *watches;
static unsigned watch_vcpu = 1;
/* handles are opaque and may be NULL (r0 is register 0) */
static struct qemu_plugin_register *reg_r[4], *reg_lr;
static bool have_regs;

static uint32_t rd_reg(struct qemu_plugin_register *r)
{
    g_autoptr(GByteArray) buf = g_byte_array_new();
    uint32_t v = 0;

    if (have_regs && qemu_plugin_read_register(r, buf) > 0 && buf->len >= 4) {
        memcpy(&v, buf->data, 4);
    }
    return v;
}

static bool rd_mem(uint32_t va, void *out, size_t len)
{
    g_autoptr(GByteArray) buf = g_byte_array_new();

    if (!qemu_plugin_read_memory_vaddr(va, buf, len) || buf->len < len) {
        return false;
    }
    memcpy(out, buf->data, len);
    return true;
}

static void rd_str(uint32_t va, char *out, size_t max)
{
    size_t i;

    for (i = 0; i + 1 < max; i++) {
        char c;
        if (!rd_mem(va + i, &c, 1) || !c) {
            break;
        }
        out[i] = (c >= 32 && c < 127) ? c : '?';
    }
    out[i] = 0;
}

/* a small printf: %d %u %x %X %p %c %s with flags/width, %% */
static void format(GString *o, const char *fmt, const uint32_t *args)
{
    int n = 0;

    for (const char *p = fmt; *p; p++) {
        char spec[16];
        int k = 0;

        if (*p != '%') {
            g_string_append_c(o, *p);
            continue;
        }
        spec[k++] = '%';
        p++;
        while (*p && strchr("-+ #0123456789.lh", *p) && k < 12) {
            if (*p != 'l' && *p != 'h') {
                spec[k++] = *p;
            }
            p++;
        }
        if (!*p) {
            break;
        }
        if (*p == '%') {
            g_string_append_c(o, '%');
            continue;
        }
        if (n >= 3) {
            g_string_append(o, "<?>");
            continue;
        }
        if (*p == 's') {
            char s[96];
            rd_str(args[n++], s, sizeof(s));
            g_string_append(o, s);
            continue;
        }
        spec[k++] = strchr("diuxXpc", *p) ? (*p == 'p' ? 'x' : *p) : 'x';
        spec[k] = 0;
        g_string_append_printf(o, spec, args[n++]);
    }
}

/* host seconds since the plugin loaded (about the guest's time) */
static gint64 t0;
static double now(void)
{
    return (g_get_monotonic_time() - t0) / 1e6;
}

static void on_exec(unsigned int vcpu, void *udata)
{
    const Watch *w = &g_array_index(watches, Watch, GPOINTER_TO_UINT(udata));
    uint32_t r[4], lr = rd_reg(reg_lr);
    g_autoptr(GString) o = g_string_new(NULL);

    if (vcpu != watch_vcpu) {
        return;
    }
    for (int i = 0; i < 4; i++) {
        r[i] = rd_reg(reg_r[i]);
    }
    if (w->kind == KIND_MSG) {
        uint32_t d[4];
        char fmt[160], file[48];

        if (!rd_mem(r[0], d, sizeof(d))) {
            return;
        }
        rd_str(d[2], fmt, sizeof(fmt));
        rd_str(d[3], file, sizeof(file));
        g_string_printf(o, "amss: %6.3f %s:%u: ", now(), file, d[0] & 0xffff);
        format(o, fmt, &r[1]);
        g_string_append_printf(o, "  [lr %08x]\n", lr);
    } else {
        g_string_printf(o, "amss: %6.3f hit %08" PRIx64 " r0 %08x r1 %08x "
                        "r2 %08x r3 %08x lr %08x\n", now(), w->va, r[0], r[1],
                        r[2], r[3], lr);
    }
    qemu_plugin_outs(o->str);
}

static void on_tb(struct qemu_plugin_tb *tb, void *userdata)
{
    size_t n = qemu_plugin_tb_n_insns(tb);

    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint64_t va = qemu_plugin_insn_vaddr(insn);

        for (guint k = 0; k < watches->len; k++) {
            if (g_array_index(watches, Watch, k).va == (va & ~1ULL)) {
                qemu_plugin_register_vcpu_insn_exec_cb(
                    insn, on_exec, QEMU_PLUGIN_CB_R_REGS,
                    GUINT_TO_POINTER(k));
            }
        }
    }
}

static void on_vcpu_init(unsigned int vcpu, void *userdata)
{
    g_autoptr(GArray) regs = NULL;

    if (vcpu != watch_vcpu) {
        return;
    }
    regs = qemu_plugin_get_registers();
    have_regs = true;
    for (guint i = 0; i < regs->len; i++) {
        qemu_plugin_reg_descriptor *d =
            &g_array_index(regs, qemu_plugin_reg_descriptor, i);
        if (!strcmp(d->name, "lr")) {
            reg_lr = d->handle;
        }
        for (int k = 0; k < 4; k++) {
            char nm[4];
            snprintf(nm, sizeof(nm), "r%d", k);
            if (!strcmp(d->name, nm)) {
                reg_r[k] = d->handle;
            }
        }
    }
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    t0 = g_get_monotonic_time();
    watches = g_array_new(false, true, sizeof(Watch));
    for (int i = 0; i < argc; i++) {
        g_auto(GStrv) kv = g_strsplit(argv[i], "=", 2);
        Watch w = { 0 };

        if (!kv[0] || !kv[1]) {
            fprintf(stderr, "amsslog: bad option %s\n", argv[i]);
            return -1;
        }
        if (!strcmp(kv[0], "vcpu")) {
            watch_vcpu = strtoul(kv[1], NULL, 0);
            continue;
        }
        if (!strcmp(kv[0], "msg")) {
            w.kind = KIND_MSG;
        } else if (!strcmp(kv[0], "hook")) {
            w.kind = KIND_HOOK;
        } else {
            fprintf(stderr, "amsslog: unknown option %s\n", kv[0]);
            return -1;
        }
        w.va = strtoull(kv[1], NULL, 0) & ~1ULL;
        g_array_append_val(watches, w);
    }
    qemu_plugin_register_vcpu_init_cb(id, on_vcpu_init, NULL);
    qemu_plugin_register_vcpu_tb_trans_cb(id, on_tb, NULL);
    return 0;
}
