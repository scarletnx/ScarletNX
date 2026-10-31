#include <stdlib.h>
#include "nca.h"
#include "aes.h"
#include "pki.h"
#include "sha.h"
#include "rsa.h"
#include "utils.h"
#include "extkeys.h"
#include "filepath.h"

#define SNX_MAGIC_DNCA 0x41434E44 /* "DNCA", NCA3 layout with plaintext sections. */

/* Initialize the context. */
void nca_init(nca_ctx_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
}

/* Updates the CTR for an offset. */
static void nca_update_ctr(unsigned char *ctr, uint64_t ofs) {
    ofs >>= 4;
    for (unsigned int j = 0; j < 0x8; j++) {
        ctr[0x10-j-1] = (unsigned char)(ofs & 0xFF);
        ofs >>= 8;
    }
}

/* ScarletNX: the FS header's words between the file's little-endian order and the host's (the same conversion both
   ways), its superblock as its file system type lays it out. The section counter is bytes, and stays. */
static void nca_fs_header_swap(nca_fs_header_t *h) {
    if (!SNX_HOST_BE) return;
    if (h->fs_type == FS_TYPE_PFS0) {
        /* An NCA0 RomFS superblock has the same layout. */
        snx_le32_array(&h->pfs0_superblock.block_size, 2);
        snx_le64_array(&h->pfs0_superblock.hash_table_offset, 4);
    } else if (h->fs_type == FS_TYPE_ROMFS) {
        /* A RomFS superblock is its IVFC header, which a patch's follows with its two tables (padding otherwise). */
        ivfc_hdr_swap(&h->bktr_superblock.ivfc_header);
        bktr_header_swap(&h->bktr_superblock.relocation_header);
        bktr_header_swap(&h->bktr_superblock.subsection_header);
    }
    snx_le64_array(&h->sparse_info.meta_offset, 2);
    snx_le32_array(&h->sparse_info.meta_header, 4);
    h->sparse_info.physical_offset = snx_le64(h->sparse_info.physical_offset);
    h->sparse_info.generation = snx_le16(h->sparse_info.generation);
    snx_le64_array(&h->compression_info.table_offset, 2);
    snx_le32_array(&h->compression_info.table_header, 4);
    h->compression_info._0x20 = snx_le64(h->compression_info._0x20);
    snx_le64_array(&h->metadata_hash_info.offset, 2);
}

/* ScarletNX: the header's words between the file's order and the host's, the same way. */
void nca_header_swap(nca_header_t *h) {
    if (!SNX_HOST_BE) return;
    h->magic = snx_le32(h->magic);
    h->nca_size = snx_le64(h->nca_size);
    h->title_id = snx_le64(h->title_id);
    h->sdk_version = snx_le32(h->sdk_version);
    for (unsigned int i = 0; i < 4; i++) {
        snx_le32_array(&h->section_entries[i].media_start_offset, 2);
        nca_fs_header_swap(&h->fs_headers[i]);
    }
}

/* ScarletNX: the header as its file has it, ctx's own on a little-endian host, else converted into copy. */
static const nca_header_t *nca_header_le(const nca_ctx_t *ctx, nca_header_t *copy) {
    if (!SNX_HOST_BE) return &ctx->header;
    *copy = ctx->header;
    nca_header_swap(copy);
    return copy;
}

/* ScarletNX: sections are read through storage chains ported from LibHac (storage.c), built as its Nca.OpenStorage
   builds them: sparse layer, decryption, indirect (patch) layer, then decompression past the hash layers. */

static storage_t *nca_section_chain(nca_section_ctx_t *ctx, storage_t *storage) {
    if (storage == NULL) return NULL;
    if (ctx->chain_length == sizeof(ctx->chain) / sizeof(ctx->chain[0])) {
        fprintf(stderr, "Section %"PRId32" needs too many storages!\n", ctx->section_num);
        exit(EXIT_FAILURE);
    }
    ctx->chain[ctx->chain_length++] = storage;
    return storage;
}

/* ScarletNX: [offset, offset + size) of the NCA's file, from its mapping when it has one. */
static storage_t *nca_section_file(nca_section_ctx_t *ctx, uint64_t offset, uint64_t size) {
    if (ctx->map != NULL) return storage_memory_new(ctx->map, ctx->map_size, ctx->base_offset + offset, size);
    return storage_file_new(ctx->file, ctx->base_offset + offset, size);
}

/* The upper half of the section's AES-CTR counter, big-endian; generation, if nonzero, replaces its low word. */
static void nca_section_upper_iv(nca_section_ctx_t *ctx, unsigned char iv[8], uint32_t generation, int replace) {
    for (unsigned int j = 0; j < 0x8; j++) {
        iv[j] = ctx->header->section_ctr[0x8-j-1];
    }
    if (replace) {
        iv[4] = (unsigned char)(generation >> 24);
        iv[5] = (unsigned char)(generation >> 16);
        iv[6] = (unsigned char)(generation >> 8);
        iv[7] = (unsigned char)generation;
    }
}

/* A sparse section keeps only the blocks its update does not replace: a table at physical_offset maps the section's
   offsets onto those blocks or onto zeros. */
static storage_t *nca_section_open_sparse(nca_section_ctx_t *ctx) {
    nca_sparse_info_t *info = &ctx->header->sparse_info;
    if (!bucket_tree_header_verify(&info->meta_header)) return NULL;
    if (info->meta_header.entry_count == 0) {
        return nca_section_chain(ctx, storage_indirect_new(NULL, NULL, bucket_tree_empty(ctx->size)));
    }
    if (info->meta_offset > UINT64_MAX - info->meta_size) {
        fprintf(stderr, "Invalid sparse table!\n");
        return NULL;
    }
    storage_t *physical = nca_section_chain(ctx, nca_section_file(ctx, info->physical_offset, info->meta_offset + info->meta_size));
    storage_t *meta = nca_section_chain(ctx, storage_sub_new(physical, info->meta_offset, info->meta_size));
    if (meta == NULL) return NULL;
    if (!ctx->is_decrypted) {
        unsigned char iv[8];
        nca_section_upper_iv(ctx, iv, (uint32_t)info->generation << 16, 1);
        meta = nca_section_chain(ctx, storage_aes_ctr_new(meta, ctx->aes, iv, info->physical_offset + info->meta_offset));
    }
    bucket_tree_t *tree = bucket_tree_load(meta, 0, meta->size, INDIRECT_ENTRY_SIZE, (uint32_t)info->meta_header.entry_count);
    if (tree == NULL) return NULL;
    storage_t *zeros = nca_section_chain(ctx, storage_fill_new(UINT64_MAX, 0));
    return nca_section_chain(ctx, storage_indirect_new(physical, zeros, tree));
}

/* An update's section: its data in AES-CTR runs of their own generations, then its tables in plain AES-CTR. */
static storage_t *nca_section_open_aes_ctr_ex(nca_section_ctx_t *ctx, storage_t *body) {
    bktr_superblock_t *sb = &ctx->header->bktr_superblock;
    bucket_tree_header_t *header = (bucket_tree_header_t *)&sb->subsection_header.magic;
    uint64_t tables_offset = sb->relocation_header.offset;
    uint64_t ctr_ex_offset = sb->subsection_header.offset;
    unsigned char iv[8];
    if (tables_offset > body->size || ctr_ex_offset < tables_offset || ctr_ex_offset > body->size) {
        fprintf(stderr, "Invalid BKTR layout!\n");
        return NULL;
    }
    if (!bucket_tree_header_verify(header)) return NULL;
    nca_section_upper_iv(ctx, iv, 0, 0);
    storage_t *tables = nca_section_chain(ctx, storage_sub_new(body, tables_offset, body->size - tables_offset));
    tables = nca_section_chain(ctx, storage_aes_ctr_new(tables, ctx->aes, iv, ctx->offset + tables_offset));
    storage_t *data = nca_section_chain(ctx, storage_sub_new(body, 0, tables_offset));
    bucket_tree_t *tree;
    if (header->entry_count == 0) {
        /* Ryubing's LibHac: an update may carry no AesCtrEx entries. */
        tree = bucket_tree_empty(0);
    } else {
        tree = bucket_tree_load(tables, ctr_ex_offset - tables_offset, body->size - ctr_ex_offset, AES_CTR_EX_ENTRY_SIZE, (uint32_t)header->entry_count);
    }
    if (tree == NULL) return NULL;
    storage_t *ctr_ex = nca_section_chain(ctx, storage_aes_ctr_ex_new(data, ctx->aes, iv, ctx->offset, tree));
    return nca_section_chain(ctx, storage_switch_new(ctr_ex, tables_offset, tables));
}

/* The decrypted section, without any update applied. */
static int nca_section_open_raw(nca_section_ctx_t *ctx) {
    storage_t *body;
    if (ctx->header->sparse_info.generation != 0 && ctx->crypt_type != CRYPT_NCA0) {
        body = nca_section_open_sparse(ctx);
    } else {
        body = nca_section_chain(ctx, nca_section_file(ctx, ctx->offset, ctx->size));
    }
    if (body == NULL) return 0;

    storage_t *raw = body;
    if (!ctx->is_decrypted) {
        unsigned char iv[8];
        nca_section_upper_iv(ctx, iv, 0, 0);
        if (ctx->aes == NULL) {
            fprintf(stderr, "Section %"PRId32" has no key for encryption type %"PRId32"!\n", ctx->section_num, (int32_t)ctx->crypt_type);
            return 0;
        }
        switch (ctx->crypt_type) {
            case CRYPT_XTS:
                raw = nca_section_chain(ctx, storage_aes_xts_new(body, ctx->aes, 0x200, 0));
                break;
            case CRYPT_NCA0:
                /* NCA0 numbers its XTS sectors from the end of the header. */
                raw = nca_section_chain(ctx, storage_aes_xts_new(body, ctx->aes, 0x200, (ctx->offset - 0x400ULL) >> 9));
                break;
            case CRYPT_CTR:
                raw = nca_section_chain(ctx, storage_aes_ctr_new(body, ctx->aes, iv, ctx->offset));
                break;
            case CRYPT_BKTR:
                raw = nca_section_open_aes_ctr_ex(ctx, body);
                break;
            default:
                fprintf(stderr, "Section %"PRId32" has unsupported encryption type %"PRId32"!\n", ctx->section_num, (int32_t)ctx->crypt_type);
                return 0;
        }
        if (raw == NULL) return 0;
    }
    ctx->raw_storage = raw;
    ctx->hashed_storage = raw;
    ctx->storage = raw;
    return 1;
}

/* A compression table at table_offset of the hashed data (which starts at data_offset) turns what the hashes cover
   into the blocks of the file system. */
static int nca_section_open_compression(nca_section_ctx_t *ctx, uint64_t data_offset) {
    nca_compression_info_t *info = &ctx->header->compression_info;
    /* NCA0 FS headers have no such field. */
    if (ctx->crypt_type == CRYPT_NCA0 || info->table_offset == 0 || info->table_size == 0) return 1;
    if (!bucket_tree_header_verify(&info->table_header)) return 0;
    storage_t *hashed = ctx->hashed_storage;
    if (data_offset > hashed->size || info->table_offset > hashed->size - data_offset ||
        info->table_size > hashed->size - data_offset - info->table_offset) {
        fprintf(stderr, "Invalid compression table!\n");
        return 0;
    }
    storage_t *data = nca_section_chain(ctx, storage_sub_new(hashed, data_offset, info->table_offset));
    storage_t *table = nca_section_chain(ctx, storage_sub_new(hashed, data_offset + info->table_offset, info->table_size));
    bucket_tree_t *tree = bucket_tree_load(table, 0, table->size, COMPRESSED_ENTRY_SIZE, (uint32_t)info->table_header.entry_count);
    if (tree == NULL) return 0;
    ctx->compressed = nca_section_chain(ctx, storage_compressed_new(data, tree));
    ctx->compressed_offset = data_offset;
    ctx->storage = nca_section_chain(ctx, storage_switch_new(hashed, data_offset, ctx->compressed));
    return 1;
}

