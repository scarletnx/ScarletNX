/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Save states. A save pauses the game (pause.c), lands what the GPU holds of guest memory, and writes the address
   space, g_hle, the services' heap memory and the GPU's state into a state file (state_file.c). A load, in the same
   process, reads and checks all of it first, then ends every guest thread where it stopped (kernel_retire), puts the
   state's memory, g_hle and GPU in place of the running ones, makes the host's side of the services again for what
   the state holds, and starts each guest thread again as it stopped (kthread_resume).

   g_hle is written as it is (tests/hle_flat.c keeps it free of the host's objects), less the fields this file
   rewrites: pointers into g_hle become offsets from it, and functions and the service objects' names their names
   (state_fns.h, the NAMES section), so any build with the same layout reads them back wherever it was loaded. Heap
   memory a member owns goes into the HEAP section beside it, and what the host's side keeps of the game (caches, the
   file tree, the decoders) stays the running process's. */
#include "nx/runtime/state.h"
#include "common/utilities/build_info.h"
#include "common/utilities/endian.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/renderer.h"
#include "nx/gpu/video/host1x.h"
#include "nx/hle/hle.h"
#include "nx/hle/hle_walk.h"
#include "nx/hle/services/audren_dsp.h"
#include "nx/hle/services/services.h"
#include "nx/memory/mem.h"
#include "nx/memory/own.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/main.h"
#include "nx/runtime/pause.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/overlay.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/ui.h"
#include "nx/runtime/platform/unattended.h"
#include "nx/runtime/options.h"
#include "nx/runtime/state_fns.h"
#include "nx/runtime/state_io.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef char state_pointers_are_8_bytes[sizeof(void*) == 8 ? 1 : -1]; /* pointer fields hold the numbers below */

/* ============================================================ streams */
/* a stream in memory: a section read whole before a load changes anything, or one written before it goes out */
typedef struct {
    u8* p;
    u64 n, at, cap;
} Buf;
static int buf_put(void* ctx, const void* p, u64 n) {
    Buf* b = (Buf*)ctx;
    if (b->n + n > b->cap) {
        u64 cap = b->cap ? b->cap * 2 : 1 << 16;
        u8* q;
        while (cap < b->n + n) cap *= 2;
        if (!(q = (u8*)realloc(b->p, (size_t)cap))) return -1;
        b->p = q;
        b->cap = cap;
    }
    if (n) memcpy(b->p + b->n, p, (size_t)n);
    b->n += n;
    return 0;
}
static int buf_get(void* ctx, void* p, u64 n) {
    Buf* b = (Buf*)ctx;
    if (n > b->n - b->at) return -1;
    if (n) memcpy(p, b->p + b->at, (size_t)n);
    b->at += n;
    return 0;
}
static StateIo buf_io(Buf* b) {
    StateIo io = {b, buf_put, buf_get};
    return io;
}
/* a section of the reader read whole into b: 0, or -1 when the state has none or it is damaged */
static int section_read(StateReader* r, const char* tag, u64* schema, Buf* b) {
    u64 n;
    if (sr_begin(r, tag, schema)) return -1;
    n = sr_left(r);
    b->p = (u8*)malloc(n ? (size_t)n : 1);
    b->n = b->cap = n;
    b->at = 0;
    return b->p && !sr_get(r, b->p, n) ? 0 : -1;
}
static int section_write(StateWriter* w, const char* tag, u64 schema, const Buf* b) {
    return sw_begin(w, tag, schema) || sw_put(w, b->p, b->n) || sw_end(w) ? -1 : 0;
}

/* ============================================================ the schema */
/* A hash of the names and types of every field g_hle holds, and of the other types the state holds as raw bytes, from
   Clang's walk of each struct (hle_walk.h): a state whose types had other fields isn't read as this runtime's. The
   values the walk prints aren't part of it */
