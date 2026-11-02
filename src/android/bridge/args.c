/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A guest call's arguments and the guest's printf (args.h) */
#include "android/bridge/args.h"
#include <stdio.h>
#include <string.h>

static uint64_t load64(uint64_t address) {
    uint64_t v;
    memcpy(&v, (const void*)(uintptr_t)address, 8);
    return v;
}
static double load_double(uint64_t address) {
    double v;
    memcpy(&v, (const void*)(uintptr_t)address, 8);
    return v;
}

void args_from_registers(GuestArgs* a, const CPU* c, unsigned first, unsigned vfirst) {
    memset(a, 0, sizeof *a);
    a->c = c;
    a->next_x = first;
    a->next_v = vfirst;
    a->stack = c->x[31];
}

void args_from_va_list(GuestArgs* a, uint64_t va_list) {
    memset(a, 0, sizeof *a);
    a->va = true;
    a->stack = load64(va_list);
    a->gr_top = load64(va_list + 8);
    a->vr_top = load64(va_list + 16);
    uint32_t offs[2];
    memcpy(offs, (const void*)(uintptr_t)(va_list + 24), 8);
    a->gr_offs = (int32_t)offs[0];
    a->vr_offs = (int32_t)offs[1];
}

uint64_t args_int(GuestArgs* a) {
    if (a->va) {
        if (a->gr_offs < 0) {
            uint64_t v = load64(a->gr_top + (uint64_t)(int64_t)a->gr_offs);
            a->gr_offs += 8;
            return v;
        }
    } else if (a->next_x < 8) {
        return a->c->x[a->next_x++];
    }
    uint64_t v = load64(a->stack);
    a->stack += 8;
    return v;
}

double args_double(GuestArgs* a) {
    if (a->va) {
        if (a->vr_offs < 0) {
            /* each vector register saved whole: 16 bytes, the double in the low 8 */
            double v = load_double(a->vr_top + (uint64_t)(int64_t)a->vr_offs);
            a->vr_offs += 16;
            return v;
        }
    } else if (a->next_v < 8) {
        double v;
        memcpy(&v, &a->c->v[a->next_v++].df[0], 8);
        return v;
    }
    double v = load_double(a->stack);
    a->stack += 8;
    return v;
}

/* one piece of output: n bytes at text, into out as far as it has room */
static void put(char* out, size_t size, size_t* at, const char* text, size_t n) {
    if (*at < size) memcpy(out + *at, text, *at + n < size ? n : size - *at);
    *at += n;
}

int guest_format(char* out, size_t size, const char* format, GuestArgs* a) {
    size_t at = 0;
    char piece[512];
    for (const char* p = format; *p;) {
        if (*p != '%') {
            const char* end = strchr(p, '%');
            size_t n = end ? (size_t)(end - p) : strlen(p);
            put(out, size, &at, p, n);
            p += n;
            continue;
        }
        /* one conversion: copied into spec with the guest's arguments read for its * fields */
        char spec[64];
        size_t s = 0;
        spec[s++] = *p++;
        int star[2], stars = 0;
        while (*p && strchr("-+ #0", *p) && s < 40) spec[s++] = *p++;
        if (*p == '*') {
            star[stars++] = (int)args_int(a);
            spec[s++] = *p++;
        }
        while (*p >= '0' && *p <= '9' && s < 40) spec[s++] = *p++;
        if (*p == '.') {
            spec[s++] = *p++;
            if (*p == '*') {
                star[stars++] = (int)args_int(a);
                spec[s++] = *p++;
            }
            while (*p >= '0' && *p <= '9' && s < 40) spec[s++] = *p++;
        }
        /* the length: the guest's long is 64 bits as the host's (LP64) */
        int length = 0; /* 0 int, 1 long, 2 long long, 3 size_t/intmax/ptrdiff, 4 long double, -1 short, -2 char */
        for (;;) {
            if (*p == 'l') length = length == 1 ? 2 : 1;
            else if (*p == 'h') length = length == -1 ? -2 : -1;
            else if (*p == 'z' || *p == 'j' || *p == 't' || *p == 'q') length = 3;
            else if (*p == 'L') length = 4;
            else break;
            ++p;
        }
        char conv = *p ? *p++ : 0;
        int n = 0;
        switch (conv) {
        case '%': put(out, size, &at, "%", 1); continue;
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c': {
            spec[s++] = 'l';
            spec[s++] = 'l';
            spec[s++] = conv;
            spec[s] = 0;
            uint64_t v = args_int(a);
            long long value;
            bool is_signed = conv == 'd' || conv == 'i';
            if (length >= 1 || conv == 'c') value = (long long)v;
            else if (length == -1) value = is_signed ? (short)v : (unsigned short)v;
            else if (length == -2) value = is_signed ? (signed char)v : (unsigned char)v;
            else value = is_signed ? (int32_t)v : (uint32_t)v;
            if (conv == 'c') {
                spec[s - 3] = 'c';
                spec[s - 2] = 0;
                n = stars == 2 ? snprintf(piece, sizeof piece, spec, star[0], star[1], (int)value)
                    : stars == 1 ? snprintf(piece, sizeof piece, spec, star[0], (int)value)
                                 : snprintf(piece, sizeof piece, spec, (int)value);
            } else {
                n = stars == 2 ? snprintf(piece, sizeof piece, spec, star[0], star[1], value)
                    : stars == 1 ? snprintf(piece, sizeof piece, spec, star[0], value)
                                 : snprintf(piece, sizeof piece, spec, value);
            }
            break;
        }
        case 'p': {
            spec[s++] = 'p';
            spec[s] = 0;
            void* v = (void*)(uintptr_t)args_int(a);
            n = stars == 1 ? snprintf(piece, sizeof piece, spec, star[0], v) : snprintf(piece, sizeof piece, spec, v);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            spec[s++] = conv;
            spec[s] = 0;
            /* a long double is 128 bits on AArch64: passed in a q register, and printed as the double nearest */
            double v = args_double(a);
            n = stars == 2 ? snprintf(piece, sizeof piece, spec, star[0], star[1], v)
                : stars == 1 ? snprintf(piece, sizeof piece, spec, star[0], v)
                             : snprintf(piece, sizeof piece, spec, v);
            break;
        }
        case 's': {
            spec[s++] = 's';
            spec[s] = 0;
            const char* v = (const char*)(uintptr_t)args_int(a);
            if (!v) v = "(null)";
            /* a long string goes out whole, without the piece's limit */
            if (stars == 0 && s == 2) {
                put(out, size, &at, v, strlen(v));
                continue;
            }
            n = stars == 2 ? snprintf(piece, sizeof piece, spec, star[0], star[1], v)
                : stars == 1 ? snprintf(piece, sizeof piece, spec, star[0], v)
                             : snprintf(piece, sizeof piece, spec, v);
            break;
        }
        case 'n': args_int(a); continue; /* nothing written back: bionic aborts on %n */
        case 'm': n = snprintf(piece, sizeof piece, "(errno)"); break;
        default: put(out, size, &at, spec, s); continue;
        }
        if (n > 0) put(out, size, &at, piece, (size_t)n < sizeof piece ? (size_t)n : sizeof piece - 1);
    }
    if (size) out[at < size ? at : size - 1] = 0;
    return (int)at;
}