static void nca_section_close_storages(nca_section_ctx_t *ctx) {
    while (ctx->chain_length > 0) {
        storage_free(ctx->chain[--ctx->chain_length]);
    }
    ctx->raw_storage = ctx->hashed_storage = ctx->storage = ctx->compressed = NULL;
    ctx->bktr_ctx.indirect = NULL;
}

/* Seek to an offset within a section. */
void nca_section_fseek(nca_section_ctx_t *ctx, uint64_t offset) {
    ctx->cur_seek = offset;
}

size_t nca_section_fread(nca_section_ctx_t *ctx, void *buffer, size_t count) {
    storage_t *storage = ctx->physical_reads ? ctx->raw_storage : ctx->uncompressed_reads ? ctx->hashed_storage : ctx->storage;
    if (storage == NULL || ctx->cur_seek > storage->size) {
        return 0;
    }
    if (count > storage->size - ctx->cur_seek) {
        count = storage->size - ctx->cur_seek;
    }
    if (!storage_read(storage, ctx->cur_seek, buffer, count)) {
        return 0;
    }
    ctx->cur_seek += count;
    return count;
}

void nca_free_section_contexts(nca_ctx_t *ctx) {
    for (unsigned int i = 0; i < 4; i++) {
        if (ctx->section_contexts[i].is_present) {
            nca_section_close_storages(&ctx->section_contexts[i]);
            if (ctx->section_contexts[i].aes) {
                free_aes_ctx(ctx->section_contexts[i].aes);
            }
            if (ctx->section_contexts[i].type == PFS0) {
                free(ctx->section_contexts[i].pfs0_ctx.header);
                if (ctx->section_contexts[i].pfs0_ctx.is_exefs) {
                    free(ctx->section_contexts[i].pfs0_ctx.npdm);
                }
            } else if (ctx->section_contexts[i].type == ROMFS) {
                if (ctx->section_contexts[i].romfs_ctx.directories) {
                    free(ctx->section_contexts[i].romfs_ctx.directories);
                }
                if (ctx->section_contexts[i].romfs_ctx.files) {
                    free(ctx->section_contexts[i].romfs_ctx.files);
                }
            }  else if (ctx->section_contexts[i].type == NCA0_ROMFS) {
                if (ctx->section_contexts[i].nca0_romfs_ctx.directories) {
                    free(ctx->section_contexts[i].nca0_romfs_ctx.directories);
                }
                if (ctx->section_contexts[i].nca0_romfs_ctx.files) {
                    free(ctx->section_contexts[i].nca0_romfs_ctx.files);
                }
            } else if (ctx->section_contexts[i].type == BKTR) {
                if (ctx->section_contexts[i].bktr_ctx.directories) {
                    free(ctx->section_contexts[i].bktr_ctx.directories);
                }
                if (ctx->section_contexts[i].bktr_ctx.files) {
                    free(ctx->section_contexts[i].bktr_ctx.files);
                }
            }
        }
    }
}

static const char *nca_get_section_type_name(enum nca_section_type type) {
    switch (type) {
        case PFS0:
            return "pfs0";
        case ROMFS:
        case BKTR:
        case NCA0_ROMFS:
            return "romfs";
        default:
            return "unknown";
    }
}

static void nca_save(nca_ctx_t *ctx) {
    /* Save header. */
    filepath_t *header_path = &ctx->tool_ctx->settings.header_path;

    if (header_path->valid == VALIDITY_VALID) {
        printf("Saving Header to %s...\n", header_path->char_path);
        FILE *f_hdr = os_fopen(header_path->os_path, OS_MODE_WRITE);

        if (f_hdr != NULL) {
            nca_header_t copy;
            fwrite(nca_header_le(ctx, &copy), 1, 0xC00, f_hdr);
            fclose(f_hdr);
        } else {
            fprintf(stderr, "Failed to open %s!\n", header_path->char_path);
        }
    }


    for (unsigned int i = 0; i < 4; i++) {
        if (ctx->section_contexts[i].is_present) {
            /* printf("Saving section %"PRId32"...\n", i); */
            nca_save_section(&ctx->section_contexts[i]);
            printf("\n");
        }
    }

    /* Save Decrypted NCA. */
    filepath_t *dec_path = &ctx->tool_ctx->settings.plaintext_path;

    if (dec_path->valid == VALIDITY_VALID) {
        printf("Saving Decrypted NCA to %s...\n", dec_path->char_path);
        FILE *f_dec = os_fopen(dec_path->os_path, OS_MODE_WRITE);

        if (f_dec != NULL) {
            nca_header_t copy;
            if (fwrite(nca_header_le(ctx, &copy), 1, 0xC00, f_dec) != 0xC00) {
                fprintf(stderr, "Failed to write header!\n");
                exit(EXIT_FAILURE);
            }

            unsigned char *buf = malloc(0x400000);
            if (buf == NULL) {
                fprintf(stderr, "Failed to allocate file-save buffer!\n");
                exit(EXIT_FAILURE);
            }
            for (unsigned int i = 0; i < 4; i++) {
                if (ctx->section_contexts[i].is_present) {
                    fseeko64(f_dec, ctx->section_contexts[i].offset, SEEK_SET);
                    ctx->section_contexts[i].physical_reads = 1;

                    uint64_t read_size = 0x400000; /* 4 MB buffer. */
                    memset(buf, 0xCC, read_size); /* Debug in case I fuck this up somehow... */
                    uint64_t ofs = 0;
                    uint64_t end_ofs = ofs + ctx->section_contexts[i].size;
                    nca_section_fseek(&ctx->section_contexts[i], ofs);
                    while (ofs < end_ofs) {
                        if (ofs + read_size >= end_ofs) read_size = end_ofs - ofs;
                        if (nca_section_fread(&ctx->section_contexts[i], buf, read_size) != read_size) {
                            fprintf(stderr, "Failed to read file!\n");
                            exit(EXIT_FAILURE);
                        }
                        if (fwrite(buf, 1, read_size, f_dec) != read_size) {
                            fprintf(stderr, "Failed to write file!\n");
                            exit(EXIT_FAILURE);
                        }
                        ofs += read_size;
                    }

                    ctx->section_contexts[i].physical_reads = 0;
                }
            }

            fclose(f_dec);

            free(buf);
        } else {
            fprintf(stderr, "Failed to open %s!\n", dec_path->char_path);
        }
    }
}

void nca_process(nca_ctx_t *ctx) {
    /* First things first, decrypt header. */
    if (!nca_decrypt_header(ctx)) {
        fprintf(stderr, "Invalid NCA header! Are keys correct?\n");
        return;
    }

    nca_header_t copy;
    const nca_header_t *signed_header = nca_header_le(ctx, &copy); /* the signatures cover the bytes as stored */
    if (ctx->header.fixed_key_generation < sizeof(ctx->tool_ctx->settings.keyset.nca_hdr_fixed_key_moduli) / sizeof(ctx->tool_ctx->settings.keyset.nca_hdr_fixed_key_moduli[0])) {
        if (rsa2048_pss_verify(&signed_header->magic, 0x200, ctx->header.fixed_key_sig, ctx->tool_ctx->settings.keyset.nca_hdr_fixed_key_moduli[ctx->header.fixed_key_generation])) {
            ctx->fixed_sig_validity = VALIDITY_VALID;
        } else {
            ctx->fixed_sig_validity = VALIDITY_INVALID;
        }
    } else {
        ctx->fixed_sig_validity = VALIDITY_INVALID;
    }

    /* Sort out crypto type. */
    ctx->crypto_type = ctx->header.crypto_type;
    if (ctx->header.crypto_type2 > ctx->header.crypto_type)
        ctx->crypto_type = ctx->header.crypto_type2;

    if (ctx->crypto_type)
        ctx->crypto_type--; /* 0, 1 are both master key 0. */

    /* Rights ID. */
    for (unsigned int i = 0; i < 0x10; i++) {
        if (ctx->header.rights_id[i] != 0) {
            ctx->has_rights_id = 1;
            break;
        }
    }

    if (ctx->is_cli_target && ctx->tool_ctx->base_nca_ctx != NULL) {
        uint64_t base_tid = ctx->tool_ctx->base_nca_ctx->header.title_id;
        uint64_t expectation = ctx->header.title_id & 0xFFFFFFFFFFFFF7FFULL;
        if (base_tid != expectation) {
            printf("[WARN] Base NCA Title ID doesn't match expectation (%016"PRIx64" != %016"PRIx64")\n", base_tid, expectation);
        }
    }

    /* Enforce content type for extraction if required. */
    if (ctx->tool_ctx->settings.has_expected_content_type) {
        if (ctx->tool_ctx->settings.expected_content_type != ctx->header.content_type) {
            ctx->tool_ctx->action &= ~(ACTION_EXTRACT);
        }
    }

    /* Decrypt key area if required. */
    /* DNCA may retain its rights ID and decrypted key area; neither is needed for plaintext section reads. */
    if (!ctx->is_decrypted && !ctx->has_rights_id) {
        nca_decrypt_key_area(ctx);
    } else if (!ctx->is_decrypted) {
        /* Decrypt title key. */
        aes_ctx_t *aes_ctx = new_aes_ctx(ctx->tool_ctx->settings.keyset.titlekeks[ctx->crypto_type], 16, AES_MODE_ECB);
        if (ctx->is_cli_target && ctx->tool_ctx->settings.has_cli_titlekey) {
            aes_decrypt(aes_ctx, ctx->tool_ctx->settings.dec_cli_titlekey, ctx->tool_ctx->settings.cli_titlekey, 0x10);
        } else if (settings_has_titlekey(&ctx->tool_ctx->settings, ctx->header.rights_id)) {
            titlekey_entry_t *entry = settings_get_titlekey(&ctx->tool_ctx->settings, ctx->header.rights_id);
            aes_decrypt(aes_ctx, entry->dec_titlekey, entry->titlekey, 0x10);
        }
        free_aes_ctx(aes_ctx);
    }

    /* Parse sections. */
    for (unsigned int i = 0; i < 4; i++) {
        if (ctx->header.section_entries[i].media_start_offset) { /* Section exists. */
            ctx->section_contexts[i].is_present = 1;
            ctx->section_contexts[i].is_decrypted = ctx->is_decrypted;
            ctx->section_contexts[i].tool_ctx = ctx->tool_ctx;
            ctx->section_contexts[i].file = ctx->file;
            ctx->section_contexts[i].base_offset = ctx->base_offset;
            ctx->section_contexts[i].map = ctx->map;
            ctx->section_contexts[i].map_size = ctx->map_size;
            ctx->section_contexts[i].section_num = i;
            ctx->section_contexts[i].offset = media_to_real(ctx->header.section_entries[i].media_start_offset);
            ctx->section_contexts[i].size = media_to_real(ctx->header.section_entries[i].media_end_offset) - ctx->section_contexts[i].offset;
            ctx->section_contexts[i].header = &ctx->header.fs_headers[i];
            ctx->section_contexts[i].crypt_type = ctx->section_contexts[i].header->crypt_type;
            if (ctx->format_version == NCAVERSION_NCA0 || ctx->format_version == NCAVERSION_NCA0_BETA) {
                ctx->section_contexts[i].crypt_type = CRYPT_NCA0;
            }
            if (ctx->section_contexts[i].header->partition_type == PARTITION_PFS0 && ctx->section_contexts[i].header->fs_type == FS_TYPE_PFS0) {
                ctx->section_contexts[i].type = PFS0;
                ctx->section_contexts[i].pfs0_ctx.superblock = &ctx->section_contexts[i].header->pfs0_superblock;
            } else if (ctx->section_contexts[i].header->partition_type == PARTITION_ROMFS && ctx->section_contexts[i].header->fs_type == FS_TYPE_ROMFS) {
                if (ctx->section_contexts[i].crypt_type == CRYPT_BKTR ||
                    (ctx->section_contexts[i].is_decrypted &&
                     ctx->section_contexts[i].header->bktr_superblock.relocation_header.magic == MAGIC_BKTR &&
                     ctx->section_contexts[i].header->bktr_superblock.subsection_header.magic == MAGIC_BKTR)) {
                    /* Decrypted encryption flags do not remove the indirect tables needed for virtual reads. */
                    ctx->section_contexts[i].crypt_type = CRYPT_BKTR;
                    ctx->section_contexts[i].type = BKTR;
                    ctx->section_contexts[i].bktr_ctx.superblock = &ctx->section_contexts[i].header->bktr_superblock;
                } else {
                    ctx->section_contexts[i].type = ROMFS;
                    ctx->section_contexts[i].romfs_ctx.superblock = &ctx->section_contexts[i].header->romfs_superblock;
                }
            } else if (ctx->section_contexts[i].header->partition_type == PARTITION_ROMFS && ctx->section_contexts[i].header->fs_type == FS_TYPE_PFS0 && (ctx->format_version == NCAVERSION_NCA0 || ctx->format_version == NCAVERSION_NCA0_BETA)) {
                ctx->section_contexts[i].type = NCA0_ROMFS;
                ctx->section_contexts[i].nca0_romfs_ctx.superblock = &ctx->section_contexts[i].header->nca0_romfs_superblock;
            } else {
                ctx->section_contexts[i].type = INVALID;
            }
            uint64_t ofs = ctx->section_contexts[i].offset >> 4;
            for (unsigned int j = 0; j < 0x8; j++) {
                ctx->section_contexts[i].ctr[j] = ctx->section_contexts[i].header->section_ctr[0x8-j-1];
                ctx->section_contexts[i].ctr[0x10-j-1] = (unsigned char)(ofs & 0xFF);
                ofs >>= 8;
            }
            ctx->section_contexts[i].sector_num = 0;
            ctx->section_contexts[i].sector_ofs = 0;

            if (ctx->section_contexts[i].crypt_type == CRYPT_NONE) {
                ctx->section_contexts[i].is_decrypted = 1;
            }

            if (ctx->section_contexts[i].is_decrypted) {
                ctx->section_contexts[i].aes = NULL;
            } else if (ctx->is_cli_target && ctx->tool_ctx->settings.has_cli_contentkey) {
                ctx->section_contexts[i].aes = new_aes_ctx(ctx->tool_ctx->settings.cli_contentkey, 16, AES_MODE_CTR);
            } else {
                if (ctx->has_rights_id) {
                    if (ctx->is_cli_target && ctx->tool_ctx->settings.has_cli_titlekey) {
                        ctx->section_contexts[i].aes = new_aes_ctx(ctx->tool_ctx->settings.dec_cli_titlekey, 16, AES_MODE_CTR);
                    } else if (settings_has_titlekey(&ctx->tool_ctx->settings, ctx->header.rights_id)) {
                        titlekey_entry_t *entry = settings_get_titlekey(&ctx->tool_ctx->settings, ctx->header.rights_id);
                        ctx->section_contexts[i].aes = new_aes_ctx(entry->dec_titlekey, 16, AES_MODE_CTR);
                    } else {
                        if (i == 0) {
                            printf("[WARN] Unable to match rights id to titlekey. Update title.keys?\n");
                        }
                        unsigned char fallback[0x10] = {0};
                        ctx->section_contexts[i].aes = new_aes_ctx(fallback, 16, AES_MODE_CTR);
                    }
                } else {
                    if (ctx->section_contexts[i].crypt_type == CRYPT_CTR || ctx->section_contexts[i].crypt_type == CRYPT_BKTR) {
                        ctx->section_contexts[i].aes = new_aes_ctx(ctx->decrypted_keys[2], 16, AES_MODE_CTR);
                    } else if (ctx->section_contexts[i].crypt_type == CRYPT_XTS || ctx->section_contexts[i].crypt_type == CRYPT_NCA0) {
                        ctx->section_contexts[i].aes = new_aes_ctx(ctx->decrypted_keys, 32, AES_MODE_XTS);
                        ctx->section_contexts[i].sector_size = 0x200ULL;
                    }
                    if (ctx->section_contexts[i].sector_size) {
                        ctx->section_contexts[i].sector_mask = ctx->section_contexts[i].sector_size - 1ULL;
                    }
                }
            }

            if (ctx->section_contexts[i].type != INVALID && ctx->section_contexts[i].type != BKTR &&
                !nca_section_open_raw(&ctx->section_contexts[i])) {
                fprintf(stderr, "Section %"PRId32" cannot be read.\n", i);
                nca_section_close_storages(&ctx->section_contexts[i]);
                ctx->section_contexts[i].type = INVALID;
            }

            if (ctx->tool_ctx->action & ACTION_VERIFY) {
                printf("Verifying section %"PRId32"...\n", i);
            }
            switch (ctx->section_contexts[i].type) {
                case PFS0:
                    nca_process_pfs0_section(&ctx->section_contexts[i]);
                    /* Verify NPDM sig now, if we can... */
                    if (ctx->section_contexts[i].pfs0_ctx.is_exefs) {
                        ctx->npdm = ctx->section_contexts[i].pfs0_ctx.npdm;
                        if (rsa2048_pss_verify(&signed_header->magic, 0x200, ctx->header.npdm_key_sig, npdm_get_acid(ctx->npdm)->modulus)) {
                            ctx->npdm_sig_validity = VALIDITY_VALID;
                        } else {
                            ctx->npdm_sig_validity = VALIDITY_INVALID;
                        }
                    }
                    break;
                case ROMFS:
                    nca_process_ivfc_section(&ctx->section_contexts[i]);
                    break;
                case NCA0_ROMFS:
                    nca_process_nca0_romfs_section(&ctx->section_contexts[i]);
                    break;
                case BKTR:
                    nca_process_bktr_section(&ctx->section_contexts[i]);
                    break;
                case INVALID:
                default:
                    break;
            }
        }
    }

    if (ctx->tool_ctx->action & ACTION_INFO) {
        nca_print(ctx);
    }

    if (ctx->tool_ctx->action & ACTION_EXTRACT) {
        nca_save(ctx);
    }
}

