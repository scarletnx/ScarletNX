/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The offline compiler, part of the driver: the image's blocks to a native module, as the 3DS driver's is
   (3ds/aot/compiler.c). The blocks are found by following the code from what is known to be code: module_start, the
   code pointers the relocations wrote, and the hints of earlier runs. Each block is compiled once into a scratch
   module to learn where the block compiler ends it, which says where the next ones are (cpu/arm/successors.h); the
   workers then compile them for real, in chunks, in parallel */
#include "vita/aot/module.h"
#include "common/cpu/arm/llvm.h"
#include "common/cpu/arm/successors.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/llvm/link.h"
#include "common/cpu/recompiler/compile_thread.h"
#include "common/utilities/common.h"
#include "common/platform/file.h"
#include "common/utilities/endian.h"
#include "snx_settings.h"
#include <llvm-c/Core.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#define CHUNK_BLOCKS 512u
/* the most blocks one block goes on to, a branch's target and the code after it */
#define SUCCESSORS   2u

typedef struct {
    uint32_t key, units;
    uint32_t next[SUCCESSORS]; /* the keys it may go on to */
    uint32_t next_count;
} Block;
typedef struct {
    const VitaImage* image;
    Block* blocks;
    size_t count, capacity;
    uint32_t* set; /* every key ever queued: open addressing, 0 empty (no code is at address 0) */
    size_t set_count, set_capacity;
    uint32_t* work;
    size_t work_count, work_capacity;
    bool failed;
} Discovery;

