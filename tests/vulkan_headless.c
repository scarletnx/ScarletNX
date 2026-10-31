/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Opt-in integration test: real renderer, no game data. Works with Mesa lavapipe and no display server. */
#include "common/utilities/common.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/gpu/maxwell/draw_state.h"
#include "nx/gpu/renderer.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/maxwell/kepler_compute_regs.h"
#include "nx/gpu/maxwell/engines_int.h"
#include "nx/gpu/maxwell/maxwell_3d_regs.h"
#include "nx/gpu/maxwell/gob.h"
#include "nx/gpu/texture/blocklinear.h"
#include "nx/gpu/nvn/nvn_copy.h"
#include "nx/gpu/nvn/nvn_draw.h"
#include "nx/gpu/video/vic.h"
#include "nx/memory/mem.h"
#include "nx/memory/own.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/platform/backend.h"
#include "spirv_emitter_fixture.h"
#include "support/dma_ref.h"
#include "support/fermi2d_ref.h"
#include "support/nvdec_ref.h"
#include "support/vic_ref.h"
#include <libavutil/buffer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "vulkan_headless:%d: %s\n", __LINE__, #x);                                                 \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

/* A buffer copied into staging as it is hashed gets the hash vk_hash gives it, which the cache compares with hashes
   taken without a copy, and the bytes: every size around the hash's 32-byte steps, from every alignment */
static int check_hash_copy(void) {
    u8 src[300], dst[300];
    for (u32 i = 0; i < sizeof src; i++) src[i] = (u8)(i * 151 + 7);
    for (u32 at = 0; at < 8; at++)
        for (u32 n = 0; at + n <= 290; n++) {
            memset(dst, 0xA5, sizeof dst);
            CHECK(vk_hash_copy(dst + (7 - at), src + at, n) == vk_hash(src + at, n));
            CHECK(!memcmp(dst + (7 - at), src + at, n));
            CHECK(dst[7 - at + n] == 0xA5 && (at == 7 || dst[6 - at] == 0xA5));
        }
    return 0;
}

/* A long buffer texture keeps consecutive texels across an image-row boundary,
   including a final row that is only partly used. Read actual uploaded pixels. */
