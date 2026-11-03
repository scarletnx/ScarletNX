/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Float, half, double and conversion operations */
#include "nx/gpu/shader/translate.h"

/* the absolute value and negation of a value with no subnormal have none either */
u32 shader_float_mod(DirectCtx* c, u32 value, int absolute, int negative) {
    int flushed = shader_ftz_has(c, value);
    if (absolute) value = spv_emit_glsl_unary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_F_ABS, value);
    if (negative) value = spv_emit_fnegate(&c->emit, c->f32_type, value);
    if (flushed && (absolute || negative)) shader_ftz_mark(c, value);
    return value;
}

u32 shader_double_read(DirectCtx* c, u32 vec2_u32, u32 index) {
    u32 low = reg_read(c, index);
    u32 high = reg_read(c, index < 254 ? index + 1 : 255);
    u32 bits = spv_emit_vec2_construct(&c->emit, vec2_u32, low, high);
    return spv_emit_bitcast(&c->emit, c->f64_type, bits);
}

u32 shader_double_mod(DirectCtx* c, u32 value, int absolute, int negative) {
    if (absolute) value = spv_emit_glsl_unary(&c->emit, c->f64_type, c->glsl, SPV_GLSL_F_ABS, value);
    if (negative) value = spv_emit_fnegate(&c->emit, c->f64_type, value);
    return value;
}

void shader_double_write(DirectCtx* c, u32 vec2_u32, u32 index, u32 value) {
    u32 bits = spv_emit_bitcast(&c->emit, vec2_u32, value);
    reg_write(c, index, spv_emit_vec4_extract(&c->emit, c->u32_type, bits, 0));
    if (index < 254) reg_write(c, index + 1, spv_emit_vec4_extract(&c->emit, c->u32_type, bits, 1));
}

u32 shader_half_pair(DirectCtx* c, u32 vec2, u32 raw, u32 swizzle) {
    if (swizzle == 1) {
        u32 scalar = spv_emit_bitcast(&c->emit, c->f32_type, raw);
        return spv_emit_vec2_construct(&c->emit, vec2, scalar, scalar);
    }
    u32 unpacked = spv_emit_glsl_unary(&c->emit, vec2, c->glsl, SPV_GLSL_UNPACK_HALF2X16, raw);
    if (swizzle == 0) return unpacked;
    u32 scalar = spv_emit_vec4_extract(&c->emit, c->f32_type, unpacked, swizzle == 3);
    return spv_emit_vec2_construct(&c->emit, vec2, scalar, scalar);
}

u32 shader_half_pair_mod(DirectCtx* c, u32 vec2, u32 pair, int absolute, int negative) {
    if (absolute) pair = spv_emit_glsl_unary(&c->emit, vec2, c->glsl, SPV_GLSL_F_ABS, pair);
    if (negative) pair = spv_emit_fnegate(&c->emit, vec2, pair);
    return pair;
}

/* The two halves of an HADD2, HMUL2, HFMA2, HSET2 or HSETP2 immediate: 9 bits of each half's exponent and mantissa
   (bits 20 and 30, from the half's bit 6) and their signs (bits 29 and 56) */
u32 shader_half_immediate(DirectCtx* c, u64 w) {
    u32 low = (B(w, 20, 9) << 6) | (B(w, 29, 1) << 15);
    u32 high = (B(w, 30, 9) << 6) | (B(w, 56, 1) << 15);
    return spv_emit_constant_u32(&c->emit, c->u32_type, low | (high << 16));
}

/* HMUL2's and HFMA2's FMZ precision (2): a product with a zero factor is zero, even with an infinity or a NaN, so a
   lane whose A or B is zero gets zero_value's lane */
u32 shader_half_fmz(DirectCtx* c, u32 vec2, u32 pair, u32 lhs, u32 rhs, u32 zero_value) {
    u32 zero = spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f), lanes[2];
    for (u32 lane = 0; lane < 2; lane++) {
        u32 factor_a = spv_emit_vec4_extract(&c->emit, c->f32_type, lhs, lane);
        u32 factor_b = spv_emit_vec4_extract(&c->emit, c->f32_type, rhs, lane);
        u32 any = spv_emit_bool_binary(&c->emit, c->bool_type, 1,
                                       spv_emit_fcompare(&c->emit, c->bool_type, 2, factor_a, zero),
                                       spv_emit_fcompare(&c->emit, c->bool_type, 2, factor_b, zero));
        lanes[lane] =
            spv_emit_select(&c->emit, c->f32_type, any,
                            zero_value ? spv_emit_vec4_extract(&c->emit, c->f32_type, zero_value, lane) : zero,
                            spv_emit_vec4_extract(&c->emit, c->f32_type, pair, lane));
    }
    return spv_emit_vec2_construct(&c->emit, vec2, lanes[0], lanes[1]);
}

