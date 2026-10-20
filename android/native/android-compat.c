/*
 * Android 8.x (API 27) compatibility for the API 28 QEMU build.
 *
 * QEMU and glib are compiled against API 28 because that is the first
 * bionic that declares <iconv.h>, getrandom(), posix_spawn() and
 * pthread_attr_setinheritsched().  The Nexus 6P runs Android 8.1 (API 27),
 * where those symbols are not in libc, so the dynamic linker aborts before
 * main() is reached:
 *
 *   CANNOT LINK EXECUTABLE ... cannot locate symbol "getrandom"
 *
 * These definitions are linked into the QEMU executables themselves, so the
 * binaries keep working on API 28+ and also start on API 27.  Only the small
 * surface QEMU and glib actually reference is implemented here.
 *
 * getrandom() is implemented over /dev/urandom on purpose: the Nexus 6P's
 * 3.10 kernel predates the getrandom(2) syscall as well, so a raw syscall
 * would fail with ENOSYS.
 *
 * iconv() supports the UTF-8/ASCII/Latin-1/UTF-16/UCS-2/UCS-4 family.  QEMU
 * itself never converts charsets; this is here so glib's gconvert.o has a
 * working implementation if a locale path reaches it.
 *
 * posix_spawn() is only used by glib's g_spawn() (the FM radio's ffmpeg
 * source).  It returns ENOSYS, which g_spawn() reports as an error instead
 * of crashing.  Nothing on the boot path calls it.
 */
#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <iconv.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * On API 28 and later the real bionic functions exist.  Prefer them so the
 * shim does not change behaviour on modern phones; on API 27 dlsym() returns
 * NULL and the fallback below is used.  RTLD_NEXT skips this executable.
 */
#define COMPAT_REAL(fn)                                    \
    static __typeof__(fn) *fn##_real;                      \
    static int fn##_checked;                               \
    if (!fn##_checked) {                                   \
        fn##_real = (__typeof__(fn) *)dlsym(RTLD_NEXT, #fn); \
        fn##_checked = 1;                                  \
    }                                                      \
    if (fn##_real)

/* ------------------------------------------------------------------ */
/* getrandom(2)                                                        */
/* ------------------------------------------------------------------ */

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags)
{
    int fd;
    size_t done = 0;

    COMPAT_REAL(getrandom) {
        return getrandom_real(buf, buflen, flags);
    }

    (void)flags;

    /* QEMU probes with getrandom(NULL, 0, 0) to decide whether to use it. */
    if (buflen == 0) {
        return 0;
    }

    fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    while (done < buflen) {
        ssize_t got = read(fd, (char *)buf + done, buflen - done);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            {
                int saved = errno;
                close(fd);
                errno = saved;
                return -1;
            }
        }
        if (got == 0) {
            close(fd);
            errno = EIO;
            return -1;
        }
        done += (size_t)got;
    }

    close(fd);
    return (ssize_t)buflen;
}

/* ------------------------------------------------------------------ */
/* pthread_attr_setinheritsched()                                      */
/* ------------------------------------------------------------------ */

int pthread_attr_setinheritsched(pthread_attr_t *attr, int inheritsched)
{
    COMPAT_REAL(pthread_attr_setinheritsched) {
        return pthread_attr_setinheritsched_real(attr, inheritsched);
    }

    /*
     * PTHREAD_INHERIT_SCHED is the default glib asks for.  There is no
     * portable way to express it with the API 27 pthread_attr_t, and
     * leaving the attribute untouched gives the same behaviour.
     */
    (void)attr;
    (void)inheritsched;
    return 0;
}

/* ------------------------------------------------------------------ */
/* iconv                                                               */
/* ------------------------------------------------------------------ */

enum compat_encoding {
    ENC_UTF8,
    ENC_ASCII,
    ENC_LATIN1,
    ENC_UTF16LE,
    ENC_UTF16BE,
    ENC_UCS2LE,
    ENC_UCS2BE,
    ENC_UCS4LE,
    ENC_UCS4BE,
};

struct compat_iconv {
    enum compat_encoding from;
    enum compat_encoding to;
};

static int compat_name_is(const char *name, const char *want)
{
    char buf[32];
    size_t i = 0;

    while (*name && *name != '/' && i + 1 < sizeof(buf)) {
        char c = *name++;
        if (c == '-' || c == '_' || c == ' ') {
            continue;
        }
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        buf[i++] = c;
    }
    buf[i] = '\0';
    return strcmp(buf, want) == 0;
}

