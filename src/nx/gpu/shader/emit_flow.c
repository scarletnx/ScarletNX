/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Control flow: the branches, calls, returns, KIL and EXIT, the control stack's pushes and pops, and the structured
   constructs the structure gives */
#include "nx/gpu/shader/translate.h"

/* Remove the nearest token of kind, dropping tokens pushed after it as Maxwell's CRS stack does. */
void shader_dynamic_pop(DirectCtx* c, u32 stack_ptr, const u32 stack[32], u32 kind_value, u32 fallback, u32* found,
                        u32* destination, u32* new_sp) {
    u32 sp = spv_emit_load(&c->emit, c->u32_type, stack_ptr);
    u32 kind = spv_emit_constant_u32(&c->emit, c->u32_type, kind_value);
    u32 shift = spv_emit_constant_u32(&c->emit, c->u32_type, 16);
    u32 mask = spv_emit_constant_u32(&c->emit, c->u32_type, 0xffff);
    *found = c->false_value;
    *destination = fallback;
    *new_sp = sp;
    for (int k = 31; k >= 0; k--) {
        u32 index = spv_emit_constant_u32(&c->emit, c->u32_type, (u32)k);
        u32 in_range = spv_emit_u_less(&c->emit, c->bool_type, index, sp);
        u32 token = spv_emit_load(&c->emit, c->u32_type, stack[k]);
        u32 matches = spv_emit_i_equal(&c->emit, c->bool_type,
                                       spv_emit_shift_right_u32(&c->emit, c->u32_type, token, shift), kind);
        u32 choose = spv_emit_bool_binary(&c->emit, c->bool_type, 0, in_range, matches);
        choose =
            spv_emit_bool_binary(&c->emit, c->bool_type, 0, choose, spv_emit_bool_not(&c->emit, c->bool_type, *found));
        *destination = spv_emit_select(&c->emit, c->u32_type, choose,
                                       spv_emit_bitwise_and_u32(&c->emit, c->u32_type, token, mask), *destination);
        *new_sp = spv_emit_select(&c->emit, c->u32_type, choose, index, *new_sp);
        *found = spv_emit_bool_binary(&c->emit, c->bool_type, 1, *found, choose);
    }
}

static void se_branch(DirectCtx* c, StructEmit* se, u32 label) {
    spv_emit_branch(&c->emit, label);
    se->open = 0;
}

/* a scope's merge: from a break, or from running off its end */
static void se_break(DirectCtx* c, StructEmit* se, u32 scope) {
    se->used[scope] = 1;
    se_branch(c, se, se->label[4 * scope + 3]);
}

static void se_ensure_open(DirectCtx* c, StructEmit* se) {
    if (se->open) return;
    spv_emit_label(&c->emit, spv_emit_id(&c->emit)); /* code nothing reaches, which SPIR-V takes in a block */
    se->open = 1;
}

/* At the merge of a scope that a pending exit may leave through: it goes on to the parent scope (which it leaves
   by its merge, or repeats when it is the exit's loop) when the variable is set, and the code after the scope runs
   when it isn't */
