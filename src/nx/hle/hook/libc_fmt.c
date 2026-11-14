/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The C library's formatted output and input a game imports from its SDK (hooks.hook's libc_fmt family): snprintf,
   sprintf, vsnprintf, vsprintf, printf, vprintf, puts, putchar, perror, sscanf and vsscanf, run by the host's C
   library on guest memory.

   The host's printf can't read the guest's arguments, so each conversion of the format goes to the host's snprintf
   alone, with its argument taken where the guest's calling convention put it: a direct call's in the registers after
   the fixed ones (x, then v for floating point) and then the caller's stack, a va_list's through its AAPCS64 record
   (__stack, __gr_top, __vr_top, __gr_offs, __vr_offs). What musl, the SDK's C library, prints otherwise than the host
   follows musl: %p is 0x and 16 hex digits, %s of NULL is "(null)". A long double is the guest's IEEE quad, converted.
   A format with a conversion this doesn't take (a numbered argument, %m, an unknown one) goes to the function's own
   code, before anything is written.

   sscanf's directives each go to the host's sscanf, with %n after it for how far it read, writing straight through
   the guest's pointers (the guest's types and the host's are the same size and order, a long double apart).

   The standard output (printf, puts, putchar, perror's standard error) is the runtime's log, a line at a time, as a
   game's svcOutputDebugString is ("[game] ..."). Off on Windows, as libc_str */
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "nx/runtime/platform/log.h"
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define S(a) ((char*)hook_host(c, a))

/* ---- the guest's arguments: a direct call's, or a va_list's */
typedef struct {
    CPU* c;
    int va;
    int gr, vr; /* a direct call's next x and v register */
    u64 stack;  /* the next stack argument's address */
    u64 gr_top, vr_top;
    i32 gr_offs, vr_offs;
} Args;
static void args_direct(Args* a, CPU* c, int fixed) {
    memset(a, 0, sizeof *a);
    a->c = c;
    a->gr = fixed;
    a->stack = c->x[31];
}
static void args_va(Args* a, CPU* c, u64 ap) {
    const u8* p = hook_host(c, ap);
    memset(a, 0, sizeof *a);
    a->c = c;
    a->va = 1;
    a->stack = snx_ld_le64(p);
    a->gr_top = snx_ld_le64(p + 8);
    a->vr_top = snx_ld_le64(p + 16);
    a->gr_offs = (i32)snx_ld_le32(p + 24);
    a->vr_offs = (i32)snx_ld_le32(p + 28);
}
static u64 stack_u64(Args* a, u64 align) {
    a->stack = (a->stack + align - 1) & ~(align - 1);
    u64 v = snx_ld_le64(hook_host(a->c, a->stack));
    a->stack += 8;
    return v;
}
static u64 arg_int(Args* a) {
    if (a->va) {
        if (a->gr_offs < 0) {
            u64 v = snx_ld_le64(hook_host(a->c, a->gr_top + (u64)(i64)a->gr_offs));
            a->gr_offs += 8;
            return v;
        }
        return stack_u64(a, 8);
    }
    return a->gr < 8 ? a->c->x[a->gr++] : stack_u64(a, 8);
}
static u64 arg_fp_bits(Args* a, u64* hi) { /* a floating-point argument's 16 bytes: a double in the low 8 */
    *hi = 0;
    if (a->va) {
        if (a->vr_offs < 0) {
            const u8* p = hook_host(a->c, a->vr_top + (u64)(i64)a->vr_offs);
            a->vr_offs += 16;
            *hi = snx_ld_le64(p + 8);
            return snx_ld_le64(p);
        }
        return stack_u64(a, 8);
    }
    if (a->vr < 8) {
        int r = a->vr++;
        *hi = SNX_VD(a->c->v[r], 1);
        return SNX_VD(a->c->v[r], 0);
    }
    return stack_u64(a, 8);
}
static f64 arg_double(Args* a) {
    u64 hi, bits = arg_fp_bits(a, &hi);
    f64 d;
    memcpy(&d, &bits, 8);
    return d;
}
/* the guest's long double, an IEEE quad (a va_list's or a register's 16 bytes, a stack's 16-aligned), as a double */
static f64 quad_double(u64 lo, u64 hi) {
    u32 e = (u32)(hi >> 48) & 0x7fff;
    u64 sign = hi >> 63, mant = (hi << 16) | (lo >> 48); /* the quad's top 64 fraction bits */
    u64 bits;
    if (e == 0x7fff)
        bits = sign << 63 | 0x7ffull << 52 | (mant ? (mant >> 12) | 1 : 0);
    else if (e == 0 && !mant)
        bits = sign << 63;
    else {
        i32 de = (i32)e - 16383 + 1023;
        if (de >= 0x7ff)
            bits = sign << 63 | 0x7ffull << 52;
        else if (de <= 0)
            bits = sign << 63; /* below a double's range: zero */
        else
            bits = sign << 63 | (u64)de << 52 | (mant >> 12);
    }
    f64 d;
    memcpy(&d, &bits, 8);
    return d;
}
static void double_quad(f64 d, u64* lo, u64* hi) {
    u64 bits;
    memcpy(&bits, &d, 8);
    u64 sign = bits >> 63, e = (bits >> 52) & 0x7ff, m = bits & ((1ull << 52) - 1);
    u64 qe = e == 0x7ff ? 0x7fff : e == 0 ? 0 : e - 1023 + 16383; /* a subnormal double: zero */
    if (!e) m = 0;
    *hi = sign << 63 | qe << 48 | (m >> 4);
    *lo = m << 60;
}
static f64 arg_long_double(Args* a) {
    if (!a->va && a->vr >= 8) {
        a->stack = (a->stack + 15) & ~15ull;
        u64 lo = snx_ld_le64(hook_host(a->c, a->stack)), hi = snx_ld_le64(hook_host(a->c, a->stack + 8));
        a->stack += 16;
        return quad_double(lo, hi);
    }
    if (a->va && a->vr_offs >= 0) {
        a->stack = (a->stack + 15) & ~15ull;
        u64 lo = snx_ld_le64(hook_host(a->c, a->stack)), hi = snx_ld_le64(hook_host(a->c, a->stack + 8));
        a->stack += 16;
        return quad_double(lo, hi);
    }
    u64 hi, lo = arg_fp_bits(a, &hi);
    return quad_double(lo, hi);
}

/* ---- the output, a host buffer that grows */
typedef struct {
    char* p;
    size_t n, cap;
} Out;
static void out_put(Out* o, const char* s, size_t n) {
    if (o->n + n + 1 > o->cap) {
        size_t cap = o->cap ? o->cap : 256;
        while (cap < o->n + n + 1) cap *= 2;
        char* p = realloc(o->p, cap);
        if (!p) fatal("hooks: no host memory for a formatted string");
        o->p = p;
        o->cap = cap;
    }
    memcpy(o->p + o->n, s, n);
    o->n += n;
    o->p[o->n] = 0;
}

/* a conversion of the format: what it is, read from fmt at f */
typedef struct {
    char flags[8];
    int width, prec; /* -1: none, -2: an argument's ('*') */
    char len[3];     /* hh h l ll L z j t q */
    char conv;
    const char* end; /* the format after it */
} Spec;
static int spec_parse(const char* f, Spec* s) {
    int n = 0;
    memset(s, 0, sizeof *s);
    s->width = s->prec = -1;
    while (*f && strchr("-+ #0'", *f) && n < 7) s->flags[n++] = *f++;
    if (*f == '*') {
        s->width = -2;
        f++;
    } else if (isdigit((unsigned char)*f)) {
        s->width = (int)strtol(f, (char**)&f, 10);
        if (*f == '$') return 0; /* a numbered argument */
    }
    if (*f == '.') {
        f++;
        if (*f == '*') {
            s->prec = -2;
            f++;
        } else
            s->prec = (int)strtol(f, (char**)&f, 10);
    }
    for (n = 0; *f && strchr("hlLzjtq", *f) && n < 2; n++) s->len[n] = *f++;
    if (!*f || !strchr("diouxXcspneEfFgGaA%", *f)) return 0;
    s->conv = *f++;
    s->end = f;
    return 1;
}
/* every conversion of fmt is one this takes */
static int format_known(const char* f) {
    Spec s;
    for (; *f; f++)
        if (*f == '%') {
            if (!spec_parse(f + 1, &s)) return 0;
            f = s.end - 1;
        }
    return 1;
}
static int is_long(const Spec* s) { return s->len[0] && strchr("lzjtq", s->len[0]); }
/* fmt formatted into o with a's arguments: what it wrote, or -1 */
static int format(CPU* c, Out* o, const char* f, Args* a) {
    Spec s;
    char spec[48], buf[512];
    while (*f) {
        const char* pct = strchr(f, '%');
        if (!pct) {
            out_put(o, f, strlen(f));
            break;
        }
        out_put(o, f, (size_t)(pct - f));
        if (!spec_parse(pct + 1, &s)) return -1; /* format_known said otherwise */
        f = s.end;
        if (s.conv == '%') {
            out_put(o, "%", 1);
            continue;
        }
        int width = s.width == -2 ? (int)(i32)arg_int(a) : s.width;
        int prec = s.prec == -2 ? (int)(i32)arg_int(a) : s.prec;
        char flags[9];
        snprintf(flags, sizeof flags, "%s%s", s.flags, width < 0 && s.width == -2 ? "-" : "");
        if (width < 0 && s.width == -2) width = -width;
        char conv = s.conv;
        int n = -1;
        /* the spec for the host: the guest's flags, width and precision, a length of the host's for the value */
#define SPEC(len, cv)                                                                                                  \
    do {                                                                                                               \
        char *w_ = spec, *e_ = spec + sizeof spec;                                                                     \
        w_ += snprintf(w_, (size_t)(e_ - w_), "%%%s", flags);                                                          \
        if (width >= 0) w_ += snprintf(w_, (size_t)(e_ - w_), "%d", width);                                            \
        if (prec >= 0) w_ += snprintf(w_, (size_t)(e_ - w_), ".%d", prec);                                             \
        snprintf(w_, (size_t)(e_ - w_), "%s%c", len, cv);                                                              \
    } while (0)
        switch (conv) {
            case 'd':
            case 'i':
            case 'o':
            case 'u':
            case 'x':
            case 'X': {
                u64 v = arg_int(a);
                if (!strcmp(s.len, "hh"))
                    v = conv == 'd' || conv == 'i' ? (u64)(i64)(i8)v : (u8)v;
                else if (s.len[0] == 'h')
                    v = conv == 'd' || conv == 'i' ? (u64)(i64)(i16)v : (u16)v;
                else if (!is_long(&s))
                    v = conv == 'd' || conv == 'i' ? (u64)(i64)(i32)v : (u32)v;
                SPEC("ll", conv);
                n = snprintf(buf, sizeof buf, spec, (long long)v);
                break;
            }
            case 'c': {
                u64 v = arg_int(a);
                if (s.len[0] == 'l') {
                    SPEC("l", 'c');
                    n = snprintf(buf, sizeof buf, spec, (wint_t)(u32)v);
                } else {
                    SPEC("", 'c');
                    n = snprintf(buf, sizeof buf, spec, (int)(u8)v);
                }
                break;
            }
            case 's': {
                u64 p = arg_int(a);
                if (s.len[0] == 'l' && p) {
                    SPEC("l", 's');
                    n = snprintf(NULL, 0, spec, (const wchar_t*)hook_host(c, p));
                    char* big = n >= 0 ? malloc((size_t)n + 1) : NULL;
                    if (big) {
                        snprintf(big, (size_t)n + 1, spec, (const wchar_t*)hook_host(c, p));
                        out_put(o, big, (size_t)n);
                        free(big);
                    }
                    continue;
                }
                SPEC("", 's');
                const char* str = p ? S(p) : "(null)"; /* musl's */
                n = snprintf(NULL, 0, spec, str);
                if (n >= (int)sizeof buf) {
                    char* big = malloc((size_t)n + 1);
                    if (!big) return -1;
                    snprintf(big, (size_t)n + 1, spec, str);
                    out_put(o, big, (size_t)n);
                    free(big);
                    continue;
                }
                snprintf(buf, sizeof buf, spec, str);
                break;
            }
            case 'p': { /* musl's: 0x and 16 hex digits, as %#.16x */
                u64 v = arg_int(a);
                if (prec < 16) prec = 16;
                snprintf(flags, sizeof flags, "#%s", s.flags);
                SPEC("ll", 'x');
                n = snprintf(buf, sizeof buf, spec, (unsigned long long)v);
                break;
            }
            case 'n': { /* what was written so far, through the guest's pointer, of the length's size */
                u64 p = arg_int(a);
                u8* d = hook_host(c, p);
                if (!strcmp(s.len, "hh"))
                    *d = (u8)o->n;
                else if (s.len[0] == 'h')
                    snx_st_le16(d, (u16)o->n);
                else if (is_long(&s) || !strcmp(s.len, "ll"))
                    snx_st_le64(d, o->n);
                else
                    snx_st_le32(d, (u32)o->n);
                continue;
            }
            default: { /* e E f F g G a A */
                f64 v = s.len[0] == 'L' ? arg_long_double(a) : arg_double(a);
                SPEC("", conv);
                n = snprintf(buf, sizeof buf, spec, v);
                if (n >= (int)sizeof buf) {
                    char* big = malloc((size_t)n + 1);
                    if (!big) return -1;
                    snprintf(big, (size_t)n + 1, spec, v);
                    out_put(o, big, (size_t)n);
                    free(big);
                    continue;
                }
                break;
            }
        }
#undef SPEC
        if (n < 0) return -1;
        out_put(o, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
    }
    return (int)o->n;
}

/* the formatted string into the guest's buffer, at most size bytes with its terminator: the whole length */
static i32 to_guest(CPU* c, u64 dst, u64 size, Out* o, int n) {
    if (n >= 0 && size) {
        size_t k = (size_t)n < size - 1 ? (size_t)n : (size_t)size - 1;
        if (k) memcpy(S(dst), o->p, k);
        S(dst)[k] = 0;
    }
    free(o->p);
    return n;
}
#define OWN_CODE()                                                                                                     \
    do {                                                                                                               \
        hook_run_original(c, c->pc);                                                                                   \
        return (i32)c->x[0];                                                                                           \
    } while (0)

i32 libc_vsnprintf(CPU* c, u64 dst, u64 size, u64 fmt, u64 ap) {
    Args a;
    Out o = {0};
    if (!format_known(S(fmt))) OWN_CODE();
    args_va(&a, c, ap);
    int n = format(c, &o, S(fmt), &a);
    if (n < 0) libc_set_errno(c, EOVERFLOW);
    return to_guest(c, dst, size, &o, n);
}
i32 libc_snprintf(CPU* c, u64 dst, u64 size, u64 fmt) {
    Args a;
    Out o = {0};
    if (!format_known(S(fmt))) OWN_CODE();
    args_direct(&a, c, 3);
    return to_guest(c, dst, size, &o, format(c, &o, S(fmt), &a));
}
i32 libc_vsprintf(CPU* c, u64 dst, u64 fmt, u64 ap) {
    Args a;
    Out o = {0};
    if (!format_known(S(fmt))) OWN_CODE();
    args_va(&a, c, ap);
    return to_guest(c, dst, ~0ull, &o, format(c, &o, S(fmt), &a));
}
i32 libc_sprintf(CPU* c, u64 dst, u64 fmt) {
    Args a;
    Out o = {0};
    if (!format_known(S(fmt))) OWN_CODE();
    args_direct(&a, c, 2);
    return to_guest(c, dst, ~0ull, &o, format(c, &o, S(fmt), &a));
}

/* ---- the standard output: the log, a line at a time, each thread's line its own */
static _Thread_local Out g_line;
static void out_text(const char* s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') {
            LOG("[game] %s\n", g_line.p ? g_line.p : "");
            g_line.n = 0;
            if (g_line.p) g_line.p[0] = 0;
        } else
            out_put(&g_line, s + i, 1);
    }
}
i32 libc_vprintf(CPU* c, u64 fmt, u64 ap) {
    Args a;
    Out o = {0};
    if (!format_known(S(fmt))) OWN_CODE();
    args_va(&a, c, ap);
    int n = format(c, &o, S(fmt), &a);
    if (n > 0) out_text(o.p, (size_t)n);
    free(o.p);
    return n;
}
i32 libc_printf(CPU* c, u64 fmt) {
    Args a;
    Out o = {0};
    if (!format_known(S(fmt))) OWN_CODE();
    args_direct(&a, c, 1);
    int n = format(c, &o, S(fmt), &a);
    if (n > 0) out_text(o.p, (size_t)n);
    free(o.p);
    return n;
}
i32 libc_puts(CPU* c, u64 s) {
    size_t n = strlen(S(s));
    out_text(S(s), n);
    out_text("\n", 1);
    return (i32)(n + 1);
}
i32 libc_putchar(CPU* c, i32 ch) {
    (void)c;
    char b = (char)ch;
    out_text(&b, 1);
    return (u8)ch;
}
/* perror: the guest's errno's message, the host's words for it */
void libc_perror(CPU* c, u64 s) {
    i32 e = libc_get_errno(c);
    const char* msg = strerror(e);
    if (s && *S(s)) {
        out_text(S(s), strlen(S(s)));
        out_text(": ", 2);
    }
    out_text(msg, strlen(msg));
    out_text("\n", 1);
}

