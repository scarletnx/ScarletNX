/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The PICA200's fragment lighting in software. Ported from Azahar video_core/renderer_software/sw_lighting.cpp
   (ComputeFragmentsColors), with the register layout of video_core/pica/regs_lighting.h, the LUT entries of
   pica_core.h and common/quaternion.h's rotation (GPL-2.0-or-later, Copyright 2014-2026 Citra Emulator Project /
   Azahar Emulator Project) at azahar-emu/azahar a4b335e. Vector arithmetic is plain host float, as there. */
#include "3ds/gpu/raster.h"
#include <math.h>
#include <string.h>

/* LightingRegs, from 0x140: eight lights of 16 words, then the shared registers */
enum {
    LIGHT = 0x140,
    GLOBAL_AMBIENT = 0x1c0,
    MAX_LIGHT = 0x1c2,
    CONFIG0 = 0x1c3,
    CONFIG1 = 0x1c4,
    ABS_INPUT = 0x1d0,
    LUT_INPUT = 0x1d1,
    LUT_SCALE = 0x1d2,
    LIGHT_ENABLE = 0x1d9
};
/* LightingSampler: the LUT each input reads */
enum { D0 = 0, D1 = 1, FR = 3, RB = 4, RG = 5, RR = 6, SP = 8, DA = 16 };

