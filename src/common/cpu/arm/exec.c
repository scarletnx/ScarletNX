/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* AArch32 blocks compiled while a program runs: each block is an object of this host's own block compiler
   (recompiler/mach/native.h), of any profile and memory mode, loaded into a code arena by the runtime JIT's loader
   (code_loader.c), as the A64 JIT's units are. What that compiler refuses, and everything on a host without one, goes
   to the fallback (recompiler/fallback.h): LLVM, started when first needed, or none, which refuses it. The objects
   call the runtime library's helpers, which the loader is given by name: no MCJIT, and no symbols the program must
   export. Where the loader has no stub for a call out of reach of its arena, the helpers are the 32-bit modules' own
   (armlib.c), loaded into the arena first, as library.c's arena_load loads rtlib.c with the A64 code: on PowerPC64 the
   arena has one TOC, which a direct call into the runtime's copies (another TOC) would leave behind; RISC-V, LoongArch,
   MIPS64 and s390x call within their own displacement's reach. An object the loader does not take is as one the
   compiler refuses: the fallback's to make */
#include "common/cpu/arm/exec.h"
#include "common/cpu/arm/checked.h"
#include "common/cpu/arm/codegen.h"
#include "common/cpu/arm/simd.h"
#include "common/cpu/recompiler/fallback.h"
#include "common/cpu/recompiler/loader/code_loader.h"
#include "common/cpu/recompiler/rtlib/rtlib_object.h"
#include "common/cpu/recompiler/mach/native.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* a block's -O level, the A64 JIT's default */
#define BLOCK_OPT '0'
/* 1 where the helpers' object goes into the arena: the loader has no stub for a call out of reach of it */
#if defined(__powerpc64__) || defined(__riscv) || defined(__loongarch__) || defined(__mips64) || defined(__s390x__)
#define ARENA_ARMLIB 1
#else
#define ARENA_ARMLIB 0
#endif
/* how many reasons a block was left to the fallback for are said, each once */
#define REASONS 32

