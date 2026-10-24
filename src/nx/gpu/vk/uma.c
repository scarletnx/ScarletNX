/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Unified memory: guest memory imported as buffers (VK_EXT_external_memory_host), so draws read vertices, indices and
   storage buffers where the guest's CPU wrote them, as the console's GPU does, with no copy and nothing to check.
   Without it (the uma setting off, or a GPU without the extension) buffer.c copies them and watches for the CPU's
   writes.

   Imports are spans of the reservation's own committed pages, the UMA_CHUNK aligned chunk around a range or the two
   it reaches across (a longer range: its own pages), sorted by where they start (they may overlap), and let go of
   when no draw has used them for UMA_IDLE presents. They are small and few because a driver may walk an import's
   pages at every submit that names it: amdgpu takes a userptr buffer's pages again (hmm_range_fault over all of
   them) in each command submission, and RADV's imports are userptr buffers. 64 MiB chunks around whatever a draw
   touched, kept for good, were 16384 pages each, every submit. The
   alias pool's pages are never imported: GPU-owned memory lives there, trapped (docs/GPU_OWNERSHIP.md), and a driver
   that takes the pages again at each submit (RADV and ANV take imports through MMU notifiers) would fault on a trapped
   page from the kernel, where userfaultfd answers EFAULT. A driver that pins the pages (NVIDIA) holds on to the old
   ones when guest memory is unmapped or moved, so an import whose pages change is retired (mem.c's hook), destroyed
   once the submits that read it are done, and the range imported again.

   What waits: the guest's fences already order its CPU's writes after the GPU's reads (vk.h), but the runtime writes
   guest memory too while it records (inline to memory, constant buffer updates, ranges becoming GPU-owned), and the
   draws recorded before such a write must not see it. Two page tables keep that apart: front, the pages the runtime
   has written that way, which are never read in place again (buffer.c copies them as before), and live, the submit
   that last read each page in place. A first write to a page a submit not yet done reads waits for it.

   Shaders write storage buffers in place too (vk_uma_write): a draw's or dispatch's writes are pending (out, counted
   in out_pages and so in g_pend_any) until its submit is done, with nothing to land. What reads or writes those bytes
   on the GPU after them in place comes after a barrier (order), what copies them on the GPU copies them from the import
   (owned.c's cpu_read), and the runtime's own CPU readers wait for the submit (vk_uma_mem_need), as do its writes.
   The submit's end makes them visible to the host (vk_uma_host_barrier) before the release that tells the guest. */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/maxwell/write_pages.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/memory/own.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/pending_pages.h"

#define UMA_CHUNK   (2ull << 20) /* what one import covers around a range, aligned to it */
#define UMA_IDLE    120          /* presents an import may go unused before it is let go */
#define UMA_IMPORTS 1024          /* imports at most: the device may have as few as 4096 allocations */
#define UMA_MISS_N  256           /* spans that couldn't be imported, not tried again until memory changes */
#define UMA_USE_N   1024          /* ranges stamped into live in the submit being recorded */
#define UMA_PREP_N  64            /* in-place writes of one draw or dispatch */

typedef struct {
    u64 lo, hi; /* host offsets into g_mem */
    VkBuffer buf;
    VkDeviceMemory mem;
    u64 used; /* the submit that last read it */
    u32 seen; /* g_vk_present then */
} UmaImport;
typedef struct {
    u64 cpu, len, gen;
} UmaMiss;
typedef struct {
    u64 cpu, len, submit;
} UmaUse;
typedef struct {
    u64 a, n;
} UmaChange;
typedef struct {
    u64 gva, cpu, len, submit;
} UmaOut;

static struct {
    UmaImport imp[UMA_IMPORTS]; /* sorted by lo */
    u32 n, last;                /* imports; the last one found */
    u64 max_len;                /* the longest import made: none starts further than that before what it holds */
    u32 mem_type_bits_ok;       /* memoryTypeBits of the host-coherent types */
    u64 bytes, waits, refused;
    u64 gen;      /* moves when imports are retired: the misses before it may import now */
    u64 recorded; /* the submit being recorded when an import was last read */
    UmaMiss miss[UMA_MISS_N];
    UmaUse use[UMA_USE_N];
    WritePages front, live;
    /* in-place writes: the draw's being set up, and those recorded, oldest first, until their submit is done */
    UmaOut prep[UMA_PREP_N];
    u32 nprep, nout, capout;
    UmaOut* out;
    PendPages out_pages;
    /* the pages shaders wrote and read in place since the last barrier: stamped with barrier_no + 1 */
    WritePages written, read;
    UmaUse rmemo[UMA_USE_N]; /* ranges stamped into read since that barrier */
    u64 barrier_no, barriers, out_waits;
    /* ranges whose host pages changed, noted under mem.c's lock and retired on the next lookup under vk_lock */
    PMutex chg_lk;
    UmaChange chg[64];
    int nchg; /* atomic; past the array every import is retired */
} g_uma = {.chg_lk = PMUTEX_INIT, .out_pages = {.also = &g_pend_any}};
static _Thread_local int t_gpu_reads; /* vk_uma_gpu_reads: the caller copies pending writes on the GPU */

static void import_free(void* arg) {
    UmaImport* m = arg;
    vkDestroyBuffer(g_vk.dev, m->buf, NULL);
    vkFreeMemory(g_vk.dev, m->mem, NULL);
    free(m);
}
/* import i goes: destroyed once the submits that may have read it are done */
static void retire(u32 i) {
    UmaImport* m = malloc(sizeof *m);
    if (!m) { /* nowhere to keep it until then: the GPU finishes with it now */
        vk_finish();
        m = &g_uma.imp[i];
        vkDestroyBuffer(g_vk.dev, m->buf, NULL);
        vkFreeMemory(g_vk.dev, m->mem, NULL);
    } else {
        *m = g_uma.imp[i];
        vk_defer_call(import_free, m);
    }
    g_uma.bytes -= g_uma.imp[i].hi - g_uma.imp[i].lo;
    memmove(&g_uma.imp[i], &g_uma.imp[i + 1], (g_uma.n - i - 1) * sizeof *g_uma.imp);
    g_uma.n--;
    g_uma.last = 0;
    g_uma.gen++;
}
static void retire_range(u64 a, u64 n) {
    for (u32 i = g_uma.n; i-- > 0;)
        if (g_uma.imp[i].lo < a + n && a < g_uma.imp[i].hi) retire(i);
}
static void backing_changed(u64 a, u64 n) {
    pm_lock(&g_uma.chg_lk);
    int k = g_uma.nchg;
    if (k < (int)(sizeof g_uma.chg / sizeof *g_uma.chg)) g_uma.chg[k] = (UmaChange){a, n};
    __atomic_store_n(&g_uma.nchg, k + 1, __ATOMIC_RELEASE);
    pm_unlock(&g_uma.chg_lk);
}
static void take_changes(void) {
    UmaChange c[64];
    int k;
    if (!__atomic_load_n(&g_uma.nchg, __ATOMIC_ACQUIRE)) return;
    pm_lock(&g_uma.chg_lk);
    k = g_uma.nchg;
    if (k <= (int)(sizeof c / sizeof *c)) memcpy(c, g_uma.chg, (size_t)k * sizeof *c);
    __atomic_store_n(&g_uma.nchg, 0, __ATOMIC_RELEASE);
    pm_unlock(&g_uma.chg_lk);
    if (k > (int)(sizeof c / sizeof *c))
        while (g_uma.n) retire(g_uma.n - 1);
    else
        for (int i = 0; i < k; i++) retire_range(c[i].a, c[i].n);
    g_uma.gen++; /* pages that couldn't be imported may be importable now */
}

void vk_uma_init(void) {
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &host};
    VkPhysicalDeviceMemoryProperties mp;
    if (!g_vk.uma) return;
    vkGetPhysicalDeviceProperties2(g_vk.phys, &props);
    g_vk.uma_align = host.minImportedHostPointerAlignment ? host.minImportedHostPointerAlignment : 4096;
    /* host-coherent: the CPU's writes before a submit are seen by it, with no flush (an import has no mapping to
       flush) */
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
    for (u32 i = 0; i < mp.memoryTypeCount; i++)
        if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) g_uma.mem_type_bits_ok |= 1u << i;
    if (!g_uma.mem_type_bits_ok || g_vk.uma_align & (g_vk.uma_align - 1) || g_vk.uma_align > UMA_CHUNK) {
        WARN("vulkan: unified memory was asked for, but the GPU imports host memory only in a way it can't be used "
             "(no host-coherent memory type, or %llu-byte alignment): guest buffers are copied for it\n",
             (unsigned long long)g_vk.uma_align);
        g_vk.uma = 0;
        return;
    }
    mem_set_backing_hook(backing_changed);
    /* one or the other: the caches that asked write tracking compare contents from now on (plat.h) */
    plat_ww_off();
    LOG("vulkan: unified memory: draws read guest buffers in place (VK_EXT_external_memory_host, imports aligned to "
        "%llu bytes)%s\n",
        (unsigned long long)g_vk.uma_align,
        !g_vk.uma_shared           ? ", on a discrete GPU: the CPU checks textures, which the GPU doesn't read across PCIe"
        : g_vk.copy_indirect == 1  ? ", the GPU checks textures and turns their rebuilds off with VK_KHR_copy_memory_indirect"
        : g_vk.copy_indirect == 2  ? ", the GPU checks textures and turns their rebuilds off with VK_NV_copy_memory_indirect"
                                   : ", the GPU checks textures and turns their rebuilds off storing what their copies would");
}

