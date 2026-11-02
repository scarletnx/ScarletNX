/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Synthetic archives for the tests that run hactool (src/nx/runtime/driver/hactool): blobs, RomFS and IVFC images,
   PFS0 and HFS0 tables, decrypted NCAs (DNCA) of a game's ExeFS and RomFS, NSPs and gamecards. Nothing from a console.
   The includer defines CHECK(value) and includes nca.h, pfs0.h, hfs0.h, xci.h and sha.h first */
#ifndef SNX_TEST_HAC_SYNTH_H
#define SNX_TEST_HAC_SYNTH_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef MAGIC_DNCA /* ScarletNX's decrypted containers, as nca.c and xci.c name them */
#define MAGIC_DNCA 0x41434E44U
#define MAGIC_DXCI 0x49435844U
#endif
#define IVFC_BLOCK 0x200
#define IVFC_BLOCK_LOG2 9

typedef struct {
    unsigned char* data;
    size_t size;
} blob_t;

static inline void* zalloc(size_t size) {
    void* p = calloc(1, size ? size : 1);
    CHECK(p != NULL);
    return p;
}

static inline size_t align_up(size_t value, size_t alignment) { return (value + alignment - 1) / alignment * alignment; }

static inline void put32(unsigned char* p, uint32_t v) {
    for (unsigned i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}

static inline void put64(unsigned char* p, uint64_t v) {
    for (unsigned i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i));
}

/* A file of a RomFS, a PFS0 or an HFS0. */
typedef struct {
    const char* name;
    blob_t content;
} romfs_file_t;

/* A RomFS of a root directory and its files. */
static inline blob_t romfs_build(const romfs_file_t* files, unsigned count) {
    size_t meta = 0;
    for (unsigned i = 0; i < count; i++) meta += 0x20 + align_up(strlen(files[i].name), 4);
    size_t data_offset = align_up(0x70 + meta, 0x10);
    size_t size = data_offset;
    for (unsigned i = 0; i < count; i++) size = align_up(size, 0x10) + files[i].content.size;
    blob_t romfs = {zalloc(size), size};
    unsigned char* h = romfs.data;
    put64(h, ROMFS_HEADER_SIZE);
    put64(h + 0x08, 0x50); /* directory hashes */
    put64(h + 0x10, 4);
    put64(h + 0x18, 0x54); /* directories */
    put64(h + 0x20, 0x18);
    put64(h + 0x28, 0x6C); /* file hashes */
    put64(h + 0x30, 4);
    put64(h + 0x38, 0x70); /* files */
    put64(h + 0x40, meta);
    put64(h + 0x48, data_offset);
    put32(h + 0x50, 0);
    put32(h + 0x54 + 0x4, ROMFS_ENTRY_EMPTY); /* root: no sibling, no child, files from 0 */
    put32(h + 0x54 + 0x8, ROMFS_ENTRY_EMPTY);
    put32(h + 0x54 + 0xC, count ? 0 : ROMFS_ENTRY_EMPTY);
    put32(h + 0x54 + 0x10, ROMFS_ENTRY_EMPTY);
    put32(h + 0x6C, 0);
    size_t entry = 0, data = 0;
    for (unsigned i = 0; i < count; i++) {
        unsigned char* f = h + 0x70 + entry;
        size_t name = strlen(files[i].name);
        size_t next = entry + 0x20 + align_up(name, 4);
        data = align_up(data, 0x10);
        put32(f + 0x4, i + 1 < count ? (uint32_t)next : ROMFS_ENTRY_EMPTY);
        put64(f + 0x8, data);
        put64(f + 0x10, files[i].content.size);
        put32(f + 0x18, ROMFS_ENTRY_EMPTY);
        put32(f + 0x1C, (uint32_t)name);
        memcpy(f + 0x20, files[i].name, name);
        memcpy(h + data_offset + data, files[i].content.data, files[i].content.size);
        data += files[i].content.size;
        entry = next;
    }
    return romfs;
}

