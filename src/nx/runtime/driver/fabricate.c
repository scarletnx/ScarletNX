/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An extracted game made into fabricated plain NCAs (fabricate.h). The layouts are Switchbrew's ("NCA", "RomFS",
   "PFS0", "CNMT", "NACP"), as hactool (nca.c) reads them:

   - an NCA3 header of 0xC00 bytes, plain (its padding at 0x340 zero, which tells a reader it is not encrypted), then
     its sections, each a multiple of 0x200, each FS header saying it is not encrypted;
   - an ExeFS or a content meta as a PFS0 after its table of SHA-256 hashes, a hash a block;
   - a RomFS image (common/filesystem/romfs_layout.h, the runtime's own layout) under an IVFC tree of six levels:
     levels 0 to 4 hash the level after them a 0x4000-byte block at a time, the last block padded with zeros, before
     level 5, the image. The FS header's master hash is level 0's block's.

   Each section's hashes lie before its data, so they are written as zeros, the data streamed through the hashing
   once, and the hashes kept as patches; so are the NCA headers, which hold the sections' master hashes */
#include "nx/runtime/driver/fabricate.h"
#include "nx/runtime/driver/container.h" /* snx_size_text */
#include "common/filesystem/romfs_layout.h"
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"
#include "nx/loader/loader.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NCA_HEADER  0xC00u
#define MEDIA       0x200u   /* the unit of an NCA's section table */
#define EXEFS_BLOCK 0x10000u /* what an ExeFS's hash table hashes a block at a time */
#define META_BLOCK  0x1000u
#define IVFC_LEVELS 6
#define IVFC_LOG2   14
#define IVFC_BLOCK  (1u << IVFC_LOG2)
#define CHUNK       0x400000u
#define NACP_SIZE   0x4000u
#define STUB_SAVE   0x4000000u /* 64 MiB: the stub control.nacp's account and device saves, and their journals */
#define CNMT_SIZE   (0x30u + 2u * 0x38u + 0x20u)

enum { CONTENT_PROGRAM = 0, CONTENT_META = 1, CONTENT_CONTROL = 2 };       /* the NCA header's */
enum { RECORD_PROGRAM = 1, RECORD_CONTROL = 3 };                           /* the content meta's */
enum { NCA_PROGRAM, NCA_CONTROL, NCA_META, NCAS };

/* ns's ApplicationLanguage, the order of control.nacp's titles and the names of the Control NCA's icons */
static const char* const languages[16] = {
    "AmericanEnglish", "BritishEnglish", "Japanese",   "French",     "German",           "LatinAmericanSpanish",
    "Spanish",         "Italian",        "Dutch",      "CanadianFrench", "Portuguese",   "Russian",
    "Korean",          "TraditionalChinese", "SimplifiedChinese", "BrazilianPortuguese"};

/* a file or a folder: a file is a path on disk or bytes in memory (owned) */
typedef struct FabNode {
    char* name;
    struct FabNode *children, *next;
    char* path;
    unsigned char* data;
    SnxU64 size;
    int is_dir;
} FabNode;

typedef struct {
    int romfs;
    FabNode* root; /* a PFS0's files are its children, in order */
    unsigned block;
    /* a PFS0 */
    unsigned char* pfs_header;
    size_t pfs_header_size;
    SnxU64 table_size;
    /* a RomFS image: its header and tables up to its data, its files in data order with their offsets in it */
    unsigned char* meta;
    SnxU64 meta_size;
    FabNode** files;
    SnxU64* offsets;
    size_t nfiles;
    SnxU64 level_offset[IVFC_LEVELS], level_size[IVFC_LEVELS];
    /* both */
    SnxU64 hash_size; /* the hashes before the data */
    SnxU64 data_size; /* the PFS0, or the RomFS image */
    SnxU64 size;      /* the section, padded to MEDIA */
    unsigned char fs_header[0x200];
} FabSection;

typedef struct {
    int content;
    FabSection sections[2];
    int count;
    SnxU64 size;
    unsigned char header[NCA_HEADER];
    unsigned char hash[32]; /* of its header: what names it */
    char name[48];
} FabNca;

struct SnxFab {
    SnxU64 title;
    FabNca ncas[NCAS];
    FabNode *exefs, *romfs, *control, *meta;
    unsigned char* cnmt; /* the Meta NCA's file, filled in once the others are written */
    SnxFabPatch* patches;
    size_t npatches, cap;
};

/* ------------------------------------------------------------------ small things */

static void sha256(const void* data, size_t size, unsigned char out[32]) {
    unsigned int n = 32;
    EVP_Digest(data, size, out, &n, EVP_sha256(), NULL);
}

static char* text_copy(const char* text) {
    size_t n = strlen(text);
    char* copy = (char*)malloc(n + 1);
    if (copy) memcpy(copy, text, n + 1);
    return copy;
}

static void node_free(FabNode* n) {
    while (n) {
        FabNode* next = n->next;
        node_free(n->children);
        free(n->name);
        free(n->path);
        free(n->data);
        free(n);
        n = next;
    }
}

static FabNode* node_new(const char* name, int is_dir) {
    FabNode* n = (FabNode*)calloc(1, sizeof *n);
    if (n && !(n->name = text_copy(name))) {
        free(n);
        n = NULL;
    }
    if (n) n->is_dir = is_dir;
    return n;
}

/* child put among dir's children in strcmp order, as the runtime's image keeps them */
static void node_add(FabNode* dir, FabNode* child) {
    FabNode** at = &dir->children;
    while (*at && strcmp((*at)->name, child->name) < 0) at = &(*at)->next;
    child->next = *at;
    *at = child;
}

/* a file of bytes (owned from here on, freed when it cannot be added) into dir */
static int node_bytes(FabNode* dir, const char* name, unsigned char* bytes, SnxU64 size) {
    FabNode* n = node_new(name, 0);
    if (!n) {
        free(bytes);
        return snx_error("out of memory");
    }
    n->data = bytes;
    n->size = size;
    node_add(dir, n);
    return 0;
}

/* wget's listing of an FTP folder, which a mirror of one keeps in each folder: no file of the game */
static int left_out(const char* name) { return !strcmp(name, ".listing"); }

static int by_name(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }

/* the files and folders under path into dir, counted: 0, else nonzero said. A folder's listing is sorted once and its
   entries appended in order: a folder of a hundred thousand files must not take a search for each */
static int node_walk(FabNode* dir, const char* path, int deep, size_t* folders, size_t* files, size_t* skipped,
                     SnxU64* bytes) {
    char** names;
    size_t count;
    if (snx_list_dir(path, &names, &count)) return snx_error("cannot list %s", path);
    /* an empty folder's list is NULL, which qsort takes not even for no entries */
    if (count) qsort(names, count, sizeof *names, by_name);
    FabNode** tail = &dir->children;
    while (*tail) tail = &(*tail)->next;
    int failed = 0;
    for (size_t i = 0; i < count && !failed; i++) {
        char* full = snx_path_join(path, names[i]);
        FabNode* n = NULL;
        if (!full)
            failed = snx_error("out of memory");
        else if (left_out(names[i]))
            ++*skipped;
        else if (snx_is_dir(full)) {
            if (!deep) {
                ++*skipped;
            } else if (!(n = node_new(names[i], 1)))
                failed = snx_error("out of memory");
            else {
                *tail = n;
                tail = &n->next;
                ++*folders;
                failed = node_walk(n, full, deep, folders, files, skipped, bytes);
            }
        } else if (snx_is_file(full)) {
            if (!(n = node_new(names[i], 0)))
                failed = snx_error("out of memory");
            else {
                n->path = full;
                full = NULL;
                n->size = snx_file_size(n->path);
                *tail = n;
                tail = &n->next;
                ++*files;
                *bytes += n->size;
            }
        }
        free(full);
    }
    snx_free_strings(names, count);
    return failed;
}

/* ------------------------------------------------------------------ partitions */

unsigned char* snx_partition_header(const char* const* names, const SnxU64* sizes, const uint32_t* hashed,
                                    const unsigned char (*hashes)[32], size_t count, int hfs0, SnxU64 align,
                                    size_t* size) {
    size_t strings = 0, entry = hfs0 ? 0x40 : 0x18;
    for (size_t i = 0; i < count; i++) strings += strlen(names[i]) + 1;
    size_t total = (size_t)snx_align_up(0x10 + count * entry + strings, align);
    unsigned char* h = (unsigned char*)calloc(1, total);
    if (!h) return NULL;
    memcpy(h, hfs0 ? "HFS0" : "PFS0", 4);
    snx_st_le32(h + 4, (uint32_t)count);
    snx_st_le32(h + 8, (uint32_t)(total - 0x10 - count * entry));
    SnxU64 at = 0;
    size_t name_at = 0;
    for (size_t i = 0; i < count; i++) {
        unsigned char* e = h + 0x10 + i * entry;
        snx_st_le64(e, at);
        snx_st_le64(e + 8, sizes[i]);
        snx_st_le32(e + 16, (uint32_t)name_at);
        if (hfs0 && hashed) {
            snx_st_le32(e + 20, hashed[i]);
            memcpy(e + 32, hashes[i], 32);
        }
        size_t n = strlen(names[i]) + 1;
        memcpy(h + 0x10 + count * entry + name_at, names[i], n);
        name_at += n;
        at += sizes[i];
    }
    *size = total;
    return h;
}

/* a PFS0 section of root's files, hashed block bytes at a time: its layout */
static int pfs_plan(FabSection* s, FabNode* root, unsigned block) {
    size_t count = 0;
    for (FabNode* n = root->children; n; n = n->next) count++;
    const char** names = (const char**)calloc(count ? count : 1, sizeof *names);
    SnxU64* sizes = (SnxU64*)calloc(count ? count : 1, sizeof *sizes);
    if (!names || !sizes) {
        free(names);
        free(sizes);
        return snx_error("out of memory");
    }
    size_t i = 0;
    SnxU64 files = 0;
    for (FabNode* n = root->children; n; n = n->next, i++) {
        names[i] = n->name;
        sizes[i] = n->size;
        files += n->size;
    }
    s->romfs = 0;
    s->root = root;
    s->block = block;
    s->pfs_header = snx_partition_header(names, sizes, NULL, NULL, count, 0, 0x20, &s->pfs_header_size);
    free(names);
    free(sizes);
    if (!s->pfs_header) return snx_error("out of memory");
    s->data_size = s->pfs_header_size + files;
    s->table_size = snx_div_up(s->data_size, block) * 32;
    s->hash_size = snx_align_up(s->table_size, MEDIA);
    s->size = snx_align_up(s->hash_size + s->data_size, MEDIA);
    return 0;
}

/* ------------------------------------------------------------------ RomFS */

/* a folder while the image is laid out */
typedef struct {
    const FabNode* node;
    uint32_t parent, off, child, file;
} RomDir;

/* the RomFS image of the tree under root, as the runtime's romfs_build lays out romfs/ (nx/hle/filesystem/fs.c): the
   folders breadth first, each folder's files in strcmp order, the data in the file table's order a multiple of 16
   apart; then its IVFC levels */
static int romfs_plan(FabSection* s, FabNode* root) {
    uint32_t ndirs = 1, nfiles = 0;
    SnxU64 dir_bytes = SNX_ROMFS_DIR_ENTRY, file_bytes = 0;
    /* count, breadth first, through a queue of the folders */
    size_t cap = 64, head = 0, tail = 0;
    const FabNode** queue = (const FabNode**)malloc(cap * sizeof *queue);
    if (!queue) return snx_error("out of memory");
    queue[tail++] = root;
    while (head < tail) {
        const FabNode* d = queue[head++];
        for (const FabNode* c = d->children; c; c = c->next) {
            if (c->is_dir) {
                if (tail == cap) {
                    const FabNode** more = (const FabNode**)realloc(queue, 2 * cap * sizeof *queue);
                    if (!more) {
                        free(queue);
                        return snx_error("out of memory");
                    }
                    queue = more;
                    cap *= 2;
                }
                queue[tail++] = c;
                ndirs++;
                dir_bytes += snx_romfs_entry_size(SNX_ROMFS_DIR_ENTRY, c->name);
            } else {
                nfiles++;
                file_bytes += snx_romfs_entry_size(SNX_ROMFS_FILE_ENTRY, c->name);
            }
        }
    }
    if (dir_bytes >= SNX_ROMFS_NONE || file_bytes >= SNX_ROMFS_NONE) {
        free(queue);
        return snx_error("the RomFS's tables are too large");
    }
    uint32_t dbuckets = snx_romfs_buckets(ndirs), fbuckets = snx_romfs_buckets(nfiles);
    SnxU64 dhash = SNX_ROMFS_HEADER, dtab = dhash + (SnxU64)dbuckets * 4, fhash = dtab + dir_bytes,
           ftab = fhash + (SnxU64)fbuckets * 4, end = ftab + file_bytes, data_at = snx_align_up(end, 0x200);
    RomDir* dirs = (RomDir*)malloc(ndirs * sizeof *dirs);
    unsigned char* m = (unsigned char*)calloc(1, (size_t)data_at);
    s->files = (FabNode**)malloc((nfiles ? nfiles : 1) * sizeof *s->files);
    s->offsets = (SnxU64*)malloc((nfiles ? nfiles : 1) * sizeof *s->offsets);
    if (!dirs || !m || !s->files || !s->offsets) {
        free(queue);
        free(dirs);
        free(m);
        return snx_error("out of memory");
    }
    memset(m + dhash, 0xFF, (size_t)dbuckets * 4);
    memset(m + fhash, 0xFF, (size_t)fbuckets * 4);
    /* the folders, with their table offsets: the queue's order */
    uint32_t nd = 0, off = 0;
    for (size_t i = 0; i < tail; i++) {
        dirs[nd].node = queue[i];
        dirs[nd].off = off;
        dirs[nd].child = dirs[nd].file = SNX_ROMFS_NONE;
        dirs[nd].parent = 0;
        off += (uint32_t)(i ? snx_romfs_entry_size(SNX_ROMFS_DIR_ENTRY, queue[i]->name) : SNX_ROMFS_DIR_ENTRY);
        nd++;
    }
    free(queue);
    /* each folder's first child and its children's parent: the children of folder i come in a run, in order */
    for (uint32_t i = 0, next = 1; i < nd; i++)
        for (const FabNode* c = dirs[i].node->children; c; c = c->next)
            if (c->is_dir) {
                if (dirs[i].child == SNX_ROMFS_NONE) dirs[i].child = dirs[next].off;
                dirs[next++].parent = i;
            }
    /* the files, folder by folder, with their data offsets */
    SnxU64 data = 0;
    uint32_t nf = 0, foff = 0;
    for (uint32_t i = 0; i < nd; i++) {
        unsigned char* e = NULL;
        for (const FabNode* c = dirs[i].node->children; c; c = c->next) {
            if (c->is_dir) continue;
            uint32_t len = (uint32_t)strlen(c->name);
            if (e) snx_st_le32(e + 4, foff); /* the previous file's sibling */
            if (dirs[i].file == SNX_ROMFS_NONE) dirs[i].file = foff;
            data = snx_align_up(data, 16);
            e = m + ftab + foff;
            snx_st_le32(e, dirs[i].off);
            snx_st_le32(e + 4, SNX_ROMFS_NONE);
            snx_st_le64(e + 8, data);
            snx_st_le64(e + 16, c->size);
            uint32_t b = snx_romfs_hash(dirs[i].off, c->name) % fbuckets;
            memcpy(e + 24, m + fhash + (SnxU64)b * 4, 4);
            snx_st_le32(m + fhash + (SnxU64)b * 4, foff);
            snx_st_le32(e + 28, len);
            memcpy(e + SNX_ROMFS_FILE_ENTRY, c->name, len);
            s->files[nf] = (FabNode*)c;
            s->offsets[nf++] = data_at + data;
            data += c->size;
            foff += (uint32_t)snx_romfs_entry_size(SNX_ROMFS_FILE_ENTRY, c->name);
        }
    }
    /* the folder entries, now that their first files are known */
    for (uint32_t i = 0; i < nd; i++) {
        unsigned char* e = m + dtab + dirs[i].off;
        const char* name = i ? dirs[i].node->name : "";
        uint32_t len = (uint32_t)strlen(name), parent = dirs[dirs[i].parent].off;
        int last = i + 1 == nd || dirs[i + 1].parent != dirs[i].parent || !i;
        snx_st_le32(e, parent);
        snx_st_le32(e + 4, last ? SNX_ROMFS_NONE : dirs[i + 1].off);
        snx_st_le32(e + 8, dirs[i].child);
        snx_st_le32(e + 12, dirs[i].file);
        uint32_t b = snx_romfs_hash(parent, name) % dbuckets;
        memcpy(e + 16, m + dhash + (SnxU64)b * 4, 4);
        snx_st_le32(m + dhash + (SnxU64)b * 4, dirs[i].off);
        snx_st_le32(e + 20, len);
        memcpy(e + SNX_ROMFS_DIR_ENTRY, name, len);
    }
    free(dirs);
    SnxU64 head_fields[10] = {SNX_ROMFS_HEADER, dhash, (SnxU64)dbuckets * 4, dtab, dir_bytes,
                              fhash, (SnxU64)fbuckets * 4, ftab, file_bytes, data_at};
    for (int i = 0; i < 10; i++) snx_st_le64(m + 8 * i, head_fields[i]);
    s->romfs = 1;
    s->root = root;
    s->block = IVFC_BLOCK;
    s->meta = m;
    s->meta_size = data_at;
    s->nfiles = nf;
    s->data_size = data_at + data;
    /* the IVFC levels: 5 the image, each one before it a hash a block of it, each at a block's boundary */
    s->level_size[IVFC_LEVELS - 1] = s->data_size;
    for (int i = IVFC_LEVELS - 2; i >= 0; i--) s->level_size[i] = snx_div_up(s->level_size[i + 1], IVFC_BLOCK) * 32;
    s->level_offset[0] = 0;
    for (int i = 1; i < IVFC_LEVELS; i++)
        s->level_offset[i] = snx_align_up(s->level_offset[i - 1] + s->level_size[i - 1], IVFC_BLOCK);
    if (s->level_size[0] > IVFC_BLOCK) return snx_error("the RomFS is too large for six IVFC levels");
    s->hash_size = s->level_offset[IVFC_LEVELS - 1];
    s->size = snx_align_up(s->hash_size + s->data_size, MEDIA);
    return 0;
}

/* ------------------------------------------------------------------ writing */

typedef struct {
    SnxFab* fab;
    int (*write)(const void*, size_t, void*);
    void* data;
    SnxU64 at;
    unsigned char* chunk;
} Out;

static int out_bytes(Out* o, const void* bytes, size_t size) {
    if (size && o->write(bytes, size, o->data)) return snx_error("cannot write the NCAs");
    o->at += size;
    return 0;
}

static int out_zeros(Out* o, SnxU64 size) {
    memset(o->chunk, 0, CHUNK);
    while (size) {
        size_t n = size < CHUNK ? (size_t)size : CHUNK;
        if (out_bytes(o, o->chunk, n)) return 1;
        size -= n;
    }
    return 0;
}

/* bytes at (from the start of the NCAs) kept to be written over the zeros there */
static int out_patch(Out* o, SnxU64 at, const void* bytes, size_t size) {
    SnxFab* f = o->fab;
    if (f->npatches == f->cap) {
        size_t cap = f->cap ? 2 * f->cap : 8;
        SnxFabPatch* more = (SnxFabPatch*)realloc(f->patches, cap * sizeof *more);
        if (!more) return snx_error("out of memory");
        f->patches = more;
        f->cap = cap;
    }
    unsigned char* copy = (unsigned char*)malloc(size ? size : 1);
    if (!copy) return snx_error("out of memory");
    memcpy(copy, bytes, size);
    f->patches[f->npatches++] = (SnxFabPatch){at, copy, size};
    return 0;
}

/* the hashes of a stream, a block at a time: the last block padded with zeros (pad), or as it is */
typedef struct {
    unsigned block;
    int pad;
    unsigned char* buf;
    size_t fill;
    unsigned char* out;
    size_t count, cap;
} Hasher;

static void hash_block(Hasher* h, const unsigned char* block, size_t size) {
    if (h->count < h->cap) sha256(block, size, h->out + 32 * h->count);
    h->count++;
}

static void hasher_feed(Hasher* h, const unsigned char* p, size_t size) {
    while (size) {
        if (!h->fill && size >= h->block) { /* whole blocks straight from the data */
            hash_block(h, p, h->block);
            p += h->block;
            size -= h->block;
            continue;
        }
        size_t n = h->block - h->fill < size ? h->block - h->fill : size;
        memcpy(h->buf + h->fill, p, n);
        h->fill += n;
        p += n;
        size -= n;
        if (h->fill == h->block) {
            hash_block(h, h->buf, h->block);
            h->fill = 0;
        }
    }
}

static void hasher_end(Hasher* h) {
    if (!h->fill) return;
    if (h->pad) memset(h->buf + h->fill, 0, h->block - h->fill);
    hash_block(h, h->buf, h->pad ? h->block : h->fill);
    h->fill = 0;
}

/* bytes written and hashed */
static int out_hashed(Out* o, Hasher* h, const void* bytes, size_t size) {
    hasher_feed(h, (const unsigned char*)bytes, size);
    return out_bytes(o, bytes, size);
}

/* a file's bytes, written and hashed: from memory, or read once from disk, which must still have its size */
static int out_file(Out* o, Hasher* h, const FabNode* n) {
    if (!n->path) return out_hashed(o, h, n->data, (size_t)n->size);
    FILE* file = snx_fopen(n->path, "rb");
    if (!file) return snx_error("cannot open %s", n->path);
    SnxU64 left = n->size;
    int failed = 0;
    while (left && !failed) {
        size_t want = left < CHUNK ? (size_t)left : CHUNK;
        if (fread(o->chunk, 1, want, file) != want)
            failed = snx_error("%s is shorter than the %llu bytes it had when it was listed", n->path,
                               (unsigned long long)n->size);
        else
            failed = out_hashed(o, h, o->chunk, want);
        left -= want;
    }
    if (!failed && fgetc(file) != EOF)
        failed = snx_error("%s grew past the %llu bytes it had when it was listed", n->path,
                           (unsigned long long)n->size);
    fclose(file);
    return failed;
}

/* a section, its hashes as zeros, then its data hashed; its hashes kept as a patch and its FS header made */
static int section_write(Out* o, FabSection* s) {
    SnxU64 start = o->at;
    Hasher h = {s->block, s->romfs, NULL, 0, NULL, 0, 0};
    h.cap = (size_t)snx_div_up(s->data_size, s->block);
    h.buf = (unsigned char*)malloc(s->block);
    h.out = (unsigned char*)calloc(h.cap ? h.cap : 1, 32);
    unsigned char* region = (unsigned char*)calloc(1, (size_t)(s->hash_size ? s->hash_size : 1));
    int failed = !h.buf || !h.out || !region ? snx_error("out of memory") : 0;
    if (!failed) failed = out_zeros(o, s->hash_size);
    if (!failed && !s->romfs) {
        failed = out_hashed(o, &h, s->pfs_header, s->pfs_header_size);
        for (FabNode* n = s->root->children; n && !failed; n = n->next) failed = out_file(o, &h, n);
    } else if (!failed) {
        SnxU64 at = s->meta_size;
        failed = out_hashed(o, &h, s->meta, (size_t)s->meta_size);
        for (size_t i = 0; i < s->nfiles && !failed; i++) {
            if (s->offsets[i] > at) { /* the alignment before a file */
                static const unsigned char zero[16];
                failed = out_hashed(o, &h, zero, (size_t)(s->offsets[i] - at));
            }
            if (!failed) failed = out_file(o, &h, s->files[i]);
            at = s->offsets[i] + s->files[i]->size;
        }
    }
    if (!failed) {
        hasher_end(&h);
        if (h.count != h.cap) failed = snx_error("the section's data came out a different size than laid out");
    }
    if (!failed) failed = out_zeros(o, s->size - s->hash_size - s->data_size);
    unsigned char master[32];
    if (!failed && !s->romfs) {
        memcpy(region, h.out, (size_t)s->table_size);
        sha256(h.out, (size_t)s->table_size, master);
    } else if (!failed) {
        /* level 4 hashes the image; each level before it hashes the level after it, padded blocks */
        unsigned char* block = (unsigned char*)malloc(IVFC_BLOCK);
        if (!block) failed = snx_error("out of memory");
        if (!failed) memcpy(region + s->level_offset[IVFC_LEVELS - 2], h.out, (size_t)s->level_size[IVFC_LEVELS - 2]);
        for (int i = IVFC_LEVELS - 3; i >= 0 && !failed; i--) {
            const unsigned char* above = region + s->level_offset[i + 1];
            for (SnxU64 b = 0; b * IVFC_BLOCK < s->level_size[i + 1]; b++) {
                SnxU64 n = s->level_size[i + 1] - b * IVFC_BLOCK;
                if (n > IVFC_BLOCK) n = IVFC_BLOCK;
                memset(block, 0, IVFC_BLOCK);
                memcpy(block, above + b * IVFC_BLOCK, (size_t)n);
                sha256(block, IVFC_BLOCK, region + s->level_offset[i] + b * 32);
            }
        }
        if (!failed) {
            memset(block, 0, IVFC_BLOCK);
            memcpy(block, region, (size_t)s->level_size[0]);
            sha256(block, IVFC_BLOCK, master);
        }
        free(block);
    }
    if (!failed && s->hash_size) failed = out_patch(o, start, region, (size_t)s->hash_size);
    if (!failed) { /* the FS header: version 2, not encrypted, then its hashes' superblock */
        unsigned char* f = s->fs_header;
        memset(f, 0, sizeof s->fs_header);
        snx_st_le16(f, 2);
        f[4] = 1; /* no encryption */
        if (!s->romfs) {
            f[2] = 1; /* a PFS0 */
            f[3] = 2; /* hashed by a table */
            memcpy(f + 8, master, 32);
            snx_st_le32(f + 8 + 0x20, s->block);
            snx_st_le32(f + 8 + 0x24, 2);
            snx_st_le64(f + 8 + 0x28, 0);
            snx_st_le64(f + 8 + 0x30, s->table_size);
            snx_st_le64(f + 8 + 0x38, s->hash_size);
            snx_st_le64(f + 8 + 0x40, s->data_size);
        } else {
            unsigned char* v = f + 8; /* the IVFC header */
            f[2] = 0; /* a RomFS */
            f[3] = 3; /* hashed by IVFC */
            memcpy(v, "IVFC", 4);
            snx_st_le32(v + 4, 0x20000);
            snx_st_le32(v + 8, 32);
            snx_st_le32(v + 12, IVFC_LEVELS + 1);
            for (int i = 0; i < IVFC_LEVELS; i++) {
                snx_st_le64(v + 0x10 + 0x18 * i, s->level_offset[i]);
                snx_st_le64(v + 0x18 + 0x18 * i, s->level_size[i]);
                snx_st_le32(v + 0x20 + 0x18 * i, IVFC_LOG2);
            }
            memcpy(v + 0xC0, master, 32);
        }
    }
    free(h.buf);
    free(h.out);
    free(region);
    return failed;
}

/* an NCA: its header as zeros, its sections, then its header kept as a patch, and its name from its hash */
static int nca_write(Out* o, FabNca* n, SnxU64 title) {
    SnxU64 start = o->at, at = NCA_HEADER;
    if (out_zeros(o, NCA_HEADER)) return 1;
    unsigned char* h = n->header;
    memset(h, 0, NCA_HEADER);
    for (int i = 0; i < n->count; i++) {
        FabSection* s = &n->sections[i];
        if (section_write(o, s)) return 1;
        snx_st_le32(h + 0x240 + 0x10 * i, (uint32_t)(at / MEDIA));
        snx_st_le32(h + 0x244 + 0x10 * i, (uint32_t)((at + s->size) / MEDIA));
        memcpy(h + 0x400 + 0x200 * i, s->fs_header, 0x200);
        sha256(s->fs_header, 0x200, h + 0x280 + 0x20 * i);
        at += s->size;
    }
    /* signatures zero, the padding at 0x340 zero: a plain NCA3 of no key generation, no rights ID */
    memcpy(h + 0x200, "NCA3", 4);
    h[0x205] = (unsigned char)n->content;
    snx_st_le64(h + 0x208, n->size);
    snx_st_le64(h + 0x210, title);
    if (o->at - start != n->size) return snx_error("an NCA came out a different size than laid out");
    sha256(h, NCA_HEADER, n->hash);
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        n->name[2 * i] = digits[n->hash[i] >> 4];
        n->name[2 * i + 1] = digits[n->hash[i] & 15];
    }
    snprintf(n->name + 32, sizeof n->name - 32, "%s", n->content == CONTENT_META ? ".cnmt.nca" : ".nca");
    return out_patch(o, start, h, NCA_HEADER);
}

