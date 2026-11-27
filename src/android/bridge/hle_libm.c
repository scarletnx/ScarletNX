/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* bionic's libm as HLE (hle.h): the host's libm, called with the guest's registers by one adapter per type, and
   bionic's own where the host's differs or lacks a function. Its functions of long double, IEEE's quadruple precision
   on AArch64, which no host type holds, are not here */
#include "android/bridge/hle.h"
#include "android/bridge/hle_impl.h"
#include "common/utilities/ascii.h"
#include "common/utilities/bits.h"
#include <complex.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

static struct {
    int signgam; /* the sign of the last lgamma's gamma, bionic's object */
} g_hle_libm;

/* ---- the adapters ----
   By type: d for double, f for float, i for int, l for long, p for a pointer. Each gives the host's result as bionic's
   would be. bionic's libm never sets errno (its math_errhandling is MATH_ERREXCEPT), where the host's does (glibc's
   log(0) sets ERANGE): each call keeps the guest's. A NaN the host made from numbers (sqrt(-1), log(-1)) is AArch64's
   default NaN, whose sign is clear, where x86's is set: an app's printf says "nan", not "-nan". A NaN given goes
   through as the host's arithmetic took it */
static void put_d(CPU* c, double r, bool nan_given, int kept) {
    errno = kept;
    hle_set_d(c, isnan(r) && !nan_given ? (double)NAN : r);
}
static void put_f(CPU* c, float r, bool nan_given, int kept) {
    errno = kept;
    hle_set_f(c, isnan(r) && !nan_given ? NAN : r);
}
/* an int argument, w as the guest passes it */
static int arg_int(const CPU* c, unsigned n) { return snx_sext32((uint32_t)c->x[n], 32); }
/* a long argument, 64 bits in bionic, as an int: scalbn's exponent past an int's range scales any number to 0 or to
   infinity, as one at its end does */
static int arg_long_int(const CPU* c, unsigned n) {
    const int64_t v = snx_sext64(c->x[n], 64);
    return v < INT_MIN ? INT_MIN : v > INT_MAX ? INT_MAX : (int)v;
}

static void d_d(CPU* c, void* fn) {
    const int kept = errno;
    const double x = hle_d(c, 0);
    put_d(c, ((double (*)(double))fn)(x), isnan(x), kept);
}
static void d_dd(CPU* c, void* fn) {
    const int kept = errno;
    const double x = hle_d(c, 0), y = hle_d(c, 1);
    put_d(c, ((double (*)(double, double))fn)(x, y), isnan(x) || isnan(y), kept);
}
static void d_ddd(CPU* c, void* fn) {
    const int kept = errno;
    const double x = hle_d(c, 0), y = hle_d(c, 1), z = hle_d(c, 2);
    put_d(c, ((double (*)(double, double, double))fn)(x, y, z), isnan(x) || isnan(y) || isnan(z), kept);
}
static void d_di(CPU* c, void* fn) {
    const int kept = errno;
    const double x = hle_d(c, 0);
    put_d(c, ((double (*)(double, int))fn)(x, arg_int(c, 0)), isnan(x), kept);
}
static void d_dl(CPU* c, void* fn) {
    const int kept = errno;
    const double x = hle_d(c, 0);
    put_d(c, ((double (*)(double, int))fn)(x, arg_long_int(c, 0)), isnan(x), kept);
}
static void d_id(CPU* c, void* fn) {
    const int kept = errno;
    const double x = hle_d(c, 0);
    put_d(c, ((double (*)(int, double))fn)(arg_int(c, 0), x), isnan(x), kept);
}
static void d_dp(CPU* c, void* fn) {
    const int kept = errno;
    const double x = hle_d(c, 0);
    put_d(c, ((double (*)(double, void*))fn)(x, PTR(0)), isnan(x), kept);
}
static void f_f(CPU* c, void* fn) {
    const int kept = errno;
    const float x = hle_f(c, 0);
    put_f(c, ((float (*)(float))fn)(x), isnan(x), kept);
}
static void f_ff(CPU* c, void* fn) {
    const int kept = errno;
    const float x = hle_f(c, 0), y = hle_f(c, 1);
    put_f(c, ((float (*)(float, float))fn)(x, y), isnan(x) || isnan(y), kept);
}
static void f_fff(CPU* c, void* fn) {
    const int kept = errno;
    const float x = hle_f(c, 0), y = hle_f(c, 1), z = hle_f(c, 2);
    put_f(c, ((float (*)(float, float, float))fn)(x, y, z), isnan(x) || isnan(y) || isnan(z), kept);
}
static void f_fi(CPU* c, void* fn) {
    const int kept = errno;
    const float x = hle_f(c, 0);
    put_f(c, ((float (*)(float, int))fn)(x, arg_int(c, 0)), isnan(x), kept);
}
static void f_fl(CPU* c, void* fn) {
    const int kept = errno;
    const float x = hle_f(c, 0);
    put_f(c, ((float (*)(float, int))fn)(x, arg_long_int(c, 0)), isnan(x), kept);
}
static void f_fp(CPU* c, void* fn) {
    const int kept = errno;
    const float x = hle_f(c, 0);
    put_f(c, ((float (*)(float, void*))fn)(x, PTR(0)), isnan(x), kept);
}
/* a float through the host's function of double: the float Bessel functions, which not every host's libm has */
static void f_d(CPU* c, void* fn) {
    const int kept = errno;
    const float x = hle_f(c, 0);
    put_f(c, (float)((double (*)(double))fn)(x), isnan(x), kept);
}
static void f_id(CPU* c, void* fn) {
    const int kept = errno;
    const float x = hle_f(c, 0);
    put_f(c, (float)((double (*)(int, double))fn)(arg_int(c, 0), x), isnan(x), kept);
}
/* a result the host made, as put_d gives it: the default NaN for one made from numbers */
static double made_d(double r, bool nan_given) { return isnan(r) && !nan_given ? (double)NAN : r; }
static void sincos_d(CPU* c, void* fn) {
    (void)fn;
    const int kept = errno;
    const double x = hle_d(c, 0), s = made_d(sin(x), isnan(x)), co = made_d(cos(x), isnan(x));
    errno = kept;
    memcpy(PTR(0), &s, sizeof s);
    memcpy(PTR(1), &co, sizeof co);
}
static void sincos_f(CPU* c, void* fn) {
    (void)fn;
    const int kept = errno;
    const float x = hle_f(c, 0), s = (float)made_d(sinf(x), isnan(x)), co = (float)made_d(cosf(x), isnan(x));
    errno = kept;
    memcpy(PTR(0), &s, sizeof s);
    memcpy(PTR(1), &co, sizeof co);
}

