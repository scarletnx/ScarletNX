/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Derived from SwitchRecomp nxloader.py, Copyright 2026 MZ Labs.
   Licensed under Apache-2.0; retain LICENSE.apache, NOTICE and credit to SwitchRecomp by MZ Labs.
   This fork's changes are GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700
#endif
#include "nx/loader/loader.h"
#include <lz4.h>
/* OpenSSL's bignum header (from evp.h) types BN_ULONG as unsigned long long where long is 32 bits (Windows), which
   C90 doesn't have: only its declarations use it here */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlong-long"
#include <openssl/evp.h>
#pragma clang diagnostic pop
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

typedef struct {
    SnxModuleImage* module;
    char* error;
    size_t error_size;
    size_t symbol_cap, function_cap, relptr_cap, fde_cap, import_cap;
    int oom;
    int elf32; /* a 32-bit program's module (the NPDM says): ELF32 dynamic structures, Thumb bit in code addresses */
} Parser;

static void error_text(char* out, size_t capacity, const char* text) {
    size_t n;
    if (!out || !capacity) return;
    n = strlen(text);
    if (n >= capacity) n = capacity - 1;
    memcpy(out, text, n);
    out[n] = 0;
}

static int fail(Parser* p, const char* message) {
    error_text(p->error, p->error_size, message);
    return -1;
}

static int no_memory(Parser* p) {
    p->oom = 1;
    return fail(p, "loader: out of memory");
}

static char* copy_string(const char* s) {
    size_t n;
    char* r;
    n = strlen(s) + 1;
    r = (char*)malloc(n);
    if (r) memcpy(r, s, n);
    return r;
}

static int span(size_t size, SnxU64 offset, SnxU64 length) {
    return offset <= (SnxU64)size && length <= (SnxU64)size - offset;
}

static SnxU32 get16(const unsigned char* p) { return (SnxU32)p[0] | ((SnxU32)p[1] << 8); }

static SnxU32 get32(const unsigned char* p) {
    return (SnxU32)p[0] | ((SnxU32)p[1] << 8) | ((SnxU32)p[2] << 16) | ((SnxU32)p[3] << 24);
}

static SnxU64 get64(const unsigned char* p) { return (SnxU64)get32(p) | ((SnxU64)get32(p + 4) << 32); }

static void* grow(Parser* p, void* array, size_t* capacity, size_t count, size_t item_size) {
    size_t n;
    void* r;
    if (count < *capacity) return array;
    if (count == (size_t)-1 || count + 1 > (size_t)-1 / item_size) {
        no_memory(p);
        return NULL;
    }
    n = *capacity ? *capacity : 16;
    while (n <= count) {
        if (n > ((size_t)-1 / item_size) / 2) {
            n = count + 1;
            break;
        }
        n *= 2;
    }
    r = realloc(array, n * item_size);
    if (!r) {
        no_memory(p);
        return NULL;
    }
    *capacity = n;
    return r;
}

static int add_offset(Parser* p, SnxU64** array, size_t* count, size_t* capacity, SnxU64 value) {
    void* r;
    r = grow(p, *array, capacity, *count, sizeof **array);
    if (!r) return -1;
    *array = (SnxU64*)r;
    (*array)[(*count)++] = value;
    return 0;
}

/* A64 code is word aligned. An AArch32 code address has the Thumb bit in bit 0, as interworking branches take it: an
   A32 one is word aligned, a Thumb one halfword aligned */
static int in_text32(const SnxModuleImage* m, SnxU64 offset, int elf32) {
    if (elf32 ? (offset & 3) == 2 : (offset & 3) != 0) return 0;
    offset &= ~(SnxU64)1;
    return offset >= m->text_off && offset - m->text_off < m->text_size;
}
static int in_text(const SnxModuleImage* m, SnxU64 offset) { return in_text32(m, offset, 0); }

static int compare_offset(const void* a, const void* b) {
    SnxU64 x, y;
    x = *(const SnxU64*)a;
    y = *(const SnxU64*)b;
    return x < y ? -1 : x > y;
}

static void unique_offsets(SnxU64* array, size_t* count) {
    size_t i, used;
    if (*count < 2) return;
    qsort(array, *count, sizeof *array, compare_offset);
    used = 1;
    for (i = 1; i < *count; i++)
        if (array[i] != array[used - 1]) array[used++] = array[i];
    *count = used;
}

/* Stable ordering retains Python's last-symbol-wins rule for aliases at the same address. */
static void sort_symbols(SnxSymbol* s, SnxSymbol* tmp, size_t count) {
    size_t middle, i, j, k;
    if (count < 2) return;
    middle = count / 2;
    sort_symbols(s, tmp, middle);
    sort_symbols(s + middle, tmp + middle, count - middle);
    i = 0;
    j = middle;
    k = 0;
    while (i < middle && j < count) tmp[k++] = s[i].value <= s[j].value ? s[i++] : s[j++];
    while (i < middle) tmp[k++] = s[i++];
    while (j < count) tmp[k++] = s[j++];
    memcpy(s, tmp, count * sizeof *s);
}

static int finish_symbols(Parser* p) {
    SnxModuleImage* m;
    SnxSymbol* tmp;
    size_t i, used;
    m = p->module;
    if (m->symbol_count < 2) return 0;
    tmp = (SnxSymbol*)malloc(m->symbol_count * sizeof *tmp);
    if (!tmp) return no_memory(p);
    sort_symbols(m->symbols, tmp, m->symbol_count);
    free(tmp);
    used = 0;
    for (i = 0; i < m->symbol_count; i++) {
        if (used && m->symbols[used - 1].value == m->symbols[i].value) {
            free(m->symbols[used - 1].name);
            m->symbols[used - 1] = m->symbols[i];
        } else
            m->symbols[used++] = m->symbols[i];
    }
    m->symbol_count = used;
    return 0;
}

