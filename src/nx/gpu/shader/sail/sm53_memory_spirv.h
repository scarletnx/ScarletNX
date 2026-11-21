/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sm53_memory.sail's back end (after sm53_spirv.h): the memory to_spirv declared (the shared array or its spill
   buffer, the local array, the global buffer slots the prescan found), its words' pointers, loads, stores and atomics,
   and the CAS loops of the stores and atomics SPIR-V has not, each as emit_memory made it */

[[maybe_unused]] static SgV sg_CbufReadBank(SgG* g, int64_t first, SgV bank, SgV address) {
    return sg_v(shader_cbuf_read_bank(g->c, (u32)first, sg_u32(g, bank), sg_u32(g, address)), SGT_U32);
}
[[maybe_unused]] static int64_t sg_TextureCount(SgG* g) { return g->t->texture_count; }
/* an indexed texture whose handle this LDC loads */
[[maybe_unused]] static int sg_TexIndexedBy(SgG* g, int64_t k) {
    const ShTex* x = &g->t->textures[k];
    return (x->flags & SNX_TEX_INDEXED) && x->bl2_off == g->i + 1;
}
[[maybe_unused]] static int64_t sg_TexIndexShift(SgG* g, int64_t k) { return g->t->textures[k].flags >> 4; }
[[maybe_unused]] static void sg_TexIndexStore(SgG* g, int64_t k, SgV v) { spv_emit_store(&g->c->emit, g->c->tex_index[k], sg_u32(g, v)); }
[[maybe_unused]] static int64_t sg_SharedWords(SgG* g) { return g->t->shared_words; }
[[maybe_unused]] static int64_t sg_LocalWords(SgG* g) { return g->t->local_words; }
[[maybe_unused]] static SgV sg53_ptr(u32 id) { return sg_vt(id, 0); }
[[maybe_unused]] static SgV sg_SharedPointer(SgG* g, SgV index) { return sg53_ptr(shader_shared_pointer(g->c, g->t, sg_u32(g, index))); }
[[maybe_unused]] static SgV sg_LocalPointer(SgG* g, SgV index) {
    return sg53_ptr(spv_emit_function_element_pointer(&g->c->emit, g->t->local_element_pointer, g->t->local_array, sg_u32(g, index)));
}
[[maybe_unused]] static SgV sg_GbufPointer(SgG* g, int64_t slot, SgV word) {
    DirectCtx* c = g->c;
    u32 index = shader_gbuf_word_index(c, g->t->stage, (u32)slot, sg_u32(g, word));
    return sg53_ptr(spv_emit_storage_access_u32(&c->emit, c->gbuf_pointer, c->gbuf[slot], c->zero, index));
}
[[maybe_unused]] static SgV sg_Load(SgG* g, SgV p) { return sg_v(spv_emit_load(&g->c->emit, g->c->u32_type, p.id), SGT_U32); }
[[maybe_unused]] static void sg_Store(SgG* g, SgV p, SgV v) { spv_emit_store(&g->c->emit, p.id, sg_u32(g, v)); }
[[maybe_unused]] static SgV sg_AtomicLoad(SgG* g, SgV p) { return sg_v(spv_emit_atomic_load_u32(&g->c->emit, g->c->u32_type, p.id), SGT_U32); }
[[maybe_unused]] static SgV sg_AtomicLoadShared(SgG* g, SgV p) {
    return sg_v(spv_emit_atomic_load_shared_u32(&g->c->emit, g->c->u32_type, p.id), SGT_U32);
}
/* n bits of register r into the word at bit_offset, compare-and-swap till it lands; a buffer's word (Device scope) or
   shared memory's */
