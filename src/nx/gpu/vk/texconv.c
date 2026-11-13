/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Texture formats the host can't take as they are, converted on the GPU (gpu/texture/texconv.comp) from linear rows
   into rows the copies into the image take. One pipeline for every format, made on first use and kept for the
   device's lifetime, like the other internal pipelines; a second, the shader's ETC2 build, for BCn blocks encoded again
   as ETC2 and EAC (gpu/texture/etc2_encode.glsl), its ASTC build for those encoded again as ASTC 4x4, its
   conversion build for the blits that convert between pixel formats (TC_BLIT_CONVERT) and its NV12 build for a CPU
   decoded picture's chroma (TC_NV12_UV), each made the first time it is used. Uses are on g_vk.gfx, under vk_lock() */
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/ledger.h"
#include "nx/gpu/vk/descriptors.h"
#include "nx/gpu/texture/texconv_spv.h" /* compiled from texconv.comp by the build (cmake/glsl.cmake) */
#include "nx/gpu/texture/texconv_modes.glsl"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

/* texconv.comp's push constants, member for member */
typedef struct {
    u32 mode, units, rows, in_pitch, out_pitch, st_pitch, in_bias, out_bias, st_bias, zfloat;
    u32 remap, const_a, const_b, line_out;
} TcParams;

static VkDescriptorSetLayout g_tc_ds;
static VkPipelineLayout g_tc_layout;
/* texconv.comp's builds: the plain one, its ETC2 build, its ASTC build's three passes, its conversion build and its
   NV12 build */
static const struct {
    const u32* code;
    const u64* size;
    const char* what;
} tc_builds[7] = {{texconv_spv, &texconv_spv_size, ""},
                  {texconv_etc2_spv, &texconv_etc2_spv_size, " (its ETC2 build)"},
                  {texconv_astc_spv, &texconv_astc_spv_size, " (its ASTC build)"},
                  {texconv_astc_b_spv, &texconv_astc_b_spv_size, " (its ASTC build's second pass)"},
                  {texconv_astc_c_spv, &texconv_astc_c_spv_size, " (its ASTC build's third pass)"},
                  {texconv_convert_spv, &texconv_convert_spv_size, " (its conversion build)"},
                  {texconv_nv12_spv, &texconv_nv12_spv_size, " (its NV12 build)"}};
static VkPipeline g_tc_pipe[7]; /* tc_builds' pipelines, made on first use */
/* the ASTC build's work lists (texconv.comp's Work): the blocks each pass leaves to a later one, two lists of a word a
   block after a head of four words each, as large as the largest dispatch yet */
static VkBuffer g_tc_work;
static DevMem g_tc_work_mem;
static u64 g_tc_work_size;
#define TC_WORK_HEAD 32u

/* mode's build in tc_builds: the ETC2 or the ASTC build for the BCn modes they encode again (the ASTC build's pass
   `pass`, 0 to 2), the conversion build for TC_BLIT_CONVERT, the NV12 build for TC_NV12_UV, else the plain one */
