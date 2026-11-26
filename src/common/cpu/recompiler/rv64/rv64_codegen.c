/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The riscv64 back end's guest functions: mach/mach_codegen.inc over sailgen's riscv64 emitters (a64_rv64.inc), with
   the RISC back ends' glue (mach/risc_codegen.inc) over rv64.h's branches, calls and frame.

   The function's ABI is GuestFn's under the RISC-V psABI: the state in a0, kept in s1, the entry in a1
   (SGX_FN_TARGET); ra, s0 and s1 saved, and s2 to s11 where the function pins guest registers there (the callee's: no
   call changes them), sp 16-aligned below a frame of spill slots whose size is patched in at the end. The object is
   ELF RISC-V: a call R_RISCV_CALL_PLT (AUIPC and JALR, a tail call's of t6 and x0), resolved here where the callee is
   this object's own; snx_stale's address an R_RISCV_64 in the code */
#include "common/cpu/recompiler/rv64/rv64_codegen.h"
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

#include "a64_rv64.inc"

/* ---- what mach_codegen.inc asks of the machine ---- */
#define MG_EM EM_RISCV
#define MG_ELF_FLAGS 0x5 /* RVC, the LP64D float ABI: LLVM's riscv64 objects' */
#define MG_R_ABS64 SNX_RV_64
#define MG_R_LITERAL SNX_RV_32_PCREL
#define MG_R_CALL SNX_RV_CALL_PLT
#define MG_R_TAIL SNX_RV_CALL_PLT
#define SGX_FN_TARGET 11 /* a1 */
static const signed char mg_pin_hosts[10] = {18, 19, 20, 21, 22, 23, 24, 25, 26, 27}; /* s2-s11 */
#define MG_FAR_JUMPS 1 /* past JAL's 1 MiB: AUIPC and JALR */
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
#define MG_DESC snx_rv64_desc
#define MG_DESC_NAME "riscv64"
#include "common/cpu/recompiler/mach/risc_desc.inc"

int snx_rv64_available(void) {
#if defined(__riscv) && __riscv_xlen == 64 && defined(__ELF__)
    return 1;
#else
    return 0;
#endif
}

int snx_rv64_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                    unsigned char* made, unsigned char** object, size_t* size, char* error, size_t error_size) {
    return mach_object(codegen, first, last, options, made, object, size, error, error_size, 0);
}

#else
int snx_rv64_available(void) { return 0; }
int snx_rv64_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                    unsigned char* made, unsigned char** object, size_t* size, char* error, size_t error_size) {
    (void)codegen, (void)options, (void)error, (void)error_size;
    memset(made, 0, last - first);
    *object = NULL;
    *size = 0;
    return 0;
}
#endif
