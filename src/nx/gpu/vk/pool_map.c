/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The pool map in shadow mode (pool_map.h). Each pool's records are in pages of 256 by index, made when a resolve
   first fills one there, with a list of the valid ones that the sweeps walk. A sweep runs before each slot's compare
   and takes in the events since the last one: each kind of event moves a stamp (the scale, the queues of swaps and
   views, the surface table's epoch, the targets' write generations, and the write tracking's epoch, the GPU's writes and
   the results added for the headers), and a sweep walks the valid records only for the stamps that moved. The walks
   are a cost of this measure, not of the design: a map the GPU reads needs the events by address (step 4) */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "common/platform/thread.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/vk/pool_map.h"
#include "nx/gpu/vk/pending_pages.h"
#include "nx/runtime/platform/log.h"

#define POOL_PAGE_SHIFT 8
#define POOL_PAGE       (1u << POOL_PAGE_SHIFT)
#define POOL_TIC_INDEX  (1u << 20) /* a handle's TIC index has 20 bits, its TSC index 12 */
#define POOL_TSC_INDEX  (1u << 12)
#define POOL_MAPS       8          /* texture and sampler pools held at once, the one used longest ago given up */
#define POOL_SWAPS      64
#define POOL_VIEWS      1024

typedef struct {
    const Surf *t, *over;
    u64 t_gva, over_gva;
    u32 t_bepoch, over_bepoch, over_wgen, view_kind;
    VkImageView view;
    u32 words[8]; /* the TIC, or a sampler record's TSC */
    u32 at;       /* its place in the valid list plus one: 0 is not valid */
} PoolRec;
typedef struct {
    u64 pool; /* 0: a free map */
    int sampler;
    PoolRec** page;
    u32* valid;
    u32 nvalid, cap, top; /* top: the highest index filled, plus one */
    u32 epoch, wseen;     /* the header sweep's stamps, read before it looked */
    u64 gw, adds, used;
} PoolMapOne;
static struct {
    PoolMapOne map[POOL_MAPS];
    u32 surf_epoch, surf_gen;
    float scale;
    u64 use;
    PMutex lk; /* the queues below: their events come from any thread */
    const Surf* swapped[POOL_SWAPS];
    VkImageView views[POOL_VIEWS];
    u32 nswap, nview;
    int swap_over, view_over;
    _Atomic u32 queued; /* any event in the queues */
    PoolMapCounts n, was;
} g_pool = {.lk = PMUTEX_INIT, .scale = 1.0f};

static u64 adds_now(void) { return atomic_load_explicit(&g_pend_any.adds, memory_order_relaxed) + vk_uma_writes(); }
static PoolRec* rec_at(PoolMapOne* m, u32 index, int make) {
    const u32 pages = (m->sampler ? POOL_TSC_INDEX : POOL_TIC_INDEX) >> POOL_PAGE_SHIFT, p = index >> POOL_PAGE_SHIFT;
    if (p >= pages) return NULL;
    if (!m->page) {
        if (!make || !(m->page = calloc(pages, sizeof *m->page))) return NULL;
    }
    if (!m->page[p] && (!make || !(m->page[p] = calloc(POOL_PAGE, sizeof **m->page)))) return NULL;
    return &m->page[p][index & (POOL_PAGE - 1)];
}
static void rec_clear(PoolMapOne* m, u32 index, int cause) {
    PoolRec* r = rec_at(m, index, 0);
    if (!r || !r->at) return;
    const u32 last = m->valid[--m->nvalid];
    if (r->at - 1 < m->nvalid) {
        m->valid[r->at - 1] = last;
        rec_at(m, last, 0)->at = r->at;
    }
    r->at = 0;
    if (cause >= 0) g_pool.n.cleared[cause]++;
}
static int rec_valid(PoolMapOne* m, u32 index, PoolRec* r) {
    if (m->nvalid == m->cap) {
        u32 cap = m->cap ? 2 * m->cap : 256, *v = realloc(m->valid, cap * sizeof *v);
        if (!v) return 0;
        m->valid = v;
        m->cap = cap;
    }
    m->valid[m->nvalid++] = index;
    r->at = m->nvalid;
    if (index + 1 > m->top) m->top = index + 1;
    return 1;
}
/* a new map's stamps: older than any header its records will hold (a resolve read those before the map was made), so
   its first sweep compares each record with memory */
