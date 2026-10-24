/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Unified memory's texture check: a texture is a cache of guest memory that the GPU checks and rebuilds itself, in
   order with the draws (gpu/texture/texcheck.comp). No CPU hash and no write tracking: the check reads the texture's
   guest bytes where they are, through the import of guest memory (uma.c), and compares them with what they were at
   its last check, a 64 KiB piece at a time.

   The rebuild is recorded as an upload always is (texture.c), with the gate open (vk.h's VkGate): each of its
   dispatches is recorded indirect, and each copy into the image an indirect copy (VK_KHR_copy_memory_indirect, else
   VK_NV_copy_memory_indirect), whose arguments the check's second dispatch writes: those the CPU recorded when the
   texture changed, and no work when it didn't (no workgroups, a copy no texel wide, which both extensions allow). A
   device with neither stores what each copy would with a dispatch (texstore.comp), into a view of the image's level
   in an unsigned format of the unit's size, the image made able to take it (vk_texcheck_prepare).

   The arguments the CPU recorded are written into staging once the rebuild is recorded and before the submit, which
   makes them visible to the GPU. A rebuild with more slots of arguments than were made room for (gate_room) runs the
   rest as it is: a
   rebuild of a texture that didn't change gives the same texels, so that is slower, never wrong. */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/texture/texcheck_spv.h" /* compiled from texcheck.comp and texstore.comp (cmake/glsl.cmake) */
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/gpu/vk/vk.h"

#define PIECE       (64ull << 10) /* texcheck.comp's PIECE_WORDS, in bytes */
#define GATE_SLOTS  512u          /* the most 16 byte slots of a rebuild's arguments a check makes room for */
#define COPY_SLOTS  4u            /* a copy's command (VkCopyMemoryToImageIndirectCommandKHR) padded to 64 bytes */
#define COPY_STRIDE (COPY_SLOTS * 16u)

/* texcheck.comp's and texstore.comp's push constants, member for member */
typedef struct {
    u32 mode, bias, words, force;
} CheckParams;
typedef struct {
    u32 src_byte, row_units, rows_per_slice, bpp;
    i32 off[4];
    u32 ext[4];
} StoreParams;

static struct {
    VkDescriptorSetLayout check_ds, store_ds;
    VkPipelineLayout check_layout, store_layout;
    VkPipeline check, store, store3d;
    /* the gate of the check being recorded: the arguments for the texture changed (on) and not (off), in staging,
       followed by the header the second dispatch reads, and where it writes the ones the rebuild takes */
    u8* in;
    VkBuffer out;
    u64 out_off;
    VkDeviceAddress out_at;
    u32 n, room;
    VkGate gate;
    u64 checks, bytes, copies, stores, overflow;
} g_tc;

static VkShaderModule module(const u32* code, u64 size) {
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    VkShaderModule m;
    if (!VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &m))) fatal("vulkan: cannot load the texture check shaders");
    return m;
}
static VkPipeline pipeline(VkPipelineLayout layout, const u32* code, u64 size) {
    VkShaderModule m = module(code, size);
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = m,
                                                .pName = "main"},
                                      .layout = layout};
    VkPipeline p;
    if (!VK_OK(vkCreateComputePipelines(g_vk.dev, vk_pipeline_cache(), 1, &ci, NULL, &p)))
        fatal("vulkan: cannot create the texture check pipelines");
    vkDestroyShaderModule(g_vk.dev, m, NULL);
    return p;
}
static void layout(const VkDescriptorSetLayoutBinding* b, u32 n, u32 push, VkDescriptorSetLayout* ds,
                   VkPipelineLayout* pl) {
    VkDescriptorSetLayoutCreateInfo di = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = n, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, push};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = ds,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &pr};
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &di, NULL, ds)) ||
        !VK_OK(vkCreatePipelineLayout(g_vk.dev, &li, NULL, pl)))
        fatal("vulkan: cannot create the texture check layouts");
}
static void tc_init(void) {
    if (g_tc.check) return;
    const VkShaderStageFlags cs = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutBinding cb[4] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, cs, NULL},
                                          {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, cs, NULL},
                                          {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, cs, NULL},
                                          {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, cs, NULL}};
    VkDescriptorSetLayoutBinding sb[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, cs, NULL},
                                          {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, cs, NULL}};
    layout(cb, 4, sizeof(CheckParams), &g_tc.check_ds, &g_tc.check_layout);
    g_tc.check = pipeline(g_tc.check_layout, texcheck_spv, texcheck_spv_size);
    if (g_vk.copy_indirect) return;
    layout(sb, 2, sizeof(StoreParams), &g_tc.store_ds, &g_tc.store_layout);
    g_tc.store = pipeline(g_tc.store_layout, texstore_spv, texstore_spv_size);
    g_tc.store3d = pipeline(g_tc.store_layout, texstore3d_spv, texstore3d_spv_size);
}

