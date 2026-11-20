/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Constant buffers, local, shared and global memory and their atomics */
#include "nx/gpu/shader/translate.h"
#include "nx/gpu/shader/write_log.h"

/* a shared memory word index kept inside the array the shader declares: out of range is undefined on the console,
   and past an OpTypeArray in SPIR-V. A vertex of an input or output array is kept inside it the same way */
u32 shader_shared_index(DirectCtx* c, u32 index, u32 words) {
    u32 last = shader_u32(c, words - 1);
    return spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->u32_type,
                            spv_emit_binary(&c->emit, SPV_OP_U_LESS_THAN, c->bool_type, last, index), last, index);
}
/* what an atomic INC (decrement 0) or DEC (1) leaves in place of old, value its operand: INC wraps to 0 once old
   reaches value, DEC to value from 0 or from above it (CUDA's atomicInc and atomicDec, which these are) */
u32 shader_wrap_step(DirectCtx* c, u32 old, u32 value, int decrement) {
    if (!decrement) {
        u32 increment = spv_emit_iadd_u32(&c->emit, c->u32_type, old, shader_u32(c, 1));
        return spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->u32_type,
                                cc_not(c, spv_emit_binary(&c->emit, SPV_OP_U_LESS_THAN, c->bool_type, old, value)),
                                c->zero, increment);
    }
    u32 decremented = spv_emit_binary(&c->emit, SPV_OP_I_SUB, c->u32_type, old, shader_u32(c, 1));
    u32 zero = spv_emit_binary(&c->emit, SPV_OP_I_EQUAL, c->bool_type, old, c->zero);
    u32 greater = spv_emit_binary(&c->emit, SPV_OP_U_LESS_THAN, c->bool_type, value, old);
    return spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->u32_type, cc_or(c, zero, greater), value, decremented);
}
/* an atomic INC or DEC (shader_wrap_step) on pointer, an image texel's, which only atomics may read: a compare and exchange
   loop through old_var. What the texel held before */
u32 shader_atomic_wrap_loop(DirectCtx* c, u32 old_var, u32 pointer, u32 value, int decrement) {
    u32 header = spv_emit_id(&c->emit), body = spv_emit_id(&c->emit);
    u32 cont = spv_emit_id(&c->emit), merge = spv_emit_id(&c->emit);
    spv_emit_store(&c->emit, old_var, spv_emit_atomic_load_u32(&c->emit, c->u32_type, pointer));
    spv_emit_branch(&c->emit, header);
    spv_emit_label(&c->emit, header);
    spv_emit_loop_merge(&c->emit, merge, cont);
    spv_emit_branch(&c->emit, body);
    spv_emit_label(&c->emit, body);
    u32 old = spv_emit_load(&c->emit, c->u32_type, old_var);
    u32 observed = spv_emit_atomic_compare_exchange_u32(&c->emit, c->u32_type, pointer,
                                                        shader_wrap_step(c, old, value, decrement), old);
    spv_emit_store(&c->emit, old_var, observed);
    spv_emit_branch_conditional(&c->emit, spv_emit_binary(&c->emit, SPV_OP_I_EQUAL, c->bool_type, observed, old), merge,
                                cont);
    spv_emit_label(&c->emit, cont);
    spv_emit_branch(&c->emit, header);
    spv_emit_label(&c->emit, merge);
    return spv_emit_load(&c->emit, c->u32_type, old_var);
}

/* A constant buffer word is loaded as an integer: a float load may flush the bits of an integer that look like a
   denormal (Adreno does, in a shader that declares DenormFlushToZero), and a jump table of small offsets reads 0. */
/* element plus bank's window offset in 16-byte units (cb_windows: a 16-bit field of the push block, two banks a word),
   or element where the bank reads no window */
