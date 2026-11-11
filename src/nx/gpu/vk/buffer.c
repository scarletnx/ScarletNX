/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Guest memory ranges as buffers the GPU reads: vertices, indices and storage buffers. A range is checked once per
   guest submit (and again after the GPU front end wrote it). Data that changed is copied into the frame's staging
   ring, which draws read directly; data that stayed the same over a few checks gets a buffer of its own, which a
   change replaces, so nothing waits for the GPU. A big range's own buffer instead takes the pages written since it
   last matched memory, copied on the GPU in order with the draws (patch_own). Big ranges that have been stable for
   long are checked less often. */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/memory/own.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/tracy.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/resource_stats.h"
#include "nx/gpu/vk/descriptors.h"
#include "nx/gpu/vk/pending_pages.h"
#include "nx/runtime/platform/tracy.h"

typedef struct {
    u64 start, len, hash, shash;
    u32 epoch, strict_epoch, chk_epoch, schk, stable, used;
    u64 gw;    /* GPU-side write generation at the last check */
    u64 cs_adds, gfx_adds; /* vk_compute_adds and vk_buf_adds when pending results were last checked for */
    u64 frame; /* the staging frame buf/off point into, when not own */
    int own;   /* buf is this entry's own buffer */
    u32 wseen; /* write-tracking clock when it last matched memory */
    u32 host_gen; /* g_vb_map_gen when host_cpu was found: 0 never */
    u64 host_cpu; /* the guest memory the range shows, ~0 for none (entry_host) */
    VkBuffer buf;
    VmaAllocation mem;
    u64 off;
    u32 own_prev, own_next; /* own buffers, from the least recently used to the most (vk_buf_reclaim); a free entry's
                               next free one in own_next */
} Vb;
/* The cache of guest ranges as buffers: its entries in one array by index (0 is none), and an index of them by range,
   open addressed. Entries live twenty seconds after their last use, and draws make new ranges all the time (constant
   buffers streamed through a ring, vertices at new offsets): chained buckets of separately allocated entries grew
   long, and walking them, a cache miss an entry, was a twentieth of the GPU thread in one game. A lookup reads one
   slot of the index, rarely two */
static Vb* g_vb;
static u32 g_nvb, g_capvb, g_vbfree, g_vblive;
typedef struct {
    u64 start;
    u32 len, idx; /* idx 0: an empty slot */
} VbSlot;
static VbSlot* g_vbi;
static u32 g_vbimask; /* the index's size less one, a power of two; at most half its slots are used */
static u64 g_vb_lookups, g_vb_probes;
static int copy_stats_on(void);
static u32 vb_home(u64 start, u32 len) {
    /* every bit of the range, mixed by multiplying: constant buffers are 256-byte aligned and buffers often 4K */
    return (u32)((((start ^ ((u64)len << 40)) * 0x9E3779B97F4A7C15ull) >> 32) & g_vbimask);
}
/* the slot holding the range, or the empty one where it would go */
static u32 vb_slot(u64 start, u32 len) {
    u32 i = vb_home(start, len);
    if (copy_stats_on()) g_vb_lookups++;
    while (g_vbi[i].idx && (g_vbi[i].start != start || g_vbi[i].len != len)) {
        i = (i + 1) & g_vbimask;
        if (copy_stats_on()) g_vb_probes++;
    }
    return i;
}
/* room in the index for one more range: 0 when none could be made */
static int vb_index_room(void) {
    u32 cap = g_vbi ? g_vbimask + 1 : 0, ncap = cap ? cap * 2 : 1u << 16;
    VbSlot *old = g_vbi, *n;
    if (cap && (g_vblive + 1) * 2 <= cap) return 1;
    n = calloc(ncap, sizeof *n);
    if (!n) return cap && g_vblive + 1 < cap; /* fuller than meant, but still a free slot */
    g_vbi = n;
    g_vbimask = ncap - 1;
    for (u32 i = 0; i < cap; i++)
        if (old[i].idx) g_vbi[vb_slot(old[i].start, old[i].len)] = old[i];
    free(old);
    return 1;
}
/* the range's slot emptied, the entries after it in its run moved back over it (no tombstones) */
static void vb_index_remove(u64 start, u32 len) {
    u32 i = vb_slot(start, len), j = i;
    if (!g_vbi[i].idx) return;
    for (;;) {
        g_vbi[i].idx = 0;
        for (;;) {
            j = (j + 1) & g_vbimask;
            if (!g_vbi[j].idx) return;
            u32 h = vb_home(g_vbi[j].start, g_vbi[j].len);
            /* j's entry can move to i when its home isn't in (i, j], cyclically */
            if (i <= j ? (h <= i || h > j) : (h <= i && h > j)) break;
        }
        g_vbi[i] = g_vbi[j];
        i = j;
    }
}
/* a new entry for the range, indexed: its index, 0 out of memory */
static u32 vb_new(u64 start, u32 len) {
    u32 idx;
    if (!vb_index_room()) return 0;
    if (g_vbfree) {
        idx = g_vbfree;
        g_vbfree = g_vb[idx].own_next;
    } else {
        if (g_nvb + 1 >= g_capvb) {
            u32 cap = g_capvb ? g_capvb * 2 : 4096;
            Vb* n = realloc(g_vb, (size_t)cap * sizeof *n);
            if (!n) return 0;
            g_vb = n;
            g_capvb = cap;
            if (!g_nvb) g_nvb = 1; /* entry 0 is none */
        }
        idx = g_nvb++;
    }
    memset(&g_vb[idx], 0, sizeof g_vb[idx]);
    g_vb[idx].start = start;
    g_vb[idx].len = len;
    g_vbi[vb_slot(start, len)] = (VbSlot){start, len, idx};
    g_vblive++;
    return idx;
}
static void vb_free(u32 idx) {
    vb_index_remove(g_vb[idx].start, (u32)g_vb[idx].len);
    g_vb[idx].len = 0; /* free: the sweep skips it */
    g_vb[idx].own_next = g_vbfree;
    g_vbfree = idx;
    g_vblive--;
}
#define PATCH_RUNS0 256 /* the runs a patch holds before it needs memory of its own (patch_note) */
static struct {
    u32 head, tail;
    u32 n;
    u64 bytes;
    u64 copy_src_read; /* vk_buf_copy_src_read */
    u64 ahead; /* fills recorded ahead of the submission's rendering passes (vk_cmd_upload) */
    /* Pending buffer writes, and the work covered by the last consumer barrier. Read-only draws do not
       advance writes. A writable reuse still needs ordering against intervening readers. */
    struct {
        u64 writes, visible, submit, work;
        u64 barriers, read_reuses, counted_work, explicit_barriers, parallel_writes;
        /* owners bound where they are for reading (buf_get_owned, VkOwnRes.read), and how many of those reads the last
           barrier from all work had behind it: a draw writing an owner in place (keep_new) waits for its reads */
        u64 owner_reads, owner_reads_ordered;
        int reused_write, read_pending, required;
    } order;
    /* What writable bindings cost since the start (vk_buf_writable_stats), and up to the last report: guest bytes
       copied for them before their draws (their original bytes and their output's fill), their results' bytes copied
       back from the GPU to land, and the results landed by comparing them with their original bytes; and the bindings
       kept on the GPU (keep_new) */
    struct {
        u64 copied, read_back, merged, kept;
    } wstat, wstat_seen;
    /* the runs of the patch being made (patch_note): in r0, or in more (cap of them) once they didn't fit */
    struct {
        VkBufferCopy r0[PATCH_RUNS0];
        VkBufferCopy* more;
        u32 cap;
    } patch;
} g_buffer;
/* why keep_new turned a writable binding away (temporary diagnosis of the writeback cost) */
enum { KF_NONE, KF_TRAPS_OFF, KF_UMA, KF_EMPTY, KF_UNALIGNED, KF_NOT_CONTIG, KF_CPU_READ, KF_N };
static u64 g_keep_fail[KF_N];
static void own_link(u32 i) {
    Vb* e = &g_vb[i];
    e->own_next = 0;
    e->own_prev = g_buffer.tail;
    if (e->own_prev)
        g_vb[e->own_prev].own_next = i;
    else
        g_buffer.head = i;
    g_buffer.tail = i;
}
static void own_unlink(u32 i) {
    Vb* e = &g_vb[i];
    if (e->own_prev)
        g_vb[e->own_prev].own_next = e->own_next;
    else
        g_buffer.head = e->own_next;
    if (e->own_next)
        g_vb[e->own_next].own_prev = e->own_prev;
    else
        g_buffer.tail = e->own_prev;
}
/* Optional copy telemetry. Counters are updated under vk_lock. */
static u64 g_copy_stage, g_copy_owned, g_copy_uncached, g_copy_zero, g_copy_calls, g_copy_peak, g_read_bound;
static u64 g_owned_bound; /* bytes draws read where their owners hold them (buf_get_owned) */
static u64 g_wb_outputs, g_wb_bytes, g_wb_waits, g_wb_wait_ns, g_wb_reuses;
static u64 g_alias_draws, g_alias_pages;
enum { WB_DRAW, WB_READ, WB_WRITE, WB_SIGNAL, WB_SYNCWAIT, WB_MAP, WB_REASON_N };
static u64 g_wb_reason[WB_REASON_N], g_wb_reason_ns[WB_REASON_N], g_wb_draw_miss[4];
static int copy_stats_on(void) { return g_settings.buffer_copy_stats; }
/* guest memory checked by its contents (vk_buf_checks): compared with a pending result's original bytes, hashed whole,
   and sampled */
static struct {
    u64 compares, hashes, samples;
} g_checks;
void vk_buf_checks(u64* compares, u64* hashes, u64* samples) {
    *compares = g_checks.compares;
    *hashes = g_checks.hashes;
    *samples = g_checks.samples;
}
static u64 buf_sample_hash(u64 gva, u64 len) {
    g_checks.samples++;
    return vk_sample_hash(gva, len);
}
/* Why buf_get copied a range into staging: no entry yet, its copy gone, a newer submit than its copy's, a GPU write
   over it, a newer guest submit with nothing written, or (with a buffer of its own) a write the tracking saw. Each by
   whether the bytes had changed, with the copies' sizes and the ranges that copied most */
enum { ST_NEW, ST_NOBUF, ST_SUBMIT, ST_DIRTY, ST_EPOCH, ST_WRITTEN, ST_N };
static u64 g_st_bytes[ST_N][2], g_st_n[ST_N][2], g_st_size[4], g_st_submit0;
static struct {
    u64 gva, bytes, n;
    u32 len;
} g_st_top[64];
static void stage_note(int why, int changed, u64 gva, u64 len) {
    g_st_bytes[why][changed] += len;
    g_st_n[why][changed]++;
    g_st_size[len < 4096 ? 0 : len < (64u << 10) ? 1 : len < (1u << 20) ? 2 : 3] += len;
    u32 h = (u32)((gva * 0x9E3779B97F4A7C15ull) >> 58);
    for (u32 k = 0; k < 64; k++) {
        u32 s = (h + k) & 63;
        if (g_st_top[s].gva && g_st_top[s].gva != gva) continue;
        g_st_top[s].gva = gva;
        g_st_top[s].bytes += len;
        g_st_top[s].n++;
        if (len > g_st_top[s].len) g_st_top[s].len = (u32)len;
        return;
    }
}
static void stage_report(void) {
    static const char* const names[ST_N] = {"new", "no_copy", "submit", "gpu_write", "epoch", "written"};
    char line[1024];
    int at = 0;
    for (int w = 0; w < ST_N; w++)
        at += snprintf(line + at, sizeof line - (size_t)at, " %s=%llu/%lluMB same %llu/%lluMB changed", names[w],
                       (unsigned long long)g_st_n[w][0], (unsigned long long)(g_st_bytes[w][0] >> 20),
                       (unsigned long long)g_st_n[w][1], (unsigned long long)(g_st_bytes[w][1] >> 20));
    LOG("vulkan: staged by reason:%s; by size <4K %lluMB <64K %lluMB <1M %lluMB >=1M %lluMB; %llu submits\n", line,
        (unsigned long long)(g_st_size[0] >> 20), (unsigned long long)(g_st_size[1] >> 20),
        (unsigned long long)(g_st_size[2] >> 20), (unsigned long long)(g_st_size[3] >> 20),
        (unsigned long long)(vk_submit_number() - g_st_submit0));
    at = 0;
    for (int t = 0; t < 6; t++) {
        int best = -1;
        for (int s = 0; s < 64; s++)
            if (g_st_top[s].bytes && (best < 0 || g_st_top[s].bytes > g_st_top[best].bytes)) best = s;
        if (best < 0) break;
        at += snprintf(line + at, sizeof line - (size_t)at, " %llx+%x:%llux/%lluMB", (unsigned long long)g_st_top[best].gva,
                       g_st_top[best].len, (unsigned long long)g_st_top[best].n,
                       (unsigned long long)(g_st_top[best].bytes >> 20));
        g_st_top[best].bytes = 0;
    }
    LOG("vulkan: staged most (address+largest length:copies/bytes):%s\n", line);
    memset(g_st_bytes, 0, sizeof g_st_bytes);
    memset(g_st_n, 0, sizeof g_st_n);
    memset(g_st_size, 0, sizeof g_st_size);
    memset(g_st_top, 0, sizeof g_st_top);
    g_st_submit0 = vk_submit_number();
}
static void copy_note(u64 gva, u64 len) {
    if (!copy_stats_on()) return;
    g_copy_uncached += len;
    g_copy_calls++;
    if (len > g_copy_peak) g_copy_peak = len;
    if (!gva) g_copy_zero += len;
}

typedef struct GfxOut {
    u64 addr, frame, frame_id; /* the submit that recorded it, and its frame (whose staging holds buf) */
    u64 written; /* buffer-write generation, including an in-place reuse prepared for the next recorded work */
    u32 size;
    VkBuffer buf;
    u64 off;
    VmaAllocation mem;
    u8* ptr;
    /* the bytes it started from, which landing compares it with; NULL for a logged result, whose writers' write logs
       give the bytes they wrote (logs) */
    u8* org;
    int org_alloc; /* org allocated on its own: taken when a writer that logs nothing joined a logged result */
    /* the writer binds a buffer the program writes but never reads: its input was not copied in, so its staging's
       bytes are not guest memory and a reader must compose, not read it in place */
    int no_input;
    /* GPU-owned input snapshotted before shader writes, read only at the frame fence. */
    u8* initial;
    VmaAllocation initial_mem;
    u64* pages; /* sorted guest CPU pages behind this GPU virtual range */
    u64* vmap;  /* the same pages in the range's order, ~0 for an unmapped one */
    u64 cpu;    /* the guest CPU address of addr when vmap is one run of guest memory, ~0 otherwise */
    u32 npages;
    /* earlier pending results were copied into buf on the GPU when it was made (gfx_overlay, atlas_fill): org holds
       guest memory without them until each lands (out_rebase) */
    int composed;
    u32 wseen; /* the write tracking clock before its input was read (vk_ww_now): copyable_span skips comparing pages since */
    /* of a logged result, its writers' logs, each holding a reference: part of log gives the bytes that writer wrote
       of [at, at + len) */
    struct OutLog {
        VkWriteLog* log;
        u32 part, at, len;
    } * logs;
    u32 nlogs, caplogs;
    /* kept on the GPU (keep_new): the owner of the range's bytes, which its draws write in place, with a reference;
       the range from keep_off on in it. It has no original bytes and lands nothing: the trap lands the owner */
    VkOwnRes* keep;
    u64 keep_off;
    u64 seq;   /* its place in g_out's order: higher is later (g_outx.seq when it went to the end) */
    struct GfxOut* next;
    /* being set up to be written back on the GPU once its dispatch is recorded (back_record), never pending */
    int back;
} GfxOut;
static GfxOut *g_prep, *g_prep_tail, *g_out, *g_out_tail;
/* The writers being bound that log their writes (vk_buf_copy_writable_logged): each result they write gets a
   reference to their log at vk_buf_writable_commit, once their work is recorded, and none when it is abandoned. The
   GPU thread's */
static struct {
    GfxOut* o;
    VkWriteLog* log;
    u32 part, at, len;
} g_pw[32];
static u32 g_npw;
static VkWriteLog* g_bind_log; /* the log of the writer vk_buf_copy_writable is binding, NULL when it logs nothing */
static int g_bind_no_read; /* the writer binds a buffer the program writes but never reads: its input is not copied */
static u32 g_bind_part;
/* the dispatches' own references to their logs, dropped once the submit that recorded them is done */
static struct {
    VkWriteLog* log;
    u64 submit;
}* g_retire;
static u32 g_nretire, g_capretire;
/* gfx_gpu_src's result taken in place for writing, when it answers 1 */
static GfxOut* g_src_match;
/* The results in g_out by the pages they cover, so a binding or a read compares (out_meets) the results over its range
   alone, not each of the hundreds a frame keeps pending until it is done. Each run of a result's guest pages [lo, hi]
   is an entry, in order of lo, as surface.c keeps the live render targets by address (g_rts); a search starts as far
   back as the longest run there is, known from how many runs have each bit length, so it narrows again once a long
   result lands. A range also meets a result at the same GPU address where both have no guest page (out_meets): a
   result with an unmapped page has its GPU pages in a second index. Under vk_lock, as the lists */
typedef struct {
    u64 lo, hi;
    GfxOut* o;
} OutRun;
typedef struct {
    OutRun* v;
    u32 n, cap;
    u32 lens[65]; /* the entries whose page count is of each bit length */
} OutIndex;
static struct {
    OutIndex guest, gpu;
    u64 seq;       /* the last seq given */
    u64 inspected; /* results that searches gave their callers to compare (vk_buf_pending_inspected) */
} g_outx;
/* the guest pages the results in g_out cover, so a read or write that meets none (almost all) skips the walk over
   them: each adds its own translated pages when it is committed and removes them when it lands. A range that meets a
   result only at the same GPU address through an unmapped page skips it too: nothing is read or written there. A GMMU
   change (WB_MAP) still checks every result, so one over a page about to be mapped lands first */
static PendPages g_out_pages = {.also = &g_pend_any};
static void out_pages(const GfxOut* o, int delta) {
    for (u32 i = 0, j; i < o->npages && o->pages[i] != ~0ull; i = j) { /* in runs of consecutive pages, sorted */
        for (j = i + 1; j < o->npages && o->pages[j] == o->pages[j - 1] + 1; j++) continue;
        pend_add_pages(&g_out_pages, o->pages[i], o->pages[j - 1], delta);
    }
}
static int bit_length(u64 x) {
    int b = 0;
    for (; x; x >>= 1) b++;
    return b;
}
/* the first entry of t whose run starts at page lo or past it */
static u32 outx_lower(const OutIndex* t, u64 lo) {
    u32 a = 0, b = t->n;
    while (a < b) {
        u32 m = a + (b - a) / 2;
        if (t->v[m].lo < lo)
            a = m + 1;
        else
            b = m;
    }
    return a;
}
static void outx_add(OutIndex* t, u64 lo, u64 hi, GfxOut* o) {
    if (t->n == t->cap) {
        u32 cap = t->cap ? t->cap * 2 : 256;
        OutRun* v = realloc(t->v, (size_t)cap * sizeof *v);
        if (!v) fatal("Out of memory indexing %u pending storage results", t->n + 1);
        t->v = v;
        t->cap = cap;
    }
    u32 at = outx_lower(t, lo);
    memmove(&t->v[at + 1], &t->v[at], (size_t)(t->n - at) * sizeof *t->v);
    t->v[at] = (OutRun){lo, hi, o};
    t->n++;
    t->lens[bit_length(hi - lo + 1)]++;
}
static void outx_del(OutIndex* t, u64 lo, u64 hi, const GfxOut* o) {
    for (u32 at = outx_lower(t, lo); at < t->n && t->v[at].lo == lo; at++)
        if (t->v[at].o == o && t->v[at].hi == hi) {
            memmove(&t->v[at], &t->v[at + 1], (size_t)(t->n - at - 1) * sizeof *t->v);
            t->n--;
            t->lens[bit_length(hi - lo + 1)]--;
            return;
        }
    fatal("The pending storage result at GPU address %llx is not indexed at pages %llx to %llx",
          (unsigned long long)o->addr, (unsigned long long)lo, (unsigned long long)hi);
}
/* the end of the run of o's guest pages that starts at its sorted page i: a page o has twice (two of its GPU pages over
   it) stays in the run */
static u32 out_run_end(const GfxOut* o, u32 i) {
    u32 j = i + 1;
    while (j < o->npages && o->pages[j] != ~0ull && o->pages[j] <= o->pages[j - 1] + 1) j++;
    return j;
}
/* o in the indexes (delta 1) or out of them (-1), with the pages it has now: committed, and landed */
static void outx_result(GfxOut* o, int delta) {
    u32 i = 0, j;
    for (; i < o->npages && o->pages[i] != ~0ull; i = j) {
        j = out_run_end(o, i);
        if (delta > 0)
            outx_add(&g_outx.guest, o->pages[i], o->pages[j - 1], o);
        else
            outx_del(&g_outx.guest, o->pages[i], o->pages[j - 1], o);
    }
    if (i < o->npages && o->size) { /* an unmapped page (they sort last) */
        u64 lo = o->addr >> 12, hi = (o->addr + o->size - 1) >> 12;
        if (delta > 0)
            outx_add(&g_outx.gpu, lo, hi, o);
        else
            outx_del(&g_outx.gpu, lo, hi, o);
    }
}
/* Results a search found: each once, in g_out's order, in local while it holds them */
typedef struct {
    GfxOut** v;
    u32 n, cap;
    GfxOut* local[64];
} OutList;
static void out_list_init(OutList* l) {
    l->v = l->local;
    l->n = 0;
    l->cap = sizeof l->local / sizeof *l->local;
}
static void out_list_free(OutList* l) {
    if (l->v != l->local) free(l->v);
}
static void out_list_push(OutList* l, GfxOut* o) {
    if (l->n == l->cap) {
        u32 cap = l->cap * 2;
        GfxOut** v = l->v == l->local ? malloc((size_t)cap * sizeof *v) : realloc(l->v, (size_t)cap * sizeof *v);
        if (!v) fatal("Out of memory listing %u pending storage results", cap);
        if (l->v == l->local) memcpy(v, l->local, sizeof l->local);
        l->v = v;
        l->cap = cap;
    }
    l->v[l->n++] = o;
}
/* every result with an entry of t that meets pages [lo, hi], into l */
static void outx_find(OutList* l, const OutIndex* t, u64 lo, u64 hi) {
    int top = 64;
    while (top && !t->lens[top]) top--;
    if (!top) return;
    u64 reach = top == 64 ? ~0ull : (1ull << top) - 1; /* no run is longer */
    for (u32 at = outx_lower(t, lo > reach ? lo - reach : 0); at < t->n && t->v[at].lo <= hi; at++)
        if (t->v[at].hi >= lo) out_list_push(l, t->v[at].o);
}
static int out_seq_cmp(const void* a, const void* b) {
    u64 x = (*(GfxOut* const*)a)->seq, y = (*(GfxOut* const*)b)->seq;
    return (x > y) - (x < y);
}
/* l in g_out's order, each result once (a result's runs can each have met the pages asked about) */
static void out_list_sort(OutList* l) {
    u32 n = 0;
    if (l->n > 1) qsort(l->v, l->n, sizeof *l->v, out_seq_cmp);
    for (u32 i = 0; i < l->n; i++)
        if (!n || l->v[n - 1] != l->v[i]) l->v[n++] = l->v[i];
    l->n = n;
    g_outx.inspected += n;
}
u64 vk_buf_pending_inspected(void) { return g_outx.inspected; }
/* where and how pending results were written: shader stores (an Ounce's task and mesh programs' too), copies, and
   transform feedback's captures when the device has it (its flags are invalid without the extension) */
