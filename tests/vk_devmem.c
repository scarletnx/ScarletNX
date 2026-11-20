/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The renderer's device memory (gpu/vk/devmem.c) over a driver that the test plays, which checks every call as the
   Vulkan specification reads: a resource is bound at its alignment, inside its memory, in a memory type it allows, on
   no byte another resource has, and on no page of the device's granularity that a resource of the other kind has; a
   resource the driver wants alone gets memory made for it, of its size; memory is mapped once; a flush names whole
   atoms inside the memory. Over that: the GPU's own heap stays under the driver's budget less the margin and its
   resources never land in system memory, a large request takes a block's free run, a resource the driver would
   rather have alone goes into a block when the driver has no memory for it alone, and requests from several threads
   beside the allocator's own thread keep all of it. Where the driver moves memory
   (VK_EXT_pageable_device_local_memory), each memory has the priority of the highest class it holds, and the driver
   is told only when that changes. Requests of the low class take blocks of their own, and those of the others only
   when no new block can be made. No Vulkan loader, device or GPU is used. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/platform/thread.h"
#include "common/platform/time.h"
/* the allocator's thread starts only while g_threads is set: the checks before that count the driver's calls */
static int g_threads;
static int thread_or_none(PThreadFn fn, void* arg, size_t stack) {
    return g_threads ? plat_thread(fn, arg, stack) : -1;
}
#define plat_thread thread_or_none
#include "nx/gpu/vk/devmem.c"
#undef plat_thread

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)
#define KIB 1024ull
#define MIB (1024 * KIB)
#define GIB (1024 * MIB)

VkState g_vk;
void vk_descriptor_heaps_forget_buffer(VkBuffer buffer) { (void)buffer; } /* no descriptor heaps here */
PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
PFN_vkGetPhysicalDeviceMemoryProperties2 vkGetPhysicalDeviceMemoryProperties2;
PFN_vkMapMemory vkMapMemory;
PFN_vkCreateBuffer vkCreateBuffer;
PFN_vkGetBufferMemoryRequirements2 vkGetBufferMemoryRequirements2;
PFN_vkBindBufferMemory vkBindBufferMemory;
PFN_vkDestroyBuffer vkDestroyBuffer;
PFN_vkCreateImage vkCreateImage;
PFN_vkGetImageMemoryRequirements2 vkGetImageMemoryRequirements2;
PFN_vkBindImageMemory vkBindImageMemory;
PFN_vkDestroyImage vkDestroyImage;
PFN_vkFlushMappedMemoryRanges vkFlushMappedMemoryRanges;
PFN_vkInvalidateMappedMemoryRanges vkInvalidateMappedMemoryRanges;
PFN_vkSetDeviceMemoryPriorityEXT vkSetDeviceMemoryPriorityEXT;

void logf_(int lvl, const char* fmt, ...) { (void)lvl, (void)fmt; }
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(2);
}

/* ------------------------------------------------ the driver */
#define MEMS 4096
#define RES  8192
typedef struct {
    u64 size;
    u32 type, res; /* res: the resource it was made for, as its index plus one */
    float prio;    /* its priority: Vulkan's default 0.5 until it is set */
    u8 live, mapped;
} DrvMem;
typedef struct {
    u64 size, align, at;
    u32 bits, mem; /* mem: its memory's index plus one, once bound */
    u8 live, image, must, rather;
} DrvRes;
/* what a resource's memory must be like, in the chain of its create info */
typedef struct {
    VkStructureType sType;
    const void* pNext;
    u64 size, align;
    u32 bits;
    u8 must, rather;
} DrvAsk;
#define DRV_ASK ((VkStructureType)0x7ffe0001)
static struct {
    PMutex lk; /* the driver's calls come from every thread */
    PCond cv;
    VkPhysicalDeviceMemoryProperties mp;
    u64 budget[VK_MAX_MEMORY_HEAPS], room[VK_MAX_MEMORY_HEAPS], used[VK_MAX_MEMORY_HEAPS], peak[VK_MAX_MEMORY_HEAPS];
    DrvMem mem[MEMS];
    DrvRes res[RES];
    u32 mems, allocations, no_alone; /* no_alone: it has no memory for a resource alone */
    u32 hold_free, free_waits;       /* a free waits while hold_free is set, and says so */
    u64 gran, atom;
    VkMappedMemoryRange range; /* the last one flushed or invalidated */
    u32 ranges;
    u32 priorities; /* calls that set a priority */
} g_drv = {.lk = PMUTEX_INIT};

static u32 drv_heap(u32 type) { return g_drv.mp.memoryTypes[type].heapIndex; }
VkResult VKAPI_CALL vk_ledger_allocate(VkDevice dev, const VkMemoryAllocateInfo* ai, const VkAllocationCallbacks* cb,
                                       VkDeviceMemory* mem) {
    (void)dev, (void)cb;
    const VkMemoryDedicatedAllocateInfo* alone = NULL;
    const VkMemoryPriorityAllocateInfoEXT* prio = NULL;
    for (const VkBaseInStructure* s = ai->pNext; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) alone = (const void*)s;
        if (s->sType == VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT) prio = (const void*)s;
    }
    pm_lock(&g_drv.lk);
    CHECK(ai->allocationSize && ai->memoryTypeIndex < g_drv.mp.memoryTypeCount);
    CHECK(!prio || (g_vk.has_pageable && prio->priority >= 0.0f && prio->priority <= 1.0f));
    u32 heap = drv_heap(ai->memoryTypeIndex), res = 0, i = 0;
    if (alone && (alone->buffer || alone->image)) {
        CHECK(!alone->buffer || !alone->image);
        res = (u32)(alone->buffer ? (uintptr_t)alone->buffer : (uintptr_t)alone->image);
        const DrvRes* r = &g_drv.res[res - 1];
        CHECK(res <= RES && r->live && !r->mem && r->image == (alone->image != VK_NULL_HANDLE));
        CHECK(ai->allocationSize == r->size && (r->bits >> ai->memoryTypeIndex & 1));
    }
    VkResult out = VK_SUCCESS;
    if ((res && g_drv.no_alone) || g_drv.used[heap] + ai->allocationSize > g_drv.room[heap])
        out = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    else if (g_drv.mems >= g_vk.props.limits.maxMemoryAllocationCount)
        out = VK_ERROR_TOO_MANY_OBJECTS;
    if (out == VK_SUCCESS) {
        while (i < MEMS && g_drv.mem[i].live) i++;
        CHECK(i < MEMS);
        g_drv.mem[i] = (DrvMem){.size = ai->allocationSize,
                                .type = ai->memoryTypeIndex,
                                .res = res,
                                .prio = prio ? prio->priority : 0.5f,
                                .live = 1};
        g_drv.used[heap] += ai->allocationSize;
        if (g_drv.used[heap] > g_drv.peak[heap]) g_drv.peak[heap] = g_drv.used[heap];
        g_drv.mems++;
        g_drv.allocations++;
        *mem = (VkDeviceMemory)(uintptr_t)(i + 1);
    }
    pm_unlock(&g_drv.lk);
    return out;
}
void VKAPI_CALL vk_ledger_free(VkDevice dev, VkDeviceMemory mem, const VkAllocationCallbacks* cb) {
    (void)dev, (void)cb;
    u32 m = (u32)(uintptr_t)mem;
    pm_lock(&g_drv.lk);
    while (g_drv.hold_free) {
        g_drv.free_waits = 1;
        pc_wait(&g_drv.cv, &g_drv.lk, -1);
    }
    CHECK(m && m <= MEMS && g_drv.mem[m - 1].live);
    for (u32 i = 0; i < RES; i++) CHECK(!g_drv.res[i].live || g_drv.res[i].mem != m); /* nothing is bound to it */
    g_drv.used[drv_heap(g_drv.mem[m - 1].type)] -= g_drv.mem[m - 1].size;
    g_drv.mem[m - 1].live = 0;
    g_drv.mems--;
    pm_unlock(&g_drv.lk);
}

/* a resource as its create info asks, its index plus one */
static u32 res_new(const void* chain, int image) {
    const DrvAsk* ask = NULL;
    for (const VkBaseInStructure* s = chain; s; s = s->pNext)
        if (s->sType == DRV_ASK) ask = (const void*)s;
    CHECK(ask && ask->size && ask->align);
    pm_lock(&g_drv.lk);
    u32 i = 0;
    while (i < RES && g_drv.res[i].live) i++;
    CHECK(i < RES);
    g_drv.res[i] = (DrvRes){.size = ask->size,
                            .align = ask->align,
                            .bits = ask->bits,
                            .live = 1,
                            .image = (u8)image,
                            .must = ask->must,
                            .rather = ask->rather};
    pm_unlock(&g_drv.lk);
    return i + 1;
}
/* resource res bound at `at` of memory mem: what the specification asks of the binding */
static VkResult res_bind(u32 res, u32 mem, u64 at) {
    pm_lock(&g_drv.lk);
    CHECK(res && res <= RES && mem && mem <= MEMS);
    DrvRes* r = &g_drv.res[res - 1];
    const DrvMem* m = &g_drv.mem[mem - 1];
    CHECK(r->live && !r->mem && m->live);
    CHECK(at % r->align == 0 && at + r->size <= m->size && (r->bits >> m->type & 1));
    CHECK(!r->must || m->res == res);         /* the memory was made for it */
    CHECK(!m->res || (m->res == res && !at)); /* and for no other resource */
    for (u32 i = 0; i < RES; i++) {
        const DrvRes* o = &g_drv.res[i];
        if (!o->live || o->mem != mem) continue;
        CHECK(at + r->size <= o->at || o->at + o->size <= at);
        if (o->image == r->image) continue;
        /* a buffer and an image: the last page of the lower one is before the first page of the other */
        u64 low_end = at < o->at ? at + r->size : o->at + o->size, high = at < o->at ? o->at : at;
        CHECK((low_end - 1) / g_drv.gran < high / g_drv.gran);
    }
    r->mem = mem;
    r->at = at;
    pm_unlock(&g_drv.lk);
    return VK_SUCCESS;
}
static void res_end(u32 res) {
    if (!res) return;
    pm_lock(&g_drv.lk);
    CHECK(res <= RES && g_drv.res[res - 1].live);
    g_drv.res[res - 1].live = 0;
    pm_unlock(&g_drv.lk);
}
static void res_requirements(u32 res, VkMemoryRequirements2* req) {
    pm_lock(&g_drv.lk);
    const DrvRes* r = &g_drv.res[res - 1];
    CHECK(res && res <= RES && r->live);
    req->memoryRequirements = (VkMemoryRequirements){r->size, r->align, r->bits};
    for (VkBaseOutStructure* s = req->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
            ((VkMemoryDedicatedRequirements*)s)->requiresDedicatedAllocation = r->must;
            ((VkMemoryDedicatedRequirements*)s)->prefersDedicatedAllocation = r->must || r->rather;
        }
    pm_unlock(&g_drv.lk);
}

