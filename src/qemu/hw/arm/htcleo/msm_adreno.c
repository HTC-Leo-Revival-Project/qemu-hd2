/*
 * Qualcomm Adreno 200 ("Yamato", a20x) GPU of the QSD8250 at 0xA0000000
 *
 * The register interface the kernel's KGSL driver (drivers/video/msm/gpu/
 * kgsl) programs:
 *   - RBBM: chip id, soft reset, interrupt status/mask/ack, idle status;
 *   - MH: the GPU MMU, a single-level page table of 4 KiB pages covering
 *     MH_MMU_VA_RANGE; addresses outside it are physical;
 *   - CP: the command processor.  The ring buffer is executed as soon as
 *     the driver moves CP_RB_WPTR.  The PM4 and PFP microcode the driver
 *     uploads is not run; its packets (type 0/1/2/3) are interpreted here.
 *     The read pointer is written back to CP_RB_RPTR_ADDR, scratch
 *     registers to SCRATCH_ADDR, and CACHE_FLUSH_TS events store the
 *     timestamps KGSL waits for.
 *   - the 3D pipe, whose state lives in the context registers (0x2000+)
 *     and shader constants (0x4000+).  Draws go to msm_adreno_draw.c.
 *
 * GMEM (256 KiB of on-chip render memory) sits at GPU address 0 as
 * RB_EDRAM_INFO says; the CPU never sees it, so its layout is our own.
 *
 * -global msm-adreno.trace=FILE logs every packet the CP executes.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "msm_adreno.h"

/* chip id 0x00020100: core 0, major 2, minor 1, patch 0 (Yamato DX 2.1) */
#define ADRENO_PERIPHID1    0x00
#define ADRENO_PERIPHID2    0x20
#define ADRENO_PATCH_REL    0x01
/* RBBM_STATUS when idle: what kgsl_yamato_is_idle() waits for */
#define ADRENO_RBBM_IDLE    0x110

/* PM4 type 3 opcodes */
#define CP_NOP                  0x10
#define CP_REG_RMW              0x21
#define CP_DRAW_INDX            0x22
#define CP_VIZ_QUERY            0x23
#define CP_SET_STATE            0x25
#define CP_WAIT_FOR_IDLE        0x26
#define CP_IM_LOAD              0x27
#define CP_IM_LOAD_IMMEDIATE    0x2b
#define CP_IM_STORE             0x2c
#define CP_SET_CONSTANT         0x2d
#define CP_LOAD_CONSTANT_CONTEXT 0x2e
#define CP_SET_BIN_DATA         0x2f
#define CP_DRAW_INDX_BIN        0x34
#define CP_DRAW_INDX_2_BIN      0x35
#define CP_DRAW_INDX_2          0x36
#define CP_INDIRECT_BUFFER_PFD  0x37
#define CP_INVALIDATE_STATE     0x3b
#define CP_WAIT_REG_MEM         0x3c
#define CP_MEM_WRITE            0x3d
#define CP_REG_TO_MEM           0x3e
#define CP_INDIRECT_BUFFER      0x3f
#define CP_INTERRUPT            0x40
#define CP_COND_EXEC            0x44
#define CP_COND_WRITE           0x45
#define CP_EVENT_WRITE          0x46
#define CP_ME_INIT              0x48
#define CP_SET_SHADER_BASES     0x4a
#define CP_SET_BIN_BASE_OFFSET  0x4b
#define CP_SET_BIN              0x4c
#define CP_MEM_WRITE_CNTR       0x4f
#define CP_SET_BIN_MASK         0x50
#define CP_SET_BIN_SELECT       0x51
#define CP_WAIT_REG_EQ          0x52
#define CP_WAIT_REG_GTE         0x53
#define CP_EVENT_WRITE_SHD      0x58
#define CP_EVENT_WRITE_CFL      0x59
#define CP_EVENT_WRITE_ZPD      0x5b
#define CP_WAIT_UNTIL_READ      0x5c
#define CP_WAIT_IB_PFD_COMPLETE 0x5d
#define CP_CONTEXT_UPDATE       0x5e
#define CP_SET_PROTECTED_MODE   0x5f

#define CACHE_FLUSH_TS          4

