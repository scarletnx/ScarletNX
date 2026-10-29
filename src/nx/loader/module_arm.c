/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game module of a 32-bit program, ahead of time. The AArch32 block compiler (cpu/arm/llvm.h) makes one host
   function per block, from its entry to the next control transfer, and the runtime runs them one after another
   (nx/runtime/arm.c). The blocks are found by following the code from what is known to be code: each module's entry,
   its function symbols, the code pointers its relocations make, and the addresses the runtime recorded in the hints
   file because it had to compile them while the game ran (nx/runtime/arm.c). From each block, the blocks its last
   instruction leads to: a direct branch's target, and the next instruction after a call or a conditional transfer.
   An indirect branch's targets are known only when the game runs, unless something above names them */
#include "nx/loader/module_ir.h"
#include "nx/loader/module_abi.h"
#include "nx/loader/unit_cache.h"
#include "common/cpu/arm/llvm.h"
#include "common/cpu/recompiler/llvm/link.h"
#include "snx_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

/* the most code one block may run through: the block compiler's limit, in instructions (A32) or halfwords (Thumb) */
#define BLOCK_LIMIT 65536
/* the most blocks one block goes on to: a32_next and thumb_next queue its branch's target and what follows it */
#define SUCCESSORS  2
/* the module's i64 the runtime writes (SnxArmModule.poll_offset), which chained blocks read */
#define POLL_OFFSET "snx_arm_poll_offset"

typedef struct {
    SnxU32 key; /* the address, with bit 0 set for Thumb */
    SnxU32 units;
} Block;

typedef struct {
    const SnxProgram* program;
    Block* blocks;
    size_t count, capacity;
    SnxU32* set; /* every key ever queued: open addressing, 0 is empty (no code is at address 0) */
    size_t set_capacity, set_count;
    SnxU32* work;
    size_t work_count, work_capacity;
    int collect; /* queue only lists the keys in work, for one block's successors (block_next) */
} Discovery;

/* the code from address to the end of its module's text, and how many bytes of it */
static const unsigned char* code_at(const SnxProgram* p, SnxU32 address, size_t* bytes) {
    size_t i;
    for (i = 0; i < p->module_count; ++i) {
        const SnxModuleImage* m = &p->modules[i];
        SnxU64 lo = m->base + m->text_off, hi = lo + m->text_size;
        if (address >= lo && address < hi) {
            *bytes = (size_t)(hi - address);
            return m->image + m->text_off + (address - lo);
        }
    }
    *bytes = 0;
    return NULL;
}

static int set_insert(Discovery* d, SnxU32 key) {
    size_t i;
    if ((d->set_count + 1) * 2 > d->set_capacity) {
        size_t capacity = d->set_capacity ? d->set_capacity * 2 : 4096, j;
        SnxU32* set = (SnxU32*)snx_alloc(capacity * sizeof(*set));
        memset(set, 0, capacity * sizeof(*set));
        for (j = 0; j < d->set_capacity; ++j)
            if (d->set[j]) {
                i = (size_t)(d->set[j] * 0x9E3779B1u) & (capacity - 1);
                while (set[i]) i = (i + 1) & (capacity - 1);
                set[i] = d->set[j];
            }
        free(d->set);
        d->set = set;
        d->set_capacity = capacity;
    }
    i = (size_t)(key * 0x9E3779B1u) & (d->set_capacity - 1);
    while (d->set[i]) {
        if (d->set[i] == key) return 0;
        i = (i + 1) & (d->set_capacity - 1);
    }
    d->set[i] = key;
    ++d->set_count;
    return 1;
}

/* queues the block at address, in Thumb state or not, when it is code this program has and wasn't queued yet. A32
   code is word aligned, Thumb code halfword aligned */
static void queue(Discovery* d, SnxU32 address, int thumb) {
    size_t bytes;
    SnxU32 key = address | (thumb ? 1u : 0u);
    if (address & (thumb ? 1u : 3u)) return;
    if (!code_at(d->program, address, &bytes) || bytes < (thumb ? 2u : 4u)) return;
    if (d->collect) {
        if (d->work_count < SUCCESSORS) d->work[d->work_count++] = key;
        return;
    }
    if (!set_insert(d, key)) return;
    if (d->work_count == d->work_capacity) {
        d->work_capacity = d->work_capacity ? d->work_capacity * 2 : 1024;
        d->work = (SnxU32*)snx_realloc(d->work, d->work_capacity * sizeof(*d->work));
    }
    d->work[d->work_count++] = key;
}
/* a code pointer: interworking's, Thumb when bit 0 is set */
static void queue_pointer(Discovery* d, SnxU64 pointer) {
    if (pointer >> 32) return;
    queue(d, (SnxU32)pointer & ~1u, (int)(pointer & 1));
}

static SnxU32 sign_extend(SnxU32 value, unsigned bits) {
    SnxU32 sign = 1u << (bits - 1);
    return (value ^ sign) - sign;
}