static int tc_build(u32 mode, int pass) {
    return mode >= TC_BCN_ASTC       ? 2 + pass
           : mode >= TC_BCN_ETC2     ? 1
           : mode == TC_BLIT_CONVERT ? 5
           : mode == TC_NV12_UV      ? 6
                                     : 0;
}
u64 vk_tc_spirv_bytes(u32 mode) { return *tc_builds[tc_build(mode, 0)].size; }
/* the pipeline of mode's build (tc_build), made on first use */
static VkPipeline tc_init(u32 mode, int pass) {
    int build = tc_build(mode, pass);
    VkPipeline* pipe = &g_tc_pipe[build];
    if (*pipe) return *pipe;
    /* the third buffer is the ASTC build's work list; the other conversions name two */
    if (!g_tc_layout && !VK_OK(snx_vk_compute_layouts(g_vk.dev, 3, 0, sizeof(TcParams), &g_tc_ds, &g_tc_layout)))
        fatal("vulkan: cannot create the texture conversion descriptor and pipeline layouts");
    VkShaderModule module;
    if (!VK_OK(snx_vk_shader_module(g_vk.dev, tc_builds[build].code, *tc_builds[build].size, &module)))
        fatal("vulkan: cannot load the texture conversion shader%s", tc_builds[build].what);
    u64 started = plat_ns();
    if (!VK_OK(snx_vk_module_pipeline(g_vk.dev, vk_pipeline_cache(), g_tc_layout, module, NULL, pipe)))
        fatal("vulkan: cannot create the texture conversion compute pipeline%s", tc_builds[build].what);
    vk_texture_pipeline_made(plat_ns() - started);
    vkDestroyShaderModule(g_vk.dev, module, NULL);
    return *pipe;
}
/* the work list with room for blocks blocks: 0 when no memory was found for a larger one */
static int tc_work(u64 blocks) {
    const u64 size = TC_WORK_HEAD + blocks * 8;
    if (size <= g_tc_work_size) return 1;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = size < (1u << 20) ? 1u << 20 : size,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    DevMemWant ai = {.at = DM_DEVICE};
    VkBuffer buf;
    DevMem mem;
    if (bi.size > g_vk.props.limits.maxStorageBufferRange ||
        !VK_OK(vk_buffer_create(&bi, &ai, &buf, &mem, NULL)))
        return 0;
    vk_mem_track(mem, VK_OWN_CONVERT);
    if (g_tc_work) vk_defer_buffer(g_tc_work, g_tc_work_mem);
    g_tc_work = buf;
    g_tc_work_mem = mem;
    g_tc_work_size = bi.size;
    return 1;
}

/* the block formats BCn is encoded again into where the GPU samples ETC2 and not BCn (format.c) */
static int is_etc2(VkFormat f) {
    return f >= VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK && f <= VK_FORMAT_EAC_R11G11_SNORM_BLOCK;
}

/* the shader's mode for a format's conversion */
static u32 tc_mode(const VkFmt* f) {
    switch (f->conv) {
        case VKC_RGBA4: return TC_RGBA4;
        case VKC_RGB5A1: return TC_RGB5A1;
        case VKC_A5B5G5R1: return TC_A5B5G5R1;
        case VKC_RGB32F: return TC_RGB32F;
        case VKC_D24S8: return TC_D24S8;
        case VKC_S8D24: return TC_S8D24;
        case VKC_D24X8: return TC_D24X8;
        case VKC_D32S8: return TC_D32S8;
        case VKC_NONE: return f->bpp == 8 ? TC_D32S8 : TC_D24S8; /* a depth and stencil format as it is in memory */
    }
    /* the ETC2 and EAC kinds, and the BCn ones, each in the same order; BCn into ETC2 or EAC blocks is the ETC2 build's */
    if (f->conv >= VKC_BC1)
        return (f->vk == VK_FORMAT_ASTC_4x4_UNORM_BLOCK || f->vk == VK_FORMAT_ASTC_4x4_SRGB_BLOCK ? TC_BCN_ASTC
                : is_etc2(f->vk)                                                                     ? TC_BCN_ETC2
                                                                                                     : TC_BC1) +
               (f->conv - VKC_BC1);
    return TC_ETC2_RGB + (f->conv - VKC_ETC2);
}

int vk_tc_converts(const VkFmt* f) { return f->conv == VKC_NONE ? f->depth && f->stencil : f->conv != VKC_ASTC; }

/* The BCn blocks one submission encodes again at most (VkTcShape's submit_units). A submission is one job of the
   kernel's GPU scheduler, which ends a job that runs past its timeout, 2 s in kgsl (adreno_drawobj_timeout), and its
   fence then never signals. An Adreno 610 takes 27 us a block of the ETC2 build: a 2048x2048 level (262144 blocks) is
   7.1 s of it, which the kernel ended as one dispatch, and as 16 dispatches in one submission. In 16 submissions of
   these each took 0.44 to 0.48 s, and the level came out whole */
#define TC_ENCODE_SUBMIT_BLOCKS 16384u

