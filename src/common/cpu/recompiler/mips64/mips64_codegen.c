/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The mips64 back end's guest functions: mach/mach_codegen.inc over sailgen's mips64 emitters (a64_mips64.inc), with
   the RISC back ends' glue (mach/risc_codegen.inc) over mips64.h's branches, calls and frame.

   The function's ABI is GuestFn's under n64: the state in a0, kept in s0, the entry in a1 (SGX_FN_TARGET); ra, gp, fp
   and s0-s7 saved (mips64.h's frame), s1-s7 and gp the pinned guest registers. The object is ELF MIPS64 (n64, its
   relocations' r_info laid out as n64's): a call's and snx_stale's literal in the code is a distance (R_MIPS_PC32), or
   in a JIT unit the address (R_MIPS_64), the callee entered through t9 */
#include "common/cpu/recompiler/mips64/mips64_codegen.h"
#include "common/cpu/recompiler/object/link.h"
#include "common/cpu/recompiler/object/elf_object.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include "common/cpu/aarch64/cpu.h"
#include "common/runtime/interface.h"
#include "common/utilities/bits.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

/* a word the emitters can't make (bits.h's sg_fail) leaves the function to another back end */
static _Thread_local jmp_buf* g_fail;
static _Thread_local const char* g_why; /* what it was */
#define SG_FAIL_JUMP g_fail
#define SG_FAIL_NOTE(what, a, b) (g_why = (what), (void)(a), (void)(b))
#define SGX_FN 1

typedef struct SgXAsm SgXAsm;
static inline void sgx_machine_op(SgXAsm* a, int at, const unsigned char* r);

/* the big-endian host's back end (mips64be_codegen.c) is this one's with MIPS_BE 1: its words, its guest memory's
   accesses and its object in that order */
#ifndef MIPS_BE
#define MIPS_BE 0
#endif
#define RK_BIG_ENDIAN MIPS_BE
#define MG_ELF_BIG MIPS_BE
#include "a64_mips64.inc"

/* ---- what mach_codegen.inc asks of the machine ---- */
#define MG_EM EM_MIPS
#define MG_ELF_FLAGS         0x80000007u /* MIPS64 release 2, PIC, CPIC, NOREORDER */
#define MG_R_INFO(sym, type) snx_elf_info(EM_MIPS, (sym), (type))
#define MG_R_ABS64 SNX_MIPS_64
#define MG_R_LITERAL SNX_MIPS_PC32
#define MG_R_FAR SNX_MIPS_64 /* a JIT unit's literals: its code may be out of a PC32's reach of what it names */
#define MG_R_CALL MG_R_LITERAL
#define MG_R_TAIL MG_R_LITERAL
#define SGX_FN_TARGET 5 /* a1 */
static const signed char mg_pin_hosts[8] = {17, 18, 19, 20, 21, 22, 23, 28}; /* s1-s7, gp */
#define MG_FAR_JUMPS 1 /* past B's 128 KiB: rk_j_far */
#define MG_NPINS 8
#define MG_NPINS_FIXED 8
#define MG_PIN_RESERVED(h) 0
#define MG_VPIN(k) (k)
#define MG_NVPINS 0
#define MG_XRESERVED 1u
static void unit_reloc(void* machine, size_t at, const char* sym, long long addend);

#include "common/cpu/recompiler/mach/mach_codegen.inc"
#include "common/cpu/recompiler/mach/risc_codegen.inc"
/* the module descriptor's functions (native.h's SnxNativeDesc) */
#define MG_DESC snx_mips64_desc
#define MG_DESC_NAME MIPS_BE ? "mips64" : "mips64el"
#include "common/cpu/recompiler/mach/risc_desc.inc"

int snx_mips64_available(void) {
#if defined(__mips64) && defined(__ELF__) && defined(__BYTE_ORDER__) &&                                              \
    __BYTE_ORDER__ == (MIPS_BE ? __ORDER_BIG_ENDIAN__ : __ORDER_LITTLE_ENDIAN__)
    return 1;
#else
    return 0;
#endif
}

int snx_mips64_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                    unsigned char* made, unsigned char** object, size_t* size, char* error, size_t error_size) {
    return mach_object(codegen, first, last, options, made, object, size, error, error_size, 0);
}

#else
int snx_mips64_available(void) { return 0; }
int snx_mips64_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                    unsigned char* made, unsigned char** object, size_t* size, char* error, size_t error_size) {
    (void)codegen, (void)options, (void)error, (void)error_size;
    memset(made, 0, last - first);
    *object = NULL;
    *size = 0;
    return 0;
}
#endif
