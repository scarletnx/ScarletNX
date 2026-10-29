/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Port of Azahar service/ldr_ro/cro_helper.cpp at a4b335e.
   Copyright 2016 Citra Emulator Project
   Licensed under GPLv2 or any later version.
   The CRO format and its load-time work, ported to the runtime's checked guest memory. A CRO's header starts at
   module_address + 0x80, every field is a little-endian u32 in the order of the header enum below, and the tables it
   points at hold 1 to 20-byte records. */
#include "3ds/services/cro.h"
#include "3ds/services/ldr.h"
#include "3ds/runtime/internal.h"
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"
#include <assert.h>
#include <string.h>
#include <stdlib.h>

#define CRO_MAGIC          0x304f5243u /* "CRO0" */
#define CRO_FIXD           0x44584946u /* "FIXD" */
#define CRO_HEADER         0x138u      /* the header, its hash included */
#define CRO_HASH           0x80u
#define CRO_MAX_SIZE       0x10000000u
#define CRO_MAX_SEGMENTS   16u /* four bits in a segment tag */
#define ADDRESS_SPACE      0x100000000ull
#define SEGMENT_SIZE       12u
#define RELOCATION_SIZE    12u
#define IMPORT_SIZE        8u
#define UNDO_BYTES         12u
#define UNDO_INITIAL       64u
#define WORD_BYTES         4u
#define IMPORT_MODULE_PAIR 8u         /* an import module's offset word and count word, for each of its two tables */
#define TREE_INDEX_MASK    0x7fffu    /* the high bit of an export tree link is a flag, not part of its node index */
#define NO_DATA_IMAGE      UINT32_MAX /* rebase_crs: a CRS has no .data image that its relocations write into first */
/* the kinds of relocation records that the loader applies */
enum { RELOCATION_NOTHING = 0, RELOCATION_ABSOLUTE = 2, RELOCATION_RELATIVE = 3, RELOCATION_ABSOLUTE2 = 38 };
/* the offsets of the fields of the records that do not start at zero */
enum {
    SEGMENT_BYTES_AT = 4,
    SEGMENT_TYPE_AT = 8,
    EXPORT_TAG_AT = 4,
    TREE_LEFT_AT = 2,
    TREE_RIGHT_AT = 4,
    IMPORT_BATCH_AT = 4, /* of a named, indexed or anonymous import, and of a static anonymous entry */
    IMPORT_MODULE_INDEXED_AT = 4,
    IMPORT_MODULE_ANONYMOUS_AT = 12,
    RELOCATION_KIND_AT = 4,
    RELOCATION_END_AT = 5,    /* of a batch, in the external and static tables */
    RELOCATION_SYMBOL_AT = 5, /* the symbol's segment, in the internal table */
    RELOCATION_STATE_AT = 6,  /* of the first record of a batch: 1 once the batch is applied */
    RELOCATION_ADDEND_AT = 8
};
/* the bytes that the tag of a relocation of this kind names: a Nothing record names one byte, the others a word */
static uint32_t relocation_width(uint32_t type) { return type == RELOCATION_NOTHING ? 1 : WORD_BYTES; }

enum {
    MAGIC,
    NAME_OFFSET,
    NEXT_CRO,
    PREVIOUS_CRO,
    FILE_SIZE,
    BSS_SIZE,
    FIXED_SIZE,
    UNKNOWN_ZERO,
    UNK_SEGMENT_TAG,
    ON_LOAD_TAG,
    ON_EXIT_TAG,
    ON_UNRESOLVED_TAG,
    CODE_OFFSET,
    CODE_SIZE,
    DATA_OFFSET,
    DATA_SIZE,
    MODULE_NAME_OFFSET,
    MODULE_NAME_SIZE,
    SEGMENT_TABLE,
    SEGMENT_NUM,
    EXPORT_NAMED_TABLE,
    EXPORT_NAMED_NUM,
    EXPORT_INDEXED_TABLE,
    EXPORT_INDEXED_NUM,
    EXPORT_STRINGS,
    EXPORT_STRINGS_SIZE,
    EXPORT_TREE_TABLE,
    EXPORT_TREE_NUM,
    IMPORT_MODULE_TABLE,
    IMPORT_MODULE_NUM,
    EXTERNAL_RELOCATION_TABLE,
    EXTERNAL_RELOCATION_NUM,
    IMPORT_NAMED_TABLE,
    IMPORT_NAMED_NUM,
    IMPORT_INDEXED_TABLE,
    IMPORT_INDEXED_NUM,
    IMPORT_ANONYMOUS_TABLE,
    IMPORT_ANONYMOUS_NUM,
    IMPORT_STRINGS,
    IMPORT_STRINGS_SIZE,
    STATIC_ANONYMOUS_TABLE,
    STATIC_ANONYMOUS_NUM,
    INTERNAL_RELOCATION_TABLE,
    INTERNAL_RELOCATION_NUM,
    STATIC_RELOCATION_TABLE,
    STATIC_RELOCATION_NUM,
    FIX0_BARRIER
};
enum { SEGMENT_CODE, SEGMENT_RODATA, SEGMENT_DATA, SEGMENT_BSS };
#define FIX1_BARRIER STATIC_ANONYMOUS_TABLE
#define FIX2_BARRIER IMPORT_MODULE_TABLE
#define FIX3_BARRIER EXPORT_NAMED_TABLE

/* the record size of each table, in the order of the header from CODE_OFFSET */
static const uint8_t entry_size[] = {1, 1, 1, 12, 8, 4, 1, 8, 20, 12, 8, 8, 8, 1, 8, 12, 12};
static const uint8_t fix_barrier[] = {FIX0_BARRIER, FIX1_BARRIER, FIX2_BARRIER, FIX3_BARRIER};
static const uint8_t import_tables[] = {IMPORT_NAMED_TABLE, IMPORT_INDEXED_TABLE, IMPORT_ANONYMOUS_TABLE};
static const uint8_t relocation_tables[] = {EXTERNAL_RELOCATION_TABLE, INTERNAL_RELOCATION_TABLE,
                                            STATIC_RELOCATION_TABLE};

typedef struct {
    uint32_t address, size;
    uint8_t bytes[UNDO_BYTES];
} Undo;
struct SnxCtrCroTransaction {
    size_t count, capacity;
    uint32_t module, size, buffer;
    uint32_t data_image, data_target, data_size; /* LoadCRO_New writes imports before transferring .data */
    bool failed;
    Undo writes[];
};
bool snx_ctr_cro_begin(SnxCtrProcess* p, uint32_t module, uint32_t size, uint32_t buffer) {
    if (p->cro_transaction) return false;
    struct SnxCtrCroTransaction* t = calloc(1, sizeof *t + UNDO_INITIAL * sizeof(Undo));
    if (!t) return false;
    t->capacity = UNDO_INITIAL;
    t->module = module;
    t->size = size;
    t->buffer = buffer;
    p->cro_transaction = t;
    return true;
}
bool snx_ctr_cro_good(SnxCtrProcess* p) { return p->cro_transaction && !p->cro_transaction->failed; }