static int check_buffer_texture_rows(void) {
    const u64 gva = 0x6000000, cpu = HEAP_BASE + 0x600000;
    const u32 row = 1u << SH_TEXEL_ROW_SHIFT, count = row + 3;
    const u32 samples[] = {row - 1, row, count - 1};
    u32 tic[8] = {0x08 | (2u << 7) | (2u << 10) | (2u << 13) | (2u << 16),
                  (u32)gva, 0, (count - 1) >> 16,
                  ((count - 1) & 0xffff) | (6u << 23), 0, 0, 0};
    gmmu_map(gva, cpu, (u64)row * 8);
    memset(GP(cpu), 0xcd, (size_t)row * 8);
    for (u32 i = 0; i < count; i++) snx_st_le32(GP(cpu) + (u64)i * 4, 0xff000000u | i);
    vk_lock();
    Surf* texture = vk_tex_from_tic(tic);
    CHECK(texture && texture->upload_valid && texture->f.bpp == 4);
    CHECK(texture->w == row && texture->h == 2);
    VkBuffer buffer;
    u8* result = vk_readback_buffer(sizeof samples, &buffer);
    CHECK(result);
    vk_surf_use(vk_cmd(), texture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copies[3] = {{0}};
    for (u32 i = 0; i < 3; i++) {
        copies[i].bufferOffset = i * 4;
        copies[i].imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copies[i].imageOffset = (VkOffset3D){(i32)(samples[i] % row), (i32)(samples[i] / row), 0};
        copies[i].imageExtent = (VkExtent3D){1, 1, 1};
    }
    rec_CmdCopyImageToBuffer(vk_cmd(), texture->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 3, copies);
    vk_finish();
    vk_readback_ready();
    for (u32 i = 0; i < 3; i++) CHECK(snx_ld_le32(result + i * 4) == (0xff000000u | samples[i]));
    vk_unlock();
    return 0;
}

/* Guest memory read in place (gpu/vk/uma.c, --vk_memory=2 where the device has VK_EXT_external_memory_host): a vertex
   range is guest memory itself, so the GPU reads the bytes that are there when it runs, a CPU write after the binding
   included. A range off the binding's alignment is copied. Guest pages given back and mapped again are other pages,
   and the import of the old ones goes (mem.c's hook): the GPU reads the new ones */
static int check_uma(void) {
    const u64 gva = 0x7000000, cpu = HEAP_BASE + 0x780000, span = 0x10000;
    VkBuffer b, b2, rb;
    u64 off, off2;
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    gmmu_map(gva, cpu, span);
    memset(GP(cpu), 0x11, (size_t)span);
    vk_lock();
    CHECK(vk_buf_get_input(gva + 0x100, 256, 4, &b, &off));
    memset(GP(cpu) + 0x100, 0x5a, 256); /* after the binding, before the GPU runs */
    u8* result = vk_readback_buffer(512, &rb);
    CHECK(result);
    VkBufferCopy c = {off, 0, 256};
    rec_CmdCopyBuffer(vk_cmd(), b, rb, 1, &c);
    vk_finish();
    vk_readback_ready();
    for (u32 i = 0; i < 256; i++) CHECK(result[i] == 0x5a);
    CHECK(vk_buf_get_input(gva + 0x100, 256, 4, &b2, &off2) && b2 == b && off2 == off); /* the same import */
    CHECK(vk_buf_get_input(gva + 0x102, 8, 4, &b2, &off2) && b2 != b);                 /* off its alignment: copied */
    vk_unlock();
    /* the pages given back and mapped again: zero, then written */
    CHECK(mem_unmap(cpu, span) == 0 && mem_map(cpu, span, MS_NORMAL, PERM_RW) == 0);
    g_renderer->mem_may_have_changed();
    memset(GP(cpu), 0x33, (size_t)span);
    vk_lock();
    CHECK(vk_buf_get_input(gva + 0x100, 256, 4, &b2, &off2));
    c = (VkBufferCopy){off2, 256, 256};
    rec_CmdCopyBuffer(vk_cmd(), b2, rb, 1, &c);
    vk_finish();
    vk_readback_ready();
    for (u32 i = 256; i < 512; i++) CHECK(result[i] == 0x33);
    vk_unlock();
    return 0;
}

/* Overflow blocks remain live through submission, then return with their slot. */
static int check_staging_overflow(void)
{
	VkBuffer seen[VK_FRAMES][3] = {{0}};
	u64 saved_cap = g_vk.alloc_cap;

	g_vk.alloc_cap = 1u << 20;
	for (u32 frame = 0; frame < VK_FRAMES * 3; frame++) {
		VkBuffer rb, output, next_output;
		VmaAllocation memory;
		VkBufferCopy output_copy;
		u64 output_offset, next_offset;
		u8* result = vk_readback_buffer(80, &rb);
		u8* writable = vk_stage_output(g_vk.alloc_cap, 16, &output, &output_offset, &memory);
		u32 slot = vk_frame_slot();

		CHECK(result && writable && memory);
		memset(writable, 0xa0 + frame, 16);
		output_copy = (VkBufferCopy){output_offset, 48, 16};
		rec_CmdCopyBuffer(vk_cmd(), output, rb, 1, &output_copy);
		for (u32 block = 0; block < 3; block++) {
			VkBuffer buffer;
			u64 offset;
			u8* data = vk_stage_upload(g_vk.alloc_cap, 16, &buffer, &offset);
			VkBufferCopy copy = {offset, block * 16, 16};

			CHECK(data);
			CHECK(buffer != output);
			memset(data, frame * 3 + block + 1, 16);
			if (frame < VK_FRAMES) {
				for (u32 before = 0; before < block; before++)
					CHECK(seen[slot][before] != buffer);
				seen[slot][block] = buffer;
			} else {
				CHECK(buffer == seen[slot][0] || buffer == seen[slot][1] ||
				      buffer == seen[slot][2]);
			}
			rec_CmdCopyBuffer(vk_cmd(), buffer, rb, 1, &copy);
		}
		writable = vk_stage_output(16, 16, &next_output, &next_offset, &memory);
		CHECK(writable && next_output != output);
		memset(writable, 0xb0 + frame, 16);
		output_copy = (VkBufferCopy){next_offset, 64, 16};
		rec_CmdCopyBuffer(vk_cmd(), next_output, rb, 1, &output_copy);
		vk_finish();
		vk_readback_ready();
		for (u32 i = 0; i < 48; i++)
			CHECK(result[i] == frame * 3 + i / 16 + 1);
		for (u32 i = 48; i < 64; i++)
			CHECK(result[i] == 0xa0 + frame);
		for (u32 i = 64; i < 80; i++)
			CHECK(result[i] == 0xb0 + frame);
		vk_frame_next();
	}
	g_vk.alloc_cap = saved_cap;
	return 0;
}

/* the renderer's viewport index and its first viewport's facing for the registers r, through the draw state */
static VkViewport viewport_of(const u32* r, u32 index) {
    DsDynamic d;
    DsViewports v;
    mx_draw_dynamic(r, &d);
    mx_draw_viewports(r, &v);
    return vk_viewport_from(&d, &v, index);
}
static VkFrontFace front_face_of(const u32* r) {
    DsDynamic d;
    DsViewports v;
    mx_draw_dynamic(r, &d);
    mx_draw_viewports(r, &v);
    return vk_render_front_face(&d, &v);
}

static int check_viewport_orientation(void) {
    u32 r[0xE00] = {0};
    float transform[6] = {32, 17, 0.5f, 40, 37, 0.5f};
    memcpy(&r[0x280], transform, sizeof transform);
    r[0x64B] = 1;
    r[0x286] = 0x6420; /* identity x/y/z/w swizzle */
    r[0x647] = 0x900;  /* guest clockwise */
    VkViewport vp = viewport_of(r, 0);
    CHECK(vp.x == 8 && vp.width == 64 && vp.y == 20 && vp.height == 34);
    CHECK(vp.minDepth == 0 && vp.maxDepth == 1);
    CHECK(front_face_of(r) == VK_FRONT_FACE_CLOCKWISE);
    /* NegativeY reverses the endpoints about translate_y, keeping the same viewport rectangle. */
    r[0x286] = 0x6430;
    vp = viewport_of(r, 0);
    CHECK(vp.y == 54 && vp.height == -34 && vp.y + vp.height == 20);
    CHECK(front_face_of(r) == VK_FRONT_FACE_CLOCKWISE);
    /* Negative scale and negative coordinate swizzle cancel in the viewport. */
    transform[1] = -17;
    memcpy(&r[0x280], transform, sizeof transform);
    vp = viewport_of(r, 0);
    CHECK(vp.y == 20 && vp.height == 34);
    CHECK(front_face_of(r) == VK_FRONT_FACE_CLOCKWISE);
    r[0x4EB] = 0x10;
    CHECK(front_face_of(r) == VK_FRONT_FACE_COUNTER_CLOCKWISE);
    /* The origin facing flip and a negative viewport cancel. */
    transform[1] = 17;
    memcpy(&r[0x280], transform, sizeof transform);
    CHECK(viewport_of(r, 0).height == -34);
    CHECK(front_face_of(r) == VK_FRONT_FACE_CLOCKWISE);
    transform[1] = -17;
    memcpy(&r[0x280], transform, sizeof transform);
    r[0x647] = 0x901;
    CHECK(front_face_of(r) == VK_FRONT_FACE_CLOCKWISE);
    transform[1] = 17;
    memcpy(&r[0x280], transform, sizeof transform);
    r[0x286] = 0x6420;
    r[0x647] = 0x900;
    r[0x3FE] = 100u << 16;
    r[0x4EB] = 1; /* lower-left window origin flips the viewport */
    vp = viewport_of(r, 0);
    CHECK(vp.y == 120 && vp.height == -34);
    CHECK(front_face_of(r) == VK_FRONT_FACE_CLOCKWISE);
    /* and NegativeY on it reverses it again about the flipped translate_y */
    r[0x286] = 0x6430;
    vp = viewport_of(r, 0);
    CHECK(vp.y == 86 && vp.height == 34);
    CHECK(front_face_of(r) == VK_FRONT_FACE_CLOCKWISE);
    r[0x286] = 0x6420;
    /* Viewport 1 has independent transform, swizzle and depth registers. */
    float second[6] = {10, 20, 0.25f, 50, 60, 0.5f};
    memcpy(&r[0x288], second, sizeof second);
    r[0x28e] = 0x6420;
    r[0x4eb] = 0;
    vp = viewport_of(r, 1);
    CHECK(vp.x == 40 && vp.y == 40 && vp.width == 20 && vp.height == 40);
    CHECK(vp.minDepth == 0.25f && vp.maxDepth == 0.75f);
    CHECK(viewport_of(r, 0).width == 64);
    return 0;
}

/* n texels of packed depth and stencil (one row) converted on the device (vk/texconv.c): the depth aspect's words and
   the stencil aspect's bytes a copy would take */
static int gpu_unpack(const VkFmt* f, const u32* src, u32 n, u32* depth, u8* stencil) {
    VkTcShape s = vk_tc_shape(f, n);
    u32 pitch = (n * f->bpp + s.in_align - 1) / s.in_align * s.in_align;
    u64 at, cap, out_size = vk_tc_out_size(&s, 1, 1);
    VkBuffer staged, rb;
    u8* p = vk_stage(pitch, vk_stage_align(), &staged, &at);
    VkBuffer scratch = vk_bl_scratch(out_size, &cap);
    u8* out = vk_readback_buffer(out_size, &rb);
    CHECK(p && scratch && out);
    memset(p, 0, pitch);
    memcpy(p, src, (size_t)n * f->bpp);
    vk_bl_barrier(vk_cmd(), 1);
    CHECK(vk_tc_convert(vk_cmd(), f, n, 1, 1, staged, at, pitch, scratch, 0));
    vk_bl_barrier(vk_cmd(), 0);
    VkBufferCopy c = {0, 0, out_size};
    rec_CmdCopyBuffer(vk_cmd(), scratch, rb, 1, &c);
    vk_finish();
    vk_readback_ready();
    memcpy(depth, out, (size_t)n * 4);
    memcpy(stencil, out + (u64)s.out_texels * 4, n);
    return 0;
}

static int check_packed_depth(void) {
    const u32 formats[] = {0x0D, 0x0E, 0x29, 0x2A, 0x2B, 0x30};
    for (u32 i = 0; i < sizeof formats / sizeof *formats; i++) {
        VkFmt f = vk_tic_format(formats[i], 0);
        CHECK(f.depth && f.stencil);
        const int reversed = formats[i] == 0x0E || formats[i] == 0x29;
        const int floating = formats[i] == 0x30;
        CHECK(f.bpp == (floating ? 8 : 4));
        u32 src[6] = {0}, depth[3];
        u8 stencil[3];
        const u32 z[] = {0, 0x800000, 0xffffff};
        for (u32 k = 0; k < 3; k++) {
            if (floating) {
                float v = (float)k * 0.5f;
                memcpy(&src[k * 2], &v, 4);
                src[k * 2 + 1] = 0x12340000 | (0x51 + k);
            } else
                src[k] = reversed ? (z[k] << 8) | (0x51 + k) : z[k] | ((0x51 + k) << 24);
        }
        for (int fallback = 0; fallback < 2; fallback++) {
            f.vk = fallback || floating ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D24_UNORM_S8_UINT;
            CHECK(!gpu_unpack(&f, src, 3, depth, stencil));
            for (u32 k = 0; k < 3; k++) {
                CHECK(stencil[k] == (formats[i] == 0x2A ? 0 : 0x51 + k));
                if (floating)
                    CHECK(depth[k] == src[k * 2]);
                else if (fallback) {
                    float value;
                    memcpy(&value, &depth[k], 4);
                    CHECK(value == (float)z[k] / 16777215.0f);
                } else
                    CHECK(depth[k] == z[k]);
            }
        }
    }
    return 0;
}

/* Clear separate shadow cascades through the real register path, then read each depth layer back. */
/* a clear as the 3D engine hands it to the renderer (maxwell3d.c r3d_clear). The tests set the registers directly,
   not through the engine's methods, so every register group counts as changed */
static void engine_clear(GpuChannel* ch, u32 v) {
    DrawClear c;
    ch->r3d_dirty = ~0u;
    mx_draw_state_sync(ch);
    mx_draw_clear(ch->r3d, v, &c);
    vk_render_clear(&ch->ds, &c, &ch->owner);
}
static int check_depth_array(void) {
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    u32* r = ch->r3d;
    r[0x54E] = 1;
    r[0x3F9] = 0x100000;
    r[0x3FA] = 0x0A;   /* Z32F */
    r[0x3FB] = 0x1000; /* pitch linear */
    r[0x3FC] = 1024;   /* 4096 byte layer stride */
    r[0x48A] = r[0x48B] = 16;
    r[0x48C] = 3;
    for (u32 layer = 0; layer < 3; layer++) {
        float depth = 0.25f * (layer + 1);
        memcpy(&r[0x364], &depth, 4);
        engine_clear(ch, 1 | (layer << 10));
    }
    vk_render_end();
    Surf* target = vk_rt_find(r[0x3F9]);
    CHECK(target && target->layers == 3 && target->layer_size == 4096);
    Surf* drawn[4];
    /* A generated texture may be sampled for the first time after a loading step.
       Its pixels still live in the render target, regardless of presentation age. */
    g_vk_present += 5;
    CHECK(vk_rt_drawn_in(target->gva, target->gva + target->layer_size * target->layers, drawn, 4) >= 1);
    CHECK(drawn[0] == target && vk_rt_drawn_fresh(target));
    VkBuffer buffer;
    u8* data = vk_readback_buffer(12, &buffer);
    CHECK(data);
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copies[3] = {0};
    for (u32 layer = 0; layer < 3; layer++) {
        copies[layer].bufferOffset = layer * 4;
        copies[layer].imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_DEPTH_BIT, 0, layer, 1};
        copies[layer].imageExtent = (VkExtent3D){1, 1, 1};
    }
    rec_CmdCopyImageToBuffer(cb, target->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 3, copies);
    vk_finish();
    vk_readback_ready();
    for (u32 layer = 0; layer < 3; layer++) {
        float depth;
        memcpy(&depth, data + layer * 4, 4);
        CHECK(depth == 0.25f * (layer + 1));
    }
    gpu_mem_written_shader(target->gva, 4);
    CHECK(!vk_rt_drawn_fresh(target)); /* reused guest memory must still win */
    r[0x3F9] = 0x200000;
    r[0x48C] = 3 | (1u << 16);
    engine_clear(ch, 1);
    vk_render_end();
    CHECK(vk_rt_find(r[0x3F9])->layers == 1);
    /* Sampling a packed-depth array must retain the rendered image, including all cascade layers. */
    r[0x3F9] = 0x300000;
    r[0x3FA] = 0x14;
    r[0x48C] = 3;
    engine_clear(ch, 3);
    vk_render_end();
    u32 tic[8] = {0x2B, r[0x3F9], 2u << 21, 2, 15 | (7u << 23), 15 | (2u << 16), 0, 0};
    CHECK(vk_tex_from_tic(tic) == vk_rt_find(r[0x3F9]));
    free(ch);
    return 0;
}

/* Switch sRGB writes on one image without losing its contents. Both full clears and the masked-clear draw
   must use the selected attachment format, including when it changes in an already-open pass. */
static int check_srgb_write(void) {
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    u32* r = ch->r3d;
    r[0x487] = 1;
    r[0x201] = 0x400000;
    r[0x202] = 64; /* 16 RGBA pixels, pitch linear */
    r[0x203] = 16;
    r[0x204] = 0xD6; /* A8B8G8R8 sRGB */
    r[0x205] = 0x1000;
    r[0x206] = 2;
    r[0x207] = 1024;
    float color[4] = {0.25f, 0.25f, 0.25f, 1};
    memcpy(&r[0x360], color, sizeof color);
    r[0x56E] = 1;
    engine_clear(ch, 15 << 2);
    r[0x56E] = 0;
    engine_clear(ch, (15 << 2) | (1 << 10));
    color[0] = 0.5f;
    memcpy(&r[0x360], color, sizeof color);
    r[0x56E] = 1;
    engine_clear(ch, (1 << 2) | (1 << 10));
    r[0x56E] = 0;
    engine_clear(ch, 1 << 2);
    vk_render_end();
    Surf* target = vk_rt_find(r[0x201]);
    CHECK(target && target->f.vk == VK_FORMAT_R8G8B8A8_SRGB && target->layers == 2);
    VkBuffer buffer;
    u8* data = vk_readback_buffer(8, &buffer);
    CHECK(data);
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copies[2] = {0};
    for (u32 i = 0; i < 2; i++) {
        copies[i].bufferOffset = i * 4;
        copies[i].imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, i, 1};
        copies[i].imageExtent = (VkExtent3D){1, 1, 1};
    }
    rec_CmdCopyImageToBuffer(cb, target->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 2, copies);
    vk_finish();
    vk_readback_ready();
    const u8 expected[8] = {128, 137, 137, 255, 188, 64, 64, 255};
    for (u32 i = 0; i < 8; i++) CHECK(abs((int)data[i] - expected[i]) <= 1);
    free(ch);
    return 0;
}

