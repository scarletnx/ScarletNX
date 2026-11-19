/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sm53_io.sail's back end (after sm53_spirv.h): the attributes' interface variables to_spirv declared, read and
   written by address, the streams of a geometry program, and a fragment's inputs and their interpolation (the
   translator's helpers in emit_io.c and translate.h). Each function is the branch of emit_io's it was */

/* A capture draw contains one original primitive. Keep its guest-visible ID while the native draw starts at 0 */
[[maybe_unused]] static u32 sg53_primitive_read(DirectCtx* c, u32 variable) {
    u32 value = variable ? spv_emit_load(&c->emit, c->u32_type, variable) : c->zero;
    return c->primitive_base ? spv_emit_iadd_u32(&c->emit, c->u32_type, value, c->primitive_base) : value;
}
[[maybe_unused]] static int64_t sg_OutVertices(SgG* g) { return g->t->out_vertices; }
[[maybe_unused]] static SgV sg_SharedIndex(SgG* g, SgV x, int64_t words) {
    return sg_v(shader_shared_index(g->c, sg_u32(g, x), (u32)words), SGT_U32);
}
[[maybe_unused]] static int sg_HasInput(SgG* g, int64_t v) { return v < 32 ? g->t->inputs[v] != 0 : g->t->position_in != 0; }
[[maybe_unused]] static int sg_HasOutputAttr(SgG* g, int64_t v) { return g->t->output_attrs[v] != 0; }
[[maybe_unused]] static int sg_HasPosition(SgG* g) { return g->t->position != 0; }
[[maybe_unused]] static int sg_HasPointSize(SgG* g, int output) {
    return (output ? g->t->point_size : g->t->point_size_in) != 0;
}
[[maybe_unused]] static int sg_HasClip(SgG* g, int output) { return (output ? g->t->clip_output : g->t->clip_input) != 0; }
[[maybe_unused]] static int sg_ClipUsed(SgG* g, int64_t k) { return (g->t->clip_used >> k) & 1; }
[[maybe_unused]] static int sg_HasLayerOutput(SgG* g) { return g->t->layer_output != 0; }
[[maybe_unused]] static int sg_HasLayerInput(SgG* g) { return g->t->layer_input != 0; }
[[maybe_unused]] static int sg_HasViewportOutput(SgG* g) { return g->t->viewport_output != 0; }
[[maybe_unused]] static int sg_HasFragcoord(SgG* g) { return g->t->fragcoord != 0; }
[[maybe_unused]] static int sg_AttrPrivate(SgG* g, int64_t index) { return g->t->attr_private[index]; }
[[maybe_unused]] static SgV sg_InputBits(SgG* g, int64_t v, int64_t has_vertex, SgV vertex, SgV component) {
    const Tr* t = g->t;
    DirectCtx* c = g->c;
    u32 variable = v < 32 ? t->inputs[v] : t->position_in;
    u32 type = v < 32 ? t->input_component_type[v] : c->f32_type;
    u32 bits = spv_emit_load(&c->emit, type,
                             spv_emit_interface_scalar_pointer(&c->emit, SPV_STORAGE_INPUT, type, variable,
                                                               has_vertex ? sg_u32(g, vertex) : 0, sg_u32(g, component)));
    if (type != c->u32_type) bits = spv_emit_unary(&c->emit, SPV_OP_BITCAST, c->u32_type, bits);
    return sg_v(bits, SGT_U32);
}
[[maybe_unused]] static SgV sg53_load_f32(SgG* g, u32 pointer) {
    return sg_v(spv_emit_load(&g->c->emit, g->c->f32_type, pointer), SGT_F32);
}
[[maybe_unused]] static SgV sg_PatchLoad(SgG* g, int64_t storage, int64_t p, SgV component) {
    return sg53_load_f32(g, spv_emit_interface_scalar_pointer(&g->c->emit, (u32)storage, g->c->f32_type,
                                                              g->t->patch_vars[p], 0, sg_u32(g, component)));
}
[[maybe_unused]] static SgV sg_TessLevelLoad(SgG* g, int64_t storage, int64_t inner, int64_t k) {
    return sg53_load_f32(g, spv_emit_interface_scalar_pointer(&g->c->emit, (u32)storage, g->c->f32_type,
                                                              inner ? g->t->tess_inner : g->t->tess_outer, 0,
                                                              shader_u32(g->c, (u32)k)));
}
[[maybe_unused]] static SgV sg_TessCoord(SgG* g, int64_t k) {
    return sg53_load_f32(g, spv_emit_interface_scalar_pointer(&g->c->emit, SPV_STORAGE_INPUT, g->c->f32_type,
                                                              g->t->tess_coord, 0, shader_u32(g->c, (u32)k)));
}
[[maybe_unused]] static SgV sg_PointSizeLoad(SgG* g, SgV output, SgV vertex) {
    DirectCtx* c = g->c;
    int out = output.id == c->true_value;
    u32 type = spv_emit_type_pointer(&c->emit, out ? 3 : 1, c->f32_type);
    return sg53_load_f32(g, SPV(&c->emit, SPV_OP_ACCESS_CHAIN, type, out ? g->t->point_size : g->t->point_size_in,
                                sg_u32(g, vertex)));
}
[[maybe_unused]] static SgV sg_ClipLoad(SgG* g, SgV output, SgV vertex, int64_t k) {
    DirectCtx* c = g->c;
    int out = output.id == c->true_value;
    return sg53_load_f32(g, spv_emit_interface_scalar_pointer(&c->emit, out ? 3 : 1, c->f32_type,
                                                              out ? g->t->clip_output : g->t->clip_input,
                                                              sg_u32(g, vertex), shader_u32(c, (u32)k)));
}
[[maybe_unused]] static SgV sg_OutputLoad(SgG* g, int64_t location, SgV vertex, SgV component) {
    const Tr* t = g->t;
    return sg53_load_f32(g, spv_emit_interface_scalar_pointer(
                                &g->c->emit, location >= 0x80 && t->attr_private[(location - 0x80) >> 4] ? 6 : 3,
                                g->c->f32_type, location < 0x80 ? t->position : t->output_attrs[(location - 0x80) >> 4],
                                sg_u32(g, vertex), sg_u32(g, component)));
}
[[maybe_unused]] static SgV sg_InputLoad(SgG* g, int64_t location, SgV vertex, SgV component) {
    const Tr* t = g->t;
    return sg53_load_f32(g, spv_emit_interface_scalar_pointer(&g->c->emit, SPV_STORAGE_INPUT, g->c->f32_type,
                                                              location < 0x80 ? t->position_in
                                                                              : t->inputs[(location - 0x80) >> 4],
                                                              sg_u32(g, vertex), sg_u32(g, component)));
}
[[maybe_unused]] static SgV sg_PrimitiveRead(SgG* g) { return sg_v(sg53_primitive_read(g->c, g->t->primitive_id), SGT_U32); }
[[maybe_unused]] static SgV sg_InstanceId(SgG* g) {
    const Tr* t = g->t;
    DirectCtx* c = g->c;
    u32 instance = spv_emit_load(&c->emit, c->u32_type, t->instance_index);
    u32 base_bits = spv_emit_push_component_f32(&c->emit, t->push_pointer, t->push, c->zero, c->zero, c->f32_type);
    u32 base = spv_emit_unary(&c->emit, SPV_OP_BITCAST, c->u32_type, base_bits);
    /* with draw parameters the push holds what the draw's first instance adds to the base instance (render.c),
       which BaseInstance gives */
    if (t->base_instance)
        base = spv_emit_binary(&c->emit, SPV_OP_I_SUB, c->u32_type, spv_emit_load(&c->emit, c->u32_type, t->base_instance),
                               base);
    return sg_v(spv_emit_binary(&c->emit, SPV_OP_I_SUB, c->u32_type, instance, base), SGT_U32);
}
[[maybe_unused]] static SgV sg_VertexId(SgG* g) {
    return sg_v(spv_emit_load(&g->c->emit, g->c->u32_type, g->t->vertex_index), SGT_U32);
}
[[maybe_unused]] static void sg_InputComponentWrite(SgG* g, int64_t r, int64_t index, int64_t component) {
    const Tr* t = g->t;
    DirectCtx* c = g->c;
    u32 value = shader_input_component(c, t->input_vec_type[index], t->inputs[index], t->input_component_type[index],
                                       (u32)component);
    reg_write_as(c, (u32)r, value, t->input_component_type[index]);
}
[[maybe_unused]] static SgV sg_InvocationIdInput(SgG* g) {
    return sg_v(shader_input(g->c, g->c->u32_type, g->t->invocation_id), SGT_U32);
}
[[maybe_unused]] static void sg53_store_f32(SgG* g, u32 pointer, int64_t r) {
    if (pointer) spv_emit_store(&g->c->emit, pointer, reg_read_as(g->c, (u32)r, g->c->f32_type));
}
[[maybe_unused]] static void sg_PatchStore(SgG* g, int64_t r, int64_t p, SgV component) {
    sg53_store_f32(g, spv_emit_interface_scalar_pointer(&g->c->emit, SPV_STORAGE_OUTPUT, g->c->f32_type,
                                                        g->t->patch_vars[p], 0, sg_u32(g, component)), r);
}
[[maybe_unused]] static void sg_TessLevelStore(SgG* g, int64_t r, int64_t inner, int64_t k) {
    sg53_store_f32(g, spv_emit_interface_scalar_pointer(&g->c->emit, SPV_STORAGE_OUTPUT, g->c->f32_type,
                                                        inner ? g->t->tess_inner : g->t->tess_outer, 0,
                                                        shader_u32(g->c, (u32)k)), r);
}
[[maybe_unused]] static void sg_PointSizeStore(SgG* g, int64_t r, SgV vertex) {
    DirectCtx* c = g->c;
    u32 type = spv_emit_type_pointer(&c->emit, SPV_STORAGE_OUTPUT, c->f32_type);
    sg53_store_f32(g, SPV(&c->emit, SPV_OP_ACCESS_CHAIN, type, g->t->point_size, sg_u32(g, vertex)), r);
}
[[maybe_unused]] static void sg_ClipStore(SgG* g, int64_t r, SgV vertex, int64_t k) {
    sg53_store_f32(g, spv_emit_interface_scalar_pointer(&g->c->emit, SPV_STORAGE_OUTPUT, g->c->f32_type,
                                                        g->t->clip_output, sg_u32(g, vertex), shader_u32(g->c, (u32)k)),
                   r);
}
[[maybe_unused]] static void sg_OutputStore(SgG* g, int64_t r, int64_t location, SgV vertex, SgV component) {
    const Tr* t = g->t;
    sg53_store_f32(g, spv_emit_interface_scalar_pointer(
                          &g->c->emit, location >= 0x80 && t->attr_private[(location - 0x80) >> 4] ? 6 : 3,
                          g->c->f32_type, location < 0x80 ? t->position : t->output_attrs[(location - 0x80) >> 4],
                          sg_u32(g, vertex), sg_u32(g, component)),
                   r);
}
[[maybe_unused]] static void sg_PointSizeOut(SgG* g, int64_t r) {
    spv_emit_store(&g->c->emit, g->t->point_size, reg_read_as(g->c, (u32)r, g->c->f32_type));
}
[[maybe_unused]] static void sg_ViewportOut(SgG* g, int64_t r) {
    spv_emit_store(&g->c->emit, g->t->viewport_output, reg_read_as(g->c, (u32)r, g->t->i32_type));
}
[[maybe_unused]] static void sg_LayerOut(SgG* g, int64_t r) {
    spv_emit_store(&g->c->emit, g->t->layer_output, reg_read_as(g->c, (u32)r, g->t->i32_type));
}
[[maybe_unused]] static void sg_ClipOut(SgG* g, int64_t index, int64_t r) {
    DirectCtx* c = g->c;
    u32 ptr = spv_emit_output_element_pointer(&c->emit, c->f32_type, g->t->clip_output, shader_u32(c, (u32)index));
    spv_emit_store(&c->emit, ptr, reg_read_as(c, (u32)r, c->f32_type));
}
[[maybe_unused]] static void sg_VectorOut(SgG* g, int64_t location, int64_t component, int64_t r) {
    const Tr* t = g->t;
    DirectCtx* c = g->c;
    u32 variable = location < 0x80 ? t->position : t->output_attrs[(location - 0x80) >> 4];
    u32 vector = spv_emit_load(&c->emit, t->vec4, variable);
    u32 value = reg_read_as(c, (u32)r, c->f32_type);
    vector = spv_emit_vec4_insert(&c->emit, t->vec4, vector, value, (u32)component);
    spv_emit_store(&c->emit, variable, vector);
}
[[maybe_unused]] static int64_t sg_StreamMask(SgG* g) { return g->t->stream_mask; }
[[maybe_unused]] static int sg_XfbOn(SgG* g) { return g->t->xfb_on; }
[[maybe_unused]] static int sg_PositionFix(SgG* g) { return g->t->position_fix; }
[[maybe_unused]] static int sg_PointSizeUsed(SgG* g) { return g->t->point_size_used; }
/* the selection of stream target: its emit's label taken where the stream is it (its merge, out[0]) */
[[maybe_unused]] static void sg_StreamSelect(SgG* g, SgV stream, int64_t target) {
    DirectCtx* c = g->c;
    u32 literal = shader_u32(c, (u32)target);
    u32 emit_label = spv_emit_id(&c->emit), merge = spv_emit_id(&c->emit);
    u32 selected = spv_emit_binary(&c->emit, SPV_OP_I_EQUAL, c->bool_type, sg_u32(g, stream), literal);
    spv_emit_selection_merge(&c->emit, merge);
    spv_emit_branch_conditional(&c->emit, selected, emit_label, merge);
    spv_emit_label(&c->emit, emit_label);
    g->out[0] = merge;
}
[[maybe_unused]] static void sg_StreamEnd(SgG* g) {
    spv_emit_branch(&g->c->emit, g->out[0]);
    spv_emit_label(&g->c->emit, g->out[0]);
}
/* stream 0's vertex: the position as written to transform feedback, before its fix, and the primitive's number */
[[maybe_unused]] static void sg_VertexEnd(SgG* g) {
    const Tr* t = g->t;
    DirectCtx* c = g->c;
    if (t->xfb_on) shader_xfb_store(c, t->xfb_out);
    if (t->position_fix) shader_vertex_position_fix(c, t->position, t->push, t->push_pointer, t->vec4);
    spv_emit_store(&c->emit, t->primitive_id_out, spv_emit_load(&c->emit, c->u32_type, t->primitive_id));
}
/* stream target's capture of transform feedback: each word its buffers take, from the attribute it names */
[[maybe_unused]] static void sg_XfbCapture(SgG* g, int64_t target) {
    const Tr* t = g->t;
    DirectCtx* c = g->c;
    for (u32 buffer = 0; buffer < 4; buffer++) {
        if (t->si->xfb.stream[buffer] != target) continue;
        for (u32 word = 0; word < t->si->xfb.count[buffer] && word < 128; word++) {
            u32 variable = t->stream_capture[buffer][word];
            if (!variable) continue;
            u32 attr = t->si->xfb.layout[buffer][word], value = t->zero_f;
            if (shader_legacy_group(attr * 4) >= 0) attr = shader_legacy_address(attr * 4) / 4;
            if (attr >= 32 && t->output_attrs[(attr - 32) / 4])
                value = spv_emit_vec4_extract(&c->emit, c->f32_type,
                                              spv_emit_load(&c->emit, t->vec4, t->output_attrs[(attr - 32) / 4]), attr & 3);
            else if (attr >= 28 && attr <= 31 && t->position)
                value = spv_emit_vec4_extract(&c->emit, c->f32_type, spv_emit_load(&c->emit, t->vec4, t->position),
                                              attr - 28);
            else if (attr == 27 && t->point_size)
                value = spv_emit_load(&c->emit, c->f32_type, t->point_size);
            else if (attr == 25 && t->layer_output)
                value = spv_emit_unary(&c->emit, SPV_OP_BITCAST, c->f32_type,
                                       spv_emit_load(&c->emit, t->i32_type, t->layer_output));
            else if (attr == 26 && t->viewport_output)
                value = spv_emit_unary(&c->emit, SPV_OP_BITCAST, c->f32_type,
                                       spv_emit_load(&c->emit, t->i32_type, t->viewport_output));
            spv_emit_store(&c->emit, variable, value);
        }
    }
}
[[maybe_unused]] static void sg_EmitVertex(SgG* g, int64_t target) {
    u32 literal = shader_u32(g->c, (u32)target);
    if (g->t->stream_mask)
        spv_emit_void_op(&g->c->emit, SPV_OP_EMIT_STREAM_VERTEX, &literal, 1);
    else
        spv_emit_emit_vertex(&g->c->emit);
}
[[maybe_unused]] static void sg_EndPrimitive(SgG* g, int64_t target) {
    u32 literal = shader_u32(g->c, (u32)target);
    if (g->t->stream_mask)
        spv_emit_void_op(&g->c->emit, SPV_OP_END_STREAM_PRIMITIVE, &literal, 1);
    else
        spv_emit_end_primitive(&g->c->emit);
}
[[maybe_unused]] static void sg_DefaultPointSize(SgG* g) { spv_emit_store(&g->c->emit, g->t->point_size, g->t->default_point_size); }
[[maybe_unused]] static int sg_SphImapAny(SgG* g, int64_t v) { return (g->t->sph[6 + v / 4] >> (v % 4 * 8)) & 0xff; }
[[maybe_unused]] static int64_t sg_IpaInterpolation(SgG* g, int64_t input) { return shader_ipa_interpolation(g->t->sph, (u32)input); }
[[maybe_unused]] static int sg_IpaPosition(SgG* g, int64_t addr) { return shader_ipa_position(g->t->pos_in, g->t->sph, (u32)addr); }
[[maybe_unused]] static int sg_IpaOverW(SgG* g) { return shader_direct_ipa_over_w(g->t->flow, g->i); }
[[maybe_unused]] static int sg_PvMask(SgG* g, int64_t input) { return (g->t->pv_mask >> input) & 1; }
[[maybe_unused]] static SgV sg_FragNdc(SgG* g, int64_t k) { return sg_v(g->t->frag_ndc[k], SGT_F32); }
/* its three vertices' values: the first's, and the others' differences from it by their weights, so that a value the
   three share comes out as it is (Eden: one game truncates such an input) */
