/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* COFF: x86-64 objects into a PE32+ DLL without imports or an entry point.
   Sections: .text, .rdata (read-only data, unwind data, the export directory), .data, .bss, .pdata (function table,
   sorted), .reloc (DIR64 base relocations for absolute addresses). COMDAT sections keep their first copy.
   The structures are read and written byte by byte, so this builds anywhere (no <windows.h>). */
#include "common/cpu/recompiler/llvm/coff_defs.h"
#include "common/cpu/recompiler/llvm/link_internal.h"
#include "common/utilities/bits.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_BASE        ((uint64_t)0x180000000ULL)
#define SECTION_ALIGNMENT 0x1000
#define FILE_ALIGNMENT    0x200

enum { O_TEXT, O_RDATA, O_DATA, O_BSS, O_PDATA, O_RELOC, O_COUNT };
static const char* const output_names[O_COUNT] = {".text", ".rdata", ".data", ".bss", ".pdata", ".reloc"};
static const uint32_t output_flags[O_COUNT] = {
    SCN_CODE | SCN_EXECUTE | SCN_READ,        SCN_INITIALIZED | SCN_READ, SCN_INITIALIZED | SCN_READ | SCN_WRITE,
    SCN_UNINITIALIZED | SCN_READ | SCN_WRITE, SCN_INITIALIZED | SCN_READ, SCN_INITIALIZED | SCN_READ | SCN_DISCARDABLE};

typedef struct {
    char name[256];
    const unsigned char* header;
    uint32_t flags, size, raw, relocs, nrelocs;
    int output;    /* O_*, or -1 when not loaded */
    int discarded; /* a later copy of a COMDAT */
    int selection, associate;
    uint32_t rva; /* final */
} Sec;

typedef struct {
    const SnxLinkInput* input;
    const unsigned char* symbols;
    uint32_t nsymbols, nsections;
    const char* strings;
    uint32_t strings_size;
    Sec* sections;
} Obj;

typedef struct {
    Obj* objs;
    size_t nobjs;
    SnxLinkNames globals; /* name -> object << 32 | symbol index */
    SnxLinkNames comdats;
    SnxLinkNames imports; /* "__imp_X" of a defined X -> pointer slot */
    uint32_t imports_rva;
    uint32_t out_rva[O_COUNT], out_size[O_COUNT], out_raw[O_COUNT], out_raw_size[O_COUNT];
    char* error;
    size_t error_size;
} Link;

static int fail(Link* l, const char* format, const char* a, const char* b) {
    return snx_link_fail(l->error, l->error_size, format, a, b);
}

static const unsigned char* symbol(const Obj* o, uint32_t index) { return o->symbols + 18 * (size_t)index; }

static int classify(Sec* s) {
    if (s->flags & (SCN_LNK_REMOVE | SCN_LNK_INFO)) return -1;
    if (!strncmp(s->name, ".debug", 6) || !strncmp(s->name, ".llvm", 5)) return -1;
    if (!strcmp(s->name, ".pdata") || !strncmp(s->name, ".pdata$", 7)) return O_PDATA;
    if (s->flags & SCN_CODE) return O_TEXT;
    if (s->flags & SCN_UNINITIALIZED) return O_BSS;
    if (s->flags & SCN_WRITE) return O_DATA;
    return O_RDATA;
}

