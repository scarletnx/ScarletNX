/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A CPU reference for SM86 compute programs (tests/support/sm86_ref.c), and a small encoder that makes their words
   by the rules of Mesa's NAK encoder (sm70_encode.rs). The reference runs each invocation alone, one after the other,
   with the semantics of NAK's constant folding (ir.rs) and the hardware's rounding modes, which the C library's
   fesetround gives. It takes the instructions of the captured compute programs, but no BAR, SHFL, texture or surface
   instruction, which need the invocations to run together. */
#ifndef SNX_TESTS_SM86_REF_H
#define SNX_TESTS_SM86_REF_H
#include "nx/gpu/shader/sm86.h"

/* the memory a program sees: constant buffers by bank, and global buffers at a GPU address each, which a constant
   buffer holds for ULDC.64 */
typedef struct {
    const u32* cbuf[18];
    u32 cbuf_words[18];
    u8* global[4];
    u64 global_va[4];
    u32 global_bytes[4];
    u32 local[3], groups[3];
    u32 shared_bytes;
} Sm86RefEnv;
/* Runs every invocation of the dispatch: NULL when it ran, or why it could not */
const char* sm86_ref_run(const u8* code, u32 bytes, Sm86RefEnv* env);

/* ---- the encoder ---- */
typedef struct {
    u64 w[2];
} Sm86Word;
void sm86_set(Sm86Word* x, unsigned lo, unsigned n, u64 v);
/* an instruction with its 12-bit opcode, on PT, without barriers */
Sm86Word sm86_fix(u32 opcode);
/* an ALU instruction: the 9-bit opcode, its form, the destination and source a */
Sm86Word sm86_alu(u32 opcode9, u32 form, u32 dst, u32 a);
/* a constant buffer operand in bits 32-58 */
void sm86_cbuf(Sm86Word* x, u32 bank, u32 off);
Sm86Word sm86_bra(u32 ip, u32 target, u32 pred, int negate);
Sm86Word sm86_exit(void);
void sm86_put(u8* code, u32 index, Sm86Word x);
#endif
