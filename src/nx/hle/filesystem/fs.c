/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/file_sys/fsmitm_romfsbuild.cpp):
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* fsp-srv over the game's indexed data folder: the RomFS (a romfs.bin, or an image made up from the romfs/ folder),
   save data, the SD card and the system archives */
#include "nx/hle/filesystem/fs.h"
#include "nx/hle/filesystem/fs_tree.h"
#include "nx/runtime/main.h"
#include "nx/hle/services/services.h"
#include "nx/hle/services/services_int.h"
#include "nx/hle/kernel/ipc.h"
#include "hle/filesystem/fs.swipc.inc"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

/* nn::fs results */
#define FS_NOTFOUND            RC(2, 1)
#define FS_EXISTS              RC(2, 2)
#define FS_DIR_NOT_EMPTY       RC(2, 8)
#define FS_NO_SPACE            RC(2, 30) /* UsableSpaceNotEnough */
#define FS_OUT_OF_RANGE        RC(2, 3005)
#define FS_PATH_TOO_LONG       RC(2, 6003)
#define FS_DIR_UNOBTAINABLE    RC(2, 6006) /* a ".." above the root */
#define FS_INVALID_OFFSET      RC(2, 6061)
#define FS_INVALID_SIZE        RC(2, 6062)
#define FS_INVALID_OPEN_MODE   RC(2, 6072)
#define FS_NO_APPEND           RC(2, 6201) /* FileExtensionWithoutOpenModeAllowAppend */
#define FS_READ_NOT_PERMITTED  RC(2, 6202)
#define FS_WRITE_NOT_PERMITTED RC(2, 6203)

/* RomFS. With no romfs.bin, the image is made up from the romfs/ folder: its header and tables in memory (rmeta), the
   files' data read from the files themselves when the game reads that part of the image */
typedef struct {
    FsNode* node; /* retained for the run: the image keeps its files */
    u64 off;      /* where its data starts in the image */
    int logged;   /* its first read was logged, and its first failure warned (atomics: game threads read at once) */
    int warned;
} RFile;
/* unmapped RomFS files open on the host, least recently used closed first */
#define NOPEN 48
typedef struct {
    FsNode* node;
    FILE* f;
    u64 used; /* the clock when it was last read */
} ROpen;
static struct {
    FsTree tree;
    RFile* rfiles; /* in offset order: romfs_build lays the files out in this order */
    int nrfiles;
    u8* rmeta;
    u64 rmeta_size; /* header + tables sit at the start of the image */
    u64 rdata_off, rtotal;
    FILE* rraw;     /* plain romfs.bin, when it couldn't be mapped */
    FsNode* rimage; /* plain romfs.bin, mapped */
    ROpen openf[NOPEN]; /* rf_open's files, under tree.lock */
    u64 openclk;
    u8 emptyrom[0x200]; /* the empty RomFS of the system archives (emptyrom_init) */
} fsp;

static u32 tree_result(FsTreeResult r) {
    switch (r) {
        case FS_TREE_OK: return 0;
        case FS_TREE_NOTFOUND: return FS_NOTFOUND;
        case FS_TREE_EXISTS: return FS_EXISTS;
        case FS_TREE_NOTEMPTY: return FS_DIR_NOT_EMPTY;
        case FS_TREE_NOTDELETABLE: return RC(2, 6031);
        case FS_TREE_NOTRENAMABLE: return RC(2, 6032);
        case FS_TREE_INVALID: return FS_DIR_UNOBTAINABLE;
        case FS_TREE_TOOLONG: return FS_PATH_TOO_LONG;
        case FS_TREE_READONLY: return FS_WRITE_NOT_PERMITTED;
        default: return FS_NO_SPACE;
    }
}

void fs_init(void) {
    FsTreeResult r = fs_tree_init(&fsp.tree, g_datadir);
    if (r != FS_TREE_OK) fatal("Could not index game filesystem %s (error %d)", g_datadir, r);
    size_t n = strlen(fsp.tree.base);
    if (n + 2 > sizeof g_datadir) fatal("Game filesystem path is too long");
    memcpy(g_datadir, fsp.tree.base, n);
    if (n && g_datadir[n - 1] != '/' && g_datadir[n - 1] != '\\') g_datadir[n++] = '/';
    g_datadir[n] = 0;
    /* RomFS and ExeFS are what games read most and never change: open and map them once, for the whole run, instead
       of opening them on demand like the save and SD card files */
    static const char* const shared_paths[] = {"romfs", "exefs", "romfs.bin"};
    unsigned shared = 0, failed = 0;
    pm_lock(&fsp.tree.lock);
    for (size_t i = 0; i < sizeof shared_paths / sizeof shared_paths[0]; i++) {
        FsNode* node;
        if (fs_tree_lookup(&fsp.tree, NULL, shared_paths[i], &node)) continue;
        r = fs_tree_share(&fsp.tree, node, &shared, &failed);
        if (r != FS_TREE_OK) fatal("Could not map %s%s (error %d)", g_datadir, shared_paths[i], r);
    }
    pm_unlock(&fsp.tree.lock);
    LOG("fs: %u RomFS/ExeFS files mapped for the whole run\n", shared);
    if (failed) WARN("fs: %u RomFS/ExeFS files could not be mapped, they are read on demand\n", failed);
}

/* Contents held in memory, read without the host: an injected file, or one mapped at startup */
static int node_in_memory(const FsNode* node) { return node->is_virtual || node->fd >= 0; }
static const u8* node_memory(const FsNode* node) { return node->is_virtual ? node->virtual_data : node->map; }

/* Callers hold tree.lock: content I/O is permitted only after resolving an indexed node. */
static FILE* node_open(FsNode* node, const char* mode) { return fs_tree_open(&fsp.tree, node, mode); }

FILE* fs_open_read(const char* path, u64* size) {
    FsNode* node;
    FILE* f = NULL;
    if (size) *size = 0;
    pm_lock(&fsp.tree.lock);
    if (!fs_tree_lookup(&fsp.tree, NULL, path, &node)) {
        f = node_open(node, "rb");
        if (f && size) *size = node->size;
    }
    pm_unlock(&fsp.tree.lock);
    return f;
}

int fs_map_file(const char* path, PFileMap* map) {
    FsNode* node;
    char host[FS_TREE_PATH_MAX];
    int r = -1;
    memset(map, 0, sizeof *map);
    pm_lock(&fsp.tree.lock);
    if (!fs_tree_lookup(&fsp.tree, NULL, path, &node) && !node->is_dir) {
        if (node->fd >= 0) { /* already mapped for the whole run: lend that view */
            map->data = node->map;
            map->size = node->size;
            map->borrowed = 1;
            r = 0;
        } else if (!node->is_virtual && !fs_tree_path(&fsp.tree, node, host, sizeof host))
            r = plat_file_map_sized(map, host, node->size);
    }
    pm_unlock(&fsp.tree.lock);
    return r;
}

