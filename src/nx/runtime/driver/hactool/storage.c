/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from LibHac (https://github.com/Thealexbarney/LibHac), as maintained by Ryubing: BucketTree, IndirectStorage,
   SparseStorage, AesCtrCounterExtendedStorage and CompressedStorage, under its license:

   MIT License

   Copyright (c) 2018 Alex Barney

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in all
   copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE. */
#include <stdlib.h>
#include <string.h>
#include "storage.h"
#include "utils.h"
#include "lz4.h"

#define BUCKET_TREE_NODE_HEADER_SIZE 0x10
#define AES_BLOCK 0x10

static uint64_t read_u64(const unsigned char *p) {
    uint64_t value = 0;
    for (unsigned int i = 0; i < 8; i++) value |= (uint64_t)p[i] << (8 * i);
    return value;
}

static uint32_t read_u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void *storage_alloc(size_t size, uint64_t storage_size,
                           int (*read)(storage_t *, uint64_t, void *, uint64_t), void (*destroy)(storage_t *)) {
    storage_t *storage = calloc(1, size);
    if (storage == NULL) {
        fprintf(stderr, "Failed to allocate storage!\n");
        exit(EXIT_FAILURE);
    }
    storage->read = read;
    storage->destroy = destroy;
    storage->size = storage_size;
    return storage;
}

int storage_read(storage_t *storage, uint64_t offset, void *buffer, uint64_t size) {
    if (offset > storage->size || size > storage->size - offset) {
        fprintf(stderr, "Storage read out of range (0x%"PRIx64"+0x%"PRIx64" of 0x%"PRIx64")!\n", offset, size, storage->size);
        return 0;
    }
    return size == 0 || storage->read(storage, offset, buffer, size);
}

void storage_free(storage_t *storage) {
    if (storage == NULL) return;
    if (storage->destroy) storage->destroy(storage);
    free(storage);
}

/* Bucket trees. A table is node storage (the L1 node, then any L2 nodes) and entry storage (entry sets of one node
   each). Every entry set starts with a node header, and the sets together hold the entries in offset order. */

typedef struct {
    int32_t index;
    int32_t count;
    int64_t offset_end;
} bucket_tree_node_header_t;

static uint32_t bucket_tree_entries_per_node(uint64_t entry_size) {
    return (uint32_t)((BUCKET_TREE_NODE_SIZE - BUCKET_TREE_NODE_HEADER_SIZE) / entry_size);
}

static uint32_t bucket_tree_offsets_per_node(void) {
    return (BUCKET_TREE_NODE_SIZE - BUCKET_TREE_NODE_HEADER_SIZE) / sizeof(uint64_t);
}

static uint32_t bucket_tree_entry_set_count(uint64_t entry_size, uint32_t entry_count) {
    uint32_t per_node = bucket_tree_entries_per_node(entry_size);
    return (entry_count + per_node - 1) / per_node;
}

static uint32_t bucket_tree_node_l2_count(uint64_t entry_size, uint32_t entry_count) {
    uint32_t offsets = bucket_tree_offsets_per_node();
    uint32_t sets = bucket_tree_entry_set_count(entry_size, entry_count);
    if (sets <= offsets) return 0;
    uint32_t l2 = (sets + offsets - 1) / offsets;
    /* LibHac aborts here: a tree this big would need a third level. */
    if (l2 > offsets) return UINT32_MAX;
    return (sets - (offsets - (l2 - 1)) + offsets - 1) / offsets;
}

int bucket_tree_header_verify(const bucket_tree_header_t *header) {
    if (header->magic != BUCKET_TREE_MAGIC) {
        fprintf(stderr, "Invalid bucket tree signature!\n");
        return 0;
    }
    if (header->entry_count < 0) {
        fprintf(stderr, "Invalid bucket tree entry count!\n");
        return 0;
    }
    if (header->version > 1) {
        fprintf(stderr, "Unsupported bucket tree version %"PRIu32"!\n", header->version);
        return 0;
    }
    return 1;
}

uint64_t bucket_tree_query_node_storage_size(uint64_t entry_size, uint32_t entry_count) {
    if (entry_count == 0) return 0;
    uint32_t l2 = bucket_tree_node_l2_count(entry_size, entry_count);
    if (l2 == UINT32_MAX) return UINT64_MAX;
    return (1 + (uint64_t)l2) * BUCKET_TREE_NODE_SIZE;
}

uint64_t bucket_tree_query_entry_storage_size(uint64_t entry_size, uint32_t entry_count) {
    if (entry_count == 0) return 0;
    return (uint64_t)bucket_tree_entry_set_count(entry_size, entry_count) * BUCKET_TREE_NODE_SIZE;
}

static int bucket_tree_node_header_verify(const unsigned char *node, int32_t index, uint64_t entry_size, bucket_tree_node_header_t *out) {
    out->index = (int32_t)read_u32(node);
    out->count = (int32_t)read_u32(node + 4);
    out->offset_end = (int64_t)read_u64(node + 8);
    if (out->index != index) {
        fprintf(stderr, "Invalid bucket tree node index (%"PRId32", expected %"PRId32")!\n", out->index, index);
        return 0;
    }
    if (out->count <= 0 || (uint32_t)out->count > bucket_tree_entries_per_node(entry_size)) {
        fprintf(stderr, "Invalid bucket tree node entry count!\n");
        return 0;
    }
    if (out->offset_end < 0) {
        fprintf(stderr, "Invalid bucket tree node offset!\n");
        return 0;
    }
    return 1;
}

bucket_tree_t *bucket_tree_empty(uint64_t end_offset) {
    bucket_tree_t *tree = calloc(1, sizeof(*tree));
    if (tree == NULL) {
        fprintf(stderr, "Failed to allocate bucket tree!\n");
        exit(EXIT_FAILURE);
    }
    tree->end_offset = end_offset;
    return tree;
}

void bucket_tree_free(bucket_tree_t *tree) {
    if (tree == NULL) return;
    free(tree->entries);
    free(tree);
}

bucket_tree_t *bucket_tree_load(storage_t *table, uint64_t offset, uint64_t size, uint64_t entry_size, uint32_t entry_count) {
    if (entry_count == 0 || entry_count > INT32_MAX) {
        fprintf(stderr, "Invalid bucket tree entry count!\n");
        return NULL;
    }
    uint64_t node_size = bucket_tree_query_node_storage_size(entry_size, entry_count);
    uint64_t entry_storage_size = bucket_tree_query_entry_storage_size(entry_size, entry_count);
    if (node_size == UINT64_MAX || node_size > size || entry_storage_size > size - node_size) {
        fprintf(stderr, "Bucket tree of %"PRIu32" entries does not fit its table (0x%"PRIx64" bytes)!\n", entry_count, size);
        return NULL;
    }

    unsigned char *node = malloc(BUCKET_TREE_NODE_SIZE);
    bucket_tree_t *tree = calloc(1, sizeof(*tree));
    if (node == NULL || tree == NULL || (tree->entries = malloc((size_t)entry_size * entry_count)) == NULL) {
        fprintf(stderr, "Failed to allocate bucket tree!\n");
        exit(EXIT_FAILURE);
    }
    tree->entry_size = entry_size;
    tree->entry_count = entry_count;

    /* The L1 node gives the end of the tree; the entry sets give everything else. */
    bucket_tree_node_header_t header;
    if (!storage_read(table, offset, node, BUCKET_TREE_NODE_SIZE)) goto fail;
    if (!bucket_tree_node_header_verify(node, 0, sizeof(uint64_t), &header)) goto fail;
    tree->end_offset = (uint64_t)header.offset_end;

    uint32_t sets = bucket_tree_entry_set_count(entry_size, entry_count);
    uint32_t loaded = 0;
    uint64_t previous = 0;
    for (uint32_t i = 0; i < sets; i++) {
        if (!storage_read(table, offset + node_size + (uint64_t)i * BUCKET_TREE_NODE_SIZE, node, BUCKET_TREE_NODE_SIZE)) goto fail;
        if (!bucket_tree_node_header_verify(node, (int32_t)i, entry_size, &header)) goto fail;
        if ((uint32_t)header.count > entry_count - loaded) {
            fprintf(stderr, "Bucket tree holds more entries than its header says!\n");
            goto fail;
        }
        unsigned char *entries = tree->entries + (size_t)loaded * entry_size;
        memcpy(entries, node + BUCKET_TREE_NODE_HEADER_SIZE, (size_t)header.count * entry_size);
        for (int32_t j = 0; j < header.count; j++) {
            uint64_t entry_offset = read_u64(entries + (size_t)j * entry_size);
            /* The tree starts at 0, and its offsets only grow. */
            if ((loaded == 0 && j == 0) ? entry_offset != 0 : entry_offset < previous) {
                fprintf(stderr, "Invalid bucket tree entry offset!\n");
                goto fail;
            }
            previous = entry_offset;
        }
        if ((uint64_t)header.offset_end < previous || (uint64_t)header.offset_end > tree->end_offset) {
            fprintf(stderr, "Invalid bucket tree entry set end offset!\n");
            goto fail;
        }
        loaded += (uint32_t)header.count;
    }
    if (loaded != entry_count || previous >= tree->end_offset) {
        fprintf(stderr, "Bucket tree is inconsistent with its header!\n");
        goto fail;
    }
    free(node);
    return tree;

fail:
    free(node);
    bucket_tree_free(tree);
    return NULL;
}

static const unsigned char *bucket_tree_entry(const bucket_tree_t *tree, uint32_t i) {
    return tree->entries + (size_t)i * tree->entry_size;
}

static uint64_t bucket_tree_entry_offset(const bucket_tree_t *tree, uint32_t i) {
    return read_u64(bucket_tree_entry(tree, i));
}

/* Where entry i ends: the next entry's offset, or the tree's end. */
static uint64_t bucket_tree_entry_end(const bucket_tree_t *tree, uint32_t i) {
    return i + 1 < tree->entry_count ? bucket_tree_entry_offset(tree, i + 1) : tree->end_offset;
}

/* The last entry starting at or before offset, which must be inside the tree. */
static uint32_t bucket_tree_find(const bucket_tree_t *tree, uint64_t offset) {
    uint32_t low = 0, high = tree->entry_count;
    while (high - low > 1) {
        uint32_t mid = low + (high - low) / 2;
        if (bucket_tree_entry_offset(tree, mid) <= offset) {
            low = mid;
        } else {
            high = mid;
        }
    }
    return low;
}

/* Plain storages. */

typedef struct {
    storage_t base;
    FILE *file;
    uint64_t offset;
} file_storage_t;

static int file_storage_read(storage_t *storage, uint64_t offset, void *buffer, uint64_t size) {
    file_storage_t *s = (file_storage_t *)storage;
    if (fseeko64(s->file, s->offset + offset, SEEK_SET) != 0 || fread(buffer, 1, size, s->file) != size) {
        fprintf(stderr, "Failed to read 0x%"PRIx64" bytes at 0x%"PRIx64"!\n", size, s->offset + offset);
        return 0;
    }
    return 1;
}

storage_t *storage_file_new(FILE *file, uint64_t offset, uint64_t size) {
    file_storage_t *s = storage_alloc(sizeof(*s), size, file_storage_read, NULL);
    s->file = file;
    s->offset = offset;
    return &s->base;
}

/* ScarletNX: a container mapped into memory, which several threads can read at once (no file position) */
typedef struct {
    storage_t base;
    const unsigned char *data;
} memory_storage_t;

static int memory_storage_read(storage_t *storage, uint64_t offset, void *buffer, uint64_t size) {
    memcpy(buffer, ((memory_storage_t *)storage)->data + offset, size);
    return 1;
}

storage_t *storage_memory_new(const unsigned char *data, uint64_t data_size, uint64_t offset, uint64_t size) {
    if (offset > data_size || size > data_size - offset) {
        fprintf(stderr, "0x%"PRIx64"+0x%"PRIx64" is outside the mapped container (0x%"PRIx64")!\n", offset, size, data_size);
        return NULL;
    }
    memory_storage_t *s = storage_alloc(sizeof(*s), size, memory_storage_read, NULL);
    s->data = data + offset;
    return &s->base;
}

typedef struct {
    storage_t base;
    storage_t *inner;
    uint64_t offset;
} sub_storage_t;

static int sub_storage_read(storage_t *storage, uint64_t offset, void *buffer, uint64_t size) {
    sub_storage_t *s = (sub_storage_t *)storage;
    return storage_read(s->inner, s->offset + offset, buffer, size);
}

storage_t *storage_sub_new(storage_t *base, uint64_t offset, uint64_t size) {
    if (offset > base->size || size > base->size - offset) {
        fprintf(stderr, "Sub-storage 0x%"PRIx64"+0x%"PRIx64" is outside its storage (0x%"PRIx64")!\n", offset, size, base->size);
        return NULL;
    }
    sub_storage_t *s = storage_alloc(sizeof(*s), size, sub_storage_read, NULL);
    s->inner = base;
    s->offset = offset;
    return &s->base;
}

typedef struct {
    storage_t base;
    unsigned char value;
} fill_storage_t;

static int fill_storage_read(storage_t *storage, uint64_t offset, void *buffer, uint64_t size) {
    (void)offset;
    memset(buffer, ((fill_storage_t *)storage)->value, size);
    return 1;
}

storage_t *storage_fill_new(uint64_t size, unsigned char value) {
    fill_storage_t *s = storage_alloc(sizeof(*s), size, fill_storage_read, NULL);
    s->value = value;
    return &s->base;
}

typedef struct {
    storage_t base;
    storage_t *low;
    storage_t *high;
    uint64_t split;
} switch_storage_t;

static int switch_storage_read(storage_t *storage, uint64_t offset, void *buffer, uint64_t size) {
    switch_storage_t *s = (switch_storage_t *)storage;
    if (offset < s->split) {
        uint64_t low_size = s->split - offset < size ? s->split - offset : size;
        if (!storage_read(s->low, offset, buffer, low_size)) return 0;
        buffer = (unsigned char *)buffer + low_size;
        offset += low_size;
        size -= low_size;
    }
    return size == 0 || storage_read(s->high, offset - s->split, buffer, size);
}

storage_t *storage_switch_new(storage_t *low, uint64_t split, storage_t *high) {
    if (split > low->size) {
        fprintf(stderr, "Storage split 0x%"PRIx64" is past its storage (0x%"PRIx64")!\n", split, low->size);
        return NULL;
    }
    switch_storage_t *s = storage_alloc(sizeof(*s), split + high->size, switch_storage_read, NULL);
    s->low = low;
    s->high = high;
    s->split = split;
    return &s->base;
}

/* AES. */

static void aes_ctr_make_iv(unsigned char iv[AES_BLOCK], const unsigned char upper_iv[8], uint64_t offset) {
    memcpy(iv, upper_iv, 8);
    offset /= AES_BLOCK;
    for (unsigned int i = 0; i < 8; i++) {
        iv[AES_BLOCK - 1 - i] = (unsigned char)(offset & 0xFF);
        offset >>= 8;
    }
}

/* Decrypts base [offset, offset + size) in AES-CTR, the counter of byte x being that of counter_offset + x, which is
   block-aligned (aes_ctr_check). */
static int aes_ctr_read(storage_t *base, aes_ctx_t *aes, const unsigned char upper_iv[8], uint64_t counter_offset,
                        uint64_t offset, unsigned char *buffer, uint64_t size) {
    unsigned char iv[AES_BLOCK];
    uint64_t skip = offset % AES_BLOCK;
    if (skip != 0) {
        /* The first block is partial: decrypt it whole, as far as the storage goes. */
        unsigned char block[AES_BLOCK];
        uint64_t available = base->size - (offset - skip);
        uint64_t length = available < AES_BLOCK ? available : AES_BLOCK;
        uint64_t used = length - skip < size ? length - skip : size;
        if (!storage_read(base, offset - skip, block, length)) return 0;
        aes_ctr_make_iv(iv, upper_iv, counter_offset + offset - skip);
        aes_setiv(aes, iv, AES_BLOCK);
        aes_decrypt(aes, block, block, length);
        memcpy(buffer, block + skip, used);
        buffer += used;
        offset += used;
        size -= used;
    }
    if (size == 0) return 1;
    if (!storage_read(base, offset, buffer, size)) return 0;
    aes_ctr_make_iv(iv, upper_iv, counter_offset + offset);
    aes_setiv(aes, iv, AES_BLOCK);
    aes_decrypt(aes, buffer, buffer, size);
    return 1;
}

static void aes_ctr_check(uint64_t counter_offset) {
    if (counter_offset % AES_BLOCK != 0) {
        fprintf(stderr, "AES-CTR counter offset 0x%"PRIx64" is not block-aligned!\n", counter_offset);
        exit(EXIT_FAILURE);
    }
}

typedef struct {
    storage_t base;
    storage_t *inner;
    aes_ctx_t *aes;
    unsigned char upper_iv[8];
    uint64_t counter_offset;
} aes_ctr_storage_t;

static int aes_ctr_storage_read(storage_t *storage, uint64_t offset, void *buffer, uint64_t size) {
    aes_ctr_storage_t *s = (aes_ctr_storage_t *)storage;
    return aes_ctr_read(s->inner, s->aes, s->upper_iv, s->counter_offset, offset, buffer, size);
}

storage_t *storage_aes_ctr_new(storage_t *base, aes_ctx_t *aes, const unsigned char upper_iv[8], uint64_t counter_offset) {
    aes_ctr_check(counter_offset);
    aes_ctr_storage_t *s = storage_alloc(sizeof(*s), base->size, aes_ctr_storage_read, NULL);
    s->inner = base;
    s->aes = aes;
    memcpy(s->upper_iv, upper_iv, 8);
    s->counter_offset = counter_offset;
    return &s->base;
}

typedef struct {
    storage_t base;
    storage_t *inner;
    aes_ctx_t *aes;
    uint64_t sector_size;
    uint64_t first_sector;
} aes_xts_storage_t;

#define XTS_CHUNK 0x100000 /* a multiple of every sector size */

static int aes_xts_storage_read(storage_t *storage, uint64_t offset, void *buffer, uint64_t size) {
    aes_xts_storage_t *s = (aes_xts_storage_t *)storage;
    unsigned char *out = buffer;
    uint64_t span = (offset % s->sector_size + size + s->sector_size - 1) / s->sector_size * s->sector_size;
    unsigned char *chunk = malloc(span < XTS_CHUNK ? span : XTS_CHUNK);
    if (chunk == NULL) {
        fprintf(stderr, "Failed to allocate XTS buffer!\n");
        exit(EXIT_FAILURE);
    }
    int ok = 1;
    while (ok && size > 0) {
        uint64_t sector = offset / s->sector_size;
        uint64_t start = sector * s->sector_size;
        uint64_t skip = offset - start;
        /* Whole sectors, at most XTS_CHUNK bytes of them. */
        uint64_t want = skip + size < XTS_CHUNK ? skip + size : XTS_CHUNK;
        uint64_t length = (want + s->sector_size - 1) / s->sector_size * s->sector_size;
        if (length > s->inner->size - start) {
            fprintf(stderr, "XTS read past the end of its sectors!\n");
            ok = 0;
            break;
        }
        ok = storage_read(s->inner, start, chunk, length);
        if (!ok) break;
        aes_xts_decrypt(s->aes, chunk, chunk, length, s->first_sector + sector, s->sector_size);
        uint64_t used = want - skip;
        memcpy(out, chunk + skip, used);
        out += used;
        offset += used;
        size -= used;
    }
    free(chunk);
    return ok;
}

storage_t *storage_aes_xts_new(storage_t *base, aes_ctx_t *aes, uint64_t sector_size, uint64_t first_sector) {
    aes_xts_storage_t *s = storage_alloc(sizeof(*s), base->size, aes_xts_storage_read, NULL);
    s->inner = base;
    s->aes = aes;
    s->sector_size = sector_size;
    s->first_sector = first_sector;
    return &s->base;
}

typedef struct {
    storage_t base;
    bucket_tree_t *tree;
} tree_storage_t;

static void tree_storage_destroy(storage_t *storage) {
    /* Every storage with a tree keeps it right after its storage_t. */
    bucket_tree_free(((tree_storage_t *)storage)->tree);
}

/* AES-CTR with a counter per entry: an update's section is encrypted in runs, each with its own generation in the
   upper counter, or in none (LibHac's FsSystem AesCtrCounterExtendedStorage; its Tools copy ignores the flag). Past the
   tree's end, the bytes before the patch tables take the section's own counter. */
typedef struct {
    storage_t base;
    bucket_tree_t *tree;
    storage_t *inner;
    aes_ctx_t *aes;
    unsigned char upper_iv[8];
    uint64_t counter_offset;
} aes_ctr_ex_storage_t;

static int aes_ctr_ex_storage_read(storage_t *storage, uint64_t offset, void *buffer, uint64_t size) {
    aes_ctr_ex_storage_t *s = (aes_ctr_ex_storage_t *)storage;
    unsigned char *out = buffer;
    while (size > 0) {
        unsigned char upper_iv[8];
        memcpy(upper_iv, s->upper_iv, 8);
        uint64_t end = s->base.size;
        int encrypted = 1;
        if (offset < s->tree->end_offset) {
            uint32_t i = bucket_tree_find(s->tree, offset);
            const unsigned char *entry = bucket_tree_entry(s->tree, i);
            uint32_t generation = read_u32(entry + 0xC);
            end = bucket_tree_entry_end(s->tree, i);
            if (read_u64(entry) % AES_BLOCK != 0 || end % AES_BLOCK != 0) {
                fprintf(stderr, "Invalid AesCtrEx entry offset!\n");
                return 0;
            }
            encrypted = entry[8] == 0;
            upper_iv[4] = (unsigned char)(generation >> 24);
            upper_iv[5] = (unsigned char)(generation >> 16);
            upper_iv[6] = (unsigned char)(generation >> 8);
            upper_iv[7] = (unsigned char)generation;
        }
        uint64_t length = end - offset < size ? end - offset : size;
        if (encrypted) {
            if (!aes_ctr_read(s->inner, s->aes, upper_iv, s->counter_offset, offset, out, length)) return 0;
        } else if (!storage_read(s->inner, offset, out, length)) {
            return 0;
        }
        out += length;
        offset += length;
        size -= length;
    }
    return 1;
}

storage_t *storage_aes_ctr_ex_new(storage_t *base, aes_ctx_t *aes, const unsigned char upper_iv[8], uint64_t counter_offset, bucket_tree_t *tree) {
    aes_ctr_check(counter_offset);
    aes_ctr_ex_storage_t *s = storage_alloc(sizeof(*s), base->size, aes_ctr_ex_storage_read, tree_storage_destroy);
    s->inner = base;
    s->aes = aes;
    memcpy(s->upper_iv, upper_iv, 8);
    s->counter_offset = counter_offset;
    s->tree = tree;
    return &s->base;
}

/* Indirect storage: each entry maps a run of the virtual storage to the original (0) or the patch (1). A sparse
   storage is one whose "patch" reads as zeros. */
typedef struct {
    storage_t base;
    bucket_tree_t *tree;
    storage_t *storages[2];
} indirect_storage_t;

static int indirect_storage_read(storage_t *storage, uint64_t offset, void *buffer, uint64_t size) {
    indirect_storage_t *s = (indirect_storage_t *)storage;
    unsigned char *out = buffer;
    if (s->tree->entry_count == 0) {
        /* LibHac's SparseStorage of no entries reads as zeros. */
        memset(out, 0, size);
        return 1;
    }
    while (size > 0) {
        uint32_t i = bucket_tree_find(s->tree, offset);
        const unsigned char *entry = bucket_tree_entry(s->tree, i);
        uint64_t virtual_offset = read_u64(entry);
        uint64_t physical_offset = read_u64(entry + 8);
        uint32_t index = read_u32(entry + 0x10);
        uint64_t end = bucket_tree_entry_end(s->tree, i);
        if (index > 1 || offset >= end) {
            fprintf(stderr, "Invalid indirect storage entry!\n");
            return 0;
        }
        uint64_t length = end - offset < size ? end - offset : size;
        if (!storage_read(s->storages[index], physical_offset + (offset - virtual_offset), out, length)) return 0;
        out += length;
        offset += length;
        size -= length;
    }
    return 1;
}

storage_t *storage_indirect_new(storage_t *original, storage_t *patch, bucket_tree_t *tree) {
    indirect_storage_t *s = storage_alloc(sizeof(*s), tree->end_offset, indirect_storage_read, tree_storage_destroy);
    s->tree = tree;
    s->storages[0] = original;
    s->storages[1] = patch;
    return &s->base;
}

int storage_indirect_has_patch_data(storage_t *indirect, uint64_t offset, uint64_t size) {
    indirect_storage_t *s = (indirect_storage_t *)indirect;
    if (s->tree->entry_count == 0 || offset >= s->tree->end_offset) return 0;
    for (uint32_t i = bucket_tree_find(s->tree, offset); i < s->tree->entry_count; i++) {
        if (bucket_tree_entry_offset(s->tree, i) >= offset + size) break;
        if (read_u32(bucket_tree_entry(s->tree, i) + 0x10) == 1) return 1;
    }
    return 0;
}

/* Compressed storage: each entry is a block stored as is, as zeros, or in LZ4. The last block decompressed is kept, as
   hactool reads RomFS tables and files in pieces. */
typedef struct {
    storage_t base;
    bucket_tree_t *tree;
    storage_t *data;
    unsigned char *cache;
    uint64_t cache_capacity;
    uint32_t cache_entry;
    int cache_valid;
} compressed_storage_t;

static void compressed_storage_destroy(storage_t *storage) {
    compressed_storage_t *s = (compressed_storage_t *)storage;
    free(s->cache);
    bucket_tree_free(s->tree);
}

/* Each read failure says the entry, its kind, the image bytes it gives and the stored bytes it takes, so that a
   game's log tells a damaged block from a table this port reads otherwise than the console. */
static int compressed_storage_block(compressed_storage_t *s, uint32_t i, uint64_t block_size) {
    if (s->cache_valid && s->cache_entry == i) return 1;
    const unsigned char *entry = bucket_tree_entry(s->tree, i);
    uint64_t virtual_offset = read_u64(entry);
    uint64_t physical_offset = read_u64(entry + 8);
    uint32_t physical_size = read_u32(entry + 0x14);
    if (block_size > LZ4_MAX_INPUT_SIZE || physical_size > LZ4_MAX_INPUT_SIZE) {
        fprintf(stderr, "Compressed storage entry %"PRIu32" (LZ4, image 0x%"PRIx64"+0x%"PRIx64", stored at 0x%"PRIx64"+0x%"PRIx32") is past LZ4's 0x%x bytes!\n",
                i, virtual_offset, block_size, physical_offset, physical_size, (unsigned int)LZ4_MAX_INPUT_SIZE);
        return 0;
    }
    if (physical_offset > s->data->size || physical_size > s->data->size - physical_offset) {
        fprintf(stderr, "Compressed storage entry %"PRIu32" (LZ4, image 0x%"PRIx64"+0x%"PRIx64") is stored at 0x%"PRIx64"+0x%"PRIx32", past the stored data's 0x%"PRIx64" bytes!\n",
                i, virtual_offset, block_size, physical_offset, physical_size, s->data->size);
        return 0;
    }
    if (s->cache_capacity < block_size + physical_size) {
        free(s->cache);
        s->cache_capacity = block_size + physical_size;
        if ((s->cache = malloc(s->cache_capacity)) == NULL) {
            fprintf(stderr, "Failed to allocate decompression buffer!\n");
            exit(EXIT_FAILURE);
        }
    }
    s->cache_valid = 0;
    unsigned char *compressed = s->cache + block_size;
    if (!storage_read(s->data, physical_offset, compressed, physical_size)) return 0;
    int decoded = LZ4_decompress_safe((const char *)compressed, (char *)s->cache, (int)physical_size, (int)block_size);
    if (decoded != (int)block_size) {
        /* A negative count is LZ4's error, at that many bytes into the stored block. */
        fprintf(stderr, "Compressed storage entry %"PRIu32" (LZ4, image 0x%"PRIx64"+0x%"PRIx64", stored at 0x%"PRIx64"+0x%"PRIx32"): LZ4_decompress_safe returns %d!\n",
                i, virtual_offset, block_size, physical_offset, physical_size, decoded);
        return 0;
    }
    s->cache_entry = i;
    s->cache_valid = 1;
    return 1;
}

static int compressed_storage_read(storage_t *storage, uint64_t offset, void *buffer, uint64_t size) {
    compressed_storage_t *s = (compressed_storage_t *)storage;
    unsigned char *out = buffer;
    while (size > 0) {
        uint32_t i = bucket_tree_find(s->tree, offset);
        const unsigned char *entry = bucket_tree_entry(s->tree, i);
        uint64_t virtual_offset = read_u64(entry);
        uint64_t physical_offset = read_u64(entry + 8);
        uint64_t end = bucket_tree_entry_end(s->tree, i);
        if (offset >= end) {
            fprintf(stderr, "Compressed storage entry %"PRIu32" (image 0x%"PRIx64" to 0x%"PRIx64") does not hold 0x%"PRIx64"!\n",
                    i, virtual_offset, end, offset);
            return 0;
        }
        uint64_t within = offset - virtual_offset;
        uint64_t length = end - offset < size ? end - offset : size;
        switch (entry[0x10]) {
            case COMPRESSION_NONE:
                if (physical_offset > s->data->size || within > s->data->size - physical_offset ||
                    length > s->data->size - physical_offset - within) {
                    fprintf(stderr, "Compressed storage entry %"PRIu32" (NONE, image 0x%"PRIx64"+0x%"PRIx64", stored at 0x%"PRIx64") reads stored bytes 0x%"PRIx64"+0x%"PRIx64", past the stored data's 0x%"PRIx64"!\n",
                            i, virtual_offset, end - virtual_offset, physical_offset, physical_offset + within, length, s->data->size);
                    return 0;
                }
                if (!storage_read(s->data, physical_offset + within, out, length)) return 0;
                break;
            case COMPRESSION_ZEROED:
                memset(out, 0, length);
                break;
            case COMPRESSION_LZ4:
                if (!compressed_storage_block(s, i, end - virtual_offset)) return 0;
                memcpy(out, s->cache + within, length);
                break;
            default:
                fprintf(stderr, "Compressed storage entry %"PRIu32" (image 0x%"PRIx64"+0x%"PRIx64", stored at 0x%"PRIx64"+0x%"PRIx32") has compression type %"PRIu8", not NONE (0), ZEROED (1) or LZ4 (3)!\n",
                        i, virtual_offset, end - virtual_offset, physical_offset, read_u32(entry + 0x14), entry[0x10]);
                return 0;
        }
        out += length;
        offset += length;
        size -= length;
    }
    return 1;
}

storage_t *storage_compressed_new(storage_t *data, bucket_tree_t *tree) {
    compressed_storage_t *s = storage_alloc(sizeof(*s), tree->end_offset, compressed_storage_read, compressed_storage_destroy);
    s->tree = tree;
    s->data = data;
    return &s->base;
}
