/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/loader/relocate.h"
#include "common/cpu/recompiler/llvm/elf_defs.h"
#include "common/utilities/endian.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ELF64's sizes: a symbol, a RELA entry, a dynamic entry, a DT_RELR word */
#define SYM_SIZE           24
#define RELA_SIZE          24
#define DYN_SIZE           16
#define WORD               8
/* R_AARCH64_ABS32 by its number: the console's linker gives it R_AARCH64_ABS64's case */
#define SNX_RELOCATE_ABS32 258

/* a definition of a module: its name (in the image), its value and its symbol's place in the table */
typedef struct {
    const char* name;
    SnxU64 value, index;
    unsigned char info;
} Def;

/* a module's dynamic tables, as offsets from its start, and its definitions in the order of their names */
typedef struct {
    SnxU64 rela, relasz, jmprel, pltrelsz, relr, relrsz, symtab, strtab, strsz, symbols, init, fini;
    Def* defs;
    size_t def_count;
} Tables;

/* a relocation's kind: its word plus the base, the base plus its addend, a symbol's address plus its addend in the
   RELA table and in the PLT's, and a type with no case */
enum { RELR, RELATIVE, SYMBOLIC, SLOT, OTHER };
typedef void (*Each)(void* user, size_t module, int kind, SnxU64 target, SnxU64 symbol, SnxU64 addend);

static int fail(char* error, size_t error_size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    if (error_size) vsnprintf(error, error_size, format, args);
    va_end(args);
    return -1;
}
static int inside(SnxU64 offset, SnxU64 length, SnxU64 total) { return offset <= total && length <= total - offset; }

static int def_order(const void* a, const void* b) {
    const Def *x = (const Def*)a, *y = (const Def*)b;
    int names = strcmp(x->name, y->name);
    if (names) return names;
    return x->index < y->index ? -1 : x->index > y->index;
}

/* the module's tables: none (all 0) for a module without MOD0. 0, or -1 said in error */
static int read_tables(const SnxRelocModule* m, Tables* t, char* error, size_t error_size) {
    SnxU64 mod0, at, tag, value, hash = 0;
    int have_hash = 0;
    memset(t, 0, sizeof(*t));
    if (m->size < 8) return 0;
    mod0 = snx_ld_le32(m->image + 4);
    if (!inside(mod0, 8, m->size) || memcmp(m->image + mod0, "MOD0", 4)) return 0;
    at = mod0 + (SnxU64)(int32_t)snx_ld_le32(m->image + mod0 + 4); /* a signed displacement */
    for (;; at += DYN_SIZE) {
        if (!inside(at, DYN_SIZE, m->size))
            return fail(error, error_size, "%s: the dynamic table runs past the module's %llu bytes", m->name,
                        (unsigned long long)m->size);
        tag = snx_ld_le64(m->image + at);
        value = snx_ld_le64(m->image + at + 8);
        if (tag == DT_NULL) break;
        if (tag == DT_RELA && !t->rela) t->rela = value;
        if (tag == DT_RELASZ && !t->relasz) t->relasz = value;
        if (tag == DT_JMPREL && !t->jmprel) t->jmprel = value;
        if (tag == DT_PLTRELSZ && !t->pltrelsz) t->pltrelsz = value;
        if (tag == DT_RELR && !t->relr) t->relr = value;
        if (tag == DT_RELRSZ && !t->relrsz) t->relrsz = value;
        if (tag == DT_SYMTAB && !t->symtab) t->symtab = value;
        if (tag == DT_STRTAB && !t->strtab) t->strtab = value;
        if (tag == DT_STRSZ && !t->strsz) t->strsz = value;
        if (tag == DT_HASH && !have_hash) hash = value, have_hash = 1;
        if (tag == DT_INIT && !t->init) t->init = value;
        if (tag == DT_FINI && !t->fini) t->fini = value;
    }
    /* the number of symbols: DT_HASH's chain count, else the room between the symbols and the strings after them */
    if (have_hash && inside(hash, 8, m->size))
        t->symbols = snx_ld_le32(m->image + hash + 4);
    else if (t->symtab && t->strtab > t->symtab)
        t->symbols = (t->strtab - t->symtab) / SYM_SIZE;
    if (t->relasz % RELA_SIZE || t->pltrelsz % RELA_SIZE || t->relrsz % WORD || !inside(t->rela, t->relasz, m->size) ||
        !inside(t->jmprel, t->pltrelsz, m->size) || !inside(t->relr, t->relrsz, m->size))
        return fail(error, error_size,
                    "%s: a relocation table (RELA %llu bytes at %#llx, JMPREL %llu at %#llx, RELR %llu at %#llx) is no "
                    "table of whole entries inside the module's %llu bytes",
                    m->name, (unsigned long long)t->relasz, (unsigned long long)t->rela,
                    (unsigned long long)t->pltrelsz, (unsigned long long)t->jmprel, (unsigned long long)t->relrsz,
                    (unsigned long long)t->relr, (unsigned long long)m->size);
    if (t->symbols > m->size / SYM_SIZE || !inside(t->symtab, t->symbols * SYM_SIZE, m->size) ||
        !inside(t->strtab, t->strsz, m->size))
        return fail(
            error, error_size,
            "%s: the symbols (%llu at %#llx) or their names (%llu bytes at %#llx) lie outside the module's %llu "
            "bytes",
            m->name, (unsigned long long)t->symbols, (unsigned long long)t->symtab, (unsigned long long)t->strsz,
            (unsigned long long)t->strtab, (unsigned long long)m->size);
    return 0;
}

