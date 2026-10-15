/*
 * Qualcomm Adreno 200 ("Yamato", a20x) GPU of the QSD8250 - shared state
 *
 * msm_adreno.c       registers, MMU, command processor (PM4), interrupts
 * msm_adreno_sq.c    shader sequencer: a2xx microcode interpreter
 * msm_adreno_draw.c  vertex fetch, primitive assembly, rasterizer, render
 *                    backend (GMEM) and resolves to memory
 *
 * Register indices are dword offsets from the 0xA0000000 window, as in the
 * kernel's yamato_reg.h and freedreno's a2xx.xml.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#ifndef HW_ARM_HTCLEO_MSM_ADRENO_H
#define HW_ARM_HTCLEO_MSM_ADRENO_H

#include "hw/core/sysbus.h"

#define TYPE_MSM_ADRENO "msm-adreno"
OBJECT_DECLARE_SIMPLE_TYPE(MsmAdrenoState, MSM_ADRENO)

#define ADRENO_REG_SIZE     0x20000
#define ADRENO_NREGS        (ADRENO_REG_SIZE / 4)
#define ADRENO_GMEM_SIZE    (256 * 1024)
/* shader instruction memory, in instructions of 3 dwords */
#define ADRENO_IMEM_INSNS   4096

/* registers (dword index) */
#define A2_RBBM_PATCH_RELEASE       0x0001
#define A2_RBBM_CNTL                0x003b
#define A2_RBBM_SOFT_RESET          0x003c
#define A2_MH_MMU_CONFIG            0x0040
#define A2_MH_MMU_VA_RANGE          0x0041
#define A2_MH_MMU_PT_BASE           0x0042
#define A2_MH_MMU_PAGE_FAULT        0x0043
#define A2_MH_MMU_TRAN_ERROR        0x0044
#define A2_MH_MMU_INVALIDATE        0x0045
#define A2_MH_MMU_MPU_BASE          0x0046
#define A2_MH_MMU_MPU_END           0x0047
#define A2_CP_PFP_UCODE_ADDR        0x00c0
#define A2_CP_PFP_UCODE_DATA        0x00c1
#define A2_CP_RB_BASE               0x01c0
#define A2_CP_RB_CNTL               0x01c1
#define A2_CP_RB_RPTR_ADDR          0x01c3
#define A2_CP_RB_RPTR               0x01c4
#define A2_CP_RB_WPTR               0x01c5
#define A2_CP_RB_WPTR_DELAY         0x01c6
#define A2_CP_RB_RPTR_WR            0x01c7
#define A2_CP_RB_WPTR_BASE          0x01c8
#define A2_CP_QUEUE_THRESHOLDS      0x01d5
#define A2_SCRATCH_UMSK             0x01dc
#define A2_SCRATCH_ADDR             0x01dd
#define A2_CP_INT_CNTL              0x01f2
#define A2_CP_INT_STATUS            0x01f3
#define A2_CP_INT_ACK               0x01f4
#define A2_CP_ME_CNTL               0x01f6
#define A2_CP_ME_STATUS             0x01f7
#define A2_CP_ME_RAM_WADDR          0x01f8
#define A2_CP_ME_RAM_RADDR          0x01f9
#define A2_CP_ME_RAM_DATA           0x01fa
#define A2_CP_DEBUG                 0x01fc
#define A2_CP_CSQ_RB_STAT           0x01fd
#define A2_CP_CSQ_IB1_STAT          0x01fe
#define A2_CP_CSQ_IB2_STAT          0x01ff
#define A2_RBBM_PERFCOUNTER1_LO     0x0399
#define A2_RBBM_DEBUG               0x039b
#define A2_RBBM_PM_OVERRIDE1        0x039c
#define A2_RBBM_PM_OVERRIDE2        0x039d
#define A2_RBBM_READ_ERROR          0x03b3
#define A2_RBBM_INT_CNTL            0x03b4
#define A2_RBBM_INT_STATUS          0x03b5
#define A2_RBBM_INT_ACK             0x03b6
#define A2_MASTER_INT_SIGNAL        0x03b7
#define A2_RBBM_PERIPHID1           0x03f9
#define A2_RBBM_PERIPHID2           0x03fa
#define A2_CP_ST_BASE               0x044d
#define A2_CP_ST_BUFSZ              0x044e
#define A2_CP_IB1_BASE              0x0458
#define A2_CP_IB1_BUFSZ             0x0459
#define A2_CP_IB2_BASE              0x045a
#define A2_CP_IB2_BUFSZ             0x045b
#define A2_CP_STAT                  0x047f
#define A2_SCRATCH_REG0             0x0578
#define A2_RBBM_STATUS              0x05d0
#define A2_MH_ARBITER_CONFIG        0x0a40
#define A2_MH_INTERRUPT_MASK        0x0a42
#define A2_MH_INTERRUPT_STATUS      0x0a43
#define A2_MH_INTERRUPT_CLEAR       0x0a44
#define A2_MH_AXI_ERROR             0x0a45
#define A2_PA_SC_VIZ_QUERY_STATUS   0x0c44
#define A2_SQ_INST_STORE_MANAGMENT  0x0d02
#define A2_SQ_INT_CNTL              0x0d34
#define A2_SQ_INT_STATUS            0x0d35
#define A2_SQ_INT_ACK               0x0d36
#define A2_TC_CNTL_STATUS           0x0e00
#define A2_RB_EDRAM_INFO            0x0f02