/* whether control never reaches the instruction after the A32 instruction w when it runs: a branch without link, a
   write to the PC, BKPT. A call returns there, as does an instruction the block compiler leaves to the backend
   (SnxArmRt.undefined), which may run it. Anything else ends a block only at the code's end */
static int a32_ends(SnxU32 w) {
    unsigned rd = (w >> 12) & 15;
    switch (snx_a32_decode(w)) {
        case SNX_A32_BRANCH: return !(w & (1u << 24)); /* B; BL returns */
        case SNX_A32_BX: return !(w & 0x20);           /* BX; BLX returns */
        case SNX_A32_DATA: return rd == 15;
        case SNX_A32_SINGLE:
        case SNX_A32_EXTRA: return rd == 15 && (w & (1u << 20));
        case SNX_A32_MULTIPLE: return (w & (1u << 15)) && (w & (1u << 20));
        case SNX_A32_BKPT: return 1;
        default: return 0;
    }
}

/* What follows an A32 block whose last instruction is w at pc: the target of a direct branch, and the next
   instruction unless control can't get there (a32_ends, when the instruction isn't conditional). Leaving a successor
   out only sends the game through the runtime's block compiler there; one too many costs a block of code that never
   runs */
static void a32_next(Discovery* d, SnxU32 pc, SnxU32 w) {
    SnxA32Kind kind = snx_a32_decode(w);
    unsigned cond = w >> 28;
    if (kind == SNX_A32_BRANCH) queue(d, pc + 8 + (sign_extend(w & 0xFFFFFF, 24) << 2), 0);
    if (kind == SNX_A32_BLX_IMM) queue(d, pc + 8 + (sign_extend(w & 0xFFFFFF, 24) << 2) + ((w >> 23) & 2), 1);
    if (!(a32_ends(w) && (cond == 14 || cond == 15))) queue(d, pc + 4, 0);
}

/* The same for a Thumb block of units halfwords at pc, compiled from count halfwords: its instructions are walked as
   the block compiler walks them, IT state included, to its last one */
static void thumb_next(Discovery* d, SnxU32 pc, const unsigned char* code, size_t units, size_t count) {
    size_t used = 0, len = 1;
    unsigned it_len = 0, it_idx = 0, it_conds[4], cond = 14, i;
    SnxThumb t;
    memset(&t, 0, sizeof(t));
    while (used < units) {
        unsigned hw1 = code[used * 2] | (unsigned)code[used * 2 + 1] << 8;
        unsigned hw2 = used + 1 < count ? code[used * 2 + 2] | (unsigned)code[used * 2 + 3] << 8 : 0;
        len = snx_thumb_decode((uint16_t)hw1, (uint16_t)hw2, &t);
        if (!len || (len == TH_F32 && used + 1 >= count)) { /* as the block compiler steps over it */
            t.shape = TH_UNDEF;
            len = (hw1 >> 11) >= 0x1D && used + 1 < count ? 2 : 1;
        }
        cond = it_idx < it_len ? it_conds[it_idx++] : 14;
        if (used + len >= units) break;
        used += len;
        if (t.shape == TH_IT) { /* as llvm_thumb.inc reads it: 4 - ctz(mask) instructions */
            unsigned mask = t.imm & 15, first = (t.imm >> 4) & 15;
            for (it_len = 4; !(mask & 1); mask >>= 1) --it_len;
            mask = t.imm & 15;
            it_idx = 0;
            for (i = 0; i < it_len; ++i) it_conds[i] = i == 0 ? first : ((first & 0xE) | ((mask >> (4 - i)) & 1));
        }
    }
    pc += (SnxU32)used * 2; /* the last instruction */
    {
        SnxU32 next = pc + (SnxU32)len * 2, base = pc + 4;
        int always = cond == 14, ends = 0;
        switch (t.shape) {
            case TH_BRANCH: {
                unsigned hw1 = t.rn, hw2 = t.rm, s = (hw1 >> 10) & 1;
                unsigned i1 = !(((hw2 >> 13) & 1) ^ s), i2 = !(((hw2 >> 11) & 1) ^ s);
                switch (t.op) {
                    case BR_B:
                        queue(d, base + (sign_extend(t.imm, 11) << 1), 1);
                        ends = 1;
                        break;
                    case BR_BCOND:
                        queue(d, base + (sign_extend(t.imm, 8) << 1), 1);
                        always = 0;
                        break;
                    case BR_CBZ:
                    case BR_CBNZ:
                        queue(d, base + t.imm, 1);
                        always = 0;
                        break;
                    case BR_BL:
                    case BR_BW:
                        queue(d,
                              base +
                                  (sign_extend(
                                       (s << 23) | (i1 << 22) | (i2 << 21) | ((hw1 & 0x3FF) << 11) | (hw2 & 0x7FF), 24)
                                   << 1),
                              1);
                        ends = t.op == BR_BW;
                        break;
                    case BR_BLX_IMM:
                        queue(d,
                              (base & ~3u) + (sign_extend((s << 23) | (i1 << 22) | (i2 << 21) | ((hw1 & 0x3FF) << 11) |
                                                              (((hw2 >> 1) & 0x3FF) << 1),
                                                          24)
                                              << 1),
                              0);
                        break;
                    case BR_BCOND_W:
                        queue(d,
                              base + (sign_extend((s << 19) | (((hw2 >> 11) & 1) << 18) | (((hw2 >> 13) & 1) << 17) |
                                                      ((hw1 & 0x3F) << 11) | (hw2 & 0x7FF),
                                                  20)
                                      << 1),
                              1);
                        always = 0;
                        break;
                    case BR_BX: ends = 1; break;
                    case BR_BKPT: ends = 1; break;
                    default: break; /* BLX register and SVC return */
                }
                break;
            }
            case TH_TABLE: ends = 1; break;
            case TH_HI: ends = t.rd == 15 && t.op != 1; break; /* MOV or ADD to the PC; not CMP */
            case TH_LOAD: ends = t.rd == 15; break;
            case TH_PUSHPOP: ends = t.op == 1 && t.ra; break; /* POP with the PC */
            case TH_A32: ends = a32_ends(t.imm); break;
            default: break;
        }
        if (!(ends && always)) queue(d, next, 1);
    }
}