/* ------------------------------------------------------------------ guest memory */
static bool rdb(SnxCtrProcess* p, uint32_t at, void* out, size_t size) {
    bool result = snx_ctr_read_ram(p->space, at, out, size) == SNX_CTR_MEM_OK;
    if (!result && p->cro_transaction) p->cro_transaction->failed = true;
    return result;
}
static uint32_t rd32(SnxCtrProcess* p, uint32_t at) {
    uint8_t bytes[4] = {0};
    rdb(p, at, bytes, sizeof bytes);
    return snx_ld_le32(bytes);
}
static uint16_t rd16(SnxCtrProcess* p, uint32_t at) {
    uint8_t bytes[2] = {0};
    rdb(p, at, bytes, sizeof bytes);
    return snx_ld_le16(bytes);
}
static uint8_t rd8(SnxCtrProcess* p, uint32_t at) {
    uint8_t byte = 0;
    rdb(p, at, &byte, sizeof byte);
    return byte;
}
/* a write through the guest view, or, for a page the process may only read (the static module's text, which the
   service relocates), through the physical memory behind it, as a service's own mapping would */
static bool write_raw(SnxCtrProcess* p, uint32_t at, const void* data, size_t size) {
    if (snx_ctr_write_ram(p->space, at, data, size) == SNX_CTR_MEM_OK) return true;
    const uint8_t* bytes = data;
    while (size) {
        uint32_t pa = 0;
        size_t n = SNX_CTR_PAGE_SIZE - at % SNX_CTR_PAGE_SIZE;
        if (n > size) n = size;
        if (snx_ctr_query(p->space, at, &pa, NULL) != SNX_CTR_MEM_OK) return false;
        if (snx_ctr_phys_write(p->memory, pa, bytes, n) != SNX_CTR_MEM_OK) return false;
        at += (uint32_t)n;
        bytes += n;
        size -= n;
    }
    return true;
}
/* keeps the bytes a write is about to replace, so that a transaction that fails puts them back */
static bool record_undo(SnxCtrProcess* p, uint32_t at, size_t size) {
    struct SnxCtrCroTransaction* t = p->cro_transaction;
    if (size > UNDO_BYTES || size > ADDRESS_SPACE - at) return false;
    if (t->count == t->capacity) {
        if (t->capacity > (SIZE_MAX - sizeof *t) / (2 * sizeof(Undo))) return false;
        size_t capacity = t->capacity * 2;
        t = realloc(t, sizeof *t + capacity * sizeof(Undo));
        if (!t) return false;
        p->cro_transaction = t;
        t->capacity = capacity;
    }
    Undo* u = &t->writes[t->count];
    u->address = at;
    u->size = (uint32_t)size;
    if (!rdb(p, at, u->bytes, size)) return false;
    ++t->count;
    return true;
}
static bool journaled_write(SnxCtrProcess* p, uint32_t at, const void* data, size_t size) {
    if (!record_undo(p, at, size)) return false;
    uint64_t code = p->segment[0].address, end = code + p->segment[0].bytes;
    /* the stale windows are marked before the write, while every view is mapped, so a commit never has to allocate */
    if (at < end && (uint64_t)at + size > code) {
        if (!p->compile || !snx_ctr_aot_mark_stale(p, at, size)) return false;
    }
    return write_raw(p, at, data, size);
}
static bool write_guest(SnxCtrProcess* p, uint32_t at, const void* data, size_t size) {
    if (!p->cro_transaction) return false;
    if (!p->cro_transaction->failed && journaled_write(p, at, data, size)) return true;
    p->cro_transaction->failed = true;
    return false;
}
bool snx_ctr_cro_finish(SnxCtrProcess* p, bool commit) {
    struct SnxCtrCroTransaction* t = p->cro_transaction;
    if (!t) return false;
    bool success = commit && !t->failed;
    if (!success) {
        while (t->count) {
            Undo* u = &t->writes[--t->count];
            /* every journaled address is mapped: the callers unmap only once the commit is certain to succeed */
            if (!write_raw(p, u->address, u->bytes, u->size)) abort();
        }
    }
    free(t);
    p->cro_transaction = NULL;
    if (p->flush) p->flush(p->compiler);
    return success;
}
bool snx_ctr_cro_write(SnxCtrProcess* p, uint32_t address, const void* data, uint32_t size) {
    if (size > ADDRESS_SPACE - address) {
        if (p->cro_transaction) p->cro_transaction->failed = true;
        return false;
    }
    const uint8_t* bytes = data;
    for (uint32_t off = 0; off < size;) {
        uint32_t n = snx_min_u32(size - off, UNDO_BYTES);
        if (!write_guest(p, address + off, bytes + off, n)) return false;
        off += n;
    }
    return true;
}
static void wr32(SnxCtrProcess* p, uint32_t at, uint32_t value) {
    uint8_t bytes[4];
    snx_st_le32(bytes, value);
    (void)write_guest(p, at, bytes, sizeof bytes);
}
static void wr8(SnxCtrProcess* p, uint32_t at, uint8_t value) { (void)write_guest(p, at, &value, 1); }
static void wrb(SnxCtrProcess* p, uint32_t at, const void* data, size_t size) { (void)write_guest(p, at, data, size); }
typedef struct {
    uint32_t address, size; /* size includes the terminator, zero is invalid */
} Name;
/* Scan within the guest table, without imposing a host buffer's length on a name. */
static Name read_name(SnxCtrProcess* p, uint32_t at, uint32_t size) {
    if (size > ADDRESS_SPACE - at) return (Name){0};
    for (uint32_t off = 0; off < size;) {
        uint8_t bytes[256];
        uint32_t n = snx_min_u32(size - off, sizeof bytes);
        if (!rdb(p, at + off, bytes, n)) return (Name){0};
        const uint8_t* end = memchr(bytes, 0, n);
        if (end) return (Name){at, off + (uint32_t)(end - bytes) + 1};
        off += n;
    }
    return (Name){0};
}
static bool same_name(SnxCtrProcess* p, Name a, Name b) {
    if (!a.size || a.size != b.size) return false;
    for (uint32_t off = 0; off < a.size;) {
        uint8_t left[256], right[256];
        uint32_t n = snx_min_u32(a.size - off, sizeof left);
        if (!rdb(p, a.address + off, left, n) || !rdb(p, b.address + off, right, n) || memcmp(left, right, n))
            return false;
        off += n;
    }
    return true;
}
static bool name_is(SnxCtrProcess* p, Name name, const char* literal) {
    if (name.size != strlen(literal) + 1) return false;
    for (uint32_t i = 0; i < name.size; ++i)
        if (rd8(p, name.address + i) != (uint8_t)literal[i]) return false;
    return true;
}

/* ------------------------------------------------------------------ header and tables */
static uint32_t field_address(uint32_t module, uint32_t index) { return module + CRO_HASH + index * 4; }
static uint32_t field(SnxCtrProcess* p, uint32_t module, uint32_t index) {
    return rd32(p, field_address(module, index));
}
static void set_field(SnxCtrProcess* p, uint32_t module, uint32_t index, uint32_t value) {
    wr32(p, field_address(module, index), value);
}
/* the record size of a table whose offset is the header field `table` */
static uint32_t table_size(uint32_t table) { return entry_size[(table - CODE_OFFSET) / 2]; }
static uint32_t entry(SnxCtrProcess* p, uint32_t module, uint32_t table, uint32_t index) {
    return field(p, module, table) + index * table_size(table);
}
static uint32_t segment_tag_address(SnxCtrProcess* p, uint32_t module, uint32_t tag) {
    uint32_t index = tag & 15, offset = tag >> 4;
    if (index >= field(p, module, SEGMENT_NUM)) return 0;
    uint8_t s[SEGMENT_SIZE];
    if (!rdb(p, entry(p, module, SEGMENT_TABLE, index), s, sizeof s)) return 0;
    uint32_t base = snx_ld_le32(s);
    uint32_t size = snx_ld_le32(s + SEGMENT_BYTES_AT);
    if (offset >= size || base > UINT32_MAX - offset) return 0;
    return base + offset;
}
static Name module_name(SnxCtrProcess* p, uint32_t module) {
    return read_name(p, field(p, module, MODULE_NAME_OFFSET), field(p, module, MODULE_NAME_SIZE));
}
static Name table_name(SnxCtrProcess* p, uint32_t module, uint32_t name, uint32_t strings_field) {
    uint32_t first = field(p, module, strings_field), size = field(p, module, strings_field + 1);
    if (name < first || name - first >= size) return (Name){0};
    return read_name(p, name, size - (name - first));
}
static bool verify_string_table(SnxCtrProcess* p, uint32_t at, uint32_t size) {
    return !size || rd8(p, at + size - 1) == 0;
}