/* Decrypt NCA header, left as the file has it: little-endian. */
static int nca_decrypt_header_le(nca_ctx_t *ctx) {
    fseeko64(ctx->file, ctx->base_offset, SEEK_SET);
    
    size_t read_size = fread(&ctx->header, 1, 0xC00, ctx->file);
    
    if (read_size != 0xC00 && read_size != 0xA00) {
        fprintf(stderr, "Failed to read NCA header!\n");
        return 0;
    }

    /* DNCA explicitly marks a complete decrypted NCA3 header and all section payloads. */
    if (snx_le32(ctx->header.magic) == SNX_MAGIC_DNCA) {
        if (read_size != 0xC00) {
            fprintf(stderr, "Failed to read DNCA header!\n");
            return 0;
        }
        ctx->is_decrypted = 1;
        ctx->format_version = NCAVERSION_NCA3;
        return 1;
    }

    /* Try to support decrypted NCA headers. */
    if (snx_le32(ctx->header.magic) == MAGIC_NCA3 || snx_le32(ctx->header.magic) == MAGIC_NCA2) {
        if (ctx->header._0x340[0] == 0 && !memcmp(ctx->header._0x340, ctx->header._0x340 + 1, 0xBF)) {
            ctx->is_decrypted = 1;
            if (snx_le32(ctx->header.magic) == MAGIC_NCA3) {
                ctx->format_version = NCAVERSION_NCA3;
            } else {
                ctx->format_version = NCAVERSION_NCA2;
            }
            return 1;
        }
    }

    ctx->is_decrypted = 0;

    nca_header_t dec_header;



    aes_ctx_t *hdr_aes_ctx = new_aes_ctx(ctx->tool_ctx->settings.keyset.header_key, 32, AES_MODE_XTS);
    aes_xts_decrypt(hdr_aes_ctx, &dec_header, &ctx->header, 0x400, 0, 0x200);

    if(read_size == 0xA00 && snx_le32(dec_header.magic) != MAGIC_NCA0) {
        fprintf(stderr, "Failed to read NCA header!\n");
        return 0;
    }
    
    if (snx_le32(dec_header.magic) == MAGIC_NCA3) {
        ctx->format_version = NCAVERSION_NCA3;
        aes_xts_decrypt(hdr_aes_ctx, &dec_header, &ctx->header, 0xC00, 0, 0x200);
        ctx->header = dec_header;
    } else if (snx_le32(dec_header.magic) == MAGIC_NCA2) {
        ctx->format_version = NCAVERSION_NCA2;
        for (unsigned int i = 0; i < 4; i++) {
            if (dec_header.fs_headers[i]._0x148[0] != 0 || memcmp(dec_header.fs_headers[i]._0x148, dec_header.fs_headers[i]._0x148 + 1, 0xB7)) {
                aes_xts_decrypt(hdr_aes_ctx, &dec_header.fs_headers[i], &ctx->header.fs_headers[i], 0x200, 0, 0x200);
            } else {
                memset(&dec_header.fs_headers[i], 0, sizeof(nca_fs_header_t));
            }
        }
        ctx->header = dec_header;
    } else if (snx_le32(dec_header.magic) == MAGIC_NCA0) {
        memset(ctx->decrypted_keys, 0, 0x40);
        unsigned char out_keydata[0x100];
        size_t out_len = 0;
        if (rsa2048_oaep_decrypt_verify(out_keydata, sizeof(out_keydata), (const unsigned char *)dec_header.encrypted_keys, pki_get_beta_nca0_modulus(), pki_get_beta_nca0_exponent(), 0x100, pki_get_beta_nca0_label_hash(), &out_len)) {
            if (out_len >= 0x20) {
                memcpy(ctx->decrypted_keys, out_keydata, 0x20);
                ctx->format_version = NCAVERSION_NCA0_BETA;
            }
        } else {
            unsigned char calc_hash[0x20];
            static const unsigned char expected_hash[0x20] = {0x9A, 0xBB, 0xD2, 0x11, 0x86, 0x00, 0x21, 0x9D, 0x7A, 0xDC, 0x5B, 0x43, 0x95, 0xF8, 0x4E, 0xFD, 0xFF, 0x6B, 0x25, 0xEF, 0x9F, 0x96, 0x85, 0x28, 0x18, 0x9E, 0x76, 0xB0, 0x92, 0xF0, 0x6A, 0xCB};
            sha256_hash_buffer(calc_hash, dec_header.encrypted_keys, 0x20);
            if (memcmp(calc_hash, expected_hash, sizeof(calc_hash)) == 0) {
                ctx->format_version = NCAVERSION_NCA0;
                memcpy(ctx->decrypted_keys, dec_header.encrypted_keys, 0x40);
            } else {
                ctx->format_version = NCAVERSION_NCA0;
                aes_ctx_t *aes_ctx = new_aes_ctx(ctx->tool_ctx->settings.keyset.key_area_keys[ctx->crypto_type][dec_header.kaek_ind], 16, AES_MODE_ECB);
                aes_decrypt(aes_ctx, ctx->decrypted_keys, dec_header.encrypted_keys, 0x20);
                free_aes_ctx(aes_ctx);
            }
        }
        if (ctx->format_version != NCAVERSION_UNKNOWN) {
            memset(dec_header.fs_headers, 0, sizeof(dec_header.fs_headers));
            aes_ctx_t *aes_ctx = new_aes_ctx(ctx->decrypted_keys, 32, AES_MODE_XTS);
            for (unsigned int i = 0; i < 4; i++) {
                if (snx_le32(dec_header.section_entries[i].media_start_offset)) { /* Section exists. */
                    uint64_t offset = media_to_real(snx_le32(dec_header.section_entries[i].media_start_offset));
                    fseeko64(ctx->file, ctx->base_offset + offset, SEEK_SET);
                    if (fread(&dec_header.fs_headers[i], sizeof(dec_header.fs_headers[i]), 1, ctx->file) != 1) {
                        fprintf(stderr, "Failed to read NCA0 FS header at %" PRIx64"!\n", offset);
                        exit(EXIT_FAILURE);
                    }
                    aes_xts_decrypt(aes_ctx, &dec_header.fs_headers[i], &dec_header.fs_headers[i], sizeof(dec_header.fs_headers[i]), (offset - 0x400ULL) >> 9ULL, 0x200);
                }
            }
            free_aes_ctx(aes_ctx);
            ctx->header = dec_header;
        }
    }

    free_aes_ctx(hdr_aes_ctx);
    return ctx->format_version != NCAVERSION_UNKNOWN;
}

