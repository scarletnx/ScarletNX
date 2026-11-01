/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   ldr:ro, the loader of CRO libraries. The module format, the rebase and link steps and the order of the checks
   follow Azahar's service/ldr_ro/{ldr_ro,cro_helper}.cpp (GPL-2.0-or-later, from Citra, revision in docs/3DS.md). Two
   facts there differ: the .data segment is copied to the game's buffer after the link, as the console's RO does, and
   the ARM branch relocation types are applied. The code of a library runs through the compiler at run time, which
   checks the bytes of a block before it runs them, so a write here needs no flush. */
#include "3ds/runtime/internal.h"
#include "3ds/services/ro.h"
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"
#include "snx_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    HEADER_SIZE = 0x138,
    HASH_SIZE = 0x80,
    MAGIC_CRO0 = 0x304f5243,
    MAGIC_FIXD = 0x44584946,
    ALIGNED_PAGE = 4096
};
/* the words of the header from +0x80, in order */
enum {
    F_MAGIC,
    F_NAME_OFFSET,
    F_NEXT,
    F_PREVIOUS,
    F_FILE_SIZE,
    F_BSS_SIZE,
    F_FIXED_SIZE,
    F_ZERO,
    F_UNKNOWN_TAG,
    F_ON_LOAD_TAG,
    F_ON_EXIT_TAG,
    F_ON_UNRESOLVED_TAG,
    F_CODE_OFFSET,
    F_CODE_SIZE,
    F_DATA_OFFSET,
    F_DATA_SIZE,
    F_MODULE_NAME_OFFSET,
    F_MODULE_NAME_SIZE,
    F_SEGMENT_OFFSET,
    F_SEGMENT_NUM,
    F_EXPORT_NAMED_OFFSET,
    F_EXPORT_NAMED_NUM,
    F_EXPORT_INDEXED_OFFSET,
    F_EXPORT_INDEXED_NUM,
    F_EXPORT_STRINGS_OFFSET,
    F_EXPORT_STRINGS_SIZE,
    F_EXPORT_TREE_OFFSET,
    F_EXPORT_TREE_NUM,
    F_IMPORT_MODULE_OFFSET,
    F_IMPORT_MODULE_NUM,
    F_EXTERNAL_OFFSET,
    F_EXTERNAL_NUM,
    F_IMPORT_NAMED_OFFSET,
    F_IMPORT_NAMED_NUM,
    F_IMPORT_INDEXED_OFFSET,
    F_IMPORT_INDEXED_NUM,
    F_IMPORT_ANON_OFFSET,
    F_IMPORT_ANON_NUM,
    F_IMPORT_STRINGS_OFFSET,
    F_IMPORT_STRINGS_SIZE,
    F_STATIC_ANON_OFFSET,
    F_STATIC_ANON_NUM,
    F_INTERNAL_OFFSET,
    F_INTERNAL_NUM,
    F_STATIC_OFFSET,
    F_STATIC_NUM,
    F_FIX0_BARRIER,
    F_FIX1_BARRIER = F_STATIC_ANON_OFFSET,
    F_FIX2_BARRIER = F_IMPORT_MODULE_OFFSET,
    F_FIX3_BARRIER = F_EXPORT_NAMED_OFFSET
};
_Static_assert(F_FIX0_BARRIER == (HEADER_SIZE - HASH_SIZE) / 4, "the CRO header has 46 words after its hash");
/* the size of an entry in each table from the code to the static relocations (1 for bytes) */
static const unsigned entry_size[17] = {1, 1, 1, 12, 8, 4, 1, 8, 20, 12, 8, 8, 8, 1, 8, 12, 12};
static const unsigned fix_barrier[4] = {F_FIX0_BARRIER, F_FIX1_BARRIER, F_FIX2_BARRIER, F_FIX3_BARRIER};
enum { SEGMENT_CODE, SEGMENT_RODATA, SEGMENT_DATA, SEGMENT_BSS };
/* relocation types (ELF's ARM numbers) */
enum {
    R_NOTHING = 0,
    R_ABSOLUTE = 2,
    R_RELATIVE = 3,
    R_THUMB_BRANCH = 10,
    R_ARM_BRANCH = 28,
    R_ARM_JUMP = 29,
    R_ABSOLUTE2 = 38,
    R_PREL31 = 42
};
/* the result codes, as Azahar names them */
enum {
    ALREADY_INITIALIZED = 0xd9612ff9u,
    NOT_INITIALIZED = 0xd9612ff8u,
    BUFFER_TOO_SMALL = 0xe0e12c1fu,
    MISALIGNED_ADDRESS = 0xd9012ff1u,
    MISALIGNED_SIZE = 0xd9012ff2u,
    ILLEGAL_ADDRESS = 0xe1612c0fu,
    INVALID_MEMORY_STATE = 0xd8a12c08u,
    NOT_LOADED = 0xd8a12c0du
};
#define FORMAT(description) (0xd9012c00u | (description))

typedef struct {
    SnxCtrProcess* p;
    uint32_t base;
} Cro;
/* Reads and writes go to the pages themselves: a loaded library is read-only to the game, and RO writes it all the
   same */
