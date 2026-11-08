/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The system's libraries for the 32-bit bridge (arm32.h): bionic's libc and libm, liblog, libdl and the kernel's
   helpers in ILP32's layouts, the guest's printf and scanf, and GLES 1.1, whose handlers are the 64-bit bridge's
   (hle_gles.c): a call's 32-bit arguments are put in an AArch64 CPU's registers, as AAPCS64 passes them, its pointers
   made host pointers */
#define _GNU_SOURCE
#include "android/bridge/arm32.h"
#include "android/bridge/hle.h"
#include "android/bridge/hle_impl.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "gnuabi/kernel/kernel.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if !defined(_WIN32)
#include <sys/mman.h>
#endif

#define R(n)    (c->r[n])
#define P(n)    (arm32_host(c->r[n]))
#define S(n)    ((const char*)arm32_host(c->r[n]))
#define HANDLER(name) static void name(SnxArmCPU* c, void* data)
#define UNUSED (void)data

static uint32_t rd32(uint32_t a) { return snx_ld_le32(arm32_mem + a); }
static void wr32(uint32_t a, uint32_t v) { snx_st_le32(arm32_mem + a, v); }
/* the n'th argument word, past r3 on the stack */
static uint32_t word(const SnxArmCPU* c, unsigned n) { return n < 4 ? c->r[n] : rd32(c->r[13] + 4 * (n - 4)); }
static void set64(SnxArmCPU* c, uint64_t v) {
    c->r[0] = (uint32_t)v;
    c->r[1] = (uint32_t)(v >> 32);
}
static double arg_double(const SnxArmCPU* c, unsigned first) {
    uint64_t bits = word(c, first) | (uint64_t)word(c, first + 1) << 32;
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}
static float arg_float(const SnxArmCPU* c, unsigned n) {
    uint32_t bits = word(c, n);
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}
static void ret_double(SnxArmCPU* c, double d) {
    uint64_t bits;
    memcpy(&bits, &d, sizeof bits);
    set64(c, bits);
}
static void ret_float(SnxArmCPU* c, float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits);
    c->r[0] = bits;
}
static void set_errno(int host_error) { wr32(arm32_errno_address(), (uint32_t)gnu_host_errno(host_error)); }

/* ---- the guest's printf ---- */

static void put(char* out, size_t size, size_t* n, const char* text, size_t length) {
    if (*n < size) {
        size_t room = size - *n - 1;
        memcpy(out + *n, text, length < room ? length : room);
        out[*n + (length < room ? length : room)] = 0;
    }
    *n += length;
}

int arm32_format(char* out, size_t size, const char* f, Arm32Args* a) {
    size_t n = 0;
    if (size) out[0] = 0;
    while (*f) {
        if (*f != '%') {
            const char* next = strchr(f, '%');
            size_t length = next ? (size_t)(next - f) : strlen(f);
            put(out, size, &n, f, length);
            f += length;
            continue;
        }
        const char* start = f++;
        char spec[64];
        size_t at = 0;
        spec[at++] = '%';
        while (*f && strchr("-+ #0'", *f) && at < 20) spec[at++] = *f++;
        if (*f == '*') {
            at += (size_t)snprintf(spec + at, sizeof spec - at, "%d", (int32_t)arm32_arg(a));
            ++f;
        } else {
            while (isdigit((unsigned char)*f) && at < 40) spec[at++] = *f++;
        }
        if (*f == '.') {
            spec[at++] = *f++;
            if (*f == '*') {
                at += (size_t)snprintf(spec + at, sizeof spec - at, "%d", (int32_t)arm32_arg(a));
                ++f;
            } else {
                while (isdigit((unsigned char)*f) && at < 50) spec[at++] = *f++;
            }
        }
        /* the length: 64 bits for ll, j and q; 32 for l, z, t (ILP32) and none */
        int length = 0; /* 0 int, 1 char, 2 short, 8 a 64-bit one */
        if (*f == 'h') {
            length = f[1] == 'h' ? 1 : 2;
            f += f[1] == 'h' ? 2 : 1;
        } else if (*f == 'l') {
            if (f[1] == 'l') length = 8, ++f;
            ++f;
        } else if (*f == 'j' || *f == 'q') {
            length = 8;
            ++f;
        } else if (*f == 'z' || *f == 't' || *f == 'L') {
            ++f;
        }
        char conv = *f ? *f++ : 0;
        char text[512];
        int written = 0;
        switch (conv) {
        case 'd':
        case 'i':
            if (length == 8) {
                memcpy(spec + at, "lld", 4);
                written = snprintf(text, sizeof text, spec, (long long)(int64_t)arm32_arg64(a));
            } else {
                int32_t v = (int32_t)arm32_arg(a);
                if (length == 1) v = (int8_t)v;
                if (length == 2) v = (int16_t)v;
                spec[at] = 'd', spec[at + 1] = 0;
                written = snprintf(text, sizeof text, spec, (int)v);
            }
            break;
        case 'u':
        case 'o':
        case 'x':
        case 'X':
            if (length == 8) {
                spec[at] = 'l', spec[at + 1] = 'l', spec[at + 2] = conv, spec[at + 3] = 0;
                written = snprintf(text, sizeof text, spec, (unsigned long long)arm32_arg64(a));
            } else {
                uint32_t v = arm32_arg(a);
                if (length == 1) v = (uint8_t)v;
                if (length == 2) v = (uint16_t)v;
                spec[at] = conv, spec[at + 1] = 0;
                written = snprintf(text, sizeof text, spec, (unsigned)v);
            }
            break;
        case 'c':
            spec[at] = 'c', spec[at + 1] = 0;
            written = snprintf(text, sizeof text, spec, (int)arm32_arg(a));
            break;
        case 'p':
            written = snprintf(text, sizeof text, "0x%x", (unsigned)arm32_arg(a));
            break;
        case 'e':
        case 'E':
        case 'f':
        case 'F':
        case 'g':
        case 'G':
        case 'a':
        case 'A': {
            uint64_t bits = arm32_arg64(a);
            double d;
            memcpy(&d, &bits, sizeof d);
            spec[at] = conv, spec[at + 1] = 0;
            written = snprintf(text, sizeof text, spec, d);
            break;
        }
        case 's': {
            uint32_t p = arm32_arg(a);
            const char* str = p ? (const char*)arm32_host(p) : "(null)";
            spec[at] = 's', spec[at + 1] = 0;
            int need = snprintf(NULL, 0, spec, str);
            if (need >= (int)sizeof text) {
                char* big = malloc((size_t)need + 1);
                if (big) {
                    snprintf(big, (size_t)need + 1, spec, str);
                    put(out, size, &n, big, (size_t)need);
                    free(big);
                }
                continue;
            }
            written = snprintf(text, sizeof text, spec, str);
            break;
        }
        case 'n': {
            uint32_t p = arm32_arg(a);
            if (p) wr32(p, (uint32_t)n);
            continue;
        }
        case '%': written = snprintf(text, sizeof text, "%%"); break;
        default: put(out, size, &n, start, (size_t)(f - start)); continue;
        }
        if (written > 0) put(out, size, &n, text, (size_t)written < sizeof text ? (size_t)written : sizeof text - 1);
    }
    return (int)n;
}