static u32 cbuf_window(DirectCtx* c, u32 bank, u32 element) {
    if (!c->cbw_push || bank >= 18 || !(c->cbw_banks >> bank & 1)) return element;
    const u32 word = bank / 2;
    u32 packed = spv_emit_push_component_f32(&c->emit, c->cbw_pointer, c->cbw_push, shader_u32(c, c->cbw_member + word / 4),
                                             shader_u32(c, word & 3), c->u32_type);
    if (bank & 1) packed = spv_emit_binary(&c->emit, SPV_OP_SHIFT_RIGHT_LOGICAL, c->u32_type, packed, shader_u32(c, 16));
    const u32 offset = spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, packed, shader_u32(c, 0xFFFF));
    return spv_emit_iadd_u32(&c->emit, c->u32_type, element, offset);
}
/* the cache entry of instruction w's constant buffer operand, read in this block if it wasn't */
static u32 cbuf_entry(DirectCtx* c, u64 w) {
    u32 idx = B(w, 34, 5), off = B(w, 20, 14) * 4, key = (idx << 16 | off >> 2) + 1;
    u32 at = (key * 2654435761u) >> 23; /* 9 bits */
    if (c->cbuf_seen[at].key == key && c->cbuf_seen[at].block == c->emit.block) return at;
    u32 folded = 0;
    c->cbuf_seen[at].key = key;
    c->cbuf_seen[at].block = c->emit.block;
    if (g_shader_cb_fold && g_shader_cb_fold(idx, off, &folded)) {
        c->cbuf_seen[at].word = shader_u32(c, folded);
        c->cbuf_seen[at].as_float = 0;
        return at;
    }
    u32 member = shader_u32(c, 0);
    u32 element = cbuf_window(c, idx, shader_u32(c, off >> 4));
    u32 component = shader_u32(c, (off >> 2) & 3);
    u32 word = spv_emit_uniform_component(&c->emit, c->cbuf_pointer[idx], c->cbuf[idx], member, element, component,
                                          c->u32_type);
    if (c->dp_push && idx == 0 && (off == SH_CB0_BASE_VERTEX || off == SH_CB0_BASE_INSTANCE)) {
        /* an indirect draw's base vertex or instance: its record's, which the GPU has, where the push says so */
        int vertex = off == SH_CB0_BASE_VERTEX;
        u32 flags = spv_emit_unary(&c->emit, SPV_OP_BITCAST, c->u32_type,
                                   spv_emit_push_component_f32(&c->emit, c->dp_push_pointer, c->dp_push, c->zero,
                                                               shader_u32(c, 2), c->f32_type));
        u32 bit = shader_u32(c, vertex ? SH_PUSH_GPU_BASE_VERTEX : SH_PUSH_GPU_BASE_INSTANCE);
        u32 on = spv_emit_binary(&c->emit, SPV_OP_I_NOT_EQUAL, c->bool_type,
                                 spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, flags, bit), c->zero);
        u32 gpu = spv_emit_load(&c->emit, c->u32_type, vertex ? c->dp_base_vertex : c->dp_base_instance);
        word = spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->u32_type, on, gpu, word);
    }
    c->cbuf_seen[at].word = word;
    c->cbuf_seen[at].as_float = 0;
    return at;
}
u32 shader_cbuf_word(DirectCtx* c, u64 w) { return c->cbuf_seen[cbuf_entry(c, w)].word; }
u32 shader_cbuf_read(DirectCtx* c, u64 w) {
    u32 at = cbuf_entry(c, w);
    if (!c->cbuf_seen[at].as_float)
        c->cbuf_seen[at].as_float = shader_cast(c, c->f32_type, c->cbuf_seen[at].word, c->u32_type);
    return c->cbuf_seen[at].as_float;
}
u32 shader_cbuf_read_dynamic(DirectCtx* c, u32 bank, u32 address) {
    u32 four = shader_u32(c, 4);
    u32 two = shader_u32(c, 2);
    u32 three = shader_u32(c, 3);
    u32 element = cbuf_window(c, bank, spv_emit_binary(&c->emit, SPV_OP_SHIFT_RIGHT_LOGICAL, c->u32_type, address, four));
    u32 component =
        spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type,
                        spv_emit_binary(&c->emit, SPV_OP_SHIFT_RIGHT_LOGICAL, c->u32_type, address, two), three);
    return spv_emit_uniform_component(&c->emit, c->cbuf_pointer[bank], c->cbuf[bank], c->zero, element, component,
                                      c->u32_type);
}

/* Logical descriptor indexing does not require a device's dynamic-uniform-buffer indexing feature. Only the
   selected case reads its buffer. The default bank returns zero without an out-of-range descriptor access. */