static SnxU32 read32(const unsigned char* p) {
    return (SnxU32)p[0] | (SnxU32)p[1] << 8 | (SnxU32)p[2] << 16 | (SnxU32)p[3] << 24;
}

/* the halfwords or words a block compiler reads: the guest's little-endian code as host integers */
static void* units_of(const unsigned char* code, size_t count, int thumb) {
    size_t i;
    if (thumb) {
        uint16_t* h = (uint16_t*)snx_alloc(count * sizeof(*h));
        for (i = 0; i < count; ++i) h[i] = (uint16_t)(code[i * 2] | code[i * 2 + 1] << 8);
        return h;
    } else {
        uint32_t* w = (uint32_t*)snx_alloc(count * sizeof(*w));
        for (i = 0; i < count; ++i) w[i] = read32(code + i * 4);
        return w;
    }
}

/* compiles the block at key into module as name, and *units how much code it covers. NULL when the block compiler
   refuses the input (it doesn't for code it can read: it compiles what it can't run to an exit) */
static LLVMValueRef compile_block(LLVMModuleRef module, const SnxProgram* p, SnxU32 key, const char* name,
                                  size_t* units, size_t* count) {
    size_t bytes, consumed = 0;
    int thumb = (int)(key & 1);
    SnxU32 address = key & ~1u;
    const unsigned char* code = code_at(p, address, &bytes);
    void* input;
    LLVMValueRef fn;
    *count = bytes / (thumb ? 2 : 4);
    if (*count > BLOCK_LIMIT) *count = BLOCK_LIMIT;
    input = units_of(code, *count, thumb);
    fn = thumb ? snx_thumb_llvm_block(module, name, (const uint16_t*)input, *count, address, &consumed)
               : snx_arm_llvm_block(module, name, (const uint32_t*)input, *count, address, &consumed);
    free(input);
    *units = consumed;
    return fn;
}

static void block_name(char* out, SnxU32 key) {
    sprintf(out, "%c_%08lx", key & 1 ? 'T' : 'A', (unsigned long)(key & ~1u));
}

/* Every block reachable from the known code. Each is compiled once into a scratch module to learn where it ends (the
   block compiler decides that), and thrown away: the units compile them again, in parallel */
static int discover(Discovery* d, const SnxCodegenOptions* options, char* error, size_t error_size) {
    const SnxProgram* p = d->program;
    SnxIr* scratch;
    size_t i, j;
    scratch = snx_ir_new("discovery", '0', error, error_size);
    if (!scratch) return -1;
    for (i = 0; i < p->module_count; ++i) {
        const SnxModuleImage* m = &p->modules[i];
        queue(d, (SnxU32)(m->base + m->text_off), 0); /* the module's start: Loader enters rtld's in A32 state */
        for (j = 0; j < m->function_count; ++j) queue_pointer(d, m->base + m->functions[j]);
        for (j = 0; j < m->relptr_count; ++j) queue_pointer(d, m->base + m->relptrs[j]);
    }
    for (i = 0; i < options->hint_count; ++i) queue_pointer(d, options->hints[i]);
    while (d->work_count) {
        SnxU32 key = d->work[--d->work_count], address = key & ~1u;
        size_t units, count, bytes;
        const unsigned char* code = code_at(p, address, &bytes);
        LLVMValueRef fn = compile_block(scratch->mod, p, key, "block", &units, &count);
        if (!fn || !units) {
            char name[16];
            block_name(name, key);
            snprintf(error, error_size, "the AArch32 block compiler refused the block %s", name);
            snx_ir_free(scratch);
            return -1;
        }
        LLVMDeleteFunction(fn);
        if (d->count == d->capacity) {
            d->capacity = d->capacity ? d->capacity * 2 : 4096;
            d->blocks = (Block*)snx_realloc(d->blocks, d->capacity * sizeof(*d->blocks));
        }
        d->blocks[d->count].key = key;
        d->blocks[d->count++].units = (SnxU32)units;
        if (key & 1)
            thumb_next(d, address, code, units, count);
        else
            a32_next(d, address + (SnxU32)(units - 1) * 4, read32(code + (units - 1) * 4));
    }
    snx_ir_free(scratch);
    return 0;
}