/* the name of the module's symbol `index`, or NULL for one outside the table or the strings */
static const char* symbol_name(const SnxRelocModule* m, const Tables* t, SnxU64 index) {
    SnxU64 name;
    if (index >= t->symbols) return NULL;
    name = snx_ld_le32(m->image + t->symtab + index * SYM_SIZE);
    if (name >= t->strsz || !memchr(m->image + t->strtab + name, 0, (size_t)(t->strsz - name))) return NULL;
    return (const char*)m->image + t->strtab + name;
}

/* the module's definitions into defs (room for every symbol of it), sorted by name: how many */
static size_t read_defs(const SnxRelocModule* m, const Tables* t, Def* defs) {
    size_t n = 0;
    SnxU64 i;
    for (i = 1; i < t->symbols; ++i) {
        const unsigned char* s = m->image + t->symtab + i * SYM_SIZE;
        unsigned section = snx_ld_le16(s + 6);
        const char* name = symbol_name(m, t, i);
        if (!name || !name[0] || section == SHN_UNDEF || section == SHN_COMMON || ELF64_ST_TYPE(s[4]) == STT_FILE)
            continue;
        defs[n].name = name;
        defs[n].value = snx_ld_le64(s + 8);
        defs[n].index = i;
        defs[n].info = s[4];
        ++n;
    }
    if (n) qsort(defs, n, sizeof(*defs), def_order);
    return n;
}

/* the module's definition of name, the first of the table when it has several, or NULL */
static const Def* find(const Tables* t, const char* name) {
    size_t lo = 0, hi = t->def_count;
    while (lo < hi) {
        size_t middle = lo + (hi - lo) / 2;
        if (strcmp(t->defs[middle].name, name) < 0)
            lo = middle + 1;
        else
            hi = middle;
    }
    return lo < t->def_count && !strcmp(t->defs[lo].name, name) ? &t->defs[lo] : NULL;
}