/* .SAT (spv_emit_saturate): clamped to [0, 1], and +0 for a NaN */
u32 shader_float_saturate(DirectCtx* c, u32 value, int saturate) {
    if (!saturate) return value;
    return spv_emit_saturate(&c->emit, c->f32_type, c->u32_type, c->bool_type, c->glsl, value);
}

u32 shader_half_pair_result(DirectCtx* c, u32 vec2, u32 pair, u32 old, u32 merge, int saturate) {
    if (saturate)
        pair = spv_emit_vec2_construct(
            &c->emit, vec2, shader_float_saturate(c, spv_emit_vec4_extract(&c->emit, c->f32_type, pair, 0), 1),
            shader_float_saturate(c, spv_emit_vec4_extract(&c->emit, c->f32_type, pair, 1), 1));
    if (merge == 1)
        return spv_emit_bitcast(&c->emit, c->u32_type, spv_emit_vec4_extract(&c->emit, c->f32_type, pair, 0));
    u32 packed = spv_emit_glsl_unary(&c->emit, c->u32_type, c->glsl, SPV_GLSL_PACK_HALF2X16, pair);
    if (merge == 2 || merge == 3) {
        u32 mask = spv_emit_constant_u32(&c->emit, c->u32_type, merge == 2 ? 0xffffu : 0xffff0000u);
        return spv_emit_bitwise_or_u32(
            &c->emit, c->u32_type,
            spv_emit_bitwise_and_u32(&c->emit, c->u32_type, old, spv_emit_not_u32(&c->emit, c->u32_type, mask)),
            spv_emit_bitwise_and_u32(&c->emit, c->u32_type, packed, mask));
    }
    return packed;
}

u32 shader_float_imm20(DirectCtx* c, u64 w) {
    return spv_emit_constant_u32(&c->emit, c->f32_type, (B(w, 20, 19) << 12) | (B(w, 56, 1) << 31));
}

u32 shader_float_imm32(DirectCtx* c, u64 w) { return spv_emit_constant_u32(&c->emit, c->f32_type, B(w, 20, 32)); }

/* A conversion's register, constant-buffer or immediate source, unpacked from F16 when requested. */
u32 shader_conversion_source_f32(DirectCtx* c, u32 vec2, u64 w, int kind, u32 format) {
    u32 raw = kind == 2 ? spv_emit_bitcast(&c->emit, c->u32_type, shader_float_imm20(c, w))
                        : shader_integer_right(c, w, kind);
    if (format != 1) return spv_emit_bitcast(&c->emit, c->f32_type, raw);
    u32 unpacked = spv_emit_glsl_unary(&c->emit, vec2, c->glsl, SPV_GLSL_UNPACK_HALF2X16, raw);
    return spv_emit_vec4_extract(&c->emit, c->f32_type, unpacked, B(w, 41, 1));
}

u32 shader_double_source(DirectCtx* c, u32 vec2_u32, u64 w, int kind) {
    if (kind == 0) return shader_double_read(c, vec2_u32, B(w, 20, 8));
    u32 low = c->zero, high;
    if (kind == 1) {
        u32 bank = B(w, 34, 5), offset = B(w, 20, 14) * 4;
        if (!(offset & 4))
            low = shader_cbuf_read_dynamic(c, bank, spv_emit_constant_u32(&c->emit, c->u32_type, offset));
        high = shader_cbuf_read_dynamic(c, bank, spv_emit_constant_u32(&c->emit, c->u32_type, offset | 4u));
    } else
        high = spv_emit_constant_u32(&c->emit, c->u32_type, (B(w, 20, 19) << 12) | (B(w, 56, 1) << 31));
    return spv_emit_bitcast(&c->emit, c->f64_type, spv_emit_vec2_construct(&c->emit, vec2_u32, low, high));
}