static VkResult VKAPI_CALL create_buffer(VkDevice dev, const VkBufferCreateInfo* ci, const VkAllocationCallbacks* cb,
                                         VkBuffer* out) {
    (void)dev, (void)cb;
    *out = (VkBuffer)(uintptr_t)res_new(ci->pNext, 0);
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL create_image(VkDevice dev, const VkImageCreateInfo* ci, const VkAllocationCallbacks* cb,
                                        VkImage* out) {
    (void)dev, (void)cb;
    *out = (VkImage)(uintptr_t)res_new(ci->pNext, 1);
    return VK_SUCCESS;
}
static void VKAPI_CALL buffer_requirements(VkDevice dev, const VkBufferMemoryRequirementsInfo2* of,
                                           VkMemoryRequirements2* req) {
    (void)dev;
    res_requirements((u32)(uintptr_t)of->buffer, req);
}
static void VKAPI_CALL image_requirements(VkDevice dev, const VkImageMemoryRequirementsInfo2* of,
                                          VkMemoryRequirements2* req) {
    (void)dev;
    res_requirements((u32)(uintptr_t)of->image, req);
}
static VkResult VKAPI_CALL bind_buffer(VkDevice dev, VkBuffer b, VkDeviceMemory m, VkDeviceSize at) {
    (void)dev;
    return res_bind((u32)(uintptr_t)b, (u32)(uintptr_t)m, at);
}
static VkResult VKAPI_CALL bind_image(VkDevice dev, VkImage i, VkDeviceMemory m, VkDeviceSize at) {
    (void)dev;
    return res_bind((u32)(uintptr_t)i, (u32)(uintptr_t)m, at);
}
static void VKAPI_CALL destroy_buffer(VkDevice dev, VkBuffer b, const VkAllocationCallbacks* cb) {
    (void)dev, (void)cb;
    res_end((u32)(uintptr_t)b);
}
static void VKAPI_CALL destroy_image(VkDevice dev, VkImage i, const VkAllocationCallbacks* cb) {
    (void)dev, (void)cb;
    res_end((u32)(uintptr_t)i);
}
/* memory m's address for the CPU: nothing reads it */
static u8* map_of(u32 m) { return (u8*)((uintptr_t)m << 32); }
static VkResult VKAPI_CALL map_memory(VkDevice dev, VkDeviceMemory mem, VkDeviceSize at, VkDeviceSize size,
                                      VkMemoryMapFlags flags, void** out) {
    (void)dev, (void)flags;
    u32 m = (u32)(uintptr_t)mem;
    pm_lock(&g_drv.lk);
    CHECK(m && m <= MEMS && g_drv.mem[m - 1].live && !g_drv.mem[m - 1].mapped && !at && size == VK_WHOLE_SIZE);
    CHECK(g_drv.mp.memoryTypes[g_drv.mem[m - 1].type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    g_drv.mem[m - 1].mapped = 1;
    pm_unlock(&g_drv.lk);
    *out = map_of(m);
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL mapped_ranges(VkDevice dev, uint32_t n, const VkMappedMemoryRange* r) {
    (void)dev;
    u32 m = (u32)(uintptr_t)r->memory;
    pm_lock(&g_drv.lk);
    CHECK(n == 1 && m && m <= MEMS && g_drv.mem[m - 1].live && g_drv.mem[m - 1].mapped);
    /* coherent memory needs none, and the allocator says it makes none for it */
    CHECK(!(g_drv.mp.memoryTypes[g_drv.mem[m - 1].type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
    CHECK(r->size && r->offset % g_drv.atom == 0 && r->offset + r->size <= g_drv.mem[m - 1].size);
    CHECK(r->size % g_drv.atom == 0 || r->offset + r->size == g_drv.mem[m - 1].size);
    g_drv.range = *r;
    g_drv.ranges++;
    pm_unlock(&g_drv.lk);
    return VK_SUCCESS;
}
/* only a device with the extension's feature takes a priority, of [0, 1], and the allocator sets only a new one */
static void VKAPI_CALL set_priority(VkDevice dev, VkDeviceMemory mem, float prio) {
    (void)dev;
    u32 m = (u32)(uintptr_t)mem;
    pm_lock(&g_drv.lk);
    CHECK(g_vk.has_pageable && m && m <= MEMS && g_drv.mem[m - 1].live);
    CHECK(prio >= 0.0f && prio <= 1.0f && prio != g_drv.mem[m - 1].prio);
    g_drv.mem[m - 1].prio = prio;
    g_drv.priorities++;
    pm_unlock(&g_drv.lk);
}
/* the priorities asked so far, told the driver: the thread ahead takes them from its queue (devmem.c prio_set) */
static void prio_settle(void) {
    pm_lock(&g_devmem.lk);
    while (g_devmem.ahead.on && (g_devmem.ahead.nprio || g_devmem.ahead.prio_slot))
        pc_wait(&g_devmem.ahead.done, &g_devmem.lk, 1000000);
    pm_unlock(&g_devmem.lk);
}
/* ledger.c's timed priority: the driver's, here the model's */
void vk_ledger_priority(VkDevice dev, VkDeviceMemory mem, float priority) { vkSetDeviceMemoryPriorityEXT(dev, mem, priority); }
static void VKAPI_CALL memory_properties(VkPhysicalDevice phys, VkPhysicalDeviceMemoryProperties* out) {
    (void)phys;
    *out = g_drv.mp;
}
static void VKAPI_CALL memory_properties2(VkPhysicalDevice phys, VkPhysicalDeviceMemoryProperties2* out) {
    (void)phys;
    pm_lock(&g_drv.lk);
    out->memoryProperties = g_drv.mp;
    for (VkBaseOutStructure* s = out->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT)
            for (u32 h = 0; h < g_drv.mp.memoryHeapCount; h++) {
                ((VkPhysicalDeviceMemoryBudgetPropertiesEXT*)s)->heapBudget[h] = g_drv.budget[h];
                ((VkPhysicalDeviceMemoryBudgetPropertiesEXT*)s)->heapUsage[h] = g_drv.used[h];
            }
    pm_unlock(&g_drv.lk);
}

/* ------------------------------------------------ the devices */
#define DL  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
#define HV  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
#define COH VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
#define CAC VK_MEMORY_PROPERTY_HOST_CACHED_BIT
enum { T_GPU, T_HOST, T_CACHED, T_WINDOW }; /* the discrete GPU's memory types */
static u8 g_pristine[sizeof g_devmem];      /* the allocator before its first device */

/* a device, and the allocator started over on it. uma: one heap that the CPU maps, with coherence in its last memory
   type alone; else a discrete
   GPU with its own heap, system memory and a small window of its own memory that the CPU maps */
static void device(int uma, u64 gran, u64 atom, u64 budget) {
    static int first = 1;
    if (first) memcpy(g_pristine, &g_devmem, sizeof g_devmem);
    first = 0;
    /* not while the allocator's thread runs: the copy below goes over the lock and the condition it waits on, and
       vk_devmem_init starts a second thread on the same state */
    CHECK(!g_devmem.ahead.on);
    for (u32 i = 0; i < g_devmem.nblock; i++) space_destroy(&g_devmem.block[i].free);
    free(g_devmem.block);
    free(g_devmem.rec);
    memcpy(&g_devmem, g_pristine, sizeof g_devmem);
    memset(&g_drv.mp, 0, sizeof g_drv - offsetof(__typeof__(g_drv), mp));
    g_vk = (VkState){0};
    g_vk.alloc_cap = 2 * GIB;
    g_vk.mem_model = uma ? VK_MEM_UMA : VK_MEM_SMALL_BAR;
    g_vk.has_budget = budget != 0;
    g_vk.props.limits.bufferImageGranularity = g_drv.gran = gran;
    g_vk.props.limits.nonCoherentAtomSize = g_drv.atom = atom;
    g_vk.props.limits.maxMemoryAllocationCount = 4096;
    VkPhysicalDeviceMemoryProperties* mp = &g_drv.mp;
    if (uma) {
        mp->memoryHeapCount = 1;
        mp->memoryHeaps[0] = (VkMemoryHeap){8 * GIB, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
        mp->memoryTypeCount = 4;
        mp->memoryTypes[0] = (VkMemoryType){DL, 0};
        mp->memoryTypes[1] = (VkMemoryType){DL | HV, 0};
        mp->memoryTypes[2] = (VkMemoryType){DL | HV | CAC, 0};
        mp->memoryTypes[3] = (VkMemoryType){DL | HV | COH, 0};
    } else {
        mp->memoryHeapCount = 3;
        mp->memoryHeaps[0] = (VkMemoryHeap){8 * GIB, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
        mp->memoryHeaps[1] = (VkMemoryHeap){16 * GIB, 0};
        mp->memoryHeaps[2] = (VkMemoryHeap){256 * MIB, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
        mp->memoryTypeCount = 4;
        mp->memoryTypes[T_GPU] = (VkMemoryType){DL, 0};
        mp->memoryTypes[T_HOST] = (VkMemoryType){HV | COH, 1};
        mp->memoryTypes[T_CACHED] = (VkMemoryType){HV | COH | CAC, 1};
        mp->memoryTypes[T_WINDOW] = (VkMemoryType){DL | HV | COH, 2};
    }
    for (u32 h = 0; h < mp->memoryHeapCount; h++) {
        g_drv.room[h] = mp->memoryHeaps[h].size;
        g_drv.budget[h] = h ? mp->memoryHeaps[h].size : budget;
    }
    CHECK(vk_devmem_init(1));
}

/* ------------------------------------------------ requests */
typedef struct {
    VkBuffer buf;
    VkImage img;
    DevMem mem;
    DevMemInfo info;
} Held;
/* a buffer (image 0) or an image of size bytes whose memory must be as ask says, where want says */
static VkResult make(int image, u64 size, u64 align, u32 bits, int alone, DevMemWant want, Held* h) {
    DrvAsk ask = {
        .sType = DRV_ASK, .size = size, .align = align, .bits = bits, .must = alone == 2, .rather = alone == 1};
    *h = (Held){0};
    VkResult res;
    if (image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &ask};
        res = vk_devmem_image(&ci, &want, &h->img, &h->mem);
        if (res == VK_SUCCESS) vk_devmem_info(h->mem, &h->info);
    } else {
        VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &ask, .size = size};
        res = vk_devmem_buffer(&ci, &want, &h->buf, &h->mem, &h->info);
    }
    if (res != VK_SUCCESS) {
        CHECK(!h->buf && !h->img && !h->mem);
        return res;
    }
    u32 m = (u32)(uintptr_t)h->info.memory;
    VkMemoryPropertyFlags f = g_drv.mp.memoryTypes[h->info.type].propertyFlags;
    CHECK(h->mem && h->info.size == size && h->info.type == g_drv.mem[m - 1].type && (bits >> h->info.type & 1));
    CHECK(want.at == DM_DEVICE || (f & HV));
    CHECK(want.at != DM_DEVICE || g_vk.mem_model == VK_MEM_UMA || (f & DL)); /* never system memory */
    CHECK(h->info.ptr == (want.mapped && (f & HV) ? map_of(m) + h->info.offset : NULL));
    return res;
}
static void unmake(Held* h) {
    if (h->img)
        vk_devmem_image_destroy(h->img, h->mem);
    else
        vk_devmem_buffer_destroy(h->buf, h->mem);
    *h = (Held){0};
}
/* whether the memory of h was made for it alone */
static int alone_of(const Held* h) {
    u32 m = (u32)(uintptr_t)h->info.memory;
    return g_drv.mem[m - 1].res != 0 || g_drv.mem[m - 1].size == h->info.size;
}

static u32 g_seed = 0x9e3779b9u;
static u32 rnd_of(u32* seed) {
    *seed ^= *seed << 13;
    *seed ^= *seed >> 17;
    *seed ^= *seed << 5;
    return *seed;
}
static u32 rnd(void) { return rnd_of(&g_seed); }

/* a request of a random kind, size and alignment */
static VkResult make_random(u32* seed, Held* h) {
    static const u32 at[] = {DM_DEVICE, DM_DEVICE, DM_DEVICE, DM_DEVICE, DM_HOST, DM_CACHED, DM_WINDOW};
    u32 kind = rnd_of(seed) % 10, where = kind < 3 ? DM_DEVICE : at[rnd_of(seed) % 7], pick = rnd_of(seed) % 64;
    u64 size = 1 + rnd_of(seed) % (256 * KIB);
    if (pick < 8) size = 1 + rnd_of(seed) % (4 * MIB);
    if (pick == 8) size = 16 * MIB + rnd_of(seed) % (40 * MIB); /* more than a quarter of a block */
    if (pick == 9) size = 64 * MIB + rnd_of(seed) % (8 * MIB);  /* more than a block */
    u32 alone = rnd_of(seed) % 32;
    DevMemWant want = {.at = where,
                       .mapped = where != DM_DEVICE,
                       .own = rnd_of(seed) % 64 == 0,
                       .low = where == DM_DEVICE && rnd_of(seed) % 2};
    return make(kind < 3, size, 1ull << rnd_of(seed) % 17, ~0u, alone == 0 ? 2 : alone == 1 ? 1 : 0, want, h);
}

/* the allocator's counts are the driver's, with no thread of its own at work */
static void check_counts(u32 held) {
    DevMemBudget b[VK_MAX_MEMORY_HEAPS];
    u32 allocations = 0, blocks = 0;
    vk_devmem_budgets(b);
    for (u32 h = 0; h < g_drv.mp.memoryHeapCount; h++) {
        CHECK(b[h].block_bytes == g_drv.used[h] && b[h].allocated_bytes <= b[h].block_bytes);
        allocations += b[h].allocations;
        blocks += b[h].blocks;
    }
    CHECK(allocations == held && blocks == g_drv.mems);
}

/* each block's classes are the units its allocations hold, and where the driver moves memory it has the priority of
   the class of most of the units in a block of the low class, of the highest class in another. An empty block keeps
   the priority it had */
static void check_priorities(void) {
    prio_settle();
    u32 holds[MEMS][DM_PRIO_N], count[MEMS];
    memset(holds, 0, sizeof holds);
    memset(count, 0, sizeof count);
    for (u32 i = 0; i < g_devmem.nrec; i++) {
        const Rec* r = &g_devmem.rec[i];
        if (!r->block) continue;
        CHECK(r->prio < DM_PRIO_N && r->block <= MEMS);
        holds[r->block - 1][r->prio] += r->units ? r->units : 1;
        count[r->block - 1]++;
    }
    for (u32 k = 0; k < g_devmem.nblock; k++) {
        const Block* b = &g_devmem.block[k];
        if (!b->mem) continue;
        CHECK(count[k] == b->allocs);
        /* blocks of the low class take requests only where the driver moves memory (check_priority turns that off
           with an empty one left, which the runtime never does) */
        CHECK(!b->low || g_vk.has_pageable || !b->allocs);
        u32 top = DM_PRIO_N, most = 0;
        for (u32 c = 0; c < DM_PRIO_N; c++) {
            CHECK(b->holds[c] == holds[k][c]);
            if (!holds[k][c]) continue;
            if (!b->low)
                top = c;
            else if (holds[k][c] >= most) { /* a tie goes to the higher class */
                top = c;
                most = holds[k][c];
            }
        }
        if (g_vk.has_pageable) {
            CHECK(top == DM_PRIO_N || b->prio == top);
            prio_settle();
            CHECK(g_drv.mem[(u32)(uintptr_t)b->mem - 1].prio == k_prio[b->prio]);
        }
    }
}

static void check_random(int uma, u64 gran, u64 atom, u32 rounds) {
    enum { CAP = 1024 };
    static Held held[CAP];
    u32 n = 0;
    device(uma, gran, atom, 0);
    g_vk.has_pageable = !uma; /* and the bookkeeping alone where the driver does not move memory */
    for (u32 r = 0; r < rounds; r++) {
        if (n < CAP && (!n || rnd() % 5 < 3)) {
            if (make_random(&g_seed, &held[n]) == VK_SUCCESS) n++;
        } else if (rnd() % 3 == 0) { /* an owner gives its class, or a texture's changes */
            vk_devmem_priority(held[rnd() % n].mem, rnd() % DM_PRIO_N);
        } else {
            u32 k = rnd() % n;
            unmake(&held[k]);
            held[k] = held[--n];
        }
        if (r % 64 == 0) check_counts(n);
        if (r % 64 == 0) check_priorities();
        /* a block is a power of two of 64 MiB at most, made for no resource */
        if (r % 64 == 0)
            for (u32 k = 0; k < g_devmem.nblock; k++) {
                const Block* b = &g_devmem.block[k];
                if (!b->mem || b->own) continue;
                const DrvMem* m = &g_drv.mem[(u32)(uintptr_t)b->mem - 1];
                CHECK(m->live && m->size == b->size && m->size <= 64 * MIB && !(m->size & (m->size - 1)) && !m->res);
                CHECK(!b->ptr == !m->mapped);
            }
    }
    while (n) unmake(&held[--n]);
    check_counts(0);
    check_priorities();
    /* what stays is one empty block at most for a memory type's buffers, and for its images where they are apart: the
       two classes, and what moves and what does not, share it */
    u32 kinds[VK_MAX_MEMORY_TYPES][DM_KINDS] = {{0}};
    for (u32 k = 0; k < g_devmem.nblock; k++) {
        const Block* b = &g_devmem.block[k];
        if (b->mem && b->own) /* memory of its own the driver only preferred, kept for reuse (own_keep) */
            CHECK(!b->allocs && b->reuse && b->idle);
        else if (b->mem)
            CHECK(!b->allocs && !b->free.used && ++kinds[b->type][b->image & DM_KIND_IMAGE] == 1);
    }
}

/* a resource the driver wants alone, would rather have alone, and one whose owner asks for memory of its own */
static void check_alone(void) {
    Held must, rather, own, block, h;
    DevMemWant gpu = {.at = DM_DEVICE}, mine = {.at = DM_DEVICE, .own = 1};
    device(0, 1024, 64, 0);
    CHECK(make(0, MIB, 256, ~0u, 2, gpu, &must) == VK_SUCCESS && alone_of(&must) && !must.info.offset);
    /* the driver's preference holds for more than a block's share: a smaller one goes into a block, with no call to
       the driver of its own, and the report counts it */
    CHECK(make(1, 32 * MIB, 256, ~0u, 1, gpu, &rather) == VK_SUCCESS && alone_of(&rather));
    CHECK(make(0, MIB, 256, ~0u, 0, mine, &own) == VK_SUCCESS && alone_of(&own));
    CHECK(make(0, MIB, 256, ~0u, 0, gpu, &block) == VK_SUCCESS && !alone_of(&block));
    CHECK(make(1, MIB, 256, ~0u, 1, gpu, &h) == VK_SUCCESS && !alone_of(&h) && h.info.memory == block.info.memory);
    CHECK(g_devmem.rather_in_block == 1 && g_devmem.rather_in_block_bytes == MIB);
    unmake(&h);
    /* the driver has no memory for a resource alone: the one it would rather have so goes into a block, which has
       room, and the one it must have so fails */
    g_drv.no_alone = 1;
    CHECK(make(1, MIB, 256, ~0u, 1, gpu, &h) == VK_SUCCESS && !alone_of(&h));
    CHECK(h.info.memory == block.info.memory);
    unmake(&h);
    CHECK(make(1, MIB, 256, ~0u, 2, gpu, &h) != VK_SUCCESS);
    g_drv.no_alone = 0;
    /* with three quarters of the device's allocations in use, the driver's preference gives way to a block */
    g_vk.props.limits.maxMemoryAllocationCount = g_drv.mems + 1;
    CHECK(make(1, MIB, 256, ~0u, 1, gpu, &h) == VK_SUCCESS && !alone_of(&h));
    unmake(&h);
    g_vk.props.limits.maxMemoryAllocationCount = 4096;
    /* a memory type the resource does not allow is never its memory: the window's type alone */
    CHECK(make(0, MIB, 256, 1u << T_WINDOW, 0, gpu, &h) == VK_SUCCESS && h.info.type == T_WINDOW);
    unmake(&h);
    CHECK(make(0, MIB, 256, 1u << T_HOST, 0, gpu, &h) != VK_SUCCESS); /* system memory, for the GPU's own: none */
    unmake(&must);
    unmake(&rather);
    unmake(&own);
    /* the last allocation of a block goes: the block stays, and the next request makes no call to the driver */
    VkDeviceMemory memory = block.info.memory;
    unmake(&block);
    u32 calls = g_drv.allocations;
    CHECK(make(0, MIB, 256, ~0u, 0, gpu, &h) == VK_SUCCESS && h.info.memory == memory && g_drv.allocations == calls);
    unmake(&h);
    check_counts(0);
}

/* two allocations in one block, and one with memory of its own: the block has the priority of the higher class it
   holds, and the driver is told only when that changes, and never where it does not move memory */
static void check_priority(void) {
    Held a, b, own;
    DevMemWant gpu = {.at = DM_DEVICE}, mine = {.at = DM_DEVICE, .own = 1};
    device(0, 1024, 64, 0);
    g_vk.has_pageable = 1;
    CHECK(make(0, MIB, 256, ~0u, 0, gpu, &a) == VK_SUCCESS && make(0, MIB, 256, ~0u, 0, gpu, &b) == VK_SUCCESS);
    CHECK(a.info.memory == b.info.memory);
    const DrvMem* block = &g_drv.mem[(u32)(uintptr_t)a.info.memory - 1];
    prio_settle();
    CHECK(block->prio == 0.5f && !g_drv.priorities); /* both are of the middle class at first, as the memory */
    vk_devmem_priority(a.mem, DM_PRIO_LOW);
    prio_settle();
    CHECK(block->prio == 0.5f && !g_drv.priorities); /* b holds it there */
    vk_devmem_priority(b.mem, DM_PRIO_LOW);
    prio_settle();
    CHECK(block->prio == 0.0f && g_drv.priorities == 1);
    vk_devmem_priority(b.mem, DM_PRIO_LOW); /* the same class: no call */
    vk_devmem_priority(a.mem, DM_PRIO_HIGH);
    prio_settle();
    CHECK(block->prio == 1.0f && g_drv.priorities == 2);
    unmake(&a);
    prio_settle();
    CHECK(block->prio == 0.0f && g_drv.priorities == 3);
    VkDeviceMemory memory = b.info.memory;
    unmake(&b); /* the block stays, empty, at the low class */
    prio_settle();
    CHECK(g_drv.mem[(u32)(uintptr_t)memory - 1].live && block->prio == 0.0f && g_drv.priorities == 3);
    CHECK(make(0, MIB, 256, ~0u, 0, gpu, &a) == VK_SUCCESS && a.info.memory == memory);
    prio_settle();
    CHECK(block->prio == 0.5f && g_drv.priorities == 4); /* its new allocation's class */
    CHECK(make(0, MIB, 256, ~0u, 0, mine, &own) == VK_SUCCESS && alone_of(&own));
    vk_devmem_priority(own.mem, DM_PRIO_HIGH);
    prio_settle();
    CHECK(g_drv.mem[(u32)(uintptr_t)own.info.memory - 1].prio == 1.0f && g_drv.priorities == 5);
    check_priorities();
    unmake(&own);
    unmake(&a);
    /* a request of the low class where the driver has no room for a new block, and no block of that class exists:
       it takes the room of a block of the other class, and does not fail where it found memory before the classes */
    Held low_a, low_b, low_own;
    DevMemWant low = {.at = DM_DEVICE, .low = 1}, low_mine = {.at = DM_DEVICE, .low = 1, .own = 1};
    CHECK(make(0, MIB, 256, ~0u, 0, gpu, &a) == VK_SUCCESS);
    u64 room = g_drv.room[0];
    g_drv.room[0] = g_drv.used[0];
    CHECK(make(0, MIB, 256, ~0u, 0, low, &low_a) == VK_SUCCESS && low_a.info.memory == a.info.memory);
    check_priorities();
    unmake(&low_a);
    g_drv.room[0] = room;
    g_devmem.full_frame[T_GPU] = 0; /* the heap has room again */
    /* with room: a block of its own, made at the low priority with no call; memory of its own at that priority too */
    u32 calls_low = g_drv.priorities;
    CHECK(make(0, MIB, 256, ~0u, 0, low, &low_a) == VK_SUCCESS && low_a.info.memory != a.info.memory);
    CHECK(make(0, MIB, 256, ~0u, 0, low, &low_b) == VK_SUCCESS && low_b.info.memory == low_a.info.memory);
    prio_settle();
    CHECK(g_drv.mem[(u32)(uintptr_t)low_a.info.memory - 1].prio == 0.0f && g_drv.priorities == calls_low);
    CHECK(make(0, MIB, 256, ~0u, 0, low_mine, &low_own) == VK_SUCCESS && alone_of(&low_own));
    prio_settle();
    CHECK(g_drv.mem[(u32)(uintptr_t)low_own.info.memory - 1].prio == 0.0f && g_drv.priorities == calls_low);
    /* a texture that took drawn pixels: its block rises, and falls again when it loads from memory */
    vk_devmem_priority(low_b.mem, DM_PRIO_MID);
    prio_settle();
    CHECK(g_drv.mem[(u32)(uintptr_t)low_a.info.memory - 1].prio == 0.5f && g_drv.priorities == calls_low + 1);
    vk_devmem_priority(low_b.mem, DM_PRIO_LOW);
    prio_settle();
    CHECK(g_drv.mem[(u32)(uintptr_t)low_a.info.memory - 1].prio == 0.0f && g_drv.priorities == calls_low + 2);
    check_priorities();
    VkDeviceMemory low_mem = low_a.info.memory;
    unmake(&low_b);
    unmake(&low_a); /* the low block stays empty, the one spare block of the kind */
    unmake(&low_own);
    unmake(&a); /* the mid block goes: the low block is the spare */
    CHECK(g_drv.mem[(u32)(uintptr_t)low_mem - 1].live);
    /* a request of the other class takes the empty block, with no new one from the driver */
    u32 made = g_drv.allocations;
    CHECK(make(0, MIB, 256, ~0u, 0, gpu, &a) == VK_SUCCESS && a.info.memory == low_mem && g_drv.allocations == made);
    CHECK(!g_devmem.block[g_devmem.rec[a.mem - 1].block - 1].low);
    check_priorities();
    unmake(&a);
    /* a driver that does not move memory: the classes are kept, with no call */
    g_vk.has_pageable = 0;
    u32 calls = g_drv.priorities;
    CHECK(make(0, MIB, 256, ~0u, 0, gpu, &a) == VK_SUCCESS);
    vk_devmem_priority(a.mem, DM_PRIO_HIGH);
    prio_settle();
    CHECK(g_drv.priorities == calls && g_devmem.rec[a.mem - 1].prio == DM_PRIO_HIGH);
    check_priorities();
    unmake(&a);
    check_counts(0);
}

/* the block of an allocation (its index), from the allocator's table */
static u32 block_of(const Held* h) { return g_devmem.rec[h->mem - 1].block - 1; }
/* What moves and what does not, in blocks of their own kinds where blocks are emptied: an allocation that no owner moves
   kept every block from being emptied (drain_can), as in one game's race under a budget of 2 GB, where sparse blocks
   and buffer pages went into every block of the GPU's heap. A block of what moves is emptied while those exist, and a
   move goes to the blocks of what moves. A UMA device empties no block, and keeps one kind */
static void check_kinds(void) {
    enum { N = 24, F = 8 };
    Held m[N], f[F];
    const DevMemWant moves = {.at = DM_DEVICE, .moves = 1}, fixed = {.at = DM_DEVICE};
    device(0, 1024, 64, 0);
    CHECK(g_devmem.drain.on);
    /* no room for a new block: a request takes the room of the other side, and does not fail where it found room
       before the kinds */
    CHECK(make(0, MIB, 256, ~0u, 0, moves, &m[0]) == VK_SUCCESS);
    u64 room = g_drv.room[0];
    g_drv.room[0] = g_drv.used[0];
    CHECK(make(0, MIB, 256, ~0u, 0, fixed, &f[0]) == VK_SUCCESS && block_of(&f[0]) == block_of(&m[0]));
    unmake(&f[0]);
    unmake(&m[0]);
    g_drv.room[0] = room;
    g_devmem.full_frame[T_GPU] = 0; /* the heap has room again */
    for (u32 i = 0, j = 0; i < N; i++) {
        CHECK(make(0, MIB, 256, ~0u, 0, moves, &m[i]) == VK_SUCCESS && !alone_of(&m[i]));
        vk_devmem_owner(m[i].mem, (void*)(uintptr_t)(i + 1));
        if (i % 3 == 0 && j < F) CHECK(make(0, MIB, 256, ~0u, 0, fixed, &f[j++]) == VK_SUCCESS && !alone_of(&f[j - 1]));
    }
    for (u32 i = 0; i < N; i++)
        for (u32 j = 0; j < F; j++) CHECK(block_of(&m[i]) != block_of(&f[j]));
    for (u32 j = 0; j < F; j++) CHECK(g_devmem.block[block_of(&f[j])].image & DM_KIND_FIXED);
    /* what does not move and comes and goes (DevMemWant.brief) has blocks apart from what stays, and from what moves:
       a block of brief allocations empties when they go, though rings and encoders stay */
    Held b[2];
    const DevMemWant brief = {.at = DM_DEVICE, .brief = 1};
    CHECK(make(0, MIB, 256, ~0u, 0, brief, &b[0]) == VK_SUCCESS && make(0, MIB, 256, ~0u, 0, brief, &b[1]) == VK_SUCCESS);
    CHECK(block_of(&b[0]) == block_of(&b[1]));
    CHECK(g_devmem.block[block_of(&b[0])].image == (DM_KIND_FIXED | DM_KIND_BRIEF));
    for (u32 j = 0; j < F; j++) CHECK(block_of(&b[0]) != block_of(&f[j]));
    for (u32 i = 0; i < N; i++) CHECK(block_of(&b[0]) != block_of(&m[i]));
    const u32 bb = block_of(&b[0]);
    unmake(&b[0]);
    unmake(&b[1]);
    CHECK(!g_devmem.block[bb].allocs);
    /* the first block of what moves keeps one allocation and the last another, with room for the first's block: the
       first is emptied, past the budget, though the blocks of what does not move hold allocations still */
    const u32 first = block_of(&m[0]), last = block_of(&m[N - 1]);
    CHECK(first != last);
    for (u32 i = 1; i < N - 1; i++) unmake(&m[i]);
    DevMem mems[4];
    void* owners[4];
    u32 n = 0;
    for (u32 p = 0; p < 31 && !n; p++) {
        vk_devmem_frame();
        n = vk_devmem_moving(mems, owners, 4, 1);
    }
    CHECK(n == 1 && mems[0] == m[0].mem && owners[0] == (void*)(uintptr_t)1);
    CHECK(g_devmem.drain.slot == first + 1);
    /* the move: into the last block, the one of what moves that holds something and is not being emptied */
    Held to;
    CHECK(make(0, MIB, 256, ~0u, 0, (DevMemWant){.at = DM_DEVICE, .existing = 1}, &to) == VK_SUCCESS);
    CHECK(block_of(&to) == last);
    vk_devmem_owner(m[0].mem, NULL);
    unmake(&m[0]);
    vk_devmem_moved(MIB);
    unmake(&to);
    unmake(&m[N - 1]);
    for (u32 j = 0; j < F; j++) unmake(&f[j]);
    check_counts(0);
    /* a UMA device: one kind, and what moves shares blocks with what does not */
    device(1, 1, 64, 0);
    CHECK(!g_devmem.drain.on);
    CHECK(make(0, MIB, 256, ~0u, 0, moves, &m[0]) == VK_SUCCESS && make(0, MIB, 256, ~0u, 0, fixed, &f[0]) == VK_SUCCESS);
    CHECK(block_of(&m[0]) == block_of(&f[0]));
    unmake(&m[0]);
    unmake(&f[0]);
    check_counts(0);
}

/* With no room for a new block, a request takes a block of another kind of its side: what stays takes the blocks of
   what moves before the brief ones, which it would keep from being emptied, and what comes and goes takes the blocks of
   what stays first */
static void check_kind_fallback(void) {
    const DevMemWant moves = {.at = DM_DEVICE, .moves = 1}, fixed = {.at = DM_DEVICE},
                     brief = {.at = DM_DEVICE, .brief = 1};
    Held a, b, f;
    device(0, 1024, 64, 0);
    CHECK(make(0, MIB, 256, ~0u, 0, moves, &a) == VK_SUCCESS);
    vk_devmem_owner(a.mem, (void*)(uintptr_t)1);
    CHECK(make(0, MIB, 256, ~0u, 0, brief, &b) == VK_SUCCESS && block_of(&b) != block_of(&a));
    u64 room = g_drv.room[0];
    g_drv.room[0] = g_drv.used[0];
    CHECK(make(0, MIB, 256, ~0u, 0, fixed, &f) == VK_SUCCESS && block_of(&f) == block_of(&a));
    g_drv.room[0] = room;
    unmake(&f);
    unmake(&b);
    vk_devmem_owner(a.mem, NULL);
    unmake(&a);
    check_counts(0);
    device(0, 1024, 64, 0);
    CHECK(make(0, MIB, 256, ~0u, 0, moves, &a) == VK_SUCCESS);
    vk_devmem_owner(a.mem, (void*)(uintptr_t)1);
    CHECK(make(0, MIB, 256, ~0u, 0, fixed, &f) == VK_SUCCESS && block_of(&f) != block_of(&a));
    room = g_drv.room[0];
    g_drv.room[0] = g_drv.used[0];
    CHECK(make(0, MIB, 256, ~0u, 0, brief, &b) == VK_SUCCESS && block_of(&b) == block_of(&f));
    g_drv.room[0] = room;
    unmake(&b);
    unmake(&f);
    vk_devmem_owner(a.mem, NULL);
    unmake(&a);
    check_counts(0);
}

/* Memory of its own that the driver only preferred is kept when let go, for the next request it fits, with no call to
   the driver (own_keep, own_take). It names no resource, and fits a request of its size up to an eighth less. Kept
   memory goes after DM_KEEP_PRESENTS presents, and the oldest of its heap goes first past keep_cap */
static void check_keep(void) {
    Held a, b, c;
    const DevMemWant gpu = {.at = DM_DEVICE};
    device(0, 1024, 64, 0);
    g_vk.has_pageable = 1;
    CHECK(make(1, 32 * MIB, 256, ~0u, 1, gpu, &a) == VK_SUCCESS && alone_of(&a));
    CHECK(!g_drv.mem[(u32)(uintptr_t)a.info.memory - 1].res); /* no resource named: another may take it */
    const VkDeviceMemory memory = a.info.memory;
    unmake(&a);
    CHECK(g_devmem.kept_bytes == 32 * MIB);
    /* kept: handed out for the caches (no free room inside blocks), at the low class's priority */
    DevMemBudget bud[VK_MAX_MEMORY_HEAPS];
    vk_devmem_budgets(bud);
    CHECK(bud[0].allocated_bytes == 32 * MIB && bud[0].block_bytes == 32 * MIB);
    prio_settle();
    CHECK(g_drv.mem[(u32)(uintptr_t)memory - 1].prio == 0.0f);
    u32 calls = g_drv.allocations;
    CHECK(make(1, 30 * MIB, 256, ~0u, 1, gpu, &a) == VK_SUCCESS && a.info.memory == memory);
    CHECK(g_drv.allocations == calls && g_devmem.kept_bytes == 0 && g_devmem.kept_taken == 1);
    /* more than it holds, or far less: memory of its own made for it */
    CHECK(make(0, 40 * MIB, 256, ~0u, 1, gpu, &b) == VK_SUCCESS && b.info.memory != memory);
    unmake(&a);
    CHECK(make(0, 20 * MIB, 256, ~0u, 1, gpu, &c) == VK_SUCCESS && c.info.memory != memory);
    unmake(&b);
    unmake(&c);
    CHECK(g_devmem.kept_bytes == 92 * MIB);
    /* past keep_cap the oldest goes: DM_KEEP_BYTES under a budget of 2 GiB */
    const u64 drv = g_devmem.drv_budget[0];
    const int had = g_vk.has_budget;
    g_vk.has_budget = 1;
    g_devmem.drv_budget[0] = 2ull << 30;
    CHECK(keep_cap(0) == DM_KEEP_BYTES);
    Held m[8];
    for (u32 i = 0; i < 8; i++) CHECK(make(0, 33 * MIB + i * MIB, 256, ~0u, 1, gpu, &m[i]) == VK_SUCCESS);
    for (u32 i = 0; i < 8; i++) unmake(&m[i]);
    CHECK(g_devmem.kept_bytes <= DM_KEEP_BYTES && g_devmem.kept_gone > 0);
    /* a heap of a large budget keeps a share of it */
    g_devmem.drv_budget[0] = 16ull << 30;
    CHECK(keep_cap(0) == heap_budget(0) / DM_KEEP_SHARE && keep_cap(0) > DM_KEEP_BYTES);
    /* only in the GPU's own heap: the others keep DM_KEEP_BYTES */
    CHECK(heap_capped(0));
    for (u32 h = 0; h < g_devmem.mp.memoryHeapCount; h++)
        if (!heap_capped(h)) CHECK(keep_cap(h) == DM_KEEP_BYTES);
    /* the other heaps count together against that cap, the GPU's own heap alone */
    CHECK(!heap_capped(1) && !heap_capped(2));
    const u64 k0 = g_devmem.kept_heap[0], k1 = g_devmem.kept_heap[1], k2 = g_devmem.kept_heap[2];
    g_devmem.kept_heap[1] = 10, g_devmem.kept_heap[2] = 20, g_devmem.kept_heap[0] = 40;
    CHECK(kept_with(1) == 30 && kept_with(2) == 30 && kept_with(0) == 40);
    g_devmem.kept_heap[0] = k0, g_devmem.kept_heap[1] = k1, g_devmem.kept_heap[2] = k2;
    /* under a caches' budget less than the driver's (vk_budget_mb), the share and the full mark are of that one */
    vk_devmem_cache_budget(2ull << 30);
    CHECK(keep_budget(0) == 2ull << 30 && keep_cap(0) == DM_KEEP_BYTES);
    /* and a request the driver would rather have alone, past four fifths of that budget, gets no memory kept for
       reuse (DM_OWN_RATHER), which own_keep would give straight back to the driver */
    {
        Held r;
        const u64 bb = g_devmem.block_bytes[0], rather = g_devmem.own_why[DM_OWN_RATHER];
        g_devmem.block_bytes[0] = 2ull << 30;
        CHECK(make(1, 32 * MIB, 256, ~0u, 1, gpu, &r) == VK_SUCCESS);
        CHECK(g_devmem.own_why[DM_OWN_RATHER] == rather && !g_devmem.block[block_of(&r)].reuse);
        g_devmem.block_bytes[0] = g_devmem.block_bytes[0] - (2ull << 30) + bb; /* its own bytes stay counted */
        unmake(&r);
    }
    vk_devmem_cache_budget(0);
    CHECK(keep_budget(0) == heap_budget(0));
    g_devmem.drv_budget[0] = drv;
    g_vk.has_budget = had;
    /* and all of it after DM_KEEP_PRESENTS presents */
    for (u32 p = 0; p <= DM_KEEP_PRESENTS; p++) vk_devmem_frame();
    CHECK(g_devmem.kept_bytes == 0);
    /* a request with no room but the kept memory's: it goes, and the request takes the room (the thread ahead is off,
       so that the driver has its memory back before the request asks again) */
    g_devmem.ahead.on = 0;
    CHECK(make(1, 32 * MIB, 256, ~0u, 1, gpu, &a) == VK_SUCCESS);
    unmake(&a);
    CHECK(g_devmem.kept_bytes == 32 * MIB);
    u64 room = g_drv.room[0];
    g_drv.room[0] = g_drv.used[0] + 16 * MIB;
    CHECK(make(0, 40 * MIB, 256, ~0u, 1, gpu, &b) == VK_SUCCESS && g_devmem.kept_bytes == 0 && b.info.type == T_GPU);
    unmake(&b);
    g_drv.room[0] = room;
    g_vk.has_pageable = 0;
    check_counts(0);
}

/* the GPU's own heap under a budget of 1 GiB: its use stops a margin below it, though the driver has room for more,
   what does not fit goes to the window and then fails, never to system memory */
static void check_cap(void) {
    enum { CAP = 512 };
    static Held held[CAP];
    const u64 budget = GIB, margin = 256 * MIB;
    DevMemWant gpu = {.at = DM_DEVICE};
    u32 n = 0;
    device(0, 1024, 64, budget);
    VkResult res = VK_SUCCESS;
    while (n < CAP && (res = make(0, 4 * MIB, 4096, ~0u, 0, gpu, &held[n])) == VK_SUCCESS) n++;
    CHECK(n < CAP && res == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    CHECK(g_drv.peak[0] <= budget - margin && g_drv.used[0] > budget - margin - 64 * MIB);
    CHECK(g_drv.used[2] == g_drv.room[2] && !g_drv.used[1]);
    check_counts(n);
    /* six neighbours of a 64 MiB block go: a request of 20 MiB, more than a quarter of a block, takes their run
       with no call to the driver */
    u32 first = 0, run = 0;
    for (u32 i = 0; i < n && run < 6; i++) {
        u32 m = (u32)(uintptr_t)held[i].info.memory;
        int next = run && held[i].info.memory == held[first].info.memory &&
                   held[i].info.offset == held[first].info.offset + run * 4 * MIB;
        if (next)
            run++;
        else if (g_drv.mem[m - 1].size == 64 * MIB && drv_heap(held[i].info.type) == 0) {
            first = i;
            run = 1;
        } else
            run = 0;
    }
    CHECK(run == 6);
    VkDeviceMemory memory = held[first].info.memory;
    u64 at = held[first].info.offset;
    for (u32 i = 0; i < 6; i++) unmake(&held[first + i]);
    u32 calls = g_drv.allocations;
    CHECK(make(0, 20 * MIB, 4096, ~0u, 0, gpu, &held[first]) == VK_SUCCESS && g_drv.allocations == calls);
    CHECK(held[first].info.memory == memory && held[first].info.offset == at);
    for (u32 i = 0; i < n; i++)
        if (held[i].mem) unmake(&held[i]);
    check_counts(0);
}

/* memory the CPU maps without coherence: an allocation starts on an atom, and a flush or an invalidate names whole
   atoms that hold the bytes asked and no byte of another allocation */
static void check_atoms(u64 atom) {
    enum { CAP = 256 };
    static Held held[CAP];
    Held gpu;
    u32 n = 0;
    device(1, 1024, atom, 0);
    for (u32 r = 0; r < 4000; r++) {
        if (n < CAP && (!n || rnd() % 5 < 3)) {
            DevMemWant want = {.at = rnd() % 2 ? DM_HOST : DM_CACHED, .mapped = 1};
            const u64 size = 1 + rnd() % (300 * KIB), align = 1ull << rnd() % 9; /* in this order on every compiler */
            CHECK(make(0, size, align, ~0u, 0, want, &held[n]) == VK_SUCCESS);
            CHECK(held[n].info.offset % atom == 0 && held[n].info.type == (want.at == DM_HOST ? 1u : 2u));
            n++;
        } else {
            u32 k = rnd() % n;
            unmake(&held[k]);
            held[k] = held[--n];
        }
        const Held* h = &held[rnd() % (n ? n : 1)];
        if (!n) continue;
        u64 off = rnd() % h->info.size, size = rnd() % 4 ? 1 + rnd() % (h->info.size - off) : VK_WHOLE_SIZE;
        u32 before = g_drv.ranges;
        CHECK((rnd() % 2 ? vk_devmem_flush : vk_devmem_invalidate)(h->mem, off, size) == VK_SUCCESS);
        CHECK(g_drv.ranges == before + 1 && g_drv.range.memory == h->info.memory);
        u64 lo = h->info.offset + off, hi = size == VK_WHOLE_SIZE ? h->info.offset + h->info.size : lo + size;
        CHECK(g_drv.range.offset <= lo && g_drv.range.offset + g_drv.range.size >= hi);
        for (u32 i = 0; i < n; i++)
            CHECK(&held[i] == h || held[i].info.memory != h->info.memory ||
                  held[i].info.offset + held[i].info.size <= g_drv.range.offset ||
                  held[i].info.offset >= g_drv.range.offset + g_drv.range.size);
        /* nothing past the allocation's end, and nothing for memory the CPU does not map */
        CHECK(vk_devmem_flush(h->mem, h->info.size, 16) == VK_SUCCESS && g_drv.ranges == before + 1);
    }
    DevMemWant own = {.at = DM_DEVICE}, mapped = {.at = DM_HOST, .mapped = 1},
               mine = {.at = DM_HOST, .mapped = 1, .own = 1};
    CHECK(make(0, MIB, 256, ~0u, 0, own, &gpu) == VK_SUCCESS);
    u32 before = g_drv.ranges;
    CHECK(vk_devmem_flush(gpu.mem, 0, VK_WHOLE_SIZE) == VK_SUCCESS && g_drv.ranges == before);
    unmake(&gpu);
    /* and nothing for memory the CPU maps with coherence, on a device that has both */
    CHECK(make(0, MIB, 256, 1u << 3, 0, mapped, &gpu) == VK_SUCCESS && gpu.info.type == 3 && gpu.info.ptr);
    CHECK(vk_devmem_flush(gpu.mem, 0, VK_WHOLE_SIZE) == VK_SUCCESS && g_drv.ranges == before);
    unmake(&gpu);
    /* memory of its own that ends inside an atom: the range ends where the memory does */
    CHECK(make(0, 5 * atom + 1, 1, ~0u, 0, mine, &gpu) == VK_SUCCESS && alone_of(&gpu));
    CHECK(vk_devmem_flush(gpu.mem, atom + 1, VK_WHOLE_SIZE) == VK_SUCCESS && g_drv.ranges == before + 1);
    CHECK(g_drv.range.offset == atom && g_drv.range.size == 4 * atom + 1);
    unmake(&gpu);
    while (n) unmake(&held[--n]);
    check_counts(0);
}

/* ------------------------------------------------ several threads, and the allocator's own */
#define WORKERS 4
static struct {
    PMutex lk;
    PCond cv;
    u32 done;
} g_work = {.lk = PMUTEX_INIT};
static void worker(void* arg) {
    enum { CAP = 128 };
    Held held[CAP];
    u32 seed = 0x1234567u + (u32)(uintptr_t)arg * 0x9e3779b9u, n = 0;
    for (u32 r = 0; r < 6000; r++) {
        if (n < CAP && (!n || rnd_of(&seed) % 5 < 3)) {
            if (make_random(&seed, &held[n]) == VK_SUCCESS) n++;
        } else {
            u32 k = rnd_of(&seed) % n;
            unmake(&held[k]);
            held[k] = held[--n];
        }
    }
    while (n) unmake(&held[--n]);
    pm_lock(&g_work.lk);
    g_work.done++;
    pc_signal(&g_work.cv);
    pm_unlock(&g_work.lk);
}
/* the allocator's thread has made the block it was asked for and given back what it had to: or `until` is set */
static void ahead_wait(const u32* until) {
    /* by the clock, not by the waits: a timed wait may come back early, and on a CI machine of macOS 20,000 of them
       ended in 0.3 s while the thread was still at work */
    const u64 t0 = plat_ns();
    pm_lock(&g_work.lk);
    for (;;) {
        u32 blocks = 0;
        pm_lock(&g_devmem.lk);
        int idle = !g_devmem.ahead.want && !g_devmem.ahead.busy && !g_devmem.ahead.ngone;
        for (u32 h = 0; h < VK_MAX_MEMORY_HEAPS; h++) blocks += g_devmem.blocks[h];
        pm_unlock(&g_devmem.lk);
        pm_lock(&g_drv.lk);
        int done = until ? *until != 0 : idle && blocks == g_drv.mems; /* a free on its way is still the driver's */
        pm_unlock(&g_drv.lk);
        if (done) break;
        CHECK(plat_ns() - t0 < 30000000000ull);
        pc_wait(&g_work.cv, &g_work.lk, 1000000);
    }
    pm_unlock(&g_work.lk);
}
/* The block that the thread was asked for and has not begun counts in the heap's use: requests that fill the heap
   meanwhile leave room for it, and the heap stays under its cap when the thread makes it. The thread is kept from it
   in the driver's free of an allocation of system memory */
static void check_ahead_cap(u64 budget) {
    enum { CAP = 512 };
    static Held held[CAP];
    Held first, own;
    DevMemWant gpu = {.at = DM_DEVICE}, mine = {.at = DM_HOST, .mapped = 1, .own = 1};
    u32 n = 0;
    CHECK(make(0, 4 * MIB, 4096, ~0u, 0, gpu, &first) == VK_SUCCESS);
    ahead_wait(NULL); /* the next block of the kind is there */
    CHECK(make(0, MIB, 4096, ~0u, 0, mine, &own) == VK_SUCCESS && drv_heap(own.info.type) == 1);
    pm_lock(&g_drv.lk);
    g_drv.hold_free = 1;
    pm_unlock(&g_drv.lk);
    unmake(&own);
    ahead_wait(&g_drv.free_waits);
    while (n < CAP && make(0, 4 * MIB, 4096, ~0u, 0, gpu, &held[n]) == VK_SUCCESS)
        if (drv_heap(held[n++].info.type) != 0) break; /* the heap is full: this one went to the window */
    pm_lock(&g_devmem.lk);
    CHECK(g_devmem.ahead.want && !g_devmem.ahead.busy && heap_capped(0));
    CHECK(g_devmem.mp.memoryTypes[g_devmem.ahead.type].heapIndex == 0);
    pm_unlock(&g_devmem.lk);
    CHECK(n && n < CAP && g_drv.used[0] <= budget - 256 * MIB);
    pm_lock(&g_drv.lk);
    g_drv.hold_free = g_drv.free_waits = 0;
    pc_broadcast(&g_drv.cv);
    pm_unlock(&g_drv.lk);
    ahead_wait(NULL);
    CHECK(g_drv.peak[0] <= budget - 256 * MIB && g_drv.used[0] > budget - 256 * MIB - 64 * MIB);
    unmake(&first);
    while (n) unmake(&held[--n]);
    ahead_wait(NULL);
    check_counts(0);
}
/* The thread makes the next block of a kind only when no empty block of that kind is there, of either class: an
   empty block of the low class serves a request of the middle class (block_adopt), so no more is made for it */
static void check_ahead_class(void) {
    Held mid, low_a, mid_b;
    DevMemWant gpu = {.at = DM_DEVICE}, low = {.at = DM_DEVICE, .low = 1};
    g_vk.has_pageable = 1;
    CHECK(make(0, MIB, 4096, ~0u, 0, gpu, &mid) == VK_SUCCESS);
    ahead_wait(NULL); /* the spare block of the kind, of the middle class */
    CHECK(make(0, MIB, 4096, ~0u, 0, low, &low_a) == VK_SUCCESS && low_a.info.memory != mid.info.memory);
    ahead_wait(NULL); /* the low request took the spare: a new spare */
    unmake(&low_a); /* one of the two empty blocks goes, and one of the low class stays */
    ahead_wait(NULL);
    u32 made = g_drv.allocations;
    CHECK(make(0, MIB, 4096, ~0u, 0, gpu, &mid_b) == VK_SUCCESS && mid_b.info.memory == mid.info.memory);
    ahead_wait(NULL);
    CHECK(g_drv.allocations == made); /* the empty block of the low class is the spare of the middle class too */
    unmake(&mid_b);
    unmake(&mid);
    ahead_wait(NULL);
    g_vk.has_pageable = 0;
    check_counts(0);
}
/* With the thread ahead, the driver is told a block's priority on that thread (devmem.c prio_set), not on the one that
   asked: what each block should have is what the driver has once the queue is done, and a change undone before the
   thread came to it makes no call. On check_threads' device and its one thread: a device() of its own left this
   thread running, and the next device() copied a new state over the lock and the condition it waited on, then
   started a second thread on it. The macOS CI found the two of them in check_ahead_cap */
static void check_priority_ahead(void) {
    Held a;
    const DevMemWant gpu = {.at = DM_DEVICE};
    CHECK(g_devmem.ahead.on);
    g_vk.has_pageable = 1;
    CHECK(make(0, MIB, 256, ~0u, 0, gpu, &a) == VK_SUCCESS);
    const DrvMem* block = &g_drv.mem[(u32)(uintptr_t)a.info.memory - 1];
    vk_devmem_priority(a.mem, DM_PRIO_HIGH);
    prio_settle();
    CHECK(block->prio == 1.0f);
    check_priorities();
    pm_lock(&g_devmem.lk); /* the thread waits for the lock: both changes are in before it comes */
    const u32 calls = g_drv.priorities;
    Block* k = &g_devmem.block[g_devmem.rec[a.mem - 1].block - 1];
    prio_set(k, DM_PRIO_LOW);
    prio_set(k, DM_PRIO_HIGH);
    pm_unlock(&g_devmem.lk);
    prio_settle();
    CHECK(block->prio == 1.0f && g_drv.priorities == calls);
    unmake(&a);
    g_vk.has_pageable = 0;
    ahead_wait(NULL); /* the thread gives back what was let go */
    check_counts(0);
}
static void check_threads(void) {
    const u64 budget = GIB;
    g_threads = 1;
    device(0, 1024, 64, budget);
    CHECK(g_devmem.ahead.on);
    pc_init(&g_work.cv);
    pc_init(&g_drv.cv);
    check_ahead_class();
    check_ahead_cap(budget);
    check_priority_ahead();
    for (u32 i = 0; i < WORKERS; i++) CHECK(plat_thread(worker, (void*)(uintptr_t)i, 0) == 0);
    pm_lock(&g_work.lk);
    while (g_work.done < WORKERS) pc_wait(&g_work.cv, &g_work.lk, -1);
    pm_unlock(&g_work.lk);
    ahead_wait(NULL);
    CHECK(g_devmem.ahead.made); /* it made blocks ahead of their requests */
    check_counts(0);
}

/* Defragmentation (vk_devmem_moving), with and without the priority classes (pageable). Images of 1 MiB fill
   blocks, three of every four go, and the owners move what the allocator names, as texture.c's moves do: a move
   lands in a block in use, never in the one being emptied or an empty one, and where the driver moves memory by
   priority, in a block of its own class. No other request takes a run of the block being emptied. Blocks are
   emptied, and the counts and priorities hold throughout. With pageable, a block of the low class given allocations
   of a higher class is repaired: those alone move, to blocks of their class. Without it, no block is repaired */
enum { DEFRAG_N = 256 };
static Held g_dh[DEFRAG_N];
static int defrag_round(u64 budget) {
    DevMem mems[64];
    void* owners[64];
    g_devmem.frame += 31;
    const u32 n = vk_devmem_moving(mems, owners, 64, budget);
    /* the moves' frees count toward the forced trim (vk_defer_moved) only out of a block closed to requests: one
       emptied, not one repaired */
    CHECK(vk_devmem_emptying() == (g_devmem.drain.slot && !g_devmem.drain.repair));
    int moved = 0;
    for (u32 i = 0; i < n; i++) {
        Held* x = owners[i];
        CHECK(x >= g_dh && x < g_dh + DEFRAG_N && x->mem == mems[i]);
        const u32 from = g_devmem.rec[x->mem - 1].block;
        const u32 prio = g_devmem.rec[x->mem - 1].prio;
        Held to;
        DevMemWant want = {.at = DM_DEVICE, .image = 1, .existing = 1, .low = prio == DM_PRIO_LOW};
        if (make(1, x->info.size, 4096, ~0u, 0, want, &to) != VK_SUCCESS) {
            vk_devmem_moving_stop();
            break;
        }
        const Rec* r = &g_devmem.rec[to.mem - 1];
        const Block* k = &g_devmem.block[r->block - 1];
        CHECK(r->block != from && !k->drain && k->allocs >= 2); /* a block in use, which held something before */
        if (g_vk.has_pageable) CHECK(k->low == (prio == DM_PRIO_LOW)); /* of its own class */
        /* the block that took it settles: 120 presents, 600 after a repair's move, as a class can change back */
        CHECK(k->drain_skip >= g_devmem.frame + (g_devmem.drain.repair ? 600 : 120));
        vk_devmem_priority(to.mem, prio);
        vk_devmem_owner(x->mem, NULL);
        unmake(x);
        *x = to;
        vk_devmem_owner(x->mem, x);
        vk_devmem_moved(x->info.size);
        moved++;
    }
    /* while a block is emptied, no request takes its runs */
    if (g_devmem.drain.slot && !g_devmem.drain.repair) {
        Held probe; /* of the class of the block being emptied, which its blocks are */
        DevMemWant gpu = {.at = DM_DEVICE, .image = 1, .low = g_devmem.block[g_devmem.drain.slot - 1].low,
                          .moves = 1};
        CHECK(make(1, 64 * KIB, 4096, ~0u, 0, gpu, &probe) == VK_SUCCESS);
        CHECK(g_devmem.rec[probe.mem - 1].block != g_devmem.drain.slot || !g_devmem.drain.slot);
        unmake(&probe);
    }
    return moved;
}
static void check_defrag(int pageable) {
    const u64 budget = 4 * GIB;
    device(0, 1024, 64, budget);
    g_vk.has_pageable = pageable;
    CHECK(g_devmem.drain.on);
    DevMemWant tex = {.at = DM_DEVICE, .image = 1, .low = 1, .moves = 1}, mid = {.at = DM_DEVICE, .image = 1, .moves = 1};
    for (u32 i = 0; i < DEFRAG_N; i++) {
        CHECK(make(1, MIB, 4096, ~0u, 0, i < 192 ? tex : mid, &g_dh[i]) == VK_SUCCESS);
        vk_devmem_priority(g_dh[i].mem, i < 192 ? DM_PRIO_LOW : DM_PRIO_MID);
        vk_devmem_owner(g_dh[i].mem, &g_dh[i]);
    }
    for (u32 i = 0; i < DEFRAG_N; i++)
        if (i % 4) {
            vk_devmem_owner(g_dh[i].mem, NULL);
            unmake(&g_dh[i]);
        }
    u32 live = 0;
    for (u32 i = 0; i < DEFRAG_N; i++) live += g_dh[i].mem != 0;
    int moves = 0;
    for (u32 round = 0; round < 200; round++) {
        moves += defrag_round(budget);
        check_counts(live);
        if (pageable) check_priorities();
    }
    CHECK(moves > 0 && g_devmem.drain.emptied + g_devmem.drain.kept > 0);
    CHECK(!g_devmem.drain.repaired); /* no class was mixed */
    if (!pageable) CHECK(!repair_pick()); /* the classes are only kept where the driver does not move memory */
    printf("vk_devmem: defrag %s: %d allocations moved, %llu blocks given back, %llu kept\n",
           pageable ? "with the priority classes" : "without them", moves,
           (unsigned long long)g_devmem.drain.emptied, (unsigned long long)g_devmem.drain.kept);
    for (u32 i = 0; i < DEFRAG_N; i++)
        if (g_dh[i].mem) {
            vk_devmem_owner(g_dh[i].mem, NULL);
            unmake(&g_dh[i]);
        }
    check_counts(0);
    if (pageable) {
        /* Repair: blocks filled in order, with no holes to gather, and in the first block of the low class half its
           allocations rise to the high class. The repair moves those, and only those, to blocks of the higher
           classes, and the block stays with its low ones */
        device(0, 1024, 64, budget);
        g_vk.has_pageable = 1;
        enum { LOWS = 40, MIDS = 40 };
        for (u32 i = 0; i < LOWS + MIDS; i++) {
            CHECK(make(1, MIB, 4096, ~0u, 0, i < LOWS ? tex : mid, &g_dh[i]) == VK_SUCCESS);
            vk_devmem_priority(g_dh[i].mem, i < LOWS ? DM_PRIO_LOW : DM_PRIO_MID);
            vk_devmem_owner(g_dh[i].mem, &g_dh[i]);
        }
        const u32 slot = g_devmem.rec[g_dh[0].mem - 1].block;
        CHECK(g_devmem.block[slot - 1].low);
        u32 raised = 0, stays = 0;
        for (u32 i = 0; i < LOWS; i++)
            if (g_devmem.rec[g_dh[i].mem - 1].block == slot) {
                if (i % 2) {
                    vk_devmem_priority(g_dh[i].mem, DM_PRIO_HIGH);
                    raised++;
                } else
                    stays++;
            }
        CHECK(raised && stays && misplaced(&g_devmem.block[slot - 1]));
        int moved = 0;
        for (u32 round = 0; round < 50 && !g_devmem.drain.repaired; round++) {
            moved += defrag_round(budget);
            check_counts(LOWS + MIDS);
            check_priorities();
        }
        CHECK(g_devmem.drain.repaired == 1 && (u32)moved == raised);
        CHECK(!misplaced(&g_devmem.block[slot - 1]) && g_devmem.block[slot - 1].allocs == stays);
        printf("vk_devmem: repair: %d allocations of the high class moved out of a block of the low class\n", moved);
        /* a move of the low class when its blocks have no room and the other class's have: it fails, and takes
           no run of the other class (alloc_in's fallback is a request's, not a move's) */
        Held fill[64], spare, moved_low;
        u32 nfill = 0;
        int full = 0;
        while (nfill < 64 && !full) {
            CHECK(make(1, MIB, 4096, ~0u, 0, tex, &fill[nfill]) == VK_SUCCESS);
            nfill++;
            full = 1;
            for (u32 i = 0; i < g_devmem.nblock; i++) {
                const Block* k = &g_devmem.block[i];
                if (k->mem && !k->own && k->low && k->free.used < k->free.units) full = 0;
            }
        }
        CHECK(full);
        CHECK(make(1, MIB, 4096, ~0u, 0, mid, &spare) == VK_SUCCESS); /* a mid block with room */
        DevMemWant move_low = {.at = DM_DEVICE, .image = 1, .existing = 1, .low = 1};
        g_devmem.drain.moving = 0;
        CHECK(make(1, MIB, 4096, ~0u, 0, move_low, &moved_low) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
        unmake(&spare);
        while (nfill) unmake(&fill[--nfill]);
        for (u32 i = 0; i < LOWS + MIDS; i++) {
            vk_devmem_owner(g_dh[i].mem, NULL);
            unmake(&g_dh[i]);
        }
        check_counts(0);
    }
    /* a GPU that shares the system's memory empties no block, and keeps no owner */
    device(1, 1, 4096, 0);
    g_vk.has_pageable = pageable;
    CHECK(!g_devmem.drain.on);
    Held h;
    CHECK(make(1, MIB, 4096, ~0u, 0, tex, &h) == VK_SUCCESS);
    vk_devmem_owner(h.mem, &h);
    CHECK(!g_devmem.rec[h.mem - 1].owner);
    DevMem mems[4];
    void* owners[4];
    g_devmem.frame += 31;
    CHECK(vk_devmem_moving(mems, owners, 4, GIB) == 0);
    unmake(&h);
}

/* A request that must have memory of its own (the driver requires it, or the caller asks) and finds none while a block
   is emptied fails as it does with none emptied (check_alone): no run of a block is memory of its own, so the drain
   is not stopped for it and its retry takes no run */
static void check_drain_alone(void) {
    const u64 budget = 4 * GIB;
    device(0, 1024, 64, budget);
    DevMemWant tex = {.at = DM_DEVICE, .image = 1, .moves = 1}, gpu = {.at = DM_DEVICE}, mine = {.at = DM_DEVICE, .own = 1};
    for (u32 i = 0; i < DEFRAG_N; i++) {
        CHECK(make(1, MIB, 4096, ~0u, 0, tex, &g_dh[i]) == VK_SUCCESS);
        vk_devmem_owner(g_dh[i].mem, &g_dh[i]);
    }
    for (u32 i = 0; i < DEFRAG_N; i++)
        if (i % 4) {
            vk_devmem_owner(g_dh[i].mem, NULL);
            unmake(&g_dh[i]);
        }
    DevMem mems[64];
    void* owners[64];
    g_devmem.frame += 31;
    CHECK(vk_devmem_moving(mems, owners, 64, budget) > 0 && g_devmem.drain.slot && !g_devmem.drain.repair);
    const u32 slot = g_devmem.drain.slot;
    g_drv.no_alone = 1; /* the driver has no memory for a resource alone; the blocks have room */
    Held h;
    CHECK(make(1, MIB, 256, ~0u, 2, gpu, &h) != VK_SUCCESS);
    CHECK(make(0, MIB, 256, ~0u, 0, mine, &h) != VK_SUCCESS);
    g_drv.no_alone = 0;
    CHECK(g_devmem.drain.slot == slot && g_devmem.block[slot - 1].drain && !g_devmem.drain.gave_up);
    for (u32 i = 0; i < DEFRAG_N; i++)
        if (g_dh[i].mem) {
            vk_devmem_owner(g_dh[i].mem, NULL);
            unmake(&g_dh[i]);
        }
    check_counts(0);
}


/* drain_pick's room test, as DXVK's on a chunk's used pages: a block is emptied when the free space in the other
   blocks of its kind holds what it holds and half as much again, whatever the block's size. Two blocks of 1 MiB
   images, the first filled and the second with 3 MiB free, past the budget: the first with 3 MiB in it stays, as
   4.5 MiB do not fit in the room. With 1 MiB in it, it is emptied, though the room is less than 1.5 times its size */
static void check_drain_room(void) {
    const u64 room = 3 * MIB, high = 3 * MIB, low = MIB;
    DevMemWant tex = {.at = DM_DEVICE, .image = 1, .moves = 1};
    device(0, 1024, 64, 4 * GIB);
    g_vk.has_pageable = 0;
    u32 n = 0, first = 0, second = 0;
    for (;;) {
        CHECK(n < DEFRAG_N && make(1, MIB, 4096, ~0u, 0, tex, &g_dh[n]) == VK_SUCCESS);
        vk_devmem_owner(g_dh[n].mem, &g_dh[n]);
        const u32 b = g_devmem.rec[g_dh[n++].mem - 1].block;
        if (!first) first = b;
        if (b != first && !second) second = b;
        CHECK(b == first || b == second);
        if (second && g_devmem.block[second - 1].free.units - g_devmem.block[second - 1].free.used == room / DM_UNIT)
            break;
    }
    const u64 size = g_devmem.block[first - 1].size;
    CHECK(high <= size / 2 && 2 * room < 3 * high && 2 * room >= 3 * low && 2 * room < 3 * size);
    u64 held = size;
    for (u32 i = 0; i < n && held > high; i++)
        if (g_devmem.rec[g_dh[i].mem - 1].block == first) {
            vk_devmem_owner(g_dh[i].mem, NULL);
            unmake(&g_dh[i]);
            held -= MIB;
        }
    CHECK(held == high && g_devmem.block[first - 1].free.used * DM_UNIT == high);
    pm_lock(&g_devmem.lk);
    CHECK(drain_pick(MIB) == 0); /* 1.5 times what it holds is more than the room */
    pm_unlock(&g_devmem.lk);
    for (u32 i = 0; i < n && held > low; i++)
        if (g_dh[i].mem && g_devmem.rec[g_dh[i].mem - 1].block == first) {
            vk_devmem_owner(g_dh[i].mem, NULL);
            unmake(&g_dh[i]);
            held -= MIB;
        }
    pm_lock(&g_devmem.lk);
    CHECK(drain_pick(MIB) == first); /* the room holds 1.5 times what it holds, not 1.5 times its size */
    pm_unlock(&g_devmem.lk);
    for (u32 i = 0; i < n; i++)
        if (g_dh[i].mem) {
            vk_devmem_owner(g_dh[i].mem, NULL);
            unmake(&g_dh[i]);
        }
    check_counts(0);
}

/* A block of the low class whose first allocations rose to the high class, as check_defrag's repair: its index plus
   one, and the count raised */
static u32 repair_setup(u32* raised) {
    DevMemWant tex = {.at = DM_DEVICE, .image = 1, .low = 1, .moves = 1}, mid = {.at = DM_DEVICE, .image = 1, .moves = 1};
    device(0, 1024, 64, 4 * GIB);
    g_vk.has_pageable = 1;
    for (u32 i = 0; i < 80; i++) {
        CHECK(make(1, MIB, 4096, ~0u, 0, i < 40 ? tex : mid, &g_dh[i]) == VK_SUCCESS);
        vk_devmem_priority(g_dh[i].mem, i < 40 ? DM_PRIO_LOW : DM_PRIO_MID);
        vk_devmem_owner(g_dh[i].mem, &g_dh[i]);
    }
    const u32 slot = g_devmem.rec[g_dh[0].mem - 1].block;
    *raised = 0;
    for (u32 i = 1; i < 40; i += 2)
        if (g_devmem.rec[g_dh[i].mem - 1].block == slot) {
            vk_devmem_priority(g_dh[i].mem, DM_PRIO_HIGH);
            ++*raised;
        }
    CHECK(g_devmem.block[slot - 1].low && *raised);
    return slot;
}
static void repair_done(void) {
    for (u32 i = 0; i < 80; i++)
        if (g_dh[i].mem) {
            vk_devmem_owner(g_dh[i].mem, NULL);
            unmake(&g_dh[i]);
        }
}
/* The end of a repair. An allocation of the wrong class with no owner, which no move takes, ends it when the moves
   are done: the block rests 600 presents, and it is not repaired or given up. A request that finds no memory while
   the block is repaired leaves the repair as it is: the block takes requests. A block that its frees empty while it
   is repaired ends the repair */
static void check_repair_end(void) {
    const u64 budget = 4 * GIB;
    u32 raised;
    u32 slot = repair_setup(&raised);
    Held none;
    DevMemWant tex = {.at = DM_DEVICE, .image = 1, .low = 1, .moves = 1};
    CHECK(defrag_round(budget) == (int)raised && g_devmem.drain.slot == slot && g_devmem.drain.repair);
    CHECK(make(1, MIB, 4096, 0, 0, tex, &none) != VK_SUCCESS); /* no memory type: no memory */
    CHECK(g_devmem.drain.slot == slot && g_devmem.drain.repair && !g_devmem.drain.gave_up);
    /* an allocation of the high class that no owner moves comes into the block, as a ring's chunk would */
    CHECK(g_devmem.rec[g_dh[0].mem - 1].block == slot);
    vk_devmem_owner(g_dh[0].mem, NULL);
    vk_devmem_priority(g_dh[0].mem, DM_PRIO_HIGH);
    CHECK(defrag_round(budget) == 0); /* nothing more that moves: the repair ends */
    CHECK(!g_devmem.drain.slot && !g_devmem.drain.repair && !g_devmem.drain.repaired && !g_devmem.drain.gave_up);
    CHECK(misplaced(&g_devmem.block[slot - 1]) && g_devmem.block[slot - 1].drain_skip == g_devmem.frame + 600);
    repair_done();
    check_counts(0);
    /* the frees empty the block under repair */
    slot = repair_setup(&raised);
    CHECK(defrag_round(budget) == (int)raised && g_devmem.drain.slot == slot && g_devmem.drain.repair);
    for (u32 i = 0; i < 80; i++)
        if (g_dh[i].mem && g_devmem.rec[g_dh[i].mem - 1].block == slot) {
            vk_devmem_owner(g_dh[i].mem, NULL);
            unmake(&g_dh[i]);
        }
    CHECK(!g_devmem.drain.slot && !g_devmem.drain.repair);
    repair_done();
    check_counts(0);
}

/* The priority classes and the defrag are the device's: where the driver has the two memory extensions, the renderer
   turns them on (device.c), and where it has not, one class of blocks and no priorities (check_priority). Each
   extension has its one setting, as every extension the renderer can use. No setting empties no block, or lowers the
   GPU's heap below the driver's budget */
static void check_no_settings(void) {
    static const char* const exts[] = {VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME,
                                       VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME};
    static const char* const args[] = {"vk-defrag", "vk-heap-budget-mb"};
    CHECK(g_vk_extension_settings_count > 0 && g_setting_args_count > 0);
    for (u32 e = 0; e < sizeof exts / sizeof exts[0]; e++) {
        u32 found = 0;
        for (int k = 0; k < g_vk_extension_settings_count; k++) found += !strcmp(g_vk_extension_settings[k].name, exts[e]);
        CHECK(found == 1);
    }
    for (int i = 0; i < g_setting_args_count; i++)
        for (u32 a = 0; a < sizeof args / sizeof args[0]; a++) CHECK(strcmp(g_setting_args[i], args[a]));
}

int main(void) {
    vkGetPhysicalDeviceMemoryProperties = memory_properties;
    vkGetPhysicalDeviceMemoryProperties2 = memory_properties2;
    vkMapMemory = map_memory;
    vkCreateBuffer = create_buffer;
    vkGetBufferMemoryRequirements2 = buffer_requirements;
    vkBindBufferMemory = bind_buffer;
    vkDestroyBuffer = destroy_buffer;
    vkCreateImage = create_image;
    vkGetImageMemoryRequirements2 = image_requirements;
    vkBindImageMemory = bind_image;
    vkDestroyImage = destroy_image;
    vkFlushMappedMemoryRanges = mapped_ranges;
    vkInvalidateMappedMemoryRanges = mapped_ranges;
    vkSetDeviceMemoryPriorityEXT = set_priority;
    check_random(0, 1024, 64, 30000); /* buffers and images share the blocks */
    check_random(0, 4096, 64, 30000); /* a granularity over a unit: they have blocks apart */
    check_random(1, 1, 4096, 30000);  /* one heap, and atoms of four units */
    check_alone();
    check_priority();
    check_no_settings();
    check_defrag(0); /* no priority classes: the fallback */
    check_defrag(1);
    check_repair_end();
    check_drain_alone();
    check_drain_room();
    check_kinds();
    check_kind_fallback();
    check_keep();
    check_cap();
    check_atoms(64);
    check_atoms(4096);
    check_threads(); /* the last: the allocator's thread runs from it to the end (check_priority_ahead in it) */
    puts("vk_devmem: ok");
    return 0;
}
