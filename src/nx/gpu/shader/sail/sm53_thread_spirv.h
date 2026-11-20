/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sm53_thread.sail's back end (after sm53_spirv.h): the system registers, the subgroup's lanes, votes, shuffles and
   barriers, and the quad's reads through derivatives that a fragment's lone lane shuffles with (which were
   emit_thread.c's) */

/* the lane a lane-model invocation is in its warp of one: a fragment's place in its quad, else lane 0 */
[[maybe_unused]] static u32 lone_lane(DirectCtx* c) { return c->quad_lane ? c->quad_lane : c->zero; }

/* whether this fragment's quad lane is one of lanes (bit L for lane L) */
[[maybe_unused]] static u32 quad_lane_in(DirectCtx* c, u32 lanes) {
    if (lanes == 0 || lanes == 15) return lanes ? c->true_value : c->false_value;
    u32 bit = spv_emit_binary(
        &c->emit, SPV_OP_BITWISE_AND, c->u32_type,
        spv_emit_binary(&c->emit, SPV_OP_SHIFT_RIGHT_LOGICAL, c->u32_type, shader_u32(c, lanes), c->quad_lane),
        shader_u32(c, 1));
    return spv_emit_binary(&c->emit, SPV_OP_I_NOT_EQUAL, c->bool_type, bit, c->zero);
}

/* halves (a vector of two floats) as the pixel next to this one along an axis has them: the fine derivative is the
   difference of the pair's values, added where the other pixel comes after this one and taken off where before */
[[maybe_unused]] static u32 quad_across(DirectCtx* c, u32 vec2_f32, u32 halves, int y_axis) {
    u32 derivative = spv_emit_derivative_fine(&c->emit, vec2_f32, halves, y_axis);
    u32 step = spv_emit_binary(&c->emit, SPV_OP_VECTOR_TIMES_SCALAR, vec2_f32, derivative, c->quad_sign[y_axis]);
    u32 across = spv_emit_binary(&c->emit, SPV_OP_F_ADD, vec2_f32, halves, step);
    spv_emit_no_contraction(&c->emit, step);
    spv_emit_no_contraction(&c->emit, across);
    return across;
}

/* value's bits in the other pixels of this fragment's 2x2 quad where the stage has no subgroup, read through fine
   derivatives, as the console's SHFL reads the quad's lanes: across[1] the pixel beside it, across[2] the one above
   or below and across[3] the one diagonally across, for each whose bit is in need (bit D for across[D]). The value
   goes as its two 16-bit halves in floats: each derivative is then the difference of two whole numbers below 2^16
   and adding it back gives the other one, both exact in F32 under any rounding (whole numbers below 2^24), where
   the bits taken as one float would not come back from a NaN, an infinity or a flushed subnormal. The derivatives
   read every pixel of the quad, those a triangle's edge leaves out as helpers included, as the console's lanes
   for them run the program and SHFL reads them */
[[maybe_unused]] static void quad_read(DirectCtx* c, u32 value, u32 need, u32 across[4]) {
    u32 vec2_u32 = spv_emit_type_vector(&c->emit, c->u32_type, 2),
        vec2_f32 = spv_emit_type_vector(&c->emit, c->f32_type, 2);
    u32 low = spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, value, shader_u32(c, 0xffffu));
    u32 high = spv_emit_binary(&c->emit, SPV_OP_SHIFT_RIGHT_LOGICAL, c->u32_type, value, shader_u32(c, 16));
    u32 halves = spv_emit_unary(&c->emit, SPV_OP_CONVERT_U_TO_F, vec2_f32,
                                spv_emit_vec2_construct(&c->emit, vec2_u32, low, high));
    u32 read[4] = {0, 0, 0, 0};
    if (need & 0xa) read[1] = quad_across(c, vec2_f32, halves, 0);
    if (need & 0x4) read[2] = quad_across(c, vec2_f32, halves, 1);
    if (need & 0x8) read[3] = quad_across(c, vec2_f32, read[1], 1);
    for (u32 k = 1; k < 4; k++) {
        if (!(need & (1u << k))) continue;
        u32 bits = spv_emit_unary(&c->emit, SPV_OP_CONVERT_F_TO_U, vec2_u32, read[k]);
        across[k] = spv_emit_binary(
            &c->emit, SPV_OP_BITWISE_OR, c->u32_type, spv_emit_vec4_extract(&c->emit, c->u32_type, bits, 0),
            spv_emit_binary(&c->emit, SPV_OP_SHIFT_LEFT_LOGICAL, c->u32_type,
                            spv_emit_vec4_extract(&c->emit, c->u32_type, bits, 1), shader_u32(c, 16)));
    }
}

