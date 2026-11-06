/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* --bench-sparse-volume: what a sparse volume costs the renderer, made the way a game makes one through NVN
   (nvn_tex_layout, nvn_tic_encode) at the size of one game's light volumes, a few of its 64 KiB tiles mapped: the
   texture made, a frame with nothing changed, tiles mapped (the game's MapVirtual), the CPU writing a tile, a tile
   unmapped while copies submitted before it still run. Each step is timed on the calling thread's CPU clock and on
   the wall clock to the end of the GPU's work for it (vk_finish), with the bytes the texture loaded (vk_tex_stats),
   the sparse binds it made on the queue and the host's waits for the GPU before that end (sc_now). The checks of the
   ordinary run say the image is right */
#include <time.h>
static u64 sb_cpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}
typedef struct {
    u64 cpu0, wall0, bytes0;
    ScCount c0;
} SbStep;
static void sb_begin(SbStep* s) {
    u64 uploads;
    vk_tex_stats(&uploads, &s->bytes0);
    s->c0 = sc_now();
    s->cpu0 = sb_cpu_ns();
    s->wall0 = plat_ns();
}
/* the step's times, bytes, binds and waits, said after what */
static void sb_end(const SbStep* s, const char* name, const char* what) {
    u64 uploads, bytes, cpu = sb_cpu_ns() - s->cpu0;
    vk_lock();
    ScCount c1 = sc_now(); /* before the step's own wait for the GPU's end */
    vk_finish();
    ScCount c2 = sc_now();
    vk_unlock();
    vk_tex_stats(&uploads, &bytes);
    printf("bench sparse volume %s, %s: %.3f ms CPU, %.3f ms to the GPU's end, %llu KB loaded, %llu queue binds, %llu "
           "host waits (%.3f ms)\n",
           name, what, cpu / 1e6, (plat_ns() - s->wall0) / 1e6, (unsigned long long)((bytes - s->bytes0) >> 10),
           (unsigned long long)(c2.binds - s->c0.binds), (unsigned long long)(c1.waits - s->c0.waits),
           (c1.wait_ns - s->c0.wait_ns) / 1e6);
}
static Surf* sb_frame(const u32* tic) {
    g_renderer->mem_may_have_changed();
    g_vk_present++;
    vk_lock();
    Surf* t = vk_tex_from_tic(tic);
    vk_unlock();
    return t;
}
static int bench_sparse_volume(int format, const char* name, u64 gva, u64 cpu) {
    NvnTexture tex = {0};
    NvnTexLayout lay;
    u32 tile[3] = {0}, tic[8];
    SbStep s;
    tex.d.target = NVN_TEXTURE_TARGET_3D;
    tex.d.format = (u16)format;
    tex.d.levels = 1;
    tex.d.flags = NVN_TEXTURE_FLAGS_SPARSE;
    tex.d.width = 1664, tex.d.height = 448, tex.d.depth = 1344;
    tex.gpu = gva;
    CHECK(nvn_tex_sparse_tile(&tex.d, tile));
    nvn_tex_layout(&tex.d, &lay);
    CHECK(nvn_tic_encode(&tex, NULL, 0, 0, tic));
    if (vk_tic_format_3d(tic[0], 0).vk == VK_FORMAT_UNDEFINED) {
        printf("bench sparse volume %s: no 3D images of its format here, skipped\n", name);
        return 0;
    }
    const u32 across = 1664 / (tile[0] * 4), down = 448 / (tile[1] * 4), deep = 1344 / tile[2];
    const u64 tiles = (u64)across * down * deep;
    CHECK(lay.size == tiles * 0x10000);
    /* 6 tiles mapped when it is made, 6 more later, 1 a frame after that: each a page of its own */
    CHECK(mem_map(cpu, 13 * 0x10000, MS_NORMAL, PERM_RW) == 0);
    for (u32 i = 0; i < 13 * 0x10000; i++) GP(cpu)[i] = (u8)(i * 13 + (i >> 16) * 7 + 1);
    u64 at[13];
    for (u32 k = 0; k < 13; k++) at[k] = (((u64)(k * 6 % deep) * down + k % down) * across + k * 5 % across) * 0x10000;
    for (u32 k = 0; k < 6; k++) gmmu_map(gva + at[k], cpu + k * 0x10000, 0x10000);
    printf("bench sparse volume %s: %ux%ux%u, %llu MB, %llu tiles of %ux%ux%u texels, 6 mapped\n", name, tex.d.width,
           tex.d.height, tex.d.depth, (unsigned long long)(lay.size >> 20), (unsigned long long)tiles, tile[0] * 4,
           tile[1] * 4, tile[2]);
    sb_begin(&s);
    vk_lock();
    Surf* t = vk_tex_from_tic(tic);
    vk_unlock();
    sb_end(&s, name, "made");
    CHECK(t);
    for (int i = 0; i < 60; i++) CHECK(sb_frame(tic) == t); /* its checks settle */
    sb_begin(&s);
    for (int i = 0; i < 60; i++) CHECK(sb_frame(tic) == t);
    u64 cpu60 = sb_cpu_ns() - s.cpu0;
    sb_end(&s, name, "60 frames unchanged");
    printf("bench sparse volume %s, a frame unchanged: %.1f us CPU\n", name, cpu60 / 60e3);
    sb_begin(&s);
    for (u32 k = 6; k < 12; k++) gmmu_map(gva + at[k], cpu + k * 0x10000, 0x10000);
    CHECK(sb_frame(tic) == t);
    sb_end(&s, name, "6 tiles mapped");
    for (int i = 0; i < 60; i++) CHECK(sb_frame(tic) == t);
    sb_begin(&s);
    memset(GP(cpu + 3 * 0x10000 + 0x2340), 0x5A, 64);
    CHECK(sb_frame(tic) == t);
    sb_end(&s, name, "the CPU writing 64 bytes of a tile");
    sb_begin(&s);
    gmmu_map(gva + at[12], cpu + 12 * 0x10000, 0x10000);
    CHECK(sb_frame(tic) == t);
    sb_end(&s, name, "1 tile mapped");
    /* a tile unmapped while the GPU is still busy with work submitted before: 32 copies of 64 MiB */
    VkBuffer from, to;
    u64 at_from, at_to;
    vk_lock();
    CHECK(vk_stage_device(64u << 20, 256, &from, &at_from) && vk_stage_device(64u << 20, 256, &to, &at_to));
    for (int i = 0; i < 32; i++) {
        VkBufferCopy c = {at_from, at_to, 64u << 20};
        rec_CmdCopyBuffer(vk_cmd(), from, to, 1, &c);
    }
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    vk_unlock();
    sb_begin(&s);
    gmmu_space_unmap(gva + at[12], 0x10000);
    CHECK(sb_frame(tic) == t);
    sb_end(&s, name, "1 tile unmapped, 2 GiB of copies in flight");
    return 0;
}
static int bench_sparse_volumes(void) {
    sc_install();
    CHECK(!bench_sparse_volume(NVN_FORMAT_RGBA_DXT1, "BC1", 0x600000000ull, HEAP_BASE + 0x1000000));
    CHECK(!bench_sparse_volume(NVN_FORMAT_BPTC_UFLOAT, "BC6H", 0x680000000ull, HEAP_BASE + 0x1100000));
    return 0;
}