static int read_file(const char* path, unsigned char** data, size_t* size, char* error, size_t error_size) {
    FILE* file;
    unsigned char* bytes;
    SnxU64 length;
    size_t n;
    int bad;
#if !defined(_WIN32)
    off_t end;
#endif
    *data = NULL;
    *size = 0;
    file = snx_fopen(path, "rb");
    if (!file) {
        error_text(error, error_size, "loader: cannot open input file");
        return -1;
    }
#if defined(_WIN32)
    bad = _fseeki64(file, 0, SEEK_END);
    length = bad ? (SnxU64)-1 : (SnxU64)_ftelli64(file);
    bad = bad || length == (SnxU64)-1 || _fseeki64(file, 0, SEEK_SET);
#else
    bad = fseeko(file, 0, SEEK_END);
    end = bad ? (off_t)-1 : ftello(file);
    length = (SnxU64)end;
    bad = bad || end < 0 || fseeko(file, 0, SEEK_SET);
#endif
    if (bad || length > (SnxU64)(size_t)-1) {
        fclose(file);
        error_text(error, error_size, "loader: input file size is unsupported");
        return -1;
    }
    n = (size_t)length;
    bytes = (unsigned char*)malloc(n ? n : 1);
    if (!bytes) {
        fclose(file);
        error_text(error, error_size, "loader: out of memory reading file");
        return -1;
    }
    bad = n && fread(bytes, 1, n, file) != n;
    if (fclose(file)) bad = 1;
    if (bad) {
        free(bytes);
        error_text(error, error_size, "loader: failed reading input file");
        return -1;
    }
    *data = bytes;
    *size = n;
    return 0;
}

/* LZ4_decompress_safe never reads past n bytes or writes past size, and fails on a back reference before the start;
   a segment has to come out exactly as long as its header says */
static int lz4_block(Parser* p, const unsigned char* src, size_t n, unsigned char* dst, size_t size) {
    int got;
    if (n > LZ4_MAX_INPUT_SIZE || size > LZ4_MAX_INPUT_SIZE) return fail(p, "LZ4: segment too large");
    got = LZ4_decompress_safe((const char*)src, (char*)dst, (int)n, (int)size);
    if (got < 0) return fail(p, "LZ4: corrupt segment");
    return (size_t)got == size ? 0 : fail(p, "LZ4: decompressed length differs from segment header");
}

/* The dynamic table's tags it reads: DT_PLTRELSZ (2) to DT_RELENT (19). A 64-bit module has RELA relocations, a
   32-bit one REL, whose addend is the word the relocation is applied to */
#define DYN_TAGS 20
static int relr_pointer(Parser* p, SnxU64 target) {
    SnxModuleImage* m = p->module;
    SnxU64 addend, word = p->elf32 ? 4 : 8;
    if (target > m->size || m->size - target < word) return fail(p, "ELF: relocation target exceeds module");
    addend = span(m->image_size, target, word)
                 ? (p->elf32 ? get32(m->image + (size_t)target) : get64(m->image + (size_t)target))
                 : 0;
    return in_text32(m, addend, p->elf32) ? add_offset(p, &m->relptrs, &m->relptr_count, &p->relptr_cap, addend) : 0;
}

static int parse_relr(Parser* p, SnxU64 table, SnxU64 size, SnxU64 entry_size) {
    const SnxModuleImage* m = p->module;
    SnxU64 at, entry, where, i, word = p->elf32 ? 4 : 8;
    if ((entry_size && entry_size != word) || size % word) return fail(p, "ELF: invalid RELR entry size");
    if (!span(m->image_size, table, size)) return fail(p, "ELF: RELR table exceeds image");
    where = 0;
    for (at = table; at < table + size; at += word) {
        entry = p->elf32 ? get32(m->image + (size_t)at) : get64(m->image + (size_t)at);
        if (!(entry & 1)) {
            if (relr_pointer(p, entry)) return -1;
            where = entry + word;
            continue;
        }
        if (where > m->size) return fail(p, "ELF: relocation target exceeds module");
        for (i = 0, entry >>= 1; entry; entry >>= 1, i++)
            if ((entry & 1) && relr_pointer(p, where + i * word)) return -1;
        where += (word * 8 - 1) * word;
    }
    return 0;
}

