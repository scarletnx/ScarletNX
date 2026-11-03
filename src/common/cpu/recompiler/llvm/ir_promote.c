/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The guest's registers as locals of the whole function. The emitters read and write X0-X30 and SP, NZCV and V0-V31
   in the CPU structure, and a call that takes c can read or change any of them, so LLVM keeps each in memory across
   every loop that has one, even a call it never makes (an FP helper's slow path). The poll on a backward branch is
   not in the loop: it leaves the function (ir_codegen.c's poll).
   Once the function is emitted, ir_promote_registers moves those accesses onto a stack slot per register, which
   SROA and mem2reg make SSA values. It runs at O1 and above: at O0 those passes do not run, so the slots and the
   copies at every site stay, which costs call-heavy code and grows the JIT's arena. Two passes over the function's
   blocks keep the copies to the few that matter: a call or return (a site) stores the registers written since the
   last site on some path to it (dirty), and the function loads a register on entry and after a call only where it is
   read before it is written or stored by a site (live). A store of part of a register (a lane) reads the rest. The
   CPU structure holds the architectural registers wherever anything else can look at them: in the runtime, a callee,
   a helper. In between (a fault's crash report) it holds them as they were at the last call. FPCR and FPSR are
   locals too, for the FP fast paths' tests: the helpers that use only them (ir_noregs) are sites for those two
   alone, which read FPCR and change FPSR */
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/aarch64/cpu.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* x[0..31], then n, z, cf, vf, then v[0..31], then fpcr, fpsr */
#define SLOT_FLAGS 32
#define SLOT_V     36
#define SLOT_FPCR  68
#define SLOT_FPSR  69
#define SLOTS      70

typedef struct {
    LLVMValueRef inst;    /* a load or store of the CPU structure */
    unsigned slot, delta; /* its register, and its offset in it */
    unsigned kind;        /* READ, WRITE (all of it) or PART (a store of part of it) */
} Access;

enum { READ, WRITE, PART, SITE };

/* a set of registers */
typedef struct {
    SnxU64 w[2];
} Set;

/* what a block does, in order: an access (READ, WRITE, PART) to a register, or a site */
typedef struct {
    unsigned kind, index; /* the register, or the site's index */
} Event;

typedef struct {
    LLVMValueRef inst;
    Set seen, changed; /* what it can read of the registers in the CPU structure, and what it can change */
    Set dirty;         /* stored before it */
    Set reload;        /* loaded after it (a call that returns) */
} Site;

typedef struct {
    LLVMBasicBlockRef block;
    size_t first, count; /* its events */
    Set dirty_in, live_in, live_out;
} Block;

typedef struct {
    Access* access;
    size_t count, cap;
    unsigned char used[SLOTS];
} Scan;

static size_t slot_offset(unsigned slot) {
    if (slot < SLOT_FLAGS) return offsetof(CPU, x) + 8 * (size_t)slot;
    if (slot < SLOT_V) return offsetof(CPU, n) + 4 * (size_t)(slot - SLOT_FLAGS);
    if (slot < SLOT_FPCR) return offsetof(CPU, v) + 16 * (size_t)(slot - SLOT_V);
    return slot == SLOT_FPCR ? offsetof(CPU, fpcr) : offsetof(CPU, fpsr);
}
static unsigned slot_size(unsigned slot) { return slot < SLOT_FLAGS ? 8 : slot < SLOT_V || slot >= SLOT_FPCR ? 4 : 16; }

/* the register [offset, offset+size) lies in, SLOTS for none, SLOTS+1 for an access across two */
static unsigned slot_of(size_t offset, size_t size) {
    unsigned slot;
    if (offset < offsetof(CPU, x) + sizeof(((CPU*)0)->x))
        slot = (unsigned)((offset - offsetof(CPU, x)) / 8);
    else if (offset >= offsetof(CPU, n) && offset < offsetof(CPU, vf) + 4)
        slot = SLOT_FLAGS + (unsigned)((offset - offsetof(CPU, n)) / 4);
    else if (offset >= offsetof(CPU, v) && offset < offsetof(CPU, v) + sizeof(((CPU*)0)->v))
        slot = SLOT_V + (unsigned)((offset - offsetof(CPU, v)) / 16);
    else if (offset == offsetof(CPU, fpcr) || offset == offsetof(CPU, fpsr))
        slot = offset == offsetof(CPU, fpcr) ? SLOT_FPCR : SLOT_FPSR;
    else
        return offset + size > offsetof(CPU, x) && offset < offsetof(CPU, x) + sizeof(((CPU*)0)->x) ? SLOTS + 1
                                                                                                     : SLOTS;
    return offset + size <= slot_offset(slot) + slot_size(slot) ? slot : SLOTS + 1;
}

static int push_access(Scan* s, LLVMValueRef inst, unsigned slot, unsigned delta, unsigned kind) {
    if (s->count == s->cap) {
        size_t cap = s->cap ? 2 * s->cap : 64;
        Access* grown = (Access*)realloc(s->access, cap * sizeof(*grown));
        if (!grown) return -1;
        s->access = grown;
        s->cap = cap;
    }
    s->access[s->count].inst = inst;
    s->access[s->count].slot = slot;
    s->access[s->count].delta = delta;
    s->access[s->count].kind = kind;
    ++s->count;
    return 0;
}

/* a load or store of the field at offset through pointer: 0, or -1 when its register can't be promoted */
static int access(SnxIr* ir, Scan* s, LLVMValueRef inst, LLVMValueRef pointer, size_t offset) {
    LLVMTargetDataRef layout = LLVMGetModuleDataLayout(ir->mod);
    int store = LLVMIsAStoreInst(inst) != NULL;
    if (store && LLVMGetOperand(inst, 1) != pointer) return -1; /* the pointer itself stored somewhere */
    LLVMValueRef value = store ? LLVMGetOperand(inst, 0) : inst;
    unsigned size = (unsigned)LLVMStoreSizeOfType(layout, LLVMTypeOf(value));
    unsigned slot = slot_of(offset, size);
    if (slot == SLOTS) return 0;
    if (slot > SLOTS || LLVMGetVolatile(inst) || LLVMGetOrdering(inst) != LLVMAtomicOrderingNotAtomic) return -1;
    s->used[slot] = 1;
    unsigned kind = !store ? READ : size == slot_size(slot) ? WRITE : PART;
    return push_access(s, inst, slot, (unsigned)(offset - slot_offset(slot)), kind);
}

/* the uses of a pointer to the CPU structure at offset: loads and stores, and calls (sites already) */
static int pointer_uses(SnxIr* ir, Scan* s, LLVMValueRef pointer, size_t offset) {
    for (LLVMUseRef use = LLVMGetFirstUse(pointer); use; use = LLVMGetNextUse(use)) {
        LLVMValueRef user = LLVMGetUser(use);
        if (LLVMIsALoadInst(user) || LLVMIsAStoreInst(user)) {
            if (access(ir, s, user, pointer, offset)) return -1;
        } else if (LLVMIsACallInst(user)) {
            continue; /* every call is a site, the helpers that never touch the registers included */
        } else if (LLVMIsAGetElementPtrInst(user) && LLVMGetOperand(user, 0) == pointer && pointer == ir->c &&
                   LLVMGetNumOperands(user) == 2 && LLVMIsAConstantInt(LLVMGetOperand(user, 1)) &&
                   LLVMGetGEPSourceElementType(user) == ir->t_i8) {
            if (pointer_uses(ir, s, user, (size_t)LLVMConstIntGetZExtValue(LLVMGetOperand(user, 1)))) return -1;
        } else if (slot_of(offset, 1) != SLOTS) {
            return -1; /* a register reached some other way */
        }
    }
    return 0;
}

/* every pointer the call is given is a local of the function (ir_local's allocas) */
static int locals_only(LLVMValueRef call) {
    unsigned n = LLVMGetNumArgOperands(call);
    for (unsigned i = 0; i < n; ++i) {
        LLVMValueRef arg = LLVMGetOperand(call, i);
        if (LLVMGetTypeKind(LLVMTypeOf(arg)) == LLVMPointerTypeKind && !LLVMIsAAllocaInst(arg)) return 0;
    }
    return 1;
}
/* what a call can see of the registers in the CPU structure: NONE for an intrinsic, a function that touches no
   memory (ir_pure), one that touches only its arguments' memory (ir_argmem, the argument bits of "memory") given
   locals alone (snx_fp_host_lanes), or a helper not given c that writes guest memory alone (ir_nocpu: DC ZVA's);
   ENV for a helper that uses only FPCR, FPSR and the exclusive monitor (ir_noregs); ALL else */
enum { NONE, ENV, ALL };
static int call_site(LLVMValueRef call) {
    LLVMValueRef callee = LLVMGetCalledValue(call);
    LLVMAttributeRef memory;
    if (callee && LLVMIsAFunction(callee)) {
        if (LLVMGetIntrinsicID(callee)) return NONE;
        memory = LLVMGetEnumAttributeAtIndex(callee, LLVMAttributeFunctionIndex,
                                             LLVMGetEnumAttributeKindForName("memory", 6));
        if (memory && LLVMGetEnumAttributeValue(memory) == 0) return NONE;
        if (memory && !(LLVMGetEnumAttributeValue(memory) & ~3ull) && locals_only(call)) return NONE;
        if (LLVMGetStringAttributeAtIndex(callee, LLVMAttributeFunctionIndex, "snx-nocpu", 9)) return NONE;
        if (LLVMGetStringAttributeAtIndex(callee, LLVMAttributeFunctionIndex, "snx-noregs", 10)) return ENV;
    }
    return ALL;
}

/* the return of a musttail call, which nothing may come between: the call's site stored the registers */
static int after_tail_call(LLVMValueRef ret) {
    LLVMValueRef call = LLVMGetPreviousInstruction(ret);
    return call && LLVMIsACallInst(call) && LLVMGetTailCallKind(call) == LLVMTailCallKindMustTail;
}

static LLVMTypeRef slot_type(SnxIr* ir, unsigned slot) {
    if (slot >= SLOT_V && slot < SLOT_FPCR) return LLVMVectorType(ir->t_i64, 2);
    return slot < SLOT_FLAGS ? ir->t_i64 : ir->t_i32;
}

/* *at = *from, as the register's own type */
static void copy(SnxIr* ir, LLVMBuilderRef b, unsigned slot, LLVMValueRef from, LLVMValueRef at, int to_cpu) {
    LLVMValueRef load = LLVMBuildLoad2(b, slot_type(ir, slot), from, "");
    LLVMSetAlignment(load, slot_size(slot) > 8 ? 8 : slot_size(slot));
    LLVMValueRef store = LLVMBuildStore(b, load, at);
    LLVMSetAlignment(store, slot_size(slot) > 8 ? 8 : slot_size(slot));
    ir_tag_cpu(ir, to_cpu ? store : load);
}

static LLVMValueRef cpu_pointer(SnxIr* ir, LLVMBuilderRef b, unsigned slot) {
    LLVMValueRef index = ir_i64(ir, slot_offset(slot));
    if (!slot_offset(slot)) return ir->c;
    return LLVMBuildInBoundsGEP2(b, ir->t_i8, ir->c, &index, 1, "");
}

static void set_add(Set* s, unsigned slot) { s->w[slot >> 6] |= (SnxU64)1 << (slot & 63); }
static void set_remove(Set* s, unsigned slot) { s->w[slot >> 6] &= ~((SnxU64)1 << (slot & 63)); }
static int set_has(const Set* s, unsigned slot) { return (int)(s->w[slot >> 6] >> (slot & 63)) & 1; }
/* *to |= from: whether that changed it */
static int set_join(Set* to, const Set* from) {
    Set old = *to;
    to->w[0] |= from->w[0];
    to->w[1] |= from->w[1];
    return old.w[0] != to->w[0] || old.w[1] != to->w[1];
}

static int access_by_inst(const void* a, const void* b) {
    SnxU64 x = (SnxU64)(uintptr_t)((const Access*)a)->inst, y = (SnxU64)(uintptr_t)((const Access*)b)->inst;
    return x < y ? -1 : x > y;
}

/* the index of block in the sorted keys, whose values are block indices */
typedef struct {
    LLVMBasicBlockRef block;
    size_t index;
} BlockKey;
static int key_by_block(const void* a, const void* b) {
    SnxU64 x = (SnxU64)(uintptr_t)((const BlockKey*)a)->block, y = (SnxU64)(uintptr_t)((const BlockKey*)b)->block;
    return x < y ? -1 : x > y;
}
static size_t block_index(const BlockKey* keys, size_t count, LLVMBasicBlockRef block) {
    BlockKey key;
    key.block = block;
    BlockKey* found = (BlockKey*)bsearch(&key, keys, count, sizeof(*keys), key_by_block);
    return found ? found->index : count;
}

static int returns(LLVMValueRef site) {
    return !LLVMIsAReturnInst(site) && LLVMGetTailCallKind(site) != LLVMTailCallKindMustTail;
}

/* what a site can see: every register, or (env) FPCR and FPSR; and of those what it can change */
static Set seen(int env) {
    Set s = {0};
    if (!env) return (Set){{~0ull, ~0ull}};
    set_add(&s, SLOT_FPCR);
    set_add(&s, SLOT_FPSR);
    return s;
}
static Set changed(int env) {
    Set s = {0};
    if (!env) return (Set){{~0ull, ~0ull}};
    set_add(&s, SLOT_FPSR);
    return s;
}

/* the dirty registers after a block's events, from those before it; with record, what each site stores */
static Set dirty_through(const Block* block, const Event* events, Site* sites, int record, Set dirty) {
    for (size_t e = block->first; e < block->first + block->count; ++e) {
        const Event* ev = &events[e];
        if (ev->kind == WRITE || ev->kind == PART)
            set_add(&dirty, ev->index);
        else if (ev->kind == SITE) {
            Set mask = sites[ev->index].seen;
            if (record) {
                sites[ev->index].dirty.w[0] = dirty.w[0] & mask.w[0];
                sites[ev->index].dirty.w[1] = dirty.w[1] & mask.w[1];
            }
            dirty.w[0] &= ~mask.w[0];
            dirty.w[1] &= ~mask.w[1];
        }
    }
    return dirty;
}

/* the live registers before a block's events, from those after it; with reload, what each site loads after it */
static Set live_through(const Block* block, const Event* events, Site* sites, int reload, Set live) {
    for (size_t e = block->first + block->count; e-- > block->first;) {
        const Event* ev = &events[e];
        if (ev->kind == READ || ev->kind == PART)
            set_add(&live, ev->index);
        else if (ev->kind == WRITE)
            set_remove(&live, ev->index);
        else {
            const Site* site = &sites[ev->index];
            Set mask = site->changed;
            if (reload) {
                sites[ev->index].reload.w[0] = live.w[0] & mask.w[0];
                sites[ev->index].reload.w[1] = live.w[1] & mask.w[1];
            }
            live.w[0] = (live.w[0] & ~mask.w[0]) | site->dirty.w[0];
            live.w[1] = (live.w[1] & ~mask.w[1]) | site->dirty.w[1];
        }
    }
    return live;
}

/* what a site sees and changes: an ENV helper FPCR and FPSR (changing FPSR), any other call or return every
   register */
static void site_sets(const SnxIr* ir, LLVMValueRef inst, Set* see, Set* change) {
    (void)ir;
    if (LLVMIsACallInst(inst) && LLVMGetTailCallKind(inst) != LLVMTailCallKindMustTail) {
        if (call_site(inst) == ENV) {
            *see = seen(1);
            *change = changed(1);
            return;
        }
    }
    *see = seen(0);
    *change = changed(0);
}

/* Each block's loop depth, at most LOOP_DEPTH: the natural loops holding it. A loop is a header a depth-first walk
   from the entry comes back to (a retreating edge), its body the blocks that reach one of its latches without passing
   the header. The dispatch block's switch to every label is no loop: it is the entry at any leader. 0, or -1 out of
   memory */
#define LOOP_DEPTH  4
#define LOOP_WEIGHT 4 /* the bits a weight grows by a level: 16 times, a loop's turns against its way in */
typedef struct {
    size_t latch, header;
} Retreat;
static int by_header(const void* a, const void* b) {
    size_t x = ((const Retreat*)a)->header, y = ((const Retreat*)b)->header;
    return x < y ? -1 : x > y;
}
static int loop_depths(const Block* blocks, size_t nblocks, const BlockKey* keys, unsigned char* depth) {
    size_t *succ_first = (size_t*)calloc(nblocks + 1, sizeof(size_t)),
           *pred_first = (size_t*)calloc(nblocks + 1, sizeof(size_t));
    size_t *succs = NULL, *preds = NULL, *fill = NULL, *stamp = NULL, *stack = NULL, *next = NULL, nedges = 0;
    size_t nretreats = 0, i, k, top;
    unsigned char* color = (unsigned char*)calloc(nblocks + 1, 1);
    Retreat* retreats = NULL;
    int rc = -1;
    memset(depth, 0, nblocks);
    if (!succ_first || !pred_first || !color) goto done;
    for (size_t pass = 0; pass < 2; ++pass) {
        if (pass) {
            for (i = 0; i < nblocks; ++i) {
                succ_first[i + 1] += succ_first[i];
                pred_first[i + 1] += pred_first[i];
            }
            succs = (size_t*)malloc((nedges ? nedges : 1) * sizeof(size_t));
            preds = (size_t*)malloc((nedges ? nedges : 1) * sizeof(size_t));
            fill = (size_t*)malloc((nblocks ? nblocks : 1) * sizeof(size_t));
            if (!succs || !preds || !fill) goto done;
            memcpy(fill, pred_first, nblocks * sizeof(size_t));
            nedges = 0;
        }
        for (i = 0; i < nblocks; ++i) {
            LLVMValueRef term = LLVMGetBasicBlockTerminator(blocks[i].block);
            unsigned n = term && !LLVMIsASwitchInst(term) ? LLVMGetNumSuccessors(term) : 0;
            for (unsigned j = 0; j < n; ++j) {
                k = block_index(keys, nblocks, LLVMGetSuccessor(term, j));
                if (k >= nblocks) continue;
                if (pass) {
                    succs[nedges++] = k;
                    preds[fill[k]++] = i;
                } else {
                    ++succ_first[i + 1];
                    ++pred_first[k + 1];
                    ++nedges;
                }
            }
        }
    }
    /* the retreating edges: to a block still on the walk's stack */
    stack = (size_t*)malloc((nblocks + 1) * sizeof(size_t));
    next = (size_t*)calloc(nblocks + 1, sizeof(size_t));
    retreats = (Retreat*)malloc((nedges ? nedges : 1) * sizeof(Retreat));
    stamp = (size_t*)calloc(nblocks + 1, sizeof(size_t));
    if (!stack || !next || !retreats || !stamp) goto done;
    top = 0;
    if (nblocks) {
        stack[top++] = 0;
        color[0] = 1;
        next[0] = succ_first[0];
    }
    while (top) {
        size_t u = stack[top - 1];
        if (next[u] == succ_first[u + 1]) {
            color[u] = 2;
            --top;
            continue;
        }
        k = succs[next[u]++];
        if (color[k] == 1) {
            retreats[nretreats].latch = u;
            retreats[nretreats++].header = k;
        } else if (!color[k]) {
            color[k] = 1;
            next[k] = succ_first[k];
            stack[top++] = k;
        }
    }
    /* each header's body, once: from its latches backwards, the header the wall */
    qsort(retreats, nretreats, sizeof(*retreats), by_header);
    for (size_t r = 0; r < nretreats; ++r) {
        size_t header = retreats[r].header, mark = header + 1;
        if (stamp[header] != mark) {
            stamp[header] = mark;
            if (depth[header] < LOOP_DEPTH) ++depth[header];
        }
        top = 0;
        if (stamp[retreats[r].latch] != mark) { /* each block counted and pushed once a header */
            stamp[retreats[r].latch] = mark;
            if (depth[retreats[r].latch] < LOOP_DEPTH) ++depth[retreats[r].latch];
            stack[top++] = retreats[r].latch;
        }
        while (top) {
            size_t b = stack[--top];
            for (size_t p = pred_first[b]; p < pred_first[b + 1]; ++p)
                if (stamp[preds[p]] != mark) {
                    stamp[preds[p]] = mark;
                    if (depth[preds[p]] < LOOP_DEPTH) ++depth[preds[p]];
                    stack[top++] = preds[p];
                }
        }
    }
    rc = 0;
done:
    free(succ_first);
    free(pred_first);
    free(succs);
    free(preds);
    free(fill);
    free(stamp);
    free(stack);
    free(next);
    free(color);
    free(retreats);
    return rc;
}

/* the successor of a two-way branch its weights make at least 1000 times less likely than the other (ir_unlikely's
   1 to 2000, or a profile's), 2 for none */
static unsigned unlikely_successor(LLVMValueRef branch, unsigned prof) {
    LLVMValueRef md = LLVMGetMetadata(branch, prof), ops[3];
    unsigned length = 0;
    const char* name;
    SnxU64 a, b;
    if (!md || LLVMGetMDNodeNumOperands(md) != 3) return 2;
    LLVMGetMDNodeOperands(md, ops);
    name = LLVMGetMDString(ops[0], &length);
    if (!name || length != 14 || memcmp(name, "branch_weights", 14) || !LLVMIsAConstantInt(ops[1]) ||
        !LLVMIsAConstantInt(ops[2]))
        return 2;
    a = LLVMConstIntGetZExtValue(ops[1]);
    b = LLVMConstIntGetZExtValue(ops[2]);
    return a * 1000 <= b ? 0 : b * 1000 <= a ? 1 : 2;
}
/* the blocks the entry reaches without taking an unlikely edge (the poll, a return to another address, a stale
   function, an entry at another leader): what only such an edge reaches runs seldom. 0, or -1 out of memory */
static int likely_blocks(SnxIr* ir, const Block* blocks, size_t nblocks, const BlockKey* keys, unsigned char* likely) {
    size_t* stack = (size_t*)malloc((nblocks ? nblocks : 1) * sizeof(size_t));
    unsigned prof = LLVMGetMDKindIDInContext(ir->ctx, "prof", 4);
    size_t top = 0;
    if (!stack) return -1;
    memset(likely, 0, nblocks);
    if (nblocks) {
        likely[0] = 1;
        stack[top++] = 0;
    }
    while (top) {
        LLVMValueRef term = LLVMGetBasicBlockTerminator(blocks[stack[--top]].block);
        unsigned n = term ? LLVMGetNumSuccessors(term) : 0, cold = n == 2 ? unlikely_successor(term, prof) : 2;
        for (unsigned j = 0; j < n; ++j) {
            size_t k = j == cold ? nblocks : block_index(keys, nblocks, LLVMGetSuccessor(term, j));
            if (k >= nblocks || likely[k]) continue;
            likely[k] = 1;
            stack[top++] = k;
        }
    }
    free(stack);
    return 0;
}

/* the definition of each register reaching each block: UNSEEN before any path reaches it, ENTRY the entry's load,
   else the index of the block that defines it (a write or a site's reload), or of the block a PHI joins it in, where
   its predecessors bring different ones. A block takes its predecessors' meet again whenever one of them changed,
   until none does: sweeping every block until nothing changed moved a change one block a sweep wherever a successor
   came first, and a function of 58185 blocks took minutes of sweeps. phis[slot] counts the PHIs where the register is
   live. 0, or -1 out of memory */
#define UNSEEN ((size_t)-1)
#define ENTRY  ((size_t)-2)
static int count_phis(const Block* blocks, size_t nblocks, const BlockKey* keys, const Event* events,
                      const Site* sites, size_t* phis) {
    size_t *in = (size_t*)malloc(nblocks * SLOTS * sizeof(*in)), *pred_first = (size_t*)calloc(nblocks + 1, sizeof(size_t));
    size_t *succ_first = (size_t*)calloc(nblocks + 1, sizeof(size_t));
    size_t* work = (size_t*)malloc((nblocks ? nblocks : 1) * sizeof(*work));
    size_t *preds = NULL, *succs = NULL, npreds = 0, nsucc = 0, k, head = 0, queued_n = 0;
    Set* defines = (Set*)calloc(nblocks, sizeof(*defines));
    unsigned char* queued = (unsigned char*)calloc(nblocks ? nblocks : 1, 1);
    int changed, rc = -1;
    if (!in || !pred_first || !succ_first || !work || !defines || !queued) goto done;
    /* the predecessors and successors, as lists by block: counted, then placed */
    for (size_t pass = 0; pass < 2; ++pass) {
        size_t* fill = NULL;
        if (pass) {
            for (size_t i = 0; i < nblocks; ++i) {
                pred_first[i + 1] += pred_first[i];
                succ_first[i + 1] += succ_first[i];
            }
            preds = (size_t*)malloc((npreds ? npreds : 1) * sizeof(*preds));
            succs = (size_t*)malloc((npreds ? npreds : 1) * sizeof(*succs));
            fill = (size_t*)malloc((nblocks ? nblocks : 1) * sizeof(*fill));
            if (!preds || !succs || !fill) {
                free(fill);
                goto done;
            }
            memcpy(fill, pred_first, nblocks * sizeof(*fill));
        }
        for (size_t i = 0; i < nblocks; ++i) {
            LLVMValueRef term = LLVMGetBasicBlockTerminator(blocks[i].block);
            unsigned n = term ? LLVMGetNumSuccessors(term) : 0;
            for (unsigned j = 0; j < n; ++j) {
                k = block_index(keys, nblocks, LLVMGetSuccessor(term, j));
                if (k >= nblocks) continue;
                if (pass) {
                    preds[fill[k]++] = i;
                    succs[nsucc++] = k;
                } else {
                    ++pred_first[k + 1];
                    ++succ_first[i + 1];
                    ++npreds;
                }
            }
        }
        free(fill);
    }
    for (size_t i = 0; i < nblocks; ++i)
        for (size_t e = blocks[i].first; e < blocks[i].first + blocks[i].count; ++e) {
            const Event* ev = &events[e];
            if (ev->kind == WRITE || ev->kind == PART)
                set_add(&defines[i], ev->index);
            else if (ev->kind == SITE)
                (void)set_join(&defines[i], &sites[ev->index].reload);
        }
    for (size_t i = 0; i < nblocks * SLOTS; ++i) in[i] = i < SLOTS ? ENTRY : UNSEEN;
    /* every block but the entry once, in order, then again each block a predecessor of changed: a PHI stays one (the
       meet only moves up: UNSEEN, one definition, a PHI), so the work is bounded */
    for (k = 1; k < nblocks; ++k) {
        work[queued_n++] = k;
        queued[k] = 1;
    }
    while (queued_n) {
        k = work[head];
        head = head + 1 == nblocks ? 0 : head + 1;
        --queued_n;
        queued[k] = 0;
        changed = 0;
        for (unsigned slot = 0; slot < SLOTS; ++slot) {
            size_t meet = UNSEEN;
            if (in[k * SLOTS + slot] == k) continue;
            for (size_t p = pred_first[k]; p < pred_first[k + 1]; ++p) {
                size_t from = preds[p], out = set_has(&defines[from], slot) ? from : in[from * SLOTS + slot];
                if (out == UNSEEN || out == meet) continue;
                if (meet != UNSEEN) {
                    meet = k;
                    break;
                }
                meet = out;
            }
            if (meet != in[k * SLOTS + slot]) {
                in[k * SLOTS + slot] = meet;
                changed = 1;
            }
        }
        if (changed)
            for (size_t i = succ_first[k]; i < succ_first[k + 1]; ++i) {
                size_t to = succs[i];
                if (!to || queued[to]) continue;
                work[(head + queued_n++) % nblocks] = to;
                queued[to] = 1;
            }
    }
    memset(phis, 0, SLOTS * sizeof(*phis));
    for (k = 1; k < nblocks; ++k)
        for (unsigned slot = 0; slot < SLOTS; ++slot)
            if (in[k * SLOTS + slot] == k && set_has(&blocks[k].live_in, slot)) ++phis[slot];
    rc = 0;
done:
    free(in);
    free(pred_first);
    free(succ_first);
    free(work);
    free(preds);
    free(succs);
    free(defines);
    free(queued);
    return rc;
}

/* A block's first site stores the registers dirty on some way into the block; one not written in the block before
   the site and clean on another way in has its store made on the ways it is dirty on instead: at the end of the
   predecessor, or in a block of its own on the edge when the predecessor goes elsewhere too. A loop whose call
   stores a register written before the loop then stores it once, on the way in, not on every turn: the value the
   CPU structure has on the way back is the one it would store (regalloc2's redundant spill, ION.md). A block that
   starts with PHIs (the FP fast paths' joins) keeps its stores: its edges are left as they are. 0, or -1 out of
   memory */
static int edge_stores(SnxIr* ir, LLVMValueRef fn, LLVMBuilderRef b, const Block* blocks, size_t nblocks,
                       const Event* events, Site* sites, LLVMValueRef* local) {
    Set* out = (Set*)malloc((nblocks ? nblocks : 1) * sizeof(*out));
    size_t i, e, p;
    if (!out) return -1;
    for (i = 0; i < nblocks; ++i) out[i] = dirty_through(&blocks[i], events, sites, 0, blocks[i].dirty_in);
    for (i = 1; i < nblocks; ++i) {
        Set written = {{0, 0}}, moved = {{0, 0}};
        size_t site = (size_t)-1;
        LLVMValueRef first = LLVMGetFirstInstruction(blocks[i].block);
        if (first && LLVMIsAPHINode(first)) continue;
        for (e = blocks[i].first; e < blocks[i].first + blocks[i].count; ++e) {
            if (events[e].kind == WRITE || events[e].kind == PART) set_add(&written, events[e].index);
            if (events[e].kind == SITE) {
                site = events[e].index;
                break;
            }
        }
        if (site == (size_t)-1) continue;
        for (unsigned slot = 0; slot < SLOTS; ++slot) {
            if (!local[slot] || !set_has(&sites[site].dirty, slot) || set_has(&written, slot)) continue;
            for (p = 0; p < nblocks; ++p) {
                LLVMValueRef term = LLVMGetBasicBlockTerminator(blocks[p].block);
                unsigned n = term ? LLVMGetNumSuccessors(term) : 0, j;
                for (j = 0; j < n; ++j)
                    if (LLVMGetSuccessor(term, j) == blocks[i].block && !set_has(&out[p], slot)) break;
                if (j < n) break;
            }
            if (p < nblocks) set_add(&moved, slot);
        }
        if (!moved.w[0] && !moved.w[1]) continue;
        sites[site].dirty.w[0] &= ~moved.w[0];
        sites[site].dirty.w[1] &= ~moved.w[1];
        for (p = 0; p < nblocks; ++p) {
            LLVMValueRef term = LLVMGetBasicBlockTerminator(blocks[p].block);
            unsigned n = term ? LLVMGetNumSuccessors(term) : 0;
            Set need = {{out[p].w[0] & moved.w[0], out[p].w[1] & moved.w[1]}};
            if (!need.w[0] && !need.w[1]) continue;
            for (unsigned j = 0; j < n; ++j) {
                if (LLVMGetSuccessor(term, j) != blocks[i].block) continue;
                if (n == 1) {
                    LLVMPositionBuilderBefore(b, term);
                } else {
                    LLVMBasicBlockRef edge = LLVMAppendBasicBlockInContext(ir->ctx, fn, "edge");
                    LLVMSetSuccessor(term, j, edge);
                    LLVMPositionBuilderAtEnd(b, edge);
                    LLVMPositionBuilderBefore(b, LLVMBuildBr(b, blocks[i].block));
                }
                for (unsigned slot = 0; slot < SLOTS; ++slot)
                    if (set_has(&need, slot)) {
                        copy(ir, b, slot, local[slot], cpu_pointer(ir, b, slot), 1);
                        if (ir->stats.enabled) {
                            ++ir->stats.sync_stores;
                            ++ir->stats.edge_stores;
                            ir->stats.store_bytes += slot_size(slot);
                        }
                    }
            }
        }
    }
    free(out);
    return 0;
}

void ir_promote_registers(SnxIr* ir, LLVMValueRef fn) {
    Scan s = {0};
    LLVMBasicBlockRef bb;
    LLVMValueRef local[SLOTS];
    Block* blocks = NULL;
    BlockKey* keys = NULL;
    Event* events = NULL;
    Site* sites = NULL;
    size_t i, nblocks = 0, nevents = 0, nsites = 0, cap = 0, site_cap = 0, k;
    size_t phis[SLOTS];
    int changed;
    if (pointer_uses(ir, &s, ir->c, 0) || !s.count) goto done;
    qsort(s.access, s.count, sizeof(*s.access), access_by_inst);
    /* the blocks and their events */
    nblocks = LLVMCountBasicBlocks(fn);
    blocks = (Block*)calloc(nblocks, sizeof(*blocks));
    keys = (BlockKey*)malloc(nblocks * sizeof(*keys));
    if (!blocks || !keys) goto done;
    for (bb = LLVMGetFirstBasicBlock(fn), i = 0; bb; bb = LLVMGetNextBasicBlock(bb), ++i) {
        blocks[i].block = bb;
        blocks[i].first = nevents;
        keys[i].block = bb;
        keys[i].index = i;
        for (LLVMValueRef inst = LLVMGetFirstInstruction(bb); inst; inst = LLVMGetNextInstruction(inst)) {
            Event ev;
            if (LLVMIsALoadInst(inst) || LLVMIsAStoreInst(inst)) {
                Access key, *a;
                key.inst = inst;
                a = (Access*)bsearch(&key, s.access, s.count, sizeof(*s.access), access_by_inst);
                if (!a) continue;
                ev.kind = a->kind;
                ev.index = a->slot;
            } else if ((LLVMIsACallInst(inst) && call_site(inst) != NONE) ||
                       (LLVMIsAReturnInst(inst) && !after_tail_call(inst))) {
                if (nsites == site_cap) {
                    Site* grown;
                    site_cap = site_cap ? 2 * site_cap : 32;
                    grown = (Site*)realloc(sites, site_cap * sizeof(*grown));
                    if (!grown) goto done;
                    sites = grown;
                }
                sites[nsites] = (Site){0};
                sites[nsites].inst = inst;
                site_sets(ir, inst, &sites[nsites].seen, &sites[nsites].changed);
                ev.kind = SITE;
                ev.index = (unsigned)nsites++;
            } else
                continue;
            if (nevents == cap) {
                Event* grown;
                cap = cap ? 2 * cap : 256;
                grown = (Event*)realloc(events, cap * sizeof(*grown));
                if (!grown) goto done;
                events = grown;
            }
            events[nevents++] = ev;
        }
        blocks[i].count = nevents - blocks[i].first;
    }
    qsort(keys, nblocks, sizeof(*keys), key_by_block);
    /* dirty: forward, from nothing on entry */
    do {
        changed = 0;
        for (i = 0; i < nblocks; ++i) {
            Set out = dirty_through(&blocks[i], events, sites, 0, blocks[i].dirty_in);
            LLVMValueRef term = LLVMGetBasicBlockTerminator(blocks[i].block);
            unsigned n = term ? LLVMGetNumSuccessors(term) : 0, j;
            for (j = 0; j < n; ++j) {
                k = block_index(keys, nblocks, LLVMGetSuccessor(term, j));
                if (k < nblocks && set_join(&blocks[k].dirty_in, &out)) changed = 1;
            }
        }
    } while (changed);
    for (i = 0; i < nblocks; ++i) (void)dirty_through(&blocks[i], events, sites, 1, blocks[i].dirty_in);
    /* live: backward, from nothing after a return */
    do {
        changed = 0;
        for (i = nblocks; i-- > 0;) {
            LLVMValueRef term = LLVMGetBasicBlockTerminator(blocks[i].block);
            unsigned n = term ? LLVMGetNumSuccessors(term) : 0, j;
            for (j = 0; j < n; ++j) {
                k = block_index(keys, nblocks, LLVMGetSuccessor(term, j));
                if (k < nblocks) (void)set_join(&blocks[i].live_out, &blocks[k].live_in);
            }
            Set in = live_through(&blocks[i], events, sites, 0, blocks[i].live_out);
            if (set_join(&blocks[i].live_in, &in)) changed = 1;
        }
    } while (changed);
    for (i = 0; i < nblocks; ++i) (void)live_through(&blocks[i], events, sites, 1, blocks[i].live_out);
    /* the PHIs mem2reg will make of each register: where paths bring different definitions of it (the entry's load,
       a block's write, a site's reload) into a block it is live into. One with more of those than the accesses it
       saves (and a few) stays in the CPU structure. A dispatch block's switch to every label makes one of each
       register live across the function: 5820 PHIs and five times the compile in one game function, which the
       register allocator then spends its time on. An access weighs 1 << LOOP_WEIGHT for each loop it is in, as
       regalloc2 weighs a use in a spill decision (liveranges.rs): it runs once a turn, and a register a loop uses
       left in memory costs a load or store there every turn, whatever the joins around calls elsewhere count. Its
       copies as a local (a store before a site, a reload after a call, the load on entry) weigh the same, and one
       whose copies outweigh its accesses stays in the CPU structure too: written once and live across a run of
       calls, it costs a reload after each as a local, a load where it is read in memory. What only an unlikely edge
       reaches (a return check's way out, the poll) weighs nothing: its stores would count every dirty register
       twice for each call */
    if (count_phis(blocks, nblocks, keys, events, sites, phis)) goto done;
    {
        size_t uses[SLOTS] = {0}, copies[SLOTS] = {0};
        unsigned char* depth = (unsigned char*)malloc(2 * nblocks + 1);
        unsigned char* likely = depth + nblocks;
        if (!depth || loop_depths(blocks, nblocks, keys, depth) || likely_blocks(ir, blocks, nblocks, keys, likely)) {
            free(depth);
            goto done;
        }
        for (i = 0; i < s.count; ++i) {
            size_t at = block_index(keys, nblocks, LLVMGetInstructionParent(s.access[i].inst));
            if (at < nblocks && likely[at]) uses[s.access[i].slot] += (size_t)1 << (LOOP_WEIGHT * depth[at]);
        }
        for (k = 0; k < nblocks; ++k)
            for (size_t e = blocks[k].first; e < blocks[k].first + blocks[k].count && likely[k]; ++e)
                if (events[e].kind == SITE) {
                    const Site* site = &sites[events[e].index];
                    size_t weight = (size_t)1 << (LOOP_WEIGHT * depth[k]);
                    for (unsigned slot = 0; slot < SLOTS; ++slot)
                        copies[slot] += weight * (size_t)(set_has(&site->dirty, slot) + set_has(&site->reload, slot));
                }
        for (unsigned slot = 0; slot < SLOTS; ++slot)
            if (set_has(&blocks[0].live_in, slot)) ++copies[slot];
        free(depth);
        for (unsigned slot = 0; slot < SLOTS; ++slot) {
            if (!s.used[slot]) continue;
            if (phis[slot] > uses[slot] + 8 || copies[slot] > uses[slot]) s.used[slot] = 0;
            if (ir->stats.enabled) ++*(s.used[slot] ? &ir->stats.promoted_slots : &ir->stats.memory_slots);
        }
    }
    LLVMBuilderRef b = LLVMCreateBuilderInContext(ir->ctx);
    /* the locals, loaded on entry where live */
    LLVMPositionBuilderBefore(b, LLVMGetFirstInstruction(blocks[0].block));
    for (unsigned slot = 0; slot < SLOTS; ++slot) {
        local[slot] = NULL;
        if (!s.used[slot]) continue;
        local[slot] = LLVMBuildAlloca(b, slot_type(ir, slot), "");
        if (ir->dump_active)
            fprintf(stderr, "IR promote slot %u cpu-offset %zu bytes %u\n", slot, slot_offset(slot), slot_size(slot));
        LLVMSetAlignment(local[slot], slot_size(slot) > 8 ? 16 : slot_size(slot));
    }
    for (unsigned slot = 0; slot < SLOTS; ++slot)
        if (local[slot] && set_has(&blocks[0].live_in, slot))
            copy(ir, b, slot, cpu_pointer(ir, b, slot), local[slot], 0);
    /* every access on its local */
    for (i = 0; i < s.count; ++i) {
        Access* a = &s.access[i];
        LLVMValueRef pointer = local[a->slot], index;
        if (!pointer) continue;
        LLVMPositionBuilderBefore(b, a->inst);
        if (a->delta) {
            index = ir_i64(ir, a->delta);
            pointer = LLVMBuildInBoundsGEP2(b, ir->t_i8, pointer, &index, 1, "");
        }
        LLVMSetOperand(a->inst, LLVMIsAStoreInst(a->inst) ? 1 : 0, pointer);
        if (LLVMGetAlignment(a->inst) > (unsigned)(a->delta ? a->delta & -a->delta : 16))
            LLVMSetAlignment(a->inst, a->delta & -a->delta);
    }
    if (edge_stores(ir, fn, b, blocks, nblocks, events, sites, local)) {
        LLVMDisposeBuilder(b);
        goto done;
    }
    /* the dirty registers into the CPU structure before each site, the live ones back after a call */
    if (ir->stats.enabled) ++ir->stats.promoted_functions;
    for (i = 0; i < nsites; ++i) {
        LLVMValueRef site = sites[i].inst;
        LLVMPositionBuilderBefore(b, site);
        for (unsigned slot = 0; slot < SLOTS; ++slot)
            if (local[slot] && set_has(&sites[i].dirty, slot)) {
                copy(ir, b, slot, local[slot], cpu_pointer(ir, b, slot), 1);
                if (ir->stats.enabled) {
                    ++ir->stats.sync_stores;
                    ir->stats.store_bytes += slot_size(slot);
                    if (LLVMIsACallInst(site)) ++ir->stats.call_stores;
                }
            }
        if (!returns(site)) continue;
        LLVMPositionBuilderBefore(b, LLVMGetNextInstruction(site));
        for (unsigned slot = 0; slot < SLOTS; ++slot)
            if (local[slot] && set_has(&sites[i].reload, slot)) {
                copy(ir, b, slot, cpu_pointer(ir, b, slot), local[slot], 0);
                if (ir->stats.enabled) {
                    ++ir->stats.sync_reloads;
                    ir->stats.reload_bytes += slot_size(slot);
                }
            }
    }
    LLVMDisposeBuilder(b);
done:
    free(s.access);
    free(blocks);
    free(keys);
    free(events);
    free(sites);
}

/* ---- the checker (ir_check_promoted), after regalloc2's (checker.rs) ---- */

/* the slot a pointer of the CPU structure reaches (c itself, or c plus a constant), SLOTS for none; *part when the
   access is narrower than the slot */
static unsigned cpu_slot(SnxIr* ir, LLVMValueRef c, LLVMValueRef pointer, LLVMTypeRef type, int* part) {
    LLVMTargetDataRef layout = LLVMGetModuleDataLayout(ir->mod);
    size_t offset, size = (size_t)LLVMStoreSizeOfType(layout, type);
    unsigned slot;
    if (pointer == c)
        offset = 0;
    else if (LLVMIsAGetElementPtrInst(pointer) && LLVMGetOperand(pointer, 0) == c && LLVMGetNumOperands(pointer) == 2 &&
             LLVMIsAConstantInt(LLVMGetOperand(pointer, 1)))
        offset = (size_t)LLVMConstIntGetZExtValue(LLVMGetOperand(pointer, 1));
    else
        return SLOTS;
    slot = slot_of(offset, size);
    if (slot >= SLOTS) return SLOTS;
    *part = size < slot_size(slot);
    return slot;
}

/* the alloca under a pointer (itself, or a GEP of it by a constant), NULL for none */
static LLVMValueRef alloca_of(LLVMValueRef pointer) {
    if (LLVMIsAAllocaInst(pointer)) return pointer;
    if (LLVMIsAGetElementPtrInst(pointer) && LLVMIsAAllocaInst(LLVMGetOperand(pointer, 0)))
        return LLVMGetOperand(pointer, 0);
    return NULL;
}

typedef struct {
    LLVMValueRef alloca;
    unsigned slot;
} Local;

/* A promoted function's state, a pair of sets: the registers whose local holds their architectural value, and those
   whose field of the CPU structure does. Every path is followed (the meet of a block's predecessors is their
   intersection: a register holds its value at a join only if it does on every way in). On entry the CPU structure holds
   every register and no local does. An emitter's write makes the local hold it and the CPU field stale; a copy moves
   "holds" from one to the other; a site (a call or a return) must find every register it sees held in the CPU
   structure, and after a call that can change a register its local is stale and its CPU field holds it only if it did
   before: the callee may leave it as it was (a way through it without the write, or a stale callee that runs nothing);
   an emitter's read, of a local or of a register left in the CPU structure, must find it held. A copy is a load right
   before a store of the loaded value, between a local and its register's field: what ir_promote_registers' copy writes.
   The problems found are counted in ir->check_failures, the first one written to ir->check_why */
void ir_check_promoted(SnxIr* ir, LLVMValueRef fn) {
    LLVMValueRef c = LLVMGetParam(fn, 0);
    size_t nblocks = LLVMCountBasicBlocks(fn), nlocals = 0, i, k;
    LLVMBasicBlockRef* order = (LLVMBasicBlockRef*)malloc((nblocks ? nblocks : 1) * sizeof(*order));
    BlockKey* keys = (BlockKey*)malloc((nblocks ? nblocks : 1) * sizeof(*keys));
    Set *local_in = (Set*)malloc((nblocks ? nblocks : 1) * sizeof(Set)),
        *cpu_in = (Set*)malloc((nblocks ? nblocks : 1) * sizeof(Set));
    unsigned char *seen_block = (unsigned char*)calloc(nblocks ? nblocks : 1, 1),
                  *queued = (unsigned char*)calloc(nblocks ? nblocks : 1, 1);
    size_t* work = (size_t*)malloc((nblocks ? nblocks : 1) * sizeof(size_t));
    Local* locals = NULL;
    size_t head = 0, count = 0, cap = 0;
    int reporting = 0;
    if (!order || !keys || !local_in || !cpu_in || !seen_block || !queued || !work) {
        ++ir->check_failures;
        goto done;
    }
    i = 0;
    for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn); bb; bb = LLVMGetNextBasicBlock(bb), ++i) {
        order[i] = bb;
        keys[i].block = bb;
        keys[i].index = i;
    }
    qsort(keys, nblocks, sizeof(*keys), key_by_block);
    /* the locals: an alloca a copy joins to a register's field */
    for (i = 0; i < nblocks; ++i)
        for (LLVMValueRef inst = LLVMGetFirstInstruction(order[i]); inst; inst = LLVMGetNextInstruction(inst)) {
            LLVMValueRef next = LLVMGetNextInstruction(inst), from, to, a;
            unsigned slot;
            int part = 0;
            if (!LLVMIsALoadInst(inst) || !next || !LLVMIsAStoreInst(next) || LLVMGetOperand(next, 0) != inst) continue;
            from = LLVMGetOperand(inst, 0);
            to = LLVMGetOperand(next, 1);
            a = LLVMIsAAllocaInst(from) ? from : LLVMIsAAllocaInst(to) ? to : NULL;
            slot = cpu_slot(ir, c, a == from ? to : from, LLVMTypeOf(inst), &part);
            if (!a || slot >= SLOTS || part) continue;
            for (k = 0; k < nlocals && locals[k].alloca != a; ++k) {}
            if (k < nlocals) continue;
            if (nlocals == cap) {
                Local* grown;
                cap = cap ? 2 * cap : 16;
                grown = (Local*)realloc(locals, cap * sizeof(*grown));
                if (!grown) {
                    ++ir->check_failures;
                    goto done;
                }
                locals = grown;
            }
            locals[nlocals].alloca = a;
            locals[nlocals++].slot = slot;
        }
    for (i = 0; i < nblocks; ++i) {
        local_in[i] = (Set){{~0ull, ~0ull}};
        cpu_in[i] = (Set){{~0ull, ~0ull}};
    }
    local_in[0] = (Set){{0, 0}};
    seen_block[0] = 1;
    work[count++] = 0;
    queued[0] = 1;
    /* to a fixed point, then once more with the problems reported */
    for (int pass = 0; pass < 2; ++pass) {
        if (pass) {
            reporting = 1;
            count = 0;
            head = 0;
            for (i = 0; i < nblocks; ++i)
                if (seen_block[i]) work[count++] = i;
        }
        while (count) {
            size_t at = work[head];
            Set local = local_in[at], cpu = cpu_in[at];
            head = head + 1 == nblocks ? 0 : head + 1;
            --count;
            queued[at] = 0;
            for (LLVMValueRef inst = LLVMGetFirstInstruction(order[at]); inst; inst = LLVMGetNextInstruction(inst)) {
                LLVMValueRef next = LLVMGetNextInstruction(inst), pointer, a;
                unsigned slot = SLOTS;
                int part = 0, problem = 0;
                const char* what = "";
                if (LLVMIsALoadInst(inst) || LLVMIsAStoreInst(inst)) {
                    int store = LLVMIsAStoreInst(inst) != NULL;
                    LLVMTypeRef type = store ? LLVMTypeOf(LLVMGetOperand(inst, 0)) : LLVMTypeOf(inst);
                    pointer = LLVMGetOperand(inst, store ? 1 : 0);
                    a = alloca_of(pointer);
                    if (a) {
                        for (k = 0; k < nlocals && locals[k].alloca != a; ++k) {}
                        if (k == nlocals) continue; /* a local of an emitter's own (a helper's argument) */
                        slot = locals[k].slot;
                        part = pointer != a ||
                               LLVMStoreSizeOfType(LLVMGetModuleDataLayout(ir->mod), type) < slot_size(slot);
                        if (!store && next && LLVMIsAStoreInst(next) && LLVMGetOperand(next, 0) == inst) {
                            int field_part = 0;
                            if (cpu_slot(ir, c, LLVMGetOperand(next, 1), type, &field_part) == slot && !field_part) {
                                /* a copy into the CPU structure: its field holds what the local holds */
                                if (set_has(&local, slot))
                                    set_add(&cpu, slot);
                                else
                                    set_remove(&cpu, slot);
                                inst = next;
                                continue;
                            }
                        }
                        if (store) {
                            LLVMValueRef value = LLVMGetOperand(inst, 0);
                            int field_part = 0;
                            if (LLVMIsALoadInst(value) && LLVMGetNextInstruction(value) == inst &&
                                cpu_slot(ir, c, LLVMGetOperand(value, 0), type, &field_part) == slot && !field_part) {
                                /* a copy from the CPU structure: the local holds what the field holds */
                                if (set_has(&cpu, slot))
                                    set_add(&local, slot);
                                else
                                    set_remove(&local, slot);
                                continue;
                            }
                            if (part && !set_has(&local, slot)) {
                                problem = 1;
                                what = "a part of its local written while the rest is not its value";
                            }
                            set_add(&local, slot);
                            set_remove(&cpu, slot);
                        } else if (!set_has(&local, slot)) {
                            problem = 1;
                            what = "its local read while it does not hold its value";
                        }
                    } else {
                        slot = cpu_slot(ir, c, pointer, type, &part);
                        if (slot >= SLOTS) continue;
                        if (!store && next && LLVMIsAStoreInst(next) && LLVMGetOperand(next, 0) == inst &&
                            alloca_of(LLVMGetOperand(next, 1)) == LLVMGetOperand(next, 1) && !part)
                            continue; /* a copy into a local: the store says what it holds */
                        if (store) {  /* an emitter's write of a register left in the CPU structure */
                            if (part && !set_has(&cpu, slot)) {
                                problem = 1;
                                what = "a part of its field written while the rest is not its value";
                            }
                            set_add(&cpu, slot);
                            set_remove(&local, slot);
                        } else if (!set_has(&cpu, slot)) {
                            problem = 1;
                            what = "its field read while it does not hold its value";
                        }
                    }
                } else if ((LLVMIsACallInst(inst) && call_site(inst) != NONE) ||
                           (LLVMIsAReturnInst(inst) && !after_tail_call(inst))) {
                    Set see, change;
                    site_sets(ir, inst, &see, &change);
                    for (unsigned r = 0; r < SLOTS; ++r)
                        if (set_has(&see, r) && !set_has(&cpu, r)) {
                            slot = r;
                            problem = 1;
                            what = LLVMIsAReturnInst(inst) ? "not in the CPU structure at a return"
                                                           : "not in the CPU structure at a call that can see it";
                            break;
                        }
                    if (LLVMIsACallInst(inst) && returns(inst)) {
                        local.w[0] &= ~change.w[0];
                        local.w[1] &= ~change.w[1];
                    }
                }
                if (problem && reporting) {
                    if (!ir->check_failures++) {
                        size_t n = 0;
                        const char* name = LLVMGetValueName2(fn, &n);
                        snprintf(ir->check_why, sizeof(ir->check_why), "%.*s, block %zu: register slot %u: %s", (int)n,
                                 name, at, slot, what);
                    }
                }
            }
            {
                LLVMValueRef term = LLVMGetBasicBlockTerminator(order[at]);
                unsigned n = term ? LLVMGetNumSuccessors(term) : 0;
                for (unsigned j = 0; j < n; ++j) {
                    size_t to = block_index(keys, nblocks, LLVMGetSuccessor(term, j));
                    Set l, m;
                    if (to >= nblocks) continue;
                    l.w[0] = local_in[to].w[0] & local.w[0];
                    l.w[1] = local_in[to].w[1] & local.w[1];
                    m.w[0] = cpu_in[to].w[0] & cpu.w[0];
                    m.w[1] = cpu_in[to].w[1] & cpu.w[1];
                    if (!seen_block[to] || l.w[0] != local_in[to].w[0] || l.w[1] != local_in[to].w[1] ||
                        m.w[0] != cpu_in[to].w[0] || m.w[1] != cpu_in[to].w[1]) {
                        local_in[to] = l;
                        cpu_in[to] = m;
                        seen_block[to] = 1;
                        if (!reporting && !queued[to]) {
                            work[(head + count++) % nblocks] = to;
                            queued[to] = 1;
                        }
                    }
                }
            }
        }
    }
done:
    free(order);
    free(keys);
    free(local_in);
    free(cpu_in);
    free(seen_block);
    free(queued);
    free(work);
    free(locals);
}
