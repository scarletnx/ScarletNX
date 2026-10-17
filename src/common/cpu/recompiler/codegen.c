/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Derived from SwitchRecomp codegen.py, Copyright 2026 MZ Labs, Apache-2.0.
   Retain LICENSE.apache and NOTICE; the fork's changes are GPL-3.0-or-later. */
#include "common/cpu/recompiler/codegen.h"
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
    size_t i;
    if (!set->capacity) return 0;
    i = address_hash(value) & (set->capacity - 1);
    while (set->used[i]) {
        if (set->keys[i] == value) return 1;
        i = (i + 1) & (set->capacity - 1);
    }
    return 0;
}

static int set_add(AddressSet* set, SnxU64 value) {
    AddressSet old;
    size_t i;
    if (!set->capacity || (set->count + 1) * 4 >= set->capacity * 3) {
        old = *set;
        set->capacity = old.capacity ? old.capacity * 2 : 128;
        set->count = 0;
        set->keys = (SnxU64*)snx_alloc(set->capacity * sizeof(*set->keys));
        set->used = (unsigned char*)snx_alloc(set->capacity);
        memset(set->used, 0, set->capacity);
        for (i = 0; i < old.capacity; ++i)
            if (old.used[i]) set_add(set, old.keys[i]);
        free(old.keys);
        free(old.used);
    }
    i = address_hash(value) & (set->capacity - 1);
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
    memset(set, 0, sizeof(*set));
}
static int compare_address(const void* a, const void* b) {
    SnxU64 x, y;
    x = *(const SnxU64*)a;
    y = *(const SnxU64*)b;
    return x < y ? -1 : x > y;
}
static int compare_function(const void* a, const void* b) {
    const SnxFunction *x, *y;
    x = (const SnxFunction*)a;
    y = (const SnxFunction*)b;
    return x->start < y->start ? -1 : x->start > y->start;
}
static int compare_range(const void* a, const void* b) {
    const Range *x, *y;
    x = (const Range*)a;
    y = (const Range*)b;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    return x->ordinal > y->ordinal ? -1 : x->ordinal < y->ordinal;
}

SnxCodeImage* snx_codegen_module(SnxCodegen* codegen, SnxU64 address) {
    size_t i;
    SnxCodeImage* module;
    SnxU64 start;
    module = codegen->last_module;
    if (module) {
        start = module->base + module->text_off;
        if (address >= start && address - start < module->text_size) return module;
    }
    for (i = 0; i < codegen->module_count; ++i) {
        module = &codegen->modules[i];
        start = module->base + module->text_off;
        if (address >= start && address - start < module->text_size) {
            codegen->last_module = module;
            return module;
        }
    }
    return NULL;
}

int snx_codegen_word(SnxCodegen* codegen, SnxU64 address, SnxU32* word) {
    SnxCodeImage* module;
    size_t off;
    const unsigned char* p;
    module = snx_codegen_module(codegen, address);
    if (!module || (address & 3)) return 0;
    off = (size_t)(address - module->base);
    if (off > module->image_size || module->image_size - off < 4 ||
        address - (module->base + module->text_off) + 4 > module->text_size)
        return 0;
    p = module->image + off;
    *word = (SnxU32)p[0] | ((SnxU32)p[1] << 8) | ((SnxU32)p[2] << 16) | ((SnxU32)p[3] << 24);
    return 1;
}

SnxU64 snx_function_address(const SnxFunction* function, size_t index) {
    return function->addresses ? function->addresses[index] : function->start + (SnxU64)index * 4;
}