/* the keys of what follows the block at key, covering units of code, as discover queues them, into next */
static size_t block_next(const SnxProgram* p, SnxU32 key, size_t units, size_t count, SnxU32* next) {
    Discovery s = {0};
    size_t bytes;
    SnxU32 address = key & ~1u;
    const unsigned char* code = code_at(p, address, &bytes);
    s.program = p;
    s.collect = 1;
    s.work = next;
    if (key & 1)
        thumb_next(&s, address, code, units, count);
    else
        a32_next(&s, address + (SnxU32)(units - 1) * 4, read32(code + (units - 1) * 4));
    return s.work_count;
}

static int compare_block(const void* a, const void* b) {
    SnxU32 x = ((const Block*)a)->key, y = ((const Block*)b)->key;
    return x < y ? -1 : x > y;
}

/* ---- the units: blocks [first, last), compiled in parallel as module_llvm.c compiles its chunks ---- */

typedef struct {
    size_t first, last;
    unsigned char* object;
    size_t size;
    int reused; /* the object came from the unit cache */
} Unit;

typedef struct {
    const SnxProgram* program;
    const Block* blocks; /* sorted by key */
    size_t nblocks;
    const SnxLlvmBuild* build;
    SnxUnitCache* cache; /* the units' objects kept between recompiles (NULL: none), an interrupted one's too */
    Unit* units;
    size_t count, next, done;
    int failed;
    char error[1024];
#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
} Queue;

static void lock(Queue* q) {
#ifdef _WIN32
    EnterCriticalSection(&q->lock);
#else
    pthread_mutex_lock(&q->lock);
#endif
}
static void unlock(Queue* q) {
#ifdef _WIN32
    LeaveCriticalSection(&q->lock);
#else
    pthread_mutex_unlock(&q->lock);
#endif
}

static int save_ir(SnxIr* ir, const char* dir, const char* name) {
    char *text = snx_ir_text(ir), *file = snx_format("%s.ll", name), *path = snx_path_join(dir, file);
    int rc = snx_write_file(path, text, strlen(text));
    LLVMDisposeMessage(text);
    free(file);
    free(path);
    return rc;
}

/* Block chaining: a block that ends where a block of the module starts goes on to it by a tail call, instead of
   returning to the runtime's loop (arm_thread_run), which looks the next block up after every one. Each of the
   block's returns becomes: return when it set an exit; else, for each successor discover found (block_next), when
   r15 and CPSR.T are its address and state, a musttail call of it; else return. A successor at or before the
   block's start (every loop has one) first reads the thread's poll, as the loop does after each block: when a thread
   of higher priority waits for the core, the block returns and the loop yields it */
