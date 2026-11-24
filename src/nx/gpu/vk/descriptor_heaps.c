/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Existing set/binding decorations map to immutable arrays of byte offsets. Only reachable writes consume index
   words or descriptor encodings. Exact descriptor sizes and separate alignments avoid a worst-case global stride.
   Each frame slot owns a disjoint user range until cmd.c waits for its work and resets its command pool. The
   implementation's reserved range remains identical for every binding and is never touched by the application.
   Sampler bytes are deterministic for identical create infos (VK_EXT_descriptor_heap), so byte interning preserves
   sampler state while it removes duplicate heap copies. All cache comparisons use CPU memory, never mapped GPU memory.
   The descriptors of objects that outlive a frame (views, and the buffers vk_descriptor_heaps_lasting
   names) go into a lasting part after the reserved range instead, kept from frame to frame: only their tables are
   written again (lasting, below). */
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/ledger.h"
#include "nx/gpu/vk/descriptor_heaps.h"
#include "common/utilities/endian.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/tracy.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#define HEAP_SAMPLER_FRAME (128ull << 10)
#define HEAP_RESOURCE_CACHE 4096u
#define HEAP_SAMPLER_CACHE 2048u
#define HEAP_SAMPLER_HANDLES 1024u
#define HEAP_CHAIN_BYTES 1024u

enum { HEAP_UBO, HEAP_SSBO, HEAP_IMAGE, HEAP_STORAGE_IMAGE, HEAP_TEXEL, HEAP_SAMPLER, HEAP_KINDS };
#define LASTING_KINDS          HEAP_SAMPLER /* the resource kinds, each with slots of its own size */
#define LASTING_KEY_BUCKETS    16384u
#define LASTING_OBJECT_BUCKETS 4096u
#define LASTING_QUEUE          1024u /* forgets made without vk_lock, until a lookup takes them */
#define LASTING_PROBES         64u   /* slots a full kind looks at for one that no frame's table names */
typedef struct {
    VkBuffer buffer;
    DevMem memory;
    u8* mapped;
    VkDeviceAddress address;
    VkDeviceSize allocation_bytes;
    VkDescriptorHeapMemoryPlan plan;
    VkDeviceSize used[VK_FRAMES];
} Heap;
typedef VkDescriptorHeapResourceEntry ResourceEntry;
typedef VkDescriptorHeapSamplerEntry SamplerEntry;
typedef struct { VkSampler sampler; u32 offset; } SamplerHandle;
enum { LASTING_FREE, LASTING_LIVE, LASTING_DYING };
/* A slot of the lasting part. Live: in its chains by key and by object, found by lookups. Dying: its object was
   forgotten or its entry taken back, out of both chains, and a table of the frame it was last used in can still name
   its bytes. Free: in its kind's free list */
typedef struct {
    ResourceEntry key; /* heap_offset: the slot's bytes, fixed */
    u64 frame;         /* the last frame whose tables name it (vk_frame_id) */
    u64 submit;        /* the last submission that used it (vk_submit_number) */
    u32 key_next, object_next, next; /* its chain by key, by object, and the free or dying list */
    u8 kind, state;
} Lasting;
typedef struct {
    Lasting* slot; /* VK_DESCRIPTOR_HEAP_LASTING_SLOTS + 1, slot 0 none; one allocation with the buckets and shadow */
    u32 *key_bucket, *object_bucket;
    u8* shadow; /* the bytes written into the lasting part, which draw_state_check=1 compares (never the heap's) */
    size_t cpu_bytes;
    u32 first[LASTING_KINDS], count[LASTING_KINDS], free[LASTING_KINDS], hand[LASTING_KINDS], dying;
    VkDeviceSize start[LASTING_KINDS], stride[LASTING_KINDS]; /* each kind's run, from the lasting part's start */
    /* the buffers vk_descriptor_heaps_lasting named, by handle: open addressing, 0 empty, ~0 a removed one */
    u64* buffers;
    u32 buffer_cap, buffer_used;
    /* the frame of the last write; the submission that every table of the frames before it is in, at most; and the
       one being recorded. A slot that an earlier frame last named is free to use again once the GPU has done `ended` */
    u64 frame, ended, submit;
} LastingState;
/* The forgets of threads without vk_lock. The hook runs before vkDestroy*, so a handle can't come back before its
   forget is queued, and every lookup empties the queue first. Any thread writes it, under its own lock: lines of its
   own */
typedef struct {
    char pad0[64];
    _Atomic char lock;
    int lost; /* the queue was full: the next lookup forgets everything */
    _Atomic u32 n;
    u64 handle[LASTING_QUEUE];
    char pad1[64];
} LastingQueue;
/* A table word's last descriptor: what the write named, the heap offset it found, and the memo's
   generation when it was kept. A buffer is its handle, offset and range; an image its view and layout (in offset);
   a sampler its handle, of type MEMO_SAMPLER */
typedef struct {
    u64 object, offset, range;
    u64 gen;
    u32 type, word;
} DescMemo;
#define MEMO_SAMPLER 0xffffffffu
typedef struct {
    VkDescriptorSetLayout layout;
    u32 offset[SNX_VKB_END], sampler_offset[SNX_VKB_END], count[SNX_VKB_END], words;
    u32 pairs; /* the constant buffer slots whose binding has two words, 8-byte aligned: an index or an address */
    VkDeviceSize full_size;
    VkDescriptorType type[SNX_VKB_END];
    DescMemo* memo; /* one for each word of the table: the stage's last table */
} HeapLayout;
typedef struct {
    Heap resource, sampler;
    HeapLayout layouts[10];
    u32 nlayouts;
    VkDeviceSize sizes[HEAP_KINDS], alignment[HEAP_KINDS];
    struct {
        ResourceEntry resources[HEAP_RESOURCE_CACHE];
        SamplerEntry samplers[HEAP_SAMPLER_CACHE];
        SamplerHandle handles[HEAP_SAMPLER_HANDLES];
    } cache; /* CPU-only metadata, used synchronously by the current frame's writer. */
    VkDescriptorHeapCacheTag cache_frame;
    int exhausted[VK_FRAMES];
    u8 *scratch, *sampler_bytes;
    u32 begun;
    u64 epoch, bound_epoch;
    size_t scratch_size;
    VkDescriptorHeapStats stats, reported;
    int ready, active;
    u32 buffer_begun;
    VkDescriptorSetLayout transition_descriptors;
    VkPipelineLayout transition_layout;
    VkDescriptorPool transition_pool;
    VkDescriptorSet transition_set;
    LastingState lasting;
    _Atomic int lasting_ready; /* read by the forgets of every thread */
    LastingQueue queue;
    u64 memo_gen; /* the generation of the DescMemo entries that hold: a new one empties them all */
} HeapState;
static HeapState g_heaps;
/* Every word of every stage's last table forgotten: at a new frame (the frame's offsets go with its arena), when an
   object is forgotten (its handle can come back for another object), when a slot of the lasting part goes, and when
   the heap has no more space. It starts at 1: an entry of generation 0 holds nothing */
static void memo_clear(void) { g_heaps.memo_gen++; }
static int memo_on(void) { return g_heaps.memo_gen != 0; }
/* word's descriptor as the stage's last table had it, into *word: 1 when that write named the same */
static int memo_get(const HeapLayout* l, u32 at, u64 object, u64 offset, u64 range, u32 type, u32* word) {
    const DescMemo* m = &l->memo[at];
    if (m->gen != g_heaps.memo_gen || m->object != object || m->offset != offset || m->range != range ||
        m->type != type)
        return 0;
    *word = m->word;
    return 1;
}
static void memo_put(const HeapLayout* l, u32 at, u64 object, u64 offset, u64 range, u32 type, u32 word) {
    l->memo[at] = (DescMemo){object, offset, range, g_heaps.memo_gen, type, word};
}
/* draw_state_check=1: a word the memo had (kept) against the one the lookups found again (found) */
static void memo_compare(u32 kept, u32 found, u64 object) {
    g_heaps.stats.memo_checked++;
    if (kept == found) return;
    g_heaps.stats.memo_differs++;
    WARN_LIMIT(32,
               "vulkan: draw_state_check=1: a descriptor kept from the stage's last table (object 0x%llx, heap offset "
               "%u) differs from the one found again (%u)\n",
               (unsigned long long)object, kept, found);
}

static u64 handle_key(VkBuffer buffer) {
#if VK_USE_64_BIT_PTR_DEFINES
    return (u64)(uintptr_t)buffer;
#else
    return (u64)buffer;
#endif
}
static VkDeviceAddress buffer_address(VkBuffer buffer) {
    if (!buffer) return 0;
    VkBufferDeviceAddressInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer};
    g_heaps.stats.address_queries++;
    return vkGetBufferDeviceAddress(g_vk.dev, &info);
}
static int kind(VkDescriptorType type) {
    switch (type) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: return HEAP_UBO;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: return HEAP_SSBO;
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: return HEAP_IMAGE;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: return HEAP_STORAGE_IMAGE;
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return HEAP_TEXEL;
        case VK_DESCRIPTOR_TYPE_SAMPLER: return HEAP_SAMPLER;
        default: return -1;
    }
}
static void heap_discard(Heap* heap) {
    if (heap->buffer) {
        vk_mem_untrack(heap->memory);
        vk_devmem_buffer_destroy(heap->buffer, heap->memory);
    }
    *heap = (Heap){0};
}
/* Heaps refused at setup: what failed, and the graphics backend the device keeps instead (device.c chose buffers
   or sets before it tried heaps) */