/* IVFC levels 0 to 4 hashing data (level 5), from offset 0 of a new section image. */
static inline blob_t ivfc_build(const blob_t* data, ivfc_hdr_t* hdr) {
    uint64_t size[IVFC_MAX_LEVEL], offset[IVFC_MAX_LEVEL];
    size[IVFC_MAX_LEVEL - 1] = data->size;
    for (int i = IVFC_MAX_LEVEL - 2; i >= 0; i--) size[i] = (size[i + 1] + IVFC_BLOCK - 1) / IVFC_BLOCK * 0x20;
    CHECK(size[0] <= IVFC_BLOCK);
    offset[0] = 0;
    for (unsigned i = 1; i < IVFC_MAX_LEVEL; i++) offset[i] = align_up(offset[i - 1] + size[i - 1], IVFC_BLOCK);
    blob_t section = {0};
    section.size = align_up(offset[IVFC_MAX_LEVEL - 1] + size[IVFC_MAX_LEVEL - 1], 0x200);
    section.data = zalloc(section.size + IVFC_BLOCK);
    memcpy(section.data + offset[IVFC_MAX_LEVEL - 1], data->data, data->size);
    for (int i = IVFC_MAX_LEVEL - 2; i >= 0; i--) {
        for (uint64_t b = 0; b * IVFC_BLOCK < size[i + 1]; b++) {
            /* A short last block is hashed padded with zeros (hactool's full_block). */
            unsigned char block[IVFC_BLOCK] = {0};
            uint64_t n = size[i + 1] - b * IVFC_BLOCK < IVFC_BLOCK ? size[i + 1] - b * IVFC_BLOCK : IVFC_BLOCK;
            memcpy(block, section.data + offset[i + 1] + b * IVFC_BLOCK, n);
            sha256_hash_buffer(section.data + offset[i] + b * 0x20, block, IVFC_BLOCK);
        }
    }
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic = MAGIC_IVFC;
    hdr->id = 0x20000;
    hdr->master_hash_size = 0x20;
    hdr->num_levels = IVFC_MAX_LEVEL + 1;
    for (unsigned i = 0; i < IVFC_MAX_LEVEL; i++) {
        hdr->level_headers[i].logical_offset = offset[i];
        hdr->level_headers[i].hash_data_size = size[i];
        hdr->level_headers[i].block_size = IVFC_BLOCK_LOG2;
    }
    unsigned char block[IVFC_BLOCK] = {0};
    memcpy(block, section.data, size[0]);
    sha256_hash_buffer(hdr->master_hash, block, IVFC_BLOCK);
    return section;
}

static inline void section_entry(nca_header_t* header, unsigned i, size_t start, size_t size) {
    CHECK(start % 0x200 == 0 && size % 0x200 == 0);
    header->section_entries[i].media_start_offset = (uint32_t)(start / 0x200);
    header->section_entries[i].media_end_offset = (uint32_t)((start + size) / 0x200);
}

/* A PFS0 (an NSP, an ExeFS) or an HFS0 (a gamecard's partitions): its table, then its files one after another, from
   file_align on in the file (an odd one, so nothing read in place lines up by chance). An HFS0's entries hash nothing */
static inline blob_t partition_build(uint32_t magic, const romfs_file_t* files, unsigned count, size_t file_align) {
    size_t entry_size = magic == MAGIC_HFS0 ? sizeof(hfs0_file_entry_t) : sizeof(pfs0_file_entry_t);
    size_t strings = 0, data = 0;
    for (unsigned i = 0; i < count; i++) strings += strlen(files[i].name) + 1;
    strings = align_up(strings, 0x20);
    size_t table = 0x10 + count * entry_size + strings;
    size_t at[16];
    CHECK(count <= 16);
    for (unsigned i = 0; i < count; i++) {
        at[i] = data;
        data += align_up(files[i].content.size, file_align ? file_align : 1);
    }
    blob_t out = {zalloc(table + data), table + data};
    put32(out.data, magic);
    put32(out.data + 4, count);
    put32(out.data + 8, (uint32_t)strings);
    size_t name = 0;
    for (unsigned i = 0; i < count; i++) {
        unsigned char* e = out.data + 0x10 + i * entry_size;
        put64(e, at[i]);
        put64(e + 8, files[i].content.size);
        put32(e + 0x10, (uint32_t)name);
        strcpy((char*)out.data + 0x10 + count * entry_size + name, files[i].name);
        name += strlen(files[i].name) + 1;
        memcpy(out.data + table + at[i], files[i].content.data, files[i].content.size);
    }
    return out;
}

/* A section image holding a PFS0 after its hash table, and its superblock. */
static inline blob_t pfs0_section_build(const blob_t* pfs0, pfs0_superblock_t* sb) {
    uint32_t block = 0x1000;
    size_t blocks = (pfs0->size + block - 1) / block;
    size_t pfs0_offset = align_up(blocks * 0x20, 0x200);
    blob_t section = {zalloc(align_up(pfs0_offset + pfs0->size, 0x200)), align_up(pfs0_offset + pfs0->size, 0x200)};
    memcpy(section.data + pfs0_offset, pfs0->data, pfs0->size);
    for (size_t b = 0; b < blocks; b++) {
        size_t n = pfs0->size - b * block < block ? pfs0->size - b * block : block;
        sha256_hash_buffer(section.data + b * 0x20, pfs0->data + b * block, n);
    }
    memset(sb, 0, sizeof(*sb));
    sha256_hash_buffer(sb->master_hash, section.data, blocks * 0x20);
    sb->block_size = block;
    sb->always_2 = 2;
    sb->hash_table_size = blocks * 0x20;
    sb->pfs0_offset = pfs0_offset;
    sb->pfs0_size = pfs0->size;
    return section;
}

/* A decrypted NCA (DNCA) of content_type: an ExeFS of exefs_count files when exefs is not NULL (main.npdm makes it
   one), then a RomFS of romfs_count files when romfs is not NULL. */
