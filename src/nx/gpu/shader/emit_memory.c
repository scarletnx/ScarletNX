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
/* the cache entry of instruction w's constant buffer operand, read in this block if it wasn't */
static u32 cbuf_entry(DirectCtx* c, u64 w) {
    u32 idx = B(w, 34, 5), off = B(w, 20, 14) * 4, key = (idx << 16 | off >> 2) + 1;
    u32 at = (key * 2654435761u) >> 23; /* 9 bits */
    if (c->cbuf_seen[at].key == key && c->cbuf_seen[at].block == c->emit.block) return at;
    u32 member = shader_u32(c, 0);
    u32 element = shader_u32(c, off >> 4);
    u32 component = shader_u32(c, (off >> 2) & 3);
    c->cbuf_seen[at].key = key;
    c->cbuf_seen[at].block = c->emit.block;
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
    u32 element = spv_emit_binary(&c->emit, SPV_OP_SHIFT_RIGHT_LOGICAL, c->u32_type, address, four);
    u32 component =
        spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type,
                        spv_emit_binary(&c->emit, SPV_OP_SHIFT_RIGHT_LOGICAL, c->u32_type, address, two), three);
    return spv_emit_uniform_component(&c->emit, c->cbuf_pointer[bank], c->cbuf[bank], c->zero, element, component,
                                      c->u32_type);
}

/* Logical descriptor indexing does not require a device's dynamic-uniform-buffer indexing feature. Only the
   selected case reads its buffer. The default bank returns zero without an out-of-range descriptor access. */