/* the content meta's record of an NCA written */
static void cnmt_record(unsigned char* r, const FabNca* n, int type) {
    memcpy(r, n->hash, 32);
    memcpy(r + 0x20, n->hash, 16);
    for (int i = 0; i < 6; i++) r[0x30 + i] = (unsigned char)(n->size >> (8 * i));
    r[0x36] = (unsigned char)type;
    r[0x37] = 0;
}

int snx_fab_write(SnxFab* fab, int (*write)(const void*, size_t, void*), void* data) {
    Out o = {fab, write, data, 0, (unsigned char*)malloc(CHUNK)};
    for (size_t i = 0; i < fab->npatches; i++) free(fab->patches[i].bytes);
    fab->npatches = 0;
    if (!o.chunk) return snx_error("out of memory");
    int failed = nca_write(&o, &fab->ncas[NCA_PROGRAM], fab->title) ||
                 nca_write(&o, &fab->ncas[NCA_CONTROL], fab->title);
    if (!failed) { /* the content meta: an application of the two */
        unsigned char* c = fab->cnmt;
        memset(c, 0, CNMT_SIZE);
        snx_st_le64(c, fab->title);
        c[0xC] = 0x80;              /* an application */
        snx_st_le16(c + 0xE, 0x10); /* its extended header: the patch's title ID, the versions required */
        snx_st_le16(c + 0x10, 2);
        snx_st_le64(c + 0x20, fab->title | 0x800);
        cnmt_record(c + 0x30, &fab->ncas[NCA_PROGRAM], RECORD_PROGRAM);
        cnmt_record(c + 0x30 + 0x38, &fab->ncas[NCA_CONTROL], RECORD_CONTROL);
        failed = nca_write(&o, &fab->ncas[NCA_META], fab->title);
    }
    free(o.chunk);
    return failed;
}

