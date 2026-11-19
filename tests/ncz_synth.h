/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* NCZs made from an NCA in memory as nsz's BlockCompressor.py and SolidCompressor.py write them (the layout is in
   src/nx/runtime/driver/hactool/ncz.h): the NCA's first 0x4000 bytes as they are, NCZSECTN and its sections, then
   NCZBLOCK and the blocks (one zstd makes no smaller kept as it is) or one zstd stream, of the NCA's bytes past 0x4000
   with its AES-CTR sections decrypted. The includer defines CHECK(value) and includes hac_synth.h, after nca.h */
#ifndef SNX_TEST_NCZ_SYNTH_H
#define SNX_TEST_NCZ_SYNTH_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <zstd.h>

/* a section of an NCZ's table: its NCA offset and size, nsz's crypto type (3 AES-CTR, 4 an AesCtrEx run, 1 none),
   its key, and the counter whose first 8 bytes are the upper half, big-endian */
typedef struct {
    uint64_t offset, size, crypto;
    unsigned char key[16], counter[16];
} ncz_part_t;

/* how many blocks ncz_build kept as they are and how many it compressed */
typedef struct {
    unsigned stored, compressed;
} ncz_blocks_t;

/* AES-CTR over [at, at + size) of data, which starts at NCA offset at, the counter of a byte its upper half followed
   by its offset / 16 */
static inline void ncz_ctr(const ncz_part_t* part, uint64_t at, unsigned char* data, size_t size) {
    aes_ctx_t* aes = new_aes_ctx(part->key, 16, AES_MODE_CTR);
    while (size) {
        unsigned char iv[16], block[16];
        uint64_t counter = at / 16;
        size_t skip = (size_t)(at % 16), n = 16 - skip < size ? 16 - skip : size;
        memcpy(iv, part->counter, 8);
        for (unsigned i = 0; i < 8; i++) iv[15 - i] = (unsigned char)(counter >> (8 * i));
        memset(block, 0, sizeof block);
        aes_setiv(aes, iv, sizeof iv);
        aes_encrypt(aes, block, block, sizeof block); /* the block's keystream */
        for (size_t i = 0; i < n; i++) data[i] ^= block[skip + i];
        data += n;
        at += n;
        size -= n;
    }
    free_aes_ctx(aes);
}

/* the parts, AES-CTR ones (crypto 3 and 4) encrypted, over plain: the NCA they make */
static inline void ncz_encrypt(unsigned char* nca, const ncz_part_t* parts, unsigned count) {
    for (unsigned i = 0; i < count; i++)
        if (parts[i].crypto == 3 || parts[i].crypto == 4)
            ncz_ctr(&parts[i], parts[i].offset, nca + parts[i].offset, (size_t)parts[i].size);
}

/* nca's NCZ, parts its section table, in blocks of 1 << block_exponent bytes, or one zstd stream when block_exponent is
   0; blocks, when not NULL, says how the blocks were kept */
static inline blob_t ncz_build(const blob_t* nca, const ncz_part_t* parts, unsigned count, unsigned block_exponent,
                               ncz_blocks_t* blocks) {
    const size_t head = 0x4000, table = head + 0x10 + (size_t)count * 0x40;
    size_t body = nca->size > head ? nca->size - head : 0, block = block_exponent ? (size_t)1 << block_exponent : 0;
    size_t n_blocks = block ? (body + block - 1) / block : 0, at, i, bound = table + 0x18 + 4 * n_blocks;
    unsigned char* plain = (unsigned char*)zalloc(nca->size);
    blob_t out;
    if (!block) bound += ZSTD_compressBound(body);
    for (i = 0; i < n_blocks; i++) bound += ZSTD_compressBound(body - i * block < block ? body - i * block : block);
    out.data = (unsigned char*)zalloc(bound);
    out.size = 0;
    CHECK(body > 0); /* nsz compresses no NCA of 0x4000 bytes or fewer */
    memcpy(plain, nca->data, nca->size);
    for (i = 0; i < count; i++) { /* decrypted as nsz's partitions read them; the part below 0x4000 stays as it is */
        uint64_t start = parts[i].offset < head ? head : parts[i].offset, end = parts[i].offset + parts[i].size;
        if ((parts[i].crypto == 3 || parts[i].crypto == 4) && end > start)
            ncz_ctr(&parts[i], start, plain + start, (size_t)(end - start));
    }
    memcpy(out.data, nca->data, head);
    memcpy(out.data + head, "NCZSECTN", 8);
    put64(out.data + head + 8, count);
    for (i = 0; i < count; i++) {
        unsigned char* e = out.data + head + 0x10 + i * 0x40;
        put64(e, parts[i].offset);
        put64(e + 8, parts[i].size);
        put64(e + 0x10, parts[i].crypto);
        memcpy(e + 0x20, parts[i].key, 16);
        memcpy(e + 0x30, parts[i].counter, 16);
    }
    at = table;
    if (!block) {
        size_t made = ZSTD_compress(out.data + at, bound - at, plain + head, body, 3);
        CHECK(!ZSTD_isError(made));
        at += made;
    } else {
        unsigned char* sizes = out.data + at + 0x18;
        memcpy(out.data + at, "NCZBLOCK", 8);
        out.data[at + 8] = 2; /* version */
        out.data[at + 9] = 1; /* type */
        out.data[at + 11] = (unsigned char)block_exponent;
        put32(out.data + at + 12, (uint32_t)n_blocks);
        put64(out.data + at + 16, body);
        at += 0x18 + 4 * n_blocks;
        for (i = 0; i < n_blocks; i++) {
            size_t holds = body - i * block < block ? body - i * block : block;
            size_t made = ZSTD_compress(out.data + at, bound - at, plain + head + i * block, holds, 3);
            CHECK(!ZSTD_isError(made));
            if (made >= holds) { /* no smaller: kept as it is */
                memcpy(out.data + at, plain + head + i * block, holds);
                made = holds;
                if (blocks) blocks->stored++;
            } else if (blocks)
                blocks->compressed++;
            put32(sizes + 4 * i, (uint32_t)made);
            at += made;
        }
    }
    out.size = at;
    free(plain);
    return out;
}

/* a decrypted NCA's (dnca_build's) NCZ: its sections as its header lists them, none encrypted */
static inline blob_t ncz_of_dnca(const blob_t* nca, unsigned block_exponent) {
    ncz_part_t parts[4];
    unsigned count = 0;
    memset(parts, 0, sizeof parts);
    for (unsigned i = 0; i < 4; i++) {
        const unsigned char* e = nca->data + offsetof(nca_header_t, section_entries) + sizeof(nca_section_entry_t) * i;
        uint64_t start = (uint64_t)(e[0] | e[1] << 8 | e[2] << 16 | (uint32_t)e[3] << 24) * 0x200;
        uint64_t end = (uint64_t)(e[4] | e[5] << 8 | e[6] << 16 | (uint32_t)e[7] << 24) * 0x200;
        if (!start) continue;
        parts[count].offset = start;
        parts[count].size = end - start;
        parts[count++].crypto = 1;
    }
    CHECK(count > 0);
    return ncz_build(nca, parts, count, block_exponent, NULL);
}

#endif