static VkPipelineStageFlags2 out_stages(void) {
    return VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT |
           (g_shader_tessellation ? VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT |
                                        VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT
                                  : 0) |
           (g_shader_geometry ? VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT : 0) |
           (g_vk.has_mesh ? VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT : 0) |
           (g_vk.has_task ? VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT : 0) |
           (g_vk.has_xfb ? VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT : 0);
}
static VkAccessFlags2 out_access(void) {
    return VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
           (g_vk.has_xfb ? VK_ACCESS_2_TRANSFORM_FEEDBACK_WRITE_BIT_EXT : 0);
}
/* an output with its page lists and its original bytes after it, in one block */
static GfxOut* gfx_out_new(u32 npages, u32 len) {
    /* not calloc: its caller fills the original bytes and out_map the page lists, and zeroing them first was a tenth
       of one game's GPU thread's copying */
    GfxOut* o = malloc(sizeof *o + 2 * (size_t)npages * sizeof *o->pages + len);
    if (!o) return NULL;
    *o = (GfxOut){0};
    o->pages = (u64*)(o + 1);
    o->vmap = o->pages + npages;
    o->npages = npages;
    o->org = (u8*)(o->vmap + npages);
    o->cpu = ~0ull;
    o->wseen = vk_ww_now(); /* before its caller reads org: a write in between only makes copyable_span compare */
    return o;
}
static int page_cmp(const void* a, const void* b);
/* o's page lists for its range at gva, as the GMMU maps it now */
static void out_map(GfxOut* o, u64 gva) {
    int run = 1;
    for (u32 i = 0; i < o->npages; i++) {
        u64 cpu = gmmu_translate((gva & ~0xFFFull) + (u64)i * 0x1000);
        o->vmap[i] = o->pages[i] = cpu ? cpu >> 12 : ~0ull;
        run &= cpu && o->vmap[i] == o->vmap[0] + i;
    }
    o->cpu = run && o->npages ? o->vmap[0] << 12 | (gva & 0xFFF) : ~0ull;
    qsort(o->pages, o->npages, sizeof *o->pages, page_cmp);
}
/* The bytes of [gva, gva + len) that are bytes of o: the same guest memory, through the same GPU address or another
   one mapping the same pages (an unmapped page only through the same address). rmap is the range's pages
   (range_map). fn(user, at, from, n) gets each run: n bytes at gva + at that are o's bytes at o->addr + from */
typedef void (*SpanFn)(void* user, u64 at, u64 from, u64 n);
/* the run of R's page gr ([ra, rb) of it) that o's page j holds, if any */
static void out_span_page(const GfxOut* o, u64 j, u64 gva, u64 gr, u64 ra, u64 rb, SpanFn fn, void* user) {
    u64 go = (o->addr & ~4095ull) + j * 4096, oend = o->addr + o->size;
    u64 oa = go > o->addr ? 0 : o->addr - go, ob = go + 4096 < oend ? 4096 : oend - go;
    u64 a = ra > oa ? ra : oa, b = rb < ob ? rb : ob;
    if (a < b) fn(user, gr + a - gva, go + a - o->addr, b - a);
}
static void out_spans(const GfxOut* o, u64 gva, u64 len, const u64* rmap, SpanFn fn, void* user) {
    u64 rbase = gva & ~4095ull, rend = gva + len, obase = o->addr & ~4095ull, n = o->npages;
    /* both one run of guest memory (most are): their bytes in common are where the runs meet, one span. The walk
       below looked each of the range's pages up in o's, for every result a draw's binding met: a storage range of
       half a megabyte against a few dozen results was a sixth of one game's GPU thread */
    if (len && o->cpu != ~0ull && rmap[0] != ~0ull) {
        u64 np = (rend - rbase + 4095) >> 12, i = 1;
        while (i < np && rmap[i] == rmap[0] + i) i++;
        if (i == np) {
            u64 rcpu = rmap[0] << 12 | (gva & 4095), lo = rcpu > o->cpu ? rcpu : o->cpu;
            u64 hi = rcpu + len < o->cpu + o->size ? rcpu + len : o->cpu + o->size;
            if (lo < hi) fn(user, lo - rcpu, lo - o->cpu, hi - lo);
            return;
        }
    }
    for (u64 i = 0, gr = rbase; gr < rend; i++, gr += 4096) {
        u64 ra = gr > gva ? 0 : gva - gr, rb = gr + 4096 < rend ? 4096 : rend - gr, page = rmap[i];
        u64 same = gr >= obase && gr - obase < n * 4096 ? (gr - obase) >> 12 : n; /* o's page at this address */
        const u64* hit;
        if (page == ~0ull) { /* unmapped: only o's page at the same address, if that is unmapped too */
            if (same < n && o->vmap[same] == ~0ull) out_span_page(o, same, gva, gr, ra, rb, fn, user);
            continue;
        }
        hit = bsearch(&page, o->pages, n, sizeof page, page_cmp);
        if (!hit) continue;
        if ((hit == o->pages || hit[-1] != page) && (hit + 1 == o->pages + n || hit[1] != page) && same < n &&
            o->vmap[same] == page) {
            out_span_page(o, same, gva, gr, ra, rb, fn, user); /* o holds the page once, at this address */
            continue;
        }
        for (u64 j = 0; j < n; j++) /* elsewhere in o, or more than once */
            if (o->vmap[j] == page) out_span_page(o, j, gva, gr, ra, rb, fn, user);
    }
}
/* the guest CPU page behind each GPU page of [gva, gva + len), ~0 for an unmapped one: in local when 16 do, malloc'd
   otherwise (NULL out of memory) */
static u64* range_map(u64 gva, u64 len, u64 local[16]) {
    u64 base = gva & ~4095ull, n = (gva + len - base + 4095) >> 12;
    u64* map = n <= 16 ? local : malloc(n * sizeof *map);
    if (!map) return NULL;
    for (u64 i = 0; i < n; i++) {
        u64 cpu = gmmu_translate(base + i * 4096);
        map[i] = cpu ? cpu >> 12 : ~0ull;
    }
    return map;
}

typedef struct WwStage WwStage;
/* this thread's state */
static _Thread_local struct {
    /* Suppress recursive lookups by the landing thread. Other readers still take vk_lock and wait for landing. */
    int out_landing;
    /* A job on the GPU thread checks against the newest epoch when it started (vk_ww_stream), not one a later
       submission starts while it runs: what the CPU wrote for that submission is the later job's to see. That leaves
       the scanner thread (ww_scanner) the time until the job starts to scan what it will check */
    u32 ww_stream;
    int ww_streaming;
    WwStage* wstage; /* the running scan's, for ww_note */
} t_buffer;

static void out_free(GfxOut* o);
static int back_record(GfxOut* o);
void vk_buf_writable_abort(void) {
    g_npw = 0;
    vk_uma_writes_abort();
    g_buffer.order.reused_write = 0;
    while (g_prep) {
        GfxOut* o = g_prep;
        g_prep = o->next;
        out_free(o);
    }
    g_prep_tail = NULL;
}
/* a reference to log for o, from a writer whose work was recorded: a result that compares (org) takes none, as its
   landing finds what changed without one */
static void out_add_log(GfxOut* o, VkWriteLog* log, u32 part, u32 at, u32 len) {
    if (o->org) return;
    if (o->nlogs == o->caplogs) {
        u32 cap = o->caplogs ? o->caplogs * 2 : 4;
        struct OutLog* grown = realloc(o->logs, (size_t)cap * sizeof *grown);
        if (!grown) fatal("Out of memory recording a storage result's writers");
        o->logs = grown;
        o->caplogs = cap;
    }
    o->logs[o->nlogs++] = (struct OutLog){log, part, at, len};
    vk_wlog_ref(log);
}
void vk_buf_log_retire(VkWriteLog* l) {
    if (g_nretire == g_capretire) {
        u32 cap = g_capretire ? g_capretire * 2 : 64;
        void* grown = realloc(g_retire, (size_t)cap * sizeof *g_retire);
        if (!grown) fatal("Out of memory keeping a write log");
        g_retire = grown;
        g_capretire = cap;
    }
    g_retire[g_nretire].log = l;
    g_retire[g_nretire++].submit = vk_submit_number();
}
void vk_buf_writable_commit(void) {
    vk_uma_writes_commit();
    for (u32 i = 0; i < g_npw; i++) out_add_log(g_pw[i].o, g_pw[i].log, g_pw[i].part, g_pw[i].at, g_pw[i].len);
    g_npw = 0;
    /* Reusing an output writes its existing buffer without adding another pending result. Count that write
       too, but only after the draw or dispatch was recorded. An abandoned setup must not invent writes. */
    if (g_prep || g_buffer.order.reused_write) g_buffer.order.writes++;
    g_buffer.order.reused_write = 0;
    /* the results written back on the GPU after the work just recorded go, never pending; one that can't be is
       pending as any other */
    for (GfxOut **pp = &g_prep, *o; (o = *pp) != NULL;) {
        if (o->back && back_record(o)) {
            *pp = o->next;
            out_free(o);
        } else {
            o->back = 0;
            pp = &o->next;
        }
    }
    g_prep_tail = NULL;
    for (GfxOut* o = g_prep; o; o = o->next) g_prep_tail = o;
    if (!g_prep) return;
    for (GfxOut* o = g_prep; o; o = o->next) {
        o->written = g_buffer.order.writes;
        o->seq = ++g_outx.seq;
        out_pages(o, 1);
        outx_result(o, 1);
    }
    if (g_out_tail)
        g_out_tail->next = g_prep;
    else
        g_out = g_prep;
    g_out_tail = g_prep_tail;
    g_prep = g_prep_tail = NULL;
    g_gfx_any = 1;
}
/* whether two sorted page lists share a page */
static int pages_meet(const GfxOut* a, const GfxOut* b) {
    u32 i = 0, j = 0;
    while (i < a->npages && j < b->npages) {
        if (a->pages[i] == ~0ull || b->pages[j] == ~0ull) return 0; /* unmapped pages sort last */
        if (a->pages[i] < b->pages[j])
            i++;
        else if (a->pages[i] > b->pages[j])
            j++;
        else
            return 1;
    }
    return 0;
}
/* e has landed: a later composed result o read e's result where they share guest memory, not the bytes org holds
   there, so the bytes e changed become o's original bytes. Then o lands only what its own draw changed, and doesn't
   take e's landing for a guest write and drop its own writes over it (vk_write_changed). Either GPU range may map
   the shared pages at other addresses: they meet through the GMMU, whose mappings stay put while results are pending
   (a mapping change lands them first) */
/* The runs of the landing result's bytes its draw changed, its 8-byte words that differ from its original ones,
   merged: found by the landing itself, which compares them already (vk_write_changed_runs). Rebasing every later composed result compared all of the landed
   result's bytes again for each of them. One game landed 20,000 results in 10 s that changed 4 MB of their 590, and
   the rebases read 42 GB, most of the release thread, under vk_lock, which the GPU thread waits for */
typedef VkRun ChgRun;
static ChgRun g_chg_first[64]; /* where the runs start, so even a failed allocation has room for one */
static ChgRun* g_chg = g_chg_first;
static u32 g_nchg, g_capchg = 64;
/* a run of changed granules of the result landing (vk_write_changed_runs's) into g_chg; with no memory for more, every
   byte is a candidate (one run) */
static void chg_add(void* user, u32 from, u32 to) {
    const GfxOut* e = (const GfxOut*)user;
    if (g_nchg == 1 && g_chg[0].from == 0 && g_chg[0].to == e->size) return; /* every byte already */
    if (g_nchg == g_capchg) {
        u32 cap = g_capchg * 2;
        ChgRun* b = g_chg == g_chg_first ? malloc((size_t)cap * sizeof *b) : realloc(g_chg, (size_t)cap * sizeof *b);
        if (!b) {
            if (!e->org) fatal("Out of memory landing a storage result"); /* a logged one lands its runs alone */
            g_chg[0] = (ChgRun){0, e->size};
            g_nchg = 1;
            return;
        }
        if (g_chg == g_chg_first) memcpy(b, g_chg_first, sizeof g_chg_first);
        g_chg = b;
        g_capchg = cap;
    }
    g_chg[g_nchg++] = (ChgRun){from, to};
}
typedef struct {
    const GfxOut* e;
    GfxOut* o;
} Rebase;
/* the changed bytes of e's [from, from + n) into o's original bytes at at. Eight bytes at a time: a result is mostly
   the bytes it read, and a byte loop here was 84% of the release thread's time in one game */
static void rebase_bytes(const Rebase* r, u64 at, u64 from, u64 n) {
    const u8 *res = r->e->ptr + from, *org = r->e->org + from;
    u8* dst = r->o->org + at;
    u64 k = 0;
    for (; k + 8 <= n; k += 8) {
        u64 a, b, d, m;
        memcpy(&a, res + k, 8);
        memcpy(&b, org + k, 8);
        if (a == b) continue;
        memcpy(&d, dst + k, 8);
        m = a ^ b; /* the low bit of each byte becomes the OR of its byte's bits, then the byte becomes 0 or 0xFF */
        m |= m >> 4;
        m |= m >> 2;
        m |= m >> 1;
        m = (m & 0x0101010101010101ull) * 0xFF;
        d = (d & ~m) | (a & m);
        memcpy(dst + k, &d, 8);
    }
    for (; k < n; k++)
        if (res[k] != org[k]) dst[k] = res[k];
}
/* a span of e in o: only where it meets e's changed runs (g_chg, sorted), the first found by binary search */
static void rebase_span(void* user, u64 at, u64 from, u64 n) {
    u32 lo = 0, hi = g_nchg;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (g_chg[mid].to <= from)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (u32 i = lo; i < g_nchg && g_chg[i].from < from + n; i++) {
        u64 a = g_chg[i].from > from ? g_chg[i].from : from, b = g_chg[i].to < from + n ? g_chg[i].to : from + n;
        const Rebase* r = user;
        if (!r->e->org) /* a logged result's runs are the bytes its writers wrote, every one */
            memcpy(r->o->org + at + (a - from), r->e->ptr + a, b - a);
        else
            rebase_bytes(user, at + (a - from), a, b - a);
    }
}
static void out_rebase(const GfxOut* e, GfxOut* o) {
    Rebase r = {e, o};
    out_spans(e, o->addr, o->size, o->vmap, rebase_span, &r);
}
/* the composed results recorded after e that share its guest pages, in this frame's landed list (rest), later frames'
   (the index) and the draw being set up, from e's changed runs in g_chg, which its landing found */
static void out_rebase_later(const GfxOut* e, GfxOut* rest) {
    OutList l;
    if (!g_nchg) return; /* it changed nothing: memory has what every later one took as original */
    for (GfxOut* o = rest; o; o = o->next)
        if (o->composed && o->org && pages_meet(e, o)) out_rebase(e, o); /* a logged one compares nothing */
    out_list_init(&l);
    for (u32 i = 0, j; i < e->npages && e->pages[i] != ~0ull; i = j) {
        j = out_run_end(e, i);
        outx_find(&l, &g_outx.guest, e->pages[i], e->pages[j - 1]);
    }
    out_list_sort(&l);
    for (u32 i = 0; i < l.n; i++)
        if (l.v[i]->composed && l.v[i]->org && pages_meet(e, l.v[i])) out_rebase(e, l.v[i]);
    out_list_free(&l);
    for (GfxOut* o = g_prep; o; o = o->next)
        if (o->composed && o->org && pages_meet(e, o)) out_rebase(e, o);
}

static int run_cmp(const void* a, const void* b) {
    u32 x = ((const ChgRun*)a)->from, y = ((const ChgRun*)b)->from;
    return (x > y) - (x < y);
}
typedef struct {
    const GfxOut* o;
    u32 at;
} LogRuns;
static void log_run(void* user, u32 from, u32 to) {
    const LogRuns* r = user;
    u32 a = r->at + from, b = r->at + to;
    if (a < r->o->size) chg_add((void*)r->o, a, b < r->o->size ? b : r->o->size);
}
static u64 g_wb_logged, g_wb_logged_bytes, g_wb_logged_windows, g_wb_logged_conflicts;
/* o, a logged result: the bytes its writers' logs give, every one of them and no other, from where its shader wrote
   them, their runs in g_chg for the later results that compose it (out_rebase_later). A byte no writer wrote holds what
   o started from, or a byte of an earlier result composed into it (gfx_overlay), which that one lands */
static void land_logged(GfxOut* o) {
    u32 windows = 0;
    for (u32 k = 0; k < o->nlogs; k++) {
        LogRuns r = {o, o->logs[k].at};
        windows += vk_wlog_runs(o->logs[k].log, o->logs[k].part, o->logs[k].len, log_run, &r);
    }
    if (o->nlogs > 1 && g_nchg > 1) { /* each writer's runs in order: all of them sorted, and those that meet joined */
        qsort(g_chg, g_nchg, sizeof *g_chg, run_cmp);
        u32 n = 0;
        for (u32 i = 0; i < g_nchg; i++)
            if (n && g_chg[i].from <= g_chg[n - 1].to) {
                if (g_chg[i].to > g_chg[n - 1].to) g_chg[n - 1].to = g_chg[i].to;
            } else
                g_chg[n++] = g_chg[i];
        g_nchg = n;
    }
    u32 conflicts = vk_write_runs(o->addr, o->ptr, g_chg, g_nchg, o->wseen);
    if (copy_stats_on()) {
        g_wb_logged++;
        g_wb_logged_windows += windows;
        g_wb_logged_conflicts += conflicts;
        for (u32 i = 0; i < g_nchg; i++) g_wb_logged_bytes += g_chg[i].to - g_chg[i].from;
    }
}
static void out_free(GfxOut* o) {
    for (u32 k = 0; k < o->nlogs; k++) vk_wlog_unref(o->logs[k].log);
    free(o->logs);
    if (o->org_alloc) free(o->org);
    if (o->keep) vk_own_res_unref(o->keep); /* a made owner stays published: it holds memory's bytes */
    free(o);
}
/* The frame fence has signaled. Its stage allocations still exist here; cmd.c frees them only after this call. */
void vk_buf_frame_done(u64 frame) {
    GfxOut *done = NULL, *done_tail = NULL;
    for (GfxOut** pp = &g_out; *pp;) {
        GfxOut* o = *pp;
        if (o->frame != frame) {
            pp = &o->next;
            continue;
        }
        *pp = o->next;
        o->next = NULL;
        outx_result(o, -1); /* the index holds g_out alone: a search while these land finds what the walk found */
        if (done_tail)
            done_tail->next = o;
        else
            done = o;
        done_tail = o;
    }
    g_out_tail = NULL;
    for (GfxOut* o = g_out; o; o = o->next) g_out_tail = o;
    g_gfx_any = g_out != NULL || done != NULL;
    VmaAllocation invalidated = VK_NULL_HANDLE;
    t_buffer.out_landing++;
    while (done) {
        GfxOut* o = done;
        done = o->next;
        if (o->keep) { /* its bytes stay in their owner, written: what is built from them takes the new ones */
            out_pages(o, -1);
            gpu_mem_written_shader(o->addr, o->size);
            vk_own_res_unref(o->keep);
            free(o);
            continue;
        }
        if (o->mem != invalidated) {
            if (!VK_OK(vmaInvalidateAllocation(g_vk.vma, o->mem, 0, VK_WHOLE_SIZE)))
                fatal("Could not invalidate a graphics storage buffer after its frame completed");
            invalidated = o->mem;
        }
        if (o->initial) {
            if (!VK_OK(vmaInvalidateAllocation(g_vk.vma, o->initial_mem, 0, VK_WHOLE_SIZE)))
                fatal("Could not invalidate a graphics storage buffer baseline");
            memcpy(o->org, o->initial, o->size);
            o->initial = NULL;
        }
        g_nchg = 0;
        if (!o->org)
            land_logged(o);
        else {
            vk_write_changed_runs(o->addr, o->ptr, o->org, o->size, chg_add, o);
            g_buffer.wstat.merged++;
        }
        out_pages(o, -1);
        out_rebase_later(o, done);
        if (copy_stats_on()) {
            g_wb_outputs++;
            g_wb_bytes += o->size;
        }
        out_free(o);
    }
    t_buffer.out_landing--;
    /* the dispatches' references to their logs, of the submits done (the results' own, just dropped, came first) */
    u32 kept = 0;
    for (u32 i = 0; i < g_nretire; i++)
        if (g_retire[i].submit <= frame)
            vk_wlog_unref(g_retire[i].log);
        else
            g_retire[kept++] = g_retire[i];
    g_nretire = kept;
    vk_mirror_done(frame);
    g_gfx_any = g_out != NULL;
    vk_uma_frame_done(frame);
}
/* At the frame's submit: its storage results are copied into host-cached memory (vk_stage_back), where landing them
   compares bytes quickly, or takes a logged one's runs. A result whose copy can't be allocated is read where the
   shader wrote it. A kept one stays in its owner */
void vk_buf_host_barrier(VkCommandBuffer cb, u64 frame) {
    vk_uma_host_barrier(cb, frame);
    vk_mirror_harvest(cb, frame); /* the pages the submit's programs wrote in the GPU mirror */
    GfxOut* first = g_out;
    while (first && (first->frame != frame || first->keep)) first = first->next;
    /* the write logs of the submit's dispatches are read by the host too, results of theirs pending or not (the
       retire list is in submit order) */
    int logs = g_nretire && g_retire[g_nretire - 1].submit == frame;
    if (!first && !logs) return;
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = out_stages(),
                          .srcAccessMask = out_access(),
                          .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                          .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    if (first) rec_CmdPipelineBarrier2(cb, &d);
    /* consecutive results usually share both buffers: one copy command each run */
    VkBufferCopy regions[64];
    u32 n = 0;
    VkBuffer src = VK_NULL_HANDLE, dst = VK_NULL_HANDLE;
    for (GfxOut* o = first; o; o = o->next) {
        if (o->frame != frame || o->keep) continue;
        /* a logged one too, whole: where its shader wrote it, the output ring, can be device memory behind the BAR,
           and the CPU's reads of its logged runs there were 19% of a Switch game's GPU thread in its street (728
           results a frame of 3.8 KB on average, all logged since the Maxwell programs log their stores). An Ounce
           program's 16 MiB bindings, which made this one take its runs in place, are in the GPU mirror */
        VkBuffer back;
        u64 at;
        VmaAllocation mem;
        u8* p = vk_stage_back(o->size, &back, &at, &mem);
        if (!p) break; /* out of memory: this result and the rest stay where the shader wrote them */
        if (n && (n == sizeof regions / sizeof *regions || o->buf != src || back != dst)) {
            rec_CmdCopyBuffer(cb, src, dst, n, regions);
            n = 0;
        }
        src = o->buf;
        dst = back;
        regions[n++] = (VkBufferCopy){.srcOffset = o->off, .dstOffset = at, .size = o->size};
        g_buffer.wstat.read_back += o->size;
        o->ptr = p;
        o->mem = mem;
    }
    if (n) rec_CmdCopyBuffer(cb, src, dst, n, regions);
    b.srcStageMask |= VK_PIPELINE_STAGE_2_COPY_BIT;
    b.srcAccessMask |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    rec_CmdPipelineBarrier2(cb, &d);
}
static void wait_finish(int reason) {
    u64 t = copy_stats_on() ? plat_ns() : 0;
    vk_finish();
    if (t) {
        u64 ns = plat_ns() - t;
        g_wb_waits++;
        g_wb_wait_ns += ns;
        g_wb_reason[reason]++;
        g_wb_reason_ns[reason] += ns;
    }
}
static void land_reason(int reason) {
    if (!g_gfx_any || t_buffer.out_landing) return;
    vk_lock();
    if (g_gfx_any && !t_buffer.out_landing) wait_finish(reason);
    vk_unlock();
}
void vk_buf_land(void) { land_reason(WB_SIGNAL); }
void vk_buf_land_syncwait(void) { land_reason(WB_SYNCWAIT); }
void vk_buf_land_map(void) { land_reason(WB_MAP); }
/* Two GPU virtual ranges can name the same guest pages. Mapping changes land pending output while holding vk_lock,
   so the translations remain stable until this check and any following landing have completed. */
static int page_cmp(const void* a, const void* b) {
    u64 x = *(const u64*)a, y = *(const u64*)b;
    return (x > y) - (x < y);
}
/* A range's guest pages, translated once for a walk of the pending results (translating them again for every result
   was the GPU thread's top cost once results stayed pending through a frame). Up to 16 pages are kept; a longer
   range compares with each result's pages through bounds alone, then page by page */
