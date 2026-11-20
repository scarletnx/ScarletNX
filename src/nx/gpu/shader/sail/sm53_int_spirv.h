/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sm53_int.sail's back end (after sm53_spirv.h): emit_int.c's helpers, the i64s of the video instructions, and the
   typed and raw operations emit_int made where the generic ones would fold or cast otherwise */

[[maybe_unused]] static SgV sg_IntegerAdd(SgG* g, SgV x, SgV y, int64_t avg, int64_t extended, int64_t write_cc) {
    return sg_v(shader_integer_add(g->c, sg_u32(g, x), sg_u32(g, y), (u32)avg, (int)extended, (int)write_cc), SGT_U32);
}
[[maybe_unused]] static SgV sg_IntegerCompare(SgG* g, SgV x, SgV y) {
    return sg_v(shader_integer_compare(g->c, g->t->i32_type, g->w, sg_u32(g, x), sg_u32(g, y)), SGT_BOOL);
}
[[maybe_unused]] static SgV sg_IntegerCompareNoX(SgG* g, SgV x, SgV y) {
    return sg_v(shader_integer_compare(g->c, g->t->i32_type, g->w & ~(1ull << 43), sg_u32(g, x), sg_u32(g, y)), SGT_BOOL);
}
[[maybe_unused]] static SgV sg_MulHigh(SgG* g, SgV x, SgV y, int64_t sx, int64_t sy) {
    return sg_v(shader_integer_mul_high(g->c, sg_u32(g, x), sg_u32(g, y), (int)sx, (int)sy), SGT_U32);
}
[[maybe_unused]] static SgV sg_XmadHalf(SgG* g, SgV x, int64_t high, int64_t is_signed) {
    return sg_v(shader_xmad_half(g->c, g->t->i32_type, sg_u32(g, x), (int)high, (int)is_signed), SGT_U32);
}
[[maybe_unused]] static void sg_FlagsLogic(SgG* g, SgV v) { shader_flags_logic(g->c, sg_u32(g, v)); }
[[maybe_unused]] static void sg_FlagsLogicExtended(SgG* g, SgV v, int64_t x) {
    shader_flags_logic_extended(g->c, sg_u32(g, v), (int)x);
}
[[maybe_unused]] static SgV sg_PrmtByte(SgG* g, SgV low, SgV high, SgV idx) {
    return sg_v(shader_prmt_byte(g->c, sg_u32(g, low), sg_u32(g, high), sg_u32(g, idx)), SGT_U32);
}
[[maybe_unused]] static SgV sg_VmSelect(SgG* g, SgV x, int64_t sel) {
    return sg_v(shader_vm_select(g->c, g->t->i32_type, sg_u32(g, x), (u32)sel), SGT_U32);
}
[[maybe_unused]] static SgV sg53_i64(SgG* g, u32 id) { return sg_vt(id, g->t->i64_type); }
[[maybe_unused]] static SgV sg_VideoOperand(SgG* g, SgV x, int64_t sel) {
    const Tr* t = g->t;
    return sg53_i64(g, shader_video_operand(g->c, t->i32_type, t->i64_type, t->u64_type, sg_u32(g, x), (u32)sel));
}
[[maybe_unused]] static SgV sg_VideoFinish(SgG* g, SgV result, SgV third, int64_t mode, int64_t saturate, int64_t dsigned,
                                           int64_t unsigned_nonnegative) {
    const Tr* t = g->t;
    return sg_v(shader_video_finish(g->c, t->i32_type, t->i64_type, t->u64_type, result.id, sg_u32(g, third), (u32)mode,
                                    (int)saturate, (int)dsigned, (int)unsigned_nonnegative),
                SGT_U32);
}
[[maybe_unused]] static SgV sg_I64Const(SgG* g, int64_t v) { return sg53_i64(g, spv_emit_constant64(&g->c->emit, g->t->i64_type, (u64)v)); }
[[maybe_unused]] static SgV sg_I64Add(SgG* g, SgV a, SgV b) { return sg53_i64(g, spv_emit_iadd_u32(&g->c->emit, g->t->i64_type, a.id, b.id)); }
[[maybe_unused]] static SgV sg_I64Sub(SgG* g, SgV a, SgV b) {
    return sg53_i64(g, spv_emit_binary(&g->c->emit, SPV_OP_I_SUB, g->t->i64_type, a.id, b.id));
}
[[maybe_unused]] static SgV sg_I64And(SgG* g, SgV a, SgV b) {
    return sg53_i64(g, spv_emit_binary(&g->c->emit, SPV_OP_BITWISE_AND, g->t->i64_type, a.id, b.id));
}
[[maybe_unused]] static SgV sg_I64Shl(SgG* g, SgV a, SgV n) {
    return sg53_i64(g, spv_emit_binary(&g->c->emit, SPV_OP_SHIFT_LEFT_LOGICAL, g->t->i64_type, a.id, n.id));
}
[[maybe_unused]] static SgV sg_I64Sar(SgG* g, SgV a, SgV n) {
    return sg53_i64(g, spv_emit_binary(&g->c->emit, SPV_OP_SHIFT_RIGHT_ARITHMETIC, g->t->i64_type, a.id, n.id));
}
[[maybe_unused]] static SgV sg_I64Shr(SgG* g, SgV a, SgV n) {
    return sg53_i64(g, spv_emit_binary(&g->c->emit, SPV_OP_SHIFT_RIGHT_LOGICAL, g->t->i64_type, a.id, n.id));
}
[[maybe_unused]] static SgV sg_I64Greater(SgG* g, SgV a, SgV b) {
    return sg_v(spv_emit_binary(&g->c->emit, SPV_OP_S_GREATER_THAN, g->c->bool_type, a.id, b.id), SGT_BOOL);
}
[[maybe_unused]] static SgV sg_I64Equal(SgG* g, SgV a, SgV b) {
    return sg_v(spv_emit_binary(&g->c->emit, SPV_OP_I_EQUAL, g->c->bool_type, a.id, b.id), SGT_BOOL);
}
[[maybe_unused]] static SgV sg_I64Select(SgG* g, SgV c, SgV a, SgV b) {
    return sg53_i64(g, spv_emit_ternary(&g->c->emit, SPV_OP_SELECT, g->t->i64_type, c.id, a.id, b.id));
}
[[maybe_unused]] static SgV sg_I64Of(SgG* g, SgV x, int64_t is_signed) {
    const Tr* t = g->t;
    DirectCtx* c = g->c;
    u32 v = sg_u32(g, x);
    return sg53_i64(g, is_signed ? spv_emit_int_convert(&c->emit, t->i64_type, spv_emit_unary(&c->emit, SPV_OP_BITCAST, t->i32_type, v), 1)
                                 : spv_emit_unary(&c->emit, SPV_OP_BITCAST, t->i64_type, spv_emit_int_convert(&c->emit, t->u64_type, v, 0)));
}
[[maybe_unused]] static SgV sg_CcTest(SgG* g, int64_t cond) { return sg_v(shader_cc_test(g->c, (u32)cond), SGT_BOOL); }
[[maybe_unused]] static SgV sg_SGreater(SgG* g, SgV x, SgV y) {
    DirectCtx* c = g->c;
    u32 a = spv_emit_unary(&c->emit, SPV_OP_BITCAST, g->t->i32_type, sg_u32(g, x));
    u32 b = spv_emit_unary(&c->emit, SPV_OP_BITCAST, g->t->i32_type, sg_u32(g, y));
    return sg_v(spv_emit_binary(&c->emit, SPV_OP_S_GREATER_THAN, c->bool_type, a, b), SGT_BOOL);
}
[[maybe_unused]] static SgV sg_Negative(SgG* g, SgV x) {
    DirectCtx* c = g->c;
    u32 zero = spv_emit_constant_u32(&c->emit, g->t->i32_type, 0);
    return sg_v(spv_emit_binary(&c->emit, SPV_OP_S_GREATER_THAN, c->bool_type, zero,
                                spv_emit_unary(&c->emit, SPV_OP_BITCAST, g->t->i32_type, sg_u32(g, x))),
                SGT_BOOL);
}
[[maybe_unused]] static SgV sg_AshrS(SgG* g, SgV x, SgV n) {
    DirectCtx* c = g->c;
    u32 i32_type = g->t->i32_type;
    u32 shifted = spv_emit_binary(&c->emit, SPV_OP_SHIFT_RIGHT_ARITHMETIC, i32_type,
                                  spv_emit_unary(&c->emit, SPV_OP_BITCAST, i32_type, sg_u32(g, x)), sg_u32(g, n));
    return sg_v(spv_emit_unary(&c->emit, SPV_OP_BITCAST, c->u32_type, shifted), SGT_U32);
}
[[maybe_unused]] static SgV sg_RawNot(SgG* g, SgV b) {
    return sg_v(spv_emit_unary(&g->c->emit, SPV_OP_LOGICAL_NOT, g->c->bool_type, b.id), SGT_BOOL);
}
[[maybe_unused]] static SgV sg_RawAnd(SgG* g, SgV a, SgV b) { return sg_v(spv_emit_bool_binary(&g->c->emit, g->c->bool_type, 0, a.id, b.id), SGT_BOOL); }
[[maybe_unused]] static SgV sg_RawOr(SgG* g, SgV a, SgV b) { return sg_v(spv_emit_bool_binary(&g->c->emit, g->c->bool_type, 1, a.id, b.id), SGT_BOOL); }
[[maybe_unused]] static SgV sg_RawSelect(SgG* g, SgV cond, SgV a, SgV b) {
    u32 x = sg_u32(g, a), y = sg_u32(g, b);
    return sg_v(spv_emit_ternary(&g->c->emit, SPV_OP_SELECT, g->c->u32_type, cond.id, x, y), SGT_U32);
}
/* PT or !PT picks the minimum or the maximum as the program is translated: the other one isn't made */
[[maybe_unused]] static SgV sg_MinMax(SgG* g, SgV choose_min, SgV less, SgV x, SgV y) {
    DirectCtx* c = g->c;
    u32 left = sg_u32(g, x), right = sg_u32(g, y);
    u32 min_value = choose_min.id == c->false_value ? 0 : spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->u32_type, less.id, left, right);
    u32 max_value = choose_min.id == c->true_value ? 0 : spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->u32_type, less.id, right, left);
    return sg_v(shader_select(c, c->u32_type, choose_min.id, min_value, max_value), SGT_U32);
}
[[maybe_unused]] static void sg_R2PSet(SgG* g, int64_t p, int64_t flag, SgV enabled, SgV value) {
    DirectCtx* c = g->c;
    u32 v = flag ? VAR_FLAG + (u32)p : VAR_PRED + (u32)p; /* the flags z, s, c, o */
    var_write(c, v, spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->bool_type, enabled.id, value.id, var_read(c, v)));
}
