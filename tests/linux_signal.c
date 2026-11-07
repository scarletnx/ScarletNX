/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Linux AArch64 signal ABI that the GNU ABI runtime and the Android bridge share
   (src/common/runtime/linux_signal.c): the frame at the kernel's offsets, and a round trip through rt_sigreturn */
#include "common/runtime/linux_signal.h"
#include "common/utilities/endian.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t frame[SNX_LINUX_SIGFRAME_SIZE];

int main(void) {
    CPU c = {0};
    for (int i = 0; i < 32; ++i) c.x[i] = 0x1000 + (uint64_t)i;
    c.x[31] = 0x7fff1238; /* not 16-aligned: the frame is */
    c.pc = 0x400123;
    c.n = 1;
    c.vf = 1;
    c.fpcr = 0x03000000;
    c.fpsr = 0x10;
    for (int i = 0; i < 32; ++i)
        for (int j = 0; j < 16; ++j) c.v[i].b[j] = (uint8_t)(i * 16 + j);

    /* the frame's place: the record 16-aligned below sp, the frame below it */
    SnxLinuxAltStack none = {0, SNX_SS_DISABLE, 0};
    uint64_t record, at = snx_linux_sigframe_place(c.x[31], 0, &none, &record);
    assert(record == 0x7fff1220 && at % 16 == 0 && record - at >= SNX_LINUX_SIGFRAME_SIZE);
    /* SA_ONSTACK moves it to the top of the alternate stack, unless the thread is on it */
    SnxLinuxAltStack alt = {0x500000, 0, 0x10000};
    uint64_t r2, on = snx_linux_sigframe_place(c.x[31], SNX_SA_ONSTACK, &alt, &r2);
    assert(r2 == 0x50fff0 && on < r2);
    assert(snx_linux_sigframe_place(0x508000, SNX_SA_ONSTACK, &alt, &r2) < 0x508000 && r2 == 0x507ff0);
    assert(snx_linux_altstack_flags(&alt, 0x508000) == SNX_SS_ONSTACK && snx_linux_altstack_flags(&alt, 0x600000) == 0);

    SnxLinuxSiginfo info = {.signo = 30, .code = SNX_SI_TKILL, .pid = 77, .uid = 1000};
    uint8_t si[SNX_LINUX_SIGINFO_SIZE];
    snx_linux_siginfo_encode(si, &info);
    assert(snx_ld_le32(si) == 30 && (int32_t)snx_ld_le32(si + 8) == SNX_SI_TKILL && snx_ld_le32(si + 16) == 77);
    snx_linux_sigframe_build(frame, &c, si, 0x5, &none, 0);

    /* the kernel's offsets: struct rt_sigframe { siginfo; ucontext { flags, link, stack, sigmask, pad, mcontext } } */
    assert(snx_ld_le32(frame) == 30);
    assert(snx_ld_le32(frame + 128 + 16 + 8) == SNX_SS_DISABLE); /* uc_stack.ss_flags */
    assert(snx_ld_le64(frame + 128 + 40) == 0x5);                /* uc_sigmask */
    assert(snx_ld_le64(frame + 304 + 8) == 0x1000);              /* regs[0] */
    assert(snx_ld_le64(frame + 304 + 8 + 8 * 30) == 0x1000 + 30); /* regs[30] */
    assert(snx_ld_le64(frame + 304 + 256) == 0x7fff1238);         /* sp */
    assert(snx_ld_le64(frame + 304 + 264) == 0x400123);           /* pc */
    assert(snx_ld_le64(frame + 304 + 272) == 0x90000000u);        /* pstate: N and V */
    assert(snx_ld_le32(frame + 592) == 0x46508001u && snx_ld_le32(frame + 596) == 528); /* fpsimd_context */
    assert(snx_ld_le32(frame + 600) == 0x10 && snx_ld_le32(frame + 604) == 0x03000000);
    assert(frame[608] == 0 && frame[608 + 16 * 31 + 15] == 255);
    assert(snx_ld_le32(frame + 592 + 528) == 0); /* the terminator */

    /* the handler's entry */
    CPU h = c;
    snx_linux_sigframe_enter(&h, 30, at, record, 0x401000, 0x402000);
    assert(h.x[0] == 30 && h.x[1] == at && h.x[2] == at + 128 && h.x[31] == at && h.x[29] == record &&
           h.x[30] == 0x402000 && h.pc == 0x401000);

    /* rt_sigreturn: the state back, with what the handler changed in the frame */
    snx_st_le64(frame + 304 + 8 + 8 * 5, 0xabc);
    snx_st_le64(frame + 304 + 264, 0x400200);
    uint64_t mask = 0;
    CPU back = {0};
    assert(snx_linux_sigframe_restore(&back, frame, &mask));
    assert(mask == 0x5 && back.x[5] == 0xabc && back.x[6] == 0x1006 && back.x[31] == 0x7fff1238 &&
           back.pc == 0x400200 && back.n == 1 && back.z == 0 && back.vf == 1 && back.fpcr == 0x03000000 &&
           back.fpsr == 0x10 && !memcmp(back.v, c.v, sizeof c.v));
    /* a frame without its FP/SIMD record is refused */
    snx_st_le32(frame + 592, 0x12345678);
    assert(!snx_linux_sigframe_restore(&back, frame, &mask));

    /* the order of delivery, and the masks */
    assert(snx_linux_signal_next(SNX_SIGBIT(30) | SNX_SIGBIT(11) | SNX_SIGBIT(2), 0) == 11);
    assert(snx_linux_signal_next(SNX_SIGBIT(30) | SNX_SIGBIT(2), SNX_SIGBIT(2)) == 30);
    assert(snx_linux_signal_next(SNX_SIGBIT(9), ~0ull) == 9);
    SnxLinuxSigaction act = {0x401000, SNX_SA_SIGINFO | SNX_SA_RESTART, 0, SNX_SIGBIT(24)};
    assert(snx_linux_handler_mask(SNX_SIGBIT(1), 30, &act) == (SNX_SIGBIT(1) | SNX_SIGBIT(24) | SNX_SIGBIT(30)));
    act.flags |= SNX_SA_NODEFER;
    assert(snx_linux_handler_mask(0, 30, &act) == SNX_SIGBIT(24));
    SnxLinuxSigaction dfl = {0};
    assert(snx_linux_signal_ignored(17, &dfl) && !snx_linux_signal_ignored(30, &dfl));
    uint8_t raw[32];
    snx_linux_sigaction_encode(raw, &act);
    SnxLinuxSigaction again = snx_linux_sigaction_decode(raw);
    assert(!memcmp(&again, &act, sizeof act));
    puts("linux_signal: ok");
    return 0;
}