static int chain_has(const Queue* q, SnxU32 key) {
    size_t lo = 0, hi = q->nblocks;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (q->blocks[mid].key == key) return 1;
        if (q->blocks[mid].key < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}
static void chain_block(SnxIr* ir, LLVMValueRef fn, SnxU32 key, const SnxU32* next, size_t count) {
    LLVMTypeRef block_type = LLVMFunctionType(ir->t_void, &ir->t_ptr, 1, 0);
    LLVMValueRef *rets, cpu = LLVMGetParam(fn, 0), poll_offset = LLVMGetNamedGlobal(ir->mod, POLL_OFFSET);
    size_t nrets = 0, i, k;
    rets = (LLVMValueRef*)snx_alloc(LLVMCountBasicBlocks(fn) * sizeof(*rets));
    for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn); bb; bb = LLVMGetNextBasicBlock(bb)) {
        LLVMValueRef t = LLVMGetBasicBlockTerminator(bb);
        if (t && LLVMGetInstructionOpcode(t) == LLVMRet) rets[nrets++] = t;
    }
    if (!poll_offset) {
        poll_offset = LLVMAddGlobal(ir->mod, ir->t_i64, POLL_OFFSET);
        LLVMSetVisibility(poll_offset, LLVMHiddenVisibility); /* the module's own: reached PC-relative */
    }
    /* one tail for all of them: a block's returns are its exits too (a fault, an SVC), which the tail's first check
       takes back out */
    LLVMBasicBlockRef tail = LLVMAppendBasicBlockInContext(ir->ctx, fn, "chain"),
                      go = LLVMAppendBasicBlockInContext(ir->ctx, fn, "next"),
                      out = LLVMAppendBasicBlockInContext(ir->ctx, fn, "out");
    LLVMValueRef c, pc, t;
    for (i = 0; i < nrets; ++i) {
        LLVMPositionBuilderBefore(ir->b, rets[i]);
        LLVMBuildBr(ir->b, tail);
        LLVMInstructionEraseFromParent(rets[i]);
    }
    LLVMPositionBuilderAtEnd(ir->b, out);
    LLVMBuildRetVoid(ir->b);
    LLVMPositionBuilderAtEnd(ir->b, tail);
    c = LLVMBuildGEP2(ir->b, ir->t_i8, cpu, (LLVMValueRef[]){ir_i64(ir, offsetof(SnxArmCPU, exit))}, 1, "");
    c = LLVMBuildICmp(ir->b, LLVMIntEQ, LLVMBuildLoad2(ir->b, ir->t_i32, c, ""), ir_i32(ir, SNX_ARM_OK), "");
    LLVMBuildCondBr(ir->b, c, go, out);
    LLVMPositionBuilderAtEnd(ir->b, go);
    pc = LLVMBuildGEP2(ir->b, ir->t_i8, cpu, (LLVMValueRef[]){ir_i64(ir, offsetof(SnxArmCPU, r) + 15 * 4)}, 1, "");
    pc = LLVMBuildLoad2(ir->b, ir->t_i32, pc, "");
    t = LLVMBuildGEP2(ir->b, ir->t_i8, cpu, (LLVMValueRef[]){ir_i64(ir, offsetof(SnxArmCPU, cpsr))}, 1, "");
    t = LLVMBuildAnd(ir->b, LLVMBuildLoad2(ir->b, ir->t_i32, t, ""), ir_i32(ir, SNX_ARM_T), "");
    for (k = 0; k < count; ++k) {
        LLVMBasicBlockRef call = LLVMAppendBasicBlockInContext(ir->ctx, fn, "call"),
                          miss = LLVMAppendBasicBlockInContext(ir->ctx, fn, "other");
        LLVMValueRef target, site;
        char name[16];
        c = LLVMBuildAnd(ir->b, LLVMBuildICmp(ir->b, LLVMIntEQ, pc, ir_i32(ir, next[k] & ~1u), ""),
                         LLVMBuildICmp(ir->b, LLVMIntEQ, t, ir_i32(ir, next[k] & 1 ? SNX_ARM_T : 0), ""), "");
        LLVMBuildCondBr(ir->b, c, call, miss);
        LLVMPositionBuilderAtEnd(ir->b, call);
        if ((next[k] & ~1u) <= (key & ~1u)) {
            LLVMBasicBlockRef run = LLVMAppendBasicBlockInContext(ir->ctx, fn, "run");
            LLVMValueRef off = LLVMBuildLoad2(ir->b, ir->t_i64, poll_offset, "");
            LLVMValueRef poll = LLVMBuildLoad2(ir->b, ir->t_i32, LLVMBuildGEP2(ir->b, ir->t_i8, cpu, &off, 1, ""), "");
            LLVMSetOrdering(poll, LLVMAtomicOrderingMonotonic); /* the loop's relaxed load */
            LLVMSetAlignment(poll, 4);
            LLVMBuildCondBr(ir->b, LLVMBuildICmp(ir->b, LLVMIntEQ, poll, ir_i32(ir, 0), ""), run, out);
            LLVMPositionBuilderAtEnd(ir->b, run);
        }
        block_name(name, next[k]);
        target = LLVMGetNamedFunction(ir->mod, name);
        if (!target) target = LLVMAddFunction(ir->mod, name, block_type);
        site = LLVMBuildCall2(ir->b, block_type, target, &cpu, 1, "");
        LLVMSetTailCallKind(site, LLVMTailCallKindMustTail);
        LLVMBuildRetVoid(ir->b);
        LLVMPositionBuilderAtEnd(ir->b, miss);
    }
    LLVMBuildBr(ir->b, out);
    free(rets);
}

static int compile_unit(Queue* q, size_t index, char* error, size_t error_size) {
    Unit* u = &q->units[index];
    char name[32], block[16];
    size_t i, units;
    SnxIr* ir;
    int rc;
    sprintf(name, "code_%03lu", (unsigned long)index);
    ir = snx_ir_new(name, q->build->opt, error, error_size);
    if (!ir) return -1;
    snx_ir_tune(ir, q->build->tuning);
    /* every block of the unit first: the block compiler refuses a name the module has, and chain_block declares the
       successors it calls */
    LLVMValueRef* fns = (LLVMValueRef*)snx_alloc((u->last - u->first) * sizeof(*fns));
    size_t* counts = (size_t*)snx_alloc((u->last - u->first) * sizeof(*counts));
    for (i = u->first; i < u->last; ++i) {
        block_name(block, q->blocks[i].key);
        fns[i - u->first] = compile_block(ir->mod, q->program, q->blocks[i].key, block, &units, &counts[i - u->first]);
        if (!fns[i - u->first] || units != q->blocks[i].units) {
            snprintf(error, error_size, "the AArch32 block %s compiled differently the second time", block);
            break;
        }
    }
    for (rc = i < u->last ? -1 : 0, i = u->first; !rc && i < u->last; ++i) {
        SnxU32 next[SUCCESSORS];
        size_t n = block_next(q->program, q->blocks[i].key, q->blocks[i].units, counts[i - u->first], next), k;
        for (k = 0; k < n && chain_has(q, next[k]); ++k) {}
        if (k < n) {
            char to[16];
            block_name(block, q->blocks[i].key);
            block_name(to, next[k]);
            snprintf(error, error_size, "the AArch32 block %s goes on to %s, a block discovery did not find", block,
                     to);
            rc = -1;
        } else
            chain_block(ir, fns[i - u->first], q->blocks[i].key, next, n);
    }
    free(fns);
    free(counts);
    if (rc) {
        snx_ir_free(ir);
        return -1;
    }
    rc = q->build->keep_ir && save_ir(ir, q->build->keep_ir, name);
    if (rc) snprintf(error, error_size, "cannot save the IR of %s", name);
    if (!rc && q->cache)
        rc = snx_unit_cache_object(q->cache, ir, &u->object, &u->size, &u->reused, NULL, error, error_size);
    else if (!rc)
        rc = snx_ir_object(ir, q->build->opt, &u->object, &u->size, error, error_size);
    snx_ir_free(ir);
    return rc;
}

