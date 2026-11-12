/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* AArch32 blocks compiled while a program runs: each block is emitted into a module of the executor's LLVM context,
   compiled to an object with its target machine (snx_ir_module_object) and loaded into a code arena by the runtime
   JIT's loader (code_loader.c), as the A64 JIT's units are. The objects call the runtime library's helpers, which the
   loader is given by name: no MCJIT, and no symbols the program must export. On PowerPC64 the helpers are the 32-bit
   modules' own (armlib.c), loaded into the arena first, as library.c's arena_load loads rtlib.c with the A64 code:
   the arena has one TOC, which a direct call into the runtime's copies (another TOC) would leave behind */
#include "common/cpu/arm/exec.h"
#include "common/cpu/arm/llvm.h"
#include "common/cpu/arm/simd.h"
#include "common/cpu/recompiler/llvm/code_loader.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>

/* a block's -O level, the A64 JIT's default */
#define BLOCK_OPT '0'

typedef struct {
    uint64_t key; /* (pc << 1 | thumb) + 1, 0 for an empty slot */
    SnxArmBlock fn;
} CachedBlock;
struct SnxArmExec {
    SnxIr* ir; /* the context and target machine every block's module is made with */
    SnxCodeLoader* loader;
    CachedBlock* blocks; /* open addressing, a power of two slots, at most half full */
    size_t count, capacity, compiles;
    SnxArmProfile profile; /* the one snx_arm_exec_block compiles for, A57 unless set */
};

static size_t code_span(const SnxArmExecRange* ranges, size_t n, uint32_t pc, unsigned unit) {
    for (size_t i = 0; i < n; i++) {
        uint64_t lo = ranges[i].address, hi = lo + ranges[i].size;
        if (pc >= lo && pc < hi) return (size_t)((hi - pc) / unit);
    }
    return 0;
}

static size_t home_of(const SnxArmExec* e, uint64_t key) {
    return (size_t)((key * 0x9E3779B97F4A7C15ULL) >> 20) & (e->capacity - 1);
}
static size_t slot_of(const SnxArmExec* e, uint64_t key) {
    size_t i = home_of(e, key);
    while (e->blocks[i].key && e->blocks[i].key != key) i = (i + 1) & (e->capacity - 1);
    return i;
}
/* empties slot i. A lookup stops at the first empty slot, so each later entry of its run whose probe passed i moves
   back into the hole, which moves on to where it was */
static void drop_at(SnxArmExec* e, size_t i) {
    size_t mask = e->capacity - 1, j = i;
    for (;;) {
        j = (j + 1) & mask;
        if (!e->blocks[j].key) break;
        size_t home = home_of(e, e->blocks[j].key);
        if (i <= j ? i < home && home <= j : i < home || home <= j) continue; /* its probe starts after the hole */
        e->blocks[i] = e->blocks[j];
        i = j;
    }
    e->blocks[i].key = 0;
    e->blocks[i].fn = NULL;
    e->count--;
}
void snx_arm_exec_forget(SnxArmExec* e, uint64_t lo, uint64_t hi) {
    size_t i = 0;
    while (i < e->capacity) {
        uint64_t key = e->blocks[i].key, pc = (key - 1) >> 1;
        if (key && pc >= lo && pc < hi)
            drop_at(e, i); /* an entry from further on may have moved to i: it is looked at next */
        else
            i++;
    }
}

/* room for one more block: 0, or -1 out of memory */
static int reserve(SnxArmExec* e) {
    CachedBlock* old = e->blocks;
    size_t old_capacity = e->capacity;
    if (e->count + 1 <= e->capacity / 2) return 0;
    e->capacity = old_capacity ? old_capacity * 2 : 256;
    e->blocks = calloc(e->capacity, sizeof *e->blocks);
    if (!e->blocks) {
        e->blocks = old;
        e->capacity = old_capacity;
        return -1;
    }
    for (size_t i = 0; i < old_capacity; i++)
        if (old[i].key) e->blocks[slot_of(e, old[i].key)] = old[i];
    free(old);
    return 0;
}

