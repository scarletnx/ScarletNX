/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The defaults of a program without an NPDM follow Eden (src/core/file_sys/program_metadata.cpp, GetDefault):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project (GPL-3.0-or-later)
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project (GPL-2.0-or-later) */
/* The recompiler's loader: NSO and NRO modules decoded (LZ4, SHA-256) and laid out at the run-time bases, MOD0, the
   ELF dynamic table (symbols, RELA, REL and RELR relocations, hash tables), .eh_frame_hdr, NPDM and control.nacp.
   Written from Switchbrew's NSO, NRO, MOD0, NPDM and NACP pages, the ELF and LSB specifications (dynamic section,
   relocations, DT_RELR, DWARF pointer encodings) and what the callers expect */
#include "nx/loader/loader.h"
#include <stdlib.h>
#include <string.h>
#include <lz4.h>
#include <openssl/evp.h>

#define NSO_HEADER_SIZE  0x100
#define NRO_HEADER_SIZE  0x80
#define ASET_HEADER_SIZE 0x38
#define MOD0_SIZE        0x18
#define GUARD_SIZE       SNX_PAGE_SIZE
/* where each kind of program's code region ends */
#define CODE_END64 0x2000000000ull
#define CODE_END32 ((SnxU64)0x40000000UL)

/* ELF dynamic tags read (the ones below DT_COUNT by number) */
#define DT_HASH     4
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_RELAENT  9
#define DT_STRSZ    10
#define DT_SYMENT   11
#define DT_REL      17
#define DT_RELSZ    18
#define DT_RELENT   19
#define DT_RELRSZ   35
#define DT_RELR     36
#define DT_RELRENT  37
#define DT_COUNT    38
#define DT_GNU_HASH 0x6ffffef5UL

#define R_AARCH64_RELATIVE 1027
#define R_ARM_RELATIVE     23
#define STT_FUNC           2

static const char* const module_names[] = {"rtld",    "main",    "subsdk0", "subsdk1", "subsdk2",
                                           "subsdk3", "subsdk4", "subsdk5", "subsdk6", "subsdk7",
                                           "subsdk8", "subsdk9", "sdk"};
#define MODULE_NAMES (sizeof(module_names) / sizeof(module_names[0]))

/* ------------------------------------------------------------------ small helpers */

/* Copies message (allocated, consumed) into error, cut to fit and always terminated. Returns -1 */
static int fail(char* error, size_t error_size, char* message) {
    size_t n = strlen(message);
    if (error_size) {
        if (n > error_size - 1) n = error_size - 1;
        memcpy(error, message, n);
        error[n] = 0;
    }
    free(message);
    return -1;
}

static SnxU32 rd16(const unsigned char* p) { return (SnxU32)p[0] | (SnxU32)p[1] << 8; }
static SnxU32 rd32(const unsigned char* p) {
    return (SnxU32)p[0] | (SnxU32)p[1] << 8 | (SnxU32)p[2] << 16 | (SnxU32)p[3] << 24;
}
static SnxU64 rd64(const unsigned char* p) { return (SnxU64)rd32(p) | (SnxU64)rd32(p + 4) << 32; }
/* a signed 32-bit field widened to 64 bits, two's complement */
static SnxU64 sext32(SnxU32 v) { return (SnxU64)v | (v & 0x80000000UL ? 0xFFFFFFFF00000000ull : 0); }

/* whether [offset, offset + length) lies inside [0, total) */
static int inside(SnxU64 offset, SnxU64 length, SnxU64 total) { return offset <= total && length <= total - offset; }

static SnxU64 page_up(SnxU64 v) { return (v + SNX_PAGE_SIZE - 1) & ~(SnxU64)(SNX_PAGE_SIZE - 1); }

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }
static int separator(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}
/* the last component of path, ignoring trailing separators: its start and its length */
static const char* leaf(const char* path, size_t* length) {
    size_t end = strlen(path), start;
    while (end > 1 && separator(path[end - 1])) --end;
    start = end;
    while (start && !separator(path[start - 1])) --start;
    *length = end - start;
    return path + start;
}
static int same_word(const char* s, size_t n, const char* word) {
    size_t i;
    if (strlen(word) != n) return 0;
    for (i = 0; i < n; ++i)
        if (lower(s[i]) != word[i]) return 0;
    return 1;
}
static int nro_name(const char* path) {
    size_t n = strlen(path);
    return n >= 4 && path[n - 4] == '.' && same_word(path + n - 3, 3, "nro");
}

/* A growable list of addresses, for the seeds while a module is read */
typedef struct {
    SnxU64* v;
    size_t n, cap;
} List;
static void push(List* l, SnxU64 value) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->v = (SnxU64*)snx_realloc(l->v, l->cap * sizeof(*l->v));
    }
    l->v[l->n++] = value;
}
static int u64_order(const void* a, const void* b) {
    SnxU64 x = *(const SnxU64*)a, y = *(const SnxU64*)b;
    return x < y ? -1 : x > y;
}
static void sort_unique(List* l) {
    size_t i, kept = 0;
    if (!l->n) return;
    qsort(l->v, l->n, sizeof(*l->v), u64_order);
    for (i = 0; i < l->n; ++i)
        if (!kept || l->v[kept - 1] != l->v[i]) l->v[kept++] = l->v[i];
    l->n = kept;
}

/* ------------------------------------------------------------------ one module's metadata */

/* What reading a module's metadata works with */
typedef struct {
    SnxModuleImage* m;
    int is64;
    unsigned word; /* an address's size in the module: 8, or 4 for ELF32 */
    List functions, relptrs, fdes, exported;
    char* error;
    size_t error_size;
} Parse;

/* A code address discovery may start from: inside .text, a whole instruction. AArch32 addresses carry the Thumb bit
   (bit 0), so an odd one is Thumb code and one ending in binary 10 is neither */
static int code_address(const Parse* p, SnxU64 a) {
    SnxU64 lo = p->m->text_off, hi = lo + p->m->text_size;
    if (p->is64) {
        if (a & 3) return 0;
    } else {
        if ((a & 3) == 2) return 0;
        a &= ~(SnxU64)1;
    }
    return a >= lo && a < hi;
}

static SnxU64 word_at(const Parse* p, SnxU64 offset) {
    return p->word == 8 ? rd64(p->m->image + offset) : rd32(p->m->image + offset);
}

/* MOD0's displacement field at offset `at` from MOD0 (at mod0) as a module offset. bss_end alone may be read unsigned:
   a module of several GiB puts its BSS end more than 2 GiB past MOD0, where the signed reading would land before the
   module's start */
static int mod0_field(const Parse* p, SnxU64 mod0, unsigned at, int may_be_unsigned, SnxU64* out) {
    SnxU32 raw = rd32(p->m->image + mod0 + at);
    SnxU64 magnitude;
    if (!(raw & 0x80000000UL)) {
        *out = mod0 + raw;
        return 0;
    }
    magnitude = ((SnxU64)1 << 32) - raw;
    if (magnitude <= mod0)
        *out = mod0 - magnitude;
    else if (may_be_unsigned)
        *out = mod0 + raw;
    else
        return -1;
    return 0;
}