u32 shader_cbuf_read_bank(DirectCtx* c, u32 first, u32 bank, u32 address) {
    u32 merge = spv_emit_id(&c->emit), other = spv_emit_id(&c->emit);
    u32 literals[18], labels[18], incoming[38], count = 18 - first;
    for (u32 i = 0; i < count; i++) {
        literals[i] = first + i;
        labels[i] = spv_emit_id(&c->emit);
    }
    spv_emit_selection_merge(&c->emit, merge);
    spv_emit_switch_u32(&c->emit, bank, other, literals, labels, count);
    for (u32 i = 0; i < count; i++) {
        spv_emit_label(&c->emit, labels[i]);
        incoming[2 * i] = shader_cbuf_read_dynamic(c, first + i, address);
        incoming[2 * i + 1] = labels[i];
        spv_emit_branch(&c->emit, merge);
    }
    spv_emit_label(&c->emit, other);
    spv_emit_branch(&c->emit, merge);
    incoming[2 * count] = c->zero;
    incoming[2 * count + 1] = other;
    spv_emit_label(&c->emit, merge);
    return spv_emit_op(&c->emit, SPV_OP_PHI, c->u32_type, incoming, 2 * (count + 1));
}

static u32 smap_word(DirectCtx* c, u32 index) {
    u32 pointer = spv_emit_storage_access_u32(&c->emit, c->gbuf_pointer, c->smap, c->zero, index);
    return spv_emit_load(&c->emit, c->u32_type, pointer);
}

u32 shader_gbuf_word_index(DirectCtx* c, int stage, u32 slot, u32 word) {
    /* Compute dispatches bind each guest buffer as one contiguous storage buffer.
       The graphics page table descriptor is not present in their descriptor set. */
    if (stage == ST_CS) return word;
    u32 h = smap_word(c, shader_u32(c, slot));
    u32 no_map = spv_emit_binary(&c->emit, SPV_OP_I_EQUAL, c->bool_type, h, shader_u32(c, 0xffffffffu));
    u32 offset = smap_word(c, shader_u32(c, 16 + slot));
    u32 count = smap_word(c, shader_u32(c, 32 + slot));
    u32 valid = spv_emit_binary(&c->emit, SPV_OP_U_LESS_THAN, c->bool_type, word, count);
    u32 mapped_word = spv_emit_iadd_u32(&c->emit, c->u32_type, word, offset);
    u32 page = spv_emit_binary(&c->emit, SPV_OP_SHIFT_RIGHT_LOGICAL, c->u32_type, mapped_word, shader_u32(c, 10));
    u32 safe_h = spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->u32_type, no_map, c->zero, h);
    u32 safe_page = spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->u32_type, no_map, c->zero, page);
    u32 page_ix = spv_emit_iadd_u32(&c->emit, c->u32_type, safe_h, safe_page);
    u32 mapped_page = smap_word(c, page_ix);
    u32 high = spv_emit_binary(&c->emit, SPV_OP_SHIFT_LEFT_LOGICAL, c->u32_type, mapped_page, shader_u32(c, 10));
    u32 low = spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, mapped_word, shader_u32(c, 1023));
    u32 mapped = spv_emit_binary(&c->emit, SPV_OP_BITWISE_OR, c->u32_type, high, low);
    mapped = spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->u32_type, valid, mapped, shader_u32(c, 0xffffffffu));
    return spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->u32_type, no_map, word, mapped);
}

/* a compute program's store or atomic of bytes bytes at byte offset into global buffer slot, recorded in its write
   log (write_log.c): the renderer lands those bytes and no others */
void shader_log_written(DirectCtx* c, int stage, u32 slot, u32 offset, u32 bytes) {
    if (stage != ST_CS || !c->wlog || !c->wlog_mark[slot]) return;
    shader_wlog_store(&c->emit, c->u32_type, c->bool_type, c->wlog, c->wlog_mark[slot] - 1u, offset, bytes);
}

/* the word an atomic of global buffer slot operates on, and its byte offset into the buffer in *offset */
u32 shader_global_atomic_pointer(DirectCtx* c, int stage, u32 slot, u32 base_off, u32 a, u64 w, u32* offset) {
    u32 base = shader_cbuf_read_dynamic(c, SH_GBUF_BANK(base_off), shader_u32(c, SH_GBUF_OFFSET(base_off)));
    u32 immediate = (u32)((i32)(B(w, 28, 20) << 12) >> 12);
    u32 address = spv_emit_iadd_u32(&c->emit, c->u32_type, reg_read(c, a), shader_u32(c, immediate));
    address = spv_emit_binary(&c->emit, SPV_OP_I_SUB, c->u32_type, address, base);
    *offset = address;
    u32 word = spv_emit_binary(&c->emit, SPV_OP_SHIFT_RIGHT_LOGICAL, c->u32_type, address, shader_u32(c, 2));
    return spv_emit_storage_access_u32(&c->emit, c->gbuf_pointer, c->gbuf[slot], c->zero,
                                       shader_gbuf_word_index(c, stage, slot, word));
}