/* a guest format into a host string of its own (free it) */
static char* format_alloc(const char* format, Arm32Args* a) {
    Arm32Args again = *a;
    int n = arm32_format(NULL, 0, format, a);
    char* text = malloc((size_t)n + 1);
    if (text) arm32_format(text, (size_t)n + 1, format, &again);
    return text;
}

/* ---- the guest's sscanf ---- */

static int scan(const char* input, const char* f, Arm32Args* a) {
    const char* in = input;
    int count = 0;
    while (*f) {
        if (isspace((unsigned char)*f)) {
            while (isspace((unsigned char)*f)) ++f;
            while (isspace((unsigned char)*in)) ++in;
            continue;
        }
        if (*f != '%' || f[1] == '%') {
            if (*f == '%') {
                ++f;
                while (isspace((unsigned char)*in)) ++in;
            }
            if (*in != *f) break;
            ++in, ++f;
            continue;
        }
        ++f;
        bool suppress = *f == '*';
        if (suppress) ++f;
        char width[16];
        size_t w = 0;
        while (isdigit((unsigned char)*f) && w < sizeof width - 1) width[w++] = *f++;
        width[w] = 0;
        int length = 0; /* 1 char, 2 short, 4 int (and l), 8 long long, 'd' a double */
        if (*f == 'h') {
            length = f[1] == 'h' ? 1 : 2;
            f += f[1] == 'h' ? 2 : 1;
        } else if (*f == 'l') {
            length = f[1] == 'l' ? 8 : 'l';
            f += f[1] == 'l' ? 2 : 1;
        } else if (*f == 'L' || *f == 'q' || *f == 'j') {
            length = *f == 'L' ? 'l' : 8;
            ++f;
        } else if (*f == 'z' || *f == 't') {
            ++f;
        }
        char conv = *f;
        if (!conv) break;
        char set[256] = "";
        if (conv == '[') {
            const char* end = f + 1;
            if (*end == '^') ++end;
            if (*end == ']') ++end;
            while (*end && *end != ']') ++end;
            size_t len = (size_t)(end - f) + 1;
            if (!*end || len >= sizeof set) break;
            memcpy(set, f, len);
            set[len] = 0;
            f = end + 1;
        } else {
            set[0] = conv, set[1] = 0;
            ++f;
        }
        if (conv == 'n') {
            uint32_t p = suppress ? 0 : arm32_arg(a);
            if (p) wr32(p, (uint32_t)(in - input));
            continue;
        }
        if (!*in) return count ? count : -1;
        char fmt[300];
        int consumed = -1, r;
        if (conv == 's' || conv == 'c' || conv == '[') {
            snprintf(fmt, sizeof fmt, "%%%s%s%s%%n", suppress ? "*" : "", width, set);
            if (suppress) r = sscanf(in, fmt, &consumed) == 0 && consumed >= 0;
            else r = sscanf(in, fmt, (char*)arm32_host(arm32_arg(a)), &consumed);
            if (r != 1) break;
            in += consumed;
            if (!suppress) ++count;
            continue;
        }
        bool floating = strchr("eEfFgGaA", conv) != NULL;
        if (conv == 'p') conv = 'x', length = 0;
        if (!floating && !strchr("diuoxX", conv)) break;
        if (floating) {
            double d = 0;
            snprintf(fmt, sizeof fmt, "%%%s%slf%%n", suppress ? "*" : "", width);
            r = suppress ? (sscanf(in, fmt, &consumed) == 0 && consumed >= 0) : sscanf(in, fmt, &d, &consumed);
            if (r != 1) break;
            in += consumed;
            if (suppress) continue;
            uint32_t p = arm32_arg(a);
            if (length == 'l') {
                memcpy(arm32_mem + p, &d, 8);
            } else {
                float v = (float)d;
                memcpy(arm32_mem + p, &v, 4);
            }
            ++count;
            continue;
        }
        long long v = 0;
        snprintf(fmt, sizeof fmt, "%%%s%sll%c%%n", suppress ? "*" : "", width, conv);
        r = suppress ? (sscanf(in, fmt, &consumed) == 0 && consumed >= 0) : sscanf(in, fmt, &v, &consumed);
        if (r != 1) break;
        in += consumed;
        if (suppress) continue;
        uint32_t p = arm32_arg(a);
        if (length == 1) arm32_mem[p] = (uint8_t)v;
        else if (length == 2) snx_st_le16(arm32_mem + p, (uint16_t)v);
        else if (length == 8) memcpy(arm32_mem + p, &v, 8);
        else wr32(p, (uint32_t)v);
        ++count;
    }
    return count;
}

/* ---- stdio ---- */

static uint32_t sF; /* bionic's __sF: stdin, stdout and stderr, 84 bytes each */
enum { FILE_SIZE = 84, MAX_FILES = 256 };
static struct {
    pthread_mutex_t lock;
    uint32_t guest[MAX_FILES];
    FILE* host[MAX_FILES];
} files = {.lock = PTHREAD_MUTEX_INITIALIZER};

static FILE* host_file(uint32_t f) {
    if (f == sF) return stdin;
    if (f == sF + FILE_SIZE) return stdout;
    if (f == sF + 2 * FILE_SIZE) return stderr;
    FILE* found = NULL;
    pthread_mutex_lock(&files.lock);
    for (unsigned i = 0; i < MAX_FILES; ++i)
        if (files.guest[i] == f) {
            found = files.host[i];
            break;
        }
    pthread_mutex_unlock(&files.lock);
    return found;
}

