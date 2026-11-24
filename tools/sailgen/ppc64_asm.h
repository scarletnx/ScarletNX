/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's powerpc64 back end, its frame (after mach.h, before risc_asm.h): a step's or a block's prologue and
   epilogue, ppc64.h's function frame, its size patched in once the code is done */

static inline void sgx_prologue(SgXAsm* a) {
    size_t at = rk_fn_prologue(a->b, 0);
    if (a->nframe < 256) a->frame_at[a->nframe++] = at;
}
static inline void sgx_epilogue(SgXAsm* a) { rk_fn_epilogue(a->b, 0); }
static inline void sgx_patch_frame(SgXAsm* a) {
    int k;
    for (k = 0; k < a->nframe; ++k)
        if (!rk_fn_patch_frame(a->b, a->frame_at[k], (long long)a->slots_max * 8)) sg_fail("a frame past 32 KiB", a->slots_max, 0);
}
