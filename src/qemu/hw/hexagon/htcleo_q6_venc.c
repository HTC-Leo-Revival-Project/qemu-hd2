/*
 * QSD8250 QDSP6 subsystem: the "Janus" video encoder core
 *
 * The encoder command processor at 0xA1000000 has the decoder's register
 * interface (htcleo_q6.c) and command-buffer packet grammar
 * (htcleo_q6_vdc.c); packet register numbers are word offsets into the
 * core's register space.  For MPEG-4/H.263 the DSP1 firmware does the
 * transform, quantisation, VLC and reconstruction itself; this core
 * fetches macroblocks and does the motion search.  Reverse-engineered
 * from the firmware (which buffers its kernels read, and how):
 *
 *   header        word 1 source picture luma, word 2 the reference
 *                 picture (the previous picture's reconstruction, which
 *                 the firmware writes into the next command's word 2),
 *                 word 3 MB rows << 16 | MB columns
 *   reg 14        the output DMA table: six-word entries, bits 8-11 of
 *                 word 0 the stream (0 source MBs, 2 prediction MBs, 7
 *                 MB information), word 3 its buffer
 *   reg 1096      fetch an MB: column in bits 0-8, row in 16-24, bit 28
 *                 the first of a row, bit 27 motion search (the positions
 *                 then come from reg 96)
 *   reg 96        fetch an MB for the motion search, row << 16 | column (P
 *                 pictures interleave two MB rows)
 *   the fetches are pipelined: each step's outputs (114b-114d) belong to
 *   the oldest MB fetched and not yet written out, one or two steps back
 *   reg 114b      offset of the MB's source luma (16x16, stride 16) in
 *                 the source stream
 *   reg 114c      offset of its prediction luma in the prediction stream
 *   reg 114d      offset of its 20-byte information record, the motion
 *                 search's result: word 2 bits 0-1 = 3 codes the MB intra,
 *                 else bits 2-3 = 3 inter with one vector, 0 inter with
 *                 four; words 3 and 4 the vectors, x and y signed bytes in
 *                 quarter pels (MPEG-4 codes the half pels)
 *   the chroma the firmware reads from the pictures itself
 *
 * By default (venc-me=0) there is no motion search and every MB of a P
 * picture is coded intra, which always decodes correctly.  venc-me=1/2 is
 * an experimental search (full pels over +-15, then half pels, a zero
 * vector bias, intra for MBs flatter than their best prediction) whose
 * prediction stream carries the vector's interpolated block; the
 * firmware codes those vectors as expected, but its inter frames still
 * overflow the 25 KB output buffer ("Buffer overrun"), not yet understood.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "system/physmem.h"
#include "hw/hexagon/htcleo_q6_vdc.h"

#define CMD_MAX         0x10000     /* words */
/* bits 8-11 of a table entry's first word: the stream */
#define STREAM_SRC      0x0
#define STREAM_PRED     0x2
#define STREAM_INFO     0x7
/* word 2 of an MB information record: the macroblock's coding */
#define INFO_INTRA      0x3         /* bits 0-1 = 3 */
#define INFO_INTER      0xc         /* bits 2-3 = 3: one vector */
#define INFO_INTER4V    0x0         /* bits 2-3 = 0: four vectors */

static unsigned frame_no;
bool htcleo_venc_test_pattern;      /* debug: paint the source pictures */
uint32_t htcleo_venc_me;            /* motion search: 0 none, 1 full, 2 half pel */

#define ME_RANGE        15          /* full pels each way (f_code 1) */
#define ME_ZERO_BIAS    128         /* SAD bonus of the zero vector */
#define ME_INTRA_BIAS   512         /* intra when deviation + this < SAD */

/* the pictures of one encode, luma, in host memory */
typedef struct VencPics {
    uint8_t *src, *ref;
    int w, h;
} VencPics;

static int sad16(const uint8_t *a, const uint8_t *b, int stride, int limit)
{
    int sad = 0;

    for (int y = 0; y < 16 && sad < limit; y++) {
        for (int x = 0; x < 16; x++) {
            sad += abs(a[y * stride + x] - b[y * stride + x]);
        }
    }
    return sad;
}

