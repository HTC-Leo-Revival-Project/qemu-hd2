/*
 * HD2 Bench native tests: CPU (integer, floating point, memory) and
 * storage (sequential and random I/O with the page cache dropped).
 *
 * Plain C against the API 16 NDK sysroot, loadable on Android 4.0.4: only
 * old bionic symbols (fadvise goes through syscall(), posix_memalign and
 * posix_fadvise are newer than ICS).
 */
#include <jni.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define JFN(name) Java_com_hd2bench_Native_##name

static volatile uint32_t sink;
static char last_error[256];

static double now(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

/* 16-byte aligned memory from plain malloc (no posix_memalign on ICS) */
static void *alloc_aligned(size_t n, void **raw)
{
    *raw = malloc(n + 64);
    if (!*raw) {
        return NULL;
    }
    return (void *)(((uintptr_t)*raw + 63) & ~(uintptr_t)63);
}

/*
 * Run fn(iters) with doubling iteration counts until one run takes at
 * least secs; return units of work per second (fn returns the work done).
 */
typedef double (*work_fn)(void *ctx, long iters);

static double rate(work_fn fn, void *ctx, double secs)
{
    long iters = 1;

    fn(ctx, 1);     /* warm up */
    for (;;) {
        double t0 = now(), work = fn(ctx, iters), dt = now() - t0;

        if (dt >= secs || iters > (1L << 28)) {
            return work / dt;
        }
        iters = dt < secs / 8 ? iters * 8 : iters * 2;
    }
}

/* ---------------------------------------------------------------- */
/* CPU                                                               */
/* ---------------------------------------------------------------- */

static uint32_t crc_table[256];
static uint8_t *crc_buf;

static double crc_work(void *ctx, long iters)
{
    uint32_t crc = 0xffffffff;

    for (long i = 0; i < iters; i++) {
        for (int j = 0; j < 65536; j++) {
            crc = crc_table[(crc ^ crc_buf[j]) & 0xff] ^ (crc >> 8);
        }
    }
    sink = crc;
    return iters * 65536.0 / 1e6;      /* MB */
}

JNIEXPORT jdouble JNICALL JFN(cpuCrc32)(JNIEnv *e, jclass c, jdouble secs)
{
    double r;

    for (uint32_t i = 0; i < 256; i++) {
        uint32_t v = i;

        for (int k = 0; k < 8; k++) {
            v = (v >> 1) ^ (0xedb88320 & -(v & 1));
        }
        crc_table[i] = v;
    }
    crc_buf = malloc(65536);
    if (!crc_buf) {
        return -1;
    }
    for (int i = 0; i < 65536; i++) {
        crc_buf[i] = i * 7 + (i >> 8);
    }
    r = rate(crc_work, NULL, secs);
    free(crc_buf);
    return r;
}

/* integer mix: multiply, shifts, xor, add and a software divide */
static double intmix_work(void *ctx, long iters)
{
    uint32_t x = 12345, y = 678, z = 9;

    for (long i = 0; i < iters; i++) {
        for (int j = 0; j < 1000; j++) {
            x = x * 1103515245u + 12345u;
            y ^= x >> 7;
            z += y / ((x & 255) + 1);
            y += z << 3;
        }
    }
    sink = x + y + z;
    return iters * 1000.0 * 8 / 1e6;    /* Mops (8 ops per round) */
}

JNIEXPORT jdouble JNICALL JFN(cpuIntMix)(JNIEnv *e, jclass c, jdouble secs)
{
    return rate(intmix_work, NULL, secs);
}

/* double precision 64x64 matrix multiply (VFP) */
#define MN 64
static double *ma, *mb, *mc;

static double dgemm_work(void *ctx, long iters)
{
    for (long it = 0; it < iters; it++) {
        for (int i = 0; i < MN; i++) {
            for (int j = 0; j < MN; j++) {
                double s = 0;

                for (int k = 0; k < MN; k++) {
                    s += ma[i * MN + k] * mb[k * MN + j];
                }
                mc[i * MN + j] = s;
            }
        }
        ma[it % (MN * MN)] += mc[0] * 1e-9;
    }
    sink = (uint32_t)mc[5];
    return iters * 2.0 * MN * MN * MN / 1e6;    /* MFLOP */
}

JNIEXPORT jdouble JNICALL JFN(cpuDgemm)(JNIEnv *e, jclass c, jdouble secs)
{
    double r;

    ma = malloc(3 * MN * MN * sizeof(double));
    if (!ma) {
        return -1;
    }
    mb = ma + MN * MN;
    mc = mb + MN * MN;
    for (int i = 0; i < MN * MN; i++) {
        ma[i] = (i % 17) * 0.25;
        mb[i] = (i % 13) * 0.5;
    }
    r = rate(dgemm_work, NULL, secs);
    free(ma);
    return r;
}

/* single precision a*x+y over 4096 floats: NEON when vectorized */
#define SN 4096
static float *sx, *sy;

static double saxpy_work(void *ctx, long iters)
{
    float a = 1.0001f;

    for (long it = 0; it < iters; it++) {
        for (int i = 0; i < SN; i++) {
            sy[i] = a * sx[i] + sy[i];
        }
        a = -a;
    }
    sink = (uint32_t)sy[7];
    return iters * 2.0 * SN / 1e6;
}

JNIEXPORT jdouble JNICALL JFN(cpuSaxpy)(JNIEnv *e, jclass c, jdouble secs)
{
    void *raw;
    double r;

    sx = alloc_aligned(2 * SN * sizeof(float), &raw);
    if (!sx) {
        return -1;
    }
    sy = sx + SN;
    for (int i = 0; i < SN; i++) {
        sx[i] = i * 0.001f;
        sy[i] = 1.0f;
    }
    r = rate(saxpy_work, NULL, secs);
    free(raw);
    return r;
}

/* memory bandwidth: memcpy between two 8 MiB buffers */
#define MB8 (8 << 20)
static uint8_t *mem_a, *mem_b;

static double memcpy_work(void *ctx, long iters)
{
    for (long i = 0; i < iters; i++) {
        memcpy(i & 1 ? mem_a : mem_b, i & 1 ? mem_b : mem_a, MB8);
    }
    sink = mem_a[100] + mem_b[200];
    return iters * (double)MB8 / 1e6;
}

JNIEXPORT jdouble JNICALL JFN(memCopy)(JNIEnv *e, jclass c, jdouble secs)
{
    void *ra, *rb;
    double r;

    mem_a = alloc_aligned(MB8, &ra);
    mem_b = alloc_aligned(MB8, &rb);
    if (!mem_a || !mem_b) {
        free(ra);
        free(rb);
        return -1;
    }
    memset(mem_a, 1, MB8);
    memset(mem_b, 2, MB8);
    r = rate(memcpy_work, NULL, secs);
    free(ra);
    free(rb);
    return r;
}

/* memory latency: a random pointer chase through 16 MiB */
#define CHASE (16 << 20)
static uint32_t *chase;

static double chase_work(void *ctx, long iters)
{
    uint32_t p = 0;

    for (long i = 0; i < iters; i++) {
        for (int j = 0; j < 100000; j++) {
            p = chase[p];
        }
    }
    sink = p;
    return iters * 100000.0;
}

JNIEXPORT jdouble JNICALL JFN(memLatency)(JNIEnv *e, jclass c, jdouble secs)
{
    uint32_t n = CHASE / 4 / 16, seed = 1;   /* one entry per 64 bytes */
    uint32_t *order;
    double r;

    chase = malloc(CHASE);
    order = malloc(n * sizeof(uint32_t));
    if (!chase || !order) {
        free(chase);
        free(order);
        return -1;
    }
    for (uint32_t i = 0; i < n; i++) {
        order[i] = i;
    }
    for (uint32_t i = n - 1; i > 0; i--) {
        uint32_t j, t;

        seed = seed * 1664525u + 1013904223u;
        j = seed % (i + 1);
        t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    for (uint32_t i = 0; i < n; i++) {
        chase[order[i] * 16] = order[(i + 1) % n] * 16;
    }
    free(order);
    r = rate(chase_work, NULL, secs);
    free(chase);
    return 1e9 / r;     /* ns per load */
}

/* ---------------------------------------------------------------- */
/* Storage                                                           */
/* ---------------------------------------------------------------- */

#ifndef __NR_arm_fadvise64_64
#define __NR_arm_fadvise64_64 270
#endif

/* forget the file's cached pages, so that reads come from the device */
static void drop_cache(int fd)
{
    fsync(fd);
    /* fd, advice POSIX_FADV_DONTNEED, offset 0, length 0 (all) */
    syscall(__NR_arm_fadvise64_64, fd, 4, 0, 0, 0, 0);
}

static uint32_t rnd(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

#define CHUNK (64 << 10)

/*
 * In dir: [sequential write MB/s, sequential read MB/s, random 4K read
 * IOPS, random 4K synced write IOPS, free MB]; NULL on error (lastError).
 */
JNIEXPORT jdoubleArray JNICALL JFN(storage)(JNIEnv *e, jclass c, jstring jdir,
                                            jint mb)
{
    const char *dir = (*e)->GetStringUTFChars(e, jdir, NULL);
    char path[512];
    struct statfs sf;
    double res[5] = { 0 };
    void *raw;
    uint8_t *buf = alloc_aligned(CHUNK, &raw);
    uint32_t seed = 4321;
    long nchunks = (long)mb * (1 << 20) / CHUNK;
    long pages = (long)mb * (1 << 20) / 4096;
    int fd = -1, ok = 0;
    double t0;
    jdoubleArray out = NULL;

    snprintf(path, sizeof(path), "%s/hd2bench.tmp", dir);
    (*e)->ReleaseStringUTFChars(e, jdir, dir);
    if (!buf) {
        snprintf(last_error, sizeof(last_error), "out of memory");
        return NULL;
    }
    for (int i = 0; i < CHUNK; i++) {
        buf[i] = rnd(&seed);
    }
    {
        char d[512];

        snprintf(d, sizeof(d), "%s", path);
        *strrchr(d, '/') = 0;
        if (!statfs(d, &sf)) {
            res[4] = (double)sf.f_bavail * sf.f_bsize / (1 << 20);
            if (res[4] < mb + 4) {
                snprintf(last_error, sizeof(last_error),
                         "only %.0f MB free, %d MB needed", res[4], mb + 4);
                goto out;
            }
        }
    }

    /* sequential write, through to the device */
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        snprintf(last_error, sizeof(last_error), "open %s: %s", path,
                 strerror(errno));
        goto out;
    }
    t0 = now();
    for (long i = 0; i < nchunks; i++) {
        buf[0] = i;
        if (write(fd, buf, CHUNK) != CHUNK) {
            snprintf(last_error, sizeof(last_error), "write: %s",
                     strerror(errno));
            goto out;
        }
    }
    fsync(fd);
    res[0] = mb / (now() - t0);

    /* sequential read, from the device */
    drop_cache(fd);
    lseek(fd, 0, SEEK_SET);
    t0 = now();
    for (long i = 0; i < nchunks; i++) {
        if (read(fd, buf, CHUNK) != CHUNK) {
            snprintf(last_error, sizeof(last_error), "read: %s",
                     strerror(errno));
            goto out;
        }
    }
    res[1] = mb / (now() - t0);

    /* random 4 KiB reads */
    drop_cache(fd);
    t0 = now();
    for (int i = 0; i < 512; i++) {
        off_t o = (off_t)(rnd(&seed) % pages) * 4096;

        if (pread(fd, buf, 4096, o) != 4096) {
            snprintf(last_error, sizeof(last_error), "pread: %s",
                     strerror(errno));
            goto out;
        }
    }
    res[2] = 512 / (now() - t0);

    /* random 4 KiB writes, each synced (as a database commit) */
    t0 = now();
    for (int i = 0; i < 64; i++) {
        off_t o = (off_t)(rnd(&seed) % pages) * 4096;

        buf[1] = i;
        if (pwrite(fd, buf, 4096, o) != 4096) {
            snprintf(last_error, sizeof(last_error), "pwrite: %s",
                     strerror(errno));
            goto out;
        }
        fdatasync(fd);
    }
    res[3] = 64 / (now() - t0);
    ok = 1;

out:
    if (fd >= 0) {
        close(fd);
        unlink(path);
    }
    free(raw);
    if (ok) {
        out = (*e)->NewDoubleArray(e, 5);
        (*e)->SetDoubleArrayRegion(e, out, 0, 5, res);
    }
    return out;
}

JNIEXPORT jstring JNICALL JFN(lastError)(JNIEnv *e, jclass c)
{
    return (*e)->NewStringUTF(e, last_error);
}