/* context registers (SET_CONSTANT type 4 writes at 0x2000 + offset) */
#define A2_RB_SURFACE_INFO          0x2000
#define A2_RB_COLOR_INFO            0x2001
#define A2_RB_DEPTH_INFO            0x2002
#define A2_PA_SC_SCREEN_SCISSOR_TL  0x200e
#define A2_PA_SC_SCREEN_SCISSOR_BR  0x200f
#define A2_PA_SC_WINDOW_OFFSET      0x2080
#define A2_PA_SC_WINDOW_SCISSOR_TL  0x2081
#define A2_PA_SC_WINDOW_SCISSOR_BR  0x2082
#define A2_VGT_MAX_VTX_INDX         0x2100
#define A2_VGT_MIN_VTX_INDX         0x2101
#define A2_VGT_INDX_OFFSET          0x2102
#define A2_RB_COLOR_MASK            0x2104
#define A2_RB_BLEND_RED             0x2105
#define A2_RB_BLEND_GREEN           0x2106
#define A2_RB_BLEND_BLUE            0x2107
#define A2_RB_BLEND_ALPHA           0x2108
#define A2_RB_STENCILREFMASK_BF     0x210c
#define A2_RB_STENCILREFMASK        0x210d
#define A2_RB_ALPHA_REF             0x210e
#define A2_PA_CL_VPORT_XSCALE       0x210f
#define A2_PA_CL_VPORT_XOFFSET      0x2110
#define A2_PA_CL_VPORT_YSCALE       0x2111
#define A2_PA_CL_VPORT_YOFFSET      0x2112
#define A2_PA_CL_VPORT_ZSCALE       0x2113
#define A2_PA_CL_VPORT_ZOFFSET      0x2114
#define A2_SQ_PROGRAM_CNTL          0x2180
#define A2_SQ_CONTEXT_MISC          0x2181
#define A2_SQ_INTERPOLATOR_CNTL     0x2182
#define A2_SQ_WRAPPING_0            0x2183
#define A2_SQ_WRAPPING_1            0x2184
#define A2_SQ_PS_PROGRAM            0x21f6
#define A2_SQ_VS_PROGRAM            0x21f7
#define A2_VGT_EVENT_INITIATOR      0x21f9
#define A2_VGT_DMA_BASE             0x21fa
#define A2_VGT_DMA_SIZE             0x21fb
#define A2_VGT_DRAW_INITIATOR       0x21fc
#define A2_VGT_IMMED_DATA           0x21fd
#define A2_RB_DEPTHCONTROL          0x2200
#define A2_RB_BLEND_CONTROL         0x2201
#define A2_RB_COLORCONTROL          0x2202
#define A2_PA_CL_CLIP_CNTL          0x2204
#define A2_PA_SU_SC_MODE_CNTL       0x2205
#define A2_PA_CL_VTE_CNTL           0x2206
#define A2_RB_MODECONTROL           0x2208
#define A2_RB_SAMPLE_POS            0x220a
#define A2_CLEAR_COLOR              0x220b
#define A2_PA_SU_POINT_SIZE         0x2280
#define A2_PA_SU_POINT_MINMAX       0x2281
#define A2_PA_SU_LINE_CNTL          0x2282
#define A2_PA_SC_LINE_CNTL          0x2300
#define A2_PA_SC_AA_CONFIG          0x2301
#define A2_PA_SU_VTX_CNTL           0x2302
#define A2_SQ_VS_CONST              0x2307
#define A2_SQ_PS_CONST              0x2308
#define A2_PA_SC_AA_MASK            0x2312
#define A2_RB_COPY_CONTROL          0x2318
#define A2_RB_COPY_DEST_BASE        0x2319
#define A2_RB_COPY_DEST_PITCH       0x231a
#define A2_RB_COPY_DEST_INFO        0x231b
#define A2_RB_COPY_DEST_OFFSET      0x231c
#define A2_RB_DEPTH_CLEAR           0x231d
#define A2_RB_COLOR_DEST_MASK       0x2326
#define A2_PA_SU_POLY_OFFSET_FRONT_SCALE  0x2380
#define A2_PA_SU_POLY_OFFSET_FRONT_OFFSET 0x2381

