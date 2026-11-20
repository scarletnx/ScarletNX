/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Synthetic archives exercise the real pinned hactool without console keys or game assets. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif
#if defined(_WIN32) && defined(__clang__)
/* The pinned upstream header redeclares the MinGW CRT's imported fseeko64. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winconsistent-dllimport"
#endif
#include "nx/runtime/driver/hactool/nca.h"
#include "nx/runtime/driver/hactool/hfs0.h"
#include "nx/runtime/driver/hactool/pfs0.h"
#include "nx/runtime/driver/hactool/xci.h"
#include "nx/runtime/driver/hactool/sha.h"
#if defined(_WIN32) && defined(__clang__)
#pragma clang diagnostic pop
#endif

#define MAGIC_DNCA 0x41434E44U
#define MAGIC_DXCI 0x49435844U

static unsigned checks;

static void check(int good, const char* expression, int line) {
    checks++;
    if (!good) {
        fprintf(stderr, "hactool_decrypted.c:%d: check failed: %s\n", line, expression);
        exit(90);
    }
}
#define CHECK(value) check((value) != 0, #value, __LINE__)

static FILE* archive(const void* header, size_t header_size, const void* body, size_t body_size) {
    FILE* file = tmpfile();
    CHECK(file != NULL);
    CHECK(fwrite(header, 1, header_size, file) == header_size);
    if (body_size) CHECK(fwrite(body, 1, body_size, file) == body_size);
    CHECK(!fflush(file));
    rewind(file);
    return file;
}

static void plaintext_headers(void) {
    hactool_ctx_t tool = {0};
    const uint32_t magics[] = {MAGIC_DNCA, MAGIC_NCA3, MAGIC_NCA2};
    for (unsigned i = 0; i < sizeof(magics) / sizeof(magics[0]); i++) {
        nca_header_t header = {0};
        nca_ctx_t ctx;
        header.magic = magics[i];
        header.fixed_key_generation = 0xFF;
        header.crypto_type = 0xFF;
        header.crypto_type2 = 0xFF;
        memset(header.rights_id, 0xA5, sizeof(header.rights_id));
        /* DNCA has an explicit marker and does not depend on hactool's padding heuristic. */
        if (i == 0) header._0x340[0] = 1;
        nca_init(&ctx);
        ctx.tool_ctx = &tool;
        nca_header_swap(&header); /* stored little-endian */
        ctx.file = archive(&header, sizeof(header), NULL, 0);
        nca_process(&ctx);
        CHECK(ctx.is_decrypted);
        CHECK(ctx.has_rights_id);
        CHECK(ctx.header.magic == magics[i]);
        CHECK(ctx.format_version == (i == 2 ? NCAVERSION_NCA2 : NCAVERSION_NCA3));
        CHECK(!fclose(ctx.file));
    }

    nca_header_t header = {0};
    nca_ctx_t ctx;
    header.magic = MAGIC_DNCA;
    nca_init(&ctx);
    ctx.tool_ctx = &tool;
    nca_header_swap(&header);
    ctx.file = archive(&header, 0xA00, NULL, 0);
    CHECK(!nca_decrypt_header(&ctx));
    CHECK(!fclose(ctx.file));
}