static void map_stamps(PoolMapOne* m) {
    m->epoch = vk_ww_epoch() - 1;
    m->wseen = 0;
    m->gw = 0;
    m->adds = adds_now() - 1;
}
static void map_free(PoolMapOne* m) {
    if (m->page) {
        const u32 pages = (m->sampler ? POOL_TSC_INDEX : POOL_TIC_INDEX) >> POOL_PAGE_SHIFT;
        for (u32 p = 0; p < pages; p++) free(m->page[p]);
        free(m->page);
    }
    free(m->valid);
    *m = (PoolMapOne){0};
}
/* every valid record of every map cleared for cause (-1: not counted) */
static void clear_all(int cause) {
    for (u32 k = 0; k < POOL_MAPS; k++)
        while (g_pool.map[k].pool && g_pool.map[k].nvalid) rec_clear(&g_pool.map[k], g_pool.map[k].valid[0], cause);
}
static PoolMapOne* map_find(u64 pool, int sampler, int make) {
    PoolMapOne* lru = NULL; /* a free map, else the one used longest ago */
    for (u32 k = 0; k < POOL_MAPS; k++) {
        PoolMapOne* m = &g_pool.map[k];
        if (m->pool == pool && m->sampler == sampler) {
            m->used = ++g_pool.use;
            return m;
        }
        if (!lru || (lru->pool && (!m->pool || m->used < lru->used))) lru = m;
    }
    if (!make) return NULL;
    map_free(lru);
    lru->pool = pool;
    lru->sampler = sampler;
    lru->used = ++g_pool.use;
    map_stamps(lru);
    return lru;
}
static int in_views(VkImageView v, const VkImageView* views, u32 n) {
    for (u32 i = 0; i < n; i++)
        if (views[i] == v) return 1;
    return 0;
}
/* the queued swaps and destroyed views: the records of a swapped texture, or with a destroyed view, cleared */
static void sweep_queues(void) {
    if (!atomic_load_explicit(&g_pool.queued, memory_order_acquire)) return;
    pm_lock(&g_pool.lk);
    const Surf* swapped[POOL_SWAPS];
    static VkImageView views[POOL_VIEWS];
    const u32 nswap = g_pool.nswap, nview = g_pool.nview;
    const int swap_over = g_pool.swap_over, view_over = g_pool.view_over;
    memcpy(swapped, g_pool.swapped, nswap * sizeof *swapped);
    memcpy(views, g_pool.views, nview * sizeof *views);
    g_pool.nswap = g_pool.nview = 0;
    g_pool.swap_over = g_pool.view_over = 0;
    atomic_store_explicit(&g_pool.queued, 0, memory_order_release);
    pm_unlock(&g_pool.lk);
    for (u32 k = 0; k < POOL_MAPS; k++) {
        PoolMapOne* m = &g_pool.map[k];
        if (!m->pool || m->sampler) continue;
        for (u32 i = 0; i < m->nvalid;) {
            const u32 index = m->valid[i];
            const PoolRec* r = rec_at(m, index, 0);
            int cause = -1;
            for (u32 s = 0; s < nswap && cause < 0; s++)
                if (r->t == swapped[s]) cause = POOL_CLEAR_SWAP;
            if (cause < 0 && swap_over) cause = POOL_CLEAR_SWAP;
            if (cause < 0 && r->view && (view_over || in_views(r->view, views, nview))) cause = POOL_CLEAR_VIEW;
            if (cause < 0) {
                i++;
                continue;
            }
            rec_clear(m, index, cause); /* the last valid one moves into place i */
        }
    }
}
/* the texture records whose texture or crop target's bucket of the surface table changed (surf_changed: a surface made,
   taken or freed there), then those whose crop target was drawn since. The first before the second: a freed target
   moves its bucket, so the second reads only live targets */
static void sweep_surfaces(void) {
    const u32 epoch = atomic_load_explicit(&g_vk_surf_epoch, memory_order_relaxed), gen = vk_surf_gen_now();
    const int surfaces = epoch != g_pool.surf_epoch, drawn = gen != g_pool.surf_gen;
    if (!surfaces && !drawn) return;
    g_pool.surf_epoch = epoch;
    g_pool.surf_gen = gen;
    for (u32 k = 0; k < POOL_MAPS; k++) {
        PoolMapOne* m = &g_pool.map[k];
        if (!m->pool || m->sampler) continue;
        for (u32 i = 0; i < m->nvalid;) {
            const u32 index = m->valid[i];
            const PoolRec* r = rec_at(m, index, 0);
            int cause = -1;
            if (surfaces && (vk_surf_epoch_at(r->t_gva) != r->t_bepoch ||
                             (r->over && vk_surf_epoch_at(r->over_gva) != r->over_bepoch)))
                cause = POOL_CLEAR_SURFACE;
            else if (drawn && r->over && r->over->wgen != r->over_wgen)
                cause = POOL_CLEAR_DRAWN;
            if (cause < 0) {
                i++;
                continue;
            }
            rec_clear(m, index, cause);
        }
    }
}
/* a map's records whose header changed or is under a pending result, where the pool's filled range was written since
   the map's stamps: by the CPU (the write tracking), by the GPU, or a result added over it */