typedef struct {
    u64 page[16];
    u32 n;
    u64 lo, hi; /* the lowest and highest guest page, ~0 and 0 when none is mapped */
    u64 a, an;
    int run; /* every page mapped, each the one after the last: [lo, hi] whole */
} RangePages;
static void range_pages(u64 a, u64 an, RangePages* q) {
    q->n = 0;
    q->lo = ~0ull;
    q->hi = 0;
    q->a = a;
    q->an = an;
    q->run = 1;
    for (u64 ap = a >> 12, ae = (a + an - 1) >> 12; ap <= ae; ap++) {
        u64 ac = gmmu_translate(ap << 12), page;
        if (!ac) {
            q->run = 0;
            continue;
        }
        page = ac >> 12;
        if (q->n && page != q->hi + 1) q->run = 0;
        if (q->n < 16) q->page[q->n] = page;
        q->n++;
        if (page < q->lo) q->lo = page;
        if (page > q->hi) q->hi = page;
    }
}
static int backing_overlap(const RangePages* q, const GfxOut* o) {
    u64 olo, ohi;
    u32 i;
    if (!q->n || !o->npages) return 0;
    olo = o->pages[0];
    ohi = o->pages[o->npages - 1];
    if (ohi == ~0ull) /* unmapped pages sort last */
        for (i = o->npages; i-- && o->pages[i] == ~0ull;) ohi = i ? o->pages[i - 1] : 0;
    if (q->hi < olo || ohi < q->lo) return 0;
    if (q->run && o->cpu != ~0ull) return 1; /* two whole runs of pages whose bounds meet */
    if (q->n <= 16) {
        for (i = 0; i < q->n; i++)
            if (bsearch(&q->page[i], o->pages, o->npages, sizeof q->page[i], page_cmp)) return 1;
        return 0;
    }
    for (u64 ap = q->a >> 12, ae = (q->a + q->an - 1) >> 12; ap <= ae; ap++) {
        u64 ac = gmmu_translate(ap << 12);
        if (!ac) continue;
        u64 page = ac >> 12;
        if (bsearch(&page, o->pages, o->npages, sizeof page, page_cmp)) return 1;
    }
    return 0;
}
static void any_span(void* user, u64 at, u64 from, u64 n) {
    (void)at;
    (void)from;
    (void)n;
    *(int*)user = 1;
}
/* [gva, gva + len) and o share bytes, through the same GPU address or pages in common (q): a result lands its own
   bytes alone, so the rest of a page it shares is free to read and write (Pokemon Legends Z-A writes a word next to
   its transform feedback captures before each, through a macro's inline upload). *rmap is the range's pages, made
   once; without memory for them, any page in common counts */
static int out_meets(const GfxOut* o, u64 gva, u64 len, const RangePages* q, u64** rmap, u64 local[16]) {
    int hit = 0;
    if (gva < o->addr + o->size && o->addr < gva + len) return 1;
    if (!backing_overlap(q, o)) return 0;
    if (!*rmap && !(*rmap = range_map(gva, len, local))) return 1;
    out_spans(o, gva, len, *rmap, any_span, &hit);
    return hit;
}
/* Into l, in g_out's order, the pending results that can share bytes with [gva, gva + len) (q: its guest pages), for
   the caller to compare as it did each result of the list: those on a guest page of the range (its pages one by one
   when q lists them, else their bounds), and those with an unmapped page whose GPU range meets it. Any other result
   has neither a guest page nor a GPU address in common with the range */
static void out_near(OutList* l, u64 gva, u64 len, const RangePages* q) {
    out_list_init(l);
    if (q->run || q->n > 16) {
        if (q->n) outx_find(l, &g_outx.guest, q->lo, q->hi);
    } else
        for (u32 i = 0; i < q->n; i++) outx_find(l, &g_outx.guest, q->page[i], q->page[i]);
    if (g_outx.gpu.n && len) outx_find(l, &g_outx.gpu, gva >> 12, (gva + len - 1) >> 12);
    out_list_sort(l);
}
static void mem_need_reason(u64 gva, u64 len, int reason) {
    RangePages q;
    OutList l;
    u64 local[16], *rmap = NULL;
    if (!g_gfx_any || t_buffer.out_landing || !len) return;
    /* asked first without the lock, which another thread may hold: results are added on the GPU thread, so a miss
       here can only be one another thread has landed since. A hit is asked again with it */
    if (reason != WB_MAP && !pend_hit(&g_out_pages, gva, len)) return;
    vk_lock();
    if (reason != WB_MAP && !pend_hit(&g_out_pages, gva, len)) {
        vk_unlock();
        return;
    }
    range_pages(gva, len, &q);
    out_near(&l, gva, len, &q);
    /* the results over the range land: those of submits already made by waiting for the newest of them, the GPU
       going on with what came after, and only one in the commands being recorded by finishing them all */
    u64 newest = 0;
    int recording = 0;
    for (u32 i = 0; i < l.n; i++) {
        const GfxOut* o = l.v[i];
        if (reason == WB_MAP ? (gva < o->addr + o->size && o->addr < gva + len) || backing_overlap(&q, o)
                             : out_meets(o, gva, len, &q, &rmap, local)) {
            if (o->frame > vk_last_submit()) recording = 1;
            if (o->frame > newest) newest = o->frame;
        }
    }
    out_list_free(&l);
    if (recording)
        wait_finish(reason);
    else if (newest) {
        vk_wait_submit(newest);
        vk_land_through(newest);
    }
    if (rmap && rmap != local) free(rmap);
    vk_unlock();
}
void vk_buf_mem_need(u64 gva, u64 len) { mem_need_reason(gva, len, WB_READ); }
int vk_buf_pending_any(void) { return g_gfx_any; }
u64 vk_buf_adds(void) { return g_out_pages.adds + vk_uma_out_adds(); } /* a write in place is one too */
int vk_buf_pending_unsubmitted(void) {
    for (GfxOut* o = g_out; o; o = o->next)
        if (o->frame == vk_submit_number()) return 1;
    return 0;
}
int vk_buf_pending(u64 gva, u64 len) {
    RangePages q;
    OutList l;
    int hit = 0;
    if (!g_gfx_any || !len || !pend_hit(&g_out_pages, gva, len)) return 0;
    range_pages(gva, len, &q);
    out_near(&l, gva, len, &q);
    for (u32 i = 0; i < l.n && !hit; i++)
        hit = (gva < l.v[i]->addr + l.v[i]->size && l.v[i]->addr < gva + len) || backing_overlap(&q, l.v[i]);
    out_list_free(&l);
    return hit;
}
void vk_buf_mem_need_map(u64 gva, u64 len) { mem_need_reason(gva, len, WB_MAP); }
void vk_buf_before_write(u64 gva, u64 len) {
    if (len)
        mem_need_reason(gva, len, WB_WRITE);
    else
        land_reason(WB_SIGNAL);
}
/* GPU copies of pending results into staging, in the order the results were recorded, after the shaders and copies
   that wrote them: copies over bytes an earlier copy wrote wait for it behind a barrier (one copy command mustn't
   overlap itself, and two unordered ones would land in either order), so the later result ends up on top */
typedef struct {
    VkBufferCopy regions[64];
    u64 written[128][2]; /* destination bytes copied since the last barrier */
    u32 n, nwritten;
    VkBuffer src, dst;
    int started;
} CopyBatch;
static void copy_barrier(VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                         VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = src_stage,
                          .srcAccessMask = src_access,
                          .dstStageMask = dst_stage,
                          .dstAccessMask = dst_access};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(vk_cmd(), &d);
}
static void copy_flush(CopyBatch* c) {
    if (c->n) rec_CmdCopyBuffer(vk_cmd(), c->src, c->dst, c->n, c->regions);
    c->n = 0;
}
static void copy_add(CopyBatch* c, VkBuffer src, u64 src_off, u64 dst_off, u64 size) {
    u32 k;
    int overlap = c->nwritten == sizeof c->written / sizeof *c->written;
    if (!c->started) {
        vk_render_end();
        copy_barrier(out_stages(), out_access(), VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        c->started = 1;
    }
    for (k = 0; k < c->nwritten && !overlap; k++)
        overlap = dst_off < c->written[k][1] && c->written[k][0] < dst_off + size;
    if (overlap) {
        copy_flush(c);
        copy_barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT);
        c->nwritten = 0;
    }
    if (c->n && (c->n == sizeof c->regions / sizeof *c->regions || src != c->src)) copy_flush(c);
    c->src = src;
    c->regions[c->n++] = (VkBufferCopy){.srcOffset = src_off, .dstOffset = dst_off, .size = size};
    c->written[c->nwritten][0] = dst_off;
    c->written[c->nwritten][1] = dst_off + size;
    c->nwritten++;
}
static void copy_end(CopyBatch* c) {
    copy_flush(c);
    if (c->started) /* before draws and dispatches read (or write) what was copied */
        copy_barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
}

/* guest bytes without landing pending results (they are copied over them on the GPU); unmapped pages read as zero */
static void read_untracked(u64 gva, u8* dst, u64 len) {
    const u8* contiguous;

    if (!len) return;
    contiguous = gmem_ptr_untracked(gva, len);
    if (contiguous) {
        memcpy(dst, contiguous, len);
        return;
    }
    while (len) {
        u64 n = 4096 - (gva & 4095);
        const u8* p;
        if (n > len) n = len;
        p = gmem_ptr_untracked(gva, n);
        if (p)
            memcpy(dst, p, n);
        else
            memset(dst, 0, n);
        gva += n;
        dst += n;
        len -= n;
    }
}
/* whether n bytes of guest memory (host, as write tracking watches them; untracked, the same read without a trap) still
   hold org, written answering vk_written_since since a snapshot: 0, all of them; 1, those of the pages it saw written
   alone, as the others do; -1 (no tracking), all compared. A storage range of an Ounce program is all of the memory
   mapped from its address, up to 16 MiB, and one page written in it had every byte compared: a tenth of one game's
   CPU time */
typedef struct {
    const u8 *untracked, *org;
    int same;
} OrgRuns;
static void org_run(void* user, u64 at, u64 n) {
    OrgRuns* r = user;
    if (r->same && memcmp(r->untracked + at, r->org + at, n)) r->same = 0;
}
static int org_holds(const u8* host, const u8* untracked, const u8* org, u64 n, u32 since, int written) {
    if (!written) return 1;
    if (!untracked) return 0;
    if (written < 0) return !memcmp(untracked, org, n);
    OrgRuns r = {untracked, org, 1};
    vk_written_runs(host, n, since, org_run, &r);
    return r.same;
}
/* A kept result's bytes are still where its draws wrote them: the owner it writes holds all of its range, as its bind
   left it. The CPU touching them landed that owner, after those draws, and let go of it: a draw writing it again would
   write bytes nothing lands */
static int keep_intact(const GfxOut* o) {
    OwnRange r[2];
    return own_lookup(o->cpu, o->size, r, 2) == 1 && r[0].res == o->keep && r[0].addr == o->cpu &&
           r[0].size == o->size && r[0].res_off == o->keep_off;
}
/* a pending result can be copied over guest memory on the GPU: this frame's staging or the last one's still holds
   it, and the guest hasn't written the bytes it covers in [lo, hi) since it was snapshotted (a kept one: its owner
   still holds them) */
static int out_copyable(const GfxOut* o, u64 lo, u64 hi) {
    if (o->keep) return keep_intact(o);
    const u8* host = gmem_host(lo, hi - lo);
    if (o->frame_id + 1 < vk_frame_id() || !host) return 0;
    int written = vk_written_since(host, hi - lo, o->wseen);
    if (written == 0) return 1;
    if (o->initial || !o->org) return 0; /* the baseline is still being produced by the GPU, or there is none */
    const u8* untracked = gmem_ptr_untracked(lo, hi - lo);
    if (untracked) g_checks.compares++;
    return org_holds(host, untracked, o->org + (lo - o->addr), hi - lo, o->wseen, written);
}
/* the same for each run of o's bytes in [gva, gva + len), through whichever GPU address maps them (out_spans) */
typedef struct {
    const GfxOut* o;
    u64 gva;
    int ok;
} Copyable;
/* Every draw that binds memory under pending results asks this of each of them, so comparing their bytes made the
   walk quadratic in a frame's results: a frame of 200 composed bindings spent a fifth of the GPU thread in memcmp. Pages
   write tracking saw unwritten since the result read its original bytes still hold them. It sees a write from the
   next write epoch on, which is when the stream can see it too, and a host without tracking compares */
static void copyable_span(void* user, u64 at, u64 from, u64 n) {
    Copyable* c = user;
    const u8* host = c->ok ? gmem_host(c->gva + at, n) : NULL;
    if (!host) {
        /* a page the result doesn't map (vmap ~0: out_spans pairs it only with the range's unmapped page) holds no guest
           bytes for the guest to write, and its landing writes nothing there either. One game's storage results end 336
           bytes into an unmapped page: answered "changed", each binding of them landed, 15 waits for the GPU a frame */
        u64 j = (((c->o->addr + from) & ~4095ull) - (c->o->addr & ~4095ull)) >> 12;
        if (c->ok && j < c->o->npages && c->o->vmap[j] == ~0ull) return;
        c->ok = 0;
        return;
    }
    int written = vk_written_since(host, n, c->o->wseen);
    if (written == 0) return;
    if (c->o->initial || !c->o->org) { /* no original bytes to tell the guest's writes by: it lands first */
        c->ok = 0;
        return;
    }
    const u8* untracked = gmem_ptr_untracked(c->gva + at, n);
    if (untracked) g_checks.compares++;
    if (!org_holds(host, untracked, c->o->org + from, n, c->o->wseen, written)) c->ok = 0;
}
/* A composition takes each byte from the newest pending result over it: that result was made over the ones before it
   (composed, or over guest memory they hadn't touched) and its draw ran after theirs, so its buffer holds the byte as
   every result before it left it, and copying the older ones too only to overwrite them made composing quadratic. One
   game composed 200 bindings a frame over a few hundred results each and its GPU copied 955 MB a frame, 147 ms. The
   results are walked from the newest, and a byte of the destination one of them gave is skipped by the older ones:
   Covered holds those bytes as sorted, disjoint runs of destination offsets */
typedef struct {
    u64 a, b;
} CoverRun;
typedef struct {
    CoverRun* v;
    u32 n, cap;
    u64 len;              /* the destination's size: all of it covered ends the walk */
    CoverRun first[32];   /* where the runs start */
} Covered;
static void cover_init(Covered* c, u64 len) {
    c->v = c->first;
    c->n = 0;
    c->cap = sizeof c->first / sizeof *c->first;
    c->len = len;
}
static void cover_free(Covered* c) {
    if (c->v != c->first) free(c->v);
}
static int cover_full(const Covered* c) { return c->n == 1 && c->v[0].a == 0 && c->v[0].b >= c->len; }
/* [a, b) of the destination, given by a newer result than the ones still to come: fn(user, a', b') for each of its
   parts no newer result gave, then all of it covered. 0 when the runs can't grow (nothing covered then: the older
   results copy over it too, as before, which is only slower) */
static int cover_take(Covered* c, u64 a, u64 b, void (*fn)(void* user, u64 a, u64 b), void* user) {
    u32 i = 0, j;
    while (i < c->n && c->v[i].b < a) i++;
    u64 at = a;
    for (j = i; j < c->n && c->v[j].a <= b; j++) {
        if (c->v[j].a > at) fn(user, at, c->v[j].a);
        if (c->v[j].b > at) at = c->v[j].b;
    }
    if (at < b) fn(user, at, b);
    /* the runs i..j-1 meet [a, b]: one run in their place */
    u64 na = a, nb = b;
    if (i < j) {
        if (c->v[i].a < na) na = c->v[i].a;
        if (c->v[j - 1].b > nb) nb = c->v[j - 1].b;
    }
    if (i == j) { /* a new run at i */
        if (c->n == c->cap) {
            u32 cap = c->cap * 2;
            CoverRun* v = c->v == c->first ? malloc((size_t)cap * sizeof *v) : realloc(c->v, (size_t)cap * sizeof *v);
            if (!v) return 0;
            if (c->v == c->first) memcpy(v, c->first, sizeof c->first);
            c->v = v;
            c->cap = cap;
        }
        memmove(&c->v[i + 1], &c->v[i], (size_t)(c->n - i) * sizeof *c->v);
        c->n++;
    } else {
        memmove(&c->v[i + 1], &c->v[j], (size_t)(c->n - j) * sizeof *c->v);
        c->n -= j - i - 1;
    }
    c->v[i] = (CoverRun){na, nb};
    return 1;
}
typedef struct {
    CopyBatch* c;
    const GfxOut* o;
    u64 dst_off, at, from; /* the span being taken: its destination offset and its offset in o */
    Covered* cov;
} Overlay;
static void overlay_part(void* user, u64 a, u64 b) {
    const Overlay* v = user;
    copy_add(v->c, v->o->buf, v->o->off + v->from + (a - v->at), v->dst_off + a, b - a);
}
/* the run [a, b) of an overlay's destination that no result gives: written in place, or staged and copied on the GPU
   (0 when staging has no room) */
static int overlay_gap(CopyBatch* c, u64 dst_off, u8* host, u64 a, u64 b, void (*gap)(void* user, u8* to, u64 a, u64 b),
                       void* user) {
    VkBuffer sb;
    u64 so;
    u8* p;
    if (host) {
        gap(user, host + a, a, b);
        return 1;
    }
    if (!(p = vk_stage_upload(b - a, 4, &sb, &so))) return 0;
    gap(user, p, a, b);
    copy_add(c, sb, so, dst_off + a, b - a);
    return 1;
}
static void overlay_span(void* user, u64 at, u64 from, u64 n) {
    Overlay* v = user;
    v->at = at;
    v->from = from;
    if (!cover_take(v->cov, at, at + n, overlay_part, v)) overlay_part(v, at, at + n);
}
/* copies the pending results' bytes in [gva, gva + len), through any GPU address, into dst at dst_off (after
   gfx_gpu_src said they can be): each byte from the newest result over it. gap(user, to, a, b) writes the bytes of
   each run [a, b) of the destination no result gives at to: into host, the destination's mapping, or (host NULL, dst
   in device memory) into staging, which the GPU copies into dst with the results. A result gives most of a composed
   range in the end, and copying all of it before (a few results over storage ranges of half a megabyte, bound and read
   a few hundred times a frame) was a fifth of one game's GPU thread. The CPU writes the gaps before the submit that
   copies the results, and a run cover_take couldn't record is filled too, then copied over on the GPU as before. 0 when
   a gap couldn't be staged (never with host) */
static int gfx_overlay(u64 gva, u64 len, VkBuffer dst, u64 dst_off, u8* host,
                       void (*gap)(void* user, u8* to, u64 a, u64 b), void* user) {
    CopyBatch c;
    Covered cov;
    RangePages q;
    OutList l;
    u64 local[16], *rmap = range_map(gva, len, local);
    if (!rmap) fatal("Out of memory composing a draw's storage buffer");
    range_pages(gva, len, &q);
    out_near(&l, gva, len, &q);
    memset(&c, 0, sizeof c);
    c.dst = dst;
    cover_init(&cov, len);
    for (u32 i = l.n; i-- && !cover_full(&cov);) { /* newest first */
        Overlay v = {&c, l.v[i], dst_off, 0, 0, &cov};
        out_spans(l.v[i], gva, len, rmap, overlay_span, &v);
    }
    out_list_free(&l);
    u64 at = 0;
    int ok = 1;
    for (u32 i = 0; i <= cov.n && at < len && ok; i++) {
        u64 a = i < cov.n ? cov.v[i].a : len, b = a < len ? a : len;
        if (b > at) ok = overlay_gap(&c, dst_off, host, at, b, gap, user);
        if (i < cov.n && cov.v[i].b > at) at = cov.v[i].b;
    }
    copy_end(&c);
    cover_free(&cov);
    if (rmap != local) free(rmap);
    return ok;
}

/* whether the guest still holds o's original bytes in [gva, gva + len) (rmap: made on first use) */
static int out_still_copyable(const GfxOut* o, u64 gva, u64 len, u64** rmap, u64 local[16]) {
    if (o->keep) return keep_intact(o); /* no original bytes to compare: its owner answers for all of them */
    /* A contained range in a contiguous result has the same byte offsets at this address. out_copyable checks its
       host run and write history directly, without allocating and translating one entry per page for out_spans. */
    if (o->cpu != ~0ull && gva >= o->addr && len <= o->size && gva - o->addr <= o->size - len)
        return out_copyable(o, gva, gva + len);
    Copyable c = {o, gva, o->frame_id + 1 >= vk_frame_id()};
    if (c.ok && !*rmap && !(*rmap = range_map(gva, len, local))) c.ok = 0;
    if (c.ok) out_spans(o, gva, len, *rmap, copyable_span, &c);
    return c.ok;
}
/* A result of an earlier submit of this frame that a draw of this one writes again in place, landing at this submit's
   end instead: its copy back for landing is recorded again then (the earlier one is left unread), and it moves to the
   end of the list, which is the order results land in. Only when no result recorded after it has bytes of it: such a
   result took its bytes as they were and lands before it, and it would land over those bytes as written since. A
   game binding a suffix of a storage buffer for each of its passes, about thirty submits a frame, made a new result
   for nearly every one: half a megabyte copied from guest memory and compared at landing each time */
static int out_take_into_submit(GfxOut* m) {
    RangePages q;
    OutList l;
    u64 local[16], *rmap = NULL;
    int later = 0;
    /* a kept one from an earlier frame too: it is in its owner, not that frame's staging, and copies nothing back.
       Refused, each frame's binding of it composed again while the GPU ran a frame behind, and landed the owner */
    if ((m->frame_id != vk_frame_id() && !m->keep) || m->frame >= vk_submit_number()) return 0;
    range_pages(m->addr, m->size, &q);
    out_near(&l, m->addr, m->size, &q);
    for (u32 i = 0; i < l.n && !later; i++) /* recorded after m: later in g_out */
        later = l.v[i]->seq > m->seq && out_meets(l.v[i], m->addr, m->size, &q, &rmap, local);
    out_list_free(&l);
    for (GfxOut* o = g_prep; o && !later; o = o->next) later = out_meets(o, m->addr, m->size, &q, &rmap, local);
    if (rmap && rmap != local) free(rmap);
    if (later) return 0;
    m->frame = vk_submit_number();
    m->frame_id = vk_frame_id();
    if (m != g_out_tail) {
        GfxOut** pp = &g_out;
        while (*pp != m) pp = &(*pp)->next;
        *pp = m->next;
        m->next = NULL;
        g_out_tail->next = m;
        g_out_tail = m;
        m->seq = ++g_outx.seq;
    }
    return 1;
}
/* A draw or dispatch can read an earlier one's storage result without a round trip through guest RAM. Returns 1 with
   that result's buffer itself in *buf, *off when the newest result over the range covers all of it, unchanged since,
   at an offset that is a multiple of align: from this submit when the reader writes it (its copy back for landing was
   recorded at its submit's end) or taken into it (out_take_into_submit), from this frame otherwise. Returns 2 when
   pending results cover the range and can all be copied over guest memory on the GPU (gfx_overlay), 0 when none do,
   or after landing them because one can't: the guest wrote its original bytes since. A result counts only with bytes
   of the range (out_meets): two storage buffers sharing a page, one after the other, made each read of one compose
   with the other's results, which gave it nothing. A result of the frame before is read where it is too (transform
   feedback captures the same buffers every frame, over the last frame's capture), its frame's rings held until the
   reader is done (vk_frame_hold) */