static VkTcShape tc_shape(u32 mode, u32 w) {
    VkTcShape s = {.out_bpp = 4, .out_rows = 1, .in_align = 16};
    if (mode >= TC_BCN_ETC2) {
        /* a row of blocks into a row of blocks of the same bytes, as a row of texels four times as wide: a quarter of
           a block's bytes a texel, which the copy's bufferRowLength counts */
        u32 k = mode - (mode >= TC_BCN_ASTC ? TC_BCN_ASTC : TC_BCN_ETC2) + TC_BC1;
        s.units = (w + 3) / 4;
        s.out_texels = s.units * 4;
        s.out_bpp = k == TC_BC1 || k == TC_BC4 || k == TC_BC4_SNORM ? 2 : 4;
        if (mode >= TC_BCN_ASTC) s.out_bpp = 4; /* an ASTC block is 16 bytes whatever it was made of */
        s.submit_units = TC_ENCODE_SUBMIT_BLOCKS;
    } else if (mode <= TC_A5B5G5R1 || (mode >= TC_VI_RGB565 && mode <= TC_VI_RGBA4444)) {
        s.units = (w + 1) / 2;
        s.out_texels = s.units * 2;
    } else if (mode >= TC_BC1 && mode <= TC_BC7) {
        s.units = (w + 3) / 4;
        s.out_texels = s.units * 4;
        s.out_rows = 4;
        if (mode == TC_BC4 || mode == TC_BC4_SNORM) s.out_bpp = 2;
        if (mode == TC_BC6H_UF || mode == TC_BC6H_SF) s.out_bpp = 8;
    } else if (mode >= TC_VI_RGBX8888) {
        s.units = s.out_texels = w;
    } else if (mode == TC_RGB32F) {
        s.units = s.out_texels = w;
        s.out_bpp = 16;
        s.in_align = 48;
    } else if (mode <= TC_D32S8) {
        s.units = (w + 3) / 4;
        s.out_texels = s.units * 4;
        s.stencil = 1;
        if (mode == TC_D32S8) s.in_align = 32;
    } else {
        s.units = (w + 3) / 4;
        s.out_texels = s.units * 4;
        s.out_rows = 4;
        if (mode == TC_EAC_R || mode == TC_EAC_R_SNORM) s.out_bpp = 2;
    }
    return s;
}
VkTcShape vk_tc_shape(const VkFmt* f, u32 w) { return tc_shape(tc_mode(f), w); }

/* vi's pixel formats (present.c's bytes_per_pixel): 2 RGBX8888, 4 RGB565, 5 BGRA8888, 6 RGBA5551, 7 RGBA4444 */
static u32 tc_vi_mode(u32 fmt) {
    switch (fmt) {
        case 2: return TC_VI_RGBX8888;
        case 4: return TC_VI_RGB565;
        case 5: return TC_VI_BGRA8888;
        case 6: return TC_VI_RGBA5551;
        case 7: return TC_VI_RGBA4444;
    }
    return 0;
}
int vk_tc_vi_converts(u32 fmt) { return tc_vi_mode(fmt) != 0; }
VkTcShape vk_tc_vi_shape(u32 fmt, u32 w) { return tc_shape(tc_vi_mode(fmt), w); }

u64 vk_tc_out_size(const VkTcShape* s, u32 rows, u32 slices) {
    u64 texels = (u64)s->out_texels * s->out_rows * rows * slices;
    return texels * s->out_bpp + (s->stencil ? texels : 0);
}

/* the dispatch of gx x rows x slices workgroups with p, over the input range [in_off, in_off + in_size) of in and the
   output's of out (offsets the device's alignment for storage, which p's biases count from) */
