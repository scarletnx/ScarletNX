/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The file of translated shaders and pipelines made (vk.h's pipeline_file.c). Records are appended as they are made
   and flushed, so a run that crashes keeps what it made, packed into units as they add up (pf_pack), and read once at
   startup. It calls no Vulkan function: pipeline.c makes the modules and pipelines from what it hands back */
#include <stdlib.h>
#include <string.h>
#include <zstd.h>
#include "common/utilities/common.h"
#include "nx/runtime/platform/log.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/pipeline_file.h"
#if defined(_WIN32)
#include <io.h> /* _chsize_s */
#else
#include <unistd.h> /* getpid, ftruncate */
#endif
#if defined(__linux__) && !defined(__ANDROID__)
#include <sys/mman.h> /* posix_madvise */
#endif

#define PF_MAGIC   "SNXVKSH\n"
#define PF_VERSION 3 /* 2: PF_SOURCE records, and a key whose guest part comes first. 3: PF_UNIT */

/* the bytes of records a unit closes at: about 70 shaders with their sources and pipelines in the files measured, which
   pack to 23% as units of 100 shaders do, in a time and memory that don't grow with the shaders' sizes */
#define PF_UNIT_BYTES (2u << 20)
/* zstd's level: its default, which packed the files measured to 23%, and level 1 to 25% in 70% of the time */
#define PF_UNIT_LEVEL 3
/* a unit record's payload, before its frame: the records' bytes unpacked, the frame's bytes, and how many records it
   holds and of them shader records (PF_SHADER, PF_SHIPREF), which reading counts again */
typedef struct {
    u32 raw, packed, records, shaders;
} PfUnit;

/* the check of a payload: words of it after its type and size (FNV-1a over 32-bit words, with a last mix) */
static u64 pf_mix(u64 h, const void* p, size_t n) {
    const u8* b = (const u8*)p;
    size_t i;
    for (i = 0; i + 4 <= n; i += 4) {
        u32 w;
        memcpy(&w, b + i, 4);
        h = (h ^ w) * 0x100000001B3ull;
    }
    return h;
}
static u64 pf_start(u32 type, u32 size) {
    u64 h = 0xCBF29CE484222325ull;
    h = (h ^ type) * 0x100000001B3ull;
    return (h ^ size) * 0x100000001B3ull;
}
static u64 pf_end(u64 h) {
    h ^= h >> 31;
    h *= 0x9E3779B97F4A7C15ull;
    return h ^ (h >> 29);
}
static u32 pf_get32(const u8* p) {
    u32 v;
    memcpy(&v, p, 4);
    return v;
}
static u64 pf_get64(const u8* p) {
    u64 v;
    memcpy(&v, p, 8);
    return v;
}

#if defined(__linux__)
/* Independent records can be checked together. Ranges end at record boundaries and callbacks still run in file
   order, stopping at the earliest bad record. No Vulkan object or translator state is touched by these workers. */
typedef struct {
    const u8* bytes;
    size_t at[65], bad[64];
    u32 count, next;
} PfChecks;
static void* pf_check_worker(void* arg) {
    PfChecks* c = arg;
    for (;;) {
        u32 job = __atomic_fetch_add(&c->next, 1, __ATOMIC_RELAXED);
        if (job >= c->count) break;
        size_t at = c->at[job], end = c->at[job + 1];
        while (at < end) {
            u32 type = pf_get32(c->bytes + at), len = pf_get32(c->bytes + at + 4);
            u64 h = pf_end(pf_mix(pf_start(type, len), c->bytes + at + PF_REC, len));
            if (h != pf_get64(c->bytes + at + 8)) break;
            at += PF_REC + len;
        }
        c->bad[job] = at;
    }
    return NULL;
}
/* the threads that check or unpack a file at once: at most two during background preparation, leaving the guest and
   pipeline workers CPU time */
static int pf_workers(void) {
    int ids[8], n = plat_cpus(ids, 8);
    if (n > 8) n = 8;
    if (g_settings.prepare_shaders != 2 && n > 2) n = 2;
    return n;
}
/* A large warm file only: the small files and other platforms keep the serial checks. The caller includes the
   original file size when reporting a damaged tail, even though only the checked prefix goes to pf_walk. */
static size_t pf_check_parallel(const u8* bytes, size_t start, size_t size, int* checked) {
    int n;
    *checked = 0;
    if (size - start < (8u << 20) || (n = pf_workers()) < 2) return size;
    PfChecks c = {.bytes = bytes, .at = {start}};
    size_t target = (size - start) / ((size_t)n * 8), at = start;
    while (size - at >= PF_REC) {
        u32 len = pf_get32(bytes + at + 4);
        if (len % 8 || len > size - at - PF_REC) break;
        at += PF_REC + len;
        if (c.count < 63 && at - c.at[c.count] >= target) c.at[++c.count] = at;
    }
    if (at != c.at[c.count]) c.at[++c.count] = at;
    if (!c.count) return start;
    pthread_t threads[7];
    unsigned made = 0;
    for (int i = 1; i < n; i++)
        if (!pthread_create(&threads[made], NULL, pf_check_worker, &c)) made++;
    pf_check_worker(&c);
    for (unsigned i = 0; i < made; i++) pthread_join(threads[i], NULL);
    for (u32 i = 0; i < c.count; i++)
        if (c.bad[i] < c.at[i + 1] && c.bad[i] < at) at = c.bad[i];
    *checked = 1;
    return at;
}
#endif

/* the module's slot in the map: its own, or the free one where it would go */
static PfMod* pf_slot(PfMod* map, u32 cap, u64 module) {
    u32 i = (u32)((module * 0x9E3779B97F4A7C15ull) >> 32) & (cap - 1);
    while (map[i].module && map[i].module != module) i = (i + 1) & (cap - 1);
    return &map[i];
}
static void pf_map_add(PFile* pf, u64 module, u32 rec, u64 check) {
    PfMod* m;
    if (!module) return;
    if ((pf->nmap + 1) * 2 > pf->cap) { /* at most half full */
        u32 cap = pf->cap ? pf->cap * 2 : 1024, i;
        PfMod* grown = (PfMod*)calloc(cap, sizeof *grown);
        if (!grown) return; /* pipelines with this module are not recorded */
        for (i = 0; i < pf->cap; i++)
            if (pf->map[i].module) *pf_slot(grown, cap, pf->map[i].module) = pf->map[i];
        free(pf->map);
        pf->map = grown;
        pf->cap = cap;
    }
    m = pf_slot(pf->map, pf->cap, module);
    if (!m->module) pf->nmap++;
    m->module = module;
    m->check = check;
    m->rec = rec;
}

/* the header for key, malloc'd, its size a multiple of 8 */
static u8* pf_header(const char* key, size_t* size) {
    size_t n = strlen(key), total = (16 + n + 7) & ~(size_t)7;
    u8* h = (u8*)calloc(1, total);
    u32 v = PF_VERSION, len = (u32)n;
    if (!h) return NULL;
    memcpy(h, PF_MAGIC, 8);
    memcpy(h + 8, &v, 4);
    memcpy(h + 12, &len, 4);
    memcpy(h + 16, key, n);
    *size = total;
    return h;
}

/* the file cut to size bytes, through f */
static int pf_cut(FILE* f, u64 size) {
#if defined(_WIN32)
    return _chsize_s(_fileno(f), (long long)size) ? -1 : 0;
#else
    return ftruncate(fileno(f), (off_t)size) ? -1 : 0;
#endif
}

/* The open unit's records (pf->open to the end, whole records), once they reach PF_UNIT_BYTES: read back, checked,
   packed and written over themselves as one PF_UNIT record, the file cut after it, and the next unit opened there.
   Records that can't be read back or don't come out smaller are left unpacked, and the next unit starts after them; a
   write that fails ends the appending, as pf_append's does: the unit's records are lost then, and nothing after them
   could be read. Called with pf->lock */
static void pf_pack(PFile* pf) {
    size_t n = (size_t)(pf->size - pf->open), bound = ZSTD_compressBound(n), packed = 0, total = 0, at = 0;
    u8* raw = (u8*)malloc(n + PF_REC + sizeof(PfUnit) + bound + 8); /* the records, then the unit record */
    u8* rec = raw ? raw + n : NULL;                                 /* n is a multiple of 8 */
    FILE* rw = raw ? fopen(pf->path, "r+b") : NULL;
    PfUnit u = {(u32)n, 0, 0, 0};
    char why[256] = "out of memory";
    int ok = rw && !fseek(rw, (long)pf->open, SEEK_SET) && fread(raw, 1, n, rw) == n;
    if (raw && !ok) snx_os_error(why, sizeof why);
    for (; ok && n - at >= PF_REC; u.records++) { /* as appended: a file another process wrote to isn't */
        u32 type = pf_get32(raw + at), len = pf_get32(raw + at + 4);
        if (len % 8 || len > n - at - PF_REC || type == PF_UNIT ||
            pf_end(pf_mix(pf_start(type, len), raw + at + PF_REC, len)) != pf_get64(raw + at + 8)) {
            snprintf(why, sizeof why, "the record at %llu is not as written", (unsigned long long)(pf->open + at));
            ok = 0;
        } else {
            u.shaders += type == PF_SHADER || type == PF_SHIPREF;
            at += PF_REC + len;
        }
    }
    if (ok) {
        packed = ZSTD_compress(rec + PF_REC + sizeof u, bound, raw, n, PF_UNIT_LEVEL);
        if (ZSTD_isError(packed)) {
            snprintf(why, sizeof why, "zstd: %s", ZSTD_getErrorName(packed));
            ok = 0;
        }
        total = PF_REC + sizeof u + ((packed + 7) & ~(size_t)7);
    }
    if (!ok || total >= n) {
        if (!ok)
            LOG("vulkan: cannot pack the %zu bytes of records at %llu in %s (%s): they stay unpacked\n", n,
                (unsigned long long)pf->open, pf->path, why);
        if (rw) fclose(rw);
        free(raw);
        pf->open = pf->size;
        return;
    }
    {
        u32 type = PF_UNIT, len = (u32)(total - PF_REC);
        u64 h;
        u.packed = (u32)packed;
        memcpy(rec + PF_REC, &u, sizeof u);
        memset(rec + PF_REC + sizeof u + packed, 0, total - PF_REC - sizeof u - packed);
        h = pf_end(pf_mix(pf_start(type, len), rec + PF_REC, len));
        memcpy(rec, &type, 4);
        memcpy(rec + 4, &len, 4);
        memcpy(rec + 8, &h, 8);
    }
    ok = !fseek(rw, (long)pf->open, SEEK_SET) && fwrite(rec, 1, total, rw) == total && !fflush(rw) &&
         !pf_cut(rw, pf->open + total);
    if (!ok) snx_os_error(why, sizeof why);
    if (fclose(rw) && ok) {
        snx_os_error(why, sizeof why);
        ok = 0;
    }
    /* pf->f goes on at the new end: a file started over was opened for writing, not appending */
    if (ok && fseek(pf->f, 0, SEEK_END)) {
        snx_os_error(why, sizeof why);
        ok = 0;
    }
    free(raw);
    if (!ok) {
        WARN("vulkan: cannot write %s (%s): what is made from now on is not kept\n", pf->path, why);
        fclose(pf->f);
        pf->f = NULL;
        return;
    }
    pf->size = pf->open + total;
    pf->open = pf->size;
}

/* appends a record of pieces (each a multiple of 4 bytes), padded to 8: 0, or -1 when it can't be (the file is full or
   a write failed; nothing more is appended then). Called with pf->lock */
static int pf_append(PFile* pf, u32 type, const void* const* piece, const size_t* len, int pieces, u64* check) {
    static const u8 zero[8] = {0};
    size_t payload = 0, pad;
    u8 head[PF_REC];
    u64 h, max = pf->max ? pf->max : PF_MAX;
    u32 size;
    int i, ok;
    if (!pf->f) return -1;
    for (i = 0; i < pieces; i++) payload += len[i];
    pad = (8 - payload % 8) % 8;
    if (payload + pad > 0xFFFFFFF0u || pf->size + PF_REC + payload + pad > max) {
        LOG("vulkan: %s reached %llu MiB: what is made from now on is not kept for the next run\n", pf->path,
            (unsigned long long)(max >> 20));
        fclose(pf->f);
        pf->f = NULL;
        return -1;
    }
    size = (u32)(payload + pad);
    h = pf_start(type, size);
    for (i = 0; i < pieces; i++) h = pf_mix(h, piece[i], len[i]);
    h = pf_end(pf_mix(h, zero, pad));
    memcpy(head, &type, 4);
    memcpy(head + 4, &size, 4);
    memcpy(head + 8, &h, 8);
    ok = fwrite(head, 1, PF_REC, pf->f) == PF_REC;
    for (i = 0; i < pieces && ok; i++) ok = fwrite(piece[i], 1, len[i], pf->f) == len[i];
    if (ok && pad) ok = fwrite(zero, 1, pad, pf->f) == pad;
    if (ok) ok = fflush(pf->f) == 0;
    if (!ok) { /* a record cut short ends what the next run reads, which is all it can tell */
        char why[256];
        WARN("vulkan: cannot write %s (%s): what is made from now on is not kept\n", pf->path,
             snx_os_error(why, sizeof why));
        fclose(pf->f);
        pf->f = NULL;
        return -1;
    }
    pf->size += PF_REC + size;
    *check = h;
    if (pf->open && pf->size - pf->open >= PF_UNIT_BYTES) pf_pack(pf);
    return 0;
}

u64 pf_shader(PFile* pf, u64 module, const PfShader* s, const u32* cbr, const u32* spv, const PfSource* src,
              const u8* code, const ShXfb* xfb) {
    const void* piece[4];
    size_t len[4];
    u64 check, at = 0;
    int bad = 0;
    pm_lock(&pf->lock);
    if (src) { /* the source first, in the same hold of the lock: the shader record that follows is what it made */
        piece[0] = src;
        len[0] = sizeof *src;
        piece[1] = cbr; /* the shader's reads */
        len[1] = (size_t)src->ncbr * 16;
        piece[2] = code;
        len[2] = src->ncode;
        piece[3] = xfb;
        len[3] = src->flags & PF_SRC_XFB ? sizeof *xfb : 0;
        bad = pf_append(pf, PF_SOURCE, piece, len, 4, &check);
    }
    piece[0] = s;
    len[0] = sizeof *s;
    piece[1] = cbr;
    len[1] = (size_t)s->ncbr * 16;
    piece[2] = spv;
    len[2] = (size_t)s->nspv * 4;
    at = pf->size;
    if (!bad && !pf_append(pf, PF_SHADER, piece, len, 3, &check)) {
        pf->nsh++;
        pf_map_add(pf, module, pf->nsh, check);
    } else
        at = 0;
    pm_unlock(&pf->lock);
    return at;
}

void pf_pipe(PFile* pf, const u64 modules[5], const void* key, u32 keylen) {
    const void* piece[2];
    size_t len[2];
    PfPipe p;
    u64 check;
    int i;
    memset(&p, 0, sizeof p);
    p.keylen = keylen;
    piece[0] = &p;
    len[0] = sizeof p;
    piece[1] = key;
    len[1] = keylen;
    pm_lock(&pf->lock);
    for (i = 0; i < 5; i++) {
        const PfMod* m;
        if (!modules[i]) continue;
        m = pf->cap ? pf_slot(pf->map, pf->cap, modules[i]) : NULL;
        if (!m || !m->module) { /* not a shader of the file (a clear's), or its record failed */
            pm_unlock(&pf->lock);
            return;
        }
        p.sh[i] = m->rec;
        p.check[i] = m->check;
    }
    pf_append(pf, PF_PIPE, piece, len, 2, &check);
    pm_unlock(&pf->lock);
}

void pf_shipref(PFile* pf, u64 module, u64 at, u64 check) {
    u64 words[2], c;
    const void* piece[1];
    size_t len[1];
    words[0] = at;
    words[1] = check;
    piece[0] = words;
    len[0] = sizeof words;
    pm_lock(&pf->lock);
    if (!pf_append(pf, PF_SHIPREF, piece, len, 1, &c)) {
        pf->nsh++;
        pf_map_add(pf, module, pf->nsh, c);
    }
    pm_unlock(&pf->lock);
}
void pf_scan(PFile* pf, u64 what, u64 id) {
    u64 words[2], check;
    const void* piece[1];
    size_t len[1];
    words[0] = what;
    words[1] = id;
    piece[0] = words;
    len[0] = sizeof words;
    pm_lock(&pf->lock);
    pf_append(pf, PF_SCAN, piece, len, 1, &check);
    pm_unlock(&pf->lock);
}

/* a unit record (at, its header) unpacked: its records' bytes (n of them in raw, cap allocated), and whether they held
   (ok: its check, its frame, and each record's check, count and type, as pf_unit_job checks them) */
typedef struct {
    const u8* at;
    u8* raw;
    size_t cap;
    u32 n;
    int ok;
} PfSlot;
#define PF_AHEAD 8 /* the units unpacked at once, one a worker */
/* What reading a file keeps from one record to the next, through its units. rec: by shader record number (from 1, a
   PF_SHADER or a PF_SHIPREF) its check and module, for the pipeline records after it. source: in a rebuild, the
   module of the last PF_SOURCE record, for the PF_SHADER record after it. slot: the units unpacked ahead, nslot of
   them from the one at islot, by workers threads (1 for none), each taking the next job; checked: their checks were
   checked already. open: where the records after the last unit start, for the file's appending to go on packing
   from */
typedef struct {
    struct {
        u64 check, module;
    }* rec;
    u32 cap, n;
    u64 source;
    PfSlot slot[PF_AHEAD];
    u32 nslot, islot, workers, job;
    int checked;
    u64 open;
} PfReader;
/* room for one more shader record: 0, or -1 */
static int pf_rec_room(PfReader* r) {
    if (r->n + 1 >= r->cap) {
        u32 c = r->cap ? r->cap * 2 : 1024;
        void* grown = realloc(r->rec, c * sizeof *r->rec);
        if (!grown) return -1;
        r->rec = grown;
        r->cap = c;
    }
    return 0;
}
/* a pipeline record (len bytes at p) to load when its shaders were all made: 0, or -1 when it isn't one */
static int pf_pipe_rec(const PfReader* r, const u8* p, u32 len, const PfLoad* load, u32 loaded[2]) {
    PfPipe q;
    u64 mods[5];
    int i, ok = 1;
    if (len < sizeof q) return -1;
    memcpy(&q, p, sizeof q);
    if (q.keylen > len - sizeof q || len - sizeof q - q.keylen >= 8) return -1;
    for (i = 0; i < 5; i++) {
        mods[i] = 0;
        if (!q.sh[i]) continue;
        /* a shader record before it, the one it names (by its check), that was made */
        if (q.sh[i] > r->n || r->rec[q.sh[i]].check != q.check[i] || !r->rec[q.sh[i]].module)
            ok = 0;
        else
            mods[i] = r->rec[q.sh[i]].module;
    }
    if (ok) {
        load->pipe(load->ctx, mods, p + sizeof q, q.keylen);
        loaded[1]++;
    }
    return 0;
}
/* a record (type, len bytes at p, its check h) of a file read to load: 0, or -1 when it ends the reading. Called with
   pf->lock */
static int pf_take(PFile* pf, PfReader* r, u32 type, const u8* p, u32 len, u64 h, const PfLoad* load, u32 loaded[2]) {
    if (type == PF_SHADER || (type == PF_SHIPREF && len == 16)) {
        PfShader s;
        u64 made;
        if (type == PF_SHADER) {
            u64 need;
            if (len < sizeof s) return -1;
            memcpy(&s, p, sizeof s);
            need = sizeof s + (u64)s.ncbr * 16 + (u64)s.nspv * 4;
            if (need > len || len - need >= 8 || s.stage > ST_FS || s.nspv < 5) return -1;
        }
        if (pf_rec_room(r)) return -1;
        /* the reads and the words are 8-byte aligned (a mapping, or malloc'd), past the fixed part's multiple of 8. A
           PF_SHIPREF names a shader record of the shipped file, by where it starts there and its check */
        made = type == PF_SHADER ? load->shader(load->ctx, &s, (const u32*)(const void*)(p + sizeof s),
                                                (const u32*)(const void*)(p + sizeof s + (size_t)s.ncbr * 16))
                                 : load->shipref(load->ctx, pf_get64(p), pf_get64(p + 8));
        r->n++;
        r->rec[r->n].check = h;
        r->rec[r->n].module = made;
        if (made) {
            pf_map_add(pf, made, r->n, h);
            loaded[0]++;
        }
        return 0;
    }
    if (type == PF_PIPE) return pf_pipe_rec(r, p, len, load, loaded);
    return type == PF_SOURCE ? 0 : -1; /* a source matters only to a rebuild (pf_retake) */
}
/* pf_take's for a file being rebuilt into a new one of another key, which load->source and load->pipe write to: a
   PF_SHADER record stands for the module the PF_SOURCE record before it was translated into again, as its layout
   (ShaderInfo's) is the old build's; a PF_SHIPREF names a record of a shipped file that was started over. Called
   without pf->lock, which the callbacks' writing takes */
static int pf_retake(PfReader* r, u32 type, const u8* p, u32 len, u64 h, const PfLoad* load, u32 loaded[2]) {
    if (type == PF_SOURCE) {
        PfSource s;
        ShXfb xfb;
        u64 need;
        if (len < sizeof s) return -1;
        memcpy(&s, p, sizeof s);
        need = sizeof s + (u64)s.ncbr * 16 + s.ncode + (s.flags & PF_SRC_XFB ? sizeof xfb : 0);
        if (need > len || len - need >= 8 || s.stage > ST_FS || s.ncode % 8) return -1;
        if (s.flags & PF_SRC_XFB) memcpy(&xfb, p + sizeof s + (size_t)s.ncbr * 16 + s.ncode, sizeof xfb);
        r->source = load->source(load->ctx, &s, (const u32*)(const void*)(p + sizeof s),
                                 p + sizeof s + (size_t)s.ncbr * 16, s.flags & PF_SRC_XFB ? &xfb : NULL);
        if (r->source) loaded[0]++;
        return 0;
    }
    if (type == PF_SHADER || type == PF_SHIPREF) {
        if (pf_rec_room(r)) return -1;
        r->n++;
        r->rec[r->n].check = h;
        r->rec[r->n].module = type == PF_SHADER ? r->source : 0;
        r->source = 0;
        return 0;
    }
    return type == PF_PIPE ? pf_pipe_rec(r, p, len, load, loaded) : -1;
}
/* the unit record at s->at unpacked into s->raw and checked whole, on any thread: its check (unless checked), its
   frame unpacked to the bytes it says, then each record's check, as many records and shader records as the unit says,
   and no unit among them. s->ok when it holds */
static void pf_unit_job(PfSlot* s, int checked) {
    u32 len = pf_get32(s->at + 4), records = 0, shaders = 0;
    const u8* p = s->at + PF_REC;
    size_t got, at = 0;
    PfUnit u;
    s->ok = 0;
    if (!checked && pf_end(pf_mix(pf_start(PF_UNIT, len), p, len)) != pf_get64(s->at + 8)) return;
    if (len < sizeof u) return;
    memcpy(&u, p, sizeof u);
    if (u.packed > len - sizeof u || len - sizeof u - u.packed >= 8 || u.raw > PF_MAX) return;
    if (u.raw > s->cap) {
        free(s->raw);
        s->cap = 0;
        if (!(s->raw = (u8*)malloc(u.raw))) return;
        s->cap = u.raw;
    }
    got = ZSTD_decompress(s->raw, u.raw, p + sizeof u, u.packed);
    if (ZSTD_isError(got) || got != u.raw) return;
    for (; u.raw - at >= PF_REC; records++) {
        u32 type = pf_get32(s->raw + at), n = pf_get32(s->raw + at + 4);
        if (n % 8 || n > u.raw - at - PF_REC || type == PF_UNIT ||
            pf_end(pf_mix(pf_start(type, n), s->raw + at + PF_REC, n)) != pf_get64(s->raw + at + 8))
            return;
        shaders += type == PF_SHADER || type == PF_SHIPREF;
        at += PF_REC + n;
    }
    s->n = u.raw;
    s->ok = at == u.raw && records == u.records && shaders == u.shaders;
}
static void* pf_unit_worker(void* arg) {
    PfReader* r = arg;
    for (;;) {
        u32 job = __atomic_fetch_add(&r->job, 1, __ATOMIC_RELAXED);
        if (job >= r->nslot) break;
        pf_unit_job(&r->slot[job], r->checked);
    }
    return NULL;
}
/* the unit records from buf + at on (as many as there are workers, while records are units), unpacked into the slots
   at once: a 34 MB file's units took 32 ms to read on one thread, where its records unpacked took 3 */
static void pf_unpack_ahead(PfReader* r, const u8* buf, size_t at, size_t size, int checked) {
    u32 n = 0, want = r->workers > 1 ? r->workers : 1;
    while (n < want && n < PF_AHEAD && size - at >= PF_REC) {
        u32 len = pf_get32(buf + at + 4);
        if (pf_get32(buf + at) != PF_UNIT || len % 8 || len > size - at - PF_REC) break;
        r->slot[n++].at = buf + at;
        at += PF_REC + len;
    }
    r->nslot = n;
    r->islot = 0;
    r->job = 0;
    r->checked = checked;
#if defined(__linux__)
    if (n > 1) {
        pthread_t threads[PF_AHEAD];
        u32 made = 0;
        for (u32 i = 1; i < n; i++)
            if (!pthread_create(&threads[made], NULL, pf_unit_worker, r)) made++;
        pf_unit_worker(r);
        for (u32 i = 0; i < made; i++) pthread_join(threads[i], NULL);
        return;
    }
#endif
    pf_unit_worker(r);
}
static size_t pf_walk(PFile* pf, PfReader* r, const u8* buf, size_t at, size_t size, int checked, const PfLoad* load,
                      u32 loaded[2]);
/* the unit record at buf + at, unpacked ahead with those after it, its records walked as the file's own: 0, or -1
   when it doesn't hold, before any of them is handed on */
static int pf_unit(PFile* pf, PfReader* r, const u8* buf, size_t at, size_t size, int checked, const PfLoad* load,
                   u32 loaded[2]) {
    const PfSlot* s;
    if (r->islot >= r->nslot || r->slot[r->islot].at != buf + at) pf_unpack_ahead(r, buf, at, size, checked);
    s = &r->slot[r->islot++];
    return s->ok && pf_walk(pf, r, s->raw, 0, s->n, 1, load, loaded) == s->n ? 0 : -1;
}
/* the records of buf[at, size) in order to load (pf_take, or pf_retake while load->rebuild), each unit's unpacked in
   its place (pf_unit; a unit holds none): how far they go while they check out, or were checked already (checked) */
static size_t pf_walk(PFile* pf, PfReader* r, const u8* buf, size_t at, size_t size, int checked, const PfLoad* load,
                      u32 loaded[2]) {
    while (size - at >= PF_REC) {
        u32 type = pf_get32(buf + at), len = pf_get32(buf + at + 4);
        const u8* p = buf + at + PF_REC;
        u64 h = pf_get64(buf + at + 8);
        if (len % 8 || len > size - at - PF_REC) break;
        if (type == PF_UNIT) { /* its check is pf_unit_job's */
            if (pf_unit(pf, r, buf, at, size, checked, load, loaded)) break;
            r->open = at + PF_REC + len;
        } else if ((!checked && h != pf_end(pf_mix(pf_start(type, len), p, len))) ||
                   (load->rebuild ? pf_retake(r, type, p, len, h, load, loaded)
                                  : pf_take(pf, r, type, p, len, h, load, loaded)))
            break;
        at += PF_REC + len;
    }
    return at;
}
/* a reading's allocations */
static void pf_reader_free(PfReader* r) {
    free(r->rec);
    for (u32 i = 0; i < PF_AHEAD; i++) free(r->slot[i].raw);
}
/* a file of another key read to rebuild it (pf_retake), from at, its first record. Its units unpack one at a time:
   translating their shaders again takes far longer */
static void pf_rebuild(const u8* buf, size_t at, size_t size, const PfLoad* load, u32 loaded[2]) {
    PfReader r = {0};
    pf_walk(NULL, &r, buf, at, size, 0, load, loaded);
    pf_reader_free(&r);
}

/* whether buf (size bytes) is a file of this format whose key's first guest bytes are key's: the size of its header,
   else 0 */
static size_t pf_same_guest(const u8* buf, size_t size, const char* key, size_t guest) {
    u32 v, len;
    if (!guest || size < 16 || memcmp(buf, PF_MAGIC, 8)) return 0;
    v = pf_get32(buf + 8);
    len = pf_get32(buf + 12);
    if ((v != PF_VERSION && v != 2) || len < guest || len > size - 16 || memcmp(buf + 16, key, guest)) return 0;
    return ((size_t)16 + len + 7) & ~(size_t)7;
}

void pf_open(PFile* pf, const char* path, const char* key, size_t guest, PfLoad* load, u32 loaded[2]) {
    size_t hsize = 0, size = 0, used = 0, old_head = 0;
    u8 *head = pf_header(key, &hsize), *owned = NULL;
    const u8* buf = NULL;
    PFileMap view = {0};
    PfReader r = {0};
    FILE* f;
    long length = -1;
    char why[256] = "";
    loaded[0] = loaded[1] = 0;
    pm_lock(&pf->lock);
    snprintf(pf->path, sizeof pf->path, "%s", path);
    if (!head) {
        pm_unlock(&pf->lock);
        return;
    }
    if (!plat_file_map(&view, path) && view.data) {
        /* Sequential validation reads the page cache directly. Keep no second file-sized allocation on a warm run. */
        if (view.size >= hsize && view.size <= 2ull * PF_MAX) {
            size = (size_t)view.size;
            length = (long)size;
            buf = view.data;
#if defined(__linux__) && !defined(__ANDROID__)
            if (!view.heap && !view.borrowed && size >= (1u << 20))
                (void)posix_madvise((void*)view.data, size, POSIX_MADV_SEQUENTIAL);
#endif
        } else
            length = view.size ? 1 : 0;
    } else {
        /* Mapping can fail even when ordinary reads work. Keep the buffered path on every platform. */
        f = fopen(path, "rb");
        if (f && !fseek(f, 0, SEEK_END)) length = ftell(f);
        if (f && length >= (long)hsize && (unsigned long)length <= 2ul * PF_MAX && !fseek(f, 0, SEEK_SET)) {
            size = (size_t)length;
            owned = (u8*)malloc(size);
            SNX_ZONE_V(z, "shader cache read", size);
            if (owned && fread(owned, 1, size, f) == size) buf = owned;
        }
        if (f) fclose(f);
    }
    if (buf && !memcmp(buf, head, hsize)) {
        int checked = 0;
        size_t end = size;
#if defined(__linux__)
        end = pf_check_parallel(buf, hsize, size, &checked);
        r.workers = (u32)pf_workers();
#endif
        r.open = hsize;
        used = pf_walk(pf, &r, buf, hsize, end, checked, load, loaded);
        pf->nsh = r.n;
        pf_reader_free(&r);
        if (used < size)
            LOG("vulkan: %s: the records past %lu of its %lu bytes were cut short or changed, and are dropped\n", path,
                (unsigned long)used, (unsigned long)size);
    } else if (buf && (old_head = pf_same_guest(buf, size, key, guest)) != 0)
        LOG("vulkan: %s is of another build or driver: its shaders are translated again and its pipelines made "
            "again\n",
            path);
    else if (length > 0)
        LOG("vulkan: %s is of another build, GPU or driver, or unreadable: started over\n", path);
    if (view.data) {
        /* A repair or rebuild must retain its bytes before truncating the file. The warm path needs none afterwards.
           Unmap before opening for writes, also for Windows, whose read-only mapping excludes write handles. */
        size_t keep = old_head ? size : used < size ? used : 0;
        if (keep) {
            owned = malloc(keep);
            if (!owned) {
                WARN("vulkan: cannot keep %s for a cache rebuild: left unchanged\n", path);
                plat_file_unmap(&view);
                free(head);
                pm_unlock(&pf->lock);
                return;
            }
            memcpy(owned, buf, keep);
        }
        buf = owned;
        plat_file_unmap(&view);
    }
    if (used && used == size) {
        pf->f = fopen(path, "ab");
        if (!pf->f) snx_os_error(why, sizeof why);
    } else { /* started over, or rewritten with the records that checked out (what a crash cut short goes) */
        int ok;
        pf->f = fopen(path, "wb");
        ok = pf->f && fwrite(used ? buf : head, 1, used ? used : hsize, pf->f) == (used ? used : hsize) &&
             !fflush(pf->f);
        if (!ok) snx_os_error(why, sizeof why); /* before fclose's */
        if (!ok && pf->f) {
            fclose(pf->f);
            pf->f = NULL;
        }
        if (!used) {
            pf->nsh = 0;
            used = hsize;
        }
    }
    pf->size = used;
    pf->open = r.open ? r.open : used; /* the records after its last unit, or none */
    if (!pf->f)
        WARN("vulkan: cannot write %s (%s): shaders and pipelines made are not kept for the next run\n", path, why);
    free(head);
    pm_unlock(&pf->lock);
    if (old_head) { /* into the file just started over, which the callbacks write to */
        load->rebuild = 1;
        pf_rebuild(buf, old_head, size, load, loaded);
        load->rebuild = 0;
    }
    free(owned);
}

unsigned pf_pid(void) {
#if defined(_WIN32)
    return (unsigned)GetCurrentProcessId();
#else
    return (unsigned)getpid();
#endif
}
void pf_open_mapped(PFile* pf, const char* path, const char* key, PfEach each, void* ctx) {
    size_t hsize = 0;
    u8* head = pf_header(key, &hsize);
    u64 at = 0, size = 0;
    char why[256] = "";
    int mapped;
    FILE* probe;
    pm_lock(&pf->lock);
    snprintf(pf->path, sizeof pf->path, "%s", path);
    if (!head) {
        pm_unlock(&pf->lock);
        return;
    }
    mapped = !plat_file_map(&pf->view, path);
    if (!mapped) snx_os_error(why, sizeof why); /* the map's reason, before the probe's calls */
    if (mapped && pf->view.data) size = pf->view.size;
    if (!mapped && (probe = fopen(path, "rb")) != NULL) { /* there, but not mapped: kept as it is, for a later run */
        fclose(probe);
        WARN("vulkan: cannot map %s (%s): its shaders aren't used this run, nor added to\n", path, why);
        pf->kept = 1;
        free(head);
        pm_unlock(&pf->lock);
        return;
    }
    if (size >= hsize && !memcmp(pf->view.data, head, hsize)) {
        const u8* d = pf->view.data;
        for (at = hsize; size - at >= PF_REC;) {
            u32 type = pf_get32(d + at), len = pf_get32(d + at + 4);
            /* a tail of no known type (zeros a lost write left) ends the records as one cut short does */
            if (len % 8 || len > size - at - PF_REC || type < PF_SHADER || type > PF_SHIPREF) break;
            each(ctx, type, d + at + PF_REC, len, at);
            at += PF_REC + len;
        }
    } else if (size)
        LOG("vulkan: %s is of another build, GPU or driver: started over\n", path);
    if (!at || at < size) {
        /* started over (its header alone), or rewritten without what a crash cut short: written beside it and moved
           over it, so another run that has it mapped keeps reading the old one */
        char* tmp = snx_format("%s.%u.tmp", path, pf_pid());
        FILE* t = tmp ? fopen(tmp, "wb") : NULL;
        const u8* keep = at ? pf->view.data : head;
        size_t n = at ? (size_t)at : hsize;
        int ok = t && fwrite(keep, 1, n, t) == n;
        if (!ok) snx_os_error(why, sizeof why); /* each failure's reason where it happened, before the cleanup's */
        if (t && fclose(t) && ok) {
            snx_os_error(why, sizeof why);
            ok = 0;
        }
        plat_file_unmap(&pf->view);
        if (ok && plat_replace_file(tmp, path)) {
            snx_os_error(why, sizeof why);
            ok = 0;
        }
        if (!ok && tmp) remove(tmp);
        free(tmp);
        if (!at) at = hsize;
        if (!ok) { /* as it was: nothing is appended after what it holds */
            WARN("vulkan: cannot write %s (%s): what is made is not kept for the next run\n", path, why);
            pf->kept = 1;
        }
        if (plat_file_map(&pf->view, path)) memset(&pf->view, 0, sizeof pf->view);
    }
    pf->size = at;
    free(head);
    pm_unlock(&pf->lock);
}
const u8* pf_payload(const PFile* pf, u64 at, u32* len, u64* check) {
    const u8* d = pf->view.data;
    u32 type, n;
    if (!d || at > pf->view.size || pf->view.size - at < PF_REC) return NULL;
    type = pf_get32(d + at);
    n = pf_get32(d + at + 4);
    if (n > pf->view.size - at - PF_REC ||
        pf_end(pf_mix(pf_start(type, n), d + at + PF_REC, n)) != pf_get64(d + at + 8))
        return NULL;
    *len = n;
    *check = pf_get64(d + at + 8);
    return d + at + PF_REC;
}
void pf_unseal(PFile* pf) {
    pm_lock(&pf->lock);
    plat_file_unmap(&pf->view);
    if (!pf->kept && !pf->f) {
        char why[256];
        pf->f = fopen(pf->path, "ab");
        if (!pf->f)
            WARN("vulkan: cannot write %s (%s): what is made is not kept for the next run\n", pf->path,
                 snx_os_error(why, sizeof why));
    }
    pm_unlock(&pf->lock);
}
void pf_close(PFile* pf) {
    pm_lock(&pf->lock);
    if (pf->f) {
        fclose(pf->f);
        pf->f = NULL;
    }
    plat_file_unmap(&pf->view);
    pm_unlock(&pf->lock);
}
void pf_seal(PFile* pf) {
    pm_lock(&pf->lock);
    if (pf->f) {
        fclose(pf->f);
        pf->f = NULL;
    }
    plat_file_unmap(&pf->view);
    if (plat_file_map(&pf->view, pf->path)) memset(&pf->view, 0, sizeof pf->view);
    pm_unlock(&pf->lock);
}
