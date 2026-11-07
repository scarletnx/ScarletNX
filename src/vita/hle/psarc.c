/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "vita/hle/psarc.h"
#include "vita/loader/inflate.h"
#include "common/utilities/common.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t size, offset;
    uint32_t first_block;
} Entry;
struct VitaPsarc {
    FILE* file;
    char compression[5];
    uint32_t block_size, count, toc_length, width, size_count;
    Entry* entries;
    unsigned char* sizes; /* the blocks' stored sizes, width bytes each, big-endian */
    char* manifest;       /* the names, cut at their line ends */
    char** names;         /* of the entries 1 on */
};

static bool read_at(FILE* f, uint64_t at, void* out, size_t n) {
    if (!n) return true;
#ifdef _WIN32
    if (_fseeki64(f, (long long)at, SEEK_SET)) return false;
#else
    if (fseeko(f, (off_t)at, SEEK_SET)) return false;
#endif
    return fread(out, 1, n, f) == n;
}
static uint64_t be(const unsigned char* p, unsigned n) {
    uint64_t v = 0;
    while (n--) v = v << 8 | *p++;
    return v;
}
/* a block's size in the file: 0 in the table is a whole block, stored */
static uint32_t stored_size(const VitaPsarc* a, uint32_t block) {
    uint32_t v = block < a->size_count ? (uint32_t)be(a->sizes + (size_t)block * a->width, a->width) : 0;
    return v ? v : a->block_size;
}
static bool decode_block(const VitaPsarc* a, uint64_t at, uint32_t stored, unsigned char* out, uint32_t raw) {
    if (stored == raw) return read_at(a->file, at, out, raw);
    if (strcmp(a->compression, "zlib")) return false;
    unsigned char* in = malloc(stored);
    bool ok = in && read_at(a->file, at, in, stored) &&
              (vita_zlib_inflate(in, stored, out, raw) || vita_inflate(in, stored, out, raw));
    free(in);
    return ok;
}
bool vita_psarc_read(const VitaPsarc* a, int index, uint64_t offset, void* out, size_t length, size_t* got) {
    const Entry* e = &a->entries[index];
    *got = 0;
    if (offset >= e->size) return true;
    if (length > e->size - offset) length = (size_t)(e->size - offset);
    unsigned char* block = malloc(a->block_size);
    if (!block) return false;
    uint32_t k = (uint32_t)(offset / a->block_size);
    uint64_t at = e->offset;
    for (uint32_t j = 0; j < k; ++j) at += stored_size(a, e->first_block + j);
    size_t skip = (size_t)(offset % a->block_size);
    bool ok = true;
    while (length && ok) {
        uint64_t remaining = e->size - (uint64_t)k * a->block_size;
        uint32_t raw = remaining < a->block_size ? (uint32_t)remaining : a->block_size;
        uint32_t stored = stored_size(a, e->first_block + k);
        ok = decode_block(a, at, stored, block, raw);
        if (!ok) break;
        size_t n = raw - skip < length ? raw - skip : length;
        memcpy((unsigned char*)out + *got, block + skip, n);
        *got += n, length -= n, skip = 0, at += stored, ++k;
    }
    free(block);
    return ok;
}
void vita_psarc_close(VitaPsarc* a) {
    if (!a) return;
    if (a->file) fclose(a->file);
    free(a->entries), free(a->sizes), free(a->manifest), free(a->names), free(a);
}
VitaPsarc* vita_psarc_open(const char* path) {
    FILE* f = snx_fopen(path, "rb");
    unsigned char h[32];
    if (!f) return NULL;
    if (!read_at(f, 0, h, sizeof h) || memcmp(h, "PSAR", 4)) {
        fclose(f);
        return NULL;
    }
    VitaPsarc* a = calloc(1, sizeof *a);
    if (!a) {
        fclose(f);
        return NULL;
    }
    a->file = f;
    memcpy(a->compression, h + 8, 4);
    a->toc_length = (uint32_t)be(h + 12, 4), a->count = (uint32_t)be(h + 20, 4), a->block_size = (uint32_t)be(h + 24, 4);
    uint32_t entry_size = (uint32_t)be(h + 16, 4);
    if (entry_size != 30 || !a->count || !a->block_size || a->toc_length > (1u << 28) ||
        a->toc_length < 32 + (uint64_t)a->count * 30) {
        printf("vita: psarc \"%s\": header not understood (toc %u, entries %u of %u bytes, block %u)\n", path,
               a->toc_length, a->count, entry_size, a->block_size);
        vita_psarc_close(a);
        return NULL;
    }
    a->width = a->block_size <= 0x10000 ? 2 : a->block_size <= 0x1000000 ? 3 : 4;
    size_t toc_size = a->toc_length - 32, table = toc_size - (size_t)a->count * 30;
    unsigned char* toc = malloc(toc_size);
    a->entries = calloc(a->count, sizeof(Entry));
    a->sizes = malloc(table ? table : 1);
    if (!toc || !a->entries || !a->sizes || !read_at(f, 32, toc, toc_size)) {
        free(toc);
        vita_psarc_close(a);
        return NULL;
    }
    for (uint32_t i = 0; i < a->count; ++i) {
        const unsigned char* e = toc + (size_t)i * 30;
        a->entries[i] = (Entry){.first_block = (uint32_t)be(e + 16, 4), .size = be(e + 20, 5), .offset = be(e + 25, 5)};
    }
    memcpy(a->sizes, toc + (size_t)a->count * 30, table);
    a->size_count = (uint32_t)(table / a->width);
    free(toc);
    /* the manifest: entry 0 */
    uint64_t manifest_size = a->entries[0].size;
    size_t got = 0;
    a->manifest = manifest_size < (1u << 28) ? malloc((size_t)manifest_size + 1) : NULL;
    a->names = calloc(a->count, sizeof(char*));
    if (!a->manifest || !a->names || !vita_psarc_read(a, 0, 0, a->manifest, (size_t)manifest_size, &got) ||
        got != manifest_size) {
        printf("vita: psarc \"%s\": manifest not readable (compression \"%s\", %u entries)\n", path, a->compression,
               a->count);
        vita_psarc_close(a);
        return NULL;
    }
    a->manifest[got] = 0;
    char* line = a->manifest;
    for (uint32_t i = 0; i + 1 < a->count && *line; ++i) {
        char* end = strchr(line, '\n');
        size_t n = end ? (size_t)(end - line) : strlen(line);
        if (n && line[n - 1] == '\r') --n;
        char* next = end ? end + 1 : line + n;
        line[n] = 0;
        while (*line == '/') ++line;
        a->names[i] = line;
        line = next;
    }
    printf("vita: psarc \"%s\": %u entries, compression \"%s\", block %u\n", path, a->count - 1, a->compression,
           a->block_size);
    return a;
}
static bool same(const char* x, const char* y) {
    for (; *x && *y; ++x, ++y)
        if (tolower((unsigned char)*x) != tolower((unsigned char)*y)) return false;
    return !*x && !*y;
}
int vita_psarc_find(const VitaPsarc* a, const char* name) {
    while (*name == '/') ++name;
    for (uint32_t i = 0; i + 1 < a->count; ++i)
        if (a->names[i] && same(a->names[i], name)) return (int)i + 1;
    return -1;
}
unsigned vita_psarc_count(const VitaPsarc* a) { return a->count - 1; }
uint32_t vita_psarc_toc_length(const VitaPsarc* a) { return a->toc_length; }
uint64_t vita_psarc_size(const VitaPsarc* a, int entry) { return a->entries[entry].size; }