/* every relocation of the module, in its tables' order: DT_RELR, DT_RELA, DT_JMPREL */
static void walk(const SnxRelocModule* m, const Tables* t, size_t module, Each each, void* user) {
    SnxU64 at, where = 0;
    for (at = t->relr; at < t->relr + t->relrsz; at += WORD) {
        SnxU64 entry = snx_ld_le64(m->image + at);
        unsigned bit;
        if (!(entry & 1)) { /* an address, then the bitmaps of the 63 words after it each */
            each(user, module, RELR, entry, 0, 0);
            where = entry + WORD;
            continue;
        }
        for (bit = 1; bit < 64; ++bit)
            if ((entry >> bit) & 1) each(user, module, RELR, where + (SnxU64)(bit - 1) * WORD, 0, 0);
        where += 63 * WORD;
    }
    for (at = t->rela; at < t->rela + t->relasz; at += RELA_SIZE) {
        SnxU64 target = snx_ld_le64(m->image + at), info = snx_ld_le64(m->image + at + 8);
        SnxU64 addend = snx_ld_le64(m->image + at + 16), type = info & 0xFFFFFFFFu;
        int kind = type == R_AARCH64_RELATIVE                                                            ? RELATIVE
                   : type == R_AARCH64_ABS64 || type == SNX_RELOCATE_ABS32 || type == R_AARCH64_GLOB_DAT ? SYMBOLIC
                                                                                                         : OTHER;
        each(user, module, kind, target, info >> 32, addend);
    }
    for (at = t->jmprel; at < t->jmprel + t->pltrelsz; at += RELA_SIZE) {
        SnxU64 info = snx_ld_le64(m->image + at + 8);
        each(user, module, (info & 0xFFFFFFFFu) == R_AARCH64_JUMP_SLOT ? SLOT : OTHER, snx_ld_le64(m->image + at),
             info >> 32, snx_ld_le64(m->image + at + 16));
    }
}

/* where the word at target of the module is relocated, or NULL for one that is not all in its data */
static unsigned char* word_at(const SnxRelocModule* m, SnxU64 target) {
    if (target < m->data_off || !inside(target - m->data_off, WORD, m->data_size)) return NULL;
    return m->data + (target - m->data_off);
}

/* refused: the name of the first symbol that a closed module's definition would take, of module refused_in, the
   closed one refused_by */
typedef struct {
    SnxRelocModule* modules;
    const Tables* tables;
    size_t count;
    SnxRelocCounts* counts;
    const char* refused;
    size_t refused_in, refused_by;
} Linker;

/* The address of the module's symbol: 1 with it in *address, or 0 when nothing defines it, *weak saying whether it
   is weak and *name its name (NULL for a symbol outside the table) */
static int resolve(Linker* l, size_t module, SnxU64 symbol, SnxU64* address, int* weak, const char** name) {
    const SnxRelocModule* m = &l->modules[module];
    const Tables* t = &l->tables[module];
    const unsigned char* s;
    const Def* def;
    size_t i;
    *weak = 0;
    *name = symbol_name(m, t, symbol);
    if (!*name) return 0;
    s = m->image + t->symtab + symbol * SYM_SIZE;
    *weak = ELF64_ST_BIND(s[4]) == STB_WEAK;
    if (s[5] & 3) { /* protected or hidden: the module's own */
        def = find(t, *name);
        if (def) *address = m->base + def->value;
        return def != NULL;
    }
    for (i = 0; i < l->count; ++i) {
        def = find(&l->tables[i], *name);
        if (!def || ELF64_ST_BIND(def->info) == STB_LOCAL) continue;
        if (l->modules[i].closed && i != module) {
            if (!l->refused) l->refused = *name, l->refused_in = module, l->refused_by = i;
            return 0;
        }
        *address = l->modules[i].base + def->value;
        return 1;
    }
    return 0;
}

static void link_one(void* user, size_t module, int kind, SnxU64 target, SnxU64 symbol, SnxU64 addend) {
    Linker* l = (Linker*)user;
    SnxRelocModule* m = &l->modules[module];
    unsigned char* p = word_at(m, target);
    SnxU64 address = 0;
    const char* name;
    int weak;
    if (kind == OTHER) {
        ++l->counts->other;
        return;
    }
    if (!p) {
        ++l->counts->outside;
        return;
    }
    if (kind == RELR || kind == RELATIVE) {
        snx_st_le64(p, m->base + (kind == RELR ? snx_ld_le64(p) : addend));
        ++l->counts->relative;
        return;
    }
    if (kind == SLOT && !m->lazy) m->lazy = m->base + snx_ld_le64(p);
    if (resolve(l, module, symbol, &address, &weak, &name)) {
        snx_st_le64(p, address + addend);
        ++*(kind == SLOT ? &l->counts->slots : &l->counts->symbolic);
    } else if (weak) {
        snx_st_le64(p, addend);
        ++l->counts->absent;
    } else {
        if (kind == SLOT) snx_st_le64(p, m->base + snx_ld_le64(p));
        if (!l->counts->unresolved++) l->counts->first_unresolved = name;
    }
}

