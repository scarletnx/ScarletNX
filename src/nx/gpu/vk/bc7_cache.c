/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The BC7 blocks of ASTC levels encoded before (astc.c), kept across runs in one file: shader_cache/<game>.vkbc7.
   Where the GPU samples BCn but not ASTC, a level encoded as BC7 costs the GPU 3 to 5 ms (the full encoder on 256x256,
   the fast build on 4096x4096, docs/layers/gpu/astc.md), and the memory budget's trim evicts textures that the game then uses
   again: one game encoded the same levels 6,000 to 14,000 times in 150 seconds. The blocks never change for the same
   ASTC bytes, so the first encode's blocks are kept, read back from the GPU when its submission is done, and every
   later upload of that level copies them into the image with no decode and no encode.

   The file is a header (BC7_MAGIC, BC7_FILE_VERSION and the version the caller gives: the encoder, the shaders' SPIR-V,
   the GPU and its driver, astc.c's vk_astc_cache_open) and records appended as they are made, by the cache's I/O
   thread: {u64 key[2], u64 bytes, u64 check} and the blocks, bytes of them, a multiple of 16. A file of another version starts over, and so does a
   file that would pass its bound. Only the index is in memory (an entry of 32 bytes a record): a hit reads its
   record's blocks from the file a piece at a time, through one buffer of BC7_PIECE bytes, checks them on the way and
   writes them into the staging memory given. A record cut short by a crash ends the reading, and the file is cut
   there; a record whose blocks don't check out on a read is dropped from the index, and the level is encoded again.
   The file is locked while it is open (snx_lock_file), so a second game process runs without the cache rather than
   writes the same file.

   Beside it, <file>.idx: the same header, then the index's entries in the file's order, {u64 key[2], u64 at, u64
   bytes} (BC7_IDX), one appended with each record. The cache opens from it, where the records' own headers lie all
   through the file: a racing game's 91,000 records, 4.3 GB, took minutes to open off a disk whose cache had let
   the file go, where its index is 3 MB. An entry is taken while it lies past the last one and its blocks within the
   file; the file is cut after the last one taken (a record after it, written as a crash came before its entry, is
   lost). Without an index of the file's version, the records' headers are read as before and the index written from
   them.

   A hit of an upload whose command buffer the recording thread records (record.c) is read on the cache's I/O thread
   (vk_bc7_cache_read_later): the GPU thread looks the record up in the index and goes on, the I/O thread reads and
   checks the blocks into a buffer of its own, and the recording thread puts them into staging just before it records
   their copy. When they don't check out, it leaves the copy out, and the GPU thread drops the record from the index
   and, at its next present, has the texture loaded again (vk_bc7_cache_take_failed), which encodes the level as a
   failed read at once did. Read on the GPU thread, one game's hits took that thread 1.3 s in 110 s (32,000 levels,
   2.5 GB). The I/O thread has its own handle on the file (Bc7Reader): on Windows a read at an offset moves the file
   position of a handle, and the GPU thread's appends go by that position */
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#if defined(__linux__)
#include <fcntl.h>
#endif
#include "common/platform/file.h"
#include "common/platform/time.h"
#include "common/utilities/bits.h"
#include "common/utilities/common.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/tracy.h"
#include "nx/gpu/vk/vk.h"

#define BC7_MAGIC        "SNXBC7\n"
#define BC7_FILE_VERSION 1
#define BC7_HEAD         32u /* the header: the magic's 8 bytes, the file version (u64) and the caller's version */
#define BC7_REC          32u /* a record's header: its key, its bytes and their check */
/* a hit's read goes through this many bytes at a time, which stay in the CPU's cache between the read and the copy */
#define BC7_PIECE        (1u << 20)
#define BC7_IDX          32u /* an index file's entry */

/* an index entry: a record's key and where its blocks start in the file */
typedef struct {
    u64 key[2];
    u64 at;   /* 0: a free slot (the header comes first) */
    u64 size; /* 0: a record that didn't check out, whose slot stays taken so that probes go on past it */
} Bc7Entry;

/* the reads off the GPU thread (vk_bc7_cache_read_later): the I/O thread's own handle and buffer, and the reads that
   the recording thread found did not check out, or left late. The GPU thread drops the first ones' records from the
   index (bc7_drop_failed), and gives all to their owners (vk_bc7_cache_take_failed). The atomics that more than one
   thread writes each have a cache line */
typedef struct {
    u64 key[2], at;
    void* owner; /* vk_bc7_cache_read_later's */
    u64 gva;
    int dropped; /* none to drop from the index: out of it already, or a read late or not made */
} Bc7Failed;
typedef struct {
    FILE* _Atomic f; /* atomic, the GPU thread's: NULL, the hits are read on the GPU thread */
    PMutex m; /* failed, nfailed and cap */
    u32 cap;
    Bc7Failed* failed;
    int init; /* m made */
    char pad0[64];
    _Atomic u32 nfailed; /* atomic: looked at without m */
    char pad1[64];
    _Atomic u64 ns; /* atomic: what the reads of both threads took */
    char pad2[64];
    _Atomic u64 bytes; /* atomic: what they read */
    char pad3[64];
    _Atomic u64 hits; /* atomic, the recording thread's */
    u8 piece[BC7_PIECE];
} Bc7Reader;

/* The cache's I/O thread. The file is a large one read at random, and a read of it can take seconds on a busy disk and
   minutes on one nearly full: the recording thread, which read the hits itself, sat 184 s in one read in a race, its
   ring filled, and the GPU thread drew nothing all that while (#2487). The reads run on a thread of their own, in the
   order they were asked, each into a buffer of its own, never straight into staging: a read that comes back after its
   frame would write over a ring handed out again. The recording thread takes a read that is done, and waits for one
   that is not while the disk answers: it leaves it (IO_LATE) once the read the I/O thread is on has run
   BC7_IO_LATE_NS, its copy left out, and its texture loads again, encoded (vk_bc7_cache_take_failed), the record
   kept. However long the read it wants takes while the disk answers: at a race's loads, reads off a busy disk that
   answered took 55 ms, and a bound of 50 ms on each left 15,000 of them to be encoded again. Once it left one, it
   waits for no other until the I/O thread has caught up.
   Meanwhile, and while a read runs past BC7_IO_LATE_NS, the GPU thread queues none (vk_bc7_cache_read_later answers
   0, and the level is encoded at once). The thread reads ahead of the recording thread by BC7_IO_AHEAD bytes at
   most */
/* reads queued at once at most: as many as the recording ring (record.c's 4 MiB) holds of their packets, so a load's
   burst of hits is never refused for want of one (1024 refused 489 of a race's reads off a disk that answered) */
