#include <string.h>
#include <stdio.h>
#include "kip.h"
#include "npdm.h"

void ini1_process(ini1_ctx_t *ctx) {
    /* Read *just* safe amount. */
    ini1_header_t raw_header;
    fseeko64(ctx->file, 0, SEEK_SET);
    if (fread(&raw_header, 1, sizeof(raw_header), ctx->file) != sizeof(raw_header)) {
        fprintf(stderr, "Failed to read INI1 header!\n");
        exit(EXIT_FAILURE);
    }

    if (snx_le32(raw_header.magic) != MAGIC_INI1 || snx_le32(raw_header.num_processes) > INI1_MAX_KIPS) {
        printf("Error: INI1 is corrupt!\n");
        exit(EXIT_FAILURE);
    }

    ctx->header = malloc(snx_le32(raw_header.size));
    if (ctx->header == NULL) {
        fprintf(stderr, "Failed to allocate INI1 header!\n");
        exit(EXIT_FAILURE);
    }

    fseeko64(ctx->file, 0, SEEK_SET);
    if (fread(ctx->header, 1, snx_le32(raw_header.size), ctx->file) != snx_le32(raw_header.size)) {
        fprintf(stderr, "Failed to read INI1!\n");
        exit(EXIT_FAILURE);
    }

    uint64_t offset = 0;
    for (unsigned int i = 0; i < snx_le32(ctx->header->num_processes); i++) {
        ctx->kips[i].tool_ctx = ctx->tool_ctx;
        ctx->kips[i].header = (kip1_header_t *)&ctx->header->kip_data[offset];
        if (snx_le32(ctx->kips[i].header->magic) != MAGIC_KIP1) {
            fprintf(stderr, "INI1 is corrupted!\n");
            exit(EXIT_FAILURE);
        }
        offset += kip1_get_size(&ctx->kips[i]);
    }

    if (ctx->tool_ctx->action & ACTION_INFO) {
        ini1_print(ctx);
    }

    if (ctx->tool_ctx->action & ACTION_EXTRACT) {
        ini1_save(ctx);
    }
}

void ini1_print(ini1_ctx_t *ctx) {
    printf("INI1:\n");
    printf("    Number of Processes:            %02"PRIx32"\n", snx_le32(ctx->header->num_processes));
    printf("    Size:                           %08"PRIx32"\n", snx_le32(ctx->header->size));
    printf("\n");
    for (unsigned int i = 0; i < snx_le32(ctx->header->num_processes); i++) {
        printf("Process %02"PRIx32":\n", i);
        kip1_print(&ctx->kips[i], 1);
        printf("\n");
    }
    printf("\n");
}

void ini1_save(ini1_ctx_t *ctx) {
    filepath_t *dirpath = NULL;
    if (ctx->tool_ctx->file_type == FILETYPE_INI1 && ctx->tool_ctx->settings.out_dir_path.enabled) {
        dirpath = &ctx->tool_ctx->settings.out_dir_path.path;
    }
    if (dirpath == NULL || dirpath->valid != VALIDITY_VALID) {
        dirpath = &ctx->tool_ctx->settings.ini1_dir_path;
    }
    if (dirpath != NULL && dirpath->valid == VALIDITY_VALID) {
        os_makedir(dirpath->os_path);
        for (unsigned int i = 0; i < snx_le32(ctx->header->num_processes); i++) {
            char padded_name[0x20];
            memset(&padded_name, 0, sizeof(padded_name));
            memcpy(&padded_name, ctx->kips[i].header->name, sizeof(ctx->kips[i].header->name));
            strcat(padded_name, ".kip1");
            printf("Saving %s to %s/%s...\n", padded_name, dirpath->char_path, padded_name);
            save_buffer_to_directory_file(ctx->kips[i].header, kip1_get_size(&ctx->kips[i]), dirpath, padded_name);
        }
    }
}