static void worker(Queue* q) {
    char error[1024], name[32], *label;
    double start, seconds;
    for (;;) {
        size_t i;
        int rc;
        lock(q);
        if (q->failed || q->next == q->count) {
            unlock(q);
            return;
        }
        i = q->next++;
        unlock(q);
        error[0] = 0;
        start = snx_seconds();
        rc = compile_unit(q, i, error, sizeof(error));
        seconds = snx_seconds() - start;
        label = rc || q->build->quiet
                    ? NULL
                    : snx_format("%08lx-%08lx, %lu blocks", (unsigned long)(q->blocks[q->units[i].first].key & ~1u),
                                 (unsigned long)(q->blocks[q->units[i].last - 1].key & ~1u),
                                 (unsigned long)(q->units[i].last - q->units[i].first));
        lock(q);
        ++q->done;
        if (rc && !q->failed) {
            q->failed = 1;
            snprintf(q->error, sizeof(q->error), "%s", error);
        }
        if (label) {
            sprintf(name, "code_%03lu", (unsigned long)i);
            snx_report_unit(q->done, q->count + 1, name, label, q->units[i].reused, seconds); /* + the descriptor */
        }
        unlock(q);
        free(label);
    }
}
#ifdef _WIN32
static DWORD WINAPI thread_main(LPVOID p) {
    worker((Queue*)p);
    return 0;
}
#else
static void* thread_main(void* p) {
    worker((Queue*)p);
    return NULL;
}
#endif

static void run(Queue* q, unsigned jobs) {
    unsigned i, created = 0;
#ifdef _WIN32
    HANDLE* threads;
    InitializeCriticalSection(&q->lock);
#else
    pthread_t* threads;
    pthread_mutex_init(&q->lock, NULL);
#endif
    if (jobs > q->count) jobs = (unsigned)q->count;
    if (jobs < 1) jobs = 1;
    threads = snx_alloc(jobs * sizeof(*threads));
    for (i = 1; i < jobs; ++i) {
#ifdef _WIN32
        HANDLE h = CreateThread(NULL, 0, thread_main, q, 0, NULL);
        if (!h) break;
        threads[created++] = h;
#else
        if (pthread_create(&threads[created], NULL, thread_main, q)) break;
        ++created;
#endif
    }
    worker(q);
    for (i = 0; i < created; ++i) {
#ifdef _WIN32
        WaitForSingleObject(threads[i], INFINITE);
        CloseHandle(threads[i]);
#else
        pthread_join(threads[i], NULL);
#endif
    }
#ifdef _WIN32
    DeleteCriticalSection(&q->lock);
#else
    pthread_mutex_destroy(&q->lock);
#endif
    free(threads);
}

/* ---- the descriptor: SnxArmModule, a constant the exported snx_module_arm() returns ---- */

static LLVMValueRef constant(SnxIr* ir, LLVMTypeRef type, const char* name, LLVMValueRef value) {
    LLVMValueRef g = LLVMAddGlobal(ir->mod, type, name);
    LLVMSetLinkage(g, LLVMInternalLinkage);
    LLVMSetInitializer(g, value);
    LLVMSetGlobalConstant(g, 1);
    return g;
}