#define BC7_IO_OPS      32768u
#define BC7_IO_LATE_NS 250000000ull /* a read running this long is a held disk's: none more is queued, and it is said */
#define BC7_IO_AHEAD   (64ull << 20)
#define BC7_IO_CLOSE_NS 5000000000ull /* a close waits this long for the writes queued */
/* an op's state: free, queued, being read, read and checked, read and not checked out (the record dropped), not
   read for want of memory or of the file (the record kept), left by the recording thread */
enum { IO_FREE, IO_QUEUED, IO_READING, IO_OK, IO_BAD, IO_FAIL, IO_LATE };
typedef struct {
    _Atomic u32 state;
    u64 key[2], at, size;
    u8* buf; /* IO_OK's blocks */
} Bc7Op;
/* The records the GPU thread puts go to the same thread, which writes them and their index entries: a write on a disk
   that holds it held the GPU thread as a read held the recording thread. A record is in the index at once and in the
   file once its write is done; the writes go before any read, so a read finds the blocks of a record put before it.
   BC7_IO_PUTS bytes of blocks wait at most: past that a record is not kept this run, its level encoded again in the
   next (counted) */
#define BC7_IO_PUTS (64ull << 20)
typedef struct Bc7Put {
    struct Bc7Put* next;
    u64 at;               /* where its header goes */
    u64 size;             /* its blocks' bytes; 0: the file and its index start over (a restart at the bound) */
    u64 rec[BC7_REC / 8]; /* its header, the check the I/O thread's to make */
    u8 blocks[];
} Bc7Put;
typedef struct {
    int started; /* the GPU thread's: 0 not yet, 1 running, -1 it could not start (reads and writes stay where asked) */
    PMutex m;    /* below, but for the atomics; ops' fields before they are queued and after they are done */
    PCond work;  /* the I/O thread waits on it for an op, or for room ahead */
    PCond done;  /* the recording thread waits on it for its read, and a close for the thread to be idle */
    Bc7Op ops[BC7_IO_OPS];
    u64 head;  /* ops queued so far: the next goes at head % BC7_IO_OPS */
    u64 next;  /* the next op the I/O thread takes */
    u64 ahead; /* bytes of reads done and not taken yet */
    u64 worst; /* the longest read since the last line about the late ones, in ns */
    int late;  /* the recording thread left a read: it waits for no other until the I/O thread has none queued */
    _Atomic u64 since;                 /* plat_ns when the read running began, 0 none: atomic, the GPU thread looks */
    _Atomic u64 n_late, n_skipped;     /* reads left late and not tried, since the last line that said so */
    _Atomic u64 all_late, all_skipped; /* the same in all, for the statistics */
    Bc7Put *puts, **puts_end;          /* the writes queued, oldest first */
    u64 put_bytes;                     /* their blocks' bytes */
    FILE *wf, *widx;     /* the file and its index, the thread's to write while the cache is open (NULL: none) */
    u8 hdr[BC7_HEAD];    /* their header, for a restart */
    char idx_path[1210]; /* the index file's, removed when a write to it fails */
    _Atomic int werr;    /* the errno of a write to the file that failed: the GPU thread turns the cache off */
    _Atomic u64 n_unkept, all_unkept; /* records not kept for want of room in the queue: since the last line, in all */
} Bc7Io;

typedef struct {
    FILE* f;   /* NULL: no cache */
    FILE* idx; /* the index file: NULL where it couldn't be written (the next run reads the records' headers) */
    intptr_t lock;
    char path[1200];
    u64 version[2];
    u64 end; /* where the next record goes */
    u64 max; /* the file's bound */
    Bc7Entry* map;
    u32 cap; /* slots in map, a power of two */
    u32 used;
    u64 hits, stored, restarts;
    int started_empty;   /* it held no level as it opened (vk_bc7_cache_cold) */
    u8 piece[BC7_PIECE]; /* what a close clears ends before it */
    Bc7Reader rd;        /* kept over a close: its failed reads go to their owners still */
    Bc7Io io;            /* kept over a close: the thread lives as long as the process */
} Bc7Cache;
static Bc7Cache g_bc7_cache;

void vk_bc7_content(u64 content[2], const void* p, u64 n) {
    u64 words[4] = {content[0], content[1], 0, 0};
    vk_hash128(p, n, words + 2);
    vk_hash128(words, sizeof words, content);
}

void vk_bc7_key(const u64 content[2], const VkBc7Level* lv, u64 key[2]) {
    const u64 words[7] = {content[0],
                          content[1],
                          lv->w | (u64)lv->h << 32,
                          lv->depth | (u64)lv->layers << 32,
                          lv->bw | (u64)lv->bh << 32,
                          lv->srgb | (u64)lv->build << 32,
                          VK_BC7_ENCODER_VERSION};
    vk_hash128(words, sizeof words, key);
}

/* a payload's check: its pieces' hashes, folded */
static u64 bc7_check_step(u64 h, u64 piece) { return snx_fnv_step(h, piece); }
static u64 bc7_check(const u8* p, u64 size) {
    u64 h = SNX_FNV_BASIS;
    for (u64 o = 0; o < size; o += BC7_PIECE)
        h = bc7_check_step(h, vk_hash(p + o, size - o < BC7_PIECE ? size - o : BC7_PIECE));
    return h;
}

/* key's slot: its entry, or the free slot where it would go */
static Bc7Entry* bc7_slot(const u64 key[2]) {
    for (u32 i = (u32)key[0] & (g_bc7_cache.cap - 1);; i = (i + 1) & (g_bc7_cache.cap - 1)) {
        Bc7Entry* e = &g_bc7_cache.map[i];
        if (!e->at || (e->key[0] == key[0] && e->key[1] == key[1])) return e;
    }
}
/* the record of key at `at` in the index, over an older one of the same key: 0 when memory ran out */
static int bc7_index(const u64 key[2], u64 at, u64 size) {
    if ((u64)(g_bc7_cache.used + 1) * 2 > g_bc7_cache.cap) {
        u32 cap = g_bc7_cache.cap * 2;
        Bc7Entry *old = g_bc7_cache.map, *map = cap ? calloc(cap, sizeof *map) : NULL;
        if (!map) return 0;
        u32 was = g_bc7_cache.cap;
        g_bc7_cache.map = map;
        g_bc7_cache.cap = cap;
        for (u32 i = 0; i < was; i++)
            if (old[i].at) *bc7_slot(old[i].key) = old[i];
        free(old);
    }
    Bc7Entry* e = bc7_slot(key);
    g_bc7_cache.used += !e->at;
    *e = (Bc7Entry){{key[0], key[1]}, at, size};
    return 1;
}