static int compat_encoding_for(const char *name, enum compat_encoding *out)
{
    if (compat_name_is(name, "UTF8") || compat_name_is(name, "UTF")) {
        *out = ENC_UTF8;
    } else if (compat_name_is(name, "ASCII") ||
               compat_name_is(name, "USASCII") ||
               compat_name_is(name, "ANSIX3.4-1968") ||
               compat_name_is(name, "ANSIX3.4-1986")) {
        *out = ENC_ASCII;
    } else if (compat_name_is(name, "ISO88591") ||
               compat_name_is(name, "ISO8859-1") ||
               compat_name_is(name, "LATIN1") ||
               compat_name_is(name, "L1")) {
        *out = ENC_LATIN1;
    } else if (compat_name_is(name, "UTF16LE")) {
        *out = ENC_UTF16LE;
    } else if (compat_name_is(name, "UTF16BE") ||
               compat_name_is(name, "UTF16")) {
        *out = ENC_UTF16BE;
    } else if (compat_name_is(name, "UCS2LE")) {
        *out = ENC_UCS2LE;
    } else if (compat_name_is(name, "UCS2BE") ||
               compat_name_is(name, "UCS2")) {
        *out = ENC_UCS2BE;
    } else if (compat_name_is(name, "UCS4LE")) {
        *out = ENC_UCS4LE;
    } else if (compat_name_is(name, "UCS4BE") ||
               compat_name_is(name, "UCS4") ||
               compat_name_is(name, "UTF32") ||
               compat_name_is(name, "UTF32BE")) {
        *out = ENC_UCS4BE;
    } else {
        return -1;
    }
    return 0;
}

iconv_t iconv_open(const char *tocode, const char *fromcode)
{
    struct compat_iconv *cd;

    cd = calloc(1, sizeof(*cd));
    if (cd == NULL) {
        return (iconv_t)-1;
    }
    if (compat_encoding_for(tocode, &cd->to) < 0 ||
        compat_encoding_for(fromcode, &cd->from) < 0) {
        free(cd);
        errno = EINVAL;
        return (iconv_t)-1;
    }
    return (iconv_t)cd;
}

static int compat_decode(enum compat_encoding enc, const unsigned char *in,
                         size_t left, uint32_t *cp, size_t *used)
{
    switch (enc) {
    case ENC_UTF8: {
        unsigned char b = in[0];
        if (b < 0x80) {
            *cp = b;
            *used = 1;
            return 0;
        }
        if (b >= 0xC2 && b <= 0xDF) {
            if (left < 2) return 1;
            if ((in[1] & 0xC0) != 0x80) return -1;
            *cp = ((uint32_t)(b & 0x1F) << 6) | (in[1] & 0x3F);
            *used = 2;
            return 0;
        }
        if (b >= 0xE0 && b <= 0xEF) {
            if (left < 3) return 1;
            if ((in[1] & 0xC0) != 0x80 || (in[2] & 0xC0) != 0x80) return -1;
            if (b == 0xE0 && in[1] < 0xA0) return -1;
            if (b == 0xED && in[1] >= 0xA0) return -1;
            *cp = ((uint32_t)(b & 0x0F) << 12) |
                  ((uint32_t)(in[1] & 0x3F) << 6) | (in[2] & 0x3F);
            *used = 3;
            return 0;
        }
        if (b >= 0xF0 && b <= 0xF4) {
            if (left < 4) return 1;
            if ((in[1] & 0xC0) != 0x80 || (in[2] & 0xC0) != 0x80 ||
                (in[3] & 0xC0) != 0x80) return -1;
            if (b == 0xF0 && in[1] < 0x90) return -1;
            if (b == 0xF4 && in[1] >= 0x90) return -1;
            *cp = ((uint32_t)(b & 0x07) << 18) |
                  ((uint32_t)(in[1] & 0x3F) << 12) |
                  ((uint32_t)(in[2] & 0x3F) << 6) | (in[3] & 0x3F);
            *used = 4;
            return 0;
        }
        return -1;
    }
    case ENC_ASCII:
        if (in[0] > 0x7F) return -1;
        *cp = in[0];
        *used = 1;
        return 0;
    case ENC_LATIN1:
        *cp = in[0];
        *used = 1;
        return 0;
    case ENC_UTF16LE:
    case ENC_UTF16BE:
    case ENC_UCS2LE:
    case ENC_UCS2BE: {
        uint32_t w;
        int be = (enc == ENC_UTF16BE || enc == ENC_UCS2BE);
        if (left < 2) return 1;
        w = be ? ((uint32_t)in[0] << 8 | in[1])
               : ((uint32_t)in[1] << 8 | in[0]);
        *used = 2;
        if (w >= 0xD800 && w <= 0xDBFF &&
            (enc == ENC_UTF16LE || enc == ENC_UTF16BE)) {
            uint32_t w2;
            if (left < 4) return 1;
            w2 = be ? ((uint32_t)in[2] << 8 | in[3])
                    : ((uint32_t)in[3] << 8 | in[2]);
            if (w2 < 0xDC00 || w2 > 0xDFFF) return -1;
            *cp = 0x10000 + ((w - 0xD800) << 10) + (w2 - 0xDC00);
            *used = 4;
            return 0;
        }
        if (w >= 0xD800 && w <= 0xDFFF) return -1;
        *cp = w;
        return 0;
    }
    case ENC_UCS4LE:
    case ENC_UCS4BE: {
        uint32_t v;
        int be = (enc == ENC_UCS4BE);
        if (left < 4) return 1;
        v = be ? ((uint32_t)in[0] << 24 | (uint32_t)in[1] << 16 |
                  (uint32_t)in[2] << 8 | in[3])
               : ((uint32_t)in[3] << 24 | (uint32_t)in[2] << 16 |
                  (uint32_t)in[1] << 8 | in[0]);
        if (v > 0x10FFFF) return -1;
        *cp = v;
        *used = 4;
        return 0;
    }
    }
    return -1;
}

