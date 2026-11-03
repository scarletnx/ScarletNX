/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Integer, logic, video and predicate operations */
#include "nx/gpu/shader/translate.h"

u32 shader_vm_select(DirectCtx* c, u32 i32_type, u32 value, u32 select) {
    u32 lane = select & 7;
    if (lane >= 6) return value;
    u32 width = lane < 4 ? 8 : 16;
    u32 offset = lane < 4 ? lane * 8 : (lane - 4) * 16;
    u32 off_id = spv_emit_constant_u32(&c->emit, c->u32_type, offset);
    u32 width_id = spv_emit_constant_u32(&c->emit, c->u32_type, width);
    return select & 8 ? spv_emit_bitcast(&c->emit, c->u32_type,
                                         spv_emit_bitfield_sextract(&c->emit, i32_type,
                                                                    spv_emit_bitcast(&c->emit, i32_type, value), off_id,
                                                                    width_id))
                      : spv_emit_bitfield_uextract(&c->emit, c->u32_type, value, off_id, width_id);
}

/* A video operand as PTX's scalar video instructions take it: its byte, half or word (select & 7), sign-extended
   when select has bit 3, as a 64-bit integer (an s33) */
u32 shader_video_operand(DirectCtx* c, u32 i32_type, u32 i64_type, u32 u64_type, u32 value, u32 select) {
    u32 narrow = shader_vm_select(c, i32_type, value, select);
    return select & 8 ? spv_emit_int_convert(&c->emit, i64_type, spv_emit_bitcast(&c->emit, i32_type, narrow), 1)
                      : spv_emit_bitcast(&c->emit, i64_type, spv_emit_int_convert(&c->emit, u64_type, narrow, 0));
}

/* The end of a scalar video instruction, as PTX defines it: the 64-bit result clamped to the destination's range
   (signed when dsigned, the width of the merged part) with saturate, then the secondary operation with C (bits
   51-53: 0 and 1 merge into C's high or low half, 2 and 3 into its byte 0 or 2, 4 adds C, 5 and 6 take the
   minimum or maximum, 7 none), and the low 32 bits */
u32 shader_video_finish(DirectCtx* c, u32 i32_type, u32 i64_type, u32 u64_type, u32 result, u32 third, u32 mode,
                        int saturate, int dsigned, int unsigned_nonnegative) {
    if (saturate) {
        u32 width = mode < 2 ? 16 : mode < 4 ? 8 : 32;
        u64 low = dsigned ? (u64)0 - (1ull << (width - 1)) : 0;
        u64 high = dsigned ? (1ull << (width - 1)) - 1 : (1ull << width) - 1;
        u32 low_id = spv_emit_constant64(&c->emit, i64_type, low),
            high_id = spv_emit_constant64(&c->emit, i64_type, high);
        if (!unsigned_nonnegative)
            result = spv_emit_select(&c->emit, i64_type, spv_emit_s_greater(&c->emit, c->bool_type, low_id, result),
                                     low_id, result);
        u32 above = unsigned_nonnegative
                        ? spv_emit_u_less(&c->emit, c->bool_type, spv_emit_bitcast(&c->emit, u64_type, high_id),
                                          spv_emit_bitcast(&c->emit, u64_type, result))
                        : spv_emit_s_greater(&c->emit, c->bool_type, result, high_id);
        result = spv_emit_select(&c->emit, i64_type, above, high_id, result);
    }
    if (mode >= 4 && mode <= 6) {
        u32 wide = dsigned ? spv_emit_int_convert(&c->emit, i64_type, spv_emit_bitcast(&c->emit, i32_type, third), 1)
                           : spv_emit_bitcast(&c->emit, i64_type, spv_emit_int_convert(&c->emit, u64_type, third, 0));
        if (mode == 4)
            result = spv_emit_iadd_u32(&c->emit, i64_type, result, wide);
        else {
            u32 greater = spv_emit_s_greater(&c->emit, c->bool_type, result, wide);
            result = spv_emit_select(&c->emit, i64_type, greater, mode == 5 ? wide : result, mode == 5 ? result : wide);
        }
    }
    u32 value = spv_emit_int_convert(&c->emit, c->u32_type, spv_emit_bitcast(&c->emit, u64_type, result), 0);
    if (mode < 4)
        value = spv_emit_bitfield_insert(&c->emit, c->u32_type, third, value,
                                         spv_emit_constant_u32(&c->emit, c->u32_type, mode == 0 || mode == 3 ? 16 : 0),
                                         spv_emit_constant_u32(&c->emit, c->u32_type, mode < 2 ? 16 : 8));
    return value;
}

u32 shader_xmad_half(DirectCtx* c, u32 i32_type, u32 value, int high, int is_signed) {
    u32 offset = spv_emit_constant_u32(&c->emit, c->u32_type, high ? 16 : 0);
    u32 width = spv_emit_constant_u32(&c->emit, c->u32_type, 16);
    if (is_signed) {
        u32 signed_value = spv_emit_bitcast(&c->emit, i32_type, value);
        u32 half = spv_emit_bitfield_sextract(&c->emit, i32_type, signed_value, offset, width);
        return spv_emit_bitcast(&c->emit, c->u32_type, half);
    }
    return spv_emit_bitfield_uextract(&c->emit, c->u32_type, value, offset, width);
}

u32 shader_subgroup_ballot_word(DirectCtx* c, u32 vec4_u32, u32 lane, u32 predicate) {
    u32 mask = spv_emit_subgroup_ballot(&c->emit, vec4_u32, c->u32_type, predicate);
    u32 word = spv_emit_shift_right_u32(&c->emit, c->u32_type, lane, spv_emit_constant_u32(&c->emit, c->u32_type, 5));
    u32 result = spv_emit_vec4_extract(&c->emit, c->u32_type, mask, 0);
    for (u32 i = 1; i < 4; i++)
        result = spv_emit_select(
            &c->emit, c->u32_type,
            spv_emit_i_equal(&c->emit, c->bool_type, word, spv_emit_constant_u32(&c->emit, c->u32_type, i)),
            spv_emit_vec4_extract(&c->emit, c->u32_type, mask, i), result);
    return result;
}

u32 shader_integer_add(DirectCtx* c, u32 left, u32 right, u32 avg, int extended, int write_cc) {
    if (!extended && !write_cc) {
        if (avg == 2) left = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, left);
        if (avg == 1) right = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, right);
        u32 sum = spv_emit_iadd_u32(&c->emit, c->u32_type, left, right);
        return avg == 3 ? spv_emit_iadd_u32(&c->emit, c->u32_type, sum, spv_emit_constant_u32(&c->emit, c->u32_type, 1))
                        : sum;
    }
    if (avg == 2) left = spv_emit_not_u32(&c->emit, c->u32_type, left);
    if (avg == 1) right = spv_emit_not_u32(&c->emit, c->u32_type, right);
    u32 sum0 = spv_emit_iadd_u32(&c->emit, c->u32_type, left, right);
    u32 carry0 = spv_emit_u_less(&c->emit, c->bool_type, sum0, left);
    u32 cin = avg ? spv_emit_constant_u32(&c->emit, c->u32_type, 1) : c->zero;
    if (extended) {
        u32 old_carry = flag_read(c, c->cc_c);
        u32 extra =
            spv_emit_select(&c->emit, c->u32_type, old_carry, spv_emit_constant_u32(&c->emit, c->u32_type, 1), c->zero);
        cin = spv_emit_iadd_u32(&c->emit, c->u32_type, cin, extra);
    }
    u32 sum = spv_emit_iadd_u32(&c->emit, c->u32_type, sum0, cin);
    if (write_cc) {
        u32 carry1 = spv_emit_u_less(&c->emit, c->bool_type, sum, sum0);
        u32 carry = spv_emit_bool_binary(&c->emit, c->bool_type, 1, carry0, carry1);
        u32 signs = spv_emit_bitwise_xor_u32(&c->emit, c->u32_type, sum, left);
        u32 same =
            spv_emit_not_u32(&c->emit, c->u32_type, spv_emit_bitwise_xor_u32(&c->emit, c->u32_type, left, right));
        u32 bits = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, signs, same);
        u32 top = spv_emit_constant_u32(&c->emit, c->u32_type, 0x80000000u);
        u32 overflow = spv_emit_i_not_equal(&c->emit, c->bool_type,
                                            spv_emit_bitwise_and_u32(&c->emit, c->u32_type, bits, top), c->zero);
        u32 zero = spv_emit_i_equal(&c->emit, c->bool_type, sum, c->zero);
        if (extended) zero = spv_emit_bool_binary(&c->emit, c->bool_type, 0, zero, flag_read(c, c->cc_z));
        u32 sign = spv_emit_i_not_equal(&c->emit, c->bool_type,
                                        spv_emit_bitwise_and_u32(&c->emit, c->u32_type, sum, top), c->zero);
        cc_store(c, c->cc_c, carry);
        cc_store(c, c->cc_o, overflow);
        cc_store(c, c->cc_z, zero);
        cc_store(c, c->cc_s, sign);
    }
    return sum;
}
/* a lane whose writes must not land: a helper, or one a KIL inside the flow loop marked */
u32 shader_lane_dead(DirectCtx* c) {
    u32 helper = spv_emit_is_helper(&c->emit, c->bool_type);
    if (!c->discarded) return helper;
    return spv_emit_bool_binary(&c->emit, c->bool_type, 1, helper, spv_emit_load(&c->emit, c->bool_type, c->discarded));
}