/* a single-layer target's pixel 0 as its image holds it */
static int read_pixel0(Surf* target, u8 out[4]) {
    VkBuffer buffer;
    u8* data = vk_readback_buffer(4, &buffer);
    CHECK(data);
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {1, 1, 1}};
    rec_CmdCopyImageToBuffer(cb, target->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    vk_finish();
    vk_readback_ready();
    memcpy(out, data, 4);
    return 0;
}

/* Clears as load ops: a whole-target clear before any draw of its pass is the pass's LOAD_OP_CLEAR, and gives the
   pixels a vkCmdClearAttachments gave. A discarded target's next pass loads it as DONT_CARE, and what that pass writes
   is there */
static int check_clear_load_op(void) {
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    u32* r = ch->r3d;
    r[0x487] = 1;
    r[0x201] = 0x480000;
    r[0x202] = 64; /* 16 RGBA pixels, pitch linear */
    r[0x203] = 16;
    r[0x204] = 0xD5; /* A8B8G8R8 */
    r[0x205] = 0x1000;
    r[0x206] = 1;
    float red[4] = {1, 0, 0, 1}, half[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    memcpy(&r[0x360], red, sizeof red);
    u64 loads = g_vk_clear_loads, cmds = g_vk_clear_cmds, dontcare = g_vk_dontcare_loads;
    engine_clear(ch, 15 << 2);
    vk_render_end();
    CHECK(g_vk_clear_loads == loads + 1 && g_vk_clear_cmds == cmds);
    Surf* target = vk_rt_find(r[0x201]);
    CHECK(target);
    u8 px[4];
    CHECK(!read_pixel0(target, px));
    CHECK(px[0] == 255 && px[1] == 0 && px[2] == 0 && px[3] == 255);
    /* discarded, then only its red written (a masked clear is a draw): red is what the draw wrote */
    vk_render_discard(&ch->ds, 1);
    memcpy(&r[0x360], half, sizeof half);
    engine_clear(ch, 1 << 2);
    vk_render_end();
    CHECK(g_vk_dontcare_loads == dontcare + 1);
    CHECK(!read_pixel0(target, px));
    CHECK(abs((int)px[0] - 128) <= 1);
    free(ch);
    return 0;
}

/* Images in pieces (sparse binding: vkQueueBindSparse on the graphics queue) made on this thread while the recording
   thread submits frames to the same queue: Vulkan requires the two calls to be externally synchronized, which the
   validation layer's threading checks report when they overlap */
static int check_sparse_while_submitting(void) {
    if (!g_vk.sparse) return 0;
    u64 saved_cap = g_vk.alloc_cap;
    g_vk.alloc_cap = 64u << 10;
    for (int i = 0; i < 2000; i++) {
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .extent = {256, 256, 1}, /* 256 KiB: four pieces */
                                .mipLevels = 1,
                                .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VkImage img;
        VmaAllocation mem;
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        VkDependencyInfo di = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vk_lock();
        rec_CmdPipelineBarrier2(vk_cmd(), &di); /* a frame with a command, so it is submitted */
        CHECK(vk_image_create(&ii, &img, &mem));
        vk_image_destroy(img, mem);
        vk_frame_next(); /* the frame's submission, made by the recording thread */
        vk_unlock();
    }
    g_vk.alloc_cap = saved_cap;
    return 0;
}

/* A fetch of a multisampled texture's sample (TLDS.MS and TLD.MS, the sample in Rb) translates into a module the
   device takes (the validation layer checks its SPIR-V), with a multisampled texture slot */
static int check_multisample_shader_gate(void) {
    u64 guest[14] = {0};
    guest[10] = 0x001f8000fc0007e0ull; /* scheduler */
    guest[12] = 0xe30000000007000full; /* EXIT */
    const u64 fetch[2] = {0xd000000000000000ull | 6ull << 53 | 1ull << 57 | 0x10ull << 36 | 0xffull << 28 | 4ull << 20 |
                              7ull << 16 | 2ull << 8, /* TLDS.MS */
                          0xdc00000000000000ull | 7ull << 51 | 1ull << 50 | 0xfull << 31 | 2ull << 28 | 0x10ull << 36 |
                              4ull << 20 | 7ull << 16 | 2ull << 8}; /* bound TLD.MS */
    for (int k = 0; k < 2; k++) {
        ShaderInfo info = {0};
        size_t words = 0;
        guest[11] = fetch[k];
        u32* spirv = vk_translate_spirv((const u8*)guest, sizeof guest, ST_FS, &info, &words, NULL, NULL);
        if (!spirv) fprintf(stderr, "multisample fetch %d: %s\n", k, info.first_unsupported);
        CHECK(spirv && !info.unsupported && info.ntex == 1 && (info.tex[0].flags & 2));
        VkShaderModuleCreateInfo smi = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = words * sizeof(u32), .pCode = spirv};
        VkShaderModule module;
        CHECK(vkCreateShaderModule(g_vk.dev, &smi, NULL, &module) == VK_SUCCESS);
        vkDestroyShaderModule(g_vk.dev, module, NULL);
        free(spirv);
    }
    return 0;
}

/* Execute the direct writer's module, not only spirv-val's structural check. */
/* code, a compute module of one invocation over set 0 binding 0, run once on count words of data, read back
   into data */
