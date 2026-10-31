/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    SnxU64* values;
    size_t count, capacity;
} Addresses;
typedef struct {
    SnxU64* keys;
    unsigned char* used;
    size_t count, capacity;
} AddressSet;
typedef struct {
    SnxU64 start, end;
    size_t ordinal;
} Range;

static void push(Addresses* a, SnxU64 value) {
    if (a->count == a->capacity) {
        a->capacity = a->capacity ? a->capacity * 2 : 64;
        a->values = (SnxU64*)snx_realloc(a->values, a->capacity * sizeof(*a->values));
    }
    a->values[a->count++] = value;
}

static size_t address_hash(SnxU64 value) { return (size_t)((value ^ (value >> 32)) * 2654435761UL); }

static int set_has(const AddressSet* set, SnxU64 value) {
    if (!set->capacity) return 0;
    size_t i = address_hash(value) & (set->capacity - 1);
    while (set->used[i]) {
        if (set->keys[i] == value) return 1;
        i = (i + 1) & (set->capacity - 1);
    }
    return 0;
}

static int set_add(AddressSet* set, SnxU64 value) {
    if (!set->capacity || (set->count + 1) * 4 >= set->capacity * 3) {
        AddressSet old = *set;
        set->capacity = old.capacity ? old.capacity * 2 : 128;
        set->count = 0;
        set->keys = (SnxU64*)snx_alloc(set->capacity * sizeof(*set->keys));
        set->used = (unsigned char*)snx_alloc(set->capacity);
        memset(set->used, 0, set->capacity);
        for (size_t i = 0; i < old.capacity; ++i)
            if (old.used[i]) set_add(set, old.keys[i]);
        free(old.keys);
        free(old.used);
    }
    size_t i = address_hash(value) & (set->capacity - 1);
    while (set->used[i]) {
        if (set->keys[i] == value) return 0;
        i = (i + 1) & (set->capacity - 1);
    }
    set->used[i] = 1;
    set->keys[i] = value;
    ++set->count;
    return 1;
}

static void set_free(AddressSet* set) {
    free(set->keys);
    free(set->used);
    *set = (AddressSet){0};
}
static int compare_function(const void* a, const void* b) {
    const SnxFunction* x = (const SnxFunction*)a;
    const SnxFunction* y = (const SnxFunction*)b;
    return x->start < y->start ? -1 : x->start > y->start;
}
static int compare_range(const void* a, const void* b) {
    const Range* x = (const Range*)a;
    const Range* y = (const Range*)b;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    return x->ordinal > y->ordinal ? -1 : x->ordinal < y->ordinal;
}

SnxCodeImage* snx_codegen_module(SnxCodegen* codegen, SnxU64 address) {
    SnxCodeImage* module = codegen->last_module;
    if (module) {
        SnxU64 start = module->base + module->text_off;
        if (address >= start && address - start < module->text_size) return module;
    }
    for (size_t i = 0; i < codegen->module_count; ++i) {
        module = &codegen->modules[i];
        SnxU64 start = module->base + module->text_off;
        if (address >= start && address - start < module->text_size) {
            codegen->last_module = module;
            return module;
        }
    }
    return NULL;
}

int snx_codegen_word(SnxCodegen* codegen, SnxU64 address, SnxU32* word) {
    SnxCodeImage* module = snx_codegen_module(codegen, address);
    if (!module || (address & 3)) return 0;
    size_t off = (size_t)(address - module->base);
    if (off > module->image_size || module->image_size - off < 4 ||
        address - (module->base + module->text_off) + 4 > module->text_size)
        return 0;
    const unsigned char* p = module->image + off;
    *word = snx_ld_le32(p);
    return 1;
}

SnxU64 snx_function_address(const SnxFunction* function, size_t index) {
    return function->addresses ? function->addresses[index] : function->start + (SnxU64)index * 4;
}