static size_t function_index(const SnxFunction* function, SnxU64 address) {
    size_t lo, hi, middle;
    if (!function->addresses) {
        if (address < function->start || address >= function->end || ((address - function->start) & 3))
            return function->count;
        return (size_t)((address - function->start) / 4);
    }
    lo = 0;
    hi = function->count;
    while (lo < hi) {
        middle = lo + (hi - lo) / 2;
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
    size_t lo, hi, middle;
    lo = 0;
    hi = codegen->function_count;
    while (lo < hi) {
        middle = lo + (hi - lo) / 2;
        if (codegen->functions[middle].start < address)
            lo = middle + 1;
        else
            hi = middle;
    }
    return lo < codegen->function_count && codegen->functions[lo].start == address ? &codegen->functions[lo] : NULL;
}

static const Range* inside_range(const Range* ranges, size_t count, SnxU64 address) {
    size_t lo, hi, middle;
    lo = 0;
    hi = count;
    while (lo < hi) {
        middle = lo + (hi - lo) / 2;
        if (ranges[middle].start <= address)
            lo = middle + 1;
        else
            hi = middle;
    }
    return lo && address < ranges[lo - 1].end ? &ranges[lo - 1] : NULL;
}

static void add_function(SnxCodegen* codegen, SnxU64 start, SnxU64 end, Addresses* addresses) {
    SnxFunction* f;
    if (codegen->function_count == codegen->function_capacity) {
        codegen->function_capacity = codegen->function_capacity ? codegen->function_capacity * 2 : 128;
        codegen->functions =
            (SnxFunction*)snx_realloc(codegen->functions, codegen->function_capacity * sizeof(*codegen->functions));
    }
    f = &codegen->functions[codegen->function_count++];
    memset(f, 0, sizeof(*f));
    f->start = start;
    f->end = end;
    if (addresses) {
        f->addresses = addresses->values;
        f->count = addresses->count;
        memset(addresses, 0, sizeof(*addresses));
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

/* whether a BR on reg follows within a few instructions of address: the address taken there is a jump table's base,
   a place in the function its cases are reached from, not a function */
static int jump_table_base(SnxCodegen* codegen, SnxU64 address, SnxU32 reg) {
    SnxU32 after;
    unsigned i;
    for (i = 1; i <= 6; ++i) {
        if (!snx_codegen_word(codegen, address + (SnxU64)i * 4, &after)) return 0;
        if ((after & 0xfffffc1fUL) == 0xd61f0000UL) return ((after >> 5) & 31) == reg; /* BR */
        if (snx_a64_decode(after)->kind != SNX_FLOW_PLAIN) return 0;
    }
    return 0;
}

/* The address the instruction at address puts in a register, 0 for none and for a jump table's base: ADR's, or an
   ADD's of the page an ADRP a few instructions before put in its source (the linker relaxes such a pair into a NOP and
   an ADR when the target is near). Code takes function pointers this way, for a callback or a call through a
   register. Ender Magnolia's main, which has unwind info for a seventh of its text, reaches four fifths of the
   functions it misses only through them */
static SnxU64 taken_address(SnxCodegen* codegen, SnxU64 address, SnxU32 word) {
    SnxU64 imm;
    SnxU32 before, reg;
    unsigned i;
    if ((word & 0x9f000000UL) == 0x10000000UL) { /* ADR */
        if (jump_table_base(codegen, address, word & 31)) return 0;
        imm = ((SnxU64)((word >> 5) & 0x7ffffUL) << 2) | ((word >> 29) & 3);
        return address + (imm & 0x100000UL ? imm | ~(SnxU64)0x1fffffUL : imm);
    }
    if ((word & 0xff800000UL) != 0x91000000UL || jump_table_base(codegen, address, word & 31))
        return 0; /* ADD Xd, Xn, #imm12 */
    reg = (word >> 5) & 31;
    for (i = 1; i <= 8 && address >= (SnxU64)i * 4; ++i) {
        if (!snx_codegen_word(codegen, address - (SnxU64)i * 4, &before)) return 0;
        if ((before & 0x9f000000UL) == 0x90000000UL && (before & 31) == reg) { /* ADRP */
            imm = ((SnxU64)((before >> 5) & 0x7ffffUL) << 2) | ((before >> 29) & 3);
            if (imm & 0x100000UL) imm |= ~(SnxU64)0x1fffffUL;
            return ((address - (SnxU64)i * 4) & ~(SnxU64)0xfff) + (imm << 12) +
                   ((SnxU64)((word >> 10) & 0xfff) << ((word & (1UL << 22)) ? 12 : 0));
        }
        /* the register written in between (or perhaps only stored: bits 0-4 are Rt too), or a way in from elsewhere */
        if ((before & 31) == reg || snx_a64_decode(before)->kind != SNX_FLOW_PLAIN) return 0;
    }
    return 0;
}

static int sorted_has(const Addresses* sorted, SnxU64 address) {
    size_t lo = 0, hi = sorted->count, middle;
    while (lo < hi) {
        middle = lo + (hi - lo) / 2;
        if (sorted->values[middle] < address)
            lo = middle + 1;
        else
            hi = middle;
    }
    return lo < sorted->count && sorted->values[lo] == address;
}

/* The function at start: what its branches reach. BL targets, and the addresses its code takes (taken_address) outside
   the function, go to work; those it takes inside it, the base of a jump table for one, to inner, as leaders */
static Addresses explore(SnxCodegen* codegen, SnxU64 start, const Known* known, Addresses* work, Addresses* inner) {
    Addresses result, todo, taken;
    AddressSet seen;
    SnxU64 address, target, distance;
    SnxU32 word;
    enum SnxFlow kind;
    size_t i;
    memset(&result, 0, sizeof(result));
    memset(&todo, 0, sizeof(todo));
    memset(&taken, 0, sizeof(taken));
    memset(&seen, 0, sizeof(seen));
    push(&todo, start);
    while (todo.count && seen.count <= 200000) {
        address = todo.values[--todo.count];
        for (;;) {
            if (set_has(&seen, address) || (address != start && is_known(known, address))) break;
            if (!snx_codegen_word(codegen, address, &word)) break;
            set_add(&seen, address);
            push(&result, address);
            kind = snx_a64_decode(word)->kind;
            if (kind == SNX_FLOW_UNDEF) break;
            if (kind == SNX_FLOW_BC || kind == SNX_FLOW_B || kind == SNX_FLOW_BL) {
                target = snx_a64_target(word, address);
                if (kind == SNX_FLOW_BC) {
                    if (snx_codegen_module(codegen, target)) push(&todo, target);
                } else if (kind == SNX_FLOW_B) {
                    distance = target > start ? target - start : start - target;
                    if (snx_codegen_module(codegen, target) && !is_known(known, target) && distance < 0x100000UL)
                        push(&todo, target);
                    break;
                } else if (work && snx_codegen_module(codegen, target))
                    push(work, target);
            } else if (kind == SNX_FLOW_BR || kind == SNX_FLOW_RET || kind == SNX_FLOW_BRK)
                break;
            else if (work && inner && kind == SNX_FLOW_PLAIN) {
                target = taken_address(codegen, address, word);
                if (target && !(target & 3) && snx_codegen_module(codegen, target)) push(&taken, target);
            }
            address += 4;
            if (seen.count > 200000) break;
        }
    }
    if (result.count) qsort(result.values, result.count, sizeof(*result.values), compare_address);
    for (i = 0; i < taken.count; ++i)
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
    size_t index;
    index = function_index(function, address);
    if (index < function->count && !function->leaders[index]) {
        function->leaders[index] = 1;
        ++function->leader_count;
    }
}

static void leaders(SnxCodegen* codegen, const AddressSet* extra, const AddressSet* hints) {
    size_t i, j;
    SnxFunction* f;
    SnxU64 address;
    SnxU32 word;
    enum SnxFlow kind;
    int previous_terminal;
    for (i = 0; i < codegen->function_count; ++i) {
        f = &codegen->functions[i];
        f->leaders = (unsigned char*)snx_alloc(f->count);
        memset(f->leaders, 0, f->count);
        mark_leader(f, f->start);
        previous_terminal = 0;
        for (j = 0; j < f->count; ++j) {
            address = snx_function_address(f, j);
            if (previous_terminal || set_has(extra, address) || set_has(hints, address)) mark_leader(f, address);
            kind = snx_codegen_word(codegen, address, &word) ? snx_a64_decode(word)->kind : SNX_FLOW_UNDEF;
            if (kind == SNX_FLOW_B || kind == SNX_FLOW_BC) mark_leader(f, snx_a64_target(word, address));
            if (kind == SNX_FLOW_BL || kind == SNX_FLOW_BLR) mark_leader(f, address + 4);
            previous_terminal = kind == SNX_FLOW_B || kind == SNX_FLOW_BR || kind == SNX_FLOW_RET ||
                                kind == SNX_FLOW_BRK || kind == SNX_FLOW_UNDEF;
        }
        codegen->instruction_count += f->count;
    }
}

/* The instructions of the functions explored so far, a bit per word of each module's text after module_count offsets
   (module i's first bit): a seed found there later is a leader of that function, not a second function from its
   middle repeating the rest. Addresses taken (taken_address) land in the middle of functions explored before them */
static size_t* explored_new(const SnxCodegen* codegen) {
    size_t i, bits = 0, *explored;
    for (i = 0; i < codegen->module_count; ++i) bits += codegen->modules[i].text_size / 4 + 1;
    explored = (size_t*)snx_alloc((codegen->module_count + 1) * sizeof(*explored) + bits / 8 + 1);
    for (i = 0, bits = 0; i < codegen->module_count; ++i) {
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
    size_t i, bit;
    for (i = 0; i < found->count; ++i) {
        bit = explored_bit(codegen, explored, found->values[i]);
        ((unsigned char*)(explored + codegen->module_count))[bit / 8] |= (unsigned char)(1u << (bit % 8));
    }
}

int snx_codegen_analyze(SnxCodegen* codegen, const SnxCodeImage* modules, size_t module_count, const SnxU64* hints,
                        size_t hint_count, int quiet) {
    Range* ranges;
    const Range* range;
    size_t nranges, capranges, i, j, covered, round, pending_start, pending_end, next_start;
    Addresses work, found, fresh, inner;
    AddressSet known, seen_seed, extra, hint_set, seeds, fde_starts;
    size_t* explored;
    Known stop;
    SnxCodeImage* m;
    SnxU64 a, end, text_end, target;
    SnxU32 word;
    enum SnxFlow kind;
    SnxFunction* f;
    memset(codegen, 0, sizeof(*codegen));
    codegen->modules = (SnxCodeImage*)snx_alloc(module_count * sizeof(*modules));
    memcpy(codegen->modules, modules, module_count * sizeof(*modules));
    codegen->module_count = module_count;
    explored = explored_new(codegen);
    memset(&work, 0, sizeof(work));
    memset(&inner, 0, sizeof(inner));
    memset(&known, 0, sizeof(known));
    memset(&seen_seed, 0, sizeof(seen_seed));
    memset(&extra, 0, sizeof(extra));
    memset(&hint_set, 0, sizeof(hint_set));
    memset(&seeds, 0, sizeof(seeds));
    memset(&fde_starts, 0, sizeof(fde_starts));
    stop.set = &known;
    stop.callback = NULL;
    stop.user = NULL;
    ranges = NULL;
    nranges = 0;
    capranges = 0;
    for (i = 0; i < module_count; ++i) {
        m = &codegen->modules[i];
        text_end = m->base + m->text_off + m->text_size;
        for (j = 0; j < m->fde_count; ++j) {
            a = m->base + m->fde[j].start;
            if (a < m->base + m->text_off || a >= text_end || !m->fde[j].size || (a & 3)) continue;
            set_add(&fde_starts, a);
            end = m->fde[j].size > text_end - a ? text_end : a + m->fde[j].size;
            if (nranges == capranges) {
                capranges = capranges ? capranges * 2 : 128;
                ranges = (Range*)snx_realloc(ranges, capranges * sizeof(*ranges));
            }
            ranges[nranges].start = a;
            ranges[nranges].end = end;
            ranges[nranges].ordinal = nranges;
            ++nranges;
        }
        for (j = 0; j < m->function_count; ++j)
            if (set_add(&seeds, m->base + m->functions[j])) push(&work, m->base + m->functions[j]);
        for (j = 0; j < m->relptr_count; ++j)
            if (set_add(&seeds, m->base + m->relptrs[j])) push(&work, m->base + m->relptrs[j]);
        if (set_add(&seeds, m->base + m->text_off)) push(&work, m->base + m->text_off);
    }
    for (i = 0; i < hint_count; ++i) {
        set_add(&hint_set, hints[i]);
        if (snx_codegen_module(codegen, hints[i]) && set_add(&seeds, hints[i])) push(&work, hints[i]);
    }
    /* Symbols/hints at an FDE start do not become recursive seeds, including
       FDEs later discarded because an earlier range covers them. */
    covered = 0;
    for (i = 0; i < work.count; ++i)
        if (!set_has(&fde_starts, work.values[i])) work.values[covered++] = work.values[i];
    work.count = covered;
    if (nranges) qsort(ranges, nranges, sizeof(*ranges), compare_range);
    covered = 0;
    end = 0;
    for (i = 0; i < nranges; ++i) {
        if (ranges[i].start < end) continue;
        ranges[covered++] = ranges[i];
        end = ranges[i].end;
        add_function(codegen, ranges[i].start, end, NULL);
        set_add(&known, ranges[i].start);
    }
    nranges = covered;
    while (work.count) {
        a = work.values[--work.count];
        if ((a & 3) || !set_add(&seen_seed, a)) continue;
        range = inside_range(ranges, nranges, a);
        if (range) {
            if (a != range->start) set_add(&extra, a);
            continue;
        }
        if (set_has(&known, a) || !snx_codegen_module(codegen, a)) continue;
        if (explored_has(codegen, explored, a)) {
            set_add(&extra, a);
            continue;
        }
        found = explore(codegen, a, &stop, &work, &inner);
        if (found.count) {
            explored_add(codegen, explored, &found);
            add_function(codegen, a, 0, &found);
            set_add(&known, a);
        }
        free(found.values);
    }
    for (i = 0; i < inner.count; ++i) set_add(&extra, inner.values[i]);
    free(inner.values);
    pending_start = 0;
    pending_end = codegen->function_count;
    for (round = 0; round < 8 && pending_start < pending_end; ++round) {
        memset(&fresh, 0, sizeof(fresh));
        for (i = pending_start; i < pending_end; ++i) {
            f = &codegen->functions[i];
            for (j = 0; j < f->count; ++j) {
                a = snx_function_address(f, j);
                if (!snx_codegen_word(codegen, a, &word)) continue;
                kind = snx_a64_decode(word)->kind;
                if (kind == SNX_FLOW_PLAIN) { /* an address taken (taken_address): a call through it to come */
                    target = taken_address(codegen, a, word);
                    if (!target || (target & 3)) continue;
                    if (snx_function_contains(f, target)) {
                        if (target != f->start) set_add(&extra, target);
                        continue;
                    }
                    kind = SNX_FLOW_BL;
                } else if (kind != SNX_FLOW_BL && kind != SNX_FLOW_B && kind != SNX_FLOW_BC)
                    continue;
                else
                    target = snx_a64_target(word, a);
                if (set_has(&known, target) || set_has(&seen_seed, target) || !snx_codegen_module(codegen, target))
                    continue;
                if (kind != SNX_FLOW_BL && snx_function_contains(f, target)) continue;
                range = inside_range(ranges, nranges, target);
                if (range) {
                    if (kind == SNX_FLOW_BL && target != range->start) set_add(&extra, target);
                    continue;
                }
                if (kind == SNX_FLOW_BL || target < f->start || target > snx_function_address(f, f->count - 1))
                    push(&fresh, target);
            }
        }
        next_start = codegen->function_count;
        for (i = 0; i < fresh.count; ++i) {
            a = fresh.values[i];
            if (set_has(&known, a) || !set_add(&seen_seed, a)) continue;
            if (explored_has(codegen, explored, a)) {
                set_add(&extra, a);
                continue;
            }
            found = explore(codegen, a, &stop, NULL, NULL);
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
        printf("Functions: %lu  instructions: %lu\n", (unsigned long)codegen->function_count,
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
                         size_t root_count, SnxCodegenKnown known, void* user, size_t budget) {
    Addresses work, found, inner;
    AddressSet local, seen_seed, entries, inside, none;
    Known stop;
    size_t next, total, i, searched;
    SnxU64 a;
    memset(codegen, 0, sizeof(*codegen));
    codegen->modules = (SnxCodeImage*)snx_alloc((module_count ? module_count : 1) * sizeof(*modules));
    memcpy(codegen->modules, modules, module_count * sizeof(*modules));
    codegen->module_count = module_count;
    memset(&work, 0, sizeof(work));
    memset(&inner, 0, sizeof(inner));
    memset(&local, 0, sizeof(local));
    memset(&seen_seed, 0, sizeof(seen_seed));
    memset(&entries, 0, sizeof(entries));
    memset(&inside, 0, sizeof(inside));
    memset(&none, 0, sizeof(none));
    stop.set = &local;
    stop.callback = known;
    stop.user = user;
    /* The roots first, by address, then the functions they call in the order they turn up, nearest first: a call left
       out once the budget is spent goes through CPU.disp and misses on its own. A root inside a function found before
       it (a pc in the middle of a function that missed on its own, where a thread came back from other code) is a
       leader of that function, not a function of its own, so the function stays whole */
    for (i = 0; i < root_count; ++i)
        if (!(roots[i] & 3) && snx_codegen_module(codegen, roots[i])) {
            push(&work, roots[i]);
            set_add(&entries, roots[i]);
        }
    searched = work.count; /* the roots in work: searched whatever the budget and whatever known says */
    if (searched) qsort(work.values, searched, sizeof(*work.values), compare_address);
    total = 0;
    for (next = 0; next < work.count; ++next) {
        a = work.values[next];
        if ((a & 3) || !set_add(&seen_seed, a)) continue;
        if (next < searched) {
            if (set_has(&inside, a)) continue;
        } else {
            if (total >= budget) break;
            if (is_known(&stop, a) || !snx_codegen_module(codegen, a)) continue;
        }
        found = explore(codegen, a, &stop, &work, &inner);
        if (found.count) {
            total += found.count;
            for (i = 0; i < found.count; ++i) set_add(&inside, found.values[i]);
            add_function(codegen, a, 0, &found);
            set_add(&local, a);
        }
        free(found.values);
    }
    if (codegen->function_count)
        qsort(codegen->functions, codegen->function_count, sizeof(*codegen->functions), compare_function);
    for (i = 0; i < inner.count; ++i) set_add(&entries, inner.values[i]); /* leaders, not roots */
    free(inner.values);
    leaders(codegen, &entries, &none);
    free(work.values);
    set_free(&local);
    set_free(&seen_seed);
    set_free(&entries);
    set_free(&inside);
    for (i = 0; i < root_count; ++i)
        if (snx_codegen_entry(codegen, roots[i])) return 0;
    return -1;
}

const SnxFunction* snx_codegen_entry(const SnxCodegen* codegen, SnxU64 address) {
    const SnxFunction* f = snx_codegen_function(codegen, address);
    size_t i, index;
    for (i = 0; !f && i < codegen->function_count; ++i) {
        index = function_index(&codegen->functions[i], address);
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
                             void* context) {
    Addresses work, found, calls;
    AddressSet known, seen, extra, hint_set;
    Known stop;
    size_t i, j, next, total;
    SnxU64 a;
    memset(codegen, 0, sizeof(*codegen));
    codegen->modules = (SnxCodeImage*)snx_alloc(module_count * sizeof(*modules));
    memcpy(codegen->modules, modules, module_count * sizeof(*modules));
    codegen->module_count = module_count;
    memset(&work, 0, sizeof(work));
    memset(&known, 0, sizeof(known));
    memset(&seen, 0, sizeof(seen));
    memset(&extra, 0, sizeof(extra));
    memset(&hint_set, 0, sizeof(hint_set));
    stop.set = &known;
    stop.callback = NULL;
    stop.user = NULL;
    for (i = 0; i < hint_count; ++i) {
        set_add(&hint_set, hints[i]);
        push(&work, hints[i]);
    }
    total = 0;
    for (next = 0; next < work.count && total < limit; ++next) {
        a = work.values[next];
        if ((a & 3) || !set_add(&seen, a) || set_has(&known, a) || !snx_codegen_module(codegen, a)) continue;
        if (next >= hint_count && skip && skip(context, a)) continue;
        memset(&calls, 0, sizeof(calls));
        found = explore(codegen, a, &stop, &calls, NULL);
        if (found.count) {
            total += found.count;
            set_add(&known, a);
            if (found.count <= SNX_JIT_FUNCTION_INSTRUCTIONS)
                add_function(codegen, a, 0, &found);
            else
                /* pieces of consecutive instructions: a branch from one into another goes through the dispatch table
                   (a leader there, below), and a piece's last instruction runs on into the next piece's first */
                for (i = 0; i < found.count; i += SNX_JIT_FUNCTION_INSTRUCTIONS) {
                    Addresses piece;
                    size_t n = found.count - i < SNX_JIT_FUNCTION_INSTRUCTIONS ? found.count - i
                                                                               : SNX_JIT_FUNCTION_INSTRUCTIONS;
                    memset(&piece, 0, sizeof(piece));
                    for (j = 0; j < n; ++j) push(&piece, found.values[i + j]);
                    set_add(&known, piece.values[0]);
                    add_function(codegen, piece.values[0], 0, &piece);
                }
        }
        for (i = 0; i < calls.count; ++i) push(&work, calls.values[i]);
        free(calls.values);
        free(found.values);
    }
    if (codegen->function_count)
        qsort(codegen->functions, codegen->function_count, sizeof(*codegen->functions), compare_function);
    /* a branch into another function (a piece of the same one) is an entry of that function */
    for (i = 0; i < codegen->function_count; ++i) {
        const SnxFunction* f = &codegen->functions[i];
        for (j = 0; j < f->count; ++j) {
            SnxU32 word;
            enum SnxFlow kind;
            a = snx_function_address(f, j);
            if (!snx_codegen_word(codegen, a, &word)) continue;
            kind = snx_a64_decode(word)->kind;
            if ((kind == SNX_FLOW_B || kind == SNX_FLOW_BC) && !snx_function_contains(f, snx_a64_target(word, a)))
                set_add(&extra, snx_a64_target(word, a));
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
    size_t i;
    for (i = 0; i < codegen->function_count; ++i) {
        free(codegen->functions[i].addresses);
        free(codegen->functions[i].leaders);
    }
    free(codegen->functions);
    free(codegen->modules);
    memset(codegen, 0, sizeof(*codegen));
}

static void function_name(FILE* out, SnxU64 address) {
    fputs("F_", out);
    snx_a64_hex(out, address);
}
static void label(FILE* out, SnxU64 address) {
    fputs("L_", out);
    snx_a64_hex(out, address);
}

static void call(FILE* out, const SnxCodegen* codegen, SnxU64 target, int tail) {
    const SnxFunction* function;
    function = snx_codegen_function(codegen, target);
    if (tail) fputs("{ ", out);
    /* --hook-direct-calls: through CPU.disp even to a known function, where runtime wrappers can see the call */
    if (function && !codegen->hook_direct_calls) {
        function_name(out, target);
        fputs("(c,", out);
    } else
        fputs("CALLDYN(", out);
    snx_a64_literal(out, target);
    fputs(");", out);
    if (tail) fputs(" return; }", out);
}

/* a backward branch is how a loop goes round: there a thread that makes no SVC can still be taken off its core */
static void poll(FILE* out, SnxU64 resume) {
    fputs("if(UNLIKELY(__atomic_load_n(&c->poll,__ATOMIC_RELAXED))){ c->pc=", out);
    snx_a64_literal(out, resume);
    fputs("; c->rt->poll(c); } ", out);
}

static void branch(FILE* out, const SnxCodegen* codegen, const SnxFunction* function, SnxU64 address, SnxU64 target) {
    if (snx_function_contains(function, target)) {
        if (target <= address) {
            fputs("{ ", out);
            poll(out, target);
        }
        fputs("goto ", out);
        label(out, target);
        fputc(';', out);
        if (target <= address) fputs(" }", out);
    } else
        call(out, codegen, target, 1);
}

void snx_codegen_emit(FILE* out, SnxCodegen* codegen, const SnxFunction* function) {
    size_t i;
    SnxU64 address, next, target;
    SnxU32 word;
    const SnxA64Case* c;
    int dispatch, terminal;
    fputs("void ", out);
    function_name(out, function->start);
    fputs("(CPU* RESTRICT c,u64 e){\n u8* const M=c->mem;\n", out);
    fputs(" if(UNLIKELY(e!=", out);
    snx_a64_literal(out, function->start);
    dispatch = function->leader_count > 1;
    fputs(dispatch ? ")){ c->pc=e; goto D_; }\n" : ")){ CALLDYN(e); return; }\n", out);
    if (function->count && snx_function_address(function, 0) != function->start) {
        fputs(" goto ", out);
        label(out, function->start);
        fputs(";\n", out);
    }
    for (i = 0; i < function->count; ++i) {
        address = snx_function_address(function, i);
        next = address + 4;
        if (function->leaders[i]) {
            label(out, address);
            fputs(":;\n", out);
        }
        word = 0;
        if (!snx_codegen_word(codegen, address, &word))
            c = snx_a64_decode(0);
        else
            c = snx_a64_decode(word);
        if (codegen->check_fetch) {
            fputs(" {u32 live; snx_fetch(c,", out);
            snx_a64_literal(out, address);
            fprintf(out, ",&live); if(live!=0x%08lXu){snx_interp(c,", (unsigned long)word);
            snx_a64_literal(out, address);
            fputs(");return;}}\n", out);
        }
        if (c->kind == SNX_FLOW_UNDEF) {
            fputs(" c->pc=", out);
            snx_a64_literal(out, address);
            fputs("; c->rt->undef(c,", out);
            snx_a64_literal(out, address);
            fprintf(out, ",0x%08lXu); return;\n", (unsigned long)word);
            continue;
        }
        terminal = 0;
        snx_a64_scope(out, word, address, c);
        switch (c->kind) {
            case SNX_FLOW_PLAIN:
                if (!snx_a64_fast_simd(out, word)) snx_a64_body(out, c->first, "c->rt->undef(c,pc,w)");
                break;
            case SNX_FLOW_B:
                branch(out, codegen, function, address, snx_a64_target(word, address));
                terminal = 1;
                break;
            case SNX_FLOW_BC:
                fprintf(out, "if(%s) ", c->first);
                branch(out, codegen, function, address, snx_a64_target(word, address));
                break;
            case SNX_FLOW_BL:
                target = snx_a64_target(word, address);
                fputs("c->x[30]=pc+4; ", out);
                call(out, codegen, target, 0);
                fputs(" if(UNLIKELY(c->pc!=pc+4)) return;", out);
                break;
            case SNX_FLOW_BLR:
                fprintf(out, "{ u64 t_=%s; c->x[30]=pc+4; CALLDYN(t_); if(UNLIKELY(c->pc!=pc+4)) return; }", c->first);
                break;
            case SNX_FLOW_BR:
                if (dispatch)
                    fprintf(out, "c->pc=%s; goto D_;", c->first);
                else
                    fprintf(out, "{ u64 t_=%s; CALLDYN(t_); return; }", c->first);
                terminal = 1;
                break;
            case SNX_FLOW_RET:
                fprintf(out, "c->pc=%s; return;", c->first);
                terminal = 1;
                break;
            case SNX_FLOW_SVC: fprintf(out, "c->pc=pc+4; c->rt->svc(c,%s);", c->first); break;
            case SNX_FLOW_BRK:
                fprintf(out, "c->pc=pc; c->rt->brk(c,pc,%s); return;", c->first);
                terminal = 1;
                break;
            case SNX_FLOW_UNDEF: break;
        }
        fputs(" }\n", out);
        if (!terminal && (!snx_function_contains(function, next) ||
                          (i + 1 < function->count && snx_function_address(function, i + 1) != next))) {
            fputc(' ', out);
            call(out, codegen, next, 1);
            fputc('\n', out);
        }
    }
    if (dispatch) {
        fputs(" D_: if(UNLIKELY(__atomic_load_n(&c->poll,__ATOMIC_RELAXED))) c->rt->poll(c);\n switch(c->pc){\n", out);
        for (i = 0; i < function->count; ++i)
            if (function->leaders[i]) {
                address = snx_function_address(function, i);
                fputs(" case ", out);
                snx_a64_literal(out, address);
                fputs(": goto ", out);
                label(out, address);
                fputs(";\n", out);
            }
        fputs(" default: CALLDYN(c->pc); return; }\n", out);
    }
    fputs("}\n", out);
}
