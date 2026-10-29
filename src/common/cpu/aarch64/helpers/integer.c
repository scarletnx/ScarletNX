/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported in part from QEMU (target/arm/tcg/helper-a64.c, translate-a64.c), LGPL-2.1-or-later:
   Copyright (c) 2013 Alexander Graf <agraf@suse.de> */
/* The integer helpers every generated-code module keeps: the 16-byte compare-and-swap and the system registers EL0
   reaches. The compiler evaluates logical immediates itself, and CRC32 is emitted as IR. */

/* STXP of two doublewords: one atomic compare-and-swap of all 16 bytes. Swapping the halves one at a time let
   Tears of the Kingdom's lock-free lists pair one thread's pointer with another's counter. Clang emits
   lock cmpxchg16b with cx16 enabled (the builds also pass -mcx16: Clang 18 still called libatomic with the
   attribute alone), LDAXP/STLXP on AArch64, and lqarx/stqcx. on PowerPC64 from POWER8 (cmake/cross.cmake's
   -mcpu=power8); GCC makes it a libatomic call, which a module can't link. RISC-V and LoongArch machines have it
   only with Zacas and from v1.1: their modules call the runtime's RtAPI.cas128, the instruction where there is one,
   else a lock. */
#ifndef __clang__
#error "the CPU helpers need Clang: GCC turns the 16-byte compare-and-swap into a libatomic call"
#endif
#if defined(__riscv) || defined(__loongarch__)
u32 snx_cas128(CPU* c, u64* p, u64 o0, u64 o1, u64 n0, u64 n1) { return c->rt->cas128(p, o0, o1, n0, n1); }
#elif defined(__powerpc64__) && !defined(_ARCH_PWR8)
/* a PowerPC64 baseline before POWER8 (cmake/cross.cmake's SNX_PPC64_CPU, the PowerPC 970): RtAPI.cas128 too, which
   takes the doublewords as the host reads p[0] and p[1], each byte-reversed on a big-endian host */
u32 snx_cas128(CPU* c, u64* p, u64 o0, u64 o1, u64 n0, u64 n1) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    o0 = snx_bswap64(o0), o1 = snx_bswap64(o1), n0 = snx_bswap64(n0), n1 = snx_bswap64(n1);
#endif
    return c->rt->cas128(p, o0, o1, n0, n1);
}
#else
/* guest memory, may_alias as cpu.h's snx_guest_u64. A 16-byte compare-and-swap has no ISO C form: it takes the
   compiler's 128-bit integer */
#if !defined(__SIZEOF_INT128__)
#error "STXP's 16-byte compare-and-swap needs the compiler's 128-bit integer, which this compiler has not"
#endif
__extension__ typedef unsigned __int128 __attribute__((may_alias)) snx_u128;
#if defined(__x86_64__)
__attribute__((target("cx16")))
#endif
u32 snx_cas128(CPU* c, u64* p, u64 o0, u64 o1, u64 n0, u64 n1) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    /* the guest's 16 bytes are two little-endian doublewords, the first at p: a big-endian host reads the first as
       the high half, each one byte-reversed */
    snx_u128 expected = ((snx_u128)snx_bswap64(o0) << 64) | snx_bswap64(o1),
             desired = ((snx_u128)snx_bswap64(n0) << 64) | snx_bswap64(n1);
#else
    snx_u128 expected = ((snx_u128)o1 << 64) | o0, desired = ((snx_u128)n1 << 64) | n0;
#endif
    (void)c;
    return __atomic_compare_exchange_n((snx_u128*)p, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
#endif

/* o0:op1:CRn:CRm:op2 of an MRS/MSR system register with op0 = 3 */
#define SYSREG(op1, crn, crm, op2) ((1u << 14) | ((op1) << 11) | ((crn) << 7) | ((crm) << 3) | (op2))
/* the Cortex-A57's values (TRM DDI0488H): CTR_EL0 for 64-byte lines, DCZID_EL0 for 64-byte DC ZVA blocks */
#define A57_CTR_EL0   0x8444C004u
#define A57_DCZID_EL0 4u
#define CNTFRQ_HZ     19200000u
/* the FPCR bits the A57 implements (AHP, DN, FZ, RMode; the trap enables are RES0, TRM tables 14-3 and 14-4) and
   FEAT_FP16's FZ16, which the generated code clears first for a guest without it (the Switch's, where it is RES0),
   and FPSR's N, Z, C, V, QC and cumulative flags */
#define A64_FPCR_MASK 0x07C80000u
#define A57_FPSR_MASK 0xF800009Fu

u64 snx_mrs(CPU* c, u32 sysreg) {
    switch (sysreg) {
        case SYSREG(3, 4, 2, 0): return get_nzcv(c);
        case SYSREG(3, 4, 4, 0): return c->fpcr;
        case SYSREG(3, 4, 4, 1): return c->fpsr;
        case SYSREG(3, 13, 0, 2): return c->tpidr;
        case SYSREG(3, 13, 0, 3): return c->tpidrro;
        case SYSREG(3, 0, 0, 1): return A57_CTR_EL0;
        case SYSREG(3, 0, 0, 7): return A57_DCZID_EL0;
        case SYSREG(3, 14, 0, 0): return CNTFRQ_HZ;
        case SYSREG(3, 14, 0, 1): /* CNTPCT_EL0 */
        case SYSREG(3, 14, 0, 2): /* CNTVCT_EL0 */
            return c->rt && c->rt->ticks ? c->rt->ticks() : 0;
        default: return 0;
    }
}

void snx_msr(CPU* c, u32 sysreg, u64 v) {
    switch (sysreg) {
        case SYSREG(3, 4, 2, 0): set_nzcv(c, (u32)(v >> 28)); break;
        case SYSREG(3, 4, 4, 0):
            c->fpcr = (u32)v & A64_FPCR_MASK;
            if (c->rt && c->rt->fpcr_changed) c->rt->fpcr_changed(c);
            break;
        case SYSREG(3, 4, 4, 1): c->fpsr = (u32)v & A57_FPSR_MASK; break;
        case SYSREG(3, 13, 0, 2): c->tpidr = v; break;
        default: break; /* read-only at EL0 (TPIDRRO_EL0, the ID and counter registers) or not modelled */
    }
}
#undef SYSREG

/* IC IVAU: code the guest wrote in the 64-byte line at a (CTR_EL0's IminLine) is to be fetched from now on, so what
   was compiled from the line is stale (RtAPI.code_changed, which a runtime without code changes leaves NULL) */
void snx_ic_ivau(CPU* c, u64 a) {
    if (c->rt && c->rt->code_changed) c->rt->code_changed(c, a & ~(u64)63);
}