/* code run once on data, count words at set 0 binding 0, with view and sampler at binding 1 when view isn't null */
static int run_compute(const u32* code, size_t words, u32* data, u32 count, VkImageView view, VkSampler sampler) {
    VkShaderModuleCreateInfo smi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = words * sizeof(u32), .pCode = code};
    VkShaderModule module;
    CHECK(vkCreateShaderModule(g_vk.dev, &smi, NULL, &module) == VK_SUCCESS);
    u32 nbindings = view ? 2 : 1;
    VkDescriptorSetLayoutBinding bindings[2] = {{.binding = 0,
                                                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                 .descriptorCount = 1,
                                                 .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
                                                {.binding = 1,
                                                 .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                 .descriptorCount = 1,
                                                 .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo dli = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = nbindings, .pBindings = bindings};
    VkDescriptorSetLayout dlayout;
    CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &dli, NULL, &dlayout) == VK_SUCCESS);
    VkPipelineLayoutCreateInfo pli = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dlayout};
    VkPipelineLayout playout;
    CHECK(vkCreatePipelineLayout(g_vk.dev, &pli, NULL, &playout) == VK_SUCCESS);
    VkComputePipelineCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                       .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                 .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                 .module = module,
                                                 .pName = "main"},
                                       .layout = playout};
    VkPipeline pipeline;
    CHECK(vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &pci, NULL, &pipeline) == VK_SUCCESS);
    vkDestroyShaderModule(g_vk.dev, module, NULL);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = (VkDeviceSize)count * sizeof(u32),
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VmaAllocationCreateInfo ai = {.flags =
                                      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                  .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer buffer;
    VmaAllocation memory;
    VmaAllocationInfo allocation;
    CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &buffer, &memory, &allocation) == VK_SUCCESS);
    memcpy(allocation.pMappedData, data, (size_t)count * sizeof(u32));
    CHECK(vmaFlushAllocation(g_vk.vma, memory, 0, VK_WHOLE_SIZE) == VK_SUCCESS);
    VkDescriptorPoolSize pool_sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                          {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .maxSets = 1,
                                      .poolSizeCount = nbindings,
                                      .pPoolSizes = pool_sizes};
    VkDescriptorPool pool;
    CHECK(vkCreateDescriptorPool(g_vk.dev, &dpi, NULL, &pool) == VK_SUCCESS);
    VkDescriptorSetAllocateInfo dsi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = pool,
                                       .descriptorSetCount = 1,
                                       .pSetLayouts = &dlayout};
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(g_vk.dev, &dsi, &set) == VK_SUCCESS);
    VkDescriptorBufferInfo dbi = {buffer, 0, (VkDeviceSize)count * sizeof(u32)};
    VkDescriptorImageInfo dii = {sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = set,
                                       .dstBinding = 0,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .pBufferInfo = &dbi},
                                      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = set,
                                       .dstBinding = 1,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       .pImageInfo = &dii}};
    vkUpdateDescriptorSets(g_vk.dev, nbindings, writes, 0, NULL);
    VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .queueFamilyIndex = g_vk.gfx_family};
    VkCommandPool cpool;
    CHECK(vkCreateCommandPool(g_vk.dev, &cpi, NULL, &cpool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = cpool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = 1};
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(g_vk.dev, &cbi, &cmd) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, playout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cmd, 1, 1, 1);
    VkMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(cmd, &dependency);
    CHECK(vkEndCommandBuffer(cmd) == VK_SUCCESS);
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
    CHECK(vkQueueSubmit(g_vk.gfx, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
    CHECK(vkQueueWaitIdle(g_vk.gfx) == VK_SUCCESS);
    CHECK(vmaInvalidateAllocation(g_vk.vma, memory, 0, VK_WHOLE_SIZE) == VK_SUCCESS);
    memcpy(data, allocation.pMappedData, (size_t)count * sizeof(u32));
    vkDestroyCommandPool(g_vk.dev, cpool, NULL);
    vkDestroyDescriptorPool(g_vk.dev, pool, NULL);
    vmaDestroyBuffer(g_vk.vma, buffer, memory);
    vkDestroyPipeline(g_vk.dev, pipeline, NULL);
    vkDestroyPipelineLayout(g_vk.dev, playout, NULL);
    vkDestroyDescriptorSetLayout(g_vk.dev, dlayout, NULL);
    return 0;
}

static int run_storage_compute(const u32* code, size_t words, u32* data, u32 count) {
    return run_compute(code, words, data, count, VK_NULL_HANDLE, VK_NULL_HANDLE);
}

static int check_direct_spirv(void) {
    size_t words = 0;
    u32* code = snx_test_spirv_compute(&words), value = 0;
    int r = run_storage_compute(code, words, &value, 1);
    free(code);
    CHECK(!r && value == 42);
    return 0;
}

/* A sampler's min/max reduction (the TSC's word 1, bits 10-11) where a linear filter reads texels 0 and 1 of an R32F
   row, 0.0 and 1.0, at their middle: their average, the least, the greatest, and the average for 3, which names no
   mode. Where the device or the view's format takes no min/max filter, or the texture is the null one or an integer
   one, the filter nearest or the sampler a depth compare, the sampler is the averaging one. So is it for a swizzled
   view where the device's min/max filter takes no swizzle */
static int check_sampler_reduction(void) {
    static const float texels[2] = {0.0f, 1.0f}, want[4] = {0.5f, 0.0f, 1.0f, 0.5f};
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R32_SFLOAT,
                            .extent = {2, 1, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo device = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage image;
    VmaAllocation image_memory;
    CHECK(vmaCreateImage(g_vk.vma, &ii, &device, &image, &image_memory, NULL) == VK_SUCCESS);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = sizeof texels,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo host = {.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                             VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                    .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer staging;
    VmaAllocation staging_memory;
    VmaAllocationInfo allocation;
    CHECK(vmaCreateBuffer(g_vk.vma, &bi, &host, &staging, &staging_memory, &allocation) == VK_SUCCESS);
    memcpy(allocation.pMappedData, texels, sizeof texels);
    CHECK(vmaFlushAllocation(g_vk.vma, staging_memory, 0, VK_WHOLE_SIZE) == VK_SUCCESS);
    vk_render_end();
    VkCommandBuffer cb = vk_cmd();
    VkImageMemoryBarrier2 to_copy = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                     .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                     .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                     .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                     .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .image = image,
                                     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageMemoryBarrier2 to_read = to_copy;
    to_read.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_read.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1};
    dep.pImageMemoryBarriers = &to_copy;
    rec_CmdPipelineBarrier2(cb, &dep);
    VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {2, 1, 1}};
    rec_CmdCopyBufferToImage(cb, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    dep.pImageMemoryBarriers = &to_read;
    rec_CmdPipelineBarrier2(cb, &dep);
    vk_finish();
    vmaDestroyBuffer(g_vk.vma, staging, staging_memory);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R32_SFLOAT,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view;
    CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &view) == VK_SUCCESS);
    size_t words = 0;
    u32* code = snx_test_spirv_sample(&words);
    CHECK(code);
    /* the swizzle R, G, B, A: a view of its own components */
    u32 tic[8] = {2u << 19 | 3u << 22 | 4u << 25 | 5u << 28}, swizzled[8] = {3u << 19 | 3u << 22 | 4u << 25 | 5u << 28};
    ShTex plain = {0}, shadow = {0};
    Surf tex = {0};
    shadow.shadow = 1;
    tex.f.vk = VK_FORMAT_R32_SFLOAT;
    tex.levels = 1;
    /* the device takes the feature wherever the GPU has it */
    VkPhysicalDeviceVulkan12Features has12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 has = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &has12};
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &has);
    CHECK(g_vk.feat12.samplerFilterMinmax == has12.samplerFilterMinmax);
    int minmax = vk_format_minmax_filter(VK_FORMAT_R32_SFLOAT);
    printf("Sampler reduction: %s\n", minmax ? "the device filters R32F min/max" : "no min/max filter for R32F");
    /* a linear filter of R32F is optional: without it the reads below would be invalid */
    VkFormatProperties r32f;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, VK_FORMAT_R32_SFLOAT, &r32f);
    int filtered = (r32f.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    if (!filtered) printf("Sampler reduction: R32F has no linear filter, its reads are skipped\n");
    /* clamp to edge; linear magnification and minification, no mip filter; mode the reduction */
    for (u32 mode = 0; filtered && mode < 4; mode++) {
        u32 tsc[8] = {2u | 2u << 3 | 2u << 6, 2u | 2u << 4 | 1u << 6 | mode << 10};
        VkSampler sampler = vk_tex_bind_sampler(tsc, tic, &plain, 0, &tex, VK_IMAGE_VIEW_TYPE_2D);
        u32 bits = 0;
        float got;
        CHECK(sampler && !run_compute(code, words, &bits, 1, view, sampler));
        memcpy(&got, &bits, 4);
        if (got != (minmax || mode == 3 ? want[mode] : 0.5f)) {
            fprintf(stderr, "vulkan_headless: reduction %u read %g between 0 and 1\n", mode, got);
            return 1;
        }
    }
    free(code);
    u32 average[8] = {2u | 2u << 3 | 2u << 6, 2u | 2u << 4 | 1u << 6}, least[8];
    memcpy(least, average, sizeof least);
    least[1] |= 1u << 10;
    VkSampler avg = vk_tex_bind_sampler(average, tic, &plain, 0, &tex, VK_IMAGE_VIEW_TYPE_2D);
    CHECK(vk_tex_bind_sampler(least, tic, &plain, 0, NULL, VK_IMAGE_VIEW_TYPE_2D) ==
          vk_tex_bind_sampler(average, tic, &plain, 0, NULL, VK_IMAGE_VIEW_TYPE_2D));
    CHECK(vk_tex_bind_sampler(least, tic, &plain, 1, &tex, VK_IMAGE_VIEW_TYPE_2D) ==
          vk_tex_bind_sampler(average, tic, &plain, 1, &tex, VK_IMAGE_VIEW_TYPE_2D));
    CHECK(vk_tex_bind_sampler(least, tic, &shadow, 0, &tex, VK_IMAGE_VIEW_TYPE_2D) ==
          vk_tex_bind_sampler(average, tic, &shadow, 0, &tex, VK_IMAGE_VIEW_TYPE_2D));
    u32 nearest[8] = {average[0], 1u | 1u << 4 | 1u << 6}, nearest_least[8] = {average[0], nearest[1] | 1u << 10};
    CHECK(vk_tex_bind_sampler(nearest_least, tic, &plain, 0, &tex, VK_IMAGE_VIEW_TYPE_2D) ==
          vk_tex_bind_sampler(nearest, tic, &plain, 0, &tex, VK_IMAGE_VIEW_TYPE_2D));
    VkBool32 had = g_vk.feat12.samplerFilterMinmax;
    g_vk.feat12.samplerFilterMinmax = VK_FALSE;
    CHECK(vk_tex_bind_sampler(least, tic, &plain, 0, &tex, VK_IMAGE_VIEW_TYPE_2D) == avg);
    g_vk.feat12.samplerFilterMinmax = had;
    CHECK(!minmax || vk_tex_bind_sampler(least, tic, &plain, 0, &tex, VK_IMAGE_VIEW_TYPE_2D) != avg);
    /* a device whose min/max filter takes no swizzle: a view of its own components keeps the reduction, and one that
       reads g into r averages */
    int had_swizzle = g_vk.has_minmax_swizzle;
    g_vk.has_minmax_swizzle = 0;
    CHECK(vk_tex_bind_sampler(least, swizzled, &plain, 0, &tex, VK_IMAGE_VIEW_TYPE_2D) == avg);
    CHECK(!minmax || vk_tex_bind_sampler(least, tic, &plain, 0, &tex, VK_IMAGE_VIEW_TYPE_2D) != avg);
    g_vk.has_minmax_swizzle = 1;
    CHECK(!minmax || vk_tex_bind_sampler(least, swizzled, &plain, 0, &tex, VK_IMAGE_VIEW_TYPE_2D) != avg);
    g_vk.has_minmax_swizzle = had_swizzle;
    vkDestroyImageView(g_vk.dev, view, NULL);
    vmaDestroyImage(g_vk.vma, image, image_memory);
    return 0;
}

/* The per-instruction FTZ flush (spv_emit_flush_subnormal) on this device, against the console's: a subnormal becomes
   the zero of its sign, and zeros, normals, infinities and NaNs (their payloads too) keep their bits. The float test
   only where the device keeps signed zeros, as the translator uses it */
static int check_flush_subnormal(void) {
    static const u32 in[] = {0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u, 0x007fffffu, 0x807fffffu,
                             0x00400000u, 0x80400000u, 0x00000123u, 0x80654321u, 0x00800000u, 0x80800000u,
                             0x00800001u, 0x3f800000u, 0xbf800000u, 0x7f7fffffu, 0xff7fffffu, 0x7f800000u,
                             0xff800000u, 0x7fc00000u, 0xffc00000u, 0x7fa00001u, 0x7fc12345u, 0x0d000000u,
                             0x8c000000u, 0x34000000u, 0x00012345u, 0x80000100u, 0x01000000u, 0x81000000u,
                             0x3e800000u, 0x42f60000u};
    enum { N = sizeof in / sizeof *in };
    for (int signed_zeros = 0; signed_zeros < 2; signed_zeros++) {
        if (signed_zeros && !g_vk_preserve_infnan) continue;
        size_t words = 0;
        u32* code = snx_test_spirv_flush(N, signed_zeros, &words), data[2 * N] = {0};
        memcpy(data, in, sizeof in);
        int r = run_storage_compute(code, words, data, 2 * N);
        free(code);
        CHECK(!r);
        for (u32 k = 0; k < N; k++) {
            u32 want = (in[k] & 0x7f800000u) ? in[k] : in[k] & 0x80000000u;
            if (data[N + k] != want) {
                printf("flush %s: %08x gave %08x, not %08x\n", signed_zeros ? "float test" : "bits", in[k], data[N + k],
                       want);
                return 1;
            }
        }
    }
    puts("subnormal flush: every input's bits as the console's FTZ gives them");
    return 0;
}

/* VIC through the compute shader: raw YUV and alpha, odd starts kept, luma-only NV12, an NV21 slot over the background
   colour, the background with no slot, bytes outside the target and the slots untouched, a colour matrix, and NV12
   rows that aren't word aligned (the shader's unaligned build). The planes are guest memory, as VIC's always are: the
   picture stays on the GPU (vk/vic.c publishes it), and reading it here is a CPU access to GPU-owned memory, which
   the trap answers by landing it (memory/own.c). Each case's output starts where the last one's picture is owned, so
   the next job copies what the GPU holds of it on the GPU */
