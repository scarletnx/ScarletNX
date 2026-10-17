/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The JIT loader's COFF objects (code_loader.h): Windows' x86-64 objects, as LLVM writes them, placed in the code arena
   and relocated there, by the rules link_coff.c links a module by. The structures are read byte by byte, so this
   builds anywhere and a test can load Windows objects into any host's arena; only a Windows host registers the
   function table and runs the code.

   A load's parts go into the arena's regions: [code] [read-only data and unwind data, the function table, import
   slots] [data, bss]. Symbols resolve to the load's own definitions, then to the loader's table (earlier loads, defined
   names). A COMDAT section whose key an earlier load or object already defines is left out, and its symbols resolve
   to that definition (LLVM puts floating-point constants in COMDATs named after their value, __real@...). An
   __imp_X reference (a dllimport declaration) gets a slot holding X's address. ADDR32NB values (the function table's,
   the unwind data's) are offsets from the arena's start, which RtlAddFunctionTable is given as their image base: the
   arena is at most 2 GiB, so they fit. A REL32 reaches anything in the arena. A call or jump (E8, E9 before the field)
   to a table symbol outside it goes through a stub after the load's code, jmp [rip + slot] with its slot among the
   import slots, as code_loader.c's x86-64 stubs do. Data outside the arena is reached through .refptr pointers, which
   MinGW code uses for it. JIT code calls the runtime library loaded first, and the AOT module through absolute
   addresses */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* snprintf */
#endif
#include "common/cpu/recompiler/llvm/code_loader_int.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define SCN_CODE          0x00000020u
#define SCN_UNINITIALIZED 0x00000080u
#define SCN_LNK_INFO      0x00000200u
#define SCN_LNK_REMOVE    0x00000800u
#define SCN_LNK_COMDAT    0x00001000u
#define SCN_ALIGN_MASK    0x00F00000u
#define SCN_NRELOC_OVFL   0x01000000u
#define SCN_WRITE         0x80000000u
#define SYM_EXTERNAL      2
#define SYM_STATIC        3
#define SYM_WEAK_EXTERNAL 105
#define SEL_ASSOCIATIVE   5
#define REL_ABSOLUTE      0
#define REL_ADDR64        1
#define REL_ADDR32        2
#define REL_ADDR32NB      3
#define REL_REL32         4
#define REL_REL32_5       9
#define REL_SECREL        11
#define CODE_LINE         64 /* where each load's code starts, as code_loader.c's */
#define STUB_SIZE         8  /* jmp qword ptr [rip + slot], int3, int3 */

/* where a loaded section goes */
enum { K_TEXT, K_RODATA, K_PDATA, K_DATA, K_BSS, KINDS };

typedef struct {
    char name[64];
    uint32_t flags, size, raw, relocs, nrelocs;
    int kind;      /* K_*, -1 when not loaded */
    int selection; /* a COMDAT's, from its section symbol */
    uint32_t associate;
    uint64_t position, address; /* in its kind, in the arena */
} Sec;

typedef struct {
    const SnxLinkInput* input;
    const unsigned char* symbols;
    uint32_t nsymbols, nsections;
    const char* strings;
    uint32_t strings_size;
    Sec* sections; /* 1-based */
} Obj;

typedef struct {
    Obj* objs;
    size_t count;
    SnxLinkNames globals; /* the load's: name -> object << 32 | symbol index */
    SnxLinkNames imports; /* "__imp_X", or "@address" of a stub's table symbol -> slot */
    SnxLinkNames stubs;   /* "@address" -> stub */
    size_t import_count, stub_count;
    uint64_t base, import_address, stub_address;
    uint64_t arena_end;
} Load;