/* the index of the first import starting after cpu (g_uma.n when none does) */
static u32 import_after(u64 cpu) {
    u32 lo = 0, hi = g_uma.n;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (g_uma.imp[mid].lo <= cpu)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
/* an import holding [cpu, cpu + len), or g_uma.n. Imports may overlap (a range across a chunk's end takes both
   chunks), and none is longer than max_len, so only those starting less than that before cpu can hold it */
static u32 import_find(u64 cpu, u64 len) {
    for (u32 k = import_after(cpu); k-- > 0 && g_uma.imp[k].lo + g_uma.max_len > cpu;)
        if (cpu + len <= g_uma.imp[k].hi) return k;
    return g_uma.n;
}
/* host offsets [lo, hi) imported as import k (the imports after it move up): 0 when the driver refuses them */
static int import_span(u32 k, u64 lo, u64 hi) {
    VkMemoryHostPointerPropertiesEXT pointer = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkExternalMemoryBufferCreateInfo ext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = &ext,
        .size = hi - lo,
        /* every use a staging ring's buffer has (cmd.c's stage_alloc): draws take either */
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
                 vk_bda_usage() | (g_vk.has_xfb ? VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT : 0),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkImportMemoryHostPointerInfoEXT import = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                               .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                               .pHostPointer = GP(lo)};
    /* the texture check's copies read guest memory at device addresses */
    VkMemoryAllocateFlagsInfo flags = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                       .pNext = &import,
                                       .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &import, .allocationSize = hi - lo};
    if (g_vk.has_bda) ai.pNext = &flags;
    VkMemoryRequirements req;
    UmaImport m = {.lo = lo, .hi = hi};
    u32 bits;
    if (vkGetMemoryHostPointerPropertiesEXT(g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, GP(lo),
                                            &pointer) != VK_SUCCESS)
        return 0;
    if (vkCreateBuffer(g_vk.dev, &bi, NULL, &m.buf) != VK_SUCCESS) return 0;
    vkGetBufferMemoryRequirements(g_vk.dev, m.buf, &req);
    bits = req.memoryTypeBits & pointer.memoryTypeBits & g_uma.mem_type_bits_ok;
    if (!bits || req.size > hi - lo) {
        vkDestroyBuffer(g_vk.dev, m.buf, NULL);
        return 0;
    }
    ai.memoryTypeIndex = (u32)__builtin_ctz(bits);
    if (vkAllocateMemory(g_vk.dev, &ai, NULL, &m.mem) != VK_SUCCESS) {
        vkDestroyBuffer(g_vk.dev, m.buf, NULL);
        return 0;
    }
    if (vkBindBufferMemory(g_vk.dev, m.buf, m.mem, 0) != VK_SUCCESS) {
        vkDestroyBuffer(g_vk.dev, m.buf, NULL);
        vkFreeMemory(g_vk.dev, m.mem, NULL);
        return 0;
    }
    memmove(&g_uma.imp[k + 1], &g_uma.imp[k], (g_uma.n - k) * sizeof *g_uma.imp);
    g_uma.imp[k] = m;
    g_uma.n++;
    g_uma.bytes += hi - lo;
    if (hi - lo > g_uma.max_len) g_uma.max_len = hi - lo;
    return 1;
}
/* a new import holding [cpu, cpu + len): the chunk around it, or the two it reaches across, narrowed to the
   reservation's own committed pages; a longer range, its own pages. Its index, or g_uma.n when there can't be one */