#define VIC_BASE (HEAP_BASE + 0x100000)
static int check_vic(void) {
    enum { W = 32, H = 16, SENTINEL = 0xA5 };
    u32 validation = g_vk_validation_messages; /* the job's first words are a uniform buffer (vk/vic.c, owned.c) */
    u8 *y = GP(VIC_BASE), *uv = GP(VIC_BASE + 0x1000), *out = GP(VIC_BASE + 0x2000);
    u64 lands = own_landings();
    for (u32 j = 0; j < H; ++j)
        for (u32 i = 0; i < W; ++i) y[j * W + i] = (u8)(16 + i + 4 * j);
    for (u32 j = 0; j < H / 2; ++j)
        for (u32 i = 0; i < W / 2; ++i) {
            uv[j * W + i * 2] = (u8)(40 + i + 8 * j);
            uv[j * W + i * 2 + 1] = (u8)(240 - i - 8 * j);
        }
    static const struct {
        u32 fmt, sx0, sy0, dx0, dy0, w, h; /* w 0: no slot */
        u32 vu, tx0, ty0, tw, th; /* an NV21 slot, the target rectangle the background fills (and the slot lies in, as
                                     vic.c's slot_resolve clips it) */
        u32 pad, mat;             /* bytes added to the output's pitch, the matrix below */
    } cases[] = {{0x1F, 0, 0, 0, 0, W, H, 0, 0, 0, W, H, 0, 0},   {0x1F, 3, 1, 5, 3, 10, 7, 0, 5, 3, 10, 7, 0, 0},
                 {0x44, 0, 0, 0, 0, W, H, 0, 0, 0, W, H, 0, 0},   {0x1F, 3, 1, 5, 3, 10, 7, 1, 2, 1, 20, 12, 0, 0},
                 {0x1F, 0, 0, 0, 0, 0, 0, 0, 4, 2, 6, 9, 0, 0},   {0x20, 2, 2, 5, 3, 10, 7, 0, 2, 1, 20, 12, 0, 1},
                 {0x44, 2, 2, 4, 2, 20, 9, 0, 2, 2, 26, 12, 1, 1}};
    /* BT.601 limited range in the 10 bit domain, S12.8, with a shift of 1: out = ((Y*c0 + U*c1 + V*c2) >> 1 + c3) >> 8 */
    static const int m601[3][4] = {{596, 0, 818, -(298 * 64 + 409 * 512)},
                                   {596, -200, -416, -(298 * 64) + (100 + 208) * 512},
                                   {596, 1032, 0, -(298 * 64 + 516 * 512)}};
    static const u32 bg[4] = {1023, 512, 0, 1023}; /* 8 bit 255, 128, 0, 255 */
    for (u32 c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
        const int rgba = cases[c].fmt != 0x44;
        const u32 bpp = rgba ? 4 : 1;
        VicJob job;
        memset(&job, 0, sizeof job);
        memset(out, SENTINEL, W * 4 * H + 64); /* a CPU write: what the GPU held there lands first */
        job.fmt = cases[c].fmt;
        job.sw = W;
        job.sh = H;
        const u32 pitch = W * bpp + cases[c].pad;
        job.oy = (VicPlane){out, (u64)pitch * H, pitch, H, 0, 0};
        job.tx0 = cases[c].tx0;
        job.ty0 = cases[c].ty0;
        job.tw = cases[c].tw;
        job.th = cases[c].th;
        memcpy(job.bg, bg, sizeof job.bg);
        VicSlot* sl = &job.slots[0];
        sl->on = cases[c].w != 0;
        sl->vu = cases[c].vu;
        sl->sx0 = cases[c].sx0;
        sl->sy0 = cases[c].sy0;
        sl->dx0 = cases[c].dx0;
        sl->dy0 = cases[c].dy0;
        sl->w = cases[c].w;
        sl->h = cases[c].h;
        sl->alpha = 0x3FF;
        sl->clamp_lo = 64;
        sl->clamp_hi = 0x3FF - 64;
        sl->mat_enable = (int)cases[c].mat;
        sl->mat_shift = 1;
        memcpy(sl->mat, m601, sizeof m601);
        sl->iy = (VicPlane){y, W * H, W, H, 0, 0};
        sl->ic = (VicPlane){uv, W * H / 2, W, H / 2, 0, 0};
        CHECK(g_renderer->vic_compose(&job));
        CHECK(!own_traps() || own_lookup(VIC_BASE + 0x2000, (u64)pitch * H, NULL, 0) > 0); /* on the GPU until read */
        for (u32 j = 0; j < H; ++j)
            for (u32 i = 0; i < W; ++i) {
                const u8* o = out + j * pitch + i * bpp;
                const int in = i >= sl->dx0 && i < sl->dx0 + sl->w && j >= sl->dy0 && j < sl->dy0 + sl->h;
                const int target = i >= job.tx0 && i < job.tx0 + job.tw && j >= job.ty0 && j < job.ty0 + job.th;
                const u32 rb = cases[c].fmt == 0x20 ? 2 : 0; /* A8R8G8B8 stores R and B the other way around */
                if (!in && !target) {
                    CHECK(o[0] == SENTINEL);
                    continue;
                }
                if (!in) {
                    CHECK(rgba ? o[rb] == 255 && o[1] == 128 && o[2 - rb] == 0 && o[3] == 255 : o[0] == 255);
                    continue;
                }
                const u32 sx = sl->sx0 + i - sl->dx0, sy = sl->sy0 + j - sl->dy0;
                const u8* p = uv + (sy / 2) * W + (sx & ~1u);
                const int q[3] = {y[sy * W + sx] << 2, p[sl->vu] << 2, p[1 - sl->vu] << 2};
                int v[3];
                for (int k = 0; k < 3; ++k) {
                    v[k] = q[k];
                    if (!sl->mat_enable) continue;
                    v[k] = (((q[0] * m601[k][0] + q[1] * m601[k][1] + q[2] * m601[k][2]) >> 1) + m601[k][3]) >> 8;
                    v[k] = v[k] < 64 ? 64 : v[k] > 0x3FF - 64 ? 0x3FF - 64 : v[k];
                }
                if (!rgba)
                    CHECK(o[0] == v[0] >> 2);
                else
                    CHECK(o[rb] == v[0] >> 2 && o[1] == v[1] >> 2 && o[2 - rb] == v[2] >> 2 && o[3] == 255);
            }
    }
    /* each picture was read here: landed once, by the trap, where the host has traps */
    CHECK(!own_traps() || own_landings() >= lands + sizeof cases / sizeof cases[0]);
    CHECK(g_vk_validation_messages == validation);
    return 0;
}
/* two jobs into the same output with no CPU access between: the second's picture outside its target rectangle is the
   first one's, which the GPU held and copied on the GPU (one landing, when it is read at the end) */
static int check_vic_chain(const char* shot) {
    enum { W = 32, H = 16 };
    u8 *y = GP(VIC_BASE), *uv = GP(VIC_BASE + 0x1000), *out = GP(VIC_BASE + 0x2000);
    static const u32 bg[4] = {1023, 512, 0, 1023};
    VicJob job;
    u64 lands;
    memset(out, 0, W * 4 * H);
    memset(&job, 0, sizeof job);
    job.fmt = 0x1F;
    job.sw = W;
    job.sh = H;
    job.oy = (VicPlane){out, (u64)W * 4 * H, W * 4, H, 0, 0};
    job.tw = W;
    job.th = H;
    memcpy(job.bg, bg, sizeof job.bg);
    job.slots[0] = (VicSlot){.on = 1, .w = W, .h = H, .alpha = 0x3FF, .clamp_hi = 0x3FF};
    job.slots[0].iy = (VicPlane){y, W * H, W, H, 0, 0};
    job.slots[0].ic = (VicPlane){GP(VIC_BASE + 0x1000), W * H / 2, W, H / 2, 0, 0};
    CHECK(g_renderer->vic_compose(&job));
    memset(&job.slots[0], 0, sizeof job.slots[0]); /* the second: the background in a smaller rectangle */
    job.tx0 = 4, job.ty0 = 2, job.tw = 6, job.th = 9;
    CHECK(g_renderer->vic_compose(&job));
    lands = own_landings();
    /* the display takes the picture on the GPU, where it is: nothing lands */
    {
        const i32 full[4] = {0, 0, 0, 0};
        u64 expected = 1469598103934665603ull;
        for (u32 j = 0; j < H; ++j)
            for (u32 i = 0; i < W; ++i) {
                int t = i >= 4 && i < 10 && j >= 2 && j < 11;
                const u8* c = uv + (j / 2) * W + (i & ~1u);
                u32 px = t ? 0xFF0080FFu : y[j * W + i] | (u32)c[0] << 8 | (u32)c[1] << 16 | 0xFF000000u;
                expected = (expected ^ px) * 1099511628211ull;
            }
        g_renderer->present_buffer(VIC_BASE + 0x2000, W, H, W * 4, 0, 0, 1, full, 0, 0, 0, 0);
        CHECK(g_renderer->screenshot(shot) == expected);
        CHECK(own_landings() == lands);
    }
    for (u32 j = 0; j < H; ++j)
        for (u32 i = 0; i < W; ++i) {
            const u8* o = out + (j * W + i) * 4;
            if (i >= 4 && i < 10 && j >= 2 && j < 11)
                CHECK(o[0] == 255 && o[1] == 128 && o[2] == 0 && o[3] == 255);
            else
                CHECK(o[0] == y[j * W + i] && o[3] == 255); /* the first job's: the samples as they are */
        }
    CHECK(!own_traps() || own_landings() == lands + 1);
    return 0;
}
/* NVDEC's picture laid out on the GPU (vk/nvdec.c), then composed by VIC from there, with no CPU access between: the
   same steps on the CPU's references (nvdec_ref.c, vic_ref.c) into twin surfaces must give the same bytes, the GPU's
   read last (which lands them). Pictures whose rows end inside a 16 byte run, and surface padding that must keep its
   bytes */
#define NV_GPU (HEAP_BASE + 0x40000)
#define NV_REF (HEAP_BASE + 0xC0000)
static int nvdec_vic_case(u32 w, u32 h, u32 pitch, u32 sh, u32 bh) {
    static u8 y[64 * 48], u[32 * 24], v[32 * 24];
    u32 pairs = (w + 1) / 2;
    u64 gob_rows = 8u << bh, ext_y = (u64)(pitch + 63) / 64 * 64 * ((sh + gob_rows - 1) / gob_rows * gob_rows);
    u64 ext_c = (u64)(pitch + 63) / 64 * 64 * (((sh + 1) / 2 + gob_rows - 1) / gob_rows * gob_rows);
    u64 out_size = (u64)w * 4 * h;
    for (u32 i = 0; i < w * h; i++) y[i] = (u8)(i * 7 + 3);
    for (u32 i = 0; i < pairs * ((h + 1) / 2); i++) u[i] = (u8)(i * 3 + 1), v[i] = (u8)(i * 5 + 2);
    /* the twins: the same bytes before (padding included), luma, then chroma, then the output */
    for (int k = 0; k < 2; k++) {
        u8* base = GP(k ? NV_REF : NV_GPU);
        for (u64 i = 0; i < 0x40000; i++) base[i] = (u8)(i * 13 + 5);
    }
    NvdecPic p = {{y, u, v}, {(int)w, (int)pairs, (int)pairs}, w, h, NV_GPU, NV_GPU + 0x10000, ext_y, ext_c,
                  pitch, pitch, sh, bh};
    NvdecPic q = p;
    q.luma = NV_REF;
    q.chroma = NV_REF + 0x10000;
    CHECK(g_renderer->nvdec_write(&p));
    nvdec_write_ref(&q);
    /* VIC: the picture as an RGBA one, from the surfaces as NVDEC left them */
    for (int k = 0; k < 2; k++) {
        u64 at = k ? NV_REF : NV_GPU;
        VicJob job;
        memset(&job, 0, sizeof job);
        job.fmt = 0x1F;
        job.sw = w;
        job.sh = h;
        job.oy = (VicPlane){GP(at + 0x20000), out_size, w * 4, h, 0, 0};
        job.tw = w;
        job.th = h;
        job.slots[0] = (VicSlot){.on = 1, .w = w & ~1u, .h = h & ~1u, .alpha = 0x3FF, .clamp_hi = 0x3FF};
        job.slots[0].iy = (VicPlane){GP(at), ext_y, pitch, sh, 1, bh};
        job.slots[0].ic = (VicPlane){GP(at + 0x10000), ext_c, pitch, (sh + 1) / 2, 1, bh};
        if (k)
            vic_compose_ref(&job);
        else
            CHECK(g_renderer->vic_compose(&job));
    }
    /* the GPU's bytes, read now (landing them), against the references' */
    CHECK(!memcmp(GP(NV_GPU), GP(NV_REF), ext_y));
    CHECK(!memcmp(GP(NV_GPU + 0x10000), GP(NV_REF + 0x10000), ext_c));
    CHECK(!memcmp(GP(NV_GPU + 0x20000), GP(NV_REF + 0x20000), out_size));
    return 0;
}
static int check_nvdec_vic(void) {
    u64 lands = own_landings();
    CHECK(!nvdec_vic_case(64, 32, 64, 32, 1));
    CHECK(!nvdec_vic_case(33, 17, 64, 24, 0)); /* rows ending inside a run, padding rows, blocks of one GOB */
    CHECK(!nvdec_vic_case(54, 40, 128, 48, 2)); /* a wider surface than the picture */
    /* each case lands its three ranges when read, nothing before */
    CHECK(!own_traps() || own_landings() <= lands + 9);
    return 0;
}