/* the header of the file and of its index */
static void bc7_head(u8 head[BC7_HEAD]) {
    const u64 version = BC7_FILE_VERSION;
    memset(head, 0, BC7_HEAD);
    memcpy(head, BC7_MAGIC, 8);
    memcpy(head + 8, &version, 8);
    memcpy(head + 16, g_bc7_cache.version, 16);
}
/* the index file given up for this run: the next one reads the records' headers */
static void bc7_idx_off(void) {
    if (!g_bc7_cache.idx) return;
    fclose(g_bc7_cache.idx);
    g_bc7_cache.idx = NULL;
    char path[1210];
    snprintf(path, sizeof path, "%s.idx", g_bc7_cache.path);
    remove(path);
}
/* the index file emptied to its header */
static void bc7_idx_start_over(void) {
    u8 head[BC7_HEAD];
    bc7_head(head);
    if (g_bc7_cache.idx && (snx_seek(g_bc7_cache.idx, 0) || fwrite(head, 1, BC7_HEAD, g_bc7_cache.idx) != BC7_HEAD ||
                            fflush(g_bc7_cache.idx) || snx_cut(g_bc7_cache.idx, BC7_HEAD)))
        bc7_idx_off();
}
/* a record's entry appended to the index file */
static void bc7_idx_put(const u64 key[2], u64 at, u64 size) {
    const u64 e[BC7_IDX / 8] = {key[0], key[1], at, size};
    if (g_bc7_cache.idx && (fseek(g_bc7_cache.idx, 0, SEEK_END) || fwrite(e, 1, BC7_IDX, g_bc7_cache.idx) != BC7_IDX ||
                            fflush(g_bc7_cache.idx)))
        bc7_idx_off();
}
/* the index from the index file, over a file of size bytes: where its records end, 0 where the index file is not
   this file's (another version, or not there) */
static u64 bc7_idx_load(u64 size) {
    u8 head[BC7_HEAD], have[BC7_HEAD];
    bc7_head(head);
    if (!g_bc7_cache.idx || snx_seek(g_bc7_cache.idx, 0) || fread(have, 1, BC7_HEAD, g_bc7_cache.idx) != BC7_HEAD ||
        memcmp(head, have, BC7_HEAD))
        return 0;
    u64 end = BC7_HEAD, n = 0, e[(64u << 10) / 8];
    size_t got;
    int done = 0;
    while (!done && (got = fread(e, 1, sizeof e, g_bc7_cache.idx)) >= BC7_IDX) {
        for (size_t i = 0; i + BC7_IDX <= got && !done; i += BC7_IDX) {
            const u64* r = e + i / 8;
            /* past the last record, its blocks a whole number of 16 bytes within the file */
            if (r[2] < end + BC7_REC || !r[3] || r[3] % 16 || r[3] > size || r[2] > size - r[3] ||
                !bc7_index(r, r[2], r[3]))
                done = 1;
            else {
                end = r[2] + r[3];
                n++;
            }
        }
        done |= got < sizeof e;
    }
    /* the entries taken, and nothing after them */
    if (snx_cut(g_bc7_cache.idx, BC7_HEAD + n * BC7_IDX)) return 0;
    return end;
}
/* the index file written anew from the index, its entries in the file's order */
static int bc7_at_order(const void* a, const void* b) {
    const u64 x = ((const Bc7Entry*)a)->at, y = ((const Bc7Entry*)b)->at;
    return (x > y) - (x < y);
}
static void bc7_idx_write(void) {
    bc7_idx_start_over();
    if (!g_bc7_cache.idx) return;
    Bc7Entry* all = malloc(((size_t)g_bc7_cache.used + 1) * sizeof *all);
    if (!all) {
        bc7_idx_off();
        return;
    }
    u32 n = 0;
    for (u32 i = 0; i < g_bc7_cache.cap; i++)
        if (g_bc7_cache.map[i].at) all[n++] = g_bc7_cache.map[i];
    qsort(all, n, sizeof *all, bc7_at_order);
    for (u32 i = 0; i < n && g_bc7_cache.idx; i++) {
        const u64 e[BC7_IDX / 8] = {all[i].key[0], all[i].key[1], all[i].at, all[i].size};
        if (fwrite(e, 1, BC7_IDX, g_bc7_cache.idx) != BC7_IDX) bc7_idx_off();
    }
    if (g_bc7_cache.idx && fflush(g_bc7_cache.idx)) bc7_idx_off();
    free(all);
}

/* the file emptied to its header, and the index with it: 0 when it couldn't be written */
static int bc7_start_over(void) {
    u8 head[BC7_HEAD];
    bc7_head(head);
    memset(g_bc7_cache.map, 0, (size_t)g_bc7_cache.cap * sizeof *g_bc7_cache.map);
    g_bc7_cache.used = 0;
    g_bc7_cache.end = BC7_HEAD;
    bc7_idx_start_over();
    return !snx_seek(g_bc7_cache.f, 0) && fwrite(head, 1, BC7_HEAD, g_bc7_cache.f) == BC7_HEAD &&
           !fflush(g_bc7_cache.f) && !snx_cut(g_bc7_cache.f, BC7_HEAD);
}

static int io_idle_by(u64 until);
/* the I/O thread's handle closed once the reads queued before are done: the recording thread's packets that take them,
   then the thread's own reads. It stays open where they do not end in 2 s each (a fatal error stops the recording
   thread inside a packet, a disk holds a read): a read queued still takes the handle that it finds, or fails with
   none. The failed reads stay for their owners */
static void bc7_reader_close(void) {
    FILE* f = atomic_load_explicit(&g_bc7_cache.rd.f, memory_order_acquire);
    const int done = !f || (vk_rec_sync_until(plat_ns() + 2000000000ull) && io_idle_by(plat_ns() + 2000000000ull));
    atomic_store_explicit(&g_bc7_cache.rd.f, NULL, memory_order_release);
    if (f && done) fclose(f);
}

/* the writes queued: made, by until; or else dropped, the file and its index left open to a write the thread may still
   be in, and the file kept locked for this run. 1 when they were made */