static void pfs_sections(void) {
    for (unsigned encrypted = 0; encrypted < 2; encrypted++) {
        hactool_ctx_t tool = {0};
        nca_header_t header = {0};
        nca_header_t stored_header;
        nca_ctx_t ctx;
        unsigned char body[0x200] = {0};
        unsigned char stored_body[sizeof(body)];
        unsigned char section_keys[0x40];
        pfs0_header_t pfs = {0};
        pfs0_superblock_t* sb = &header.fs_headers[0].pfs0_superblock;
        pfs.magic = snx_le32(MAGIC_PFS0); /* stored little-endian, as the archive's header below */
        memcpy(body + 0x20, &pfs, sizeof(pfs));
        sha256_hash_buffer(body, &pfs, sizeof(pfs));
        sha256_hash_buffer(sb->master_hash, body, 0x20);
        sb->block_size = 0x20;
        sb->hash_table_size = 0x20;
        sb->pfs0_offset = 0x20;
        sb->pfs0_size = sizeof(pfs);
        header.magic = encrypted ? MAGIC_NCA3 : MAGIC_DNCA;
        header.fixed_key_generation = 0xFF;
        header.section_entries[0].media_start_offset = 6;
        header.section_entries[0].media_end_offset = 7;
        header.fs_headers[0].partition_type = PARTITION_PFS0;
        header.fs_headers[0].fs_type = FS_TYPE_PFS0;
        header.fs_headers[0].crypt_type = CRYPT_CTR;
        for (unsigned i = 0; i < sizeof(tool.settings.keyset.header_key); i++)
            tool.settings.keyset.header_key[i] = (unsigned char)(i + 1);
        for (unsigned i = 0; i < sizeof(section_keys); i++) section_keys[i] = (unsigned char)(i + 0x31);
        memcpy(stored_body, body, sizeof(body));
        nca_header_swap(&header);
        if (encrypted) {
            unsigned char ctr[0x10] = {0};
            aes_ctx_t* aes = new_aes_ctx(tool.settings.keyset.key_area_keys[0][0], 16, AES_MODE_ECB);
            aes_encrypt(aes, header.encrypted_keys, section_keys, sizeof(section_keys));
            free_aes_ctx(aes);
            aes = new_aes_ctx(tool.settings.keyset.header_key, 32, AES_MODE_XTS);
            aes_xts_encrypt(aes, &stored_header, &header, sizeof(header), 0, 0x200);
            free_aes_ctx(aes);
            ctr[15] = 0xC0; /* Section offset 0xC00, measured in AES blocks. */
            aes = new_aes_ctx(section_keys + 0x20, 16, AES_MODE_CTR);
            aes_setiv(aes, ctr, sizeof(ctr));
            aes_encrypt(aes, stored_body, body, sizeof(body));
            free_aes_ctx(aes);
        } else {
            header.crypto_type = 0xFF; /* A plaintext archive must never index key arrays. */
            memset(header.rights_id, 0xA5, sizeof(header.rights_id));
            stored_header = header;
        }
        tool.action = ACTION_VERIFY;
        nca_init(&ctx);
        ctx.tool_ctx = &tool;
        ctx.file = archive(&stored_header, sizeof(stored_header), stored_body, sizeof(stored_body));
        nca_process(&ctx);
        CHECK(ctx.is_decrypted == !encrypted);
        CHECK(ctx.section_contexts[0].is_decrypted == !encrypted);
        CHECK((ctx.section_contexts[0].aes != NULL) == encrypted);
        CHECK(ctx.section_contexts[0].type == PFS0);
        CHECK(ctx.section_contexts[0].superblock_hash_validity == VALIDITY_VALID);
        CHECK(ctx.section_contexts[0].pfs0_ctx.hash_table_validity == VALIDITY_VALID);
        CHECK(ctx.section_contexts[0].pfs0_ctx.header != NULL);
        CHECK(ctx.section_contexts[0].pfs0_ctx.header->magic == MAGIC_PFS0);
        unsigned char got[sizeof(pfs)];
        nca_section_fseek(&ctx.section_contexts[0], 0x20);
        CHECK(nca_section_fread(&ctx.section_contexts[0], got, sizeof(got)) == sizeof(got));
        CHECK(!memcmp(got, &pfs, sizeof(got)));
        nca_free_section_contexts(&ctx);
        CHECK(!fclose(ctx.file));
    }
}