[[gnu::format(printf, 1, 2)]] static int refused(const char* fmt, ...);
static int refused(const char* fmt, ...) {
    char why[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    WARN("vulkan: descriptor heaps are not used: %s; graphics descriptors use %s\n", why,
         g_vk.has_descriptor_buffers ? "buffers" : "sets");
    return 0;
}
/* usage: what the heap's buffer is used as besides a heap. The resource heap holds the index tables that push data
   names, which a shader reads as a uniform buffer: the buffer whose address push data has must have
   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT (VUID-vkCmdDraw-None-11437) */
static int heap_create(Heap* heap, const VkDescriptorHeapMemoryPlan* plan, const char* name, VkBufferUsageFlags usage) {
    VkBufferCreateInfo create = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = plan->allocation_size,
        .usage = VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    DevMemWant allocation = {.at = DM_HOST,
                             .mapped = 1,
                             .need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    DevMemInfo details;
    if (!VK_OK(vk_devmem_buffer(&create, &allocation, &heap->buffer, &heap->memory, &details)))
        return refused("the %s heap's %llu-byte buffer could not be made", name,
                       (unsigned long long)plan->allocation_size);
    vk_mem_track(heap->memory, VK_OWN_DESCRIPTORS);
    VkDeviceAddress original = buffer_address(heap->buffer);
    VkDeviceSize padding = snx_align_up(original, plan->alignment) - original;
    if (!original || !details.ptr || padding > plan->allocation_size - plan->range_size) {
        heap_discard(heap);
        return refused("the %s heap's buffer is at address 0x%llx, %s, and needs %llu bytes of padding to its "
                       "%llu-byte alignment, %llu at most",
                       name, (unsigned long long)original, details.ptr ? "mapped" : "not mapped",
                       (unsigned long long)padding, (unsigned long long)plan->alignment,
                       (unsigned long long)(plan->allocation_size - plan->range_size));
    }
    heap->address = original + padding;
    heap->mapped = (u8*)details.ptr + padding;
    heap->plan = *plan;
    heap->allocation_bytes = details.size;
    return 1;
}
static void transition_destroy(void) {
    if (g_heaps.transition_pool) vkDestroyDescriptorPool(g_vk.dev, g_heaps.transition_pool, NULL);
    if (g_heaps.transition_layout) vkDestroyPipelineLayout(g_vk.dev, g_heaps.transition_layout, NULL);
    if (g_heaps.transition_descriptors) vkDestroyDescriptorSetLayout(g_vk.dev, g_heaps.transition_descriptors, NULL);
    g_heaps.transition_set = VK_NULL_HANDLE;
    g_heaps.transition_pool = VK_NULL_HANDLE;
    g_heaps.transition_layout = VK_NULL_HANDLE;
    g_heaps.transition_descriptors = VK_NULL_HANDLE;
}
/* Each failed call is logged with its VkResult by VK_OK */
static int transition_create(void) {
    if (!g_vk.has_descriptor_buffers) return 1;
    VkDescriptorSetLayoutCreateInfo descriptors = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkDescriptorSetLayout made_descriptors = VK_NULL_HANDLE;
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &descriptors, NULL, &made_descriptors)))
        return refused("the empty descriptor set that leaves descriptor buffers: vkCreateDescriptorSetLayout failed");
    g_heaps.transition_descriptors = made_descriptors;
    VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &g_heaps.transition_descriptors};
    VkPipelineLayout made_layout = VK_NULL_HANDLE;
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &layout, NULL, &made_layout)))
        return refused("the empty descriptor set that leaves descriptor buffers: vkCreatePipelineLayout failed");
    g_heaps.transition_layout = made_layout;
    VkDescriptorPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1};
    VkDescriptorPool made_pool = VK_NULL_HANDLE;
    if (!VK_OK(vkCreateDescriptorPool(g_vk.dev, &pool, NULL, &made_pool)))
        return refused("the empty descriptor set that leaves descriptor buffers: vkCreateDescriptorPool failed");
    g_heaps.transition_pool = made_pool;
    VkDescriptorSetAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = g_heaps.transition_pool, .descriptorSetCount = 1,
        .pSetLayouts = &g_heaps.transition_descriptors};
    VkDescriptorSet made_set = VK_NULL_HANDLE;
    if (!VK_OK(vkAllocateDescriptorSets(g_vk.dev, &allocate, &made_set)))
        return refused("the empty descriptor set that leaves descriptor buffers: vkAllocateDescriptorSets failed");
    g_heaps.transition_set = made_set;
    return 1;
}

/* ---------------------------------------------------------------- the lasting part
   A view's or a named buffer's descriptor bytes depend on the object and the key alone (address and range, view and
   layout), never on the bytes the object holds, and neither changes while the object lives: they are made once, into
   a slot of their kind, and a later frame's table names the same slot. What ends that is the object's destroy, whose
   hook (vk_descriptor_heaps_forget_*) takes the entries out at once, and the slot's bytes are written again only when
   no table can name them: tables live in their frame's slot, and draws take a set again only in its frame (render.c's
   desc_set_cached, g_dset), so a slot last named in an earlier frame is free once the GPU has done that frame's
   submissions. A staging or stream range is never kept: its buffer is no named one */
static int lasting_on(void) { return g_heaps.lasting.slot != NULL; }
static u32 lasting_mix(u64 h) { return (u32)((h * 0x9e3779b97f4a7c15ull) >> 32); }
static void lasting_init(void) {
    LastingState* L = &g_heaps.lasting;
    const VkDescriptorHeapMemoryPlan* plan = &g_heaps.resource.plan;
    if (!plan->lasting_size) {
        LOG("vulkan: descriptor heaps: no lasting part fits: descriptors are made again in each frame\n");
        return;
    }
    size_t slots = (size_t)VK_DESCRIPTOR_HEAP_LASTING_SLOTS + 1;
    size_t keys = sizeof(u32) * LASTING_KEY_BUCKETS, objects = sizeof(u32) * LASTING_OBJECT_BUCKETS;
    size_t bytes = sizeof(Lasting) * slots + keys + objects + (size_t)plan->lasting_size;
    u8* block = calloc(1, bytes); /* one allocation: the slots, the two bucket arrays, the shadow */
    if (!block) {
        LOG("vulkan: descriptor heaps: no memory for the lasting part: descriptors are made again in each frame\n");
        return;
    }
    L->slot = (Lasting*)(void*)block;
    L->key_bucket = (u32*)(void*)(block + sizeof(Lasting) * slots);
    L->object_bucket = (u32*)(void*)(block + sizeof(Lasting) * slots + keys);
    L->shadow = block + sizeof(Lasting) * slots + keys + objects;
    L->cpu_bytes = bytes;
    for (u32 k = 0; k < LASTING_KINDS; k++) {
        L->free[k] = L->count[k] ? L->first[k] : 0;
        for (u32 i = 0; i < L->count[k]; i++) {
            Lasting* e = &L->slot[L->first[k] + i];
            e->kind = (u8)k;
            e->next = i + 1 < L->count[k] ? L->first[k] + i + 1 : 0;
            e->key.heap_offset = (u32)(plan->lasting_offset + L->start[k] + L->stride[k] * i);
        }
    }
    g_heaps.stats.lasting_bytes = plan->lasting_size;
    atomic_store_explicit(&g_heaps.lasting_ready, 1, memory_order_release);
}
static void lasting_destroy(void) {
    atomic_store_explicit(&g_heaps.lasting_ready, 0, memory_order_release);
    free(g_heaps.lasting.slot);
    free(g_heaps.lasting.buffers);
}
/* the buffers named, by handle */
static int lasting_named(u64 object) {
    const LastingState* L = &g_heaps.lasting;
    if (!L->buffer_cap) return 0;
    for (u32 i = lasting_mix(object) & (L->buffer_cap - 1);; i = (i + 1) & (L->buffer_cap - 1)) {
        if (L->buffers[i] == object) return 1;
        if (!L->buffers[i]) return 0;
    }
}
static void lasting_name(u64 object) {
    LastingState* L = &g_heaps.lasting;
    if (!object || object == ~0ull || lasting_named(object)) return;
    if ((L->buffer_used + 1) * 2 > L->buffer_cap) { /* grown, or the removed ones dropped, at half full */
        u32 cap = L->buffer_cap ? L->buffer_cap : 256;
        u32 live = 0;
        for (u32 i = 0; i < L->buffer_cap; i++) live += L->buffers[i] && L->buffers[i] != ~0ull;
        while ((live + 1) * 4 > cap) cap *= 2;
        u64* fresh = calloc(cap, sizeof *fresh);
        if (!fresh) return; /* not named: its descriptors are made in each frame, as without the lasting part */
        for (u32 i = 0; i < L->buffer_cap; i++) {
            u64 h = L->buffers[i];
            if (!h || h == ~0ull) continue;
            u32 j = lasting_mix(h) & (cap - 1);
            while (fresh[j]) j = (j + 1) & (cap - 1);
            fresh[j] = h;
        }
        free(L->buffers);
        L->buffers = fresh;
        L->buffer_cap = cap;
        L->buffer_used = live;
    }
    u32 i = lasting_mix(object) & (L->buffer_cap - 1);
    while (L->buffers[i] && L->buffers[i] != ~0ull) i = (i + 1) & (L->buffer_cap - 1);
    L->buffer_used += !L->buffers[i];
    L->buffers[i] = object;
}
static void lasting_unname(u64 object) {
    LastingState* L = &g_heaps.lasting;
    if (!L->buffer_cap) return;
    for (u32 i = lasting_mix(object) & (L->buffer_cap - 1); L->buffers[i]; i = (i + 1) & (L->buffer_cap - 1))
        if (L->buffers[i] == object) {
            L->buffers[i] = ~0ull;
            return;
        }
}
static u32 lasting_key_bucket(u64 hash) { return (u32)hash & (LASTING_KEY_BUCKETS - 1); }
static u32 lasting_object_bucket(u64 object) { return lasting_mix(object) & (LASTING_OBJECT_BUCKETS - 1); }
static u32 lasting_find(const ResourceEntry* key, u64 hash) {
    const LastingState* L = &g_heaps.lasting;
    for (u32 s = L->key_bucket[lasting_key_bucket(hash)]; s; s = L->slot[s].key_next)
        if (vk_descriptor_heap_resource_equal(&L->slot[s].key, key)) return s;
    return 0;
}
/* slot s out of its chain by key, then on the dying list (its chain by object is the caller's) */
static void lasting_die(u32 s) {
    LastingState* L = &g_heaps.lasting;
    Lasting* e = &L->slot[s];
    u32* link = &L->key_bucket[lasting_key_bucket(vk_hash(&e->key, VK_DESCRIPTOR_HEAP_RESOURCE_KEY_BYTES))];
    while (*link && *link != s) link = &L->slot[*link].key_next;
    if (*link) *link = e->key_next;
    e->key_next = e->object_next = 0;
    e->state = LASTING_DYING;
    e->next = L->dying;
    L->dying = s;
}
static void lasting_drop(u32 s) {
    LastingState* L = &g_heaps.lasting;
    u32* link = &L->object_bucket[lasting_object_bucket(L->slot[s].key.object)];
    while (*link && *link != s) link = &L->slot[*link].object_next;
    if (*link) *link = L->slot[s].object_next;
    lasting_die(s);
}
/* no table can name slot e's bytes: the frame that last named it is over, and the GPU has done its submissions */
static int lasting_idle(const Lasting* e, u64 done) {
    const LastingState* L = &g_heaps.lasting;
    return e->frame < L->frame && L->ended <= done && e->submit <= done;
}
static u64 lasting_done(void) {
    u64 submitted, done;
    vk_submit_progress(&submitted, &done);
    return done;
}
/* the dying slots that no table can name, free again. None can be while the GPU has not done L->ended, which every
   slot's lasting_idle needs: at a new frame L->ended is the last submission, which the GPU has almost never done
   yet, and the walk over every slot waiting would free none */