static void hash_str(u64* h, const char* s) {
    for (; *s; s++) *h = (*h ^ (u8)*s) * 1099511628211ull;
    *h = (*h ^ 0x1F) * 1099511628211ull;
}
static int schema_out(u64* h, const char* fmt, ...) {
    va_list ap;
    const char* f;
    int before = 1;
    va_start(ap, fmt);
    for (f = fmt; *f; f++) {
        int longs = 0;
        if (*f == '=') before = 0;
        if (*f != '%') {
            if (before) *h = (*h ^ (u8)*f) * 1099511628211ull;
            continue;
        }
        f++;
        while (*f == 'l' || *f == 'h' || *f == 'z' || *f == 'j' || *f == 't') longs += *f++ == 'l';
        switch (*f) {
            case 's': {
                const char* s = va_arg(ap, const char*);
                if (before && s) hash_str(h, s);
                break;
            }
            case 'p': (void)va_arg(ap, void*); break;
            case 'f':
            case 'g':
            case 'e': (void)va_arg(ap, double); break;
            case 'c':
            case 'd':
            case 'i':
            case 'u':
            case 'x':
            case 'X':
            case 'o':
                if (longs >= 2)
                    (void)va_arg(ap, long long);
                else if (longs)
                    (void)va_arg(ap, long);
                else
                    (void)va_arg(ap, int);
                break;
            case 0: f--; break;
            default: break;
        }
    }
    va_end(ap);
    return 0;
}
/* instances of the types written raw outside g_hle, for their walk */
static const GpuChannel z_chan;
static const ARState z_ar;
static const ARVoice z_voice;
static const ARChannel z_archan;
static const ARMix z_mix;
static const AREffect z_effect;
static const ARSink z_sink;
static const ARSplitter z_split;
static const ARDest z_dest;
static const GMap z_gmap;
static const GRun z_grun;
static const VaRange z_va;
static const MemBlock z_blk;
static u64 state_schema(void) {
    static u64 hash;
    u64 h = 0xCBF29CE484222325ull ^ sizeof(Hle);
    if (hash) return hash; /* the types don't change while the runtime runs */
#define SCHEMA_WALK(name, obj)                                                                                         \
    hash_str(&h, #name);                                                                                               \
    __builtin_dump_struct(&(obj), schema_out, &h);
    HLE_WALKS(SCHEMA_WALK, g_hle)
    SCHEMA_WALK(GpuChannel, z_chan)
    SCHEMA_WALK(ARState, z_ar)
    SCHEMA_WALK(ARVoice, z_voice)
    SCHEMA_WALK(ARChannel, z_archan)
    SCHEMA_WALK(ARMix, z_mix)
    SCHEMA_WALK(AREffect, z_effect)
    SCHEMA_WALK(ARSink, z_sink)
    SCHEMA_WALK(ARSplitter, z_split)
    SCHEMA_WALK(ARDest, z_dest)
    SCHEMA_WALK(GMap, z_gmap)
    SCHEMA_WALK(GRun, z_grun)
    SCHEMA_WALK(VaRange, z_va)
    SCHEMA_WALK(MemBlock, z_blk)
#undef SCHEMA_WALK
    gpu_state_schema(schema_out, &h);
    return hash = h;
}

/* ============================================================ the header */
/* the game's update and mods as its recompile stamped them (<exename>.abi beside the game module) */
static void game_stamp(char* out, size_t size) {
    char path[sizeof g_exedir + sizeof g_exename];
    snprintf(path, sizeof path, "%s%s", g_exedir, g_exename);
    snx_state_stamp(path, out, size);
}
static void header_make(SnxStateHeader* h) {
    memset(h, 0, sizeof *h);
    h->format = SNX_STATE_FORMAT;
    h->abi = SNX_ABI;
    h->title_id = g_hle.kernel.title_id;
    h->created = (u64)time(NULL);
    h->guest_ns = guest_ns();
    snprintf(h->runtime, sizeof h->runtime, "%s", snx_build_info());
    game_stamp(h->stamp, sizeof h->stamp);
    snprintf(h->game, sizeof h->game, "%s", g_gamename);
}
int state_check(const SnxStateHeader* h, char* why, size_t whysize) {
    char stamp[sizeof h->stamp];
    game_stamp(stamp, sizeof stamp);
    return snx_state_fits(h, g_hle.kernel.title_id, stamp, why, whysize);
}
void state_path(char* out, size_t size, const char* slot) {
    snprintf(out, size, "%sstates/%s.%s" SNX_STATE_EXT, g_exedir, g_exename, slot);
}

/* ============================================================ guest memory */
/* Pages go in groups of 64 behind a bitmap of those that aren't all zero: only those follow. A fresh mapping and the
   pool's fresh pages read zero, so a load writes only the others (and all of a shared memory's, whose pages are
   reused) */
#define GROUP 64
#define PAGE  0x1000ull /* the guest's page, as mem.c's */
static int page_zero(const u8* p) {
    const u64* q = (const u64*)(const void*)p;
    u32 i;
    for (i = 0; i < PAGE / 8; i++)
        if (q[i]) return 0;
    return 1;
}
static int put_pages(StateWriter* w, const u8* p, u64 size) {
    u64 at;
    for (at = 0; at < size; at += GROUP * PAGE) {
        u32 n = (size - at) / PAGE < GROUP ? (u32)((size - at) / PAGE) : GROUP, k;
        u64 bits = 0;
        for (k = 0; k < n; k++)
            if (!page_zero(p + at + (u64)k * PAGE)) bits |= 1ull << k;
        if (sw_put(w, &bits, sizeof bits)) return -1;
        for (k = 0; k < n; k++)
            if (((bits >> k) & 1) && sw_put(w, p + at + (u64)k * PAGE, PAGE)) return -1;
    }
    return 0;
}
/* a group's pages into out (GROUP * PAGE bytes), those all zero cleared or left as they are, its bitmap in *bits:
   0, or -1 on a read error */
static int get_group(StateReader* r, u8* out, u32 n, int clear, u64* bits) {
    u32 k;
    if (sr_get(r, bits, sizeof *bits)) return -1;
    for (k = 0; k < n; k++)
        if ((*bits >> k) & 1) {
            if (sr_get(r, out + (u64)k * PAGE, PAGE)) return -1;
        } else if (clear)
            memset(out + (u64)k * PAGE, 0, PAGE);
    return 0;
}

/* the shared memory a span of host view pages belongs to: its slot plus 1, 0 for none */
static u32 shmem_of(u64 off) {
    u32 i;
    for (i = 0; i < KSHMEM_MAX; i++) {
        const KShmem* s = &g_hle.svcmem.shmems[i];
        if (s->o.type == KO_SHMEM && s->pool && s->pool - 1 == off) return i + 1;
    }
    return 0;
}
/* MEMORY: the heap's and physical memory's sizes, the blocks, the pool's spans (a host view's with its shared memory),
   then each span's pages and each mapped block's that isn't pool pages or the modules' code */
static int save_memory(StateWriter* w, char* why, size_t whysize) {
    u32 nblk = mem_blocks(NULL, 0), nspan = mem_spans(NULL, 0), i;
    MemBlock* blk = (MemBlock*)malloc(sizeof *blk * (nblk + 1));
    MemSpan* span = (MemSpan*)malloc(sizeof *span * (nspan + 1));
    u8* buf = (u8*)malloc(GROUP * PAGE);
    u64 hdr[2] = {__atomic_load_n(&g_heap_size, __ATOMIC_ACQUIRE), __atomic_load_n(&g_phys_mapped, __ATOMIC_ACQUIRE)};
    int r = -1;
    if (!blk || !span || !buf) goto out;
    nblk = mem_blocks(blk, nblk);
    nspan = mem_spans(span, nspan);
    if (sw_begin(w, "MEMORY", 1) || sw_put(w, hdr, sizeof hdr) || sw_put(w, &nblk, 4) || sw_put(w, &nspan, 4) ||
        sw_put(w, blk, sizeof *blk * nblk))
        goto out;
    for (i = 0; i < nspan; i++) {
        u32 rec[2] = {(u32)span[i].host, span[i].host ? shmem_of(span[i].off) : 0};
        if (span[i].host && !rec[1]) {
            snprintf(why, whysize, "a service's shared memory has no object");
            goto out;
        }
        if (sw_put(w, &span[i].off, 8) || sw_put(w, &span[i].size, 8) || sw_put(w, rec, sizeof rec)) goto out;
    }
    for (i = 0; i < nspan; i++) {
        u64 at;
        for (at = 0; at < span[i].size; at += GROUP * PAGE) {
            u64 n = span[i].size - at < GROUP * PAGE ? span[i].size - at : GROUP * PAGE;
            if (mem_pool_read(span[i].off + at, buf, n) || put_pages(w, buf, n)) goto out;
        }
    }
    for (i = 0; i < nblk; i++)
        if (blk[i].state != MS_FREE && blk[i].state != MS_CODE && !blk[i].pool &&
            put_pages(w, GP(blk[i].addr), blk[i].size))
            goto out;
    r = sw_end(w);
out:
    if (r && !why[0]) snprintf(why, whysize, "guest memory couldn't be written");
    free(blk);
    free(span);
    free(buf);
    return r;
}

/* A load's guest memory. The state's shared memories keep their pages where the running process's are, when a live
   shared memory has the slot and size (the services make theirs at startup, the same in every run); one that has
   none gets new host view pages here, before the memory lock is taken, and is told of them after g_hle is in place.
   Every other span gets new pool pages, and the blocks' pool offsets move with them */
typedef struct {
    u8* host[KSHMEM_MAX]; /* the host view each saved shared memory's pages are at now */
    u64 off[KSHMEM_MAX];
    u8 made[KSHMEM_MAX]; /* made for the load: the running process had none */
} ShmPlan;
static void shm_plan_undo(ShmPlan* plan, const u64* size) {
    u32 i;
    for (i = 0; i < KSHMEM_MAX; i++)
        if (plan->made[i]) mem_pool_host_free(plan->host[i], plan->off[i], size[i]);
}
static int load_memory(StateReader* r, ShmPlan* plan, char* why, size_t whysize) {
    u64 hdr[2], *span_off = NULL, *new_off = NULL, *span_size = NULL, shm_size[KSHMEM_MAX];
    u32 nblk, nspan, i, *span_rec = NULL;
    MemBlock* blk = NULL;
    u8* buf = (u8*)malloc(GROUP * PAGE);
    int r_ = -1, begun = 0;
    memset(plan, 0, sizeof *plan);
    memset(shm_size, 0, sizeof shm_size);
    if (!buf || sr_begin(r, "MEMORY", NULL) || sr_get(r, hdr, sizeof hdr) || sr_get(r, &nblk, 4) ||
        sr_get(r, &nspan, 4) || nblk > (1u << 24) || nspan > (1u << 24))
        goto bad;
    blk = (MemBlock*)malloc(sizeof *blk * (nblk + 1));
    span_off = (u64*)malloc(8 * (nspan + 1));
    span_size = (u64*)malloc(8 * (nspan + 1));
    new_off = (u64*)malloc(8 * (nspan + 1));
    span_rec = (u32*)malloc(8 * (nspan + 1));
    if (!blk || !span_off || !span_size || !new_off || !span_rec || sr_get(r, blk, sizeof *blk * nblk)) goto bad;
    for (i = 0; i < nspan; i++)
        if (sr_get(r, &span_off[i], 8) || sr_get(r, &span_size[i], 8) || sr_get(r, &span_rec[2 * i], 8)) goto bad;
    /* the shared memories' pages */
    for (i = 0; i < nspan; i++) {
        u32 slot = span_rec[2 * i + 1];
        const KShmem* s;
        if (!span_rec[2 * i]) continue;
        if (!slot || slot > KSHMEM_MAX) goto bad;
        s = &g_hle.svcmem.shmems[slot - 1];
        shm_size[slot - 1] = span_size[i];
        if (s->o.type == KO_SHMEM && s->pool && ((s->size + PAGE - 1) & ~(u64)(PAGE - 1)) == span_size[i]) {
            plan->host[slot - 1] = s->host;
            plan->off[slot - 1] = s->pool - 1;
        } else {
            plan->host[slot - 1] = mem_pool_host(span_size[i], &plan->off[slot - 1]);
            plan->made[slot - 1] = 1;
            if (!plan->host[slot - 1]) {
                plan->made[slot - 1] = 0;
                snprintf(why, whysize, "there is no room for its shared memory");
                goto undo;
            }
        }
        new_off[i] = plan->off[slot - 1];
    }
    /* past here, the running game's memory is gone */
    begun = 1;
    if (mem_restore_begin()) goto fatal;
    for (i = 0; i < nspan; i++) {
        u64 at;
        int host = span_rec[2 * i] != 0;
        if (!host && (new_off[i] = mem_restore_alloc(span_size[i])) == ~0ull) goto fatal;
        for (at = 0; at < span_size[i]; at += GROUP * PAGE) {
            u32 n = (u32)((span_size[i] - at < GROUP * PAGE ? span_size[i] - at : GROUP * PAGE) / PAGE), k;
            u64 bits;
            if (get_group(r, buf, n, host, &bits)) goto fatal;
            for (k = 0; k < n; k++)
                if ((host || ((bits >> k) & 1)) &&
                    mem_restore_write(new_off[i] + at + (u64)k * PAGE, buf + (u64)k * PAGE, PAGE))
                    goto fatal;
        }
    }
    /* the blocks' pool pages where their spans went */
    for (i = 0; i < nblk; i++) {
        u64 o;
        u32 k;
        if (!blk[i].pool) continue;
        o = blk[i].pool - 1;
        for (k = 0; k < nspan && !(span_off[k] <= o && o < span_off[k] + span_size[k]); k++) {}
        if (k == nspan) goto fatal;
        blk[i].pool = new_off[k] + (o - span_off[k]) + 1;
    }
    if (mem_restore_blocks(blk, nblk)) goto fatal;
    for (i = 0; i < nblk; i++) {
        u64 at;
        if (blk[i].state == MS_FREE || blk[i].state == MS_CODE || blk[i].pool) continue;
        for (at = 0; at < blk[i].size; at += GROUP * PAGE) {
            u32 n = (u32)((blk[i].size - at < GROUP * PAGE ? blk[i].size - at : GROUP * PAGE) / PAGE);
            u64 bits;
            if (get_group(r, GP(blk[i].addr + at), n, 0, &bits)) goto fatal;
        }
    }
    mem_restore_end(hdr[0], hdr[1]);
    r_ = 0;
    goto out;
fatal:
    if (begun) fatal("The save state's guest memory couldn't be put in place, and the game can't go on");
bad:
    snprintf(why, whysize, "its guest memory is damaged");
undo:
    shm_plan_undo(plan, shm_size);
out:
    free(buf);
    free(blk);
    free(span_off);
    free(span_size);
    free(new_off);
    free(span_rec);
    return r_;
}

/* ============================================================ g_hle */
/* The names a state's g_hle holds functions and strings by, in the NAMES section: each a kind byte (NAME_FN, a
   function of state_fns.h, or NAME_STR, a string) and the name. A load reads them before anything changes, and finds
   each function in this build or refuses the state */
enum { NAME_FN = 1, NAME_STR = 2 };
typedef struct {
    char** name;
    u8* kind;
    void** ptr; /* a load's: what each names in this process */
    u32 n, cap;
    int bad; /* a save met a pointer it can't name: it is refused */
} Names;
static u32 names_add(Names* t, int kind, const char* s) {
    u32 i;
    for (i = 0; i < t->n; i++)
        if (t->kind[i] == kind && !strcmp(t->name[i], s)) return i;
    if (t->n == t->cap) {
        u32 cap = t->cap ? t->cap * 2 : 64;
        char** nn = (char**)realloc(t->name, sizeof *nn * cap);
        u8* kk;
        if (nn) t->name = nn;
        kk = nn ? (u8*)realloc(t->kind, cap) : NULL;
        if (kk) t->kind = kk;
        if (!nn || !kk) {
            t->bad = 1;
            return 0;
        }
        t->cap = cap;
    }
    if (!(t->name[t->n] = (char*)malloc(strlen(s) + 1))) {
        t->bad = 1;
        return 0;
    }
    strcpy(t->name[t->n], s);
    t->kind[t->n] = (u8)kind;
    return t->n++;
}
static void names_free(Names* t) {
    u32 i;
    for (i = 0; i < t->n; i++) free(t->name[i]);
    free(t->name);
    free(t->kind);
    free(t->ptr);
    memset(t, 0, sizeof *t);
}
static int names_write(StateWriter* w, const Names* t) {
    u32 i;
    if (sw_begin(w, "NAMES", 1) || sw_put(w, &t->n, 4)) return -1;
    for (i = 0; i < t->n; i++) {
        u32 len = (u32)strlen(t->name[i]);
        if (sw_put(w, &t->kind[i], 1) || sw_put(w, &len, 4) || sw_put(w, t->name[i], len)) return -1;
    }
    return sw_end(w);
}
/* A string kept for the process: a loaded service object's name, which IPC's crash ring may keep past the object.
   One copy of each, however many loads name it */
typedef struct Interned {
    struct Interned* next;
    char s[1];
} Interned;
static Interned* g_interned;
static const char* intern(const char* s) {
    Interned* i;
    for (i = g_interned; i; i = i->next)
        if (!strcmp(i->s, s)) return i->s;
    if (!(i = (Interned*)malloc(sizeof *i + strlen(s)))) return NULL;
    strcpy(i->s, s);
    i->next = g_interned;
    g_interned = i;
    return i->s;
}
/* NAMES read, each name found in this process: 0, or -1 with the reason when this build lacks a function */
static int names_read(StateReader* r, Names* t, char* why, size_t whysize) {
    u32 n, i;
    if (sr_begin(r, "NAMES", NULL) || sr_get(r, &n, 4) || n > 1u << 20) goto bad;
    if (!(t->name = (char**)calloc(n ? n : 1, sizeof *t->name)) || !(t->kind = (u8*)calloc(n ? n : 1, 1)) ||
        !(t->ptr = (void**)calloc(n ? n : 1, sizeof *t->ptr)))
        goto bad;
    t->n = t->cap = n;
    for (i = 0; i < n; i++) {
        u32 len;
        if (sr_get(r, &t->kind[i], 1) || sr_get(r, &len, 4) || len > 4096 ||
            !(t->name[i] = (char*)malloc((size_t)len + 1)) || sr_get(r, t->name[i], len))
            goto bad;
        t->name[i][len] = 0;
        if (t->kind[i] == NAME_FN) {
            StateFnPtr f = state_fn_find(t->name[i]);
            memcpy(&t->ptr[i], &f, sizeof f);
            if (!f) {
                snprintf(why, whysize, "it needs %s, which this ScarletNX doesn't have", t->name[i]);
                return -1;
            }
        } else if (t->kind[i] != NAME_STR || !(t->ptr[i] = (void*)(uintptr_t)intern(t->name[i])))
            goto bad;
    }
    return 0;
bad:
    snprintf(why, whysize, "it is damaged");
    return -1;
}
/* A pointer field of the saved copy holds a number, the kind in its top two bits: a pointer into g_hle as its offset
   (1), a function or a string as its index in NAMES (2 and 3) */
#define HLE_LO ((uintptr_t)&g_hle)
#define HLE_HI ((uintptr_t)&g_hle + sizeof g_hle)
static u64 swz(Names* t, const void* p) {
    uintptr_t a = (uintptr_t)p;
    if (!p) return 0;
    if (a >= HLE_LO && a < HLE_HI) return (1ull << 62) | (u64)(a - HLE_LO);
    WARN("state: a pointer of g_hle to %p, outside it, can't be saved\n", p);
    t->bad = 1;
    return 0;
}
static u64 swz_fn(Names* t, StateFnPtr f) {
    const char* name;
    if (!f) return 0;
    if (!(name = state_fn_name(f))) {
        WARN("state: the function at %p has no name in state_fns.h's tables, and can't be saved\n", (void*)(uintptr_t)f);
        t->bad = 1;
        return 0;
    }
    return (2ull << 62) | names_add(t, NAME_FN, name);
}
static u64 swz_str(Names* t, const char* s) {
    uintptr_t a = (uintptr_t)s;
    if (!s) return 0;
    if (a >= HLE_LO && a < HLE_HI) return (1ull << 62) | (u64)(a - HLE_LO); /* a stand-in's, in g_hle */
    return (3ull << 62) | names_add(t, NAME_STR, s);
}
static void* unswz(const Names* t, u64 v) {
    u64 x = v & ((1ull << 62) - 1);
    switch (v >> 62) {
        case 1: return x < sizeof g_hle ? (void*)(HLE_LO + (uintptr_t)x) : NULL;
        case 2:
        case 3: return x < t->n && t->kind[x] == (v >> 62 == 2 ? NAME_FN : NAME_STR) ? t->ptr[x] : NULL;
        default: return NULL;
    }
}
/* the field's pointer as a number, through its bytes (a function pointer converts as an object pointer does): one into
   g_hle, a function's, a string's */
#define SWZ_AS(field, how)                                                                                             \
    do {                                                                                                               \
        u64 v_ = (how);                                                                                                \
        memcpy(&(field), &v_, sizeof v_);                                                                              \
    } while (0)
#define SWZ(field)     SWZ_AS(field, swz(names, (const void*)(field)))
#define SWZ_FN(field)  SWZ_AS(field, swz_fn(names, (StateFnPtr)(field)))
#define SWZ_STR(field) SWZ_AS(field, swz_str(names, (field)))
#define UNSWZ(field)                                                                                                   \
    do {                                                                                                               \
        u64 v_;                                                                                                        \
        void* p_;                                                                                                      \
        memcpy(&v_, &(field), sizeof v_);                                                                              \
        p_ = unswz(names, v_);                                                                                         \
        memcpy(&(field), &p_, sizeof p_);                                                                              \
    } while (0)
#define NIL(field) memset(&(field), 0, sizeof(field))

/* the heap memory g_hle's members own, beside it in HEAP: records of a kind, a slot and a size */
enum { HEAP_END, HEAP_STORAGE, HEAP_NRR, HEAP_AUDREN, HEAP_GMMU, HEAP_GMAPS, HEAP_GRUNS, HEAP_VAFREE, HEAP_FS };
static int heap_put(StateIo* io, u32 kind, u32 slot, const void* p, u64 n) {
    return io_put32(io, kind) | io_put32(io, slot) | io_put64(io, n) | io_put(io, p, n);
}

/* what the state can't hold: a game with these going is refused a save, as nothing could make them again */
static int savable(char* why, size_t whysize) {
    u32 i, k;
    for (i = 0; i < BSD_MAX; i++)
        for (k = 0; k < BSD_MAX_FD; k++)
            if (g_hle.bsd.bsds[i].fd[k].s) {
                snprintf(why, whysize, "the game has a network connection open");
                return -1;
            }
    for (i = 0; i < LDN_MAX; i++)
        if (g_hle.ldn.ldn[i].lan) {
            snprintf(why, whysize, "the game is playing locally with others");
            return -1;
        }
    for (i = 0; i < SSL_CONN_MAX; i++)
        if (g_hle.ssl.conns[i].obj) {
            snprintf(why, whysize, "the game has a secure connection open");
            return -1;
        }
    for (i = 0; i < NV_FDS; i++)
        if (g_hle.nvdrv.nvf[i].h1x) {
            snprintf(why, whysize, "the game is decoding a video");
            return -1;
        }
    for (i = 0; i < RO_SESSIONS; i++)
        for (k = 0; k < RO_LIMIT; k++)
            if (g_hle.ldr.states[i].nro[k].map.base) {
                snprintf(why, whysize, "the game has loaded code modules (NROs), which states don't hold yet");
                return -1;
            }
    return 0;
}

/* HLE (g_hle, its pointers numbers) and HEAP (what its members own) into the buffers, the names its numbers stand for
   in names */
static int save_hle(Buf* hle, Buf* heap, Names* names) {
    Hle* c = (Hle*)malloc(sizeof *c);
    StateIo io = buf_io(heap);
    u32 i, k;
    int r = 0;
    if (!c) return -1;
    memcpy(c, &g_hle, sizeof *c);
    /* the host's: kthread_resume and the load's plan set them again */
    for (i = 0; i < KTHREAD_MAX; i++) {
        KThread* t = &c->thread.threads[i];
        NIL(t->cpu.mem);
        NIL(t->cpu.disp);
        NIL(t->cpu.rt);
        NIL(t->cpu.thread);
        NIL(t->arm.mem);
        NIL(t->arm.rt);
    }
    for (i = 0; i < KSHMEM_MAX; i++) NIL(c->svcmem.shmems[i].host);
    /* IPC: the objects' functions and names, their data and the sessions' and domains' objects */
    for (i = 0; i < SVCOBJ_MAX; i++) {
        SvcObj* o = &c->ipc.svcobjs[i];
        SWZ_STR(o->name);
        SWZ_FN(o->dispatch);
        SWZ_FN(o->destroy);
        SWZ(o->data);
    }
    for (i = 0; i < SESSION_MAX; i++) {
        SWZ(c->ipc.sessions[i].obj);
        SWZ(c->ipc.sessions[i].domain);
    }
    for (i = 0; i < DOMAIN_MAX; i++)
        for (k = 0; k < DOMAIN_OBJECTS; k++) SWZ(c->ipc.domains[i].obj[k]);
    /* am: the storages' bytes */
    for (i = 0; i < AM_STORAGES; i++) {
        StorageData* s = &c->am.storages[i];
        if (s->obj && s->d) r |= heap_put(&io, HEAP_STORAGE, i, s->d, s->size ? s->size : 1);
        NIL(s->d);
    }
    for (i = 0; i < AM_ACCESSORS; i++) SWZ(c->am.accessors[i].storage);
    for (i = 0; i < AM_APPLETS; i++) {
        for (k = 0; k < AM_IN_DATA; k++) SWZ(c->am.applets[i].in[k]);
        for (k = 0; k < AM_OUT_DATA; k++) SWZ(c->am.applets[i].out[k]);
    }
    for (i = 0; i < AM_USER_CHANNEL; i++) SWZ(c->am.user_channel[i]);
    /* audio: the outs' list; their host streams and the decoders are made again */
    SWZ(c->audio.outs);
    for (i = 0; i < AUDOUT_MAX; i++) {
        SWZ(c->audio.outs_pool[i].next);
        NIL(c->audio.outs_pool[i].output);
    }
    for (i = 0; i < HWOPUS_POOL; i++) {
        HwOpus* h = &c->audio.opus[i];
#ifndef SNX_NO_H264
        NIL(h->dec);
        NIL(h->pkt);
        NIL(h->frame);
#endif
        NIL(h->queue);
        NIL(h->taps);
        NIL(h->hist);
        NIL(h->lk);
    }
    /* audren: each renderer's state as its bytes */
    for (i = 0; i < SNX_AUDREN_MAX; i++) {
        ARRen* a = &c->audren.rens[i];
        if (a->o && a->s) {
            u64 n = audren_state_size(a->s);
            u8* b = (u8*)malloc((size_t)n);
            if (!b) {
                r = -1;
                break;
            }
            audren_state_pack(a->s, b);
            r |= heap_put(&io, HEAP_AUDREN, i, b, n);
            free(b);
        }
        SWZ(a->o);
        NIL(a->s);
        NIL(a->out);
        a->out_failed = a->out_started = 0;
    }
    /* ldr: the NRRs' hashes */
    for (i = 0; i < RO_SESSIONS; i++)
        for (k = 0; k < RO_LIMIT; k++) {
            RoNrr* n = &c->ldr.states[i].nrr[k];
            if (n->hashes) r |= heap_put(&io, HEAP_NRR, i * RO_LIMIT + k, n->hashes, (u64)n->count * 32);
            NIL(n->hashes);
        }
    /* nvdrv: the GPU's page table, its mappings and runs, and the VA allocator's free ranges */
    for (i = 0; i < NV_GMMU_L1; i++) {
        if (c->nvdrv.gmmu[i]) r |= heap_put(&io, HEAP_GMMU, i, c->nvdrv.gmmu[i], 4096 * 8);
        NIL(c->nvdrv.gmmu[i]);
    }
    if (c->nvdrv.gmaps) r |= heap_put(&io, HEAP_GMAPS, 0, c->nvdrv.gmaps, (u64)c->nvdrv.ngmaps * sizeof(GMap));
    if (c->nvdrv.gruns) r |= heap_put(&io, HEAP_GRUNS, 0, c->nvdrv.gruns, (u64)c->nvdrv.ngruns * sizeof(GRun));
    if (c->nvdrv.vafree) r |= heap_put(&io, HEAP_VAFREE, 0, c->nvdrv.vafree, (u64)c->nvdrv.nvafree * sizeof(VaRange));
    NIL(c->nvdrv.gmaps);
    NIL(c->nvdrv.gruns);
    NIL(c->nvdrv.vafree);
    NIL(c->nvdrv.alias);
    for (i = 0; i < NV_FDS; i++) NIL(c->nvdrv.nvf[i].h1x);
    /* fs: the open file systems, files and folders by path */
    {
        Buf paths = {0};
        StateIo pio = buf_io(&paths);
        if (fs_state_save(&pio))
            r = -1;
        else
            r |= heap_put(&io, HEAP_FS, 0, paths.p, paths.n);
        free(paths.p);
    }
    NIL(c->fs);
    memcpy(c->fs.fss, g_hle.fs.fss, sizeof c->fs.fss);
    memcpy(c->fs.files, g_hle.fs.files, sizeof c->fs.files);
    memcpy(c->fs.dirs, g_hle.fs.dirs, sizeof c->fs.dirs);
    c->fs.fs_next = g_hle.fs.fs_next;
    c->fs.file_next = g_hle.fs.file_next;
    c->fs.dir_next = g_hle.fs.dir_next;
    for (i = 0; i < FS_FS_MAX; i++) NIL(c->fs.fss[i].root);
    for (i = 0; i < FS_FILE_MAX; i++) {
        NIL(c->fs.files[i].f);
        NIL(c->fs.files[i].node);
    }
    for (i = 0; i < FS_DIR_MAX; i++) NIL(c->fs.dirs[i].node);
    /* the rest of the host's side: the running process keeps its own */
    NIL(c->ipc.reg);
    NIL(c->ipc.ring);
    NIL(c->debug.syms);
    NIL(c->am.nacp);
    NIL(c->bsd.room);
    SWZ(c->bsd.client);
    NIL(c->ssl.roots);
    NIL(c->ssl.bio);
    for (i = 0; i < SSL_CONTEXT_MAX; i++) {
        SslContext* x = &c->ssl.ctxs[i];
        NIL(x->ctx);
        NIL(x->store);
        NIL(x->client_key);
        NIL(x->client_cert);
        NIL(x->client_chain);
        NIL(x->lk);
    }
    for (i = 0; i < BSD_MAX; i++) NIL(c->bsd.bsds[i].lk);
    for (i = 0; i < LDN_MAX; i++) NIL(c->ldn.ldn[i].lk);
    r |= io_put32(&io, HEAP_END);
    r |= buf_put(hle, c, sizeof *c);
    free(c);
    return r || names->bad ? -1 : 0;
}

/* The host's side of the running game that a load lets go of before g_hle is overwritten: the services' heap memory,
   the decoders and streams, the open files. What it owned goes with the old g_hle */
static void drop_host(void) {
    u32 i, k;
    audio_state_drop();
    audren_state_drop();
    fs_state_drop();
    for (i = 0; i < AM_STORAGES; i++)
        if (g_hle.am.storages[i].obj) free(g_hle.am.storages[i].d);
    for (i = 0; i < RO_SESSIONS; i++)
        for (k = 0; k < RO_LIMIT; k++) free(g_hle.ldr.states[i].nrr[k].hashes);
    for (i = 0; i < NV_GMMU_L1; i++) free(g_hle.nvdrv.gmmu[i]);
    free(g_hle.nvdrv.gmaps);
    free(g_hle.nvdrv.gruns);
    free(g_hle.nvdrv.vafree);
    for (i = 0; i < NV_FDS; i++)
        if (g_hle.nvdrv.nvf[i].h1x) host1x_close(g_hle.nvdrv.nvf[i].h1x);
}

/* the loaded copy's heap memory from HEAP, its pointers back, and the host's fields the running process keeps: then
   it is g_hle. 0, or -1 when HEAP doesn't hold what g_hle names (fatal by then) */
static int apply_hle(Hle* ld, Buf* heap, const Names* names, const ShmPlan* plan, Buf* fs_paths) {
    StateIo io = buf_io(heap);
    u32 i, k, kind, slot;
    u64 n;
    /* the host's fields, as the running process has them */
    memcpy(ld->thread.host_cpu, g_hle.thread.host_cpu, sizeof ld->thread.host_cpu);
    ld->thread.host_ncpu = g_hle.thread.host_ncpu;
    memcpy(ld->thread.pin_said, g_hle.thread.pin_said, sizeof ld->thread.pin_said);
    ld->thread.hold = 1; /* the game is held until the load lets it go */
    memset(ld->thread.core_owner, 0, sizeof ld->thread.core_owner);
    memset(ld->thread.core_wait, 0, sizeof ld->thread.core_wait);
    for (i = 0; i < NCORES; i++) ld->thread.core_best[i] = 64;
    memcpy(ld->ipc.reg, g_hle.ipc.reg, sizeof ld->ipc.reg);
    ld->ipc.reg_next = g_hle.ipc.reg_next;
    memcpy(ld->ipc.ring, g_hle.ipc.ring, sizeof ld->ipc.ring);
    ld->ipc.ring_n = g_hle.ipc.ring_n;
    ld->debug.syms = g_hle.debug.syms;
    ld->debug.nsyms = g_hle.debug.nsyms;
    ld->am.nacp = g_hle.am.nacp;
    ld->am.nacp_size = g_hle.am.nacp_size;
    ld->am.nacp_read = g_hle.am.nacp_read;
    ld->audio.hold = g_hle.audio.hold;
    ld->audio.held = g_hle.audio.held;
    ld->bsd.room = g_hle.bsd.room;
    ld->ssl.roots = g_hle.ssl.roots;
    ld->ssl.bio = g_hle.ssl.bio;
    ld->nvdrv.alias = g_hle.nvdrv.alias;
    ld->nvdrv.alias.stale = 1;     /* built again from the state's runs */
    ld->hid.pad1 = g_hle.hid.pad1; /* the players' input as the host reads it now */
    memcpy(ld->hid.pad_extra, g_hle.hid.pad_extra, sizeof ld->hid.pad_extra);
    ld->hid.touch = g_hle.hid.touch;
    {
        HleFs fs = g_hle.fs; /* the file tree, the RomFS and the dumps, with the state's objects */
        memcpy(fs.fss, ld->fs.fss, sizeof fs.fss);
        memcpy(fs.files, ld->fs.files, sizeof fs.files);
        memcpy(fs.dirs, ld->fs.dirs, sizeof fs.dirs);
        fs.fs_next = ld->fs.fs_next;
        fs.file_next = ld->fs.file_next;
        fs.dir_next = ld->fs.dir_next;
        memset(fs.openf, 0, sizeof fs.openf);
        ld->fs = fs;
    }
    /* the pointers */
    for (i = 0; i < SVCOBJ_MAX; i++) {
        SvcObj* o = &ld->ipc.svcobjs[i];
        UNSWZ(o->name);
        UNSWZ(o->dispatch);
        UNSWZ(o->destroy);
        UNSWZ(o->data);
    }
    for (i = 0; i < SESSION_MAX; i++) {
        UNSWZ(ld->ipc.sessions[i].obj);
        UNSWZ(ld->ipc.sessions[i].domain);
    }
    for (i = 0; i < DOMAIN_MAX; i++)
        for (k = 0; k < DOMAIN_OBJECTS; k++) UNSWZ(ld->ipc.domains[i].obj[k]);
    for (i = 0; i < AM_ACCESSORS; i++) UNSWZ(ld->am.accessors[i].storage);
    for (i = 0; i < AM_APPLETS; i++) {
        for (k = 0; k < AM_IN_DATA; k++) UNSWZ(ld->am.applets[i].in[k]);
        for (k = 0; k < AM_OUT_DATA; k++) UNSWZ(ld->am.applets[i].out[k]);
    }
    for (i = 0; i < AM_USER_CHANNEL; i++) UNSWZ(ld->am.user_channel[i]);
    UNSWZ(ld->audio.outs);
    for (i = 0; i < AUDOUT_MAX; i++) UNSWZ(ld->audio.outs_pool[i].next);
    for (i = 0; i < SNX_AUDREN_MAX; i++) UNSWZ(ld->audren.rens[i].o);
    UNSWZ(ld->bsd.client);
    for (i = 0; i < KSHMEM_MAX; i++)
        if (ld->svcmem.shmems[i].o.type == KO_SHMEM) {
            ld->svcmem.shmems[i].host = plan->host[i];
            ld->svcmem.shmems[i].pool = plan->off[i] + 1;
        }
    /* the heap memory */
    for (;;) {
        u8* p;
        if (io_get32(&io, &kind)) return -1;
        if (kind == HEAP_END) break;
        if (io_get32(&io, &slot) || io_get64(&io, &n) || n > heap->n - heap->at) return -1;
        p = heap->p + heap->at;
        heap->at += n;
        switch (kind) {
            case HEAP_STORAGE:
                if (slot >= AM_STORAGES || !(ld->am.storages[slot].d = (u8*)malloc((size_t)n))) return -1;
                memcpy(ld->am.storages[slot].d, p, (size_t)n);
                break;
            case HEAP_NRR: {
                RoNrr* nrr;
                if (slot >= RO_SESSIONS * RO_LIMIT) return -1;
                nrr = &ld->ldr.states[slot / RO_LIMIT].nrr[slot % RO_LIMIT];
                if (!(nrr->hashes = (u8*)malloc((size_t)(n ? n : 1)))) return -1;
                memcpy(nrr->hashes, p, (size_t)n);
                break;
            }
            case HEAP_AUDREN:
                if (slot >= SNX_AUDREN_MAX || !(ld->audren.rens[slot].s = audren_state_unpack(p, n))) return -1;
                break;
            case HEAP_GMMU:
                if (slot >= NV_GMMU_L1 || n != 4096 * 8 || !(ld->nvdrv.gmmu[slot] = (u64*)malloc((size_t)n))) return -1;
                memcpy(ld->nvdrv.gmmu[slot], p, (size_t)n);
                break;
            case HEAP_GMAPS:
                if (n != (u64)ld->nvdrv.ngmaps * sizeof(GMap) ||
                    !(ld->nvdrv.gmaps = (GMap*)malloc((size_t)(n ? n : 1))))
                    return -1;
                memcpy(ld->nvdrv.gmaps, p, (size_t)n);
                ld->nvdrv.capgmaps = ld->nvdrv.ngmaps;
                break;
            case HEAP_GRUNS:
                if (n != (u64)ld->nvdrv.ngruns * sizeof(GRun) ||
                    !(ld->nvdrv.gruns = (GRun*)malloc((size_t)(n ? n : 1))))
                    return -1;
                memcpy(ld->nvdrv.gruns, p, (size_t)n);
                ld->nvdrv.capgruns = ld->nvdrv.ngruns;
                break;
            case HEAP_VAFREE:
                if (n != (u64)ld->nvdrv.nvafree * sizeof(VaRange) ||
                    !(ld->nvdrv.vafree = (VaRange*)malloc((size_t)(n ? n : 1))))
                    return -1;
                memcpy(ld->nvdrv.vafree, p, (size_t)n);
                ld->nvdrv.capvafree = ld->nvdrv.nvafree;
                break;
            case HEAP_FS:
                fs_paths->p = p;
                fs_paths->n = n;
                fs_paths->at = 0;
                break;
            default: return -1;
        }
    }
    if (!ld->nvdrv.gmaps) ld->nvdrv.capgmaps = ld->nvdrv.ngmaps = 0;
    if (!ld->nvdrv.gruns) ld->nvdrv.capgruns = ld->nvdrv.ngruns = 0;
    if (!ld->nvdrv.vafree) ld->nvdrv.capvafree = ld->nvdrv.nvafree = 0;
    memcpy(&g_hle, ld, sizeof g_hle);
    return 0;
}

/* ============================================================ save and load */
/* what the GPU owns of guest memory lands, range by range: own_acquire over the whole address space would let go of
   the traps on all of it, page by page, at each landing. The renderer's lock is held (g_gpu_mapping_begin) */
static void own_land_all(void) {
    OwnRange r[64];
    u32 n, i;
    while ((n = own_lookup(AS_BASE, AS_END - AS_BASE, r, 64)) != 0)
        for (i = 0; i < n && i < 64; i++) own_acquire(r[i].addr, r[i].size);
}
int state_save(const char* path, char* why, size_t whysize) {
    SnxStateHeader h;
    StateWriter* w;
    Buf hle = {0}, heap = {0}, gpu = {0}, surf = {0};
    StateIo gio = buf_io(&gpu), sio = buf_io(&surf);
    Names names = {0};
    char folder[sizeof g_exedir + 16];
    u64 t0 = plat_ns();
    u8* thumb = NULL;
    int r = -1;
    why[0] = 0;
    if (game_pause()) {
        snprintf(why, whysize, "the game didn't stop to be saved");
        return -1;
    }
    if (savable(why, whysize)) goto resume;
    /* the frame the player sees, the game held: what the menu and the launcher show of the state */
    thumb = (u8*)malloc((size_t)SNX_UI_IMAGE_W * SNX_UI_IMAGE_H * 4);
    if (thumb && (!g_renderer->thumbnail || g_renderer->thumbnail(thumb, SNX_UI_IMAGE_W, SNX_UI_IMAGE_H))) {
        free(thumb);
        thumb = NULL;
    }
    /* what the GPU holds of guest memory lands, and stays landed while the state is written: the GPU is held */
    if (g_renderer->flush_targets) g_renderer->flush_targets();
    if (g_gpu_mapping_begin) g_gpu_mapping_begin(0, 0, 1);
    own_land_all();
    header_make(&h);
    h.thumb_w = SNX_UI_IMAGE_W;
    h.thumb_h = SNX_UI_IMAGE_H;
    if (save_hle(&hle, &heap, &names) || gpu_state_save(&gio) ||
        (g_renderer->save_targets && g_renderer->save_targets(&sio))) {
        snprintf(why, whysize, "the game's state couldn't be gathered (the log says what)");
        goto end;
    }
    snprintf(folder, sizeof folder, "%sstates", g_exedir);
    plat_mkdir(folder);
    w = sw_open(path, &h, thumb, why, whysize);
    if (!w) goto end;
    if (section_write(w, "HLE", state_schema(), &hle) || names_write(w, &names) || section_write(w, "HEAP", 1, &heap) ||
        section_write(w, "GPU", 1, &gpu) || (surf.n && section_write(w, "SURF", 1, &surf)) ||
        save_memory(w, why, whysize)) {
        sw_abort(w);
        if (!why[0]) snprintf(why, whysize, "the file couldn't be written");
        goto end;
    }
    r = sw_finish(w, why, whysize);
end:
    if (g_gpu_mapping_end) g_gpu_mapping_end();
resume:
    game_resume();
    names_free(&names);
    free(thumb);
    free(hle.p);
    free(heap.p);
    free(gpu.p);
    free(surf.p);
    if (r)
        WARN("state: %s wasn't saved: %s\n", path, why);
    else
        LOG("state: saved %s at guest time %.3f s (frame %d) in %.0f ms\n", path, (double)h.guest_ns / 1e9,
            __atomic_load_n(&g_frames, __ATOMIC_RELAXED), (double)(plat_ns() - t0) / 1e6);
    return r;
}

int state_load(const char* path, char* why, size_t whysize) {
    SnxStateHeader h;
    StateReader* r;
    Buf hle = {0}, heap = {0}, gpu = {0}, fs_paths = {0}, surf = {0};
    StateIo gio = buf_io(&gpu), sio = buf_io(&surf), fio;
    Names names = {0};
    ShmPlan plan;
    u8* cur_host[KSHMEM_MAX];
    u64 cur_off[KSHMEM_MAX], cur_size[KSHMEM_MAX], schema = 0, t0 = plat_ns();
    u32 i;
    why[0] = 0;
    r = sr_open(path, &h, why, whysize);
    if (!r) return -1;
    /* everything that can refuse it before the running game is touched */
    if (state_check(&h, why, whysize)) goto refuse;
    if (section_read(r, "HLE", &schema, &hle) || hle.n != sizeof(Hle) || schema != state_schema()) {
        snprintf(why, whysize, "it holds the game's state in another layout, or is damaged");
        goto refuse;
    }
    if (names_read(r, &names, why, whysize)) goto refuse;
    if (section_read(r, "HEAP", NULL, &heap) || section_read(r, "GPU", NULL, &gpu)) {
        snprintf(why, whysize, "it is damaged");
        goto refuse;
    }
    /* the depth targets' pixels, which a state without depth targets doesn't have */
    if (section_read(r, "SURF", NULL, &surf)) {
        free(surf.p);
        memset(&surf, 0, sizeof surf);
    }
    if (game_pause()) {
        snprintf(why, whysize, "the game didn't stop to be loaded over");
        goto refuse;
    }
    /* past here, nothing of the running game is kept: a failure ends it */
    if (kernel_retire(3000)) fatal("The game's threads didn't stop for the save state to be loaded");
    for (i = 0; i < KSHMEM_MAX; i++) {
        const KShmem* s = &g_hle.svcmem.shmems[i];
        int live = s->o.type == KO_SHMEM && s->pool;
        cur_host[i] = live ? s->host : NULL;
        cur_off[i] = live ? s->pool - 1 : 0;
        cur_size[i] = live ? (s->size + PAGE - 1) & ~(PAGE - 1) : 0;
    }
    drop_host();
    if (load_memory(r, &plan, why, whysize)) fatal("The save state's guest memory couldn't be loaded: %s", why);
    if (apply_hle((Hle*)hle.p, &heap, &names, &plan, &fs_paths)) fatal("The save state's game state couldn't be loaded");
    for (i = 0; i < KSHMEM_MAX; i++) /* the running game's shared memories the state has no use for */
        if (cur_host[i] && cur_host[i] != plan.host[i]) mem_pool_host_free(cur_host[i], cur_off[i], cur_size[i]);
    fio = buf_io(&fs_paths);
    if (fs_paths.n && fs_state_reopen(&fio, why, whysize)) fatal("The save state couldn't be loaded: %s", why);
    audio_state_rebuild();
    /* the renderer forgets the old memory, and the state's channels and queue take the old ones' place */
    if (g_renderer->reset) g_renderer->reset();
    if (surf.n && g_renderer->load_targets && g_renderer->load_targets(&sio))
        WARN("state: the save state's depth targets couldn't be read: they start empty\n");
    gpu_mem_written_all();
    if (g_renderer->mem_may_have_changed) g_renderer->mem_may_have_changed();
    if (gpu_state_load(&gio)) fatal("The save state's GPU state couldn't be loaded");
    clock_set(h.guest_ns);
    for (i = 0; i < KTHREAD_MAX; i++) {
        KThread* t = &g_hle.thread.threads[i];
        if (t->o.type == KO_THREAD && t->state == 1) kthread_resume(t);
    }
    game_resume();
    sr_close(r);
    names_free(&names);
    free(hle.p);
    free(heap.p);
    free(gpu.p);
    free(surf.p);
    LOG("state: loaded %s (guest time %.3f s) at frame %d in %.0f ms\n", path, (double)h.guest_ns / 1e9,
        __atomic_load_n(&g_frames, __ATOMIC_RELAXED), (double)(plat_ns() - t0) / 1e6);
    return 0;
refuse:
    sr_close(r);
    names_free(&names);
    free(hle.p);
    free(heap.p);
    free(gpu.p);
    free(surf.p);
    WARN("state: %s wasn't loaded: %s\n", path, why);
    return -1;
}

/* ============================================================ unattended runs */
/* the setting's entry i as at:slot, 0 when there are fewer */
static int row_get(const char* p, int i, u64* at, char* slot, size_t size) {
    for (;;) {
        char* e;
        unsigned long long a = strtoull(p, &e, 10);
        size_t n;
        if (e == p || *e != ':') return 0;
        p = e + 1;
        n = strcspn(p, ",");
        if (!i--) {
            *at = a;
            snprintf(slot, size, "%.*s", (int)(n < size ? n : size - 1), p);
            return n > 0;
        }
        if (!p[n]) return 0;
        p += n + 1;
    }
}
/* a slot's file, or the file itself when the row names one (the launcher's Play from a state) */
static void row_path(char* out, size_t size, const char* slot) {
    if (strchr(slot, '/') || strchr(slot, '\\'))
        snprintf(out, size, "%s", slot);
    else
        state_path(out, size, slot);
}
static void state_unattended(void) {
    static int saves, loads;
    char slot[512], path[sizeof g_exedir + sizeof g_exename + 512], why[256];
    u64 at, ms = unattended_ms();
    if (row_get(g_settings.state_saves, saves, &at, slot, sizeof slot) && ms >= at) {
        saves++;
        row_path(path, sizeof path, slot);
        state_save(path, why, sizeof why);
    }
    if (row_get(g_settings.state_loads, loads, &at, slot, sizeof slot) && ms >= at) {
        loads++;
        row_path(path, sizeof path, slot);
        state_load(path, why, sizeof why);
    }
}

/* ============================================================ the player's: hotkeys and the menu */
static int g_slot = 1;        /* the slot F7 and F8 use, 1 to state_slots */
static int g_ask, g_ask_slot; /* a save, load or delete asked for, done at the next pass */
static int g_menu_pause;      /* the save states' pages hold the game: 1 paused, -1 it didn't stop */

static void slot_name(char* out, size_t size, int slot) {
    if (slot)
        snprintf(out, size, "%d", slot);
    else
        snprintf(out, size, "auto");
}
/* a slot as a sentence names it, at its start (Slot 2) or within it (slot 2) */
static void slot_label(char* out, size_t size, int slot, int start) {
    if (slot)
        snprintf(out, size, "%s %d", start ? "Slot" : "slot", slot);
    else
        snprintf(out, size, "%s auto slot", start ? "The" : "the");
}
static void toast(const char* fmt, ...) {
    char text[OVERLAY_COLS_MAX + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    ui_lock();
    overlay_toast(text, plat_ns());
    ui_unlock();
}
/* a state's date and the play time then, as the menu lists them */
static void slot_when(char* out, size_t size, const SnxStateHeader* h) {
    time_t t = (time_t)h->created;
    struct tm tm;
    u64 s = h->guest_ns / 1000000000u;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    snprintf(out, size, "%04d-%02d-%02d %02d:%02d  %llu:%02u:%02u", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, (unsigned long long)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
}
/* the slots the menu lists, read from their files' headers and thumbnails */
static void slots_list(void) {
    OverlaySlot list[OVERLAY_SLOTS_MAX];
    int n = g_settings.save_state_slots + 1, i;
    if (n > OVERLAY_SLOTS_MAX) n = OVERLAY_SLOTS_MAX;
    memset(list, 0, sizeof list);
    for (i = 0; i < n; i++) {
        char name[16], path[sizeof g_exedir + sizeof g_exename + 64], why[256];
        SnxStateHeader h;
        u8* thumb = NULL;
        slot_name(name, sizeof name, i);
        state_path(path, sizeof path, name);
        if (snx_state_peek(path, &h, &thumb, why, sizeof why)) continue;
        slot_when(list[i].when, sizeof list[i].when, &h);
        if (state_check(&h, why, sizeof why)) snprintf(list[i].why, sizeof list[i].why, "It can't load: %s.", why);
        if (thumb && h.thumb_w == SNX_UI_IMAGE_W && h.thumb_h == SNX_UI_IMAGE_H)
            list[i].thumb = thumb;
        else
            free(thumb);
    }
    ui_lock();
    overlay_slots_set(list, n);
    ui_unlock();
}
void state_ask(int op, int slot) {
    g_ask = op;
    g_ask_slot = slot < 0 ? g_slot : slot;
}
void state_slot_next(void) {
    char name[16], path[sizeof g_exedir + sizeof g_exename + 64], why[256], when[32];
    SnxStateHeader h;
    g_slot = g_slot % g_settings.save_state_slots + 1;
    slot_name(name, sizeof name, g_slot);
    state_path(path, sizeof path, name);
    if (snx_state_peek(path, &h, NULL, why, sizeof why))
        toast("Slot %d: empty", g_slot);
    else {
        slot_when(when, sizeof when, &h);
        toast("Slot %d: %s", g_slot, when);
    }
}
/* what was asked, done, and said */
static void ask_do(int op, int slot) {
    char name[16], label[24], start[24], path[sizeof g_exedir + sizeof g_exename + 64], why[256];
    SnxStateHeader h;
    slot_name(name, sizeof name, slot);
    slot_label(label, sizeof label, slot, 0);
    slot_label(start, sizeof start, slot, 1);
    state_path(path, sizeof path, name);
    if (op == STATE_SAVE) {
        if (!slot) return; /* only a load saves there */
        if (state_save(path, why, sizeof why))
            toast("Not saved: %s", why);
        else
            toast("Saved to %s", label);
        return;
    }
    if (snx_state_peek(path, &h, NULL, why, sizeof why)) {
        toast("%s is empty", start);
        return;
    }
    if (op == STATE_DELETE) {
        if (remove(path))
            toast("%s couldn't be deleted", start);
        else {
            LOG("state: deleted %s\n", path);
            toast("Deleted %s", label);
        }
        return;
    }
    if (state_check(&h, why, sizeof why)) {
        toast("Not loaded: %s", why);
        WARN("state: %s wasn't loaded: %s\n", path, why);
        return;
    }
    /* the game as it was, kept to go back to: a load of the auto slot itself keeps that one */
    if (g_settings.save_state_backup && slot) {
        char back[sizeof path], bwhy[256];
        state_path(back, sizeof back, "auto");
        if (state_save(back, bwhy, sizeof bwhy)) WARN("state: no auto slot before the load: %s\n", bwhy);
    }
    if (state_load(path, why, sizeof why))
        toast("Not loaded: %s", why);
    else
        toast("Loaded %s", label);
}
void state_poll(void) {
    int want = overlay_states_shown(), op = g_ask;
    state_unattended();
    /* the save states' pages hold the game while they show: their thumbnails are what the player sees */
    if (want && !g_menu_pause)
        g_menu_pause = game_pause() ? -1 : 1;
    else if (!want && g_menu_pause) {
        if (g_menu_pause > 0) game_resume();
        g_menu_pause = 0;
    }
    if (op) {
        g_ask = 0;
        ask_do(op, g_ask_slot);
    }
    if (want && (op || overlay_states_stale())) slots_list();
}
