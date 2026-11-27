/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The JIT loader's COFF objects (code_loader.h): Windows' x86-64 and ARM64 objects, as the back ends and Clang write
   them, placed in the code arena and relocated there, by the rules link_coff.c links a module by. The structures are
   read byte by byte, so this builds anywhere and a test can load Windows objects into any host's arena; only a Windows
   host of the objects' machine registers the function table and runs the code.

   A load's parts go into the arena's regions: [code] [read-only data and unwind data, the function table, import
   slots] [data, bss]. Symbols resolve to the load's own definitions, then to the loader's table (earlier loads, defined
   names). A COMDAT section whose key an earlier load or object already defines is left out, and its symbols resolve
   to that definition (Clang puts floating-point constants in COMDATs named after their value, __real@...). An
   __imp_X reference (a dllimport declaration) gets a slot holding X's address. ADDR32NB values (the function table's,
   the unwind data's) are offsets from the arena's start, which RtlAddFunctionTable is given as their image base: the
   arena is at most 2 GiB, so they fit. A REL32 reaches anything in the arena. A call or jump (E8, E9 before the field)
   to a table symbol outside it goes through a stub after the load's code, jmp [rip + slot] with its slot among the
   import slots, as code_loader.c's x86-64 stubs do. On ARM64 a B or BL to any table symbol may be out of its 128 MiB
   (the arena is larger), so each gets a stub, a veneer through its import slot (snx_coff_a64_veneer), which the
   branch takes when the symbol is out of its reach; ADRP reaches across the arena. Data outside the arena is reached
   through .refptr pointers, which MinGW code uses for it. JIT code calls the runtime library loaded first, and the AOT
   module through absolute addresses */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* snprintf */
#endif
#include "common/cpu/recompiler/loader/code_loader_int.h"
#include "common/cpu/recompiler/object/coff_defs.h"
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define CODE_LINE         64 /* where each load's code starts, as code_loader.c's */
#define STUB_SIZE         8  /* x86-64's: jmp qword ptr [rip + slot], int3, int3; ARM64's SNX_COFF_A64_VENEER_SIZE */
#ifdef _WIN32
/* the machine whose objects this process runs, whose function table it registers */
#if defined(__aarch64__) || defined(_M_ARM64)
#define HOST_COFF COFF_ARM64
#else
#define HOST_COFF COFF_X64
#endif
#endif

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
    unsigned machine; /* COFF_X64 or COFF_ARM64: the loader's */
} Load;

static uint64_t stub_size(unsigned machine) { return machine == COFF_ARM64 ? SNX_COFF_A64_VENEER_SIZE : STUB_SIZE; }

static const unsigned char* symbol(const Obj* o, uint32_t index) { return o->symbols + 18 * (size_t)index; }

static int classify(const Sec* s) {
    if (s->flags & (SCN_LNK_REMOVE | SCN_LNK_INFO)) return -1;
    if (!strncmp(s->name, ".debug", 6) || !strncmp(s->name, ".llvm", 5)) return -1;
    if (!strcmp(s->name, ".pdata") || !strncmp(s->name, ".pdata$", 7)) return K_PDATA;
    if (s->flags & SCN_CODE) return K_TEXT;
    if (s->flags & SCN_UNINITIALIZED) return K_BSS;
    if (s->flags & SCN_WRITE) return K_DATA;
    return K_RODATA;
}