static void lasting_reap(u64 done) {
    LastingState* L = &g_heaps.lasting;
    if (L->ended > done) {
        g_heaps.stats.lasting_reaps_skipped++;
        SNX_ZONE_T(z, "vk descriptor reap", "skipped: the GPU has not done the last submit");
        return;
    }
    SNX_ZONE(z, "vk descriptor reap");
    u64 freed = g_heaps.stats.lasting_freed;
    [[maybe_unused]] u64 looked = g_heaps.stats.lasting_reap_looked;
    g_heaps.stats.lasting_reaps++;
    for (u32* link = &L->dying; *link;) {
        Lasting* e = &L->slot[*link];
        g_heaps.stats.lasting_reap_looked++;
        if (!lasting_idle(e, done)) {
            link = &e->next;
            continue;
        }
        u32 s = *link;
        *link = e->next;
        e->state = LASTING_FREE;
        e->next = L->free[e->kind];
        L->free[e->kind] = s;
        g_heaps.stats.lasting_freed++;
    }
    SNX_ZONE_TEXT(z, "done: %llu slots freed of %llu looked at",
                  (unsigned long long)(g_heaps.stats.lasting_freed - freed),
                  (unsigned long long)(g_heaps.stats.lasting_reap_looked - looked));
    SNX_ZONE_VALUE(z, g_heaps.stats.lasting_freed - freed);
    SNX_ZONE_END(z);
    SNX_PLOT("vk pipe desc reaped", (double)(g_heaps.stats.lasting_freed - freed));
}
/* a free slot of kind k, 0 for none: one that no table names any more is taken back from its object */
static u32 lasting_take(int k) {
    LastingState* L = &g_heaps.lasting;
    u64 done = 0;
    int asked = 0;
    if (!L->free[k] && L->dying) {
        done = lasting_done();
        asked = 1;
        lasting_reap(done);
    }
    if (!L->free[k]) {
        if (!asked) done = lasting_done();
        for (u32 i = 0; i < LASTING_PROBES && i < L->count[k]; i++) {
            u32 s = L->first[k] + L->hand[k];
            L->hand[k] = L->hand[k] + 1 == L->count[k] ? 0 : L->hand[k] + 1;
            Lasting* e = &L->slot[s];
            if (e->state != LASTING_LIVE || !lasting_idle(e, done)) continue;
            lasting_drop(s);
            g_heaps.stats.lasting_evicted++;
            lasting_reap(done);
            break;
        }
    }
    u32 s = L->free[k];
    if (s) L->free[k] = L->slot[s].next;
    return s;
}
/* every live entry of object out, at once, and the buffer no longer named. The frame cache's entries too, while they
   are of the frame being recorded: a destroy in the middle of a frame (a guest memory import's, uma.c imp_kill) lets
   a new object of the same handle come in the same frame */
static void lasting_forget_now(u64 object) {
    LastingState* L = &g_heaps.lasting;
    u32* link = &L->object_bucket[lasting_object_bucket(object)];
    while (*link) {
        u32 s = *link;
        Lasting* e = &L->slot[s];
        if (e->key.object != object) {
            link = &e->object_next;
            continue;
        }
        *link = e->object_next;
        lasting_die(s);
        g_heaps.stats.lasting_forgotten++;
    }
    lasting_unname(object);
    memo_clear(); /* a new object can have the handle now */
    if (g_heaps.cache_frame.valid && g_heaps.cache_frame.frame == vk_frame_id())
        for (u32 i = 0; i < HEAP_RESOURCE_CACHE; i++)
            if (g_heaps.cache.resources[i].object == object) g_heaps.cache.resources[i] = (ResourceEntry){0};
}
static void lasting_forget_all(void) {
    LastingState* L = &g_heaps.lasting;
    for (u32 s = 1; s <= VK_DESCRIPTOR_HEAP_LASTING_SLOTS; s++)
        if (L->slot[s].state == LASTING_LIVE) {
            lasting_die(s);
            g_heaps.stats.lasting_forgotten++;
        }
    memset(L->object_bucket, 0, sizeof(u32) * LASTING_OBJECT_BUCKETS);
    if (L->buffers) memset(L->buffers, 0, sizeof *L->buffers * L->buffer_cap);
    L->buffer_used = 0;
    vk_descriptor_heap_cache_reset(&g_heaps.cache_frame);
    memo_clear();
}
static void queue_lock(void) {
    while (atomic_exchange_explicit(&g_heaps.queue.lock, 1, memory_order_acquire)) {}
}
static void queue_unlock(void) { atomic_store_explicit(&g_heaps.queue.lock, 0, memory_order_release); }
/* the forgets other threads queued, done (vk_lock held) */
static void lasting_drain(void) {
    if (!atomic_load_explicit(&g_heaps.queue.n, memory_order_acquire)) return;
    u64 handles[LASTING_QUEUE];
    queue_lock();
    u32 n = atomic_load_explicit(&g_heaps.queue.n, memory_order_relaxed);
    int lost = g_heaps.queue.lost;
    memcpy(handles, g_heaps.queue.handle, sizeof *handles * n);
    g_heaps.queue.lost = 0;
    atomic_store_explicit(&g_heaps.queue.n, 0, memory_order_release);
    queue_unlock();
    if (lost) {
        lasting_forget_all();
        return;
    }
    for (u32 i = 0; i < n; i++) lasting_forget_now(handles[i]);
}
static void lasting_forget(u64 object) {
    if (!object || !atomic_load_explicit(&g_heaps.lasting_ready, memory_order_acquire)) return;
    if (vk_lock_held()) { /* vk_lock orders it with every lookup: they run under it */
        lasting_drain();
        lasting_forget_now(object);
        return;
    }
    queue_lock();
    u32 n = atomic_load_explicit(&g_heaps.queue.n, memory_order_relaxed);
    if (n < LASTING_QUEUE) {
        g_heaps.queue.handle[n] = object;
        atomic_store_explicit(&g_heaps.queue.n, n + 1, memory_order_release);
    } else
        g_heaps.queue.lost = 1;
    queue_unlock();
}
/* the forgets so far, of any mode's objects: render.c's descriptor sets of the frame name the objects by handle too */
static _Atomic u64 g_forgets;
u64 vk_descriptor_heaps_forgets(void) { return atomic_load_explicit(&g_forgets, memory_order_acquire); }
void vk_descriptor_heaps_forget_buffer(VkBuffer buffer) {
    atomic_fetch_add_explicit(&g_forgets, 1, memory_order_release);
    lasting_forget(handle_key(buffer));
}
void vk_descriptor_heaps_forget_view(VkImageView view) {
    atomic_fetch_add_explicit(&g_forgets, 1, memory_order_release);
#if VK_USE_64_BIT_PTR_DEFINES
    lasting_forget((u64)(uintptr_t)view);
#else
    lasting_forget((u64)view);
#endif
}
/* without vk_lock the buffer is not named: its descriptors are then made in each frame */
void vk_descriptor_heaps_lasting(VkBuffer buffer) {
    if (!buffer || !atomic_load_explicit(&g_heaps.lasting_ready, memory_order_acquire) || !vk_lock_held()) return;
    lasting_drain();
    lasting_name(handle_key(buffer));
}
/* a write's start: the queued forgets done; at a new frame, the last submission every earlier frame's tables can be
   in (vk_frame_next submitted what the frame recorded) */
static void lasting_begin(void) {
    LastingState* L = &g_heaps.lasting;
    if (!L->slot) return;
    lasting_drain();
    L->submit = vk_submit_number();
    u64 frame = vk_frame_id();
    if (L->frame == frame) return;
    L->frame = frame;
    L->ended = vk_last_submit();
    if (L->dying) lasting_reap(lasting_done());
}
static int lasting_hit(u32 s, int is_buffer, u32* offset) {
    Lasting* e = &g_heaps.lasting.slot[s];
    e->frame = g_heaps.lasting.frame;
    e->submit = g_heaps.lasting.submit;
    *offset = e->key.heap_offset;
    g_heaps.stats.lasting_hits++;
    if (is_buffer) g_heaps.stats.buffer_hits++;
    else g_heaps.stats.image_hits++;
    return 1;
}
/* draw_state_check=1 found slot s wrong: out, and its descriptor made again */
static void lasting_differs(u32 s, int k) {
    g_heaps.stats.lasting_differs++;
    WARN_LIMIT(32,
               "vulkan: draw_state_check=1: a kept descriptor of the lasting part (object 0x%llx, kind %d) differs "
               "from the one made again; it is made again\n",
               (unsigned long long)g_heaps.lasting.slot[s].key.object, k);
    lasting_drop(s);
    memo_clear(); /* a word may name its slot */
}
/* draw_state_check=1: slot s's bytes against those just made into scratch */
static int lasting_same(u32 s, int k) {
    const LastingState* L = &g_heaps.lasting;
    g_heaps.stats.lasting_checked++;
    if (!memcmp(L->shadow + (L->slot[s].key.heap_offset - g_heaps.resource.plan.lasting_offset), g_heaps.scratch,
                (size_t)g_heaps.sizes[k])) return 1;
    lasting_differs(s, k);
    return 0;
}
/* the descriptor in scratch into a slot of kind k for key: 0 when no slot is free */
static int lasting_place(int k, const ResourceEntry* key, u64 hash, u32* offset) {
    LastingState* L = &g_heaps.lasting;
    u32 s = lasting_take(k);
    if (!s) {
        g_heaps.stats.lasting_full++;
        return 0;
    }
    Lasting* e = &L->slot[s];
    u32 at = e->key.heap_offset;
    e->key = *key;
    e->key.heap_offset = at;
    e->frame = L->frame;
    e->submit = L->submit;
    e->state = LASTING_LIVE;
    u32* keys = &L->key_bucket[lasting_key_bucket(hash)];
    e->key_next = *keys;
    *keys = s;
    u32* objects = &L->object_bucket[lasting_object_bucket(key->object)];
    e->object_next = *objects;
    *objects = s;
    e->next = 0;
    /* the GPU reads no table that names the slot (lasting_idle): its bytes change under no reader */
    memcpy(g_heaps.resource.mapped + at, g_heaps.scratch, (size_t)g_heaps.sizes[k]);
    memcpy(L->shadow + (at - g_heaps.resource.plan.lasting_offset), g_heaps.scratch, (size_t)g_heaps.sizes[k]);
    g_heaps.stats.lasting_made++;
    *offset = at;
    return 1;
}