/* The DMA copy engine on the GPU (vk/dma.c) against its CPU reference (dma_ref.c), each on one of two twin regions of
   guest memory mapped into the GPU's address space: pitch and block linear sides, origins not on 16 bytes, lines
   past the surface, the remap's shuffles, constants and kept bytes */
#define DMA_A (HEAP_BASE + 0x400000) /* the GPU's twin, at GPU address DMA_GA */
#define DMA_B (HEAP_BASE + 0x600000) /* the reference's, at DMA_GB */
#define DMA_GA 0x200000000ull
#define DMA_GB 0x300000000ull
#define DMA_SPAN 0x100000u
static int dma_case(GpuDmaOp o) {
    GpuDmaOp g = o, c = o;
    for (int k = 0; k < 2; k++) { /* the same bytes in both */
        u8* t = GP(k ? DMA_B : DMA_A);
        for (u32 i = 0; i < DMA_SPAN; i++) t[i] = (u8)(i * 29 + (i >> 9) * 7 + 3);
    }
    g.src.addr += DMA_GA, g.dst.addr += DMA_GA;
    c.src.addr += DMA_GB, c.dst.addr += DMA_GB;
    CHECK(g_renderer->dma_copy(&g));
    dma_copy_ref(&c);
    CHECK(!memcmp(GP(DMA_A), GP(DMA_B), DMA_SPAN)); /* reading the GPU's lands it */
    return 0;
}
/* the same with a linear source read at submission into the renderer's memory (maxwell/dma.c's snapshot): the copy
   takes those bytes, not guest memory's, which changed after (the program freed its source), and lets go of them */
static int dma_snap_case(GpuDmaOp o) {
    GpuDmaOp g = o, c = o;
    u64 span = (u64)o.src.pitch * (o.lines - 1) + o.line_in, id = 0, held = vk_snap_bytes();
    for (int k = 0; k < 2; k++) {
        u8* t = GP(k ? DMA_B : DMA_A);
        for (u32 i = 0; i < DMA_SPAN; i++) t[i] = (u8)(i * 31 + (i >> 9) * 5 + 1);
    }
    g.src.addr += DMA_GA, g.dst.addr += DMA_GA;
    c.src.addr += DMA_GB, c.dst.addr += DMA_GB;
    u8* snap = g_renderer->snap_alloc(span, &id);
    CHECK(snap && id && vk_snap_bytes() == held + span);
    memcpy(snap, GP(DMA_A) + o.src.addr, span);
    for (u64 i = 0; i < span; i++) GP(DMA_A)[o.src.addr + i] ^= 0x5A; /* guest memory changes after the read */
    g.snap = snap;
    g.snap_id = id;
    CHECK(g_renderer->dma_copy(&g));
    for (u64 i = 0; i < span; i++) GP(DMA_A)[o.src.addr + i] ^= 0x5A; /* the same again, for the comparison */
    dma_copy_ref(&c);
    CHECK(!memcmp(GP(DMA_A), GP(DMA_B), DMA_SPAN));
    vk_lock();
    for (int i = 0; i <= VK_FRAMES; i++) { /* the copy's frame slot used again, its work done: its memory went */
        vk_frame_next();
        vk_cmd();
    }
    vk_unlock();
    CHECK(vk_snap_bytes() == held);
    return 0;
}
static GpuDmaSide pitch_side(u64 addr, u32 pitch) {
    GpuDmaSide s = {addr, 0, pitch, 0, 0, 0, 0, 0, 0, 0};
    return s;
}
static GpuDmaSide bl_side(u64 addr, u32 wb, u32 h, u32 bh, u32 ox, u32 oy) {
    GpuDmaSide s = {addr, 1, 0, wb, h, 0, bh, 0, ox, oy};
    return s;
}
static int check_dma(void) {
    GpuDmaOp o;
    gmmu_map(DMA_GA, DMA_A, DMA_SPAN);
    gmmu_map(DMA_GB, DMA_B, DMA_SPAN);
    /* a one-dimensional copy */
    memset(&o, 0, sizeof o);
    o.width = o.line_in = o.line_out = 1001;
    o.lines = 1;
    o.src = pitch_side(0x100, 0);
    o.dst = pitch_side(0x80003, 0);
    CHECK(!dma_case(o));
    CHECK(!dma_snap_case(o));
    /* pitch to block linear, a window of a surface (a texture upload) */
    memset(&o, 0, sizeof o);
    o.multi = 1;
    o.width = o.line_in = o.line_out = 80;
    o.lines = 20;
    o.src = pitch_side(0x1000, 100);
    o.dst = bl_side(0x80000, 128, 40, 2, 16, 3);
    CHECK(!dma_case(o));
    CHECK(!dma_snap_case(o));
    /* block linear to pitch, from an origin not on 16 bytes, lines past the surface reading zero */
    o.src = bl_side(0x2000, 192, 40, 1, 7, 35);
    o.dst = pitch_side(0x90001, 120);
    o.width = o.line_in = o.line_out = 101;
    o.lines = 10;
    CHECK(!dma_case(o));
    /* block linear to block linear, lines past the destination dropped */
    o.src = bl_side(0x4000, 256, 64, 3, 32, 5);
    o.dst = bl_side(0xA0000, 200, 30, 0, 9, 22);
    o.width = o.line_in = o.line_out = 150;
    o.lines = 16;
    CHECK(!dma_case(o));
    /* the remap: B and R swapped (4 components of a byte), pitch to pitch */
    memset(&o, 0, sizeof o);
    o.multi = o.remap = o.shuffle = 1;
    o.cs = 1, o.ns = 4, o.nd = 4;
    o.sel[0] = 2, o.sel[1] = 1, o.sel[2] = 0, o.sel[3] = 3;
    o.width = 33;
    o.line_in = o.line_out = 33 * 4;
    o.lines = 7;
    o.src = pitch_side(0x100, 140);
    o.dst = pitch_side(0x80002, 150);
    CHECK(!dma_case(o));
    CHECK(!dma_snap_case(o));
    /* constants only (a fill), 2 byte components, into block linear */
    o.fill = 1, o.shuffle = 0;
    o.cs = 2, o.ns = 2, o.nd = 2;
    o.sel[0] = 4, o.sel[1] = 5;
    o.const_a = 0x1234ABCD, o.const_b = 0x5678EF01;
    o.width = 21;
    o.line_in = o.line_out = 21 * 4;
    o.lines = 9;
    o.dst = bl_side(0x80000, 128, 16, 1, 6, 2);
    CHECK(!dma_case(o));
    /* a component kept: the destination's own bytes stay, 3 components out of 2 in */
    o.fill = 0, o.shuffle = 1, o.keep = 1;
    o.cs = 1, o.ns = 2, o.nd = 3;
    o.sel[0] = 1, o.sel[1] = 6, o.sel[2] = 0;
    o.width = 40;
    o.line_in = 80, o.line_out = 120;
    o.lines = 6;
    o.src = pitch_side(0x3000, 90);
    o.dst = bl_side(0x80000, 256, 16, 0, 5, 1);
    CHECK(!dma_case(o));
    return 0;
}

/* The 2D engine's blit from memory on the GPU (vk/blit.c) against its CPU reference (fermi2d_ref.c), on the DMA
   test's twin regions: block linear and pitch planes, windows not on 16 bytes, scaled up and down, 1 to 16 bytes a
   pixel, and conversions between float formats (fmt), the regions' bytes holding NaNs, infinities and subnormals */