size_t snx_fab_patches(const SnxFab* fab, const SnxFabPatch** patches) {
    *patches = fab->patches;
    return fab->npatches;
}

/* ------------------------------------------------------------------ the plan */

int snx_fab_is_game(const char* folder) {
    char* exefs = snx_exefs_folder(folder);
    int game = exefs != NULL;
    free(exefs);
    return game;
}

/* a stub control.nacp: name the title of the first language, a required user, the saves STUB_SAVE, the add-on
   content's base and the save's owner the game's, as the runtime reads them (am.c) */
static unsigned char* stub_nacp(const char* name, SnxU64 title) {
    unsigned char* p = (unsigned char*)calloc(1, NACP_SIZE);
    if (!p) return NULL;
    size_t n = strlen(name);
    if (n > 0x1FF) { /* at a character's start */
        n = 0x1FF;
        while (n && ((unsigned char)name[n] & 0xC0) == 0x80) n--;
    }
    memcpy(p, name, n);
    memcpy(p + 0x200, "ScarletNX", 9); /* the publisher */
    p[0x3025] = 1;                     /* a user account is required at startup */
    memcpy(p + 0x3060, "1.0.0", 5);    /* the display version */
    snx_st_le64(p + 0x3070, title + 0x1000);
    snx_st_le64(p + 0x3078, title);
    for (int i = 0; i < 4; i++) snx_st_le64(p + 0x3080 + 8 * i, STUB_SAVE);
    return p;
}

