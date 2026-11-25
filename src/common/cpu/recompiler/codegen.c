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

/* read_block's arrays, read_capacity entries each */
static SnxU64* read_addresses(const SnxCodegen* codegen) { return (SnxU64*)(void*)codegen->read_block; }
static SnxU32* read_words(const SnxCodegen* codegen) {
    return (SnxU32*)(void*)(codegen->read_block + codegen->read_capacity * sizeof(SnxU64));
}
static unsigned char* read_used(const SnxCodegen* codegen) {
    return codegen->read_block + codegen->read_capacity * (sizeof(SnxU64) + sizeof(SnxU32));
}

static void read_put(SnxCodegen* codegen, SnxU64 address, SnxU32 word) {
    if (!codegen->read_capacity || (codegen->read_count + 1) * 4 >= codegen->read_capacity * 3) {
        SnxCodegen old = {.read_block = codegen->read_block, .read_capacity = codegen->read_capacity};
        codegen->read_capacity = old.read_capacity ? old.read_capacity * 2 : 128;
        codegen->read_count = 0;
        codegen->read_block = (unsigned char*)snx_alloc(codegen->read_capacity * (sizeof(SnxU64) + sizeof(SnxU32) + 1));
        memset(read_used(codegen), 0, codegen->read_capacity);
        for (size_t i = 0; i < old.read_capacity; ++i)
            if (read_used(&old)[i]) read_put(codegen, read_addresses(&old)[i], read_words(&old)[i]);
        free(old.read_block);
    }
    size_t i = address_hash(address) & (codegen->read_capacity - 1);
    while (read_used(codegen)[i]) i = (i + 1) & (codegen->read_capacity - 1);
    read_used(codegen)[i] = 1;
    read_addresses(codegen)[i] = address;
    read_words(codegen)[i] = word;
    ++codegen->read_count;
}

/* the word at offset off of m: from a module whose copy an update can change meanwhile, the one this codegen read
   there first. Read twice, a word that changed in between would give discovery one instruction and the emitter
   another (a branch to an address discovery made no block for) */
static SnxU32 image_word_once(SnxCodegen* codegen, const SnxCodeImage* m, size_t off) {
    if (!m->generation) return snx_code_image_word(m, off);
    SnxU64 address = m->base + off;
    if (codegen->read_capacity)
        for (size_t i = address_hash(address) & (codegen->read_capacity - 1); read_used(codegen)[i];
             i = (i + 1) & (codegen->read_capacity - 1))
            if (read_addresses(codegen)[i] == address) return read_words(codegen)[i];
    SnxU32 word = snx_code_image_word(m, off);
    read_put(codegen, address, word);
    return word;
}

int snx_codegen_words_current(const SnxCodegen* codegen) {
    for (size_t i = 0; i < codegen->read_capacity; ++i) {
        if (!read_used(codegen)[i]) continue;
        SnxU64 address = read_addresses(codegen)[i];
        const SnxCodeImage* m = NULL;
        for (size_t k = 0; !m && k < codegen->module_count; ++k) {
            const SnxCodeImage* c = &codegen->modules[k];
            if (c->generation && address >= c->base && address - c->base + 4 <= c->image_size) m = c;
        }
        if (!m || snx_code_image_word(m, (size_t)(address - m->base)) != read_words(codegen)[i]) return 0;
    }
    return 1;
}