static void kip1_blz_uncompress(void *hdr_end) {
    uint32_t addl_size = snx_ld_le32((unsigned char *)hdr_end - 4);
    uint32_t header_size = snx_ld_le32((unsigned char *)hdr_end - 8);
    uint32_t cmp_and_hdr_size = snx_ld_le32((unsigned char *)hdr_end - 12);

    unsigned char *cmp_start = (unsigned char *)(((uintptr_t)hdr_end) - cmp_and_hdr_size);
    uint32_t cmp_ofs = cmp_and_hdr_size - header_size;
    uint32_t out_ofs = cmp_and_hdr_size + addl_size;

    while (out_ofs) {
        unsigned char control = cmp_start[--cmp_ofs];
        for (unsigned int i = 0; i < 8; i++) {
            if (control & 0x80) {
                if (cmp_ofs < 2) {
                    fprintf(stderr, "KIP1 decompression out of bounds!\n");
                    exit(EXIT_FAILURE);
                }
                cmp_ofs -= 2;
                uint16_t seg_val = ((unsigned int)cmp_start[cmp_ofs+1] << 8) | cmp_start[cmp_ofs];
                uint32_t seg_size = ((seg_val >> 12) & 0xF) + 3;
                uint32_t seg_ofs = (seg_val & 0x0FFF) + 3;
                if (out_ofs < seg_size) {
                    /* Kernel restricts segment copy to stay in bounds. */
                    seg_size = out_ofs;
                }
                out_ofs -= seg_size;

                for (unsigned int j = 0; j < seg_size; j++) {
                    cmp_start[out_ofs + j] = cmp_start[out_ofs + j + seg_ofs];
                }
            } else {
                /* Copy directly. */
                if (cmp_ofs < 1) {
                    fprintf(stderr, "KIP1 decompression out of bounds!\n");
                    exit(EXIT_FAILURE);
                }
                cmp_start[--out_ofs] = cmp_start[--cmp_ofs];
            }
            control <<= 1;
            if (out_ofs == 0) {
                return;
            }
        }
    }
}

static void *kip1_uncompress(kip1_ctx_t *ctx, uint64_t *size) {
    /* Make new header with correct sizes, fixed flags. */
    kip1_header_t new_header = *ctx->header;
    for (unsigned int i = 0; i < 3; i++) {
        /* ScarletNX: words copied as stored, little-endian */
        new_header.section_headers[i].compressed_size = new_header.section_headers[i].out_size;
    }
    new_header.flags &= 0xF8;

    *size = kip1_get_size_from_header(&new_header);
    unsigned char *new_kip = calloc(1, *size);
    if (new_kip == NULL) {
        fprintf(stderr, "Failed to allocate uncompressed KIP1!\n");
        exit(EXIT_FAILURE);
    }
    *((kip1_header_t *)new_kip) = new_header;

    uint64_t new_offset = 0x100;
    uint64_t old_offset = 0x100;
    for (unsigned int i = 0; i < 3; i++) {
        // Copy in section data */
        memcpy(new_kip + new_offset, (unsigned char *)ctx->header + old_offset, snx_le32(ctx->header->section_headers[i].compressed_size));
        if (ctx->header->flags & (1 << i)) {
            kip1_blz_uncompress(new_kip + new_offset + snx_le32(ctx->header->section_headers[i].compressed_size));
        }
        new_offset += snx_le32(ctx->header->section_headers[i].out_size);
        old_offset += snx_le32(ctx->header->section_headers[i].compressed_size);
    }

    return new_kip;
}

