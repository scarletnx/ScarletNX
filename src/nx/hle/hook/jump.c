/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The jump over a native hook's start (jump.h). Only x86_64's is run by a test (tests/hook_invalidated.c); every
   encoder's bytes are checked on any host (tests/hook_jump.c), against what llvm-mc disassembles them to, and IA-64's
   against GNU objdump's, as clang has no IA-64 target yet */
#include "nx/hle/hook/jump.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

static void put16be(u8* p, u32 v) { p[0] = (u8)(v >> 8), p[1] = (u8)v; }
static void put32le(u8* p, u32 v) { p[0] = (u8)v, p[1] = (u8)(v >> 8), p[2] = (u8)(v >> 16), p[3] = (u8)(v >> 24); }
static void put32be(u8* p, u32 v) { p[0] = (u8)(v >> 24), p[1] = (u8)(v >> 16), p[2] = (u8)(v >> 8), p[3] = (u8)v; }
static void put64le(u8* p, u64 v) { put32le(p, (u32)v), put32le(p + 4, (u32)(v >> 32)); }
static void put64be(u8* p, u64 v) { put32be(p, (u32)(v >> 32)), put32be(p + 4, (u32)v); }
static void put32(u8* p, u32 v, int big) {
    if (big)
        put32be(p, v);
    else
        put32le(p, v);
}
/* whether the signed distance d fits in `bits` bits */
static int fits(i64 d, int bits) { return d >= -((i64)1 << (bits - 1)) && d < ((i64)1 << (bits - 1)); }

size_t snx_hook_jump_x86_64(u8 out[SNX_HOOK_JUMP_MAX], u64 at, u64 to) {
    i64 rel = (i64)(to - (at + 5));
    if (fits(rel, 32)) {
        out[0] = 0xE9; /* jmp rel32 */
        put32le(out + 1, (u32)rel);
        return 5;
    }
    out[0] = 0x49, out[1] = 0xBB; /* movabs r11, imm64 */
    put64le(out + 2, to);
    out[10] = 0x41, out[11] = 0xFF, out[12] = 0xE3; /* jmp r11 */
    return 13;
}

size_t snx_hook_jump_aarch64(u8 out[SNX_HOOK_JUMP_MAX], u64 at, u64 to) {
    i64 off = (i64)(to - at);
    if (!(off & 3) && fits(off, 28)) {
        put32le(out, 0x14000000u | ((u32)(off >> 2) & 0x3FFFFFFu)); /* b */
        return 4;
    }
    put32le(out, 0x58000050u);     /* ldr x16, #8 */
    put32le(out + 4, 0xD61F0200u); /* br x16 */
    put64le(out + 8, to);
    return 16;
}

size_t snx_hook_jump_ppc64(u8 out[SNX_HOOK_JUMP_MAX], u64 to, int elfv1, int big) {
    u32 w[9];
    size_t n = 0, i;
    w[n++] = 0x3D800000u | (u32)(to >> 48 & 0xFFFF); /* lis r12, to@highest */
    w[n++] = 0x618C0000u | (u32)(to >> 32 & 0xFFFF); /* ori r12, r12, to@higher */
    w[n++] = 0x798C07C6u;                            /* sldi r12, r12, 32 */
    w[n++] = 0x658C0000u | (u32)(to >> 16 & 0xFFFF); /* oris r12, r12, to@h */
    w[n++] = 0x618C0000u | (u32)(to & 0xFFFF);       /* ori r12, r12, to@l */
    if (elfv1) {
        w[n++] = 0xE80C0000u; /* ld r0, 0(r12): the entry */
        w[n++] = 0xE84C0008u; /* ld r2, 8(r12): its TOC */
        w[n++] = 0x7C0903A6u; /* mtctr r0 */
    } else
        w[n++] = 0x7D8903A6u; /* mtctr r12 */
    w[n++] = 0x4E800420u;     /* bctr */
    for (i = 0; i < n; i++) put32(out + 4 * i, w[i], big);
    return 4 * n;
}

