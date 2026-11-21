/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sm53_flow.sail's back end (after sm53_spirv.h): the dispatch loop's pc, the call stack and the CRS stack, which are
   to_spirv's Function variables, the static walk's targets, and EXIT's end of the program (emit_flow.c's shader_exit) */

[[maybe_unused]] static int sg_HasCallStack(SgG* g) { return g->t->call_ptr != 0; }
[[maybe_unused]] static SgV sg_CallPtr(SgG* g) { return sg_v(spv_emit_load(&g->c->emit, g->c->u32_type, g->t->call_ptr), SGT_U32); }
[[maybe_unused]] static void sg_CallPtrWrite(SgG* g, SgV v) { spv_emit_store(&g->c->emit, g->t->call_ptr, sg_u32(g, v)); }
[[maybe_unused]] static SgV sg_CallStackRead(SgG* g, int64_t k) {
    return sg_v(spv_emit_load(&g->c->emit, g->c->u32_type, g->t->call_stack[k]), SGT_U32);
}
[[maybe_unused]] static void sg_CallStackWrite(SgG* g, int64_t k, SgV v) {
    spv_emit_store(&g->c->emit, g->t->call_stack[k], sg_u32(g, v));
}
[[maybe_unused]] static int sg_DynamicStack(SgG* g) { return g->t->dynamic_stack; }
[[maybe_unused]] static SgV sg_StackPtr(SgG* g) { return sg_v(spv_emit_load(&g->c->emit, g->c->u32_type, g->t->stack_ptr), SGT_U32); }
[[maybe_unused]] static void sg_StackPtrWrite(SgG* g, SgV v) { spv_emit_store(&g->c->emit, g->t->stack_ptr, sg_u32(g, v)); }
[[maybe_unused]] static SgV sg_StackRead(SgG* g, int64_t k) {
    return sg_v(spv_emit_load(&g->c->emit, g->c->u32_type, g->t->stack[k]), SGT_U32);
}
[[maybe_unused]] static void sg_StackWrite(SgG* g, int64_t k, SgV v) { spv_emit_store(&g->c->emit, g->t->stack[k], sg_u32(g, v)); }
[[maybe_unused]] static void sg_DynamicPop(SgG* g, int64_t kind, SgV fallback) {
    shader_dynamic_pop(g->c, g->t->stack_ptr, g->t->stack, (u32)kind, sg_u32(g, fallback), &g->out[0], &g->out[1], &g->out[2]);
}
[[maybe_unused]] static SgV sg_PopFound(SgG* g) { return sg_v(g->out[0], SGT_BOOL); }
[[maybe_unused]] static SgV sg_PopDestination(SgG* g) { return sg_v(g->out[1], SGT_U32); }
[[maybe_unused]] static SgV sg_PopStackPtr(SgG* g) { return sg_v(g->out[2], SGT_U32); }
[[maybe_unused]] static SgV sg_Pc(SgG* g) { return sg_v(spv_emit_load(&g->c->emit, g->c->u32_type, g->t->pc), SGT_U32); }
[[maybe_unused]] static void sg_PcWrite(SgG* g, SgV v) { spv_emit_store(&g->c->emit, g->t->pc, sg_u32(g, v)); }
[[maybe_unused]] static int64_t sg_PopTarget(SgG* g) { return g->t->pop_targets ? g->t->pop_targets[g->i] : 0; }
[[maybe_unused]] static int64_t sg_BraTarget(SgG* g) { return bra_target(g->w, g->i); }
[[maybe_unused]] static int64_t sg_NextInsn(SgG* g) { return shader_next_instruction(g->i, g->t->n); }
[[maybe_unused]] static int64_t sg_InsnIndex(SgG* g) { return g->i; }
[[maybe_unused]] static int64_t sg_FlowBase(SgG* g) { return g->t->si->flow_base; }
[[maybe_unused]] static SgV sg_CbufReadDynamic(SgG* g, int64_t bank, SgV address) {
    return sg_v(shader_cbuf_read_dynamic(g->c, (u32)bank, sg_u32(g, address)), SGT_U32);
}
[[maybe_unused]] static int sg_Structured(SgG* g) { return g->t->structure != NULL; }
[[maybe_unused]] static void sg_BranchTarget(SgG* g, SgV v) { g->c->branch_target = sg_u32(g, v); /* the structure's SHS_SWITCH */ }
[[maybe_unused]] static void sg_Kill(SgG* g) {
    DirectCtx* c = g->c;
    if (c->discarded)
        spv_emit_store(&c->emit, c->discarded, c->true_value);
    else
        spv_emit_demote(&c->emit);
}
[[maybe_unused]] static void sg_Exit(SgG* g) { shader_exit(g->t, g->i, g->w); }
[[maybe_unused]] static SgV sg_SignedWords(SgG* g, int64_t r, int64_t field) {
    DirectCtx* c = g->c;
    u32 i32_type = g->t->i32_type;
    u32 offset = spv_emit_constant_u32(&c->emit, i32_type, (u32)((i32)((u32)field << 8) >> 8));
    u32 signed_index = spv_emit_iadd_u32(&c->emit, i32_type, reg_read_as(c, (u32)r, i32_type), offset);
    u32 words = spv_emit_binary(&c->emit, SPV_OP_S_DIV, i32_type, signed_index, spv_emit_constant_u32(&c->emit, i32_type, 8));
    return sg_v(spv_emit_unary(&c->emit, SPV_OP_BITCAST, c->u32_type, words), SGT_U32);
}
