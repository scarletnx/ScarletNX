/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Functions of bionic that are guest code here: they move the guest's own registers and stack, which a host function
   can't. The words are AArch64 code (from llvm-mc), written into the code window, where the JIT compiles them as any
   guest code. The jmp_buf layout is these functions' own (bionic's has room for it): x19 to x30, sp, d8 to d15 */
#include "android/bridge/bridge.h"
#include "android/bridge/hle.h"
#include <string.h>

static const uint32_t jump_code[] = {
    /* setjmp(env): the callee-saved registers, sp and the return address into env, then 0 */
    0x910003f0, /* mov x16, sp */
    0xa9005013, /* stp x19, x20, [x0] */
    0xa9015815, /* stp x21, x22, [x0, #16] */
    0xa9026017, /* stp x23, x24, [x0, #32] */
    0xa9036819, /* stp x25, x26, [x0, #48] */
    0xa904701b, /* stp x27, x28, [x0, #64] */
    0xa905781d, /* stp x29, x30, [x0, #80] */
    0xf9003010, /* str x16, [x0, #96] */
    0x6d06a408, /* stp d8, d9, [x0, #104] */
    0x6d07ac0a, /* stp d10, d11, [x0, #120] */
    0x6d08b40c, /* stp d12, d13, [x0, #136] */
    0x6d09bc0e, /* stp d14, d15, [x0, #152] */
    0x52800000, /* mov w0, #0 */
    0xd65f03c0, /* ret */
    /* longjmp(env, value): back to setjmp's return, with value (1 for 0) */
    0xa9405013, /* ldp x19, x20, [x0] */
    0xa9415815, /* ldp x21, x22, [x0, #16] */
    0xa9426017, /* ldp x23, x24, [x0, #32] */
    0xa9436819, /* ldp x25, x26, [x0, #48] */
    0xa944701b, /* ldp x27, x28, [x0, #64] */
    0xa945781d, /* ldp x29, x30, [x0, #80] */
    0xf9403010, /* ldr x16, [x0, #96] */
    0x9100021f, /* mov sp, x16 */
    0x6d46a408, /* ldp d8, d9, [x0, #104] */
    0x6d47ac0a, /* ldp d10, d11, [x0, #120] */
    0x6d48b40c, /* ldp d12, d13, [x0, #136] */
    0x6d49bc0e, /* ldp d14, d15, [x0, #152] */
    0x7100003f, /* cmp w1, #0 */
    0x1a9f1420, /* csinc w0, w1, wzr, ne */
    0xd65f03c0, /* ret */
};
enum { SETJMP = 0, LONGJMP = 14 };

/* dlopen(name, flags) as libdl's is: a call of the linker's __loader_dlopen with its own return address as the
   caller. Code that finds the linker reads it so: liblinkernsbypass (libadrenotools') takes the target of dlopen's
   first BL as __loader_dlopen and calls it. The BL goes to a veneer beside it, whose literal is the address of the
   HLE's __loader_dlopen, written at init */
static const uint32_t dlopen_code[] = {
    0xa9bf7bfd, /* stp x29, x30, [sp, #-16]! */
    0x910003fd, /* mov x29, sp */
    0xaa1e03e2, /* mov x2, x30 */
    0x94000003, /* bl veneer */
    0xa8c17bfd, /* ldp x29, x30, [sp], #16 */
    0xd65f03c0, /* ret */
    0x58000050, /* veneer: ldr x16, literal */
    0xd61f0200, /* br x16 */
    0, 0,       /* literal: __loader_dlopen */
};
/* dlopen at an even word after setjmp's and longjmp's, so that its literal is 8-byte aligned */
enum { DLOPEN = (sizeof jump_code / sizeof *jump_code + 1) & ~1u, LITERAL = DLOPEN + 8 };
_Static_assert(LITERAL % 2 == 0, "the veneer's literal is 8-byte aligned");

static uint64_t code;

void hle_code_init(void) {
    code = bridge_window_reserve(0x10000, 0x10000);
    if (!code) return;
    uint32_t words[DLOPEN + sizeof dlopen_code / sizeof *dlopen_code] = {0};
    memcpy(words, jump_code, sizeof jump_code);
    memcpy(words + DLOPEN, dlopen_code, sizeof dlopen_code);
    uint64_t loader = hle_symbol("__loader_dlopen");
    words[LITERAL] = (uint32_t)loader;
    words[LITERAL + 1] = (uint32_t)(loader >> 32);
    /* the window is reserved without access: this page is the guest's, readable for the JIT */
    bridge_map_code(code, words, sizeof words);
}

uint64_t hle_code_symbol(const char* name) {
    if (!code) return 0;
    /* sigsetjmp's mask is not saved: the guest's signal mask is not the bridge's to change yet */
    if (!strcmp(name, "setjmp") || !strcmp(name, "_setjmp") || !strcmp(name, "sigsetjmp"))
        return code + 4 * SETJMP;
    if (!strcmp(name, "longjmp") || !strcmp(name, "_longjmp") || !strcmp(name, "siglongjmp"))
        return code + 4 * LONGJMP;
    if (!strcmp(name, "dlopen")) return code + 4 * DLOPEN;
    return 0;
}
