/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The x86-64 back end's A32 blocks (src/common/cpu/recompiler/x64/x64_arm.c), or built with SNX_TEST_ARM64 or
   SNX_TEST_RV64 the arm64 or riscv64 back end's (run under Unicorn: snx-test-arm-arm64, -rv64, without the hot paths'
   witnesses and the chain), or with SNX_TEST_WIN64 the x86-64 one's for Windows (its calling convention, run by an
   x86-64 host itself: support/host_win64.c), or with SNX_TEST_ARM64WIN beside SNX_TEST_ARM64 the arm64 one's for
   Windows, against the AArch32 block compiler's LLVM ones (snx_arm_llvm_block, which
   module_arm.c makes a 32-bit program's blocks with), at -O0: their IR as it is, which LLVM 22's -O2 makes otherwise in
   one case (VRSHL's witness, below): the same words consumed, and the same results from the same random states.
     snx-test-arm-x64 [--profile a57|a9|arm11|arm946e|arm7tdmi] [--memory flat|hosted|checked] [--trace] [--blocks N]
         [--thumb N] [--states N] [--pool N] [--features MASK] [--seed N] [--word HEX]... [--halves HEX[,HEX]...]
         [--dump] [--step]
   --step (Windows' blocks): the stack unwound by the objects' function table at every instruction of the blocks. Each
   block is words drawn from the A32 instructions the back end makes (data processing, branches, the loads and
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
   state did; --dump prints a failing state's start.
   --profile and --memory are the options' (cpu/arm/codegen.h), as LLVM's blocks take them; the words are drawn from
   what the profile decodes, UNDEFINED ones now and then. Hosted memory and the ARM7TDMI's and the ARM946E's bus run on
   a window of all 4 GiB and a guard, 256 KiB of memory mirrored through it; their hook (SnxArmCPU.io) and checked
   memory's callback are the test's, whose every call, with the state as it was then, must be the same on both sides:
   the hook's writes end the block one time in four, the callback faults, changes a fetched word and loses a reservation
   now and then. --trace keeps the ARM7TDMI's trace and skipped bits, which must match too.
   --callee OBJECT (under Unicorn alone) is the runtime library's functions a block calls, compiled for the machine
   (arm_call_abi_callee.c), in the place of the host's: with it the runs have a runtime (SnxArmRt) whose callbacks are
   that object's on one side and the test's on the other, so an argument a block passes as the machine's ABI does not
   have it shows, and the hook (SnxArmCPU.io) is that object's too */
#include "common/cpu/arm/llvm.h"
#include "common/cpu/arm/simd.h"
#include "common/cpu/recompiler/loader/code_loader.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "support/arm_back.h"
#if !defined(SNX_TEST_NATIVE)
#include "common/cpu/recompiler/x64/x64_elf.h"
#endif
#include "common/utilities/bits.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "support/arm_rt_callbacks.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <sys/mman.h>
#include <unistd.h>
#endif

typedef uint64_t u64;

/* each object goes into a loader of its own, so its names are only its own either way: COFF objects' are published */
#if defined(_WIN32)
#define load_object snx_code_load
#else
#define load_object snx_code_load_private
#endif

#define PC        0x1000u /* the blocks' address */
#define MEM_SIZE  0x10000u
#define CHUNK     256
#define MAX_WORDS 48
#define MAX_HALF  16 /* halfwords of a Thumb block */
/* the window of the modes that reach past MEM_SIZE: VIEW bytes of memory mirrored through 4 GiB and a guard */
#define VIEW      0x40000u
#define WINDOW    ((1ull << 32) + VIEW)

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
#if defined(SNX_TEST_UNWINDS)
    int step; /* --step: hostuc_step */
#endif
    /* the x86-64 blocks' calls of snx_arm_fp_x64, by op: the VFP's hot paths leave to it what they can't do */
    size_t fp_calls[256], fp_hot_calls;
    SnxArmCodegenOptions options; /* --profile, --memory and --trace */
    int windowed;                 /* the runs' memory is the windows' */
    unsigned char *window[2], *view[2],
        *pristine;                      /* each side's window and the memory it mirrors; what both start from */
    size_t hooked, transfers;           /* the hook's calls and checked memory's, LLVM's side's */
    uint32_t chain_returns, chain_sets; /* what chain_io gives a write, and sets R2 to (0: leaves it) */
    unsigned char* callee; /* --callee's object, callee_size bytes */
    size_t callee_size;
    SnxArmRt rt[2]; /* with it, each side's runtime */
    uint32_t (*io[2])(SnxArmCPU* cpu, uint32_t address, uint32_t size_write, uint32_t value); /* and hook */
} g_test;
static int legacy(void) {
    return g_test.options.profile == SNX_ARM_PROFILE_ARM7TDMI || g_test.options.profile == SNX_ARM_PROFILE_ARM946E;
}
static int checked(void) { return g_test.options.memory == SNX_ARM_MEMORY_CHECKED; }
static int hosted(void) { return g_test.options.memory == SNX_ARM_MEMORY_HOSTED; }
/* a profile with Thumb-2 (its T32 instructions and IT) */
static int thumb2(void) {
    return g_test.options.profile == SNX_ARM_PROFILE_A57 || g_test.options.profile == SNX_ARM_PROFILE_A9;
}
static SnxA32Kind decode(uint32_t w) { return snx_a32_decode_profile(g_test.options.profile, w); }

static u64 rnd(void) {
    u64 x = g_test.seed += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

/* ---- the words ---- */

/* Unicorn's MIPS64 raises an exception for an LL or LLD of an address past 4 GiB, as the hosted and the buses' blocks'
   window is: an exclusive of theirs is not drawn for it */
static int no_exclusives(void) {
#if defined(SNX_TEST_MIPS64) || defined(SNX_TEST_MIPS64BE)
    return hosted() || legacy();
#else
    return 0;
#endif
}
/* whether the T32 pair is an exclusive (LDREX, STREX and their byte, halfword, doubleword and acquire forms) */
static int t32_exclusive(uint16_t a, uint16_t b) {
    unsigned kind = b >> 4 & 15;
    return (a & 0xFFE0u) == 0xE840u ||
           ((a & 0xFFE0u) == 0xE8C0u && (kind == 4 || kind == 5 || kind == 7 || kind >= 12));
}

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
   the unconditional space keeps its 1111), a register field 15 one time in eight; 0 where the profile has none of them
   but UNDEFINED ones, one in 16 of which is taken */
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
        if (g_test.options.profile == SNX_ARM_PROFILE_A57) {
            if (snx_a32_decode(w) != c->kind) continue;
            return w;
        }
        /* another profile's: what it runs, as another kind of its own some (the ARM11's UADD8, MULH) */
        if (decode(w) != SNX_A32_UNDEFINED || c->kind == SNX_A32_UNDEFINED) return w;
        if (tries > 2000) return rnd() % 16 ? 0 : w;
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
#include "common/cpu/recompiler/object/elf_object.h"
#define SNX_TEST_SWAP_ARM 1
#include "support/swap_state.h"
/* a state with nothing beside it to turn over */
static void swap_cpu(void* cpu) { snx_test_swap_arm((SnxArmCPU*)cpu); }
static void swap_arm(void* state);
#endif

#define TRACE 64 /* the words a trace has room for */
typedef struct {
    union {
        SnxArmCPU cpu;
        SnxArmCheckedCPU checked;
    };
    uint32_t trace[TRACE], skipped[2];
    /* the hook's or the callback's calls: how many, and a mix of all they were given and the state they saw */
    size_t calls;
    u64 log, salt;
    const unsigned char* code; /* checked memory's fetches */
    size_t code_size;
    uint32_t code_pc;
    uint32_t reserved, reserved_at, reserved_size; /* the bus's own reservation */
    SnxArmRt rt; /* --callee's runtime */
    unsigned char mem[MEM_SIZE];
} RunState;
#ifdef SNX_TEST_NATIVE
static void swap_arm(void* state) {
    RunState* s = (RunState*)state;
    snx_test_swap_checked(&s->checked);
    for (size_t k = 0; k < TRACE; ++k) snx_test_sw32(&s->trace[k]);
    for (size_t k = 0; k < 2; ++k) snx_test_sw32(&s->skipped[k]);
    snx_test_swap_u64s(&s->rt, sizeof s->rt / 8);
}
#endif
static u64 mix(u64 h, u64 v) {
    h = (h ^ v) * 0xBF58476D1CE4E5B9ull;
    return h ^ (h >> 29);
}
/* a call seen: what it was given, and the state as the block has it then, its trace so far with it (the AGB board's
   hook reads the running instruction's own slot) */
static u64 seen(RunState* s, uint32_t a, uint32_t b, uint32_t c) {
    const SnxArmCPU* cpu = &s->cpu;
    u64 h = mix(mix(mix(s->log, a), b), c);
    for (unsigned r = 0; r < 16; ++r) h = mix(h, cpu->r[r]);
    h = mix(mix(mix(h, cpu->cpsr), cpu->exit), cpu->syndrome);
    h = mix(mix(h, cpu->exclusive_valid), cpu->exclusive_value);
    for (unsigned k = 0; k < 2 * MAX_HALF; ++k) h = mix(h, s->trace[k]);
    h = mix(mix(h, s->skipped[0]), s->skipped[1]);
    ++s->calls;
    return s->log = h;
}
/* SnxArmCPU.io: a read gives a value of its width (an ARMv4T LDRSH's sign extended), from the call itself; a write
   ends the block one time in four */
static uint32_t test_io(SnxArmCPU* cpu, uint32_t address, uint32_t size_write, uint32_t value) {
    u64 h = seen((RunState*)cpu, address, size_write, value);
    uint32_t v = (uint32_t)(h >> 32);
    if (size_write & 0x100) return h & 3 ? 0 : v | 1;
    switch (size_write) {
        case 1: return v & 0xff;
        case 2: return v & 0xffff;
        case 0x202: return v & 0x10000 ? (uint32_t)(int32_t)(int16_t)v : (uint32_t)(int32_t)(int8_t)v;
        default: return v;
    }
}
/* checked memory's callback (checked.h): one fetch in 64 faults, one in 64 finds another word; a transfer faults by its
   address's sixteen bytes, one in eight; a store-exclusive is lost one time in four */
static uint32_t test_access(void* context, uint32_t operation, uint32_t address, uint32_t size, void* bytes) {
    RunState* s = (RunState*)context;
    unsigned char* p = (unsigned char*)bytes;
    uint32_t op = operation & 3, sum = 0;
    if (op == SNX_ARM_ACCESS_WRITE)
        for (uint32_t i = 0; i < size; ++i) sum = sum * 257 + p[i];
    u64 h = seen(s, operation, address, mix(sum, size) & 0xffffffffu);
    if (op == SNX_ARM_ACCESS_FETCH) {
        uint32_t at = address - s->code_pc;
        if (at > s->code_size || size > s->code_size - at || !(h & 63)) return 1;
        memcpy(p, s->code + at, size);
        if ((h & 63) == 1) p[(h >> 8) % size] ^= (unsigned char)(1u << (h >> 16) % 8);
        return 0;
    }
    if (!(mix(s->salt, address >> 4) >> 20 & 7)) return 1;
    if (operation & SNX_ARM_ACCESS_EXCLUSIVE) {
        if (op == SNX_ARM_ACCESS_READ) {
            s->reserved = 1, s->reserved_at = address, s->reserved_size = size;
        } else {
            uint32_t held = s->reserved && s->reserved_at == address && s->reserved_size == size;
            s->reserved = 0;
            if (!held && (h & 3)) return SNX_ARM_EXCLUSIVE_FAILED;
            if (!(h >> 4 & 3)) return (h >> 8 & 1) ? SNX_ARM_EXCLUSIVE_FAILED : 3;
        }
    }
    for (uint32_t i = 0; i < size; ++i) {
        unsigned char* m = &s->mem[(address + i) % MEM_SIZE];
        if (op == SNX_ARM_ACCESS_READ)
            p[i] = *m;
        else
            *m = p[i];
    }
    return 0;
}
/* the windows, made once: 0, -1 with why on stderr */
static int windows_new(void) {
#if defined(_WIN32)
    fprintf(stderr, "FAIL: no window of guest memory on this host\n");
    return -1;
#else
    g_test.pristine = (unsigned char*)malloc(VIEW);
    for (int side = 0; g_test.pristine && side < 2; ++side) {
        FILE* f = tmpfile();
        unsigned char* w = (unsigned char*)mmap(NULL, WINDOW, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (!f || ftruncate(fileno(f), VIEW) || w == MAP_FAILED) break;
        for (u64 at = 0; at < WINDOW; at += VIEW)
            if (mmap(w + at, VIEW, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fileno(f), 0) == MAP_FAILED) {
                fprintf(stderr, "FAIL: no view of the guest window at %llx\n", (unsigned long long)at);
                return -1;
            }
        g_test.window[side] = w;
        g_test.view[side] = w;
        fclose(f);
    }
    if (!g_test.window[1]) {
        fprintf(stderr, "FAIL: no window of guest memory of %llu bytes\n", (unsigned long long)WINDOW);
        return -1;
    }
    for (size_t i = 0; i < VIEW; i += 8) {
        u64 x = rnd();
        memcpy(g_test.pristine + i, &x, 8);
    }
    return 0;
#endif
}

/* a random state of the kth kind */
/* A block is generated code, with no type signature before its entry, which the function sanitizer's check of an
   indirect call reads: the 8 bytes before the callee, below the code loader's arena for its first one. So the call is
   unchecked, as exec.c's run_block */
[[clang::no_sanitize("function")]] static void run_block(SnxU64 addr, SnxArmCPU* cpu) {
    ((SnxArmBlock)(uintptr_t)addr)(cpu);
}
static void start(RunState* s, unsigned k, uint32_t pc) {
    SnxArmCPU* cpu = &s->cpu;
    s->checked = (SnxArmCheckedCPU){0};
    for (size_t i = 0; i < MEM_SIZE && !g_test.windowed; i += 8) {
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
    if (legacy()) /* most of them in a region of the bus's, some next to its end, aligned or not */
        for (unsigned r = 0; r < 15; ++r) {
            static const unsigned char agb[16] = {0, 1, 2, 2, 3, 3, 4, 5, 6, 7, 8, 0xA, 0xD, 0xE, 0xF, 0x10};
            static const unsigned char nds[16] = {0, 1, 2, 2, 2, 2, 3, 3, 4, 4, 4, 4, 5, 6, 7, 0xFF};
            u64 x = rnd();
            uint32_t top = g_test.options.profile == SNX_ARM_PROFILE_ARM7TDMI ? agb[x & 15] : nds[x & 15];
            uint32_t low = (x >> 4 & 7) == 0 ? 0xFFFFFFu - (uint32_t)(x >> 8) % 72 : (uint32_t)(x >> 8) & 0x3FFFF;
            if ((x >> 40) % 5) cpu->r[r] = (top << 24 | low) & ((x >> 44) & 1 ? ~3u : ~0u);
        }
    cpu->r[15] = pc;
    cpu->cpsr = 0x10u | ((uint32_t)rnd() & 0xF80F0000u);
    cpu->exit = (uint32_t)rnd();
    cpu->syndrome = (uint32_t)rnd();
    cpu->mem = s->mem;
    cpu->mem_size = MEM_SIZE;
    if (g_test.windowed) /* the AGB board's, past 4 GiB by a guard, or one that ends in a region */
        cpu->mem_size = k % 4 == 3 ? 0x0E000010u : (1ull << 32) + 0x1000;
    if (hosted()) cpu->exit = SNX_ARM_OK; /* the dispatch loop's */
    if (legacy() && k % 8 != 7) cpu->io = test_io;
    if (checked()) {
        s->checked.abi = SNX_ARM_CHECKED_ABI + (k % 16 == 13);
        s->checked.access = k % 16 == 14 ? NULL : test_access;
        s->checked.context = s;
    }
    s->calls = 0;
    s->log = rnd();
    s->salt = rnd();
    s->reserved = 0;
    for (unsigned i = 0; i < TRACE; ++i) s->trace[i] = 0xdead0000u + i;
    s->skipped[0] = s->skipped[1] = (uint32_t)s->salt;
    {
        static const uint32_t sizes[4] = {1, 2, 4, 8};
        u64 x = rnd();
        cpu->exclusive_valid = k % 3 != 0;
        cpu->exclusive_address = x & 1 ? cpu->r[(x >> 1) % 15] : (uint32_t)(x >> 8) % MEM_SIZE;
        cpu->exclusive_size = sizes[(x >> 40) % 4];
        s->reserved = (x >> 45) & 1, s->reserved_at = cpu->exclusive_address, s->reserved_size = cpu->exclusive_size;
        if ((x >> 44) & 1 &&
            (g_test.windowed || cpu->exclusive_address <= MEM_SIZE - 8)) /* the value there: a STREX takes */
            memcpy(&cpu->exclusive_value,
                   g_test.windowed ? g_test.pristine + cpu->exclusive_address % (VIEW - 8)
                                   : s->mem + cpu->exclusive_address,
                   8);
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
        if (g_test.options.profile == SNX_ARM_PROFILE_ARM11 && (x >> 55) % 4 == 0) /* LEN and STRIDE: a short vector */
            f |= ((uint32_t)(x >> 57) & 7) << 16 | ((uint32_t)(x >> 60) & 3) << 20;
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
    if (a->calls != b->calls || a->log != b->log)
        return snprintf(why, sizeof g_test.why, "%zu calls of the hook or the bus, " BACK_NAME " %zu%s", a->calls,
                        b->calls, a->calls == b->calls ? ", not the same ones" : ""),
               why;
    if (a->reserved != b->reserved || a->reserved_at != b->reserved_at) return "the bus's reservation";
    for (unsigned i = 0; i < TRACE; ++i)
        if (a->trace[i] != b->trace[i])
            return snprintf(why, sizeof g_test.why, "trace %u %08x, " BACK_NAME " %08x", i, a->trace[i], b->trace[i]),
                   why;
    if (a->skipped[0] != b->skipped[0] || a->skipped[1] != b->skipped[1])
        return snprintf(why, sizeof g_test.why, "skipped %08x, " BACK_NAME " %08x", a->skipped[0], b->skipped[0]), why;
    if (g_test.windowed) {
        if (!memcmp(g_test.view[0], g_test.view[1], VIEW)) return "";
        for (size_t i = 0; i < VIEW; ++i)
            if (g_test.view[0][i] != g_test.view[1][i])
                return snprintf(why, sizeof g_test.why, "guest memory at %05zx of its view: %02x, " BACK_NAME " %02x",
                                i, g_test.view[0][i], g_test.view[1][i]),
                       why;
    }
    for (size_t i = 0; i < MEM_SIZE && !g_test.windowed; ++i)
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
                   snx_code_define(loader, "snx_arm_multiply_carry", (SnxU64)(uintptr_t)snx_arm_multiply_carry) ||
                   snx_code_define(loader, "snx_arm_io_read", (SnxU64)(uintptr_t)snx_arm_io_read) ||
                   snx_code_define(loader, "snx_arm_io_write", (SnxU64)(uintptr_t)snx_arm_io_write) ||
                   snx_code_define(loader, "snx_arm_checked_fetch", (SnxU64)(uintptr_t)snx_arm_checked_fetch) ||
                   snx_code_define(loader, "snx_arm_checked_access", (SnxU64)(uintptr_t)snx_arm_checked_access) ||
                   snx_code_define(loader, "snx_arm_checked_strex", (SnxU64)(uintptr_t)snx_arm_checked_strex))) {
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
    uint32_t pc;
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
        if (!anything && !snx_thumb_decode_profile(g_test.options.profile, x, y, &t)) continue;
        if (no_exclusives() && t32_exclusive(x, y)) continue;
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
        if ((hw1 >> 11) < 0x1D || !thumb2()) {
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
    if (thumb2() && rnd() % 4 == 0) {
        b->half[b->n++] = (uint16_t)(0xBF00u | (unsigned)(rnd() % 15) << 4 | (1u + (unsigned)(rnd() % 15)));
        n = 1 + (unsigned)(rnd() % 4);
    }
    for (; k < n; ++k) {
        if (!thumb2() && rnd() % 6 == 0) { /* BL's and BLX's halves, and what is none */
            b->half[b->n++] = (uint16_t)(0xE800u + rnd() % 0x1800);
        } else if (thumb2() && rnd() % 3 == 0) {
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
    int rc = load_object(loader, &input, 1, names, n, got, error, sizeof error);
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
    if (g_test.callee) { /* the functions a block calls, the machine's own code: its symbols are the block's calls */
        if (hostuc_load(uc, g_test.callee, g_test.callee_size, error, sizeof error)) {
            fprintf(stderr, "FAIL: --callee's object: %s\n", error);
            return 1;
        }
        g_test.rt[1].undefined =
            (void (*)(SnxArmCPU*, uint32_t, uint32_t))(uintptr_t)hostuc_symbol(uc, "snx_test_undefined");
        g_test.rt[1].coproc = (void (*)(SnxArmCPU*, uint32_t))(uintptr_t)hostuc_symbol(uc, "snx_test_coproc");
        g_test.io[1] =
            (uint32_t (*)(SnxArmCPU*, uint32_t, uint32_t, uint32_t))(uintptr_t)hostuc_symbol(uc, "snx_test_io");
        if (!g_test.rt[1].undefined || !g_test.rt[1].coproc || !g_test.io[1]) {
            fprintf(stderr, "FAIL: --callee's object has no callbacks\n");
            return 1;
        }
    } else if (!hostuc_native(uc, "snx_arm_undefined", (void*)snx_arm_undefined) ||
               !hostuc_native(uc, "snx_arm_coproc", (void*)snx_arm_coproc) ||
               !hostuc_native(uc, "snx_arm_multiply_carry", (void*)snx_arm_multiply_carry) ||
               !hostuc_native(uc, "snx_arm_io_read", (void*)snx_arm_io_read) ||
               !hostuc_native(uc, "snx_arm_io_write", (void*)snx_arm_io_write) ||
               !hostuc_native(uc, "snx_arm_checked_fetch", (void*)snx_arm_checked_fetch) ||
               !hostuc_native(uc, "snx_arm_checked_access", (void*)snx_arm_checked_access) ||
               !hostuc_native(uc, "snx_arm_checked_strex", (void*)snx_arm_checked_strex)) {
        fprintf(stderr, "FAIL: the " BACK_NAME " code's helpers: out of memory\n");
        return 1;
    }
    if (!hostuc_native(uc, "snx_arm_fp_x64", (void*)counted_fp) ||
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
#if defined(SNX_TEST_UNWINDS)
    if (uc && g_test.step) hostuc_step(uc, 1);
#endif
    if (s) hostuc_swapper(uc, swap_arm, &s[1]); /* a big-endian machine's state turned over around its runs */
    if (s && g_test.windowed && hostuc_map(uc, g_test.window[1], WINDOW)) s = NULL;
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
        if (!b->pc) b->pc = PC;
        if (!(b->thumb ? snx_thumb_llvm_block_ex(module, name, b->half, b->n, b->pc, &lc, &g_test.options)
                       : snx_arm_llvm_block_ex(module, name, b->words, b->n, b->pc, &lc, &g_test.options))) {
            block_text(b, text, sizeof text);
            fprintf(stderr, "FAIL: [%s]: LLVM refused it\n", text);
            ++g_test.failures;
            rc = -1;
            break;
        }
        b->consumed = lc;
        b->made = b->thumb ? back_(thumb_block)(x64, name, code_of(b), b->n, b->pc, &xc, NULL, NULL)
                           : back_(block)(x64, name, code_of(b), b->n, b->pc, &xc, NULL, NULL);
        if (!b->made) {
            SnxA32Kind k = b->thumb ? SNX_A32_UNDEFINED : decode(b->words[0]);
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
                start(&s[side], k, b->pc);
                if (b->thumb) s[side].cpu.cpsr |= SNX_ARM_T;
                if (g_test.callee) s[side].rt = g_test.rt[side], s[side].cpu.rt = &s[side].rt;
                if (g_test.callee && s[side].cpu.io) s[side].cpu.io = g_test.io[side];
                s[side].code = code_of(b), s[side].code_size = b->n * (b->thumb ? 2 : 4), s[side].code_pc = b->pc;
                if (g_test.windowed) {
                    memcpy(g_test.view[side], g_test.pristine, VIEW);
                    s[side].cpu.mem = g_test.window[side];
                }
#if defined(SNX_TEST_NATIVE)
                if (side) {
                    if (hostuc_call(uc, xaddr[i], (u64)(uintptr_t)&s[1].cpu, 0, NULL)) s[1].cpu.exit = 0xdead;
                    continue;
                }
#endif
                run_block(side ? xaddr[i] : laddr[i], &s[side].cpu);
            }
            ++g_test.ran;
            *(legacy() ? &g_test.hooked : &g_test.transfers) += s[0].calls;
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
                    start(&s[0], k, b->pc);
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
            start(&s[side], 1, PC);
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
            start(&s[side], 1, PC);
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
    if (!s || load_object(loader, &input, 1, names, 1, &addr, error, sizeof error)) {
        fprintf(stderr, "FAIL: the MSR's chain's object: %s\n", error);
        free(s), free(object);
        snx_code_loader_free(loader);
        return 1;
    }
    for (int e = 0; e < 2; ++e) {
        start(s, 1, PC);
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

/* ---- the other modes' block to block: a write whose hook ends the block goes nowhere else, however its block goes on;
   a hosted block checks nothing as it is entered and clears nothing, and one that ends inside an IT block goes back to
   its dispatch loop, which looks at ITSTATE ---- */

static uint32_t chain_io(SnxArmCPU* cpu, uint32_t address, uint32_t size_write, uint32_t value) {
    (void)address, (void)size_write, (void)value;
    if (g_test.chain_sets) cpu->r[2] = g_test.chain_sets;
    return g_test.chain_returns;
}
static size_t chain_next(void* user, uint32_t key, size_t units, size_t count, uint32_t keys[2]) {
    (void)user, (void)count;
    if ((key & ~1u) != PC) return 0;
    keys[0] = (key & ~1u) + (uint32_t)units * (key & 1 ? 2 : 4) + (key & 1);
    return 1;
}
typedef struct {
    const char* what;
    const unsigned char code[16]; /* the first block's units, then the second's one */
    int thumb;
    size_t units;                             /* the first block's */
    uint32_t returns;                         /* the hook's */
    uint32_t r15, cpsr, exit;                 /* the state it starts from */
    uint32_t end15, end_cpsr, end_exit, end2; /* R15, CPSR, the exit and R2 (0 to start with) as it ends */
    uint32_t sets;                            /* what the hook sets R2 to, 0 for nothing */
} ChainCase;
static const ChainCase bus_chains[] = {
    /* STR R0, [R1]; MOV R2, #1: the hook's write returns 0, the block goes on; nonzero, it ends there */
    {"STR whose hook goes on",
     {0x00, 0x00, 0x81, 0xe5, 0x01, 0x20, 0xa0, 0xe3},
     0,
     1,
     0,
     PC,
     0x10,
     0,
     PC + 8,
     0x10,
     0,
     1,
     0},
    {"STR whose hook yields",
     {0x00, 0x00, 0x81, 0xe5, 0x01, 0x20, 0xa0, 0xe3},
     0,
     1,
     1,
     PC,
     0x10,
     0,
     PC + 4,
     0x10,
     0,
     0,
     0},
    /* the same in Thumb: STR R0, [R1]; MOVS R2, #1 */
    {"T16 STR whose hook goes on", {0x08, 0x60, 0x01, 0x22}, 1, 1, 0, PC, 0x30, 0, PC + 4, 0x30, 0, 1, 0},
    {"T16 STR whose hook yields", {0x08, 0x60, 0x01, 0x22}, 1, 1, 0x80000000u, PC, 0x30, 0, PC + 2, 0x30, 0, 0, 0},
    /* ADD R2, R2, #1; STR R0, [R1], whose hook sets R2 to 0x40; ADD R2, R2, #1; NOP: the instruction after it reads
       what the hook left, not the value the block had */
    {"STR whose hook sets a register",
     {0x01, 0x20, 0x82, 0xe2, 0x00, 0x00, 0x81, 0xe5, 0x01, 0x20, 0x82, 0xe2, 0x00, 0xf0, 0x20, 0xe3},
     0,
     3,
     0,
     PC,
     0x10,
     0,
     PC + 16,
     0x10,
     0,
     0x41,
     0x40},
    /* STREQ, its condition failing (Z clear): nothing stored, the block goes on whatever the hook gave before */
    {"STREQ not run", {0x00, 0x00, 0x81, 0x05, 0x01, 0x20, 0xa0, 0xe3}, 0, 1, 1, PC, 0x10, 0, PC + 8, 0x10, 0, 1, 0},
};
static const ChainCase hosted_chains[] = {
    /* NOP; MOV R2, #1, from a state no block of another mode takes: R15 elsewhere, CPSR.T and E set, an exit left */
    {"a hosted entry",
     {0x00, 0xf0, 0x20, 0xe3, 0x01, 0x20, 0xa0, 0xe3},
     0,
     1,
     0,
     0x777,
     0x230,
     5,
     PC + 8,
     0x230,
     5,
     1,
     0},
    /* IT EQ; MOVS R2, #1: the first block ends inside the IT block (Z clear) */
    {"a hosted IT block's end", {0x08, 0xbf, 0x01, 0x22}, 1, 1, 0, PC, 0x30, 0, PC + 2, 0x30 | 0x00000800, 0, 0, 0},
};
static int run_chains(Back* x64) {
    const ChainCase* cases = legacy() ? bus_chains : hosted() ? hosted_chains : NULL;
    size_t n = legacy() ? sizeof bus_chains / sizeof *bus_chains : sizeof hosted_chains / sizeof *hosted_chains;
    size_t failures = 0, ran = 0;
    for (size_t i = 0; cases && i < n; ++i) {
        const ChainCase* k = &cases[i];
        unsigned char* object = NULL;
        size_t consumed = 0, size = 0, unit = k->thumb ? 2 : 4;
        char error[512] = "", name[16];
        SnxCodeLoader* loader = NULL;
        SnxU64 addr = 0;
        if (k->thumb && !thumb2() && k->code[1] == 0xbf) continue;
        snprintf(name, sizeof name, "%c_%08x", k->thumb ? 'T' : 'A', PC);
        int made = k->thumb ? back_(thumb_block)(x64, NULL, k->code, k->units, PC, &consumed, chain_next, NULL) &&
                                  back_(thumb_block)(x64, NULL, k->code + k->units * unit, 1,
                                                      PC + (uint32_t)(k->units * unit), &consumed, chain_next, NULL)
                            : back_(block)(x64, NULL, k->code, k->units, PC, &consumed, chain_next, NULL) &&
                                  back_(block)(x64, NULL, k->code + k->units * unit, 1,
                                               PC + (uint32_t)(k->units * unit), &consumed, chain_next, NULL);
        SnxLinkInput input = {0};
        const char* names[1] = {name};
        RunState* s = (RunState*)calloc(1, sizeof *s);
        if (made && s && !back_(object)(x64, &object, &size, NULL) && (loader = new_loader(error, sizeof error)) &&
            (input = (SnxLinkInput){.data = object, .size = size, .name = "chain"}, 1) &&
            !snx_code_load_private(loader, &input, 1, names, 1, &addr, error, sizeof error)) {
            g_test.chain_returns = k->returns;
            g_test.chain_sets = k->sets;
            s->cpu.mem = g_test.window[1];
            s->cpu.mem_size = 1ull << 32;
            s->cpu.io = chain_io;
            s->cpu.r[1] = 0x04000100;
            s->cpu.r[15] = k->r15;
            s->cpu.cpsr = k->cpsr;
            s->cpu.exit = k->exit;
            run_block(addr, &s->cpu);
            ++ran;
            if (s->cpu.r[15] != k->end15 || s->cpu.cpsr != k->end_cpsr || s->cpu.exit != k->end_exit ||
                s->cpu.r[2] != k->end2) {
                fprintf(stderr,
                        "FAIL: %s: R15 %08x, CPSR %08x, exit %u, R2 %u; wanted R15 %08x, CPSR %08x, exit %u, R2 %u\n",
                        k->what, s->cpu.r[15], s->cpu.cpsr, s->cpu.exit, s->cpu.r[2], k->end15, k->end_cpsr,
                        k->end_exit, k->end2);
                ++failures;
            }
        } else {
            fprintf(stderr, "FAIL: %s: %s%s\n", k->what, back_(why)(x64), error);
            ++failures;
        }
        free(s), free(object);
        if (loader) snx_code_loader_free(loader);
    }
    if (cases) printf("%zu cases of a block to the next, %zu failures\n", ran, failures);
    g_test.failures += failures;
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
    if (!s || load_object(loader, &input, 1, names, 1, &addr, error, sizeof error)) {
        fprintf(stderr, "FAIL: VRSHL's witness's object: %s\n", error);
        free(s), free(object);
        snx_code_loader_free(loader);
        return 1;
    }
    start(s, 1, PC);
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

#endif

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
#if defined(SNX_TEST_NATIVE)
    HostUc* uc = hostuc_new(BACK_UC, 16u << 20);
    RunState* s = uc ? (RunState*)hostuc_alloc(uc, 2 * sizeof *s) : NULL;
    if (s) hostuc_swapper(uc, swap_arm, &s[1].cpu);
#else
    RunState* s = (RunState*)malloc(2 * sizeof *s);
#endif
    char error[512];
    if (!blocks || !laddr || !xaddr || !s) {
        fprintf(stderr, "FAIL: out of memory for the %zu Thumb witnesses\n", count);
        free(blocks), free(laddr), free(xaddr);
#if defined(SNX_TEST_NATIVE)
        if (uc) hostuc_free(uc);
#else
        free(s);
#endif
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
            !(b->made = back_(thumb_block)(x64, name, code_of(b), b->n, PC, &xconsumed, NULL, NULL))) {
            fprintf(stderr, "FAIL: the Thumb witness %s: a back end refused it: %s\n", thumb_cases[i].what,
                    back_(why)(x64));
            return 1;
        }
        if (consumed != xconsumed) {
            fprintf(stderr,
                    "FAIL: the Thumb witness %s: LLVM's block takes %zu halfwords, the " BACK_NAME " back end's %zu\n",
                    thumb_cases[i].what, consumed, xconsumed);
            ++failures;
        }
    }
    if (snx_ir_module_object(ir, module, '0', &lobj, &lsize, error, sizeof error) ||
        back_(object)(x64, &xobj, &xsize, NULL) || !(lloader = new_loader(error, sizeof error)) ||
        !(xloader = new_loader(error, sizeof error)) || load(lloader, lobj, lsize, "LLVM", blocks, count, 0, laddr) ||
#if defined(SNX_TEST_NATIVE)
        load_uc(uc, xobj, xsize, blocks, count, xaddr)) {
#else
        load(xloader, xobj, xsize, BACK_NAME, blocks, count, 1, xaddr)) {
#endif
        fprintf(stderr, "FAIL: the Thumb witnesses' objects: %s\n", error);
        return 1;
    }
    for (size_t i = 0; i < count; ++i) {
        const ThumbCase* w = &thumb_cases[i];
        for (unsigned k = 0; k < (w->exit == ANY ? 16u : 1u); ++k) {
            u64 seed = g_test.seed;
            for (int side = 0; side < 2; ++side) {
                g_test.seed = seed;
                start(&s[side], w->exit == ANY ? k : 1, PC);
                s[side].cpu.cpsr = w->set & CPSR_SET ? w->cpsr : s[side].cpu.cpsr | SNX_ARM_T;
                if (w->set & R15_SET) s[side].cpu.r[15] = w->r15;
                if (w->set & REGS_SET) s[side].cpu.r[0] = w->r0, s[side].cpu.r[1] = w->r1;
#if defined(SNX_TEST_NATIVE)
                if (side) {
                    if (hostuc_call(uc, xaddr[i], (u64)(uintptr_t)&s[1].cpu, 0, NULL)) s[1].cpu.exit = 0xdead;
                    continue;
                }
#endif
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
#if defined(SNX_TEST_NATIVE)
    hostuc_free(uc);
#else
    free(s);
#endif
    free(lobj), free(xobj), free(laddr), free(xaddr), free(blocks);
    snx_code_loader_free(lloader);
    snx_code_loader_free(xloader);
    LLVMDisposeModule(module);
    return 0;
}

/* ---- a block that goes on to its own start: past the thread's poll, the word at snx_arm_poll_offset from the state,
   which the module's variable holds. Set, the block returns there, R15 its start; clear, it loops */
typedef struct {
    SnxArmCPU cpu;
    uint32_t other;
    uint32_t poll; /* the thread's */
} PollState;
static size_t poll_next(void* user, uint32_t key, size_t units, size_t count, uint32_t keys[2]) {
    (void)user, (void)units, (void)count;
    if ((key & ~1u) != PC) return 0;
    keys[0] = key; /* itself, T set or not */
    return 1;
}
/* thumb: a Thumb block's, entered with T set */
static int run_poll_chain(Back* x64, int thumb) {
    static const uint32_t words[2] = {0xE2500001u, 0x1AFFFFFDu}; /* SUBS R0, R0, #1; BNE the block */
    static const uint16_t halves[2] = {0x3801u, 0xD1FDu};        /* the same in T16 */
    unsigned char* object = NULL;
    size_t consumed = 0, size = 0, failures = 0;
    char error[512] = "", name[16];
    SnxU64 addr = 0;
    int rc = 1;
    snprintf(name, sizeof name, "%c_%08x", thumb ? 'T' : 'A', PC);
    if (!(thumb ? back_(thumb_block)(x64, NULL, (const unsigned char*)halves, 2, PC, &consumed, poll_next, NULL)
                : back_(block)(x64, NULL, (const unsigned char*)words, 2, PC, &consumed, poll_next, NULL)) ||
        back_(object)(x64, &object, &size, NULL)) {
        fprintf(stderr, "FAIL: the poll's block: %s\n", back_(why)(x64));
        return 1;
    }
#if defined(SNX_TEST_NATIVE)
    HostUc* uc = hostuc_new(BACK_UC, 1u << 20);
    PollState* t = uc ? (PollState*)hostuc_alloc(uc, sizeof *t) : NULL;
    int64_t* offset = uc ? (int64_t*)hostuc_alloc(uc, sizeof *offset) : NULL;
    if (!t || !offset) goto done;
    *offset = (int64_t)offsetof(PollState, poll);
    if (hostuc_big(BACK_UC)) *offset = (int64_t)snx_bswap64((uint64_t)*offset);
    hostuc_swapper(uc, swap_cpu, &t->cpu);
    if (hostuc_define(uc, "snx_arm_poll_offset", (uint64_t)(uintptr_t)offset) ||
        hostuc_load(uc, object, size, error, sizeof error) || !(addr = hostuc_symbol(uc, name)))
        goto done;
#else
    /* the module's variable in an object of its own: the code reads it within 2 GiB */
    SnxCodeLoader* loader = NULL;
    PollState* t = (PollState*)malloc(sizeof *t);
    int64_t offset = (int64_t)offsetof(PollState, poll);
    SnxXElf* data = snx_xelf_new();
    unsigned char* data_object = NULL;
    size_t data_size = 0;
    const char* names[1] = {name};
    int made = data != NULL;
    if (made) {
        size_t at = snx_xelf_put(data, SNX_ELF_DATA, &offset, sizeof offset, 8);
        snx_xelf_symbol(data, "snx_arm_poll_offset", SNX_ELF_DATA, at, sizeof offset, SNX_ELF_GLOBAL);
        made = !snx_xelf_write(data, &data_object, &data_size);
        snx_xelf_free(data);
    }
    SnxLinkInput inputs[2] = {{.data = object, .size = size, .name = "poll"},
                              {.data = data_object, .size = data_size, .name = "data"}};
    if (!made || !t || !(loader = new_loader(error, sizeof error)) ||
        load_object(loader, inputs, 2, names, 1, &addr, error, sizeof error))
        goto done;
#endif
    for (int set = 0; set < 2; ++set) {
        *t = (PollState){0};
        t->cpu.r[0] = 5;
        t->cpu.r[15] = PC;
        t->cpu.cpsr = 0x10u | (thumb ? SNX_ARM_T : 0);
        t->poll = (uint32_t)set;
#if defined(SNX_TEST_NATIVE)
        if (hostuc_call(uc, addr, (uint64_t)(uintptr_t)&t->cpu, 0, NULL)) {
            fprintf(stderr, "FAIL: the poll's block stopped, the poll %s\n", set ? "set" : "clear");
            ++failures;
            continue;
        }
#else
        run_block(addr, &t->cpu);
#endif
        uint32_t r0 = set ? 4 : 0, r15 = set ? PC : thumb ? PC + 4 : PC + 8;
        if (t->cpu.exit != SNX_ARM_OK || t->cpu.r[0] != r0 || t->cpu.r[15] != r15) {
            fprintf(stderr, "FAIL: the poll's block, the poll %s: exit %u, R0 %u, R15 %08x; wanted R0 %u, R15 %08x\n",
                    set ? "set" : "clear", t->cpu.exit, t->cpu.r[0], t->cpu.r[15], r0, r15);
            ++failures;
        }
    }
    printf("a%s block back to its start past the thread's poll, set and clear: %zu failures\n",
           thumb ? " Thumb" : "n A32", failures);
    g_test.failures += failures;
    rc = 0;
done:
    if (rc) fprintf(stderr, "FAIL: the poll's object: %s\n", error);
    free(object);
#if defined(SNX_TEST_NATIVE)
    if (uc) hostuc_free(uc);
#else
    free(t), free(data_object);
    snx_code_loader_free(loader);
#endif
    return rc;
}

#if defined(SNX_TEST_UNWINDS)
/* the entry's checks of an A32 and a Thumb block (llvm.c's block_begin and thumb_block, in their order), each failed one
   with its exit and syndrome, R15 left as it is; run an instruction at a time, its exit's code having no frame */
static int run_entries(Back* x64) {
    enum { WORD = 0xE3A00001u /* MOV R0, #1 */, HALF = 0x2001u /* MOVS R0, #1 */ };
    static const struct {
        const char* what;
        int thumb;
        uint32_t r15, cpsr, exit, syndrome;
    } cases[] = {
        {"R15 elsewhere", 0, PC + 4, 0x10u, SNX_ARM_BAD_ENTRY, PC},
        {"T set", 0, PC, 0x30u, SNX_ARM_BAD_ENTRY, WORD},
        {"E set", 0, PC, 0x210u, SNX_ARM_ENDIAN_UNSUPPORTED, WORD},
        {"T and E set", 0, PC, 0x230u, SNX_ARM_BAD_ENTRY, WORD},
        {"R15 elsewhere", 1, PC + 2, 0x30u, SNX_ARM_BAD_ENTRY, PC},
        {"T clear", 1, PC, 0x10u, SNX_ARM_BAD_ENTRY, HALF},
        {"ITSTATE's high bits", 1, PC, 0x430u, SNX_ARM_IT_ENTRY, 0x04},
        {"ITSTATE's low bits", 1, PC, 0x06000030u, SNX_ARM_IT_ENTRY, 0x03},
        {"E set", 1, PC, 0x230u, SNX_ARM_ENDIAN_UNSUPPORTED, HALF},
        {"none", 0, PC, 0x10u, SNX_ARM_OK, 0},
        {"none", 1, PC, 0x30u, SNX_ARM_OK, 0},
    };
    static const uint32_t word = WORD;
    static const uint16_t half = HALF;
    unsigned char* object = NULL;
    size_t consumed = 0, size = 0, failures = 0;
    char error[512] = "";
    SnxU64 addr[2] = {0};
    if (!back_(block)(x64, "E_arm", (const unsigned char*)&word, 1, PC, &consumed, NULL, NULL) ||
        !back_(thumb_block)(x64, "E_thumb", (const unsigned char*)&half, 1, PC, &consumed, NULL, NULL) ||
        back_(object)(x64, &object, &size, NULL)) {
        fprintf(stderr, "FAIL: the entries' blocks: %s\n", back_(why)(x64));
        return 1;
    }
    HostUc* uc = hostuc_new(BACK_UC, 1u << 20);
    RunState* s = uc ? (RunState*)hostuc_alloc(uc, sizeof *s) : NULL;
    if (!s || hostuc_load(uc, object, size, error, sizeof error) || !(addr[0] = hostuc_symbol(uc, "E_arm")) ||
        !(addr[1] = hostuc_symbol(uc, "E_thumb"))) {
        fprintf(stderr, "FAIL: the entries' object: %s\n", error);
        free(object);
        if (uc) hostuc_free(uc);
        return 1;
    }
    hostuc_step(uc, 1);
    for (size_t k = 0; k < sizeof cases / sizeof *cases; ++k) {
        const int ok = cases[k].exit == SNX_ARM_OK;
        memset(s, 0, sizeof *s);
        s->cpu.r[15] = cases[k].r15;
        s->cpu.cpsr = cases[k].cpsr;
        s->cpu.exit = 0x55, s->cpu.syndrome = 0x55;
        uint32_t r15 = ok ? PC + (cases[k].thumb ? 2 : 4) : cases[k].r15;
        if (hostuc_call(uc, addr[cases[k].thumb], (uint64_t)(uintptr_t)&s->cpu, 0, NULL) ||
            s->cpu.exit != cases[k].exit || s->cpu.syndrome != cases[k].syndrome || s->cpu.r[15] != r15 ||
            s->cpu.r[0] != (unsigned)ok) {
            fprintf(stderr, "FAIL: a%s block's entry, %s: exit %u, syndrome %x, R15 %x, R0 %u; wanted %u, %x, %x, %d\n",
                    cases[k].thumb ? " Thumb" : "n A32", cases[k].what, s->cpu.exit, s->cpu.syndrome, s->cpu.r[15],
                    s->cpu.r[0], cases[k].exit, cases[k].syndrome, r15, ok);
            ++failures;
        }
    }
    printf("the entries' checks, %zu states: %zu failures\n", sizeof cases / sizeof *cases, failures);
    free(object);
    hostuc_step(uc, 0);
    hostuc_free(uc);
    g_test.failures += failures;
    return 0;
}
#endif

/* ---- the hosted blocks that go on by themselves (codegen.h's SnxArmChainedOptions), on every machine ---- */

#define SG_TEST_IT_BITS 0x0600FC00u /* ITSTATE in CPSR */
/* the state a chained block gets, and what the runtime keeps past it */
typedef struct {
    SnxArmCPU cpu;
    int32_t poll;
    const SnxArmLookup* lookup;
    uint32_t (*direct)(SnxArmCPU* cpu, uint32_t import);
    uint32_t calls, import, r15, returns, sets15; /* the direct function's: what it saw, what it gives, R15 it leaves */
} ChainedState;
static uint32_t chained_direct(SnxArmCPU* cpu, uint32_t import) {
    ChainedState* s = (ChainedState*)cpu;
    ++s->calls;
    s->import = import;
    s->r15 = cpu->r[15];
    if (s->sets15) cpu->r[15] = s->sets15;
    return s->returns;
}
#define CHAINED_IMPORT 77u
/* the blocks: a loop, SUBS R0, R0, #1 and BNE to it; after it MOV R2, #1; BX R1; MOV R2, #2; an import's stub, whose
   SVC's immediate is not the import's number; after it MOV R2, #3; BXEQ R1; an SVC that is no stub's. The Thumb ones:
   SUBS R0, #1 and BNE to it; BX R1; a stub; after it MOVS R2, #4; IT EQ alone; CBZ R0 to MOVS R2, #5, past MOVS R2, #6 */
enum {
    C_LOOP = 0x1000,
    C_AFTER = 0x1008,
    C_BX = 0x1100,
    C_MOV = 0x1200,
    C_STUB = 0x1300,
    C_PAST = 0x1304,
    C_BXEQ = 0x1400,
    C_SVC = 0x1500,
    C_TLOOP = 0x1601,
    C_TBX = 0x1701,
    C_TSTUB = 0x1801,
    C_TPAST = 0x1803,
    C_TIT = 0x1901,
    C_TCBZ = 0x1A01,
    C_TNONZERO = 0x1A03,
    C_TZERO = 0x1A09
};
static const struct {
    uint32_t key;
    unsigned char code[8];
    size_t units;
} chained_blocks[] = {
    {C_LOOP, {0x01, 0x00, 0x50, 0xe2, 0xfd, 0xff, 0xff, 0x1a}, 2},
    {C_AFTER, {0x01, 0x20, 0xa0, 0xe3}, 1},
    {C_BX, {0x11, 0xff, 0x2f, 0xe1}, 1},
    {C_MOV, {0x02, 0x20, 0xa0, 0xe3}, 1},
    {C_STUB, {0x05, 0x00, 0x00, 0xef}, 1},
    {C_PAST, {0x03, 0x20, 0xa0, 0xe3}, 1},
    {C_BXEQ, {0x11, 0xff, 0x2f, 0x01}, 1},
    {C_SVC, {0x05, 0x00, 0x00, 0xef}, 1},
    {C_TLOOP, {0x01, 0x38, 0xfd, 0xd1}, 2},
    {C_TBX, {0x08, 0x47}, 1},
    {C_TSTUB, {0x05, 0xdf}, 1},
    {C_TPAST, {0x04, 0x22}, 1},
    {C_TIT, {0x08, 0xbf}, 1},
    {C_TCBZ, {0x10, 0xb1}, 1},
    {C_TNONZERO, {0x06, 0x22}, 1},
    {C_TZERO, {0x05, 0x22}, 1},
};
#define CHAINED_BLOCKS (sizeof chained_blocks / sizeof *chained_blocks)
/* every block that may come next is one of them, but the targets only R1 or the table names */
static size_t chained_next(void* user, uint32_t key, size_t units, size_t count, uint32_t keys[2]) {
    (void)user, (void)units, (void)count;
    switch (key) {
        case C_LOOP: keys[0] = C_LOOP, keys[1] = C_AFTER; return 2;
        case C_STUB: keys[0] = C_PAST; return 1;
        case C_TLOOP: keys[0] = C_TLOOP; return 1;
        case C_TSTUB: keys[0] = C_TPAST; return 1;
        case C_TCBZ: keys[0] = C_TZERO, keys[1] = C_TNONZERO; return 2;
        default: return 0;
    }
}
typedef struct {
    const char* what;
    uint32_t entry, r0, r1, cpsr;
    int32_t poll;
    uint32_t key, block;      /* the table's one slot: its key, and the block there (a block's key, 0 for null) */
    int table, direct;        /* a table, a direct function */
    uint32_t returns, sets15; /* the direct function's */
    uint32_t end0, end2, end15, endt; /* R0, R2 (0 to start with), R15 and CPSR's T and IT bits as it ends */
    int32_t end_poll;
    uint32_t exit, syndrome, calls;
} ChainedCase;
static const ChainedCase chained_cases[] = {
    {"a loop within the poll", .entry = C_LOOP, .r0 = 5, .poll = 100, .table = 1, .end2 = 1, .end15 = C_AFTER + 4,
     .end_poll = 96},
    {"a loop past the poll", .entry = C_LOOP, .r0 = 5, .poll = 2, .table = 1, .end0 = 3, .end15 = C_LOOP},
    {"a loop with no table", .entry = C_LOOP, .r0 = 5, .poll = 100, .end2 = 1, .end15 = C_AFTER + 4, .end_poll = 96},
    {"BX forward", .entry = C_BX, .r1 = C_MOV, .poll = 1, .key = C_MOV, .block = C_MOV, .table = 1, .end2 = 2,
     .end15 = C_MOV + 4, .end_poll = 1},
    {"BX forward, the poll out", .entry = C_BX, .r1 = C_MOV, .key = C_MOV, .block = C_MOV, .table = 1, .end15 = C_MOV},
    {"BX back", .entry = C_BX, .r1 = C_AFTER, .poll = 3, .key = C_AFTER, .block = C_AFTER, .table = 1, .end2 = 1,
     .end15 = C_AFTER + 4, .end_poll = 2},
    {"BX back at the poll's last", .entry = C_BX, .r1 = C_AFTER, .poll = 1, .key = C_AFTER, .block = C_AFTER,
     .table = 1, .end15 = C_AFTER},
    {"BX to itself", .entry = C_BX, .r1 = C_BX, .poll = 4, .key = C_BX, .block = C_BX, .table = 1, .end15 = C_BX},
    {"BX to a Thumb key", .entry = C_BX, .r1 = C_MOV | 1, .poll = 9, .key = C_MOV | 1, .block = C_MOV, .table = 1,
     .end2 = 2, .end15 = C_MOV + 4, .endt = SNX_ARM_T, .end_poll = 9},
    {"BX to Thumb, an ARM key", .entry = C_BX, .r1 = C_MOV | 1, .poll = 9, .key = C_MOV, .block = C_MOV, .table = 1,
     .end15 = C_MOV, .endt = SNX_ARM_T, .end_poll = 9},
    {"BX, the slot another's", .entry = C_BX, .r1 = C_MOV, .poll = 9, .key = C_MOV + 0x20000, .block = C_MOV,
     .table = 1, .end15 = C_MOV, .end_poll = 9},
    {"BX, the slot's block null", .entry = C_BX, .r1 = C_MOV, .poll = 9, .key = C_MOV, .table = 1, .end15 = C_MOV,
     .end_poll = 9},
    {"BX with no table", .entry = C_BX, .r1 = C_MOV, .poll = 9, .key = C_MOV, .block = C_MOV, .end15 = C_MOV,
     .end_poll = 9},
    {"on, through the table", .entry = C_MOV, .poll = 9, .key = C_MOV + 4, .block = C_AFTER, .table = 1, .end2 = 1,
     .end15 = C_AFTER + 4, .end_poll = 9},
    {"BXEQ not taken", .entry = C_BXEQ, .r1 = C_AFTER, .poll = 9, .key = C_BXEQ + 4, .block = C_MOV, .table = 1,
     .end2 = 2, .end15 = C_MOV + 4, .end_poll = 9},
    {"BXEQ taken", .entry = C_BXEQ, .r1 = C_AFTER, .cpsr = 1u << 30, .poll = 9, .key = C_AFTER, .block = C_AFTER,
     .table = 1, .end2 = 1, .end15 = C_AFTER + 4, .end_poll = 8},
    {"an SVC", .entry = C_SVC, .poll = 9, .key = C_SVC + 4, .block = C_MOV, .table = 1, .direct = 1, .returns = 1,
     .end15 = C_SVC + 4, .end_poll = 9, .exit = SNX_ARM_SVC, .syndrome = 5},
    {"a stub with no function", .entry = C_STUB, .poll = 9, .table = 1, .end15 = C_PAST, .end_poll = 9,
     .exit = SNX_ARM_SVC, .syndrome = CHAINED_IMPORT},
    {"a stub that goes on", .entry = C_STUB, .poll = 9, .table = 1, .direct = 1, .returns = 1, .end2 = 3,
     .end15 = C_PAST + 4, .end_poll = 9, .calls = 1},
    {"a stub that stops", .entry = C_STUB, .poll = 9, .key = C_PAST, .block = C_PAST, .table = 1, .direct = 1,
     .end15 = C_PAST, .end_poll = 9, .calls = 1},
    {"a stub whose function moves R15", .entry = C_STUB, .poll = 9, .table = 1, .direct = 1, .sets15 = 0x5554,
     .end15 = 0x5554, .end_poll = 9, .calls = 1},
    {"a Thumb loop within the poll", .entry = C_TLOOP, .r0 = 5, .poll = 100, .table = 1, .end15 = C_TLOOP + 3,
     .endt = SNX_ARM_T, .end_poll = 96},
    {"a Thumb loop past the poll", .entry = C_TLOOP, .r0 = 5, .poll = 2, .table = 1, .end0 = 3, .end15 = C_TLOOP - 1,
     .endt = SNX_ARM_T},
    {"a Thumb BX to ARM", .entry = C_TBX, .r1 = C_AFTER, .poll = 9, .key = C_AFTER, .block = C_AFTER, .table = 1,
     .end2 = 1, .end15 = C_AFTER + 4, .end_poll = 8},
    {"a Thumb BX to Thumb", .entry = C_TBX, .r1 = C_TPAST, .poll = 9, .key = C_TPAST, .block = C_TPAST, .table = 1,
     .end2 = 4, .end15 = C_TPAST + 1, .endt = SNX_ARM_T, .end_poll = 9},
    {"a Thumb stub that goes on", .entry = C_TSTUB, .poll = 9, .table = 1, .direct = 1, .returns = 1, .end2 = 4,
     .end15 = C_TPAST + 1, .endt = SNX_ARM_T, .end_poll = 9, .calls = 1},
    {"a Thumb stub with no function", .entry = C_TSTUB, .poll = 9, .table = 1, .end15 = C_TPAST - 1, .endt = SNX_ARM_T,
     .end_poll = 9, .exit = SNX_ARM_SVC, .syndrome = CHAINED_IMPORT},
    /* both ways known: each its own jump, no table's */
    {"CBZ taken", .entry = C_TCBZ, .poll = 9, .end2 = 5, .end15 = C_TZERO + 1, .endt = SNX_ARM_T, .end_poll = 9},
    {"CBZ not taken", .entry = C_TCBZ, .r0 = 1, .poll = 9, .end0 = 1, .end2 = 6, .end15 = C_TNONZERO + 1,
     .endt = SNX_ARM_T, .end_poll = 9},
    /* IT EQ leaves ITSTATE 0x08 in CPSR: its dispatch loop's, whatever the table has */
    {"an IT block's end", .entry = C_TIT, .poll = 9, .key = C_TIT + 2, .block = C_MOV, .table = 1, .end15 = C_TIT + 1,
     .endt = SNX_ARM_T | 0x800, .end_poll = 9},
};
#define CHAINED_CASES (sizeof chained_cases / sizeof *chained_cases)
static int chained_makes(uint32_t key) {
    return !(key & 1) || (key != C_TIT && key != C_TCBZ) || thumb2(); /* IT and CBZ are Thumb-2's */
}
#if defined(SNX_TEST_NATIVE)
static void swap_chained(void* state) {
    ChainedState* s = (ChainedState*)state;
    snx_test_swap_arm(&s->cpu);
    snx_test_sw32(&s->poll);
    snx_test_sw64((void*)&s->lookup);
    snx_test_sw64((void*)&s->direct);
}
#endif
static int run_chained(Back* x64) {
    SnxArmChainedOptions options = {
        .options = g_test.options, .poll = offsetof(ChainedState, poll), .lookup = offsetof(ChainedState, lookup)};
    unsigned char* object = NULL;
    size_t size = 0, failures = 0, ran = 0;
    char error[512] = "";
    SnxU64 addr[CHAINED_BLOCKS] = {0};
    const char* names[CHAINED_BLOCKS];
    char name[CHAINED_BLOCKS][24];
    size_t of[CHAINED_BLOCKS], n = 0;
    int rc = 1;
    /* options that are not these: a table and no poll, another memory, the plain ABI's number */
    SnxArmChainedOptions bad = options;
    bad.options.abi = SNX_ARM_CHAINED_OPTIONS_ABI, bad.poll = 0;
    int took = !back_(options)(x64, &bad.options);
    bad = options, bad.options.abi = SNX_ARM_CHAINED_OPTIONS_ABI, bad.options.memory = SNX_ARM_MEMORY_FLAT;
    took |= !back_(options)(x64, &bad.options);
    bad.options.abi = 3;
    took |= !back_(options)(x64, &bad.options);
    if (took) {
        fprintf(stderr, "FAIL: the " BACK_NAME " back end takes chained options it has no meaning for\n");
        ++failures;
    }
    for (size_t i = 0; i < CHAINED_BLOCKS; ++i) {
        const uint32_t key = chained_blocks[i].key, pc = key & ~1u;
        const int stub = key == C_STUB || key == C_TSTUB;
        size_t consumed = 0;
        if (!chained_makes(key)) continue;
        options.options.abi = SNX_ARM_CHAINED_OPTIONS_ABI;
        options.direct = stub ? offsetof(ChainedState, direct) : 0;
        options.import = stub ? CHAINED_IMPORT : 0;
        int made = !back_(options)(x64, &options.options);
        if (made)
            made = key & 1 ? back_(thumb_block)(x64, NULL, chained_blocks[i].code, chained_blocks[i].units, pc,
                                                &consumed, chained_next, NULL)
                           : back_(block)(x64, NULL, chained_blocks[i].code, chained_blocks[i].units, pc, &consumed,
                                          chained_next, NULL);
        if (!made || consumed != chained_blocks[i].units) {
            fprintf(stderr, "FAIL: the chained block at %08x: %s\n", key, back_(why)(x64));
            return 1;
        }
        snprintf(name[n], sizeof name[n], "%c_%08x", key & 1 ? 'T' : 'A', pc);
        names[n] = name[n];
        of[n++] = i;
    }
    back_(options)(x64, &g_test.options);
    if (back_(object)(x64, &object, &size, NULL)) {
        fprintf(stderr, "FAIL: the chained blocks' object\n");
        return 1;
    }
    SnxU64 loaded[CHAINED_BLOCKS] = {0};
#if defined(SNX_TEST_NATIVE)
    HostUc* uc = hostuc_new(BACK_UC, 1u << 20);
    ChainedState* t = uc ? (ChainedState*)hostuc_alloc(uc, sizeof *t) : NULL;
    SnxArmLookup* table = uc ? (SnxArmLookup*)hostuc_alloc(uc, SNX_ARM_LOOKUP_SLOTS * sizeof *table) : NULL;
    const int big = hostuc_big(BACK_UC);
    if (!t || !table) goto done;
    hostuc_swapper(uc, swap_chained, t);
    if (!hostuc_native(uc, "snx_arm_direct", (void*)snx_arm_direct) ||
        hostuc_load(uc, object, size, error, sizeof error))
        goto done;
#if defined(SNX_TEST_UNWINDS)
    hostuc_step(uc, 1); /* each frame unwound at every instruction, a tail call's way out of it among them */
#endif
    for (size_t k = 0; k < n; ++k)
        if (!(loaded[k] = hostuc_symbol(uc, names[k]))) goto done;
#else
    SnxCodeLoader* loader = NULL;
    ChainedState* t = (ChainedState*)malloc(sizeof *t);
    SnxArmLookup* table = (SnxArmLookup*)calloc(SNX_ARM_LOOKUP_SLOTS, sizeof *table);
    SnxLinkInput input = {.data = object, .size = size, .name = "chained"};
    if (!t || !table || !(loader = new_loader(error, sizeof error)) ||
        snx_code_define(loader, "snx_arm_direct", (SnxU64)(uintptr_t)snx_arm_direct) ||
        snx_code_load_private(loader, &input, 1, names, n, loaded, error, sizeof error))
        goto done;
#endif
    for (size_t k = 0; k < n; ++k) addr[of[k]] = loaded[k];
    for (size_t i = 0; i < CHAINED_CASES; ++i) {
        const ChainedCase* k = &chained_cases[i];
        SnxU64 entry = 0, target = 0;
        for (size_t b = 0; b < CHAINED_BLOCKS; ++b) {
            if (chained_blocks[b].key == k->entry) entry = addr[b];
            if (chained_blocks[b].key == k->block) target = addr[b];
        }
        if (!entry || (k->block && !target)) continue; /* a Thumb-2 one, where there is none */
        SnxArmLookup* slot = &table[(k->key >> 1) & (SNX_ARM_LOOKUP_SLOTS - 1)];
        *slot = (SnxArmLookup){k->key, (SnxArmBlock)(uintptr_t)target};
        *t = (ChainedState){0};
        t->cpu.r[0] = k->r0;
        t->cpu.r[1] = k->r1;
        t->cpu.r[15] = k->entry & ~1u;
        t->cpu.cpsr = 0x10u | k->cpsr | (k->entry & 1 ? SNX_ARM_T : 0);
        t->poll = k->poll;
        t->lookup = k->table ? table : NULL;
        t->direct = k->direct ? chained_direct : NULL;
        t->returns = k->returns;
        t->sets15 = k->sets15;
#if defined(SNX_TEST_NATIVE)
        if (big) snx_test_sw32(&slot->key), snx_test_sw64((void*)&slot->block);
        if (hostuc_call(uc, entry, (uint64_t)(uintptr_t)&t->cpu, 0, NULL)) {
            fprintf(stderr, "FAIL: %s: the code stopped\n", k->what);
            ++failures;
        }
#else
        run_block(entry, &t->cpu);
#endif
        *slot = (SnxArmLookup){0};
        ++ran;
        const uint32_t state = t->cpu.cpsr & (SNX_ARM_T | SG_TEST_IT_BITS);
        if (t->cpu.r[0] != k->end0 || t->cpu.r[2] != k->end2 || t->cpu.r[15] != k->end15 || state != k->endt ||
            t->poll != k->end_poll || t->cpu.exit != k->exit || (k->exit && t->cpu.syndrome != k->syndrome) ||
            t->calls != k->calls ||
            (k->calls && (t->import != CHAINED_IMPORT || t->r15 != (k->entry & ~1u) + (k->entry & 1 ? 2 : 4)))) {
            fprintf(
                stderr,
                "FAIL: %s: R0 %u, R2 %u, R15 %08x, T and IT %x, poll %d, exit %u, syndrome %u, calls %u (import %u, "
                "R15 %08x); wanted R0 %u, R2 %u, R15 %08x, T and IT %x, poll %d, exit %u, syndrome %u, calls %u\n",
                k->what, t->cpu.r[0], t->cpu.r[2], t->cpu.r[15], state, t->poll, t->cpu.exit, t->cpu.syndrome, t->calls,
                t->import, t->r15, k->end0, k->end2, k->end15, k->endt, k->end_poll, k->exit, k->syndrome, k->calls);
            ++failures;
        }
    }
    printf("%zu cases of chained blocks, %zu failures\n", ran, failures);
    g_test.failures += failures;
    rc = 0;
done:
    if (rc) fprintf(stderr, "FAIL: the chained blocks' object: %s\n", error);
    free(object);
#if defined(SNX_TEST_UNWINDS)
    if (uc) hostuc_step(uc, 0);
#endif
#if defined(SNX_TEST_NATIVE)
    if (uc) hostuc_free(uc);
#else
    free(t), free(table);
    if (loader) snx_code_loader_free(loader);
#endif
    return rc;
}

#if defined(SNX_TEST_ARM64WIN)
/* a block whose frame is past a page (vector floating-point additions, whose lanes are kept over the helper's calls): its
   prologue reads each page first, once, and it unwinds from every instruction. The results are the sums */
static int run_big_frame(Back* x64) {
    enum { WORDS = 40 };
    /* LDRB WZR, [X17]; CMP X17, X16 */
    static const unsigned char probe[] = {0x3F, 0x02, 0x40, 0x39, 0x3F, 0x02, 0x10, 0xEB};
    uint32_t words[WORDS + 1], sum[4];
    unsigned char* object = NULL;
    size_t consumed = 0, size = 0, probes = 0, bad = 0, unwound;
    char error[512] = "";
    SnxU64 addr = 0;
    for (unsigned k = 0; k < WORDS; ++k) words[k] = 0xF2000D44u; /* VADD.F32 Q0, Q0, Q2 */
    words[WORDS] = 0xE12FFF1Eu;                                  /* BX LR */
    for (unsigned e = 0; e < 4; ++e) {
        const float f = (float)(e + WORDS); /* Q0's lanes e, Q2's 1 */
        memcpy(&sum[e], &f, 4);
    }
    if (!back_(block)(x64, "B_big", (const unsigned char*)words, WORDS + 1, PC, &consumed, NULL, NULL) ||
        consumed != WORDS + 1 || back_(object)(x64, &object, &size, NULL))
        return fprintf(stderr, "FAIL: the block of a frame of more than a page: %s\n", back_(why)(x64)), 1;
    for (size_t k = 0; k + sizeof probe <= size; ++k) probes += !memcmp(object + k, probe, sizeof probe);
    HostUc* uc = hostuc_new(BACK_UC, 1u << 20);
    RunState* s = uc ? (RunState*)hostuc_alloc(uc, sizeof *s) : NULL;
    if (!s || !hostuc_native(uc, "snx_arm_fp_x64", (void*)counted_fp) ||
        hostuc_load(uc, object, size, error, sizeof error) || !(addr = hostuc_symbol(uc, "B_big"))) {
        fprintf(stderr, "FAIL: the object of a frame of more than a page: %s\n", error);
        free(object);
        hostuc_free(uc);
        return 1;
    }
    memset(s, 0, sizeof *s);
    s->cpu.r[15] = PC;
    s->cpu.r[14] = 0x300000u;
    s->cpu.cpsr = 0x10u;
    for (unsigned e = 0; e < 4; ++e) {
        const float f[2] = {(float)e, 1};
        uint32_t w[2];
        memcpy(w, f, sizeof w);
        s->cpu.d[e / 2] |= (uint64_t)w[0] << 32 * (e % 2);
        s->cpu.d[4 + e / 2] |= (uint64_t)w[1] << 32 * (e % 2);
    }
    hostuc_step(uc, 1);
    int wrong = hostuc_call(uc, addr, (uint64_t)(uintptr_t)&s->cpu, 0, NULL);
    unwound = hostuc_unwound(uc, &bad);
    wrong |= s->cpu.exit || s->cpu.r[15] != 0x300000u || memcmp(s->cpu.d, sum, sizeof sum) != 0 || probes != 1 || bad ||
             unwound < 4 * WORDS;
    if (wrong)
        fprintf(stderr,
                "FAIL: the block of a frame of more than a page: exit %u, R15 %x, Q0 %llx:%llx, %zu probes, %zu states "
                "unwound, %zu wrongly\n",
                s->cpu.exit, s->cpu.r[15], (unsigned long long)s->cpu.d[1], (unsigned long long)s->cpu.d[0], probes,
                unwound, bad);
    printf("a block of a frame of more than a page: %zu states unwound, %d failures\n", unwound, wrong);
    free(object);
    hostuc_free(uc);
    return wrong;
}
#endif
/* a block's address: the bus's open bus reads the code by its region, and in IWRAM by its halfword */
static uint32_t block_pc(int thumb) {
    static const uint32_t at[] = {0x1000, 0x2001000, 0x3001000, 0x3001002, 0x7000010, 0x8001000, 0x8001006};
    if (g_test.options.profile != SNX_ARM_PROFILE_ARM7TDMI) return PC;
    uint32_t pc = at[rnd() % (sizeof at / sizeof *at)];
    return thumb ? pc : pc & ~3u;
}
int main(int argc, char** argv) {
    char error[512];
    SnxIr* ir = NULL;
    g_test.states = 16;
    g_test.blocks = 6000;
    g_test.thumb = 4000;
    g_test.features = ~0u;
    g_test.seed = 0xa32;
    g_test.options.abi = SNX_ARM_CODEGEN_OPTIONS_ABI;
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
        else if (!strcmp(argv[i], "--callee") && i + 1 < argc) {
            g_test.callee = snx_read_file(argv[++i], &g_test.callee_size);
            if (!g_test.callee) return fprintf(stderr, "FAIL: cannot read %s\n", argv[i]), 1;
            g_test.rt[0] = (SnxArmRt){snx_test_undefined, snx_test_coproc};
            g_test.io[0] = snx_test_io;
#if !defined(SNX_TEST_NATIVE)
            return fprintf(stderr, "--callee %s: the x86-64 blocks run here, with the host's functions\n", argv[i]), 2;
#endif
        } else if (!strcmp(argv[i], "--dump"))
            g_test.dump = 1;
#if defined(SNX_TEST_UNWINDS)
        else if (!strcmp(argv[i], "--step"))
            g_test.step = 1;
#endif
        else if (!strcmp(argv[i], "--trace"))
            g_test.options.trace = offsetof(RunState, trace), g_test.options.skipped = offsetof(RunState, skipped);
        else if (!strcmp(argv[i], "--profile") && i + 1 < argc) {
            static const char* const names[] = {[SNX_ARM_PROFILE_A57] = "a57",
                                                [SNX_ARM_PROFILE_ARM11] = "arm11",
                                                [SNX_ARM_PROFILE_A9] = "a9",
                                                [SNX_ARM_PROFILE_ARM7TDMI] = "arm7tdmi",
                                                [SNX_ARM_PROFILE_ARM946E] = "arm946e"};
            unsigned k = 0;
            while (k < 5 && strcmp(argv[i + 1], names[k])) ++k;
            if (k == 5)
                return fprintf(stderr, "--profile %s is not a57, arm11, a9, arm7tdmi or arm946e\n", argv[i + 1]), 2;
            g_test.options.profile = (SnxArmProfile)k;
            ++i;
        } else if (!strcmp(argv[i], "--memory") && i + 1 < argc) {
            static const char* const names[] = {[SNX_ARM_MEMORY_FLAT] = "flat",
                                                [SNX_ARM_MEMORY_CHECKED] = "checked",
                                                [SNX_ARM_MEMORY_HOSTED] = "hosted"};
            unsigned k = 0;
            while (k < 3 && strcmp(argv[i + 1], names[k])) ++k;
            if (k == 3) return fprintf(stderr, "--memory %s is not flat, checked or hosted\n", argv[i + 1]), 2;
            g_test.options.memory = (SnxArmMemoryMode)k;
            ++i;
        } else if (!strcmp(argv[i], "--word") && i + 1 < argc && g_test.nonly < 64) {
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
            return fprintf(
                       stderr,
                       "usage: snx-test-arm-x64 [--profile NAME] [--memory NAME] [--trace] [--blocks N] [--thumb N] [--states N] [--pool N] [--features MASK] "
                       "[--seed N] [--object FILE] [--callee OBJECT] [--word HEX]... "
                       "[--halves HEX[,HEX]...]\n%s is not one of "
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
    back_begin();
#else
    if (!back_(available)()) {
        printf("SKIP: no " BACK_NAME " back end on this host\n");
        return 77;
    }
#endif
    if (!snx_arm_codegen_options_valid(&g_test.options))
        return fprintf(stderr, "FAIL: no block compiler takes this profile, memory and trace\n"), 2;
    if (g_test.callee && checked())
        return fprintf(stderr,
                       "--callee and --memory checked: the callback is the test's, which compiled code cannot call\n"),
               2;
    g_test.windowed = hosted() || legacy();
#if defined(_WIN32)
    if (g_test.windowed) {
        printf("SKIP: no window of guest memory of %llu bytes on Windows\n", (unsigned long long)WINDOW);
        return 77;
    }
#endif
    if (g_test.windowed && windows_new()) return 1;
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
    if (back_(options)(x64, &g_test.options)) {
        fprintf(stderr, "FAIL: the " BACK_NAME " back end takes no such options\n");
        return 1;
    }
    int plain = g_test.options.profile == SNX_ARM_PROFILE_A57 && g_test.options.memory == SNX_ARM_MEMORY_FLAT;
#if defined(SNX_TEST_NATIVE)
    back_(limit)(x64, g_test.pool, g_test.features | ~7u);
    size_t done = 0;
    for (int thumb = 0; thumb < 2; ++thumb)
        if (plain && !g_test.nonly && !g_test.nhalves && run_poll_chain(x64, thumb)) ++g_test.failures;
#if defined(SNX_TEST_UNWINDS)
    if (plain && !g_test.nonly && !g_test.nhalves && run_entries(x64)) ++g_test.failures;
#endif
#if defined(SNX_TEST_ARM64WIN)
    if (plain && !g_test.nonly && !g_test.nhalves && run_big_frame(x64)) ++g_test.failures;
#endif
#else
    back_(limit)(x64, g_test.pool, g_test.features | ~7u);
    size_t done = 0;
    /* the witnesses are the Cortex-A57's on flat memory */
    if (plain && !g_test.nonly && !g_test.nhalves && run_witnesses(ir, x64)) ++g_test.failures;
    if (plain && !g_test.nonly && !g_test.nhalves && run_endian_chain(x64)) ++g_test.failures;
    for (int thumb = 0; thumb < 2; ++thumb)
        if (plain && !g_test.nonly && !g_test.nhalves && run_poll_chain(x64, thumb)) ++g_test.failures;
    if (plain && !g_test.nonly && !g_test.nhalves && run_vrshl_witness(x64)) ++g_test.failures;
    if (!plain && !g_test.nonly && !g_test.nhalves && run_chains(x64)) ++g_test.failures;
#endif
    if (hosted() && !g_test.nonly && !g_test.nhalves && run_chained(x64)) ++g_test.failures;
    if (plain && !g_test.nonly && !g_test.nhalves && run_thumb_witnesses(ir, x64)) ++g_test.failures;
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
            b->n = rnd() % 3 == 0 ? 1 : 2 + (size_t)(rnd() % 5); /* MAX_WORDS is --word's */
            b->pc = block_pc(0);
            for (size_t k = 0; k < b->n; ++k) {
                const Class* c;
                do c = pick_class();
                while (!(b->words[k] = word_of(c)) ||
                       (no_exclusives() && (c->kind == SNX_A32_EXCLUSIVE || c->kind == SNX_A32_ACQUIRE_RELEASE)));
                ++g_test.kinds[decode(b->words[k]) & 63];
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
            blocks[i].pc = block_pc(1);
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
    if (g_test.hooked || g_test.transfers)
        printf("%zu calls of the bus hook, %zu of checked memory's callback\n", g_test.hooked, g_test.transfers);
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