static void plaintext_patch(unsigned crypto, size_t gap, size_t trailing) {
    const size_t subsection_offset = 0x8200 + gap;
    const size_t body_size = subsection_offset + 0x8000 + trailing;
    unsigned char* body = calloc(1, body_size);
    CHECK(body != NULL);
    for (unsigned i = 0; i < 0x200; i++) body[i] = (unsigned char)(i ^ 0x53);
    bktr_relocation_block_t* relocs = (bktr_relocation_block_t*)(body + 0x200);
    bktr_subsection_block_t* subs = (bktr_subsection_block_t*)(body + subsection_offset);
    if (gap) memset(body + 0x8200, 0x6D, gap);
    if (trailing) memset(body + subsection_offset + 0x8000, 0xA9, trailing);
    relocs->num_buckets = snx_le32(1); /* the tables are little-endian */
    relocs->total_size = snx_le64(16);
    relocs->buckets[0].num_entries = snx_le32(3);
    relocs->buckets[0].virtual_offset_end = snx_le64(16);
    relocs->buckets[0].entries[0].phys_offset = snx_le64(4);
    relocs->buckets[0].entries[1].virt_offset = snx_le64(5);
    relocs->buckets[0].entries[1].phys_offset = snx_le64(0x23);
    relocs->buckets[0].entries[1].is_patch = snx_le32(1);
    relocs->buckets[0].entries[2].virt_offset = snx_le64(10);
    relocs->buckets[0].entries[2].phys_offset = snx_le64(20);
    subs->num_buckets = snx_le32(1);
    subs->total_size = snx_le64(gap || trailing ? 0x200 : subsection_offset);
    subs->buckets[0].num_entries = snx_le32(1);
    subs->buckets[0].physical_offset_end = snx_le64(0x200);
    subs->buckets[0].entries[0].ctr_val = snx_le32(0x12345678);

    hactool_ctx_t tool = {0};
    nca_header_t header = {0};
    nca_ctx_t ctx;
    header.magic = MAGIC_DNCA;
    header.fixed_key_generation = 0xFF;
    header.section_entries[0].media_start_offset = 6;
    header.section_entries[0].media_end_offset = 6 + (uint32_t)(body_size / 0x200);
    header.fs_headers[0].partition_type = PARTITION_ROMFS;
    header.fs_headers[0].fs_type = FS_TYPE_ROMFS;
    header.fs_headers[0].crypt_type = (unsigned char)crypto;
    bktr_superblock_t* sb = &header.fs_headers[0].bktr_superblock;
    sb->ivfc_header.magic = MAGIC_IVFC;
    sb->relocation_header.magic = MAGIC_BKTR;
    sb->relocation_header.offset = 0x200;
    sb->relocation_header.size = 0x8000;
    sb->relocation_header.num_entries = 3;
    sb->subsection_header.magic = MAGIC_BKTR;
    sb->subsection_header.offset = subsection_offset;
    sb->subsection_header.size = 0x8000;
    sb->subsection_header.num_entries = 1;

    /* The base is opened before the update, as main.c does. */
    unsigned char base[64], expected[16], got[16];
    for (unsigned i = 0; i < sizeof(base); i++) base[i] = (unsigned char)(i + 0x80);
    memcpy(expected, base + 4, 5);
    memcpy(expected + 5, body + 0x23, 5);
    memcpy(expected + 10, base + 20, 6);
    tool.base_file = archive(base, sizeof(base), NULL, 0);
    tool.base_file_type = BASEFILE_ROMFS;
    nca_init(&ctx);
    ctx.tool_ctx = &tool;
    nca_header_swap(&header);
    ctx.file = archive(&header, sizeof(header), body, body_size);
    nca_process(&ctx);
    nca_section_ctx_t* section = &ctx.section_contexts[0];
    CHECK(section->type == BKTR);
    CHECK(section->crypt_type == CRYPT_BKTR);
    CHECK(section->is_decrypted);
    CHECK(section->aes == NULL);
    CHECK(section->header->crypt_type == crypto);
    CHECK(section->bktr_ctx.indirect != NULL);
    CHECK(section->bktr_ctx.indirect->size == 16);
    nca_section_fseek(section, 0);
    CHECK(nca_section_fread(section, got, sizeof(got)) == sizeof(got));
    CHECK(!memcmp(got, expected, sizeof(got)));
    nca_section_fseek(section, 0);
    CHECK(nca_section_fread(section, got, 3) == 3);
    CHECK(nca_section_fread(section, got + 3, sizeof(got) - 3) == sizeof(got) - 3);
    CHECK(!memcmp(got, expected, sizeof(got)));

    /* Raw saves ignore relocations, even if a base is available and the offset exceeds the virtual RomFS. */
    section->physical_reads = 1;
    nca_section_fseek(section, 0x20);
    CHECK(nca_section_fread(section, got, sizeof(got)) == sizeof(got));
    CHECK(!memcmp(got, body + 0x20, sizeof(got)));
    if (gap) {
        nca_section_fseek(section, 0x8200);
        CHECK(nca_section_fread(section, got, sizeof(got)) == sizeof(got));
        CHECK(!memcmp(got, body + 0x8200, sizeof(got)));
    }
    if (trailing) {
        nca_section_fseek(section, subsection_offset + 0x8000);
        CHECK(nca_section_fread(section, got, sizeof(got)) == sizeof(got));
        CHECK(!memcmp(got, body + subsection_offset + 0x8000, sizeof(got)));
    }
    section->physical_reads = 0;
    nca_free_section_contexts(&ctx);
    CHECK(!fclose(tool.base_file));
    tool.base_file = NULL;

    /* Without a base, the update reads as its own section. */
    rewind(ctx.file);
    nca_process(&ctx);
    CHECK(section->type == BKTR);
    CHECK(section->bktr_ctx.indirect == NULL);
    nca_section_fseek(section, 0x30);
    CHECK(nca_section_fread(section, got, sizeof(got)) == sizeof(got));
    CHECK(!memcmp(got, body + 0x30, sizeof(got)));
    nca_free_section_contexts(&ctx);
    CHECK(!fclose(ctx.file));
    free(body);
}

