/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A save state's file. The header and the thumbnail come first, uncompressed, so the launcher lists states without
   decompressing anything. The sections follow, each zstd frames of CHUNK bytes at most with their content checksums
   (which the reader checks as it reaches each frame's end), compressed side by side on a few host threads and written
   in order, and the table of the sections last, which the header points at once it is written.
   Every number is little-endian.

     0    the header and the thumbnail (common/utilities/state_header.c)
     ...  the sections
     table_off  nsections entries: char tag[8], u64 schema, u64 offset, u64 stored size, u64 size */
#include "nx/runtime/state_file.h"
#include "common/utilities/endian.h"
#include "nx/runtime/platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#define HEADER_SIZE SNX_STATE_HEADER
#define ENTRY_SIZE  40
#define SECTIONS    SNX_STATE_SECTIONS
#define LEVEL       1  /* zstd's: a save is a pause the player waits through, and guest memory is mostly zeros */
#define CHUNK       (4u << 20) /* a frame's bytes: what a worker compresses at a time */
#define WORKERS     8          /* the most worker threads a writer has */
#define RING        (2 * WORKERS)

/* a chunk of a section: filled by sw_put, compressed by a worker, written by the writer in the order filled */
enum { CH_FREE, CH_QUEUED, CH_BUSY, CH_DONE, CH_FAILED };
typedef struct {
    u8 *in, *out;
    size_t in_n, out_n;
    int state;
} Chunk;

typedef struct {
    char tag[8];
    u64 schema, off, stored, size;
} Section;

struct StateWriter {
    FILE* f;
    char path[1100], part[1110];
    SnxStateHeader h;
    Section sec[SECTIONS];
    int n, open, failed;
    u64 at; /* the file's length so far */
    /* the chunks, the next one sw_put fills and the next one written, their counts since sw_open; and the workers,
       under lk: they wait on cv for a chunk queued, and the writer for one done or a worker gone */
    Chunk ring[RING];
    u64 filled, written;
    int nworkers, live, quit;
    PMutex lk;
    PCond cv;
};
struct StateReader {
    FILE* f;
    Section sec[SECTIONS];
    int n, cur;
    int ended;                    /* the section's frame has ended (zstd has checked its checksum) */
    u64 in_at, in_left, out_left; /* the current section's next stored byte, its stored bytes not read, and its bytes
                                     not given out */
    ZSTD_DCtx* z;
    u8* in;
    size_t insize;
    ZSTD_inBuffer ib;
};

static void why_set(char* why, size_t n, const char* text) {
    if (why && n) snprintf(why, n, "%s", text);
}

/* ============================================================ writing */
static void put64(u8* p, u64 v) { snx_st_le64(p, v); }
static int out(StateWriter* w, const void* p, size_t n);
/* a worker: each queued chunk compressed into its own frame, with its own context */
static void worker(void* arg) {
    StateWriter* w = (StateWriter*)arg;
    ZSTD_CCtx* z = ZSTD_createCCtx();
    if (z) {
        ZSTD_CCtx_setParameter(z, ZSTD_c_compressionLevel, LEVEL);
        ZSTD_CCtx_setParameter(z, ZSTD_c_checksumFlag, 1);
    }
    pm_lock(&w->lk);
    for (;;) {
        Chunk* c = NULL;
        u32 i;
        for (i = 0; i < RING && !c; i++)
            if (w->ring[i].state == CH_QUEUED) c = &w->ring[i];
        if (!c) {
            if (w->quit) break;
            pc_wait(&w->cv, &w->lk, -1);
            continue;
        }
        c->state = CH_BUSY;
        pm_unlock(&w->lk);
        c->out_n = z ? ZSTD_compress2(z, c->out, ZSTD_compressBound(CHUNK), c->in, c->in_n) : 0;
        pm_lock(&w->lk);
        c->state = !z || ZSTD_isError(c->out_n) ? CH_FAILED : CH_DONE;
        pc_broadcast(&w->cv);
    }
    w->live--;
    pc_broadcast(&w->cv);
    pm_unlock(&w->lk);
    ZSTD_freeCCtx(z);
}
/* the oldest chunk filled, once compressed, written to the file and free again: 0, or -1 */
static int write_oldest(StateWriter* w) {
    Chunk* c = &w->ring[w->written % RING];
    int st;
    pm_lock(&w->lk);
    while (c->state == CH_QUEUED || c->state == CH_BUSY) pc_wait(&w->cv, &w->lk, -1);
    st = c->state;
    pm_unlock(&w->lk);
    if (st != CH_DONE || out(w, c->out, c->out_n)) w->failed = 1;
    c->state = CH_FREE;
    c->in_n = 0;
    w->written++;
    return w->failed ? -1 : 0;
}
/* the chunk being filled queued for a worker, the oldest written first when the ring is full */
static int queue_chunk(StateWriter* w) {
    Chunk* c = &w->ring[w->filled % RING];
    pm_lock(&w->lk);
    c->state = CH_QUEUED;
    pc_signal(&w->cv);
    pm_unlock(&w->lk);
    w->filled++;
    return w->filled - w->written == RING ? write_oldest(w) : 0;
}
/* the workers told to end, and waited for */
static void workers_end(StateWriter* w) {
    pm_lock(&w->lk);
    w->quit = 1;
    pc_broadcast(&w->cv);
    while (w->live) pc_wait(&w->cv, &w->lk, -1);
    pm_unlock(&w->lk);
}
static void writer_free(StateWriter* w) {
    u32 i;
    workers_end(w);
    for (i = 0; i < RING; i++) {
        free(w->ring[i].in);
        free(w->ring[i].out);
    }
    pm_destroy(&w->lk);
    free(w);
}
static int out(StateWriter* w, const void* p, size_t n) {
    if (w->failed || (n && fwrite(p, 1, n, w->f) != n)) {
        w->failed = 1;
        return -1;
    }
    w->at += n;
    return 0;
}
StateWriter* sw_open(const char* path, const SnxStateHeader* h, const u8* thumb, char* why, size_t whysize) {
    StateWriter* w = calloc(1, sizeof *w);
    u8 b[HEADER_SIZE];
    if (!w) {
        why_set(why, whysize, "out of memory");
        return NULL;
    }
    snprintf(w->path, sizeof w->path, "%s", path);
    snprintf(w->part, sizeof w->part, "%s.part", path);
    w->h = *h;
    w->h.format = SNX_STATE_FORMAT;
    if (!thumb) w->h.thumb_w = w->h.thumb_h = 0;
    pm_init(&w->lk);
    pc_init(&w->cv);
    {
        int ids[64], cpus = plat_cpus(ids, 64), i, ok = 1;
        w->nworkers = cpus > 1 ? (cpus - 1 < WORKERS ? cpus - 1 : WORKERS) : 1;
        for (i = 0; i < RING; i++) {
            w->ring[i].in = (u8*)malloc(CHUNK);
            w->ring[i].out = (u8*)malloc(ZSTD_compressBound(CHUNK));
            ok = ok && w->ring[i].in && w->ring[i].out;
        }
        for (i = 0; ok && i < w->nworkers; i++) {
            pm_lock(&w->lk);
            w->live++;
            pm_unlock(&w->lk);
            if (plat_thread(worker, w, 1 << 20)) {
                pm_lock(&w->lk);
                w->live--;
                pm_unlock(&w->lk);
                ok = i > 0; /* fewer workers still compress */
                break;
            }
        }
        w->f = ok ? fopen(w->part, "wb") : NULL;
        if (!w->f) {
            why_set(why, whysize, ok ? "the file can't be made" : "out of memory");
            sw_abort(w);
            return NULL;
        }
    }
    snx_state_header_write(b, &w->h, 0, 0); /* again with the table at the end */
    out(w, b, sizeof b);
    if (thumb) out(w, thumb, (size_t)w->h.thumb_w * w->h.thumb_h * 4);
    if (w->failed) {
        why_set(why, whysize, "the file can't be written");
        sw_abort(w);
        return NULL;
    }
    return w;
}
int sw_begin(StateWriter* w, const char* tag, u64 schema) {
    Section* s;
    if (w->failed || w->open || w->n == SECTIONS) return w->failed = 1, -1;
    s = &w->sec[w->n++];
    memset(s, 0, sizeof *s);
    strncpy(s->tag, tag, sizeof s->tag);
    s->schema = schema;
    s->off = w->at;
    w->open = 1;
    return 0;
}
int sw_put(StateWriter* w, const void* p, u64 n) {
    const u8* b = p;
    if (w->failed || !w->open) return w->failed = 1, -1;
    w->sec[w->n - 1].size += n;
    while (n) {
        Chunk* c = &w->ring[w->filled % RING];
        size_t k = CHUNK - c->in_n < n ? CHUNK - c->in_n : (size_t)n;
        memcpy(c->in + c->in_n, b, k);
        c->in_n += k;
        b += k;
        n -= k;
        if (c->in_n == CHUNK && queue_chunk(w)) return -1;
    }
    return 0;
}
int sw_end(StateWriter* w) {
    Section* s = &w->sec[w->n - 1];
    if (w->failed || !w->open) return w->failed = 1, -1;
    /* the chunk left, and an empty section's one frame */
    if ((w->ring[w->filled % RING].in_n || !s->size) && queue_chunk(w)) return -1;
    while (w->written < w->filled)
        if (write_oldest(w)) return -1;
    s->stored = w->at - s->off;
    w->open = 0;
    return 0;
}
void sw_abort(StateWriter* w) {
    if (!w) return;
    if (w->f) {
        fclose(w->f);
        remove(w->part);
    }
    writer_free(w);
}
int sw_finish(StateWriter* w, char* why, size_t whysize) {
    u8 b[HEADER_SIZE], e[ENTRY_SIZE];
    u64 table = w->at;
    int i, r;
    if (w->open) sw_end(w);
    for (i = 0; i < w->n && !w->failed; i++) {
        memset(e, 0, sizeof e);
        memcpy(e, w->sec[i].tag, 8);
        put64(e + 8, w->sec[i].schema);
        put64(e + 16, w->sec[i].off);
        put64(e + 24, w->sec[i].stored);
        put64(e + 32, w->sec[i].size);
        out(w, e, sizeof e);
    }
    snx_state_header_write(b, &w->h, table, (u32)w->n);
    if (!w->failed && (fseek(w->f, 0, SEEK_SET) || fwrite(b, 1, sizeof b, w->f) != sizeof b)) w->failed = 1;
    if (!w->failed && fflush(w->f)) w->failed = 1;
    if (w->failed) {
        why_set(why, whysize, "the file can't be written (is the disk full?)");
        sw_abort(w);
        return -1;
    }
    fclose(w->f);
    w->f = NULL;
    r = plat_replace_file(w->part, w->path);
    if (r) why_set(why, whysize, "the file can't be moved into place");
    writer_free(w);
    return r;
}

/* ============================================================ reading */
/* the header, and the table into sec: the open file, or NULL */
static FILE* open_state(const char* path, SnxStateHeader* h, Section* sec, int* n, char* why, size_t whysize) {
    FILE* f = fopen(path, "rb");
    u8 b[HEADER_SIZE], e[ENTRY_SIZE];
    u64 table;
    u32 count, i;
    if (!f) {
        why_set(why, whysize, "the file can't be opened");
        return NULL;
    }
    if (fread(b, 1, sizeof b, f) != sizeof b || snx_state_header_read(b, h, &table, &count, why, whysize)) {
        if (!why || !why[0]) why_set(why, whysize, "it is not a save state");
        fclose(f);
        return NULL;
    }
    for (i = 0; sec && i < count; i++) {
        if (plat_pread(f, e, sizeof e, table + (u64)i * sizeof e) != (i64)sizeof e) {
            why_set(why, whysize, "it was not written to its end");
            fclose(f);
            return NULL;
        }
        memcpy(sec[i].tag, e, 8);
        sec[i].schema = snx_ld_le64(e + 8);
        sec[i].off = snx_ld_le64(e + 16);
        sec[i].stored = snx_ld_le64(e + 24);
        sec[i].size = snx_ld_le64(e + 32);
    }
    if (n) *n = (int)count;
    return f;
}
StateReader* sr_open(const char* path, SnxStateHeader* h, char* why, size_t whysize) {
    StateReader* r = calloc(1, sizeof *r);
    if (!r) {
        why_set(why, whysize, "out of memory");
        return NULL;
    }
    r->cur = -1;
    r->f = open_state(path, h, r->sec, &r->n, why, whysize);
    r->insize = ZSTD_DStreamInSize();
    r->in = malloc(r->insize);
    r->z = ZSTD_createDCtx();
    if (!r->f || !r->in || !r->z) {
        if (r->f) why_set(why, whysize, "out of memory");
        sr_close(r);
        return NULL;
    }
    return r;
}
int sr_begin(StateReader* r, const char* tag, u64* schema) {
    int i;
    char t[8] = {0};
    strncpy(t, tag, sizeof t);
    for (i = 0; i < r->n && memcmp(r->sec[i].tag, t, 8); i++) {}
    if (i == r->n) return -1;
    r->cur = i;
    r->in_at = r->sec[i].off;
    r->in_left = r->sec[i].stored;
    r->ended = 0;
    r->out_left = r->sec[i].size;
    r->ib.src = r->in;
    r->ib.size = r->ib.pos = 0;
    ZSTD_DCtx_reset(r->z, ZSTD_reset_session_only);
    if (schema) *schema = r->sec[i].schema;
    return 0;
}
/* the next stored bytes, once the last ones are used: 0, or -1 at the section's end or on a read error */
static int refill(StateReader* r) {
    size_t want;
    if (r->ib.pos < r->ib.size) return 0;
    if (!r->in_left) return -1;
    want = r->in_left < r->insize ? (size_t)r->in_left : r->insize;
    if (plat_pread(r->f, r->in, want, r->in_at) != (i64)want) return -1;
    r->in_at += want;
    r->in_left -= want;
    r->ib.size = want;
    r->ib.pos = 0;
    return 0;
}
/* Decompresses into o until it is full, or with end set until the frame ends, where zstd checks the content checksum:
   o is then a byte the frame must not fill, as zstd ends no frame into an empty buffer */
static int inflate(StateReader* r, ZSTD_outBuffer* o, int end) {
    for (;;) {
        size_t ret, at = o->pos;
        u64 left = r->in_left + (r->ib.size - r->ib.pos);
        refill(r); /* none left is fine: zstd may still hold output */
        if (end && r->ended) return 0;
        ret = ZSTD_decompressStream(r->z, o, &r->ib);
        if (ZSTD_isError(ret)) return -1; /* a damaged frame, or a checksum that doesn't match */
        r->ended = ret == 0;
        if (end ? ret == 0 : o->pos == o->size) return end && o->pos ? -1 : 0;
        if (o->pos == at && r->in_left + (r->ib.size - r->ib.pos) == left && !r->in_left && r->ib.pos == r->ib.size)
            return -1; /* nothing moved and nothing left: the section is cut short */
    }
}
int sr_get(StateReader* r, void* p, u64 n) {
    u8* b = p;
    if (r->cur < 0 || n > r->out_left) return -1;
    while (n) {
        size_t k = n > ((u64)1 << 30) ? (size_t)1 << 30 : (size_t)n;
        ZSTD_outBuffer o = {b, k, 0};
        if (inflate(r, &o, 0)) return -1;
        b += k;
        n -= k;
        r->out_left -= k;
    }
    if (!r->out_left) {
        u8 extra;
        ZSTD_outBuffer o = {&extra, 1, 0};
        if (inflate(r, &o, 1)) return -1;
    }
    return 0;
}
u64 sr_left(const StateReader* r) { return r->cur < 0 ? 0 : r->out_left; }
void sr_close(StateReader* r) {
    if (!r) return;
    if (r->f) fclose(r->f);
    ZSTD_freeDCtx(r->z);
    free(r->in);
    free(r);
}