/* a module of an earlier load, once another was added: a symbol of it that binds to the added module's definition */
static void later_one(void* user, size_t module, int kind, SnxU64 target, SnxU64 symbol, SnxU64 addend) {
    Linker* l = (Linker*)user;
    SnxRelocModule* m = &l->modules[module];
    const SnxRelocModule* added = &l->modules[l->count - 1];
    const char* name = symbol_name(m, &l->tables[module], symbol);
    const Def* def;
    unsigned char* p;
    SnxU64 address = 0;
    int weak;
    if ((kind != SYMBOLIC && kind != SLOT) || !name) return;
    def = find(&l->tables[l->count - 1], name);
    p = word_at(m, target);
    if (!def || !p || !resolve(l, module, symbol, &address, &weak, &name) || address != added->base + def->value)
        return;
    snx_st_le64(p, address + addend);
    ++l->counts->earlier;
}

/* every module's tables with their definitions, in one block to free: NULL said in error */
static Tables* read_all(const SnxRelocModule* modules, size_t count, char* error, size_t error_size) {
    Tables* tables;
    Tables one;
    Def* defs;
    SnxU64 symbols = 0;
    size_t i, used = 0;
    for (i = 0; i < count; ++i) {
        if (read_tables(&modules[i], &one, error, error_size)) return NULL;
        symbols += one.symbols;
    }
    tables = (Tables*)snx_alloc((count ? count : 1) * sizeof(*tables) + (size_t)symbols * sizeof(*defs));
    defs = (Def*)(tables + (count ? count : 1));
    for (i = 0; i < count; ++i) {
        read_tables(&modules[i], &tables[i], error, error_size); /* read once already */
        tables[i].defs = defs + used;
        tables[i].def_count = read_defs(&modules[i], &tables[i], tables[i].defs);
        used += tables[i].def_count;
    }
    return tables;
}
/* links the modules from `first` on, then gives the ones before it what the last defines when `first` is not 0 */
static int link_from(SnxRelocModule* modules, size_t count, size_t first, SnxRelocCounts* counts, char* error,
                     size_t error_size) {
    Tables* tables;
    Linker l = {0};
    size_t i;
    int rc = 0;
    memset(counts, 0, sizeof(*counts));
    tables = read_all(modules, count, error, error_size);
    if (!tables) return -1;
    l.modules = modules;
    l.tables = tables;
    l.count = count;
    l.counts = counts;
    for (i = first; i < count; ++i) {
        modules[i].lazy = 0;
        modules[i].init = tables[i].init ? modules[i].base + tables[i].init : 0;
        modules[i].fini = tables[i].fini ? modules[i].base + tables[i].fini : 0;
    }
    for (i = first; i < count; ++i) walk(&modules[i], &tables[i], i, link_one, &l);
    for (i = 0; i < first; ++i) walk(&modules[i], &tables[i], i, later_one, &l);
    if (l.refused)
        rc = fail(error, error_size, "%s binds %s to %s's definition, a module loaded for no other to bind to",
                  modules[l.refused_in].name, l.refused, modules[l.refused_by].name);
    free(tables);
    return rc;
}

int snx_relocate(SnxRelocModule* modules, size_t count, SnxRelocCounts* counts, char* error, size_t error_size) {
    return link_from(modules, count, 0, counts, error, error_size);
}

int snx_relocate_added(SnxRelocModule* modules, size_t count, SnxRelocCounts* counts, char* error, size_t error_size) {
    return link_from(modules, count, count ? count - 1 : 0, counts, error, error_size);
}

typedef struct {
    const SnxRelocModule* modules;
    size_t gone;
    SnxU64* words;
} Reverter;