static int parse(Link* l, Obj* o, const SnxLinkInput* input) {
    const unsigned char* d = input->data;
    o->input = input;
    if (input->size < 20 || snx_ld_le16(d) != 0x8664 || snx_ld_le16(d + 16))
        return fail(l, "%s: not an x86-64 COFF object%s", input->name, "");
    o->nsections = snx_ld_le16(d + 2);
    uint32_t table = snx_ld_le32(d + 8);
    o->nsymbols = snx_ld_le32(d + 12);
    if (20 + 40 * (size_t)o->nsections > input->size || table + 18 * (size_t)o->nsymbols + 4 > input->size)
        return fail(l, "%s: truncated COFF object%s", input->name, "");
    o->symbols = d + table;
    o->strings = (const char*)d + table + 18 * (size_t)o->nsymbols;
    o->strings_size = snx_ld_le32((const unsigned char*)o->strings);
    o->sections = (Sec*)calloc(o->nsections + 1, sizeof(Sec));
    if (!o->sections) return fail(l, "out of memory%s%s", "", "");
    for (uint32_t i = 1; i <= o->nsections; ++i) {
        Sec* s = &o->sections[i];
        const unsigned char* h = d + 20 + 40 * (size_t)(i - 1);
        s->header = h;
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
        if (s->flags & SCN_NRELOC_OVFL) s->nrelocs = snx_ld_le32(d + s->relocs) - 1, s->relocs += 10;
        if (!(s->flags & SCN_UNINITIALIZED) && (size_t)s->raw + s->size > input->size)
            return fail(l, "%s: truncated section %s", input->name, s->name);
        if ((size_t)s->relocs + 10 * (size_t)s->nrelocs > input->size)
            return fail(l, "%s: truncated relocations in %s", input->name, s->name);
        s->output = classify(s);
    }
    return 0;
}

/* COMDAT: the first object to define a key keeps its section; associative sections follow their section */
static void comdats(Link* l) {
    char name[256];
    int changed;
    for (size_t i = 0; i < l->nobjs; ++i) {
        Obj* o = &l->objs[i];
        for (uint32_t j = 0; j < o->nsymbols; j += 1 + o->symbols[18 * (size_t)j + 17]) {
            const unsigned char* s = symbol(o, j);
            int16_t section = (int16_t)snx_ld_le16(s + 12);
            if (section <= 0 || (uint32_t)section > o->nsections) continue;
            Sec* sec = &o->sections[section];
            if (!(sec->flags & SCN_LNK_COMDAT)) continue;
            if (s[16] == SYM_STATIC && s[17] && !snx_ld_le32(s + 8) && !sec->selection) {
                /* the section symbol: its auxiliary record has the selection and the associated section */
                sec->selection = s[18 + 14] ? s[18 + 14] : 2;
                sec->associate = snx_ld_le16(s + 18 + 12);
                continue;
            }
            if (sec->selection && sec->selection != SEL_ASSOCIATIVE && s[16] == SYM_EXTERNAL && !sec->discarded &&
                !sec->rva) {
                size_t first;
                snx_coff_symbol_name(o->strings, o->strings_size, s, name, sizeof(name));
                if (snx_link_find(&l->comdats, name, &first) && first != i)
                    sec->discarded = 1;
                else
                    snx_link_insert(&l->comdats, name, i);
                sec->rva = 1; /* key handled */
            }
        }
        for (uint32_t n = 1; n <= o->nsections; ++n) o->sections[n].rva = 0;
    }
    do {
        changed = 0;
        for (size_t i = 0; i < l->nobjs; ++i)
            for (uint32_t n = 1; n <= l->objs[i].nsections; ++n) {
                Sec* s = &l->objs[i].sections[n];
                if (s->selection == SEL_ASSOCIATIVE && !s->discarded && s->associate &&
                    s->associate <= (int)l->objs[i].nsections && l->objs[i].sections[s->associate].discarded) {
                    s->discarded = 1;
                    changed = 1;
                }
            }
    } while (changed);
    for (size_t i = 0; i < l->nobjs; ++i)
        for (uint32_t n = 1; n <= l->objs[i].nsections; ++n)
            if (l->objs[i].sections[n].discarded) l->objs[i].sections[n].output = -1;
}

