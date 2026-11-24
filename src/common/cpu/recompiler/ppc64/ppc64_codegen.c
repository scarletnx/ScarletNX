/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The powerpc64 back end's guest functions: mach/mach_codegen.inc over sailgen's powerpc64 emitters (a64_ppc64.inc), with
   the RISC back ends' glue (mach/risc_codegen.inc) over ppc64.h's branches, calls and frame.

   The function's ABI is GuestFn's under ELFv2: the state in r3, kept in r30, the entry in r4 (SGX_FN_TARGET); r2 and
   r14-r31 saved (ppc64.h's frame), r20-r29 the pinned guest registers. The object is ELF PowerPC64 (ELFv2): a call's
   and snx_stale's absolute address an R_PPC64_ADDR64 in the code, the callee entered through CTR with its address in
   r12 */
#include "common/cpu/recompiler/ppc64/ppc64_codegen.h"
#include "common/cpu/recompiler/llvm/link.h"
#include "common/cpu/recompiler/llvm/elf_object.h"
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
#define SG_FAIL_JUMP g_fail
#define SGX_FN 1

typedef struct SgXAsm SgXAsm;
static inline void sgx_machine_op(SgXAsm* a, int at, const unsigned char* r);

/* the big-endian host's back end (ppc64be_codegen.c) is this one's with PPC_BE 1: its words, its guest memory's
   accesses and its object in that order */
#ifndef PPC_BE
#define PPC_BE 0
#endif
#define RK_BIG_ENDIAN PPC_BE
#define MG_ELF_BIG PPC_BE
#include "a64_ppc64.inc"

/* ---- what mach_codegen.inc asks of the machine ---- */
#define MG_EM EM_PPC64
#define MG_ELF_FLAGS 2 /* ELFv2 */
#define MG_R_ABS64 SNX_PPC_ADDR64
#define MG_R_CALL SNX_PPC_ADDR64
#define MG_R_TAIL SNX_PPC_ADDR64
#define SGX_FN_TARGET 4 /* r4 */
static const signed char mg_pin_hosts[10] = {20, 21, 22, 23, 24, 25, 26, 27, 28, 29};
#define MG_NPINS 10
#define MG_NPINS_FIXED 10
#define MG_PIN_RESERVED(h) 0
#define MG_VPIN(k) (k)
#define MG_NVPINS 0
#define MG_XRESERVED 1u
static void unit_reloc(void* machine, size_t at, const char* sym, long long addend);

#include "common/cpu/recompiler/mach/mach_codegen.inc"
#include "common/cpu/recompiler/mach/risc_codegen.inc"
/* the module descriptor's functions (native.h's SnxNativeDesc) */
#define MG_DESC snx_ppc64_desc
#define MG_DESC_NAME PPC_BE ? "ppc64" : "ppc64le"
#include "common/cpu/recompiler/mach/risc_desc.inc"

int snx_ppc64_available(void) {
#if defined(__powerpc64__) && defined(__ELF__) && defined(__BYTE_ORDER__) &&                                          \
    __BYTE_ORDER__ == (PPC_BE ? __ORDER_BIG_ENDIAN__ : __ORDER_LITTLE_ENDIAN__) && (!PPC_BE || _CALL_ELF == 2)
    return 1;
#else
    return 0;
#endif
}

int snx_ppc64_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                    unsigned char* made, unsigned char** object, size_t* size, char* error, size_t error_size) {
    return mach_object(codegen, first, last, options, made, object, size, error, error_size, 0);
}

#else
int snx_ppc64_available(void) { return 0; }
int snx_ppc64_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                    unsigned char* made, unsigned char** object, size_t* size, char* error, size_t error_size) {
    (void)codegen, (void)options, (void)error, (void)error_size;
    memset(made, 0, last - first);
    *object = NULL;
    *size = 0;
    return 0;
}
#endif