static int gfx_gpu_src(u64 gva, u64 len, VkBuffer* buf, u64* off, int writable, u64 align, int ordered) {
    GfxOut *match = NULL, *newest = NULL;
    int count = 0, before = 0;
    RangePages q;
    OutList l;
    u64 local[16], *rmap = NULL;
    if (!g_gfx_any || t_buffer.out_landing || !gva || !len) return 0;
    if (g_cs_any) vk_compute_mem_need(gva, len);
    if (!pend_hit(&g_out_pages, gva, len)) return 0;
    out_list_init(&l);
    /* A range contained in the last result already has the newest writer. Rebinding its suffix is common for NVN
       storage draws: do not translate all its pages and search the older results only to select this same tail.
       Other addresses still use the full alias-aware search below. */
    if (g_out_tail && gva >= g_out_tail->addr && len <= g_out_tail->size &&
        gva - g_out_tail->addr <= g_out_tail->size - len)
        newest = g_out_tail;
    else {
        range_pages(gva, len, &q);
        out_near(&l, gva, len, &q);
        for (u32 i = 0; i < l.n; i++)
            if (out_meets(l.v[i], gva, len, &q, &rmap, local)) newest = l.v[i];
    }
    if (!newest) {
        out_list_free(&l);
        if (rmap && rmap != local) free(rmap);
        return 0;
    }
    /* The newest result over the range was composed over the older ones when it was made (gfx_overlay, through any
       address), and its draw ran after theirs: when it covers the whole range at this address, its buffer holds every
       byte as all of them left it, and the older ones need no check. Requiring a single result made every binding
       after a second one compose again: in one game each storage range was copied, written back and landed about 16
       times a frame (114,000 compositions in 600 frames, against 3,600 for an offset or a submit) */
    if (gva >= newest->addr && gva + len <= newest->addr + newest->size) {
        if (!out_still_copyable(newest, gva, len, &rmap, local)) {
            if (copy_stats_on()) g_wb_draw_miss[newest->frame_id != vk_frame_id() ? 0 : 2]++;
            out_list_free(&l);
            if (rmap && rmap != local) free(rmap);
            land_reason(WB_DRAW);
            return 0;
        }
        match = newest;
        count = 1;
        before = newest->frame_id != vk_frame_id();
    } else
        for (u32 i = 0; i < l.n; i++) { /* not inside the tail: l holds what the search found */
            GfxOut* o = l.v[i];
            if (!out_meets(o, gva, len, &q, &rmap, local)) continue;
            if (!out_still_copyable(o, gva, len, &rmap, local)) {
                if (copy_stats_on()) g_wb_draw_miss[o->frame_id != vk_frame_id() ? 0 : 2]++;
                out_list_free(&l);
                if (rmap && rmap != local) free(rmap);
                land_reason(WB_DRAW);
                return 0;
            }
            match = o;
            count++;
            before |= o->frame_id != vk_frame_id();
        }
    out_list_free(&l);
    if (rmap && rmap != local) free(rmap);
    if (!count) return 0;
    if (before) vk_frame_hold(vk_frame_id() - 1);
    if (count == 1 && gva >= match->addr && gva + len <= match->addr + match->size &&
        !((match->off + (gva - match->addr)) & (align - 1)) &&
        /* a logged result, whose original bytes are gone, is written in place only by a writer that logs too: one
           that doesn't composes a result of its own over it. One whose input was not copied (a write-only binding)
           has no guest memory in its staging either, so a reader of it composes too */
        (match->org || !match->no_input) && (!writable || match->org || g_bind_log) &&
        (!writable || match->frame == vk_submit_number() || out_take_into_submit(match))) {
        /* Maxwell bindings retain conservative read/write ordering. NVN permits overlapping draws and supplies
           storage dependencies through explicit barriers; a writable view alone does not create a dependency.
           Read-only consumers and new submissions retain their automatic visibility checks. */
        u64 work = g_vk_drawn + g_vk_dispatches;
        int new_write = match->written > g_buffer.order.visible;
        /* A writable binding below stamps the next generation before its draw is recorded. Later bindings of
           this same draw need no second barrier. After commit, work advances and its consumers see the stamp.
           An aborted setup can leave a future stamp, which only causes an extra barrier, never a missing one. */
        if (g_buffer.order.submit != vk_submit_number() ||
            (((writable && (ordered || g_buffer.order.required)) || (!writable && new_write)) &&
             (g_buffer.order.work != work || g_buffer.order.visible != g_buffer.order.writes || g_buffer.order.required))) {
            vk_render_end();
            copy_barrier(out_stages(), out_access(),
                         VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
            g_buffer.order.submit = vk_submit_number();
            g_buffer.order.work = work;
            g_buffer.order.visible = g_buffer.order.writes;
            g_buffer.order.read_pending = 0;
            g_buffer.order.required = 0;
            if (copy_stats_on()) {
                g_buffer.order.barriers++;
                g_buffer.order.counted_work = work;
            }
        } else if (copy_stats_on() && g_buffer.order.counted_work != work) {
            /* Count omitted barriers once per consumer, excluding bindings the old per-work check skipped too. */
            if (writable && !ordered) g_buffer.order.parallel_writes++;
            else if (!writable) g_buffer.order.read_reuses++;
            g_buffer.order.counted_work = work;
        }
        if (writable) {
            match->written = g_buffer.order.writes + 1;
            g_buffer.order.reused_write = 1;
            g_src_match = match;
            if (match->keep) vk_own_res_wrote(match->keep); /* a landing waits for this submission's draw */
        } else {
            /* A later in-place writer must wait for this read at the front end's execution-order point.
               An abandoned setup may leave the flag set, which only adds a conservative barrier. */
            g_buffer.order.read_pending = 1;
        }
        *buf = match->buf;
        *off = match->off + (gva - match->addr);
        /* read (or written) where its owner is, maybe in a later submission than its own, still running when the
           result is done: the owner's next draw in place orders itself after it (keep_new) */
        if (match->keep) match->keep->read = ++g_buffer.order.owner_reads;
        if (copy_stats_on()) g_wb_reuses++;
        return 1;
    }
    if (copy_stats_on()) g_wb_draw_miss[count > 1 ? 1 : 3]++; /* composed: several results, or an offset or submit */
    return 2;
}
typedef struct {
    u64 gva;
} GapRead;
static void gap_read(void* user, u8* to, u64 a, u64 b) {
    const GapRead* g = user;
    read_untracked(g->gva + a, to, b - a);
}
/* the range for reading on the GPU, with its pending results composed over guest memory in device memory: the draws
   that bind it read it there. In staging, the GPU's copies of the results went out over PCIe and every read came back
   over it: about 45 MB a frame of composed storage ranges in one game, most of its GPU-to-host traffic */
static int gfx_composed(u64 gva, u64 len, VkBuffer* buf, u64* off) {
    if (!vk_stage_device(len, vk_stage_align(), buf, off)) return 0;
    GapRead g = {gva};
    return gfx_overlay(gva, len, *buf, *off, NULL, gap_read, &g);
}

/* written-page tracking (plat_ww_scan): a generation per 4K page of guest memory, bumped when a scan finds it
   written. A cache entry remembers the clock when it last matched memory, and is stale only if a page in its range
   has a newer generation. Scanning re-protects what it reports, and the shared table keeps that from hiding a change
   from another entry over the same memory. The table has a leaf of 1024 pages per 4 MB that has been written, with
   the newest generation in the leaf beside it so whole leaves are skipped */
#define WW_LEAF_SHIFT 22
static u32* g_wleaf[AS_TOP >> WW_LEAF_SHIFT];
static u32 g_wleaf_max[AS_TOP >> WW_LEAF_SHIFT];
/* The CPU writes a GPU stream may see come before its submission or the end of one of its waits: each of those starts
   a write epoch (vk_ww_new_epoch). A leaf scanned whole in an epoch isn't scanned again in it, and a scan covers whole
   leaves, so the textures and buffers checked in one epoch share a few scans: one game made 5,600 of them a frame.
   g_wleaf_seen holds the epoch + 1 whose scans of the leaf its table has taken in (checks only, under vk_lock) */
static u32 g_wleaf_seen[AS_TOP >> WW_LEAF_SHIFT];
static u32 g_ww_epoch; /* atomic */
static u32 g_wclock = 1;
static int g_ww = -1; /* -1 not tried yet, 0 not available, 1 on */
static void ww_mark(u8* p, u64 n) {
    u64 o = (u64)(p - g_mem);
    if (o >= AS_SIZE || !n) return;
    for (u64 pg = o >> 12, e = (o + n - 1) >> 12; pg <= e && pg < (AS_SIZE >> 12); pg++) {
        u64 l = pg >> (WW_LEAF_SHIFT - 12);
        if (!g_wleaf[l]) {
            /* a leaf without a table stands for all its pages at its newest generation (ww_mark_range, or a table
               that couldn't be allocated): a new table starts them there */
            u32* t = calloc(1u << (WW_LEAF_SHIFT - 12), 4);
            if (t && g_wleaf_max[l])
                for (u32 i = 0; i < 1u << (WW_LEAF_SHIFT - 12); i++) t[i] = g_wleaf_max[l];
            g_wleaf[l] = t;
        }
        if (g_wleaf[l]) g_wleaf[l][pg & ((1u << (WW_LEAF_SHIFT - 12)) - 1)] = g_wclock;
        g_wleaf_max[l] = g_wclock;
    }
}
/* a range of guest memory [a, e) */
typedef struct {
    u64 a, e;
} WwRange;
/* the pages of guest memory [o, e) as written now. A leaf the range covers whole, and that has no table, only gets
   its newest generation: kernel/mem.c can unmap gigabytes at once */
static void ww_mark_pages(u64 o, u64 e) {
    const u64 lp = 1ull << (WW_LEAF_SHIFT - 12);
    for (u64 pg = o >> 12, pe = (e + 4095) >> 12; pg < pe;) {
        u64 l = pg >> (WW_LEAF_SHIFT - 12), lend = (l + 1) * lp, end = pe < lend ? pe : lend;
        if (!g_wleaf[l] && pg == l * lp && end == lend)
            g_wleaf_max[l] = g_wclock;
        else
            ww_mark(GP(pg << 12), (end - pg) << 12);
        pg = end;
    }
}
/* Pages with two addresses (a view and its source, mem_alias_peers): what a check marks written is noted, and once
   the scans' locks are gone (ww_take_peers) its pages are marked at their other addresses too, so a write through
   either address is news to a cache over the other. Under vk_lock; when full, the last range grows over the next,
   marking more than was written */
#define WW_PEER_N 4096
static struct {
    WwRange r[WW_PEER_N];
    int n;
} g_wpeer;
static void ww_mark_range(u64 o, u64 e) {
    if (o < e) {
        if (g_wpeer.n && g_wpeer.r[g_wpeer.n - 1].e == o)
            g_wpeer.r[g_wpeer.n - 1].e = e;
        else if (g_wpeer.n < WW_PEER_N)
            g_wpeer.r[g_wpeer.n++] = (WwRange){o, e};
        else {
            WwRange* r = &g_wpeer.r[WW_PEER_N - 1];
            if (o < r->a) r->a = o;
            if (e > r->e) r->e = e;
        }
    }
    ww_mark_pages(o, e);
}

/* ranges of guest memory changed without a write the tracking sees (vk_mem_changed: decommitted pages, which read
   as zero now). Noted on the game's threads, outside vk_lock, and marked written by the next vk_written_since, as if
   its scan had found them. When the list is full its last range grows to cover the new one, marking more than
   changed: a cache over the rest only checks its contents once */
#define WW_CHG_N 32
static WwRange g_wchg[WW_CHG_N];
static int g_wchg_n; /* atomic: read without the lock to skip it */
static PMutex g_wchg_lock = PMUTEX_INIT;
void vk_mem_changed(u64 cpu, u64 len) {
    if (cpu >= AS_SIZE || !len) return;
    u64 e = len > AS_SIZE - cpu ? AS_SIZE : cpu + len;
    pm_lock(&g_wchg_lock);
    int n = __atomic_load_n(&g_wchg_n, __ATOMIC_RELAXED);
    if (n < WW_CHG_N) {
        g_wchg[n] = (WwRange){cpu, e};
        __atomic_store_n(&g_wchg_n, n + 1, __ATOMIC_RELEASE);
    } else {
        WwRange* r = &g_wchg[WW_CHG_N - 1];
        if (cpu < r->a) r->a = cpu;
        if (e > r->e) r->e = e;
    }
    pm_unlock(&g_wchg_lock);
}
static void ww_take_changed(void) {
    if (!__atomic_load_n(&g_wchg_n, __ATOMIC_ACQUIRE)) return;
    WwRange c[WW_CHG_N];
    pm_lock(&g_wchg_lock);
    int n = __atomic_load_n(&g_wchg_n, __ATOMIC_RELAXED);
    memcpy(c, g_wchg, (size_t)n * sizeof *c);
    __atomic_store_n(&g_wchg_n, 0, __ATOMIC_RELAXED);
    pm_unlock(&g_wchg_lock);
    if (n) g_wclock++; /* what it marks is newer than every baseline taken before (vk_written_since) */
    for (int i = 0; i < n; i++) ww_mark_range(c[i].a, c[i].e);
}

/* Scans, on the scanner thread as soon as an epoch starts and on the checks' thread for what it hasn't scanned yet.
   A scan of a 4 MB leaf takes about 9 us in one game, 2.4 k cycles of each draw's setup on the GPU thread when the
   checks did them all. A scan claims its leaves first (g_wleaf_busy), so one leaf is never scanned twice at once, and
   the pages it finds written are in g_wres before the leaves' g_wleaf_done say they were scanned and their claims
   end: a page a scan re-protects can't be missed by a check that scans the same leaf. The next check marks them
   (ww_take_scanned). g_wleaf_done holds the epoch + 1 of the leaf's newest scan, read before it started, and
   g_wleaf_used the epoch + 1 a check last needed the leaf in. The scanner only tries claims, and a check waits only
   for a leaf the scanner is scanning right then: a lock over all scans had checks waiting out whole passes, 0.5 ms
   each */
#define WW_RES_N       4096
#define WW_RUN_MAX     8 /* leaves the scanner scans in one call at most */
#define WW_USED_EPOCHS 4 /* the scanner scans the leaves the checks of the last epochs needed */
static u32 g_wleaf_done[AS_TOP >> WW_LEAF_SHIFT]; /* atomic, stored by the leaf's claimer */
static u32 g_wleaf_used[AS_TOP >> WW_LEAF_SHIFT]; /* atomic: only a hint for the scanner */
/* The leaves a check ever needed, a bit each, with a bit for each word that has one. A game's few used leaves can lie
   far apart: one had 15 under a g_wleaf_top of 18090, and passes that tested every leaf below it were a sixth of the
   samples of the runtime. Set by the checks under vk_lock, and read by the scanner as a hint like g_wleaf_used */
#define WW_EVER_WORDS ((AS_TOP >> WW_LEAF_SHIFT) / 64)
#define WW_EVER_TOPS  (WW_EVER_WORDS / 64)
static u64 g_wleaf_ever[WW_EVER_WORDS];    /* atomic */
static u64 g_wleaf_ever_top[WW_EVER_TOPS]; /* atomic */
static u8 g_wleaf_busy[AS_TOP >> WW_LEAF_SHIFT];  /* atomic: 1 while a scan has claimed the leaf */
static u32 g_ww_used[WW_USED_EPOCHS];             /* the last epochs + 1 checks needed leaves in, under vk_lock */
static u32 g_ww_used_i;
static u32 g_ww_used_from; /* atomic: the oldest of them */
/* one past the highest leaf a check has needed, where the scanner's pass ends: a leaf above it was never needed, and a
   pass to AS_SIZE's last leaf read 131072 of them at 512 GiB (atomic, stored by the checks under vk_lock) */
static u64 g_wleaf_top;
/* a scan's written pages before they go to g_wres: the scanner's, and the checks' (under vk_lock) */
struct WwStage {
    WwRange r[WW_RES_N];
    int n;
};
static WwStage g_wstage_scanner, g_wstage_check;
static WwRange g_wres[WW_RES_N]; /* what scans found written, for the next check to mark */
static int g_wres_n;             /* atomic: read without the lock to skip it */
static PMutex g_wres_lk = PMUTEX_INIT; /* after a leaf's claim */
static int g_wscan_failed; /* atomic */
static int g_wscan_on;     /* atomic: the scanner runs */
static PMutex g_wscan_wlk = PMUTEX_INIT;
static PCond g_wscan_cv;
/* plat_ww_scan's callback: a run of written pages. When the stage is full its last range grows over the new one
   (scans go up through memory), marking more than was written: a cache over the rest only checks its contents once */
static void ww_note(u8* p, u64 n) {
    WwStage* st = t_buffer.wstage;
    u64 a = (u64)(p - g_mem);
    if (a >= AS_SIZE || !n) return;
    if (st->n && st->r[st->n - 1].e == a)
        st->r[st->n - 1].e = a + n;
    else if (st->n < WW_RES_N)
        st->r[st->n++] = (WwRange){a, a + n};
    else
        st->r[WW_RES_N - 1].e = a + n;
}
static int ww_claim(u64 l) {
    u8 free = 0;
    return __atomic_compare_exchange_n(&g_wleaf_busy[l], &free, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
/* guest memory [a, e) scanned, but for the pages an import of unified memory holds: the driver takes them again at
   each submission, writable, which a scan would report as the CPU's writes, and after a scan protected them it faults
   every one of them in again (uma.c). vk_written_since can't tell there */
static int ww_scan_free(u64 a, u64 e) {
    int r = 0;
    while (a < e && !r) {
        u64 hi, lo = vk_uma_import_in(a, e, &hi);
        if (lo > a) r = plat_ww_scan(GP(a), lo - a, ww_note);
        a = hi;
    }
    return r;
}
/* what a scan put in st, in g_wres for the next check to mark, before its leaves' claims end */
static void ww_publish(WwStage* st) {
    pm_lock(&g_wres_lk);
    int n = g_wres_n;
    for (int i = 0; i < st->n; i++)
        if (n < WW_RES_N)
            g_wres[n++] = st->r[i];
        else { /* full: the last range covers this one too */
            WwRange* w = &g_wres[WW_RES_N - 1];
            if (st->r[i].a < w->a) w->a = st->r[i].a;
            if (st->r[i].e > w->e) w->e = st->r[i].e;
        }
    __atomic_store_n(&g_wres_n, n, __ATOMIC_RELEASE);
    pm_unlock(&g_wres_lk);
    st->n = 0;
}
/* the scans since the last stats (vk_ww_stats), the scanner's [0] and the checks' [1]: leaves, and their time */
static u64 g_wscan_leaves[2], g_wscan_ns[2]; /* atomic */
static u64 g_wscan_skipped; /* the leaves a check claimed that the scanner had scanned in their epoch already */
/* scans leaves [lo, hi], claimed by the caller, in the epoch seen - 1, and ends the claims: 0, or -1 when tracking
   failed */
static int ww_scan_leaves(u64 lo, u64 hi, u32 seen, WwStage* st) {
    t_buffer.wstage = st;
    u64 t0 = plat_ns();
    int r = ww_scan_free(lo << WW_LEAF_SHIFT, (hi + 1) << WW_LEAF_SHIFT);
    __atomic_add_fetch(&g_wscan_leaves[st != &g_wstage_scanner], hi - lo + 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_wscan_ns[st != &g_wstage_scanner], plat_ns() - t0, __ATOMIC_RELAXED);
    ww_publish(st);
    if (r < 0) __atomic_store_n(&g_wscan_failed, 1, __ATOMIC_RELEASE);
    for (u64 l = lo; l <= hi; l++) {
        if (r == 0) __atomic_store_n(&g_wleaf_done[l], seen, __ATOMIC_RELEASE);
        __atomic_store_n(&g_wleaf_busy[l], 0, __ATOMIC_RELEASE);
    }
    return r < 0 ? -1 : 0;
}
/* Pages a landing wrote (own.c, docs/layers/gpu/ownership.md), with their bytes as it left them. The ownership lets go of
   them after, which watches them for writes again, and a page just watched reads as written: the next scan reports
   all of them, whatever happened to them. The landing is the GPU's write, counted when its result was published
   (vk_own_publish's gpu_mem_written_cpu) and older than anything drawn over those bytes since. Taken for a CPU write,
   newer than every baseline, it had targets drawn there after the publish give their pixels up for memory's older
   bytes: one game's reflection array, drawn whole over the memory of the previous one, kept seven layers of it, NaN
   included. So a report of such a page counts only when its bytes are not the landing's any more. They are read
   through the pool's mirror, which no trap covers, or in place where there is none (landed_unchanged), and the entry
   goes with the report: the scan protected the page */
typedef struct {
    u64 page, hash;  /* the 4K page's number, and its bytes' vk_hash after the landing */
    const u8* bytes; /* its place in the mirror */
    u32 seq;         /* the newest of a page's entries wins */
} WwLanded;
#define WW_LANDED_MAX (1u << 18) /* pages (1 GB): past it the reports count as they are */
static WwLanded* g_wland;
static u32 g_wland_n, g_wland_cap, g_wland_seq;
/* g_wland is sorted (landed_order) up to g_wland_sorted, the entries after it added since, in their order. Sorting
   all of them again for each landing's few, with the qsort of the C library through a function pointer, was a
   fourteenth of one game's GPU thread: the new ones are sorted alone and merged in (landed_sort) */
static u32 g_wland_sorted;
static WwLanded* g_wland_tmp; /* the merge's copy of the new entries */
static u32 g_wland_tmp_cap;
static PMutex g_wland_lk = PMUTEX_INIT; /* after g_wres_lk */
void vk_ww_landed(u64 cpu, u64 size, const u8* bytes) {
    if (!bytes || cpu >= AS_SIZE || size > AS_SIZE - cpu) return;
    pm_lock(&g_wland_lk);
    for (u64 o = 0; o + 4096 <= size; o += 4096) {
        if (g_wland_n == g_wland_cap) {
            u32 cap = g_wland_cap ? g_wland_cap * 2 : 1024;
            WwLanded* n = cap <= WW_LANDED_MAX ? (WwLanded*)realloc(g_wland, (size_t)cap * sizeof *n) : NULL;
            if (!n) break; /* those pages' reports count */
            g_wland = n;
            g_wland_cap = cap;
        }
        g_wland[g_wland_n++] = (WwLanded){(cpu + o) >> 12, vk_hash(bytes + o, 4096), bytes + o, g_wland_seq++};
    }
    pm_unlock(&g_wland_lk);
}
static int landed_order(const void* a, const void* b) {
    const WwLanded *x = (const WwLanded*)a, *y = (const WwLanded*)b;
    if (x->page != y->page) return x->page < y->page ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}
/* g_wland by page, a page's newest entry alone: the entries added since the last sort sorted (by page, then by their
   order, which is newer than every sorted entry's) and merged into the sorted ones from the end, then a page's older
   entries dropped from where the merge began */
static void landed_sort(void) {
    u32 s = g_wland_sorted, k = g_wland_n - s, n, from;
    if (!k) return;
    qsort(g_wland + s, k, sizeof *g_wland, landed_order);
    from = s;
    if (s && g_wland[s - 1].page >= g_wland[s].page) {
        if (g_wland_tmp_cap < k) {
            WwLanded* t = (WwLanded*)realloc(g_wland_tmp, (size_t)k * sizeof *t);
            if (!t) { /* all of them sorted again, as before */
                qsort(g_wland, g_wland_n, sizeof *g_wland, landed_order);
                from = 0;
                goto dedup;
            }
            g_wland_tmp = t;
            g_wland_tmp_cap = k;
        }
        memcpy(g_wland_tmp, g_wland + s, (size_t)k * sizeof *g_wland);
        i64 i = (i64)s - 1, j = (i64)k - 1, w = (i64)g_wland_n - 1;
        while (j >= 0) /* a new entry of a page goes after its sorted ones: its seq is newer */
            if (i >= 0 && g_wland[i].page > g_wland_tmp[j].page)
                g_wland[w--] = g_wland[i--];
            else
                g_wland[w--] = g_wland_tmp[j--];
        from = (u32)(i + 1); /* below it nothing moved */
    }
dedup:
    n = from ? from - 1 : 0; /* the entry before where the merge began may have the same page as the first after it */
    for (u32 i = n; i < g_wland_n; i++) {
        if (n && g_wland[n - 1].page == g_wland[i].page) n--;
        g_wland[n++] = g_wland[i];
    }
    g_wland_n = n;
    g_wland_sorted = n;
}
u32 vk_ww_landed_check(void) {
    pm_lock(&g_wland_lk);
    landed_sort();
    u32 n = g_wland_n;
    for (u32 i = 1; i < g_wland_n; i++)
        if (g_wland[i - 1].page >= g_wland[i].page) n = ~0u;
    pm_unlock(&g_wland_lk);
    return n;
}
u64 vk_ww_landed_hash(u64 cpu) {
    u64 h = 0;
    pm_lock(&g_wland_lk);
    landed_sort();
    for (u32 i = 0; i < g_wland_n; i++)
        if (g_wland[i].page == cpu >> 12) h = g_wland[i].hash;
    pm_unlock(&g_wland_lk);
    return h;
}
/* whether a landed page still holds the landing's bytes. Where traps land in place (no mirror: Windows) they are the
   page's own, read without faulting: an owner may hold it again, or mem.c have given it back, and its report counts */
static int landed_unchanged(const WwLanded* l) {
    u8 page[4096];
    const u8* bytes = l->bytes;
    if (!plat_alias_mirror()) {
        if (plat_vm_read(GP(l->page << 12), page, sizeof page)) return 0;
        bytes = page;
    }
    return vk_hash(bytes, 4096) == l->hash;
}
/* what a scan found written, [o, e), marked but for the landed pages on it that still hold the landing's bytes. Every
   entry on it goes (no bytes until ww_take_scanned drops them, from the first, g_wland_gone) */
static u32 g_wland_gone;
static void ww_mark_scanned(u64 o, u64 e) {
    u32 lo = 0, hi = g_wland_n;
    u64 at = o;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (g_wland[mid].page < o >> 12)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (u32 i = lo; i < g_wland_n && g_wland[i].page << 12 < e; i++) {
        u64 a = g_wland[i].page << 12;
        if (!g_wland[i].bytes) continue;
        if (a + 4096 <= e && landed_unchanged(&g_wland[i])) {
            if (a > at) ww_mark_range(at, a);
            at = a + 4096;
        }
        g_wland[i].bytes = NULL;
        if (i < g_wland_gone) g_wland_gone = i;
    }
    if (at < e) ww_mark_range(at, e);
}
/* marks what scans found written, newer than every baseline taken before: those were taken before the scans
   re-protected the pages, and their caches may have read memory before the writes */
static void ww_take_scanned(void) {
    if (!__atomic_load_n(&g_wres_n, __ATOMIC_ACQUIRE)) return;
    pm_lock(&g_wres_lk);
    if (g_wres_n) g_wclock++;
    pm_lock(&g_wland_lk);
    if (g_wland_n) {
        landed_sort();
        g_wland_gone = g_wland_n;
        for (int i = 0; i < g_wres_n; i++) ww_mark_scanned(g_wres[i].a, g_wres[i].e);
        u32 n = g_wland_gone; /* the entries before the first that went stay where they are */
        for (u32 i = n; i < g_wland_n; i++)
            if (g_wland[i].bytes) g_wland[n++] = g_wland[i];
        g_wland_n = g_wland_sorted = n;
    } else
        for (int i = 0; i < g_wres_n; i++) ww_mark_range(g_wres[i].a, g_wres[i].e);
    pm_unlock(&g_wland_lk);
    __atomic_store_n(&g_wres_n, 0, __ATOMIC_RELAXED);
    pm_unlock(&g_wres_lk);
}
/* whether the scanner scans leaf l for the epoch seen - 1: a check of the last epochs needed it, and it wasn't scanned
   in this epoch or a later one */
static int ww_wanted(u64 l, u32 seen, u32 from) {
    u32 u = __atomic_load_n(&g_wleaf_used[l], __ATOMIC_RELAXED);
    return u && (i32)(u - from) >= 0 && (i32)(__atomic_load_n(&g_wleaf_done[l], __ATOMIC_ACQUIRE) - seen) < 0;
}
/* the first leaf at or after l that a check ever needed; n or more when there is none below n */
static u64 ww_next_ever(u64 l, u64 n) {
    while (l < n) {
        u64 w = l >> 6, bits = __atomic_load_n(&g_wleaf_ever[w], __ATOMIC_RELAXED) >> (l & 63);
        if (bits) return l + snx_ctz64(bits);
        w++;
        u64 t = w >> 6;
        if (t >= WW_EVER_TOPS) return n;
        u64 words = __atomic_load_n(&g_wleaf_ever_top[t], __ATOMIC_RELAXED) >> (w & 63);
        while (!words) {
            if (++t >= WW_EVER_TOPS) return n;
            w = t << 6;
            words = __atomic_load_n(&g_wleaf_ever_top[t], __ATOMIC_RELAXED);
        }
        l = (w + snx_ctz64(words)) << 6;
    }
    return n;
}
/* Scans the leaves the checks of the last epochs needed each time an epoch starts, up through memory, and starts over
   for a newer epoch. A check that comes before the scanner got to its leaf scans it itself */
static void ww_scanner(void* arg) {
    u32 last = 0;
    (void)arg;
    SNX_THREAD_NAME("Vulkan write scans");
    for (;;) {
        u32 e, seen, from;
        pm_lock(&g_wscan_wlk);
        while ((e = __atomic_load_n(&g_ww_epoch, __ATOMIC_ACQUIRE)) == last) pc_wait(&g_wscan_cv, &g_wscan_wlk, -1);
        pm_unlock(&g_wscan_wlk);
        last = e;
        seen = e + 1;
        from = __atomic_load_n(&g_ww_used_from, __ATOMIC_RELAXED);
        for (u64 l = 0, n = __atomic_load_n(&g_wleaf_top, __ATOMIC_RELAXED); l < n;) {
            if (!ww_wanted(l, seen, from)) {
                l = ww_next_ever(l + 1, n);
                continue;
            }
            u64 lo = l;
            while (l < n && l - lo < WW_RUN_MAX && ww_wanted(l, seen, from) && ww_claim(l)) l++;
            if (l == lo) { /* a check is scanning it */
                l++;
                continue;
            }
            if (ww_scan_leaves(lo, l - 1, seen, &g_wstage_scanner) < 0) return;
            if (__atomic_load_n(&g_ww_epoch, __ATOMIC_RELAXED) != e) break; /* a newer one: start over for it */
        }
    }
}
void vk_ww_new_epoch(void) {
    u32 e = __atomic_add_fetch(&g_ww_epoch, 1, __ATOMIC_RELEASE);
    if (t_buffer.ww_streaming)
        t_buffer.ww_stream = e; /* a wait of the running job ended: what the CPU wrote before is its to see */
    if (__atomic_load_n(&g_wscan_on, __ATOMIC_ACQUIRE)) {
        pm_lock(&g_wscan_wlk);
        pc_signal(&g_wscan_cv);
        pm_unlock(&g_wscan_wlk);
    }
}
void vk_ww_seen(void) {
    if (t_buffer.ww_streaming) t_buffer.ww_stream = __atomic_load_n(&g_ww_epoch, __ATOMIC_ACQUIRE);
}
u32 vk_ww_epoch(void) {
    return t_buffer.ww_streaming ? t_buffer.ww_stream : __atomic_load_n(&g_ww_epoch, __ATOMIC_ACQUIRE);
}
void vk_ww_stream(int on) {
    t_buffer.ww_streaming = on;
    t_buffer.ww_stream = __atomic_load_n(&g_ww_epoch, __ATOMIC_ACQUIRE);
}
/* a check in the epoch seen - 1 needs leaves: the scanner's next passes scan what the last epochs needed */
static void ww_used_epoch(u32 seen) {
    u32 cur = g_ww_used[g_ww_used_i % WW_USED_EPOCHS];
    if (cur && (i32)(seen - cur) <= 0) return; /* this one, or an older one's check (another thread's) */
    g_ww_used[++g_ww_used_i % WW_USED_EPOCHS] = seen;
    u32 oldest = g_ww_used[(g_ww_used_i + 1) % WW_USED_EPOCHS];
    __atomic_store_n(&g_ww_used_from, oldest ? oldest : seen, __ATOMIC_RELAXED);
}

/* the leaves [lo, hi] taken in for the epoch seen - 1: those not taken in yet scanned in it (by the scanner already,
   or here) and what the scans found marked. 0, or -1 when the tracking failed (under vk_lock) */
static int ww_take_leaves(u64 lo, u64 hi, u32 seen) {
    u64 l, a;
    for (l = lo; l <= hi && (i32)(g_wleaf_seen[l] - seen) >= 0; l++) {}
    if (l > hi) return 0;
    u64 first = hi + 1, last = 0;
    ww_used_epoch(seen);
    if (__atomic_load_n(&g_wleaf_top, __ATOMIC_RELAXED) <= hi) __atomic_store_n(&g_wleaf_top, hi + 1, __ATOMIC_RELAXED);
    for (a = l; a <= hi; a++) {
        if ((i32)(g_wleaf_seen[a] - seen) >= 0) continue;
        __atomic_store_n(&g_wleaf_used[a], seen, __ATOMIC_RELAXED);
        if (!(__atomic_load_n(&g_wleaf_ever[a >> 6], __ATOMIC_RELAXED) >> (a & 63) & 1)) {
            __atomic_fetch_or(&g_wleaf_ever[a >> 6], 1ull << (a & 63), __ATOMIC_RELAXED);
            __atomic_fetch_or(&g_wleaf_ever_top[a >> 12], 1ull << (a >> 6 & 63), __ATOMIC_RELAXED);
        }
        if ((i32)(__atomic_load_n(&g_wleaf_done[a], __ATOMIC_ACQUIRE) - seen) < 0) {
            if (first > hi) first = a;
            last = a;
        }
    }
    if (first <= hi) {
        /* the scanner may be scanning some of them: wait for those, which it may have scanned in this epoch */
        for (a = first; a <= last; a++)
            while (!ww_claim(a)) plat_yield();
        /* the runs of them not scanned in this epoch, each scanned once: one the scanner did between two the check
           needs is let go, as one at either end is */
        for (a = first; a <= last;) {
            if ((i32)(__atomic_load_n(&g_wleaf_done[a], __ATOMIC_RELAXED) - seen) >= 0) {
                __atomic_store_n(&g_wleaf_busy[a], 0, __ATOMIC_RELEASE);
                g_wscan_skipped++;
                a++;
                continue;
            }
            u64 run = a;
            while (a <= last && (i32)(__atomic_load_n(&g_wleaf_done[a], __ATOMIC_RELAXED) - seen) < 0) a++;
            if (ww_scan_leaves(run, a - 1, seen, &g_wstage_check) < 0) {
                for (; a <= last; a++) __atomic_store_n(&g_wleaf_busy[a], 0, __ATOMIC_RELEASE);
                return -1;
            }
        }
    }
    ww_take_scanned();
    for (a = l; a <= hi; a++)
        if ((i32)(g_wleaf_seen[a] - seen) < 0) g_wleaf_seen[a] = seen;
    return 0;
}
/* the other addresses of a range's pages (mem_alias_peers), noted to be taken in after the memory lock goes; past
   WW_PEERS of them the range's answer is -1 (the caches compare contents) */
#define WW_PEERS 64
typedef struct {
    WwRange r[WW_PEERS];
    int n, over;
} WwPeers;
static void ww_peer_note(void* user, u64 at, u64 n) {
    WwPeers* p = (WwPeers*)user;
    if (p->n < WW_PEERS)
        p->r[p->n++] = (WwRange){at, at + n};
    else
        p->over = 1;
}
static void ww_peer_mark(void* user, u64 at, u64 n) {
    (void)user;
    ww_mark_pages(at, at + n);
}
/* what this check marked written, marked at the pages' other addresses too (the scans' locks gone: mem.c's lock comes
   before them, as a landing under it notes its pages) */
static void ww_take_peers(void) {
    int n = g_wpeer.n;
    g_wpeer.n = 0;
    if (!n || !mem_alias_any()) return; /* read after the takes: a view made before a write its scan found counts */
    for (int i = 0; i < n; i++) mem_alias_peers(g_wpeer.r[i].a, g_wpeer.r[i].e - g_wpeer.r[i].a, ww_peer_mark, NULL);
}

int vk_written_since(const u8* host, u64 len, u32 since) {
    if (!g_ww || !host || !len) return -1;
    if (g_ww < 0) {
        g_ww = mem_ww_probe(ww_mark) == 0;
        LOG("vulkan: %s\n",
            g_ww ? "write tracking on (caches skip unchanged memory)" : "no write tracking (caches check contents)");
        if (!g_ww) return -1;
        pc_init(&g_wscan_cv);
        if (!plat_thread(ww_scanner, NULL, (size_t)1 << 20)) __atomic_store_n(&g_wscan_on, 1, __ATOMIC_RELEASE);
    }
    if (__atomic_load_n(&g_wscan_failed, __ATOMIC_ACQUIRE)) {
        g_ww = 0;
        LOG("vulkan: write tracking failed, caches check contents\n");
        return -1;
    }
    /* Existing cache baselines cannot be ordered against a wrapped clock. Fall back
       to content checks permanently before consuming any dirty pages at wrap. */
    if (g_wclock == UINT32_MAX) {
        g_ww = 0;
        return -1;
    }
    u64 o = (u64)(host - g_mem), lo = o >> WW_LEAF_SHIFT, hi = (o + len - 1) >> WW_LEAF_SHIFT, end;
    if (vk_uma_import_in(o, o + len, &end) < o + len) return -1; /* pages no scan watches (ww_scan_free) */
    u32 seen = vk_ww_epoch() + 1;
    WwPeers peers;
    peers.n = peers.over = 0;
    /* the clock moves only when this call marks pages, in ww_take_changed and ww_take_scanned when they take ranges,
       so what it marks is newer than every baseline taken before: a query that marks nothing, most of them, leaves
       it, and it doesn't reach its wrap within a session at thousands of queries a frame */
    ww_take_changed();
    int r = ww_take_leaves(lo, hi, seen);
    /* pages of the range with another address (a view's): that address's leaves taken in too, so a write through it is
       found, and marked here below */
    if (!r) mem_alias_peers(o, len, ww_peer_note, &peers);
    for (int i = 0; !r && i < peers.n; i++)
        if (vk_uma_import_in(peers.r[i].a, peers.r[i].e, &end) < peers.r[i].e)
            peers.over = 1; /* written through pages no scan watches */
        else
            r = ww_take_leaves(peers.r[i].a >> WW_LEAF_SHIFT, (peers.r[i].e - 1) >> WW_LEAF_SHIFT, seen);
    ww_take_peers();
    if (r < 0) {
        g_ww = 0;
        LOG("vulkan: write tracking failed, caches check contents\n");
        return -1;
    }
    if (peers.over) return -1;
    for (u64 pg = o >> 12, e = (o + len - 1) >> 12; pg <= e;) {
        u64 l = pg >> (WW_LEAF_SHIFT - 12), lend = (l + 1) << (WW_LEAF_SHIFT - 12);
        if (g_wleaf_max[l] > since) {
            if (!g_wleaf[l]) return 1; /* the whole leaf was marked */
            for (u64 q = pg; q <= e && q < lend; q++)
                if (g_wleaf[l][q & ((1u << (WW_LEAF_SHIFT - 12)) - 1)] > since) return 1;
        }
        pg = lend;
    }
    return 0;
}
u32 vk_ww_now(void) { return g_wclock; }
void vk_ww_stats(u32 presents) {
    static u32 epoch0;
    u32 e = __atomic_load_n(&g_ww_epoch, __ATOMIC_RELAXED);
    u64 l[2], ns[2];
    for (int i = 0; i < 2; i++) {
        l[i] = __atomic_exchange_n(&g_wscan_leaves[i], 0, __ATOMIC_RELAXED);
        ns[i] = __atomic_exchange_n(&g_wscan_ns[i], 0, __ATOMIC_RELAXED);
    }
    if (g_ww > 0 && presents)
        LOG("vulkan: write scans over %u presents: %.1f epochs a present; the scanner %.1f leaves of 4 MiB a present "
            "(%.2f ms), the checks %.1f (%.2f ms, and %.1f the scanner had)\n",
            presents, (double)(e - epoch0) / presents, (double)l[0] / presents, (double)ns[0] / 1e6 / presents,
            (double)l[1] / presents, (double)ns[1] / 1e6 / presents, (double)g_wscan_skipped / presents);
    epoch0 = e;
    g_wscan_skipped = 0;
}
/* the hot pages of guest memory [a, e) protected again and reported (plat_ww_cool), its leaves claimed as a check's
   scan claims them: 1 when it had any, which the next ww_take_scanned marks */
static int ww_cool_range(u64 a, u64 e) {
    u64 lo = a >> WW_LEAF_SHIFT, hi = (e - 1) >> WW_LEAF_SHIFT;
    if (!plat_ww_hot_in(GP(a), e - a)) return 0;
    for (u64 l = lo; l <= hi; l++)
        while (!ww_claim(l)) plat_yield();
    t_buffer.wstage = &g_wstage_check;
    plat_ww_cool(GP(a), e - a, ww_note);
    ww_publish(&g_wstage_check);
    for (u64 l = lo; l <= hi; l++) __atomic_store_n(&g_wleaf_busy[l], 0, __ATOMIC_RELEASE);
    return 1;
}
void vk_ww_cool(const u8* host, u64 len) {
    if (g_ww <= 0 || !host || !len) return;
    u64 o = (u64)(host - g_mem);
    if (o >= AS_SIZE) return;
    if (len > AS_SIZE - o) len = AS_SIZE - o;
    int cooled = ww_cool_range(o, o + len);
    if (mem_alias_any()) { /* a write through another address of the pages reads as one at these (ww_take_peers) */
        WwPeers at;
        at.n = at.over = 0;
        mem_alias_peers(o, len, ww_peer_note, &at);
        for (int i = 0; i < at.n; i++) cooled |= ww_cool_range(at.r[i].a, at.r[i].e);
    }
    if (!cooled) return;
    ww_take_scanned(); /* newer than every baseline before, and no newer than the one the caller takes next */
    ww_take_peers();
}
/* fn(user, at, n) for each run of [host, host + len) on pages written after since, at the range's byte offset at: for
   a range vk_written_since(host, len, since) has just found written, so its pages are marked */
static int page_written(u64 pg, u32 since) {
    u64 l = pg >> (WW_LEAF_SHIFT - 12);
    return g_wleaf_max[l] > since && (!g_wleaf[l] || g_wleaf[l][pg & ((1u << (WW_LEAF_SHIFT - 12)) - 1)] > since);
}
void vk_written_runs(const u8* host, u64 len, u32 since, void (*fn)(void*, u64, u64), void* user) {
    u64 o = (u64)(host - g_mem), first = o >> 12, last = (o + len - 1) >> 12, run = 0, pg;
    int in = 0;
    for (pg = first; pg <= last + 1; pg++) {
        int w = pg <= last && page_written(pg, since);
        if (w && !in) run = pg;
        if (!w && in) {
            u64 a = run << 12 > o ? (run << 12) - o : 0, b = pg << 12 < o + len ? (pg << 12) - o : len;
            fn(user, a, b - a);
        }
        in = w;
    }
}

static u8* g_tmp;
static u64 g_tmp_cap;
/* the guest bytes, without a copy when they are contiguous in host memory */
static const u8* guest_bytes(u64 gva, u64 len) {
    u8* p = gmem_ptr(gva, len);
    if (p) return p;
    if (g_tmp_cap < len) {
        u8* n = realloc(g_tmp, len);
        if (!n) return NULL;
        g_tmp = n;
        g_tmp_cap = len;
    }
    gmem_read(gva, g_tmp, len);
    return g_tmp;
}

/* the guest bytes a draw reads, with the samples-passed reports queued over them read past (vk_release_read_counts):
   their counts, *n of them, are for the GPU to write where the bytes go (vk_query_counts). Reading the bytes as they are
   waited for the GPU to run everything recorded until the reports */
static const u8* draw_bytes(u64 gva, u64 len, SamplesCount* counts, u32 max, u32* n) {
    *n = 0;
    if (__atomic_load_n(&g_gpu_releases, __ATOMIC_ACQUIRE) && len <= 0x10000) {
        if (g_tmp_cap < len) {
            u8* grown = realloc(g_tmp, len);
            if (!grown) return NULL;
            g_tmp = grown;
            g_tmp_cap = len;
        }
        if (vk_release_read_counts(gva, g_tmp, len, counts, max, n) > 0 && *n) return g_tmp;
        *n = 0;
    }
    return guest_bytes(gva, len);
}

int vk_buf_copy(u64 gva, u64 len, VkBuffer* buf, u64* off) {
    int src = gva ? gfx_gpu_src(gva, len, buf, off, 0, vk_stage_align(), 1) : 0;
    if (src == 1) return 1;
    if (src == 2) return gfx_composed(gva, len, buf, off);
    if (gva && vk_uma_pending(gva, len)) { /* written in place, the write pending: copied after it, on the GPU */
        u8* q = vk_stage(len, vk_stage_align(), buf, off);
        if (!q) return 0;
        vk_render_end();
        vk_guest_read(gva, len, q, *buf, *off);
        copy_note(gva, len);
        return 1;
    }
    u8* p = vk_stage(len ? len : 16, vk_stage_align(), buf, off);
    if (!p) return 0;
    if (!gva)
        memset(p, 0, len);
    else {
        const u8* src = guest_bytes(gva, len);
        if (!src) return 0;
        memcpy(p, src, len);
    }
    copy_note(gva, len);
    return 1;
}

static int range_owned(u64 gva, u64 len);
static int written_since_gva(u64 gva, u64 len, u32 since);
/* Keep an owned input on the GPU. Only the baseline needed to merge later CPU edits
   is copied to host-cached staging, asynchronously with the draw. */
static int writable_owned(GfxOut* o, u64 gva, u32 len, VkBuffer buf, u64 off) {
    VkBuffer back;
    u64 at;
    if (!o->org) { /* logged: only its input, on the GPU */
        vk_render_end();
        vk_guest_read(gva, len, o->ptr, buf, off);
        copy_barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        return 1;
    }
    o->initial = vk_stage_back(len, &back, &at, &o->initial_mem);
    if (!o->initial) return 0;
    vk_render_end();
    vk_guest_read(gva, len, o->ptr, buf, off);
    g_buffer.wstat.copied += len;
    g_buffer.wstat.read_back += len;
    copy_barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferCopy copy = {off, at, len};
    rec_CmdCopyBuffer(vk_cmd(), buf, back, 1, &copy);
    copy_barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    return 1;
}

/* A writable binding kept on the GPU, as a dispatch keeps the storage buffers it writes (compute.c res_bind): the
   range's bytes are GPU-owned from the bind on, by the owner holding all of them (one a draw, a dispatch or a copy
   engine left there) or by one made now from memory, and the draws write them there, in place. Nothing is copied on
   the CPU, read back or compared: the host's trap lands the owner when the CPU touches the bytes, after the
   submission that wrote it (stamped here, and at each draw that writes it again: gfx_gpu_src), so the CPU's access
   comes after the draws', as on the console once the GPU ran them. The copies, read back and merged, wrote a range's
   bytes twice on the CPU before its draw and compared all of them after it, whether or not the CPU read them. An
   owner draws read where it is since the last barrier from all work gets one before it is written, out of the pass.
   Kept: a host with traps, a range of whole pages in one run of guest memory (a page shared with the CPU's bytes
   would trap at each of their writes), the CPU not having made it land lately (one it reads every frame would wait
   for the GPU each time, which the copies never do) and no unified memory, where the shader writes guest memory
   itself (vk_uma_write). 1 with the owner's place in *buf, *off and the result in g_prep, 0 when the binding takes
   the copies */
#define KEEP_CPU_PRESENTS 1200 /* as long as compute.c's storage buffers stay off the GPU once the CPU read them */
static int keep_new(u64 gva, u32 len, u64 align, VkBuffer* buf, u64* off) {
    OwnRange r[2];
    u64 cpu = 0, at = 0;
    VkOwnRes* res;
    GfxOut* o;
    int why = KF_NONE;
    if (!own_traps())
        why = KF_TRAPS_OFF;
    else if (vk_uma())
        why = KF_UMA;
    else if (!len)
        why = KF_EMPTY;
    else if ((gva | len) & 0xFFF)
        why = KF_UNALIGNED;
    else if (gmmu_run(gva, len, &cpu) != len)
        why = KF_NOT_CONTIG;
    else if (vk_own_cpu_read(cpu, len, KEEP_CPU_PRESENTS))
        why = KF_CPU_READ;
    if (why) {
        g_keep_fail[why]++;
        return 0;
    }
    if (own_lookup(cpu, len, r, 2) == 1 && r[0].addr == cpu && r[0].size == len) {
        if (r[0].res_off & (align - 1)) return 0;
        res = (VkOwnRes*)r[0].res;
        at = r[0].res_off;
        if (res->read > g_buffer.order.owner_reads_ordered) { /* draws read it where it is, inside a pass maybe */
            vk_render_end();
            copy_barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                         VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0);
            g_buffer.order.owner_reads_ordered = g_buffer.order.owner_reads;
        }
        vk_own_res_ref(res);
        vk_own_res_wrote(res);
    } else {
        if (!(res = vk_own_res_new(len))) return 0;
        if (!vk_guest_read_device(gva, len, res->buf, 0)) { /* the CPU's bytes staged, the GPU's copied there */
            vk_own_res_unref(res);
            return 0;
        }
        vk_own_publish(cpu, len, res, 0);
        if (own_lookup(cpu, len, r, 2) != 1 || r[0].res != res || r[0].addr != cpu || r[0].size != len) {
            vk_own_res_unref(res); /* landed at once instead: memory has its bytes */
            return 0;
        }
        g_buffer.wstat.copied += len; /* the range staged once, as memory held it */
    }
    if (!(o = gfx_out_new(len >> 12, 0))) {
        vk_own_res_unref(res);
        return 0;
    }
    out_map(o, gva);
    o->keep = res;
    o->keep_off = at;
    o->addr = gva;
    o->size = len;
    o->buf = *buf = res->buf;
    o->off = *off = at;
    o->frame = vk_submit_number();
    o->frame_id = vk_frame_id();
    if (g_prep_tail)
        g_prep_tail->next = o;
    else
        g_prep = o;
    g_prep_tail = o;
    g_buffer.wstat.kept++;
    return 1;
}

/* Unified memory: a logged result written back on the GPU right after the dispatch that wrote it, in stream order, and
   never pending. On unified memory a storage binding is written in place (vk_uma_write), but not where another
   resource holds some of its bytes (GPU-owned: a storage image's publish) or where no import can hold them (pages
   the driver refuses: amdgpu's user pointers take no alias-pool page, which every trapped span becomes, and a whole
   2 MiB window goes with one such page). Such a binding was a result in staging, landed by the CPU after its
   work: one game binds 16 MiB windows of a tensor pool, the last of which reaches 185 KB into its decoded movie
   frame's GPU-owned span, and the next dispatch reading that window couldn't tell whether the CPU wrote it since
   (the import's pages aren't write-tracked, and a logged result has no original bytes to compare), so it landed it
   with a vk_finish: 72 ms each time, about twice a frame. Pending longer, it would have made the windows written in
   place over it compose in staging instead. Here the bytes the program's write log marks (and no others, as the
   CPU's landing writes them: land_logged) are merged on the GPU (vk_wlog_merge) where the range's bytes are kept, a
   part at a time:
   - an import's part, in place, a write pending until its submit is done like a shader's (vk_uma_gpu_wrote);
   - an owner's part, into that owner, which a landing then waits for (vk_own_res_wrote), as a draw writing a kept
     owner (keep_new);
   - whole pages that neither holds, into an owner made for them from memory, published over them GPU-owned
     (vk_own_publish) unless an import can be made there.
   Ownership holds as for any producer: a CPU access to an owner's part traps and lands it after this submission,
   the in-place part reaches the CPU when its submit is done (the release waits for it: vk_uma_bound, and the
   runtime's own reads wait for the write), and what reads the bytes on the GPU next is recorded after the merge's
   barrier. A CPU write racing the dispatch keeps the bytes the program didn't write, as with the landing */
enum { BACK_IMPORT, BACK_OWNER, BACK_NEW };
typedef struct {
    u64 cpu, len; /* guest memory [cpu, cpu + len) */
    int kind;
    VkOwnRes* res; /* BACK_OWNER: the owner holding it there (no reference); BACK_NEW: one made for it (one) */
    u64 res_off;   /* where the part's first byte is in res */
    VkBuffer buf;  /* BACK_IMPORT: the import holding it, and the part's first byte's offset in it */
    u64 off;
} BackPart;
#define BACK_PARTS 16
static u64 g_back_results, g_back_bytes, g_back_owners;
/* guest memory [cpu, cpu + len) a part at a time into p (*n of them): an import's where one holds the bytes, an
   owner's where one does, and where neither does an import made now (make) or else whole pages that an owner made
   for them can take, as keep_new's (a page shared with the CPU's bytes would trap at each of their writes). 0 when
   a part can't be any of these, isn't at a multiple of 4 bytes (wlog_merge.comp's words) or there are more parts */
static int back_parts(u64 cpu, u64 len, int make, BackPart* p, u32* n) {
    u32 k = 0;
    for (u64 c = cpu, e = cpu + len; c < e;) {
        int held;
        u64 part = vk_uma_import_part(c, e - c, &held);
        if (!part) return 0;
        if (held) {
            if (k == BACK_PARTS) return 0;
            p[k++] = (BackPart){.cpu = c, .len = part, .kind = BACK_IMPORT};
            c += part;
            continue;
        }
        OwnRange r[BACK_PARTS];
        u32 owners = own_any() ? own_lookup(c, part, r, BACK_PARTS) : 0, i;
        if (owners > BACK_PARTS) return 0;
        u64 from = c;
        for (i = 0; i <= owners; i++) {
            u64 to = i < owners ? r[i].addr : c + part;
            if (to > from) {
                if (k == BACK_PARTS) return 0;
                p[k++] = (BackPart){.cpu = from, .len = to - from, .kind = BACK_NEW};
            }
            if (i == owners) break;
            if (((r[i].addr | r[i].size | r[i].res_off) & 3) || k == BACK_PARTS) return 0;
            p[k++] = (BackPart){.cpu = r[i].addr,
                                .len = r[i].size,
                                .kind = BACK_OWNER,
                                .res = (VkOwnRes*)r[i].res,
                                .res_off = r[i].res_off};
            from = r[i].addr + r[i].size;
        }
        c += part;
    }
    for (u32 i = 0; i < k; i++) {
        if (p[i].kind != BACK_NEW) continue;
        if (make && vk_uma_gpu_write(p[i].cpu, p[i].len, &p[i].buf, &p[i].off)) {
            p[i].kind = BACK_IMPORT;
            continue;
        }
        if (((p[i].cpu | p[i].len) & 0xFFF) || !own_traps() || vk_own_cpu_read(p[i].cpu, p[i].len, KEEP_CPU_PRESENTS))
            return 0;
    }
    *n = k;
    return 1;
}
/* whether a logged writable binding of [gva, gva + len) can be written back on the GPU (back_record): nothing else
   the renderer holds over it to land first (gmem_need, which the binding then skips: the writes in place pending
   there are ordered before the merge by its barrier, not waited for) */
static int back_can(u64 gva, u64 len) {
    BackPart p[BACK_PARTS];
    u64 cpu = 0;
    u32 n;
    return vk_uma() && len && !((gva | len) & 3) && gmmu_run(gva, len, &cpu) == len &&
           !(g_cs_any && vk_compute_pending(gva, len)) && !(g_gfx_img_any && vk_gfx_image_pending(gva, len)) &&
           !vk_release_pending(gva, len) && !vk_mirror_pending(gva, len) && back_parts(cpu, len, 0, p, &n);
}
static void back_unref(BackPart* p, u32 n) {
    for (u32 i = 0; i < n; i++)
        if (p[i].kind == BACK_NEW && p[i].res) {
            vk_own_res_unref(p[i].res);
            p[i].res = NULL;
        }
}
/* o, a logged result of one writer that its dispatch, just recorded, wrote: written back on the GPU. 0 with nothing
   published when it can't be (it stays pending, landed as before) */
static int back_record(GfxOut* o) {
    BackPart p[BACK_PARTS];
    u32 n = 0, i;
    const struct OutLog* w = o->logs;
    if (o->nlogs != 1 || w->at || w->len != o->size || o->cpu == ~0ull || !back_parts(o->cpu, o->size, 1, p, &n))
        return 0;
    for (i = 0; i < n; i++)
        if (p[i].kind == BACK_IMPORT && !p[i].buf && !vk_uma_gpu_write(p[i].cpu, p[i].len, &p[i].buf, &p[i].off))
            return 0; /* nothing recorded yet */
    for (i = 0; i < n; i++) {
        if (p[i].kind != BACK_NEW) continue;
        /* the bytes memory has there, the program's merged over them below */
        if (!(p[i].res = vk_own_res_new(p[i].len)) ||
            !vk_guest_read_device(o->addr + (p[i].cpu - o->cpu), p[i].len, p[i].res->buf, 0)) {
            back_unref(p, n);
            return 0;
        }
    }
    vk_render_end();
    VkCommandBuffer cb = vk_cmd();
    vk_all_barrier(cb); /* the program's writes, and what read or wrote the bytes before it, first */
    int ok = 1;
    for (i = 0; i < n && ok; i++) {
        u32 from = (u32)(p[i].cpu - o->cpu), to = from + (u32)p[i].len;
        if (p[i].kind == BACK_IMPORT)
            ok = vk_wlog_merge(w->log, w->part, from, to, o->buf, o->off, p[i].buf, p[i].off);
        else
            ok = vk_wlog_merge(w->log, w->part, from, to, o->buf, o->off, p[i].res->buf, p[i].res_off);
    }
    vk_all_barrier(cb); /* and what reads or writes them next, after it */
    if (!ok) {          /* out of descriptors: the bytes merged so far are the ones its landing writes there too */
        back_unref(p, n);
        return 0;
    }
    for (i = 0; i < n; i++)
        if (p[i].kind == BACK_IMPORT)
            vk_uma_gpu_wrote(p[i].cpu, p[i].len);
        else if (p[i].kind == BACK_OWNER) {
            vk_own_res_wrote(p[i].res);
            gpu_mem_written_cpu(p[i].cpu, p[i].len); /* what is built from the bytes takes the new ones */
        } else {
            vk_own_publish(p[i].cpu, p[i].len, p[i].res, 0);
            vk_own_res_unref(p[i].res);
            if (copy_stats_on()) g_back_owners++;
        }
    if (copy_stats_on()) {
        g_back_results++;
        g_back_bytes += o->size;
    }
    return 1;
}

typedef struct {
    const u8* src;
} GapCopy;
static void gap_copy(void* user, u8* to, u64 a, u64 b) {
    const GapCopy* g = user;
    memcpy(to, g->src + a, b - a);
    g_buffer.wstat.copied += b - a;
}
/* Each writable binding keeps its original bytes until its draw's frame completes. The guest can keep writing RAM:
   vk_write_changed resolves those writes against the GPU result at the fence. A writer that logs its writes
   (vk_buf_copy_writable_logged) makes a logged result instead, with no original bytes: it lands the bytes logged */
/* o is written at [at, at + len) by the writer being bound: its log, when it logs, goes to o at the commit */
static void writer_of(GfxOut* o, u64 at, u32 len) {
    if (!g_bind_log) return;
    if (g_npw == sizeof g_pw / sizeof *g_pw) fatal("vulkan: more logged storage bindings than a dispatch has");
    g_pw[g_npw].o = o;
    g_pw[g_npw].log = g_bind_log;
    g_pw[g_npw].part = g_bind_part;
    g_pw[g_npw].at = (u32)at;
    g_pw[g_npw++].len = len;
}
/* a writer that logs nothing joins o, a logged result being set up: its original bytes are taken now, which its
   input still is, as no work of it has been recorded (an earlier result composed into it is copied on the GPU, and
   guest memory has the bytes without it, as a composed result's original bytes are). It compares from then on */
static int out_take_org(GfxOut* o) {
    o->org = malloc(o->size ? o->size : 1);
    if (!o->org) return 0;
    o->org_alloc = 1;
    read_untracked(o->addr, o->org, o->size);
    return 1;
}
static int copy_writable(u64 gva, u32 len, VkBuffer* buf, u64* off, int ordered) {
    GfxOut* o;
    if (!gva) return vk_buf_copy(0, len, buf, off);
    /* Repeated bindings of one writable range in the same draw must name the same storage. */
    for (GfxOut* p = g_prep; p; p = p->next)
        if (p->addr == gva && p->size == len) {
            if (!p->org && !g_bind_log && !out_take_org(p)) return 0;
            *buf = p->buf;
            *off = p->off;
            writer_of(p, 0, len);
            return 1;
        }
    /* bound as a storage buffer, a storage texel buffer (a compute buffer image) or a transform feedback buffer (a
       multiple of 4): an earlier result is taken in place at any offset those accept */
    u64 bind_align = g_vk.props.limits.minStorageBufferOffsetAlignment;
    if (bind_align < g_vk.props.limits.minTexelBufferOffsetAlignment)
        bind_align = g_vk.props.limits.minTexelBufferOffsetAlignment;
    if (bind_align < 4) bind_align = 4;
    g_src_match = NULL;
    int pending = gfx_gpu_src(gva, len, buf, off, 1, bind_align, ordered);
    if (pending == 1) {
        if (g_src_match) writer_of(g_src_match, gva - g_src_match->addr, len);
        return 1;
    }
    /* with unified memory the shader writes guest memory where it is, with nothing to land: not over bytes the GPU
       holds elsewhere (owned, a compute or storage image result, a queued release), which land first as before. Its
       barriers go by page (uma.c's order) whether or not the front end gives its own (ordered 0): more than NVN asks
       for, never fewer */
    if (!pending && vk_uma() && !range_owned(gva, len) && !(g_cs_any && vk_compute_pending(gva, len)) &&
        !(g_gfx_img_any && vk_gfx_image_pending(gva, len)) && !vk_release_pending(gva, len) &&
        vk_uma_write(gva, len, bind_align, buf, off))
        return 1;
    /* a logged result has no original bytes: its input goes straight into the buffer the shader writes */
    int logged = g_bind_log != NULL;
    /* where in place couldn't be, a logged one is written back on the GPU after its dispatch (back_record). Its input
       needs no landing or wait here: a write-only one has none, and another's is copied on the GPU (writable_owned)
       or read as below */
    int back = !pending && logged && back_can(gva, len);
    if (!pending && !back) gmem_need(gva, len);
    if (!pending && !back && keep_new(gva, len, bind_align, buf, off)) return 1;
    int owned = !pending && range_owned(gva, len);
    if (owned) (void)written_since_gva(gva, len, 0); /* establish the CPU-write baseline before the GPU copy */
    o = gfx_out_new((u32)(((gva & 0xFFF) + len + 0xFFF) >> 12), logged ? 0 : len);
    if (!o) return 0;
    o->back = back;
    if (logged)
        o->org = NULL;
    else if (pending == 2) { /* guest memory's bytes, with the earlier results copied over them below (out_rebase) */
        read_untracked(gva, o->org, len);
        g_buffer.wstat.copied += len;
    } else if (!owned) {
        const u8* src = guest_bytes(gva, len);
        if (!src) {
            free(o);
            return 0;
        }
        memcpy(o->org, src, len);
        g_buffer.wstat.copied += len;
    }
    o->composed = pending == 2;
    out_map(o, gva);
    u64 align = vk_stage_align();
    if (align < g_vk.props.limits.nonCoherentAtomSize) align = g_vk.props.limits.nonCoherentAtomSize;
    /* a write-only logged binding's staging is filled with zeros on the GPU (below), in whole words */
    int zeros = !(owned && !(back && g_bind_no_read)) && pending != 2 && logged && g_bind_no_read;
    o->ptr = vk_stage_output(zeros ? (len + 3) & ~3ull : len, align, buf, off, &o->mem);
    if (!o->ptr) {
        free(o);
        return 0;
    }
    if (owned && !(back && g_bind_no_read)) {
        if (!writable_owned(o, gva, len, *buf, *off)) {
            free(o);
            return 0;
        }
    } else if (pending == 2) {
        if (logged) { /* the gaps from guest memory, as a read's composition takes them */
            GapRead g = {gva};
            (void)gfx_overlay(gva, len, *buf, *off, o->ptr, gap_read, &g);
        } else {
            GapCopy g = {o->org};
            (void)gfx_overlay(gva, len, *buf, *off, o->ptr, gap_copy, &g); /* in place: nothing to stage */
        }
    } else if (logged) {
        if (!g_bind_no_read) { /* the program reads the buffer: its bytes are the input */
            const u8* src = guest_bytes(gva, len);
            if (!src) {
                free(o);
                return 0;
            }
            memcpy(o->ptr, src, len);
            g_buffer.wstat.copied += len;
        } else {
            /* write-only: the program only writes it, its bytes (if any) land from the log. But its stores are
               partial, and the GPU's read-modify-write of the surrounding cache line reads the staging it would
               have written over: give it zeros, not uninitialized ring memory, which can hang the GPU. The GPU
               writes them, in the upload command buffer that runs first in the submission: nothing before them
               there uses this new staging, and nothing on the CPU writes it after. The CPU's memset of up to
               16 MiB a binding was a twentieth of one game's GPU thread */
            rec_CmdFillBuffer(vk_cmd_upload(0), *buf, *off, (len + 3) & ~3ull, 0);
            o->no_input = 1;
        }
    } else {
        memcpy(o->ptr, o->org, len);
        g_buffer.wstat.copied += len;
    }
    o->addr = gva;
    o->size = len;
    o->buf = *buf;
    o->off = *off;
    o->frame = vk_submit_number();
    o->frame_id = vk_frame_id();
    if (g_prep_tail)
        g_prep_tail->next = o;
    else
        g_prep = o;
    g_prep_tail = o;
    writer_of(o, 0, len);
    copy_note(gva, len);
    return 1;
}
int vk_buf_copy_writable(u64 gva, u32 len, VkBuffer* buf, u64* off, int ordered) {
    g_bind_log = NULL;
    g_bind_no_read = 0;
    return copy_writable(gva, len, buf, off, ordered);
}
int vk_buf_copy_writable_logged(u64 gva, u32 len, VkBuffer* buf, u64* off, int ordered, VkWriteLog* l, u32 part,
                                int no_read) {
    g_bind_log = l;
    g_bind_part = part;
    g_bind_no_read = no_read;
    int ok = copy_writable(gva, len, buf, off, ordered);
    g_bind_log = NULL;
    return ok;
}

typedef struct {
    u64 key, gva;
    int writable, bind;
} DrawPage;
static int draw_page_cmp(const void* a, const void* b) {
    u64 x = ((const DrawPage*)a)->key, y = ((const DrawPage*)b)->key;
    return (x > y) - (x < y);
}
static u64 draw_page_key(u64 gva) {
    u64 cpu = gmmu_translate(gva);
    /* Unmapped virtual pages are distinct and have no guest destination for writeback. */
    return cpu ? cpu >> 12 : (1ull << 63) | (gva >> 12);
}
static DrawPage* draw_pages(const VkDrawStorage* draw, size_t* count) {
    size_t n = 0;
    for (int s = 0; s < VK_DRAW_STAGES; s++)
        for (int i = 0; i < draw->n[s]; i++) {
            const VkDrawGbuf* b = &draw->b[s][i];
            if (b->addr) n += ((b->addr & 4095) + b->size + 4095) >> 12;
        }
    DrawPage* p = malloc((n ? n : 1) * sizeof *p);
    if (!p) return NULL;
    size_t k = 0;
    for (int s = 0; s < VK_DRAW_STAGES; s++)
        for (int i = 0; i < draw->n[s]; i++) {
            const VkDrawGbuf* b = &draw->b[s][i];
            if (!b->addr) continue;
            for (u64 g = b->addr & ~4095ull, e = b->addr + b->size; g < e; g += 4096)
                p[k++] = (DrawPage){draw_page_key(g), g, b->writable, s * 16 + i};
        }
    qsort(p, k, sizeof *p, draw_page_cmp);
    *count = k;
    return p;
}
static int draw_alias_bindings(const DrawPage* p, size_t n, u8 active[VK_DRAW_STAGES * 16]) {
    memset(active, 0, VK_DRAW_STAGES * 16);
    int alias = 0;
    for (size_t i = 0; i < n;) {
        size_t j = i + 1;
        int writable = p[i].writable;
        while (j < n && p[j].key == p[i].key) writable |= p[j++].writable;
        if (j - i > 1 && writable) {
            for (size_t k = i; k < j; k++) active[p[k].bind] = 1;
            alias = 1;
        }
        i = j;
    }
    return alias;
}
static int draw_has_writable(const VkDrawStorage* draw) {
    for (int s = 0; s < VK_DRAW_STAGES; s++)
        for (int i = 0; i < draw->n[s]; i++)
            if (draw->b[s][i].addr && draw->b[s][i].writable) return 1;
    return 0;
}
/* Whether the draw's bindings may share a guest page with a writable one: 0 when each binding is one run of guest
   memory and their page ranges, compared pairwise, meet no writable one's, 1 otherwise (the page list decides). Every
   draw with storage asked the page list, allocated and sorted, twice: a twentieth of the GPU thread in one game */
static int draw_may_alias(const VkDrawStorage* draw) {
    u64 lo[VK_DRAW_STAGES * 16], hi[VK_DRAW_STAGES * 16];
    int w[VK_DRAW_STAGES * 16], k = 0;
    for (int s = 0; s < VK_DRAW_STAGES; s++)
        for (int i = 0; i < draw->n[s]; i++) {
            const VkDrawGbuf* b = &draw->b[s][i];
            u64 cpu = 0;
            if (!b->addr) continue;
            if (!b->size || gmmu_run(b->addr, b->size, &cpu) != b->size) return 1;
            lo[k] = cpu >> 12;
            hi[k] = (cpu + b->size - 1) >> 12;
            w[k] = b->writable;
            for (int j = 0; j < k; j++)
                if ((w[j] || w[k]) && lo[j] <= hi[k] && lo[k] <= hi[j]) return 1;
            k++;
        }
    return 0;
}
int vk_buf_draw_has_alias(const VkDrawStorage* draw) {
    if (!draw_has_writable(draw) || !draw_may_alias(draw)) return 0;
    size_t n;
    DrawPage* p = draw_pages(draw, &n);
    if (!p) return -1;
    u8 active[VK_DRAW_STAGES * 16];
    int alias = draw_alias_bindings(p, n, active);
    free(p);
    return alias;
}
static u32 draw_page_index(const DrawPage* p, size_t n, u64 key) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (p[mid].key < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return (u32)lo;
}
static int draw_output_page(const DrawPage* p, u8* ptr, VkBuffer buf, u64 off, VmaAllocation mem, int composed) {
    GfxOut* o;
    const u8* page;
    if (p->key & (1ull << 63)) return 1;
    o = gfx_out_new(1, 4096);
    if (!o) return 0;
    /* the original bytes from guest memory, which the atlas page was filled from: staging can be device-local
       memory behind the BAR, where a CPU read crosses PCIe uncached (this copy was most of the GPU thread's memcpy
       time in one game) */
    page = gmem_ptr_untracked(p->gva, 4096);
    if (page) {
        memcpy(o->org, page, 4096);
        g_buffer.wstat.copied += 4096;
    } else
        memset(o->org, 0, 4096);
    o->pages[0] = o->vmap[0] = p->key;
    o->cpu = p->key << 12;
    o->composed = composed;
    o->addr = p->gva;
    o->size = 4096;
    o->frame = vk_submit_number();
    o->frame_id = vk_frame_id();
    o->buf = buf;
    o->off = off;
    o->mem = mem;
    o->ptr = ptr;
    if (g_prep_tail)
        g_prep_tail->next = o;
    else
        g_prep = o;
    g_prep_tail = o;
    return 1;
}
/* An atlas page starts from guest memory, and the pending results over it are copied in on the GPU in the order they
   were recorded, after the shaders that wrote them: an aliased draw doesn't wait for earlier results to land in guest
   memory (one game drew about four a frame, each landing everything with a vk_finish). Its outputs keep guest memory as
   their original bytes until those results land (out_rebase). Compute results that haven't landed land for the
   atlas's pages alone. A result whose original bytes the guest has written since can't be copied whole: it lands
   first, as in gfx_gpu_src, which submits, and the draw starts its setup over. 1 when a result was copied in */
/* into l, the pending results on the atlas's pages (p, sorted by key): on a run of its guest pages, or with an unmapped
   page at a GPU page the atlas has unmapped (draw_page_key) */
static void out_near_atlas(OutList* l, const DrawPage* p, size_t unique) {
    out_list_init(l);
    for (size_t i = 0, j; i < unique; i = j) {
        j = i + 1;
        if (p[i].key & (1ull << 63)) {
            outx_find(l, &g_outx.gpu, p[i].gva >> 12, p[i].gva >> 12);
            continue;
        }
        while (j < unique && p[j].key == p[j - 1].key + 1) j++;
        outx_find(l, &g_outx.guest, p[i].key, p[j - 1].key);
    }
    out_list_sort(l);
}
static int atlas_fill(const DrawPage* p, size_t unique, u8* ptr, VkBuffer atlas, u64 atlas_off) {
    CopyBatch c;
    OutList l;
    size_t i;
    int landed = 0, composed = 0;
    if (g_cs_any)
        for (i = 0; i < unique; i++)
            if (!(p[i].key & (1ull << 63))) vk_compute_mem_need(p[i].gva, 4096);
    /* first, so a landing comes before memory is read; it frees the results, and leaves nothing to copy */
    out_list_init(&l);
    if (!t_buffer.out_landing) out_near_atlas(&l, p, unique);
    for (u32 k = 0; k < l.n && !landed; k++) {
        const GfxOut* o = l.v[k];
        for (u64 g = o->addr & ~4095ull, e = o->addr + o->size; g < e && !landed; g += 4096) {
            u64 key = draw_page_key(g), lo = g > o->addr ? g : o->addr, hi = g + 4096 < e ? g + 4096 : e;
            u32 at = draw_page_index(p, unique, key);
            if (at < unique && p[at].key == key && !out_copyable(o, lo, hi)) {
                land_reason(WB_DRAW);
                landed = 1;
            }
        }
    }
    out_list_free(&l);
    for (i = 0; i < unique; i++) {
        const u8* page = p[i].key & (1ull << 63) ? NULL : gmem_ptr_untracked(p[i].gva, 4096);
        if (page) {
            memcpy(ptr + i * 4096, page, 4096);
            g_buffer.wstat.copied += 4096;
        } else
            memset(ptr + i * 4096, 0, 4096);
    }
    memset(&c, 0, sizeof c);
    c.dst = atlas;
    /* each byte from the newest result over it, as gfx_overlay takes them */
    Covered cov;
    out_list_init(&l);
    if (!t_buffer.out_landing) out_near_atlas(&l, p, unique); /* again: a landing above freed what it found */
    cover_init(&cov, (u64)unique * 4096);
    for (u32 k = l.n; k-- && !cover_full(&cov);) { /* newest first */
        const GfxOut* o = l.v[k];
        for (u64 g = o->addr & ~4095ull, e = o->addr + o->size; g < e; g += 4096) {
            u64 key = draw_page_key(g), lo = g > o->addr ? g : o->addr, hi = g + 4096 < e ? g + 4096 : e;
            u32 at = draw_page_index(p, unique, key);
            if (at < unique && p[at].key == key) {
                Overlay v = {&c, o, atlas_off, at * 4096ull + (lo - g), lo - o->addr, &cov};
                u64 a = v.at, b = a + (hi - lo);
                if (!cover_take(&cov, a, b, overlay_part, &v)) overlay_part(&v, a, b);
                if (o->frame_id != vk_frame_id()) vk_frame_hold(o->frame_id);
                composed = 1;
            }
        }
    }
    copy_end(&c);
    cover_free(&cov);
    out_list_free(&l);
    return composed;
}

/* All bindings of an aliasing draw index one page atlas. This also covers fragmented GPU mappings: the shader's
   word index is translated through its binding's page list, so a write through either VA reaches the same word. */
int vk_buf_draw_prepare(VkDrawStorage* draw) {
    size_t n = 0;
    int writable = draw_has_writable(draw) && draw_may_alias(draw);
    DrawPage* p = writable ? draw_pages(draw, &n) : NULL;
    if (writable && !p) return 0;
    u8 active[VK_DRAW_STAGES * 16] = {0};
    int alias = p ? draw_alias_bindings(p, n, active) : 0;
    size_t unique = 0;
    if (alias)
        for (size_t i = 0; i < n; i++)
            if (active[p[i].bind] && (!unique || p[i].key != p[unique - 1].key)) p[unique++] = p[i];
    VkBuffer atlas = VK_NULL_HANDLE;
    u64 atlas_off = 0, atlas_size = unique * 4096ull;
    VmaAllocation mem = VK_NULL_HANDLE;
    u8* ptr = NULL;
    int composed = 0;
    if (alias) {
        if (atlas_size > g_vk.props.limits.maxStorageBufferRange) {
            WARN_LIMIT(1, "vulkan: aliased draw needs %llu storage bytes, beyond the device's descriptor range\n",
                       (unsigned long long)atlas_size);
            free(p);
            return 0;
        }
        u64 align = vk_stage_align();
        if (align < g_vk.props.limits.nonCoherentAtomSize) align = g_vk.props.limits.nonCoherentAtomSize;
        ptr = vk_stage_output(atlas_size, align, &atlas, &atlas_off, &mem);
        if (!ptr) {
            free(p);
            return 0;
        }
        composed = atlas_fill(p, unique, ptr, atlas, atlas_off);
        copy_note(1, atlas_size);
    }
    for (int s = 0; s < VK_DRAW_STAGES; s++) {
        if (!draw->n[s]) continue;
        size_t words = 48;
        if (alias)
            for (int i = 0; i < draw->n[s]; i++) {
                VkDrawGbuf* b = &draw->b[s][i];
                if (b->addr && active[s * 16 + i]) words += ((b->addr & 4095) + b->size + 4095) >> 12;
            }
        u32* map = (u32*)vk_stage(words * 4, vk_stage_align(), &draw->map_buf[s], &draw->map_off[s]);
        if (!map) {
            free(p);
            return 0;
        }
        draw->map_size[s] = words * 4;
        for (int i = 0; i < 16; i++) map[i] = ~0u;
        for (int i = 16; i < 32; i++) map[i] = 0;
        for (int i = 32; i < 48; i++) map[i] = 0;
        size_t at = 48;
        if (alias)
            for (int i = 0; i < draw->n[s]; i++) {
                VkDrawGbuf* b = &draw->b[s][i];
                if (!b->addr || !active[s * 16 + i]) continue;
                map[i] = (u32)at;
                map[16 + i] = (u32)((b->addr & 4095) >> 2);
                map[32 + i] = b->size >> 2;
                for (u64 g = b->addr & ~4095ull, e = b->addr + b->size; g < e; g += 4096)
                    map[at++] = draw_page_index(p, unique, draw_page_key(g));
                b->aliased = 1;
                b->buf = atlas;
                b->off = atlas_off;
                b->range = atlas_size;
            }
        snx_le32_array(map, words); /* the shader reads the table little-endian */
    }
    if (alias)
        for (size_t i = 0; i < unique; i++)
            if (!draw_output_page(&p[i], ptr + i * 4096, atlas, atlas_off + i * 4096, mem, composed)) {
                vk_buf_writable_abort();
                free(p);
                return 0;
            }
    if (alias && copy_stats_on()) {
        g_alias_draws++;
        g_alias_pages += unique;
    }
    free(p);
    return 1;
}

/* the entry's data where draws read it */
static void use(u32 i, VkBuffer* buf, u64* off) {
    Vb* e = &g_vb[i];
    if (e->used != g_vk_present) {
        e->used = g_vk_present;
        if (e->own) { /* to the recent end */
            own_unlink(i);
            own_link(i);
        }
    }
    if (copy_stats_on()) g_read_bound += e->len;
    *buf = e->buf;
    *off = e->off;
}
static void drop_own(u32 i) {
    Vb* e = &g_vb[i];
    if (e->own) {
        vk_defer_buffer(e->buf, e->mem);
        own_unlink(i);
        g_buffer.n--;
        g_buffer.bytes -= e->len;
    }
    e->own = 0;
    e->buf = VK_NULL_HANDLE;
}
/* A big own buffer takes the pages of its range written since it last matched memory (vk_written_runs), through this
   frame's staging ring, with a copy on the GPU between the draws before and after: a write anywhere in a 64 MiB window
   (vk_buf_get_window) made it hash and copy all of it, again at each submit until it held still for two checks. In
   one game's underground, where terrain streams in as the player moves, that was most of each hitch */
#define PATCH_MIN (256u << 10)
/* The runs are copied as they are, however many: past 64 of them, they were joined into one copy from the first to the
   end of the last, and a Switch game's city street, whose CPU writes pages all over a 64 MiB window of indices, copied
   about 50 MB a frame that way, a quarter of its GPU thread. One patch is made at a time (vk_lock), in g_buffer.patch */
typedef struct {
    const u8* src;
    u32 n;
    int whole; /* no room for the runs: from the first to the end of the last, in one copy (the first run's) */
} Patch;
static VkBufferCopy* patch_runs(void) { return g_buffer.patch.more ? g_buffer.patch.more : g_buffer.patch.r0; }
static void patch_note(void* user, u64 at, u64 n) {
    Patch* p = user;
    VkBufferCopy* r = patch_runs();
    if (p->whole) {
        r[0].size = at + n - r[0].dstOffset;
        return;
    }
    if (p->n && r[p->n - 1].dstOffset + r[p->n - 1].size == at) { /* touching the last: one */
        r[p->n - 1].size += n;
        return;
    }
    u32 cap = g_buffer.patch.more ? g_buffer.patch.cap : PATCH_RUNS0;
    if (p->n == cap) {
        VkBufferCopy* more = cap * 2 <= cap ? NULL : realloc(g_buffer.patch.more, (size_t)cap * 2 * sizeof *more);
        if (!more) { /* the runs so far and this one in one copy */
            p->whole = 1;
            r[0].size = at + n - r[0].dstOffset;
            p->n = 1;
            return;
        }
        if (!g_buffer.patch.more) memcpy(more, g_buffer.patch.r0, sizeof g_buffer.patch.r0);
        g_buffer.patch.more = r = more;
        g_buffer.patch.cap = cap * 2;
    }
    r[p->n++] = (VkBufferCopy){0, at, n};
}
static int patch_apply(const Vb* e, Patch* pt);
static int patch_own(u32 i, const u8* host, u32 since) {
    Vb* e = &g_vb[i];
    Patch pt = {.src = host};
    vk_written_runs(host, e->len, since, patch_note, &pt);
    return patch_apply(e, &pt);
}
/* pt's runs of the entry's bytes (pt->src) copied over its own buffer: staged, then copied on the GPU */
static int patch_apply(const Vb* e, Patch* pt) {
    VkBufferCopy* r = patch_runs();
    VkBuffer sb;
    u64 so, at = 0, bytes = 0;
    u8* p;
    if (!pt->n) return 1;
    for (u32 k = 0; k < pt->n; k++) bytes += r[k].size; /* a whole one's span is more than its runs */
    if (!(p = vk_stage_upload(bytes, vk_stage_align(), &sb, &so))) return 0;
    for (u32 k = 0; k < pt->n; k++) {
        memcpy(p + at, pt->src + r[k].dstOffset, r[k].size);
        r[k].srcOffset = so + at;
        at += r[k].size;
    }
    if (copy_stats_on()) g_copy_owned += bytes;
    vk_render_end(); /* copies happen outside rendering passes */
    /* the draws recorded and submitted before read the old bytes, and an earlier patch wrote them: both are done
       before the copy writes */
    copy_barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT);
    rec_CmdCopyBuffer(vk_cmd(), sb, e->buf, pt->n, r);
    copy_barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 VK_ACCESS_2_MEMORY_READ_BIT);
    return 1;
}
/* An own buffer a GPU-side write touched (an inline upload, an engine's result landed in memory): the pages that write
   stamped (gpu_mem_dirty_since) and those the CPU wrote since (the kernel's dirty bits, which vk_written_since reads)
   copied again, instead of the whole range hashed again to learn whether anything changed. A write to one page of
   a big buffer hashed all of it: most of one game's GPU thread. 0 when there is no write tracking */