static int parse_dynamic(Parser* p, size_t offset) {
    SnxModuleImage* m;
    SnxU64 tags[DYN_TAGS], relr[3], present, tag, value, count, i, at, string_size, name_at, symbol_value, symbol_size;
    SnxU64 addend, target, info, word, entry, symbol_entry, table, table_size, table_entry;
    unsigned relative;
    const unsigned char *s, *end;
    char* name;
    void* r;
    SnxU32 name_offset, section;
    unsigned type;
    int terminated;
    m = p->module;
    word = p->elf32 ? 4 : 8;
    entry = 2 * word;                  /* Elf32_Dyn, Elf64_Dyn */
    symbol_entry = p->elf32 ? 16 : 24; /* Elf32_Sym, Elf64_Sym */
    relative = p->elf32 ? 23 : 1027;   /* R_ARM_RELATIVE, R_AARCH64_RELATIVE */
    memset(tags, 0, sizeof tags);
    memset(relr, 0, sizeof relr);
    present = 0;
    terminated = 0;
    while (span(m->image_size, offset, entry)) {
        tag = p->elf32 ? get32(m->image + offset) : get64(m->image + offset);
        value = p->elf32 ? get32(m->image + offset + 4) : get64(m->image + offset + 8);
        offset += (size_t)entry;
        if (!tag) {
            terminated = 1;
            break;
        }
        if (tag < DYN_TAGS && !(present & ((SnxU64)1 << (unsigned)tag))) {
            tags[(unsigned)tag] = value;
            present |= (SnxU64)1 << (unsigned)tag;
        } else if (tag >= 35 && tag <= 37 && !(present & ((SnxU64)1 << (unsigned)tag))) {
            relr[(unsigned)tag - 35] = value; /* DT_RELRSZ, DT_RELR, DT_RELRENT */
            present |= (SnxU64)1 << (unsigned)tag;
        }
    }
    if (!terminated) return fail(p, "MOD0: unterminated dynamic table");
    if (!(present & ((SnxU64)1 << 11))) tags[11] = symbol_entry;
    if (!(present & ((SnxU64)1 << 9))) tags[9] = 3 * word;
    if (!(present & ((SnxU64)1 << 19))) tags[19] = 2 * word;
    count = 0;
    if (present & ((SnxU64)1 << 4)) {
        if (!span(m->image_size, tags[4], 8)) return fail(p, "ELF: hash header exceeds image");
        count = get32(m->image + (size_t)tags[4] + 4);
        value = get32(m->image + (size_t)tags[4]);
        if (!span(m->image_size, tags[4], 8 + (count + value) * 4)) return fail(p, "ELF: hash table exceeds image");
    }
    if ((present & 0x60) == 0x60) {
        if (tags[11] < symbol_entry) return fail(p, "ELF: invalid symbol entry size");
        if (!(present & ((SnxU64)1 << 4)) && tags[5] > tags[6]) count = (tags[5] - tags[6]) / tags[11];
        if (tags[6] > m->image_size || count > ((SnxU64)m->image_size - tags[6]) / tags[11])
            return fail(p, "ELF: symbol table exceeds image");
        if (tags[5] > m->image_size) return fail(p, "ELF: string table exceeds image");
        string_size = (present & ((SnxU64)1 << 10)) ? tags[10] : (SnxU64)m->image_size - tags[5];
        if (!span(m->image_size, tags[5], string_size)) return fail(p, "ELF: string table exceeds image");
        for (i = 0; i < count; i++) {
            at = tags[6] + i * tags[11];
            s = m->image + (size_t)at;
            name_offset = get32(s);
            if (p->elf32) { /* st_name, st_value, st_size, st_info, st_other, st_shndx */
                symbol_value = get32(s + 4);
                symbol_size = get32(s + 8);
                type = s[12] & 15;
                section = get16(s + 14);
            } else {
                type = s[4] & 15;
                section = get16(s + 6);
                symbol_value = get64(s + 8);
                symbol_size = get64(s + 16);
            }
            if (!name_offset) continue;
            if (name_offset >= string_size) return fail(p, "ELF: symbol name exceeds string table");
            name_at = tags[5] + name_offset;
            s = m->image + (size_t)name_at;
            end = (const unsigned char*)memchr(s, 0, (size_t)(string_size - name_offset));
            if (!end) return fail(p, "ELF: unterminated symbol name");
            if (end == s) continue;
            if (section && (!symbol_value || (type != 0 && type != 1 && type != 2))) continue;
            name = copy_string((const char*)s);
            if (!name) return no_memory(p);
            if (!section) {
                r = grow(p, m->imports, &p->import_cap, m->import_count, sizeof *m->imports);
                if (!r) {
                    free(name);
                    return -1;
                }
                m->imports = (char**)r;
                m->imports[m->import_count++] = name;
            } else {
                r = grow(p, m->symbols, &p->symbol_cap, m->symbol_count, sizeof *m->symbols);
                if (!r) {
                    free(name);
                    return -1;
                }
                m->symbols = (SnxSymbol*)r;
                m->symbols[m->symbol_count].name = name;
                m->symbols[m->symbol_count].value = symbol_value;
                m->symbols[m->symbol_count].size = symbol_size;
                m->symbols[m->symbol_count++].type = type;
                if (type == 2 && in_text32(m, symbol_value, p->elf32) &&
                    add_offset(p, &m->functions, &m->function_count, &p->function_cap, symbol_value))
                    return -1;
            }
        }
    }
    /* DT_RELA (7, 8, 9), then DT_REL (17, 18, 19) */
    for (i = 0; i < 2; i++) {
        table = tags[i ? 17 : 7];
        table_size = tags[i ? 18 : 8];
        table_entry = tags[i ? 19 : 9];
        if (!(present & ((SnxU64)1 << (i ? 17 : 7)))) continue;
        if (table_entry != (i ? 2 : 3) * word || table_size % table_entry)
            return fail(p, i ? "ELF: invalid REL entry size" : "ELF: invalid RELA entry size");
        if (!span(m->image_size, table, table_size)) return fail(p, "ELF: relocation table exceeds image");
        for (at = table; at < table + table_size; at += table_entry) {
            s = m->image + (size_t)at;
            target = p->elf32 ? get32(s) : get64(s);
            info = p->elf32 ? get32(s + 4) & 0xFF : get64(s + 8) & 0xFFFFFFFFUL;
            if (target > m->size || m->size - target < word) return fail(p, "ELF: relocation target exceeds module");
            if (info != relative) continue;
            if (i) /* REL: the addend is in the image, unless the target is in the BSS */
                addend = span(m->image_size, target, word)
                             ? (p->elf32 ? get32(m->image + (size_t)target) : get64(m->image + (size_t)target))
                             : 0;
            else
                addend = p->elf32 ? get32(s + 8) : get64(s + 16);
            if (in_text32(m, addend, p->elf32) && add_offset(p, &m->relptrs, &m->relptr_count, &p->relptr_cap, addend))
                return -1;
        }
    }
    if ((present & ((SnxU64)1 << 36)) && parse_relr(p, relr[1], relr[0], relr[2])) return -1;
    return 0;
}

static int read_leb(Parser* p, size_t* cursor, size_t limit, int is_signed, SnxU64* value) {
    unsigned i, byte, payload, shift;
    SnxU64 result;
    result = 0;
    for (i = 0; i < 10; i++) {
        if (*cursor >= limit) return fail(p, "unwind: truncated LEB128");
        byte = p->module->image[(*cursor)++];
        payload = byte & 127;
        shift = i * 7;
        if (i == 9 && (is_signed ? (payload != 0 && payload != 127) : payload > 1))
            return fail(p, "unwind: LEB128 overflow");
        result |= (SnxU64)payload << shift;
        if (!(byte & 128)) {
            if (is_signed && (byte & 64) && shift + 7 < 64) result |= (SnxU64)-1 << (shift + 7);
            *value = result;
            return 0;
        }
    }
    return fail(p, "unwind: unterminated LEB128");
}

static int read_encoded(Parser* p, size_t* cursor, size_t limit, unsigned encoding, SnxU64 data_relative,
                        int dereference, SnxU64* value) {
    size_t at, width;
    unsigned format, application;
    SnxU64 v;
    const unsigned char* image;
    *value = 0;
    if (encoding == 255) return 0;
    format = encoding & 15;
    application = encoding & 112;
    if (application == 80) {
        if (format || *cursor > (size_t)-1 - 7) return fail(p, "unwind: invalid aligned encoding");
        *cursor = (*cursor + 7) & ~(size_t)7;
    }
    at = *cursor;
    image = p->module->image;
    if (format == 1 || format == 9) {
        if (read_leb(p, cursor, limit, format == 9, &v)) return -1;
    } else {
        switch (format) {
            case 0:
            case 4:
            case 12: width = 8; break;
            case 2:
            case 10: width = 2; break;
            case 3:
            case 11: width = 4; break;
            default: return fail(p, "unwind: unsupported pointer encoding");
        }
        if (at > limit || width > limit - at) return fail(p, "unwind: encoded pointer exceeds record");
        v = width == 2 ? get16(image + at) : width == 4 ? get32(image + at) : get64(image + at);
        if (format == 10 && (v & 0x8000)) v |= (SnxU64)-1 << 16;
        if (format == 11 && (v & 0x80000000UL)) v |= (SnxU64)-1 << 32;
        *cursor += width;
    }
    switch (application) {
        case 0:
        case 80: break;
        case 16: v += at; break;
        case 32: v += p->module->text_off; break;
        case 48: v += data_relative; break;
        default: return fail(p, "unwind: unsupported pointer application");
    }
    if ((encoding & 128) && dereference) {
        if (!span(p->module->image_size, v, 8)) return fail(p, "unwind: indirect pointer exceeds image");
        v = get64(image + (size_t)v);
    }
    *value = v;
    return 0;
}