u8* fs_read_file(const char* path, u64* size) {
    FsNode* node;
    u8* data = NULL;
    if (size) *size = 0;
    pm_lock(&fsp.tree.lock);
    if (!fs_tree_lookup(&fsp.tree, NULL, path, &node) && !node->is_dir && node->size <= SIZE_MAX) {
        data = malloc(node->size ? (size_t)node->size : 1);
        if (data) {
            int ok = 1;
            if (node_in_memory(node)) {
                if (node->size) memcpy(data, node_memory(node), (size_t)node->size);
            } else {
                FILE* f = node_open(node, "rb");
                ok = f && fread(data, 1, (size_t)node->size, f) == node->size;
                if (f) fclose(f);
            }
            if (!ok) {
                free(data);
                data = NULL;
            } else if (size)
                *size = node->size;
        }
    }
    pm_unlock(&fsp.tree.lock);
    return data;
}

int fs_add_virtual_file(const char* path, const void* data, u64 size) {
    pm_lock(&fsp.tree.lock);
    FsTreeResult r = fs_tree_add_virtual(&fsp.tree, path, data, size, NULL);
    pm_unlock(&fsp.tree.lock);
    return r ? -1 : 0;
}

/* The RomFS image's layout (Switchbrew's "RomFS"; the hash and bucket count as Eden's fsmitm_romfsbuild): a header of
   ten u64s, then the directory hash table, the directory table, the file hash table and the file table, padded to
   0x200, then the file data. Table entries refer to each other by their offset in their table */
#define ROM_NONE      0xFFFFFFFFu
#define ROM_DIR_ENTRY 0x18 /* parent, sibling, first child, first file, next in bucket, name length; the name */
#define ROM_FILE_ENTRY                                                                                                 \
    0x20 /* parent, sibling, u64 data offset, u64 size, next in bucket, name length; the name */
static void put32(u8* p, u32 v) { memcpy(p, &v, 4); }
static void put64(u8* p, u64 v) { memcpy(p, &v, 8); }
/* the bucket of an entry named name in the directory at table offset parent. The name's bytes count unsigned */
static u32 rhash(u32 parent, const char* name) {
    const unsigned char* p = (const unsigned char*)name;
    u32 h = parent ^ 123456789u;
    while (*p) {
        h = (h >> 5) | (h << 27);
        h ^= *p++;
    }
    return h;
}
/* the buckets of a hash table for n entries */
static u32 hcount(u32 n) {
    u32 c = n;
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    while (c % 2 == 0 || c % 3 == 0 || c % 5 == 0 || c % 7 == 0 || c % 11 == 0 || c % 13 == 0 || c % 17 == 0) c++;
    return c;
}
static u64 entry_size(u32 fixed, const char* name) { return fixed + ((strlen(name) + 3) & ~(size_t)3); }
/* adds the folders and files under dir to the counts and table sizes */
static void scan(const FsNode* dir, u32* ndirs, u32* nfiles, u64* dir_bytes, u64* file_bytes) {
    const FsNode* c;
    for (c = dir->children; c; c = c->next) {
        if (*ndirs >= INT32_MAX || *nfiles >= INT32_MAX) fatal("RomFS: too many entries in romfs/\n");
        if (c->is_dir) {
            ++*ndirs;
            *dir_bytes += entry_size(ROM_DIR_ENTRY, c->name);
            scan(c, ndirs, nfiles, dir_bytes, file_bytes);
        } else {
            ++*nfiles;
            *file_bytes += entry_size(ROM_FILE_ENTRY, c->name);
        }
    }
}
/* a folder while the image is made: its table offset and its entry's links */
typedef struct {
    const FsNode* node;
    u32 parent; /* index in the folder list */
    u32 off, child, file;
} RDir;

/* Makes the image from the folder tree under dir (tree.lock held). Folders are listed breadth first, so each folder's
   subfolders are consecutive and chain as siblings, then each folder's files, folder by folder, in the index's strcmp
   order. File data follows the file table's order, each file at a multiple of 16: the file list is in offset order as
   it is made, and must not be sorted (a zero-size file shares its offset with the next file) */
static void romfs_build(FsNode* dir) {
    u32 ndirs = 1, nfiles = 0, i, nd, nf, dbuckets, fbuckets;
    u64 dir_bytes = ROM_DIR_ENTRY, file_bytes = 0, dhash, dtab, fhash, ftab, end, data = 0;
    RDir* dirs;
    u8* m;
    scan(dir, &ndirs, &nfiles, &dir_bytes, &file_bytes);
    if (dir_bytes >= ROM_NONE || file_bytes >= ROM_NONE) fatal("RomFS: the tables of romfs/ are too large\n");
    dbuckets = hcount(ndirs);
    fbuckets = hcount(nfiles);
    dhash = 0x50;
    dtab = dhash + (u64)dbuckets * 4;
    fhash = dtab + dir_bytes;
    ftab = fhash + (u64)fbuckets * 4;
    end = ftab + file_bytes;
    if (end > SIZE_MAX) fatal("RomFS: the tables of romfs/ are too large\n");
    if ((u64)ndirs * sizeof(RDir) > SIZE_MAX || (u64)nfiles * sizeof(RFile) > SIZE_MAX)
        fatal("RomFS: too many entries in romfs/\n");
    dirs = malloc(ndirs * sizeof *dirs);
    fsp.rfiles = malloc((nfiles ? nfiles : 1) * sizeof *fsp.rfiles);
    m = calloc(1, (size_t)end);
    if (!dirs || !fsp.rfiles || !m) fatal("RomFS: out of memory for the image of romfs/\n");
    memset(m + dhash, 0xFF, (size_t)dbuckets * 4);
    memset(m + fhash, 0xFF, (size_t)fbuckets * 4);

    /* the folders, breadth first, with their table offsets */
    dirs[0].node = dir;
    dirs[0].parent = 0;
    dirs[0].off = 0;
    nd = 1;
    {
        u32 off = ROM_DIR_ENTRY;
        for (i = 0; i < nd; i++) {
            const FsNode* c;
            dirs[i].child = dirs[i].file = ROM_NONE;
            for (c = dirs[i].node->children; c; c = c->next) {
                if (!c->is_dir) continue;
                if (dirs[i].child == ROM_NONE) dirs[i].child = off;
                dirs[nd].node = c;
                dirs[nd].parent = i;
                dirs[nd].off = off;
                nd++;
                off += (u32)entry_size(ROM_DIR_ENTRY, c->name);
            }
        }
    }
    /* the files, folder by folder, with their data offsets */
    nf = 0;
    {
        u32 off = 0;
        for (i = 0; i < nd; i++) {
            FsNode* c;
            u8* e = NULL;
            for (c = dirs[i].node->children; c; c = c->next) {
                u32 len = (u32)strlen(c->name), b;
                if (c->is_dir) continue;
                if (e) put32(e + 4, off); /* the previous file's sibling */
                if (dirs[i].file == ROM_NONE) dirs[i].file = off;
                data = (data + 15) & ~(u64)15;
                if (data > (u64)INT64_MAX - c->size) fatal("RomFS: romfs/ is too large\n");
                e = m + ftab + off;
                put32(e, dirs[i].off);
                put32(e + 4, ROM_NONE);
                put64(e + 8, data);
                put64(e + 16, c->size);
                b = rhash(dirs[i].off, c->name) % fbuckets;
                memcpy(e + 24, m + fhash + (u64)b * 4, 4);
                put32(m + fhash + (u64)b * 4, off);
                put32(e + 28, len);
                memcpy(e + ROM_FILE_ENTRY, c->name, len);
                fsp.rfiles[nf].node = c;
                fsp.rfiles[nf].off = data; /* relative for now */
                fsp.rfiles[nf].logged = fsp.rfiles[nf].warned = 0;
                fs_tree_retain(c);
                nf++;
                data += c->size;
                off += (u32)entry_size(ROM_FILE_ENTRY, c->name);
            }
        }
    }
    /* the folder entries, now that their first files are known */
    for (i = 0; i < nd; i++) {
        u8* e = m + dtab + dirs[i].off;
        const char* name = i ? dirs[i].node->name : "";
        u32 len = (u32)strlen(name), b, parent = dirs[dirs[i].parent].off;
        int last = i + 1 == nd || dirs[i + 1].parent != dirs[i].parent || !i;
        put32(e, parent);
        put32(e + 4, last ? ROM_NONE : dirs[i + 1].off);
        put32(e + 8, dirs[i].child);
        put32(e + 12, dirs[i].file);
        b = rhash(parent, name) % dbuckets;
        memcpy(e + 16, m + dhash + (u64)b * 4, 4);
        put32(m + dhash + (u64)b * 4, dirs[i].off);
        put32(e + 20, len);
        memcpy(e + ROM_DIR_ENTRY, name, len);
    }
    free(dirs);

    fsp.rmeta = m;
    fsp.rmeta_size = end;
    fsp.rdata_off = (end + 0x1FF) & ~(u64)0x1FF;
    if (data > (u64)INT64_MAX - fsp.rdata_off) fatal("RomFS: romfs/ is too large\n");
    for (i = 0; i < nf; i++) fsp.rfiles[i].off += fsp.rdata_off;
    fsp.nrfiles = (int)nf;
    fsp.rtotal = fsp.rdata_off + data;
    put64(m, 0x50);
    put64(m + 8, dhash);
    put64(m + 16, (u64)dbuckets * 4);
    put64(m + 24, dtab);
    put64(m + 32, dir_bytes);
    put64(m + 40, fhash);
    put64(m + 48, (u64)fbuckets * 4);
    put64(m + 56, ftab);
    put64(m + 64, file_bytes);
    put64(m + 72, fsp.rdata_off);
    LOG("RomFS: built from romfs/ (%u folders, %u files, %.1f MB)\n", nd, nf, (double)fsp.rtotal / 1048576.0);
}

