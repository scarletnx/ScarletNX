/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The PICA200's back end for sailgen --gpu (after tools/sailgen/spirv.h): pica.sail's vals over vk/vsgen.c's
   generator (Gen), which includes the file sailgen writes after its helpers. The registers are vsgen.c's Private
   variables (an input register's and a temporary's made at its first use), the uniforms its uniform block, and the
   flow control its structured blocks and subroutines. An instruction's context is the generator, the instruction's
   word and offset; sgp_begin makes it */

struct SgG {
    SgSpv spv;
    Gen* gen;
    uint32_t w;      /* the instruction's word */
    uint32_t offset; /* its offset */
    int form;        /* its opcode's place in its roots line */
    uint32_t merge;  /* IfBegin's merge block */
};
static SgSpv* sg_spv(SgG* g) { return &g->spv; }
[[maybe_unused]] static uint32_t sgp_t_u64(SgG* g) { return (void)g, 0; }
[[maybe_unused]] static void sgp_begin(SgG* g, Gen* gen, uint32_t offset, uint32_t w) {
    g->gen = gen;
    g->w = w;
    g->offset = offset;
    g->form = 0;
    g->merge = 0;
    g->spv.e = gen->s;
    g->spv.t_u32 = gen->t_uint;
    g->spv.t_i32 = gen->t_int;
    g->spv.t_f32 = gen->t_float;
    g->spv.t_bool = gen->t_bool;
    g->spv.glsl = spv_emit_glsl450_import(gen->s);
    g->spv.true_v = g->spv.false_v = 0; /* the Sail names no boolean constant */
    g->spv.t_u64 = sgp_t_u64;
    g->spv.ncast = 0;
    g->spv.fold = 0;
    g->spv.fresh_casts = 0;
    g->spv.known = 1; /* vsgen.c's constants and types are the module's known ones (spv_emit_const_*) */
}
[[maybe_unused]] static void sg_set_form(SgG* g, int k) { g->form = k; }
[[maybe_unused]] static uint64_t sg_Field(SgG* g, int64_t lo, int64_t n) { return snx_bits(g->w, (unsigned)lo, (unsigned)n); }
[[maybe_unused]] static int64_t sg_Form(SgG* g) { return g->form; }
[[maybe_unused]] static uint64_t sg_Pattern(SgG* g, int64_t desc) { return g->gen->setup->swizzle[desc]; }

static uint32_t splat4(Gen* g, uint32_t type, uint32_t v) {
    return SPV(g->s, SPV_OP_COMPOSITE_CONSTRUCT, type, v, v, v, v);
}
/* uniforms.f[index], uniforms.i[index] and uniforms.b */
static uint32_t uniform_f(Gen* g, uint32_t index) {
    uint32_t pointer = spv_emit_type_pointer(g->s, SPV_STORAGE_UNIFORM, g->t_vec4);
    return load(g, g->t_vec4, SPV(g->s, SPV_OP_ACCESS_CHAIN, pointer, g->pica, sconst(g, 2), index));
}
static uint32_t uniform_i(Gen* g, unsigned index) {
    uint32_t pointer = spv_emit_type_pointer(g->s, SPV_STORAGE_UNIFORM, g->t_uvec4);
    uint32_t element = SPV(g->s, SPV_OP_ACCESS_CHAIN, pointer, g->pica, sconst(g, 1), sconst(g, (int32_t)index));
    return load(g, g->t_uvec4, element);
}
static uint32_t uniform_b(Gen* g) {
    uint32_t pointer = spv_emit_type_pointer(g->s, SPV_STORAGE_UNIFORM, g->t_uint);
    return load(g, g->t_uint, SPV(g->s, SPV_OP_ACCESS_CHAIN, pointer, g->pica, sconst(g, 0)));
}
/* get_offset_register: uniforms.f[(base + offset) & 0x7f] with an offset outside -128..127 taken as 0, and 1.0 past
   the 96 registers (the load clamped to them and the result selected, which keeps it in bounds) */