static int io_writes_done(u64 until) {
    Bc7Io* io = &g_bc7_cache.io;
    if (io->started <= 0) return 1;
    const int done = io_idle_by(until);
    pm_lock(&io->m);
    u64 dropped = 0;
    for (Bc7Put* w = io->puts; w;) {
        Bc7Put* next = w->next;
        dropped += w->size;
        free(w);
        w = next;
    }
    io->puts = NULL;
    io->puts_end = &io->puts;
    io->put_bytes = 0;
    io->wf = io->widx = NULL;
    pm_unlock(&io->m);
    if (!done)
        WARN("vulkan: the BC7 cache's last writes were not made, %llu KiB of records: the disk had not taken them in "
             "%.0f s\n",
             (unsigned long long)(dropped >> 10), (double)BC7_IO_CLOSE_NS / 1e9);
    return done;
}
void vk_bc7_cache_close(void) {
    bc7_reader_close();
    if (!g_bc7_cache.f) return;
    FILE* idx = g_bc7_cache.idx;
    if (g_bc7_cache.io.started > 0) { /* the thread's, which closes the index file when a write to it fails */
        pm_lock(&g_bc7_cache.io.m);
        if (g_bc7_cache.io.wf == g_bc7_cache.f) idx = g_bc7_cache.io.widx;
        pm_unlock(&g_bc7_cache.io.m);
    }
    if (io_writes_done(plat_ns() + BC7_IO_CLOSE_NS)) {
        if (idx) fclose(idx);
        fclose(g_bc7_cache.f);
        snx_unlock_file(g_bc7_cache.lock);
    }
    free(g_bc7_cache.map);
    memset(&g_bc7_cache, 0, offsetof(Bc7Cache, piece)); /* its path and buffer make it over a megabyte */
}

/* the cache off from now on, for why (an error writing or reading the file) */
static void bc7_off(const char* why) {
    WARN("vulkan: the BC7 cache %s is off for this run: %s\n", g_bc7_cache.path, why);
    vk_bc7_cache_close();
}

static int io_start(void);
/* the cache open: the I/O thread's own handle to read with, and the file and its index its to write */
static void bc7_io_open(void) {
    Bc7Io* io = &g_bc7_cache.io;
    if (!g_bc7_cache.rd.init) {
        pm_init(&g_bc7_cache.rd.m);
        g_bc7_cache.rd.init = 1;
    }
    if (!io_start()) return;
    FILE* f = snx_fopen(g_bc7_cache.path, "rb");
    if (!f) LOG("vulkan: the BC7 cache's hits are read on the GPU thread: a second handle on it failed\n");
#if defined(__linux__)
    /* no readahead past a record, as for the cache's own handle (below) */
    else
        posix_fadvise(fileno(f), 0, 0, POSIX_FADV_RANDOM);
#endif
    atomic_store_explicit(&g_bc7_cache.rd.f, f, memory_order_release);
    pm_lock(&io->m);
    io->wf = g_bc7_cache.f;
    io->widx = g_bc7_cache.idx;
    bc7_head(io->hdr);
    snprintf(io->idx_path, sizeof io->idx_path, "%s.idx", g_bc7_cache.path);
    atomic_store_explicit(&io->werr, 0, memory_order_relaxed);
    pm_unlock(&io->m);
}
/* the cache, unless a write of the I/O thread failed: then it is off (1: it is open) */
static int bc7_writes_ok(void) {
    const int err = atomic_load_explicit(&g_bc7_cache.io.werr, memory_order_acquire);
    if (!g_bc7_cache.f || !err) return g_bc7_cache.f != NULL;
    char why[300];
    snprintf(why, sizeof why, "a record could not be written (%s)", strerror(err));
    bc7_off(why);
    return 0;
}

int vk_bc7_cache_open(const char* path, const u64 version[2], u64 max) {
    vk_bc7_cache_close();
    if (snprintf(g_bc7_cache.path, sizeof g_bc7_cache.path, "%s", path) >= (int)sizeof g_bc7_cache.path) return 0;
    int locked = snx_lock_file(path, &g_bc7_cache.lock);
    if (locked) {
        LOG("vulkan: the BC7 cache %s is %s: none this run\n", path,
            locked > 0 ? "in use by another process" : "not to be locked");
        return 0;
    }
    g_bc7_cache.f = snx_fopen(path, "r+b");
    if (!g_bc7_cache.f) {
        snx_unlock_file(g_bc7_cache.lock);
        memset(&g_bc7_cache, 0, offsetof(Bc7Cache, piece));
        return 0;
    }
    g_bc7_cache.map = calloc(1024, sizeof *g_bc7_cache.map);
    if (!g_bc7_cache.map) {
        bc7_off("out of memory");
        return 0;
    }
    g_bc7_cache.cap = 1024;
    g_bc7_cache.max = max;
    memcpy(g_bc7_cache.version, version, 16);
    {
        char idx[1210];
        snprintf(idx, sizeof idx, "%s.idx", path);
        g_bc7_cache.idx = snx_fopen(idx, "r+b");
        if (!g_bc7_cache.idx) g_bc7_cache.idx = snx_fopen(idx, "w+b");
    }
    u64 head[BC7_HEAD / 8];
    int same = fread(head, 1, BC7_HEAD, g_bc7_cache.f) == BC7_HEAD && !memcmp(head, BC7_MAGIC, 8) &&
               head[1] == BC7_FILE_VERSION && head[2] == version[0] && head[3] == version[1];
    if (!same) {
        if (!bc7_start_over()) {
            bc7_off("its header could not be written");
            return 0;
        }
        LOG("vulkan: the BC7 cache %s starts empty\n", path);
        g_bc7_cache.started_empty = 1;
        bc7_io_open();
        return 1;
    }
    /* the records' headers, each where the one before it ends, until one is cut short or makes no sense. Read where
       they lie (plat_pread), not through the stream: after a seek each read of 32 bytes filled the stream's buffer of a
       block (128 KiB on ZFS), and a racing game's cache of 84,000 records, 4.3 GB, took minutes to open */
    if (fseek(g_bc7_cache.f, 0, SEEK_END)) {
        bc7_off("its size could not be read");
        return 0;
    }
    const int64_t told = snx_tell(g_bc7_cache.f);
    const u64 size = told < 0 ? 0 : (u64)told;
    u64 at = bc7_idx_load(size), bytes = 0;
    const int from_idx = at != 0;
    if (from_idx)
        for (u32 i = 0; i < g_bc7_cache.cap; i++) bytes += g_bc7_cache.map[i].at ? g_bc7_cache.map[i].size : 0;
    else
        at = BC7_HEAD;
    while (!from_idx && size - at >= BC7_REC) {
        u64 rec[BC7_REC / 8];
        if (plat_pread(g_bc7_cache.f, rec, BC7_REC, at) != BC7_REC) break;
        if (!rec[2] || rec[2] % 16 || rec[2] > size - at - BC7_REC) break;
        if (!bc7_index(rec, at + BC7_REC, rec[2])) break;
        bytes += rec[2];
        at += BC7_REC + rec[2];
    }
    g_bc7_cache.end = at;
    if (at < size && snx_cut(g_bc7_cache.f, at)) {
        bc7_off("a record cut short could not be cut off");
        return 0;
    }
    if (!from_idx) bc7_idx_write();
#if defined(__linux__)
    /* a hit reads one level's records, wherever they lie: the kernel's readahead past them (64 MiB a read on one USB
       disk of 40 MB/s) took that disk from the game's own reads for seconds while a very large level's pieces loaded
       (astc.c's bg_load) */
    posix_fadvise(fileno(g_bc7_cache.f), 0, 0, POSIX_FADV_RANDOM);
#endif
    LOG("vulkan: the BC7 cache %s holds %u levels of %llu MB\n", path, g_bc7_cache.used,
        (unsigned long long)(bytes >> 20));
    g_bc7_cache.started_empty = !g_bc7_cache.used;
    bc7_io_open();
    return 1;
}
int vk_bc7_cache_cold(void) { return g_bc7_cache.started_empty; } /* a close clears it */