void romfs_init(void) {
    pm_lock(&fsp.tree.lock);
    FsNode* node;
    if (!fs_tree_lookup(&fsp.tree, NULL, "romfs.bin", &node) && !node->is_dir) {
        if (node->fd >= 0)
            fsp.rimage = node;
        else if (!(fsp.rraw = node_open(node, "rb")))
            fatal("Could not open indexed romfs.bin");
        fsp.rtotal = node->size;
        LOG("RomFS: image (%.1f MB)\n", (double)fsp.rtotal / 1048576.0);
    } else if (!fs_tree_lookup(&fsp.tree, NULL, "romfs:/", &node) && node->is_dir) {
        romfs_build(node);
    } else {
        LOG("RomFS: none in %s\n", g_datadir);
        if (fs_tree_lookup(&fsp.tree, NULL, "exefs/main.nro", &node))
            WARN("Extract the game's RomFS into %sromfs/ before running it\n", g_datadir);
    }
    pm_unlock(&fsp.tree.lock);
}

/* an unmapped RomFS file open for reading (tree.lock held), NULL when it can't be: a node deleted from the index
   isn't opened, and a failed open isn't kept */
static FILE* rf_open(FsNode* node) {
    ROpen* slot = &fsp.openf[0];
    FILE* f;
    unsigned i;
    if (!node->attached) return NULL;
    for (i = 0; i < NOPEN; i++) {
        if (fsp.openf[i].f && fsp.openf[i].node == node) {
            fsp.openf[i].used = ++fsp.openclk;
            return fsp.openf[i].f;
        }
        if (slot->f && (!fsp.openf[i].f || fsp.openf[i].used < slot->used)) slot = &fsp.openf[i];
    }
    f = node_open(node, "rb");
    if (!f) return NULL;
    if (slot->f) fclose(slot->f);
    slot->node = node;
    slot->f = f;
    slot->used = ++fsp.openclk;
    return f;
}
/* size bytes of the file at offset rel into dst: 0, or -1 with the part that couldn't be read zeroed */
static int rfile_read(RFile* rf, u64 rel, u8* dst, u64 size) {
    FsNode* node = rf->node;
    i64 got = (i64)size;
    if (!__atomic_exchange_n(&rf->logged, 1, __ATOMIC_RELAXED))
        LOG("RomFS: reading %s (%.1f KB)\n", node->name, (double)node->size / 1024.0);
    if (node->fd >= 0) { /* mapped for the whole run, and never changes */
        memcpy(dst, node->map + rel, (size_t)size);
        return 0;
    }
    pm_lock(&fsp.tree.lock);
    if (node->is_virtual && node->attached)
        memcpy(dst, node->virtual_data + rel, (size_t)size);
    else {
        FILE* f = rf_open(node);
        got = f ? plat_pread(f, dst, size, rel) : -1;
    }
    pm_unlock(&fsp.tree.lock);
    if (got == (i64)size) return 0;
    if (got < 0) got = 0;
    memset(dst + got, 0, (size_t)(size - (u64)got));
    if (!__atomic_exchange_n(&rf->warned, 1, __ATOMIC_RELAXED))
        WARN("RomFS: could not read %s at %llx (%llx bytes): the game gets zeros\n", node->name,
             (unsigned long long)rel, (unsigned long long)size);
    return -1;
}
/* size bytes of the image at off into dst. The caller checked the range against rtotal. 0, or -1 when part of it
   couldn't be read from the host, that part zeroed */