/* ---- complex numbers ----
   A complex argument is two floating registers, its real part first, as AAPCS64 passes a homogeneous pair, and so is
   its result: z for a double complex, c for a float complex. A part the host made a NaN from numbers is the default
   NaN, as a real function's (put_d) */
static double complex arg_z(const CPU* c, unsigned n) {
    const double parts[2] = {hle_d(c, n), hle_d(c, n + 1)};
    double complex z;
    memcpy(&z, parts, sizeof z);
    return z;
}
static float complex arg_c(const CPU* c, unsigned n) {
    const float parts[2] = {hle_f(c, n), hle_f(c, n + 1)};
    float complex z;
    memcpy(&z, parts, sizeof z);
    return z;
}
static bool nan_z(double complex z) { return isnan(creal(z)) || isnan(cimag(z)); }
static void put_z(CPU* c, double complex z, bool nan_given, int kept) {
    errno = kept;
    const double parts[2] = {made_d(creal(z), nan_given), made_d(cimag(z), nan_given)};
    c->v[0] = (V128){0};
    c->v[1] = (V128){0};
    memcpy(&c->v[0].df[0], &parts[0], sizeof parts[0]);
    memcpy(&c->v[1].df[0], &parts[1], sizeof parts[1]);
}
static void put_c(CPU* c, float complex z, bool nan_given, int kept) {
    errno = kept;
    const float parts[2] = {(float)made_d(crealf(z), nan_given), (float)made_d(cimagf(z), nan_given)};
    c->v[0] = (V128){0};
    c->v[1] = (V128){0};
    memcpy(&c->v[0].f[0], &parts[0], sizeof parts[0]);
    memcpy(&c->v[1].f[0], &parts[1], sizeof parts[1]);
}
static void z_z(CPU* c, void* fn) {
    const int kept = errno;
    const double complex z = arg_z(c, 0);
    put_z(c, ((double complex (*)(double complex))fn)(z), nan_z(z), kept);
}
static void z_zz(CPU* c, void* fn) {
    const int kept = errno;
    const double complex z = arg_z(c, 0), w = arg_z(c, 2);
    put_z(c, ((double complex (*)(double complex, double complex))fn)(z, w), nan_z(z) || nan_z(w), kept);
}
static void d_z(CPU* c, void* fn) {
    const int kept = errno;
    const double complex z = arg_z(c, 0);
    put_d(c, ((double (*)(double complex))fn)(z), nan_z(z), kept);
}
static void c_c(CPU* c, void* fn) {
    const int kept = errno;
    const float complex z = arg_c(c, 0);
    put_c(c, ((float complex (*)(float complex))fn)(z), nan_z(z), kept);
}
static void c_cc(CPU* c, void* fn) {
    const int kept = errno;
    const float complex z = arg_c(c, 0), w = arg_c(c, 2);
    put_c(c, ((float complex (*)(float complex, float complex))fn)(z, w), nan_z(z) || nan_z(w), kept);
}
static void f_c(CPU* c, void* fn) {
    const int kept = errno;
    const float complex z = arg_c(c, 0);
    put_f(c, ((float (*)(float complex))fn)(z), nan_z(z), kept);
}
/* cabs and cabsf, msun's: the hypotenuse of the two parts (Windows' C library marks its cabs deprecated) */
static void cabs_d(CPU* c, void* fn) {
    (void)fn;
    const int kept = errno;
    const double x = hle_d(c, 0), y = hle_d(c, 1);
    put_d(c, hypot(x, y), isnan(x) || isnan(y), kept);
}
static void cabs_f(CPU* c, void* fn) {
    (void)fn;
    const int kept = errno;
    const float x = hle_f(c, 0), y = hle_f(c, 1);
    put_f(c, hypotf(x, y), isnan(x) || isnan(y), kept);
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
    const int kept = errno;
    const double x = hle_d(c, 0), n = hle_d(c, 1);
    put_d(c, scalb_of(x, n), isnan(x) || isnan(n), kept);
}
static void scalb_f(CPU* c, void* fn) {
    (void)fn;
    const int kept = errno;
    const float x = hle_f(c, 0), n = hle_f(c, 1);
    put_f(c, (float)scalb_of(x, n), isnan(x) || isnan(n), kept);
}
/* significand, x scaled into [1, 2): msun's scalb of x by its ilogb, so 0, infinity and NaN stay */
static void significand_d(CPU* c, void* fn) {
    (void)fn;
    const int kept = errno;
    const double x = hle_d(c, 0);
    put_d(c, scalb_of(x, -(double)ilogb_of(x)), isnan(x), kept);
}
static void significand_f(CPU* c, void* fn) {
    (void)fn;
    const int kept = errno;
    const float x = hle_f(c, 0);
    put_f(c, (float)scalb_of(x, -(double)ilogb_of(x)), isnan(x), kept);
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
    put_d(c, r, isnan(x) || isnan(y), kept);
}
static void remquo_f(CPU* c, void* fn) {
    (void)fn;
    const int kept = errno;
    const float x = hle_f(c, 0), y = hle_f(c, 1), r = remainderf(x, y);
    if (!isnan(r)) {
        const int32_t q = remquo_bits(x, y);
        memcpy(PTR(0), &q, sizeof q);
    }
    put_f(c, r, isnan(x) || isnan(y), kept);
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
    const int kept = errno;
    const double x = hle_d(c, 0);
    const int32_t sign = gamma_sign(x);
    memcpy(PTR(0), &sign, sizeof sign);
    put_d(c, lgamma(x), isnan(x), kept);
}
static void lgamma_r_f(CPU* c, void* fn) {
    (void)fn;
    const int kept = errno;
    const float x = hle_f(c, 0);
    const int32_t sign = gamma_sign(x);
    memcpy(PTR(0), &sign, sizeof sign);
    put_f(c, lgammaf(x), isnan(x), kept);
}
static void lgamma_d(CPU* c, void* fn) {
    (void)fn;
    const int kept = errno;
    const double x = hle_d(c, 0);
    g_hle_libm.signgam = gamma_sign(x);
    put_d(c, lgamma(x), isnan(x), kept);
}
static void lgamma_f(CPU* c, void* fn) {
    (void)fn;
    const int kept = errno;
    const float x = hle_f(c, 0);
    g_hle_libm.signgam = gamma_sign(x);
    put_f(c, lgammaf(x), isnan(x), kept);
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
/* lround and llround, FCVTAS: to nearest with ties away from zero */
static void lround_d(CPU* c, void* fn) {
    (void)fn;
    const double x = hle_d(c, 0);
    X(0) = fcvt64(c, x, round(x));
}
static void lround_f(CPU* c, void* fn) {
    (void)fn;
    const double x = hle_f(c, 0);
    X(0) = fcvt64(c, x, round(x));
}

/* fmax and fmin, FMAXNM and FMINNM: a quiet NaN gives way to a number, a signaling one is the result, quiet, with
   Invalid Operation, and +0 is above -0, where the host's may give either zero */
static double max_min(CPU* c, double x, double y, bool snan_x, bool snan_y, bool max) {
    if (snan_x || snan_y) {
        c->fpsr |= FPSR_IOC;
        uint64_t bits;
        memcpy(&bits, snan_x ? &x : &y, sizeof bits);
        bits |= 1ull << 51; /* quiet: a float's came here quiet already */
        double r;
        memcpy(&r, &bits, sizeof r);
        return r;
    }
    if (isnan(x) || isnan(y)) return isnan(y) ? x : y; /* of two quiet NaNs the first */
    if (x == y) return (signbit(x) != 0) == max ? y : x;
    return (x > y) == max ? x : y;
}
static void fmax_d(CPU* c, void* fn) {
    (void)fn;
    const double x = hle_d(c, 0), y = hle_d(c, 1);
    hle_set_d(c, max_min(c, x, y, signaling(x), signaling(y), true));
}
static void fmin_d(CPU* c, void* fn) {
    (void)fn;
    const double x = hle_d(c, 0), y = hle_d(c, 1);
    hle_set_d(c, max_min(c, x, y, signaling(x), signaling(y), false));
}
static void fmax_f(CPU* c, void* fn) {
    (void)fn;
    const float x = hle_f(c, 0), y = hle_f(c, 1);
    hle_set_f(c, (float)max_min(c, x, y, signaling_f(x), signaling_f(y), true));
}
static void fmin_f(CPU* c, void* fn) {
    (void)fn;
    const float x = hle_f(c, 0), y = hle_f(c, 1);
    hle_set_f(c, (float)max_min(c, x, y, signaling_f(x), signaling_f(y), false));
}

/* ---- fenv.h: the guest's FPCR and FPSR, as bionic's reads and writes the registers ----
   Its exceptions are FPSR's cumulative flags (FE_INVALID 0x01 to FE_INEXACT 0x10, FE_DENORMAL 0x80), its rounding
   modes FPCR.RMode's values (FE_TONEAREST 0 to FE_TOWARDZERO 3), and its fenv_t the two registers. The host's numbers
   are other ones, and its registers not the guest's */
enum { FE_ALL = 0x9f, RMODE_SHIFT = 22 };
static const uint32_t fe_dfl_env[2] = {0, 0}; /* __fe_dfl_env: FPCR 0, FPSR 0 */

static void fegetenv_(CPU* c, void* fn) {
    (void)fn;
    const uint32_t env[2] = {c->fpcr, c->fpsr};
    memcpy(PTR(0), env, sizeof env);
    X(0) = 0;
}
static void set_env(CPU* c, const void* guest) {
    uint32_t env[2];
    memcpy(env, guest, sizeof env);
    c->fpcr = env[0];
    c->fpsr = env[1];
}
static void fesetenv_(CPU* c, void* fn) {
    (void)fn;
    set_env(c, PTR(0));
    X(0) = 0;
}
/* feholdexcept: the environment saved, the flags cleared */
static void feholdexcept_(CPU* c, void* fn) {
    fegetenv_(c, fn);
    c->fpsr &= ~(uint32_t)FE_ALL;
}
/* feupdateenv: the environment set, with the flags raised so far kept */
static void feupdateenv_(CPU* c, void* fn) {
    (void)fn;
    const uint32_t raised = c->fpsr & FE_ALL;
    set_env(c, PTR(0));
    c->fpsr |= raised;
    X(0) = 0;
}
static void feclearexcept_(CPU* c, void* fn) {
    (void)fn;
    c->fpsr &= ~((uint32_t)X(0) & FE_ALL);
    X(0) = 0;
}
static void feraiseexcept_(CPU* c, void* fn) {
    (void)fn;
    c->fpsr |= (uint32_t)X(0) & FE_ALL;
    X(0) = 0;
}
static void fetestexcept_(CPU* c, void* fn) {
    (void)fn;
    X(0) = c->fpsr & (uint32_t)X(0) & FE_ALL;
}
/* fegetexceptflag(flags, excepts) and fesetexceptflag: a fexcept_t is FPSR's bits */
static void fegetexceptflag_(CPU* c, void* fn) {
    (void)fn;
    const uint32_t flags = c->fpsr & (uint32_t)X(1) & FE_ALL;
    memcpy(PTR(0), &flags, sizeof flags);
    X(0) = 0;
}
static void fesetexceptflag_(CPU* c, void* fn) {
    (void)fn;
    const uint32_t excepts = (uint32_t)X(1) & FE_ALL;
    uint32_t flags;
    memcpy(&flags, PTR(0), sizeof flags);
    c->fpsr = (c->fpsr & ~excepts) | (flags & excepts);
    X(0) = 0;
}
static void fegetround_(CPU* c, void* fn) {
    (void)fn;
    X(0) = c->fpcr >> RMODE_SHIFT & 3u;
}
/* fesetround: -1 for a mode bionic has not */
static void fesetround_(CPU* c, void* fn) {
    (void)fn;
    const int mode = arg_int(c, 0);
    if (mode < 0 || mode > 3) {
        X(0) = UINT32_MAX;
        return;
    }
    c->fpcr = (c->fpcr & ~(3u << RMODE_SHIFT)) | (uint32_t)mode << RMODE_SHIFT;
    X(0) = 0;
}
/* AArch64's floating-point traps are optional and Android's kernel enables none: bionic's feenableexcept refuses
   each, fedisableexcept has none to disable and fegetexcept says none is on */
static void feenableexcept_(CPU* c, void* fn) {
    (void)fn;
    X(0) = UINT32_MAX;
}
static void fe_none(CPU* c, void* fn) {
    (void)fn;
    X(0) = 0;
}

/* a host function through an adapter (TH: the host's of another name), a handler of its own, an object, and the
   table's end */
#define T(name, adapter)      {#name, adapter, (void*)name}
#define TH(name, adapter, fn) {#name, adapter, (void*)fn}
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
    H(fmax, fmax_d),
    H(fmin, fmin_d),
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
    H(lround, lround_d),
    H(llround, lround_d),
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
    H(fmaxf, fmax_f),
    H(fminf, fmin_f),
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
    H(lroundf, lround_f),
    H(llroundf, lround_f),
    TH(j0f, f_d, BESSEL(j0)),
    TH(j1f, f_d, BESSEL(j1)),
    TH(jnf, f_id, BESSEL(jn)),
    TH(y0f, f_d, BESSEL(y0)),
    TH(y1f, f_d, BESSEL(y1)),
    TH(ynf, f_id, BESSEL(yn)),
    /* the host's, of complex numbers */
    T(cacos, z_z),
    T(cacosh, z_z),
    T(casin, z_z),
    T(casinh, z_z),
    T(catan, z_z),
    T(catanh, z_z),
    T(ccos, z_z),
    T(ccosh, z_z),
    T(csin, z_z),
    T(csinh, z_z),
    T(ctan, z_z),
    T(ctanh, z_z),
    T(cexp, z_z),
    T(clog, z_z),
    T(csqrt, z_z),
    T(cproj, z_z),
    T(conj, z_z),
    T(cpow, z_zz),
    T(carg, d_z),
    T(creal, d_z),
    T(cimag, d_z),
    T(cacosf, c_c),
    T(cacoshf, c_c),
    T(casinf, c_c),
    T(casinhf, c_c),
    T(catanf, c_c),
    T(catanhf, c_c),
    T(ccosf, c_c),
    T(ccoshf, c_c),
    T(csinf, c_c),
    T(csinhf, c_c),
    T(ctanf, c_c),
    T(ctanhf, c_c),
    T(cexpf, c_c),
    T(clogf, c_c),
    T(csqrtf, c_c),
    T(cprojf, c_c),
    T(conjf, c_c),
    T(cpowf, c_cc),
    T(cargf, f_c),
    T(crealf, f_c),
    T(cimagf, f_c),
    H(cabs, cabs_d),
    H(cabsf, cabs_f),
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
    H(fegetenv, fegetenv_),
    H(fesetenv, fesetenv_),
    H(feholdexcept, feholdexcept_),
    H(feupdateenv, feupdateenv_),
    H(feclearexcept, feclearexcept_),
    H(feraiseexcept, feraiseexcept_),
    H(fetestexcept, fetestexcept_),
    H(fegetexceptflag, fegetexceptflag_),
    H(fesetexceptflag, fesetexceptflag_),
    H(fegetround, fegetround_),
    H(fesetround, fesetround_),
    H(feenableexcept, feenableexcept_),
    H(fedisableexcept, fe_none),
    H(fegetexcept, fe_none),
    D(__fe_dfl_env, (void*)fe_dfl_env),
    END,
};