[[maybe_unused]] static void sg53_cas_insert(SgG* g, SgV p, SgV bit_offset, SgV width, int64_t r, int shared) {
    DirectCtx* c = g->c;
    u32 header = spv_emit_id(&c->emit), body = spv_emit_id(&c->emit);
    u32 retry = spv_emit_id(&c->emit), done = spv_emit_id(&c->emit);
    spv_emit_branch(&c->emit, header);
    spv_emit_label(&c->emit, header);
    spv_emit_loop_merge(&c->emit, done, retry);
    spv_emit_branch(&c->emit, body);
    spv_emit_label(&c->emit, body);
    u32 old = shared ? spv_emit_atomic_load_shared_u32(&c->emit, c->u32_type, p.id)
                     : spv_emit_atomic_load_u32(&c->emit, c->u32_type, p.id);
    u32 desired = spv_emit_bitfield_insert(&c->emit, c->u32_type, old, reg_read(c, (u32)r), sg_u32(g, bit_offset),
                                           sg_u32(g, width));
    u32 actual = shared ? spv_emit_atomic_compare_exchange_shared_u32(&c->emit, c->u32_type, p.id, desired, old)
                        : spv_emit_atomic_compare_exchange_u32(&c->emit, c->u32_type, p.id, desired, old);
    u32 success = spv_emit_binary(&c->emit, SPV_OP_I_EQUAL, c->bool_type, actual, old);
    spv_emit_branch_conditional(&c->emit, success, done, retry);
    spv_emit_label(&c->emit, retry);
    spv_emit_branch(&c->emit, header);
    spv_emit_label(&c->emit, done);
}
[[maybe_unused]] static void sg_CasInsert(SgG* g, SgV p, SgV bit_offset, SgV width, int64_t r) {
    sg53_cas_insert(g, p, bit_offset, width, r, 0);
}
[[maybe_unused]] static void sg_CasInsertShared(SgG* g, SgV p, SgV bit_offset, SgV width, int64_t r) {
    sg53_cas_insert(g, p, bit_offset, width, r, 1);
}
[[maybe_unused]] static SgV sg_AtomicOp(SgG* g, int64_t opcode, SgV p, SgV v) {
    return sg_v(spv_emit_atomic_u32(&g->c->emit, g->c->u32_type, (u32)opcode, p.id, sg_u32(g, v)), SGT_U32);
}
[[maybe_unused]] static SgV sg_AtomicOpShared(SgG* g, int64_t opcode, SgV p, SgV v) {
    return sg_v(spv_emit_atomic_shared_u32(&g->c->emit, g->c->u32_type, (u32)opcode, p.id, sg_u32(g, v)), SGT_U32);
}
[[maybe_unused]] static SgV sg_AtomicCas(SgG* g, SgV p, SgV desired, SgV comparator) {
    u32 x = sg_u32(g, desired), y = sg_u32(g, comparator);
    return sg_v(spv_emit_atomic_compare_exchange_u32(&g->c->emit, g->c->u32_type, p.id, x, y), SGT_U32);
}
[[maybe_unused]] static SgV sg_AtomicCasShared(SgG* g, SgV p, SgV desired, SgV comparator) {
    u32 x = sg_u32(g, desired), y = sg_u32(g, comparator);
    return sg_v(spv_emit_atomic_compare_exchange_shared_u32(&g->c->emit, g->c->u32_type, p.id, x, y), SGT_U32);
}
/* ATOMS's wrap step (INC, DEC) as a CAS loop on atomic_old_var, the register read in the loop */
[[maybe_unused]] static SgV sg_AtomicLoopShared(SgG* g, SgV p, int64_t r, int64_t decrement) {
    DirectCtx* c = g->c;
    u32 atomic_old_var = g->t->atomic_old_var, pointer = p.id;
    u32 header = spv_emit_id(&c->emit), body = spv_emit_id(&c->emit);
    u32 cont = spv_emit_id(&c->emit), merge = spv_emit_id(&c->emit);
    spv_emit_store(&c->emit, atomic_old_var, spv_emit_atomic_load_shared_u32(&c->emit, c->u32_type, pointer));
    spv_emit_branch(&c->emit, header);
    spv_emit_label(&c->emit, header);
    spv_emit_loop_merge(&c->emit, merge, cont);
    spv_emit_branch(&c->emit, body);
    spv_emit_label(&c->emit, body);
    u32 old = spv_emit_load(&c->emit, c->u32_type, atomic_old_var);
    u32 next = shader_wrap_step(c, old, reg_read(c, (u32)r), (int)decrement);
    u32 observed = spv_emit_atomic_compare_exchange_shared_u32(&c->emit, c->u32_type, pointer, next, old);
    spv_emit_store(&c->emit, atomic_old_var, observed);
    spv_emit_branch_conditional(&c->emit, spv_emit_binary(&c->emit, SPV_OP_I_EQUAL, c->bool_type, observed, old), merge,
                                cont);
    spv_emit_label(&c->emit, cont);
    spv_emit_branch(&c->emit, header);
    spv_emit_label(&c->emit, merge);
    return sg_v(spv_emit_load(&c->emit, c->u32_type, atomic_old_var), SGT_U32);
}
/* RED's and ATOM's operations SPIR-V has not, as a CAS loop on atomic_old_var: a float add (size 3), else the wrap
   step (operation 3 INC, else DEC) */