/* ------------------------------------------------------------ the image's format, for the stores */
/* the bytes of the image format's unit (a texel, or a block of a compressed format) and the block's texels across and
   down: 0 for a format whose bits a store can't take as they are (depth, stencil) */
static u32 format_unit(VkFormat f, u32* bw, u32* bh) {
    static const u8 astc[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5},  {6, 6},   {8, 5},   {8, 6},
                                   {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    *bw = *bh = 1;
    if (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_EAC_R11G11_SNORM_BLOCK) {
        *bw = *bh = 4;
        switch (f) {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC4_UNORM_BLOCK:
        case VK_FORMAT_BC4_SNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
        case VK_FORMAT_EAC_R11_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11_SNORM_BLOCK: return 8;
        default: return 16;
        }
    }
    if (f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) {
        u32 i = (u32)(f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) / 2;
        *bw = astc[i][0];
        *bh = astc[i][1];
        return 16;
    }
    switch (f) {
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8_SNORM:
    case VK_FORMAT_R8_UINT:
    case VK_FORMAT_R8_SINT: return 1;
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R8G8_SNORM:
    case VK_FORMAT_R8G8_UINT:
    case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16_SNORM:
    case VK_FORMAT_R16_UINT:
    case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R5G6B5_UNORM_PACK16:
    case VK_FORMAT_B5G6R5_UNORM_PACK16:
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16: return 2;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SNORM:
    case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
    case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R16G16_SNORM:
    case VK_FORMAT_R16G16_UINT:
    case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R32_SINT:
    case VK_FORMAT_R32_SFLOAT: return 4;
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R32G32_SFLOAT: return 8;
    case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: return 0;
    }
}
/* the unsigned format a store writes a unit of that many bytes as */
static VkFormat store_format(u32 bytes) {
    switch (bytes) {
    case 1: return VK_FORMAT_R8_UINT;
    case 2: return VK_FORMAT_R16_UINT;
    case 4: return VK_FORMAT_R32_UINT;
    case 8: return VK_FORMAT_R32G32_UINT;
    case 16: return VK_FORMAT_R32G32B32A32_UINT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

int vk_texcheck_prepare(VkImageCreateInfo* ii, VkViewFormats* vf) {
    u32 bw, bh, bytes;
    /* a discrete GPU's texture checks stay the CPU's (no texels across PCIe), and so do all of them when the setting
       asks */
    if (!g_vk.uma_shared || !g_settings.uma_texture_checks) return 0;
    if (g_vk.copy_indirect) return 1; /* copies, which any image a texture is made as takes */
    bytes = format_unit(ii->format, &bw, &bh);
    VkFormat sf = store_format(bytes);
    VkFormatProperties fp;
    if (sf == VK_FORMAT_UNDEFINED || (ii->imageType == VK_IMAGE_TYPE_3D && bw > 1)) return 0;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, sf, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) return 0;
    /* stored through a view of the unsigned format: of the same size, or of a compressed format's block */
    VkImageCreateInfo t = *ii;
    VkViewFormats keep = *vf;
    t.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    t.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT |
               (bw > 1 ? VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT : 0);
    if (t.pNext == &vf->list) { /* the list of the formats it is viewed as takes this one too */
        vf->f[vf->list.viewFormatCount] = sf;
        vf->list.viewFormatCount++;
    }
    VkPhysicalDeviceImageFormatInfo2 fi = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
                                           .pNext = t.pNext,
                                           .format = t.format,
                                           .type = t.imageType,
                                           .tiling = t.tiling,
                                           .usage = t.usage,
                                           .flags = t.flags};
    VkImageFormatProperties2 fo = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    if (vkGetPhysicalDeviceImageFormatProperties2(g_vk.phys, &fi, &fo) != VK_SUCCESS ||
        fo.imageFormatProperties.maxMipLevels < t.mipLevels || fo.imageFormatProperties.maxArrayLayers < t.arrayLayers) {
        *vf = keep;
        return 0;
    }
    *ii = t;
    return 1;
}

/* the view a store writes level of t through, made the first time: one for the level's layers, or for a compressed
   format one a layer (a block-texel view takes one level and one layer) */
static VkImageView store_view(Surf* t, u32 level, u32 layer, int per_layer) {
    u32 bw, bh, n = t->levels * (per_layer ? t->layers : 1), i = level * (per_layer ? t->layers : 1) + layer;
    if (!t->stv && !(t->stv = calloc(n, sizeof *t->stv))) return VK_NULL_HANDLE;
    t->nstv = n;
    if (t->stv[i]) return t->stv[i];
    int is3d = t->type == VK_IMAGE_VIEW_TYPE_3D;
    VkImageViewUsageCreateInfo use = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                      .usage = VK_IMAGE_USAGE_STORAGE_BIT};
    VkImageViewCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = &use,
        .image = t->img,
        .viewType = is3d ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D_ARRAY,
        .format = store_format(format_unit(t->f.vk, &bw, &bh)),
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, per_layer ? layer : 0, per_layer ? 1 : t->layers}};
    if (!VK_OK(vkCreateImageView(g_vk.dev, &vi, NULL, &t->stv[i]))) t->stv[i] = VK_NULL_HANDLE;
    return t->stv[i];
}
void vk_texcheck_free(Surf* t) {
    for (u32 i = 0; i < t->nstv; i++)
        if (t->stv[i]) vk_defer_image(VK_NULL_HANDLE, VK_NULL_HANDLE, t->stv[i], VK_NULL_HANDLE);
    free(t->stv);
    t->stv = NULL;
    t->nstv = 0;
    if (t->chk_buf) vk_defer_buffer(t->chk_buf, t->chk_mem);
    t->chk_buf = VK_NULL_HANDLE;
}