static int patch_dirty(u32 i, u64 gva, const u8* host, u64 gw, u32 ws) {
    const Vb* e = &g_vb[i];
    Patch pt = {.src = host};
    u64 o = (u64)(host - g_mem), first = o >> 12, last = (o + e->len - 1) >> 12, run = 0;
    int in = 0;
    if (vk_written_since(host, e->len, ws) < 0) return 0; /* page_written is current from here */
    for (u64 pg = first; pg <= last + 1; pg++) {
        int d = 0;
        if (pg <= last) {
            u64 a = pg == first ? 0 : (pg << 12) - o, b = pg == last ? e->len : ((pg + 1) << 12) - o;
            d = page_written(pg, ws) || gpu_mem_dirty_since(gw, gva + a, b - a);
        }
        if (d && !in) run = pg;
        if (!d && in) {
            u64 a = run == first ? 0 : (run << 12) - o, b = pg > last ? e->len : (pg << 12) - o;
            patch_note(&pt, a, b - a);
        }
        in = d;
    }
    return patch_apply(e, &pt);
}

/* An own buffer is device memory the GPU reads at full speed, filled by a copy from this frame's staging ring. Mapped
   memory for it is the BAR's window on most GPUs (256 MB, which VMA prefers for host writes), and thousands of own
   buffers there crowded out the staging rings: one game's rings, made again in the window as they grew after loading,
   took writes at a twentieth of their speed for minutes (18 to 25 fps against 43). sb at so is the entry's bytes, in
   the staging ring already. Nothing before the copy reads the new buffer and the CPU wrote its bytes, so it runs ahead
   of the submission's work (vk_cmd_upload), and the rendering pass a draw before it opened goes on: on a tiling GPU an
   end stores every target of the pass, and its next begin loads them again */
