/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A save state's file. The header and the thumbnail come first, uncompressed, so the launcher lists states without
   decompressing anything. The sections follow, each one zstd frame with its content checksum (which the reader checks
   as it reaches the frame's end), and the table of the sections last, which the header points at once it is written.
   Every number is little-endian.

     0    "SNXSTATE"
     8    u32 format, u32 abi, u64 title_id, u64 created, u64 guest_ns, u32 thumb_w, u32 thumb_h,
          u64 table_off, u32 nsections, u32 0, char runtime[96], char stamp[64], char game[128]
     512  the thumbnail: thumb_w * thumb_h RGBA8 pixels
     ...  the sections
     table_off  nsections entries: char tag[8], u64 schema, u64 offset, u64 stored size, u64 size */
#include "nx/runtime/state_file.h"
#include "common/utilities/endian.h"
#include "nx/runtime/platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#define HEADER_SIZE 512
#define ENTRY_SIZE  40
#define SECTIONS    64 /* a state holds this many sections at most */
#define LEVEL       1  /* zstd's: a save is a pause the player waits through, and guest memory is mostly zeros */

typedef struct {
    char tag[8];
    u64 schema, off, stored, size;
} Section;

struct StateWriter {
    FILE* f;
    char path[1100], part[1110];
    StateHeader h;
    Section sec[SECTIONS];
    int n, open, failed;
    u64 at; /* the file's length so far */
    ZSTD_CCtx* z;
    u8* out;
    size_t outsize;
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

/* ============================================================ the header */
static void put32(u8* p, u32 v) { snx_st_le32(p, v); }
static void put64(u8* p, u64 v) { snx_st_le64(p, v); }
static void header_write(u8 b[HEADER_SIZE], const StateHeader* h, u64 table_off, u32 nsections) {
    memset(b, 0, HEADER_SIZE);
    memcpy(b, "SNXSTATE", 8);
    put32(b + 8, h->format);
    put32(b + 12, h->abi);
    put64(b + 16, h->title_id);
    put64(b + 24, h->created);
    put64(b + 32, h->guest_ns);
    put32(b + 40, h->thumb_w);
    put32(b + 44, h->thumb_h);
    put64(b + 48, table_off);
    put32(b + 56, nsections);
    memcpy(b + 64, h->runtime, sizeof h->runtime);
    memcpy(b + 160, h->stamp, sizeof h->stamp);
    memcpy(b + 224, h->game, sizeof h->game);
}
/* 0, or -1 when b isn't a state's header this runtime reads */
static int header_read(const u8 b[HEADER_SIZE], StateHeader* h, u64* table_off, u32* nsections, char* why, size_t n) {
    if (memcmp(b, "SNXSTATE", 8)) {
        why_set(why, n, "it is not a save state");
        return -1;
    }
    memset(h, 0, sizeof *h);
    h->format = snx_ld_le32(b + 8);
    h->abi = snx_ld_le32(b + 12);
    h->title_id = snx_ld_le64(b + 16);
    h->created = snx_ld_le64(b + 24);
    h->guest_ns = snx_ld_le64(b + 32);
    h->thumb_w = snx_ld_le32(b + 40);
    h->thumb_h = snx_ld_le32(b + 44);
    *table_off = snx_ld_le64(b + 48);
    *nsections = snx_ld_le32(b + 56);
    memcpy(h->runtime, b + 64, sizeof h->runtime - 1);
    memcpy(h->stamp, b + 160, sizeof h->stamp - 1);
    memcpy(h->game, b + 224, sizeof h->game - 1);
    if (h->format != STATE_FORMAT) {
        why_set(why, n, "it was saved in another layout of the state file");
        return -1;
    }
    if (*nsections > SECTIONS || h->thumb_w > 4096 || h->thumb_h > 4096) {
        why_set(why, n, "its header is damaged");
        return -1;
    }
    return 0;
}

/* ============================================================ writing */
static int out(StateWriter* w, const void* p, size_t n) {
    if (w->failed || (n && fwrite(p, 1, n, w->f) != n)) {
        w->failed = 1;
        return -1;
    }
    w->at += n;
    return 0;
}
StateWriter* sw_open(const char* path, const StateHeader* h, const u8* thumb, char* why, size_t whysize) {
    StateWriter* w = calloc(1, sizeof *w);
    u8 b[HEADER_SIZE];
    if (!w) {
        why_set(why, whysize, "out of memory");
        return NULL;
    }
    snprintf(w->path, sizeof w->path, "%s", path);
    snprintf(w->part, sizeof w->part, "%s.part", path);
    w->h = *h;
    w->h.format = STATE_FORMAT;
    if (!thumb) w->h.thumb_w = w->h.thumb_h = 0;
    w->outsize = ZSTD_CStreamOutSize();
    w->out = malloc(w->outsize);
    w->z = ZSTD_createCCtx();
    w->f = fopen(w->part, "wb");
    if (!w->out || !w->z || !w->f) {
        why_set(why, whysize, w->f ? "out of memory" : "the file can't be made");
        sw_abort(w);
        return NULL;
    }
    header_write(b, &w->h, 0, 0); /* again with the table at the end */
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
    ZSTD_CCtx_reset(w->z, ZSTD_reset_session_and_parameters);
    ZSTD_CCtx_setParameter(w->z, ZSTD_c_compressionLevel, LEVEL);
    ZSTD_CCtx_setParameter(w->z, ZSTD_c_checksumFlag, 1);
    return 0;
}
/* feeds p (n bytes) to the section's frame, mode ZSTD_e_end closing it */
static int squeeze(StateWriter* w, const void* p, size_t n, ZSTD_EndDirective mode) {
    ZSTD_inBuffer in = {p, n, 0};
    for (;;) {
        ZSTD_outBuffer o = {w->out, w->outsize, 0};
        size_t left = ZSTD_compressStream2(w->z, &o, &in, mode);
        if (ZSTD_isError(left) || out(w, w->out, o.pos)) return w->failed = 1, -1;
        if (mode == ZSTD_e_end ? left == 0 : in.pos == in.size) return 0;
    }
}
int sw_put(StateWriter* w, const void* p, u64 n) {
    const u8* b = p;
    if (w->failed || !w->open) return w->failed = 1, -1;
    w->sec[w->n - 1].size += n;
    while (n) { /* in pieces a size_t holds on every host */
        size_t k = n > ((u64)1 << 30) ? (size_t)1 << 30 : (size_t)n;
        if (squeeze(w, b, k, ZSTD_e_continue)) return -1;
        b += k;
        n -= k;
    }
    return 0;
}
int sw_end(StateWriter* w) {
    if (w->failed || !w->open || squeeze(w, NULL, 0, ZSTD_e_end)) return w->failed = 1, -1;
    w->sec[w->n - 1].stored = w->at - w->sec[w->n - 1].off;
    w->open = 0;
    return 0;
}
void sw_abort(StateWriter* w) {
    if (!w) return;
    if (w->f) {
        fclose(w->f);
        remove(w->part);
    }
    ZSTD_freeCCtx(w->z);
    free(w->out);
    free(w);
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
    header_write(b, &w->h, table, (u32)w->n);
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
    ZSTD_freeCCtx(w->z);
    free(w->out);
    free(w);
    return r;
}

/* ============================================================ reading */
/* the header, and the table into sec: the open file, or NULL */
static FILE* open_state(const char* path, StateHeader* h, Section* sec, int* n, char* why, size_t whysize) {
    FILE* f = fopen(path, "rb");
    u8 b[HEADER_SIZE], e[ENTRY_SIZE];
    u64 table;
    u32 count, i;
    if (!f) {
        why_set(why, whysize, "the file can't be opened");
        return NULL;
    }
    if (fread(b, 1, sizeof b, f) != sizeof b || header_read(b, h, &table, &count, why, whysize)) {
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
int sf_peek(const char* path, StateHeader* h, u8** thumb, char* why, size_t whysize) {
    FILE* f = open_state(path, h, NULL, NULL, why, whysize);
    if (!f) return -1;
    if (thumb) {
        size_t size = (size_t)h->thumb_w * h->thumb_h * 4;
        *thumb = NULL;
        if (size) {
            *thumb = malloc(size);
            if (!*thumb || plat_pread(f, *thumb, size, HEADER_SIZE) != (i64)size) {
                free(*thumb);
                *thumb = NULL;
            }
        }
    }
    fclose(f);
    return 0;
}
StateReader* sr_open(const char* path, StateHeader* h, char* why, size_t whysize) {
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