static u32 cbuf_read_bank(DirectCtx* c, u32 first, u32 bank, u32 address) {
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
    return spv_emit_op(&c->emit, 245, c->u32_type, incoming, 2 * (count + 1)); /* OpPhi */
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
static void log_written(DirectCtx* c, int stage, u32 slot, u32 offset, u32 bytes) {
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
        if (gbuf_off[slot] == 0xb0) continue; /* the vertex stage's bare pointer, a buffer of its own */
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
        if (op == O_STG) log_written(c, stage, (u32)slot, relative, words * 4);
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

/* the constant buffer, local, shared and global memory instructions */
void emit_memory(const Tr* t, u32 i, u64 w, int op, u32 d, u32 a, u32 b) {
    int stage = t->stage;
    ShTex* textures = t->textures;
    int texture_count = t->texture_count;
    int k_indexed;
    u8* gb_at = t->gb_at;
    int ngbuf = t->ngbuf;
    u32* gbuf_off = t->gbuf_off;
    DirectCtx* c = t->c;
    u32 i32_type = t->i32_type;
    u32 shared_words = t->shared_words;
    u32 shared_element_pointer = t->shared_element_pointer;
    u32 shared_array = t->shared_array;
    u32 local_words = t->local_words;
    u32 local_element_pointer = t->local_element_pointer;
    u32 local_array = t->local_array;
    u32 atomic_old_var = t->atomic_old_var;
    if (op == O_LDC) {
        u32 off = shader_u32(c, (u32)(i32)(i16)B(w, 20, 16));
        u32 source = reg_read(c, a), dynamic_bank = 0;
        u32 bank = B(w, 36, 5);
        if (B(w, 44, 1)) {
            dynamic_bank = spv_emit_iadd_u32(
                &c->emit, c->u32_type, shader_u32(c, bank),
                spv_emit_binary(&c->emit, SPV_OP_SHIFT_RIGHT_LOGICAL, c->u32_type, source, shader_u32(c, 16)));
            source = spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, source, shader_u32(c, 0xffff));
        }
        u32 address = spv_emit_iadd_u32(&c->emit, c->u32_type, source, off);
        u32 value = dynamic_bank ? cbuf_read_bank(c, bank, dynamic_bank, address)
                                  : shader_cbuf_read_dynamic(c, bank, address);
        u32 size = B(w, 48, 3);
        /* the element an indexed texture whose handle this loads picks: the address register over the stride */
        for (k_indexed = 0; k_indexed < texture_count; k_indexed++)
            if ((textures[k_indexed].flags & SNX_TEX_INDEXED) && textures[k_indexed].bl2_off == i + 1)
                spv_emit_store(&c->emit, c->tex_index[k_indexed],
                               spv_emit_binary(&c->emit, SPV_OP_SHIFT_RIGHT_LOGICAL, c->u32_type, reg_read(c, a),
                                               shader_u32(c, textures[k_indexed].flags >> 4)));
        if (size < 4) {
            u32 byte_offset =
                spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, address, shader_u32(c, size < 2 ? 3 : 2));
            u32 bit_offset =
                spv_emit_binary(&c->emit, SPV_OP_SHIFT_LEFT_LOGICAL, c->u32_type, byte_offset, shader_u32(c, 3));
            u32 width = shader_u32(c, size < 2 ? 8 : 16);
            value = size & 1
                        ? spv_emit_unary(&c->emit, SPV_OP_BITCAST, c->u32_type,
                                         spv_emit_ternary(&c->emit, SPV_OP_BIT_FIELD_S_EXTRACT, i32_type,
                                                          spv_emit_unary(&c->emit, SPV_OP_BITCAST, i32_type, value),
                                                          bit_offset, width))
                        : spv_emit_ternary(&c->emit, SPV_OP_BIT_FIELD_U_EXTRACT, c->u32_type, value, bit_offset, width);
        }
        reg_write(c, d, value);
        if (size == 5) {
            u32 next = spv_emit_iadd_u32(&c->emit, c->u32_type, address, shader_u32(c, 4));
            reg_write(c, d + 1, dynamic_bank ? cbuf_read_bank(c, bank, dynamic_bank, next)
                                            : shader_cbuf_read_dynamic(c, bank, next));
        }
    } else if (op == O_ATOMS) {
        u32 index = shader_shared_atomic_index(c, a, w, shared_words);
        u32 pointer = spv_emit_function_element_pointer(&c->emit, shared_element_pointer, shared_array, index);
        u32 operation = shader_shared_atomic_opcode(w);
        if (operation == 0xfffffffeu) {
            u32 next_index =
                shader_shared_index(c, spv_emit_iadd_u32(&c->emit, c->u32_type, index, shader_u32(c, 1)), shared_words);
            u32 high = spv_emit_function_element_pointer(&c->emit, shared_element_pointer, shared_array, next_index);
            u32 old_low = spv_emit_load(&c->emit, c->u32_type, pointer);
            u32 old_high = spv_emit_load(&c->emit, c->u32_type, high);
            spv_emit_store(&c->emit, pointer, reg_read(c, b));
            spv_emit_store(&c->emit, high, reg_read(c, b + 1));
            reg_write(c, d, old_low);
            reg_write(c, d + 1, old_high);
        } else if (operation != 0xffffffffu) {
            reg_write(c, d, spv_emit_atomic_shared_u32(&c->emit, c->u32_type, operation, pointer, reg_read(c, b)));
        } else {
            u32 header = spv_emit_id(&c->emit), body = spv_emit_id(&c->emit);
            u32 cont = spv_emit_id(&c->emit), merge = spv_emit_id(&c->emit);
            spv_emit_store(&c->emit, atomic_old_var, spv_emit_atomic_load_shared_u32(&c->emit, c->u32_type, pointer));
            spv_emit_branch(&c->emit, header);
            spv_emit_label(&c->emit, header);
            spv_emit_loop_merge(&c->emit, merge, cont);
            spv_emit_branch(&c->emit, body);
            spv_emit_label(&c->emit, body);
            u32 old = spv_emit_load(&c->emit, c->u32_type, atomic_old_var);
            u32 next = shader_wrap_step(c, old, reg_read(c, b), B(w, 52, 4) != 3);
            u32 observed = spv_emit_atomic_compare_exchange_shared_u32(&c->emit, c->u32_type, pointer, next, old);
            spv_emit_store(&c->emit, atomic_old_var, observed);
            spv_emit_branch_conditional(
                &c->emit, spv_emit_binary(&c->emit, SPV_OP_I_EQUAL, c->bool_type, observed, old), merge, cont);
            spv_emit_label(&c->emit, cont);
            spv_emit_branch(&c->emit, header);
            spv_emit_label(&c->emit, merge);
            reg_write(c, d, spv_emit_load(&c->emit, c->u32_type, atomic_old_var));
        }
    } else if (op == O_LDS || op == O_STS) {
        u32 immediate = shader_u32(c, (u32)((i32)(B(w, 20, 24) << 8) >> 8));
        u32 address = a == 255 ? immediate : spv_emit_iadd_u32(&c->emit, c->u32_type, reg_read(c, a), immediate);
        u32 first = spv_emit_binary(&c->emit, SPV_OP_SHIFT_RIGHT_LOGICAL, c->u32_type, address, shader_u32(c, 2));
        u32 words = B(w, 48, 3) == 5 ? 2 : B(w, 48, 3) >= 6 ? 4 : 1;
        u32 size = B(w, 48, 3);
        for (u32 k = 0; k < words; k++) {
            u32 index = k ? spv_emit_iadd_u32(&c->emit, c->u32_type, first, shader_u32(c, k)) : first;
            index = shader_shared_index(c, index, shared_words);
            u32 pointer = spv_emit_function_element_pointer(&c->emit, shared_element_pointer, shared_array, index);
            if (size < 4) {
                u32 bit_offset = spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, address,
                                                 shader_u32(c, size < 2 ? 3 : 2));
                bit_offset =
                    spv_emit_binary(&c->emit, SPV_OP_SHIFT_LEFT_LOGICAL, c->u32_type, bit_offset, shader_u32(c, 3));
                u32 width = shader_u32(c, size < 2 ? 8 : 16);
                if (op == O_STS) {
                    u32 header = spv_emit_id(&c->emit), body = spv_emit_id(&c->emit);
                    u32 retry = spv_emit_id(&c->emit), done = spv_emit_id(&c->emit);
                    spv_emit_branch(&c->emit, header);
                    spv_emit_label(&c->emit, header);
                    spv_emit_loop_merge(&c->emit, done, retry);
                    spv_emit_branch(&c->emit, body);
                    spv_emit_label(&c->emit, body);
                    u32 old = spv_emit_atomic_load_shared_u32(&c->emit, c->u32_type, pointer);
                    u32 desired =
                        spv_emit_bitfield_insert(&c->emit, c->u32_type, old, reg_read(c, d), bit_offset, width);
                    u32 actual =
                        spv_emit_atomic_compare_exchange_shared_u32(&c->emit, c->u32_type, pointer, desired, old);
                    u32 success = spv_emit_binary(&c->emit, SPV_OP_I_EQUAL, c->bool_type, actual, old);
                    spv_emit_branch_conditional(&c->emit, success, done, retry);
                    spv_emit_label(&c->emit, retry);
                    spv_emit_branch(&c->emit, header);
                    spv_emit_label(&c->emit, done);
                } else {
                    u32 old = spv_emit_atomic_load_shared_u32(&c->emit, c->u32_type, pointer);
                    u32 value =
                        size & 1
                            ? spv_emit_unary(&c->emit, SPV_OP_BITCAST, c->u32_type,
                                             spv_emit_ternary(&c->emit, SPV_OP_BIT_FIELD_S_EXTRACT, i32_type,
                                                              spv_emit_unary(&c->emit, SPV_OP_BITCAST, i32_type, old),
                                                              bit_offset, width))
                            : spv_emit_ternary(&c->emit, SPV_OP_BIT_FIELD_U_EXTRACT, c->u32_type, old, bit_offset,
                                               width);
                    reg_write(c, d, value);
                }
            } else if (op == O_STS)
                spv_emit_store(&c->emit, pointer, reg_read(c, d + k));
            else
                reg_write(c, d + k, spv_emit_load(&c->emit, c->u32_type, pointer));
        }
    } else if (op == O_LDL || op == O_STL) {
        u32 immediate = shader_u32(c, (u32)((i32)(B(w, 20, 24) << 8) >> 8));
        u32 address = a == 255 ? immediate : spv_emit_iadd_u32(&c->emit, c->u32_type, reg_read(c, a), immediate);
        u32 first = spv_emit_binary(&c->emit, SPV_OP_SHIFT_RIGHT_LOGICAL, c->u32_type, address, shader_u32(c, 2));
        u32 words = B(w, 48, 3) == 5 ? 2 : B(w, 48, 3) >= 6 ? 4 : 1;
        u32 size = B(w, 48, 3);
        for (u32 k = 0; k < words; k++) {
            u32 index = k ? spv_emit_iadd_u32(&c->emit, c->u32_type, first, shader_u32(c, k)) : first;
            index = spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, index, shader_u32(c, local_words - 1));
            u32 pointer = spv_emit_function_element_pointer(&c->emit, local_element_pointer, local_array, index);
            if (size < 4) {
                u32 bit_offset = spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, address,
                                                 shader_u32(c, size < 2 ? 3 : 2));
                bit_offset =
                    spv_emit_binary(&c->emit, SPV_OP_SHIFT_LEFT_LOGICAL, c->u32_type, bit_offset, shader_u32(c, 3));
                u32 width = shader_u32(c, size < 2 ? 8 : 16);
                u32 old = spv_emit_load(&c->emit, c->u32_type, pointer);
                if (op == O_STL)
                    spv_emit_store(
                        &c->emit, pointer,
                        spv_emit_bitfield_insert(&c->emit, c->u32_type, old, reg_read(c, d), bit_offset, width));
                else {
                    u32 value =
                        size & 1
                            ? spv_emit_unary(&c->emit, SPV_OP_BITCAST, c->u32_type,
                                             spv_emit_ternary(&c->emit, SPV_OP_BIT_FIELD_S_EXTRACT, i32_type,
                                                              spv_emit_unary(&c->emit, SPV_OP_BITCAST, i32_type, old),
                                                              bit_offset, width))
                            : spv_emit_ternary(&c->emit, SPV_OP_BIT_FIELD_U_EXTRACT, c->u32_type, old, bit_offset,
                                               width);
                    reg_write(c, d, value);
                }
            } else if (op == O_STL)
                spv_emit_store(&c->emit, pointer, reg_read(c, d + k));
            else
                reg_write(c, d + k, spv_emit_load(&c->emit, c->u32_type, pointer));
        }
    } else if (op == O_STG && gb_at[i] == 255) {
        /* SR_DIRECTCBEWRITEENABLED is zero: use the shader's ordinary attribute-export path. */
    } else if ((op == O_LDG || op == O_STG) && gb_at[i] == GB_DYNAMIC) {
        shader_global_dynamic(c, stage, w, op, gbuf_off, ngbuf);
    } else if (op == O_LDG || op == O_STG) {
        u32 slot = gb_at[i] - 1;
        u32 offset = shader_u32(c, SH_GBUF_OFFSET(gbuf_off[slot]));
        u32 base = shader_cbuf_read_dynamic(c, SH_GBUF_BANK(gbuf_off[slot]), offset);
        u32 immediate = shader_u32(c, (u32)((i32)(B(w, 20, 24) << 8) >> 8));
        u32 address = spv_emit_iadd_u32(&c->emit, c->u32_type, reg_read(c, a), immediate);
        address = spv_emit_binary(&c->emit, SPV_OP_I_SUB, c->u32_type, address, base);
        u32 index = spv_emit_binary(&c->emit, SPV_OP_SHIFT_RIGHT_LOGICAL, c->u32_type, address, shader_u32(c, 2));
        u32 words = B(w, 48, 3) == 5 ? 2 : B(w, 48, 3) >= 6 ? 4 : 1;
        u32 size = B(w, 48, 3);
        if (op == O_STG) log_written(c, stage, slot, address, size < 2 ? 1 : size < 4 ? 2 : words * 4);
        for (u32 k = 0; k < words; k++) {
            u32 word = k ? spv_emit_iadd_u32(&c->emit, c->u32_type, index, shader_u32(c, k)) : index;
            word = shader_gbuf_word_index(c, stage, slot, word);
            u32 ptr = spv_emit_storage_access_u32(&c->emit, c->gbuf_pointer, c->gbuf[slot], c->zero, word);
            if (size < 4) {
                u32 byte_offset = spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, address,
                                                  shader_u32(c, size < 2 ? 3 : 2));
                u32 bit_offset =
                    spv_emit_binary(&c->emit, SPV_OP_SHIFT_LEFT_LOGICAL, c->u32_type, byte_offset, shader_u32(c, 3));
                u32 width = shader_u32(c, size < 2 ? 8 : 16);
                if (op == O_STG) {
                    u32 header = spv_emit_id(&c->emit), body = spv_emit_id(&c->emit);
                    u32 retry = spv_emit_id(&c->emit), done = spv_emit_id(&c->emit);
                    spv_emit_branch(&c->emit, header);
                    spv_emit_label(&c->emit, header);
                    spv_emit_loop_merge(&c->emit, done, retry);
                    spv_emit_branch(&c->emit, body);
                    spv_emit_label(&c->emit, body);
                    u32 old = spv_emit_atomic_load_u32(&c->emit, c->u32_type, ptr);
                    u32 desired =
                        spv_emit_bitfield_insert(&c->emit, c->u32_type, old, reg_read(c, d), bit_offset, width);
                    u32 actual = spv_emit_atomic_compare_exchange_u32(&c->emit, c->u32_type, ptr, desired, old);
                    u32 success = spv_emit_binary(&c->emit, SPV_OP_I_EQUAL, c->bool_type, actual, old);
                    spv_emit_branch_conditional(&c->emit, success, done, retry);
                    spv_emit_label(&c->emit, retry);
                    spv_emit_branch(&c->emit, header);
                    spv_emit_label(&c->emit, done);
                } else {
                    u32 old = spv_emit_atomic_load_u32(&c->emit, c->u32_type, ptr);
                    u32 value =
                        size & 1
                            ? spv_emit_unary(&c->emit, SPV_OP_BITCAST, c->u32_type,
                                             spv_emit_ternary(&c->emit, SPV_OP_BIT_FIELD_S_EXTRACT, i32_type,
                                                              spv_emit_unary(&c->emit, SPV_OP_BITCAST, i32_type, old),
                                                              bit_offset, width))
                            : spv_emit_ternary(&c->emit, SPV_OP_BIT_FIELD_U_EXTRACT, c->u32_type, old, bit_offset,
                                               width);
                    reg_write(c, d, value);
                }
            } else if (op == O_STG)
                spv_emit_store(&c->emit, ptr, reg_read(c, d + k));
            else
                reg_write(c, d + k, spv_emit_load(&c->emit, c->u32_type, ptr));
        }
    } else if (op == O_ATOM_CAS) {
        u32 slot = gb_at[i] - 1;
        u32 offset;
        u32 pointer = shader_global_atomic_pointer(c, stage, slot, gbuf_off[slot], a, w, &offset);
        log_written(c, stage, slot, offset, 4);
        reg_write(
            c, d,
            spv_emit_atomic_compare_exchange_u32(&c->emit, c->u32_type, pointer, reg_read(c, b + 1), reg_read(c, b)));
    } else if (op == O_ATOMS_CAS) {
        u32 index = shader_shared_atomic_index(c, a, w, shared_words);
        u32 pointer = spv_emit_function_element_pointer(&c->emit, shared_element_pointer, shared_array, index);
        reg_write(c, d,
                  spv_emit_atomic_compare_exchange_shared_u32(&c->emit, c->u32_type, pointer, reg_read(c, b + 1),
                                                              reg_read(c, b)));
    } else if (op == O_RED || op == O_ATOM) {
        u32 slot = gb_at[i] - 1;
        u32 offset;
        u32 pointer = shader_global_atomic_pointer(c, stage, slot, gbuf_off[slot], a, w, &offset);
        log_written(c, stage, slot, offset, 4); /* each one 32 bits (shader_global_atomic_opcode) */
        u32 value = reg_read(c, op == O_RED ? d : b);
        u32 atomic_opcode = shader_global_atomic_opcode(w, op == O_RED);
        u32 prior;
        if (atomic_opcode != 0xffffffffu)
            prior = spv_emit_atomic_u32(&c->emit, c->u32_type, atomic_opcode, pointer, value);
        else {
            u32 operation = op == O_RED ? B(w, 23, 3) : B(w, 52, 4);
            u32 size = op == O_RED ? B(w, 20, 3) : B(w, 49, 3);
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
                u32 sum = spv_emit_binary(&c->emit, SPV_OP_F_ADD, c->f32_type,
                                          spv_emit_unary(&c->emit, SPV_OP_BITCAST, c->f32_type, old),
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
            prior = spv_emit_load(&c->emit, c->u32_type, atomic_old_var);
        }
        if (op == O_ATOM) reg_write(c, d, prior);
    }
}