static int emit_descriptor(SnxIr* ir, const Block* blocks, size_t count, const SnxCodegenOptions* options,
                           const SnxProgram* p, char* error, size_t error_size) {
    static const size_t offsets[13] = {
        offsetof(SnxArmModule, abi),        offsetof(SnxArmModule, nmods),      offsetof(SnxArmModule, mods),
        offsetof(SnxArmModule, nentries),   offsetof(SnxArmModule, entry_addr), offsetof(SnxArmModule, entry_fn),
        offsetof(SnxArmModule, title),      offsetof(SnxArmModule, title_id),   offsetof(SnxArmModule, main_stack),
        offsetof(SnxArmModule, main_prio),  offsetof(SnxArmModule, main_core),  offsetof(SnxArmModule, aspace),
        offsetof(SnxArmModule, poll_offset)};
    LLVMTypeRef fields[13], type, info, block_type;
    LLVMValueRef *values, f[13], mod, fn, poll_offset;
    LLVMTargetDataRef layout;
    size_t n = count ? count : 1, i;
    char name[16];
    int ok;
    info = snx_module_info_type(ir, error, error_size);
    if (!info) return -1;
    fields[0] = fields[1] = fields[3] = ir->t_i32;
    fields[2] = fields[4] = fields[5] = fields[6] = ir->t_ptr;
    fields[7] = ir->t_i64;
    for (i = 8; i < 12; ++i) fields[i] = ir->t_i32;
    fields[12] = ir->t_ptr;
    type = LLVMStructTypeInContext(ir->ctx, fields, 13, 0);
    layout = LLVMGetModuleDataLayout(ir->mod);
    ok = LLVMABISizeOfType(layout, type) == sizeof(SnxArmModule);
    for (i = 0; i < 13; ++i) ok = ok && LLVMOffsetOfElement(layout, type, (unsigned)i) == offsets[i];
    if (!ok) {
        snprintf(error, error_size, "SnxArmModule layout differs between the recompiler and the LLVM target");
        return -1;
    }
    block_type = LLVMFunctionType(ir->t_void, &ir->t_ptr, 1, 0);
    values = (LLVMValueRef*)snx_alloc(n * sizeof(*values));
    for (i = 0; i < n; ++i) values[i] = ir_i32(ir, i < count ? blocks[i].key : 0);
    f[4] =
        constant(ir, LLVMArrayType(ir->t_i32, (unsigned)n), "ENT_ADDR", LLVMConstArray(ir->t_i32, values, (unsigned)n));
    for (i = 0; i < n; ++i) {
        if (i < count) {
            block_name(name, blocks[i].key);
            values[i] = LLVMAddFunction(ir->mod, name, block_type);
        } else
            values[i] = LLVMConstNull(ir->t_ptr);
    }
    f[5] =
        constant(ir, LLVMArrayType(ir->t_ptr, (unsigned)n), "ENT_FN", LLVMConstArray(ir->t_ptr, values, (unsigned)n));
    free(values);
    f[0] = ir_i32(ir, SNX_ARM_MODULE_ABI);
    f[1] = ir_i32(ir, (SnxU32)p->module_count);
    f[2] = snx_module_mods(ir, info, p);
    f[3] = ir_i32(ir, (SnxU32)count);
    f[6] = snx_module_string(ir, options->title ? options->title : "Game");
    f[7] = ir_i64(ir, p->npdm.title_id);
    f[8] = ir_i32(ir, (SnxU32)p->npdm.stack);
    f[9] = ir_i32(ir, p->npdm.prio);
    f[10] = ir_i32(ir, p->npdm.core);
    f[11] = ir_i32(ir, p->npdm.aspace);
    /* the blocks' poll offset (chain_block), which arm_module writes */
    poll_offset = LLVMAddGlobal(ir->mod, ir->t_i64, POLL_OFFSET);
    LLVMSetVisibility(poll_offset, LLVMHiddenVisibility);
    LLVMSetInitializer(poll_offset, ir_i64(ir, 0));
    f[12] = poll_offset;
    mod = constant(ir, type, "MOD", LLVMConstStructInContext(ir->ctx, f, 13, 0));
    /* SNX_EXPORT const SnxArmModule* snx_module_arm(void) */
    fn = LLVMAddFunction(ir->mod, "snx_module_arm", LLVMFunctionType(ir->t_ptr, NULL, 0, 0));
    if (ir->windows) LLVMSetDLLStorageClass(fn, LLVMDLLExportStorageClass);
    LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, fn, "entry"));
    LLVMBuildRet(ir->b, mod);
    return 0;
}

static const char* base_name(const char* path) {
    const char *name = path, *s;
    for (s = path; *s; ++s)
        if (*s == '/' || *s == '\\') name = s + 1;
    return name;
}

