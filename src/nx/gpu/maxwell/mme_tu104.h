/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Instruction format and semantics from Mesa a57603e485bd, src/nouveau/mme/mme_tu104.h and mme_tu104_sim.c:
   Copyright © 2022 Collabora Ltd. SPDX-License-Identifier: MIT */
#ifndef SNX_MME_TU104_H
#define SNX_MME_TU104_H
#include "common/cpu/aarch64/cpu.h"

#define TU104_INSTRUCTIONS 0x1000u
#define TU104_DRAM_WORDS   0xC00u
#define TU104_FIFO_WORDS   1024u
/* An instruction's fields, decoded when its words are uploaded. A zeroed entry is the decode of zero words */
typedef struct {
    u8 op[2], dst[2], a[2], b[2], out_method[2], out_emit[2];
    u16 imm[2];
    u8 pred, mode, end_next;
    u8 load_mask; /* the parameters it reads: bit 0 into load[0], bit 1 into load[1] */
    u8 off[2];    /* the lanes (bits 0 and 1 the ALUs, 2 and 3 the outputs) its predicate turns off when false, true */
} MmeTu104Ins;
/* Instruction addresses count 96-bit instructions, but uploads deliver individual 32-bit words. */
typedef struct {
    u32 code[TU104_INSTRUCTIONS * 3], start[0x80];
    u32 upload, bind, words;
    MmeTu104Ins ins[TU104_INSTRUCTIONS]; /* code decoded */
} MmeTu104Program;
/* One allocation per TSG, shared by its channels. Compute runs the graphics program and start table until a compute
   0x45 or 0x47 sets compute, as Eglantine's KeplerCompute does. */
typedef struct {
    u64 tsg;
    u32 refs, closed; /* GPU-thread channel references, reconstructed on state load, and the TSG close notice */
    u32 compute;      /* compute uploads and binds go to program[1], and compute calls run it */
    MmeTu104Program program[2];
} MmeTu104Group;
/* Per-channel execution and data RAM. Graphics and compute share the data RAM. The scheduler uses GpuMme.running for
   both instruction sets. */
typedef struct {
    u32 group;           /* 1 + g_gpu.tu104[] index, 0 while detached */
    u32 engine, program; /* last call's engine (1 graphics, 2 compute) and selected shared program */
    u32 overflow;        /* discard the rest of an oversized parameter packet */
    u32 regs[24], initialized;
    u32 dram[TU104_DRAM_WORDS];
    u32 fifo[TU104_FIFO_WORDS], fifo_count, fifo_next, fifo_dma[2];
    u32 alu[2], load[2], imm[2], out_method[2], out_emit[2];
    u32 mask, output, next_pc, stop, end_next;
    u32 loop_count, loop_start, loop_end;
} MmeTu104;

struct GpuChannel;
/* Native-only methods: uploads, macro calls, and MME DMA. 1 if handled, 0 for an ordinary engine method. */
int mme_tu104_method(struct GpuChannel* ch, u32 engine, u32 method, u32 value, int last);
void mme_tu104_resume(struct GpuChannel* ch);
/* GPU thread only: change the TSG identity before a job, and release a channel's macro storage. */
void mme_tu104_set_group(struct GpuChannel* ch, u64 tsg);
void mme_tu104_tsg_close(u64 tsg);
/* Grow a program-owner table on the GPU thread or while parsing a state. New slots are NULL. */
int mme_tu104_groups_grow(MmeTu104Group*** groups, u32* capacity);
void mme_tu104_free(struct GpuChannel* ch);
#endif
