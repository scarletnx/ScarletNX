/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Shared AArch32 executor: runs A32 blocks and checks the DRY SVC surface
   zero-extends r0-r7 from 32-bit to 64-bit for the common HLE handler. */
#include "common/cpu/arm/checked.h"
#include "common/cpu/arm/codegen.h"
#include "common/cpu/arm/exec.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/mach/native.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    uint64_t number, arguments[6];
    int seen;
} Svc;

static int syscall(void* context, uint32_t imm, uint64_t number, const uint64_t arguments[6]) {
    Svc* s = (Svc*)context;
    (void)imm;
    s->number = number;
    for (unsigned i = 0; i < 6; i++) s->arguments[i] = arguments[i];
    s->seen = 1;
    return 1; /* stop */
}

/* Blocks forgotten in a range compile again from what memory holds there now, as when ldr:ro maps an NRO where it
   unmapped another, and every other block stays cached. 500 blocks at scattered addresses fill 1024 slots to just
   under the half the table grows at, so some share a home slot and sit further on, and every other block from
   FORGET_LO to FORGET_HI goes, one range each, from among them (blocks in a row would each have a home of their own:
   the hash spreads a run of addresses evenly) */
#define FORGET_BLOCKS 500
#define FORGET_SPAN   8192 /* 8-byte places for a block */
#define FORGET_LO     100
#define FORGET_HI     350
static void block_code(unsigned char* mem, uint32_t pc, unsigned value) {
    const uint32_t words[2] = {0xE3A00000u | (value & 0xFFu), 0xEF000000u}; /* mov r0, #value; svc #0 */
    for (unsigned k = 0; k < 8; k++) mem[pc + k] = (unsigned char)(words[k / 4] >> (k % 4 * 8));
}
/* the forget check's memory, out of the stack: the blocks' code, which places are taken, each block's address and the
   block first compiled there */
static struct {
    unsigned char mem[0x1000 + FORGET_SPAN * 8];
    unsigned char taken[FORGET_SPAN];
    uint32_t at[FORGET_BLOCKS];
    SnxArmBlock first[FORGET_BLOCKS];
} g_forget;
static int forget_test(void) {
    unsigned char* mem = g_forget.mem;
    unsigned char* taken = g_forget.taken;
    uint32_t* at = g_forget.at;
    SnxArmBlock* first = g_forget.first;
    SnxArmExecRange range = {0x1000, mem + 0x1000, FORGET_SPAN * 8};
    SnxArmExec* exec = snx_arm_exec_new(NULL, 0);
    uint32_t x = 12345;
    int failures = 0;
    if (!exec) return 1;
    for (int i = 0; i < FORGET_BLOCKS;) { /* the places, from a fixed sequence (an LCG) */
        x = x * 1103515245u + 12345u;
        uint32_t place = ((x & 0x7FFFFFFFu) >> 8) % FORGET_SPAN;
        if (taken[place]) continue;
        taken[place] = 1;
        at[i] = 0x1000u + place * 8;
        block_code(mem, at[i], (unsigned)i);
        i++;
    }
    for (int i = 0; i < FORGET_BLOCKS; i++) {
        first[i] = snx_arm_exec_block(exec, &range, 1, at[i], 0, mem);
        if (!first[i]) {
            fprintf(stderr, "FAIL: block %d does not compile\n", i);
            snx_arm_exec_free(exec);
            return 1;
        }
    }
    /* the odd blocks from FORGET_LO to FORGET_HI are another program's now */
    for (int i = FORGET_LO | 1; i < FORGET_HI; i += 2) {
        block_code(mem, at[i], (unsigned)i + 7);
        snx_arm_exec_forget(exec, at[i], at[i] + 8);
    }
    /* the blocks kept first, as nothing is compiled for them: a new block takes the first free slot of its probe, which
       would fill a hole that hid a kept block from its lookup */
    for (int k = 0; k < 2 * FORGET_BLOCKS; k++) {
        int i = k % FORGET_BLOCKS, moved = i >= FORGET_LO && i < FORGET_HI && (i & 1);
        uint32_t pc = at[i];
        if (moved != (k >= FORGET_BLOCKS)) continue;
        SnxArmBlock now = snx_arm_exec_block(exec, &range, 1, pc, 0, mem);
        if (!moved && now != first[i]) {
            fprintf(stderr, "FAIL: block %d, outside the range forgotten, was compiled again\n", i);
            failures++;
        }
        if (moved && now == first[i]) {
            fprintf(stderr, "FAIL: block %d, in the range forgotten, is still the one compiled before\n", i);
            failures++;
        }
        SnxArmCPU cpu = {0};
        Svc svc = {0};
        cpu.mem = mem;
        cpu.mem_size = sizeof g_forget.mem;
        cpu.cpsr = 0x10;
        cpu.r[15] = pc;
        (void)snx_arm_exec_run(exec, &cpu, &range, 1, syscall, &svc);
        unsigned want = (unsigned)(moved ? i + 7 : i) & 0xFFu;
        if (!svc.seen || cpu.r[0] != want) {
            fprintf(stderr, "FAIL: block %d ran r0 = %u, %u wanted\n", i, (unsigned)cpu.r[0], want);
            failures++;
        }
    }
    snx_arm_exec_free(exec);
    return failures;
}