static int blit_case(GpuDmaSide src, GpuDmaSide dst, u32 bpp, u32 x0, u32 y0, u32 w, u32 h, u32 sw, u32 sh, u32 fmt) {
    static u32 scol[256], srow[256];
    GpuBlit g, c;
    for (u32 i = 0; i < w; i++) scol[i] = (u32)((u64)i * sw / w); /* nearest, the source scaled onto the window */
    for (u32 i = 0; i < h; i++) srow[i] = (u32)((u64)(h - 1 - i) * sh / h); /* flipped */
    for (int k = 0; k < 2; k++) {
        u8* t = GP(k ? DMA_B : DMA_A);
        for (u32 i = 0; i < DMA_SPAN; i++) t[i] = (u8)(i * 13 + (i >> 8) * 5 + 1);
    }
    g = (GpuBlit){src, dst, bpp, x0, x0 + w, y0, y0 + h, scol, srow, fmt};
    c = g;
    g.src.addr += DMA_GA, g.dst.addr += DMA_GA;
    c.src.addr += DMA_GB, c.dst.addr += DMA_GB;
    CHECK(g_renderer->blit_mem(&g));
    blit_mem_ref(&c);
    CHECK(!memcmp(GP(DMA_A), GP(DMA_B), DMA_SPAN));
    return 0;
}
static GpuDmaSide pitch_plane(u64 addr, u32 pitch, u32 wb, u32 h) { /* as fermi2d.c's f2d_plane makes them */
    GpuDmaSide s = pitch_side(addr, pitch);
    s.wb = wb, s.h = h;
    return s;
}
static int check_blit(void) {
    /* block linear halved into block linear, an odd window */
    CHECK(!blit_case(bl_side(0x1000, 128 * 4, 64, 2, 0, 0), bl_side(0x80000, 64 * 4, 32, 1, 0, 0), 4, 5, 3, 21, 9,
                     42, 18, 0));
    /* pitch scaled up into block linear, 2 bytes a pixel */
    CHECK(!blit_case(pitch_plane(0x3000, 70, 31 * 2, 12), bl_side(0x80000, 100 * 2, 40, 0, 0, 0), 2, 7, 1, 90, 33, 31, 12, 0));
    /* block linear into pitch, 16 bytes a pixel, a source whose rows end inside a 16 byte run */
    CHECK(!blit_case(bl_side(0x1000, 37 * 16, 24, 1, 0, 0), pitch_plane(0x90003, 40 * 16, 40 * 16, 24), 16, 2, 2, 30, 20, 37, 24, 0));
    /* pitch to pitch, a byte a pixel */
    CHECK(!blit_case(pitch_plane(0x101, 77, 77, 30), pitch_plane(0x90001, 99, 99, 15), 1, 3, 4, 95, 11, 77, 30, 0));
    /* 8 bytes a pixel, block linear 3 GOBs a block high */
    CHECK(!blit_case(bl_side(0x2000, 50 * 8, 48, 3, 0, 0), bl_side(0xA0000, 40 * 8, 20, 1, 0, 0), 8, 1, 0, 39, 20,
                     50, 48, 0));
    /* RGBA32F pitch into RGBA16F block linear, an odd window scaled down */
    CHECK(!blit_case(pitch_plane(0x3000, 40 * 16, 40 * 16, 20), bl_side(0x80000, 64 * 8, 32, 1, 0, 0), 16, 5, 3, 21, 9,
                     40, 20, GPU_BLIT_FLOAT(4, 4, 4, 2, 0)));
    /* R16F block linear into RGBA32F pitch: green and blue 0, alpha 1 */
    CHECK(!blit_case(bl_side(0x1000, 33 * 2, 24, 1, 0, 0), pitch_plane(0x90000, 40 * 16, 40 * 16, 24), 2, 2, 1, 37, 20,
                     33, 24, GPU_BLIT_FLOAT(1, 2, 4, 4, 0)));
    /* RGBX16F into RG32F, and R32F into R16F an odd number of pixels wide (half words left at the rows' ends) */
    CHECK(!blit_case(bl_side(0x1000, 30 * 8, 16, 0, 0, 0), bl_side(0xA0000, 50 * 8, 20, 1, 0, 0), 8, 1, 2, 45, 17,
                     30, 16, GPU_BLIT_FLOAT(4, 2, 2, 4, 1)));
    CHECK(!blit_case(pitch_plane(0x101, 77 * 4, 77 * 4, 30), pitch_plane(0x90002, 99 * 2, 99 * 2, 15), 4, 3, 4, 95,
                     11, 77, 30, GPU_BLIT_FLOAT(1, 4, 1, 2, 0)));
    return 0;
}

/* a block linear A8B8G8R8 target of w by h at gva (blocks 2^bh GOBs high) cleared to red or blue */
static void rt_clear_format(GpuChannel* ch, u64 gva, u32 w, u32 h, u32 bh, int blue, u32 format) {
    u32* r = ch->r3d;
    const float color[4] = {blue ? 0.0f : 1.0f, 0, blue ? 1.0f : 0.0f, 1};
    r[0x487] = 1;
    r[0x200] = (u32)(gva >> 32);
    r[0x201] = (u32)gva;
    r[0x202] = w;
    r[0x203] = h;
    r[0x204] = format;
    r[0x205] = bh << 4;
    r[0x206] = 1;
    r[0x207] = 0;
    memcpy(&r[0x360], color, sizeof color);
    engine_clear(ch, 15 << 2);
    vk_render_end();
}
static void rt_clear(GpuChannel* ch, u64 gva, u32 w, u32 h, u32 bh, int blue) {
    rt_clear_format(ch, gva, w, h, bh, blue, 0xD5);
}
/* n bytes at p all blue pixels */
static int all_blue(const u8* p, u32 n) {
    static const u8 blue[4] = {0, 0, 255, 255};
    for (u32 i = 0; i < n; i += 4)
        if (memcmp(p + i, blue, 4)) return 0;
    return 1;
}
/* n bytes at p all v */
static int all_bytes(const u8* p, u32 n, u8 v) {
    for (u32 i = 0; i < n; i++)
        if (p[i] != v) return 0;
    return 1;
}
/* Render targets over one another in the DMA test's region, as when a program frees an image and makes a bigger one
   there: a red 64 by 64 target 64 KiB into a blue 160 by 128 one cleared after it is its block columns 8 and 9 (pixels
   128 to 159 of every row). The newer one's pixels are on top in a DMA copy of it, and in guest memory once both are
   written back, together or the newer one alone. A target reaching past a copy's range is one whose memory a smaller
   surface took over: the copy takes what the program wrote there since */
static int check_rt_overlap(void) {
    GpuChannel* ch = calloc(1, sizeof *ch);
    GpuDmaOp o;
    u64 readbacks;
    CHECK(ch);
    memset(&o, 0, sizeof o);
    o.multi = 1;
    o.width = o.line_in = o.line_out = 160 * 4;
    o.lines = 128;
    o.src = bl_side(DMA_GA, 160 * 4, 128, 4, 0, 0);
    o.dst = pitch_side(DMA_GA + 0x40000, 160 * 4);
    rt_clear(ch, DMA_GA + 0x10000, 64, 64, 3, 0);
    rt_clear(ch, DMA_GA, 160, 128, 4, 1);
    CHECK(g_renderer->dma_copy(&o));
    CHECK(all_blue(GP(DMA_A + 0x40000), 160 * 128 * 4)); /* reading it lands it */
    vk_rt_before_write(DMA_GA, 0x14000);
    CHECK(all_blue(GP(DMA_A), 0x14000));
    /* the newer one written back alone: the older one first, under it, and not again after */
    rt_clear(ch, DMA_GA + 0x10000, 64, 64, 3, 0);
    rt_clear(ch, DMA_GA, 160, 128, 4, 1);
    g_renderer->rt_flush(DMA_GA);
    vk_rt_before_write(DMA_GA, 0x14000);
    CHECK(all_blue(GP(DMA_A), 0x14000));
    /* a 160 by 256 target, the program's own bytes over its memory after it, and a copy of the 160 by 128 surface */
    rt_clear(ch, DMA_GA, 160, 256, 4, 1);
    memset(GP(DMA_A), 0x11, 0x28000);
    readbacks = g_vk_rt_readbacks;
    CHECK(g_renderer->dma_copy(&o));
    CHECK(g_vk_rt_readbacks == readbacks + 1);
    CHECK(all_bytes(GP(DMA_A + 0x40000), 160 * 128 * 4, 0x11));
    /* A render target and a newer buffer can use different GPU aliases of the
       same memory. A later partial read of the image must not restore its old
       pixels over the buffer written through the other alias. */
    const u64 alias = DMA_GA + 0x800000;
    gmmu_map(alias, DMA_A, DMA_SPAN);
    rt_clear(ch, alias, 160, 256, 4, 1);
    memset(GP(DMA_B), 0x6d, 512);
    o.src = pitch_side(DMA_GB, 512);
    o.dst = pitch_side(DMA_GA + 0x10000, 512);
    o.width = o.line_in = o.line_out = 512;
    o.lines = 1;
    CHECK(g_renderer->dma_copy(&o));
    o.src = pitch_side(alias + 0x1000, 512);
    o.dst = pitch_side(DMA_GA + 0x40000, 512);
    CHECK(g_renderer->dma_copy(&o));
    CHECK(all_bytes(GP(DMA_A + 0x10000), 512, 0x6d));
    CHECK(all_blue(GP(DMA_A), 512));
    /* Replacing a GPU mapping must resolve the old target against its old
       backing, before an image read can reach the replacement buffer. */
    rt_clear(ch, alias, 160, 256, 4, 1);
    memset(GP(DMA_B), 0x37, 0x28000);
    gmmu_map(alias, DMA_B, DMA_SPAN);
    CHECK(all_blue(GP(DMA_A), 0x28000));
    o.src = pitch_side(alias + 0x1000, 512);
    o.dst = pitch_side(DMA_GA + 0x40000, 512);
    CHECK(g_renderer->dma_copy(&o));
    CHECK(all_bytes(GP(DMA_B), 0x28000, 0x37));
    CHECK(all_bytes(GP(DMA_A + 0x40000), 512, 0x37));
    /* Dirty metadata queries must leave GPU-owned pixels on the GPU. */
    const u64 tracked = DMA_GA + 0xe0000, tracked_cpu = DMA_A + 0xe0000;
    rt_clear(ch, tracked, 64, 64, 3, 1);
    vk_lock();
    Surf* target = vk_rt_find(tracked);
    CHECK(target && vk_surf_publish(target));
    int tracking = vk_written_since(gmem_host(tracked, 0x4000), 0x4000, 0) >= 0;
    u64 landings = own_landings();
    vk_rt_drawn_full(target);
    (void)vk_rt_drawn_fresh(target);
    CHECK(own_landings() == landings);
    if (own_traps()) CHECK(own_lookup(tracked_cpu, 0x4000, NULL, 0) > 0);
    vk_unlock();
    memset(GP(tracked_cpu + 16), 0x39, 16);
    g_renderer->mem_may_have_changed();
    vk_lock();
    if (tracking) CHECK(!vk_rt_drawn_fresh(target));
    vk_unlock();
    /* A target the program wrote part of after its draw, as when a game gives a target's memory to its command lists:
       a GPU write in its span writes it back, but for the pages the program wrote, which keep its bytes. The program
       wrote them after the GPU drew there. Without write tracking, the write-back can't tell them */
    rt_clear(ch, DMA_GA, 160, 256, 4, 1);
    memset(GP(DMA_A + 0x8000), 0x5a, 0x2000);
    g_renderer->mem_may_have_changed();
    vk_rt_before_write(DMA_GA + 0x20000, 16);
    if (tracking) CHECK(all_bytes(GP(DMA_A + 0x8000), 0x2000, 0x5a));
    CHECK(all_blue(GP(DMA_A), 0x8000));
    CHECK(all_blue(GP(DMA_A + 0xa000), 0x28000 - 0xa000));
    /* drawn over whole again: its next write-back is every pixel, the program's bytes no longer newer */
    rt_clear(ch, DMA_GA, 160, 256, 4, 1);
    vk_rt_before_write(DMA_GA + 0x20000, 16);
    CHECK(all_blue(GP(DMA_A), 0x28000));
    /* A target loaded from memory and then changed by a copy, with no draw since, so no draw baseline: the pages the
       program wrote after the load keep its bytes too. The rest is the image, as it was loaded */
    memset(GP(DMA_A), 0x21, 0x28000);
    g_renderer->mem_may_have_changed();
    vk_lock();
    vk_rt_reload(DMA_GA);
    Surf* copied = vk_rt_find(DMA_GA);
    CHECK(copied);
    vk_surf_wrote(copied);
    vk_unlock();
    memset(GP(DMA_A + 0x10000), 0x5b, 0x1000);
    g_renderer->mem_may_have_changed();
    vk_rt_before_write(DMA_GA + 0x20000, 16);
    if (tracking) CHECK(all_bytes(GP(DMA_A + 0x10000), 0x1000, 0x5b));
    CHECK(all_bytes(GP(DMA_A), 0x10000, 0x21));
    CHECK(all_bytes(GP(DMA_A + 0x11000), 0x17000, 0x21));
    /* A target whose pages lie in two runs of guest memory, as a virtual pool maps them: the page of the second run
       that the program wrote keeps its bytes too */
    const u64 split = DMA_GA + 0xa00000;
    gmmu_map(split, DMA_B, 0x10000);
    gmmu_map(split + 0x10000, DMA_A + 0x40000, 0x10000);
    rt_clear(ch, split, 160, 128, 4, 1);
    memset(GP(DMA_A + 0x41000), 0x6c, 0x1000);
    g_renderer->mem_may_have_changed();
    vk_rt_before_write(split + 0x13000, 16);
    if (tracking) CHECK(all_bytes(GP(DMA_A + 0x41000), 0x1000, 0x6c));
    CHECK(all_blue(GP(DMA_B), 0x10000));
    CHECK(all_blue(GP(DMA_A + 0x40000), 0x1000));
    CHECK(all_blue(GP(DMA_A + 0x42000), 0x2000));
    free(ch);
    return 0;
}

