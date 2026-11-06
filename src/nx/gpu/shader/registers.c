/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The program's registers, predicates and condition flags as SPIR-V values: the function variables, the
   values kept in the block being written, flush to zero, and the flag tests */
#include "nx/gpu/shader/translate.h"

/* Before a merge or a branch: the block's last values into their variables. One written in a block that ended in a
   return or a kill, which store nothing, is dropped */
void shader_var_flush(void* user) {
    DirectCtx* c = user;
    for (u32 v = 0; v < VAR_COUNT; v++) {
        if (!c->var_dirty[v]) continue;
        c->var_dirty[v] = 0;
        if (c->var_block[v] != c->emit.block) continue;
        if (!c->var_val[v]) c->var_val[v] = spv_emit_bitcast(&c->emit, c->u32_type, c->reg_alt[v]);
        spv_emit_store(&c->emit, var_of(c, v), c->var_val[v]);
    }
}
/* whether register r holds a flushed value when instruction i reads it: the last instruction that may write it in the
   straight-line code before i (no way in between) is an unpredicated FTZ float instruction, or, past where control
   may come in, nothing but FTZ float instructions writes it anywhere (tainted) */
static int reg_flushed_at(const DirectCtx* c, const u64* insn, const u8* block_start, u32 i, u32 r) {
    if (r >= 255) return 1; /* RZ: zero */
    if (!c->taint_known) return 0;
    if (!block_start || !block_start[i])
        for (u32 j = i; j-- > 11;) {
            if (!IDX_OK(j)) continue;
            if (shader_may_write(insn[j], r))
                return shader_ftz_writer(insn[j]) && B(insn[j], 16, 3) == 7 && !B(insn[j], 19, 1);
            if (block_start && block_start[j]) break;
        }
    return !c->tainted[r];
}
/* value, a read of register r at instruction i, noted flushed when it is (reg_flushed_at) */
u32 shader_ftz_read(DirectCtx* c, const u64* insn, const u8* block_start, u32 i, u32 r, u32 value) {
    if (reg_flushed_at(c, insn, block_start, i, r)) shader_ftz_mark(c, value);
    return value;
}

void shader_flags_logic(DirectCtx* c, u32 value) {
    u32 sign = spv_emit_i_not_equal(&c->emit, c->bool_type,
                                    spv_emit_bitwise_and_u32(&c->emit, c->u32_type, value,
                                                             spv_emit_constant_u32(&c->emit, c->u32_type, 0x80000000u)),
                                    c->zero);
    cc_store(c, c->cc_c, c->false_value);
    cc_store(c, c->cc_o, c->false_value);
    cc_store(c, c->cc_z, spv_emit_i_equal(&c->emit, c->bool_type, value, c->zero));
    cc_store(c, c->cc_s, sign);
}

/* LOP, LOP32I and LOP3 flags: X retains zero only when the previous word was also zero, as IADD.X chains across
   the words of a wider value. */