typedef struct {
    uint64_t key; /* (pc << 1 | thumb) + 1, 0 for an empty slot */
    SnxArmBlock fn;
} CachedBlock;
struct SnxArmExec {
    SnxNativeArm* native; /* the host's own block compiler, NULL where it has none */
    SnxFallbackArm* fallback; /* makes the blocks that compiler leaves */
    SnxCodeLoader* loader;
    CachedBlock* blocks; /* open addressing, a power of two slots, at most half full */
    size_t count, capacity, compiles;
    size_t refused; /* the blocks the native compiler did not make, and why the last of them */
    char why[96];
    char said[REASONS][96]; /* the reasons said, each once */
    size_t said_count;
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
/* the block's object as name, made by the host's own compiler: 1, or 0 with why in e->why where there is none or it
   refuses the block */
static int native_object(SnxArmExec* e, const char* name, const void* code, size_t count, uint32_t pc, int thumb,
                         const SnxArmCodegenOptions* options, size_t* consumed, unsigned char** object, size_t* size) {
    if (!e->native) return 0;
    if (snx_native_arm_options(e->native, options)) {
        snprintf(e->why, sizeof e->why, "options no block compiler takes");
        return 0;
    }
    /* it reads the code as guest memory holds it, little-endian, and the caller's are host integers */
    const unsigned char* bytes = (const unsigned char*)code;
    unsigned char* swapped = NULL;
    if (SNX_HOST_BE) {
        bytes = swapped = (unsigned char*)malloc(count * (thumb ? 2 : 4));
        if (!swapped) {
            snprintf(e->why, sizeof e->why, "out of memory");
            return 0;
        }
        for (size_t i = 0; i < count; ++i)
            if (thumb)
                snx_st_le16(swapped + i * 2, ((const uint16_t*)code)[i]);
            else
                snx_st_le32(swapped + i * 4, ((const uint32_t*)code)[i]);
    }
    int made = thumb ? snx_native_thumb_block(e->native, name, bytes, count, pc, consumed, NULL, NULL)
                     : snx_native_arm_block(e->native, name, bytes, count, pc, consumed, NULL, NULL);
    free(swapped);
    if (!made) {
        snprintf(e->why, sizeof e->why, "%s", snx_native_arm_why(e->native));
        return 0;
    }
    if (snx_native_arm_object(e->native, object, size, NULL)) {
        snprintf(e->why, sizeof e->why, "out of memory");
        return 0;
    }
    return 1;
}
/* a reason a block is not the native compiler's (what says the fallback makes it, or nothing does), said once each */
static void say(SnxArmExec* e, const char* what, const char* reason) {
    for (size_t i = 0; i < e->said_count; ++i)
        if (!strncmp(e->said[i], reason, sizeof e->said[i] - 1)) return;
    if (e->said_count == REASONS) return;
    snprintf(e->said[e->said_count++], sizeof e->said[0], "%s", reason);
    fprintf(stderr, "A32 JIT, back end %s: %s: %s%s\n", snx_native_name(), what, reason,
            e->said_count == REASONS ? " (any further reason is not said)" : "");
}
/* the block's object as name, the fallback's: 0, -1 with why in error */
static int fallback_object(SnxArmExec* e, const char* name, const void* code, size_t count, uint32_t pc, int thumb,
                           const SnxArmCodegenOptions* options, size_t* consumed, unsigned char** object, size_t* size,
                           char* error, size_t error_size) {
    return snx_fallback_arm_block(e->fallback, name, code, count, pc, thumb, options, e->why, consumed, error,
                                  error_size) ||
                   snx_fallback_arm_object(e->fallback, object, size, error, error_size)
               ? -1
               : 0;
}
/* the block the object defines as name, loaded and the object freed: NULL with why in error */
static SnxArmBlock load_object(SnxArmExec* e, unsigned char* object, size_t size, const char* name, char* error,
                               size_t error_size) {
    SnxLinkInput input = {.data = object, .size = size, .name = name};
    const char* names[1] = {name};
    SnxU64 address = 0;
    int rc = snx_code_load(e->loader, &input, 1, names, 1, &address, error, error_size);
    free(object);
    if (!rc && !address) snprintf(error, error_size, "the object has no %s", name);
    return rc ? NULL : (SnxArmBlock)(uintptr_t)address;
}
SnxArmBlock snx_arm_exec_compile_ex(SnxArmExec* e, const void* code, size_t count, uint32_t pc, int thumb,
                                    const SnxArmCodegenOptions* options, size_t* consumed) {
    size_t size = 0;
    char name[24], error[512] = ""; /* a native block's name is of 23 characters at most */
    unsigned char* object = NULL;
    SnxArmBlock block = NULL;
    *consumed = 0;
    /* a name of its own each time: the same pc compiles again when its code changes */
    snprintf(name, sizeof name, "a32_%zx", e->compiles++);
    if (native_object(e, name, code, count, pc, thumb, options, consumed, &object, &size)) {
        if ((block = load_object(e, object, size, name, error, sizeof error))) return block;
        snprintf(e->why, sizeof e->why, "its object does not load: %.60s", error);
        object = NULL, size = 0, *consumed = 0;
    }
    ++e->refused;
    if (e->native && snx_fallback_available()) say(e, "LLVM makes a block it leaves", e->why);
    if (fallback_object(e, name, code, count, pc, thumb, options, consumed, &object, &size, error, sizeof error) ||
        !(block = load_object(e, object, size, name, error, sizeof error)))
        say(e, "nothing makes a block", error);
    return block;
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

/* the helpers blocks call, loaded into the arena, whose globals they resolve to. They reach the runtime only through
   cpu->rt, pointer calls that LLVM makes with r2 saved on PowerPC64. 0, or -1 with why in error */
static int load_armlib(SnxArmExec* e, char* error, size_t error_size) {
    SnxLinkInput armlib = {.data = (unsigned char*)snx_armlib_object, /* rtlib_object.h's */
                           .size = (size_t)snx_armlib_object_size,
                           .name = "armlib"};
    return snx_code_load(e->loader, &armlib, 1, NULL, 0, NULL, error, error_size);
}
/* the runtime library's functions blocks call (simd.h, arm.h, checked.h), by the names the block compilers declare */
static int define_helpers(SnxArmExec* e) {
    return snx_code_define(e->loader, "snx_arm_fp", (SnxU64)(uintptr_t)snx_arm_fp) ||
           snx_code_define(e->loader, "snx_arm_fp_x64", (SnxU64)(uintptr_t)snx_arm_fp_x64) ||
           snx_code_define(e->loader, "snx_arm_fp_env", (SnxU64)(uintptr_t)snx_arm_fp_env) ||
           snx_code_define(e->loader, "snx_arm_io_read", (SnxU64)(uintptr_t)snx_arm_io_read) ||
           snx_code_define(e->loader, "snx_arm_io_write", (SnxU64)(uintptr_t)snx_arm_io_write) ||
           snx_code_define(e->loader, "snx_arm_checked_fetch", (SnxU64)(uintptr_t)snx_arm_checked_fetch) ||
           snx_code_define(e->loader, "snx_arm_checked_access", (SnxU64)(uintptr_t)snx_arm_checked_access) ||
           snx_code_define(e->loader, "snx_arm_checked_strex", (SnxU64)(uintptr_t)snx_arm_checked_strex) ||
           snx_code_define(e->loader, "snx_arm_crypto", (SnxU64)(uintptr_t)snx_arm_crypto) ||
           snx_code_define(e->loader, "snx_arm_undefined", (SnxU64)(uintptr_t)snx_arm_undefined) ||
           snx_code_define(e->loader, "snx_arm_coproc", (SnxU64)(uintptr_t)snx_arm_coproc) ||
           snx_code_define(e->loader, "snx_arm_multiply_carry", (SnxU64)(uintptr_t)snx_arm_multiply_carry);
}
SnxArmExec* snx_arm_exec_new(char* error, size_t error_size) {
    char ignored[512]; /* for a caller that wants no reason: the loader writes one whatever its size */
    if (!error_size) error = ignored, error_size = sizeof ignored;
    error[0] = 0;
    SnxArmExec* e = calloc(1, sizeof *e);
    if (!e) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    if (snx_native_arm_available() && !(e->native = snx_native_arm_new())) {
        snprintf(error, error_size, "out of memory for the %s back end's block compiler", snx_native_name());
        snx_arm_exec_free(e);
        return NULL;
    }
    if (!e->native && !snx_native_floor(e->why, sizeof e->why))
        snprintf(e->why, sizeof e->why, "no native back end on this host");
    if (!(e->fallback = snx_fallback_arm_new("a32exec", BLOCK_OPT, 1))) {
        snprintf(error, error_size, "out of memory");
        snx_arm_exec_free(e);
        return NULL;
    }
    /* without one the fallback makes every block: a host it has no code generator for is said here, not at each of
       them */
    if (e->native || !snx_fallback_arm_start(e->fallback, error, error_size))
        e->loader = snx_code_loader_new(0, error, error_size);
    if (!e->loader || (ARENA_ARMLIB ? load_armlib(e, error, error_size) : define_helpers(e))) {
        if (!error[0]) snprintf(error, error_size, "cannot name the runtime library's functions");
        snx_arm_exec_free(e);
        return NULL;
    }
    return e;
}

void snx_arm_exec_set_profile(SnxArmExec* e, SnxArmProfile profile) { e->profile = profile; }
size_t snx_arm_exec_refused(const SnxArmExec* e, const char** why) {
    if (why) *why = e->why;
    return e->refused;
}
void snx_arm_exec_free(SnxArmExec* e) {
    if (!e) return;
    snx_code_loader_free(e->loader);
    snx_native_arm_free(e->native);
    snx_fallback_arm_free(e->fallback);
    free(e->blocks);
    free(e);
}

/* A block is generated code, with no type signature before its entry, which the function sanitizer's check of an
   indirect call reads: the 8 bytes before the callee. Before the first block of a code arena those are below the
   arena, readable or not as the system placed the mappings made after it. So the call is made unchecked */
[[clang::no_sanitize("function")]] static void run_block(SnxArmBlock block, SnxArmCPU* cpu) { block(cpu); }

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
        run_block(block, cpu);
        if (cpu->exit == SNX_ARM_SVC && syscall) {
            cpu->exit = SNX_ARM_OK;
            if (run_syscall(syscall, context, cpu, cpu->syndrome)) break;
        }
    }
    return (int)cpu->exit;
}