static void se_pending(DirectCtx* c, StructEmit* se, u32 scope) {
    const ShsScope* s = &se->st->scope[scope];
    u32 parent = s->parent - 1;
    u32 t = spv_emit_load(&c->emit, c->u32_type, se->flag);
    u32 go = spv_emit_id(&c->emit), pending = spv_emit_id(&c->emit);
    u32 here = spv_emit_constant_u32(&c->emit, c->u32_type, parent + 1);
    u32 set = spv_emit_i_not_equal(&c->emit, c->bool_type, t, c->zero);
    spv_emit_selection_merge(&c->emit, go);
    spv_emit_branch_conditional(&c->emit, set, pending, go);
    spv_emit_label(&c->emit, pending);
    if (!se->st->scope[parent].loop) {
        if (s->arrive && s->beyond)
            spv_emit_store(
                &c->emit, se->flag,
                spv_emit_select(&c->emit, c->u32_type, spv_emit_i_equal(&c->emit, c->bool_type, t, here), c->zero, t));
        else if (s->arrive)
            spv_emit_store(&c->emit, se->flag, c->zero);
        se_break(c, se, parent);
    } else if (s->arrive && s->beyond) {
        u32 again = spv_emit_id(&c->emit), out = spv_emit_id(&c->emit), merge = spv_emit_id(&c->emit);
        u32 arrived = spv_emit_i_equal(&c->emit, c->bool_type, t, here);
        spv_emit_selection_merge(&c->emit, merge);
        spv_emit_branch_conditional(&c->emit, arrived, again, out);
        spv_emit_label(&c->emit, again);
        spv_emit_store(&c->emit, se->flag, c->zero);
        spv_emit_branch(&c->emit, se->label[4 * parent + 2]);
        spv_emit_label(&c->emit, out);
        se_break(c, se, parent);
        spv_emit_label(&c->emit, merge);
        spv_emit_void_op(&c->emit, SPV_OP_UNREACHABLE, NULL, 0);
    } else if (s->arrive) {
        spv_emit_store(&c->emit, se->flag, c->zero);
        spv_emit_branch(&c->emit, se->label[4 * parent + 2]);
    } else
        se_break(c, se, parent);
    spv_emit_label(&c->emit, go);
    se->open = 1;
}

/* Writes the tokens up to the next body (SE_BODY, its words in body_start and body_end), the program's end that
   control runs off (SE_END: the caller writes what EXIT writes, without the outputs, and returns), or the last
   (SE_DONE) */