typedef struct {
    SnxU64 offset;
    unsigned encoding;
} CieEncoding;

static int parse_cie(Parser* p, size_t offset, unsigned* encoding) {
    const unsigned char *image, *terminator;
    SnxU32 length;
    SnxU64 ignored, augmentation_size;
    size_t cursor, end, aug, aug_length, augmentation_end, i;
    unsigned version, pe;
    image = p->module->image;
    if (!span(p->module->image_size, offset, 9)) return fail(p, "unwind: CIE header exceeds image");
    length = get32(image + offset);
    if (length == 0xffffffffUL || length < 5 || !span(p->module->image_size, offset, (SnxU64)length + 4))
        return fail(p, "unwind: invalid CIE length");
    if (get32(image + offset + 4)) return fail(p, "unwind: invalid CIE identifier");
    cursor = offset + 8;
    end = offset + 4 + length;
    version = image[cursor++];
    if (version != 1 && version != 3 && version != 4) return fail(p, "unwind: unsupported CIE version");
    aug = cursor;
    terminator = (const unsigned char*)memchr(image + cursor, 0, end - cursor);
    if (!terminator) return fail(p, "unwind: unterminated CIE augmentation");
    aug_length = (size_t)(terminator - image) - aug;
    cursor = (size_t)(terminator - image) + 1;
    if (version == 4) {
        if (end - cursor < 2 || image[cursor] != 8 || image[cursor + 1] != 0)
            return fail(p, "unwind: unsupported CIE address or segment size");
        cursor += 2;
    }
    if (aug_length == 2 && !memcmp(image + aug, "eh", 2)) {
        if (end - cursor < 8) return fail(p, "unwind: truncated legacy CIE pointer");
        cursor += 8;
    }
    if (read_leb(p, &cursor, end, 0, &ignored) || read_leb(p, &cursor, end, 1, &ignored)) return -1;
    if (version == 1) {
        if (cursor == end) return fail(p, "unwind: missing return register");
        cursor++;
    } else if (read_leb(p, &cursor, end, 0, &ignored))
        return -1;
    *encoding = 0;
    if (aug_length && image[aug] == 'z') {
        if (read_leb(p, &cursor, end, 0, &augmentation_size)) return -1;
        if (augmentation_size > end - cursor) return fail(p, "unwind: CIE augmentation exceeds record");
        augmentation_end = cursor + (size_t)augmentation_size;
        for (i = 1; i < aug_length; i++) {
            switch (image[aug + i]) {
                case 'R':
                    if (cursor == augmentation_end) return fail(p, "unwind: missing FDE encoding");
                    *encoding = image[cursor++];
                    break;
                case 'P':
                    if (cursor == augmentation_end) return fail(p, "unwind: missing personality encoding");
                    pe = image[cursor++];
                    if (read_encoded(p, &cursor, augmentation_end, pe, 0, 0, &ignored)) return -1;
                    break;
                case 'L':
                    if (cursor == augmentation_end) return fail(p, "unwind: missing LSDA encoding");
                    cursor++;
                    break;
                case 'S': break;
                default: return fail(p, "unwind: unsupported CIE augmentation");
            }
        }
    } else if (aug_length && !(aug_length == 2 && !memcmp(image + aug, "eh", 2)))
        return fail(p, "unwind: unsupported CIE augmentation");
    return 0;
}

static int parse_fde(Parser* p, SnxU64 offset, CieEncoding** cies, size_t* cie_count, size_t* cie_capacity) {
    SnxModuleImage* m;
    SnxU32 length, backward;
    SnxU64 cie, start, range;
    size_t i, cursor, end;
    unsigned encoding;
    void* r;
    m = p->module;
    if (!span(m->image_size, offset, 4)) return fail(p, "unwind: FDE length exceeds image");
    length = get32(m->image + (size_t)offset);
    if (!length || length == 0xffffffffUL) return 0;
    if (length < 4 || !span(m->image_size, offset, (SnxU64)length + 4)) return fail(p, "unwind: FDE exceeds image");
    backward = get32(m->image + (size_t)offset + 4);
    if ((SnxU64)backward > offset + 4) return fail(p, "unwind: CIE reference underflow");
    cie = offset + 4 - backward;
    encoding = 0;
    for (i = 0; i < *cie_count; i++) {
        if ((*cies)[i].offset == cie) {
            encoding = (*cies)[i].encoding;
            break;
        }
    }
    if (i == *cie_count) {
        if (parse_cie(p, (size_t)cie, &encoding)) return -1;
        r = grow(p, *cies, cie_capacity, *cie_count, sizeof **cies);
        if (!r) return -1;
        *cies = (CieEncoding*)r;
        (*cies)[*cie_count].offset = cie;
        (*cies)[(*cie_count)++].encoding = encoding;
    }
    if (encoding == 255) return fail(p, "unwind: omitted FDE start");
    cursor = (size_t)offset + 8;
    end = (size_t)offset + 4 + length;
    if (read_encoded(p, &cursor, end, encoding, 0, 1, &start) ||
        read_encoded(p, &cursor, end, encoding & 15, 0, 0, &range))
        return -1;
    r = grow(p, m->fde, &p->fde_cap, m->fde_count, sizeof *m->fde);
    if (!r) return -1;
    m->fde = (SnxFde*)r;
    m->fde[m->fde_count].start = start;
    m->fde[m->fde_count++].size = range;
    return 0;
}