static int romfs_read(u64 off, void* dst, u64 size) {
    u8* out = dst;
    int rc = 0;
    int lo, hi, k;
    if (fsp.rimage) {
        memcpy(out, fsp.rimage->map + off, (size_t)size);
        return 0;
    }
    if (fsp.rraw) {
        i64 got;
        pm_lock(&fsp.tree.lock);
        got = plat_pread(fsp.rraw, out, size, off);
        pm_unlock(&fsp.tree.lock);
        if (got == (i64)size) return 0;
        if (got < 0) got = 0;
        memset(out + got, 0, (size_t)(size - (u64)got));
        return -1;
    }
    if (off < fsp.rmeta_size) {
        u64 n = size < fsp.rmeta_size - off ? size : fsp.rmeta_size - off;
        memcpy(out, fsp.rmeta + off, (size_t)n);
        out += n;
        off += n;
        size -= n;
    }
    /* the last file starting at or before off; a zero-size file comes before the one sharing its offset */
    lo = 0;
    hi = fsp.nrfiles;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (fsp.rfiles[mid].off <= off)
            lo = mid + 1;
        else
            hi = mid;
    }
    k = lo - 1;
    while (size) {
        u64 n;
        while (k + 1 < fsp.nrfiles && fsp.rfiles[k + 1].off <= off) k++; /* past the end of file k */
        if (k >= 0 && off - fsp.rfiles[k].off < fsp.rfiles[k].node->size) { /* inside file k */
            RFile* rf = &fsp.rfiles[k];
            n = rf->off + rf->node->size - off;
            if (n > size) n = size;
            if (rfile_read(rf, off - rf->off, out, n)) rc = -1;
        } else { /* padding up to the next file, or past the last */
            n = k + 1 < fsp.nrfiles ? fsp.rfiles[k + 1].off - off : size;
            if (n > size) n = size;
            memset(out, 0, (size_t)n);
        }
        out += n;
        off += n;
        size -= n;
    }
    return rc;
}
/* ============================================================ IStorage: the RomFS (no data) or a system archive */
/* an IFileSystem's root folder, retained */
typedef struct {
    FsNode* root;
} FsData;
/* an IFile, or a system archive's IStorage (the RomFS's has no data). f is NULL for a node held in memory */
typedef struct {
    FILE* f;
    FsNode* node;
    u32 mode; /* OpenMode: 1 read, 2 write, 4 allow append */
} FileData;
/* an IDirectory: the entries filter picks (1 folders, 2 files), after the name last listed, until done */
typedef struct {
    FsNode* node;
    u32 filter;
    char last[FS_TREE_PATH_MAX]; /* a name, not a node: the entry may be deleted or renamed meanwhile */
    int done;
} DirData;