static uint32_t rd32(const Cro* c, uint32_t va) {
    unsigned char b[4];
    return snx_ctr_read_ram(c->p->space, va, b, 4) == SNX_CTR_MEM_OK ? snx_ld_le32(b) : 0;
}
static bool write_bytes(SnxCtrProcess* p, uint32_t va, const void* data, size_t size) {
    const unsigned char* bytes = data;
    while (size) {
        uint32_t pa;
        size_t n = ALIGNED_PAGE - (va & (ALIGNED_PAGE - 1));
        if (n > size) n = size;
        if (snx_ctr_query(p->space, va, &pa, NULL) != SNX_CTR_MEM_OK ||
            snx_ctr_phys_write(p->memory, pa, bytes, n) != SNX_CTR_MEM_OK)
            return false;
        va += (uint32_t)n;
        bytes += n;
        size -= n;
    }
    return true;
}
static void wr32(const Cro* c, uint32_t va, uint32_t value) {
    unsigned char b[4];
    snx_st_le32(b, value);
    (void)write_bytes(c->p, va, b, 4);
}
static uint32_t get(const Cro* c, unsigned field) { return rd32(c, c->base + HASH_SIZE + field * 4); }
static void set(const Cro* c, unsigned field, uint32_t value) { wr32(c, c->base + HASH_SIZE + field * 4, value); }
/* a NUL-terminated string of at most max bytes, in out */
static void cstring(const Cro* c, uint32_t va, uint32_t max, char* out, size_t out_size) {
    size_t n = 0;
    for (; n < max && n + 1 < out_size; ++n) {
        unsigned char b;
        if (snx_ctr_read_ram(c->p->space, va + (uint32_t)n, &b, 1) != SNX_CTR_MEM_OK || !b) break;
        out[n] = (char)b;
    }
    out[n] = 0;
}
/* an entry's address in a table (the offset field of the table, the size of an entry, an index) */
static uint32_t entry(const Cro* c, unsigned offset_field, unsigned size, uint32_t index) {
    return get(c, offset_field) + index * size;
}
static uint32_t seg_address(const Cro* c, uint32_t tag) {
    uint32_t index = tag & 15, offset = tag >> 4;
    if (index >= get(c, F_SEGMENT_NUM)) return 0;
    uint32_t segment = entry(c, F_SEGMENT_OFFSET, 12, index);
    return offset >= rd32(c, segment + 4) ? 0 : rd32(c, segment) + offset;
}
static uint32_t apply_relocation(const Cro* c, uint32_t target, unsigned type, uint32_t addend, uint32_t symbol,
                                 uint32_t future) {
    uint32_t value = symbol + addend;
    switch (type) {
        case R_NOTHING: break;
        case R_ABSOLUTE:
        case R_ABSOLUTE2: wr32(c, target, value); break;
        case R_RELATIVE: wr32(c, target, value - future); break;
        case R_PREL31: wr32(c, target, ((value - future) & 0x7fffffffu) | (rd32(c, target) & 0x80000000u)); break;
        case R_ARM_BRANCH:
        case R_ARM_JUMP: {
            /* a branch to Thumb code from a BL becomes a BLX, whose H bit is bit 1 of the distance */
            uint32_t instruction = rd32(c, target), distance = (value & ~1u) - future;
            if (type == R_ARM_BRANCH && (value & 1) && instruction >> 28 == 0xe)
                instruction = 0xfa000000u | ((distance >> 1 & 1) << 24);
            wr32(c, target, (instruction & 0xff000000u) | (distance >> 2 & 0x00ffffffu));
            break;
        }
        case R_THUMB_BRANCH: {
            uint32_t distance = (value & ~1u) - future, pair = (0xf000u | (distance >> 12 & 0x7ff)) |
                                                                (((value & 1 ? 0xf800u : 0xe800u) | (distance >> 1 & 0x7ff)) << 16);
            wr32(c, target, pair);
            break;
        }
        default: return FORMAT(0x22);
    }
    return 0;
}
static uint32_t clear_relocation(const Cro* c, uint32_t target, unsigned type) {
    switch (type) {
        case R_NOTHING: break;
        case R_ABSOLUTE:
        case R_ABSOLUTE2:
        case R_RELATIVE: wr32(c, target, 0); break;
        case R_PREL31:
        case R_ARM_BRANCH:
        case R_ARM_JUMP:
        case R_THUMB_BRANCH: break; /* a branch is no address to clear */
        default: return FORMAT(0x22);
    }
    return 0;
}
/* the fields of the 12-byte relocation entry at address */
static uint32_t rel_tag(const Cro* c, uint32_t address) { return rd32(c, address); }
static unsigned rel_type(const Cro* c, uint32_t address) { return rd32(c, address + 4) & 0xff; }
static bool rel_end(const Cro* c, uint32_t address) { return rd32(c, address + 4) >> 8 & 0xff; }
static bool rel_resolved(const Cro* c, uint32_t address) { return rd32(c, address + 4) >> 16 & 0xff; }
static void rel_set_resolved(const Cro* c, uint32_t address, bool resolved) {
    wr32(c, address + 4, (rd32(c, address + 4) & 0xff00ffffu) | (resolved ? 0x10000u : 0));
}
/* a batch of relocations to one symbol, or back to the unresolved state when reset */
static uint32_t apply_batch(const Cro* c, uint32_t batch, uint32_t symbol, bool reset) {
    if (!symbol && !reset) return FORMAT(0x10);
    for (uint32_t at = batch;; at += 12) {
        uint32_t target = seg_address(c, rel_tag(c, at));
        if (!target) return FORMAT(0x12);
        uint32_t result = apply_relocation(c, target, rel_type(c, at), rd32(c, at + 8), symbol, target);
        if (result) return result;
        if (rel_end(c, at)) break;
    }
    rel_set_resolved(c, batch, !reset);
    return 0;
}
/* an exported symbol of the module by name, by its tree of bit tests */
static uint32_t find_export(const Cro* c, const char* name) {
    if (!get(c, F_EXPORT_TREE_NUM)) return 0;
    size_t length = strlen(name);
    uint32_t node = entry(c, F_EXPORT_TREE_OFFSET, 8, 0), next = rd32(c, node + 2) & 0xffff, found;
    for (unsigned guard = 0;; ++guard) {
        if (guard > get(c, F_EXPORT_TREE_NUM) + 1) return 0;
        node = entry(c, F_EXPORT_TREE_OFFSET, 8, next & 0x7fff);
        if (next & 0x8000) {
            found = rd32(c, node + 4) >> 16;
            break;
        }
        uint32_t test = rd32(c, node) & 0xffff, byte = test >> 3;
        bool right = byte < length && ((unsigned char)name[byte] >> (test & 7) & 1);
        next = right ? rd32(c, node + 4) & 0xffff : rd32(c, node + 2) & 0xffff;
    }
    if (found >= get(c, F_EXPORT_NAMED_NUM)) return 0;
    uint32_t symbol = entry(c, F_EXPORT_NAMED_OFFSET, 8, found);
    char text[256];
    cstring(c, rd32(c, symbol), get(c, F_EXPORT_STRINGS_SIZE), text, sizeof text);
    return strcmp(text, name) ? 0 : seg_address(c, rd32(c, symbol + 4));
}
/* adds the module's address to the offset at address (when set) and checks it against [low, high) */
static uint32_t rebase_word(const Cro* c, uint32_t address, uint32_t low, uint32_t high, bool inclusive, uint32_t error) {
    uint32_t offset = rd32(c, address);
    if (!offset) return 0;
    offset += c->base;
    wr32(c, address, offset);
    return (offset < low || offset > high - (inclusive ? 0 : 1)) ? error : 0;
}
static uint32_t rebase_header(const Cro* c, uint32_t size) {
    uint32_t error = FORMAT(0x11);
    if (get(c, F_MAGIC) != MAGIC_CRO0 || get(c, F_NEXT) || get(c, F_PREVIOUS) || get(c, F_FILE_SIZE) > 0x10000000 ||
        get(c, F_BSS_SIZE) > 0x10000000 || get(c, F_FIXED_SIZE) || get(c, F_CODE_OFFSET) < HEADER_SIZE)
        return error;
    static const unsigned order[18] = {F_CODE_OFFSET,           F_MODULE_NAME_OFFSET,     F_SEGMENT_OFFSET,
                                       F_EXPORT_NAMED_OFFSET,   F_EXPORT_TREE_OFFSET,     F_EXPORT_INDEXED_OFFSET,
                                       F_EXPORT_STRINGS_OFFSET, F_IMPORT_MODULE_OFFSET,   F_EXTERNAL_OFFSET,
                                       F_IMPORT_NAMED_OFFSET,   F_IMPORT_INDEXED_OFFSET,  F_IMPORT_ANON_OFFSET,
                                       F_IMPORT_STRINGS_OFFSET, F_STATIC_ANON_OFFSET,     F_INTERNAL_OFFSET,
                                       F_STATIC_OFFSET,         F_DATA_OFFSET,            F_FILE_SIZE};
    uint32_t previous = get(c, order[0]);
    for (unsigned i = 1; i < 18; ++i) {
        uint32_t current = get(c, order[i]);
        if (current < previous) return error;
        previous = current;
    }
    uint32_t name = get(c, F_NAME_OFFSET);
    if (name) set(c, F_NAME_OFFSET, name + c->base);
    for (unsigned field = F_CODE_OFFSET; field < F_FIX0_BARRIER; field += 2) {
        uint32_t offset = get(c, field);
        if (offset) set(c, field, offset + c->base);
    }
    uint64_t end = (uint64_t)c->base + size;
    for (unsigned field = F_CODE_OFFSET, i = 0; field < F_FIX0_BARRIER; field += 2, ++i)
        if ((uint64_t)get(c, field) + (uint64_t)get(c, field + 1) * entry_size[i] > end) return error;
    return 0;
}
/* the segments take their rebased offsets or the game's buffers; the old address of the data in the library is out */
static uint32_t rebase_segments(const Cro* c, uint32_t size, uint32_t data, uint32_t data_size, uint32_t bss,
                                uint32_t bss_size, uint32_t* old_data) {
    *old_data = 0;
    for (uint32_t i = 0, n = get(c, F_SEGMENT_NUM); i < n; ++i) {
        uint32_t at = entry(c, F_SEGMENT_OFFSET, 12, i), offset = rd32(c, at), length = rd32(c, at + 4),
                 type = rd32(c, at + 8);
        if (type == SEGMENT_DATA) {
            if (length) {
                if (length > data_size) return BUFFER_TOO_SMALL;
                *old_data = offset;
                wr32(c, at, data);
            }
        } else if (type == SEGMENT_BSS) {
            if (length) {
                if (length > bss_size) return BUFFER_TOO_SMALL;
                wr32(c, at, bss);
            }
        } else if (offset) {
            offset += c->base;
            wr32(c, at, offset);
            if (offset > c->base + size) return FORMAT(0x19);
        }
    }
    return 0;
}
static uint32_t rebase_tables(const Cro* c) {
    uint32_t result;
    uint32_t low = get(c, F_EXPORT_STRINGS_OFFSET), high = low + get(c, F_EXPORT_STRINGS_SIZE);
    for (uint32_t i = 0, n = get(c, F_EXPORT_NAMED_NUM); i < n; ++i)
        if ((result = rebase_word(c, entry(c, F_EXPORT_NAMED_OFFSET, 8, i), low, high, false, FORMAT(0x11)))) return result;
    for (uint32_t i = 0, n = get(c, F_EXPORT_TREE_NUM); i < n; ++i) {
        uint32_t node = entry(c, F_EXPORT_TREE_OFFSET, 8, i), words = rd32(c, node + 2);
        if ((words & 0x7fff) >= n || (words >> 16 & 0x7fff) >= n) return FORMAT(0x11);
    }
    return 0;
}
static uint32_t verify_strings(const Cro* c, uint32_t address, uint32_t size) {
    unsigned char last;
    if (size && (snx_ctr_read_ram(c->p->space, address + size - 1, &last, 1) != SNX_CTR_MEM_OK || last))
        return FORMAT(0x0b);
    return 0;
}
static uint32_t rebase_imports(const Cro* c) {
    uint32_t result;
    uint32_t strings = get(c, F_IMPORT_STRINGS_OFFSET), strings_end = strings + get(c, F_IMPORT_STRINGS_SIZE);
    uint32_t indexed = get(c, F_IMPORT_INDEXED_OFFSET), indexed_end = indexed + get(c, F_IMPORT_INDEXED_NUM) * 8;
    uint32_t anon = get(c, F_IMPORT_ANON_OFFSET), anon_end = anon + get(c, F_IMPORT_ANON_NUM) * 8;
    uint32_t external = get(c, F_EXTERNAL_OFFSET), external_end = external + get(c, F_EXTERNAL_NUM) * 12;
    for (uint32_t i = 0, n = get(c, F_IMPORT_MODULE_NUM); i < n; ++i) {
        uint32_t at = entry(c, F_IMPORT_MODULE_OFFSET, 20, i);
        if ((result = rebase_word(c, at, strings, strings_end, false, FORMAT(0x18))) ||
            (result = rebase_word(c, at + 4, indexed, indexed_end, true, FORMAT(0x18))) ||
            (result = rebase_word(c, at + 12, anon, anon_end, true, FORMAT(0x18))))
            return result;
    }
    /* every external relocation starts unresolved, pointing at the module's own OnUnresolved */
    uint32_t unresolved = seg_address(c, get(c, F_ON_UNRESOLVED_TAG));
    uint32_t count = get(c, F_EXTERNAL_NUM);
    if (count && !rel_end(c, entry(c, F_EXTERNAL_OFFSET, 12, count - 1))) return FORMAT(0x12);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t at = entry(c, F_EXTERNAL_OFFSET, 12, i), target = seg_address(c, rel_tag(c, at));
        if (!target) return FORMAT(0x12);
        if ((result = apply_relocation(c, target, rel_type(c, at), rd32(c, at + 8), unresolved, target))) return result;
        if (i == 0 || rel_end(c, entry(c, F_EXTERNAL_OFFSET, 12, i - 1))) rel_set_resolved(c, at, false);
    }
    for (uint32_t i = 0, n = get(c, F_IMPORT_NAMED_NUM); i < n; ++i) {
        uint32_t at = entry(c, F_IMPORT_NAMED_OFFSET, 8, i);
        if ((result = rebase_word(c, at, strings, strings_end, false, FORMAT(0x1b))) ||
            (result = rebase_word(c, at + 4, external, external_end, true, FORMAT(0x1b))))
            return result;
    }
    for (uint32_t i = 0, n = get(c, F_IMPORT_INDEXED_NUM); i < n; ++i)
        if ((result = rebase_word(c, entry(c, F_IMPORT_INDEXED_OFFSET, 8, i) + 4, external, external_end, true, FORMAT(0x14))))
            return result;
    for (uint32_t i = 0, n = get(c, F_IMPORT_ANON_NUM); i < n; ++i)
        if ((result = rebase_word(c, entry(c, F_IMPORT_ANON_OFFSET, 8, i) + 4, external, external_end, true, FORMAT(0x17))))
            return result;
    return 0;
}
/* the static module's relocations that the library's anonymous exports resolve */
static uint32_t apply_static_anonymous(const Cro* c, uint32_t crs_address) {
    Cro crs = {c->p, crs_address};
    uint32_t low = get(c, F_STATIC_OFFSET), high = low + get(c, F_STATIC_NUM) * 12;
    for (uint32_t i = 0, n = get(c, F_STATIC_ANON_NUM); i < n; ++i) {
        uint32_t at = entry(c, F_STATIC_ANON_OFFSET, 8, i), batch = rd32(c, at + 4) + c->base;
        if (batch < low || batch > high) return FORMAT(0x16);
        uint32_t result = apply_batch(&crs, batch, seg_address(c, rd32(c, at)), false);
        if (result) return result;
    }
    return 0;
}
static uint32_t apply_internal(const Cro* c, uint32_t old_data) {
    uint32_t segments = get(c, F_SEGMENT_NUM);
    for (uint32_t i = 0, n = get(c, F_INTERNAL_NUM); i < n; ++i) {
        uint32_t at = entry(c, F_INTERNAL_OFFSET, 12, i), tag = rd32(c, at), word = rd32(c, at + 4);
        uint32_t final = seg_address(c, tag);
        if (!final) return FORMAT(0x15);
        /* a target in the data segment is relocated where the library holds it, to be copied after the link */
        uint32_t target = rd32(c, entry(c, F_SEGMENT_OFFSET, 12, tag & 15) + 8) == SEGMENT_DATA ? old_data + (tag >> 4) : final;
        unsigned symbol_segment = word >> 8 & 0xff;
        if (symbol_segment >= segments) return FORMAT(0x15);
        uint32_t symbol = rd32(c, entry(c, F_SEGMENT_OFFSET, 12, symbol_segment));
        uint32_t result = apply_relocation(c, target, word & 0xff, rd32(c, at + 8), symbol, final);
        if (result) return result;
    }
    return 0;
}
/* the modules linked at load: the static module, then the registered ones */
static uint32_t next_module(const Cro* c) { return get(c, F_NEXT); }
static uint32_t apply_exit(const Cro* c, uint32_t crs_address) {
    uint32_t strings = get(c, F_IMPORT_STRINGS_SIZE);
    for (uint32_t i = 0, n = get(c, F_IMPORT_NAMED_NUM); i < n; ++i) {
        uint32_t at = entry(c, F_IMPORT_NAMED_OFFSET, 8, i), batch = rd32(c, at + 4);
        char name[64];
        cstring(c, rd32(c, at), strings, name, sizeof name);
        if (strcmp(name, "__aeabi_atexit")) continue;
        for (uint32_t m = crs_address; m;) {
            Cro source = {c->p, m};
            uint32_t symbol = find_export(&source, "nnroAeabiAtexit_");
            if (symbol) {
                uint32_t result = apply_batch(c, batch, symbol, false);
                if (result) return result;
                break;
            }
            m = next_module(&source);
        }
    }
    return 0;
}
static uint32_t rebase(const Cro* c, uint32_t crs_address, uint32_t size, uint32_t data, uint32_t data_size,
                       uint32_t bss, uint32_t bss_size, bool is_crs, uint32_t* old_data) {
    uint32_t result = rebase_header(c, size);
    *old_data = 0;
    if (result) return result;
    if ((result = verify_strings(c, get(c, F_MODULE_NAME_OFFSET), get(c, F_MODULE_NAME_SIZE)))) return result;
    if (!is_crs && (result = rebase_segments(c, size, data, data_size, bss, bss_size, old_data))) return result;
    if ((result = rebase_tables(c))) return result;
    if ((result = verify_strings(c, get(c, F_EXPORT_STRINGS_OFFSET), get(c, F_EXPORT_STRINGS_SIZE)))) return result;
    if ((result = rebase_imports(c))) return result;
    if ((result = verify_strings(c, get(c, F_IMPORT_STRINGS_OFFSET), get(c, F_IMPORT_STRINGS_SIZE)))) return result;
    if (!is_crs && (result = apply_static_anonymous(c, crs_address))) return result;
    if ((result = apply_internal(c, *old_data ? *old_data + c->base : 0))) return result;
    if (!is_crs && (result = apply_exit(c, crs_address))) return result;
    return 0;
}
static void unrebase_word(const Cro* c, uint32_t address) {
    uint32_t offset = rd32(c, address);
    if (offset) wr32(c, address, offset - c->base);
}
static void unrebase(const Cro* c, bool is_crs) {
    for (uint32_t i = 0, n = get(c, F_IMPORT_ANON_NUM); i < n; ++i) unrebase_word(c, entry(c, F_IMPORT_ANON_OFFSET, 8, i) + 4);
    for (uint32_t i = 0, n = get(c, F_IMPORT_INDEXED_NUM); i < n; ++i)
        unrebase_word(c, entry(c, F_IMPORT_INDEXED_OFFSET, 8, i) + 4);
    for (uint32_t i = 0, n = get(c, F_IMPORT_NAMED_NUM); i < n; ++i) {
        uint32_t at = entry(c, F_IMPORT_NAMED_OFFSET, 8, i);
        unrebase_word(c, at);
        unrebase_word(c, at + 4);
    }
    for (uint32_t i = 0, n = get(c, F_IMPORT_MODULE_NUM); i < n; ++i) {
        uint32_t at = entry(c, F_IMPORT_MODULE_OFFSET, 20, i);
        unrebase_word(c, at);
        unrebase_word(c, at + 4);
        unrebase_word(c, at + 12);
    }
    for (uint32_t i = 0, n = get(c, F_EXPORT_NAMED_NUM); i < n; ++i) unrebase_word(c, entry(c, F_EXPORT_NAMED_OFFSET, 8, i));
    if (!is_crs)
        for (uint32_t i = 0, n = get(c, F_SEGMENT_NUM); i < n; ++i) {
            uint32_t at = entry(c, F_SEGMENT_OFFSET, 12, i);
            if (rd32(c, at + 8) == SEGMENT_BSS)
                wr32(c, at, 0);
            else
                unrebase_word(c, at);
        }
    set(c, F_NEXT, 0);
    set(c, F_PREVIOUS, 0);
    set(c, F_FIXED_SIZE, 0);
    unrebase_word(c, c->base + HASH_SIZE + F_NAME_OFFSET * 4);
    for (unsigned field = F_CODE_OFFSET; field < F_FIX0_BARRIER; field += 2) {
        uint32_t offset = get(c, field);
        if (offset) set(c, field, offset - c->base);
    }
}
/* what each import of the module gets from the registered modules */
static uint32_t link_named(const Cro* c, uint32_t crs_address) {
    uint32_t strings = get(c, F_IMPORT_STRINGS_SIZE);
    for (uint32_t i = 0, n = get(c, F_IMPORT_NAMED_NUM); i < n; ++i) {
        uint32_t at = entry(c, F_IMPORT_NAMED_OFFSET, 8, i), batch = rd32(c, at + 4);
        if (rel_resolved(c, batch)) continue;
        char name[256];
        cstring(c, rd32(c, at), strings, name, sizeof name);
        for (uint32_t m = crs_address; m;) {
            Cro source = {c->p, m};
            uint32_t symbol = find_export(&source, name);
            if (symbol) {
                uint32_t result = apply_batch(c, batch, symbol, false);
                if (result) return result;
                break;
            }
            m = next_module(&source);
        }
    }
    return 0;
}
static uint32_t link_modules(const Cro* c, uint32_t crs_address) {
    uint32_t strings = get(c, F_IMPORT_STRINGS_SIZE);
    for (uint32_t i = 0, n = get(c, F_IMPORT_MODULE_NUM); i < n; ++i) {
        uint32_t at = entry(c, F_IMPORT_MODULE_OFFSET, 20, i);
        char wanted[256];
        cstring(c, rd32(c, at), strings, wanted, sizeof wanted);
        for (uint32_t m = crs_address; m;) {
            Cro source = {c->p, m};
            char name[256];
            cstring(&source, get(&source, F_MODULE_NAME_OFFSET), get(&source, F_MODULE_NAME_SIZE), name, sizeof name);
            if (!strcmp(wanted, name)) {
                for (uint32_t j = 0, count = rd32(c, at + 8); j < count; ++j) {
                    uint32_t im = rd32(c, at + 4) + j * 8;
                    uint32_t ex = entry(&source, F_EXPORT_INDEXED_OFFSET, 4, rd32(c, im));
                    uint32_t result = apply_batch(c, rd32(c, im + 4), seg_address(&source, rd32(&source, ex)), false);
                    if (result) return result;
                }
                for (uint32_t j = 0, count = rd32(c, at + 16); j < count; ++j) {
                    uint32_t im = rd32(c, at + 12) + j * 8;
                    uint32_t result = apply_batch(c, rd32(c, im + 4), seg_address(&source, rd32(c, im)), false);
                    if (result) return result;
                }
                break;
            }
            m = next_module(&source);
        }
    }
    return 0;
}
/* the symbols that the module exports to a registered module (target) which imports them */
static uint32_t export_to(const Cro* c, const Cro* target, bool reset) {
    uint32_t unresolved = seg_address(target, get(target, F_ON_UNRESOLVED_TAG));
    uint32_t strings = get(target, F_IMPORT_STRINGS_SIZE);
    for (uint32_t i = 0, n = get(target, F_IMPORT_NAMED_NUM); i < n; ++i) {
        uint32_t at = entry(target, F_IMPORT_NAMED_OFFSET, 8, i), batch = rd32(target, at + 4);
        if (rel_resolved(target, batch) != reset) continue;
        char name[256];
        cstring(target, rd32(target, at), strings, name, sizeof name);
        uint32_t symbol = find_export(c, name);
        if (symbol) {
            uint32_t result = apply_batch(target, batch, reset ? unresolved : symbol, reset);
            if (result) return result;
        }
    }
    char own[256];
    cstring(c, get(c, F_MODULE_NAME_OFFSET), get(c, F_MODULE_NAME_SIZE), own, sizeof own);
    for (uint32_t i = 0, n = get(target, F_IMPORT_MODULE_NUM); i < n; ++i) {
        uint32_t at = entry(target, F_IMPORT_MODULE_OFFSET, 20, i);
        char wanted[256];
        cstring(target, rd32(target, at), strings, wanted, sizeof wanted);
        if (strcmp(wanted, own)) continue;
        for (uint32_t j = 0, count = rd32(target, at + 8); j < count; ++j) {
            uint32_t im = rd32(target, at + 4) + j * 8;
            uint32_t ex = entry(c, F_EXPORT_INDEXED_OFFSET, 4, rd32(target, im));
            uint32_t result = apply_batch(target, rd32(target, im + 4), reset ? unresolved : seg_address(c, rd32(c, ex)), reset);
            if (result) return result;
        }
        for (uint32_t j = 0, count = rd32(target, at + 16); j < count; ++j) {
            uint32_t im = rd32(target, at + 12) + j * 8;
            uint32_t result = apply_batch(target, rd32(target, im + 4), reset ? unresolved : seg_address(c, rd32(target, im)), reset);
            if (result) return result;
        }
    }
    return 0;
}
static uint32_t link_exports(const Cro* c, uint32_t crs_address, bool reset) {
    for (uint32_t m = crs_address; m;) {
        Cro target = {c->p, m};
        uint32_t result = export_to(c, &target, reset);
        if (result) return result;
        m = next_module(&target);
    }
    return 0;
}
/* Links the module with the registered ones. With the fix of the console's 7.2 (LoadCRO_New) the data segment is
   still in the library while the imports land, so they reach the copy the game gets */
