/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A jump written over the start of a native hook's own host code, for a hook the game invalidated (hook.c,
   docs/platforms/nx/hooks.md): every call that still reaches the hook, through its dispatch slot or a copy of its
   address (an NRO's import, the JIT's direct call), goes on at another host function with the arguments its caller
   left, as a tail call. One encoder per host architecture, each a pure function of the two addresses, so a test
   checks every one on any host; hook_jump_patch writes the host's own */
#ifndef SNX_HLE_HOOK_JUMP_H
#define SNX_HLE_HOOK_JUMP_H
#include "common/cpu/aarch64/cpu.h"
#include <stddef.h>

/* the longest jump of any architecture, in bytes */
#define SNX_HOOK_JUMP_MAX 40

/* Each writes into out the jump from code at `at` to `to` and gives its size in bytes, or 0 when `to` is out of the
   form's reach. The registers the jump uses are scratch ones no argument is passed in. Addresses are the host's */
/* jmp rel32, or movabs r11, imm64; jmp r11 */
size_t snx_hook_jump_x86_64(u8 out[SNX_HOOK_JUMP_MAX], u64 at, u64 to);
/* b imm26, or ldr x16, #8; br x16; .quad (the words little-endian, as AArch64's instructions always are) */
size_t snx_hook_jump_aarch64(u8 out[SNX_HOOK_JUMP_MAX], u64 at, u64 to);
/* r12 loaded with to (lis, ori, sldi, oris, ori), then: ELFv2, to being the function's global entry, mtctr r12;
   bctr, so the callee computes its TOC from r12; ELFv1, to being its descriptor, ld r0,0(r12); ld r2,8(r12);
   mtctr r0; bctr. big: the words big-endian */
size_t snx_hook_jump_ppc64(u8 out[SNX_HOOK_JUMP_MAX], u64 to, int elfv1, int big);
/* brcl 15, rel32, or lgrl %r1 of a doubleword after it; br %r1 */
size_t snx_hook_jump_s390x(u8 out[SNX_HOOK_JUMP_MAX], u64 at, u64 to);
/* $t9 loaded with to (lui, ori, dsll...), then jalr $zero, $t9 and its delay slot's nop: the callee of n64's PIC finds
   its $gp from $t9. big: the words big-endian */
size_t snx_hook_jump_mips64(u8 out[SNX_HOOK_JUMP_MAX], u64 to, int big);
/* auipc t1; jalr zero, t1, or auipc t1, 0; ld t1 of a doubleword after it; jalr zero, t1 */
size_t snx_hook_jump_riscv64(u8 out[SNX_HOOK_JUMP_MAX], u64 at, u64 to);
/* pcaddu18i $t0; jirl $zero, $t0 (128 GiB either way) */
size_t snx_hook_jump_loongarch64(u8 out[SNX_HOOK_JUMP_MAX], u64 at, u64 to);
/* one MLX bundle: nop.m 0; brl.sptk.few to (both 16-byte aligned bundles of one load module, which share its gp) */
size_t snx_hook_jump_ia64(u8 out[SNX_HOOK_JUMP_MAX], u64 at, u64 to);

/* the name of the host architecture whose encoder hook_jump_patch uses, NULL for a host with none */
const char* snx_hook_jump_host(void);
/* rewrites the start of the host function fn (past its landing pad: x86_64's endbr64, AArch64's bti) into the host's
   jump to `to`: 0; -1 when the host has no encoder, `to` is out of reach, or the system refused to make the code
   writable, with why the reason and the values; 1 when the jump is written but the code stayed writable (why says
   why). Assumes fn's code is long enough for the jump (hooks.md) */
int snx_hook_jump_patch(GuestFn fn, GuestFn to, char* why, size_t why_size);
#endif
