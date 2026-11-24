/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The pool map that SM86 programs read (the setting pool_map, ShCaps.pool_map, shader.h's SH_MAP_* layout). A record
   for each TIC index of the draws' texture pool holds, for each view class, the view a draw bound for it and that
   view's slot in the texture table; a record for each TSC index holds the plain and the depth compare sampler and
   their slots. A draw binds a slot's view and sampler as before (render.c stage_set_), and where the record holds the
   same view and sampler, the slot takes them from the map and needs no texture word (vk_pool_gpu_take). Else the draw
   takes its texture word and the record learns the new ones (vk_pool_gpu_learn), for the draws after it: the record
   is never trusted over what the draw binds, so it needs no event but the handles of views that go (a new view can
   take a destroyed one's handle, and its slot another view): vk_pool_gpu_view_gone, from vk_image_view_destroy and
   from the table's own reclaim.

   The GPU reads the map from an arena of each frame slot, host memory with a device address, which a new frame takes
   from its first word again: the GPU work of the frame that last used the slot is done by then (cmd.c). A draw
   (vk_pool_gpu_draw) writes the pages that changed since the last draw, a new directory of each pool and a new
   header, and names that header: pages, directories and headers are never written again while a frame may read
   them. A draw that gets no room takes its texture words, and every program still reads a map: the permanent empty
   map then, which names the empty pages for every index. All under vk_lock, on the GPU thread */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "common/platform/thread.h"
#include "common/utilities/endian.h"
#include "nx/gpu/shader/shader.h"
#include "nx/gpu/vk/devmem.h"
#include "nx/gpu/vk/ledger.h"
#include "nx/gpu/vk/pool_map.h"
#include "nx/runtime/platform/log.h"

#define TIC_PAGES   (1u << (20 - SH_MAP_PAGE_SHIFT))
#define TSC_PAGES   (1u << (12 - SH_MAP_PAGE_SHIFT))
#define PAGE        (1u << SH_MAP_PAGE_SHIFT)
#define ARENA_BYTES (8u << 20)
#define VIEWS_GONE  1024

typedef struct {
    VkImageView view[SH_MAP_CLASSES];
    u16 slot[SH_MAP_CLASSES];
    u8 any;
} GpuTic;
typedef struct {
    VkSampler smp[2];
    u16 slot[2];
} GpuTsc;
static struct {
    /* the records of the pools of the draws */
    u64 tex_pool, smp_pool;
    GpuTic* tic[TIC_PAGES];
    GpuTsc* tsc[TSC_PAGES];
    /* the arenas, and the frame's use of its slot's */
    VkBuffer buf[VK_FRAMES];
    DevMem mem[VK_FRAMES];
    u8* mapped[VK_FRAMES];
    u64 address[VK_FRAMES];
    int made; /* 1 made, -1 could not be */
    u64 frame;
    u32 slot, used, words; /* the arena's words */
    u64 full_frame;        /* the frame whose arena ran out, +1 */
    u32 tic_at[TIC_PAGES], tsc_at[TSC_PAGES]; /* where each page is in this frame's arena, 0 for not written */
    u8 tic_dirty[TIC_PAGES], tsc_dirty[TSC_PAGES];
    u32 zero_tic, zero_tsc; /* the empty pages in this frame's arena, 0 for not written */
    u32 tic_pages, tsc_pages, header;
    int stale;
    /* the permanent empty map */
    VkBuffer empty_buf;
    DevMem empty_mem;
    u64 empty_address;
    int null_written;
    /* views that went, for the next lookup to forget (vk_pool_gpu_view_gone) */
    PMutex lk;
    VkImageView gone[VIEWS_GONE];
    u32 ngone;
    int gone_over;
    _Atomic u32 gone_any;
    VkPoolGpuCounts n;
} g_pg = {.lk = PMUTEX_INIT};

static int arena_make(u32 k, u64 bytes, VkBuffer* buf, DevMem* mem, u8** mapped, u64* address) {
    const VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                     .size = bytes,
                                     .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                     .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    const DevMemWant want = {.at = DM_HOST,
                             .mapped = 1,
                             .need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    DevMemInfo details;
    (void)k;
    if (!VK_OK(vk_devmem_buffer(&info, &want, buf, mem, &details)) || !details.ptr) return 0;
    vk_mem_track(*mem, VK_OWN_DESCRIPTORS);
    *mapped = details.ptr;
    *address = vk_buffer_address(*buf);
    return *address != 0;
}
/* The empty map: a header at word 0, a TIC directory of every page and a TSC directory of every page, each entry the
   empty page of its pool */
static int empty_make(void) {
    const u32 tic_dir = 4, tsc_dir = tic_dir + TIC_PAGES, zero_tic = tsc_dir + TSC_PAGES,
              zero_tsc = zero_tic + PAGE * SH_MAP_TIC_WORDS, words = zero_tsc + PAGE * SH_MAP_TSC_WORDS;
    u8* p;
    if (!arena_make(0, (u64)words * 4, &g_pg.empty_buf, &g_pg.empty_mem, &p, &g_pg.empty_address)) return 0;
    memset(p, 0, (size_t)words * 4);
    snx_st_le32(p + 4 * SH_MAP_TIC_DIR, tic_dir);
    snx_st_le32(p + 4 * SH_MAP_TSC_DIR, tsc_dir);
    for (u32 i = 0; i < TIC_PAGES; i++) snx_st_le32(p + 4 * (tic_dir + i), zero_tic);
    for (u32 i = 0; i < TSC_PAGES; i++) snx_st_le32(p + 4 * (tsc_dir + i), zero_tsc);
    return 1;
}
static int made(void) {
    if (g_pg.made) return g_pg.made > 0;
    g_pg.made = -1;
    g_pg.words = ARENA_BYTES / 4;
    for (u32 k = 0; k < VK_FRAMES; k++)
        if (!arena_make(k, ARENA_BYTES, &g_pg.buf[k], &g_pg.mem[k], &g_pg.mapped[k], &g_pg.address[k])) {
            LOG("vulkan: no host memory with a device address for the pool map: textures take their words\n");
            return 0;
        }
    if (!empty_make()) return 0;
    g_pg.n.gpu_bytes = (u64)VK_FRAMES * ARENA_BYTES;
    g_pg.made = 1;
    return 1;
}
u64 vk_pool_gpu_empty(u32* header) {
    *header = 0;
    return made() ? g_pg.empty_address : 0;
}
/* n words into this frame's arena: their word index, 0 when there is no room */
static u32 put(const u32* words, u32 n) {
    if (g_pg.used + n > g_pg.words) return 0;
    u8* p = g_pg.mapped[g_pg.slot] + (size_t)g_pg.used * 4;
    for (u32 i = 0; i < n; i++) snx_st_le32(p + 4 * i, words[i]);
    const u32 at = g_pg.used;
    g_pg.used += n;
    g_pg.n.words_written += n;
    return at;
}
static void records_free(void) {
    for (u32 p = 0; p < TIC_PAGES; p++) free(g_pg.tic[p]), g_pg.tic[p] = NULL;
    for (u32 p = 0; p < TSC_PAGES; p++) free(g_pg.tsc[p]), g_pg.tsc[p] = NULL;
    memset(g_pg.tic_dirty, 1, sizeof g_pg.tic_dirty);
    memset(g_pg.tsc_dirty, 1, sizeof g_pg.tsc_dirty);
    g_pg.stale = 1;
}
/* the views that went: each record that holds one forgets it */
static void take_gone(void) {
    if (!atomic_load_explicit(&g_pg.gone_any, memory_order_acquire)) return;
    static VkImageView gone[VIEWS_GONE];
    pm_lock(&g_pg.lk);
    const u32 n = g_pg.ngone;
    const int over = g_pg.gone_over;
    memcpy(gone, g_pg.gone, n * sizeof *gone);
    g_pg.ngone = 0;
    g_pg.gone_over = 0;
    atomic_store_explicit(&g_pg.gone_any, 0, memory_order_release);
    pm_unlock(&g_pg.lk);
    for (u32 p = 0; p < TIC_PAGES; p++) {
        GpuTic* page = g_pg.tic[p];
        if (!page) continue;
        for (u32 i = 0; i < PAGE; i++) {
            if (!page[i].any) continue;
            for (u32 c = 0; c < SH_MAP_CLASSES; c++) {
                if (!page[i].view[c]) continue;
                int hit = over;
                for (u32 k = 0; k < n && !hit; k++) hit = gone[k] == page[i].view[c];
                if (!hit) continue;
                page[i].view[c] = VK_NULL_HANDLE;
                page[i].slot[c] = 0;
                g_pg.tic_dirty[p] = 1;
                g_pg.stale = 1;
                g_pg.n.forgotten++;
            }
        }
    }
}
void vk_pool_gpu_view_gone(VkImageView v) {
    if (!g_spirv_translate.pool_map || !v) return;
    pm_lock(&g_pg.lk);
    if (g_pg.ngone < VIEWS_GONE)
        g_pg.gone[g_pg.ngone++] = v;
    else
        g_pg.gone_over = 1;
    atomic_store_explicit(&g_pg.gone_any, 1, memory_order_release);
    pm_unlock(&g_pg.lk);
}
/* the null texture and the default sampler at slot 0 of the table, once */
static int null_slots(void) {
    if (g_pg.null_written) return 1;
    const u32 tsc[8] = {0};
    g_pg.null_written = vk_table_null(vk_null_view(VK_IMAGE_VIEW_TYPE_2D, 0, 0), vk_sampler(tsc, 0, 0));
    return g_pg.null_written;
}
int vk_pool_gpu_draw(u64 tex_pool, u32 tex_max, u64 smp_pool, u32 smp_max, u64* address, u32* header) {
    if (!made() || !null_slots()) return 0;
    take_gone();
    if (tex_pool != g_pg.tex_pool || smp_pool != g_pg.smp_pool) {
        records_free();
        g_pg.tex_pool = tex_pool;
        g_pg.smp_pool = smp_pool;
        g_pg.n.switches++;
    }
    const u64 frame = vk_frame_id();
    if (frame != g_pg.frame) {
        g_pg.frame = frame;
        g_pg.slot = vk_frame_slot() % VK_FRAMES;
        g_pg.used = 4; /* word 0 is no place: 0 stands for none */
        memset(g_pg.tic_at, 0, sizeof g_pg.tic_at);
        memset(g_pg.tsc_at, 0, sizeof g_pg.tsc_at);
        g_pg.zero_tic = g_pg.zero_tsc = 0;
        g_pg.stale = 1;
    }
    if (g_pg.full_frame == frame + 1) return 0;
    const u32 tic_pages = ((tex_max > 0xFFFFF ? 0xFFFFF : tex_max) >> SH_MAP_PAGE_SHIFT) + 1,
              tsc_pages = ((smp_max > 0xFFF ? 0xFFF : smp_max) >> SH_MAP_PAGE_SHIFT) + 1;
    if (tic_pages != g_pg.tic_pages || tsc_pages != g_pg.tsc_pages) g_pg.stale = 1;
    if (!g_pg.stale) {
        *address = g_pg.address[g_pg.slot];
        *header = g_pg.header;
        return 1;
    }
    static u32 words[PAGE * SH_MAP_TIC_WORDS];
    int ok = 1;
    for (u32 p = 0; p < tic_pages && ok; p++) {
        if (g_pg.tic_at[p] && !g_pg.tic_dirty[p]) continue;
        const GpuTic* page = g_pg.tic[p];
        if (!page) {
            if (!g_pg.zero_tic) {
                memset(words, 0, PAGE * SH_MAP_TIC_WORDS * 4);
                ok = (g_pg.zero_tic = put(words, PAGE * SH_MAP_TIC_WORDS)) != 0;
            }
            g_pg.tic_at[p] = g_pg.zero_tic;
        } else {
            memset(words, 0, PAGE * SH_MAP_TIC_WORDS * 4);
            for (u32 i = 0; i < PAGE; i++)
                for (u32 c = 0; c < SH_MAP_CLASSES; c++)
                    words[i * SH_MAP_TIC_WORDS + c / 2] |= (u32)page[i].slot[c] << (16 * (c & 1));
            ok = (g_pg.tic_at[p] = put(words, PAGE * SH_MAP_TIC_WORDS)) != 0;
            g_pg.n.pages_written++;
        }
        g_pg.tic_dirty[p] = 0;
    }
    for (u32 p = 0; p < tsc_pages && ok; p++) {
        if (g_pg.tsc_at[p] && !g_pg.tsc_dirty[p]) continue;
        const GpuTsc* page = g_pg.tsc[p];
        if (!page) {
            if (!g_pg.zero_tsc) {
                memset(words, 0, PAGE * SH_MAP_TSC_WORDS * 4);
                ok = (g_pg.zero_tsc = put(words, PAGE * SH_MAP_TSC_WORDS)) != 0;
            }
            g_pg.tsc_at[p] = g_pg.zero_tsc;
        } else {
            for (u32 i = 0; i < PAGE; i++) {
                words[i * SH_MAP_TSC_WORDS] = (u32)page[i].slot[0] | (u32)page[i].slot[1] << 16;
                words[i * SH_MAP_TSC_WORDS + 1] = 0;
            }
            ok = (g_pg.tsc_at[p] = put(words, PAGE * SH_MAP_TSC_WORDS)) != 0;
            g_pg.n.pages_written++;
        }
        g_pg.tsc_dirty[p] = 0;
    }
    u32 tic_dir = ok ? put(g_pg.tic_at, tic_pages) : 0, tsc_dir = tic_dir ? put(g_pg.tsc_at, tsc_pages) : 0;
    const u32 head[2] = {tic_dir, tsc_dir};
    const u32 h = tsc_dir ? put(head, 2) : 0;
    if (!h) { /* no room until the next frame: the pages not written stay dirty */
        g_pg.full_frame = frame + 1;
        g_pg.n.full++;
        memset(g_pg.tic_at, 0, sizeof g_pg.tic_at);
        memset(g_pg.tsc_at, 0, sizeof g_pg.tsc_at);
        g_pg.stale = 1;
        return 0;
    }
    g_pg.tic_pages = tic_pages;
    g_pg.tsc_pages = tsc_pages;
    g_pg.header = h;
    g_pg.stale = 0;
    g_pg.n.versions++;
    *address = g_pg.address[g_pg.slot];
    *header = h;
    return 1;
}
int vk_pool_gpu_take(u32 handle, u32 cls, int shadow, VkImageView view, VkSampler smp, u32* image_slot) {
    if (atomic_load_explicit(&g_pg.gone_any, memory_order_acquire)) return 0; /* the next draw takes them in */
    const u32 tic = handle & 0xFFFFFu, tsc = (handle >> 20) & 0xFFFu;
    const GpuTic* page = g_pg.tic[tic >> SH_MAP_PAGE_SHIFT];
    const GpuTsc* spage = g_pg.tsc[tsc >> SH_MAP_PAGE_SHIFT];
    /* the page as the draw's map holds it: one the draw changed is the next draw's */
    if (!page || !spage || g_pg.tic_dirty[tic >> SH_MAP_PAGE_SHIFT] || g_pg.tsc_dirty[tsc >> SH_MAP_PAGE_SHIFT])
        return 0;
    const GpuTic* r = &page[tic & (PAGE - 1)];
    const GpuTsc* s = &spage[tsc & (PAGE - 1)];
    if (r->view[cls] != view || !r->slot[cls] || s->smp[shadow] != smp || !s->slot[shadow]) return 0;
    *image_slot = r->slot[cls];
    return 1;
}
void vk_pool_gpu_learn(u32 handle, u32 cls, int shadow, VkImageView view, VkSampler smp, u32 image_slot,
                       u32 sampler_slot) {
    const u32 tic = handle & 0xFFFFFu, tsc = (handle >> 20) & 0xFFFu;
    if (!image_slot || image_slot > 0xFFFF || !sampler_slot || sampler_slot > 0xFFFF) return;
    GpuTic** tp = &g_pg.tic[tic >> SH_MAP_PAGE_SHIFT];
    GpuTsc** sp = &g_pg.tsc[tsc >> SH_MAP_PAGE_SHIFT];
    if (!*tp && !(*tp = calloc(PAGE, sizeof **tp))) return;
    if (!*sp && !(*sp = calloc(PAGE, sizeof **sp))) return;
    GpuTsc* s = &(*sp)[tsc & (PAGE - 1)];
    if (s->smp[shadow] && s->smp[shadow] != smp) { /* the index's other sampler stays: this slot takes its word */
        g_pg.n.conflicts++;
        return;
    }
    if (!s->smp[shadow]) {
        s->smp[shadow] = smp;
        s->slot[shadow] = (u16)sampler_slot;
        g_pg.tsc_dirty[tsc >> SH_MAP_PAGE_SHIFT] = 1;
    }
    GpuTic* r = &(*tp)[tic & (PAGE - 1)];
    r->view[cls] = view;
    r->slot[cls] = (u16)image_slot;
    r->any = 1;
    g_pg.tic_dirty[tic >> SH_MAP_PAGE_SHIFT] = 1;
    g_pg.stale = 1;
    g_pg.n.learned++;
}
void vk_pool_gpu_count(int why) {
    if (why >= 0 && why < VK_POOL_GPU_WHY_N) g_pg.n.why[why]++;
}
void vk_pool_gpu_counts(VkPoolGpuCounts* c) { *c = g_pg.n; }
void vk_pool_gpu_forget(void) {
    records_free();
    g_pg.tex_pool = g_pg.smp_pool = 0;
}
void vk_pool_gpu_log(u64 presents) {
    if (!g_spirv_translate.pool_map) return;
    static VkPoolGpuCounts was;
    const VkPoolGpuCounts* n = &g_pg.n;
    const double p = presents ? (double)presents : 1.0;
    LOG("vulkan: pool map, a present: %.1f slots through the map; their own words for %.1f a record the draw could not "
        "use, %.1f memory or indexed handles, %.1f a view of another surface or layout, %.1f handles past the pools, "
        "%.1f a colour depth compare, %.1f no map for the draw; %.1f records learned (%.1f samplers kept over another), "
        "%.1f forgotten; %.1f pages and %.1f KB written, %.1f versions, %.1f arenas full; %llu MB of map memory\n",
        (double)(n->why[VK_POOL_GPU_MAP] - was.why[VK_POOL_GPU_MAP]) / p,
        (double)(n->why[VK_POOL_GPU_MISS] - was.why[VK_POOL_GPU_MISS]) / p,
        (double)(n->why[VK_POOL_GPU_HANDLE] - was.why[VK_POOL_GPU_HANDLE]) / p,
        (double)(n->why[VK_POOL_GPU_VIEW] - was.why[VK_POOL_GPU_VIEW]) / p,
        (double)(n->why[VK_POOL_GPU_PAST] - was.why[VK_POOL_GPU_PAST]) / p,
        (double)(n->why[VK_POOL_GPU_COMPARE] - was.why[VK_POOL_GPU_COMPARE]) / p,
        (double)(n->why[VK_POOL_GPU_NONE] - was.why[VK_POOL_GPU_NONE]) / p, (double)(n->learned - was.learned) / p,
        (double)(n->conflicts - was.conflicts) / p, (double)(n->forgotten - was.forgotten) / p,
        (double)(n->pages_written - was.pages_written) / p, (double)(n->words_written - was.words_written) * 4 / 1024 / p,
        (double)(n->versions - was.versions) / p, (double)(n->full - was.full) / p,
        (unsigned long long)(n->gpu_bytes >> 20));
    was = *n;
}