static const char *cp_opname(unsigned op)
{
    switch (op) {
    case CP_NOP: return "NOP";
    case CP_REG_RMW: return "REG_RMW";
    case CP_DRAW_INDX: return "DRAW_INDX";
    case CP_VIZ_QUERY: return "VIZ_QUERY";
    case CP_SET_STATE: return "SET_STATE";
    case CP_WAIT_FOR_IDLE: return "WAIT_FOR_IDLE";
    case CP_IM_LOAD: return "IM_LOAD";
    case CP_IM_LOAD_IMMEDIATE: return "IM_LOAD_IMMEDIATE";
    case CP_IM_STORE: return "IM_STORE";
    case CP_SET_CONSTANT: return "SET_CONSTANT";
    case CP_LOAD_CONSTANT_CONTEXT: return "LOAD_CONSTANT_CONTEXT";
    case CP_SET_BIN_DATA: return "SET_BIN_DATA";
    case CP_DRAW_INDX_BIN: return "DRAW_INDX_BIN";
    case CP_DRAW_INDX_2_BIN: return "DRAW_INDX_2_BIN";
    case CP_DRAW_INDX_2: return "DRAW_INDX_2";
    case CP_INDIRECT_BUFFER_PFD: return "INDIRECT_BUFFER_PFD";
    case CP_INVALIDATE_STATE: return "INVALIDATE_STATE";
    case CP_WAIT_REG_MEM: return "WAIT_REG_MEM";
    case CP_MEM_WRITE: return "MEM_WRITE";
    case CP_REG_TO_MEM: return "REG_TO_MEM";
    case CP_INDIRECT_BUFFER: return "INDIRECT_BUFFER";
    case CP_INTERRUPT: return "INTERRUPT";
    case CP_COND_EXEC: return "COND_EXEC";
    case CP_COND_WRITE: return "COND_WRITE";
    case CP_EVENT_WRITE: return "EVENT_WRITE";
    case CP_ME_INIT: return "ME_INIT";
    case CP_SET_SHADER_BASES: return "SET_SHADER_BASES";
    case CP_SET_BIN_BASE_OFFSET: return "SET_BIN_BASE_OFFSET";
    case CP_SET_BIN: return "SET_BIN";
    case CP_MEM_WRITE_CNTR: return "MEM_WRITE_CNTR";
    case CP_SET_BIN_MASK: return "SET_BIN_MASK";
    case CP_SET_BIN_SELECT: return "SET_BIN_SELECT";
    case CP_WAIT_REG_EQ: return "WAIT_REG_EQ";
    case CP_WAIT_REG_GTE: return "WAIT_REG_GTE";
    case CP_EVENT_WRITE_SHD: return "EVENT_WRITE_SHD";
    case CP_EVENT_WRITE_CFL: return "EVENT_WRITE_CFL";
    case CP_EVENT_WRITE_ZPD: return "EVENT_WRITE_ZPD";
    case CP_WAIT_UNTIL_READ: return "WAIT_UNTIL_READ";
    case CP_WAIT_IB_PFD_COMPLETE: return "WAIT_IB_PFD_COMPLETE";
    case CP_CONTEXT_UPDATE: return "CONTEXT_UPDATE";
    case CP_SET_PROTECTED_MODE: return "SET_PROTECTED_MODE";
    }
    return "?";
}