static int compat_encode(enum compat_encoding enc, uint32_t cp,
                         unsigned char *out, size_t *used)
{
    switch (enc) {
    case ENC_UTF8:
        if (cp < 0x80) {
            out[0] = (unsigned char)cp;
            *used = 1;
        } else if (cp < 0x800) {
            out[0] = (unsigned char)(0xC0 | (cp >> 6));
            out[1] = (unsigned char)(0x80 | (cp & 0x3F));
            *used = 2;
        } else if (cp < 0x10000) {
            if (cp >= 0xD800 && cp <= 0xDFFF) return -1;
            out[0] = (unsigned char)(0xE0 | (cp >> 12));
            out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
            out[2] = (unsigned char)(0x80 | (cp & 0x3F));
            *used = 3;
        } else if (cp <= 0x10FFFF) {
            out[0] = (unsigned char)(0xF0 | (cp >> 18));
            out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
            out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
            out[3] = (unsigned char)(0x80 | (cp & 0x3F));
            *used = 4;
        } else {
            return -1;
        }
        return 0;
    case ENC_ASCII:
        if (cp > 0x7F) return -1;
        out[0] = (unsigned char)cp;
        *used = 1;
        return 0;
    case ENC_LATIN1:
        if (cp > 0xFF) return -1;
        out[0] = (unsigned char)cp;
        *used = 1;
        return 0;
    case ENC_UTF16LE:
    case ENC_UTF16BE:
    case ENC_UCS2LE:
    case ENC_UCS2BE: {
        int be = (enc == ENC_UTF16BE || enc == ENC_UCS2BE);
        if (cp > 0xFFFF) {
            uint32_t w1, w2;
            if (enc == ENC_UCS2LE || enc == ENC_UCS2BE) return -1;
            cp -= 0x10000;
            w1 = 0xD800 + (cp >> 10);
            w2 = 0xDC00 + (cp & 0x3FF);
            if (be) {
                out[0] = (unsigned char)(w1 >> 8); out[1] = (unsigned char)w1;
                out[2] = (unsigned char)(w2 >> 8); out[3] = (unsigned char)w2;
            } else {
                out[0] = (unsigned char)w1; out[1] = (unsigned char)(w1 >> 8);
                out[2] = (unsigned char)w2; out[3] = (unsigned char)(w2 >> 8);
            }
            *used = 4;
            return 0;
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) return -1;
        if (be) {
            out[0] = (unsigned char)(cp >> 8); out[1] = (unsigned char)cp;
        } else {
            out[0] = (unsigned char)cp; out[1] = (unsigned char)(cp >> 8);
        }
        *used = 2;
        return 0;
    }
    case ENC_UCS4LE:
    case ENC_UCS4BE:
        if (cp > 0x10FFFF) return -1;
        if (enc == ENC_UCS4BE) {
            out[0] = (unsigned char)(cp >> 24); out[1] = (unsigned char)(cp >> 16);
            out[2] = (unsigned char)(cp >> 8); out[3] = (unsigned char)cp;
        } else {
            out[0] = (unsigned char)cp; out[1] = (unsigned char)(cp >> 8);
            out[2] = (unsigned char)(cp >> 16); out[3] = (unsigned char)(cp >> 24);
        }
        *used = 4;
        return 0;
    }
    return -1;
}