static inline blob_t dnca_build(uint8_t content_type, uint64_t title_id, const romfs_file_t* exefs, unsigned exefs_count,
                         const romfs_file_t* romfs, unsigned romfs_count) {
    nca_header_t header;
    blob_t sections[2] = {{0}}, body;
    unsigned n = 0;
    memset(&header, 0, sizeof(header));
    header.magic = MAGIC_DNCA;
    header.content_type = content_type;
    header.fixed_key_generation = 0xFF;
    header.crypto_type = 0xFF; /* a plaintext archive never indexes key arrays */
    header.title_id = title_id;
    if (exefs) {
        blob_t pfs0 = partition_build(MAGIC_PFS0, exefs, exefs_count, 1);
        nca_fs_header_t* fs = &header.fs_headers[n];
        fs->partition_type = PARTITION_PFS0;
        fs->fs_type = FS_TYPE_PFS0;
        fs->crypt_type = CRYPT_NONE;
        sections[n++] = pfs0_section_build(&pfs0, &fs->pfs0_superblock);
        free(pfs0.data);
    }
    if (romfs) {
        blob_t image = romfs_build(romfs, romfs_count);
        nca_fs_header_t* fs = &header.fs_headers[n];
        fs->partition_type = PARTITION_ROMFS;
        fs->fs_type = FS_TYPE_ROMFS;
        fs->crypt_type = CRYPT_NONE;
        sections[n++] = ivfc_build(&image, &fs->romfs_superblock.ivfc_header);
        free(image.data);
    }
    body.size = 0;
    for (unsigned i = 0; i < n; i++) body.size += sections[i].size;
    body.data = zalloc(body.size);
    size_t at = 0;
    for (unsigned i = 0; i < n; i++) {
        section_entry(&header, i, 0xC00 + at, sections[i].size);
        memcpy(body.data + at, sections[i].data, sections[i].size);
        at += sections[i].size;
        free(sections[i].data);
    }
    header.nca_size = 0xC00 + body.size;
    nca_header_swap(&header); /* little-endian, as a file has it */
    blob_t nca = {zalloc(0xC00 + body.size), 0xC00 + body.size};
    memcpy(nca.data, &header, sizeof(header));
    memcpy(nca.data + 0xC00, body.data, body.size);
    free(body.data);
    return nca;
}

/* A dnca_build NCA turned into an encrypted one of master key generation: an NCA3 header encrypted with header_key
   (AES-XTS, 0x200-byte sectors from 0, as nca.c decrypts it) whose sections are AES-CTR, its key area left zero. Only
   its header can be read: it says what the keys file must have, which the tests' keys files lack */
static inline void nca_encrypt(blob_t* nca, const unsigned char header_key[32], unsigned generation) {
    nca_header_t header;
    unsigned char sealed[0xC00];
    CHECK(nca->size >= 0xC00 && sizeof(header) == 0xC00 && generation < 0xFF);
    memcpy(&header, nca->data, sizeof(header));
    nca_header_swap(&header); /* the host's order, then the file's again */
    header.magic = MAGIC_NCA3;
    header.crypto_type = 2;
    header.crypto_type2 = (uint8_t)(generation + 1); /* 0 and 1 are both master key 0 */
    header.kaek_ind = 0;
    for (unsigned i = 0; i < 4; i++)
        if (header.section_entries[i].media_start_offset) header.fs_headers[i].crypt_type = CRYPT_CTR;
    nca_header_swap(&header);
    aes_ctx_t* aes = new_aes_ctx(header_key, 32, AES_MODE_XTS);
    aes_xts_encrypt(aes, sealed, &header, sizeof(sealed), 0, 0x200);
    free_aes_ctx(aes);
    memcpy(nca->data, sealed, sizeof(sealed));
}

/* A gamecard (DXCI) whose root partition holds a secure one of files, after the card's key area when key_area. */
static inline blob_t gamecard_build(const romfs_file_t* files, unsigned count, int key_area) {
    blob_t secure = partition_build(MAGIC_HFS0, files, count, 0x203);
    romfs_file_t part = {"secure", secure};
    blob_t root = partition_build(MAGIC_HFS0, &part, 1, 1);
    size_t root_header = 0x10 + sizeof(hfs0_file_entry_t) + 0x20; /* "secure" and its padding */
    size_t lead = key_area ? 0x1000 : 0;
    xci_header_t header;
    memset(&header, 0, sizeof(header));
    header.magic = snx_le32(MAGIC_DXCI); /* little-endian, as a file has it */
    header.hfs0_offset = snx_le64(0xF000);
    header.hfs0_header_size = snx_le64(root_header);
    sha256_hash_buffer(header.hfs0_header_hash, root.data, root_header);
    blob_t card = {zalloc(lead + 0xF000 + root.size), lead + 0xF000 + root.size};
    memcpy(card.data + lead, &header, sizeof(header));
    memcpy(card.data + lead + 0xF000, root.data, root.size);
    free(secure.data);
    free(root.data);
    return card;
}

#endif