static void color(uint32_t c, float out[3]) {
    out[0] = (float)((c >> 20) & 0x3ff) / 255.f;
    out[1] = (float)((c >> 10) & 0x3ff) / 255.f;
    out[2] = (float)(c & 0x3ff) / 255.f;
}
static float dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross(const float* a, const float* b, float* o) {
    float r[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    memcpy(o, r, sizeof r);
}
static float normalize(float* v) {
    float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    for (unsigned i = 0; i < 3; ++i) v[i] /= length;
    return length;
}
/* QuaternionRotate: v + 2 * cross(q.xyz, cross(q.xyz, v) + v * q.w) */
static void rotate(const float q[4], const float v[3], float out[3]) {
    float c[3], t[3];
    cross(q, v, c);
    for (unsigned i = 0; i < 3; ++i) t[i] = c[i] + v[i] * q[3];
    cross(q, t, c);
    for (unsigned i = 0; i < 3; ++i) out[i] = v[i] + 2 * c[i];
}

static bool supported(unsigned config, unsigned sampler) {
    switch (sampler) {
        case D0: return config != 1;
        case D1: return config != 0 && config != 1 && config != 5;
        case SP: return config != 2 && config != 3;
        case FR: return config != 0 && config != 2 && config != 4;
        case RR: return config != 3;
        default: return config == 4 || config == 5 || config == 8; /* RG, RB */
    }
}
static float lut(const SnxCtrPica* p, unsigned table, unsigned index, float delta) {
    uint32_t e = p->lighting_lut[table][index & 0xff];
    float diff = (float)((e >> 12) & 0x7ff) / 2047.f;
    return (float)(e & 0xfff) / 4095.f + ((e >> 23) & 1 ? -diff : diff) * delta;
}
static float scale_of(unsigned s) {
    static const float scales[8] = {1, 2, 4, 8, 0, 0, 0.25f, 0.5f};
    return scales[s & 7];
}

typedef struct {
    const SnxCtrPica* p;
    const uint32_t* light;
    float normal[3], tangent[3], light_vector[3], norm_view[3], half[3];
} Context;

/* get_lut_value: the input's cosine, absolute (or clamped) into 0..255 or signed into -128..127, scaled */
static float lut_value(const Context* c, unsigned which, unsigned sampler) {
    const uint32_t* regs = c->p->regs;
    static const unsigned abs_bit[7] = {1, 5, 9, 13, 17, 21, 25}; /* d0 d1 sp fr rb rg rr */
    unsigned input = (regs[LUT_INPUT] >> (which * 4)) & 7, scale = (regs[LUT_SCALE] >> (which * 4)) & 7;
    bool absolute = !((regs[ABS_INPUT] >> abs_bit[which]) & 1);
    float result = 0.0f, h[3];
    memcpy(h, c->half, sizeof h);
    switch (input) {
        case 0: normalize(h), result = dot(c->normal, h); break;    /* NH */
        case 1: normalize(h), result = dot(c->norm_view, h); break; /* VH */
        case 2: result = dot(c->normal, c->norm_view); break;       /* NV */
        case 3: result = dot(c->light_vector, c->normal); break;    /* LN */
        case 4: {                                                   /* SP: the spotlight direction, 1.1.11 */
            float s[3] = {(float)((int32_t)(c->light[6] << 19) >> 19) / 2047.0f,
                          (float)((int32_t)(c->light[6] << 3) >> 19) / 2047.0f,
                          (float)((int32_t)(c->light[7] << 19) >> 19) / 2047.0f};
            result = dot(c->light_vector, s);
            break;
        }
        case 5: /* CP, in configuration 7 only */
            if (((regs[CONFIG0] >> 4) & 15) == 8) {
                float proj[3], d;
                normalize(h);
                d = dot(c->normal, h);
                for (unsigned i = 0; i < 3; ++i) proj[i] = h[i] - c->normal[i] * d;
                result = dot(proj, c->tangent);
            }
            break;
        default: break; /* unknown inputs: UNIMPLEMENTED in Azahar, 0 */
    }
    unsigned index;
    float delta;
    if (absolute) {
        result = (c->light[9] >> 1) & 1 ? fabsf(result) : fmaxf(result, 0.0f); /* two-sided diffuse */
        float flr = floorf(result * 256.0f);
        index = (unsigned)(uint8_t)(flr < 0 ? 0 : flr > 255 ? 255 : flr);
        delta = result * 256 - (float)index;
    } else {
        float flr = floorf(result * 128.0f);
        int8_t signed_index = (int8_t)(flr < -128 ? -128 : flr > 127 ? 127 : flr);
        delta = result * 128.0f - signed_index;
        index = (uint8_t)signed_index;
    }
    return scale_of(scale) * lut(c->p, sampler, index, delta);
}

void snx_ctr_lighting(const SnxCtrPica* p, const float quat[4], const float view[3], uint8_t textures[4][4],
                      uint8_t primary[4], uint8_t secondary[4]) {
    const uint32_t* regs = p->regs;
    uint32_t c0 = regs[CONFIG0], c1 = regs[CONFIG1];
    unsigned config = (c0 >> 4) & 15;
    float shadow[4] = {1, 1, 1, 1}, normal[3] = {0, 0, 0}, tangent[3] = {0, 0, 0};
    if (c0 & 1) { /* enable_shadow */
        for (unsigned k = 0; k < 4; ++k) shadow[k] = (float)textures[(c0 >> 24) & 3][k] / 255.0f;
        if ((c0 >> 18) & 1)
            for (unsigned k = 0; k < 4; ++k) shadow[k] = 1.0f - shadow[k];
    }
    unsigned bump = (c0 >> 28) & 3;
    if (bump) {
        float t[3];
        for (unsigned k = 0; k < 3; ++k) t[k] = (float)textures[(c0 >> 22) & 3][k] / 127.5f - 1.0f;
        if (bump == 1) { /* NormalMap */
            if (!((c0 >> 30) & 1)) t[2] = sqrtf(fmaxf(1 - (t[0] * t[0] + t[1] * t[1]), 0.0f));
            memcpy(normal, t, sizeof t);
            tangent[0] = 1;
        } else if (bump == 2) { /* TangentMap */
            normal[2] = 1;
            memcpy(tangent, t, sizeof t);
        } /* an unknown mode leaves both zero, as Azahar does */
    } else
        normal[2] = 1, tangent[0] = 1;
    Context c;
    c.p = p;
    rotate(quat, normal, c.normal);
    rotate(quat, tangent, c.tangent);
    float diffuse_sum[4] = {0, 0, 0, 1}, specular_sum[4] = {0, 0, 0, 1};
    for (unsigned li = 0; li <= (regs[MAX_LIGHT] & 7); ++li) {
        unsigned num = (regs[LIGHT_ENABLE] >> (li * 4)) & 7;
        const uint32_t* l = &regs[LIGHT + num * 16];
        c.light = l;
        float position[3] = {snx_ctr_pica_float(l[4] & 0xffff, 10, 5), snx_ctr_pica_float(l[4] >> 16, 10, 5),
                             snx_ctr_pica_float(l[5] & 0xffff, 10, 5)};
        for (unsigned k = 0; k < 3; ++k) c.light_vector[k] = l[9] & 1 ? position[k] : position[k] + view[k];
        float length = normalize(c.light_vector);
        memcpy(c.norm_view, view, sizeof c.norm_view);
        normalize(c.norm_view);
        for (unsigned k = 0; k < 3; ++k) c.half[k] = c.norm_view[k] + c.light_vector[k];
        float dist_atten = 1.0f;
        if (!((c1 >> 24) & (1u << num))) {
            float scale = snx_ctr_pica_float(l[11] & 0xfffff, 12, 7), bias = snx_ctr_pica_float(l[10] & 0xfffff, 12, 7);
            float s = scale * length + bias;
            s = s < 0 ? 0 : s > 1 ? 1 : s;
            float f = floorf(s * 256.0f);
            unsigned index = (unsigned)(uint8_t)(f < 0 ? 0 : f > 255 ? 255 : f);
            dist_atten = lut(p, DA + num, index, s * 256 - (float)index);
        }
        float spot_atten = 1.0f, d0 = 1.0f, d1 = 1.0f, refl[3], s0[3], s1[3], col[3];
        if (!((c1 >> 8) & (1u << num)) && supported(config, SP)) spot_atten = lut_value(&c, 2, SP + num);
        if (!((c1 >> 16) & 1) && supported(config, D0)) d0 = lut_value(&c, 0, D0);
        color(l[0], col);
        for (unsigned k = 0; k < 3; ++k) s0[k] = d0 * col[k];
        refl[0] = !((c1 >> 20) & 1) && supported(config, RR) ? lut_value(&c, 6, RR) : 1.0f;
        refl[1] = !((c1 >> 21) & 1) && supported(config, RG) ? lut_value(&c, 5, RG) : refl[0];
        refl[2] = !((c1 >> 22) & 1) && supported(config, RB) ? lut_value(&c, 4, RB) : refl[0];
        if (!((c1 >> 17) & 1) && supported(config, D1)) d1 = lut_value(&c, 1, D1);
        color(l[1], col);
        for (unsigned k = 0; k < 3; ++k) s1[k] = d1 * refl[k] * col[k];
        if (li == (regs[MAX_LIGHT] & 7) && !((c1 >> 19) & 1) && supported(config, FR)) {
            float fresnel = lut_value(&c, 3, FR);
            if ((c0 >> 2) & 1) diffuse_sum[3] = fresnel;
            if ((c0 >> 3) & 1) specular_sum[3] = fresnel;
        }
        float d = dot(c.light_vector, c.normal);
        d = (l[9] >> 1) & 1 ? fabsf(d) : fmaxf(d, 0.0f);
        float clamp_highlights = (c0 >> 27) & 1 ? (d == 0.0f ? 0.0f : 1.0f) : 1.0f;
        if ((l[9] >> 2) & 3) { /* geometric factors */
            float geo = dot(c.half, c.half);
            geo = geo == 0.0f ? 0.0f : fminf(d / geo, 1.0f);
            if ((l[9] >> 2) & 1)
                for (unsigned k = 0; k < 3; ++k) s0[k] *= geo;
            if ((l[9] >> 3) & 1)
                for (unsigned k = 0; k < 3; ++k) s1[k] *= geo;
        }
        bool shadowed = !(c1 & (1u << num));
        bool sp = ((c0 >> 16) & 1) && shadowed, ss = ((c0 >> 17) & 1) && shadowed;
        float dif[3], amb[3];
        color(l[2], dif);
        color(l[3], amb);
        for (unsigned k = 0; k < 3; ++k) {
            diffuse_sum[k] += (dif[k] * d * (sp ? shadow[k] : 1.0f) + amb[k]) * dist_atten * spot_atten;
            specular_sum[k] += (s0[k] + s1[k]) * clamp_highlights * dist_atten * spot_atten * (ss ? shadow[k] : 1.0f);
        }
    }
    if ((c0 >> 19) & 1) { /* shadow_alpha */
        if ((c0 >> 2) & 1) diffuse_sum[3] *= shadow[3];
        if ((c0 >> 3) & 1) specular_sum[3] *= shadow[3];
    }
    float ambient[3];
    color(regs[GLOBAL_AMBIENT], ambient);
    for (unsigned k = 0; k < 3; ++k) diffuse_sum[k] += ambient[k];
    for (unsigned k = 0; k < 4; ++k) {
        float a = diffuse_sum[k], b = specular_sum[k];
        /* NaN clamps to 0 as std::clamp's comparisons give it */
        primary[k] = (uint8_t)((a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a == a ? a : 0.0f) * 255);
        secondary[k] = (uint8_t)((b < 0.0f ? 0.0f : b > 1.0f ? 1.0f : b == b ? b : 0.0f) * 255);
    }
}
