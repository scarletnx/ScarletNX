/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Control flow: the structured constructs the structure gives, and the control stack's pops */
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
