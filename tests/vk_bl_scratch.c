/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The block linear scratch buffer (gpu/vk/blocklinear.c's vk_bl_scratch) when the device's memory refuses a larger one.
   The test plays the buffer's allocator, which it can make refuse, the heap's use the trim reads, and the log. A refused
   call gives no buffer and no room; the scratch already made stays in use for what fits it; each refused size is said
   once, with the size asked, the one that stays (or none) and the driver's result, whatever the number of conversions
   that ask for it, sizes asked in turn included, until the scratch changes (made, or let go by the trim); a refused
   ask weighs nothing in the trim's choice. No Vulkan loader, device or GPU is used. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/blocklinear.c"

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)
#define MIB (1ull << 20)

VkState g_vk;
int g_gob_native;

/* the allocator: buffers numbered from 1, and the bytes it refuses from (0: none) */
static struct {
    u64 refuse_from, usage;
    unsigned made, deferred, tracked;
    VkBuffer last_deferred;
    unsigned warnings;
    char last[512];
} g_t;
VkResult vk_buffer_create(const VkBufferCreateInfo* info, const DevMemWant* want, VkBuffer* buffer, DevMem* memory,
                          DevMemInfo* details) {
    (void)want, (void)details;
    if (g_t.refuse_from && info->size >= g_t.refuse_from) {
        *buffer = VK_NULL_HANDLE;
        *memory = 0;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    ++g_t.made;
    *buffer = (VkBuffer)(uintptr_t)g_t.made;
    *memory = (DevMem)g_t.made;
    return VK_SUCCESS;
}
void vk_defer_buffer(VkBuffer b, DevMem a) {
    (void)a;
    ++g_t.deferred;
    g_t.last_deferred = b;
}
void vk_mem_track(DevMem a, u32 owner) {
    (void)a, (void)owner;
    ++g_t.tracked;
}
void vk_mem_use(u64* usage, u64* budget) { *usage = g_t.usage, *budget = 1000; }
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_t.last, sizeof g_t.last, fmt, ap);
    va_end(ap);
    if (lvl == 0) ++g_t.warnings;
    fputs(g_t.last, stderr);
}
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}
int vk_ok_(VkResult r, const char* what, const char* file, int line) {
    if (r != VK_SUCCESS)
        logf_(0, "vulkan: %s failed: %s (%d) at %s:%d\n", what, snx_vk_result_str(r), (int)r, file, line);
    return r == VK_SUCCESS;
}
/* what the moves need, which no case here reaches */
u64 plat_ns(void) { return 0; }
void vk_texture_pipeline_made(u64 ns) { (void)ns; }
VkPipelineCache vk_pipeline_cache(void) { return VK_NULL_HANDLE; }
VkDescriptorSet vk_desc_alloc(VkDescriptorSetLayout layout) {
    (void)layout;
    return VK_NULL_HANDLE;
}
void vk_ck_(VkCommandBuffer cb, u32 kind, u32 a, u32 b, u32 c, u32 d) {
    (void)cb, (void)kind, (void)a, (void)b, (void)c, (void)d;
}
void vk_gpuprof_tag_at(u64 key, const void* site) { (void)key, (void)site; }
void rec_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* d) { (void)cb, (void)d; }
void rec_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline pipe) {
    (void)cb, (void)bp, (void)pipe;
}
void rec_CmdDispatch(VkCommandBuffer cb, u32 x, u32 y, u32 z) { (void)cb, (void)x, (void)y, (void)z; }
void rec_CmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages, u32 offset, u32 size,
                          const void* data) {
    (void)cb, (void)layout, (void)stages, (void)offset, (void)size, (void)data;
}
void rec_CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, u32 first, u32 n,
                               const VkDescriptorSet* sets, u32 ndyn, const u32* dyn) {
    (void)cb, (void)bp, (void)layout, (void)first, (void)n, (void)sets, (void)ndyn, (void)dyn;
}