/* shader constants: 256 float4 ALU, 32 x 6 dword fetch, bool, loop */
#define A2_SQ_CONSTANT_0            0x4000
#define A2_SQ_FETCH_0               0x4800
#define A2_SQ_CF_BOOLEANS           0x4900
#define A2_SQ_CF_LOOP               0x4908

/* interrupt bits */
#define A2_CP_INT_SW            (1u << 19)
#define A2_CP_INT_T0_PACKET_IB  (1u << 23)
#define A2_CP_INT_OPCODE_ERROR  (1u << 24)
#define A2_CP_INT_PROTECTED     (1u << 25)
#define A2_CP_INT_RESERVED_BIT  (1u << 26)
#define A2_CP_INT_IB_ERROR      (1u << 27)
#define A2_CP_INT_IB2           (1u << 29)
#define A2_CP_INT_IB1           (1u << 30)
#define A2_CP_INT_RB            (1u << 31)
#define A2_MH_INT_PAGE_FAULT    (1u << 2)
#define A2_MASTER_MH            (1u << 5)
#define A2_MASTER_SQ            (1u << 26)
#define A2_MASTER_CP            (1u << 30)
#define A2_MASTER_RBBM          (1u << 31)

/* RB_MODECONTROL.EDRAM_MODE */
#define A2_EDRAM_NOP            0
#define A2_EDRAM_COLOR_DEPTH    4
#define A2_EDRAM_DEPTH_ONLY     5
#define A2_EDRAM_COPY           6

/* VGT_DRAW_INITIATOR primitive types */
#define A2_PT_POINTLIST_PSIZE   1
#define A2_PT_LINELIST          2
#define A2_PT_LINESTRIP         3
#define A2_PT_TRILIST           4
#define A2_PT_TRIFAN            5
#define A2_PT_TRISTRIP          6
#define A2_PT_LINELOOP          7
#define A2_PT_RECTLIST          8
#define A2_PT_POINTLIST         9

/* shader types (IM_LOAD) */
#define A2_SHADER_VS            0
#define A2_SHADER_PS            1

/*
 * Shaders run on AL lanes at once: vertices in batches, or a 4x4 block of
 * pixels made of four 2x2 quads (lanes 4q..4q+3 = (x,y) (x+1,y) (x,y+1)
 * (x+1,y+1)), whose neighbours give the texture derivatives.
 */
#define AL              16
#define A2_MAX_INTERP   16

#define ADRENO_MAX_THREADS 16

#include "qemu/thread.h"

typedef struct AdrenoVertex {
    float pos[4];       /* clip space */
    float psize;
    float interp[A2_MAX_INTERP][4];
} AdrenoVertex;

/* A per-thread cache of GPU page -> host pointer (RAM only) */
#define ADRENO_TLB_SIZE 256
typedef struct AdrenoTlb {
    uint32_t tag[ADRENO_TLB_SIZE];      /* page number + 1, 0 = empty */
    uint8_t *host[ADRENO_TLB_SIZE];     /* NULL: not RAM or a fault */
} AdrenoTlb;

typedef struct AdrenoProgram AdrenoProgram;

/*
 * Decoded 32x32 texel tiles (RGBA8) of the textures a draw samples, per
 * thread; a draw's generation number invalidates them.
 */