[[maybe_unused]] static SgV sg_AtomicLoop(SgG* g, SgV p, SgV v, int64_t size, int64_t operation) {
    DirectCtx* c = g->c;
    u32 atomic_old_var = g->t->atomic_old_var, pointer = p.id, value = sg_u32(g, v);
    u32 header = spv_emit_id(&c->emit), body = spv_emit_id(&c->emit);
    u32 cont = spv_emit_id(&c->emit), merge = spv_emit_id(&c->emit);
    spv_emit_store(&c->emit, atomic_old_var, spv_emit_load(&c->emit, c->u32_type, pointer));
    spv_emit_branch(&c->emit, header);
    spv_emit_label(&c->emit, header);
    spv_emit_loop_merge(&c->emit, merge, cont);
    spv_emit_branch(&c->emit, body);
    spv_emit_label(&c->emit, body);
    u32 old = spv_emit_load(&c->emit, c->u32_type, atomic_old_var), desired;
    if (size == 3) {
        u32 sum = spv_emit_binary(&c->emit, SPV_OP_F_ADD, c->f32_type, spv_emit_unary(&c->emit, SPV_OP_BITCAST, c->f32_type, old),
                                  spv_emit_unary(&c->emit, SPV_OP_BITCAST, c->f32_type, value));
        desired = spv_emit_unary(&c->emit, SPV_OP_BITCAST, c->u32_type, sum);
    } else
        desired = shader_wrap_step(c, old, value, operation != 3);
    u32 observed = spv_emit_atomic_compare_exchange_u32(&c->emit, c->u32_type, pointer, desired, old);
    spv_emit_store(&c->emit, atomic_old_var, observed);
    u32 done = spv_emit_binary(&c->emit, SPV_OP_I_EQUAL, c->bool_type, observed, old);
    spv_emit_branch_conditional(&c->emit, done, merge, cont);
    spv_emit_label(&c->emit, cont);
    spv_emit_branch(&c->emit, header);
    spv_emit_label(&c->emit, merge);
    return sg_v(spv_emit_load(&c->emit, c->u32_type, atomic_old_var), SGT_U32);
}
[[maybe_unused]] static int64_t sg_SharedAtomicOpcode(SgG* g) { return shader_shared_atomic_opcode(g->w); }
[[maybe_unused]] static int64_t sg_GlobalAtomicOpcode(SgG* g, int64_t reduction) {
    return shader_global_atomic_opcode(g->w, (int)reduction);
}
[[maybe_unused]] static SgV sg_SharedAtomicIndex(SgG* g) {
    return sg_v(shader_shared_atomic_index(g->c, g->a, g->w, g->t->shared_words), SGT_U32);
}
[[maybe_unused]] static int64_t sg_GbAt(SgG* g) { return g->t->gb_at[g->i]; }
[[maybe_unused]] static int64_t sg_GbufBank(SgG* g, int64_t slot) { return SH_GBUF_BANK(g->t->gbuf_off[slot]); }
[[maybe_unused]] static int64_t sg_GbufOffset(SgG* g, int64_t slot) { return SH_GBUF_OFFSET(g->t->gbuf_off[slot]); }
[[maybe_unused]] static void sg_GlobalDynamic(SgG* g) {
    shader_global_dynamic(g->c, g->t->stage, g->w, g->op, g->t->gbuf_off, g->t->ngbuf);
}
[[maybe_unused]] static SgV sg_GlobalAtomicPointer(SgG* g, int64_t slot) {
    return sg53_ptr(shader_global_atomic_pointer(g->c, g->t->stage, (u32)slot, g->t->gbuf_off[slot], g->a, g->w, &g->out[0]));
}
[[maybe_unused]] static SgV sg_AtomicOffset(SgG* g) { return sg_v(g->out[0], SGT_U32); }
[[maybe_unused]] static void sg_LogWritten(SgG* g, int64_t slot, SgV offset, int64_t bytes) {
    shader_log_written(g->c, g->t->stage, (u32)slot, sg_u32(g, offset), (u32)bytes);
}