static u32 import_new(u64 cpu, u64 len) {
    u64 al = g_vk.uma_align, lo = cpu & ~(UMA_CHUNK - 1), hi = (cpu + len + UMA_CHUNK - 1) & ~(UMA_CHUNK - 1);
    UmaMiss* ms = &g_uma.miss[((cpu >> 12) ^ (len << 7)) % UMA_MISS_N];
    if (ms->cpu == cpu && ms->len == len && ms->gen == g_uma.gen) return g_uma.n;
    if (g_uma.n == UMA_IMPORTS) goto miss;
    if (hi - lo > 2 * UMA_CHUNK) {
        lo = cpu & ~(al - 1);
        hi = (cpu + len + al - 1) & ~(al - 1);
    }
    if (mem_private_span(cpu, len, &lo, &hi)) goto miss;
    if (hi - lo > g_vk.alloc_cap) { /* past the device's cap on one allocation: the range's own pages */
        lo = cpu & ~(al - 1);
        hi = (cpu + len + al - 1) & ~(al - 1);
        if (hi - lo > g_vk.alloc_cap) goto miss;
    }
    lo = (lo + al - 1) & ~(al - 1);
    hi &= ~(al - 1);
    if (cpu < lo || cpu + len > hi || !import_span(import_after(lo), lo, hi)) goto miss;
    return import_find(cpu, len);
miss:
    g_uma.refused++;
    *ms = (UmaMiss){cpu, len, g_uma.gen};
    return g_uma.n;
}