void adreno_trace(MsmAdrenoState *s, const char *fmt, ...)
{
    va_list ap;

    if (!s->trace) {
        return;
    }
    va_start(ap, fmt);
    vfprintf(s->trace, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* Interrupts                                                          */
/* ------------------------------------------------------------------ */

static uint32_t adreno_master_int(MsmAdrenoState *s)
{
    uint32_t *r = s->regs;
    uint32_t m = 0;

    if (r[A2_CP_INT_STATUS] & r[A2_CP_INT_CNTL]) {
        m |= A2_MASTER_CP;
    }
    if (r[A2_RBBM_INT_STATUS] & r[A2_RBBM_INT_CNTL]) {
        m |= A2_MASTER_RBBM;
    }
    if (r[A2_MH_INTERRUPT_STATUS] & r[A2_MH_INTERRUPT_MASK]) {
        m |= A2_MASTER_MH;
    }
    if (r[A2_SQ_INT_STATUS] & r[A2_SQ_INT_CNTL]) {
        m |= A2_MASTER_SQ;
    }
    return m;
}

static void adreno_update_irq(MsmAdrenoState *s)
{
    qemu_set_irq(s->irq, adreno_master_int(s) != 0);
}

static void adreno_cp_irq(MsmAdrenoState *s, uint32_t bits)
{
    s->regs[A2_CP_INT_STATUS] |= bits;
    adreno_update_irq(s);
}

/* ------------------------------------------------------------------ */
/* MH: GPU address translation                                        */
/* ------------------------------------------------------------------ */

bool adreno_translate(MsmAdrenoState *s, uint32_t va, hwaddr *pa)
{
    uint32_t range = s->regs[A2_MH_MMU_VA_RANGE];
    uint32_t base = range & 0xfffff000;
    uint32_t size = (range & 0xfff) << 16;
    uint32_t pte;

    if (!(s->regs[A2_MH_MMU_CONFIG] & 1) || va - base >= size) {
        *pa = va;
        return true;
    }
    pte = ldl_le_phys(&address_space_memory, s->regs[A2_MH_MMU_PT_BASE] +
                      ((va - base) >> 12) * 4);
    if (!(pte & 3)) {
        if (!(s->regs[A2_MH_INTERRUPT_STATUS] & A2_MH_INT_PAGE_FAULT)) {
            s->regs[A2_MH_MMU_PAGE_FAULT] = (va & 0xfffff000) | 1;
            s->regs[A2_MH_INTERRUPT_STATUS] |= A2_MH_INT_PAGE_FAULT;
            adreno_update_irq(s);
        }
        adreno_trace(s, "mmu: page fault at %08x\n", va);
        return false;
    }
    *pa = (pte & 0xfffff000) | (va & 0xfff);
    return true;
}

static bool adreno_access(MsmAdrenoState *s, uint32_t va, void *buf,
                          uint32_t len, bool write)
{
    uint8_t *p = buf;
    bool ok = true;

    while (len) {
        uint32_t chunk = MIN(len, 0x1000 - (va & 0xfff));
        hwaddr pa;

        if (!adreno_translate(s, va, &pa)) {
            if (!write) {
                memset(p, 0, chunk);
            }
            ok = false;
        } else {
            address_space_rw(&address_space_memory, pa,
                             MEMTXATTRS_UNSPECIFIED, p, chunk, write);
        }
        va += chunk;
        p += chunk;
        len -= chunk;
    }
    return ok;
}

bool adreno_read(MsmAdrenoState *s, uint32_t va, void *buf, uint32_t len)
{
    return adreno_access(s, va, buf, len, false);
}

bool adreno_write(MsmAdrenoState *s, uint32_t va, const void *buf,
                  uint32_t len)
{
    return adreno_access(s, va, (void *)buf, len, true);
}

uint32_t adreno_ldl(MsmAdrenoState *s, uint32_t va)
{
    uint32_t v = 0;

    adreno_read(s, va & ~3u, &v, 4);
    return le32_to_cpu(v);
}

void adreno_stl(MsmAdrenoState *s, uint32_t va, uint32_t val)
{
    val = cpu_to_le32(val);
    adreno_write(s, va & ~3u, &val, 4);
}

/* ------------------------------------------------------------------ */
/* Registers                                                           */
/* ------------------------------------------------------------------ */


/* A write from the CPU or from a type 0/1 packet or SET_CONSTANT */
static void adreno_reg_write(MsmAdrenoState *s, uint32_t reg, uint32_t val)
{
    uint32_t *r = s->regs;

    if (reg >= ADRENO_NREGS) {
        return;
    }
    switch (reg) {
    case A2_CP_INT_ACK:
        r[A2_CP_INT_STATUS] &= ~val;
        adreno_update_irq(s);
        return;
    case A2_RBBM_INT_ACK:
        r[A2_RBBM_INT_STATUS] &= ~val;
        adreno_update_irq(s);
        return;
    case A2_SQ_INT_ACK:
        r[A2_SQ_INT_STATUS] &= ~val;
        adreno_update_irq(s);
        return;
    case A2_MH_INTERRUPT_CLEAR:
        r[A2_MH_INTERRUPT_STATUS] &= ~val;
        adreno_update_irq(s);
        return;
    case A2_CP_INT_CNTL:
    case A2_RBBM_INT_CNTL:
    case A2_SQ_INT_CNTL:
    case A2_MH_INTERRUPT_MASK:
        r[reg] = val;
        adreno_update_irq(s);
        return;
    case A2_CP_ME_RAM_WADDR:
        s->me_ram_addr = val;
        r[reg] = val;
        return;
    case A2_CP_ME_RAM_DATA:
        s->me_ram_addr++;
        return;
    case A2_CP_PFP_UCODE_ADDR:
        s->pfp_addr = val;
        r[reg] = val;
        return;
    case A2_CP_PFP_UCODE_DATA:
        s->pfp_addr++;
        return;
    case A2_RBBM_SOFT_RESET:
        r[reg] = val;
        return;
    case A2_SQ_INST_STORE_MANAGMENT:
        r[reg] = val;
        return;
    }
    r[reg] = val;
    /* scratch registers: SCRATCH_UMSK selects the ones mirrored in memory */
    if (reg >= A2_SCRATCH_REG0 && reg < A2_SCRATCH_REG0 + 8) {
        unsigned n = reg - A2_SCRATCH_REG0;

        if (r[A2_SCRATCH_UMSK] & (1u << n)) {
            adreno_stl(s, r[A2_SCRATCH_ADDR] + n * 4, val);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Command processor                                                   */
/* ------------------------------------------------------------------ */

/* A stream of command dwords: the ring buffer or an indirect buffer */
typedef struct CpStream {
    uint32_t base;      /* GPU address */
    uint32_t size;      /* ring size in dwords (wraps), 0 for an IB */
    uint32_t pos;       /* dword index */
    uint32_t end;       /* dword index where the stream stops */
} CpStream;

static uint32_t cp_next(MsmAdrenoState *s, CpStream *st)
{
    uint32_t v = adreno_ldl(s, st->base + st->pos * 4);

    st->pos++;
    if (st->size && st->pos >= st->size) {
        st->pos = 0;
    }
    return v;
}

static uint32_t cp_avail(CpStream *st)
{
    if (st->size) {
        return (st->end - st->pos + st->size) % st->size;
    }
    return st->end - st->pos;
}

static void cp_exec_stream(MsmAdrenoState *s, CpStream *st);

static void cp_indirect(MsmAdrenoState *s, uint32_t addr, uint32_t len)
{
    CpStream ib = { .base = addr, .pos = 0, .end = len & 0xfffff };

    if (s->ib_depth >= 2) {
        adreno_trace(s, "cp: IB nested too deep\n");
        adreno_cp_irq(s, A2_CP_INT_IB_ERROR);
        return;
    }
    s->ib_depth++;
    if (s->ib_depth == 1) {
        s->regs[A2_CP_IB1_BASE] = addr;
        s->regs[A2_CP_IB1_BUFSZ] = len;
    } else {
        s->regs[A2_CP_IB2_BASE] = addr;
        s->regs[A2_CP_IB2_BUFSZ] = len;
    }
    cp_exec_stream(s, &ib);
    s->ib_depth--;
}

/* SET_CONSTANT / LOAD_CONSTANT_CONTEXT targets: type -> register base */
static int cp_const_base(uint32_t type)
{
    switch (type) {
    case 0: return A2_SQ_CONSTANT_0;    /* ALU */
    case 1: return A2_SQ_FETCH_0;       /* fetch */
    case 2: return A2_SQ_CF_BOOLEANS;
    case 3: return A2_SQ_CF_LOOP;
    case 4: return 0x2000;              /* context registers */
    }
    return -1;
}

static bool cp_compare(uint32_t func, uint32_t v, uint32_t ref)
{
    switch (func & 7) {
    case 1: return v < ref;
    case 2: return v <= ref;
    case 3: return v == ref;
    case 4: return v != ref;
    case 5: return v >= ref;
    case 6: return v > ref;
    case 7: return true;
    }
    return false;
}

static void cp_load_imem(MsmAdrenoState *s, uint32_t type, uint32_t start,
                         uint32_t size, const uint32_t *code, uint32_t addr)
{
    if (type > A2_SHADER_PS) {
        /* "shared" instructions of a split partition: not modelled */
        if (size) {
            adreno_trace(s, "cp: shared shader load (%u dwords) ignored\n",
                         size);
        }
        return;
    }
    if (start + size > ADRENO_IMEM_INSNS * 3) {
        adreno_trace(s, "cp: shader load out of range\n");
        return;
    }
    if (code) {
        memcpy(&s->imem[type][start], code, size * 4);
    } else {
        adreno_read(s, addr, &s->imem[type][start], size * 4);
        for (uint32_t i = 0; i < size; i++) {
            s->imem[type][start + i] = le32_to_cpu(s->imem[type][start + i]);
        }
    }
    s->imem_size[type] = start + size;
}

static void cp_trace_packet(MsmAdrenoState *s, uint32_t hdr,
                            const uint32_t *p, uint32_t n)
{
    unsigned op = (hdr >> 8) & 0x7f;

    if (!s->trace) {
        return;
    }
    fprintf(s->trace, "%*s%s", s->ib_depth * 2, "", cp_opname(op));
    if (op == CP_SET_CONSTANT && n) {
        fprintf(s->trace, " type %u off %03x:", (p[0] >> 16) & 0xff,
                p[0] & 0x7ff);
        p++;
        n--;
    }
    for (uint32_t i = 0; i < n && i < 24; i++) {
        fprintf(s->trace, " %08x", p[i]);
    }
    fprintf(s->trace, n > 24 ? " ...\n" : "\n");
}

/* Returns the number of dwords after this packet to skip (COND_EXEC) */
static uint32_t cp_type3(MsmAdrenoState *s, uint32_t hdr, uint32_t *p,
                         uint32_t n)
{
    unsigned op = (hdr >> 8) & 0x7f;
    uint32_t *r = s->regs;

    cp_trace_packet(s, hdr, p, n);

    /* predicated packets run only for the selected bins */
    if ((hdr & 1) && !(s->bin_select & s->bin_mask)) {
        return 0;
    }

    switch (op) {
    case CP_NOP:
    case CP_WAIT_FOR_IDLE:
    case CP_WAIT_UNTIL_READ:
    case CP_WAIT_IB_PFD_COMPLETE:
    case CP_INVALIDATE_STATE:
    case CP_CONTEXT_UPDATE:
    case CP_SET_STATE:
    case CP_VIZ_QUERY:
    case CP_SET_BIN_DATA:
    case CP_EVENT_WRITE_CFL:
        break;

    case CP_ME_INIT:
        /* p[3..10] sub-block offsets, p[11] shader bases */
        if (n > 11) {
            s->shader_bases = p[11];
        }
        break;

    case CP_SET_PROTECTED_MODE:
        s->protected_mode = n && p[0];
        break;

    case CP_INDIRECT_BUFFER:
    case CP_INDIRECT_BUFFER_PFD:
        if (n >= 2) {
            cp_indirect(s, p[0], p[1]);
        }
        break;

    case CP_INTERRUPT:
        if (n >= 1) {
            adreno_cp_irq(s, p[0]);
        }
        break;

    case CP_EVENT_WRITE:
        if (n >= 1) {
            r[A2_VGT_EVENT_INITIATOR] = p[0] & 0x3f;
        }
        if (n >= 3) {
            adreno_stl(s, p[1], p[2]);
        }
        break;

    case CP_EVENT_WRITE_SHD:
        if (n >= 3) {
            r[A2_VGT_EVENT_INITIATOR] = p[0] & 0x3f;
            adreno_stl(s, p[1], p[2]);
        }
        break;

    case CP_EVENT_WRITE_ZPD:
        break;

    case CP_MEM_WRITE:
        for (uint32_t i = 1; i < n; i++) {
            adreno_stl(s, p[0] + (i - 1) * 4, p[i]);
        }
        break;

    case CP_MEM_WRITE_CNTR:
        break;

    case CP_REG_TO_MEM:
        if (n >= 2) {
            adreno_stl(s, p[1], r[p[0] & 0x7fff]);
        }
        break;

    case CP_REG_RMW:
        if (n >= 3) {
            uint32_t reg = p[0] & 0x7fff;
            uint32_t v = r[reg];

            v &= (p[0] & (1u << 31)) ? r[p[1] & 0x7fff] : p[1];
            v |= (p[0] & (1u << 30)) ? r[p[2] & 0x7fff] : p[2];
            adreno_reg_write(s, reg, v);
        }
        break;

    case CP_COND_EXEC:
        /* run the next p[3] dwords if *(p[0]<<2) && *(p[1]<<2) <= p[2] */
        if (n >= 4) {
            uint32_t a = adreno_ldl(s, p[0] << 2);
            uint32_t b = adreno_ldl(s, p[1] << 2);

            if (!(a && b <= p[2])) {
                return p[3];
            }
        }
        break;

    case CP_COND_WRITE:
        if (n >= 6) {
            uint32_t v = (p[0] & 0x10) ? adreno_ldl(s, p[1])
                                        : r[p[1] & 0x7fff];

            if (cp_compare(p[0], v & p[3], p[2])) {
                if (p[0] & 0x100) {
                    adreno_stl(s, p[4], p[5]);
                } else {
                    adreno_reg_write(s, p[4] & 0x7fff, p[5]);
                }
            }
        }
        break;

    case CP_WAIT_REG_MEM:
        /* everything before has completed: just report a wait that fails */
        if (n >= 5) {
            uint32_t v = (p[0] & 0x10) ? adreno_ldl(s, p[1])
                                        : r[p[1] & 0x7fff];

            if (!cp_compare(p[0], v & p[3], p[2])) {
                adreno_trace(s, "cp: WAIT_REG_MEM would wait forever\n");
            }
        }
        break;

    case CP_WAIT_REG_EQ:
    case CP_WAIT_REG_GTE:
        break;

    case CP_SET_CONSTANT:
        if (n >= 1) {
            uint32_t type = (p[0] >> 16) & 0xff;
            uint32_t off = p[0] & 0x7ff;
            int base = cp_const_base(type);

            if (base < 0) {
                adreno_trace(s, "cp: SET_CONSTANT type %u\n", type);
                break;
            }
            for (uint32_t i = 1; i < n; i++, off++) {
                adreno_reg_write(s, base + off, p[i]);
                if (type < 5 && s->shadow_en[type]) {
                    adreno_stl(s, s->shadow_addr[type] + off * 4, p[i]);
                }
            }
        }
        break;

    case CP_LOAD_CONSTANT_CONTEXT:
        /*
         * p[0] memory base; then (shadow << 24 | type << 16 | offset,
         * count) pairs.  The memory is copied into the constants, and with
         * the shadow bit set later SET_CONSTANTs of that type also write
         * through to it (KGSL's context save area).
         */
        for (uint32_t i = 1; i + 1 < n; i += 2) {
            uint32_t type = (p[i] >> 16) & 0xff;
            uint32_t off = p[i] & 0x7ff;
            uint32_t cnt = p[i + 1];
            uint32_t mem = p[0] & ~3u;
            int base = cp_const_base(type);

            if (base < 0 || type >= 5) {
                continue;
            }
            if (i == 1) {
                s->shadow_en[type] = (p[i] >> 24) & 1;
                s->shadow_addr[type] = mem;
            }
            for (uint32_t k = 0; k < cnt && base + off + k < ADRENO_NREGS;
                 k++) {
                s->regs[base + off + k] = adreno_ldl(s, mem + (off + k) * 4);
            }
        }
        break;

    case CP_IM_LOAD:
        if (n >= 2) {
            cp_load_imem(s, p[0] & 3, p[1] >> 16, p[1] & 0xffff, NULL,
                         p[0] & ~3u);
        }
        break;

    case CP_IM_LOAD_IMMEDIATE:
        if (n >= 2) {
            uint32_t size = MIN(p[1] & 0xffff, n - 2);

            cp_load_imem(s, p[0] & 3, p[1] >> 16, size, &p[2], 0);
        }
        break;

    case CP_IM_STORE:
        /* save the instructions to p[0], their start/size word to p[1] */
        if (n >= 2) {
            uint32_t type = p[0] & 3;
            uint32_t size = type <= A2_SHADER_PS ? s->imem_size[type] : 0;

            for (uint32_t i = 0; i < size; i++) {
                adreno_stl(s, (p[0] & ~3u) + i * 4, s->imem[type][i]);
            }
            adreno_stl(s, p[1], size);
        }
        break;

    case CP_SET_SHADER_BASES:
        if (n >= 1) {
            s->shader_bases = p[0];
        }
        break;

    case CP_SET_BIN_MASK:
        if (n >= 1) {
            s->bin_mask = p[0];
        }
        break;
    case CP_SET_BIN_SELECT:
        if (n >= 1) {
            s->bin_select = p[0];
        }
        break;
    case CP_SET_BIN_BASE_OFFSET:
        if (n >= 1) {
            s->bin_base_offset = p[0];
        }
        break;
    case CP_SET_BIN:
        break;

    case CP_DRAW_INDX:
    case CP_DRAW_INDX_BIN:
        /*
         * viz query, VGT_DRAW_INITIATOR (a20x: index count in 31:16),
         * [bin base, bin size,] [index base, index size]
         */
        if (n >= 2) {
            uint32_t init = p[1];
            uint32_t *dma = &p[op == CP_DRAW_INDX_BIN ? 4 : 2];
            uint32_t have = n - (dma - p);

            r[A2_VGT_DRAW_INITIATOR] = init;
            if (((init >> 6) & 3) == 0 && have >= 2) {
                r[A2_VGT_DMA_BASE] = dma[0];
                r[A2_VGT_DMA_SIZE] = dma[1];
                adreno_draw(s, init, init >> 16, dma[0], dma[1], NULL, 0);
            } else {
                adreno_draw(s, init, init >> 16, 0, 0, NULL, 0);
            }
        }
        break;

    case CP_DRAW_INDX_2:
    case CP_DRAW_INDX_2_BIN:
        /* VGT_DRAW_INITIATOR, [bin base, bin size,] indices */
        if (n >= 1) {
            uint32_t skip = op == CP_DRAW_INDX_2_BIN ? 3 : 1;

            r[A2_VGT_DRAW_INITIATOR] = p[0];
            if (n >= skip) {
                adreno_draw(s, p[0], p[0] >> 16, 0, 0, &p[skip], n - skip);
            }
        }
        break;

    default:
        qemu_log_mask(LOG_UNIMP, "msm-adreno: PM4 opcode 0x%02x\n", op);
        adreno_trace(s, "cp: unknown opcode %02x\n", op);
        break;
    }
    return 0;
}

static void cp_exec_stream(MsmAdrenoState *s, CpStream *st)
{
    while (cp_avail(st)) {
        uint32_t start = st->pos;
        uint32_t hdr = cp_next(s, st);
        uint32_t type = hdr >> 30;
        uint32_t n, skip;

        switch (type) {
        case 0: {
            uint32_t reg = hdr & 0x7fff;
            bool one = hdr & (1u << 15);

            n = ((hdr >> 16) & 0x3fff) + 1;
            if (n > cp_avail(st)) {
                /* not all written yet: wait for the next WPTR */
                st->pos = start;
                return;
            }
            for (uint32_t i = 0; i < n; i++) {
                uint32_t v = cp_next(s, st);

                adreno_trace(s, "%*sREG %04x = %08x\n", s->ib_depth * 2, "",
                             one ? reg : reg + i, v);
                adreno_reg_write(s, one ? reg : reg + i, v);
            }
            break;
        }
        case 1: {
            uint32_t v0 = cp_next(s, st), v1 = cp_next(s, st);

            adreno_reg_write(s, hdr & 0x7ff, v0);
            adreno_reg_write(s, (hdr >> 11) & 0x7ff, v1);
            break;
        }
        case 2:
            break;
        case 3:
            n = ((hdr >> 16) & 0x3fff) + 1;
            if (n > cp_avail(st)) {
                /*
                 * Not all written yet (KGSL submits the NOP that pads the
                 * ring's end before the rest): wait for the next WPTR.
                 */
                st->pos = start;
                return;
            }
            for (uint32_t i = 0; i < n; i++) {
                s->pkt[i] = cp_next(s, st);
            }
            skip = cp_type3(s, hdr, s->pkt, n);
            while (skip-- && cp_avail(st)) {
                cp_next(s, st);
            }
            break;
        }
    }
}

static void cp_run_ring(MsmAdrenoState *s)
{
    uint32_t *r = s->regs;
    uint32_t size = 2u << (r[A2_CP_RB_CNTL] & 0x3f);
    CpStream rb;

    if (r[A2_CP_ME_CNTL] & (1u << 28)) {
        return;     /* micro engine halted */
    }
    rb.base = r[A2_CP_RB_BASE];
    rb.size = size;
    rb.pos = r[A2_CP_RB_RPTR] % size;
    rb.end = r[A2_CP_RB_WPTR] % size;
    s->ib_depth = 0;
    cp_exec_stream(s, &rb);
    r[A2_CP_RB_RPTR] = rb.pos;
    /* RB_NO_UPDATE clear: the read pointer goes to memory */
    if (!(r[A2_CP_RB_CNTL] & (1u << 27))) {
        adreno_stl(s, r[A2_CP_RB_RPTR_ADDR], rb.pos);
    }
    if (s->trace) {
        fflush(s->trace);
    }
}

/* ------------------------------------------------------------------ */
/* MMIO                                                                */
/* ------------------------------------------------------------------ */

static uint64_t adreno_mmio_read(void *opaque, hwaddr off, unsigned size)
{
    MsmAdrenoState *s = opaque;
    uint32_t reg = off >> 2;

    switch (reg) {
    case A2_RBBM_PERIPHID1:
        return ADRENO_PERIPHID1;
    case A2_RBBM_PERIPHID2:
        return ADRENO_PERIPHID2;
    case A2_RBBM_PATCH_RELEASE:
        return ADRENO_PATCH_REL;
    case A2_RBBM_STATUS:
        return ADRENO_RBBM_IDLE;
    case A2_MASTER_INT_SIGNAL:
        return adreno_master_int(s);
    case A2_CP_STAT:
    case A2_CP_ME_STATUS:
        return 0;
    case A2_CP_ME_RAM_WADDR:
        return s->me_ram_addr;
    }
    return s->regs[reg];
}

static void adreno_mmio_write(void *opaque, hwaddr off, uint64_t val,
                              unsigned size)
{
    MsmAdrenoState *s = opaque;
    uint32_t reg = off >> 2;

    if (reg == A2_CP_RB_WPTR) {
        s->regs[reg] = val;
        cp_run_ring(s);
        return;
    }
    if (reg == A2_CP_ME_CNTL) {
        s->regs[reg] = val;
        if (!(val & (1u << 28))) {
            cp_run_ring(s);
        }
        return;
    }
    if (reg == A2_CP_RB_BASE || reg == A2_CP_RB_CNTL) {
        /* a new ring starts empty */
        s->regs[reg] = val;
        s->regs[A2_CP_RB_RPTR] = 0;
        return;
    }
    adreno_reg_write(s, reg, val);
}

static const MemoryRegionOps adreno_mmio_ops = {
    .read = adreno_mmio_read,
    .write = adreno_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void adreno_reset(DeviceState *dev)
{
    MsmAdrenoState *s = MSM_ADRENO(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->imem_size, 0, sizeof(s->imem_size));
    memset(s->shadow_en, 0, sizeof(s->shadow_en));
    s->regs[A2_CP_ME_CNTL] = 1u << 28;     /* halted until the driver starts it */
    s->bin_mask = s->bin_select = 0;
    s->shader_bases = 0;
    s->protected_mode = false;
    adreno_update_irq(s);
}

static void adreno_realize(DeviceState *dev, Error **errp)
{
    MsmAdrenoState *s = MSM_ADRENO(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &adreno_mmio_ops, s,
                          "msm-adreno", ADRENO_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    s->gmem = g_malloc0(ADRENO_GMEM_SIZE);
    s->pkt = g_new(uint32_t, 0x4000 + 1);
    if (s->trace_path && *s->trace_path) {
        s->trace = fopen(s->trace_path, "w");
        if (!s->trace) {
            error_setg_errno(errp, errno, "msm-adreno: cannot open %s",
                             s->trace_path);
        }
    }
}

static const Property adreno_props[] = {
    DEFINE_PROP_STRING("trace", MsmAdrenoState, trace_path),
};

static void adreno_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = adreno_realize;
    device_class_set_legacy_reset(dc, adreno_reset);
    device_class_set_props(dc, adreno_props);
}

static const TypeInfo adreno_info = {
    .name = TYPE_MSM_ADRENO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmAdrenoState),
    .class_init = adreno_class_init,
};

static void adreno_register_types(void)
{
    type_register_static(&adreno_info);
}

type_init(adreno_register_types)