static int buffer_valid(u64 addr, u64 size) { return addr <= AS_SIZE && size <= AS_SIZE - addr; }
/* 64-bit file offsets: long is 32 bits on Windows. */
static int file_seek(FILE* f, u64 off) {
#if defined(_WIN32)
    return _fseeki64(f, (long long)off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}

/* Atmosphere's StorageInterfaceAdapter and FileInterfaceAdapter validate the signed arguments before the backend, in
   this order, which is observable on errors */
static int check_transfer(i64 offset, IpcBuf buffer, i64 size) {
    if (offset < 0) return FS_INVALID_OFFSET;
    if (size < 0) return FS_INVALID_SIZE;
    if ((i64)buffer.size < 0 || (u64)size > buffer.size) return FS_INVALID_SIZE;
    return 0;
}
static int nn_fssrv_sf_IStorage_Read(SvcObj* self, i64 offset, IpcBuf buffer, i64 size_in) {
    FileData* data = self->data;
    u64 off = (u64)offset, sz = (u64)size_in, a = buffer.addr;
    int rc = check_transfer(offset, buffer, size_in);
    if (rc) return rc;
    if (!sz) return 0;
    if (!data) { /* the RomFS */
        if (sz > (u64)INT64_MAX - off || off > fsp.rtotal || sz > fsp.rtotal - off) return FS_OUT_OF_RANGE;
        if (!buffer_valid(a, sz)) return FS_OUT_OF_RANGE;
        return romfs_read(off, GP(a), sz) ? FS_OUT_OF_RANGE : 0;
    }
    if (sz > (u64)INT64_MAX - off || !buffer_valid(a, sz)) return FS_OUT_OF_RANGE;
    pm_lock(&fsp.tree.lock);
    if (off > data->node->size || sz > data->node->size - off)
        rc = FS_OUT_OF_RANGE;
    else if (node_in_memory(data->node)) {
        if (sz) memcpy(GP(a), node_memory(data->node) + off, (size_t)sz);
    } else if (file_seek(data->f, off) || fread(GP(a), 1, (size_t)sz, data->f) != sz) {
        rc = FS_OUT_OF_RANGE;
    }
    pm_unlock(&fsp.tree.lock);
    return rc;
}
/* the read-only storages: their backend rejects Write and SetSize, after the adapter's argument checks. Flush
   succeeds for these immutable images */
static int nn_fssrv_sf_IStorage_Write(SvcObj* self, i64 offset, IpcBuf buffer, i64 size) {
    int rc = check_transfer(offset, buffer, size);
    return rc ? rc : RC(2, 6300);
}
static int nn_fssrv_sf_IStorage_Flush(SvcObj* self) { return 0; }
static int nn_fssrv_sf_IStorage_SetSize(SvcObj* self, i64 size) { return size < 0 ? FS_INVALID_SIZE : RC(2, 6300); }
static int nn_fssrv_sf_IStorage_GetSize(SvcObj* self, i64* size) {
    FileData* data = self->data;
    if (!data) {
        *size = (i64)fsp.rtotal;
        return 0;
    }
    pm_lock(&fsp.tree.lock);
    *size = (i64)data->node->size;
    pm_unlock(&fsp.tree.lock);
    return 0;
}
/* QueryRange (OperateRange): an all-zero QueryRangeInfo */
static int nn_fssrv_sf_IStorage_OperateRange(SvcObj* self, i32 operation_id, i64 offset, i64 size,
                                             nn_fs_QueryRangeInfo* info) {
    return 0;
}

/* ============================================================ Indexed IFileSystem */
static u32 read_path(IpcBuf b, char out[0x301]) {
    if (!b.addr || !b.size || !buffer_valid(b.addr, b.size)) return FS_DIR_UNOBTAINABLE;
    size_t limit = b.size < 0x301 ? (size_t)b.size : 0x301;
    const char* start = (const char*)GP(b.addr);
    const char* end = memchr(start, 0, limit);
    if (!end) return FS_PATH_TOO_LONG;
    memcpy(out, start, (size_t)(end - start) + 1);
    return 0;
}

static int file_truncate(FILE* f, u64 n, u64* actual) {
    if (fflush(f)) return -1;
#if defined(_WIN32)
    int rc = _chsize_s(_fileno(f), (long long)n) ? -1 : 0;
    if (rc) {
        /* A failed extension can still change the file. Inspect this open handle, never rediscover its name. */
        i64 size = _filelengthi64(_fileno(f));
        if (size >= 0) *actual = (u64)size;
    }
#else
    int rc = ftruncate(fileno(f), (off_t)n);
    if (rc) {
        struct stat st;
        if (!fstat(fileno(f), &st) && st.st_size >= 0) *actual = (u64)st.st_size;
    }
#endif
    if (!rc) *actual = n;
    return rc;
}

/* All service operations share tree.lock, including file size updates across different handles: file_disp,
   dir_disp and fs_disp hold it around the commands. So these objects are made with those dispatchers (file_new,
   OpenDirectory, hostfs_new), never by the generated _new. ipc.c drops a failed request's objects once the dispatcher
   returned, outside the lock an IFile's destroy takes */
static int nn_fssrv_sf_IFile_Read(SvcObj* self, nn_fs_ReadOption option, i64 offset, IpcBuf buffer, i64 size,
                                  i64* bytes_read) {
    FileData* f = self->data;
    u64 off = (u64)offset, sz = (u64)size, a = buffer.addr;
    /* IFile::Read -> DryRead: the order is observable on errors */
    int rc = check_transfer(offset, buffer, size);
    if (rc) return rc;
    if (!sz) return 0;
    if (sz > (u64)INT64_MAX - off) return FS_OUT_OF_RANGE;
    if (!(f->mode & 1)) return FS_READ_NOT_PERMITTED;
    if (off > f->node->size) return FS_OUT_OF_RANGE;
    if (sz > f->node->size - off) sz = f->node->size - off;
    if (!buffer_valid(a, sz)) return FS_OUT_OF_RANGE;
    u64 n = sz;
    if (node_in_memory(f->node)) {
        if (sz) memcpy(GP(a), node_memory(f->node) + off, (size_t)sz);
    } else {
        if (file_seek(f->f, off)) return FS_OUT_OF_RANGE;
        clearerr(f->f);
        n = fread(GP(a), 1, (size_t)sz, f->f);
        if (ferror(f->f)) return FS_OUT_OF_RANGE;
    }
    *bytes_read = (i64)n;
    return 0;
}
static int nn_fssrv_sf_IFile_Write(SvcObj* self, nn_fs_WriteOption option, i64 offset, IpcBuf buffer, i64 size) {
    FileData* f = self->data;
    u64 off = (u64)offset, sz = (u64)size, a = buffer.addr;
    int rc = check_transfer(offset, buffer, size);
    if (rc) return rc;
    /* IFile::Write succeeds without checking mode/EOF for an empty write, but honors Flush. */
    if (!sz) return (option & 1) && f->f && fflush(f->f) ? FS_NO_SPACE : 0;
    if (sz > (u64)INT64_MAX - off) return FS_OUT_OF_RANGE;
    if (!(f->mode & 2) || f->node->readonly) return FS_WRITE_NOT_PERMITTED;
    if (!buffer_valid(a, sz)) return FS_OUT_OF_RANGE;
    if (off + sz > f->node->size && !(f->mode & 4)) return FS_NO_APPEND;
    if (file_seek(f->f, off)) return FS_OUT_OF_RANGE;
    size_t n = fwrite(GP(a), 1, (size_t)sz, f->f);
    if (n && off + n > f->node->size) f->node->size = off + n;
    if (n != sz || fflush(f->f)) return FS_NO_SPACE;
    return 0;
}
static int nn_fssrv_sf_IFile_Flush(SvcObj* self) {
    FileData* f = self->data;
    return f->f && fflush(f->f) ? FS_NO_SPACE : 0;
}
static int nn_fssrv_sf_IFile_SetSize(SvcObj* self, i64 size) {
    FileData* f = self->data;
    if (size < 0) return FS_INVALID_SIZE;
    if (!(f->mode & 2) || f->node->readonly) return FS_WRITE_NOT_PERMITTED;
    if (file_truncate(f->f, (u64)size, &f->node->size)) return FS_NO_SPACE;
    return 0;
}
static int nn_fssrv_sf_IFile_GetSize(SvcObj* self, i64* size) {
    *size = (i64)((FileData*)self->data)->node->size;
    return 0;
}
static int nn_fssrv_sf_IFile_OperateRange(SvcObj* self, i32 operation_id, i64 offset, i64 size,
                                          nn_fs_QueryRangeInfo* info) {
    return 0;
}
static int file_disp(SvcObj* o, IpcCtx* c) {
    pm_lock(&fsp.tree.lock);
    int r = nn_fssrv_sf_IFile_dispatch(o, c);
    pm_unlock(&fsp.tree.lock);
    return r;
}
static void file_destroy(SvcObj* o) {
    FileData* data = o->data;
    pm_lock(&fsp.tree.lock);
    if (data->f != NULL) fclose(data->f);
    fs_tree_release(data->node);
    pm_unlock(&fsp.tree.lock);
}
/* an IFile on the node, or an IStorage for a system archive */
static SvcObj* file_new(FsNode* node, u32 mode, int storage, u32* rc) {
    if (node->is_dir) {
        *rc = FS_NOTFOUND;
        return NULL;
    }
    if (!(mode & 3) || (mode & ~7u)) {
        *rc = FS_INVALID_OPEN_MODE;
        return NULL;
    }
    if ((mode & 2) && node->readonly) {
        *rc = FS_WRITE_NOT_PERMITTED;
        return NULL;
    }
    /* a mapped node is readonly, so this handle only reads, from the mapping */
    FILE* host = node_in_memory(node) ? NULL : node_open(node, (mode & 2) ? "rb+" : "rb");
    if (!node_in_memory(node) && !host) {
        *rc = FS_NOTFOUND;
        return NULL;
    }
    SvcObj* o = storage ? nn_fssrv_sf_IStorage_new(sizeof(FileData))
                        : svcobj_new("nn::fssrv::sf::IFile", file_disp, sizeof(FileData));
    FileData* data = o->data;
    data->f = host;
    data->node = node;
    data->mode = mode;
    fs_tree_retain(node);
    o->destroy = file_destroy;
    *rc = 0;
    return o;
}

static int nn_fssrv_sf_IDirectory_Read(SvcObj* self, IpcBuf entries, i64* count) {
    DirData* d = self->data;
    u64 a = entries.addr, s = entries.size, n = 0;
    if (!buffer_valid(a, s)) return FS_OUT_OF_RANGE;
    u64 max = s / 0x310;
    FsNode* node = d->node->children;
    while (node && strcmp(node->name, d->last) <= 0) node = node->next;
    while (node && n < max && !d->done) {
        strcpy(d->last, node->name);
        if (node->is_dir ? (d->filter & 1) : (d->filter & 2)) {
            /* a 0x310-byte DirectoryEntry: the name (0x300 at most), the type at 0x304, the size at 0x308 */
            u8* entry = GP(a) + n++ * 0x310;
            u64 size = node->is_dir ? 0 : node->size;
            memset(entry, 0, 0x310);
            strncpy((char*)entry, node->name, 0x300);
            entry[0x304] = node->is_dir ? 0 : 1;
            memcpy(entry + 0x308, &size, 8);
        }
        node = node->next;
    }
    if (!node) d->done = 1;
    *count = (i64)n;
    return 0;
}
static int nn_fssrv_sf_IDirectory_GetEntryCount(SvcObj* self, i64* count) {
    DirData* d = self->data;
    u64 n = 0;
    for (FsNode* node = d->node->children; node; node = node->next)
        if (node->is_dir ? (d->filter & 1) : (d->filter & 2)) n++;
    *count = (i64)n;
    return 0;
}
/* a directory deleted or moved away answers NotFound to everything */
static int dir_disp(SvcObj* o, IpcCtx* c) {
    pm_lock(&fsp.tree.lock);
    int r = ((DirData*)o->data)->node->attached ? nn_fssrv_sf_IDirectory_dispatch(o, c) : (int)FS_NOTFOUND;
    pm_unlock(&fsp.tree.lock);
    return r;
}
static void dir_destroy(SvcObj* o) {
    DirData* d = o->data;
    pm_lock(&fsp.tree.lock);
    fs_tree_release(d->node);
    pm_unlock(&fsp.tree.lock);
}
static FsNode* fs_root(SvcObj* self) { return ((FsData*)self->data)->root; }
static int nn_fssrv_sf_IFileSystem_CreateFile(SvcObj* self, IpcBuf path, i64 size, i32 option) {
    char p[0x301];
    u32 rc;
    if (size < 0) return FS_INVALID_SIZE;
    if ((rc = read_path(path, p))) return (int)rc;
    return (int)tree_result(fs_tree_create(&fsp.tree, fs_root(self), p, 0, (u64)size, NULL));
}
static int nn_fssrv_sf_IFileSystem_DeleteFile(SvcObj* self, IpcBuf path) {
    char p[0x301];
    u32 rc;
    if ((rc = read_path(path, p))) return (int)rc;
    return (int)tree_result(fs_tree_remove(&fsp.tree, fs_root(self), p, 0, 0, 0));
}
static int nn_fssrv_sf_IFileSystem_CreateDirectory(SvcObj* self, IpcBuf path) {
    char p[0x301];
    u32 rc;
    if ((rc = read_path(path, p))) return (int)rc;
    return (int)tree_result(fs_tree_create(&fsp.tree, fs_root(self), p, 1, 0, NULL));
}
/* DeleteDirectory, DeleteDirectoryRecursively, CleanDirectoryRecursively (which keeps the directory itself) */
static int remove_dir(SvcObj* self, IpcBuf path, int recursive, int clean) {
    char p[0x301];
    u32 rc;
    if ((rc = read_path(path, p))) return (int)rc;
    return (int)tree_result(fs_tree_remove(&fsp.tree, fs_root(self), p, 1, recursive, clean));
}
static int nn_fssrv_sf_IFileSystem_DeleteDirectory(SvcObj* self, IpcBuf path) { return remove_dir(self, path, 0, 0); }
static int nn_fssrv_sf_IFileSystem_DeleteDirectoryRecursively(SvcObj* self, IpcBuf path) {
    return remove_dir(self, path, 1, 0);
}
static int nn_fssrv_sf_IFileSystem_CleanDirectoryRecursively(SvcObj* self, IpcBuf path) {
    return remove_dir(self, path, 1, 1);
}
static int rename_entry(SvcObj* self, IpcBuf old_path, IpcBuf new_path, int dir) {
    char p[0x301], p2[0x301];
    u32 rc;
    if ((rc = read_path(old_path, p))) return (int)rc;
    if ((rc = read_path(new_path, p2))) return (int)rc;
    return (int)tree_result(fs_tree_rename(&fsp.tree, fs_root(self), p, p2, dir));
}
static int nn_fssrv_sf_IFileSystem_RenameFile(SvcObj* self, IpcBuf old_path, IpcBuf new_path) {
    return rename_entry(self, old_path, new_path, 0);
}
static int nn_fssrv_sf_IFileSystem_RenameDirectory(SvcObj* self, IpcBuf old_path, IpcBuf new_path) {
    return rename_entry(self, old_path, new_path, 1);
}
static int nn_fssrv_sf_IFileSystem_GetEntryType(SvcObj* self, IpcBuf path, u32* type) {
    char p[0x301];
    u32 rc;
    FsNode* node;
    if ((rc = read_path(path, p))) return (int)rc;
    if ((rc = tree_result(fs_tree_lookup(&fsp.tree, fs_root(self), p, &node)))) return (int)rc;
    *type = node->is_dir ? 0 : 1;
    return 0;
}
static int nn_fssrv_sf_IFileSystem_OpenFile(SvcObj* self, IpcBuf path, u32 mode, SvcObj** file) {
    char p[0x301];
    u32 rc;
    FsNode* node;
    if ((rc = read_path(path, p))) return (int)rc;
    rc = tree_result(fs_tree_lookup(&fsp.tree, fs_root(self), p, &node));
    /* Lookup also normalizes the path. Normalization errors precede mode validation,
       but IFileSystem checks mode before its backend reports a missing file. */
    if (rc && rc != FS_NOTFOUND) return (int)rc;
    if (!(mode & 3) || (mode & ~7u)) return FS_INVALID_OPEN_MODE;
    if (rc) return (int)rc;
    *file = file_new(node, mode, 0, &rc);
    return (int)rc;
}
static int nn_fssrv_sf_IFileSystem_OpenDirectory(SvcObj* self, IpcBuf path, u32 mode, SvcObj** directory) {
    char p[0x301];
    u32 rc;
    FsNode* node;
    if ((rc = read_path(path, p))) return (int)rc;
    rc = tree_result(fs_tree_lookup(&fsp.tree, fs_root(self), p, &node));
    if (rc && rc != FS_NOTFOUND) return (int)rc;
    /* OpenDirectoryMode_NotRequireFileSize is bit 31, not another entry-type filter. */
    if (!(mode & 3) || (mode & ~0x80000003u)) return FS_INVALID_OPEN_MODE;
    if (rc) return (int)rc;
    if (!node->is_dir) return FS_NOTFOUND;
    SvcObj* dir = svcobj_new("nn::fssrv::sf::IDirectory", dir_disp, sizeof(DirData));
    DirData* data = dir->data;
    data->node = node;
    data->filter = mode;
    fs_tree_retain(node);
    dir->destroy = dir_destroy;
    *directory = dir;
    return 0;
}
static int nn_fssrv_sf_IFileSystem_GetFreeSpaceSize(SvcObj* self, IpcBuf path, i64* size) {
    *size = 4ll << 30;
    return 0;
}
static int nn_fssrv_sf_IFileSystem_GetTotalSpaceSize(SvcObj* self, IpcBuf path, i64* size) {
    *size = 8ll << 30;
    return 0;
}
/* a filesystem whose root was deleted or moved away answers NotFound to everything */
static int fs_disp(SvcObj* o, IpcCtx* c) {
    pm_lock(&fsp.tree.lock);
    int r = ((FsData*)o->data)->root->attached ? nn_fssrv_sf_IFileSystem_dispatch(o, c) : (int)FS_NOTFOUND;
    pm_unlock(&fsp.tree.lock);
    return r;
}
static void fs_destroy(SvcObj* o) {
    FsData* data = o->data;
    pm_lock(&fsp.tree.lock);
    fs_tree_release(data->root);
    pm_unlock(&fsp.tree.lock);
}
static SvcObj* hostfs_new(const char* root, u32* rc) { /* tree.lock held */
    FsNode* node;
    *rc = tree_result(fs_tree_mkdirs(&fsp.tree, root, &node));
    if (*rc) return NULL;
    SvcObj* o = svcobj_new("nn::fssrv::sf::IFileSystem", fs_disp, sizeof(FsData));
    ((FsData*)o->data)->root = node;
    fs_tree_retain(node);
    o->destroy = fs_destroy;
    return o;
}

/* the system archives' stand-in: a RomFS with an empty root folder and nothing else, 0x200 bytes */
static void emptyrom_init(void) {
    u8* m = fsp.emptyrom;
    u32 dbuckets = hcount(1), fbuckets = hcount(0);
    u64 dtab = 0x50 + (u64)dbuckets * 4, fhash = dtab + ROM_DIR_ENTRY, ftab = fhash + (u64)fbuckets * 4;
    memset(m + 0x50, 0xFF, (size_t)(dtab - 0x50));
    memset(m + fhash, 0xFF, (size_t)(ftab - fhash));
    put32(m + 0x50 + (u64)(rhash(0, "") % dbuckets) * 4, 0); /* the root, at offset 0 */
    put32(m + dtab, 0);                                       /* its own parent */
    put32(m + dtab + 4, ROM_NONE);
    put32(m + dtab + 8, ROM_NONE);
    put32(m + dtab + 12, ROM_NONE);
    put32(m + dtab + 16, ROM_NONE);
    put32(m + dtab + 20, 0);
    put64(m, 0x50);
    put64(m + 8, 0x50);
    put64(m + 16, (u64)dbuckets * 4);
    put64(m + 24, dtab);
    put64(m + 32, ROM_DIR_ENTRY);
    put64(m + 40, fhash);
    put64(m + 48, (u64)fbuckets * 4);
    put64(m + 56, ftab);
    put64(m + 64, 0);
    put64(m + 72, sizeof fsp.emptyrom);
}
/* save data folders, by SaveDataType */
static const char* const g_save_kinds[] = {"system", "user", "bcat", "device", "temp", "cache", "sysbcat"};
/* Earlier versions read the save type from the wrong byte, which is always 0, so every save went to save/system.
   An Account save (save/user) that doesn't exist yet takes over that folder, once */
static u32 save_migrate(void) { /* tree.lock held */
    FsNode* src;
    FsNode* dst;
    if (!fs_tree_lookup(&fsp.tree, NULL, "save/user", &dst)) return 0;
    if (fs_tree_lookup(&fsp.tree, NULL, "save/system", &src) || !src->is_dir || !src->children) return 0;
    FsTreeResult r = fs_tree_rename(&fsp.tree, NULL, "save/system", "save/user", 1);
    if (r)
        WARN("fs: could not move save/system to save/user\n");
    else
        LOG("fs: moved save/system to save/user\n");
    return tree_result(r);
}
/* the filesystems there are not here */
static int nn_fssrv_sf_IFileSystemProxy_OpenDataFileSystemByCurrentProcess(SvcObj* self, SvcObj** filesystem) {
    return RC(2, 1001);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenFileSystem(SvcObj* self, IpcBuf path, nn_fs_FileSystemProxyType type,
                                                       SvcObj** filesystem) {
    return RC(2, 1001);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenDataFileSystemByProgramId(SvcObj* self, nn_ncm_ProgramId program_id,
                                                                      SvcObj** filesystem) {
    return RC(2, 1001);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenBisStorage(SvcObj* self, u32 partition_id, SvcObj** storage) {
    return RC(2, 1001);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenGameCardStorage(SvcObj* self, u32 handle, u32 partition, SvcObj** storage) {
    return RC(2, 1001);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenImageDirectoryFileSystem(SvcObj* self, u32 storage_id,
                                                                     SvcObj** filesystem) {
    return RC(2, 1001);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenContentStorageFileSystem(SvcObj* self, u32 content_storage_id,
                                                                     SvcObj** filesystem) {
    return RC(2, 1001);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenDataStorageByPath(SvcObj* self, IpcBuf path,
                                                              nn_fs_ContentAttributes attributes, u32 type,
                                                              SvcObj** storage) {
    return RC(2, 1001);
}
/* index 0 is this program, the only one GetProgramIndexForAccessLog reports: its data filesystem is not here either */
static int nn_fssrv_sf_IFileSystemProxy_OpenDataFileSystemWithProgramIndex(SvcObj* self, u8 program_index,
                                                                           SvcObj** filesystem) {
    return program_index ? RC(2, 1002) : RC(2, 1001);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenFileSystemWithPatch(SvcObj* self, nn_ncm_ProgramId program_id,
                                                                nn_fs_FileSystemProxyType type, SvcObj** filesystem) {
    return RC(2, 1001);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenFileSystemWithId(SvcObj* self, IpcBuf path, nn_ncm_ProgramId program_id,
                                                             nn_fs_FileSystemProxyType type, SvcObj** filesystem) {
    return RC(2, 1001);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenBisFileSystem(SvcObj* self, IpcBuf path, u32 partition_id,
                                                          SvcObj** filesystem) {
    return RC(2, 1001);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenHostFileSystem(SvcObj* self, IpcBuf path, SvcObj** filesystem) {
    return RC(2, 1001);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenHostFileSystemWithOption(SvcObj* self, IpcBuf path, u32 option,
                                                                     SvcObj** filesystem) {
    return RC(2, 1001);
}
static int nn_fssrv_sf_IFileSystemProxy_FormatSdCardFileSystem(SvcObj* self) { return RC(2, 1001); }
static int nn_fssrv_sf_IFileSystemProxy_OpenGameCardFileSystem(SvcObj* self, u32 handle, u32 partition,
                                                               SvcObj** filesystem) {
    return RC(2, 1001);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenPatchDataStorageByCurrentProcess(SvcObj* self, SvcObj** storage) {
    return RC(2, 1001);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenRegisteredUpdatePartition(SvcObj* self, SvcObj** filesystem) {
    return RC(2, 1001);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenSdCardFileSystem(SvcObj* self, SvcObj** filesystem) {
    u32 rc;
    pm_lock(&fsp.tree.lock);
    *filesystem = hostfs_new("sdmc", &rc);
    pm_unlock(&fsp.tree.lock);
    return (int)rc;
}
/* the card OpenSdCardFileSystem serves is always there */
static int nn_fssrv_sf_IFileSystemProxy_IsSdCardAccessible(SvcObj* self, u8* accessible) {
    *accessible = 1;
    return 0;
}
/* one program, the first: a program count of 0 doesn't happen on the console */
static int nn_fssrv_sf_IFileSystemProxy_GetProgramIndexForAccessLog(SvcObj* self, u32* program_index,
                                                                    u32* program_count) {
    *program_count = 1;
    return 0;
}
/* save/<kind>, by the attribute's SaveDataType */
static int open_save(const nn_fs_SaveDataAttribute* attribute, SvcObj** filesystem) {
    char p[64];
    u8 type = attribute->type;
    const char* kind = type < sizeof g_save_kinds / sizeof g_save_kinds[0] ? g_save_kinds[type] : "system";
    snprintf(p, sizeof p, "save/%s", kind);
    pm_lock(&fsp.tree.lock);
    u32 rc = type == 1 ? save_migrate() : 0;
    *filesystem = rc ? NULL : hostfs_new(p, &rc);
    pm_unlock(&fsp.tree.lock);
    return (int)rc;
}
static int nn_fssrv_sf_IFileSystemProxy_OpenSaveDataFileSystem(SvcObj* self, nn_fs_SaveDataSpaceId space_id,
                                                               const nn_fs_SaveDataAttribute* attribute,
                                                               SvcObj** filesystem) {
    return open_save(attribute, filesystem);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenSaveDataFileSystemBySystemSaveDataId(
    SvcObj* self, nn_fs_SaveDataSpaceId space_id, const nn_fs_SaveDataAttribute* attribute, SvcObj** filesystem) {
    return open_save(attribute, filesystem);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenReadOnlySaveDataFileSystem(SvcObj* self, nn_fs_SaveDataSpaceId space_id,
                                                                       const nn_fs_SaveDataAttribute* attribute,
                                                                       SvcObj** filesystem) {
    return open_save(attribute, filesystem);
}
/* no save data to list */
static int info_reader(SvcObj** reader) {
    *reader = nn_fssrv_sf_ISaveDataInfoReader_new(0);
    return 0;
}
static int nn_fssrv_sf_IFileSystemProxy_OpenSaveDataInfoReader(SvcObj* self, SvcObj** reader) {
    return info_reader(reader);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenSaveDataInfoReaderBySaveDataSpaceId(SvcObj* self,
                                                                                nn_fs_SaveDataSpaceId space_id,
                                                                                SvcObj** reader) {
    return info_reader(reader);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenSaveDataInfoReaderOnlyCacheStorage(SvcObj* self, SvcObj** reader) {
    return info_reader(reader);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenDataStorageByCurrentProcess(SvcObj* self, SvcObj** storage) {
    if (!fsp.rtotal)
        WARN("fs: the game opens its RomFS, and %s has none (extract it into romfs/ there)\n", g_datadir);
    *storage = nn_fssrv_sf_IStorage_new(0);
    return 0;
}
/* nn::fs::QueryMountRomCacheSize(size_t*, int) and MountRom's program index overloads (Metroid Prime 4).
   Index 0 is this program, the only one GetProgramIndexForAccessLog reports. Any other index names a program
   the application doesn't have: TargetNotFound, never a success without a storage, which the SDK wraps as a
   storage with a null backend and calls through. */
static int nn_fssrv_sf_IFileSystemProxy_OpenDataStorageWithProgramIndex(SvcObj* self, u8 program_index,
                                                                        SvcObj** storage) {
    if (program_index) return RC(2, 1002);
    return nn_fssrv_sf_IFileSystemProxy_OpenDataStorageByCurrentProcess(self, storage);
}
/* this program's id is the only one there is: another program's is TargetNotFound */
static int nn_fssrv_sf_IFileSystemProxy_OpenDataStorageByProgramId(SvcObj* self, nn_ncm_ProgramId program_id,
                                                                   SvcObj** storage) {
    if (program_id != g_title_id) return RC(2, 1002);
    return nn_fssrv_sf_IFileSystemProxy_OpenDataStorageByCurrentProcess(self, storage);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenDataStorageByDataId(SvcObj* self, nn_ncm_DataId data_id,
                                                                nn_ncm_StorageId storage_id, SvcObj** storage) {
    char p[64];
    snprintf(p, sizeof p, "system/archives/%016llx.romfs", (unsigned long long)data_id);
    pm_lock(&fsp.tree.lock);
    FsNode* node;
    FsTreeResult r = fs_tree_lookup(&fsp.tree, NULL, p, &node);
    if (r == FS_TREE_NOTFOUND) {
        r = fs_tree_add_virtual(&fsp.tree, p, fsp.emptyrom, sizeof fsp.emptyrom, &node);
        if (!r) LOG("fs: provided empty system archive %016llx\n", (unsigned long long)data_id);
    }
    u32 rc = tree_result(r);
    *storage = rc ? NULL : file_new(node, 1, 1, &rc);
    pm_unlock(&fsp.tree.lock);
    return (int)rc;
}
static int nn_fssrv_sf_IFileSystemProxy_OpenDeviceOperator(SvcObj* self, SvcObj** device_operator) {
    *device_operator = nn_fssrv_sf_IDeviceOperator_new(0);
    return 0;
}
/* a notifier keeps one event (u[0]), which GetEventHandle gives each time. The card is always there, no game card
   comes or goes and system data never updates, so it is never signaled */
static void notifier_destroy(SvcObj* o) { kobj_unref(&((KEvent*)(size_t)o->u[0])->o); }
static SvcObj* notifier_new(const char* name) {
    SvcObj* o = nn_fssrv_sf_IEventNotifier_new(0);
    o->u[0] = (u64)(size_t)owned_event(name);
    o->destroy = notifier_destroy;
    return o;
}
static int nn_fssrv_sf_IEventNotifier_GetEventHandle(SvcObj* self, u32* event) {
    *event = ev_handle((KEvent*)(size_t)self->u[0]);
    return 0;
}
static int nn_fssrv_sf_IFileSystemProxy_OpenSdCardDetectionEventNotifier(SvcObj* self, SvcObj** notifier) {
    *notifier = notifier_new("sd card detection");
    return 0;
}
static int nn_fssrv_sf_IFileSystemProxy_OpenGameCardDetectionEventNotifier(SvcObj* self, SvcObj** notifier) {
    *notifier = notifier_new("game card detection");
    return 0;
}
static int nn_fssrv_sf_IFileSystemProxy_OpenSystemDataUpdateEventNotifier(SvcObj* self, SvcObj** notifier) {
    *notifier = notifier_new("system data update");
    return 0;
}
/* the SDK holds the prohibiter while it writes a save that mustn't be moved to another console meanwhile: nothing
   moves saves here, so it has nothing to do */
static int nn_fssrv_sf_IFileSystemProxy_OpenSaveDataTransferProhibiter(SvcObj* self,
                                                                       nn_ncm_ApplicationId application_id,
                                                                       SvcObj** prohibiter) {
    *prohibiter = nn_fssrv_sf_ISaveDataTransferProhibiter_new(0);
    return 0;
}
static int nn_fssrv_sf_IFileSystemProxy_OpenMultiCommitManager(SvcObj* self, SvcObj** manager) {
    *manager = nn_fssrv_sf_IMultiCommitManager_new(0);
    return 0;
}
static SvcObj* new_fsp(void) { return nn_fssrv_sf_IFileSystemProxy_new(0); }
static SvcObj* new_fsppr(void) { return nn_fssrv_sf_IProgramRegistry_new(0); }
static SvcObj* new_fspldr(void) { return nn_fssrv_sf_IFileSystemProxyForLoader_new(0); }

void svc_fs_init(void) {
    emptyrom_init();
    nn_fssrv_sf_IFileSystemProxy_register(new_fsp);
    nn_fssrv_sf_IProgramRegistry_register(new_fsppr);
    nn_fssrv_sf_IFileSystemProxyForLoader_register(new_fspldr);
}