/* the 16x16 prediction at half-pel vector (mvx, mvy), MPEG-4 rounding 0 */
static void pred_mb(const VencPics *p, int mbx, int mby, int mvx, int mvy,
                    uint8_t *out)
{
    int x0 = mbx * 16 + (mvx >> 1), y0 = mby * 16 + (mvy >> 1);
    int fx = mvx & 1, fy = mvy & 1;
    const uint8_t *r = p->ref + y0 * p->w + x0;

    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            const uint8_t *q = r + y * p->w + x;
            int v;

            if (fx && fy) {
                v = (q[0] + q[1] + q[p->w] + q[p->w + 1] + 2) >> 2;
            } else if (fx) {
                v = (q[0] + q[1] + 1) >> 1;
            } else if (fy) {
                v = (q[0] + q[p->w] + 1) >> 1;
            } else {
                v = q[0];
            }
            out[y * 16 + x] = v;
        }
    }
}

/* is the half-pel vector's block (with its interpolation taps) inside? */
static bool mv_inside(const VencPics *p, int mbx, int mby, int mvx, int mvy)
{
    int x0 = mbx * 16 * 2 + mvx, y0 = mby * 16 * 2 + mvy;

    return x0 >= 0 && y0 >= 0 && x0 + 30 + (mvx & 1) * 2 <= (p->w - 1) * 2 &&
           y0 + 30 + (mvy & 1) * 2 <= (p->h - 1) * 2;
}

static int sad_mv(const VencPics *p, int mbx, int mby, int mvx, int mvy,
                  int limit)
{
    uint8_t blk[256];
    const uint8_t *s = p->src + mby * 16 * p->w + mbx * 16;
    int sad = 0;

    if (!((mvx | mvy) & 1)) {
        return sad16(s, p->ref + (mby * 16 + mvy / 2) * p->w + mbx * 16 +
                     mvx / 2, p->w, limit);
    }
    pred_mb(p, mbx, mby, mvx, mvy, blk);
    for (int y = 0; y < 16 && sad < limit; y++) {
        for (int x = 0; x < 16; x++) {
            sad += abs(s[y * p->w + x] - blk[y * 16 + x]);
        }
    }
    return sad;
}

/*
 * The MB's vector in half pels, or false for intra: a full search over
 * +-ME_RANGE pels, then the eight half-pel neighbours of the best.
 */
static bool search_mb(const VencPics *p, int mbx, int mby, int *bx, int *by)
{
    const uint8_t *s = p->src + mby * 16 * p->w + mbx * 16;
    int best = sad_mv(p, mbx, mby, 0, 0, INT_MAX) - ME_ZERO_BIAS;
    int mean = 0, dev = 0;

    *bx = *by = 0;
    if (htcleo_venc_me >= 1) {
        for (int dy = -ME_RANGE; dy <= ME_RANGE; dy++) {
            for (int dx = -ME_RANGE; dx <= ME_RANGE; dx++) {
                int sad;

                if ((!dx && !dy) || !mv_inside(p, mbx, mby, dx * 2, dy * 2)) {
                    continue;
                }
                sad = sad_mv(p, mbx, mby, dx * 2, dy * 2, best);
                if (sad < best) {
                    best = sad;
                    *bx = dx * 2;
                    *by = dy * 2;
                }
            }
        }
    }
    if (htcleo_venc_me >= 2) {
        int cx = *bx, cy = *by;

        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int sad;

                if ((!dx && !dy) ||
                    !mv_inside(p, mbx, mby, cx + dx, cy + dy)) {
                    continue;
                }
                sad = sad_mv(p, mbx, mby, cx + dx, cy + dy, best);
                if (sad < best) {
                    best = sad;
                    *bx = cx + dx;
                    *by = cy + dy;
                }
            }
        }
    }
    /* intra when the MB is flatter than its best prediction */
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            mean += s[y * p->w + x];
        }
    }
    mean = (mean + 128) >> 8;
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            dev += abs(s[y * p->w + x] - mean);
        }
    }
    return dev + ME_INTRA_BIAS >= best;
}