void shader_flags_logic_extended(DirectCtx* c, u32 value, int extended) {
    u32 previous = extended ? flag_read(c, c->cc_z) : 0;
    u32 zero = spv_emit_i_equal(&c->emit, c->bool_type, value, c->zero);
    if (extended) zero = spv_emit_bool_binary(&c->emit, c->bool_type, 0, previous, zero);
    u32 sign = spv_emit_i_not_equal(&c->emit, c->bool_type,
                                    spv_emit_bitwise_and_u32(&c->emit, c->u32_type, value,
                                                             spv_emit_constant_u32(&c->emit, c->u32_type, 0x80000000u)),
                                    c->zero);
    cc_store(c, c->cc_z, zero);
    cc_store(c, c->cc_s, sign);
}
u32 shader_cc_test(DirectCtx* c, u32 code) {
    if (code == 0 || code == 15) return code == 15 ? c->true_value : c->false_value;
    u32 s = flag_read(c, c->cc_s);
    u32 z = flag_read(c, c->cc_z);
    u32 carry = flag_read(c, c->cc_c);
    u32 overflow = flag_read(c, c->cc_o);
    switch (code) {
        case 1: return cc_xor(c, cc_and(c, s, cc_not(c, z)), overflow);
        case 2: return cc_and(c, cc_not(c, s), z);
        case 3: return cc_xor(c, s, cc_or(c, z, overflow));
        case 4: return cc_not(c, cc_or(c, cc_xor(c, s, overflow), z));
        case 5: return cc_not(c, z);
        case 6: return cc_not(c, cc_xor(c, s, overflow));
        case 7: return cc_not(c, cc_and(c, s, z));
        case 8: return cc_and(c, s, z);
        case 9: return cc_xor(c, s, overflow);
        case 10: return z;
        case 11: return cc_or(c, cc_xor(c, s, overflow), z);
        case 12: return cc_xor(c, cc_not(c, s), cc_or(c, overflow, z));
        case 13: return cc_or(c, s, cc_not(c, z));
        case 14: return cc_xor(c, cc_or(c, cc_not(c, s), z), overflow);
        case 16: return cc_not(c, overflow);
        case 17: return cc_not(c, carry);
        case 18: return cc_not(c, s);
        case 19: return cc_or(c, z, cc_not(c, carry));
        case 20: return cc_and(c, carry, cc_not(c, z));
        case 21: return s;
        case 22: return carry;
        case 23: return overflow;
        case 30: return cc_or(c, s, z);
        case 31: return cc_not(c, cc_or(c, s, z));
        default: return c->false_value;
    }
}

/* whether value is one float_ftz made, or one known to have no subnormal */
int shader_ftz_has(const DirectCtx* c, u32 value) {
    return (size_t)value / 8 < c->ftz_bytes && (c->ftz[value / 8] >> (value % 8) & 1);
}
void shader_ftz_mark(DirectCtx* c, u32 value) {
    if ((size_t)value / 8 >= c->ftz_bytes) {
        size_t bytes = ((size_t)value / 8 + 1) * 2;
        u8* grown = (u8*)realloc(c->ftz, bytes);
        if (!grown) return; /* not noted: flushed again where it is used */
        memset(grown + c->ftz_bytes, 0, bytes - c->ftz_bytes);
        c->ftz = grown;
        c->ftz_bytes = bytes;
    }
    c->ftz[value / 8] |= (u8)(1u << (value % 8));
}

/* An FTZ arithmetic instruction consumes and produces signed zero instead of a subnormal. Done here per operand and
   result when the Vulkan device can't request DenormFlushToZero, or when this shader mixes FTZ and non-FTZ
   instructions: with a float test where the module keeps signed zeros, else on the bits (spv_emit_flush_subnormal) */
u32 shader_float_ftz(DirectCtx* c, u32 value) {
    u32 type, constant;
    /* a value flushed once has no subnormal left, wherever it is used; a constant is flushed here */
    if (shader_ftz_has(c, value)) return value;
    if (spv_emit_constant_bits(&c->emit, value, &type, &constant) && type == c->f32_type)
        return spv_emit_constant_u32(&c->emit, c->f32_type, constant & 0x7f800000u ? constant : constant & 0x80000000u);
    if (value < c->ftz_of_cap && c->ftz_of[value] && c->ftz_of_block[value] == c->emit.block) return c->ftz_of[value];
    u32 result =
        spv_emit_flush_subnormal(&c->emit, c->f32_type, c->u32_type, c->bool_type, c->glsl, value, c->ftz_float);
    shader_ftz_mark(c, result);
    if (value >= c->ftz_of_cap) {
        size_t cap = ((size_t)value + 1) * 2;
        u32* of = (u32*)realloc(c->ftz_of, cap * sizeof *of);
        u32* block = of ? (u32*)realloc(c->ftz_of_block, cap * sizeof *block) : NULL;
        if (of) c->ftz_of = of;
        if (!block) return result; /* not noted: flushed again where it is used */
        memset(of + c->ftz_of_cap, 0, (cap - c->ftz_of_cap) * sizeof *of);
        c->ftz_of_block = block;
        c->ftz_of_cap = cap;
    }
    c->ftz_of[value] = result;
    c->ftz_of_block[value] = c->emit.block;
    return result;
}