static size_t function_index(const SnxFunction* function, SnxU64 address) {
    if (!function->addresses) {
        if (address < function->start || address >= function->end || ((address - function->start) & 3))
            return function->count;
        return (size_t)((address - function->start) / 4);
    }
    size_t lo = 0;
    size_t hi = function->count;
    while (lo < hi) {
        size_t middle = lo + (hi - lo) / 2;
        if (function->addresses[middle] < address)
            lo = middle + 1;
        else
            hi = middle;
    }
    return lo < function->count && function->addresses[lo] == address ? lo : function->count;
}

int snx_function_contains(const SnxFunction* function, SnxU64 address) {
    return function_index(function, address) < function->count;
}

const SnxFunction* snx_codegen_function(const SnxCodegen* codegen, SnxU64 address) {
    size_t lo = 0;
    size_t hi = codegen->function_count;
    while (lo < hi) {
        size_t middle = lo + (hi - lo) / 2;
        if (codegen->functions[middle].start < address)
            lo = middle + 1;
        else
            hi = middle;
    }
    return lo < codegen->function_count && codegen->functions[lo].start == address ? &codegen->functions[lo] : NULL;
}

static const Range* inside_range(const Range* ranges, size_t count, SnxU64 address) {
    size_t lo = 0;
    size_t hi = count;
    while (lo < hi) {
        size_t middle = lo + (hi - lo) / 2;
        if (ranges[middle].start <= address)
            lo = middle + 1;
        else
            hi = middle;
    }
    return lo && address < ranges[lo - 1].end ? &ranges[lo - 1] : NULL;
}

static void add_function(SnxCodegen* codegen, SnxU64 start, SnxU64 end, Addresses* addresses) {
    if (codegen->function_count == codegen->function_capacity) {
        codegen->function_capacity = codegen->function_capacity ? codegen->function_capacity * 2 : 128;
        codegen->functions =
            (SnxFunction*)snx_realloc(codegen->functions, codegen->function_capacity * sizeof(*codegen->functions));
    }
    SnxFunction* f = &codegen->functions[codegen->function_count++];
    *f = (SnxFunction){0};
    f->start = start;
    f->end = end;
    if (addresses) {
        f->addresses = addresses->values;
        f->count = addresses->count;
        *addresses = (Addresses){0};
    } else
        f->count = (size_t)((end - start + 3) / 4);
}

/* What explore stops at besides the function's own start: the starts found so far and, for the runtime JIT, code that
   exists already (snx_codegen_discover's callback) */
typedef struct {
    const AddressSet* set;
    SnxCodegenKnown callback;
    void* user;
} Known;

static int is_known(const Known* known, SnxU64 address) {
    return set_has(known->set, address) || (known->callback && known->callback(known->user, address));
}

/* what word does to the flow, for the guest's features, with insn its decode (a branch's target) */
static SnxA64Flow decode(const SnxCodegen* codegen, SnxU32 word, SnxA64Insn* insn) {
    snx_a64_decode_word(word, codegen->features, insn);
    return insn->flow;
}

/* whether a BR on reg follows within a few instructions of address: the address taken there is a jump table's base,
   a place in the function its cases are reached from, not a function */
static int jump_table_base(SnxCodegen* codegen, SnxU64 address, SnxU32 reg) {
    for (unsigned i = 1; i <= 6; ++i) {
        SnxU32 after;
        if (!snx_codegen_word(codegen, address + (SnxU64)i * 4, &after)) return 0;
        if ((after & 0xfffffc1fUL) == 0xd61f0000UL) return ((after >> 5) & 31) == reg; /* BR */
        if (snx_a64_flow(after, codegen->features) != SNX_A64_FLOW_PLAIN) return 0;
    }
    return 0;
}

/* The address the instruction at address puts in a register, 0 for none and for a jump table's base: ADR's, or an
   ADD's of the page an ADRP a few instructions before put in its source (the linker relaxes such a pair into a NOP and
   an ADR when the target is near). Code takes function pointers this way, for a callback or a call through a
   register. Ender Magnolia's main, which has unwind info for a seventh of its text, reaches four fifths of the
   functions it misses only through them */
