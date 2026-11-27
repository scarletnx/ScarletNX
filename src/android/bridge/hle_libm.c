/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* bionic's libm as HLE (hle.h): the host's libm, called with the guest's registers by one adapter per type */
#include "android/bridge/hle.h"
#include "android/bridge/hle_impl.h"
#include <errno.h>
#include <fenv.h>
#include <math.h>
#include <string.h>

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
static void d_di(CPU* c, void* fn) { KEEP_ERRNO(hle_set_d(c, ((double (*)(double, int))fn)(hle_d(c, 0), (int)X(0)))); }
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

/* a host function through an adapter, one of its own, and the table's end */
#define T(name, adapter) {#name, adapter, (void*)name}
#define P(name)          {#name, hle_pass, (void*)name}
#define H(name, handler) {#name, handler, NULL}
#define END              {"", NULL, NULL}

const HleSymbol hle_libm[] = {
    T(acos, d_d),     T(asin, d_d),        T(atan, d_d),
    T(cos, d_d),      T(cosh, d_d),        T(exp, d_d),
    T(exp2, d_d),     T(log, d_d),         T(log10, d_d),
    T(log2, d_d),     T(sin, d_d),         T(sinh, d_d),
    T(sqrt, d_d),     T(tan, d_d),         T(tanh, d_d),
    T(cbrt, d_d),     T(expm1, d_d),       T(log1p, d_d),
    T(cbrtf, f_f),    T(expm1f, f_f),      T(log1pf, f_f),
    T(atan2, d_dd),   T(fmod, d_dd),       T(hypot, d_dd),
    T(pow, d_dd),     T(nextafter, d_dd),  T(fmax, d_dd),
    T(fmin, d_dd),    T(fma, d_ddd),       T(fmaf, f_fff),
    T(lround, l_d),   T(llround, l_d),     T(lroundf, l_f),
    T(llroundf, l_f), T(acosf, f_f),       T(asinf, f_f),
    T(atanf, f_f),    T(cosf, f_f),        T(expf, f_f),
    T(exp2f, f_f),    T(logf, f_f),        T(log10f, f_f),
    T(log2f, f_f),    T(sinf, f_f),        T(sqrtf, f_f),
    T(tanf, f_f),     T(atan2f, f_ff),     T(fmodf, f_ff),
    T(powf, f_ff),    T(remainderf, f_ff), T(nextafterf, f_ff),
    T(fmaxf, f_ff),   T(fminf, f_ff),      T(ldexp, d_di),
    T(frexp, d_dp),   T(modf, d_dp),       T(frexpf, f_fp),
    T(modff, f_fp),   H(sincos, sincos_d), H(sincosf, sincos_f),
    P(feclearexcept), P(fetestexcept),     END,
};