static int read_mod0(Parse* p) {
    SnxModuleImage* m = p->m;
    SnxMod0* h = &m->mod0;
    SnxU64 at;
    if (m->image_size < 8) return 0;
    at = rd32(m->image + 4);
    /* no MOD0 is allowed (small homebrew): no dynamic table, nothing to seed from */
    if (!inside(at, MOD0_SIZE, m->image_size) || memcmp(m->image + at, "MOD0", 4)) return 0;
    h->off = at;
    if (mod0_field(p, at, 4, 0, &h->dynamic) || mod0_field(p, at, 8, 0, &h->bss_start) ||
        mod0_field(p, at, 12, 1, &h->bss_end) || mod0_field(p, at, 16, 0, &h->ehf_start) ||
        mod0_field(p, at, 20, 0, &h->ehf_end))
        return fail(p->error, p->error_size, snx_format("%s: MOD0 points before the module", m->path));
    if (h->bss_start > h->bss_end || h->bss_end > m->size)
        return fail(p->error, p->error_size, snx_format("%s: MOD0's BSS lies outside the module", m->path));
    if (h->ehf_start > h->ehf_end || h->ehf_end > m->image_size)
        return fail(p->error, p->error_size, snx_format("%s: MOD0's .eh_frame_hdr lies outside the image", m->path));
    if (h->dynamic >= m->image_size)
        return fail(p->error, p->error_size, snx_format("%s: MOD0's dynamic table lies outside the image", m->path));
    m->has_mod0 = 1;
    return 0;
}

typedef struct {
    SnxU64 value[DT_COUNT], gnu_hash;
    unsigned char have[DT_COUNT], have_gnu_hash;
} Dynamic;

static int read_dynamic(Parse* p, Dynamic* d) {
    SnxModuleImage* m = p->m;
    SnxU64 at = m->mod0.dynamic, tag, value, entry = 2 * (SnxU64)p->word;
    memset(d, 0, sizeof(*d));
    for (;; at += entry) {
        if (!inside(at, entry, m->image_size))
            return fail(p->error, p->error_size, snx_format("%s: the dynamic table runs past the image", m->path));
        tag = word_at(p, at);
        value = word_at(p, at + p->word);
        if (!tag) return 0;
        /* the first of each tag counts */
        if (tag < DT_COUNT && !d->have[tag]) {
            d->have[tag] = 1;
            d->value[tag] = value;
        } else if (tag == DT_GNU_HASH && !d->have_gnu_hash) {
            d->have_gnu_hash = 1;
            d->gnu_hash = value;
        }
    }
}

/* The number of symbols in the table: DT_HASH's chain count, else the end of DT_GNU_HASH's last chain, else
   (neither, as linkers may leave out) the room between the symbol table and the string table after it */
static int symbol_count(Parse* p, const Dynamic* d, SnxU64 entry, SnxU64* count) {
    SnxModuleImage* m = p->m;
    SnxU64 at, buckets, first, blooms, chains, i, last = 0;
    if (d->have[DT_HASH]) {
        if (!inside(d->value[DT_HASH], 8, m->image_size))
            return fail(p->error, p->error_size, snx_format("%s: DT_HASH lies outside the image", m->path));
        *count = rd32(m->image + d->value[DT_HASH] + 4);
        return 0;
    }
    if (d->have_gnu_hash) {
        at = d->gnu_hash;
        if (!inside(at, 16, m->image_size))
            return fail(p->error, p->error_size, snx_format("%s: DT_GNU_HASH lies outside the image", m->path));
        buckets = rd32(m->image + at);
        first = rd32(m->image + at + 4);
        blooms = rd32(m->image + at + 8);
        at += 16 + blooms * p->word;
        if (!inside(at, buckets * 4, m->image_size))
            return fail(p->error, p->error_size, snx_format("%s: DT_GNU_HASH lies outside the image", m->path));
        for (i = 0; i < buckets; ++i)
            if (rd32(m->image + at + i * 4) > last) last = rd32(m->image + at + i * 4);
        if (!last) {
            *count = first;
            return 0;
        }
        if (last < first)
            return fail(p->error, p->error_size, snx_format("%s: DT_GNU_HASH is inconsistent", m->path));
        chains = at + buckets * 4;
        for (;; ++last) {
            SnxU64 link = chains + (last - first) * 4;
            if (!inside(link, 4, m->image_size))
                return fail(p->error, p->error_size, snx_format("%s: DT_GNU_HASH runs past the image", m->path));
            if (rd32(m->image + link) & 1) break;
        }
        *count = last + 1;
        return 0;
    }
    *count = d->value[DT_STRTAB] > d->value[DT_SYMTAB] ? (d->value[DT_STRTAB] - d->value[DT_SYMTAB]) / entry : 0;
    return 0;
}

/* A defined symbol before deduplication, with its table index to keep the last of equal values */
typedef struct {
    SnxU64 value, size, index;
    SnxU32 name;
    unsigned type;
} Candidate;
static int candidate_order(const void* a, const void* b) {
    const Candidate *x = (const Candidate*)a, *y = (const Candidate*)b;
    if (x->value != y->value) return x->value < y->value ? -1 : 1;
    return x->index < y->index ? -1 : x->index > y->index;
}

/* the symbols: defined ones (named, not undefined, NOTYPE/OBJECT/FUNC, value not 0) one per value, the last in the
   table winning; undefined ones as imports; functions in .text as seeds */