int shader_structure_step(DirectCtx* c, StructEmit* se) {
    const ShStructure* st = se->st;
    while (se->next < st->ntokens) {
        const ShsToken* t = &st->token[se->next++];
        u32* sel = se->label + 4 * st->nscopes + 2 * (se->selections ? se->selections - 1 : 0);
        u8* sel_used = se->used + st->nscopes + (se->selections ? se->selections - 1 : 0);
        u32* l = se->label + 4 * t->a;
        switch (t->kind) {
            case SHS_BODY:
                se_ensure_open(c, se);
                se->body_start = t->a;
                se->body_end = t->b;
                return SE_BODY;
            case SHS_END: se_ensure_open(c, se); return SE_END;
            case SHS_RETURNED: se->open = 0; break;
            case SHS_OPEN_BLOCK:
            case SHS_OPEN_LOOP:
                for (u32 k = 0; k < 4; k++) l[k] = spv_emit_id(&c->emit);
                if (se->open) spv_emit_branch(&c->emit, l[0]);
                spv_emit_label(&c->emit, l[0]);
                spv_emit_loop_merge(&c->emit, l[3], l[2]);
                spv_emit_branch(&c->emit, l[1]);
                spv_emit_label(&c->emit, l[1]);
                se->open = 1;
                break;
            case SHS_CLOSE:
                if (se->open) se_break(c, se, t->a);
                /* a block scope's continue target is never reached: the loop it would repeat runs once */
                spv_emit_label(&c->emit, l[2]);
                spv_emit_branch(&c->emit, l[0]);
                spv_emit_label(&c->emit, l[3]);
                if (!se->used[t->a]) {
                    spv_emit_void_op(&c->emit, SPV_OP_UNREACHABLE, NULL, 0);
                    se->open = 0;
                } else {
                    se->open = 1;
                    if (st->scope[t->a].check) se_pending(c, se, t->a);
                }
                break;
            case SHS_IF:
                se_ensure_open(c, se);
                sel += se->selections ? 2 : 0;
                sel_used += se->selections ? 1 : 0;
                se->selections++;
                sel[0] = spv_emit_id(&c->emit);
                sel[1] = spv_emit_id(&c->emit);
                *sel_used = 0;
                {
                    u32 then = spv_emit_id(&c->emit);
                    spv_emit_selection_merge(&c->emit, sel[0]);
                    spv_emit_branch_conditional(&c->emit, c->branch_cond ? c->branch_cond : c->true_value, then,
                                                sel[1]);
                    spv_emit_label(&c->emit, then);
                }
                se->open = 1;
                break;
            case SHS_ELSE:
                if (se->open) {
                    *sel_used = 1;
                    se_branch(c, se, sel[0]);
                }
                spv_emit_label(&c->emit, sel[1]);
                se->open = 1;
                break;
            case SHS_SWITCH: {
                /* the cases' words, from their tokens up to the switch's end */
                u32 literals[64], labels[64], count = 0, depth = 0;
                for (u32 k = se->next; k < st->ntokens && count < 64; k++) {
                    u32 kind = st->token[k].kind;
                    if (kind == SHS_SWITCH) depth++;
                    if (kind == SHS_ENDSWITCH && !depth--) break;
                    if (kind == SHS_CASE && !depth) literals[count++] = st->token[k].b;
                }
                se_ensure_open(c, se);
                sel += se->selections ? 2 : 0;
                sel_used += se->selections ? 1 : 0;
                se->selections++;
                sel[0] = spv_emit_id(&c->emit);
                sel[1] = spv_emit_id(&c->emit); /* the default's label, the cases' from the next id on */
                for (u32 k = 0; k < count; k++) labels[k] = spv_emit_id(&c->emit);
                *sel_used = 0;
                spv_emit_selection_merge(&c->emit, sel[0]);
                spv_emit_switch_u32(&c->emit, c->branch_target, sel[1], literals, labels, count);
                se->open = 0;
                break;
            }
            case SHS_CASE:
            case SHS_DEFAULT:
                if (se->open) {
                    *sel_used = 1;
                    se_branch(c, se, sel[0]);
                }
                spv_emit_label(&c->emit, t->kind == SHS_DEFAULT ? sel[1] : sel[1] + 1 + t->a);
                se->open = 1;
                break;
            case SHS_ENDSWITCH:
            case SHS_ENDIF:
                if (se->open) {
                    *sel_used = 1;
                    se_branch(c, se, sel[0]);
                }
                spv_emit_label(&c->emit, sel[0]);
                se->open = *sel_used;
                if (!se->open) spv_emit_void_op(&c->emit, SPV_OP_UNREACHABLE, NULL, 0);
                se->selections--;
                break;
            case SHS_BREAK:
                se_ensure_open(c, se);
                if (t->b) spv_emit_store(&c->emit, se->flag, spv_emit_constant_u32(&c->emit, c->u32_type, t->b));
                se_break(c, se, t->a);
                break;
            case SHS_CONTINUE:
                se_ensure_open(c, se);
                se_branch(c, se, l[2]);
                break;
            default: break;
        }
    }
    return SE_DONE;
}

