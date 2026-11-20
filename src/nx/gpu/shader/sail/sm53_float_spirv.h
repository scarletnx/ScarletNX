/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sm53_float.sail's back end (after sm53_spirv.h): emit_float.c's helpers, the doubles' and the halves' pairs, and the
   operations emit_float made with NoContraction, each as it made them */

[[maybe_unused]] static SgV sg_RegF(SgG* g, int64_t r) {
    const Tr* t = g->t;
    DirectCtx* c = g->c;
    return sg_v(shader_ftz_read(c, t->insn, t->block_start, g->i, (u32)r, reg_read_as(c, (u32)r, c->f32_type)), SGT_F32);
}
[[maybe_unused]] static SgV sg_FloatMod(SgG* g, SgV x, int64_t absolute, int64_t negative) {
    return sg_v(shader_float_mod(g->c, sg_f(g, x), (int)absolute, (int)negative), SGT_F32);
}
[[maybe_unused]] static SgV sg_CbufF(SgG* g) { return sg_v(shader_cbuf_read(g->c, g->w), SGT_F32); }
[[maybe_unused]] static SgV sg_Imm20F(SgG* g) { return sg_v(shader_float_imm20(g->c, g->w), SGT_F32); }
[[maybe_unused]] static SgV sg_Imm32F(SgG* g) { return sg_v(shader_float_imm32(g->c, g->w), SGT_F32); }
[[maybe_unused]] static SgV sg_ImmBits20(SgG* g) {
    DirectCtx* c = g->c;
    return sg_v(shader_cast(c, c->u32_type, shader_float_imm20(c, g->w), c->f32_type), SGT_U32);
}
[[maybe_unused]] static int sg_SoftFtz(SgG* g) { return g->c->soft_ftz_on; }
[[maybe_unused]] static SgV sg_FloatFtz(SgG* g, SgV x) { return sg_v(shader_float_ftz(g->c, sg_f(g, x)), SGT_F32); }
[[maybe_unused]] static SgV sg53_nc(SgG* g, u32 v) {
    spv_emit_no_contraction(&g->c->emit, v);
    return sg_v(v, SGT_F32);
}
[[maybe_unused]] static SgV sg_FAddNC(SgG* g, SgV x, SgV y) {
    u32 a = sg_f(g, x), b = sg_f(g, y);
    return sg53_nc(g, spv_emit_binary(&g->c->emit, SPV_OP_F_ADD, g->c->f32_type, a, b));
}
[[maybe_unused]] static SgV sg_FMulNC(SgG* g, SgV x, SgV y) {
    u32 a = sg_f(g, x), b = sg_f(g, y);
    return sg53_nc(g, spv_emit_binary(&g->c->emit, SPV_OP_F_MUL, g->c->f32_type, a, b));
}
/* the guest's FMA rounds once; an Fma without NoContraction may be split into a multiply and an add */
[[maybe_unused]] static SgV sg_FmaNC(SgG* g, SgV x, SgV y, SgV z) {
    DirectCtx* c = g->c;
    u32 a = sg_f(g, x), b = sg_f(g, y), d = sg_f(g, z);
    return sg53_nc(g, spv_emit_glsl_ternary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_FMA, a, b, d));
}
/* PT or !PT picks the minimum or the maximum as the program is translated: the other one isn't made */
[[maybe_unused]] static SgV sg_MinMaxF(SgG* g, SgV choose_min, SgV x, SgV y) {
    DirectCtx* c = g->c;
    u32 left = sg_f(g, x), right = sg_f(g, y);
    u32 minimum = choose_min.id == c->false_value ? 0 : spv_emit_glsl_binary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_F_MIN, left, right);
    u32 maximum = choose_min.id == c->true_value ? 0 : spv_emit_glsl_binary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_F_MAX, left, right);
    return sg_v(shader_select(c, c->f32_type, choose_min.id, minimum, maximum), SGT_F32);
}
[[maybe_unused]] static SgV sg53_half(SgG* g, u32 id) { return sg_vt(id, g->t->vec2); }
[[maybe_unused]] static SgV sg_HalfPairOf(SgG* g, SgV raw, int64_t swizzle) {
    return sg53_half(g, shader_half_pair(g->c, g->t->vec2, sg_u32(g, raw), (u32)swizzle));
}
[[maybe_unused]] static SgV sg_HalfPairMod(SgG* g, SgV pair, int64_t absolute, int64_t negative) {
    return sg53_half(g, shader_half_pair_mod(g->c, g->t->vec2, pair.id, (int)absolute, (int)negative));
}
[[maybe_unused]] static SgV sg_HalfImmediate(SgG* g) { return sg_v(shader_half_immediate(g->c, g->w), SGT_U32); }
[[maybe_unused]] static SgV sg_Half2FmaNC(SgG* g, SgV a, SgV b, SgV d) {
    DirectCtx* c = g->c;
    u32 v = spv_emit_glsl_ternary(&c->emit, g->t->vec2, c->glsl, SPV_GLSL_FMA, a.id, b.id, d.id);
    spv_emit_no_contraction(&c->emit, v);
    return sg53_half(g, v);
}
[[maybe_unused]] static SgV sg_Half2MulNC(SgG* g, SgV a, SgV b) {
    u32 v = spv_emit_binary(&g->c->emit, SPV_OP_F_MUL, g->t->vec2, a.id, b.id);
    spv_emit_no_contraction(&g->c->emit, v);
    return sg53_half(g, v);
}
[[maybe_unused]] static SgV sg_Half2AddNC(SgG* g, SgV a, SgV b) {
    u32 v = spv_emit_binary(&g->c->emit, SPV_OP_F_ADD, g->t->vec2, a.id, b.id);
    spv_emit_no_contraction(&g->c->emit, v);
    return sg53_half(g, v);
}
[[maybe_unused]] static SgV sg_HalfFmz(SgG* g, SgV pair, SgV lhs, SgV rhs) {
    return sg53_half(g, shader_half_fmz(g->c, g->t->vec2, pair.id, lhs.id, rhs.id, 0));
}
[[maybe_unused]] static SgV sg_HalfFmzFma(SgG* g, SgV pair, SgV lhs, SgV rhs, SgV addend) {
    return sg53_half(g, shader_half_fmz(g->c, g->t->vec2, pair.id, lhs.id, rhs.id, addend.id));
}
[[maybe_unused]] static SgV sg_HalfPairResult(SgG* g, SgV pair, SgV old, int64_t merge, int64_t saturate) {
    return sg_v(shader_half_pair_result(g->c, g->t->vec2, pair.id, sg_u32(g, old), (u32)merge, (int)saturate), SGT_U32);
}
[[maybe_unused]] static SgV sg53_f64(SgG* g, u32 id) { return sg_vt(id, g->c->f64_type); }
[[maybe_unused]] static SgV sg_DoubleRead(SgG* g, int64_t r) { return sg53_f64(g, shader_double_read(g->c, g->t->vec2_u32, (u32)r)); }
[[maybe_unused]] static SgV sg_DoubleSource(SgG* g, int64_t kind) {
    return sg53_f64(g, shader_double_source(g->c, g->t->vec2_u32, g->w, (int)kind));
}
[[maybe_unused]] static SgV sg_DoubleMod(SgG* g, SgV x, int64_t absolute, int64_t negative) {
    return sg53_f64(g, shader_double_mod(g->c, x.id, (int)absolute, (int)negative));
}
[[maybe_unused]] static void sg_DoubleWrite(SgG* g, int64_t r, SgV x) { shader_double_write(g->c, g->t->vec2_u32, (u32)r, x.id); }
[[maybe_unused]] static SgV sg_DAddNC(SgG* g, SgV a, SgV b) {
    u32 v = spv_emit_binary(&g->c->emit, SPV_OP_F_ADD, g->c->f64_type, a.id, b.id);
    spv_emit_no_contraction(&g->c->emit, v);
    return sg53_f64(g, v);
}
[[maybe_unused]] static SgV sg_DMulNC(SgG* g, SgV a, SgV b) {
    u32 v = spv_emit_binary(&g->c->emit, SPV_OP_F_MUL, g->c->f64_type, a.id, b.id);
    spv_emit_no_contraction(&g->c->emit, v);
    return sg53_f64(g, v);
}
[[maybe_unused]] static SgV sg_DFmaNC(SgG* g, SgV a, SgV b, SgV d) {
    DirectCtx* c = g->c;
    u32 v = spv_emit_glsl_ternary(&c->emit, c->f64_type, c->glsl, SPV_GLSL_FMA, a.id, b.id, d.id);
    spv_emit_no_contraction(&c->emit, v);
    return sg53_f64(g, v);
}
/* as FMNMX: PT or !PT picks one as the program is translated, a NaN giving the other operand */
[[maybe_unused]] static SgV sg_DMinMax(SgG* g, SgV choose, SgV x, SgV y) {
    DirectCtx* c = g->c;
    u32 left = x.id, right = y.id, choose_min = choose.id;
    u32 left_nan = spv_emit_unary(&c->emit, SPV_OP_IS_NAN, c->bool_type, left);
    u32 right_nan = spv_emit_unary(&c->emit, SPV_OP_IS_NAN, c->bool_type, right);
    u32 min = 0, max = 0;
    if (choose_min != c->false_value) {
        min = spv_emit_glsl_binary(&c->emit, c->f64_type, c->glsl, SPV_GLSL_F_MIN, left, right);
        min = spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->f64_type, left_nan, right, min);
        min = spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->f64_type, right_nan, left, min);
    }
    if (choose_min != c->true_value) {
        max = spv_emit_glsl_binary(&c->emit, c->f64_type, c->glsl, SPV_GLSL_F_MAX, left, right);
        max = spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->f64_type, left_nan, right, max);
        max = spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->f64_type, right_nan, left, max);
    }
    return sg53_f64(g, shader_select(c, c->f64_type, choose_min, min, max));
}
[[maybe_unused]] static SgV sg_DFromHigh(SgG* g, SgV high) {
    DirectCtx* c = g->c;
    u32 pair = spv_emit_vec2_construct(&c->emit, g->t->vec2_u32, c->zero, sg_u32(g, high));
    return sg53_f64(g, spv_emit_unary(&c->emit, SPV_OP_BITCAST, c->f64_type, pair));
}
[[maybe_unused]] static SgV sg_DRcp(SgG* g, SgV x) {
    DirectCtx* c = g->c;
    return sg53_f64(g, spv_emit_binary(&c->emit, SPV_OP_F_DIV, c->f64_type,
                                       spv_emit_constant64(&c->emit, c->f64_type, 0x3ff0000000000000ull), x.id));
}
[[maybe_unused]] static SgV sg_DInvSqrt(SgG* g, SgV x) {
    return sg53_f64(g, spv_emit_glsl_unary(&g->c->emit, g->c->f64_type, g->c->glsl, SPV_GLSL_INVERSE_SQRT, x.id));
}
[[maybe_unused]] static SgV sg_DHigh(SgG* g, SgV x) {
    DirectCtx* c = g->c;
    return sg_v(spv_emit_vec4_extract(&c->emit, c->u32_type, spv_emit_unary(&c->emit, SPV_OP_BITCAST, g->t->vec2_u32, x.id), 1),
                SGT_U32);
}
[[maybe_unused]] static SgV sg_DCompare(SgG* g, int64_t op, SgV a, SgV b) {
    return sg_v(spv_emit_fcompare(&g->c->emit, g->c->bool_type, (u32)op, a.id, b.id), SGT_BOOL);
}
[[maybe_unused]] static int64_t sg_FswzMask(SgG* g, int64_t mode) { return shader_fswzadd_lanes(g->w, (u32)mode); }
[[maybe_unused]] static SgV sg_QuadLane(SgG* g) {
    DirectCtx* c = g->c;
    return sg_v(c->quad_lane ? c->quad_lane
                             : spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type,
                                               shader_input(c, c->u32_type, c->subgroup_lane_input), shader_u32(c, 3)),
                SGT_U32);
}
[[maybe_unused]] static void sg_F2F(SgG* g) { shader_f2f(g->t, g->i, g->w, g->op, g->d, g->a, g->b); }
[[maybe_unused]] static void sg_F2I(SgG* g) { shader_f2i(g->t, g->i, g->w, g->op, g->d, g->a, g->b); }
[[maybe_unused]] static void sg_I2F(SgG* g) { shader_i2f(g->t, g->i, g->w, g->op, g->d, g->a, g->b); }