static int parse_eh_records(Parser* p) {
    SnxModuleImage* m;
    SnxU64 hs, he, ignored, count, i, location, fde;
    size_t cursor, end, cie_count, cie_capacity;
    unsigned pointer_encoding, count_encoding, table_encoding;
    CieEncoding* cies;
    int result;
    m = p->module;
    hs = m->mod0.ehf_start;
    he = m->mod0.ehf_end;
    if (!hs || hs == he) return 0;
    if (hs > he || !span(m->image_size, hs, he - hs) || he - hs < 4)
        return fail(p, "unwind: invalid .eh_frame_hdr range");
    cursor = (size_t)hs;
    end = (size_t)he;
    if (m->image[cursor] != 1) return 0;
    pointer_encoding = m->image[cursor + 1];
    count_encoding = m->image[cursor + 2];
    table_encoding = m->image[cursor + 3];
    cursor += 4;
    if (read_encoded(p, &cursor, end, pointer_encoding, hs, 1, &ignored) ||
        read_encoded(p, &cursor, end, count_encoding, hs, 0, &count))
        return -1;
    if (count && (table_encoding == 255 || count > (end - cursor) / 2))
        return fail(p, "unwind: table count exceeds header");
    cies = NULL;
    cie_count = 0;
    cie_capacity = 0;
    result = 0;
    for (i = 0; i < count; i++) {
        if (read_encoded(p, &cursor, end, table_encoding, hs, 1, &location) ||
            read_encoded(p, &cursor, end, table_encoding, hs, 1, &fde) ||
            parse_fde(p, fde, &cies, &cie_count, &cie_capacity)) {
            result = -1;
            break;
        }
    }
    free(cies);
    return result;
}

static int signed_relative(Parser* p, SnxU64 base, SnxU32 displacement, SnxU64* out) {
    SnxU32 magnitude;
    if (displacement & 0x80000000UL) {
        magnitude = ~displacement + 1;
        if (base < magnitude) return fail(p, "MOD0: relative pointer precedes image");
        *out = base - magnitude;
    } else
        *out = base + displacement;
    return 0;
}

static int parse_module(Parser* p) {
    SnxModuleImage* m;
    SnxU32 mod0;
    SnxU64* fields[5];
    size_t i, error_size;
    char unwind_error[192];
    char* error;
    int bad;
    m = p->module;
    if (m->image_size < 8) return fail(p, "module: image is shorter than startup header");
    mod0 = get32(m->image + 4);
    if (!span(m->image_size, mod0, 4) || memcmp(m->image + mod0, "MOD0", 4)) return 0;
    if (!span(m->image_size, mod0, 0x1c)) return fail(p, "MOD0: truncated header");
    m->has_mod0 = 1;
    m->mod0.off = mod0;
    fields[0] = &m->mod0.dynamic;
    fields[1] = &m->mod0.bss_start;
    fields[2] = &m->mod0.bss_end;
    fields[3] = &m->mod0.ehf_start;
    fields[4] = &m->mod0.ehf_end;
    for (i = 0; i < 5; i++) {
        SnxU32 displacement = get32(m->image + mod0 + 4 + i * 4);
        /* Large BSS ranges can exceed signed 32-bit reach. */
        if (i == 2 && (displacement & 0x80000000UL) && mod0 < (~displacement + 1)) {
            *fields[i] = (SnxU64)mod0 + displacement;
        } else if (signed_relative(p, mod0, displacement, fields[i])) {
            return -1;
        }
    }
    if (m->mod0.bss_start > m->mod0.bss_end || m->mod0.bss_end > m->size)
        return fail(p, "MOD0: BSS range exceeds module");
    if (!span(m->image_size, m->mod0.dynamic, 16)) return fail(p, "MOD0: dynamic table exceeds image");
    if (parse_dynamic(p, (size_t)m->mod0.dynamic)) return -1;
    /* AArch32 code unwinds with .ARM.exidx, not .eh_frame: no FDEs */
    if (p->elf32) {
        unique_offsets(m->functions, &m->function_count);
        unique_offsets(m->relptrs, &m->relptr_count);
        return finish_symbols(p);
    }
    error = p->error;
    error_size = p->error_size;
    p->error = unwind_error;
    p->error_size = sizeof unwind_error;
    unwind_error[0] = 0;
    bad = parse_eh_records(p);
    p->error = error;
    p->error_size = error_size;
    if (p->oom) return no_memory(p);
    if (bad) {
        m->eh_error = copy_string(unwind_error);
        if (!m->eh_error) return no_memory(p);
    }
    for (i = 0; i < m->fde_count; i++)
        if (in_text(m, m->fde[i].start) &&
            add_offset(p, &m->functions, &m->function_count, &p->function_cap, m->fde[i].start))
            return -1;
    unique_offsets(m->functions, &m->function_count);
    unique_offsets(m->relptrs, &m->relptr_count);
    return finish_symbols(p);
}

void snx_free_module(SnxModuleImage* m) {
    size_t i;
    if (!m) return;
    free(m->name);
    free(m->path);
    free(m->image);
    free(m->functions);
    free(m->relptrs);
    free(m->fde);
    free(m->eh_error);
    for (i = 0; i < m->symbol_count; i++) free(m->symbols[i].name);
    free(m->symbols);
    for (i = 0; i < m->import_count; i++) free(m->imports[i]);
    free(m->imports);
    memset(m, 0, sizeof *m);
}

static const char* path_basename(const char* path) {
    const char* base;
    base = path;
    while (*path) {
        if (*path == '/' || *path == '\\') base = path + 1;
        path++;
    }
    return base;
}

static int image_layout(Parser* p) {
    SnxModuleImage* m;
    SnxU64 offsets[3], sizes[3], image_end, end;
    unsigned i, j;
    m = p->module;
    offsets[0] = m->text_off;
    offsets[1] = m->ro_off;
    offsets[2] = m->data_off;
    sizes[0] = m->text_size;
    sizes[1] = m->ro_size;
    sizes[2] = m->data_size;
    for (i = 0; i < 3; i++) {
        if (!span(m->image_size, offsets[i], sizes[i])) return fail(p, "module: segment exceeds image");
        for (j = 0; j < i; j++)
            if (sizes[i] && sizes[j] && offsets[i] < offsets[j] + sizes[j] && offsets[j] < offsets[i] + sizes[i])
                return fail(p, "module: segments overlap");
    }
    image_end = (SnxU64)m->data_off + m->data_size;
    if (image_end < m->image_size) image_end = m->image_size;
    end = image_end + m->bss;
    if (end < image_end || end > (SnxU64)-1 - (SNX_PAGE_SIZE - 1)) return fail(p, "module: address size overflow");
    m->size = (end + SNX_PAGE_SIZE - 1) & ~((SnxU64)SNX_PAGE_SIZE - 1);
    return 0;
}