u32 shader_shared_atomic_index(DirectCtx* c, u32 a, u64 w, u32 shared_words) {
    i32 relative = (i32)(B(w, 30, 22) << 10) >> 10;
    u32 immediate = shader_u32(c, (u32)(relative * 4));
    u32 address = a == 255 ? immediate : spv_emit_iadd_u32(&c->emit, c->u32_type, reg_read(c, a), immediate);
    return shader_shared_index(
        c, spv_emit_binary(&c->emit, SPV_OP_SHIFT_RIGHT_LOGICAL, c->u32_type, address, shader_u32(c, 2)), shared_words);
}

/* An LDG or STG of whole words whose base register the tracking lost (gb_at's GB_DYNAMIC): as Eden's SPIR-V backend
   does for such an access (DefineGlobalMemoryFunctions), its 64-bit address is compared with each buffer the
   program binds (cbuf 0's descriptor at gbuf_off, address and size), and the buffer that holds it is accessed.
   Without .E (bit 45) the address is the register zero-extended; the 24-bit offset is signed. A load from an address
   in none of them reads 0, and a store there is dropped, where the console would fault. The buffers' sizes are those
   the renderer binds (vk_gbuf_desc: 16 bytes for 0, at most 16 MiB) */
void shader_global_dynamic(DirectCtx* c, int stage, u64 w, int op, const u32* gbuf_off, int ngbuf) {
    u32 a = B(w, 8, 8), d = B(w, 0, 8), words = B(w, 48, 3) == 5 ? 2 : B(w, 48, 3) >= 6 ? 4 : 1, k;
    /* an absolute address (RZ) has an unsigned offset, as Eden reads it */
    i32 offset = a == 255 ? (i32)B(w, 20, 24) : (i32)(B(w, 20, 24) << 8) >> 8;
    u32 lo = reg_read(c, a), hi = B(w, 45, 1) && a < 255 ? reg_read(c, a + 1) : c->zero;
    u32 address = spv_emit_iadd_u32(&c->emit, c->u32_type, lo, shader_u32(c, (u32)offset));
    /* the offset's carry into the high word, or its borrow */
    u32 wrapped = offset < 0 ? spv_emit_binary(&c->emit, SPV_OP_U_LESS_THAN, c->bool_type, lo, address)
                             : spv_emit_binary(&c->emit, SPV_OP_U_LESS_THAN, c->bool_type, address, lo);
    u32 one = shader_u32(c, 1);
    u32 carry = spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->u32_type, wrapped, one, c->zero);
    u32 data[4];
    hi = offset < 0 ? spv_emit_binary(&c->emit, SPV_OP_I_SUB, c->u32_type, hi, carry)
                    : spv_emit_iadd_u32(&c->emit, c->u32_type, hi, carry);
    for (k = 0; k < words; k++) data[k] = op == O_STG ? reg_read(c, d + k) : c->zero;
    if (op == O_LDG)
        for (k = 0; k < words; k++) reg_write(c, d + k, c->zero);
    for (int slot = 0; slot < ngbuf; slot++) {
        u32 then_label, merge_label, base_lo, base_hi, size, relative, borrow, inside;
        u32 bank = SH_GBUF_BANK(gbuf_off[slot]), at = SH_GBUF_OFFSET(gbuf_off[slot]);
        base_lo = shader_cbuf_read_dynamic(c, bank, shader_u32(c, at));
        base_hi = shader_cbuf_read_dynamic(c, bank, shader_u32(c, at + 4));
        /* a bare address has no size beside it: the cap below, of which the renderer binds what is mapped */
        if (shader_gbuf_sized(stage, gbuf_off[slot])) {
            size = shader_cbuf_read_dynamic(c, 0, shader_u32(c, at + 8));
            size = spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->u32_type,
                                    spv_emit_binary(&c->emit, SPV_OP_I_EQUAL, c->bool_type, size, c->zero),
                                    shader_u32(c, 16), size);
        } else
            size = shader_u32(c, 16u << 20);
        size = spv_emit_ternary(
            &c->emit, SPV_OP_SELECT, c->u32_type,
            spv_emit_binary(&c->emit, SPV_OP_U_LESS_THAN, c->bool_type, shader_u32(c, 16u << 20), size),
            shader_u32(c, 16u << 20), size);
        relative = spv_emit_binary(&c->emit, SPV_OP_I_SUB, c->u32_type, address, base_lo);
        borrow = spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->u32_type,
                                  spv_emit_binary(&c->emit, SPV_OP_U_LESS_THAN, c->bool_type, address, base_lo), one,
                                  c->zero);
        /* inside: the high words' difference, less the low words' borrow, is 0, and the low one is under the size
           (each access's words lie within the binding, as the tracked path's do) */
        inside =
            spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->u32_type,
                             spv_emit_binary(&c->emit, SPV_OP_U_LESS_THAN, c->bool_type, relative, size),
                             spv_emit_binary(&c->emit, SPV_OP_I_SUB, c->u32_type,
                                             spv_emit_binary(&c->emit, SPV_OP_I_SUB, c->u32_type, hi, base_hi), borrow),
                             one);
        inside = spv_emit_binary(&c->emit, SPV_OP_I_EQUAL, c->bool_type, inside,
                                 c->zero); /* before the merge, which precedes the branch */
        then_label = spv_emit_id(&c->emit);
        merge_label = spv_emit_id(&c->emit);
        spv_emit_selection_merge(&c->emit, merge_label);
        spv_emit_branch_conditional(&c->emit, inside, then_label, merge_label);
        spv_emit_label(&c->emit, then_label);
        if (op == O_STG) shader_log_written(c, stage, (u32)slot, relative, words * 4);
        for (k = 0; k < words; k++) {
            u32 word = spv_emit_iadd_u32(
                &c->emit, c->u32_type,
                spv_emit_binary(&c->emit, SPV_OP_SHIFT_RIGHT_LOGICAL, c->u32_type, relative, shader_u32(c, 2)),
                shader_u32(c, k));
            u32 ptr = spv_emit_storage_access_u32(&c->emit, c->gbuf_pointer, c->gbuf[slot], c->zero,
                                                  shader_gbuf_word_index(c, stage, (u32)slot, word));
            if (op == O_STG)
                spv_emit_store(&c->emit, ptr, data[k]);
            else
                reg_write(c, d + k, spv_emit_load(&c->emit, c->u32_type, ptr));
        }
        spv_emit_branch(&c->emit, merge_label);
        spv_emit_label(&c->emit, merge_label);
    }
}