SnxArmBlock snx_arm_exec_compile(SnxArmExec* e, const void* code, size_t count, uint32_t pc, int thumb,
                                 SnxArmProfile profile, int memory, size_t* consumed) {
    SnxArmCodegenOptions options = {SNX_ARM_CODEGEN_OPTIONS_ABI, profile, (SnxArmMemoryMode)memory, 0, 0};
    return snx_arm_exec_compile_ex(e, code, count, pc, thumb, &options, consumed);
}
SnxArmBlock snx_arm_exec_compile_ex(SnxArmExec* e, const void* code, size_t count, uint32_t pc, int thumb,
                                    const SnxArmCodegenOptions* options, size_t* consumed) {
    size_t size = 0;
    char name[48], error[512];
    *consumed = 0;
    /* a name of its own each time: the same pc compiles again when its code changes */
    snprintf(name, sizeof name, "a32_%08x_%d_%zu", pc, thumb, e->compiles++);
    LLVMModuleRef module = LLVMModuleCreateWithNameInContext(name, e->ir->ctx);
    LLVMSetTarget(module, LLVMGetTarget(e->ir->mod));
    LLVMSetDataLayout(module, LLVMGetDataLayoutStr(e->ir->mod));
    LLVMValueRef fn = thumb ? snx_thumb_llvm_block_ex(module, name, (const uint16_t*)code, count, pc, consumed, options)
                            : snx_arm_llvm_block_ex(module, name, (const uint32_t*)code, count, pc, consumed, options);
    unsigned char* object = NULL;
    int rc = fn ? snx_ir_module_object(e->ir, module, BLOCK_OPT, &object, &size, error, sizeof error) : -1;
    LLVMDisposeModule(module);
    if (rc) return NULL;
    SnxLinkInput input;
    const char* names[1] = {name};
    SnxU64 address = 0;
    input.data = object;
    input.size = size;
    input.name = name;
    rc = snx_code_load(e->loader, &input, 1, names, 1, &address, error, sizeof error);
    free(object);
    return rc || !address ? NULL : (SnxArmBlock)(uintptr_t)address;
}

SnxArmBlock snx_arm_exec_block(SnxArmExec* e, const SnxArmExecRange* ranges, size_t nranges, uint32_t pc, int thumb,
                               const unsigned char* base) {
    uint64_t key = (((uint64_t)pc << 1) | (unsigned)thumb) + 1;
    if (e->capacity && e->blocks[slot_of(e, key)].key == key) return e->blocks[slot_of(e, key)].fn;
    size_t consumed = 0, span = code_span(ranges, nranges, pc, thumb ? 2 : 4);
    if (!span || reserve(e)) return NULL;
    if (span > 65536) span = 65536; /* the block compiler's limit */
    /* the block compiler reads host integers, and the guest's code is little-endian */
    const void* code = base + pc;
    void* swapped = NULL;
    if (SNX_HOST_BE) {
        swapped = malloc(span * (thumb ? 2 : 4));
        if (!swapped) return NULL;
        for (size_t i = 0; i < span; ++i)
            if (thumb)
                ((uint16_t*)swapped)[i] = snx_ld_le16(base + pc + i * 2);
            else
                ((uint32_t*)swapped)[i] = snx_ld_le32(base + pc + i * 4);
        code = swapped;
    }
    SnxArmBlock block = snx_arm_exec_compile(e, code, span, pc, thumb, e->profile, 0, &consumed);
    free(swapped);
    if (!block) return NULL;
    size_t slot = slot_of(e, key);
    e->blocks[slot].key = key;
    e->blocks[slot].fn = block;
    e->count++;
    return block;
}

/* DRY: present A32 r0-r5 zero-extended to the 64-bit syscall surface, and r7
   as the number. The same HLE handler A64 uses then sees 64-bit values. */