/* ScarletNX: the decrypted header in the host's order, which the RSA signatures and a saved header see as the file has
   it: nca_header_le. */
int nca_decrypt_header(nca_ctx_t *ctx) {
    if (!nca_decrypt_header_le(ctx)) return 0;
    nca_header_swap(&ctx->header);
    return 1;
}

/* Decrypt key area. */
void nca_decrypt_key_area(nca_ctx_t *ctx) {
    if (ctx->format_version == NCAVERSION_NCA0_BETA || ctx->format_version == NCAVERSION_NCA0) return;
    aes_ctx_t *aes_ctx = new_aes_ctx(ctx->tool_ctx->settings.keyset.key_area_keys[ctx->crypto_type][ctx->header.kaek_ind], 16, AES_MODE_ECB);
    aes_decrypt(aes_ctx, ctx->decrypted_keys, ctx->header.encrypted_keys, 0x40);
    free_aes_ctx(aes_ctx);
}


static const char *nca_get_distribution_type(nca_ctx_t *ctx) {
    switch (ctx->header.distribution) {
        case 0:
            return "Download";
        case 1:
            return "Gamecard";
        default:
            return "Unknown";
    }
}

static const char *nca_get_content_type(nca_ctx_t *ctx) {
    switch (ctx->header.content_type) {
        case 0:
            return "Program";
        case 1:
            return "Meta";
        case 2:
            return "Control";
        case 3:
            return "Manual";
        case 4:
            return "Data";
        case 5:
            return "PublicData";
        default:
            return "Unknown";
    }
}

static const char *nca_get_encryption_type(nca_ctx_t *ctx) {
    if (ctx->has_rights_id) {
        return "Titlekey crypto";
    } else {
        return "Standard crypto";
    }
}

static void nca_print_key_area(nca_ctx_t *ctx) {
    if (ctx->format_version == NCAVERSION_NCA0_BETA) {
        printf("Key Area (Encrypted):\n");
        memdump(stdout, "Key (RSA-OAEP Encrypted):           ", &ctx->header.encrypted_keys, 0x100);
        if (!ctx->tool_ctx->settings.suppress_keydata_output) {
            printf("Key Area (Decrypted):\n");
            for (unsigned int i = 0; i < 0x2; i++) {
                printf("    Key %"PRId32" (Decrypted):              ", i);
                memdump(stdout, "", &ctx->decrypted_keys[i], 0x10);
            }
        }
    } else if (ctx->format_version == NCAVERSION_NCA0) {
        printf("Key Area (Encrypted):\n");
        for (unsigned int i = 0; i < 0x2; i++) {
            printf("    Key %"PRId32" (Encrypted):              ", i);
            memdump(stdout, "", &ctx->header.encrypted_keys[i], 0x10);
        }
        if (!ctx->tool_ctx->settings.suppress_keydata_output) {
            printf("Key Area (Decrypted):\n");
            for (unsigned int i = 0; i < 0x2; i++) {
                printf("    Key %"PRId32" (Decrypted):              ", i);
                memdump(stdout, "", &ctx->decrypted_keys[i], 0x10);
            }
        }
    } else {
        printf("Key Area (Encrypted):\n");
        for (unsigned int i = 0; i < 0x4; i++) {
            printf("    Key %"PRId32" (Encrypted):              ", i);
            memdump(stdout, "", &ctx->header.encrypted_keys[i], 0x10);
        }
        if (!ctx->tool_ctx->settings.suppress_keydata_output) {
            printf("Key Area (Decrypted):\n");
            for (unsigned int i = 0; i < 0x4; i++) {
                printf("    Key %"PRId32" (Decrypted):              ", i);
                memdump(stdout, "", &ctx->decrypted_keys[i], 0x10);
            }
        }
    }
}

static const char *nca_get_section_type(nca_section_ctx_t *meta) {
    switch (meta->type) {
        case PFS0: {
            if (meta->pfs0_ctx.is_exefs) return "ExeFS";
            return "PFS0";
        }
        case NCA0_ROMFS:     return "NCA0 RomFS";
        case ROMFS:     return "RomFS";
        case BKTR:      return "Patch RomFS";
        case INVALID:
        default:
            return "Unknown/Invalid";
    }
}


static void nca_print_sections(nca_ctx_t *ctx) {
    printf("Sections:\n");
    for (unsigned int i = 0; i < 4; i++) {
        if (ctx->section_contexts[i].is_present) { /* Section exists. */
            printf("    Section %"PRId32":\n", i);
            printf("        Offset:                     0x%012"PRIx64"\n", ctx->section_contexts[i].offset);
            printf("        Size:                       0x%012"PRIx64"\n", ctx->section_contexts[i].size);
            printf("        Partition Type:             %s\n", nca_get_section_type(&ctx->section_contexts[i]));
            if (!(ctx->format_version == NCAVERSION_NCA0 || ctx->format_version == NCAVERSION_NCA0_BETA)) {
                nca_update_ctr(ctx->section_contexts[i].ctr, ctx->section_contexts[i].offset);
                memdump(stdout, "        Section CTR:                ", &ctx->section_contexts[i].ctr, 16);
                if (ctx->section_contexts[i].header->sparse_info.generation != 0) {
                    printf("        Sparse Generation:          0x%04"PRIx16"\n", ctx->section_contexts[i].header->sparse_info.generation);
                }
                if (ctx->section_contexts[i].compressed != NULL) {
                    printf("        Decompressed Size:          0x%012"PRIx64"\n", ctx->section_contexts[i].compressed->size);
                }
            }
            switch (ctx->section_contexts[i].type) {
                case PFS0:     {
                    nca_print_pfs0_section(&ctx->section_contexts[i]);
                    break;
                }
                case ROMFS:     {
                    nca_print_ivfc_section(&ctx->section_contexts[i]);
                    break;
                }
                case NCA0_ROMFS:     {
                    nca_print_nca0_romfs_section(&ctx->section_contexts[i]);
                    break;
                }
                case BKTR:     {
                    nca_print_bktr_section(&ctx->section_contexts[i]);
                    break;
                }
                case INVALID:
                default:     {
                    printf("        Unknown/invalid superblock!");
                }
            }
        }
    }

}

/* Print out information about the NCA. */
void nca_print(nca_ctx_t *ctx) {
    printf("\nNCA:\n");
    print_magic("Magic:                              ", ctx->header.magic);

    if (ctx->tool_ctx->action & ACTION_VERIFY) {
        if (ctx->header.fixed_key_generation < sizeof(ctx->tool_ctx->settings.keyset.nca_hdr_fixed_key_moduli) / sizeof(ctx->tool_ctx->settings.keyset.nca_hdr_fixed_key_moduli[0])) {
            printf("Fixed-Key Index (GOOD):             0x%"PRIX8"\n", ctx->header.fixed_key_generation);
        } else {
            printf("Fixed-Key Index (FAIL):             0x%"PRIX8"\n", ctx->header.fixed_key_generation);
        }
    } else {
        printf("Fixed-Key Index:                    0x%"PRIX8"\n", ctx->header.fixed_key_generation);
    }

    if (ctx->tool_ctx->action & ACTION_VERIFY && ctx->fixed_sig_validity != VALIDITY_UNCHECKED) {
        if (ctx->fixed_sig_validity == VALIDITY_VALID) {
            memdump(stdout, "Fixed-Key Signature (GOOD):         ", &ctx->header.fixed_key_sig, 0x100);
        } else {
            memdump(stdout, "Fixed-Key Signature (FAIL):         ", &ctx->header.fixed_key_sig, 0x100);
        }
    } else {
        memdump(stdout, "Fixed-Key Signature:                ", &ctx->header.fixed_key_sig, 0x100);
    }
    if (ctx->tool_ctx->action & ACTION_VERIFY && ctx->npdm_sig_validity != VALIDITY_UNCHECKED) {
        if (ctx->npdm_sig_validity == VALIDITY_VALID) {
            memdump(stdout, "NPDM Signature (GOOD):              ", &ctx->header.npdm_key_sig, 0x100);
        } else {
            memdump(stdout, "NPDM Signature (FAIL):              ", &ctx->header.npdm_key_sig, 0x100);
        }
    } else {
         memdump(stdout, "NPDM Signature:                     ", &ctx->header.npdm_key_sig, 0x100);
    }
    printf("Content Size:                       0x%012"PRIx64"\n", ctx->header.nca_size);
    printf("Title ID:                           %016"PRIx64"\n", ctx->header.title_id);
    printf("SDK Version:                        %"PRIu8".%"PRIu8".%"PRIu8".%"PRIu8"\n", (uint8_t)(ctx->header.sdk_version >> 24), (uint8_t)(ctx->header.sdk_version >> 16), (uint8_t)(ctx->header.sdk_version >> 8), (uint8_t)ctx->header.sdk_version);
    printf("Distribution type:                  %s\n", nca_get_distribution_type(ctx));
    printf("Content Type:                       %s\n", nca_get_content_type(ctx));
    printf("Master Key Revision:                0x%"PRIX8" (%s)\n", ctx->crypto_type, get_key_revision_summary(ctx->crypto_type));
    printf("Encryption Type:                    %s\n", nca_get_encryption_type(ctx));

    if (ctx->has_rights_id) {
        memdump(stdout, "Rights ID:                          ", &ctx->header.rights_id, 0x10);
        if (ctx->is_cli_target && ctx->tool_ctx->settings.has_cli_titlekey) {
            if (!ctx->tool_ctx->settings.suppress_keydata_output) {
                memdump(stdout, "Titlekey (Encrypted) (From CLI)     ", ctx->tool_ctx->settings.cli_titlekey, 0x10);
                memdump(stdout, "Titlekey (Decrypted) (From CLI)     ", ctx->tool_ctx->settings.dec_cli_titlekey, 0x10);
            }
        } else if (settings_has_titlekey(&ctx->tool_ctx->settings, ctx->header.rights_id)) {
            titlekey_entry_t *entry = settings_get_titlekey(&ctx->tool_ctx->settings, ctx->header.rights_id);
            if (!ctx->tool_ctx->settings.suppress_keydata_output) {
                memdump(stdout, "Titlekey (Encrypted)                ", entry->titlekey, 0x10);
                memdump(stdout, "Titlekey (Decrypted)                ", entry->dec_titlekey, 0x10);
            }
        } else {
            printf("Titlekey:                           Unknown\n");
        }
    } else {
        printf("Key Area Encryption Key:            %"PRIx8"\n", ctx->header.kaek_ind);
        nca_print_key_area(ctx);
    }

    if (ctx->npdm) {
        npdm_process(ctx->npdm, ctx->tool_ctx);
    }

    nca_print_sections(ctx);

    printf("\n");
}

static validity_t nca_section_check_external_hash_table(nca_section_ctx_t *ctx, unsigned char *hash_table, uint64_t data_ofs, uint64_t data_len, uint64_t block_size, int full_block) {
    if (block_size == 0) {
        /* Block size of 0 is always invalid. */
        return VALIDITY_INVALID;
    }
    unsigned char cur_hash[0x20];
    uint64_t read_size = block_size;
    unsigned char *block = malloc(block_size);
    if (block == NULL) {
        fprintf(stderr, "Failed to allocate hash block!\n");
        exit(EXIT_FAILURE);
    }

    validity_t result = VALIDITY_VALID;
    unsigned char *cur_hash_table_entry = hash_table;
    /* Hashes cover the stored blocks, compressed or not. */
    int uncompressed_reads = ctx->uncompressed_reads;
    ctx->uncompressed_reads = 1;
    for (uint64_t ofs = 0; ofs < data_len; ofs += read_size) {
        nca_section_fseek(ctx, ofs + data_ofs);
        if (ofs + read_size > data_len) {
            /* Last block... */
            memset(block, 0, read_size);
            read_size = data_len - ofs;
        }

        if (nca_section_fread(ctx, block, read_size) != read_size) {
            fprintf(stderr, "Failed to read section!\n");
            exit(EXIT_FAILURE);
        }
        sha256_hash_buffer(cur_hash, block, full_block ? block_size : read_size);
        if (memcmp(cur_hash, cur_hash_table_entry, 0x20) != 0) {
            result = VALIDITY_INVALID;
            break;
        }
        cur_hash_table_entry += 0x20;
    }
    ctx->uncompressed_reads = uncompressed_reads;
    free(block);

    return result;

}