static int parse(Obj* o, const SnxLinkInput* input, unsigned machine, char* error, size_t error_size) {
    const unsigned char* d = input->data;
    o->input = input;
    if (input->size < 20 || snx_ld_le16(d) != machine || snx_ld_le16(d + 16)) {
        snprintf(error, error_size, "%s: not an %s COFF object", input->name,
                 machine == COFF_ARM64 ? "ARM64" : "x86-64");
        return -1;
    }
    o->nsections = snx_ld_le16(d + 2);
    uint32_t table = snx_ld_le32(d + 8);
    o->nsymbols = snx_ld_le32(d + 12);
    if (20 + 40 * (size_t)o->nsections > input->size || (size_t)table + 18 * (size_t)o->nsymbols + 4 > input->size) {
        snprintf(error, error_size, "%s: truncated COFF object", input->name);
        return -1;
    }
    o->symbols = d + table;
    o->strings = (const char*)d + table + 18 * (size_t)o->nsymbols;
    o->strings_size = snx_ld_le32((const unsigned char*)o->strings);
    o->sections = (Sec*)calloc(o->nsections + 1, sizeof(Sec));
    if (!o->sections) {
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    for (uint32_t i = 1; i <= o->nsections; ++i) {
        Sec* s = &o->sections[i];
        const unsigned char* h = d + 20 + 40 * (size_t)(i - 1);
        if (h[0] == '/') {
            unsigned long offset = strtoul((const char*)h + 1, NULL, 10);
            snprintf(s->name, sizeof(s->name), "%s", offset < o->strings_size ? o->strings + offset : "");
        } else {
            memcpy(s->name, h, 8);
            s->name[8] = 0;
        }
        s->size = snx_ld_le32(h + 16);
        s->raw = snx_ld_le32(h + 20);
        s->relocs = snx_ld_le32(h + 24);
        s->nrelocs = snx_ld_le16(h + 32);
        s->flags = snx_ld_le32(h + 36);
        if ((s->flags & SCN_NRELOC_OVFL) && (size_t)s->relocs + 10 <= input->size) {
            s->nrelocs = snx_ld_le32(d + s->relocs) - 1; /* the first relocation holds the count */
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
    SnxLinkNames keys = {0};
    size_t found;
    char name[256];
    int changed;
    for (size_t i = 0; i < ld->count; ++i) {
        Obj* o = &ld->objs[i];
        for (uint32_t j = 0; j < o->nsymbols; j += 1 + o->symbols[18 * (size_t)j + 17]) {
            const unsigned char* s = symbol(o, j);
            int16_t section = (int16_t)snx_ld_le16(s + 12);
            if (section <= 0 || (uint32_t)section > o->nsections) continue;
            Sec* sec = &o->sections[section];
            if (!(sec->flags & SCN_LNK_COMDAT) || sec->kind < 0) continue;
            if (s[16] == SYM_STATIC && s[17] && !snx_ld_le32(s + 8) && !sec->selection) {
                /* the section symbol: its auxiliary record has the selection and the associated section */
                sec->selection = s[18 + 14] ? s[18 + 14] : 2;
                sec->associate = snx_ld_le16(s + 18 + 12);
                continue;
            }
            if (sec->selection && sec->selection != SEL_ASSOCIATIVE && s[16] == SYM_EXTERNAL) {
                snx_coff_symbol_name(o->strings, o->strings_size, s, name, sizeof(name));
                if (snx_link_find(&loader->symbols, name, &found) || snx_link_find(&keys, name, &found))
                    sec->kind = -1;
                else
                    snx_link_insert(&keys, name, i);
            }
        }
    }
    do {
        changed = 0;
        for (size_t i = 0; i < ld->count; ++i)
            for (uint32_t j = 1; j <= ld->objs[i].nsections; ++j) {
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
    size_t found;
    char name[256];
    for (size_t i = 0; i < ld->count; ++i) {
        const Obj* o = &ld->objs[i];
        for (uint32_t j = 0; j < o->nsymbols; j += 1 + o->symbols[18 * (size_t)j + 17]) {
            const unsigned char* s = symbol(o, j);
            int16_t section = (int16_t)snx_ld_le16(s + 12);
            if (s[16] != SYM_EXTERNAL || section <= 0 || (uint32_t)section > o->nsections ||
                o->sections[section].kind < 0)
                continue;
            snx_coff_symbol_name(o->strings, o->strings_size, s, name, sizeof(name));
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
    size_t found;
    char name[256];
    *section_address = 0;
    for (int depth = 0; depth < 8; ++depth) {
        if (index >= o->nsymbols) {
            snprintf(error, error_size, "%s: bad symbol index", o->input->name);
            return -1;
        }
        const unsigned char* s = symbol(o, index);
        int16_t section = (int16_t)snx_ld_le16(s + 12);
        snx_coff_symbol_name(o->strings, o->strings_size, s, name, sizeof(name));
        if (section > 0) {
            if ((uint32_t)section > o->nsections) {
                snprintf(error, error_size, "%s: bad section number", o->input->name);
                return -1;
            }
            const Sec* sec = &o->sections[section];
            if (sec->kind >= 0) {
                *value = sec->address + snx_ld_le32(s + 8);
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
            *value = snx_ld_le32(s + 8);
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
            index = snx_ld_le32(s + 18); /* the default definition */
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
    size_t found;
    char name[256];
    for (size_t i = 0; i < ld->count; ++i) {
        const Obj* o = &ld->objs[i];
        for (uint32_t j = 0; j < o->nsymbols; j += 1 + o->symbols[18 * (size_t)j + 17]) {
            const unsigned char* s = symbol(o, j);
            if (snx_ld_le16(s + 12) || s[16] != SYM_EXTERNAL) continue;
            snx_coff_symbol_name(o->strings, o->strings_size, s, name, sizeof(name));
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

/* "@address" in hexadecimal */
static void address_key(char* key, uint64_t address) { sprintf(key, "@%llx", (unsigned long long)address); }

/* Whether object's symbol index names a table symbol (its address in *address) that a call may need a stub for: on
   x86-64 one outside the arena, which rel32 may not reach (inside, it reaches across); on ARM64 any, since the arena
   is larger than B's reach. The load's own definitions are in reach of its calls */
static int stub_symbol(const SnxCodeLoader* loader, const Load* ld, size_t object, uint32_t index, uint64_t* address) {
    const Obj* o = &ld->objs[object];
    char name[256];
    size_t found;
    if (index >= o->nsymbols) return 0;
    const unsigned char* s = symbol(o, index);
    if ((int16_t)snx_ld_le16(s + 12) != 0) return 0;
    snx_coff_symbol_name(o->strings, o->strings_size, s, name, sizeof(name));
    if (snx_link_find(&ld->globals, name, &found) || !snx_code_find(loader, name, address)) return 0;
    return ld->machine == COFF_ARM64 || *address < ld->base || *address >= ld->arena_end;
}

/* a call or jump in an object's code section, at offset: x86-64's REL32 with E8 or E9 just before the field, ARM64's
   BRANCH26 (B, BL) */
static int call_relocation(const Load* ld, const Obj* o, const Sec* sec, uint32_t offset, uint32_t type) {
    if (sec->kind != K_TEXT) return 0;
    if (ld->machine == COFF_ARM64) return type == REL_A64_BRANCH26;
    return type == REL_REL32 && offset > 0 &&
           (o->input->data[sec->raw + offset - 1] == 0xE8 || o->input->data[sec->raw + offset - 1] == 0xE9);
}

/* the stubs and their slots: every call or jump to a table symbol that may need one (stub_symbol) */
static void find_stubs(const SnxCodeLoader* loader, Load* ld) {
    size_t found;
    uint64_t address;
    char key[48];
    for (size_t i = 0; i < ld->count; ++i) {
        const Obj* o = &ld->objs[i];
        for (uint32_t j = 1; j <= o->nsections; ++j) {
            const Sec* sec = &o->sections[j];
            if (sec->kind != K_TEXT) continue;
            for (uint32_t k = 0; k < sec->nrelocs; ++k) {
                const unsigned char* r = o->input->data + sec->relocs + 10 * (size_t)k;
                if (!call_relocation(ld, o, sec, snx_ld_le32(r), snx_ld_le16(r + 8)) ||
                    !stub_symbol(loader, ld, i, snx_ld_le32(r + 4), &address))
                    continue;
                address_key(key, address);
                if (snx_link_find(&ld->stubs, key, &found)) continue;
                snx_link_insert(&ld->stubs, key, ld->stub_count++);
                snx_link_insert(&ld->imports, key, ld->import_count++);
            }
        }
    }
}

static uint64_t section_alignment(const Sec* s) {
    uint32_t n = (s->flags & SCN_ALIGN_MASK) >> 20;
    return n ? (uint64_t)1 << (n - 1) : 16;
}

/* ARM64's relocation r of object's section sec, to S (in the section at start): the data ones here, the instructions'
   by snx_coff_a64_relocate, and a B or BL to a table symbol it does not reach through the symbol's stub. 0, or -1 with
   the reason in error */
static int relocate_a64(const SnxCodeLoader* loader, const Load* ld, size_t object, const Sec* sec,
                        const unsigned char* r, uint64_t S, uint64_t start, char* error, size_t error_size) {
    const Obj* o = &ld->objs[object];
    uint32_t offset = snx_ld_le32(r), type = snx_ld_le16(r + 8);
    uint64_t P = sec->address + offset, v, address;
    unsigned char* p = snx_code_at(loader, P);
    int rc;
    switch (type) {
        case REL_A64_ADDR64: snx_st_le64(p, S + snx_ld_le64(p)); return 0;
        case REL_A64_ADDR32:
            v = S + snx_ld_le32(p);
            rc = v > (uint64_t)0xFFFFFFFFu ? SNX_LINK_RANGE : 0;
            if (!rc) snx_st_le32(p, (uint32_t)v);
            break;
        default:
            rc = snx_coff_a64_relocate(p, sec->size - offset, type, S, P, ld->base, start);
            /* a call with no addend: its stub jumps to the symbol itself */
            if (rc == SNX_LINK_RANGE && call_relocation(ld, o, sec, offset, type) && !(snx_ld_le32(p) & 0x3FFFFFFu) &&
                stub_symbol(loader, ld, object, snx_ld_le32(r + 4), &address)) {
                char key[48];
                size_t stub;
                address_key(key, address);
                if (snx_link_find(&ld->stubs, key, &stub))
                    rc = snx_coff_a64_relocate(p, 4, type, ld->stub_address + SNX_COFF_A64_VENEER_SIZE * (uint64_t)stub,
                                               P, ld->base, 0);
            }
    }
    if (!rc) return 0;
    snprintf(error, error_size,
             "%s: ARM64 relocation %u at %s+%#x to %#llx: %s (every target but a call's must be in the code arena)",
             o->input->name, (unsigned)type, sec->name, (unsigned)offset, (unsigned long long)S,
             rc == SNX_LINK_RANGE        ? "out of the instruction's reach"
             : rc == SNX_LINK_MISALIGNED ? "not aligned as the instruction needs"
             : rc == SNX_LINK_OUTSIDE    ? "its field is past the end of its section"
                                         : "a type the loader does not know");
    return -1;
}

/* every relocation of the loaded sections, written through the arena's read-write views */
static int relocate(const SnxCodeLoader* loader, const Load* ld, char* error, size_t error_size) {
    for (size_t i = 0; i < ld->count; ++i) {
        const Obj* o = &ld->objs[i];
        for (uint32_t j = 1; j <= o->nsections; ++j) {
            const Sec* sec = &o->sections[j];
            if (sec->kind < 0 || sec->kind == K_BSS) continue;
            for (uint32_t k = 0; k < sec->nrelocs; ++k) {
                const unsigned char* r = o->input->data + sec->relocs + 10 * (size_t)k;
                uint32_t offset = snx_ld_le32(r), type = snx_ld_le16(r + 8);
                uint64_t S, start, P = sec->address + offset, v;
                unsigned width = type == (ld->machine == COFF_ARM64 ? REL_A64_ADDR64 : REL_ADDR64) ? 8 : 4;
                if (type == REL_ABSOLUTE) continue; /* ARM64's REL_A64_ABSOLUTE too */
                if ((uint64_t)offset + width > sec->size) {
                    snprintf(error, error_size, "%s: relocation outside section %s", o->input->name, sec->name);
                    return -1;
                }
                if (resolve(loader, ld, i, snx_ld_le32(r + 4), &S, &start, error, error_size)) return -1;
                unsigned char* p = snx_code_at(loader, P);
                if (ld->machine == COFF_ARM64) {
                    if (relocate_a64(loader, ld, i, sec, r, S, start, error, error_size)) return -1;
                    continue;
                }
                if (type == REL_ADDR64) {
                    v = S + snx_ld_le64(p);
                    snx_st_le64(p, v);
                    continue;
                }
                if (type == REL_ADDR32NB)
                    v = S - ld->base + snx_ld_le32(p);
                else if (type >= REL_REL32 && type <= REL_REL32_5)
                    v = S + (uint64_t)(int64_t)(int32_t)snx_ld_le32(p) - (P + 4 + (type - REL_REL32));
                else if (type == REL_ADDR32)
                    v = S + snx_ld_le32(p);
                else if (type == REL_SECREL && start)
                    v = S - start + snx_ld_le32(p);
                else {
                    snprintf(error, error_size, "%s: unsupported relocation type %u in %s", o->input->name,
                             (unsigned)type, sec->name);
                    return -1;
                }
                /* a call out of reach goes through its stub (find_stubs) */
                if (type == REL_REL32 && v + (uint64_t)0x80000000u > (uint64_t)0xFFFFFFFFu &&
                    call_relocation(ld, o, sec, offset, type) && stub_symbol(loader, ld, i, snx_ld_le32(r + 4), &S)) {
                    char key[48];
                    size_t stub;
                    address_key(key, S);
                    if (snx_link_find(&ld->stubs, key, &stub))
                        v = ld->stub_address + STUB_SIZE * (uint64_t)stub + (uint64_t)(int64_t)(int32_t)snx_ld_le32(p) -
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
                snx_st_le32(p, (uint32_t)v);
            }
        }
    }
    return 0;
}

int snx_code_load_coff(SnxCodeLoader* loader, const SnxLinkInput* inputs, size_t count, const char* const* names,
                       size_t name_count, SnxU64* addresses, char* error, size_t error_size) {
    static const char* const region_names[SNX_CODE_REGIONS] = {"code", "read-only data", "data"};
    Load ld = {0};
    uint64_t kind_size[KINDS], kind_align[KINDS], kind_address[KINDS], start[SNX_CODE_REGIONS], end[SNX_CODE_REGIONS];
    uint64_t value, section_start;
    size_t found;
    int rc = -1;
    ld.count = count;
    ld.machine = loader->machine;
    ld.base = (uint64_t)(size_t)loader->base;
    ld.arena_end = ld.base + loader->reserved;
    ld.objs = (Obj*)calloc(count ? count : 1, sizeof(Obj));
    if (!ld.objs) {
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    for (size_t i = 0; i < count; ++i)
        if (parse(&ld.objs[i], &inputs[i], ld.machine, error, error_size)) goto done;
    comdats(loader, &ld);
    if (collect_globals(loader, &ld, error, error_size) || find_imports(loader, &ld, error, error_size)) goto done;
    find_stubs(loader, &ld);
    for (size_t i = 0; i < name_count; ++i)
        if (!snx_link_find(&ld.globals, names[i], &found) && !snx_link_find(&loader->symbols, names[i], &found)) {
            snprintf(error, error_size, "symbol %s is not defined", names[i]);
            goto done;
        }
    /* each kind's sections one after another, in object and section order */
    for (int k = 0; k < KINDS; ++k) {
        kind_size[k] = 0;
        kind_align[k] = k == K_PDATA ? 4 : 1;
    }
    for (size_t i = 0; i < count; ++i)
        for (uint32_t j = 1; j <= ld.objs[i].nsections; ++j) {
            Sec* s = &ld.objs[i].sections[j];
            uint64_t a = s->kind == K_PDATA ? 4 : section_alignment(s);
            if (s->kind < 0) continue;
            if (a > kind_align[s->kind]) kind_align[s->kind] = a;
            s->position = snx_align_up(kind_size[s->kind], a);
            kind_size[s->kind] = s->position + s->size;
        }
    /* [code] [read-only data, function table, import slots] [data, bss] */
    start[SNX_CODE_TEXT] = snx_align_up(snx_code_next_free(loader, SNX_CODE_TEXT),
                                        kind_align[K_TEXT] > CODE_LINE ? kind_align[K_TEXT] : CODE_LINE);
    kind_address[K_TEXT] = start[SNX_CODE_TEXT];
    ld.stub_address = snx_align_up(kind_address[K_TEXT] + kind_size[K_TEXT], 8);
    end[SNX_CODE_TEXT] = ld.stub_address + stub_size(ld.machine) * (uint64_t)ld.stub_count;
    start[SNX_CODE_RODATA] = snx_align_up(snx_code_next_free(loader, SNX_CODE_RODATA), kind_align[K_RODATA]);
    kind_address[K_RODATA] = start[SNX_CODE_RODATA];
    kind_address[K_PDATA] = snx_align_up(kind_address[K_RODATA] + kind_size[K_RODATA], kind_align[K_PDATA]);
    ld.import_address = snx_align_up(kind_address[K_PDATA] + kind_size[K_PDATA], 8);
    end[SNX_CODE_RODATA] = ld.import_address + 8 * (uint64_t)ld.import_count;
    start[SNX_CODE_DATA] = snx_align_up(snx_code_next_free(loader, SNX_CODE_DATA), kind_align[K_DATA]);
    kind_address[K_DATA] = start[SNX_CODE_DATA];
    kind_address[K_BSS] = snx_align_up(kind_address[K_DATA] + kind_size[K_DATA], kind_align[K_BSS]);
    end[SNX_CODE_DATA] = kind_address[K_BSS] + kind_size[K_BSS];
    for (size_t i = 0; i < SNX_CODE_REGIONS; ++i)
        if (end[i] > ld.base + loader->region[i].start + loader->region[i].size) {
            snprintf(error, error_size, "the code arena's %s is full (%lu KiB)", region_names[i],
                     (unsigned long)(loader->region[i].size >> 10));
            goto done;
        }
    if (snx_code_commit(loader, end[SNX_CODE_DATA], error, error_size)) goto done;
    /* written from each region's first free byte, int3 (x86-64) or UDF #0 (ARM64, zeros) between functions: a failed
       load leaves its bytes for the next to write over (code_loader.c) */
    snx_code_writable(1);
    for (size_t i = 0; i < SNX_CODE_REGIONS; ++i)
        memset(snx_code_at(loader, snx_code_next_free(loader, (int)i)),
               i == SNX_CODE_TEXT && ld.machine == COFF_X64 ? 0xCC : 0,
               (size_t)(end[i] - snx_code_next_free(loader, (int)i)));
    for (size_t i = 0; i < count; ++i)
        for (uint32_t j = 1; j <= ld.objs[i].nsections; ++j) {
            Sec* s = &ld.objs[i].sections[j];
            if (s->kind < 0) continue;
            s->address = kind_address[s->kind] + s->position;
            if (s->kind != K_BSS && s->size) memcpy(snx_code_at(loader, s->address), inputs[i].data + s->raw, s->size);
        }
    for (size_t i = 0; i < ld.imports.capacity; ++i)
        if (ld.imports.keys[i]) {
            const char* target = ld.imports.keys[i] + 6;
            uint64_t slot = ld.import_address + 8 * (uint64_t)ld.imports.values[i];
            unsigned char* p = snx_code_at(loader, slot);
            value = 0;
            if (ld.imports.keys[i][0] == '@') { /* a stub's: the table symbol's address, and the stub jumping there */
                size_t stub = 0;
                unsigned long long address = 0;
                sscanf(ld.imports.keys[i], "@%llx", &address); /* address_key's */
                value = address;
                snx_link_find(&ld.stubs, ld.imports.keys[i], &stub);
                uint64_t at = ld.stub_address + stub_size(ld.machine) * (uint64_t)stub;
                if (ld.machine == COFF_ARM64) /* both in the arena, the slot 8-aligned: in ADRP's reach */
                    snx_coff_a64_veneer(snx_code_at(loader, at), at, slot, 1);
                else {
                    snx_code_at(loader, at)[0] = 0xFF;
                    snx_code_at(loader, at)[1] = 0x25;
                    snx_st_le32(snx_code_at(loader, at + 2), (uint32_t)(slot - (at + 6))); /* both in the arena */
                }
            } else if (snx_link_find(&ld.globals, target, &found)) {
                if (resolve(loader, &ld, found >> 32, (uint32_t)found, &value, &section_start, error, error_size))
                    goto done;
            } else if (snx_link_find(&loader->symbols, target, &found))
                value = found;
            snx_st_le64(p, value);
        }
    if (relocate(loader, &ld, error, error_size)) goto done;
    size_t entry = snx_coff_function_size(ld.machine), pdata_count = (size_t)(kind_size[K_PDATA] / entry);
    if (pdata_count) qsort(snx_code_at(loader, kind_address[K_PDATA]), pdata_count, entry, snx_coff_compare_function);
    snx_code_writable(0);
    if (snx_code_publish(loader, start[SNX_CODE_TEXT], end[SNX_CODE_TEXT], error, error_size)) goto done;
#ifdef _WIN32
    /* the table where exceptions and longjmp find it: in the arena, read-only, for as long as the code lives */
    if (pdata_count && loader->machine == HOST_COFF &&
        !RtlAddFunctionTable((PRUNTIME_FUNCTION)(size_t)kind_address[K_PDATA], (DWORD)pdata_count, (DWORD64)ld.base)) {
        snprintf(error, error_size, "cannot register the function table of %zu functions", pdata_count);
        goto done;
    }
#endif
    for (size_t i = 0; i < name_count; ++i) {
        uint64_t address = 0;
        if (snx_link_find(&ld.globals, names[i], &found)) {
            if (resolve(loader, &ld, found >> 32, (uint32_t)found, &address, &section_start, error, error_size))
                goto done;
        } else
            snx_code_lookup(loader, names[i], &address);
        addresses[i] = address;
    }
    for (size_t i = 0; i < ld.globals.capacity; ++i)
        if (ld.globals.keys[i] && !resolve(loader, &ld, ld.globals.values[i] >> 32, (uint32_t)ld.globals.values[i],
                                           &value, &section_start, error, error_size))
            snx_link_insert(&loader->symbols, ld.globals.keys[i], (size_t)value);
    for (size_t i = 0; i < SNX_CODE_REGIONS; ++i)
        if (end[i] > start[i]) loader->region[i].used = (size_t)(end[i] - ld.base - loader->region[i].start);
    rc = 0;
done:
    snx_code_writable(0); /* when a load failed while writing */
    for (size_t i = 0; i < count; ++i) free(ld.objs[i].sections);
    free(ld.objs);
    snx_link_names_free(&ld.globals);
    snx_link_names_free(&ld.imports);
    snx_link_names_free(&ld.stubs);
    return rc;
}