/* ---- sscanf: each directive the host's sscanf's, %n after it saying how far it read */
static int scan(CPU* c, const char* in, const char* f, Args* a) {
    size_t at = 0;
    int assigned = 0, converted = 0;
    while (*f) {
        if (isspace((unsigned char)*f)) { /* any whitespace, none included */
            while (isspace((unsigned char)*f)) f++;
            while (isspace((unsigned char)in[at])) at++;
            continue;
        }
        if (*f != '%' || f[1] == '%') { /* a character that must come next ('%%' after whitespace) */
            if (*f == '%') {
                f++;
                while (isspace((unsigned char)in[at])) at++;
            }
            if (!in[at]) return converted || assigned ? assigned : -1;
            if (in[at] != *f) return assigned;
            at++, f++;
            continue;
        }
        /* %[*][width][length]conv, conv one of diouxXaeEfFgGscp[n */
        const char* start = f++;
        int suppress = *f == '*';
        if (suppress) f++;
        while (isdigit((unsigned char)*f)) f++;
        char len[3] = {0};
        for (int k = 0; *f && strchr("hlLzjtq", *f) && k < 2; k++) len[k] = *f++;
        char conv = *f;
        if (!conv) break;
        if (conv == '[') { /* the set, a ']' first in it included */
            f++;
            if (*f == '^') f++;
            if (*f == ']') f++;
            while (*f && *f != ']') f++;
        }
        f++;
        char spec[300];
        size_t sl = (size_t)(f - start);
        if (sl > sizeof spec - 4) return assigned;
        memcpy(spec, start, sl);
        strcpy(spec + sl, "%n");
        int used = -1, r;
        if (conv == 'n') {
            if (!suppress) {
                u8* d = hook_host(c, arg_int(a));
                if (len[0] == 'l' || len[0] == 'z' || len[0] == 'j' || len[0] == 't' || len[0] == 'q')
                    snx_st_le64(d, at);
                else if (!strcmp(len, "hh"))
                    *d = (u8)at;
                else if (len[0] == 'h')
                    snx_st_le16(d, (u16)at);
                else
                    snx_st_le32(d, (u32)at);
            }
            continue;
        }
        if (!in[at] || (strchr("cn[", conv) == NULL && !in[at + strspn(in + at, " \t\n\v\f\r")]))
            return converted || assigned ? assigned : -1; /* the input ended before the conversion */
        if (suppress)
            r = sscanf(in + at, spec, &used);
        else if (len[0] == 'L' && strchr("aeEfFgG", conv)) { /* the guest's quad from the host's double */
            double d;
            char host[300];
            memcpy(host, start, sl);
            host[sl] = 0;
            char* L = strchr(host, 'L');
            if (L) *L = 'l';
            strcat(host, "%n");
            r = sscanf(in + at, host, &d, &used);
            if (r == 1 && used >= 0) {
                u64 lo, hi, p = arg_int(a);
                double_quad(d, &lo, &hi);
                snx_st_le64(hook_host(c, p), lo);
                snx_st_le64(hook_host(c, p + 8), hi);
            }
        } else /* straight through the guest's pointer: the same types, sizes and order */
            r = sscanf(in + at, spec, (void*)hook_host(c, arg_int(a)), &used);
        if (used < 0) return converted || assigned ? assigned : (r == EOF ? -1 : assigned);
        converted++;
        if (!suppress) assigned++;
        at += (size_t)used;
    }
    return assigned;
}
i32 libc_vsscanf(CPU* c, u64 s, u64 fmt, u64 ap) {
    Args a;
    args_va(&a, c, ap);
    return scan(c, S(s), S(fmt), &a);
}
i32 libc_sscanf(CPU* c, u64 s, u64 fmt) {
    Args a;
    args_direct(&a, c, 2);
    return scan(c, S(s), S(fmt), &a);
}