/* --bench-sparse-texture: what a 2D sparse texture costs, made through NVN the way a game makes one. First the device
   memory and the time making it takes with a few MiB of its large levels mapped (as a game streams in a region of an
   8192x8192 RGBA8, a 16384x16384 BC7 or an 8192x8192 ASTC 4x4 texture, held as BC7 where the GPU samples BCn and not
   ASTC), with memory only where it is mapped and made whole (the device told it has no sparse residency of 2D images,
   as main had it). Then the GPU's time to read each texel of a 4096x4096 RGBA8 texture once, in order and at hashed
   places, from an image made whole, from one with memory where it is mapped bound tile after tile in order, and from
   one bound in a shuffled order, its blocks' memory away from their neighbours': what the GPU's address translation and
   prefetching cost a texture in many pieces */
#include "common/utilities/spirv_emit.h"
/* tile (x, y) of level l of a sparse 2D texture as a page of its layout: its rows are whole tiles */
static u64 sb_tile(const NvnTexDesc* d, const u32 tile[3], u32 bw, u32 l, u32 x, u32 y) {
    const u32 across = ((d->width >> l) / bw + tile[0] - 1) / tile[0];
    return nvn_tex_level_offset(d, l) / 0x10000 + (u64)y * across + x;
}
static int bench_texture_memory(int format, const char* name, u32 bw, u32 size, u64 gva, u64 cpu, u32 round) {
    NvnTexture tex = {0};
    NvnTexLayout lay;
    u32 tile[3] = {0}, tic[2][8], levels = 0, tiled = 0, n = 0;
    u64 pages[64];
    while (size >> levels) levels++;
    tex.d.target = NVN_TEXTURE_TARGET_2D;
    tex.d.format = (u16)format;
    tex.d.flags = NVN_TEXTURE_FLAGS_SPARSE;
    tex.d.levels = (u8)levels;
    tex.d.width = tex.d.height = size;
    tex.d.depth = 1;
    CHECK(nvn_tex_sparse_tile(&tex.d, tile));
    while (tiled < levels && (size >> tiled) / bw >= tile[0] && (size >> tiled) / bw >= tile[1]) tiled++;
    nvn_tex_layout(&tex.d, &lay);
    for (u32 k = 0; k < 2; k++) {
        tex.gpu = gva + k * 0x100000000ull;
        CHECK(nvn_tic_encode(&tex, NULL, 0, 0, tic[k]));
    }
    if (vk_tic_format(tic[0][0], 0).vk == VK_FORMAT_UNDEFINED) {
        printf("bench sparse texture %s: no images of its format here, skipped\n", name);
        return 0;
    }
    /* a region streamed in: 32 tiles of level 0 (2 MiB), 16 of level 1, 8 of level 2, and the mip tail */
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 8; x++) pages[n++] = sb_tile(&tex.d, tile, bw, 0, 8 + x, 4 + y);
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++) pages[n++] = sb_tile(&tex.d, tile, bw, 1, 4 + x, 2 + y);
    for (u32 y = 0; y < 2; y++)
        for (u32 x = 0; x < 4; x++) pages[n++] = sb_tile(&tex.d, tile, bw, 2, 2 + x, 1 + y);
    pages[n++] = nvn_tex_level_offset(&tex.d, tiled) / 0x10000;
    CHECK(mem_map(cpu, n * 0x10000ull, MS_NORMAL, PERM_RW) == 0);
    for (u32 i = 0; i < n * 0x10000u; i++) GP(cpu)[i] = (u8)(i * 13 + (i >> 16) * 7 + 1);
    for (u32 k = 0; k < 2; k++)
        for (u32 i = 0; i < n; i++)
            gmmu_map(gva + k * 0x100000000ull + pages[i] * 0x10000, cpu + i * 0x10000ull, 0x10000);
    printf("bench sparse texture %s, round %u: %ux%u, %u levels (%u in tiles of %ux%u texels), %llu MB, %u tiles "
           "mapped (%u KiB)\n",
           name, round, size, size, levels, tiled, tile[0] * bw, tile[1] * bw, (unsigned long long)(lay.size >> 20), n,
           n * 64);
    Surf* t[2];
    for (u32 k = 0; k < 2; k++) {
        const VkBool32 had = g_vk.feat.sparseResidencyImage2D;
        u64 uploads, bytes0, bytes1, took0 = rv_allocated(), cpu0 = sb_cpu_ns(), wall0 = plat_ns();
        vk_tex_stats(&uploads, &bytes0);
        vk_lock();
        if (k) g_vk.feat.sparseResidencyImage2D = VK_FALSE;
        t[k] = vk_tex_from_tic(tic[k]);
        g_vk.feat.sparseResidencyImage2D = had;
        u64 cpu1 = sb_cpu_ns();
        vk_finish();
        vk_unlock();
        u64 wall1 = plat_ns(), took1 = rv_allocated();
        vk_tex_stats(&uploads, &bytes1);
        CHECK(t[k]);
        printf("bench sparse texture %s, round %u, %s: its image %llu KiB, %llu KiB more allocated, %llu KiB loaded, "
               "made in %.3f ms CPU, %.3f ms to the GPU's end\n",
               name, round, k ? "made whole" : "memory where it is mapped", (unsigned long long)(t[k]->bytes >> 10),
               (unsigned long long)((i64)(took1 - took0) >> 10), (unsigned long long)((bytes1 - bytes0) >> 10),
               (cpu1 - cpu0) / 1e6, (wall1 - wall0) / 1e6);
    }
    CHECK(!t[1]->bricks && (!t[0]->bricks || t[0]->bytes < t[1]->bytes));
    for (u32 k = 0; k < 2; k++) gmmu_space_unmap(gva + k * 0x100000000ull, lay.size);
    CHECK(mem_unmap(cpu, n * 0x10000ull) == 0);
    return 0;
}