static validity_t nca_section_check_hash_table(nca_section_ctx_t *ctx, uint64_t hash_ofs, uint64_t data_ofs, uint64_t data_len, uint64_t block_size, int full_block) {
    if (block_size == 0) {
        /* Block size of 0 is always invalid. */
        return VALIDITY_INVALID;
    }
    uint64_t hash_table_size = data_len / block_size;
    if (data_len % block_size) hash_table_size++;
    hash_table_size *= 0x20;
    unsigned char *hash_table = malloc(hash_table_size);
    if (hash_table == NULL) {
        fprintf(stderr, "Failed to allocate hash table!\n");
        exit(EXIT_FAILURE);
    }

    int uncompressed_reads = ctx->uncompressed_reads;
    ctx->uncompressed_reads = 1;
    nca_section_fseek(ctx, hash_ofs);
    if (nca_section_fread(ctx, hash_table, hash_table_size) != hash_table_size) {
        fprintf(stderr, "Failed to read section!\n");
        exit(EXIT_FAILURE);
    }
    ctx->uncompressed_reads = uncompressed_reads;

    validity_t result = nca_section_check_external_hash_table(ctx, hash_table, data_ofs, data_len, block_size, full_block);

    free(hash_table);

    return result;
}

static void nca_save_pfs0_file(nca_section_ctx_t *ctx, uint32_t i, filepath_t *dirpath) {
    if (i >= ctx->pfs0_ctx.header->num_files) {
        fprintf(stderr, "Could not save file %"PRId32"!\n", i);
        exit(EXIT_FAILURE);
    }
    pfs0_file_entry_t *cur_file = pfs0_get_file_entry(ctx->pfs0_ctx.header, i);
    /* A compressed section's files can be bigger than the section. */
    if (cur_file->size >= ctx->storage->size) {
        fprintf(stderr, "File %"PRId32" too big in PFS0 (section %"PRId32")!\n", i, ctx->section_num);
        exit(EXIT_FAILURE);
    }

    if (strlen(pfs0_get_file_name(ctx->pfs0_ctx.header, i)) >= MAX_PATH - strlen(dirpath->char_path) - 1) {
        fprintf(stderr, "Filename too long in PFS0!\n");
        exit(EXIT_FAILURE);
    }

    filepath_t filepath;
    filepath_copy(&filepath, dirpath);
    filepath_append(&filepath, "%s", pfs0_get_file_name(ctx->pfs0_ctx.header, i));

    printf("Saving %s to %s...\n", pfs0_get_file_name(ctx->pfs0_ctx.header, i), filepath.char_path);
    uint64_t ofs = ctx->pfs0_ctx.superblock->pfs0_offset + pfs0_get_header_size(ctx->pfs0_ctx.header) + cur_file->offset;
    nca_save_section_file(ctx, ofs, cur_file->size, &filepath);
}


void nca_process_pfs0_section(nca_section_ctx_t *ctx) {
    pfs0_superblock_t *sb = ctx->pfs0_ctx.superblock;
    ctx->superblock_hash_validity = nca_section_check_external_hash_table(ctx, sb->master_hash, sb->hash_table_offset, sb->hash_table_size, sb->hash_table_size, 0);
    if (ctx->tool_ctx->action & ACTION_VERIFY) {
        /* Verify actual PFS0... */
        ctx->pfs0_ctx.hash_table_validity = nca_section_check_hash_table(ctx, sb->hash_table_offset, sb->pfs0_offset, sb->pfs0_size, sb->block_size, 0);
    }

    if (ctx->superblock_hash_validity != VALIDITY_VALID) return;

    if (!nca_section_open_compression(ctx, sb->pfs0_offset)) {
        ctx->superblock_hash_validity = VALIDITY_INVALID;
        return;
    }

    /* Read *just* safe amount. */
    pfs0_header_t raw_header;
    nca_section_fseek(ctx, sb->pfs0_offset);
    if (nca_section_fread(ctx, &raw_header, sizeof(raw_header)) != sizeof(raw_header)) {
        fprintf(stderr, "Failed to read PFS0 header!\n");
        exit(EXIT_FAILURE);
    }
    pfs0_header_from_le(&raw_header, 0);

    uint64_t header_size = pfs0_get_header_size(&raw_header);
    ctx->pfs0_ctx.header = malloc(header_size);
    if (ctx->pfs0_ctx.header == NULL) {
        fprintf(stderr, "Failed to get PFS0 header size!\n");
        exit(EXIT_FAILURE);
    }
    nca_section_fseek(ctx, sb->pfs0_offset);
    if (nca_section_fread(ctx, ctx->pfs0_ctx.header, header_size) != header_size) {
        fprintf(stderr, "Failed to read PFS0 header!\n");
        exit(EXIT_FAILURE);
    }
    pfs0_header_from_le(ctx->pfs0_ctx.header, 1);

    for (unsigned int i = 0; i < ctx->pfs0_ctx.header->num_files; i++) {
        pfs0_file_entry_t *cur_file = pfs0_get_file_entry(ctx->pfs0_ctx.header, i);
        if (strcmp(pfs0_get_file_name(ctx->pfs0_ctx.header, i), "main.npdm") == 0) {
            /* We might have found the exefs... */
            if (cur_file->size >= sb->pfs0_size) {
                fprintf(stderr, "NPDM too big!\n");
                exit(EXIT_FAILURE);
            }

            ctx->pfs0_ctx.npdm = malloc(cur_file->size);
            if (ctx->pfs0_ctx.npdm == NULL) {
                fprintf(stderr, "Failed to allocate NPDM!\n");
                exit(EXIT_FAILURE);
            }
            nca_section_fseek(ctx, sb->pfs0_offset + pfs0_get_header_size(ctx->pfs0_ctx.header) + cur_file->offset);
            if (nca_section_fread(ctx, ctx->pfs0_ctx.npdm, cur_file->size) != cur_file->size) {
                fprintf(stderr, "Failed to read NPDM!\n");
                exit(EXIT_FAILURE);
            }

            if (ctx->pfs0_ctx.npdm->magic == SNX_LE32(MAGIC_META)) {
                ctx->pfs0_ctx.is_exefs = 1;
            }
        }
    }
}


void nca_process_ivfc_section(nca_section_ctx_t *ctx) {
    romfs_superblock_t *sb = ctx->romfs_ctx.superblock;
    for (unsigned int i = 0; i < IVFC_MAX_LEVEL; i++) {
        /* Load in the current level's header data. */
        ivfc_level_ctx_t *cur_level = &ctx->romfs_ctx.ivfc_levels[i];
        cur_level->data_offset = sb->ivfc_header.level_headers[i].logical_offset;
        cur_level->data_size = sb->ivfc_header.level_headers[i].hash_data_size;
        cur_level->hash_block_size = 1 << sb->ivfc_header.level_headers[i].block_size;

        if (i != 0) {
            /* Hash table is previous level's data. */
            cur_level->hash_offset = ctx->romfs_ctx.ivfc_levels[i-1].data_offset;
        } else {
            /* Hash table is the superblock hash. Always check the superblock hash. */
            ctx->superblock_hash_validity = nca_section_check_external_hash_table(ctx, sb->ivfc_header.master_hash, cur_level->data_offset, cur_level->data_size, cur_level->hash_block_size, 1);
            cur_level->hash_validity = ctx->superblock_hash_validity;
        }
        if (ctx->tool_ctx->action & ACTION_VERIFY && i != 0) {
            /* Actually check the table. */
            printf("    Verifying IVFC Level %"PRId32"...\n", i);
            cur_level->hash_validity = nca_section_check_hash_table(ctx, cur_level->hash_offset, cur_level->data_offset, cur_level->data_size, cur_level->hash_block_size, 1);
        }
    }

    ctx->romfs_ctx.romfs_offset = ctx->romfs_ctx.ivfc_levels[IVFC_MAX_LEVEL - 1].data_offset;
    if (!nca_section_open_compression(ctx, ctx->romfs_ctx.romfs_offset)) {
        ctx->superblock_hash_validity = VALIDITY_INVALID;
        return;
    }
    nca_section_fseek(ctx, ctx->romfs_ctx.romfs_offset);
    if (nca_section_fread(ctx, &ctx->romfs_ctx.header, sizeof(romfs_hdr_t)) != sizeof(romfs_hdr_t)) {
        fprintf(stderr, "Failed to read RomFS header!\n");
    }
    snx_le64_array(&ctx->romfs_ctx.header, sizeof(ctx->romfs_ctx.header) / 8);

    if ((ctx->tool_ctx->action & (ACTION_EXTRACT | ACTION_LISTROMFS)) && ctx->romfs_ctx.header.header_size == ROMFS_HEADER_SIZE) {
        /* Pre-load the file/data entry caches. */
        ctx->romfs_ctx.directories = calloc(1, ctx->romfs_ctx.header.dir_meta_table_size);
        if (ctx->romfs_ctx.directories == NULL) {
            fprintf(stderr, "Failed to allocate RomFS directory cache!\n");
            exit(EXIT_FAILURE);
        }

        nca_section_fseek(ctx, ctx->romfs_ctx.romfs_offset + ctx->romfs_ctx.header.dir_meta_table_offset);
        if (nca_section_fread(ctx, ctx->romfs_ctx.directories, ctx->romfs_ctx.header.dir_meta_table_size) != ctx->romfs_ctx.header.dir_meta_table_size) {
            fprintf(stderr, "Failed to read RomFS directory cache!\n");
            exit(EXIT_FAILURE);
        }

        ctx->romfs_ctx.files = calloc(1, ctx->romfs_ctx.header.file_meta_table_size);
        if (ctx->romfs_ctx.files == NULL) {
            fprintf(stderr, "Failed to allocate RomFS file cache!\n");
            exit(EXIT_FAILURE);
        }
        nca_section_fseek(ctx, ctx->romfs_ctx.romfs_offset + ctx->romfs_ctx.header.file_meta_table_offset);
        if (nca_section_fread(ctx, ctx->romfs_ctx.files, ctx->romfs_ctx.header.file_meta_table_size) != ctx->romfs_ctx.header.file_meta_table_size) {
            fprintf(stderr, "Failed to read RomFS file cache!\n");
            exit(EXIT_FAILURE);
        }
    }
}


