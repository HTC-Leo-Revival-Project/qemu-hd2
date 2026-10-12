/*
 * hd2nanddump DEVICE PREFIX
 *
 * Reads a NAND MTD device page by page (the data, then the spare/OOB bytes
 * the driver exposes) and writes:
 *   PREFIX.nand  every page as writesize data bytes + oobsize OOB bytes;
 *                blocks the driver reports bad are written as 0xFF
 *   PREFIX.txt   geometry, bad blocks, pages that failed to read and ECC
 *                statistics (a failed read keeps what the driver returned;
 *                a failed OOB read leaves the OOB as 0xFF)
 * Read only: the device is opened O_RDONLY and only read ioctls are used.
 *
 * Built without a C library (raw ARM EABI system calls) so the same binary
 * runs on the HD2's 2.6.32 recovery kernel and anything later:
 *   arm-eabi-gcc -std=gnu99 -Os -march=armv5te -mabi=aapcs-linux -nostdlib -static -fno-pic
 *       -fno-builtin -Wl,-Ttext=0x10000 -o hd2nanddump hd2nanddump.c -lgcc
 */

typedef unsigned int u32;
typedef unsigned long long u64;

#define O_RDONLY    0
#define O_WRONLY    1
#define O_CREAT     0100
#define O_TRUNC     01000
#define SEEK_SET    0

#define MEMGETINFO      0x80204d01
#define MEMREADOOB      0xc00c4d04
#define MEMGETBADBLOCK  0x40084d0b
#define ECCGETSTATS     0x80104d12

struct mtd_info_user {
    unsigned char type;
    u32 flags, size, erasesize, writesize, oobsize, ecctype, eccsize;
};

struct mtd_oob_buf {
    u32 start, length;
    unsigned char *ptr;
};

struct mtd_ecc_stats {
    u32 corrected, failed, badblocks, bbtblocks;
};

static long sys(long n, long a, long b, long c, long d, long e)
{
    register long r0 asm("r0") = a, r1 asm("r1") = b, r2 asm("r2") = c;
    register long r3 asm("r3") = d, r4 asm("r4") = e, r7 asm("r7") = n;

    asm volatile("svc #0" : "+r"(r0)
                 : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r7) : "memory");
    return r0;
}

#define sys_exit(c)         sys(1, (c), 0, 0, 0, 0)
#define sys_read(f, b, n)   sys(3, (f), (long)(b), (n), 0, 0)
#define sys_write(f, b, n)  sys(4, (f), (long)(b), (n), 0, 0)
#define sys_open(p, fl, m)  sys(5, (long)(p), (fl), (m), 0, 0)
#define sys_close(f)        sys(6, (f), 0, 0, 0, 0)
#define sys_fsync(f)        sys(118, (f), 0, 0, 0, 0)
#define sys_ioctl(f, r, a)  sys(54, (f), (long)(r), (long)(a), 0, 0)

static long sys_seek(int fd, u64 off)
{
    u64 result;

    return sys(140, fd, (long)(off >> 32), (long)off, (long)&result,
               SEEK_SET);
}

/* the compiler may call these for struct copies and loops */
void *memset(void *d, int c, unsigned long n)
{
    unsigned char *p = d;

    while (n--) {
        *p++ = c;
    }
    return d;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
    unsigned char *p = d;
    const unsigned char *q = s;

    while (n--) {
        *p++ = *q++;
    }
    return d;
}

/* ---- tiny output helpers ---- */

static char line[256];
static int line_len;

static void put(const char *s)
{
    while (*s && line_len < (int)sizeof(line) - 1) {
        line[line_len++] = *s++;
    }
}

