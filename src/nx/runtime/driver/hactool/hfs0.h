#ifndef HACTOOL_hfs0_H
#define HACTOOL_hfs0_H

#include <string.h>
#include "types.h"
#include "utils.h"
#include "settings.h"

#define MAGIC_HFS0 0x30534648

typedef struct {
    uint32_t magic;
    uint32_t num_files;
    uint32_t string_table_size;
    uint32_t reserved;
} hfs0_header_t;

typedef struct {
    uint64_t offset;
    uint64_t size;
    uint32_t string_table_offset;
    uint32_t hashed_size;
    uint64_t reserved;
    unsigned char hash[0x20];
} hfs0_file_entry_t;

typedef struct {
    FILE *file;
    uint64_t offset;
    uint64_t size;
    hactool_ctx_t *tool_ctx;
    hfs0_header_t *header;
    const char *name;
    const uint8_t *hash_suffix;
} hfs0_ctx_t;

static inline hfs0_file_entry_t *hfs0_get_file_entry(hfs0_header_t *hdr, uint32_t i) {
    if (i >= hdr->num_files) return NULL;
    return (hfs0_file_entry_t *)((char *)(hdr) + sizeof(*hdr) + i * sizeof(hfs0_file_entry_t));
}

static inline char *hfs0_get_string_table(hfs0_header_t *hdr) {
    return (char *)(hdr) + sizeof(*hdr) + hdr->num_files * sizeof(hfs0_file_entry_t);
}

static inline uint64_t hfs0_get_header_size(hfs0_header_t *hdr) {
    return sizeof(*hdr) + hdr->num_files * sizeof(hfs0_file_entry_t) + hdr->string_table_size;
}

static inline char *hfs0_get_file_name(hfs0_header_t *hdr, uint32_t i) {
    return hfs0_get_string_table(hdr) + hfs0_get_file_entry(hdr, i)->string_table_offset;
}

/* ScarletNX: the first file whose name does not start in the string table or has no end in it, num_files when every
   name is whole. The offsets are the dump's words, and a name is read to its end wherever it starts: a header is
   checked once, as it is loaded (in the host's order), and its names are read freely after */
static inline uint32_t hfs0_first_bad_name(hfs0_header_t *hdr) {
    for (uint32_t i = 0; i < hdr->num_files; i++) {
        uint32_t at = hfs0_get_file_entry(hdr, i)->string_table_offset;
        if (at >= hdr->string_table_size ||
            !memchr(hfs0_get_string_table(hdr) + at, 0, hdr->string_table_size - at)) return i;
    }
    return hdr->num_files;
}

/* ScarletNX: a header read from its little-endian file put in the host's order, and with entries its file entries too,
   which follow it as num_files counts them (in the host's order by then) */
static inline void hfs0_header_from_le(hfs0_header_t *hdr, int entries) {
    snx_le32_array(hdr, sizeof(*hdr) / 4);
    for (uint32_t i = 0; SNX_HOST_BE && entries && i < hdr->num_files; i++) {
        hfs0_file_entry_t *entry = hfs0_get_file_entry(hdr, i);
        snx_le64_array(entry, 2);
        snx_le32_array(&entry->string_table_offset, 2);
        entry->reserved = snx_le64(entry->reserved);
    }
}

void hfs0_process(hfs0_ctx_t *ctx);
void hfs0_save(hfs0_ctx_t *ctx);
void hfs0_print(hfs0_ctx_t *ctx);

void hfs0_save_file(hfs0_ctx_t *ctx, uint32_t i, filepath_t *dirpath);

#endif
