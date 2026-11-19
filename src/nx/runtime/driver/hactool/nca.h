#ifndef HACTOOL_NCA_H
#define HACTOOL_NCA_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "settings.h"
#include "aes.h"
#include "npdm.h"
#include "pfs0.h"
#include "ivfc.h"
#include "nca0_romfs.h"
#include "bktr.h"
#include "storage.h"

#define MAGIC_NCA3 0x3341434E /* "NCA3" */
#define MAGIC_NCA2 0x3241434E /* "NCA2" */
#define MAGIC_NCA0 0x3041434E /* "NCA0" */

typedef struct {
    uint32_t media_start_offset;
    uint32_t media_end_offset;
    uint8_t _0x8[0x8]; /* Padding. */
} nca_section_entry_t;

typedef struct {
    ivfc_hdr_t ivfc_header;
    uint8_t _0xE0[0x18];
    bktr_header_t relocation_header;
    bktr_header_t subsection_header;
} bktr_superblock_t;

typedef struct {
    bktr_superblock_t *superblock;
    FILE *file;
    validity_t superblock_hash_validity;
    storage_t *indirect; /* The patched section: base and patch through the relocation table, once it is read. */
    ivfc_level_ctx_t ivfc_levels[IVFC_MAX_LEVEL];
    uint64_t romfs_offset;
    romfs_hdr_t header;
    romfs_direntry_t *directories;
    romfs_fentry_t *files;
} bktr_section_ctx_t;

typedef enum {
    PARTITION_ROMFS = 0,
    PARTITION_PFS0 = 1
} section_partition_type_t;

typedef enum {
    FS_TYPE_PFS0 = 2,
    FS_TYPE_ROMFS = 3
} section_fs_type_t;

typedef enum {
    CRYPT_NONE = 1,
    CRYPT_XTS = 2,
    CRYPT_CTR = 3,
    CRYPT_BKTR = 4,
    /* Hash layers in plaintext: LibHac names these but builds no storage for them. */
    CRYPT_CTR_SKIP_LAYER_HASH = 5,
    CRYPT_BKTR_SKIP_LAYER_HASH = 6,
    CRYPT_NCA0 = MAGIC_NCA0
} section_crypt_type_t;

/* The FS header's tables past the counter, as LibHac's NcaFsHeader lays them out. */
typedef struct {
    uint64_t meta_offset;
    uint64_t meta_size;
    bucket_tree_header_t meta_header;
    uint64_t physical_offset;
    uint16_t generation; /* Nonzero for a sparse section. */
    uint8_t _0x2A[0x6];
} nca_sparse_info_t;

typedef struct {
    uint64_t table_offset; /* From the start of the hashed data. */
    uint64_t table_size;
    bucket_tree_header_t table_header;
    uint64_t _0x20;
} nca_compression_info_t;

typedef struct {
    uint64_t offset;
    uint64_t size;
    uint8_t hash[0x20];
} nca_metadata_hash_info_t;

/* NCA FS header. */
typedef struct {
    uint8_t _0x0;
    uint8_t _0x1;
    uint8_t partition_type;
    uint8_t fs_type;
    uint8_t crypt_type;
    uint8_t _0x5[0x3];
    union { /* FS-specific superblock. Size = 0x138. */
        pfs0_superblock_t pfs0_superblock;
        romfs_superblock_t romfs_superblock;
        nca0_romfs_superblock_t nca0_romfs_superblock;
        bktr_superblock_t bktr_superblock;
    };
    union {
        uint8_t section_ctr[0x8];
        struct {
            uint32_t section_ctr_low;
            uint32_t section_ctr_high;
        };
    };
    union {
        uint8_t _0x148[0xB8];
        struct {
            nca_sparse_info_t sparse_info;
            nca_compression_info_t compression_info;
            nca_metadata_hash_info_t metadata_hash_info;
            uint8_t _0x1D0[0x30]; /* Padding. */
        };
    };
} nca_fs_header_t;

/* Content archive (NCA) header. ScarletNX: nca_decrypt_header leaves its words, and its FS headers', in the host's
   order. */
typedef struct {
    uint8_t fixed_key_sig[0x100]; /* RSA-PSS signature over header with fixed key. */
    uint8_t npdm_key_sig[0x100]; /* RSA-PSS signature over header with key in NPDM. */
    uint32_t magic;
    uint8_t distribution; /* System vs gamecard. */
    uint8_t content_type;
    uint8_t crypto_type; /* Which keyblob (field 1) */
    uint8_t kaek_ind; /* Which kaek index? */
    uint64_t nca_size; /* Entire archive size. */
    uint64_t title_id;
    uint8_t _0x218[0x4]; /* Padding. */
    uint32_t sdk_version; /* What SDK was this built with? Major, minor, micro, revision, from the top byte down. */
    uint8_t crypto_type2; /* Which keyblob (field 2) */
    uint8_t fixed_key_generation;
    uint8_t _0x222[0xE]; /* Padding. */
    uint8_t rights_id[0x10]; /* Rights ID (for titlekey crypto). */
    nca_section_entry_t section_entries[4]; /* Section entry metadata. */
    uint8_t section_hashes[4][0x20]; /* SHA-256 hashes for each section header. */
    uint8_t encrypted_keys[4][0x10]; /* Encrypted key area. */
    uint8_t _0x340[0xC0]; /* Padding. */
    nca_fs_header_t fs_headers[4]; /* FS section headers. */
} nca_header_t;