static void block_name(char out[24], uint32_t key) {
    snprintf(out, 24, "v_%c_%08x", key & 1 ? 't' : 'a', (unsigned)(key & ~1u));
}
/* the code from address to the end of its executable segment, and how many units of it (at most the block limit) */
static const unsigned char* code_at(const VitaImage* image, uint32_t address, int thumb, size_t* units) {
    const VitaSegment* s = vita_image_segment(image, address, thumb ? 2 : 4);
    *units = 0;
    if (!s || !(s->flags & 1)) return NULL;
    *units = (s->address + s->size - address) / (thumb ? 2 : 4);
    if (*units > VITA_BLOCK_LIMIT) *units = VITA_BLOCK_LIMIT;
    return s->bytes + (address - s->address);
}
static bool insert(Discovery* d, uint32_t key) {
    if ((d->set_count + 1) * 2 > d->set_capacity) {
        size_t capacity = d->set_capacity ? d->set_capacity * 2 : 4096;
        uint32_t* set = calloc(capacity, sizeof *set);
        if (!set) {
            d->failed = true;
            return false;
        }
        for (size_t j = 0; j < d->set_capacity; ++j)
            if (d->set[j]) {
                size_t i = (size_t)(d->set[j] * 0x9E3779B1u) & (capacity - 1);
                while (set[i]) i = (i + 1) & (capacity - 1);
                set[i] = d->set[j];
            }
        free(d->set);
        d->set = set, d->set_capacity = capacity;
    }
    size_t i = (size_t)(key * 0x9E3779B1u) & (d->set_capacity - 1);
    while (d->set[i]) {
        if (d->set[i] == key) return false;
        i = (i + 1) & (d->set_capacity - 1);
    }
    d->set[i] = key;
    ++d->set_count;
    return true;
}
static void queue(void* context, uint32_t address, int thumb) {
    Discovery* d = context;
    size_t units;
    if (address & (thumb ? 1u : 3u) || !code_at(d->image, address, thumb, &units) || !units) return;
    uint32_t key = address | (thumb ? 1u : 0u);
    if (!insert(d, key)) return;
    if (d->work_count == d->work_capacity) {
        size_t capacity = d->work_capacity ? d->work_capacity * 2 : 1024;
        uint32_t* work = realloc(d->work, capacity * sizeof *work);
        if (!work) {
            d->failed = true;
            return;
        }
        d->work = work, d->work_capacity = capacity;
    }
    d->work[d->work_count++] = key;
}
/* a block's successors as discovery meets them, queued and kept on the block for chaining */
typedef struct {
    Discovery* d;
    Block* block;
} Found;
static void found(void* context, uint32_t address, int thumb) {
    Found* f = context;
    size_t units;
    queue(f->d, address, thumb);
    if (address & (thumb ? 1u : 3u) || !code_at(f->d->image, address, thumb, &units) || !units) return;
    uint32_t key = address | (thumb ? 1u : 0u);
    Block* b = f->block;
    for (uint32_t i = 0; i < b->next_count; ++i)
        if (b->next[i] == key) return;
    if (b->next_count < SUCCESSORS) b->next[b->next_count++] = key;
}
static void queue_pointer(Discovery* d, uint32_t pointer) { queue(d, pointer & ~1u, (int)(pointer & 1)); }
/* the block compiler's input: the guest's little-endian code as host integers, the caller's to free */
static void* units_of(const unsigned char* code, size_t count, int thumb) {
    if (thumb) {
        uint16_t* h = malloc(count * sizeof *h);
        for (size_t i = 0; h && i < count; ++i) h[i] = snx_ld_le16(code + i * 2);
        return h;
    }
    uint32_t* w = malloc(count * sizeof *w);
    for (size_t i = 0; w && i < count; ++i) w[i] = snx_ld_le32(code + i * 4);
    return w;
}
static LLVMValueRef emit(LLVMModuleRef module, const VitaImage* image, uint32_t key, size_t count, const char* name,
                         size_t* consumed) {
    size_t available;
    int thumb = (int)(key & 1);
    const unsigned char* code = code_at(image, key & ~1u, thumb, &available);
    if (!code || count > available) return NULL;
    void* input = units_of(code, count, thumb);
    if (!input) return NULL;
    /* hosted (llvm.h): the run loop is the only caller, and the guest's 4 GiB window is followed by pages that fault
       (VITA_SPACE) */
    const SnxArmCodegenOptions options = {SNX_ARM_CODEGEN_OPTIONS_ABI, SNX_ARM_PROFILE_A9, SNX_ARM_MEMORY_HOSTED};
    LLVMValueRef fn = thumb ? snx_thumb_llvm_block_ex(module, name, input, count, key & ~1u, consumed, &options)
                            : snx_arm_llvm_block_ex(module, name, input, count, key & ~1u, consumed, &options);
    free(input);
    return fn;
}
static bool discover(Discovery* d, const char* folder, char* error, size_t error_size) {
    const VitaImage* image = d->image;
    size_t hint_count = 0, refused = 0;
    uint32_t* hints = vita_hints_read(folder, &hint_count);
    if (hint_count) printf("Seeding discovery with %zu blocks earlier runs compiled at run time\n", hint_count);
    queue_pointer(d, image->module_start);
    if (image->module_stop) queue_pointer(d, image->module_stop);
    for (size_t i = 0; i < image->seed_count; ++i) queue_pointer(d, image->seeds[i]);
    for (size_t i = 0; i < hint_count; ++i) queue_pointer(d, hints[i]);
    free(hints);
    LLVMContextRef context = LLVMContextCreate();
    LLVMModuleRef scratch = LLVMModuleCreateWithNameInContext("vita-discovery", context);
    while (d->work_count && !d->failed) {
        uint32_t key = d->work[--d->work_count];
        size_t count, consumed = 0;
        const unsigned char* code = code_at(image, key & ~1u, (int)(key & 1), &count);
        LLVMValueRef fn = emit(scratch, image, key, count, "block", &consumed);
        if (!fn || !consumed) { /* not code after all (data a pointer led to): the run-time compiler has it if not */
            ++refused;
            continue;
        }
        LLVMDeleteFunction(fn);
        if (d->count == d->capacity) {
            size_t capacity = d->capacity ? d->capacity * 2 : 4096;
            Block* grown = realloc(d->blocks, capacity * sizeof *grown);
            if (!grown) {
                d->failed = true;
                break;
            }
            d->blocks = grown, d->capacity = capacity;
        }
        d->blocks[d->count++] = (Block){.key = key, .units = (uint32_t)consumed};
        Found f = {d, &d->blocks[d->count - 1]};
        snx_arm_successors(SNX_ARM_PROFILE_A9, key & ~1u, code, consumed, count, (int)(key & 1), found, &f);
    }
    LLVMDisposeModule(scratch);
    LLVMContextDispose(context);
    if (d->failed) {
        snprintf(error, error_size, "out of memory discovering the code");
        return false;
    }
    printf("Discovered %zu blocks (%zu addresses were not code)\n", d->count, refused);
    return true;
}
static int by_key(const void* a, const void* b) {
    uint32_t x = ((const Block*)a)->key, y = ((const Block*)b)->key;
    return (x > y) - (x < y);
}

/* a module made for this machine as the backend makes the Switch's (ir.c): the target, CPU and ABI the helpers
   (armlib.c) are compiled for, which the module is linked with */
static LLVMModuleRef new_module(SnxIr* ir) {
    LLVMModuleRef m = LLVMModuleCreateWithNameInContext("vita-aot", ir->ctx);
    LLVMSetTarget(m, LLVMGetTarget(ir->mod));
    LLVMSetDataLayout(m, LLVMGetDataLayoutStr(ir->mod));
    return m;
}