static uint32_t link_cro(const Cro* c, uint32_t crs_address, bool at_load_fixed) {
    uint32_t saved = 0, data_entry = 0, result;
    bool moved = at_load_fixed && get(c, F_SEGMENT_NUM) >= 2;
    if (moved) {
        data_entry = entry(c, F_SEGMENT_OFFSET, 12, 2);
        saved = rd32(c, data_entry);
        wr32(c, data_entry, get(c, F_DATA_OFFSET));
    }
    result = link_named(c, crs_address);
    if (!result) result = link_modules(c, crs_address);
    if (moved) wr32(c, data_entry, saved);
    return result ? result : link_exports(c, crs_address, false);
}
static uint32_t reset_imports(const Cro* c) {
    uint32_t unresolved = seg_address(c, get(c, F_ON_UNRESOLVED_TAG)), result;
    static const struct {
        unsigned offset, count, size;
    } tables[3] = {{F_IMPORT_NAMED_OFFSET, F_IMPORT_NAMED_NUM, 8},
                   {F_IMPORT_INDEXED_OFFSET, F_IMPORT_INDEXED_NUM, 8},
                   {F_IMPORT_ANON_OFFSET, F_IMPORT_ANON_NUM, 8}};
    for (unsigned t = 0; t < 3; ++t)
        for (uint32_t i = 0, n = get(c, tables[t].count); i < n; ++i)
            if ((result = apply_batch(c, rd32(c, entry(c, tables[t].offset, tables[t].size, i) + 4), unresolved, true)))
                return result;
    return 0;
}
static uint32_t unlink_cro(const Cro* c, uint32_t crs_address) {
    uint32_t result = reset_imports(c);
    return result ? result : link_exports(c, crs_address, true);
}
static uint32_t clear_relocations(const Cro* c) {
    uint32_t result;
    bool begin = true;
    for (uint32_t i = 0, n = get(c, F_EXTERNAL_NUM); i < n; ++i) {
        uint32_t at = entry(c, F_EXTERNAL_OFFSET, 12, i), target = seg_address(c, rel_tag(c, at));
        if (!target) return FORMAT(0x12);
        if ((result = clear_relocation(c, target, rel_type(c, at)))) return result;
        if (begin) rel_set_resolved(c, at, false);
        begin = rel_end(c, at);
    }
    for (uint32_t i = 0, n = get(c, F_INTERNAL_NUM); i < n; ++i) {
        uint32_t at = entry(c, F_INTERNAL_OFFSET, 12, i), target = seg_address(c, rd32(c, at));
        if (!target) return FORMAT(0x15);
        if ((result = clear_relocation(c, target, rd32(c, at + 4) & 0xff))) return result;
    }
    return 0;
}
/* the modules registered after the static module: a list of auto-link ones from its next, and the others from its
   previous, each head pointing back to its tail */