/* size bytes of key's record, its blocks at `at`, read from f through piece and checked on the way into dst: 1, 0 when
   they don't check out, -1 when its header can't be read */
static int bc7_read_at(FILE* f, u8* piece, const u64 key[2], u64 at, u64 size, u8* dst) {
    u64 rec[BC7_REC / 8];
    const u64 t0 = plat_ns();
    /* where they lie, as the index's scan reads them: through the stream, a record of 16 KiB read a block of 128 */
    if (plat_pread(f, rec, BC7_REC, at - BC7_REC) != BC7_REC) return -1;
    u64 h = SNX_FNV_BASIS;
    for (u64 o = 0; o < size && rec[0] == key[0] && rec[1] == key[1] && rec[2] == size; o += BC7_PIECE) {
        size_t n = (size_t)(size - o < BC7_PIECE ? size - o : BC7_PIECE);
        if (plat_pread(f, piece, n, at + o) != (int64_t)n) break;
        h = bc7_check_step(h, vk_hash_copy(dst + o, piece, n));
    }
    atomic_fetch_add_explicit(&g_bc7_cache.rd.ns, plat_ns() - t0, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_bc7_cache.rd.bytes, size, memory_order_relaxed);
    return rec[0] == key[0] && rec[1] == key[1] && rec[2] == size && h == rec[3];
}
static void bc7_failed_log(void) {
    LOG_LIMIT(8, "vulkan: a record of the BC7 cache did not check out: dropped, its level is encoded again\n");
}
/* the records of the reads that the recording thread found did not check out, dropped from the index: probes go on
   past them, and the next upload of their levels encodes them. Only the record that was read: one written since at
   another place stays */
static void bc7_drop_failed(void) {
    if (!atomic_load_explicit(&g_bc7_cache.rd.nfailed, memory_order_acquire) || !g_bc7_cache.map) return;
    pm_lock(&g_bc7_cache.rd.m);
    for (u32 i = 0; i < atomic_load_explicit(&g_bc7_cache.rd.nfailed, memory_order_relaxed); i++) {
        Bc7Failed* x = &g_bc7_cache.rd.failed[i];
        Bc7Entry* e = x->dropped ? NULL : bc7_slot(x->key);
        if (e && e->at == x->at) e->size = 0;
        x->dropped = 1;
    }
    pm_unlock(&g_bc7_cache.rd.m);
}

int vk_bc7_cache_failed_pending(void) {
    return atomic_load_explicit(&g_bc7_cache.rd.nfailed, memory_order_acquire) != 0;
}
void vk_bc7_cache_take_failed(void (*fn)(void* owner, u64 gva, const u64 key[2])) {
    if (!atomic_load_explicit(&g_bc7_cache.rd.nfailed, memory_order_acquire)) return;
    bc7_drop_failed();
    pm_lock(&g_bc7_cache.rd.m);
    Bc7Failed* failed = g_bc7_cache.rd.failed;
    const u32 n = atomic_load_explicit(&g_bc7_cache.rd.nfailed, memory_order_relaxed);
    g_bc7_cache.rd.failed = NULL;
    g_bc7_cache.rd.cap = 0;
    atomic_store_explicit(&g_bc7_cache.rd.nfailed, 0, memory_order_release);
    pm_unlock(&g_bc7_cache.rd.m);
    for (u32 i = 0; i < n; i++) fn(failed[i].owner, failed[i].gva, failed[i].key);
    free(failed);
}

/* key's entry when it holds size bytes, after the reads that failed are out of the index: NULL when it doesn't */
static Bc7Entry* bc7_entry(const u64 key[2], u64 size) {
    bc7_drop_failed();
    Bc7Entry* e = bc7_slot(key);
    return e->at && size && e->size == size ? e : NULL;
}

u64 vk_bc7_cache_find(const u64 key[2]) {
    if (!g_bc7_cache.f) return 0;
    bc7_drop_failed();
    const Bc7Entry* e = bc7_slot(key);
    return e->at ? e->size : 0;
}

static u32 io_take(Bc7Op* op);
static int io_queue_read(const u64 key[2], u64 at, u64 size, u64* n);
int vk_bc7_cache_read(const u64 key[2], u8* dst, u64 size) {
    if (!bc7_writes_ok()) return 0;
    Bc7Entry* e = bc7_entry(key, size);
    if (!e) return 0;
    /* through the I/O thread when it runs, after the writes queued: waited for as the recording thread waits, the
       level encoded when it is late */
    if (g_bc7_cache.io.started > 0 && atomic_load_explicit(&g_bc7_cache.rd.f, memory_order_acquire)) {
        u64 n;
        if (!io_queue_read(key, e->at, size, &n)) return 0;
        Bc7Op* op = &g_bc7_cache.io.ops[n % BC7_IO_OPS];
        const u32 s = io_take(op);
        if (s == IO_OK) {
            memcpy(dst, op->buf, (size_t)size);
            free(op->buf);
            op->buf = NULL;
        }
        if (s != IO_LATE) atomic_store_explicit(&op->state, IO_FREE, memory_order_release);
        if (s == IO_BAD) {
            bc7_failed_log();
            e->size = 0;
        }
        g_bc7_cache.hits += s == IO_OK;
        return s == IO_OK;
    }
    const int r = bc7_read_at(g_bc7_cache.f, g_bc7_cache.piece, key, e->at, size, dst);
    if (r < 0) {
        bc7_off("a record could not be read");
        return 0;
    }
    if (!r) {
        bc7_failed_log();
        e->size = 0;
        return 0;
    }
    g_bc7_cache.hits++;
    return 1;
}

/* the I/O thread: each op in the order queued, the reads past BC7_IO_LATE_NS said once they end, with the reads the
   recording thread left and those the GPU thread did not ask meanwhile */