int vk_uma_get(u64 cpu, u64 len, u64 align, VkBuffer* buf, u64* off) {
    UmaImport* m;
    u32 k;
    if (!g_vk.uma || !len || cpu + len < cpu) return 0;
    take_changes();
    m = &g_uma.imp[g_uma.last];
    if (g_uma.last >= g_uma.n || cpu < m->lo || cpu + len > m->hi) {
        k = import_find(cpu, len);
        if (k == g_uma.n) k = import_new(cpu, len);
        if (k == g_uma.n) return 0;
        g_uma.last = k;
        m = &g_uma.imp[k];
    }
    if ((cpu - m->lo) % (align ? align : 1)) return 0; /* the binding's offset the device can't take */
    m->used = g_uma.recorded = vk_submit_number();
    m->seen = g_vk_present;
    *buf = m->buf;
    *off = cpu - m->lo;
    return 1;
}

/* a draw being recorded reads [cpu, cpu + len) in place: until its submit is done, a write there other than the
   guest's waits for it */
static void uma_use(u64 cpu, u64 len) {
    u64 now = vk_submit_number();
    UmaUse* u = &g_uma.use[((cpu >> 8) ^ (len << 3)) % UMA_USE_N];
    if (u->cpu == cpu && u->len == len && u->submit == now) return; /* stamped in this submit already */
    *u = (UmaUse){cpu, len, now};
    write_pages_stamp(&g_uma.live, cpu, len, now);
}
/* A barrier before what reads [cpu, cpu + len) in place, when shaders wrote it in place since the last one, or before
   what writes it, when they read or wrote it: the console's GPU runs its work in order. Outside a rendering pass,
   which a barrier can't be recorded in for these stages */
static void order(u64 cpu, u64 len, int write) {
    if (!write_pages_since(&g_uma.written, g_uma.barrier_no, cpu, len) &&
        !(write && write_pages_since(&g_uma.read, g_uma.barrier_no, cpu, len)))
        return;
    vk_render_end();
    vk_all_barrier(vk_cmd());
    g_uma.barrier_no++;
    g_uma.barriers++;
}
static void note_read(u64 cpu, u64 len) {
    UmaUse* u = &g_uma.rmemo[((cpu >> 8) ^ (len << 3)) % UMA_USE_N];
    if (u->cpu == cpu && u->len == len && u->submit == g_uma.barrier_no + 1) return;
    *u = (UmaUse){cpu, len, g_uma.barrier_no + 1};
    write_pages_stamp(&g_uma.read, cpu, len, g_uma.barrier_no + 1);
}
/* [gva, gva + len) can be read or written in place: one run of guest memory, cpu, with nothing pending over it but
   in-place writes, nothing the runtime wrote while recording and nothing GPU-owned */