static SnxU64 taken_address(SnxCodegen* codegen, SnxU64 address, SnxU32 word) {
    if ((word & 0x9f000000UL) == 0x10000000UL) { /* ADR */
        if (jump_table_base(codegen, address, word & 31)) return 0;
        SnxU64 imm = ((SnxU64)((word >> 5) & 0x7ffffUL) << 2) | ((word >> 29) & 3);
        return address + (imm & 0x100000UL ? imm | ~(SnxU64)0x1fffffUL : imm);
    }
    if ((word & 0xff800000UL) != 0x91000000UL || jump_table_base(codegen, address, word & 31))
        return 0; /* ADD Xd, Xn, #imm12 */
    SnxU32 reg = (word >> 5) & 31;
    for (unsigned i = 1; i <= 8 && address >= (SnxU64)i * 4; ++i) {
        SnxU32 before;
        if (!snx_codegen_word(codegen, address - (SnxU64)i * 4, &before)) return 0;
        if ((before & 0x9f000000UL) == 0x90000000UL && (before & 31) == reg) { /* ADRP */
            SnxU64 imm = ((SnxU64)((before >> 5) & 0x7ffffUL) << 2) | ((before >> 29) & 3);
            if (imm & 0x100000UL) imm |= ~(SnxU64)0x1fffffUL;
            return ((address - (SnxU64)i * 4) & ~(SnxU64)0xfff) + (imm << 12) +
                   ((SnxU64)((word >> 10) & 0xfff) << ((word & (1UL << 22)) ? 12 : 0));
        }
        /* the register written in between (or perhaps only stored: bits 0-4 are Rt too), or a way in from elsewhere */
        if ((before & 31) == reg || snx_a64_flow(before, codegen->features) != SNX_A64_FLOW_PLAIN) return 0;
    }
    return 0;
}

static int sorted_has(const Addresses* sorted, SnxU64 address) {
    size_t lo = 0, hi = sorted->count;
    while (lo < hi) {
        size_t middle = lo + (hi - lo) / 2;
        if (sorted->values[middle] < address)
            lo = middle + 1;
        else
            hi = middle;
    }
    return lo < sorted->count && sorted->values[lo] == address;
}

/* The function at start: what its branches reach. BL targets, and the addresses its code takes (taken_address) outside
   the function, go to work; those it takes inside it, the base of a jump table for one, to inner, as leaders */
static Addresses explore(SnxCodegen* codegen, SnxU64 start, const Known* known, Addresses* work, Addresses* inner,
                          size_t limit) {
    Addresses result = {0}, todo = {0}, taken = {0};
    AddressSet seen = {0};
    push(&todo, start);
    if (!limit) limit = 200001;
    while (todo.count && seen.count < limit) {
        SnxU64 address = todo.values[--todo.count];
        for (;;) {
            SnxU32 word;
            SnxA64Insn insn;
            if (set_has(&seen, address) || (address != start && is_known(known, address))) break;
            if (!snx_codegen_word(codegen, address, &word)) break;
            set_add(&seen, address);
            push(&result, address);
            SnxA64Flow kind = decode(codegen, word, &insn);
            if (kind == SNX_A64_FLOW_UNDEF) break;
            if (kind == SNX_A64_FLOW_BC || kind == SNX_A64_FLOW_B || kind == SNX_A64_FLOW_BL) {
                SnxU64 target = snx_a64_branch_target(&insn, address);
                if (kind == SNX_A64_FLOW_BC) {
                    if (snx_codegen_module(codegen, target)) push(&todo, target);
                } else if (kind == SNX_A64_FLOW_B) {
                    SnxU64 distance = target > start ? target - start : start - target;
                    if (snx_codegen_module(codegen, target) && !is_known(known, target) && distance < 0x100000UL)
                        push(&todo, target);
                    break;
                } else if (work && snx_codegen_module(codegen, target))
                    push(work, target);
            } else if (kind == SNX_A64_FLOW_BR || kind == SNX_A64_FLOW_RET || kind == SNX_A64_FLOW_BRK)
                break;
            else if (work && inner && kind == SNX_A64_FLOW_PLAIN) {
                SnxU64 target = taken_address(codegen, address, word);
                if (target && !(target & 3) && snx_codegen_module(codegen, target)) push(&taken, target);
            }
            address += 4;
            if (seen.count >= limit) break;
        }
    }
    if (result.count) qsort(result.values, result.count, sizeof(*result.values), snx_compare_u64);
    for (size_t i = 0; i < taken.count; ++i)
        if (!sorted_has(&result, taken.values[i]))
            push(work, taken.values[i]);
        else if (taken.values[i] != start)
            push(inner, taken.values[i]);
    free(taken.values);
    free(todo.values);
    set_free(&seen);
    return result;
}