static int collect_globals(Link* l) {
    size_t found;
    char name[256];
    for (size_t i = 0; i < l->nobjs; ++i) {
        Obj* o = &l->objs[i];
        for (uint32_t j = 0; j < o->nsymbols; j += 1 + o->symbols[18 * (size_t)j + 17]) {
            const unsigned char* s = symbol(o, j);
            int16_t section = (int16_t)snx_ld_le16(s + 12);
            if (s[16] != SYM_EXTERNAL || section == 0) continue;
            if (section > 0 && ((uint32_t)section > o->nsections || o->sections[section].output < 0)) continue;
            snx_coff_symbol_name(o->strings, o->strings_size, s, name, sizeof(name));
            if (snx_link_find(&l->globals, name, &found)) {
                const Obj* other = &l->objs[found >> 32];
                int16_t other_section = (int16_t)snx_ld_le16(symbol(other, (uint32_t)found) + 12);
                if (section > 0 && (o->sections[section].flags & SCN_LNK_COMDAT) && other_section > 0 &&
                    (other->sections[other_section].flags & SCN_LNK_COMDAT))
                    continue;
                return fail(l, "duplicate symbol %s (%s)", name, o->input->name);
            }
            snx_link_insert(&l->globals, name, (i << 32) | j);
        }
    }
    return 0;
}

/* the RVA (or, absolute, the value) of object o's symbol index */
static int resolve(Link* l, size_t object, uint32_t index, uint64_t* value, int* absolute, int* out_section) {
    const Obj* o = &l->objs[object];
    size_t found;
    char name[256];
    for (int depth = 0; depth < 8; ++depth) {
        if (index >= o->nsymbols) return fail(l, "%s: bad symbol index%s", o->input->name, "");
        const unsigned char* s = symbol(o, index);
        int16_t section = (int16_t)snx_ld_le16(s + 12);
        *absolute = 0;
        *out_section = -1;
        if (section > 0) {
            if ((uint32_t)section > o->nsections) return fail(l, "%s: bad section number%s", o->input->name, "");
            const Sec* sec = &o->sections[section];
            if (sec->output < 0) {
                /* a discarded COMDAT copy: its external symbols resolve to the kept copy */
                snx_coff_symbol_name(o->strings, o->strings_size, s, name, sizeof(name));
                if (s[16] == SYM_EXTERNAL && snx_link_find(&l->globals, name, &found)) {
                    object = found >> 32;
                    index = (uint32_t)found;
                    o = &l->objs[object];
                    continue;
                }
                return fail(l, "%s: reference to discarded section %s", o->input->name, sec->name);
            }
            *value = sec->rva + snx_ld_le32(s + 8);
            *out_section = sec->output;
            return 0;
        }
        if (section == -1) {
            *absolute = 1;
            *value = snx_ld_le32(s + 8);
            return 0;
        }
        snx_coff_symbol_name(o->strings, o->strings_size, s, name, sizeof(name));
        if (snx_link_find(&l->globals, name, &found) && (found >> 32 != object || (uint32_t)found != index)) {
            object = found >> 32;
            index = (uint32_t)found;
            o = &l->objs[object];
            continue;
        }
        if (s[16] == SYM_WEAK_EXTERNAL && s[17]) {
            index = snx_ld_le32(s + 18); /* the default definition */
            continue;
        }
        if (snx_link_find(&l->imports, name, &found)) {
            *value = l->imports_rva + 8 * found;
            *out_section = O_DATA;
            return 0;
        }
        if (!strcmp(name, "__ImageBase")) {
            *value = 0;
            return 0;
        }
        if (!strcmp(name, "_fltused")) {
            *absolute = 1;
            *value = 0x9875;
            return 0;
        }
        if (snx_ld_le32(s + 8)) return fail(l, "%s: common symbol %s is not supported", o->input->name, name);
        return fail(l, "undefined symbol %s (referenced by %s)", name, o->input->name);
    }
    return fail(l, "%s: symbol alias loop%s", o->input->name, "");
}