/* Validate file offsets before rebasing any of them. Counts are bounded by the supplied image, never by a loop cap. */
static bool file_range(uint32_t size, uint32_t at, uint32_t count, uint32_t width) {
    return at <= size && count <= (size - at) / width && (!count || at >= CRO_HEADER);
}
static bool file_name(SnxCtrProcess* p, uint32_t module, uint32_t name, uint32_t strings_field) {
    uint32_t first = field(p, module, strings_field), size = field(p, module, strings_field + 1);
    return name >= first && name - first < size && read_name(p, module + name, size - (name - first)).size;
}
static bool file_tag(SnxCtrProcess* p, uint32_t module, uint32_t tag, uint32_t width) {
    uint32_t index = tag & 15, offset = tag >> 4;
    if (index >= field(p, module, SEGMENT_NUM)) return false;
    uint32_t bytes = rd32(p, module + entry(p, module, SEGMENT_TABLE, index) + SEGMENT_BYTES_AT);
    return offset <= bytes && width <= bytes - offset;
}
static bool file_batch(SnxCtrProcess* p, uint32_t module, uint32_t batch, uint32_t table) {
    uint32_t first = field(p, module, table), count = field(p, module, table + 1);
    if (batch < first || (batch - first) % RELOCATION_SIZE || (batch - first) / RELOCATION_SIZE >= count) return false;
    for (uint32_t i = (batch - first) / RELOCATION_SIZE; i < count; ++i)
        if (rd8(p, module + first + i * RELOCATION_SIZE + RELOCATION_END_AT)) return true;
    return false;
}
bool snx_ctr_cro_validate(SnxCtrProcess* p, uint32_t module, uint32_t size, bool crs) {
    if (size < CRO_HEADER || size > CRO_MAX_SIZE || size > UINT32_MAX - module ||
        snx_ctr_check_ram(p->space, module, size, SNX_CTR_READ) != SNX_CTR_MEM_OK ||
        field(p, module, MAGIC) != CRO_MAGIC || field(p, module, FILE_SIZE) > size)
        return false;
    uint32_t file_size = field(p, module, FILE_SIZE);
    if (file_size < CRO_HEADER) return false;
    for (uint32_t f = CODE_OFFSET; f < FIX0_BARRIER; f += 2)
        if (!file_range(file_size, field(p, module, f), field(p, module, f + 1), table_size(f))) return false;
    uint32_t segments = field(p, module, SEGMENT_NUM);
    if (!segments || segments > CRO_MAX_SEGMENTS) return false;
    for (uint32_t i = 0; i < segments; ++i) {
        uint32_t at = module + entry(p, module, SEGMENT_TABLE, i);
        uint32_t offset = rd32(p, at), bytes = rd32(p, at + SEGMENT_BYTES_AT), type = rd32(p, at + SEGMENT_TYPE_AT);
        if (type > SEGMENT_BSS || bytes > UINT32_MAX - offset) return false;
        /* CRS segments describe absolute symbol ranges, not necessarily mapped in full. Each relocation access
           is checked when applied; CRO segments still have to fit the supplied file. */
        if (!crs && type != SEGMENT_BSS && !file_range(file_size, offset, bytes, 1)) return false;
        for (uint32_t j = 0; j < i; ++j) {
            uint32_t other = module + entry(p, module, SEGMENT_TABLE, j);
            uint32_t other_offset = rd32(p, other), other_size = rd32(p, other + SEGMENT_BYTES_AT),
                     other_type = rd32(p, other + SEGMENT_TYPE_AT);
            if (!crs && type != SEGMENT_BSS && other_type != SEGMENT_BSS &&
                snx_ranges_overlap(offset, bytes, other_offset, other_size))
                return false;
        }
        if (!crs && type == SEGMENT_DATA && bytes &&
            (offset != field(p, module, DATA_OFFSET) || bytes > field(p, module, DATA_SIZE)))
            return false;
        if (!crs && type == SEGMENT_CODE && bytes &&
            (offset != field(p, module, CODE_OFFSET) || bytes > field(p, module, CODE_SIZE)))
            return false;
    }
    if (!read_name(p, module + field(p, module, MODULE_NAME_OFFSET), field(p, module, MODULE_NAME_SIZE)).size)
        return false;
    for (uint32_t i = 0; i < field(p, module, EXPORT_NAMED_NUM); ++i) {
        uint32_t at = module + entry(p, module, EXPORT_NAMED_TABLE, i);
        if (!file_name(p, module, rd32(p, at), EXPORT_STRINGS) || !file_tag(p, module, rd32(p, at + EXPORT_TAG_AT), 1))
            return false;
    }
    for (uint32_t t = 0; t < sizeof relocation_tables / sizeof *relocation_tables; ++t) {
        uint32_t table = relocation_tables[t];
        uint32_t first = field(p, module, table), count = field(p, module, table + 1);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t at = module + first + i * RELOCATION_SIZE;
            uint32_t type = rd8(p, at + RELOCATION_KIND_AT);
            if (type != RELOCATION_NOTHING && type != RELOCATION_ABSOLUTE && type != RELOCATION_RELATIVE &&
                type != RELOCATION_ABSOLUTE2)
                return false;
            if (table != STATIC_RELOCATION_TABLE && !file_tag(p, module, rd32(p, at), relocation_width(type)))
                return false;
            if (table == INTERNAL_RELOCATION_TABLE && rd8(p, at + RELOCATION_SYMBOL_AT) >= segments) return false;
        }
        if (count && table != INTERNAL_RELOCATION_TABLE &&
            !rd8(p, module + first + (count - 1) * RELOCATION_SIZE + RELOCATION_END_AT))
            return false;
    }
    for (uint32_t t = 0; t < sizeof import_tables / sizeof *import_tables; ++t) {
        uint32_t table = import_tables[t];
        for (uint32_t i = 0; i < field(p, module, table + 1); ++i) {
            uint32_t at = module + entry(p, module, table, i);
            if (table == IMPORT_NAMED_TABLE && !file_name(p, module, rd32(p, at), IMPORT_STRINGS)) return false;
            if (!file_batch(p, module, rd32(p, at + IMPORT_BATCH_AT), EXTERNAL_RELOCATION_TABLE)) return false;
        }
    }
    for (uint32_t i = 0; i < field(p, module, IMPORT_MODULE_NUM); ++i) {
        uint32_t at = module + entry(p, module, IMPORT_MODULE_TABLE, i);
        if (!file_name(p, module, rd32(p, at), IMPORT_STRINGS)) return false;
        for (uint32_t j = 0; j < 2; ++j) {
            uint32_t table = j ? IMPORT_ANONYMOUS_TABLE : IMPORT_INDEXED_TABLE;
            uint32_t pair = at + IMPORT_MODULE_INDEXED_AT + j * IMPORT_MODULE_PAIR;
            uint32_t offset = rd32(p, pair), count = rd32(p, pair + WORD_BYTES);
            uint32_t first = field(p, module, table), bytes = field(p, module, table + 1) * IMPORT_SIZE;
            if (count && (offset < first || offset - first > bytes || (offset - first) % IMPORT_SIZE ||
                          count > (bytes - (offset - first)) / IMPORT_SIZE))
                return false;
        }
    }
    for (uint32_t i = 0; i < field(p, module, STATIC_ANONYMOUS_NUM); ++i) {
        uint32_t at = module + entry(p, module, STATIC_ANONYMOUS_TABLE, i);
        if (!file_tag(p, module, rd32(p, at), 1) ||
            !file_batch(p, module, rd32(p, at + IMPORT_BATCH_AT), STATIC_RELOCATION_TABLE))
            return false;
    }
    return true;
}