enum nca_section_type {
    PFS0,
    ROMFS,
    BKTR,
    NCA0_ROMFS,
    INVALID
};

enum nca_version {
    NCAVERSION_UNKNOWN = 0,
    NCAVERSION_NCA0_BETA,
    NCAVERSION_NCA0,
    /* NCAVERSION_NCA1, // Does this exist? */
    NCAVERSION_NCA2,
    NCAVERSION_NCA3
};

enum nca_content_type {
    NCACONTENTTYPE_PROGRAM    = 0,
    NCACONTENTTYPE_META       = 1,
    NCACONTENTTYPE_CONTROL    = 2,
    NCACONTENTTYPE_MANUAL     = 3,
    NCACONTENTTYPE_DATA       = 4,
    NCACONTENTTPYE_PUBLICDATA = 5,
};

typedef struct {
    int is_present;
    enum nca_section_type type;
    FILE *file; /* Pointer to file. */
    uint64_t base_offset; /* ScarletNX: where the NCA starts in file, as the NCA's context has it. */
    const unsigned char *map; /* ScarletNX: file mapped into memory, as the NCA's context has it. */
    uint64_t map_size;
    uint64_t offset;
    uint64_t size;
    uint32_t section_num;
    nca_fs_header_t *header;
    int is_decrypted;
    uint64_t sector_size;
    uint64_t sector_mask;
    aes_ctx_t *aes; /* AES context for the section. */
    hactool_ctx_t *tool_ctx;
    union {
        pfs0_ctx_t pfs0_ctx;
        romfs_ctx_t romfs_ctx;
        nca0_romfs_ctx_t nca0_romfs_ctx;
        bktr_section_ctx_t bktr_ctx;
    };
    validity_t superblock_hash_validity;
    unsigned char ctr[0x10];
    uint64_t cur_seek;
    size_t sector_num;
    uint32_t sector_ofs;
    int physical_reads; /* Should reads be forced physical? */
    int uncompressed_reads; /* Read hashed data as stored, for hash checks. */
    section_crypt_type_t crypt_type;
    /* Storage chains, after LibHac's Nca: raw is the decrypted section (the patch alone, for an update), hashed is what
       the hashes cover (base and patch combined, for an update), and storage adds decompression to hashed. */
    storage_t *raw_storage;
    storage_t *hashed_storage;
    storage_t *storage;
    storage_t *compressed; /* The decompressed data, NULL without a compression table. */
    uint64_t compressed_offset; /* Where it starts in storage. */
    storage_t *chain[12]; /* Every storage built for the section, freed with it. */
    unsigned int chain_length;
} nca_section_ctx_t;

typedef struct nca_ctx {
    FILE *file; /* File for this NCA. */
    uint64_t base_offset; /* ScarletNX: where the NCA starts in file, 0 but for one read in place inside an XCI or NSP. */
    const unsigned char *map; /* ScarletNX: file mapped into memory (map_size bytes), which sections read instead of
                                 file when it is set: the runtime's reads, from several threads. NULL otherwise. */
    uint64_t map_size;
    size_t file_size;
    unsigned char crypto_type;
    int has_rights_id;
    int is_decrypted;
    int is_cli_target;
    int raw_only; /* ScarletNX: nca_process opens each section's raw storage (the decrypted bytes as the file lays
                     them out, an update's tables included) and reads no file system. */
    enum nca_version format_version;
    validity_t fixed_sig_validity;
    validity_t npdm_sig_validity;
    hactool_ctx_t *tool_ctx;
    unsigned char decrypted_keys[4][0x10];
    unsigned char title_key[0x10];
    nca_section_ctx_t section_contexts[4];
    npdm_t *npdm;
    nca_header_t header;
} nca_ctx_t;

void nca_init(nca_ctx_t *ctx);
void nca_process(nca_ctx_t *ctx);
int nca_decrypt_header(nca_ctx_t *ctx);
/* ScarletNX: a header's words, its FS headers' included, between little-endian and the host's order (the same both
   ways; nothing on a little-endian host). */
void nca_header_swap(nca_header_t *header);
void nca_decrypt_key_area(nca_ctx_t *ctx);
void nca_print(nca_ctx_t *ctx);

void nca_free_section_contexts(nca_ctx_t *ctx);

void nca_section_fseek(nca_section_ctx_t *ctx, uint64_t offset);
size_t nca_section_fread(nca_section_ctx_t *ctx, void *buffer, size_t count);

void nca_save_section_file(nca_section_ctx_t *ctx, uint64_t ofs, uint64_t total_size, filepath_t *filepath);

/* These have to be in nca.c, sadly... */
void nca_process_pfs0_section(nca_section_ctx_t *ctx);
void nca_process_ivfc_section(nca_section_ctx_t *ctx);
void nca_process_nca0_romfs_section(nca_section_ctx_t *ctx);
void nca_process_bktr_section(nca_section_ctx_t *ctx);
void nca_print_pfs0_section(nca_section_ctx_t *ctx);
void nca_print_ivfc_section(nca_section_ctx_t *ctx);
void nca_print_nca0_romfs_section(nca_section_ctx_t *ctx);
void nca_print_bktr_section(nca_section_ctx_t *ctx);


void nca_save_section(nca_section_ctx_t *ctx);
void nca_save_pfs0_section(nca_section_ctx_t *ctx);
void nca_save_ivfc_section(nca_section_ctx_t *ctx);
void nca_save_nca0_romfs_section(nca_section_ctx_t *ctx);
void nca_save_bktr_section(nca_section_ctx_t *ctx);

#endif