/* A word of shared memory, at its index in the program's own (shader_shared_index's): in the workgroup's array, or where
   it is in a buffer (ShaderInfo.shared_spill), in this block's part of global buffer slot SH_SHARED_SPILL, the block's
   place in the grid (x, then y, then z) times the program's words */
u32 shader_shared_pointer(DirectCtx* c, const Tr* t, u32 index) {
    if (!t->shared_groups)
        return spv_emit_function_element_pointer(&c->emit, t->shared_element_pointer, t->shared_array, index);
    u32 group = spv_emit_load(&c->emit, t->vec3_u32, t->group_input);
    u32 groups = spv_emit_load(&c->emit, t->vec3_u32, t->shared_groups);
    u32 place = spv_emit_vec4_extract(&c->emit, c->u32_type, group, 2);
    for (int k = 1; k >= 0; k--) {
        u32 across = spv_emit_vec4_extract(&c->emit, c->u32_type, groups, (u32)k);
        place = spv_emit_iadd_u32(&c->emit, c->u32_type, spv_emit_vec4_extract(&c->emit, c->u32_type, group, (u32)k),
                                  spv_emit_binary(&c->emit, SPV_OP_I_MUL, c->u32_type, place, across));
    }
    u32 word = spv_emit_iadd_u32(
        &c->emit, c->u32_type,
        spv_emit_binary(&c->emit, SPV_OP_I_MUL, c->u32_type, place, shader_u32(c, t->shared_words)), index);
    return spv_emit_storage_access_u32(&c->emit, t->shared_element_pointer, t->shared_array, c->zero, word);
}