size_t snx_hook_jump_s390x(u8 out[SNX_HOOK_JUMP_MAX], u64 at, u64 to) {
    i64 off = (i64)(to - at);
    u64 q, i;
    if (!(off & 1) && fits(off / 2, 32)) {
        out[0] = 0xC0, out[1] = 0xF4; /* brcl 15, rel32 (halfwords) */
        put32be(out + 2, (u32)(off / 2));
        return 6;
    }
    q = (at + 8 + 7) & ~(u64)7;   /* the doubleword after the 8 bytes of code, which lgrl needs aligned */
    out[0] = 0xC4, out[1] = 0x18; /* lgrl %r1, q */
    put32be(out + 2, (u32)((q - at) / 2));
    out[6] = 0x07, out[7] = 0xF1;                             /* br %r1 */
    for (i = 8; i < q - at; i += 2) put16be(out + i, 0x0700); /* nopr */
    put64be(out + (q - at), to);
    return (size_t)(q - at + 8);
}

size_t snx_hook_jump_mips64(u8 out[SNX_HOOK_JUMP_MAX], u64 to, int big) {
    const u32 w[8] = {
        0x3C190000u | (u32)(to >> 48 & 0xFFFF), /* lui $t9, to >> 48 */
        0x37390000u | (u32)(to >> 32 & 0xFFFF), /* ori $t9, $t9, ... */
        0x0019CC38u,                            /* dsll $t9, $t9, 16 */
        0x37390000u | (u32)(to >> 16 & 0xFFFF), /* ori */
        0x0019CC38u,                            /* dsll */
        0x37390000u | (u32)(to & 0xFFFF),       /* ori */
        0x03200009u,                            /* jalr $zero, $t9: jr, also on release 6 */
        0x00000000u,                            /* nop, the delay slot */
    };
    size_t i;
    for (i = 0; i < 8; i++) put32(out + 4 * i, w[i], big);
    return 32;
}

size_t snx_hook_jump_riscv64(u8 out[SNX_HOOK_JUMP_MAX], u64 at, u64 to) {
    i64 off = (i64)(to - at);
    u64 q, i;
    if (off >= -((i64)1 << 31) - 0x800 && off < ((i64)1 << 31) - 0x800) {
        i64 hi = (off + 0x800) >> 12, lo = off - hi * 4096;
        put32le(out, 0x00000317u | ((u32)hi & 0xFFFFFu) << 12);   /* auipc t1, hi */
        put32le(out + 4, 0x00030067u | ((u32)lo & 0xFFFu) << 20); /* jalr zero, lo(t1) */
        return 8;
    }
    q = (at + 12 + 7) & ~(u64)7;                         /* an aligned doubleword after the 12 bytes of code */
    put32le(out, 0x00000317u);                           /* auipc t1, 0 */
    put32le(out + 4, 0x00033303u | (u32)(q - at) << 20); /* ld t1, q(t1) */
    put32le(out + 8, 0x00030067u);                       /* jalr zero, 0(t1) */
    for (i = 12; i + 4 <= q - at; i += 4) put32le(out + i, 0x00000013u); /* nop */
    if (i < q - at) out[i] = 0x01, out[i + 1] = 0x00; /* c.nop: code 2 bytes off a word is compressed code */
    put64le(out + (q - at), to);
    return (size_t)(q - at + 8);
}

size_t snx_hook_jump_loongarch64(u8 out[SNX_HOOK_JUMP_MAX], u64 at, u64 to) {
    i64 off = (i64)(to - at), hi = (off + 0x20000) >> 18, lo = off - hi * 0x40000;
    if ((off & 3) || !fits(hi, 20)) return 0;
    put32le(out, 0x1E000000u | ((u32)hi & 0xFFFFFu) << 5 | 12);                 /* pcaddu18i $t0, hi */
    put32le(out + 4, 0x4C000000u | ((u32)(lo >> 2) & 0xFFFFu) << 10 | 12 << 5); /* jirl $zero, $t0, lo */
    return 8;
}