HANDLER(f_fopen) {
    UNUSED;
    FILE* file = fopen(S(0), S(1));
    if (!file) {
        set_errno(errno);
        R(0) = 0;
        return;
    }
    uint32_t g = arm32_malloc(FILE_SIZE);
    pthread_mutex_lock(&files.lock);
    unsigned i = 0;
    while (i < MAX_FILES && files.guest[i]) ++i;
    if (i < MAX_FILES && g) {
        files.guest[i] = g;
        files.host[i] = file;
    }
    pthread_mutex_unlock(&files.lock);
    if (i == MAX_FILES || !g) {
        fclose(file);
        arm32_free(g);
        set_errno(EMFILE);
        g = 0;
    }
    R(0) = g;
}
HANDLER(f_fclose) {
    UNUSED;
    uint32_t g = R(0);
    FILE* file = NULL;
    pthread_mutex_lock(&files.lock);
    for (unsigned i = 0; i < MAX_FILES; ++i)
        if (files.guest[i] == g) {
            file = files.host[i];
            files.guest[i] = 0;
            files.host[i] = NULL;
            break;
        }
    pthread_mutex_unlock(&files.lock);
    R(0) = file ? (uint32_t)fclose(file) : (uint32_t)-1;
    if (file) arm32_free(g);
}
HANDLER(f_fread) {
    UNUSED;
    FILE* file = host_file(R(3));
    R(0) = file ? (uint32_t)fread(P(0), R(1), R(2), file) : 0;
}
HANDLER(f_fwrite) {
    UNUSED;
    FILE* file = host_file(R(3));
    R(0) = file ? (uint32_t)fwrite(P(0), R(1), R(2), file) : 0;
}
HANDLER(f_fgets) {
    UNUSED;
    FILE* file = host_file(R(2));
    if (!file || !fgets(P(0), (int)R(1), file)) R(0) = 0;
}
HANDLER(f_fseek) {
    UNUSED;
    FILE* file = host_file(R(0));
    R(0) = file ? (uint32_t)fseek(file, (long)(int32_t)R(1), (int)R(2)) : (uint32_t)-1;
}
HANDLER(f_ftell) {
    UNUSED;
    FILE* file = host_file(R(0));
    R(0) = file ? (uint32_t)ftell(file) : (uint32_t)-1;
}
HANDLER(f_fprintf) {
    UNUSED;
    Arm32Args a;
    arm32_args(&a, c, 2);
    char* text = format_alloc(S(1), &a);
    FILE* file = host_file(R(0));
    R(0) = text && file ? (uint32_t)fputs(text, file) : (uint32_t)-1;
    if (text && file) R(0) = (uint32_t)strlen(text);
    free(text);
}
HANDLER(f_printf) {
    UNUSED;
    Arm32Args a;
    arm32_args(&a, c, 1);
    char* text = format_alloc(S(0), &a);
    R(0) = text ? (uint32_t)strlen(text) : 0;
    if (text) fputs(text, stdout);
    free(text);
}
HANDLER(f_puts) {
    UNUSED;
    R(0) = (uint32_t)puts(S(0));
}
HANDLER(f_sprintf) {
    UNUSED;
    Arm32Args a;
    arm32_args(&a, c, 2);
    char* text = format_alloc(S(1), &a);
    size_t n = text ? strlen(text) : 0;
    if (text) memcpy(P(0), text, n + 1);
    R(0) = (uint32_t)n;
    free(text);
}
HANDLER(f_snprintf) {
    UNUSED;
    Arm32Args a;
    arm32_args(&a, c, 3);
    R(0) = (uint32_t)arm32_format(R(1) ? P(0) : NULL, R(1), S(2), &a);
}
HANDLER(f_vsnprintf) {
    UNUSED;
    Arm32Args a;
    arm32_args_va(&a, R(3));
    R(0) = (uint32_t)arm32_format(R(1) ? P(0) : NULL, R(1), S(2), &a);
}
HANDLER(f_sscanf) {
    UNUSED;
    Arm32Args a;
    arm32_args(&a, c, 2);
    R(0) = (uint32_t)scan(S(0), S(1), &a);
}

HANDLER(f_stat) {
    UNUSED;
    struct stat st;
    if (stat(S(0), &st)) {
        set_errno(errno);
        R(0) = (uint32_t)-1;
        return;
    }
    /* bionic's struct stat on 32-bit ARM (the kernel's stat64), 104 bytes */
    uint8_t* o = P(1);
    memset(o, 0, 104);
    uint64_t v = (uint64_t)st.st_dev;
    memcpy(o, &v, 8);
    snx_st_le32(o + 12, (uint32_t)st.st_ino);
    snx_st_le32(o + 16, (uint32_t)st.st_mode);
    snx_st_le32(o + 20, (uint32_t)st.st_nlink);
    snx_st_le32(o + 24, (uint32_t)st.st_uid);
    snx_st_le32(o + 28, (uint32_t)st.st_gid);
    v = (uint64_t)st.st_rdev;
    memcpy(o + 32, &v, 8);
    v = (uint64_t)st.st_size;
    memcpy(o + 48, &v, 8);
    snx_st_le32(o + 56, (uint32_t)st.st_blksize);
    v = (uint64_t)st.st_blocks;
    memcpy(o + 64, &v, 8);
    snx_st_le32(o + 72, (uint32_t)st.st_atime);
    snx_st_le32(o + 80, (uint32_t)st.st_mtime);
    snx_st_le32(o + 88, (uint32_t)st.st_ctime);
    v = (uint64_t)st.st_ino;
    memcpy(o + 96, &v, 8);
    R(0) = 0;
}

/* ---- memory ---- */

HANDLER(f_malloc) {
    UNUSED;
    R(0) = arm32_malloc(R(0));
}
HANDLER(f_calloc) {
    UNUSED;
    uint64_t n = (uint64_t)R(0) * R(1);
    uint32_t p = n >> 32 ? 0 : arm32_malloc((uint32_t)n);
    if (p) memset(arm32_mem + p, 0, (size_t)n);
    R(0) = p;
}
HANDLER(f_realloc) {
    UNUSED;
    R(0) = arm32_realloc(R(0), R(1));
}
HANDLER(f_free) {
    UNUSED;
    arm32_free(R(0));
}
HANDLER(f_memalign) {
    UNUSED;
    R(0) = arm32_memalign(R(0), R(1));
}

/* mmap(addr, length, prot, flags, fd, offset): anonymous memory from the window, a file's read into it (its pages
   mapped over it) */