/* a moving gradient with a box, so every encoder stage shows */
static void paint_test_pattern(uint32_t luma, uint32_t chroma, int w, int h)
{
    g_autofree uint8_t *y = g_malloc(w * h);
    g_autofree uint8_t *c = g_malloc(w * h / 2);
    int bx = (frame_no * 3) % (w - 32), by = (frame_no * 2) % (h - 32);

    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            bool box = i >= bx && i < bx + 32 && j >= by && j < by + 32;
            y[j * w + i] = box ? 235 : 16 + ((i + j + frame_no * 4) & 0x7f);
        }
    }
    for (int j = 0; j < h / 2; j++) {
        for (int i = 0; i < w / 2; i++) {
            c[j * w + 2 * i] = 128 + ((i * 4 + frame_no) & 0x3f) - 32;
            c[j * w + 2 * i + 1] = 128 - ((j * 4) & 0x3f) + 32;
        }
    }
    physical_memory_write(luma, y, w * h);
    physical_memory_write(chroma, c, w * h / 2);
}

static int packet_len(uint32_t h)
{
    switch (h >> 24) {
    case 0x04: return 2;
    case 0x03: return 3;
    case 0x02: return 4;
    case 0x08: return 3;
    default: return 1;
    }
}

static void copy_mb(uint32_t pic, int stride, int mbx, int mby, uint32_t dst)
{
    uint8_t mb[256];

    for (int y = 0; y < 16; y++) {
        physical_memory_read(pic + (mby * 16 + y) * stride + mbx * 16,
                             mb + 16 * y, 16);
    }
    physical_memory_write(dst, mb, sizeof(mb));
}