void nca_process_nca0_romfs_section(nca_section_ctx_t *ctx) {
    nca0_romfs_superblock_t *sb = ctx->nca0_romfs_ctx.superblock;
    ctx->superblock_hash_validity = nca_section_check_external_hash_table(ctx, sb->master_hash, sb->hash_table_offset, sb->hash_table_size, sb->hash_table_size, 0);
    if (ctx->tool_ctx->action & ACTION_VERIFY) {
        /* Verify actual ROMFS... */
        ctx->nca0_romfs_ctx.hash_table_validity = nca_section_check_hash_table(ctx, sb->hash_table_offset, sb->romfs_offset, sb->romfs_size, sb->block_size, 0);
    }

    if (ctx->superblock_hash_validity != VALIDITY_VALID) return;

    ctx->nca0_romfs_ctx.romfs_offset = sb->romfs_offset;
    nca_section_fseek(ctx, ctx->nca0_romfs_ctx.romfs_offset);
    if (nca_section_fread(ctx, &ctx->nca0_romfs_ctx.header, sizeof(nca0_romfs_hdr_t)) != sizeof(nca0_romfs_hdr_t)) {
        fprintf(stderr, "Failed to read NCA0 RomFS header!\n");
    }
    snx_le32_array(&ctx->nca0_romfs_ctx.header, sizeof(ctx->nca0_romfs_ctx.header) / 4);

    if ((ctx->tool_ctx->action & (ACTION_EXTRACT | ACTION_LISTROMFS)) && ctx->nca0_romfs_ctx.header.header_size == NCA0_ROMFS_HEADER_SIZE) {
        /* Pre-load the file/data entry caches. */
        ctx->nca0_romfs_ctx.directories = calloc(1, ctx->nca0_romfs_ctx.header.dir_meta_table_size);
        if (ctx->nca0_romfs_ctx.directories == NULL) {
            fprintf(stderr, "Failed to allocate NCA0 RomFS directory cache!\n");
            exit(EXIT_FAILURE);
        }

        nca_section_fseek(ctx, ctx->nca0_romfs_ctx.romfs_offset + ctx->nca0_romfs_ctx.header.dir_meta_table_offset);
        if (nca_section_fread(ctx, ctx->nca0_romfs_ctx.directories, ctx->nca0_romfs_ctx.header.dir_meta_table_size) != ctx->nca0_romfs_ctx.header.dir_meta_table_size) {
            fprintf(stderr, "Failed to read NCA0 RomFS directory cache!\n");
            exit(EXIT_FAILURE);
        }

        ctx->nca0_romfs_ctx.files = calloc(1, ctx->nca0_romfs_ctx.header.file_meta_table_size);
        if (ctx->nca0_romfs_ctx.files == NULL) {
            fprintf(stderr, "Failed to allocate NCA0 RomFS file cache!\n");
            exit(EXIT_FAILURE);
        }
        nca_section_fseek(ctx, ctx->nca0_romfs_ctx.romfs_offset + ctx->nca0_romfs_ctx.header.file_meta_table_offset);
        if (nca_section_fread(ctx, ctx->nca0_romfs_ctx.files, ctx->nca0_romfs_ctx.header.file_meta_table_size) != ctx->nca0_romfs_ctx.header.file_meta_table_size) {
            fprintf(stderr, "Failed to read NCA0 RomFS file cache!\n");
            exit(EXIT_FAILURE);
        }
    }
}

/* What an update's relocation table reads as the original section: the base NCA's RomFS section as stored (its sparse
   layer and decryption, not its compression), a base RomFS image, or filler. */
static storage_t *nca_bktr_original_storage(nca_section_ctx_t *ctx) {
    switch (ctx->tool_ctx->base_file_type) {
        case BASEFILE_ROMFS:
            return nca_section_chain(ctx, storage_file_new(ctx->tool_ctx->base_file, 0, UINT64_MAX));
        case BASEFILE_NCA: {
            nca_ctx_t *base_ctx = ctx->tool_ctx->base_nca_ctx;
            for (unsigned int i = 0; i < 4; i++) {
                if (base_ctx->section_contexts[i].type == ROMFS && base_ctx->section_contexts[i].raw_storage != NULL) {
                    return base_ctx->section_contexts[i].raw_storage;
                }
            }
            fprintf(stderr, "Failed to read from Base NCA RomFS!\n");
            exit(EXIT_FAILURE);
        }
        case BASEFILE_FAKE:
            return nca_section_chain(ctx, storage_fill_new(UINT64_MAX, 0xCC));
        default:
            fprintf(stderr, "Unknown Base File Type!\n");
            exit(EXIT_FAILURE);
    }
}

void nca_process_bktr_section(nca_section_ctx_t *ctx) {
    bktr_superblock_t *sb = ctx->bktr_ctx.superblock;
    /* Validate magics. */
    if (sb->relocation_header.magic == MAGIC_BKTR && sb->subsection_header.magic == MAGIC_BKTR) {
        /* The tables only have to fit the section in order, without overflow or overlap: as in LibHac, either may be
           followed by a gap, and integrity metadata may follow the AesCtrEx table. */
        if (sb->relocation_header.offset > ctx->size ||
            sb->relocation_header.size > ctx->size - sb->relocation_header.offset ||
            sb->subsection_header.offset > ctx->size ||
            sb->subsection_header.size > ctx->size - sb->subsection_header.offset ||
            sb->relocation_header.offset + sb->relocation_header.size > sb->subsection_header.offset) {
            fprintf(stderr, "Invalid BKTR layout!\n");
            exit(EXIT_FAILURE);
        }

        if (!nca_section_open_raw(ctx)) {
            nca_section_close_storages(ctx);
            ctx->superblock_hash_validity = VALIDITY_INVALID;
            return;
        }

        /* Without a base, or a relocation table (LibHac's OpenRawStorageWithPatch), reads stay in the update's own
           section. */
        if (ctx->tool_ctx->base_file != NULL && sb->relocation_header.size != 0) {
            bucket_tree_header_t *header = (bucket_tree_header_t *)&sb->relocation_header.magic;
            bucket_tree_t *tree = NULL;
            if (bucket_tree_header_verify(header) && header->entry_count > 0) {
                tree = bucket_tree_load(ctx->raw_storage, sb->relocation_header.offset, sb->relocation_header.size, INDIRECT_ENTRY_SIZE, (uint32_t)header->entry_count);
            } else if (header->entry_count == 0) {
                fprintf(stderr, "BKTR relocation table is empty!\n");
            }
            if (tree == NULL) {
                nca_section_close_storages(ctx);
                ctx->superblock_hash_validity = VALIDITY_INVALID;
                return;
            }
            storage_t *original = nca_bktr_original_storage(ctx);
            ctx->bktr_ctx.indirect = nca_section_chain(ctx, storage_indirect_new(original, ctx->raw_storage, tree));
            ctx->hashed_storage = ctx->bktr_ctx.indirect;
            ctx->storage = ctx->bktr_ctx.indirect;
        }

        /* Now parse out the romfs stuff. */
        for (unsigned int i = 0; i < IVFC_MAX_LEVEL; i++) {
            /* Load in the current level's header data. */
            ivfc_level_ctx_t *cur_level = &ctx->bktr_ctx.ivfc_levels[i];
            cur_level->data_offset = sb->ivfc_header.level_headers[i].logical_offset;
            cur_level->data_size = sb->ivfc_header.level_headers[i].hash_data_size;
            cur_level->hash_block_size = 1 << sb->ivfc_header.level_headers[i].block_size;

            if (i != 0) {
                /* Hash table is previous level's data. */
                cur_level->hash_offset = ctx->bktr_ctx.ivfc_levels[i-1].data_offset;
            } else if (ctx->tool_ctx->base_file != NULL) {
                /* Hash table is the superblock hash. Always check the superblock hash. */
                ctx->superblock_hash_validity = nca_section_check_external_hash_table(ctx, sb->ivfc_header.master_hash, cur_level->data_offset, cur_level->data_size, cur_level->hash_block_size, 1);
                cur_level->hash_validity = ctx->superblock_hash_validity;
            }
            if (ctx->tool_ctx->action & ACTION_VERIFY && i != 0) {
                /* Actually check the table. */
                printf("    Verifying IVFC Level %"PRId32"...\n", i);
                cur_level->hash_validity = nca_section_check_hash_table(ctx, cur_level->hash_offset, cur_level->data_offset, cur_level->data_size, cur_level->hash_block_size, 1);
            }
        }

        ctx->bktr_ctx.romfs_offset = ctx->bktr_ctx.ivfc_levels[IVFC_MAX_LEVEL - 1].data_offset;
        if (ctx->tool_ctx->base_file != NULL) {
            /* The update's compression table describes the patched section. */
            if (!nca_section_open_compression(ctx, ctx->bktr_ctx.romfs_offset)) {
                ctx->superblock_hash_validity = VALIDITY_INVALID;
                return;
            }
            nca_section_fseek(ctx, ctx->bktr_ctx.romfs_offset);
            if (nca_section_fread(ctx, &ctx->bktr_ctx.header, sizeof(romfs_hdr_t)) != sizeof(romfs_hdr_t)) {
                fprintf(stderr, "Failed to read BKTR Virtual RomFS header!\n");
            }
            snx_le64_array(&ctx->bktr_ctx.header, sizeof(ctx->bktr_ctx.header) / 8);

            if ((ctx->tool_ctx->action & (ACTION_EXTRACT | ACTION_LISTROMFS)) && ctx->bktr_ctx.header.header_size == ROMFS_HEADER_SIZE) {
                /* Pre-load the file/data entry caches. */
                ctx->bktr_ctx.directories = calloc(1, ctx->bktr_ctx.header.dir_meta_table_size);
                if (ctx->bktr_ctx.directories == NULL) {
                    fprintf(stderr, "Failed to allocate RomFS directory cache!\n");
                    exit(EXIT_FAILURE);
                }

                nca_section_fseek(ctx, ctx->bktr_ctx.romfs_offset + ctx->bktr_ctx.header.dir_meta_table_offset);
                if (nca_section_fread(ctx, ctx->bktr_ctx.directories, ctx->bktr_ctx.header.dir_meta_table_size) != ctx->bktr_ctx.header.dir_meta_table_size) {
                    fprintf(stderr, "Failed to read RomFS directory cache!\n");
                    exit(EXIT_FAILURE);
                }
                ctx->bktr_ctx.files = calloc(1, ctx->bktr_ctx.header.file_meta_table_size);
                if (ctx->bktr_ctx.files == NULL) {
                    fprintf(stderr, "Failed to allocate RomFS file cache!\n");
                    exit(EXIT_FAILURE);
                }
                nca_section_fseek(ctx, ctx->bktr_ctx.romfs_offset + ctx->bktr_ctx.header.file_meta_table_offset);
                if (nca_section_fread(ctx, ctx->bktr_ctx.files, ctx->bktr_ctx.header.file_meta_table_size) != ctx->bktr_ctx.header.file_meta_table_size) {
                    fprintf(stderr, "Failed to read RomFS file cache!\n");
                    exit(EXIT_FAILURE);
                }
            }
        }
    }
}

void nca_print_pfs0_section(nca_section_ctx_t *ctx) {
    if (ctx->tool_ctx->action & ACTION_VERIFY) {
        if (ctx->superblock_hash_validity == VALIDITY_VALID) {
            memdump(stdout, "        Superblock Hash (GOOD):     ", &ctx->pfs0_ctx.superblock->master_hash, 0x20);
        } else {
            memdump(stdout, "        Superblock Hash (FAIL):     ", &ctx->pfs0_ctx.superblock->master_hash, 0x20);
        }
        printf("        Hash Table (%s):\n", GET_VALIDITY_STR(ctx->pfs0_ctx.hash_table_validity));
    } else {
        memdump(stdout, "        Superblock Hash:            ", &ctx->pfs0_ctx.superblock->master_hash, 0x20);
        printf("        Hash Table:\n");
    }
    printf("            Offset:                 %012"PRIx64"\n", ctx->pfs0_ctx.superblock->hash_table_offset);
    printf("            Size:                   %012"PRIx64"\n", ctx->pfs0_ctx.superblock->hash_table_size);
    printf("            Block Size:             0x%"PRIx32"\n", ctx->pfs0_ctx.superblock->block_size);
    printf("        PFS0 Offset:                %012"PRIx64"\n", ctx->pfs0_ctx.superblock->pfs0_offset);
    printf("        PFS0 Size:                  %012"PRIx64"\n", ctx->pfs0_ctx.superblock->pfs0_size);
}