/* the branches, calls, returns, the control stack's pushes and pops, KIL and EXIT */
void emit_flow(const Tr* t, u32 i, u64 w, int op, u32 d, u32 a, u32 b) {
    ShaderInfo* si = t->si;
    int stage = t->stage;
    u32 n = t->n;
    u32 target = t->target;
    int passthrough = t->passthrough;
    u32 in_vertices = t->in_vertices;
    int has_pexit = t->has_pexit;
    u32* pop_targets = t->pop_targets;
    int dynamic_stack = t->dynamic_stack;
    DirectCtx* c = t->c;
    u32 i32_type = t->i32_type;
    u32 vec4 = t->vec4;
    u32 vec4_u32 = t->vec4_u32;
    u32 vec4_i32 = t->vec4_i32;
    u32 zero_f = t->zero_f;
    u32* inputs = t->inputs;
    u32* outputs = t->outputs;
    u32 position = t->position;
    u32* output_attrs = t->output_attrs;
    u32 point_size = t->point_size;
    u32 layer_output = t->layer_output;
    u8* attr_private = t->attr_private;
    u32(*xfb_components)[4] = t->xfb_components;
    int xfb_on = t->xfb_on;
    u32 clip_output = t->clip_output;
    u32 clip_count = t->clip_count;
    u32 depth_output = t->depth_output;
    u32 position_in = t->position_in;
    u32 primitive_id = t->primitive_id;
    u32 primitive_id_out = t->primitive_id_out;
    int depth_fix = t->depth_fix;
    u32 sample_mask_output = t->sample_mask_output;
    u32 push = t->push;
    u32 push_pointer = t->push_pointer;
    u32 stack_ptr = t->stack_ptr;
    u32* stack = t->stack;
    u32 call_ptr = t->call_ptr;
    u32* call_stack = t->call_stack;
    u32 pc = t->pc;
    ShStructure* structure = t->structure;
    if (op == O_CAL || op == O_PRET) {
        /* CAL pushes the instruction after it and jumps. PRET only pushes its target, where a later RET returns */
        u32 sp = spv_emit_load(&c->emit, c->u32_type, call_ptr);
        u32 room = spv_emit_u_less(&c->emit, c->bool_type, sp, spv_emit_constant_u32(&c->emit, c->u32_type, 32));
        u32 address = spv_emit_constant_u32(&c->emit, c->u32_type,
                                            op == O_PRET ? bra_target(w, i) : shader_next_instruction(i, n));
        for (u32 k = 0; k < 32; k++) {
            u32 at = spv_emit_i_equal(&c->emit, c->bool_type, sp, spv_emit_constant_u32(&c->emit, c->u32_type, k));
            u32 old = spv_emit_load(&c->emit, c->u32_type, call_stack[k]);
            spv_emit_store(&c->emit, call_stack[k], spv_emit_select(&c->emit, c->u32_type, at, address, old));
        }
        spv_emit_store(&c->emit, call_ptr,
                       spv_emit_iadd_u32(&c->emit, c->u32_type, sp,
                                         spv_emit_select(&c->emit, c->u32_type, room,
                                                         spv_emit_constant_u32(&c->emit, c->u32_type, 1), c->zero)));
        if (op == O_CAL)
            spv_emit_store(&c->emit, pc,
                           spv_emit_select(&c->emit, c->u32_type, room,
                                           spv_emit_constant_u32(&c->emit, c->u32_type, bra_target(w, i)),
                                           spv_emit_constant_u32(&c->emit, c->u32_type, UINT32_MAX)));
    } else if (op == O_RET) {
        u32 sp = spv_emit_load(&c->emit, c->u32_type, call_ptr);
        u32 has_entry = spv_emit_u_less(&c->emit, c->bool_type, c->zero, sp);
        u32 index = spv_emit_isub_u32(&c->emit, c->u32_type, sp, spv_emit_constant_u32(&c->emit, c->u32_type, 1));
        u32 destination = spv_emit_constant_u32(&c->emit, c->u32_type, UINT32_MAX);
        for (u32 k = 0; k < 32; k++) {
            u32 at = spv_emit_i_equal(&c->emit, c->bool_type, index, spv_emit_constant_u32(&c->emit, c->u32_type, k));
            u32 old = spv_emit_load(&c->emit, c->u32_type, call_stack[k]);
            destination = spv_emit_select(&c->emit, c->u32_type, at, old, destination);
        }
        spv_emit_store(&c->emit, call_ptr, spv_emit_select(&c->emit, c->u32_type, has_entry, index, c->zero));
        spv_emit_store(&c->emit, pc, destination);
    } else if (dynamic_stack && (op == O_SSY || op == O_PBK || op == O_PCNT || op == O_PEXIT)) {
        u32 sp = spv_emit_load(&c->emit, c->u32_type, stack_ptr);
        u32 token = spv_emit_constant_u32(&c->emit, c->u32_type, bra_target(w, i) | (u32)CRS_KIND(op) << 16);
        for (u32 k = 0; k < 32; k++) {
            u32 at = spv_emit_i_equal(&c->emit, c->bool_type, sp, spv_emit_constant_u32(&c->emit, c->u32_type, k));
            u32 old = spv_emit_load(&c->emit, c->u32_type, stack[k]);
            spv_emit_store(&c->emit, stack[k], spv_emit_select(&c->emit, c->u32_type, at, token, old));
        }
        u32 room = spv_emit_u_less(&c->emit, c->bool_type, sp, spv_emit_constant_u32(&c->emit, c->u32_type, 32));
        u32 inc =
            spv_emit_select(&c->emit, c->u32_type, room, spv_emit_constant_u32(&c->emit, c->u32_type, 1), c->zero);
        spv_emit_store(&c->emit, stack_ptr, spv_emit_iadd_u32(&c->emit, c->u32_type, sp, inc));
    } else if (dynamic_stack && (op == O_SYNC || op == O_BRK || op == O_CONT)) {
        u32 found, destination, new_sp;
        shader_dynamic_pop(c, stack_ptr, stack, CRS_KIND(op), spv_emit_load(&c->emit, c->u32_type, pc), &found,
                           &destination, &new_sp);
        spv_emit_store(&c->emit, pc, destination);
        spv_emit_store(&c->emit, stack_ptr, new_sp);
    } else if (op == O_BRA || op == O_SYNC || op == O_BRK || op == O_CONT) {
        u32 target_index = op == O_BRA ? bra_target(w, i) : pop_targets ? pop_targets[i] : 0;
        u32 destination = spv_emit_constant_u32(&c->emit, c->u32_type, target_index);
        spv_emit_store(&c->emit, pc, destination);
    } else if (op == O_BRX) {
        u32 offset = spv_emit_constant_u32(&c->emit, i32_type, (u32)((i32)(B(w, 20, 24) << 8) >> 8));
        u32 signed_index = spv_emit_iadd_u32(&c->emit, i32_type, reg_read_as(c, a, i32_type), offset);
        u32 words = spv_emit_sdiv_i32(&c->emit, i32_type, signed_index, spv_emit_constant_u32(&c->emit, i32_type, 8));
        u32 target = spv_emit_iadd_u32(&c->emit, c->u32_type, spv_emit_bitcast(&c->emit, c->u32_type, words),
                                       spv_emit_constant_u32(&c->emit, c->u32_type, i + 1));
        u32 after_ten =
            spv_emit_isub_u32(&c->emit, c->u32_type, target, spv_emit_constant_u32(&c->emit, c->u32_type, 10));
        u32 aligned = spv_emit_i_equal(
            &c->emit, c->bool_type,
            spv_emit_bitwise_and_u32(&c->emit, c->u32_type, after_ten, spv_emit_constant_u32(&c->emit, c->u32_type, 3)),
            c->zero);
        u32 in_range = spv_emit_bool_not(
            &c->emit, c->bool_type,
            spv_emit_u_less(&c->emit, c->bool_type, target, spv_emit_constant_u32(&c->emit, c->u32_type, 10)));
        u32 needs_skip = spv_emit_bool_binary(&c->emit, c->bool_type, 0, aligned, in_range);
        target = spv_emit_iadd_u32(&c->emit, c->u32_type, target,
                                   spv_emit_select(&c->emit, c->u32_type, needs_skip,
                                                   spv_emit_constant_u32(&c->emit, c->u32_type, 1), c->zero));
        if (structure)
            c->branch_target = target; /* the structure's SHS_SWITCH */
        else
            spv_emit_store(&c->emit, pc, target);
    } else if (op == O_CCTL || op == O_CCTLL || op == O_IDE || op == O_BPT) {
        /* These cache and breakpoint hints have no shader-visible result. */
    } else if (op == O_KIL) {
        /* the lane goes on as a helper, so that its quad's derivatives stay defined, and nothing it writes
           lands: what the hardware gives, and what Eden does. Inside the flow loop it goes on as a lane that
           only remembers it, and is discarded after the loop (DirectCtx.discarded) */
        if (c->discarded)
            spv_emit_store(&c->emit, c->discarded, c->true_value);
        else
            spv_emit_demote(&c->emit);
    } else if (op == O_EXIT) {
        u32 pexit_merge = 0;
        if (has_pexit) {
            u32 found, destination, new_sp;
            u32 branch_label = spv_emit_id(&c->emit), exit_label = spv_emit_id(&c->emit);
            pexit_merge = spv_emit_id(&c->emit);
            shader_dynamic_pop(c, stack_ptr, stack, CRS_KIND(O_PEXIT), c->zero, &found, &destination, &new_sp);
            spv_emit_selection_merge(&c->emit, pexit_merge);
            spv_emit_branch_conditional(&c->emit, found, branch_label, exit_label);
            spv_emit_label(&c->emit, branch_label);
            spv_emit_store(&c->emit, pc, destination);
            spv_emit_store(&c->emit, stack_ptr, new_sp);
            spv_emit_branch(&c->emit, pexit_merge);
            spv_emit_label(&c->emit, exit_label);
        }
        if (passthrough)
            shader_geometry_passthrough(c, si, in_vertices, vec4, position, position_in, output_attrs, inputs,
                                        point_size, layer_output, i32_type, clip_output, clip_count, primitive_id,
                                        primitive_id_out, depth_fix, push, push_pointer);
        if (xfb_on && stage != ST_GS) shader_xfb_store(c, vec4, output_attrs, attr_private, xfb_components);
        if (depth_fix && stage != ST_GS) { /* a geometry program's vertices are fixed as it emits them */
            shader_vertex_depth_fix(c, position, push, push_pointer, vec4);
        } else if (stage == ST_FS) {
            u32 reg = 0, output = 0, alpha = zero_f;
            for (u32 rt = 0; rt < 8; rt++) {
                u32 mask = (target >> (rt * 4)) & 15;
                if (!mask) continue;
                u32 components[4];
                int integer = !!(si->rt_int_mask & (1u << rt));
                int signed_color = !!(si->rt_sint_mask & (1u << rt));
                u32 color_type = integer ? signed_color ? vec4_i32 : vec4_u32 : vec4;
                for (u32 k = 0; k < 4; k++, reg++) {
                    u32 raw = mask & (1u << k) ? reg_read(c, reg) : c->zero;
                    components[k] = integer            ? signed_color ? spv_emit_bitcast(&c->emit, i32_type, raw) : raw
                                    : mask & (1u << k) ? spv_emit_bitcast(&c->emit, c->f32_type, raw)
                                                       : zero_f;
                    if (rt == 0 && k == 3) alpha = spv_emit_bitcast(&c->emit, c->f32_type, raw);
                }
                u32 color = spv_emit_vec4_construct(&c->emit, color_type, components);
                spv_emit_store(&c->emit, outputs[output++], color);
            }
            if (sample_mask_output)
                spv_emit_store(
                    &c->emit,
                    spv_emit_interface_scalar_pointer(&c->emit, 3, c->u32_type, sample_mask_output, 0, c->zero),
                    reg_read(c, reg));
            if (depth_output) spv_emit_store(&c->emit, depth_output, reg_read_as(c, reg + 1, c->f32_type));
            if (push && (target & 15)) shader_alpha_test(c, push, push_pointer, alpha);
        }
        if (c->discarded && !structure) { /* the lane leaves the flow loop, and a discarded one is discarded after it */
            spv_emit_store(&c->emit, pc, spv_emit_constant_u32(&c->emit, c->u32_type, UINT32_MAX));
            if (has_pexit) spv_emit_branch(&c->emit, pexit_merge);
        } else {
            if (c->discarded) { /* structured: a discarded lane is discarded as it ends */
                u32 kill_label = spv_emit_id(&c->emit), kept = spv_emit_id(&c->emit);
                u32 discard = spv_emit_load(&c->emit, c->bool_type, c->discarded);
                spv_emit_selection_merge(&c->emit, kept);
                spv_emit_branch_conditional(&c->emit, discard, kill_label, kept);
                spv_emit_label(&c->emit, kill_label);
                spv_emit_kill(&c->emit);
                spv_emit_label(&c->emit, kept);
            }
            spv_emit_return(&c->emit);
        }
        if (has_pexit) spv_emit_label(&c->emit, pexit_merge);
    }
}
