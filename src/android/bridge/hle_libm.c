/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* bionic's libm as HLE (hle.h): the host's libm, called with the guest's registers by one adapter per type, and
   bionic's own where the host's differs or lacks a function. Its functions of long double, IEEE's quadruple precision
   on AArch64, which no host type holds, are not here */
#include "android/bridge/hle.h"
#include "android/bridge/hle_impl.h"
#include "common/utilities/ascii.h"
#include "common/utilities/bits.h"
#include <errno.h>
#include <fenv.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

static struct {
    int signgam; /* the sign of the last lgamma's gamma, bionic's object */
} g_hle_libm;

/* ---- the adapters ----
   By type: d for double, f for float, i for int, p for a pointer. bionic's libm never sets errno (its
   math_errhandling is MATH_ERREXCEPT), where the host's does (glibc's log(0) sets ERANGE): each call keeps the
   guest's */
#define KEEP_ERRNO(call)                                                                                               \
    do {                                                                                                               \
        const int kept = errno;                                                                                        \
        call;                                                                                                          \
        errno = kept;                                                                                                  \
    } while (0)

static void d_d(CPU* c, void* fn) { KEEP_ERRNO(hle_set_d(c, ((double (*)(double))fn)(hle_d(c, 0)))); }
static void d_dd(CPU* c, void* fn) {
    KEEP_ERRNO(hle_set_d(c, ((double (*)(double, double))fn)(hle_d(c, 0), hle_d(c, 1))));
}
static void f_f(CPU* c, void* fn) { KEEP_ERRNO(hle_set_f(c, ((float (*)(float))fn)(hle_f(c, 0)))); }
static void f_ff(CPU* c, void* fn) {
    KEEP_ERRNO(hle_set_f(c, ((float (*)(float, float))fn)(hle_f(c, 0), hle_f(c, 1))));
}
static void d_ddd(CPU* c, void* fn) {
    KEEP_ERRNO(hle_set_d(c, ((double (*)(double, double, double))fn)(hle_d(c, 0), hle_d(c, 1), hle_d(c, 2))));
}
static void f_fff(CPU* c, void* fn) {
    KEEP_ERRNO(hle_set_f(c, ((float (*)(float, float, float))fn)(hle_f(c, 0), hle_f(c, 1), hle_f(c, 2))));
}
/* lround and llround: bionic's long is 64 bits as long long is, which Windows' long is not */
static void l_d(CPU* c, void* fn) {
    (void)fn;
    KEEP_ERRNO(X(0) = (uint64_t)llround(hle_d(c, 0)));
}
static void l_f(CPU* c, void* fn) {
    (void)fn;
    KEEP_ERRNO(X(0) = (uint64_t)llroundf(hle_f(c, 0)));
}
/* an int argument, w as the guest passes it */
static int arg_int(const CPU* c, unsigned n) { return snx_sext32((uint32_t)c->x[n], 32); }
/* a long argument, 64 bits in bionic, as an int: scalbn's exponent past an int's range scales any number to 0 or to
   infinity, as one at its end does */
static int arg_long_int(const CPU* c, unsigned n) {
    const int64_t v = snx_sext64(c->x[n], 64);
    return v < INT_MIN ? INT_MIN : v > INT_MAX ? INT_MAX : (int)v;
}
static void d_di(CPU* c, void* fn) {
    KEEP_ERRNO(hle_set_d(c, ((double (*)(double, int))fn)(hle_d(c, 0), arg_int(c, 0))));
}
static void f_fi(CPU* c, void* fn) {
    KEEP_ERRNO(hle_set_f(c, ((float (*)(float, int))fn)(hle_f(c, 0), arg_int(c, 0))));
}
static void d_dl(CPU* c, void* fn) {
    KEEP_ERRNO(hle_set_d(c, ((double (*)(double, int))fn)(hle_d(c, 0), arg_long_int(c, 0))));
}
static void f_fl(CPU* c, void* fn) {
    KEEP_ERRNO(hle_set_f(c, ((float (*)(float, int))fn)(hle_f(c, 0), arg_long_int(c, 0))));
}
static void d_id(CPU* c, void* fn) {
    KEEP_ERRNO(hle_set_d(c, ((double (*)(int, double))fn)(arg_int(c, 0), hle_d(c, 0))));
}
/* a float through the host's function of double: the float Bessel functions, which not every host's libm has */
static void f_d(CPU* c, void* fn) { KEEP_ERRNO(hle_set_f(c, (float)((double (*)(double))fn)(hle_f(c, 0)))); }
static void f_id(CPU* c, void* fn) {
    KEEP_ERRNO(hle_set_f(c, (float)((double (*)(int, double))fn)(arg_int(c, 0), hle_f(c, 0))));
}
static void d_dp(CPU* c, void* fn) { KEEP_ERRNO(hle_set_d(c, ((double (*)(double, void*))fn)(hle_d(c, 0), PTR(0)))); }
static void f_fp(CPU* c, void* fn) { KEEP_ERRNO(hle_set_f(c, ((float (*)(float, void*))fn)(hle_f(c, 0), PTR(0)))); }
static void sincos_d(CPU* c, void* fn) {
    (void)fn;
    const int kept = errno;
    double x = hle_d(c, 0), s = sin(x), co = cos(x);
    errno = kept;
    memcpy(PTR(0), &s, sizeof s);
    memcpy(PTR(1), &co, sizeof co);
}
static void sincos_f(CPU* c, void* fn) {
    (void)fn;
    const int kept = errno;
    float x = hle_f(c, 0), s = sinf(x), co = cosf(x);
    errno = kept;
    memcpy(PTR(0), &s, sizeof s);
    memcpy(PTR(1), &co, sizeof co);
}

