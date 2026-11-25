/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The guest's Vulkan functions (gvk.h), each in the slot of the game's own: the game's arguments in x0 to x7 and on
   its stack (AAPCS64), its handles the host's, on the renderer's device and graphics queue.

   - The instance, the physical device, the device and the queue are the renderer's. Creating one hands the game the
     renderer's, and destroying it does nothing. The features the game is told of are those the renderer's device has
     on.
   - Memory: the game sees one heap with three types. Type 0 is device-local memory alone. Types 1 and 2 are memory
     the CPU maps, at guest addresses in the alias region, so a mapping is the guest's own address. The host imports
     the guest pages as host memory (VK_EXT_external_memory_host), or where it can't (RenderDoc takes no host memory)
     its own memory is exported as a dma-buf and mapped over the guest pages. The game's memoryTypeBits are the host's
     translated to these three.
   - The window: the surface is the vi layer's, and the swapchain's images are the host's own, which a present blits
     into the renderer's window (renderer.h present_image). An acquire waits for the game's pace (present_pace), as vi's
     dequeue does, and for the display to let the image go, as the console's buffer queue does.
   - Descriptor sets: where the host has host-only pools, each set has a twin in host memory that copies read
     (GvkTwinSet). */
#include "nx/gpu/gvk/gvk.h"
#include "common/utilities/bits.h"
#include "nx/gpu/gvk/gvk_query.h"
#include "nx/gpu/gvk/gvk_rect.h"
#include "nx/gpu/renderer.h"
#include "nx/gpu/vk/ledger.h"
#include "nx/gpu/vk/vk.h"
#include "nx/nn/hook.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/hle/services/vi.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define GVK_MEMS       (1u << 16) /* the memory allocations the game holds at once, at most */
#define GVK_IMAGES     (1u << 16) /* the images it holds at once that can be bound to guest memory, at most */
#define GVK_RPASSES    (1u << 12) /* its render passes */
#define GVK_CBS        (1u << 12) /* its command buffers inside a render pass at once */
#define GVK_SCALED     (1u << 14) /* its images, views and framebuffers drawn at the resolution scale */
#define GVK_DUALS      (1u << 14) /* its fragment modules with a second color at Location 1 */
#define GVK_SWAPCHAINS 4
#define GVK_POOLS      (1u << 12) /* its descriptor pools with a twin */
#define GVK_SC_IMAGES  4
#define GVK_SURFACE    0x5653000000000001ull /* the vi layer's surface: the game's only one */
#define GVK_GUEST_API  VK_API_VERSION_1_1    /* what the game is told: its loader has Vulkan 1.1's functions */
#define GVK_HEAP       (4ull << 30)          /* the one heap the game is told of: the console's memory */
#define GVK_SC_MAX     1920u                 /* the widest swapchain the game is told of (maxImageExtent) */
#define GVK_SC_MAX_H   1080u                 /* and the highest */
#define GVK_TEMPLATES  (1u << 10)            /* its descriptor update templates */
#define GVK_QUERIES    (1u << 10)            /* its pools of pipeline statistics */

typedef struct {
    u64 mem;  /* the host's VkDeviceMemory, 0 for a free slot */
    u64 va;   /* where the game maps it, 0 for device-local memory */
    u64 size; /* the guest pages at va */
    u64 allocation_size;
} GvkMem;

typedef struct {
    u64 img;
    VkFormat format;
    VkSampleCountFlagBits samples;
    VkFormatFeatureFlags features;
    VkImageAspectFlags preinit;
    int importable;
    VkFormat game_depth; /* the 24-bit depth format the game made it with, held as a 32-bit one (host_format), else 0 */
} GvkImage;

/* The render pass, its resume twin and the self-dependencies supported by Vulkan inside it. */
typedef struct {
    u64 rp, resume;
    u32 subpasses, nself;
    VkSubpassDependency* self;
} GvkRpass;

/* a command buffer inside a render pass: what it began, and whether its framebuffer is drawn at the resolution scale
   (area: the host's, scaled with it) */
typedef struct {
    u64 cb, rp, fb;
    VkRect2D area;
    u32 subpass;
    VkSubpassContents contents;
    u32 scaled;
} GvkPass;

/* With the resolution scale: a 2D image of optimal tiling the game made, which can become a target. Its pixels are
   drawn at scale, 1 until it first is a framebuffer's attachment: then gvk draws them in shadow, an image of its own at
   the scale, and each command, view and descriptor of the game's image takes the shadow (fb_scale). The game's image
   stays behind it with its memory. A swapchain image is made at the scale itself, with no shadow */
typedef struct {
    u64 img;
    VkImage shadow;
    u32 w, h;        /* as the game made it */
    VkFormat format; /* the host's */
    VkImageUsageFlags usage;
    VkImageCreateFlags flags;
    DevMem shadow_mem;
    u16 layers;
    u8 mips, samples;
    u8 scale;
    u8 written; /* a submitted command wrote the game's image before its shadow: the shadow starts with its pixels */
    /* why it can't get a shadow now: the command buffers recorded with a command of the game's image that can still
       run it (GvkCbWrites), and a view of it in a descriptor set, which would read the game's image. blocked: for good,
       as a framebuffer at the game's size takes it, or a command gvk couldn't follow used it (a secondary command
       buffer's, or one of too many images in a command buffer) */
    u16 pending;
    u8 in_set, blocked;
    u32 wgen; /* a count of the writes recorded: a shadow made while one is recorded misses it (shadows_publish) */
} GvkTarget;
/* a view of a GvkTarget, and the same view of its shadow once it has one. in_set: it was written to a descriptor set
   before then */
typedef struct {
    u64 view, img;
    VkImageView shadow;
    VkImageViewType type;
    VkFormat format;
    VkImageUsageFlags usage; /* the view's own (VkImageViewUsageCreateInfo), 0 for the image's */
    u8 swizzle[4];
    u16 aspects;
    u8 base_level, levels;  /* 0xff: the rest */
    u16 base_layer, layers; /* 0xffff: the rest */
    u8 in_set;
} GvkView;
/* the targets without a shadow that a command buffer of the game used (host_image): they get none while it can run */
#define GVK_CB_WRITES 7
typedef struct {
    u64 cb, img[GVK_CB_WRITES];
} GvkCbWrites;
/* A framebuffer at the game's size that takes targets with a shadow (fb_scale): it draws their game's images, which
   the other commands do not read. Its passes blit each shadow down into the game's image before they begin, and the
   game's image back up when they end (mixed_sync) */
typedef struct {
    u64 fb, img[GVK_CB_WRITES];
} GvkMixed;
/* a descriptor update template's entries, which its updates read (gvk_UpdateDescriptorSetWithTemplate) */
typedef struct {
    u64 tmpl;
    u32 n;
    VkDescriptorUpdateTemplateEntry* e;
} GvkTemplate;
/* With the resolution scale, the image descriptors of a set that hold a view of an image drawn at the scale: bit
   binding * 8 + element, for bindings and elements below 8 (set_bit). A set with none has no entry. pool: its pool's,
   which counts its sets with a mask (GvkPoolMasks), to forget them as it resets */
typedef struct {
    u64 set, pool, mask;
} GvkSetScale;
typedef struct {
    u64 pool;
    u32 n;
} GvkPoolMasks;
/* a fragment module whose size queries gvk divides (gvk_spirv_scaled_size), by its handle, and a graphics pipeline of
   one, by its handle, with the layout that takes its push constants and the stages they are pushed for */
typedef struct {
    u64 key;
    union {
        u64 layout; /* psizes: the layout that takes its push constants */
        u64 orig;   /* msizes: unrewritten module for a layout with no push-constant room */
    };
    u32 stages;
    u16 slot[GVK_SIZE_SLOTS];
    u8 n;
} GvkSizes;
/* a pipeline layout with gvk's push constants past GVK_SIZE_PUSH: the stages of the fragment stage's range, which
   holds them, and where that range starts. Its pipelines with size queries push through it (sizes_push): the game's
   vkDestroyPipelineLayout only marks it gone while they last */
typedef struct {
    u64 layout;
    u32 stages, from, pipes;
    u8 gone;
} GvkLayout;
/* the descriptors of a set layout's bindings 0 to 7, and a set of one with its pool and those counts, which it keeps
   when the game destroys the layout: where a write or a copy of more descriptors than a binding holds goes on in the
   next (slot_bit) */
typedef struct {
    u64 layout;
    u32 count[8];
} GvkDsLayout;
typedef struct {
    u64 set, pool;
    u32 count[8];
    u8 known;
} GvkSetInfo;
/* a command buffer's masks of its bound graphics sets 0 to 3, its graphics pipeline, and the size bits it pushed last,
   with the layout it pushed them with (0: to push at the next draw) */
typedef struct {
    u64 cb, mask[4], pipe, pushed_layout;
    u32 pushed;
} GvkBound;

/* a command buffer's first viewport and scissor as the game set them, and the scale each was set at: a render pass
   at another scale sets them again at its own */
typedef struct {
    u64 cb;
    VkViewport vp;
    VkRect2D sc;
    u32 has_vp, has_sc, vp_at, sc_at;
} GvkDyn;

typedef struct {
    u64 cb, pool;
    VkCommandBufferLevel level;
    u32 inherited_scale;
    u32 inherited_subpass; /* with inherited_rp: the subpass a secondary continues */
    u64 inherited_rp;      /* the render pass a secondary continues (RENDER_PASS_CONTINUE), 0 for none */
    u32 dynamic;           /* flags of the bound graphics pipeline */
    u8 writes; /* bit k: a command recorded since the last begin writes GvkCbWrites.img[k] (cb_submitted_nl) */
} GvkCmd;
typedef struct {
    u64 pipeline;
    u32 dynamic;
} GvkGraphics;
enum { DYN_VIEWPORT = 1, DYN_SCISSOR = 2 };

typedef struct {
    u32 n, next;
    u32 acquired; /* a bit for each image the game holds: acquired and not presented yet */
    u32 reserved;
    int retired, destroying;
    VkFormat fmt;
    u32 w, h, scale;
    i32 crop[4];
    VkImage img[GVK_SC_IMAGES];
    VkDeviceMemory mem[GVK_SC_IMAGES];
    u64 shown[GVK_SC_IMAGES]; /* the display's number for each image's last present (vk_display_frames), 0 none */
} GvkSwapchain;

/* a fragment module of the game, and its twin with the second color where dual-source blending reads it
   (gvk_spirv_dual_source): what a pipeline that blends with it takes */
typedef struct {
    u64 module, dual;
} GvkDual;
/* a pool of pipeline statistics, with the values of each of its queries */
typedef struct {
    u64 pool;
    u32 values;
} GvkQuery;

/* Objects the guest has released, in queue completion order. Keep imported pages mapped until the host memory goes.
   An image with dm is a shadow (GvkTarget), its memory the renderer's */
typedef struct {
    u64 gate;
    VkImage image;
    DevMem dm;
    GvkMem memory;
} GvkRetired;

typedef struct {
    u64 id, fn, user;
    VkDebugReportFlagsEXT flags;
} GvkDebugReport;

/* RADV keeps a pool's descriptors in device memory that the CPU writes through write-combining, so a copy out of a set
   reads uncached memory: one game's 1164 copies a frame took 2.3 ms there. With host-only pools (g_vk.gvk_host_sets)
   each pool of the game has a twin in host memory, and each set a twin in it. A write goes to both, and a copy reads
   its source's twin */
/* a set's twin, in g_gvk_api.tsets by index (0 none): in its bucket's chain, and in its pool's list through prev and
   next (a free slot's next is the next free one) */
typedef struct {
    u64 set, pool;
    VkDescriptorSet twin;
    u32 chain, prev, next;
} GvkTwinSet;
typedef struct {
    u64 pool;
    VkDescriptorPool twin;
    u32 sets; /* the first of its sets with a twin */
} GvkPoolTwin;

static struct {
    PMutex lock;
    PCond sc_available;
    _Atomic int ready;           /* the memory types are chosen */
    u32 local, coherent, cached; /* the host's memory types behind the game's 0, 1 and 2 */
    u32 ncmd, npool, ngraphics;
    GvkCmd cmd[GVK_CBS];
    u64 pool[GVK_CBS];
    GvkGraphics graphics[GVK_SCALED];
    u64 import_align;                               /* minImportedHostPointerAlignment, or a host page for dma-bufs */
    VkExternalMemoryHandleTypeFlagBits handle_type; /* how the host imports guest pages: host memory or a dma-buf */
    u32 nmem, nimage, nrpass, npass, ntarget, nview, nsfb, ndyn, ndual, nsetscale, nmsizes, npsizes, nbound, nlayout,
        ncbw, ndsl, nsetinfo, npoolmasks, nmixed, ntmpl, nquery;
    GvkMem mem[GVK_MEMS]; /* open addressing by handle */
    GvkImage image[GVK_IMAGES];
    GvkRpass rpass[GVK_RPASSES];
    GvkPass pass[GVK_CBS];
    GvkTarget target[GVK_IMAGES];
    GvkView view[GVK_IMAGES];
    u64 sfb[GVK_SCALED];
    GvkMixed mixed[GVK_SCALED];
    GvkTemplate tmpl[GVK_TEMPLATES];
    GvkQuery query[GVK_QUERIES];
    GvkDyn dyn[GVK_CBS];
    GvkDual dual[GVK_DUALS];
    GvkSetScale setscale[GVK_IMAGES / 4];
    GvkSizes msizes[GVK_DUALS], psizes[GVK_SCALED];
    GvkLayout layout[GVK_SCALED];
    GvkBound bound[GVK_CBS];
    GvkCbWrites cbw[GVK_CBS];
    GvkDsLayout dsl[GVK_SCALED];
    GvkSetInfo setinfo[GVK_IMAGES];
    GvkPoolMasks poolmasks[GVK_POOLS];
    GvkSwapchain sc[GVK_SWAPCHAINS];
    u64 vi_window; /* guest android::Surface* (ANativeWindow*) from VkViSurfaceCreateInfoNN::window */
    u32 ntpool, ntset, tset_cap, tset_free, nbuckets;
    GvkPoolTwin tpool[GVK_POOLS];
    GvkTwinSet* tsets; /* tset_cap slots */
    u32* buckets;      /* the first set with a twin of each hash of its handle */
    u64 presents;
    GvkDebugReport debug[32];
    u64 debug_id;
    _Atomic int debug_enabled;
    /* Protected by vk_lock, like submission to the shared graphics queue. */
    VkHostTimeline retire;
    u64 retire_done;
    int retire_needs_signal;
    GvkRetired* retired;
    size_t retire_head, retire_count, retire_cap;
} g_gvk_api = {.lock = PMUTEX_INIT};

/* ------------------------------------------------------------ calls */
#define H(T, v) ((T)(uintptr_t)(v))
#define U(h)    ((u64)(uintptr_t)(h))

static void ret(CPU* RESTRICT c, u64 v) {
    c->x[0] = v;
    c->pc = c->x[30];
}
static void ret_vk(CPU* RESTRICT c, VkResult r) { ret(c, (u32)r); }
/* the i'th argument passed on the stack (the ninth argument and on), 0 when the stack isn't mapped there */
static u64 stack_arg(const CPU* c, u32 i) {
    u64 a = c->x[31] + 8ull * i;
    return mem_user_mapped(a, 8) ? gr64(a) : 0;
}
/* the host address of the game's array of n elements of elem bytes at a: NULL for none, or when not all of it is
   mapped (said once a call site) */
static const void* garr(u64 a, u64 n, u64 elem) {
    return a && n && n <= AS_SIZE / elem && mem_user_mapped(a, n * elem) ? GP(a) : NULL;
}
/* V, the array of n T at the game's A, or the call ends there when n names one that isn't mapped: what the host would
   read past the guest's memory */
#define GUEST(V, T, A, N)                                                                                              \
    const T* V = garr((A), (N), sizeof(T));                                                                            \
    if ((N) && !V) {                                                                                                   \
        LOG_LIMIT(8, "gvk: %s at %llx isn't mapped: the call does nothing\n", #V, (unsigned long long)(A));            \
        ret_vk(c, VK_ERROR_INITIALIZATION_FAILED);                                                                     \
        return;                                                                                                        \
    }
/* n bytes of src written to the game's g, when it mapped them */
static int put(u64 g, const void* src, size_t n) {
    if (!gvk_writable(g, n)) return 0;
    memcpy(GP(g), src, n);
    return 1;
}
static void put_handle(u64 g, u64 h) {
    if (gvk_writable(g, 8)) gw64(g, h);
}
#define REQUIRE_OUT(A, SIZE)                                                                                           \
    if (!gvk_writable((A), (SIZE))) {                                                                                  \
        ret_vk(c, VK_ERROR_INITIALIZATION_FAILED);                                                                     \
        return;                                                                                                        \
    }
/* a count and an array the game asked for (Vulkan's two-call idiom): *pCount set, at most *pCount of the n elements of
   elem bytes written. VK_INCOMPLETE when some didn't fit */
static VkResult put_list(u64 p_count, u64 p_out, const void* items, u32 n, size_t elem) {
    if (!gvk_writable(p_count, 4)) return VK_ERROR_INITIALIZATION_FAILED;
    if (!p_out) {
        gw32(p_count, n);
        return VK_SUCCESS;
    }
    u32 room = gr32(p_count), k = room < n ? room : n;
    if (k && !put(p_out, items, k * elem)) return VK_ERROR_INITIALIZATION_FAILED;
    gw32(p_count, k);
    return k < n ? VK_INCOMPLETE : VK_SUCCESS;
}

static void release_object(GvkRetired e) {
    if (e.image && e.dm) {
        vk_image_destroy(e.image, e.dm);
    } else if (e.image) {
        vkDestroyImage(g_vk.dev, e.image, NULL);
    } else {
        vkFreeMemory(g_vk.dev, (VkDeviceMemory)(uintptr_t)e.memory.mem, NULL);
        if (e.memory.va) mem_unmap(e.memory.va, e.memory.size);
    }
}

/* Under vk_lock. Queue idle also proves completion when the guest explicitly waits for it. */
static void collect_retired(int idle) {
    if (idle) {
        g_gvk_api.retire_needs_signal = 0;
        g_gvk_api.retire_done = g_gvk_api.retire.value;
    } else if (g_gvk_api.retire_count) {
        u64 done;
        if (!VK_OK(vkGetSemaphoreCounterValue(g_vk.dev, g_gvk_api.retire.sem, &done))) return;
        g_gvk_api.retire_done = done;
    }
    while (g_gvk_api.retire_count) {
        GvkRetired e = g_gvk_api.retired[g_gvk_api.retire_head];
        if (e.gate > g_gvk_api.retire_done) break;
        g_gvk_api.retire_head = (g_gvk_api.retire_head + 1) & (g_gvk_api.retire_cap - 1);
        g_gvk_api.retire_count--;
        release_object(e);
    }
}

static void retire_object(GvkRetired e) {
    vk_lock();
    collect_retired(0);
    VkHostTimeline* t = &g_gvk_api.retire;
    /* A guest can release an image just after submitting its upload, without waiting on the fence. One signal
       covers all releases with no intervening submission, without making destruction wait for the GPU. */
    if (g_gvk_api.retire_needs_signal) {
        u64 value = vk_host_next(t);
        if (!value) fatal("gvk: could not create the resource retirement timeline");
        VkSemaphoreSubmitInfo signal = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                        .semaphore = t->sem,
                                        .value = value,
                                        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        VkSubmitInfo2 submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &signal};
        vk_queue_lock(g_vk.gfx);
        VkResult r = vkQueueSubmit2(g_vk.gfx, 1, &submit, VK_NULL_HANDLE);
        vk_queue_unlock(g_vk.gfx);
        if (r != VK_SUCCESS) fatal("gvk: could not signal the resource retirement timeline (%d)", (int)r);
        g_gvk_api.retire_needs_signal = 0;
    }
    if (t->value <= g_gvk_api.retire_done) {
        release_object(e);
    } else {
        size_t cap = g_gvk_api.retire_cap;
        if (g_gvk_api.retire_count == cap) {
            if (cap > SIZE_MAX / sizeof e / 2) fatal("gvk: too many resources awaiting retirement");
            size_t next = cap ? 2 * cap : 32;
            GvkRetired* objects = malloc(next * sizeof *objects);
            if (!objects) fatal("gvk: could not grow the resource retirement queue");
            for (size_t i = 0; i < cap; i++) objects[i] = g_gvk_api.retired[(g_gvk_api.retire_head + i) & (cap - 1)];
            free(g_gvk_api.retired);
            g_gvk_api.retired = objects;
            g_gvk_api.retire_head = 0;
            g_gvk_api.retire_cap = cap = next;
        }
        e.gate = t->value;
        g_gvk_api.retired[(g_gvk_api.retire_head + g_gvk_api.retire_count++) & (cap - 1)] = e;
    }
    vk_unlock();
}

/* the graphics queue, which the renderer's recording thread also submits to: after the frames it has queued */
static VkResult queue_submit(u32 n, const VkSubmitInfo* s, VkFence fence) {
    vk_lock();
    collect_retired(0);
    vk_rec_queue(g_vk.gfx);
    vk_queue_lock(g_vk.gfx);
    VkResult r = vkQueueSubmit(g_vk.gfx, n, s, fence);
    vk_queue_unlock(g_vk.gfx);
    if (r == VK_SUCCESS && n) g_gvk_api.retire_needs_signal = 1;
    vk_unlock();
    if (r != VK_SUCCESS) WARN_LIMIT(16, "gvk: vkQueueSubmit failed (%d)\n", (int)r);
    return r;
}

/* ------------------------------------------------------------ tables */
/* Open addressing by the nonzero handle each entry starts with (size bytes an entry, cap a power of two), under
   g_gvk_api.lock. A free entry is all zeros, and an entry let go moves the ones after it in its run back where a
   lookup finds them, so there are no tombstones */
#define TAB(T) (u8*)g_gvk_api.T, sizeof *g_gvk_api.T, (u32)(sizeof g_gvk_api.T / sizeof *g_gvk_api.T)
static u64 tab_key(const u8* t, size_t size, u32 i) {
    u64 k;
    memcpy(&k, t + (size_t)i * size, sizeof k);
    return k;
}
static u32 tab_slot(const u8* t, size_t size, u32 cap, u64 key) {
    u32 i = (u32)((key >> 4) * 2654435761u) & (cap - 1);
    while (tab_key(t, size, i) && tab_key(t, size, i) != key) i = (i + 1) & (cap - 1);
    return i;
}
static void tab_put_nl(u8* t, size_t size, u32 cap, u32* n, const void* e) {
    u64 key;
    memcpy(&key, e, sizeof key);
    u32 i = tab_slot(t, size, cap, key);
    if (!tab_key(t, size, i)) {
        if (*n == cap - 1) fatal("gvk: the game holds more than %u objects of one kind", cap - 1);
        ++*n;
    }
    memcpy(t + (size_t)i * size, e, size);
}
static void tab_put(u8* t, size_t size, u32 cap, u32* n, const void* e) {
    pm_lock(&g_gvk_api.lock);
    tab_put_nl(t, size, cap, n, e);
    pm_unlock(&g_gvk_api.lock);
}
/* the entry of key copied to out (zeros for none): whether there is one */
static int tab_get(const u8* t, size_t size, u32 cap, u64 key, void* out) {
    pm_lock(&g_gvk_api.lock);
    u32 i = tab_slot(t, size, cap, key);
    int has = key && tab_key(t, size, i) == key;
    if (has)
        memcpy(out, t + (size_t)i * size, size);
    else
        memset(out, 0, size);
    pm_unlock(&g_gvk_api.lock);
    return has;
}
static int tab_take_nl(u8* t, size_t size, u32 cap, u32* n, u64 key, void* out) {
    u32 i = tab_slot(t, size, cap, key);
    int has = key && tab_key(t, size, i) == key;
    if (has) {
        memcpy(out, t + (size_t)i * size, size);
        --*n;
        memset(t + (size_t)i * size, 0, size);
        u8 e[64];
        for (u32 j = (i + 1) & (cap - 1); tab_key(t, size, j); j = (j + 1) & (cap - 1)) {
            memcpy(e, t + (size_t)j * size, size);
            memset(t + (size_t)j * size, 0, size);
            memcpy(t + (size_t)tab_slot(t, size, cap, tab_key(e, size, 0)) * size, e, size);
        }
    } else {
        memset(out, 0, size);
    }
    return has;
}
static int tab_take(u8* t, size_t size, u32 cap, u32* n, u64 key, void* out) {
    pm_lock(&g_gvk_api.lock);
    int found = tab_take_nl(t, size, cap, n, key, out);
    pm_unlock(&g_gvk_api.lock);
    return found;
}
static_assert(sizeof(GvkMem) <= 64 && sizeof(GvkRpass) <= 64 && sizeof(GvkPass) <= 64 && sizeof(GvkTarget) <= 64 &&
                  sizeof(GvkView) <= 64 && sizeof(GvkDyn) <= 64 && sizeof(GvkPoolTwin) <= 64 &&
                  sizeof(GvkSizes) <= 64 && sizeof(GvkBound) <= 64 && sizeof(GvkLayout) <= 64 &&
                  sizeof(GvkCbWrites) <= 64 && sizeof(GvkDsLayout) <= 64 && sizeof(GvkSetInfo) <= 64 &&
                  sizeof(GvkPoolMasks) <= 64 && sizeof(GvkMixed) <= 64 && sizeof(GvkTemplate) <= 64 &&
                  sizeof(GvkQuery) <= 64,
              "tab_take_nl moves an entry through 64 bytes");

static void mem_put(GvkMem m) { tab_put(TAB(mem), &g_gvk_api.nmem, &m); }
static GvkMem mem_get(u64 mem) {
    GvkMem m;
    tab_get(TAB(mem), mem, &m);
    return m;
}
static GvkMem mem_take(u64 mem) {
    GvkMem m;
    tab_take(TAB(mem), &g_gvk_api.nmem, mem, &m);
    return m;
}
static u32 scale_factor(void);
static void image_track(VkImage img, const VkImageCreateInfo* ci, int importable, VkImageAspectFlags preinit,
                        VkFormat game_depth) {
    GvkImage e = {U(img), ci->format, ci->samples, 0, preinit, importable, game_depth};
    if (scale_factor() > 1 && (ci->tiling == VK_IMAGE_TILING_OPTIMAL || ci->tiling == VK_IMAGE_TILING_LINEAR)) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, ci->format, &props);
        e.features = ci->tiling == VK_IMAGE_TILING_OPTIMAL ? props.optimalTilingFeatures : props.linearTilingFeatures;
    }
    tab_put(TAB(image), &g_gvk_api.nimage, &e);
}
static GvkImage image_get(u64 img) {
    GvkImage e;
    tab_get(TAB(image), img, &e);
    return e;
}
static int importable_has(u64 img) { return image_get(img).importable; }
static void image_untrack(u64 img) {
    GvkImage e;
    tab_take(TAB(image), &g_gvk_api.nimage, img, &e);
}

/* ------------------------------------------------------------ the resolution scale */
/* the widest and highest image the host takes as a framebuffer's attachment */
static u32 fb_max_w(void) {
    const VkPhysicalDeviceLimits* l = &g_vk.props.limits;
    return l->maxFramebufferWidth < l->maxImageDimension2D ? l->maxFramebufferWidth : l->maxImageDimension2D;
}
static u32 fb_max_h(void) {
    const VkPhysicalDeviceLimits* l = &g_vk.props.limits;
    return l->maxFramebufferHeight < l->maxImageDimension2D ? l->maxFramebufferHeight : l->maxImageDimension2D;
}
/* The resolution setting as a whole factor, 1 for none. An image the game draws to is drawn that much larger from the
   first framebuffer that takes it (GvkTarget, fb_scale), and the commands that name its pixels are scaled with it:
   render areas, viewports, scissors, clear rectangles, copies and blits. A copy between a scaled image and one that
   isn't becomes a blit. A texture is never a framebuffer's, whatever its usage says (one game gives every texture
   COLOR_ATTACHMENT), so it stays at the game's size */
static u32 scale_factor(void) {
    float s = g_settings.resolution_scale;
    u32 f = s >= 1.5f ? (u32)(s + 0.5f) : 1, w = fb_max_w() / GVK_SC_MAX, h = fb_max_h() / GVK_SC_MAX_H;
    u32 max = w < h ? w : h;
    /* the swapchain is made at the scale: the largest one the game can make must fit the host's framebuffers */
    if (max && f > max) {
        WARN_LIMIT(1, "gvk: resolution %u is past what the host's framebuffers hold (%ux%u): drawn at %u\n", f,
                   fb_max_w(), fb_max_h(), max);
        return max;
    }
    return f;
}
/* whether the size queries of images drawn at the scale answer the game's size: the push constants past
   GVK_SIZE_PUSH are gvk's, where the host has them (gvk_spirv_scaled_size) */
static int sizes_on(void) { return scale_factor() > 1 && g_vk.props.limits.maxPushConstantsSize >= GVK_SIZE_PUSH + 16; }
/* the aspects of an image of format f */
static VkImageAspectFlags format_aspects(VkFormat f) {
    switch (f) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT: return VK_IMAGE_ASPECT_DEPTH_BIT;
        case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}
/* img as a GvkTarget, of w x h at the game's size, drawn at f */
static void target_track(u64 img, const VkImageCreateInfo* ci, u32 w, u32 h, u32 f) {
    GvkTarget t = {.img = img,
                   .w = w,
                   .h = h,
                   .format = ci->format,
                   .usage = ci->usage,
                   .flags = ci->flags,
                   .layers = (u16)ci->arrayLayers,
                   .mips = (u8)ci->mipLevels,
                   .samples = (u8)ci->samples,
                   .scale = (u8)f};
    tab_put(TAB(target), &g_gvk_api.ntarget, &t);
}
/* A command of the game's command buffer cb on target t, which has no shadow: t gets none while cb can run it. A shadow
   made now would miss what cb writes later, and cb would read the game's image after the shadow took its place
   (GvkTarget.pending). A secondary command buffer runs from primaries gvk doesn't follow, so its command blocks the
   shadow for good, as does one past GVK_CB_WRITES images. writes: the command writes t, which counts at submission */
static void cb_used_nl(u64 cb, GvkTarget* t, int writes) {
    GvkCmd* cmd = &g_gvk_api.cmd[tab_slot(TAB(cmd), cb)];
    if (cmd->cb != cb || cmd->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY) {
        t->blocked = 1;
        return;
    }
    GvkCbWrites* e = &g_gvk_api.cbw[tab_slot(TAB(cbw), cb)];
    if (e->cb != cb) {
        GvkCbWrites fresh = {.cb = cb};
        tab_put_nl(TAB(cbw), &g_gvk_api.ncbw, &fresh);
        e = &g_gvk_api.cbw[tab_slot(TAB(cbw), cb)];
    }
    for (u32 k = 0; k < GVK_CB_WRITES; k++) {
        if (!e->img[k]) {
            e->img[k] = t->img;
            t->pending++;
        }
        if (e->img[k] == t->img) {
            if (writes) cmd->writes |= (u8)(1u << k);
            return;
        }
    }
    t->blocked = 1;
}
/* the game submitted cb: the targets that its commands write now have pixels (GvkTarget.written). A command buffer
   recorded and then reset or recorded again without a submission wrote nothing, and its target stays UNDEFINED */
