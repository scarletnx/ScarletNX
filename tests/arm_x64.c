/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The x86-64 back end's A32 blocks (src/common/cpu/recompiler/x64/x64_arm.c), or built with SNX_TEST_ARM64 or
   SNX_TEST_RV64 the arm64 or riscv64 back end's (run under Unicorn: snx-test-arm-arm64, -rv64, without the hot paths'
   witnesses, the chain and Thumb), against the AArch32 block compiler's LLVM ones (snx_arm_llvm_block, which
   module_arm.c makes a 32-bit program's blocks with), at -O0: their IR as it is, which LLVM 22's -O2 makes otherwise in
   one case (VRSHL's witness, below): the same words consumed, and the same results from the same random states.
     snx-test-arm-x64 [--blocks N] [--thumb N] [--states N] [--pool N] [--features MASK] [--seed N] [--word HEX]...
         [--halves HEX[,HEX]...] [--dump]
   Each block is words drawn from the A32 instructions the back end makes (data processing, branches, the loads and
   stores of every kind, the multiplies and divides, the bit operations, MRS and MSR, the exclusives and acquire and
   release, the barriers and hints, SVC, BKPT, the coprocessor's and UNDEFINED, ARMv6's and ARMv7's DSP and media
   instructions, VFP and Advanced SIMD: its data processing, its element and structure loads and stores, the lane moves
   and the crypto rounds), under AL half of the time and any
   condition otherwise: one word alone, or two to six in a row (the block ending where one ends it). Thumb blocks (--thumb
   N of them) are halfwords drawn the same way: T16 ones of every form, T32 ones of every family, and one in four starts
   with an IT, the instructions of its block after it, from CPSR.T set and ITSTATE clear; the block compiler of
   llvm_thumb.inc is the reference there. Both back ends make
   it, x64_arm.c's ELF object and LLVM's are loaded by the code loader, and each block runs from --states random states
   (registers into 64 KiB of guest memory of random bytes or anywhere, NZCV, Q and GE random, the exclusive monitor
   armed or not on a register's address with the value there or another): the registers, CPSR, the exit and its
   syndrome, the monitor, the TLS registers, the VFP registers and FPSCR, and guest memory must match. --pool N keeps
   the x86 values in N host registers, so they spill; --features takes the host's BMI1 (1), BMI2 (2) and LZCNT (4) out
   where not in MASK; --word runs the words given (HEX, or HEX,HEX... in a row), one block each, and says what each
   state did; --dump prints a failing state's start */
#include "common/cpu/arm/llvm.h"
#include "common/cpu/arm/simd.h"
#include "common/cpu/recompiler/llvm/code_loader.h"
#include "common/cpu/recompiler/llvm/ir.h"
#if defined(SNX_TEST_ARM64)
#define SNX_TEST_NATIVE 1
#include "common/cpu/recompiler/arm64/arm64_arm.h"
#include "support/host_unicorn.h"
typedef SnxArm64Arm Back;
#define back_(name) snx_arm64_arm_##name
#define BACK_NAME   "arm64"
#define BACK_UC     HOSTUC_ARM64
#elif defined(SNX_TEST_LA64)
#define SNX_TEST_NATIVE 1
#include "common/cpu/recompiler/la64/la64_arm.h"
#include "support/host_unicorn.h"
typedef SnxLa64Arm Back;
#define back_(name) snx_la64_arm_##name
#define BACK_NAME   "loongarch64"
#define BACK_UC     HOSTUC_LA64
#elif defined(SNX_TEST_MIPS64)
#define SNX_TEST_NATIVE 1
#include "common/cpu/recompiler/mips64/mips64_arm.h"
#include "support/host_unicorn.h"
typedef SnxMips64Arm Back;
#define back_(name) snx_mips64_arm_##name
#define BACK_NAME   "mips64el"
#define BACK_UC     HOSTUC_MIPS64EL
#elif defined(SNX_TEST_PPC64BE)
#define SNX_TEST_NATIVE 1
#include "common/cpu/recompiler/ppc64/ppc64be_arm.h"
#include "support/host_unicorn.h"
typedef SnxPpc64beArm Back;
#define back_(name) snx_ppc64be_arm_##name
#define BACK_NAME   "ppc64"
#define BACK_UC     HOSTUC_PPC64
#elif defined(SNX_TEST_S390X)
#define SNX_TEST_NATIVE 1
#include "common/cpu/recompiler/s390x/s390x_arm.h"
#include "support/host_unicorn.h"
typedef SnxS390xArm Back;
#define back_(name) snx_s390x_arm_##name
#define BACK_NAME   "s390x"
#define BACK_UC     HOSTUC_S390X
#elif defined(SNX_TEST_MIPS64BE)
#define SNX_TEST_NATIVE 1
#include "common/cpu/recompiler/mips64/mips64be_arm.h"
#include "support/host_unicorn.h"
typedef SnxMips64beArm Back;
#define back_(name) snx_mips64be_arm_##name
#define BACK_NAME   "mips64"
#define BACK_UC     HOSTUC_MIPS64
#elif defined(SNX_TEST_PPC64)
#define SNX_TEST_NATIVE 1
#include "common/cpu/recompiler/ppc64/ppc64_arm.h"
#include "support/host_unicorn.h"
typedef SnxPpc64Arm Back;
#define back_(name) snx_ppc64_arm_##name
#define BACK_NAME   "ppc64le"
#define BACK_UC     HOSTUC_PPC64LE
#elif defined(SNX_TEST_RV64)
#define SNX_TEST_NATIVE 1
#include "common/cpu/recompiler/rv64/rv64_arm.h"
#include "support/host_unicorn.h"
typedef SnxRv64Arm Back;
#define back_(name) snx_rv64_arm_##name
#define BACK_NAME   "riscv64"
#define BACK_UC     HOSTUC_RV64
#else
#include "common/cpu/recompiler/x64/x64_arm.h"
typedef SnxX64Arm Back;
#define back_(name) snx_x64_arm_##name
#define BACK_NAME   "x86-64"
#endif
#include "common/utilities/bits.h"
#include "common/utilities/common.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t u64;

#define PC        0x1000u /* the blocks' address */
#define MEM_SIZE  0x10000u
#define CHUNK     256
#define MAX_WORDS 6
#define MAX_HALF  16 /* halfwords of a Thumb block */

static struct {
    unsigned states, blocks, thumb, features;
    int pool;
    u64 seed;
    size_t failures, refused, ran, compared, words, consumed_words;
    uint32_t only[64][MAX_WORDS];
    size_t nonly, only_n[64];
    uint16_t halves[MAX_HALF]; /* --halves: one Thumb block */
    size_t nhalves;
    char why[200];
    size_t kinds[64], refusals[64];
    size_t shapes[32], t16; /* the Thumb instructions drawn, by the decoder's shape; the 16-bit ones apart */
    size_t exits[16];   /* the runs' exits, LLVM's: how many ran through and how many stopped, and at what */
    const char* object; /* --object FILE: the first chunk's x86-64 object written there (objdump -d reads it) */
    const char* ll;     /* --ll FILE: the first chunk's LLVM module, before its passes, written there */
    int dump;           /* --dump: a failing state's start printed */
    /* the x86-64 blocks' calls of snx_arm_fp_x64, by op: the VFP's hot paths leave to it what they can't do */
    size_t fp_calls[256], fp_hot_calls;
} g_test;

static u64 rnd(void) {
    u64 x = g_test.seed += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

/* ---- the words ---- */

/* a class of A32 instructions: its fixed bits (value under mask) and the kind the decoder gives its words */
typedef struct {
    const char* name;
    uint32_t mask, value;
    SnxA32Kind kind;
    int weight;
} Class;
static const Class classes[] = {
    {"data, immediate", 0x0e000000u, 0x02000000u, SNX_A32_DATA, 6},
    {"data, register", 0x0e000010u, 0x00000000u, SNX_A32_DATA, 6},
    {"data, register-shifted", 0x0e000090u, 0x00000010u, SNX_A32_DATA, 2},
    {"MOVW, MOVT", 0x0fb00000u, 0x03000000u, SNX_A32_MOVW, 2},
    {"MOVT", 0x0ff00000u, 0x03400000u, SNX_A32_MOVT, 1},
    {"B, BL", 0x0e000000u, 0x0a000000u, SNX_A32_BRANCH, 2},
    {"BX, BLX", 0x0ffffFd0u, 0x012fff10u, SNX_A32_BX, 1},
    {"BLX (immediate)", 0xfe000000u, 0xfa000000u, SNX_A32_BLX_IMM, 1},
    {"LDR, STR, immediate", 0x0e000000u, 0x04000000u, SNX_A32_SINGLE, 8},
    {"LDR, STR, register", 0x0e000010u, 0x06000000u, SNX_A32_SINGLE, 4},
    {"halfword, signed, doubleword", 0x0e000090u, 0x00000090u, SNX_A32_EXTRA, 4},
    {"LDM, STM", 0x0e000000u, 0x08000000u, SNX_A32_MULTIPLE, 4},
    {"MUL, MLA", 0x0fc000f0u, 0x00000090u, SNX_A32_MUL, 1},
    {"long multiplies", 0x0f8000f0u, 0x00800090u, SNX_A32_MULL, 1},
    {"UMAAL", 0x0ff000f0u, 0x00400090u, SNX_A32_MULL, 1},
    {"MLS", 0x0ff000f0u, 0x00600090u, SNX_A32_MLS, 1},
    {"SDIV, UDIV", 0x0fd0f0f0u, 0x0710f010u, SNX_A32_DIV, 1},
    {"CLZ", 0x0fff0ff0u, 0x016f0f10u, SNX_A32_CLZ, 1},
    {"REV, REV16, REVSH, RBIT", 0x0f3f0f70u, 0x063f0f30u, SNX_A32_REV, 1},
    {"extends", 0x0f8003f0u, 0x06800070u, SNX_A32_EXTEND, 2},
    {"SBFX, UBFX", 0x0fa00070u, 0x07a00050u, SNX_A32_BITFIELD, 1},
    {"BFI, BFC", 0x0fe00070u, 0x07c00010u, SNX_A32_BITFIELD, 1},
    {"MRS", 0x0fbf0fffu, 0x010f0000u, SNX_A32_MRS, 1},
    {"MSR (register)", 0x0ff0fff0u, 0x0120f000u, SNX_A32_MSR, 1},
    {"MSR (immediate)", 0x0ff0f000u, 0x0320f000u, SNX_A32_MSR, 1},
    {"exclusives", 0x0f8000f0u, 0x01800090u, SNX_A32_EXCLUSIVE, 2},
    {"acquire and release", 0x0f800cf0u, 0x01800c90u, SNX_A32_ACQUIRE_RELEASE, 1},
    {"CLREX", 0xffffffffu, 0xf57ff01fu, SNX_A32_CLREX, 1},
    {"barriers", 0xffffff00u, 0xf57ff000u, SNX_A32_BARRIER, 1},
    {"hints", 0x0fffff00u, 0x0320f000u, SNX_A32_HINT, 1},
    {"SVC", 0x0f000000u, 0x0f000000u, SNX_A32_SVC, 1},
    {"BKPT", 0xfff000f0u, 0xe1200070u, SNX_A32_BKPT, 1},
    {"MRC, MCR p15 c13 c0 2 and 3 (TLS)", 0x0fef0fdfu, 0x0e0d0f50u, SNX_A32_COPROC, 1},
    {"MRC, MCR p15", 0x0f000f10u, 0x0e000f10u, SNX_A32_COPROC, 1},
    {"UNDEFINED", 0x0ff000f0u, 0x07f000f0u, SNX_A32_UNDEFINED, 1},
    /* ARMv6's and ARMv7's DSP and media */
    {"parallel add and subtract", 0x0f800f10u, 0x06000f10u, SNX_A32_PARALLEL, 3},
    {"SSAT, USAT", 0x0fa00030u, 0x06a00010u, SNX_A32_SATURATE, 1},
    {"SSAT16, USAT16", 0x0fb00ff0u, 0x06a00f30u, SNX_A32_SATURATE, 1},
    {"PKHBT, PKHTB", 0x0ff00030u, 0x06800010u, SNX_A32_PACK, 1},
    {"SEL", 0x0ff00ff0u, 0x06800fb0u, SNX_A32_SELECT, 1},
    {"QADD, QSUB, QDADD, QDSUB", 0x0f900ff0u, 0x01000050u, SNX_A32_QARITH, 1},
    {"halfword multiplies", 0x0f900090u, 0x01000080u, SNX_A32_HALF_MUL, 2},
    {"dual and most significant word multiplies", 0x0f800010u, 0x07000010u, SNX_A32_DUAL_MUL, 2},
    {"USAD8, USADA8", 0x0ff000f0u, 0x07800010u, SNX_A32_ABS_DIFF, 1},
    {"CRC32, CRC32C", 0x0f900df0u, 0x01000040u, SNX_A32_CRC32, 1},
    {"SETEND", 0xfffffdffu, 0xf1010000u, SNX_A32_SETEND, 1},
    /* VFP: its loads and stores, transfers, FPSCR's, and its data processing (the A57's, scalar) */
    {"VLDR, VSTR", 0x0f200e00u, 0x0d000a00u, SNX_A32_VECTOR, 3},
    {"VLDM, VSTM, VPUSH, VPOP", 0x0e000e00u, 0x0c000a00u, SNX_A32_VECTOR, 2},
    {"VMOV core and S", 0x0fe00f7fu, 0x0e000a10u, SNX_A32_VECTOR, 1},
    {"VMOV two cores", 0x0fe00ed0u, 0x0c400a10u, SNX_A32_VECTOR, 1},
    {"VMRS, VMSR", 0x0fef0fffu, 0x0ee10a10u, SNX_A32_VECTOR, 1},
    {"VFP data processing", 0x0f000e10u, 0x0e000a00u, SNX_A32_VECTOR, 8},
    {"VSEL, VMINNM, VMAXNM, VRINT, VCVT", 0xff000e10u, 0xfe000a00u, SNX_A32_VECTOR, 1},
    /* Advanced SIMD: its data processing (the crypto rounds among them), its element and structure loads and stores,
       and the lane moves between core registers and D registers */
    {"Advanced SIMD data processing", 0xfe000000u, 0xf2000000u, SNX_A32_VECTOR, 10},
    {"VLD1-4, VST1-4", 0xff100000u, 0xf4000000u, SNX_A32_VECTOR, 4},
    {"VMOV lane and core, VDUP", 0x0f000f10u, 0x0e000b10u, SNX_A32_VECTOR, 2},
};
#define CLASSES (sizeof classes / sizeof *classes)

/* a word of class c: its free bits random, two times in three under AL and else any condition but 1111 (a class of
   the unconditional space keeps its 1111), a register field 15 one time in eight */
static uint32_t word_of(const Class* c) {
    for (int tries = 0; tries < 100000; ++tries) {
        u64 x = rnd();
        uint32_t w = ((uint32_t)x & ~c->mask) | c->value, cond;
        if ((c->mask >> 28) != 15) {
            cond = (x >> 32) % 3 ? 14u : (uint32_t)(x >> 36) % 15;
            w = (w & 0x0fffffffu) | cond << 28;
        }
        for (unsigned f = 0; f < 4; ++f) { /* Rm, Rs, Rd, Rn, where the class leaves them free */
            static const unsigned at[4] = {0, 8, 12, 16};
            if (((x >> (40 + 3 * f)) & 7) == 0) w |= 15u << at[f] & ~c->mask;
        }
        if (snx_a32_decode(w) != c->kind) continue;
        return w;
    }
    fprintf(stderr, "FAIL: no word of class %s found\n", c->name);
    exit(1);
}
static const Class* pick_class(void) {
    int total = 0, r;
    for (size_t k = 0; k < CLASSES; ++k) total += classes[k].weight;
    r = (int)(rnd() % (u64)total);
    for (size_t k = 0; k < CLASSES; ++k) {
        r -= classes[k].weight;
        if (r < 0) return &classes[k];
    }
    return &classes[0];
}

/* ---- running ---- */

#ifdef SNX_TEST_NATIVE
#define SNX_TEST_SWAP_ARM 1
#include "support/swap_state.h"
static void swap_arm(void* cpu) { snx_test_swap_arm((SnxArmCPU*)cpu); }
#endif

typedef struct {
    SnxArmCPU cpu;
    unsigned char mem[MEM_SIZE];
} RunState;

/* a random state of the kth kind */
/* A block is generated code, with no type signature before its entry, which the function sanitizer's check of an
   indirect call reads: the 8 bytes before the callee, below the code loader's arena for its first one. So the call is
   unchecked, as exec.c's run_block */
[[clang::no_sanitize("function")]] static void run_block(SnxU64 addr, SnxArmCPU* cpu) {
    ((SnxArmBlock)(uintptr_t)addr)(cpu);
}
static void start(RunState* s, unsigned k) {
    SnxArmCPU* cpu = &s->cpu;
    *cpu = (SnxArmCPU){0};
    for (size_t i = 0; i < MEM_SIZE; i += 8) {
        u64 x = rnd();
        memcpy(s->mem + i, &x, 8);
    }
    for (unsigned r = 0; r < 15; ++r) {
        u64 x = rnd();
        int anywhere = k % 4 == 3 || (k % 4 == 2 && (r & 1)) || (k % 4 == 0 && !(r & 1));
        cpu->r[r] = anywhere ? (uint32_t)x : (uint32_t)(x % (MEM_SIZE - 0x400)) & (k % 4 == 1 ? ~3u : ~0u);
        if (!anywhere && (x >> 60) == 0) cpu->r[r] = (uint32_t)(x >> 32) & 31; /* small: shift amounts, divisors */
    }
    cpu->r[13] = 0x8000u + (uint32_t)(rnd() % 0x4000) * (k % 2 ? 4 : 1);
    cpu->r[15] = PC;
    cpu->cpsr = 0x10u | ((uint32_t)rnd() & 0xF80F0000u);
    cpu->exit = (uint32_t)rnd();
    cpu->syndrome = (uint32_t)rnd();
    cpu->mem = s->mem;
    cpu->mem_size = MEM_SIZE;
    {
        static const uint32_t sizes[4] = {1, 2, 4, 8};
        u64 x = rnd();
        cpu->exclusive_valid = k % 3 != 0;
        cpu->exclusive_address = x & 1 ? cpu->r[(x >> 1) % 15] : (uint32_t)(x >> 8) % MEM_SIZE;
        cpu->exclusive_size = sizes[(x >> 40) % 4];
        if ((x >> 44) & 1 && cpu->exclusive_address <= MEM_SIZE - 8)
            memcpy(&cpu->exclusive_value, s->mem + cpu->exclusive_address, 8); /* the value there: a STREX takes */
        else
            cpu->exclusive_value = rnd();
        if (cpu->exclusive_size < 8) cpu->exclusive_value &= (1ull << (8 * cpu->exclusive_size)) - 1;
    }
    cpu->tpidrurw = (uint32_t)rnd();
    cpu->tpidruro = (uint32_t)rnd();
    {
        /* FPSCR: NZCV and the cumulative flags random, IXC set three times in five (the hot paths need it), FZ and DN
           half the time, a rounding other than to nearest one time in five, a trap enabled one time in ten */
        u64 x = rnd();
        uint32_t f = (uint32_t)x & 0xf000009fu;
        if (x >> 32 & 1) f |= 1u << 24;    /* FZ */
        if (x >> 33 & 1) f |= 1u << 25;    /* DN */
        if ((x >> 34) % 5 < 3) f |= 0x10u; /* IXC */
        if ((x >> 38) % 5 == 0) f |= (uint32_t)((x >> 42) & 3) << 22;
        if ((x >> 45) % 10 == 0) f |= 1u << (8 + (x >> 50) % 5);
        cpu->fpscr = f;
    }
    for (unsigned i = 0; i < 32; ++i) {
        /* singles and doubles near 1, zeros, denormals, infinities, NaNs and random bits */
        uint64_t v = 0;
        for (int half = 0; half < (i & 1 ? 1 : 2); ++half) {
            u64 x = rnd();
            int wide = i & 1, sign = (int)(x >> 63);
            unsigned kind = (unsigned)(x >> 56) % 20;
            uint64_t bits, mant = x & (wide ? 0xfffffffffffffull : 0x7fffffull);
            if (kind < 8) /* normal, exponent near the bias */
                bits =
                    wide ? (uint64_t)(1020 + (x >> 52) % 8) << 52 | mant : (uint64_t)(124 + (x >> 40) % 8) << 23 | mant;
            else if (kind < 10)
                bits = 0;
            else if (kind < 12)
                bits = mant ? mant : 1; /* a denormal */
            else if (kind < 13)
                bits = wide ? 0x7ffull << 52 : 0xffull << 23; /* infinity */
            else if (kind < 14)
                bits = (wide ? 0x7ffull << 52 : 0xffull << 23) | (mant ? mant : 1); /* a NaN */
            else if (kind < 15) /* the smallest normals, where a product lands under them */
                bits = (wide ? 1ull << 52 : 1ull << 23) | (mant & 0xff);
            else
                bits = wide ? x & ~(1ull << 63) : x & 0x7fffffffu;
            bits |= (uint64_t)sign << (wide ? 63 : 31);
            if (wide)
                v = bits;
            else
                v |= bits << (32 * half);
        }
        cpu->d[i] = v;
    }
}
/* what differs between the two runs, "" for nothing */
static const char* differs(const RunState* a, const RunState* b) {
    char* why = g_test.why;
    const SnxArmCPU *x = &a->cpu, *y = &b->cpu;
    for (unsigned r = 0; r < 16; ++r)
        if (x->r[r] != y->r[r])
            return snprintf(why, sizeof g_test.why, "r%u %08x, " BACK_NAME " %08x", r, x->r[r], y->r[r]), why;
    if (x->cpsr != y->cpsr)
        return snprintf(why, sizeof g_test.why, "cpsr %08x, " BACK_NAME " %08x", x->cpsr, y->cpsr), why;
    if (x->exit != y->exit) return snprintf(why, sizeof g_test.why, "exit %u, " BACK_NAME " %u", x->exit, y->exit), why;
    if (x->syndrome != y->syndrome)
        return snprintf(why, sizeof g_test.why, "syndrome %08x, " BACK_NAME " %08x", x->syndrome, y->syndrome), why;
    if (x->exclusive_valid != y->exclusive_valid || x->exclusive_address != y->exclusive_address ||
        x->exclusive_size != y->exclusive_size || x->exclusive_value != y->exclusive_value)
        return snprintf(why, sizeof g_test.why,
                        "monitor valid %u address %08x size %u value %016llx, " BACK_NAME " %u %08x %u %016llx",
                        x->exclusive_valid, x->exclusive_address, x->exclusive_size,
                        (unsigned long long)x->exclusive_value, y->exclusive_valid, y->exclusive_address,
                        y->exclusive_size, (unsigned long long)y->exclusive_value),
               why;
    if (x->tpidrurw != y->tpidrurw || x->tpidruro != y->tpidruro) return "the TLS registers";
    if (x->fpscr != y->fpscr)
        return snprintf(why, sizeof g_test.why, "fpscr %08x, " BACK_NAME " %08x", x->fpscr, y->fpscr), why;
    for (unsigned i = 0; i < 32; ++i)
        if (x->d[i] != y->d[i])
            return snprintf(why, sizeof g_test.why, "d%u %016llx, " BACK_NAME " %016llx", i,
                            (unsigned long long)x->d[i],
                            (unsigned long long)y->d[i]),
                   why;
    for (size_t i = 0; i < MEM_SIZE; ++i)
        if (a->mem[i] != b->mem[i])
            return snprintf(why, sizeof g_test.why, "guest memory at %04zx: %02x, " BACK_NAME " %02x", i, a->mem[i],
                            b->mem[i]),
                   why;
    return "";
}

/* snx_arm_fp_x64, counted: an arithmetic op or a compare the hot path could have done (fp_call's), and the others */
static uint64_t counted_fp(SnxArmCPU* cpu, uint64_t x, uint64_t y, uint64_t add, uint64_t desc) {
    unsigned op = (unsigned)desc & 255;
    ++g_test.fp_calls[op];
    return snx_arm_fp_x64(cpu, x, y, add, desc);
}
static SnxCodeLoader* new_loader(char* error, size_t size) {
    SnxCodeLoader* loader = snx_code_loader_new(0, error, size);
    if (loader && (snx_code_define(loader, "snx_arm_fp", (SnxU64)(uintptr_t)snx_arm_fp) ||
                   snx_code_define(loader, "snx_arm_fp_x64", (SnxU64)(uintptr_t)counted_fp) ||
                   snx_code_define(loader, "snx_arm_crypto", (SnxU64)(uintptr_t)snx_arm_crypto) ||
                   snx_code_define(loader, "snx_arm_undefined", (SnxU64)(uintptr_t)snx_arm_undefined) ||
                   snx_code_define(loader, "snx_arm_coproc", (SnxU64)(uintptr_t)snx_arm_coproc) ||
                   snx_code_define(loader, "snx_arm_multiply_carry", (SnxU64)(uintptr_t)snx_arm_multiply_carry))) {
        snprintf(error, size, "cannot name the runtime library's functions");
        snx_code_loader_free(loader);
        return NULL;
    }
    return loader;
}
typedef struct {
    uint32_t words[MAX_WORDS];
    uint16_t half[MAX_HALF];
    int thumb; /* a Thumb block: half, n halfwords; else words, n of them */
    size_t n, consumed;
    int made; /* the x86-64 back end made it */
} Block;
/* the block's code, as the two back ends take it: the host is little-endian, as guest memory */
static const unsigned char* code_of(const Block* b) {
    return b->thumb ? (const unsigned char*)b->half : (const unsigned char*)b->words;
}

/* ---- Thumb ---- */

/* classes of T16 halfwords that random ones rarely are: their fixed bits and how often they are drawn */
typedef struct {
    uint16_t mask, value;
    int weight;
} Half;
static const Half t16_classes[] = {
    {0xE000, 0x0000, 4}, {0xF800, 0x1800, 2}, {0xE000, 0x2000, 3}, {0xFC00, 0x4000, 4}, {0xFC00, 0x4400, 3},
    {0xF800, 0x4800, 1}, {0xF000, 0x5000, 3}, {0xE000, 0x6000, 3}, {0xF000, 0x8000, 2}, {0xF000, 0x9000, 2},
    {0xF000, 0xA000, 2}, {0xFF00, 0xB000, 1}, {0xF500, 0xB100, 2}, {0xFE00, 0xB400, 2}, {0xFE00, 0xBC00, 2},
    {0xFF00, 0xB200, 1}, {0xFF00, 0xBA00, 1}, {0xFF00, 0xBE00, 1}, {0xFFE0, 0xBF00, 1}, {0xF800, 0xC000, 2},
    {0xF800, 0xC800, 2}, {0xFF00, 0xDF00, 1}, {0xF000, 0xD000, 2}, {0xF800, 0xE000, 1},
};
/* families of T32 instructions: the fixed bits of their halfwords, how often they are drawn */
typedef struct {
    uint16_t mask1, value1, mask2, value2;
    int weight;
} Pair;
static const Pair t32_classes[] = {
    {0xFE00, 0xF800, 0x0000, 0x0000, 4}, /* loads and stores of a single register, PLD, PLI */
    {0xFE00, 0xE800, 0x0000, 0x0000, 4}, /* LDM, STM, LDRD, STRD, the exclusives, TBB, TBH */
    {0xFE00, 0xEA00, 0x8000, 0x0000, 4}, /* data processing with a shifted register */
    {0xFA00, 0xF000, 0x8000, 0x0000, 4}, /* data processing with a modified immediate */
    {0xFA00, 0xF200, 0x8000, 0x0000, 3}, /* MOVW, MOVT, ADDW, SUBW, SBFX, UBFX, BFI, SSAT, USAT */
    {0xF800, 0xF000, 0x8000, 0x8000, 2}, /* branches and control */
    {0xFF00, 0xFA00, 0x0000, 0x0000, 4}, /* register shifts, extends, parallel, DSP, the multiplies' neighbours */
    {0xFF80, 0xFB00, 0x0000, 0x0000, 2}, /* multiplies */
    {0xFF80, 0xFB80, 0x0000, 0x0000, 2}, /* long multiplies and divides */
    {0xEC00, 0xEC00, 0x0000, 0x0000, 4}, /* the coprocessors, VFP and Advanced SIMD */
    {0xFF10, 0xF900, 0x0000, 0x0000, 1}, /* Advanced SIMD loads and stores (VLD, VST) */
    {0xFFF0, 0xFB00, 0x00F0, 0x0000, 2}, /* MUL, MLA */
    {0xFFF0, 0xFB00, 0x00F0, 0x0010, 1}, /* MLS */
    {0xFFF0, 0xF340, 0x8020, 0x0000, 1}, /* SBFX */
    {0xFFF0, 0xF3C0, 0x8020, 0x0000, 1}, /* UBFX */
    {0xFFF0, 0xF360, 0x8020, 0x0000, 1}, /* BFI, BFC */
    {0xFFF0, 0xE8D0, 0xFFE0, 0xF000, 2}, /* TBB, TBH */
    {0xFE50, 0xE840, 0x0000, 0x0000, 2}, /* LDRD, STRD (and the exclusives' space) */
    {0xFFF0, 0xE850, 0x0F00, 0x0F00, 1}, /* LDREX (T1) */
    {0xFFF0, 0xE840, 0x0000, 0x0000, 1}, /* STREX (T1) */
};
static uint16_t t16_word(void) {
    if (rnd() & 1) {
        uint16_t h;
        do h = (uint16_t)rnd();
        while ((h >> 11) >= 0x1D);
        return h;
    }
    int total = 0, r;
    for (size_t k = 0; k < sizeof t16_classes / sizeof *t16_classes; ++k) total += t16_classes[k].weight;
    r = (int)(rnd() % (u64)total);
    for (size_t k = 0;; ++k) {
        r -= t16_classes[k].weight;
        if (r < 0) return (uint16_t)(((uint16_t)rnd() & ~t16_classes[k].mask) | t16_classes[k].value);
    }
}
/* a T32 instruction's two halfwords, of any family; one in three any word of the space, defined or not, else one the
   decoder has */
static void t32_pair(uint16_t* a, uint16_t* b) {
    int total = 0, anything = rnd() % 3 == 0;
    for (size_t k = 0; k < sizeof t32_classes / sizeof *t32_classes; ++k) total += t32_classes[k].weight;
    for (int tries = 0; tries < 10000; ++tries) {
        int r = (int)(rnd() % (u64)total);
        size_t k = 0;
        while ((r -= t32_classes[k].weight) >= 0) ++k;
        const Pair* c = &t32_classes[k];
        uint16_t x = (uint16_t)(((uint16_t)rnd() & ~c->mask1) | c->value1);
        uint16_t y = (uint16_t)(((uint16_t)rnd() & ~c->mask2) | c->value2);
        SnxThumb t;
        if ((x >> 11) < 0x1D) continue;
        if (!anything && !snx_thumb_decode(x, y, &t)) continue;
        *a = x;
        *b = y;
        return;
    }
    fprintf(stderr, "FAIL: no T32 word found\n");
    exit(1);
}
/* the instructions of a Thumb block counted by their shape (the 16-bit ones apart: the Sail's) */
static void count_shapes(const Block* b) {
    for (size_t k = 0; k < b->n; ++k) {
        SnxThumb t;
        unsigned hw1 = b->half[k], hw2 = k + 1 < b->n ? b->half[k + 1] : 0;
        if ((hw1 >> 11) < 0x1D) {
            ++g_test.t16;
            continue;
        }
        (void)snx_thumb_decode((uint16_t)hw1, (uint16_t)hw2, &t);
        ++g_test.shapes[t.shape & 31];
        ++k;
    }
}
/* a Thumb block: one to five instructions, one time in four an IT first whose block they are, else with one among them */
static void random_thumb(Block* b) {
    unsigned n = rnd() % 3 == 0 ? 1 : 2 + (unsigned)(rnd() % 4), k = 0;
    b->thumb = 1;
    b->n = 0;
    if (rnd() % 4 == 0) {
        b->half[b->n++] = (uint16_t)(0xBF00u | (unsigned)(rnd() % 15) << 4 | (1u + (unsigned)(rnd() % 15)));
        n = 1 + (unsigned)(rnd() % 4);
    }
    for (; k < n; ++k) {
        if (rnd() % 3 == 0) {
            t32_pair(&b->half[b->n], &b->half[b->n + 1]);
            b->n += 2;
        } else {
            b->half[b->n++] = t16_word();
        }
    }
}

/* the object loaded, each block's function in addr (those the x86-64 back end made alone, for its object) */
static int load(SnxCodeLoader* loader, unsigned char* object, size_t size, const char* what, const Block* blocks,
                size_t count, int made_only, SnxU64* addr) {
    char error[512], (*storage)[16] = calloc(count + 1, 16);
    const char** names = (const char**)calloc(count + 1, sizeof *names);
    SnxU64* got = (SnxU64*)calloc(count + 1, sizeof *got);
    size_t* index = (size_t*)calloc(count + 1, sizeof *index);
    size_t n = 0;
    if (!storage || !names || !got || !index) {
        fprintf(stderr, "FAIL: the %s object: out of memory for the names of %zu blocks\n", what, count);
        free(names), free(storage), free(got), free(index);
        return 1;
    }
    for (size_t i = 0; i < count; ++i) {
        if (made_only && !blocks[i].made) continue;
        snprintf(storage[n], sizeof storage[n], "B_%zu", i);
        names[n] = storage[n];
        index[n++] = i;
    }
    SnxLinkInput input = {.data = object, .size = size, .name = what};
    int rc = snx_code_load_private(loader, &input, 1, names, n, got, error, sizeof error);
    if (rc) fprintf(stderr, "FAIL: the %s object: %s\n", what, error);
    for (size_t k = 0; !rc && k < n; ++k) addr[index[k]] = got[k];
    free(names);
    free(storage);
    free(got);
    free(index);
    return rc;
}

#if defined(SNX_TEST_NATIVE)
/* the native object loaded into Unicorn, the helpers its blocks call its traps: each block's function in addr */
static int load_uc(HostUc* uc, unsigned char* object, size_t size, const Block* blocks, size_t count, SnxU64* addr) {
    char error[512] = "";
    if (!hostuc_native(uc, "snx_arm_fp_x64", (void*)counted_fp) ||
        !hostuc_native(uc, "snx_arm_undefined", (void*)snx_arm_undefined) ||
        !hostuc_native(uc, "snx_arm_coproc", (void*)snx_arm_coproc) ||
        !hostuc_native(uc, "snx_arm_crypto", (void*)snx_arm_crypto) ||
        hostuc_load(uc, object, size, error, sizeof error)) {
        fprintf(stderr, "FAIL: the " BACK_NAME " object: %s\n", error);
        return 1;
    }
    for (size_t i = 0; i < count; ++i) {
        char name[16];
        if (!blocks[i].made) continue;
        snprintf(name, sizeof name, "B_%zu", i);
        if (!(addr[i] = hostuc_symbol(uc, name))) {
            fprintf(stderr, "FAIL: the " BACK_NAME " object has no %s\n", name);
            return 1;
        }
    }
    return 0;
}
#endif

static void block_text(const Block* b, char* out, size_t size) {
    size_t at = 0;
    for (size_t i = 0; i < b->n && at < size; ++i)
        at += (size_t)snprintf(out + at, size - at, "%s%0*x", i ? " " : "", b->thumb ? 4 : 8,
                               b->thumb ? b->half[i] : b->words[i]);
}

/* the chunk's blocks made by both, loaded and run */
static int run_chunk(SnxIr* ir, Back* x64, Block* blocks, size_t count) {
    LLVMModuleRef module = LLVMModuleCreateWithNameInContext("llvm", ir->ctx);
    SnxCodeLoader *lloader = NULL, *xloader = NULL;
    unsigned char *lobj = NULL, *xobj = NULL;
    size_t lsize = 0, xsize = 0, made = 0;
    SnxU64 *laddr = calloc(count + 1, sizeof *laddr), *xaddr = calloc(count + 1, sizeof *xaddr);
    char error[512], text[100];
#if defined(SNX_TEST_NATIVE)
    /* the native code's world: the states in memory Unicorn maps at their own addresses */
    HostUc* uc = hostuc_new(BACK_UC, 16u << 20);
    RunState* s = uc ? (RunState*)hostuc_alloc(uc, 2 * sizeof *s) : NULL;
    if (s) hostuc_swapper(uc, swap_arm, &s[1].cpu); /* a big-endian machine's state turned over around its runs */
#else
    RunState* s = (RunState*)malloc(2 * sizeof *s);
#endif
    int rc = 0;
    if (!laddr || !xaddr || !s) {
        fprintf(stderr, "FAIL: out of memory for a chunk of %zu blocks\n", count);
        free(laddr), free(xaddr);
#if defined(SNX_TEST_NATIVE)
        if (uc) hostuc_free(uc);
#else
        free(s);
#endif
        LLVMDisposeModule(module);
        ++g_test.failures;
        return -1;
    }
    LLVMSetTarget(module, LLVMGetTarget(ir->mod));
    LLVMSetDataLayout(module, LLVMGetDataLayoutStr(ir->mod));
    for (size_t i = 0; i < count; ++i) {
        Block* b = &blocks[i];
        char name[16];
        size_t lc = 0, xc = 0;
        snprintf(name, sizeof name, "B_%zu", i);
        if (!(b->thumb ? snx_thumb_llvm_block(module, name, b->half, b->n, PC, &lc)
                       : snx_arm_llvm_block(module, name, b->words, b->n, PC, &lc))) {
            block_text(b, text, sizeof text);
            fprintf(stderr, "FAIL: [%s]: LLVM refused it\n", text);
            ++g_test.failures;
            rc = -1;
            break;
        }
        b->consumed = lc;
#if defined(SNX_TEST_NATIVE)
        b->made = back_(block)(x64, name, code_of(b), b->n, PC, &xc, NULL, NULL);
#else
        b->made = b->thumb ? snx_x64_thumb_block(x64, name, code_of(b), b->n, PC, &xc, NULL, NULL)
                           : back_(block)(x64, name, code_of(b), b->n, PC, &xc, NULL, NULL);
#endif
        if (!b->made) {
            SnxA32Kind k = b->thumb ? SNX_A32_UNDEFINED : snx_a32_decode(b->words[0]);
            block_text(b, text, sizeof text);
            ++g_test.refused;
            ++g_test.refusals[k & 63];
            fprintf(stderr, "FAIL: [%s]: the " BACK_NAME " back end refused it: %s\n", text, back_(why)(x64));
            ++g_test.failures;
            continue;
        }
        ++made;
        if (xc != lc) {
            block_text(b, text, sizeof text);
            fprintf(stderr, "FAIL: [%s]: LLVM's block takes %zu words, the " BACK_NAME " back end's %zu\n", text, lc,
                    xc);
            ++g_test.failures;
            b->made = 0;
        }
        if (!b->thumb) {
            g_test.words += b->n;
            g_test.consumed_words += lc;
        }
    }
    if (!rc && g_test.ll) { /* --ll FILE: the first chunk's LLVM module before any pass, for opt and llc */
        char* message = NULL;
        if (LLVMPrintModuleToFile(module, g_test.ll, &message))
            fprintf(stderr, "FAIL: cannot write %s: %s\n", g_test.ll, message ? message : ""), ++g_test.failures;
        LLVMDisposeMessage(message);
        g_test.ll = NULL;
    }
    if (!rc && (snx_ir_module_object(ir, module, '0', &lobj, &lsize, error, sizeof error) ||
                back_(object)(x64, &xobj, &xsize, NULL))) {
        fprintf(stderr, "FAIL: an object: %s\n", error);
        ++g_test.failures;
        rc = -1;
    }
    if (!rc && g_test.object) {
        FILE* f = fopen(g_test.object, "wb");
        if (!f || fwrite(xobj, 1, xsize, f) != xsize)
            fprintf(stderr, "FAIL: cannot write %s\n", g_test.object), ++g_test.failures;
        if (f) fclose(f);
        g_test.object = NULL;
    }
    if (!rc && (!(lloader = new_loader(error, sizeof error)) || !(xloader = new_loader(error, sizeof error)))) {
        fprintf(stderr, "FAIL: no code loader: %s\n", error);
        ++g_test.failures;
        rc = -1;
    }
#if defined(SNX_TEST_NATIVE)
    if (!rc && (load(lloader, lobj, lsize, "LLVM", blocks, count, 0, laddr) ||
                (made && load_uc(uc, xobj, xsize, blocks, count, xaddr)))) {
#else
    if (!rc && (load(lloader, lobj, lsize, "LLVM", blocks, count, 0, laddr) ||
                (made && load(xloader, xobj, xsize, BACK_NAME, blocks, count, 1, xaddr)))) {
#endif
        ++g_test.failures;
        rc = -1;
    }
    for (size_t i = 0; !rc && i < count; ++i) {
        Block* b = &blocks[i];
        if (!b->made) continue;
        ++g_test.compared;
        for (unsigned k = 0; k < g_test.states; ++k) {
            u64 seed = g_test.seed;
            const char* why;
            for (int side = 0; side < 2; ++side) {
                g_test.seed = seed;
                start(&s[side], k);
                if (b->thumb) s[side].cpu.cpsr |= SNX_ARM_T;
#if defined(SNX_TEST_NATIVE)
                if (side) {
                    if (hostuc_call(uc, xaddr[i], (u64)(uintptr_t)&s[1].cpu, 0, NULL)) s[1].cpu.exit = 0xdead;
                    continue;
                }
#endif
                run_block(side ? xaddr[i] : laddr[i], &s[side].cpu);
            }
            ++g_test.ran;
            ++g_test.exits[s[0].cpu.exit & 15];
            why = differs(&s[0], &s[1]);
            if (g_test.nonly) {
                block_text(b, text, sizeof text);
                printf("[%s] state %u: LLVM exit %u r15 %08x cpsr %08x, " BACK_NAME " exit %u r15 %08x cpsr %08x%s%s\n",
                       text, k, s[0].cpu.exit, s[0].cpu.r[15], s[0].cpu.cpsr, s[1].cpu.exit, s[1].cpu.r[15], s[1].cpu.cpsr,
                       *why ? ": " : "", why);
            }
            if (*why) {
                block_text(b, text, sizeof text);
                fprintf(stderr, "FAIL: [%s] state %u: %s\n", text, k, why);
                if (g_test.dump) { /* --dump: the state the runs started from, to rebuild the case by hand */
                    g_test.seed = seed;
                    start(&s[0], k);
                    if (b->thumb) s[0].cpu.cpsr |= SNX_ARM_T;
                    fprintf(stderr, "  start: fpscr %08x cpsr %08x\n", s[0].cpu.fpscr, s[0].cpu.cpsr);
                    for (unsigned r = 0; r < 16; ++r)
                        fprintf(stderr, "  r%u %08x%s", r, s[0].cpu.r[r], r % 8 == 7 ? "\n" : "");
                    for (unsigned d = 0; d < 32; ++d)
                        fprintf(stderr, "  d%u %016llx%s", d, (unsigned long long)s[0].cpu.d[d],
                                d % 4 == 3 ? "\n" : "");
                }
                ++g_test.failures;
                break;
            }
        }
    }
#if defined(SNX_TEST_NATIVE)
    hostuc_free(uc);
#else
    free(s);
#endif
    free(lobj);
    free(xobj);
    free(laddr);
    free(xaddr);
    if (lloader) snx_code_loader_free(lloader);
    if (xloader) snx_code_loader_free(xloader);
    LLVMDisposeModule(module);
    return rc;
}

#if !defined(SNX_TEST_NATIVE)
/* ---- the VFP's hot paths, witnessed: each word with its operands and FPSCR, and how many times its x86-64 block calls
   snx_arm_fp_x64 (0: the hot path made the result), its results LLVM's ---- */

typedef struct {
    const char* what;
    uint32_t word, fpscr;
    uint64_t d0, d1, d2; /* S0 S1 = D0's low and high words, S2 = D1's low one; or D0 to D2 */
    int cold;
    uint32_t r1; /* R1, an access's base, where not 0 */
} Witness;
#define F32(a, b) ((uint64_t)(b) << 32 | (a))
static const Witness witnesses[] = {
    /* VADD.F32 S0, S1, S2 */
    {"an add of normals, IXC set", 0xee300a81u, 0x10u, F32(0, 0x3f800000u), 0x40000000u, 0, 0, 0},
    {"an add of normals under FZ", 0xee300a81u, 0x01000010u, F32(0, 0x3f800000u), 0x40000000u, 0, 0, 0},
    {"an add under FZ whose exact zero stays hot", 0xee300a81u, 0x01000010u, F32(0, 0x3f800000u), 0xbf800000u, 0, 0, 0},
    {"an add with IXC clear", 0xee300a81u, 0, F32(0, 0x3f800000u), 0x40000000u, 0, 1, 0},
    {"an add rounding toward zero", 0xee300a81u, 0x00c00010u, F32(0, 0x3f800000u), 0x40000000u, 0, 1, 0},
    {"an add of a denormal", 0xee300a81u, 0x10u, F32(0, 0x3f800000u), 0x00000001u, 0, 1, 0},
    {"an add of a NaN", 0xee300a81u, 0x10u, F32(0, 0x7fc00000u), 0x40000000u, 0, 1, 0},
    {"an add with a trap enabled", 0xee300a81u, 0x110u, F32(0, 0x3f800000u), 0x40000000u, 0, 1, 0},
    /* VSUB, VMUL, VDIV, VMLA, VNMLS, VNMUL .F32 S0, S1, S2 */
    {"a subtraction", 0xee300ac1u, 0x10u, F32(0, 0x3f800000u), 0x40000000u, 0, 0, 0},
    {"a product", 0xee200a81u, 0x10u, F32(0, 0x3fc00000u), 0x40000000u, 0, 0, 0},
    {"a product under FZ", 0xee200a81u, 0x01000010u, F32(0, 0x3fc00000u), 0x40000000u, 0, 0, 0},
    {"a product that underflows", 0xee200a81u, 0x10u, F32(0, 0x00800000u), 0x3f000000u, 0, 1, 0},
    {"a product of a zero", 0xee200a81u, 0x10u, F32(0, 0x00000000u), 0x40000000u, 0, 0, 0},
    {"a quotient", 0xee800a81u, 0x10u, F32(0, 0x40400000u), 0x40000000u, 0, 0, 0},
    {"a quotient by zero", 0xee800a81u, 0x10u, F32(0, 0x40400000u), 0x00000000u, 0, 1, 0},
    {"a multiply-accumulate", 0xee000a81u, 0x10u, F32(0x3f800000u, 0x3fc00000u), 0x40000000u, 0, 0, 0},
    {"a multiply-accumulate of a denormal addend", 0xee000a81u, 0x10u, F32(0x00000001u, 0x3fc00000u), 0x40000000u, 0, 1,
     0},
    {"a negated multiply-subtract", 0xee100a81u, 0x10u, F32(0x3f800000u, 0x3fc00000u), 0x40000000u, 0, 0, 0},
    {"a negated product", 0xee200ac1u, 0x10u, F32(0, 0x3fc00000u), 0x40000000u, 0, 0, 0},
    /* VCMP.F32 S0, S2 and VCMPE.F32 S0, #0 */
    {"a compare, IXC clear", 0xeeb40a41u, 0, F32(0x3f800000u, 0), 0x40000000u, 0, 0, 0},
    {"a compare of a NaN", 0xeeb40a41u, 0, F32(0x7fc00000u, 0), 0x40000000u, 0, 1, 0},
    {"a compare with zero", 0xeeb50ac0u, 0x01000000u, F32(0xbf800000u, 0), 0, 0, 0, 0},
    {"a compare, greater", 0xeeb40a41u, 0x10u, F32(0x40000000u, 0), 0x3f800000u, 0, 0, 0},
    {"a compare, equal", 0xeeb40a41u, 0x10u, F32(0x3f800000u, 0), 0x3f800000u, 0, 0, 0},
    /* VADD, VMUL, VDIV .F64 D0, D1, D2 */
    {"a double add", 0xee310b02u, 0x10u, 0, 0x3ff0000000000000ull, 0x4000000000000000ull, 0, 0},
    {"a double product", 0xee210b02u, 0x10u, 0, 0x3ff8000000000000ull, 0x4000000000000000ull, 0, 0},
    {"a double quotient of a denormal", 0xee810b02u, 0x10u, 0, 0x0000000000000001ull, 0x4000000000000000ull, 1, 0},
    /* memory's last bytes: each access ending at the memory's last byte, and one past it (DATA_ABORT) */
    {"LDR R0, [R1] of the last word", 0xe5910000u, 0x10u, 0, 0, 0, 0, MEM_SIZE - 4},
    {"LDR R0, [R1] one byte past", 0xe5910000u, 0x10u, 0, 0, 0, 0, MEM_SIZE - 3},
    {"STR R0, [R1] of the last word", 0xe5810000u, 0x10u, 0, 0, 0, 0, MEM_SIZE - 4},
    {"STR R0, [R1] one byte past", 0xe5810000u, 0x10u, 0, 0, 0, 0, MEM_SIZE - 3},
    {"LDRH R0, [R1] of the last halfword", 0xe1d100b0u, 0x10u, 0, 0, 0, 0, MEM_SIZE - 2},
    {"LDRH R0, [R1] one byte past", 0xe1d100b0u, 0x10u, 0, 0, 0, 0, MEM_SIZE - 1},
    {"LDRB R0, [R1] of the last byte", 0xe5d10000u, 0x10u, 0, 0, 0, 0, MEM_SIZE - 1},
    {"LDRB R0, [R1] one byte past", 0xe5d10000u, 0x10u, 0, 0, 0, 0, MEM_SIZE},
    {"LDRD R2, R3, [R1] of the last two words", 0xe1c120d0u, 0x10u, 0, 0, 0, 0, MEM_SIZE - 8},
    {"LDRD R2, R3, [R1] a word past", 0xe1c120d0u, 0x10u, 0, 0, 0, 0, MEM_SIZE - 4},
    {"LDM R1, {R2-R5} of the last four words", 0xe891003cu, 0x10u, 0, 0, 0, 0, MEM_SIZE - 16},
    {"LDM R1, {R2-R5} a word past", 0xe891003cu, 0x10u, 0, 0, 0, 0, MEM_SIZE - 12},
    {"VLDR D0, [R1] of the last two words", 0xed910b00u, 0x10u, 0, 0, 0, 0, MEM_SIZE - 8},
    {"VLDR D0, [R1] a word past", 0xed910b00u, 0x10u, 0, 0, 0, 0, MEM_SIZE - 4},
    /* Advanced SIMD's saturations of the most negative lanes, which random lanes rarely are, and the permutations */
    {"VQDMULH.S16 D0, D1, D2 of -32768 by -32768", 0xf2110b02u, 0x10u, 0, 0x80008000800021c3ull, 0x80007fff8000a5a5ull,
     0, 0},
    {"VQDMULH.S32 D0, D1, D2 of -2^31 by -2^31", 0xf2210b02u, 0x10u, 0, 0x8000000080000000ull, 0x800000007fffffffull, 0,
     0},
    {"VQRDMULH.S16 D0, D1, D2 of -32768 by -32768", 0xf3110b02u, 0x10u, 0, 0x8000800080000001ull, 0x8000ffff80008000ull,
     0, 0},
    {"VQDMULL.S32 Q0, D1, D2 of -2^31 by -2^31", 0xf2a10d02u, 0x10u, 0, 0x8000000080000000ull, 0x8000000012345678ull, 0,
     0},
    {"VQDMLAL.S32 Q0, D2, D2 past either end", 0xf2a20902u, 0x10u, 1, 0x7ffffffffffffffcull, 0x0000000280000000ull, 0,
     0},
    {"VZIP.8 D0, D1", 0xf3b20181u, 0x10u, 0x0706050403020100ull, 0x1716151413121110ull, 0, 0, 0},
    {"VUZP.8 D0, D1", 0xf3b20101u, 0x10u, 0x0706050403020100ull, 0x1716151413121110ull, 0, 0, 0},
    {"VTRN.8 D0, D1", 0xf3b20081u, 0x10u, 0x0706050403020100ull, 0x1716151413121110ull, 0, 0, 0},
};

static int run_witnesses(SnxIr* ir, Back* x64) {
    size_t count = sizeof witnesses / sizeof *witnesses, failures = 0;
    Block* blocks = (Block*)calloc(count, sizeof *blocks);
    LLVMModuleRef module = LLVMModuleCreateWithNameInContext("witnesses", ir->ctx);
    SnxCodeLoader *lloader = NULL, *xloader = NULL;
    unsigned char *lobj = NULL, *xobj = NULL;
    size_t lsize = 0, xsize = 0;
    SnxU64 *laddr = calloc(count, sizeof *laddr), *xaddr = calloc(count, sizeof *xaddr);
    RunState* s = (RunState*)malloc(2 * sizeof *s);
    char error[512];
    if (!blocks || !laddr || !xaddr || !s) {
        fprintf(stderr, "FAIL: out of memory for the %zu witnesses\n", count);
        free(blocks), free(laddr), free(xaddr), free(s);
        LLVMDisposeModule(module);
        return 1;
    }
    LLVMSetTarget(module, LLVMGetTarget(ir->mod));
    LLVMSetDataLayout(module, LLVMGetDataLayoutStr(ir->mod));
    for (size_t i = 0; i < count; ++i) {
        char name[16];
        size_t consumed;
        snprintf(name, sizeof name, "B_%zu", i);
        blocks[i].words[0] = witnesses[i].word;
        blocks[i].n = 1;
        if (!snx_arm_llvm_block(module, name, blocks[i].words, 1, PC, &consumed) ||
            !(blocks[i].made =
                  back_(block)(x64, name, (const unsigned char*)blocks[i].words, 1, PC, &consumed, NULL, NULL))) {
            fprintf(stderr, "FAIL: the witness %s (%08x): a back end refused it\n", witnesses[i].what,
                    witnesses[i].word);
            return 1;
        }
    }
    if (snx_ir_module_object(ir, module, '0', &lobj, &lsize, error, sizeof error) ||
        back_(object)(x64, &xobj, &xsize, NULL) || !(lloader = new_loader(error, sizeof error)) ||
        !(xloader = new_loader(error, sizeof error)) || load(lloader, lobj, lsize, "LLVM", blocks, count, 0, laddr) ||
        load(xloader, xobj, xsize, BACK_NAME, blocks, count, 1, xaddr)) {
        fprintf(stderr, "FAIL: the witnesses' objects: %s\n", error);
        return 1;
    }
    for (size_t i = 0; i < count; ++i) {
        const Witness* w = &witnesses[i];
        size_t before = 0, after = 0;
        const char* why;
        u64 seed = g_test.seed;
        for (int side = 0; side < 2; ++side) {
            g_test.seed = seed;
            start(&s[side], 1);
            s[side].cpu.cpsr = 0x10u;
            s[side].cpu.fpscr = w->fpscr;
            s[side].cpu.d[0] = w->d0;
            s[side].cpu.d[1] = w->d1;
            s[side].cpu.d[2] = w->d2;
            if (w->r1) s[side].cpu.r[1] = w->r1;
            for (unsigned k = 0; k < 256; ++k) before += side ? g_test.fp_calls[k] : 0;
            run_block(side ? xaddr[i] : laddr[i], &s[side].cpu);
            for (unsigned k = 0; k < 256; ++k) after += side ? g_test.fp_calls[k] : 0;
        }
        why = differs(&s[0], &s[1]);
        if (*why) {
            fprintf(stderr, "FAIL: the witness %s (%08x): %s\n", w->what, w->word, why);
            ++failures;
        }
        if ((int)(after - before) != w->cold) {
            fprintf(stderr, "FAIL: the witness %s (%08x): %zu calls of the helper, %d wanted\n", w->what, w->word,
                    after - before, w->cold);
            ++failures;
        }
    }
    /* the entry's checks (block_begin's): R15 not the block's address, CPSR.T, CPSR.E, both; R15 and the rest as LLVM's
       leave them */
    for (unsigned e = 0; e < 4; ++e) {
        static const char* const what[4] = {"R15 elsewhere", "CPSR.T set", "CPSR.E set", "CPSR.T and E set"};
        u64 seed = g_test.seed;
        const char* why;
        for (int side = 0; side < 2; ++side) {
            g_test.seed = seed;
            start(&s[side], 1);
            if (e == 0) s[side].cpu.r[15] = PC + 4;
            if (e & 1 && e) s[side].cpu.cpsr |= SNX_ARM_T;
            if (e >= 2) s[side].cpu.cpsr |= SNX_ARM_E;
            run_block(side ? xaddr[0] : laddr[0], &s[side].cpu);
        }
        why = differs(&s[0], &s[1]);
        if (*why || s[0].cpu.exit == SNX_ARM_OK) {
            fprintf(stderr, "FAIL: the entry with %s: %s (LLVM's exit %u)\n", what[e], *why ? why : "no exit",
                    s[0].cpu.exit);
            ++failures;
        }
    }
    printf("%zu witnesses (the VFP's hot paths, memory's last bytes) and 4 of the entry's checks: %zu failures\n",
           count, failures);
    g_test.failures += failures;
    for (unsigned k = 0; k < 256; ++k) g_test.fp_calls[k] = 0;
    free(s), free(lobj), free(xobj), free(laddr), free(xaddr), free(blocks);
    snx_code_loader_free(lloader);
    snx_code_loader_free(xloader);
    LLVMDisposeModule(module);
    return 0;
}

/* ---- an MSR that may set CPSR.E: its block ends there, and the next is entered through its checks (llvm.c's
   block_begin), which stop on E with that block's first word; the entry past them (X) would run it as little-endian */
static size_t endian_next(void* user, uint32_t key, size_t units, size_t count, uint32_t keys[2]) {
    (void)user, (void)units, (void)count;
    if (key != PC) return 0;
    keys[0] = PC + 4;
    return 1;
}
static int run_endian_chain(Back* x64) {
    static const uint32_t msr[1] = {0xe122f000u}, mov[1] = {0xe3a01001u}; /* MSR CPSR_x, R0 (E its bit 9); MOV R1, #1 */
    unsigned char* object = NULL;
    size_t consumed = 0, size = 0, failures = 0;
    char error[512] = "", name[16];
    snprintf(name, sizeof name, "A_%08x", PC);
    SnxCodeLoader* loader = NULL;
    if (!back_(block)(x64, NULL, (const unsigned char*)msr, 1, PC, &consumed, endian_next, NULL) ||
        !back_(block)(x64, NULL, (const unsigned char*)mov, 1, PC + 4, &consumed, endian_next, NULL) ||
        back_(object)(x64, &object, &size, NULL) || !(loader = new_loader(error, sizeof error))) {
        fprintf(stderr, "FAIL: the MSR's chain: %s%s\n", back_(why)(x64), error);
        return 1;
    }
    SnxLinkInput input = {.data = object, .size = size, .name = "chain"};
    const char* names[1] = {name};
    SnxU64 addr = 0;
    RunState* s = (RunState*)malloc(sizeof *s);
    if (!s || snx_code_load_private(loader, &input, 1, names, 1, &addr, error, sizeof error)) {
        fprintf(stderr, "FAIL: the MSR's chain's object: %s\n", error);
        free(s), free(object);
        snx_code_loader_free(loader);
        return 1;
    }
    for (int e = 0; e < 2; ++e) {
        start(s, 1);
        s->cpu.cpsr = 0x10u;
        s->cpu.r[0] = e ? SNX_ARM_E : 0;
        s->cpu.r[1] = 0;
        run_block(addr, &s->cpu);
        /* E set: the MOV's block stops at its entry, R15 its address; clear: the MOV runs, R15 past it */
        uint32_t exit = e ? SNX_ARM_ENDIAN_UNSUPPORTED : SNX_ARM_OK, r1 = e ? 0 : 1, r15 = e ? PC + 4 : PC + 8;
        if (s->cpu.exit != exit || (e && s->cpu.syndrome != mov[0]) || s->cpu.r[1] != r1 || s->cpu.r[15] != r15) {
            fprintf(
                stderr,
                "FAIL: the MSR's chain with E %s: exit %u, syndrome %08x, R1 %u, R15 %08x; wanted exit %u, %s R1 %u, "
                "R15 %08x\n",
                e ? "set" : "clear", s->cpu.exit, s->cpu.syndrome, s->cpu.r[1], s->cpu.r[15], exit,
                e ? "syndrome e3a01001," : "", r1, r15);
            ++failures;
        }
    }
    printf("an MSR's block and the next, E set and clear: %zu failures\n", failures);
    g_test.failures += failures;
    free(s), free(object);
    snx_code_loader_free(loader);
    return 0;
}

/* VRSHL.S8 by the Arm ARM: (x + 2^(n-1)) >> n for a shift of -n, x << s for one of s, the lane's low 8 bits; past 9
   to the right every lane's sum is below 2^n, so 0 as at 9 */
static uint8_t rshl8(int8_t x, int8_t s) {
    if (s >= 0) return s >= 8 ? 0 : (uint8_t)((unsigned)(uint8_t)x << s);
    unsigned n = (unsigned)(-(int)s);
    if (n > 9) n = 9;
    return (uint8_t)snx_asr64((uint64_t)((long long)x + (1ll << (n - 1))), n);
}
/* VRSHL.S8 Q6, Q14, Q3 against rshl8, not LLVM: each lane's shift 0, +-1, -8 and -9, 7 and 8, -128 and 127, and a lane
   that shifts by 0 beside one that shifts by -128 in one doubleword, which LLVM 22 at -O2 gives the other's (its lane
   shifted right by 1) */
static int run_vrshl_witness(Back* x64) {
    static const uint32_t word[1] = {0xf206c56cu};
    static const int8_t shift[16] = {0, -1, 1, -8, -9, 7, 8, 127, -128, 3, -2, 0, -7, 0, 0, -128};
    static const int8_t value[16] = {0x67, -27, 127, -128, 1, -1, 64, -64, 100, -100, 3, -3, 0x22, 0x67, 0x40, -27};
    unsigned char* object = NULL;
    size_t consumed = 0, size = 0, failures = 0;
    char error[512] = "", name[16];
    snprintf(name, sizeof name, "A_%08x", PC);
    SnxCodeLoader* loader = NULL;
    if (!back_(block)(x64, NULL, (const unsigned char*)word, 1, PC, &consumed, NULL, NULL) ||
        back_(object)(x64, &object, &size, NULL) || !(loader = new_loader(error, sizeof error))) {
        fprintf(stderr, "FAIL: VRSHL's witness: %s%s\n", back_(why)(x64), error);
        return 1;
    }
    SnxLinkInput input = {.data = object, .size = size, .name = "vrshl"};
    const char* names[1] = {name};
    SnxU64 addr = 0;
    RunState* s = (RunState*)malloc(sizeof *s);
    if (!s || snx_code_load_private(loader, &input, 1, names, 1, &addr, error, sizeof error)) {
        fprintf(stderr, "FAIL: VRSHL's witness's object: %s\n", error);
        free(s), free(object);
        snx_code_loader_free(loader);
        return 1;
    }
    start(s, 1);
    s->cpu.cpsr = 0x10u;
    for (unsigned i = 0; i < 16; ++i) {
        unsigned byte = i % 8 * 8;
        s->cpu.d[6 + i / 8] = (s->cpu.d[6 + i / 8] & ~(0xffull << byte)) | (uint64_t)(uint8_t)shift[i] << byte;
        s->cpu.d[28 + i / 8] = (s->cpu.d[28 + i / 8] & ~(0xffull << byte)) | (uint64_t)(uint8_t)value[i] << byte;
    }
    run_block(addr, &s->cpu);
    for (unsigned i = 0; i < 16; ++i) {
        uint8_t got = (uint8_t)(s->cpu.d[12 + i / 8] >> (i % 8 * 8)), want = rshl8(value[i], shift[i]);
        if (got != want) {
            fprintf(stderr, "FAIL: VRSHL.S8 of %d by %d: %02x, the Arm ARM's %02x\n", value[i], shift[i], got, want);
            ++failures;
        }
    }
    printf("VRSHL.S8 by the Arm ARM, 16 lanes: %zu failures\n", failures);
    g_test.failures += failures;
    free(s), free(object);
    snx_code_loader_free(loader);
    return 0;
}

/* ---- Thumb's witnesses: the entry's checks (thumb_block's), and the accesses the block compilers don't agree on
   without care ---- */

typedef struct {
    const char* what;
    uint16_t half[6];
    size_t n;
    uint32_t r0, r1, cpsr, r15;
    /* CPSR_SET: CPSR is cpsr; R15_SET: R15 is r15; REGS_SET: R0 and R1 are r0 and r1; else as start makes them, CPSR
       with T set */
    int set;
    int exit; /* the exit wanted, ANY: the two runs are only compared, from 16 states */
} ThumbCase;
#define CPSR_SET 1
#define R15_SET  2
#define REGS_SET 4
#define ANY      (-1)
/* CPSR's bits in these: T 0x20, E 0x200, ITSTATE's 0x0600FC00; the mode 0x10 */
static const ThumbCase thumb_cases[] = {
    /* the entry: R15 not the block's address, T clear, ITSTATE, E, and their combinations (R15 first, then T, ITSTATE,
       E) */
    {"the entry, as it should be", {0xBF00}, 1, 0, 0, 0, 0, 0, SNX_ARM_OK},
    {"the entry with R15 elsewhere", {0xBF00}, 1, 0, 0, 0, PC + 2, R15_SET, SNX_ARM_BAD_ENTRY},
    {"the entry with T clear", {0xBF00}, 1, 0, 0, 0x10, 0, CPSR_SET, SNX_ARM_BAD_ENTRY},
    {"the entry with ITSTATE in CPSR", {0xBF00}, 1, 0, 0, 0x430, 0, CPSR_SET, SNX_ARM_IT_ENTRY},
    {"the entry with ITSTATE's low bits", {0xBF00}, 1, 0, 0, 0x06000030, 0, CPSR_SET, SNX_ARM_IT_ENTRY},
    {"the entry with E set", {0xBF00}, 1, 0, 0, 0x230, 0, CPSR_SET, SNX_ARM_ENDIAN_UNSUPPORTED},
    {"the entry with T clear, ITSTATE and E", {0xBF00}, 1, 0, 0, 0x610, 0, CPSR_SET, SNX_ARM_BAD_ENTRY},
    {"the entry with ITSTATE and E", {0xBF00}, 1, 0, 0, 0x8230, 0, CPSR_SET, SNX_ARM_IT_ENTRY},
    {"the entry, R15 elsewhere and T clear", {0xBF00}, 1, 0, 0, 0x10, PC + 2, CPSR_SET | R15_SET, SNX_ARM_BAD_ENTRY},
    /* TBB [R0, R1] and TBH [R0, R1, LSL #1] whose table entry is past memory: a data abort, as a load's */
    {"TBB of a byte at the memory's last one", {0xE8D0, 0xF001}, 2, MEM_SIZE - 1, 0, 0, 0, REGS_SET, SNX_ARM_OK},
    {"TBB past memory", {0xE8D0, 0xF001}, 2, MEM_SIZE, 0, 0, 0, REGS_SET, SNX_ARM_DATA_ABORT},
    {"TBH past memory", {0xE8D0, 0xF011}, 2, MEM_SIZE - 2, 1, 0, 0, REGS_SET, SNX_ARM_DATA_ABORT},
    /* the compares without a result (Rd 15), and the reserved ones, whose flags are the subtraction's and the addition's */
    {"TST.W R0, R1", {0xEA10, 0x0F01}, 2, 0, 0, 0, 0, 0, ANY},
    {"TEQ.W R0, R1", {0xEA90, 0x0F01}, 2, 0, 0, 0, 0, 0, ANY},
    {"CMN.W R0, R1", {0xEB10, 0x0F01}, 2, 0, 0, 0, 0, 0, ANY},
    {"CMP.W R0, R1", {0xEBB0, 0x0F01}, 2, 0, 0, 0, 0, 0, ANY},
    {"ADCS.W of Rd 15", {0xEB50, 0x0F01}, 2, 0, 0, 0, 0, 0, ANY},
    {"SBCS.W of Rd 15", {0xEB70, 0x0F01}, 2, 0, 0, 0, 0, 0, ANY},
    /* a logical operation's carry: LSL #0 leaves C, RRX takes it */
    {"ANDS.W R0, R0, R1", {0xEA10, 0x0001}, 2, 0, 0, 0, 0, 0, ANY},
    {"BICS.W R0, R0, R1", {0xEA30, 0x0001}, 2, 0, 0, 0, 0, 0, ANY},
    {"MOVS.W R0, R1", {0xEA5F, 0x0001}, 2, 0, 0, 0, 0, 0, ANY},
    {"ORNS.W R0, R0, R1", {0xEA70, 0x0001}, 2, 0, 0, 0, 0, 0, ANY},
    {"EORS.W R0, R0, R1", {0xEA90, 0x0001}, 2, 0, 0, 0, 0, 0, ANY},
    {"RRXS.W R0, R1", {0xEA5F, 0x0031}, 2, 0, 0, 0, 0, 0, ANY},
    {"MOVS.W R0, #0x80000000, its carry the modified immediate's", {0xF05F, 0x4000}, 2, 0, 0, 0, 0, 0, ANY},
    /* a register's shift by a multiple of 256: an amount of 0 leaves the value and C */
    {"LSLS.W R0, R0, R1 by 0x100", {0xFA10, 0xF001}, 2, 0x80000001, 0x100, 0, 0, REGS_SET, ANY},
    {"LSRS.W R0, R0, R1 by 0x100", {0xFA30, 0xF001}, 2, 0x80000001, 0x100, 0, 0, REGS_SET, ANY},
    {"ASRS.W R0, R0, R1 by 0x100", {0xFA50, 0xF001}, 2, 0x80000001, 0x100, 0, 0, REGS_SET, ANY},
    {"RORS.W R0, R0, R1 by 0x100", {0xFA70, 0xF001}, 2, 0x80000001, 0x100, 0, 0, REGS_SET, ANY},
    /* Align(PC, 4) read by an A32 instruction's twin, at an address 2 mod 4 */
    {"NOP; VLDR D0, [PC, #4]", {0xBF00, 0xED9F, 0x0B01}, 3, 0, 0, 0, 0, 0, SNX_ARM_OK},
    /* a doubleword's second register, any in Thumb: STREXD stores what LDREXD read, where Rt2 is not Rt + 1 */
    {"LDREXD and STREXD of R0 and R3", {0xE8D1, 0x037F, 0xE8C1, 0x0372}, 4, 0, 0x100, 0, 0, REGS_SET, SNX_ARM_OK},
    /* an UNDEFINED word under an IT condition that always holds (the AL block's 1111): the block goes on past it */
    {"IT AL of an UNDEFINED word, then a POP", {0xBFED, 0x49E9, 0xEB92, 0x2741, 0xBD1B}, 5, 0, 0, 0, 0, 0, ANY},
};
static int run_thumb_witnesses(SnxIr* ir, Back* x64) {
    size_t count = sizeof thumb_cases / sizeof *thumb_cases, failures = 0;
    Block* blocks = (Block*)calloc(count, sizeof *blocks);
    LLVMModuleRef module = LLVMModuleCreateWithNameInContext("thumb witnesses", ir->ctx);
    SnxCodeLoader *lloader = NULL, *xloader = NULL;
    unsigned char *lobj = NULL, *xobj = NULL;
    size_t lsize = 0, xsize = 0;
    SnxU64 *laddr = calloc(count, sizeof *laddr), *xaddr = calloc(count, sizeof *xaddr);
    RunState* s = (RunState*)malloc(2 * sizeof *s);
    char error[512];
    if (!blocks || !laddr || !xaddr || !s) {
        fprintf(stderr, "FAIL: out of memory for the %zu Thumb witnesses\n", count);
        free(blocks), free(laddr), free(xaddr), free(s);
        LLVMDisposeModule(module);
        return 1;
    }
    LLVMSetTarget(module, LLVMGetTarget(ir->mod));
    LLVMSetDataLayout(module, LLVMGetDataLayoutStr(ir->mod));
    for (size_t i = 0; i < count; ++i) {
        Block* b = &blocks[i];
        size_t consumed, xconsumed;
        char name[16];
        b->thumb = 1;
        b->n = thumb_cases[i].n;
        memcpy(b->half, thumb_cases[i].half, sizeof thumb_cases[i].half);
        snprintf(name, sizeof name, "B_%zu", i);
        if (!snx_thumb_llvm_block(module, name, b->half, b->n, PC, &consumed) ||
            !(b->made = snx_x64_thumb_block(x64, name, code_of(b), b->n, PC, &xconsumed, NULL, NULL))) {
            fprintf(stderr, "FAIL: the Thumb witness %s: a back end refused it: %s\n", thumb_cases[i].what,
                    back_(why)(x64));
            return 1;
        }
        if (consumed != xconsumed) {
            fprintf(stderr, "FAIL: the Thumb witness %s: LLVM's block takes %zu halfwords, the x86-64 back end's %zu\n",
                    thumb_cases[i].what, consumed, xconsumed);
            ++failures;
        }
    }
    if (snx_ir_module_object(ir, module, '0', &lobj, &lsize, error, sizeof error) ||
        back_(object)(x64, &xobj, &xsize, NULL) || !(lloader = new_loader(error, sizeof error)) ||
        !(xloader = new_loader(error, sizeof error)) || load(lloader, lobj, lsize, "LLVM", blocks, count, 0, laddr) ||
        load(xloader, xobj, xsize, BACK_NAME, blocks, count, 1, xaddr)) {
        fprintf(stderr, "FAIL: the Thumb witnesses' objects: %s\n", error);
        return 1;
    }
    for (size_t i = 0; i < count; ++i) {
        const ThumbCase* w = &thumb_cases[i];
        for (unsigned k = 0; k < (w->exit == ANY ? 16u : 1u); ++k) {
            u64 seed = g_test.seed;
            for (int side = 0; side < 2; ++side) {
                g_test.seed = seed;
                start(&s[side], w->exit == ANY ? k : 1);
                s[side].cpu.cpsr = w->set & CPSR_SET ? w->cpsr : s[side].cpu.cpsr | SNX_ARM_T;
                if (w->set & R15_SET) s[side].cpu.r[15] = w->r15;
                if (w->set & REGS_SET) s[side].cpu.r[0] = w->r0, s[side].cpu.r[1] = w->r1;
                run_block(side ? xaddr[i] : laddr[i], &s[side].cpu);
            }
            const char* why = differs(&s[0], &s[1]);
            if (*why || (w->exit != ANY && s[0].cpu.exit != (uint32_t)w->exit)) {
                fprintf(stderr, "FAIL: the Thumb witness %s, state %u: %s (LLVM's exit %u, wanted %d)\n", w->what, k,
                        why, s[0].cpu.exit, w->exit);
                ++failures;
                break;
            }
        }
    }
    printf("%zu Thumb witnesses (the entry's checks, TBB and TBH past memory, the compares and the carries): %zu "
           "failures\n",
           count, failures);
    g_test.failures += failures;
    free(s), free(lobj), free(xobj), free(laddr), free(xaddr), free(blocks);
    snx_code_loader_free(lloader);
    snx_code_loader_free(xloader);
    LLVMDisposeModule(module);
    return 0;
}
#endif

int main(int argc, char** argv) {
    char error[512];
    SnxIr* ir = NULL;
    g_test.states = 16;
    g_test.blocks = 6000;
#if !defined(SNX_TEST_NATIVE)
    g_test.thumb = 4000;
#endif
    g_test.features = ~0u;
    g_test.seed = 0xa32;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--states") && i + 1 < argc)
            g_test.states = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--blocks") && i + 1 < argc)
            g_test.blocks = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--thumb") && i + 1 < argc)
            g_test.thumb = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--pool") && i + 1 < argc)
            g_test.pool = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--features") && i + 1 < argc)
            g_test.features = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc)
            g_test.seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--object") && i + 1 < argc)
            g_test.object = argv[++i];
        else if (!strcmp(argv[i], "--ll") && i + 1 < argc)
            g_test.ll = argv[++i];
        else if (!strcmp(argv[i], "--dump"))
            g_test.dump = 1;
        else if (!strcmp(argv[i], "--word") && i + 1 < argc && g_test.nonly < 64) {
            const char* w = argv[++i];
            size_t n = 0;
            for (char* end; n < MAX_WORDS && *w; w = *end == ',' ? end + 1 : end) {
                g_test.only[g_test.nonly][n++] = (uint32_t)strtoul(w, &end, 16);
                if (end == w) break;
            }
            g_test.only_n[g_test.nonly++] = n;
        } else if (!strcmp(argv[i], "--halves") && i + 1 < argc) {
            char* at = argv[++i];
            while (g_test.nhalves < MAX_HALF) {
                g_test.halves[g_test.nhalves++] = (uint16_t)strtoul(at, &at, 16);
                if (*at != ',') break;
                ++at;
            }
        } else
            return fprintf(stderr,
                           "usage: snx-test-arm-x64 [--blocks N] [--thumb N] [--states N] [--pool N] [--features MASK] "
                           "[--seed N] [--object FILE] [--word HEX]... [--halves HEX[,HEX]...]\n%s is not one of "
                           "these\n",
                           argv[i]),
                   2;
    }
#if defined(SNX_TEST_NATIVE)
    {
        HostUc* probe = hostuc_new(BACK_UC, 1u << 16);
        if (!probe) {
            printf("SKIP: no Unicorn " BACK_NAME "\n");
            return 77;
        }
        hostuc_free(probe);
    }
    if (g_test.thumb || g_test.nhalves)
        return fprintf(stderr,
                       "FAIL: --thumb %u, --halves of %zu: the " BACK_NAME " back end makes no Thumb blocks yet\n",
                       g_test.thumb, g_test.nhalves),
               2;
#else
    if (!back_(available)()) {
        printf("SKIP: no " BACK_NAME " back end on this host\n");
        return 77;
    }
#endif
    if (snx_ir_init(error, sizeof error) || !(ir = snx_ir_new_jit("arm-x64", '2', error, sizeof error))) {
        fprintf(stderr, "FAIL: no LLVM for this host: %s\n", error);
        return 1;
    }
    Back* x64 = back_(new)();
    Block* blocks = (Block*)calloc(CHUNK, sizeof *blocks);
    if (!x64 || !blocks) {
        fprintf(stderr, "FAIL: out of memory for the " BACK_NAME " back end's compiler or the blocks\n");
        return 1;
    }
#if defined(SNX_TEST_NATIVE)
    back_(limit)(x64, g_test.pool, ~0u);
    size_t done = 0;
#else
    back_(limit)(x64, g_test.pool, g_test.features | ~7u);
    size_t done = 0;
    if (!g_test.nonly && !g_test.nhalves && run_witnesses(ir, x64)) ++g_test.failures;
    if (!g_test.nonly && !g_test.nhalves && run_endian_chain(x64)) ++g_test.failures;
    if (!g_test.nonly && !g_test.nhalves && run_vrshl_witness(x64)) ++g_test.failures;
    if (!g_test.nonly && !g_test.nhalves && run_thumb_witnesses(ir, x64)) ++g_test.failures;
#endif
    if (g_test.nhalves) {
        blocks[0].thumb = 1;
        blocks[0].n = g_test.nhalves;
        memcpy(blocks[0].half, g_test.halves, sizeof blocks[0].half);
        g_test.nonly = 1; /* each state printed */
        run_chunk(ir, x64, blocks, 1);
        done = 1;
    } else if (g_test.nonly) {
        for (size_t i = 0; i < g_test.nonly; ++i) {
            memcpy(blocks[i].words, g_test.only[i], sizeof blocks[i].words);
            blocks[i].n = g_test.only_n[i];
        }
        run_chunk(ir, x64, blocks, g_test.nonly);
        done = g_test.nonly;
    }
    while (!g_test.nonly && done < g_test.blocks) {
        size_t count = g_test.blocks - done < CHUNK ? g_test.blocks - done : CHUNK;
        for (size_t i = 0; i < count; ++i) {
            Block* b = &blocks[i];
            *b = (Block){0};
            /* one word alone a third of the time, else two to six */
            b->n = rnd() % 3 == 0 ? 1 : 2 + (size_t)(rnd() % (MAX_WORDS - 1));
            for (size_t k = 0; k < b->n; ++k) {
                const Class* c = pick_class();
                b->words[k] = word_of(c);
                ++g_test.kinds[c->kind & 63];
            }
        }
        if (run_chunk(ir, x64, blocks, count)) break;
        done += count;
    }
    size_t thumb_done = 0;
    while (!g_test.nonly && !g_test.nhalves && thumb_done < g_test.thumb) {
        size_t count = g_test.thumb - thumb_done < CHUNK ? g_test.thumb - thumb_done : CHUNK;
        for (size_t i = 0; i < count; ++i) {
            blocks[i] = (Block){0};
            random_thumb(&blocks[i]);
            count_shapes(&blocks[i]);
        }
        if (run_chunk(ir, x64, blocks, count)) break;
        thumb_done += count;
    }
    if (thumb_done) {
        static const char* const names[32] = {
            [TH_UNDEF] = "undefined", [TH_LOAD] = "LDR",          [TH_STORE] = "STR",    [TH_BRANCH] = "branch",
            [TH_MUL] = "MUL",         [TH_BITFIELD] = "bitfield", [TH_WIMM] = "MOVW",    [TH_WDATA] = "data",
            [TH_TABLE] = "TBB",       [TH_HINT] = "hint",         [TH_A32] = "A32 twin", [TH_DUAL] = "LDRD"};
        printf("%zu Thumb blocks of %zu 16-bit instructions and, by the decoder's shape,", thumb_done, g_test.t16);
        for (size_t k = 0; k < 32; ++k)
            if (g_test.shapes[k] && names[k]) printf(" %s %zu", names[k], g_test.shapes[k]);
        printf("\n");
    }
    printf("%zu blocks of %zu words (%zu in the blocks LLVM made of them), %zu compared from %u states each: %zu runs, "
           "%zu refused by the " BACK_NAME " back end\n",
           done, g_test.words, g_test.consumed_words, g_test.compared, g_test.states, g_test.ran, g_test.refused);
    {
        static const SnxArmVectorOp hot[] = {AV_FADD,  AV_FSUB,  AV_FMUL,  AV_FDIV, AV_FMLA, AV_FMLS,
                                             AV_FNMLA, AV_FNMLS, AV_FNMUL, AV_FCMP, AV_FCMPE};
        size_t cold = 0, other = 0;
        for (unsigned k = 0; k < 256; ++k) other += g_test.fp_calls[k];
        for (size_t k = 0; k < sizeof hot / sizeof *hot; ++k) cold += g_test.fp_calls[hot[k]];
        printf("snx_arm_fp_x64: %zu calls of the hot paths' operations (their cold paths), %zu of the others\n", cold,
               other - cold);
    }
    printf("their exits:");
    for (unsigned e = 0; e < 16; ++e)
        if (g_test.exits[e])
            printf(" %u (%s) %zu", e,
                   e == SNX_ARM_OK           ? "on"
                   : e == SNX_ARM_UNDEFINED  ? "UNDEFINED"
                   : e == SNX_ARM_SVC        ? "SVC"
                   : e == SNX_ARM_BREAKPOINT ? "BKPT"
                   : e == SNX_ARM_DATA_ABORT ? "DATA_ABORT"
                   : e == SNX_ARM_ALIGNMENT  ? "ALIGNMENT"
                                             : "other",
                   g_test.exits[e]);
    printf("\n");
    free(blocks);
    back_(free)(x64);
    snx_ir_free(ir);
    if (g_test.failures) printf("%zu failures\n", g_test.failures);
    return g_test.failures != 0;
}