static int tc_dispatch(VkCommandBuffer cb, const TcParams* p, u32 gx, u32 rows, u32 slices, VkBuffer in, u64 in_off,
                       u64 in_size, VkBuffer out, u64 out_off, u64 out_size) {
    const VkPhysicalDeviceLimits* lim = &g_vk.props.limits;
    if (in_size > lim->maxStorageBufferRange || out_size > lim->maxStorageBufferRange ||
        gx > lim->maxComputeWorkGroupCount[0] || rows > lim->maxComputeWorkGroupCount[1] ||
        slices > lim->maxComputeWorkGroupCount[2]) {
        LOG_LIMIT(1, "vulkan: a %ux%u conversion exceeds the device's limits (left out)\n", p->units, rows);
        return 0;
    }
    /* BCn into ASTC: three passes (texconv.glsl's ASTC main). The first over the blocks, which moves those that are an
       ASTC layout already and lists the others; the second and third indirect dispatches over the two lists: the
       blocks with lines of their own decoded and those tried, then the rest decoded and searched for */
    const int astc = p->mode >= TC_BCN_ASTC;
    const u64 blocks = (u64)p->units * rows * slices;
    if (astc && (blocks > 0xFFFFFFFFu || !tc_work(blocks))) {
        LOG_LIMIT(1, "vulkan: no memory for the work lists of a %ux%u conversion (left out)\n", p->units, rows);
        return 0;
    }
    TcParams q = *p;
    if (astc) q.const_a = (u32)blocks; /* a list's words */
    VkPipeline pipe = tc_init(p->mode, 0);
    VkDescriptorSet set = vk_desc_alloc(g_tc_ds);
    if (!set) return 0;
    VkDescriptorBufferInfo sides[3] = {
        {in, in_off, in_size}, {out, out_off, out_size}, {g_tc_work, 0, TC_WORK_HEAD + blocks * 8}};
    snx_vk_storage_buffers(g_vk.dev, set, sides, astc ? 3 : 2);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    VkDependencyInfo md = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    if (astc) { /* no block listed, and 0 x 1 x 1 workgroups for each later pass, which the passes before raise */
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                           VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        mb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        rec_CmdPipelineBarrier2(cb, &md);
        rec_CmdFillBuffer(cb, g_tc_work, 0, TC_WORK_HEAD, 0);
        rec_CmdFillBuffer(cb, g_tc_work, 4, 8, 1);
        rec_CmdFillBuffer(cb, g_tc_work, 20, 8, 1);
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        rec_CmdPipelineBarrier2(cb, &md);
    }
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_tc_layout, 0, 1, &set, 0, NULL);
    rec_CmdPushConstants(cb, g_tc_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof q, &q);
    vk_ck(cb, VK_CK_CONVERT, gx, rows, slices, 0);
    vk_gpuprof_tag(VK_PROF_TEXCONV);
    rec_CmdDispatch(cb, gx, rows, slices);
    for (int pass = 1; astc && pass < 3; pass++) {
        /* each later pass after the writes of the ones before: of its list and the list's head, which the indirect
           dispatch reads, and of the blocks beside the ones it writes */
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                           VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        rec_CmdPipelineBarrier2(cb, &md);
        rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, tc_init(p->mode, pass));
        vk_gpuprof_tag(VK_PROF_TEXCONV);
        rec_CmdDispatchIndirect(cb, g_tc_work, (VkDeviceSize)(pass - 1) * 16);
    }
    return 1;
}
static u64 storage_align(void) {
    u64 a = g_vk.props.limits.minStorageBufferOffsetAlignment;
    return a < 4 ? 4 : a;
}
static int tc_record(VkCommandBuffer cb, u32 mode, u32 zfloat, u32 w, u32 rows, u32 slices, VkBuffer in, u64 in_at,
                     u32 in_pitch, VkBuffer out, u64 out_at) {
    VkTcShape s = tc_shape(mode, w);
    u64 align = storage_align();
    u64 in_off = in_at & ~(align - 1), out_off = out_at & ~(align - 1);
    u64 depth_bytes = (u64)s.out_texels * s.out_rows * rows * slices * s.out_bpp;
    if ((in_at | out_at | in_pitch) & 3) {
        LOG_LIMIT(1, "vulkan: a %ux%u texture to convert isn't word aligned (left out)\n", w, rows);
        return 0;
    }
    TcParams p = {.mode = mode,
                  .units = s.units,
                  .rows = rows,
                  .in_pitch = in_pitch / 4,
                  .out_pitch = s.out_texels * s.out_bpp / 4,
                  .st_pitch = s.out_texels / 4,
                  .in_bias = (u32)((in_at - in_off) / 4),
                  .out_bias = (u32)((out_at - out_off) / 4),
                  .st_bias = (u32)((out_at - out_off + depth_bytes) / 4),
                  .zfloat = zfloat};
    return tc_dispatch(cb, &p, (s.units + 63) / 64, rows, slices, in, in_off,
                       in_at + (u64)in_pitch * rows * slices - in_off, out, out_off,
                       out_at + vk_tc_out_size(&s, rows, slices) - out_off);
}
int vk_tc_nv12_uv(VkCommandBuffer cb, u32 pairs, u32 rows, VkBuffer in, u64 u_at, u64 v_at, u32 in_pitch, VkBuffer out,
                  u64 out_at, u32 out_pitch) {
    u64 align = storage_align(), lo = u_at < v_at ? u_at : v_at, hi = (u_at > v_at ? u_at : v_at);
    u64 in_off = lo & ~(align - 1), out_off = out_at & ~(align - 1);
    u32 units = (pairs + 3) / 4;
    if ((u_at | v_at | in_pitch | out_at | out_pitch) & 3) return 0;
    TcParams p = {.mode = TC_NV12_UV,
                  .units = units,
                  .rows = rows,
                  .in_pitch = in_pitch / 4,
                  .out_pitch = out_pitch / 4,
                  .st_pitch = in_pitch / 4,
                  .in_bias = (u32)((u_at - in_off) / 4),
                  .out_bias = (u32)((out_at - out_off) / 4),
                  .st_bias = (u32)((v_at - in_off) / 4)};
    return tc_dispatch(cb, &p, (units + 63) / 64, rows, 1, in, in_off, hi + (u64)in_pitch * rows - in_off, out, out_off,
                       out_at + (u64)out_pitch * rows - out_off);
}
int vk_tc_convert(VkCommandBuffer cb, const VkFmt* f, u32 w, u32 rows, u32 slices, VkBuffer in, u64 in_at, u32 in_pitch,
                  VkBuffer out, u64 out_at) {
    u32 mode = tc_mode(f);
    /* 24 bit depth into D32_SFLOAT_S8_UINT, where D24S8 isn't supported, goes out as a float */
    u32 zfloat = f->vk == VK_FORMAT_D32_SFLOAT_S8_UINT && mode != TC_D32S8;
    return tc_record(cb, mode, zfloat, w, rows, slices, in, in_at, in_pitch, out, out_at);
}
int vk_tc_convert_vi(VkCommandBuffer cb, u32 fmt, u32 w, u32 rows, VkBuffer in, u64 in_at, u32 in_pitch, VkBuffer out,
                     u64 out_at) {
    return tc_record(cb, tc_vi_mode(fmt), 0, w, rows, 1, in, in_at, in_pitch, out, out_at);
}
int vk_tc_dma_remap(VkCommandBuffer cb, const GpuDmaOp* o, u32 lines, VkBuffer in, u64 in_at, i64 in_pitch,
                    VkBuffer out, u64 out_at, u32 out_pitch) {
    /* the bound input from its lowest line, the last one when the pitch steps down: the shader's u32 offsets from
       line 0 wrap back into it */
    u64 in_low = in_at + (u64)(in_pitch < 0 && lines ? in_pitch * (i64)(lines - 1) : 0);
    u64 align = storage_align(), in_off = in_low & ~(align - 1), out_off = out_at & ~(align - 1);
    u32 units = (o->line_out + 3) / 4, k, remap = o->cs | o->ns << 4 | o->nd << 8;
    if ((out_at | out_pitch) & 3) return 0;
    for (k = 0; k < 4; k++) remap |= (o->sel[k] & 7) << (12 + 4 * k);
    TcParams p = {.mode = TC_DMA_REMAP,
                  .units = units,
                  .rows = lines,
                  .in_pitch = (u32)in_pitch, /* bytes, for this mode; a step down as its two's complement */
                  .out_pitch = out_pitch / 4,
                  .in_bias = (u32)(in_at - in_off),
                  .out_bias = (u32)((out_at - out_off) / 4),
                  .remap = remap,
                  .const_a = o->const_a,
                  .const_b = o->const_b,
                  .line_out = o->line_out};
    /* the input range covers every line's bytes (a fill reads none: a word of it still binds) */
    u64 in_size =
        in_low + (lines ? (u64)(in_pitch < 0 ? -in_pitch : in_pitch) * (lines - 1) + o->line_in : 0) + 4 - in_off;
    return tc_dispatch(cb, &p, (units + 63) / 64, lines, 1, in, in_off, (in_size + 3) & ~3ull, out, out_off,
                       out_at + (u64)out_pitch * lines - out_off);
}
int vk_tc_blit(VkCommandBuffer cb, u32 bpp, u32 fmt, u32 nb, u32 rows, VkBuffer in, u64 in_size, u64 src_at,
               u32 in_pitch, u64 col_at, u64 row_at, VkBuffer out, u64 out_at, u32 out_pitch) {
    u64 out_off = out_at & ~(storage_align() - 1);
    u32 units = (nb + 3) / 4;
    if ((col_at | row_at | out_at | out_pitch) & 3) return 0;
    TcParams p = {.mode = (fmt & GPU_BLIT_CONVERT_BIT) ? TC_BLIT_CONVERT
                          : fmt                        ? TC_BLIT_FLOAT
                                                       : TC_BLIT,
                  .units = units,
                  .rows = rows,
                  .in_pitch = in_pitch, /* bytes, for this mode */
                  .out_pitch = out_pitch / 4,
                  .in_bias = (u32)src_at,
                  .out_bias = (u32)((out_at - out_off) / 4),
                  .remap = fmt ? fmt : bpp,
                  .const_a = (u32)(col_at / 4),
                  .const_b = (u32)(row_at / 4),
                  .line_out = nb};
    return tc_dispatch(cb, &p, (units + 63) / 64, rows, 1, in, 0, (in_size + 3) & ~3ull, out, out_off,
                       out_at + (u64)out_pitch * rows - out_off);
}
int vk_tc_index(VkCommandBuffer cb, u32 mode, u32 remap, u32 restart, u32 n, VkBuffer in, u64 in_at, u64 in_size,
                VkBuffer out, u64 out_at, u64 out_size) {
    u64 align = storage_align(), in_off = in_at & ~(align - 1), out_off = out_at & ~(align - 1);
    /* rows of at most 65,535 workgroups, the least maxComputeWorkGroupCount[0] the specification allows */
    u32 units = n < 65535u * 64 ? n : 65535u * 64, rows = n ? (n + units - 1) / units : 0;
    if ((in_at | out_at) & 3) return 0;
    if (!n) return 1;
    TcParams p = {.mode = mode,
                  .units = units,
                  .rows = rows,
                  .in_bias = (u32)(in_at - in_off), /* bytes, for these modes */
                  .out_bias = (u32)((out_at - out_off) / 4),
                  .remap = remap,
                  .const_a = restart,
                  .line_out = n};
    /* a word of input still binds when there is none (vertex numbers made into quads) */
    return tc_dispatch(cb, &p, (units + 63) / 64, rows, 1, in, in_off,
                       (in_at + (in_size ? in_size : 4) - in_off + 3) & ~3ull, out, out_off,
                       out_at + out_size - out_off);
}
int vk_tc_restride(VkCommandBuffer cb, u32 in_stride, u32 out_stride, u32 nv, VkBuffer in, u64 in_at, u64 len,
                   VkBuffer out, u64 out_at) {
    u64 align = storage_align(), in_off = in_at & ~(align - 1), out_off = out_at & ~(align - 1);
    u64 n = (u64)nv * (out_stride / 4), in_end = (in_at + len + 3) & ~3ull;
    if ((out_stride | out_at) & 3 || !out_stride || n > 0xFFFFFFFFu || len > 0xFFFFFFFFu) return 0;
    if (!n) return 1;
    u32 units = n < 65535u * 64 ? (u32)n : 65535u * 64, rows = (u32)((n + units - 1) / units);
    TcParams p = {.mode = TC_RESTRIDE,
                  .units = units,
                  .rows = rows,
                  .in_pitch = in_stride, /* bytes, for this mode */
                  .out_pitch = out_stride / 4,
                  .in_bias = (u32)(in_at - in_off), /* bytes */
                  .out_bias = (u32)((out_at - out_off) / 4),
                  .const_a = (u32)len,
                  .line_out = (u32)n};
    return tc_dispatch(cb, &p, (units + 63) / 64, rows, 1, in, in_off, in_end - in_off, out, out_off,
                       out_at + n * 4 - out_off);
}