/* ------------------------------------------------------------------ relocations */
static bool metadata_target(SnxCtrProcess* p, uint32_t module, uint32_t buffer, uint32_t size, uint32_t target) {
    uint32_t offset = 0;
    if (target < (uint64_t)module + size && (uint64_t)target + 4 > module) {
        if (target < module) return true;
        offset = target - module;
    } else if (target < (uint64_t)buffer + size && (uint64_t)target + 4 > buffer) {
        if (target < buffer) return true;
        offset = target - buffer;
    } else {
        return false;
    }
    if (offset < CRO_HEADER) return true;
    /* relocations run after rebase_header, so each table field is an address here: less the module, the file offset */
    for (uint32_t f = MODULE_NAME_OFFSET; f < FIX0_BARRIER; f += 2) {
        uint32_t first = field(p, module, f) - module;
        uint64_t bytes = (uint64_t)field(p, module, f + 1) * table_size(f);
        if (bytes && offset < first + bytes && first < (uint64_t)offset + 4) return true;
    }
    return false;
}
static bool relocation_target(SnxCtrProcess* p, uint32_t target) {
    struct SnxCtrCroTransaction* t = p->cro_transaction;
    if (!t || metadata_target(p, t->module, t->buffer, t->size, target)) return false;
    if (p->ldr_crs && metadata_target(p, p->ldr_crs, p->ldr_buffer, p->ldr_crs_size, target)) return false;
    for (size_t i = 0; i < p->ldr_count; ++i) {
        SnxCtrLdrModule* m = &p->ldr_modules[i];
        if (metadata_target(p, m->address, m->buffer, m->size, target)) return false;
    }
    return true;
}
static bool apply_relocation(SnxCtrProcess* p, uint32_t target, uint32_t type, uint32_t addend, uint32_t symbol,
                             uint32_t future) {
    if (type != RELOCATION_NOTHING && !relocation_target(p, target)) return false;
    uint8_t bytes[4];
    switch (type) {
        case RELOCATION_NOTHING: return true;
        case RELOCATION_ABSOLUTE:
        case RELOCATION_ABSOLUTE2:
            snx_st_le32(bytes, symbol + addend);
            return write_guest(p, target, bytes, sizeof bytes);
        case RELOCATION_RELATIVE:
            snx_st_le32(bytes, symbol + addend - future);
            return write_guest(p, target, bytes, sizeof bytes);
        default: return false; /* branch relocation kinds are not implemented */
    }
}
static uint32_t relocation_address(SnxCtrProcess* p, uint32_t module, uint32_t record) {
    uint32_t tag = rd32(p, record), index = tag & 15, offset = tag >> 4;
    uint32_t target = segment_tag_address(p, module, tag);
    if (!target) return 0;
    uint32_t size = rd32(p, entry(p, module, SEGMENT_TABLE, index) + SEGMENT_BYTES_AT);
    uint32_t width = relocation_width(rd8(p, record + RELOCATION_KIND_AT));
    return offset <= size && width <= size - offset ? target : 0;
}
static bool apply_batch(SnxCtrProcess* p, uint32_t module, uint32_t batch, uint32_t end, uint32_t symbol, bool reset) {
    if (!symbol && !reset) return false;
    uint32_t at = batch;
    bool ended = false;
    for (; at < end; at += RELOCATION_SIZE) {
        uint8_t r[RELOCATION_SIZE];
        if (!rdb(p, at, r, sizeof r)) return false;
        uint32_t target = relocation_address(p, module, at);
        if (!target) return false;
        uint32_t future = target;
        struct SnxCtrCroTransaction* t = p->cro_transaction;
        if (t && target >= t->data_image && target - t->data_image < t->data_size)
            future = t->data_target + (target - t->data_image);
        if (!apply_relocation(p, target, r[RELOCATION_KIND_AT], snx_ld_le32(r + RELOCATION_ADDEND_AT), symbol, future))
            return false;
        if (r[RELOCATION_END_AT]) {
            ended = true;
            break;
        }
    }
    if (!ended) return false;
    wr8(p, batch + RELOCATION_STATE_AT, reset ? 0 : 1);
    return true;
}
static bool apply_import_batch(SnxCtrProcess* p, uint32_t module, uint32_t batch, uint32_t symbol, bool reset) {
    uint32_t first = field(p, module, EXTERNAL_RELOCATION_TABLE);
    uint32_t count = field(p, module, EXTERNAL_RELOCATION_NUM);
    if (batch < first || (batch - first) % RELOCATION_SIZE || (batch - first) / RELOCATION_SIZE >= count) return false;
    return apply_batch(p, module, batch, first + count * RELOCATION_SIZE, symbol, reset);
}
/* The exit shim is looked up by a host literal. All other names stay in guest memory. */
static uint32_t find_export(SnxCtrProcess* p, uint32_t module, Name name, const char* literal) {
    uint32_t count = field(p, module, EXPORT_NAMED_NUM);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t at = entry(p, module, EXPORT_NAMED_TABLE, i);
        Name found = table_name(p, module, rd32(p, at), EXPORT_STRINGS);
        if (literal ? name_is(p, found, literal) : same_name(p, found, name))
            return segment_tag_address(p, module, rd32(p, at + EXPORT_TAG_AT));
    }
    return 0;
}
/* the auto-link chain: the static module first, then each registered CRO through its NextCRO field */
static uint32_t next_module(SnxCtrProcess* p, uint32_t module) { return field(p, module, NEXT_CRO); }

