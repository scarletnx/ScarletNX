/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The loongarch64 back end's guest functions: mach/mach_codegen.inc over sailgen's loongarch64 emitters (a64_la64.inc), with
   the RISC back ends' glue (mach/risc_codegen.inc) over la64.h's branches, calls and frame.

   The function's ABI is GuestFn's under the LoongArch LP64D psABI: the state in a0, kept in s0, the entry in a1
   (SGX_FN_TARGET); ra, fp and s0-s8 saved, s1-s8 the pinned guest registers (the callee's: no call changes them), sp
   16-aligned below a frame of spill slots whose size is patched in at the end. The object is ELF LoongArch: a call
   R_LARCH_CALL36 (PCADDU18I and JIRL, a tail call's of t8 and zero), resolved here where the callee is this object's
   own; snx_stale's address an R_LARCH_64 in the code */
#include "common/cpu/recompiler/la64/la64_codegen.h"
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
static _Thread_local const char* g_why; /* what it was */
#define SG_FAIL_JUMP g_fail
#define SG_FAIL_NOTE(what, a, b) (g_why = (what), (void)(a), (void)(b))
#define SGX_FN 1

typedef struct SgXAsm SgXAsm;
static inline void sgx_machine_op(SgXAsm* a, int at, const unsigned char* r);

#include "a64_la64.inc"

/* ---- what mach_codegen.inc asks of the machine ---- */
#define MG_EM EM_LOONGARCH
#define MG_ELF_FLAGS 0x43 /* the LP64D ABI, object file v1: LLVM's loongarch64 objects' */
#define MG_R_ABS64 SNX_LA_64
#define MG_R_LITERAL SNX_LA_64_PCREL
#define MG_R_CALL SNX_LA_CALL36
#define MG_R_TAIL SNX_LA_CALL36
#define SGX_FN_TARGET 5 /* a1 */
static const signed char mg_pin_hosts[8] = {24, 25, 26, 27, 28, 29, 30, 31}; /* s1-s8 */
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
#define MG_DESC snx_la64_desc
#define MG_DESC_NAME "loongarch64"
#include "common/cpu/recompiler/mach/risc_desc.inc"

int snx_la64_available(void) {
#if defined(__loongarch64) && defined(__ELF__)
    return 1;
#else
    return 0;
#endif
}

int snx_la64_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                    unsigned char* made, unsigned char** object, size_t* size, char* error, size_t error_size) {
    return mach_object(codegen, first, last, options, made, object, size, error, error_size, 0);
}

#else
int snx_la64_available(void) { return 0; }
int snx_la64_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                    unsigned char* made, unsigned char** object, size_t* size, char* error, size_t error_size) {
    (void)codegen, (void)options, (void)error, (void)error_size;
    memset(made, 0, last - first);
    *object = NULL;
    *size = 0;
    return 0;
}
#endif