/* ---- bionic's own ----
   Where bionic's libm, FreeBSD's msun, gives other results than the host's, or the host lacks the function */

/* ilogb: FP_ILOGB0 is -INT_MAX and FP_ILOGBNAN INT_MAX in bionic, where glibc's on x86 are INT_MIN; infinity INT_MAX */
static int ilogb_of(double x) {
    if (x == 0) return -INT_MAX;
    if (!isfinite(x)) return INT_MAX;
    const int kept = errno;
    const int e = ilogb(x);
    errno = kept;
    return e;
}
static void ilogb_d(CPU* c, void* fn) {
    (void)fn;
    X(0) = (uint32_t)ilogb_of(hle_d(c, 0));
}
static void ilogb_f(CPU* c, void* fn) {
    (void)fn;
    X(0) = (uint32_t)ilogb_of(hle_f(c, 0));
}
static void finite_d(CPU* c, void* fn) {
    (void)fn;
    X(0) = isfinite(hle_d(c, 0)) ? 1 : 0;
}
static void finite_f(CPU* c, void* fn) {
    (void)fn;
    X(0) = isfinite(hle_f(c, 0)) ? 1 : 0;
}
static void signbit_d(CPU* c, void* fn) {
    (void)fn;
    X(0) = signbit(hle_d(c, 0)) ? 1 : 0;
}
static void signbit_f(CPU* c, void* fn) {
    (void)fn;
    X(0) = signbit(hle_f(c, 0)) ? 1 : 0;
}

/* scalb, x times 2 to a double n, as msun's: a NaN in either is NaN, an infinite n scales to infinity or to 0, a
   fractional one is invalid, and past 65000 any number overflows or underflows */
static double scalb_of(double x, double n) {
    if (isnan(x) || isnan(n)) return x * n;
    if (isinf(n)) return n > 0 ? x * n : x / -n;
    if (rint(n) != n) return NAN;
    if (n > 65000.0) return scalbn(x, 65000);
    if (n < -65000.0) return scalbn(x, -65000);
    return scalbn(x, (int)n);
}
static void scalb_d(CPU* c, void* fn) {
    (void)fn;
    KEEP_ERRNO(hle_set_d(c, scalb_of(hle_d(c, 0), hle_d(c, 1))));
}
static void scalb_f(CPU* c, void* fn) {
    (void)fn;
    KEEP_ERRNO(hle_set_f(c, (float)scalb_of(hle_f(c, 0), hle_f(c, 1))));
}
/* significand, x scaled into [1, 2): msun's scalb of x by its ilogb, so 0, infinity and NaN stay */
static void significand_d(CPU* c, void* fn) {
    (void)fn;
    const double x = hle_d(c, 0);
    KEEP_ERRNO(hle_set_d(c, scalb_of(x, -(double)ilogb_of(x))));
}
static void significand_f(CPU* c, void* fn) {
    (void)fn;
    const double x = hle_f(c, 0);
    KEEP_ERRNO(hle_set_f(c, (float)scalb_of(x, -(double)ilogb_of(x))));
}

/* remquo's quotient as bionic gives it, the sign of x / y and the low 31 bits of the integer nearest to it, ties to
   even, where glibc's keeps 3 bits. |x| modulo 2^31 |y| keeps those bits and is exact; its quotient by |y| is then
   below 2^32, which a double gives to well within a half */