static void mark_leader(SnxFunction* function, SnxU64 address) {
    size_t index = function_index(function, address);
    if (index < function->count && !function->leaders[index]) {
        function->leaders[index] = 1;
        ++function->leader_count;
    }
}

static void leaders(SnxCodegen* codegen, const AddressSet* extra, const AddressSet* hints) {
    for (size_t i = 0; i < codegen->function_count; ++i) {
        SnxFunction* f = &codegen->functions[i];
        f->leaders = (unsigned char*)snx_alloc(f->count);
        memset(f->leaders, 0, f->count);
        mark_leader(f, f->start);
        int previous_terminal = 0;
        for (size_t j = 0; j < f->count; ++j) {
            SnxU32 word;
            SnxA64Insn insn;
            SnxU64 address = snx_function_address(f, j);
            if (previous_terminal || set_has(extra, address) || set_has(hints, address)) mark_leader(f, address);
            SnxA64Flow kind = snx_codegen_word(codegen, address, &word) ? decode(codegen, word, &insn)
                : SNX_A64_FLOW_UNDEF;
            if (kind == SNX_A64_FLOW_B || kind == SNX_A64_FLOW_BC)
                mark_leader(f, snx_a64_branch_target(&insn, address));
            if (kind == SNX_A64_FLOW_BL || kind == SNX_A64_FLOW_BLR) mark_leader(f, address + 4);
            previous_terminal = kind == SNX_A64_FLOW_B || kind == SNX_A64_FLOW_BR || kind == SNX_A64_FLOW_RET ||
                                kind == SNX_A64_FLOW_BRK || kind == SNX_A64_FLOW_UNDEF;
        }
        codegen->instruction_count += f->count;
    }
}

/* The instructions of the functions explored so far, a bit per word of each module's text after module_count offsets
   (module i's first bit): a seed found there later is a leader of that function, not a second function from its
   middle repeating the rest. Addresses taken (taken_address) land in the middle of functions explored before them */
static size_t* explored_new(const SnxCodegen* codegen) {
    size_t bits = 0;
    for (size_t i = 0; i < codegen->module_count; ++i) bits += codegen->modules[i].text_size / 4 + 1;
    size_t* explored = (size_t*)snx_alloc((codegen->module_count + 1) * sizeof(*explored) + bits / 8 + 1);
    bits = 0;
    for (size_t i = 0; i < codegen->module_count; ++i) {
        explored[i] = bits;
        bits += codegen->modules[i].text_size / 4 + 1;
    }
    memset(explored + codegen->module_count, 0, bits / 8 + 1);
    return explored;
}
/* the bit of address, a text address of some module */
static size_t explored_bit(SnxCodegen* codegen, const size_t* explored, SnxU64 address) {
    const SnxCodeImage* m = snx_codegen_module(codegen, address);
    return explored[m - codegen->modules] + (size_t)((address - (m->base + m->text_off)) / 4);
}
static int explored_has(SnxCodegen* codegen, const size_t* explored, SnxU64 address) {
    size_t bit = explored_bit(codegen, explored, address);
    return ((const unsigned char*)(explored + codegen->module_count))[bit / 8] >> (bit % 8) & 1;
}
static void explored_add(SnxCodegen* codegen, size_t* explored, const Addresses* found) {
    for (size_t i = 0; i < found->count; ++i) {
        size_t bit = explored_bit(codegen, explored, found->values[i]);
        ((unsigned char*)(explored + codegen->module_count))[bit / 8] |= (unsigned char)(1u << (bit % 8));
    }
}