/* ------------------------------------------------------------------ rebase */
/* The file offset at `at` becomes an address, which has to lie in [first, end). Zero means none and stays zero. */
static bool rebase_word(SnxCtrProcess* p, uint32_t module, uint32_t at, uint64_t first, uint64_t end) {
    uint32_t value = rd32(p, at);
    if (!value) return true;
    value += module;
    if (value < first || value >= end) return false;
    wr32(p, at, value);
    return true;
}
static bool rebase_header(SnxCtrProcess* p, uint32_t module, uint32_t size) {
    /* a CRO file lays its fields out in this order, so a header that breaks the order is refused */
    static const uint8_t order[] = {CODE_OFFSET,
                                    MODULE_NAME_OFFSET,
                                    SEGMENT_TABLE,
                                    EXPORT_NAMED_TABLE,
                                    EXPORT_TREE_TABLE,
                                    EXPORT_INDEXED_TABLE,
                                    EXPORT_STRINGS,
                                    IMPORT_MODULE_TABLE,
                                    EXTERNAL_RELOCATION_TABLE,
                                    IMPORT_NAMED_TABLE,
                                    IMPORT_INDEXED_TABLE,
                                    IMPORT_ANONYMOUS_TABLE,
                                    IMPORT_STRINGS,
                                    STATIC_ANONYMOUS_TABLE,
                                    INTERNAL_RELOCATION_TABLE,
                                    STATIC_RELOCATION_TABLE,
                                    DATA_OFFSET,
                                    FILE_SIZE};
    if (field(p, module, MAGIC) != CRO_MAGIC) return false;
    if (field(p, module, NEXT_CRO) || field(p, module, PREVIOUS_CRO)) return false;
    if (field(p, module, FILE_SIZE) > CRO_MAX_SIZE || field(p, module, BSS_SIZE) > CRO_MAX_SIZE) return false;
    if (field(p, module, FIXED_SIZE)) return false;
    if (field(p, module, CODE_OFFSET) < CRO_HEADER) return false;
    uint32_t previous = field(p, module, order[0]);
    for (uint32_t i = 1; i < sizeof order / sizeof *order; ++i) {
        uint32_t current = field(p, module, order[i]);
        if (current < previous) return false;
        previous = current;
    }
    if (!rebase_word(p, module, field_address(module, NAME_OFFSET), 0, ADDRESS_SPACE)) return false;
    for (uint32_t f = CODE_OFFSET; f < FIX0_BARRIER; f += 2) {
        if (!rebase_word(p, module, field_address(module, f), 0, ADDRESS_SPACE)) return false;
        uint32_t end = field(p, module, f) + field(p, module, f + 1) * table_size(f);
        if (end > module + size) return false;
    }
    return true;
}
static bool rebase_segments(SnxCtrProcess* p, uint32_t module, uint32_t size, uint32_t data, uint32_t data_size,
                            uint32_t bss, uint32_t bss_size, uint32_t* old_data) {
    uint32_t count = field(p, module, SEGMENT_NUM), previous = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t at = entry(p, module, SEGMENT_TABLE, i);
        uint8_t s[SEGMENT_SIZE];
        if (!rdb(p, at, s, sizeof s)) return false;
        uint32_t offset = snx_ld_le32(s);
        uint32_t bytes = snx_ld_le32(s + SEGMENT_BYTES_AT);
        uint32_t type = snx_ld_le32(s + SEGMENT_TYPE_AT);
        uint32_t new_offset = offset;
        if (type == SEGMENT_DATA && bytes) {
            if (bytes > data_size) return false;
            previous = offset;
            new_offset = data;
        } else if (type == SEGMENT_BSS && bytes) {
            if (bytes > bss_size) return false;
            new_offset = bss;
        } else if (offset) {
            new_offset = offset + module;
            if (new_offset > module + size) return false;
        }
        if (new_offset != offset) {
            snx_st_le32(s, new_offset);
            wrb(p, at, s, sizeof s);
        }
    }
    *old_data = previous + module;
    return true;
}
static bool rebase_export_names(SnxCtrProcess* p, uint32_t module) {
    uint32_t strings = field(p, module, EXPORT_STRINGS), end = strings + field(p, module, EXPORT_STRINGS_SIZE);
    uint32_t count = field(p, module, EXPORT_NAMED_NUM);
    for (uint32_t i = 0; i < count; ++i)
        if (!rebase_word(p, module, entry(p, module, EXPORT_NAMED_TABLE, i), strings, end)) return false;
    return true;
}
static bool verify_export_tree(SnxCtrProcess* p, uint32_t module) {
    uint32_t count = field(p, module, EXPORT_TREE_NUM);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t at = entry(p, module, EXPORT_TREE_TABLE, i);
        uint32_t left = rd16(p, at + TREE_LEFT_AT), right = rd16(p, at + TREE_RIGHT_AT);
        if ((left & TREE_INDEX_MASK) >= count || (right & TREE_INDEX_MASK) >= count) return false;
    }
    return true;
}
static bool rebase_import_modules(SnxCtrProcess* p, uint32_t module) {
    uint32_t strings = field(p, module, IMPORT_STRINGS), end = strings + field(p, module, IMPORT_STRINGS_SIZE);
    uint32_t indexed = field(p, module, IMPORT_INDEXED_TABLE);
    uint32_t indexed_end = indexed + field(p, module, IMPORT_INDEXED_NUM) * IMPORT_SIZE;
    uint32_t anonymous = field(p, module, IMPORT_ANONYMOUS_TABLE);
    uint32_t anonymous_end = anonymous + field(p, module, IMPORT_ANONYMOUS_NUM) * IMPORT_SIZE;
    uint32_t count = field(p, module, IMPORT_MODULE_NUM);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t at = entry(p, module, IMPORT_MODULE_TABLE, i);
        /* an import module with no entries of a kind points at the end of its table: the bound includes that end */
        if (!rebase_word(p, module, at, strings, end) ||
            !rebase_word(p, module, at + IMPORT_MODULE_INDEXED_AT, indexed, indexed_end + 1ull) ||
            !rebase_word(p, module, at + IMPORT_MODULE_ANONYMOUS_AT, anonymous, anonymous_end + 1ull))
            return false;
    }
    return true;
}
static bool rebase_relocations(SnxCtrProcess* p, uint32_t module) {
    uint32_t table = field(p, module, EXTERNAL_RELOCATION_TABLE);
    uint32_t end = table + field(p, module, EXTERNAL_RELOCATION_NUM) * RELOCATION_SIZE;
    uint32_t strings = field(p, module, IMPORT_STRINGS);
    uint32_t strings_end = strings + field(p, module, IMPORT_STRINGS_SIZE);
    for (uint32_t t = 0; t < sizeof import_tables / sizeof *import_tables; ++t) {
        uint32_t count = field(p, module, import_tables[t] + 1);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t at = entry(p, module, import_tables[t], i);
            if (!rd32(p, at + IMPORT_BATCH_AT)) continue;
            /* a batch may begin at the end of its table, as an import module may: the bound includes that end */
            if (!rebase_word(p, module, at + IMPORT_BATCH_AT, table, end + 1ull)) return false;
            if (import_tables[t] == IMPORT_NAMED_TABLE && !rebase_word(p, module, at, strings, strings_end))
                return false;
        }
    }
    return true;
}
static bool reset_external_relocations(SnxCtrProcess* p, uint32_t module) {
    uint32_t count = field(p, module, EXTERNAL_RELOCATION_NUM);
    if (!count) return true;
    uint32_t unresolved = segment_tag_address(p, module, field(p, module, ON_UNRESOLVED_TAG));
    uint8_t last[RELOCATION_SIZE];
    if (!rdb(p, entry(p, module, EXTERNAL_RELOCATION_TABLE, count - 1), last, sizeof last) || !last[RELOCATION_END_AT])
        return false;
    bool begin = true;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t at = entry(p, module, EXTERNAL_RELOCATION_TABLE, i);
        uint8_t r[RELOCATION_SIZE];
        if (!rdb(p, at, r, sizeof r)) return false;
        uint32_t target = relocation_address(p, module, at);
        if (!target) return false;
        if (!apply_relocation(p, target, r[RELOCATION_KIND_AT], snx_ld_le32(r + RELOCATION_ADDEND_AT), unresolved,
                              target))
            return false;
        if (begin) wr8(p, at + RELOCATION_STATE_AT, 0);
        begin = r[RELOCATION_END_AT] != 0;
    }
    return true;
}
static bool apply_internal_relocations(SnxCtrProcess* p, uint32_t module, uint32_t old_data) {
    uint32_t segments = field(p, module, SEGMENT_NUM);
    uint32_t count = field(p, module, INTERNAL_RELOCATION_NUM);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t at = entry(p, module, INTERNAL_RELOCATION_TABLE, i);
        uint8_t r[RELOCATION_SIZE];
        if (!rdb(p, at, r, sizeof r)) return false;
        uint32_t tag = snx_ld_le32(r);
        uint32_t target = segment_tag_address(p, module, tag);
        if (!target) return false;
        uint8_t s[SEGMENT_SIZE];
        if (!rdb(p, entry(p, module, SEGMENT_TABLE, tag & 15), s, sizeof s)) return false;
        uint32_t type = snx_ld_le32(s + SEGMENT_TYPE_AT);
        uint32_t write_at = target;
        /* CRS segments are already absolute; CRO .data is still in the image until the load transfers it. */
        if (type == SEGMENT_DATA && old_data != NO_DATA_IMAGE) write_at = old_data + (tag >> 4);
        if (r[RELOCATION_SYMBOL_AT] >= segments) return false;
        uint8_t sym[SEGMENT_SIZE];
        if (!rdb(p, entry(p, module, SEGMENT_TABLE, r[RELOCATION_SYMBOL_AT]), sym, sizeof sym)) return false;
        uint32_t symbol = snx_ld_le32(sym);
        uint32_t addend = snx_ld_le32(r + RELOCATION_ADDEND_AT);
        if (!apply_relocation(p, write_at, r[RELOCATION_KIND_AT], addend, symbol, target)) return false;
    }
    return true;
}
static bool apply_static_anonymous(SnxCtrProcess* p, uint32_t module, uint32_t crs) {
    uint32_t table = field(p, module, STATIC_RELOCATION_TABLE);
    uint32_t end = table + field(p, module, STATIC_RELOCATION_NUM) * RELOCATION_SIZE;
    uint32_t count = field(p, module, STATIC_ANONYMOUS_NUM);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t at = entry(p, module, STATIC_ANONYMOUS_TABLE, i);
        uint32_t batch = rd32(p, at + IMPORT_BATCH_AT) + module;
        if (batch < table || batch >= end || (batch - table) % RELOCATION_SIZE) return false;
        uint32_t symbol = segment_tag_address(p, module, rd32(p, at));
        if (!apply_batch(p, crs, batch, end, symbol, false)) return false;
    }
    return true;
}
static bool apply_exit_relocations(SnxCtrProcess* p, uint32_t module, uint32_t crs) {
    uint32_t count = field(p, module, IMPORT_NAMED_NUM);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t at = entry(p, module, IMPORT_NAMED_TABLE, i);
        uint32_t batch = rd32(p, at + IMPORT_BATCH_AT);
        Name name = table_name(p, module, rd32(p, at), IMPORT_STRINGS);
        if (!name.size) return false;
        if (!name_is(p, name, "__aeabi_atexit")) continue;
        for (uint32_t m = crs; m; m = next_module(p, m)) {
            uint32_t symbol = find_export(p, m, (Name){0}, "nnroAeabiAtexit_");
            if (symbol) return apply_import_batch(p, module, batch, symbol, false);
        }
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ link */
static bool apply_import_named(SnxCtrProcess* p, uint32_t module, uint32_t crs) {
    uint32_t count = field(p, module, IMPORT_NAMED_NUM);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t at = entry(p, module, IMPORT_NAMED_TABLE, i);
        uint32_t batch = rd32(p, at + IMPORT_BATCH_AT);
        if (rd8(p, batch + RELOCATION_STATE_AT)) continue; /* already resolved */
        Name name = table_name(p, module, rd32(p, at), IMPORT_STRINGS);
        if (!name.size) return false;
        for (uint32_t m = crs; m; m = next_module(p, m)) {
            uint32_t symbol = find_export(p, m, name, NULL);
            if (!symbol) continue;
            if (!apply_import_batch(p, module, batch, symbol, false)) return false;
            break;
        }
    }
    return true;
}
/* Resolve the batches that the import module entry at `at` in importer takes from the exports of exporter */
static bool import_from(SnxCtrProcess* p, uint32_t importer, uint32_t at, uint32_t exporter) {
    uint32_t indexed_table = rd32(p, at + IMPORT_MODULE_INDEXED_AT),
             indexed_num = rd32(p, at + IMPORT_MODULE_INDEXED_AT + WORD_BYTES);
    uint32_t anonymous_table = rd32(p, at + IMPORT_MODULE_ANONYMOUS_AT),
             anonymous_num = rd32(p, at + IMPORT_MODULE_ANONYMOUS_AT + WORD_BYTES);
    for (uint32_t j = 0; j < indexed_num; ++j) {
        uint32_t im = indexed_table + j * IMPORT_SIZE;
        uint32_t index = rd32(p, im);
        if (index >= field(p, exporter, EXPORT_INDEXED_NUM)) return false;
        uint32_t tag = rd32(p, entry(p, exporter, EXPORT_INDEXED_TABLE, index));
        if (!apply_import_batch(p, importer, rd32(p, im + IMPORT_BATCH_AT), segment_tag_address(p, exporter, tag),
                                false))
            return false;
    }
    for (uint32_t j = 0; j < anonymous_num; ++j) {
        uint32_t im = anonymous_table + j * IMPORT_SIZE;
        uint32_t symbol = segment_tag_address(p, exporter, rd32(p, im));
        if (!apply_import_batch(p, importer, rd32(p, im + IMPORT_BATCH_AT), symbol, false)) return false;
    }
    return true;
}
static bool apply_module_import(SnxCtrProcess* p, uint32_t module, uint32_t crs) {
    uint32_t count = field(p, module, IMPORT_MODULE_NUM);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t at = entry(p, module, IMPORT_MODULE_TABLE, i);
        Name want = table_name(p, module, rd32(p, at), IMPORT_STRINGS);
        if (!want.size) return false;
        for (uint32_t m = crs; m; m = next_module(p, m)) {
            if (!same_name(p, want, module_name(p, m))) continue;
            if (!import_from(p, module, at, m)) return false;
            break;
        }
    }
    return true;
}
static bool reset_export_named(SnxCtrProcess* p, uint32_t module, uint32_t target) {
    uint32_t unresolved = segment_tag_address(p, target, field(p, target, ON_UNRESOLVED_TAG));
    uint32_t count = field(p, target, IMPORT_NAMED_NUM);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t at = entry(p, target, IMPORT_NAMED_TABLE, i);
        uint32_t batch = rd32(p, at + IMPORT_BATCH_AT);
        if (!rd8(p, batch + RELOCATION_STATE_AT)) continue;
        Name name = table_name(p, target, rd32(p, at), IMPORT_STRINGS);
        if (!name.size) return false;
        if (find_export(p, module, name, NULL) && !apply_import_batch(p, target, batch, unresolved, true)) return false;
    }
    return true;
}
static bool reset_module_export(SnxCtrProcess* p, uint32_t module, uint32_t target) {
    Name name = module_name(p, module);
    if (!name.size) return false;
    uint32_t count = field(p, target, IMPORT_MODULE_NUM);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t at = entry(p, target, IMPORT_MODULE_TABLE, i);
        Name want = table_name(p, target, rd32(p, at), IMPORT_STRINGS);
        if (!want.size) return false;
        if (!same_name(p, want, name)) continue;
        uint32_t unresolved = segment_tag_address(p, target, field(p, target, ON_UNRESOLVED_TAG));
        for (uint32_t j = 0; j < 2; ++j) {
            uint32_t pair = at + IMPORT_MODULE_INDEXED_AT + j * IMPORT_MODULE_PAIR;
            uint32_t first = rd32(p, pair), num = rd32(p, pair + WORD_BYTES);
            for (uint32_t k = 0; k < num; ++k) {
                uint32_t batch = rd32(p, first + k * IMPORT_SIZE + IMPORT_BATCH_AT);
                if (!apply_import_batch(p, target, batch, unresolved, true)) return false;
            }
        }
    }
    return true;
}
static bool apply_export_named(SnxCtrProcess* p, uint32_t module, uint32_t target) {
    uint32_t count = field(p, target, IMPORT_NAMED_NUM);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t at = entry(p, target, IMPORT_NAMED_TABLE, i);
        uint32_t batch = rd32(p, at + IMPORT_BATCH_AT);
        if (rd8(p, batch + RELOCATION_STATE_AT)) continue;
        Name name = table_name(p, target, rd32(p, at), IMPORT_STRINGS);
        if (!name.size) return false;
        uint32_t symbol = find_export(p, module, name, NULL);
        if (symbol && !apply_import_batch(p, target, batch, symbol, false)) return false;
    }
    return true;
}
static bool apply_module_export(SnxCtrProcess* p, uint32_t module, uint32_t target) {
    Name name = module_name(p, module);
    if (!name.size) return false;
    uint32_t count = field(p, target, IMPORT_MODULE_NUM);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t at = entry(p, target, IMPORT_MODULE_TABLE, i);
        Name want = table_name(p, target, rd32(p, at), IMPORT_STRINGS);
        if (!want.size) return false;
        if (same_name(p, want, name) && !import_from(p, target, at, module)) return false;
    }
    return true;
}
static bool apply_exports(SnxCtrProcess* p, uint32_t module, uint32_t crs) {
    for (uint32_t m = crs; m; m = next_module(p, m))
        if (!apply_export_named(p, module, m) || !apply_module_export(p, module, m)) return false;
    return true;
}