static void cb_submitted_nl(u64 cb) {
    const GvkCmd* cmd = &g_gvk_api.cmd[tab_slot(TAB(cmd), cb)];
    const GvkCbWrites* e = &g_gvk_api.cbw[tab_slot(TAB(cbw), cb)];
    if (cmd->cb != cb || !cmd->writes || e->cb != cb) return;
    for (u32 k = 0; k < GVK_CB_WRITES && e->img[k]; k++) {
        if (!(cmd->writes >> k & 1)) continue;
        GvkTarget* t = &g_gvk_api.target[tab_slot(TAB(target), e->img[k])];
        if (t->img == e->img[k]) t->written = 1;
    }
}
/* cb can't run its commands any more: recorded again, reset or freed */
static void cb_writes_drop_nl(u64 cb) {
    GvkCmd* cmd = &g_gvk_api.cmd[tab_slot(TAB(cmd), cb)];
    if (cmd->cb == cb) cmd->writes = 0;
    GvkCbWrites e;
    if (!tab_take_nl(TAB(cbw), &g_gvk_api.ncbw, cb, &e)) return;
    for (u32 k = 0; k < GVK_CB_WRITES && e.img[k]; k++) {
        GvkTarget* t = &g_gvk_api.target[tab_slot(TAB(target), e.img[k])];
        if (t->img == e.img[k] && t->pending) t->pending--;
    }
}
/* img gone: the command buffers that used it forget it, as a new image can get its handle */
static void cb_writes_forget_nl(u64 img) {
    for (u32 i = 0; i < GVK_CBS; i++) {
        GvkCbWrites* e = &g_gvk_api.cbw[i];
        for (u32 k = 0; e->cb && k < GVK_CB_WRITES; k++)
            if (e->img[k] == img) {
                memmove(e->img + k, e->img + k + 1, sizeof *e->img * (GVK_CB_WRITES - 1 - k));
                e->img[GVK_CB_WRITES - 1] = 0;
                GvkCmd* cmd = &g_gvk_api.cmd[tab_slot(TAB(cmd), e->cb)];
                if (cmd->cb == e->cb) /* the bits after k move down with their images */
                    cmd->writes = (u8)((cmd->writes & ((1u << k) - 1)) | (cmd->writes >> (k + 1) << k));
                break;
            }
    }
}
/* the image the host draws img's pixels in, and the factor they are drawn at (*f). cb: the command buffer of the
   command that uses them, which writes them where writes is set, 0 for none: before img has a shadow, it gets none
   while cb can run (cb_used_nl), and a shadow made after starts with what cb wrote once it is submitted */
static VkImage host_image(u64 img, u32* f, u64 cb, int writes) {
    VkImage h = H(VkImage, img);
    *f = 1;
    if (!img || scale_factor() == 1) return h;
    pm_lock(&g_gvk_api.lock);
    GvkTarget* t = &g_gvk_api.target[tab_slot(TAB(target), img)];
    if (t->img == img) {
        *f = t->scale;
        if (t->shadow) {
            h = t->shadow;
        } else if (cb && t->scale == 1) {
            if (writes) t->wgen++;
            cb_used_nl(cb, t, writes);
        }
    }
    pm_unlock(&g_gvk_api.lock);
    return h;
}
/* v made again on img */
static VkImageView view_make(const GvkView* v, VkImage img) {
    VkImageViewUsageCreateInfo usage = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = v->usage};
    VkImageViewCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = v->usage ? &usage : NULL,
        .image = img,
        .viewType = v->type,
        .format = v->format,
        .components = {(VkComponentSwizzle)v->swizzle[0], (VkComponentSwizzle)v->swizzle[1],
                       (VkComponentSwizzle)v->swizzle[2], (VkComponentSwizzle)v->swizzle[3]},
        .subresourceRange = {v->aspects, v->base_level, v->levels == 0xff ? VK_REMAINING_MIP_LEVELS : v->levels,
                             v->base_layer, v->layers == 0xffff ? VK_REMAINING_ARRAY_LAYERS : v->layers}};
    VkImageView h = VK_NULL_HANDLE;
    VkResult r = vkCreateImageView(g_vk.dev, &ci, NULL, &h);
    if (r != VK_SUCCESS) WARN_LIMIT(8, "gvk: a view of a target drawn at the scale failed (%d)\n", (int)r);
    return h;
}
/* a view of a GvkTarget, and its shadow's view where the target has a shadow */
static void view_track(u64 view, const VkImageViewCreateInfo* ci) {
    const VkImageSubresourceRange* s = &ci->subresourceRange;
    VkImageUsageFlags usage = 0; /* the view's own usage, which its shadow's view keeps */
    for (const void* p = ci->pNext; p;) {
        VkBaseInStructure base;
        memcpy(&base, p, sizeof base);
        if (base.sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO) {
            VkImageViewUsageCreateInfo u;
            memcpy(&u, p, sizeof u);
            usage = u.usage;
        }
        p = base.pNext;
    }
    GvkView v = {.view = view,
                 .img = U(ci->image),
                 .type = ci->viewType,
                 .format = ci->format,
                 .usage = usage,
                 .swizzle = {(u8)ci->components.r, (u8)ci->components.g, (u8)ci->components.b, (u8)ci->components.a},
                 .aspects = (u16)s->aspectMask,
                 .base_level = (u8)s->baseMipLevel,
                 .levels = s->levelCount == VK_REMAINING_MIP_LEVELS ? 0xff : (u8)s->levelCount,
                 .base_layer = (u16)s->baseArrayLayer,
                 .layers = s->layerCount == VK_REMAINING_ARRAY_LAYERS ? 0xffff : (u16)s->layerCount};
    pm_lock(&g_gvk_api.lock);
    const GvkTarget* t = &g_gvk_api.target[tab_slot(TAB(target), v.img)];
    if (v.img && t->img == v.img) {
        if (t->shadow) v.shadow = view_make(&v, t->shadow);
        tab_put_nl(TAB(view), &g_gvk_api.nview, &v);
    }
    pm_unlock(&g_gvk_api.lock);
}
static int set_has(u64* set, u32 cap, u64 k) {
    u64 e;
    return k && tab_get((u8*)set, sizeof *set, cap, k, &e);
}
static void set_put(u64* set, u32 cap, u32* n, u64 k) { tab_put((u8*)set, sizeof *set, cap, n, &k); }
static void set_take(u64* set, u32 cap, u32* n, u64 k) {
    u64 e;
    if (k) tab_take((u8*)set, sizeof *set, cap, n, k, &e);
}
static VkViewport viewport_at(VkViewport v, u32 f) {
    v.x *= (float)f;
    v.y *= (float)f;
    v.width *= (float)f;
    v.height *= (float)f;
    return v;
}
static VkOffset3D offset_at(VkOffset3D o, u32 f) {
    o.x = (i32)((u32)o.x * f);
    o.y = (i32)((u32)o.y * f);
    return o;
}

/* size bytes of guest pages in the alias region, readable and writable, their host address aligned to align: 0 when
   none are free */
static u64 guest_pages(u64 size, u64 align) {
    for (;;) {
        u64 a = mem_find_free(ALIAS_BASE, ALIAS_BASE + ALIAS_SIZE, size, align);
        if (!a) return 0;
        if ((uintptr_t)GP(a) & (align - 1))
            fatal("gvk: guest memory's host addresses aren't %llu-aligned", (unsigned long long)align);
        int r = mem_map(a, size, MS_NORMAL, PERM_RW);
        if (!r) return a;
        if (r != MEM_BADSTATE) return 0; /* else another thread took the range first */
    }
}

static u32 first_type(u32 bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags not) {
    const VkPhysicalDeviceMemoryProperties* mp = vk_devmem_props();
    for (u32 i = 0; i < mp->memoryTypeCount; i++)
        if ((bits >> i & 1) && (mp->memoryTypes[i].propertyFlags & want) == want &&
            !(mp->memoryTypes[i].propertyFlags & not))
            return i;
    return ~0u;
}

/* the memory types the host imports guest pages as host memory in, read from a page of guest memory: 0 for none */
static u32 host_import_bits(void) {
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &hp};
    vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
    g_gvk_api.import_align =
        hp.minImportedHostPointerAlignment > 0x10000 ? hp.minImportedHostPointerAlignment : 0x10000;
    u64 page = guest_pages(g_gvk_api.import_align, g_gvk_api.import_align);
    if (!page) fatal("gvk: no guest memory for a page to import");
    VkMemoryHostPointerPropertiesEXT pp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkResult r = vkGetMemoryHostPointerPropertiesEXT(g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                     GP(page), &pp);
    mem_unmap(page, g_gvk_api.import_align);
    if (r != VK_SUCCESS) WARN("gvk: the host can't import guest memory as host memory (%d)\n", (int)r);
    return r == VK_SUCCESS ? pp.memoryTypeBits : 0;
}
/* the memory types the host exports as dma-bufs that the CPU maps over guest pages: its host-visible system memory,
   where buffers can be exported as dma-bufs. 0 for none */
