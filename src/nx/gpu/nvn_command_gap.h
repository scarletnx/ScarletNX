/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What a span of Maxwell methods writes, checked against packets captured from Tears of the Kingdom
   (tests/nvn_command_gap.c). A "gap" is the words an NVN command buffer holds between two calls the investigation
   observed (docs/NVN_HLE.md): inlined NVN writes them. "The caller must stage" means a user that wants to replay the
   span instead of decoding it has to apply those writes itself. Only the tests build this today */
#ifndef SNX_NVN_COMMAND_GAP_H
#define SNX_NVN_COMMAND_GAP_H
#include "common/cpu/aarch64/cpu.h"

typedef struct {
    u64 index_address;
    u32 primitive, index_type, count;
} NvnGapIndexedDraw;
typedef struct {
    u32 macro_argument;
    u32 words[3];
} NvnGapMacroData3;
typedef struct {
    u32 puller_high, puller_low, puller_payload, query_high, query_low, query_sequence;
    u32 cbuf_high, cbuf_low, cbuf_offset;
    u8 puller_known, query_known, cbuf_known;
} NvnGapMemoryState;
typedef struct {
    u64 address;
    u32 size, value;
    u8 kind, value_known, fake_report, fake_known;
} NvnGapMemoryWrite;
enum { NVN_GAP_WRITE_PULLER, NVN_GAP_WRITE_QUERY, NVN_GAP_WRITE_CBUF, NVN_GAP_WRITE_UNIFORM };

typedef void (*NvnGapMethodVisitor)(u32 subchannel, u32 method, u32 value, void* context);

/* Visit only complete, well-formed cursor gaps. No callbacks run if validation fails. */
int nvn_command_gap_methods(const u32* words, u32 count, NvnGapMethodVisitor visit, void* context);

/* True only for validated register writes with no direct guest-memory or attachment write effect. */
int nvn_command_gap_no_writes(const u32* words, u32 count);

/* Extract bounded guest-memory writes in packet order. The caller retains state across adjacent cursor gaps.
   Returns zero if the packets, address state, or output capacity are incomplete; on failure state is unchanged. */
int nvn_command_gap_memory_writes(const u32* words, u32 count, NvnGapMemoryState* state, NvnGapMemoryWrite* writes,
                                  u32 capacity, u32* write_count);

/* Recognize one complete indexed draw packet at the end of a command-buffer cursor gap. */
int nvn_command_gap_indexed_draw(const u32* words, u32 count, NvnGapIndexedDraw* out);

/* True when the only attachment write in a complete gap is its recognized final indexed draw.
   The caller must stage that draw separately before treating the gap as modeled. */
int nvn_command_gap_modeled_indexed_draw(const u32* words, u32 count);

/* True when all effects are read-only state, bounded memory writes, and optionally
   one recognized final indexed draw. The caller must successfully extract and
   stage the memory writes, and stage the draw when indexed_draw_staged is set. */
int nvn_command_gap_staged_writes(const u32* words, u32 count, int indexed_draw_staged);

/* Locate one syntactically complete MME call followed immediately by a three-word constant-buffer upload.
   The caller must separately prove what the uploaded macro does to the constant-buffer offset. */
int nvn_command_gap_macro_data3(const u32* words, u32 count, u32 macro_method, NvnGapMacroData3* out);
#endif