/* a compute program reading one texel of level 0 of a size x size 2D texture an invocation (size x size of them, in
   groups of 64 across), at its own place or at one hashed from it, storing its red channel's bits so the read stays */
static u32* sb_read_program(u32 size, int hashed, size_t* words) {
    SpvEmitter e;
    u32 log2 = 0, sampled_type, io[3];
    while ((1u << log2) < size) log2++;
    spv_emit_init(&e, 64);
    spv_emit_shader_capability(&e);
    const u32 function = spv_emit_id(&e), label = spv_emit_id(&e), void_type = spv_emit_type_void(&e),
              u32_type = spv_emit_type_u32(&e), f32_type = spv_emit_type_f32(&e),
              uvec2 = spv_emit_type_vec2(&e, u32_type), uvec3 = spv_emit_type_vec3(&e, u32_type),
              vec4 = spv_emit_type_vec4(&e, f32_type), function_type = spv_emit_type_function_void(&e, void_type),
              block = spv_emit_type_struct_one(&e, spv_emit_type_runtime_array(&e, u32_type, 4));
    io[0] = spv_emit_storage_buffer(&e, block, 0, 0);
    io[1] = spv_emit_sampler2d(&e, f32_type, 0, 1, 0, &sampled_type);
    io[2] = spv_emit_builtin_input_vec3(&e, uvec3, 28); /* GlobalInvocationId */
    const u32 pointer = spv_emit_type_storage_pointer(&e, u32_type);
    spv_emit_compute_entry_str(&e, function, "main", io, 3);
    spv_emit_local_size(&e, function, 64, 1, 1);
    const u32 zero = spv_emit_constant_u32(&e, u32_type, 0), mask = spv_emit_constant_u32(&e, u32_type, size - 1),
              slots = spv_emit_constant_u32(&e, u32_type, 1023), shift = spv_emit_constant_u32(&e, u32_type, log2),
              mul_x = spv_emit_constant_u32(&e, u32_type, 0x9E3779B1u),
              mul_y = spv_emit_constant_u32(&e, u32_type, 0x85EBCA77u), high = spv_emit_constant_u32(&e, u32_type, 7);
    spv_emit_function_void(&e, void_type, function, function_type);
    spv_emit_label(&e, label);
    const u32 id = spv_emit_load(&e, uvec3, io[2]);
    u32 x = spv_emit_vec4_extract(&e, u32_type, id, 0), y = spv_emit_vec4_extract(&e, u32_type, id, 1);
    if (hashed) { /* invocation i = y * size + x reads at two of its hashes' bits */
        const u32 i = spv_emit_bitwise_or_u32(&e, u32_type, spv_emit_shift_left_u32(&e, u32_type, y, shift), x);
        x = spv_emit_bitwise_and_u32(
            &e, u32_type, spv_emit_shift_right_u32(&e, u32_type, spv_emit_imul_u32(&e, u32_type, i, mul_x), high),
            mask);
        y = spv_emit_bitwise_and_u32(
            &e, u32_type, spv_emit_shift_right_u32(&e, u32_type, spv_emit_imul_u32(&e, u32_type, i, mul_y), high),
            mask);
    }
    const u32 texel =
        spv_emit_image_fetch(&e, vec4, sampled_type, io[1], spv_emit_vec2_construct(&e, uvec2, x, y), zero);
    const u32 red = spv_emit_bitcast(&e, u32_type, spv_emit_vec4_extract(&e, f32_type, texel, 0));
    spv_emit_store_u32(
        &e, spv_emit_storage_access_u32(&e, pointer, io[0], zero, spv_emit_bitwise_and_u32(&e, u32_type, x, slots)),
        red);
    spv_emit_return(&e);
    spv_emit_function_end(&e);
    u32* code = spv_emit_finish(&e, words);
    spv_emit_destroy(&e);
    return code;
}
/* the median of n values (sorted in place) */
static double sb_median(double* v, u32 n) {
    for (u32 i = 1; i < n; i++)
        for (u32 j = i; j && v[j - 1] > v[j]; j--) {
            double s = v[j];
            v[j] = v[j - 1];
            v[j - 1] = s;
        }
    return n & 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}