static uint32_t offset_register(Gen* g, unsigned base, uint32_t offset) {
    uint32_t low = SPV(g->s, SPV_OP_S_GREATER_THAN_EQUAL, g->t_bool, offset, sconst(g, -128));
    uint32_t high = SPV(g->s, SPV_OP_S_LESS_THAN_EQUAL, g->t_bool, offset, sconst(g, 127));
    uint32_t fixed = SPV(g->s, SPV_OP_SELECT, g->t_int, SPV(g->s, SPV_OP_LOGICAL_AND, g->t_bool, low, high), offset,
                         sconst(g, 0));
    uint32_t sum = SPV(g->s, SPV_OP_I_ADD, g->t_int, sconst(g, (int32_t)base), fixed);
    uint32_t wrapped = SPV(g->s, SPV_OP_BITWISE_AND, g->t_int, sum, sconst(g, 0x7f));
    uint32_t index = SPV(g->s, SPV_OP_BITCAST, g->t_uint, wrapped);
    uint32_t inside = SPV(g->s, SPV_OP_U_LESS_THAN, g->t_bool, index, uconst(g, 96));
    uint32_t v = uniform_f(g, SPV_EXT(g->s, g->t_uint, SPV_GLSL_U_MIN, index, uconst(g, 95)));
    return SPV(g->s, SPV_OP_SELECT, g->t_vec4, splat4(g, g->t_bool4, inside), v, vec4_const(g, 1, 1, 1, 1));
}
static uint32_t temporary(Gen* g, unsigned i) {
    if (!g->tmp[i]) g->tmp[i] = private_vec4(g);
    return g->tmp[i];
}

/* the registers */
[[maybe_unused]] static SgV sg_InputReg(SgG* g, int64_t r) {
    Gen* gen = g->gen;
    if (!gen->in_reg[r]) gen->in_reg[r] = private_vec4(gen);
    return sg_vt(load(gen, gen->t_vec4, gen->in_reg[r]), gen->t_vec4);
}
[[maybe_unused]] static SgV sg_TempReg(SgG* g, int64_t r) {
    Gen* gen = g->gen;
    return sg_vt(load(gen, gen->t_vec4, temporary(gen, (unsigned)r)), gen->t_vec4);
}
[[maybe_unused]] static SgV sg_UniformF(SgG* g, int64_t r) {
    return sg_vt(uniform_f(g->gen, sconst(g->gen, (int32_t)r)), g->gen->t_vec4);
}
[[maybe_unused]] static SgV sg_UniformOffset(SgG* g, int64_t base, SgV offset) {
    return sg_vt(offset_register(g->gen, (unsigned)base, sg_as(g, offset, SGT_I32)), g->gen->t_vec4);
}
[[maybe_unused]] static SgV sg_AddrRead(SgG* g, int64_t i) { return sg_v(load(g->gen, g->gen->t_int, g->gen->addr[i]), SGT_I32); }
[[maybe_unused]] static void sg_AddrWrite(SgG* g, int64_t i, SgV v) { store(g->gen, g->gen->addr[i], sg_as(g, v, SGT_I32)); }
/* the destination register's variable, 0 for an output the mask leaves out */
[[maybe_unused]] static int64_t sg_DestVar(SgG* g, int64_t reg) {
    Gen* gen = g->gen;
    if (reg < 0x10) return gen->config->output_map[reg] < gen->config->num_outputs ? gen->out[gen->config->output_map[reg]] : 0;
    if (reg < 0x20) return temporary(gen, (unsigned)reg - 0x10);
    return 0;
}
[[maybe_unused]] static SgV sg_VarLoad(SgG* g, int64_t var) { return sg_vt(load(g->gen, g->gen->t_vec4, (uint32_t)var), g->gen->t_vec4); }
[[maybe_unused]] static void sg_VarStore(SgG* g, int64_t var, SgV v) { store(g->gen, (uint32_t)var, v.id); }
[[maybe_unused]] static SgV sg_CondRead(SgG* g, int64_t i) { return sg_v(load(g->gen, g->gen->t_bool, g->gen->cc[i]), SGT_BOOL); }
[[maybe_unused]] static void sg_CondWrite(SgG* g, int64_t i, SgV v) { store(g->gen, g->gen->cc[i], v.id); }
[[maybe_unused]] static SgV sg_UniformB(SgG* g) { return sg_v(uniform_b(g->gen), SGT_U32); }

/* if (c) { ... } */
[[maybe_unused]] static void sg_IfBegin(SgG* g, SgV c) { g->merge = if_begin(g->gen, c.id); }
[[maybe_unused]] static void sg_IfEnd(SgG* g) { place(g->gen, g->merge); }