/* a dllimport declaration of a function the module defines itself (a C library function rtlib.c provides) refers to
   __imp_<name>, a pointer to it: give each one a slot in .data, as lld-link does for locally defined imports */
static void find_imports(Link* l) {
    size_t found;
    char name[256];
    for (size_t i = 0; i < l->nobjs; ++i) {
        const Obj* o = &l->objs[i];
        for (uint32_t j = 0; j < o->nsymbols; j += 1 + o->symbols[18 * (size_t)j + 17]) {
            const unsigned char* s = symbol(o, j);
            if (s[16] != SYM_EXTERNAL || snx_ld_le16(s + 12)) continue;
            snx_coff_symbol_name(o->strings, o->strings_size, s, name, sizeof(name));
            if (strncmp(name, "__imp_", 6) || snx_link_find(&l->imports, name, &found)) continue;
            if (snx_link_find(&l->globals, name + 6, &found)) snx_link_insert(&l->imports, name, l->imports.count);
        }
    }
}
static uint32_t section_alignment(const Sec* s) {
    uint32_t bits = (s->flags & SCN_ALIGN_MASK) >> 20;
    return bits ? 1u << (bits - 1) : 16;
}

typedef struct {
    size_t object;
    uint32_t section;
} Order;
static const Link* sort_link;
static int compare_order(const void* a, const void* b) {
    const Order *x = (const Order*)a, *y = (const Order*)b;
    const Sec *p = &sort_link->objs[x->object].sections[x->section],
              *q = &sort_link->objs[y->object].sections[y->section];
    const char *dp = strchr(p->name, '$'), *dq = strchr(q->name, '$');
    /* grouped section names sort by their $ suffix; equal names keep object order */
    int c = strcmp(dp ? dp : "", dq ? dq : "");
    if (c) return c;
    if (x->object != y->object) return x->object < y->object ? -1 : 1;
    return x->section < y->section ? -1 : x->section > y->section;
}
static int compare_u32(const void* a, const void* b) {
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}