static void sweep_headers(PoolMapOne* m) {
    const u32 epoch = vk_ww_epoch(), now = vk_ww_now();
    const u64 gw = gpu_mem_generation(), adds = adds_now();
    if (epoch == m->epoch && gw == m->gw && adds == m->adds) return;
    const u64 bytes = (u64)m->top * 32;
    if (bytes) {
        const u8* host = gmem_host(m->pool, bytes);
        const int written = (epoch != m->epoch && (!host || vk_written_since(host, bytes, m->wseen) != 0)) ||
                            (gw != m->gw && gpu_mem_dirty_since(m->gw, m->pool, bytes)) ||
                            (adds != m->adds && pend_hit(&g_pend_any, m->pool, bytes));
        for (u32 i = 0; written && i < m->nvalid;) {
            const u32 index = m->valid[i];
            const PoolRec* r = rec_at(m, index, 0);
            const u64 at = m->pool + (u64)index * 32;
            u32 words[8];
            vk_gmem_words(at, words, 8);
            if (!memcmp(words, r->words, sizeof words) && !pend_hit(&g_pend_any, at, 32)) {
                i++;
                continue;
            }
            rec_clear(m, index, POOL_CLEAR_HEADER);
        }
    }
    m->epoch = epoch;
    m->wseen = now;
    m->gw = gw;
    m->adds = adds;
}
static void sweep(void) {
    if (g_vk_scale != g_pool.scale) {
        clear_all(POOL_CLEAR_SCALE);
        g_pool.scale = g_vk_scale;
    }
    sweep_queues();
    sweep_surfaces();
    for (u32 k = 0; k < POOL_MAPS; k++)
        if (g_pool.map[k].pool) sweep_headers(&g_pool.map[k]);
}
static const char* diff_name(int cause) {
    static const char* const names[POOL_DIFF_N] = {"the surface", "the texture header", "the sampler header",
                                                   "the crop's target", "the view"};
    return names[cause];
}
static void fill_tic(PoolMapOne* m, u32 index, PoolRec* r, const PoolSlot* s) {
    *r = (PoolRec){.t = s->t,
                   .over = s->over,
                   .t_gva = s->t->gva,
                   .over_gva = s->over ? s->over->gva : 0,
                   .t_bepoch = vk_surf_epoch_at(s->t->gva),
                   .over_bepoch = s->over ? vk_surf_epoch_at(s->over->gva) : 0,
                   .over_wgen = s->over_wgen,
                   .view_kind = s->view_kind,
                   .view = s->view};
    memcpy(r->words, s->tic, sizeof r->words);
    if (rec_valid(m, index, r)) g_pool.n.filled++;
}
void vk_pool_map_slot(u64 tex_pool, u32 tex_max, u64 smp_pool, u32 smp_max, u32 handle, const PoolSlot* s) {
    const u32 tic = handle & 0xFFFFFu, tsc = (handle >> 20) & 0xFFFu;
    if (tic > tex_max || tsc > smp_max || !tex_pool || !smp_pool) {
        g_pool.n.past++;
        return;
    }
    sweep();
    PoolMapOne *tm = map_find(tex_pool, 0, 1), *sm = map_find(smp_pool, 1, 1);
    PoolRec* r = rec_at(tm, tic, 1);
    if (r && r->at) {
        const int cause = r->t != s->t                          ? POOL_DIFF_SURFACE
                          : memcmp(r->words, s->tic, 32)        ? POOL_DIFF_HEADER
                          : r->over != s->over || (s->over && r->over_wgen != s->over_wgen) ? POOL_DIFF_OVER
                          : s->view && r->view && r->view_kind == s->view_kind && r->view != s->view ? POOL_DIFF_VIEW
                                                                                                      : -1;
        if (cause < 0) {
            g_pool.n.agree++;
            if (s->view && !r->view) r->view = s->view, r->view_kind = s->view_kind;
        } else {
            g_pool.n.differ[cause]++;
            if (g_settings.draw_state_check)
                WARN_LIMIT(16,
                           "vulkan: pool_map_shadow: handle %08x (TIC %u) differs in %s: the record holds the surface "
                           "at %llx (%ux%u, view %llx), the resolve the one at %llx (%ux%u, view %llx)\n",
                           handle, tic, diff_name(cause), (unsigned long long)r->t_gva, r->t ? r->t->w : 0,
                           r->t ? r->t->h : 0, (unsigned long long)(uintptr_t)r->view,
                           (unsigned long long)(s->t ? s->t->gva : 0), s->t ? s->t->w : 0, s->t ? s->t->h : 0,
                           (unsigned long long)(uintptr_t)s->view);
            rec_clear(tm, tic, -1); /* filled again below, to measure again */
        }
    }
    if (r && !r->at) {
        if (s->t && s->keepable)
            fill_tic(tm, tic, r, s);
        else
            g_pool.n.unkept++;
    }
    PoolRec* q = rec_at(sm, tsc, 1);
    if (q && q->at) {
        if (!memcmp(q->words, s->tsc, 32)) {
            g_pool.n.samplers_agree++;
            return;
        }
        g_pool.n.differ[POOL_DIFF_SAMPLER]++;
        if (g_settings.draw_state_check)
            WARN_LIMIT(16, "vulkan: pool_map_shadow: handle %08x (TSC %u) differs in the sampler header\n", handle, tsc);
        rec_clear(sm, tsc, -1);
    }
    if (q) {
        *q = (PoolRec){0};
        memcpy(q->words, s->tsc, sizeof q->words);
        rec_valid(sm, tsc, q);
    }
}
static void queue_event(const Surf* t, VkImageView v) {
    if (!g_settings.pool_map_shadow) return;
    pm_lock(&g_pool.lk);
    if (t) {
        if (g_pool.nswap < POOL_SWAPS)
            g_pool.swapped[g_pool.nswap++] = t;
        else
            g_pool.swap_over = 1;
    } else if (g_pool.nview < POOL_VIEWS)
        g_pool.views[g_pool.nview++] = v;
    else
        g_pool.view_over = 1;
    atomic_store_explicit(&g_pool.queued, 1, memory_order_release);
    pm_unlock(&g_pool.lk);
}
void vk_pool_map_swapped(const Surf* t) {
    if (t) queue_event(t, VK_NULL_HANDLE);
}
void vk_pool_map_view_gone(VkImageView v) {
    vk_pool_gpu_view_gone(v);
    if (v) queue_event(NULL, v);
}
void vk_pool_map_counts(PoolMapCounts* c) { *c = g_pool.n; }
int vk_pool_map_valid(u64 tex_pool, u32 index) {
    sweep();
    PoolMapOne* m = map_find(tex_pool, 0, 0);
    const PoolRec* r = m ? rec_at(m, index, 0) : NULL;
    return r && r->at;
}
void vk_pool_map_forget(void) {
    for (u32 k = 0; k < POOL_MAPS; k++) map_free(&g_pool.map[k]);
}
void vk_pool_map_log(u64 presents) {
    if (!g_settings.pool_map_shadow) return;
    const PoolMapCounts* n = &g_pool.n;
    const PoolMapCounts* w = &g_pool.was;
    const double p = presents ? (double)presents : 1.0;
    u32 valid = 0;
    for (u32 k = 0; k < POOL_MAPS; k++) valid += g_pool.map[k].pool && !g_pool.map[k].sampler ? g_pool.map[k].nvalid : 0;
    LOG("vulkan: pool map (shadow), a present: %.1f slots agreed with their record, %.1f samplers; differed in the "
        "surface %.1f, the texture header %.1f, the sampler header %.1f, the crop's target %.1f, the view %.1f; %.1f "
        "records filled, %.1f resolves no record holds, %.1f handles past the pools; cleared by a header write %.1f, "
        "the surface table %.1f, a draw to a crop's target %.1f, the scale %.1f, an image swap %.1f, a view destroyed "
        "%.1f; %u texture records valid\n",
        (double)(n->agree - w->agree) / p, (double)(n->samplers_agree - w->samplers_agree) / p,
        (double)(n->differ[POOL_DIFF_SURFACE] - w->differ[POOL_DIFF_SURFACE]) / p,
        (double)(n->differ[POOL_DIFF_HEADER] - w->differ[POOL_DIFF_HEADER]) / p,
        (double)(n->differ[POOL_DIFF_SAMPLER] - w->differ[POOL_DIFF_SAMPLER]) / p,
        (double)(n->differ[POOL_DIFF_OVER] - w->differ[POOL_DIFF_OVER]) / p,
        (double)(n->differ[POOL_DIFF_VIEW] - w->differ[POOL_DIFF_VIEW]) / p, (double)(n->filled - w->filled) / p,
        (double)(n->unkept - w->unkept) / p, (double)(n->past - w->past) / p,
        (double)(n->cleared[POOL_CLEAR_HEADER] - w->cleared[POOL_CLEAR_HEADER]) / p,
        (double)(n->cleared[POOL_CLEAR_SURFACE] - w->cleared[POOL_CLEAR_SURFACE]) / p,
        (double)(n->cleared[POOL_CLEAR_DRAWN] - w->cleared[POOL_CLEAR_DRAWN]) / p,
        (double)(n->cleared[POOL_CLEAR_SCALE] - w->cleared[POOL_CLEAR_SCALE]) / p,
        (double)(n->cleared[POOL_CLEAR_SWAP] - w->cleared[POOL_CLEAR_SWAP]) / p,
        (double)(n->cleared[POOL_CLEAR_VIEW] - w->cleared[POOL_CLEAR_VIEW]) / p, valid);
    g_pool.was = g_pool.n;
}
