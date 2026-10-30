/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   CTR level-3 layout/hash facts: Azahar core/hle/romfs.cpp and libctru
   romfs_dev.c. Independent serializer over NX's shared host FsTree, not NX
   IPC or Switch RomFS metadata; or, without an extraction, the CIA's own
   level 3 read where it lies in the mapped CIA. */
#include "3ds/loader/ctr_loader.h"
#include "3ds/services/romfs.h"
#include "common/filesystem/tree.h"
#include "common/utilities/bits.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define NONE  UINT32_MAX
typedef struct {
    FsNode* node;
    uint32_t parent, next, child, file, off;
} Dir;
typedef struct {
    FsNode* node;
    uint64_t data;
    uint32_t parent, next, off;
} File;
struct SnxCtrRomfs {
    /* a CIA's level 3 read where it lies (snx_ctr_romfs_open_image): the image, its size and the CIA's mtime, and no
       tree */
    const unsigned char* image;
    uint64_t image_size, mtime;
    FsTree* tree;
    uint32_t dirs, files;
    size_t meta_size;
    Dir dir[];
};
typedef struct {
    uint32_t dirs, files, dir_bytes, file_bytes;
    uint64_t data;
} Measure;
static File* files(SnxCtrRomfs* r) { return (File*)(r->dir + r->dirs); }
static unsigned char* meta(SnxCtrRomfs* r) { return (unsigned char*)(files(r) + r->files); }
static uint32_t buckets(uint32_t n) { return n < 3 ? 3 : n | 1; }
static bool name16(const char* s, uint16_t out[256], uint32_t* bytes) {
    unsigned n = 0;
    while (*s) {
        uint32_t cp = (unsigned char)*s++, low = 0;
        unsigned count = 0;
        if (cp >= 0xc2 && cp <= 0xdf) {
            cp &= 31;
            count = 1;
            low = 0x80;
        } else if (cp >= 0xe0 && cp <= 0xef) {
            cp &= 15;
            count = 2;
            low = 0x800;
        } else if (cp >= 0xf0 && cp <= 0xf4) {
            cp &= 7;
            count = 3;
            low = 0x10000;
        } else if (cp >= 0x80)
            return false;
        for (unsigned i = 0; i < count; ++i) {
            unsigned c = (unsigned char)*s++;
            if ((c & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (c & 63);
        }
        if (cp < low || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
        if (cp > 0xffff) {
            if (n >= 254) return false;
            cp -= 0x10000;
            out[n++] = (uint16_t)(0xd800 + (cp >> 10));
            out[n++] = (uint16_t)(0xdc00 + (cp & 1023));
        } else {
            if (n >= 255) return false;
            out[n++] = (uint16_t)cp;
        }
    }
    *bytes = n * 2;
    return true;
}
static bool measure(FsNode* node, Measure* m, bool root) {
    uint16_t name[256];
    uint32_t bytes;
    if (!name16(root ? "" : node->name, name, &bytes) || m->dirs + m->files >= 100000) return false;
    if (node->is_dir) {
        ++m->dirs;
        m->dir_bytes += 24 + ((bytes + 3) & ~3u);
        for (FsNode* c = node->children; c; c = c->next)
            if (!measure(c, m, false)) return false;
    } else {
        ++m->files;
        m->file_bytes += 32 + ((bytes + 3) & ~3u);
        uint64_t aligned = snx_align_up(m->data, 16);
        if (aligned > SNX_CTR_MAX_CONTENT || node->size > SNX_CTR_MAX_CONTENT - aligned) return false;
        m->data = aligned + node->size;
    }
    return true;
}
static uint32_t hash(uint32_t parent, const uint16_t* name, uint32_t bytes) {
    uint32_t h = parent ^ 123456789u;
    for (uint32_t i = 0; i < bytes / 2; ++i) h = ((h >> 5) | (h << 27)) ^ name[i];
    return h;
}
static void fill(SnxCtrRomfs* r, FsNode* node, uint32_t parent, Measure* m) {
    uint32_t id = m->dirs++;
    Dir* d = &r->dir[id];
    d->node = node;
    d->parent = parent;
    d->off = m->dir_bytes;
    d->next = d->child = d->file = NONE;
    uint16_t name[256];
    uint32_t bytes;
    (void)name16(id ? node->name : "", name, &bytes);
    m->dir_bytes += 24 + ((bytes + 3) & ~3u);
    uint32_t last_dir = NONE, last_file = NONE;
    for (FsNode* c = node->children; c; c = c->next) {
        if (c->is_dir) {
            uint32_t next = m->dirs;
            if (last_dir == NONE)
                d->child = next;
            else
                r->dir[last_dir].next = next;
            last_dir = next;
            fill(r, c, id, m);
        } else {
            uint32_t next = m->files++;
            File* f = &files(r)[next];
            f->node = c;
            f->parent = id;
            f->off = m->file_bytes;
            f->next = NONE;
            if (last_file == NONE)
                d->file = next;
            else
                files(r)[last_file].next = next;
            last_file = next;
            (void)name16(c->name, name, &bytes);
            m->file_bytes += 32 + ((bytes + 3) & ~3u);
            m->data = snx_align_up(m->data, 16);
            f->data = r->meta_size + m->data;
            m->data += c->size;
        }
    }
}
static void serialize(SnxCtrRomfs* r, const Measure* m) {
    unsigned char* b = meta(r);
    uint32_t dh = 40, dt = dh + 4 * buckets(r->dirs), fh = dt + m->dir_bytes, ft = fh + 4 * buckets(r->files);
    uint32_t header[] = {40,
                         dh,
                         4 * buckets(r->dirs),
                         dt,
                         m->dir_bytes,
                         fh,
                         4 * buckets(r->files),
                         ft,
                         m->file_bytes,
                         (uint32_t)r->meta_size};
    for (unsigned i = 0; i < 10; ++i) snx_st_le32(b + i * 4, header[i]);
    memset(b + dh, 0xff, 4 * buckets(r->dirs));
    memset(b + fh, 0xff, 4 * buckets(r->files));
    for (unsigned type = 0; type < 2; ++type) {
        uint32_t count = type ? r->files : r->dirs, ht = type ? fh : dh, table = type ? ft : dt;
        for (uint32_t i = 0; i < count; ++i) {
            Dir* d = type ? NULL : &r->dir[i];
            File* f = type ? &files(r)[i] : NULL;
            uint32_t off = type ? f->off : d->off, parent = r->dir[type ? f->parent : d->parent].off;
            uint16_t name[256];
            uint32_t bytes;
            (void)name16(type ? f->node->name : i ? d->node->name : "", name, &bytes);
            unsigned char* record = b + table + off;
            uint32_t slot = hash(parent, name, bytes) % buckets(count), previous = 0;
            for (unsigned j = 0; j < 4; ++j) previous |= (uint32_t)b[ht + slot * 4 + j] << (j * 8);
            snx_st_le32(b + ht + slot * 4, off);
            snx_st_le32(record, parent);
            uint32_t next = type ? f->next : d->next;
            snx_st_le32(record + 4, next == NONE ? NONE : type ? files(r)[next].off : r->dir[next].off);
            if (type) {
                snx_st_le64(record + 8, f->data - r->meta_size);
                snx_st_le64(record + 16, f->node->size);
                snx_st_le32(record + 24, previous);
                snx_st_le32(record + 28, bytes);
            } else {
                snx_st_le32(record + 8, d->child == NONE ? NONE : r->dir[d->child].off);
                snx_st_le32(record + 12, d->file == NONE ? NONE : files(r)[d->file].off);
                snx_st_le32(record + 16, previous);
                snx_st_le32(record + 20, bytes);
            }
            for (uint32_t j = 0; j < bytes / 2; ++j) {
                record[(type ? 32 : 24) + j * 2] = (unsigned char)name[j];
                record[(type ? 32 : 24) + j * 2 + 1] = (unsigned char)(name[j] >> 8);
            }
        }
    }
}
bool snx_ctr_romfs_open(FsTree* tree, SnxCtrRomfs** out) {
    *out = NULL;
    if (!tree) return false;
    pm_lock(&tree->lock);
    FsNode* root = NULL;
    FsTreeResult rc = fs_tree_lookup(tree, NULL, "romfs", &root);
    if (rc == FS_TREE_NOTFOUND) {
        pm_unlock(&tree->lock);
        return true;
    }
    Measure m = {0};
    if (rc || !root->is_dir || !measure(root, &m, true)) goto fail;
    uint64_t meta_size = snx_align_up(40 + 4ull * buckets(m.dirs) + m.dir_bytes + 4ull * buckets(m.files)
                                      + m.file_bytes, 16);
    uint64_t alloc =
        sizeof(SnxCtrRomfs) + (uint64_t)m.dirs * sizeof(Dir) + (uint64_t)m.files * sizeof(File) + meta_size;
    if (meta_size + m.data > SNX_CTR_MAX_CONTENT || alloc > SIZE_MAX) goto fail;
    SnxCtrRomfs* r = calloc(1, (size_t)alloc);
    if (!r) goto fail;
    r->tree = tree;
    r->dirs = m.dirs;
    r->files = m.files;
    r->meta_size = (size_t)meta_size;
    FsShare share = {0};
    if (fs_tree_share(tree, root, &share)) {
        free(r);
        goto fail;
    }
    Measure pos = {0};
    fill(r, root, 0, &pos);
    serialize(r, &m);
    pm_unlock(&tree->lock);
    *out = r;
    return true;
fail:
    pm_unlock(&tree->lock);
    return false;
}
bool snx_ctr_romfs_open_image(const unsigned char* image, uint64_t size, uint64_t mtime, SnxCtrRomfs** out) {
    *out = NULL;
    if (!image) { /* the image an empty romfs/ folder gives, serialized after the view, which then reads it */
        FsNode root = {.is_dir = 1};
        Measure m = {0}, pos = {0};
        if (!measure(&root, &m, true)) return false;
        size_t meta_size = (size_t)snx_align_up(40 + 4ull * buckets(m.dirs) + m.dir_bytes + 4ull * buckets(m.files),
                                                16);
        SnxCtrRomfs* r = calloc(1, sizeof *r + sizeof(Dir) + meta_size);
        if (!r) return false;
        r->dirs = 1;
        r->meta_size = meta_size;
        fill(r, &root, 0, &pos);
        serialize(r, &m);
        r->image = meta(r);
        r->image_size = meta_size;
        r->mtime = mtime;
        *out = r;
        return true;
    }
    if (size < 0x28 || size > SNX_CTR_MAX_CONTENT) return false;
    SnxCtrRomfs* r = calloc(1, sizeof *r);
    if (!r) return false;
    r->image = image;
    r->image_size = size;
    r->mtime = mtime;
    *out = r;
    return true;
}
void snx_ctr_romfs_free(SnxCtrRomfs* r) { free(r); /* the shared tree, or the CIA's mapping, is the process's */ }
static uint32_t image32(const SnxCtrRomfs* r, uint64_t at) {
    if (at > r->image_size || r->image_size - at < 4) return NONE;
    const unsigned char* p = r->image + at;
    return snx_ld_le32(p);
}
/* whether path (relative, '/'-separated UTF-8) names an entry of the image's level-3 tables: the root for "", then
   each component among the folder's child folders, the last also among its files. The loader walked the tables
   already; the bounds are checked again all the same */
static bool image_has(const SnxCtrRomfs* r, const char* path) {
    uint32_t dir_table = image32(r, 12), dir_size = image32(r, 16), file_table = image32(r, 28),
             file_size = image32(r, 32);
    uint32_t dir = 0;
    if (dir_table == NONE || file_table == NONE || dir_size == NONE || file_size == NONE ||
        (uint64_t)dir_table + dir_size > r->image_size || (uint64_t)file_table + file_size > r->image_size)
        return false;
    while (*path) {
        const char* end = strchr(path, '/');
        size_t length = end ? (size_t)(end - path) : strlen(path);
        char part[768];
        uint16_t name[256];
        uint32_t bytes;
        if (!length) { /* a doubled or trailing separator */
            path += 1;
            continue;
        }
        if (length >= sizeof part) return false;
        memcpy(part, path, length);
        part[length] = 0;
        if (!name16(part, name, &bytes)) return false;
        bool found = false;
        /* the folder's children, then (for the last component) its files */
        for (unsigned kind = 0; kind < (end ? 1u : 2u) && !found; ++kind) {
            uint32_t base = kind ? file_table : dir_table, size = kind ? file_size : dir_size, head = kind ? 32 : 24;
            uint32_t at = image32(r, (uint64_t)dir_table + dir + (kind ? 12 : 8));
            for (unsigned guard = 0; at != NONE && guard < 100000; ++guard) {
                if (at > size || size - at < head) return false;
                uint32_t length16 = image32(r, (uint64_t)base + at + head - 4);
                if (length16 > size - at - head) return false;
                if (length16 == bytes) {
                    bool same = true;
                    for (uint32_t i = 0; i < bytes / 2 && same; ++i) {
                        const unsigned char* q = r->image + base + at + head + i * 2;
                        same = (uint16_t)(q[0] | q[1] << 8) == name[i];
                    }
                    if (same) {
                        found = true;
                        if (!kind) dir = at;
                        break;
                    }
                }
                at = image32(r, (uint64_t)base + at + 4);
            }
        }
        if (!found) return false;
        path += length + (end ? 1 : 0);
    }
    return true;
}
/* 3DS datetime milliseconds since 1900-01-01; the Unix epoch starts
   2208988800 seconds later. */
#define UNIX_EPOCH_1900_MS 2208988800000ull
bool snx_ctr_utf16_path(const unsigned char* path16, size_t bytes, char* out, size_t out_size) {
    if (!path16 || !out || out_size < 2 || !bytes || (bytes & 1) || bytes > 256 || out_size > 768) return false;
    unsigned n = 0;
    bool terminated = false;
    for (size_t i = 0; i < bytes / 2; ++i) {
        uint32_t cp = (uint32_t)path16[2 * i] | (uint32_t)path16[2 * i + 1] << 8;
        if (!cp) {
            terminated = true;
            break;
        }
        if (cp >= 0xd800 && cp < 0xdc00) {
            if (++i >= bytes / 2) return false;
            uint32_t low = (uint32_t)path16[2 * i] | (uint32_t)path16[2 * i + 1] << 8;
            if (low < 0xdc00 || low >= 0xe000) return false;
            cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
        } else if (cp >= 0xdc00 && cp < 0xe000)
            return false;
        if (cp < 0x80) {
            if (n + 1 >= out_size) return false;
            out[n++] = (char)cp;
        } else if (cp < 0x800) {
            if (n + 2 >= out_size) return false;
            out[n++] = (char)(0xc0 | (cp >> 6));
            out[n++] = (char)(0x80 | (cp & 63));
        } else if (cp < 0x10000) {
            if (n + 3 >= out_size) return false;
            out[n++] = (char)(0xe0 | (cp >> 12));
            out[n++] = (char)(0x80 | ((cp >> 6) & 63));
            out[n++] = (char)(0x80 | (cp & 63));
        } else {
            if (n + 4 >= out_size) return false;
            out[n++] = (char)(0xf0 | (cp >> 18));
            out[n++] = (char)(0x80 | ((cp >> 12) & 63));
            out[n++] = (char)(0x80 | ((cp >> 6) & 63));
            out[n++] = (char)(0x80 | (cp & 63));
        }
    }
    if (!terminated) return false;
    out[n] = 0;
    for (unsigned i = 0; i < n; ++i)
        if (out[i] == '\\') out[i] = '/'; /* FS archive paths accept either separator */
    return true;
}
SnxCtrTimeResult snx_ctr_romfs_time(SnxCtrRomfs* r, const unsigned char* path16, size_t bytes, uint64_t* datetime) {
    if (!r || !path16 || !datetime) return SNX_CTR_TIME_INVALID;
    char path[768];
    if (!snx_ctr_utf16_path(path16, bytes, path, sizeof path)) return SNX_CTR_TIME_INVALID;
    const char* relative = path;
    while (*relative == '/') ++relative;
    if (r->image) { /* every entry has the CIA's time, the backing file's */
        if (!image_has(r, relative)) return SNX_CTR_TIME_NOTFOUND;
        if (!r->mtime || r->mtime > UINT64_MAX - UNIX_EPOCH_1900_MS) return SNX_CTR_TIME_INVALID;
        *datetime = r->mtime + UNIX_EPOCH_1900_MS;
        return SNX_CTR_TIME_OK;
    }
    pm_lock(&r->tree->lock);
    FsNode *root = NULL, *node = root;
    SnxCtrTimeResult result = SNX_CTR_TIME_NOTFOUND;
    if (fs_tree_lookup(r->tree, NULL, "romfs", &root) == FS_TREE_OK && root && root->is_dir) {
        node = root;
        if (*relative && fs_tree_lookup(r->tree, root, relative, &node) != FS_TREE_OK) node = NULL;
        if (node) {
            uint64_t unix_ms = 0;
            if (fs_tree_mtime(r->tree, node, &unix_ms) == FS_TREE_OK && unix_ms <= UINT64_MAX - UNIX_EPOCH_1900_MS) {
                *datetime = unix_ms + UNIX_EPOCH_1900_MS;
                result = SNX_CTR_TIME_OK;
            } else
                result = SNX_CTR_TIME_INVALID;
        }
    } else
        result = SNX_CTR_TIME_INVALID;
    pm_unlock(&r->tree->lock);
    return result;
}
uint64_t snx_ctr_romfs_size(const SnxCtrRomfs* r) {
    if (!r) return 0;
    if (r->image) return r->image_size;
    pm_lock(&r->tree->lock);
    const File* f = (const File*)(r->dir + r->dirs);
    uint64_t size = r->files ? f[r->files - 1].data + f[r->files - 1].node->size : r->meta_size;
    pm_unlock(&r->tree->lock);
    return size;
}
bool snx_ctr_romfs_read(SnxCtrRomfs* r, uint64_t offset, void* output, size_t size) {
    if (!r || offset > snx_ctr_romfs_size(r) || size > snx_ctr_romfs_size(r) - offset) return false;
    if (!size) return true;
    if (!output) return false;
    if (r->image) { /* as the CIA holds it */
        memcpy(output, r->image + offset, size);
        return true;
    }
    pm_lock(&r->tree->lock);
    unsigned char* data = output;
    memset(data, 0, size);
    if (offset < r->meta_size) {
        size_t n = r->meta_size - (size_t)offset;
        if (n > size) n = size;
        memcpy(data, meta(r) + offset, n);
    }
    for (uint32_t i = 0; i < r->files; ++i) {
        File* f = &files(r)[i];
        uint64_t start = offset > f->data ? offset : f->data, end = offset + size;
        if (end > f->data + f->node->size) end = f->data + f->node->size;
        if (start >= end) continue;
        size_t n = (size_t)(end - start);
        uint64_t at = start - f->data;
        unsigned char* dest = data + (size_t)(start - offset);
        FsNode* node = f->node;
        if (node->fd >= 0)
            memcpy(dest, node->map + at, n);
        else {
            FILE* file = fs_tree_open(r->tree, node, "rb");
            if (!file) goto error;
            bool ok = !snx_seek(file, at) && fread(dest, 1, n, file) == n;
            if (fclose(file)) ok = false;
            if (!ok) goto error;
        }
    }
    pm_unlock(&r->tree->lock);
    return true;
error:
    pm_unlock(&r->tree->lock);
    return false;
}