static void io_said(u64 size, u64 took, u64 worst) {
    const u64 late = atomic_exchange_explicit(&g_bc7_cache.io.n_late, 0, memory_order_relaxed);
    const u64 skipped = atomic_exchange_explicit(&g_bc7_cache.io.n_skipped, 0, memory_order_relaxed);
    if (size)
        LOG("vulkan: a read of %llu KiB from the BC7 cache took %.2f s: %llu reads came too late meanwhile and %llu "
            "were not tried, their levels encoded instead\n",
            (unsigned long long)(size >> 10), (double)took / 1e9, (unsigned long long)late,
            (unsigned long long)skipped);
    else if (late || skipped)
        LOG("vulkan: the BC7 cache's reads caught up: %llu more came too late and %llu were not tried, their levels "
            "encoded instead (the longest read took %.0f ms)\n",
            (unsigned long long)late, (unsigned long long)skipped, (double)worst / 1e6);
}
/* the I/O thread's index file given up, as bc7_idx_off */
static void io_idx_off(Bc7Io* io, FILE* x) {
    fclose(x);
    pm_lock(&io->m);
    if (io->widx == x) io->widx = NULL;
    pm_unlock(&io->m);
    remove(io->idx_path);
}
/* w written: its record and its index entry, or (size 0) the file and its index emptied to their header. 0, or the
   errno of the file's write that failed (a failed write to the index gives it up, as bc7_idx_off) */
static int io_write(Bc7Io* io, FILE* f, FILE* x, Bc7Put* w) {
    const int fd = fileno(f);
    int err = 0;
    if (!w->size) {
        if (plat_pwrite_fd(fd, io->hdr, BC7_HEAD, 0) != BC7_HEAD || snx_cut(f, BC7_HEAD)) err = errno ? errno : EIO;
        if (x && (plat_pwrite_fd(fileno(x), io->hdr, BC7_HEAD, 0) != BC7_HEAD || snx_cut(x, BC7_HEAD)))
            io_idx_off(io, x);
        return err;
    }
    w->rec[3] = bc7_check(w->blocks, w->size);
    if (plat_pwrite_fd(fd, w->rec, BC7_REC, w->at) != BC7_REC ||
        plat_pwrite_fd(fd, w->blocks, w->size, w->at + BC7_REC) != (int64_t)w->size)
        return errno ? errno : EIO;
    const u64 e[BC7_IDX / 8] = {w->rec[0], w->rec[1], w->at + BC7_REC, w->size};
    if (x && (fseek(x, 0, SEEK_END) || fwrite(e, 1, BC7_IDX, x) != BC7_IDX || fflush(x))) io_idx_off(io, x);
    return 0;
}
/* under m: the oldest write queued, made with m let go; after a failed one the file is written no more */
static void io_put(Bc7Io* io) {
    Bc7Put* w = io->puts;
    io->puts = w->next;
    if (!io->puts) io->puts_end = &io->puts;
    FILE *f = atomic_load_explicit(&io->werr, memory_order_relaxed) ? NULL : io->wf, *x = io->widx;
    const u64 t0 = plat_ns();
    atomic_store_explicit(&io->since, t0, memory_order_release);
    pm_unlock(&io->m);
    const int err = f ? io_write(io, f, x, w) : 0;
    if (err) atomic_store_explicit(&io->werr, err, memory_order_release);
    const u64 took = plat_ns() - t0;
    if (took >= BC7_IO_LATE_NS && w->size)
        LOG("vulkan: a write of %llu KiB to the BC7 cache took %.2f s: %llu new levels were not kept meanwhile, more "
            "than %llu MiB waiting to be written\n",
            (unsigned long long)(w->size >> 10), (double)took / 1e9,
            (unsigned long long)atomic_exchange_explicit(&io->n_unkept, 0, memory_order_relaxed),
            (unsigned long long)(BC7_IO_PUTS >> 20));
    pm_lock(&io->m);
    atomic_store_explicit(&io->since, 0, memory_order_release);
    io->put_bytes -= w->size;
    free(w);
    pc_broadcast(&io->done);
}
static void io_thread(void* arg) {
    Bc7Io* io = &g_bc7_cache.io;
    (void)arg;
    SNX_THREAD_NAME("BC7 cache I/O");
    pm_lock(&io->m);
    for (;;) {
        if (io->puts) {
            io_put(io);
            continue;
        }
        if (io->next == io->head || io->ahead >= BC7_IO_AHEAD) {
            if (io->next == io->head && io->late) {
                /* caught up: what was left goes in the log, and only then is the thread idle (io_idle_by), so a wait
                   for it finds the line written. Nothing is queued meanwhile: the GPU thread asks none while late */
                const u64 worst = io->worst;
                io->worst = 0;
                pm_unlock(&io->m);
                io_said(0, 0, worst);
                pm_lock(&io->m);
                io->late = 0;
                continue;
            }
            pc_broadcast(&io->done); /* idle, for a close */
            pc_wait(&io->work, &io->m, -1);
            continue;
        }
        Bc7Op* op = &io->ops[io->next++ % BC7_IO_OPS];
        u32 s = IO_QUEUED;
        if (!atomic_compare_exchange_strong_explicit(&op->state, &s, IO_READING, memory_order_acq_rel,
                                                     memory_order_acquire)) {
            atomic_store_explicit(&op->state, IO_FREE, memory_order_release); /* left before it began */
            continue;
        }
        const u64 t0 = plat_ns(), size = op->size; /* a slot left late is handed out again once it is free */
        atomic_store_explicit(&io->since, t0, memory_order_release);
        pm_unlock(&io->m);
        FILE* f = atomic_load_explicit(&g_bc7_cache.rd.f, memory_order_acquire);
        u8* b = f ? malloc((size_t)size) : NULL;
        const int r = b ? bc7_read_at(f, g_bc7_cache.rd.piece, op->key, op->at, size, b) : 0;
        const u64 took = plat_ns() - t0;
        if (took >= BC7_IO_LATE_NS) io_said(size, took, 0); /* before the thread is idle (io_idle_by) */
        pm_lock(&io->m);
        atomic_store_explicit(&io->since, 0, memory_order_release);
        if (took > io->worst) io->worst = took;
        op->buf = r > 0 ? b : NULL;
        s = IO_READING;
        if (atomic_compare_exchange_strong_explicit(&op->state, &s,
                                                    r > 0 ? IO_OK
                                                    : b   ? IO_BAD
                                                          : IO_FAIL,
                                                    memory_order_acq_rel, memory_order_acquire)) {
            if (r > 0)
                io->ahead += size;
            else
                free(b);
        } else { /* left while it was read */
            free(b);
            op->buf = NULL;
            atomic_store_explicit(&op->state, IO_FREE, memory_order_release);
        }
        if (took >= BC7_IO_LATE_NS) io->worst = 0; /* said */
        pc_broadcast(&io->done);
    }
}
/* the thread, at the first read that goes there: 0 when it could not start */
static int io_start(void) {
    Bc7Io* io = &g_bc7_cache.io;
    if (io->started) return io->started > 0;
    pm_init(&io->m);
    pc_init(&io->work);
    pc_init(&io->done);
    io->puts_end = &io->puts;
    io->started = plat_thread(io_thread, NULL, (size_t)256 << 10) ? -1 : 1;
    if (io->started < 0) {
        char why[256];
        WARN("vulkan: the BC7 cache's I/O thread could not start (%s): its hits are read on the recording thread and "
             "its records written on the GPU thread\n",
             snx_os_error(why, sizeof why));
    }
    return io->started > 0;
}
/* the I/O thread held by a read past BC7_IO_LATE_NS, or behind a read the recording thread left: no read is queued */
static int io_stalled(Bc7Io* io) {
    const u64 since = atomic_load_explicit(&io->since, memory_order_acquire);
    if (since && plat_ns() - since >= BC7_IO_LATE_NS) return 1;
    pm_lock(&io->m);
    const int late = io->late;
    pm_unlock(&io->m);
    return late;
}
/* the I/O thread with nothing queued, no read or write running and no read left late, by until (plat_ns): 0 when it
   still had one then */