static int make_own_(u32 i, VkBuffer sb, u64 so);
/* a buffer of its own being made, as a profiler zone (SNX_TRACY builds) */
static int make_own(u32 i, VkBuffer sb, u64 so) {
    VkResourceScope scope = vk_resource_begin();
    SNX_ZONE_V(z, "own buffer", g_vb[i].len);
    int r = make_own_(i, sb, so);
    SNX_ZONE_END(z);
    vk_resource_end(VK_RESOURCE_BUFFER_CACHE_BUILD, scope);
    return r;
}
static int make_own_(u32 i, VkBuffer sb, u64 so) {
    Vb* e = &g_vb[i];
    u64 len = e->len;
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = len,
                              .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                                       VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                       VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT, /* render.c copies indices out of it */
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkBuffer b;
    VmaAllocation m;
    if (vk_buffer_create(g_vk.vma, &bci, &aci, &b, &m, NULL) != VK_SUCCESS) return 0;
    vk_mem_track(m);
    if (copy_stats_on()) g_copy_owned += len;
    VkBufferCopy r = {so, 0, len};
    rec_CmdCopyBuffer(vk_cmd_upload(0), sb, b, 1, &r);
    g_buffer.ahead++;
    drop_own(i);
    e->own = 1;
    e->buf = b;
    e->mem = m;
    e->off = 0;
    e->used = g_vk_present;
    own_link(i);
    g_buffer.n++;
    g_buffer.bytes += len;
    return 1;
}