static int in_place(u64 gva, u64 len, u64* cpu) {
    u64 h = 0;
    if (!g_vk.uma || !len || gmmu_run(gva, len, cpu) != len) return 0;
    /* results or releases pending: what reads the range takes them as before (gfx_gpu_src and the like) */
    if (!pend_empty(&g_pend_any) && pend_hit(&g_pend_any, gva, len) &&
        (vk_buf_pending(gva, len) || vk_compute_pending(gva, len) || vk_release_pending(gva, len)))
        return 0;
    if (g_gfx_img_any && vk_gfx_image_pending(gva, len)) return 0;
    if (write_pages_since(&g_uma.front, 0, *cpu, len)) return 0;
    /* GPU-owned bytes are in the pool, never imported: an import over them can only be one made before they were */
    if (__atomic_load_n(&g_own_any, __ATOMIC_ACQUIRE) && own_hash(*cpu, len, &h)) return 0;
    return 1;
}
int vk_uma_read(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off) {
    u64 cpu = 0;
    if (!in_place(gva, len, &cpu) || !vk_uma_get(cpu, len, align, buf, off)) return 0;
    order(cpu, len, 0);
    note_read(cpu, len);
    uma_use(cpu, len);
    return 1;
}
int vk_uma_write(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off) {
    u64 cpu = 0;
    for (u32 i = 0; i < g_uma.nprep; i++) /* bound twice in one draw: the same binding */
        if (g_uma.prep[i].gva == gva && g_uma.prep[i].len == len)
            return vk_uma_get(g_uma.prep[i].cpu, len, align, buf, off);
    if (g_uma.nprep == UMA_PREP_N || !in_place(gva, len, &cpu) || !vk_uma_get(cpu, len, align, buf, off)) return 0;
    order(cpu, len, 1);
    uma_use(cpu, len);
    g_uma.prep[g_uma.nprep++] = (UmaOut){gva, cpu, len, 0};
    return 1;
}
void vk_uma_writes_commit(void) {
    u64 now = vk_submit_number();
    for (u32 i = 0; i < g_uma.nprep; i++) {
        UmaOut o = g_uma.prep[i];
        if (g_uma.nout == g_uma.capout) {
            u32 cap = g_uma.capout ? g_uma.capout * 2 : 256;
            UmaOut* n = realloc(g_uma.out, (size_t)cap * sizeof *n);
            if (!n) { /* nowhere to note it: the GPU finishes it now, and then nothing is pending */
                vk_finish();
                continue;
            }
            g_uma.out = n;
            g_uma.capout = cap;
        }
        o.submit = now;
        g_uma.out[g_uma.nout++] = o;
        write_pages_stamp(&g_uma.written, o.cpu, o.len, g_uma.barrier_no + 1);
        pend_add_pages(&g_uma.out_pages, o.cpu >> 12, (o.cpu + o.len - 1) >> 12, 1);
    }
    g_uma.nprep = 0;
    g_uma_any = g_uma.nout != 0;
}
void vk_uma_writes_abort(void) { g_uma.nprep = 0; }
void vk_uma_frame_done(u64 frame) {
    u32 k = 0;
    while (k < g_uma.nout && g_uma.out[k].submit <= frame) {
        const UmaOut* o = &g_uma.out[k++];
        pend_add_pages(&g_uma.out_pages, o->cpu >> 12, (o->cpu + o->len - 1) >> 12, -1);
        gpu_mem_written_cpu(o->cpu, o->len); /* what was built from the bytes (buffers, textures) takes the new ones */
    }
    if (!k) return;
    memmove(g_uma.out, g_uma.out + k, (g_uma.nout - k) * sizeof *g_uma.out);
    g_uma.nout -= k;
    g_uma_any = g_uma.nout != 0;
}
void vk_uma_host_barrier(VkCommandBuffer cb, u64 frame) {
    if (!g_uma.nout || g_uma.out[g_uma.nout - 1].submit != frame) return;
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                          .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &d);
}
int vk_uma_pending(u64 gva, u64 len) { return g_uma_any && len && pend_hit(&g_uma.out_pages, gva, len); }
int vk_uma_pending_cpu(u64 cpu, u64 len) {
    return g_uma_any && len && pend_hit_pages(&g_uma.out_pages, cpu >> 12, (cpu + len - 1) >> 12);
}
/* the newest submit with an in-place write over host offsets [cpu, cpu + n), 0 for none */
static u64 newest_out(u64 cpu, u64 n) {
    for (u32 k = g_uma.nout; k-- > 0;)
        if (g_uma.out[k].cpu < cpu + n && cpu < g_uma.out[k].cpu + g_uma.out[k].len) return g_uma.out[k].submit;
    return 0;
}
/* until the GPU has done that submit: the one being recorded is submitted first */
static void wait_out(u64 submit) {
    if (!submit) return;
    g_uma.out_waits++;
    if (submit > vk_last_submit())
        vk_finish();
    else {
        vk_wait_submit(submit);
        vk_land_through(submit);
    }
}
void vk_uma_mem_need(u64 gva, u64 len) {
    u64 newest = 0;
    if (!g_uma_any || t_gpu_reads || !len) return;
    vk_lock();
    if (pend_hit(&g_uma.out_pages, gva, len))
        for (u64 at = 0; at < len;) {
            u64 cpu = 0, n = gmmu_run(gva + at, len - at, &cpu), s;
            if (!n) {
                n = 0x1000 - ((gva + at) & 0xFFF);
                if (n > len - at) n = len - at;
            } else if ((s = newest_out(cpu, n)) > newest)
                newest = s;
            at += n;
        }
    wait_out(newest);
    vk_unlock();
}
void vk_uma_mem_need_cpu(u64 cpu, u64 len) {
    if (!vk_uma_pending_cpu(cpu, len)) return;
    vk_lock();
    wait_out(newest_out(cpu, len));
    vk_unlock();
}
void vk_uma_gpu_reads(int on) { t_gpu_reads = on; }