u32 shader_integer_compare(DirectCtx* c, u32 i32_type, u64 w, u32 left, u32 right) {
    u32 cmp = B(w, 49, 3);
    if (cmp == 0 || cmp == 7) return cmp == 7 ? c->true_value : c->false_value;
    if (B(w, 43, 1)) {
        /* the high words decide unless equal: LE and GE take LT and GT there, equality goes to the carry */
        u64 plain = w & ~(1ull << 43);
        u64 strict = cmp == 3 ? 1 : cmp == 6 ? 4 : cmp;
        u32 base = shader_integer_compare(c, i32_type, (plain & ~(7ull << 49)) | (strict << 49), left, right);
        u32 equal = shader_integer_compare(c, i32_type, (plain & ~(7ull << 49)) | (2ull << 49), left, right);
        u32 carry = flag_read(c, c->cc_c);
        u32 zero = flag_read(c, c->cc_z);
        u32 no_carry = spv_emit_bool_not(&c->emit, c->bool_type, carry);
        if (cmp == 1 || cmp == 6)
            return spv_emit_bool_binary(
                &c->emit, c->bool_type, 1, base,
                spv_emit_bool_binary(&c->emit, c->bool_type, 0, equal, cmp == 1 ? no_carry : carry));
        if (cmp == 2) return spv_emit_bool_binary(&c->emit, c->bool_type, 0, equal, zero);
        if (cmp == 3)
            return spv_emit_bool_binary(
                &c->emit, c->bool_type, 1, base,
                spv_emit_bool_binary(&c->emit, c->bool_type, 0, equal,
                                     spv_emit_bool_binary(&c->emit, c->bool_type, 1, zero, no_carry)));
        if (cmp == 4)
            return spv_emit_bool_binary(
                &c->emit, c->bool_type, 1, base,
                spv_emit_bool_binary(&c->emit, c->bool_type, 0, equal,
                                     spv_emit_bool_binary(&c->emit, c->bool_type, 0,
                                                          spv_emit_bool_not(&c->emit, c->bool_type, zero), carry)));
        if (cmp == 5)
            return spv_emit_bool_binary(&c->emit, c->bool_type, 1, base,
                                        spv_emit_bool_not(&c->emit, c->bool_type, zero));
    }
    if (cmp == 2) return spv_emit_i_equal(&c->emit, c->bool_type, left, right);
    if (cmp == 5) return spv_emit_i_not_equal(&c->emit, c->bool_type, left, right);
    u32 less, greater;
    if (B(w, 48, 1)) {
        u32 lhs = spv_emit_bitcast(&c->emit, i32_type, left);
        u32 rhs = spv_emit_bitcast(&c->emit, i32_type, right);
        less = spv_emit_s_greater(&c->emit, c->bool_type, rhs, lhs);
        greater = spv_emit_s_greater(&c->emit, c->bool_type, lhs, rhs);
    } else {
        less = spv_emit_u_less(&c->emit, c->bool_type, left, right);
        greater = spv_emit_u_less(&c->emit, c->bool_type, right, left);
    }
    if (cmp == 1) return less;
    if (cmp == 4) return greater;
    return spv_emit_bool_not(&c->emit, c->bool_type, cmp == 3 ? greater : less);
}
u32 shader_integer_right(DirectCtx* c, u64 w, int kind) {
    if (kind == 0) return reg_read(c, B(w, 20, 8));
    if (kind == 1) return shader_cbuf_word(c, w);
    u32 imm = B(w, 20, 19);
    return spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 56, 1) ? imm | 0xfff80000u : imm);
}
/* Two's-complement signed operands correct the unsigned high word by subtracting
   the other operand when their sign bit is set. The low word needs no correction. */
u32 shader_integer_mul_high(DirectCtx* c, u32 lhs, u32 rhs, int signed_lhs, int signed_rhs) {
    u32 high = spv_emit_umul_high_u32(&c->emit, c->u32_type, lhs, rhs);
    u32 sign = spv_emit_constant_u32(&c->emit, c->u32_type, 31);
    if (signed_lhs)
        high = spv_emit_isub_u32(
            &c->emit, c->u32_type, high,
            spv_emit_imul_u32(&c->emit, c->u32_type, rhs, spv_emit_shift_right_u32(&c->emit, c->u32_type, lhs, sign)));
    if (signed_rhs)
        high = spv_emit_isub_u32(
            &c->emit, c->u32_type, high,
            spv_emit_imul_u32(&c->emit, c->u32_type, lhs, spv_emit_shift_right_u32(&c->emit, c->u32_type, rhs, sign)));
    return high;
}

/* PRMT's byte idx (0-7) of the eight in low (0-3) and high (4-7) */
u32 shader_prmt_byte(DirectCtx* c, u32 low, u32 high, u32 idx) {
    u32 shift = spv_emit_shift_left_u32(
        &c->emit, c->u32_type,
        spv_emit_bitwise_and_u32(&c->emit, c->u32_type, idx, spv_emit_constant_u32(&c->emit, c->u32_type, 3)),
        spv_emit_constant_u32(&c->emit, c->u32_type, 3));
    u32 upper = spv_emit_i_not_equal(
        &c->emit, c->bool_type,
        spv_emit_bitwise_and_u32(&c->emit, c->u32_type, idx, spv_emit_constant_u32(&c->emit, c->u32_type, 4)), c->zero);
    u32 word = spv_emit_select(&c->emit, c->u32_type, upper, high, low);
    return spv_emit_bitwise_and_u32(&c->emit, c->u32_type, spv_emit_shift_right_u32(&c->emit, c->u32_type, word, shift),
                                    spv_emit_constant_u32(&c->emit, c->u32_type, 0xff));
}