/* an import's stub, whose svc block calls the import directly (stub_block) */
typedef struct Stub {
    uint32_t key, import;
} Stub;
static int stub_order(const void* a, const void* b) {
    uint32_t x = ((const Stub*)a)->key, y = ((const Stub*)b)->key;
    return (x > y) - (x < y);
}
/* the stubs sorted by key, the ones two imports share left out. The caller frees them */
static Stub* stubs_make(const VitaImage* image, size_t* count) {
    Stub* all = malloc((image->import_count ? image->import_count : 1) * sizeof *all);
    size_t n = 0;
    *count = 0;
    if (!all) return NULL;
    for (uint32_t i = 0; i < image->import_count; ++i) all[n++] = (Stub){image->imports[i].stub, i};
    qsort(all, n, sizeof *all, stub_order);
    size_t kept = 0;
    for (size_t i = 0; i < n; ++i) {
        bool shared = (i && all[i - 1].key == all[i].key) || (i + 1 < n && all[i + 1].key == all[i].key);
        if (!shared) all[kept++] = all[i];
    }
    *count = kept;
    return all;
}
/* the chunks shared out to a worker per host core, each with an SnxIr of its own */
typedef struct {
    const VitaImage* image;
    const Block* blocks;
    SnxLinkInput* objects; /* chunk i's object */
    const Stub* stubs;     /* the import stubs by key */
    size_t stub_count;
    size_t count;
    unsigned chunks, next, finished;
    bool failed;
    char error[512];
#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
} Work;
static uint32_t stub_import(const Work* w, uint32_t key) {
    size_t lo = 0, hi = w->stub_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (w->stubs[mid].key == key) return w->stubs[mid].import;
        if (w->stubs[mid].key < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return UINT32_MAX;
}
static void work_lock(Work* w) {
#ifdef _WIN32
    EnterCriticalSection(&w->lock);
#else
    pthread_mutex_lock(&w->lock);
#endif
}
static void work_unlock(Work* w) {
#ifdef _WIN32
    LeaveCriticalSection(&w->lock);
#else
    pthread_mutex_unlock(&w->lock);
#endif
}
/* Block chaining. A block that ends where another block of the module starts goes on to it by a tail call, instead
   of returning to the runtime's loop to look it up. Each return of the block becomes a return when it set an exit
   (an SVC, a fault), else for each successor a musttail call when r15 and the Thumb state are its own, else a return.
   A successor at or before the block's start (every loop has one) first counts down the thread's poll, and the block
   returns when it runs out, so the loop gets to run the vblank and the other threads. A branch to no successor goes
   through the runtime's block table (chain_lookup) */
static bool has_block(const Block* blocks, size_t count, uint32_t key) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (blocks[mid].key == key) return true;
        if (blocks[mid].key < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return false;
}
/* the function of the block at key, declared when this module lacks it, for a musttail call. It keeps the caller's
   parameter attributes, and a hosted block's cpu is noalias */
static LLVMValueRef block_target(SnxIr* ir, LLVMModuleRef mod, uint32_t key) {
    char name[24];
    block_name(name, key);
    LLVMValueRef target = LLVMGetNamedFunction(mod, name);
    if (!target) {
        LLVMTypeRef block_type = LLVMFunctionType(ir->t_void, &ir->t_ptr, 1, 0);
        target = LLVMAddFunction(mod, name, block_type);
        LLVMSetVisibility(target, LLVMHiddenVisibility);
        LLVMAddAttributeAtIndex(target, 1 /* first parameter */,
                                LLVMCreateEnumAttribute(ir->ctx, LLVMGetEnumAttributeKindForName("noalias", 7), 0));
    }
    return target;
}
static LLVMValueRef cpu_field(SnxIr* ir, LLVMValueRef cpu, size_t offset) {
    return LLVMBuildGEP2(ir->b, ir->t_i8, cpu, (LLVMValueRef[]){ir_i64(ir, offset)}, 1, "");
}
/* The end of a block's tail when no successor matched, an indirect branch. The target is looked up in the runtime's
   block table (VitaLookup), which the run loop fills as it dispatches, and the block goes on to it by a musttail call
   when it is there and no IT block is under way. Else, or with no table, it returns to the run loop. A target at or
   before the block's own start counts down the poll as a backward successor does. Every cycle of blocks has one
   such edge, so a loop made of indirect branches still ends the slice */
static void chain_lookup(SnxIr* ir, LLVMValueRef fn, LLVMValueRef cpu, uint32_t key, LLVMValueRef pc,
                         LLVMValueRef cpsr, LLVMValueRef thumb, LLVMBasicBlockRef out) {
    LLVMTypeRef block_type = LLVMFunctionType(ir->t_void, &ir->t_ptr, 1, 0);
    LLVMBasicBlockRef probe = LLVMAppendBasicBlockInContext(ir->ctx, fn, "probe"),
                      hit = LLVMAppendBasicBlockInContext(ir->ctx, fn, "hit"),
                      run = LLVMAppendBasicBlockInContext(ir->ctx, fn, "run");
    LLVMValueRef table = LLVMBuildLoad2(ir->b, ir->t_ptr, cpu_field(ir, cpu, VITA_LOOKUP_OFFSET), "");
    LLVMBuildCondBr(ir->b, LLVMBuildICmp(ir->b, LLVMIntNE, table, LLVMConstNull(ir->t_ptr), ""), probe, out);
    LLVMPositionBuilderAtEnd(ir->b, probe);
    LLVMValueRef target = LLVMBuildOr(
        ir->b, pc, LLVMBuildSelect(ir->b, LLVMBuildICmp(ir->b, LLVMIntNE, thumb, ir_i32(ir, 0), ""), ir_i32(ir, 1),
                                   ir_i32(ir, 0), ""), "");
    LLVMValueRef slot = LLVMBuildZExt(
        ir->b, LLVMBuildAnd(ir->b, LLVMBuildLShr(ir->b, target, ir_i32(ir, 1), ""), ir_i32(ir, VITA_LOOKUP_SLOTS - 1), ""),
        ir->t_i64, "");
    LLVMValueRef entry = LLVMBuildGEP2(ir->b, ir->t_i8, table,
                                       (LLVMValueRef[]){LLVMBuildMul(ir->b, slot, ir_i64(ir, sizeof(VitaLookup)), "")},
                                       1, "");
    LLVMValueRef seen = LLVMBuildLoad2(ir->b, ir->t_i32, entry, "");
    LLVMValueRef block = LLVMBuildLoad2(ir->b, ir->t_ptr, cpu_field(ir, entry, offsetof(VitaLookup, block)), "");
    /* ITSTATE sits in CPSR bits 26 and 25 and bits 15 to 10, and a hosted block is entered with it clear (llvm.h) */
    LLVMValueRef plain = LLVMBuildICmp(ir->b, LLVMIntEQ, LLVMBuildAnd(ir->b, cpsr, ir_i32(ir, 0x0600fc00u), ""),
                                       ir_i32(ir, 0), "");
    LLVMValueRef found = LLVMBuildAnd(ir->b, LLVMBuildICmp(ir->b, LLVMIntEQ, seen, target, ""),
                                      LLVMBuildICmp(ir->b, LLVMIntNE, block, LLVMConstNull(ir->t_ptr), ""), "");
    LLVMBuildCondBr(ir->b, LLVMBuildAnd(ir->b, found, plain, ""), hit, out);
    LLVMPositionBuilderAtEnd(ir->b, hit);
    LLVMValueRef at = cpu_field(ir, cpu, VITA_POLL_OFFSET);
    LLVMValueRef back = LLVMBuildICmp(ir->b, LLVMIntULE, pc, ir_i32(ir, key & ~1u), "");
    LLVMValueRef left = LLVMBuildSub(ir->b, LLVMBuildLoad2(ir->b, ir->t_i32, at, ""),
                                     LLVMBuildZExt(ir->b, back, ir->t_i32, ""), "");
    LLVMBuildStore(ir->b, left, at);
    LLVMBuildCondBr(ir->b, LLVMBuildICmp(ir->b, LLVMIntSGT, left, ir_i32(ir, 0), ""), run, out);
    LLVMPositionBuilderAtEnd(ir->b, run);
    LLVMValueRef site = LLVMBuildCall2(ir->b, block_type, block, &cpu, 1, "");
    LLVMSetTailCallKind(site, LLVMTailCallKindMustTail);
    LLVMAddCallSiteAttribute(site, 1 /* first parameter */,
                             LLVMCreateEnumAttribute(ir->ctx, LLVMGetEnumAttributeKindForName("noalias", 7), 0));
    LLVMBuildRetVoid(ir->b);
}
static void chain_block(SnxIr* ir, LLVMModuleRef mod, LLVMValueRef fn, uint32_t key, const uint32_t* next,
                        size_t count) {
    LLVMTypeRef block_type = LLVMFunctionType(ir->t_void, &ir->t_ptr, 1, 0);
    LLVMValueRef cpu = LLVMGetParam(fn, 0);
    LLVMValueRef* rets = malloc((LLVMCountBasicBlocks(fn) + 1) * sizeof *rets);
    size_t nrets = 0;
    if (!rets) return;
    for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn); bb; bb = LLVMGetNextBasicBlock(bb)) {
        LLVMValueRef t = LLVMGetBasicBlockTerminator(bb);
        if (t && LLVMGetInstructionOpcode(t) == LLVMRet) rets[nrets++] = t;
    }
    /* one tail for all of them, which takes a block's exits back out */
    LLVMBasicBlockRef tail = LLVMAppendBasicBlockInContext(ir->ctx, fn, "chain"),
                      go = LLVMAppendBasicBlockInContext(ir->ctx, fn, "next"),
                      out = LLVMAppendBasicBlockInContext(ir->ctx, fn, "out");
    for (size_t i = 0; i < nrets; ++i) {
        LLVMPositionBuilderBefore(ir->b, rets[i]);
        LLVMBuildBr(ir->b, tail);
        LLVMInstructionEraseFromParent(rets[i]);
    }
    free(rets);
    LLVMPositionBuilderAtEnd(ir->b, out);
    LLVMBuildRetVoid(ir->b);
    LLVMPositionBuilderAtEnd(ir->b, tail);
    LLVMValueRef c = LLVMBuildLoad2(ir->b, ir->t_i32, cpu_field(ir, cpu, offsetof(SnxArmCPU, exit)), "");
    c = LLVMBuildICmp(ir->b, LLVMIntEQ, c, ir_i32(ir, SNX_ARM_OK), "");
    LLVMBuildCondBr(ir->b, c, go, out);
    LLVMPositionBuilderAtEnd(ir->b, go);
    LLVMValueRef pc = LLVMBuildLoad2(ir->b, ir->t_i32, cpu_field(ir, cpu, offsetof(SnxArmCPU, r) + 15 * 4), "");
    LLVMValueRef cpsr = LLVMBuildLoad2(ir->b, ir->t_i32, cpu_field(ir, cpu, offsetof(SnxArmCPU, cpsr)), "");
    LLVMValueRef t = LLVMBuildAnd(ir->b, cpsr, ir_i32(ir, SNX_ARM_T), "");
    for (size_t k = 0; k < count; ++k) {
        LLVMBasicBlockRef call = LLVMAppendBasicBlockInContext(ir->ctx, fn, "call"),
                          miss = LLVMAppendBasicBlockInContext(ir->ctx, fn, "other");
        c = LLVMBuildAnd(ir->b, LLVMBuildICmp(ir->b, LLVMIntEQ, pc, ir_i32(ir, next[k] & ~1u), ""),
                         LLVMBuildICmp(ir->b, LLVMIntEQ, t, ir_i32(ir, next[k] & 1 ? SNX_ARM_T : 0), ""), "");
        LLVMBuildCondBr(ir->b, c, call, miss);
        LLVMPositionBuilderAtEnd(ir->b, call);
        if ((next[k] & ~1u) <= (key & ~1u)) {
            LLVMBasicBlockRef run = LLVMAppendBasicBlockInContext(ir->ctx, fn, "run");
            LLVMValueRef at = cpu_field(ir, cpu, VITA_POLL_OFFSET);
            LLVMValueRef left = LLVMBuildSub(ir->b, LLVMBuildLoad2(ir->b, ir->t_i32, at, ""), ir_i32(ir, 1), "");
            LLVMBuildStore(ir->b, left, at);
            LLVMBuildCondBr(ir->b, LLVMBuildICmp(ir->b, LLVMIntSGT, left, ir_i32(ir, 0), ""), run, out);
            LLVMPositionBuilderAtEnd(ir->b, run);
        }
        LLVMValueRef site = LLVMBuildCall2(ir->b, block_type, block_target(ir, mod, next[k]), &cpu, 1, "");
        LLVMSetTailCallKind(site, LLVMTailCallKindMustTail);
        LLVMBuildRetVoid(ir->b);
        LLVMPositionBuilderAtEnd(ir->b, miss);
    }
    chain_lookup(ir, fn, cpu, key, pc, cpsr, t, out);
}
/* The block of an import's stub, which is the stub's svc instruction. Where the run loop would take the SVC and then
   run the stub's bx lr, this calls the import through the thread's direct function and goes on to the bx lr block
   (next, 0 when the module lacks it) when the call needs nothing from the loop. Without the function it is the SVC
   block as the block compiler makes it, with the immediate (the import's index) in the syndrome */