/* ---- the flow control ---- */
[[maybe_unused]] static void sg_End(SgG* g) { ret(g->gen, true); }
[[maybe_unused]] static void sg_JumpTo(SgG* g, SgV cond, int64_t dest) {
    Gen* gen = g->gen;
    if (!gen->jmp_to) {
        /* a range the analysis took from its cache without the labels: the GLSL had no loop to break */
        gen->fail = "a jump in a subroutine without labels";
        return;
    }
    uint32_t merge = if_begin(gen, cond.id);
    store(gen, gen->jmp_to, uconst(gen, (uint32_t)dest));
    jump(gen, gen->switch_merge);
    place(gen, merge);
}
[[maybe_unused]] static void sg_CallSub(SgG* g, int64_t dest, int64_t num) {
    call_sub(g->gen, sub(g->gen, (uint32_t)dest, (uint32_t)(dest + num)));
}
[[maybe_unused]] static void sg_CallIf(SgG* g, SgV cond, int64_t dest, int64_t num) {
    uint32_t merge = if_begin(g->gen, cond.id);
    sg_CallSub(g, dest, num);
    place(g->gen, merge);
}
/* the body (up to dest) when cond holds, else the else part (dest to dest + num) */
[[maybe_unused]] static void sg_IfElse(SgG* g, SgV cond, int64_t dest, int64_t num) {
    Gen* gen = g->gen;
    uint32_t else_offset = (uint32_t)dest, endif_offset = else_offset + (uint32_t)num;
    uint32_t then = spv_emit_id(gen->s), merge = spv_emit_id(gen->s);
    uint32_t otherwise = num ? spv_emit_id(gen->s) : merge;
    SPV_VOID(gen->s, SPV_OP_SELECTION_MERGE, merge, SPV_SELECTION_CONTROL_NONE);
    SPV_VOID(gen->s, SPV_OP_BRANCH_CONDITIONAL, cond.id, then, otherwise);
    gen->open = false;
    place(gen, then);
    call_sub(gen, sub(gen, g->offset + 1, else_offset));
    if (num) {
        jump(gen, merge);
        place(gen, otherwise);
        call_sub(gen, sub(gen, else_offset, endif_offset));
    }
    place(gen, merge);
}
/* aL = i.y; for (uint n = 0u; n <= i.x; aL += i.z, ++n) */
[[maybe_unused]] static void sg_Loop(SgG* g, int64_t id, int64_t dest) {
    Gen* gen = g->gen;
    uint32_t i = uniform_i(gen, (unsigned)id);
    store(gen, gen->addr[2], SPV(gen->s, SPV_OP_BITCAST, gen->t_int, extract(gen, gen->t_uint, i, 1)));
    uint32_t counter = spv_emit_local(gen->s, gen->t_uint);
    store(gen, counter, uconst(gen, 0));
    uint32_t header = spv_emit_id(gen->s), body = spv_emit_id(gen->s), next = spv_emit_id(gen->s);
    uint32_t merge = spv_emit_id(gen->s);
    place(gen, header);
    uint32_t more = SPV(gen->s, SPV_OP_U_LESS_THAN_EQUAL, gen->t_bool, load(gen, gen->t_uint, counter),
                        extract(gen, gen->t_uint, i, 0));
    SPV_VOID(gen->s, SPV_OP_LOOP_MERGE, merge, next, SPV_LOOP_CONTROL_NONE);
    SPV_VOID(gen->s, SPV_OP_BRANCH_CONDITIONAL, more, body, merge);
    gen->open = false;
    place(gen, body);
    call_sub(gen, sub(gen, g->offset + 1, (uint32_t)dest + 1));
    place(gen, next);
    uint32_t step = SPV(gen->s, SPV_OP_BITCAST, gen->t_int, extract(gen, gen->t_uint, i, 2));
    store(gen, gen->addr[2], SPV(gen->s, SPV_OP_I_ADD, gen->t_int, load(gen, gen->t_int, gen->addr[2]), step));
    store(gen, counter, SPV(gen->s, SPV_OP_I_ADD, gen->t_uint, load(gen, gen->t_uint, counter), uconst(gen, 1)));
    jump(gen, header);
    place(gen, merge);
}
/* an instruction the decompiler refuses */
[[maybe_unused]] static void sg_throw_Untranslated(SgG* g, const char* why, ...) { g->gen->fail = why; }