static SnxU64 nca_size(FabNca* n) {
    SnxU64 size = NCA_HEADER;
    for (int i = 0; i < n->count; i++) size += n->sections[i].size;
    n->size = size;
    return size;
}

SnxFab* snx_fab_plan(const char* folder, const char* name, int say) {
    SnxFab* fab = (SnxFab*)calloc(1, sizeof *fab);
    char *exefs = snx_exefs_folder(folder), *npdm_path = NULL, *romfs = NULL, *nacp_path = NULL, *icon_path = NULL;
    char why[512];
    size_t ex_dirs = 0, ex_files = 0, rom_dirs = 0, rom_files = 0, skipped = 0;
    SnxU64 ex_bytes = 0, rom_bytes = 0;
    int failed = 0, icons = 0;
    const char* stub = NULL; /* the stub control.nacp's title, NULL for the folder's control.nacp */
    if (!fab) {
        free(exefs);
        snx_error("out of memory");
        return NULL;
    }
    if (!exefs) {
        failed = snx_error("%s has no ExeFS (exefs/ with main)", folder);
        goto done;
    }
    /* the title ID: main.npdm's */
    npdm_path = snx_path_join(exefs, "main.npdm");
    {
        size_t size = 0;
        unsigned char* bytes = npdm_path ? snx_read_file(npdm_path, &size) : NULL;
        SnxNpdm npdm;
        if (!bytes) {
            failed = snx_error("cannot read %s: its title ID is the game's", npdm_path ? npdm_path : "main.npdm");
            goto done;
        }
        int bad = snx_parse_npdm(bytes, size, &npdm, why, sizeof why);
        free(bytes);
        if (bad || !npdm.title_id) {
            failed = snx_error("%s gives no title ID%s%s", npdm_path, bad ? ": " : "", bad ? why : "");
            goto done;
        }
        fab->title = npdm.title_id;
    }
    /* the ExeFS, flat; the RomFS, the whole tree under romfs/ */
    if (!(fab->exefs = node_new("", 1)) || !(fab->romfs = node_new("", 1)) || !(fab->control = node_new("", 1)) ||
        !(fab->meta = node_new("", 1))) {
        failed = snx_error("out of memory");
        goto done;
    }
    if ((failed = node_walk(fab->exefs, exefs, 0, &ex_dirs, &ex_files, &skipped, &ex_bytes))) goto done;
    romfs = snx_path_join(folder, "romfs");
    if (romfs && !snx_is_dir(romfs)) {
        free(romfs);
        romfs = snx_path_join(folder, "RomFS");
    }
    int has_romfs = romfs && snx_is_dir(romfs);
    if (has_romfs && (failed = node_walk(fab->romfs, romfs, 1, &rom_dirs, &rom_files, &skipped, &rom_bytes)))
        goto done;
    /* the Control NCA's: the folder's control.nacp, else a stub; its icon, a copy for each language it names */
    nacp_path = snx_path_join(folder, "control.nacp");
    icon_path = snx_path_join(folder, "icon.jpg");
    if (!nacp_path || !icon_path) {
        failed = snx_error("out of memory");
        goto done;
    }
    {
        size_t size = 0;
        unsigned char* nacp = snx_is_file(nacp_path) ? snx_read_file(nacp_path, &size) : NULL;
        if (nacp && size != NACP_SIZE) {
            free(nacp);
            failed = snx_error("%s is %lu bytes, and a control.nacp is 0x4000", nacp_path, (unsigned long)size);
            goto done;
        }
        if (!nacp) {
            const char* leaf = snx_path_leaf(folder);
            stub = name ? name : leaf && leaf[0] ? leaf : "Game";
            nacp = stub_nacp(stub, fab->title);
            if (!nacp) {
                failed = snx_error("out of memory");
                goto done;
            }
        }
        int titled[16], any = 0;
        for (int i = 0; i < 16; i++) any |= titled[i] = nacp[0x300 * i] != 0;
        if ((failed = node_bytes(fab->control, "control.nacp", nacp, NACP_SIZE))) goto done;
        if (snx_is_file(icon_path)) {
            size_t icon_size = 0;
            unsigned char* icon = snx_read_file(icon_path, &icon_size);
            if (!icon) {
                failed = snx_error("cannot read %s", icon_path);
                goto done;
            }
            for (int i = 0; i < 16 && !failed; i++) {
                if (any ? !titled[i] : i != 0) continue; /* every language named, the first when none is */
                char file[64];
                unsigned char* copy = (unsigned char*)malloc(icon_size ? icon_size : 1);
                if (copy) memcpy(copy, icon, icon_size);
                snprintf(file, sizeof file, "icon_%s.dat", languages[i]);
                failed = copy ? node_bytes(fab->control, file, copy, icon_size) : snx_error("out of memory");
                icons++;
            }
            free(icon);
            if (failed) goto done;
        }
    }
    /* the Meta NCA's content meta, filled in as it is written */
    {
        char file[64];
        snprintf(file, sizeof file, "Application_%016llx.cnmt", (unsigned long long)fab->title);
        unsigned char* cnmt = (unsigned char*)calloc(1, CNMT_SIZE);
        if (!cnmt || node_bytes(fab->meta, file, cnmt, CNMT_SIZE)) {
            failed = cnmt ? 1 : snx_error("out of memory");
            goto done;
        }
        fab->cnmt = cnmt;
    }
    /* the NCAs' sections */
    {
        FabNca* p = &fab->ncas[NCA_PROGRAM];
        p->content = CONTENT_PROGRAM;
        if ((failed = pfs_plan(&p->sections[p->count++], fab->exefs, EXEFS_BLOCK))) goto done;
        if (has_romfs && (failed = romfs_plan(&p->sections[p->count++], fab->romfs))) goto done;
        FabNca* c = &fab->ncas[NCA_CONTROL];
        c->content = CONTENT_CONTROL;
        if ((failed = romfs_plan(&c->sections[c->count++], fab->control))) goto done;
        FabNca* m = &fab->ncas[NCA_META];
        m->content = CONTENT_META;
        if ((failed = pfs_plan(&m->sections[m->count++], fab->meta, META_BLOCK))) goto done;
        for (int i = 0; i < NCAS; i++) {
            nca_size(&fab->ncas[i]);
            memset(fab->ncas[i].name, '0', 32);
            snprintf(fab->ncas[i].name + 32, sizeof fab->ncas[i].name - 32, "%s",
                     i == NCA_META ? ".cnmt.nca" : ".nca");
        }
    }
    if (say) {
        char size[32];
        printf("Fabricating NCAs from %s, a game of title ID %016llX\n", folder, (unsigned long long)fab->title);
        snx_size_text(ex_bytes, size, sizeof size);
        printf("  ExeFS: %lu files, %s\n", (unsigned long)ex_files, size);
        snx_size_text(rom_bytes, size, sizeof size);
        if (has_romfs)
            printf("  RomFS: %lu files in %lu folders, %s\n", (unsigned long)rom_files, (unsigned long)rom_dirs + 1,
                   size); /* the root too, as the extraction counts */
        else
            puts("  RomFS: none");
        if (stub)
            printf("  control.nacp: a stub, its title \"%s\"\n", stub);
        else
            puts("  control.nacp: the folder's");
        printf("  icon: %s\n", icons ? "icon.jpg" : "none");
        if (skipped) printf("  left out: %lu (folders in the ExeFS, wget's .listing files)\n", (unsigned long)skipped);
        for (int i = 0; i < NCAS; i++) {
            static const char* const what[NCAS] = {"Program", "Control", "Meta"};
            snx_size_text(fab->ncas[i].size, size, sizeof size);
            printf("  %s NCA: %s\n", what[i], size);
        }
    }
done:
    free(exefs);
    free(npdm_path);
    free(romfs);
    free(nacp_path);
    free(icon_path);
    if (failed) {
        snx_fab_free(fab);
        return NULL;
    }
    return fab;
}

static void section_free(FabSection* s) {
    free(s->pfs_header);
    free(s->meta);
    free(s->files);
    free(s->offsets);
}

void snx_fab_free(SnxFab* fab) {
    if (!fab) return;
    for (int i = 0; i < NCAS; i++)
        for (int j = 0; j < fab->ncas[i].count; j++) section_free(&fab->ncas[i].sections[j]);
    node_free(fab->exefs);
    node_free(fab->romfs);
    node_free(fab->control);
    node_free(fab->meta); /* the cnmt with it */
    for (size_t i = 0; i < fab->npatches; i++) free(fab->patches[i].bytes);
    free(fab->patches);
    free(fab);
}

SnxU64 snx_fab_title(const SnxFab* fab) { return fab->title; }
size_t snx_fab_count(const SnxFab* fab) {
    (void)fab;
    return NCAS;
}
SnxU64 snx_fab_size(const SnxFab* fab, size_t nca) { return fab->ncas[nca].size; }
const char* snx_fab_name(const SnxFab* fab, size_t nca) { return fab->ncas[nca].name; }