static void put_num(u32 v, u32 base, int width)
{
    char tmp[24];
    int n = 0;

    do {
        tmp[n++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    while (n < width) {
        tmp[n++] = '0';
    }
    while (n) {
        char c[2] = { tmp[--n], 0 };
        put(c);
    }
}

static void flush(int fd)
{
    sys_write(fd, line, line_len);
    line_len = 0;
}

/* to the console and the report file */
static int report = -1;

static void say(void)
{
    sys_write(1, line, line_len);
    if (report >= 0) {
        sys_write(report, line, line_len);
    }
    line_len = 0;
}

static int str_len(const char *s)
{
    int n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

static void str_cat(char *d, const char *a, const char *b)
{
    while (*a) {
        *d++ = *a++;
    }
    while (*b) {
        *d++ = *b++;
    }
    *d = 0;
}

static void fail(const char *what, long err)
{
    put("hd2nanddump: ");
    put(what);
    put(" failed (error ");
    put_num(-err, 10, 0);
    put(")\n");
    say();
    sys_exit(1);
}

/* one 128 KiB block of 64 pages with their OOB, at most 4 KiB pages */
static unsigned char blockbuf[256 * (4096 + 224)];

void main_c(int argc, char **argv)
{
    struct mtd_info_user mi;
    struct mtd_ecc_stats before, after;
    char path[512];
    int dev, out;
    u32 blocks, pages, rec, bad = 0, errors = 0;
    long r;

    if (argc != 3) {
        put("usage: hd2nanddump /dev/mtd/mtdN PREFIX\n");
        say();
        sys_exit(2);
    }
    if (str_len(argv[2]) > 400) {
        fail("prefix length", -36);
    }
    dev = sys_open(argv[1], O_RDONLY, 0);
    if (dev < 0) {
        fail("open device", dev);
    }
    r = sys_ioctl(dev, MEMGETINFO, &mi);
    if (r < 0) {
        fail("MEMGETINFO", r);
    }
    if (!mi.erasesize || !mi.writesize || mi.erasesize % mi.writesize ||
        mi.erasesize / mi.writesize > 256 || mi.writesize > 4096 ||
        mi.oobsize > 224) {
        fail("unexpected geometry", -22);
    }
    blocks = mi.size / mi.erasesize;
    pages = mi.erasesize / mi.writesize;
    rec = mi.writesize + mi.oobsize;

    str_cat(path, argv[2], ".txt");
    report = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    str_cat(path, argv[2], ".nand");
    out = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (report < 0 || out < 0) {
        fail("create output", report < 0 ? report : out);
    }

    put("device ");
    put(argv[1]);
    put("\nsize 0x");
    put_num(mi.size, 16, 0);
    put(" (");
    put_num(blocks, 10, 0);
    put(" blocks of 0x");
    put_num(mi.erasesize, 16, 0);
    put("), page ");
    put_num(mi.writesize, 10, 0);
    put(" + oob ");
    put_num(mi.oobsize, 10, 0);
    put("\nlayout: per page, data then oob; bad blocks as 0xff\n");
    say();

    before.failed = before.corrected = 0;
    sys_ioctl(dev, ECCGETSTATS, &before);

    for (u32 b = 0; b < blocks; b++) {
        u64 base = (u64)b * mi.erasesize;
        unsigned char *p = blockbuf;
        long isbad = sys_ioctl(dev, MEMGETBADBLOCK, &base);

        if (isbad > 0) {
            for (u32 i = 0; i < pages * rec; i++) {
                blockbuf[i] = 0xff;
            }
            bad++;
            put("bad block ");
            put_num(b, 10, 0);
            put(" (0x");
            put_num(b, 16, 0);
            put(")\n");
            say();
        } else {
            for (u32 pg = 0; pg < pages; pg++, p += rec) {
                struct mtd_oob_buf oob = {
                    (u32)(base + (u64)pg * mi.writesize), mi.oobsize,
                    p + mi.writesize,
                };
                long rd, ro;
                u32 i;

                for (i = 0; i < rec; i++) {
                    p[i] = 0xff;
                }
                if (sys_seek(dev, base + (u64)pg * mi.writesize) < 0) {
                    fail("seek", -5);
                }
                rd = sys_read(dev, p, mi.writesize);
                ro = sys_ioctl(dev, MEMREADOOB, &oob);
                if (rd != (long)mi.writesize || ro < 0) {
                    errors++;
                    if (errors <= 64) {
                        put("read error at block ");
                        put_num(b, 10, 0);
                        put(" page ");
                        put_num(pg, 10, 0);
                        put(rd != (long)mi.writesize ? " (data" : " (oob");
                        put(", error ");
                        put_num(rd != (long)mi.writesize ?
                                (rd < 0 ? -rd : 5) : -ro, 10, 0);
                        put(")\n");
                        say();
                    }
                }
            }
        }
        if (sys_write(out, blockbuf, pages * rec) != (long)(pages * rec)) {
            fail("write output (is the SD card full?)", -28);
        }
        if (b % 32 == 31 || b == blocks - 1) {
            put("  ");
            put_num(b + 1, 10, 0);
            put("/");
            put_num(blocks, 10, 0);
            put(" blocks\n");
            flush(1);
        }
    }

    after = before;
    sys_ioctl(dev, ECCGETSTATS, &after);
    put("done: ");
    put_num(blocks, 10, 0);
    put(" blocks, ");
    put_num(bad, 10, 0);
    put(" bad, ECC corrected ");
    put_num(after.corrected - before.corrected, 10, 0);
    put(", uncorrectable ");
    put_num(after.failed - before.failed, 10, 0);
    put(", pages with read errors ");
    put_num(errors, 10, 0);
    put("\n");
    say();
    sys_fsync(out);
    sys_fsync(report);
    sys_close(out);
    sys_close(report);
    sys_close(dev);
    sys_exit(0);
}

asm(".globl _start\n_start:\n ldr r0, [sp]\n add r1, sp, #4\n bl main_c\n");
