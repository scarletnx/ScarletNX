/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The guest's registers as locals of the whole function. The emitters read and write X0-X30 and SP, NZCV and V0-V31
   in the CPU structure, and a call that takes c can read or change any of them, so LLVM keeps each in memory across
   every loop that has one, even a call it never makes (the poll on each backward branch, an FP helper's slow path).
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
    int env;    /* a helper's call that uses only FPCR and FPSR, not a call or return that can see every register */
    Set dirty;  /* stored before it */
    Set reload; /* loaded after it (a call that returns) */
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

/* what a call can see of the registers in the CPU structure: NONE for an intrinsic or a function that touches no
   memory (ir_pure), ENV for a helper that uses only FPCR, FPSR and the exclusive monitor (ir_noregs), ALL else */
enum { NONE, ENV, ALL };
static int call_site(LLVMValueRef call) {
    LLVMValueRef callee = LLVMGetCalledValue(call);
    LLVMAttributeRef memory;
    if (callee && LLVMIsAFunction(callee)) {
        if (LLVMGetIntrinsicID(callee)) return NONE;
        memory = LLVMGetEnumAttributeAtIndex(callee, LLVMAttributeFunctionIndex,
                                             LLVMGetEnumAttributeKindForName("memory", 6));
        if (memory && LLVMGetEnumAttributeValue(memory) == 0) return NONE;
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
            Set mask = seen(sites[ev->index].env);
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
            Set mask = changed(site->env);
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
                sites[nsites].env = LLVMIsACallInst(inst) && call_site(inst) == ENV;
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
       register allocator then spends its time on */
    if (count_phis(blocks, nblocks, keys, events, sites, phis)) goto done;
    {
        size_t uses[SLOTS] = {0};
        for (i = 0; i < s.count; ++i) ++uses[s.access[i].slot];
        for (unsigned slot = 0; slot < SLOTS; ++slot)
            if (phis[slot] > uses[slot] + 8) s.used[slot] = 0;
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