static int load_nso(const char* path, const char* name, int elf32, SnxModuleImage* m, char* error, size_t error_size) {
    Parser p;
    unsigned char *file, digest[32];
    size_t file_size, i;
    SnxU32 flags, file_offset[3], memory_offset[3], length[3], stored[3];
    SnxU64 image_size;
    int result;
    if (!m) {
        error_text(error, error_size, "NSO: missing output");
        return -1;
    }
    memset(m, 0, sizeof *m);
    error_text(error, error_size, "");
    memset(&p, 0, sizeof p);
    p.module = m;
    p.error = error;
    p.error_size = error_size;
    p.elf32 = elf32;
    if (!path) return fail(&p, "NSO: missing path");
    if (read_file(path, &file, &file_size, error, error_size)) return -1;
    result = -1;
    if (file_size < 0x100 || memcmp(file, "NSO0", 4)) {
        fail(&p, "NSO: missing or truncated NSO0 header");
        goto done;
    }
    flags = get32(file + 12);
    for (i = 0; i < 3; i++) {
        file_offset[i] = get32(file + 0x10 + i * 0x10);
        memory_offset[i] = get32(file + 0x14 + i * 0x10);
        length[i] = get32(file + 0x18 + i * 0x10);
        stored[i] = get32(file + 0x60 + i * 4);
        if (!span(file_size, file_offset[i], stored[i])) {
            fail(&p, "NSO: segment exceeds file");
            goto done;
        }
        if (!(flags & (1U << i)) && stored[i] < length[i]) {
            fail(&p, "NSO: uncompressed segment is too short");
            goto done;
        }
    }
    image_size = (SnxU64)memory_offset[2] + length[2];
    if (image_size > (SnxU64)(size_t)-1) {
        fail(&p, "NSO: image exceeds host address space");
        goto done;
    }
    m->image_size = (size_t)image_size;
    m->text_off = memory_offset[0];
    m->text_size = length[0];
    m->ro_off = memory_offset[1];
    m->ro_size = length[1];
    m->data_off = memory_offset[2];
    m->data_size = length[2];
    m->bss = get32(file + 0x3c);
    memcpy(m->build_id, file + 0x40, 32);
    if (image_layout(&p)) goto done;
    m->image = (unsigned char*)calloc(m->image_size ? m->image_size : 1, 1);
    m->name = copy_string(name ? name : path_basename(path));
    m->path = copy_string(path);
    if (!m->image || !m->name || !m->path) {
        no_memory(&p);
        goto done;
    }
    for (i = 0; i < 3; i++) {
        if (flags & (1U << i)) {
            if (lz4_block(&p, file + file_offset[i], stored[i], m->image + memory_offset[i], length[i])) goto done;
        } else
            memcpy(m->image + memory_offset[i], file + file_offset[i], length[i]);
        if (flags & (8U << i)) {
            if (!EVP_Digest(m->image + memory_offset[i], length[i], digest, NULL, EVP_sha256(), NULL)) {
                no_memory(&p);
                goto done;
            }
            if (memcmp(digest, file + 0xa0 + i * 32, 32)) {
                fail(&p, "NSO: segment SHA-256 mismatch");
                goto done;
            }
        }
    }
    if (parse_module(&p)) goto done;
    result = 0;
done:
    free(file);
    if (result) snx_free_module(m);
    return result;
}

int snx_load_nso(const char* path, const char* name, SnxModuleImage* m, char* error, size_t error_size) {
    return load_nso(path, name, 0, m, error, error_size);
}
int snx_load_nso32(const char* path, const char* name, SnxModuleImage* m, char* error, size_t error_size) {
    return load_nso(path, name, 1, m, error, error_size);
}

int snx_load_nro(const char* path, SnxModuleImage* m, char* error, size_t error_size) {
    Parser p;
    unsigned char* file;
    size_t file_size;
    SnxU64 offset, length;
    unsigned i;
    if (!m) {
        error_text(error, error_size, "NRO: missing output");
        return -1;
    }
    memset(m, 0, sizeof *m);
    error_text(error, error_size, "");
    memset(&p, 0, sizeof p);
    p.module = m;
    p.error = error;
    p.error_size = error_size;
    if (!path) return fail(&p, "NRO: missing path");
    if (read_file(path, &file, &file_size, error, error_size)) return -1;
    m->image = file;
    if (file_size < 0x80 || memcmp(file + 0x10, "NRO0", 4)) {
        fail(&p, "NRO: missing or truncated NRO0 header");
        goto bad;
    }
    /* Atmosphere's ro::NroHeader flag 2 denotes a compressed image, not raw segment bytes. */
    if (get32(file + 0x1c) & 2) {
        fail(&p, "NRO: compressed images are not supported");
        goto bad;
    }
    m->image_size = get32(file + 0x18);
    if (m->image_size < 0x80 || m->image_size > file_size) {
        fail(&p, "NRO: declared image exceeds file");
        goto bad;
    }
    m->text_off = get32(file + 0x20);
    m->text_size = get32(file + 0x24);
    m->ro_off = get32(file + 0x28);
    m->ro_size = get32(file + 0x2c);
    m->data_off = get32(file + 0x30);
    m->data_size = get32(file + 0x34);
    m->bss = get32(file + 0x38);
    /* Atmosphere ro::ValidateNro: raw segments start at zero and are contiguous through Size.
       Keep accepting byte-packed fixtures here; guest permissions are rounded to pages at runtime. */
    if (m->text_off || m->text_size < 0x80 || (SnxU64)m->text_size != m->ro_off ||
        (SnxU64)m->ro_off + m->ro_size != m->data_off || (SnxU64)m->data_off + m->data_size != m->image_size) {
        fail(&p, "NRO: segments must be contiguous and cover the declared image");
        goto bad;
    }
    memcpy(m->build_id, file + 0x40, 32);
    m->is_nro = 1;
    m->nro_asset = file + m->image_size;
    m->nro_asset_size = file_size - m->image_size;
    m->name = copy_string("main");
    m->path = copy_string(path);
    if (!m->name || !m->path) {
        no_memory(&p);
        goto bad;
    }
    if (image_layout(&p)) goto bad;
    if (m->nro_asset_size >= 4 && !memcmp(m->nro_asset, "ASET", 4)) {
        if (m->nro_asset_size < 0x38) {
            fail(&p, "NRO: truncated ASET header");
            goto bad;
        }
        for (i = 0; i < 3; i++) {
            offset = get64(m->nro_asset + 8 + i * 16);
            length = get64(m->nro_asset + 16 + i * 16);
            if (length && (offset < 0x38 || !span(m->nro_asset_size, offset, length))) {
                fail(&p, "NRO: asset exceeds ASET data");
                goto bad;
            }
            if (i == 1 && length) {
                m->nacp_data = m->nro_asset + (size_t)offset;
                m->nacp_size = (size_t)length;
            }
            if (i == 2 && length) {
                m->romfs_data = m->nro_asset + (size_t)offset;
                m->romfs_size = (size_t)length;
            }
        }
    }
    if (parse_module(&p)) goto bad;
    return 0;
bad:
    snx_free_module(m);
    return -1;
}