/* ------------------------------------------------------------ the gate */
/* k free slots of the gate's arguments, at their index, or ~0u when the gate is full */
static u32 slots(u32 k) {
    if (g_tc.n + k > g_tc.room) {
        g_tc.overflow++;
        return ~0u;
    }
    g_tc.n += k;
    return g_tc.n - k;
}
static u32* slot_on(u32 i) { return (u32*)(g_tc.in + 16 + (u64)i * 16); }
static u32* slot_off(u32 i) { return (u32*)(g_tc.in + 16 + ((u64)g_tc.room + i) * 16); }
/* the slots t's rebuild takes, about: a level of a layer is a deswizzle, a conversion and a copy (four slots) or a
   store, and a level past the staging band a few strips more. Every check took room for GATE_SLOTS, 24 KiB of staging
   each, hundreds of them a frame: the staging ring grew by megabytes a frame, kept for hundreds of frames */
static u32 gate_room(const Surf* t) {
    u64 subs = (u64)t->levels * (t->type == VK_IMAGE_VIEW_TYPE_3D ? 1 : t->layers), room = subs * 8 + 16;
    return room > GATE_SLOTS ? GATE_SLOTS : (u32)room;
}
static void gate_dispatch(void* ctx, VkCommandBuffer cb, u32 x, u32 y, u32 z) {
    u32 i = slots(1);
    (void)ctx;
    if (i == ~0u) {
        rec_CmdDispatch(cb, x, y, z);
        return;
    }
    u32 on[4] = {x, y, z, 0}, off[4] = {0, 0, 0, 0};
    memcpy(slot_on(i), on, 16);
    memcpy(slot_off(i), off, 16);
    rec_CmdDispatchIndirect(cb, g_tc.out, g_tc.out_off + (u64)i * 16);
}
static VkDeviceAddress buffer_address(VkBuffer b) {
    VkBufferDeviceAddressInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b};
    return vkGetBufferDeviceAddress(g_vk.dev, &bi);
}
/* regions of src copied into t's image by copies the check turns off: the commands for when it changed are the
   regions', those for when it didn't the same no texel wide */
