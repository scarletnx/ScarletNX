/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* --bench-stage-memory: what the CPU's copies into a staging ring cost in each kind of host memory the device has. A
   ring's buffer is made the two ways cmd.c's chunk_new makes one (vk_stage_memory): written in order, which VMA puts
   in memory that is not cached where the device has such a type (write-combined on the drivers that map it so), and
   read and written anywhere, which it puts in cached memory. 64 MiB of ordinary memory is then copied into each in
   blocks of one size, one block after the other at the ring's alignment, as a frame's uploads fill a ring. The best
   of eight passes counts: the first one of a buffer takes its page faults. A line per kind and block size */
#define STAGE_BENCH_BYTES (64u << 20)
static u64 stage_bench_pass(u8* dst, const u8* src, u32 block) {
    u32 step = (block + 63) & ~63u;
    u64 t = plat_ns();
    for (u32 at = 0; at + block <= STAGE_BENCH_BYTES; at += step) memcpy(dst + at, src + at, block);
    return plat_ns() - t;
}
static int bench_stage_memory(void) {
    static const u32 blocks[] = {64, 256, 4096, 65536, 1u << 20, 16u << 20};
    static const struct {
        const char* name;
        VmaAllocationCreateFlags access;
    } kinds[] = {{"sequential", VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT},
                 {"cached", VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT}};
    const VkPhysicalDeviceMemoryProperties* mp;
    vmaGetMemoryProperties(g_vk.vma, &mp);
    u8* src = malloc(STAGE_BENCH_BYTES);
    CHECK(src);
    for (u32 i = 0; i < STAGE_BENCH_BYTES; i += 4) snx_st_le32(src + i, i * 2654435761u);
    for (u32 k = 0; k < sizeof kinds / sizeof *kinds; k++) {
        VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  .size = STAGE_BENCH_BYTES,
                                  .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                  .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VmaAllocationCreateInfo aci = {.flags = kinds[k].access | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                       .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
        VmaAllocationInfo info;
        VkBuffer buf;
        VmaAllocation mem;
        vk_lock();
        CHECK(VK_OK(vk_buffer_create(g_vk.vma, &bci, &aci, &buf, &mem, &info)));
        vk_unlock();
        CHECK(info.pMappedData);
        VkMemoryPropertyFlags f = mp->memoryTypes[info.memoryType].propertyFlags;
        for (u32 b = 0; b < sizeof blocks / sizeof *blocks; b++) {
            u64 best = ~0ull;
            for (u32 pass = 0; pass < 8; pass++) {
                u64 ns = stage_bench_pass(info.pMappedData, src, blocks[b]);
                if (ns < best) best = ns;
            }
            u64 bytes = (u64)(STAGE_BENCH_BYTES / ((blocks[b] + 63) & ~63u)) * blocks[b];
            printf("GPU_HOTPATH_BENCH workload=stage_memory kind=%s memory_type=%u device_local=%d cached=%d "
                   "coherent=%d block=%u bytes=%llu ns=%llu mb_per_s=%llu\n",
                   kinds[k].name, info.memoryType, !!(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
                   !!(f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT), !!(f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT), blocks[b],
                   (unsigned long long)bytes, (unsigned long long)best,
                   (unsigned long long)(best ? bytes * 1000000000ull / best >> 20 : 0));
        }
        vk_lock();
        vmaDestroyBuffer(g_vk.vma, buf, mem);
        vk_unlock();
    }
    free(src);
    return 0;
}