[[maybe_unused]] static int sg_LaneModel(SgG* g) { return g->t->lane_model; }
[[maybe_unused]] static int sg_LaneIsInvocation(SgG* g) { return g->t->lane_is_invocation; }
[[maybe_unused]] static int sg_QuadLaneModel(SgG* g) { return g->c->quad_lane != 0; }
[[maybe_unused]] static SgV sg_LaneId(SgG* g) {
    const Tr* t = g->t;
    DirectCtx* c = g->c;
    return sg_v(t->lane_is_invocation ? spv_emit_load(&c->emit, c->u32_type, t->invocation_id)
                : t->lane_model       ? lone_lane(c)
                                      : shader_input(c, c->u32_type, c->subgroup_lane_input),
                SGT_U32);
}
[[maybe_unused]] static SgV sg_LoneLane(SgG* g) { return sg_v(lone_lane(g->c), SGT_U32); }
[[maybe_unused]] static SgV sg_SubgroupLane(SgG* g) {
    return sg_v(shader_input(g->c, g->c->u32_type, g->c->subgroup_lane_input), SGT_U32);
}
[[maybe_unused]] static SgV sg_ComputeIds(SgG* g, SgV local) {
    const Tr* t = g->t;
    return sg_vt(spv_emit_load(&g->c->emit, t->vec3_u32, local.id == g->c->true_value ? t->local_input : t->group_input),
                 t->vec3_u32);
}
[[maybe_unused]] static SgV sg_VecAt(SgG* g, SgV v, int64_t k) {
    return sg_v(spv_emit_vec4_extract(&g->c->emit, g->c->u32_type, v.id, (u32)k), SGT_U32);
}
[[maybe_unused]] static SgV sg_LaneDead(SgG* g) { return sg_v(shader_lane_dead(g->c), SGT_BOOL); }
[[maybe_unused]] static SgV sg_InvocationId(SgG* g) {
    return sg_v(shader_input(g->c, g->c->u32_type, g->t->invocation_id), SGT_U32);
}
[[maybe_unused]] static SgV sg_PatchVertices(SgG* g) {
    return sg_v(spv_emit_load(&g->c->emit, g->c->u32_type, g->t->patch_vertices), SGT_U32);
}
[[maybe_unused]] static SgV sg_ReadClock(SgG* g, int64_t scope) {
    const Tr* t = g->t;
    return sg_vt(spv_emit_unary(&g->c->emit, SPV_OP_READ_CLOCK_KHR, t->vec2_u32, shader_u32(g->c, (u32)scope)),
                 t->vec2_u32);
}
[[maybe_unused]] static SgV sg_ClockAt(SgG* g, SgV v, int64_t k) { return sg_VecAt(g, v, k); }
[[maybe_unused]] static SgV sg_BitsInsert(SgG* g, SgV base, SgV insert, SgV off, SgV n) {
    return sg_v(spv_emit_bitfield_insert(&g->c->emit, g->c->u32_type, sg_u32(g, base), sg_u32(g, insert), sg_u32(g, off),
                                         sg_u32(g, n)),
                SGT_U32);
}
[[maybe_unused]] static SgV sg_BallotWord(SgG* g, SgV lane, SgV p) {
    return sg_v(shader_subgroup_ballot_word(g->c, g->t->vec4_u32, sg_u32(g, lane), p.id), SGT_U32);
}
[[maybe_unused]] static int64_t sg_QuadShfl(SgG* g) { return g->c->quad ? shader_shfl_quad(g->w, NULL) : 0; }
[[maybe_unused]] static int64_t sg_QuadShflOperand(SgG* g) {
    u32 operand = 0;
    shader_shfl_quad(g->w, &operand);
    return operand;
}
[[maybe_unused]] static SgV sg_SubgroupQuad(SgG* g, SgV value, int64_t swap, int64_t operand) {
    DirectCtx* c = g->c;
    return sg_v(spv_emit_subgroup_quad_u32(&c->emit, c->u32_type, sg_u32(g, value), (int)swap, shader_u32(c, (u32)operand)),
                SGT_U32);
}
[[maybe_unused]] static int sg_QuadShflLanes(SgG* g) { return g->c->quad_lane && shader_shfl_quad_lanes(g->w, NULL, NULL); }
/* a fragment's lane alone in its quad: its SHFL within the quad reads the pixel the lane would */
[[maybe_unused]] static void sg_ShflQuadLanes(SgG* g) {
    DirectCtx* c = g->c;
    u64 w = g->w;
    u32 a = g->a, d = g->d;
    u8 dir[4];
    u32 valid, lanes[4] = {0, 0, 0, 0}, across[4] = {0, 0, 0, 0}, need = 0;
    shader_shfl_quad_lanes(w, dir, &valid);
    for (u32 lane = 0; lane < 4; lane++) lanes[dir[lane]] |= 1u << lane;
    for (u32 k = 1; k < 4; k++)
        if (lanes[k]) need |= 1u << k;
    u32 value = reg_read(c, a), result = value;
    if (need) quad_read(c, value, need, across);
    for (u32 k = 1; k < 4; k++)
        if (lanes[k])
            result = lanes[k] == 15 ? across[k]
                                    : spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->u32_type,
                                                       quad_lane_in(c, lanes[k]), across[k], result);
    pred_write(c, B(w, 48, 3), quad_lane_in(c, valid));
    reg_write(c, d, result);
}
[[maybe_unused]] static SgV sg_SubgroupShuffle(SgG* g, SgV value, SgV lane) {
    uint32_t v = sg_u32(g, value), l = sg_u32(g, lane);
    return sg_v(spv_emit_subgroup_shuffle_u32(&g->c->emit, g->c->u32_type, v, l), SGT_U32);
}
[[maybe_unused]] static void sg_PatchBarrier(SgG* g) { spv_emit_patch_barrier(&g->c->emit, g->c->u32_type); }
[[maybe_unused]] static void sg_ControlBarrier(SgG* g) { spv_emit_control_barrier(&g->c->emit, g->c->u32_type); }
[[maybe_unused]] static void sg_MemoryBarrier(SgG* g, int64_t scope, int64_t semantics) {
    spv_emit_memory_barrier(&g->c->emit, g->c->u32_type, (u32)scope, (u32)semantics);
}
