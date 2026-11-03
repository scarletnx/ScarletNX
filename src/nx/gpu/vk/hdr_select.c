/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/vk/hdr_select.h"
#include <math.h>
#include <string.h>
#include "common/utilities/ascii.h"

static int equal_ci(const char* a, const char* b) {
    while (*a && *b) {
        if (snx_to_lower((unsigned char)*a) != snx_to_lower((unsigned char)*b)) return 0;
        ++a;
        ++b;
    }
    return !*a && !*b;
}

static const char* const mode_keys[SNX_HDR_COUNT] = {"off", "auto", "on"};
static const char* const mode_labels[SNX_HDR_COUNT] = {"Off", "Auto", "On"};

int snx_hdr_parse(const char* value) {
    static const char* const words[] = {"off", "none", "sdr", "auto", "on", "hdr", "hdr10"};
    static const signed char ids[] = {SNX_HDR_OFF,  SNX_HDR_OFF,  SNX_HDR_OFF,  SNX_HDR_AUTO,
                                      SNX_HDR_ON,   SNX_HDR_ON,   SNX_HDR_ON};
    char text[16];
    size_t i, n;
    if (!value) return SNX_HDR_OFF;
    while (snx_is_blank((unsigned char)*value)) value++;
    n = strlen(value);
    while (n && snx_is_blank((unsigned char)value[n - 1])) n--;
    if (n >= sizeof text) return SNX_HDR_OFF;
    memcpy(text, value, n);
    text[n] = 0;
    for (i = 0; i < sizeof words / sizeof words[0]; i++)
        if (equal_ci(text, words[i])) return ids[i];
    return SNX_HDR_OFF;
}

const char* snx_hdr_key(int mode) { return mode >= 0 && mode < SNX_HDR_COUNT ? mode_keys[mode] : mode_keys[SNX_HDR_OFF]; }
const char* snx_hdr_label(int mode) {
    return mode >= 0 && mode < SNX_HDR_COUNT ? mode_labels[mode] : mode_labels[SNX_HDR_OFF];
}

static const char* const source_keys[SNX_HDR_SRC_COUNT] = {"auto", "sdr", "linear", "pq"};
static const char* const source_labels[SNX_HDR_SRC_COUNT] = {"Auto", "SDR", "Linear", "PQ (HDR10)"};

int snx_hdr_source_parse(const char* value) {
    static const char* const words[] = {"auto",  "detect", "format", "sdr",  "srgb", "linear",
                                        "hdr",   "lin",    "pq",     "hdr10", "st2084", "2084"};
    static const signed char ids[] = {SNX_HDR_SRC_AUTO,   SNX_HDR_SRC_AUTO,   SNX_HDR_SRC_AUTO,   SNX_HDR_SRC_SDR,
                                      SNX_HDR_SRC_SDR,    SNX_HDR_SRC_LINEAR, SNX_HDR_SRC_LINEAR, SNX_HDR_SRC_LINEAR,
                                      SNX_HDR_SRC_PQ,     SNX_HDR_SRC_PQ,     SNX_HDR_SRC_PQ,     SNX_HDR_SRC_PQ};
    char text[16];
    size_t i, n;
    if (!value) return SNX_HDR_SRC_AUTO;
    while (snx_is_blank((unsigned char)*value)) value++;
    n = strlen(value);
    while (n && snx_is_blank((unsigned char)value[n - 1])) n--;
    if (n >= sizeof text) return SNX_HDR_SRC_AUTO;
    memcpy(text, value, n);
    text[n] = 0;
    for (i = 0; i < sizeof words / sizeof words[0]; i++)
        if (equal_ci(text, words[i])) return ids[i];
    return SNX_HDR_SRC_AUTO;
}

const char* snx_hdr_source_key(int source) {
    return source >= 0 && source < SNX_HDR_SRC_COUNT ? source_keys[source] : source_keys[SNX_HDR_SRC_AUTO];
}
const char* snx_hdr_source_label(int source) {
    return source >= 0 && source < SNX_HDR_SRC_COUNT ? source_labels[source] : source_labels[SNX_HDR_SRC_AUTO];
}
int snx_hdr_source_value(int source) { return source == SNX_HDR_SRC_LINEAR || source == SNX_HDR_SRC_PQ ? source : SNX_HDR_SRC_SDR; }

static const char* const cs_keys[SNX_HDR_CS_COUNT] = {"auto", "p3", "scrgb", "hdr10"};
static const char* const cs_labels[SNX_HDR_CS_COUNT] = {"Auto", "Display P3", "scRGB", "HDR10 (PQ)"};

