/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Verify mode (docs/HOOKS.md): a hook's handler checked against the function's own code, call by call. The two can't
   both run on the guest's live memory, since a side effect would happen twice, so only a pure hook is verified at run
   time: one whose function depends on nothing but its arguments, the memory they point to and constant data, and
   changes nothing but its result and the memory its out arguments name (hooks.hook's `pure`, with in[...], out[...]
   and inout[...] on its address arguments). hook_verify_call copies the buffers those arguments name into scratch
   guest memory, runs the function's own code on the CPU with the arguments pointing at the copies, puts the
   registers back, runs the handler on the live memory, and compares the result, the callee-saved registers and the
   bytes of every buffer. The functions with other side effects (waits, I/O, the clock) are verified in tests, on
   recorded inputs */
#ifndef SNX_HLE_HOOK_VERIFY_H
#define SNX_HLE_HOOK_VERIFY_H
#include "common/cpu/aarch64/cpu.h"

/* how a buffer argument is used: read, written, or both */
enum { HOOK_IN = 1, HOOK_OUT = 2, HOOK_INOUT = 3 };
/* the result's kind, which says how much of x0 or v0 is compared: a narrow integer only its own bits (the caller
   reads no more), a bool whether it is 0, an address with the buffers' copies mapped back to the live buffers */
enum {
    HOOK_RET_VOID,
    HOOK_RET_BOOL,
    HOOK_RET_X8,
    HOOK_RET_X16,
    HOOK_RET_X32,
    HOOK_RET_X64,
    HOOK_RET_ADDR,
    HOOK_RET_F32,
    HOOK_RET_F64
};
#define HOOK_NO_REG 0xFF
#define HOOK_BUFS   4

/* a buffer argument: the address in x[reg], len_add + x[len_reg] * len_mul bytes long (len_add alone when len_reg is
   HOOK_NO_REG) */
typedef struct {
    u8 reg, mode, len_reg;
    u32 len_mul;
    u64 len_add;
} HookBuf;

/* a hook's verification: pure 0 for one verify mode skips */
typedef struct {
    u8 pure, ret, nbufs;
    HookBuf buf[HOOK_BUFS];
} HookVerify;

/* what hook_verify_call found */
enum {
    HOOK_AGREE,
    HOOK_DIFFER_RESULT, /* x0 or v0: a and b the handler's and the original's */
    HOOK_DIFFER_REG,    /* the callee-saved register reg (0-31 an x register, 31 sp, 32 + n the low half of vn) */
    HOOK_DIFFER_BYTES,  /* buffer buf, at offset from its start: a and b the bytes there */
    HOOK_SKIP_SIZE,     /* the buffers don't fit in the scratch memory */
    HOOK_SKIP_RANGE     /* a buffer's length overflows, or it runs past the guest's address space */
};
typedef struct {
    int what, reg, buf;
    u64 offset, a, b;
} HookVerifyResult;

/* Runs the call c holds (its arguments in the registers, x30 its return address) through the function's own code
   (original, the recompiled code for entry, which returns through the guest dispatch loop when it leaves early) on
   copies of its buffers in [scratch, scratch + scratch_size), then through handler (the hook's adapter) on the live
   memory, and leaves c as the handler left it. Returns r->what */
int hook_verify_call(CPU* c, u64 entry, GuestFn original, GuestFn handler, const HookVerify* v, u64 scratch,
                     u64 scratch_size, HookVerifyResult* r);
/* a result as the log names it */
const char* hook_verify_text(int what);
#endif