size_t iconv(iconv_t cd, char **inbuf, size_t *inbytesleft,
             char **outbuf, size_t *outbytesleft)
{
    struct compat_iconv *c = (struct compat_iconv *)cd;
    unsigned char *in, *out;
    size_t left, oleft;

    if (inbuf == NULL || *inbuf == NULL) {
        return 0;
    }
    if (c == NULL) {
        errno = EBADF;
        return (size_t)-1;
    }

    in = (unsigned char *)*inbuf;
    out = (unsigned char *)*outbuf;
    left = *inbytesleft;
    oleft = *outbytesleft;

    while (left > 0) {
        uint32_t cp;
        unsigned char tmp[4];
        size_t used = 0, olen = 0;
        int ret;

        ret = compat_decode(c->from, in, left, &cp, &used);
        if (ret == 1) {
            errno = EINVAL;
            goto fail;
        }
        if (ret < 0) {
            errno = EILSEQ;
            goto fail;
        }
        ret = compat_encode(c->to, cp, tmp, &olen);
        if (ret < 0) {
            errno = EILSEQ;
            goto fail;
        }
        if (olen > oleft) {
            errno = E2BIG;
            goto fail;
        }
        memcpy(out, tmp, olen);
        in += used;
        left -= used;
        out += olen;
        oleft -= olen;
    }

    *inbuf = (char *)in;
    *outbuf = (char *)out;
    *inbytesleft = left;
    *outbytesleft = oleft;
    return 0;

fail:
    *inbuf = (char *)in;
    *outbuf = (char *)out;
    *inbytesleft = left;
    *outbytesleft = oleft;
    return (size_t)-1;
}

int iconv_close(iconv_t cd)
{
    free(cd);
    return 0;
}

/* ------------------------------------------------------------------ */
/* posix_spawn (glib g_spawn, only used by the FM radio's ffmpeg)      */
/* ------------------------------------------------------------------ */

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp,
                char *const argv[], char *const envp[])
{
    COMPAT_REAL(posix_spawn) {
        return posix_spawn_real(pid, path, file_actions, attrp, argv, envp);
    }

    (void)pid;
    (void)path;
    (void)file_actions;
    (void)attrp;
    (void)argv;
    (void)envp;
    errno = ENOSYS;
    return ENOSYS;
}

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *attrp,
                 char *const argv[], char *const envp[])
{
    COMPAT_REAL(posix_spawnp) {
        return posix_spawnp_real(pid, file, file_actions, attrp, argv, envp);
    }

    return posix_spawn(pid, file, file_actions, attrp, argv, envp);
}

int posix_spawnattr_init(posix_spawnattr_t *attr)
{
    COMPAT_REAL(posix_spawnattr_init) {
        return posix_spawnattr_init_real(attr);
    }

    memset(attr, 0, sizeof(*attr));
    return 0;
}

int posix_spawnattr_destroy(posix_spawnattr_t *attr)
{
    COMPAT_REAL(posix_spawnattr_destroy) {
        return posix_spawnattr_destroy_real(attr);
    }

    (void)attr;
    return 0;
}

int posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags)
{
    COMPAT_REAL(posix_spawnattr_setflags) {
        return posix_spawnattr_setflags_real(attr, flags);
    }

    (void)attr;
    (void)flags;
    return 0;
}

int posix_spawnattr_setsigdefault(posix_spawnattr_t *attr, const sigset_t *set)
{
    COMPAT_REAL(posix_spawnattr_setsigdefault) {
        return posix_spawnattr_setsigdefault_real(attr, set);
    }

    (void)attr;
    (void)set;
    return 0;
}

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *file_actions)
{
    COMPAT_REAL(posix_spawn_file_actions_init) {
        return posix_spawn_file_actions_init_real(file_actions);
    }

    memset(file_actions, 0, sizeof(*file_actions));
    return 0;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *file_actions)
{
    COMPAT_REAL(posix_spawn_file_actions_destroy) {
        return posix_spawn_file_actions_destroy_real(file_actions);
    }

    (void)file_actions;
    return 0;
}

int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *file_actions,
                                      int fd)
{
    COMPAT_REAL(posix_spawn_file_actions_addclose) {
        return posix_spawn_file_actions_addclose_real(file_actions, fd);
    }

    (void)file_actions;
    (void)fd;
    return 0;
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *file_actions,
                                     int fd, int newfd)
{
    COMPAT_REAL(posix_spawn_file_actions_adddup2) {
        return posix_spawn_file_actions_adddup2_real(file_actions, fd, newfd);
    }

    (void)file_actions;
    (void)fd;
    (void)newfd;
    return 0;
}

/* ------------------------------------------------------------------ */
/* LeakSanitizer hooks (weak in the API 28 binary)                     */
/* ------------------------------------------------------------------ */

void __lsan_disable(void) {}
void __lsan_enable(void) {}
void __lsan_ignore_object(const void *p) { (void)p; }
