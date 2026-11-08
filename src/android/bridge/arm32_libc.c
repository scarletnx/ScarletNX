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
#include <sys/mman.h>

#define R(n)    (c->r[n])
#define P(n)    (arm32_host(c->r[n]))
#define S(n)    ((const char*)arm32_host(c->r[n]))
#define HANDLER(name) static void name(SnxArmCPU* c, void* data)
#define UNUSED (void)data

static uint32_t rd32(uint32_t a) { return snx_ld_le32(arm32_mem + a); }
static void wr32(uint32_t a, uint32_t v) { snx_st_le32(arm32_mem + a, v); }
/* the n'th argument word, past r3 on the stack */
uint32_t arm32_word(const SnxArmCPU* c, unsigned n) { return n < 4 ? c->r[n] : rd32(c->r[13] + 4 * (n - 4)); }
#define word arm32_word
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
void arm32_set_errno(int host_error) { wr32(arm32_errno_address(), (uint32_t)-gnu_host_errno(host_error)); }
void arm32_set_errno_linux(int linux_error) { wr32(arm32_errno_address(), (uint32_t)linux_error); }
#define set_errno arm32_set_errno

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

static int scan_at(const char* input, const char* f, Arm32Args* a, const char** end);
static int scan_used(const char* input, const char* f, Arm32Args* a, size_t* used) {
    const char* in = input;
    int count = scan_at(input, f, a, &in);
    *used = (size_t)(in - input);
    return count;
}
static int scan(const char* input, const char* f, Arm32Args* a) {
    const char* in = input;
    return scan_at(input, f, a, &in);
}