int snx_codegen_analyze(SnxCodegen* codegen, const SnxCodeImage* modules, size_t module_count, const SnxU64* hints,
                        size_t hint_count, int quiet, SnxU64 features) {
    *codegen = (SnxCodegen){0};
    codegen->features = features;
    codegen->modules = (SnxCodeImage*)snx_alloc(module_count * sizeof(*modules));
    memcpy(codegen->modules, modules, module_count * sizeof(*modules));
    codegen->module_count = module_count;
    size_t* explored = explored_new(codegen);
    Addresses work = {0}, inner = {0};
    AddressSet known = {0}, seen_seed = {0}, extra = {0}, hint_set = {0}, seeds = {0}, fde_starts = {0};
    Known stop = {&known, NULL, NULL};
    Range* ranges = NULL;
    size_t nranges = 0, capranges = 0;
    for (size_t i = 0; i < module_count; ++i) {
        SnxCodeImage* m = &codegen->modules[i];
        SnxU64 text_end = m->base + m->text_off + m->text_size;
        for (size_t j = 0; j < m->fde_count; ++j) {
            SnxU64 a = m->base + m->fde[j].start;
            if (a < m->base + m->text_off || a >= text_end || !m->fde[j].size || (a & 3)) continue;
            set_add(&fde_starts, a);
            SnxU64 end = m->fde[j].size > text_end - a ? text_end : a + m->fde[j].size;
            if (nranges == capranges) {
                capranges = capranges ? capranges * 2 : 128;
                ranges = (Range*)snx_realloc(ranges, capranges * sizeof(*ranges));
            }
            ranges[nranges].start = a;
            ranges[nranges].end = end;
            ranges[nranges].ordinal = nranges;
            ++nranges;
        }
        for (size_t j = 0; j < m->function_count; ++j)
            if (set_add(&seeds, m->base + m->functions[j])) push(&work, m->base + m->functions[j]);
        for (size_t j = 0; j < m->relptr_count; ++j)
            if (set_add(&seeds, m->base + m->relptrs[j])) push(&work, m->base + m->relptrs[j]);
        if (set_add(&seeds, m->base + m->text_off)) push(&work, m->base + m->text_off);
    }
    for (size_t i = 0; i < hint_count; ++i) {
        set_add(&hint_set, hints[i]);
        if (snx_codegen_module(codegen, hints[i]) && set_add(&seeds, hints[i])) push(&work, hints[i]);
    }
    /* Symbols/hints at an FDE start do not become recursive seeds, including
       FDEs later discarded because an earlier range covers them. */
    size_t covered = 0;
    for (size_t i = 0; i < work.count; ++i)
        if (!set_has(&fde_starts, work.values[i])) work.values[covered++] = work.values[i];
    work.count = covered;
    if (nranges) qsort(ranges, nranges, sizeof(*ranges), compare_range);
    covered = 0;
    SnxU64 end = 0;
    for (size_t i = 0; i < nranges; ++i) {
        if (ranges[i].start < end) continue;
        ranges[covered++] = ranges[i];
        end = ranges[i].end;
        add_function(codegen, ranges[i].start, end, NULL);
        set_add(&known, ranges[i].start);
    }
    nranges = covered;
    while (work.count) {
        SnxU64 a = work.values[--work.count];
        if ((a & 3) || !set_add(&seen_seed, a)) continue;
        const Range* range = inside_range(ranges, nranges, a);
        if (range) {
            if (a != range->start) set_add(&extra, a);
            continue;
        }
        if (set_has(&known, a) || !snx_codegen_module(codegen, a)) continue;
        if (explored_has(codegen, explored, a)) {
            set_add(&extra, a);
            continue;
        }
        Addresses found = explore(codegen, a, &stop, &work, &inner, 0);
        if (found.count) {
            explored_add(codegen, explored, &found);
            add_function(codegen, a, 0, &found);
            set_add(&known, a);
        }
        free(found.values);
    }
    for (size_t i = 0; i < inner.count; ++i) set_add(&extra, inner.values[i]);
    free(inner.values);
    size_t pending_start = 0;
    size_t pending_end = codegen->function_count;
    for (size_t round = 0; round < 8 && pending_start < pending_end; ++round) {
        Addresses fresh = {0};
        for (size_t i = pending_start; i < pending_end; ++i) {
            SnxFunction* f = &codegen->functions[i];
            for (size_t j = 0; j < f->count; ++j) {
                SnxU32 word;
                SnxA64Insn insn;
                SnxU64 target;
                SnxU64 a = snx_function_address(f, j);
                if (!snx_codegen_word(codegen, a, &word)) continue;
                SnxA64Flow kind = decode(codegen, word, &insn);
                if (kind == SNX_A64_FLOW_PLAIN) { /* an address taken (taken_address): a call through it to come */
                    target = taken_address(codegen, a, word);
                    if (!target || (target & 3)) continue;
                    if (snx_function_contains(f, target)) {
                        if (target != f->start) set_add(&extra, target);
                        continue;
                    }
                    kind = SNX_A64_FLOW_BL;
                } else if (kind != SNX_A64_FLOW_BL && kind != SNX_A64_FLOW_B && kind != SNX_A64_FLOW_BC)
                    continue;
                else
                    target = snx_a64_branch_target(&insn, a);
                if (set_has(&known, target) || set_has(&seen_seed, target) || !snx_codegen_module(codegen, target))
                    continue;
                if (kind != SNX_A64_FLOW_BL && snx_function_contains(f, target)) continue;
                const Range* range = inside_range(ranges, nranges, target);
                if (range) {
                    if (kind == SNX_A64_FLOW_BL && target != range->start) set_add(&extra, target);
                    continue;
                }
                if (kind == SNX_A64_FLOW_BL || target < f->start || target > snx_function_address(f, f->count - 1))
                    push(&fresh, target);
            }
        }
        size_t next_start = codegen->function_count;
        for (size_t i = 0; i < fresh.count; ++i) {
            SnxU64 a = fresh.values[i];
            if (set_has(&known, a) || !set_add(&seen_seed, a)) continue;
            if (explored_has(codegen, explored, a)) {
                set_add(&extra, a);
                continue;
            }
            Addresses found = explore(codegen, a, &stop, NULL, NULL, 0);
            if (found.count) {
                explored_add(codegen, explored, &found);
                add_function(codegen, a, 0, &found);
                set_add(&known, a);
            }
            free(found.values);
        }
        free(fresh.values);
        pending_start = next_start;
        pending_end = codegen->function_count;
    }
    if (codegen->function_count)
        qsort(codegen->functions, codegen->function_count, sizeof(*codegen->functions), compare_function);
    leaders(codegen, &extra, &hint_set);
    if (!quiet)
        printf("%lu functions, %lu instructions\n", (unsigned long)codegen->function_count,
               (unsigned long)codegen->instruction_count);
    free(work.values);
    free(ranges);
    free(explored);
    set_free(&known);
    set_free(&seen_seed);
    set_free(&extra);
    set_free(&hint_set);
    set_free(&seeds);
    set_free(&fde_starts);
    return 0;
}