static void register_cro(const Cro* c, uint32_t crs_address, bool auto_link) {
    Cro crs = {c->p, crs_address};
    Cro head = {c->p, auto_link ? get(&crs, F_NEXT) : get(&crs, F_PREVIOUS)};
    if (head.base) {
        Cro tail = {c->p, get(&head, F_PREVIOUS)};
        set(c, F_PREVIOUS, tail.base);
        set(&tail, F_NEXT, c->base);
        set(&head, F_PREVIOUS, c->base);
    } else {
        set(c, F_PREVIOUS, c->base);
        set(&crs, auto_link ? F_NEXT : F_PREVIOUS, c->base);
    }
    set(c, F_NEXT, 0);
}
static void unregister_cro(const Cro* c, uint32_t crs_address) {
    Cro crs = {c->p, crs_address};
    Cro next_head = {c->p, get(&crs, F_NEXT)}, previous_head = {c->p, get(&crs, F_PREVIOUS)};
    Cro next = {c->p, get(c, F_NEXT)}, previous = {c->p, get(c, F_PREVIOUS)};
    if (c->base == next_head.base || c->base == previous_head.base) {
        if (next.base) set(&next, F_PREVIOUS, previous.base);
        set(&crs, c->base == previous_head.base ? F_PREVIOUS : F_NEXT, next.base);
    } else if (next.base) {
        set(&previous, F_NEXT, next.base);
        set(&next, F_PREVIOUS, previous.base);
    } else {
        set(&previous, F_NEXT, 0);
        if (next_head.base && get(&next_head, F_PREVIOUS) == c->base)
            set(&next_head, F_PREVIOUS, previous.base);
        else if (previous_head.base && get(&previous_head, F_PREVIOUS) == c->base)
            set(&previous_head, F_PREVIOUS, previous.base);
    }
    set(c, F_NEXT, 0);
    set(c, F_PREVIOUS, 0);
}
/* The end of what stays for the fix level, and the module cut to it: the tables past the barrier go, so the memory
   from there to the end of the library is the game's again. Returns the size that stays, page-aligned */