/* the integer, logic, predicate and video instructions */
void emit_int(const Tr* t, u32 i, u64 w, int op, u32 d, u32 a, u32 b) {
    DirectCtx* c = t->c;
    u32 i32_type = t->i32_type;
    u32 i64_type = t->i64_type;
    u32 u64_type = t->u64_type;
    if (op == O_MOV32I) {
        u32 value = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 32));
        reg_write(c, d, value);
    } else if (op == O_IMUL_R || op == O_IMUL_C || op == O_IMUL_I || op == O_IMUL32I || op == O_IMAD_R ||
               op == O_IMAD_RC || op == O_IMAD_CR || op == O_IMAD_I || op == O_IMAD32I) {
        int immediate32 = op == O_IMUL32I || op == O_IMAD32I;
        int mad = op >= O_IMAD_R && op <= O_IMAD32I;
        u32 lhs = reg_read(c, a);
        u32 rhs = immediate32       ? spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 32))
                  : op == O_IMAD_RC ? reg_read(c, B(w, 39, 8))
                  : op == O_IMAD_CR ? shader_cbuf_word(c, w)
                                    : shader_integer_right(c, w,
                                                           op == O_IMUL_R || op == O_IMAD_R ? 0
                                                           : op == O_IMUL_C                 ? 1
                                                                                            : 2);
        u32 addend = c->zero;
        int high;
        int signed_lhs;
        int signed_rhs;
        u32 product;
        if (mad) {
            u32 mode;
            addend = op == O_IMAD32I   ? reg_read(c, d)
                     : op == O_IMAD_RC ? shader_cbuf_word(c, w)
                                       : reg_read(c, B(w, 39, 8));
            mode = B(w, immediate32 ? 55 : 51, 2);
            if (mode == 2) rhs = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, rhs);
            if (mode == 1) addend = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, addend);
            if (mode == 3)
                addend =
                    spv_emit_iadd_u32(&c->emit, c->u32_type, addend, spv_emit_constant_u32(&c->emit, c->u32_type, 1));
            if (!immediate32 && B(w, 49, 1))
                addend = spv_emit_iadd_u32(&c->emit, c->u32_type, addend,
                                           spv_emit_select(&c->emit, c->u32_type, flag_read(c, c->cc_c),
                                                           spv_emit_constant_u32(&c->emit, c->u32_type, 1), c->zero));
        }
        high = B(w, immediate32 ? 53 : mad ? 54 : 39, 1);
        signed_lhs = B(w, immediate32 ? 54 : mad ? 48 : 40, 1);
        signed_rhs = B(w, immediate32 ? mad ? 57 : 55 : mad ? 53 : 41, 1);
        product = high ? shader_integer_mul_high(c, lhs, rhs, signed_lhs, signed_rhs)
                       : spv_emit_imul_u32(&c->emit, c->u32_type, lhs, rhs);
        reg_write(c, d, mad ? spv_emit_iadd_u32(&c->emit, c->u32_type, product, addend) : product);
    } else if (op == O_FLO_R || op == O_FLO_C || op == O_FLO_I) {
        u32 value = shader_integer_right(c, w, op - O_FLO_R);
        if (B(w, 40, 1)) value = spv_emit_not_u32(&c->emit, c->u32_type, value);
        if (B(w, 48, 1)) {
            u32 negative = spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_constant_u32(&c->emit, i32_type, 0),
                                              spv_emit_bitcast(&c->emit, i32_type, value));
            value =
                spv_emit_select(&c->emit, c->u32_type, negative, spv_emit_not_u32(&c->emit, c->u32_type, value), value);
        }
        u32 spread = value;
        for (u32 shift = 1; shift <= 16; shift <<= 1)
            spread =
                spv_emit_bitwise_or_u32(&c->emit, c->u32_type, spread,
                                        spv_emit_shift_right_u32(&c->emit, c->u32_type, spread,
                                                                 spv_emit_constant_u32(&c->emit, c->u32_type, shift)));
        u32 index = spv_emit_isub_u32(&c->emit, c->u32_type, spv_emit_bit_count(&c->emit, c->u32_type, spread),
                                      spv_emit_constant_u32(&c->emit, c->u32_type, 1));
        if (B(w, 41, 1)) {
            u32 found = spv_emit_i_not_equal(&c->emit, c->bool_type, value, c->zero);
            index = spv_emit_select(
                &c->emit, c->u32_type, found,
                spv_emit_isub_u32(&c->emit, c->u32_type, spv_emit_constant_u32(&c->emit, c->u32_type, 31), index),
                index);
        }
        reg_write(c, d, index);
    } else if (op == O_POPC_R || op == O_POPC_C || op == O_POPC_I) {
        u32 value = shader_integer_right(c, w, op - O_POPC_R);
        if (B(w, 40, 1)) value = spv_emit_not_u32(&c->emit, c->u32_type, value);
        reg_write(c, d, spv_emit_bit_count(&c->emit, c->u32_type, value));
    } else if (op == O_MOV_R || op == O_MOV_C || op == O_MOV_I) {
        if (B(w, 39, 4) == 1 || B(w, 39, 4) == 15) reg_write(c, d, shader_integer_right(c, w, op - O_MOV_R));
    } else if (op == O_IADD32I) {
        u32 immediate = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 32));
        reg_write(c, d, shader_integer_add(c, reg_read(c, a), immediate, B(w, 55, 2), B(w, 53, 1), B(w, 52, 1)));
    } else if (op == O_IADD3_R || op == O_IADD3_C || op == O_IADD3_I) {
        u32 first = reg_read(c, a);
        u32 second = shader_integer_right(c, w, op - O_IADD3_R);
        u32 third = reg_read(c, B(w, 39, 8));
        if (op == O_IADD3_R) {
            u32 values[3] = {first, second, third};
            for (u32 k = 0; k < 3; k++) {
                u32 half = B(w, 35 - 2 * k, 2);
                if (half == 1)
                    values[k] = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, values[k],
                                                         spv_emit_constant_u32(&c->emit, c->u32_type, 0xffff));
                if (half == 2)
                    values[k] = spv_emit_shift_right_u32(&c->emit, c->u32_type, values[k],
                                                         spv_emit_constant_u32(&c->emit, c->u32_type, 16));
            }
            first = values[0];
            second = values[1];
            third = values[2];
        }
        if (B(w, 51, 1)) first = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, first);
        if (B(w, 50, 1)) second = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, second);
        if (B(w, 49, 1)) third = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, third);
        u32 sum = spv_emit_iadd_u32(&c->emit, c->u32_type, first, second);
        u32 carry0 = spv_emit_u_less(&c->emit, c->bool_type, sum, first);
        u32 shift = op == O_IADD3_R ? B(w, 37, 2) : 0;
        if (shift == 1) /* the 33-bit sum shifted: its carry becomes bit 16 */
            sum = spv_emit_bitwise_or_u32(
                &c->emit, c->u32_type,
                spv_emit_shift_right_u32(&c->emit, c->u32_type, sum, spv_emit_constant_u32(&c->emit, c->u32_type, 16)),
                spv_emit_select(&c->emit, c->u32_type, carry0, spv_emit_constant_u32(&c->emit, c->u32_type, 0x10000),
                                c->zero));
        if (shift == 2)
            sum = spv_emit_shift_left_u32(&c->emit, c->u32_type, sum, spv_emit_constant_u32(&c->emit, c->u32_type, 16));
        u32 sum2 = spv_emit_iadd_u32(&c->emit, c->u32_type, sum, third);
        u32 carry1 = spv_emit_u_less(&c->emit, c->bool_type, sum2, sum);
        u32 carry2 = c->false_value;
        if (B(w, 48, 1)) {
            u32 cin = spv_emit_select(&c->emit, c->u32_type, flag_read(c, c->cc_c),
                                      spv_emit_constant_u32(&c->emit, c->u32_type, 1), c->zero);
            u32 final = spv_emit_iadd_u32(&c->emit, c->u32_type, sum2, cin);
            carry2 = spv_emit_u_less(&c->emit, c->bool_type, final, sum2);
            sum2 = final;
        }
        if (B(w, 47, 1)) {
            u32 carry = spv_emit_bool_binary(&c->emit, c->bool_type, 1, carry1, carry2);
            if (shift != 1 && shift != 2) carry = spv_emit_bool_binary(&c->emit, c->bool_type, 1, carry, carry0);
            u32 zero = spv_emit_i_equal(&c->emit, c->bool_type, sum2, c->zero);
            if (B(w, 48, 1)) zero = spv_emit_bool_binary(&c->emit, c->bool_type, 0, zero, flag_read(c, c->cc_z));
            u32 sign =
                spv_emit_i_not_equal(&c->emit, c->bool_type,
                                     spv_emit_bitwise_and_u32(&c->emit, c->u32_type, sum2,
                                                              spv_emit_constant_u32(&c->emit, c->u32_type, 0x80000000)),
                                     c->zero);
            cc_store(c, c->cc_c, carry);
            cc_store(c, c->cc_z, zero);
            cc_store(c, c->cc_s, sign);
        }
        reg_write(c, d, sum2);
    } else if (op == O_IADD_R || op == O_IADD_C || op == O_IADD_I) {
        u32 left = reg_read(c, a), right = shader_integer_right(c, w, op - O_IADD_R);
        reg_write(c, d, shader_integer_add(c, left, right, B(w, 48, 2), B(w, 43, 1), B(w, 47, 1)));
    } else if (op == O_LOP32I) {
        u32 left = reg_read(c, a);
        u32 right = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 32));
        if (B(w, 55, 1)) left = spv_emit_not_u32(&c->emit, c->u32_type, left);
        if (B(w, 56, 1)) right = spv_emit_not_u32(&c->emit, c->u32_type, right);
        u32 value;
        switch (B(w, 53, 2)) {
            case 0: value = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, left, right); break;
            case 1: value = spv_emit_bitwise_or_u32(&c->emit, c->u32_type, left, right); break;
            case 2: value = spv_emit_bitwise_xor_u32(&c->emit, c->u32_type, left, right); break;
            default: value = right; break;
        }
        reg_write(c, d, value);
        if (B(w, 52, 1)) shader_flags_logic_extended(c, value, (int)B(w, 57, 1));
    } else if (op == O_LOP_R || op == O_LOP_C || op == O_LOP_I) {
        u32 left = reg_read(c, a), right = shader_integer_right(c, w, op - O_LOP_R);
        if (B(w, 39, 1)) left = spv_emit_not_u32(&c->emit, c->u32_type, left);
        if (B(w, 40, 1)) right = spv_emit_not_u32(&c->emit, c->u32_type, right);
        u32 value;
        switch (B(w, 41, 2)) {
            case 0: value = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, left, right); break;
            case 1: value = spv_emit_bitwise_or_u32(&c->emit, c->u32_type, left, right); break;
            case 2: value = spv_emit_bitwise_xor_u32(&c->emit, c->u32_type, left, right); break;
            default: value = right; break;
        }
        reg_write(c, d, value);
        if (B(w, 47, 1)) shader_flags_logic_extended(c, value, (int)B(w, 43, 1));
        if (B(w, 48, 3) < 7) {
            u32 mode = B(w, 44, 2);
            u32 predicate = mode == 0   ? c->false_value
                            : mode == 1 ? c->true_value
                            : mode == 2 ? spv_emit_i_equal(&c->emit, c->bool_type, value, c->zero)
                                        : spv_emit_i_not_equal(&c->emit, c->bool_type, value, c->zero);
            pred_write(c, B(w, 48, 3), predicate);
        }
    } else if (op == O_LOP3_R || op == O_LOP3_C || op == O_LOP3_I) {
        u32 first = reg_read(c, a), second = shader_integer_right(c, w, op - O_LOP3_R);
        u32 third = reg_read(c, B(w, 39, 8));
        u32 lut = op == O_LOP3_R ? B(w, 28, 8) : B(w, 48, 8);
        u32 value = c->zero;
        for (u32 bit = 0; bit < 8; bit++) {
            if (!(lut & (1u << bit))) continue;
            u32 x = bit & 4 ? first : spv_emit_not_u32(&c->emit, c->u32_type, first);
            u32 y = bit & 2 ? second : spv_emit_not_u32(&c->emit, c->u32_type, second);
            u32 z = bit & 1 ? third : spv_emit_not_u32(&c->emit, c->u32_type, third);
            u32 term = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, x, y);
            term = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, term, z);
            value = spv_emit_bitwise_or_u32(&c->emit, c->u32_type, value, term);
        }
        reg_write(c, d, value);
        /* LOP3.CC is bit 47; X is bit 38 for R, 56 for C and 57 for I. */
        if (B(w, 47, 1))
            shader_flags_logic_extended(c, value, (int)B(w, op == O_LOP3_R ? 38 : op == O_LOP3_C ? 56 : 57, 1));
        if (op == O_LOP3_R && B(w, 48, 3) < 7) {
            u32 mode = B(w, 36, 2);
            u32 predicate = mode == 0   ? c->false_value
                            : mode == 1 ? c->true_value
                            : mode == 2 ? spv_emit_i_equal(&c->emit, c->bool_type, value, c->zero)
                                        : spv_emit_i_not_equal(&c->emit, c->bool_type, value, c->zero);
            pred_write(c, B(w, 48, 3), predicate);
        }
    } else if (op == O_SEL_R || op == O_SEL_C || op == O_SEL_I) {
        u32 condition = pred_read(c, B(w, 39, 3), B(w, 42, 1));
        u32 rhs = shader_integer_right(c, w, op - O_SEL_R);
        reg_write(c, d, spv_emit_select(&c->emit, c->u32_type, condition, reg_read(c, a), rhs));
    } else if (op >= O_XMAD_R && op <= O_XMAD_I) {
        /* Each form's operands and fields, as Eden reads them: B is Rb (R), Rc at bit 39 (RC), a constant buffer
           word (CR) or a 16-bit immediate (I), and C is Rc or the constant buffer word. The product of A's and
           B's halves (signed by bits 48 and 49) is shifted up 16 by PSL; the select picks C, its low or high
           half, or adds B << 16 to it (CBCC); MRG puts B's low half in the result's high half. X adds the carry
           and CC writes the flags, as IADD's do (Eden refuses both). */
        int narrow = op == O_XMAD_RC || op == O_XMAD_CR;
        u32 source_b = op == O_XMAD_R    ? reg_read(c, b)
                       : op == O_XMAD_RC ? reg_read(c, B(w, 39, 8))
                       : op == O_XMAD_CR ? shader_cbuf_word(c, w)
                                         : spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 16));
        u32 source_c = op == O_XMAD_RC ? shader_cbuf_word(c, w) : reg_read(c, B(w, 39, 8));
        u32 select = narrow ? B(w, 50, 2) : B(w, 50, 3);
        int half_b = op == O_XMAD_R ? (int)B(w, 35, 1) : narrow ? (int)B(w, 52, 1) : 0;
        int psl = op == O_XMAD_CR ? (int)B(w, 55, 1) : op == O_XMAD_RC ? 0 : (int)B(w, 36, 1);
        int mrg = op == O_XMAD_CR ? (int)B(w, 56, 1) : op == O_XMAD_RC ? 0 : (int)B(w, 37, 1);
        int extended = narrow ? (int)B(w, 54, 1) : (int)B(w, 38, 1);
        u32 sixteen = spv_emit_constant_u32(&c->emit, c->u32_type, 16);
        u32 left = shader_xmad_half(c, i32_type, reg_read(c, a), B(w, 53, 1), B(w, 48, 1));
        u32 right = shader_xmad_half(c, i32_type, source_b, half_b, B(w, 49, 1));
        u32 product = spv_emit_imul_u32(&c->emit, c->u32_type, left, right);
        if (psl) product = spv_emit_shift_left_u32(&c->emit, c->u32_type, product, sixteen);
        u32 third = select == 1   ? shader_xmad_half(c, i32_type, source_c, 0, 0)
                    : select == 2 ? shader_xmad_half(c, i32_type, source_c, 1, 0)
                    : select == 4
                        ? spv_emit_iadd_u32(&c->emit, c->u32_type,
                                            spv_emit_shift_left_u32(&c->emit, c->u32_type, source_b, sixteen), source_c)
                        : source_c;
        u32 sum = shader_integer_add(c, product, third, 0, extended, (int)B(w, 47, 1));
        if (mrg) {
            u32 low_mask = spv_emit_constant_u32(&c->emit, c->u32_type, 0xffff);
            sum = spv_emit_bitwise_or_u32(
                &c->emit, c->u32_type, spv_emit_bitwise_and_u32(&c->emit, c->u32_type, sum, low_mask),
                spv_emit_shift_left_u32(&c->emit, c->u32_type,
                                        spv_emit_bitwise_and_u32(&c->emit, c->u32_type, source_b, low_mask), sixteen));
        }
        reg_write(c, d, sum);
    } else if (op == O_VADD || op == O_VABSDIFF || op == O_VSET || op == O_VSHL || op == O_VSHR) {
        /* Ryujinx's decoding and PTX's semantics: A's and B's selects at 36 and 28 with their signs at 48 and 49
           (the shifts' B is unsigned, and bit 49 wraps their count), B a 16-bit immediate when bit 50 is clear,
           a signed destination at 54 and saturate at 55 (VSET's comparison is there instead), the secondary
           operation at 51-53, and VADD's negation/plus-one mode at 56-57 */
        int shift = op == O_VSHL || op == O_VSHR;
        u32 asel = B(w, 36, 3) | (B(w, 48, 1) << 3);
        u32 bsel = B(w, 28, 3) | (shift ? 0 : B(w, 49, 1) << 3);
        u32 left = shader_video_operand(c, i32_type, i64_type, u64_type, reg_read(c, a), asel);
        u32 right = B(w, 50, 1) ? shader_video_operand(c, i32_type, i64_type, u64_type, reg_read(c, b), bsel)
                                : shader_video_operand(c, i32_type, i64_type, u64_type,
                                                       spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 16)),
                                                       4 | (bsel & 8));
        u32 zero64 = spv_emit_constant64(&c->emit, i64_type, 0), one64 = spv_emit_constant64(&c->emit, i64_type, 1);
        u32 result;
        if (op == O_VADD) {
            u32 mode = B(w, 56, 2);
            result = mode == 1   ? spv_emit_isub_u32(&c->emit, i64_type, left, right)
                     : mode == 2 ? spv_emit_isub_u32(&c->emit, i64_type, right, left)
                                 : spv_emit_iadd_u32(&c->emit, i64_type, left, right);
            if (mode == 3) result = spv_emit_iadd_u32(&c->emit, i64_type, result, one64);
        } else if (op == O_VABSDIFF) {
            u32 difference = spv_emit_isub_u32(&c->emit, i64_type, left, right);
            result = spv_emit_select(&c->emit, i64_type, spv_emit_s_greater(&c->emit, c->bool_type, zero64, difference),
                                     spv_emit_isub_u32(&c->emit, i64_type, zero64, difference), difference);
        } else if (op == O_VSET) {
            u32 compare = B(w, 54, 3);
            u32 less = spv_emit_s_greater(&c->emit, c->bool_type, right, left);
            u32 greater = spv_emit_s_greater(&c->emit, c->bool_type, left, right);
            u32 equal = spv_emit_i_equal(&c->emit, c->bool_type, left, right);
            u32 test = compare == 0   ? c->false_value
                       : compare == 1 ? less
                       : compare == 2 ? equal
                       : compare == 3 ? cc_not(c, greater)
                       : compare == 4 ? greater
                       : compare == 5 ? cc_not(c, equal)
                       : compare == 6 ? cc_not(c, less)
                                      : c->true_value;
            result = spv_emit_select(&c->emit, i64_type, test, one64, zero64);
        } else {
            /* the count wraps to 5 bits (bit 49) or clamps at 32; VSHR fills with A's sign when A is signed */
            u32 count = B(w, 49, 1) ? spv_emit_bitwise_and_u32(&c->emit, i64_type, right,
                                                               spv_emit_constant64(&c->emit, i64_type, 31))
                                    : spv_emit_select(&c->emit, i64_type,
                                                      spv_emit_s_greater(&c->emit, c->bool_type, right,
                                                                         spv_emit_constant64(&c->emit, i64_type, 32)),
                                                      spv_emit_constant64(&c->emit, i64_type, 32), right);
            result = op == O_VSHL  ? spv_emit_shift_left_u32(&c->emit, i64_type, left, count)
                     : B(w, 48, 1) ? spv_emit_shift_right_s32(&c->emit, i64_type, left, count)
                                   : spv_emit_shift_right_u32(&c->emit, i64_type, left, count);
        }
        reg_write(c, d,
                  shader_video_finish(c, i32_type, i64_type, u64_type, result, reg_read(c, B(w, 39, 8)), B(w, 51, 3),
                                      op != O_VSET && B(w, 55, 1), op != O_VSET && B(w, 54, 1),
                                      op == O_VSHL && !B(w, 48, 1)));
    } else if (op >= O_PRMT_R && op <= O_PRMT_I) {
        /* Bytes 0-3 are Ra's and 4-7 C's (Rc at bit 39, or the constant buffer word in the RC form). The selector
           (Rb, Rc in the RC form, the constant buffer word, or the immediate) picks each result byte: in the
           index mode (0) its nibble i, whose bit 3 replicates the byte's sign; in the others by c, its low two
           bits, as PTX's prmt defines F4E (1), B4E (2), RC8 (3), ECL (4), ECR (5) and RC16 (6). */
        u32 low = reg_read(c, a);
        u32 selector = op == O_PRMT_R    ? reg_read(c, b)
                       : op == O_PRMT_RC ? reg_read(c, B(w, 39, 8))
                       : op == O_PRMT_CR ? shader_cbuf_word(c, w)
                                         : spv_emit_constant_u32(&c->emit, c->u32_type,
                                                                 B(w, 20, 19) | (B(w, 56, 1) ? 0xfff80000u : 0));
        u32 high = op == O_PRMT_RC ? shader_cbuf_word(c, w) : reg_read(c, B(w, 39, 8));
        u32 mode = B(w, 48, 3), result = c->zero;
        u32 three = spv_emit_constant_u32(&c->emit, c->u32_type, 3);
        u32 sel = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, selector, three);
        for (u32 k = 0; k < 4; k++) {
            u32 position = spv_emit_constant_u32(&c->emit, c->u32_type, k), idx, byte;
            u32 nibble = 0;
            switch (mode) {
                case 0:
                    nibble = spv_emit_bitwise_and_u32(
                        &c->emit, c->u32_type,
                        spv_emit_shift_right_u32(&c->emit, c->u32_type, selector,
                                                 spv_emit_constant_u32(&c->emit, c->u32_type, 4 * k)),
                        spv_emit_constant_u32(&c->emit, c->u32_type, 15));
                    idx = nibble;
                    break;
                case 1: idx = spv_emit_iadd_u32(&c->emit, c->u32_type, sel, position); break;
                case 2: idx = spv_emit_isub_u32(&c->emit, c->u32_type, sel, position); break;
                case 3: idx = sel; break;
                case 4:
                    idx = spv_emit_select(&c->emit, c->u32_type, spv_emit_u_less(&c->emit, c->bool_type, sel, position),
                                          position, sel);
                    break;
                case 5:
                    idx = spv_emit_select(&c->emit, c->u32_type, spv_emit_u_less(&c->emit, c->bool_type, sel, position),
                                          sel, position);
                    break;
                default:
                    idx =
                        spv_emit_iadd_u32(&c->emit, c->u32_type,
                                          spv_emit_shift_left_u32(
                                              &c->emit, c->u32_type,
                                              spv_emit_bitwise_and_u32(&c->emit, c->u32_type, sel,
                                                                       spv_emit_constant_u32(&c->emit, c->u32_type, 1)),
                                              spv_emit_constant_u32(&c->emit, c->u32_type, 1)),
                                          spv_emit_constant_u32(&c->emit, c->u32_type, k & 1));
                    break;
            }
            byte = shader_prmt_byte(c, low, high, idx);
            if (mode == 0) { /* a nibble's bit 3 replicates the byte's sign across it */
                u32 replicate =
                    spv_emit_i_not_equal(&c->emit, c->bool_type,
                                         spv_emit_bitwise_and_u32(&c->emit, c->u32_type, nibble,
                                                                  spv_emit_constant_u32(&c->emit, c->u32_type, 8)),
                                         c->zero);
                u32 negative =
                    spv_emit_i_not_equal(&c->emit, c->bool_type,
                                         spv_emit_bitwise_and_u32(&c->emit, c->u32_type, byte,
                                                                  spv_emit_constant_u32(&c->emit, c->u32_type, 0x80)),
                                         c->zero);
                byte = spv_emit_select(&c->emit, c->u32_type, replicate,
                                       spv_emit_select(&c->emit, c->u32_type, negative,
                                                       spv_emit_constant_u32(&c->emit, c->u32_type, 0xff), c->zero),
                                       byte);
            }
            result =
                spv_emit_bitwise_or_u32(&c->emit, c->u32_type, result,
                                        spv_emit_shift_left_u32(&c->emit, c->u32_type, byte,
                                                                spv_emit_constant_u32(&c->emit, c->u32_type, 8 * k)));
        }
        reg_write(c, d, result);
    } else if (op == O_PSET) {
        u32 pa = pred_read(c, B(w, 12, 3), B(w, 15, 1));
        u32 pb = pred_read(c, B(w, 29, 3), B(w, 32, 1));
        u32 pc = pred_read(c, B(w, 39, 3), B(w, 42, 1));
        u32 test = bool_combine(c, B(w, 45, 2), bool_combine(c, B(w, 24, 2), pa, pb), pc);
        u32 value = spv_emit_select(
            &c->emit, c->u32_type, test,
            spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 44, 1) ? 0x3f800000u : 0xffffffffu), c->zero);
        if (B(w, 47, 1)) shader_flags_logic(c, value);
        reg_write(c, d, value);
    } else if (op == O_PSETP) {
        u32 pa = pred_read(c, B(w, 12, 3), B(w, 15, 1));
        u32 pb = pred_read(c, B(w, 29, 3), B(w, 32, 1));
        u32 pc = pred_read(c, B(w, 39, 3), B(w, 42, 1));
        u32 first = bool_combine(c, B(w, 24, 2), pa, pb);
        u32 second = bool_combine(c, B(w, 24, 2), spv_emit_bool_not(&c->emit, c->bool_type, pa), pb);
        pred_write(c, B(w, 3, 3), bool_combine(c, B(w, 45, 2), first, pc));
        pred_write(c, B(w, 0, 3), bool_combine(c, B(w, 45, 2), second, pc));
    } else if (op == O_CSETP || op == O_CSET) {
        u32 test = shader_cc_test(c, B(w, 8, 5));
        u32 condition = pred_read(c, B(w, 39, 3), B(w, 42, 1));
        u32 yes = bool_combine(c, B(w, 45, 2), test, condition);
        if (op == O_CSET) {
            u32 truth = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 44, 1) ? 0x3f800000u : 0xffffffffu);
            u32 result = spv_emit_select(&c->emit, c->u32_type, yes, truth, c->zero);
            if (B(w, 47, 1)) shader_flags_logic(c, result);
            reg_write(c, d, result);
        } else {
            pred_write(c, B(w, 3, 3), yes);
            pred_write(c, B(w, 0, 3), bool_combine(c, B(w, 45, 2), cc_not(c, test), condition));
        }
    } else if (op >= O_P2R_R && op <= O_P2R_I) {
        u32 mask = shader_integer_right(c, w, op - O_P2R_R);
        if (op != O_P2R_I)
            mask = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, mask,
                                            spv_emit_constant_u32(&c->emit, c->u32_type, 255));
        u32 bits = c->zero;
        u32 flags[4] = {VAR_FLAG, VAR_FLAG + 1, VAR_FLAG + 2, VAR_FLAG + 3}; /* z, s, c, o */
        for (u32 p = 0; p < (B(w, 40, 1) ? 4u : 7u); p++) {
            u32 value = B(w, 40, 1) ? var_read(c, flags[p]) : pred_read(c, p, 0);
            u32 bit = spv_emit_select(&c->emit, c->u32_type, value,
                                      spv_emit_constant_u32(&c->emit, c->u32_type, 1u << p), c->zero);
            bits = spv_emit_bitwise_or_u32(&c->emit, c->u32_type, bits, bit);
        }
        u32 shift = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 41, 2) * 8);
        u32 shifted_mask = spv_emit_shift_left_u32(&c->emit, c->u32_type, mask, shift);
        u32 base = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, reg_read(c, a),
                                            spv_emit_not_u32(&c->emit, c->u32_type, shifted_mask));
        u32 picked = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, bits, mask);
        u32 shifted = spv_emit_shift_left_u32(&c->emit, c->u32_type, picked, shift);
        reg_write(c, d, spv_emit_bitwise_or_u32(&c->emit, c->u32_type, base, shifted));
    } else if (op >= O_R2P_R && op <= O_R2P_I) {
        u32 source = reg_read(c, a);
        if (B(w, 41, 2))
            source = spv_emit_shift_right_u32(&c->emit, c->u32_type, source,
                                              spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 41, 2) * 8));
        u32 mask = shader_integer_right(c, w, op - O_R2P_R);
        u32 flags[4] = {VAR_FLAG, VAR_FLAG + 1, VAR_FLAG + 2, VAR_FLAG + 3}; /* z, s, c, o */
        for (u32 p = 0; p < (B(w, 40, 1) ? 4u : 7u); p++) {
            u32 enabled =
                spv_emit_i_not_equal(&c->emit, c->bool_type,
                                     spv_emit_bitwise_and_u32(&c->emit, c->u32_type, mask,
                                                              spv_emit_constant_u32(&c->emit, c->u32_type, 1u << p)),
                                     c->zero);
            u32 bit = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, source,
                                               spv_emit_constant_u32(&c->emit, c->u32_type, 1u << p));
            u32 value = spv_emit_i_not_equal(&c->emit, c->bool_type, bit, c->zero);
            u32 v = B(w, 40, 1) ? flags[p] : VAR_PRED + p;
            var_write(c, v, spv_emit_select(&c->emit, c->bool_type, enabled, value, var_read(c, v)));
        }
    } else if (op == O_ISETP_R || op == O_ISETP_C || op == O_ISETP_I) {
        u32 left = reg_read(c, a), right = shader_integer_right(c, w, op - O_ISETP_R);
        u32 test = shader_integer_compare(c, i32_type, w, left, right);
        u32 condition = pred_read(c, B(w, 39, 3), B(w, 42, 1));
        u32 yes = bool_combine(c, B(w, 45, 2), test, condition);
        u32 no = bool_combine(c, B(w, 45, 2), spv_emit_bool_not(&c->emit, c->bool_type, test), condition);
        pred_write(c, B(w, 3, 3), yes);
        pred_write(c, B(w, 0, 3), no);
    } else if (op == O_ISET_R || op == O_ISET_C || op == O_ISET_I) {
        u32 left = reg_read(c, a), right = shader_integer_right(c, w, op - O_ISET_R);
        u32 test = shader_integer_compare(c, i32_type, w, left, right);
        u32 condition = pred_read(c, B(w, 39, 3), B(w, 42, 1));
        test = bool_combine(c, B(w, 45, 2), test, condition);
        u32 truth = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 44, 1) ? 0x3f800000u : 0xffffffffu);
        u32 result = spv_emit_select(&c->emit, c->u32_type, test, truth, c->zero);
        if (B(w, 47, 1)) shader_flags_logic(c, result);
        reg_write(c, d, result);
    } else if (op == O_IMNMX_R || op == O_IMNMX_C || op == O_IMNMX_I) {
        u32 left = reg_read(c, a), right = shader_integer_right(c, w, op - O_IMNMX_R);
        u32 less = B(w, 48, 1) ? spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_bitcast(&c->emit, i32_type, right),
                                                    spv_emit_bitcast(&c->emit, i32_type, left))
                               : spv_emit_u_less(&c->emit, c->bool_type, left, right);
        u32 min_value = spv_emit_select(&c->emit, c->u32_type, less, left, right);
        u32 max_value = spv_emit_select(&c->emit, c->u32_type, less, right, left);
        u32 choose_min = pred_read(c, B(w, 39, 3), B(w, 42, 1));
        u32 value = spv_emit_select(&c->emit, c->u32_type, choose_min, min_value, max_value);
        if (B(w, 47, 1)) shader_flags_logic(c, value);
        reg_write(c, d, value);
    } else if (op == O_VSETP) {
        /* Each operand is independently sign- or zero-extended to 33 bits before comparing. */
        u32 left = shader_vm_select(c, i32_type, reg_read(c, a), B(w, 36, 3) | (B(w, 48, 1) << 3));
        u32 right = B(w, 50, 1)
                        ? shader_vm_select(c, i32_type, reg_read(c, b), B(w, 28, 3) | (B(w, 49, 1) << 3))
                        : shader_vm_select(c, i32_type, spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 16)),
                                           4 | (B(w, 49, 1) << 3));
        u32 left64 = B(w, 48, 1)
                         ? spv_emit_int_convert(&c->emit, i64_type, spv_emit_bitcast(&c->emit, i32_type, left), 1)
                         : spv_emit_bitcast(&c->emit, i64_type, spv_emit_int_convert(&c->emit, u64_type, left, 0));
        u32 right64 = B(w, 49, 1)
                          ? spv_emit_int_convert(&c->emit, i64_type, spv_emit_bitcast(&c->emit, i32_type, right), 1)
                          : spv_emit_bitcast(&c->emit, i64_type, spv_emit_int_convert(&c->emit, u64_type, right, 0));
        u32 less = spv_emit_s_greater(&c->emit, c->bool_type, right64, left64);
        u32 greater = spv_emit_s_greater(&c->emit, c->bool_type, left64, right64);
        u32 equal = spv_emit_i_equal(&c->emit, c->bool_type, left64, right64);
        u32 cmp = B(w, 43, 2) | (B(w, 47, 1) << 2);
        u32 result = cmp == 0   ? c->false_value
                     : cmp == 1 ? less
                     : cmp == 2 ? equal
                     : cmp == 3 ? cc_not(c, greater)
                     : cmp == 4 ? greater
                     : cmp == 5 ? cc_not(c, equal)
                     : cmp == 6 ? cc_not(c, less)
                                : c->true_value;
        u32 condition = pred_read(c, B(w, 39, 3), B(w, 42, 1));
        pred_write(c, B(w, 3, 3), bool_combine(c, B(w, 45, 2), result, condition));
        pred_write(c, B(w, 0, 3), bool_combine(c, B(w, 45, 2), cc_not(c, result), condition));
    } else if (op == O_VMAD) {
        u32 left = shader_vm_select(c, i32_type, reg_read(c, a), B(w, 36, 3) | (B(w, 48, 1) << 3));
        u32 right = B(w, 50, 1)
                        ? shader_vm_select(c, i32_type, reg_read(c, b), B(w, 28, 3) | (B(w, 49, 1) << 3))
                        : shader_vm_select(c, i32_type, spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 16)),
                                           4 | (B(w, 49, 1) << 3));
        u32 product = spv_emit_imul_u32(&c->emit, c->u32_type, left, right);
        reg_write(c, d, spv_emit_iadd_u32(&c->emit, c->u32_type, product, reg_read(c, B(w, 39, 8))));
    } else if (op == O_VMNMX) {
        u32 asel = B(w, 36, 3) | (B(w, 48, 1) << 3);
        u32 bsel = B(w, 28, 3) | (B(w, 49, 1) << 3);
        int sa = asel >> 3, sb = bsel >> 3, maximum = B(w, 56, 1);
        u32 va = shader_vm_select(c, i32_type, reg_read(c, a), asel);
        u32 vb = B(w, 50, 1)
                     ? shader_vm_select(c, i32_type, reg_read(c, b), bsel)
                     : spv_emit_constant_u32(&c->emit, c->u32_type, sb ? (u32)(i32)(i16)B(w, 20, 16) : B(w, 20, 16));
        u32 less;
        if (sa == sb) {
            less = sa ? spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_bitcast(&c->emit, i32_type, vb),
                                           spv_emit_bitcast(&c->emit, i32_type, va))
                      : spv_emit_u_less(&c->emit, c->bool_type, va, vb);
        } else if (sa) {
            u32 negative = spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_constant_u32(&c->emit, i32_type, 0),
                                              spv_emit_bitcast(&c->emit, i32_type, va));
            less = spv_emit_bool_binary(&c->emit, c->bool_type, 1, negative,
                                        spv_emit_u_less(&c->emit, c->bool_type, va, vb));
        } else {
            u32 nonnegative = spv_emit_bool_not(&c->emit, c->bool_type,
                                                spv_emit_s_greater(&c->emit, c->bool_type,
                                                                   spv_emit_constant_u32(&c->emit, i32_type, 0),
                                                                   spv_emit_bitcast(&c->emit, i32_type, vb)));
            less = spv_emit_bool_binary(&c->emit, c->bool_type, 0, nonnegative,
                                        spv_emit_u_less(&c->emit, c->bool_type, va, vb));
        }
        u32 result = spv_emit_select(&c->emit, c->u32_type, less, maximum ? vb : va, maximum ? va : vb);
        int result_signed = sa == sb ? sa : !maximum;
        int signed_format = B(w, 54, 1);
        if (B(w, 55, 1)) {
            if (signed_format && !result_signed) {
                u32 cap = spv_emit_constant_u32(&c->emit, c->u32_type, 0x7fffffffu);
                u32 above = spv_emit_u_less(&c->emit, c->bool_type, cap, result);
                result = spv_emit_select(&c->emit, c->u32_type, above, cap, result);
            } else if (!signed_format && result_signed) {
                u32 negative = spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_constant_u32(&c->emit, i32_type, 0),
                                                  spv_emit_bitcast(&c->emit, i32_type, result));
                result = spv_emit_select(&c->emit, c->u32_type, negative, c->zero, result);
            }
        }
        u32 third = reg_read(c, B(w, 39, 8));
        u32 mode = B(w, 51, 3);
        if (mode < 4) {
            u32 off = mode == 0 || mode == 3 ? 16 : 0;
            u32 width = mode == 2 || mode == 3 ? 8 : 16;
            result = spv_emit_bitfield_insert(&c->emit, c->u32_type, third, result,
                                              spv_emit_constant_u32(&c->emit, c->u32_type, off),
                                              spv_emit_constant_u32(&c->emit, c->u32_type, width));
        } else if (mode == 4) {
            result = spv_emit_iadd_u32(&c->emit, c->u32_type, result, third);
        } else if (mode == 5 || mode == 6) {
            u32 cmp = signed_format
                          ? spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_bitcast(&c->emit, i32_type, result),
                                               spv_emit_bitcast(&c->emit, i32_type, third))
                          : spv_emit_u_less(&c->emit, c->bool_type, third, result);
            result =
                spv_emit_select(&c->emit, c->u32_type, cmp, mode == 5 ? third : result, mode == 5 ? result : third);
        }
        reg_write(c, d, result);
    } else if (op == O_ICMP_R || op == O_ICMP_RC || op == O_ICMP_CR || op == O_ICMP_I) {
        u32 third = reg_read(c, B(w, 39, 8));
        u32 right = op == O_ICMP_RC || op == O_ICMP_CR ? shader_cbuf_word(c, w)
                    : op == O_ICMP_I                   ? shader_integer_right(c, w, 2)
                                                       : reg_read(c, b);
        u32 comparison = op == O_ICMP_RC ? right : third;
        u32 alternative = op == O_ICMP_RC ? third : right;
        /* ICMP has no .X: bit 43 is part of Rc */
        u32 test = shader_integer_compare(c, i32_type, w & ~(1ull << 43), comparison, c->zero);
        reg_write(c, d, spv_emit_select(&c->emit, c->u32_type, test, reg_read(c, a), alternative));
    } else if (op == O_BFI_R || op == O_BFI_RC || op == O_BFI_CR || op == O_BFI_I) {
        u32 base = op == O_BFI_RC ? shader_cbuf_word(c, w) : reg_read(c, B(w, 39, 8));
        u32 packed = op == O_BFI_CR   ? shader_cbuf_word(c, w)
                     : op == O_BFI_RC ? reg_read(c, B(w, 39, 8))
                     : op == O_BFI_I  ? shader_integer_right(c, w, 2)
                                      : reg_read(c, b);
        u32 mask = spv_emit_constant_u32(&c->emit, c->u32_type, 255);
        u32 offset = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, packed, mask);
        u32 width = spv_emit_bitwise_and_u32(
            &c->emit, c->u32_type,
            spv_emit_shift_right_u32(&c->emit, c->u32_type, packed, spv_emit_constant_u32(&c->emit, c->u32_type, 8)),
            mask);
        u32 thirty_two = spv_emit_constant_u32(&c->emit, c->u32_type, 32);
        u32 valid = spv_emit_u_less(&c->emit, c->bool_type, offset, thirty_two);
        u32 safe_offset = spv_emit_select(&c->emit, c->u32_type, valid, offset, c->zero);
        u32 remaining = spv_emit_isub_u32(&c->emit, c->u32_type, thirty_two, safe_offset);
        u32 fits = spv_emit_u_less(&c->emit, c->bool_type, width, remaining);
        width = spv_emit_select(&c->emit, c->u32_type, fits, width, remaining);
        u32 value = spv_emit_bitfield_insert(&c->emit, c->u32_type, base, reg_read(c, a), safe_offset, width);
        value = spv_emit_select(&c->emit, c->u32_type, valid, value, base);
        if (B(w, 47, 1)) shader_flags_logic(c, value);
        reg_write(c, d, value);
    } else if (op == O_I2I_R || op == O_I2I_C || op == O_I2I_I) {
        u32 value = shader_integer_right(c, w, op - O_I2I_R);
        u32 src_size = B(w, 10, 2), dst_size = B(w, 8, 2);
        if (src_size < 2) {
            u32 offset = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 41, 3) * 8);
            u32 width = spv_emit_constant_u32(&c->emit, c->u32_type, 8u << src_size);
            value = B(w, 13, 1)
                        ? spv_emit_bitcast(&c->emit, c->u32_type,
                                           spv_emit_bitfield_sextract(&c->emit, i32_type,
                                                                      spv_emit_bitcast(&c->emit, i32_type, value),
                                                                      offset, width))
                        : spv_emit_bitfield_uextract(&c->emit, c->u32_type, value, offset, width);
        }
        if (B(w, 49, 1)) {
            u32 negative = spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_constant_u32(&c->emit, i32_type, 0),
                                              spv_emit_bitcast(&c->emit, i32_type, value));
            u32 absolute = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, value);
            value = spv_emit_select(&c->emit, c->u32_type, negative, absolute, value);
        }
        if (B(w, 45, 1)) value = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, value);
        if (B(w, 50, 1) && dst_size < 2) {
            u32 high = (B(w, 12, 1) ? 0x7fffffffu : 0xffffffffu) >> (32 - (8u << dst_size));
            u32 hi = spv_emit_constant_u32(&c->emit, c->u32_type, high);
            if (B(w, 13, 1)) {
                u32 negative = spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_constant_u32(&c->emit, i32_type, 0),
                                                  spv_emit_bitcast(&c->emit, i32_type, value));
                if (B(w, 12, 1)) {
                    u32 lo = spv_emit_constant_u32(&c->emit, c->u32_type, ~high);
                    u32 below = spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_bitcast(&c->emit, i32_type, lo),
                                                   spv_emit_bitcast(&c->emit, i32_type, value));
                    value = spv_emit_select(&c->emit, c->u32_type, below, lo, value);
                } else
                    value = spv_emit_select(&c->emit, c->u32_type, negative, c->zero, value);
            }
            u32 above = B(w, 13, 1) && B(w, 12, 1)
                            ? spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_bitcast(&c->emit, i32_type, value),
                                                 spv_emit_bitcast(&c->emit, i32_type, hi))
                            : spv_emit_u_less(&c->emit, c->bool_type, hi, value);
            value = spv_emit_select(&c->emit, c->u32_type, above, hi, value);
        } else if (B(w, 50, 1) && B(w, 12, 1) != B(w, 13, 1)) {
            /* 32 bits saturate only across a change of signedness */
            if (B(w, 13, 1))
                value = spv_emit_select(&c->emit, c->u32_type,
                                        spv_emit_s_greater(&c->emit, c->bool_type,
                                                           spv_emit_constant_u32(&c->emit, i32_type, 0),
                                                           spv_emit_bitcast(&c->emit, i32_type, value)),
                                        c->zero, value);
            else {
                u32 top = spv_emit_constant_u32(&c->emit, c->u32_type, 0x7fffffffu);
                value = spv_emit_select(&c->emit, c->u32_type, spv_emit_u_less(&c->emit, c->bool_type, top, value), top,
                                        value);
            }
        }
        if (dst_size < 2) {
            u32 width = spv_emit_constant_u32(&c->emit, c->u32_type, 8u << dst_size);
            value = spv_emit_bitfield_uextract(&c->emit, c->u32_type, value, c->zero, width);
        }
        if (B(w, 47, 1)) shader_flags_logic(c, value);
        reg_write(c, d, value);
    } else if (op == O_ISCADD_R || op == O_ISCADD_C || op == O_ISCADD_I || op == O_ISCADD32I) {
        /* ISCADD32I: a 32-bit immediate, the shift in bits 53-57 and CC in bit 52, and no negation */
        int immediate32 = op == O_ISCADD32I;
        u32 left = reg_read(c, a);
        u32 avg = immediate32 ? 0 : B(w, 48, 2);
        u32 shifted = spv_emit_shift_left_u32(
            &c->emit, c->u32_type, left, spv_emit_constant_u32(&c->emit, c->u32_type, B(w, immediate32 ? 53 : 39, 5)));
        u32 rhs = immediate32 ? spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 32))
                              : shader_integer_right(c, w, op - O_ISCADD_R);
        reg_write(c, d, shader_integer_add(c, shifted, rhs, avg, 0, B(w, immediate32 ? 52 : 47, 1)));
    } else if (op >= O_BFE_R && op <= O_BFE_I) {
        u32 packed = shader_integer_right(c, w, op - O_BFE_R);
        u32 source = reg_read(c, a);
        if (B(w, 40, 1)) source = spv_emit_bit_reverse_u32(&c->emit, c->u32_type, source);
        u32 raw_offset =
            spv_emit_bitwise_and_u32(&c->emit, c->u32_type, packed, spv_emit_constant_u32(&c->emit, c->u32_type, 255));
        u32 thirty_one = spv_emit_constant_u32(&c->emit, c->u32_type, 31);
        u32 offset =
            spv_emit_select(&c->emit, c->u32_type, spv_emit_u_less(&c->emit, c->bool_type, thirty_one, raw_offset),
                            thirty_one, raw_offset);
        u32 raw_width = spv_emit_bitwise_and_u32(
            &c->emit, c->u32_type,
            spv_emit_shift_right_u32(&c->emit, c->u32_type, packed, spv_emit_constant_u32(&c->emit, c->u32_type, 8)),
            spv_emit_constant_u32(&c->emit, c->u32_type, 255));
        u32 remaining =
            spv_emit_isub_u32(&c->emit, c->u32_type, spv_emit_constant_u32(&c->emit, c->u32_type, 32), offset);
        u32 width = spv_emit_select(
            &c->emit, c->u32_type, spv_emit_u_less(&c->emit, c->bool_type, remaining, raw_width), remaining, raw_width);
        u32 nonzero = spv_emit_i_not_equal(&c->emit, c->bool_type, width, c->zero);
        if (!B(w, 48, 1))
            nonzero = cc_and(
                c, nonzero,
                spv_emit_u_less(&c->emit, c->bool_type, raw_offset, spv_emit_constant_u32(&c->emit, c->u32_type, 32)));
        u32 safe_width =
            spv_emit_select(&c->emit, c->u32_type, nonzero, width, spv_emit_constant_u32(&c->emit, c->u32_type, 1));
        u32 extracted = B(w, 48, 1)
                            ? spv_emit_bitcast(&c->emit, c->u32_type,
                                               spv_emit_bitfield_sextract(&c->emit, i32_type,
                                                                          spv_emit_bitcast(&c->emit, i32_type, source),
                                                                          offset, safe_width))
                            : spv_emit_bitfield_uextract(&c->emit, c->u32_type, source, offset, safe_width);
        u32 value = spv_emit_select(&c->emit, c->u32_type, nonzero, extracted, c->zero);
        if (B(w, 47, 1)) shader_flags_logic(c, value); /* BFE.CC, as Eden writes it */
        reg_write(c, d, value);
    } else if (op == O_SHL_R || op == O_SHL_C || op == O_SHL_I) {
        u32 shift = shader_integer_right(c, w, op - O_SHL_R);
        u32 safe = B(w, 39, 1) ? spv_emit_bitwise_and_u32(&c->emit, c->u32_type, shift,
                                                          spv_emit_constant_u32(&c->emit, c->u32_type, 31))
                               : shift;
        u32 value = spv_emit_shift_left_u32(&c->emit, c->u32_type, reg_read(c, a), safe);
        if (!B(w, 39, 1)) {
            u32 bound = spv_emit_constant_u32(&c->emit, c->u32_type, 32);
            u32 valid = spv_emit_u_less(&c->emit, c->bool_type, shift, bound);
            value = spv_emit_select(&c->emit, c->u32_type, valid, value, c->zero);
        }
        reg_write(c, d, value);
    } else if (op == O_LEA_LR || op == O_LEA_LC || op == O_LEA_LI || op == O_LEA_HR || op == O_LEA_HC) {
        int high_form = op == O_LEA_HR || op == O_LEA_HC;
        u32 shift = high_form ? B(w, op == O_LEA_HR ? 28 : 51, 5) : B(w, 39, 5);
        int negative = high_form ? B(w, op == O_LEA_HR ? 37 : 56, 1) : B(w, 45, 1);
        u32 source = reg_read(c, a);
        u32 rhs = op == O_LEA_LR || op == O_LEA_HR ? reg_read(c, b)
                  : op == O_LEA_LI                 ? shader_integer_right(c, w, 2)
                                                   : shader_integer_right(c, w, 1);
        u32 value;
        int extended;
        u32 old_carry;
        u32 sum;
        if (high_form) {
            u32 hi = reg_read(c, B(w, 39, 8));
            value = shift ? spv_emit_bitwise_or_u32(
                                &c->emit, c->u32_type,
                                spv_emit_shift_right_u32(&c->emit, c->u32_type, source,
                                                         spv_emit_constant_u32(&c->emit, c->u32_type, 32 - shift)),
                                spv_emit_shift_left_u32(&c->emit, c->u32_type, hi,
                                                        spv_emit_constant_u32(&c->emit, c->u32_type, shift)))
                          : hi;
            if (negative) {
                u32 carry =
                    spv_emit_i_equal(&c->emit, c->bool_type,
                                     spv_emit_shift_left_u32(&c->emit, c->u32_type, source,
                                                             spv_emit_constant_u32(&c->emit, c->u32_type, shift)),
                                     c->zero);
                value = spv_emit_iadd_u32(&c->emit, c->u32_type, spv_emit_not_u32(&c->emit, c->u32_type, value),
                                          spv_emit_select(&c->emit, c->u32_type, carry,
                                                          spv_emit_constant_u32(&c->emit, c->u32_type, 1), c->zero));
            }
        } else {
            if (negative) source = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, source);
            value = spv_emit_shift_left_u32(&c->emit, c->u32_type, source,
                                            spv_emit_constant_u32(&c->emit, c->u32_type, shift));
        }
        /* .X adds the carry in (LO at 46, HI at 38 or 57), .CC writes the flags, and LO's predicate takes the
           add's carry out: LEA.LO ... P0 then LEA.HI.X is how a 64-bit address is formed */
        extended = B(w, op == O_LEA_HR ? 38 : op == O_LEA_HC ? 57 : 46, 1);
        old_carry = extended ? flag_read(c, c->cc_c) : 0;
        sum = shader_integer_add(c, value, rhs, 0, extended, B(w, 47, 1));
        reg_write(c, d, sum);
        if (B(w, 48, 3) < 7) {
            u32 carry = spv_emit_u_less(&c->emit, c->bool_type, sum, value);
            if (extended)
                carry =
                    spv_emit_bool_binary(&c->emit, c->bool_type, 1, carry,
                                         spv_emit_bool_binary(&c->emit, c->bool_type, 0, old_carry,
                                                              spv_emit_i_equal(&c->emit, c->bool_type, sum, value)));
            pred_write(c, B(w, 48, 3), carry);
        }
    } else if (op == O_SHF_LR || op == O_SHF_LI || op == O_SHF_RR || op == O_SHF_RI) {
        int left_shift = op == O_SHF_LR || op == O_SHF_LI;
        u32 shift = (op == O_SHF_LR || op == O_SHF_RR) ? reg_read(c, b)
                                                       : spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 19));
        u32 longest = B(w, 37, 2) >= 2;
        u32 limit = spv_emit_constant_u32(&c->emit, c->u32_type, longest ? 63 : 32);
        u32 wrapped = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, shift,
                                               spv_emit_constant_u32(&c->emit, c->u32_type, longest ? 63 : 31));
        u32 safe = B(w, 50, 1) ? wrapped
                               : spv_emit_select(&c->emit, c->u32_type,
                                                 spv_emit_u_less(&c->emit, c->bool_type, limit, shift), limit, shift);
        u32 low = reg_read(c, a), high = reg_read(c, B(w, 39, 8));
        u32 small =
            spv_emit_bitwise_and_u32(&c->emit, c->u32_type, safe, spv_emit_constant_u32(&c->emit, c->u32_type, 31));
        u32 left = spv_emit_shift_left_u32(&c->emit, c->u32_type, high, small);
        u32 reverse = spv_emit_isub_u32(&c->emit, c->u32_type, spv_emit_constant_u32(&c->emit, c->u32_type, 32), safe);
        reverse =
            spv_emit_bitwise_and_u32(&c->emit, c->u32_type, reverse, spv_emit_constant_u32(&c->emit, c->u32_type, 31));
        u32 right = spv_emit_shift_right_u32(&c->emit, c->u32_type, low, reverse);
        u32 combined = spv_emit_bitwise_or_u32(&c->emit, c->u32_type, left, right);
        if (left_shift)
            combined = spv_emit_select(&c->emit, c->u32_type, spv_emit_i_equal(&c->emit, c->bool_type, safe, c->zero),
                                       high, combined);
        else {
            u32 low_part = spv_emit_shift_right_u32(&c->emit, c->u32_type, low, small);
            u32 high_part = spv_emit_shift_left_u32(&c->emit, c->u32_type, high, reverse);
            combined = spv_emit_bitwise_or_u32(&c->emit, c->u32_type, low_part, high_part);
            combined = spv_emit_select(&c->emit, c->u32_type, spv_emit_i_equal(&c->emit, c->bool_type, safe, c->zero),
                                       low, combined);
        }
        u32 long_shift =
            spv_emit_isub_u32(&c->emit, c->u32_type, safe, spv_emit_constant_u32(&c->emit, c->u32_type, 32));
        long_shift = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, long_shift,
                                              spv_emit_constant_u32(&c->emit, c->u32_type, 31));
        u32 long_value =
            left_shift ? spv_emit_shift_left_u32(&c->emit, c->u32_type, low, long_shift)
            : B(w, 37, 2) == 3
                ? spv_emit_bitcast(&c->emit, c->u32_type,
                                   spv_emit_shift_right_s32(&c->emit, i32_type,
                                                            spv_emit_bitcast(&c->emit, i32_type, high), long_shift))
                : spv_emit_shift_right_u32(&c->emit, c->u32_type, high, long_shift);
        u32 result = spv_emit_select(
            &c->emit, c->u32_type,
            spv_emit_u_less(&c->emit, c->bool_type, safe, spv_emit_constant_u32(&c->emit, c->u32_type, 32)), combined,
            long_value);
        reg_write(c, d, result);
    } else if (op == O_SHR_R || op == O_SHR_C || op == O_SHR_I) {
        u32 shift = shader_integer_right(c, w, op - O_SHR_R);
        u32 source = reg_read(c, a);
        if (B(w, 40, 1)) source = spv_emit_bit_reverse_u32(&c->emit, c->u32_type, source);
        u32 safe =
            spv_emit_bitwise_and_u32(&c->emit, c->u32_type, shift, spv_emit_constant_u32(&c->emit, c->u32_type, 31));
        u32 value;
        if (B(w, 48, 1)) {
            if (!B(w, 39, 1)) {
                u32 below =
                    spv_emit_u_less(&c->emit, c->bool_type, shift, spv_emit_constant_u32(&c->emit, c->u32_type, 31));
                safe = spv_emit_select(&c->emit, c->u32_type, below, shift,
                                       spv_emit_constant_u32(&c->emit, c->u32_type, 31));
            }
            value = spv_emit_bitcast(
                &c->emit, c->u32_type,
                spv_emit_shift_right_s32(&c->emit, i32_type, spv_emit_bitcast(&c->emit, i32_type, source), safe));
        } else {
            value = spv_emit_shift_right_u32(&c->emit, c->u32_type, source, safe);
        }
        if (!B(w, 48, 1) && !B(w, 39, 1)) {
            u32 valid =
                spv_emit_u_less(&c->emit, c->bool_type, shift, spv_emit_constant_u32(&c->emit, c->u32_type, 32));
            value = spv_emit_select(&c->emit, c->u32_type, valid, value, c->zero);
        }
        reg_write(c, d, value);
    }
}