int snx_hdr_colorspace_parse(const char* value) {
    static const char* const words[] = {"auto", "detect", "p3", "display p3", "p3 linear", "scrgb",
                                        "extended srgb", "linear", "hdr10", "pq", "2084"};
    static const signed char ids[] = {SNX_HDR_CS_AUTO, SNX_HDR_CS_AUTO,   SNX_HDR_CS_P3,    SNX_HDR_CS_P3,
                                      SNX_HDR_CS_P3,   SNX_HDR_CS_SCRGB, SNX_HDR_CS_SCRGB, SNX_HDR_CS_SCRGB,
                                      SNX_HDR_CS_PQ,   SNX_HDR_CS_PQ,    SNX_HDR_CS_PQ};
    char text[16];
    size_t i, n;
    if (!value) return SNX_HDR_CS_AUTO;
    while (snx_is_blank((unsigned char)*value)) value++;
    n = strlen(value);
    while (n && snx_is_blank((unsigned char)value[n - 1])) n--;
    if (n >= sizeof text) return SNX_HDR_CS_AUTO;
    memcpy(text, value, n);
    text[n] = 0;
    for (i = 0; i < sizeof words / sizeof words[0]; i++)
        if (equal_ci(text, words[i])) return ids[i];
    return SNX_HDR_CS_AUTO;
}
const char* snx_hdr_colorspace_key(int cs) {
    return cs >= 0 && cs < SNX_HDR_CS_COUNT ? cs_keys[cs] : cs_keys[SNX_HDR_CS_AUTO];
}
const char* snx_hdr_colorspace_label(int cs) {
    return cs >= 0 && cs < SNX_HDR_CS_COUNT ? cs_labels[cs] : cs_labels[SNX_HDR_CS_AUTO];
}

/* ---- the numbers ---- */
float snx_hdr_srgb_to_linear(float c) {
    if (!(c > 0.0f)) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

float snx_hdr_linear_to_srgb(float c) {
    if (!(c > 0.0f)) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

/* SMPTE ST 2084's EOTF and its inverse, with the reference 10000 nits */
float snx_hdr_pq_encode(float nits) {
    const float m1 = 2610.0f / 16384.0f, m2 = 2523.0f / 32.0f;
    const float c1 = 3424.0f / 4096.0f, c2 = 2413.0f / 128.0f, c3 = 2392.0f / 128.0f;
    float y;
    if (!(nits > 0.0f)) return 0.0f;
    if (nits > 10000.0f) nits = 10000.0f;
    y = powf(nits / 10000.0f, m1);
    return powf((c1 + c2 * y) / (1.0f + c3 * y), m2);
}

float snx_hdr_pq_decode(float code) {
    const float m1 = 2610.0f / 16384.0f, m2 = 2523.0f / 32.0f;
    const float c1 = 3424.0f / 4096.0f, c2 = 2413.0f / 128.0f, c3 = 2392.0f / 128.0f;
    float c;
    if (!(code > 0.0f)) return 0.0f;
    c = code > 1.0f ? 1.0f : code;
    c = powf(c, 1.0f / m2);
    return 10000.0f * powf(fmaxf(c - c1, 0.0f) / (c2 - c3 * c), 1.0f / m1);
}

float snx_hdr_sdr_nits(float linear, float paper_nits, float boost) {
    float c;
    if (!(linear > 0.0f)) return 0.0f;
    c = linear > 1.0f ? 1.0f : linear;
    if (!(boost > 0.0f)) return c * paper_nits;
    return paper_nits * (c + boost * c * c * c * c);
}

float snx_hdr_tonemap(float nits, float paper_nits, float peak_nits) {
    /* the SDR range up to the paper white takes 90% of the output, whole and even; the frame's own headroom above it
       takes the last 10%, so a highlight is lighter than white and monotonic, and the peak lands at 1 */
    const float sp = 0.9f;
    float t, tp;
    if (!(nits > 0.0f) || !(paper_nits > 0.0f) || !(peak_nits > 0.0f)) return 0.0f;
    tp = paper_nits / peak_nits;
    if (tp > 1.0f) tp = 1.0f;
    t = nits / peak_nits;
    if (t > 1.0f) t = 1.0f;
    if (t <= tp) return tp > 0.0f ? t * sp / tp : 0.0f;
    if (tp >= 1.0f) return sp;
    return sp + (1.0f - sp) * (t - tp) / (1.0f - tp);
}

/* BT.709 to BT.2020 at D65, the matrix of ITU-R BT.2087 (the values round to what the primaries give) */
void snx_hdr_bt709_to_bt2020(float rgb[3]) {
    const float r = rgb[0], g = rgb[1], b = rgb[2];
    rgb[0] = 0.6274f * r + 0.3293f * g + 0.0433f * b;
    rgb[1] = 0.0691f * r + 0.9195f * g + 0.0114f * b;
    rgb[2] = 0.0164f * r + 0.0880f * g + 0.8956f * b;
}

void snx_hdr_bt2020_to_p3(float rgb[3]) {
    const float r = rgb[0], g = rgb[1], b = rgb[2];
    rgb[0] = 1.343578f * r - 0.282180f * g - 0.061399f * b;
    rgb[1] = -0.065297f * r + 1.075788f * g - 0.010490f * b;
    rgb[2] = 0.002822f * r - 0.019598f * g + 1.016777f * b;
}

float snx_hdr_expand_code(float code, float e) {
    float x = code < 0.0f ? 0.0f : code > 1.0f ? 1.0f : code;
    float s = x * x * (3.0f - 2.0f * x);
    return x + (s - x) * e;
}

void snx_hdr_bt2020_to_bt709(float rgb[3]) {
    const float r = rgb[0], g = rgb[1], b = rgb[2];
    rgb[0] = 1.6605f * r - 0.5876f * g - 0.0728f * b;
    rgb[1] = -0.1246f * r + 1.1329f * g - 0.0083f * b;
    rgb[2] = -0.0182f * r - 0.1006f * g + 1.1187f * b;
}