int snx_build_module_arm(SnxProgram* loaded, const SnxCodegenOptions* options, const SnxLlvmBuild* build) {
    Discovery d;
    Queue q;
    SnxIr* ir = NULL;
    SnxLinkInput* inputs = NULL;
    SnxLinkOutput linked;
    unsigned char *module_object = NULL, *armlib_object = NULL;
    size_t module_size = 0, armlib_size = 0, first, last, units, limit, i, n = 0;
    const char* exports[1];
    char error[1024], *tmp = NULL;
    double start;
    int rc = -1;
    memset(&d, 0, sizeof(d));
    memset(&q, 0, sizeof(q));
    memset(&linked, 0, sizeof(linked));
    if (!loaded->module_count) return snx_error("no modules to generate");
    if (snx_ir_init(error, sizeof(error))) return snx_error("%s", error);
    d.program = loaded;
    start = snx_seconds();
    if (discover(&d, options, error, sizeof(error))) {
        snx_error("AArch32: %s", error);
        goto done;
    }
    qsort(d.blocks, d.count, sizeof(*d.blocks), compare_block);
    if (!options->quiet) printf("AArch32: %lu blocks found (%.1f s)\n", (unsigned long)d.count, snx_seconds() - start);
    /* units of whole blocks, by the code they cover, as module_llvm.c's chunks of whole functions */
    limit = options->chunk_instrs ? options->chunk_instrs : (size_t)g_settings.unit_size;
    q.units = (Unit*)snx_alloc((d.count + 1) * sizeof(*q.units));
    memset(q.units, 0, (d.count + 1) * sizeof(*q.units));
    for (first = 0; first < d.count; first = last) {
        last = first;
        units = 0;
        do { units += d.blocks[last++].units; } while (last < d.count && units < limit);
        q.units[q.count].first = first;
        q.units[q.count++].last = last;
    }
    q.program = loaded;
    q.blocks = d.blocks;
    q.nblocks = d.count;
    q.build = build;
    /* the unit cache, as module_llvm.c's: a unit compiled before is taken from it, a recompile cut short's too */
    if (build->unit_cache) {
        char llvm[128];
        SnxUnitIdentity identity;
        snx_unit_cache_llvm(llvm, sizeof llvm);
        identity.llvm = llvm;
        identity.opt = build->opt;
        identity.tuning = build->tuning ? build->tuning->text : NULL;
        q.cache = snx_unit_cache_open(build->unit_cache, &identity, error, sizeof error);
        if (!q.cache) {
            snx_error("unit cache: %s", error);
            goto done;
        }
        if (build->unit_cache_disk) snx_unit_cache_disk(q.cache, build->unit_cache_disk);
    }
    if (!options->quiet)
        printf("LLVM: %lu code unit%s and the module descriptor\n", (unsigned long)q.count, q.count == 1 ? "" : "s");
    if (q.count) run(&q, build->jobs);
    if (q.failed) {
        snx_error("LLVM backend: %s", q.error);
        goto done;
    }
    start = snx_seconds();
    ir = snx_ir_new("module", build->opt, error, sizeof(error));
    if (!ir || emit_descriptor(ir, d.blocks, d.count, options, loaded, error, sizeof(error)) ||
        (build->keep_ir && save_ir(ir, build->keep_ir, "module")) ||
        snx_ir_object(ir, build->opt, &module_object, &module_size, error, sizeof(error))) {
        snx_error("LLVM backend: %s", error);
        goto done;
    }
    if (!options->quiet) {
        char* label = snx_format("SnxArmModule descriptor, %lu blocks", (unsigned long)d.count);
        snx_report_unit(q.count + 1, q.count + 1, "module", label, 0, snx_seconds() - start);
        free(label);
    }
    if (snx_ir_armlib_object(build->opt, &armlib_object, &armlib_size, error, sizeof(error))) {
        snx_error("LLVM backend: %s", error);
        goto done;
    }
    inputs = (SnxLinkInput*)snx_alloc((q.count + 2) * sizeof(*inputs));
    inputs[n].data = armlib_object;
    inputs[n].size = armlib_size;
    inputs[n++].name = "armlib";
    inputs[n].data = module_object;
    inputs[n].size = module_size;
    inputs[n++].name = "module";
    for (i = 0; i < q.count; ++i) {
        inputs[n].data = q.units[i].object;
        inputs[n].size = q.units[i].size;
        inputs[n++].name = "code unit";
    }
    exports[0] = "snx_module_arm";
    if (snx_link(inputs, n, exports, 1, base_name(build->output), &linked, error, sizeof(error))) {
        snx_error("linking failed: %s", error);
        goto done;
    }
    tmp = snx_format("%s.tmp", build->output);
    if (snx_write_file(tmp, linked.data, linked.size) || snx_replace(tmp, build->output)) {
        snx_error("linking failed: cannot write %s", build->output);
        if (snx_is_file(tmp)) snx_remove_tree(tmp);
        goto done;
    }
    if (!options->quiet)
        printf("LLVM module: %.1f MB, %lu AArch32 blocks\n", (double)linked.size / 1000000.0, (unsigned long)d.count);
    if (q.cache) {
        SnxU64 bytes = 0;
        size_t reused = 0;
        for (i = 0; i < q.count; ++i)
            if (q.units[i].reused) {
                ++reused;
                bytes += q.units[i].size;
            }
        snx_unit_cache_report(stdout, build->unit_cache, build->unit_cache_limit, reused, q.count, bytes,
                              options->quiet);
    }
    rc = 0;
done:
    for (i = 0; i < q.count; ++i) free(q.units[i].object);
    free(q.units);
    free(inputs);
    free(module_object);
    free(armlib_object);
    free(linked.data);
    free(tmp);
    free(d.blocks);
    free(d.set);
    free(d.work);
    snx_ir_free(ir);
    snx_unit_cache_close(q.cache);
    return rc;
}
