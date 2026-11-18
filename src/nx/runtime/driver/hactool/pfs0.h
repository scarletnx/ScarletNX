#ifndef HACTOOL_PFS0_H
#define HACTOOL_PFS0_H

#include "types.h"
#include "utils.h"
#include "npdm.h"

#define MAGIC_PFS0 0x30534650

typedef struct {
    uint32_t magic;
    uint32_t num_files;
    uint32_t string_table_size;
    uint32_t reserved;
} pfs0_header_t;

typedef struct {
    uint64_t offset;
    uint64_t size;
    uint32_t string_table_offset;
    uint32_t reserved;
} pfs0_file_entry_t;

typedef struct {
    uint8_t master_hash[0x20]; /* SHA-256 hash of the hash table. */
    uint32_t block_size; /* In bytes. */
    uint32_t always_2;
    uint64_t hash_table_offset; /* Normally zero. */
    uint64_t hash_table_size;
    uint64_t pfs0_offset;
    uint64_t pfs0_size;
    uint8_t _0x48[0xF0];
} pfs0_superblock_t;

typedef struct {
    pfs0_superblock_t *superblock;
    FILE *file;
    hactool_ctx_t *tool_ctx;
    validity_t superblock_hash_validity;
    validity_t hash_table_validity;
    int is_exefs;
    npdm_t *npdm;
    uint64_t npdm_size; /* ScarletNX: the bytes npdm holds, which its offsets must stay in */
    pfs0_header_t *header;
} pfs0_ctx_t;

static inline pfs0_file_entry_t *pfs0_get_file_entry(pfs0_header_t *hdr, uint32_t i) {
    if (i >= hdr->num_files) return NULL;
    return (pfs0_file_entry_t *)((char *)(hdr) + sizeof(*hdr) + i * sizeof(pfs0_file_entry_t));
}

static inline char *pfs0_get_string_table(pfs0_header_t *hdr) {
    return (char *)(hdr) + sizeof(*hdr) + hdr->num_files * sizeof(pfs0_file_entry_t);
}

static inline uint64_t pfs0_get_header_size(pfs0_header_t *hdr) {
    return sizeof(*hdr) + hdr->num_files * sizeof(pfs0_file_entry_t) + hdr->string_table_size;
}

static inline char *pfs0_get_file_name(pfs0_header_t *hdr, uint32_t i) {
    return pfs0_get_string_table(hdr) + pfs0_get_file_entry(hdr, i)->string_table_offset;
}

/* ScarletNX: a header read from its little-endian file put in the host's order, and with entries its file entries too,
   which follow it as num_files counts them (in the host's order by then) */
static inline void pfs0_header_from_le(pfs0_header_t *hdr, int entries) {
    snx_le32_array(hdr, sizeof(*hdr) / 4);
    for (uint32_t i = 0; SNX_HOST_BE && entries && i < hdr->num_files; i++) {
        pfs0_file_entry_t *entry = pfs0_get_file_entry(hdr, i);
        snx_le64_array(entry, 2);
        snx_le32_array(&entry->string_table_offset, 2);
    }
}

void pfs0_process(pfs0_ctx_t *ctx);
void pfs0_save(pfs0_ctx_t *ctx);
void pfs0_print(pfs0_ctx_t *ctx);

#endif