/* Each profile and memory mode the executor compiles for, by the host's own back end where it has one: a store, a load
   of it back, a multiply that sets the flags and an SVC, in A32 and in Thumb, through what the mode reaches memory by
   (the bus hook, the checked callback), with the ARM7TDMI's trace */
typedef struct {
    union {
        SnxArmCPU cpu;
        SnxArmCheckedCPU checked;
    };
    uint32_t trace[16], skipped[1];
    uint32_t hook; /* the hook's word at 0x04000100 */
    unsigned hooked, accessed;
    unsigned char mem[0x3000];
} ModeState;
static uint32_t mode_io(SnxArmCPU* cpu, uint32_t address, uint32_t size_write, uint32_t value) {
    ModeState* s = (ModeState*)cpu;
    if (address != 0x04000100u) return 0;
    ++s->hooked;
    if (size_write & 0x100) s->hook = value;
    return size_write & 0x100 ? 0 : s->hook;
}
static uint32_t mode_access(void* context, uint32_t operation, uint32_t address, uint32_t size, void* bytes) {
    ModeState* s = (ModeState*)context;
    if (address > sizeof s->mem - size) return 1;
    ++s->accessed;
    if ((operation & 3) == SNX_ARM_ACCESS_WRITE)
        memcpy(s->mem + address, bytes, size);
    else
        memcpy(bytes, s->mem + address, size);
    return 0;
}
/* generated code has no type signature before its entry, which the function sanitizer's check would read (exec.c's) */
[[clang::no_sanitize("function")]] static void run_mode(SnxArmBlock block, SnxArmCPU* cpu) { block(cpu); }
static int modes_test(void) {
    static const uint32_t arm[] = {0xE5810000, 0xE5912000, 0xE0130290, 0xEF000000}; /* str r0, [r1]; ldr r2, [r1];
                                                                                       muls r3, r0, r2; svc #0 */
    static const uint16_t thumb[] = {0x6008, 0x680A, 0x4342,
                                     0xDF00}; /* str r0, [r1]; ldr r2, [r1]; muls r2, r0; svc #0 */
    static const struct {
        const char* name;
        SnxArmProfile profile;
        SnxArmMemoryMode memory;
        int trace;
    } modes[] = {
        {"a57 flat", SNX_ARM_PROFILE_A57, SNX_ARM_MEMORY_FLAT, 0},
        {"a9 flat", SNX_ARM_PROFILE_A9, SNX_ARM_MEMORY_FLAT, 0},
        {"arm11 flat", SNX_ARM_PROFILE_ARM11, SNX_ARM_MEMORY_FLAT, 0},
        {"arm946e", SNX_ARM_PROFILE_ARM946E, SNX_ARM_MEMORY_FLAT, 0},
        {"arm7tdmi", SNX_ARM_PROFILE_ARM7TDMI, SNX_ARM_MEMORY_FLAT, 0},
        {"arm7tdmi traced", SNX_ARM_PROFILE_ARM7TDMI, SNX_ARM_MEMORY_FLAT, 1},
        {"a57 hosted", SNX_ARM_PROFILE_A57, SNX_ARM_MEMORY_HOSTED, 0},
        {"a9 hosted", SNX_ARM_PROFILE_A9, SNX_ARM_MEMORY_HOSTED, 0},
        {"arm11 hosted", SNX_ARM_PROFILE_ARM11, SNX_ARM_MEMORY_HOSTED, 0},
        {"a57 checked", SNX_ARM_PROFILE_A57, SNX_ARM_MEMORY_CHECKED, 0},
        {"a9 checked", SNX_ARM_PROFILE_A9, SNX_ARM_MEMORY_CHECKED, 0},
        {"arm11 checked", SNX_ARM_PROFILE_ARM11, SNX_ARM_MEMORY_CHECKED, 0},
    };
    int native = snx_native_arm_available();
    int failures = 0;
    for (size_t m = 0; m < sizeof modes / sizeof *modes; ++m)
        for (int t = 0; t < 2; ++t) {
            int bus = modes[m].profile == SNX_ARM_PROFILE_ARM946E || modes[m].profile == SNX_ARM_PROFILE_ARM7TDMI;
            int checked = modes[m].memory == SNX_ARM_MEMORY_CHECKED;
            SnxArmCodegenOptions options = {SNX_ARM_CODEGEN_OPTIONS_ABI, modes[m].profile, modes[m].memory, 0, 0};
            if (modes[m].trace)
                options.trace = offsetof(ModeState, trace), options.skipped = offsetof(ModeState, skipped);
            SnxArmExec* exec = snx_arm_exec_new(NULL, 0);
            if (!exec) return failures + 1;
            size_t consumed = 0, count = t ? sizeof thumb / sizeof *thumb : sizeof arm / sizeof *arm;
            SnxArmBlock block = snx_arm_exec_compile_ex(exec, t ? (const void*)thumb : (const void*)arm, count, 0x1000,
                                                        t, &options, &consumed);
            const char* why = "";
            size_t refused = snx_arm_exec_refused(exec, &why);
            ModeState s = {0};
            for (size_t i = 0; i < count; ++i) /* the code in guest memory, which a checked block fetches */
                for (unsigned k = 0; k < (t ? 2u : 4u); ++k)
                    s.mem[0x1000 + i * (t ? 2 : 4) + k] = (unsigned char)((t ? thumb[i] : arm[i]) >> 8 * k);
            s.cpu.mem = s.mem;
            s.cpu.mem_size = bus ? 1ull << 32 : sizeof s.mem; /* the hook's address is in range */
            s.cpu.cpsr = t ? 0x30 : 0x10;
            s.cpu.r[0] = 0x80000003u;
            s.cpu.r[1] = bus ? 0x04000100u : 0x2000u;
            s.cpu.r[15] = 0x1000;
            if (bus) s.cpu.io = mode_io;
            s.checked.abi = SNX_ARM_CHECKED_ABI;
            s.checked.context = &s;
            s.checked.access = mode_access;
            if (block) run_mode(block, &s.cpu);
            uint32_t product = t ? s.cpu.r[2] : s.cpu.r[3], end = 0x1000 + (uint32_t)count * (t ? 2 : 4);
            uint32_t traced = modes[m].trace ? 0x04000100u : 0;
            if (!block || consumed != count || s.cpu.exit != SNX_ARM_SVC || product != 0x80000003u * 0x80000003u ||
                s.cpu.r[15] != end || s.hooked != (bus ? 2u : 0) || s.accessed != (checked ? 6u : 0) ||
                s.trace[0] != traced || s.trace[1] != traced) {
                fprintf(
                    stderr,
                    "FAIL: %s %s: %s, consumed %zu of %zu, exit %u, product %08x, R15 %08x, hooked %u, accessed %u, "
                    "trace %08x %08x\n",
                    modes[m].name, t ? "Thumb" : "A32", block ? "ran" : "not compiled", consumed, count, s.cpu.exit,
                    product, s.cpu.r[15], s.hooked, s.accessed, s.trace[0], s.trace[1]);
                ++failures;
            }
            if (refused != (native ? 0u : 1u)) {
                fprintf(stderr, "FAIL: %s %s: %zu blocks asked of LLVM: %s\n", modes[m].name, t ? "Thumb" : "A32",
                        refused, why);
                ++failures;
            }
            snx_arm_exec_free(exec);
        }
    return failures;
}