static int io_idle_by(u64 until) {
    Bc7Io* io = &g_bc7_cache.io;
    if (io->started <= 0) return 1;
    pm_lock(&io->m);
    for (;;) {
        const int idle =
            io->next == io->head && !io->puts && !io->late && !atomic_load_explicit(&io->since, memory_order_acquire);
        const u64 now = plat_ns();
        if (idle || now >= until) {
            pm_unlock(&io->m);
            return idle;
        }
        pc_wait(&io->done, &io->m, (int64_t)(until - now));
    }
}
/* the recording thread: op's read once it is done, IO_OK, IO_BAD or IO_FAIL; IO_LATE when it left it, once the read
   the I/O thread is on has run BC7_IO_LATE_NS (or BC7_IO_LATE_NS after it came with the thread on none), or at once
   while the thread is behind a read left before. An IO_OK's bytes stop counting ahead */
static u32 io_take(Bc7Op* op) {
    Bc7Io* io = &g_bc7_cache.io;
    const u64 came = plat_ns();
    pm_lock(&io->m);
    for (;;) {
        u32 s = atomic_load_explicit(&op->state, memory_order_acquire);
        if (s == IO_OK || s == IO_BAD || s == IO_FAIL) {
            if (s == IO_OK) {
                io->ahead -= op->size;
                pc_signal(&io->work);
            }
            pm_unlock(&io->m);
            return s;
        }
        const u64 now = plat_ns(), since = atomic_load_explicit(&io->since, memory_order_acquire);
        const u64 from = since ? since : came; /* the read running, or (none) this wait's start */
        if (!io->late && now - from < BC7_IO_LATE_NS) {
            pc_wait(&io->done, &io->m, (int64_t)(BC7_IO_LATE_NS - (now - from)));
            continue;
        }
        if (atomic_compare_exchange_strong_explicit(&op->state, &s, IO_LATE, memory_order_acq_rel,
                                                    memory_order_acquire)) {
            io->late = 1;
            pm_unlock(&io->m);
            atomic_fetch_add_explicit(&io->n_late, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&io->all_late, 1, memory_order_relaxed);
            return IO_LATE;
        }
    }
}

/* a read of the I/O thread taken on the recording thread, and the copy that it is for */
typedef struct {
    u64 key[2], at, size, off, gva, op;
    u8* dst;
    void* owner;
    VkCommandBuffer cb;
    VkBuffer buf;
    VkImage img;
    VkImageLayout layout; /* img's, VK_LAYOUT's of the GPU thread */
    DevMem mem;
    VkBufferImageCopy copy;
} Bc7Later;
/* each a packet of record.c's ring, with its header of 16 bytes: BC7_IO_OPS of them take more than its 4 MiB */
_Static_assert((u64)BC7_IO_OPS * (sizeof(Bc7Later) + 16) >= (4ull << 20), "fewer reads than the recording ring holds");
static void bc7_later_run(const void* p) {
    const Bc7Later* l = p;
    Bc7Op* op = &g_bc7_cache.io.ops[l->op % BC7_IO_OPS];
    const u32 s = io_take(op);
    if (s == IO_OK) {
        memcpy(l->dst, op->buf, (size_t)l->size);
        free(op->buf);
        op->buf = NULL;
        atomic_store_explicit(&op->state, IO_FREE, memory_order_release);
        /* staging may not be host coherent, and its submission's flush (cmd.c's rings_flush) came before this */
        vk_devmem_flush(l->mem, l->off, l->size);
        if (l->img) vkCmdCopyBufferToImage(l->cb, l->buf, l->img, l->layout, 1, &l->copy);
        atomic_fetch_add_explicit(&g_bc7_cache.rd.hits, 1, memory_order_relaxed);
        return;
    }
    if (s != IO_LATE) atomic_store_explicit(&op->state, IO_FREE, memory_order_release); /* a late one is the thread's */
    if (s == IO_BAD) bc7_failed_log();
    pm_lock(&g_bc7_cache.rd.m);
    if (atomic_load_explicit(&g_bc7_cache.rd.nfailed, memory_order_relaxed) == g_bc7_cache.rd.cap) {
        const u32 cap = g_bc7_cache.rd.cap ? g_bc7_cache.rd.cap * 2 : 16;
        Bc7Failed* grown = realloc(g_bc7_cache.rd.failed, (size_t)cap * sizeof *grown);
        if (grown) {
            g_bc7_cache.rd.failed = grown;
            g_bc7_cache.rd.cap = cap;
        }
    }
    /* out of memory: its record stays, and its next read fails again. A read late or not made keeps its record (as
       dropped already): the texture is loaded again, encoded while the thread is behind */
    if (atomic_load_explicit(&g_bc7_cache.rd.nfailed, memory_order_relaxed) < g_bc7_cache.rd.cap) {
        g_bc7_cache.rd.failed[atomic_load_explicit(&g_bc7_cache.rd.nfailed, memory_order_relaxed)] =
            (Bc7Failed){{l->key[0], l->key[1]}, l->at, l->owner, l->gva, s != IO_BAD};
        atomic_store_explicit(&g_bc7_cache.rd.nfailed,
                              atomic_load_explicit(&g_bc7_cache.rd.nfailed, memory_order_relaxed) + 1,
                              memory_order_release);
    }
    pm_unlock(&g_bc7_cache.rd.m);
}

/* a read of key's record (its blocks at at, size bytes of them) queued for the I/O thread, its op's number in *n: 0
   when none is, the thread behind a slow disk or its ops all taken (counted as not tried) */