static int32_t remquo_bits(double x, double y) {
    const double ay = fabs(y), span = 0x1p31 * ay;
    const double m = isinf(span) ? fabs(x) : fmod(fabs(x), span);
    const int32_t q = (int32_t)((uint64_t)nearbyint((m - remainder(m, ay)) / ay) & 0x7fffffffu);
    return (signbit(x) != 0) != (signbit(y) != 0) ? -q : q;
}
/* the remainder, and the quotient's bits into the guest's int; for a NaN (y 0, x infinite or a NaN given) bionic
   writes none */
static void remquo_d(CPU* c, void* fn) {
    (void)fn;
    const int kept = errno;
    const double x = hle_d(c, 0), y = hle_d(c, 1), r = remainder(x, y);
    if (!isnan(r)) {
        const int32_t q = remquo_bits(x, y);
        memcpy(PTR(0), &q, sizeof q);
    }
    errno = kept;
    hle_set_d(c, r);
}
static void remquo_f(CPU* c, void* fn) {
    (void)fn;
    const int kept = errno;
    const float x = hle_f(c, 0), y = hle_f(c, 1), r = remainderf(x, y);
    if (!isnan(r)) {
        const int32_t q = remquo_bits(x, y);
        memcpy(PTR(0), &q, sizeof q);
    }
    errno = kept;
    hle_set_f(c, r);
}

/* lgamma_r's sign of gamma(x), as msun gives it: 1 for a NaN, the infinities, a positive x and a pole at a negative
   integer; a zero's own sign; and between two negative integers, -1 above an odd one */
static int32_t gamma_sign(double x) {
    if (x == 0) return signbit(x) ? -1 : 1;
    if (!(x < 0) || isinf(x) || x == floor(x)) return 1;
    return fmod(floor(x), 2.0) == 0 ? 1 : -1;
}
/* lgamma_r and gamma_r: the sign into the guest's int; lgamma and gamma: into signgam, bionic's object */
static void lgamma_r_d(CPU* c, void* fn) {
    (void)fn;
    const double x = hle_d(c, 0);
    const int32_t sign = gamma_sign(x);
    KEEP_ERRNO(hle_set_d(c, lgamma(x)));
    memcpy(PTR(0), &sign, sizeof sign);
}
static void lgamma_r_f(CPU* c, void* fn) {
    (void)fn;
    const float x = hle_f(c, 0);
    const int32_t sign = gamma_sign(x);
    KEEP_ERRNO(hle_set_f(c, lgammaf(x)));
    memcpy(PTR(0), &sign, sizeof sign);
}
static void lgamma_d(CPU* c, void* fn) {
    (void)fn;
    const double x = hle_d(c, 0);
    g_hle_libm.signgam = gamma_sign(x);
    KEEP_ERRNO(hle_set_d(c, lgamma(x)));
}
static void lgamma_f(CPU* c, void* fn) {
    (void)fn;
    const float x = hle_f(c, 0);
    g_hle_libm.signgam = gamma_sign(x);
    KEEP_ERRNO(hle_set_f(c, lgammaf(x)));
}

/* nan and nanf as msun's: the hexadecimal digits of the tag, after a 0x, fill the low bits from its last digit up,
   and a quiet NaN's bits go over them */
static uint64_t nan_payload(const char* tag, unsigned bits) {
    if (tag[0] == '0' && (tag[1] == 'x' || tag[1] == 'X')) tag += 2;
    size_t n = 0;
    while (snx_is_hex(tag[n])) ++n;
    uint64_t payload = 0;
    for (unsigned at = 0; at < bits && n > 0; at += 4) payload |= (uint64_t)snx_hex_value(tag[--n]) << at;
    return payload;
}
static void nan_d(CPU* c, void* fn) {
    (void)fn;
    const uint64_t bits = nan_payload(STR(0), 64) | 0x7ff8000000000000ull;
    c->v[0] = (V128){0};
    memcpy(&c->v[0].df[0], &bits, sizeof bits);
}
static void nan_f(CPU* c, void* fn) {
    (void)fn;
    const uint32_t bits = (uint32_t)nan_payload(STR(0), 32) | 0x7fc00000u;
    c->v[0] = (V128){0};
    memcpy(&c->v[0].f[0], &bits, sizeof bits);
}

/* rint, nearbyint, lrint and llrint round as the guest's FPCR.RMode says, as FRINTX and FRINTI do: to nearest with
   ties to even (the host's own mode, which generated code keeps), up, down or toward zero */