#define SB_SIZE   4096u
#define SB_ROUNDS 15u
#define SB_REPS   4u
static int bench_texture_reads(void) {
    static const char* const how[3] = {"made whole", "bound in order", "bound shuffled"};
    const u64 gva = 0x700000000ull, cpu = HEAP_BASE + 0x1400000;
    const u32 across = SB_SIZE / 128, tiles = across * across;
    NvnTexture tex = {0};
    NvnTexLayout lay;
    u32 tic[3][8], order[SB_SIZE / 128 * SB_SIZE / 128];
    Surf* t[3];
    tex.d.target = NVN_TEXTURE_TARGET_2D;
    tex.d.format = NVN_FORMAT_RGBA8;
    tex.d.flags = NVN_TEXTURE_FLAGS_SPARSE;
    tex.d.levels = 1;
    tex.d.width = tex.d.height = SB_SIZE;
    tex.d.depth = 1;
    nvn_tex_layout(&tex.d, &lay);
    CHECK(lay.size == (u64)tiles * 0x10000);
    for (u32 k = 0; k < 3; k++) {
        tex.gpu = gva + k * 0x10000000ull;
        CHECK(nvn_tic_encode(&tex, NULL, 0, 0, tic[k]));
    }
    if (!g_vk.props.limits.timestampComputeAndGraphics) {
        puts("bench sparse texture reads: no timestamps on the graphics queue, skipped");
        return 0;
    }
    /* 16 pages behind every tile in turn; the shuffled texture's tiles in an order of their own */
    CHECK(mem_map(cpu, 16 * 0x10000ull, MS_NORMAL, PERM_RW) == 0);
    for (u32 i = 0; i < 16 * 0x10000u; i++) GP(cpu)[i] = (u8)(i * 13 + (i >> 16) * 7 + 1);
    for (u32 i = 0; i < tiles; i++) order[i] = i;
    for (u32 i = tiles - 1, h = 12345; i; i--) {
        h = h * 1103515245u + 12345u;
        u32 j = (h >> 8) % (i + 1), s = order[i];
        order[i] = order[j];
        order[j] = s;
    }
    /* made whole: every tile mapped, no hole. In order: all but the last when made, then the last. Shuffled: one when
       made, then 64 more each frame, the shuffled order's */
    for (u32 i = 0; i < tiles; i++) gmmu_map(gva + i * 0x10000ull, cpu + i % 16 * 0x10000ull, 0x10000);
    for (u32 i = 0; i + 1 < tiles; i++)
        gmmu_map(gva + 0x10000000ull + i * 0x10000ull, cpu + i % 16 * 0x10000ull, 0x10000);
    gmmu_map(gva + 0x20000000ull + order[0] * 0x10000ull, cpu, 0x10000);
    vk_lock();
    for (u32 k = 0; k < 3; k++) t[k] = vk_tex_from_tic(tic[k]);
    vk_unlock();
    CHECK(t[0] && t[1] && t[2]);
    gmmu_map(gva + 0x10000000ull + (tiles - 1) * 0x10000ull, cpu + (tiles - 1) % 16 * 0x10000ull, 0x10000);
    CHECK(sb_frame(tic[1]) == t[1]);
    for (u32 i = 1; i < tiles; i += 64) {
        for (u32 j = i; j < i + 64 && j < tiles; j++)
            gmmu_map(gva + 0x20000000ull + order[j] * 0x10000ull, cpu + order[j] % 16 * 0x10000ull, 0x10000);
        CHECK(sb_frame(tic[2]) == t[2]);
    }
    printf(
        "bench sparse texture reads: %ux%u RGBA8, %u tiles: %s %llu KiB, %s %llu KiB in %u bricks, %s %llu KiB in %u "
        "bricks\n",
        SB_SIZE, SB_SIZE, tiles, how[0], (unsigned long long)(t[0]->bytes >> 10), how[1],
        (unsigned long long)(t[1]->bytes >> 10), t[1]->bricks, how[2], (unsigned long long)(t[2]->bytes >> 10),
        t[2]->bricks);
    if (!t[1]->bricks || !t[2]->bricks) {
        puts("bench sparse texture reads: the textures with holes were made whole here, skipped");
        return 0;
    }
    /* the programs, one storage buffer, a view and a set for each texture, two timestamps */
    VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dli = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings};
    VkDescriptorSetLayout dlayout;
    VkPipelineLayout playout;
    VkPipeline pipeline[2];
    CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &dli, NULL, &dlayout) == VK_SUCCESS);
    VkPipelineLayoutCreateInfo pli = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dlayout};
    CHECK(vkCreatePipelineLayout(g_vk.dev, &pli, NULL, &playout) == VK_SUCCESS);
    for (u32 v = 0; v < 2; v++) {
        size_t words = 0;
        u32* code = sb_read_program(SB_SIZE, (int)v, &words);
        VkShaderModuleCreateInfo smi = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = words * sizeof(u32), .pCode = code};
        VkShaderModule module;
        CHECK(code && vkCreateShaderModule(g_vk.dev, &smi, NULL, &module) == VK_SUCCESS);
        free(code);
        VkComputePipelineCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                           .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                     .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                     .module = module,
                                                     .pName = "main"},
                                           .layout = playout};
        CHECK(vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &pci, NULL, &pipeline[v]) == VK_SUCCESS);
        vkDestroyShaderModule(g_vk.dev, module, NULL);
    }
    VkBufferCreateInfo bi = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VmaAllocationCreateInfo bai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkBuffer out;
    VmaAllocation out_memory;
    CHECK(vmaCreateBuffer(g_vk.vma, &bi, &bai, &out, &out_memory, NULL) == VK_SUCCESS);
    VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                               .magFilter = VK_FILTER_NEAREST,
                               .minFilter = VK_FILTER_NEAREST,
                               .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkSampler sampler;
    CHECK(vkCreateSampler(g_vk.dev, &sci, NULL, &sampler) == VK_SUCCESS);
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3},
                                     {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3}};
    VkDescriptorPoolCreateInfo dpi = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 3, .poolSizeCount = 2, .pPoolSizes = sizes};
    VkDescriptorPool pool;
    VkDescriptorSet set[3];
    VkImageView view[3];
    VkDescriptorSetLayout layouts[3] = {dlayout, dlayout, dlayout};
    CHECK(vkCreateDescriptorPool(g_vk.dev, &dpi, NULL, &pool) == VK_SUCCESS);
    VkDescriptorSetAllocateInfo dsi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = pool,
                                       .descriptorSetCount = 3,
                                       .pSetLayouts = layouts};
    CHECK(vkAllocateDescriptorSets(g_vk.dev, &dsi, set) == VK_SUCCESS);
    for (u32 k = 0; k < 3; k++) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                    .image = t[k]->img,
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                    .format = VK_FORMAT_R8G8B8A8_UNORM,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &view[k]) == VK_SUCCESS);
        VkDescriptorBufferInfo dbi = {out, 0, 4096};
        VkDescriptorImageInfo dii = {sampler, view[k], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .dstSet = set[k],
                                      .dstBinding = 0,
                                      .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      .pBufferInfo = &dbi},
                                     {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .dstSet = set[k],
                                      .dstBinding = 1,
                                      .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      .pImageInfo = &dii}};
        vkUpdateDescriptorSets(g_vk.dev, 2, w, 0, NULL);
    }
    VkQueryPoolCreateInfo qpi = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2};
    VkQueryPool queries;
    CHECK(vkCreateQueryPool(g_vk.dev, &qpi, NULL, &queries) == VK_SUCCESS);
    /* each round reads with every texture, their order turning from round to round; each read SB_REPS passes */
    double ms[2][3][SB_ROUNDS];
    for (u32 round = 0; round < SB_ROUNDS; round++)
        for (u32 v = 0; v < 2; v++)
            for (u32 j = 0; j < 3; j++) {
                const u32 k = (j + round) % 3;
                u64 stamp[2] = {0};
                vk_lock();
                vk_render_end();
                VkCommandBuffer cb = vk_cmd();
                vk_surf_use(cb, t[k], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                rec_CmdResetQueryPool(cb, queries, 0, 2);
                rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline[v]);
                rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, playout, 0, 1, &set[k], 0, NULL);
                rec_CmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, 0);
                for (u32 r = 0; r < SB_REPS; r++) rec_CmdDispatch(cb, SB_SIZE / 64, SB_SIZE, 1);
                rec_CmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, 1);
                vk_finish();
                vk_unlock();
                CHECK(vkGetQueryPoolResults(g_vk.dev, queries, 0, 2, sizeof stamp, stamp, 8,
                                            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS);
                ms[v][k][round] = (double)(stamp[1] - stamp[0]) * g_vk.props.limits.timestampPeriod / 1e6;
            }
    for (u32 v = 0; v < 2; v++) {
        double m[3];
        for (u32 k = 0; k < 3; k++) m[k] = sb_median(ms[v][k], SB_ROUNDS); /* sorted: the least first, the most last */
        printf("bench sparse texture reads, %s: %u reads of each of its %u texels, medians of %u rounds (least, most): "
               "%s %.3f ms (%.3f, %.3f), %s %.3f ms (%+.1f%%; %.3f, %.3f), %s %.3f ms (%+.1f%%; %.3f, %.3f)\n",
               v ? "at hashed places" : "in order", SB_REPS, SB_SIZE * SB_SIZE, SB_ROUNDS, how[0], m[0], ms[v][0][0],
               ms[v][0][SB_ROUNDS - 1], how[1], m[1], (m[1] / m[0] - 1) * 100, ms[v][1][0], ms[v][1][SB_ROUNDS - 1],
               how[2], m[2], (m[2] / m[0] - 1) * 100, ms[v][2][0], ms[v][2][SB_ROUNDS - 1]);
    }
    vkDestroyQueryPool(g_vk.dev, queries, NULL);
    for (u32 k = 0; k < 3; k++) vkDestroyImageView(g_vk.dev, view[k], NULL);
    vkDestroyDescriptorPool(g_vk.dev, pool, NULL);
    vkDestroySampler(g_vk.dev, sampler, NULL);
    vmaDestroyBuffer(g_vk.vma, out, out_memory);
    for (u32 v = 0; v < 2; v++) vkDestroyPipeline(g_vk.dev, pipeline[v], NULL);
    vkDestroyPipelineLayout(g_vk.dev, playout, NULL);
    vkDestroyDescriptorSetLayout(g_vk.dev, dlayout, NULL);
    for (u32 k = 0; k < 3; k++) gmmu_space_unmap(gva + k * 0x10000000ull, lay.size);
    CHECK(mem_unmap(cpu, 16 * 0x10000ull) == 0);
    return 0;
}
static int bench_sparse_textures(void) {
    sc_install();
    /* twice each: the first texture of a run also grows the staging and deswizzle buffers */
    for (u32 round = 1; round <= 2; round++) {
        const u64 at = 0x1000000000ull * round;
        CHECK(!bench_texture_memory(NVN_FORMAT_RGBA8, "RGBA8", 1, 8192, at, HEAP_BASE + 0x1200000, round));
        CHECK(!bench_texture_memory(NVN_FORMAT_BPTC_UNORM, "BC7", 4, 16384, at + 0x400000000ull, HEAP_BASE + 0x1200000,
                                    round));
        const int had = g_settings.astc_bc;
        g_settings.astc_bc = 1; /* held as BC7 where the GPU samples BCn and not ASTC */
        const int failed = bench_texture_memory(NVN_FORMAT_RGBA_ASTC_4x4, "ASTC 4x4", 4, 8192, at + 0x800000000ull,
                                                HEAP_BASE + 0x1200000, round);
        g_settings.astc_bc = had;
        CHECK(!failed);
    }
    CHECK(!bench_texture_reads());
    return 0;
}