static void copy_indirect(VkCommandBuffer cb, Surf* t, VkBuffer src, u32 n, const VkBufferImageCopy* r) {
    VkDeviceAddress at = buffer_address(src);
    VkImageSubresourceLayers sub[64];
    vk_all_barrier(cb); /* what the rebuild wrote before the copies read it, in whichever stage they do */
    for (u32 i = 0; i < n;) {
        u32 k = n - i < 64 ? n - i : 64, first = slots(k * COPY_SLOTS);
        if (first == ~0u) {
            rec_CmdCopyBufferToImage(cb, src, t->img, VK_IMAGE_LAYOUT_GENERAL, n - i, r + i);
            return;
        }
        for (u32 j = 0; j < k; j++) {
            const VkBufferImageCopy* c = &r[i + j];
            VkCopyMemoryToImageIndirectCommandKHR cmd = {.srcAddress = at + c->bufferOffset,
                                                         .bufferRowLength = c->bufferRowLength,
                                                         .bufferImageHeight = c->bufferImageHeight,
                                                         .imageSubresource = c->imageSubresource,
                                                         .imageOffset = c->imageOffset,
                                                         .imageExtent = c->imageExtent};
            u32 s = first + j * COPY_SLOTS;
            memset(slot_on(s), 0, COPY_STRIDE);
            memset(slot_off(s), 0, COPY_STRIDE);
            memcpy(slot_on(s), &cmd, sizeof cmd);
            cmd.imageExtent.width = 0;
            memcpy(slot_off(s), &cmd, sizeof cmd);
            sub[j] = c->imageSubresource;
        }
        rec_CmdCopyMemoryToImageIndirect(cb, g_tc.out_at + (u64)first * 16, k, COPY_STRIDE, t->img,
                                         VK_IMAGE_LAYOUT_GENERAL, sub);
        g_tc.copies += k;
        i += k;
    }
}
/* the same with dispatches that store what the copies would (texstore.comp) */
static void copy_stored(VkCommandBuffer cb, Surf* t, VkBuffer src, u32 n, const VkBufferImageCopy* r) {
    const VkPhysicalDeviceLimits* lim = &g_vk.props.limits;
    u64 al = lim->minStorageBufferOffsetAlignment < 4 ? 4 : lim->minStorageBufferOffsetAlignment;
    u32 bw, bh, bpp = format_unit(t->f.vk, &bw, &bh);
    int is3d = t->type == VK_IMAGE_VIEW_TYPE_3D, per_layer = bw > 1;
    vk_all_barrier(cb);
    for (u32 i = 0; i < n; i++) {
        const VkBufferImageCopy* c = &r[i];
        u32 w = (c->imageExtent.width + bw - 1) / bw, h = (c->imageExtent.height + bh - 1) / bh;
        u32 row = ((c->bufferRowLength ? c->bufferRowLength : c->imageExtent.width) + bw - 1) / bw;
        u32 rows = ((c->bufferImageHeight ? c->bufferImageHeight : c->imageExtent.height) + bh - 1) / bh;
        u32 slices = is3d ? c->imageExtent.depth : c->imageSubresource.layerCount;
        u64 slice = (u64)row * rows * bpp;
        for (u32 l = 0; l < (per_layer ? slices : 1); l++) {
            u64 at = c->bufferOffset + (per_layer ? l * slice : 0), base = at & ~(al - 1);
            u32 z = per_layer ? 1 : slices;
            VkImageView v = store_view(t, c->imageSubresource.mipLevel, c->imageSubresource.baseArrayLayer + l,
                                       per_layer);
            VkDescriptorSet set = v ? vk_desc_alloc(g_tc.store_ds) : VK_NULL_HANDLE;
            if (!set) {
                LOG_LIMIT(1, "vulkan: a texture's rebuild couldn't store a %ux%u region (left out)\n", w, h);
                continue;
            }
            VkDescriptorBufferInfo bi = {src, base, at - base + slice * z};
            VkDescriptorImageInfo ii = {VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet wr[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = set,
                                           .dstBinding = 0,
                                           .descriptorCount = 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                           .pBufferInfo = &bi},
                                          {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = set,
                                           .dstBinding = 1,
                                           .descriptorCount = 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                           .pImageInfo = &ii}};
            vkUpdateDescriptorSets(g_vk.dev, 2, wr, 0, NULL);
            StoreParams p = {.src_byte = (u32)(at - base),
                             .row_units = row,
                             .rows_per_slice = rows,
                             .bpp = bpp,
                             .off = {c->imageOffset.x / (i32)bw, c->imageOffset.y / (i32)bh,
                                     is3d ? c->imageOffset.z : per_layer ? 0 : (i32)c->imageSubresource.baseArrayLayer,
                                     0},
                             .ext = {w, h, z, 0}};
            rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, is3d ? g_tc.store3d : g_tc.store);
            rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_tc.store_layout, 0, 1, &set, 0, NULL);
            rec_CmdPushConstants(cb, g_tc.store_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
            gate_dispatch(NULL, cb, (w + 7) / 8, (h + 7) / 8, z);
            g_tc.stores++;
        }
    }
}
static void gate_copy(void* ctx, VkCommandBuffer cb, Surf* t, VkBuffer src, u32 n, const VkBufferImageCopy* r) {
    (void)ctx;
    if (g_vk.copy_indirect)
        copy_indirect(cb, t, src, n, r);
    else
        copy_stored(cb, t, src, n, r);
}