int snx_codegen_word(SnxCodegen* codegen, SnxU64 address, SnxU32* word) {
    SnxCodeImage* module = snx_codegen_module(codegen, address);
    if (!module || (address & 3)) return 0;
    size_t off = (size_t)(address - module->base);
    if (off > module->image_size || module->image_size - off < 4 ||
        address - (module->base + module->text_off) + 4 > module->text_size)
        return 0;
    *word = image_word_once(codegen, module, off);
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

int snx_codegen_routed(const SnxCodegen* codegen, SnxU64 address) {
    size_t i, lo, hi, middle;
    for (i = 0; i < codegen->module_count; ++i) {
        const SnxCodeImage* m = &codegen->modules[i];
        SnxU64 start = m->base + m->text_off, offset;
        if (address < start || address - start >= m->text_size) continue;
        offset = address - m->base;
        lo = 0;
        hi = m->routed_count;
        while (lo < hi) {
            middle = lo + (hi - lo) / 2;
            if (m->routed[middle] < offset)
                lo = middle + 1;
            else
                hi = middle;
        }
        return lo < m->routed_count && m->routed[lo] == offset;
    }
    return 0;
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

/* where a function being explored ends: at a function found before it, and at one a module routes, whose dispatch
   slot a hook may hold: a branch into it is a tail call through that slot, never a jump inside one function */
static int is_boundary(const SnxCodegen* codegen, const Known* known, SnxU64 address) {
    return is_known(known, address) || snx_codegen_routed(codegen, address);
}

/* what word does to the flow, for the guest's features, with insn its decode (a branch's target) */
static SnxA64Flow decode(const SnxCodegen* codegen, SnxU32 word, SnxA64Insn* insn) {
    snx_a64_decode_word(word, codegen->features, insn);
    return insn->flow;
}

/* ---- jump tables: the address-load-plus-branch shapes compilers emit for a switch ---- */

/* where a BR reads its cases: the table's base, the entry's width and sign, the index register, and the ADD that turns
   an entry into an address (its shift and extension; none when the table holds absolute addresses) */
typedef struct {
    SnxU64 base;
    /* what the ADD adds an entry to: the table's base, or an address in the code that the function takes for it
       (LLVM's compressed tables: unsigned bytes or halves, counted in instructions from a label) */
    SnxU64 anchor;
    SnxU64 load; /* the entry load's address, the bound's search starts there */
    unsigned index_reg;
    unsigned base_reg; /* the register the load takes the table's address from */
    unsigned width; /* entry bytes */
    unsigned scale; /* the ADD's left shift of the entry */
    unsigned ext;   /* 1 UXTW, 2 SXTW, 0 the 64-bit entry */
    int sign;       /* the load sign-extends the entry */
    int absolute;   /* the entry is the target, not an offset from base */
} JumpTable;

/* instructions a compiler puts between the ADD that makes a case's address and its BR, and between the code that
   takes a table's address and the entry's load (the first cases' moves; a loop keeps the table's address outside) */
#define SNX_JUMP_TABLE_GAP   16
#define SNX_JUMP_TABLE_BASE  256
#define SNX_JUMP_TABLE_FAR   64 /* between an ADRP and its ADD, scheduled apart */
/* entries of a table nothing says the end of, or a mask alone in an explored function */
#define SNX_JUMP_TABLE_LOOSE 256
/* instructions either side of a table's code in which the branch to it is looked for */
#define SNX_JUMP_TABLE_REACH 8192

#define SNX_A64_NOP 0xd503201fUL
static int is_br(SnxU32 word) { return (word & 0xfffffc1fUL) == 0xd61f0000UL; }

/* the sign-extended value of the low bits of value */
static int64_t sign_extend(SnxU64 value, unsigned bits) {
    SnxU64 sign = (SnxU64)1 << (bits - 1);
    return (int64_t)((value ^ sign) - sign);
}

/* a load of a table entry, LDR/LDRB/LDRH/LDRSW/LDRSB/LDRSH [base, index, ...]; width and sign are the entry's */
static int table_load(SnxU32 word, unsigned* base, unsigned* index, unsigned* entry, unsigned* width, int* sign) {
    unsigned size, opc;
    if ((word & 0x3c200c00UL) != 0x38200800UL) return 0; /* load/store register, register offset, V=0 */
    size = (word >> 30) & 3;
    opc = (word >> 22) & 3;
    if (!opc || (size == 3 && opc != 1) || (size == 2 && opc == 3)) return 0; /* a store, PRFM, PRFM's 64-bit form */
    *base = (word >> 5) & 31;
    *index = (word >> 16) & 31;
    *entry = word & 31;
    *width = 1u << size;
    *sign = opc >= 2;
    return 1;
}

/* ADD Xrd, Xrn, Xrm with the entry rm shifted; ext says the ADD extends rm from 32 bits */
static int table_add(SnxU32 word, unsigned* rd, unsigned* rn, unsigned* rm, unsigned* scale, unsigned* ext) {
    if ((word & 0xff208000UL) == 0x8b000000UL) { /* ADD (shifted register) */
        unsigned shift = (word >> 22) & 3, amount = (word >> 10) & 63;
        if (shift || amount > 3) return 0;
        *rd = word & 31;
        *rn = (word >> 5) & 31;
        *rm = (word >> 16) & 31;
        *scale = amount;
        *ext = 0;
        return 1;
    }
    if ((word & 0xff200000UL) == 0x8b200000UL) { /* ADD (extended register) */
        unsigned option = (word >> 13) & 7, amount = (word >> 10) & 7;
        if (amount > 3 || (option != 2 && option != 3 && option != 6)) return 0;
        *rd = word & 31;
        *rn = (word >> 5) & 31;
        *rm = (word >> 16) & 31;
        *scale = amount;
        *ext = option == 2 ? 1 : option == 6 ? 2 : 0;
        return 1;
    }
    return 0;
}

/* The address the instruction at address puts in a register, 0 for none: ADR's, or an ADD of the page an ADRP a few
   instructions before put in its source (the linker relaxes such a pair into a NOP and an ADR when the target is
   near). Code takes function pointers this way, for a callback or a call through a register. One game's main, which
   has unwind info for a seventh of its text, reaches four fifths of the functions it misses only through them */
static SnxU64 address_value(SnxCodegen* codegen, SnxU64 address, SnxU32 word, unsigned* reg) {
    if ((word & 0x9f000000UL) == 0x10000000UL) { /* ADR */
        SnxU64 imm = ((SnxU64)((word >> 5) & 0x7ffffUL) << 2) | ((word >> 29) & 3);
        *reg = word & 31;
        return address + (imm & 0x100000UL ? imm | ~(SnxU64)0x1fffffUL : imm);
    }
    if ((word & 0xff800000UL) != 0x91000000UL) return 0; /* ADD Xd, Xn, #imm12 */
    *reg = word & 31;
    SnxU32 source = (word >> 5) & 31;
    for (unsigned i = 1; i <= 8 && address >= (SnxU64)i * 4; ++i) {
        SnxU32 before;
        if (!snx_codegen_word(codegen, address - (SnxU64)i * 4, &before)) return 0;
        if ((before & 0x9f000000UL) == 0x90000000UL && (before & 31) == source) { /* ADRP */
            SnxU64 imm = ((SnxU64)((before >> 5) & 0x7ffffUL) << 2) | ((before >> 29) & 3);
            if (imm & 0x100000UL) imm |= ~(SnxU64)0x1fffffUL;
            return ((address - (SnxU64)i * 4) & ~(SnxU64)0xfff) + (imm << 12) +
                   ((SnxU64)((word >> 10) & 0xfff) << ((word & (1UL << 22)) ? 12 : 0));
        }
        /* the register written in between (or perhaps only stored: bits 0-4 are Rt too), or a way in from elsewhere */
        if ((before & 31) == source || snx_a64_flow(before, codegen->features) != SNX_A64_FLOW_PLAIN) return 0;
    }
    return 0;
}

/* whether the address a plain instruction at address takes is a jump table's base: an entry load from the register
   and a BR on the entry, or on the base plus the entry, follows within a few instructions */
static int jump_table_base(SnxCodegen* codegen, SnxU64 address, SnxU32 reg) {
    for (unsigned i = 1; i <= 8; ++i) {
        SnxU32 after;
        unsigned base, index, entry, width, rd, rn, rm, scale, ext;
        int sign;
        if (!snx_codegen_word(codegen, address + (SnxU64)i * 4, &after)) return 0;
        if (is_br(after)) return ((after >> 5) & 31) == reg; /* adr x, table; ...; br x */
        if (table_load(after, &base, &index, &entry, &width, &sign) && base == reg) {
            /* ldr xentry, [reg, ...]; add xrd, reg, xentry; br xrd, or br xentry for an absolute table */
            for (unsigned j = i + 1; j <= i + 3; ++j) {
                SnxU32 next;
                if (!snx_codegen_word(codegen, address + (SnxU64)j * 4, &next)) return 0;
                if (!sign && is_br(next) && ((next >> 5) & 31) == entry) return 1;
                if (table_add(next, &rd, &rn, &rm, &scale, &ext) && rn == reg && rm == entry) {
                    SnxU32 br;
                    if (snx_codegen_word(codegen, address + (SnxU64)(j + 1) * 4, &br) && is_br(br) &&
                        ((br >> 5) & 31) == rd)
                        return 1;
                }
            }
        }
        if (snx_a64_flow(after, codegen->features) != SNX_A64_FLOW_PLAIN) return 0;
    }
    return 0;
}

/* taken_address's: 0 for a jump table's base, a place in the function its cases are reached from, not a function */
static SnxU64 taken_address(SnxCodegen* codegen, SnxU64 address, SnxU32 word) {
    unsigned reg;
    SnxU64 target = address_value(codegen, address, word, &reg);
    return target && !jump_table_base(codegen, address, reg) ? target : 0;
}

/* whether a plain instruction may write general register reg: its Rd or Rt (bits 0-4, an FP register's number there
   too, which only ends a search early), a load pair's second register, the base a load or store writes back */
static int may_write(SnxU32 word, unsigned reg) {
    if ((word & 31) == reg) return 1;
    if ((word & 0x0a000000UL) != 0x08000000UL) return 0; /* not a load or store */
    if ((word & 0x3a000000UL) == 0x28000000UL)           /* a pair: a load's Rt2, a pre- or post-indexed one's base */
        return ((word & (1UL << 22)) && ((word >> 10) & 31) == reg) ||
               ((word & (1UL << 23)) && ((word >> 5) & 31) == reg);
    return (word & 0x3b200400UL) == 0x38000400UL && ((word >> 5) & 31) == reg; /* pre- or post-indexed: its base */
}

/* The instruction that gives reg the value it has at address: the nearest before it that may write reg, at most limit
   back, 0 for none. Only plain instructions lie in between; with far, also branches and, for a register a call keeps
   (x19 to x29: a function without a frame pointer uses x29 as any other), calls: the nearest in the function's layout, where a loop's setup comes before the loop */
static SnxU64 written_at(SnxCodegen* codegen, SnxU64 address, unsigned reg, unsigned limit, int far) {
    for (unsigned i = 1; i <= limit && address >= (SnxU64)i * 4; ++i) {
        SnxU32 word;
        SnxA64Flow flow;
        if (!snx_codegen_word(codegen, address - (SnxU64)i * 4, &word)) return 0;
        flow = snx_a64_flow(word, codegen->features);
        if (flow == SNX_A64_FLOW_PLAIN) {
            if (may_write(word, reg)) return address - (SnxU64)i * 4;
        } else if (!far || (flow != SNX_A64_FLOW_BC && flow != SNX_A64_FLOW_B &&
                            !((flow == SNX_A64_FLOW_BL || flow == SNX_A64_FLOW_BLR) && reg >= 19 && reg <= 29)))
            return 0;
    }
    return 0;
}

/* the address reg holds at address, which the code took for it (address_value) at most limit instructions back, 0 for
   none */
static SnxU64 address_in(SnxCodegen* codegen, SnxU64 address, unsigned reg, unsigned limit, int far) {
    SnxU64 at = written_at(codegen, address, reg, limit, far);
    SnxU32 word;
    unsigned written;
    SnxU64 value = at && snx_codegen_word(codegen, at, &word) ? address_value(codegen, at, word, &written) : 0;
    return value && written == reg ? value : 0;
}

/* what explore or the leaders found for the BR at address from its function's flow: the entry, or NULL */
static SnxFlowTable* recorded(const SnxCodegen* codegen, SnxU64 address) {
    size_t lo = 0, hi = codegen->flow_table_count;
    while (lo < hi) {
        size_t middle = lo + (hi - lo) / 2;
        if (codegen->flow_tables[middle].address < address)
            lo = middle + 1;
        else
            hi = middle;
    }
    return lo < codegen->flow_table_count && codegen->flow_tables[lo].address == address ? &codegen->flow_tables[lo]
                                                                                         : NULL;
}
#define SNX_TABLE_REFUSED (~(SnxU64)0) /* a base the flow gave, and then another way to the BR didn't */
/* the BR at address's entry, made where there is none */
static SnxFlowTable* record_table(SnxCodegen* codegen, SnxU64 address) {
    SnxFlowTable* r = recorded(codegen, address);
    size_t at = codegen->flow_table_count;
    if (r) return r;
    if (codegen->flow_table_count == codegen->flow_table_capacity) {
        size_t capacity = codegen->flow_table_capacity ? codegen->flow_table_capacity * 2 : 64;
        codegen->flow_tables = (SnxFlowTable*)snx_realloc(codegen->flow_tables, capacity * sizeof(*r));
        codegen->flow_table_capacity = capacity;
    }
    while (at && codegen->flow_tables[at - 1].address > address) --at;
    r = &codegen->flow_tables[at];
    memmove(r + 1, r, (codegen->flow_table_count - at) * sizeof(*r));
    *r = (SnxFlowTable){.address = address};
    ++codegen->flow_table_count;
    return r;
}

/* the first of the program's jump tables past base's, ~0 for none */
static SnxU64 next_table(const SnxCodegen* codegen, SnxU64 base) {
    size_t lo = 0, hi = codegen->table_base_count;
    while (lo < hi) {
        size_t middle = lo + (hi - lo) / 2;
        if (codegen->table_bases[middle] <= base)
            lo = middle + 1;
        else
            hi = middle;
    }
    return lo < codegen->table_base_count ? codegen->table_bases[lo] : ~(SnxU64)0;
}

/* whether the BR at address dispatches through a jump table, filling in where its cases are */
static int jump_table_shape(SnxCodegen* codegen, SnxU64 address, SnxU32 word, JumpTable* table) {
    SnxU32 made, load;
    unsigned rd, rn, rm, scale, ext, base, index, entry, width, target = (word >> 5) & 31;
    int sign;
    memset(table, 0, sizeof(*table));
    /* what makes the BR's register: an ADD of the entry to an address, or the entry's own load */
    SnxU64 at = written_at(codegen, address, target, SNX_JUMP_TABLE_GAP, 0);
    if (!at || !snx_codegen_word(codegen, at, &made)) return 0;
    if (table_add(made, &rd, &rn, &rm, &scale, &ext)) {
        if (rd != target) return 0;
        table->load = written_at(codegen, at, rm, 4, 0);
        if (!table->load || !snx_codegen_word(codegen, table->load, &load) ||
            !table_load(load, &base, &index, &entry, &width, &sign) || entry != rm)
            return 0;
        table->scale = scale;
        table->ext = ext;
        table->sign = sign;
    } else if (at == address - 4 && table_load(made, &base, &index, &entry, &width, &sign) && entry == target &&
               width == 8 && !sign) {
        /* ldr xentry, [base, ...]; br xentry: absolute addresses */
        table->load = at;
        table->absolute = 1;
        rn = base;
    } else
        return 0;
    table->index_reg = index;
    table->base_reg = base;
    table->width = width;
    /* the base's own address: ADR, or ADRP and an ADD, by the load or, for a table outside the text, further back
       (before the loop the switch is in) */
    table->base = address_in(codegen, table->load, base, 8, 0);
    if (!table->base && !table->absolute) {
        /* as function_base found it from the function's flow, else further back, outside the text */
        const SnxFlowTable* r = recorded(codegen, address);
        if (r && r->base == SNX_TABLE_REFUSED) return 0;
        table->base = r ? r->base : 0;
        if (!table->base) {
            table->base = address_in(codegen, table->load, base, SNX_JUMP_TABLE_BASE, 1);
            if (snx_codegen_module(codegen, table->base)) return 0;
        }
    }
    if (!table->base) return 0;
    table->anchor = rn == base ? table->base : address_in(codegen, at, rn, 8, 0);
    return table->anchor != 0;
}

/* an image read, not only the text: a jump table sits in .rodata */
static int image_read(SnxCodegen* codegen, SnxU64 address, unsigned width, SnxU64* value) {
    for (size_t i = 0; i < codegen->module_count; ++i) {
        const SnxCodeImage* m = &codegen->modules[i];
        if (!m->image || address < m->base || address - m->base > m->image_size ||
            m->image_size - (size_t)(address - m->base) < width)
            continue;
        size_t off = (size_t)(address - m->base);
        if (m->atomic_words) {
            unsigned char bytes[8];
            size_t residue = (size_t)(4 - (m->base & 3)) & 3;
            /* Jump-table entries are discovery hints; under sustained patching, omit this table and let the runtime
               JIT handle its targets rather than pinning a worker on an image that never stays still. */
            for (unsigned retry = 0;; ++retry) {
                if (retry == 128) return 0;
                uint64_t before = m->generation ? atomic_load_explicit(m->generation, memory_order_seq_cst) : 0;
                uint64_t after;
                size_t last_word_off = SIZE_MAX;
                uint32_t word = 0;
                if (before & 1) continue;
                for (unsigned i = 0; i < width; ++i) {
                    size_t byte_off = off + i;
                    if (byte_off < residue) {
                        bytes[i] = m->image[byte_off];
                        continue;
                    }
                    size_t word_off = byte_off - ((byte_off - residue) & 3);
                    if (word_off + 4 > m->image_size) {
                        bytes[i] = m->image[byte_off];
                        continue;
                    }
                    if (word_off != last_word_off) {
                        word = image_word_once(codegen, m, word_off);
                        last_word_off = word_off;
                    }
                    bytes[i] = (unsigned char)(word >> ((byte_off - word_off) * 8));
                }
                after = m->generation ? atomic_load_explicit(m->generation, memory_order_seq_cst) : before;
                if (before == after) break;
            }
            *value = width == 1 ? bytes[0] : width == 2 ? snx_ld_le16(bytes) : width == 4 ? snx_ld_le32(bytes)
                                                                                             : snx_ld_le64(bytes);
        } else {
            const unsigned char* p = m->image + off;
            *value = width == 1 ? p[0] : width == 2 ? snx_ld_le16(p) : width == 4 ? snx_ld_le32(p) : snx_ld_le64(p);
        }
        return 1;
    }
    return 0;
}

static const _Atomic uint64_t* image_generation(const SnxCodegen* codegen, SnxU64 address) {
    for (size_t i = 0; i < codegen->module_count; ++i) {
        const SnxCodeImage* m = &codegen->modules[i];
        if (m->atomic_words && address >= m->base && address - m->base < m->image_size) return m->generation;
    }
    return NULL;
}

/* the entries a CMP Wn, #imm or Xn, #imm of the index bounds a table to, cases + 1; 0 when word is no such compare */
static unsigned compare_bound(SnxU32 word, unsigned index_reg) {
    if (((word & 0xff800000UL) != 0x71000000UL && (word & 0xff800000UL) != 0xf1000000UL) ||
        ((word >> 5) & 31) != index_reg || (word & 31) != 31)
        return 0;
    SnxU64 imm = (word >> 10) & 0xfff;
    if (word & (1UL << 22)) imm <<= 12;
    return imm < SNX_JUMP_TABLE_ENTRIES ? (unsigned)imm + 1 : SNX_JUMP_TABLE_ENTRIES;
}

/* the bound of a table whose code starts a block, block, that the compare's branch jumps to: cmp index, #imm;
   b.ls block, in the instructions around it */
static unsigned branch_bound(SnxCodegen* codegen, SnxU64 block, unsigned index_reg) {
    for (int64_t at = -SNX_JUMP_TABLE_REACH; at < SNX_JUMP_TABLE_REACH; ++at) {
        SnxU64 address = block + (SnxU64)(at * 4);
        SnxU32 word, compare;
        unsigned limit;
        if (!snx_codegen_word(codegen, address, &word) || (word & 0xff00001fUL) != 0x54000009UL) continue; /* B.LS */
        if (address + (SnxU64)(sign_extend((word >> 5) & 0x7ffffUL, 19) * 4) != block) continue;
        limit = snx_codegen_word(codegen, address - 4, &compare) ? compare_bound(compare, index_reg) : 0;
        if (limit) return limit;
    }
    return 0;
}

/* the entries the index's own value bounds a table to where no compare guards it, its making nearest before at: an
   AND of its low k bits, a UBFX of k bits (LSR, UXTB and UXTH are UBFMs too), 2^k for k up to 8; a 32-bit MOV's
   source's (the index widened); 0 for none */
static unsigned index_bound(SnxCodegen* codegen, SnxU64 at, unsigned reg, int depth) {
    SnxU64 made = written_at(codegen, at, reg, SNX_JUMP_TABLE_GAP, 0);
    SnxU32 word;
    unsigned k = 0;
    if (!made || !snx_codegen_word(codegen, made, &word) || (word & 31) != reg) return 0;
    if ((word & 0x7f800000UL) == 0x12000000UL && !((word >> 16) & 63)) { /* AND (immediate), no rotation */
        unsigned imms = (word >> 10) & 63;
        if ((word >> 22) & 1) /* N: one 64-bit element */
            k = imms + 1;
        else if (!(imms & 0x20)) /* one 32-bit element */
            k = (imms & 31) + 1;
    } else if ((word & 0x7f800000UL) == 0x53000000UL) { /* UBFM */
        unsigned immr = (word >> 16) & 63, imms = (word >> 10) & 63;
        if (imms >= immr) k = imms - immr + 1;
    } else if ((word & 0xffe0ffe0UL) == 0x2a0003e0UL && depth < 2) /* mov wd, wm */
        return index_bound(codegen, made, (word >> 16) & 31, depth + 1);
    return k && k <= 8 ? 1u << k : 0;
}

/* whether the plain instruction word writes general register reg: its Rd, but a load's or store's Rt where it loads
   it (and a pair's Rt2), the base a pre- or post-indexed access or structure writes back, a SIMD&FP instruction's Rd
   only where it moves or converts to a general register. What it can't tell writes reg */
static int writes_reg(SnxU32 w, unsigned reg) {
    unsigned rt = w & 31, rn = (w >> 5) & 31, v = (w >> 26) & 1, opc = (w >> 22) & 3, idx = (w >> 10) & 3;
    if ((w & 0x0a000000UL) == 0x08000000UL) {   /* loads and stores */
        if ((w & 0x3f000000UL) == 0x08000000UL) /* exclusives: Rt, Rt2, Rs */
            return rt == reg || ((w >> 10) & 31) == reg || ((w >> 16) & 31) == reg;
        if ((w & 0x3a000000UL) == 0x28000000UL) /* pairs: bit 22 a load, bit 23 a writeback */
            return (!v && (w >> 22 & 1) && (rt == reg || ((w >> 10) & 31) == reg)) || ((w >> 23 & 1) && rn == reg);
        if ((w & 0x3b000000UL) == 0x18000000UL) return !v && rt == reg;        /* LDR (literal) */
        if ((w & 0x3b000000UL) == 0x39000000UL) return !v && opc && rt == reg; /* unsigned offset */
        if ((w & 0x3b200000UL) == 0x38000000UL)                                /* unscaled, pre, post */
            return (!v && opc && rt == reg) || ((idx == 1 || idx == 3) && rn == reg);
        if ((w & 0x3b200c00UL) == 0x38200800UL) return !v && opc && rt == reg;     /* register offset */
        if ((w & 0xbe000000UL) == 0x0c000000UL) return (w >> 23 & 1) && rn == reg; /* structures' post-index */
        return rt == reg || rn == reg || ((w >> 16) & 31) == reg;
    }
    if ((w & 0x0e000000UL) == 0x0e000000UL) {
        /* SIMD and FP: UMOV, SMOV and the scalar moves and conversions (bit 21 clear: of fixed point) to a general
           register, not from one (SCVTF, UCVTF, FMOV) */
        unsigned op = (w >> 16) & 7;
        if (rt != reg) return 0;
        if ((w & 0x5f000000UL) == 0x1e000000UL && (!((w >> 21) & 1) || !((w >> 10) & 63)))
            return op != 2 && op != 3 && op != 7;
        return (w & 0xbfe0fc00UL) == 0x0e003c00UL || (w & 0xbfe0fc00UL) == 0x0e002c00UL;
    }
    if ((w & 0xffe00000UL) == 0xd5000000UL) return 0;                       /* MSR, SYS: Rt is read */
    if ((w & 0x1fe00000UL) == 0x1a400000UL) return 0;                       /* CCMP, CCMN: the flags */
    if ((w & 0x1fe00000UL) == 0x1a000000UL && ((w >> 10) & 0x3f)) return 0; /* RMIF, SETF: a mask */
    return rt == reg;
}

/* the entries the compare guarding the table bounds: cases + 1, or 0 when there is no such compare */
static unsigned table_bound(SnxCodegen* codegen, SnxU64 load, unsigned index_reg) {
    for (unsigned i = 1; i <= 16 && load >= (SnxU64)i * 4; ++i) {
        SnxU32 word;
        SnxA64Flow flow;
        unsigned limit;
        if (!snx_codegen_word(codegen, load - (SnxU64)i * 4, &word)) break;
        limit = compare_bound(word, index_reg);
        if (limit) return limit;
        flow = snx_a64_flow(word, codegen->features);
        /* nothing runs on into the table's code: the compare is where a branch to it is */
        if (flow == SNX_A64_FLOW_B || flow == SNX_A64_FLOW_RET || flow == SNX_A64_FLOW_BR)
            return branch_bound(codegen, load - (SnxU64)i * 4 + 4, index_reg);
        if (flow != SNX_A64_FLOW_PLAIN && flow != SNX_A64_FLOW_BC) break;
        /* the index widened to 64 bits after the compare (mov wI, wS; and xI, xS, #0xffffffff): the compare is on
           the register it came from */
        if (flow == SNX_A64_FLOW_PLAIN && writes_reg(word, index_reg)) {
            if ((word & 0x7fe0ffe0UL) == 0x2a0003e0UL)
                index_reg = (word >> 16) & 31;
            else if ((word & 0xfffffc00UL) == 0x92407c00UL)
                index_reg = (word >> 5) & 31;
            else
                return 0; /* made here: a compare before is of another value */
        }
    }
    return 0;
}

/* where the table's entry i sends the BR: 1, or 0 where it can't be read */
static int entry_target(SnxCodegen* codegen, const JumpTable* table, unsigned i, SnxU64* target) {
    SnxU64 raw;
    int64_t value;
    if (!image_read(codegen, table->base + (SnxU64)i * table->width, table->width, &raw)) return 0;
    value = table->sign ? sign_extend(raw, table->width * 8) : (int64_t)raw;
    if (table->ext == 1)
        value = (int64_t)(SnxU32)value;
    else if (table->ext == 2)
        value = (int64_t)(int32_t)value;
    *target = table->absolute ? (SnxU64)value : table->anchor + (SnxU64)(value * ((int64_t)1 << table->scale));
    return 1;
}

/* whether target, an entry's, is in the unwind range its table's function has */
static int in_range(const SnxFlowTable* flow, SnxU64 target) {
    return target >= flow->first && (target - flow->first) / 4 < flow->count;
}

/* the entries of a table no compare bounds, in an unwind range, which holds every case of it: up to the first that
   leaves the range (the address at its very end is an unreachable case's, whose block has no instruction), where the
   program's next table starts, or past what the index can reach. All the table's are among them, and where other data
   follows it some of that, a leader more in the function each */
static unsigned table_extent(SnxCodegen* codegen, const JumpTable* table, const SnxFlowTable* flow) {
    SnxU64 room = (next_table(codegen, table->base) - table->base) / table->width, target;
    unsigned limit = flow->max ? flow->max : table->width == 1 ? SNX_JUMP_TABLE_LOOSE : SNX_JUMP_TABLE_ENTRIES, n = 0;
    while (n < limit && n < room && entry_target(codegen, table, n, &target) && !(target & 3) &&
           (in_range(flow, target) || target == flow->first + (SnxU64)flow->count * 4))
        ++n;
    return n;
}

size_t snx_codegen_jump_table(SnxCodegen* codegen, SnxU64 address, SnxU64* targets, size_t capacity) {
    JumpTable table;
    const SnxFlowTable* flow;
    const _Atomic uint64_t* code_generation = image_generation(codegen, address);
    const _Atomic uint64_t* table_generation;
    uint64_t code_version = code_generation ? atomic_load_explicit(code_generation, memory_order_seq_cst) : 0;
    uint64_t table_version;
    SnxU32 word;
    unsigned limit;
    size_t count = 0;
    int ranged = 0;
    if (!capacity || (code_version & 1) || !snx_codegen_word(codegen, address, &word) || !is_br(word) ||
        !jump_table_shape(codegen, address, word, &table))
        return 0;
    limit = table_bound(codegen, table.load, table.index_reg);
    flow = recorded(codegen, address);
    if (!limit && flow) {
        limit = flow->bound;
        ranged = !limit && flow->count;
        if (ranged && !(limit = table_extent(codegen, &table, flow))) return 0;
    }
    if (!limit) limit = index_bound(codegen, table.load, table.index_reg, 0);
    table_generation = image_generation(codegen, table.base);
    table_version = table_generation == code_generation
                        ? code_version
                        : table_generation ? atomic_load_explicit(table_generation, memory_order_seq_cst) : 0;
    if ((table_version & 1) ||
        (code_generation && atomic_load_explicit(code_generation, memory_order_seq_cst) != code_version))
        return 0;
    /* an entry of four bytes that is no case leaves the text and ends an unbounded table; a byte or a half from an
       address in the code lands in the text whatever it holds, so such a table is followed only to its bound */
    if (!limit && table.anchor != table.base && table.width < 4) return 0;
    for (unsigned i = 0; i < (limit ? limit : SNX_JUMP_TABLE_LOOSE) && count < capacity; ++i) {
        SnxU64 target;
        int found = 0;
        if (!entry_target(codegen, &table, i, &target)) break;
        if (ranged && !in_range(flow, target)) continue;
        if ((target & 3) || !snx_codegen_module(codegen, target)) {
            if (limit) continue; /* not code: the switch's default, or the next table's word */
            break;               /* no bound: the table ends where the code does */
        }
        for (size_t k = 0; k < count; ++k)
            if (targets[k] == target) {
                found = 1;
                break;
            }
        if (!found) targets[count++] = target;
    }
    if ((code_generation && atomic_load_explicit(code_generation, memory_order_seq_cst) != code_version) ||
        (table_generation && table_generation != code_generation &&
         atomic_load_explicit(table_generation, memory_order_seq_cst) != table_version))
        return 0;
    return count;
}

/* the page the ADRP word at address puts in its register */
static SnxU64 adrp_page(SnxU64 address, SnxU32 word) {
    SnxU64 imm = ((SnxU64)((word >> 5) & 0x7ffffUL) << 2) | ((word >> 29) & 3);
    if (imm & 0x100000UL) imm |= ~(SnxU64)0x1fffffUL;
    return (address & ~(SnxU64)0xfff) + (imm << 12);
}

/* the instruction at body's index of address (body sorted, or count words from first), count where none */
static size_t body_index(const SnxU64* body, SnxU64 first, size_t count, SnxU64 address) {
    size_t lo = 0, hi = count;
    if (!body) return address >= first && !((address - first) & 3) && (address - first) / 4 < count
                          ? (size_t)((address - first) / 4)
                          : count;
    while (lo < hi) {
        size_t middle = lo + (hi - lo) / 2;
        if (body[middle] < address)
            lo = middle + 1;
        else
            hi = middle;
    }
    return lo < count && body[lo] == address ? lo : count;
}

/* a function's flow, backward: for each instruction of body (count sorted addresses, or as many words from first),
   its predecessors pred[start[k] .. start[k + 1]], each 2 * the instruction's index, + 1 where it comes by its branch
   rather than falling through. A BR whose table is known comes to each of its cases */
typedef struct {
    size_t *start, *pred;
} Flow;
static void flow_build(SnxCodegen* codegen, const SnxU64* body, SnxU64 first, size_t count, Flow* flow) {
    size_t edges = 0, capacity = 2 * count + SNX_JUMP_TABLE_MAX;
    size_t (*edge)[2] = (size_t (*)[2])snx_alloc(capacity * sizeof(*edge)); /* to, its predecessor */
    for (size_t k = 0; k < count; ++k) {
        SnxU64 a = body ? body[k] : first + (SnxU64)k * 4, targets[SNX_JUMP_TABLE_MAX];
        SnxA64Insn insn;
        SnxU32 w;
        SnxA64Flow kind;
        size_t n = 0;
        if (!snx_codegen_word(codegen, a, &w)) continue;
        kind = decode(codegen, w, &insn);
        if (kind == SNX_A64_FLOW_BR) n = snx_codegen_jump_table(codegen, a, targets, SNX_JUMP_TABLE_MAX);
        if (edges + n + 2 > capacity) {
            capacity = 2 * capacity + n;
            edge = (size_t (*)[2])snx_realloc(edge, capacity * sizeof(*edge));
        }
        for (size_t t = 0; t < n; ++t)
            edge[edges][0] = body_index(body, first, count, targets[t]), edge[edges++][1] = 2 * k;
        if (kind != SNX_A64_FLOW_B && kind != SNX_A64_FLOW_BR && kind != SNX_A64_FLOW_RET &&
            kind != SNX_A64_FLOW_UNDEF && kind != SNX_A64_FLOW_BRK)
            edge[edges][0] = body_index(body, first, count, a + 4), edge[edges++][1] = 2 * k;
        if (kind == SNX_A64_FLOW_B || kind == SNX_A64_FLOW_BC)
            edge[edges][0] = body_index(body, first, count, snx_a64_branch_target(&insn, a)),
            edge[edges++][1] = 2 * k + 1;
    }
    flow->start = (size_t*)snx_alloc((count + 2 + edges) * sizeof(*flow->start));
    memset(flow->start, 0, (count + 2) * sizeof(*flow->start));
    flow->pred = flow->start + count + 2; /* the same block's */
    for (size_t e = 0; e < edges; ++e)
        if (edge[e][0] < count) ++flow->start[edge[e][0] + 2];
    for (size_t k = 0; k < count; ++k) flow->start[k + 2] += flow->start[k + 1];
    for (size_t e = 0; e < edges; ++e)
        if (edge[e][0] < count) flow->pred[flow->start[edge[e][0] + 1]++] = edge[e][1];
    free(edge);
}

/* the address an ADD of an immediate at address makes of the page an ADRP further back than address_value's put in its
   source, no other write of it in between, in a block of plain instructions; 0 for none */
static SnxU64 far_address(SnxCodegen* codegen, SnxU64 address, SnxU32 word, unsigned* reg) {
    unsigned source = (word >> 5) & 31;
    if ((word & 0xff800000UL) != 0x91000000UL) return 0;
    *reg = word & 31;
    for (unsigned i = 1; i <= SNX_JUMP_TABLE_FAR && address >= (SnxU64)i * 4; ++i) {
        SnxU64 at = address - (SnxU64)i * 4;
        SnxU32 before;
        if (!snx_codegen_word(codegen, at, &before) || snx_a64_flow(before, codegen->features) != SNX_A64_FLOW_PLAIN)
            return 0;
        if ((before & 0x9f000000UL) == 0x90000000UL && (before & 31) == source)
            return adrp_page(at, before) + ((SnxU64)((word >> 10) & 0xfff) << ((word & (1UL << 22)) ? 12 : 0));
        if (writes_reg(before, source)) return 0;
    }
    return 0;
}

/* The base of the table of the BR at br that the code just before its load doesn't give (the register set further
   back: before a loop, a call or another way in), from its function's flow (entry the function's): the writes of the
   load's base register that reach the load, each path followed back from it to its first. Where every one takes one
   same address for it (ADR, an ADD of an ADRP's page, an ADRP alone), that is the base: compiled code reaches the BR
   only with the table's address there. A path with no write, through a call or an SVC that changes the register or
   back to an entry that has nothing in it, isn't one the code takes to the BR (a call that doesn't return); but the
   entry's of x0 to x7 may be the caller's table: 0. With whole, so is one back to an instruction with no known way to
   it (the case of a switch whose table isn't known, the BR's own before it is), but a NOP, which pads a block after a
   branch */
static SnxU64 function_base(SnxCodegen* codegen, const JumpTable* table, const SnxU64* body, SnxU64 first, size_t count,
                            SnxU64 entry, const Flow* flow, int whole) {
    SnxU64 value = 0;
    size_t top = 0, capacity = 64, k, load = body_index(body, first, count, table->load);
    size_t* stack = (size_t*)snx_alloc(capacity * sizeof(*stack));  /* 32 * an instruction's index + the register */
    SnxU32* seen = (SnxU32*)snx_alloc((count + 1) * sizeof(*seen)); /* the registers each was come to with */
    int ok = load < count && table->load != entry && flow->start[load] < flow->start[load + 1], start = 1;
    memset(seen, 0, (count + 1) * sizeof(*seen));
    if (ok) stack[top++] = 32 * load + table->base_reg;
    while (ok && top) {
        size_t at = stack[--top] / 32;
        SnxU64 a = body ? body[at] : first + (SnxU64)at * 4, v;
        SnxU32 w;
        unsigned reg = (unsigned)(stack[top] % 32), written = 32;
        if (!snx_codegen_word(codegen, a, &w)) {
            ok = 0;
            break;
        }
        if (!start) { /* the load itself is past the paths */
            SnxA64Flow kind = snx_a64_flow(w, codegen->features);
            if (kind == SNX_A64_FLOW_PLAIN && writes_reg(w, reg)) {
                if ((w & 0xffe0ffe0UL) == 0xaa0003e0UL && ((w >> 16) & 31) != 31) /* mov xD, xM: it was in M before */
                    reg = (w >> 16) & 31;
                else {
                    if ((w & 0x9f000000UL) == 0x90000000UL &&
                        (w & 31) == reg) /* ADRP, no ADD of it on the way: its page */
                        v = adrp_page(a, w), written = reg;
                    else
                        v = address_value(codegen, a, w, &written);
                    if (!v) v = far_address(codegen, a, w, &written);
                    if (!v || written != reg || (value && v != value)) ok = 0;
                    value = v;
                    continue;
                }
            } else if ((kind == SNX_A64_FLOW_BL || kind == SNX_A64_FLOW_BLR || kind == SNX_A64_FLOW_SVC) &&
                       (reg <= 18 || reg == 30))
                continue;
        }
        start = 0;
        if (a == entry ? reg <= 7 : whole && flow->start[at] == flow->start[at + 1] && w != SNX_A64_NOP) {
            ok = 0;
            break;
        }
        if (top + flow->start[at + 1] - flow->start[at] > capacity) {
            capacity = 2 * capacity + flow->start[at + 1] - flow->start[at];
            stack = (size_t*)snx_realloc(stack, capacity * sizeof(*stack));
        }
        for (k = flow->start[at]; k < flow->start[at + 1]; ++k) {
            size_t p = flow->pred[k] / 2;
            if (seen[p] >> reg & 1) continue;
            seen[p] |= 1u << reg;
            stack[top++] = 32 * p + reg;
        }
    }
    free(stack), free(seen);
    return ok ? value : 0;
}

/* whether the word may set the flags: of what is classified below, the ones that set them (an S bit, CCMP and CCMN,
   RMIF and SETF); loads and stores but MOPS's and RCW's, hints and the vectors' don't, and anything else (scalar
   floating point, MSR) may */
static int sets_flags(SnxU32 w) {
    if ((w & 0x3f000000UL) == 0x19000000UL || (w & 0x3b208c00UL) == 0x38208000UL) return 1; /* MOPS, RCW among them */
    if ((w & 0x0a000000UL) == 0x08000000UL) return 0;                                       /* loads and stores */
    if ((w & 0xfffff01fUL) == 0xd503201fUL) return 0;                                       /* hints */
    if ((w & 0x9e000000UL) == 0x0e000000UL) return 0;                                       /* AdvSIMD */
    if ((w & 0x1c000000UL) == 0x10000000UL) /* data processing (immediate) */
        return ((w >> 23) & 7) == 2 ? (w >> 29) & 1 : ((w >> 23) & 7) == 4 ? ((w >> 29) & 3) == 3 : 0;
    if ((w & 0x0e000000UL) == 0x0a000000UL) { /* data processing (register) */
        if (!((w >> 28) & 1)) return ((w >> 24) & 1) ? (w >> 29) & 1 : ((w >> 29) & 3) == 3;
        switch ((w >> 21) & 0xf) {
            case 0: return (w >> 29) & 1 || (w >> 10) & 0x3f; /* ADCS, SBCS; RMIF, SETF */
            case 2: return 1;                                 /* CCMN, CCMP */
            case 6: return (w >> 29) & 1;                     /* SUBPS */
            default: return 0;                                /* CSEL, the 3-source */
        }
    }
    return 1;
}

/* a state of function_bound's walk: the instruction, the index's register on the path, and the guard passed (0 none,
   1 HI/LS: the bound is the compare's immediate + 1, 2 HS/LO: the immediate) */
typedef struct {
    size_t at;
    unsigned char reg, guard;
} BoundState;
/* the guard passed by the instruction at address a, left by its branch (by) or by falling through: a B.HI's or a
   B.HS's way past, a B.LS's or a B.LO's jump */
static unsigned bound_guard(SnxCodegen* codegen, SnxU64 a, unsigned by) {
    SnxU32 w;
    if (!snx_codegen_word(codegen, a, &w) || (w & 0xff000010UL) != 0x54000000UL) return 0;
    switch (w & 15) {
        case 8: return by ? 0 : 1;
        case 9: return by ? 1 : 0;
        case 2: return by ? 0 : 2;
        case 3: return by ? 2 : 0;
        default: return 0;
    }
}
/* each instruction and guard once (seen: its register + 1); 0 where it comes again with the index in another register */
static int bound_push(BoundState* stack, size_t* top, unsigned char* seen, size_t at, unsigned reg, unsigned guard) {
    unsigned char* s = &seen[3 * at + guard];
    if (*s) return *s == reg + 1;
    *s = (unsigned char)(reg + 1);
    stack[*top].at = at;
    stack[*top].reg = (unsigned char)reg;
    stack[(*top)++].guard = (unsigned char)guard;
    return 1;
}

/* the largest value the plain instruction at body's index k leaves in reg, which it writes: an AND's or a UBFM's of its
   source (made by what alone runs into it), a byte's or a half's load, a MOVZ, a MOV, an ADD of an immediate; ~0 where
   it can't be told */
static SnxU64 made_max(SnxCodegen* codegen, const SnxU64* body, SnxU64 first, const Flow* flow, size_t k, unsigned reg,
                       int depth) {
    SnxU64 source = ~(SnxU64)0, a = body ? body[k] : first + (SnxU64)k * 4;
    SnxU32 w;
    unsigned from = 32, immr, imms, idx;
    if (!snx_codegen_word(codegen, a, &w) || (w & 31) != reg)
        return ~(SnxU64)0; /* a pair's second, a writeback's base */
    immr = (w >> 16) & 63, imms = (w >> 10) & 63, idx = (w >> 10) & 3;
    if ((w & 0x7f800000UL) == 0x12000000UL || (w & 0x7f800000UL) == 0x53000000UL || (w & 0x7f800000UL) == 0x11000000UL)
        from = (w >> 5) & 31;                    /* AND (immediate), UBFM, ADD (immediate) */
    else if ((w & 0x7fe0ffe0UL) == 0x2a0003e0UL) /* MOV */
        from = (w >> 16) & 31;
    else if ((w & 0x7fe00000UL) == 0x52800000UL) /* MOVZ, not shifted */
        return (w >> 5) & 0xffff;
    else if ((w & 0xbfc00000UL) == 0x39400000UL || ((w & 0xbfe00c00UL) == 0x38600800UL) ||
             ((w & 0xbfe00000UL) == 0x38400000UL && idx != 2)) /* LDRB, LDRH */
        return (w >> 30) ? 0xffff : 0xff;
    else
        return ~(SnxU64)0;
    for (unsigned steps = 0; depth < 4 && from < 31 && steps < 32; ++steps) {
        SnxU32 before;
        SnxA64Flow kind;
        /* only what runs into the instruction from the one before it, and that alone */
        if (!k || flow->start[k + 1] - flow->start[k] != 1 || flow->pred[flow->start[k]] != 2 * (k - 1) ||
            !snx_codegen_word(codegen, body ? body[k - 1] : first + (SnxU64)(k - 1) * 4, &before))
            break;
        --k;
        kind = snx_a64_flow(before, codegen->features);
        if (kind == SNX_A64_FLOW_PLAIN && writes_reg(before, from)) {
            source = made_max(codegen, body, first, flow, k, from, depth + 1);
            break;
        }
        if (kind != SNX_A64_FLOW_PLAIN && kind != SNX_A64_FLOW_BC) break;
    }
    if (!(w >> 31) && source > 0xffffffffull) source = 0xffffffffull; /* a W register's */
    if ((w & 0x7f800000UL) == 0x12000000UL) {
        SnxU64 mask;
        if (immr || ((w >> 22) & 1 ? imms == 63 : imms & 0x20)) return source; /* not its low bits alone */
        mask = ((SnxU64)2 << imms) - 1;
        return mask < source ? mask : source;
    }
    if ((w & 0x7f800000UL) == 0x53000000UL) {
        SnxU64 mask;
        if (imms < immr) return ~(SnxU64)0; /* shifted left */
        mask = imms - immr < 63 ? ((SnxU64)2 << (imms - immr)) - 1 : ~(SnxU64)0;
        source >>= immr;
        return mask < source ? mask : source;
    }
    if ((w & 0x7f800000UL) == 0x11000000UL) {
        SnxU64 imm = (w >> 10) & 0xfff;
        if ((w >> 22) & 1) imm <<= 12;
        return from < 31 && source < 0x7fffffffull ? source + imm : ~(SnxU64)0;
    }
    return source;
}

/* The bound of the table of the BR at br that no compare just before its load gives, from its function's flow: every
   path back from the load passes the guard of the default, then the compare of the index with an immediate it guards,
   nothing else setting the flags between; or the index's making on it bounds its value (made_max's; masked then). The
   largest of the paths', 0 where one has none, or where two ways to one instruction have the index in two registers
   (what bounds one says nothing of the other). whole as function_base's */
static unsigned function_bound(SnxCodegen* codegen, const JumpTable* table, const SnxU64* body, SnxU64 first,
                               size_t count, SnxU64 entry, const Flow* flow, int whole, int* masked) {
    size_t top = 0, k, load = body_index(body, first, count, table->load);
    BoundState* stack = (BoundState*)malloc((3 * count + 1) * sizeof(*stack));
    unsigned char* seen = (unsigned char*)calloc(3 * count + 1, 1);
    unsigned bound = 0;
    int ok = stack && seen && load < count && bound_push(stack, &top, seen, load, table->index_reg, 0);
    *masked = 0;
    while (ok && top) {
        BoundState s = stack[--top];
        SnxU64 a = body ? body[s.at] : first + (SnxU64)s.at * 4;
        SnxU32 w = 0;
        SnxA64Flow kind;
        unsigned reg = s.reg;
        if (s.at != load || s.guard) { /* the load itself is past the paths */
            if (!snx_codegen_word(codegen, a, &w)) {
                ok = 0;
                break;
            }
            kind = snx_a64_flow(w, codegen->features);
            if (s.guard) {
                unsigned limit = compare_bound(w, reg);
                if (limit) { /* cmp index, #imm: imm + 1 entries past HI/LS, imm past HS/LO */
                    limit -= s.guard == 2;
                    bound = limit > bound ? limit : bound;
                    continue;
                }
                if (kind == SNX_A64_FLOW_PLAIN && sets_flags(w)) {
                    ok = 0;
                    break;
                }
            }
            if (kind == SNX_A64_FLOW_PLAIN && writes_reg(w, reg)) {
                if ((w & 0xffe0ffe0UL) == 0x2a0003e0UL) /* mov wI, wS: the index was S's before */
                    reg = (w >> 16) & 31;
                else if ((w & 0xfffffc00UL) == 0x92407c00UL) /* and xI, xS, #0xffffffff */
                    reg = (w >> 5) & 31;
                else {
                    SnxU64 max = made_max(codegen, body, first, flow, s.at, reg, 0);
                    if (max >= (body ? SNX_JUMP_TABLE_LOOSE : SNX_JUMP_TABLE_ENTRIES)) {
                        ok = 0;
                        break;
                    }
                    bound = (unsigned)max + 1 > bound ? (unsigned)max + 1 : bound;
                    *masked = 1;
                    continue;
                }
            } else if (kind == SNX_A64_FLOW_BL || kind == SNX_A64_FLOW_BLR || kind == SNX_A64_FLOW_SVC) {
                ok = 0;
                break;
            }
        }
        if (a == entry || (whole && flow->start[s.at] == flow->start[s.at + 1] && w != SNX_A64_NOP))
            ok = 0; /* the caller's index; no known way here */
        for (k = flow->start[s.at]; ok && k < flow->start[s.at + 1]; ++k) {
            size_t at = flow->pred[k] / 2;
            SnxU64 from = body ? body[at] : first + (SnxU64)at * 4;
            unsigned guard = s.guard ? s.guard : bound_guard(codegen, from, (unsigned)(flow->pred[k] & 1));
            ok = bound_push(stack, &top, seen, at, reg, guard);
        }
    }
    free(stack), free(seen);
    return ok ? bound : 0;
}

static void add_table_base(SnxCodegen* codegen, SnxU64 base) {
    size_t at = codegen->table_base_count;
    while (at && codegen->table_bases[at - 1] > base) --at;
    if (at && codegen->table_bases[at - 1] == base) return;
    codegen->table_bases =
        (SnxU64*)snx_realloc(codegen->table_bases, (codegen->table_base_count + 1) * sizeof(*codegen->table_bases));
    memmove(&codegen->table_bases[at + 1], &codegen->table_bases[at],
            (codegen->table_base_count - at) * sizeof(*codegen->table_bases));
    codegen->table_bases[at] = base;
    ++codegen->table_base_count;
}

/* The tables of the function's BRs that the code before their loads doesn't give whole (the base, the bound), from its
   flow: recorded, for every later snx_codegen_jump_table. In rounds, each with the ways to the cases of the tables known
   before it (a switch in another's case, whose base is set before both; a loop's, through its own cases), until nothing
   changes; a base found is looked for again in each, and refused where a way known since takes another. Then again with
   the ways all known: what one nothing leads to (a switch not found, a landing pad) may reach is refused. brs: the
   function's BRs, nbrs of them, NULL where they are to be looked for. Whether any was found */
static int function_tables(SnxCodegen* codegen, const SnxU64* body, SnxU64 first, size_t count, SnxU64 entry,
                           const SnxU64* brs, size_t nbrs) {
    SnxU64* pending = NULL;
    size_t n = 0, capacity = 0;
    int found = 0, changed = 1;
    for (size_t k = 0; k < (brs ? nbrs : count); ++k) {
        SnxU64 br = brs ? brs[k] : body ? body[k] : first + (SnxU64)k * 4;
        JumpTable table;
        SnxU32 word;
        if (!snx_codegen_word(codegen, br, &word) || !is_br(word)) continue;
        if (jump_table_shape(codegen, br, word, &table)
                ? table.absolute || (address_in(codegen, table.load, table.base_reg, 8, 0) &&
                                     table_bound(codegen, table.load, table.index_reg))
                : !table.load || table.absolute)
            continue;
        if (n == capacity) {
            capacity = capacity ? capacity * 2 : 16;
            pending = (SnxU64*)snx_realloc(pending, capacity * sizeof(*pending));
        }
        pending[n++] = br;
    }
    for (int round = 0, whole = 0; n && round < 32; ++round) {
        Flow flow;
        if (!changed && whole++) break;
        changed = 0;
        flow_build(codegen, body, first, count, &flow);
        for (size_t i = 0; i < n; ++i) {
            SnxU64 br = pending[i], base;
            SnxFlowTable* r = recorded(codegen, br);
            JumpTable table;
            SnxU32 word = 0;
            int shaped, masked;
            unsigned bound, max;
            if (r && r->base == SNX_TABLE_REFUSED) continue;
            (void)snx_codegen_word(codegen, br, &word);
            shaped = jump_table_shape(codegen, br, word, &table);
            if (!address_in(codegen, table.load, table.base_reg, 8, 0)) {
                /* no base before the load (or one found further back in the layout, which the flow is surer of) */
                base = function_base(codegen, &table, body, first, count, entry, &flow, whole);
                if (r && r->base && base != r->base) base = SNX_TABLE_REFUSED;
                if (base && (!r || base != r->base)) {
                    r = record_table(codegen, br);
                    r->base = base;
                    changed = 1;
                    if (base != SNX_TABLE_REFUSED) add_table_base(codegen, base);
                }
                shaped = jump_table_shape(codegen, br, word, &table);
            }
            if (!shaped || table_bound(codegen, table.load, table.index_reg)) continue;
            /* a range's table without a compare (the caller's index, a byte from memory, a mask) ends as table_extent
               says; one of an explored function is followed only to a bound */
            bound = function_bound(codegen, &table, body, first, count, entry, &flow, whole, &masked);
            max = masked && !body ? bound : 0;
            if (max) bound = 0;
            if (!bound && body) continue;
            if (!r) r = record_table(codegen, br), changed = 1;
            if (!body && !r->count) r->first = first, r->count = count, changed = 1;
            if (r->bound != bound || r->max != max) r->bound = bound, r->max = max, changed = 1;
        }
        free(flow.start);
        found |= changed;
    }
    free(pending);
    return found;
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
    Addresses result = {0}, todo = {0}, taken = {0}, pending = {0};
    AddressSet seen = {0};
    push(&todo, start);
    if (!limit) limit = 200001;
again:
    while (todo.count && seen.count < limit) {
        SnxU64 address = todo.values[--todo.count];
        for (;;) {
            SnxU32 word;
            SnxA64Insn insn;
            if (set_has(&seen, address) || (address != start && is_boundary(codegen, known, address))) break;
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
                    if (snx_codegen_module(codegen, target) && !is_boundary(codegen, known, target) &&
                        distance < 0x100000UL)
                        push(&todo, target);
                    break;
                } else if (work && snx_codegen_module(codegen, target))
                    push(work, target);
            } else if (kind == SNX_A64_FLOW_BR) {
                SnxU64 targets[SNX_JUMP_TABLE_MAX];
                size_t n = snx_codegen_jump_table(codegen, address, targets, SNX_JUMP_TABLE_MAX);
                for (size_t k = 0; k < n; ++k)
                    if (!is_boundary(codegen, known, targets[k])) push(&todo, targets[k]);
                if (!n) push(&pending, address);
                break;
            } else if (kind == SNX_A64_FLOW_RET || kind == SNX_A64_FLOW_BRK)
                break;
            else if (work && inner && kind == SNX_A64_FLOW_PLAIN) {
                SnxU64 target = taken_address(codegen, address, word);
                if (target && !(target & 3) && snx_codegen_module(codegen, target)) push(&taken, target);
            }
            address += 4;
            if (seen.count >= limit) break;
        }
    }
    /* the BRs whose table's base the code before them didn't give: from the whole function explored so far, whose
       new cases may give the others theirs */
    if (seen.count < limit && pending.count) {
        int found = 0;
        SnxU64* sorted = (SnxU64*)snx_alloc(result.count * sizeof(*sorted) + 1);
        memcpy(sorted, result.values, result.count * sizeof(*sorted));
        qsort(sorted, result.count, sizeof(*sorted), snx_compare_u64);
        (void)function_tables(codegen, sorted, 0, result.count, start, NULL, 0);
        for (size_t i = pending.count; i-- > 0;) {
            SnxU64 targets[SNX_JUMP_TABLE_MAX];
            size_t n = snx_codegen_jump_table(codegen, pending.values[i], targets, SNX_JUMP_TABLE_MAX);
            if (!n) continue;
            for (size_t k = 0; k < n; ++k)
                if (!is_boundary(codegen, known, targets[k])) push(&todo, targets[k]);
            pending.values[i] = pending.values[--pending.count];
            found |= n != 0;
        }
        free(sorted);
        if (found) goto again;
    }
    free(pending.values);
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

/* the leader for an address a function reaches indirectly (a jump table's case) in whichever function holds it: a
   function explored without unwind information stops before it, and the JIT's pieces can split it from the branch */
static void mark_entry(SnxCodegen* codegen, SnxU64 address) {
    size_t lo = 0, hi = codegen->function_count;
    while (lo < hi) {
        size_t middle = lo + (hi - lo) / 2;
        if (codegen->functions[middle].start <= address)
            lo = middle + 1;
        else
            hi = middle;
    }
    if (lo) mark_leader(&codegen->functions[lo - 1], address);
}

/* the bases of the tables of the program's BRs that the code before their loads gives (function_tables adds the
   others') */
static void table_bases(SnxCodegen* codegen, const Addresses* brs) {
    size_t n = 0, k = 0;
    codegen->table_bases = (SnxU64*)snx_realloc(codegen->table_bases, (brs->count + 1) * sizeof(SnxU64));
    for (size_t i = 0; i < brs->count; ++i) {
        JumpTable table;
        SnxU32 word;
        if (snx_codegen_word(codegen, brs->values[i], &word) && jump_table_shape(codegen, brs->values[i], word, &table))
            codegen->table_bases[n++] = table.base;
    }
    if (n) qsort(codegen->table_bases, n, sizeof(SnxU64), snx_compare_u64);
    for (size_t i = 0; i < n; ++i)
        if (!k || codegen->table_bases[k - 1] != codegen->table_bases[i])
            codegen->table_bases[k++] = codegen->table_bases[i];
    codegen->table_base_count = k;
}

static void leaders(SnxCodegen* codegen, const AddressSet* extra, const AddressSet* hints) {
    Addresses brs = {0}; /* every function's BRs, the functions' in order, and where each one's start */
    size_t* first = (size_t*)snx_alloc((codegen->function_count + 1) * sizeof(*first));
    /* every function's first: a jump table's case can lie in a function after its BR's (mark_entry) */
    for (size_t i = 0; i < codegen->function_count; ++i) {
        SnxFunction* f = &codegen->functions[i];
        f->leaders = (unsigned char*)snx_alloc(f->count);
        memset(f->leaders, 0, f->count);
    }
    for (size_t i = 0; i < codegen->function_count; ++i) {
        SnxFunction* f = &codegen->functions[i];
        first[i] = brs.count;
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
            /* where a thread goes on through dispatch: after a call, and after an SVC it was stopped in (a save
               state's threads start again from there) */
            if (kind == SNX_A64_FLOW_BL || kind == SNX_A64_FLOW_BLR || kind == SNX_A64_FLOW_SVC)
                mark_leader(f, address + 4);
            if (kind == SNX_A64_FLOW_BR && is_br(word)) push(&brs, address);
            previous_terminal = kind == SNX_A64_FLOW_B || kind == SNX_A64_FLOW_BR || kind == SNX_A64_FLOW_RET ||
                                kind == SNX_A64_FLOW_BRK || kind == SNX_A64_FLOW_UNDEF;
        }
        codegen->instruction_count += f->count;
    }
    table_bases(codegen, &brs);
    /* a function of an unwind range, which explore didn't see: its tables from its flow */
    for (size_t i = 0; i < codegen->function_count; ++i) {
        const SnxFunction* f = &codegen->functions[i];
        size_t n = (i + 1 < codegen->function_count ? first[i + 1] : brs.count) - first[i];
        if (n) (void)function_tables(codegen, f->addresses, f->start, f->count, f->start, brs.values + first[i], n);
    }
    for (size_t i = 0; i < brs.count; ++i) {
        SnxU64 targets[SNX_JUMP_TABLE_MAX];
        size_t n = snx_codegen_jump_table(codegen, brs.values[i], targets, SNX_JUMP_TABLE_MAX);
        for (size_t k = 0; k < n; ++k) mark_entry(codegen, targets[k]);
    }
    free(brs.values);
    free(first);
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
    free(codegen->read_block);
    free(codegen->flow_tables);
    free(codegen->table_bases);
    *codegen = (SnxCodegen){0};
}