size_t snx_hook_jump_ia64(u8 out[SNX_HOOK_JUMP_MAX], u64 at, u64 to) {
    i64 bundles = (i64)(to - at) >> 4;
    u64 imm, slot0 = (u64)1 << 27, slot1, slot2, lo, hi; /* slot 0: nop.m 0 */
    if (((at | to) & 15) || !fits(bundles, 60)) return 0;
    imm = (u64)bundles & (((u64)1 << 60) - 1);
    slot1 = (imm >> 20 & (((u64)1 << 39) - 1)) << 2;                        /* L: imm39 */
    slot2 = (u64)0xC << 37 | (imm >> 59 & 1) << 36 | (imm & 0xFFFFF) << 13; /* X: brl.sptk.few, i, imm20b */
    lo = 0x05 | slot0 << 5 | slot1 << 46;                                   /* template MLX, a stop at its end */
    hi = slot1 >> 18 | slot2 << 23;
    put64le(out, lo);
    put64le(out + 8, hi);
    return 16;
}

/* ---- the host's */

#if defined(__x86_64__) || defined(_M_X64)
#define HOST "x86_64"
#elif defined(__aarch64__) || defined(_M_ARM64)
#define HOST "aarch64"
#elif defined(__powerpc64__) && defined(_CALL_ELF) && _CALL_ELF == 2
#define HOST "ppc64 (ELFv2)"
#elif defined(__powerpc64__)
#define HOST "ppc64 (ELFv1)"
#elif defined(__s390x__)
#define HOST "s390x"
#elif defined(__mips64)
#define HOST "mips64"
#elif defined(__riscv) && __riscv_xlen == 64
#define HOST "riscv64"
#elif defined(__loongarch64)
#define HOST "loongarch64"
#elif defined(__ia64__)
#define HOST "ia64"
#endif

const char* snx_hook_jump_host(void) {
#ifdef HOST
    return HOST;
#else
    return NULL;
#endif
}

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define HOST_BIG 1
#else
#define HOST_BIG 0
#endif

#if (defined(__powerpc64__) && !(defined(_CALL_ELF) && _CALL_ELF == 2)) || defined(__ia64__)
/* a function pointer is a descriptor whose first doubleword is the code's address */
static uintptr_t code_of(GuestFn f) { return *(const uintptr_t*)(uintptr_t)f; }
#else
static uintptr_t code_of(GuestFn f) { return (uintptr_t)f; }
#endif

/* the host's jump at `at` (after any landing pad) to `to`, landing pad included: its size, 0 when out of reach */
static size_t host_jump(u8 out[SNX_HOOK_JUMP_MAX + 4], uintptr_t at, GuestFn to, size_t* skip) {
    *skip = 0;
#if defined(__x86_64__) || defined(_M_X64)
    if (!memcmp((const void*)at, "\xF3\x0F\x1E\xFA", 4)) *skip = 4; /* endbr64 stays the landing pad */
    return snx_hook_jump_x86_64(out, at + *skip, code_of(to));
#elif defined(__aarch64__) || defined(_M_ARM64)
    {
        u32 first;
        size_t pad = 0;
        memcpy(&first, (const void*)at, 4);
        if ((first & 0xFFFFFF3Fu) == 0xD503241Fu) /* bti: stays the landing pad */
            *skip = 4;
        else if (first == 0xD503233Fu || first == 0xD503237Fu) { /* paciasp or pacibsp, also a landing pad */
            put32le(out, 0xD503245Fu); /* bti c in its place: the jump must not sign the return address */
            pad = 4;
        }
        return pad ? pad + snx_hook_jump_aarch64(out + pad, at + pad, code_of(to))
                   : snx_hook_jump_aarch64(out, at + *skip, code_of(to));
    }
#elif defined(__powerpc64__) && defined(_CALL_ELF) && _CALL_ELF == 2
    (void)at;
    return snx_hook_jump_ppc64(out, (u64)(uintptr_t)to, 0, HOST_BIG);
#elif defined(__powerpc64__)
    (void)at;
    return snx_hook_jump_ppc64(out, (u64)(uintptr_t)to, 1, 1);
#elif defined(__s390x__)
    return snx_hook_jump_s390x(out, at, code_of(to));
#elif defined(__mips64)
    (void)at;
#if defined(__MIPSEB__)
    return snx_hook_jump_mips64(out, code_of(to), 1);
#else
    return snx_hook_jump_mips64(out, code_of(to), 0);
#endif
#elif defined(__riscv) && __riscv_xlen == 64
    return snx_hook_jump_riscv64(out, at, code_of(to));
#elif defined(__loongarch64)
    return snx_hook_jump_loongarch64(out, at, code_of(to));
#elif defined(__ia64__)
    return snx_hook_jump_ia64(out, at, code_of(to));
#else
    (void)out, (void)at, (void)to;
    return 0;
#endif
}