int snx_parse_npdm(const unsigned char* data, size_t size, SnxNpdm* npdm, char* error, size_t error_size) {
    SnxU32 aci_offset, aci_size, acid_offset, acid_size;
    if (!npdm) {
        error_text(error, error_size, "NPDM: missing output");
        return -1;
    }
    memset(npdm, 0, sizeof *npdm);
    error_text(error, error_size, "");
    npdm->stack = 0x100000;
    npdm->prio = 44;
    npdm->is64 = 1;
    npdm->aspace = 3;
    npdm->title_id = SNX_NPDM_DEFAULT_TITLE;
    memcpy(npdm->name, "Application", 12);
    if (!size) return 0;
    if (!data) {
        error_text(error, error_size, "NPDM: missing input bytes");
        goto bad;
    }
    if (size < 4 || memcmp(data, "META", 4)) return 0;
    if (size < 0x80) {
        error_text(error, error_size, "NPDM: truncated META header");
        goto bad;
    }
    npdm->is64 = !!(data[12] & 1);
    npdm->aspace = (data[12] >> 1) & 7;
    npdm->prio = data[14];
    npdm->core = data[15];
    npdm->stack = get32(data + 0x1c);
    if (!npdm->stack) npdm->stack = 0x100000;
    memcpy(npdm->name, data + 0x20, 16);
    npdm->name[16] = 0;
    aci_offset = get32(data + 0x70);
    aci_size = get32(data + 0x74);
    acid_offset = get32(data + 0x78);
    acid_size = get32(data + 0x7c);
    if ((aci_size && !span(size, aci_offset, aci_size)) || (acid_size && !span(size, acid_offset, acid_size))) {
        error_text(error, error_size, "NPDM: access-control region exceeds file");
        goto bad;
    }
    if (aci_offset) {
        if (!span(size, aci_offset, 0x18) || (aci_size && aci_size < 0x18) || memcmp(data + aci_offset, "ACI0", 4)) {
            error_text(error, error_size, "NPDM: invalid ACI0 header");
            goto bad;
        }
        npdm->title_id = get64(data + aci_offset + 0x10);
    }
    return 0;
bad:
    memset(npdm, 0, sizeof *npdm);
    return -1;
}

static char* join_path(const char* a, const char* b) {
    size_t na, nb;
    char* r;
    int slash;
    na = strlen(a);
    nb = strlen(b);
    slash = na && a[na - 1] != '/' && a[na - 1] != '\\';
    if (na > (size_t)-1 - nb - 2) return NULL;
    r = (char*)malloc(na + nb + (size_t)slash + 1);
    if (!r) return NULL;
    memcpy(r, a, na);
    if (slash) r[na++] = '/';
    memcpy(r + na, b, nb + 1);
    return r;
}

static char* parent_path(const char* path) {
    char* r;
    size_t n;
    r = copy_string(path);
    if (!r) return NULL;
    n = (size_t)(path_basename(r) - r);
    if (!n) {
        r[0] = '.';
        r[1] = 0;
        return r;
    }
    while (n > 1 && (r[n - 1] == '/' || r[n - 1] == '\\')) {
        if (n == 3 && r[1] == ':') break;
        n--;
    }
    r[n] = 0;
    return r;
}

static char* absolute_path(const char* path) {
    char* out;
    size_t n;
#if defined(_WIN32)
    char* p;
#endif
    out = snx_absolute(path);
    if (!out) return NULL;
#if defined(_WIN32)
    for (p = out; *p; p++)
        if (*p == '\\') *p = '/';
#endif
    n = strlen(out);
    while (n > 1 && (out[n - 1] == '/' || out[n - 1] == '\\')) {
        if (n == 3 && out[1] == ':') break;
        out[--n] = 0;
    }
    return out;
}

/* This offline loader runs before the runtime index. Common wrappers use UTF-8/wide APIs on Windows. */
static int path_kind(const char* path, int directory) { return directory ? snx_is_dir(path) : snx_is_file(path); }

static int same_ascii(const char* a, const char* b) {
    unsigned x, y;
    do {
        x = (unsigned char)*a++;
        y = (unsigned char)*b++;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return 0;
    } while (x);
    return 1;
}

static int find_exefs(const char* entry, char** out, char* error, size_t error_size) {
    static const char* candidates[3] = {"exefs", "ExeFS", ""};
    char *candidate, *main_path;
    unsigned i;
    int kind;
    *out = NULL;
    for (i = 0; i < 3; i++) {
        candidate = candidates[i][0] ? join_path(entry, candidates[i]) : copy_string(entry);
        if (!candidate) {
            error_text(error, error_size, "loader: out of memory");
            return -1;
        }
        main_path = join_path(candidate, "main");
        if (!main_path) {
            free(candidate);
            error_text(error, error_size, "loader: out of memory");
            return -1;
        }
        kind = path_kind(main_path, 0);
        free(main_path);
        if (kind == 1) {
            *out = candidate;
            return 0;
        }
        free(candidate);
        if (kind < 0) {
            error_text(error, error_size, "loader: cannot query ExeFS directory");
            return -1;
        }
    }
    error_text(error, error_size, "loader: cannot find exefs/main");
    return -1;
}

void snx_free_program(SnxProgram* program) {
    size_t i;
    if (!program) return;
    for (i = 0; i < program->module_count; i++) snx_free_module(&program->modules[i]);
    free(program->modules);
    free(program->npdm_data);
    free(program->nacp);
    free(program->entry_path);
    free(program->exefs_path);
    free(program->root_path);
    free(program->romfs_path);
    memset(program, 0, sizeof *program);
}