static int io_queue_read(const u64 key[2], u64 at, u64 size, u64* n) {
    Bc7Io* io = &g_bc7_cache.io;
    Bc7Op* op = &io->ops[io->head % BC7_IO_OPS];
    if (io_stalled(io) || atomic_load_explicit(&op->state, memory_order_acquire) != IO_FREE) {
        atomic_fetch_add_explicit(&io->n_skipped, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&io->all_skipped, 1, memory_order_relaxed);
        return 0;
    }
    pm_lock(&io->m);
    op->key[0] = key[0];
    op->key[1] = key[1];
    op->at = at;
    op->size = size;
    op->buf = NULL;
    atomic_store_explicit(&op->state, IO_QUEUED, memory_order_release);
    *n = io->head++;
    pc_signal(&io->work);
    pm_unlock(&io->m);
    return 1;
}

int vk_bc7_cache_read_later(VkCommandBuffer cb, const u64 key[2], u64 size, u8* dst, DevMem mem, u64 off, VkBuffer buf,
                            VkImage img, VkImageLayout layout, const VkBufferImageCopy* copy, void* owner, u64 gva) {
    if (!bc7_writes_ok()) return 0;
    /* the I/O thread's reads, for the recording thread once it runs: the cache can open before it starts */
    if (!vk_rec_threaded() || !atomic_load_explicit(&g_bc7_cache.rd.f, memory_order_acquire)) return -1;
    const Bc7Entry* e = bc7_entry(key, size);
    u64 n;
    if (!e || !io_queue_read(key, e->at, size, &n)) return 0;
    const Bc7Later l = {{key[0], key[1]}, e->at, size, off, gva, n, dst, owner, cb, buf, img, layout, mem, *copy};
    vk_rec_call(bc7_later_run, &l, sizeof l);
    return 1;
}

void vk_bc7_cache_late(u64* late, u64* skipped, u64* unkept) {
    *late = atomic_load_explicit(&g_bc7_cache.io.all_late, memory_order_relaxed);
    *skipped = atomic_load_explicit(&g_bc7_cache.io.all_skipped, memory_order_relaxed);
    *unkept = atomic_load_explicit(&g_bc7_cache.io.all_unkept, memory_order_relaxed);
}

u64 vk_bc7_cache_read_ns(u64 bytes) {
    const u64 ns = atomic_load_explicit(&g_bc7_cache.rd.ns, memory_order_relaxed);
    const u64 n = atomic_load_explicit(&g_bc7_cache.rd.bytes, memory_order_relaxed);
    if (!n) return bytes ? ~0ull >> 1 : 0; /* no rate yet: a budget takes its one read and stops */
    return (u64)((double)bytes * (double)ns / (double)n);
}

/* a record written on the GPU thread, where the I/O thread does not run */
static void bc7_put_now(const u64 key[2], const u8* p, u64 size) {
    if (g_bc7_cache.end + BC7_REC + size > g_bc7_cache.max) {
        LOG("vulkan: the BC7 cache %s reached %llu MB: it starts over\n", g_bc7_cache.path,
            (unsigned long long)(g_bc7_cache.max >> 20));
        g_bc7_cache.restarts++;
        if (!bc7_start_over()) {
            bc7_off("it could not be emptied");
            return;
        }
    }
    const u64 rec[BC7_REC / 8] = {key[0], key[1], size, bc7_check(p, size)};
    const int fd = fileno(g_bc7_cache.f);
    if (plat_pwrite_fd(fd, rec, BC7_REC, g_bc7_cache.end) != BC7_REC ||
        plat_pwrite_fd(fd, p, size, g_bc7_cache.end + BC7_REC) != (int64_t)size) {
        bc7_off("a record could not be written");
        return;
    }
    if (bc7_index(key, g_bc7_cache.end + BC7_REC, size)) g_bc7_cache.stored++;
    bc7_idx_put(key, g_bc7_cache.end + BC7_REC, size);
    g_bc7_cache.end += BC7_REC + size;
}
/* w to the I/O thread, after the writes before it */
static void io_queue_put(Bc7Put* w) {
    Bc7Io* io = &g_bc7_cache.io;
    w->next = NULL;
    pm_lock(&io->m);
    *io->puts_end = w;
    io->puts_end = &w->next;
    io->put_bytes += w->size;
    pc_signal(&io->work);
    pm_unlock(&io->m);
}
void vk_bc7_cache_put(const u64 key[2], const u8* p, u64 size) {
    if (!bc7_writes_ok() || !size || size % 16 || size > g_bc7_cache.max - BC7_HEAD - BC7_REC || vk_bc7_cache_find(key))
        return;
    Bc7Io* io = &g_bc7_cache.io;
    int queued = 0, room = 0;
    if (io->started > 0) {
        pm_lock(&io->m);
        queued = io->wf == g_bc7_cache.f;
        room = io->put_bytes + size <= BC7_IO_PUTS;
        pm_unlock(&io->m);
    }
    if (!queued) {
        bc7_put_now(key, p, size);
        return;
    }
    if (g_bc7_cache.end + BC7_REC + size > g_bc7_cache.max) { /* the file and its index start over, in their turn */
        LOG("vulkan: the BC7 cache %s reached %llu MB: it starts over\n", g_bc7_cache.path,
            (unsigned long long)(g_bc7_cache.max >> 20));
        Bc7Put* r = calloc(1, sizeof *r);
        if (!r) {
            bc7_off("out of memory to start it over");
            return;
        }
        g_bc7_cache.restarts++;
        memset(g_bc7_cache.map, 0, (size_t)g_bc7_cache.cap * sizeof *g_bc7_cache.map);
        g_bc7_cache.used = 0;
        g_bc7_cache.end = BC7_HEAD;
        io_queue_put(r);
    }
    Bc7Put* w = room ? malloc(sizeof *w + (size_t)size) : NULL;
    if (!w) { /* not kept this run: the level is encoded again in the next */
        atomic_fetch_add_explicit(&io->n_unkept, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&io->all_unkept, 1, memory_order_relaxed);
        return;
    }
    w->at = g_bc7_cache.end;
    w->size = size;
    w->rec[0] = key[0];
    w->rec[1] = key[1];
    w->rec[2] = size;
    w->rec[3] = 0;
    memcpy(w->blocks, p, (size_t)size);
    if (bc7_index(key, g_bc7_cache.end + BC7_REC, size)) g_bc7_cache.stored++;
    g_bc7_cache.end += BC7_REC + size;
    io_queue_put(w);
}

u64 vk_bc7_cache_max(void) { return g_bc7_cache.max; }
void vk_bc7_cache_stats(u64* hits, u64* stored, u64* restarts, u64* bytes) {
    *hits = g_bc7_cache.hits + atomic_load_explicit(&g_bc7_cache.rd.hits, memory_order_relaxed);
    *stored = g_bc7_cache.stored;
    *restarts = g_bc7_cache.restarts;
    *bytes = g_bc7_cache.f ? g_bc7_cache.end : 0;
}