int snx_codegen_discover(SnxCodegen* codegen, const SnxCodeImage* modules, size_t module_count, const SnxU64* roots,
                         size_t root_count, SnxCodegenKnown known, void* user, size_t budget, SnxU64 features,
                         size_t region_limit, size_t overflow_limit) {
    *codegen = (SnxCodegen){0};
    codegen->features = features;
    codegen->modules = (SnxCodeImage*)snx_alloc((module_count ? module_count : 1) * sizeof(*modules));
    memcpy(codegen->modules, modules, module_count * sizeof(*modules));
    codegen->module_count = module_count;
    Addresses work = {0}, inner = {0};
    AddressSet local = {0}, seen_seed = {0}, entries = {0}, inside = {0}, none = {0};
    Known stop = {&local, known, user};
    /* The roots first, by address, then the functions they call in the order they turn up, nearest first: a call left
       out once the budget is spent goes through CPU.disp and misses on its own. A root inside a function found before
       it (a pc in the middle of a function that missed on its own, where a thread came back from other code) is a
       leader of that function, not a function of its own, so the function stays whole */
    for (size_t i = 0; i < root_count; ++i)
        if (!(roots[i] & 3) && snx_codegen_module(codegen, roots[i])) {
            push(&work, roots[i]);
            set_add(&entries, roots[i]);
        }
    size_t searched = work.count; /* the roots in work: searched whatever the budget and whatever known says */
    if (searched) qsort(work.values, searched, sizeof(*work.values), snx_compare_u64);
    size_t total = 0;
    for (size_t next = 0; next < work.count; ++next) {
        SnxU64 a = work.values[next];
        if ((a & 3) || !set_add(&seen_seed, a)) continue;
        if (next < searched) {
            if (set_has(&inside, a)) continue;
        } else {
            if (total >= budget) break;
            if (is_known(&stop, a) || !snx_codegen_module(codegen, a)) continue;
        }
        size_t pending = work.count, interiors = inner.count;
        Addresses found = explore(codegen, a, &stop, &work, &inner,
                                   region_limit + (overflow_limit && region_limit < SIZE_MAX));
        if (overflow_limit && found.count > region_limit) {
            /* Exploration only appends to these lists. Discard the oversized graph's speculative work before
               finding its first bounded region; its external branches retain the normal dispatch semantics. */
            free(found.values);
            work.count = pending;
            inner.count = interiors;
            found = explore(codegen, a, &stop, &work, &inner, overflow_limit);
        }
        if (found.count) {
            total += found.count;
            for (size_t i = 0; i < found.count; ++i) set_add(&inside, found.values[i]);
            add_function(codegen, a, 0, &found);
            set_add(&local, a);
        }
        free(found.values);
    }
    if (codegen->function_count)
        qsort(codegen->functions, codegen->function_count, sizeof(*codegen->functions), compare_function);
    for (size_t i = 0; i < inner.count; ++i) set_add(&entries, inner.values[i]); /* leaders, not roots */
    free(inner.values);
    leaders(codegen, &entries, &none);
    free(work.values);
    set_free(&local);
    set_free(&seen_seed);
    set_free(&entries);
    set_free(&inside);
    for (size_t i = 0; i < root_count; ++i)
        if (snx_codegen_entry(codegen, roots[i])) return 0;
    return -1;
}