/* ------------------------------------------------------------------ public */
static bool rebase_start(SnxCtrProcess* p, uint32_t module, uint32_t size) {
    return rebase_header(p, module, size) && module_name(p, module).size &&
           verify_string_table(p, field(p, module, EXPORT_STRINGS), field(p, module, EXPORT_STRINGS_SIZE));
}
static bool rebase_tables(SnxCtrProcess* p, uint32_t module) {
    return rebase_export_names(p, module) && verify_export_tree(p, module) && rebase_import_modules(p, module) &&
           reset_external_relocations(p, module) && rebase_relocations(p, module) &&
           verify_string_table(p, field(p, module, IMPORT_STRINGS), field(p, module, IMPORT_STRINGS_SIZE));
}
bool snx_ctr_cro_rebase(SnxCtrProcess* p, uint32_t module, uint32_t size, uint32_t data, uint32_t data_size,
                        uint32_t bss, uint32_t bss_size, uint32_t crs) {
    uint32_t old_data = 0;
    if (!rebase_start(p, module, size) ||
        !rebase_segments(p, module, size, data, data_size, bss, bss_size, &old_data) || !rebase_tables(p, module))
        return false;
    if (!apply_static_anonymous(p, module, crs)) return false;
    return apply_internal_relocations(p, module, old_data);
}
bool snx_ctr_cro_rebase_crs(SnxCtrProcess* p, uint32_t module, uint32_t size) {
    if (!rebase_start(p, module, size) || !rebase_tables(p, module)) return false;
    return apply_internal_relocations(p, module, NO_DATA_IMAGE);
}
bool snx_ctr_cro_link(SnxCtrProcess* p, uint32_t module, uint32_t crs) {
    return apply_import_named(p, module, crs) && apply_module_import(p, module, crs) && apply_exports(p, module, crs) &&
           apply_exit_relocations(p, module, crs);
}
bool snx_ctr_cro_link_load(SnxCtrProcess* p, uint32_t module, uint32_t crs, bool new_command) {
    assert(p->cro_transaction);
    uint32_t data_entry = 0, data_address = 0;
    if (new_command) {
        for (uint32_t i = 0; i < field(p, module, SEGMENT_NUM); ++i) {
            uint32_t at = entry(p, module, SEGMENT_TABLE, i);
            if (rd32(p, at + SEGMENT_TYPE_AT) == SEGMENT_DATA) {
                data_entry = at;
                data_address = rd32(p, at);
                p->cro_transaction->data_image = field(p, module, DATA_OFFSET);
                p->cro_transaction->data_target = data_address;
                p->cro_transaction->data_size = rd32(p, at + SEGMENT_BYTES_AT);
                wr32(p, at, field(p, module, DATA_OFFSET));
                break;
            }
        }
    }
    bool result = apply_import_named(p, module, crs) && apply_module_import(p, module, crs) &&
                  apply_exit_relocations(p, module, crs);
    if (data_entry) wr32(p, data_entry, data_address);
    p->cro_transaction->data_size = 0;
    return result && apply_exports(p, module, crs);
}
bool snx_ctr_cro_unlink(SnxCtrProcess* p, uint32_t module, uint32_t crs) {
    uint32_t unresolved = segment_tag_address(p, module, field(p, module, ON_UNRESOLVED_TAG));
    for (uint32_t t = 0; t < sizeof import_tables / sizeof *import_tables; ++t) {
        uint32_t count = field(p, module, import_tables[t] + 1);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t batch = rd32(p, entry(p, module, import_tables[t], i) + IMPORT_BATCH_AT);
            if (!apply_import_batch(p, module, batch, unresolved, true)) return false;
        }
    }
    for (uint32_t m = crs; m; m = next_module(p, m))
        if (!reset_export_named(p, module, m) || !reset_module_export(p, module, m)) return false;
    return true;
}
void snx_ctr_cro_register(SnxCtrProcess* p, uint32_t module, uint32_t crs, bool auto_link) {
    uint32_t head = auto_link ? field(p, crs, NEXT_CRO) : field(p, crs, PREVIOUS_CRO);
    if (head) {
        uint32_t tail = field(p, head, PREVIOUS_CRO);
        set_field(p, module, PREVIOUS_CRO, tail);
        set_field(p, tail, NEXT_CRO, module);
        set_field(p, head, PREVIOUS_CRO, module);
    } else {
        set_field(p, module, PREVIOUS_CRO, module);
        if (auto_link)
            set_field(p, crs, NEXT_CRO, module);
        else
            set_field(p, crs, PREVIOUS_CRO, module);
    }
    set_field(p, module, NEXT_CRO, 0);
}
void snx_ctr_cro_unregister(SnxCtrProcess* p, uint32_t module, uint32_t crs) {
    uint32_t next_head = field(p, crs, NEXT_CRO), previous_head = field(p, crs, PREVIOUS_CRO);
    uint32_t next = field(p, module, NEXT_CRO), previous = field(p, module, PREVIOUS_CRO);
    if (module == next_head || module == previous_head) {
        if (next) set_field(p, next, PREVIOUS_CRO, previous);
        if (module == previous_head)
            set_field(p, crs, PREVIOUS_CRO, next);
        else
            set_field(p, crs, NEXT_CRO, next);
    } else if (next) {
        set_field(p, previous, NEXT_CRO, next);
        set_field(p, next, PREVIOUS_CRO, previous);
    } else {
        set_field(p, previous, NEXT_CRO, 0);
        if (next_head && field(p, next_head, PREVIOUS_CRO) == module)
            set_field(p, next_head, PREVIOUS_CRO, previous);
        else if (previous_head && field(p, previous_head, PREVIOUS_CRO) == module)
            set_field(p, previous_head, PREVIOUS_CRO, previous);
    }
    set_field(p, module, NEXT_CRO, 0);
    set_field(p, module, PREVIOUS_CRO, 0);
}
/* ------------------------------------------------------------------ unload */
/* A module fixed at level zero keeps every table live after the unload, so the words the load's relocations wrote
   return to zero and their batches to the unresolved state (Azahar's ClearRelocations). */
