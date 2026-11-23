/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The memory model of the device, and guest memory that the GPU reads in place (vk_memory, docs/layers/gpu/ownership.md).

   The console's CPU and GPU share one memory: a game writes vertices and indices, and the GPU reads those bytes with no
   copy. The copy path (buffer.c) checks each guest range at each guest submit, and copies what changed into a staging
   ring. On a discrete GPU those bytes then cross PCIe. On a GPU that shares system memory (an integrated GPU: a Steam
   Deck, a laptop, a phone), the staging ring is the same memory as guest memory, and the copy is only cost.

   On such a device (or with vk_memory=2) the guest memory under a draw's vertex, index and read-only storage ranges is
   imported into Vulkan (VK_EXT_external_memory_host) and bound where it is. An import covers the 2 MiB windows that a
   range touches, clipped to mapped guest memory, so that an import is never the whole heap: a driver that checks each
   import's pages at each submit (amdgpu's user pointers) pays for the bytes imported, and one import of a whole guest
   heap cost 45 ms a frame there. Imports do not overlap. A range across two imports makes one import of their union.

   Where the driver can't import host memory but takes buffers of a kind of its own in place, guest memory moves into
   them instead with vk_memory=2 (the migrate way: Android, whose drivers all take an AHardwareBuffer, and Qualcomm's
   has no VK_EXT_external_memory_host); auto copies there, as no phone has measured the moves yet (vk_uma_way).
   On Linux, where the driver's imports are dear (amdgpu faults in every import's pages again at each submission),
   auto moves guest memory into memfds the GPU imports as dma-bufs (/dev/udmabuf, the dma-buf way): the import costs a
   submission about a microsecond, and the CPU keeps an ordinary shared mapping of the pages. A region is at most what
   one udmabuf holds (the module's size_limit_mb, 64 MiB on SteamOS), and nothing punches its memfd while the GPU holds
   the pages (they are pinned: the GPU would keep the old ones). In one game's race the recording thread went from 95%
   of a core, 87% of it in the kernel, to 24%.
   uma_memory compares the two ways on a device that has both: 1 only imports, 2 only moves. The
   windows a range touches become a region: a buffer imported once, which the pages then move into with their bytes,
   mapped where they were (mem.c's mem_migrate); the regions it meets move on into it, so one region of their union
   holds them, once the writes in place pending in them have landed (the move copies what their pages hold). A region
   is 256 MiB at most, the biggest AHardwareBuffer Android's
   allocator gave on a Snapdragon 8 Elite (512 MiB aborted its service), and its memory is taken whole when it is made
   and never swapped: one that goes for idleness or room moves its pages back into the reservation's own (mem_unmigrate)
   first. A region's buffer is memory of its own, which no change of the guest's pages touches: one whose pages are
   unmapped, decommitted or trapped needs no wait, and goes with its last reader.

   What stays a copy, and why:
   - Vertices, indices, windows and constant buffers where imports are dear (vk_uma_imports_dear, uma_copies: amdgpu
     takes every import's pages again at each submission), unless a write in place is pending over them.
   - The pages the runtime wrote (front): constant buffers the GPU front end writes between draws (CONST_BUFFER_DATA,
     r3d_const_upload), on the CPU, when the GPU thread meets the method, and the rest of gmem_write's. A draw recorded
     before such a write, but that the GPU runs after it, would read the new bytes in place. The other constant buffers
     are read in place (vk_buf_get_uniform).
   - Ranges that GPU results not landed yet cover (buffer.c's pending results, compute results) and GPU-owned ranges
     (memory/own.c): their bytes are not in guest memory.
   - Textures and render targets: their layout differs (block linear, compressed formats). Their loads read their
     bytes in a region all the same (vk_uma_read_cpu, owned.c's vk_guest_in_place): the
     deswizzle or the copy into the image takes the region's buffer where it took staging.

   The rules that keep it correct:
   - A release that the guest can see (a syncpoint, a semaphore) is performed only after the GPU ran every command that
     read guest memory in place before the stream stopped (release.c, vk_uma_bound). The copy path read those bytes when
     the commands were recorded. In place, the GPU reads them when it runs them, and after a release the guest writes
     the next frame's data.
   - Before guest pages are trapped (a GPU result published over them) or get other host pages (an unmap, a decommit, a
     move into the alias pool), the commands that read an import over them run (quiesce). mem.c then tells which
     pages changed, and the imports on them go (pages_replaced): a driver that pins pages (NVIDIA) holds the old ones,
     and amdgpu looks them up again at each submit and refuses pages in the alias pool, a file's pages.
   - Those imports leave the driver before anything else is submitted, and what was submitted before reaches it first
     (vk_uma_mapping_begin, vk_uma_mapping_end); the imports over pages to be trapped leave it before the trap
     (vk_uma_owning): a driver keeps every import in every submission, and amdgpu takes all their pages again in each
     submission's ioctl, which fails whole (EFAULT, then the device is lost) on pages gone or trapped.
   - A write of the runtime's own (gmem_write: an inline write to memory, a constant buffer update, a target's
     write-back) to a page that a submit not done yet reads in place waits for that submit (live, the submit that last
     read each page in place), as the console's GPU read it before the write. The page is copied from then on (front):
     a constant buffer updated draw after draw waits once, not at every draw. As #690 has it.
   - Any other GPU-side write (a landed result) on a page that a command not finished yet reads in place is counted
     (the hazards line of the report). That command can read the new bytes.
   - Storage and transform feedback buffers are written in place too (vk_uma_write): a draw's or dispatch's writes are
     pending (out, in out_pages, and so in g_pend_any) until its submit is done, with nothing to land. What reads or
     writes those bytes on the GPU after them in place comes after a barrier (order); what copies them on the GPU
     copies them from the import (owned.c's cpu_read); the runtime's own CPU readers wait for the submit
     (vk_uma_mem_need), and so do its writes (front_write). The submit's end makes them visible to the host
     (vk_uma_host_barrier) before a release tells the guest: a release waits for the submit that last bound guest
     memory in place, a write included (vk_uma_bound). As #690 has it.
   - An engine's result (the copy engine's, 2D, VIC, NVDEC, a target's write-back) over memory an import holds already
     (the GPU reads it in place) is copied there on the GPU, in stream order, a write in place like the shaders'
     (vk_uma_publish): not GPU-owned or trapped, and the import stays. Elsewhere it is published GPU-owned as without
     UMA: an import made for it would take its pages from write tracking, and a sparse texture a copy feeds would load
     again whole instead of the pages written (Liz, #1079).
   - Write tracking leaves the pages an import holds alone (vk_uma_import_in): amdgpu takes them again, writable, at
     each submission, which undoes a scan's protection, and the next scan reports them written with no CPU write. What
     trusted that report kept a guest page's old bytes over the pixels a render target drew there. And a submission
     after a scan faulted every protected page in again: 16 ms for 32 MiB imported, against 0.3 ms. Write tracking
     can't tell there (vk_written_since), and its users compare the bytes. A dma-buf region's pages it watches as the
     rest of guest memory (Imp.tracked): the driver pinned them once, when the udmabuf was made, and leaves the CPU's
     page table alone. A region made or gone is no write: the move keeps what the tracking says of each page
     (plat_vm_share), and a region that goes marks none (imp_destroy). Marked written whole at either, a render target
     drawn before, its pixels not yet in memory, read as older than memory: a racing game's colour grading table,
     drawn once as eight targets in a page, loaded as zeros, and its race drew black under the HUD. The GPU's writes in
     place there reach the caches as everywhere, when their submit is done (vk_uma_frame_done's gpu_mem_written_cpu),
     not through a scan. */
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "common/utilities/bits.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/maxwell/write_pages.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/descriptor_heaps.h"
#include "nx/gpu/vk/pending_pages.h"
#include "nx/gpu/vk/uma_pages.h"
#include "nx/gpu/renderer.h"
#include "common/gpu/vk/vk_common.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/memory/own.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#if defined(__ANDROID__)
#include <vulkan/vulkan_android.h>
#endif

#define UMA_WINDOW (2ull << 20) /* imports cover whole windows of this size where guest memory is mapped */
#define UMA_MAX    512          /* imports at most: the least recently used goes past it */
#define UMA_IDLE   600          /* presents unused before an import goes */
#define UMA_REGION_MAX (256ull << 20) /* the migrate way's biggest region */
#define UMA_PREP_N 64           /* in-place writes of one draw or dispatch */

typedef struct {
    u64 cpu, size; /* guest memory [cpu, cpu + size) */
    VkDeviceMemory mem;
    VkBuffer buf;
    u32 used; /* g_vk_present when last bound */
    u64 sub;  /* the submit its last lookup was in (vk_submit_number): last_read gives the one to wait for */
    int dead; /* its pages are other pages now (mem.c): it goes before the next submission */
    /* a region of the migrate way: the buffer its pages were moved into (plat_gpu_buffer), and what the driver imported
       of it; fd -1 for an import of guest memory where it is */
    int fd;
    void* handle;
    int tracked; /* a dma-buf region (imp_make_dmabuf): the driver leaves the CPU's page table alone, and scans watch it */
    u32 rid;     /* its table of pages (Rgn) while it is in the table, 0 for none */
} Imp;
/* Spans the driver refused to import (pages it can't pin, or amdgpu's refusal of the alias pool's), whose ranges are copied
   until UMA_IDLE presents pass or their pages change. Without them every range in such a window asked the driver
   again, twice: one game fell from 28 to 5 frames a second */
#define FAIL_N 64
typedef struct {
    u64 lo, hi; /* hi 0: none */
    u32 at;     /* g_vk_present when it failed */
} Fail;
typedef struct {
    u64 cpu, len, sub;
} Seen;
#define UMA_SOON_N 512
typedef struct {
    u64 gva, cpu, len, sub; /* the GPU writes guest memory [cpu, cpu + len) in place in submit sub */
    u64 prog;               /* the compute program whose dispatch writes it (vk_uma_writer), 0 for a draw's */
} Out;
/* a window of the heap or physical memory that is GPU memory mapped where it is (placed maps, below) */
typedef struct {
    u64 cpu; /* guest memory [cpu, cpu + MEM_PLACED) */
    VkDeviceMemory mem;
    VkBuffer buf;
    u64 sub;                     /* the submit its last binding was in (last_read's rule) */
    u64 pages[MEM_PLACED >> 18]; /* its committed 4 KiB pages, a bit each */
    int fd;                      /* its dma-buf once a view maps it, -1 before */
    u32 rid;                     /* its table of pages (Rgn) while it is in the table, 0 for none */
} Chunk;
typedef struct {
    u64 dst, src, size; /* guest memory [dst, dst + size) maps the pages of a chunk's [src, src + size) (a view) */
} View;
/* The page model: each region (an Imp, or a Chunk of placed maps) has a table of its pages, in a slot of
   g_uma.rg, and g_uma.win names the slot of each 2 MiB window of guest memory, as regions are whole windows. A page's
   table says what a read in place needs to know of it, with no other lookup:
   - blk: some other place holds newer bytes of the page (a pending graphics or compute result, a storage image not
     published, a queued release, the mirror's writes: the pages g_pend_blk counts, pend_set_any_watch), or the runtime
     wrote it (front). Such a page takes the old chain of buffer.c;
   - its stamps: the barrier sequence of its last write and read in place (UMA_WSEQ, UMA_RSEQ: barrier_no + 1 while no
     barrier came since, order), and the submission that last read it in place (UMA_LIVE, front_write and the hazard
     count). Each group of 64 pages and the whole region keep the newest of each, so a question about a region no page
     of which is newer than asked is one compare.
   Its generations move with g_uma.clock: gen when it is made or goes, bgen when a page gains a blk bit. A region that
   goes keeps its slot (in drg) until no stamp of it can still matter (rgn_reap): the questions look at those too.
   The writes in place still pending are counted by page in out_pages and so in g_pend_any, as before, which a lookup
   by GPU address asks with no translation. They count in no adds but out_pages' own (adds_alone): they
   stay out of g_pend_any.adds and vk_buf_adds, which made every later draw's buffer cache entries check every kind of
   result again at each storage write. The caches that kept bytes add their count (vk_uma_writes). A question that the
   GMMU translated at each lookup (the regions' tokens) made a quarter of one game's GPU thread gmmu_run. A page in no
   region (a view of a chunk) is stamped in the tables of before (written, read, live) */
enum { UMA_WSEQ, UMA_RSEQ, UMA_LIVE, UMA_ST_N };
typedef struct {
    u64 cpu, size;        /* guest memory [cpu, cpu + size), whole windows; size 0: a free slot */
    VkBuffer buf;         /* its Imp's or Chunk's */
    u64 gen, bgen;        /* from g_uma.clock (above) */
    _Atomic u64 newest[UMA_ST_N]; /* the newest stamp of each kind on any page */
    _Atomic u64* t;       /* front bits, pend bits (a word each 64 pages), the stamps by page, the newest by group */
    u32 nblk;             /* words with a front or pend bit */
    u32 at;               /* its Imp's or Chunk's index when last found (rgn_owner checks it) */
    u32 next;             /* a free slot's: the next free one */
    int chunk;            /* a Chunk's, not an Imp's */
    int live;             /* in g_uma.win; 0 once it went (in drg) */
} Rgn;
/* the fast path's memo (vk_uma_read), by GPU range: what it resolved to while the GMMU's mappings and the region hold */
#define UMA_MEMO_N 4096
typedef struct {
    u64 gva, len, cpu, gen, bgen;
    u32 map_gen, rid;
} UmaMemo;
/* draw_state_check=1: the tables of before kept beside the page tables, and the answers compared */
typedef struct {
    WritePages written, read, live;
} UmaCheck;
#define UMA_WINS (AS_TOP >> 21) /* the 2 MiB windows of guest addresses g_uma.win covers */
enum { UMA_FAST_DEAD, UMA_FAST_RUN, UMA_FAST_NONE, UMA_FAST_EDGE, UMA_FAST_BLK, UMA_FAST_ALIGN, UMA_FAST_N };
#define UMA_LINE 64 /* a cache line (x86-64, AArch64) */
/* The file's state. Different threads write its groups, each apart from the next by a cache line: the GPU thread
   (under vk_lock), the threads that change guest mappings (mem.c's hook, under lk), any thread that writes guest memory
   for the GPU (the hazard count), and the GPU thread again for the page table that those threads read */
typedef struct {
    /* the GPU thread's, under vk_lock */
    Imp imp[UMA_MAX]; /* sorted by cpu, none overlapping, nimp of them; their dead flags under lk */
    u32 nimp;
    Imp* gone; /* dropped from the table, the driver's until the submit that last read them is done: ngone of cap; their
                  entries and count under lk too */
    u32 ngone, capgone;
    u64 seg; /* the submit being recorded when the draw or dispatch being set up began (vk_uma_settle), or once all that
                was recorded is done (read_done): a lookup since may be used in any submit up to the one being
                recorded */
    VkResult import_result; /* the last import's, for the log */
    u64 align;              /* minImportedHostPointerAlignment, and the host's page in the migrate way */
    int migrate;            /* the migrate way: guest memory moves into regions (vk_uma_choose, vk_uma_migrate) */
    int dmabuf;             /* and a region is a memfd the driver imports as a dma-buf (udmabuf), not a host pointer */
    u64 region_max;         /* a region's most bytes: UMA_REGION_MAX, or what one udmabuf holds */
    int can_dmabuf;         /* regions can be dma-bufs: udmabuf opens, and the device imports them (vk_uma_init) */
    u64 dmabuf_max;         /* what one udmabuf holds, whole windows */
    char why[96];           /* why the last region couldn't be made, for the log */
    u64 bound;              /* the submit that last read guest memory in place (vk_submit_number) */
    Seen seen[256];         /* the ranges hz_mark marked, with their submit */
    WritePages live;        /* the submit that last read each page in place */
    WritePages front;       /* the pages the runtime wrote (gmem_write): never read in place again */
    u64 lag_lo, lag_hi;     /* live stamps from lag_lo on may have been read up to submit lag_hi (vk_uma_settle) */
    /* in-place writes: the draw's or dispatch's being set up (prep), and those recorded, oldest first, until their
       submit is done (out, by guest page in out_pages) */
    Out prep[UMA_PREP_N];
    u32 nprep, nout, capout;
    Out* out;
    PendPages out_pages;
    WritePages written, read; /* the pages written and read in place since the last barrier: barrier_no + 1 */
    Seen rmemo[256];          /* the ranges note_read stamped since that barrier */
    u64 barrier_no;
    /* placed maps: the chunks, sorted by cpu, and those given back until their last binding's submit is done; each
       table changed under lk too, as vk_uma_import_in reads them on any thread */
    Chunk* chunk;
    u32 nchunk, capchunk;
    Chunk* cgone;
    u32 ncgone, capcgone;
    View* view; /* sorted by dst */
    u32 nview, capview;
    int can_view;   /* and exports a chunk as a dma-buf, which a view maps */
    int can_place;  /* the device maps memory where it is told, and gives the address back reserved */
    int placed;     /* on: the windows committed whole are chunks (vk_uma_placed) */
    u32 place_type; /* the memory type of the chunks: host-visible and coherent, cached where it can be */
    u64 chunks_made, chunks_gone;
    /* statistics */
    u64 hits, hit_bytes, misses, imports, import_bytes, import_ns, failed, dropped, quiesces, waits;
    u64 written_bytes, barriers, out_waits, published;
    /* the writes in place (out_add), those of a range that a write of the same submission had (a direct-mapped
       table of the last ones, by range: a range it lost to another counts as new), and the pages they counted */
    u64 out_n, out_same, out_npages;
    Seen out_seen[4096];
    _Atomic u64 wait_by[4]; /* out_waits by the caller of wait_out_why: region, mem-need, mem-need-cpu, front-write */
    u64 imp_why[6]; /* why imp_get made no import: chunked, unmade over imports, unmapped, over the cap, refused
                       before, table full */
    u64 open_waits; /* of out_waits, those for writes still in the submission being recorded */
    u64 wait_ns[4], wait_open[4]; /* wait_by's time, and those of them that finished the submission being recorded */
    /* order's barriers by what asked for them (UMA_ORDER_*), and those of them that ended a rendering pass */
    u64 order_n[3], order_pass[3];
    u64 refused[VK_UMA_REFUSED_N]; /* the writes in place refused, by why (vk_uma_refused) */
    u64 merges, merged;            /* regions made over others (imp_get), and the others they took over */
    /* an aliasing draw's storage bound in place (vk_uma_cover, buffer.c's alias_in_place): imp_get makes no wait
       while no_wait is on, and counts in would_wait the imports it did not make or let go for that. regions moves at
       each import or chunk made or gone, never set back: the group's buffers are one region's while it holds */
    int no_wait;
    u64 would_wait, regions;
    /* the programs whose dispatches go to the GPU at once (vk_uma_read_soon): open addressing, 0 for a free slot */
    u64 soon[UMA_SOON_N];
    u32 nsoon;
    u64 writer; /* the program whose writes vk_uma_writes_commit takes (vk_uma_writer) */
    /* the page model (above Rgn): the slots, rg[0] none, nrg used of caprg and rg_free the first free one;
       the slots of regions that went, still asked (drg); the windows' slots (win). The slots, their tables and win
       change under lk too, as other threads read them (the hazard count, vkr_mem_pending) */
    Rgn* rg;
    u32 nrg, caprg, rg_free;
    u32* drg;
    _Atomic u32 ndrg;
    u32 capdrg;
    u64 clock;    /* the generations' source */
    _Atomic int old_any[UMA_ST_N]; /* the old tables hold a stamp of that kind (a page in no region) */
    UmaMemo memo[UMA_MEMO_N];
    UmaCheck* chk; /* draw_state_check=1 */
    /* statistics: the fast path's hits and misses by why (UMA_FAST_*), and the cross-checks' differences */
    u64 fast_hits, fast_miss[UMA_FAST_N], check_diffs;
    _Atomic u32 win[UMA_WINS];
    char apart0[UMA_LINE];
    /* mem.c's hook writes these too */
    PMutex lk;         /* a leaf, under vk_lock or mem.c's lock: the dead flags, and the imports for write tracking */
    _Atomic int any_dead; /* under lk */
    Fail fail[FAIL_N]; /* under lk */
    char apart1[UMA_LINE];
    _Atomic u64 hazards; /* any thread's, atomic */
    _Atomic u64 hz_done; /* a submit known done when the hazard count last asked (gpu_wrote), any thread's, atomic */
    char apart2[UMA_LINE];
} Uma;
static_assert(offsetof(Uma, lk) - offsetof(Uma, apart0) >= UMA_LINE &&
                  offsetof(Uma, hazards) - offsetof(Uma, apart1) >= UMA_LINE,
              "two groups of Uma share a cache line");
static Uma g_uma = {.lk = PMUTEX_INIT, .align = 4096, .out_pages = {.also = &g_pend_any}};
/* this thread's: vk_uma_gpu_reads, the caller copies the in-place writes pending on the GPU */
static _Thread_local struct {
    int gpu_reads;
} t_uma;

int vk_uma(void) { return g_vk.mem_path == VK_MEM_PATH_UMA; }
/* amdgpu looks up the pages of every user-pointer buffer in a submission's list again at each submission (the CS
   ioctl), and RADV puts every buffer the device holds in that list: each submission costs the kernel time for all the
   pages imported. One game's battle map held about 90 imports, 270 MB, and made about 380 submissions a present, one
   per compute dispatch: about 0.2 ms each on the recording thread, 70% of a core, and the GPU idled through most of
   each frame for them (8.5 frames a second on a Radeon 890M). Its vertex and index ranges held about 120 MB of those
   imports and its constant buffers about 40 MB; copied as on a discrete GPU, 90 MB stayed and the map ran at 14.9.
   What stays imported are the pages written in place and the dispatches' storage ranges, 16 MiB windows that are far
   dearer copied (5 frames a second). The drivers that take every import's pages again at each submission are the
   extension bans' off lines on VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT (g_vk.imports_dear). uma_copies
   overrides it */
static int imports_dear(void) {
    if (g_settings.guest_buffers_copied) return g_settings.guest_buffers_copied == 2; /* uma_copies */
    return g_vk.imports_dear;
}
/* a discrete GPU reads guest memory in place only through dma-bufs, whose import costs a submission no lookup of its
   pages: its vertices, indices and constant buffers are read in place too, unless uma_copies copies them */
int vk_uma_imports_dear(void) {
    if (!g_settings.guest_buffers_copied && g_uma.dmabuf && g_vk.mem_model != VK_MEM_UMA) return 0;
    return imports_dear();
}
u64 vk_uma_bound(void) { return g_uma.bound; }

/* ------------------------------------------------------------ the page model (above Rgn) */
/* a slot's table: the front and pend bits, a word for each 64 pages; then for each kind of stamp the pages', the
   newest of each group of 64, and the stamp of each group written whole (a 16 MiB storage window is 64 stores, not
   4096) */
static u64 rg_words(const Rgn* r) { return r->size >> 18; }
static _Atomic u64* rg_front(const Rgn* r) { return r->t; }
static _Atomic u64* rg_pend(const Rgn* r) { return r->t + rg_words(r); }
static _Atomic u64* rg_st(const Rgn* r, int k) { return r->t + 2 * rg_words(r) + (u64)k * (r->size >> 12); }
static _Atomic u64* rg_grp(const Rgn* r, int k) {
    return r->t + 2 * rg_words(r) + UMA_ST_N * (r->size >> 12) + (u64)k * rg_words(r);
}
static _Atomic u64* rg_whole(const Rgn* r, int k) {
    return r->t + 2 * rg_words(r) + UMA_ST_N * (r->size >> 12) + (UMA_ST_N + (u64)k) * rg_words(r);
}
static u64 rg_table_words(u64 size) { return (2 + 2 * UMA_ST_N) * (size >> 18) + UMA_ST_N * (size >> 12); }
/* the slot of the live region over guest address cpu, 0 for none. Atomic: the hazard count reads it on any thread */
static u32 rid_at(u64 cpu) {
    u64 w = cpu >> 21;
    return w < UMA_WINS ? atomic_load_explicit(&g_uma.win[w], memory_order_relaxed) : 0;
}
/* r's table holds [cpu, cpu + len), r is whole windows and has its table: else a fatal error that names them, not a
   store past the table (who: the caller) */
static void rg_holds(const Rgn* r, u64 cpu, u64 len, const char* who) {
    if (r->t && r->size && !((r->cpu | r->size) & (UMA_WINDOW - 1)) && cpu >= r->cpu && len &&
        cpu + len <= r->cpu + r->size)
        return;
    fatal("vulkan: page model: %s of guest %llx+%llx outside the table of region %llx+%llx (live %d, table %s)", who,
          (unsigned long long)cpu, (unsigned long long)len, (unsigned long long)r->cpu, (unsigned long long)r->size,
          r->live, r->t ? "there" : "freed");
}
/* r's pages of [cpu, cpu + len) (within it) stamped v of kind k. Each stamp is the newest of its kind (a submission or
   the barrier sequence, which only grow), so the groups' and the region's take it as it is. Atomic: other threads read
   them (WSUB under lk, LIVE for the hazard count) */
static void rg_stamp(Rgn* r, int k, u64 cpu, u64 len, u64 v) {
    rg_holds(r, cpu, len, "a stamp");
    u64 p0 = (cpu - r->cpu) >> 12, p1 = (cpu + len - 1 - r->cpu) >> 12;
    _Atomic u64 *s = rg_st(r, k), *g = rg_grp(r, k), *w = rg_whole(r, k);
    for (u64 p = p0; p <= p1;) {
        u64 gi = p >> 6, ge = (gi << 6) + 63;
        if (!(p & 63) && ge <= p1) { /* the whole group */
            atomic_store_explicit(&w[gi], v, memory_order_relaxed);
            p = ge + 1;
        } else
            for (u64 e = ge < p1 ? ge : p1; p <= e; p++) atomic_store_explicit(&s[p], v, memory_order_relaxed);
        if (atomic_load_explicit(&g[gi], memory_order_relaxed) < v)
            atomic_store_explicit(&g[gi], v, memory_order_relaxed);
    }
    if (atomic_load_explicit(&r->newest[k], memory_order_relaxed) < v)
        atomic_store_explicit(&r->newest[k], v, memory_order_relaxed);
}
/* whether a page of r in [cpu, cpu + len) has a stamp of kind k after x: one compare where none of r's is */
static int rg_since(const Rgn* r, int k, u64 cpu, u64 len, u64 x) {
    u64 end = cpu + len, rend = r->cpu + r->size, lo = cpu > r->cpu ? cpu : r->cpu, hi = end < rend ? end : rend;
    if (lo >= hi || atomic_load_explicit(&r->newest[k], memory_order_relaxed) <= x) return 0;
    rg_holds(r, lo, hi - lo, "a question");
    const _Atomic u64 *s = rg_st(r, k), *g = rg_grp(r, k), *w = rg_whole(r, k);
    u64 p0 = (lo - r->cpu) >> 12, p1 = (hi - 1 - r->cpu) >> 12;
    for (u64 gi = p0 >> 6; gi <= p1 >> 6; gi++) {
        if (atomic_load_explicit(&g[gi], memory_order_relaxed) <= x) continue;
        if (atomic_load_explicit(&w[gi], memory_order_relaxed) > x) return 1;
        u64 a = gi << 6 > p0 ? gi << 6 : p0, b = (gi << 6) + 63 < p1 ? (gi << 6) + 63 : p1;
        for (u64 p = a; p <= b; p++)
            if (atomic_load_explicit(&s[p], memory_order_relaxed) > x) return 1;
    }
    return 0;
}
/* whether a page of r in [cpu, cpu + len) (within it) has a blk bit */
static int rg_blk(const Rgn* r, u64 cpu, u64 len) {
    rg_holds(r, cpu, len, "a blk question");
    if (!r->nblk) return 0;
    const _Atomic u64 *f = rg_front(r), *pd = rg_pend(r);
    u64 p0 = (cpu - r->cpu) >> 12, p1 = (cpu + len - 1 - r->cpu) >> 12;
    for (u64 wi = p0 >> 6; wi <= p1 >> 6; wi++)
        if ((atomic_load_explicit(&f[wi], memory_order_relaxed) | atomic_load_explicit(&pd[wi], memory_order_relaxed)) &
            vk_uma_word_mask(wi, p0, p1))
            return 1;
    return 0;
}
/* word wi of r's front or pend bits becomes now (bits, 1 for front): nblk follows, and bgen moves for a bit gained */
static void rg_bits(Rgn* r, u64 wi, int front, u64 now) {
    _Atomic u64 *f = rg_front(r), *pd = rg_pend(r);
    u64 was = front ? atomic_load_explicit(&f[wi], memory_order_relaxed)
                    : atomic_load_explicit(&pd[wi], memory_order_relaxed),
        other = front ? atomic_load_explicit(&pd[wi], memory_order_relaxed)
                      : atomic_load_explicit(&f[wi], memory_order_relaxed);
    if (now == was) return;
    if (!(was | other) != !(now | other)) r->nblk = now | other ? r->nblk + 1 : r->nblk - 1;
    if (front)
        atomic_store_explicit(&f[wi], now, memory_order_relaxed);
    else
        atomic_store_explicit(&pd[wi], now, memory_order_relaxed);
    if (now & ~was) r->bgen = ++g_uma.clock;
}
/* r's pend bits over [cpu, cpu + len) (within it) as g_pend_blk counts its pages now */
static void rg_pend_sync(Rgn* r, u64 cpu, u64 len) {
    rg_holds(r, cpu, len, "a pend bit");
    for (u64 wi = (cpu - r->cpu) >> 18, e = (cpu + len - 1 - r->cpu) >> 18; wi <= e; wi++)
        rg_bits(r, wi, 0, pend_guest_group(&g_pend_blk, (r->cpu >> 18) + wi));
}
static void rg_front_run(void* ctx, u64 at, u64 n) { /* write_pages_runs's: pages the runtime wrote, as front */
    Rgn* r = ctx;
    rg_holds(r, at, n, "a front bit");
    u64 p0 = (at - r->cpu) >> 12, p1 = (at + n - 1 - r->cpu) >> 12;
    for (u64 p = p0; p <= p1; p++)
        rg_bits(r, p >> 6, 1, atomic_load_explicit(&(rg_front(r)[p >> 6]), memory_order_relaxed) | 1ull << (p & 63));
}
/* pending_pages.c's watch: guest pages first to last gained or lost a result in g_pend_blk (vk_lock and pend_lock
   held) */
static void pend_watch(u64 first, u64 last) {
    u64 lo = first << 12, hi = (last + 1) << 12;
    if (first >= UMA_WINS << 9) return;
    if (last >= UMA_WINS << 9) hi = (u64)UMA_WINS << 21;
    for (u64 at = lo; at < hi;) {
        u64 wend = (at | (UMA_WINDOW - 1)) + 1, n = (wend < hi ? wend : hi) - at;
        u32 rid = rid_at(at);
        if (rid) rg_pend_sync(&g_uma.rg[rid], at, n);
        at += n;
    }
}
/* whether every reader of a LIVE stamp s is done (done the last submission done): one looked up in the draw being
   set up, or one whose use was recorded in a later submission than its own (vk_uma_settle's lag), is not */
static int live_over(u64 s, u64 done) {
    return s <= done && s < g_uma.seg && (s < g_uma.lag_lo || g_uma.lag_hi <= done);
}
/* the slots of regions that went are freed once no stamp of theirs can matter: their writes landed, their readers
   done, and a barrier came after their stamps of order (vk_lock held) */
static void rgn_reap(void) {
    u64 submitted, done;
    if (!atomic_load_explicit(&g_uma.ndrg, memory_order_relaxed)) return;
    vk_submit_progress(&submitted, &done);
    for (u32 i = atomic_load_explicit(&g_uma.ndrg, memory_order_relaxed); i-- > 0;) {
        u32 id = g_uma.drg[i];
        Rgn* r = &g_uma.rg[id];
        if (!live_over(atomic_load_explicit(&r->newest[UMA_LIVE], memory_order_relaxed), done) ||
            atomic_load_explicit(&r->newest[UMA_WSEQ], memory_order_relaxed) > g_uma.barrier_no ||
            atomic_load_explicit(&r->newest[UMA_RSEQ], memory_order_relaxed) > g_uma.barrier_no)
            continue;
        pm_lock(&g_uma.lk);
        free(r->t);
        *r = (Rgn){.next = g_uma.rg_free};
        g_uma.rg_free = id;
        g_uma.drg[i] = g_uma.drg[atomic_load_explicit(&g_uma.ndrg, memory_order_relaxed) - 1];
        atomic_store_explicit(&g_uma.ndrg, atomic_load_explicit(&g_uma.ndrg, memory_order_relaxed) - 1,
                              memory_order_relaxed);
        pm_unlock(&g_uma.lk);
    }
}
/* a table for the region over guest memory [cpu, cpu + size) (whole windows), its buffer buf: its slot. The pages
   g_pend_blk counts and those the runtime wrote are blk from the start (vk_lock held) */
static u32 rgn_new(u64 cpu, u64 size, VkBuffer buf, int chunk) {
    if ((cpu | size) & (UMA_WINDOW - 1) || !size)
        fatal("vulkan: page model: a table for guest %llx+%llx, which is not whole windows", (unsigned long long)cpu,
              (unsigned long long)size);
    _Atomic u64* t = calloc(rg_table_words(size), sizeof *t);
    if (!t)
        fatal("vulkan: out of memory for the page table of %llu MiB of guest memory", (unsigned long long)(size >> 20));
    Rgn r = {.cpu = cpu, .size = size, .buf = buf, .t = t, .chunk = chunk, .live = 1};
    r.gen = r.bgen = ++g_uma.clock;
    rg_pend_sync(&r, cpu, size);
    write_pages_runs(&g_uma.front, 0, cpu, size, rg_front_run, &r);
    rgn_reap();
    pm_lock(&g_uma.lk);
    u32 id = g_uma.rg_free;
    if (id)
        g_uma.rg_free = g_uma.rg[id].next;
    else {
        if (g_uma.nrg + 1 >= g_uma.caprg) {
            u32 cap = g_uma.caprg ? 2 * g_uma.caprg : 64;
            Rgn* n = realloc(g_uma.rg, sizeof *n * cap);
            if (!n) fatal("vulkan: out of memory for %u page tables of guest memory", cap);
            if (!g_uma.caprg) n[0] = (Rgn){0}; /* slot 0 is none */
            g_uma.rg = n;
            g_uma.caprg = cap;
        }
        id = ++g_uma.nrg;
    }
    g_uma.rg[id] = r;
    for (u64 w = cpu >> 21; w < (cpu + size) >> 21 && w < UMA_WINS; w++)
        atomic_store_explicit(&g_uma.win[w], id, memory_order_relaxed);
    pm_unlock(&g_uma.lk);
    return id;
}
/* the region of slot id goes from the windows (merged, let go, killed): its table is still asked until rgn_reap frees
   it (vk_lock held) */
static void rgn_drop(u32 id) {
    if (!id) return;
    Rgn* r = &g_uma.rg[id];
    pm_lock(&g_uma.lk);
    for (u64 w = r->cpu >> 21; w < (r->cpu + r->size) >> 21 && w < UMA_WINS; w++)
        if (atomic_load_explicit(&g_uma.win[w], memory_order_relaxed) == id)
            atomic_store_explicit(&g_uma.win[w], 0, memory_order_relaxed);
    r->live = 0;
    r->gen = ++g_uma.clock;
    if (atomic_load_explicit(&g_uma.ndrg, memory_order_relaxed) == g_uma.capdrg) {
        u32 cap = g_uma.capdrg ? 2 * g_uma.capdrg : 16;
        u32* n = realloc(g_uma.drg, sizeof *n * cap);
        if (!n) fatal("vulkan: out of memory for %u page tables of regions that went", cap);
        g_uma.drg = n;
        g_uma.capdrg = cap;
    }
    g_uma.drg[atomic_load_explicit(&g_uma.ndrg, memory_order_relaxed)] = id;
    atomic_store_explicit(&g_uma.ndrg, atomic_load_explicit(&g_uma.ndrg, memory_order_relaxed) + 1,
                          memory_order_relaxed);
    pm_unlock(&g_uma.lk);
}
/* region d, made over region s, takes the stamps of s's pages: the order, the pending writes and the readers of the
   bytes that moved into it (imp_get's merge) */
static void rgn_take(Rgn* d, const Rgn* s) {
    u64 lo = d->cpu > s->cpu ? d->cpu : s->cpu, de = d->cpu + d->size, se = s->cpu + s->size, hi = de < se ? de : se;
    if (lo >= hi) return;
    u64 dp = (lo - d->cpu) >> 12, sp = (lo - s->cpu) >> 12, np = (hi - lo) >> 12;
    for (int k = 0; k < UMA_ST_N; k++) {
        _Atomic u64 *ds = rg_st(d, k), *dg = rg_grp(d, k), *dw = rg_whole(d, k);
        const _Atomic u64 *ss = rg_st(s, k), *sg = rg_grp(s, k), *sw = rg_whole(s, k);
        for (u64 p = 0; p < np; p++)
            if (atomic_load_explicit(&ss[sp + p], memory_order_relaxed) >
                atomic_load_explicit(&ds[dp + p], memory_order_relaxed))
                atomic_store_explicit(&ds[dp + p], atomic_load_explicit(&ss[sp + p], memory_order_relaxed),
                                      memory_order_relaxed);
        for (u64 g = 0; g < np >> 6; g++) {
            if (atomic_load_explicit(&sg[(sp >> 6) + g], memory_order_relaxed) >
                atomic_load_explicit(&dg[(dp >> 6) + g], memory_order_relaxed))
                atomic_store_explicit(&dg[(dp >> 6) + g],
                                      atomic_load_explicit(&sg[(sp >> 6) + g], memory_order_relaxed),
                                      memory_order_relaxed);
            if (atomic_load_explicit(&sw[(sp >> 6) + g], memory_order_relaxed) >
                atomic_load_explicit(&dw[(dp >> 6) + g], memory_order_relaxed))
                atomic_store_explicit(&dw[(dp >> 6) + g],
                                      atomic_load_explicit(&sw[(sp >> 6) + g], memory_order_relaxed),
                                      memory_order_relaxed);
        }
        if (atomic_load_explicit(&s->newest[k], memory_order_relaxed) >
            atomic_load_explicit(&d->newest[k], memory_order_relaxed))
            atomic_store_explicit(&d->newest[k], atomic_load_explicit(&s->newest[k], memory_order_relaxed),
                                  memory_order_relaxed);
    }
}
/* the tables of before (the pages in no region): whether a page of [cpu, cpu + len) has a
   stamp of kind k after x, and the stamp */
static int old_since(const WritePages* written, const WritePages* read, const WritePages* live, int k, u64 cpu, u64 len,
                     u64 x) {
    switch (k) {
        case UMA_WSEQ: return write_pages_since(written, x, cpu, len) != 0;
        case UMA_RSEQ: return write_pages_since(read, x, cpu, len) != 0;
        default: return write_pages_since(live, x, cpu, len) != 0;
    }
}
static void hz_old(u64 cpu, u64 len);
static void old_stamp(int k, u64 cpu, u64 len, u64 v) {
    if (k == UMA_WSEQ) write_pages_stamp(&g_uma.written, cpu, len, v);
    if (k == UMA_RSEQ) write_pages_stamp(&g_uma.read, cpu, len, v);
    if (k == UMA_LIVE) hz_old(cpu, len);
}
/* the pieces of guest memory [cpu, cpu + len) by window: fn(ctx, rid, at, n) for each run of one region (rid its slot)
   or of none (0) */
static int st_pieces(u64 cpu, u64 len, int (*fn)(void* ctx, u32 rid, u64 at, u64 n), void* ctx) {
    for (u64 at = cpu, end = cpu + len; at < end;) {
        u32 rid = rid_at(at);
        u64 stop = rid ? g_uma.rg[rid].cpu + g_uma.rg[rid].size : (at | (UMA_WINDOW - 1)) + 1;
        u64 n = (stop < end ? stop : end) - at;
        if (fn(ctx, rid, at, n)) return 1;
        at += n;
    }
    return 0;
}
typedef struct {
    int k;
    u64 x;
} StAsk;
static int since_piece(void* ctx, u32 rid, u64 at, u64 n) {
    const StAsk* q = ctx;
    if (rid) rg_holds(&g_uma.rg[rid], at, n, "a question by window");
    return rid ? rg_since(&g_uma.rg[rid], q->k, at, n, q->x) : 0;
}
/* whether a page of guest memory [cpu, cpu + len) has a stamp of kind k after x: in the live regions, in the regions
   that went and keep theirs still, and in the tables of before for a page that was in no region (vk_lock held, or lk
   on another thread) */
static int st_since(int k, u64 cpu, u64 len, u64 x) {
    StAsk q = {k, x};
    if (!len) return 0;
    if (st_pieces(cpu, len, since_piece, &q)) return 1;
    for (u32 i = 0; i < atomic_load_explicit(&g_uma.ndrg, memory_order_relaxed); i++)
        if (rg_since(&g_uma.rg[g_uma.drg[i]], k, cpu, len, x)) return 1;
    return atomic_load_explicit(&g_uma.old_any[k], memory_order_relaxed) &&
           old_since(&g_uma.written, &g_uma.read, &g_uma.live, k, cpu, len, x);
}
typedef struct {
    int k;
    u64 v;
} StPut;
static int stamp_piece(void* ctx, u32 rid, u64 at, u64 n) {
    const StPut* q = ctx;
    if (rid)
        rg_stamp(&g_uma.rg[rid], q->k, at, n, q->v);
    else {
        atomic_store_explicit(&g_uma.old_any[q->k], 1, memory_order_relaxed);
        old_stamp(q->k, at, n, q->v);
    }
    return 0;
}
/* guest memory [cpu, cpu + len) stamped v of kind k (not WSUB: out_add) */
static void st_stamp(int k, u64 cpu, u64 len, u64 v) {
    StPut q = {k, v};
    if (len) st_pieces(cpu, len, stamp_piece, &q);
}
/* draw_state_check=1: an answer of the page tables that the tables of before don't give */
static void check_differs(const char* what, u64 cpu, u64 len, int now, int before) {
    g_uma.check_diffs++;
    WARN_LIMIT(16, "vulkan: page model: %s of guest %llx+%llx is %d, the tables of before say %d\n", what,
               (unsigned long long)cpu, (unsigned long long)len, now, before);
}
static int chk_since(int k, u64 cpu, u64 len, u64 x) {
    const UmaCheck* c = g_uma.chk;
    return old_since(&c->written, &c->read, &c->live, k, cpu, len, x);
}

/* ------------------------------------------------------------ the model */
static const char* model_name(int m) {
    return m == VK_MEM_UMA     ? "shares system memory (UMA)"
           : m == VK_MEM_REBAR ? "discrete, the CPU maps all of its memory (resizable BAR)"
                               : "discrete, the CPU maps a small window of its memory";
}
/* Which memory the device has (vk_common.h's snx_vk_memory_model, which the 3DS's device asks too) */
static int mem_model(void) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
    SnxVkMemoryModel model = snx_vk_memory_model(g_vk.props.deviceType, &mp);
    return model == SNX_VK_UMA ? VK_MEM_UMA : model == SNX_VK_REBAR ? VK_MEM_REBAR : VK_MEM_SMALL_BAR;
}
/* before the device is made: the model, and the path that vk_memory (0 auto, 1 copy, 2 UMA) and the device's
   extensions give: guest memory imported where it is, else (Android) moved into AHardwareBuffers. The extension to
   enable, VK_UMA_WITH_* */
#if defined(__ANDROID__)
#define UMA_NO_IMPORT "neither VK_EXT_external_memory_host nor VK_ANDROID_external_memory_android_hardware_buffer"
#else
#define UMA_NO_IMPORT "no VK_EXT_external_memory_host"
#endif
/* The moves into AHardwareBuffers are taken only when asked for (want 2, or how 2): auto keeps copying where the device
   has no VK_EXT_external_memory_host, as no phone has measured them yet. how is uma_memory, which compares the two ways
   on one device: 1 never moves, 2 never imports guest memory where it is */
/* Linux's dma-bufs of memfds (udmabuf, has_dmabuf) are moved into under auto where the driver's imports are dear
   (dear): a dma-buf's import costs a submission about a microsecond, where amdgpu faults in a host pointer's pages
   again at each. On a Steam Deck a submission took 20 us with 192 MB in three dma-bufs, 346 us with it in 48 host
   pointers, and one game's race held about 180 MB of imports, its recording thread 87% in the kernel */
int vk_uma_way(int want, int how, int model, int has_import, int has_ahb, int has_dmabuf, int dear) {
    /* auto reads in place on a device that shares memory, and on a discrete one through dma-bufs where host imports are
       dear: there an import of a host pointer cost a game's race about 258 ms, and froze its boot for 45 s */
    const int auto_in_place = model == VK_MEM_UMA || (has_dmabuf && dear && how != 1);
    if (want != 2 && (want != 0 || !auto_in_place)) return VK_UMA_WITH_NONE;
    if (how == 2) return has_ahb ? VK_UMA_WITH_AHB : has_dmabuf ? VK_UMA_WITH_DMABUF : VK_UMA_WITH_NONE;
    if (how == 0 && has_dmabuf && dear) return VK_UMA_WITH_DMABUF;
    if (has_import) return VK_UMA_WITH_HOST;
    return how == 0 && want == 2 && has_ahb ? VK_UMA_WITH_AHB : VK_UMA_WITH_NONE;
}
/* whether a buffer of guest_usage() can be a dma-buf the device imports, not one it takes only whole (dedicated) */
static VkBufferUsageFlags guest_usage(void);
static int dmabuf_importable(void) {
    VkPhysicalDeviceExternalBufferInfo bi = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
                                             /* the device address too, which the device's later features may add */
                                             .usage = guest_usage() | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                             .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkExternalBufferProperties p = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    vkGetPhysicalDeviceExternalBufferProperties(g_vk.phys, &bi, &p);
    VkExternalMemoryFeatureFlags f = p.externalMemoryProperties.externalMemoryFeatures;
    return (f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) && !(f & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT);
}
int vk_uma_choose(int has_import, int has_ahb, int has_placed, int has_dmabuf) {
    int want = g_settings.guest_memory, how = g_settings.guest_memory_in_place, with;
    g_vk.mem_model = mem_model();
    int dear = imports_dear();
    /* a region holds whole windows: a dma-buf that can't hold one is none */
    u64 dmabuf_max = has_dmabuf && (g_vk.mem_model == VK_MEM_UMA || dear) ? plat_dmabuf_limit() : 0;
    int can_dmabuf = dmabuf_max >= UMA_WINDOW && dmabuf_importable();
    with = vk_uma_way(want, how, g_vk.mem_model, has_import, has_ahb, can_dmabuf, dear);
    int ahb_held = with == VK_UMA_WITH_NONE && how == 0 && want == 0 && g_vk.mem_model == VK_MEM_UMA && has_ahb;
    g_vk.mem_path = with == VK_UMA_WITH_NONE ? VK_MEM_PATH_COPY : VK_MEM_PATH_UMA;
    if (with == VK_UMA_WITH_NONE && !ahb_held && (want == 2 || (want == 0 && g_vk.mem_model == VK_MEM_UMA))) {
        if (how == 1 && !has_import)
            WARN("vulkan: guest memory in place was asked for, imported only (uma_memory=1), but the device has no "
                 "VK_EXT_external_memory_host: guest memory is copied\n");
        else if (how == 2)
            WARN("vulkan: guest memory in place was asked for, moved only (uma_memory=2), but the device has no "
                 "VK_ANDROID_external_memory_android_hardware_buffer: guest memory is copied\n");
        else
            WARN("vulkan: guest memory in place was asked for, but the device has " UMA_NO_IMPORT
                 ": guest memory is copied\n");
    }
    g_uma.migrate = with == VK_UMA_WITH_AHB || with == VK_UMA_WITH_DMABUF;
    g_uma.dmabuf = with == VK_UMA_WITH_DMABUF;
    g_uma.can_dmabuf = can_dmabuf;
    g_uma.dmabuf_max = dmabuf_max & ~(UMA_WINDOW - 1);
    g_uma.region_max = g_uma.dmabuf && g_uma.dmabuf_max < UMA_REGION_MAX ? g_uma.dmabuf_max : UMA_REGION_MAX;
    /* placed maps: a whole chunk mapped at an address of a window's alignment, given back reserved. Linux's and
       Android's, whose reservation plat_alias_unmap gives back. Decided again for a device made again without
       VK_EXT_map_memory_placed (device.c's create_device) */
    g_uma.can_place = 0;
#if defined(__linux__)
    if (has_placed && g_vk.mem_path == VK_MEM_PATH_UMA) {
        VkPhysicalDeviceMapMemoryPlacedFeaturesEXT f = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f};
        VkPhysicalDeviceMapMemoryPlacedPropertiesEXT p = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p};
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &f2);
        vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
        g_uma.can_place = f.memoryMapPlaced && f.memoryUnmapReserve && p.minPlacedMemoryMapAlignment &&
                          p.minPlacedMemoryMapAlignment <= MEM_PLACED;
    }
#endif
    LOG("vulkan: the device %s; guest buffers %s (vk_memory=%s%s), uma_memory=%s; VK_EXT_external_memory_host %s, "
        "VK_EXT_map_memory_placed %s\n",
        model_name(g_vk.mem_model),
        g_vk.mem_path != VK_MEM_PATH_UMA ? "copied"
        : g_uma.dmabuf ? "moved into memfds the GPU imports as dma-bufs (udmabuf) and reads in place, where they can be"
        : g_uma.migrate ? "moved into AHardwareBuffers the GPU reads in place, where they can be"
                                         : "read in place where they can be",
        want == 1   ? "copy"
        : want == 2 ? "uma"
                    : "auto",
        want == 2 && g_vk.mem_model != VK_MEM_UMA ? ", forced: the GPU reads them across the bus"
        : ahb_held                                ? ": AHardwareBuffers only with vk_memory=2 or uma_memory=2"
                                                  : "",
        how == 1   ? "import"
        : how == 2 ? "move"
                   : "auto",
        has_import ? "yes" : "no",
        !has_placed       ? "no"
        : g_uma.can_place ? "yes (the heap off it until vk_uma_placed)"
                          : "yes, without a whole chunk's placing or its address kept reserved");
    return with;
}

/* ------------------------------------------------------------ imports */
/* until the GPU has done submit sub, which is submitted first if it is still the one being recorded (vk_lock held) */
static void read_done(u64 sub) {
    u64 submitted, done;
    vk_submit_progress(&submitted, &done);
    if (sub <= done) return;
    if (sub > vk_last_submit()) { /* all that is recorded: every lookup so far has been read once it is done */
        vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
        g_uma.seg = vk_submit_number();
    }
    vk_wait_submit(sub);
}
/* The last submit that may read import m: the one its last lookup was in, or for a lookup since the draw or dispatch
   being set up began, the one being recorded now. A setup that waits for a landing (buffer.c's wait_finish, surface.c's
   flush_wait) submits what it recorded so far, and records the lookup's use in the next submit */
static u64 last_read(const Imp* m) { return m->sub >= g_uma.seg ? vk_submit_number() : m->sub; }
static u64 last_bound(const Chunk* c) { return c->sub >= g_uma.seg ? vk_submit_number() : c->sub; }
static void imp_destroy(const Imp* m) {
    vk_descriptor_heaps_forget_buffer(m->buf); /* its kept descriptors, before a new buffer can get its handle */
    vkDestroyBuffer(g_vk.dev, m->buf, NULL);
    vkFreeMemory(g_vk.dev, m->mem, NULL);
    if (m->fd >= 0) plat_gpu_buffer_free(m->fd, m->handle, m->size); /* a region's, whose pages moved off it */
    /* no scan watched its pages: what the CPU wrote there meanwhile counts. A dma-buf region's were watched */
    if (!m->tracked) vk_mem_changed(m->cpu, m->size);
}
/* dropped import i destroyed, then out of the list, which write tracking reads (vk_lock held) */
static void gone_out(u32 i) {
    imp_destroy(&g_uma.gone[i]);
    pm_lock(&g_uma.lk);
    g_uma.gone[i] = g_uma.gone[--g_uma.ngone];
    pm_unlock(&g_uma.lk);
}
/* the dropped imports whose last reader is done are destroyed (vk_lock held) */
static void gone_reap(void) {
    u64 submitted, done;
    vk_submit_progress(&submitted, &done);
    for (u32 i = g_uma.ngone; i-- > 0;)
        if (last_read(&g_uma.gone[i]) <= done) gone_out(i);
}
/* the dropped imports over guest memory [lo, hi) are destroyed now, once their last reader is done: their pages change
   or are trapped next (vk_lock held). Not a dropped region, whose buffer holds none of the guest's pages any more */
static void gone_kill(u64 lo, u64 hi) {
    for (u32 i = g_uma.ngone; i-- > 0;)
        if (g_uma.gone[i].fd < 0 && g_uma.gone[i].cpu < hi && lo < g_uma.gone[i].cpu + g_uma.gone[i].size) {
            read_done(last_read(&g_uma.gone[i]));
            gone_out(i);
        }
}
/* the import at i goes from the table, with its pages as they are: destroyed once the submit that last read it is done
   (gone_reap), or as soon as its pages change or are trapped (gone_kill), as the driver keeps it in every submission
   until then (vk_lock held). Never a wait here: the submit may be the one being recorded, a draw half set up */
static void imp_drop(u32 i) {
    rgn_drop(g_uma.imp[i].rid); /* its table is asked until its stamps are old */
    g_uma.imp[i].rid = 0;
    pm_lock(&g_uma.lk);
    if (g_uma.ngone == g_uma.capgone) {
        u32 cap = g_uma.capgone ? 2 * g_uma.capgone : 64;
        Imp* g = realloc(g_uma.gone, sizeof *g * cap);
        if (!g) fatal("vulkan: out of memory for %u imports of guest memory to destroy", cap);
        g_uma.gone = g;
        g_uma.capgone = cap;
    }
    g_uma.gone[g_uma.ngone++] = g_uma.imp[i];
    g_uma.dropped++;
    g_uma.regions++;
    memmove(&g_uma.imp[i], &g_uma.imp[i + 1], sizeof *g_uma.imp * (size_t)(g_uma.nimp - i - 1));
    g_uma.nimp--;
    pm_unlock(&g_uma.lk);
}
/* the import at i goes from the table (imp_drop), and a region's pages still in its buffer first move back into the
   reservation's own with their bytes, so that its memory, which the system never swaps, goes with the buffer. Pending
   commands read the buffer as it was (vk_lock held) */
static void region_writes_land(u64 lo, u64 hi);
static int region_writes_wait(u64 lo, u64 hi);
static void imp_let_go(u32 i) {
    const Imp* m = &g_uma.imp[i];
    int r;
    pm_lock(&g_uma.lk);
    int dead = m->dead;
    pm_unlock(&g_uma.lk);
    /* the writes in place still pending there land in its buffer first, which the move back copies; a dead one's pages
       are other pages already, which waited for them (vk_uma_mapping_begin) */
    if (m->fd >= 0 && !dead) region_writes_land(m->cpu, m->cpu + m->size);
    if (m->fd >= 0 && (r = mem_unmigrate(m->cpu, m->size)) != 0)
        WARN("vulkan: guest memory %llx+%llx stays in the buffer it moved into (error %d) until it is unmapped\n",
             (unsigned long long)m->cpu, (unsigned long long)m->size, r);
    imp_drop(i);
}
/* mem.c: host pages under guest memory [a, a + size) are other pages now (under its lock). The imports on them hold the
   old ones */
static void pages_replaced(u64 a, u64 size) {
    u64 end = a + size < a ? ~0ull : a + size;
    pm_lock(&g_uma.lk);
    for (u32 i = 0; i < FAIL_N; i++)
        if (g_uma.fail[i].hi && g_uma.fail[i].lo < end && a < g_uma.fail[i].hi)
            g_uma.fail[i].hi = 0; /* other pages: try again */
    for (u32 i = 0; i < g_uma.nimp; i++)
        if (g_uma.imp[i].cpu < end && a < g_uma.imp[i].cpu + g_uma.imp[i].size) {
            g_uma.imp[i].dead = 1;
            atomic_store_explicit(&g_uma.any_dead, 1, memory_order_release); /* the fast path reads it without lk */
        }
    pm_unlock(&g_uma.lk);
}
/* the import at i goes from the table and is destroyed now, once the submits that read it are done (vk_lock held),
   not when the frame's work is: the driver keeps it in every submission until then. A dead one's were waited for before
   its pages changed (vk_uma_mapping_begin), and a submit now would take it to the driver with the new ones */
static void imp_kill(u32 i, int dead) {
    if (!dead) read_done(last_read(&g_uma.imp[i]));
    rgn_drop(g_uma.imp[i].rid);
    imp_destroy(&g_uma.imp[i]); /* before it leaves the table: write tracking leaves its pages alone until then */
    pm_lock(&g_uma.lk);
    memmove(&g_uma.imp[i], &g_uma.imp[i + 1], sizeof *g_uma.imp * (size_t)(g_uma.nimp - i - 1));
    g_uma.nimp--;
    pm_unlock(&g_uma.lk);
    g_uma.dropped++;
    g_uma.regions++;
}
/* the dead ones go from the driver (vk_lock held) */
static void drop_dead(void) {
    pm_lock(&g_uma.lk);
    int any = atomic_load_explicit(&g_uma.any_dead, memory_order_relaxed);
    atomic_store_explicit(&g_uma.any_dead, 0, memory_order_relaxed);
    pm_unlock(&g_uma.lk);
    if (!any) return;
    for (u32 i = g_uma.nimp; i-- > 0;) {
        pm_lock(&g_uma.lk);
        int dead = g_uma.imp[i].dead;
        pm_unlock(&g_uma.lk);
        if (dead && g_uma.imp[i].fd >= 0)
            imp_let_go(i); /* its buffer keeps the bytes recorded commands read: no hurry */
        else if (dead)
            imp_kill(i, 1);
    }
}
/* the first import that ends after cpu */
static u32 imp_find(u64 cpu) {
    u32 lo = 0, hi = g_uma.nimp;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (g_uma.imp[mid].cpu + g_uma.imp[mid].size <= cpu)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
/* what a buffer over guest memory is bound as: what the staging ring's buffers are (cmd.c), as callers bind either,
   written in place and published into too */
static VkBufferUsageFlags guest_usage(void) {
    VkBufferUsageFlags u = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                           VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (g_vk.has_descriptor_buffers) u |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (g_vk.has_xfb) u |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    return u;
}

/* ------------------------------------------------------------ placed maps */
/* The heap and physical memory as GPU memory where they are (VK_EXT_map_memory_placed, while vk_uma_placed is on): a
   window of MEM_PLACED bytes they commit whole is a chunk of host-visible, coherent device memory that the driver maps
   at its guest address, over the reservation, and the GPU binds with nothing imported. No user pointers for the driver
   to take again at each submission (amdgpu's), so the rules of the imports above don't apply to it. mem.c commits and
   decommits its pages through here (mem_set_placed): a page committed again is zeroed then, as the GPU may still read
   it until its submits are done; a chunk with none committed goes, its address reserved again, and its memory once
   the submit that last bound it is done. Write tracking leaves its pages alone (vk_uma_import_in): userfaultfd can't
   register a mapping of the driver's */
static u32 chunk_find(u64 cpu) { /* the first chunk that ends after cpu */
    u32 lo = 0, hi = g_uma.nchunk;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (g_uma.chunk[mid].cpu + MEM_PLACED <= cpu)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
/* the chunk that holds all of [cpu, cpu + len), or none */
static Chunk* chunk_of(u64 cpu, u64 len) {
    u32 i = chunk_find(cpu);
    return i < g_uma.nchunk && g_uma.chunk[i].cpu <= cpu && cpu + len <= g_uma.chunk[i].cpu + MEM_PLACED
               ? &g_uma.chunk[i]
               : NULL;
}
static int chunk_meets(u64 lo, u64 hi) {
    u32 i = chunk_find(lo);
    return i < g_uma.nchunk && g_uma.chunk[i].cpu < hi;
}
static u32 view_find(u64 cpu) { /* the first view that ends after cpu */
    u32 lo = 0, hi = g_uma.nview;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (g_uma.view[mid].dst + g_uma.view[mid].size <= cpu)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
static int view_meets(u64 lo, u64 hi) {
    u32 i = view_find(lo);
    return i < g_uma.nview && g_uma.view[i].dst < hi;
}
/* the chunk that holds [*cpu, *cpu + len), in place or through a view (*cpu then the chunk's address of it) */
static Chunk* placed_of(u64* cpu, u64 len) {
    Chunk* c = chunk_of(*cpu, len);
    u32 i;
    if (c || !g_uma.nview) return c;
    i = view_find(*cpu);
    if (i == g_uma.nview || g_uma.view[i].dst > *cpu || *cpu + len > g_uma.view[i].dst + g_uma.view[i].size)
        return NULL;
    u64 at = g_uma.view[i].src + (*cpu - g_uma.view[i].dst);
    c = chunk_of(at, len);
    if (c) *cpu = at;
    return c;
}
static void chunk_destroy(const Chunk* c) {
    vk_descriptor_heaps_forget_buffer(c->buf); /* its kept descriptors, before a new buffer can get its handle */
    vkDestroyBuffer(g_vk.dev, c->buf, NULL);
    vkFreeMemory(g_vk.dev, c->mem, NULL);
    plat_vm_fd_close(c->fd); /* a view still mapped keeps its pages */
}
/* the chunks given back whose last binding's submit is done go (vk_lock held) */
static void chunk_reap(void) {
    u64 submitted, done;
    vk_submit_progress(&submitted, &done);
    for (u32 i = g_uma.ncgone; i-- > 0;)
        if (g_uma.cgone[i].sub <= done) {
            chunk_destroy(&g_uma.cgone[i]);
            pm_lock(&g_uma.lk);
            g_uma.cgone[i] = g_uma.cgone[--g_uma.ncgone];
            pm_unlock(&g_uma.lk);
        }
}
/* GPU memory for the window at cpu, mapped there over the reservation: 0 when the driver refuses */
static int chunk_make(Chunk* c, u64 cpu) {
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = MEM_PLACED,
                              .usage = guest_usage(),
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkMemoryRequirements req;
    /* exportable as a dma-buf, which views map */
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    if (g_uma.can_view) bci.pNext = &eb;
    *c = (Chunk){.cpu = cpu, .fd = -1};
    if (!VK_OK(vkCreateBuffer(g_vk.dev, &bci, NULL, &c->buf))) return 0;
    vkGetBufferMemoryRequirements(g_vk.dev, c->buf, &req);
    VkMemoryAllocateFlagsInfo flags = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                       .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkExportMemoryAllocateInfo ex = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                                     .pNext = g_vk.has_descriptor_buffers ? &flags : NULL,
                                     .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .pNext = g_uma.can_view                ? (const void*)&ex
                                        : g_vk.has_descriptor_buffers ? (const void*)&flags
                                                                      : NULL,
                               .allocationSize = MEM_PLACED,
                               .memoryTypeIndex = g_uma.place_type};
    if (req.size > MEM_PLACED || !((req.memoryTypeBits >> g_uma.place_type) & 1) ||
        !VK_OK(vkAllocateMemory(g_vk.dev, &ai, NULL, &c->mem))) {
        vkDestroyBuffer(g_vk.dev, c->buf, NULL);
        return 0;
    }
    VkMemoryMapPlacedInfoEXT pi = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT, .pPlacedAddress = GP(cpu)};
    VkMemoryMapInfoKHR mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR,
                             .pNext = &pi,
                             .flags = VK_MEMORY_MAP_PLACED_BIT_EXT,
                             .memory = c->mem,
                             .size = VK_WHOLE_SIZE};
    void* at = NULL;
    if (!VK_OK(vkBindBufferMemory(g_vk.dev, c->buf, c->mem, 0)) || !VK_OK(vkMapMemory2KHR(g_vk.dev, &mi, &at)) ||
        at != (void*)GP(cpu)) {
        if (at) {
            VkMemoryUnmapInfoKHR ui = {.sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR,
                                       .flags = VK_MEMORY_UNMAP_RESERVE_BIT_EXT,
                                       .memory = c->mem};
            vkUnmapMemory2KHR(g_vk.dev, &ui);
        }
        plat_alias_unmap(GP(cpu), MEM_PLACED); /* the reservation again, whatever a failed map left there */
        chunk_destroy(c);
        return 0;
    }
    return 1;
}
/* the chunk at i given back: its address reserved again, as plat_vm_reserve leaves it (watched for writes again), and
   its memory the driver's until the submit that last bound it is done (vk_lock held) */
static u64 last_bound(const Chunk* c);
static void chunk_give_back(u32 i) {
    rgn_drop(g_uma.chunk[i].rid);
    g_uma.chunk[i].rid = 0;
    Chunk c = g_uma.chunk[i];
    VkMemoryUnmapInfoKHR ui = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR, .flags = VK_MEMORY_UNMAP_RESERVE_BIT_EXT, .memory = c.mem};
    vkUnmapMemory2KHR(g_vk.dev, &ui);
    if (plat_alias_unmap(GP(c.cpu), MEM_PLACED))
        fatal("memory: guest memory %llx+%llx could not be reserved again after its GPU memory went",
              (unsigned long long)c.cpu, (unsigned long long)MEM_PLACED);
    c.sub = last_bound(&c);
    pm_lock(&g_uma.lk);
    if (g_uma.ncgone == g_uma.capcgone) {
        u32 cap = g_uma.capcgone ? 2 * g_uma.capcgone : 16;
        Chunk* n = realloc(g_uma.cgone, sizeof *n * cap);
        if (!n) fatal("vulkan: out of memory for %u chunks of guest memory to destroy", cap);
        g_uma.cgone = n;
        g_uma.capcgone = cap;
    }
    g_uma.cgone[g_uma.ncgone++] = c;
    memmove(&g_uma.chunk[i], &g_uma.chunk[i + 1], sizeof *g_uma.chunk * (size_t)(g_uma.nchunk - i - 1));
    g_uma.nchunk--;
    pm_unlock(&g_uma.lk);
    g_uma.chunks_gone++;
    g_uma.regions++;
}
static void page_bits(Chunk* c, u64 a, u64 size, int on) {
    for (u64 p = (a - c->cpu) >> 12, e = (a - c->cpu + size + 0xFFF) >> 12; p < e; p++)
        if (on)
            c->pages[p >> 6] |= 1ull << (p & 63);
        else
            c->pages[p >> 6] &= ~(1ull << (p & 63));
}
/* mem.c's hooks, under its lock and vk_lock (mapping_begin), [a, a + size) within one window */
static int placed_commit(u64 a, u64 size) {
    u64 w = a & ~(MEM_PLACED - 1);
    Chunk* c = chunk_of(w, MEM_PLACED);
    if (c) { /* pages of a chunk committed again: zero, as fresh pages read */
        if (plat_vm_commit(GP(a), size)) return -1;
        memset(GP(a), 0, (size_t)size);
        page_bits(c, a, size, 1);
        return 0;
    }
    if (!g_uma.placed || a != w || size != MEM_PLACED || mem_identity()) return -1;
    chunk_reap();
    if (g_uma.nchunk == g_uma.capchunk) {
        u32 cap = g_uma.capchunk ? 2 * g_uma.capchunk : 64;
        Chunk* n = realloc(g_uma.chunk, sizeof *n * cap);
        if (!n) fatal("vulkan: out of memory for %u chunks of guest memory", cap);
        pm_lock(&g_uma.lk);
        g_uma.chunk = n;
        g_uma.capchunk = cap;
        pm_unlock(&g_uma.lk);
    }
    Chunk m;
    if (!chunk_make(&m, w)) {
        LOG_LIMIT(8, "vulkan: guest memory %llx+%llx could not be GPU memory where it is: host pages there\n",
                  (unsigned long long)w, (unsigned long long)MEM_PLACED);
        return -1;
    }
    vk_descriptor_heaps_lasting(m.buf); /* it lives past the frame: its descriptors are kept (chunk_destroy forgets) */
    memset(GP(w), 0, (size_t)MEM_PLACED); /* the driver's memory may hold anything */
    memset(m.pages, 0xFF, sizeof m.pages);
    m.rid = rgn_new(w, MEM_PLACED, m.buf, 1);
    u32 i = chunk_find(w);
    pm_lock(&g_uma.lk);
    memmove(&g_uma.chunk[i + 1], &g_uma.chunk[i], sizeof *g_uma.chunk * (size_t)(g_uma.nchunk - i));
    g_uma.chunk[i] = m;
    g_uma.nchunk++;
    pm_unlock(&g_uma.lk);
    g_uma.chunks_made++;
    g_uma.regions++;
    return 0;
}
/* the views over [a, a + size) unmapped, their addresses reserved again, and the rest of the range decommitted */
static void view_unmap(u64 a, u64 size) {
    u64 end = a + size, at = a;
    for (u32 i = view_find(a); i < g_uma.nview && g_uma.view[i].dst < end;) {
        View v = g_uma.view[i];
        u64 lo = v.dst > a ? v.dst : a, hi = v.dst + v.size < end ? v.dst + v.size : end;
        if (lo > at) plat_vm_decommit(GP(at), lo - at);
        if (plat_alias_unmap(GP(lo), hi - lo))
            fatal("memory: guest memory %llx+%llx could not be reserved again after its view went",
                  (unsigned long long)lo, (unsigned long long)(hi - lo));
        at = hi;
        pm_lock(&g_uma.lk);
        if (v.dst < lo && v.dst + v.size > hi) { /* the middle of it: two views */
            if (g_uma.nview == g_uma.capview) {
                u32 cap = g_uma.capview ? 2 * g_uma.capview : 16;
                View* n = realloc(g_uma.view, sizeof *n * cap);
                if (!n) fatal("vulkan: out of memory for %u views of GPU memory", cap);
                g_uma.view = n;
                g_uma.capview = cap;
            }
            memmove(&g_uma.view[i + 2], &g_uma.view[i + 1], sizeof *g_uma.view * (size_t)(g_uma.nview - i - 1));
            g_uma.view[i] = (View){v.dst, v.src, lo - v.dst};
            g_uma.view[i + 1] = (View){hi, v.src + (hi - v.dst), v.dst + v.size - hi};
            g_uma.nview++;
            i += 2;
        } else if (v.dst < lo) { /* its head stays */
            g_uma.view[i].size = lo - v.dst;
            i++;
        } else if (v.dst + v.size > hi) { /* its tail */
            g_uma.view[i] = (View){hi, v.src + (hi - v.dst), v.dst + v.size - hi};
            i++;
        } else { /* all of it */
            memmove(&g_uma.view[i], &g_uma.view[i + 1], sizeof *g_uma.view * (size_t)(g_uma.nview - i - 1));
            g_uma.nview--;
        }
        pm_unlock(&g_uma.lk);
    }
    if (at < end) plat_vm_decommit(GP(at), end - at);
}
static int placed_decommit(u64 a, u64 size) {
    u64 w = a & ~(MEM_PLACED - 1);
    Chunk* c = chunk_of(w, MEM_PLACED);
    if (!c && g_uma.nview && view_meets(a, a + size)) {
        view_unmap(a, size);
        return 1;
    }
    if (!c) return 0;
    plat_vm_decommit(GP(a), size); /* inaccessible: what it held stays, for the GPU's reads still to run */
    page_bits(c, a, size, 0);
    for (u32 k = 0; k < sizeof c->pages / sizeof *c->pages; k++)
        if (c->pages[k]) return 1;
    chunk_give_back((u32)(c - g_uma.chunk));
    return 1;
}
static int placed_held(u64 a, u64 size) {
    pm_lock(&g_uma.lk);
    int r = (g_uma.nchunk && chunk_meets(a, a + size)) || (g_uma.nview && view_meets(a, a + size));
    pm_unlock(&g_uma.lk);
    return r;
}
/* a chunk's dma-buf, made once: -1 when the driver gives none */
static int chunk_fd(Chunk* c) {
    if (c->fd >= 0) return c->fd;
    VkMemoryGetFdInfoKHR gi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
                               .memory = c->mem,
                               .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    int fd = -1;
    if (VK_OK(vkGetMemoryFdKHR(g_vk.dev, &gi, &fd))) c->fd = fd;
    return c->fd;
}
/* the piece of src from s on that one chunk or one view holds, up to n bytes */
static u64 view_piece(u64 s, u64 n) {
    u64 w = (s | (MEM_PLACED - 1)) + 1;
    if (w - s < n) n = w - s;
    u32 v = g_uma.nview ? view_find(s) : 0;
    if (v < g_uma.nview && g_uma.view[v].dst <= s && g_uma.view[v].dst + g_uma.view[v].size - s < n)
        n = g_uma.view[v].dst + g_uma.view[v].size - s;
    return n;
}
/* MapMemory's or code memory's view of src at free dst: the chunks' own pages mapped there too, each piece of src a
   chunk's or a view's of one. -1, nothing mapped, when a piece isn't, or a mapping fails (mem.c copies then) */
static int placed_view(u64 dst, u64 src, u64 size) {
    u64 at;
    if (!g_uma.can_view) return -1;
    for (at = 0; at < size;) { /* every piece first, before anything is mapped */
        u64 s = src + at, n = view_piece(s, size - at);
        if (!placed_of(&s, n)) return -1;
        at += n;
    }
    for (at = 0; at < size;) {
        u64 s = src + at, n = view_piece(s, size - at);
        Chunk* c = placed_of(&s, n);
        int fd = chunk_fd(c);
        if (fd < 0 || plat_vm_map_fd(GP(dst + at), n, fd, s - c->cpu, MEM_PLACED)) {
            plat_alias_unmap(GP(dst + at), n); /* whatever a failed MAP_FIXED left */
            if (at) view_unmap(dst, at);
            LOG_LIMIT(8, "vulkan: a view of GPU memory %llx+%llx at %llx could not be mapped: it is a copy\n",
                      (unsigned long long)src, (unsigned long long)size, (unsigned long long)dst);
            return -1;
        }
        pm_lock(&g_uma.lk);
        if (g_uma.nview == g_uma.capview) {
            u32 cap = g_uma.capview ? 2 * g_uma.capview : 16;
            View* m = realloc(g_uma.view, sizeof *m * cap);
            if (!m) fatal("vulkan: out of memory for %u views of GPU memory", cap);
            g_uma.view = m;
            g_uma.capview = cap;
        }
        u32 i = view_find(dst + at);
        memmove(&g_uma.view[i + 1], &g_uma.view[i], sizeof *g_uma.view * (size_t)(g_uma.nview - i));
        g_uma.view[i] = (View){dst + at, s, n};
        g_uma.nview++;
        pm_unlock(&g_uma.lk);
        at += n;
    }
    return 0;
}
static const MemPlaced g_placed_hooks = {placed_commit, placed_decommit, placed_held, placed_view};
int vk_uma_can_place(void) { return g_uma.can_place; }
int vk_uma_placed(int on) {
    int was = g_uma.placed;
    g_uma.placed = on && g_uma.can_place;
    return was;
}
int vk_uma_placed_at(u64 cpu, u64 len) { return g_uma.nchunk && placed_of(&cpu, len); }
void vk_uma_dmabuf(int has) { g_uma.can_view = has; }
int vk_uma_can_view(void) { return g_uma.can_view && g_uma.can_place; }

/* m's buffer over guest memory [cpu, cpu + size), in memory the driver imports: import, an import info of handle type
   type, in a memory type that bits allows, alloc bytes of it. 0 when the driver refused it */
static int imp_bind(Imp* m, u64 cpu, u64 size, VkExternalMemoryHandleTypeFlagBits type, const void* import, u32 bits,
                    u64 alloc) {
    VkExternalMemoryBufferCreateInfo ext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                            .handleTypes = (VkExternalMemoryHandleTypeFlags)type};
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .pNext = &ext,
                              .size = size,
                              .usage = guest_usage(),
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkMemoryRequirements req;
    const VkPhysicalDeviceMemoryProperties* mp;
    *m = (Imp){.cpu = cpu, .size = size, .used = g_vk_present, .fd = -1};
    if (!VK_OK(vkCreateBuffer(g_vk.dev, &bci, NULL, &m->buf))) {
        m->buf = VK_NULL_HANDLE;
        return 0;
    }
    vkGetBufferMemoryRequirements(g_vk.dev, m->buf, &req);
    mp = vk_devmem_props();
    bits &= req.memoryTypeBits;
    u32 t = ~0u;
    /* the GPU reads it: device-local where the import can be (a GPU that shares memory), else any it takes */
    for (u32 k = 0; k < mp->memoryTypeCount; k++)
        if ((bits >> k) & 1) {
            if (t == ~0u || (mp->memoryTypes[k].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) t = k;
        }
    VkMemoryAllocateFlagsInfo flags = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                       .pNext = import,
                                       .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .pNext = g_vk.has_descriptor_buffers ? (const void*)&flags : import,
                               .allocationSize = alloc,
                               .memoryTypeIndex = t};
    if (t == ~0u || req.size > alloc ||
        (g_uma.import_result = vkAllocateMemory(g_vk.dev, &ai, NULL, &m->mem)) != VK_SUCCESS) {
        vkDestroyBuffer(g_vk.dev, m->buf, NULL);
        m->buf = VK_NULL_HANDLE;
        m->mem = VK_NULL_HANDLE;
        return 0;
    }
    if (!VK_OK(vkBindBufferMemory(g_vk.dev, m->buf, m->mem, 0))) {
        vkDestroyBuffer(g_vk.dev, m->buf, NULL);
        vkFreeMemory(g_vk.dev, m->mem, NULL);
        m->buf = VK_NULL_HANDLE;
        m->mem = VK_NULL_HANDLE;
        return 0;
    }
    /* the memory type the driver gave a region (once for each handle type): whether the GPU snoops the CPU's caches
       on it (coherent and cached), and whether the CPU's writes need a flush, which no code here does */
    const VkMemoryPropertyFlags pf = mp->memoryTypes[t].propertyFlags;
    LOG_LIMIT(2,
              "vulkan: a region imported as %s in memory type %u of the %u it could take (%x): host visible %d, "
              "coherent %d, cached %d, device local %d\n",
              type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT ? "a dma-buf" : "host memory", t,
              snx_popcount64(bits), (unsigned)bits, (pf & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0,
              (pf & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0, (pf & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0,
              (pf & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0);
    return 1;
}
/* guest memory [cpu, cpu + size), at host, imported as a buffer: 0 when the driver refused it. host is the guest's own
   memory (GP(cpu)), or in the migrate way off Android the buffer it moves into (the tests' memfd) */
static int imp_make(Imp* m, u64 cpu, u64 size, void* host) {
    VkMemoryHostPointerPropertiesEXT hp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                            .pHostPointer = host};
    /* a refusal is expected (pages the driver can't pin) and said once for the span (fail), not as a failed call */
    g_uma.import_result = vkGetMemoryHostPointerPropertiesEXT(
        g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &hp);
    if (g_uma.import_result != VK_SUCCESS) return 0;
    return imp_bind(m, cpu, size, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, &imp, hp.memoryTypeBits,
                    size);
}
#if defined(__ANDROID__)
/* a region's AHardwareBuffer imported as its buffer: 0 when the driver refused it. Through the device's function, as
   volk loads only the extensions of the platforms it was built for */
static int imp_make_ahb(Imp* m, u64 cpu, u64 size, void* ahb) {
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID props =
        (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkGetDeviceProcAddr(
            g_vk.dev, "vkGetAndroidHardwareBufferPropertiesANDROID");
    VkAndroidHardwareBufferPropertiesANDROID ap = {.sType =
                                                       VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    VkImportAndroidHardwareBufferInfoANDROID imp = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, .buffer = (struct AHardwareBuffer*)ahb};
    g_uma.import_result =
        props ? props(g_vk.dev, (const struct AHardwareBuffer*)ahb, &ap) : VK_ERROR_EXTENSION_NOT_PRESENT;
    if (g_uma.import_result != VK_SUCCESS) return 0;
    return imp_bind(m, cpu, size, VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID, &imp,
                    ap.memoryTypeBits, ap.allocationSize);
}
#endif
/* a region's memfd fd as a dma-buf (udmabuf) imported as its buffer: 0 when it can't be. The memfd is never punched
   while the dma-buf lives (plat_gpu_buffer_free only closes and unmaps): its pinned pages would stay the GPU's while
   the CPU's became others */
static int imp_make_dmabuf(Imp* m, u64 cpu, u64 size, int fd) {
    int dmabuf = plat_gpu_buffer_dmabuf(fd, size);
    if (dmabuf < 0) {
        g_uma.import_result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
        return 0;
    }
    VkMemoryFdPropertiesKHR fp = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    VkResult r = vkGetMemoryFdPropertiesKHR(g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dmabuf, &fp);
    if (r != VK_SUCCESS) {
        g_uma.import_result = r;
        plat_vm_fd_close(dmabuf);
        return 0;
    }
    /* host-visible, coherent and cached where the driver offers it: the pages are the system's, snooped, which the
       guest's CPU reads and writes through the memfd's mapping, not through vkMapMemory */
    const VkPhysicalDeviceMemoryProperties* mp = vk_devmem_props();
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                       VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    u32 bits = 0;
    for (u32 k = 0; k < mp->memoryTypeCount; k++)
        if ((fp.memoryTypeBits >> k & 1) && (mp->memoryTypes[k].propertyFlags & want) == want) bits |= 1u << k;
    VkImportMemoryFdInfoKHR imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                                   .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                   .fd = dmabuf};
    /* the import owns the descriptor once vkAllocateMemory took it (imp_bind's import_result), and frees it with the
       memory when the bind after it fails: it is closed here only when the allocation never took it */
    g_uma.import_result = VK_ERROR_INITIALIZATION_FAILED;
    if (!imp_bind(m, cpu, size, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, &imp, bits ? bits : fp.memoryTypeBits,
                  size)) {
        if (g_uma.import_result != VK_SUCCESS) plat_vm_fd_close(dmabuf);
        return 0;
    }
    m->tracked = 1;
    return 1;
}
/* guest memory [cpu, cpu + size) as a region (the migrate way): a buffer of its own, imported, that its pages then move
   into with their bytes, those of the regions it meets included. NULL, or why it can't be */
static const char* region_make(Imp* m, u64 cpu, u64 size) {
    int fd, r;
    void* handle;
    if (plat_gpu_buffer(size, &fd, &handle)) {
        snprintf(g_uma.why, sizeof g_uma.why, "no buffer of that size: %s", strerror(errno));
        return g_uma.why;
    }
#if defined(__ANDROID__)
    r = imp_make_ahb(m, cpu, size, handle);
#else
    r = g_uma.dmabuf ? imp_make_dmabuf(m, cpu, size, fd) : imp_make(m, cpu, size, handle);
#endif
    if (!r) {
        plat_gpu_buffer_free(fd, handle, size);
        return snx_vk_result_str(g_uma.import_result);
    }
    if ((r = mem_migrate(cpu, size, fd)) != 0) {
        vkDestroyBuffer(g_vk.dev, m->buf, NULL);
        vkFreeMemory(g_vk.dev, m->mem, NULL);
        plat_gpu_buffer_free(fd, handle, size);
        snprintf(g_uma.why, sizeof g_uma.why, "its pages could not move into the buffer (error %d)", r);
        return g_uma.why;
    }
    m->fd = fd;
    m->handle = handle;
    return NULL;
}
/* whether [lo, hi) meets a span the driver refused lately (vk_lock held) */
static int failed(u64 lo, u64 hi) {
    int r = 0;
    pm_lock(&g_uma.lk);
    for (u32 i = 0; i < FAIL_N && !r; i++)
        r = g_uma.fail[i].hi && (u32)(g_vk_present - g_uma.fail[i].at) <= UMA_IDLE && g_uma.fail[i].lo < hi &&
            lo < g_uma.fail[i].hi;
    pm_unlock(&g_uma.lk);
    return r;
}
/* [lo, hi) refused, for why: in place of the oldest span kept */
static void fail(u64 lo, u64 hi, const char* why) {
    u32 old = 0;
    g_uma.failed++;
    LOG_LIMIT(8, "vulkan: guest memory %llx+%llx could not be imported (%s): its ranges are copied for a while\n",
              (unsigned long long)lo, (unsigned long long)(hi - lo), why);
    pm_lock(&g_uma.lk);
    for (u32 i = 1; i < FAIL_N && g_uma.fail[old].hi; i++)
        if (!g_uma.fail[i].hi || (i32)(g_uma.fail[i].at - g_uma.fail[old].at) < 0) old = i;
    g_uma.fail[old] = (Fail){lo, hi, g_vk_present};
    pm_unlock(&g_uma.lk);
}
/* whether guest memory [lo, hi) can be imported: mapped, and in the migrate way the guest's data that can move (on
   the reservation's own pages, or pool pages no other address maps: mem_movable). No page a trap holds (memory/own.c): a driver may read an import's pages on the CPU from
   this thread, which holds vk_lock and so can't have them landed (PanVK on a Mali phone read one up to its trapped
   page, #1875), and amdgpu fails a submission over one */
static int imp_can(u64 lo, u64 hi) {
    if (own_trapped(lo, hi - lo)) return 0;
    return g_uma.migrate ? mem_movable(lo, hi - lo) : mem_mapped_run(lo, hi - lo) == hi - lo;
}
/* the import that holds guest memory [cpu, cpu + len), made if there is none: its index, or ~0 for none (the range is
   copied) */
static u32 imp_get(u64 cpu, u64 len) {
    u32 i = imp_find(cpu);
    if (i < g_uma.nimp && g_uma.imp[i].cpu <= cpu && cpu + len <= g_uma.imp[i].cpu + g_uma.imp[i].size) return i;
    /* the windows the range touches, and the imports they meet, as one import. Not over a chunk of placed memory,
       which the driver maps already: a range across its edge is copied */
    u64 lo = cpu & ~(UMA_WINDOW - 1), hi = (cpu + len + UMA_WINDOW - 1) & ~(UMA_WINDOW - 1);
    if ((g_uma.nchunk && chunk_meets(lo, hi)) || (g_uma.nview && view_meets(lo, hi))) {
        g_uma.imp_why[0]++;
        return ~0u;
    }
    u32 first = imp_find(lo), last = first;
    while (last < g_uma.nimp && g_uma.imp[last].cpu < hi) {
        if (g_uma.imp[last].cpu < lo) lo = g_uma.imp[last].cpu;
        if (g_uma.imp[last].cpu + g_uma.imp[last].size > hi) hi = g_uma.imp[last].cpu + g_uma.imp[last].size;
        last++;
    }
    /* only mapped guest memory (the migrate way: the guest's data on the reservation's own pages, which can move) with
       no page trapped: the range's own pages where the windows are not all of it */
    if (!imp_can(lo, hi)) {
        if (last > first) {
            g_uma.imp_why[1]++;
            return ~0u;
        }
        lo = cpu & ~(g_uma.align - 1);
        hi = (cpu + len + g_uma.align - 1) & ~(g_uma.align - 1);
        if (!imp_can(lo, hi)) {
            g_uma.imp_why[2]++;
            return ~0u;
        }
    }
    u64 cap = g_uma.migrate && g_vk.alloc_cap > g_uma.region_max ? g_uma.region_max : g_vk.alloc_cap;
    if (hi - lo > cap || failed(lo, hi)) {
        g_uma.imp_why[hi - lo > cap ? 3 : 4]++;
        return ~0u;
    }
    if (g_uma.nimp - (last - first) >= UMA_MAX) { /* full: the least recently used goes */
        u32 old = 0;
        for (u32 k = 1; k < g_uma.nimp; k++)
            if ((i32)(g_uma.imp[k].used - g_uma.imp[old].used) < 0) old = k;
        if (g_uma.no_wait && region_writes_wait(g_uma.imp[old].cpu, g_uma.imp[old].cpu + g_uma.imp[old].size)) {
            g_uma.would_wait++;
            return ~0u;
        }
        imp_let_go(old);
        g_uma.imp_why[5]++;
        return ~0u; /* the indexes moved: the next lookup makes it */
    }
    /* the regions the new one takes over: the writes in place still pending in them land in their buffers first, as
       the move copies what their pages hold (mem_migrate), and a write landing after it would land in a buffer no
       page maps any more (a copy engine's results into a region read nought past the lookup that grew it) */
    if (g_uma.migrate && last > first) {
        if (g_uma.no_wait && region_writes_wait(lo, hi)) {
            g_uma.would_wait++;
            return ~0u;
        }
        u32 n = g_uma.nimp;
        u64 dropped = g_uma.dropped;
        region_writes_land(lo, hi);
        if (g_uma.nimp != n || g_uma.dropped != dropped) return ~0u; /* the wait moved the indexes: the next lookup */
    }
    Imp m;
    u64 t0 = plat_ns();
    /* the driver takes the pages for writing, and refuses those a scan protected (write tracking by protection,
       write_tracking=1 or a host without userfaultfd's): they open for the import, and close once it is in the table,
       whose pages the scans leave alone from then on (vk_uma_import_in) */
    const int opened = !g_uma.migrate;
    if (opened) plat_ww_open(GP(lo), hi - lo);
    const char* why = g_uma.migrate                       ? region_make(&m, lo, hi - lo)
                      : imp_make(&m, lo, hi - lo, GP(lo)) ? NULL
                                                          : snx_vk_result_str(g_uma.import_result);
    if (why) {
        if (opened) plat_ww_close(GP(lo), hi - lo);
        fail(lo, hi, why);
        return ~0u; /* the imports there stay */
    }
    vk_descriptor_heaps_lasting(m.buf); /* it lives past the frame: its descriptors are kept (imp_destroy forgets) */
    g_uma.imports++;
    g_uma.import_bytes += hi - lo;
    g_uma.import_ns += plat_ns() - t0;
    if (last > first) {
        g_uma.merges++;
        g_uma.merged += last - first;
    }
    /* its table, with the stamps of the regions it takes over. Only a region of whole windows has one: where the
       windows' pages are not all mapped or movable, the import holds the range's own pages (above), which the tables
       of before stamp, as g_uma.win names a slot for whole windows only. A table of whole windows over two pages
       wrote past its end, and below its start for a page beside them (a race crashed in rg_stamp) */
    if (!((lo | hi) & (UMA_WINDOW - 1))) {
        m.rid = rgn_new(lo, hi - lo, m.buf, 0);
        for (u32 k = first; k < last; k++)
            if (g_uma.imp[k].rid) rgn_take(&g_uma.rg[m.rid], &g_uma.rg[g_uma.imp[k].rid]);
    }
    for (u32 k = last; k-- > first;) imp_drop(k); /* a region's pages are the new one's now */
    pm_lock(&g_uma.lk);
    memmove(&g_uma.imp[first + 1], &g_uma.imp[first], sizeof *g_uma.imp * (size_t)(g_uma.nimp - first));
    g_uma.imp[first] = m;
    g_uma.nimp++;
    g_uma.regions++;
    pm_unlock(&g_uma.lk);
    if (opened) plat_ww_close(GP(lo), hi - lo);
    return first;
}

/* ------------------------------------------------------------ the renderer's side */
static void hz_mark(u64 cpu, u64 len);
static void order(u64 cpu, u64 len, int write);
static void note_read(u64 cpu, u64 len);
static int bind(u64 cpu, u64 len, u64 align, VkBuffer* buf, u64* off);
/* The GPU range [gva, gva + len) bound where it is in guest memory, at an offset that is a multiple of align: 1 with
   the buffer and offset, 0 when it is copied (vk_lock held). The caller has checked that no pending GPU result is on
   it and that the GPU does not own it. It is read after what wrote it in place (order) */
int vk_uma_get(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off) {
    u64 cpu = 0;
    if (!len || gmmu_run(gva, len, &cpu) < len) { /* not one run of guest memory */
        g_uma.misses++;
        return 0;
    }
    /* the runtime wrote it: copied, as when it was bound. Not while the GPU writes it in place (an engine's result
       published there): its bytes are only in the import, a copy of them on the GPU would end the rendering pass, and
       the runtime's next write there waits for that write anyway (front_write) */
    if (write_pages_since(&g_uma.front, 0, cpu, len) && !vk_uma_pending_cpu(cpu, len)) {
        g_uma.misses++;
        return 0;
    }
    if (!bind(cpu, len, align, buf, off)) return 0;
    order(cpu, len, 0);
    note_read(cpu, len);
    return 1;
}
/* guest memory [cpu, cpu + len) bound where it is (vk_lock held): the import's buffer and the offset in it, stamped
   with the submit being recorded */
static Chunk* chunk_of(u64 cpu, u64 len);
static int bind(u64 cpu, u64 len, u64 align, VkBuffer* buf, u64* off) {
    drop_dead();
    u64 at = cpu;
    Chunk* c = g_uma.nchunk ? placed_of(&at, len) : NULL;
    if (c) { /* GPU memory where it is, or a view's: nothing to import */
        cpu = at;
        if ((cpu - c->cpu) % (align ? align : 1)) {
            g_uma.misses++;
            return 0;
        }
        c->sub = vk_submit_number();
        *buf = c->buf;
        *off = cpu - c->cpu;
        g_uma.bound = vk_submit_number();
        g_uma.hits++;
        g_uma.hit_bytes += len;
        hz_mark(cpu, len);
        return 1;
    }
    u32 i = imp_get(cpu, len);
    if (i == ~0u || ((cpu - g_uma.imp[i].cpu) % (align ? align : 1))) {
        g_uma.misses++;
        return 0;
    }
    g_uma.imp[i].used = g_vk_present;
    g_uma.imp[i].sub = vk_submit_number();
    *buf = g_uma.imp[i].buf;
    *off = cpu - g_uma.imp[i].cpu;
    g_uma.bound = vk_submit_number();
    g_uma.hits++;
    g_uma.hit_bytes += len;
    hz_mark(cpu, len);
    return 1;
}

/* ------------------------------------------------------------ the fast path */
/* r's Imp or Chunk (slot id), found at its index when last looked up, else again: its last binding is now, as bind
   stamps it (last_read, the idle presents). 0 when it has none (vk_lock held) */
static int rgn_bound(Rgn* r, u32 id) {
    if (r->chunk) {
        if (r->at >= g_uma.nchunk || g_uma.chunk[r->at].rid != id) r->at = chunk_find(r->cpu);
        if (r->at >= g_uma.nchunk || g_uma.chunk[r->at].rid != id) return 0;
        g_uma.chunk[r->at].sub = vk_submit_number();
        return 1;
    }
    if (r->at >= g_uma.nimp || g_uma.imp[r->at].rid != id) r->at = imp_find(r->cpu);
    if (r->at >= g_uma.nimp || g_uma.imp[r->at].rid != id) return 0;
    g_uma.imp[r->at].used = g_vk_present;
    g_uma.imp[r->at].sub = vk_submit_number();
    return 1;
}
/* [cpu, cpu + len) of region r bound for a read: what bind, vk_uma_get and their stamps do. ordered 0 leaves out the
   barrier after the writes in place before it (order), for a read the caller puts ahead of them */
static void read_in(const Rgn* r, u64 cpu, u64 len, int ordered, VkBuffer* buf, u64* off) {
    *buf = r->buf;
    *off = cpu - r->cpu;
    g_uma.bound = vk_submit_number();
    g_uma.hits++;
    g_uma.hit_bytes += len;
    hz_mark(cpu, len);
    if (ordered) order(cpu, len, 0);
    note_read(cpu, len);
}
int vk_uma_read(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off) {
    if (!len) return 0;
    /* a region whose pages are other pages now goes first, in the old chain's bind (drop_dead) */
    if (atomic_load_explicit(&g_uma.any_dead, memory_order_acquire)) {
        g_uma.fast_miss[UMA_FAST_DEAD]++;
        return 0;
    }
    UmaMemo* m = &g_uma.memo[(u32)(((gva ^ (len << 40)) * 0x9E3779B97F4A7C15ull) >> 52)];
    const u32 mg = vk_buf_map_gen();
    u64 cpu = 0;
    Rgn* r = m->rid ? &g_uma.rg[m->rid] : NULL;
    /* resolved before, with the same mappings into the same region, and no page of it blk since: no other lookup */
    if (r && m->gva == gva && m->len == len && m->map_gen == mg && r->live && r->gen == m->gen &&
        (r->bgen == m->bgen || !rg_blk(r, m->cpu, len))) {
        cpu = m->cpu;
        m->bgen = r->bgen;
    } else {
        int why = gmmu_run(gva, len, &cpu) < len ? UMA_FAST_RUN : -1;
        u32 rid = why < 0 ? rid_at(cpu) : 0;
        r = rid ? &g_uma.rg[rid] : NULL;
        if (why < 0 && !r) why = UMA_FAST_NONE;
        if (why < 0 && cpu + len > r->cpu + r->size) why = UMA_FAST_EDGE;
        if (why < 0 && rg_blk(r, cpu, len)) why = UMA_FAST_BLK;
        if (why >= 0) {
            g_uma.fast_miss[why]++;
            m->rid = 0;
            return 0;
        }
        *m = (UmaMemo){.gva = gva, .len = len, .cpu = cpu, .gen = r->gen, .bgen = r->bgen, .map_gen = mg, .rid = rid};
    }
    const u32 rid = (u32)(r - g_uma.rg);
    if ((cpu - r->cpu) % (align ? align : 1)) {
        g_uma.fast_miss[UMA_FAST_ALIGN]++;
        return 0;
    }
    if (!rgn_bound(r, rid)) { /* no Imp or Chunk holds it: going */
        g_uma.fast_miss[UMA_FAST_DEAD]++;
        return 0;
    }
    g_uma.fast_hits++;
    read_in(r, cpu, len, 1, buf, off);
    return 1;
}
int vk_uma_read_cpu(u64 cpu, u64 len, u64 align, int ordered, VkBuffer* buf, u64* off) {
    if (!len || atomic_load_explicit(&g_uma.any_dead, memory_order_acquire)) return 0;
    const u32 rid = rid_at(cpu);
    Rgn* r = rid ? &g_uma.rg[rid] : NULL;
    if (!r || cpu + len > r->cpu + r->size || (cpu - r->cpu) % (align ? align : 1) || rg_blk(r, cpu, len) ||
        !rgn_bound(r, rid))
        return 0;
    read_in(r, cpu, len, ordered, buf, off);
    return 1;
}
void vk_uma_fast_counts(u64* hits, u64* blk) {
    *hits = g_uma.fast_hits;
    *blk = g_uma.fast_miss[UMA_FAST_BLK];
}
/* draw_state_check=1: what the old chain's own checks of uma.c say of a range the fast path bound at buf, off: the
   reason it would not be that, NULL when it would */
const char* vk_uma_read_differs(u64 gva, u64 len, VkBuffer buf, u64 off) {
    u64 cpu = 0;
    if (gmmu_run(gva, len, &cpu) < len) return "not one run of guest memory";
    if (write_pages_since(&g_uma.front, 0, cpu, len)) return "a page the runtime wrote";
    u32 i = imp_find(cpu);
    if (i < g_uma.nimp && g_uma.imp[i].cpu <= cpu && cpu + len <= g_uma.imp[i].cpu + g_uma.imp[i].size) {
        pm_lock(&g_uma.lk);
        int dead = g_uma.imp[i].dead;
        pm_unlock(&g_uma.lk);
        if (dead) return "a region whose pages are other pages";
        return g_uma.imp[i].buf == buf && g_uma.imp[i].cpu + off == cpu ? NULL : "not its import's buffer and offset";
    }
    const Chunk* c = g_uma.nchunk ? chunk_of(cpu, len) : NULL;
    if (c) return c->buf == buf && c->cpu + off == cpu ? NULL : "not its chunk's buffer and offset";
    return "no import holds it";
}
void vk_uma_check_diff(const char* what, u64 gva, u64 len) {
    g_uma.check_diffs++;
    WARN_LIMIT(16, "vulkan: fast path: a read of %llx+%llx bound in place over %s\n", (unsigned long long)gva,
               (unsigned long long)len, what);
}
u64 vk_uma_check_diffs(void) { return g_uma.check_diffs; }
/* ------------------------------------------------------------ writes in place */
/* A barrier before what reads [cpu, cpu + len) in place, when it was written in place since the last one, or before
   what writes it, when it was read or written: the console's GPU runs its work in order. Outside a rendering pass,
   which a barrier for these stages can't be recorded in */
static const Out* newest_out(u64 cpu, u64 n);
enum { UMA_ORDER_NOW, UMA_ORDER_BEFORE, UMA_ORDER_OVER }; /* order_n's: what the barrier is for */
static void order(u64 cpu, u64 len, int write) {
    const u64 since = g_uma.barrier_no;
    const int written = st_since(UMA_WSEQ, cpu, len, since);
    const int read = write && st_since(UMA_RSEQ, cpu, len, since);
    if (g_uma.chk) {
        const int was = chk_since(UMA_WSEQ, cpu, len, since) || (write && chk_since(UMA_RSEQ, cpu, len, since));
        if (was != (written || read))
            check_differs(write ? "a write's barrier" : "a read's barrier", cpu, len, written || read, was);
    }
    if (!written && !read) return;
    /* the statistics: a write over a read or a write, else a read of a write still pending in the submission being
       recorded, or of one from an earlier submission (pending or done), which the last barrier came before. The pages
       are what the tables stamp */
    int why = UMA_ORDER_OVER;
    if (written && !write) {
        u64 lo = cpu & ~0xFFFull, hi = (cpu + len + 0xFFF) & ~0xFFFull;
        const Out* o = newest_out(lo, hi - lo);
        why = o && o->sub == vk_submit_number() ? UMA_ORDER_NOW : UMA_ORDER_BEFORE;
    }
    g_uma.order_n[why]++;
    if (vk_render_begun()) g_uma.order_pass[why]++;
    vk_render_end();
    vk_all_barrier(vk_cmd());
    g_uma.barrier_no++;
    g_uma.barriers++;
}
static void note_read(u64 cpu, u64 len) {
    Seen* m = &g_uma.rmemo[(u32)(((cpu ^ (len << 32)) * 0x9E3779B97F4A7C15ull) >> 56)];
    if (m->cpu == cpu && m->len == len && m->sub == g_uma.barrier_no + 1) return;
    *m = (Seen){cpu, len, g_uma.barrier_no + 1};
    st_stamp(UMA_RSEQ, cpu, len, g_uma.barrier_no + 1);
    if (g_uma.chk) write_pages_stamp(&g_uma.chk->read, cpu, len, g_uma.barrier_no + 1);
}
void vk_uma_refused(int why) { g_uma.refused[why]++; }
int vk_uma_write(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off) {
    u64 cpu = 0;
    if (!vk_uma()) return 0;
    if (!len || gmmu_run(gva, len, &cpu) < len) {
        g_uma.refused[VK_UMA_REFUSED_RUN]++;
        return 0;
    }
    for (u32 i = 0; i < g_uma.nprep; i++) /* bound twice in one draw: the same binding */
        if (g_uma.prep[i].gva == gva && g_uma.prep[i].len == len) {
            if (bind(cpu, len, align, buf, off)) return 1;
            g_uma.refused[VK_UMA_REFUSED_REGION]++;
            return 0;
        }
    int why = g_uma.nprep == UMA_PREP_N                      ? VK_UMA_REFUSED_FULL
              : write_pages_since(&g_uma.front, 0, cpu, len) ? VK_UMA_REFUSED_FRONT
              : !bind(cpu, len, align, buf, off)             ? VK_UMA_REFUSED_REGION
                                                             : -1;
    if (why >= 0) {
        g_uma.refused[why]++;
        return 0;
    }
    order(cpu, len, 1);
    g_uma.prep[g_uma.nprep++] = (Out){gva, cpu, len, 0};
    return 1;
}
/* the GPU writes guest memory [cpu, cpu + len) in place in the submit being recorded: pending until it is done */
static void out_add(u64 gva, u64 cpu, u64 len) {
    if (g_uma.nout == g_uma.capout) {
        u32 cap = g_uma.capout ? g_uma.capout * 2 : 256;
        Out* n = realloc(g_uma.out, (size_t)cap * sizeof *n);
        if (!n) fatal("vulkan: out of memory for %u writes of guest memory in place", cap);
        g_uma.out = n;
        g_uma.capout = cap;
    }
    /* pending by page until its submit is done (out_pages, in no other table), and the order's stamp of a write in
       its region's table, or in the table of before */
    pend_add_pages(&g_uma.out_pages, cpu >> 12, (cpu + len - 1) >> 12, 1);
    st_stamp(UMA_WSEQ, cpu, len, g_uma.barrier_no + 1);
    if (g_uma.chk) write_pages_stamp(&g_uma.chk->written, cpu, len, g_uma.barrier_no + 1);
    const u64 sub = vk_submit_number();
    g_uma.out[g_uma.nout++] = (Out){gva, cpu, len, sub, g_uma.writer};
    g_uma.written_bytes += len;
    g_uma.out_n++;
    g_uma.out_npages += ((cpu + len - 1) >> 12) - (cpu >> 12) + 1;
    Seen* seen = &g_uma.out_seen[(u32)(((cpu ^ (len << 40)) * 0x9E3779B97F4A7C15ull) >> 52)];
    if (seen->cpu == cpu && seen->len == len && seen->sub == sub)
        g_uma.out_same++;
    else
        *seen = (Seen){cpu, len, sub};
    atomic_store_explicit(&g_uma_any, 1, memory_order_relaxed);
}
void vk_uma_writes_commit(void) {
    for (u32 i = 0; i < g_uma.nprep; i++) out_add(g_uma.prep[i].gva, g_uma.prep[i].cpu, g_uma.prep[i].len);
    g_uma.nprep = 0;
}
void vk_uma_writes_abort(void) { g_uma.nprep = 0; }
/* The writes in place of the draw being set up so far (vk_uma_prep_mark), and those after the mark undone: a group of
   bindings that went to the atlas after some of its writes were bound in place leaves none of them pending. The
   barriers order recorded for them stay, which only order more */
u32 vk_uma_prep_mark(void) { return g_uma.nprep; }
void vk_uma_writes_rollback(u32 mark) {
    if (mark < g_uma.nprep) g_uma.nprep = mark;
}
void vk_uma_no_wait(int on) { g_uma.no_wait = on; }
u64 vk_uma_regions(void) { return g_uma.regions; }
/* The one region that holds guest memory [cpu, cpu + len), made or grown over the imports there as a lookup makes it,
   but with no wait (no_wait): its buffer. 1 when there is one, 0 when the range is copied, -1 when the region would
   need a wait for the writes in place pending in the imports it takes over or lets go (vk_lock held), with the
   offset of cpu in the buffer. It stamps nothing: each binding's own lookup (vk_uma_write, vk_uma_get) does */
int vk_uma_cover(u64 cpu, u64 len, VkBuffer* buf, u64* off) {
    if (!vk_uma() || !len) return 0;
    drop_dead();
    u64 at = cpu;
    const Chunk* c = g_uma.nchunk ? placed_of(&at, len) : NULL;
    if (c) {
        *buf = c->buf;
        *off = at - c->cpu;
        return 1;
    }
    const int was = g_uma.no_wait;
    const u64 waits = g_uma.would_wait;
    g_uma.no_wait = 1;
    u32 i = imp_get(cpu, len);
    g_uma.no_wait = was;
    if (i == ~0u) return g_uma.would_wait != waits ? -1 : 0;
    *buf = g_uma.imp[i].buf;
    *off = cpu - g_uma.imp[i].cpu;
    return 1;
}
void vk_uma_frame_done(u64 frame) {
    u32 k = 0;
    while (k < g_uma.nout && g_uma.out[k].sub <= frame) {
        const Out* o = &g_uma.out[k++];
        pend_add_pages(&g_uma.out_pages, o->cpu >> 12, (o->cpu + o->len - 1) >> 12, -1);
        gpu_mem_written_cpu(o->cpu, o->len); /* what was built from the bytes (buffers, textures) takes the new ones */
    }
    if (!k) return;
    memmove(g_uma.out, g_uma.out + k, (g_uma.nout - k) * sizeof *g_uma.out);
    g_uma.nout -= k;
    atomic_store_explicit(&g_uma_any, g_uma.nout != 0, memory_order_relaxed);
}
void vk_uma_host_barrier(VkCommandBuffer cb, u64 frame) {
    if (!g_uma.nout || g_uma.out[g_uma.nout - 1].sub != frame) return;
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = vk_buffer_stages(), /* the writes in place of buffers' writers */
                          .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                          .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &d);
}
int vk_uma_pending(u64 gva, u64 len) {
    return atomic_load_explicit(&g_uma_any, memory_order_relaxed) && len && pend_hit(&g_uma.out_pages, gva, len);
}
int vk_uma_pending_cpu(u64 cpu, u64 len) {
    return atomic_load_explicit(&g_uma_any, memory_order_relaxed) && len &&
           pend_hit_pages(&g_uma.out_pages, cpu >> 12, (cpu + len - 1) >> 12);
}
/* in-place writes added so far, for the caches that ask vk_uma_writes */
u64 vk_uma_writes(void) { return atomic_load_explicit(&g_uma.out_pages.adds, memory_order_relaxed); }
u64 vk_uma_out_waits(void) { return g_uma.out_waits; }
void vk_uma_writer(u64 prog) { g_uma.writer = prog; }
/* whether a dispatch of prog goes to the GPU at once: a CPU reader waited for one of its writes while it was still in
   the submission being recorded. A full table answers yes for every program, as before there was one */
int vk_uma_read_soon(u64 prog) {
    if (g_uma.nsoon == UMA_SOON_N) return 1;
    for (u32 i = (u32)(prog % UMA_SOON_N), k = 0; k < UMA_SOON_N; k++, i = (i + 1) % UMA_SOON_N) {
        if (g_uma.soon[i] == prog) return 1;
        if (!g_uma.soon[i]) return 0;
    }
    return 1;
}
static void read_soon_add(u64 prog) {
    if (!prog || vk_uma_read_soon(prog)) return;
    for (u32 i = (u32)(prog % UMA_SOON_N);; i = (i + 1) % UMA_SOON_N)
        if (!g_uma.soon[i]) {
            g_uma.soon[i] = prog;
            g_uma.nsoon++;
            return;
        }
}
/* the newest in-place write over guest memory [cpu, cpu + n), NULL for none */
static const Out* newest_out(u64 cpu, u64 n) {
    for (u32 k = g_uma.nout; k-- > 0;)
        if (g_uma.out[k].cpu < cpu + n && cpu < g_uma.out[k].cpu + g_uma.out[k].len) return &g_uma.out[k];
    return NULL;
}
static const Out* newer_out(const Out* a, const Out* b) { return !b || (a && a->sub >= b->sub) ? a : b; }
/* until the GPU has done o's submit, its writes landed through it: the one being recorded is submitted first, and o's
   program is submitted at once from then on (compute.c), as a reader of its results comes before the stream's end */
static void wait_out_why(const Out* o, int why) {
    if (!o) return;
    const u64 sub = o->sub, t0 = plat_ns();
    atomic_fetch_add_explicit(&g_uma.wait_by[why], 1, memory_order_relaxed);
    g_uma.out_waits++;
    if (sub > vk_last_submit()) {
        g_uma.open_waits++;
        g_uma.wait_open[why]++;
        read_soon_add(o->prog);
        vk_finish();
    } else {
        vk_wait_submit(sub);
        vk_land_through(sub);
    }
    g_uma.wait_ns[why] += plat_ns() - t0;
}
/* The writes in place pending in the regions over guest memory [lo, hi) done, before their pages change or move back:
   a region's buffer is the guest's memory until then, and its readers are waited for by nobody (quiesce), as the buffer
   keeps the bytes they read; a write landing after the change would land in the buffer alone (vk_lock held) */
static const Out* region_writes_newest(u64 lo, u64 hi) {
    const Out* newest = NULL;
    if (!atomic_load_explicit(&g_uma_any, memory_order_relaxed)) return NULL;
    for (u32 i = 0; i < g_uma.nimp; i++) {
        const Imp* m = &g_uma.imp[i];
        if (m->fd < 0 || m->cpu >= hi || lo >= m->cpu + m->size) continue;
        u64 a = m->cpu > lo ? m->cpu : lo, b = m->cpu + m->size < hi ? m->cpu + m->size : hi;
        newest = newer_out(newest, newest_out(a, b - a));
    }
    return newest;
}
static void region_writes_land(u64 lo, u64 hi) { wait_out_why(region_writes_newest(lo, hi), 0); }
/* whether region_writes_land over [lo, hi) would wait */
static int region_writes_wait(u64 lo, u64 hi) { return region_writes_newest(lo, hi) != NULL; }
void vk_uma_mem_need(u64 gva, u64 len) {
    const Out* newest = NULL;
    if (!atomic_load_explicit(&g_uma_any, memory_order_relaxed) || t_uma.gpu_reads || !len) return;
    vk_lock();
    if (vk_uma_pending(gva, len))
        for (u64 at = 0; at < len;) {
            u64 cpu = 0, n = gmmu_run(gva + at, len - at, &cpu);
            if (!n) {
                n = 0x1000 - ((gva + at) & 0xFFF);
                if (n > len - at) n = len - at;
            } else
                newest = newer_out(newest, newest_out(cpu, n));
            at += n;
        }
    if (newest)
        LOG_LIMIT(20, "vulkan: a read of guest %llx + %llx waits for an in-place write of submit %llu (now %llu)\n",
                  (unsigned long long)gva, (unsigned long long)len, (unsigned long long)newest->sub,
                  (unsigned long long)vk_last_submit());
    wait_out_why(newest, 1);
    vk_unlock();
}
void vk_uma_mem_need_cpu(u64 cpu, u64 len) {
    if (!vk_uma_pending_cpu(cpu, len)) return;
    vk_lock();
    wait_out_why(newest_out(cpu, len), 2);
    vk_unlock();
}
void vk_uma_gpu_reads(int on) { t_uma.gpu_reads = on; }
int vk_uma_held(u64 cpu, u64 len) {
    if (!vk_uma() || !len) return 0;
    if (g_uma.nchunk && placed_of(&cpu, len)) return 1;
    drop_dead();
    u32 i = imp_find(cpu);
    return i < g_uma.nimp && g_uma.imp[i].cpu <= cpu && cpu + len <= g_uma.imp[i].cpu + g_uma.imp[i].size;
}
int vk_uma_none_why(u64 cpu, u64 len) {
    if (!vk_uma() || !len) return VK_UMA_NONE_OFF;
    if (vk_uma_held(cpu, len)) return VK_UMA_NONE_HELD;
    /* imp_get's tests, in its order, of the windows the range touches */
    const u64 lo = cpu & ~(UMA_WINDOW - 1), hi = (cpu + len + UMA_WINDOW - 1) & ~(UMA_WINDOW - 1);
    const u64 cap = g_uma.migrate && g_vk.alloc_cap > g_uma.region_max ? g_uma.region_max : g_vk.alloc_cap;
    if ((g_uma.nchunk && chunk_meets(lo, hi)) || (g_uma.nview && view_meets(lo, hi))) return VK_UMA_NONE_PLACED;
    if (own_trapped(lo, hi - lo)) return VK_UMA_NONE_TRAPPED;
    if (!imp_can(lo, hi)) return VK_UMA_NONE_UNMOVABLE;
    if (hi - lo > cap) return VK_UMA_NONE_CAP;
    return failed(lo, hi) ? VK_UMA_NONE_FAILED : VK_UMA_NONE_UNASKED;
}
int vk_uma_publish(u64 cpu, u64 len, VkBuffer src, u64 off) {
    VkBuffer ib;
    u64 io;
    if (!vk_uma_held(cpu, len) || !bind(cpu, len, 1, &ib, &io)) return 0; /* no import made for it */
    vk_render_end();
    VkCommandBuffer cb = vk_cmd();
    vk_all_barrier(cb); /* the producer's writes, and what read or wrote the bytes in place before, first */
    VkBufferCopy c = {off, io, len};
    rec_CmdCopyBuffer(cb, src, ib, 1, &c);
    /* and what comes after it, before it is set up: a draw that reads the result in the next pass reads it there,
       with no barrier of its own to end that pass for (order) */
    vk_all_barrier(cb);
    out_add(0, cpu, len);
    g_uma.barrier_no += 2;
    g_uma.barriers += 2;
    g_uma.published += len;
    return 1;
}
int vk_uma_copy_pending(u64 cpu, u64 len, VkBuffer* buf, u64* off) {
    return vk_uma_pending_cpu(cpu, len) && bind(cpu, len, 1, buf, off);
}
u64 vk_uma_import_part(u64 cpu, u64 len, int* held) {
    *held = 0;
    if (!vk_uma() || !len || g_uma.nchunk) return 0; /* placed memory isn't split here */
    drop_dead();
    u32 i = imp_find(cpu);
    if (i < g_uma.nimp && g_uma.imp[i].cpu <= cpu) {
        u64 end = g_uma.imp[i].cpu + g_uma.imp[i].size;
        *held = 1;
        return end - cpu < len ? end - cpu : len;
    }
    return i < g_uma.nimp && g_uma.imp[i].cpu - cpu < len ? g_uma.imp[i].cpu - cpu : len;
}
/* The GPU writes guest memory [cpu, cpu + len) where it is, from a program of the runtime's own recorded next
   (buffer.c's write-back of a logged result, between barriers of the caller's before and after it, as
   vk_uma_publish's copy): an import holding it, made if there is none and the driver takes the pages, as vk_uma_write
   binds a shader's. Asked twice: a full table lets its least recently used import go and makes none (imp_get), and
   the caller would make the part GPU-owned instead, its pages trapped and in the alias pool for good */
int vk_uma_gpu_write(u64 cpu, u64 len, VkBuffer* buf, u64* off) {
    return vk_uma() && len && (bind(cpu, len, 4, buf, off) || bind(cpu, len, 4, buf, off));
}
/* that write recorded: pending until its submit is done, as a shader's in place (out_add), and the caller's barrier
   after it orders what reads or writes the bytes next (order) */
void vk_uma_gpu_wrote(u64 cpu, u64 len) {
    out_add(0, cpu, len);
    g_uma.barrier_no += 2;
    g_uma.barriers += 2;
}
/* the pages of [cpu, cpu + len) as read in place by the submit being recorded: once a submit for each range, as draws
   bind the same ranges again and again */
static void hz_mark(u64 cpu, u64 len) {
    u32 h = (u32)(((cpu ^ (len << 32)) * 0x9E3779B97F4A7C15ull) >> 56);
    if (g_uma.seen[h].cpu == cpu && g_uma.seen[h].len == len && g_uma.seen[h].sub == g_uma.bound) return;
    g_uma.seen[h].cpu = cpu;
    g_uma.seen[h].len = len;
    g_uma.seen[h].sub = g_uma.bound;
    st_stamp(UMA_LIVE, cpu, len, g_uma.bound);
    if (g_uma.chk) write_pages_stamp(&g_uma.chk->live, cpu, len, g_uma.bound);
}
/* the table of before for a page read in place in no region: live */
static void hz_old(u64 cpu, u64 len) { write_pages_stamp(&g_uma.live, cpu, len, g_uma.bound); }
/* a GPU-side write to guest memory [cpu, cpu + n): counted when a command that is not done yet read a page of it in
   place (any thread). live holds the submit that last read each page in place, and its readers take no lock. The count
   had a table of its own, a page and a submit stored for every page of every range bound: a sixth of the GPU thread
   in one game whose programs bind 16 MiB windows. The driver is asked how far the GPU got only when a page was read
   in place since the last answer */
static void gpu_wrote(u64 cpu, u64 n) {
    u64 done = atomic_load_explicit(&g_uma.hz_done, memory_order_relaxed), dummy;
    if (!n) return;
    /* the LIVE stamps of the regions there, under lk (their tables go only under it), when a region holds a window
       of it or one that went may still. A stamp stored as this reads it may be missed: its submit is one the write
       did not need to wait for */
    int any = atomic_load_explicit(&g_uma.ndrg, memory_order_relaxed) != 0;
    for (u64 w = cpu >> 21, e = (cpu + n - 1) >> 21; w <= e && w < UMA_WINS && !any; w++)
        any = atomic_load_explicit(&g_uma.win[w], memory_order_relaxed) != 0;
    if (any) {
        vk_submit_progress(&dummy, &done);
        pm_lock(&g_uma.lk);
        const int hit = st_since(UMA_LIVE, cpu, n, done);
        pm_unlock(&g_uma.lk);
        if (hit) atomic_fetch_add_explicit(&g_uma.hazards, 1, memory_order_relaxed);
        return;
    }
    if (!atomic_load_explicit(&g_uma.old_any[UMA_LIVE], memory_order_relaxed) ||
        !write_pages_since(&g_uma.live, done, cpu, n))
        return;
    vk_submit_progress(&dummy, &done);
    atomic_store_explicit(&g_uma.hz_done, done, memory_order_relaxed);
    if (write_pages_since(&g_uma.live, done, cpu, n))
        atomic_fetch_add_explicit(&g_uma.hazards, 1, memory_order_relaxed);
}

void vk_uma_init(void) {
    if (!vk_uma()) return;
    /* the device's dma-buf functions, there only when the extensions were enabled (this way, or placed maps' views) */
    if (!vkGetMemoryFdPropertiesKHR) g_uma.can_dmabuf = 0;
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &hp};
    if (g_uma.migrate) { /* regions move whole host pages (mem_migrate) */
        if (plat_vm_page() > g_uma.align) g_uma.align = plat_vm_page();
        LOG("vulkan: guest memory moves into %s as the GPU reads it, in regions of whole %llu MiB windows, %llu MiB "
            "at most\n",
            g_uma.dmabuf ? "memfds the GPU imports as dma-bufs (udmabuf)" : "AHardwareBuffers",
            (unsigned long long)(UMA_WINDOW >> 20), (unsigned long long)(g_uma.region_max >> 20));
    } else {
        vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
        if (hp.minImportedHostPointerAlignment > g_uma.align) g_uma.align = hp.minImportedHostPointerAlignment;
    }
    if (g_uma.align > UMA_WINDOW) {
        WARN("vulkan: imports must be aligned to %llu KiB: guest memory is copied\n",
             (unsigned long long)(g_uma.align >> 10));
        g_vk.mem_path = VK_MEM_PATH_COPY;
        return;
    }
    mem_set_pages_replaced(pages_replaced);
    gpu_set_write_watch(gpu_wrote);
    /* the page model (above Rgn): in-place writes stay out of g_pend_any.adds, and g_pend_blk gives the blk
       bits; draw_state_check=1 keeps the tables of before beside it and compares their answers */
    g_uma.out_pages.adds_alone = 1;
    pend_set_any_watch(pend_watch);
    if (g_settings.draw_state_check && !g_uma.chk && !(g_uma.chk = calloc(1, sizeof *g_uma.chk)))
        fatal("vulkan: out of memory for the tables the page model is checked against");
    LOG("vulkan: guest memory in place: a table of pages for each region, reads bound with no other check%s\n",
        g_uma.chk ? ", the tables of before kept to compare (draw_state_check=1)" : "");
    if (g_uma.can_place) {
        /* the chunks' memory: host-visible and coherent (the guest's CPU and the GPU see each other's writes with no
           flush), cached where a type is */
        const VkPhysicalDeviceMemoryProperties* mp;
        u32 want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, type = ~0u;
        mp = vk_devmem_props();
        /* not AMD's device-coherent or uncached types, which want a feature of their own, nor protected memory */
        u32 skip = VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD | VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD |
                   VK_MEMORY_PROPERTY_PROTECTED_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
        for (u32 t = 0; t < mp->memoryTypeCount; t++) {
            VkMemoryPropertyFlags f = mp->memoryTypes[t].propertyFlags;
            if ((f & want) == want && !(f & skip) &&
                (type == ~0u || ((f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) &&
                                 !(mp->memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))))
                type = t;
        }
        g_uma.can_place = type != ~0u;
        g_uma.place_type = type;
        if (g_uma.can_place) mem_set_placed(&g_placed_hooks);
        LOG("vulkan: placed maps: %s\n", g_uma.can_place
                                             ? (mp->memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT
                                                    ? "memory type cached and coherent"
                                                    : "memory type coherent, not cached")
                                             : "no memory type the CPU maps coherent: none");
    }
}
/* each present (vk_lock held): imports unused for long go, and every 600 presents the statistics */
void vk_uma_frame(void) {
    if (!vk_uma()) return;
    drop_dead();
    gone_reap();
    chunk_reap();
    rgn_reap();
    if (g_vk_present % 60 == 0)
        for (u32 i = g_uma.nimp; i-- > 0;)
            if ((u32)(g_vk_present - g_uma.imp[i].used) > UMA_IDLE) imp_let_go(i);
    if (g_vk_present % 600) return;
    u64 bytes = 0;
    for (u32 i = 0; i < g_uma.nimp; i++) bytes += g_uma.imp[i].size;
    LOG("vulkan: in place over 600 presents: %llu ranges (%llu MB bound) read in guest memory, %llu copied; %llu "
        "imports made (%llu MB, %llu ms), %llu spans refused, %llu dropped; %u imports hold %llu MB; %llu hazards (a "
        "GPU-side write on a page that unfinished work reads in place); %llu waits for that work before a trap or a "
        "remap, %llu before a write of the runtime's; %llu MB written in place (%llu MB of them engines' results), "
        "%llu barriers for it, %llu waits for it; %u chunks GPU memory where they are (%llu made, %llu given back); "
        "%llu writes, %llu the same range as one of the same submission, %llu pages counted\n"
        "vulkan:   those waits by who asked: %llu region, %llu mem-need, %llu mem-need-cpu, %llu front-write\n",
        (unsigned long long)g_uma.hits, (unsigned long long)(g_uma.hit_bytes >> 20), (unsigned long long)g_uma.misses,
        (unsigned long long)g_uma.imports, (unsigned long long)(g_uma.import_bytes >> 20),
        (unsigned long long)(g_uma.import_ns / 1000000), (unsigned long long)g_uma.failed,
        (unsigned long long)g_uma.dropped, g_uma.nimp, (unsigned long long)(bytes >> 20),
        (unsigned long long)atomic_load_explicit(&g_uma.hazards, memory_order_relaxed),
        (unsigned long long)g_uma.quiesces, (unsigned long long)g_uma.waits,
        (unsigned long long)(g_uma.written_bytes >> 20), (unsigned long long)(g_uma.published >> 20),
        (unsigned long long)g_uma.barriers, (unsigned long long)g_uma.out_waits, g_uma.nchunk,
        (unsigned long long)g_uma.chunks_made, (unsigned long long)g_uma.chunks_gone, (unsigned long long)g_uma.out_n,
        (unsigned long long)g_uma.out_same, (unsigned long long)g_uma.out_npages,
        (unsigned long long)atomic_load_explicit(&g_uma.wait_by[0], memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&g_uma.wait_by[1], memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&g_uma.wait_by[2], memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&g_uma.wait_by[3], memory_order_relaxed));
    LOG("vulkan:   imports declined: %llu chunked, %llu unmade over imports, %llu unmapped, %llu over the cap, %llu "
        "refused before, %llu table full\n",
        (unsigned long long)g_uma.imp_why[0], (unsigned long long)g_uma.imp_why[1],
        (unsigned long long)g_uma.imp_why[2], (unsigned long long)g_uma.imp_why[3],
        (unsigned long long)g_uma.imp_why[4], (unsigned long long)g_uma.imp_why[5]);
    memset(g_uma.imp_why, 0, sizeof g_uma.imp_why);
    LOG("vulkan: dispatches left for the stream's end over 600 presents: %llu; waits for writes in place still "
        "unsubmitted: %llu; programs whose dispatches go at once: %u\n",
        (unsigned long long)g_vk_cs_left, (unsigned long long)g_uma.open_waits, g_uma.nsoon);
    /* what tells why reading in place costs more than copying (uma_copies): the barriers order records, before a read
       of a page written in place in the submission being recorded or in an earlier one, or before a write; those that
       ended a rendering pass; the writes in place refused, by why; the regions merged and the waits for their writes
       in place, each a finish when the write was not submitted yet */
    LOG("vulkan:   order of the reads and writes in place, a present: %.2f barriers before a read of this "
        "submission's write (%.2f ended a pass), %.2f of an earlier submission's (%.2f), %.2f before a write over a "
        "read or write (%.2f); writes in place refused over 600 presents: %llu a result pending, %llu owned, %llu "
        "compute pending, %llu storage image, %llu release, %llu not one run, %llu draw full, %llu runtime-written "
        "(front), %llu no import; %llu regions merged (%llu taken over); waits for writes in place: %llu region "
        "(%.1f ms, %llu finishes), %llu mem-need (%.1f ms, %llu), %llu mem-need-cpu (%.1f ms, %llu), %llu "
        "front-write (%.1f ms, %llu)\n",
        (double)g_uma.order_n[UMA_ORDER_NOW] / 600, (double)g_uma.order_pass[UMA_ORDER_NOW] / 600,
        (double)g_uma.order_n[UMA_ORDER_BEFORE] / 600, (double)g_uma.order_pass[UMA_ORDER_BEFORE] / 600,
        (double)g_uma.order_n[UMA_ORDER_OVER] / 600, (double)g_uma.order_pass[UMA_ORDER_OVER] / 600,
        (unsigned long long)g_uma.refused[VK_UMA_REFUSED_RESULT],
        (unsigned long long)g_uma.refused[VK_UMA_REFUSED_OWNED],
        (unsigned long long)g_uma.refused[VK_UMA_REFUSED_COMPUTE],
        (unsigned long long)g_uma.refused[VK_UMA_REFUSED_IMAGE],
        (unsigned long long)g_uma.refused[VK_UMA_REFUSED_RELEASE],
        (unsigned long long)g_uma.refused[VK_UMA_REFUSED_RUN], (unsigned long long)g_uma.refused[VK_UMA_REFUSED_FULL],
        (unsigned long long)g_uma.refused[VK_UMA_REFUSED_FRONT],
        (unsigned long long)g_uma.refused[VK_UMA_REFUSED_REGION], (unsigned long long)g_uma.merges,
        (unsigned long long)g_uma.merged,
        (unsigned long long)atomic_load_explicit(&g_uma.wait_by[0], memory_order_relaxed),
        (double)g_uma.wait_ns[0] / 1e6, (unsigned long long)g_uma.wait_open[0],
        (unsigned long long)atomic_load_explicit(&g_uma.wait_by[1], memory_order_relaxed),
        (double)g_uma.wait_ns[1] / 1e6, (unsigned long long)g_uma.wait_open[1],
        (unsigned long long)atomic_load_explicit(&g_uma.wait_by[2], memory_order_relaxed),
        (double)g_uma.wait_ns[2] / 1e6, (unsigned long long)g_uma.wait_open[2],
        (unsigned long long)atomic_load_explicit(&g_uma.wait_by[3], memory_order_relaxed),
        (double)g_uma.wait_ns[3] / 1e6, (unsigned long long)g_uma.wait_open[3]);
    /* the page model: the reads the fast path bound (vk_uma_read) and why the others took the old chain, the
       tables, and the cross-check's differences (draw_state_check=1, here and in buffer.c's) */
    u32 live = 0, kept = 0;
    u64 words = 0;
    for (u32 i = 1; i <= g_uma.nrg; i++)
        if (g_uma.rg[i].size) {
            live += g_uma.rg[i].live != 0;
            kept += !g_uma.rg[i].live;
            words += rg_table_words(g_uma.rg[i].size);
        }
    LOG("vulkan:   page model over 600 presents: %llu reads bound in place with no other check, %llu took the "
        "old chain (%llu a region going, %llu not one run, %llu in no region, %llu across a region's edge, %llu "
        "blk, %llu off the alignment); %u regions with tables, %u that went kept, %llu KB of tables; %llu "
        "differences of the cross-check since the start\n",
        (unsigned long long)g_uma.fast_hits,
        (unsigned long long)(g_uma.fast_miss[UMA_FAST_DEAD] + g_uma.fast_miss[UMA_FAST_RUN] +
                             g_uma.fast_miss[UMA_FAST_NONE] + g_uma.fast_miss[UMA_FAST_EDGE] +
                             g_uma.fast_miss[UMA_FAST_BLK] + g_uma.fast_miss[UMA_FAST_ALIGN]),
        (unsigned long long)g_uma.fast_miss[UMA_FAST_DEAD], (unsigned long long)g_uma.fast_miss[UMA_FAST_RUN],
        (unsigned long long)g_uma.fast_miss[UMA_FAST_NONE], (unsigned long long)g_uma.fast_miss[UMA_FAST_EDGE],
        (unsigned long long)g_uma.fast_miss[UMA_FAST_BLK], (unsigned long long)g_uma.fast_miss[UMA_FAST_ALIGN], live,
        kept, (unsigned long long)(words * 8 >> 10), (unsigned long long)g_uma.check_diffs);
    g_uma.fast_hits = 0;
    memset(g_uma.fast_miss, 0, sizeof g_uma.fast_miss);
    memset(g_uma.order_n, 0, sizeof g_uma.order_n);
    memset(g_uma.order_pass, 0, sizeof g_uma.order_pass);
    memset(g_uma.refused, 0, sizeof g_uma.refused);
    memset(g_uma.wait_ns, 0, sizeof g_uma.wait_ns);
    memset(g_uma.wait_open, 0, sizeof g_uma.wait_open);
    g_uma.merges = g_uma.merged = 0;
    g_vk_cs_left = g_uma.open_waits = 0;
    g_uma.hits = g_uma.hit_bytes = g_uma.misses = g_uma.imports = g_uma.import_bytes = g_uma.import_ns = g_uma.failed =
        g_uma.dropped = g_uma.quiesces = g_uma.waits = g_uma.written_bytes = g_uma.barriers = g_uma.out_waits =
            g_uma.published = g_uma.chunks_made = g_uma.chunks_gone = g_uma.out_n = g_uma.out_same =
                g_uma.out_npages = 0;
    memset(g_uma.wait_by, 0, sizeof g_uma.wait_by);
    atomic_store_explicit(&g_uma.hazards, 0, memory_order_relaxed);
}
/* Guest memory [cpu, cpu + size) (size ~0: all of it) is about to be trapped (a GPU result published over it,
   memory/own.c) or to get other pages (mem.c): the commands that read an import over it in place run first, submitted
   if they are still recorded (vk_lock held, mem.c's lock not). A device whose GPU is the CPU (lavapipe) reads an import
   through the CPU's own mapping, and faulted on the trapped page in a command that ran later: landing the page waited
   for a readback queued behind that command, and edenprobe's dispatch and draw pairs over one buffer hung there.
   amdgpu looks up an import's pages again at every submit and refuses them once they are in the alias pool, whose
   pages are a file's (the trap moves them there): a command recorded before the publish and submitted after it would
   be refused. The pages the command reads are the ones they were when it was recorded, as the console's GPU reads them.
   1 when an import is over it. Not for a region, whose buffer the change doesn't touch */
static int quiesce(u64 cpu, u64 size) {
    u64 end = size == ~0ull || cpu + size < cpu ? ~0ull : cpu + size, sub = 0, submitted, done;
    int any = 0;
    for (u32 i = 0; i < g_uma.nimp; i++)
        if (g_uma.imp[i].fd < 0 && g_uma.imp[i].cpu < end && cpu < g_uma.imp[i].cpu + g_uma.imp[i].size) {
            any = 1;
            if (last_read(&g_uma.imp[i]) > sub) sub = last_read(&g_uma.imp[i]);
        }
    if (!sub) return any;
    vk_submit_progress(&submitted, &done);
    if (done >= sub) return any;
    read_done(sub);
    g_uma.quiesces++;
    return any;
}
/* Guest memory [cpu, cpu + size) becomes GPU-owned next (vk_own_publish), its pages trapped: the imports over them
   go from the driver first, after their readers ran, as amdgpu fails a later submission (EFAULT) on trapped pages.
   Whole host pages, as own_begin traps them. A region stays: the trap moves its pages into the pool, and it goes
   then (pages_replaced). The pages stay pool pages once the owner lets go, and the next read makes a region of them
   again (mem_migrate takes pool pages that no other address maps) */
void vk_uma_owning(u64 cpu, u64 size) {
    u64 page = plat_vm_page(), lo = cpu & ~(page - 1), hi = (cpu + size + page - 1) & ~(page - 1);
    if (!vk_uma()) return;
    gone_kill(lo, hi);
    region_writes_land(lo, hi);
    if (!quiesce(lo, hi - lo)) return;
    for (u32 i = g_uma.nimp; i-- > 0;)
        if (g_uma.imp[i].fd < 0 && g_uma.imp[i].cpu < hi && lo < g_uma.imp[i].cpu + g_uma.imp[i].size) imp_kill(i, 0);
}
/* mem.c changes guest memory [cpu, cpu + size) (size ~0: all of it) next, maybe its host pages (vk_lock held until
   vk_uma_mapping_end): with an import over it, its readers run, and every submission made so far is in the driver,
   not only in the recording thread's queue (vk_rec_sync), as a submission's ioctl takes the import's pages */
void vk_uma_mapping_begin(u64 cpu, u64 size) {
    u64 end = size == ~0ull || cpu + size < cpu ? ~0ull : cpu + size;
    if (!vk_uma()) return;
    gone_kill(cpu, end);
    region_writes_land(cpu, end);
    if (quiesce(cpu, size)) vk_rec_sync();
}
/* they changed: the imports over pages that are other pages now go before anything else is submitted */
void vk_uma_mapping_end(void) {
    if (vk_uma()) drop_dead();
}
u64 vk_uma_import_in(u64 cpu, u64 end, u64* hi) {
    u64 lo = end;
    *hi = end;
    if (!vk_uma() || cpu >= end) return end;
    pm_lock(&g_uma.lk);
    /* the first import a scan leaves alone: not a dma-buf region, whose pages the driver doesn't take again at each
       submission (an ordinary shared mapping of a memfd, which a scan protects and watches, as wp.c's probe found on
       kernel 7.2: the region's pages are tracked as the rest of guest memory) */
    u32 i = imp_find(cpu);
    while (i < g_uma.nimp && g_uma.imp[i].cpu < end && g_uma.imp[i].tracked) i++;
    if (i < g_uma.nimp && g_uma.imp[i].cpu < end) {
        lo = g_uma.imp[i].cpu > cpu ? g_uma.imp[i].cpu : cpu;
        *hi = g_uma.imp[i].cpu + g_uma.imp[i].size;
    }
    i = chunk_find(cpu); /* GPU memory where it is, and its views: no scan watches them either */
    if (i < g_uma.nchunk && g_uma.chunk[i].cpu < end && (g_uma.chunk[i].cpu > cpu ? g_uma.chunk[i].cpu : cpu) < lo) {
        lo = g_uma.chunk[i].cpu > cpu ? g_uma.chunk[i].cpu : cpu;
        *hi = g_uma.chunk[i].cpu + MEM_PLACED;
    }
    i = view_find(cpu);
    if (i < g_uma.nview && g_uma.view[i].dst < end && (g_uma.view[i].dst > cpu ? g_uma.view[i].dst : cpu) < lo) {
        lo = g_uma.view[i].dst > cpu ? g_uma.view[i].dst : cpu;
        *hi = g_uma.view[i].dst + g_uma.view[i].size;
    }
    for (u32 k = 0; k < g_uma.ngone; k++) {
        const Imp* g = &g_uma.gone[k];
        u64 at = g->cpu > cpu ? g->cpu : cpu;
        if (!g->tracked && g->cpu < end && cpu < g->cpu + g->size && at < lo) {
            lo = at;
            *hi = g->cpu + g->size;
        }
    }
    pm_unlock(&g_uma.lk);
    if (*hi > end) *hi = end;
    return lo;
}
u32 vk_uma_imports(u64 cpu, u64 size) {
    u32 n = 0;
    for (u32 i = 0; i < g_uma.nimp; i++)
        n += g_uma.imp[i].cpu < cpu + size && cpu < g_uma.imp[i].cpu + g_uma.imp[i].size;
    for (u32 i = 0; i < g_uma.ngone; i++)
        n += g_uma.gone[i].cpu < cpu + size && cpu < g_uma.gone[i].cpu + g_uma.gone[i].size;
    return n;
}
/* A draw or dispatch begins, before its setup (vk_frame_check): the uses of what the ones before it looked up are all
   recorded, in submits up to ended, the one being recorded when they were done (vk_lock held). Only when they
   submitted is a lookup's own submit not its last reader */
void vk_uma_settle(u64 ended) {
    if (!vk_uma()) return;
    if (ended > g_uma.seg) {
        u64 submitted, done;
        vk_submit_progress(&submitted, &done);
        if (g_uma.lag_hi <= done) g_uma.lag_lo = g_uma.seg; /* the pages stamped since were read by ended at last */
        g_uma.lag_hi = ended;
        for (u32 i = 0; i < g_uma.nimp; i++)
            if (g_uma.imp[i].sub >= g_uma.seg && g_uma.imp[i].sub < ended) g_uma.imp[i].sub = ended;
        pm_lock(&g_uma.lk);
        for (u32 i = 0; i < g_uma.ngone; i++)
            if (g_uma.gone[i].sub >= g_uma.seg && g_uma.gone[i].sub < ended) g_uma.gone[i].sub = ended;
        pm_unlock(&g_uma.lk);
    }
    g_uma.seg = vk_submit_number();
}
/* Guest memory [cpu, cpu + n) is written next by the runtime: the submits that read it in place are done first, a
   lookup's use recorded in a later submit than its own included (last_read), and it is copied from then on */
/* whether a page of guest memory [cpu, cpu + n) was read in place by a submit after x */
static int live_since(u64 cpu, u64 n, u64 x) {
    const int now = st_since(UMA_LIVE, cpu, n, x);
    if (g_uma.chk) {
        const int was = write_pages_since(&g_uma.chk->live, x, cpu, n) != 0;
        if (was != now) check_differs("a read in place since", cpu, n, now, was);
    }
    return now;
}
static int front_piece(void* ctx, u32 rid, u64 at, u64 n) { /* front_write's, in a region's table */
    (void)ctx;
    if (rid) rg_front_run(&g_uma.rg[rid], at, n);
    return 0;
}
static void front_write(u64 cpu, u64 n) {
    u64 submitted, done, wait = 0;
    write_pages_stamp(&g_uma.front, cpu, n, 1);
    st_pieces(cpu, n, front_piece, NULL); /* blk in the regions there: the fast path takes it no more */
    if (vk_uma_pending_cpu(cpu, n)) wait_out_why(newest_out(cpu, n), 3); /* the GPU's bytes first, then the runtime's */
    vk_submit_progress(&submitted, &done);
    if (live_since(cpu, n, g_uma.seg ? g_uma.seg - 1 : 0))
        wait = vk_submit_number(); /* looked up since the draw being set up began */
    else if (g_uma.lag_hi > done && live_since(cpu, n, g_uma.lag_lo ? g_uma.lag_lo - 1 : 0))
        wait = g_uma.lag_hi;
    else if (live_since(cpu, n, done))
        wait = vk_last_submit();
    if (!wait) return;
    g_uma.waits++;
    read_done(wait);
}
void vk_uma_before_write(u64 gva, u64 len) {
    if (!vk_uma()) return;
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
/* a save state's load: every import goes */
void vk_uma_forget_all(void) {
    for (u32 i = g_uma.nimp; i-- > 0;) imp_let_go(i);
}
int vk_uma_can_dmabuf(void) { return vk_uma() && g_uma.can_dmabuf; }
int vk_uma_migrate(int on) {
    if (!vk_uma() || (on == 2 && !g_uma.can_dmabuf)) return 0;
    for (u32 i = g_uma.nimp; i-- > 0;) imp_let_go(i); /* the other way's go first */
    g_uma.migrate = on != 0;
    g_uma.dmabuf = on == 2;
    g_uma.region_max = on == 2 && g_uma.dmabuf_max < UMA_REGION_MAX ? g_uma.dmabuf_max : UMA_REGION_MAX;
    if (on && plat_vm_page() > g_uma.align) g_uma.align = plat_vm_page();
    return 1;
}