int main(void) {
    /* mov r0,#0x80000000; mov r1,#0xFFFFFFFF; mov r7,#7; svc #0 */
    static const uint32_t code[] = {0xE3A00102, 0xE3E01000, 0xE3A07007, 0xEF000000};
    unsigned char mem[0x10000];
    memset(mem, 0, sizeof mem);
    /* the guest's code is little-endian whatever the host's byte order */
    for (unsigned i = 0; i < sizeof code; i++) mem[0x1000 + i] = (unsigned char)(code[i / 4] >> (i % 4 * 8));
    SnxArmCPU cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.mem = mem;
    cpu.mem_size = sizeof mem;
    cpu.cpsr = 0x10; /* USR, A32 */
    cpu.r[15] = 0x1000;
    SnxArmExecRange range = {0x1000, mem + 0x1000, sizeof code};
    SnxArmExec* exec = snx_arm_exec_new(NULL, 0);
    if (!exec) {
        fprintf(stderr, "FAIL: cannot create executor\n");
        return 1;
    }
    Svc svc = {0};
    int exit = snx_arm_exec_run(exec, &cpu, &range, 1, syscall, &svc);
    int failures = 0;
    /* a program of the A57's blocks is the host's back end's, where it has one, and LLVM never starts for it */
    const char* why = "";
    size_t refused = snx_arm_exec_refused(exec, &why);
    if (snx_native_arm_available() ? refused || snx_ir_started() : !refused || !snx_ir_started()) {
        fprintf(stderr, "FAIL: a program of the A57's blocks: %zu asked of LLVM (%s), LLVM started %d\n", refused, why,
                snx_ir_started());
        failures++;
    }
    /* a checked block loads without the compiler runtime's stack probe, which the JIT loader does not define */
    static const uint32_t checked_code[] = {0xE1A00000, 0xEAFFFFFE}; /* mov r0, r0; b . */
    size_t consumed = 0;
    if (!snx_arm_exec_compile(exec, checked_code, 2, 0x1000, 0, SNX_ARM_PROFILE_ARM11, 1, &consumed)) {
        fprintf(stderr, "FAIL: a checked block does not compile and load\n");
        failures++;
    }
    snx_arm_exec_free(exec);
    if (!svc.seen) {
        fprintf(stderr, "FAIL: syscall not reached (exit %d)\n", exit);
        failures++;
    } else {
        /* r0=0x80000000 and r1=0xFFFFFFFF must arrive ZERO-extended, r7 as number. */
        if (svc.arguments[0] != 0x80000000ull) {
            fprintf(stderr, "FAIL: args[0]=%llx expected 0x80000000 (zero-extend)\n",
                    (unsigned long long)svc.arguments[0]);
            failures++;
        }
        if (svc.arguments[1] != 0xFFFFFFFFull) {
            fprintf(stderr, "FAIL: args[1]=%llx expected 0xffffffff (zero-extend)\n",
                    (unsigned long long)svc.arguments[1]);
            failures++;
        }
        if (svc.number != 7) {
            fprintf(stderr, "FAIL: number=%llu expected 7\n", (unsigned long long)svc.number);
            failures++;
        }
    }
    /* The Vita's A9 profile: a store with writeback into the register it stores (a Vita game's Thumb
       str r0, [r0, #28]!, then A32's) stores the register as it was, then writes the base back */
    static const unsigned char thumb_store[] = {0x40, 0xf8, 0x1c, 0x0f, 0x00, 0xdf};     /* str.w; svc #0 */
    static const unsigned char a32_store[] = {0x1c, 0x00, 0xa0, 0xe5, 0x00, 0x00, 0x00, 0xef}; /* str; svc #0 */
    for (int thumb = 1; thumb >= 0; --thumb) {
        const unsigned char* bytes = thumb ? thumb_store : a32_store;
        size_t size = thumb ? sizeof thumb_store : sizeof a32_store;
        memset(mem, 0, sizeof mem);
        memcpy(mem + 0x1000, bytes, size);
        memset(&cpu, 0, sizeof cpu);
        cpu.mem = mem;
        cpu.mem_size = sizeof mem;
        cpu.cpsr = thumb ? 0x30 : 0x10;
        cpu.r[15] = 0x1000;
        cpu.r[0] = 0x2000;
        SnxArmExecRange store_range = {0x1000, mem + 0x1000, size};
        SnxArmExec* a9 = snx_arm_exec_new(NULL, 0);
        if (!a9) return 1;
        snx_arm_exec_set_profile(a9, SNX_ARM_PROFILE_A9);
        Svc stop = {0};
        exit = snx_arm_exec_run(a9, &cpu, &store_range, 1, syscall, &stop);
        snx_arm_exec_free(a9);
        uint32_t stored = mem[0x201c] | (uint32_t)mem[0x201d] << 8 | (uint32_t)mem[0x201e] << 16 |
                          (uint32_t)mem[0x201f] << 24;
        if (!stop.seen || stored != 0x2000 || cpu.r[0] != 0x201c) {
            fprintf(stderr, "FAIL: A9 %s store with writeback: exit %d, stored %08x, r0 %08x\n", thumb ? "Thumb" : "A32",
                    exit, (unsigned)stored, (unsigned)cpu.r[0]);
            failures++;
        }
    }
    /* TBB [r0, r1] of a table entry past guest memory stops with a data abort, as a load does, not with a read of the
       host's memory */
    static const unsigned char tbb[] = {0xd0, 0xe8, 0x01, 0xf0}; /* tbb [r0, r1] */
    memset(mem, 0, sizeof mem);
    memcpy(mem + 0x1000, tbb, sizeof tbb);
    cpu = (SnxArmCPU){0};
    cpu.mem = mem;
    cpu.mem_size = sizeof mem;
    cpu.cpsr = 0x30;
    cpu.r[15] = 0x1000;
    cpu.r[0] = (uint32_t)sizeof mem;
    SnxArmExecRange tbb_range = {0x1000, mem + 0x1000, sizeof tbb};
    SnxArmExec* tbb_exec = snx_arm_exec_new(NULL, 0);
    if (!tbb_exec) return 1;
    Svc tbb_stop = {0};
    exit = snx_arm_exec_run(tbb_exec, &cpu, &tbb_range, 1, syscall, &tbb_stop);
    snx_arm_exec_free(tbb_exec);
    if (exit != SNX_ARM_DATA_ABORT || cpu.r[15] != 0x1000) {
        fprintf(stderr, "FAIL: TBB past guest memory: exit %d, R15 %08x; a data abort (%d) at 00001000 wanted\n", exit,
                (unsigned)cpu.r[15], SNX_ARM_DATA_ABORT);
        failures++;
    }
    /* a block that calls the runtime library's helpers: VADD of a NaN (the VFP's cold path), MRC of CP15's thread
       pointer; the native back end's object names them, and the loader has them */
    static const uint32_t helpers[] = {0xEE300A81u, 0xEE1D0F70u, 0xEF000000u}; /* vadd.f32 s0, s1, s2; mrc; svc #0 */
    memset(mem, 0, sizeof mem);
    for (unsigned i = 0; i < sizeof helpers; i++) mem[0x1000 + i] = (unsigned char)(helpers[i / 4] >> (i % 4 * 8));
    for (int nan = 0; nan < 2; nan++) {
        cpu = (SnxArmCPU){0};
        cpu.mem = mem;
        cpu.mem_size = sizeof mem;
        cpu.cpsr = 0x10;
        cpu.r[15] = 0x1000;
        cpu.tpidruro = 0x12345678;
        cpu.d[0] = (uint64_t)0x3F800000u << 32;     /* S0 0, S1 1.0 */
        cpu.d[1] = nan ? 0x7FC12345u : 0x40000000u; /* S2 a NaN, or 2.0 */
        SnxArmExecRange helper_range = {0x1000, mem + 0x1000, sizeof helpers};
        SnxArmExec* helper_exec = snx_arm_exec_new(NULL, 0);
        if (!helper_exec) return 1;
        Svc helper_stop = {0};
        (void)snx_arm_exec_run(helper_exec, &cpu, &helper_range, 1, syscall, &helper_stop);
        const char* helper_why = "";
        size_t helper_refused = snx_arm_exec_refused(helper_exec, &helper_why);
        uint32_t s0 = (uint32_t)cpu.d[0], want = nan ? 0x7FC12345u : 0x40400000u; /* the NaN itself, or 3.0 */
        if (!helper_stop.seen || s0 != want || cpu.r[0] != 0x12345678u) {
            fprintf(stderr, "FAIL: VADD and MRC: S0 %08x, %08x wanted, R0 %08x\n", (unsigned)s0, (unsigned)want,
                    (unsigned)cpu.r[0]);
            failures++;
        }
        if (snx_native_arm_available() && helper_refused) {
            fprintf(stderr, "FAIL: a block with helpers asked of LLVM: %s\n", helper_why);
            failures++;
        }
        snx_arm_exec_free(helper_exec);
    }
    failures += forget_test();
    failures += modes_test();
    if (!failures) puts("A32 executor and DRY zero-extending SVC passed");
    return failures ? 1 : 0;
}