static void invalid_patch_layout(unsigned kind) {
    hactool_ctx_t tool = {0};
    nca_section_ctx_t section = {0};
    bktr_superblock_t sb = {0};
    section.tool_ctx = &tool;
    section.type = BKTR;
    section.is_decrypted = 1;
    section.size = 0x10200;
    section.bktr_ctx.superblock = &sb;
    sb.relocation_header.magic = MAGIC_BKTR;
    sb.relocation_header.offset = 0x200;
    sb.relocation_header.size = 0x8000;
    sb.subsection_header.magic = MAGIC_BKTR;
    sb.subsection_header.offset = 0x8200;
    sb.subsection_header.size = 0x8000;
    switch (kind) {
        case 0: sb.subsection_header.offset = 0x8100; break; /* Overlap. */
        case 1: sb.relocation_header.offset = UINT64_MAX - 0x100; break;
        case 2: sb.subsection_header.size = UINT64_MAX; break;
        case 3: sb.subsection_header.offset = section.size + 1; break;
    }
    /* An invalid range must exit before allocating tables or accessing the nonexistent file. */
    nca_process_bktr_section(&section);
    exit(90);
}

/* A dump's words that name more than it holds: a file's name that starts past its table's string table (kind 4, a
   PFS0; 6, an HFS0; 7, an NCA's PFS0 section) or has no end in it (5), which strcmp and strlen would read to wherever
   a zero is, and an IVFC level's block size of 2^32 (8), a shift by the width of what is shifted */
static void invalid_table(unsigned kind) {
    hactool_ctx_t tool = {0};
    struct {
        pfs0_header_t header;
        pfs0_file_entry_t entry;
        char names[8];
    } pfs = {{snx_le32(MAGIC_PFS0), snx_le32(1), snx_le32(8), 0}, {0, 0, snx_le32(0x7FFFFFF0), 0}, "a"};
    if (kind == 5) pfs.entry.string_table_offset = 0, memset(pfs.names, 'a', sizeof(pfs.names));
    if (kind == 4 || kind == 5) {
        pfs0_ctx_t ctx = {0};
        ctx.tool_ctx = &tool;
        ctx.file = archive(&pfs, sizeof(pfs), NULL, 0);
        pfs0_process(&ctx);
    } else if (kind == 6) {
        struct {
            hfs0_header_t header;
            hfs0_file_entry_t entry;
            char names[8];
        } hfs = {{snx_le32(MAGIC_HFS0), snx_le32(1), snx_le32(8), 0}, {0}, "a"};
        hfs0_ctx_t ctx = {0};
        hfs.entry.string_table_offset = snx_le32(8); /* the first byte after the table */
        ctx.tool_ctx = &tool;
        ctx.file = archive(&hfs, sizeof(hfs), NULL, 0);
        hfs0_process(&ctx);
    } else if (kind == 7) {
        /* pfs_sections' plaintext archive, its PFS0 the one above: the hash table's one block covers it */
        nca_header_t header = {0};
        nca_ctx_t ctx;
        unsigned char body[0x200] = {0};
        pfs0_superblock_t* sb = &header.fs_headers[0].pfs0_superblock;
        memcpy(body + 0x20, &pfs, sizeof(pfs));
        sha256_hash_buffer(body, &pfs, sizeof(pfs));
        sha256_hash_buffer(sb->master_hash, body, 0x20);
        sb->block_size = 0x40;
        sb->hash_table_size = 0x20;
        sb->pfs0_offset = 0x20;
        sb->pfs0_size = sizeof(pfs);
        header.magic = MAGIC_DNCA;
        header.fixed_key_generation = 0xFF;
        header.crypto_type = 0xFF;
        header.section_entries[0].media_start_offset = 6;
        header.section_entries[0].media_end_offset = 7;
        header.fs_headers[0].partition_type = PARTITION_PFS0;
        header.fs_headers[0].fs_type = FS_TYPE_PFS0;
        header.fs_headers[0].crypt_type = CRYPT_CTR;
        memset(header.rights_id, 0xA5, sizeof(header.rights_id));
        nca_header_swap(&header);
        tool.action = ACTION_VERIFY;
        nca_init(&ctx);
        ctx.tool_ctx = &tool;
        ctx.file = archive(&header, sizeof(header), body, sizeof(body));
        nca_process(&ctx);
    } else {
        nca_section_ctx_t section = {0};
        romfs_superblock_t sb = {0};
        section.tool_ctx = &tool;
        section.romfs_ctx.superblock = &sb;
        sb.ivfc_header.level_headers[0].block_size = 32;
        nca_process_ivfc_section(&section);
    }
    exit(90); /* read on, where the table is refused */
}

