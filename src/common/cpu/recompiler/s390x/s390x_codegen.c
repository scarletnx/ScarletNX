/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The s390x back end's guest functions: mach/mach_codegen.inc over sailgen's s390x emitters (a64_s390x.inc), with
   the RISC back ends' glue (mach/risc_codegen.inc) over s390x.h's branches, calls and frame.

   The function's ABI is GuestFn's under the s390x ELF ABI: the state in r2, kept in r13, the entry in r3
   (SGX_FN_TARGET); r6-r15 saved (s390x.h's frame), no register left to pin a guest register in. The object is ELF
   s390x, big-endian: a call's and snx_stale's absolute address an R_390_64 in the code, loaded by LARL and LG and
   called with BASR */
#include "common/cpu/recompiler/s390x/s390x_codegen.h"
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

/* s390x is big-endian: its instructions, its guest memory's accesses and its object in that order */
#define RK_BIG_ENDIAN 1
#define MG_ELF_BIG 1
#include "a64_s390x.inc"

/* ---- what mach_codegen.inc asks of the machine ---- */
#define MG_EM EM_S390
#define MG_ELF_FLAGS 0
#define MG_R_ABS64 SNX_390_64
#define MG_R_CALL SNX_390_64
#define MG_R_TAIL SNX_390_64
#define SGX_FN_TARGET 3 /* r3 */
static const signed char mg_pin_hosts[1] = {-1}; /* none (s390x.h) */
#define MG_NPINS 0
#define MG_NPINS_FIXED 0
#define MG_PIN_RESERVED(h) 0
#define MG_VPIN(k) (k)
#define MG_NVPINS 0
#define MG_XRESERVED 1u
static void unit_reloc(void* machine, size_t at, const char* sym, long long addend);

#include "common/cpu/recompiler/mach/mach_codegen.inc"
#include "common/cpu/recompiler/mach/risc_codegen.inc"
/* the module descriptor's functions (native.h's SnxNativeDesc) */
#define MG_DESC snx_s390x_desc
#define MG_DESC_NAME "s390x"
#include "common/cpu/recompiler/mach/risc_desc.inc"

int snx_s390x_available(void) {
#if defined(__s390x__) && defined(__ELF__)
    return 1;
#else
    return 0;
#endif
}

int snx_s390x_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                    unsigned char* made, unsigned char** object, size_t* size, char* error, size_t error_size) {
    return mach_object(codegen, first, last, options, made, object, size, error, error_size, 0);
}

#else
int snx_s390x_available(void) { return 0; }
int snx_s390x_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                    unsigned char* made, unsigned char** object, size_t* size, char* error, size_t error_size) {
    (void)codegen, (void)options, (void)error, (void)error_size;
    memset(made, 0, last - first);
    *object = NULL;
    *size = 0;
    return 0;
}
#endif