static void stub_block(SnxIr* ir, LLVMModuleRef mod, LLVMValueRef fn, uint32_t import, uint32_t after,
                       uint32_t next) {
    LLVMTypeRef block_type = LLVMFunctionType(ir->t_void, &ir->t_ptr, 1, 0);
    LLVMTypeRef params[2] = {ir->t_ptr, ir->t_i32};
    LLVMTypeRef call_type = LLVMFunctionType(ir->t_i32, params, 2, 0);
    /* the function stays, as blocks of the chunk may already call it. Its body goes, last instruction first */
    for (LLVMBasicBlockRef bb = LLVMGetLastBasicBlock(fn); bb; bb = LLVMGetPreviousBasicBlock(bb))
        for (LLVMValueRef in = LLVMGetLastInstruction(bb); in;) {
            LLVMValueRef before = LLVMGetPreviousInstruction(in);
            if (LLVMGetTypeKind(LLVMTypeOf(in)) != LLVMVoidTypeKind) LLVMReplaceAllUsesWith(in, LLVMGetUndef(LLVMTypeOf(in)));
            LLVMInstructionEraseFromParent(in);
            in = before;
        }
    while (LLVMGetFirstBasicBlock(fn)) LLVMDeleteBasicBlock(LLVMGetFirstBasicBlock(fn));
    LLVMValueRef cpu = LLVMGetParam(fn, 0);
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(ir->ctx, fn, "entry"),
                      plain = LLVMAppendBasicBlockInContext(ir->ctx, fn, "svc"),
                      fast = LLVMAppendBasicBlockInContext(ir->ctx, fn, "call"),
                      go = LLVMAppendBasicBlockInContext(ir->ctx, fn, "next"),
                      out = LLVMAppendBasicBlockInContext(ir->ctx, fn, "out");
    LLVMPositionBuilderAtEnd(ir->b, entry);
    LLVMValueRef hook = LLVMBuildLoad2(ir->b, ir->t_ptr, cpu_field(ir, cpu, VITA_DIRECT_OFFSET), "");
    LLVMBuildCondBr(ir->b, LLVMBuildICmp(ir->b, LLVMIntEQ, hook, LLVMConstNull(ir->t_ptr), ""), plain, fast);
    LLVMPositionBuilderAtEnd(ir->b, plain);
    LLVMBuildStore(ir->b, ir_i32(ir, import), cpu_field(ir, cpu, offsetof(SnxArmCPU, syndrome)));
    LLVMBuildStore(ir->b, ir_i32(ir, SNX_ARM_SVC), cpu_field(ir, cpu, offsetof(SnxArmCPU, exit)));
    LLVMBuildStore(ir->b, ir_i32(ir, after), cpu_field(ir, cpu, offsetof(SnxArmCPU, r) + 15 * 4));
    LLVMBuildRetVoid(ir->b);
    LLVMPositionBuilderAtEnd(ir->b, fast);
    /* r15 is past the svc while the import runs, as it is when the loop takes the SVC */
    LLVMBuildStore(ir->b, ir_i32(ir, after), cpu_field(ir, cpu, offsetof(SnxArmCPU, r) + 15 * 4));
    LLVMValueRef args[2] = {cpu, ir_i32(ir, import)};
    LLVMValueRef done = LLVMBuildCall2(ir->b, call_type, hook, args, 2, "");
    LLVMBuildCondBr(ir->b, LLVMBuildICmp(ir->b, LLVMIntNE, done, ir_i32(ir, 0), ""), next ? go : out, out);
    LLVMPositionBuilderAtEnd(ir->b, go);
    if (next) {
        LLVMValueRef site = LLVMBuildCall2(ir->b, block_type, block_target(ir, mod, next), &cpu, 1, "");
        LLVMSetTailCallKind(site, LLVMTailCallKindMustTail);
    }
    LLVMBuildRetVoid(ir->b);
    LLVMPositionBuilderAtEnd(ir->b, out);
    LLVMBuildRetVoid(ir->b);
}
static bool chunk(SnxIr* ir, Work* w, unsigned index, char* error, size_t size) {
    LLVMModuleRef mod = new_module(ir);
    size_t begin = (size_t)index * CHUNK_BLOCKS, end = begin + CHUNK_BLOCKS;
    if (end > w->count) end = w->count;
    bool ok = true;
    LLVMValueRef* fns = calloc(end - begin + 1, sizeof *fns);
    if (!fns) {
        snprintf(error, size, "out of memory");
        ok = false;
    }
    for (size_t i = begin; ok && i < end; ++i) {
        char name[24];
        size_t consumed = 0;
        uint32_t key = w->blocks[i].key;
        block_name(name, key);
        LLVMValueRef fn = emit(mod, w->image, key, w->blocks[i].units, name, &consumed);
        if (!fn || consumed != w->blocks[i].units) {
            snprintf(error, size, "the block at 0x%08x compiled differently from its discovery", (unsigned)key);
            ok = false;
            break;
        }
        LLVMSetVisibility(fn, LLVMHiddenVisibility);
        fns[i - begin] = fn;
    }
    /* every block of the chunk exists now, so a successor in it is found by name, and one in another chunk is a
       declaration the linker resolves */
    for (size_t i = begin; ok && i < end; ++i) {
        uint32_t next[SUCCESSORS], key = w->blocks[i].key;
        size_t n = 0;
        uint32_t import = w->blocks[i].units == 1 ? stub_import(w, key) : UINT32_MAX;
        if (import != UINT32_MAX) {
            /* an import's stub, which the block compiler made the svc block of, is replaced by the direct call */
            uint32_t after = (key & ~1u) + (key & 1 ? 2u : 4u), after_key = after | (key & 1);
            stub_block(ir, mod, fns[i - begin], import, after,
                       has_block(w->blocks, w->count, after_key) ? after_key : 0);
            continue;
        }
        for (uint32_t k = 0; k < w->blocks[i].next_count; ++k)
            if (has_block(w->blocks, w->count, w->blocks[i].next[k])) next[n++] = w->blocks[i].next[k];
        chain_block(ir, mod, fns[i - begin], key, next, n);
    }
    free(fns);
    unsigned char* object = NULL;
    ok = ok && !snx_ir_module_object(ir, mod, '1', &object, &w->objects[index].size, error, size);
    w->objects[index].data = object;
    LLVMDisposeModule(mod);
    return ok;
}
static void worker(Work* w) {
    char error[512] = "";
    SnxIr* ir = snx_ir_new("vita-aot", '1', error, sizeof error);
    bool ok = ir != NULL;
    for (;;) {
        work_lock(w);
        if (!ok && !w->failed) {
            w->failed = true;
            snprintf(w->error, sizeof w->error, "%s", error);
        }
        unsigned i = w->failed ? w->chunks : w->next++;
        work_unlock(w);
        if (i >= w->chunks) break;
        ok = chunk(ir, w, i, error, sizeof error);
        work_lock(w);
        if (ok) printf("Compiled %u/%u chunks of blocks\n", ++w->finished, w->chunks);
        work_unlock(w);
    }
    snx_ir_free(ir);
}
#ifdef _WIN32
static DWORD WINAPI worker_main(LPVOID w) {
    worker(w);
    return 0;
}
#else
static void* worker_main(void* w) {
    worker(w);
    return NULL;
}
#endif
/* on the threads the setting aot_threads gives, as the Switch's recompile: N, or for 0 the machine's own count */
static bool compile_chunks(Work* w) {
    unsigned jobs = g_settings.aot_threads > 0 ? (unsigned)g_settings.aot_threads : snx_build_threads(), created = 0;
    if (jobs > w->chunks) jobs = w->chunks;
    if (jobs < 1) jobs = 1;
    printf("Compiling %u chunks of blocks on %u threads\n", w->chunks, jobs);
#ifdef _WIN32
    HANDLE* threads = calloc(jobs, sizeof *threads);
    InitializeCriticalSection(&w->lock);
#else
    pthread_t* threads = calloc(jobs, sizeof *threads);
    pthread_mutex_init(&w->lock, NULL);
#endif
    for (unsigned i = 1; threads && i < jobs; ++i) {
#ifdef _WIN32
        if (!(threads[created] = snx_compile_thread(worker_main, w))) break;
#else
        if (snx_compile_thread(&threads[created], worker_main, w)) break;
#endif
        ++created;
    }
    worker(w);
    for (unsigned i = 0; i < created; ++i) {
#ifdef _WIN32
        WaitForSingleObject(threads[i], INFINITE);
        CloseHandle(threads[i]);
#else
        pthread_join(threads[i], NULL);
#endif
    }
#ifdef _WIN32
    DeleteCriticalSection(&w->lock);
#else
    pthread_mutex_destroy(&w->lock);
#endif
    free(threads);
    return !w->failed;
}
/* The index: the sorted keys, each one's block, and the descriptor (VitaModule) the runtime looks up */
static bool index_object(SnxIr* ir, const VitaImage* image, const Block* blocks, size_t count, SnxLinkInput* output,
                         char* error, size_t size) {
    LLVMContextRef c = ir->ctx;
    LLVMModuleRef mod = new_module(ir);
    LLVMTargetDataRef layout = LLVMGetModuleDataLayout(mod);
    LLVMTypeRef i8 = LLVMInt8TypeInContext(c), i32 = LLVMInt32TypeInContext(c), i64 = LLVMInt64TypeInContext(c);
    LLVMTypeRef ptr = LLVMPointerTypeInContext(c, 0), ft = LLVMFunctionType(LLVMVoidTypeInContext(c), &ptr, 1, 0);
    size_t n = count ? count : 1;
    LLVMValueRef *keys = malloc(n * sizeof *keys), *functions = malloc(n * sizeof *functions);
    bool ok = keys && functions;
    for (size_t i = 0; ok && i < n; ++i) {
        char name[24];
        keys[i] = LLVMConstInt(i32, i < count ? blocks[i].key : 0, 0);
        if (i < count) {
            block_name(name, blocks[i].key);
            functions[i] = LLVMAddFunction(mod, name, ft);
            LLVMSetVisibility(functions[i], LLVMHiddenVisibility);
        } else
            functions[i] = LLVMConstNull(ptr);
    }
    if (ok) {
        LLVMValueRef tables[2];
        LLVMTypeRef types[2] = {LLVMArrayType(i32, (unsigned)n), LLVMArrayType(ptr, (unsigned)n)};
        LLVMValueRef values[2] = {LLVMConstArray(i32, keys, (unsigned)n), LLVMConstArray(ptr, functions, (unsigned)n)};
        for (unsigned t = 0; t < 2; ++t) {
            tables[t] = LLVMAddGlobal(mod, types[t], t ? "vita_blocks" : "vita_keys");
            LLVMSetInitializer(tables[t], values[t]);
            LLVMSetGlobalConstant(tables[t], 1);
            LLVMSetLinkage(tables[t], LLVMInternalLinkage);
        }
        LLVMValueRef hash[32], fields[10];
        LLVMTypeRef field_types[10] = {i64, i32, i32, i32, i32, i32, i32, LLVMArrayType(i8, 32), ptr, ptr};
        for (unsigned i = 0; i < 32; ++i) hash[i] = LLVMConstInt(i8, image->identity[i], 0);
        uint32_t integers[6] = {VITA_MODULE_ABI, sizeof(VitaModule), sizeof(SnxArmCPU), SNX_ARM_ABI, sizeof(void*),
                                (uint32_t)count};
        fields[0] = LLVMConstInt(i64, VITA_MODULE_MAGIC, 0);
        for (unsigned i = 0; i < 6; ++i) fields[i + 1] = LLVMConstInt(i32, integers[i], 0);
        fields[7] = LLVMConstArray(i8, hash, 32);
        fields[8] = tables[0], fields[9] = tables[1];
        LLVMTypeRef structure = LLVMStructTypeInContext(c, field_types, 10, 0);
        static const size_t offsets[10] = {offsetof(VitaModule, magic),       offsetof(VitaModule, abi),
                                           offsetof(VitaModule, descriptor_size), offsetof(VitaModule, cpu_size),
                                           offsetof(VitaModule, arm_abi),     offsetof(VitaModule, pointer_size),
                                           offsetof(VitaModule, count),       offsetof(VitaModule, identity),
                                           offsetof(VitaModule, keys),        offsetof(VitaModule, blocks)};
        ok = LLVMABISizeOfType(layout, structure) == sizeof(VitaModule);
        for (unsigned i = 0; ok && i < 10; ++i) ok = LLVMOffsetOfElement(layout, structure, i) == offsets[i];
        if (!ok)
            snprintf(error, size, "the module descriptor's layout differs between the compiler and the LLVM target");
        else {
            LLVMValueRef global = LLVMAddGlobal(mod, structure, "snx_vita_module_v1");
            LLVMSetInitializer(global, LLVMConstNamedStruct(structure, fields, 10));
            LLVMSetGlobalConstant(global, 1);
            unsigned char* object = NULL;
            ok = !snx_ir_module_object(ir, mod, '0', &object, &output->size, error, size);
            output->data = object;
        }
    } else
        snprintf(error, size, "out of memory for the module's index");
    free(keys);
    free(functions);
    LLVMDisposeModule(mod);
    return ok;
}
/* The module: the blocks' chunks, the index and the helpers they call (armlib.c), linked by the backend's own linker
   into the shared library the runtime loads (load.c), which exports the descriptor alone */
