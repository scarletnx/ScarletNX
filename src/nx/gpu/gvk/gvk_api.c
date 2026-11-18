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
     dequeue does. */
#include "nx/gpu/gvk/gvk.h"
#include "nx/gpu/renderer.h"
#include "nx/gpu/vk/vk.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <string.h>

#define GVK_MEMS       (1u << 16) /* the memory allocations the game holds at once, at most */
#define GVK_IMAGES     (1u << 16) /* the images it holds at once that can be bound to guest memory, at most */
#define GVK_RPASSES    (1u << 12) /* its render passes */
#define GVK_CBS        (1u << 12) /* its command buffers inside a render pass at once */
#define GVK_SCALED     (1u << 14) /* its images, views and framebuffers drawn at the resolution scale */
#define GVK_DUALS      (1u << 14) /* its fragment modules with a second color at Location 1 */
#define GVK_SWAPCHAINS 4
#define GVK_SC_IMAGES  4
#define GVK_SURFACE    0x5653000000000001ull /* the vi layer's surface: the game's only one */
#define GVK_GUEST_API  VK_API_VERSION_1_1    /* what the game is told: its loader has Vulkan 1.1's functions */
#define GVK_HEAP       (4ull << 30)          /* the one heap the game is told of: the console's memory */

typedef struct {
    u64 mem;  /* the host's VkDeviceMemory, 0 for a free slot */
    u64 va;   /* where the game maps it, 0 for device-local memory */
    u64 size; /* the guest pages at va */
} GvkMem;

/* a render pass, and its twin that goes on with what the first part left (pass_back); self: a subpass depends on
   itself, so a barrier is legal inside it */