static u32 g_vb_map_gen = 1;
void vk_buf_mapping_changed(void) {
    if (!++g_vb_map_gen) g_vb_map_gen = 1;
}
/* the host bytes of an entry's range, for the write tracking's question about them: kept with the generation of the GPU
   mappings, which a change of either kind moves (vk_buf_mapping_changed). An own buffer asked it each time it was
   checked, translating the range, 1.6% of one game's GPU thread. The question needs no more than the address: the
   pending results were checked already, and the bytes aren't read */
static const u8* entry_host(Vb* e, u64 gva, u64 len) {
    if (e->host_gen != g_vb_map_gen) {
        const u8* p = gmem_host(gva, len);
        e->host_cpu = p ? (u64)(p - g_mem) : ~0ull;
        e->host_gen = g_vb_map_gen;
    }
    return e->host_cpu == ~0ull ? NULL : GP(e->host_cpu);
}
/* vk_written_since by GPU address, run by run of the host memory behind it: a range the GPU maps from several pieces
   of guest memory (a buffer of several memory handles) has no one host pointer, and had no answer, so it was hashed
   whole at every check. In one game's matches that was 140 GB every 600 frames, nearly all of its GPU thread */
static int written_since_gva(u64 gva, u64 len, u32 since) {
    int any = 0;
    for (u64 o = 0; o < len;) {
        u64 cpu = 0, n = gmmu_run(gva + o, len - o, &cpu);
        if (!n) { /* an unmapped page: nothing writes it (a mapping change moves g_vb_map_gen) */
            n = 0x1000 - ((gva + o) & 0xFFF);
            if (n > len - o) n = len - o;
        } else {
            int w = vk_written_since(GP(cpu), n, since);
            if (w < 0) return -1;
            any |= w;
        }
        o += n;
    }
    return any;
}
/* whether pool memory is under [gva, gva + len) (mem_pool_any), run by run of the GPU's page table */
static int pool_pages(u64 gva, u64 len) {
    for (u64 o = 0; o < len;) {
        u64 cpu = 0, n = gmmu_run(gva + o, len - o, &cpu);
        if (!n) {
            n = 0x1000 - ((gva + o) & 0xFFF);
            if (n > len - o) n = len - o;
        } else if (mem_pool_any(cpu, n))
            return 1;
        o += n;
    }
    return 0;
}
/* whether GPU-owned bytes are on [gva, gva + len)'s pages (memory/own.c): reading them on the CPU lands them, a wait
   for the GPU (vk_lock held, as own_hash needs) */
static int range_owned(u64 gva, u64 len) {
    if (!own_any()) return 0;
    for (u64 o = 0; o < len;) {
        u64 cpu = 0, h = 0, n = gmmu_run(gva + o, len - o, &cpu);
        if (!n) {
            n = 0x1000 - ((gva + o) & 0xFFF);
            if (n > len - o) n = len - o;
        } else if (own_hash(cpu, n, &h))
            return 1;
        o += n;
    }
    return 0;
}
int vk_buf_owned(u64 gva, u64 len) { return range_owned(gva, len); }
u64 vk_buf_ahead(void) { return g_buffer.ahead; }
/* A range over bytes the GPU holds, with nothing landed. One owner holding it whole is bound where it is, as a dispatch
   binds it (compute.c res_bind): every write to an owner's buffer is recorded outside rendering passes and followed by
   a barrier before the next one begins (vk_own_res_wrote), so a draw reads it inside the pass it is in. Copied, each
   such draw ended the pass, about 22 of a frame's 380 in one game, and on a tiling GPU every target of the pass is
   stored and loaded again. Any other range is copied into this frame's device ring, the CPU's bytes staged by the CPU
   and the owned ones from their owners by the GPU (vk_guest_read_device). Neither is hashed (the CPU's copy of the
   owned bytes is stale) or given a buffer of its own: it is bound or copied again when the GPU writes those bytes
   again (gpu_mem_dirty_since) or a later submit reads it. Bytes all written before this submission are copied ahead
   of its work (vk_guest_read_ahead), and the pass goes on; bytes it writes are copied after those writes, outside the
   pass. Draws over the results of the GPU's engines landed them, a wait for the GPU each */
static int buf_get_owned(u32 i, Vb* e, u64 gva, u64 len, int strict, u32 epoch, u64 align, VkBuffer* buf, u64* off) {
    VkBuffer sb;
    u64 so, cpu = 0;
    OwnRange o[2];
    if (!e) {
        if (!(i = vb_new(gva, (u32)len))) return 0;
        e = &g_vb[i];
        e->cs_adds = vk_compute_adds(); /* checked for pending results above */
        e->gfx_adds = vk_buf_adds();
    }
    drop_own(i);
    e->hash = 0;
    e->stable = 0;
    if (gmmu_run(gva, len, &cpu) == len && own_lookup(cpu, len, o, 2) == 1 && o[0].addr == cpu && o[0].size == len &&
        !(o[0].res_off & ((align ? align : 1) - 1))) {
        sb = ((const VkOwnRes*)o[0].res)->buf;
        so = o[0].res_off;
        ((VkOwnRes*)o[0].res)->read = ++g_buffer.order.owner_reads;
        if (copy_stats_on()) g_owned_bound += len;
    } else {
        /* in device memory, where the GPU's bytes stay on the GPU and the draws read all of it: in staging, the GPU
           copied its own bytes out over PCIe and every draw read them back over it */
        if (!vk_stage_device(len, vk_stage_align(), &sb, &so)) return 0;
        if (vk_guest_read_ahead(gva, len, sb, so))
            g_buffer.ahead++;
        else {
            vk_render_end(); /* the GPU's copies happen outside rendering passes */
            if (!vk_guest_read_device(gva, len, sb, so)) return 0;
        }
        if (copy_stats_on()) g_copy_stage += len;
    }
    e->buf = sb;
    e->off = so;
    e->frame = vk_submit_number();
    e->epoch = e->chk_epoch = epoch;
    if (strict) e->strict_epoch = epoch;
    e->gw = gpu_mem_generation();
    e->wseen = vk_ww_now();
    use(i, buf, off);
    return 1;
}
/* inplace: IN_NO, the range is copied; IN_TRY, read in place in guest memory where it can be (uma.c), else copied;
   IN_ONLY, in place or 0 (a window is copied instead) */
enum { IN_NO, IN_TRY, IN_ONLY };
/* the biggest copy engine's source that takes a buffer of its own at once (buf_get): a sixty-fourth of the budget */
static u64 copy_src_at_once(void) {
    u64 usage, budget;
    vk_mem_use(&usage, &budget);
    return budget / 64;
}
static int buf_get(u64 gva, u64 len, VkBuffer* buf, u64* off, int strict, u64 align, int inplace) {
    int pending;
    u32 i = 0, epoch = vk_ww_epoch(); /* before anything is checked or read */
    Vb* e = NULL;
    if (!len || len > 0xFFFFFFFFull) return 0; /* no buffer the GPU reads is 4 GB */
    /* over writes of the GPU mirror's programs that haven't landed: read in the mirror, which has them, after them. At
       an offset the use can't bind (a draw's constants or vertices, a few bytes at any address, where a uniform
       buffer binds at 64), the bytes are copied on the GPU from the mirror into device memory that it can: landing
       them took a submit and a wait for the GPU, 150 a second in one game's menus */
    /* So with those it owns, landed only where the CPU touched them: read elsewhere, the CPU's bytes between them were
       copied again for every draw, 16 MiB a binding, about half of one game's GPU thread */
    if (vk_mirror_pending(gva, len) || vk_mirror_holds(gva, len)) {
        u64 cpu = 0, a = align ? align : 1;
        VkBuffer mb;
        u64 mo;
        if (gmmu_run(gva, len, &cpu) == len && vk_mirror_slice(gva, len, &mb, &mo)) {
            if (!(cpu & (a - 1))) {
                /* the programs' writes before it, once for all the reads after them: a barrier for each binding
                   ended the rendering pass at nearly every draw, about 470 passes a frame of one game's race, and
                   the GPU drained at each (30 ms a frame). The mirror's copies of the CPU's bytes take their own
                   barrier (mirror.c) */
                static u64 barrier_gen;
                if (barrier_gen != vk_mirror_write_gen()) {
                    vk_render_end();
                    copy_barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
                    barrier_gen = vk_mirror_write_gen();
                }
                *buf = mb;
                *off = mo;
                return 1;
            }
            vk_render_end(); /* copies happen outside rendering passes */
            if (vk_stage_device(len, a > vk_stage_align() ? a : vk_stage_align(), buf, off)) {
                copy_barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                VkBufferCopy r = {mo, *off, len};
                rec_CmdCopyBuffer(vk_cmd(), mb, *buf, 1, &r);
                copy_barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
                return 1;
            }
        }
        vk_mirror_mem_need(gva, len); /* where it can't be: landed first */
    }
    if (g_vbi) {
        u32 sl = vb_slot(gva, (u32)len);
        if ((i = g_vbi[sl].idx)) e = &g_vb[i];
    }
    /* Pending results over the range, unless none was added since they were last checked for here (only the sweep in
       vk_buf_frame frees entries, and only this function adds them, so e stays good meanwhile) */
    if (!e || e->cs_adds != vk_compute_adds() || e->gfx_adds != vk_buf_adds()) {
        pending = gfx_gpu_src(gva, len, buf, off, 0, align, 1);
        if (pending == 1) return 1;
        if (pending == 2) return gfx_composed(gva, len, buf, off); /* newer than any cached copy */
        if (vk_compute_gpu_src(gva, len, buf, off)) return 1;      /* a compute result that hasn't landed yet */
        /* one that covers the range only in part lands first (its write shows in gpu_mem_dirty_since below): the
           cached copy would otherwise be used without it */
        if (g_cs_any) vk_compute_mem_need(gva, len);
        if (e) {
            e->cs_adds = vk_compute_adds();
            e->gfx_adds = vk_buf_adds();
        }
    }
    /* no GPU result is pending over it: guest memory holds its bytes, which the GPU reads where they are on a device
       that shares memory with the CPU. Not GPU-owned bytes, which only the GPU holds */
    if (inplace && vk_uma() && !range_owned(gva, len) && vk_uma_get(gva, len, align, buf, off)) return 1;
    if (inplace == IN_ONLY) return 0;
    /* valid: the data is where draws can read it this frame */
    /* not while an in-place write is pending over it: a copy from before the write would be read */
    int valid = e && e->buf && (e->own || e->frame == vk_submit_number()) && !vk_uma_pending(gva, len);
    int told = 0; /* write tracking answered for the range (it was written): its checks need no samples */
    if (valid && !gpu_mem_dirty_since(e->gw, gva, len)) {
        if (e->epoch == epoch && (!strict || e->strict_epoch == epoch)) {
            use(i, buf, off);
            return 1;
        }
        /* with write tracking, an own buffer is checked only when its memory was written. A copy engine's source
           (strict 2) on pool pages is compared instead: they can be written where the tracking doesn't look
           (mem_pool_any), and what a copy lands in keeps a stale byte for good */
        int tracked = e->own && !(strict == 2 && pool_pages(gva, len));
        const u8* host = tracked ? entry_host(e, gva, len) : NULL;
        int w = !tracked ? -1 : host ? vk_written_since(host, len, e->wseen) : written_since_gva(gva, len, e->wseen);
        if (w == 1 && host && len >= PATCH_MIN) { /* the written pages, not the whole range again */
            u32 wnow = vk_ww_now();
            if (patch_own(i, host, e->wseen)) {
                e->wseen = wnow;
                w = 0;
            }
        }
        if (w == 0) {
            e->epoch = epoch;
            if (strict) e->strict_epoch = epoch;
            use(i, buf, off);
            return 1;
        }
        told = w > 0;
        /* without it: own buffers that stayed the same for long are checked every few submits, big ones sampled in
           between. A CPU write in between shows that many submits late; writes of the GPU front end always show at
           once (gpu_mem_dirty_since above) */
        if (!strict && w < 0 && e->own && e->stable >= 16 && len >= (64u << 10) && epoch - e->chk_epoch < 8) {
            e->epoch = epoch;
            use(i, buf, off);
            return 1;
        }
        if (!strict && w < 0 && e->own && e->stable >= 32 && len < (64u << 10) && epoch - e->chk_epoch < 4) {
            e->epoch = epoch;
            use(i, buf, off);
            return 1;
        }
        if (!strict && w < 0 && e->own && e->stable >= 64 && len >= 4096 && e->shash &&
            epoch - e->chk_epoch < (e->stable >= 256 ? 240u : 64u)) {
            if (epoch - e->schk < 4 || buf_sample_hash(gva, len) == e->shash) {
                if (epoch - e->schk >= 4) e->schk = epoch;
                e->epoch = epoch;
                use(i, buf, off);
                return 1;
            }
        }
    }
    else if (valid && e->own && len >= PATCH_MIN && !range_owned(gva, len)) {
        /* a GPU-side write on its pages: those pages copied again, not the range hashed again. Not a small range: one
           is hashed in microseconds, and GPU-side writes beside it stamp its pages (in one game 760 ranges a frame,
           78% of them unchanged), each copy ending the rendering pass */
        const u8* host = entry_host(e, gva, len);
        u32 wnow = vk_ww_now();
        u64 gnow = gpu_mem_generation(); /* both before the bytes are read */
        if (host && patch_dirty(i, gva, host, e->gw, e->wseen)) {
            e->gw = gnow;
            e->wseen = wnow;
            e->epoch = epoch;
            if (strict) e->strict_epoch = epoch;
            use(i, buf, off);
            return 1;
        }
    }
    int why = 0;
    if (copy_stats_on())
        why = !e ? ST_NEW
            : !e->buf ? ST_NOBUF
            : !valid ? ST_SUBMIT
            : gpu_mem_dirty_since(e->gw, gva, len) ? ST_DIRTY
            : e->own ? ST_WRITTEN
                     : ST_EPOCH;
    /* GPU-owned bytes, or bytes written in place by a submit not done yet: copied on the GPU (vk_guest_read) */
    if (range_owned(gva, len) || vk_uma_pending(gva, len))
        return buf_get_owned(i, e, gva, len, strict, epoch, align, buf, off);
    u32 wnow = vk_ww_now();
    SamplesCount counts[4];
    u32 ncounts;
    const u8* src = draw_bytes(gva, len, counts, 4, &ncounts);
    if (!src) return 0;
    if (strict == 2) g_buffer.copy_src_read += len;
    if (ncounts && e) drop_own(i); /* the counts are written into this frame's staging, which the draw reads */
    /* Data without a buffer of its own goes into this frame's staging ring whatever its hash says, so it is copied as
       it is hashed and read once: reading it twice, for the hash and then the copy, was half of one game's GPU thread */
    VkBuffer sb = VK_NULL_HANDLE;
    u64 so = 0, h;
    u8* staged = NULL;
    if (!e || !e->own) {
        if (e) e->buf = VK_NULL_HANDLE; /* a failed allocation must not leave the old snapshot reusable */
        if (!(staged = vk_stage_upload(len, vk_stage_align(), &sb, &so))) return 0;
        h = vk_hash_copy(staged, src, len);
    } else
        h = vk_hash(src, len);
    g_checks.hashes++;
    if (!e) {
        if (!(i = vb_new(gva, (u32)len))) return 0;
        e = &g_vb[i];
        e->hash = h ^ 1;
        e->cs_adds = vk_compute_adds(); /* checked for pending results above */
        e->gfx_adds = vk_buf_adds();
    }
    int changed = e->hash != h;
    if (e->hash == h && e->stable < 100000)
        e->stable++;
    else if (e->hash != h) {
        e->hash = h;
        e->stable = 0;
        drop_own(i);
    }
    e->epoch = e->chk_epoch = epoch;
    if (strict) e->strict_epoch = epoch;
    e->gw = gpu_mem_generation();
    e->wseen = wnow;
    /* the samples serve only the checks above that write tracking can't answer: where it answered none is kept (0),
       so a stable range the CPU rewrites with the same bytes takes none at each write, and should the tracking stop
       answering for it, its next check hashes it whole first */
    if (told)
        e->shash = 0;
    else if (e->stable >= 63 && len >= 4096) {
        e->shash = buf_sample_hash(gva, len);
        e->schk = epoch;
    }
    if (!e->own) {
        if (!staged) { /* its own buffer was dropped above: the bytes changed */
            if (!(staged = vk_stage_upload(len, vk_stage_align(), &sb, &so))) return 0;
            memcpy(staged, src, len);
        }
        /* a big range takes its own buffer at once when writes are tracked: changes then patch it (patch_own). Not a
           copy engine's source past a sixty-fourth of the memory budget: a texture's upload reads its source once,
           and an 8192x8192 RGBA8 texture's levels took 760 MiB of them that nothing read again, where a phone had
           1 GB available. Such a source takes one once it stays the same, as every other range does */
        int at_once = len >= PATCH_MIN && g_ww > 0 && (strict != 2 || len <= copy_src_at_once());
        if (ncounts || !((e->stable >= 2 || at_once) && make_own(i, sb, so))) {
            /* changing (or new) data: read from this frame's staging ring */
            e->buf = sb;
            e->off = so;
            if (copy_stats_on()) {
                g_copy_stage += len;
                stage_note(why, changed, gva, len);
            }
            e->frame = vk_submit_number();
        }
    }
    if (ncounts && !vk_query_counts(sb, so, counts, ncounts)) { /* not on the GPU: the reports performed, waited for */
        const u8* performed = guest_bytes(gva, len);
        if (!performed) return 0;
        memcpy(staged, performed, len);
        e->hash ^= 1; /* not the bytes the hash is of */
        e->gw = gpu_mem_generation();
    }
    use(i, buf, off);
    return 1;
}