const SnxFunction* snx_codegen_entry(const SnxCodegen* codegen, SnxU64 address) {
    const SnxFunction* f = snx_codegen_function(codegen, address);
    for (size_t i = 0; !f && i < codegen->function_count; ++i) {
        size_t index = function_index(&codegen->functions[i], address);
        if (index < codegen->functions[i].count && codegen->functions[i].leaders[index]) f = &codegen->functions[i];
    }
    return f;
}

/* instructions of one function of a just-in-time step: the backends' time grows faster than a function's size, and a
   program's largest (a compiler's generated recognizers) would take minutes */
#define SNX_JIT_FUNCTION_INSTRUCTIONS 4096

/* A just-in-time compiler's discovery: functions from the hints only, and then the functions they call, breadth
   first (the callees closest to where execution is come first), until the functions hold limit instructions. A
   target compiled already (skip) is not explored again: the call to it goes through the dispatch table */
int snx_codegen_analyze_from(SnxCodegen* codegen, const SnxCodeImage* modules, size_t module_count, const SnxU64* hints,
                             size_t hint_count, size_t limit, int (*skip)(void* context, SnxU64 address),
                             void* context, SnxU64 features) {
    *codegen = (SnxCodegen){0};
    codegen->features = features;
    codegen->modules = (SnxCodeImage*)snx_alloc(module_count * sizeof(*modules));
    memcpy(codegen->modules, modules, module_count * sizeof(*modules));
    codegen->module_count = module_count;
    Addresses work = {0};
    AddressSet known = {0}, seen = {0}, extra = {0}, hint_set = {0};
    Known stop = {&known, NULL, NULL};
    for (size_t i = 0; i < hint_count; ++i) {
        set_add(&hint_set, hints[i]);
        push(&work, hints[i]);
    }
    size_t total = 0;
    for (size_t next = 0; next < work.count && total < limit; ++next) {
        SnxU64 a = work.values[next];
        if ((a & 3) || !set_add(&seen, a) || set_has(&known, a) || !snx_codegen_module(codegen, a)) continue;
        if (next >= hint_count && skip && skip(context, a)) continue;
        Addresses calls = {0};
        Addresses found = explore(codegen, a, &stop, &calls, NULL, 0);
        if (found.count) {
            total += found.count;
            set_add(&known, a);
            if (found.count <= SNX_JIT_FUNCTION_INSTRUCTIONS)
                add_function(codegen, a, 0, &found);
            else
                /* pieces of consecutive instructions: a branch from one into another goes through the dispatch table
                   (a leader there, below), and a piece's last instruction runs on into the next piece's first */
                for (size_t i = 0; i < found.count; i += SNX_JIT_FUNCTION_INSTRUCTIONS) {
                    Addresses piece = {0};
                    size_t n = found.count - i < SNX_JIT_FUNCTION_INSTRUCTIONS ? found.count - i
                                                                               : SNX_JIT_FUNCTION_INSTRUCTIONS;
                    for (size_t j = 0; j < n; ++j) push(&piece, found.values[i + j]);
                    set_add(&known, piece.values[0]);
                    add_function(codegen, piece.values[0], 0, &piece);
                }
        }
        for (size_t i = 0; i < calls.count; ++i) push(&work, calls.values[i]);
        free(calls.values);
        free(found.values);
    }
    if (codegen->function_count)
        qsort(codegen->functions, codegen->function_count, sizeof(*codegen->functions), compare_function);
    /* a branch into another function (a piece of the same one) is an entry of that function */
    for (size_t i = 0; i < codegen->function_count; ++i) {
        const SnxFunction* f = &codegen->functions[i];
        for (size_t j = 0; j < f->count; ++j) {
            SnxU32 word;
            SnxA64Insn insn;
            SnxU64 a = snx_function_address(f, j);
            if (!snx_codegen_word(codegen, a, &word)) continue;
            SnxA64Flow kind = decode(codegen, word, &insn);
            if ((kind == SNX_A64_FLOW_B || kind == SNX_A64_FLOW_BC) &&
                !snx_function_contains(f, snx_a64_branch_target(&insn, a)))
                set_add(&extra, snx_a64_branch_target(&insn, a));
        }
    }
    leaders(codegen, &extra, &hint_set);
    free(work.values);
    set_free(&known);
    set_free(&seen);
    set_free(&extra);
    set_free(&hint_set);
    return codegen->function_count ? 0 : -1;
}

void snx_codegen_free(SnxCodegen* codegen) {
    for (size_t i = 0; i < codegen->function_count; ++i) {
        free(codegen->functions[i].addresses);
        free(codegen->functions[i].leaders);
    }
    free(codegen->functions);
    free(codegen->modules);
    *codegen = (SnxCodegen){0};
}