void nca_print_ivfc_section(nca_section_ctx_t *ctx) {
    if (ctx->tool_ctx->action & ACTION_VERIFY) {
        if (ctx->superblock_hash_validity == VALIDITY_VALID) {
            memdump(stdout, "        Superblock Hash (GOOD):     ",  &ctx->romfs_ctx.superblock->ivfc_header.master_hash, 0x20);
        } else {
            memdump(stdout, "        Superblock Hash (FAIL):     ",  &ctx->romfs_ctx.superblock->ivfc_header.master_hash, 0x20);
        }
    } else {
            memdump(stdout, "        Superblock Hash:            ", &ctx->romfs_ctx.superblock->ivfc_header.master_hash, 0x20);
    }
    print_magic("        Magic:                      ", ctx->romfs_ctx.superblock->ivfc_header.magic);
    printf("        ID:                         %08"PRIx32"\n", ctx->romfs_ctx.superblock->ivfc_header.id);
    for (unsigned int i = 0; i < IVFC_MAX_LEVEL; i++) {
        if (ctx->tool_ctx->action & ACTION_VERIFY) {
            printf("        Level %"PRId32" (%s):\n", i, GET_VALIDITY_STR(ctx->romfs_ctx.ivfc_levels[i].hash_validity));
        } else {
            printf("        Level %"PRId32":\n", i);
        }
        printf("            Data Offset:            0x%012"PRIx64"\n", ctx->romfs_ctx.ivfc_levels[i].data_offset);
        printf("            Data Size:              0x%012"PRIx64"\n", ctx->romfs_ctx.ivfc_levels[i].data_size);
        if (i != 0) printf("            Hash Offset:            0x%012"PRIx64"\n", ctx->romfs_ctx.ivfc_levels[i].hash_offset);
        printf("            Hash Block Size:        0x%08"PRIx32"\n", ctx->romfs_ctx.ivfc_levels[i].hash_block_size);
    }
}

void nca_print_nca0_romfs_section(nca_section_ctx_t *ctx) {
    if (ctx->tool_ctx->action & ACTION_VERIFY) {
        if (ctx->superblock_hash_validity == VALIDITY_VALID) {
            memdump(stdout, "        Superblock Hash (GOOD):     ", &ctx->nca0_romfs_ctx.superblock->master_hash, 0x20);
        } else {
            memdump(stdout, "        Superblock Hash (FAIL):     ", &ctx->nca0_romfs_ctx.superblock->master_hash, 0x20);
        }
        printf("        Hash Table (%s):\n", GET_VALIDITY_STR(ctx->nca0_romfs_ctx.hash_table_validity));
    } else {
        memdump(stdout, "        Superblock Hash:            ", &ctx->nca0_romfs_ctx.superblock->master_hash, 0x20);
        printf("        Hash Table:\n");
    }
    printf("            Offset:                 %012"PRIx64"\n", ctx->nca0_romfs_ctx.superblock->hash_table_offset);
    printf("            Size:                   %012"PRIx64"\n", ctx->nca0_romfs_ctx.superblock->hash_table_size);
    printf("            Block Size:             0x%"PRIx32"\n", ctx->nca0_romfs_ctx.superblock->block_size);
    printf("        RomFS Offset:               %012"PRIx64"\n", ctx->nca0_romfs_ctx.superblock->romfs_offset);
    printf("        RomFS Size:                 %012"PRIx64"\n", ctx->nca0_romfs_ctx.superblock->romfs_size);
}

void nca_print_bktr_section(nca_section_ctx_t *ctx) {
    if (ctx->raw_storage == NULL) {
        printf("        BKTR section seems to be corrupted.\n");
        return;
    }
    int did_verify = (ctx->tool_ctx->action & ACTION_VERIFY) && (ctx->tool_ctx->base_file != NULL);
    if (did_verify ) {
        if (ctx->superblock_hash_validity == VALIDITY_VALID) {
            memdump(stdout, "        Superblock Hash (GOOD):     ",  &ctx->bktr_ctx.superblock->ivfc_header.master_hash, 0x20);
        } else {
            memdump(stdout, "        Superblock Hash (FAIL):     ",  &ctx->bktr_ctx.superblock->ivfc_header.master_hash, 0x20);
        }
    } else {
            memdump(stdout, "        Superblock Hash:            ", &ctx->bktr_ctx.superblock->ivfc_header.master_hash, 0x20);
    }
    print_magic("        Magic:                      ", ctx->bktr_ctx.superblock->ivfc_header.magic);
    printf("        ID:                         %08"PRIx32"\n", ctx->bktr_ctx.superblock->ivfc_header.id);
    for (unsigned int i = 0; i < IVFC_MAX_LEVEL; i++) {
        if (did_verify) {
            printf("        Level %"PRId32" (%s):\n", i, GET_VALIDITY_STR(ctx->bktr_ctx.ivfc_levels[i].hash_validity));
        } else {
            printf("        Level %"PRId32":\n", i);
        }
        printf("            Data Offset:            0x%012"PRIx64"\n", ctx->bktr_ctx.ivfc_levels[i].data_offset);
        printf("            Data Size:              0x%012"PRIx64"\n", ctx->bktr_ctx.ivfc_levels[i].data_size);
        if (i != 0) printf("            Hash Offset:            0x%012"PRIx64"\n", ctx->bktr_ctx.ivfc_levels[i].hash_offset);
        printf("            Hash Block Size:        0x%08"PRIx32"\n", ctx->bktr_ctx.ivfc_levels[i].hash_block_size);
    }
}

void nca_save_section_file(nca_section_ctx_t *ctx, uint64_t ofs, uint64_t total_size, filepath_t *filepath) {
    FILE *f_out = os_fopen(filepath->os_path, OS_MODE_WRITE);

    if (f_out == NULL) {
        fprintf(stderr, "Failed to open %s!\n", filepath->char_path);
        return;
    }

    /* ScarletNX: the file's size up to 4 MB. A RomFS is thousands of small files, and Windows' heap hands out 4 MB as
       fresh pages each time, 1024 of them faulted in by the fill below. */
    uint64_t read_size = total_size < 0x400000 ? total_size : 0x400000;
    unsigned char *buf = malloc(read_size ? read_size : 1);
    if (buf == NULL) {
        fprintf(stderr, "Failed to allocate file-save buffer!\n");
        exit(EXIT_FAILURE);
    }
    memset(buf, 0xCC, read_size); /* Debug in case I fuck this up somehow... */
    uint64_t end_ofs = ofs + total_size;
    while (ofs < end_ofs) {
        nca_section_fseek(ctx, ofs);
        if (ofs + read_size >= end_ofs) read_size = end_ofs - ofs;
        if (nca_section_fread(ctx, buf, read_size) != read_size) {
            fprintf(stderr, "Failed to read file!\n");
            exit(EXIT_FAILURE);
        }
        if (fwrite(buf, 1, read_size, f_out) != read_size) {
            fprintf(stderr, "Failed to write file!\n");
            exit(EXIT_FAILURE);
        }
        ofs += read_size;
    }

    fclose(f_out);

    free(buf);
}

void nca_save_section(nca_section_ctx_t *ctx) {
    /* Save raw section file... */
    uint64_t offset = 0;
    uint64_t size = ctx->size;
    if (!(ctx->tool_ctx->action & ACTION_RAW)) {
        switch (ctx->type) {
            case PFS0:
                offset = ctx->pfs0_ctx.superblock->pfs0_offset;
                size = ctx->pfs0_ctx.superblock->pfs0_size;
                break;
            case ROMFS:
                offset = ctx->romfs_ctx.ivfc_levels[IVFC_MAX_LEVEL - 1].data_offset;
                size = ctx->romfs_ctx.ivfc_levels[IVFC_MAX_LEVEL - 1].data_size;
                break;
            case NCA0_ROMFS:
                offset = ctx->nca0_romfs_ctx.superblock->romfs_offset;
                size = ctx->nca0_romfs_ctx.superblock->romfs_size;
                break;
            case BKTR:
                if (ctx->tool_ctx->base_file != NULL) {
                    offset = ctx->bktr_ctx.ivfc_levels[IVFC_MAX_LEVEL - 1].data_offset;
                    size = ctx->bktr_ctx.ivfc_levels[IVFC_MAX_LEVEL - 1].data_size;
                }
                break;
            case INVALID:
                break;
        }
    } else if (ctx->type == BKTR && ctx->bktr_ctx.indirect != NULL) {
        size = ctx->bktr_ctx.indirect->size;
    }
    /* A file system is saved decompressed, a raw section as stored. */
    if (ctx->compressed != NULL && !(ctx->tool_ctx->action & ACTION_RAW) && offset == ctx->compressed_offset) {
        size = ctx->compressed->size;
    }
    ctx->uncompressed_reads = (ctx->tool_ctx->action & ACTION_RAW) != 0;

    /* Extract to file. */
    filepath_t *secpath = &ctx->tool_ctx->settings.section_paths[ctx->section_num];

    /* Handle overrides. */
    if (ctx->type == PFS0 && ctx->pfs0_ctx.is_exefs && ctx->tool_ctx->settings.exefs_path.enabled && ctx->tool_ctx->settings.exefs_path.path.valid == VALIDITY_VALID) {
        secpath = &ctx->tool_ctx->settings.exefs_path.path;
    } else if ((ctx->type == ROMFS || ctx->type == NCA0_ROMFS) && ctx->tool_ctx->settings.romfs_path.enabled && ctx->tool_ctx->settings.romfs_path.path.valid == VALIDITY_VALID) {
        secpath = &ctx->tool_ctx->settings.romfs_path.path;
    }

    if (secpath != NULL && secpath->valid == VALIDITY_VALID) {
        filepath_t appended_path;
        filepath_init(&appended_path);
        filepath_copy(&appended_path, secpath);
        if (ctx->tool_ctx->settings.append_section_types) {
            filepath_set_format(&appended_path, "%s.%s", secpath->char_path, nca_get_section_type_name(ctx->type));
            if (appended_path.valid == VALIDITY_VALID) {
                secpath = &appended_path;
            } else {
                printf("[WARN] Failed to append section type to path\n");
            }
        }

        printf("Saving Section %"PRId32" to %s...\n", ctx->section_num, secpath->char_path);
        printf("Size: %012"PRIx64"\n", size);
        nca_save_section_file(ctx, offset, size, secpath);
    }
    ctx->uncompressed_reads = 0;

    switch (ctx->type) {
        case PFS0:
            nca_save_pfs0_section(ctx);
            break;
        case ROMFS:
            nca_save_ivfc_section(ctx);
            break;
        case NCA0_ROMFS:
            nca_save_nca0_romfs_section(ctx);
            break;
        case BKTR:
            if (ctx->tool_ctx->base_file == NULL) {
                fprintf(stderr, "Note: cannot save BKTR section without base romfs.\n");
                break;
            }
            nca_save_bktr_section(ctx);
            break;
        case INVALID:
            break;
    }
}

void nca_save_pfs0_section(nca_section_ctx_t *ctx) {
    if (ctx->superblock_hash_validity == VALIDITY_VALID && ctx->pfs0_ctx.header->magic == MAGIC_PFS0) {
        /* Extract to directory. */
        filepath_t *dirpath = NULL;
        if (ctx->pfs0_ctx.is_exefs && ctx->tool_ctx->settings.exefs_dir_path.enabled) {
            dirpath = &ctx->tool_ctx->settings.exefs_dir_path.path;
        }
        if (dirpath == NULL || dirpath->valid != VALIDITY_VALID) {
            dirpath = &ctx->tool_ctx->settings.section_dir_paths[ctx->section_num];
        }
        if (dirpath != NULL && dirpath->valid == VALIDITY_VALID) {
            filepath_t appended_path;
            filepath_init(&appended_path);
            filepath_copy(&appended_path, dirpath);
            if (ctx->tool_ctx->settings.append_section_types) {
                filepath_set_format(&appended_path, "%s_%s", dirpath->char_path, nca_get_section_type_name(ctx->type));
                if (appended_path.valid == VALIDITY_VALID) {
                    dirpath = &appended_path;
                } else {
                    printf("[WARN] Failed to append section type to path\n");
                }
            }

            os_makedir(dirpath->os_path);
            for (uint32_t i = 0; i < ctx->pfs0_ctx.header->num_files; i++) {
                nca_save_pfs0_file(ctx, i, dirpath);
            }
        }
    } else {
        fprintf(stderr, "Error: section %"PRId32" is corrupted!\n", ctx->section_num);
        return;
    }
}

/* RomFS functions... */
static int nca_is_romfs_file_updated(nca_section_ctx_t *ctx, uint64_t file_offset, uint64_t file_size) {
    /* All files in a Base RomFS are "updated". */
    if (ctx->type == ROMFS) {
        return 1;
    }

    /* Without a relocation table, everything is the update's; through a compression table, the relocated blocks are not
       the file's, so it counts as updated. */
    if (ctx->bktr_ctx.indirect == NULL || ctx->compressed != NULL) {
        return 1;
    }
    return storage_indirect_has_patch_data(ctx->bktr_ctx.indirect, file_offset, file_size);
}