int vita_aot_compile(const char* folder, const VitaImage* image, char* error, size_t error_size) {
    Discovery d = {.image = image};
    Work work = {0};
    SnxIr* ir = NULL;
    SnxLinkInput* objects = NULL;
    SnxLinkOutput linked = {NULL, 0};
    unsigned char* armlib = NULL;
    size_t armlib_size = 0;
    char *target = NULL, *staged = NULL;
    Stub* stubs = NULL;
    const char* exports[1] = {"snx_vita_module_v1"};
    unsigned files = 0;
    int result = 1;
    if (error_size) error[0] = 0;
    if (snx_ir_init(error, error_size)) goto done;
    if (!discover(&d, folder, error, error_size)) goto done;
    qsort(d.blocks, d.count, sizeof *d.blocks, by_key);
    if (!(ir = snx_ir_new("vita-aot", '1', error, error_size))) goto done;
    target = vita_game_file(folder, VITA_MODULE_SUFFIX);
    staged = target ? snx_format("%s.tmp", target) : NULL;
    unsigned chunks = (unsigned)((d.count + CHUNK_BLOCKS - 1) / CHUNK_BLOCKS);
    files = chunks + 2; /* the helpers, the index, the chunks */
    objects = calloc(files, sizeof *objects);
    if (!objects || !staged) {
        snprintf(error, error_size, "out of memory");
        goto done;
    }
    objects[0].name = "armlib", objects[1].name = "index";
    for (unsigned i = 2; i < files; ++i) objects[i].name = "blocks";
    stubs = stubs_make(image, &work.stub_count);
    if (!stubs && image->import_count) {
        snprintf(error, error_size, "out of memory");
        goto done;
    }
    work = (Work){.image = image, .blocks = d.blocks, .objects = objects + 2, .count = d.count, .chunks = chunks,
                  .stubs = stubs, .stub_count = work.stub_count};
    if (chunks && !compile_chunks(&work)) {
        snprintf(error, error_size, "%s", work.error);
        goto done;
    }
    if (!index_object(ir, image, d.blocks, d.count, &objects[1], error, error_size)) goto done;
    if (snx_ir_armlib_object('1', &armlib, &armlib_size, error, error_size)) goto done;
    objects[0].data = armlib, objects[0].size = armlib_size;
    if (snx_link(objects, files, exports, 1, snx_path_leaf(target), &linked, error, error_size)) goto done;
    if (snx_write_file(staged, linked.data, linked.size) || plat_replace_file(staged, target)) {
        char why[256];
        snprintf(error, error_size, "cannot write %s: %s", target, snx_os_error(why, sizeof why));
        (void)remove(staged);
        goto done;
    }
    printf("Compiled %zu blocks into %s\n", d.count, target);
    result = 0;
done:
    if (objects)
        for (unsigned i = 1; i < files; ++i) free((void*)objects[i].data);
    free(objects);
    free(armlib);
    free(stubs);
    free(linked.data);
    free(staged);
    free(target);
    free(d.blocks);
    free(d.set);
    free(d.work);
    snx_ir_free(ir);
    return result;
}