static void revert_one(void* user, size_t module, int kind, SnxU64 target, SnxU64 symbol, SnxU64 addend) {
    Reverter* r = (Reverter*)user;
    const SnxRelocModule *m = &r->modules[module], *gone = &r->modules[r->gone];
    unsigned char* p = word_at(m, target);
    (void)symbol, (void)addend;
    if ((kind != SYMBOLIC && kind != SLOT) || !p || snx_ld_le64(p) - gone->base >= gone->size) return;
    snx_st_le64(p, kind == SLOT ? m->lazy : 0);
    ++*r->words;
}

int snx_relocate_removed(const SnxRelocModule* modules, size_t count, size_t gone, SnxU64* words, char* error,
                         size_t error_size) {
    Reverter r;
    size_t i;
    *words = 0;
    r.modules = modules;
    r.gone = gone;
    r.words = words;
    for (i = 0; i < count; ++i) {
        Tables t;
        if (i == gone) continue;
        if (read_tables(&modules[i], &t, error, error_size)) return -1;
        walk(&modules[i], &t, i, revert_one, &r);
    }
    return 0;
}

typedef struct {
    const SnxRelocModule* modules;
    SnxRelocDiff* diff;
} Comparer;

static void compare_one(void* user, size_t module, int kind, SnxU64 target, SnxU64 symbol, SnxU64 addend) {
    Comparer* c = (Comparer*)user;
    const SnxRelocModule* m = &c->modules[module];
    const unsigned char* p = word_at(m, target);
    SnxU64 has, wants;
    (void)symbol, (void)addend;
    if (kind == OTHER || !p || !inside(target, WORD, m->size)) return;
    has = snx_ld_le64(m->image + target);
    wants = snx_ld_le64(p);
    ++c->diff->words;
    if (has == wants) return;
    if (kind == SLOT && m->lazy && has == m->lazy) {
        ++c->diff->lazy;
        return;
    }
    if (!c->diff->differ++) {
        c->diff->module = module;
        c->diff->at = target;
        c->diff->has = has;
        c->diff->wants = wants;
    }
}

int snx_relocate_compare(const SnxRelocModule* modules, size_t count, SnxRelocDiff* diff, char* error,
                         size_t error_size) {
    Comparer c;
    size_t i;
    memset(diff, 0, sizeof(*diff));
    c.modules = modules;
    c.diff = diff;
    for (i = 0; i < count; ++i) {
        Tables t;
        if (read_tables(&modules[i], &t, error, error_size)) return -1;
        walk(&modules[i], &t, i, compare_one, &c);
    }
    return 0;
}

/* the module's first symbol that defines name, as the sorted definitions give it, or NULL */
static const unsigned char* defined(const SnxRelocModule* m, const Tables* t, const char* name) {
    SnxU64 k;
    for (k = 1; k < t->symbols; ++k) {
        const unsigned char* s = m->image + t->symtab + k * SYM_SIZE;
        unsigned section = snx_ld_le16(s + 6);
        const char* own = symbol_name(m, t, k);
        if (own && !strcmp(own, name) && section != SHN_UNDEF && section != SHN_COMMON &&
            ELF64_ST_TYPE(s[4]) != STT_FILE)
            return s;
    }
    return NULL;
}

int snx_relocate_find(const SnxRelocModule* modules, size_t count, size_t first, const char* name, SnxU64* address,
                      char* error, size_t error_size) {
    size_t i;
    for (i = first; i < count; ++i) {
        const SnxRelocModule* m = &modules[i];
        const unsigned char* s;
        Tables t;
        if (read_tables(m, &t, error, error_size)) return -1;
        s = defined(m, &t, name);
        if (!s || ELF64_ST_BIND(s[4]) == STB_LOCAL) continue; /* a local one is the module's answer: the next's */
        *address = m->base + snx_ld_le64(s + 8);
        return 1;
    }
    return 0;
}

int snx_relocate_own(const SnxRelocModule* module, const char* name, SnxU64* address, char* error, size_t error_size) {
    const unsigned char* s;
    Tables t;
    if (read_tables(module, &t, error, error_size)) return -1;
    s = defined(module, &t, name);
    if (s) *address = module->base + snx_ld_le64(s + 8);
    return s != NULL;
}
