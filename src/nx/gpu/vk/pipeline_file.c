/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The file of translated shaders and pipelines made (vk.h's pipeline_file.c). The main file's records are held in
   memory as they are made and written when the game closes, packed into units (pf_save); the others' are appended
   and flushed as they are made. Read once at startup. It calls no Vulkan function: pipeline.c makes the modules and
   pipelines from what it hands back */
#include <stdlib.h>
#include <string.h>
#include <zstd.h>
#include <stdatomic.h>
#include "common/platform/time.h"
#include "common/utilities/common.h"
#include "nx/runtime/platform/log.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/pipeline_file.h"
#if !defined(_WIN32)
#include <pthread.h> /* pf_save's workers */
#include <unistd.h>  /* getpid */
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
/* zstd's level for a shipped file's shader packed alone: level 1, which packed the shipped files measured to 25.6% in
   46 us a shader, where level 3 gave 25.7% in 57 us */
#define PF_ZSHADER_LEVEL 1
/* a PF_ZSHADER's payload after its PfShader: the bytes of its reads and SPIR-V, and of the frame that packs them */
typedef struct {
    u32 raw, packed;
} PfZip;
/* a PF_ZSHADER's frame starts at a multiple of 8, as the PfShader's fields are read in place */
typedef char pf_check_shader_size[sizeof(PfShader) % 8 == 0 && sizeof(PfZip) == 8 ? 1 : -1];

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
    u32 count;
    _Atomic u32 next;
} PfChecks;
static void* pf_check_worker(void* arg) {
    PfChecks* c = arg;
    for (;;) {
        u32 job = atomic_fetch_add_explicit(&c->next, 1, memory_order_relaxed);
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
    int n = plat_cpu_count();
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

/* one unit of the records being saved: n bytes of them at raw, and the record that packs them (total 0: they are
   written as they are, as records that don't come out smaller are); done once it is either, not when a stop came first */
typedef struct {
    const u8* raw;
    size_t n;
    u8* rec;
    size_t total;
    PfUnit u;
    int done;
} PfSaveUnit;
typedef struct {
    PFile* pf; /* its progress and its stop */
    PfSaveUnit* unit;
    _Atomic u32 next;
    u32 count;
    int level;
} PfSave;
/* the unit packed in steps of PF_SAVE_STEP bytes into one zstd frame, through z (a thread's own), so that a stop
   ends it after the step at work */
static void pf_save_unit(PFile* pf, PfSaveUnit* k, ZSTD_CCtx* z) {
    size_t bound = ZSTD_compressBound(k->n), r = 0;
    k->total = 0;
    k->rec = (u8*)malloc(PF_REC + sizeof k->u + bound + 8);
    if (!k->rec || !z) {
        k->done = 1;
        atomic_fetch_add_explicit(&pf->save_packed, k->n, memory_order_relaxed);
        return;
    }
    ZSTD_outBuffer out = {k->rec + PF_REC + sizeof k->u, bound, 0};
    ZSTD_inBuffer in = {k->raw, 0, 0};
    ZSTD_CCtx_reset(z, ZSTD_reset_session_only);
    ZSTD_CCtx_setPledgedSrcSize(z, k->n);
    while (in.size < k->n || r) {
        if (atomic_load_explicit(&pf->save_stop, memory_order_relaxed)) return;
        const size_t step = k->n - in.size > PF_SAVE_STEP ? PF_SAVE_STEP : k->n - in.size;
        in.size += step;
        /* the output holds the frame's bound, so each call takes its step whole; the last ends the frame */
        r = ZSTD_compressStream2(z, &out, &in, in.size == k->n ? ZSTD_e_end : ZSTD_e_continue);
        if (ZSTD_isError(r) || (r && out.pos == out.size)) {
            LOG("vulkan: cannot pack %zu bytes of shader cache records (zstd: %s): they are saved unpacked\n", k->n,
                ZSTD_isError(r) ? ZSTD_getErrorName(r) : "no room left for the frame");
            k->done = 1;
            atomic_fetch_add_explicit(&pf->save_packed, k->n - (in.size - step), memory_order_relaxed);
            return;
        }
        atomic_fetch_add_explicit(&pf->save_packed, step, memory_order_relaxed);
    }
    k->done = 1;
    const size_t packed = out.pos, total = PF_REC + sizeof k->u + ((packed + 7) & ~(size_t)7);
    if (total >= k->n) return;
    u32 type = PF_UNIT, len = (u32)(total - PF_REC);
    k->u.raw = (u32)k->n;
    k->u.packed = (u32)packed;
    memcpy(k->rec + PF_REC, &k->u, sizeof k->u);
    memset(k->rec + PF_REC + sizeof k->u + packed, 0, total - PF_REC - sizeof k->u - packed);
    u64 h = pf_end(pf_mix(pf_start(type, len), k->rec + PF_REC, len));
    memcpy(k->rec, &type, 4);
    memcpy(k->rec + 4, &len, 4);
    memcpy(k->rec + 8, &h, 8);
    k->total = total;
}
static void* pf_save_worker(void* arg) {
    PfSave* v = arg;
    ZSTD_CCtx* z = ZSTD_createCCtx();
    if (z && ZSTD_isError(ZSTD_CCtx_setParameter(z, ZSTD_c_compressionLevel, v->level))) {
        ZSTD_freeCCtx(z);
        z = NULL;
    }
    if (!z) LOG("vulkan: no zstd context for the shader cache's save: its records are saved unpacked\n");
    while (!atomic_load_explicit(&v->pf->save_stop, memory_order_relaxed)) {
        u32 i = atomic_fetch_add_explicit(&v->next, 1, memory_order_relaxed);
        if (i >= v->count) break;
        pf_save_unit(v->pf, &v->unit[i], z);
    }
    ZSTD_freeCCtx(z);
    return NULL;
}
void pf_saving(PFile* pf, u64* total, u64* packed, u64* kept) {
    *total = atomic_load_explicit(&pf->save_total, memory_order_relaxed);
    *packed = atomic_load_explicit(&pf->save_packed, memory_order_relaxed);
    *kept = atomic_load_explicit(&pf->save_kept, memory_order_relaxed);
}
void pf_save_stop(PFile* pf) { atomic_store_explicit(&pf->save_stop, 1, memory_order_relaxed); }
/* the threads that pack a save's units: the CPUs but two, eight at most, one where there are no more units */
#define PF_SAVE_THREADS 8
void pf_save(PFile* pf) {
    u8* held;
    u64 n, at = 0, wrote = 0, kept = 0;
    const u64 t0 = plat_ns();
    PfSave v = {.pf = pf, .level = ZSTD_maxCLevel()};
    u32 cap = 0, threads = 1, whole = 0;
    int ok = 1;
    char why[256] = "out of memory";
    /* the records leave the file's lock with this thread: the game's threads append to a new run of them meanwhile,
       which follows these in the file at the next save */
    pm_lock(&pf->lock);
    held = pf->held;
    n = pf->held_n;
    pf->held = NULL;
    pf->held_n = pf->held_cap = 0;
    pf->held_full = 0;
    pm_unlock(&pf->lock);
    atomic_store_explicit(&pf->save_kept, 0, memory_order_relaxed);
    atomic_store_explicit(&pf->save_packed, 0, memory_order_relaxed);
    atomic_store_explicit(&pf->save_total, n, memory_order_relaxed);
    if (!n) {
        free(held);
        return;
    }
    while (at < n) { /* a unit: whole records until they reach PF_UNIT_BYTES, the last one what is left */
        size_t un = 0;
        PfUnit u = {0};
        while (at + un < n && un < PF_UNIT_BYTES) {
            const u32 type = pf_get32(held + at + un), len = pf_get32(held + at + un + 4);
            u.records++;
            u.shaders += type == PF_SHADER || type == PF_SHIPREF;
            un += PF_REC + len;
        }
        if (v.count == cap) {
            PfSaveUnit* grown = (PfSaveUnit*)realloc(v.unit, (size_t)(cap ? cap * 2 : 64) * sizeof *grown);
            if (!grown) {
                ok = 0;
                break;
            }
            v.unit = grown;
            cap = cap ? cap * 2 : 64;
        }
        v.unit[v.count++] = (PfSaveUnit){held + at, un, NULL, 0, u, 0};
        at += un;
    }
    if (ok) {
#if !defined(_WIN32)
        pthread_t th[PF_SAVE_THREADS];
        u32 made = 0;
        const int cpus = plat_cpu_count() - 2;
        threads = cpus > PF_SAVE_THREADS ? PF_SAVE_THREADS : cpus > 1 ? (u32)cpus : 1;
        if (threads > v.count) threads = v.count;
        for (u32 i = 1; i < threads; i++)
            if (!pthread_create(&th[made], NULL, pf_save_worker, &v)) made++;
        pf_save_worker(&v);
        for (u32 i = 0; i < made; i++) pthread_join(th[i], NULL);
        threads = made + 1;
#else
        pf_save_worker(&v);
#endif
        /* the units in their order up to the first a stop left unpacked: a later one's records would be numbered
           after those left out */
        while (whole < v.count && v.unit[whole].done) kept += v.unit[whole++].n;
        pm_lock(&pf->lock);
        ok = pf->f != NULL;
        if (!ok) snprintf(why, sizeof why, "the file is closed");
        for (u32 i = 0; i < whole && ok; i++) {
            const PfSaveUnit* k = &v.unit[i];
            ok = k->total ? fwrite(k->rec, 1, k->total, pf->f) == k->total : fwrite(k->raw, 1, k->n, pf->f) == k->n;
            wrote += k->total ? k->total : k->n;
        }
        if (ok) ok = !fflush(pf->f);
        if (!ok && pf->f) {
            snx_os_error(why, sizeof why);
            fclose(pf->f); /* a unit cut short ends what the next run reads */
            pf->f = NULL;
        }
        if (ok) {
            pf->open += wrote;
            pf->size -= n - wrote;
        }
        pm_unlock(&pf->lock);
    }
    for (u32 i = 0; i < v.count; i++) free(v.unit[i].rec);
    free(v.unit);
    free(held);
    atomic_store_explicit(&pf->save_kept, ok ? kept : 0, memory_order_relaxed);
    if (ok && kept == n)
        LOG("vulkan: %s: the %.1f MiB of shaders and pipelines this session made saved as %.1f MiB in %.0f ms (zstd "
            "level %d, %u thread(s))\n",
            pf->path, (double)n / 1048576.0, (double)wrote / 1048576.0, (double)(plat_ns() - t0) / 1e6, v.level,
            threads);
    else if (ok)
        LOG("vulkan: %s: the save was stopped: %.1f of the %.1f MiB of shaders and pipelines this session made saved as "
            "%.1f MiB in %.0f ms (%u of %u units), the other %.1f MiB not kept for the next run\n",
            pf->path, (double)kept / 1048576.0, (double)n / 1048576.0, (double)wrote / 1048576.0,
            (double)(plat_ns() - t0) / 1e6, whole, v.count, (double)(n - kept) / 1048576.0);
    else
        WARN("vulkan: cannot write %s (%s): the %.1f MiB of shaders and pipelines this session made are not kept "
             "for the next run\n",
             pf->path, why, (double)n / 1048576.0);
}

/* appends a record of pieces (each a multiple of 4 bytes), padded to 8: 0, or -1 when it can't be (the file is full or
   a write failed; nothing more is appended then). A PF_PACK_UNITS file's goes to the records held for pf_save, and the
   file is not touched. Called with pf->lock */
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
    /* a file whose records are held: max bounds the file itself, and pf_save's units may pass it once */
    if (payload + pad > 0xFFFFFFF0u || (pf->open ? pf->open : pf->size + PF_REC + payload + pad) > max) {
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
    if (pf->open) {
        u8* at;
        const u64 held_max = pf->held_max;
        if (pf->held_n + PF_REC + size > held_max) { /* what is held is still saved */
            if (!pf->held_full)
                LOG("vulkan: %s holds %llu MiB of this session's shaders and pipelines in memory (%llu MiB at most): "
                    "what is made from now on is not kept for the next run\n",
                    pf->path, (unsigned long long)(pf->held_n >> 20), (unsigned long long)(held_max >> 20));
            pf->held_full = 1;
            return -1;
        }
        if (pf->held_n + PF_REC + size > pf->held_cap) {
            u64 cap = pf->held_cap ? pf->held_cap * 2 : 4u << 20;
            u8* grown;
            while (cap < pf->held_n + PF_REC + size) cap *= 2;
            grown = (u8*)realloc(pf->held, (size_t)cap);
            if (!grown) {
                if (!pf->held_full)
                    WARN("vulkan: no memory to hold %llu MiB of this session's shaders and pipelines for %s: what "
                         "is made from now on is not kept for the next run\n",
                         (unsigned long long)(cap >> 20), pf->path);
                pf->held_full = 1;
                return -1;
            }
            pf->held = grown;
            pf->held_cap = cap;
        }
        at = pf->held + pf->held_n;
        memcpy(at, head, PF_REC);
        at += PF_REC;
        for (i = 0; i < pieces; i++) {
            memcpy(at, piece[i], len[i]);
            at += len[i];
        }
        memset(at, 0, pad);
        pf->held_n += PF_REC + size;
        pf->size += PF_REC + size;
        *check = h;
        return 0;
    }
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
    return 0;
}

/* a shader record's reads and SPIR-V packed into one zstd frame (*z its sizes), malloc'd with its padding to 8 zeroed:
   NULL when it can't be, or wouldn't make the record smaller, and the record is written as it is. On the caller's
   thread, before the file's lock */
static u8* pf_zip(const PfShader* s, const u32* cbr, const u32* spv, PfZip* z) {
    size_t ncbr = (size_t)s->ncbr * 16, raw = ncbr + (size_t)s->nspv * 4, bound = ZSTD_compressBound(raw), packed;
    u8 *in = (u8*)malloc(raw), *out = in ? (u8*)malloc(bound + 8) : NULL;
    if (!out) {
        LOG("vulkan: no memory to pack a shader of %zu bytes: it is written unpacked\n", raw);
        free(in);
        return NULL;
    }
    memcpy(in, cbr, ncbr);
    memcpy(in + ncbr, spv, raw - ncbr);
    packed = ZSTD_compress(out, bound, in, raw, PF_ZSHADER_LEVEL);
    free(in);
    if (ZSTD_isError(packed)) {
        LOG("vulkan: zstd can't pack a shader of %zu bytes (%s): it is written unpacked\n", raw,
            ZSTD_getErrorName(packed));
        free(out);
        return NULL;
    }
    if (sizeof *z + ((packed + 7) & ~(size_t)7) >= raw) { /* a small one: the frame's own bytes outweigh it */
        free(out);
        return NULL;
    }
    memset(out + packed, 0, ((packed + 7) & ~(size_t)7) - packed);
    z->raw = (u32)raw;
    z->packed = (u32)packed;
    return out;
}

u64 pf_shader(PFile* pf, u64 module, const PfShader* s, const u32* cbr, const u32* spv, const PfSource* src,
              const u8* code, const ShXfb* xfb) {
    const void* piece[4];
    size_t len[4];
    u64 check, at = 0;
    int bad = 0;
    PfZip z = {0, 0};
    /* pack is set before the file is opened, and a shipped file's shaders carry no source */
    u8* frame = pf->pack == PF_PACK_SHADERS && !src ? pf_zip(s, cbr, spv, &z) : NULL;
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
    piece[1] = frame ? (const void*)&z : (const void*)cbr;
    len[1] = frame ? sizeof z : (size_t)s->ncbr * 16;
    piece[2] = frame ? (const void*)frame : (const void*)spv;
    len[2] = frame ? ((size_t)z.packed + 7) & ~(size_t)7 : (size_t)s->nspv * 4; /* pf_append's pieces: words */
    at = pf->size;
    if (!bad && !pf_append(pf, frame ? PF_ZSHADER : PF_SHADER, piece, len, 3, &check)) {
        pf->nsh++;
        pf_map_add(pf, module, pf->nsh, check);
    } else
        at = 0;
    pm_unlock(&pf->lock);
    free(frame);
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

/* in one piece padded to 8: pf_mix takes whole words of each piece, and the key and the program are of any size */
void pf_cspipe(PFile* pf, const PfCsPipe* c, const u32* cbr, const void* key, const u8* code) {
    size_t at = sizeof *c + (size_t)c->ncbr * 16, n = (at + (c->nkey + 7u) / 8 * 8 + c->ncode + 7u) & ~(size_t)7;
    u8* payload = calloc(1, n);
    const void* piece[1] = {payload};
    size_t len[1] = {n};
    u64 check;
    if (!payload) return;
    memcpy(payload, c, sizeof *c);
    memcpy(payload + sizeof *c, cbr, (size_t)c->ncbr * 16);
    memcpy(payload + at, key, c->nkey);
    memcpy(payload + at + (c->nkey + 7u) / 8 * 8, code, c->ncode);
    pm_lock(&pf->lock);
    pf_append(pf, PF_CSPIPE, piece, len, 1, &check);
    pm_unlock(&pf->lock);
    free(payload);
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
    u32 nslot, islot, workers;
    _Atomic u32 job;
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
/* a stage a shader record can hold: a draw's, the Ounce's task and mesh stages too (compute pipelines have records of
   their own, PF_CSPIPE). Only up to the fragment stage, the first task or mesh shader of one game's file ended its
   reading, and every record after it was dropped: each run made most of its pipelines again */
static int pf_graphics_stage(u32 stage) { return stage <= ST_MS && stage != ST_CS; }
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
/* a compute pipeline record (len bytes at p) handed to load: 0, or -1 when it isn't one */
static int pf_cspipe_rec(const u8* p, u32 len, const PfLoad* load) {
    PfCsPipe c;
    u64 need;
    if (len < sizeof c) return -1;
    memcpy(&c, p, sizeof c);
    need = sizeof c + (u64)c.ncbr * 16 + ((u64)c.nkey + 7) / 8 * 8 + c.ncode;
    if (need > len || len - need >= 8) return -1;
    if (load->cspipe)
        load->cspipe(load->ctx, &c, (const u32*)(const void*)(p + sizeof c), p + sizeof c + (size_t)c.ncbr * 16,
                     p + sizeof c + (size_t)c.ncbr * 16 + (c.nkey + 7) / 8 * 8);
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
            if (need > len || len - need >= 8 || !pf_graphics_stage(s.stage) || s.nspv < 5) return -1;
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
    if (type == PF_CSPIPE) return pf_cspipe_rec(p, len, load);
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
        if (need > len || len - need >= 8 || !pf_graphics_stage(s.stage) || s.ncode % 8) return -1;
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
    if (type == PF_CSPIPE) return pf_cspipe_rec(p, len, load);
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
        u32 job = atomic_fetch_add_explicit(&r->job, 1, memory_order_relaxed);
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
    atomic_store_explicit(&r->job, 0, memory_order_relaxed);
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

/* the records a file holds in memory at most, for a machine of total bytes of memory (known 0: it didn't tell) */
static u64 pf_held_share(int known, u64 total) { return known ? total / PF_HELD_SHARE : PF_HELD_UNKNOWN; }
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
    if (!pf->held_max) {
        SnxU64 total = 0;
        const int known = snx_memory_total(&total);
        pf->held_max = pf_held_share(known, total);
    }
    /* the most a file is: what an earlier version or a save left under max, and a save of all a session may hold */
    const u64 most = 2ull * PF_MAX + pf->held_max;
    snprintf(pf->path, sizeof pf->path, "%s", path);
    if (!head) {
        pm_unlock(&pf->lock);
        return;
    }
    if (!plat_file_map(&view, path) && view.data) {
        /* Sequential validation reads the page cache directly. Keep no second file-sized allocation on a warm run. */
        if (view.size >= hsize && view.size <= most) {
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
        if (f && length >= (long)hsize && (unsigned long long)length <= most && !fseek(f, 0, SEEK_SET)) {
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
    } else if (buf && (old_head = pf_same_guest(buf, size, key, guest)) != 0) {
        char diff[768];
        pf_key_diff(diff, sizeof diff, (const char*)buf + 16, pf_get32(buf + 12), key);
        LOG("vulkan: %s is of another build or driver (%s): its shaders are translated again and its pipelines made "
            "again\n",
            path, diff);
    } else if (length > 0) {
        char diff[768];
        if (buf && size >= 16 && !memcmp(buf, PF_MAGIC, 8) && pf_get32(buf + 12) <= size - 16)
            pf_key_diff(diff, sizeof diff, (const char*)buf + 16, pf_get32(buf + 12), key);
        else
            snprintf(diff, sizeof diff, "%ld bytes without this format's header", length);
        LOG("vulkan: %s started over: its key differs from this GPU's, driver's and build's (%s)\n", path, diff);
    }
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
    /* a file that packs into units is written by pf_save alone: its own bytes, which the records held follow */
    pf->open = pf->pack != PF_PACK_UNITS ? 0 : used;
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

int pf_adopt(const char* path, const char* legacy, u64* bytes, char* why, size_t n) {
    FILE *in, *out, *there = fopen(path, "rb");
    char* tmp;
    char block[1 << 16];
    size_t got;
    int ok = 1;
    *bytes = 0;
    if (n) why[0] = 0;
    if (there) {
        fclose(there);
        return 0;
    }
    in = fopen(legacy, "rb");
    if (!in) return 0;
    tmp = snx_format("%s.%u.tmp", path, pf_pid());
    out = tmp ? fopen(tmp, "wb") : NULL;
    if (!out) {
        snx_os_error(why, n);
        fclose(in);
        free(tmp);
        return -1;
    }
    while (ok && (got = fread(block, 1, sizeof block, in)) > 0) {
        ok = fwrite(block, 1, got, out) == got;
        *bytes += got;
    }
    if (ok && ferror(in)) ok = 0;
    if (!ok) snx_os_error(why, n); /* the failure's reason, before the cleanup's calls */
    fclose(in);
    if (fclose(out) && ok) {
        snx_os_error(why, n);
        ok = 0;
    }
    if (ok && plat_replace_file(tmp, path)) {
        snx_os_error(why, n);
        ok = 0;
    }
    if (!ok) remove(tmp);
    free(tmp);
    return ok ? 1 : -1;
}

/* a key's fields: name=value words, and the words with no = (the build's name) as one field, "build" */
typedef struct {
    char name[24], value[160];
} PfField;
static int pf_fields(const char* key, size_t len, PfField* f, int max) {
    int count = 0, build = -1;
    size_t at = 0;
    while (at < len) {
        size_t end = at, eq = 0;
        while (end < len && key[end] != ' ') {
            if (key[end] == '=' && !eq) eq = end;
            end++;
        }
        size_t wlen = end - at;
        if (wlen && key[end - 1] == ';') wlen--; /* the guest part's end */
        if (wlen) {
            if (eq && eq < at + wlen && count < max) {
                size_t nl = eq - at, vl = at + wlen - eq - 1;
                snprintf(f[count].name, sizeof f[count].name, "%.*s", (int)nl, key + at);
                snprintf(f[count].value, sizeof f[count].value, "%.*s", (int)vl, key + eq + 1);
                count++;
            } else if (!eq) {
                if (build < 0 && count < max) {
                    build = count++;
                    snprintf(f[build].name, sizeof f[build].name, "build");
                    f[build].value[0] = 0;
                }
                if (build >= 0) {
                    size_t have = strlen(f[build].value);
                    snprintf(f[build].value + have, sizeof f[build].value - have, "%s%.*s", have ? " " : "", (int)wlen,
                             key + at);
                }
            }
        }
        at = end + 1;
    }
    return count;
}
/* the field of f (count of them) that is the nth (from 0) of that name, NULL for none: a key names "sizes" twice */
static const PfField* pf_field(const PfField* f, int count, const char* name, int nth) {
    for (int k = 0; k < count; k++)
        if (!strcmp(f[k].name, name) && nth-- == 0) return &f[k];
    return NULL;
}
static int pf_nth(const PfField* f, int i) {
    int nth = 0;
    for (int k = 0; k < i; k++) nth += !strcmp(f[k].name, f[i].name);
    return nth;
}
int pf_key_diff(char* out, size_t n, const char* key, size_t len, const char* want) {
    PfField was[32], now[32];
    int nw = pf_fields(key, len, was, 32), nn = pf_fields(want, strlen(want), now, 32), count = 0;
    size_t at = 0;
    if (n) out[0] = 0;
    for (int i = 0; i < nw + nn; i++) {
        const PfField* a = i < nw ? &was[i] : NULL;
        const PfField* b = i < nw ? NULL : &now[i - nw];
        const char* name = a ? a->name : b->name;
        const char *in_file = a ? a->value : NULL, *in_now = b ? b->value : NULL;
        if (a) {
            const PfField* other = pf_field(now, nn, name, pf_nth(was, i));
            in_now = other ? other->value : NULL;
        }
        if (b && pf_field(was, nw, name, pf_nth(now, i - nw))) continue; /* compared from the file's side */
        if (in_file && in_now && !strcmp(in_file, in_now)) continue;
        count++;
        if (at + 1 < n) {
            int w = snprintf(out + at, n - at, "%s%s %s in the file, %s now", at ? "; " : "", name,
                             in_file ? in_file : "none", in_now ? in_now : "none");
            at = w < 0 ? n : at + (size_t)w;
            if (at >= n) at = n - 1;
        }
    }
    return count;
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
            if (len % 8 || len > size - at - PF_REC || ((type < PF_SHADER || type > PF_SHIPREF) && type != PF_ZSHADER))
                break;
            each(ctx, type, d + at + PF_REC, len, at);
            at += PF_REC + len;
        }
    } else if (size) {
        char diff[768];
        const u8* d = pf->view.data;
        if (size >= 16 && !memcmp(d, PF_MAGIC, 8) && pf_get32(d + 12) <= size - 16)
            pf_key_diff(diff, sizeof diff, (const char*)d + 16, pf_get32(d + 12), key);
        else
            snprintf(diff, sizeof diff, "%llu bytes without this format's header", (unsigned long long)size);
        LOG("vulkan: %s started over: its key differs from this GPU's, driver's and build's (%s)\n", path, diff);
    }
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
/* a PF_ZSHADER's payload (n bytes at p, of the file at path) unpacked into the PF_SHADER payload it was made from,
   malloc'd: NULL when it doesn't unpack to the bytes it says, which its check holding leaves to a writer's bug or to
   memory running out, said in the log */
static u8* pf_unzip(const char* path, u64 at, const u8* p, u32 n, u32* len) {
    PfZip z;
    PfShader s;
    size_t got;
    u8* out;
    if (n < sizeof s + sizeof z) return NULL;
    memcpy(&s, p, sizeof s);
    memcpy(&z, p + sizeof s, sizeof z);
    if (z.packed > n - sizeof s - sizeof z || n - sizeof s - sizeof z - z.packed >= 8 || z.raw > PF_MAX ||
        z.raw != (u64)s.ncbr * 16 + (u64)s.nspv * 4) {
        LOG("vulkan: %s: the packed shader at %llu says %u bytes packed to %u, which its record of %u doesn't hold\n",
            path, (unsigned long long)at, z.raw, z.packed, n);
        return NULL;
    }
    if (!(out = (u8*)malloc(sizeof s + z.raw))) {
        LOG("vulkan: %s: no memory to unpack the shader at %llu, of %u bytes\n", path, (unsigned long long)at, z.raw);
        return NULL;
    }
    memcpy(out, &s, sizeof s);
    got = ZSTD_decompress(out + sizeof s, z.raw, p + sizeof s + sizeof z, z.packed);
    if (ZSTD_isError(got) || got != z.raw) {
        LOG("vulkan: %s: the packed shader at %llu unpacks to %zu bytes, not %u (%s)\n", path, (unsigned long long)at,
            ZSTD_isError(got) ? (size_t)0 : got, z.raw, ZSTD_isError(got) ? ZSTD_getErrorName(got) : "short");
        free(out);
        return NULL;
    }
    *len = (u32)(sizeof s + z.raw);
    return out;
}
const u8* pf_payload(const PFile* pf, u64 at, u32* len, u64* check, u8** owned) {
    const u8* d = pf->view.data;
    u32 type, n;
    *owned = NULL;
    if (!d || at > pf->view.size || pf->view.size - at < PF_REC) return NULL;
    type = pf_get32(d + at);
    n = pf_get32(d + at + 4);
    if (n > pf->view.size - at - PF_REC ||
        pf_end(pf_mix(pf_start(type, n), d + at + PF_REC, n)) != pf_get64(d + at + 8))
        return NULL;
    *check = pf_get64(d + at + 8);
    if (type == PF_ZSHADER) return *owned = pf_unzip(pf->path, at, d + at + PF_REC, n, len);
    *len = n;
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