static int read_symbols(Parse* p, const Dynamic* d) {
    SnxModuleImage* m = p->m;
    SnxU64 entry = p->is64 ? 24 : 16, symtab, strtab, strsz, count, i;
    Candidate* defined = NULL;
    SnxU32* imported = NULL;
    size_t defined_n = 0, imported_n = 0, bytes, kept, k;
    char* names;
    if (!d->have[DT_SYMTAB]) return 0;
    if (d->have[DT_SYMENT] && d->value[DT_SYMENT] != entry)
        return fail(p->error, p->error_size, snx_format("%s: invalid symbol entry size", m->path));
    if (!d->have[DT_STRTAB])
        return fail(p->error, p->error_size, snx_format("%s: a symbol table without strings", m->path));
    symtab = d->value[DT_SYMTAB];
    strtab = d->value[DT_STRTAB];
    strsz = d->have[DT_STRSZ] ? d->value[DT_STRSZ] : 0;
    if (!inside(strtab, strsz, m->image_size))
        return fail(p->error, p->error_size, snx_format("%s: the string table lies outside the image", m->path));
    if (symbol_count(p, d, entry, &count)) return -1;
    if (count > m->image_size || !inside(symtab, count * entry, m->image_size))
        return fail(p->error, p->error_size, snx_format("%s: the symbol table lies outside the image", m->path));
    if (count) {
        defined = (Candidate*)snx_alloc((size_t)count * sizeof(*defined));
        imported = (SnxU32*)snx_alloc((size_t)count * sizeof(*imported));
    }
    for (i = 0; i < count; ++i) {
        const unsigned char* s = m->image + symtab + i * entry;
        SnxU32 name = rd32(s);
        SnxU64 value, size;
        unsigned type, shndx;
        if (p->is64) {
            type = s[4] & 15;
            shndx = rd16(s + 6);
            value = rd64(s + 8);
            size = rd64(s + 16);
        } else {
            value = rd32(s + 4);
            size = rd32(s + 8);
            type = s[12] & 15;
            shndx = rd16(s + 14);
        }
        if (!name) continue;
        if (name >= strsz || !memchr(m->image + strtab + name, 0, (size_t)(strsz - name))) {
            free(defined);
            free(imported);
            return fail(p->error, p->error_size,
                        snx_format("%s: symbol %zu: symbol name outside the string table", m->path, i));
        }
        if (!shndx) {
            imported[imported_n++] = name;
            continue;
        }
        if (type == STT_FUNC && code_address(p, value)) {
            push(&p->functions, value);
            push(&p->exported, value);
        }
        if (type <= STT_FUNC && value) {
            defined[defined_n].value = value;
            defined[defined_n].size = size;
            defined[defined_n].index = i;
            defined[defined_n].name = name;
            defined[defined_n].type = type;
            ++defined_n;
        }
    }
    if (defined_n) {
        qsort(defined, defined_n, sizeof(*defined), candidate_order);
        for (i = 0, kept = 0; i < defined_n; ++i) {
            if (kept && defined[kept - 1].value == defined[i].value) --kept;
            defined[kept++] = defined[(size_t)i];
        }
        defined_n = kept;
    }
    /* one block, owned through symbols: the symbols, the imports, then the names both point at */
    if (defined_n || imported_n) {
        bytes = defined_n * sizeof(SnxSymbol) + imported_n * sizeof(char*);
        for (k = 0; k < defined_n; ++k) bytes += strlen((const char*)m->image + strtab + defined[k].name) + 1;
        for (k = 0; k < imported_n; ++k) bytes += strlen((const char*)m->image + strtab + imported[k]) + 1;
        m->symbols = (SnxSymbol*)snx_alloc(bytes);
        m->imports = (char**)(m->symbols + defined_n);
        names = (char*)(m->imports + imported_n);
        for (k = 0; k < defined_n; ++k) {
            const char* name = (const char*)m->image + strtab + defined[k].name;
            size_t n = strlen(name) + 1;
            m->symbols[k].value = defined[k].value;
            m->symbols[k].size = defined[k].size;
            m->symbols[k].type = defined[k].type;
            m->symbols[k].name = names;
            memcpy(names, name, n);
            names += n;
        }
        for (k = 0; k < imported_n; ++k) {
            const char* name = (const char*)m->image + strtab + imported[k];
            size_t n = strlen(name) + 1;
            m->imports[k] = names;
            memcpy(names, name, n);
            names += n;
        }
        m->symbol_count = defined_n;
        m->import_count = imported_n;
    }
    free(defined);
    free(imported);
    return 0;
}

/* A relative relocation of the word at target: its addend, explicit or (REL, RELR) the word already there, or 0 for
   a word in the BSS. A code address among them is a seed: vtables and callbacks point at functions nothing calls */
static int relocate(Parse* p, SnxU64 target, int explicit_addend, SnxU64 addend) {
    SnxModuleImage* m = p->m;
    if (!inside(target, p->word, m->size))
        return fail(p->error, p->error_size, snx_format("%s: relocation target exceeds module", m->path));
    if (!explicit_addend) addend = inside(target, p->word, m->image_size) ? word_at(p, target) : 0;
    if (code_address(p, addend)) push(&p->relptrs, addend);
    return 0;
}

/* DT_RELA or DT_REL (words: 3 or 2 per entry). DT_JMPREL isn't read: its JUMP_SLOTs point at imports */
static int read_relocations(Parse* p, const Dynamic* d, unsigned table, unsigned size_tag, unsigned entry_tag,
                            unsigned words, const char* kind) {
    SnxModuleImage* m = p->m;
    SnxU64 entry = (SnxU64)words * p->word, size, at, info, type;
    unsigned relative = p->is64 ? R_AARCH64_RELATIVE : R_ARM_RELATIVE;
    if (!d->have[table]) return 0;
    size = d->have[size_tag] ? d->value[size_tag] : 0;
    if ((d->have[entry_tag] && d->value[entry_tag] != entry) || size % entry)
        return fail(p->error, p->error_size, snx_format("%s: invalid %s entry size", m->path, kind));
    if (!inside(d->value[table], size, m->image_size))
        return fail(p->error, p->error_size, snx_format("%s: the %s table lies outside the image", m->path, kind));
    for (at = d->value[table]; at < d->value[table] + size; at += entry) {
        info = word_at(p, at + p->word);
        type = p->is64 ? info & 0xFFFFFFFFUL : info & 0xFF;
        if (type != relative) continue;
        if (relocate(p, word_at(p, at), words == 3, words == 3 ? word_at(p, at + 2 * p->word) : 0)) return -1;
    }
    return 0;
}

/* DT_RELR, relative relocations packed as words: an even entry is the address of a word to relocate, and the next
   bitmap covers the words after it; an odd entry is that bitmap, whose bit i (from bit 1) marks the word i words
   after where it starts, and moves the start on by its number of bits */
static int read_relr(Parse* p, const Dynamic* d) {
    SnxModuleImage* m = p->m;
    SnxU64 size, at, end, entry, where = 0, bits;
    unsigned span = p->word * 8 - 1, i;
    if (!d->have[DT_RELR]) return 0;
    size = d->have[DT_RELRSZ] ? d->value[DT_RELRSZ] : 0;
    if ((d->have[DT_RELRENT] && d->value[DT_RELRENT] != p->word) || size % p->word)
        return fail(p->error, p->error_size, snx_format("%s: invalid RELR entry size", m->path));
    if (!inside(d->value[DT_RELR], size, m->image_size))
        return fail(p->error, p->error_size, snx_format("%s: the RELR table lies outside the image", m->path));
    end = d->value[DT_RELR] + size;
    for (at = d->value[DT_RELR]; at < end; at += p->word) {
        entry = word_at(p, at);
        if (!(entry & 1)) {
            if (relocate(p, entry, 0, 0)) return -1;
            where = entry + p->word;
            continue;
        }
        bits = entry >> 1;
        for (i = 0; i < span && bits; ++i, bits >>= 1)
            if ((bits & 1) && relocate(p, where + (SnxU64)i * p->word, 0, 0)) return -1;
        where += (SnxU64)span * p->word;
    }
    return 0;
}

/* ---- unwind information: the ranges of the functions .eh_frame describes */