static double integral(double x, uint32_t fpcr) {
    switch (fpcr >> 22 & 3u) {
        case 1: return ceil(x);
        case 2: return floor(x);
        case 3: return trunc(x);
        default: return nearbyint(x);
    }
}
enum { FPSR_IOC = 0x01, FPSR_IXC = 0x10 }; /* Invalid Operation and Inexact, FPSR's cumulative flags */
/* a NaN whose quiet bit is clear */
static bool signaling(double x) {
    uint64_t bits;
    memcpy(&bits, &x, sizeof bits);
    return isnan(x) && !(bits >> 51 & 1);
}
static bool signaling_f(float x) {
    uint32_t bits;
    memcpy(&bits, &x, sizeof bits);
    return isnan(x) && !(bits >> 22 & 1);
}
/* FRINTX's flags: Invalid Operation for a signaling NaN, Inexact for a result other than x */
static uint32_t rounded_flags(double x, double r, bool snan) {
    return snan ? FPSR_IOC : !isnan(x) && r != x ? FPSR_IXC : 0;
}
static void rint_d(CPU* c, void* fn) {
    (void)fn;
    const double x = hle_d(c, 0), r = integral(x, c->fpcr);
    c->fpsr |= rounded_flags(x, r, signaling(x));
    hle_set_d(c, r);
}
static void rint_f(CPU* c, void* fn) {
    (void)fn;
    const float x = hle_f(c, 0);
    const double r = integral(x, c->fpcr);
    c->fpsr |= rounded_flags(x, r, signaling_f(x));
    hle_set_f(c, (float)r);
}
/* nearbyint: msun's keeps the floating-point environment around rint, so no flag is raised */
static void nearbyint_d(CPU* c, void* fn) {
    (void)fn;
    hle_set_d(c, integral(hle_d(c, 0), c->fpcr));
}
static void nearbyint_f(CPU* c, void* fn) {
    (void)fn;
    hle_set_f(c, (float)integral(hle_f(c, 0), c->fpcr));
}
/* FCVTZS or FCVTAS to 64 bits of r, x rounded to an integral value: a NaN is 0 and a value out of range the nearest
   end, with Invalid Operation; Inexact when r is not x. The host's conversion gives INT64_MIN for all three */
static uint64_t fcvt64(CPU* c, double x, double r) {
    if (isnan(r) || !(r >= -0x1p63 && r < 0x1p63)) {
        c->fpsr |= FPSR_IOC;
        return isnan(r) ? 0 : r > 0 ? (uint64_t)INT64_MAX : (uint64_t)1 << 63;
    }
    if (r != x) c->fpsr |= FPSR_IXC;
    return (uint64_t)(int64_t)r;
}
/* lrint and llrint, FRINTX then FCVTZS: bionic's long is 64 bits as long long is */
static void lrint_d(CPU* c, void* fn) {
    (void)fn;
    const double x = hle_d(c, 0);
    X(0) = fcvt64(c, x, integral(x, c->fpcr));
}
static void lrint_f(CPU* c, void* fn) {
    (void)fn;
    const double x = hle_f(c, 0);
    X(0) = fcvt64(c, x, integral(x, c->fpcr));
}

/* a host function through an adapter (TH: the host's of another name), a handler of its own, an object, and the
   table's end */
#define T(name, adapter)      {#name, adapter, (void*)name}
#define TH(name, adapter, fn) {#name, adapter, (void*)fn}
#define P(name)               {#name, hle_pass, (void*)name}
#define H(name, handler)      {#name, handler, NULL}
#define D(name, object)       {#name, NULL, object}
#define END                   {"", NULL, NULL}

/* bionic's Bessel functions are the CRT's _j0 and the others on Windows, whose j0 is deprecated */
#ifdef _WIN32
#define BESSEL(name) _##name
#else
#define BESSEL(name) name
#endif