static bool clear_relocations(SnxCtrProcess* p, uint32_t module) {
    uint32_t count = field(p, module, EXTERNAL_RELOCATION_NUM);
    bool begin = true;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t at = entry(p, module, EXTERNAL_RELOCATION_TABLE, i);
        uint8_t r[RELOCATION_SIZE];
        if (!rdb(p, at, r, sizeof r)) return false;
        uint32_t target = relocation_address(p, module, at);
        if (!target) return false;
        if (r[RELOCATION_KIND_AT]) wr32(p, target, 0);
        if (begin) wr8(p, at + RELOCATION_STATE_AT, 0);
        begin = r[RELOCATION_END_AT] != 0;
    }
    count = field(p, module, INTERNAL_RELOCATION_NUM);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t at = entry(p, module, INTERNAL_RELOCATION_TABLE, i);
        uint8_t r[RELOCATION_SIZE];
        if (!rdb(p, at, r, sizeof r)) return false;
        uint32_t target = segment_tag_address(p, module, rd32(p, at));
        if (!target) return false;
        if (r[RELOCATION_KIND_AT]) wr32(p, target, 0);
    }
    return true;
}
/* one word of the image, back from the absolute address the load wrote to its file offset */
static void unrebase_word(SnxCtrProcess* p, uint32_t at, uint32_t module) {
    uint32_t value = rd32(p, at);
    if (value) wr32(p, at, value - module);
}
static bool unrebase_segments(SnxCtrProcess* p, uint32_t module) {
    for (uint32_t i = 0, n = field(p, module, SEGMENT_NUM); i < n; ++i) {
        uint32_t at = entry(p, module, SEGMENT_TABLE, i);
        uint8_t s[SEGMENT_SIZE];
        if (!rdb(p, at, s, sizeof s)) return false;
        uint32_t offset = snx_ld_le32(s);
        uint32_t bytes = snx_ld_le32(s + SEGMENT_BYTES_AT);
        uint32_t type = snx_ld_le32(s + SEGMENT_TYPE_AT);
        uint32_t file_offset;
        if (type == SEGMENT_BSS) {
            file_offset = 0;
        } else if (type == SEGMENT_DATA && bytes) {
            /* rebase_segments replaced the entry with the private data address; DATA_OFFSET, which unrebase
               restores below, still carries the file offset the load added module to. */
            file_offset = field(p, module, DATA_OFFSET) - module;
        } else {
            file_offset = offset ? offset - module : offset;
        }
        if (file_offset != offset) {
            snx_st_le32(s, file_offset);
            wrb(p, at, s, sizeof s);
        }
    }
    return true;
}
bool snx_ctr_cro_unrebase(SnxCtrProcess* p, uint32_t module, bool crs) {
    if (!crs && field(p, module, MAGIC) != CRO_FIXD && !clear_relocations(p, module)) return false;
    for (uint32_t i = 0, n = field(p, module, EXPORT_NAMED_NUM); i < n; ++i)
        unrebase_word(p, entry(p, module, EXPORT_NAMED_TABLE, i), module);
    for (uint32_t i = 0, n = field(p, module, IMPORT_MODULE_NUM); i < n; ++i) {
        uint32_t at = entry(p, module, IMPORT_MODULE_TABLE, i);
        unrebase_word(p, at, module);
        unrebase_word(p, at + IMPORT_MODULE_INDEXED_AT, module);
        unrebase_word(p, at + IMPORT_MODULE_ANONYMOUS_AT, module);
    }
    for (uint32_t i = 0, n = field(p, module, IMPORT_NAMED_NUM); i < n; ++i) {
        uint32_t at = entry(p, module, IMPORT_NAMED_TABLE, i);
        unrebase_word(p, at, module);
        unrebase_word(p, at + IMPORT_BATCH_AT, module);
    }
    for (uint32_t i = 0, n = field(p, module, IMPORT_INDEXED_NUM); i < n; ++i)
        unrebase_word(p, entry(p, module, IMPORT_INDEXED_TABLE, i) + IMPORT_BATCH_AT, module);
    for (uint32_t i = 0, n = field(p, module, IMPORT_ANONYMOUS_NUM); i < n; ++i)
        unrebase_word(p, entry(p, module, IMPORT_ANONYMOUS_TABLE, i) + IMPORT_BATCH_AT, module);
    if (!crs && !unrebase_segments(p, module)) return false;
    set_field(p, module, NEXT_CRO, 0);
    set_field(p, module, PREVIOUS_CRO, 0);
    set_field(p, module, FIXED_SIZE, 0);
    unrebase_word(p, field_address(module, NAME_OFFSET), module);
    for (uint32_t f = CODE_OFFSET; f < FIX0_BARRIER; f += 2) unrebase_word(p, field_address(module, f), module);
    return true;
}