int main(void) {
    u64 cap = 0;
    g_vk.alloc_cap = 1ull << 30;

    /* the first scratch refused: none stays, said so */
    g_t.refuse_from = 1;
    CHECK(vk_bl_scratch(5 * MIB, &cap) == VK_NULL_HANDLE && cap == 0);
    CHECK(vk_bl_scratch(5 * MIB, &cap) == VK_NULL_HANDLE);
    CHECK(g_t.warnings == 1 && strstr(g_t.last, "8.0 MB (5242880 bytes asked)") && strstr(g_t.last, "none stays"));

    /* memory found: a scratch of 8 MiB, which says nothing */
    g_t.refuse_from = 0;
    VkBuffer first = vk_bl_scratch(5 * MIB, &cap);
    CHECK(first != VK_NULL_HANDLE && cap == 8 * MIB && g_t.made == 1 && g_t.tracked == 1 && g_t.warnings == 1);

    /* the device refuses anything from 16 MiB: 100 conversions of 20 MiB each ask, and none gets a buffer or room */
    g_t.refuse_from = 16 * MIB;
    for (int i = 0; i < 100; i++) {
        cap = 123;
        CHECK(vk_bl_scratch(20 * MIB, &cap) == VK_NULL_HANDLE);
        CHECK(cap == 0);
    }
    /* said once, with the size asked, the one that stays and the driver's result */
    CHECK(g_t.warnings == 2);
    CHECK(strstr(g_t.last, "32.0 MB (20971520 bytes asked)") != NULL);
    CHECK(strstr(g_t.last, "VK_ERROR_OUT_OF_DEVICE_MEMORY") != NULL);
    CHECK(strstr(g_t.last, "the one of 8.0 MB stays") != NULL);
    /* the scratch made before was never let go, nothing refused was entered in the ledger, and it serves what fits */
    CHECK(g_t.deferred == 0 && g_t.tracked == 1);
    CHECK(vk_bl_scratch(6 * MIB, &cap) == first && cap == 8 * MIB);

    /* two sizes asked in turn: the new one said once, the one said before never again */
    for (int i = 0; i < 50; i++) {
        CHECK(vk_bl_scratch(40 * MIB, &cap) == VK_NULL_HANDLE && cap == 0);
        CHECK(vk_bl_scratch(20 * MIB, &cap) == VK_NULL_HANDLE && cap == 0);
    }
    CHECK(g_t.warnings == 3);
    CHECK(strstr(g_t.last, "64.0 MB (41943040 bytes asked)") != NULL);

    /* memory found again: the larger one is made, the old one deferred to its frame's end */
    g_t.refuse_from = 0;
    VkBuffer second = vk_bl_scratch(20 * MIB, &cap);
    CHECK(second != VK_NULL_HANDLE && second != first && cap == 32 * MIB && g_t.tracked == 2);
    CHECK(g_t.deferred == 1 && g_t.last_deferred == first);

    /* refused again later, a size said before is said again: the scratch changed in between */
    g_t.refuse_from = 16 * MIB;
    CHECK(vk_bl_scratch(60 * MIB, &cap) == VK_NULL_HANDLE);
    CHECK(g_t.warnings == 4 && strstr(g_t.last, "the one of 32.0 MB stays"));

    /* the trim, with the heap past four fifths: refused asks of 64 MiB weigh nothing, the 32 MiB scratch served 6 MiB
       at most since it last looked, so it goes; the size refused before it, refused after it, keeps none and is said
       again */
    vk_bl_scratch_trim(); /* what the scratch served so far counts from here */
    CHECK(vk_bl_scratch(6 * MIB, &cap) == second && cap == 32 * MIB);
    CHECK(vk_bl_scratch(60 * MIB, &cap) == VK_NULL_HANDLE);
    g_t.usage = 900;
    vk_bl_scratch_trim();
    CHECK(g_t.deferred == 2 && g_t.last_deferred == second);
    CHECK(vk_bl_scratch(60 * MIB, &cap) == VK_NULL_HANDLE && cap == 0);
    CHECK(g_t.warnings == 5 && strstr(g_t.last, "64.0 MB (62914560 bytes asked)") && strstr(g_t.last, "none stays"));

    /* past the allocation cap, and past the scratch: no buffer and no room, nothing made or let go, nothing said */
    g_t.refuse_from = 0;
    CHECK(vk_bl_scratch(10 * MIB, &cap) != VK_NULL_HANDLE && cap == 16 * MIB);
    g_vk.alloc_cap = 16 * MIB;
    cap = 123;
    const unsigned made = g_t.made, deferred = g_t.deferred, warnings = g_t.warnings;
    CHECK(vk_bl_scratch(17 * MIB, &cap) == VK_NULL_HANDLE && cap == 0);
    CHECK(g_t.made == made && g_t.deferred == deferred && g_t.warnings == warnings);
    /* a scratch clamped to the cap (not a power of two), refused: said once, as its own size */
    g_vk.alloc_cap = 24 * MIB;
    g_t.refuse_from = 16 * MIB;
    CHECK(vk_bl_scratch(20 * MIB, &cap) == VK_NULL_HANDLE);
    CHECK(vk_bl_scratch(20 * MIB, &cap) == VK_NULL_HANDLE);
    CHECK(g_t.warnings == warnings + 1 && strstr(g_t.last, "24.0 MB (20971520 bytes asked)"));
    puts("vk_bl_scratch: the scratch kept, each refused size said once");
    return 0;
}
