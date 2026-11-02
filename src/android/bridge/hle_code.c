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

static uint64_t code;

void hle_code_init(void) {
    code = bridge_window_reserve(0x10000, 0x10000);
    if (!code) return;
    /* the window is reserved without access: this page is the guest's, readable for the JIT */
    bridge_map_code(code, jump_code, sizeof jump_code);
}

uint64_t hle_code_symbol(const char* name) {
    if (!code) return 0;
    /* sigsetjmp's mask is not saved: the guest's signal mask is not the bridge's to change yet */
    if (!strcmp(name, "setjmp") || !strcmp(name, "_setjmp") || !strcmp(name, "sigsetjmp"))
        return code + 4 * SETJMP;
    if (!strcmp(name, "longjmp") || !strcmp(name, "_longjmp") || !strcmp(name, "siglongjmp"))
        return code + 4 * LONGJMP;
    return 0;
}
