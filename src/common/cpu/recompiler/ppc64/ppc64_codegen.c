/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The powerpc64 back end's guest functions: mach/mach_codegen.inc over sailgen's powerpc64 emitters (a64_ppc64.inc), with
   the RISC back ends' glue (mach/risc_codegen.inc) over ppc64.h's branches, calls and frame.

   The function's ABI is GuestFn's under ELFv2: the state in r3, kept in r30, the entry in r4 (SGX_FN_TARGET); r2 and
   r14-r31 saved (ppc64.h's frame), r20-r29 the pinned guest registers. The object is ELF PowerPC64 (ELFv2): a call's
   and snx_stale's literal in the code is a distance (R_PPC64_REL64), the callee entered through CTR with its address
   in r12 */
#include "common/cpu/recompiler/ppc64/ppc64_codegen.h"
#include "common/cpu/recompiler/object/link.h"
#include "common/cpu/recompiler/object/elf_object.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include "common/cpu/aarch64/cpu.h"
#include "common/runtime/interface.h"
#include "common/utilities/bits.h"
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__powerpc64__) && (defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__))
#include <sys/auxv.h> /* AT_HWCAP, where the host is big-endian */
#endif

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
#define MG_R_LITERAL SNX_PPC_REL64
#define MG_R_CALL MG_R_LITERAL
#define MG_R_TAIL MG_R_LITERAL
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

#if defined(__powerpc64__) && defined(__ELF__) && defined(__BYTE_ORDER__) &&                                           \
    __BYTE_ORDER__ == (PPC_BE ? __ORDER_BIG_ENDIAN__ : __ORDER_LITTLE_ENDIAN__)
#define PPC_HOST 1
#else
#define PPC_HOST 0
#endif
#if PPC_BE
/* AT_HWCAP's bits of the processors with LDBRX and STDBRX, which the guest memory's 8-byte accesses are where the host
   has them (ppc64.h's rk_gldst, SGX_F_LDBRX): ISA 2.06's, and the Cell's, which has them before it. Linux's
   (asm/cputable.h) and FreeBSD's (machine/cpu.h). A Book E core of ISA 2.06 (e5500, e6500) has neither bit, nor the
   instructions */
enum { PPC_HWCAP_ARCH_2_06 = 0x00000100, PPC_HWCAP_CELL = 0x00010000 };
/* what a test took out of AT_HWCAP (snx_ppc64be_without) */
static struct {
    _Atomic unsigned long without;
} g_ppc64be;
void snx_ppc64be_without(unsigned long hwcap) {
    atomic_store_explicit(&g_ppc64be.without, hwcap, memory_order_relaxed);
}
unsigned snx_ppc64be_features_of(unsigned long hwcap) {
    return hwcap & (PPC_HWCAP_ARCH_2_06 | PPC_HWCAP_CELL) ? SGX_F_LDBRX : 0;
}
unsigned snx_ppc64be_features(void) {
#if PPC_HOST
    unsigned long hwcap = 0;
#if defined(_ARCH_PWR7)
    hwcap = PPC_HWCAP_ARCH_2_06; /* the program's own baseline */
#elif defined(__linux__)
    hwcap = getauxval(AT_HWCAP);
#elif defined(__FreeBSD__) || defined(__OpenBSD__)
    if (elf_aux_info(AT_HWCAP, &hwcap, sizeof hwcap)) hwcap = 0;
#endif
    return snx_ppc64be_features_of(hwcap & ~atomic_load_explicit(&g_ppc64be.without, memory_order_relaxed));
#else
    return 0;
#endif
}
int snx_ppc64be_floor_of(int elf, char* why, size_t size) {
    if (size) why[0] = 0;
    if (elf == 2) return 0;
    snprintf(why, size, "abi: needs ELFv2, this build has ELFv%d", elf);
    return 1;
}
int snx_ppc64be_floor(char* why, size_t size) {
#if PPC_HOST
    return snx_ppc64be_floor_of(_CALL_ELF, why, size);
#else
    if (size) why[0] = 0;
    return 0;
#endif
}
int snx_ppc64_available(void) { return PPC_HOST && !snx_ppc64be_floor(NULL, 0); }
#define PPC_FEATURES() snx_ppc64be_features()
#else
/* little-endian: ELFv2 and POWER8, the program's own baseline */
int snx_ppc64_available(void) { return PPC_HOST; }
#define PPC_FEATURES() 0u
#endif

int snx_ppc64_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                    unsigned char* made, unsigned char** object, size_t* size, char* error, size_t error_size) {
    return mach_object(codegen, first, last, options, made, object, size, error, error_size, PPC_FEATURES());
}

#else
int snx_ppc64_available(void) { return 0; }
#if defined(PPC_BE) && PPC_BE
int snx_ppc64be_floor(char* why, size_t size) {
    if (size) why[0] = 0;
    return 0;
}
int snx_ppc64be_floor_of(int elf, char* why, size_t size) {
    (void)elf;
    if (size) why[0] = 0;
    return 0;
}
unsigned snx_ppc64be_features(void) { return 0; }
unsigned snx_ppc64be_features_of(unsigned long hwcap) {
    (void)hwcap;
    return 0;
}
void snx_ppc64be_without(unsigned long hwcap) { (void)hwcap; }
#endif
int snx_ppc64_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                    unsigned char* made, unsigned char** object, size_t* size, char* error, size_t error_size) {
    (void)codegen, (void)options, (void)error, (void)error_size;
    memset(made, 0, last - first);
    *object = NULL;
    *size = 0;
    return 0;
}
#endif