static uint16_t rd16(const unsigned char* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const unsigned char* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t rd64(const unsigned char* p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static void wr32(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static const unsigned char* symbol(const Obj* o, uint32_t index) { return o->symbols + 18 * (size_t)index; }
static void symbol_name(const Obj* o, const unsigned char* s, char* out, size_t size) {
    size_t n = 0;
    if (!rd32(s)) {
        uint32_t offset = rd32(s + 4);
        snprintf(out, size, "%s", offset < o->strings_size ? o->strings + offset : "");
        return;
    }
    while (n < 8 && s[n]) ++n;
    if (n >= size) n = size - 1;
    memcpy(out, s, n);
    out[n] = 0;
}

static int classify(const Sec* s) {
    if (s->flags & (SCN_LNK_REMOVE | SCN_LNK_INFO)) return -1;
    if (!strncmp(s->name, ".debug", 6) || !strncmp(s->name, ".llvm", 5)) return -1;
    if (!strcmp(s->name, ".pdata") || !strncmp(s->name, ".pdata$", 7)) return K_PDATA;
    if (s->flags & SCN_CODE) return K_TEXT;
    if (s->flags & SCN_UNINITIALIZED) return K_BSS;
    if (s->flags & SCN_WRITE) return K_DATA;
    return K_RODATA;
}

static int parse(Obj* o, const SnxLinkInput* input, char* error, size_t error_size) {
    const unsigned char* d = input->data;
    uint32_t i, table;
    o->input = input;
    if (input->size < 20 || rd16(d) != SNX_CODE_COFF_X64 || rd16(d + 16)) {
        snprintf(error, error_size, "%s: not an x86-64 COFF object", input->name);
        return -1;
    }
    o->nsections = rd16(d + 2);
    table = rd32(d + 8);
    o->nsymbols = rd32(d + 12);
    if (20 + 40 * (size_t)o->nsections > input->size || (size_t)table + 18 * (size_t)o->nsymbols + 4 > input->size) {
        snprintf(error, error_size, "%s: truncated COFF object", input->name);
        return -1;
    }
    o->symbols = d + table;
    o->strings = (const char*)d + table + 18 * (size_t)o->nsymbols;
    o->strings_size = rd32((const unsigned char*)o->strings);
    o->sections = (Sec*)calloc(o->nsections + 1, sizeof(Sec));
    if (!o->sections) {
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    for (i = 1; i <= o->nsections; ++i) {
        Sec* s = &o->sections[i];
        const unsigned char* h = d + 20 + 40 * (size_t)(i - 1);
        if (h[0] == '/') {
            unsigned long offset = strtoul((const char*)h + 1, NULL, 10);
            snprintf(s->name, sizeof(s->name), "%s", offset < o->strings_size ? o->strings + offset : "");
        } else {
            memcpy(s->name, h, 8);
            s->name[8] = 0;
        }
        s->size = rd32(h + 16);
        s->raw = rd32(h + 20);
        s->relocs = rd32(h + 24);
        s->nrelocs = rd16(h + 32);
        s->flags = rd32(h + 36);
        if ((s->flags & SCN_NRELOC_OVFL) && (size_t)s->relocs + 10 <= input->size) {
            s->nrelocs = rd32(d + s->relocs) - 1; /* the first relocation holds the count */
            s->relocs += 10;
        }
        if (!(s->flags & SCN_UNINITIALIZED) && (size_t)s->raw + s->size > input->size) {
            snprintf(error, error_size, "%s: truncated section %s", input->name, s->name);
            return -1;
        }
        if ((size_t)s->relocs + 10 * (size_t)s->nrelocs > input->size) {
            snprintf(error, error_size, "%s: truncated relocations in %s", input->name, s->name);
            return -1;
        }
        s->kind = classify(s);
    }
    return 0;
}

/* COMDATs: a key an earlier load or object defines leaves this copy out, and associative sections follow theirs */
static void comdats(const SnxCodeLoader* loader, Load* ld) {
    SnxLinkNames keys;
    size_t i, found;
    uint32_t j;
    char name[256];
    int changed;
    memset(&keys, 0, sizeof(keys));
    for (i = 0; i < ld->count; ++i) {
        Obj* o = &ld->objs[i];
        for (j = 0; j < o->nsymbols; j += 1 + o->symbols[18 * (size_t)j + 17]) {
            const unsigned char* s = symbol(o, j);
            int16_t section = (int16_t)rd16(s + 12);
            Sec* sec;
            if (section <= 0 || (uint32_t)section > o->nsections) continue;
            sec = &o->sections[section];
            if (!(sec->flags & SCN_LNK_COMDAT) || sec->kind < 0) continue;
            if (s[16] == SYM_STATIC && s[17] && !rd32(s + 8) && !sec->selection) {
                /* the section symbol: its auxiliary record has the selection and the associated section */
                sec->selection = s[18 + 14] ? s[18 + 14] : 2;
                sec->associate = rd16(s + 18 + 12);
                continue;
            }
            if (sec->selection && sec->selection != SEL_ASSOCIATIVE && s[16] == SYM_EXTERNAL) {
                symbol_name(o, s, name, sizeof(name));
                if (snx_link_find(&loader->symbols, name, &found) || snx_link_find(&keys, name, &found))
                    sec->kind = -1;
                else
                    snx_link_insert(&keys, name, i);
            }
        }
    }
    do {
        changed = 0;
        for (i = 0; i < ld->count; ++i)
            for (j = 1; j <= ld->objs[i].nsections; ++j) {
                Sec* s = &ld->objs[i].sections[j];
                if (s->selection == SEL_ASSOCIATIVE && s->kind >= 0 && s->associate &&
                    s->associate <= ld->objs[i].nsections && ld->objs[i].sections[s->associate].kind < 0) {
                    s->kind = -1;
                    changed = 1;
                }
            }
    } while (changed);
    snx_link_names_free(&keys);
}

/* the load's external definitions in loaded sections; one the loader's table has already is an error, unless it is
   a COMDAT's (left out above) */
static int collect_globals(const SnxCodeLoader* loader, Load* ld, char* error, size_t error_size) {
    size_t i, found;
    uint32_t j;
    char name[256];
    for (i = 0; i < ld->count; ++i) {
        const Obj* o = &ld->objs[i];
        for (j = 0; j < o->nsymbols; j += 1 + o->symbols[18 * (size_t)j + 17]) {
            const unsigned char* s = symbol(o, j);
            int16_t section = (int16_t)rd16(s + 12);
            if (s[16] != SYM_EXTERNAL || section <= 0 || (uint32_t)section > o->nsections ||
                o->sections[section].kind < 0)
                continue;
            symbol_name(o, s, name, sizeof(name));
            if (snx_link_find(&ld->globals, name, &found) || snx_link_find(&loader->symbols, name, &found)) {
                snprintf(error, error_size, "duplicate symbol %s (%s)", name, o->input->name);
                return -1;
            }
            snx_link_insert(&ld->globals, name, (i << 32) | j);
        }
    }
    return 0;
}

/* The address of object's symbol index. *section_address: the start of the section it is in (0 for none), for SECREL */
static int resolve(const SnxCodeLoader* loader, const Load* ld, size_t object, uint32_t index, uint64_t* value,
                   uint64_t* section_address, char* error, size_t error_size) {
    const Obj* o = &ld->objs[object];
    const unsigned char* s;
    int16_t section;
    size_t found;
    char name[256];
    int depth;
    *section_address = 0;
    for (depth = 0; depth < 8; ++depth) {
        if (index >= o->nsymbols) {
            snprintf(error, error_size, "%s: bad symbol index", o->input->name);
            return -1;
        }
        s = symbol(o, index);
        section = (int16_t)rd16(s + 12);
        symbol_name(o, s, name, sizeof(name));
        if (section > 0) {
            const Sec* sec;
            if ((uint32_t)section > o->nsections) {
                snprintf(error, error_size, "%s: bad section number", o->input->name);
                return -1;
            }
            sec = &o->sections[section];
            if (sec->kind >= 0) {
                *value = sec->address + rd32(s + 8);
                *section_address = sec->address;
                return 0;
            }
            /* a COMDAT copy left out: its symbols are the kept copy's */
            if (s[16] != SYM_EXTERNAL) {
                snprintf(error, error_size, "%s: reference to section %s, which is not loaded", o->input->name,
                         sec->name);
                return -1;
            }
        } else if (section == -1) {
            *value = rd32(s + 8);
            return 0;
        }
        if (snx_link_find(&ld->globals, name, &found) && (found >> 32 != object || (uint32_t)found != index)) {
            object = found >> 32;
            index = (uint32_t)found;
            o = &ld->objs[object];
            continue;
        }
        if (snx_code_find(loader, name, value)) return 0;
        if (s[16] == SYM_WEAK_EXTERNAL && s[17]) {
            index = rd32(s + 18); /* the default definition */
            continue;
        }
        if (snx_link_find(&ld->imports, name, &found)) {
            *value = ld->import_address + 8 * (uint64_t)found;
            return 0;
        }
        if (!strcmp(name, "__ImageBase")) {
            *value = ld->base;
            return 0;
        }
        if (!strcmp(name, "_fltused")) {
            *value = 0x9875;
            return 0;
        }
        snprintf(error, error_size, "undefined symbol %s (referenced by %s)", name, o->input->name);
        return -1;
    }
    snprintf(error, error_size, "%s: symbol alias loop", o->input->name);
    return -1;
}

/* the import slots: every __imp_X the load refers to, X defined by the load or the table */
static int find_imports(const SnxCodeLoader* loader, Load* ld, char* error, size_t error_size) {
    size_t i, found;
    uint32_t j;
    char name[256];
    for (i = 0; i < ld->count; ++i) {
        const Obj* o = &ld->objs[i];
        for (j = 0; j < o->nsymbols; j += 1 + o->symbols[18 * (size_t)j + 17]) {
            const unsigned char* s = symbol(o, j);
            if (rd16(s + 12) || s[16] != SYM_EXTERNAL) continue;
            symbol_name(o, s, name, sizeof(name));
            if (strncmp(name, "__imp_", 6) || snx_link_find(&ld->imports, name, &found)) continue;
            if (!snx_link_find(&ld->globals, name + 6, &found) && !snx_link_find(&loader->symbols, name + 6, &found)) {
                snprintf(error, error_size, "undefined symbol %s (referenced by %s)", name + 6, o->input->name);
                return -1;
            }
            snx_link_insert(&ld->imports, name, ld->import_count++);
        }
    }
    return 0;
}

/* "@address" in halves: unsigned long is 32 bits on LLP64 systems */
static void address_key(char* key, uint64_t address) {
    sprintf(key, "@%lx:%08lx", (unsigned long)(address >> 32), (unsigned long)(address & 0xFFFFFFFFu));
}

/* The table symbol object's symbol index names when it is outside the arena (its address in *address), else 0: the
   load's own definitions are in the arena */
static int far_symbol(const SnxCodeLoader* loader, const Load* ld, size_t object, uint32_t index, uint64_t* address) {
    const Obj* o = &ld->objs[object];
    const unsigned char* s;
    char name[256];
    size_t found;
    if (index >= o->nsymbols) return 0;
    s = symbol(o, index);
    if ((int16_t)rd16(s + 12) != 0) return 0;
    symbol_name(o, s, name, sizeof(name));
    if (snx_link_find(&ld->globals, name, &found) || !snx_code_find(loader, name, address)) return 0;
    return *address < ld->base || *address >= ld->arena_end;
}

/* a call or jump's REL32: E8 or E9 just before the field (in an object's code section, at offset) */
static int call_relocation(const Obj* o, const Sec* sec, uint32_t offset, uint32_t type) {
    return sec->kind == K_TEXT && type == REL_REL32 && offset > 0 &&
           (o->input->data[sec->raw + offset - 1] == 0xE8 || o->input->data[sec->raw + offset - 1] == 0xE9);
}

/* the stubs and their slots: every call or jump to a table symbol outside the arena */
static void find_stubs(const SnxCodeLoader* loader, Load* ld) {
    size_t i, found;
    uint32_t j, k;
    uint64_t address;
    char key[48];
    for (i = 0; i < ld->count; ++i) {
        const Obj* o = &ld->objs[i];
        for (j = 1; j <= o->nsections; ++j) {
            const Sec* sec = &o->sections[j];
            if (sec->kind != K_TEXT) continue;
            for (k = 0; k < sec->nrelocs; ++k) {
                const unsigned char* r = o->input->data + sec->relocs + 10 * (size_t)k;
                if (!call_relocation(o, sec, rd32(r), rd16(r + 8)) || !far_symbol(loader, ld, i, rd32(r + 4), &address))
                    continue;
                address_key(key, address);
                if (snx_link_find(&ld->stubs, key, &found)) continue;
                snx_link_insert(&ld->stubs, key, ld->stub_count++);
                snx_link_insert(&ld->imports, key, ld->import_count++);
            }
        }
    }
}

static uint64_t align_up(uint64_t value, uint64_t alignment) { return (value + alignment - 1) / alignment * alignment; }
static uint64_t section_alignment(const Sec* s) {
    uint32_t n = (s->flags & SCN_ALIGN_MASK) >> 20;
    return n ? (uint64_t)1 << (n - 1) : 16;
}

/* every relocation of the loaded sections, written through the arena's read-write views */
static int relocate(const SnxCodeLoader* loader, const Load* ld, char* error, size_t error_size) {
    size_t i;
    uint32_t j, k;
    for (i = 0; i < ld->count; ++i) {
        const Obj* o = &ld->objs[i];
        for (j = 1; j <= o->nsections; ++j) {
            const Sec* sec = &o->sections[j];
            if (sec->kind < 0 || sec->kind == K_BSS) continue;
            for (k = 0; k < sec->nrelocs; ++k) {
                const unsigned char* r = o->input->data + sec->relocs + 10 * (size_t)k;
                uint32_t offset = rd32(r), type = rd16(r + 8);
                uint64_t S, start, P = sec->address + offset, v;
                unsigned width = type == REL_ADDR64 ? 8 : 4;
                unsigned char* p;
                if (type == REL_ABSOLUTE) continue;
                if ((uint64_t)offset + width > sec->size) {
                    snprintf(error, error_size, "%s: relocation outside section %s", o->input->name, sec->name);
                    return -1;
                }
                if (resolve(loader, ld, i, rd32(r + 4), &S, &start, error, error_size)) return -1;
                p = snx_code_at(loader, P);
                if (type == REL_ADDR64) {
                    v = S + rd64(p);
                    wr32(p, (uint32_t)v);
                    wr32(p + 4, (uint32_t)(v >> 32));
                    continue;
                }
                if (type == REL_ADDR32NB)
                    v = S - ld->base + rd32(p);
                else if (type >= REL_REL32 && type <= REL_REL32_5)
                    v = S + (uint64_t)(int64_t)(int32_t)rd32(p) - (P + 4 + (type - REL_REL32));
                else if (type == REL_ADDR32)
                    v = S + rd32(p);
                else if (type == REL_SECREL && start)
                    v = S - start + rd32(p);
                else {
                    snprintf(error, error_size, "%s: unsupported relocation type %u in %s", o->input->name,
                             (unsigned)type, sec->name);
                    return -1;
                }
                /* a call out of reach goes through its stub (find_stubs) */
                if (type == REL_REL32 && v + (uint64_t)0x80000000u > (uint64_t)0xFFFFFFFFu &&
                    call_relocation(o, sec, offset, type) && far_symbol(loader, ld, i, rd32(r + 4), &S)) {
                    char key[48];
                    size_t stub;
                    address_key(key, S);
                    if (snx_link_find(&ld->stubs, key, &stub))
                        v = ld->stub_address + STUB_SIZE * (uint64_t)stub + (uint64_t)(int64_t)(int32_t)rd32(p) -
                            (P + 4);
                }
                /* a 32-bit field: ADDR32NB and ADDR32 hold it unsigned, REL32 signed */
                if (type >= REL_REL32 && type <= REL_REL32_5 ? v + (uint64_t)0x80000000u > (uint64_t)0xFFFFFFFFu
                                                             : v > (uint64_t)0xFFFFFFFFu) {
                    snprintf(error, error_size,
                             "%s: relocation %u out of range in %s (every target must be in the "
                             "code arena)",
                             o->input->name, (unsigned)type, sec->name);
                    return -1;
                }
                wr32(p, (uint32_t)v);
            }
        }
    }
    return 0;
}

/* RtlLookupFunctionEntry binary-searches a table: its RUNTIME_FUNCTIONs (begin, end, unwind data) by begin */
static int compare_entry(const void* a, const void* b) {
    uint32_t x = rd32((const unsigned char*)a), y = rd32((const unsigned char*)b);
    return x < y ? -1 : x > y;
}

int snx_code_load_coff(SnxCodeLoader* loader, const SnxLinkInput* inputs, size_t count, const char* const* names,
                       size_t name_count, SnxU64* addresses, char* error, size_t error_size) {
    static const char* const region_names[SNX_CODE_REGIONS] = {"code", "read-only data", "data"};
    Load ld;
    uint64_t kind_size[KINDS], kind_align[KINDS], kind_address[KINDS], start[SNX_CODE_REGIONS], end[SNX_CODE_REGIONS];
    uint64_t value, section_start;
    size_t i, found, pdata_count;
    uint32_t j;
    int k, rc = -1;
    memset(&ld, 0, sizeof(ld));
    ld.count = count;
    ld.base = (uint64_t)(size_t)loader->base;
    ld.arena_end = ld.base + loader->reserved;
    ld.objs = (Obj*)calloc(count ? count : 1, sizeof(Obj));
    if (!ld.objs) {
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    for (i = 0; i < count; ++i)
        if (parse(&ld.objs[i], &inputs[i], error, error_size)) goto done;
    comdats(loader, &ld);
    if (collect_globals(loader, &ld, error, error_size) || find_imports(loader, &ld, error, error_size)) goto done;
    find_stubs(loader, &ld);
    for (i = 0; i < name_count; ++i)
        if (!snx_link_find(&ld.globals, names[i], &found) && !snx_link_find(&loader->symbols, names[i], &found)) {
            snprintf(error, error_size, "symbol %s is not defined", names[i]);
            goto done;
        }
    /* each kind's sections one after another, in object and section order */
    for (k = 0; k < KINDS; ++k) {
        kind_size[k] = 0;
        kind_align[k] = k == K_PDATA ? 4 : 1;
    }
    for (i = 0; i < count; ++i)
        for (j = 1; j <= ld.objs[i].nsections; ++j) {
            Sec* s = &ld.objs[i].sections[j];
            uint64_t a = s->kind == K_PDATA ? 4 : section_alignment(s);
            if (s->kind < 0) continue;
            if (a > kind_align[s->kind]) kind_align[s->kind] = a;
            s->position = align_up(kind_size[s->kind], a);
            kind_size[s->kind] = s->position + s->size;
        }
    /* [code] [read-only data, function table, import slots] [data, bss] */
    start[SNX_CODE_TEXT] = align_up(snx_code_next_free(loader, SNX_CODE_TEXT),
                                    kind_align[K_TEXT] > CODE_LINE ? kind_align[K_TEXT] : CODE_LINE);
    kind_address[K_TEXT] = start[SNX_CODE_TEXT];
    ld.stub_address = align_up(kind_address[K_TEXT] + kind_size[K_TEXT], 8);
    end[SNX_CODE_TEXT] = ld.stub_address + STUB_SIZE * (uint64_t)ld.stub_count;
    start[SNX_CODE_RODATA] = align_up(snx_code_next_free(loader, SNX_CODE_RODATA), kind_align[K_RODATA]);
    kind_address[K_RODATA] = start[SNX_CODE_RODATA];
    kind_address[K_PDATA] = align_up(kind_address[K_RODATA] + kind_size[K_RODATA], kind_align[K_PDATA]);
    ld.import_address = align_up(kind_address[K_PDATA] + kind_size[K_PDATA], 8);
    end[SNX_CODE_RODATA] = ld.import_address + 8 * (uint64_t)ld.import_count;
    start[SNX_CODE_DATA] = align_up(snx_code_next_free(loader, SNX_CODE_DATA), kind_align[K_DATA]);
    kind_address[K_DATA] = start[SNX_CODE_DATA];
    kind_address[K_BSS] = align_up(kind_address[K_DATA] + kind_size[K_DATA], kind_align[K_BSS]);
    end[SNX_CODE_DATA] = kind_address[K_BSS] + kind_size[K_BSS];
    for (i = 0; i < SNX_CODE_REGIONS; ++i)
        if (end[i] > ld.base + loader->region[i].start + loader->region[i].size) {
            snprintf(error, error_size, "the code arena's %s is full (%lu KiB)", region_names[i],
                     (unsigned long)(loader->region[i].size >> 10));
            goto done;
        }
    if (snx_code_commit(loader, end[SNX_CODE_DATA], error, error_size)) goto done;
    /* written from each region's first free byte, int3 between functions: a failed load leaves its bytes for the next
       to write over (code_loader.c) */
    for (i = 0; i < SNX_CODE_REGIONS; ++i)
        memset(snx_code_at(loader, snx_code_next_free(loader, (int)i)), i == SNX_CODE_TEXT ? 0xCC : 0,
               (size_t)(end[i] - snx_code_next_free(loader, (int)i)));
    for (i = 0; i < count; ++i)
        for (j = 1; j <= ld.objs[i].nsections; ++j) {
            Sec* s = &ld.objs[i].sections[j];
            if (s->kind < 0) continue;
            s->address = kind_address[s->kind] + s->position;
            if (s->kind != K_BSS && s->size) memcpy(snx_code_at(loader, s->address), inputs[i].data + s->raw, s->size);
        }
    for (i = 0; i < ld.imports.capacity; ++i)
        if (ld.imports.keys[i]) {
            const char* target = ld.imports.keys[i] + 6;
            uint64_t slot = ld.import_address + 8 * (uint64_t)ld.imports.values[i];
            unsigned char* p = snx_code_at(loader, slot);
            value = 0;
            if (ld.imports.keys[i][0] == '@') { /* a stub's: the table symbol's address, and the stub jumping there */
                size_t stub = 0;
                unsigned long hi = 0, lo = 0;
                uint64_t at;
                sscanf(ld.imports.keys[i], "@%lx:%lx", &hi, &lo);
                value = (uint64_t)hi << 32 | lo;
                snx_link_find(&ld.stubs, ld.imports.keys[i], &stub);
                at = ld.stub_address + STUB_SIZE * (uint64_t)stub;
                snx_code_at(loader, at)[0] = 0xFF;
                snx_code_at(loader, at)[1] = 0x25;
                wr32(snx_code_at(loader, at + 2), (uint32_t)(slot - (at + 6))); /* both in the arena: in reach */
            } else if (snx_link_find(&ld.globals, target, &found)) {
                if (resolve(loader, &ld, found >> 32, (uint32_t)found, &value, &section_start, error, error_size))
                    goto done;
            } else if (snx_link_find(&loader->symbols, target, &found))
                value = found;
            wr32(p, (uint32_t)value);
            wr32(p + 4, (uint32_t)(value >> 32));
        }
    if (relocate(loader, &ld, error, error_size)) goto done;
    pdata_count = (size_t)(kind_size[K_PDATA] / 12);
    if (pdata_count) qsort(snx_code_at(loader, kind_address[K_PDATA]), pdata_count, 12, compare_entry);
    snx_code_publish(loader, start[SNX_CODE_TEXT], end[SNX_CODE_TEXT]);
#ifdef _WIN32
    /* the table where exceptions and longjmp find it: in the arena, read-only, for as long as the code lives */
    if (pdata_count && loader->machine == SNX_CODE_COFF_X64 &&
        !RtlAddFunctionTable((PRUNTIME_FUNCTION)(size_t)kind_address[K_PDATA], (DWORD)pdata_count, (DWORD64)ld.base)) {
        snprintf(error, error_size, "cannot register the function table of %lu functions", (unsigned long)pdata_count);
        goto done;
    }
#endif
    for (i = 0; i < name_count; ++i) {
        uint64_t address = 0;
        if (snx_link_find(&ld.globals, names[i], &found)) {
            if (resolve(loader, &ld, found >> 32, (uint32_t)found, &address, &section_start, error, error_size))
                goto done;
        } else
            snx_code_lookup(loader, names[i], &address);
        addresses[i] = address;
    }
    for (i = 0; i < ld.globals.capacity; ++i)
        if (ld.globals.keys[i] && !resolve(loader, &ld, ld.globals.values[i] >> 32, (uint32_t)ld.globals.values[i],
                                           &value, &section_start, error, error_size))
            snx_link_insert(&loader->symbols, ld.globals.keys[i], (size_t)value);
    for (i = 0; i < SNX_CODE_REGIONS; ++i)
        if (end[i] > start[i]) loader->region[i].used = (size_t)(end[i] - ld.base - loader->region[i].start);
    rc = 0;
done:
    for (i = 0; i < count; ++i) free(ld.objs[i].sections);
    free(ld.objs);
    snx_link_names_free(&ld.globals);
    snx_link_names_free(&ld.imports);
    snx_link_names_free(&ld.stubs);
    return rc;
}