HANDLER(f_mmap) {
    UNUSED;
    uint32_t length = R(1), flags = R(3);
    int fd = (int32_t)word(c, 4);
    uint32_t offset = word(c, 5);
    uint32_t a = arm32_map(length);
    if (!a) {
        set_errno(ENOMEM);
        R(0) = UINT32_MAX;
        return;
    }
#if !defined(_WIN32)
    if (!(flags & 0x20) /* MAP_ANONYMOUS */) {
        void* m = mmap(arm32_mem + a, length, PROT_READ | PROT_WRITE,
                       MAP_FIXED | ((flags & 1) ? MAP_SHARED : MAP_PRIVATE), fd, (off_t)offset);
        if (m == MAP_FAILED) {
            set_errno(errno);
            arm32_unmap(a, length);
            R(0) = UINT32_MAX;
            return;
        }
    }
#endif
    R(0) = a;
}
HANDLER(f_munmap) {
    UNUSED;
#if !defined(_WIN32)
    /* a file's pages made anonymous again */
    mmap(arm32_mem + R(0), R(1), PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
#endif
    arm32_unmap(R(0), R(1));
    R(0) = 0;
}

HANDLER(f_memcpy) {
    UNUSED;
    memmove(P(0), P(1), R(2));
}
HANDLER(f_memmove) {
    UNUSED;
    memmove(P(0), P(1), R(2));
}
HANDLER(f_memset) {
    UNUSED;
    memset(P(0), (int)R(1), R(2));
}
/* __aeabi_memset(dest, n, c) and __aeabi_memclr(dest, n) */
HANDLER(f_aeabi_memset) {
    UNUSED;
    memset(P(0), (int)R(2), R(1));
}
HANDLER(f_aeabi_memclr) {
    UNUSED;
    memset(P(0), 0, R(1));
}
HANDLER(f_memcmp) {
    UNUSED;
    R(0) = (uint32_t)memcmp(P(0), P(1), R(2));
}

/* ---- strings ---- */

/* a host pointer inside the guest's string at guest address base, as the guest's */
static uint32_t inside(const char* p) { return p ? arm32_guest(p) : 0; }
HANDLER(f_strlen) {
    UNUSED;
    R(0) = (uint32_t)strlen(S(0));
}
HANDLER(f_strcmp) {
    UNUSED;
    R(0) = (uint32_t)strcmp(S(0), S(1));
}
HANDLER(f_strncmp) {
    UNUSED;
    R(0) = (uint32_t)strncmp(S(0), S(1), R(2));
}
HANDLER(f_strcasecmp) {
    UNUSED;
    R(0) = (uint32_t)strcasecmp(S(0), S(1));
}
HANDLER(f_strncasecmp) {
    UNUSED;
    R(0) = (uint32_t)strncasecmp(S(0), S(1), R(2));
}
HANDLER(f_strcpy) {
    UNUSED;
    memmove(P(0), S(1), strlen(S(1)) + 1);
}
HANDLER(f_strncpy) {
    UNUSED;
    strncpy(P(0), S(1), R(2));
}
HANDLER(f_strcat) {
    UNUSED;
    strcat(P(0), S(1));
}
HANDLER(f_strncat) {
    UNUSED;
    strncat(P(0), S(1), R(2));
}
HANDLER(f_strchr) {
    UNUSED;
    R(0) = inside(strchr(S(0), (int)R(1)));
}
HANDLER(f_strrchr) {
    UNUSED;
    R(0) = inside(strrchr(S(0), (int)R(1)));
}
HANDLER(f_strstr) {
    UNUSED;
    R(0) = inside(strstr(S(0), S(1)));
}
HANDLER(f_strspn) {
    UNUSED;
    R(0) = (uint32_t)strspn(S(0), S(1));
}
HANDLER(f_strcspn) {
    UNUSED;
    R(0) = (uint32_t)strcspn(S(0), S(1));
}
HANDLER(f_strdup) {
    UNUSED;
    R(0) = arm32_strdup(S(0));
}
HANDLER(f_memchr) {
    UNUSED;
    R(0) = inside(memchr(P(0), (int)R(1), R(2)));
}
HANDLER(f_toupper) {
    UNUSED;
    R(0) = (uint32_t)toupper((int)R(0));
}
HANDLER(f_tolower) {
    UNUSED;
    R(0) = (uint32_t)tolower((int)R(0));
}
HANDLER(f_atoi) {
    UNUSED;
    R(0) = (uint32_t)atoi(S(0));
}
HANDLER(f_atoll) {
    UNUSED;
    set64(c, (uint64_t)atoll(S(0)));
}
HANDLER(f_strtol) {
    UNUSED;
    char* end;
    long long v = strtoll(S(0), &end, (int)R(2));
    if (v > INT32_MAX) v = INT32_MAX;
    if (v < INT32_MIN) v = INT32_MIN;
    if (R(1)) wr32(R(1), inside(end));
    R(0) = (uint32_t)(int32_t)v;
}
HANDLER(f_strtod) {
    UNUSED;
    char* end;
    double d = strtod(S(0), &end);
    if (R(1)) wr32(R(1), inside(end));
    ret_double(c, d);
}
HANDLER(f_atof) {
    UNUSED;
    ret_double(c, atof(S(0)));
}

/* ---- libm (soft-float: a double in two words, a float in one) ---- */

#define MATH1(name) \
    HANDLER(m_##name) { \
        UNUSED; \
        ret_double(c, name(arg_double(c, 0))); \
    } \
    HANDLER(m_##name##f) { \
        UNUSED; \
        ret_float(c, name##f(arg_float(c, 0))); \
    }
MATH1(sin)
MATH1(cos)
MATH1(tan)
MATH1(asin)
MATH1(acos)
MATH1(atan)
MATH1(sqrt)
MATH1(floor)
MATH1(ceil)
MATH1(round)
MATH1(exp)
MATH1(log)
MATH1(log10)
MATH1(fabs)
#define MATH2(name) \
    HANDLER(m_##name) { \
        UNUSED; \
        ret_double(c, name(arg_double(c, 0), arg_double(c, 2))); \
    } \
    HANDLER(m_##name##f) { \
        UNUSED; \
        ret_float(c, name##f(arg_float(c, 0), arg_float(c, 1))); \
    }
MATH2(pow)
MATH2(fmod)
MATH2(atan2)

/* ---- the rest of bionic ---- */

/* bionic's lrand48 and srand48: the 48-bit generator that POSIX gives, the same sequence on every host */
static uint64_t rand48 = 0x1234abcd330eULL;
static pthread_mutex_t rand48_lock = PTHREAD_MUTEX_INITIALIZER;
HANDLER(f_srand48) {
    UNUSED;
    pthread_mutex_lock(&rand48_lock);
    rand48 = ((uint64_t)R(0) << 16 | 0x330e) & 0xffffffffffffULL;
    pthread_mutex_unlock(&rand48_lock);
}
HANDLER(f_lrand48) {
    UNUSED;
    pthread_mutex_lock(&rand48_lock);
    rand48 = (rand48 * 0x5deece66dULL + 0xb) & 0xffffffffffffULL;
    R(0) = (uint32_t)(rand48 >> 17);
    pthread_mutex_unlock(&rand48_lock);
}
static uint32_t rand_state = 1;
HANDLER(f_srand) {
    UNUSED;
    rand_state = R(0);
}
HANDLER(f_rand) {
    UNUSED;
    rand_state = rand_state * 1103515245u + 12345u;
    R(0) = (rand_state >> 1) & 0x7fffffff;
}

HANDLER(f_time) {
    UNUSED;
    uint32_t t = (uint32_t)time(NULL);
    if (R(0)) wr32(R(0), t);
    R(0) = t;
}
/* bionic's struct tm on 32-bit: 9 ints, tm_gmtoff and tm_zone, 44 bytes, then the zone's name */
static uint32_t tm_buffer;
static void put_tm(uint32_t at, const struct tm* tm) {
    int32_t v[9] = {tm->tm_sec, tm->tm_min, tm->tm_hour, tm->tm_mday, tm->tm_mon,
                    tm->tm_year, tm->tm_wday, tm->tm_yday, tm->tm_isdst};
    for (unsigned i = 0; i < 9; ++i) wr32(at + 4 * i, (uint32_t)v[i]);
    wr32(at + 36, (uint32_t)tm->tm_gmtoff);
    snprintf((char*)arm32_mem + at + 44, 16, "%s", tm->tm_zone ? tm->tm_zone : "UTC");
    wr32(at + 40, at + 44);
}
HANDLER(f_localtime) {
    UNUSED;
    time_t t = (time_t)(int32_t)rd32(R(0));
    struct tm tm;
    if (!localtime_r(&t, &tm)) {
        R(0) = 0;
        return;
    }
    put_tm(tm_buffer, &tm);
    R(0) = tm_buffer;
}
HANDLER(f_gmtime) {
    UNUSED;
    time_t t = (time_t)(int32_t)rd32(R(0));
    struct tm tm;
    if (!gmtime_r(&t, &tm)) {
        R(0) = 0;
        return;
    }
    put_tm(tm_buffer, &tm);
    R(0) = tm_buffer;
}
HANDLER(f_gettimeofday) {
    UNUSED;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    if (R(0)) {
        wr32(R(0), (uint32_t)ts.tv_sec);
        wr32(R(0) + 4, (uint32_t)(ts.tv_nsec / 1000));
    }
    R(0) = 0;
}
HANDLER(f_clock_gettime) {
    UNUSED;
    struct timespec ts;
    clockid_t id = R(0) == 1 ? CLOCK_MONOTONIC : CLOCK_REALTIME;
    clock_gettime(id, &ts);
    wr32(R(1), (uint32_t)ts.tv_sec);
    wr32(R(1) + 4, (uint32_t)ts.tv_nsec);
    R(0) = 0;
}
HANDLER(f_usleep) {
    UNUSED;
    usleep(R(0));
    R(0) = 0;
}

HANDLER(f_errno) {
    UNUSED;
    R(0) = arm32_errno_address();
}
HANDLER(f_exit) {
    UNUSED;
    fflush(stdout);
    exit((int)R(0));
}
HANDLER(f_abort) {
    UNUSED;
    fprintf(stderr, "scarletnx-android: arm32: abort() called from 0x%08x\n", c->r[14]);
    abort();
}
HANDLER(f_raise) {
    UNUSED;
    fprintf(stderr, "scarletnx-android: arm32: raise(%u) called from 0x%08x\n", R(0), c->r[14]);
    if (R(0) == 6 || R(0) == 11 || R(0) == 4 || R(0) == 7 || R(0) == 8) abort();
    R(0) = 0;
}
HANDLER(f_assert2) {
    UNUSED;
    fprintf(stderr, "scarletnx-android: arm32: assertion \"%s\" failed: file \"%s\", line %d, function \"%s\"\n",
            S(3), S(0), (int)R(1), S(2));
    abort();
}
HANDLER(f_stack_chk_fail) {
    UNUSED;
    fprintf(stderr, "scarletnx-android: arm32: stack corruption detected, called from 0x%08x\n", c->r[14]);
    abort();
}
HANDLER(f_zero) {
    UNUSED;
    R(0) = 0;
}
HANDLER(f_nothing) { (void)c, UNUSED; }
HANDLER(f_find_exidx) {
    UNUSED;
    int count = 0;
    uint32_t e = arm32_exidx(R(0), &count);
    if (R(1)) wr32(R(1), (uint32_t)count);
    R(0) = e;
}
HANDLER(f_getenv) {
    UNUSED;
    R(0) = 0;
}

/* ---- liblog ---- */

static const char* priority(uint32_t p) {
    static const char* const letters[] = {"?", "?", "V", "D", "I", "W", "E", "F", "S"};
    return p < 9 ? letters[p] : "?";
}
HANDLER(log_print) {
    UNUSED;
    Arm32Args a;
    arm32_args(&a, c, 3);
    char* text = format_alloc(S(2), &a);
    fprintf(stderr, "%s/%s: %s\n", priority(R(0)), R(1) ? S(1) : "", text ? text : "");
    free(text);
    R(0) = 1;
}
HANDLER(log_vprint) {
    UNUSED;
    Arm32Args a;
    arm32_args_va(&a, R(3));
    char* text = format_alloc(S(2), &a);
    fprintf(stderr, "%s/%s: %s\n", priority(R(0)), R(1) ? S(1) : "", text ? text : "");
    free(text);
    R(0) = 1;
}
HANDLER(log_write) {
    UNUSED;
    fprintf(stderr, "%s/%s: %s\n", priority(R(0)), R(1) ? S(1) : "", R(2) ? S(2) : "");
    R(0) = 1;
}

/* ---- libdl: a handle is a library's address + 1, or 1 for the system's ---- */

static _Thread_local char dl_error[512];
static _Thread_local bool dl_error_set;
static uint32_t dl_error_buffer;
HANDLER(dl_open) {
    UNUSED;
    if (!R(0)) {
        R(0) = 1;
        return;
    }
    const char* name = S(0);
    const char* slash = strrchr(name, '/');
    const char* base = slash ? slash + 1 : name;
    if (arm32_hle_is_system_library(base)) {
        R(0) = 1;
        return;
    }
    char error[400];
    Arm32Library* l = arm32_load(name, error, sizeof error);
    if (!l) {
        snprintf(dl_error, sizeof dl_error, "dlopen failed: %s", error);
        dl_error_set = true;
        R(0) = 0;
        return;
    }
    R(0) = 0x10000000u | (uint32_t)(uintptr_t)l % 0x0fffffffu; /* never dereferenced: dlsym searches every library */
}
HANDLER(dl_sym) {
    UNUSED;
    const char* name = S(1);
    uint32_t a = arm32_symbol(NULL, name);
    if (!a) a = arm32_hle_symbol(name);
    if (!a) {
        snprintf(dl_error, sizeof dl_error, "undefined symbol: %s", name);
        dl_error_set = true;
    }
    R(0) = a;
}
HANDLER(dl_error_) {
    UNUSED;
    if (!dl_error_set) {
        R(0) = 0;
        return;
    }
    dl_error_set = false;
    snprintf((char*)arm32_mem + dl_error_buffer, sizeof dl_error, "%s", dl_error);
    R(0) = dl_error_buffer;
}

/* ---- pthread: a guest mutex or condition variable is a host one, found by its address ---- */

typedef struct SyncObject {
    uint32_t address;
    bool cond;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    struct SyncObject* next;
} SyncObject;
enum { SYNC_BUCKETS = 1024 };
static SyncObject* sync_table[SYNC_BUCKETS];
static pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER;
static SyncObject* sync_object(uint32_t address, bool cond) {
    unsigned b = (address >> 2) % SYNC_BUCKETS;
    pthread_mutex_lock(&sync_lock);
    SyncObject* o = sync_table[b];
    while (o && (o->address != address || o->cond != cond)) o = o->next;
    if (!o) {
        o = calloc(1, sizeof *o);
        if (!o) abort();
        o->address = address;
        o->cond = cond;
        if (cond) {
            pthread_cond_init(&o->condition, NULL);
        } else {
            /* recursive: bionic's recursive mutex is a value in the guest's word, and a normal one that a thread
               locks twice deadlocks, which no working app does */
            pthread_mutexattr_t attr;
            pthread_mutexattr_init(&attr);
            pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&o->mutex, &attr);
            pthread_mutexattr_destroy(&attr);
        }
        o->next = sync_table[b];
        sync_table[b] = o;
    }
    pthread_mutex_unlock(&sync_lock);
    return o;
}
HANDLER(p_mutex_init) {
    UNUSED;
    sync_object(R(0), false);
    R(0) = 0;
}
HANDLER(p_mutex_lock) {
    UNUSED;
    R(0) = (uint32_t)pthread_mutex_lock(&sync_object(R(0), false)->mutex);
}
HANDLER(p_mutex_trylock) {
    UNUSED;
    R(0) = pthread_mutex_trylock(&sync_object(R(0), false)->mutex) ? 16 /* EBUSY */ : 0;
}
HANDLER(p_mutex_unlock) {
    UNUSED;
    R(0) = (uint32_t)pthread_mutex_unlock(&sync_object(R(0), false)->mutex);
}
HANDLER(p_cond_wait) {
    UNUSED;
    SyncObject* m = sync_object(R(1), false);
    R(0) = (uint32_t)pthread_cond_wait(&sync_object(R(0), true)->condition, &m->mutex);
}
HANDLER(p_cond_broadcast) {
    UNUSED;
    R(0) = (uint32_t)pthread_cond_broadcast(&sync_object(R(0), true)->condition);
}
HANDLER(p_cond_signal) {
    UNUSED;
    R(0) = (uint32_t)pthread_cond_signal(&sync_object(R(0), true)->condition);
}
/* keys: 128 of them, each thread's values in a host array */
enum { KEYS = 128 };
static uint32_t key_count;
static _Thread_local uint32_t key_values[KEYS];
HANDLER(p_key_create) {
    UNUSED;
    uint32_t k = __atomic_fetch_add(&key_count, 1, __ATOMIC_RELAXED);
    if (k >= KEYS) {
        R(0) = 11; /* EAGAIN */
        return;
    }
    wr32(R(0), k);
    R(0) = 0;
}
HANDLER(p_getspecific) {
    UNUSED;
    R(0) = R(0) < KEYS ? key_values[R(0)] : 0;
}
HANDLER(p_setspecific) {
    UNUSED;
    if (R(0) < KEYS) key_values[R(0)] = R(1);
    R(0) = R(0) < KEYS ? 0 : 22;
}
HANDLER(p_self) {
    UNUSED;
    arm32_cpu();
    R(0) = c->tpidruro;
}

/* ---- GLES 1.1: hle_gles.c's handlers, with an AArch64 CPU of the call's arguments ---- */

typedef struct {
    const char* name;
    const char* params; /* i an integer, p a pointer, f a float */
    BridgeHandler handler;
    void* data;
} GlesCall;
static GlesCall gles_calls[] = {
    {"glActiveTexture", "i", NULL, NULL},    {"glAlphaFunc", "if", NULL, NULL},
    {"glBindTexture", "ii", NULL, NULL},     {"glBlendFunc", "ii", NULL, NULL},
    {"glClear", "i", NULL, NULL},            {"glClearColor", "ffff", NULL, NULL},
    {"glClientActiveTexture", "i", NULL, NULL}, {"glColor4f", "ffff", NULL, NULL},
    {"glColor4ub", "iiii", NULL, NULL},      {"glColorMask", "iiii", NULL, NULL},
    {"glColorPointer", "iiip", NULL, NULL},  {"glCullFace", "i", NULL, NULL},
    {"glDeleteTextures", "ip", NULL, NULL},  {"glDepthFunc", "i", NULL, NULL},
    {"glDepthMask", "i", NULL, NULL},        {"glDisable", "i", NULL, NULL},
    {"glDisableClientState", "i", NULL, NULL}, {"glDrawArrays", "iii", NULL, NULL},
    {"glDrawElements", "iiip", NULL, NULL},  {"glDrawTexiOES", "iiiii", NULL, NULL},
    {"glEnable", "i", NULL, NULL},           {"glEnableClientState", "i", NULL, NULL},
    {"glFinish", "", NULL, NULL},            {"glFlush", "", NULL, NULL},
    {"glFrontFace", "i", NULL, NULL},        {"glGenTextures", "ip", NULL, NULL},
    {"glGetError", "", NULL, NULL},          {"glGetIntegerv", "ip", NULL, NULL},
    {"glHint", "ii", NULL, NULL},            {"glLineWidth", "f", NULL, NULL},
    {"glLoadIdentity", "", NULL, NULL},      {"glLoadMatrixf", "p", NULL, NULL},
    {"glMatrixMode", "i", NULL, NULL},       {"glMultMatrixf", "p", NULL, NULL},
    {"glOrthof", "ffffff", NULL, NULL},      {"glPixelStorei", "ii", NULL, NULL},
    {"glPopMatrix", "", NULL, NULL},         {"glPushMatrix", "", NULL, NULL},
    {"glReadPixels", "iiiiiip", NULL, NULL}, {"glRotatef", "ffff", NULL, NULL},
    {"glScalef", "fff", NULL, NULL},         {"glScissor", "iiii", NULL, NULL},
    {"glShadeModel", "i", NULL, NULL},       {"glTexCoordPointer", "iiip", NULL, NULL},
    {"glTexEnvf", "iif", NULL, NULL},        {"glTexEnvi", "iii", NULL, NULL},
    {"glTexImage2D", "iiiiiiiip", NULL, NULL}, {"glTexParameterf", "iif", NULL, NULL},
    {"glTexParameteri", "iii", NULL, NULL},  {"glTexSubImage2D", "iiiiiiiip", NULL, NULL},
    {"glTranslatef", "fff", NULL, NULL},     {"glVertexPointer", "iiip", NULL, NULL},
    {"glViewport", "iiii", NULL, NULL},
};
static _Thread_local CPU* gles_cpu;
HANDLER(gles_call) {
    const GlesCall* g = data;
    if (!g->handler) return; /* a state that has no effect here (glLineWidth: lines are 1 pixel wide) */
    if (!gles_cpu) {
        gles_cpu = calloc(1, sizeof *gles_cpu);
        if (!gles_cpu) abort();
    }
    CPU* x = gles_cpu;
    /* AAPCS64: integers in x0 to x7 and floats in v0 to v7, then each in an 8-byte slot of the stack */
    uint64_t stack[8] = {0};
    unsigned nx = 0, nv = 0, ns = 0;
    x->x[31] = (uint64_t)(uintptr_t)stack;
    for (unsigned i = 0; g->params[i] && ns < 8; ++i) {
        uint32_t w = word(c, i);
        uint64_t v = g->params[i] == 'p' ? (w ? (uint64_t)(uintptr_t)(arm32_mem + w) : 0)
                     : g->params[i] == 'f' ? w
                                           : (uint64_t)(int64_t)(int32_t)w;
        if (g->params[i] == 'f' && nv < 8) {
            memset(&x->v[nv], 0, sizeof x->v[nv]);
            x->v[nv++].s[0] = w;
        } else if (g->params[i] != 'f' && nx < 8) {
            x->x[nx++] = v;
        } else {
            stack[ns++] = v;
        }
    }
    g->handler(x, g->data);
    R(0) = (uint32_t)x->x[0];
}

/* ---- the tables ---- */

typedef struct {
    const char* name;
    Arm32Handler handler;
    void* data;
} Arm32Symbol;
static const Arm32Symbol symbols[] = {
    {"fopen", f_fopen, NULL},
    {"fclose", f_fclose, NULL},
    {"fread", f_fread, NULL},
    {"fwrite", f_fwrite, NULL},
    {"fgets", f_fgets, NULL},
    {"fseek", f_fseek, NULL},
    {"ftell", f_ftell, NULL},
    {"fprintf", f_fprintf, NULL},
    {"printf", f_printf, NULL},
    {"puts", f_puts, NULL},
    {"sprintf", f_sprintf, NULL},
    {"snprintf", f_snprintf, NULL},
    {"vsnprintf", f_vsnprintf, NULL},
    {"sscanf", f_sscanf, NULL},
    {"stat", f_stat, NULL},
    {"malloc", f_malloc, NULL},
    {"calloc", f_calloc, NULL},
    {"realloc", f_realloc, NULL},
    {"free", f_free, NULL},
    {"memalign", f_memalign, NULL},
    {"mmap", f_mmap, NULL},
    {"munmap", f_munmap, NULL},
    {"memcpy", f_memcpy, NULL},
    {"memmove", f_memmove, NULL},
    {"memset", f_memset, NULL},
    {"memcmp", f_memcmp, NULL},
    {"memchr", f_memchr, NULL},
    {"__aeabi_memcpy", f_memcpy, NULL},
    {"__aeabi_memcpy4", f_memcpy, NULL},
    {"__aeabi_memcpy8", f_memcpy, NULL},
    {"__aeabi_memmove", f_memmove, NULL},
    {"__aeabi_memmove4", f_memmove, NULL},
    {"__aeabi_memmove8", f_memmove, NULL},
    {"__aeabi_memset", f_aeabi_memset, NULL},
    {"__aeabi_memset4", f_aeabi_memset, NULL},
    {"__aeabi_memset8", f_aeabi_memset, NULL},
    {"__aeabi_memclr", f_aeabi_memclr, NULL},
    {"__aeabi_memclr4", f_aeabi_memclr, NULL},
    {"__aeabi_memclr8", f_aeabi_memclr, NULL},
    {"strlen", f_strlen, NULL},
    {"strcmp", f_strcmp, NULL},
    {"strncmp", f_strncmp, NULL},
    {"strcasecmp", f_strcasecmp, NULL},
    {"strncasecmp", f_strncasecmp, NULL},
    {"strcpy", f_strcpy, NULL},
    {"strncpy", f_strncpy, NULL},
    {"strcat", f_strcat, NULL},
    {"strncat", f_strncat, NULL},
    {"strchr", f_strchr, NULL},
    {"strrchr", f_strrchr, NULL},
    {"strstr", f_strstr, NULL},
    {"strspn", f_strspn, NULL},
    {"strcspn", f_strcspn, NULL},
    {"strdup", f_strdup, NULL},
    {"toupper", f_toupper, NULL},
    {"tolower", f_tolower, NULL},
    {"atoi", f_atoi, NULL},
    {"atol", f_atoi, NULL},
    {"atoll", f_atoll, NULL},
    {"atof", f_atof, NULL},
    {"strtol", f_strtol, NULL},
    {"strtod", f_strtod, NULL},
    {"sin", m_sin, NULL},
    {"sinf", m_sinf, NULL},
    {"cos", m_cos, NULL},
    {"cosf", m_cosf, NULL},
    {"tan", m_tan, NULL},
    {"tanf", m_tanf, NULL},
    {"asin", m_asin, NULL},
    {"asinf", m_asinf, NULL},
    {"acos", m_acos, NULL},
    {"acosf", m_acosf, NULL},
    {"atan", m_atan, NULL},
    {"atanf", m_atanf, NULL},
    {"atan2", m_atan2, NULL},
    {"atan2f", m_atan2f, NULL},
    {"sqrt", m_sqrt, NULL},
    {"sqrtf", m_sqrtf, NULL},
    {"floor", m_floor, NULL},
    {"floorf", m_floorf, NULL},
    {"ceil", m_ceil, NULL},
    {"ceilf", m_ceilf, NULL},
    {"round", m_round, NULL},
    {"roundf", m_roundf, NULL},
    {"exp", m_exp, NULL},
    {"expf", m_expf, NULL},
    {"log", m_log, NULL},
    {"logf", m_logf, NULL},
    {"log10", m_log10, NULL},
    {"log10f", m_log10f, NULL},
    {"fabs", m_fabs, NULL},
    {"fabsf", m_fabsf, NULL},
    {"pow", m_pow, NULL},
    {"powf", m_powf, NULL},
    {"fmod", m_fmod, NULL},
    {"fmodf", m_fmodf, NULL},
    {"srand48", f_srand48, NULL},
    {"lrand48", f_lrand48, NULL},
    {"srand", f_srand, NULL},
    {"rand", f_rand, NULL},
    {"time", f_time, NULL},
    {"localtime", f_localtime, NULL},
    {"gmtime", f_gmtime, NULL},
    {"gettimeofday", f_gettimeofday, NULL},
    {"clock_gettime", f_clock_gettime, NULL},
    {"usleep", f_usleep, NULL},
    {"getenv", f_getenv, NULL},
    {"__errno", f_errno, NULL},
    {"exit", f_exit, NULL},
    {"abort", f_abort, NULL},
    {"raise", f_raise, NULL},
    {"__assert2", f_assert2, NULL},
    {"__stack_chk_fail", f_stack_chk_fail, NULL},
    {"__cxa_atexit", f_zero, NULL},
    {"__cxa_finalize", f_nothing, NULL},
    {"__gnu_Unwind_Find_exidx", f_find_exidx, NULL},
    {"__android_log_print", log_print, NULL},
    {"__android_log_vprint", log_vprint, NULL},
    {"__android_log_write", log_write, NULL},
    {"dlopen", dl_open, NULL},
    {"dlsym", dl_sym, NULL},
    {"dlclose", f_zero, NULL},
    {"dlerror", dl_error_, NULL},
    {"pthread_mutex_init", p_mutex_init, NULL},
    {"pthread_mutex_destroy", f_zero, NULL},
    {"pthread_mutex_lock", p_mutex_lock, NULL},
    {"pthread_mutex_trylock", p_mutex_trylock, NULL},
    {"pthread_mutex_unlock", p_mutex_unlock, NULL},
    {"pthread_cond_init", f_zero, NULL},
    {"pthread_cond_destroy", f_zero, NULL},
    {"pthread_cond_wait", p_cond_wait, NULL},
    {"pthread_cond_broadcast", p_cond_broadcast, NULL},
    {"pthread_cond_signal", p_cond_signal, NULL},
    {"pthread_key_create", p_key_create, NULL},
    {"pthread_key_delete", f_zero, NULL},
    {"pthread_getspecific", p_getspecific, NULL},
    {"pthread_setspecific", p_setspecific, NULL},
    {"pthread_self", p_self, NULL},
};

static const char* const system_libraries[] = {"libc.so",  "libm.so",          "libdl.so",  "liblog.so",
                                               "libstdc++.so", "libGLESv1_CM.so", "libEGL.so", "libz.so",
                                               "libandroid.so"};
bool arm32_hle_is_system_library(const char* name) {
    for (size_t i = 0; i < sizeof system_libraries / sizeof *system_libraries; ++i)
        if (!strcmp(name, system_libraries[i])) return true;
    return false;
}

static uint32_t stack_guard;

void arm32_hle_init(void) {
    sF = arm32_malloc(3 * FILE_SIZE);
    memset(arm32_mem + sF, 0, 3 * FILE_SIZE);
    tm_buffer = arm32_malloc(64);
    dl_error_buffer = arm32_malloc(512);
    stack_guard = arm32_malloc(4);
    wr32(stack_guard, 0x5ca2c0de ^ (uint32_t)time(NULL));
    for (size_t i = 0; i < sizeof gles_calls / sizeof *gles_calls; ++i)
        for (const HleSymbol* s = hle_gles; s->name[0]; ++s)
            if (!strcmp(s->name, gles_calls[i].name)) {
                gles_calls[i].handler = s->handler;
                gles_calls[i].data = s->data;
            }
}

uint32_t arm32_hle_symbol(const char* name) {
    if (!strcmp(name, "__sF")) return sF;
    if (!strcmp(name, "__stack_chk_guard")) return stack_guard;
    for (size_t i = 0; i < sizeof symbols / sizeof *symbols; ++i)
        if (!strcmp(symbols[i].name, name)) return arm32_export(name, symbols[i].handler, symbols[i].data);
    for (size_t i = 0; i < sizeof gles_calls / sizeof *gles_calls; ++i)
        if (!strcmp(gles_calls[i].name, name)) return arm32_export(name, gles_call, &gles_calls[i]);
    return 0;
}

HANDLER(missing) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    bool* said = (bool*)((char*)data + strlen(data) + 1);
    if (!*said) fprintf(stderr, "scarletnx-android: arm32: %s is not there yet, it returns 0\n", (const char*)data);
    *said = true;
    pthread_mutex_unlock(&lock);
    R(0) = 0;
    R(1) = 0;
}
uint32_t arm32_hle_missing(const char* name) {
    /* the name, then the flag that it was reported */
    size_t n = strlen(name) + 1;
    char* data = calloc(1, n + sizeof(bool));
    if (!data) return 0;
    memcpy(data, name, n);
    char export_name[300];
    snprintf(export_name, sizeof export_name, "missing:%s", name);
    return arm32_export(export_name, missing, data);
}