/* host offsets [cpu, cpu + n) are written next, other than by the guest: never read in place again, and the submits
   that read them in place done first */
static void front_write(u64 cpu, u64 n) {
    u64 submitted, done;
    write_pages_stamp(&g_uma.front, cpu, n, 1);
    /* shaders' writes there, pending: the GPU's bytes first, then the runtime's over them */
    if (vk_uma_pending_cpu(cpu, n)) wait_out(newest_out(cpu, n));
    vk_submit_progress(&submitted, &done);
    if (!write_pages_since(&g_uma.live, done, cpu, n)) return;
    g_uma.waits++;
    if (write_pages_since(&g_uma.live, vk_last_submit(), cpu, n))
        vk_finish(); /* the submit being recorded reads them */
    else
        vk_wait_submit(vk_last_submit());
}
void vk_uma_before_write(u64 gva, u64 len) {
    if (!g_vk.uma) return;
    while (len) {
        u64 cpu = 0, n = gmmu_run(gva, len, &cpu);
        if (!n) { /* an unmapped page: the write doesn't reach memory there */
            n = 0x1000 - (gva & 0xFFF);
            if (n > len) n = len;
        } else
            front_write(cpu, n);
        gva += n;
        len -= n;
    }
}
void vk_uma_owning(u64 cpu, u64 len) {
    int recorded = 0;
    if (!g_vk.uma || !len) return;
    take_changes();
    front_write(cpu, len);
    /* its pages are trapped next, and a driver that takes an import's pages again at each submit can't take trapped
       ones: the imports over them are let go, and the commands recorded with them submitted first */
    for (u32 i = 0; i < g_uma.n; i++)
        if (g_uma.imp[i].lo < cpu + len && cpu < g_uma.imp[i].hi && g_uma.imp[i].used == vk_submit_number())
            recorded = 1;
    if (recorded) vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    retire_range(cpu, len);
}
void vk_uma_mapping_begin(void) {
    /* the same for pages the guest unmaps or moves: a driver taking an import's pages again at its next submit would
       take the new ones, or none */
    if (g_vk.uma && g_uma.n && g_uma.recorded == vk_submit_number()) vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
}

void vk_uma_frame(void) {
    if (!g_vk.uma || g_vk_present % 4) return;
    for (u32 i = g_uma.n; i-- > 0;)
        if ((u32)(g_vk_present - g_uma.imp[i].seen) > UMA_IDLE) retire(i);
}
void vk_uma_stats(VkUmaStats* st) {
    st->imports = g_uma.n;
    st->bytes = g_uma.bytes;
    st->waits = g_uma.waits;
    st->out_waits = g_uma.out_waits;
    st->barriers = g_uma.barriers;
    st->pending = g_uma.nout;
}