static int nca_visit_romfs_file(nca_section_ctx_t *ctx, uint32_t file_offset, filepath_t *dir_path) {
    filepath_t *cur_path = calloc(1, sizeof(filepath_t));
    if (cur_path == NULL) {
        fprintf(stderr, "Failed to allocate filepath!\n");
        exit(EXIT_FAILURE);
    }

    int found_any_file = 0;

    do {
        romfs_fentry_t fields, *entry = &fields;
        const char *name;
        if (ctx->type == ROMFS) {
            name = romfs_get_fentry(ctx->romfs_ctx.files, file_offset, entry);
        } else {
            name = romfs_get_fentry(ctx->bktr_ctx.files, file_offset, entry);
        }

        filepath_copy(cur_path, dir_path);
        if (entry->name_size) {
            filepath_append_n(cur_path, entry->name_size, "%.*s", (int)entry->name_size, name);
        }

        int found_file = 1;

        /* If we're extracting... */
        uint64_t phys_offset;
        if (ctx->type == ROMFS) {
            phys_offset = ctx->romfs_ctx.romfs_offset + ctx->romfs_ctx.header.data_offset + entry->offset;
        } else {
            phys_offset = ctx->bktr_ctx.romfs_offset + ctx->bktr_ctx.header.data_offset + entry->offset;
        }
        if ((ctx->tool_ctx->action & ACTION_ONLYUPDATEDROMFS) == 0 || nca_is_romfs_file_updated(ctx, phys_offset, entry->size)) {
            if ((ctx->tool_ctx->action & ACTION_LISTROMFS) == 0) {
                printf("Saving %s...\n", cur_path->char_path);
                nca_save_section_file(ctx, phys_offset, entry->size, cur_path);
            } else {
                printf("rom:%s\n", cur_path->char_path);
            }
        } else {
            found_file = 0;
        }

        found_any_file |= found_file;

        file_offset = entry->sibling;
    } while (file_offset != ROMFS_ENTRY_EMPTY);

    free(cur_path);

    return found_any_file;
}

static int nca_visit_nca0_romfs_file(nca_section_ctx_t *ctx, uint32_t file_offset, filepath_t *dir_path) {
    filepath_t *cur_path = calloc(1, sizeof(filepath_t));
    if (cur_path == NULL) {
        fprintf(stderr, "Failed to allocate filepath!\n");
        exit(EXIT_FAILURE);
    }

    int found_any_file = 0;

    do {
        romfs_fentry_t fields, *entry = &fields;
        const char *name = romfs_get_fentry(ctx->nca0_romfs_ctx.files, file_offset, entry);

        filepath_copy(cur_path, dir_path);
        if (entry->name_size) {
            filepath_append_n(cur_path, entry->name_size, "%.*s", (int)entry->name_size, name);
        }

        int found_file = 1;

        /* If we're extracting... */
        uint64_t phys_offset = ctx->nca0_romfs_ctx.romfs_offset + ctx->nca0_romfs_ctx.header.data_offset + entry->offset;

        if ((ctx->tool_ctx->action & ACTION_LISTROMFS) == 0) {
            printf("Saving %s...\n", cur_path->char_path);
            nca_save_section_file(ctx, phys_offset, entry->size, cur_path);
        } else {
            printf("rom:%s\n", cur_path->char_path);
        }

        found_any_file |= found_file;

        file_offset = entry->sibling;
    } while (file_offset != ROMFS_ENTRY_EMPTY);

    free(cur_path);

    return found_any_file;
}

static int nca_visit_romfs_dir(nca_section_ctx_t *ctx, uint32_t dir_offset, filepath_t *parent_path) {
    romfs_direntry_t fields, *entry = &fields;
    const char *name;
    if (ctx->type == ROMFS) {
        name = romfs_get_direntry(ctx->romfs_ctx.directories, dir_offset, entry);
    } else {
        name = romfs_get_direntry(ctx->bktr_ctx.directories, dir_offset, entry);
    }
    filepath_t *cur_path = calloc(1, sizeof(filepath_t));
    if (cur_path == NULL) {
        fprintf(stderr, "Failed to allocate filepath!\n");
        exit(EXIT_FAILURE);
    }

    filepath_copy(cur_path, parent_path);
    if (entry->name_size) {
        filepath_append_n(cur_path, entry->name_size, "%.*s", (int)entry->name_size, name);
    }

    /* If we're actually extracting the romfs, make directory. */
    if ((ctx->tool_ctx->action & ACTION_LISTROMFS) == 0) {
        os_makedir(cur_path->os_path);
    }

    int any_files = 0;

    if (entry->file != ROMFS_ENTRY_EMPTY) {
        any_files |= nca_visit_romfs_file(ctx, entry->file, cur_path);
    }
    if (entry->child != ROMFS_ENTRY_EMPTY) {
        any_files |= nca_visit_romfs_dir(ctx, entry->child, cur_path);
    }

    if (any_files == 0 && ctx->type == BKTR && (ctx->tool_ctx->action & ACTION_ONLYUPDATEDROMFS)) {
        os_rmdir(cur_path->os_path);
    }


    if (entry->sibling != ROMFS_ENTRY_EMPTY) {
        nca_visit_romfs_dir(ctx, entry->sibling, parent_path);
    }

    free(cur_path);
    return any_files;
}

static int nca_visit_nca0_romfs_dir(nca_section_ctx_t *ctx, uint32_t dir_offset, filepath_t *parent_path) {
    romfs_direntry_t fields, *entry = &fields;
    const char *name = romfs_get_direntry(ctx->nca0_romfs_ctx.directories, dir_offset, entry);
    filepath_t *cur_path = calloc(1, sizeof(filepath_t));
    if (cur_path == NULL) {
        fprintf(stderr, "Failed to allocate filepath!\n");
        exit(EXIT_FAILURE);
    }

    filepath_copy(cur_path, parent_path);
    if (entry->name_size) {
        filepath_append_n(cur_path, entry->name_size, "%.*s", (int)entry->name_size, name);
    }

    /* If we're actually extracting the romfs, make directory. */
    if ((ctx->tool_ctx->action & ACTION_LISTROMFS) == 0) {
        os_makedir(cur_path->os_path);
    }

    int any_files = 0;

    if (entry->file != ROMFS_ENTRY_EMPTY) {
        any_files |= nca_visit_nca0_romfs_file(ctx, entry->file, cur_path);
    }
    if (entry->child != ROMFS_ENTRY_EMPTY) {
        any_files |= nca_visit_nca0_romfs_dir(ctx, entry->child, cur_path);
    }

    if (entry->sibling != ROMFS_ENTRY_EMPTY) {
        any_files |= nca_visit_nca0_romfs_dir(ctx, entry->sibling, parent_path);
    }

    free(cur_path);
    return any_files;
}

void nca_save_ivfc_section(nca_section_ctx_t *ctx) {
    if (ctx->superblock_hash_validity == VALIDITY_VALID) {
        if (ctx->romfs_ctx.header.header_size == ROMFS_HEADER_SIZE) {
            if (ctx->tool_ctx->action & ACTION_LISTROMFS) {
                filepath_t fakepath;
                filepath_init(&fakepath);
                filepath_set(&fakepath, "");

                nca_visit_romfs_dir(ctx, 0, &fakepath);
            } else {
                filepath_t *dirpath = NULL;
                if (ctx->tool_ctx->settings.romfs_dir_path.enabled) {
                    dirpath = &ctx->tool_ctx->settings.romfs_dir_path.path;
                }
                if (dirpath == NULL || dirpath->valid != VALIDITY_VALID) {
                    dirpath = &ctx->tool_ctx->settings.section_dir_paths[ctx->section_num];
                }
                if (dirpath != NULL && dirpath->valid == VALIDITY_VALID) {
                    filepath_t appended_path;
                    filepath_init(&appended_path);
                    filepath_copy(&appended_path, dirpath);
                    if (ctx->tool_ctx->settings.append_section_types) {
                        filepath_set_format(&appended_path, "%s_%s", dirpath->char_path, nca_get_section_type_name(ctx->type));
                        if (appended_path.valid == VALIDITY_VALID) {
                            dirpath = &appended_path;
                        } else {
                            printf("[WARN] Failed to append section type to path\n");
                        }
                    }

                    os_makedir(dirpath->os_path);
                    nca_visit_romfs_dir(ctx, 0, dirpath);
                }
            }

            return;
        }
    }

    fprintf(stderr, "Error: section %"PRId32" is corrupted!\n", ctx->section_num);
}


void nca_save_nca0_romfs_section(nca_section_ctx_t *ctx) {
    if (ctx->superblock_hash_validity == VALIDITY_VALID) {
        if (ctx->nca0_romfs_ctx.header.header_size == NCA0_ROMFS_HEADER_SIZE) {
            if (ctx->tool_ctx->action & ACTION_LISTROMFS) {
                filepath_t fakepath;
                filepath_init(&fakepath);
                filepath_set(&fakepath, "");

                nca_visit_nca0_romfs_dir(ctx, 0, &fakepath);
            } else {
                filepath_t *dirpath = NULL;
                if (ctx->tool_ctx->settings.romfs_dir_path.enabled) {
                    dirpath = &ctx->tool_ctx->settings.romfs_dir_path.path;
                }
                if (dirpath == NULL || dirpath->valid != VALIDITY_VALID) {
                    dirpath = &ctx->tool_ctx->settings.section_dir_paths[ctx->section_num];
                }
                if (dirpath != NULL && dirpath->valid == VALIDITY_VALID) {
                    filepath_t appended_path;
                    filepath_init(&appended_path);
                    filepath_copy(&appended_path, dirpath);
                    if (ctx->tool_ctx->settings.append_section_types) {
                        filepath_set_format(&appended_path, "%s_%s", dirpath->char_path, nca_get_section_type_name(ctx->type));
                        if (appended_path.valid == VALIDITY_VALID) {
                            dirpath = &appended_path;
                        } else {
                            printf("[WARN] Failed to append section type to path\n");
                        }
                    }

                    os_makedir(dirpath->os_path);
                    nca_visit_nca0_romfs_dir(ctx, 0, dirpath);
                }
            }

            return;
        }
    }

    fprintf(stderr, "Error: section %"PRId32" is corrupted!\n", ctx->section_num);
}

void nca_save_bktr_section(nca_section_ctx_t *ctx) {
    if (ctx->superblock_hash_validity == VALIDITY_VALID) {
        if (ctx->bktr_ctx.header.header_size == ROMFS_HEADER_SIZE) {
            if (ctx->tool_ctx->action & ACTION_LISTROMFS) {
                filepath_t fakepath;
                filepath_init(&fakepath);
                filepath_set(&fakepath, "");

                nca_visit_romfs_dir(ctx, 0, &fakepath);
            } else {
                filepath_t *dirpath = NULL;
                if (ctx->tool_ctx->settings.romfs_dir_path.enabled) {
                    dirpath = &ctx->tool_ctx->settings.romfs_dir_path.path;
                }
                if (dirpath == NULL || dirpath->valid != VALIDITY_VALID) {
                    dirpath = &ctx->tool_ctx->settings.section_dir_paths[ctx->section_num];
                }
                if (dirpath != NULL && dirpath->valid == VALIDITY_VALID) {
                    filepath_t appended_path;
                    filepath_init(&appended_path);
                    filepath_copy(&appended_path, dirpath);
                    if (ctx->tool_ctx->settings.append_section_types) {
                        filepath_set_format(&appended_path, "%s_%s", dirpath->char_path, nca_get_section_type_name(ctx->type));
                        if (appended_path.valid == VALIDITY_VALID) {
                            dirpath = &appended_path;
                        } else {
                            printf("[WARN] Failed to append section type to path\n");
                        }
                    }

                    os_makedir(dirpath->os_path);
                    nca_visit_romfs_dir(ctx, 0, dirpath);
                }
            }

            return;
        }
    }

    fprintf(stderr, "Error: section %"PRId32" is corrupted!\n", ctx->section_num);
}