/* makes the n bytes at p writable, copies bytes there and makes them executable again: 0; -1 when the system refused
   the first (nothing written); 1 when it refused the last (written, the code left writable). why says why */
static int code_write(uintptr_t p, const u8* bytes, size_t n, char* why, size_t why_size) {
#if defined(_WIN32)
    DWORD old, back;
    if (!VirtualProtect((void*)p, n, PAGE_EXECUTE_READWRITE, &old)) {
        snprintf(why, why_size, "VirtualProtect of %zu bytes at %p to read, write and execute was refused (error %lu)",
                 n, (void*)p, (unsigned long)GetLastError());
        return -1;
    }
    memcpy((void*)p, bytes, n);
    FlushInstructionCache(GetCurrentProcess(), (void*)p, n);
    if (!VirtualProtect((void*)p, n, old, &back)) {
        snprintf(why, why_size,
                 "VirtualProtect of %zu bytes at %p back to %lu was refused (error %lu): the code stays "
                 "writable",
                 n, (void*)p, (unsigned long)old, (unsigned long)GetLastError());
        return 1;
    }
    return 0;
#else
    uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE), lo = p & ~(page - 1), hi = (p + n + page - 1) & ~(page - 1);
    if (mprotect((void*)lo, hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        snprintf(why, why_size, "mprotect of %zu bytes at %p to read, write and execute was refused: %s",
                 (size_t)(hi - lo), (void*)lo, strerror(errno));
        return -1;
    }
    memcpy((void*)p, bytes, n);
    __builtin___clear_cache((char*)p, (char*)(p + n));
    if (mprotect((void*)lo, hi - lo, PROT_READ | PROT_EXEC)) {
        snprintf(why, why_size,
                 "mprotect of %zu bytes at %p back to read and execute was refused: %s: the code "
                 "stays writable",
                 (size_t)(hi - lo), (void*)lo, strerror(errno));
        return 1;
    }
    return 0;
#endif
}

int snx_hook_jump_patch(GuestFn fn, GuestFn to, char* why, size_t why_size) {
    uintptr_t at = code_of(fn);
    u8 bytes[SNX_HOOK_JUMP_MAX + 4];
    size_t skip, n;
    why[0] = 0;
    if (!snx_hook_jump_host()) {
        snprintf(why, why_size, "this host's architecture has no jump to write");
        return -1;
    }
    n = host_jump(bytes, at, to, &skip); /* reads the code, which is readable as it is */
    if (!n) {
        snprintf(why, why_size, "%s's jump from %p does not reach %p", snx_hook_jump_host(), (void*)at,
                 (void*)(uintptr_t)to);
        return -1;
    }
    return code_write(at + skip, bytes, n, why, why_size);
}