/* An unsigned or signed LEB128 at *at, before end */
static int leb128(const unsigned char* image, SnxU64 end, SnxU64* at, int is_signed, SnxU64* out) {
    SnxU64 value = 0;
    unsigned shift = 0;
    unsigned char byte;
    do {
        if (*at >= end || shift >= 64) return -1;
        byte = image[(*at)++];
        value |= (SnxU64)(byte & 0x7f) << shift;
        shift += 7;
    } while (byte & 0x80);
    if (is_signed && shift < 64 && (byte & 0x40)) value |= ~(SnxU64)0 << shift;
    *out = value;
    return 0;
}

/* A DW_EH_PE-encoded pointer at *at, before end: its format (low nibble), applied relative to the field (pcrel) or to
   base (datarel). Results are module offsets */
static int encoded(const unsigned char* image, SnxU64 end, SnxU64* at, unsigned encoding, SnxU64 base, SnxU64* out) {
    SnxU64 field = *at, value;
    unsigned size = 0;
    if (encoding == 0xff || (encoding & 0x80)) return -1;
    switch (encoding & 0x0f) {
        case 0x00: size = 8; break;
        case 0x01: if (leb128(image, end, at, 0, &value)) return -1; break;
        case 0x02: size = 2; break;
        case 0x03: size = 4; break;
        case 0x04: size = 8; break;
        case 0x09: if (leb128(image, end, at, 1, &value)) return -1; break;
        case 0x0a: size = 2; break;
        case 0x0b: size = 4; break;
        case 0x0c: size = 8; break;
        default: return -1;
    }
    if (size) {
        if (!inside(*at, size, end)) return -1;
        value = size == 2 ? rd16(image + *at) : size == 4 ? rd32(image + *at) : rd64(image + *at);
        if (size == 2 && (encoding & 0x08) && (value & 0x8000)) value |= ~(SnxU64)0xffff;
        if (size == 4 && (encoding & 0x08)) value = sext32((SnxU32)value);
        *at += size;
    }
    switch (encoding & 0x70) {
        case 0x00: break;
        case 0x10: value += field; break;
        case 0x30: value += base; break;
        default: return -1;
    }
    *out = value;
    return 0;
}

/* The pointer encoding of the FDEs a CIE introduces, or the reason it can't be read */
static const char* read_cie(const unsigned char* image, SnxU64 size, SnxU64 cie, unsigned* fde_encoding) {
    SnxU64 length, end, at, value;
    const char* augmentation;
    unsigned version;
    *fde_encoding = 0;
    if (!inside(cie, 9, size)) return "CIE outside the image";
    length = rd32(image + cie);
    if (!length || length == 0xFFFFFFFFUL) return "unsupported CIE length";
    end = cie + 4 + length;
    if (!inside(cie, 4 + length, size)) return "CIE outside the image";
    if (rd32(image + cie + 4)) return "FDE points at another FDE";
    version = image[cie + 8];
    if (version != 1 && version != 3) return "unsupported CIE version";
    augmentation = (const char*)image + cie + 9;
    if (!memchr(augmentation, 0, (size_t)(end - cie - 9))) return "unterminated augmentation";
    at = cie + 9 + strlen(augmentation) + 1;
    if (leb128(image, end, &at, 0, &value) || leb128(image, end, &at, 1, &value)) return "truncated CIE";
    if (version == 1) {
        if (at >= end) return "truncated CIE";
        ++at;
    } else if (leb128(image, end, &at, 0, &value))
        return "truncated CIE";
    if (!*augmentation) return NULL;
    if (*augmentation != 'z') return "unsupported augmentation";
    if (leb128(image, end, &at, 0, &value)) return "truncated CIE";
    for (++augmentation; *augmentation; ++augmentation) {
        unsigned char encoding;
        switch (*augmentation) {
            case 'R':
                if (at >= end) return "truncated CIE";
                *fde_encoding = image[at++];
                break;
            case 'P':
                if (at >= end) return "truncated CIE";
                encoding = image[at++];
                if (encoded(image, end, &at, encoding & 0x7f, 0, &value)) return "unreadable personality";
                break;
            case 'L':
                if (at >= end) return "truncated CIE";
                ++at;
                break;
            case 'S':
            case 'B': break;
            default: return "unknown augmentation";
        }
    }
    return NULL;
}

/* The range an FDE covers, or the reason it can't be read */
static const char* read_fde(const unsigned char* image, SnxU64 size, SnxU64 fde, SnxU64* start, SnxU64* length) {
    SnxU64 total, end, pointer, at;
    unsigned encoding;
    const char* why;
    if (!inside(fde, 8, size)) return "FDE outside the image";
    total = rd32(image + fde);
    if (!total || total == 0xFFFFFFFFUL) return "unsupported FDE length";
    if (!inside(fde, 4 + total, size)) return "FDE outside the image";
    end = fde + 4 + total;
    pointer = rd32(image + fde + 4);
    if (!pointer) return "a CIE where an FDE was expected";
    if (pointer > fde + 4) return "CIE outside the image";
    why = read_cie(image, size, fde + 4 - pointer, &encoding);
    if (why) return why;
    at = fde + 8;
    if (encoded(image, end, &at, encoding, 0, start) || encoded(image, end, &at, encoding & 0x0f, 0, length))
        return "unreadable FDE range";
    return NULL;
}

/* .eh_frame_hdr's sorted table, each of its FDEs read for the function's start and size. A damaged one stops the
   walk: what was read before it stays, eh_error says where it stopped */
static void read_unwind(Parse* p) {
    SnxModuleImage* m = p->m;
    const unsigned char* image = m->image;
    SnxU64 start = m->mod0.ehf_start, end = m->mod0.ehf_end, at, count, value, i, fde, size;
    const char* why = NULL;
    char* detail = NULL;
    if (!m->has_mod0 || end == start) return;
    if (end - start < 4)
        why = ".eh_frame_hdr is too short";
    else if (image[start] != 1)
        why = "unsupported .eh_frame_hdr version";
    else {
        at = start + 4;
        if (encoded(image, end, &at, image[start + 1], start, &value) ||
            encoded(image, end, &at, image[start + 2], start, &count))
            why = ".eh_frame_hdr has no search table";
        else if (image[start + 3] != 0x3b)
            why = "unsupported search table encoding";
        for (i = 0; !why && i < count; ++i) {
            if (!inside(at + i * 8, 8, end)) {
                why = "the search table runs past .eh_frame_hdr";
                break;
            }
            fde = start + sext32(rd32(image + at + i * 8 + 4));
            why = read_fde(image, m->image_size, fde, &value, &size);
            if (why) {
                detail = snx_format("FDE %zu: %s", i, why);
                break;
            }
            if (!code_address(p, value)) continue;
            push(&p->fdes, value);
            push(&p->fdes, size);
            push(&p->functions, value);
        }
    }
    if (why)
        m->eh_error = snx_format("stopped after %zu functions (%s)", p->fdes.n / 2,
                                 detail ? detail : why);
    free(detail);
}

/* The metadata of a decoded module (image, segments, size and path set): MOD0, the dynamic table and the unwind
   table, into the module's symbols, imports and seeds */