static u32 dmabuf_export_bits(void) {
#if defined(__linux__)
    g_gvk_api.import_align = 0x10000;
    if (!vkGetMemoryFdKHR) return 0;
    VkPhysicalDeviceExternalBufferInfo bi = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
                                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                             .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkExternalBufferProperties ep = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    vkGetPhysicalDeviceExternalBufferProperties(g_vk.phys, &bi, &ep);
    if (!(ep.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) {
        WARN("gvk: the host can't export its memory as a dma-buf\n");
        return 0;
    }
    const VkPhysicalDeviceMemoryProperties* mp = vk_devmem_props();
    u32 bits = 0;
    for (u32 i = 0; i < mp->memoryTypeCount; i++)
        if ((mp->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            !(mp->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            bits |= 1u << i;
    return bits;
#else
    return 0;
#endif
}

/* once: how the host imports guest pages, and its types behind the game's three. Returns 1 on success, or 0 when
   probe is set and the host device cannot back guest-mapped memory */
static int types_ready_try(int probe) {
    if (atomic_load_explicit(&g_gvk_api.ready, memory_order_acquire)) return 1;
    int ok = 1;
    pm_lock(&g_gvk_api.lock);
    if (!atomic_load_explicit(&g_gvk_api.ready, memory_order_relaxed)) {
        pc_init(&g_gvk_api.sc_available);
        u32 bits = 0;
        g_gvk_api.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        if (vkGetMemoryHostPointerPropertiesEXT) bits = host_import_bits();
        if (!bits) {
            g_gvk_api.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
            bits = dmabuf_export_bits();
        }
        if (!bits) {
            if (probe) {
                ok = 0;
                goto done;
            }
            fatal("gvk: the renderer's device neither imports guest memory as host memory "
                  "(VK_EXT_external_memory_host) nor exports its own as a dma-buf (VK_EXT_external_memory_dma_buf), "
                  "which the game's mapped memory needs");
        }
        const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        g_gvk_api.local = first_type(~0u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        if (g_gvk_api.local == ~0u) g_gvk_api.local = first_type(~0u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        g_gvk_api.coherent = first_type(bits, hv, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        g_gvk_api.cached = first_type(bits, hv | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
        if (g_gvk_api.coherent == ~0u) g_gvk_api.coherent = g_gvk_api.cached;
        if (g_gvk_api.cached == ~0u) g_gvk_api.cached = g_gvk_api.coherent;
        if (g_gvk_api.local == ~0u || g_gvk_api.coherent == ~0u) {
            if (probe) {
                ok = 0;
                goto done;
            }
            fatal("gvk: no memory type for the game's (device-local %d, imported %#x)", (int)g_gvk_api.local, bits);
        }
        LOG("gvk: the game's memory types are the host's %u (device-local), %u and %u (%s; the host's types for "
            "them %#x, aligned to %llu bytes)\n",
            g_gvk_api.local, g_gvk_api.coherent, g_gvk_api.cached,
            g_gvk_api.handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
                ? "the host's memory, as dma-bufs mapped over guest pages"
                : "guest pages, imported as host memory",
            bits, (unsigned long long)g_gvk_api.import_align);
        atomic_store_explicit(&g_gvk_api.ready, 1, memory_order_release);
    }
done:
    pm_unlock(&g_gvk_api.lock);
    return ok;
}
static void types_ready(void) { (void)types_ready_try(0); }
int gvk_api_supported(void) { return g_vk.dev != VK_NULL_HANDLE && types_ready_try(1); }

/* the host's memoryTypeBits as the game's three types */
static u32 guest_bits(u32 host) {
    return (host >> g_gvk_api.local & 1) | (host >> g_gvk_api.coherent & 1) << 1 | (host >> g_gvk_api.cached & 1) << 2;
}

static void memory_properties(VkPhysicalDeviceMemoryProperties* p) {
    *p = (VkPhysicalDeviceMemoryProperties){0};
    const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    p->memoryTypeCount = 3;
    p->memoryTypes[0] = (VkMemoryType){VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0};
    p->memoryTypes[1] = (VkMemoryType){hv, 0};
    p->memoryTypes[2] = (VkMemoryType){hv | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0};
    p->memoryHeapCount = 1;
    p->memoryHeaps[0] = (VkMemoryHeap){GVK_HEAP, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
}

/* ------------------------------------------------------------ the instance and the devices */
static int debug_available(void) {
    return gvk_export("vkCreateDebugReportCallbackEXT") && gvk_export("vkDestroyDebugReportCallbackEXT") &&
           gvk_export("vkDebugReportMessageEXT");
}

static void gvk_EnumerateInstanceVersion(CPU* RESTRICT c, u64 entry) {
    (void)entry;
    if (c->x[0] && gvk_writable(c->x[0], 4)) gw32(c->x[0], GVK_GUEST_API);
    ret_vk(c, VK_SUCCESS);
}

/* Vulkan 1.1 took these extensions in, and a driver lists them still: an engine written for 1.0 looks for their names and
   not for the version (SDL's GPU API wants VK_KHR_get_physical_device_properties2 and VK_KHR_maintenance1). Their
   commands are the core ones under a name with KHR at its end (proc_addr). Not the YCbCr conversion, whose feature is
   off (guest_features) */
#define EXT(N) {VK_KHR_##N##_EXTENSION_NAME, VK_KHR_##N##_SPEC_VERSION}
static const VkExtensionProperties g_instance_ext[] = {
    EXT(SURFACE),
    {"VK_NN_vi_surface", 1},
    EXT(GET_PHYSICAL_DEVICE_PROPERTIES_2),
    EXT(DEVICE_GROUP_CREATION),
    EXT(EXTERNAL_MEMORY_CAPABILITIES),
    EXT(EXTERNAL_SEMAPHORE_CAPABILITIES),
    EXT(EXTERNAL_FENCE_CAPABILITIES),
    {VK_EXT_DEBUG_REPORT_EXTENSION_NAME, VK_EXT_DEBUG_REPORT_SPEC_VERSION}, /* last: listed where debug_available */
};
static u32 instance_ext_count(void) {
    return (u32)(sizeof g_instance_ext / sizeof *g_instance_ext) - !debug_available();
}
static void gvk_EnumerateInstanceExtensionProperties(CPU* RESTRICT c, u64 entry) {
    ret_vk(c, put_list(c->x[1], c->x[2], g_instance_ext, instance_ext_count(), sizeof *g_instance_ext));
}

static const VkExtensionProperties g_device_ext[] = {
    EXT(SWAPCHAIN),
    EXT(MAINTENANCE1),
    EXT(MAINTENANCE2),
    EXT(MAINTENANCE3),
    EXT(DEDICATED_ALLOCATION),
    EXT(GET_MEMORY_REQUIREMENTS_2),
    EXT(BIND_MEMORY_2),
    EXT(DESCRIPTOR_UPDATE_TEMPLATE),
    EXT(DEVICE_GROUP),
    EXT(EXTERNAL_MEMORY),
    EXT(EXTERNAL_SEMAPHORE),
    EXT(EXTERNAL_FENCE),
    EXT(MULTIVIEW),
    EXT(RELAXED_BLOCK_LAYOUT),
    EXT(SHADER_DRAW_PARAMETERS),
    EXT(STORAGE_BUFFER_STORAGE_CLASS),
    EXT(16BIT_STORAGE),
    EXT(VARIABLE_POINTERS),
};
#undef EXT
static void gvk_EnumerateDeviceExtensionProperties(CPU* RESTRICT c, u64 entry) {
    (void)entry;
    ret_vk(c,
           put_list(c->x[2], c->x[3], g_device_ext, sizeof g_device_ext / sizeof *g_device_ext, sizeof *g_device_ext));
}

static void gvk_EnumerateInstanceLayerProperties(CPU* RESTRICT c, u64 entry) {
    ret_vk(c, put_list(c->x[0], 0, NULL, 0, sizeof(VkLayerProperties)));
}
static void gvk_EnumerateDeviceLayerProperties(CPU* RESTRICT c, u64 entry) {
    ret_vk(c, put_list(c->x[1], 0, NULL, 0, sizeof(VkLayerProperties)));
}

/* Whether every extension a create info enables is one of the n listed, as Vulkan asks: the game is told of one that
   isn't with the error a driver gives, not left to run without it */
static int extensions_listed(const char* what, u64 names, u32 count, const VkExtensionProperties* listed, u32 n) {
    char name[VK_MAX_EXTENSION_NAME_SIZE];
    if (!gvk_unlisted(names, count, listed->extensionName, sizeof *listed, n, name, sizeof name)) return 1;
    if (name[0])
        WARN_LIMIT(16, "gvk: vkCreate%s: the game enables %s, which gvk doesn't list: VK_ERROR_EXTENSION_NOT_PRESENT\n",
                   what, name);
    else
        WARN_LIMIT(16,
                   "gvk: vkCreate%s: a name of the %u extensions the game enables isn't a string it mapped: "
                   "VK_ERROR_EXTENSION_NOT_PRESENT\n",
                   what, count);
    return 0;
}
/* the names of a create info's extensions, for the log */
static void say_extensions(const char* what, u64 names, u32 n) {
    char line[512];
    size_t at = 0;
    line[0] = 0;
    for (u32 i = 0; i < n && i < 32 && at < sizeof line; i++) {
        char name[VK_MAX_EXTENSION_NAME_SIZE];
        if (!mem_user_mapped(names + 8ull * i, 8) || mem_user_string(name, gr64(names + 8ull * i), sizeof name) <= 0)
            continue;
        int w = snprintf(line + at, sizeof line - at, "%s%s", at ? " " : "", name);
        if (w < 0) break;
        at += (size_t)w;
    }
    LOG("gvk: the game's vkCreate%s, extensions: %s\n", what, at ? line : "none");
}

static void gvk_CreateInstance(CPU* RESTRICT c, u64 entry) {
    REQUIRE_OUT(c->x[2], 8);
    (void)entry;
    u64 ci = c->x[0];
    if (ci && mem_user_mapped(ci, sizeof(VkInstanceCreateInfo))) {
        u64 names = gr64(ci + offsetof(VkInstanceCreateInfo, ppEnabledExtensionNames));
        u32 n = gr32(ci + offsetof(VkInstanceCreateInfo, enabledExtensionCount));
        if (n && !mem_user_mapped(names, 8ull * n)) {
            ret_vk(c, VK_ERROR_INITIALIZATION_FAILED);
            return;
        }
        say_extensions("Instance", names, n);
        if (!extensions_listed("Instance", names, n, g_instance_ext, instance_ext_count())) {
            ret_vk(c, VK_ERROR_EXTENSION_NOT_PRESENT);
            return;
        }
        for (u32 i = 0; i < n; i++) {
            char name[VK_MAX_EXTENSION_NAME_SIZE];
            if (mem_user_string(name, gr64(names + 8ull * i), sizeof name) > 0 &&
                !strcmp(name, VK_EXT_DEBUG_REPORT_EXTENSION_NAME))
                atomic_store_explicit(&g_gvk_api.debug_enabled, 1, memory_order_release);
        }
    }
    put_handle(c->x[2], U(g_vk.instance));
    ret_vk(c, VK_SUCCESS);
}

/* The guest debug stream delivers application messages to guest callbacks. The host's loader and validation
   layers belong to the renderer's instance, so they continue to report to the runtime log. */
static void gvk_CreateDebugReportCallbackEXT(CPU* RESTRICT c, u64 entry) {
    REQUIRE_OUT(c->x[3], 8);
    u64 ci = c->x[1];
    if (c->x[0] != U(g_vk.instance) || !ci || !mem_user_mapped(ci, sizeof(VkDebugReportCallbackCreateInfoEXT)) ||
        gr32(ci) != VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT ||
        !atomic_load_explicit(&g_gvk_api.debug_enabled, memory_order_acquire)) {
        ret_vk(c, VK_ERROR_INITIALIZATION_FAILED);
        return;
    }
    GvkDebugReport cb = {.fn = gr64(ci + offsetof(VkDebugReportCallbackCreateInfoEXT, pfnCallback)),
                         .user = gr64(ci + offsetof(VkDebugReportCallbackCreateInfoEXT, pUserData)),
                         .flags = gr32(ci + offsetof(VkDebugReportCallbackCreateInfoEXT, flags))};
    MemBlock code;
    mem_query(cb.fn, &code);
    if (!cb.fn || (cb.fn & 3) || !(code.perm & PERM_X)) {
        ret_vk(c, VK_ERROR_INITIALIZATION_FAILED);
        return;
    }
    pm_lock(&g_gvk_api.lock);
    for (size_t i = 0; i < sizeof g_gvk_api.debug / sizeof *g_gvk_api.debug; i++) {
        if (g_gvk_api.debug[i].id) continue;
        cb.id = ++g_gvk_api.debug_id;
        g_gvk_api.debug[i] = cb;
        break;
    }
    pm_unlock(&g_gvk_api.lock);
    put_handle(c->x[3], cb.id);
    ret_vk(c, cb.id ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY);
}

static void gvk_DestroyDebugReportCallbackEXT(CPU* RESTRICT c, u64 entry) {
    if (c->x[0] == U(g_vk.instance) && c->x[1]) {
        pm_lock(&g_gvk_api.lock);
        for (size_t i = 0; i < sizeof g_gvk_api.debug / sizeof *g_gvk_api.debug; i++)
            if (g_gvk_api.debug[i].id == c->x[1]) {
                g_gvk_api.debug[i] = (GvkDebugReport){0};
                break;
            }
        pm_unlock(&g_gvk_api.lock);
    }
    ret(c, 0);
}

static void gvk_DebugReportMessageEXT(CPU* RESTRICT c, u64 entry) {
    if (c->x[0] == U(g_vk.instance)) {
        u64 args[7];
        for (size_t i = 0; i < 7; i++) args[i] = c->x[i + 1];
        pm_lock(&g_gvk_api.lock);
        u64 last = g_gvk_api.debug_id;
        for (size_t i = 0; i < sizeof g_gvk_api.debug / sizeof *g_gvk_api.debug; i++) {
            GvkDebugReport cb = g_gvk_api.debug[i];
            if (!cb.id || cb.id > last || !(cb.flags & (u32)args[0])) continue;
            pm_unlock(&g_gvk_api.lock);
            c->x[4] = args[4];
            c->x[5] = args[5];
            c->x[6] = args[6];
            c->x[7] = cb.user;
            hook_guest_call(c, cb.fn, args[0], args[1], args[2], args[3]);
            pm_lock(&g_gvk_api.lock);
        }
        pm_unlock(&g_gvk_api.lock);
    }
    ret(c, 0);
}

static void gvk_DestroyNothing(CPU* RESTRICT c, u64 entry) { ret(c, 0); }

static void gvk_EnumeratePhysicalDevices(CPU* RESTRICT c, u64 entry) {
    u64 phys = U(g_vk.phys);
    ret_vk(c, put_list(c->x[1], c->x[2], &phys, 1, sizeof phys));
}

static void gvk_GetPhysicalDeviceProperties(CPU* RESTRICT c, u64 entry) {
    VkPhysicalDeviceProperties p = g_vk.props;
    if (p.apiVersion > GVK_GUEST_API) p.apiVersion = GVK_GUEST_API;
    /* the push constants past GVK_SIZE_PUSH are gvk's, at the resolution scale (gvk_spirv_scaled_size) */
    if (sizes_on() && p.limits.maxPushConstantsSize > GVK_SIZE_PUSH) p.limits.maxPushConstantsSize = GVK_SIZE_PUSH;
    put(c->x[1], &p, sizeof p);
    ret(c, 0);
}
/* the properties structures a game can chain to vkGetPhysicalDeviceProperties2, by size: the host's own values */
static const struct {
    VkStructureType st;
    u32 size;
} g_prop_chain[] = {
    {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES, sizeof(VkPhysicalDeviceSubgroupProperties)},
    {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, sizeof(VkPhysicalDeviceIDProperties)},
    {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES, sizeof(VkPhysicalDeviceMaintenance3Properties)},
    {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES, sizeof(VkPhysicalDeviceMultiviewProperties)},
    {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES, sizeof(VkPhysicalDevicePointClippingProperties)},
    {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES, sizeof(VkPhysicalDeviceProtectedMemoryProperties)},
    {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, sizeof(VkPhysicalDeviceDriverProperties)},
    {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES, sizeof(VkPhysicalDeviceFloatControlsProperties)},
    {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES,
     sizeof(VkPhysicalDeviceDescriptorIndexingProperties)},
    {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, sizeof(VkPhysicalDeviceVulkan11Properties)},
    {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES, sizeof(VkPhysicalDeviceVulkan12Properties)},
};
#define GVK_CHAIN 16 /* the structures of one chain gvk fills, at most */
/* the game's chain from its structure at g: each structure in g_prop_chain the host's values, after the sType and
   pNext the game wrote. A structure gvk doesn't know stays as the game wrote it (said once a type) */
static void gvk_GetPhysicalDeviceProperties2(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    u64 g = c->x[1];
    if (!g || !mem_user_mapped(g, sizeof(VkPhysicalDeviceProperties2))) {
        ret(c, 0);
        return;
    }
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    VkBaseOutStructure* tail = NULL;
    u64 at[GVK_CHAIN];
    u32 size[GVK_CHAIN], n = 0;
    VkBaseOutStructure* host[GVK_CHAIN];
    u64 seen[64];
    u64 a = gr64(g + offsetof(VkPhysicalDeviceProperties2, pNext));
    for (u32 hops = 0; a && hops < 64 && mem_user_mapped(a, sizeof(VkBaseOutStructure)); hops++, a = gr64(a + 8)) {
        int cycle = 0;
        for (u32 j = 0; j < hops; j++)
            if (seen[j] == a) cycle = 1;
        if (cycle) break;
        seen[hops] = a;
        VkStructureType st = (VkStructureType)gr32(a);
        int dup = 0;
        for (u32 j = 0; j < n; j++)
            if (host[j]->sType == st) dup = 1;
        if (dup) continue;
        u32 bytes = 0;
        for (size_t i = 0; i < sizeof g_prop_chain / sizeof *g_prop_chain; i++)
            if (g_prop_chain[i].st == st) bytes = g_prop_chain[i].size;
        if (!bytes || n == GVK_CHAIN || !mem_user_mapped(a, bytes)) {
            LOG_LIMIT(16, "gvk: a properties structure of type %u is left as the game wrote it\n", (u32)st);
            continue;
        }
        VkBaseOutStructure* h = gvk_scratch(bytes);
        memset(h, 0, bytes);
        h->sType = st;
        if (tail)
            tail->pNext = h;
        else
            p2.pNext = h;
        tail = h;
        at[n] = a;
        size[n] = bytes;
        host[n++] = h;
    }
    vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
    if (p2.properties.apiVersion > GVK_GUEST_API) p2.properties.apiVersion = GVK_GUEST_API;
    if (sizes_on() && p2.properties.limits.maxPushConstantsSize > GVK_SIZE_PUSH)
        p2.properties.limits.maxPushConstantsSize = GVK_SIZE_PUSH;
    put(g + offsetof(VkPhysicalDeviceProperties2, properties), &p2.properties, sizeof p2.properties);
    for (u32 i = 0; i < n; i++) put(at[i] + sizeof(VkBaseOutStructure), host[i] + 1, size[i] - sizeof *host[i]);
    ret(c, 0);
}
/* What the renderer's device has on: the game shares it. Not sparse resources or YCbCr conversion, which gvk doesn't
   carry: its queue family binds no sparse memory (vkQueueBindSparse), and it passes no VkSamplerYcbcrConversionInfo
   in a sampler's or a view's chain */
static VkPhysicalDeviceFeatures guest_features(void) {
    VkPhysicalDeviceFeatures f = g_vk.feat2.features;
    f.sparseBinding = f.sparseResidencyBuffer = f.sparseResidencyImage2D = f.sparseResidencyImage3D = VK_FALSE;
    f.sparseResidency2Samples = f.sparseResidency4Samples = f.sparseResidency8Samples = VK_FALSE;
    f.sparseResidency16Samples = f.sparseResidencyAliased = VK_FALSE;
    return f;
}
static void gvk_GetPhysicalDeviceFeatures(CPU* RESTRICT c, u64 entry) {
    VkPhysicalDeviceFeatures f = guest_features();
    put(c->x[1], &f, sizeof f);
    ret(c, 0);
}
/* the 1.1 features structures a game can chain, and Vulkan 1.1's and 1.2's own, from what the renderer's device has
   on. A structure gvk doesn't know stays as the game wrote it (said once a type): a game zeroes it first, so it tells
   the game the features are off */
static void gvk_GetPhysicalDeviceFeatures2(CPU* RESTRICT c, u64 entry) {
    u64 g = c->x[1];
    if (!g || !mem_user_mapped(g, sizeof(VkPhysicalDeviceFeatures2))) {
        ret(c, 0);
        return;
    }
    VkPhysicalDeviceFeatures base = guest_features();
    put(g + offsetof(VkPhysicalDeviceFeatures2, features), &base, sizeof base);
    VkPhysicalDeviceVulkan11Features f11 = g_vk.feat11;
    f11.samplerYcbcrConversion = VK_FALSE;
    const VkPhysicalDeviceVulkan11Features* f = &f11;
    const size_t head = sizeof(VkBaseOutStructure);
    u64 a = gr64(g + offsetof(VkPhysicalDeviceFeatures2, pNext));
    for (u32 hops = 0; a && hops < 64 && mem_user_mapped(a, head); hops++, a = gr64(a + 8)) {
        VkStructureType st = (VkStructureType)gr32(a);
        switch (st) {
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
                put(a + head, (const u8*)f + head, sizeof *f - head);
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
                put(a + head, (const u8*)&g_vk.feat12 + head, sizeof g_vk.feat12 - head);
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES: {
                VkPhysicalDevice16BitStorageFeatures s = {.storageBuffer16BitAccess = f->storageBuffer16BitAccess,
                                                          .uniformAndStorageBuffer16BitAccess =
                                                              f->uniformAndStorageBuffer16BitAccess,
                                                          .storagePushConstant16 = f->storagePushConstant16,
                                                          .storageInputOutput16 = f->storageInputOutput16};
                put(a + head, (const u8*)&s + head, sizeof s - head);
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES: {
                VkPhysicalDeviceMultiviewFeatures s = {.multiview = f->multiview,
                                                       .multiviewGeometryShader = f->multiviewGeometryShader,
                                                       .multiviewTessellationShader = f->multiviewTessellationShader};
                put(a + head, (const u8*)&s + head, sizeof s - head);
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES: {
                VkPhysicalDeviceVariablePointersFeatures s = {.variablePointersStorageBuffer =
                                                                  f->variablePointersStorageBuffer,
                                                              .variablePointers = f->variablePointers};
                put(a + head, (const u8*)&s + head, sizeof s - head);
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES: {
                VkPhysicalDeviceProtectedMemoryFeatures s = {.protectedMemory = f->protectedMemory};
                put(a + head, (const u8*)&s + head, sizeof s - head);
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES: {
                VkPhysicalDeviceSamplerYcbcrConversionFeatures s = {.samplerYcbcrConversion =
                                                                        f->samplerYcbcrConversion};
                put(a + head, (const u8*)&s + head, sizeof s - head);
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES: {
                VkPhysicalDeviceShaderDrawParametersFeatures s = {.shaderDrawParameters = f->shaderDrawParameters};
                put(a + head, (const u8*)&s + head, sizeof s - head);
                break;
            }
            default:
                LOG_LIMIT(16, "gvk: a features structure of type %u is left as the game wrote it\n", (u32)st);
                break;
        }
    }
    ret(c, 0);
}
static void gvk_GetPhysicalDeviceMemoryProperties(CPU* RESTRICT c, u64 entry) {
    VkPhysicalDeviceMemoryProperties p;
    memory_properties(&p);
    put(c->x[1], &p, sizeof p);
    ret(c, 0);
}
static void gvk_GetPhysicalDeviceMemoryProperties2(CPU* RESTRICT c, u64 entry) {
    VkPhysicalDeviceMemoryProperties p;
    memory_properties(&p);
    if (c->x[1]) put(c->x[1] + offsetof(VkPhysicalDeviceMemoryProperties2, memoryProperties), &p, sizeof p);
    ret(c, 0);
}
/* one family: the renderer's graphics queue, which binds no sparse memory (guest_features) */
static VkQueueFamilyProperties guest_family(void) {
    return (VkQueueFamilyProperties){
        VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT, 1, g_vk.gfx_timestamp_bits, {1, 1, 1}};
}
static void gvk_GetPhysicalDeviceQueueFamilyProperties(CPU* RESTRICT c, u64 entry) {
    VkQueueFamilyProperties q = guest_family();
    put_list(c->x[1], c->x[2], &q, 1, sizeof q);
    ret(c, 0);
}
/* A depth format the host can't attach, as the nearest one it can: the console has 24-bit depth, which AMD's GPUs
   don't. A copy of the depth to or from a buffer then moves 32-bit floats where the game expects 24-bit integers */
static VkFormat host_format(VkFormat f) {
    VkFormat to = f == VK_FORMAT_D24_UNORM_S8_UINT     ? VK_FORMAT_D32_SFLOAT_S8_UINT
                  : f == VK_FORMAT_X8_D24_UNORM_PACK32 ? VK_FORMAT_D32_SFLOAT
                                                       : f;
    if (to == f) return f;
    VkFormatProperties p = {0};
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    if (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) return f;
    LOG_LIMIT(2, "gvk: the host has no depth format %d: the game's images of it are format %d\n", (int)f, (int)to);
    return to;
}
static void gvk_GetPhysicalDeviceFormatProperties(CPU* RESTRICT c, u64 entry) {
    VkFormatProperties p = {0};
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, host_format((VkFormat)(u32)c->x[1]), &p);
    put(c->x[2], &p, sizeof p);
    ret(c, 0);
}
static void gvk_GetPhysicalDeviceImageFormatProperties(CPU* RESTRICT c, u64 entry) {
    VkImageFormatProperties p = {0};
    VkResult r = vkGetPhysicalDeviceImageFormatProperties(g_vk.phys, host_format((VkFormat)(u32)c->x[1]),
                                                          (VkImageType)(u32)c->x[2], (VkImageTiling)(u32)c->x[3],
                                                          (VkImageUsageFlags)c->x[4], (VkImageCreateFlags)c->x[5], &p);
    put(c->x[6], &p, sizeof p);
    ret_vk(c, r);
}
/* Vulkan 1.1's two-call idiom for one structure that has its own sType and pNext: n bytes of body written at off in
   it, which leaves the game's chain alone */
static VkResult put_one_chained(u64 p_count, u64 p_out, size_t off, const void* body, size_t n) {
    if (!gvk_writable(p_count, 4)) return VK_ERROR_INITIALIZATION_FAILED;
    if (p_out && gr32(p_count) && !put(p_out + off, body, n)) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = p_out && !gr32(p_count) ? VK_INCOMPLETE : VK_SUCCESS;
    gw32(p_count, r == VK_SUCCESS);
    return r;
}
/* one group of the one device, which allocates no subsets */
static void gvk_EnumeratePhysicalDeviceGroups(CPU* RESTRICT c, u64 entry) {
    VkPhysicalDeviceGroupProperties g = {.physicalDeviceCount = 1, .physicalDevices = {g_vk.phys}};
    size_t off = offsetof(VkPhysicalDeviceGroupProperties, physicalDeviceCount);
    ret_vk(c, put_one_chained(c->x[1], c->x[2], off, (const u8*)&g + off, sizeof g - off));
}
static void gvk_GetPhysicalDeviceQueueFamilyProperties2(CPU* RESTRICT c, u64 entry) {
    VkQueueFamilyProperties q = guest_family();
    put_one_chained(c->x[1], c->x[2], offsetof(VkQueueFamilyProperties2, queueFamilyProperties), &q, sizeof q);
    ret(c, 0);
}
/* the host's answer for the base structure: no chained structure gvk passes on needs another */
static void gvk_GetPhysicalDeviceFormatProperties2(CPU* RESTRICT c, u64 entry) {
    VkFormatProperties p = {0};
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, host_format((VkFormat)(u32)c->x[1]), &p);
    put(c->x[2] + offsetof(VkFormatProperties2, formatProperties), &p, sizeof p);
    ret(c, 0);
}
/* The base query as vkGetPhysicalDeviceImageFormatProperties answers it. The game's objects have no external handle
   types (an external image query is not supported, and the properties of one are none), and no YCbCr conversion
   (guest_features): its sampler takes one descriptor */
static void gvk_GetPhysicalDeviceImageFormatProperties2(CPU* RESTRICT c, u64 entry) {
    u64 gi = c->x[1], go = c->x[2];
    if (!gi || !mem_user_mapped(gi, sizeof(VkPhysicalDeviceImageFormatInfo2)) ||
        !gvk_writable(go, sizeof(VkImageFormatProperties2))) {
        ret_vk(c, VK_ERROR_INITIALIZATION_FAILED);
        return;
    }
    VkPhysicalDeviceImageFormatInfo2 in;
    memcpy(&in, GP(gi), sizeof in);
    VkResult r = VK_SUCCESS;
    u64 a = U(in.pNext);
    for (u32 hops = 0; a && hops < 64 && mem_user_mapped(a, sizeof(VkBaseInStructure)); hops++, a = gr64(a + 8))
        if (gr32(a) == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO &&
            mem_user_mapped(a, sizeof(VkPhysicalDeviceExternalImageFormatInfo)) &&
            gr32(a + offsetof(VkPhysicalDeviceExternalImageFormatInfo, handleType)))
            r = VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkImageFormatProperties p = {0};
    if (r == VK_SUCCESS)
        r = vkGetPhysicalDeviceImageFormatProperties(g_vk.phys, host_format(in.format), in.type, in.tiling, in.usage,
                                                     in.flags, &p);
    put(go + offsetof(VkImageFormatProperties2, imageFormatProperties), &p, sizeof p);
    a = gr64(go + offsetof(VkImageFormatProperties2, pNext));
    for (u32 hops = 0; a && hops < 64 && mem_user_mapped(a, sizeof(VkBaseOutStructure)); hops++, a = gr64(a + 8)) {
        if (gr32(a) == VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES) {
            VkExternalMemoryProperties none = {0};
            put(a + offsetof(VkExternalImageFormatProperties, externalMemoryProperties), &none, sizeof none);
        } else if (gr32(a) == VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES) {
            u32 one = 1;
            put(a + offsetof(VkSamplerYcbcrConversionImageFormatProperties, combinedImageSamplerDescriptorCount), &one,
                sizeof one);
        }
    }
    ret_vk(c, r);
}
/* the game's buffers, fences and semaphores have no external handle types */
static void gvk_GetPhysicalDeviceExternalBufferProperties(CPU* RESTRICT c, u64 entry) {
    VkExternalMemoryProperties none = {0};
    put(c->x[2] + offsetof(VkExternalBufferProperties, externalMemoryProperties), &none, sizeof none);
    ret(c, 0);
}
static void gvk_GetPhysicalDeviceExternalFenceProperties(CPU* RESTRICT c, u64 entry) {
    size_t off = offsetof(VkExternalFenceProperties, exportFromImportedHandleTypes);
    VkExternalFenceProperties none = {0};
    put(c->x[2] + off, (const u8*)&none + off, sizeof none - off);
    ret(c, 0);
}
static void gvk_GetPhysicalDeviceExternalSemaphoreProperties(CPU* RESTRICT c, u64 entry) {
    size_t off = offsetof(VkExternalSemaphoreProperties, exportFromImportedHandleTypes);
    VkExternalSemaphoreProperties none = {0};
    put(c->x[2] + off, (const u8*)&none + off, sizeof none - off);
    ret(c, 0);
}
/* no sparse resources (guest_features): no formats for them, and no image of them */
static void gvk_GetPhysicalDeviceSparseImageFormatProperties(CPU* RESTRICT c, u64 entry) {
    put_list(c->x[6], c->x[7], NULL, 0, sizeof(VkSparseImageFormatProperties));
    ret(c, 0);
}
static void gvk_GetPhysicalDeviceSparseImageFormatProperties2(CPU* RESTRICT c, u64 entry) {
    put_list(c->x[2], c->x[3], NULL, 0, sizeof(VkSparseImageFormatProperties2));
    ret(c, 0);
}
static void gvk_GetImageSparseMemoryRequirements(CPU* RESTRICT c, u64 entry) {
    put_list(c->x[2], c->x[3], NULL, 0, sizeof(VkSparseImageMemoryRequirements));
    ret(c, 0);
}
static void gvk_GetImageSparseMemoryRequirements2(CPU* RESTRICT c, u64 entry) {
    put_list(c->x[2], c->x[3], NULL, 0, sizeof(VkSparseImageMemoryRequirements2));
    ret(c, 0);
}
/* A command that needs a feature that guest_features says is off: a game that calls it broke Vulkan's rules, and
   the console's driver would have done what gvk cannot say */
#define FEATURE_OFF(NAME, FEATURE)                                                                                     \
    static void gvk_##NAME(CPU* RESTRICT c, u64 entry) {                                                               \
        (void)c;                                                                                                       \
        fatal("gvk: the game called vk" #NAME ", which needs " FEATURE ", which gvk tells it is off");                 \
    }
FEATURE_OFF(QueueBindSparse, "sparseBinding")
FEATURE_OFF(CreateSamplerYcbcrConversion, "samplerYcbcrConversion")

static void gvk_CreateDevice(CPU* RESTRICT c, u64 entry) {
    REQUIRE_OUT(c->x[3], 8);
    u64 ci = c->x[1];
    if (ci && mem_user_mapped(ci, sizeof(VkDeviceCreateInfo))) {
        u64 names = gr64(ci + offsetof(VkDeviceCreateInfo, ppEnabledExtensionNames));
        u32 n = gr32(ci + offsetof(VkDeviceCreateInfo, enabledExtensionCount));
        say_extensions("Device", names, n);
        if (!extensions_listed("Device", names, n, g_device_ext, sizeof g_device_ext / sizeof *g_device_ext)) {
            ret_vk(c, VK_ERROR_EXTENSION_NOT_PRESENT);
            return;
        }
    }
    types_ready();
    put_handle(c->x[3], U(g_vk.dev));
    ret_vk(c, VK_SUCCESS);
}
static void gvk_GetDeviceQueue(CPU* RESTRICT c, u64 entry) {
    put_handle(c->x[3], U(g_vk.gfx));
    ret(c, 0);
}
static void gvk_GetDeviceQueue2(CPU* RESTRICT c, u64 entry) {
    put_handle(c->x[2], U(g_vk.gfx));
    ret(c, 0);
}
/* one device: its own memory has every peer feature */
static void gvk_GetDeviceGroupPeerMemoryFeatures(CPU* RESTRICT c, u64 entry) {
    VkPeerMemoryFeatureFlags all = VK_PEER_MEMORY_FEATURE_COPY_SRC_BIT | VK_PEER_MEMORY_FEATURE_COPY_DST_BIT |
                                   VK_PEER_MEMORY_FEATURE_GENERIC_SRC_BIT | VK_PEER_MEMORY_FEATURE_GENERIC_DST_BIT;
    put(c->x[4], &all, sizeof all);
    ret(c, 0);
}
static void gvk_GetDeviceMemoryCommitment(CPU* RESTRICT c, u64 entry) {
    VkDeviceSize bytes = 0;
    vkGetDeviceMemoryCommitment(g_vk.dev, H(VkDeviceMemory, c->x[1]), &bytes);
    put(c->x[2], &bytes, sizeof bytes);
    ret(c, 0);
}
static void gvk_GetRenderAreaGranularity(CPU* RESTRICT c, u64 entry) {
    VkExtent2D g = {1, 1};
    vkGetRenderAreaGranularity(g_vk.dev, H(VkRenderPass, c->x[1]), &g);
    put(c->x[2], &g, sizeof g);
    ret(c, 0);
}
static void gvk_DeviceWaitIdle(CPU* RESTRICT c, u64 entry) {
    kthread_host_wait_begin();
    vk_lock();
    vk_rec_queue(g_vk.gfx);
    vk_queue_lock(g_vk.gfx);
    VkResult r = vkQueueWaitIdle(g_vk.gfx);
    vk_queue_unlock(g_vk.gfx);
    if (r == VK_SUCCESS) collect_retired(1);
    vk_unlock();
    kthread_host_wait_end();
    ret_vk(c, r);
}

/* ------------------------------------------------------------ memory */
static void gvk_AllocateMemory(CPU* RESTRICT c, u64 entry) {
    REQUIRE_OUT(c->x[3], 8);
    gvk_scratch_reset();
    u64 ai = c->x[1];
    if (!ai || !mem_user_mapped(ai, sizeof(VkMemoryAllocateInfo))) {
        ret_vk(c, VK_ERROR_INITIALIZATION_FAILED);
        return;
    }
    types_ready();
    u64 size = gr64(ai + offsetof(VkMemoryAllocateInfo, allocationSize));
    u32 type = gr32(ai + offsetof(VkMemoryAllocateInfo, memoryTypeIndex));
    const void* next;
    if (!size || !gvk_chain_in(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, gr64(ai + 8), &next)) {
        ret_vk(c, VK_ERROR_INITIALIZATION_FAILED);
        return;
    }
    VkMemoryAllocateInfo hi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = size};
    VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkExportMemoryAllocateInfo exp = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                                      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    int dmabuf = g_gvk_api.handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    GvkMem m = {.allocation_size = size};
    if (type == 0) {
        hi.memoryTypeIndex = g_gvk_api.local;
        hi.pNext = next;
    } else if (type <= 2) {
        if (size > AS_SIZE - (g_gvk_api.import_align - 1)) {
            ret_vk(c, VK_ERROR_OUT_OF_HOST_MEMORY);
            return;
        }
        m.size = snx_align_up(size, g_gvk_api.import_align);
        /* guest pages come by a whole number of import_align, and a dedicated allocation's size is its resource's own:
           of another size the pages go without VkMemoryDedicatedAllocateInfo (the one structure next holds), which is a
           hint where the host doesn't ask for it */
        if (m.size != size) next = NULL;
        m.va = guest_pages(m.size, g_gvk_api.import_align);
        if (!m.va) {
            WARN_LIMIT(8, "gvk: no guest memory for an allocation of %llu bytes\n", (unsigned long long)size);
            ret_vk(c, VK_ERROR_OUT_OF_HOST_MEMORY);
            return;
        }
        hi.allocationSize = m.size;
        hi.memoryTypeIndex = type == 1 ? g_gvk_api.coherent : g_gvk_api.cached;
        if (dmabuf) {
            exp.pNext = next;
            hi.pNext = &exp;
        } else {
            imp.pNext = next;
            imp.pHostPointer = GP(m.va);
            hi.pNext = &imp;
        }
    } else {
        ret_vk(c, VK_ERROR_OUT_OF_DEVICE_MEMORY);
        return;
    }
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkResult r = vkAllocateMemory(g_vk.dev, &hi, NULL, &mem);
    if (r == VK_SUCCESS && dmabuf && m.va) {
        /* the dma-buf way: the host's memory, exported as a dma-buf that is mapped over the guest pages, so the game's
           address is the memory's. It stays mapped as the host's too: a capture layer (RenderDoc) reads what the CPU
           wrote to coherent memory through that mapping at each submit */
        VkMemoryGetFdInfoKHR gi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
                                   .memory = mem,
                                   .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        int fd = -1, moved = -1;
        void* host_map = NULL;
        r = vkGetMemoryFdKHR(g_vk.dev, &gi, &fd);
        if (r == VK_SUCCESS) {
            moved = mem_migrate(m.va, m.size, fd, NULL); /* a dma-buf of the driver's: no view of its pages here */
            plat_vm_fd_close(fd); /* the mapping holds the dma-buf */
        }
        if (r == VK_SUCCESS && !moved) r = vkMapMemory(g_vk.dev, mem, 0, VK_WHOLE_SIZE, 0, &host_map);
        if (r != VK_SUCCESS || moved) {
            WARN_LIMIT(8, "gvk: an allocation of %llu bytes can't be a dma-buf over guest memory (%d, move %d)\n",
                       (unsigned long long)size, (int)r, moved);
            vkFreeMemory(g_vk.dev, mem, NULL);
            r = VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }
    if (r != VK_SUCCESS) {
        WARN_LIMIT(16, "gvk: an allocation of %llu bytes of type %u failed (%d)\n", (unsigned long long)size, type,
                   (int)r);
        if (m.va) mem_unmap(m.va, m.size);
        ret_vk(c, r);
        return;
    }
    m.mem = U(mem);
    mem_put(m);
    put_handle(c->x[3], m.mem);
    ret_vk(c, VK_SUCCESS);
}
static void gvk_FreeMemory(CPU* RESTRICT c, u64 entry) {
    if (c->x[1]) {
        GvkMem m = mem_take(c->x[1]);
        if (!m.mem) fatal("gvk: invalid device memory handle");
        retire_object((GvkRetired){.memory = m});
    }
    ret(c, 0);
}
/* an offset and a size in the allocation (the guest pages it took, a whole number of import_align) */
static void gvk_MapMemory(CPU* RESTRICT c, u64 entry) {
    REQUIRE_OUT(c->x[5], 8);
    GvkMem m = mem_get(c->x[1]);
    u64 off = c->x[2], size = c->x[3];
    if (!m.va || off >= m.allocation_size || (size != VK_WHOLE_SIZE && (!size || size > m.allocation_size - off))) {
        ret_vk(c, VK_ERROR_MEMORY_MAP_FAILED);
        return;
    }
    put_handle(c->x[5], m.va + off);
    ret_vk(c, VK_SUCCESS);
}
/* the CPU's types are coherent: a flush or an invalidate does nothing */
static void gvk_FlushMappedMemoryRanges(CPU* RESTRICT c, u64 entry) { ret_vk(c, VK_SUCCESS); }

/* the host's memoryTypeBits as the game's, local_only for an image the host imports no guest memory for: device-local
   memory alone, also where the host's device-local type is the one it imports guest pages in */
static u32 guest_type_bits(u32 host, int local_only) { return guest_bits(host) & (local_only ? 1u : 7u); }
static void requirements_out(u64 g, VkMemoryRequirements* r, int local_only) {
    r->memoryTypeBits = guest_type_bits(r->memoryTypeBits, local_only);
    if (!r->memoryTypeBits)
        WARN_LIMIT(8, "gvk: a resource takes none of the game's memory types (%llu bytes)\n",
                   (unsigned long long)r->size);
    put(g, r, sizeof *r);
}
static void gvk_GetBufferMemoryRequirements(CPU* RESTRICT c, u64 entry) {
    VkMemoryRequirements r = {0};
    vkGetBufferMemoryRequirements(g_vk.dev, H(VkBuffer, c->x[1]), &r);
    requirements_out(c->x[2], &r, 0);
    ret(c, 0);
}
static void gvk_GetImageMemoryRequirements(CPU* RESTRICT c, u64 entry) {
    VkMemoryRequirements r = {0};
    vkGetImageMemoryRequirements(g_vk.dev, H(VkImage, c->x[1]), &r);
    requirements_out(c->x[2], &r, !importable_has(c->x[1]));
    ret(c, 0);
}
static void requirements2(CPU* RESTRICT c, int image) {
    gvk_scratch_reset();
    const void* info = gvk_in(image ? &gvk_t_imreq2 : &gvk_t_bufreq2, c->x[1], 1);
    VkMemoryRequirements2* r = gvk_in(&gvk_t_req2, c->x[2], 1);
    if (info && r) {
        if (image)
            vkGetImageMemoryRequirements2(g_vk.dev, info, r);
        else
            vkGetBufferMemoryRequirements2(g_vk.dev, info, r);
        int local_only = image && !importable_has(U(((const VkImageMemoryRequirementsInfo2*)info)->image));
        r->memoryRequirements.memoryTypeBits = guest_type_bits(r->memoryRequirements.memoryTypeBits, local_only);
        gvk_out(&gvk_t_req2, c->x[2], r);
    }
    ret(c, 0);
}
static void gvk_GetImageMemoryRequirements2(CPU* RESTRICT c, u64 entry) { requirements2(c, 1); }
static void gvk_GetBufferMemoryRequirements2(CPU* RESTRICT c, u64 entry) { requirements2(c, 0); }
static void gvk_BindBufferMemory(CPU* RESTRICT c, u64 entry) {
    ret_vk(c, vkBindBufferMemory(g_vk.dev, H(VkBuffer, c->x[1]), H(VkDeviceMemory, c->x[2]), c->x[3]));
}
/* A preinitialized external image starts undefined on the host. Initialize its host layout at bind, before writes
   made through the bound image's mapping. Its first guest barrier then preserves the texels. */
static VkResult images_general(const VkImage* img, u32 n, VkImageAspectFlags aspects);
static void gvk_BindImageMemory(CPU* RESTRICT c, u64 entry) {
    VkImage img = H(VkImage, c->x[1]);
    VkResult r = vkBindImageMemory(g_vk.dev, img, H(VkDeviceMemory, c->x[2]), c->x[3]);
    VkImageAspectFlags preinit = image_get(c->x[1]).preinit;
    if (r == VK_SUCCESS && preinit) r = images_general(&img, 1, preinit);
    ret_vk(c, r);
}
/* vkBind*Memory2: no chained structure gvk passes on (device groups have one device) */
static void gvk_BindBufferMemory2(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    u32 n = (u32)c->x[1];
    const VkBindBufferMemoryInfo* b = gvk_in(&gvk_t_bind_buffer, c->x[2], n);
    ret_vk(c, n && !b ? VK_ERROR_INITIALIZATION_FAILED : vkBindBufferMemory2(g_vk.dev, n, b));
}
static void gvk_BindImageMemory2(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    u32 n = (u32)c->x[1];
    const VkBindImageMemoryInfo* b = gvk_in(&gvk_t_bind_image, c->x[2], n);
    VkResult r = n && !b ? VK_ERROR_INITIALIZATION_FAILED : vkBindImageMemory2(g_vk.dev, n, b);
    for (u32 i = 0; r == VK_SUCCESS && i < n; i++) { /* as gvk_BindImageMemory */
        VkImageAspectFlags preinit = image_get(U(b[i].image)).preinit;
        if (preinit) r = images_general(&b[i].image, 1, preinit);
    }
    ret_vk(c, r);
}

/* ------------------------------------------------------------ objects */
/* vkCreateX(device, pCreateInfo, pAllocator, pX): the info copied as type t, the handle written where the game asked */
#define CREATE(NAME, T, TYPE)                                                                                          \
    static void gvk_##NAME(CPU* RESTRICT c, u64 entry) {                                                               \
        REQUIRE_OUT(c->x[3], 8);                                                                                       \
        gvk_scratch_reset();                                                                                           \
        const void* ci = gvk_in(&(TYPE), c->x[1], 1);                                                                  \
        T h = VK_NULL_HANDLE;                                                                                          \
        VkResult r = ci ? vk##NAME(g_vk.dev, ci, NULL, &h) : VK_ERROR_INITIALIZATION_FAILED;                           \
        if (r != VK_SUCCESS) WARN_LIMIT(16, "gvk: vk" #NAME " failed (%d)\n", (int)r);                                 \
        put_handle(c->x[3], U(h));                                                                                     \
        ret_vk(c, r);                                                                                                  \
    }
/* vkDestroyX(device, x, pAllocator) */
#define DESTROY(NAME, T)                                                                                               \
    static void gvk_##NAME(CPU* RESTRICT c, u64 entry) {                                                               \
        vk##NAME(g_vk.dev, H(T, c->x[1]), NULL);                                                                       \
        ret(c, 0);                                                                                                     \
    }

static void gvk_CreateImageView(CPU* RESTRICT c, u64 entry) {
    REQUIRE_OUT(c->x[3], 8);
    gvk_scratch_reset();
    VkImageViewCreateInfo* ci = gvk_in(&gvk_t_image_view, c->x[1], 1);
    VkImageView h = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (ci) {
        ci->format = host_format(ci->format);
        r = vkCreateImageView(g_vk.dev, ci, NULL, &h);
        if (r == VK_SUCCESS && scale_factor() > 1) view_track(U(h), ci);
    }
    if (r != VK_SUCCESS) WARN_LIMIT(16, "gvk: vkCreateImageView failed (%d)\n", (int)r);
    put_handle(c->x[3], U(h));
    ret_vk(c, r);
}
/* The console's driver ignores image layouts, and a game made on it names ones its images aren't in: a render pass
   that starts from SHADER_READ_ONLY on an image last drawn in COLOR_ATTACHMENT, or on a swapchain image made without
   SAMPLED. Each layout the game names but UNDEFINED and PREINITIALIZED is GENERAL for the host, which holds each
   image in it: in render passes, barriers, descriptors and copies, and in the renderer's copy of a swapchain image
   (renderer.h present_image) */
static VkImageLayout host_layout(VkImageLayout l) {
    return l == VK_IMAGE_LAYOUT_UNDEFINED || l == VK_IMAGE_LAYOUT_PREINITIALIZED ? l : VK_IMAGE_LAYOUT_GENERAL;
}
/* a layout argument of a command, in host_layout */
static VkImageLayout arg_layout(u64 x) { return host_layout((VkImageLayout)(u32)x); }
/* a copy of n of a subpass's attachment references, in host_layout */
static const VkAttachmentReference* refs_in(const VkAttachmentReference* r, u32 n) {
    if (!r || !n) return r;
    VkAttachmentReference* s = gvk_scratch(sizeof *s * n);
    for (u32 i = 0; i < n; i++) s[i] = (VkAttachmentReference){r[i].attachment, host_layout(r[i].layout)};
    return s;
}
/* A render pass of one subpass stores every attachment, and has a twin made beside it that loads them, from the layout
   the first one leaves them in: what pass_out ends early and pass_back goes on with. Its attachments are a copy of the
   game's, which it changes */
static void gvk_CreateRenderPass(CPU* RESTRICT c, u64 entry) {
    REQUIRE_OUT(c->x[3], 8);
    gvk_scratch_reset();
    VkRenderPassCreateInfo* ci = gvk_in(&gvk_t_rpass, c->x[1], 1);
    VkRenderPass h = VK_NULL_HANDLE, resume = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    GvkRpass meta = {0};
    if (ci) {
        VkAttachmentDescription* a = NULL;
        if (ci->pAttachments && ci->attachmentCount) {
            a = gvk_scratch(sizeof *a * ci->attachmentCount);
            memcpy(a, ci->pAttachments, sizeof *a * ci->attachmentCount);
            ci->pAttachments = a;
        }
        int split = ci->subpassCount == 1;
        for (u32 i = 0; a && i < ci->attachmentCount; i++) {
            a[i].format = host_format(a[i].format);
            a[i].initialLayout = host_layout(a[i].initialLayout);
            if (a[i].initialLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
                (a[i].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD || a[i].stencilLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD))
                a[i].initialLayout = VK_IMAGE_LAYOUT_GENERAL;
            a[i].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
            if (split) a[i].storeOp = a[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        VkSubpassDescription* sp = (VkSubpassDescription*)ci->pSubpasses;
        for (u32 i = 0; sp && i < ci->subpassCount; i++) {
            sp[i].pInputAttachments = refs_in(sp[i].pInputAttachments, sp[i].inputAttachmentCount);
            sp[i].pColorAttachments = refs_in(sp[i].pColorAttachments, sp[i].colorAttachmentCount);
            sp[i].pResolveAttachments = refs_in(sp[i].pResolveAttachments, sp[i].colorAttachmentCount);
            sp[i].pDepthStencilAttachment = refs_in(sp[i].pDepthStencilAttachment, 1);
        }
        for (u32 i = 0; i < ci->dependencyCount; i++)
            meta.nself += ci->pDependencies[i].srcSubpass != VK_SUBPASS_EXTERNAL &&
                          ci->pDependencies[i].srcSubpass == ci->pDependencies[i].dstSubpass;
        if (meta.nself) {
            meta.self = malloc(meta.nself * sizeof *meta.self);
            if (!meta.self) {
                ret_vk(c, VK_ERROR_OUT_OF_HOST_MEMORY);
                return;
            }
            for (u32 i = 0, at = 0; i < ci->dependencyCount; i++)
                if (ci->pDependencies[i].srcSubpass != VK_SUBPASS_EXTERNAL &&
                    ci->pDependencies[i].srcSubpass == ci->pDependencies[i].dstSubpass)
                    meta.self[at++] = ci->pDependencies[i];
        }
        r = vkCreateRenderPass(g_vk.dev, ci, NULL, &h);
        if (r == VK_SUCCESS && split) {
            for (u32 i = 0; a && i < ci->attachmentCount; i++) {
                a[i].loadOp = a[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                a[i].initialLayout = a[i].finalLayout;
            }
            if (vkCreateRenderPass(g_vk.dev, ci, NULL, &resume) != VK_SUCCESS) resume = VK_NULL_HANDLE;
        }
    }
    if (r != VK_SUCCESS) WARN_LIMIT(16, "gvk: vkCreateRenderPass failed (%d)\n", (int)r);
    if (h) {
        meta.rp = U(h);
        meta.resume = U(resume);
        meta.subpasses = ci->subpassCount;
        tab_put(TAB(rpass), &g_gvk_api.nrpass, &meta);
    } else {
        free(meta.self);
    }
    put_handle(c->x[3], U(h));
    ret_vk(c, r);
}
static void gvk_DestroyRenderPass(CPU* RESTRICT c, u64 entry) {
    GvkRpass e;
    if (tab_take(TAB(rpass), &g_gvk_api.nrpass, c->x[1], &e)) {
        if (e.resume) vkDestroyRenderPass(g_vk.dev, H(VkRenderPass, e.resume), NULL);
        free(e.self);
    }
    vkDestroyRenderPass(g_vk.dev, H(VkRenderPass, c->x[1]), NULL);
    ret(c, 0);
}
CREATE(CreateBufferView, VkBufferView, gvk_t_buffer_view)
CREATE(CreateSampler, VkSampler, gvk_t_sampler)
/* At the resolution scale each layout takes gvk's push constants for the size queries of its fragment shaders
   (gvk_spirv_scaled_size), past the game's, which the limit gvk tells it keeps below GVK_SIZE_PUSH */
static void gvk_CreatePipelineLayout(CPU* RESTRICT c, u64 entry) {
    REQUIRE_OUT(c->x[3], 8);
    gvk_scratch_reset();
    VkPipelineLayoutCreateInfo* ci = gvk_in(&gvk_t_playout, c->x[1], 1);
    VkPipelineLayout h = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    u32 stages = 0, from = GVK_SIZE_PUSH;
    if (ci && sizes_on()) {
        /* one range a stage: the fragment stage's reaches to gvk's bytes, or gvk's is a range of its own */
        u32 n = ci->pushConstantRangeCount, room = 1, frag = n;
        VkPushConstantRange* p = gvk_scratch(sizeof *p * (n + 1));
        for (u32 i = 0; i < n; i++) {
            p[i] = ci->pPushConstantRanges[i];
            room &= (u64)p[i].offset + p[i].size <= GVK_SIZE_PUSH;
            if (p[i].stageFlags & VK_SHADER_STAGE_FRAGMENT_BIT) frag = i;
        }
        if (room) {
            if (frag == n)
                p[n] = (VkPushConstantRange){VK_SHADER_STAGE_FRAGMENT_BIT, GVK_SIZE_PUSH, 16};
            else
                p[frag].size = GVK_SIZE_PUSH + 16 - p[frag].offset;
            ci->pPushConstantRanges = p;
            ci->pushConstantRangeCount = n + (frag == n);
            stages = p[frag].stageFlags;
            from = p[frag].offset;
        } else {
            WARN_LIMIT(4,
                       "gvk: a pipeline layout's push constants reach past %u bytes: its fragment shaders' size "
                       "queries can't be told the game's size\n",
                       GVK_SIZE_PUSH);
        }
    }
    if (ci) r = vkCreatePipelineLayout(g_vk.dev, ci, NULL, &h);
    if (r == VK_SUCCESS && stages) {
        GvkLayout e = {U(h), stages, from};
        tab_put(TAB(layout), &g_gvk_api.nlayout, &e);
    }
    if (r != VK_SUCCESS) WARN_LIMIT(16, "gvk: vkCreatePipelineLayout failed (%d)\n", (int)r);
    put_handle(c->x[3], U(h));
    ret_vk(c, r);
}
/* a command buffer of gvk's own, recorded between them and run to its end before once_end returns (images_general) */
typedef struct {
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence fence;
} GvkOnce;
static VkResult once_begin(GvkOnce* o);
static VkResult once_end(GvkOnce* o, VkResult r);
static void full_barrier(VkCommandBuffer cb);
/* a region of a target, as a copy names it at the game's size */
typedef struct {
    VkImageSubresourceLayers sub;
    VkOffset3D o;
    VkExtent3D e;
} GvkRegion;
static void target_blit(VkCommandBuffer cb, u64 img, VkImage shadow, u32 f, const GvkRegion* g, u32 n, int up);
/* t's shadow at f, in GENERAL as gvk holds each image, with the pixels that the commands submitted before wrote to the
   game's image: no command buffer that can still run writes it (GvkTarget.pending). A transient attachment takes no
   transfers and holds nothing between passes */
static int shadow_make(GvkTarget* t, u32 f) {
    const VkImageCreateFlags keep =
        VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    int transient = (t->usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) != 0;
    VkImageCreateInfo ii = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = t->flags & keep,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = t->format,
        .extent = {t->w * f, t->h * f, 1},
        .mipLevels = 1,
        .arrayLayers = t->layers,
        .samples = (VkSampleCountFlagBits)t->samples,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = t->usage | (transient ? 0 : VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkImage img = VK_NULL_HANDLE;
    DevMem mem = 0;
    VkResult r = VK_SUCCESS;
    vk_lock();
    int made = vk_image_create_r(&ii, &img, &mem, VK_OWN_TARGET, &r);
    vk_unlock();
    if (!made) {
        WARN_LIMIT(8, "gvk: no %ux%u image for a target drawn at %ux (%d)\n", ii.extent.width, ii.extent.height, f,
                   (int)r);
        return 0;
    }
    VkImageAspectFlags aspects = format_aspects(t->format);
    GvkOnce o;
    r = once_begin(&o);
    if (r == VK_SUCCESS) {
        /* the shadow, and the game's image where no command put it in GENERAL: the copies through it take that */
        VkImageMemoryBarrier b[2];
        VkImage imgs[2] = {img, H(VkImage, t->img)};
        for (u32 i = 0; i < 2; i++)
            b[i] = (VkImageMemoryBarrier){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                          .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                          .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                          .image = imgs[i],
                                          .subresourceRange = {aspects, 0, 1, 0, t->layers}};
        vkCmdPipelineBarrier(o.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                             NULL, t->written || transient ? 1 : 2, b);
        if (t->written && !transient) {
            GvkRegion whole = {{aspects, 0, 0, t->layers}, {0, 0, 0}, {t->w, t->h, 1}};
            full_barrier(o.cb);
            target_blit(o.cb, t->img, img, f, &whole, 1, 1);
        }
    }
    r = once_end(&o, r);
    if (r != VK_SUCCESS) {
        WARN_LIMIT(8, "gvk: a target's image at %ux couldn't be readied (%d)\n", f, (int)r);
        vk_lock();
        vk_image_destroy(img, mem);
        vk_unlock();
        return 0;
    }
    t->shadow = img;
    t->shadow_mem = mem;
    t->scale = (u8)f;
    return 1;
}
/* a shadow no command used yet, gone */
static void shadow_free(GvkTarget* t) {
    vk_lock();
    vk_image_destroy(t->shadow, t->shadow_mem);
    vk_unlock();
    t->shadow = VK_NULL_HANDLE;
}
/* whether target t can get a shadow at f now: an attachment of one level with no storage, which a shader writes at
   the game's coordinates. Its pixels move between its image and its shadow by blits, of one sample: a multisampled
   target that isn't transient stays at the game's size, as a copy or a resolve of it to an image at the game's size
   would lose them. A format that doesn't blit (RADV blits no depth and stencil) takes a shadow where commands wrote no
   pixels yet. No command buffer that can still run uses it, no descriptor set holds a view of it, and gvk followed
   each command of it (GvkTarget) */
static int shadow_allowed_nl(const GvkTarget* t, u32 f) {
    const VkFormatFeatureFlags blit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    const GvkImage* gi = &g_gvk_api.image[tab_slot(TAB(image), t->img)];
    u32 max_w = fb_max_w(), max_h = fb_max_h();
    int transient = (t->usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) != 0;
    int moves = transient || (t->samples == VK_SAMPLE_COUNT_1_BIT &&
                              (!t->written || (gi->img == t->img && (gi->features & blit) == blit)));
    return t->mips == 1 && !(t->usage & VK_IMAGE_USAGE_STORAGE_BIT) && t->w * f <= max_w && t->h * f <= max_h &&
           moves && !t->pending && !t->in_set && !t->blocked && (t->layers == 1 || !t->written);
}
/* The n targets of one framebuffer with their shadows, and the shadow's view of each view of them: all or none. A
   target that got a shadow from another framebuffer meanwhile keeps that one, and its shadow made here goes. None
   where one lost its image, can't have a shadow any more, or got a write recorded since its shadow was made (wgen),
   which the shadow missed, or where a view fails */
static int shadows_publish(GvkTarget* t, u32 n) {
    if (!n) return 1;
    u8* dup = gvk_scratch(n);
    memset(dup, 0, n);
    pm_lock(&g_gvk_api.lock);
    int ok = 1;
    for (u32 k = 0; ok && k < n; k++) {
        const GvkTarget* e = &g_gvk_api.target[tab_slot(TAB(target), t[k].img)];
        if (e->img != t[k].img)
            ok = 0;
        else if (e->shadow)
            ok = dup[k] = e->scale == t[k].scale;
        else
            ok = shadow_allowed_nl(e, t[k].scale) && e->wgen == t[k].wgen;
    }
    u32 nv = 0;
    for (u32 i = 0; ok && i < GVK_IMAGES; i++)
        for (u32 k = 0; k < n; k++) nv += !dup[k] && g_gvk_api.view[i].view && g_gvk_api.view[i].img == t[k].img;
    u32* at = ok ? gvk_scratch(sizeof *at * (nv + 1)) : NULL;
    VkImageView* made = ok ? gvk_scratch(sizeof *made * (nv + 1)) : NULL;
    u32 m = 0;
    for (u32 i = 0; ok && i < GVK_IMAGES; i++) {
        const GvkView* v = &g_gvk_api.view[i];
        for (u32 k = 0; ok && v->view && k < n; k++) {
            if (dup[k] || v->img != t[k].img) continue;
            at[m] = i;
            made[m] = view_make(v, t[k].shadow);
            ok = made[m++] != VK_NULL_HANDLE;
        }
    }
    if (!ok) {
        for (u32 j = 0; j < m; j++)
            if (made[j]) vkDestroyImageView(g_vk.dev, made[j], NULL);
        pm_unlock(&g_gvk_api.lock);
        return 0;
    }
    for (u32 j = 0; j < m; j++) g_gvk_api.view[at[j]].shadow = made[j];
    for (u32 k = 0; k < n; k++) {
        if (dup[k]) continue;
        GvkTarget* e = &g_gvk_api.target[tab_slot(TAB(target), t[k].img)];
        e->shadow = t[k].shadow;
        e->shadow_mem = t[k].shadow_mem;
        e->scale = t[k].scale;
    }
    pm_unlock(&g_gvk_api.lock);
    for (u32 k = 0; k < n; k++) {
        if (dup[k])
            shadow_free(&t[k]);
        else
            LOG_LIMIT(64, "gvk: a %ux%u target of format %d is drawn at %ux%u\n", t[k].w, t[k].h, (int)t[k].format,
                      t[k].w * t[k].scale, t[k].h * t[k].scale);
    }
    return 1;
}
/* The scale a framebuffer of ci's attachments is drawn at, its views changed to the shadows'. Every attachment takes
   the scale or none does, as one at another scale is drawn at other pixels: each image that has no shadow yet gets
   one (shadow_allowed_nl). Otherwise the framebuffer is drawn at the game's size, and *mixed lists its targets that
   have a shadow already, whose pixels its passes move (mixed_sync). The shadows are made with no lock held, as the
   GPU's work before them must end first (shadow_make) */
static u32 fb_scale(VkFramebufferCreateInfo* ci, GvkMixed* mixed) {
    u32 n = ci->attachmentCount, f = scale_factor();
    if (f == 1 || !n || !ci->pAttachments) return 1;
    VkImageView* views = gvk_scratch(sizeof *views * n);
    memcpy(views, ci->pAttachments, sizeof *views * n);
    GvkTarget* make = gvk_scratch(sizeof *make * n);
    u32 nmake = 0;
    int ok = 1, swapchain = 0;
    pm_lock(&g_gvk_api.lock);
    for (u32 i = 0; i < n; i++) {
        const GvkView* v = &g_gvk_api.view[tab_slot(TAB(view), U(views[i]))];
        const GvkTarget* t = &g_gvk_api.target[tab_slot(TAB(target), v->img)];
        if (!views[i] || v->view != U(views[i]) || !v->img || t->img != v->img) {
            ok = 0;
            continue;
        }
        swapchain |= t->scale > 1 && !t->shadow;
        if (t->scale > 1) continue;
        if (!shadow_allowed_nl(t, f)) {
            ok = 0;
            continue;
        }
        u32 k = 0;
        while (k < nmake && make[k].img != t->img) k++;
        if (k == nmake) make[nmake++] = *t;
    }
    pm_unlock(&g_gvk_api.lock);
    u32 made = 0;
    while (ok && made < nmake && shadow_make(&make[made], f)) made++;
    ok = ok && made == nmake && shadows_publish(make, nmake);
    for (u32 k = 0; !ok && k < made; k++) shadow_free(&make[k]);
    u32 nmixed = 0;
    int lost = 0;
    pm_lock(&g_gvk_api.lock);
    for (u32 i = 0; i < n; i++) {
        const GvkView* v = &g_gvk_api.view[tab_slot(TAB(view), U(views[i]))];
        GvkTarget* t = &g_gvk_api.target[tab_slot(TAB(target), v->img)];
        if (!v->img || t->img != v->img) continue;
        if (!ok) {
            t->blocked |= !t->shadow; /* a framebuffer at the game's size writes the game's image from now on */
            /* a transient attachment holds nothing between passes, so its shadow needs none of the pixels */
            if (!t->shadow || (t->usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)) continue;
            u32 k = 0;
            while (k < nmixed && mixed->img[k] != t->img) k++;
            if (k < nmixed) continue;
            if (nmixed < GVK_CB_WRITES)
                mixed->img[nmixed++] = t->img;
            else
                lost = 1;
        } else if (t->shadow) { /* else a swapchain image, made at the scale */
            if (v->shadow)
                views[i] = v->shadow;
            else
                ok = 0;
        }
    }
    pm_unlock(&g_gvk_api.lock);
    if (!ok && (swapchain || lost))
        WARN_LIMIT(4,
                   "gvk: a framebuffer takes %s at %ux and images that can't be: it is drawn at the game's size, "
                   "without the pixels drawn at the scale\n",
                   swapchain ? "a swapchain image" : "too many images drawn", f);
    if (!ok) return 1;
    ci->pAttachments = views;
    return f;
}
/* fb's targets that have a shadow, when it is drawn at the game's size (GvkMixed): their pixels blitted from the
   shadows to the game's images (up 0, before a pass begins), or back (up 1, after it ends). Outside a pass */
static void mixed_sync(VkCommandBuffer cb, u64 fb, int up) {
    GvkMixed m;
    if (!tab_get(TAB(mixed), fb, &m)) return;
    full_barrier(cb);
    for (u32 k = 0; k < GVK_CB_WRITES && m.img[k]; k++) {
        pm_lock(&g_gvk_api.lock);
        GvkTarget t = g_gvk_api.target[tab_slot(TAB(target), m.img[k])];
        pm_unlock(&g_gvk_api.lock);
        if (t.img != m.img[k] || !t.shadow) continue;
        GvkRegion whole = {{format_aspects(t.format), 0, 0, t.layers}, {0, 0, 0}, {t.w, t.h, 1}};
        target_blit(cb, t.img, t.shadow, t.scale, &whole, 1, up);
    }
    full_barrier(cb);
}
static void gvk_CreateFramebuffer(CPU* RESTRICT c, u64 entry) {
    REQUIRE_OUT(c->x[3], 8);
    gvk_scratch_reset();
    VkFramebufferCreateInfo* ci = gvk_in(&gvk_t_fb, c->x[1], 1);
    VkFramebuffer h = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (ci) {
        GvkMixed mixed = {0};
        u32 f = fb_scale(ci, &mixed);
        ci->width *= f;
        ci->height *= f;
        r = vkCreateFramebuffer(g_vk.dev, ci, NULL, &h);
        if (r == VK_SUCCESS && f > 1) set_put(g_gvk_api.sfb, GVK_SCALED, &g_gvk_api.nsfb, U(h));
        if (r == VK_SUCCESS && mixed.img[0]) {
            mixed.fb = U(h);
            tab_put(TAB(mixed), &g_gvk_api.nmixed, &mixed);
        }
    }
    if (r != VK_SUCCESS) WARN_LIMIT(16, "gvk: vkCreateFramebuffer failed (%d)\n", (int)r);
    put_handle(c->x[3], U(h));
    ret_vk(c, r);
}
CREATE(CreateFence, VkFence, gvk_t_fence)
CREATE(CreateSemaphore, VkSemaphore, gvk_t_sem)
/* a pool of pipeline statistics answers one value for each statistic, the other types one: gvk_GetQueryPoolResults */
static void gvk_CreateQueryPool(CPU* RESTRICT c, u64 entry) {
    REQUIRE_OUT(c->x[3], 8);
    gvk_scratch_reset();
    const VkQueryPoolCreateInfo* ci = gvk_in(&gvk_t_query, c->x[1], 1);
    VkQueryPool h = VK_NULL_HANDLE;
    VkResult r = ci ? vkCreateQueryPool(g_vk.dev, ci, NULL, &h) : VK_ERROR_INITIALIZATION_FAILED;
    if (r == VK_SUCCESS && ci->queryType == VK_QUERY_TYPE_PIPELINE_STATISTICS) {
        GvkQuery q = {.pool = U(h), .values = snx_popcount64(ci->pipelineStatistics)};
        tab_put(TAB(query), &g_gvk_api.nquery, &q);
    }
    if (r != VK_SUCCESS) WARN_LIMIT(16, "gvk: vkCreateQueryPool failed (%d)\n", (int)r);
    put_handle(c->x[3], U(h));
    ret_vk(c, r);
}
CREATE(CreateEvent, VkEvent, gvk_t_event)
DESTROY(DestroyBuffer, VkBuffer)
static void gvk_DestroyImageView(CPU* RESTRICT c, u64 entry) {
    GvkView v;
    if (scale_factor() > 1 && tab_take(TAB(view), &g_gvk_api.nview, c->x[1], &v) && v.shadow)
        vkDestroyImageView(g_vk.dev, v.shadow, NULL);
    vkDestroyImageView(g_vk.dev, H(VkImageView, c->x[1]), NULL);
    ret(c, 0);
}
DESTROY(DestroyBufferView, VkBufferView)
DESTROY(DestroySampler, VkSampler)
static void gvk_DestroyShaderModule(CPU* RESTRICT c, u64 entry) {
    GvkDual e;
    GvkSizes s;
    if (tab_take(TAB(dual), &g_gvk_api.ndual, c->x[1], &e))
        vkDestroyShaderModule(g_vk.dev, H(VkShaderModule, e.dual), NULL);
    if (scale_factor() > 1 && tab_take(TAB(msizes), &g_gvk_api.nmsizes, c->x[1], &s) && s.orig)
        vkDestroyShaderModule(g_vk.dev, H(VkShaderModule, s.orig), NULL);
    vkDestroyShaderModule(g_vk.dev, H(VkShaderModule, c->x[1]), NULL);
    ret(c, 0);
}
DESTROY(DestroyPipelineCache, VkPipelineCache)
/* a layout that pipelines with size queries push through stays until they go (gvk_DestroyPipeline) */
static void gvk_DestroyPipelineLayout(CPU* RESTRICT c, u64 entry) {
    pm_lock(&g_gvk_api.lock);
    GvkLayout* l = &g_gvk_api.layout[tab_slot(TAB(layout), c->x[1])];
    int keep = c->x[1] && l->layout == c->x[1] && l->pipes;
    GvkLayout e;
    if (keep)
        l->gone = 1;
    else
        tab_take_nl(TAB(layout), &g_gvk_api.nlayout, c->x[1], &e);
    pm_unlock(&g_gvk_api.lock);
    if (!keep) vkDestroyPipelineLayout(g_vk.dev, H(VkPipelineLayout, c->x[1]), NULL);
    ret(c, 0);
}
static void gvk_DestroyDescriptorSetLayout(CPU* RESTRICT c, u64 entry) {
    GvkDsLayout e;
    if (scale_factor() > 1) tab_take(TAB(dsl), &g_gvk_api.ndsl, c->x[1], &e);
    vkDestroyDescriptorSetLayout(g_vk.dev, H(VkDescriptorSetLayout, c->x[1]), NULL);
    ret(c, 0);
}
static void gvk_DestroyFramebuffer(CPU* RESTRICT c, u64 entry) {
    GvkMixed mixed;
    set_take(g_gvk_api.sfb, GVK_SCALED, &g_gvk_api.nsfb, c->x[1]);
    tab_take(TAB(mixed), &g_gvk_api.nmixed, c->x[1], &mixed);
    vkDestroyFramebuffer(g_vk.dev, H(VkFramebuffer, c->x[1]), NULL);
    ret(c, 0);
}
static void gvk_DestroyPipeline(CPU* RESTRICT c, u64 entry) {
    GvkGraphics e;
    GvkSizes s;
    GvkLayout gone = {0};
    tab_take(TAB(graphics), &g_gvk_api.ngraphics, c->x[1], &e);
    pm_lock(&g_gvk_api.lock);
    if (c->x[1] && tab_take_nl(TAB(psizes), &g_gvk_api.npsizes, c->x[1], &s)) {
        GvkLayout* l = &g_gvk_api.layout[tab_slot(TAB(layout), s.layout)];
        if (l->layout == s.layout && l->pipes && !--l->pipes && l->gone)
            tab_take_nl(TAB(layout), &g_gvk_api.nlayout, s.layout, &gone);
    }
    pm_unlock(&g_gvk_api.lock);
    vkDestroyPipeline(g_vk.dev, H(VkPipeline, c->x[1]), NULL);
    if (gone.layout) vkDestroyPipelineLayout(g_vk.dev, H(VkPipelineLayout, gone.layout), NULL);
    ret(c, 0);
}
DESTROY(DestroyFence, VkFence)
DESTROY(DestroySemaphore, VkSemaphore)
static void gvk_DestroyQueryPool(CPU* RESTRICT c, u64 entry) {
    GvkQuery q;
    tab_take(TAB(query), &g_gvk_api.nquery, c->x[1], &q);
    vkDestroyQueryPool(g_vk.dev, H(VkQueryPool, c->x[1]), NULL);
    ret(c, 0);
}
DESTROY(DestroyEvent, VkEvent)

/* A buffer or an image of the one queue family: its sharing exclusive. Each buffer can be bound to imported memory, and
   so can each linear image whose format and usage the host imports (image_importable): the images the game maps to
   write texels. An optimal image is told of device-local memory alone (GetImageMemoryRequirements), as an external
   one would need memory of its own (requiresDedicatedAllocation) */
static void gvk_CreateBuffer(CPU* RESTRICT c, u64 entry) {
    REQUIRE_OUT(c->x[3], 8);
    gvk_scratch_reset();
    VkBufferCreateInfo* ci = gvk_in(&gvk_t_buffer, c->x[1], 1);
    VkBuffer h = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (ci) {
        VkExternalMemoryBufferCreateInfo ext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                                .pNext = ci->pNext,
                                                .handleTypes = g_gvk_api.handle_type};
        ci->pNext = &ext;
        ci->sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci->queueFamilyIndexCount = 0;
        ci->pQueueFamilyIndices = NULL;
        r = vkCreateBuffer(g_vk.dev, ci, NULL, &h);
        if (r != VK_SUCCESS)
            WARN_LIMIT(16, "gvk: a buffer of %llu bytes, usage %#x, failed (%d)\n", (unsigned long long)ci->size,
                       ci->usage, (int)r);
    }
    put_handle(c->x[3], U(h));
    ret_vk(c, r);
}
/* the dma-buf way: a dma-buf takes an image only with a DRM format modifier, so a linear image is made with the
   linear one (DRM_FORMAT_MOD_LINEAR), where the device has the extension */
static int image_modifier(void) {
    return g_gvk_api.handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT &&
           vkGetImageDrmFormatModifierPropertiesEXT != NULL;
}
static const u64 g_mod_linear = 0; /* DRM_FORMAT_MOD_LINEAR */
static int image_importable(const VkImageCreateInfo* ci) {
    if (g_gvk_api.handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT && !image_modifier()) return 0;
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .drmFormatModifier = g_mod_linear,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkPhysicalDeviceExternalImageFormatInfo ext = {.sType =
                                                       VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
                                                   .pNext = image_modifier() ? &mod : NULL,
                                                   .handleType = g_gvk_api.handle_type};
    VkPhysicalDeviceImageFormatInfo2 info = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
                                             .pNext = &ext,
                                             .format = ci->format,
                                             .type = ci->imageType,
                                             .tiling = image_modifier() ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT
                                                                        : ci->tiling,
                                             .usage = ci->usage,
                                             .flags = ci->flags};
    VkExternalImageFormatProperties ep = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 p = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &ep};
    VkExternalMemoryFeatureFlags feature =
        image_modifier() ? VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT : VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
    return vkGetPhysicalDeviceImageFormatProperties2(g_vk.phys, &info, &p) == VK_SUCCESS &&
           (ep.externalMemoryProperties.externalMemoryFeatures & feature);
}
static void gvk_CreateImage(CPU* RESTRICT c, u64 entry) {
    REQUIRE_OUT(c->x[3], 8);
    gvk_scratch_reset();
    VkImageCreateInfo* ci = gvk_in(&gvk_t_image, c->x[1], 1);
    VkImage h = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (ci) {
        ci->sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci->queueFamilyIndexCount = 0;
        ci->pQueueFamilyIndices = NULL;
        VkFormat game = ci->format;
        ci->format = host_format(game);
        VkFormat game_depth = game == ci->format ? VK_FORMAT_UNDEFINED : game;
        for (VkBaseOutStructure* n = (VkBaseOutStructure*)ci->pNext; n; n = n->pNext) {
            if (n->sType != VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) continue;
            VkImageFormatListCreateInfo* fl = (VkImageFormatListCreateInfo*)n;
            if (!fl->viewFormatCount || !fl->pViewFormats) continue;
            VkFormat* vf = gvk_scratch(sizeof *vf * fl->viewFormatCount);
            for (u32 i = 0; i < fl->viewFormatCount; i++) vf[i] = host_format(fl->pViewFormats[i]);
            fl->pViewFormats = vf;
        }
        int importable = ci->tiling == VK_IMAGE_TILING_LINEAR && image_importable(ci);
        VkImageAspectFlags preinit = 0;
        VkImageDrmFormatModifierListCreateInfoEXT mods = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
            .pNext = ci->pNext,
            .drmFormatModifierCount = 1,
            .pDrmFormatModifiers = &g_mod_linear};
        VkExternalMemoryImageCreateInfo ext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                                               .pNext = ci->pNext,
                                               .handleTypes = g_gvk_api.handle_type};
        if (importable) {
            if (ci->initialLayout == VK_IMAGE_LAYOUT_PREINITIALIZED) preinit = format_aspects(ci->format);
            if (image_modifier()) {
                ext.pNext = &mods;
                ci->tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
            }
            ci->pNext = &ext;
            ci->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        }
        /* an image that can become a target (GvkTarget): copies between its shadow and buffers or images at the game's
           size go through it, so it takes both transfers */
        int target = scale_factor() > 1 && ci->imageType == VK_IMAGE_TYPE_2D && ci->tiling == VK_IMAGE_TILING_OPTIMAL &&
                     (ci->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT));
        if (target && !(ci->usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT))
            ci->usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        r = vkCreateImage(g_vk.dev, ci, NULL, &h);
        if (r == VK_SUCCESS && target) target_track(U(h), ci, ci->extent.width, ci->extent.height, 1);
        if (r == VK_SUCCESS && (importable || scale_factor() > 1 || game_depth))
            image_track(h, ci, importable, preinit, game_depth);
        if (r != VK_SUCCESS)
            WARN_LIMIT(32, "gvk: an image of format %d, %ux%ux%u, tiling %d, usage %#x, failed (%d)\n", (int)ci->format,
                       ci->extent.width, ci->extent.height, ci->extent.depth, (int)ci->tiling, ci->usage, (int)r);
        else if (!importable && ci->tiling == VK_IMAGE_TILING_LINEAR)
            LOG_LIMIT(16, "gvk: a linear image of format %d, usage %#x can't be bound to guest memory\n",
                      (int)ci->format, ci->usage);
    }
    put_handle(c->x[3], U(h));
    ret_vk(c, r);
}
/* a target's shadow goes with it, once the GPU is done with both */
static void gvk_DestroyImage(CPU* RESTRICT c, u64 entry) {
    if (c->x[1]) {
        image_untrack(c->x[1]);
        GvkTarget t;
        if (scale_factor() > 1 && tab_take(TAB(target), &g_gvk_api.ntarget, c->x[1], &t)) {
            if (t.pending) { /* command buffers that can still run name the handle, which a new image can get */
                pm_lock(&g_gvk_api.lock);
                cb_writes_forget_nl(c->x[1]);
                pm_unlock(&g_gvk_api.lock);
            }
            if (t.shadow) retire_object((GvkRetired){.image = t.shadow, .dm = t.shadow_mem});
        }
        retire_object((GvkRetired){.image = H(VkImage, c->x[1])});
    }
    ret(c, 0);
}
/* an image of the linear DRM format modifier tells its layout by its memory plane, not its color aspect */
static void gvk_GetImageSubresourceLayout(CPU* RESTRICT c, u64 entry) {
    VkSubresourceLayout l = {0};
    if (c->x[2] && mem_user_mapped(c->x[2], sizeof(VkImageSubresource))) {
        VkImageSubresource s;
        memcpy(&s, GP(c->x[2]), sizeof s);
        if (image_modifier() && importable_has(c->x[1])) s.aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT;
        vkGetImageSubresourceLayout(g_vk.dev, H(VkImage, c->x[1]), &s, &l);
    }
    put(c->x[3], &l, sizeof l);
    ret(c, 0);
}

/* a fragment module with a second color at Location 1 has its twin made beside it (GvkDual) */
static void gvk_CreateShaderModule(CPU* RESTRICT c, u64 entry) {
    REQUIRE_OUT(c->x[3], 8);
    gvk_scratch_reset();
    VkShaderModuleCreateInfo* ci = gvk_in(&gvk_t_shader, c->x[1], 1);
    VkShaderModule h = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (ci && ci->pCode && !((uintptr_t)ci->pCode & 3) && ci->codeSize >= 5 * sizeof(u32) &&
        ci->codeSize % sizeof(u32) == 0) {
        size_t bytes;
        u32* code = gvk_scratch(ci->codeSize);
        memcpy(code, ci->pCode, ci->codeSize);
        const u32* mended = gvk_spirv_mend(code, ci->codeSize, &bytes, g_vk.gvk_nonuniform);
        if (!mended) {
            WARN_LIMIT(16, "gvk: shader is malformed or requires buffer indexing the host device did not enable\n");
            ret_vk(c, VK_ERROR_INITIALIZATION_FAILED);
            return;
        }
        /* at the resolution scale, its size queries answer the game's size (gvk_spirv_scaled_size) */
        const u32* raw_code = mended;
        size_t raw_bytes = bytes;
        GvkSizeSlots slots = {0};
        if (sizes_on()) {
            size_t sized;
            const u32* s = gvk_spirv_scaled_size(mended, bytes, &sized, &slots);
            if (slots.skipped)
                LOG_LIMIT(8,
                          "gvk: %u size queries of a shader read an image gvk can't follow: they answer the host's "
                          "size of an image drawn at the scale\n",
                          slots.skipped);
            if (s && slots.n) {
                mended = s;
                bytes = sized;
            }
        }
        ci->pCode = mended;
        ci->codeSize = bytes;
        r = vkCreateShaderModule(g_vk.dev, ci, NULL, &h);
        if (r == VK_SUCCESS && slots.n) {
            VkShaderModule orig = VK_NULL_HANDLE;
            VkShaderModuleCreateInfo raw_ci = *ci;
            raw_ci.pCode = raw_code;
            raw_ci.codeSize = raw_bytes;
            if (vkCreateShaderModule(g_vk.dev, &raw_ci, NULL, &orig) != VK_SUCCESS) orig = VK_NULL_HANDLE;
            GvkSizes e = {.key = U(h), .orig = U(orig), .n = slots.n};
            memcpy(e.slot, slots.slot, sizeof e.slot);
            tab_put(TAB(msizes), &g_gvk_api.nmsizes, &e);
        }
        const u32* dual = r == VK_SUCCESS ? gvk_spirv_dual_source(ci->pCode, ci->codeSize, &bytes) : NULL;
        VkShaderModule twin = VK_NULL_HANDLE;
        if (dual) {
            ci->pCode = dual;
            ci->codeSize = bytes;
            if (vkCreateShaderModule(g_vk.dev, ci, NULL, &twin) == VK_SUCCESS) {
                GvkDual e = {U(h), U(twin)};
                tab_put(TAB(dual), &g_gvk_api.ndual, &e);
            }
        }
    }
    if (r != VK_SUCCESS) WARN_LIMIT(16, "gvk: vkCreateShaderModule failed (%d)\n", (int)r);
    put_handle(c->x[3], U(h));
    ret_vk(c, r);
}
/* a pipeline cache of the console's driver means nothing to the host's: each one starts empty */
static void gvk_CreatePipelineCache(CPU* RESTRICT c, u64 entry) {
    REQUIRE_OUT(c->x[3], 8);
    gvk_scratch_reset();
    VkPipelineCacheCreateInfo* ci = gvk_in(&gvk_t_cache, c->x[1], 1);
    VkPipelineCache h = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (ci) {
        ci->initialDataSize = 0;
        ci->pInitialData = NULL;
        r = vkCreatePipelineCache(g_vk.dev, ci, NULL, &h);
    }
    put_handle(c->x[3], U(h));
    ret_vk(c, r);
}
static void gvk_GetPipelineCacheData(CPU* RESTRICT c, u64 entry) {
    if (!c->x[2] || !gvk_writable(c->x[2], 8)) {
        ret_vk(c, VK_ERROR_INITIALIZATION_FAILED);
        return;
    }
    size_t size = gr64(c->x[2]);
    void* data = NULL;
    if (c->x[3]) {
        if (!gvk_writable(c->x[3], size)) {
            ret_vk(c, VK_ERROR_INITIALIZATION_FAILED);
            return;
        }
        data = GP(c->x[3]);
    }
    VkResult r = vkGetPipelineCacheData(g_vk.dev, H(VkPipelineCache, c->x[1]), &size, data);
    gw64(c->x[2], size);
    ret_vk(c, r);
}
static void gvk_MergePipelineCaches(CPU* RESTRICT c, u64 entry) {
    GUEST(src, VkPipelineCache, c->x[3], (u32)c->x[2]);
    ret_vk(c, vkMergePipelineCaches(g_vk.dev, H(VkPipelineCache, c->x[1]), (u32)c->x[2], src));
}

/* the game's layout info at g, where the immutable samplers of a binding of another type are ignored, whatever the
   pointer holds. NULL when it isn't mapped */
static VkDescriptorSetLayoutCreateInfo* dslayout_in(u64 g) {
    VkDescriptorSetLayoutCreateInfo* ci = gvk_in(&gvk_t_dslayout, g, 1);
    VkDescriptorSetLayoutBinding* b = ci ? (VkDescriptorSetLayoutBinding*)ci->pBindings : NULL;
    for (u32 i = 0; b && i < ci->bindingCount; i++)
        if (b[i].descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER &&
            b[i].descriptorType != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
            b[i].pImmutableSamplers = NULL;
    return ci;
}
static void gvk_GetDescriptorSetLayoutSupport(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    const VkDescriptorSetLayoutCreateInfo* ci = dslayout_in(c->x[1]);
    VkDescriptorSetLayoutSupport s = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT};
    if (ci) vkGetDescriptorSetLayoutSupport(g_vk.dev, ci, &s);
    put(c->x[2] + offsetof(VkDescriptorSetLayoutSupport, supported), &s.supported, sizeof s.supported);
    ret(c, 0);
}
static void gvk_CreateDescriptorSetLayout(CPU* RESTRICT c, u64 entry) {
    REQUIRE_OUT(c->x[3], 8);
    gvk_scratch_reset();
    VkDescriptorSetLayoutCreateInfo* ci = dslayout_in(c->x[1]);
    VkDescriptorSetLayout h = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (ci) {
        const VkDescriptorSetLayoutBinding* b = ci->pBindings;
        r = vkCreateDescriptorSetLayout(g_vk.dev, ci, NULL, &h);
        if (r == VK_SUCCESS && scale_factor() > 1) { /* where a write past a binding's end goes on (slot_bit) */
            GvkDsLayout e = {.layout = U(h)};
            for (u32 i = 0; b && i < ci->bindingCount; i++)
                if (b[i].binding < 8) e.count[b[i].binding] = b[i].descriptorCount;
            tab_put(TAB(dsl), &g_gvk_api.ndsl, &e);
        }
    }
    if (r != VK_SUCCESS) WARN_LIMIT(16, "gvk: vkCreateDescriptorSetLayout failed (%d)\n", (int)r);
    put_handle(c->x[3], U(h));
    ret_vk(c, r);
}

/* ---- the twins of the game's descriptor sets (GvkTwinSet), under g_gvk_api.lock */
static u32 tset_bucket(u64 set, u32 cap) { return (u32)((set >> 4) * 2654435761u) & (cap - 1); }
/* the index of set's twin in g_gvk_api.tsets, 0 for none */
static u32 tset_find_nl(u64 set) {
    u32 i = g_gvk_api.nbuckets ? g_gvk_api.buckets[tset_bucket(set, g_gvk_api.nbuckets)] : 0;
    while (i && g_gvk_api.tsets[i].set != set) i = g_gvk_api.tsets[i].chain;
    return i;
}
static VkDescriptorSet twin_of_nl(VkDescriptorSet set) {
    u32 i = tset_find_nl(U(set));
    return i ? g_gvk_api.tsets[i].twin : VK_NULL_HANDLE;
}
/* the pool's entry in g_gvk_api.tpool, NULL for a pool with no twin: until the table changes */
static GvkPoolTwin* tpool_at_nl(u64 pool) {
    u32 i = tab_slot(TAB(tpool), pool);
    return pool && g_gvk_api.tpool[i].pool == pool ? &g_gvk_api.tpool[i] : NULL;
}
static void tset_drop_nl(u32 i) {
    GvkTwinSet* e = &g_gvk_api.tsets[i];
    u32* at = &g_gvk_api.buckets[tset_bucket(e->set, g_gvk_api.nbuckets)];
    while (*at != i) at = &g_gvk_api.tsets[*at].chain;
    *at = e->chain;
    if (e->prev) {
        g_gvk_api.tsets[e->prev].next = e->next;
    } else {
        GvkPoolTwin* p = tpool_at_nl(e->pool);
        if (p) p->sets = e->next;
    }
    if (e->next) g_gvk_api.tsets[e->next].prev = e->prev;
    *e = (GvkTwinSet){.next = g_gvk_api.tset_free};
    g_gvk_api.tset_free = i;
    g_gvk_api.ntset--;
}
static void tset_put_nl(u64 set, VkDescriptorSet twin, GvkPoolTwin* p) {
    u32 old = tset_find_nl(set);
    if (old) tset_drop_nl(old);
    if (g_gvk_api.ntset >= g_gvk_api.nbuckets) { /* a bucket for each set at most: twice as many */
        u32 cap = g_gvk_api.nbuckets ? 2 * g_gvk_api.nbuckets : 1024;
        u32* b = calloc(cap, sizeof *b);
        if (!b) fatal("gvk: out of memory for the twins of %u descriptor sets", g_gvk_api.ntset);
        for (u32 k = 0; k < g_gvk_api.nbuckets; k++)
            for (u32 i = g_gvk_api.buckets[k], next; i; i = next) {
                next = g_gvk_api.tsets[i].chain;
                g_gvk_api.tsets[i].chain = b[tset_bucket(g_gvk_api.tsets[i].set, cap)];
                b[tset_bucket(g_gvk_api.tsets[i].set, cap)] = i;
            }
        free(g_gvk_api.buckets);
        g_gvk_api.buckets = b;
        g_gvk_api.nbuckets = cap;
    }
    if (!g_gvk_api.tset_free) { /* slots 1 to cap - 1, linked free; slot 0 is none */
        u32 cap = g_gvk_api.tset_cap ? 2 * g_gvk_api.tset_cap : 1024;
        GvkTwinSet* t = cap > g_gvk_api.tset_cap ? realloc(g_gvk_api.tsets, sizeof *t * cap) : NULL;
        if (!t) fatal("gvk: out of memory for the twins of %u descriptor sets", g_gvk_api.ntset);
        for (u32 i = cap - 1; i >= (g_gvk_api.tset_cap ? g_gvk_api.tset_cap : 1); i--) {
            t[i] = (GvkTwinSet){.next = g_gvk_api.tset_free};
            g_gvk_api.tset_free = i;
        }
        g_gvk_api.tsets = t;
        g_gvk_api.tset_cap = cap;
    }
    u32 i = g_gvk_api.tset_free;
    u32* bucket = &g_gvk_api.buckets[tset_bucket(set, g_gvk_api.nbuckets)];
    g_gvk_api.tset_free = g_gvk_api.tsets[i].next;
    g_gvk_api.tsets[i] = (GvkTwinSet){.set = set, .pool = p->pool, .twin = twin, .chain = *bucket, .next = p->sets};
    *bucket = i;
    if (p->sets) g_gvk_api.tsets[p->sets].prev = i;
    p->sets = i;
    g_gvk_api.ntset++;
}
static void tpool_forget_nl(GvkPoolTwin* p) {
    while (p->sets) tset_drop_nl(p->sets);
}
/* ---- which descriptors of a set hold an image drawn at the scale (GvkSetScale), under g_gvk_api.lock */
/* the bit of a set's descriptor, 64 for one past the mask */
static u32 set_bit(u32 binding, u32 element) { return binding < 8 && element < 8 ? binding * 8 + element : 64; }
/* the descriptors of the bindings 0 to 7 of set's layout, NULL where gvk doesn't know them */
static const u32* set_counts_nl(u64 set) {
    const GvkSetInfo* s = &g_gvk_api.setinfo[tab_slot(TAB(setinfo), set)];
    return set && s->set == set && s->known ? s->count : NULL;
}
/* the bit of the descriptor element places from (binding, 0), past the binding's end in the next bindings, as a
   write or a copy goes on by the layout's counts (count NULL: within the binding) */
static u32 slot_bit(const u32* count, u32 binding, u32 element) {
    while (count && binding < 8 && element >= count[binding]) element -= count[binding++];
    return set_bit(binding, element);
}
static u64 set_mask_nl(u64 set) {
    const GvkSetScale* e = &g_gvk_api.setscale[tab_slot(TAB(setscale), set)];
    return set && e->set == set ? e->mask : 0;
}
/* the count of pool's sets with a mask, by d */
static void pool_masks_nl(u64 pool, int d) {
    if (!pool) return;
    GvkPoolMasks* p = &g_gvk_api.poolmasks[tab_slot(TAB(poolmasks), pool)];
    if (p->pool != pool) {
        if (d < 0 || g_gvk_api.npoolmasks >= GVK_POOLS / 2) return;
        GvkPoolMasks fresh = {pool, 0};
        tab_put_nl(TAB(poolmasks), &g_gvk_api.npoolmasks, &fresh);
        p = &g_gvk_api.poolmasks[tab_slot(TAB(poolmasks), pool)];
    }
    p->n = d < 0 && !p->n ? 0 : p->n + (u32)d;
    GvkPoolMasks gone;
    if (!p->n) tab_take_nl(TAB(poolmasks), &g_gvk_api.npoolmasks, pool, &gone);
}
/* set's mask with the bits on set and the bits off cleared. A set past the table's room keeps no mask: its size
   queries answer the host's size */
static void set_mask_put_nl(u64 set, u64 on, u64 off) {
    GvkSetScale* e = &g_gvk_api.setscale[tab_slot(TAB(setscale), set)];
    if (!set || (e->set != set && !on)) return;
    u64 mask = ((e->set == set ? e->mask : 0) & ~off) | on;
    GvkSetScale gone;
    if (!mask) {
        tab_take_nl(TAB(setscale), &g_gvk_api.nsetscale, set, &gone);
        pool_masks_nl(gone.pool, -1);
        return;
    }
    if (e->set == set) {
        e->mask = mask;
        return;
    }
    if (g_gvk_api.nsetscale >= GVK_IMAGES / 8) {
        LOG_LIMIT(4,
                  "gvk: more than %u descriptor sets hold images drawn at the scale: the others' size queries "
                  "answer the host's size\n",
                  GVK_IMAGES / 8);
        return;
    }
    const GvkSetInfo* i = &g_gvk_api.setinfo[tab_slot(TAB(setinfo), set)];
    GvkSetScale s = {set, i->set == set ? i->pool : 0, mask};
    tab_put_nl(TAB(setscale), &g_gvk_api.nsetscale, &s);
    pool_masks_nl(s.pool, 1);
}
/* a set's pool and its layout's counts, for slot_bit and sets_forget_nl, and nothing in its mask yet */
static void set_info_put_nl(u64 set, u64 pool, u64 layout) {
    set_mask_put_nl(set, 0, ~0ull);
    GvkSetInfo* s = &g_gvk_api.setinfo[tab_slot(TAB(setinfo), set)];
    if (s->set != set && g_gvk_api.nsetinfo >= GVK_IMAGES / 2) {
        LOG_LIMIT(4,
                  "gvk: more than %u descriptor sets: a write past a binding's end in the others is taken as within "
                  "it\n",
                  GVK_IMAGES / 2);
        return;
    }
    const GvkDsLayout* l = &g_gvk_api.dsl[tab_slot(TAB(dsl), layout)];
    GvkSetInfo e = {.set = set, .pool = pool, .known = layout && l->layout == layout};
    if (e.known) memcpy(e.count, l->count, sizeof e.count);
    tab_put_nl(TAB(setinfo), &g_gvk_api.nsetinfo, &e);
}
static void set_info_drop_nl(u64 set) {
    GvkSetInfo e;
    set_mask_put_nl(set, 0, ~0ull);
    tab_take_nl(TAB(setinfo), &g_gvk_api.nsetinfo, set, &e);
}
/* The sets of pool, gone with its reset or its end: their masks at once, where it has any, and their counts at its
   end or as the table fills (a reset pool's handles come back to its next sets, which take their entries over) */
static void sets_forget_nl(u64 pool, int always) {
    if (!pool) return;
    const GvkPoolMasks* p = &g_gvk_api.poolmasks[tab_slot(TAB(poolmasks), pool)];
    for (u32 i = 0; pool && p->pool == pool && i < GVK_IMAGES / 4;) {
        const GvkSetScale* s = &g_gvk_api.setscale[i];
        if (s->set && s->pool == pool)
            set_mask_put_nl(s->set, 0, ~0ull); /* the run after it moves back: the same slot again */
        else
            i++;
    }
    if (!always && g_gvk_api.nsetinfo < GVK_IMAGES / 4) return;
    for (u32 i = 0; i < GVK_IMAGES;) {
        const GvkSetInfo* s = &g_gvk_api.setinfo[i];
        if (s->set && s->pool == pool)
            set_info_drop_nl(s->set);
        else
            i++;
    }
}
/* the pool's twin: the same sizes and flags, in host memory, where sets can't be bound */
static void gvk_CreateDescriptorPool(CPU* RESTRICT c, u64 entry) {
    REQUIRE_OUT(c->x[3], 8);
    gvk_scratch_reset();
    VkDescriptorPoolCreateInfo* ci = gvk_in(&gvk_t_dspool, c->x[1], 1);
    VkDescriptorPool h = VK_NULL_HANDLE;
    VkResult r = ci ? vkCreateDescriptorPool(g_vk.dev, ci, NULL, &h) : VK_ERROR_INITIALIZATION_FAILED;
    if (r == VK_SUCCESS && g_vk.gvk_host_sets) {
        ci->flags = (ci->flags & ~(VkDescriptorPoolCreateFlags)VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT) |
                    VK_DESCRIPTOR_POOL_CREATE_HOST_ONLY_BIT_EXT;
        VkDescriptorPool twin = VK_NULL_HANDLE;
        if (vkCreateDescriptorPool(g_vk.dev, ci, NULL, &twin) == VK_SUCCESS) {
            GvkPoolTwin e = {U(h), twin, 0};
            tab_put(TAB(tpool), &g_gvk_api.ntpool, &e);
        } else {
            WARN_LIMIT(4, "gvk: a descriptor pool has no twin in host memory: copies out of its sets read the "
                          "device's\n");
        }
    }
    if (r != VK_SUCCESS) WARN_LIMIT(16, "gvk: vkCreateDescriptorPool failed (%d)\n", (int)r);
    put_handle(c->x[3], U(h));
    ret_vk(c, r);
}
static void gvk_DestroyDescriptorPool(CPU* RESTRICT c, u64 entry) {
    GvkPoolTwin e = {0};
    pm_lock(&g_gvk_api.lock);
    GvkPoolTwin* p = tpool_at_nl(c->x[1]);
    if (scale_factor() > 1) sets_forget_nl(c->x[1], 1);
    if (p) {
        tpool_forget_nl(p);
        tab_take_nl(TAB(tpool), &g_gvk_api.ntpool, c->x[1], &e);
    }
    pm_unlock(&g_gvk_api.lock);
    if (e.twin) vkDestroyDescriptorPool(g_vk.dev, e.twin, NULL);
    vkDestroyDescriptorPool(g_vk.dev, H(VkDescriptorPool, c->x[1]), NULL);
    ret(c, 0);
}
static void gvk_ResetDescriptorPool(CPU* RESTRICT c, u64 entry) {
    VkDescriptorPool twin = VK_NULL_HANDLE;
    pm_lock(&g_gvk_api.lock);
    GvkPoolTwin* p = tpool_at_nl(c->x[1]);
    if (scale_factor() > 1) sets_forget_nl(c->x[1], 0);
    if (p) {
        tpool_forget_nl(p);
        twin = p->twin;
    }
    pm_unlock(&g_gvk_api.lock);
    if (twin) vkResetDescriptorPool(g_vk.dev, twin, 0);
    ret_vk(c, vkResetDescriptorPool(g_vk.dev, H(VkDescriptorPool, c->x[1]), (VkDescriptorPoolResetFlags)c->x[2]));
}
/* a set of a pool with a twin gets its own twin there, or none when that fails: then copies read the set itself */
static void gvk_AllocateDescriptorSets(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    VkDescriptorSetAllocateInfo* ai = gvk_in(&gvk_t_dsalloc, c->x[1], 1);
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (ai && gvk_writable(c->x[2], 8ull * ai->descriptorSetCount))
        r = vkAllocateDescriptorSets(g_vk.dev, ai, gvk_ptr(c->x[2]));
    if (r != VK_SUCCESS) WARN_LIMIT(16, "gvk: vkAllocateDescriptorSets failed (%d)\n", (int)r);
    if (r == VK_SUCCESS && scale_factor() > 1) { /* a handle back from a pool reset holds nothing yet */
        const VkDescriptorSet* made = gvk_ptr(c->x[2]);
        pm_lock(&g_gvk_api.lock);
        for (u32 i = 0; i < ai->descriptorSetCount; i++)
            set_info_put_nl(U(made[i]), U(ai->descriptorPool), U(ai->pSetLayouts[i]));
        pm_unlock(&g_gvk_api.lock);
    }
    GvkPoolTwin e;
    if (r == VK_SUCCESS && ai->descriptorSetCount && tab_get(TAB(tpool), U(ai->descriptorPool), &e)) {
        const VkDescriptorSet* sets = gvk_ptr(c->x[2]);
        VkDescriptorSet* twins = gvk_scratch(sizeof *twins * ai->descriptorSetCount);
        ai->descriptorPool = e.twin;
        if (vkAllocateDescriptorSets(g_vk.dev, ai, twins) == VK_SUCCESS) {
            pm_lock(&g_gvk_api.lock);
            GvkPoolTwin* p = tpool_at_nl(e.pool);
            for (u32 i = 0; p && i < ai->descriptorSetCount; i++) tset_put_nl(U(sets[i]), twins[i], p);
            pm_unlock(&g_gvk_api.lock);
        } else {
            WARN_LIMIT(4, "gvk: descriptor sets have no twins in host memory: copies out of them read the device's\n");
        }
    }
    ret_vk(c, r);
}
static void gvk_FreeDescriptorSets(CPU* RESTRICT c, u64 entry) {
    u32 n = (u32)c->x[2];
    GUEST(sets, VkDescriptorSet, c->x[3], n);
    gvk_scratch_reset();
    VkDescriptorSet* twins = gvk_scratch(sizeof *twins * n);
    VkDescriptorPool twin_pool = VK_NULL_HANDLE;
    u32 k = 0;
    pm_lock(&g_gvk_api.lock);
    for (u32 i = 0; scale_factor() > 1 && i < n; i++) set_info_drop_nl(U(sets[i]));
    GvkPoolTwin* p = tpool_at_nl(c->x[1]);
    for (u32 i = 0; p && i < n; i++) {
        u32 t = sets[i] ? tset_find_nl(U(sets[i])) : 0;
        if (!t) continue;
        twins[k++] = g_gvk_api.tsets[t].twin;
        tset_drop_nl(t);
    }
    if (p) twin_pool = p->twin;
    pm_unlock(&g_gvk_api.lock);
    if (k) vkFreeDescriptorSets(g_vk.dev, twin_pool, k, twins);
    ret_vk(c, vkFreeDescriptorSets(g_vk.dev, H(VkDescriptorPool, c->x[1]), n, sets));
}
/* the n copies of a run, whose sources no copy of the run writes before: each reads its source's twin as it was
   before the run (src_twin; the source itself when it has none, a run of one), and is made in the destination, then
   in its twin (dst_twin) where it has one */
static void copy_run(const VkCopyDescriptorSet* cp, const VkDescriptorSet* src_twin, const VkDescriptorSet* dst_twin,
                     u32 n, VkCopyDescriptorSet* tmp) {
    for (u32 i = 0; i < n; i++) {
        tmp[i] = cp[i];
        if (src_twin[i]) tmp[i].srcSet = src_twin[i];
    }
    vkUpdateDescriptorSets(g_vk.dev, 0, NULL, n, tmp);
    u32 k = 0;
    for (u32 i = 0; i < n; i++)
        if (dst_twin[i]) {
            tmp[k] = tmp[i];
            tmp[k++].dstSet = dst_twin[i];
        }
    if (k) vkUpdateDescriptorSets(g_vk.dev, 0, NULL, k, tmp);
}
/* Writes, then copies in order, as vkUpdateDescriptorSets makes them, with the twins kept the same as their sets.
   A copy reads its source's twin, so a run of copies ends before one whose source an earlier copy of the run wrote */
static void update_with_twins(VkWriteDescriptorSet* w, u32 nw, const VkCopyDescriptorSet* cp, u32 nc) {
    VkDescriptorSet* wt = gvk_scratch(sizeof *wt * (nw + 2 * (size_t)nc));
    VkDescriptorSet *st = wt + nw, *dt = st + nc;
    pm_lock(&g_gvk_api.lock);
    for (u32 i = 0; i < nw; i++) wt[i] = twin_of_nl(w[i].dstSet);
    for (u32 i = 0; i < nc; i++) {
        st[i] = twin_of_nl(cp[i].srcSet);
        dt[i] = twin_of_nl(cp[i].dstSet);
    }
    pm_unlock(&g_gvk_api.lock);
    if (nw) vkUpdateDescriptorSets(g_vk.dev, nw, w, 0, NULL);
    u32 k = 0;
    for (u32 i = 0; i < nw; i++)
        if (wt[i]) {
            w[k] = w[i];
            w[k++].dstSet = wt[i];
        }
    if (k) vkUpdateDescriptorSets(g_vk.dev, k, w, 0, NULL);
    VkCopyDescriptorSet* tmp = gvk_scratch(sizeof *tmp * nc);
    for (u32 i = 0, end; i < nc; i = end) {
        end = i + 1;
        for (int ok = st[i] != VK_NULL_HANDLE; ok && end < nc; end += ok) {
            ok = st[end] != VK_NULL_HANDLE;
            for (u32 j = i; ok && j < end; j++) ok = cp[j].dstSet != cp[end].srcSet;
        }
        copy_run(cp + i, st + i, dt + i, end - i, tmp);
    }
}
/* A write's image views as the host draws them: a target's shadow's where it has one. A target without one gets none
   from then on (fb_scale): the set would read the game's image. The set's mask takes the views of images drawn at the
   scale */
static void views_to_host(const VkWriteDescriptorSet* w, VkDescriptorImageInfo* im) {
    u64 on = 0, off = 0;
    pm_lock(&g_gvk_api.lock);
    const u32* count = set_counts_nl(U(w->dstSet));
    for (u32 k = 0; k < w->descriptorCount; k++) {
        u32 bit = slot_bit(count, w->dstBinding, w->dstArrayElement + k);
        u64 b = bit < 64 ? 1ull << bit : 0;
        GvkView* v = &g_gvk_api.view[tab_slot(TAB(view), U(im[k].imageView))];
        GvkTarget* t = &g_gvk_api.target[tab_slot(TAB(target), v->img)];
        int known = im[k].imageView && v->view == U(im[k].imageView) && v->img && t->img == v->img;
        if (known && v->shadow) {
            im[k].imageView = v->shadow;
        } else if (known && t->scale == 1) {
            v->in_set = 1;
            t->in_set = 1;
        }
        if (known && t->scale > 1)
            on |= b;
        else
            off |= b;
    }
    set_mask_put_nl(U(w->dstSet), on, off);
    pm_unlock(&g_gvk_api.lock);
}
/* the copies' masks, in order */
static void copies_scale(const VkCopyDescriptorSet* cp, u32 nc) {
    pm_lock(&g_gvk_api.lock);
    for (u32 i = 0; g_gvk_api.nsetscale && i < nc; i++) {
        u64 src = set_mask_nl(U(cp[i].srcSet)), on = 0, off = 0;
        if (!src && !set_mask_nl(U(cp[i].dstSet))) continue;
        const u32 *sc = set_counts_nl(U(cp[i].srcSet)), *dc = set_counts_nl(U(cp[i].dstSet));
        for (u32 k = 0; k < cp[i].descriptorCount; k++) {
            u32 s = slot_bit(sc, cp[i].srcBinding, cp[i].srcArrayElement + k);
            u32 d = slot_bit(dc, cp[i].dstBinding, cp[i].dstArrayElement + k);
            if (d == 64) continue;
            if (s < 64 && (src >> s & 1))
                on |= 1ull << d;
            else
                off |= 1ull << d;
        }
        set_mask_put_nl(U(cp[i].dstSet), on, off);
    }
    pm_unlock(&g_gvk_api.lock);
}
/* writes w and copies cp, each write reading the one of its three arrays its type names (host memory the caller may
   change), its images in host_layout and its views the shadows' (views_to_host). Nothing when a write lacks its
   array (gvk_writes_take says so) */
static void writes_update(VkWriteDescriptorSet* w, u32 nw, const VkCopyDescriptorSet* cp, u32 nc) {
    /* Validate all writes before mutating any view/set tracking state. */
    if (w && !gvk_writes_take(w, nw)) return;
    for (u32 i = 0; w && i < nw; i++) {
        if (!w[i].descriptorCount || !w[i].pImageInfo || w[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER) continue;
        VkDescriptorImageInfo* im = gvk_scratch(sizeof *im * w[i].descriptorCount);
        for (u32 k = 0; k < w[i].descriptorCount; k++) {
            im[k] = w[i].pImageInfo[k];
            im[k].imageLayout = host_layout(im[k].imageLayout);
        }
        w[i].pImageInfo = im;
        if (scale_factor() > 1) views_to_host(&w[i], im);
    }
    if (scale_factor() > 1 && cp && nc) copies_scale(cp, nc);
    if (g_vk.gvk_host_sets)
        update_with_twins(w, w ? nw : 0, cp, cp ? nc : 0);
    else
        vkUpdateDescriptorSets(g_vk.dev, w ? nw : 0, w, cp ? nc : 0, cp);
}
static void gvk_UpdateDescriptorSets(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    u32 nw = (u32)c->x[1], nc = (u32)c->x[3];
    VkWriteDescriptorSet* w = gvk_in(&gvk_t_write, c->x[2], nw);
    const VkCopyDescriptorSet* cp = gvk_in(&gvk_t_copy, c->x[4], nc);
    if ((!nw || w) && (!nc || cp)) writes_update(w, nw, cp, nc);
    ret(c, 0);
}
/* A descriptor update template keeps its entries (GvkTemplate), and an update with it becomes the writes they name:
   the same path as vkUpdateDescriptorSets, so that views of targets drawn at the scale and the sets' twins follow.
   The host's template is made only to give the game a handle */
static void gvk_CreateDescriptorUpdateTemplate(CPU* RESTRICT c, u64 entry) {
    REQUIRE_OUT(c->x[3], 8);
    gvk_scratch_reset();
    const VkDescriptorUpdateTemplateCreateInfo* ci = gvk_in(&gvk_t_template, c->x[1], 1);
    VkDescriptorUpdateTemplate h = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    /* push descriptors (VK_KHR_push_descriptor) are not among the game's extensions */
    if (ci && ci->templateType == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET) {
        u32 n = ci->descriptorUpdateEntryCount;
        GvkTemplate t = {.n = n, .e = n ? malloc(sizeof *t.e * n) : NULL};
        if (n && !t.e) {
            r = VK_ERROR_OUT_OF_HOST_MEMORY;
        } else {
            for (u32 i = 0; i < n; i++) t.e[i] = ci->pDescriptorUpdateEntries[i];
            r = vkCreateDescriptorUpdateTemplate(g_vk.dev, ci, NULL, &h);
        }
        if (r == VK_SUCCESS) {
            t.tmpl = U(h);
            tab_put(TAB(tmpl), &g_gvk_api.ntmpl, &t);
        } else {
            free(t.e);
        }
    }
    if (r != VK_SUCCESS) WARN_LIMIT(16, "gvk: vkCreateDescriptorUpdateTemplate failed (%d)\n", (int)r);
    put_handle(c->x[3], U(h));
    ret_vk(c, r);
}
static void gvk_DestroyDescriptorUpdateTemplate(CPU* RESTRICT c, u64 entry) {
    GvkTemplate t;
    if (tab_take(TAB(tmpl), &g_gvk_api.ntmpl, c->x[1], &t)) free(t.e);
    vkDestroyDescriptorUpdateTemplate(g_vk.dev, H(VkDescriptorUpdateTemplate, c->x[1]), NULL);
    ret(c, 0);
}
/* the descriptors of entry e at the game's data: count records of n bytes each, stride apart, copied to host memory.
   NULL when one is not mapped */
static void* template_records(u64 data, const VkDescriptorUpdateTemplateEntry* e, size_t n) {
    u8* out = gvk_scratch(n * e->descriptorCount);
    for (u32 k = 0; k < e->descriptorCount; k++) {
        u64 at = data + e->offset + (u64)k * e->stride; /* wraps as the game's own pointer arithmetic does */
        if (!mem_user_mapped(at, n)) return NULL;
        memcpy(out + n * k, GP(at), n);
    }
    return out;
}
static void gvk_UpdateDescriptorSetWithTemplate(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    GvkTemplate t;
    if (!tab_get(TAB(tmpl), c->x[2], &t)) fatal("gvk: invalid descriptor update template handle");
    VkWriteDescriptorSet* w = gvk_scratch(sizeof *w * (t.n ? t.n : 1));
    for (u32 i = 0; i < t.n; i++) {
        const VkDescriptorUpdateTemplateEntry* e = &t.e[i];
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .dstSet = H(VkDescriptorSet, c->x[1]),
                                      .dstBinding = e->dstBinding,
                                      .dstArrayElement = e->dstArrayElement,
                                      .descriptorCount = e->descriptorCount,
                                      .descriptorType = e->descriptorType};
        switch (e->descriptorType) {
            case VK_DESCRIPTOR_TYPE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                w[i].pImageInfo = template_records(c->x[3], e, sizeof(VkDescriptorImageInfo));
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                w[i].pTexelBufferView = template_records(c->x[3], e, sizeof(VkBufferView));
                break;
            default: w[i].pBufferInfo = template_records(c->x[3], e, sizeof(VkDescriptorBufferInfo)); break;
        }
    }
    writes_update(w, t.n, NULL, 0);
    ret(c, 0);
}

static int blends_src1(VkBlendFactor f) {
    return f >= VK_BLEND_FACTOR_SRC1_COLOR && f <= VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
}
/* A pipeline that blends with the second color takes its fragment module's twin (GvkDual). The host's compiles leave
   the game's core to its other threads (kthread_host_wait_begin): the game makes thousands of pipelines as it starts */
static void gvk_CreateGraphicsPipelines(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    u32 n = (u32)c->x[2];
    VkGraphicsPipelineCreateInfo* ci = gvk_in(&gvk_t_gpipe, c->x[3], n);
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (ci && gvk_writable(c->x[5], 8ull * n)) {
        u64* frag = gvk_scratch(sizeof *frag * n); /* each pipeline's fragment module as the game named it */
        memset(frag, 0, sizeof *frag * n);
        for (u32 i = 0; i < n; i++) { /* the tessellation state is read only with its stages */
            int tess = 0, src1 = 0;
            for (u32 s = 0; ci[i].pStages && s < ci[i].stageCount; s++)
                tess |= (ci[i].pStages[s].stage & VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT) != 0;
            if (!tess) ci[i].pTessellationState = NULL;
            if (tess && !ci[i].pTessellationState) {
                ret_vk(c, VK_ERROR_INITIALIZATION_FAILED);
                return;
            }
            const VkPipelineColorBlendStateCreateInfo* b = ci[i].pColorBlendState;
            for (u32 a = 0; b && b->pAttachments && a < b->attachmentCount; a++) {
                const VkPipelineColorBlendAttachmentState* at = &b->pAttachments[a];
                src1 |=
                    at->blendEnable && (blends_src1(at->srcColorBlendFactor) || blends_src1(at->dstColorBlendFactor) ||
                                        blends_src1(at->srcAlphaBlendFactor) || blends_src1(at->dstAlphaBlendFactor));
            }
            VkPipelineShaderStageCreateInfo* st = (VkPipelineShaderStageCreateInfo*)ci[i].pStages;
            GvkDual d;
            for (u32 s = 0; st && s < ci[i].stageCount; s++)
                if (st[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT) frag[i] = U(st[s].module);
            for (u32 s = 0; src1 && st && s < ci[i].stageCount; s++)
                if (st[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT && tab_get(TAB(dual), U(st[s].module), &d))
                    st[s].module = H(VkShaderModule, d.dual);
            /* If the layout has no push-constant room for scaled size queries, use the unrewritten module. */
            if (scale_factor() > 1 && frag[i]) {
                GvkSizes ms;
                GvkLayout lay;
                if (tab_get(TAB(msizes), frag[i], &ms) && ms.orig &&
                    (!tab_get(TAB(layout), U(ci[i].layout), &lay) || lay.gone)) {
                    for (u32 s = 0; st && s < ci[i].stageCount; s++)
                        if (st[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT) st[s].module = H(VkShaderModule, ms.orig);
                }
            }
            /* a viewport baked into the pipeline isn't scaled: which target it draws to is the render pass's */
            if (scale_factor() > 1 && ci[i].pViewportState && ci[i].pViewportState->pViewports) {
                int dynamic = 0;
                for (u32 d = 0; ci[i].pDynamicState && d < ci[i].pDynamicState->dynamicStateCount; d++)
                    dynamic |= ci[i].pDynamicState->pDynamicStates[d] == VK_DYNAMIC_STATE_VIEWPORT;
                if (!dynamic) LOG_LIMIT(4, "gvk: a pipeline's own viewport isn't scaled\n");
            }
        }
        kthread_host_wait_begin();
        int entered = snx_vk_compile_enter();
        r = vkCreateGraphicsPipelines(g_vk.dev, H(VkPipelineCache, c->x[1]), n, ci, NULL, gvk_ptr(c->x[5]));
        snx_vk_compile_leave(entered);
        kthread_host_wait_end();
        const VkPipeline* made = gvk_ptr(c->x[5]);
        for (u32 i = 0; scale_factor() > 1 && i < n; i++) {
            if (!made[i]) continue;
            u32 dynamic = 0;
            const VkPipelineDynamicStateCreateInfo* state = ci[i].pDynamicState;
            for (u32 d = 0; state && d < state->dynamicStateCount; d++) {
                if (state->pDynamicStates[d] == VK_DYNAMIC_STATE_VIEWPORT) dynamic |= DYN_VIEWPORT;
                if (state->pDynamicStates[d] == VK_DYNAMIC_STATE_SCISSOR) dynamic |= DYN_SCISSOR;
            }
            if (dynamic) {
                GvkGraphics e = {U(made[i]), dynamic};
                tab_put(TAB(graphics), &g_gvk_api.ngraphics, &e);
            }
            /* its fragment module's size queries, and the layout and stages their bits are pushed with: the layout
               stays while the pipeline does (gvk_DestroyPipelineLayout) */
            pm_lock(&g_gvk_api.lock);
            GvkLayout* lay = &g_gvk_api.layout[tab_slot(TAB(layout), U(ci[i].layout))];
            const GvkSizes* m = &g_gvk_api.msizes[tab_slot(TAB(msizes), frag[i])];
            if (frag[i] && m->key == frag[i] && lay->layout == U(ci[i].layout) && !lay->gone) {
                GvkSizes sz = *m;
                sz.key = U(made[i]);
                sz.layout = lay->layout;
                sz.stages = lay->stages;
                tab_put_nl(TAB(psizes), &g_gvk_api.npsizes, &sz);
                lay->pipes++;
            }
            pm_unlock(&g_gvk_api.lock);
        }
    }
    if (r != VK_SUCCESS) WARN_LIMIT(16, "gvk: vkCreateGraphicsPipelines failed (%d)\n", (int)r);
    ret_vk(c, r);
}
static void gvk_CreateComputePipelines(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    u32 n = (u32)c->x[2];
    const VkComputePipelineCreateInfo* ci = gvk_in(&gvk_t_cpipe, c->x[3], n);
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (ci && gvk_writable(c->x[5], 8ull * n)) {
        kthread_host_wait_begin();
        int entered = snx_vk_compile_enter();
        r = vkCreateComputePipelines(g_vk.dev, H(VkPipelineCache, c->x[1]), n, ci, NULL, gvk_ptr(c->x[5]));
        snx_vk_compile_leave(entered);
        kthread_host_wait_end();
    }
    if (r != VK_SUCCESS) WARN_LIMIT(16, "gvk: vkCreateComputePipelines failed (%d)\n", (int)r);
    ret_vk(c, r);
}

/* ------------------------------------------------------------ command buffers */
/* the game's pools are of its one family: the renderer's graphics family */
static void gvk_CreateCommandPool(CPU* RESTRICT c, u64 entry) {
    REQUIRE_OUT(c->x[3], 8);
    gvk_scratch_reset();
    VkCommandPoolCreateInfo* ci = gvk_in(&gvk_t_pool, c->x[1], 1);
    VkCommandPool h = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (ci) {
        ci->queueFamilyIndex = g_vk.gfx_family;
        pm_lock(&g_gvk_api.lock);
        r = VK_ERROR_OUT_OF_HOST_MEMORY;
        if (g_gvk_api.npool < GVK_CBS - 1) {
            r = vkCreateCommandPool(g_vk.dev, ci, NULL, &h);
            if (r == VK_SUCCESS) {
                u64 k = U(h);
                tab_put_nl(TAB(pool), &g_gvk_api.npool, &k);
            }
        }
        pm_unlock(&g_gvk_api.lock);
    }
    put_handle(c->x[3], U(h));
    ret_vk(c, r);
}
static void cb_clear_nl(u64 cb) {
    GvkPass p;
    GvkDyn d;
    GvkBound b;
    tab_take_nl(TAB(pass), &g_gvk_api.npass, cb, &p);
    tab_take_nl(TAB(dyn), &g_gvk_api.ndyn, cb, &d);
    tab_take_nl(TAB(bound), &g_gvk_api.nbound, cb, &b);
    cb_writes_drop_nl(cb);
}
static void pool_clear(u64 pool, int destroy) {
    pm_lock(&g_gvk_api.lock);
    for (u32 i = 0; i < GVK_CBS;) {
        GvkCmd* cmd = &g_gvk_api.cmd[i];
        if (cmd->cb && cmd->pool == pool) {
            cb_clear_nl(cmd->cb);
            if (destroy) {
                GvkCmd gone;
                tab_take_nl(TAB(cmd), &g_gvk_api.ncmd, cmd->cb, &gone);
                continue;
            }
            cmd->inherited_scale = 0;
            cmd->dynamic = 0;
        }
        i++;
    }
    pm_unlock(&g_gvk_api.lock);
}
static VkCommandBuffer checked_cb(u64 h) {
    GvkCmd e;
    if (!tab_get(TAB(cmd), h, &e)) fatal("gvk: invalid command buffer handle");
    return H(VkCommandBuffer, e.cb);
}
static void gvk_DestroyCommandPool(CPU* RESTRICT c, u64 entry) {
    if (c->x[1]) {
        u64 pool;
        if (!tab_take(TAB(pool), &g_gvk_api.npool, c->x[1], &pool)) fatal("gvk: invalid command pool handle");
        pool_clear(pool, 1);
    }
    vkDestroyCommandPool(g_vk.dev, H(VkCommandPool, c->x[1]), NULL);
    ret(c, 0);
}
static void gvk_ResetCommandPool(CPU* RESTRICT c, u64 entry) {
    if (!set_has(g_gvk_api.pool, GVK_CBS, c->x[1])) {
        ret_vk(c, VK_ERROR_INITIALIZATION_FAILED);
        return;
    }
    VkResult r = vkResetCommandPool(g_vk.dev, H(VkCommandPool, c->x[1]), (VkCommandPoolResetFlags)c->x[2]);
    if (r == VK_SUCCESS) pool_clear(c->x[1], 0);
    ret_vk(c, r);
}
static void gvk_TrimCommandPool(CPU* RESTRICT c, u64 entry) {
    vkTrimCommandPool(g_vk.dev, H(VkCommandPool, c->x[1]), (VkCommandPoolTrimFlags)c->x[2]);
    ret(c, 0);
}
static void gvk_AllocateCommandBuffers(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    const VkCommandBufferAllocateInfo* ai = gvk_in(&gvk_t_cballoc, c->x[1], 1);
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (ai && ai->commandBufferCount && !(c->x[2] & 7) && gvk_writable(c->x[2], 8ull * ai->commandBufferCount) &&
        set_has(g_gvk_api.pool, GVK_CBS, U(ai->commandPool))) {
        VkCommandBuffer* made = gvk_ptr(c->x[2]);
        pm_lock(&g_gvk_api.lock);
        r = VK_ERROR_OUT_OF_HOST_MEMORY;
        if (ai->commandBufferCount < GVK_CBS - g_gvk_api.ncmd) {
            r = vkAllocateCommandBuffers(g_vk.dev, ai, made);
            if (r == VK_SUCCESS)
                for (u32 i = 0; i < ai->commandBufferCount; i++) {
                    GvkCmd e = {.cb = U(made[i]), .pool = U(ai->commandPool), .level = ai->level};
                    tab_put_nl(TAB(cmd), &g_gvk_api.ncmd, &e);
                }
        }
        pm_unlock(&g_gvk_api.lock);
    }
    ret_vk(c, r);
}
static void pass_forget(u64 cb) {
    GvkPass p;
    tab_take(TAB(pass), &g_gvk_api.npass, cb, &p);
}
static void gvk_FreeCommandBuffers(CPU* RESTRICT c, u64 entry) {
    u32 n = (u32)c->x[2];
    GUEST(cbs, VkCommandBuffer, c->x[3], n);
    if (!set_has(g_gvk_api.pool, GVK_CBS, c->x[1])) fatal("gvk: invalid command pool handle");
    gvk_scratch_reset();
    VkCommandBuffer* copy = n ? gvk_scratch(n * sizeof *copy) : NULL;
    if (n) memcpy(copy, cbs, n * sizeof *copy);
    for (u32 i = 0; i < n; i++) {
        GvkCmd e;
        if (copy[i] && (!tab_get(TAB(cmd), U(copy[i]), &e) || e.pool != c->x[1]))
            fatal("gvk: invalid command buffer or pool in vkFreeCommandBuffers");
    }
    pm_lock(&g_gvk_api.lock);
    for (u32 i = 0; i < n; i++) {
        GvkCmd e;
        if (!copy[i]) continue;
        cb_clear_nl(U(copy[i]));
        tab_take_nl(TAB(cmd), &g_gvk_api.ncmd, U(copy[i]), &e);
    }
    pm_unlock(&g_gvk_api.lock);
    vkFreeCommandBuffers(g_vk.dev, H(VkCommandPool, c->x[1]), n, copy);
    ret(c, 0);
}
static void gvk_BeginCommandBuffer(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    VkCommandBuffer cb = checked_cb(c->x[0]);
    GvkCmd cmd;
    tab_get(TAB(cmd), c->x[0], &cmd);
    VkCommandBufferBeginInfo* bi = gvk_in(&gvk_t_begin, c->x[1], 1);
    u32 inherited_scale = 0, inherited_subpass = 0;
    u64 inherited_rp = 0;
    if (bi && cmd.level == VK_COMMAND_BUFFER_LEVEL_PRIMARY) bi->pInheritanceInfo = NULL;
    if (bi && cmd.level == VK_COMMAND_BUFFER_LEVEL_SECONDARY && !bi->pInheritanceInfo) bi = NULL;
    if (bi && bi->pInheritanceInfo && (bi->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT)) {
        inherited_scale = set_has(g_gvk_api.sfb, GVK_SCALED, U(bi->pInheritanceInfo->framebuffer)) ? scale_factor() : 1;
        inherited_rp = U(bi->pInheritanceInfo->renderPass);
        inherited_subpass = bi->pInheritanceInfo->subpass;
    }
    VkResult r = bi ? vkBeginCommandBuffer(cb, bi) : VK_ERROR_INITIALIZATION_FAILED;
    if (r == VK_SUCCESS) {
        pm_lock(&g_gvk_api.lock);
        cb_clear_nl(cmd.cb);
        cmd.inherited_scale = inherited_scale;
        cmd.inherited_rp = inherited_rp;
        cmd.inherited_subpass = inherited_subpass;
        cmd.dynamic = 0;
        cmd.writes = 0;
        tab_put_nl(TAB(cmd), &g_gvk_api.ncmd, &cmd);
        pm_unlock(&g_gvk_api.lock);
    }
    ret_vk(c, r);
}
static void gvk_EndCommandBuffer(CPU* RESTRICT c, u64 entry) { ret_vk(c, vkEndCommandBuffer(checked_cb(c->x[0]))); }
static void gvk_ResetCommandBuffer(CPU* RESTRICT c, u64 entry) {
    VkResult r = vkResetCommandBuffer(checked_cb(c->x[0]), (VkCommandBufferResetFlags)c->x[1]);
    if (r == VK_SUCCESS) {
        pm_lock(&g_gvk_api.lock);
        u32 i = tab_slot(TAB(cmd), c->x[0]);
        g_gvk_api.cmd[i].inherited_scale = 0;
        g_gvk_api.cmd[i].dynamic = 0;
        cb_clear_nl(c->x[0]);
        pm_unlock(&g_gvk_api.lock);
    }
    ret_vk(c, r);
}

/* Every command before it done and its writes seen by every command after it */
static void full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                         .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                         .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, NULL,
                         0, NULL);
}
/* A transfer command must run outside a pass. Only a primary single-subpass pass can resume. */
static int pass_out(VkCommandBuffer cb, GvkPass* p) {
    GvkRpass rp;
    if (!tab_get(TAB(pass), U(cb), p)) {
        GvkCmd cmd;
        if (tab_get(TAB(cmd), U(cb), &cmd) && cmd.inherited_scale)
            fatal("gvk: cannot split a render pass in a secondary command buffer");
        return 0;
    }
    if (!tab_get(TAB(rpass), p->rp, &rp) || rp.subpasses != 1 || !rp.resume) {
        fatal("gvk: cannot split this render pass for a transfer command or barrier");
    }
    vkCmdEndRenderPass(cb);
    full_barrier(cb);
    return 1;
}
static void pass_back(VkCommandBuffer cb, const GvkPass* p) {
    GvkRpass rp;
    tab_get(TAB(rpass), p->rp, &rp);
    full_barrier(cb);
    VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                .renderPass = H(VkRenderPass, rp.resume),
                                .framebuffer = H(VkFramebuffer, p->fb),
                                .renderArea = p->area};
    vkCmdBeginRenderPass(cb, &bi, p->contents);
}
/* a command in a render pass's place: outside it (pass_out, pass_back) */
#define OUTSIDE(cb, CALL)                                                                                              \
    do {                                                                                                               \
        GvkPass outside_pass;                                                                                          \
        int outside_ended = pass_out((cb), &outside_pass);                                                             \
        CALL;                                                                                                          \
        if (outside_ended) pass_back((cb), &outside_pass);                                                             \
    } while (0)

#define CB checked_cb(c->x[0])
static void gvk_CmdBindPipeline(CPU* RESTRICT c, u64 entry) {
    vkCmdBindPipeline(CB, (VkPipelineBindPoint)(u32)c->x[1], H(VkPipeline, c->x[2]));
    if ((u32)c->x[1] == VK_PIPELINE_BIND_POINT_GRAPHICS && scale_factor() > 1) {
        GvkGraphics e;
        tab_get(TAB(graphics), c->x[2], &e);
        pm_lock(&g_gvk_api.lock);
        u32 i = tab_slot(TAB(cmd), c->x[0]);
        g_gvk_api.cmd[i].dynamic = e.dynamic;
        u32 d = tab_slot(TAB(dyn), c->x[0]);
        if (!(e.dynamic & DYN_VIEWPORT)) g_gvk_api.dyn[d].has_vp = 0;
        if (!(e.dynamic & DYN_SCISSOR)) g_gvk_api.dyn[d].has_sc = 0;
        GvkBound* b = &g_gvk_api.bound[tab_slot(TAB(bound), c->x[0])];
        if (b->cb != c->x[0]) {
            GvkBound fresh = {.cb = c->x[0]};
            tab_put_nl(TAB(bound), &g_gvk_api.nbound, &fresh);
            b = &g_gvk_api.bound[tab_slot(TAB(bound), c->x[0])];
        }
        b->pipe = c->x[2];
        pm_unlock(&g_gvk_api.lock);
    }
    ret(c, 0);
}
static void gvk_CmdBindIndexBuffer(CPU* RESTRICT c, u64 entry) {
    vkCmdBindIndexBuffer(CB, H(VkBuffer, c->x[1]), c->x[2], (VkIndexType)(u32)c->x[3]);
    ret(c, 0);
}
static void gvk_CmdBindVertexBuffers(CPU* RESTRICT c, u64 entry) {
    u32 n = (u32)c->x[2];
    GUEST(bufs, VkBuffer, c->x[3], n);
    GUEST(offs, VkDeviceSize, c->x[4], n);
    vkCmdBindVertexBuffers(CB, (u32)c->x[1], n, bufs, offs);
    ret(c, 0);
}
static void gvk_CmdBindDescriptorSets(CPU* RESTRICT c, u64 entry) {
    GUEST(sets, VkDescriptorSet, c->x[5], (u32)c->x[4]);
    GUEST(dyn, u32, c->x[7], (u32)c->x[6]);
    vkCmdBindDescriptorSets(CB, (VkPipelineBindPoint)(u32)c->x[1], H(VkPipelineLayout, c->x[2]), (u32)c->x[3],
                            (u32)c->x[4], sets, (u32)c->x[6], dyn);
    /* the masks of the sets a draw's size queries read (sizes_push) */
    if ((u32)c->x[1] == VK_PIPELINE_BIND_POINT_GRAPHICS && scale_factor() > 1) {
        pm_lock(&g_gvk_api.lock);
        GvkBound* b = &g_gvk_api.bound[tab_slot(TAB(bound), c->x[0])];
        if (b->cb != c->x[0]) {
            GvkBound fresh = {.cb = c->x[0]};
            tab_put_nl(TAB(bound), &g_gvk_api.nbound, &fresh);
            b = &g_gvk_api.bound[tab_slot(TAB(bound), c->x[0])];
        }
        u32 first = (u32)c->x[3], count = (u32)c->x[4];
        if (first < 4)
            for (u32 j = 0; j < count && j < 4 - first; j++) b->mask[first + j] = set_mask_nl(U(sets[j]));
        pm_unlock(&g_gvk_api.lock);
    }
    ret(c, 0);
}
/* A push into the fragment stage's range, which gvk widened to its bytes (GvkLayout), names that range's stages too,
   as Vulkan asks of a push into a range: one game pushes a geometry stage's constants there */
static void gvk_CmdPushConstants(CPU* RESTRICT c, u64 entry) {
    GUEST(values, u8, c->x[5], (u32)c->x[4]);
    VkShaderStageFlags stages = (VkShaderStageFlags)c->x[2];
    u32 offset = (u32)c->x[3], size = (u32)c->x[4];
    VkCommandBuffer cb = CB;
    VkPipelineLayout lay = H(VkPipelineLayout, c->x[1]);
    GvkLayout l;
    if (scale_factor() > 1 && tab_get(TAB(layout), c->x[1], &l) && (u64)offset + size > l.from) {
        if (offset < l.from) {
            u32 head = l.from - offset;
            vkCmdPushConstants(cb, lay, stages, offset, head, values);
            vkCmdPushConstants(cb, lay, stages | l.stages, l.from, size - head, values + head);
            ret(c, 0);
            return;
        }
        stages |= l.stages;
    }
    vkCmdPushConstants(cb, lay, stages, offset, size, values);
    ret(c, 0);
}
/* the factor the commands of cb draw at now: its render pass's framebuffer's */
static u32 pass_scale(u64 cb) {
    GvkPass p;
    if (scale_factor() == 1) return 1;
    if (tab_get(TAB(pass), cb, &p)) return p.scaled ? scale_factor() : 1;
    GvkCmd cmd;
    return tab_get(TAB(cmd), cb, &cmd) && cmd.inherited_scale ? cmd.inherited_scale : 1;
}
static void set_viewports(VkCommandBuffer cb, u32 first, u32 n, const VkViewport* v, u32 f) {
    VkViewport* s = f > 1 ? gvk_scratch(sizeof *s * n) : NULL;
    for (u32 i = 0; s && i < n; i++) s[i] = viewport_at(v[i], f);
    vkCmdSetViewport(cb, first, n, s ? s : v);
}
static void set_scissors(VkCommandBuffer cb, u32 first, u32 n, const VkRect2D* r, u32 f) {
    VkRect2D* s = f > 1 ? gvk_scratch(sizeof *s * n) : NULL;
    for (u32 i = 0; s && i < n; i++) s[i] = gvk_rect_at(r[i], f);
    vkCmdSetScissor(cb, first, n, s ? s : r);
}
/* at the scale of the render pass it is in, the first one kept for a pass at another scale (dyn_again) */
static void gvk_CmdSetViewport(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    u32 first = (u32)c->x[1], n = (u32)c->x[2];
    const VkViewport* v = n && mem_user_mapped(c->x[3], sizeof *v * n) ? gvk_ptr(c->x[3]) : NULL;
    if (!v) {
        ret(c, 0);
        return;
    }
    u32 f = pass_scale(c->x[0]);
    set_viewports(CB, first, n, v, f);
    if (scale_factor() > 1 && first == 0) {
        GvkDyn d;
        tab_get(TAB(dyn), c->x[0], &d);
        d.cb = c->x[0];
        d.vp = v[0];
        d.has_vp = 1;
        d.vp_at = f;
        tab_put(TAB(dyn), &g_gvk_api.ndyn, &d);
    }
    ret(c, 0);
}
static void gvk_CmdSetScissor(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    u32 first = (u32)c->x[1], n = (u32)c->x[2];
    const VkRect2D* r = n && mem_user_mapped(c->x[3], sizeof *r * n) ? gvk_ptr(c->x[3]) : NULL;
    if (!r) {
        ret(c, 0);
        return;
    }
    u32 f = pass_scale(c->x[0]);
    set_scissors(CB, first, n, r, f);
    if (scale_factor() > 1 && first == 0) {
        GvkDyn d;
        tab_get(TAB(dyn), c->x[0], &d);
        d.cb = c->x[0];
        d.sc = r[0];
        d.has_sc = 1;
        d.sc_at = f;
        tab_put(TAB(dyn), &g_gvk_api.ndyn, &d);
    }
    ret(c, 0);
}
/* a render pass at scale f: the viewport and scissor set at another one set again at f */
static void dyn_again(VkCommandBuffer cb) {
    if (scale_factor() == 1) return;
    GvkCmd cmd;
    if (!tab_get(TAB(cmd), U(cb), &cmd) || !cmd.dynamic) return;
    GvkPass p;
    if (tab_get(TAB(pass), U(cb), &p) && p.contents == VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS) return;
    u32 f = pass_scale(U(cb));
    GvkDyn d;
    if (!tab_get(TAB(dyn), U(cb), &d)) return;
    int changed = 0;
    if ((cmd.dynamic & DYN_VIEWPORT) && d.has_vp && d.vp_at != f) {
        VkViewport vp = f > 1 ? viewport_at(d.vp, f) : d.vp;
        vkCmdSetViewport(cb, 0, 1, &vp);
        d.vp_at = f;
        changed = 1;
    }
    if ((cmd.dynamic & DYN_SCISSOR) && d.has_sc && d.sc_at != f) {
        VkRect2D sc = f > 1 ? gvk_rect_at(d.sc, f) : d.sc;
        vkCmdSetScissor(cb, 0, 1, &sc);
        d.sc_at = f;
        changed = 1;
    }
    if (changed) tab_put(TAB(dyn), &g_gvk_api.ndyn, &d);
}
/* The size bits of the bound graphics pipeline's queries (GvkSizes) from its sets' masks, pushed where they differ
   from the last pushed with its layout */
static void sizes_push(VkCommandBuffer cb) {
    pm_lock(&g_gvk_api.lock);
    GvkBound* b = &g_gvk_api.bound[tab_slot(TAB(bound), U(cb))];
    const GvkSizes* s = &g_gvk_api.psizes[tab_slot(TAB(psizes), b->pipe)];
    if (b->cb != U(cb) || !b->pipe || s->key != b->pipe) {
        pm_unlock(&g_gvk_api.lock);
        return;
    }
    u32 bits = 0;
    for (u32 k = 0; k < s->n; k++) {
        u32 bit = set_bit(s->slot[k] >> 8 & 63, s->slot[k] & 255);
        if (bit < 64 && (b->mask[s->slot[k] >> 14] >> bit & 1)) bits |= 1u << k;
    }
    VkPipelineLayout layout = H(VkPipelineLayout, s->layout);
    VkShaderStageFlags stages = s->stages;
    int push = b->pushed_layout != s->layout || b->pushed != bits;
    b->pushed_layout = s->layout;
    b->pushed = bits;
    pm_unlock(&g_gvk_api.lock);
    const u32 values[4] = {bits, scale_factor() - 1, 0, 0};
    if (push) vkCmdPushConstants(cb, layout, stages, GVK_SIZE_PUSH, sizeof values, values);
}
/* before a draw: the viewport and scissor at its render pass's scale, and its size queries' bits */
static void draw_scale(VkCommandBuffer cb) {
    if (scale_factor() == 1) return;
    dyn_again(cb);
    sizes_push(cb);
}
static void gvk_CmdSetLineWidth(CPU* RESTRICT c, u64 entry) {
    vkCmdSetLineWidth(CB, c->v[0].f[0]);
    ret(c, 0);
}
static void gvk_CmdSetDepthBias(CPU* RESTRICT c, u64 entry) {
    vkCmdSetDepthBias(CB, c->v[0].f[0], c->v[1].f[0], c->v[2].f[0]);
    ret(c, 0);
}
static void gvk_CmdSetBlendConstants(CPU* RESTRICT c, u64 entry) {
    GUEST(k, float, c->x[1], 4);
    vkCmdSetBlendConstants(CB, k);
    ret(c, 0);
}
static void gvk_CmdSetDepthBounds(CPU* RESTRICT c, u64 entry) {
    vkCmdSetDepthBounds(CB, c->v[0].f[0], c->v[1].f[0]);
    ret(c, 0);
}
static void gvk_CmdSetStencilCompareMask(CPU* RESTRICT c, u64 entry) {
    vkCmdSetStencilCompareMask(CB, (VkStencilFaceFlags)c->x[1], (u32)c->x[2]);
    ret(c, 0);
}
static void gvk_CmdSetStencilWriteMask(CPU* RESTRICT c, u64 entry) {
    vkCmdSetStencilWriteMask(CB, (VkStencilFaceFlags)c->x[1], (u32)c->x[2]);
    ret(c, 0);
}
static void gvk_CmdSetStencilReference(CPU* RESTRICT c, u64 entry) {
    vkCmdSetStencilReference(CB, (VkStencilFaceFlags)c->x[1], (u32)c->x[2]);
    ret(c, 0);
}
static void gvk_CmdDraw(CPU* RESTRICT c, u64 entry) {
    VkCommandBuffer cb = CB;
    draw_scale(cb);
    vkCmdDraw(cb, (u32)c->x[1], (u32)c->x[2], (u32)c->x[3], (u32)c->x[4]);
    ret(c, 0);
}
static void gvk_CmdDrawIndexed(CPU* RESTRICT c, u64 entry) {
    VkCommandBuffer cb = CB;
    draw_scale(cb);
    vkCmdDrawIndexed(cb, (u32)c->x[1], (u32)c->x[2], (u32)c->x[3], (i32)(u32)c->x[4], (u32)c->x[5]);
    ret(c, 0);
}
static void gvk_CmdDrawIndirect(CPU* RESTRICT c, u64 entry) {
    VkCommandBuffer cb = CB;
    draw_scale(cb);
    vkCmdDrawIndirect(cb, H(VkBuffer, c->x[1]), c->x[2], (u32)c->x[3], (u32)c->x[4]);
    ret(c, 0);
}
static void gvk_CmdDrawIndexedIndirect(CPU* RESTRICT c, u64 entry) {
    VkCommandBuffer cb = CB;
    draw_scale(cb);
    vkCmdDrawIndexedIndirect(cb, H(VkBuffer, c->x[1]), c->x[2], (u32)c->x[3], (u32)c->x[4]);
    ret(c, 0);
}
static void gvk_CmdDispatch(CPU* RESTRICT c, u64 entry) {
    vkCmdDispatch(CB, (u32)c->x[1], (u32)c->x[2], (u32)c->x[3]);
    ret(c, 0);
}
static void gvk_CmdDispatchBase(CPU* RESTRICT c, u64 entry) {
    vkCmdDispatchBase(CB, (u32)c->x[1], (u32)c->x[2], (u32)c->x[3], (u32)c->x[4], (u32)c->x[5], (u32)c->x[6]);
    ret(c, 0);
}
static void gvk_CmdSetDeviceMask(CPU* RESTRICT c, u64 entry) {
    vkCmdSetDeviceMask(CB, (u32)c->x[1]);
    ret(c, 0);
}
static void gvk_CmdDispatchIndirect(CPU* RESTRICT c, u64 entry) {
    vkCmdDispatchIndirect(CB, H(VkBuffer, c->x[1]), c->x[2]);
    ret(c, 0);
}
static void gvk_CmdBeginRenderPass(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    VkRenderPassBeginInfo* bi = gvk_in(&gvk_t_rpbegin, c->x[1], 1);
    if (bi) {
        VkSubpassContents contents = (VkSubpassContents)(u32)c->x[2];
        u32 scaled = set_has(g_gvk_api.sfb, GVK_SCALED, U(bi->framebuffer));
        if (scaled) bi->renderArea = gvk_rect_at(bi->renderArea, scale_factor());
        mixed_sync(CB, U(bi->framebuffer), 0);
        vkCmdBeginRenderPass(CB, bi, contents);
        GvkPass p = {c->x[0], U(bi->renderPass), U(bi->framebuffer), bi->renderArea, 0, contents, scaled};
        tab_put(TAB(pass), &g_gvk_api.npass, &p);
    }
    ret(c, 0);
}
static void gvk_CmdNextSubpass(CPU* RESTRICT c, u64 entry) {
    GvkPass p;
    vkCmdNextSubpass(CB, (VkSubpassContents)(u32)c->x[1]);
    if (tab_get(TAB(pass), c->x[0], &p)) {
        p.subpass++;
        p.contents = (VkSubpassContents)(u32)c->x[1];
        tab_put(TAB(pass), &g_gvk_api.npass, &p);
    }
    ret(c, 0);
}
static void gvk_CmdEndRenderPass(CPU* RESTRICT c, u64 entry) {
    GvkPass p;
    vkCmdEndRenderPass(CB);
    if (tab_get(TAB(pass), c->x[0], &p)) mixed_sync(CB, p.fb, 1);
    pass_forget(c->x[0]);
    ret(c, 0);
}
static void gvk_CmdExecuteCommands(CPU* RESTRICT c, u64 entry) {
    u32 n = (u32)c->x[1];
    GUEST(cbs, VkCommandBuffer, c->x[2], n);
    if (!n) {
        ret(c, 0);
        return;
    }
    gvk_scratch_reset();
    VkCommandBuffer* copy = gvk_scratch(n * sizeof *copy);
    memcpy(copy, cbs, n * sizeof *copy);
    for (u32 i = 0; i < n; i++) checked_cb(U(copy[i]));
    vkCmdExecuteCommands(CB, n, copy);
    if (scale_factor() > 1) { /* the secondaries' push constants are the primary's after them */
        pm_lock(&g_gvk_api.lock);
        GvkBound* b = &g_gvk_api.bound[tab_slot(TAB(bound), c->x[0])];
        if (b->cb == c->x[0]) b->pushed_layout = 0;
        pm_unlock(&g_gvk_api.lock);
    }
    ret(c, 0);
}
/* The stages to add to have for each access in access that none of its stages supports, as the console's driver
   takes a barrier without them: one game's uploads give TRANSFER_WRITE from the HOST stage, and SHADER_READ for the
   TRANSFER stage */
static VkPipelineStageFlags stages_for(VkAccessFlags access, VkPipelineStageFlags have) {
    enum {
        SHADERS = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
            VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT | VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT |
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        /* the shader stages added: those every device has */
        ADD_SHADERS = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        TESTS = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
    };
    static const struct {
        VkAccessFlags access;
        VkPipelineStageFlags supported, add;
    } k[] = {
        {VK_ACCESS_INDIRECT_COMMAND_READ_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT},
        {VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT},
        {VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, SHADERS, ADD_SHADERS},
        {VK_ACCESS_INPUT_ATTACHMENT_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT},
        {VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT},
        {VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, TESTS, TESTS},
        {VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_PIPELINE_STAGE_TRANSFER_BIT},
        {VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_HOST_BIT},
    };
    if (have & VK_PIPELINE_STAGE_ALL_COMMANDS_BIT) return 0;
    if (have & VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT) /* every stage of a draw */
        have |= (SHADERS & ~(VkPipelineStageFlags)VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) | TESTS |
                VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkPipelineStageFlags add = 0;
    for (u32 e = 0; e < sizeof k / sizeof *k; e++)
        if ((access & k[e].access) && !(have & k[e].supported)) add |= k[e].add;
    return add;
}
/* the game's barriers as the host takes them: one queue family, images in host_layout and as the host draws them
   (host_image). *sa and *da: the access masks of all */
static void barriers_to_host(const VkMemoryBarrier* m, u32 nm, VkBufferMemoryBarrier* b, u32 nb,
                             VkImageMemoryBarrier* i, u32 ni, VkAccessFlags* sa, VkAccessFlags* da) {
    *sa = *da = 0;
    for (u32 k = 0; k < nm; k++) {
        *sa |= m[k].srcAccessMask;
        *da |= m[k].dstAccessMask;
    }
    for (u32 k = 0; k < nb; k++) {
        b[k].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[k].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        *sa |= b[k].srcAccessMask;
        *da |= b[k].dstAccessMask;
    }
    for (u32 k = 0; k < ni; k++) {
        i[k].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        i[k].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        i[k].oldLayout = host_layout(i[k].oldLayout);
        i[k].newLayout = host_layout(i[k].newLayout);
        if (i[k].oldLayout == VK_IMAGE_LAYOUT_PREINITIALIZED && image_get(U(i[k].image)).preinit)
            i[k].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        u32 f;
        i[k].image = host_image(U(i[k].image), &f, 0, 0);
        *sa |= i[k].srcAccessMask;
        *da |= i[k].dstAccessMask;
    }
}
/* A barrier inside a render pass stays there when it fits one self-dependency of the active subpass (a primary's, or
   the subpass a secondary continues) and transitions no layout. Vulkan takes only memory barriers there, and image
   barriers on the subpass's own attachments. The console's driver takes any, so each buffer and image barrier becomes
   a memory barrier with its access masks: the same dependency over all memory, legal whatever the image is. Another
   barrier ends the pass (OUTSIDE) */
static void gvk_CmdPipelineBarrier(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    u32 nm = (u32)c->x[4], nb = (u32)c->x[6], ni = (u32)stack_arg(c, 0);
    const VkMemoryBarrier* m = gvk_in(&gvk_t_mbar, c->x[5], nm);
    VkBufferMemoryBarrier* b = gvk_in(&gvk_t_bbar, c->x[7], nb);
    VkImageMemoryBarrier* i = gvk_in(&gvk_t_ibar, stack_arg(c, 1), ni);
    if ((nm && !m) || (nb && !b) || (ni && !i)) {
        ret(c, 0);
        return;
    }
    VkAccessFlags sa, da;
    barriers_to_host(m, nm, b, nb, i, ni, &sa, &da);
    VkPipelineStageFlags src = (VkPipelineStageFlags)c->x[1], dst = (VkPipelineStageFlags)c->x[2];
    VkDependencyFlags flags = (VkDependencyFlags)c->x[3];
    GvkPass p;
    GvkRpass rp;
    GvkCmd cmd;
    VkCommandBuffer cb = CB;
    u64 in_rp = 0;
    u32 in_subpass = 0;
    if (tab_get(TAB(pass), c->x[0], &p)) {
        if (p.contents == VK_SUBPASS_CONTENTS_INLINE) {
            in_rp = p.rp;
            in_subpass = p.subpass;
        }
    } else if (tab_get(TAB(cmd), c->x[0], &cmd) && cmd.inherited_rp) {
        in_rp = cmd.inherited_rp;
        in_subpass = cmd.inherited_subpass;
    }
    int same_layouts = 1;
    for (u32 k = 0; k < ni; k++) same_layouts &= i[k].oldLayout == i[k].newLayout;
    if (in_rp && same_layouts && tab_get(TAB(rpass), in_rp, &rp)) {
        for (u32 k = 0; k < rp.nself; k++) {
            const VkSubpassDependency* d = &rp.self[k];
            if (d->srcSubpass == in_subpass && !(src & ~d->srcStageMask) && !(dst & ~d->dstStageMask) &&
                !(sa & ~d->srcAccessMask) && !(da & ~d->dstAccessMask) && flags == d->dependencyFlags) {
                VkMemoryBarrier* all = gvk_scratch(sizeof *all * ((size_t)nm + nb + ni));
                u32 n = 0;
                for (u32 j = 0; j < nm; j++) all[n++] = m[j];
                for (u32 j = 0; j < nb; j++)
                    all[n++] = (VkMemoryBarrier){VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, b[j].srcAccessMask,
                                                 b[j].dstAccessMask};
                for (u32 j = 0; j < ni; j++)
                    all[n++] = (VkMemoryBarrier){VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, i[j].srcAccessMask,
                                                 i[j].dstAccessMask};
                vkCmdPipelineBarrier(cb, src, dst, flags, n, all, 0, NULL, 0, NULL);
                ret(c, 0);
                return;
            }
        }
    }
    src |= stages_for(sa, src);
    dst |= stages_for(da, dst);
    OUTSIDE(cb, vkCmdPipelineBarrier(cb, src, dst, flags, nm, m, nb, b, ni, i));
    ret(c, 0);
}
/* its barriers as gvk_CmdPipelineBarrier's, outside a render pass */
static void gvk_CmdWaitEvents(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    u32 ne = (u32)c->x[1], nm = (u32)c->x[5], nb = (u32)c->x[7], ni = (u32)stack_arg(c, 1);
    GUEST(ev, VkEvent, c->x[2], ne);
    const VkMemoryBarrier* m = gvk_in(&gvk_t_mbar, c->x[6], nm);
    VkBufferMemoryBarrier* b = gvk_in(&gvk_t_bbar, stack_arg(c, 0), nb);
    VkImageMemoryBarrier* i = gvk_in(&gvk_t_ibar, stack_arg(c, 2), ni);
    if ((nm && !m) || (nb && !b) || (ni && !i)) {
        ret(c, 0);
        return;
    }
    VkAccessFlags sa, da;
    barriers_to_host(m, nm, b, nb, i, ni, &sa, &da);
    VkPipelineStageFlags src = (VkPipelineStageFlags)c->x[3], dst = (VkPipelineStageFlags)c->x[4];
    src |= stages_for(sa, src);
    dst |= stages_for(da, dst);
    VkCommandBuffer cb = CB;
    OUTSIDE(cb, vkCmdWaitEvents(cb, ne, ev, src, dst, nm, m, nb, b, ni, i));
    ret(c, 0);
}
static void gvk_CmdCopyBuffer(CPU* RESTRICT c, u64 entry) {
    GUEST(r, VkBufferCopy, c->x[4], (u32)c->x[3]);
    OUTSIDE(CB, vkCmdCopyBuffer(CB, H(VkBuffer, c->x[1]), H(VkBuffer, c->x[2]), (u32)c->x[3], r));
    ret(c, 0);
}
/* n regions of a target blitted between the game's image and its shadow at f: up to the shadow, or down to the
   game's image. Both are in GENERAL (shadow_make) */
static void target_blit(VkCommandBuffer cb, u64 img, VkImage shadow, u32 f, const GvkRegion* g, u32 n, int up) {
    const VkFormatFeatureFlags blit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    GvkImage gi = image_get(img);
    if ((gi.features & blit) != blit || gi.samples != VK_SAMPLE_COUNT_1_BIT) {
        WARN_LIMIT(4,
                   "gvk: a copy of a target drawn at %ux leaves its pixels out: its format or samples can't be "
                   "blitted\n",
                   f);
        return;
    }
    VkImageBlit* b = gvk_scratch(sizeof *b * n);
    int color = 1;
    for (u32 i = 0; i < n; i++) {
        /* a 2D image's: its layers are the subresource's, whatever depth a 3D image's copy names */
        VkOffset3D o = {g[i].o.x, g[i].o.y, 0}, end = {g[i].o.x + (i32)g[i].e.width, g[i].o.y + (i32)g[i].e.height, 1};
        VkOffset3D game[2] = {o, end}, scaled[2] = {offset_at(o, f), offset_at(end, f)};
        b[i] = (VkImageBlit){.srcSubresource = g[i].sub, .dstSubresource = g[i].sub};
        for (u32 k = 0; k < 2; k++) {
            b[i].srcOffsets[k] = up ? game[k] : scaled[k];
            b[i].dstOffsets[k] = up ? scaled[k] : game[k];
        }
        color &= g[i].sub.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT;
    }
    VkFilter filter = color && (gi.features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? VK_FILTER_LINEAR
                                                                                                 : VK_FILTER_NEAREST;
    VkImage game_img = H(VkImage, img);
    vkCmdBlitImage(cb, up ? game_img : shadow, VK_IMAGE_LAYOUT_GENERAL, up ? shadow : game_img, VK_IMAGE_LAYOUT_GENERAL,
                   n, b, filter);
}
/* A copy between images at one scale scales its regions. Between a target drawn at the scale and an image at the
   game's size it goes through the target's own image: blitted down from the shadow and copied out, or copied in and
   blitted up. A swapchain image, made at the scale with no image at the game's size, is blitted directly */
static void gvk_CmdCopyImage(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    u32 n = (u32)c->x[5], fs, fd;
    const VkImageCopy* r = n && mem_user_mapped(c->x[6], sizeof *r * n) ? gvk_ptr(c->x[6]) : NULL;
    if (!r) {
        ret(c, 0);
        return;
    }
    VkImage src = host_image(c->x[1], &fs, c->x[0], 0), dst = host_image(c->x[3], &fd, c->x[0], 1);
    VkImageLayout sl = arg_layout(c->x[2]), dl = arg_layout(c->x[4]);
    VkCommandBuffer cb = CB;
    int down = fs > fd;
    u64 img = down ? c->x[1] : c->x[3];
    VkImage shadow = down ? src : dst;
    if (fs == 1 && fd == 1) {
        OUTSIDE(cb, vkCmdCopyImage(cb, src, sl, dst, dl, n, r));
    } else if (fs == fd) {
        VkImageCopy* s = gvk_scratch(sizeof *s * n);
        for (u32 i = 0; i < n; i++) {
            s[i] = r[i];
            s[i].srcOffset = offset_at(r[i].srcOffset, fs);
            s[i].dstOffset = offset_at(r[i].dstOffset, fs);
            s[i].extent.width *= fs;
            s[i].extent.height *= fs;
        }
        OUTSIDE(cb, vkCmdCopyImage(cb, src, sl, dst, dl, n, s));
    } else if (shadow != H(VkImage, img)) {
        GvkRegion* g = gvk_scratch(sizeof *g * n);
        for (u32 i = 0; i < n; i++)
            g[i] = down ? (GvkRegion){r[i].srcSubresource, r[i].srcOffset, r[i].extent}
                        : (GvkRegion){r[i].dstSubresource, r[i].dstOffset, r[i].extent};
        GvkPass p;
        int ended = pass_out(cb, &p);
        if (down) {
            target_blit(cb, img, shadow, fs, g, n, 0);
            full_barrier(cb);
            vkCmdCopyImage(cb, H(VkImage, img), VK_IMAGE_LAYOUT_GENERAL, dst, dl, n, r);
        } else {
            vkCmdCopyImage(cb, src, sl, H(VkImage, img), VK_IMAGE_LAYOUT_GENERAL, n, r);
            full_barrier(cb);
            target_blit(cb, img, shadow, fd, g, n, 1);
        }
        if (ended) pass_back(cb, &p);
    } else {
        GvkImage si = image_get(c->x[1]), di = image_get(c->x[3]);
        if (si.samples != VK_SAMPLE_COUNT_1_BIT || di.samples != VK_SAMPLE_COUNT_1_BIT || si.format != di.format ||
            !(si.features & VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !(di.features & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
            LOG_LIMIT(4, "gvk: a copy between a swapchain image and an image at the game's size is left out: their "
                         "formats or samples can't be blitted\n");
            ret(c, 0);
            return;
        }
        VkImageBlit* b = gvk_scratch(sizeof *b * n);
        int color = 1;
        for (u32 i = 0; i < n; i++) {
            VkOffset3D end = {r[i].srcOffset.x + (i32)r[i].extent.width, r[i].srcOffset.y + (i32)r[i].extent.height,
                              r[i].srcOffset.z + (i32)r[i].extent.depth};
            VkOffset3D dend = {r[i].dstOffset.x + (i32)r[i].extent.width, r[i].dstOffset.y + (i32)r[i].extent.height,
                               r[i].dstOffset.z + (i32)r[i].extent.depth};
            b[i] = (VkImageBlit){.srcSubresource = r[i].srcSubresource,
                                 .srcOffsets = {offset_at(r[i].srcOffset, fs), offset_at(end, fs)},
                                 .dstSubresource = r[i].dstSubresource,
                                 .dstOffsets = {offset_at(r[i].dstOffset, fd), offset_at(dend, fd)}};
            color &= r[i].srcSubresource.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT;
        }
        VkFilter filter = color && (si.features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
                              ? VK_FILTER_LINEAR
                              : VK_FILTER_NEAREST;
        OUTSIDE(cb, vkCmdBlitImage(cb, src, sl, dst, dl, n, b, filter));
    }
    ret(c, 0);
}
static void gvk_CmdBlitImage(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    u32 n = (u32)c->x[5], fs, fd;
    const VkImageBlit* r = n && mem_user_mapped(c->x[6], sizeof *r * n) ? gvk_ptr(c->x[6]) : NULL;
    if (!r) {
        ret(c, 0);
        return;
    }
    VkImage src = host_image(c->x[1], &fs, c->x[0], 0), dst = host_image(c->x[3], &fd, c->x[0], 1);
    VkImageBlit* s = fs > 1 || fd > 1 ? gvk_scratch(sizeof *s * n) : NULL;
    for (u32 i = 0; s && i < n; i++) {
        s[i] = r[i];
        for (u32 k = 0; k < 2; k++) {
            s[i].srcOffsets[k] = offset_at(r[i].srcOffsets[k], fs);
            s[i].dstOffsets[k] = offset_at(r[i].dstOffsets[k], fd);
        }
    }
    OUTSIDE(CB, vkCmdBlitImage(CB, src, arg_layout(c->x[2]), dst, arg_layout(c->x[4]), n, s ? s : r,
                               (VkFilter)(u32)c->x[7]));
    ret(c, 0);
}
static void gvk_CmdResolveImage(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    u32 n = (u32)c->x[5], fs, fd;
    const VkImageResolve* r = n && mem_user_mapped(c->x[6], sizeof *r * n) ? gvk_ptr(c->x[6]) : NULL;
    if (!r) {
        ret(c, 0);
        return;
    }
    VkImage src = host_image(c->x[1], &fs, c->x[0], 0), dst = host_image(c->x[3], &fd, c->x[0], 1);
    VkImageResolve* s = fs == fd && fs > 1 ? gvk_scratch(sizeof *s * n) : NULL;
    if (fs != fd) LOG_LIMIT(4, "gvk: a resolve between images at two scales resolves the game's region alone\n");
    for (u32 i = 0; s && i < n; i++) {
        s[i] = r[i];
        s[i].srcOffset = offset_at(r[i].srcOffset, fs);
        s[i].dstOffset = offset_at(r[i].dstOffset, fs);
        s[i].extent.width *= fs;
        s[i].extent.height *= fs;
    }
    OUTSIDE(CB, vkCmdResolveImage(CB, src, arg_layout(c->x[2]), dst, arg_layout(c->x[4]), n, s ? s : r));
    ret(c, 0);
}
/* A buffer holds the game's pixels: a copy between one and a target drawn at the scale goes through the target's own
   image, blitted up to the shadow after the copy in, or down from it before the copy out */
/* what host_format says of a 24-bit depth format's copies: said where one is recorded, as the bytes differ */
static void depth24_copy_said(u64 img, const VkBufferImageCopy* r, u32 n, const char* what) {
    GvkImage e = image_get(img);
    if (e.game_depth && gvk_copies_depth(r, n))
        WARN_LIMIT(
            4,
            "gvk: %s of the depth of an image the game made with format %d, held as format %d: 32-bit floats are "
            "moved where the game expects 24-bit integers\n",
            what, (int)e.game_depth, (int)e.format);
}
static void gvk_CmdCopyBufferToImage(CPU* RESTRICT c, u64 entry) {
    u32 n = (u32)c->x[4], f;
    GUEST(r, VkBufferImageCopy, c->x[5], n);
    depth24_copy_said(c->x[2], r, n, "a copy from a buffer");
    gvk_scratch_reset();
    VkImage dst = host_image(c->x[2], &f, c->x[0], 1);
    VkBuffer buf = H(VkBuffer, c->x[1]);
    VkCommandBuffer cb = CB;
    if (dst == H(VkImage, c->x[2]) || !n) {
        if (f > 1) LOG_LIMIT(4, "gvk: a copy from a buffer into a swapchain image fills a part of it\n");
        OUTSIDE(cb, vkCmdCopyBufferToImage(cb, buf, dst, arg_layout(c->x[3]), n, r));
        ret(c, 0);
        return;
    }
    GvkRegion* g = gvk_scratch(sizeof *g * n);
    for (u32 i = 0; i < n; i++) g[i] = (GvkRegion){r[i].imageSubresource, r[i].imageOffset, r[i].imageExtent};
    GvkPass p;
    int ended = pass_out(cb, &p);
    vkCmdCopyBufferToImage(cb, buf, H(VkImage, c->x[2]), VK_IMAGE_LAYOUT_GENERAL, n, r);
    full_barrier(cb);
    target_blit(cb, c->x[2], dst, f, g, n, 1);
    if (ended) pass_back(cb, &p);
    ret(c, 0);
}
static void gvk_CmdCopyImageToBuffer(CPU* RESTRICT c, u64 entry) {
    u32 n = (u32)c->x[4], f;
    GUEST(r, VkBufferImageCopy, c->x[5], n);
    depth24_copy_said(c->x[1], r, n, "a copy to a buffer");
    gvk_scratch_reset();
    VkImage src = host_image(c->x[1], &f, c->x[0], 0);
    VkBuffer buf = H(VkBuffer, c->x[3]);
    VkCommandBuffer cb = CB;
    if (src == H(VkImage, c->x[1]) || !n) {
        if (f > 1) LOG_LIMIT(4, "gvk: a copy of a swapchain image into a buffer reads a part of it\n");
        OUTSIDE(cb, vkCmdCopyImageToBuffer(cb, src, arg_layout(c->x[2]), buf, n, r));
        ret(c, 0);
        return;
    }
    GvkRegion* g = gvk_scratch(sizeof *g * n);
    for (u32 i = 0; i < n; i++) g[i] = (GvkRegion){r[i].imageSubresource, r[i].imageOffset, r[i].imageExtent};
    GvkPass p;
    int ended = pass_out(cb, &p);
    target_blit(cb, c->x[1], src, f, g, n, 0);
    full_barrier(cb);
    vkCmdCopyImageToBuffer(cb, H(VkImage, c->x[1]), VK_IMAGE_LAYOUT_GENERAL, buf, n, r);
    if (ended) pass_back(cb, &p);
    ret(c, 0);
}
static void gvk_CmdUpdateBuffer(CPU* RESTRICT c, u64 entry) {
    GUEST(data, u8, c->x[4], c->x[3]);
    OUTSIDE(CB, vkCmdUpdateBuffer(CB, H(VkBuffer, c->x[1]), c->x[2], c->x[3], data));
    ret(c, 0);
}
static void gvk_CmdFillBuffer(CPU* RESTRICT c, u64 entry) {
    OUTSIDE(CB, vkCmdFillBuffer(CB, H(VkBuffer, c->x[1]), c->x[2], c->x[3], (u32)c->x[4]));
    ret(c, 0);
}
static void gvk_CmdClearColorImage(CPU* RESTRICT c, u64 entry) {
    GUEST(color, VkClearColorValue, c->x[3], 1);
    GUEST(ranges, VkImageSubresourceRange, c->x[5], (u32)c->x[4]);
    u32 f;
    VkImage img = host_image(c->x[1], &f, c->x[0], 1);
    OUTSIDE(CB, vkCmdClearColorImage(CB, img, arg_layout(c->x[2]), color, (u32)c->x[4], ranges));
    ret(c, 0);
}
static void gvk_CmdClearDepthStencilImage(CPU* RESTRICT c, u64 entry) {
    GUEST(value, VkClearDepthStencilValue, c->x[3], 1);
    GUEST(ranges, VkImageSubresourceRange, c->x[5], (u32)c->x[4]);
    u32 f;
    VkImage img = host_image(c->x[1], &f, c->x[0], 1);
    OUTSIDE(CB, vkCmdClearDepthStencilImage(CB, img, arg_layout(c->x[2]), value, (u32)c->x[4], ranges));
    ret(c, 0);
}
static void gvk_CmdClearAttachments(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    u32 n = (u32)c->x[3], f = pass_scale(c->x[0]);
    const VkClearRect* r = n && mem_user_mapped(c->x[4], sizeof *r * n) ? gvk_ptr(c->x[4]) : NULL;
    if (!r) {
        ret(c, 0);
        return;
    }
    VkClearRect* s = f > 1 ? gvk_scratch(sizeof *s * n) : NULL;
    for (u32 i = 0; s && i < n; i++) {
        s[i] = r[i];
        s[i].rect = gvk_rect_at(r[i].rect, f);
    }
    GUEST(atts, VkClearAttachment, c->x[2], (u32)c->x[1]);
    vkCmdClearAttachments(CB, (u32)c->x[1], atts, n, s ? s : r);
    ret(c, 0);
}
static void gvk_CmdResetQueryPool(CPU* RESTRICT c, u64 entry) {
    OUTSIDE(CB, vkCmdResetQueryPool(CB, H(VkQueryPool, c->x[1]), (u32)c->x[2], (u32)c->x[3]));
    ret(c, 0);
}
static void gvk_CmdBeginQuery(CPU* RESTRICT c, u64 entry) {
    vkCmdBeginQuery(CB, H(VkQueryPool, c->x[1]), (u32)c->x[2], (VkQueryControlFlags)c->x[3]);
    ret(c, 0);
}
static void gvk_CmdEndQuery(CPU* RESTRICT c, u64 entry) {
    vkCmdEndQuery(CB, H(VkQueryPool, c->x[1]), (u32)c->x[2]);
    ret(c, 0);
}
static void gvk_CmdWriteTimestamp(CPU* RESTRICT c, u64 entry) {
    vkCmdWriteTimestamp(CB, (VkPipelineStageFlagBits)(u32)c->x[1], H(VkQueryPool, c->x[2]), (u32)c->x[3]);
    ret(c, 0);
}
static void gvk_CmdCopyQueryPoolResults(CPU* RESTRICT c, u64 entry) {
    OUTSIDE(CB, vkCmdCopyQueryPoolResults(CB, H(VkQueryPool, c->x[1]), (u32)c->x[2], (u32)c->x[3], H(VkBuffer, c->x[4]),
                                          c->x[5], c->x[6], (VkQueryResultFlags)c->x[7]));
    ret(c, 0);
}
static void gvk_CmdSetEvent(CPU* RESTRICT c, u64 entry) {
    vkCmdSetEvent(CB, H(VkEvent, c->x[1]), (VkPipelineStageFlags)c->x[2]);
    ret(c, 0);
}
static void gvk_CmdResetEvent(CPU* RESTRICT c, u64 entry) {
    vkCmdResetEvent(CB, H(VkEvent, c->x[1]), (VkPipelineStageFlags)c->x[2]);
    ret(c, 0);
}
#undef CB

/* ------------------------------------------------------------ queries and synchronization */
static void gvk_GetQueryPoolResults(CPU* RESTRICT c, u64 entry) {
    size_t size = c->x[4];
    VkQueryResultFlags flags = (VkQueryResultFlags)c->x[7];
    GvkQuery q;
    u32 values = tab_get(TAB(query), c->x[1], &q) ? q.values : 1;
    u64 bytes = gvk_query_bytes((u32)c->x[3], values, flags, c->x[6]);
    if (!c->x[5] || bytes > size || !gvk_writable(c->x[5], size)) {
        WARN_LIMIT(16,
                   "gvk: vkGetQueryPoolResults of %u queries of %u values, %llu bytes apart, takes %llu bytes and the "
                   "game gives %zu at %llx: refused\n",
                   (u32)c->x[3], values, (unsigned long long)c->x[6], (unsigned long long)bytes, size,
                   (unsigned long long)c->x[5]);
        ret_vk(c, VK_ERROR_INITIALIZATION_FAILED);
        return;
    }
    int wait = (flags & VK_QUERY_RESULT_WAIT_BIT) != 0;
    if (wait) kthread_host_wait_begin();
    VkResult r = vkGetQueryPoolResults(g_vk.dev, H(VkQueryPool, c->x[1]), (u32)c->x[2], (u32)c->x[3], size, GP(c->x[5]),
                                       c->x[6], flags);
    if (wait) kthread_host_wait_end();
    ret_vk(c, r);
}
static void gvk_ResetFences(CPU* RESTRICT c, u64 entry) {
    GUEST(fences, VkFence, c->x[2], (u32)c->x[1]);
    ret_vk(c, vkResetFences(g_vk.dev, (u32)c->x[1], fences));
}
static void gvk_GetFenceStatus(CPU* RESTRICT c, u64 entry) {
    ret_vk(c, vkGetFenceStatus(g_vk.dev, H(VkFence, c->x[1])));
}
/* off the core while it waits, as the console's thread is in its wait */
static void gvk_WaitForFences(CPU* RESTRICT c, u64 entry) {
    u32 n = (u32)c->x[1];
    GUEST(fences, VkFence, c->x[2], n);
    VkResult r = vkWaitForFences(g_vk.dev, n, fences, (VkBool32)c->x[3], 0);
    if (r == VK_TIMEOUT && c->x[4]) {
        kthread_host_wait_begin();
        r = vkWaitForFences(g_vk.dev, n, fences, (VkBool32)c->x[3], c->x[4]);
        kthread_host_wait_end();
    }
    ret_vk(c, r);
}
static void gvk_GetEventStatus(CPU* RESTRICT c, u64 entry) {
    ret_vk(c, vkGetEventStatus(g_vk.dev, H(VkEvent, c->x[1])));
}
static void gvk_SetEvent(CPU* RESTRICT c, u64 entry) { ret_vk(c, vkSetEvent(g_vk.dev, H(VkEvent, c->x[1]))); }
static void gvk_ResetEvent(CPU* RESTRICT c, u64 entry) { ret_vk(c, vkResetEvent(g_vk.dev, H(VkEvent, c->x[1]))); }

static void gvk_QueueSubmit(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    u32 n = (u32)c->x[1];
    VkSubmitInfo* s = gvk_in(&gvk_t_submit, c->x[2], n);
    if (c->x[0] != U(g_vk.gfx) || (n && !s)) {
        ret_vk(c, VK_ERROR_INITIALIZATION_FAILED);
        return;
    }
    for (u32 i = 0; i < n; i++) {
        u32 count = s[i].commandBufferCount;
        if (!count) continue;
        VkCommandBuffer* copy = gvk_scratch(count * sizeof *copy);
        memcpy(copy, s[i].pCommandBuffers, count * sizeof *copy);
        for (u32 k = 0; k < count; k++) checked_cb(U(copy[k]));
        s[i].pCommandBuffers = copy;
    }
    VkResult r = queue_submit(n, s, H(VkFence, c->x[3]));
    if (r == VK_SUCCESS && scale_factor() != 1) {
        pm_lock(&g_gvk_api.lock);
        for (u32 i = 0; i < n; i++)
            for (u32 k = 0; k < s[i].commandBufferCount; k++) cb_submitted_nl(U(s[i].pCommandBuffers[k]));
        pm_unlock(&g_gvk_api.lock);
    }
    ret_vk(c, r);
}
static void gvk_QueueWaitIdle(CPU* RESTRICT c, u64 entry) { gvk_DeviceWaitIdle(c, entry); }

/* ------------------------------------------------------------ the window */
static void gvk_CreateViSurfaceNN(CPU* RESTRICT c, u64 entry) {
    REQUIRE_OUT(c->x[3], 8);
    u64 win = c->x[1] && mem_user_mapped(c->x[1], 32) ? gr64(c->x[1] + 24) : 0;
    pm_lock(&g_gvk_api.lock);
    g_gvk_api.vi_window = win;
    pm_unlock(&g_gvk_api.lock);
    put_handle(c->x[3], GVK_SURFACE);
    ret_vk(c, VK_SUCCESS);
}
static void gvk_DestroySurfaceKHR(CPU* RESTRICT c, u64 entry) {
    if (c->x[1] == GVK_SURFACE) {
        pm_lock(&g_gvk_api.lock);
        g_gvk_api.vi_window = 0;
        pm_unlock(&g_gvk_api.lock);
    }
    ret(c, 0);
}
static void gvk_GetPhysicalDeviceSurfaceSupportKHR(CPU* RESTRICT c, u64 entry) {
    if (c->x[3] && gvk_writable(c->x[3], 4)) gw32(c->x[3], VK_TRUE);
    ret_vk(c, VK_SUCCESS);
}
/* the vi layer's: the console's screen, 1920x1080 in TV mode and 1280x720 handheld. Its transform can be the layer's
   own (INHERIT), which one game's swapchain asks for */
static void gvk_GetPhysicalDeviceSurfaceCapabilitiesKHR(CPU* RESTRICT c, u64 entry) {
    u32 w = snx_tv_mode() ? 1920 : 1280, h = snx_tv_mode() ? 1080 : 720;
    VkSurfaceCapabilitiesKHR s = {
        .minImageCount = 2,
        .maxImageCount = GVK_SC_IMAGES,
        .currentExtent = {w, h},
        .minImageExtent = {1, 1},
        .maxImageExtent = {GVK_SC_MAX, GVK_SC_MAX_H},
        .maxImageArrayLayers = 1,
        .supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR | VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR,
        .currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
    put(c->x[2], &s, sizeof s);
    ret_vk(c, VK_SUCCESS);
}
static void gvk_GetPhysicalDeviceSurfaceFormatsKHR(CPU* RESTRICT c, u64 entry) {
    const VkSurfaceFormatKHR f[4] = {{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
                                     {VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
                                     {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
                                     {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
    ret_vk(c, put_list(c->x[2], c->x[3], f, 4, sizeof *f));
}
static void gvk_GetPhysicalDeviceSurfacePresentModesKHR(CPU* RESTRICT c, u64 entry) {
    const VkPresentModeKHR m[1] = {VK_PRESENT_MODE_FIFO_KHR};
    ret_vk(c, put_list(c->x[2], c->x[3], m, 1, sizeof *m));
}

static GvkSwapchain* swapchain_of(u64 h) {
    return h && h <= GVK_SWAPCHAINS && g_gvk_api.sc[h - 1].n ? &g_gvk_api.sc[h - 1] : NULL;
}
/* Called with g_gvk_api.lock held; unlocks it before returning. Waits for in-flight acquires and the queue. */
static void swapchain_free_locked(GvkSwapchain* s) {
    if (s->destroying) {
        pm_unlock(&g_gvk_api.lock);
        return;
    }
    s->destroying = 1;
    s->retired = 1;
    pc_broadcast(&g_gvk_api.sc_available);
    while (s->reserved) pc_wait(&g_gvk_api.sc_available, &g_gvk_api.lock, -1);
    u32 n = s->n;
    VkImage img[GVK_SC_IMAGES];
    VkDeviceMemory mem[GVK_SC_IMAGES];
    memcpy(img, s->img, sizeof img);
    memcpy(mem, s->mem, sizeof mem);
    *s = (GvkSwapchain){0};
    pm_unlock(&g_gvk_api.lock);
    vk_lock();
    vk_rec_queue(g_vk.gfx);
    vk_queue_lock(g_vk.gfx);
    vkQueueWaitIdle(g_vk.gfx);
    vk_queue_unlock(g_vk.gfx);
    vk_unlock();
    for (u32 i = 0; i < n; i++) {
        GvkTarget t;
        if (img[i] && scale_factor() > 1) tab_take(TAB(target), &g_gvk_api.ntarget, U(img[i]), &t);
        if (img[i]) image_untrack(U(img[i]));
        vkDestroyImage(g_vk.dev, img[i], NULL);
        vkFreeMemory(g_vk.dev, mem[i], NULL);
    }
}
static VkResult once_begin(GvkOnce* o) {
    *o = (GvkOnce){0};
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                  .queueFamilyIndex = g_vk.gfx_family};
    VkResult r = vkCreateCommandPool(g_vk.dev, &pi, NULL, &o->pool);
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = o->pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1};
    if (r == VK_SUCCESS) r = vkAllocateCommandBuffers(g_vk.dev, &ai, &o->cb);
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (r == VK_SUCCESS) r = vkCreateFence(g_vk.dev, &fi, NULL, &o->fence);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (r == VK_SUCCESS) r = vkBeginCommandBuffer(o->cb, &bi);
    return r;
}
/* r: once_begin's and the recording's */
static VkResult once_end(GvkOnce* o, VkResult r) {
    if (r == VK_SUCCESS) r = vkEndCommandBuffer(o->cb);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &o->cb};
    if (r == VK_SUCCESS) r = queue_submit(1, &si, o->fence);
    if (r == VK_SUCCESS) {
        kthread_host_wait_begin();
        r = vkWaitForFences(g_vk.dev, 1, &o->fence, VK_TRUE, UINT64_MAX);
        kthread_host_wait_end();
    }
    if (o->fence) vkDestroyFence(g_vk.dev, o->fence, NULL);
    if (o->pool) vkDestroyCommandPool(g_vk.dev, o->pool, NULL);
    return r;
}
/* a swapchain's new images put in GENERAL, the one layout the game's render passes take them in (host_layout), ahead
   of the game's work on the queue */
static VkResult images_general(const VkImage* img, u32 n, VkImageAspectFlags aspects) {
    GvkOnce o;
    VkResult r = once_begin(&o);
    if (r == VK_SUCCESS) {
        VkImageMemoryBarrier b[GVK_SC_IMAGES];
        for (u32 i = 0; i < n; i++)
            b[i] = (VkImageMemoryBarrier){
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = img[i],
                .subresourceRange = {aspects, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
        vkCmdPipelineBarrier(o.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                             NULL, n, b);
    }
    return once_end(&o, r);
}
static void gvk_CreateSwapchainKHR(CPU* RESTRICT c, u64 entry) {
    REQUIRE_OUT(c->x[3], 8);
    u64 ci = c->x[1];
    if (!ci || !mem_user_mapped(ci, sizeof(VkSwapchainCreateInfoKHR))) {
        ret_vk(c, VK_ERROR_INITIALIZATION_FAILED);
        return;
    }
    u32 want = gr32(ci + offsetof(VkSwapchainCreateInfoKHR, minImageCount));
    VkFormat fmt = (VkFormat)gr32(ci + offsetof(VkSwapchainCreateInfoKHR, imageFormat));
    u32 w = gr32(ci + offsetof(VkSwapchainCreateInfoKHR, imageExtent)),
        h = gr32(ci + offsetof(VkSwapchainCreateInfoKHR, imageExtent) + 4);
    VkImageUsageFlags usage = gr32(ci + offsetof(VkSwapchainCreateInfoKHR, imageUsage));
    /* the game draws its frame into them: at the resolution scale, as its targets. One past maxImageExtent, which
       scale_factor doesn't fit to the host, stays at the game's size, and its framebuffers with it (fb_scale) */
    u32 f = usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT ? scale_factor() : 1;
    if (f > 1 && (w > fb_max_w() / f || h > fb_max_h() / f)) f = 1;
    pm_lock(&g_gvk_api.lock);
    u64 old = gr64(ci + offsetof(VkSwapchainCreateInfoKHR, oldSwapchain));
    GvkSwapchain* retired = swapchain_of(old);
    if (retired) {
        retired->retired = 1;
        pc_broadcast(&g_gvk_api.sc_available);
    }
    u32 slot = 0;
    while (slot < GVK_SWAPCHAINS && g_gvk_api.sc[slot].n) slot++;
    if (slot == GVK_SWAPCHAINS) {
        pm_unlock(&g_gvk_api.lock);
        ret_vk(c, VK_ERROR_OUT_OF_HOST_MEMORY);
        return;
    }
    GvkSwapchain* s = &g_gvk_api.sc[slot];
    *s = (GvkSwapchain){.n = want < 2               ? 2
                             : want > GVK_SC_IMAGES ? GVK_SC_IMAGES
                                                    : want,
                        .fmt = fmt,
                        .w = w * f,
                        .h = h * f,
                        .scale = f};
    pm_unlock(&g_gvk_api.lock);
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = fmt,
                            .extent = {w * f, h * f, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkResult r = VK_SUCCESS;
    for (u32 i = 0; i < s->n && r == VK_SUCCESS; i++) {
        r = vkCreateImage(g_vk.dev, &ii, NULL, &s->img[i]);
        if (r != VK_SUCCESS) break;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(g_vk.dev, s->img[i], &req);
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                   .allocationSize = req.size,
                                   .memoryTypeIndex =
                                       first_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
        r = vkAllocateMemory(g_vk.dev, &ai, NULL, &s->mem[i]);
        if (r == VK_SUCCESS) r = vkBindImageMemory(g_vk.dev, s->img[i], s->mem[i], 0);
        if (r == VK_SUCCESS && f > 1) target_track(U(s->img[i]), &ii, w, h, f);
        if (r == VK_SUCCESS && scale_factor() > 1) image_track(s->img[i], &ii, 0, 0, VK_FORMAT_UNDEFINED);
    }
    if (r == VK_SUCCESS) r = images_general(s->img, s->n, VK_IMAGE_ASPECT_COLOR_BIT);
    if (r != VK_SUCCESS) {
        WARN("gvk: a swapchain of %u images of format %d, %ux%u, usage %#x, failed (%d)\n", s->n, (int)fmt, w, h, usage,
             (int)r);
        kthread_host_wait_begin();
        pm_lock(&g_gvk_api.lock);
        swapchain_free_locked(s);
        kthread_host_wait_end();
        ret_vk(c, r);
        return;
    }
    LOG("gvk: the game's swapchain: %u images of format %d, %ux%u, usage %#x, drawn at %ux%u\n", s->n, (int)fmt, w, h,
        usage, s->w, s->h);
    put_handle(c->x[3], slot + 1);
    ret_vk(c, VK_SUCCESS);
}
static void gvk_DestroySwapchainKHR(CPU* RESTRICT c, u64 entry) {
    kthread_host_wait_begin();
    pm_lock(&g_gvk_api.lock);
    GvkSwapchain* s = swapchain_of(c->x[1]);
    if (s)
        swapchain_free_locked(s);
    else
        pm_unlock(&g_gvk_api.lock);
    kthread_host_wait_end();
    ret(c, 0);
}
static void gvk_GetSwapchainImagesKHR(CPU* RESTRICT c, u64 entry) {
    pm_lock(&g_gvk_api.lock);
    GvkSwapchain* s = swapchain_of(c->x[1]);
    u32 n = 0;
    u64 img[GVK_SC_IMAGES];
    if (s && !s->destroying) {
        n = s->n;
        for (u32 i = 0; i < n; i++) img[i] = U(s->img[i]);
    }
    pm_unlock(&g_gvk_api.lock);
    if (!n) {
        ret_vk(c, VK_ERROR_SURFACE_LOST_KHR);
        return;
    }
    ret_vk(c, put_list(c->x[2], c->x[3], img, n, sizeof *img));
}
/* Reserve under the swapchain lock. Pacing and display waits run without the lock or guest core. */
static void gvk_AcquireNextImageKHR(CPU* RESTRICT c, u64 entry) {
    REQUIRE_OUT(c->x[5], 4);
    u64 timeout = c->x[2], start = plat_ns();
    u64 deadline = start + (timeout > (u64)INT64_MAX ? (u64)INT64_MAX : timeout);
    if (timeout) kthread_host_wait_begin();
    pm_lock(&g_gvk_api.lock);
    GvkSwapchain* s = swapchain_of(c->x[1]);
    VkResult r = VK_ERROR_OUT_OF_DATE_KHR;
    u32 idx = 0;
    u64 shown = 0;
    while (s && !s->retired) {
        u32 held = s->acquired | s->reserved;
        u32 tries = 0;
        idx = s->next;
        while (tries < s->n && (held & (1u << idx))) {
            idx = (idx + 1) % s->n;
            tries++;
        }
        if (tries < s->n) {
            s->reserved |= 1u << idx;
            shown = s->shown[idx];
            r = VK_SUCCESS;
            break;
        }
        u64 now = plat_ns();
        if (!timeout || now >= deadline) {
            r = timeout ? VK_TIMEOUT : VK_NOT_READY;
            break;
        }
        pc_wait(&g_gvk_api.sc_available, &g_gvk_api.lock, (i64)(deadline - now));
    }
    pm_unlock(&g_gvk_api.lock);
    if (r == VK_SUCCESS) {
        if (timeout && !present_pace_until(1, -1, deadline)) r = VK_TIMEOUT;
        if (r == VK_SUCCESS && shown && vk_display_frames() > shown && !vk_display_wait_frame(shown + 1, deadline))
            r = timeout ? VK_TIMEOUT : VK_NOT_READY;
        VkSemaphore sem = H(VkSemaphore, c->x[3]);
        VkFence fence = H(VkFence, c->x[4]);
        if (r == VK_SUCCESS && (sem || fence)) {
            VkSubmitInfo si = {
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .signalSemaphoreCount = sem ? 1 : 0, .pSignalSemaphores = &sem};
            r = queue_submit(1, &si, fence);
        }
        pm_lock(&g_gvk_api.lock);
        s->reserved &= ~(1u << idx);
        if (s->retired || !s->n) {
            r = VK_ERROR_OUT_OF_DATE_KHR;
            pc_broadcast(&g_gvk_api.sc_available);
        } else if (r == VK_SUCCESS) {
            s->acquired |= 1u << idx;
            s->next = (idx + 1) % s->n;
        } else {
            pc_broadcast(&g_gvk_api.sc_available);
        }
        pm_unlock(&g_gvk_api.lock);
    }
    if (timeout) kthread_host_wait_end();
    if (r == VK_SUCCESS) gw32(c->x[5], idx);
    ret_vk(c, r);
}
/* android::Surface (nnSdk's ANativeWindow) keeps mCrop (left, top, right, bottom) at +0xee0, right after its 64
   buffer slots and four request words. The game sets it via native_window_set_crop for dynamic resolution, and the
   console's driver puts it into QueueBuffer; gvk reads it here and scales it to the swapchain's host pixels */
static int vi_window_crop(u64 win, u32 game_w, u32 game_h, u32 scale, i32 out[4]) {
    if (!win || !mem_user_mapped(win, 0xef0) || gr32(win + 0x40) != 0x7fffffffu || !gr64(win + 0x98)) return 0;
    i32 l = (i32)gr32(win + 0xee0), t = (i32)gr32(win + 0xee4), r = (i32)gr32(win + 0xee8), b = (i32)gr32(win + 0xeec);
    if (l < 0 || t < 0 || r <= l || b <= t || (u32)r > game_w || (u32)b > game_h) return 0;
    out[0] = l * (i32)scale;
    out[1] = t * (i32)scale;
    out[2] = r * (i32)scale;
    out[3] = b * (i32)scale;
    return 1;
}
/* the game's semaphores waited for on the queue, then each image shown (renderer.h present_image) */
static void gvk_QueuePresentKHR(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    u64 pi = c->x[1];
    if (c->x[0] != U(g_vk.gfx) || !pi || !mem_user_mapped(pi, sizeof(VkPresentInfoKHR))) {
        ret_vk(c, VK_ERROR_INITIALIZATION_FAILED);
        return;
    }
    u32 nw = gr32(pi + offsetof(VkPresentInfoKHR, waitSemaphoreCount));
    u32 nsc = gr32(pi + offsetof(VkPresentInfoKHR, swapchainCount));
    u64 waits = gr64(pi + offsetof(VkPresentInfoKHR, pWaitSemaphores));
    u64 chains = gr64(pi + offsetof(VkPresentInfoKHR, pSwapchains));
    u64 indices = gr64(pi + offsetof(VkPresentInfoKHR, pImageIndices));
    u64 results = gr64(pi + offsetof(VkPresentInfoKHR, pResults));
    GUEST(semaphores, VkSemaphore, waits, nw);
    GUEST(swapchains, u64, chains, nsc);
    GUEST(image_indices, u32, indices, nsc);
    if (results) { REQUIRE_OUT(results, 4ull * nsc); }
    VkResult r = VK_SUCCESS;
    if (nw) {
        VkPipelineStageFlags* stages = gvk_scratch(sizeof *stages * nw);
        for (u32 i = 0; i < nw; i++) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                           .waitSemaphoreCount = nw,
                           .pWaitSemaphores = semaphores,
                           .pWaitDstStageMask = stages};
        r = queue_submit(1, &si, VK_NULL_HANDLE);
        if (r != VK_SUCCESS) {
            ret_vk(c, r);
            return;
        }
    }
    for (u32 i = 0; i < nsc; i++) {
        pm_lock(&g_gvk_api.lock);
        GvkSwapchain* s = swapchain_of(swapchains[i]);
        u32 idx = image_indices[i];
        u64 win = g_gvk_api.vi_window;
        VkResult one = s && idx < s->n && (s->acquired & (1u << idx)) ? VK_SUCCESS : VK_ERROR_OUT_OF_DATE_KHR;
        VkImage img = one == VK_SUCCESS ? s->img[idx] : VK_NULL_HANDLE;
        VkFormat fmt = one == VK_SUCCESS ? s->fmt : VK_FORMAT_UNDEFINED;
        u32 w = one == VK_SUCCESS ? s->w : 0, h = one == VK_SUCCESS ? s->h : 0, sc = one == VK_SUCCESS ? s->scale : 1;
        i32 crop[4] = {0};
        int has_crop = one == VK_SUCCESS && vi_window_crop(win, w / sc, h / sc, sc, crop);
        if (has_crop && memcmp(crop, s->crop, sizeof crop)) {
            LOG_LIMIT(16, "gvk: crop %d,%d to %d,%d\n", crop[0] / (i32)sc, crop[1] / (i32)sc, crop[2] / (i32)sc,
                      crop[3] / (i32)sc);
            memcpy(s->crop, crop, sizeof crop);
        }
        pm_unlock(&g_gvk_api.lock);
        if (one == VK_SUCCESS) {
            if (!g_gvk_api.presents++) LOG("gvk: the game's first present\n");
            present_image(U(img), (u32)fmt, w, h, has_crop ? crop : NULL);
            pm_lock(&g_gvk_api.lock);
            s->shown[idx] = vk_display_frames();
            s->acquired &= ~(1u << idx);
            pc_broadcast(&g_gvk_api.sc_available);
            pm_unlock(&g_gvk_api.lock);
        }
        if (results) gw32(results + 4ull * i, (u32)one);
        if (one != VK_SUCCESS) r = one;
    }
    ret_vk(c, r);
}

/* ------------------------------------------------------------ the loader */
/* the game's own export for name, where gvk implements it: a function it doesn't is said to be none. The name of an
   extension that Vulkan 1.1 took in is the core command's with KHR at its end */
static void proc_addr(CPU* RESTRICT c, u64 name_at) {
    char name[128];
    u64 a = 0;
    int len = mem_user_string(name, name_at, sizeof name);
    if (len > 0) {
        if (gvk_api_find(name)) a = gvk_export(name);
        size_t n = strlen(name);
        if (!a && n > 3 && !strcmp(name + n - 3, "KHR")) {
            char core[sizeof name];
            memcpy(core, name, n - 3);
            core[n - 3] = 0;
            if (gvk_api_find(core)) a = gvk_export(core);
        }
        if (!a) LOG_LIMIT(64, "gvk: the game asked for %s, which isn't implemented: it is told there is none\n", name);
    }
    ret(c, a);
}
static void gvk_GetInstanceProcAddr(CPU* RESTRICT c, u64 entry) { proc_addr(c, c->x[1]); }
static void gvk_GetDeviceProcAddr(CPU* RESTRICT c, u64 entry) { proc_addr(c, c->x[1]); }

#define E(NAME)      {"vk" #NAME, gvk_##NAME}
#define AS(NAME, FN) {"vk" #NAME, gvk_##FN}
static const struct {
    const char* name;
    GuestFn fn;
} g_api[] = {
    E(GetInstanceProcAddr),
    E(GetDeviceProcAddr),
    E(EnumerateInstanceVersion),
    E(EnumerateInstanceExtensionProperties),
    E(EnumerateDeviceExtensionProperties),
    E(EnumerateInstanceLayerProperties),
    E(EnumerateDeviceLayerProperties),
    E(CreateInstance),
    AS(DestroyInstance, DestroyNothing),
    E(CreateDebugReportCallbackEXT),
    E(DestroyDebugReportCallbackEXT),
    E(DebugReportMessageEXT),
    E(EnumeratePhysicalDevices),
    E(GetPhysicalDeviceProperties),
    E(GetPhysicalDeviceProperties2),
    E(GetPhysicalDeviceFeatures),
    E(GetPhysicalDeviceFeatures2),
    E(GetPhysicalDeviceMemoryProperties),
    E(GetPhysicalDeviceMemoryProperties2),
    E(GetPhysicalDeviceQueueFamilyProperties),
    E(GetPhysicalDeviceFormatProperties),
    E(GetPhysicalDeviceImageFormatProperties),
    E(EnumeratePhysicalDeviceGroups),
    E(GetPhysicalDeviceQueueFamilyProperties2),
    E(GetPhysicalDeviceFormatProperties2),
    E(GetPhysicalDeviceImageFormatProperties2),
    E(GetPhysicalDeviceExternalBufferProperties),
    E(GetPhysicalDeviceExternalFenceProperties),
    E(GetPhysicalDeviceExternalSemaphoreProperties),
    E(GetPhysicalDeviceSparseImageFormatProperties),
    E(GetPhysicalDeviceSparseImageFormatProperties2),
    E(CreateDevice),
    AS(DestroyDevice, DestroyNothing),
    E(GetDeviceQueue),
    E(GetDeviceQueue2),
    E(GetDeviceGroupPeerMemoryFeatures),
    E(GetDeviceMemoryCommitment),
    E(GetRenderAreaGranularity),
    E(QueueBindSparse),
    E(DeviceWaitIdle),
    E(QueueWaitIdle),
    E(QueueSubmit),
    E(AllocateMemory),
    E(FreeMemory),
    E(MapMemory),
    AS(UnmapMemory, DestroyNothing),
    E(FlushMappedMemoryRanges),
    AS(InvalidateMappedMemoryRanges, FlushMappedMemoryRanges),
    E(GetBufferMemoryRequirements),
    E(GetImageMemoryRequirements),
    E(GetBufferMemoryRequirements2),
    E(GetImageMemoryRequirements2),
    E(BindBufferMemory),
    E(BindImageMemory),
    E(BindBufferMemory2),
    E(BindImageMemory2),
    E(GetImageSparseMemoryRequirements),
    E(GetImageSparseMemoryRequirements2),
    E(CreateSamplerYcbcrConversion),
    AS(DestroySamplerYcbcrConversion, DestroyNothing),
    E(CreateBuffer),
    E(DestroyBuffer),
    E(CreateImage),
    E(DestroyImage),
    E(GetImageSubresourceLayout),
    E(CreateImageView),
    E(DestroyImageView),
    E(CreateBufferView),
    E(DestroyBufferView),
    E(CreateSampler),
    E(DestroySampler),
    E(CreateShaderModule),
    E(DestroyShaderModule),
    E(CreatePipelineCache),
    E(DestroyPipelineCache),
    E(GetPipelineCacheData),
    E(MergePipelineCaches),
    E(CreatePipelineLayout),
    E(DestroyPipelineLayout),
    E(CreateDescriptorSetLayout),
    E(DestroyDescriptorSetLayout),
    E(CreateDescriptorPool),
    E(DestroyDescriptorPool),
    E(ResetDescriptorPool),
    E(AllocateDescriptorSets),
    E(FreeDescriptorSets),
    E(UpdateDescriptorSets),
    E(CreateDescriptorUpdateTemplate),
    E(DestroyDescriptorUpdateTemplate),
    E(UpdateDescriptorSetWithTemplate),
    E(GetDescriptorSetLayoutSupport),
    E(CreateRenderPass),
    E(DestroyRenderPass),
    E(CreateFramebuffer),
    E(DestroyFramebuffer),
    E(CreateGraphicsPipelines),
    E(CreateComputePipelines),
    E(DestroyPipeline),
    E(CreateCommandPool),
    E(DestroyCommandPool),
    E(ResetCommandPool),
    E(TrimCommandPool),
    E(AllocateCommandBuffers),
    E(FreeCommandBuffers),
    E(BeginCommandBuffer),
    E(EndCommandBuffer),
    E(ResetCommandBuffer),
    E(CmdBindPipeline),
    E(CmdBindIndexBuffer),
    E(CmdBindVertexBuffers),
    E(CmdBindDescriptorSets),
    E(CmdPushConstants),
    E(CmdSetViewport),
    E(CmdSetScissor),
    E(CmdSetLineWidth),
    E(CmdSetDepthBias),
    E(CmdSetBlendConstants),
    E(CmdSetDepthBounds),
    E(CmdSetStencilCompareMask),
    E(CmdSetStencilWriteMask),
    E(CmdSetStencilReference),
    E(CmdDraw),
    E(CmdDrawIndexed),
    E(CmdDrawIndirect),
    E(CmdDrawIndexedIndirect),
    E(CmdDispatch),
    E(CmdDispatchBase),
    E(CmdSetDeviceMask),
    E(CmdDispatchIndirect),
    E(CmdBeginRenderPass),
    E(CmdNextSubpass),
    E(CmdEndRenderPass),
    E(CmdExecuteCommands),
    E(CmdPipelineBarrier),
    E(CmdWaitEvents),
    E(CmdCopyBuffer),
    E(CmdCopyImage),
    E(CmdBlitImage),
    E(CmdResolveImage),
    E(CmdCopyBufferToImage),
    E(CmdCopyImageToBuffer),
    E(CmdUpdateBuffer),
    E(CmdFillBuffer),
    E(CmdClearColorImage),
    E(CmdClearDepthStencilImage),
    E(CmdClearAttachments),
    E(CmdResetQueryPool),
    E(CmdBeginQuery),
    E(CmdEndQuery),
    E(CmdWriteTimestamp),
    E(CmdCopyQueryPoolResults),
    E(CmdSetEvent),
    E(CmdResetEvent),
    E(CreateFence),
    E(DestroyFence),
    E(ResetFences),
    E(GetFenceStatus),
    E(WaitForFences),
    E(CreateSemaphore),
    E(DestroySemaphore),
    E(CreateEvent),
    E(DestroyEvent),
    E(GetEventStatus),
    E(SetEvent),
    E(ResetEvent),
    E(CreateQueryPool),
    E(DestroyQueryPool),
    E(GetQueryPoolResults),
    E(CreateViSurfaceNN),
    E(DestroySurfaceKHR),
    E(GetPhysicalDeviceSurfaceSupportKHR),
    E(GetPhysicalDeviceSurfaceCapabilitiesKHR),
    E(GetPhysicalDeviceSurfaceFormatsKHR),
    E(GetPhysicalDeviceSurfacePresentModesKHR),
    E(CreateSwapchainKHR),
    E(DestroySwapchainKHR),
    E(GetSwapchainImagesKHR),
    E(AcquireNextImageKHR),
    E(QueuePresentKHR),
};

GuestFn gvk_api_find(const char* name) {
    for (size_t i = 0; i < sizeof g_api / sizeof *g_api; i++)
        if (!strcmp(g_api[i].name, name)) return g_api[i].fn;
    return NULL;
}