#define ADRENO_TEXCACHE 128
typedef struct AdrenoTexTile {
    uint32_t gen;
    uint32_t base, pitch, key, tx, ty;
    uint8_t rgba[32 * 32 * 4];
} AdrenoTexTile;

typedef struct AdrenoLanes {
    struct MsmAdrenoState *s;
    const AdrenoProgram *prog;
    AdrenoTlb *tlb;
    AdrenoTexTile *texcache;
    uint32_t texgen;
    bool quads;                 /* pixel lanes: derivatives available */
    float r[64][4][AL];         /* temporaries */
    /* vertex shader outputs */
    float pos[4][AL];
    float psize[AL];
    float interp[A2_MAX_INTERP][4][AL];
    /* pixel shader outputs */
    float color[4][AL];
    float depth[AL];
    uint32_t killed;
} AdrenoLanes;

typedef struct AdrenoWorker {
    struct MsmAdrenoState *s;
    QemuThread thread;
    QemuSemaphore go;
    int index;
    AdrenoTlb tlb;
    AdrenoLanes lanes;
    uint64_t pixels;
} AdrenoWorker;

typedef struct AdrenoGl AdrenoGl;
typedef struct AdrenoResolveJob AdrenoResolveJob;

struct MsmAdrenoState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t regs[ADRENO_NREGS];
    uint8_t *gmem;

    /* command processor */
    uint32_t imem[2][ADRENO_IMEM_INSNS * 3];   /* VS, PS instruction memory */
    uint32_t imem_size[2];                     /* dwords loaded */
    uint32_t shader_bases;
    uint32_t bin_mask, bin_select;
    uint32_t bin_base_offset;
    bool protected_mode;
    int ib_depth;
    uint32_t *pkt;                             /* type 3 payload */
    /* LOAD_CONSTANT_CONTEXT shadows: SET_CONSTANT writes through */
    bool shadow_en[5];
    uint32_t shadow_addr[5];
    uint32_t me_ram_addr, pfp_addr;

    /* decoded shaders, by content */
    GHashTable *programs;

    /* rasterizer threads */
    uint32_t nthreads;
    AdrenoWorker *workers;
    QemuSemaphore done;
    void *job;                  /* the raster job in progress */
    bool quit;

    /* draw statistics */
    uint64_t draws, pixels;
    uint32_t texgen;            /* texture cache generation, per draw */

    /* host GPU backend (msm_adreno_gl.c for OpenGL, msm_adreno_gles.c for
     * OpenGL ES - a phone has no desktop OpenGL, so "gles" is the one it
     * wants there) */
    char *backend;              /* "gl", "gles" or "soft" */
    AdrenoGl *gl;
    bool gl_failed;
    bool gl_gles;               /* the host context is OpenGL ES */
    bool const_dirty;           /* ALU constants changed since uploaded */
    AdrenoResolveJob *resolves; /* resolves waiting for the kick's end */
    uint32_t nresolves, cap_resolves;
    /*
     * What may have changed memory: the CPU, between command kicks, and
     * the GPU, whose writes (resolves, draws to memory) are logged.
     */
    uint32_t kick_epoch;
    uint32_t gpu_writes;
    struct { uint32_t lo, hi; } write_log[1024];

    /* ADRENO_STATS=SECONDS: where the time goes, on stderr (us) */
    bool stats_on;
    int64_t stats_us;
    struct {
        int64_t last;
        uint64_t kicks, gpu_draws, cpu_draws, soft_draws, resolves;
        uint64_t hashes, decodes, hash_bytes;
        int64_t cp, sync, resolve, tex, upload, gpu;
    } st;

    char *trace_path;
    FILE *trace;
    uint64_t trace_vtx_draws;   /* trace vertices up to this draw */
};

/* A point in time for adreno_mem_unchanged() */
typedef struct AdrenoMemStamp {
    uint32_t kick, writes;
} AdrenoMemStamp;

/* the GPU wrote [lo, hi) of its address space */
void adreno_mem_written(MsmAdrenoState *s, uint32_t lo, uint32_t hi);
/* nothing changed [lo, hi) since the stamp, which becomes now */
bool adreno_mem_unchanged(MsmAdrenoState *s, AdrenoMemStamp *st, uint32_t lo,
                          uint32_t hi);
