/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "hdr_ref.h"
#include <math.h>

double hdr_ref_srgb_to_linear(double c) {
    if (c <= 0.0) return 0.0;
    if (c >= 1.0) return 1.0;
    return c < 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

double hdr_ref_linear_to_srgb(double c) {
    if (c <= 0.0) return 0.0;
    if (c >= 1.0) return 1.0;
    return c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

/* SMPTE ST 2084's constants, with the 10000 nits HDR10 uses */
double hdr_ref_pq_encode(double nits) {
    const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
    const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
    double y;
    if (nits <= 0.0) return 0.0;
    if (nits > 10000.0) nits = 10000.0;
    y = pow(nits / 10000.0, m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

double hdr_ref_pq_decode(double code) {
    const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
    const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
    double p;
    if (code <= 0.0) return 0.0;
    if (code > 1.0) code = 1.0;
    p = pow(code, 1.0 / m2);
    return 10000.0 * pow(fmax(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}

double hdr_ref_sdr_nits(double linear, double paper, double boost) {
    double c = linear < 0.0 ? 0.0 : linear > 1.0 ? 1.0 : linear;
    return paper * (c + boost * c * c * c * c);
}

double hdr_ref_tonemap(double nits, double paper, double peak) {
    const double sp = 0.9;
    double t, tp;
    if (nits <= 0.0 || paper <= 0.0 || peak <= 0.0) return 0.0;
    tp = paper / peak;
    if (tp > 1.0) tp = 1.0;
    t = nits / peak;
    if (t > 1.0) t = 1.0;
    if (t <= tp) return tp > 0.0 ? t * sp / tp : 0.0;
    if (tp >= 1.0) return sp;
    return sp + (1.0 - sp) * (t - tp) / (1.0 - tp);
}

double hdr_ref_sdr_code_nits(double code, double paper, double boost) {
    return hdr_ref_sdr_nits(hdr_ref_srgb_to_linear(code), paper, boost);
}

double hdr_ref_sdr_nits_code(double nits, double paper, double peak) {
    return hdr_ref_linear_to_srgb(hdr_ref_tonemap(nits, paper, peak));
}

void hdr_ref_bt709_to_2020(const double in[3], double out[3]) {
    out[0] = 0.6274 * in[0] + 0.3293 * in[1] + 0.0433 * in[2];
    out[1] = 0.0691 * in[0] + 0.9195 * in[1] + 0.0114 * in[2];
    out[2] = 0.0164 * in[0] + 0.0880 * in[1] + 0.8956 * in[2];
}

void hdr_ref_bt2020_to_p3(const double in[3], double out[3]) {
    out[0] = 1.343578 * in[0] - 0.282180 * in[1] - 0.061399 * in[2];
    out[1] = -0.065297 * in[0] + 1.075788 * in[1] - 0.010490 * in[2];
    out[2] = 0.002822 * in[0] - 0.019598 * in[1] + 1.016777 * in[2];
}

double hdr_ref_expand_code(double code, double e) {
    double x = code < 0.0 ? 0.0 : code > 1.0 ? 1.0 : code;
    double s = x * x * (3.0 - 2.0 * x);
    return x + (s - x) * e;
}

double hdr_ref_expand_linear(double lin, double e) {
    if (lin <= 0.0) return 0.0;
    return hdr_ref_srgb_to_linear(hdr_ref_expand_code(hdr_ref_linear_to_srgb(lin), e));
}

void hdr_ref_bt2020_to_709(const double in[3], double out[3]) {
    out[0] = 1.6605 * in[0] - 0.5876 * in[1] - 0.0728 * in[2];
    out[1] = -0.1246 * in[0] + 1.1329 * in[1] - 0.0083 * in[2];
    out[2] = -0.0182 * in[0] - 0.1006 * in[1] + 1.1187 * in[2];
}