/* the scan, with where it stopped in *end */
static int scan_at(const char* input, const char* f, Arm32Args* a, const char** end) {
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
        if (!*in) {
            *end = in;
            return count ? count : -1;
        }
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
    *end = in;
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

void* arm32_host_file(uint32_t f) { return host_file(f); }
uint32_t arm32_guest_file(void* file) {
    FILE* host = file;
    if (host == stdin) return sF;
    if (host == stdout) return sF + FILE_SIZE;
    if (host == stderr) return sF + 2 * FILE_SIZE;
    uint32_t g = arm32_malloc(FILE_SIZE);
    pthread_mutex_lock(&files.lock);
    unsigned i = 0;
    while (i < MAX_FILES && files.guest[i]) ++i;
    if (i < MAX_FILES && g) {
        files.guest[i] = g;
        files.host[i] = host;
    }
    pthread_mutex_unlock(&files.lock);
    if (i == MAX_FILES && g) {
        arm32_free(g);
        g = 0;
    }
    return g;
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

HANDLER(f_fdopen) {
    UNUSED;
    FILE* file = fdopen((int)R(0), S(1));
    if (!file) {
        set_errno(errno);
        R(0) = 0;
        return;
    }
    R(0) = arm32_guest_file(file);
    if (!R(0)) fclose(file);
}
HANDLER(f_fflush) {
    UNUSED;
    FILE* file = R(0) ? host_file(R(0)) : NULL;
    R(0) = (uint32_t)(R(0) && !file ? EOF : fflush(file));
}
HANDLER(f_fputs) {
    UNUSED;
    FILE* file = host_file(R(1));
    R(0) = file ? (uint32_t)fputs(S(0), file) : (uint32_t)EOF;
}
HANDLER(f_fputc) {
    UNUSED;
    FILE* file = host_file(R(1));
    R(0) = file ? (uint32_t)fputc((int)R(0), file) : (uint32_t)EOF;
}
HANDLER(f_putchar) {
    UNUSED;
    R(0) = (uint32_t)putchar((int)R(0));
}
HANDLER(f_getc) {
    UNUSED;
    FILE* file = host_file(R(0));
    R(0) = file ? (uint32_t)getc(file) : (uint32_t)EOF;
}
HANDLER(f_ungetc) {
    UNUSED;
    FILE* file = host_file(R(1));
    R(0) = file ? (uint32_t)ungetc((int)R(0), file) : (uint32_t)EOF;
}
HANDLER(f_clearerr) {
    UNUSED;
    FILE* file = host_file(R(0));
    if (file) clearerr(file);
}
HANDLER(f_ferror) {
    UNUSED;
    FILE* file = host_file(R(0));
    R(0) = file ? (uint32_t)ferror(file) : 1;
}
HANDLER(f_feof) {
    UNUSED;
    FILE* file = host_file(R(0));
    R(0) = file ? (uint32_t)feof(file) : 1;
}
HANDLER(f_fileno) {
    UNUSED;
    FILE* file = host_file(R(0));
    R(0) = file ? (uint32_t)fileno(file) : UINT32_MAX;
}
/* setvbuf(file, buffer, mode, size): the host's own buffer, whatever the guest gives; the modes are Linux's 0 full,
   1 line, 2 none */
HANDLER(f_setvbuf) {
    UNUSED;
    FILE* file = host_file(R(0));
    int mode = R(2) == 0 ? _IOFBF : R(2) == 1 ? _IOLBF : _IONBF;
    R(0) = file ? (uint32_t)setvbuf(file, NULL, mode, R(3) ? R(3) : BUFSIZ) : UINT32_MAX;
}
HANDLER(f_vfprintf) {
    UNUSED;
    Arm32Args a;
    arm32_args_va(&a, R(2));
    char* text = format_alloc(S(1), &a);
    FILE* file = host_file(R(0));
    R(0) = text && file && fputs(text, file) >= 0 ? (uint32_t)strlen(text) : UINT32_MAX;
    free(text);
}
HANDLER(f_vprintf) {
    UNUSED;
    Arm32Args a;
    arm32_args_va(&a, R(1));
    char* text = format_alloc(S(0), &a);
    R(0) = text && fputs(text, stdout) >= 0 ? (uint32_t)strlen(text) : UINT32_MAX;
    free(text);
}
HANDLER(f_vsprintf) {
    UNUSED;
    Arm32Args a;
    arm32_args_va(&a, R(2));
    char* text = format_alloc(S(1), &a);
    size_t n = text ? strlen(text) : 0;
    if (text) memcpy(P(0), text, n + 1);
    R(0) = (uint32_t)n;
    free(text);
}
/* vasprintf(out, format, va_list) and asprintf(out, format, ...): the guest's malloc'd copy */
HANDLER(f_vasprintf) {
    UNUSED;
    Arm32Args a;
    if (data) arm32_args(&a, c, 2);
    else arm32_args_va(&a, R(2));
    char* text = format_alloc(S(1), &a);
    uint32_t g = text ? arm32_strdup(text) : 0;
    wr32(R(0), g);
    R(0) = g ? (uint32_t)strlen(text) : UINT32_MAX;
    free(text);
}
HANDLER(f_vsscanf) {
    UNUSED;
    Arm32Args a;
    arm32_args_va(&a, R(2));
    R(0) = (uint32_t)scan(S(0), S(1), &a);
}
/* fscanf(file, format, ...): the rest of a seekable file scanned, then the file at what the scan took */
HANDLER(f_fscanf) {
    UNUSED;
    FILE* file = host_file(R(0));
    long at = file ? ftell(file) : -1;
    if (at < 0) {
        R(0) = UINT32_MAX;
        return;
    }
    char buffer[65536];
    size_t n = fread(buffer, 1, sizeof buffer - 1, file);
    buffer[n] = 0;
    Arm32Args a;
    arm32_args(&a, c, 2);
    size_t used = 0;
    int r = scan_used(buffer, S(1), &a, &used);
    fseek(file, at + (long)used, SEEK_SET);
    R(0) = (uint32_t)r;
}
HANDLER(f_perror) {
    UNUSED;
    int e = (int)rd32(arm32_errno_address());
    char text[256];
    hle_strerror_r(e, text, sizeof text);
    if (R(0) && *S(0)) fprintf(stderr, "%s: %s\n", S(0), text);
    else fprintf(stderr, "%s\n", text);
}

/* bionic's struct stat on 32-bit ARM (the kernel's stat64), 104 bytes */
static void put_stat(uint8_t* o, const struct stat* st) {
    memset(o, 0, 104);
    uint64_t v = (uint64_t)st->st_dev;
    memcpy(o, &v, 8);
    snx_st_le32(o + 12, (uint32_t)st->st_ino);
    snx_st_le32(o + 16, (uint32_t)st->st_mode);
    snx_st_le32(o + 20, (uint32_t)st->st_nlink);
    snx_st_le32(o + 24, (uint32_t)st->st_uid);
    snx_st_le32(o + 28, (uint32_t)st->st_gid);
    v = (uint64_t)st->st_rdev;
    memcpy(o + 32, &v, 8);
    v = (uint64_t)st->st_size;
    memcpy(o + 48, &v, 8);
    snx_st_le32(o + 56, (uint32_t)st->st_blksize);
    v = (uint64_t)st->st_blocks;
    memcpy(o + 64, &v, 8);
    snx_st_le32(o + 72, (uint32_t)st->st_atime);
    snx_st_le32(o + 80, (uint32_t)st->st_mtime);
    snx_st_le32(o + 88, (uint32_t)st->st_ctime);
    v = (uint64_t)st->st_ino;
    memcpy(o + 96, &v, 8);
}
HANDLER(f_fstat) {
    UNUSED;
    struct stat st;
    if (fstat((int)R(0), &st)) {
        set_errno(errno);
        R(0) = UINT32_MAX;
        return;
    }
    put_stat(P(1), &st);
    R(0) = 0;
}
HANDLER(f_lstat) {
    UNUSED;
    struct stat st;
    if (lstat(S(0), &st)) {
        set_errno(errno);
        R(0) = UINT32_MAX;
        return;
    }
    put_stat(P(1), &st);
    R(0) = 0;
}

HANDLER(f_stat) {
    UNUSED;
    struct stat st;
    if (stat(S(0), &st)) {
        set_errno(errno);
        R(0) = UINT32_MAX;
        return;
    }
    put_stat(P(1), &st);
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
    /* MAP_FIXED (0x10): the address the guest names, which it may hold already */
    uint32_t a = flags & 0x10 ? (arm32_map_at(R(0), length) ? R(0) : 0) : arm32_map(length);
    if (!a) {
        set_errno(ENOMEM);
        R(0) = UINT32_MAX;
        return;
    }
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
    R(0) = a;
}
HANDLER(f_munmap) {
    UNUSED;
    /* a file's pages made anonymous again */
    mmap(arm32_mem + R(0), R(1), PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
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
/* the wide characters of the C locale, bionic's only one: a byte is its own character */
HANDLER(f_btowc) {
    UNUSED;
    R(0) = (int32_t)R(0) >= 0 && R(0) < 0x80 ? R(0) : UINT32_MAX; /* WEOF */
}
HANDLER(f_wctob) {
    UNUSED;
    R(0) = R(0) < 0x80 ? R(0) : UINT32_MAX; /* EOF */
}
/* wctype(name): bionic's numbers of the classes, 1 alnum to 12 xdigit; 0 for another name */
static const char* const wctypes[] = {"alnum", "alpha", "blank", "cntrl", "digit", "graph",
                                      "lower", "print", "punct", "space", "upper", "xdigit"};
HANDLER(f_wctype) {
    UNUSED;
    uint32_t n = 0;
    for (unsigned i = 0; i < 12 && !n; ++i)
        if (!strcmp(S(0), wctypes[i])) n = i + 1;
    R(0) = n;
}
HANDLER(f_iswctype) {
    UNUSED;
    uint32_t w = R(0);
    int ch = w < 0x80 ? (int)w : -1;
    bool r = false;
    if (ch >= 0) switch (R(1)) {
        case 1: r = isalnum(ch); break;
        case 2: r = isalpha(ch); break;
        case 3: r = ch == ' ' || ch == '\t'; break;
        case 4: r = iscntrl(ch); break;
        case 5: r = isdigit(ch); break;
        case 6: r = isgraph(ch); break;
        case 7: r = islower(ch); break;
        case 8: r = isprint(ch); break;
        case 9: r = ispunct(ch); break;
        case 10: r = isspace(ch); break;
        case 11: r = isupper(ch); break;
        case 12: r = isxdigit(ch); break;
        default: break;
        }
    R(0) = r;
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
/* a library's handle: its index in the libraries dlopen gave, from 2 on (1 is the system's), as the guest's word */
enum { MAX_HANDLES = 256 };
static const Arm32Library* dl_handles[MAX_HANDLES];
static pthread_mutex_t dl_lock = PTHREAD_MUTEX_INITIALIZER;
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
    pthread_mutex_lock(&dl_lock);
    unsigned i = 0;
    while (i < MAX_HANDLES && dl_handles[i] && dl_handles[i] != l) ++i;
    if (i < MAX_HANDLES) dl_handles[i] = l;
    pthread_mutex_unlock(&dl_lock);
    R(0) = i < MAX_HANDLES ? i + 2 : 0;
}
/* dlsym(handle, name): a library's own symbol, else the system's; RTLD_DEFAULT (0xffffffff), RTLD_NEXT
   (0xfffffffe) and the system's handle search every library, then the system's */
HANDLER(dl_sym) {
    UNUSED;
    const char* name = S(1);
    uint32_t h = R(0), a = 0;
    if (h >= 2 && h < MAX_HANDLES + 2) {
        pthread_mutex_lock(&dl_lock);
        const Arm32Library* l = dl_handles[h - 2];
        pthread_mutex_unlock(&dl_lock);
        a = l ? arm32_symbol(l, name) : 0;
    } else {
        a = arm32_symbol(NULL, name);
    }
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

/* ---- the tables ---- */

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
    {"fstat", f_fstat, NULL},
    {"lstat", f_lstat, NULL},
    {"fdopen", f_fdopen, NULL},
    {"fflush", f_fflush, NULL},
    {"fputs", f_fputs, NULL},
    {"fputc", f_fputc, NULL},
    {"putc", f_fputc, NULL},
    {"putchar", f_putchar, NULL},
    {"getc", f_getc, NULL},
    {"fgetc", f_getc, NULL},
    {"ungetc", f_ungetc, NULL},
    {"clearerr", f_clearerr, NULL},
    {"ferror", f_ferror, NULL},
    {"feof", f_feof, NULL},
    {"fileno", f_fileno, NULL},
    {"setvbuf", f_setvbuf, NULL},
    {"vfprintf", f_vfprintf, NULL},
    {"vprintf", f_vprintf, NULL},
    {"vsprintf", f_vsprintf, NULL},
    {"vasprintf", f_vasprintf, NULL},
    {"asprintf", f_vasprintf, (void*)1},
    {"vsscanf", f_vsscanf, NULL},
    {"fscanf", f_fscanf, NULL},
    {"perror", f_perror, NULL},
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
    {"__errno", f_errno, NULL},
    {"exit", f_exit, NULL},
    {"abort", f_abort, NULL},
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
    {"__aeabi_atexit", f_zero, NULL},
    {"btowc", f_btowc, NULL},
    {"wctob", f_wctob, NULL},
    {"wctype", f_wctype, NULL},
    {"iswctype", f_iswctype, NULL},
};

static const char* const system_libraries[] = {
    "libc.so",   "libm.so",   "libdl.so",     "liblog.so",      "libstdc++.so",     "libGLESv1_CM.so", "libGLESv2.so",
    "libGLESv3.so", "libEGL.so", "libz.so", "libandroid.so", "libOpenSLES.so", "libjnigraphics.so"};
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
    arm32_posix_init();
    arm32_thread_init();
    arm32_shim_init();
}

static uint32_t in_table(const Arm32Symbol* table, size_t count, const char* name) {
    for (size_t i = 0; count ? i < count : table[i].name[0] != 0; ++i)
        if (!strcmp(table[i].name, name))
            return table[i].handler ? arm32_export(name, table[i].handler, table[i].data)
                                    : (uint32_t)(uintptr_t)table[i].data;
    return 0;
}
uint32_t arm32_hle_symbol(const char* name) {
    if (!strcmp(name, "__sF")) return sF;
    if (!strcmp(name, "__stack_chk_guard")) return stack_guard;
    uint32_t a = in_table(symbols, sizeof symbols / sizeof *symbols, name);
    if (!a) a = in_table(arm32_posix_symbols, 0, name);
    if (!a) a = in_table(arm32_thread_symbols, 0, name);
    if (!a) a = in_table(arm32_shim_symbols, 0, name);
    if (!a) a = arm32_posix_object(name);
    return a;
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