const HleSymbol hle_libm[] = {
    /* the host's, of doubles */
    T(acos, d_d),
    T(asin, d_d),
    T(atan, d_d),
    T(cos, d_d),
    T(cosh, d_d),
    T(exp, d_d),
    T(exp2, d_d),
    T(log, d_d),
    T(log10, d_d),
    T(log2, d_d),
    T(sin, d_d),
    T(sinh, d_d),
    T(sqrt, d_d),
    T(tan, d_d),
    T(tanh, d_d),
    T(cbrt, d_d),
    T(expm1, d_d),
    T(log1p, d_d),
    T(acosh, d_d),
    T(asinh, d_d),
    T(atanh, d_d),
    T(erf, d_d),
    T(erfc, d_d),
    T(tgamma, d_d),
    T(ceil, d_d),
    T(floor, d_d),
    T(trunc, d_d),
    T(round, d_d),
    T(fabs, d_d),
    T(logb, d_d),
    T(atan2, d_dd),
    T(fmod, d_dd),
    T(hypot, d_dd),
    T(pow, d_dd),
    T(nextafter, d_dd),
    T(fmax, d_dd),
    T(fmin, d_dd),
    T(copysign, d_dd),
    T(fdim, d_dd),
    T(remainder, d_dd),
    TH(drem, d_dd, remainder),
    T(fma, d_ddd),
    T(ldexp, d_di),
    T(scalbn, d_di),
    TH(scalbln, d_dl, scalbn),
    T(frexp, d_dp),
    T(modf, d_dp),
    T(lround, l_d),
    T(llround, l_d),
    TH(j0, d_d, BESSEL(j0)),
    TH(j1, d_d, BESSEL(j1)),
    TH(jn, d_id, BESSEL(jn)),
    TH(y0, d_d, BESSEL(y0)),
    TH(y1, d_d, BESSEL(y1)),
    TH(yn, d_id, BESSEL(yn)),
    /* the host's, of floats */
    T(acosf, f_f),
    T(asinf, f_f),
    T(atanf, f_f),
    T(cosf, f_f),
    T(coshf, f_f),
    T(expf, f_f),
    T(exp2f, f_f),
    T(logf, f_f),
    T(log10f, f_f),
    T(log2f, f_f),
    T(sinf, f_f),
    T(sinhf, f_f),
    T(sqrtf, f_f),
    T(tanf, f_f),
    T(tanhf, f_f),
    T(cbrtf, f_f),
    T(expm1f, f_f),
    T(log1pf, f_f),
    T(acoshf, f_f),
    T(asinhf, f_f),
    T(atanhf, f_f),
    T(erff, f_f),
    T(erfcf, f_f),
    T(tgammaf, f_f),
    T(ceilf, f_f),
    T(floorf, f_f),
    T(truncf, f_f),
    T(roundf, f_f),
    T(fabsf, f_f),
    T(logbf, f_f),
    T(atan2f, f_ff),
    T(fmodf, f_ff),
    T(hypotf, f_ff),
    T(powf, f_ff),
    T(nextafterf, f_ff),
    T(fmaxf, f_ff),
    T(fminf, f_ff),
    T(copysignf, f_ff),
    T(fdimf, f_ff),
    T(remainderf, f_ff),
    TH(dremf, f_ff, remainderf),
    T(fmaf, f_fff),
    T(ldexpf, f_fi),
    T(scalbnf, f_fi),
    TH(scalblnf, f_fl, scalbnf),
    T(frexpf, f_fp),
    T(modff, f_fp),
    T(lroundf, l_f),
    T(llroundf, l_f),
    TH(j0f, f_d, BESSEL(j0)),
    TH(j1f, f_d, BESSEL(j1)),
    TH(jnf, f_id, BESSEL(jn)),
    TH(y0f, f_d, BESSEL(y0)),
    TH(y1f, f_d, BESSEL(y1)),
    TH(ynf, f_id, BESSEL(yn)),
    /* bionic's own */
    H(sincos, sincos_d),
    H(sincosf, sincos_f),
    H(ilogb, ilogb_d),
    H(ilogbf, ilogb_f),
    H(finite, finite_d),
    H(finitef, finite_f),
    H(__signbit, signbit_d),
    H(__signbitf, signbit_f),
    H(scalb, scalb_d),
    H(scalbf, scalb_f),
    H(significand, significand_d),
    H(significandf, significand_f),
    H(remquo, remquo_d),
    H(remquof, remquo_f),
    H(lgamma, lgamma_d),
    H(lgammaf, lgamma_f),
    H(gamma, lgamma_d),
    H(gammaf, lgamma_f),
    H(lgamma_r, lgamma_r_d),
    H(lgammaf_r, lgamma_r_f),
    H(gamma_r, lgamma_r_d),
    H(gammaf_r, lgamma_r_f),
    D(signgam, &g_hle_libm.signgam),
    H(nan, nan_d),
    H(nanf, nan_f),
    H(rint, rint_d),
    H(rintf, rint_f),
    H(nearbyint, nearbyint_d),
    H(nearbyintf, nearbyint_f),
    H(lrint, lrint_d),
    H(llrint, lrint_d),
    H(lrintf, lrint_f),
    H(llrintf, lrint_f),
    P(feclearexcept),
    P(fetestexcept),
    END,
};