int vk_buf_get(u64 gva, u64 len, VkBuffer* buf, u64* off) {
    return buf_get(gva, len, buf, off, 0, vk_stage_align(), IN_NO);
}
/* Vertex and index buffers bind at any offset that keeps their elements aligned, not only at the uniform and storage
   bindings' alignment: a draw reading an earlier draw's storage result at another offset reads its buffer in place
   instead of composing it over guest memory. In one game 216,000 vertex buffer bindings in 600 frames were composed
   that way, each a copy of its range into staging and a GPU copy of the result over it */
int vk_buf_get_input(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off) {
    return buf_get(gva, len, buf, off, 0, align, IN_TRY);
}

/* Windows: guest ranges that storage buffers with no size of their own (a bare address in a program's constant buffer,
   gbuf_tag_any) are bound inside, so draws whose ranges overlap read one copy of guest memory, each at its own offset
   into it. Each such buffer takes the 16 MiB after its address (vk_gbuf_desc): one game's terrain tiles, each with its
   own address about 1 MB after the last, made thirty overlapping copies of 16 MiB, 430 MB, and a write under them made
   up to sixteen of them copy again. A range in no window gets one of WIN_SIZE from the MiB it starts in, as far as
   guest memory runs on and short of a window above, which the ranges after it fall in until the next window. Sized up
   front rather than grown into the union of the ranges that meet it: those come one draw after another, often within a
   frame, and each growth would copy the whole window again. Windows go when the GPU mappings change (their run of guest
   memory may have) and when unused for a while, before the cache's entries for them age out */
#define WIN_N    32
#define WIN_SIZE (64ull << 20)
#define WIN_IDLE 1200 /* presents */
typedef struct {
    u64 start, end; /* end 0: none */
    u32 used, gen;  /* the present it was last bound at, g_vb_map_gen when made */
} Win;
static Win g_win[WIN_N];
static int win_live(const Win* w) {
    return w->end && w->gen == g_vb_map_gen && (u32)(g_vk_present - w->used) < WIN_IDLE;
}
static int win_get(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off) {
    u64 at, cpu = 0;
    int slot = -1;
    if (!len || gva + len < gva) return 0;
    for (int i = 0; i < WIN_N && slot < 0; i++)
        if (win_live(&g_win[i]) && g_win[i].start <= gva && gva + len <= g_win[i].end) slot = i;
    if (slot < 0) {
        u64 start = gva & ~((1ull << 20) - 1), run;
        /* the MiB's start may lie before the run of guest memory the range is in: the range's own start then */
        if (gmmu_run(start, gva + len - start, &cpu) < gva + len - start) start = gva;
        run = gmmu_run(start, WIN_SIZE, &cpu) & ~3ull;
        if (run < gva + len - start) /* not one run: the range alone */
            return buf_get(gva, len, buf, off, 0, vk_stage_align(), IN_NO);
        /* a window above, which ranges from its start on fall in: this one needs to hold no more than the ranges
           starting before it, so the two overlap by a range's length at most (ranges come in any order) */
        for (int i = 0; i < WIN_N; i++)
            if (win_live(&g_win[i]) && g_win[i].start > gva && g_win[i].start - start < run) {
                u64 need = g_win[i].start + len - start;
                if (need < run) run = need;
            }
        for (int i = 0; i < WIN_N && slot < 0; i++)
            if (!win_live(&g_win[i])) slot = i;
        if (slot < 0) /* all live: the one bound longest ago */
            for (int i = slot = 0; i < WIN_N; i++)
                if ((i32)(g_win[i].used - g_win[slot].used) < 0) slot = i;
        g_win[slot] = (Win){start, start + run, 0, g_vb_map_gen};
    }
    g_win[slot].used = g_vk_present;
    if (!buf_get(g_win[slot].start, g_win[slot].end - g_win[slot].start, buf, &at, 0, vk_stage_align(), IN_NO))
        return 0;
    /* a binding's offset must be a multiple of the device's alignment: a range that would land off it is copied
       alone */
    if ((at + (gva - g_win[slot].start)) % (align ? align : 1))
        return buf_get(gva, len, buf, off, 0, vk_stage_align(), IN_NO);
    *off = at + (gva - g_win[slot].start);
    return 1;
}
int vk_buf_get_window(u64 gva, u64 len, VkBuffer* buf, u64* off) {
    u64 align = g_vk.props.limits.minStorageBufferOffsetAlignment;
    if (vk_uma() && buf_get(gva, len, buf, off, 0, align, IN_ONLY)) return 1; /* no window needed in place */
    return win_get(gva, len, align, buf, off);
}
/* A vertex or index range whose extent isn't known (an indirect draw's: the GPU reads the count), in a window as a
   bare storage buffer's: a binding reads on to the window's end, and draws over the same memory share its copy.
   align: the elements' */
int vk_buf_get_window_input(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off) {
    if (vk_uma() && buf_get(gva, len, buf, off, 0, align, IN_ONLY)) return 1;
    return win_get(gva, len, align, buf, off);
}
/* A draw looks its buffers up one after another, and each lookup waited on two cache misses in a row, its index slot
   and then the entry the slot names: 88% of buf_get's own time in one game. A caller that knows its ranges first
   fetches every slot (step 0), then every entry they name (step 1), so the misses overlap */
void vk_buf_prefetch(u64 gva, u64 len, int step) {
    u32 sl;
    if (!g_vbi || !len || len > 0xFFFFFFFFull) return;
    sl = vb_home(gva, (u32)len);
    if (!step) {
        __builtin_prefetch(&g_vbi[sl]);
        return;
    }
    if (g_vbi[sl].idx && g_vbi[sl].start == gva && g_vbi[sl].len == (u32)len)
        for (size_t o = 0; o < sizeof(Vb); o += 64) __builtin_prefetch((const char*)&g_vb[g_vbi[sl].idx] + o);
}
/* A dispatch's read-only storage buffer (compute.c), checked with no delay as a constant buffer is: a changed word
   changes the program's result. With unified memory it is read where guest memory holds it, as vertices and constant
   buffers are: the import that holds the range, bound at its offset with no copy (vk_uma_get). An Ounce program binds
   each of its tensors as the 16 MiB after its address (vk_gbuf_desc), and the dispatches before it wrote them there in
   place (vk_uma_write); copied, every binding of one took a 16 MiB GPU copy out of the import while those writes were
   pending, and a hash and a 16 MiB copy on the GPU thread after they were done: about 35 copies, 540 MB and 15 ms of
   GPU time per frame of one game's upscaled intro movie. Read in place:
   - the dispatches that wrote the bytes in place before it come first: vk_uma_get records a barrier when the range's
     pages were written in place since the last one (uma.c's order), and a write in place after it waits for its read
     the same way (note_read). A logged result written back on the GPU (back_record) has its barrier after the merge;
   - the CPU's accesses are ordered as for vertices read in place, with no trap: no byte of the range is GPU-owned
     (range_owned, checked first). A release waits for the submit that last read guest memory in place (vk_uma_bound),
     so the guest writes the next frame's data there only after the dispatch read it, and a write of the runtime's own
     on those pages waits for that submit (vk_uma_before_write, the live pages hz_mark stamps);
   - bytes the renderer holds elsewhere are not read in guest memory: pending results of draws and dispatches are bound
     or composed where they are (gfx_gpu_src, vk_compute_gpu_src, in buf_get), GPU-owned ranges and the pages the
     runtime wrote (front) are copied as before, and so are storage images not published yet and queued releases'
     payloads, over which copy_writable refuses writes in place too: the copy lands them first (gmem_need), which a
     read in place would miss.
   At the storage binding's own alignment, which the import's offset must meet (a tensor's address is a multiple of 32
   bytes), not the staging ring's, which can be larger */
int vk_buf_get_strict(u64 gva, u64 len, VkBuffer* buf, u64* off) {
    if (vk_uma() && !(g_gfx_img_any && vk_gfx_image_pending(gva, len)) && !vk_release_pending(gva, len)) {
        u64 align = g_vk.props.limits.minStorageBufferOffsetAlignment;
        return buf_get(gva, len, buf, off, 1, align ? align : 1, IN_TRY);
    }
    return buf_get(gva, len, buf, off, 1, vk_stage_align(), IN_NO);
}
/* a constant buffer: read in place where it can be, as vertices are, but the pages the runtime wrote (uma.c's front,
   its constant buffer updates among them), which are copied as before */
int vk_buf_get_uniform(u64 gva, u64 len, VkBuffer* buf, u64* off) {
    return buf_get(gva, len, buf, off, 1, vk_stage_align(), IN_TRY);
}
int vk_buf_get_copy_src(u64 gva, u64 len, VkBuffer* buf, u64* off) {
    return buf_get(gva, len, buf, off, 2, vk_stage_align(), IN_NO);
}
u64 vk_buf_copy_src_read(void) { return g_buffer.copy_src_read; }

u64 vk_buf_reclaim(u32 before, u64 want) {
    u64 freed = 0;
    while (g_buffer.head && freed < want && (i32)(g_vb[g_buffer.head].used - before) < 0) {
        u32 i = g_buffer.head;
        freed += g_vb[i].len;
        drop_own(i); /* the entry stays: its range is copied into a buffer again when next read */
        vk_defer_bytes(g_vb[i].len);
    }
    return freed;
}
/* A front end with explicit storage ordering supplies these barriers in command-stream order. */
int vk_buf_order_pending(int writes_only) {
    return g_buffer.order.writes != g_buffer.order.visible || (!writes_only && g_buffer.order.read_pending);
}
void vk_buf_order_request(int writes_only) {
    /* One later dependency covers every earlier point. Readbacks and surface consumers retain their own orders. */
    if (vk_buf_order_pending(writes_only)) g_buffer.order.required = 1;
}
void vk_buf_ordered(void) {
    g_buffer.order.owner_reads_ordered = g_buffer.order.owner_reads; /* the renderer's barrier is from all commands */
    g_buffer.order.submit = vk_submit_number();
    g_buffer.order.work = g_vk_drawn + g_vk_dispatches;
    g_buffer.order.visible = g_buffer.order.writes;
    g_buffer.order.read_pending = 0;
    g_buffer.order.required = 0;
    if (copy_stats_on()) g_buffer.order.explicit_barriers++;
}
void vk_buf_stats(u32* count, u64* bytes) {
    *count = g_buffer.n;
    *bytes = g_buffer.bytes;
}
void vk_buf_writable_stats(u64* copied, u64* read_back, u64* merged) {
    *copied = g_buffer.wstat.copied;
    *read_back = g_buffer.wstat.read_back;
    *merged = g_buffer.wstat.merged;
}

/* buffers not read for twenty seconds, and in 120 presents, go */
static void buf_frame(void) {
    vk_uma_frame();
    if (g_vk_present % 4) return;
    vk_wlog_trim();
    if (copy_stats_on() && g_vk_present % 600 == 0) {
        LOG("vulkan: buffer bytes over 600 presents: uncached=%llu zero=%llu calls=%llu peak=%llu staged=%llu owned=%llu bound=%llu owner_bound=%llu writeback=%llu/%llu waits=%llu/%llums reused=%llu\n",
            (unsigned long long)g_copy_uncached, (unsigned long long)g_copy_zero, (unsigned long long)g_copy_calls,
            (unsigned long long)g_copy_peak, (unsigned long long)g_copy_stage, (unsigned long long)g_copy_owned,
            (unsigned long long)g_read_bound, (unsigned long long)g_owned_bound, (unsigned long long)g_wb_outputs,
            (unsigned long long)g_wb_bytes, (unsigned long long)g_wb_waits,
            (unsigned long long)(g_wb_wait_ns / 1000000), (unsigned long long)g_wb_reuses);
        LOG("vulkan: logged storage results over 600 presents: %llu landed, %llu windows written, %llu bytes "
            "landed, %llu runs the CPU wrote over while pending (the GPU's bytes taken)\n",
            (unsigned long long)g_wb_logged, (unsigned long long)g_wb_logged_windows,
            (unsigned long long)g_wb_logged_bytes, (unsigned long long)g_wb_logged_conflicts);
        LOG("vulkan: logged storage results written back on the GPU over 600 presents: %llu (%llu MB bound), %llu "
            "owners made for memory no import holds\n",
            (unsigned long long)g_back_results, (unsigned long long)(g_back_bytes >> 20),
            (unsigned long long)g_back_owners);
        g_back_results = g_back_bytes = g_back_owners = 0;
        {
            u64 logs, bytes, in_use, windows;
            vk_wlog_stats(&logs, &bytes, &in_use, &windows);
            LOG("vulkan: write logs: %llu made, %llu MB kept, %llu in use\n", (unsigned long long)logs,
                (unsigned long long)(bytes >> 20), (unsigned long long)in_use);
        }
        g_wb_logged = g_wb_logged_windows = g_wb_logged_bytes = g_wb_logged_conflicts = 0;
        {
            u64 gr, hv, pg, ld, ov;
            u32 room;
            vk_mirror_stats(&gr, &hv, &pg, &ld, &ov, &room);
            if (gr)
                LOG("vulkan: GPU mirror: %llu MB bound, %llu harvests, %llu pages written, %llu bytes landed, %llu "
                    "harvests past their room (now %u pages)\n",
                    (unsigned long long)(gr * 2), (unsigned long long)hv, (unsigned long long)pg,
                    (unsigned long long)ld, (unsigned long long)ov, room);
        }
        LOG("vulkan: buffer dependencies: %llu barriers, %llu consumer barriers avoided, %llu parallel writes, %llu explicit barriers\n",
            (unsigned long long)g_buffer.order.barriers, (unsigned long long)g_buffer.order.read_reuses,
            (unsigned long long)g_buffer.order.parallel_writes, (unsigned long long)g_buffer.order.explicit_barriers);
        g_buffer.order.barriers = g_buffer.order.read_reuses = 0;
        g_buffer.order.parallel_writes = g_buffer.order.explicit_barriers = 0;
        stage_report();
        vk_stage_report();
        LOG("vulkan: buffer waits: draw=%llu/%llums read=%llu/%llums write=%llu/%llums signal=%llu/%llums syncwait=%llu/%llums map=%llu/%llums draw_landed=old_frame:%llu changed:%llu composed=%llu+%llu\n",
            (unsigned long long)g_wb_reason[WB_DRAW], (unsigned long long)(g_wb_reason_ns[WB_DRAW] / 1000000),
            (unsigned long long)g_wb_reason[WB_READ], (unsigned long long)(g_wb_reason_ns[WB_READ] / 1000000),
            (unsigned long long)g_wb_reason[WB_WRITE], (unsigned long long)(g_wb_reason_ns[WB_WRITE] / 1000000),
            (unsigned long long)g_wb_reason[WB_SIGNAL], (unsigned long long)(g_wb_reason_ns[WB_SIGNAL] / 1000000),
            (unsigned long long)g_wb_reason[WB_SYNCWAIT], (unsigned long long)(g_wb_reason_ns[WB_SYNCWAIT] / 1000000),
            (unsigned long long)g_wb_reason[WB_MAP], (unsigned long long)(g_wb_reason_ns[WB_MAP] / 1000000),
            (unsigned long long)g_wb_draw_miss[0], (unsigned long long)g_wb_draw_miss[2],
            (unsigned long long)g_wb_draw_miss[1], (unsigned long long)g_wb_draw_miss[3]);
        LOG("vulkan: writable storage over 600 presents: %llu bytes copied before the draws, %llu read back, %llu "
            "results merged, %llu bindings kept on the GPU\n",
            (unsigned long long)(g_buffer.wstat.copied - g_buffer.wstat_seen.copied),
            (unsigned long long)(g_buffer.wstat.read_back - g_buffer.wstat_seen.read_back),
            (unsigned long long)(g_buffer.wstat.merged - g_buffer.wstat_seen.merged),
            (unsigned long long)(g_buffer.wstat.kept - g_buffer.wstat_seen.kept));
        g_buffer.wstat_seen = g_buffer.wstat;
        LOG("vulkan: keep_new rejected (traps_off=%llu uma=%llu empty=%llu unaligned=%llu not_contig=%llu "
            "cpu_read=%llu)\n",
            (unsigned long long)g_keep_fail[KF_TRAPS_OFF], (unsigned long long)g_keep_fail[KF_UMA],
            (unsigned long long)g_keep_fail[KF_EMPTY], (unsigned long long)g_keep_fail[KF_UNALIGNED],
            (unsigned long long)g_keep_fail[KF_NOT_CONTIG], (unsigned long long)g_keep_fail[KF_CPU_READ]);
        {
            u64 cmp, chg;
            vk_writeback_diag(&cmp, &chg);
            LOG("vulkan: writeback compared %llu bytes and changed %llu (%.1f%%)\n", (unsigned long long)cmp,
                (unsigned long long)chg, cmp ? (double)chg * 100.0 / (double)cmp : 0.0);
        }
        LOG("vulkan: aliased storage draws=%llu pages=%llu; buffer cache entries=%u, lookups=%llu probing %llu more "
            "slots\n",
            (unsigned long long)g_alias_draws, (unsigned long long)g_alias_pages, g_vblive,
            (unsigned long long)g_vb_lookups, (unsigned long long)g_vb_probes);
        vk_gfx_image_report();
        g_vb_lookups = g_vb_probes = 0;
        g_copy_uncached = g_copy_stage = g_copy_owned = g_copy_zero = g_copy_calls = g_copy_peak = g_read_bound =
            g_owned_bound = 0;
        g_wb_outputs = g_wb_bytes = g_wb_waits = g_wb_wait_ns = g_wb_reuses = 0;
        g_alias_draws = g_alias_pages = 0;
        memset(g_wb_reason, 0, sizeof g_wb_reason);
        memset(g_wb_reason_ns, 0, sizeof g_wb_reason_ns);
        memset(g_wb_draw_miss, 0, sizeof g_wb_draw_miss);
    }
    /* a sixteenth of the entries every fourth present: an entry goes at most 64 presents after its time. One with a
       buffer of its own goes after twenty seconds unused. One without holds only what its next check compares with:
       ranges read once (constants streamed through a ring, vertices at new offsets) made most of the entries, and the
       lookups of the ones in use missed the cache over them, so those go after two seconds */
    static u32 sweep;
    u32 before = vk_present_before(20000000000ull, 120), before_copy = vk_present_before(2000000000ull, 12);
    if ((!before && !before_copy) || !g_nvb) return;
    u32 chunk = (g_nvb + 15) / 16, from = (sweep++ & 15) * chunk;
    for (u32 i = from ? from : 1; i < from + chunk && i < g_nvb; i++) {
        u32 by = g_vb[i].own ? before : before_copy;
        if (!g_vb[i].len || !by || (i32)(g_vb[i].used - by) >= 0) continue; /* free, or used since */
        drop_own(i);
        vb_free(i);
    }
}
void vk_buf_frame(void) {
    VkResourceScope scope = vk_resource_begin();
    buf_frame();
    vk_resource_end(VK_RESOURCE_BUFFER_MAINTENANCE, scope);
}

/* a save state's load: every buffer kept of guest memory goes, and their host addresses with it */
void vk_buf_forget_all(void) {
    u32 i;
    vk_uma_forget_all();
    for (i = 1; i < g_nvb; i++) /* entry 0 is none: vb_new never writes it, and its bytes are what malloc gave */
        if (g_vb[i].len) {
            drop_own(i);
            vb_free(i);
        }
    vk_buf_mapping_changed();
}