static int read_metadata(SnxModuleImage* m, int is64, char* error, size_t error_size) {
    Parse p;
    Dynamic d;
    size_t i, bytes;
    int rc = 0;
    memset(&p, 0, sizeof(p));
    p.m = m;
    p.is64 = is64;
    p.word = is64 ? 8 : 4;
    p.error = error;
    p.error_size = error_size;
    if (read_mod0(&p)) return -1;
    if (m->has_mod0)
        rc = read_dynamic(&p, &d) || read_symbols(&p, &d) ||
             read_relocations(&p, &d, DT_RELA, DT_RELASZ, DT_RELAENT, 3, "RELA") ||
             read_relocations(&p, &d, DT_REL, DT_RELSZ, DT_RELENT, 2, "REL") || read_relr(&p, &d);
    /* ELF32 modules unwind with .ARM.exidx, which MOD0 doesn't point at */
    if (!rc && is64) read_unwind(&p);
    if (!rc) {
        sort_unique(&p.functions);
        sort_unique(&p.relptrs);
        sort_unique(&p.exported);
        /* one block: the functions, the relptrs, the exported functions, then the FDEs */
        bytes = (p.functions.n + p.relptrs.n + p.exported.n + p.fdes.n) * sizeof(SnxU64);
        m->functions = (SnxU64*)snx_alloc(bytes);
        m->relptrs = m->functions + p.functions.n;
        m->exported = m->relptrs + p.relptrs.n;
        m->fde = (SnxFde*)(m->exported + p.exported.n);
        if (p.functions.n) memcpy(m->functions, p.functions.v, p.functions.n * sizeof(SnxU64));
        if (p.relptrs.n) memcpy(m->relptrs, p.relptrs.v, p.relptrs.n * sizeof(SnxU64));
        if (p.exported.n) memcpy(m->exported, p.exported.v, p.exported.n * sizeof(SnxU64));
        for (i = 0; i < p.fdes.n / 2; ++i) {
            m->fde[i].start = p.fdes.v[2 * i];
            m->fde[i].size = p.fdes.v[2 * i + 1];
        }
        m->function_count = p.functions.n;
        m->relptr_count = p.relptrs.n;
        m->exported_count = p.exported.n;
        m->fde_count = p.fdes.n / 2;
    }
    free(p.functions.v);
    free(p.relptrs.v);
    free(p.exported.v);
    free(p.fdes.v);
    return rc ? -1 : 0;
}

/* name and path in one allocation, name first */
static void set_names(SnxModuleImage* m, const char* name, const char* path) {
    size_t n = strlen(name) + 1, k = strlen(path) + 1;
    m->name = (char*)snx_alloc(n + k);
    memcpy(m->name, name, n);
    m->path = m->name + n;
    memcpy(m->path, path, k);
}

/* the module's mapping: its furthest segment or image end, then the BSS, in whole pages */
static void set_size(SnxModuleImage* m) {
    SnxU64 end = (SnxU64)m->data_off + m->data_size;
    if (end < m->image_size) end = m->image_size;
    m->size = page_up(end + m->bss);
}

void snx_free_module(SnxModuleImage* module) {
    free(module->name); /* the path too */
    free(module->image);
    free(module->nacp_data);
    free(module->symbols);   /* the imports too */
    free(module->functions); /* the relptrs, the exported functions and the FDEs too */
    free(module->eh_error);
    memset(module, 0, sizeof(*module));
}

/* ------------------------------------------------------------------ NSO */

static const char* const segment_names[3] = {"text", "rodata", "data"};

static int decode_nso(SnxModuleImage* m, const unsigned char* file, size_t file_size, char* error, size_t error_size) {
    SnxU32 flags, file_off[3], mem_off[3], size[3], stored[3];
    SnxU64 image_size = 0, end;
    unsigned i, j;
    if (file_size < NSO_HEADER_SIZE || memcmp(file, "NSO0", 4))
        return fail(error, error_size, snx_format("%s: not an NSO", m->path));
    flags = rd32(file + 0x0c);
    for (i = 0; i < 3; ++i) {
        file_off[i] = rd32(file + 0x10 + i * 0x10);
        mem_off[i] = rd32(file + 0x14 + i * 0x10);
        size[i] = rd32(file + 0x18 + i * 0x10);
        stored[i] = flags & (1U << i) ? rd32(file + 0x60 + i * 4) : size[i];
        if (!inside(file_off[i], stored[i], file_size))
            return fail(error, error_size,
                        snx_format("%s: the %s segment lies outside the file", m->path, segment_names[i]));
        end = (SnxU64)mem_off[i] + size[i];
        if (end > image_size) image_size = end;
    }
    if (image_size > 0xFFFFFFFFUL)
        return fail(error, error_size, snx_format("%s: the segments end past 4 GiB", m->path));
    for (i = 0; i < 3; ++i)
        for (j = i + 1; j < 3; ++j)
            if (size[i] && size[j] && mem_off[i] < (SnxU64)mem_off[j] + size[j] &&
                mem_off[j] < (SnxU64)mem_off[i] + size[i])
                return fail(error, error_size, snx_format("%s: the NSO's segments overlap", m->path));
    m->image = (unsigned char*)snx_alloc((size_t)image_size);
    memset(m->image, 0, (size_t)image_size);
    m->image_size = (size_t)image_size;
    for (i = 0; i < 3; ++i) {
        unsigned char* out = m->image + mem_off[i];
        const unsigned char* in = file + file_off[i];
        if (!(flags & (1U << i)))
            memcpy(out, in, size[i]);
        else if (size[i]) {
            if (stored[i] > LZ4_MAX_INPUT_SIZE || size[i] > LZ4_MAX_INPUT_SIZE)
                return fail(error, error_size,
                            snx_format("%s: the %s segment is too large for LZ4", m->path, segment_names[i]));
            if (LZ4_decompress_safe((const char*)in, (char*)out, (int)stored[i], (int)size[i]) != (int)size[i])
                return fail(error, error_size,
                            snx_format("%s: the %s segment's LZ4 data is corrupt", m->path, segment_names[i]));
        }
        if (flags & (8U << i)) {
            unsigned char digest[EVP_MAX_MD_SIZE];
            unsigned length = 0;
            if (!EVP_Digest(out, size[i], digest, &length, EVP_sha256(), NULL) || length != 32)
                return fail(error, error_size, snx_format("%s: out of memory hashing a segment", m->path));
            if (memcmp(digest, file + 0xa0 + i * 0x20, 32))
                return fail(error, error_size,
                            snx_format("%s: the %s segment's SHA-256 doesn't match", m->path, segment_names[i]));
        }
    }
    memcpy(m->build_id, file + 0x40, 32);
    m->text_off = mem_off[0];
    m->text_size = size[0];
    m->ro_off = mem_off[1];
    m->ro_size = size[1];
    m->data_off = mem_off[2];
    m->data_size = size[2];
    m->bss = rd32(file + 0x3c);
    set_size(m);
    return 0;
}