static void invalid_patch_layouts(const char* self) {
    CHECK(!fflush(NULL));
    for (unsigned kind = 0; kind < 9; kind++) {
#if defined(_WIN32)
        char kind_text[] = {(char)('0' + kind), 0};
        char* quoted_self = malloc(strlen(self) + 3);
        CHECK(quoted_self != NULL);
        sprintf(quoted_self, "\"%s\"", self);
        const char* args[] = {quoted_self, "--invalid-bktr", kind_text, NULL};
        CHECK(_spawnv(_P_WAIT, self, args) == EXIT_FAILURE);
        free(quoted_self);
#else
        (void)self;
        pid_t child = fork();
        CHECK(child >= 0);
        if (!child) kind < 4 ? invalid_patch_layout(kind) : invalid_table(kind);
        int status;
        CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == EXIT_FAILURE);
#endif
    }
}

static void gamecard_headers(void) {
    const uint32_t magics[] = {MAGIC_DXCI, MAGIC_HEAD};
    for (unsigned i = 0; i < sizeof(magics) / sizeof(magics[0]); i++) {
        hactool_ctx_t tool = {0};
        xci_ctx_t ctx = {0};
        xci_header_t header = {0};
        hfs0_header_t root = {0};
        header.magic = snx_le32(magics[i]); /* the header and the partition's are little-endian */
        header.hfs0_offset = snx_le64(sizeof(header));
        header.hfs0_header_size = snx_le64(sizeof(root));
        root.magic = snx_le32(MAGIC_HFS0);
        sha256_hash_buffer(header.hfs0_header_hash, &root, sizeof(root));
        if (i == 0) {
            /* DXCI keeps encrypted gamecard info while its root hash uses no regional suffix. */
            unsigned char plaintext[0x70] = {0};
            unsigned char iv[0x10] = {0};
            xci_gamecard_info_t info = {0};
            info.compatibility_type = COMPAT_CHINA;
            memcpy(plaintext + 0x10, &info, sizeof(info));
            memset(tool.settings.keyset.xci_header_key, 0x57, sizeof(tool.settings.keyset.xci_header_key));
            aes_ctx_t* aes = new_aes_ctx(tool.settings.keyset.xci_header_key, 16, AES_MODE_CBC);
            aes_setiv(aes, iv, sizeof(iv));
            aes_encrypt(aes, header.encrypted_data, plaintext, sizeof(plaintext));
            free_aes_ctx(aes);
        }
        ctx.tool_ctx = &tool;
        ctx.file = archive(&header, sizeof(header), &root, sizeof(root));
        xci_process(&ctx);
        CHECK(snx_le32(ctx.header.magic) == magics[i]);
        CHECK(!ctx.has_decrypted_header);
        CHECK(ctx.hfs0_hash_validity == VALIDITY_VALID);
        CHECK(ctx.partition_ctx.header != NULL);
        CHECK(ctx.partition_ctx.header->magic == MAGIC_HFS0);
        free(ctx.partition_ctx.header);
        CHECK(!fclose(ctx.file));
    }
}

int main(int argc, char** argv) {
    if (argc == 3 && !strcmp(argv[1], "--invalid-bktr")) {
        CHECK(argv[2][0] >= '0' && argv[2][0] <= '8' && !argv[2][1]);
        argv[2][0] < '4' ? invalid_patch_layout((unsigned)(argv[2][0] - '0'))
                         : invalid_table((unsigned)(argv[2][0] - '0'));
    }
    CHECK(sizeof(nca_header_t) == 0xC00);
    CHECK(sizeof(xci_header_t) == 0x200);
    plaintext_headers();
    pfs_sections();
    plaintext_patch(CRYPT_NONE, 0, 0);
    plaintext_patch(CRYPT_BKTR, 0, 0);
    plaintext_patch(CRYPT_NONE, 0, 0x4200);
    plaintext_patch(CRYPT_NONE, 0x200, 0x4200);
    invalid_patch_layouts(argv[0]);
    gamecard_headers();
    printf("hactool decrypted: %u checks passed\n", checks);
    return 0;
}