[[maybe_unused]] static SgV sg_ProvokingValue(SgG* g, int64_t input, int64_t component) {
    const Tr* t = g->t;
    DirectCtx* c = g->c;
    const u32 from[3] = {t->inputs[input], t->pv_flat ? t->pv_copy[input][0] : t->inputs[input],
                         t->pv_flat ? t->pv_copy[input][1] : t->inputs[input]};
    u32 at[3];
    for (u32 k = 0; k < 3; k++) at[k] = shader_vertex_value(c, t->vec4, from, t->pv_flat, k, (u32)component);
    u32 value = at[0];
    for (u32 k = 1; k < 3; k++) {
        u32 past = spv_emit_binary(&c->emit, SPV_OP_F_ADD, c->f32_type, at[k],
                                   spv_emit_unary(&c->emit, SPV_OP_F_NEGATE, c->f32_type, at[0]));
        value = spv_emit_binary(&c->emit, SPV_OP_F_ADD, c->f32_type, value,
                                spv_emit_binary(&c->emit, SPV_OP_F_MUL, c->f32_type, t->pv_weight[k - 1], past));
    }
    return sg_v(value, SGT_F32);
}
[[maybe_unused]] static SgV sg_FragCoordComponent(SgG* g, int64_t k) {
    return sg_v(shader_input_component(g->c, g->t->vec4, g->t->fragcoord, g->c->f32_type, (u32)k), SGT_F32);
}
[[maybe_unused]] static SgV sg_FrontFace(SgG* g) { return sg_v(spv_emit_load(&g->c->emit, g->c->bool_type, g->t->frontface), SGT_BOOL); }
[[maybe_unused]] static SgV sg_LayerInput(SgG* g) { return sg_v(spv_emit_load(&g->c->emit, g->t->i32_type, g->t->layer_input), SGT_I32); }
[[maybe_unused]] static SgV sg_PointCoordComponent(SgG* g, int64_t k) {
    return sg_v(shader_input_component(g->c, g->t->vec2, g->t->point_coord, g->c->f32_type, (u32)k), SGT_F32);
}
[[maybe_unused]] static SgV sg_SpriteFlip(SgG* g) { return sg_v(shader_spec(g->c, SH_SPEC_SPRITE_FLIP, 0), SGT_U32); }
[[maybe_unused]] static int sg_NeedsScale(SgG* g) { return g->t->needs_scale; }
[[maybe_unused]] static SgV sg_ResolutionScale(SgG* g) {
    DirectCtx* c = g->c;
    u32 zero_index = shader_u32(c, 0);
    return sg_v(spv_emit_uniform_component(&c->emit, c->scale_pointer, c->scale_buffer, zero_index, zero_index, zero_index,
                                           c->f32_type),
                SGT_F32);
}
[[maybe_unused]] static SgV sg_FragYOrigin(SgG* g, SgV y) { return sg_v(shader_frag_y_origin(g->c, g->t->push, sg_f(g, y)), SGT_F32); }
[[maybe_unused]] static SgV sg_InputComponent(SgG* g, int64_t input, int64_t k) {
    return sg_v(shader_input_component(g->c, g->t->vec4, g->t->inputs[input], g->c->f32_type, (u32)k), SGT_F32);
}
[[maybe_unused]] static SgV sg_InputScalar(SgG* g, int64_t v, SgV component) {
    DirectCtx* c = g->c;
    return sg_v(spv_emit_load(&c->emit, c->f32_type,
                              spv_emit_interface_scalar_pointer(&c->emit, SPV_STORAGE_INPUT, c->f32_type, g->t->inputs[v],
                                                                0, sg_u32(g, component))),
                SGT_F32);
}
[[maybe_unused]] static SgV sg_FMulNoContract(SgG* g, SgV x, SgV y) {
    DirectCtx* c = g->c;
    u32 a = sg_f(g, x), b = sg_f(g, y);
    u32 v = spv_emit_binary(&c->emit, SPV_OP_F_MUL, c->f32_type, a, b);
    spv_emit_no_contraction(&c->emit, v);
    return sg_v(v, SGT_F32);
}
[[maybe_unused]] static SgV sg_FloatSaturate(SgG* g, SgV x, int64_t on) {
    return sg_v(shader_float_saturate(g->c, sg_f(g, x), (int)on), SGT_F32);
}
[[maybe_unused]] static SgV sg_SampleId(SgG* g) { return sg_v(spv_emit_load(&g->c->emit, g->c->u32_type, g->t->sample_id), SGT_U32); }
[[maybe_unused]] static SgV sg_CoverageWord(SgG* g) {
    DirectCtx* c = g->c;
    return sg_v(spv_emit_load(&c->emit, c->u32_type,
                              spv_emit_interface_scalar_pointer(&c->emit, SPV_STORAGE_INPUT, c->u32_type, g->t->coverage,
                                                                0, c->zero)),
                SGT_U32);
}