/* an NSO's bytes, read from path (a file, or a dump's ExeFS): named after its file without name */
static int decode_nso_file(unsigned char* file, size_t file_size, const char* path, const char* name, int is64,
                           SnxModuleImage* module, char* error, size_t error_size) {
    size_t n;
    const char* base;
    int rc;
    memset(module, 0, sizeof(*module));
    if (!name) { /* named after its file */
        char* own;
        base = leaf(path, &n);
        own = (char*)snx_alloc(n + 1);
        memcpy(own, base, n);
        own[n] = 0;
        set_names(module, own, path);
        free(own);
    } else
        set_names(module, name, path);
    rc = decode_nso(module, file, file_size, error, error_size) || read_metadata(module, is64, error, error_size);
    if (rc) snx_free_module(module);
    return rc ? -1 : 0;
}

static int load_nso(const char* path, const char* name, int is64, SnxModuleImage* module, char* error,
                    size_t error_size) {
    unsigned char* file;
    size_t file_size = 0;
    int rc;
    memset(module, 0, sizeof(*module));
    file = snx_read_file(path, &file_size);
    if (!file) return fail(error, error_size, snx_format("cannot read %s", path));
    rc = decode_nso_file(file, file_size, path, name, is64, module, error, error_size);
    free(file);
    return rc;
}

int snx_load_nso(const char* path, const char* name, SnxModuleImage* module, char* error, size_t error_size) {
    return load_nso(path, name, 1, module, error, error_size);
}

int snx_load_nso32(const char* path, const char* name, SnxModuleImage* module, char* error, size_t error_size) {
    return load_nso(path, name, 0, module, error, error_size);
}

/* ------------------------------------------------------------------ NRO */

/* The NRO's segments must be the image itself, one after another: text from 0 (at least its header), rodata, then
   data to the image's end. They need not be page aligned: the runtime rounds pages */
static int decode_nro(SnxModuleImage* m, const unsigned char* f, SnxU64 file_size, char* error, size_t error_size) {
    SnxU32 image_size, text_off, text_size, ro_off, ro_size, data_off, data_size;
    if (file_size < NRO_HEADER_SIZE || memcmp(f + 0x10, "NRO0", 4))
        return fail(error, error_size, snx_format("%s: not an NRO", m->path));
    if (rd32(f + 0x1c) & 2)
        return fail(error, error_size, snx_format("%s: compressed NRO images are not supported", m->path));
    image_size = rd32(f + 0x18);
    text_off = rd32(f + 0x20);
    text_size = rd32(f + 0x24);
    ro_off = rd32(f + 0x28);
    ro_size = rd32(f + 0x2c);
    data_off = rd32(f + 0x30);
    data_size = rd32(f + 0x34);
    if (image_size < NRO_HEADER_SIZE || image_size > file_size)
        return fail(error, error_size, snx_format("%s: the NRO's image size doesn't fit the file", m->path));
    if (text_off || text_size < NRO_HEADER_SIZE || ro_off != text_size || (SnxU64)ro_off + ro_size != data_off ||
        (SnxU64)data_off + data_size != image_size)
        return fail(error, error_size, snx_format("%s: the NRO's segments don't cover its image", m->path));
    m->image_size = image_size;
    m->text_off = text_off;
    m->text_size = text_size;
    m->ro_off = ro_off;
    m->ro_size = ro_size;
    m->data_off = data_off;
    m->data_size = data_size;
    m->bss = rd32(f + 0x38);
    memcpy(m->build_id, f + 0x40, 32);
    m->is_nro = 1;
    set_size(m);
    return 0;
}

/* the assets after the image (icon, control.nacp, RomFS), optional and never mapped, with file just past the image:
   the control.nacp read, the RomFS only located */
static int read_nro_assets(SnxModuleImage* m, FILE* file, SnxU64 file_size, char* error, size_t error_size) {
    unsigned char asset[ASET_HEADER_SIZE];
    SnxU64 asset_size = file_size - m->image_size, off, size;
    unsigned i;
    if (asset_size < ASET_HEADER_SIZE) return 0;
    if (fread(asset, 1, sizeof asset, file) != sizeof asset)
        return fail(error, error_size, snx_format("cannot read %s", m->path));
    if (memcmp(asset, "ASET", 4)) return 0;
    for (i = 0; i < 3; ++i) {
        off = rd64(asset + 0x08 + i * 0x10);
        size = rd64(asset + 0x10 + i * 0x10);
        if (size && !inside(off, size, asset_size))
            return fail(error, error_size, snx_format("%s: an ASET section lies outside the file", m->path));
        if (i == 1 && size) {
            m->nacp_data = (unsigned char*)snx_alloc((size_t)size);
            m->nacp_size = (size_t)size;
            if (snx_seek(file, m->image_size + off) || fread(m->nacp_data, 1, m->nacp_size, file) != m->nacp_size)
                return fail(error, error_size, snx_format("cannot read %s's control.nacp", m->path));
        } else if (i == 2 && size) {
            m->romfs_offset = m->image_size + off;
            m->romfs_size = size;
        }
    }
    return 0;
}