typedef struct {
    u64 rp, resume;
    u32 subpasses, self;
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

/* An image drawn at the resolution scale: its extent as the game made it. Its views and the framebuffers of them are
   scaled too (sets of their handles) */
typedef struct {
    u64 img;
    u32 w, h;
} GvkScaled;

/* a command buffer's first viewport and scissor as the game set them, and the scale each was set at: a render pass
   at another scale sets them again at its own */
typedef struct {
    u64 cb;
    VkViewport vp;
    VkRect2D sc;
    u32 has_vp, has_sc, vp_at, sc_at;
} GvkDyn;

typedef struct {
    u32 n, next;
    u32 acquired; /* a bit for each image the game holds: acquired and not presented yet */
    VkFormat fmt;
    u32 w, h;
    VkImage img[GVK_SC_IMAGES];
    VkDeviceMemory mem[GVK_SC_IMAGES];
} GvkSwapchain;

/* a fragment module of the game, and its twin with the second color where dual-source blending reads it
   (gvk_spirv_dual_source): what a pipeline that blends with it takes */
typedef struct {
    u64 module, dual;
} GvkDual;

static struct {
    PMutex lock;
    int ready;                   /* the memory types are chosen */
    u32 local, coherent, cached; /* the host's memory types behind the game's 0, 1 and 2 */
    u64 import_align;            /* minImportedHostPointerAlignment, or a host page for dma-bufs */
    VkExternalMemoryHandleTypeFlagBits handle_type; /* how the host imports guest pages: host memory or a dma-buf */
    u32 nmem, nimportable, nrpass, npass, nscaled, nsview, nsfb, ndyn, ndual;
    GvkMem mem[GVK_MEMS];       /* open addressing by handle */
    u64 importable[GVK_IMAGES]; /* the images the host imports guest memory for, open addressing */
    GvkRpass rpass[GVK_RPASSES];
    GvkPass pass[GVK_CBS];
    GvkScaled scaled[GVK_SCALED];
    u64 sview[GVK_SCALED], sfb[GVK_SCALED];
    GvkDyn dyn[GVK_CBS];
    GvkDual dual[GVK_DUALS];
    GvkSwapchain sc[GVK_SWAPCHAINS];
    u64 presents;
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
    if (!g || !mem_user_mapped(g, n)) return 0;
    memcpy(GP(g), src, n);
    return 1;
}
static void put_handle(u64 g, u64 h) {
    if (g && mem_user_mapped(g, 8)) gw64(g, h);
}
/* a count and an array the game asked for (Vulkan's two-call idiom): *pCount set, at most *pCount of the n elements of
   elem bytes written. VK_INCOMPLETE when some didn't fit */
static VkResult put_list(u64 p_count, u64 p_out, const void* items, u32 n, size_t elem) {
    if (!p_count || !mem_user_mapped(p_count, 4)) return VK_ERROR_INITIALIZATION_FAILED;
    if (!p_out) {
        gw32(p_count, n);
        return VK_SUCCESS;
    }
    u32 room = gr32(p_count), k = room < n ? room : n;
    if (k) put(p_out, items, k * elem);
    gw32(p_count, k);
    return k < n ? VK_INCOMPLETE : VK_SUCCESS;
}

/* the graphics queue, which the renderer's recording thread also submits to: after the frames it has queued */
static VkResult queue_submit(u32 n, const VkSubmitInfo* s, VkFence fence) {
    vk_lock();
    vk_rec_queue(g_vk.gfx);
    vk_queue_lock(g_vk.gfx);
    VkResult r = vkQueueSubmit(g_vk.gfx, n, s, fence);
    vk_queue_unlock(g_vk.gfx);
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
static void tab_put(u8* t, size_t size, u32 cap, u32* n, const void* e) {
    u64 key;
    memcpy(&key, e, sizeof key);
    pm_lock(&g_gvk_api.lock);
    u32 i = tab_slot(t, size, cap, key);
    if (!tab_key(t, size, i)) {
        if (*n == cap - 1) fatal("gvk: the game holds more than %u objects of one kind", cap - 1);
        ++*n;
    }
    memcpy(t + (size_t)i * size, e, size);
    pm_unlock(&g_gvk_api.lock);
}
/* the entry of key copied to out (zeros for none): whether there is one */
static int tab_get(const u8* t, size_t size, u32 cap, u64 key, void* out) {
    pm_lock(&g_gvk_api.lock);
    u32 i = tab_slot(t, size, cap, key);
    int has = key && tab_key(t, size, i) == key;
    memcpy(out, t + (size_t)i * size, size);
    pm_unlock(&g_gvk_api.lock);
    return has;
}
static int tab_take(u8* t, size_t size, u32 cap, u32* n, u64 key, void* out) {
    pm_lock(&g_gvk_api.lock);
    u32 i = tab_slot(t, size, cap, key);
    int has = key && tab_key(t, size, i) == key;
    memcpy(out, t + (size_t)i * size, size);
    if (has) {
        --*n;
        memset(t + (size_t)i * size, 0, size);
        u8 e[64];
        for (u32 j = (i + 1) & (cap - 1); tab_key(t, size, j); j = (j + 1) & (cap - 1)) {
            memcpy(e, t + (size_t)j * size, size);
            memset(t + (size_t)j * size, 0, size);
            memcpy(t + (size_t)tab_slot(t, size, cap, tab_key(e, size, 0)) * size, e, size);
        }
    }
    pm_unlock(&g_gvk_api.lock);
    return has;
}
typedef char gvk_entries_fit[sizeof(GvkMem) <= 64 && sizeof(GvkRpass) <= 64 && sizeof(GvkPass) <= 64 &&
                                     sizeof(GvkScaled) <= 64 && sizeof(GvkDyn) <= 64
                                 ? 1
                                 : -1];

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
static void importable_put(u64 img) { tab_put(TAB(importable), &g_gvk_api.nimportable, &img); }
static int importable_has(u64 img) {
    u64 k;
    return tab_get(TAB(importable), img, &k);
}
static void importable_take(u64 img) {
    u64 k;
    tab_take(TAB(importable), &g_gvk_api.nimportable, img, &k);
}

/* ------------------------------------------------------------ the resolution scale */
/* The resolution setting as a whole factor, 1 for none. The game's color and depth targets are made that much larger
   (scale_wanted), and the commands that name their pixels are scaled with them: render areas, viewports, scissors,
   clear rectangles, copies and blits. A copy between a scaled image and one that isn't becomes a blit */
static u32 scale_factor(void) {
    float s = g_settings.resolution_scale;
    return s >= 1.5f ? (u32)(s + 0.5f) : 1;
}
/* An image to draw at the scale: a 2D color or depth target of one level at least the console's screen. A game can
   make every texture a color target (one does, usage 0x17): a texture scaled up by a blit is filtered twice, and
   takes the square of the scale in memory. Smaller targets stay at the game's size, and what is copied between the
   two is blitted */
static int scale_wanted(const VkImageCreateInfo* ci) {
    return scale_factor() > 1 && ci->imageType == VK_IMAGE_TYPE_2D && ci->tiling == VK_IMAGE_TILING_OPTIMAL &&
           ci->mipLevels == 1 &&
           (ci->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) &&
           ci->extent.width >= 1280 && ci->extent.height >= 720;
}
static void scaled_put(u64 img, u32 w, u32 h) {
    GvkScaled e = {img, w, h};
    tab_put(TAB(scaled), &g_gvk_api.nscaled, &e);
}
/* the factor image is drawn at: the scale, or 1 */
static u32 image_scale(u64 img) {
    GvkScaled e;
    return img && scale_factor() > 1 && tab_get(TAB(scaled), img, &e) ? scale_factor() : 1;
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
static VkRect2D rect_at(VkRect2D r, u32 f) {
    r.offset.x = (i32)((u32)r.offset.x * f);
    r.offset.y = (i32)((u32)r.offset.y * f);
    r.extent.width *= f;
    r.extent.height *= f;
    return r;
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
}

/* once: how the host imports guest pages, and its types behind the game's three */
static void types_ready(void) {
    if (__atomic_load_n(&g_gvk_api.ready, __ATOMIC_ACQUIRE)) return;
    pm_lock(&g_gvk_api.lock);
    if (!g_gvk_api.ready) {
        u32 bits = 0;
        g_gvk_api.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        if (vkGetMemoryHostPointerPropertiesEXT) bits = host_import_bits();
        if (!bits) {
            g_gvk_api.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
            bits = dmabuf_export_bits();
        }
        if (!bits)
            fatal("gvk: the renderer's device neither imports guest memory as host memory "
                  "(VK_EXT_external_memory_host) nor exports its own as a dma-buf (VK_EXT_external_memory_dma_buf), "
                  "which the game's mapped memory needs");
        const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        g_gvk_api.local = first_type(~0u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        if (g_gvk_api.local == ~0u) g_gvk_api.local = first_type(~0u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        g_gvk_api.coherent = first_type(bits, hv, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        g_gvk_api.cached = first_type(bits, hv | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
        if (g_gvk_api.coherent == ~0u) g_gvk_api.coherent = g_gvk_api.cached;
        if (g_gvk_api.cached == ~0u) g_gvk_api.cached = g_gvk_api.coherent;
        if (g_gvk_api.local == ~0u || g_gvk_api.coherent == ~0u)
            fatal("gvk: no memory type for the game's (device-local %d, imported %#x)", (int)g_gvk_api.local, bits);
        LOG("gvk: the game's memory types are the host's %u (device-local), %u and %u (%s; the host's types for "
            "them %#x, aligned to %llu bytes)\n",
            g_gvk_api.local, g_gvk_api.coherent, g_gvk_api.cached,
            g_gvk_api.handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
                ? "the host's memory, as dma-bufs mapped over guest pages"
                : "guest pages, imported as host memory",
            bits, (unsigned long long)g_gvk_api.import_align);
        __atomic_store_n(&g_gvk_api.ready, 1, __ATOMIC_RELEASE);
    }
    pm_unlock(&g_gvk_api.lock);
}

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
static void gvk_EnumerateInstanceVersion(CPU* RESTRICT c, u64 entry) {
    (void)entry;
    if (c->x[0] && mem_user_mapped(c->x[0], 4)) gw32(c->x[0], GVK_GUEST_API);
    ret_vk(c, VK_SUCCESS);
}

static void gvk_EnumerateInstanceExtensionProperties(CPU* RESTRICT c, u64 entry) {
    VkExtensionProperties e[2] = {{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_SURFACE_SPEC_VERSION},
                                  {"VK_NN_vi_surface", 1}};
    ret_vk(c, put_list(c->x[1], c->x[2], e, 2, sizeof *e));
}

static void gvk_EnumerateDeviceExtensionProperties(CPU* RESTRICT c, u64 entry) {
    (void)entry;
    VkExtensionProperties e[1] = {{VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_SWAPCHAIN_SPEC_VERSION}};
    ret_vk(c, put_list(c->x[2], c->x[3], e, 1, sizeof *e));
}

static void gvk_EnumerateInstanceLayerProperties(CPU* RESTRICT c, u64 entry) {
    ret_vk(c, put_list(c->x[0], 0, NULL, 0, sizeof(VkLayerProperties)));
}
static void gvk_EnumerateDeviceLayerProperties(CPU* RESTRICT c, u64 entry) {
    ret_vk(c, put_list(c->x[1], 0, NULL, 0, sizeof(VkLayerProperties)));
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
    LOG("gvk: the game's %s, extensions: %s\n", what, at ? line : "none");
}

static void gvk_CreateInstance(CPU* RESTRICT c, u64 entry) {
    (void)entry;
    u64 ci = c->x[0];
    if (ci && mem_user_mapped(ci, sizeof(VkInstanceCreateInfo)))
        say_extensions("instance", gr64(ci + offsetof(VkInstanceCreateInfo, ppEnabledExtensionNames)),
                       gr32(ci + offsetof(VkInstanceCreateInfo, enabledExtensionCount)));
    put_handle(c->x[2], U(g_vk.instance));
    ret_vk(c, VK_SUCCESS);
}

static void gvk_DestroyNothing(CPU* RESTRICT c, u64 entry) { ret(c, 0); }

static void gvk_EnumeratePhysicalDevices(CPU* RESTRICT c, u64 entry) {
    u64 phys = U(g_vk.phys);
    ret_vk(c, put_list(c->x[1], c->x[2], &phys, 1, sizeof phys));
}

static void gvk_GetPhysicalDeviceProperties(CPU* RESTRICT c, u64 entry) {
    VkPhysicalDeviceProperties p = g_vk.props;
    if (p.apiVersion > GVK_GUEST_API) p.apiVersion = GVK_GUEST_API;
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
    VkBaseOutStructure* tail = (VkBaseOutStructure*)&p2;
    u64 at[GVK_CHAIN];
    u32 size[GVK_CHAIN], n = 0;
    VkBaseOutStructure* host[GVK_CHAIN];
    u64 a = gr64(g + offsetof(VkPhysicalDeviceProperties2, pNext));
    for (u32 hops = 0; a && hops < 64 && mem_user_mapped(a, sizeof(VkBaseOutStructure)); hops++, a = gr64(a + 8)) {
        VkStructureType st = (VkStructureType)gr32(a);
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
        tail->pNext = h;
        tail = h;
        at[n] = a;
        size[n] = bytes;
        host[n++] = h;
    }
    vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
    if (p2.properties.apiVersion > GVK_GUEST_API) p2.properties.apiVersion = GVK_GUEST_API;
    put(g + offsetof(VkPhysicalDeviceProperties2, properties), &p2.properties, sizeof p2.properties);
    for (u32 i = 0; i < n; i++) put(at[i] + sizeof(VkBaseOutStructure), host[i] + 1, size[i] - sizeof *host[i]);
    ret(c, 0);
}
/* what the renderer's device has on: the game shares it */
static void gvk_GetPhysicalDeviceFeatures(CPU* RESTRICT c, u64 entry) {
    put(c->x[1], &g_vk.feat2.features, sizeof g_vk.feat2.features);
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
    put(g + offsetof(VkPhysicalDeviceFeatures2, features), &g_vk.feat2.features, sizeof g_vk.feat2.features);
    const VkPhysicalDeviceVulkan11Features* f = &g_vk.feat11;
    const size_t head = sizeof(VkBaseOutStructure);
    u64 a = gr64(g + offsetof(VkPhysicalDeviceFeatures2, pNext));
    for (u32 hops = 0; a && hops < 64 && mem_user_mapped(a, head); hops++, a = gr64(a + 8)) {
        VkStructureType st = (VkStructureType)gr32(a);
        switch (st) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
            put(a + head, (const u8*)&g_vk.feat11 + head, sizeof g_vk.feat11 - head);
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
            VkPhysicalDeviceVariablePointersFeatures s = {
                .variablePointersStorageBuffer = f->variablePointersStorageBuffer,
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
            VkPhysicalDeviceSamplerYcbcrConversionFeatures s = {.samplerYcbcrConversion = f->samplerYcbcrConversion};
            put(a + head, (const u8*)&s + head, sizeof s - head);
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES: {
            VkPhysicalDeviceShaderDrawParametersFeatures s = {.shaderDrawParameters = f->shaderDrawParameters};
            put(a + head, (const u8*)&s + head, sizeof s - head);
            break;
        }
        default: LOG_LIMIT(16, "gvk: a features structure of type %u is left as the game wrote it\n", (u32)st); break;
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
/* one family: the renderer's graphics queue */
static void gvk_GetPhysicalDeviceQueueFamilyProperties(CPU* RESTRICT c, u64 entry) {
    VkQueueFamilyProperties q = {
        VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT, 1, g_vk.gfx_timestamp_bits, {1, 1, 1}};
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

static void gvk_CreateDevice(CPU* RESTRICT c, u64 entry) {
    u64 ci = c->x[1];
    if (ci && mem_user_mapped(ci, sizeof(VkDeviceCreateInfo)))
        say_extensions("device", gr64(ci + offsetof(VkDeviceCreateInfo, ppEnabledExtensionNames)),
                       gr32(ci + offsetof(VkDeviceCreateInfo, enabledExtensionCount)));
    types_ready();
    put_handle(c->x[3], U(g_vk.dev));
    ret_vk(c, VK_SUCCESS);
}
static void gvk_GetDeviceQueue(CPU* RESTRICT c, u64 entry) {
    put_handle(c->x[3], U(g_vk.gfx));
    ret(c, 0);
}
static void gvk_DeviceWaitIdle(CPU* RESTRICT c, u64 entry) {
    kthread_host_wait_begin();
    vk_lock();
    vk_rec_queue(g_vk.gfx);
    vk_queue_lock(g_vk.gfx);
    VkResult r = vkQueueWaitIdle(g_vk.gfx);
    vk_queue_unlock(g_vk.gfx);
    vk_unlock();
    kthread_host_wait_end();
    ret_vk(c, r);
}

/* ------------------------------------------------------------ memory */
static void gvk_AllocateMemory(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    u64 ai = c->x[1];
    if (!ai || !mem_user_mapped(ai, sizeof(VkMemoryAllocateInfo))) {
        ret_vk(c, VK_ERROR_INITIALIZATION_FAILED);
        return;
    }
    types_ready();
    u64 size = gr64(ai + offsetof(VkMemoryAllocateInfo, allocationSize));
    u32 type = gr32(ai + offsetof(VkMemoryAllocateInfo, memoryTypeIndex));
    VkMemoryAllocateInfo hi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = size};
    VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkExportMemoryAllocateInfo exp = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                                      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    int dmabuf = g_gvk_api.handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    GvkMem m = {0};
    if (type == 0) {
        hi.memoryTypeIndex = g_gvk_api.local;
        hi.pNext = gvk_chain_in(gr64(ai + 8)); /* a dedicated allocation the host requires stays one */
    } else if (type <= 2) {
        m.size = (size + g_gvk_api.import_align - 1) & ~(g_gvk_api.import_align - 1);
        m.va = guest_pages(m.size, g_gvk_api.import_align);
        if (!m.va) {
            WARN_LIMIT(8, "gvk: no guest memory for an allocation of %llu bytes\n", (unsigned long long)size);
            ret_vk(c, VK_ERROR_OUT_OF_HOST_MEMORY);
            return;
        }
        hi.allocationSize = m.size;
        hi.memoryTypeIndex = type == 1 ? g_gvk_api.coherent : g_gvk_api.cached;
        if (dmabuf) {
            hi.pNext = &exp;
        } else {
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
            moved = mem_migrate(m.va, m.size, fd);
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
        vkFreeMemory(g_vk.dev, H(VkDeviceMemory, c->x[1]), NULL);
        if (m.va) mem_unmap(m.va, m.size);
    }
    ret(c, 0);
}
static void gvk_MapMemory(CPU* RESTRICT c, u64 entry) {
    GvkMem m = mem_get(c->x[1]);
    if (!m.va) {
        ret_vk(c, VK_ERROR_MEMORY_MAP_FAILED);
        return;
    }
    put_handle(c->x[5], m.va + c->x[2]);
    ret_vk(c, VK_SUCCESS);
}
/* the CPU's types are coherent: a flush or an invalidate does nothing */
static void gvk_FlushMappedMemoryRanges(CPU* RESTRICT c, u64 entry) { ret_vk(c, VK_SUCCESS); }

static void requirements_out(u64 g, VkMemoryRequirements* r) {
    r->memoryTypeBits = guest_bits(r->memoryTypeBits);
    if (!r->memoryTypeBits)
        WARN_LIMIT(8, "gvk: a resource takes none of the game's memory types (%llu bytes)\n",
                   (unsigned long long)r->size);
    put(g, r, sizeof *r);
}
static void gvk_GetBufferMemoryRequirements(CPU* RESTRICT c, u64 entry) {
    VkMemoryRequirements r = {0};
    vkGetBufferMemoryRequirements(g_vk.dev, H(VkBuffer, c->x[1]), &r);
    requirements_out(c->x[2], &r);
    ret(c, 0);
}
/* an image the host imports no guest memory for takes device-local memory alone */
static void gvk_GetImageMemoryRequirements(CPU* RESTRICT c, u64 entry) {
    VkMemoryRequirements r = {0};
    vkGetImageMemoryRequirements(g_vk.dev, H(VkImage, c->x[1]), &r);
    if (!importable_has(c->x[1])) r.memoryTypeBits &= 1u << g_gvk_api.local;
    requirements_out(c->x[2], &r);
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
        if (image && !importable_has(U(((const VkImageMemoryRequirementsInfo2*)info)->image)))
            r->memoryRequirements.memoryTypeBits &= 1u << g_gvk_api.local;
        r->memoryRequirements.memoryTypeBits = guest_bits(r->memoryRequirements.memoryTypeBits);
        gvk_out(&gvk_t_req2, c->x[2], r);
    }
    ret(c, 0);
}
static void gvk_GetImageMemoryRequirements2(CPU* RESTRICT c, u64 entry) { requirements2(c, 1); }
static void gvk_GetBufferMemoryRequirements2(CPU* RESTRICT c, u64 entry) { requirements2(c, 0); }
static void gvk_BindBufferMemory(CPU* RESTRICT c, u64 entry) {
    ret_vk(c, vkBindBufferMemory(g_vk.dev, H(VkBuffer, c->x[1]), H(VkDeviceMemory, c->x[2]), c->x[3]));
}
static void gvk_BindImageMemory(CPU* RESTRICT c, u64 entry) {
    ret_vk(c, vkBindImageMemory(g_vk.dev, H(VkImage, c->x[1]), H(VkDeviceMemory, c->x[2]), c->x[3]));
}

/* ------------------------------------------------------------ objects */
/* vkCreateX(device, pCreateInfo, pAllocator, pX): the info copied as type t, the handle written where the game asked */
#define CREATE(NAME, T, TYPE)                                                                                          \
    static void gvk_##NAME(CPU* RESTRICT c, u64 entry) {                                                               \
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
    gvk_scratch_reset();
    VkImageViewCreateInfo* ci = gvk_in(&gvk_t_image_view, c->x[1], 1);
    VkImageView h = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (ci) {
        ci->format = host_format(ci->format);
        r = vkCreateImageView(g_vk.dev, ci, NULL, &h);
        if (r == VK_SUCCESS && image_scale(U(ci->image)) > 1)
            set_put(g_gvk_api.sview, GVK_SCALED, &g_gvk_api.nsview, U(h));
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
    gvk_scratch_reset();
    VkRenderPassCreateInfo* ci = gvk_in(&gvk_t_rpass, c->x[1], 1);
    VkRenderPass h = VK_NULL_HANDLE, resume = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    u32 self = 0;
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
            a[i].finalLayout = host_layout(a[i].finalLayout);
            if (split) a[i].storeOp = a[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        VkSubpassDescription* sp = (VkSubpassDescription*)ci->pSubpasses;
        for (u32 i = 0; sp && i < ci->subpassCount; i++) {
            sp[i].pInputAttachments = refs_in(sp[i].pInputAttachments, sp[i].inputAttachmentCount);
            sp[i].pColorAttachments = refs_in(sp[i].pColorAttachments, sp[i].colorAttachmentCount);
            sp[i].pResolveAttachments = refs_in(sp[i].pResolveAttachments, sp[i].colorAttachmentCount);
            sp[i].pDepthStencilAttachment = refs_in(sp[i].pDepthStencilAttachment, 1);
        }
        for (u32 i = 0; ci->pDependencies && i < ci->dependencyCount; i++)
            self |= ci->pDependencies[i].srcSubpass == ci->pDependencies[i].dstSubpass;
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
        GvkRpass e = {U(h), U(resume), ci->subpassCount, self};
        tab_put(TAB(rpass), &g_gvk_api.nrpass, &e);
    }
    put_handle(c->x[3], U(h));
    ret_vk(c, r);
}
static void gvk_DestroyRenderPass(CPU* RESTRICT c, u64 entry) {
    GvkRpass e;
    if (tab_take(TAB(rpass), &g_gvk_api.nrpass, c->x[1], &e) && e.resume)
        vkDestroyRenderPass(g_vk.dev, H(VkRenderPass, e.resume), NULL);
    vkDestroyRenderPass(g_vk.dev, H(VkRenderPass, c->x[1]), NULL);
    ret(c, 0);
}
CREATE(CreateBufferView, VkBufferView, gvk_t_buffer_view)
CREATE(CreateSampler, VkSampler, gvk_t_sampler)
CREATE(CreatePipelineLayout, VkPipelineLayout, gvk_t_playout)
CREATE(CreateDescriptorPool, VkDescriptorPool, gvk_t_dspool)
/* a framebuffer of a scaled image's view is scaled with it */
static void gvk_CreateFramebuffer(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    VkFramebufferCreateInfo* ci = gvk_in(&gvk_t_fb, c->x[1], 1);
    VkFramebuffer h = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (ci) {
        int scaled = 0;
        for (u32 i = 0; ci->pAttachments && i < ci->attachmentCount; i++)
            scaled |= set_has(g_gvk_api.sview, GVK_SCALED, U(ci->pAttachments[i]));
        if (scaled) {
            ci->width *= scale_factor();
            ci->height *= scale_factor();
        }
        r = vkCreateFramebuffer(g_vk.dev, ci, NULL, &h);
        if (r == VK_SUCCESS && scaled) set_put(g_gvk_api.sfb, GVK_SCALED, &g_gvk_api.nsfb, U(h));
    }
    if (r != VK_SUCCESS) WARN_LIMIT(16, "gvk: vkCreateFramebuffer failed (%d)\n", (int)r);
    put_handle(c->x[3], U(h));
    ret_vk(c, r);
}
CREATE(CreateFence, VkFence, gvk_t_fence)
CREATE(CreateSemaphore, VkSemaphore, gvk_t_sem)
CREATE(CreateQueryPool, VkQueryPool, gvk_t_query)
CREATE(CreateEvent, VkEvent, gvk_t_event)
DESTROY(DestroyBuffer, VkBuffer)
static void gvk_DestroyImageView(CPU* RESTRICT c, u64 entry) {
    set_take(g_gvk_api.sview, GVK_SCALED, &g_gvk_api.nsview, c->x[1]);
    vkDestroyImageView(g_vk.dev, H(VkImageView, c->x[1]), NULL);
    ret(c, 0);
}
DESTROY(DestroyBufferView, VkBufferView)
DESTROY(DestroySampler, VkSampler)
static void gvk_DestroyShaderModule(CPU* RESTRICT c, u64 entry) {
    GvkDual e;
    if (tab_take(TAB(dual), &g_gvk_api.ndual, c->x[1], &e))
        vkDestroyShaderModule(g_vk.dev, H(VkShaderModule, e.dual), NULL);
    vkDestroyShaderModule(g_vk.dev, H(VkShaderModule, c->x[1]), NULL);
    ret(c, 0);
}
DESTROY(DestroyPipelineCache, VkPipelineCache)
DESTROY(DestroyPipelineLayout, VkPipelineLayout)
DESTROY(DestroyDescriptorSetLayout, VkDescriptorSetLayout)
DESTROY(DestroyDescriptorPool, VkDescriptorPool)
static void gvk_DestroyFramebuffer(CPU* RESTRICT c, u64 entry) {
    set_take(g_gvk_api.sfb, GVK_SCALED, &g_gvk_api.nsfb, c->x[1]);
    vkDestroyFramebuffer(g_vk.dev, H(VkFramebuffer, c->x[1]), NULL);
    ret(c, 0);
}
DESTROY(DestroyPipeline, VkPipeline)
DESTROY(DestroyFence, VkFence)
DESTROY(DestroySemaphore, VkSemaphore)
DESTROY(DestroyQueryPool, VkQueryPool)
DESTROY(DestroyEvent, VkEvent)
DESTROY(DestroyCommandPool, VkCommandPool)

/* A buffer or an image of the one queue family: its sharing exclusive. Each buffer can be bound to imported memory, and
   so can each linear image whose format and usage the host imports (image_importable): the images the game maps to
   write texels. An optimal image is told of device-local memory alone (GetImageMemoryRequirements), as an external
   one would need memory of its own (requiresDedicatedAllocation) */
static void gvk_CreateBuffer(CPU* RESTRICT c, u64 entry) {
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
    VkPhysicalDeviceExternalImageFormatInfo ext = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
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
    return vkGetPhysicalDeviceImageFormatProperties2(g_vk.phys, &info, &p) == VK_SUCCESS &&
           (ep.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
}
static void gvk_CreateImage(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    VkImageCreateInfo* ci = gvk_in(&gvk_t_image, c->x[1], 1);
    VkImage h = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (ci) {
        ci->sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci->queueFamilyIndexCount = 0;
        ci->pQueueFamilyIndices = NULL;
        ci->format = host_format(ci->format);
        int importable = ci->tiling == VK_IMAGE_TILING_LINEAR && image_importable(ci);
        VkImageDrmFormatModifierListCreateInfoEXT mods = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
            .pNext = ci->pNext,
            .drmFormatModifierCount = 1,
            .pDrmFormatModifiers = &g_mod_linear};
        VkExternalMemoryImageCreateInfo ext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                                               .pNext = ci->pNext,
                                               .handleTypes = g_gvk_api.handle_type};
        if (importable) {
            /* an external image starts undefined, which a linear one's texels outlive: the game's preinitialized
               ones keep what it writes before their first barrier */
            if (image_modifier()) {
                ext.pNext = &mods;
                ci->tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
            }
            ci->pNext = &ext;
            ci->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        }
        u32 w = ci->extent.width, hgt = ci->extent.height;
        int scaled = scale_wanted(ci);
        if (scaled) {
            ci->extent.width *= scale_factor();
            ci->extent.height *= scale_factor();
        }
        r = vkCreateImage(g_vk.dev, ci, NULL, &h);
        if (r == VK_SUCCESS && scaled) scaled_put(U(h), w, hgt);
        if (r != VK_SUCCESS)
            WARN_LIMIT(32, "gvk: an image of format %d, %ux%ux%u, tiling %d, usage %#x, failed (%d)\n", (int)ci->format,
                       ci->extent.width, ci->extent.height, ci->extent.depth, (int)ci->tiling, ci->usage, (int)r);
        else if (importable)
            importable_put(U(h));
        else if (ci->tiling == VK_IMAGE_TILING_LINEAR)
            LOG_LIMIT(16, "gvk: a linear image of format %d, usage %#x can't be bound to guest memory\n",
                      (int)ci->format, ci->usage);
    }
    put_handle(c->x[3], U(h));
    ret_vk(c, r);
}
static void gvk_DestroyImage(CPU* RESTRICT c, u64 entry) {
    if (c->x[1]) importable_take(c->x[1]);
    if (c->x[1] && scale_factor() > 1) {
        GvkScaled e;
        tab_take(TAB(scaled), &g_gvk_api.nscaled, c->x[1], &e);
    }
    vkDestroyImage(g_vk.dev, H(VkImage, c->x[1]), NULL);
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
    gvk_scratch_reset();
    VkShaderModuleCreateInfo* ci = gvk_in(&gvk_t_shader, c->x[1], 1);
    VkShaderModule h = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (ci && ci->pCode) {
        u32 magic;
        memcpy(&magic, ci->pCode, sizeof magic);
        if (magic != 0x07230203u)
            WARN_LIMIT(4, "gvk: a shader module of %zu bytes isn't SPIR-V (its first word %08x)\n", ci->codeSize,
                       magic);
        size_t bytes = 0;
        const u32* mended = ((uintptr_t)ci->pCode & 3) ? NULL : gvk_spirv_mend(ci->pCode, ci->codeSize, &bytes);
        if (mended) {
            ci->pCode = mended;
            ci->codeSize = bytes;
        }
        r = vkCreateShaderModule(g_vk.dev, ci, NULL, &h);
        const u32* dual = r == VK_SUCCESS && !((uintptr_t)ci->pCode & 3)
                              ? gvk_spirv_dual_source(ci->pCode, ci->codeSize, &bytes)
                              : NULL;
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
    if (!c->x[2] || !mem_user_mapped(c->x[2], 8)) {
        ret_vk(c, VK_ERROR_INITIALIZATION_FAILED);
        return;
    }
    size_t size = gr64(c->x[2]);
    void* data = c->x[3] && mem_user_mapped(c->x[3], size) ? GP(c->x[3]) : NULL;
    VkResult r = vkGetPipelineCacheData(g_vk.dev, H(VkPipelineCache, c->x[1]), &size, data);
    gw64(c->x[2], size);
    ret_vk(c, r);
}
static void gvk_MergePipelineCaches(CPU* RESTRICT c, u64 entry) {
    GUEST(src, VkPipelineCache, c->x[3], (u32)c->x[2]);
    ret_vk(c, vkMergePipelineCaches(g_vk.dev, H(VkPipelineCache, c->x[1]), (u32)c->x[2], src));
}

/* the immutable samplers of a binding of another type are ignored, whatever the pointer holds */
static void gvk_CreateDescriptorSetLayout(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    VkDescriptorSetLayoutCreateInfo* ci = gvk_in(&gvk_t_dslayout, c->x[1], 1);
    VkDescriptorSetLayout h = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (ci) {
        VkDescriptorSetLayoutBinding* b = (VkDescriptorSetLayoutBinding*)ci->pBindings;
        for (u32 i = 0; b && i < ci->bindingCount; i++)
            if (b[i].descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER &&
                b[i].descriptorType != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
                b[i].pImmutableSamplers = NULL;
        r = vkCreateDescriptorSetLayout(g_vk.dev, ci, NULL, &h);
    }
    if (r != VK_SUCCESS) WARN_LIMIT(16, "gvk: vkCreateDescriptorSetLayout failed (%d)\n", (int)r);
    put_handle(c->x[3], U(h));
    ret_vk(c, r);
}
static void gvk_ResetDescriptorPool(CPU* RESTRICT c, u64 entry) {
    ret_vk(c, vkResetDescriptorPool(g_vk.dev, H(VkDescriptorPool, c->x[1]), (VkDescriptorPoolResetFlags)c->x[2]));
}
static void gvk_AllocateDescriptorSets(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    const VkDescriptorSetAllocateInfo* ai = gvk_in(&gvk_t_dsalloc, c->x[1], 1);
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (ai && c->x[2] && mem_user_mapped(c->x[2], 8ull * ai->descriptorSetCount))
        r = vkAllocateDescriptorSets(g_vk.dev, ai, gvk_ptr(c->x[2]));
    if (r != VK_SUCCESS) WARN_LIMIT(16, "gvk: vkAllocateDescriptorSets failed (%d)\n", (int)r);
    ret_vk(c, r);
}
static void gvk_FreeDescriptorSets(CPU* RESTRICT c, u64 entry) {
    GUEST(sets, VkDescriptorSet, c->x[3], (u32)c->x[2]);
    ret_vk(c, vkFreeDescriptorSets(g_vk.dev, H(VkDescriptorPool, c->x[1]), (u32)c->x[2], sets));
}
/* a write reads the one of its three arrays its type names, its images in host_layout */
static void gvk_UpdateDescriptorSets(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    u32 nw = (u32)c->x[1], nc = (u32)c->x[3];
    VkWriteDescriptorSet* w = gvk_in(&gvk_t_write, c->x[2], nw);
    const VkCopyDescriptorSet* cp = gvk_in(&gvk_t_copy, c->x[4], nc);
    for (u32 i = 0; w && i < nw; i++) {
        switch (w[i].descriptorType) {
            case VK_DESCRIPTOR_TYPE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                w[i].pBufferInfo = NULL;
                w[i].pTexelBufferView = NULL;
                if (w[i].pImageInfo && w[i].descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER) {
                    VkDescriptorImageInfo* im = gvk_scratch(sizeof *im * w[i].descriptorCount);
                    for (u32 k = 0; k < w[i].descriptorCount; k++) {
                        im[k] = w[i].pImageInfo[k];
                        im[k].imageLayout = host_layout(im[k].imageLayout);
                    }
                    w[i].pImageInfo = im;
                }
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                w[i].pImageInfo = NULL;
                w[i].pBufferInfo = NULL;
                break;
            default:
                w[i].pImageInfo = NULL;
                w[i].pTexelBufferView = NULL;
                break;
        }
    }
    vkUpdateDescriptorSets(g_vk.dev, w ? nw : 0, w, cp ? nc : 0, cp);
    ret(c, 0);
}

static int blends_src1(VkBlendFactor f) { return f >= VK_BLEND_FACTOR_SRC1_COLOR && f <= VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA; }
/* A pipeline that blends with the second color takes its fragment module's twin (GvkDual). The host's compiles leave
   the game's core to its other threads (kthread_host_wait_begin): the game makes thousands of pipelines as it starts */
static void gvk_CreateGraphicsPipelines(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    u32 n = (u32)c->x[2];
    VkGraphicsPipelineCreateInfo* ci = gvk_in(&gvk_t_gpipe, c->x[3], n);
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (ci && c->x[5] && mem_user_mapped(c->x[5], 8ull * n)) {
        for (u32 i = 0; i < n; i++) { /* the tessellation state is read only with its stages */
            int tess = 0, src1 = 0;
            for (u32 s = 0; ci[i].pStages && s < ci[i].stageCount; s++)
                tess |= (ci[i].pStages[s].stage & VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT) != 0;
            if (!tess) ci[i].pTessellationState = NULL;
            const VkPipelineColorBlendStateCreateInfo* b = ci[i].pColorBlendState;
            for (u32 a = 0; b && b->pAttachments && a < b->attachmentCount; a++) {
                const VkPipelineColorBlendAttachmentState* at = &b->pAttachments[a];
                src1 |= at->blendEnable && (blends_src1(at->srcColorBlendFactor) ||
                                            blends_src1(at->dstColorBlendFactor) ||
                                            blends_src1(at->srcAlphaBlendFactor) ||
                                            blends_src1(at->dstAlphaBlendFactor));
            }
            VkPipelineShaderStageCreateInfo* st = (VkPipelineShaderStageCreateInfo*)ci[i].pStages;
            GvkDual d;
            for (u32 s = 0; src1 && st && s < ci[i].stageCount; s++)
                if (st[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT && tab_get(TAB(dual), U(st[s].module), &d))
                    st[s].module = H(VkShaderModule, d.dual);
            /* a viewport baked into the pipeline isn't scaled: which target it draws to is the render pass's */
            if (scale_factor() > 1 && ci[i].pViewportState && ci[i].pViewportState->pViewports) {
                int dynamic = 0;
                for (u32 d = 0; ci[i].pDynamicState && d < ci[i].pDynamicState->dynamicStateCount; d++)
                    dynamic |= ci[i].pDynamicState->pDynamicStates[d] == VK_DYNAMIC_STATE_VIEWPORT;
                if (!dynamic) LOG_LIMIT(4, "gvk: a pipeline's own viewport isn't scaled\n");
            }
        }
        kthread_host_wait_begin();
        r = vkCreateGraphicsPipelines(g_vk.dev, H(VkPipelineCache, c->x[1]), n, ci, NULL, gvk_ptr(c->x[5]));
        kthread_host_wait_end();
    }
    if (r != VK_SUCCESS) WARN_LIMIT(16, "gvk: vkCreateGraphicsPipelines failed (%d)\n", (int)r);
    ret_vk(c, r);
}
static void gvk_CreateComputePipelines(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    u32 n = (u32)c->x[2];
    const VkComputePipelineCreateInfo* ci = gvk_in(&gvk_t_cpipe, c->x[3], n);
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (ci && c->x[5] && mem_user_mapped(c->x[5], 8ull * n)) {
        kthread_host_wait_begin();
        r = vkCreateComputePipelines(g_vk.dev, H(VkPipelineCache, c->x[1]), n, ci, NULL, gvk_ptr(c->x[5]));
        kthread_host_wait_end();
    }
    if (r != VK_SUCCESS) WARN_LIMIT(16, "gvk: vkCreateComputePipelines failed (%d)\n", (int)r);
    ret_vk(c, r);
}

/* ------------------------------------------------------------ command buffers */
/* the game's pools are of its one family: the renderer's graphics family */
static void gvk_CreateCommandPool(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    VkCommandPoolCreateInfo* ci = gvk_in(&gvk_t_pool, c->x[1], 1);
    VkCommandPool h = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (ci) {
        ci->queueFamilyIndex = g_vk.gfx_family;
        r = vkCreateCommandPool(g_vk.dev, ci, NULL, &h);
    }
    put_handle(c->x[3], U(h));
    ret_vk(c, r);
}
static void gvk_ResetCommandPool(CPU* RESTRICT c, u64 entry) {
    ret_vk(c, vkResetCommandPool(g_vk.dev, H(VkCommandPool, c->x[1]), (VkCommandPoolResetFlags)c->x[2]));
}
static void gvk_AllocateCommandBuffers(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    const VkCommandBufferAllocateInfo* ai = gvk_in(&gvk_t_cballoc, c->x[1], 1);
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (ai && c->x[2] && mem_user_mapped(c->x[2], 8ull * ai->commandBufferCount))
        r = vkAllocateCommandBuffers(g_vk.dev, ai, gvk_ptr(c->x[2]));
    ret_vk(c, r);
}
static void pass_forget(u64 cb) {
    GvkPass p;
    tab_take(TAB(pass), &g_gvk_api.npass, cb, &p);
}
static void gvk_FreeCommandBuffers(CPU* RESTRICT c, u64 entry) {
    u32 n = (u32)c->x[2];
    GUEST(cbs, VkCommandBuffer, c->x[3], n);
    for (u32 i = 0; i < n; i++) pass_forget(U(cbs[i]));
    vkFreeCommandBuffers(g_vk.dev, H(VkCommandPool, c->x[1]), n, cbs);
    ret(c, 0);
}
static void gvk_BeginCommandBuffer(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    pass_forget(c->x[0]);
    const VkCommandBufferBeginInfo* bi = gvk_in(&gvk_t_begin, c->x[1], 1);
    ret_vk(c, bi ? vkBeginCommandBuffer(H(VkCommandBuffer, c->x[0]), bi) : VK_ERROR_INITIALIZATION_FAILED);
}
static void gvk_EndCommandBuffer(CPU* RESTRICT c, u64 entry) {
    ret_vk(c, vkEndCommandBuffer(H(VkCommandBuffer, c->x[0])));
}
static void gvk_ResetCommandBuffer(CPU* RESTRICT c, u64 entry) {
    pass_forget(c->x[0]);
    ret_vk(c, vkResetCommandBuffer(H(VkCommandBuffer, c->x[0]), (VkCommandBufferResetFlags)c->x[1]));
}

/* Every command before it done and its writes seen by every command after it */
static void full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                         .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                         .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, NULL,
                         0, NULL);
}
/* A transfer command or a barrier inside a render pass, which the console's driver runs where the game puts it and
   Vulkan allows only outside one (VUID-vkCmdCopyImage-renderpass): the pass ended before it, 1, and pass_back begins
   its twin after it, which loads what the first part stored. 0 outside a pass, and inside one of several subpasses,
   whose rest can't be begun again */
static int pass_out(VkCommandBuffer cb, GvkPass* p) {
    GvkRpass rp;
    if (!tab_get(TAB(pass), U(cb), p)) return 0;
    if (!tab_get(TAB(rpass), p->rp, &rp) || rp.subpasses != 1 || !rp.resume) {
        LOG_LIMIT(4, "gvk: a transfer command inside a render pass of %u subpasses runs inside it\n", rp.subpasses);
        return 0;
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

#define CB H(VkCommandBuffer, c->x[0])
static void gvk_CmdBindPipeline(CPU* RESTRICT c, u64 entry) {
    vkCmdBindPipeline(CB, (VkPipelineBindPoint)(u32)c->x[1], H(VkPipeline, c->x[2]));
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
    ret(c, 0);
}
static void gvk_CmdPushConstants(CPU* RESTRICT c, u64 entry) {
    GUEST(values, u8, c->x[5], (u32)c->x[4]);
    vkCmdPushConstants(CB, H(VkPipelineLayout, c->x[1]), (VkShaderStageFlags)c->x[2], (u32)c->x[3], (u32)c->x[4],
                       values);
    ret(c, 0);
}
/* the factor the commands of cb draw at now: its render pass's framebuffer's */
static u32 pass_scale(u64 cb) {
    GvkPass p;
    return scale_factor() > 1 && tab_get(TAB(pass), cb, &p) && p.scaled ? scale_factor() : 1;
}
static void set_viewports(VkCommandBuffer cb, u32 first, u32 n, const VkViewport* v, u32 f) {
    VkViewport* s = f > 1 ? gvk_scratch(sizeof *s * n) : NULL;
    for (u32 i = 0; s && i < n; i++) s[i] = viewport_at(v[i], f);
    vkCmdSetViewport(cb, first, n, s ? s : v);
}
static void set_scissors(VkCommandBuffer cb, u32 first, u32 n, const VkRect2D* r, u32 f) {
    VkRect2D* s = f > 1 ? gvk_scratch(sizeof *s * n) : NULL;
    for (u32 i = 0; s && i < n; i++) s[i] = rect_at(r[i], f);
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
static void dyn_again(VkCommandBuffer cb, u32 f) {
    GvkDyn d;
    if (!tab_get(TAB(dyn), U(cb), &d)) return;
    int changed = 0;
    if (d.has_vp && d.vp_at != f) {
        set_viewports(cb, 0, 1, &d.vp, f);
        d.vp_at = f;
        changed = 1;
    }
    if (d.has_sc && d.sc_at != f) {
        set_scissors(cb, 0, 1, &d.sc, f);
        d.sc_at = f;
        changed = 1;
    }
    if (changed) tab_put(TAB(dyn), &g_gvk_api.ndyn, &d);
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
    vkCmdDraw(CB, (u32)c->x[1], (u32)c->x[2], (u32)c->x[3], (u32)c->x[4]);
    ret(c, 0);
}
static void gvk_CmdDrawIndexed(CPU* RESTRICT c, u64 entry) {
    vkCmdDrawIndexed(CB, (u32)c->x[1], (u32)c->x[2], (u32)c->x[3], (i32)(u32)c->x[4], (u32)c->x[5]);
    ret(c, 0);
}
static void gvk_CmdDrawIndirect(CPU* RESTRICT c, u64 entry) {
    vkCmdDrawIndirect(CB, H(VkBuffer, c->x[1]), c->x[2], (u32)c->x[3], (u32)c->x[4]);
    ret(c, 0);
}
static void gvk_CmdDrawIndexedIndirect(CPU* RESTRICT c, u64 entry) {
    vkCmdDrawIndexedIndirect(CB, H(VkBuffer, c->x[1]), c->x[2], (u32)c->x[3], (u32)c->x[4]);
    ret(c, 0);
}
static void gvk_CmdDispatch(CPU* RESTRICT c, u64 entry) {
    vkCmdDispatch(CB, (u32)c->x[1], (u32)c->x[2], (u32)c->x[3]);
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
        if (scaled) bi->renderArea = rect_at(bi->renderArea, scale_factor());
        vkCmdBeginRenderPass(CB, bi, contents);
        GvkPass p = {c->x[0], U(bi->renderPass), U(bi->framebuffer), bi->renderArea, 0, contents, scaled};
        tab_put(TAB(pass), &g_gvk_api.npass, &p);
        if (scale_factor() > 1) dyn_again(CB, scaled ? scale_factor() : 1);
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
    vkCmdEndRenderPass(CB);
    pass_forget(c->x[0]);
    ret(c, 0);
}
static void gvk_CmdExecuteCommands(CPU* RESTRICT c, u64 entry) {
    GUEST(cbs, VkCommandBuffer, c->x[2], (u32)c->x[1]);
    vkCmdExecuteCommands(CB, (u32)c->x[1], cbs);
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
/* vkCmdPipelineBarrier(cb, src, dst, flags, nm, pm, nb, pb, ni, pi): the last two on the stack. Inside a render pass
   whose subpass depends on itself it is legal and stays there, as the game gave it; inside another it goes outside
   (pass_out), its stages with those its accesses need (stages_for). Its layouts are host_layout's, and a
   guest-mapped linear image, made undefined, leaves undefined */
static void gvk_CmdPipelineBarrier(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    u32 nm = (u32)c->x[4], nb = (u32)c->x[6], ni = (u32)stack_arg(c, 0);
    const VkMemoryBarrier* m = gvk_in(&gvk_t_mbar, c->x[5], nm);
    const VkBufferMemoryBarrier* b = gvk_in(&gvk_t_bbar, c->x[7], nb);
    VkImageMemoryBarrier* i = gvk_in(&gvk_t_ibar, stack_arg(c, 1), ni);
    for (u32 k = 0; i && k < ni; k++) {
        i[k].oldLayout = host_layout(i[k].oldLayout);
        i[k].newLayout = host_layout(i[k].newLayout);
        if (i[k].oldLayout == VK_IMAGE_LAYOUT_PREINITIALIZED && importable_has(U(i[k].image)))
            i[k].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    GvkPass p;
    GvkRpass rp;
    if (tab_get(TAB(pass), c->x[0], &p) && tab_get(TAB(rpass), p.rp, &rp) && rp.self)
        vkCmdPipelineBarrier(CB, (VkPipelineStageFlags)c->x[1], (VkPipelineStageFlags)c->x[2],
                             (VkDependencyFlags)c->x[3], m ? nm : 0, m, b ? nb : 0, b, i ? ni : 0, i);
    else {
        VkAccessFlags sa = 0, da = 0;
        for (u32 k = 0; m && k < nm; k++) sa |= m[k].srcAccessMask, da |= m[k].dstAccessMask;
        for (u32 k = 0; b && k < nb; k++) sa |= b[k].srcAccessMask, da |= b[k].dstAccessMask;
        for (u32 k = 0; i && k < ni; k++) sa |= i[k].srcAccessMask, da |= i[k].dstAccessMask;
        VkPipelineStageFlags src = (VkPipelineStageFlags)c->x[1], dst = (VkPipelineStageFlags)c->x[2];
        src |= stages_for(sa, src);
        dst |= stages_for(da, dst);
        OUTSIDE(CB, vkCmdPipelineBarrier(CB, src, dst, (VkDependencyFlags)c->x[3], m ? nm : 0, m, b ? nb : 0, b,
                                         i ? ni : 0, i));
    }
    ret(c, 0);
}
static void gvk_CmdCopyBuffer(CPU* RESTRICT c, u64 entry) {
    GUEST(r, VkBufferCopy, c->x[4], (u32)c->x[3]);
    OUTSIDE(CB, vkCmdCopyBuffer(CB, H(VkBuffer, c->x[1]), H(VkBuffer, c->x[2]), (u32)c->x[3], r));
    ret(c, 0);
}
/* A copy between images at one scale scales its regions. Between a scaled image and one at the game's size it is a
   blit, filtered where both are color */
static void gvk_CmdCopyImage(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    u32 n = (u32)c->x[5], fs = image_scale(c->x[1]), fd = image_scale(c->x[3]);
    const VkImageCopy* r = n && mem_user_mapped(c->x[6], sizeof *r * n) ? gvk_ptr(c->x[6]) : NULL;
    VkImage src = H(VkImage, c->x[1]), dst = H(VkImage, c->x[3]);
    VkImageLayout sl = arg_layout(c->x[2]), dl = arg_layout(c->x[4]);
    if (!r || (fs == 1 && fd == 1)) {
        OUTSIDE(CB, vkCmdCopyImage(CB, src, sl, dst, dl, r ? n : 0, r));
    } else if (fs == fd) {
        VkImageCopy* s = gvk_scratch(sizeof *s * n);
        for (u32 i = 0; i < n; i++) {
            s[i] = r[i];
            s[i].srcOffset = offset_at(r[i].srcOffset, fs);
            s[i].dstOffset = offset_at(r[i].dstOffset, fs);
            s[i].extent.width *= fs;
            s[i].extent.height *= fs;
        }
        OUTSIDE(CB, vkCmdCopyImage(CB, src, sl, dst, dl, n, s));
    } else {
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
        OUTSIDE(CB, vkCmdBlitImage(CB, src, sl, dst, dl, n, b, color ? VK_FILTER_LINEAR : VK_FILTER_NEAREST));
    }
    ret(c, 0);
}
static void gvk_CmdBlitImage(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    u32 n = (u32)c->x[5], fs = image_scale(c->x[1]), fd = image_scale(c->x[3]);
    const VkImageBlit* r = n && mem_user_mapped(c->x[6], sizeof *r * n) ? gvk_ptr(c->x[6]) : NULL;
    VkImageBlit* s = r && (fs > 1 || fd > 1) ? gvk_scratch(sizeof *s * n) : NULL;
    for (u32 i = 0; s && i < n; i++) {
        s[i] = r[i];
        for (u32 k = 0; k < 2; k++) {
            s[i].srcOffsets[k] = offset_at(r[i].srcOffsets[k], fs);
            s[i].dstOffsets[k] = offset_at(r[i].dstOffsets[k], fd);
        }
    }
    OUTSIDE(CB, vkCmdBlitImage(CB, H(VkImage, c->x[1]), arg_layout(c->x[2]), H(VkImage, c->x[3]), arg_layout(c->x[4]),
                               r ? n : 0, s ? s : r, (VkFilter)(u32)c->x[7]));
    ret(c, 0);
}
static void gvk_CmdResolveImage(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    u32 n = (u32)c->x[5], fs = image_scale(c->x[1]), fd = image_scale(c->x[3]);
    const VkImageResolve* r = n && mem_user_mapped(c->x[6], sizeof *r * n) ? gvk_ptr(c->x[6]) : NULL;
    VkImageResolve* s = r && fs == fd && fs > 1 ? gvk_scratch(sizeof *s * n) : NULL;
    if (r && fs != fd) LOG_LIMIT(4, "gvk: a resolve between images at two scales resolves the game's region alone\n");
    for (u32 i = 0; s && i < n; i++) {
        s[i] = r[i];
        s[i].srcOffset = offset_at(r[i].srcOffset, fs);
        s[i].dstOffset = offset_at(r[i].dstOffset, fs);
        s[i].extent.width *= fs;
        s[i].extent.height *= fs;
    }
    OUTSIDE(CB, vkCmdResolveImage(CB, H(VkImage, c->x[1]), arg_layout(c->x[2]), H(VkImage, c->x[3]),
                                  arg_layout(c->x[4]), r ? n : 0, s ? s : r));
    ret(c, 0);
}
/* A buffer holds the game's pixels: a copy of a scaled image to or from one moves its top-left part alone */
static void gvk_CmdCopyBufferToImage(CPU* RESTRICT c, u64 entry) {
    GUEST(r, VkBufferImageCopy, c->x[5], (u32)c->x[4]);
    if (image_scale(c->x[2]) > 1) LOG_LIMIT(4, "gvk: a copy from a buffer into a scaled image fills a part of it\n");
    OUTSIDE(CB, vkCmdCopyBufferToImage(CB, H(VkBuffer, c->x[1]), H(VkImage, c->x[2]), arg_layout(c->x[3]),
                                       (u32)c->x[4], r));
    ret(c, 0);
}
static void gvk_CmdCopyImageToBuffer(CPU* RESTRICT c, u64 entry) {
    GUEST(r, VkBufferImageCopy, c->x[5], (u32)c->x[4]);
    if (image_scale(c->x[1]) > 1) LOG_LIMIT(4, "gvk: a copy of a scaled image into a buffer reads a part of it\n");
    OUTSIDE(CB, vkCmdCopyImageToBuffer(CB, H(VkImage, c->x[1]), arg_layout(c->x[2]), H(VkBuffer, c->x[3]),
                                       (u32)c->x[4], r));
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
    OUTSIDE(CB, vkCmdClearColorImage(CB, H(VkImage, c->x[1]), arg_layout(c->x[2]), color, (u32)c->x[4], ranges));
    ret(c, 0);
}
static void gvk_CmdClearDepthStencilImage(CPU* RESTRICT c, u64 entry) {
    GUEST(value, VkClearDepthStencilValue, c->x[3], 1);
    GUEST(ranges, VkImageSubresourceRange, c->x[5], (u32)c->x[4]);
    OUTSIDE(CB, vkCmdClearDepthStencilImage(CB, H(VkImage, c->x[1]), arg_layout(c->x[2]), value, (u32)c->x[4],
                                            ranges));
    ret(c, 0);
}
static void gvk_CmdClearAttachments(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    u32 n = (u32)c->x[3], f = pass_scale(c->x[0]);
    const VkClearRect* r = n && mem_user_mapped(c->x[4], sizeof *r * n) ? gvk_ptr(c->x[4]) : NULL;
    VkClearRect* s = r && f > 1 ? gvk_scratch(sizeof *s * n) : NULL;
    for (u32 i = 0; s && i < n; i++) {
        s[i] = r[i];
        s[i].rect = rect_at(r[i].rect, f);
    }
    GUEST(atts, VkClearAttachment, c->x[2], (u32)c->x[1]);
    vkCmdClearAttachments(CB, (u32)c->x[1], atts, r ? n : 0, s ? s : r);
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
    if (!c->x[5] || !mem_user_mapped(c->x[5], size)) {
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
    const VkSubmitInfo* s = gvk_in(&gvk_t_submit, c->x[2], n);
    ret_vk(c, queue_submit(s ? n : 0, s, H(VkFence, c->x[3])));
}
static void gvk_QueueWaitIdle(CPU* RESTRICT c, u64 entry) { gvk_DeviceWaitIdle(c, entry); }

/* ------------------------------------------------------------ the window */
static void gvk_CreateViSurfaceNN(CPU* RESTRICT c, u64 entry) {
    put_handle(c->x[3], GVK_SURFACE);
    ret_vk(c, VK_SUCCESS);
}
static void gvk_GetPhysicalDeviceSurfaceSupportKHR(CPU* RESTRICT c, u64 entry) {
    if (c->x[3] && mem_user_mapped(c->x[3], 4)) gw32(c->x[3], VK_TRUE);
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
        .maxImageExtent = {1920, 1080},
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
/* its images go once the queue is idle: the renderer's copy of the last present may still read one */
static void swapchain_free(GvkSwapchain* s) {
    vk_lock();
    vk_rec_queue(g_vk.gfx);
    vk_queue_lock(g_vk.gfx);
    vkQueueWaitIdle(g_vk.gfx);
    vk_queue_unlock(g_vk.gfx);
    vk_unlock();
    for (u32 i = 0; i < s->n; i++) {
        GvkScaled e;
        if (s->img[i] && scale_factor() > 1) tab_take(TAB(scaled), &g_gvk_api.nscaled, U(s->img[i]), &e);
        vkDestroyImage(g_vk.dev, s->img[i], NULL);
        vkFreeMemory(g_vk.dev, s->mem[i], NULL);
    }
    *s = (GvkSwapchain){0};
}
/* a swapchain's new images put in GENERAL, the one layout the game's render passes take them in (host_layout), ahead
   of the game's work on the queue */
static VkResult images_general(const VkImage* img, u32 n) {
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                  .queueFamilyIndex = g_vk.gfx_family};
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkResult r = vkCreateCommandPool(g_vk.dev, &pi, NULL, &pool);
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1};
    if (r == VK_SUCCESS) r = vkAllocateCommandBuffers(g_vk.dev, &ai, &cb);
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (r == VK_SUCCESS) r = vkCreateFence(g_vk.dev, &fi, NULL, &fence);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (r == VK_SUCCESS) r = vkBeginCommandBuffer(cb, &bi);
    if (r == VK_SUCCESS) {
        VkImageMemoryBarrier b[GVK_SC_IMAGES];
        for (u32 i = 0; i < n; i++)
            b[i] = (VkImageMemoryBarrier){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                          .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                          .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                          .image = img[i],
                                          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                             NULL, n, b);
        r = vkEndCommandBuffer(cb);
    }
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    if (r == VK_SUCCESS) r = queue_submit(1, &si, fence);
    if (r == VK_SUCCESS) {
        kthread_host_wait_begin();
        r = vkWaitForFences(g_vk.dev, 1, &fence, VK_TRUE, UINT64_MAX);
        kthread_host_wait_end();
    }
    if (fence) vkDestroyFence(g_vk.dev, fence, NULL);
    if (pool) vkDestroyCommandPool(g_vk.dev, pool, NULL);
    return r;
}
static void gvk_CreateSwapchainKHR(CPU* RESTRICT c, u64 entry) {
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
    /* the game draws its frame into them: at the resolution scale, as its targets */
    u32 f = usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT ? scale_factor() : 1;
    pm_lock(&g_gvk_api.lock);
    u32 slot = 0;
    while (slot < GVK_SWAPCHAINS && g_gvk_api.sc[slot].n) slot++;
    if (slot == GVK_SWAPCHAINS) fatal("gvk: the game holds more than %d swapchains", GVK_SWAPCHAINS);
    GvkSwapchain* s = &g_gvk_api.sc[slot];
    *s = (GvkSwapchain){.n = want < 2               ? 2
                             : want > GVK_SC_IMAGES ? GVK_SC_IMAGES
                                                    : want,
                        .fmt = fmt,
                        .w = w * f,
                        .h = h * f};
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
        if (r == VK_SUCCESS && f > 1) scaled_put(U(s->img[i]), w, h);
    }
    if (r == VK_SUCCESS) r = images_general(s->img, s->n);
    if (r != VK_SUCCESS) {
        WARN("gvk: a swapchain of %u images of format %d, %ux%u, usage %#x, failed (%d)\n", s->n, (int)fmt, w, h, usage,
             (int)r);
        swapchain_free(s);
        ret_vk(c, r);
        return;
    }
    LOG("gvk: the game's swapchain: %u images of format %d, %ux%u, usage %#x, drawn at %ux%u\n", s->n, (int)fmt, w, h,
        usage, s->w, s->h);
    put_handle(c->x[3], slot + 1);
    ret_vk(c, VK_SUCCESS);
}
static void gvk_DestroySwapchainKHR(CPU* RESTRICT c, u64 entry) {
    GvkSwapchain* s = swapchain_of(c->x[1]);
    if (s) swapchain_free(s);
    ret(c, 0);
}
static void gvk_GetSwapchainImagesKHR(CPU* RESTRICT c, u64 entry) {
    GvkSwapchain* s = swapchain_of(c->x[1]);
    if (!s) {
        ret_vk(c, VK_ERROR_SURFACE_LOST_KHR);
        return;
    }
    u64 img[GVK_SC_IMAGES];
    for (u32 i = 0; i < s->n; i++) img[i] = U(s->img[i]);
    ret_vk(c, put_list(c->x[2], c->x[3], img, s->n, sizeof *img));
}
/* the next image in turn that the game doesn't hold, once the game's pace lets it have it (vi's dequeue waits the
   same): the semaphore and the fence signaled after the work before on the queue, the renderer's copy of the image's
   last present among it. With every image held, none comes: VK_NOT_READY, or VK_TIMEOUT for a wait */
static void gvk_AcquireNextImageKHR(CPU* RESTRICT c, u64 entry) {
    GvkSwapchain* s = swapchain_of(c->x[1]);
    if (!s) {
        ret_vk(c, VK_ERROR_SURFACE_LOST_KHR);
        return;
    }
    u32 idx = s->next, tries = 0;
    while (tries < s->n && (s->acquired >> idx & 1)) {
        idx = (idx + 1) % s->n;
        tries++;
    }
    if (tries == s->n) {
        ret_vk(c, c->x[2] ? VK_TIMEOUT : VK_NOT_READY);
        return;
    }
    kthread_host_wait_begin();
    present_pace(1, -1);
    kthread_host_wait_end();
    s->acquired |= 1u << idx;
    s->next = (idx + 1) % s->n;
    VkSemaphore sem = H(VkSemaphore, c->x[3]);
    VkFence fence = H(VkFence, c->x[4]);
    VkResult r = VK_SUCCESS;
    if (sem || fence) {
        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .signalSemaphoreCount = sem ? 1 : 0, .pSignalSemaphores = &sem};
        r = queue_submit(1, &si, fence);
    }
    if (c->x[5] && mem_user_mapped(c->x[5], 4)) gw32(c->x[5], idx);
    ret_vk(c, r);
}
/* the game's semaphores waited for on the queue, then each image shown (renderer.h present_image) */
static void gvk_QueuePresentKHR(CPU* RESTRICT c, u64 entry) {
    gvk_scratch_reset();
    u64 pi = c->x[1];
    if (!pi || !mem_user_mapped(pi, sizeof(VkPresentInfoKHR))) {
        ret_vk(c, VK_ERROR_INITIALIZATION_FAILED);
        return;
    }
    u32 nw = gr32(pi + offsetof(VkPresentInfoKHR, waitSemaphoreCount));
    u32 nsc = gr32(pi + offsetof(VkPresentInfoKHR, swapchainCount));
    u64 waits = gr64(pi + offsetof(VkPresentInfoKHR, pWaitSemaphores));
    u64 chains = gr64(pi + offsetof(VkPresentInfoKHR, pSwapchains));
    u64 indices = gr64(pi + offsetof(VkPresentInfoKHR, pImageIndices));
    u64 results = gr64(pi + offsetof(VkPresentInfoKHR, pResults));
    VkResult r = VK_SUCCESS;
    if (nw && mem_user_mapped(waits, 8ull * nw)) {
        VkPipelineStageFlags* stages = gvk_scratch(sizeof *stages * nw);
        for (u32 i = 0; i < nw; i++) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                           .waitSemaphoreCount = nw,
                           .pWaitSemaphores = gvk_ptr(waits),
                           .pWaitDstStageMask = stages};
        r = queue_submit(1, &si, VK_NULL_HANDLE);
    }
    for (u32 i = 0; i < nsc && mem_user_mapped(chains + 8ull * i, 8) && mem_user_mapped(indices + 4ull * i, 4); i++) {
        GvkSwapchain* s = swapchain_of(gr64(chains + 8ull * i));
        u32 idx = gr32(indices + 4ull * i);
        VkResult one = s && idx < s->n ? VK_SUCCESS : VK_ERROR_SURFACE_LOST_KHR;
        if (one == VK_SUCCESS) {
            if (!g_gvk_api.presents++) LOG("gvk: the game's first present\n");
            present_image(U(s->img[idx]), (u32)s->fmt, s->w, s->h);
            s->acquired &= ~(1u << idx);
        }
        if (results && mem_user_mapped(results + 4ull * i, 4)) gw32(results + 4ull * i, (u32)one);
        if (one != VK_SUCCESS) r = one;
    }
    ret_vk(c, r);
}

/* ------------------------------------------------------------ the loader */
/* the game's own export for name, where gvk implements it: a function it doesn't is said to be none */
static void proc_addr(CPU* RESTRICT c, u64 name_at) {
    char name[128];
    u64 a = 0;
    if (mem_user_string(name, name_at, sizeof name) > 0) {
        if (gvk_api_find(name)) a = gvk_export(name);
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
    E(CreateDevice),
    AS(DestroyDevice, DestroyNothing),
    E(GetDeviceQueue),
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
    E(CmdDispatchIndirect),
    E(CmdBeginRenderPass),
    E(CmdNextSubpass),
    E(CmdEndRenderPass),
    E(CmdExecuteCommands),
    E(CmdPipelineBarrier),
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
    AS(DestroySurfaceKHR, DestroyNothing),
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