/* the GPU wrote [lo, hi) since the stamp (or the log cannot tell) */
bool adreno_gpu_wrote(MsmAdrenoState *s, const AdrenoMemStamp *st,
                      uint32_t lo, uint32_t hi);

/* GPU virtual address space (MH MMU) */
bool adreno_read(MsmAdrenoState *s, uint32_t gpuaddr, void *buf, uint32_t len);
bool adreno_write(MsmAdrenoState *s, uint32_t gpuaddr, const void *buf,
                  uint32_t len);
uint32_t adreno_ldl(MsmAdrenoState *s, uint32_t gpuaddr);
void adreno_stl(MsmAdrenoState *s, uint32_t gpuaddr, uint32_t val);
/* translate one page; returns false on a fault */
bool adreno_translate(MsmAdrenoState *s, uint32_t gpuaddr, hwaddr *pa);
/* host pointer to the byte at gpuaddr, valid to the end of its page */
uint8_t *adreno_host_ptr(MsmAdrenoState *s, AdrenoTlb *tlb, uint32_t gpuaddr);
void adreno_tlb_flush(AdrenoTlb *tlb);
/* read/write through the TLB (falls back to the slow path) */
void adreno_tlb_read(MsmAdrenoState *s, AdrenoTlb *tlb, uint32_t gpuaddr,
                     void *buf, uint32_t len);
void adreno_tlb_write(MsmAdrenoState *s, AdrenoTlb *tlb, uint32_t gpuaddr,
                      const void *buf, uint32_t len);

void adreno_trace(MsmAdrenoState *s, const char *fmt, ...) G_GNUC_PRINTF(2, 3);

static inline float adreno_f(uint32_t v)
{
    union { uint32_t u; float f; } c = { .u = v };
    return c.f;
}

static inline uint32_t adreno_u(float f)
{
    union { uint32_t u; float f; } c = { .f = f };
    return c.u;
}

/* msm_adreno_draw.c */
typedef struct AdrenoColorFmt {
    uint8_t bpp;        /* bytes per pixel */
    uint8_t bits[4];    /* channel widths from bit 0 */
    uint8_t kind;       /* 0 unorm, 1 snorm, 2 float16, 3 float32 */
} AdrenoColorFmt;

const AdrenoColorFmt *adreno_color_fmt(unsigned fmt);
/* do the resolves that wait (msm_adreno_gl.c snapshots of GMEM) */
void adreno_resolves_flush(MsmAdrenoState *s);
void adreno_draw(MsmAdrenoState *s, uint32_t initiator, uint32_t num_indices,
                 uint32_t index_base, uint32_t index_size,
                 const uint32_t *immediate, uint32_t immediate_dwords);
void adreno_raster_worker(AdrenoWorker *w);
float adreno_half_to_float(uint16_t h);
/* Xenos 2D tiling: byte offset of block (x, y), pitch in blocks */
uint32_t adreno_tiled_offset(int x, int y, uint32_t pitch, unsigned bpb_log2);
uint16_t adreno_float_to_half(float f);

/* msm_adreno_sq.c */
struct AdrenoProgram {
    int type;
    uint64_t hash;          /* of the microcode, the program cache's key */
    uint32_t ncf;
    struct SqCf *cf;
    struct SqInsn *insn;
    uint32_t ninsn;
    bool kills;             /* pixel shader may discard */
    bool exports_depth;
    bool exports_psize;     /* vertex shader writes the point size */
    uint32_t tex_mask;      /* texture fetch constants used */
};

const AdrenoProgram *adreno_program(MsmAdrenoState *s, int type);
void adreno_programs_free(MsmAdrenoState *s);
void adreno_shader_exec(AdrenoLanes *L, uint32_t mask);

/* msm_adreno_tex.c: sample fetch constant idx for the lanes in mask */
typedef struct AdrenoTexArgs {
    unsigned const_idx;
    unsigned mag, min, mip;         /* filters, resolved */
    bool unnormalized;
    float lod_bias;                 /* instruction bias */
    bool use_reg_lod;
    float offset[3];
} AdrenoTexArgs;

void adreno_sample(AdrenoLanes *L, const AdrenoTexArgs *a,
                   float coord[3][AL], uint32_t mask, float out[4][AL]);

#endif