/* only the image and the control.nacp come into memory: an NRO's RomFS can make it gigabytes */
int snx_load_nro(const char* path, SnxModuleImage* module, char* error, size_t error_size) {
    unsigned char header[NRO_HEADER_SIZE];
    SnxU64 file_size;
    FILE* file;
    int rc;
    memset(module, 0, sizeof(*module));
    set_names(module, "main", path);
    file = snx_fopen(path, "rb");
    if (!file) {
        snx_free_module(module);
        return fail(error, error_size, snx_format("cannot read %s", path));
    }
    file_size = snx_file_size(path);
    if (fread(header, 1, sizeof header, file) != sizeof header && file_size >= sizeof header)
        rc = fail(error, error_size, snx_format("cannot read %s", path));
    else
        rc = decode_nro(module, header, file_size, error, error_size);
    if (!rc) {
        module->image = (unsigned char*)snx_alloc(module->image_size);
        memcpy(module->image, header, sizeof header);
        if (fread(module->image + sizeof header, 1, module->image_size - sizeof header, file) !=
            module->image_size - sizeof header)
            rc = fail(error, error_size, snx_format("cannot read %s", path));
    }
    if (!rc)
        rc = read_nro_assets(module, file, file_size, error, error_size) ||
             read_metadata(module, 1, error, error_size);
    fclose(file);
    if (rc) {
        snx_free_module(module);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ NPDM */

static void default_npdm(SnxNpdm* npdm) {
    memset(npdm, 0, sizeof(*npdm));
    npdm->title_id = SNX_NPDM_DEFAULT_TITLE;
    npdm->stack = 0x100000;
    npdm->prio = 44;
    npdm->aspace = 3; /* 39-bit */
    npdm->is64 = 1;
    strcpy(npdm->name, "Application");
}

int snx_parse_npdm(const unsigned char* data, size_t size, SnxNpdm* npdm, char* error, size_t error_size) {
    SnxU64 aci, aci_size;
    if (!data && !size) {
        default_npdm(npdm);
        return 0;
    }
    memset(npdm, 0, sizeof(*npdm));
    if (!data || size < 0x80) return fail(error, error_size, snx_strdup("NPDM: shorter than its header"));
    if (memcmp(data, "META", 4)) return fail(error, error_size, snx_strdup("NPDM: no META magic"));
    aci = rd32(data + 0x70);
    aci_size = rd32(data + 0x74);
    if (!inside(aci, aci_size, size))
        return fail(error, error_size, snx_strdup("NPDM: access-control region exceeds file"));
    if (aci_size < 0x18 || memcmp(data + aci, "ACI0", 4))
        return fail(error, error_size, snx_strdup("NPDM: no ACI0 in the access-control region"));
    npdm->is64 = data[0x0c] & 1;
    npdm->aspace = (data[0x0c] >> 1) & 7;
    npdm->prio = data[0x0e];
    npdm->core = data[0x0f];
    npdm->stack = rd32(data + 0x1c);
    memcpy(npdm->name, data + 0x20, 16);
    npdm->name[16] = 0;
    npdm->title_id = rd64(data + aci + 0x10);
    return 0;
}

/* ------------------------------------------------------------------ programs */

/* The ExeFS of an extracted program: <entry>/exefs, <entry>/ExeFS or entry itself, the first holding main */
static char* find_exefs(const char* entry) {
    static const char* const folders[] = {"exefs", "ExeFS", NULL};
    char *folder, *main_path;
    unsigned i;
    for (i = 0; i < 3; ++i) {
        folder = folders[i] ? snx_path_join(entry, folders[i]) : snx_strdup(entry);
        main_path = snx_path_join(folder, "main");
        if (snx_is_file(main_path)) {
            free(main_path);
            return folder;
        }
        free(main_path);
        free(folder);
    }
    return NULL;
}

/* an ExeFS folder's file, as the readers give it */
static int folder_read(void* source, const char* name, unsigned char** data, size_t* size) {
    char* path = snx_path_join((const char*)source, name);
    int found = snx_is_file(path);
    *data = found ? snx_read_file(path, size) : NULL;
    free(path);
    return !found ? 1 : *data ? 0 : -1;
}

/* a mod's file in place of the ExeFS's of its name, else the ExeFS's (SnxExefsMods) */
static int mods_read(void* source, const char* name, unsigned char** data, size_t* size) {
    const SnxExefsMods* mods = (const SnxExefsMods*)source;
    size_t i;
    for (i = 0; i < mods->count; ++i)
        if (!strcmp(mods->names[i], name)) {
            *data = snx_read_file(mods->paths[i], size);
            return *data ? 0 : -1;
        }
    return mods->under->read(mods->under->source, name, data, size);
}
void snx_exefs_mods(SnxExefsMods* mods, const SnxExefsReader* under, char* const* names, char* const* paths,
                    size_t count) {
    mods->reader.read = mods_read;
    mods->reader.source = mods;
    mods->under = under;
    mods->names = names;
    mods->paths = paths;
    mods->count = count;
}

/* main.npdm beside the modules, parsed, and its bytes (NULL without one); where names the ExeFS in messages */
static int read_npdm(const SnxExefsReader* exefs, const char* where, SnxNpdm* npdm, unsigned char** data, size_t* size,
                     char* error, size_t error_size) {
    char* path = snx_path_join(where, "main.npdm");
    char inner[256];
    int found;
    *data = NULL;
    *size = 0;
    found = exefs->read(exefs->source, "main.npdm", data, size);
    if (found == 1) {
        free(path);
        default_npdm(npdm);
        return 0;
    }
    if (found) {
        int rc = fail(error, error_size, snx_format("cannot read the NPDM %s", path));
        free(path);
        return rc;
    }
    if (snx_parse_npdm(*data, *size, npdm, inner, sizeof(inner))) {
        int rc = fail(error, error_size, snx_format("%s: %s", path, inner));
        free(path);
        free(*data);
        *data = NULL;
        *size = 0;
        return rc;
    }
    free(path);
    return 0;
}

int snx_load_npdm(const char* entry, SnxNpdm* npdm, char* error, size_t error_size) {
    SnxExefsReader folder;
    unsigned char* data;
    size_t size;
    char* exefs;
    int rc;
    if (nro_name(entry) && snx_is_file(entry)) {
        default_npdm(npdm);
        return 0;
    }
    memset(npdm, 0, sizeof(*npdm));
    exefs = find_exefs(entry);
    if (!exefs) return fail(error, error_size, snx_format("%s holds no exefs/main", entry));
    folder.read = folder_read;
    folder.source = exefs;
    rc = read_npdm(&folder, exefs, npdm, &data, &size, error, error_size);
    free(data);
    free(exefs);
    return rc;
}

/* The program's four paths in one allocation, entry_path first; missing ones NULL */
static void set_paths(SnxProgram* program, const char* entry, const char* exefs, const char* root,
                      const char* romfs) {
    const char* parts[4];
    char** fields[4];
    size_t bytes = 0, n;
    char* at;
    unsigned i;
    parts[0] = entry;
    parts[1] = exefs;
    parts[2] = root;
    parts[3] = romfs;
    fields[0] = &program->entry_path;
    fields[1] = &program->exefs_path;
    fields[2] = &program->root_path;
    fields[3] = &program->romfs_path;
    for (i = 0; i < 4; ++i)
        if (parts[i]) bytes += strlen(parts[i]) + 1;
    at = (char*)snx_alloc(bytes);
    for (i = 0; i < 4; ++i) {
        *fields[i] = NULL;
        if (!parts[i]) continue;
        n = strlen(parts[i]) + 1;
        memcpy(at, parts[i], n);
        *fields[i] = at;
        at += n;
    }
}

static int load_nro_program(const char* entry, SnxProgram* program, char* error, size_t error_size) {
    SnxModuleImage* m;
    program->modules = (SnxModuleImage*)snx_alloc(sizeof(*program->modules));
    m = program->modules;
    if (snx_load_nro(entry, m, error, error_size)) return -1;
    program->module_count = 1;
    m->base = SNX_CODE_BASE;
    if (m->size > CODE_END64 - SNX_CODE_BASE)
        return fail(error, error_size, snx_strdup("modules exceed guest address space"));
    default_npdm(&program->npdm);
    set_paths(program, entry, NULL, NULL, NULL);
    program->romfs_offset = m->romfs_offset;
    program->romfs_size = m->romfs_size;
    program->nacp = m->nacp_data; /* the program's from here */
    program->nacp_size = m->nacp_size;
    m->nacp_data = NULL;
    m->nacp_size = 0;
    return 0;
}

/* the modules of an ExeFS and its NPDM, where naming it in messages and the modules' paths */
static int load_modules(const SnxExefsReader* exefs, const char* where, SnxProgram* program, char* error,
                        size_t error_size) {
    unsigned char* file;
    char* path;
    size_t i, size;
    SnxU64 cursor, end;
    int found;
    /* the NPDM first: whether the program is 64-bit decides how every module is read */
    if (read_npdm(exefs, where, &program->npdm, &program->npdm_data, &program->npdm_size, error, error_size))
        return -1;
    cursor = program->npdm.is64 ? SNX_CODE_BASE : SNX_CODE_BASE32;
    end = program->npdm.is64 ? CODE_END64 : CODE_END32;
    program->modules = (SnxModuleImage*)snx_alloc(MODULE_NAMES * sizeof(*program->modules));
    for (i = 0; i < MODULE_NAMES; ++i) {
        SnxModuleImage* m = &program->modules[program->module_count];
        int failed;
        path = snx_path_join(where, module_names[i]);
        found = exefs->read(exefs->source, module_names[i], &file, &size);
        if (found) {
            if (found < 0) fail(error, error_size, snx_format("cannot read %s", path));
            free(path);
            if (found < 0) return -1;
            continue;
        }
        failed = decode_nso_file(file, size, path, module_names[i], program->npdm.is64, m, error, error_size);
        free(file);
        free(path);
        if (failed) return -1;
        ++program->module_count;
        if (m->size > end - cursor) return fail(error, error_size, snx_strdup("modules exceed guest address space"));
        m->base = cursor;
        cursor += m->size;
        cursor = end - cursor >= GUARD_SIZE ? cursor + GUARD_SIZE : end;
    }
    return 0;
}

static int load_exefs_program(const char* entry, char* const* names, char* const* paths, size_t count,
                              SnxProgram* program, char* error, size_t error_size) {
    SnxExefsReader folder;
    SnxExefsMods mods;
    char *exefs, *root, *romfs = NULL, *path, *nacp_path;
    const char* name;
    size_t length;
    int rc = -1;
    exefs = find_exefs(entry);
    if (!exefs) return fail(error, error_size, snx_format("%s holds no exefs/main", entry));
    name = leaf(exefs, &length);
    root = same_word(name, length, "exefs") ? snx_dirname(exefs) : snx_strdup(exefs);
    path = snx_path_join(root, "romfs");
    if (snx_is_dir(path))
        romfs = path;
    else {
        free(path);
        path = snx_path_join(root, "RomFS");
        if (snx_is_dir(path))
            romfs = path;
        else
            free(path);
    }
    set_paths(program, entry, exefs, root, romfs);
    folder.read = folder_read;
    folder.source = exefs;
    snx_exefs_mods(&mods, &folder, names, paths, count);
    if (load_modules(&mods.reader, exefs, program, error, error_size)) goto done;
    nacp_path = snx_path_join(root, "control.nacp");
    if (snx_is_file(nacp_path)) {
        program->nacp = snx_read_file(nacp_path, &program->nacp_size);
        if (!program->nacp) {
            fail(error, error_size, snx_format("cannot query control.nacp in %s", root));
            free(nacp_path);
            goto done;
        }
        if (!program->nacp_size) {
            free(program->nacp);
            program->nacp = NULL;
        }
    }
    free(nacp_path);
    rc = 0;
done:
    free(exefs);
    free(root);
    free(romfs);
    return rc;
}

int snx_load_program_from(const SnxExefsReader* exefs, const char* entry, unsigned char* nacp, size_t nacp_size,
                          SnxProgram* program, char* error, size_t error_size) {
    int rc;
    memset(program, 0, sizeof(*program));
    set_paths(program, entry, NULL, NULL, NULL);
    program->nacp = nacp;
    program->nacp_size = nacp ? nacp_size : 0;
    rc = load_modules(exefs, entry, program, error, error_size);
    if (rc) snx_free_program(program);
    return rc;
}

int snx_load_program(const char* entry, SnxProgram* program, char* error, size_t error_size) {
    return snx_load_program_mods(entry, NULL, NULL, 0, program, error, error_size);
}
int snx_load_program_mods(const char* entry, char* const* names, char* const* paths, size_t count, SnxProgram* program,
                          char* error, size_t error_size) {
    int rc;
    memset(program, 0, sizeof(*program));
    if (nro_name(entry) && snx_is_file(entry))
        rc = load_nro_program(entry, program, error, error_size);
    else
        rc = load_exefs_program(entry, names, paths, count, program, error, error_size);
    if (rc) snx_free_program(program);
    return rc;
}

void snx_free_program(SnxProgram* program) {
    size_t i;
    for (i = 0; i < program->module_count; ++i) snx_free_module(&program->modules[i]);
    free(program->modules);
    free(program->npdm_data);
    free(program->entry_path); /* the other paths too */
    free(program->nacp);
    memset(program, 0, sizeof(*program));
}

/* ------------------------------------------------------------------ dump for tools */


int snx_dump_program(const char* entry, const char* outdir, char* error, size_t error_size) {
    SnxProgram program;
    char *path = NULL, *index = NULL, *partial = NULL, *file_name;
    FILE* f = NULL;
    size_t i, j;
    int rc = -1;
    if (snx_load_program(entry, &program, error, error_size)) return -1;
    if (snx_mkdirs(outdir)) {
        fail(error, error_size, snx_format("cannot create %s", outdir));
        goto done;
    }
    for (i = 0; i < program.module_count; ++i) {
        const SnxModuleImage* m = &program.modules[i];
        file_name = snx_format("%s.bin", m->name);
        path = snx_path_join(outdir, file_name);
        free(file_name);
        if (snx_write_file(path, m->image, m->image_size)) {
            fail(error, error_size, snx_format("cannot write %s", path));
            goto done;
        }
        free(path);
        path = NULL;
    }
    /* the index last, renamed into place: a reader finds a complete dump or none */
    index = snx_path_join(outdir, "modules.txt");
    partial = snx_format("%s.partial", index);
    f = snx_fopen(partial, "wb");
    if (!f) {
        fail(error, error_size, snx_format("cannot write %s", partial));
        goto done;
    }
    fprintf(f, "program 1 %d\n", program.npdm.is64 ? 1 : 0);
    for (i = 0; i < program.module_count; ++i) {
        const SnxModuleImage* m = &program.modules[i];
        fprintf(f, "module %s %llx %llx %llx\n", m->name, (unsigned long long)m->base, (unsigned long long)m->size,
                (unsigned long long)m->image_size);
        for (j = 0; j < m->symbol_count; ++j)
            fprintf(f, "symbol %llx %llx %llx %s\n", (unsigned long long)m->symbols[j].value,
                    (unsigned long long)m->symbols[j].size, (unsigned long long)m->symbols[j].type,
                    m->symbols[j].name);
    }
    if (fclose(f) || snx_replace(partial, index)) {
        f = NULL;
        fail(error, error_size, snx_format("cannot write %s", index));
        goto done;
    }
    f = NULL;
    rc = 0;
done:
    if (f) fclose(f);
    free(path);
    free(index);
    free(partial);
    snx_free_program(&program);
    return rc;
}