static uint32_t fix_end(SnxCtrProcess* p, uint32_t module, uint32_t level) {
    uint32_t end = CRO_HEADER;
    uint32_t code = field(p, module, CODE_OFFSET) + field(p, module, CODE_SIZE);
    if (code > end) end = code;
    for (uint32_t f = MODULE_NAME_OFFSET; f < fix_barrier[level]; f += 2) {
        uint32_t at = field(p, module, f) + field(p, module, f + 1) * table_size(f);
        if (at > end) end = at;
    }
    return end;
}
uint32_t snx_ctr_cro_fix(SnxCtrProcess* p, uint32_t module, uint32_t level) {
    if (level > 3) return 0;
    uint32_t end = fix_end(p, module, level);
    uint64_t retained_end = snx_align_up(end, SNX_CTR_PAGE_SIZE);
    for (uint32_t i = 0; i < field(p, module, SEGMENT_NUM); ++i) {
        uint32_t at = entry(p, module, SEGMENT_TABLE, i), type = rd32(p, at + SEGMENT_TYPE_AT);
        uint32_t base = rd32(p, at), size = rd32(p, at + SEGMENT_BYTES_AT);
        if (size && (type == SEGMENT_CODE || type == SEGMENT_RODATA) &&
            (base < module || (uint64_t)base + size > retained_end))
            return 0;
    }
    if (level) {
        set_field(p, module, MAGIC, CRO_FIXD);
        for (uint32_t f = fix_barrier[level]; f < FIX0_BARRIER; f += 2) {
            set_field(p, module, f, end);
            set_field(p, module, f + 1, 0);
        }
    }
    uint32_t size = (uint32_t)retained_end - module;
    set_field(p, module, FIXED_SIZE, size);
    return size;
}
void snx_ctr_cro_data(SnxCtrProcess* p, uint32_t module, uint32_t* offset, uint32_t* size) {
    *offset = field(p, module, DATA_OFFSET);
    *size = field(p, module, DATA_SIZE);
}
bool snx_ctr_cro_code(SnxCtrProcess* p, uint32_t module, uint32_t* begin, uint32_t* size) {
    uint32_t count = field(p, module, SEGMENT_NUM);
    *begin = 0;
    *size = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t s[SEGMENT_SIZE];
        if (!rdb(p, entry(p, module, SEGMENT_TABLE, i), s, sizeof s)) return false;
        uint32_t type = snx_ld_le32(s + SEGMENT_TYPE_AT);
        uint32_t bytes = snx_ld_le32(s + SEGMENT_BYTES_AT);
        if (type != SEGMENT_CODE || !bytes) continue;
        uint32_t offset = snx_ld_le32(s);
        uint64_t first = snx_align_down(offset, SNX_CTR_PAGE_SIZE);
        uint64_t last = snx_align_up((uint64_t)offset + bytes, SNX_CTR_PAGE_SIZE);
        if (last > ADDRESS_SPACE || last - first > UINT32_MAX) return false;
        *begin = (uint32_t)first;
        *size = (uint32_t)(last - first);
        return true;
    }
    return true;
}