/* ------------------------------------------------------------ the check */
int vk_texcheck_begin(Surf* t, int force) {
    const VkPhysicalDeviceLimits* lim = &g_vk.props.limits;
    VkBuffer src, in;
    u64 at, in_off, words = (t->size + 3) / 4, pieces = (t->size + PIECE - 1) / PIECE;
    if (!t->gpu_check || vk_gate_open() || !t->size || pieces > lim->maxComputeWorkGroupCount[0]) return 0;
    /* its bytes read in place, after what shaders wrote there in place (uma.c) */
    if (!vk_uma_read(t->gva, t->size, 4, &src, &at)) return 0;
    tc_init();
    u64 al = lim->minStorageBufferOffsetAlignment < 4 ? 4 : lim->minStorageBufferOffsetAlignment, base = at & ~(al - 1);
    u64 state = 16 + pieces * 8;
    if (at - base + words * 4 > lim->maxStorageBufferRange) return 0;
    if (!t->chk_buf) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                 .size = state,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (!VK_OK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &t->chk_buf, &t->chk_mem, NULL))) {
            t->chk_buf = VK_NULL_HANDLE;
            return 0;
        }
        force = 1; /* no hashes kept yet */
    }
    /* the arguments: header, on, off, as the CPU writes them, and the ones the GPU picks */
    g_tc.room = gate_room(t);
    g_tc.in = vk_stage(16 + 2ull * g_tc.room * 16, vk_stage_align(), &in, &in_off);
    u8* out = g_tc.in ? vk_stage((u64)g_tc.room * 16, vk_stage_align(), &g_tc.out, &g_tc.out_off) : NULL;
    VkDescriptorSet set = out ? vk_desc_alloc(g_tc.check_ds) : VK_NULL_HANDLE;
    if (!set) return 0;
    memset(g_tc.in, 0, 16);
    g_tc.out_at = g_vk.copy_indirect ? buffer_address(g_tc.out) + g_tc.out_off : 0;
    g_tc.n = 0;
    VkDescriptorBufferInfo bi[4] = {{src, base, at - base + words * 4},
                                    {t->chk_buf, 0, state},
                                    {in, in_off, 16 + 2ull * g_tc.room * 16},
                                    {g_tc.out, g_tc.out_off, (u64)g_tc.room * 16}};
    VkWriteDescriptorSet wr[4];
    for (u32 i = 0; i < 4; i++)
        wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = set,
                                       .dstBinding = i,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .pBufferInfo = &bi[i]};
    vkUpdateDescriptorSets(g_vk.dev, 4, wr, 0, NULL);
    vk_render_end();
    VkCommandBuffer cb = vk_cmd();
    CheckParams p = {0, (u32)((at - base) / 4), (u32)words, (u32)(force != 0)};
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_tc.check);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_tc.check_layout, 0, 1, &set, 0, NULL);
    rec_CmdPushConstants(cb, g_tc.check_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    rec_CmdDispatch(cb, (u32)pieces, 1, 1);
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &d);
    p.mode = 1;
    rec_CmdPushConstants(cb, g_tc.check_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    rec_CmdDispatch(cb, 1, 1, 1);
    /* the arguments before the dispatches and copies that read them */
    b.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                     (g_vk.copy_indirect == 1 ? VK_PIPELINE_STAGE_2_COPY_INDIRECT_BIT_KHR : 0);
    b.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    rec_CmdPipelineBarrier2(cb, &d);
    g_tc.gate = (VkGate){.ctx = NULL, .dispatch = gate_dispatch, .copy = gate_copy};
    vk_gate_set(&g_tc.gate);
    g_tc.checks++;
    g_tc.bytes += t->size;
    return 1;
}
void vk_texcheck_end(void) {
    memcpy(g_tc.in, &g_tc.n, 4); /* the slots the rebuild's arguments took, before the submit */
    memcpy(g_tc.in + 4, &g_tc.room, 4);
    vk_gate_set(NULL);
}
void vk_texcheck_stats(VkTexCheckStats* st) {
    st->checks = g_tc.checks;
    st->bytes = g_tc.bytes;
    st->copies = g_tc.copies;
    st->stores = g_tc.stores;
    st->overflow = g_tc.overflow;
}