int vk_descriptor_heaps_init(const VkPhysicalDeviceDescriptorHeapPropertiesEXT* props) {
    if (g_heaps.ready) return 1;
    if (!props) return refused("the device gave no descriptor heap properties");
    const char* missing = !vkWriteResourceDescriptorsEXT          ? "vkWriteResourceDescriptorsEXT"
                          : !vkWriteSamplerDescriptorsEXT         ? "vkWriteSamplerDescriptorsEXT"
                          : !vkGetPhysicalDeviceDescriptorSizeEXT ? "vkGetPhysicalDeviceDescriptorSizeEXT"
                          : !vkCmdBindResourceHeapEXT             ? "vkCmdBindResourceHeapEXT"
                          : !vkCmdBindSamplerHeapEXT              ? "vkCmdBindSamplerHeapEXT"
                          : !vkCmdPushDataEXT                     ? "vkCmdPushDataEXT"
                                                                  : NULL;
    if (missing) return refused("the driver has no %s", missing);
    if (props->maxPushDataSize < VK_DESCRIPTOR_HEAP_PUSH_SIZE)
        return refused("maxPushDataSize is %u bytes, %u needed", (unsigned)props->maxPushDataSize,
                       (unsigned)VK_DESCRIPTOR_HEAP_PUSH_SIZE);
    static const VkDescriptorType types[HEAP_KINDS] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLER};
    static const char* const names[HEAP_KINDS] = {"uniform buffer", "storage buffer",       "sampled image",
                                                  "storage image",  "storage texel buffer", "sampler"};
    VkDeviceSize largest = 0;
    for (u32 i = 0; i < HEAP_KINDS; i++) {
        g_heaps.sizes[i] = vkGetPhysicalDeviceDescriptorSizeEXT(g_vk.phys, types[i]);
        g_heaps.alignment[i] = vk_descriptor_heap_descriptor_alignment(props, types[i]);
        if (!g_heaps.sizes[i] || !g_heaps.alignment[i] || g_heaps.sizes[i] > UINT32_MAX ||
            g_heaps.alignment[i] > UINT32_MAX || (g_heaps.alignment[i] & (g_heaps.alignment[i] - 1)))
            return refused("a %s descriptor is %llu bytes aligned to %llu: a size and a power of two of 1 to "
                           "0xffffffff are needed",
                           names[i], (unsigned long long)g_heaps.sizes[i], (unsigned long long)g_heaps.alignment[i]);
        if (g_heaps.sizes[i] > largest) largest = g_heaps.sizes[i];
    }
    VkDeviceSize resource_alignment = props->imageDescriptorAlignment > props->bufferDescriptorAlignment
                                           ? props->imageDescriptorAlignment : props->bufferDescriptorAlignment;
    if (resource_alignment < 8) resource_alignment = 8;
    VkDeviceSize resource_address_alignment = props->resourceHeapAlignment;
    if (resource_address_alignment < 8) resource_address_alignment = 8; /* Indirect table addresses are8-byte aligned. */
    if (resource_address_alignment < resource_alignment) resource_address_alignment = resource_alignment;
    VkDeviceSize sampler_address_alignment = props->samplerHeapAlignment;
    if (sampler_address_alignment < props->samplerDescriptorAlignment)
        sampler_address_alignment = props->samplerDescriptorAlignment;
    if (!props->resourceHeapAlignment || !props->samplerHeapAlignment ||
        (props->resourceHeapAlignment & (props->resourceHeapAlignment - 1)) ||
        (props->samplerHeapAlignment & (props->samplerHeapAlignment - 1)))
        return refused("resourceHeapAlignment is %llu and samplerHeapAlignment %llu: powers of two are needed",
                       (unsigned long long)props->resourceHeapAlignment,
                       (unsigned long long)props->samplerHeapAlignment);
    VkDescriptorHeapMemoryPlan resources, samplers;
    VkDeviceSize sampler_frame = 5ull * (SH_MAXTEX + 2 * SNX_TEX_ARRAY) *
                                 snx_align_up(g_heaps.sizes[HEAP_SAMPLER], props->samplerDescriptorAlignment);
    if (sampler_frame < HEAP_SAMPLER_FRAME) sampler_frame = HEAP_SAMPLER_FRAME;
    /* the lasting part: each kind's slots in a run, at the kind's alignment */
    static const u32 lasting_slots[LASTING_KINDS] = {VK_DESCRIPTOR_HEAP_LASTING_UBO, VK_DESCRIPTOR_HEAP_LASTING_SSBO,
        VK_DESCRIPTOR_HEAP_LASTING_IMAGE, VK_DESCRIPTOR_HEAP_LASTING_STORAGE_IMAGE, VK_DESCRIPTOR_HEAP_LASTING_TEXEL};
    LastingState* L = &g_heaps.lasting;
    VkDeviceSize lasting = 0;
    for (u32 k = 0, first = 1; k < LASTING_KINDS; k++) {
        L->stride[k] = snx_align_up(g_heaps.sizes[k], g_heaps.alignment[k]);
        L->start[k] = lasting = snx_align_up(lasting, g_heaps.alignment[k]);
        L->first[k] = first;
        L->count[k] = lasting_slots[k];
        first += lasting_slots[k];
        lasting += L->stride[k] * lasting_slots[k];
    }
    lasting = snx_align_up(lasting, resource_alignment);
    if (!vk_descriptor_heap_memory_plan_lasting_fit(&resources, VK_DESCRIPTOR_HEAP_RESOURCE_FRAME, VK_FRAMES, lasting,
                                            props->minResourceHeapReservedRange, resource_address_alignment,
                                            resource_alignment, props->maxResourceHeapSize, g_vk.alloc_cap) &&
        !vk_descriptor_heap_memory_plan_fit(&resources, VK_DESCRIPTOR_HEAP_RESOURCE_FRAME, VK_FRAMES,
                                            props->minResourceHeapReservedRange, resource_address_alignment,
                                            resource_alignment, props->maxResourceHeapSize, g_vk.alloc_cap))
        return refused("no resource heap of %u frames fits: %llu bytes reserved, maxResourceHeapSize %llu, "
                       "allocations of %llu bytes at most",
                       (unsigned)VK_FRAMES, (unsigned long long)props->minResourceHeapReservedRange,
                       (unsigned long long)props->maxResourceHeapSize, (unsigned long long)g_vk.alloc_cap);
    if (!vk_descriptor_heap_memory_plan_fit(&samplers, sampler_frame, VK_FRAMES, props->minSamplerHeapReservedRange,
                                            sampler_address_alignment, props->samplerDescriptorAlignment,
                                            props->maxSamplerHeapSize, g_vk.alloc_cap))
        return refused("no sampler heap of %u frames fits: %llu bytes reserved, maxSamplerHeapSize %llu, "
                       "allocations of %llu bytes at most",
                       (unsigned)VK_FRAMES, (unsigned long long)props->minSamplerHeapReservedRange,
                       (unsigned long long)props->maxSamplerHeapSize, (unsigned long long)g_vk.alloc_cap);
    VkDeviceSize buffer_stride = g_heaps.sizes[HEAP_UBO] > g_heaps.sizes[HEAP_SSBO]
        ? g_heaps.sizes[HEAP_UBO] : g_heaps.sizes[HEAP_SSBO];
    if (buffer_stride > SIZE_MAX / VK_DESCRIPTOR_HEAP_BUFFER_BATCH)
        return refused("a buffer descriptor is %llu bytes", (unsigned long long)buffer_stride);
    VkDeviceSize encoding_bytes = buffer_stride * VK_DESCRIPTOR_HEAP_BUFFER_BATCH;
    if (encoding_bytes < largest) encoding_bytes = largest;
    if (resources.frame_size < 8 + g_heaps.sizes[HEAP_UBO] || samplers.frame_size < g_heaps.sizes[HEAP_SAMPLER] ||
        encoding_bytes > SIZE_MAX - samplers.frame_size)
        return refused("a frame takes %llu resource bytes, %llu needed, and %llu sampler bytes, %llu needed",
                       (unsigned long long)resources.frame_size, (unsigned long long)(8 + g_heaps.sizes[HEAP_UBO]),
                       (unsigned long long)samplers.frame_size, (unsigned long long)g_heaps.sizes[HEAP_SAMPLER]);
    /* One CPU allocation owns temporary encoding bytes and the sampler comparison shadow. */
    g_heaps.scratch = calloc(1, (size_t)(encoding_bytes + samplers.frame_size));
    if (!g_heaps.scratch)
        return refused("%llu bytes of encoding scratch could not be allocated",
                       (unsigned long long)(encoding_bytes + samplers.frame_size));
    g_heaps.scratch_size = (size_t)(encoding_bytes + samplers.frame_size);
    g_heaps.sampler_bytes = g_heaps.scratch + encoding_bytes;
    if (!heap_create(&g_heaps.resource, &resources, "resource", VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
        !heap_create(&g_heaps.sampler, &samplers, "sampler", 0) || !transition_create()) {
        transition_destroy();
        heap_discard(&g_heaps.resource);
        heap_discard(&g_heaps.sampler);
        free(g_heaps.scratch);
        g_heaps.scratch = g_heaps.sampler_bytes = NULL;
        g_heaps.scratch_size = 0;
        return 0;
    }
    g_heaps.ready = 1;
    g_heaps.memo_gen = 1;
    lasting_init();
    LOG("vulkan: descriptor heaps: %llu resource bytes + %llu reserved + %llu lasting, %llu sampler bytes + %llu "
        "reserved\n",
        (unsigned long long)resources.reserved_offset, (unsigned long long)resources.reserved_size,
        (unsigned long long)(L->slot ? resources.lasting_size : 0), (unsigned long long)samplers.reserved_offset,
        (unsigned long long)samplers.reserved_size);
    return 1;
}
static HeapLayout* find_layout(VkDescriptorSetLayout layout) {
    for (u32 i = 0; i < g_heaps.nlayouts; i++)
        if (g_heaps.layouts[i].layout == layout) return &g_heaps.layouts[i];
    return NULL;
}
void vk_descriptor_heaps_layout(VkDescriptorSetLayout layout, const VkDescriptorSetLayoutBinding* bindings, u32 count) {
    if (!g_heaps.ready) return;
    if (find_layout(layout)) return;
    if (g_heaps.nlayouts == 10) fatal("vulkan: too many descriptor heap layouts");
    HeapLayout* l = &g_heaps.layouts[g_heaps.nlayouts++];
    l->layout = layout;
    for (u32 i = 0; i < count; i++) {
        const VkDescriptorSetLayoutBinding* b = &bindings[i];
        int k = kind(b->descriptorType);
        if (b->binding >= SNX_VKB_END || !b->descriptorCount || k < 0 || l->count[b->binding] ||
            b->descriptorCount > UINT32_MAX - l->words - 2) fatal("vulkan: invalid descriptor heap binding");
        /* a constant buffer: two words at an even word, which hold its heap index (the first) or the address that
           vk_descriptor_heap_address_mapping reads (both), as the pipeline maps it */
        const int pair = b->binding >= SNX_VKB_CB && b->binding < SNX_VKB_CB + 18 && b->descriptorCount == 1 &&
                         b->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        if (pair) {
            l->words += l->words & 1;
            l->pairs |= 1u << (b->binding - SNX_VKB_CB);
        }
        if (!vk_descriptor_heap_table_append(&l->words, pair ? 2 : b->descriptorCount,
                b->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                &l->offset[b->binding], &l->sampler_offset[b->binding])) fatal("vulkan: descriptor heap table overflow");
        l->count[b->binding] = b->descriptorCount;
        l->type[b->binding] = b->descriptorType;
        l->full_size += (u64)b->descriptorCount * snx_align_up(g_heaps.sizes[k], g_heaps.alignment[k]);
        if (b->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
            l->full_size += (u64)b->descriptorCount *
                snx_align_up(g_heaps.sizes[HEAP_SAMPLER], g_heaps.alignment[HEAP_SAMPLER]);

    }
    if (l->words > UINT32_MAX / 4) fatal("vulkan: descriptor heap table is too large");
    l->full_size += 4ull * l->words;
    l->memo = calloc(l->words ? l->words : 1, sizeof *l->memo);
    if (!l->memo) fatal("vulkan: no memory for a descriptor heap layout's last table");
}
/* whether write w is of a constant buffer slot in cb_addr whose two words take its address */
static int by_address(const HeapLayout* l, const VkWriteDescriptorSet* w, u32 cb_addr) {
    u32 slot = w->dstBinding - SNX_VKB_CB;
    return w->dstBinding >= SNX_VKB_CB && slot < 18 && (cb_addr & l->pairs) >> slot & 1;
}
uint32_t vk_descriptor_heaps_mapping(VkDescriptorSetLayout layout, u32 set, VkDescriptorSetAndBindingMappingEXT* maps,
                                      u32 capacity, u32 cb_addr) {
    HeapLayout* l = find_layout(layout);
    if (!l || !maps || set >= 5) return 0;
    u32 count = 0;
    for (u32 binding = 0; binding < SNX_VKB_END; binding++) {
        if (!l->count[binding]) continue;
        if (count == capacity) return 0;
        const VkWriteDescriptorSet w = {.dstBinding = binding};
        if (by_address(l, &w, cb_addr) ? !vk_descriptor_heap_address_mapping(&maps[count], set, binding, l->offset[binding])
                                       : !vk_descriptor_heap_binding_mapping(&maps[count], set, binding, l->offset[binding],
                                                                             l->sampler_offset[binding], l->type[binding]))
            return 0;
        count++;
    }
    return count;
}
static int allocate(Heap* heap, u32 slot, VkDeviceSize size, VkDeviceSize alignment, u32* offset) {
    VkDeviceSize before = heap->used[slot];
    if (vk_descriptor_heap_allocate(&heap->plan, &heap->used[slot], slot, size, alignment, offset)) {
        g_heaps.stats.arena_bytes += heap->used[slot] - before;
        u64 live = g_heaps.resource.used[slot] + g_heaps.sampler.used[slot];
        if (live > g_heaps.stats.peak_used_bytes) g_heaps.stats.peak_used_bytes = live;
        return 1;
    }
    g_heaps.exhausted[slot] = 1;
    memo_clear();
    return 0;
}
static ResourceEntry* resource_entry(const ResourceEntry* key, int* hit) {
    u64 hash = vk_hash(key, VK_DESCRIPTOR_HEAP_RESOURCE_KEY_BYTES);
    u32 index = ((u32)hash & (HEAP_RESOURCE_CACHE / 4 - 1)) * 4;
    ResourceEntry* replacement = &g_heaps.cache.resources[index + (u32)(hash >> 32) % 4];
    for (u32 i = 0; i < 4; i++) {
        ResourceEntry* e = &g_heaps.cache.resources[index + i];
        if (!e->object) { *hit = 0; SNX_PLOT("vk pipe desc frame cache hit per lookup", 0); return e; }
        if (vk_descriptor_heap_resource_equal(e, key)) { *hit = 1; SNX_PLOT("vk pipe desc frame cache hit per lookup", 1); return e; }
    }
    /* Replace only CPU metadata. Its former immutable descriptor bytes remain alive in this frame. */
    *hit = 0;
    SNX_PLOT("vk pipe desc frame cache hit per lookup", 0);
    return replacement;
}
/* The lasting lookup's outcome, plotted once the decision is made, not when the slot is found. draw_state_check=1
   can find one and still make its descriptor again, so the finding alone is not the hit. */
static inline int lasting_report(int lasting, int was_hit, int ret) {
    if (lasting) SNX_PLOT("vk pipe desc lasting lookup", (double)was_hit);
    return ret;
}
/* A buffer range's or a view's descriptor: its heap offset into *offset. The lasting part first, for a view or a named
   buffer; then the frame's cache; else it is made, into a lasting slot where one is free, else into the
   frame's space */
[[gnu::always_inline]] static inline int resource_descriptor(u32 slot, const VkWriteDescriptorSet* write, u32 element,
                                VkBuffer* previous_buffer, VkDeviceAddress* previous_address, u32* offset) {
    int k = kind(write->descriptorType);
    if (k < 0 || k == HEAP_SAMPLER) return 0;
    int is_buffer = k == HEAP_UBO || k == HEAP_SSBO || k == HEAP_TEXEL;
    if (is_buffer) g_heaps.stats.buffer_requests++;
    else {
        g_heaps.stats.image_requests++;
        g_heaps.stats.image_lookups++;
    }
    g_heaps.stats.payload_bytes += g_heaps.sizes[k];
    ResourceEntry key = {0};
    if (!vk_descriptor_heap_resource_tag(write->descriptorType, VK_FORMAT_UNDEFINED, &key.tag) && k != HEAP_TEXEL)
        return 0;
    VkResourceDescriptorInfoEXT info = {.sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
                                       .type = write->descriptorType};
    VkDeviceAddressRangeEXT range = {0};
    VkTexelBufferDescriptorInfoEXT texel = {.sType = VK_STRUCTURE_TYPE_TEXEL_BUFFER_DESCRIPTOR_INFO_EXT};
    VkImageDescriptorInfoEXT image = {.sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT};
    VkImageViewCreateInfo view;
    u64 chain[HEAP_CHAIN_BYTES / 8];
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize start = 0, size = 0;
    const VkDescriptorImageInfo* in = NULL;
    if (is_buffer) {
        if (k == HEAP_TEXEL) {
            const VkBufferViewCreateInfo* v = write->pNext;
            if (!v || v->sType != VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO || write->descriptorCount != 1) return 0;
            buffer = v->buffer; start = v->offset; size = v->range;
            texel.format = v->format;
            if (!vk_descriptor_heap_resource_tag(write->descriptorType, v->format, &key.tag)) return 0;
        } else {
            if (!write->pBufferInfo) return 0;
            buffer = write->pBufferInfo[element].buffer;
            start = write->pBufferInfo[element].offset;
            size = write->pBufferInfo[element].range;
        }
        if (!buffer || !size || size == VK_WHOLE_SIZE) return 0;
        key.object = handle_key(buffer); key.offset = start; key.range = size;
    } else {
        if (!write->pImageInfo) return 0;
        in = &write->pImageInfo[element];
        if (!in->imageView) return 0;
#if VK_USE_64_BIT_PTR_DEFINES
        key.object = (u64)(uintptr_t)in->imageView;
#else
        key.object = (u64)in->imageView;
#endif
        key.offset = (u32)in->imageLayout;
    }
    /* draw_state_check=1 makes a kept one again below, and compares */
    int lasting = lasting_on() && (!is_buffer || lasting_named(key.object));
    u64 hash = 0;
    u32 kept = 0;
    if (lasting) {
        hash = vk_hash(&key, VK_DESCRIPTOR_HEAP_RESOURCE_KEY_BYTES);
        kept = lasting_find(&key, hash);
        if (kept && !g_settings.draw_state_check)
            return lasting_report(lasting, 1, lasting_hit(kept, is_buffer, offset));
    }
    ResourceEntry* entry = NULL;
    if (!kept) {
        int hit;
        entry = resource_entry(&key, &hit);
        if (hit) {
            *offset = entry->heap_offset;
            if (is_buffer) g_heaps.stats.buffer_hits++;
            else g_heaps.stats.image_hits++;
            return lasting_report(lasting, 0, 1); /* the frame's cache, not the lasting part */
        }
    }
    if (is_buffer) {
        if (*previous_buffer != buffer) { *previous_buffer = buffer; *previous_address = buffer_address(buffer); }
        if (!*previous_address || start > UINT64_MAX - *previous_address) return lasting_report(lasting, 0, 0);
        range = (VkDeviceAddressRangeEXT){*previous_address + start, size};
        if (k == HEAP_TEXEL) { texel.addressRange = range; info.data.pTexelBuffer = &texel; }
        else info.data.pAddressRange = &range;
        g_heaps.stats.buffer_driver_calls++;
        g_heaps.stats.buffer_native_calls++;
    } else {
        if (!vk_descriptor_view_info(in->imageView, &view, chain, sizeof chain)) {
            if (kept) lasting_differs(kept, k); /* a view kept that the registry no longer has */
            return lasting_report(lasting, 0, 0);
        }
        image.pView = &view; image.layout = in->imageLayout;
        info.type = k == HEAP_IMAGE ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        info.data.pImage = &image;
        g_heaps.stats.image_driver_calls++;
    }
    if (!VK_OK(vkWriteResourceDescriptorsEXT(g_vk.dev, 1, &info,
            &(VkHostAddressRangeEXT){g_heaps.scratch, (size_t)g_heaps.sizes[k]})))
        return lasting_report(lasting, 0, 0);
    if (kept && lasting_same(kept, k))
        return lasting_report(lasting, 1, lasting_hit(kept, is_buffer, offset));
    if (kept) entry = resource_entry(&key, &(int){0}); /* made again: into a slot of its own, or the frame's */
    if (lasting && lasting_place(k, &key, hash, offset)) {
        g_heaps.stats.resource_bytes += g_heaps.sizes[k];
        return lasting_report(lasting, 0, 1); /* placed now, not found */
    }
    if (!allocate(&g_heaps.resource, slot, g_heaps.sizes[k], g_heaps.alignment[k], offset))
        return lasting_report(lasting, 0, 0);
    memcpy(g_heaps.resource.mapped + *offset, g_heaps.scratch, (size_t)g_heaps.sizes[k]);
    if (entry) {
        if (is_buffer) g_heaps.stats.buffer_evictions += entry->object != 0;
        else g_heaps.stats.image_evictions += entry->object != 0;
        key.heap_offset = *offset;
        *entry = key;
    }
    g_heaps.stats.resource_bytes += g_heaps.sizes[k];
    return lasting_report(lasting, 0, 1);
}
static int sampler_descriptor(u32 slot, VkSampler sampler, u32* offset) {
    if (!sampler) return 0;
    g_heaps.stats.sampler_requests++;
    g_heaps.stats.sampler_lookups++;
    g_heaps.stats.payload_bytes += g_heaps.sizes[HEAP_SAMPLER];
#if VK_USE_64_BIT_PTR_DEFINES
    u64 handle = (u64)(uintptr_t)sampler;
#else
    u64 handle = (u64)sampler;
#endif
    SamplerHandle* shortcut = &g_heaps.cache.handles[(handle ^ (handle >> 16)) & (HEAP_SAMPLER_HANDLES - 1)];
    if (shortcut->sampler == sampler) { *offset = shortcut->offset; g_heaps.stats.sampler_hits++; return 1; }
    VkSamplerCreateInfo create;
    u64 chain[HEAP_CHAIN_BYTES / 8];
    if (!vk_descriptor_sampler_info(sampler, &create, chain, sizeof chain)) return 0;
    g_heaps.stats.sampler_driver_calls++;
    if (!VK_OK(vkWriteSamplerDescriptorsEXT(g_vk.dev, 1, &create,
            &(VkHostAddressRangeEXT){g_heaps.scratch, (size_t)g_heaps.sizes[HEAP_SAMPLER]}))) return 0;
    u64 hash = vk_hash(g_heaps.scratch, g_heaps.sizes[HEAP_SAMPLER]);
    u32 index = ((u32)hash & (HEAP_SAMPLER_CACHE / 4 - 1)) * 4;
    SamplerEntry* replacement = &g_heaps.cache.samplers[index + (u32)(hash >> 32) % 4];
    size_t shadow_base = (size_t)slot * (size_t)g_heaps.sampler.plan.frame_size;
    for (u32 i = 0; i < 4; i++) {
        SamplerEntry* entry = &g_heaps.cache.samplers[index + i];
        if (!entry->used) { replacement = entry; break; }
        if (vk_descriptor_heap_sampler_equal(entry, hash, g_heaps.sampler_bytes, shadow_base,
                g_heaps.scratch, (size_t)g_heaps.sizes[HEAP_SAMPLER])) {
            *offset = entry->offset;
            *shortcut = (SamplerHandle){sampler, *offset};
            g_heaps.stats.sampler_hits++;
            return 1;
        }
    }
    if (!allocate(&g_heaps.sampler, slot, g_heaps.sizes[HEAP_SAMPLER], g_heaps.alignment[HEAP_SAMPLER], offset)) return 0;
    memcpy(g_heaps.sampler.mapped + *offset, g_heaps.scratch, (size_t)g_heaps.sizes[HEAP_SAMPLER]);
    memcpy(g_heaps.sampler_bytes + ((size_t)*offset - shadow_base), g_heaps.scratch,
           (size_t)g_heaps.sizes[HEAP_SAMPLER]);
    g_heaps.stats.sampler_evictions += replacement->used != 0;
    *replacement = (SamplerEntry){hash, *offset, 1};
    *shortcut = (SamplerHandle){sampler, *offset};
    g_heaps.stats.sampler_bytes += g_heaps.sizes[HEAP_SAMPLER];
    return 1;
}
/* resource_descriptor for word `at` of the table: the word the stage's last table had where the write names the
   same buffer range, or the same view in the same layout, with no hash and no lookup. A texel
   buffer goes the full way. draw_state_check=1 finds a kept word again and compares */
static int memo_resource(u32 slot, const HeapLayout* l, const VkWriteDescriptorSet* w, u32 j, u32 at,
                         VkBuffer* previous_buffer, VkDeviceAddress* previous_address, u32* offset) {
    u64 object = 0, start = 0, range = 0;
    int memo = memo_on();
    if (memo &&
        (w->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
         w->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) &&
        w->pBufferInfo) {
        object = handle_key(w->pBufferInfo[j].buffer);
        start = w->pBufferInfo[j].offset;
        range = w->pBufferInfo[j].range;
    } else if (memo && kind(w->descriptorType) >= HEAP_IMAGE && kind(w->descriptorType) <= HEAP_STORAGE_IMAGE &&
               w->pImageInfo) {
#if VK_USE_64_BIT_PTR_DEFINES
        object = (u64)(uintptr_t)w->pImageInfo[j].imageView;
#else
        object = (u64)w->pImageInfo[j].imageView;
#endif
        start = (u64)w->pImageInfo[j].imageLayout;
    }
    u32 kept;
    int had = object && memo_get(l, at, object, start, range, (u32)w->descriptorType, &kept);
    if (had && !g_settings.draw_state_check) {
        if (range)
            g_heaps.stats.buffer_requests++;
        else
            g_heaps.stats.image_requests++;
        g_heaps.stats.memo_kept++;
        *offset = kept;
        return 1;
    }
    if (!resource_descriptor(slot, w, j, previous_buffer, previous_address, offset)) return 0;
    if (had) memo_compare(kept, *offset, object);
    if (object) memo_put(l, at, object, start, range, (u32)w->descriptorType, *offset);
    return 1;
}
/* sampler_descriptor for word `at` of the table, as memo_resource */
static int memo_sampler(u32 slot, const HeapLayout* l, VkSampler sampler, u32 at, u32* offset) {
#if VK_USE_64_BIT_PTR_DEFINES
    u64 object = (u64)(uintptr_t)sampler;
#else
    u64 object = (u64)sampler;
#endif
    u32 kept;
    int had = object && memo_on() && memo_get(l, at, object, 0, 0, MEMO_SAMPLER, &kept);
    if (had && !g_settings.draw_state_check) {
        g_heaps.stats.sampler_requests++;
        g_heaps.stats.memo_kept++;
        *offset = kept;
        return 1;
    }
    if (!sampler_descriptor(slot, sampler, offset)) return 0;
    if (had) memo_compare(kept, *offset, object);
    if (object && memo_on()) memo_put(l, at, object, 0, 0, MEMO_SAMPLER, *offset);
    return 1;
}
/* Keep repeated-array scanning/filling out of the scalar buffer writer's register and inlining budget. */
[[gnu::noinline]] static int write_image_array(u32 slot, const HeapLayout* layout, u32 table,
                                                      const VkWriteDescriptorSet* write) {
    VkBuffer previous_buffer = VK_NULL_HANDLE;
    VkDeviceAddress previous_address = 0;
    int combined = write->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    for (u32 j = 0; j < write->descriptorCount;) {
        u32 resource, sampler, run = 1;
        if (write->pImageInfo)
            run = vk_descriptor_heap_image_run(write->descriptorType, write->pImageInfo + j, write->descriptorCount - j);
        if (!resource_descriptor(slot, write, j, &previous_buffer, &previous_address, &resource)) return 0;
        if (combined && !sampler_descriptor(slot, write->pImageInfo[j].sampler, &sampler)) return 0;
        u32 word = layout->offset[write->dstBinding] + write->dstArrayElement + j;
        vk_descriptor_heap_write_index_run(g_heaps.resource.mapped + table + 4ull * word, run, resource);
        if (combined) {
            word = layout->sampler_offset[write->dstBinding] + write->dstArrayElement + j;
            vk_descriptor_heap_write_index_run(g_heaps.resource.mapped + table + 4ull * word, run, sampler);
        }
        if (run > 1) {
            g_heaps.stats.image_requests += run - 1;
            g_heaps.stats.image_replicas += run - 1;
            g_heaps.stats.payload_bytes += (u64)(run - 1) * g_heaps.sizes[kind(write->descriptorType)];
            if (combined) {
                g_heaps.stats.sampler_requests += run - 1;
                g_heaps.stats.sampler_replicas += run - 1;
                g_heaps.stats.payload_bytes += (u64)(run - 1) * g_heaps.sizes[HEAP_SAMPLER];
            }
        }
        j += run;
    }
    return 1;
}
/* Keep the original scalar loop in its own compiler frame. Image-array run handling must not change
   its stack/register budget or the buffer-address reuse between writes in the same table. */
[[gnu::noinline]] static int write_scalar_table(u32 slot, const HeapLayout* layout, u32 table, u32 count,
                                                       const VkWriteDescriptorSet* writes) {
    VkBuffer previous_buffer = VK_NULL_HANDLE;
    VkDeviceAddress previous_address = 0;
    for (u32 i = 0; i < count; i++) {
        const VkWriteDescriptorSet* w = &writes[i];
        for (u32 j = 0; j < w->descriptorCount; j++) {
            u32 resource, sampler;
            if (!resource_descriptor(slot, w, j, &previous_buffer, &previous_address, &resource)) return 0;
            u32 word = layout->offset[w->dstBinding] + w->dstArrayElement + j;
            snx_st_le32(g_heaps.resource.mapped + table + 4ull * word, resource);
            if (w->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                if (!sampler_descriptor(slot, w->pImageInfo[j].sampler, &sampler)) return 0;
                snx_st_le32(g_heaps.resource.mapped + table + 4ull * (layout->sampler_offset[w->dstBinding] + w->dstArrayElement + j), sampler);
            }
        }
    }
    return 1;
}
[[gnu::noinline]] static int write_array_table(u32 slot, const HeapLayout* layout, u32 table, u32 count,
                                                      const VkWriteDescriptorSet* writes) {
    VkBuffer previous_buffer = VK_NULL_HANDLE;
    VkDeviceAddress previous_address = 0;
    for (u32 i = 0; i < count; i++) {
        const VkWriteDescriptorSet* w = &writes[i];
        if (w->descriptorCount > 1 &&
            (w->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
             w->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || w->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)) {
            if (!write_image_array(slot, layout, table, w)) return 0;
            continue;
        }
        for (u32 j = 0; j < w->descriptorCount; j++) {
            u32 resource, sampler;
            if (!resource_descriptor(slot, w, j, &previous_buffer, &previous_address, &resource)) return 0;
            u32 word = layout->offset[w->dstBinding] + w->dstArrayElement + j;
            snx_st_le32(g_heaps.resource.mapped + table + 4ull * word, resource);
            if (w->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                if (!sampler_descriptor(slot, w->pImageInfo[j].sampler, &sampler)) return 0;
                snx_st_le32(g_heaps.resource.mapped + table +
                    4ull * (layout->sampler_offset[w->dstBinding] + w->dstArrayElement + j), sampler);
            }
        }
    }
    return 1;
}
/* Only unformatted scalar buffers enter this helper. Keeping its planning arrays out of the original writer
   preserves the singleton/image paths' compiler frame. Each chunk is a transaction; earlier chunks stay live. */
[[gnu::noinline]] static int write_buffer_batch(u32 slot, const HeapLayout* layout, u32 table, u32 count,
                                                       const VkWriteDescriptorSet* writes, VkBuffer* previous_buffer,
                                                       VkDeviceAddress* previous_address) {
    VkDescriptorHeapBufferBatch batch;
    batch.used = g_heaps.resource.used[slot];
    batch.count = batch.misses = batch.changes = batch.exhausted = 0;
    batch.hits = batch.evictions = batch.bytes = 0;
    VkResourceDescriptorInfoEXT info[VK_DESCRIPTOR_HEAP_BUFFER_BATCH];
    VkDeviceAddressRangeEXT ranges[VK_DESCRIPTOR_HEAP_BUFFER_BATCH];
    VkHostAddressRangeEXT destinations[VK_DESCRIPTOR_HEAP_BUFFER_BATCH];
    size_t stride = (size_t)(g_heaps.sizes[HEAP_UBO] > g_heaps.sizes[HEAP_SSBO]
        ? g_heaps.sizes[HEAP_UBO] : g_heaps.sizes[HEAP_SSBO]);
    const int memo = memo_on();
    u32 kept[VK_DESCRIPTOR_HEAP_BUFFER_BATCH]; /* draw_state_check=1: the memo's word, compared below */
    int had[VK_DESCRIPTOR_HEAP_BUFFER_BATCH] = {0};
    for (u32 i = 0; i < count; i++) {
        const VkWriteDescriptorSet* write = &writes[i];
        int k = write->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? HEAP_UBO : HEAP_SSBO;
        g_heaps.stats.buffer_requests++;
        g_heaps.stats.payload_bytes += g_heaps.sizes[k];
        if (!write->pBufferInfo) return 0;
        const VkDescriptorBufferInfo* buffer = write->pBufferInfo;
        if (!buffer->buffer || !buffer->range || buffer->range == VK_WHOLE_SIZE) return 0;
        ResourceEntry key = {.object = handle_key(buffer->buffer), .offset = buffer->offset, .range = buffer->range,
            .tag = (u32)k};
        /* the binding as the stage's last table had it: its word, with no hash and no lookup */
        if (memo && memo_get(layout, layout->offset[write->dstBinding] + write->dstArrayElement, key.object, key.offset,
                             key.range, (u32)write->descriptorType, &kept[i])) {
            had[i] = 1;
            if (!g_settings.draw_state_check) {
                batch.offsets[batch.count++] = kept[i];
                g_heaps.stats.memo_kept++;
                continue;
            }
        }
        u32 before = batch.misses;
        if (!vk_descriptor_heap_buffer_plan(&batch, g_heaps.cache.resources, HEAP_RESOURCE_CACHE,
                &g_heaps.resource.plan, slot, &key, vk_hash(&key, VK_DESCRIPTOR_HEAP_RESOURCE_KEY_BYTES),
                (u32)g_heaps.sizes[k], g_heaps.alignment[k])) {
            if (batch.exhausted) {
                g_heaps.exhausted[slot] = 1;
                memo_clear();
            }
            return 0;
        }
        if (batch.misses == before) continue;
        if (*previous_buffer != buffer->buffer) {
            *previous_buffer = buffer->buffer;
            *previous_address = buffer_address(buffer->buffer);
        }
        if (!*previous_address || buffer->offset > UINT64_MAX - *previous_address) return 0;
        ranges[before] = (VkDeviceAddressRangeEXT){*previous_address + buffer->offset, buffer->range};
        info[before] = (VkResourceDescriptorInfoEXT){.sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
            .type = write->descriptorType, .data.pAddressRange = &ranges[before]};
        destinations[before] = (VkHostAddressRangeEXT){g_heaps.scratch + (size_t)before * stride,
            (size_t)g_heaps.sizes[k]};
    }
    int encoded = 1;
    if (batch.misses) {
        g_heaps.stats.buffer_driver_calls += batch.misses;
        g_heaps.stats.buffer_native_calls++;
        encoded = VK_OK(vkWriteResourceDescriptorsEXT(g_vk.dev, batch.misses, info, destinations));
    }
    VkDeviceSize before = g_heaps.resource.used[slot];
    if (!vk_descriptor_heap_buffer_publish(&batch, g_heaps.cache.resources, &g_heaps.resource.used[slot],
            g_heaps.resource.mapped, g_heaps.scratch, stride, encoded)) return 0;
    g_heaps.stats.arena_bytes += batch.used - before;
    u64 live = batch.used + g_heaps.sampler.used[slot];
    if (live > g_heaps.stats.peak_used_bytes) g_heaps.stats.peak_used_bytes = live;
    g_heaps.stats.resource_bytes += batch.bytes;
    g_heaps.stats.buffer_hits += batch.hits;
    g_heaps.stats.buffer_evictions += batch.evictions;
    for (u32 i = 0; i < count; i++) {
        u32 word = layout->offset[writes[i].dstBinding] + writes[i].dstArrayElement;
        snx_st_le32(g_heaps.resource.mapped + table + 4ull * word, batch.offsets[i]);
        if (!memo) continue;
        const VkDescriptorBufferInfo* buffer = writes[i].pBufferInfo;
        if (had[i] && g_settings.draw_state_check) memo_compare(kept[i], batch.offsets[i], handle_key(buffer->buffer));
        memo_put(layout, word, handle_key(buffer->buffer), buffer->offset, buffer->range, (u32)writes[i].descriptorType,
                 batch.offsets[i]);
    }
    return 1;
}
/* a named buffer's descriptor goes by itself, to the lasting part (resource_descriptor) */
static int batchable_buffer(const VkWriteDescriptorSet* write) {
    return write->descriptorCount == 1 &&
        (write->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || write->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) &&
        (!lasting_on() || !write->pBufferInfo || !lasting_named(handle_key(write->pBufferInfo->buffer)));
}
[[gnu::noinline]] static int write_batched_table(u32 slot, const HeapLayout* layout, u32 table, u32 count,
                                                        const VkWriteDescriptorSet* writes) {
    VkBuffer previous_buffer = VK_NULL_HANDLE;
    VkDeviceAddress previous_address = 0;
    for (u32 i = 0; i < count; i++) {
        const VkWriteDescriptorSet* w = &writes[i];
        if (batchable_buffer(w)) {
            u32 run = 1;
            while (run < VK_DESCRIPTOR_HEAP_BUFFER_BATCH && run < count - i && batchable_buffer(w + run)) run++;
            if (run > 1) {
                if (!write_buffer_batch(slot, layout, table, run, w, &previous_buffer, &previous_address)) return 0;
                i += run - 1;
                continue;
            }
        }
        if (w->descriptorCount > 1 &&
            (w->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
             w->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || w->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)) {
            if (!write_image_array(slot, layout, table, w)) return 0;
            continue;
        }
        for (u32 j = 0; j < w->descriptorCount; j++) {
            u32 resource, sampler;
            u32 word = layout->offset[w->dstBinding] + w->dstArrayElement + j;
            if (!memo_resource(slot, layout, w, j, word, &previous_buffer, &previous_address, &resource)) return 0;
            snx_st_le32(g_heaps.resource.mapped + table + 4ull * word, resource);
            if (w->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                word = layout->sampler_offset[w->dstBinding] + w->dstArrayElement + j;
                if (!memo_sampler(slot, layout, w->pImageInfo[j].sampler, word, &sampler)) return 0;
                snx_st_le32(g_heaps.resource.mapped + table + 4ull * word, sampler);
            }
        }
    }
    return 1;
}
VkDescriptorSet vk_descriptor_heaps_write(VkDescriptorSetLayout layout, u32 count, const VkWriteDescriptorSet* writes,
                                          u32 cb_addr) {
    SNX_ZONE(z, "vk descriptor writes");
    HeapLayout* l = find_layout(layout);
    if (!l || !g_heaps.ready || (count && !writes) || count > SNX_VKB_END) return VK_NULL_HANDLE;
    VkDeviceSize words = 1;
    int image_arrays = 0, buffer_batches = 0;
    u32 buffer_run = 0, nrest = 0, naddr = 0;
    /* the writes of descriptors, and apart from them those of the constant buffers mapped by address */
    VkWriteDescriptorSet rest[SNX_VKB_END];
    const VkWriteDescriptorSet* addr[18];
    for (u32 i = 0; i < count; i++) {
        const VkWriteDescriptorSet* w = &writes[i];
        if (w->dstBinding >= SNX_VKB_END || w->descriptorType != l->type[w->dstBinding] ||
            !l->count[w->dstBinding] || w->dstArrayElement > l->count[w->dstBinding] ||
            w->descriptorCount > l->count[w->dstBinding] - w->dstArrayElement) return VK_NULL_HANDLE;
        VkDeviceSize end = l->offset[w->dstBinding] + w->dstArrayElement + w->descriptorCount;
        if (by_address(l, w, l->pairs)) end = l->offset[w->dstBinding] + 2;
        if (by_address(l, w, cb_addr)) {
            if (!w->pBufferInfo || !w->pBufferInfo->buffer || naddr == 18) return VK_NULL_HANDLE;
            addr[naddr++] = w;
            if (end > words) words = end;
            continue;
        }
        rest[nrest++] = *w;
        if (w->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
            end = l->sampler_offset[w->dstBinding] + w->dstArrayElement + w->descriptorCount;
        if (end > words) words = end;
        image_arrays |= w->descriptorCount > 1 &&
            (w->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
             w->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || w->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        buffer_run = batchable_buffer(w) ? buffer_run + 1 : 0;
        buffer_batches |= buffer_run >= 2;
    }
    count = nrest;
    writes = rest;
    vk_cmd();
    u32 slot = vk_frame_slot(), table;
    /* Recording workers and the GPU retain copied addresses and immutable heap bytes, never this CPU metadata.
       Clear before any lookup, including writes that later fail and fall back to another descriptor backend. */
    if (vk_descriptor_heap_cache_frame(&g_heaps.cache_frame, vk_frame_id())) {
        memset(&g_heaps.cache, 0, sizeof g_heaps.cache);
        memo_clear(); /* a new frame, or the cache let go: the words name what the frame's arena held */
    }
    lasting_begin();
    if (g_heaps.exhausted[slot]) return VK_NULL_HANDLE;
    if (!allocate(&g_heaps.resource, slot, words * 4, 8, &table)) return VK_NULL_HANDLE;
    /* the addresses: no lookup, no memo and no call of the driver for a descriptor */
    VkBuffer previous = VK_NULL_HANDLE;
    VkDeviceAddress base = 0;
    for (u32 i = 0; i < naddr; i++) {
        const VkDescriptorBufferInfo* b = addr[i]->pBufferInfo;
        if (b->buffer != previous) {
            previous = b->buffer;
            base = buffer_address(b->buffer);
        }
        if (!base) return VK_NULL_HANDLE;
        snx_st_le64(g_heaps.resource.mapped + table + 4ull * l->offset[addr[i]->dstBinding], base + b->offset);
        g_heaps.stats.cb_addresses++;
    }
    if (buffer_batches) {
        if (!write_batched_table(slot, l, table, count, writes)) return VK_NULL_HANDLE;
    } else if (image_arrays) {
        if (!write_array_table(slot, l, table, count, writes)) return VK_NULL_HANDLE;
    } else if (!write_scalar_table(slot, l, table, count, writes)) return VK_NULL_HANDLE;
    SNX_ZONE_TEXT(z, "%u writes into 1 set, %llu words, %u by address, %s", count, (unsigned long long)words, naddr,
                  buffer_batches ? "buffer batches" : image_arrays ? "image arrays" : "scalars");
    SNX_ZONE_VALUE(z, count);
    SNX_ZONE_END(z);
    SNX_PLOT("vk pipe desc write words", (double)words);
    g_heaps.stats.tables++;
    g_heaps.stats.table_bytes += words * 4;
    g_heaps.stats.full_layout_bytes += l->full_size;
    return (VkDescriptorSet)(uintptr_t)(table + 1);
}
void vk_descriptor_heaps_bind(VkCommandBuffer cb, u32 first, u32 count, const VkDescriptorSet* sets) {
    if (!g_heaps.ready || first > 5 || count > 5 - first || !count) return;
    u32 begun = vk_cb_begun();
    if (g_heaps.buffer_begun == begun) {
        /* Heap binding invalidates all non-heap state by specification. VVL1.4.367 nevertheless retains and
           validates old buffer offsets against the heap pipeline. An incompatible classic set0 bind
           explicitly disturbs those slots before the heap bind, without a draw, submission or wait between them.
           The one immutable empty set is device-owned; ordinary heap draws never take this transition. */
        int active = g_heaps.active;
        rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_heaps.transition_layout,
            0, 1, &g_heaps.transition_set, 0, NULL);
        g_heaps.active = active; /* the pipeline did not change: subsequent graphics constants still use push data */
        g_heaps.buffer_begun = 0;
    }
    if (g_heaps.begun != begun || g_heaps.bound_epoch != g_heaps.epoch) {
        VkBindHeapInfoEXT resource = {.sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
            .heapRange = {g_heaps.resource.address, g_heaps.resource.plan.range_size},
            .reservedRangeOffset = g_heaps.resource.plan.reserved_offset, .reservedRangeSize = g_heaps.resource.plan.reserved_size};
        VkBindHeapInfoEXT sampler = {.sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
            .heapRange = {g_heaps.sampler.address, g_heaps.sampler.plan.range_size},
            .reservedRangeOffset = g_heaps.sampler.plan.reserved_offset, .reservedRangeSize = g_heaps.sampler.plan.reserved_size};
        rec_CmdBindDescriptorHeapsEXT(cb, &resource, &sampler);
        g_heaps.begun = begun;
        g_heaps.bound_epoch = g_heaps.epoch;
    }
    u32 addresses[10];
    for (u32 i = 0; i < count; i++) {
        u64 offset = (u64)(uintptr_t)sets[i];
        if (!offset || --offset >= g_heaps.resource.plan.reserved_offset) fatal("vulkan: invalid descriptor heap table");
        u64 address = g_heaps.resource.address + offset;
        addresses[2 * i] = (u32)address;
        addresses[2 * i + 1] = (u32)(address >> 32);
    }
    rec_CmdPushDataEXT(cb, VK_DESCRIPTOR_HEAP_PUSH_OFFSET + 8 * first, count * 8, addresses);
}
void vk_descriptor_heaps_reset(u32 slot) {
    if (slot >= VK_FRAMES) return;
    g_heaps.resource.used[slot] = g_heaps.sampler.used[slot] = 0;
    g_heaps.exhausted[slot] = 0;
    if (slot == vk_frame_slot()) vk_descriptor_heap_cache_reset(&g_heaps.cache_frame);
    memo_clear(); /* the slot's offsets are free again */
}
int vk_descriptor_heaps_room(u32 slot) {
    if (!g_heaps.ready || slot >= VK_FRAMES) return 1;
    /* Declared descriptor arrays can exceed a small heap even though only a few live elements are written. Keep
       a bounded margin for the next draw; an unusually large live write falls back without a mid-draw submission. */
    if (g_heaps.exhausted[slot]) return 0;
    VkDeviceSize resource_reserve = g_heaps.resource.plan.frame_size / 8;
    VkDeviceSize sampler_reserve = g_heaps.sampler.plan.frame_size / 8;
    if (resource_reserve > 16384) resource_reserve = 16384;
    if (sampler_reserve > 4096) sampler_reserve = 4096;
    return g_heaps.resource.plan.frame_size - g_heaps.resource.used[slot] >= resource_reserve &&
           g_heaps.sampler.plan.frame_size - g_heaps.sampler.used[slot] >= sampler_reserve;
}
void vk_descriptor_heaps_get_stats(VkDescriptorHeapStats* stats) {
    *stats = g_heaps.stats;
    stats->arena_allocated_bytes = g_heaps.resource.allocation_bytes + g_heaps.sampler.allocation_bytes;
    stats->cache_allocated_bytes = sizeof g_heaps.cache + sizeof g_heaps.cache_frame + sizeof g_heaps.exhausted +
                                     g_heaps.scratch_size + g_heaps.lasting.cpu_bytes +
                                     sizeof *g_heaps.lasting.buffers * g_heaps.lasting.buffer_cap;
    stats->reserved_bytes = g_heaps.resource.plan.reserved_size + g_heaps.sampler.plan.reserved_size;
    stats->resource_range_bytes = g_heaps.resource.plan.range_size;
    stats->sampler_range_bytes = g_heaps.sampler.plan.range_size;
}
void vk_descriptor_heaps_stats(void) {
    if (!g_heaps.ready) return;
    const VkDescriptorHeapStats *s = &g_heaps.stats, *p = &g_heaps.reported;
    LOG("vulkan: descriptor heaps: %llu tables, %llu index bytes, %llu resource bytes, %llu sampler bytes, %llu reused encodings\n",
        (unsigned long long)(s->tables - p->tables), (unsigned long long)(s->table_bytes - p->table_bytes),
        (unsigned long long)(s->resource_bytes - p->resource_bytes), (unsigned long long)(s->sampler_bytes - p->sampler_bytes),
        (unsigned long long)(s->buffer_hits + s->image_hits + s->sampler_hits - p->buffer_hits - p->image_hits - p->sampler_hits));
    if (g_heaps.lasting.slot)
        LOG("vulkan: descriptor heaps' lasting part: %llu descriptors found, %llu made, %llu forgotten with their "
            "object, %llu freed, %llu taken back, %llu not kept (no slot), %llu checked, %llu different; lasting "
            "reap: %llu walks (%llu skipped, the GPU not done with the last frame), %llu slots looked at\n",
            (unsigned long long)(s->lasting_hits - p->lasting_hits),
            (unsigned long long)(s->lasting_made - p->lasting_made),
            (unsigned long long)(s->lasting_forgotten - p->lasting_forgotten),
            (unsigned long long)(s->lasting_freed - p->lasting_freed),
            (unsigned long long)(s->lasting_evicted - p->lasting_evicted),
            (unsigned long long)(s->lasting_full - p->lasting_full),
            (unsigned long long)(s->lasting_checked - p->lasting_checked),
            (unsigned long long)(s->lasting_differs - p->lasting_differs),
            (unsigned long long)(s->lasting_reaps - p->lasting_reaps),
            (unsigned long long)(s->lasting_reaps_skipped - p->lasting_reaps_skipped),
            (unsigned long long)(s->lasting_reap_looked - p->lasting_reap_looked));
    LOG("vulkan: descriptor heaps: %llu descriptors kept from the stage's last table with no lookup, of %llu buffer, "
        "image and sampler requests\n",
        (unsigned long long)(s->memo_kept - p->memo_kept),
        (unsigned long long)(s->buffer_requests + s->image_requests + s->sampler_requests - p->buffer_requests -
                             p->image_requests - p->sampler_requests));
    if (s->memo_checked != p->memo_checked)
        LOG("vulkan: draw_state_check=1: %llu descriptors the stage's last table had found again, %llu different\n",
            (unsigned long long)(s->memo_checked - p->memo_checked),
            (unsigned long long)(s->memo_differs - p->memo_differs));
    g_heaps.reported = *s;
}
void vk_descriptor_heaps_invalidate(void) { g_heaps.epoch++; g_heaps.active = 0; }
void vk_descriptor_heaps_buffer_bound(u32 begun) { if (g_heaps.ready) g_heaps.buffer_begun = begun; }
void vk_descriptor_heaps_destroy(void) {
    lasting_destroy();
    transition_destroy();
    heap_discard(&g_heaps.resource);
    heap_discard(&g_heaps.sampler);
    free(g_heaps.scratch);
    for (u32 i = 0; i < g_heaps.nlayouts; i++) free(g_heaps.layouts[i].memo);
    g_heaps = (HeapState){.epoch = g_heaps.epoch + 1};
}
uint64_t vk_descriptor_heaps_epoch(void) { return g_heaps.epoch; }
void vk_descriptor_heaps_pipeline_state(int active) { g_heaps.active = active && g_heaps.ready; }
int vk_descriptor_heaps_active(void) { return g_heaps.active; }