static uint32_t fix_end(const Cro* c, uint32_t level) {
    uint32_t end = HEADER_SIZE;
    uint32_t code = get(c, F_CODE_OFFSET) + get(c, F_CODE_SIZE);
    if (code > end) end = code;
    for (unsigned field = F_MODULE_NAME_OFFSET, size = 2; field != fix_barrier[level]; field += 2, ++size) {
        uint32_t table = get(c, field) + get(c, field + 1) * entry_size[size];
        if (table > end) end = table;
    }
    return end;
}
static uint32_t fix(const Cro* c, uint32_t level) {
    uint32_t end = fix_end(c, level);
    if (level) {
        set(c, F_MAGIC, MAGIC_FIXD);
        for (unsigned field = fix_barrier[level]; field < F_FIX0_BARRIER; field += 2) {
            set(c, field, end);
            set(c, field + 1, 0);
        }
    }
    uint32_t fixed = snx_align_up(end, ALIGNED_PAGE) - c->base;
    set(c, F_FIXED_SIZE, fixed);
    return fixed;
}
static bool is_loaded(const Cro* c) {
    uint32_t magic = get(c, F_MAGIC);
    return magic == MAGIC_CRO0 || magic == MAGIC_FIXD;
}
static void name_of(const Cro* c, char* out, size_t size) {
    cstring(c, get(c, F_MODULE_NAME_OFFSET), get(c, F_MODULE_NAME_SIZE), out, size);
}
/* ---- the service ---- */
static void reply(SnxCtrIpc* c, unsigned command, unsigned normal, uint32_t result, uint32_t value) {
    c->out[0] = command << 16 | normal << 6;
    c->out[1] = result;
    if (normal > 1) c->out[2] = value;
}
/* the game's buffer must be private, read-write pages, and the library goes to the process image region */
static bool image_region(uint32_t address, uint32_t size) {
    return address >= 0x00100000u && (uint64_t)address + size <= 0x08000000u;
}
static uint32_t check_buffer(SnxCtrProcess* p, uint32_t buffer, uint32_t address, uint32_t size, uint32_t minimum) {
    if (size < minimum) return BUFFER_TOO_SMALL;
    if ((buffer | address) & (ALIGNED_PAGE - 1)) return MISALIGNED_ADDRESS;
    if (size & (ALIGNED_PAGE - 1)) return MISALIGNED_SIZE;
    for (uint32_t off = 0; off < size; off += ALIGNED_PAGE) {
        unsigned permissions;
        if (snx_ctr_query(p->space, buffer + off, NULL, &permissions) != SNX_CTR_MEM_OK || (permissions & 3) != 3)
            return INVALID_MEMORY_STATE;
    }
    return image_region(address, size) ? 0 : ILLEGAL_ADDRESS;
}
static void initialize(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 3, 2)) return;
    SnxCtrProcess* p = c->process;
    uint32_t buffer = c->in[1], size = c->in[2], address = c->in[3], result;
    if (p->ro_crs)
        result = ALREADY_INITIALIZED;
    else if ((result = check_buffer(p, buffer, address, size, HEADER_SIZE)))
        ;
    else if ((result = snx_ctr_alias_map(p, address, buffer, size)))
        ;
    else {
        Cro crs = {p, address};
        uint32_t old;
        set(&crs, F_NEXT, 0);
        set(&crs, F_PREVIOUS, 0);
        result = rebase(&crs, 0, size, 0, 0, 0, 0, true, &old);
        if (result)
            (void)snx_ctr_alias_unmap(p, address, buffer, size);
        else
            p->ro_crs = address, p->ro_crs_buffer = buffer, p->ro_crs_size = size;
    }
    if (result) fprintf(stderr, "3ds ro: Initialize failed with 0x%08x\n", result);
    reply(c, 1, 1, result, 0);
}
static void load_cro(SnxCtrIpc* c, bool fixed) {
    if (!snx_ctr_ipc_expect(c, 11, 2)) return;
    SnxCtrProcess* p = c->process;
    uint32_t buffer = c->in[1], address = c->in[2], size = c->in[3], data = c->in[4], zero = c->in[5],
             data_size = c->in[6], bss = c->in[7], bss_size = c->in[8], level = c->in[10], result;
    bool auto_link = c->in[9] & 0xff;
    unsigned command = c->in[0] >> 16;
    uint32_t fixed_size = 0;
    if (!p->ro_crs)
        result = NOT_INITIALIZED;
    else if ((result = check_buffer(p, buffer, address, size, HEADER_SIZE)))
        ;
    else if (zero)
        result = 0xe1612c1du;
    else if (level > 3)
        result = FORMAT(0x1d);
    else if ((result = snx_ctr_alias_map(p, address, buffer, size)))
        ;
    else {
        Cro cro = {p, address};
        uint32_t old_data;
        result = rebase(&cro, p->ro_crs, size, data, data_size, bss, bss_size, false, &old_data);
        if (!result) result = link_cro(&cro, p->ro_crs, fixed);
        if (!result) {
            /* the data goes to the game's buffer, with the imports of the link in it when the fix put them there, and
               the bss is cleared */
            uint32_t length = 0;
            for (uint32_t i = 0, n = get(&cro, F_SEGMENT_NUM); i < n; ++i) {
                uint32_t at = entry(&cro, F_SEGMENT_OFFSET, 12, i);
                if (rd32(&cro, at + 8) == SEGMENT_DATA) length = rd32(&cro, at + 4);
            }
            if (length && old_data) {
                unsigned char* bytes = malloc(length);
                if (!bytes || snx_ctr_read_ram(p->space, old_data + address, bytes, length) != SNX_CTR_MEM_OK ||
                    !write_bytes(p, data, bytes, length))
                    result = INVALID_MEMORY_STATE;
                free(bytes);
            }
            for (uint32_t i = 0, n = get(&cro, F_SEGMENT_NUM); i < n && !result; ++i) {
                uint32_t at = entry(&cro, F_SEGMENT_OFFSET, 12, i), length_bss = rd32(&cro, at + 4);
                if (rd32(&cro, at + 8) == SEGMENT_BSS && length_bss && bss_size >= length_bss) {
                    unsigned char* zeros = calloc(1, length_bss);
                    if (!zeros || !write_bytes(p, bss, zeros, length_bss)) result = INVALID_MEMORY_STATE;
                    free(zeros);
                }
            }
        }
        if (!result) {
            register_cro(&cro, p->ro_crs, auto_link);
            fixed_size = fix(&cro, level);
            if (fixed_size < size) result = snx_ctr_alias_trim(p, address, fixed_size);
        }
        if (!result) {
            /* the library is read-only to the game, its code executable */
            (void)snx_ctr_protect(p->space, address, fixed_size, SNX_CTR_READ);
            for (uint32_t i = 0, n = get(&cro, F_SEGMENT_NUM); i < n; ++i) {
                uint32_t at = entry(&cro, F_SEGMENT_OFFSET, 12, i), offset = rd32(&cro, at), length = rd32(&cro, at + 4);
                if (rd32(&cro, at + 8) == SEGMENT_CODE && length) {
                    uint32_t begin = offset & ~(uint32_t)(ALIGNED_PAGE - 1), end = snx_align_up(offset + length, ALIGNED_PAGE);
                    (void)snx_ctr_protect(p->space, begin, end - begin, SNX_CTR_READ | SNX_CTR_EXEC);
                    break;
                }
            }
            char name[64];
            name_of(&cro, name, sizeof name);
            if (g_settings.ctr_ipc_trace) fprintf(stderr, "3ds ro: \"%s\" loaded at 0x%08x, 0x%x bytes kept\n", name, address, fixed_size);
        } else if (p->ro_crs) {
            (void)snx_ctr_alias_unmap(p, address, buffer, size);
        }
    }
    if (result) fprintf(stderr, "3ds ro: LoadCRO failed with 0x%08x\n", result);
    reply(c, command, 2, result, result ? 0 : fixed_size);
}
static void unload_cro(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 3, 2)) return;
    SnxCtrProcess* p = c->process;
    uint32_t address = c->in[1], buffer = c->in[3], result = 0;
    Cro cro = {p, address};
    if (!p->ro_crs)
        result = NOT_INITIALIZED;
    else if (address & (ALIGNED_PAGE - 1))
        result = MISALIGNED_ADDRESS;
    else if (!is_loaded(&cro))
        result = NOT_LOADED;
    else {
        uint32_t fixed_size = get(&cro, F_FIXED_SIZE);
        bool fixed = get(&cro, F_MAGIC) == MAGIC_FIXD;
        unregister_cro(&cro, p->ro_crs);
        result = unlink_cro(&cro, p->ro_crs);
        if (!result && !fixed) result = clear_relocations(&cro);
        if (!result) {
            unrebase(&cro, false);
            result = snx_ctr_alias_unmap(p, address, buffer, fixed_size);
        }
    }
    if (result) fprintf(stderr, "3ds ro: UnloadCRO failed with 0x%08x\n", result);
    reply(c, 5, 1, result, 0);
}
static void link_command(SnxCtrIpc* c, bool link) {
    if (!snx_ctr_ipc_expect(c, 1, 2)) return;
    SnxCtrProcess* p = c->process;
    uint32_t address = c->in[1], result;
    Cro cro = {p, address};
    if (!p->ro_crs)
        result = NOT_INITIALIZED;
    else if (address & (ALIGNED_PAGE - 1))
        result = MISALIGNED_ADDRESS;
    else if (!is_loaded(&cro))
        result = NOT_LOADED;
    else
        result = link ? link_cro(&cro, p->ro_crs, false) : unlink_cro(&cro, p->ro_crs);
    if (result) fprintf(stderr, "3ds ro: %s failed with 0x%08x\n", link ? "LinkCRO" : "UnlinkCRO", result);
    reply(c, link ? 6 : 7, 1, result, 0);
}
static void shutdown_ro(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 1, 2)) return;
    SnxCtrProcess* p = c->process;
    uint32_t result = 0;
    if (!p->ro_crs)
        result = NOT_INITIALIZED;
    else {
        Cro crs = {p, p->ro_crs};
        unrebase(&crs, true);
        result = snx_ctr_alias_unmap(p, p->ro_crs, p->ro_crs_buffer, p->ro_crs_size);
        p->ro_crs = 0;
    }
    reply(c, 8, 1, result, 0);
}
void snx_ctr_ro_dispatch(SnxCtrIpc* c) {
    switch (c->in[0] >> 16) {
        case 0x01: initialize(c); break;
        case 0x02: /* LoadCRR(buffer, size, process): the signature of the library is not checked */
            if (!snx_ctr_ipc_expect(c, 2, 2)) return;
            reply(c, 2, 1, 0, 0);
            break;
        case 0x03: /* UnloadCRR(buffer, process) */
            if (!snx_ctr_ipc_expect(c, 1, 2)) return;
            reply(c, 3, 1, 0, 0);
            break;
        case 0x04: load_cro(c, false); break;
        case 0x05: unload_cro(c); break;
        case 0x06: link_command(c, true); break;
        case 0x07: link_command(c, false); break;
        case 0x08: shutdown_ro(c); break;
        case 0x09: load_cro(c, true); break;
        default: c->state = SNX_CTR_IPC_UNKNOWN;
    }
}
