/*
 * Qualcomm QSD8x50 crypto engine (0xA0C00000), its hash half.
 *
 * The radio's boot loaders check every segment of the images they load
 * (AMSS, the QDSP6 firmware) against the SHA-1 table signed into the image,
 * and FSBL's SHA-1 is this engine: the CPU programs a segment, the data
 * mover streams the bytes into DATA_IN and reads the digest back out of the
 * IV registers.
 *
 *   0x20 STATUS        bit 0 error: always clear here
 *   0x24 CONFIG
 *   0x30 SEG_CFG       0x200 hash, 0x800 SHA-256 (else SHA-1),
 *                      0x20000 first segment: start from the algorithm's IV
 *                      (else carry on from the IV and byte count
 *                      registers), 0x40000 last segment: pad
 *   0x38 AUTH_SEG_CFG  size << 16 | start
 *   0x3C SEG_SIZE      bytes in this segment
 *   0x40 GOPROC        written once the segment is set up
 *   0x90/0x94          AUTH_BYTECNT: bytes hashed before this segment, and
 *                      after it once the segment is done
 *   0x100-0x11C        AUTH_IV0-7: the running state, the digest at the end
 *   0x8000-0xFFFF      DATA_IN: big-endian words, as the data mover's byte
 *                      swap delivers them
 *
 * The segment runs when SEG_SIZE bytes have arrived.  Encryption is not
 * modelled.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "hw/core/sysbus.h"
#include "htcleo.h"

#define TYPE_MSM_CRYPTO "msm-crypto"
OBJECT_DECLARE_SIMPLE_TYPE(MsmCryptoState, MSM_CRYPTO)

#define CE_STATUS       0x20
#define CE_SEG_CFG      0x30
#define CE_SEG_SIZE     0x3C
#define CE_GOPROC       0x40
#define CE_BYTECNT0     0x90
#define CE_BYTECNT1     0x94
#define CE_IV0          0x100
#define CE_DATA_IN      0x8000

#define SEG_SHA256      (1u << 11)
#define SEG_FIRST       (1u << 17)
#define SEG_LAST        (1u << 18)

#define SEG_MAX         (1u << 20)

struct MsmCryptoState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[0x200 / 4];
    uint8_t *buf;
    uint32_t fill;
    bool seg_done;
};

static inline uint32_t rol(uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}

static inline uint32_t ror(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha1_block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];

    for (int i = 0; i < 16; i++) {
        w[i] = ldl_be_p(p + 4 * i);
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    for (int i = 0; i < 80; i++) {
        uint32_t f, k, t;

        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void sha256_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64], v[8];

    for (int i = 0; i < 16; i++) {
        w[i] = ldl_be_p(p + 4 * i);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(v, h, sizeof(v));
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = ror(v[4], 6) ^ ror(v[4], 11) ^ ror(v[4], 25);
        uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
        uint32_t t1 = v[7] + s1 + ch + sha256_k[i] + w[i];
        uint32_t s0 = ror(v[0], 2) ^ ror(v[0], 13) ^ ror(v[0], 22);
        uint32_t mj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);

        memmove(v + 1, v, 7 * sizeof(v[0]));
        v[4] += t1;
        v[0] = t1 + s0 + mj;
    }
    for (int i = 0; i < 8; i++) {
        h[i] += v[i];
    }
}

static const uint32_t sha1_iv[5] = {
    0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0,
};
static const uint32_t sha256_iv[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c,
    0x1f83d9ab, 0x5be0cd19,
};

/* hash the segment that has arrived in buf */
static void crypto_run(MsmCryptoState *s)
{
    uint32_t cfg = s->regs[CE_SEG_CFG / 4];
    bool sha256 = cfg & SEG_SHA256;
    int words = sha256 ? 8 : 5;
    uint32_t *h = &s->regs[CE_IV0 / 4];
    uint64_t before = s->regs[CE_BYTECNT0 / 4] |
                      (uint64_t)s->regs[CE_BYTECNT1 / 4] << 32;
    uint32_t n = s->fill, off = 0;

    if (cfg & SEG_FIRST) {
        memcpy(h, sha256 ? sha256_iv : sha1_iv, words * 4);
        before = 0;
    }
    for (; off + 64 <= n; off += 64) {
        sha256 ? sha256_block(h, s->buf + off) : sha1_block(h, s->buf + off);
    }
    if (cfg & SEG_LAST) {
        uint8_t tail[128] = { 0 };
        uint32_t rest = n - off, len = rest + 9 <= 64 ? 64 : 128;
        uint64_t bits = (before + n) * 8;

        memcpy(tail, s->buf + off, rest);
        tail[rest] = 0x80;
        stq_be_p(tail + len - 8, bits);
        for (uint32_t i = 0; i < len; i += 64) {
            sha256 ? sha256_block(h, tail + i) : sha1_block(h, tail + i);
        }
    } else if (off != n) {
        qemu_log_mask(LOG_GUEST_ERROR, "msm-crypto: segment of %u bytes is "
                      "not whole blocks and not the last\n", n);
    }
    before += n;
    s->regs[CE_BYTECNT0 / 4] = before;
    s->regs[CE_BYTECNT1 / 4] = before >> 32;
    s->fill = 0;
    s->seg_done = true;
}

static uint64_t crypto_read(void *opaque, hwaddr off, unsigned size)
{
    MsmCryptoState *s = opaque;

    if (off >= CE_DATA_IN) {
        return 0;
    }
    if (off == CE_STATUS) {
        return 0;
    }
    return off < sizeof(s->regs) ? s->regs[off / 4] : 0;
}

static void crypto_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MsmCryptoState *s = opaque;

    if (off >= CE_DATA_IN) {
        uint32_t want;

        if (s->seg_done) {
            return;
        }
        want = MIN(s->regs[CE_SEG_SIZE / 4], SEG_MAX);

        /* a big-endian word, as the data mover swapped it in */
        if (s->fill + 4 <= SEG_MAX) {
            stl_be_p(s->buf + s->fill, val);
            s->fill += MIN(4, want > s->fill ? want - s->fill : 0);
        }
        if (want && s->fill >= want) {
            s->fill = want;
            crypto_run(s);
        }
        return;
    }
    if (off < sizeof(s->regs)) {
        s->regs[off / 4] = val;
    }
    if (off == CE_GOPROC || off == CE_SEG_SIZE) {
        s->fill = 0;
        s->seg_done = false;
    }
}

static const MemoryRegionOps crypto_ops = {
    .read = crypto_read,
    .write = crypto_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void crypto_init(Object *obj)
{
    MsmCryptoState *s = MSM_CRYPTO(obj);

    s->buf = g_malloc(SEG_MAX + 4);
    memory_region_init_io(&s->iomem, obj, &crypto_ops, s, "msm-crypto",
                          0x10000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void crypto_reset(DeviceState *dev)
{
    MsmCryptoState *s = MSM_CRYPTO(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->fill = 0;
    s->seg_done = false;
}

static void crypto_class_init(ObjectClass *oc, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(oc), crypto_reset);
}

static const TypeInfo crypto_info = {
    .name = TYPE_MSM_CRYPTO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmCryptoState),
    .instance_init = crypto_init,
    .class_init = crypto_class_init,
};

static void crypto_register(void)
{
    type_register_static(&crypto_info);
}

type_init(crypto_register)
