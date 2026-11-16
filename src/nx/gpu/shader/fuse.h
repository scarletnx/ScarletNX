/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_FUSE_H
#define SNX_FUSE_H
#include "nx/gpu/shader/sm86.h"

/* The pairs of neighbouring instructions that one operation takes the place of, made before the translation lowers
   an instruction into SPIR-V. A pair is a span of the instruction array (its first instruction and its length) with
   a tag that says what the pair is, in one flat array, not a chain or a tree: the translation finds the spans, uses
   them and is done with them.

   The spans are found and used by one pass (sm86_fuse), which runs after the CALLs are inlined and before the
   passes that move instructions (the globals tracked, the blocks placed). Those passes invalidate a span, so the
   list is made and used in that one pass, and nothing holds it across a pass. */
typedef struct {
    u32 pos, len; /* the pair's first instruction in the array, and its length (two) */
    u32 tag;      /* SM86_FUSE_*: what the pair is */
} FuseSpan;
enum { SM86_FUSE_NONE, SM86_FUSE_LDSM_HMMA };

/* the pairs in an array of n SM86 instructions: the spans as one array, or NULL where there is none. The array is
   read only, so it is safe before any pass of the translation */
FuseSpan* sm86_fuse_find(const Sm86Insn* in, u32 n, u32* count);
/* the spans made into fused instructions in the array, the pair's two instructions one SM86_LDSM_HMMA whose fields
   are the second half's: the array's new count comes back in n. The spans are of no use after this, and are freed */
void sm86_fuse_make(Sm86Insn* in, u32* n, FuseSpan* spans, u32 count);
#endif