static int run_syscall(SnxArmSyscallFn syscall, void* context, SnxArmCPU* cpu, uint32_t imm) {
    uint64_t arguments[6];
    for (unsigned i = 0; i < 6; i++) arguments[i] = cpu->r[i];
    return syscall(context, imm, cpu->r[7], arguments);
}

SnxArmExec* snx_arm_exec_new(char* error, size_t error_size) {
    char ignored[512]; /* for a caller that wants no reason: the loader writes one whatever its size */
    if (!error_size) error = ignored, error_size = sizeof ignored;
    error[0] = 0;
    if (snx_ir_init(error, error_size)) return NULL;
    SnxArmExec* e = calloc(1, sizeof *e);
    if (!e) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    e->ir = snx_ir_new_jit("a32exec", BLOCK_OPT, error, error_size);
    if (e->ir) e->loader = snx_code_loader_new(0, error, error_size);
#if defined(__powerpc64__)
    /* the helpers blocks call, loaded into the arena, whose globals they resolve to. They reach the runtime only
       through cpu->rt, pointer calls that LLVM makes with r2 saved */
    if (e->loader) {
        SnxLinkInput armlib = {0};
        unsigned char* object = NULL;
        armlib.name = "armlib";
        if (snx_ir_armlib_object(BLOCK_OPT, &object, &armlib.size, error, error_size)) {
            snx_arm_exec_free(e);
            return NULL;
        }
        armlib.data = object;
        int rc = snx_code_load(e->loader, &armlib, 1, NULL, 0, NULL, error, error_size);
        free(object);
        if (rc) {
            snx_arm_exec_free(e);
            return NULL;
        }
    }
    if (!e->loader) {
#else
    /* the runtime library's functions blocks call (simd.h, arm.h), by the names the block compiler declares */
    if (!e->loader || snx_code_define(e->loader, "snx_arm_fp", (SnxU64)(uintptr_t)snx_arm_fp) ||
        snx_code_define(e->loader, "snx_arm_crypto", (SnxU64)(uintptr_t)snx_arm_crypto) ||
        snx_code_define(e->loader, "snx_arm_undefined", (SnxU64)(uintptr_t)snx_arm_undefined) ||
        snx_code_define(e->loader, "snx_arm_coproc", (SnxU64)(uintptr_t)snx_arm_coproc)) {
#endif
        if (error_size && !error[0]) snprintf(error, error_size, "cannot name the runtime library's functions");
        snx_arm_exec_free(e);
        return NULL;
    }
    return e;
}

void snx_arm_exec_set_profile(SnxArmExec* e, SnxArmProfile profile) { e->profile = profile; }
void snx_arm_exec_free(SnxArmExec* e) {
    if (!e) return;
    snx_code_loader_free(e->loader);
    snx_ir_free(e->ir);
    free(e->blocks);
    free(e);
}

int snx_arm_exec_run(SnxArmExec* e, SnxArmCPU* cpu, const SnxArmExecRange* ranges, size_t range_count,
                     SnxArmSyscallFn syscall, void* context) {
    /* The code image base: blocks decode from it; generated code uses cpu->mem. */
    const unsigned char* base = range_count ? (const unsigned char*)cpu->mem : NULL;
    while (cpu->exit == SNX_ARM_OK) {
        uint32_t pc = cpu->r[15];
        int thumb = (cpu->cpsr & SNX_ARM_T) != 0;
        SnxArmBlock block = snx_arm_exec_block(e, ranges, range_count, pc, thumb, base);
        if (!block) {
            cpu->exit = SNX_ARM_BAD_ENTRY;
            break;
        }
        block(cpu);
        if (cpu->exit == SNX_ARM_SVC && syscall) {
            cpu->exit = SNX_ARM_OK;
            if (run_syscall(syscall, context, cpu, cpu->syndrome)) break;
        }
    }
    return (int)cpu->exit;
}
