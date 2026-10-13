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
 *   header        word 1 source picture luma, word 2 the reconstruction
 *                 of this picture (the firmware writes it), word 3
 *                 MB rows << 16 | MB columns; the reference of a P
 *                 picture is the previous command's reconstruction
 *   reg 14        the output DMA table: six-word entries, bits 8-11 of
 *                 word 0 the stream (0 source MBs, 2 prediction MBs, 7
 *                 MB information), word 3 its buffer
 *   reg 1096      the MB: column in bits 0-8, row in 16-24, bit 28 the
 *                 first of the picture
 *   reg 114b      offset of the MB's source luma (16x16, stride 16) in
 *                 the source stream
 *   reg 114c      offset of its prediction luma in the prediction stream
 *   reg 114d      offset of its 20-byte information record: word 2 bits
 *                 0-1 = 3 gives MPEG-4 mb_type 0 (INTER), else bits 2-3:
 *                 0 INTRA (3), 3 INTER4V (2); words 3 and 4 the vectors
 *                 (bytes)
 *   the chroma the firmware reads from the pictures itself
 *
 * The motion search is not modelled yet: every macroblock is coded with
 * a zero vector and the co-located reference macroblock as prediction,
 * which any decoder reconstructs identically.
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

static uint32_t last_recon;         /* the reference of the next picture */
static unsigned frame_no;
bool htcleo_venc_test_pattern;      /* debug: paint the source pictures */

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
    uint32_t src, recon, ref, buf_src = 0, buf_pred = 0, buf_info = 0;
    uint32_t mb = 0, off_src = 0, off_pred = 0, off_info = 0;
    bool got_src = false, got_pred = false, got_info = false;
    int mbw, mbh, stride, nmb = 0;

    physical_memory_read(addr, w, CMD_MAX * 4);
    src = le32_to_cpu(w[1]);
    recon = le32_to_cpu(w[2]);
    mbw = le32_to_cpu(w[3]) & 0xffff;
    mbh = le32_to_cpu(w[3]) >> 16;
    stride = mbw * 16;
    ref = last_recon ? last_recon : recon;
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
            if (got_src || got_pred || got_info) {
                int mbx = mb & 0x1ff, mby = (mb >> 16) & 0x1ff;

                if (mbx < mbw && mby < mbh) {
                    if (got_src && buf_src) {
                        copy_mb(src, stride, mbx, mby, buf_src + off_src);
                    }
                    if (got_pred && buf_pred) {
                        copy_mb(ref, stride, mbx, mby, buf_pred + off_pred);
                    }
                    if (got_info && buf_info) {
                        /*
                         * word 2: bits 0-1 = 3 picks the firmware's MB
                         * type 0 (MPEG-4 INTER, one vector); vectors 0
                         */
                        uint32_t rec[5] = { 0, 0, cpu_to_le32(3), 0, 0 };

                        physical_memory_write(buf_info + off_info, rec,
                                              sizeof(rec));
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
                mb = v[k];
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
        qemu_log("q6-venc: %dx%d, %d MBs, src %08x ref %08x recon %08x, "
                 "streams %08x/%08x/%08x\n", mbw * 16, mbh * 16, nmb, src,
                 ref, recon, buf_src, buf_pred, buf_info);
    }
    last_recon = recon;
    return 0;
}
