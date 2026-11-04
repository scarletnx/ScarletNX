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

/* the float, half, double and conversion instructions */
void emit_float(const Tr* t, u32 i, u64 w, int op, u32 d, u32 a, u32 b) {
    const u64* insn = t->insn;
    DirectCtx* c = t->c;
    u32 i32_type = t->i32_type;
    u32 i64_type = t->i64_type;
    u32 u64_type = t->u64_type;
    u32 vec2 = t->vec2;
    u32 vec2_u32 = t->vec2_u32;
    u32 zero_f = t->zero_f;
    u8* block_start = t->block_start;
    if (op == O_FCMP_R || op == O_FCMP_RC || op == O_FCMP_CR || op == O_FCMP_I) {
        u32 third = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, B(w, 39, 8)));
        u32 comparison = op == O_FCMP_RC ? shader_cbuf_read(c, w) : third;
        u32 alternative = op == O_FCMP_RC   ? reg_read(c, B(w, 39, 8))
                          : op == O_FCMP_CR ? shader_cbuf_word(c, w)
                          : op == O_FCMP_I  ? spv_emit_bitcast(&c->emit, c->u32_type, shader_float_imm20(c, w))
                                            : reg_read(c, b);
        u32 test = spv_emit_fcompare(&c->emit, c->bool_type, B(w, 48, 4), comparison, zero_f);
        reg_write(c, d, spv_emit_select(&c->emit, c->u32_type, test, reg_read(c, a), alternative));
    } else if (op == O_F2F_R || op == O_F2F_C || op == O_F2F_I) {
        /* Formats 1 F16, 2 F32 and 3 F64 (a register pair, or a constant buffer's or the immediate's double). An
           F64 on either side converts with the default rounding and no saturation, as Eden translates it.
           F64 to F16 passes through F32 and can round twice. */
        u32 dst_format = B(w, 8, 2), src_format = B(w, 10, 2);
        if (src_format == 3 || dst_format == 3) {
            u32 value;
            if (src_format == 3)
                value =
                    shader_double_mod(c, shader_double_source(c, vec2_u32, w, op - O_F2F_R), B(w, 49, 1), B(w, 45, 1));
            else {
                value = shader_conversion_source_f32(c, vec2, w, op - O_F2F_R, src_format);
                value = shader_float_mod(c, value, B(w, 49, 1), B(w, 45, 1));
            }
            if (dst_format == 3) {
                if (src_format != 3)
                    value = spv_emit_fconvert(&c->emit, c->f64_type, value);
                else if (B(w, 42, 1))
                    value = spv_emit_glsl_unary(&c->emit, c->f64_type, c->glsl, conversion_rounds[B(w, 39, 2)], value);
                shader_double_write(c, vec2_u32, d, value);
            } else {
                value = spv_emit_fconvert(&c->emit, c->f32_type, value);
                reg_write(c, d,
                          dst_format == 1 ? spv_emit_glsl_unary(&c->emit, c->u32_type, c->glsl, SPV_GLSL_PACK_HALF2X16,
                                                                spv_emit_vec2_construct(&c->emit, vec2, value, zero_f))
                                          : spv_emit_bitcast(&c->emit, c->u32_type, value));
            }
        } else {
            u32 src = shader_conversion_source_f32(c, vec2, w, op - O_F2F_R, src_format);
            src = shader_float_mod(c, src, B(w, 49, 1), B(w, 45, 1));
            /* Between two floats of one size, bit 42 makes it an integer rounding (39-40: even, floor, ceil,
               trunc), F16 as well as F32: Unity's half-precision frac() is an F2F.F16.F16.FLOOR. Between sizes,
               39-40 is the conversion's rounding, which this translation leaves to the host */
            if (dst_format == src_format && B(w, 42, 1))
                src = spv_emit_glsl_unary(&c->emit, c->f32_type, c->glsl, conversion_rounds[B(w, 39, 2)], src);
            src = shader_float_saturate(c, src, B(w, 50, 1));
            u32 result;
            if (dst_format == 1) {
                u32 pair =
                    spv_emit_vec2_construct(&c->emit, vec2, src, spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f));
                result = spv_emit_glsl_unary(&c->emit, c->u32_type, c->glsl, SPV_GLSL_PACK_HALF2X16, pair);
            } else
                result = spv_emit_bitcast(&c->emit, c->u32_type, src);
            reg_write(c, d, result);
        }
    } else if (op == O_F2I_R || op == O_F2I_C || op == O_F2I_I) {
        /* The source is an F16, F32 or F64 (format 3, a pair), the result 16, 32 or 64 bits (3, a pair). Out of
           range values clamp to the result's range. NaN is 0 from F16 or F32 into 32 bits, and
           1 << (bits - 1) from F64 or into 64 bits, as PTX's cvt gives it. */
        u32 dst_format = B(w, 8, 2), src_format = B(w, 10, 2);
        int wide = src_format == 3, is_signed = (int)B(w, 12, 1);
        u32 ftype = wide ? c->f64_type : c->f32_type, src;
        if (wide)
            src = shader_double_mod(c, shader_double_source(c, vec2_u32, w, op - O_F2I_R), B(w, 49, 1), B(w, 45, 1));
        else {
            src = shader_conversion_source_f32(c, vec2, w, op - O_F2I_R, src_format);
            src = shader_float_mod(c, src, B(w, 49, 1), B(w, 45, 1));
        }
        src = spv_emit_glsl_unary(&c->emit, ftype, c->glsl, conversion_rounds[B(w, 39, 2)], src);
        if (dst_format == 3) {
            /* the clamp's bounds: the range's ends, the top one the largest float below 2^63 or 2^64 */
            u64 low_bits = is_signed ? (wide ? 0xc3e0000000000000ull : 0xdf000000ull) : 0;
            u64 high_bits = is_signed ? (wide ? 0x43dfffffffffffffull : 0x5effffffull)
                                      : (wide ? 0x43efffffffffffffull : 0x5f7fffffull);
            u64 limit_bits = is_signed ? (wide ? 0x43e0000000000000ull : 0x5f000000ull)
                                       : (wide ? 0x43f0000000000000ull : 0x5f800000ull);
            u32 low = wide ? spv_emit_constant64(&c->emit, ftype, low_bits)
                           : spv_emit_constant_u32(&c->emit, ftype, (u32)low_bits);
            u32 high = wide ? spv_emit_constant64(&c->emit, ftype, high_bits)
                            : spv_emit_constant_u32(&c->emit, ftype, (u32)high_bits);
            u32 limit = wide ? spv_emit_constant64(&c->emit, ftype, limit_bits)
                             : spv_emit_constant_u32(&c->emit, ftype, (u32)limit_bits);
            u32 itype = is_signed ? i64_type : u64_type;
            u32 bounded = spv_emit_glsl_ternary(&c->emit, ftype, c->glsl, SPV_GLSL_F_CLAMP, src, low, high);
            u32 result = is_signed ? spv_emit_convert_f_to_s(&c->emit, itype, bounded)
                                   : spv_emit_convert_f_to_u(&c->emit, itype, bounded);
            result = spv_emit_select(&c->emit, itype, spv_emit_fcompare(&c->emit, c->bool_type, 6, src, limit),
                                     spv_emit_constant64(&c->emit, itype, is_signed ? 0x7fffffffffffffffull : ~0ull),
                                     result);
            result = spv_emit_select(&c->emit, itype, spv_emit_isnan(&c->emit, c->bool_type, src),
                                     spv_emit_constant64(&c->emit, itype, 1ull << 63), result);
            u32 pair = spv_emit_bitcast(&c->emit, vec2_u32, result);
            reg_write(c, d, spv_emit_vec4_extract(&c->emit, c->u32_type, pair, 0));
            if (d < 254) reg_write(c, d + 1, spv_emit_vec4_extract(&c->emit, c->u32_type, pair, 1));
        } else if (!is_signed) {
            u32 low = wide ? spv_emit_constant64(&c->emit, ftype, 0) : spv_emit_constant_f32(&c->emit, ftype, 0.0f);
            u32 high = wide ? spv_emit_constant64(&c->emit, ftype, 0x41efffffffe00000ull) /* 2^32 - 1 */
                            : spv_emit_constant_f32(&c->emit, ftype, 4294967040.0f);
            u32 bounded = spv_emit_glsl_ternary(&c->emit, ftype, c->glsl, SPV_GLSL_F_CLAMP, src, low, high);
            u32 result = spv_emit_convert_f_to_u(&c->emit, c->u32_type, bounded);
            u32 overflow = spv_emit_fcompare(&c->emit, c->bool_type, 6, src,
                                             wide ? spv_emit_constant64(&c->emit, ftype, 0x41f0000000000000ull)
                                                  : spv_emit_constant_f32(&c->emit, ftype, 4294967296.0f));
            result = spv_emit_select(&c->emit, c->u32_type, overflow,
                                     spv_emit_constant_u32(&c->emit, c->u32_type, 0xffffffffu), result);
            result =
                spv_emit_select(&c->emit, c->u32_type, spv_emit_isnan(&c->emit, c->bool_type, src),
                                wide ? spv_emit_constant_u32(&c->emit, c->u32_type, 0x80000000u) : c->zero, result);
            if (dst_format == 1) {
                u32 bound = spv_emit_constant_u32(&c->emit, c->u32_type, 65535);
                u32 large = spv_emit_u_less(&c->emit, c->bool_type, bound, result);
                result = spv_emit_select(&c->emit, c->u32_type, large, bound, result);
                if (wide)
                    result = spv_emit_select(&c->emit, c->u32_type, spv_emit_isnan(&c->emit, c->bool_type, src),
                                             spv_emit_constant_u32(&c->emit, c->u32_type, 0x8000), result);
            }
            reg_write(c, d, result);
        } else {
            u32 low = wide ? spv_emit_constant64(&c->emit, ftype, 0xc1e0000000000000ull) /* -2^31 */
                           : spv_emit_constant_f32(&c->emit, ftype, -2147483648.0f);
            u32 high = wide ? spv_emit_constant64(&c->emit, ftype, 0x41dfffffffc00000ull) /* 2^31 - 1 */
                            : spv_emit_constant_f32(&c->emit, ftype, 2147483520.0f);
            u32 bounded = spv_emit_glsl_ternary(&c->emit, ftype, c->glsl, SPV_GLSL_F_CLAMP, src, low, high);
            u32 result = spv_emit_convert_f_to_s(&c->emit, i32_type, bounded);
            u32 overflow = spv_emit_fcompare(&c->emit, c->bool_type, 6, src,
                                             wide ? spv_emit_constant64(&c->emit, ftype, 0x41e0000000000000ull)
                                                  : spv_emit_constant_f32(&c->emit, ftype, 2147483648.0f));
            result = spv_emit_select(&c->emit, i32_type, overflow,
                                     spv_emit_constant_u32(&c->emit, i32_type, 0x7fffffff), result);
            result = spv_emit_select(&c->emit, i32_type, spv_emit_isnan(&c->emit, c->bool_type, src),
                                     spv_emit_constant_u32(&c->emit, i32_type, wide ? 0x80000000u : 0), result);
            if (dst_format == 1) { /* S16 saturates like U16 */
                u32 top = spv_emit_constant_u32(&c->emit, i32_type, 32767);
                u32 bottom = spv_emit_constant_u32(&c->emit, i32_type, 0xffff8000u);
                result = spv_emit_select(&c->emit, i32_type, spv_emit_s_greater(&c->emit, c->bool_type, result, top),
                                         top, result);
                result = spv_emit_select(&c->emit, i32_type, spv_emit_s_greater(&c->emit, c->bool_type, bottom, result),
                                         bottom, result);
            }
            reg_write_as(c, d, result, i32_type);
        }
    } else if (op == O_I2F_R || op == O_I2F_C || op == O_I2F_I) {
        /* As Eden converts: abs on the integer at its source width (the most negative value stays), then neg on
           the floating result except for that value. A 64-bit source (format 3) is a register pair, a constant
           buffer's two words or the immediate sign-extended. F16 output passes through F32 and can round twice. */
        u32 size = B(w, 10, 2), dst_format = B(w, 8, 2);
        int is_signed = (int)B(w, 13, 1), absolute = (int)B(w, 49, 1), negate = (int)B(w, 45, 1);
        u32 target_type = dst_format == 3 ? c->f64_type : c->f32_type, result, least = 0;
        if (size == 3) {
            u32 low, high;
            if (op == O_I2F_R) {
                low = reg_read(c, b);
                high = reg_read(c, b < 254 ? b + 1 : 255);
            } else if (op == O_I2F_C) {
                u32 bank = B(w, 34, 5), offset = B(w, 20, 14) * 4;
                low = shader_cbuf_read_dynamic(c, bank, spv_emit_constant_u32(&c->emit, c->u32_type, offset));
                high = shader_cbuf_read_dynamic(c, bank, spv_emit_constant_u32(&c->emit, c->u32_type, offset + 4));
            } else {
                i32 immediate = (i32)(B(w, 20, 19) | (B(w, 56, 1) ? 0xfff80000u : 0));
                low = spv_emit_constant_u32(&c->emit, c->u32_type, (u32)immediate);
                high = spv_emit_constant_u32(&c->emit, c->u32_type, immediate < 0 ? 0xffffffffu : 0);
            }
            u32 itype = is_signed ? i64_type : u64_type;
            u32 value = spv_emit_bitcast(&c->emit, itype, spv_emit_vec2_construct(&c->emit, vec2_u32, low, high));
            if (is_signed && negate && !absolute)
                least =
                    spv_emit_i_equal(&c->emit, c->bool_type, value, spv_emit_constant64(&c->emit, itype, 1ull << 63));
            if (is_signed && absolute)
                value = spv_emit_select(
                    &c->emit, itype,
                    spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_constant64(&c->emit, itype, 0), value),
                    spv_emit_isub_u32(&c->emit, itype, spv_emit_constant64(&c->emit, itype, 0), value), value);
            result = is_signed ? spv_emit_convert_s_to_f(&c->emit, target_type, value)
                               : spv_emit_convert_u_to_f(&c->emit, target_type, value);
        } else {
            u32 value = shader_integer_right(c, w, op - O_I2F_R);
            u32 width = size == 0 ? 8 : size == 1 ? 16 : 32;
            if (size < 2) {
                u32 offset =
                    spv_emit_constant_u32(&c->emit, c->u32_type, size == 0 ? B(w, 41, 2) * 8 : (B(w, 41, 2) & 2) * 8);
                u32 bits = spv_emit_constant_u32(&c->emit, c->u32_type, width);
                value = is_signed
                            ? spv_emit_bitcast(&c->emit, c->u32_type,
                                               spv_emit_bitfield_sextract(&c->emit, i32_type,
                                                                          spv_emit_bitcast(&c->emit, i32_type, value),
                                                                          offset, bits))
                            : spv_emit_bitfield_uextract(&c->emit, c->u32_type, value, offset, bits);
            }
            if (is_signed && negate && !absolute)
                least = spv_emit_i_equal(&c->emit, c->bool_type, value,
                                         spv_emit_constant_u32(&c->emit, c->u32_type, 0u - (1u << (width - 1))));
            if (is_signed && absolute) {
                u32 min_value = spv_emit_constant_u32(&c->emit, c->u32_type, 0u - (1u << (width - 1)));
                u32 negative = spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_constant_u32(&c->emit, i32_type, 0),
                                                  spv_emit_bitcast(&c->emit, i32_type, value));
                u32 minimum = spv_emit_i_equal(&c->emit, c->bool_type, value, min_value);
                u32 negate_abs = spv_emit_bool_binary(&c->emit, c->bool_type, 0, negative,
                                                      spv_emit_bool_not(&c->emit, c->bool_type, minimum));
                value = spv_emit_select(&c->emit, c->u32_type, negate_abs,
                                        spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, value), value);
            }
            result = is_signed
                         ? spv_emit_convert_s_to_f(&c->emit, target_type, spv_emit_bitcast(&c->emit, i32_type, value))
                         : spv_emit_convert_u_to_f(&c->emit, target_type, value);
        }
        if (negate) {
            u32 negated = spv_emit_fnegate(&c->emit, target_type, result);
            result = least ? spv_emit_select(&c->emit, target_type, least, result, negated) : negated;
        }
        if (dst_format == 3)
            shader_double_write(c, vec2_u32, d, result);
        else if (dst_format == 1) {
            u32 pair =
                spv_emit_vec2_construct(&c->emit, vec2, result, spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f));
            reg_write(c, d, spv_emit_glsl_unary(&c->emit, c->u32_type, c->glsl, SPV_GLSL_PACK_HALF2X16, pair));
        } else
            reg_write_as(c, d, result, c->f32_type);
    } else if (op == O_FADD_R || op == O_FADD_C || op == O_FADD_I || op == O_FADD32I || op == O_FMUL_R ||
               op == O_FMUL_C || op == O_FMUL_I || op == O_FMUL32I) {
        u32 left = shader_ftz_read(c, insn, block_start, i, a, reg_read_as(c, a, c->f32_type));
        u32 right = op == O_FADD_C || op == O_FMUL_C   ? shader_cbuf_read(c, w)
                    : op == O_FADD_I || op == O_FMUL_I ? shader_float_imm20(c, w)
                    : op == O_FADD32I || op == O_FMUL32I
                        ? shader_float_imm32(c, w)
                        : shader_ftz_read(c, insn, block_start, i, b, reg_read_as(c, b, c->f32_type));
        if (op == O_FADD_R || op == O_FADD_C || op == O_FADD_I) {
            left = shader_float_mod(c, left, B(w, 46, 1), B(w, 48, 1));
            right = shader_float_mod(c, right, B(w, 49, 1), B(w, 45, 1));
        } else if (op == O_FADD32I) {
            left = shader_float_mod(c, left, B(w, 54, 1), B(w, 56, 1));
            right = shader_float_mod(c, right, B(w, 57, 1), B(w, 53, 1));
        } else if (op == O_FMUL_R || op == O_FMUL_C || op == O_FMUL_I) {
            right = shader_float_mod(c, right, 0, B(w, 48, 1));
        }
        u32 mode = op == O_FADD32I                                      ? B(w, 55, 1)
                   : op == O_FMUL32I                                    ? B(w, 53, 2)
                   : op == O_FADD_R || op == O_FADD_C || op == O_FADD_I ? B(w, 44, 1)
                                                                        : B(w, 44, 2);
        int soft_ftz = (mode & 1) && c->soft_ftz_on;
        if (soft_ftz) {
            left = shader_float_ftz(c, left);
            right = shader_float_ftz(c, right);
        }
        u32 result = op == O_FADD_R || op == O_FADD_C || op == O_FADD_I || op == O_FADD32I
                         ? spv_emit_fadd(&c->emit, c->f32_type, left, right)
                         : spv_emit_fmul(&c->emit, c->f32_type, left, right);
        spv_emit_no_contraction(&c->emit, result);
        if (soft_ftz) result = shader_float_ftz(c, result);
        /* FMZ (FMUL's 44-45, FMUL32I's 53-54 = 2): a zero factor makes the product 0, even with an infinity */
        if ((op == O_FMUL32I                                      ? B(w, 53, 2)
             : op == O_FMUL_R || op == O_FMUL_C || op == O_FMUL_I ? B(w, 44, 2)
                                                                  : 0) == 2) {
            u32 zero = spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f);
            u32 left_zero = spv_emit_fcompare(&c->emit, c->bool_type, 2, left, zero);
            u32 right_zero = spv_emit_fcompare(&c->emit, c->bool_type, 2, right, zero);
            u32 either = spv_emit_bool_binary(&c->emit, c->bool_type, 1, left_zero, right_zero);
            result = spv_emit_select(&c->emit, c->f32_type, either, zero, result);
        }
        if (op == O_FMUL_R || op == O_FMUL_C || op == O_FMUL_I) {
            static const float scale[] = {1.0f, 0.5f, 0.25f, 0.125f, 8.0f, 4.0f, 2.0f};
            u32 sc = B(w, 41, 3);
            if (sc) {
                u32 factor = spv_emit_constant_f32(&c->emit, c->f32_type, scale[sc]);
                result = spv_emit_fmul(&c->emit, c->f32_type, result, factor);
                spv_emit_no_contraction(&c->emit, result);
            }
        }
        int sat = op == O_FADD32I ? 0 : op == O_FMUL32I ? B(w, 55, 1) : B(w, 50, 1);
        result = shader_float_saturate(c, result, sat);
        reg_write_as(c, d, result, c->f32_type);
    } else if (op == O_FFMA_R || op == O_FFMA_RC || op == O_FFMA_CR || op == O_FFMA_I || op == O_FFMA32I) {
        u32 left = shader_ftz_read(c, insn, block_start, i, a, reg_read_as(c, a, c->f32_type));
        u32 right = op == O_FFMA32I   ? shader_float_imm32(c, w)
                    : op == O_FFMA_CR ? shader_cbuf_read(c, w)
                    : op == O_FFMA_I  ? shader_float_imm20(c, w)
                                      : shader_ftz_read(c, insn, block_start, i, op == O_FFMA_RC ? B(w, 39, 8) : b,
                                                        reg_read_as(c, op == O_FFMA_RC ? B(w, 39, 8) : b, c->f32_type));
        u32 addend = op == O_FFMA_RC ? shader_cbuf_read(c, w)
                                     : shader_ftz_read(c, insn, block_start, i, op == O_FFMA32I ? d : B(w, 39, 8),
                                                       reg_read_as(c, op == O_FFMA32I ? d : B(w, 39, 8), c->f32_type));
        if (op == O_FFMA32I) {
            left = shader_float_mod(c, left, 0, B(w, 56, 1));
            addend = shader_float_mod(c, addend, 0, B(w, 57, 1));
        } else {
            right = shader_float_mod(c, right, 0, B(w, 48, 1));
            addend = shader_float_mod(c, addend, 0, B(w, 49, 1));
        }
        int soft_ftz = (B(w, 53, 2) & 1) && c->soft_ftz_on;
        if (soft_ftz) {
            left = shader_float_ftz(c, left);
            right = shader_float_ftz(c, right);
            addend = shader_float_ftz(c, addend);
        }
        u32 result = spv_emit_glsl_ternary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_FMA, left, right, addend);
        if (soft_ftz) result = shader_float_ftz(c, result);
        if (B(w, 53, 2) == 2) {
            u32 zero = spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f);
            u32 left_zero = spv_emit_fcompare(&c->emit, c->bool_type, 2, left, zero);
            u32 right_zero = spv_emit_fcompare(&c->emit, c->bool_type, 2, right, zero);
            result = spv_emit_select(&c->emit, c->f32_type, cc_or(c, left_zero, right_zero), addend, result);
        }
        result = shader_float_saturate(c, result, op == O_FFMA32I ? B(w, 55, 1) : B(w, 50, 1));
        reg_write_as(c, d, result, c->f32_type);
    } else if (op == O_FMNMX_R || op == O_FMNMX_C || op == O_FMNMX_I) {
        u32 left = reg_read_as(c, a, c->f32_type);
        u32 right = op == O_FMNMX_C   ? shader_cbuf_read(c, w)
                    : op == O_FMNMX_I ? shader_float_imm20(c, w)
                                      : reg_read_as(c, b, c->f32_type);
        left = shader_float_mod(c, left, B(w, 46, 1), B(w, 48, 1));
        right = shader_float_mod(c, right, B(w, 49, 1), B(w, 45, 1));
        u32 is_left_nan = spv_emit_isnan(&c->emit, c->bool_type, left);
        u32 is_right_nan = spv_emit_isnan(&c->emit, c->bool_type, right);
        u32 minimum = spv_emit_glsl_binary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_F_MIN, left, right);
        u32 maximum = spv_emit_glsl_binary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_F_MAX, left, right);
        u32 choose_min = pred_read(c, B(w, 39, 3), B(w, 42, 1));
        u32 value = spv_emit_select(&c->emit, c->f32_type, choose_min, minimum, maximum);
        value = spv_emit_select(&c->emit, c->f32_type, is_right_nan, left, value);
        value = spv_emit_select(&c->emit, c->f32_type, is_left_nan, right, value);
        reg_write_as(c, d, value, c->f32_type);
    } else if (op == O_FSETP_R || op == O_FSETP_C || op == O_FSETP_I) {
        u32 left = reg_read_as(c, a, c->f32_type);
        left = shader_float_mod(c, left, B(w, 7, 1), B(w, 43, 1));
        u32 right;
        if (op == O_FSETP_C)
            right = shader_cbuf_read(c, w);
        else if (op == O_FSETP_I)
            right = spv_emit_constant_u32(&c->emit, c->f32_type, (B(w, 20, 19) << 12) | (B(w, 56, 1) << 31));
        else
            right = reg_read_as(c, b, c->f32_type);
        right = shader_float_mod(c, right, B(w, 44, 1), B(w, 6, 1));
        u32 test = spv_emit_fcompare(&c->emit, c->bool_type, B(w, 48, 4), left, right);
        u32 pc = pred_read(c, B(w, 39, 3), B(w, 42, 1));
        u32 op_bool = B(w, 45, 2);
        u32 yes = bool_combine(c, op_bool, test, pc);
        u32 inverse = spv_emit_bool_not(&c->emit, c->bool_type, test);
        u32 no = bool_combine(c, op_bool, inverse, pc);
        pred_write(c, B(w, 3, 3), yes);
        pred_write(c, B(w, 0, 3), no);
    } else if ((op >= O_DADD_R && op <= O_DADD_I) || (op >= O_DMUL_R && op <= O_DMUL_I) ||
               (op >= O_DFMA_R && op <= O_DFMA_I) || (op >= O_DMNMX_R && op <= O_DMNMX_I)) {
        u32 left = shader_double_read(c, vec2_u32, a);
        u32 right = op == O_DFMA_RC                      ? shader_double_read(c, vec2_u32, B(w, 39, 8))
                    : op == O_DFMA_CR                    ? shader_double_source(c, vec2_u32, w, 1)
                    : op >= O_DFMA_R && op <= O_DFMA_I   ? shader_double_source(c, vec2_u32, w, op == O_DFMA_I ? 2 : 0)
                    : (op >= O_DADD_R && op <= O_DADD_I) ? shader_double_source(c, vec2_u32, w, op - O_DADD_R)
                    : (op >= O_DMUL_R && op <= O_DMUL_I) ? shader_double_source(c, vec2_u32, w, op - O_DMUL_R)
                                                         : shader_double_source(c, vec2_u32, w, op - O_DMNMX_R);
        u32 value;
        if (op >= O_DADD_R && op <= O_DADD_I) {
            left = shader_double_mod(c, left, B(w, 46, 1), B(w, 48, 1));
            right = shader_double_mod(c, right, B(w, 49, 1), B(w, 45, 1));
            value = spv_emit_fadd(&c->emit, c->f64_type, left, right);
            spv_emit_no_contraction(&c->emit, value);
        } else if (op >= O_DMUL_R && op <= O_DMUL_I) {
            left = shader_double_mod(c, left, 0, B(w, 48, 1));
            value = spv_emit_fmul(&c->emit, c->f64_type, left, right);
            spv_emit_no_contraction(&c->emit, value);
        } else if (op >= O_DFMA_R && op <= O_DFMA_I) {
            u32 third;
            right = shader_double_mod(c, right, 0, B(w, 48, 1));
            third = op == O_DFMA_RC ? shader_double_source(c, vec2_u32, w, 1)
                                    : shader_double_read(c, vec2_u32, B(w, 39, 8));
            u32 addend = shader_double_mod(c, third, 0, B(w, 49, 1));
            value = spv_emit_glsl_ternary(&c->emit, c->f64_type, c->glsl, SPV_GLSL_FMA, left, right, addend);
        } else {
            left = shader_double_mod(c, left, B(w, 46, 1), B(w, 48, 1));
            right = shader_double_mod(c, right, B(w, 49, 1), B(w, 45, 1));
            u32 min = spv_emit_glsl_binary(&c->emit, c->f64_type, c->glsl, SPV_GLSL_F_MIN, left, right);
            u32 max = spv_emit_glsl_binary(&c->emit, c->f64_type, c->glsl, SPV_GLSL_F_MAX, left, right);
            u32 left_nan = spv_emit_isnan(&c->emit, c->bool_type, left);
            u32 right_nan = spv_emit_isnan(&c->emit, c->bool_type, right);
            min = spv_emit_select(&c->emit, c->f64_type, left_nan, right, min);
            max = spv_emit_select(&c->emit, c->f64_type, left_nan, right, max);
            min = spv_emit_select(&c->emit, c->f64_type, right_nan, left, min);
            max = spv_emit_select(&c->emit, c->f64_type, right_nan, left, max);
            value = spv_emit_select(&c->emit, c->f64_type, pred_read(c, B(w, 39, 3), B(w, 42, 1)), min, max);
        }
        shader_double_write(c, vec2_u32, d, value);
    } else if ((op >= O_DSET_R && op <= O_DSET_I) || (op >= O_DSETP_R && op <= O_DSETP_I)) {
        int is_set = op >= O_DSET_R && op <= O_DSET_I;
        u32 left =
            shader_double_mod(c, shader_double_read(c, vec2_u32, a), is_set ? B(w, 54, 1) : B(w, 7, 1), B(w, 43, 1));
        u32 right = shader_double_mod(c, shader_double_source(c, vec2_u32, w, is_set ? op - O_DSET_R : op - O_DSETP_R),
                                      B(w, 44, 1), is_set ? B(w, 53, 1) : B(w, 6, 1));
        u32 comparison = spv_emit_fcompare(&c->emit, c->bool_type, B(w, 48, 4), left, right);
        u32 predicate = pred_read(c, B(w, 39, 3), B(w, 42, 1));
        if (is_set) {
            u32 test = bool_combine(c, B(w, 45, 2), comparison, predicate);
            u32 truth = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 52, 1) ? 0x3f800000u : 0xffffffffu);
            u32 value = spv_emit_select(&c->emit, c->u32_type, test, truth, c->zero);
            if (B(w, 47, 1)) shader_flags_logic(c, value);
            reg_write(c, d, value);
        } else {
            u32 inverse = spv_emit_bool_not(&c->emit, c->bool_type, comparison);
            pred_write(c, B(w, 3, 3), bool_combine(c, B(w, 45, 2), comparison, predicate));
            pred_write(c, B(w, 0, 3), bool_combine(c, B(w, 45, 2), inverse, predicate));
        }
    } else if (op == O_FSET_R || op == O_FSET_C || op == O_FSET_I) {
        u32 left = reg_read_as(c, a, c->f32_type);
        u32 right = op == O_FSET_C   ? shader_cbuf_read(c, w)
                    : op == O_FSET_I ? shader_float_imm20(c, w)
                                     : reg_read_as(c, b, c->f32_type);
        left = shader_float_mod(c, left, B(w, 54, 1), B(w, 43, 1));
        right = shader_float_mod(c, right, B(w, 44, 1), B(w, 53, 1));
        u32 test = spv_emit_fcompare(&c->emit, c->bool_type, B(w, 48, 4), left, right);
        u32 pc = pred_read(c, B(w, 39, 3), B(w, 42, 1));
        test = bool_combine(c, B(w, 45, 2), test, pc);
        u32 truth = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 52, 1) ? 0x3f800000u : 0xffffffffu);
        u32 value = spv_emit_select(&c->emit, c->u32_type, test, truth, c->zero);
        if (B(w, 47, 1)) shader_flags_logic(c, value);
        reg_write(c, d, value);
    } else if (op == O_MUFU && (B(w, 20, 4) == 6 || B(w, 20, 4) == 7)) {
        /* RCP64H and RSQ64H: Ra is a double's high word (the low one 0), the result the high word of its
           reciprocal or reciprocal square root, as Mesa's nouveau uses them */
        u32 high = spv_emit_vec2_construct(&c->emit, vec2_u32, c->zero, reg_read(c, a));
        u32 value = shader_double_mod(c, spv_emit_bitcast(&c->emit, c->f64_type, high), B(w, 46, 1), B(w, 48, 1));
        value = B(w, 20, 4) == 6
                    ? spv_emit_fdiv(&c->emit, c->f64_type,
                                    spv_emit_constant64(&c->emit, c->f64_type, 0x3ff0000000000000ull), value)
                    : spv_emit_glsl_unary(&c->emit, c->f64_type, c->glsl, SPV_GLSL_INVERSE_SQRT, value);
        reg_write(c, d, spv_emit_vec4_extract(&c->emit, c->u32_type, spv_emit_bitcast(&c->emit, vec2_u32, value), 1));
    } else if (op == O_MUFU || op == O_RRO_R || op == O_RRO_C || op == O_RRO_I) {
        u32 source = op == O_MUFU ? a : b;
        u32 value = op == O_RRO_C   ? shader_cbuf_read(c, w)
                    : op == O_RRO_I ? shader_float_imm20(c, w)
                                    : reg_read_as(c, source, c->f32_type);
        u32 absolute = op == O_MUFU ? B(w, 46, 1) : B(w, 49, 1);
        u32 negative = op == O_MUFU ? B(w, 48, 1) : B(w, 45, 1);
        if (absolute) value = spv_emit_glsl_unary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_F_ABS, value);
        if (negative) value = spv_emit_fnegate(&c->emit, c->f32_type, value);
        if (op == O_MUFU) {
            SpvGlsl450Op fn = 0;
            switch (B(w, 20, 4)) {
                case 0: fn = SPV_GLSL_COS; break;
                case 1: fn = SPV_GLSL_SIN; break;
                case 2: fn = SPV_GLSL_EXP2; break;
                case 3: fn = SPV_GLSL_LOG2; break;
                case 5: fn = SPV_GLSL_INVERSE_SQRT; break;
                case 8: fn = SPV_GLSL_SQRT; break;
                case 4: {
                    u32 one = spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f);
                    value = spv_emit_fdiv(&c->emit, c->f32_type, one, value);
                    break;
                }
                default: break;
            }
            if (fn) value = spv_emit_glsl_unary(&c->emit, c->f32_type, c->glsl, fn, value);
            value = shader_float_saturate(c, value, (int)B(w, 50, 1));
        }
        reg_write_as(c, d, value, c->f32_type);
    } else if (op == O_FSWZADD) {
        /* Each lane of a quad (0 the upper left, 1 the upper right, 2 the lower left, 3 the lower right) does the
           mode in its bits of the swizzle (shader_fswzadd_lanes): a + b (0), b - a (1), a - b (2) or b (3). Nothing
           crosses lanes: compilers read the neighbour with SHFL.BFLY first, and dFdx is FSWZADD of it and x with
           0x99 (nouveau's handleDFDX, NAK's ddx). Nouveau's manual TXD fixes the order of the lanes and the
           modes: 0xcc adds the gradient in a to b on the right lanes and keeps b on the left ones. Eden and
           Ryujinx count the lanes from the low bits, and Eden negates every mode. The rounding mode (39-40) is
           round to nearest, as FADD's is, and NDV (38) changes nothing computed */
        u32 left = shader_ftz_read(c, insn, block_start, i, a, reg_read_as(c, a, c->f32_type));
        u32 right = shader_ftz_read(c, insn, block_start, i, b, reg_read_as(c, b, c->f32_type));
        int soft_ftz = B(w, 44, 1) && c->soft_ftz_on;
        if (soft_ftz) {
            left = shader_float_ftz(c, left);
            right = shader_float_ftz(c, right);
        }
        u32 lane = 0;
        if (shader_fswzadd_lanes(w, B(w, 28, 2)) != 15)
            lane = spv_emit_bitwise_and_u32(&c->emit, c->u32_type,
                                            spv_emit_load(&c->emit, c->u32_type, c->subgroup_lane_input),
                                            spv_emit_constant_u32(&c->emit, c->u32_type, 3));
        /* pick[k]: whether this lane's mode is k + 1, where some lanes' are and others' aren't */
        u32 masks[3] = {shader_fswzadd_lanes(w, 1), shader_fswzadd_lanes(w, 2), shader_fswzadd_lanes(w, 3)};
        u32 pick[3] = {0, 0, 0};
        for (int k = 0; k < 3; k++) {
            if (!masks[k] || masks[k] == 15) continue;
            u32 bit = spv_emit_bitwise_and_u32(
                &c->emit, c->u32_type,
                spv_emit_shift_right_u32(&c->emit, c->u32_type, spv_emit_constant_u32(&c->emit, c->u32_type, masks[k]),
                                         lane),
                spv_emit_constant_u32(&c->emit, c->u32_type, 1));
            pick[k] =
                spv_emit_i_not_equal(&c->emit, c->bool_type, bit, spv_emit_constant_u32(&c->emit, c->u32_type, 0));
        }
        u32 result = right;
        if (masks[2] != 15) {
            u32 lhs = left, rhs = right;
            if (masks[0] == 15) lhs = spv_emit_fnegate(&c->emit, c->f32_type, left);
            if (pick[0])
                lhs = spv_emit_select(&c->emit, c->f32_type, pick[0], spv_emit_fnegate(&c->emit, c->f32_type, left),
                                      left);
            if (masks[1] == 15) rhs = spv_emit_fnegate(&c->emit, c->f32_type, right);
            if (pick[1])
                rhs = spv_emit_select(&c->emit, c->f32_type, pick[1], spv_emit_fnegate(&c->emit, c->f32_type, right),
                                      right);
            result = spv_emit_fadd(&c->emit, c->f32_type, lhs, rhs);
            spv_emit_no_contraction(&c->emit, result);
            if (soft_ftz) result = shader_float_ftz(c, result);
            if (pick[2]) result = spv_emit_select(&c->emit, c->f32_type, pick[2], right, result);
        }
        reg_write_as(c, d, result, c->f32_type);
    } else if (shader_half_arithmetic(op)) {
        /* Each form's operands and fields, as Eden's half_floating_point_*.cpp decode them. A constant buffer
           operand is one F32 for both halves (swizzle 1). The 32I forms write both halves (no merge), take A's
           swizzle at 53, and HFMA2_32I adds Rd. FTZ isn't modelled: the halves are computed as F32. */
        int fma = op == O_HFMA2_R || op == O_HFMA2_RC || op == O_HFMA2_CR || op == O_HFMA2_I || op == O_HFMA2_32I;
        int multiply = op == O_HMUL2_R || op == O_HMUL2_C || op == O_HMUL2_I || op == O_HMUL2_32I;
        int imm32 = op == O_HADD2_32I || op == O_HMUL2_32I || op == O_HFMA2_32I;
        u32 raw_b = 0, raw_c = 0, swizzle_b = 0, swizzle_c = 0, precision = 0;
        int abs_a = 0, neg_a = 0, abs_b = 0, neg_b = 0, neg_c = 0, saturate = 0;
        switch (op) {
            case O_HADD2_R:
            case O_HMUL2_R:
                raw_b = reg_read(c, b);
                swizzle_b = B(w, 28, 2);
                abs_b = (int)B(w, 30, 1);
                neg_b = (int)B(w, 31, 1);
                saturate = (int)B(w, 32, 1);
                break;
            case O_HADD2_C:
            case O_HMUL2_C:
                raw_b = shader_cbuf_word(c, w);
                swizzle_b = 1;
                abs_b = (int)B(w, 54, 1);
                neg_b = op == O_HADD2_C ? (int)B(w, 56, 1) : 0;
                saturate = (int)B(w, 52, 1);
                break;
            case O_HADD2_I:
            case O_HMUL2_I:
                raw_b = shader_half_immediate(c, w);
                saturate = (int)B(w, 52, 1);
                break;
            case O_HADD2_32I:
            case O_HMUL2_32I:
                raw_b = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 32));
                saturate = (int)B(w, 52, 1);
                break;
            case O_HFMA2_R:
                raw_b = reg_read(c, b);
                swizzle_b = B(w, 28, 2);
                neg_b = (int)B(w, 31, 1);
                raw_c = reg_read(c, B(w, 39, 8));
                swizzle_c = B(w, 35, 2);
                neg_c = (int)B(w, 30, 1);
                saturate = (int)B(w, 32, 1);
                precision = B(w, 37, 2);
                break;
            case O_HFMA2_RC:
            case O_HFMA2_CR:
                raw_b = op == O_HFMA2_RC ? reg_read(c, B(w, 39, 8)) : shader_cbuf_word(c, w);
                swizzle_b = op == O_HFMA2_RC ? B(w, 53, 2) : 1;
                neg_b = (int)B(w, 56, 1);
                raw_c = op == O_HFMA2_RC ? shader_cbuf_word(c, w) : reg_read(c, B(w, 39, 8));
                swizzle_c = op == O_HFMA2_RC ? 1 : B(w, 53, 2);
                neg_c = (int)B(w, 51, 1);
                saturate = (int)B(w, 52, 1);
                precision = B(w, 57, 2);
                break;
            case O_HFMA2_I:
                raw_b = shader_half_immediate(c, w);
                raw_c = reg_read(c, B(w, 39, 8));
                swizzle_c = B(w, 53, 2);
                neg_c = (int)B(w, 51, 1);
                saturate = (int)B(w, 52, 1);
                precision = B(w, 57, 2);
                break;
            default: /* O_HFMA2_32I */
                raw_b = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 32));
                raw_c = reg_read(c, d);
                neg_c = (int)B(w, 52, 1);
                precision = B(w, 55, 2);
                break;
        }
        if (op == O_HADD2_32I)
            neg_a = (int)B(w, 56, 1);
        else if (!fma && !imm32) {
            abs_a = (int)B(w, 44, 1);
            neg_a = op == O_HMUL2_R ? 0 : (int)B(w, 43, 1);
        }
        if (multiply) precision = B(w, imm32 ? 55 : 39, 2);
        u32 lhs = shader_half_pair_mod(c, vec2, shader_half_pair(c, vec2, reg_read(c, a), B(w, imm32 ? 53 : 47, 2)),
                                       abs_a, neg_a);
        u32 rhs = shader_half_pair_mod(c, vec2, shader_half_pair(c, vec2, raw_b, swizzle_b), abs_b, neg_b);
        u32 addend = fma ? shader_half_pair_mod(c, vec2, shader_half_pair(c, vec2, raw_c, swizzle_c), 0, neg_c) : 0;
        u32 pair = fma        ? spv_emit_glsl_ternary(&c->emit, vec2, c->glsl, SPV_GLSL_FMA, lhs, rhs, addend)
                   : multiply ? spv_emit_fmul(&c->emit, vec2, lhs, rhs)
                              : spv_emit_fadd(&c->emit, vec2, lhs, rhs);
        spv_emit_no_contraction(&c->emit, pair);
        if ((fma || multiply) && precision == 2) pair = shader_half_fmz(c, vec2, pair, lhs, rhs, addend);
        reg_write(c, d, shader_half_pair_result(c, vec2, pair, reg_read(c, d), imm32 ? 0 : B(w, 49, 2), saturate));
    } else if (op == O_HSET2_R || op == O_HSET2_C || op == O_HSET2_I) {
        u32 raw_b;
        u32 lhs;
        u32 rhs;
        if (op == O_HSET2_R)
            raw_b = reg_read(c, b);
        else if (op == O_HSET2_C)
            raw_b = shader_cbuf_word(c, w);
        else
            raw_b = shader_half_immediate(c, w);
        lhs = shader_half_pair_mod(c, vec2, shader_half_pair(c, vec2, reg_read(c, a), B(w, 47, 2)), B(w, 44, 1),
                                   B(w, 43, 1));
        rhs = shader_half_pair_mod(c, vec2,
                                   shader_half_pair(c, vec2, raw_b,
                                                    op == O_HSET2_R   ? B(w, 28, 2)
                                                    : op == O_HSET2_C ? 1
                                                                      : 0),
                                   op == O_HSET2_R && B(w, 30, 1),
                                   op == O_HSET2_R ? B(w, 31, 1) : op == O_HSET2_C && B(w, 56, 1));
        u32 predicate = pred_read(c, B(w, 39, 3), B(w, 42, 1));
        u32 compare = B(w, op == O_HSET2_R ? 35 : 49, 4);
        u32 result = c->zero;
        for (u32 lane = 0; lane < 2; lane++) {
            u32 left = spv_emit_vec4_extract(&c->emit, c->f32_type, lhs, lane);
            u32 right = spv_emit_vec4_extract(&c->emit, c->f32_type, rhs, lane);
            u32 test = spv_emit_fcompare(&c->emit, c->bool_type, compare, left, right);
            test = bool_combine(c, B(w, 45, 2), test, predicate);
            u32 truth = B(w, op == O_HSET2_R ? 49 : 53, 1) ? 0x3c00u : 0xffffu;
            u32 bits = spv_emit_select(&c->emit, c->u32_type, test,
                                       spv_emit_constant_u32(&c->emit, c->u32_type, truth << (lane * 16)), c->zero);
            result = spv_emit_bitwise_or_u32(&c->emit, c->u32_type, result, bits);
        }
        reg_write(c, d, result);
    } else if (op == O_HSETP2_R || op == O_HSETP2_C || op == O_HSETP2_I) {
        u32 raw_b;
        u32 lhs;
        u32 rhs;
        if (op == O_HSETP2_R)
            raw_b = reg_read(c, b);
        else if (op == O_HSETP2_C)
            raw_b = shader_cbuf_word(c, w);
        else
            raw_b = shader_half_immediate(c, w);
        lhs = shader_half_pair_mod(c, vec2, shader_half_pair(c, vec2, reg_read(c, a), B(w, 47, 2)), B(w, 44, 1),
                                   B(w, 43, 1));
        rhs = shader_half_pair_mod(c, vec2,
                                   shader_half_pair(c, vec2, raw_b,
                                                    op == O_HSETP2_R   ? B(w, 28, 2)
                                                    : op == O_HSETP2_C ? 1
                                                                       : 0),
                                   (op == O_HSETP2_C && B(w, 54, 1)) || (op == O_HSETP2_R && B(w, 30, 1)),
                                   op == O_HSETP2_R ? B(w, 31, 1) : op == O_HSETP2_C && B(w, 56, 1));
        u32 predicate = pred_read(c, B(w, 39, 3), B(w, 42, 1));
        u32 compare = B(w, op == O_HSETP2_R ? 35 : 49, 4);
        u32 outcomes[2];
        for (u32 lane = 0; lane < 2; lane++) {
            u32 left = spv_emit_vec4_extract(&c->emit, c->f32_type, lhs, lane);
            u32 right = spv_emit_vec4_extract(&c->emit, c->f32_type, rhs, lane);
            u32 test = spv_emit_fcompare(&c->emit, c->bool_type, compare, left, right);
            outcomes[lane] = bool_combine(c, B(w, 45, 2), test, predicate);
        }
        if (B(w, op == O_HSETP2_R ? 49 : 53, 1)) {
            u32 both = spv_emit_bool_binary(&c->emit, c->bool_type, 0, outcomes[0], outcomes[1]);
            pred_write(c, B(w, 3, 3), both);
            pred_write(c, B(w, 0, 3), spv_emit_bool_not(&c->emit, c->bool_type, both));
        } else {
            pred_write(c, B(w, 3, 3), outcomes[0]);
            pred_write(c, B(w, 0, 3), outcomes[1]);
        }
    } else if (op >= O_FCHK_R && op <= O_FCHK_I) {
        pred_write(c, B(w, 3, 3), c->true_value);
    }
}
