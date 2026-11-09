/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A compute program's write log (shader.h's SNX_VKB_WPAGES, SH_WLOG_*), as both translators write it: the Maxwell one
   (spirv_translate.c, emit_memory.c, spirv_compute.c) and the SM86 one (sm86_translate.c). */
#ifndef SNX_SHADER_WRITE_LOG_H
#define SNX_SHADER_WRITE_LOG_H
#include "nx/gpu/shader/shader.h"
#include "common/utilities/spirv_emit.h"

/* the log's part of global buffer slot (ShaderInfo.gbuf_mark): the next part when the buffer has none yet */
u32 shader_wlog_part(ShaderInfo* si, int slot);
/* the log's storage buffer, a runtime array of words at SNX_VKB_WPAGES of descriptor set set */
u32 shader_wlog_buffer(SpvEmitter* e, u32 u32_type, u32 set);
/* The record of a store or atomic of bytes bytes (1, 2, 4, 8 or 16) at byte offset (a u32 value) into a global
   buffer, in part part of the log buffer log: a plain store of 1 into its window's flag, as every store of a window
   writes the same, and an atomic OR of its bytes' bits into that window's mask, which other invocations OR into too.
   The offset is taken as aligned to the size, as the GPU requires of every global access, so its bits are in one mask
   word. A store past the windows sets the last window's flag and no bit */
void shader_wlog_store(SpvEmitter* e, u32 u32_type, u32 bool_type, u32 log, u32 part, u32 offset, u32 bytes);
#endif
