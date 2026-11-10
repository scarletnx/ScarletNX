/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A compute program's write log, which its stores and atomics to global buffers record their bytes in (shader.h's
   SH_WLOG_*). The renderer lands the bytes logged and no others (vk/compute.c, vk/writelog.c, vk/mirror.c): a buffer
   found changed by comparing it with a copy taken before the dispatch costs the copy and the compare of its whole
   binding, up to 16 MiB, for each dispatch. */
#include "nx/gpu/shader/write_log.h"

u32 shader_wlog_part(ShaderInfo* si, int slot) {
    if (!si->gbuf_mark[slot]) si->gbuf_mark[slot] = ++si->nmarks;
    si->gbuf_marks |= 1u << slot;
    return (u32)(si->gbuf_mark[slot] - 1);
}

u32 shader_wlog_buffer(SpvEmitter* e, u32 u32_type, u32 set) {
    u32 array = spv_emit_type_runtime_array(e, u32_type, 4);
    u32 structure = spv_emit_type_struct_one(e, array);
    return spv_emit_storage_buffer(e, structure, set, SNX_VKB_WPAGES);
}

void shader_wlog_store(SpvEmitter* e, u32 u32_type, u32 bool_type, u32 log, u32 part, u32 offset, u32 bytes) {
    u32 base = part * SH_WLOG_PART, zero = spv_emit_constant_u32(e, u32_type, 0);
    u32 ptr_type = spv_emit_type_storage_pointer(e, u32_type);
    u32 at = spv_emit_binary(e, SPV_OP_BITWISE_AND, u32_type, offset, spv_emit_constant_u32(e, u32_type, ~(bytes - 1)));
    u32 window = spv_emit_binary(e, SPV_OP_SHIFT_RIGHT_LOGICAL, u32_type, at, spv_emit_constant_u32(e, u32_type, 12));
    u32 last = spv_emit_constant_u32(e, u32_type, SH_WPAGE_SLOT - 1);
    u32 past = spv_emit_binary(e, SPV_OP_U_LESS_THAN, bool_type, last, window);
    window = spv_emit_ternary(e, SPV_OP_SELECT, u32_type, past, last, window);
    u32 flag = spv_emit_iadd_u32(e, u32_type, window, spv_emit_constant_u32(e, u32_type, base));
    spv_emit_store(e, spv_emit_storage_access_u32(e, ptr_type, log, zero, flag), spv_emit_constant_u32(e, u32_type, 1));
    /* the mask word: window * SH_WLOG_MASK + (at & 4095) / 32, after the part's flags */
    u32 masks = spv_emit_binary(e, SPV_OP_I_MUL, u32_type, window, spv_emit_constant_u32(e, u32_type, SH_WLOG_MASK));
    u32 in_window = spv_emit_binary(e, SPV_OP_BITWISE_AND, u32_type, at, spv_emit_constant_u32(e, u32_type, 4095));
    in_window =
        spv_emit_binary(e, SPV_OP_SHIFT_RIGHT_LOGICAL, u32_type, in_window, spv_emit_constant_u32(e, u32_type, 5));
    u32 word = spv_emit_iadd_u32(e, u32_type, spv_emit_iadd_u32(e, u32_type, masks, in_window),
                                 spv_emit_constant_u32(e, u32_type, base + SH_WPAGE_SLOT));
    u32 bits = spv_emit_binary(
        e, SPV_OP_SHIFT_LEFT_LOGICAL, u32_type, spv_emit_constant_u32(e, u32_type, (u32)((1ull << bytes) - 1)),
        spv_emit_binary(e, SPV_OP_BITWISE_AND, u32_type, at, spv_emit_constant_u32(e, u32_type, 31)));
    bits = spv_emit_ternary(e, SPV_OP_SELECT, u32_type, past, zero, bits);
    spv_emit_atomic_u32(e, u32_type, SPV_OP_ATOMIC_OR, spv_emit_storage_access_u32(e, ptr_type, log, zero, word), bits);
}