void kip1_process(kip1_ctx_t *ctx) {
    /* Read *just* safe amount. */
    kip1_header_t raw_header;
    fseeko64(ctx->file, 0, SEEK_SET);
    if (fread(&raw_header, 1, sizeof(raw_header), ctx->file) != sizeof(raw_header)) {
        fprintf(stderr, "Failed to read KIP1 header!\n");
        exit(EXIT_FAILURE);
    }

    if (snx_le32(raw_header.magic) != MAGIC_KIP1) {
        printf("Error: KIP1 is corrupt!\n");
        exit(EXIT_FAILURE);
    }

    uint64_t size = kip1_get_size_from_header(&raw_header);
    ctx->header = malloc(size);
    if (ctx->header == NULL) {
        fprintf(stderr, "Failed to allocate KIP1!\n");
        exit(EXIT_FAILURE);
    }

    fseeko64(ctx->file, 0, SEEK_SET);
    if (fread(ctx->header, 1, size, ctx->file) != size) {
        fprintf(stderr, "Failed to read KIP1!\n");
        exit(EXIT_FAILURE);
    }

    if (ctx->tool_ctx->action & ACTION_INFO) {
        kip1_print(ctx, 0);
    }

    if (ctx->tool_ctx->action & ACTION_EXTRACT) {
        kip1_save(ctx);
    }
}

void kip1_print(kip1_ctx_t *ctx, int suppress) {
    if (!suppress) printf("KIP1:\n");
    printf("    Title ID:                       %016"PRIx64"\n", snx_le64(ctx->header->title_id));
    char padded_name[13];
    memset(&padded_name, 0, sizeof(padded_name));
    memcpy(&padded_name, ctx->header->name, sizeof(ctx->header->name));
    printf("    Name:                           %s\n", padded_name);
    printf("    Process Category:               %s\n", npdm_get_proc_category(snx_le32(ctx->header->process_category)));
    printf("    Main Thread Priority:           %"PRId8"\n", ctx->header->main_thread_priority);
    printf("    Default CPU Core:               %"PRId8"\n", ctx->header->default_core);
    printf("    Is 64 Bit:                      %s\n", (ctx->header->flags & (1 << 3)) ? "True" : "False");
    printf("    Is Address Space 64 Bit:        %s\n", (ctx->header->flags & (1 << 4)) ? "True" : "False");
    printf("    Sections:\n");
    printf("        .text:                      %08"PRIx32"-%08"PRIx32"\n", snx_le32(ctx->header->section_headers[0].out_offset), snx_le32(ctx->header->section_headers[0].out_offset) + align(snx_le32(ctx->header->section_headers[0].out_size), 0x1000));
    printf("        .rodata:                    %08"PRIx32"-%08"PRIx32"\n", snx_le32(ctx->header->section_headers[1].out_offset), snx_le32(ctx->header->section_headers[1].out_offset) + align(snx_le32(ctx->header->section_headers[1].out_size), 0x1000));
    printf("        .rwdata:                    %08"PRIx32"-%08"PRIx32"\n", snx_le32(ctx->header->section_headers[2].out_offset), snx_le32(ctx->header->section_headers[2].out_offset) + snx_le32(ctx->header->section_headers[2].out_size));
    printf("        .bss:                       %08"PRIx32"-%08"PRIx32"\n", snx_le32(ctx->header->section_headers[2].out_offset) + snx_le32(ctx->header->section_headers[2].out_size), align(snx_le32(ctx->header->section_headers[3].out_offset) + snx_le32(ctx->header->section_headers[3].out_size), 0x1000));
    printf("    Kernel Access Control:\n");
    kac_print(ctx->header->capabilities, 0x20);
    printf("\n");
}

void kip1_save(kip1_ctx_t *ctx) {
    filepath_t *uncmp_path = &ctx->tool_ctx->settings.uncompressed_path;
    if (ctx->tool_ctx->file_type == FILETYPE_KIP1) {
        if (uncmp_path->valid == VALIDITY_VALID) {
            FILE *f_uncmp = os_fopen(uncmp_path->os_path, OS_MODE_WRITE);
            if (f_uncmp == NULL) {
                fprintf(stderr, "Failed to open %s!\n", uncmp_path->char_path);
                return;
            }
            uint64_t sz = 0;
            void *uncmp = kip1_uncompress(ctx, &sz);
            if (fwrite(uncmp, 1, sz, f_uncmp) != sz) {
                fprintf(stderr, "Failed to write uncompressed kip!\n");
                exit(EXIT_FAILURE);
            }
            free(uncmp);
            fclose(f_uncmp);
        }
    }
}