#include "vulkan_shader_invalidate.inc"
#include "vulkan_index_draws.inc"
#include "vulkan_storage_formats.inc"
#include "vulkan_storage_atomics.inc"
#include "vulkan_surface_loads.inc"
#include "vulkan_owned_readback.inc"
#include "vulkan_storage_suffix.inc"
#include "vulkan_mesh_inputs.inc"
#include "vulkan_vertex_restride.inc"
#include "vulkan_msaa.inc"
#include "vulkan_targetless_stores.inc"

int main(int argc, char** argv) {
    const u32 w = 32, h = 16;
    const u64 address = HEAP_BASE;
    const i32 full[4] = {0, 0, 0, 0}, crop[4] = {4, 2, 20, 10};
    char *root = snx_temp_dir(NULL), *shot, *ppm;
    CHECK(root);
    CHECK(!snx_startup_options(argc, argv));
    for (int i = 1; i < argc; i++) { /* --vk_validation=1 as the runtime takes it: the Khronos validation layer */
        if (!strcmp(argv[i], "--vk_validation=1")) g_settings.vulkan_validation_layer = 1;
        if (!strcmp(argv[i], "--vk_memory=2")) g_settings.guest_memory = 2; /* guest memory read in place */
    }
    snprintf(g_exedir, 1024, "%s/", root);
    shot = snx_path_join(root, "frame");
    ppm = snx_format("%s.ppm", shot);
    mem_init(); /* the real reservation and alias pool: the renderer traps what the GPU owns where the host can */
    CHECK(mem_map(HEAP_BASE, 0x800000, MS_NORMAL, PERM_RW) == 0);
    nv_init(); /* color targets may import guest memory through the GPU page table */
    g_settings.vulkan_video_decode = 1; /* a Vulkan Video decode queue where the device has one (lavapipe has none) */
    video_init("Headless Vulkan fixture", "");
    CHECK(g_opt_headless && g_renderer_up && g_vk.dev && !g_vk.surface && !g_vk.swapchain && !g_vk.sc_count);
    if (getenv("SNX_TEST_MSAA_ONLY")) { /* the multisample checks alone */
        CHECK(!check_multisample_shader_gate());
        CHECK(!check_msaa());
        CHECK(!check_msaa_nvn());
        printf("PASS headless Vulkan multisampling only: %u validation messages\n", g_vk_validation_messages);
        return 0;
    }
    if (getenv("SNX_TEST_VIC_ONLY")) { /* the validation test: VIC's bindings, none of the other checks' traffic */
        CHECK(!check_vic());
        CHECK(!check_vic_chain(shot));
        CHECK(!check_nvdec_vic());
        printf("PASS headless Vulkan VIC only: %u validation messages\n", g_vk_validation_messages);
        return 0;
    }
    CHECK(!check_hash_copy());
    CHECK(!check_staging_overflow());
    CHECK(!check_owned_readback());
    CHECK(!check_owned_writable());
    CHECK(!check_storage_suffix());
    CHECK(!check_buffer_texture_rows());
    CHECK(!check_storage_image_formats());
    CHECK(!check_volume_surface_atomics());
    CHECK(!check_layered_surface_loads());
    CHECK(!check_viewport_orientation());
    CHECK(!check_index_draws());
    CHECK(!check_mesh_inputs());
    CHECK(!check_vertex_restride());
    CHECK(!check_targetless_stores());
    CHECK(!check_packed_depth());
    CHECK(!check_depth_array());
    CHECK(!check_srgb_write());
    CHECK(!check_clear_load_op());
    CHECK(!check_sparse_while_submitting());
    CHECK(!check_multisample_shader_gate());
    CHECK(!check_msaa());
    CHECK(!check_msaa_nvn());
    CHECK(!check_direct_spirv());
    CHECK(!check_flush_subnormal());
    CHECK(!check_sampler_reduction());
    CHECK(!check_shader_invalidation());
    CHECK(!check_vic());
    CHECK(!check_vic_chain(shot));
    CHECK(!check_nvdec_vic());
    CHECK(!check_dma());
    CHECK(!check_blit());
    CHECK(!check_rt_overlap());
    CHECK(!check_uma());
    /* the pipeline workers: pipeline_threads when given, else a quarter of the CPUs, two to six */
    CHECK(vk_pipeline_worker_count(0, 8) == 6 && vk_pipeline_worker_count(0, 12) == 10 &&
          vk_pipeline_worker_count(0, 4) == 2 && vk_pipeline_worker_count(0, 0) == 2 &&
          vk_pipeline_worker_count(0, 64) == 12);
    CHECK(vk_pipeline_worker_count(5, 12) == 5 && vk_pipeline_worker_count(32, 4) == 32 &&
          vk_pipeline_worker_count(1, 64) == 1);
    /* FFmpeg takes the device for Vulkan Video exactly when it has the decode queue */
    AVBufferRef* video_device = g_renderer->nvdec_hw_device();
    CHECK(!video_device == (g_vk.video_family == ~0u));
    av_buffer_unref(&video_device);
    CHECK(g_renderer->method_barrier);
    u32 epoch = g_vk_epoch;
    u64 finishes = g_vk_finishes;
    for (int kind = 0; kind <= 4; kind++) g_renderer->method_barrier(kind);
    CHECK(g_vk_epoch == epoch);       /* no barrier expires cached reads: their checks see writes (vkr_method_barrier) */
    CHECK(g_vk_finishes == finishes); /* method barriers order GPU work without forcing a host wait */
    printf("Headless Vulkan device: %s\n", g_vk.props.deviceName);
    /* Make any accidental WSI use during presents/recreation fail the test immediately. */
    vkCreateSwapchainKHR = NULL;
    vkAcquireNextImageKHR = NULL;
    vkQueuePresentKHR = NULL;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR = NULL;
    CHECK(!vk_swapchain_create(0));
    u32* pixels = (u32*)(g_mem + address);
    for (u32 frame = 0; frame < 8; ++frame) {
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x) pixels[y * w + x] = 0xff000000u | x | (y << 8) | (frame << 16);
        const i32* region = frame & 1 ? crop : full;
        u32 x0 = frame & 1 ? 4 : 0, y0 = frame & 1 ? 2 : 0;
        u32 width = frame & 1 ? 16 : w, height = frame & 1 ? 8 : h;
        u32 transform = frame >> 1; /* none, horizontal, vertical, both; each with a full image and a crop */
        u64 expected = 1469598103934665603ull;
        for (u32 y = 0; y < height; ++y)
            for (u32 x = 0; x < width; ++x) {
                u32 sx = x0 + ((transform & 1) ? width - 1 - x : x);
                u32 sy = y0 + ((transform & 2) ? height - 1 - y : y);
                expected = (expected ^ pixels[sy * w + sx]) * 1099511628211ull;
            }
        u64 before = vk_frame_id();
        g_renderer->present_buffer(address, w, h, w * 4, 0, 0, 1, region, transform, 0, 0, 0);
        CHECK(vk_frame_id() == before + 1);
        CHECK(!g_vk.surface && !g_vk.swapchain);
        CHECK(g_renderer->screenshot(shot) == expected);
        size_t size;
        unsigned char* data = snx_read_file(ppm, &size);
        char header[64];
        int n = snprintf(header, sizeof header, "P6\n%u %u\n255\n", width, height);
        CHECK(data && size == (size_t)n + width * height * 3 && !memcmp(data, header, (size_t)n));
        for (u32 y = 0; y < height; ++y)
            for (u32 x = 0; x < width; ++x) {
                unsigned char* p = data + n + (y * width + x) * 3;
                CHECK(p[0] == x0 + ((transform & 1) ? width - 1 - x : x));
                CHECK(p[1] == y0 + ((transform & 2) ? height - 1 - y : y));
                CHECK(p[2] == frame);
            }
        free(data);
    }
    /* Even a present without an image submits and retires the current frame slot. */
    u64 before = vk_frame_id();
    g_renderer->present_buffer(0, 0, 0, 0, 0, 0, 1, full, 0, 0, 0, 0);
    CHECK(vk_frame_id() == before + 1);
    vk_finish();
    CHECK(!snx_remove_tree(root));
    free(root);
    free(shot);
    free(ppm);
    puts(
        "PASS headless Vulkan: viewport orientation, 7 VIC compositions, a VIC picture kept on the GPU and "
        "presented from there, NVDEC pictures laid out on the GPU and composed by VIC as the CPU references do, DMA copies and 2D blits on the GPU as theirs, "
        "render targets over one another newest on top, "
        "8 cropped/transformed readbacks and PPMs, no WSI");
    return 0;
}