int snx_load_program(const char* entry, SnxProgram* program, char* error, size_t error_size) {
    static const char* order[13] = {"rtld",    "main",    "subsdk0", "subsdk1", "subsdk2", "subsdk3", "subsdk4",
                                    "subsdk5", "subsdk6", "subsdk7", "subsdk8", "subsdk9", "sdk"};
    static const char* romfs_names[2] = {"romfs", "RomFS"};
    char* path;
    const char* extension;
    size_t i;
    SnxU64 base, address_limit;
    int kind;
    if (!program) {
        error_text(error, error_size, "loader: missing output");
        return -1;
    }
    memset(program, 0, sizeof *program);
    error_text(error, error_size, "");
    if (!entry || !*entry) {
        error_text(error, error_size, "loader: missing input path");
        return -1;
    }
    program->entry_path = absolute_path(entry);
    if (!program->entry_path) {
        error_text(error, error_size, "loader: cannot resolve input path");
        goto bad;
    }
    program->modules = (SnxModuleImage*)calloc(13, sizeof *program->modules);
    if (!program->modules) goto memory;
    extension = strrchr(path_basename(program->entry_path), '.');
    kind = path_kind(program->entry_path, 0);
    if (kind < 0) {
        error_text(error, error_size, "loader: cannot query input path");
        goto bad;
    }
    if (kind && extension && same_ascii(extension, ".nro")) {
        if (snx_load_nro(program->entry_path, &program->modules[0], error, error_size)) goto bad;
        program->module_count = 1;
        program->root_path = parent_path(program->entry_path);
        if (!program->root_path) goto memory;
        program->romfs_data = program->modules[0].romfs_data;
        program->romfs_size = program->modules[0].romfs_size;
        if (program->modules[0].nacp_size) {
            program->nacp = (unsigned char*)malloc(program->modules[0].nacp_size);
            if (!program->nacp) goto memory;
            memcpy(program->nacp, program->modules[0].nacp_data, program->modules[0].nacp_size);
            program->nacp_size = program->modules[0].nacp_size;
        }
    } else {
        if (find_exefs(program->entry_path, &program->exefs_path, error, error_size)) goto bad;
        /* the NPDM first: it says whether the modules are 32-bit ones, whose ELF structures differ */
        path = join_path(program->exefs_path, "main.npdm");
        if (!path) goto memory;
        kind = path_kind(path, 0);
        if (kind < 0) {
            free(path);
            error_text(error, error_size, "loader: cannot query main.npdm");
            goto bad;
        }
        if (kind && read_file(path, &program->npdm_data, &program->npdm_size, error, error_size)) {
            free(path);
            goto bad;
        }
        free(path);
        if (snx_parse_npdm(program->npdm_data, program->npdm_size, &program->npdm, error, error_size)) goto bad;
        for (i = 0; i < 13; i++) {
            path = join_path(program->exefs_path, order[i]);
            if (!path) goto memory;
            kind = path_kind(path, 0);
            if (kind < 0) {
                free(path);
                error_text(error, error_size, "loader: cannot query module path");
                goto bad;
            }
            if (kind && load_nso(path, order[i], !program->npdm.is64, &program->modules[program->module_count], error,
                                 error_size)) {
                free(path);
                goto bad;
            }
            if (kind) program->module_count++;
            free(path);
        }
        program->root_path = same_ascii(path_basename(program->exefs_path), "exefs") ? parent_path(program->exefs_path)
                                                                                     : copy_string(program->exefs_path);
        if (!program->root_path) goto memory;
        path = join_path(program->root_path, "control.nacp");
        if (!path) goto memory;
        kind = path_kind(path, 0);
        if (kind < 0 || (kind && read_file(path, &program->nacp, &program->nacp_size, error, error_size))) {
            free(path);
            if (kind < 0) error_text(error, error_size, "loader: cannot query control.nacp");
            goto bad;
        }
        free(path);
        for (i = 0; i < 2; i++) {
            path = join_path(program->root_path, romfs_names[i]);
            if (!path) goto memory;
            kind = path_kind(path, 1);
            if (kind < 0) {
                free(path);
                error_text(error, error_size, "loader: cannot query RomFS directory");
                goto bad;
            }
            if (kind) {
                free(program->romfs_path);
                program->romfs_path = path;
            } else
                free(path);
        }
    }
    if (snx_parse_npdm(program->npdm_data, program->npdm_size, &program->npdm, error, error_size)) goto bad;
    /* Loader's code base, and the end of the code region: a 32-bit process's modules go below its alias region */
    base = program->npdm.is64 ? SNX_CODE_BASE : SNX_CODE_BASE32;
    address_limit = program->npdm.is64 ? (SnxU64)0x20 << 32 : (SnxU64)0x40000000UL;
    for (i = 0; i < program->module_count; i++) {
        if (base > address_limit - SNX_PAGE_SIZE || program->modules[i].size > address_limit - base - SNX_PAGE_SIZE) {
            error_text(error, error_size, "loader: modules exceed guest address space");
            goto bad;
        }
        program->modules[i].base = base;
        base += program->modules[i].size + SNX_PAGE_SIZE;
    }
    return 0;
memory:
    error_text(error, error_size, "loader: out of memory");
bad:
    snx_free_program(program);
    return -1;
}

int snx_load_npdm(const char* entry, SnxNpdm* npdm, char* error, size_t error_size) {
    char *full, *exefs = NULL, *path = NULL;
    const char* extension;
    unsigned char* data = NULL;
    size_t size = 0;
    int rc = -1;
    error_text(error, error_size, "");
    full = entry && *entry ? absolute_path(entry) : NULL;
    if (!full) {
        error_text(error, error_size, "loader: cannot resolve input path");
        return -1;
    }
    extension = strrchr(path_basename(full), '.');
    /* an NRO carries no NPDM: the defaults, as snx_load_program gives it */
    if (extension && same_ascii(extension, ".nro") && path_kind(full, 0))
        rc = snx_parse_npdm(NULL, 0, npdm, error, error_size);
    else if (!find_exefs(full, &exefs, error, error_size)) {
        path = join_path(exefs, "main.npdm");
        if (!path)
            error_text(error, error_size, "loader: out of memory");
        else if (!path_kind(path, 0) || !read_file(path, &data, &size, error, error_size))
            rc = snx_parse_npdm(data, size, npdm, error, error_size);
    }
    free(data);
    free(path);
    free(exefs);
    free(full);
    return rc;
}