int snx_link_coff(const SnxLinkInput* inputs, size_t count, const char* const* exports, size_t export_count,
                  const char* soname, SnxLinkOutput* output, char* error, size_t error_size) {
    Link l = {0};
    Order* order = NULL;
    size_t norder = 0;
    uint32_t edata_rva = 0, edata_size = 0, *bases = NULL, nbases = 0, cap_bases = 0;
    SnxLinkBuffer image[O_COUNT] = {{0}}, out = {0};
    const char** sorted = NULL;
    int rc = -1, nout = 0, used[O_COUNT];
    *output = (SnxLinkOutput){0};
    l.error = error;
    l.error_size = error_size;
    l.nobjs = count;
    l.objs = (Obj*)calloc(count, sizeof(Obj));
    if (!l.objs) goto done;
    for (size_t i = 0; i < count; ++i)
        if (parse(&l, &l.objs[i], &inputs[i])) goto done;
    comdats(&l);
    if (collect_globals(&l)) goto done;
    find_imports(&l);
    /* order and place the input sections inside each output section */
    for (size_t i = 0; i < count; ++i) norder += l.objs[i].nsections;
    order = (Order*)calloc(norder + 1, sizeof(Order));
    if (!order) goto done;
    norder = 0;
    for (size_t i = 0; i < count; ++i)
        for (uint32_t n = 1; n <= l.objs[i].nsections; ++n)
            if (l.objs[i].sections[n].output >= 0) {
                order[norder].object = i;
                order[norder++].section = n;
            }
    sort_link = &l;
    qsort(order, norder, sizeof(Order), compare_order);
    for (int o_index = 0; o_index < O_COUNT; ++o_index)
        for (size_t k = 0; k < norder; ++k) {
            Sec* s = &l.objs[order[k].object].sections[order[k].section];
            if (s->output != o_index) continue;
            l.out_size[o_index] = snx_align_up(l.out_size[o_index], section_alignment(s));
            s->rva = l.out_size[o_index]; /* offset for now */
            l.out_size[o_index] += s->size;
        }
    if (l.imports.count) {
        l.out_size[O_DATA] = snx_align_up(l.out_size[O_DATA], 8);
        l.imports_rva = l.out_size[O_DATA]; /* offset for now */
        l.out_size[O_DATA] += 8 * (uint32_t)l.imports.count;
    }
    /* the export directory at the end of .rdata */
    if (export_count) {
        sorted = (const char**)malloc(export_count * sizeof(*sorted));
        if (!sorted) goto done;
        memcpy(sorted, exports, export_count * sizeof(*sorted));
        qsort(sorted, export_count, sizeof(*sorted), snx_compare_strings);
        edata_size = 40 + 10 * (uint32_t)export_count + (uint32_t)strlen(soname) + 1;
        for (size_t i = 0; i < export_count; ++i) edata_size += (uint32_t)strlen(sorted[i]) + 1;
        l.out_size[O_RDATA] = snx_align_up(l.out_size[O_RDATA], 4);
        edata_rva = l.out_size[O_RDATA];
        l.out_size[O_RDATA] += edata_size;
    }
    /* RVAs: headers, then the sections that have contents, each on its own pages and without gaps between them,
       which the Windows loader requires */
    uint32_t headers = snx_align_up(0x80 + 4 + 20 + 240 + 40 * O_COUNT, FILE_ALIGNMENT);
    uint32_t rva = SECTION_ALIGNMENT;
    for (int o_index = 0; o_index < O_RELOC; ++o_index) {
        used[o_index] = o_index == O_TEXT || l.out_size[o_index] != 0;
        l.out_rva[o_index] = rva;
        if (used[o_index]) rva = snx_align_up(rva + (l.out_size[o_index] ? l.out_size[o_index] : 1), SECTION_ALIGNMENT);
    }
    for (size_t k = 0; k < norder; ++k) {
        Sec* s = &l.objs[order[k].object].sections[order[k].section];
        s->rva += l.out_rva[s->output];
    }
    edata_rva += l.out_rva[O_RDATA];
    l.imports_rva += l.out_rva[O_DATA];
    for (int o_index = 0; o_index < O_RELOC; ++o_index)
        if (o_index != O_BSS) snx_link_reserve(&image[o_index], l.out_size[o_index]);
    if (l.out_size[O_TEXT]) memset(image[O_TEXT].data, 0xCC, l.out_size[O_TEXT]); /* int3 between functions */
    for (size_t k = 0; k < norder; ++k) {
        const Obj* o = &l.objs[order[k].object];
        const Sec* s = &o->sections[order[k].section];
        if (s->output != O_BSS && !(s->flags & SCN_UNINITIALIZED) && s->size)
            memcpy(image[s->output].data + (s->rva - l.out_rva[s->output]), o->input->data + s->raw, s->size);
        else if (s->output != O_BSS && s->size)
            memset(image[s->output].data + (s->rva - l.out_rva[s->output]), 0, s->size);
    }
    /* relocations, addends implicit in the section contents */
    for (size_t k = 0; k < norder; ++k) {
        const Obj* o = &l.objs[order[k].object];
        const Sec* s = &o->sections[order[k].section];
        if (s->output == O_BSS) continue;
        for (uint32_t j = 0; j < s->nrelocs; ++j) {
            const unsigned char* r = o->input->data + s->relocs + 10 * (size_t)j;
            uint32_t at = snx_ld_le32(r), type = snx_ld_le16(r + 8), P = s->rva + at;
            uint64_t S;
            int absolute, target_section;
            if (type == REL_ABSOLUTE) continue;
            if (at + 4 > s->size || (type == REL_ADDR64 && at + 8 > s->size)) {
                snprintf(error, error_size, "%s: relocation outside section %s", o->input->name, s->name);
                goto done;
            }
            if (resolve(&l, order[k].object, snx_ld_le32(r + 4), &S, &absolute, &target_section)) goto done;
            unsigned char* p = image[s->output].data + (P - l.out_rva[s->output]);
            switch (type) {
                case REL_ADDR64: {
                    uint64_t addend = (uint64_t)snx_ld_le32(p) | (uint64_t)snx_ld_le32(p + 4) << 32;
                    snx_st_le64(p, (absolute ? S : IMAGE_BASE + S) + addend);
                    if (!absolute) {
                        if (nbases == cap_bases) {
                            uint32_t cap = cap_bases ? cap_bases * 2 : 1024;
                            uint32_t* grown = (uint32_t*)realloc(bases, cap * sizeof(*bases));
                            if (!grown) {
                                snprintf(error, error_size, "out of memory");
                                goto done;
                            }
                            bases = grown;
                            cap_bases = cap;
                        }
                        bases[nbases++] = P;
                    }
                    break;
                }
                case REL_ADDR32:
                    if (!absolute) {
                        snprintf(error, error_size, "%s: 32-bit absolute address in %s", o->input->name, s->name);
                        goto done;
                    }
                    snx_st_le32(p, (uint32_t)(S + snx_ld_le32(p)));
                    break;
                case REL_ADDR32NB: snx_st_le32(p, (uint32_t)(S + snx_ld_le32(p))); break;
                case REL_SECTION: snx_st_le16(p, target_section >= 0 ? (uint32_t)target_section + 1 : 0); break;
                case REL_SECREL:
                    snx_st_le32(p, (uint32_t)(S - (target_section >= 0 ? l.out_rva[target_section] : 0)
                                              + snx_ld_le32(p)));
                    break;
                default:
                    if (type >= REL_REL32 && type <= REL_REL32_5) {
                        /* the addend is the field's signed value, relative to the end of the instruction */
                        if (snx_link_rel32(p, S + (uint64_t)(int64_t)(int32_t)snx_ld_le32(p),
                                           P + 4 + (type - REL_REL32))) {
                            snprintf(error, error_size, "%s: relocation out of range", o->input->name);
                            goto done;
                        }
                        break;
                    }
                    snprintf(error, error_size, "%s: unsupported relocation type %u", o->input->name, type);
                    goto done;
            }
        }
    }
    for (size_t k = 0; k < l.imports.capacity; ++k)
        if (l.imports.keys[k]) {
            size_t found;
            uint64_t S;
            int absolute, section;
            uint32_t slot = l.imports_rva + 8 * (uint32_t)l.imports.values[k];
            snx_link_find(&l.globals, l.imports.keys[k] + 6, &found);
            if (resolve(&l, found >> 32, (uint32_t)found, &S, &absolute, &section)) goto done;
            snx_st_le64(image[O_DATA].data + (slot - l.out_rva[O_DATA]), IMAGE_BASE + S);
            if (nbases == cap_bases) {
                uint32_t cap = cap_bases ? cap_bases * 2 : 1024;
                uint32_t* grown = (uint32_t*)realloc(bases, cap * sizeof(*bases));
                if (!grown) {
                    snprintf(error, error_size, "out of memory");
                    goto done;
                }
                bases = grown;
                cap_bases = cap;
            }
            bases[nbases++] = slot;
        }
    /* RtlLookupFunctionEntry binary-searches .pdata: sort the RUNTIME_FUNCTIONs by start address */
    if (l.out_size[O_PDATA] % 12 == 0)
        qsort(image[O_PDATA].data, l.out_size[O_PDATA] / 12, 12, snx_coff_compare_function);
    /* export directory */
    if (export_count) {
        uint32_t functions = edata_rva + 40, names = functions + 4 * (uint32_t)export_count,
                 ordinals = names + 4 * (uint32_t)export_count, strings = ordinals + 2 * (uint32_t)export_count;
        unsigned char* e = image[O_RDATA].data + (edata_rva - l.out_rva[O_RDATA]);
        memset(e, 0, 40);
        snx_st_le32(e + 12, strings); /* Name */
        snx_st_le32(e + 16, 1);       /* Base */
        snx_st_le32(e + 20, (uint32_t)export_count);
        snx_st_le32(e + 24, (uint32_t)export_count);
        snx_st_le32(e + 28, functions);
        snx_st_le32(e + 32, names);
        snx_st_le32(e + 36, ordinals);
        memcpy(image[O_RDATA].data + (strings - l.out_rva[O_RDATA]), soname, strlen(soname) + 1);
        strings += (uint32_t)strlen(soname) + 1;
        for (size_t i = 0; i < export_count; ++i) {
            size_t found;
            uint64_t S;
            int absolute, section;
            if (!snx_link_find(&l.globals, sorted[i], &found)) {
                snprintf(error, error_size, "exported symbol %s is not defined", sorted[i]);
                goto done;
            }
            if (resolve(&l, found >> 32, (uint32_t)found, &S, &absolute, &section)) goto done;
            snx_st_le32(image[O_RDATA].data + (functions + 4 * i - l.out_rva[O_RDATA]), (uint32_t)S);
            snx_st_le32(image[O_RDATA].data + (names + 4 * i - l.out_rva[O_RDATA]), strings);
            snx_st_le16(image[O_RDATA].data + (ordinals + 2 * i - l.out_rva[O_RDATA]), (uint32_t)i);
            memcpy(image[O_RDATA].data + (strings - l.out_rva[O_RDATA]), sorted[i], strlen(sorted[i]) + 1);
            strings += (uint32_t)strlen(sorted[i]) + 1;
        }
    }
    /* base relocations: one block per 4 KiB page */
    if (nbases) qsort(bases, nbases, sizeof(*bases), compare_u32);
    for (uint32_t j = 0; j < nbases;) {
        uint32_t page = bases[j] & ~0xFFFu, start = (uint32_t)image[O_RELOC].size, entries = 0;
        unsigned char header[8], entry[2];
        snx_link_append(&image[O_RELOC], header, 8);
        for (; j < nbases && (bases[j] & ~0xFFFu) == page; ++j, ++entries) {
            snx_st_le16(entry, (10u << 12) | (bases[j] & 0xFFF)); /* IMAGE_REL_BASED_DIR64 */
            snx_link_append(&image[O_RELOC], entry, 2);
        }
        if (entries & 1) {
            snx_st_le16(entry, 0);
            snx_link_append(&image[O_RELOC], entry, 2);
        }
        snx_st_le32(image[O_RELOC].data + start, page);
        snx_st_le32(image[O_RELOC].data + start + 4, (uint32_t)image[O_RELOC].size - start);
    }
    l.out_size[O_RELOC] = (uint32_t)image[O_RELOC].size;
    l.out_rva[O_RELOC] = rva;
    /* file layout */
    used[O_RELOC] = l.out_size[O_RELOC] != 0;
    if (used[O_RELOC]) rva = snx_align_up(rva + l.out_size[O_RELOC], SECTION_ALIGNMENT);
    for (int o_index = 0; o_index < O_COUNT; ++o_index)
        if (used[o_index]) ++nout;
    snx_link_reserve(&out, headers);
    for (int o_index = 0; o_index < O_COUNT; ++o_index) {
        if (!used[o_index]) continue;
        if (o_index == O_BSS) {
            l.out_raw[o_index] = 0;
            l.out_raw_size[o_index] = 0;
            continue;
        }
        snx_link_align(&out, FILE_ALIGNMENT);
        l.out_raw[o_index] = (uint32_t)snx_link_append(&out, image[o_index].data, l.out_size[o_index]);
        snx_link_align(&out, FILE_ALIGNMENT);
        l.out_raw_size[o_index] = snx_align_up(l.out_size[o_index], FILE_ALIGNMENT);
    }
    unsigned char* p = out.data;
    /* DOS header with e_lfanew; the stub is never run */
    p[0] = 'M';
    p[1] = 'Z';
    snx_st_le32(p + 0x3C, 0x80);
    memcpy(p + 0x80, "PE\0\0", 4);
    p += 0x84;
    snx_st_le16(p, 0x8664);
    snx_st_le16(p + 2, (uint32_t)nout);
    snx_st_le16(p + 16, 240);
    snx_st_le16(p + 18, 0x0002 | 0x0020 | 0x2000); /* executable image, large address aware, DLL */
    p += 20;
    snx_st_le16(p, 0x20B);
    p[2] = 14;
    snx_st_le32(p + 4, l.out_raw_size[O_TEXT]);
    snx_st_le32(p + 8, l.out_raw_size[O_RDATA] + l.out_raw_size[O_DATA] + l.out_raw_size[O_PDATA]
                + l.out_raw_size[O_RELOC]);
    snx_st_le32(p + 12, snx_align_up(l.out_size[O_BSS], FILE_ALIGNMENT));
    snx_st_le32(p + 20, l.out_rva[O_TEXT]);
    snx_st_le64(p + 24, IMAGE_BASE);
    snx_st_le32(p + 32, SECTION_ALIGNMENT);
    snx_st_le32(p + 36, FILE_ALIGNMENT);
    snx_st_le16(p + 40, 6);
    snx_st_le16(p + 48, 6);
    snx_st_le32(p + 56, rva); /* SizeOfImage: the end of the last section */
    snx_st_le32(p + 60, headers);
    snx_st_le16(p + 68, 3);                        /* console subsystem */
    snx_st_le16(p + 70, 0x0020 | 0x0040 | 0x0100); /* high entropy VA, dynamic base, NX compatible */
    snx_st_le64(p + 72, 0x100000);
    snx_st_le64(p + 80, 0x1000);
    snx_st_le64(p + 88, 0x100000);
    snx_st_le64(p + 96, 0x1000);
    snx_st_le32(p + 108, 16);
    if (export_count) {
        snx_st_le32(p + 112, edata_rva);
        snx_st_le32(p + 116, edata_size);
    }
    if (l.out_size[O_PDATA]) {
        snx_st_le32(p + 112 + 3 * 8, l.out_rva[O_PDATA]);
        snx_st_le32(p + 116 + 3 * 8, l.out_size[O_PDATA]);
    }
    if (l.out_size[O_RELOC]) {
        snx_st_le32(p + 112 + 5 * 8, l.out_rva[O_RELOC]);
        snx_st_le32(p + 116 + 5 * 8, l.out_size[O_RELOC]);
    }
    p += 240;
    for (int o_index = 0; o_index < O_COUNT; ++o_index) {
        if (!used[o_index]) continue;
        memcpy(p, output_names[o_index], strlen(output_names[o_index]));
        snx_st_le32(p + 8, l.out_size[o_index]);
        snx_st_le32(p + 12, l.out_rva[o_index]);
        snx_st_le32(p + 16, l.out_raw_size[o_index]);
        snx_st_le32(p + 20, l.out_raw[o_index]);
        snx_st_le32(p + 36, output_flags[o_index]);
        p += 40;
    }
    output->data = out.data;
    output->size = out.size;
    out.data = NULL;
    rc = 0;
done:
    for (size_t i = 0; l.objs && i < count; ++i) free(l.objs[i].sections);
    free(l.objs);
    free(order);
    free(bases);
    free(sorted);
    for (int o_index = 0; o_index < O_COUNT; ++o_index) free(image[o_index].data);
    free(out.data);
    snx_link_names_free(&l.globals);
    snx_link_names_free(&l.comdats);
    snx_link_names_free(&l.imports);
    return rc;
}