int htcleo_venc_encode(uint32_t addr, bool trace)
{
    g_autofree uint32_t *w = g_new0(uint32_t, CMD_MAX);
    uint32_t src, ref, buf_src = 0, buf_pred = 0, buf_info = 0;
    uint32_t off_src = 0, off_pred = 0, off_info = 0;
    bool got_src = false, got_pred = false, got_info = false, search = false;
    uint32_t fifo[8];               /* fetched MBs, row << 16 | column */
    unsigned fifo_rd = 0, fifo_wr = 0;
    int mbw, mbh, stride, nmb = 0;
    VencPics pics = { 0 };

    physical_memory_read(addr, w, CMD_MAX * 4);
    src = le32_to_cpu(w[1]);
    ref = le32_to_cpu(w[2]);
    mbw = le32_to_cpu(w[3]) & 0xffff;
    mbh = le32_to_cpu(w[3]) >> 16;
    stride = mbw * 16;
    if (htcleo_venc_test_pattern && mbw <= 80 && mbh <= 46) {
        /* the firmware reads the chroma itself, from luma + w * h */
        paint_test_pattern(src, src + stride * mbh * 16, stride, mbh * 16);
    }
    frame_no++;
    if (!mbw || !mbh || mbw > 80 || mbh > 46) {
        qemu_log_mask(LOG_UNIMP, "q6-venc: unsupported picture %dx%d MBs\n",
                      mbw, mbh);
        return -1;
    }

    pics.w = stride;
    pics.h = mbh * 16;
    if (ref) {
        pics.src = g_malloc(pics.w * pics.h);
        pics.ref = g_malloc(pics.w * pics.h);
        physical_memory_read(src, pics.src, pics.w * pics.h);
        physical_memory_read(ref, pics.ref, pics.w * pics.h);
    }

    for (int i = 4; i < CMD_MAX && w[i]; ) {
        uint32_t h = le32_to_cpu(w[i]), op = h >> 24;
        uint32_t r[3] = { 0 }, v[3] = { 0 };
        int nr = 0;

        if (op == 0x01) {
            break;
        }
        switch (op) {
        case 0x04:
            r[0] = h & 0xffffff;
            nr = 1;
            break;
        case 0x03:
            r[0] = (h >> 12) & 0xfff;
            r[1] = h & 0xfff;
            nr = 2;
            break;
        case 0x02:
            r[0] = (h >> 16) & 0xff;
            r[1] = (h >> 8) & 0xff;
            r[2] = h & 0xff;
            nr = 3;
            break;
        case 0x0a:
            /* end of an MB step: produce what was asked for */
            if ((got_src || got_pred || got_info) && fifo_rd != fifo_wr) {
                uint32_t mb = fifo[fifo_rd++ % ARRAY_SIZE(fifo)];
                int mbx = mb & 0x1ff, mby = (mb >> 16) & 0x1ff;

                if (mbx < mbw && mby < mbh) {
                    if (got_src && buf_src) {
                        copy_mb(src, stride, mbx, mby, buf_src + off_src);
                    }
                    if (got_pred || got_info) {
                        int mvx = 0, mvy = 0;
                        bool inter = true;
                        uint32_t rec[5] = { 0 };
                        uint8_t blk[256];

                        if (pics.ref && htcleo_venc_me) {
                            inter = search_mb(&pics, mbx, mby, &mvx, &mvy);
                        } else {
                            inter = false;  /* no search: intra MBs */
                        }
                        if (!inter) {
                            rec[2] = cpu_to_le32(INFO_INTRA);
                        } else {
                            /* one vector, in quarter pels */
                            rec[2] = cpu_to_le32(INFO_INTER);
                            rec[3] = cpu_to_le32((uint8_t)(mvx * 2) |
                                                 (uint8_t)(mvy * 2) << 8);
                        }
                        if (got_pred && buf_pred) {
                            if (pics.ref) {
                                pred_mb(&pics, mbx, mby, mvx, mvy, blk);
                                physical_memory_write(buf_pred + off_pred,
                                                      blk, sizeof(blk));
                            } else {
                                copy_mb(ref, stride, mbx, mby,
                                        buf_pred + off_pred);
                            }
                        }
                        if (got_info && buf_info) {
                            physical_memory_write(buf_info + off_info, rec,
                                                  sizeof(rec));
                        }
                    }
                    nmb++;
                }
            }
            got_src = got_pred = got_info = false;
            break;
        }
        for (int k = 0; k < nr; k++) {
            v[k] = le32_to_cpu(w[i + 1 + k]);
            switch (r[k]) {
            case 0x14: {
                /* the output DMA table: three six-word streams */
                uint32_t t[18];

                physical_memory_read(v[k], t, sizeof(t));
                for (int e = 0; e < 3; e++) {
                    uint32_t id = (le32_to_cpu(t[6 * e]) >> 8) & 0xf;
                    uint32_t a = le32_to_cpu(t[6 * e + 3]);

                    if (id == STREAM_SRC) {
                        buf_src = a;
                    } else if (id == STREAM_PRED) {
                        buf_pred = a;
                    } else if (id == STREAM_INFO) {
                        buf_info = a;
                    }
                }
                break;
            }
            case 0x1096:
                search |= v[k] & (1u << 27);
                if (search) {
                    break;          /* the search names its MBs in 96 */
                }
                fifo[fifo_wr++ % ARRAY_SIZE(fifo)] = v[k] & 0x1ff01ff;
                break;
            case 0x96:
                fifo[fifo_wr++ % ARRAY_SIZE(fifo)] = v[k] & 0x1ff01ff;
                break;
            case 0x114b:
                off_src = v[k];
                got_src = true;
                break;
            case 0x114c:
                off_pred = v[k];
                got_pred = true;
                break;
            case 0x114d:
                off_info = v[k];
                got_info = true;
                break;
            }
        }
        if (op == 0x08) {
            /* a fed block: the output DMA table comes as reg 14 too */
            uint32_t reg = h & 0xffffff, a = le32_to_cpu(w[i + 1]);

            if (reg == 0x14) {
                uint32_t t[18];
                int n = MIN((le32_to_cpu(w[i + 2]) & 0xffff) / 6, 3);

                physical_memory_read(a, t, sizeof(t));
                for (int e = 0; e < n; e++) {
                    uint32_t id = (le32_to_cpu(t[6 * e]) >> 8) & 0xf;
                    uint32_t ba = le32_to_cpu(t[6 * e + 3]);

                    if (id == STREAM_SRC) {
                        buf_src = ba;
                    } else if (id == STREAM_PRED) {
                        buf_pred = ba;
                    } else if (id == STREAM_INFO) {
                        buf_info = ba;
                    }
                }
            }
        }
        i += packet_len(h);
    }

    if (trace) {
        qemu_log("q6-venc: %dx%d, %d MBs, src %08x ref %08x, "
                 "streams %08x/%08x/%08x\n", mbw * 16, mbh * 16, nmb, src,
                 ref, buf_src, buf_pred, buf_info);
    }
    g_free(pics.src);
    g_free(pics.ref);
    return 0;
}
