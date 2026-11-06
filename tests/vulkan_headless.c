/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Opt-in integration test: real renderer, no game data. Works with Mesa lavapipe and no display server. */
#include "common/utilities/common.h"
#include "nx/runtime/main.h"
#include "nx/runtime/cli.h"
#include "nx/runtime/ini.h"
#include "nx/runtime/options.h"
#include "nx/gpu/maxwell/draw_state.h"
#include "nx/gpu/renderer.h"
#include "common/gpu/vk/vk_common.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/descriptors.h"
#include "nx/gpu/shader/shader.h"
#include "nx/gpu/maxwell/kepler_compute_regs.h"
#include "nx/gpu/maxwell/engines_int.h"
#include "nx/gpu/maxwell/maxwell_3d_regs.h"
#include "nx/gpu/maxwell/gob.h"
#include "nx/gpu/texture/blocklinear.h"
#include "nx/gpu/nvn/nvn_copy.h"
#include "nx/gpu/nvn/nvn_draw.h"
#include "nx/gpu/nvn/nvn_tex.h"
#include "nx/gpu/nvn/nvn_tic.h"
#include "nx/gpu/nvn/nvn_replay.h"
#include "nx/gpu/nvn/nvn_state.h"
#include "nx/gpu/video/vic.h"
#include "nx/memory/mem.h"
#include "nx/memory/own.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/ini.h"
#include "snx_settings.h"
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

/* the GPU's results over guest memory [cpu, cpu + len) done before the test's CPU reads them, as the guest's fence
   would have them: with unified memory an engine's result is copied there in place (vk_uma_publish) by a submit that
   may not be done; without it the result is GPU-owned, and the read lands it by itself (as #690 has it) */
static void guest_done(u64 cpu, u64 len) {
    if (vk_uma()) vk_uma_mem_need_cpu(cpu, len);
}

/* The fatal path of a lost device runs on the recording thread: record.c's submit_run calls fatal(), whose window
   release asks the ring to drain (vk_rec_sync). A wait there waits for the thread that runs it, so the process spun at
   full speed instead of showing the error (Super Mario Bros. Wonder on MoltenVK). A call that the recording thread
   makes into the ring must return at once, and the packet after it must still run */
static void recorder_self_sync(void* arg) {
    vk_rec_sync();
    *(int*)arg = 1;
}
static int check_recorder_self_sync(void) {
    int ran = 0;
    if (!vk_rec_threaded()) {
        puts("SKIP the recording thread's own sync: vk_record=0 runs the commands on the GPU thread");
        return 0;
    }
    vk_rec_run(recorder_self_sync, &ran);
    CHECK(ran == 1);
    printf("The recording thread's own vk_rec_sync returns\n");
    return 0;
}

/* Graphics descriptor tables fill the frame's descriptor memory. vk_frame_check, which runs before a draw's setup,
   moves on to the next frame while the tables of one more draw still fit: the setup itself never submits for room */
static int check_descriptor_room(void) {
    if (!g_vk.has_descriptor_buffers) return 0;
    vk_lock();
    vk_finish();
    vk_frame_next();
    VkBuffer buffer;
    u64 offset;
    CHECK(vk_stage(256, vk_stage_align(), &buffer, &offset));
    VkDescriptorBufferInfo info = {buffer, offset, 16};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstBinding = 0,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                  .pBufferInfo = &info};
    u32 slot = vk_frame_slot();
    u64 submit = vk_submit_number(), tables = 0;
    while (vk_descriptors_room(slot)) { /* what vk_frame_check asks before each draw */
        for (int stage = 0; stage < 5; stage++) CHECK(vk_descriptors_write(g_vk_dslayout[0][0], 1, &write));
        tables += 5;
        CHECK(vk_frame_slot() == slot && vk_submit_number() == submit); /* no submit inside the "setup" */
    }
    CHECK(tables > 5 && !vk_descriptors_room(slot));
    vk_frame_check();
    CHECK(vk_frame_slot() != slot && vk_descriptors_room(vk_frame_slot()));
    vk_finish();
    vk_unlock();
    printf("Descriptor memory moves on to the next frame before a draw's setup, after %llu tables\n",
           (unsigned long long)tables);
    return 0;
}

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

/* The views a surface of each type can have (vk_tex_view_refusal), and the reason the log gives when a shader reads a
   texture through one it can't: a 3D texture bound to a 2D sampler, a render target or too few layers read as a cube.
   A refused view is never asked of the driver: vk_tex_view gives none for the 3D surface, which has no image */
static int check_tex_view_refusal(void) {
    Surf flat = {0};
    flat.type = VK_IMAGE_VIEW_TYPE_2D;
    flat.w = flat.h = 16;
    flat.layers = flat.levels = 1;
    Surf vol = flat, cube = flat, target = flat, wide = flat, line = flat;
    vol.type = VK_IMAGE_VIEW_TYPE_3D;
    vol.depth3d = 16;
    cube.type = VK_IMAGE_VIEW_TYPE_CUBE;
    cube.layers = 6;
    target = cube;
    target.is_rt = 1;
    wide = cube;
    wide.w = 32;
    line.type = VK_IMAGE_VIEW_TYPE_1D;
    const VkImageViewType all[] = {VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_CUBE,
                                   VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, VK_IMAGE_VIEW_TYPE_3D};
    const char* const why[][5] = {
        {NULL, NULL, "a cube needs 6 layers", "a cube needs 6 layers", "a 2D image has no 3D view"},
        {"a 3D image has no 2D view", "a 3D image has no 2D view", "a 3D image has no cube view",
         "a 3D image has no cube view", NULL},
        {NULL, NULL, NULL, g_vk.feat.imageCubeArray ? NULL : "the device has no cube arrays",
         "a 2D image has no 3D view"},
        {NULL, NULL, "a render target isn't made cube compatible", "a render target isn't made cube compatible",
         "a 2D image has no 3D view"},
        {NULL, NULL, "a cube's faces are square", "a cube's faces are square", "a 2D image has no 3D view"}};
    const Surf* surf[] = {&flat, &vol, &cube, &target, &wide};
    for (u32 s = 0; s < 5; s++)
        for (u32 v = 0; v < 5; v++) {
            const char* got = vk_tex_view_refusal(surf[s], all[v]);
            if (!why[s][v] ? got != NULL : !got || strcmp(got, why[s][v])) {
                fprintf(stderr, "vulkan_headless: surface %u read as %s: \"%s\", wanted \"%s\"\n", s,
                        vk_view_type_name(all[v]), got ? got : "(a view)", why[s][v] ? why[s][v] : "(a view)");
                return 1;
            }
        }
    CHECK(!strcmp(vk_tex_view_refusal(&line, VK_IMAGE_VIEW_TYPE_1D), "textures take no view of that type"));
    CHECK(!strcmp(vk_view_type_name(VK_IMAGE_VIEW_TYPE_CUBE_ARRAY), "cube array") &&
          !strcmp(vk_view_type_name(VK_IMAGE_VIEW_TYPE_3D), "3D") &&
          !strcmp(vk_view_type_name((VkImageViewType)99), "unknown"));
    const VkComponentMapping identity = {0};
    CHECK(vk_tex_view(&vol, VK_IMAGE_VIEW_TYPE_2D, 0, &identity, 0) == VK_NULL_HANDLE);
    printf("Texture views: 3D read as 2D, a target or one layer read as a cube are refused, and why\n");
    return 0;
}

/* A long buffer texture keeps consecutive texels across an image-row boundary,
   including a final row that is only partly used. Read actual uploaded pixels. */
static int check_buffer_texture_rows(void) {
    const u64 gva = 0x6000000, cpu = HEAP_BASE + 0x600000;
    const u32 row = 1u << SH_TEXEL_ROW_SHIFT, count = row + 3;
    const u32 samples[] = {row - 1, row, count - 1};
    u32 tic[8] = {0x08 | (2u << 7) | (2u << 10) | (2u << 13) | (2u << 16),
                  (u32)gva,
                  0,
                  (count - 1) >> 16,
                  ((count - 1) & 0xffff) | (6u << 23),
                  0,
                  0,
                  0};
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
    vk_surf_use(vk_cmd(), texture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
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
   and the import of the old ones goes (mem.c's hook): the GPU reads the new ones. In memory of its own: a page another
   check traps in the window it shares has the driver refuse that window, which is then copied for a while */
static int check_uma(void) {
    const u64 gva = 0x7000000, cpu = HEAP_BASE + 0x3000000, span = 0x10000;
    VkBuffer b, b2, rb;
    u64 off, off2;
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    CHECK(mem_map(cpu, span, MS_NORMAL, PERM_RW) == 0);
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
    CHECK(vk_buf_get_input(gva + 0x102, 8, 4, &b2, &off2) && b2 != b);                  /* off its alignment: copied */
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
    CHECK(mem_unmap(cpu, span) == 0);
    return 0;
}

/* Overflow blocks remain live through submission, then return with their slot. */
static int check_staging_overflow(void) {
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
                for (u32 before = 0; before < block; before++) CHECK(seen[slot][before] != buffer);
                seen[slot][block] = buffer;
            } else {
                CHECK(buffer == seen[slot][0] || buffer == seen[slot][1] || buffer == seen[slot][2]);
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
        for (u32 i = 0; i < 48; i++) CHECK(result[i] == frame * 3 + i / 16 + 1);
        for (u32 i = 48; i < 64; i++) CHECK(result[i] == 0xa0 + frame);
        for (u32 i = 64; i < 80; i++) CHECK(result[i] == 0xb0 + frame);
        vk_frame_next();
    }
    g_vk.alloc_cap = saved_cap;
    return 0;
}

/* A load that draws without a present: each draw uploads an allocation's worth, and vk_frame_check, before each draw,
   ends the frame once it has written VK_FRAME_RING_CAPS allocations to its rings. The next use of a slot waits for its
   GPU work and takes its rings again, so the rings the slots hold stay bounded, and every copy still reads its upload */
static int check_rings_without_present(void) {
    const u32 draws = 64;
    u64 saved_cap = g_vk.alloc_cap, base, most, first;
    VkBuffer rb;
    u8* result;
    vk_finish();
    vk_frame_next();
    g_vk.alloc_cap = 1u << 20;
    base = most = vk_ring_bytes();
    first = vk_frame_id();
    result = vk_readback_buffer(draws * 16, &rb);
    CHECK(result);
    for (u32 i = 0; i < draws; i++) {
        VkBuffer buffer;
        u64 offset;
        vk_frame_check(); /* a draw's start */
        u8* data = vk_stage_upload(g_vk.alloc_cap, 16, &buffer, &offset);
        CHECK(data);
        memset(data, (int)(i + 1), 16);
        VkBufferCopy copy = {offset, i * 16ull, 16};
        rec_CmdCopyBuffer(vk_cmd(), buffer, rb, 1, &copy);
        if (vk_ring_bytes() > most) most = vk_ring_bytes();
    }
    vk_finish();
    vk_readback_ready();
    for (u32 i = 0; i < draws * 16; i++) CHECK(result[i] == i / 16 + 1);
    u64 frames = vk_frame_id() - first, grown = most - base;
    g_vk.alloc_cap = saved_cap;
    vk_frame_next();
    printf("%u draws without a present: %llu frames, the slots' rings grew by %llu KiB at most\n", draws,
           (unsigned long long)frames, (unsigned long long)(grown >> 10));
    CHECK(frames >= draws / (VK_FRAME_RING_CAPS + 1));
    CHECK(grown <= (u64)VK_FRAMES * VK_FRAME_RING_CAPS * (1u << 20));
    return 0;
}

/* A frame that ended at its limit of submissions leaves its slot's counts as they were until the slot is used again:
   vk_frame_check on that slot, fresh after a present, keeps it rather than ending a frame that recorded nothing and
   moving on to the slot whose work was just submitted */
static int check_fresh_frame_kept(void) {
    u64 id;
    vk_finish();
    vk_frame_next();
    id = vk_frame_id();
    for (int i = 0; i < 128; i++) {
        vk_cmd();
        vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    }
    vk_frame_check(); /* 128 submissions: the frame ends */
    CHECK(vk_frame_id() == id + 1);
    vk_cmd();        /* the other slot records */
    vk_frame_next(); /* and presents: the first slot is current again, fresh */
    id = vk_frame_id();
    vk_frame_check();
    CHECK(vk_frame_id() == id);
    vk_finish();
    vk_frame_next();
    return 0;
}

/* n frames that each copy size bytes from one device buffer to another (size 0: none), then the slots used again, so
   each frame's timestamps are read (gpuprof.c, at frame_recycle) */
static void stats_frames(u32 n, VkBuffer from, VkBuffer to, u64 size) {
    VkBufferCopy copy = {0, 0, size};
    for (u32 i = 0; i < n; i++) {
        VkCommandBuffer cb = vk_cmd();
        if (size) rec_CmdCopyBuffer(cb, from, to, 1, &copy);
        vk_frame_next();
    }
    for (u32 i = 0; i < VK_FRAMES; i++) {
        vk_cmd();
        vk_frame_next();
    }
}
/* The stats box's GPU line (gpuprof.c, settings.txt's stats). Off, with the profile off too, frames write no
   timestamp and add no busy time. On, each command buffer is timed, and the graphics queue's busy time over frames
   that copy 32 MiB is above 0 and below their time on the clock. Stats turned off within a frame and on again: the slot
   reused while off has its pool reset at its next use, so a frame of empty command buffers doesn't count the copy
   timed before; and a command buffer begun while off isn't closed onto another's begin, so the wait between them
   isn't counted either. The device-local heap's use and budget are the driver's where it has VK_EXT_memory_budget */
static int check_stats_gpu_time(void) {
    const u64 size = 32ull << 20;
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = size,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VmaAllocationInfo info;
    VkBuffer buf[2];
    VmaAllocation mem[2];
    u64 stamps, busy, copied, t0, wall, vram, budget;
    int was = g_settings.stats;
    for (int i = 0; i < 2; i++) CHECK(vk_buffer_create(g_vk.vma, &bci, &aci, &buf[i], &mem[i], &info) == VK_SUCCESS);
    g_settings.stats = 0;
    vk_finish();
    vk_frame_next();
    stats_frames(0, buf[0], buf[1], 0); /* what an earlier check timed is read */
    stamps = g_vk_timestamps;
    busy = vk_gpuprof_busy_ns();
    stats_frames(8, buf[0], buf[1], size);
    printf("stats off: %llu timestamps written, %llu ns of GPU time added over 8 frames\n",
           (unsigned long long)(g_vk_timestamps - stamps), (unsigned long long)(vk_gpuprof_busy_ns() - busy));
    CHECK(g_vk_timestamps == stamps && vk_gpuprof_busy_ns() == busy);
    if (!vk_gpuprof_timed()) {
        printf("stats: the graphics queue writes no timestamps (timestampValidBits %u): its line says gpu ?\n",
               g_vk.gfx_timestamp_bits);
        goto done;
    }
    g_settings.stats = 1;
    t0 = plat_ns();
    busy = vk_gpuprof_busy_ns();
    stats_frames(8, buf[0], buf[1], size);
    wall = plat_ns() - t0;
    copied = vk_gpuprof_busy_ns() - busy;
    printf("stats on: 8 frames copying 32 MiB, the graphics queue busy %.3f ms of their %.3f ms, %llu timestamps\n",
           (double)copied / 1e6, (double)wall / 1e6, (unsigned long long)(g_vk_timestamps - stamps));
    CHECK(g_vk_timestamps > stamps && copied > 0 && copied < wall);
    /* a frame's copy timed, then stats off before its slot is used again, and on */
    rec_CmdCopyBuffer(vk_cmd(), buf[0], buf[1], 1, &(VkBufferCopy){0, 0, size});
    vk_frame_next();
    g_settings.stats = 0;
    stats_frames(0, buf[0], buf[1], 0); /* both slots used again, untimed */
    g_settings.stats = 1;
    busy = vk_gpuprof_busy_ns();
    stats_frames(2, buf[0], buf[1], 0);
    busy = vk_gpuprof_busy_ns() - busy;
    printf("stats on again: 2 empty frames busy %.3f ms, against %.3f ms a frame copying\n", (double)busy / 1e6,
           (double)copied / 8e6);
    CHECK(busy * 4 < copied / 8);
    /* within one frame: a command buffer begun and timed, stats off at its end, the next begun while off, a wait,
       stats on at its end */
    busy = vk_gpuprof_busy_ns();
    vk_cmd();
    g_settings.stats = 0;
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    vk_cmd();
    plat_sleep_ns(50000000);
    g_settings.stats = 1;
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    vk_frame_next();
    stats_frames(0, buf[0], buf[1], 0);
    busy = vk_gpuprof_busy_ns() - busy;
    printf("stats off and on within a frame, 50 ms between its command buffers: busy %.3f ms\n", (double)busy / 1e6);
    CHECK(busy < 25000000);
    vk_mem_driver_use(&vram, &budget);
    printf("stats: video memory %llu of %llu MiB (VK_EXT_memory_budget %s)\n", (unsigned long long)(vram >> 20),
           (unsigned long long)(budget >> 20), g_vk.has_budget ? "on" : "off");
    CHECK(g_vk.has_budget ? vram && budget : !vram && !budget);
done:
    g_settings.stats = was;
    vk_finish();
    for (int i = 0; i < 2; i++) vmaDestroyBuffer(g_vk.vma, buf[i], mem[i]);
    return 0;
}

/* a frame that uploads little: 64 bytes through vk_stage and 64 through vk_stage_upload, 16 of each copied into rb */
static int staging_light_frame(VkBuffer rb) {
    for (int upload = 0; upload < 2; upload++) {
        VkBuffer buffer;
        u64 offset;
        u8* data = upload ? vk_stage_upload(64, 16, &buffer, &offset) : vk_stage(64, 16, &buffer, &offset);
        CHECK(data);
        memset(data, 0x11, 64);
        VkBufferCopy copy = {offset, 16ull * (u32)upload, 16};
        rec_CmdCopyBuffer(vk_cmd(), buffer, rb, 1, &copy);
    }
    vk_frame_next();
    return 0;
}

/* A frame's uploads bigger than a staging chunk: a texture's, in parts of a chunk (vk_stage_chunk), one request of two
   chunks and a half, and buffer snapshots of a chunk each. The rings take chunks of the pool, and a big buffer for the
   big request: they grow by little more than the frame asked for, where a ring that doubled made each size on the
   way, and once frames that upload little have taken their place the rings hold what they held before the burst,
   where a doubled ring kept the allocation limit for 300 frames. Every copy reads its bytes */
static int check_staging_burst(void) {
    const u32 parts = 12, snapshots = 4, marks = parts + 1 + snapshots;
    u64 saved_cap = g_vk.alloc_cap, chunk, before, grown, held, asked = 0;
    VkBuffer rb;
    u8* result;
    vk_finish();
    vk_frame_next();
    g_vk.alloc_cap = 64ull << 20; /* a ring that doubled stopped here */
    chunk = vk_stage_chunk();
    result = vk_readback_buffer(marks * 16ull, &rb);
    CHECK(result && chunk < g_vk.alloc_cap);
    for (u32 i = 0; i < VK_POOL_FRAMES + VK_FRAMES; i++) CHECK(!staging_light_frame(rb));
    vk_finish(); /* the light frames' copies into rb done before the burst's, and the slot taken again */
    before = vk_ring_bytes();
    for (u32 i = 0; i < marks; i++) {
        VkBuffer buffer;
        u64 offset, size = i == parts ? chunk * 5 / 2 : chunk;
        u8* data = i <= parts ? vk_stage(size, vk_stage_align(), &buffer, &offset)
                              : vk_stage_upload(size, vk_stage_align(), &buffer, &offset);
        CHECK(data);
        memset(data + size - 16, (int)(i + 1), 16);
        VkBufferCopy copy = {offset + size - 16, i * 16ull, 16};
        rec_CmdCopyBuffer(vk_cmd(), buffer, rb, 1, &copy);
        asked += size;
    }
    grown = vk_ring_bytes() - before;
    vk_finish();
    vk_readback_ready();
    for (u32 i = 0; i < marks * 16; i++) CHECK(result[i] == i / 16 + 1);
    vk_frame_next();
    for (u32 i = 0; i < VK_POOL_FRAMES + VK_FRAMES; i++) CHECK(!staging_light_frame(rb));
    vk_finish();
    held = vk_ring_bytes();
    g_vk.alloc_cap = saved_cap;
    vk_frame_next();
    printf("a frame's %llu MiB of uploads in requests of up to %llu MiB: the rings grew by %llu MiB, and held %llu MiB "
           "%u frames later, %llu before\n",
           (unsigned long long)(asked >> 20), (unsigned long long)(chunk * 5 / 2 >> 20),
           (unsigned long long)(grown >> 20), (unsigned long long)(held >> 20), VK_POOL_FRAMES + VK_FRAMES,
           (unsigned long long)(before >> 20));
    CHECK(grown <= asked + chunk); /* and the big request's buffer, of its size class */
    CHECK(held <= before);
    return 0;
}

/* A chunk goes back to the pool only once its slot's work is done. A frame fills a chunk and copies from it in a
   submission that waits for a semaphore the host signals later; the next frame takes chunks of the pool and fills them
   with other bytes before the host signals: the copy still reads the first frame's bytes */
static int check_chunk_after_done(void) {
    VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
    VkSemaphoreSignalInfo signal = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .value = 1};
    u64 saved_cap = g_vk.alloc_cap, chunk, offset;
    VkBuffer rb, buffer;
    VkBufferCopy copy;
    u8 *result, *data;
    int taken = 1;
    CHECK(vkCreateSemaphore(g_vk.dev, &info, NULL, &signal.semaphore) == VK_SUCCESS);
    vk_finish();
    vk_frame_next();
    g_vk.alloc_cap = 1u << 20;
    chunk = vk_stage_chunk();
    result = vk_readback_buffer(32, &rb);
    CHECK(result);
    for (u32 i = 0; i < VK_POOL_FRAMES + VK_FRAMES; i++) CHECK(!staging_light_frame(rb));
    vk_finish(); /* the light frames' copies into rb done before these */
    data = vk_stage(chunk, vk_stage_align(), &buffer, &offset);
    CHECK(data);
    memset(data, 0x5a, (size_t)chunk);
    copy = (VkBufferCopy){offset + chunk - 16, 0, 16};
    rec_CmdCopyBuffer(vk_cmd(), buffer, rb, 1, &copy);
    vk_submit_after(signal.semaphore, 1, 2);
    vk_frame_next(); /* submitted, its copy waiting for the host */
    for (u32 i = 0; i < 4 && taken; i++) {
        taken = (data = vk_stage(chunk, vk_stage_align(), &buffer, &offset)) != NULL;
        if (taken) memset(data, 0xa5, (size_t)chunk);
    }
    if (taken) {
        copy = (VkBufferCopy){offset + chunk - 16, 16, 16};
        rec_CmdCopyBuffer(vk_cmd(), buffer, rb, 1, &copy);
    }
    CHECK(vkSignalSemaphore(g_vk.dev, &signal) == VK_SUCCESS); /* before any check that ends the test */
    vk_finish();
    vk_readback_ready();
    g_vk.alloc_cap = saved_cap;
    vk_frame_next();
    vkDestroySemaphore(g_vk.dev, signal.semaphore, NULL);
    CHECK(taken);
    for (u32 i = 0; i < 32; i++) CHECK(result[i] == (i < 16 ? 0x5a : 0xa5));
    return 0;
}

/* Small requests are handed out of the chunk a ring holds, with no allocation and no lock: frames of many small
   uploads, a chunk and a half of them each, make no ring buffer once the pool holds the chunks they take, and a copy of
   each frame's last one reads its bytes. What a request takes is printed */
static int check_small_uploads(void) {
    const u32 warm = 2 * VK_FRAMES, frames = 16, requests = 6 * 1024, size = 4096;
    u64 made = 0, ns = 0;
    VkBuffer rb;
    u8* result;
    vk_finish();
    vk_frame_next();
    result = vk_readback_buffer(frames * 16ull, &rb);
    CHECK(result && (u64)requests * size > vk_stage_chunk());
    for (u32 frame = 0; frame < warm + frames; frame++) {
        VkBuffer buffer = VK_NULL_HANDLE;
        u64 offset = 0, t0;
        u8* data = NULL;
        if (frame == warm) made = vk_ring_made();
        vk_cmd(); /* the slot's previous use waited for first */
        t0 = plat_ns();
        for (u32 i = 0; i < requests; i++)
            if (!(data = vk_stage(size, 16, &buffer, &offset))) break;
        if (frame >= warm) ns += plat_ns() - t0;
        CHECK(data);
        if (frame >= warm) {
            memset(data, (int)(frame - warm + 1), 16);
            VkBufferCopy copy = {offset, (frame - warm) * 16ull, 16};
            rec_CmdCopyBuffer(vk_cmd(), buffer, rb, 1, &copy);
        }
        vk_frame_next();
    }
    vk_finish();
    vk_readback_ready();
    for (u32 i = 0; i < frames * 16; i++) CHECK(result[i] == i / 16 + 1);
    printf("%u frames of %u uploads of %u bytes: %llu ring buffers made, %.1f ns a request\n", frames, requests, size,
           (unsigned long long)(vk_ring_made() - made), (double)ns / ((double)frames * requests));
    CHECK(vk_ring_made() == made);
    vk_frame_next();
    return 0;
}

/* A request bigger than a chunk every frame, as a copy engine's source of the same big surface makes, in the staging
   and the device ring: each takes the big buffer of its size class back from the pool, so once both slots have theirs
   no buffer is made, and the copies through them read their bytes; frames without one then let the pool give them up */
static int check_big_uploads(void) {
    const u32 warm = 2 * VK_FRAMES, frames = 8;
    u64 saved_cap = g_vk.alloc_cap, chunk, size, made = 0, big, before, after;
    VkMemoryBarrier2 written = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
    VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &written};
    VkBuffer rb;
    u8* result;
    vk_finish();
    vk_frame_next();
    g_vk.alloc_cap = 64ull << 20;
    chunk = vk_stage_chunk();
    size = chunk * 5 / 2;
    result = vk_readback_buffer(frames * 32ull, &rb);
    CHECK(result && size <= g_vk.alloc_cap);
    for (u32 i = 0; i < VK_POOL_FRAMES + VK_FRAMES; i++) CHECK(!staging_light_frame(rb));
    vk_finish();
    before = vk_ring_bytes();
    for (u32 frame = 0; frame < warm + frames; frame++) {
        VkBuffer host, device;
        u64 at, dat;
        u8* data;
        if (frame == warm) made = vk_ring_made();
        data = vk_stage(size, vk_stage_align(), &host, &at);
        CHECK(data && vk_stage_device(size, vk_stage_align(), &device, &dat));
        if (frame >= warm) {
            u64 n = frame - warm;
            VkBufferCopy in = {at + size - 16, dat + size - 16, 16}, out = {dat + size - 16, n * 32, 16},
                         staged = {at + size - 16, n * 32 + 16, 16};
            memset(data + size - 16, (int)(n + 1), 16);
            rec_CmdCopyBuffer(vk_cmd(), host, device, 1, &in);
            rec_CmdPipelineBarrier2(vk_cmd(), &dependency);
            rec_CmdCopyBuffer(vk_cmd(), device, rb, 1, &out);
            rec_CmdCopyBuffer(vk_cmd(), host, rb, 1, &staged);
        }
        vk_frame_next();
    }
    vk_finish();
    vk_readback_ready();
    for (u32 i = 0; i < frames * 32; i++) CHECK(result[i] == i / 32 + 1);
    big = vk_ring_made() - made;
    for (u32 i = 0; i < VK_POOL_FRAMES + VK_FRAMES; i++) CHECK(!staging_light_frame(rb));
    vk_finish();
    after = vk_ring_bytes();
    g_vk.alloc_cap = saved_cap;
    vk_frame_next();
    printf("%u frames of two %llu MiB requests: %llu ring buffers made; the rings held %llu MiB %u frames later, %llu "
           "before\n",
           frames, (unsigned long long)(size >> 20), (unsigned long long)big, (unsigned long long)(after >> 20),
           VK_POOL_FRAMES + VK_FRAMES, (unsigned long long)(before >> 20));
    CHECK(big == 0);
    CHECK(after <= before);
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
    mx_draw_dynamic(r, &d);
    return vk_render_front_face(&d);
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
    /* The origin's facing flip holds over a negative viewport too, which mirrors the winding on the console as it
       does in Vulkan: here NegativeY's, then the guest's own scale. A game's meshes drawn with the flip and a
       negative y scale came out inside out, and its full-screen passes that cull back faces culled whole */
    transform[1] = 17;
    memcpy(&r[0x280], transform, sizeof transform);
    CHECK(viewport_of(r, 0).height == -34);
    CHECK(front_face_of(r) == VK_FRONT_FACE_COUNTER_CLOCKWISE);
    r[0x286] = 0x6420;
    transform[1] = -17;
    memcpy(&r[0x280], transform, sizeof transform);
    CHECK(viewport_of(r, 0).height == -34);
    CHECK(front_face_of(r) == VK_FRONT_FACE_COUNTER_CLOCKWISE);
    r[0x286] = 0x6430;
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
    /* The transform off: every viewport is the surface clip (0x3FD's x and width, 0x3FE's y and height), depth 0 to
       1, whatever the scale, offset, swizzle and depth registers hold; a lower-left window flips it about its bottom */
    r[0x64B] = 0;
    r[0x3FD] = 64u << 16;
    r[0x3FE] = 48u << 16;
    r[0x286] = 0x6430;
    for (u32 i = 0; i < 2; i++) {
        vp = viewport_of(r, i);
        CHECK(vp.x == 0 && vp.y == 0 && vp.width == 64 && vp.height == 48 && vp.minDepth == 0 && vp.maxDepth == 1);
    }
    r[0x4eb] = 1;
    vp = viewport_of(r, 0);
    CHECK(vp.x == 0 && vp.y == 48 && vp.width == 64 && vp.height == -48);
    /* a clip wider than the device's viewports gives the widest the device takes (a game's driver leaves it 16384
       wide; 65535 here, past any device's limit) */
    r[0x4eb] = 0;
    r[0x3FD] = 0xFFFFu << 16;
    CHECK(viewport_of(r, 0).width == vk_scf((float)(u32)(g_vk.props.limits.maxViewportDimensions[0] / g_vk_scale)));
    CHECK(viewport_of(r, 0).width <= (float)g_vk.props.limits.maxViewportDimensions[0]);
    /* The transform on, a scale under half a pixel is still the transform's, never the viewport's clip rectangle: no
       area, so nothing is drawn */
    r[0x64B] = 1;
    r[0x4eb] = 0;
    r[0x286] = 0x6420;
    const float none[6] = {0, 0, 0.5f, 32, 24, 0.5f};
    memcpy(&r[0x280], none, sizeof none);
    r[0x300] = 64u << 16;
    r[0x301] = 48u << 16;
    CHECK(viewport_of(r, 0).width == 0);
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

/* astc_mode: Auto (0, the default) gives an ASTC texture a BC7 image where the GPU samples BCn but not ASTC, Exact (1)
   an RGBA8 one; where the GPU samples ASTC, both keep the game's blocks */
static int check_astc_mode(void) {
    for (int mode = 0; mode < 2; mode++) {
        g_settings.astc_mode = mode;
        const VkFormat want = g_vk.feat.textureCompressionASTC_LDR      ? VK_FORMAT_ASTC_4x4_UNORM_BLOCK
                              : g_vk.feat.textureCompressionBC && !mode ? VK_FORMAT_BC7_UNORM_BLOCK
                                                                        : VK_FORMAT_R8G8B8A8_UNORM;
        CHECK(vk_tic_format(0x40, 0).vk == want);
    }
    g_settings.astc_mode = 0;
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
/* under vk_lock, as the renderer's clear (vkr_clear) is: a landing of GPU-owned memory meanwhile records in the frame's
   command buffer too */
static void engine_clear(GpuChannel* ch, u32 v) {
    DrawClear c;
    ch->r3d_dirty = ~0u;
    mx_draw_state_sync(ch);
    mx_draw_clear(ch->r3d, v, &c);
    vk_lock();
    vk_render_clear(&ch->ds, &c, &ch->owner);
    vk_unlock();
}
/* Packed depth and stencil targets written back in the guest's layout (vk/depth_pack.c): each zeta format cleared to
   depth 0.25 and stencil 0x5A, written back, and every pixel read from guest memory as its format lays one. They used
   to stay on the GPU, so a copy of one through memory read zeros. 0.25 is 0x400000 in 24 bits with no tie to round */
static int check_depth_writeback(void) {
    static const struct {
        u32 zeta, bpp, lo, hi;
    } c[] = {
        {0x19, 8, 0x3E800000u, 0x5Au}, /* Z32_FLOAT_X24S8: the float, then the stencil byte */
        {0x14, 4, 0x4000005Au, 0},     /* Z24_UNORM_S8: the stencil in the low byte */
        {0x16, 4, 0x5A400000u, 0},     /* S8Z24: the stencil in the high byte */
        {0x15, 4, 0x00400000u, 0},     /* X8Z24 */
    };
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    u32* r = ch->r3d;
    float depth = 0.25f;
    const u64 base = HEAP_BASE + 0xA00000; /* past the heap and the blit case's region */
    CHECK(mem_map(base, 0x40000, MS_NORMAL, PERM_RW) == 0);
    for (u32 k = 0; k < sizeof c / sizeof *c; k++) {
        u64 gva = 0x9000000ull + (u64)k * 0x10000, cpu = base + (u64)k * 0x10000;
        u8 out[16 * 16 * 8];
        gmmu_map(gva, cpu, 0x10000);
        memset(GP(cpu), 0, 0x10000);
        r[0x54E] = 1;
        r[0x3F8] = 0;
        r[0x3F9] = (u32)gva;
        r[0x3FA] = c[k].zeta;
        r[0x3FB] = 0x1000; /* pitch linear */
        r[0x3FC] = 512;
        r[0x48A] = r[0x48B] = 16;
        r[0x48C] = 1;
        memcpy(&r[0x364], &depth, 4);
        r[0x368] = 0x5A;
        engine_clear(ch, 3);
        vk_render_end();
        CHECK(vk_rt_find(gva));
        vk_rt_flush(gva);
        CHECK(!gmem_read(gva, out, 16 * 16 * c[k].bpp));
        for (u32 i = 0; i < 16 * 16; i++) {
            u32 lo, hi = 0;
            memcpy(&lo, out + i * c[k].bpp, 4);
            if (c[k].bpp == 8) memcpy(&hi, out + i * 8 + 4, 4);
            if (lo != c[k].lo || hi != c[k].hi) {
                fprintf(stderr, "depth write-back of zeta %#x, pixel %u: %08x %08x, not %08x %08x\n", c[k].zeta, i, lo,
                        hi, c[k].lo, c[k].hi);
                return 1;
            }
        }
    }
    CHECK(mem_unmap(base, 0x40000) == 0);
    free(ch);
    return 0;
}
/* A volume of more than 512 Mi texels whose bytes the GPU holds is a texture (a game's light volume, 1664x448x1344 of
   BC1 in a pool mapped in pieces, read as zeros): BC1 1024x1024x513, 269 MB, mapped only at its start, comes with the
   block memory has there */
static int check_big_volume(void) {
    const u64 gva = 0x200000000ull, cpu = HEAP_BASE + 0xB00000;
    const u8 block[8] = {0x1F, 0x00, 0xE0, 0x07, 0x1B, 0x6C, 0xB1, 0xC6};
    /* BC1, 3D, block linear with blocks 16 GOBs deep, as the game's header has it */
    const u32 tic[8] = {
        0x58D24924u, (u32)gva, (u32)(gva >> 32) | 3u << 21, 4u << 6, 1023u | 2u << 23, 1023u | 512u << 16, 0, 0};
    u8 got[8] = {0};
    if (vk_tic_format_3d(tic[0], 0).vk != VK_FORMAT_BC1_RGBA_UNORM_BLOCK) {
        puts("big volume: the GPU has no 3D BC1 images (it would decode 2 GB), skipped");
        return 0;
    }
    CHECK(mem_map(cpu, 0x10000, MS_NORMAL, PERM_RW) == 0);
    memset(GP(cpu), 0, 0x10000);
    memcpy(GP(cpu), block, sizeof block);
    gmmu_map(gva, cpu, 0x10000);
    vk_lock();
    Surf* t = vk_tex_from_tic(tic);
    VkBuffer buffer;
    u8* result = t ? vk_readback_buffer(sizeof got, &buffer) : NULL;
    if (result) {
        VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {4, 4, 1}};
        vk_surf_use(vk_cmd(), t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        rec_CmdCopyImageToBuffer(vk_cmd(), t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
        vk_finish();
        vk_readback_ready();
        memcpy(got, result, sizeof got);
    }
    vk_unlock();
    if (!t || memcmp(got, block, sizeof got))
        fprintf(stderr, "big volume: texture %d, first block %02x%02x%02x%02x%02x%02x%02x%02x\n", t != NULL, got[0],
                got[1], got[2], got[3], got[4], got[5], got[6], got[7]);
    CHECK(t && t->type == VK_IMAGE_VIEW_TYPE_3D && t->w == 1024 && t->h == 1024 && t->depth3d == 513);
    CHECK(!memcmp(got, block, sizeof got));
    puts("big volume: a 1024x1024x513 BC1 texture is made, its first block from memory");
    return 0;
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

/* a single-layer target's pixel at (x, y) as its image holds it */
static int read_pixel_at(Surf* target, u32 x, u32 y, u8 out[4]) {
    VkBuffer buffer;
    u8* data = vk_readback_buffer(4, &buffer);
    CHECK(data);
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageOffset = {(i32)x, (i32)y, 0},
                              .imageExtent = {1, 1, 1}};
    rec_CmdCopyImageToBuffer(cb, target->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    vk_finish();
    vk_readback_ready();
    memcpy(out, data, 4);
    return 0;
}
/* a single-layer target's pixel 0 as its image holds it */
static int read_pixel0(Surf* target, u8 out[4]) { return read_pixel_at(target, 0, 0, out); }

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
    u32 *code = snx_test_spirv_compute(&words), value = 0;
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
    static const u32 in[] = {0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u, 0x007fffffu, 0x807fffffu, 0x00400000u,
                             0x80400000u, 0x00000123u, 0x80654321u, 0x00800000u, 0x80800000u, 0x00800001u, 0x3f800000u,
                             0xbf800000u, 0x7f7fffffu, 0xff7fffffu, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u,
                             0x7fa00001u, 0x7fc12345u, 0x0d000000u, 0x8c000000u, 0x34000000u, 0x00012345u, 0x80000100u,
                             0x01000000u, 0x81000000u, 0x3e800000u, 0x42f60000u};
    enum { N = sizeof in / sizeof *in };
    for (int signed_zeros = 0; signed_zeros < 2; signed_zeros++) {
        if (signed_zeros && !g_vk_preserve_infnan) continue;
        size_t words = 0;
        u32 *code = snx_test_spirv_flush(N, signed_zeros, &words), data[2 * N] = {0};
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

/* The float .SAT (spv_emit_saturate) on this device, against the console's: NaNs of either sign, quiet or signaling,
   give +0, and the rest is clamped to [0, 1] (a zero keeps either sign: FClamp may return the bound or the input). With
   NaNs kept by the module and without, as the translator declares them. What FClamp alone gives a NaN is printed: the
   device is free to give anything (issue #865) */
static int check_saturate(void) {
    static const struct {
        u32 in, want;
    } cases[] = {{0x7fc00000u, 0},
                 {0xffc00000u, 0},
                 {0x7f800001u, 0},
                 {0xff800001u, 0},
                 {0x7fffffffu, 0},
                 {0xffffffffu, 0},
                 {0x7fa00001u, 0},
                 {0x3f000000u, 0x3f000000u},
                 {0x3f800000u, 0x3f800000u},
                 {0x40000000u, 0x3f800000u},
                 {0x7f800000u, 0x3f800000u},
                 {0x7f7fffffu, 0x3f800000u},
                 {0xbf800000u, 0},
                 {0xff800000u, 0},
                 {0xff7fffffu, 0},
                 {0x3e800000u, 0x3e800000u},
                 {0x3f7fffffu, 0x3f7fffffu},
                 {0x00000000u, 0},
                 {0x80000000u, 0}};
    enum { N = sizeof cases / sizeof *cases };
    for (int keep = 0; keep < 2; keep++) {
        if (keep && !g_vk_preserve_infnan) continue;
        size_t words = 0;
        u32 *code = snx_test_spirv_saturate(N, keep, &words), data[3 * N] = {0};
        for (u32 k = 0; k < N; k++) data[k] = cases[k].in;
        int r = run_storage_compute(code, words, data, 3 * N);
        free(code);
        CHECK(!r);
        int nan_clamps = 0;
        for (u32 k = 0; k < N; k++) {
            u32 got = data[N + k], want = cases[k].want;
            if (got != want && !(want == 0 && got == 0x80000000u && (cases[k].in & 0x7fffffffu) == 0)) {
                printf("saturate (NaNs %s): %08x gave %08x, not %08x\n", keep ? "kept" : "not kept", cases[k].in, got,
                       want);
                return 1;
            }
            if ((cases[k].in & 0x7fffffffu) > 0x7f800000u && data[2 * N + k] != 0) nan_clamps++;
        }
        printf("saturate (NaNs %s): every input as the console's .SAT gives it; FClamp alone gave %d of 7 NaNs other "
               "than +0\n",
               keep ? "kept" : "not kept", nan_clamps);
    }
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
    u32 owned_cases = 0; /* published GPU-owned: no import held the output */
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
        vk_lock();
        int inplace = vk_uma_held(VIC_BASE + 0x2000, (u64)pitch * H);
        vk_unlock();
        owned_cases += !inplace;
        CHECK(g_renderer->vic_compose(&job));
        /* on the GPU until read; with unified memory, over an import, copied there before the syncpoint
           (vk_uma_publish) */
        CHECK(inplace ? own_lookup(VIC_BASE + 0x2000, (u64)pitch * H, NULL, 0) == 0
                      : !own_traps() || own_lookup(VIC_BASE + 0x2000, (u64)pitch * H, NULL, 0) > 0);
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
    /* each picture GPU-owned was read here: landed once, by the trap, where the host has traps */
    CHECK(!own_traps() || own_landings() >= lands + owned_cases);
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
    vk_lock();
    int inplace = vk_uma_held(VIC_BASE + 0x2000, (u64)W * 4 * H); /* the first job's output over an import */
    vk_unlock();
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
    CHECK(!own_traps() || own_landings() == lands + !inplace); /* in guest memory already, over an import */
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
    NvdecPic p = {
        {y, u, v}, {(int)w, (int)pairs, (int)pairs}, w, h, NV_GPU, NV_GPU + 0x10000, ext_y, ext_c, pitch, pitch, sh,
        bh};
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
    CHECK(!nvdec_vic_case(33, 17, 64, 24, 0));  /* rows ending inside a run, padding rows, blocks of one GOB */
    CHECK(!nvdec_vic_case(54, 40, 128, 48, 2)); /* a wider surface than the picture */
    /* each case lands its three ranges when read, nothing before */
    CHECK(!own_traps() || own_landings() <= lands + 9);
    return 0;
}

/* The DMA copy engine on the GPU (vk/dma.c) against its CPU reference (dma_ref.c), each on one of two twin regions of
   guest memory mapped into the GPU's address space: pitch and block linear sides, origins not on 16 bytes, lines
   past the surface, the remap's shuffles, constants and kept bytes */
#define DMA_A    (HEAP_BASE + 0x400000) /* the GPU's twin, at GPU address DMA_GA */
#define DMA_B    (HEAP_BASE + 0x600000) /* the reference's, at DMA_GB */
#define DMA_GA   0x200000000ull
#define DMA_GB   0x300000000ull
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
    guest_done(DMA_A, DMA_SPAN);
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
    guest_done(DMA_A, DMA_SPAN);
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
/* texel (x, y) of the RGBA8 texture t as its image holds it */
static u32 dma_texel(Surf* t, u32 x, u32 y) {
    VkBuffer buffer;
    u8* result = vk_readback_buffer(4, &buffer);
    if (!result) return 0;
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageOffset = {(i32)x, (i32)y, 0},
                              .imageExtent = {1, 1, 1}};
    vk_surf_use(vk_cmd(), t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    rec_CmdCopyImageToBuffer(vk_cmd(), t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    vk_finish();
    vk_readback_ready();
    return snx_ld_le32(result);
}
/* A DMA copy into a window of a block linear texture the cache holds (a texture upload, or a tile streamed into a big
   one): 16 lines of 8 RGBA8 texels at (16, 16) of a 64x64 one. The texture takes them in its image without loading
   all of it again, its other texels stay, and guest memory gets them as the CPU reference does */
static int check_dma_texture(void) {
    const u64 tex = 0x40000, ga = DMA_GA + tex;
    const u32 rgba8 = 0x08 | 2u << 7 | 2u << 10 | 2u << 13 | 2u << 16;
    const u32 tic[8] = {rgba8, (u32)ga, (u32)(ga >> 32) | 3u << 21, 0, 63u | 1u << 23, 63u, 0, 0};
    GpuDmaOp o = {.multi = 1, .width = 32, .line_in = 32, .line_out = 32, .lines = 16};
    u64 uploads, bytes, again;
    for (int k = 0; k < 2; k++) {
        u8* t = GP(k ? DMA_B : DMA_A);
        for (u32 i = 0; i < DMA_SPAN; i++) t[i] = (u8)(i * 37 + (i >> 9) * 3 + 11);
    }
    const u32 corner = snx_ld_le32(GP(DMA_A) + tex), copied = snx_ld_le32(GP(DMA_A) + 0x1000 + 5 * 64 + 3 * 4);
    vk_lock();
    Surf* t = vk_tex_from_tic(tic);
    vk_unlock();
    CHECK(t && t->upload_valid && t->bl && t->w == 64 && t->h == 64);
    vk_tex_stats(&uploads, &bytes);
    o.src = pitch_side(DMA_GA + 0x1000, 64);
    o.dst = bl_side(ga, 256, 64, 0, 64, 16);
    CHECK(g_renderer->dma_copy(&o));
    o.src.addr += DMA_GB - DMA_GA, o.dst.addr += DMA_GB - DMA_GA;
    dma_copy_ref(&o);
    vk_lock();
    Surf* same = vk_tex_from_tic(tic);
    vk_tex_stats(&again, &bytes);
    u32 inside = same ? dma_texel(same, 19, 21) : 0, outside = same ? dma_texel(same, 0, 0) : 0;
    vk_unlock();
    if (same != t || again != uploads || inside != copied || outside != corner)
        fprintf(stderr,
                "dma into a texture: the same texture %d, %llu loads again, texel (19, 21) %08x expected %08x, texel "
                "(0, 0) %08x expected %08x\n",
                same == t, (unsigned long long)(again - uploads), inside, copied, outside, corner);
    CHECK(same == t && again == uploads && inside == copied && outside == corner);
    CHECK(!memcmp(GP(DMA_A), GP(DMA_B), DMA_SPAN)); /* reading the GPU's lands it */
    puts("dma into a texture: its image takes the lines, nothing loads again, memory matches the reference");
    return 0;
}
/* A volume in a virtual pool, mapped a 64 KiB page at a time: 1664 KiB in blocks 1 GOB high and 16 deep, 13 GOBs a
   row, so a page runs on into the next row of blocks; BC1 416x256x32, and RGBA8 208x64x32, which every device samples
   in 3D. Pages 0 and 2 are mapped when it is made. After 60 frames of it unchanged (its checks settled), a page's bytes
   reach it each way they can: (a) page 1 mapped over memory that holds it already, (b) the CPU writing mapped page 1,
   (c) a DMA copy into page 2. After the frame that follows (a submission, a present), every texel or block of the
   image is the one memory has (zeros in the holes) */
#define SV_TILE  0x10000u
#define SV_BYTES (26 * SV_TILE) /* 832 bytes by 64 rows (of texels or blocks) in 32 slices, none of them padding */
typedef struct {
    const char* name;
    u32 tic0;     /* the header's format word */
    u32 eb, bw;   /* bytes of a texel or block, and its texels across and down */
    VkFormat vk;  /* what the device has to sample it as, or the check is skipped */
    u64 gva, cpu; /* the volume, and the tiles behind it then the DMA's source */
} SvCase;
/* the byte offset in the volume of byte xb of row y (of texels or blocks) of slice z */
static u64 sv_offset(u32 xb, u32 y, u32 z) { return bl_offset(xb, y, z, 832, 64, 0, 4); }
/* the texels or blocks of t's image that differ from memory's: how many, and the first of them in *first (x, row,
   slice) with its bytes in the image and in memory. -1 when the image couldn't be read */
static int sv_differ(const SvCase* c, Surf* t, u32 first[3], u64* image_has, u64* memory_has) {
    static u8 memory[SV_BYTES];
    const u32 across = 832 / c->eb;
    VkBuffer buffer;
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {across * c->bw, 64 * c->bw, 32}};
    vk_lock();
    u8* image = vk_readback_buffer(SV_BYTES, &buffer);
    if (image) {
        vk_surf_use(vk_cmd(), t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        rec_CmdCopyImageToBuffer(vk_cmd(), t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
        vk_finish();
        vk_readback_ready();
    }
    vk_unlock();
    if (!image) return -1;
    gmem_read(c->gva, memory, SV_BYTES); /* -1 for the holes, which read as zeros */
    int n = 0;
    for (u32 z = 0; z < 32; z++)
        for (u32 y = 0; y < 64; y++)
            for (u32 x = 0; x < across; x++) {
                const u8 *in = image + (((u64)z * 64 + y) * across + x) * c->eb,
                         *mem = memory + sv_offset(x * c->eb, y, z);
                if (!memcmp(in, mem, c->eb)) continue;
                if (!n++) {
                    first[0] = x, first[1] = y, first[2] = z;
                    *image_has = c->eb == 8 ? snx_ld_le64(in) : snx_ld_le32(in);
                    *memory_has = c->eb == 8 ? snx_ld_le64(mem) : snx_ld_le32(mem);
                }
            }
    return n;
}
/* what comes between two frames' draws of the volume: a submission (a new epoch for the caches' checks) and a present */
static Surf* sv_frame(const u32* tic) {
    g_renderer->mem_may_have_changed();
    g_vk_present++;
    vk_lock();
    Surf* t = vk_tex_from_tic(tic);
    vk_unlock();
    return t;
}
/* one way in: after it, the next frame's texture is the one made, all of it is memory's, and it loaded at most most
   bytes again (the blocks of the pages that changed, not the volume). 1 when not, said with the first texel or block
   that differs */
static int sv_way(const SvCase* c, const char* way, Surf* t, const u32* tic, u64 most) {
    u64 uploads, bytes, uploads2, bytes2, image_has = 0, memory_has = 0;
    u32 first[3] = {0};
    vk_tex_stats(&uploads, &bytes);
    Surf* now = sv_frame(tic);
    vk_tex_stats(&uploads2, &bytes2);
    int differ = now ? sv_differ(c, now, first, &image_has, &memory_has) : -1;
    if (now == t && differ == 0 && bytes2 - bytes <= most) {
        printf("sparse volume %s, %s: all of it as memory has it, %llu bytes loaded again\n", c->name, way,
               (unsigned long long)(bytes2 - bytes));
        return 0;
    }
    fprintf(stderr,
            "sparse volume %s, %s: the same texture %d, %llu bytes loaded again (%llu loads, at most %llu wanted); %d "
            "texels or blocks of the image differ from memory\n",
            c->name, way, now == t, (unsigned long long)(bytes2 - bytes), (unsigned long long)(uploads2 - uploads),
            (unsigned long long)most, differ);
    if (differ > 0)
        fprintf(stderr, "  the first, (%u, %u) of slice %u: image %016llx, memory %016llx\n", first[0], first[1],
                first[2], (unsigned long long)image_has, (unsigned long long)memory_has);
    return 1;
}
static int sparse_volume(const SvCase* c) {
    const u32 across = 832 / c->eb, w = across * c->bw - 1, h = 64 * c->bw - 1;
    const u32 tic[8] = {c->tic0, (u32)c->gva, (u32)(c->gva >> 32) | 3u << 21, 4u << 6, w | 2u << 23, h | 31u << 16, 0,
                        0};
    u32 first[3];
    u64 image_has, memory_has;
    if (vk_tic_format_3d(tic[0], 0).vk != c->vk) {
        printf("sparse volume %s: the GPU has no such 3D images, skipped\n", c->name);
        return 0;
    }
    /* tile 1 is blocks 8 to 15 of the first row of blocks: 8 to 12 of it, then 0 to 2 of the next one (rows 8 to 15) */
    CHECK(sv_offset(512, 0, 0) == SV_TILE && sv_offset(8, 9, 4) / SV_TILE == 1 && sv_offset(831, 7, 15) / SV_TILE == 1);
    CHECK(mem_map(c->cpu, 4 * SV_TILE, MS_NORMAL, PERM_RW) == 0);
    for (u32 i = 0; i < 4 * SV_TILE; i++) GP(c->cpu)[i] = (u8)(i * 29 + (i >> 16) * 71 + 3);
    memset(GP(c->cpu + 2 * SV_TILE), 0, SV_TILE); /* tile 2 holds zeros until the DMA */
    gmmu_map(c->gva, c->cpu, SV_TILE);
    gmmu_map(c->gva + 2 * SV_TILE, c->cpu + 2 * SV_TILE, SV_TILE);
    u64 uploads, bytes, uploads2, bytes2;
    vk_tex_stats(&uploads, &bytes);
    vk_lock();
    Surf* t = vk_tex_from_tic(tic);
    vk_unlock();
    vk_tex_stats(&uploads2, &bytes2);
    CHECK(t && t->type == VK_IMAGE_VIEW_TYPE_3D && t->w == w + 1 && t->h == h + 1 && t->depth3d == 32 &&
          t->size == SV_BYTES);
    /* what is mapped, read: the holes are cleared, not read */
    if (bytes2 - bytes != 2 * SV_TILE) {
        fprintf(stderr, "sparse volume %s: %llu bytes loaded to make it, its 2 mapped tiles are %u\n", c->name,
                (unsigned long long)(bytes2 - bytes), 2 * SV_TILE);
        return 1;
    }
    CHECK(sv_differ(c, t, first, &image_has, &memory_has) == 0);
    for (int i = 0; i < 60; i++) CHECK(sv_frame(tic) == t);
    /* (a) the game's MapVirtual: pages that hold the tile, mapped at its place in the pool: its 8 blocks of 8 KiB (a GOB
       across, 8 rows, 16 slices) */
    gmmu_map(c->gva + SV_TILE, c->cpu + SV_TILE, SV_TILE);
    int failed = sv_way(c, "a tile mapped", t, tic, SV_TILE);
    for (int i = 0; i < 60; i++) CHECK(sv_frame(tic) == t);
    /* (b) the CPU writes 8 bytes of the mapped tile */
    const u8 cpu_bytes[8] = {0x1F, 0x00, 0xE0, 0x07, 0x1B, 0x6C, 0xB1, 0xC6};
    memcpy(GP(c->cpu + sv_offset(576, 2, 3)), cpu_bytes, 8);
    failed |= sv_way(c, "the CPU writing a tile", t, tic, 2 * 8192); /* a page's block, or two of a 16 KiB page's */
    for (int i = 0; i < 60; i++) CHECK(sv_frame(tic) == t);
    /* (c) a DMA copy of 64 bytes into tile 2 (block 3 of the second row of blocks), from a page past the volume */
    GpuDmaOp o = {.width = 64, .line_in = 64, .line_out = 64, .lines = 1};
    const u64 src = c->gva + 0x200000;
    gmmu_map(src, c->cpu + 3 * SV_TILE, SV_TILE);
    CHECK(sv_offset(240, 10, 6) / SV_TILE == 2);
    o.src = pitch_side(src + 0x340, 0);
    o.dst = pitch_side(c->gva + sv_offset(240, 10, 6), 0);
    CHECK(g_renderer->dma_copy(&o));
    failed |= sv_way(c, "a DMA copy into a tile", t, tic, 8192); /* the page's block */
    return failed;
}
static int check_sparse_volume(void) {
    const SvCase cases[] = {
        {"BC1", 0x58D24924u, 8, 4, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 0x240000000ull, HEAP_BASE + 0xC00000},
        {"RGBA8", 0x08 | 2u << 7 | 2u << 10 | 2u << 13 | 2u << 16, 4, 1, VK_FORMAT_R8G8B8A8_UNORM, 0x250000000ull,
         HEAP_BASE + 0xC40000},
    };
    int failed = 0;
    for (u32 i = 0; i < sizeof cases / sizeof *cases; i++) failed |= sparse_volume(&cases[i]);
    CHECK(!failed);
    puts("sparse volume: a tile mapped, written by the CPU or by DMA shows in the next frame");
    return 0;
}
/* A sparse texture as a game makes one through NVN (nvn_tex_layout, nvn_tic_encode): 3 tiles across, 2 down and (a
   volume) 2 deep, three of them mapped at the offsets the game gives them, the tiles across, then down, then deep,
   each page holding one block's bytes over and over. Every block of the image in a mapped tile's brick is that block
   and every other one is zeros: each 64 KiB page is one tile. BC1 and BC6H volumes, as one game's light volumes, and
   RGBA8, which every device samples in 3D; then a BC1 and an RGBA8 volume and an RGBA8 2D texture that end half a
   tile short of their third tile across, whose rows are still whole tiles (the header's tile width). The 2D one has a
   second level, 160x128, a tile high and more than one wide: a view of it alone has rows of whole tiles too, its
   second tile mapped */
typedef struct {
    const char* name;
    int format;
    u32 eb, bw;   /* bytes of a texel or block, and its texels across and down */
    VkFormat vk;  /* what the device has to sample it as, or the check is skipped */
    u64 gva, cpu; /* the texture, and the pages of its mapped tiles */
    int is3d;     /* a volume, else a 2D texture */
    u32 cut;      /* blocks short of the third tile across */
    u32 levels;
} StCase;
static int sparse_tiles(const StCase* c) {
    static const u32 mapped3d[3] = {1, 5, 10}; /* tiles (1, 0, 0), (2, 1, 0) and (1, 1, 1) */
    static const u32 mapped2d[3] = {1, 3, 5};  /* tiles (1, 0), (0, 1) and (2, 1) */
    const u32* mapped = c->is3d ? mapped3d : mapped2d;
    NvnTexture tex = {0};
    NvnTexLayout lay;
    u32 tile[3] = {0}, tic[8], first[4] = {0}, bad = 0, tiles = c->is3d ? 12 : 6;
    VkBuffer buffer;
    tex.d.target = c->is3d ? NVN_TEXTURE_TARGET_3D : NVN_TEXTURE_TARGET_2D;
    tex.d.format = (u16)c->format;
    tex.d.levels = (u8)c->levels;
    tex.d.flags = NVN_TEXTURE_FLAGS_SPARSE;
    CHECK(nvn_tex_sparse_tile(&tex.d, tile));
    tex.d.width = (3 * tile[0] - c->cut) * c->bw, tex.d.height = 2 * tile[1] * c->bw;
    tex.d.depth = c->is3d ? 2 * tile[2] : 1;
    tex.gpu = c->gva;
    nvn_tex_layout(&tex.d, &lay);
    /* a 2D texture's second level: 2 tiles across (whole ones), one down, after the first's */
    CHECK(lay.size == (tiles + (c->levels > 1 ? 2 : 0)) * SV_TILE);
    CHECK(c->levels == 1 || nvn_tex_level_offset(&tex.d, 1) == tiles * SV_TILE);
    CHECK(nvn_tic_encode(&tex, NULL, 0, 0, tic));
    if ((c->is3d ? vk_tic_format_3d(tic[0], 0) : vk_tic_format(tic[0], 0)).vk != c->vk) {
        printf("sparse tiles %s: the GPU has no such %s images, skipped\n", c->name, c->is3d ? "3D" : "2D");
        return 0;
    }
    CHECK(mem_map(c->cpu, 4 * SV_TILE, MS_NORMAL, PERM_RW) == 0);
    for (u32 k = 0; k < 4; k++) {
        for (u32 i = 0; i < SV_TILE; i++) GP(c->cpu + k * SV_TILE)[i] = (u8)(0x31 * (k + 1) + i % c->eb * 7);
        /* the fourth page: the second level's second tile */
        if (k < 3 || c->levels > 1)
            gmmu_map(c->gva + (k < 3 ? mapped[k] : tiles + 1) * SV_TILE, c->cpu + k * SV_TILE, SV_TILE);
    }
    u64 uploads, bytes, uploads2, bytes2;
    vk_tex_stats(&uploads, &bytes);
    vk_lock();
    Surf* t = vk_tex_from_tic(tic);
    vk_tex_stats(&uploads2, &bytes2);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {tex.d.width, tex.d.height, tex.d.depth}};
    u8* image = t ? vk_readback_buffer(lay.size, &buffer) : NULL;
    if (image) {
        vk_surf_use(vk_cmd(), t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        rec_CmdCopyImageToBuffer(vk_cmd(), t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
        vk_finish();
        vk_readback_ready();
    }
    vk_unlock();
    CHECK(t && t->type == (c->is3d ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D) && image);
    const u32 across = 3 * tile[0] - c->cut, down = 2 * tile[1], deep = c->is3d ? 2 * tile[2] : 1;
    for (u32 z = 0; z < deep; z++)
        for (u32 y = 0; y < down; y++)
            for (u32 x = 0; x < across; x++) {
                u32 n = ((z / tile[2]) * 2 + y / tile[1]) * 3 + x / tile[0], k = 0;
                while (k < 3 && mapped[k] != n) k++;
                const u8* in = image + (((u64)z * down + y) * across + x) * c->eb;
                for (u32 i = 0; i < c->eb; i++)
                    if (in[i] != (k < 3 ? (u8)(0x31 * (k + 1) + i * 7) : 0)) {
                        if (!bad++) first[0] = x, first[1] = y, first[2] = z, first[3] = n;
                        break;
                    }
            }
    if (bad) {
        fprintf(stderr,
                "sparse tiles %s: %u of %u blocks aren't their tile's, the first (%u, %u, %u) of tile %u, which is %s; "
                "blocks %u GOBs high and %u deep, tiles %u GOBs across (the header's %u)\n",
                c->name, bad, across * down * deep, first[0], first[1], first[2], first[3],
                first[3] == mapped[0] || first[3] == mapped[1] || first[3] == mapped[2] ? "mapped" : "a hole",
                1u << lay.tile_h, 1u << lay.tile_d, 1u << lay.tile_w, 1u << ((tic[3] >> 10) & 7));
        return 1;
    }
    if (c->levels > 1) { /* the second level, through a view of it alone */
        NvnView v = {0};
        u32 w1 = tex.d.width / 2, h1 = tex.d.height / 2, across1 = w1 / c->bw, down1 = h1 / c->bw;
        v.set = NVN_VIEW_LEVELS;
        v.base_level = 1;
        v.levels = 1;
        CHECK(nvn_tic_encode(&tex, &v, 0, 0, tic));
        vk_lock();
        Surf* t1 = vk_tex_from_tic(tic);
        VkBufferImageCopy copy1 = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                   .imageExtent = {w1, h1, 1}};
        image = t1 ? vk_readback_buffer((u64)across1 * down1 * c->eb, &buffer) : NULL;
        if (image) {
            vk_surf_use(vk_cmd(), t1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                        VK_ACCESS_2_TRANSFER_READ_BIT);
            rec_CmdCopyImageToBuffer(vk_cmd(), t1->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy1);
            vk_finish();
            vk_readback_ready();
        }
        vk_unlock();
        CHECK(t1 && t1->w == w1 && t1->levels == 1 && image);
        for (u32 y = 0; y < down1; y++)
            for (u32 x = 0; x < across1; x++) {
                const u8* in = image + ((u64)y * across1 + x) * c->eb;
                for (u32 i = 0; i < c->eb; i++)
                    if (in[i] != (x >= tile[0] ? (u8)(0x31 * 4 + i * 7) : 0)) {
                        if (!bad++) first[0] = x, first[1] = y;
                        break;
                    }
            }
        if (bad) {
            fprintf(stderr,
                    "sparse tiles %s, level 1 (%ux%u): %u of %u blocks aren't their tile's, the first (%u, %u); tile 1 "
                    "is mapped, tile 0 a hole\n",
                    c->name, w1, h1, bad, across1 * down1, first[0], first[1]);
            return 1;
        }
    }
    /* what is mapped, read, and no hole: all of each mapped tile, but the padding past a cut row's texels */
    const u64 mapped_bytes = (c->levels > 1 ? 4 : 3) * (u64)SV_TILE;
    if (bytes2 - bytes > mapped_bytes || (!c->cut && bytes2 - bytes != mapped_bytes)) {
        fprintf(stderr, "sparse tiles %s: %llu bytes loaded to make it, its mapped tiles are %llu\n", c->name,
                (unsigned long long)(bytes2 - bytes), (unsigned long long)mapped_bytes);
        return 1;
    }
    printf("sparse tiles %s: each of the %u mapped tiles fills its %ux%ux%u brick of a %ux%ux%u texture of %u levels, "
           "the holes are zeros\n",
           c->name, c->levels > 1 ? 4 : 3, tile[0] * c->bw, tile[1] * c->bw, c->is3d ? tile[2] : 1, tex.d.width,
           tex.d.height, tex.d.depth, c->levels);
    return 0;
}
/* A view of levels 1 and 2 of a sparse texture, both smaller than a tile: RGBA16F 320x64 in tiles of 128x64 texels (16
   GOBs across, blocks 8 high). Level 1, 160x32, starts after level 0's 3 tiles and is half a tile high, so its rows
   are its own 20 GOBs, though the block shrunk to it (4 GOBs) is as high as it; level 2, 80x16, follows those rows.
   Every texel of both levels of the view is memory's */
static int check_sparse_view(void) {
    static const struct {
        u32 w, h, at, bh; /* a level's texels, its offset in the mapped page, its block height (log2 GOBs) */
    } lv[2] = {{160, 32, 0, 2}, {80, 16, 20u << 11, 1}}; /* level 2 after 20 GOBs of 4-GOB blocks */
    const u64 gva = 0x350000000ull, cpu = HEAP_BASE + 0xEC0000;
    NvnTexture tex = {0};
    NvnView v = {0};
    NvnTexLayout lay;
    u32 tic[8], bad = 0, first[3] = {0};
    VkBuffer buffer;
    tex.d.target = NVN_TEXTURE_TARGET_2D;
    tex.d.format = NVN_FORMAT_RGBA16F;
    tex.d.levels = 3;
    tex.d.flags = NVN_TEXTURE_FLAGS_SPARSE;
    tex.d.width = 320, tex.d.height = 64, tex.d.depth = 1;
    tex.gpu = gva;
    nvn_tex_layout(&tex.d, &lay);
    CHECK(lay.tile_w == 4 && lay.tile_h == 3 && nvn_tex_level_offset(&tex.d, 1) == 3 * SV_TILE + lv[0].at &&
          nvn_tex_level_offset(&tex.d, 2) == 3 * SV_TILE + lv[1].at && lay.size == 4 * SV_TILE);
    v.set = NVN_VIEW_LEVELS;
    v.base_level = 1;
    v.levels = 2;
    CHECK(nvn_tic_encode(&tex, &v, 0, 0, tic));
    if (vk_tic_format(tic[0], 0).vk != VK_FORMAT_R16G16B16A16_SFLOAT) {
        printf("sparse view: the GPU has no RGBA16F images, skipped\n");
        return 0;
    }
    CHECK(mem_map(cpu, SV_TILE, MS_NORMAL, PERM_RW) == 0);
    for (u32 i = 0; i < SV_TILE; i++) GP(cpu)[i] = (u8)(i * 13 + (i >> 9) * 7 + 1);
    gmmu_map(gva + 3 * SV_TILE, cpu, SV_TILE);
    vk_lock();
    Surf* t = vk_tex_from_tic(tic);
    VkBufferImageCopy copy[2] = {
        {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {lv[0].w, lv[0].h, 1}},
        {.bufferOffset = lv[0].w * lv[0].h * 8,
         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1},
         .imageExtent = {lv[1].w, lv[1].h, 1}}};
    u8* image = t && t->levels == 2 ? vk_readback_buffer((lv[0].w * lv[0].h + lv[1].w * lv[1].h) * 8, &buffer) : NULL;
    if (image) {
        vk_surf_use(vk_cmd(), t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        rec_CmdCopyImageToBuffer(vk_cmd(), t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 2, copy);
        vk_finish();
        vk_readback_ready();
    }
    vk_unlock();
    CHECK(t && t->w == 160 && t->h == 32 && t->levels == 2 && image);
    for (u32 l = 0; l < 2; l++)
        for (u32 y = 0; y < lv[l].h; y++)
            for (u32 x = 0; x < lv[l].w; x++) {
                const u8* in = image + copy[l].bufferOffset + ((u64)y * lv[l].w + x) * 8;
                if (memcmp(in, GP(cpu) + lv[l].at + bl_offset(x * 8, y, 0, lv[l].w * 8, lv[l].h, lv[l].bh, 0), 8) &&
                    !bad++)
                    first[0] = l + 1, first[1] = x, first[2] = y;
            }
    if (bad) {
        fprintf(stderr,
                "sparse view: %u texels of levels 1 and 2 aren't memory's, the first (%u, %u) of level %u; level 1's "
                "rows 20 GOBs across, the view's tile width %u\n",
                bad, first[1], first[2], first[0], t->tile_w);
        return 1;
    }
    puts("sparse view: a view of a sparse texture's levels smaller than a tile reads rows of their own GOBs");
    return 0;
}
static int check_sparse_tiles(void) {
    const StCase cases[] = {
        {"BC1", NVN_FORMAT_RGBA_DXT1, 8, 4, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 0x2C0000000ull, HEAP_BASE + 0xD00000, 1, 0,
         1},
        {"BC6H", NVN_FORMAT_BPTC_UFLOAT, 16, 4, VK_FORMAT_BC6H_UFLOAT_BLOCK, 0x2D0000000ull, HEAP_BASE + 0xD40000, 1, 0,
         1},
        {"RGBA8", NVN_FORMAT_RGBA8, 4, 1, VK_FORMAT_R8G8B8A8_UNORM, 0x2E0000000ull, HEAP_BASE + 0xD80000, 1, 0, 1},
        {"BC1 short", NVN_FORMAT_RGBA_DXT1, 8, 4, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 0x2F0000000ull, HEAP_BASE + 0xDC0000,
         1, 16, 1},
        {"RGBA8 short", NVN_FORMAT_RGBA8, 4, 1, VK_FORMAT_R8G8B8A8_UNORM, 0x320000000ull, HEAP_BASE + 0xE00000, 1, 16,
         1},
        {"RGBA8 2D short", NVN_FORMAT_RGBA8, 4, 1, VK_FORMAT_R8G8B8A8_UNORM, 0x330000000ull, HEAP_BASE + 0xE40000, 0,
         64, 2},
    };
    int failed = 0;
    for (u32 i = 0; i < sizeof cases / sizeof *cases; i++) failed |= sparse_tiles(&cases[i]);
    CHECK(!failed);
    return 0;
}
/* A copy into a sparse texture short of whole tiles across, through NVN as a game's CopyBufferToTexture is: RGBA8
   320x256, tiles of 128x128 (8 GOBs across, 3 tiles a row in memory), tiles (1, 1) and (2, 1) mapped and zero, a
   128x64 region at (192, 160) across both from a buffer. The image the header gives has the buffer's texels there and
   zeros elsewhere, and a target of the texture is as wide as its rows of whole tiles: 384 texels */
static int check_sparse_copy(void) {
    const u64 gva = 0x340000000ull, buf = 0x341000000ull, cpu = HEAP_BASE + 0xE80000, snap = cpu + 0x30000,
              rec = cpu + 0x30100;
    const u32 x0 = 192, y0 = 160, w = 128, h = 64;
    NvnTexture tex = {0};
    NvnTexLayout lay;
    NvnPCopyB2T r = {0};
    DsSurf target;
    int depth;
    u32 msaa, tic[8], bad = 0, first[2] = {0}, have = 0, want = 0;
    i64 stride[2] = {0, 0};
    VkBuffer buffer;
    tex.magic = NVN_MAGIC('T', 'E', 'X', 0);
    tex.d.target = NVN_TEXTURE_TARGET_2D;
    tex.d.format = NVN_FORMAT_RGBA8;
    tex.d.levels = 1;
    tex.d.flags = NVN_TEXTURE_FLAGS_SPARSE;
    tex.d.width = 320, tex.d.height = 256, tex.d.depth = 1;
    tex.gpu = gva;
    nvn_tex_layout(&tex.d, &lay);
    CHECK(lay.tile_w == 3 && lay.size == 6 * SV_TILE);
    CHECK(mem_map(cpu, 4 * SV_TILE, MS_NORMAL, PERM_RW) == 0);
    memset(GP(cpu), 0, 2 * SV_TILE);
    for (u32 i = 0; i < w * h; i++) {
        u32 v = 0x80000000u | i;
        memcpy(GP(cpu + 2 * SV_TILE) + i * 4, &v, 4);
    }
    gmmu_map(gva + 4 * SV_TILE, cpu, 2 * SV_TILE);
    gmmu_map(buf, cpu + 2 * SV_TILE, SV_TILE);
    memcpy(GP(snap), &tex, NVN_TEX_SNAP);
    CHECK(nvn_draw_surface(snap, NULL, &target, &depth, &msaa));
    if (target.width != 384) {
        fprintf(stderr, "sparse copy: a target of the texture is %u texels wide, its rows of whole tiles 384\n",
                target.width);
        return 1;
    }
    r.buffer = buf;
    r.texture = snap;
    r.r = (NVNcopyRegion){(int)x0, (int)y0, 0, (int)w, (int)h, 1};
    memcpy(GP(rec), &r, sizeof r);
    CHECK(nvn_copy_record(OP_COPY_B2T, rec, sizeof r, stride));
    CHECK(nvn_tic_encode(&tex, NULL, 0, 0, tic));
    vk_lock();
    Surf* t = vk_tex_from_tic(tic);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {320, 256, 1}};
    u8* image = t ? vk_readback_buffer(320 * 256 * 4, &buffer) : NULL;
    if (image) {
        vk_surf_use(vk_cmd(), t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        rec_CmdCopyImageToBuffer(vk_cmd(), t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
        vk_finish();
        vk_readback_ready();
    }
    vk_unlock();
    CHECK(t && image);
    for (u32 y = 0; y < 256; y++)
        for (u32 x = 0; x < 320; x++) {
            u32 in = 0, v = x - x0 < w && y - y0 < h ? 0x80000000u | ((y - y0) * w + (x - x0)) : 0;
            memcpy(&in, image + ((u64)y * 320 + x) * 4, 4);
            if (in != v && !bad++) first[0] = x, first[1] = y, have = in, want = v;
        }
    if (bad) {
        fprintf(stderr,
                "sparse copy: %u of %u texels aren't the copy's or zero, the first (%u, %u): %08x, not %08x; tiles %u "
                "GOBs across (the header's %u)\n",
                bad, 320u * 256u, first[0], first[1], have, want, 1u << lay.tile_w, 1u << ((tic[3] >> 10) & 7));
        return 1;
    }
    puts("sparse copy: a copy into a sparse texture short of whole tiles lands in its tiles, a target of it as wide");
    return 0;
}
/* A sparse volume of one game's light volumes' size, BC1 1664x448x1344 (477 MB, 13x7x84 tiles of 64 KiB), made
   through NVN with 3 tiles mapped. Where the device keeps a 3D BC1 image's memory only where it is bound (sparse
   residency, the rest reading as zeros) in bricks of the tile's 128x64x16 texels, the texture takes and loads those 3
   tiles alone: each mapped tile's brick holds its texels, the bricks around them read as zeros. Its memory follows the
   mapping: a tile mapped later takes 64 KiB more and shows in the next frame, an unmapped one gives its 64 KiB back
   and reads as zeros again */
#define RV_TILE 0x10000u
enum { RV_X = 13, RV_Y = 7, RV_Z = 84 }; /* tiles across, down and deep */
/* the bytes VMA has allocated in the heap images go to */
static u64 rv_allocated(void) {
    VmaBudget b[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(g_vk.vma, b);
    return b[vk_mem_heap()].statistics.allocationBytes;
}
/* whether the device can keep a 3D BC1 image's memory only where it is bound, in bricks of a tile, the rest reading as
   zeros */
static int rv_device_can(void) {
    VkPhysicalDeviceFeatures have;
    VkSparseImageFormatProperties p[2];
    u32 n = 2;
    vkGetPhysicalDeviceFeatures(g_vk.phys, &have);
    if (!g_vk.sparse || !have.sparseResidencyImage3D || !g_vk.props.sparseProperties.residencyNonResidentStrict)
        return 0;
    vkGetPhysicalDeviceSparseImageFormatProperties(
        g_vk.phys, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_IMAGE_TYPE_3D, VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        VK_IMAGE_TILING_OPTIMAL, &n, p);
    return n && p[0].imageGranularity.width == 128 && p[0].imageGranularity.height == 64 &&
           p[0].imageGranularity.depth == 16;
}
/* byte b of the page standing for tile k: every block differs from its neighbours and from zeros */
static u8 rv_byte(u32 k, u32 b) {
    u8 v = (u8)(((b >> 3) * 2654435761u >> 24) ^ (b & 7) * 29 ^ k * 53);
    return (b & 7) ? v : (u8)(v | 0x80);
}
/* the bricks of tiles[0..n) read back from t's image, each against memory: page from[i] of the 4 at cpu for a mapped
   tile, zeros for a hole (from[i] < 0). 0 when every block matches; else 1, said with the first that doesn't */
static int rv_check(const char* when, Surf* t, const u32* tiles, const int* from, u32 n, u64 cpu) {
    VkBufferImageCopy r[12];
    VkBuffer buffer;
    u8* image;
    u32 bad = 0, first[4] = {0};
    if (n > 12) return 1;
    vk_lock();
    image = vk_readback_buffer((u64)n * RV_TILE, &buffer);
    if (image) {
        for (u32 i = 0; i < n; i++) {
            u32 x = tiles[i] % RV_X, y = tiles[i] / RV_X % RV_Y, z = tiles[i] / (RV_X * RV_Y);
            r[i] = (VkBufferImageCopy){.bufferOffset = (u64)i * RV_TILE,
                                       .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                       .imageOffset = {(i32)x * 128, (i32)y * 64, (i32)z * 16},
                                       .imageExtent = {128, 64, 16}};
        }
        vk_surf_use(vk_cmd(), t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        rec_CmdCopyImageToBuffer(vk_cmd(), t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, n, r);
        vk_finish();
        vk_readback_ready();
    }
    vk_unlock();
    if (!image) return 1;
    for (u32 i = 0; i < n; i++)
        for (u32 bz = 0; bz < 16; bz++)
            for (u32 by = 0; by < 16; by++)
                for (u32 bx = 0; bx < 32; bx++) {
                    u32 x = tiles[i] % RV_X * 32 + bx, y = tiles[i] / RV_X % RV_Y * 16 + by,
                        z = tiles[i] / (RV_X * RV_Y) * 16 + bz;
                    /* the block's place in the volume's layout: 52 GOBs a row, blocks 2 GOBs high and 16 deep */
                    u64 o = bl_offset(x * 8, y, z, 1664 / 4 * 8, 448 / 4, 1, 4);
                    const u8* in = image + (u64)i * RV_TILE + ((bz * 16 + by) * 32 + bx) * 8;
                    u8 want[8] = {0};
                    if (o / RV_TILE != tiles[i]) return 1; /* the tile is one page of the layout */
                    if (from[i] >= 0)
                        for (u32 k = 0; k < 8; k++) want[k] = GP(cpu + (u64)from[i] * RV_TILE)[(o + k) % RV_TILE];
                    if (memcmp(in, want, 8) && !bad++) first[0] = i, first[1] = bx, first[2] = by, first[3] = bz;
                }
    if (bad)
        fprintf(stderr,
                "resident volume, %s: %u blocks differ from memory, the first block (%u, %u, %u) of tile %u (%s)\n",
                when, bad, first[1], first[2], first[3], tiles[first[0]], from[first[0]] >= 0 ? "mapped" : "a hole");
    return bad != 0;
}
static int check_resident_volume(void) {
    const u64 gva = 0x800000000ull, cpu = HEAP_BASE + 0xF00000; /* past the sparse tiles and copy cases' pages */
    /* tiles (0, 0, 0), (4, 3, 40) and (12, 6, 83) mapped from pages 0 to 2; (5, 3, 40) mapped later from page 3 */
    const u32 tiles[11] = {0, 3683, 7643, 1, 13, 91, 3682, 3670, 3774, 7642, 3684};
    const int from[11] = {0, 1, 2, -1, -1, -1, -1, -1, -1, -1, -1};
    NvnTexture tex = {0};
    NvnTexLayout lay;
    u32 tic[8];
    u64 loads[4], bytes[4], took[4], at[4];
    if (!rv_device_can()) {
        puts("resident volume: the device can't keep a 3D BC1 image's memory only where it is bound in bricks of a "
             "tile, skipped");
        return 0;
    }
    tex.d.target = NVN_TEXTURE_TARGET_3D;
    tex.d.format = NVN_FORMAT_RGBA_DXT1;
    tex.d.levels = 1;
    tex.d.flags = NVN_TEXTURE_FLAGS_SPARSE;
    tex.d.width = 1664, tex.d.height = 448, tex.d.depth = 1344;
    tex.gpu = gva;
    nvn_tex_layout(&tex.d, &lay);
    CHECK(lay.size == (u64)RV_X * RV_Y * RV_Z * RV_TILE);
    CHECK(nvn_tic_encode(&tex, NULL, 0, 0, tic));
    CHECK(mem_map(cpu, 4 * RV_TILE, MS_NORMAL, PERM_RW) == 0);
    gmmu_space_unmap(gva, lay.size); /* the pool: nothing mapped but its tiles */
    for (u32 k = 0; k < 4; k++)
        for (u32 b = 0; b < RV_TILE; b++) GP(cpu + (u64)k * RV_TILE)[b] = rv_byte(k, b);
    for (u32 k = 0; k < 3; k++) gmmu_map(gva + (u64)tiles[k] * RV_TILE, cpu + (u64)k * RV_TILE, RV_TILE);
    /* every frame's staging made first: what the volume allocates is counted, not the staging its upload goes
       through, in whichever frame a tile is mapped */
    VkBuffer staging;
    u64 staged;
    vk_lock();
    for (int i = 0; i < VK_FRAMES; i++) {
        CHECK(vk_stage(4 * RV_TILE, 16, &staging, &staged));
        vk_frame_next();
        vk_cmd();
    }
    vk_unlock();
    took[0] = rv_allocated();
    vk_tex_stats(&loads[0], &bytes[0]);
    at[0] = plat_ns();
    vk_lock();
    Surf* t = vk_tex_from_tic(tic);
    vk_unlock();
    at[1] = plat_ns();
    took[1] = rv_allocated();
    vk_tex_stats(&loads[1], &bytes[1]);
    CHECK(t && t->type == VK_IMAGE_VIEW_TYPE_3D && t->w == 1664 && t->h == 448 && t->depth3d == 1344);
    printf("resident volume: made in %.1f ms, its image %llu KiB, %llu KiB more allocated, %llu KiB loaded\n",
           (double)(at[1] - at[0]) / 1e6, (unsigned long long)(t->bytes >> 10),
           (unsigned long long)((took[1] - took[0]) >> 10), (unsigned long long)((bytes[1] - bytes[0]) >> 10));
    CHECK(!rv_check("made", t, tiles, from, 11, cpu));
    CHECK(t->bytes == 3 * RV_TILE && bytes[1] - bytes[0] == 3 * RV_TILE && took[1] - took[0] < (16ull << 20));
    /* frames go by: write tracking's scans take in the bytes written before it was made, which it loads again once */
    for (int i = 0; i < 60; i++) CHECK(sv_frame(tic) == t);
    vk_tex_stats(&loads[1], &bytes[1]);
    /* the game's MapVirtual: a tile mapped where it was a hole */
    gmmu_map(gva + (u64)tiles[10] * RV_TILE, cpu + 3 * RV_TILE, RV_TILE);
    at[2] = plat_ns();
    CHECK(sv_frame(tic) == t);
    at[3] = plat_ns();
    took[2] = rv_allocated();
    vk_tex_stats(&loads[2], &bytes[2]);
    const u32 now_mapped[2] = {tiles[10], tiles[1]};
    const int now_from[2] = {3, 1};
    CHECK(!rv_check("a tile mapped", t, now_mapped, now_from, 2, cpu));
    printf("resident volume: a tile mapped, the next frame's texture in %.1f ms, its image %llu KiB, %llu KiB more "
           "allocated, %llu KiB loaded\n",
           (double)(at[3] - at[2]) / 1e6, (unsigned long long)(t->bytes >> 10),
           (unsigned long long)((took[2] - took[1]) >> 10), (unsigned long long)((bytes[2] - bytes[1]) >> 10));
    /* the new tile's brick alone loaded */
    CHECK(t->bytes == 4 * RV_TILE && took[2] - took[1] >= RV_TILE && bytes[2] - bytes[1] == RV_TILE);
    for (int i = 0; i < 60; i++) CHECK(sv_frame(tic) == t);
    vk_tex_stats(&loads[2], &bytes[2]);
    /* a tile unmapped: a hole again */
    gmmu_space_unmap(gva, RV_TILE);
    CHECK(sv_frame(tic) == t);
    took[3] = rv_allocated();
    vk_tex_stats(&loads[3], &bytes[3]);
    const u32 unmapped[2] = {tiles[0], tiles[10]};
    const int unmapped_from[2] = {-1, 3};
    CHECK(!rv_check("a tile unmapped", t, unmapped, unmapped_from, 2, cpu));
    printf("resident volume: a tile unmapped, its image %llu KiB, %lld KiB allocated since, %llu KiB loaded\n",
           (unsigned long long)(t->bytes >> 10), (long long)((i64)(took[3] - took[2]) >> 10),
           (unsigned long long)((bytes[3] - bytes[2]) >> 10));
    CHECK(t->bytes == 3 * RV_TILE && took[3] - took[0] < (16ull << 20) && bytes[3] == bytes[2]); /* nothing loaded */
    gmmu_space_unmap(gva, lay.size);
    CHECK(mem_unmap(cpu, 4 * RV_TILE) == 0);
    puts("resident volume: memory for the mapped tiles alone, their texels, zeros in the holes, bound and released as "
         "tiles are mapped and unmapped");
    return 0;
}

/* The sparse binds' calls on the queue and the host's waits for the GPU (fences and timeline semaphores, with a
   timeout), counted on every thread through the device's entry points (volk's). And what the Vulkan spec asks of a
   bind ("Binding Resource Memory": binds are not ordered with command buffers, even on one queue), as the last two
   binds and the graphics submission after the last show it: what each waits for on the graphics timeline and on the
   binds' own, what it signals, and whether that submission waits for it. Some drivers order binds with the work
   around them anyway (RADV's queue takes its batches in order), so a missing wait shows only here there */
typedef struct {
    u64 binds, waits, wait_ns;
} ScCount;
typedef struct {
    u64 gfx;         /* its wait on the graphics timeline, 0 none */
    u64 after;       /* its wait on the semaphore it signals, the binds' own timeline: 0 none */
    VkSemaphore sem; /* what it signals, VK_NULL_HANDLE none */
    u64 value;
    int followed; /* the first graphics submission after it waits for that */
    int pending;  /* no graphics submission since it */
} ScBind;
static struct {
    PFN_vkQueueBindSparse bind; /* the device's own entry points */
    PFN_vkQueueSubmit2 submit;
    PFN_vkWaitForFences fences;
    PFN_vkWaitSemaphores sems;
    ScCount n;
    ScBind last, prev; /* written on the recording thread, read after sc_now */
} g_sc;
static VKAPI_ATTR VkResult VKAPI_CALL sc_bind(VkQueue q, u32 n, const VkBindSparseInfo* b, VkFence f) {
    ScBind x = {.pending = 1};
    const VkTimelineSemaphoreSubmitInfo* ts = NULL;
    for (const VkBaseInStructure* c = b[0].pNext; c; c = c->pNext)
        if (c->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) ts = (const void*)c;
    if (b[0].signalSemaphoreCount) {
        x.sem = b[0].pSignalSemaphores[0];
        x.value = ts && ts->signalSemaphoreValueCount ? ts->pSignalSemaphoreValues[0] : 0;
    }
    for (u32 i = 0; i < b[0].waitSemaphoreCount; i++) {
        u64 v = ts && i < ts->waitSemaphoreValueCount ? ts->pWaitSemaphoreValues[i] : 0;
        if (b[0].pWaitSemaphores[i] == vk_gfx_timeline()) x.gfx = v;
        if (x.sem && b[0].pWaitSemaphores[i] == x.sem) x.after = v;
    }
    g_sc.prev = g_sc.last;
    g_sc.last = x;
    __atomic_add_fetch(&g_sc.n.binds, 1, __ATOMIC_RELAXED);
    return g_sc.bind(q, n, b, f);
}
static VKAPI_ATTR VkResult VKAPI_CALL sc_submit(VkQueue q, u32 n, const VkSubmitInfo2* s, VkFence f) {
    if (q == g_vk.gfx && g_sc.last.pending) {
        for (u32 k = 0; k < n; k++)
            for (u32 i = 0; i < s[k].waitSemaphoreInfoCount; i++)
                if (g_sc.last.sem && s[k].pWaitSemaphoreInfos[i].semaphore == g_sc.last.sem &&
                    s[k].pWaitSemaphoreInfos[i].value >= g_sc.last.value)
                    g_sc.last.followed = 1;
        g_sc.last.pending = 0;
    }
    return g_sc.submit(q, n, s, f);
}
static void sc_waited(u64 t0, u64 timeout) {
    if (!timeout) return; /* a look, no wait */
    __atomic_add_fetch(&g_sc.n.waits, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_sc.n.wait_ns, plat_ns() - t0, __ATOMIC_RELAXED);
}
static VKAPI_ATTR VkResult VKAPI_CALL sc_fences(VkDevice d, u32 n, const VkFence* f, VkBool32 all, u64 timeout) {
    u64 t0 = plat_ns();
    VkResult r = g_sc.fences(d, n, f, all, timeout);
    sc_waited(t0, timeout);
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL sc_sems(VkDevice d, const VkSemaphoreWaitInfo* w, u64 timeout) {
    u64 t0 = plat_ns();
    VkResult r = g_sc.sems(d, w, timeout);
    sc_waited(t0, timeout);
    return r;
}
static ScCount sc_now(void) {
    vk_rec_sync(); /* the recording thread's calls so far */
    return (ScCount){__atomic_load_n(&g_sc.n.binds, __ATOMIC_RELAXED), __atomic_load_n(&g_sc.n.waits, __ATOMIC_RELAXED),
                     __atomic_load_n(&g_sc.n.wait_ns, __ATOMIC_RELAXED)};
}
static void sc_install(void) {
    if (g_sc.bind) return;
    g_sc.bind = vkQueueBindSparse;
    g_sc.submit = vkQueueSubmit2;
    g_sc.fences = vkWaitForFences;
    g_sc.sems = vkWaitSemaphores;
    vkQueueBindSparse = sc_bind;
    vkQueueSubmit2 = sc_submit;
    vkWaitForFences = sc_fences;
    vkWaitSemaphores = sc_sems;
}

/* A gate the test's GPU work waits at: a timeline semaphore the host signals to value. A watchdog thread opens it after
   400 ms, so a renderer that waits for that work on the host goes on (and the check says how long it waited) */
typedef struct {
    VkSemaphore sem;
    u64 value;
    int opened, ended;
} Gate;
static void gate_open(Gate* g) {
    VkSemaphoreSignalInfo si = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = g->sem, .value = g->value};
    if (!__atomic_exchange_n(&g->opened, 1, __ATOMIC_SEQ_CST)) VK_OK(vkSignalSemaphore(g_vk.dev, &si));
}
static void gate_watch(void* arg) {
    Gate* g = arg;
    for (int i = 0; i < 40 && !__atomic_load_n(&g->opened, __ATOMIC_SEQ_CST); i++) plat_sleep_ns(10000000);
    gate_open(g);
    __atomic_store_n(&g->ended, 1, __ATOMIC_SEQ_CST);
}
/* the gate opened, and its watchdog gone */
static void gate_end(Gate* g) {
    gate_open(g);
    while (!__atomic_load_n(&g->ended, __ATOMIC_SEQ_CST)) plat_sleep_ns(1000000);
}
static VkSemaphore gate_semaphore(void) {
    VkSemaphoreTypeCreateInfo tci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                     .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &tci};
    VkSemaphore s = VK_NULL_HANDLE;
    VK_OK(vkCreateSemaphore(g_vk.dev, &sci, NULL, &s));
    return s;
}

/* an image of the sparse checks cleared whole to rgba (from its layout from), then left to be read by copies */
static void sparse_clear(VkImage img, VkImageLayout from, const float rgba[4]) {
    VkCommandBuffer cb = vk_cmd();
    VkClearColorValue c;
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    memcpy(c.float32, rgba, sizeof c.float32);
    vk_barrier(cb, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
               from, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    rec_CmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);
    vk_barrier(cb, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
               VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
}
/* a copy of the texel at (x, 0, 0) of img into buf at at */
static void sparse_copy(VkBuffer buf, u64 at, VkImage img, u32 x) {
    VkBufferImageCopy r = {.bufferOffset = at,
                           .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .imageOffset = {(i32)x, 0, 0},
                           .imageExtent = {1, 1, 1}};
    rec_CmdCopyImageToBuffer(vk_cmd(), img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &r);
}
/* a 3D RGBA8 image two blocks wide with memory only where it is bound, none bound: 0 when the device can't */
static int sparse_resident_image(VkImage* img, VkExtent3D* g) {
    VkResidentShape s;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_3D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {1, 1, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (!vk_resident_shape(&ii, &s)) return 0; /* its block, then the shape of an image two of them wide */
    ii.extent = (VkExtent3D){s.block.width * 2, s.block.height, s.block.depth};
    if (!vk_resident_shape(&ii, &s)) return 0;
    *g = s.block;
    return vk_image_create_resident(&ii, &s, img);
}
static const float SPARSE_RED[4] = {1, 0, 0, 1}, SPARSE_GREEN[4] = {0, 1, 0, 1};
static const u8 SPARSE_RED8[4] = {255, 0, 0, 255}, SPARSE_GREEN8[4] = {0, 255, 0, 255}, SPARSE_ZERO8[4] = {0};

/* A block of a 3D image with memory only where it is bound, released after a copy that reads it was recorded: the
   release submits the copy, which waits on the GPU at a gate the test opens later, and returns without waiting for
   the GPU. The copy reads what the block held, a copy recorded after the release reads zeros, and the block's memory
   stays allocated until the frame's work is done. Then the block bound again while the work before the bind waits at
   the gate, and cleared and read at once: the read sees the new memory, as the submission after a bind waits for it */
static int check_sparse_unbind_in_flight(void) {
    u8 both[2] = {1, 1}, second[2] = {0, 1};
    VkExtent3D g;
    VkImage img;
    VkBuffer rb;
    u64 whole, bytes, sub, done;
    u8 got[6][4];
    sc_install();
    vk_lock();
    if (!sparse_resident_image(&img, &g)) {
        vk_unlock();
        puts(
            "sparse unbind in flight: the device can't keep a 3D RGBA8 image's memory only where it is bound, skipped");
        return 0;
    }
    CHECK(vk_image_resident(img, both, 2, &whole) && whole);
    u8* back = vk_readback_buffer(256, &rb);
    CHECK(back);
    sparse_clear(img, VK_IMAGE_LAYOUT_UNDEFINED, SPARSE_RED);
    sparse_copy(rb, 0, img, 0);
    sparse_copy(rb, 16, img, g.width);
    Gate gate = {gate_semaphore(), 1, 0, 0};
    CHECK(gate.sem && !plat_thread(gate_watch, &gate, 1u << 16));
    vk_submit_after(gate.sem, 1, 2); /* the submission of the copies waits at the gate */
    /* block 0 released */
    ScCount c0 = sc_now();
    u64 held0 = rv_allocated(), deferred0 = vk_deferred_bytes(), t0 = plat_ns();
    int ok = vk_image_resident(img, second, 2, &bytes);
    u64 t1 = plat_ns(), held1 = rv_allocated();
    ScCount c1 = sc_now();
    const u64 reader = vk_last_submit(); /* the copies' */
    vk_submit_progress(&sub, &done);
    const int reading = done < reader;
    sparse_copy(rb, 32, img, 0);
    sparse_copy(rb, 48, img, g.width);
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE); /* the bind, then these copies */
    u64 held2 = rv_allocated(), deferred2 = vk_deferred_bytes();
    sc_now();
    const ScBind unbind = g_sc.last;
    vk_submit_progress(&sub, &done);
    const int still = done < reader;
    gate_end(&gate);
    vk_finish();
    vk_readback_ready();
    memcpy(got[0], back, 4);
    memcpy(got[1], back + 16, 4);
    memcpy(got[2], back + 32, 4);
    memcpy(got[3], back + 48, 4);
    /* both frame slots used again: the released block's memory goes with the frame that waited for its bind */
    for (int i = 0; i < VK_FRAMES; i++) {
        vk_frame_next();
        vk_cmd();
    }
    u64 held3 = rv_allocated();
    vk_unlock();
    printf("sparse unbind in flight: the release took %.3f ms, %llu host waits, the GPU %s reading; %llu KiB of %llu "
           "freed at once, %llu KiB deferred, %llu KiB freed once the frames were done\n",
           (t1 - t0) / 1e6, (unsigned long long)(c1.waits - c0.waits), reading ? "still" : "no longer",
           (unsigned long long)((i64)(held0 - held1) >> 10), (unsigned long long)(whole >> 10),
           (unsigned long long)((deferred2 - deferred0) >> 10), (unsigned long long)((held0 - held3) >> 10));
    CHECK(ok && bytes == whole / 2);
    CHECK(reading && still && c1.waits == c0.waits && t1 - t0 < 100000000ull);     /* no wait for the GPU */
    CHECK(held1 == held0 && held2 == held0 && deferred2 - deferred0 == whole / 2); /* nothing freed under the copy */
    CHECK(held3 + whole / 2 <= held0);                                             /* freed once it is done */
    CHECK(unbind.gfx >= reader && unbind.followed); /* after the copy, and before the copies after it */
    CHECK(!memcmp(got[0], SPARSE_RED8, 4) && !memcmp(got[1], SPARSE_RED8, 4));  /* the copy read it bound */
    CHECK(!memcmp(got[2], SPARSE_ZERO8, 4) && !memcmp(got[3], SPARSE_RED8, 4)); /* and the one after, released */

    /* block 0 bound again while the work before the bind waits at the gate, then cleared and read */
    gate = (Gate){gate.sem, 3, 0, 0};
    CHECK(!plat_thread(gate_watch, &gate, 1u << 16));
    vk_lock();
    vk_submit_after(gate.sem, 3, 4);
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    const u64 gated = vk_last_submit();
    c0 = sc_now();
    t0 = plat_ns();
    ok = vk_image_resident(img, both, 2, &bytes);
    t1 = plat_ns();
    c1 = sc_now();
    sparse_clear(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, SPARSE_GREEN);
    sparse_copy(rb, 64, img, 0);
    sparse_copy(rb, 80, img, g.width);
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    sc_now();
    const ScBind rebind = g_sc.last;
    plat_sleep_ns(20000000); /* the GPU idle at the gate: the bind and the clear after it are what is left */
    gate_end(&gate);
    vk_finish();
    vk_readback_ready();
    memcpy(got[4], back + 64, 4);
    memcpy(got[5], back + 80, 4);
    vk_defer_image(img, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE);
    vk_unlock();
    vkDestroySemaphore(g_vk.dev, gate.sem, NULL);
    printf(
        "sparse unbind in flight, bound again: the bind took %.3f ms, %llu host waits; the read after it %02x%02x%02x%02x "
        "and %02x%02x%02x%02x\n",
        (t1 - t0) / 1e6, (unsigned long long)(c1.waits - c0.waits), got[4][0], got[4][1], got[4][2], got[4][3],
        got[5][0], got[5][1], got[5][2], got[5][3]);
    CHECK(ok && bytes == whole && c1.waits == c0.waits && t1 - t0 < 100000000ull);
    CHECK(rebind.gfx >= gated && rebind.followed); /* after the work before it, and before the clear */
    CHECK(!memcmp(got[4], SPARSE_GREEN8, 4) && !memcmp(got[5], SPARSE_GREEN8, 4));
    puts("sparse unbind in flight: a block released under a copy still reading it, without a host wait, its memory "
         "freed after the GPU; a block bound again read at once");
    return 0;
}

/* Binds made between two submissions go to the queue in one batch, with no host wait: two images with memory only
   where it is bound, bound one after the other, read at once */
static int check_sparse_resident_batched(void) {
    u8 both[2] = {1, 1}, second[2] = {0, 1};
    VkExtent3D g;
    VkImage a, b;
    VkBuffer rb;
    u64 bytes;
    sc_install();
    vk_lock();
    if (!sparse_resident_image(&a, &g)) {
        vk_unlock();
        puts("sparse binds batched: the device can't keep a 3D RGBA8 image's memory only where it is bound, skipped");
        return 0;
    }
    CHECK(sparse_resident_image(&b, &g));
    u8* back = vk_readback_buffer(256, &rb);
    CHECK(back);
    vk_finish();
    ScCount c0 = sc_now();
    CHECK(vk_image_resident(a, both, 2, &bytes) && vk_image_resident(b, both, 2, &bytes));
    sparse_clear(a, VK_IMAGE_LAYOUT_UNDEFINED, SPARSE_RED);
    sparse_clear(b, VK_IMAGE_LAYOUT_UNDEFINED, SPARSE_GREEN);
    sparse_copy(rb, 0, a, g.width);
    sparse_copy(rb, 16, b, g.width);
    ScCount c1 = sc_now();
    vk_finish();
    ScCount c2 = sc_now();
    vk_readback_ready();
    int right = !memcmp(back, SPARSE_RED8, 4) && !memcmp(back + 16, SPARSE_GREEN8, 4);
    /* a block of one image released and bound again with no submission between: a batch binds a block once, so
       these are two batches, the second after the first, and the read after them sees the second */
    ScCount c3 = sc_now();
    CHECK(vk_image_resident(a, second, 2, &bytes) && vk_image_resident(a, both, 2, &bytes));
    sparse_clear(a, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, SPARSE_GREEN);
    sparse_copy(rb, 32, a, 0);
    vk_finish();
    ScCount c4 = sc_now();
    const ScBind first = g_sc.prev, then = g_sc.last;
    vk_readback_ready();
    int again = !memcmp(back + 32, SPARSE_GREEN8, 4);
    vk_defer_image(a, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE);
    vk_defer_image(b, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE);
    vk_unlock();
    printf("sparse binds batched: 2 images bound one after the other, %llu queue binds, %llu host waits before the "
           "frame's end; a block released and bound again, %llu queue binds, the second after the first: %s\n",
           (unsigned long long)(c2.binds - c0.binds), (unsigned long long)(c1.waits - c0.waits),
           (unsigned long long)(c4.binds - c3.binds), first.value && then.after == first.value ? "yes" : "no");
    CHECK(right && c2.binds - c0.binds == 1 && c1.waits == c0.waits);
    CHECK(again && c4.binds - c3.binds == 2 && first.value && then.after == first.value && then.followed);
    return 0;
}
/* The same for three images in pieces made in one frame, each cleared and read whole */
static int check_sparse_pieces_batched(void) {
    VkImage pieces[3];
    VmaAllocation none[3];
    VkBuffer rb;
    if (!g_vk.sparse) {
        puts("sparse binds batched: no sparse binding, images in pieces skipped");
        return 0;
    }
    sc_install();
    vk_lock();
    /* RGBA8 256x256, 256 KiB: four pieces of 64 KiB each */
    const u64 size = 256 * 256 * 4, saved_cap = g_vk.alloc_cap;
    u8* back = vk_readback_buffer(3 * size, &rb); /* before the cap, which a readback buffer keeps within */
    CHECK(back);
    vk_finish();
    ScCount c0 = sc_now();
    g_vk.alloc_cap = 64u << 10;
    for (u32 i = 0; i < 3; i++) {
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .extent = {256, 256, 1},
                                .mipLevels = 1,
                                .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        CHECK(vk_image_create(&ii, &pieces[i], &none[i]) && !none[i]);
        sparse_clear(pieces[i], VK_IMAGE_LAYOUT_UNDEFINED, i == 1 ? SPARSE_GREEN : SPARSE_RED);
        VkBufferImageCopy r = {.bufferOffset = i * size,
                               .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               .imageExtent = {256, 256, 1}};
        rec_CmdCopyImageToBuffer(vk_cmd(), pieces[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &r);
    }
    g_vk.alloc_cap = saved_cap;
    ScCount c1 = sc_now();
    vk_finish();
    ScCount c2 = sc_now();
    vk_readback_ready();
    u32 wrong = 0;
    for (u64 at = 0; at < 3 * size; at += 4)
        wrong += !!memcmp(back + at, at / size == 1 ? SPARSE_GREEN8 : SPARSE_RED8, 4);
    for (u32 i = 0; i < 3; i++) vk_defer_image(pieces[i], VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE);
    vk_unlock();
    printf("sparse binds batched: 3 images of 4 pieces made in one frame, %llu queue binds, %llu host waits before the "
           "frame's end, %u texels not their color\n",
           (unsigned long long)(c2.binds - c0.binds), (unsigned long long)(c1.waits - c0.waits), wrong);
    CHECK(!wrong && c2.binds - c0.binds == 1 && c1.waits == c0.waits);
    puts("sparse binds batched: the binds made between two submissions go in one batch, without a host wait");
    return 0;
}
/* A sparse 2D texture as a game makes one through NVN (nvn_tex_layout, nvn_tic_encode): RGBA8 in tiles of 128x128
   texels, each a 64 KiB page, its levels at least a tile in size in rows of whole tiles and the smaller ones after
   them in the layer's last tile, its mip tail. Where the device binds such an image in blocks of a tile, with its mip
   tail from the first level smaller than one, and reads what isn't bound as zeros, the texture takes memory for its
   mapped tiles and mip tails alone */
#define RS_TILE 0x10000u
/* the format such a texture is made in: NVN's, its tile's texels across and down, and its image's blocks (texels
   across and bytes): RGBA8, or ASTC 4x4 held as BC7 (astc_mode=0, Auto), whose image is compared with the same texture made
   whole, not with memory */
typedef struct {
    const char* name;
    u16 format;
    u32 tile, bw, bpb;
    int astc;
} RsFmt;
static const RsFmt RS_RGBA8 = {"RGBA8", NVN_FORMAT_RGBA8, 128, 1, 4, 0};
static const RsFmt RS_ASTC4 = {"ASTC 4x4 as BC7", NVN_FORMAT_RGBA_ASTC_4x4, 256, 4, 16, 1};
typedef struct {
    NvnTexture tex;
    NvnTexLayout lay;
    u32 tic[8];
    u32 size, levels, layers, tiled; /* tiled: its levels at least a tile in size */
    RsFmt as;
} RsTex;
/* a tile of the texture: level == tiled for the mip tail of its layer */
typedef struct {
    u32 layer, level, x, y;
} RsAt;
static void rs_make(RsTex* r, const RsFmt* as, u32 size, u32 layers, u64 gva) {
    memset(r, 0, sizeof *r);
    r->as = *as;
    r->size = size;
    r->layers = layers;
    while (size >> r->levels) r->levels++;
    while (size >> r->tiled >= as->tile) r->tiled++;
    r->tex.d.target = layers > 1 ? NVN_TEXTURE_TARGET_2D_ARRAY : NVN_TEXTURE_TARGET_2D;
    r->tex.d.format = as->format;
    r->tex.d.flags = NVN_TEXTURE_FLAGS_SPARSE;
    r->tex.d.levels = (u8)r->levels;
    r->tex.d.width = r->tex.d.height = size;
    r->tex.d.depth = layers;
    r->tex.gpu = gva;
    nvn_tex_layout(&r->tex.d, &r->lay);
    nvn_tic_encode(&r->tex, NULL, 0, 0, r->tic);
}
/* the 64 KiB page of the texture's layout a tile is */
static u64 rs_page(const RsTex* r, RsAt a) {
    u32 across = a.level < r->tiled ? (r->size >> a.level) / r->as.tile : 1;
    return (a.layer * r->lay.layer_size + nvn_tex_level_offset(&r->tex.d, a.level)) / RS_TILE + (u64)a.y * across + a.x;
}
/* the image's bytes of level l of a layer */
static u64 rs_level_bytes(const RsTex* r, u32 l) {
    const u64 blocks = ((r->size >> l) + r->as.bw - 1) / r->as.bw;
    return blocks * blocks * r->as.bpb;
}
/* the texels (or blocks) of all its levels and layers, level after level, layer after layer */
static u64 rs_bytes(const RsTex* r) {
    u64 n = 0;
    for (u32 l = 0; l < r->levels; l++) n += rs_level_bytes(r, l);
    return n * r->layers;
}
/* whether the device binds the texture's image in blocks of a tile, its mip tail from its first level smaller than
   one, what isn't bound reading as zeros (made as tex_create makes it); its mip tails (one for all the layers or one
   a layer) and their size */
static int rs_device_can(const RsTex* r, u32* tails, u64* tail_size) {
    VkPhysicalDeviceFeatures have;
    VkFormatProperties fp;
    VkFormat views[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};
    VkSparseImageMemoryRequirements2 sr[2] = {{.sType = VK_STRUCTURE_TYPE_SPARSE_IMAGE_MEMORY_REQUIREMENTS_2},
                                              {.sType = VK_STRUCTURE_TYPE_SPARSE_IMAGE_MEMORY_REQUIREMENTS_2}};
    u32 n = 2;
    if (r->as.astc) views[0] = VK_FORMAT_BC7_UNORM_BLOCK, views[1] = VK_FORMAT_BC7_SRGB_BLOCK;
    vkGetPhysicalDeviceFeatures(g_vk.phys, &have);
    if (!g_vk.sparse || !have.sparseResidencyImage2D || !g_vk.props.sparseProperties.residencyNonResidentStrict)
        return 0;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, views[0], &fp);
    VkImageFormatListCreateInfo list = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, .viewFormatCount = 2, .pViewFormats = views};
    VkImageCreateInfo ii = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &list,
        .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_SPARSE_BINDING_BIT |
                 VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = views[0],
        .extent = {r->size, r->size, 1},
        .mipLevels = r->levels,
        .arrayLayers = r->layers,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT ? VK_IMAGE_USAGE_STORAGE_BIT : 0)};
    VkDeviceImageMemoryRequirements q = {.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
                                         .pCreateInfo = &ii};
    vkGetDeviceImageSparseMemoryRequirements(g_vk.dev, &q, &n, sr);
    const VkSparseImageMemoryRequirements* m = &sr[0].memoryRequirements;
    *tails = m->formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT ? 1 : r->layers;
    *tail_size = m->imageMipTailSize;
    return n == 1 && m->formatProperties.imageGranularity.width == r->as.tile &&
           m->formatProperties.imageGranularity.height == r->as.tile &&
           m->formatProperties.imageGranularity.depth == 1 && m->imageMipTailFirstLod == r->tiled;
}
/* every level of every layer of t's image read back (rs_bytes), malloc'd; NULL when it couldn't be */
static u8* rs_image(Surf* t, const RsTex* r) {
    VkBufferImageCopy c[64];
    VkBuffer buffer;
    u64 at = 0;
    u32 n = 0;
    for (u32 layer = 0; layer < r->layers; layer++)
        for (u32 l = 0; l < r->levels && n < 64; l++) {
            const u32 d = r->size >> l;
            c[n++] = (VkBufferImageCopy){.bufferOffset = at,
                                         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, layer, 1},
                                         .imageExtent = {d, d, 1}};
            at += rs_level_bytes(r, l);
        }
    vk_lock();
    u8* back = vk_readback_buffer(at, &buffer);
    u8* out = back ? malloc((size_t)at) : NULL;
    if (out) {
        vk_surf_use(vk_cmd(), t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        rec_CmdCopyImageToBuffer(vk_cmd(), t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, n, c);
        vk_finish();
        vk_readback_ready();
        memcpy(out, back, (size_t)at);
    }
    vk_unlock();
    return out;
}
/* the same from guest memory, laid out as the hardware does it (nvn_tex_layout), the holes zeros */
static u8* rs_memory(const RsTex* r) {
    const u64 ls = r->lay.layer_size;
    u8 *layer = malloc((size_t)ls), *out = malloc((size_t)rs_bytes(r));
    u64 at = 0;
    if (!layer || !out) {
        free(layer);
        free(out);
        return NULL;
    }
    for (u32 k = 0; k < r->layers; k++) {
        gmem_read(r->tex.gpu + k * ls, layer, ls); /* -1 for the holes, which read as zeros */
        for (u32 l = 0; l < r->levels; l++) {
            const u32 d = r->size >> l, gy = (d + 7) / 8, tw = 1u << r->lay.tile_w;
            u32 gobs = (d * 4 + 63) / 64, bh = r->lay.tile_h;
            if (l < r->tiled) gobs = (gobs + tw - 1) / tw * tw; /* rows of whole tiles */
            while (bh > 0 && gy <= 1u << (bh - 1)) bh--;        /* the block shrunk to a small level */
            const u8* lv = layer + nvn_tex_level_offset(&r->tex.d, l);
            for (u32 y = 0; y < d; y++)
                for (u32 x = 0; x < d; x++)
                    memcpy(out + at + ((u64)y * d + x) * 4, lv + bl_offset(x * 4, y, 0, gobs * 64, d, bh, 0), 4);
            at += (u64)d * d * 4;
        }
    }
    free(layer);
    return out;
}
/* An ASTC one's image, held as BC7, against the same texture made whole (ref), which decodes the holes' zero bytes:
   where memory maps any byte of a tile (or of a mip tail), its blocks are ref's, elsewhere zeros; t made whole too is
   ref's everywhere. 1 when it differs, said with the first block that does, or when no block compared is ref's
   decode of anything but zeros */
static int rs_differ_astc(const char* when, Surf* t, Surf* ref, const RsTex* r) {
    static const u8 zero[16];
    u8 *image = rs_image(t, r), *whole = rs_image(ref, r), mapped[512] = {0};
    const u64 pages = r->lay.size / RS_TILE;
    u64 o = 0, cpu, decoded = 0;
    int bad = !image || !whole || pages > sizeof mapped;
    for (u64 k = 0; !bad && k < pages; k++)
        for (u64 b = 0; b < RS_TILE && !mapped[k]; b += 0x1000)
            mapped[k] = gmmu_run(r->tex.gpu + k * RS_TILE + b, 1, &cpu) != 0;
    for (u32 layer = 0; layer < r->layers && !bad; layer++)
        for (u32 l = 0; l < r->levels && !bad; l++) {
            const u32 nb = ((r->size >> l) + 3) / 4;
            for (u32 by = 0; by < nb && !bad; by++)
                for (u32 bx = 0; bx < nb && !bad; bx++, o += 16) {
                    const RsAt a = {layer, l < r->tiled ? l : r->tiled, l < r->tiled ? bx * 4 / r->as.tile : 0,
                                    l < r->tiled ? by * 4 / r->as.tile : 0};
                    const int in = !t->bricks || mapped[rs_page(r, a)];
                    decoded += in && memcmp(whole + o, zero, 16) != 0;
                    if (!memcmp(image + o, in ? whole + o : zero, 16)) continue;
                    fprintf(stderr,
                            "resident texture %ux%ux%u %s, %s: block (%u, %u) of level %u, layer %u differs from %s\n",
                            r->size, r->size, r->layers, r->as.name, when, bx, by, l, layer,
                            in ? "the texture made whole" : "zeros (a hole)");
                    bad = 1;
                }
        }
    if (!bad && !decoded)
        fprintf(stderr, "resident texture %ux%ux%u %s, %s: no block compared holds texels\n", r->size, r->size,
                r->layers, r->as.name, when);
    free(image);
    free(whole);
    return bad || !decoded;
}
/* t's image against memory, and against the same texture made whole (ref): 1 when either differs, said with the first
   texel that does */
static int rs_differ(const char* when, Surf* t, Surf* ref, const RsTex* r) {
    if (r->as.astc) return rs_differ_astc(when, t, ref, r);
    u8 *image = rs_image(t, r), *whole = rs_image(ref, r), *memory = rs_memory(r);
    const u64 n = rs_bytes(r);
    int bad = !image || !whole || !memory;
    for (u64 o = 0; !bad && o < n; o += 4) {
        if (!memcmp(image + o, memory + o, 4) && !memcmp(whole + o, memory + o, 4)) continue;
        u64 left = o / 4;
        u32 layer = 0, l = 0;
        while (left >= (u64)(r->size >> l) * (r->size >> l)) {
            left -= (u64)(r->size >> l) * (r->size >> l);
            if (++l == r->levels) l = 0, layer++;
        }
        fprintf(stderr,
                "resident texture %ux%ux%u, %s: texel (%u, %u) of level %u, layer %u is %08x, %08x made whole, %08x in "
                "memory\n",
                r->size, r->size, r->layers, when, (u32)(left % (r->size >> l)), (u32)(left / (r->size >> l)), l, layer,
                snx_ld_le32(image + o), snx_ld_le32(whole + o), snx_ld_le32(memory + o));
        bad = 1;
    }
    free(image);
    free(whole);
    free(memory);
    return bad;
}
/* page k of the texture's memory: its bytes, or for an ASTC one blocks that each decode to a color of their own (void
   extent blocks, red the page's number, green and blue the block's in it), so a block decoded in the wrong place
   shows */
static void rs_fill(const RsTex* r, u8* page, u32 k) {
    for (u32 i = 0; i < RS_TILE; i++) page[i] = rv_byte(k, i);
    for (u32 i = 0; r->as.astc && i < RS_TILE / 16; i++) {
        snx_st_le64(page + i * 16, 0xFFFFFFFFFFFFFDFCull); /* void extent, LDR, no extent */
        snx_st_le64(page + i * 16 + 8,
                    (u64)(k & 0xFF) << 8 | (u64)(i & 0xFF) << 24 | (u64)(i >> 8 & 0xFF) << 40 | 0xFFull << 56);
    }
}
/* the bytes t's bricks and mip tails would take with the tiles at[0..n) mapped */
static u64 rs_bound(const RsTex* r, const RsAt* at, u32 n, u32 tails, u64 block, u64 tail_size) {
    u64 bytes = 0;
    u32 tail_of = 0; /* the layers whose mip tail is mapped, a bit each */
    for (u32 i = 0; i < n; i++)
        if (at[i].level < r->tiled)
            bytes += block;
        else
            tail_of |= 1u << at[i].layer;
    for (u32 k = 0; k < r->layers; k++)
        if (tail_of >> k & 1) bytes += tails == 1 ? 0 : tail_size;
    return bytes + (tails == 1 && tail_of ? tail_size : 0);
}
/* one frame of both textures (sv_frame): the same ones, each loaded where its memory changed */
static int rs_frames(const RsTex* r, const RsTex* ref, Surf* t, Surf* whole, int n) {
    for (int i = 0; i < n; i++)
        if (sv_frame(r->tic) != t || sv_frame(ref->tic) != whole) return 1;
    return 0;
}
/* The texture made with its tiles at[0..made) mapped, the same pages mapped at ref_gva behind a header the device
   makes whole (no sparse residency of 2D images), and at ref2_gva with holes that don't read as zeros: then the tile
   at[made] mapped, at[made + 1] unmapped, and the first mip tail mapped (the last of them) unmapped and mapped again
   by 4 KiB of it alone, over memory just freed. Each time its image holds memory's texels, zeros in the holes, as the
   whole one does, its memory that of its mapped tiles and mip tails, and it loads the tiles that changed */
static int resident_texture(const RsFmt* as, u32 size, u32 layers, u64 gva, u64 ref_gva, u64 ref2_gva, u64 cpu,
                            const RsAt* at, u32 made) {
    RsTex r, ref, ref2;
    u32 tails;
    u64 tail_size, uploads, bytes[2], took[2];
    rs_make(&r, as, size, layers, gva);
    rs_make(&ref, as, size, layers, ref_gva);
    rs_make(&ref2, as, size, layers, ref2_gva);
    if (!rs_device_can(&r, &tails, &tail_size)) {
        printf("resident texture %ux%ux%u %s: the device doesn't bind it in blocks of a tile with its mip tail from "
               "level %u, the rest reading as zeros, skipped\n",
               size, size, layers, as->name, r.tiled);
        return 0;
    }
    u32 bricks = 0, tail = 0; /* the bricks of a layer; the first mip tail mapped */
    for (u32 l = 0; l < r.tiled; l++) bricks += (size >> l) / as->tile * ((size >> l) / as->tile);
    while (tail < made && at[tail].level < r.tiled) tail++;
    CHECK(tail < made && mem_map(cpu, (made + 3) * (u64)RS_TILE, MS_NORMAL, PERM_RW) == 0);
    for (u32 k = 0; k < made + 3; k++) rs_fill(&r, GP(cpu + (u64)k * RS_TILE), k + 7);
    for (u32 k = 0; k < made; k++)
        for (u32 j = 0; j < 3; j++)
            gmmu_map((j ? j == 1 ? ref_gva : ref2_gva : gva) + rs_page(&r, at[k]) * RS_TILE, cpu + (u64)k * RS_TILE,
                     RS_TILE);
    took[0] = rv_allocated();
    vk_tex_stats(&uploads, &bytes[0]);
    vk_lock();
    Surf* t = vk_tex_from_tic(r.tic);
    vk_unlock();
    took[1] = rv_allocated();
    vk_tex_stats(&uploads, &bytes[1]);
    /* a device without sparse residency of 2D images, and one whose holes don't read as zeros: made whole */
    const VkBool32 had2d = g_vk.feat.sparseResidencyImage2D;
    const int had_resident = g_vk.resident;
    vk_lock();
    g_vk.feat.sparseResidencyImage2D = VK_FALSE;
    Surf* whole = vk_tex_from_tic(ref.tic);
    g_vk.feat.sparseResidencyImage2D = had2d;
    g_vk.resident = 0;
    Surf* whole2 = vk_tex_from_tic(ref2.tic);
    g_vk.resident = had_resident;
    vk_unlock();
    CHECK(t && whole && whole2 && t->levels == r.levels && t->layers == layers);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_vk.dev, t->img, &req);
    const u64 block = req.alignment, want = rs_bound(&r, at, made, tails, block, tail_size);
    printf("resident texture %ux%ux%u %s, %u levels (%u in tiles), %u mip tails of %llu KiB: made with %u tiles "
           "mapped, its image %llu KiB, %llu KiB more allocated, %llu KiB loaded; made whole %llu KiB, %llu KiB\n",
           size, size, layers, as->name, r.levels, r.tiled, tails, (unsigned long long)(tail_size >> 10), made,
           (unsigned long long)(t->bytes >> 10), (unsigned long long)((took[1] - took[0]) >> 10),
           (unsigned long long)((bytes[1] - bytes[0]) >> 10), (unsigned long long)(whole->bytes >> 10),
           (unsigned long long)(whole2->bytes >> 10));
    /* its memory that of its mapped tiles and tails, what was allocated nearer that than the whole image */
    CHECK(t->bricks == bricks && t->bytes == want && block == RS_TILE && want < whole->bytes);
    CHECK(took[1] - took[0] <= want + (whole->bytes - want) / 2);
    CHECK(!whole->bricks && !whole2->bricks && whole->bytes == whole2->bytes);
    /* each mapped tile once, whole; the mip tail's levels alone */
    CHECK(bytes[1] - bytes[0] <= (u64)made * RS_TILE && bytes[1] - bytes[0] > (u64)(made - 1) * RS_TILE);
    CHECK(!rs_differ("made", t, whole, &r) && !rs_differ("made", whole2, whole, &ref2));
    CHECK(!rs_frames(&r, &ref, t, whole, 60)); /* write tracking's scans take in what was written before */
    /* a tile mapped: memory for it, and its texels */
    for (u32 j = 0; j < 2; j++)
        gmmu_map((j ? ref_gva : gva) + rs_page(&r, at[made]) * RS_TILE, cpu + (u64)made * RS_TILE, RS_TILE);
    vk_tex_stats(&uploads, &bytes[0]);
    CHECK(sv_frame(r.tic) == t);
    vk_tex_stats(&uploads, &bytes[1]);
    CHECK(sv_frame(ref.tic) == whole);
    CHECK(t->bytes == want + block && !rs_differ("a tile mapped", t, whole, &r));
    CHECK(bytes[1] - bytes[0] == RS_TILE); /* the tile alone (an ASTC one made whole, its memory no longer with
                                               holes, is checked by its hash) */
    /* a tile unmapped: its memory goes, it reads zeros */
    for (u32 j = 0; j < 2; j++) gmmu_space_unmap((j ? ref_gva : gva) + rs_page(&r, at[made + 1]) * RS_TILE, RS_TILE);
    CHECK(!rs_frames(&r, &ref, t, whole, 1));
    CHECK(t->bytes == want && !rs_differ("a tile unmapped", t, whole, &r));
    /* a mip tail unmapped, its memory released and freed once the frames are done; then 4 KiB of it mapped again from
       another page: the tail bound again, likely over the memory it had, and loaded whole, the rest of it zeros */
    for (u32 j = 0; j < 2; j++) gmmu_space_unmap((j ? ref_gva : gva) + rs_page(&r, at[tail]) * RS_TILE, RS_TILE);
    CHECK(!rs_frames(&r, &ref, t, whole, 1));
    const u64 untailed = t->bytes;
    CHECK(untailed + tail_size == want && !rs_differ("a mip tail unmapped", t, whole, &r));
    vk_lock();
    for (int i = 0; i < VK_FRAMES; i++) {
        vk_frame_next();
        vk_cmd();
    }
    vk_unlock();
    for (u32 j = 0; j < 2; j++)
        gmmu_map((j ? ref_gva : gva) + rs_page(&r, at[tail]) * RS_TILE, cpu + (u64)(made + 2) * RS_TILE, 0x1000);
    CHECK(!rs_frames(&r, &ref, t, whole, 1));
    CHECK(t->bytes == want && !rs_differ("4 KiB of a mip tail mapped", t, whole, &r));
    CHECK(!rs_frames(&r, &ref, t, whole, 60));
    CHECK(!rs_differ("frames later", t, whole, &r));
    for (u32 j = 0; j < 3; j++) gmmu_space_unmap(j ? j == 1 ? ref_gva : ref2_gva : gva, r.lay.size);
    CHECK(mem_unmap(cpu, (made + 3) * (u64)RS_TILE) == 0);
    printf("resident texture %ux%ux%u: memory for its mapped tiles and mip tails alone, their texels and zeros in the "
           "holes in each level, as made whole, bound and released as tiles are mapped and unmapped\n",
           size, size, layers);
    return 0;
}
static int check_resident_texture(void) {
    /* 2048x2048, 12 levels, levels 0 to 4 in tiles: two tiles of level 0, one of level 1 and of level 4, the mip tail;
       then a tile of level 2 mapped, the one of level 1 unmapped */
    static const RsAt one[7] = {{0, 0, 1, 1}, {0, 0, 12, 5}, {0, 1, 1, 1}, {0, 4, 0, 0},
                                {0, 5, 0, 0}, {0, 2, 3, 2},  {0, 1, 1, 1}};
    /* 1024x1024 of 2 layers, 11 levels, levels 0 to 3 in tiles: a tile of level 0 of layer 1, one of level 2 of layer
       0, the mip tail of layer 1; then a tile of level 0 of layer 0 mapped, the one of layer 1 unmapped */
    static const RsAt two[5] = {{1, 0, 1, 1}, {0, 2, 1, 0}, {1, 4, 0, 0}, {0, 0, 3, 2}, {1, 0, 1, 1}};
    CHECK(!resident_texture(&RS_RGBA8, 2048, 1, 0x900000000ull, 0x910000000ull, 0x920000000ull, HEAP_BASE + 0x1200000,
                            one, 5));
    CHECK(!resident_texture(&RS_RGBA8, 1024, 2, 0x930000000ull, 0x940000000ull, 0x950000000ull, HEAP_BASE + 0x1280000,
                            two, 3));
    /* 512x512, 10 levels, levels 0 to 2 in tiles: all its tiles but (3, 3) of level 0 and (1, 1) of level 1, more than
       half of it, which loads whole, a brick at a time into its level; then (3, 3) mapped, (0, 0) of level 1 unmapped */
    RsAt most[22];
    u32 n = 0;
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++)
            if (x != 3 || y != 3) most[n++] = (RsAt){0, 0, x, y};
    for (u32 y = 0; y < 2; y++)
        for (u32 x = 0; x < 2; x++)
            if (x != 1 || y != 1) most[n++] = (RsAt){0, 1, x, y};
    most[n++] = (RsAt){0, 2, 0, 0};
    most[n++] = (RsAt){0, 3, 0, 0};
    most[n] = (RsAt){0, 0, 3, 3};
    most[n + 1] = (RsAt){0, 1, 0, 0};
    CHECK(n == 20);
    CHECK(!resident_texture(&RS_RGBA8, 512, 1, 0x970000000ull, 0x980000000ull, 0x990000000ull, HEAP_BASE + 0x1300000,
                            most, n));
    return 0;
}
/* the same for an ASTC 4x4 texture held as BC7 (astc_mode=0, Auto), whose tiles of 64x64 blocks are BC7's 256x256 texels:
   its bound tiles are decoded where they lie, its image the same texture's made whole there and zeros elsewhere */
static int check_resident_astc(void) {
    const int had = g_settings.astc_mode;
    RsTex probe;
    g_settings.astc_mode = 0;
    rs_make(&probe, &RS_ASTC4, 256, 1, 0xA10000000ull);
    const VkFmt f = vk_tic_format(probe.tic[0], 0);
    if (f.conv != VKC_ASTC || f.vk != VK_FORMAT_BC7_UNORM_BLOCK) {
        printf("resident texture ASTC: the GPU %s, skipped\n",
               f.conv != VKC_ASTC ? "samples ASTC as it is" : "holds no ASTC texture as BC7");
        g_settings.astc_mode = had;
        return 0;
    }
    /* the decoder's buffers made first, by a texture of its own mapped whole: what the textures below allocate is
       counted without them */
    CHECK(mem_map(HEAP_BASE + 0x1200000, probe.lay.size, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(probe.tex.gpu, HEAP_BASE + 0x1200000, probe.lay.size);
    vk_lock();
    CHECK(vk_tex_from_tic(probe.tic));
    vk_finish();
    vk_unlock();
    gmmu_space_unmap(probe.tex.gpu, probe.lay.size);
    CHECK(mem_unmap(HEAP_BASE + 0x1200000, probe.lay.size) == 0);
    /* 2048x2048, 12 levels, levels 0 to 3 in tiles: two tiles of level 0, one of level 1 and of level 3, the mip tail;
       then a tile of level 2 mapped, the one of level 1 unmapped */
    static const RsAt one[7] = {{0, 0, 1, 1}, {0, 0, 6, 5}, {0, 1, 1, 1}, {0, 3, 0, 0},
                                {0, 4, 0, 0}, {0, 2, 1, 0}, {0, 1, 1, 1}};
    /* 512x512, 10 levels, levels 0 and 1 in tiles: all its tiles but (1, 1) of level 0, and the mip tail, more than
       half of it, which loads whole, a tile at a time; then (1, 1) mapped, level 1's unmapped */
    static const RsAt most[7] = {{0, 0, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}, {0, 1, 0, 0},
                                 {0, 2, 0, 0}, {0, 0, 1, 1}, {0, 1, 0, 0}};
    int failed = resident_texture(&RS_ASTC4, 2048, 1, 0xA10000000ull, 0xA20000000ull, 0xA30000000ull,
                                  HEAP_BASE + 0x1200000, one, 5);
    failed |= resident_texture(&RS_ASTC4, 512, 1, 0xA40000000ull, 0xA50000000ull, 0xA60000000ull, HEAP_BASE + 0x1200000,
                               most, 5);
    g_settings.astc_mode = had;
    return failed;
}

/* the block of level 0 of t's image at (x, 0) copied into buf at at: a texel, or a 4x4 block of a BC7 image */
static void rs_copy(VkBuffer buf, u64 at, Surf* t, u32 x) {
    const u32 bw = t->f.vk == VK_FORMAT_BC7_UNORM_BLOCK || t->f.vk == VK_FORMAT_BC7_SRGB_BLOCK ? 4 : 1;
    VkBufferImageCopy r = {.bufferOffset = at,
                           .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .imageOffset = {(i32)x, 0, 0},
                           .imageExtent = {bw, bw, 1}};
    rec_CmdCopyImageToBuffer(vk_cmd(), t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &r);
}
/* what that block of the texture holds: memory's texel, or an ASTC one's block of the same texture made whole */
static int rs_want(const RsTex* r, Surf* whole, u32 x, u8* out) {
    u8* all = r->as.astc ? rs_image(whole, r) : rs_memory(r);
    if (!all) return 0;
    memcpy(out, all + (u64)x / r->as.bw * r->as.bpb, r->as.bpb);
    free(all);
    return 1;
}
/* A tile of a resident 2D texture (4x4 tiles wide, 3 levels in tiles) unmapped while a copy that reads it waits at a
   gate on the GPU: the texture's next check releases the tile's block without a host wait, after the copy, which
   reads the tile's texels; a copy after it reads zeros, and the block's memory is freed once the GPU is done. Then the
   tile mapped again from another page while the work before waits at a gate: the bind waits for that work, the
   tile's texels are loaded after it, and a read after them sees the new ones. An ASTC one's texels are the same
   texture's made whole (at ref_gva) */
static int resident_in_flight(const RsFmt* as, u64 gva, u64 ref_gva, u64 cpu) {
    const RsAt a = {0, 0, 1, 0}, b = {0, 0, 2, 0}; /* tiles 1 and 2 of row 0 */
    const u32 xa = as->tile + 2 * as->bw, xb = 2 * as->tile + 4 * as->bw, n = as->bpb;
    static const u8 zero[16];
    RsTex r, ref;
    u32 tails;
    u64 tail_size, sub, done;
    VkBuffer rb;
    u8 got[5][16], want[3][16];
    Surf* whole = NULL;
    rs_make(&r, as, 4 * as->tile, 1, gva);
    rs_make(&ref, as, 4 * as->tile, 1, ref_gva);
    const VkFmt f = vk_tic_format(r.tic[0], 0);
    if (as->astc && (f.conv != VKC_ASTC || f.vk != VK_FORMAT_BC7_UNORM_BLOCK)) {
        printf("resident texture in flight %s: the GPU %s, skipped\n", as->name,
               f.conv != VKC_ASTC ? "samples ASTC as it is" : "holds no ASTC texture as BC7");
        return 0;
    }
    if (!rs_device_can(&r, &tails, &tail_size)) {
        printf("resident texture in flight: the device doesn't bind a 2D %s texture in blocks of a tile, skipped\n",
               as->name);
        return 0;
    }
    sc_install();
    CHECK(mem_map(cpu, 3 * (u64)RS_TILE, MS_NORMAL, PERM_RW) == 0);
    for (u32 k = 0; k < 3; k++) rs_fill(&r, GP(cpu + (u64)k * RS_TILE), k + 20);
    for (u32 j = 0; j < 1u + (u32)as->astc; j++) {
        gmmu_map((j ? ref_gva : gva) + rs_page(&r, a) * RS_TILE, cpu, RS_TILE);
        gmmu_map((j ? ref_gva : gva) + rs_page(&r, b) * RS_TILE, cpu + RS_TILE, RS_TILE);
    }
    vk_lock();
    Surf* t = vk_tex_from_tic(r.tic);
    if (as->astc) {
        const VkBool32 had2d = g_vk.feat.sparseResidencyImage2D;
        g_vk.feat.sparseResidencyImage2D = VK_FALSE;
        whole = vk_tex_from_tic(ref.tic);
        g_vk.feat.sparseResidencyImage2D = had2d;
    }
    vk_unlock();
    CHECK(t && t->bricks && (!as->astc || (whole && !whole->bricks)));
    CHECK(rs_want(&r, whole, xa, want[0]) && rs_want(&r, whole, xb, want[1]));
    vk_lock();
    u8* back = vk_readback_buffer(256, &rb);
    CHECK(back);
    vk_surf_use(vk_cmd(), t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    rs_copy(rb, 0, t, xa);
    rs_copy(rb, 16, t, xb);
    Gate gate = {gate_semaphore(), 1, 0, 0};
    CHECK(gate.sem && !plat_thread(gate_watch, &gate, 1u << 16));
    vk_submit_after(gate.sem, 1, 2); /* the submission of the copies waits at the gate */
    vk_unlock();
    /* tile a unmapped: the next frame's check releases its block */
    ScCount c0 = sc_now();
    u64 held0 = rv_allocated(), deferred0 = vk_deferred_bytes(), bytes0 = t->bytes, t0 = plat_ns();
    gmmu_space_unmap(gva + rs_page(&r, a) * RS_TILE, RS_TILE);
    Surf* now = sv_frame(r.tic);
    u64 t1 = plat_ns(), held1 = rv_allocated();
    ScCount c1 = sc_now();
    vk_lock();
    const u64 reader = vk_last_submit(); /* the copies' */
    vk_submit_progress(&sub, &done);
    const int reading = done < reader;
    vk_surf_use(vk_cmd(), t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    rs_copy(rb, 32, t, xa);
    rs_copy(rb, 48, t, xb);
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE); /* the unbind, then these copies */
    u64 held2 = rv_allocated(), deferred2 = vk_deferred_bytes();
    sc_now();
    const ScBind unbind = g_sc.last;
    vk_submit_progress(&sub, &done);
    const int still = done < reader;
    vk_unlock();
    gate_end(&gate);
    vk_lock();
    vk_finish();
    vk_readback_ready();
    for (u32 i = 0; i < 4; i++) memcpy(got[i], back + 16 * i, n);
    for (int i = 0; i < VK_FRAMES; i++) {
        vk_frame_next();
        vk_cmd();
    }
    u64 held3 = rv_allocated();
    vk_unlock();
    printf("resident texture in flight %s: a tile unmapped under a copy reading it, the release took %.3f ms, %llu "
           "host waits, the GPU %s reading; %llu KiB of %llu freed at once, %llu KiB deferred, %llu KiB freed once the "
           "frames were done\n",
           as->name, (t1 - t0) / 1e6, (unsigned long long)(c1.waits - c0.waits), reading ? "still" : "no longer",
           (unsigned long long)((i64)(held0 - held1) >> 10), (unsigned long long)(bytes0 >> 10),
           (unsigned long long)((deferred2 - deferred0) >> 10), (unsigned long long)((i64)(held0 - held3) >> 10));
    CHECK(now == t && t->bytes == bytes0 - RS_TILE);
    CHECK(reading && still && c1.waits == c0.waits && t1 - t0 < 100000000ull);   /* no wait for the GPU */
    CHECK(held1 == held0 && held2 == held0 && deferred2 - deferred0 == RS_TILE); /* nothing freed under the copy */
    CHECK(held3 + RS_TILE <= held0);                                             /* freed once it is done */
    CHECK(unbind.gfx >= reader && unbind.followed); /* after the copy, and before the copies after it */
    CHECK(!memcmp(got[0], want[0], n) && !memcmp(got[1], want[1], n)); /* the copy read the tile bound */
    CHECK(!memcmp(got[2], zero, n) && !memcmp(got[3], want[1], n));    /* the one after, released */

    /* tile a mapped again from page 2 while the work before waits at a gate */
    gate = (Gate){gate.sem, 3, 0, 0};
    CHECK(!plat_thread(gate_watch, &gate, 1u << 16));
    vk_lock();
    vk_submit_after(gate.sem, 3, 4);
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    const u64 gated = vk_last_submit();
    vk_unlock();
    c0 = sc_now();
    t0 = plat_ns();
    gmmu_map(gva + rs_page(&r, a) * RS_TILE, cpu + 2 * (u64)RS_TILE, RS_TILE);
    now = sv_frame(r.tic);
    t1 = plat_ns();
    c1 = sc_now();
    vk_lock();
    vk_surf_use(vk_cmd(), t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    rs_copy(rb, 64, t, xa);
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    sc_now();
    const ScBind rebind = g_sc.last;
    vk_unlock();
    plat_sleep_ns(20000000); /* the GPU idle at the gate: the bind, the tile's load and the read are what is left */
    gate_end(&gate);
    vk_lock();
    vk_finish();
    vk_readback_ready();
    memcpy(got[4], back + 64, n);
    vk_unlock();
    if (as->astc) { /* the texture made whole over the same pages, for what the tile holds now */
        gmmu_map(ref_gva + rs_page(&r, a) * RS_TILE, cpu + 2 * (u64)RS_TILE, RS_TILE);
        CHECK(sv_frame(ref.tic) == whole);
    }
    CHECK(rs_want(&r, whole, xa, want[2]));
    vkDestroySemaphore(g_vk.dev, gate.sem, NULL);
    gmmu_space_unmap(gva, r.lay.size);
    if (as->astc) gmmu_space_unmap(ref_gva, r.lay.size);
    CHECK(mem_unmap(cpu, 3 * (u64)RS_TILE) == 0);
    printf("resident texture in flight %s, mapped again: %.3f ms, %llu host waits; the read after it "
           "%02x%02x%02x%02x, the texel %02x%02x%02x%02x\n",
           as->name, (t1 - t0) / 1e6, (unsigned long long)(c1.waits - c0.waits), got[4][0], got[4][1], got[4][2],
           got[4][3], want[2][0], want[2][1], want[2][2], want[2][3]);
    CHECK(now == t && t->bytes == bytes0 && c1.waits == c0.waits && t1 - t0 < 100000000ull);
    CHECK(rebind.gfx >= gated && rebind.followed); /* after the work before it, and before the tile's load */
    CHECK(!memcmp(got[4], want[2], n) && memcmp(want[2], want[0], n));
    printf("resident texture in flight %s: a tile unmapped under a copy still reading it, without a host wait, its "
           "memory freed after the GPU; a tile mapped again loaded after the work before it, read at once\n",
           as->name);
    return 0;
}
static int check_resident_texture_in_flight(void) {
    const int had = g_settings.astc_mode;
    CHECK(!resident_in_flight(&RS_RGBA8, 0x960000000ull, 0, HEAP_BASE + 0x1300000));
    g_settings.astc_mode = 0; /* held as BC7 where the GPU samples BCn and not ASTC; rs_device_can skips it elsewhere */
    int failed = resident_in_flight(&RS_ASTC4, 0xA70000000ull, 0xA80000000ull, HEAP_BASE + 0x1300000);
    g_settings.astc_mode = had;
    return failed;
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

/* A copy over more GPU-owned ranges than a read of guest memory notes at once: 20 lines of 64 bytes, 128 apart, each
   its own owner, then one line over them all from byte 32 of the first. Their bytes are copied on the GPU a batch at a
   time and nothing lands; landing them waited for the GPU and read its results back */
static int check_dma_owners(void) {
    GpuDmaOp o = {0}, g, c;
    u64 lands = 0;
    for (int k = 0; k < 2; k++) {
        u8* t = GP(k ? DMA_B : DMA_A);
        for (u32 i = 0; i < DMA_SPAN; i++) t[i] = (u8)(i * 23 + (i >> 9) * 3 + 9);
    }
    o.width = o.line_in = o.line_out = 64;
    o.lines = 1;
    for (u32 i = 0; i <= 20; i++) {
        if (i == 20) { /* over them all */
            o.width = o.line_in = o.line_out = 20 * 128 - 32 - 64;
            o.src = pitch_side(0x4000, 0);
            o.dst = pitch_side(0x80020, 0);
            lands = own_landings();
        } else {
            o.src = pitch_side(0x1000 + i * 64, 0);
            o.dst = pitch_side(0x80000 + i * 128, 0);
        }
        g = c = o;
        g.src.addr += DMA_GA, g.dst.addr += DMA_GA;
        c.src.addr += DMA_GB, c.dst.addr += DMA_GB;
        CHECK(g_renderer->dma_copy(&g));
        dma_copy_ref(&c);
    }
    if (own_traps() && own_landings() != lands) {
        fprintf(stderr, "dma owners: the copy over 20 owners took %llu landings\n",
                (unsigned long long)(own_landings() - lands));
        return 1;
    }
    guest_done(DMA_A, DMA_SPAN);
    CHECK(!memcmp(GP(DMA_A), GP(DMA_B), DMA_SPAN));
    return 0;
}

/* Copies bigger than one allocation (g_vk.alloc_cap, lowered to 64 KiB here: 256 MiB on a real device, an 8K RGBA8
   texture's level 0) go in bands of lines, each within a quarter of the cap, against the CPU reference: pitch to block
   linear from a row inside a row of blocks, block linear to pitch with the source's rows of blocks cut by the bands
   and lines past it reading zero, between block heights 8 and 1 GOBs, the remap, a fill, a kept component, the
   source read at submission, and a slice of a volume. They used to be dropped, the destination left with the bytes
   it had */
static int check_dma_bands(void) {
    const u64 cap = g_vk.alloc_cap;
    u64 copies, bands, bytes, copies0, bands0, bytes0;
    GpuDmaOp o = {0};
    int failed = 0;
    vk_dma_stats(&copies0, &bands0, &bytes0);
    g_vk.alloc_cap = 64 << 10;
    /* pitch to block linear: 300 lines of 256 bytes (80 KB in, 152 KB of rows of blocks out), from row 5 of 16 */
    o.multi = 1;
    o.width = o.line_in = o.line_out = 256;
    o.lines = 300;
    o.src = pitch_side(0x1000, 272);
    o.dst = bl_side(0x40000, 512, 320, 1, 64, 5);
    failed |= dma_case(o) || dma_snap_case(o);
    /* block linear to pitch, from an origin not on 16 bytes; the bands end anywhere in its rows of 32 */
    o.src = bl_side(0x20000, 768, 200, 2, 7, 13);
    o.dst = pitch_side(0x90001, 520);
    o.width = o.line_in = o.line_out = 500;
    o.lines = 220;
    failed |= dma_case(o);
    /* block linear to block linear, blocks 8 GOBs high to 1 */
    o.src = bl_side(0x4000, 512, 256, 3, 32, 5);
    o.dst = bl_side(0xA0000, 384, 240, 0, 9, 22);
    o.width = o.line_in = o.line_out = 300;
    o.lines = 210;
    failed |= dma_case(o);
    /* the remap, B and R swapped, pitch to pitch, and from the source read at submission */
    o = (GpuDmaOp){0};
    o.multi = o.remap = o.shuffle = 1;
    o.cs = 1, o.ns = 4, o.nd = 4;
    o.sel[0] = 2, o.sel[1] = 1, o.sel[2] = 0, o.sel[3] = 3;
    o.width = 64;
    o.line_in = o.line_out = 64 * 4;
    o.lines = 300;
    o.src = pitch_side(0x100, 260);
    o.dst = pitch_side(0x80002, 270);
    failed |= dma_case(o) || dma_snap_case(o);
    /* a fill of two constants into block linear */
    o.fill = 1, o.shuffle = 0;
    o.cs = 2, o.ns = 2, o.nd = 2;
    o.sel[0] = 4, o.sel[1] = 5;
    o.const_a = 0x1234ABCD, o.const_b = 0x5678EF01;
    o.width = 84;
    o.line_in = o.line_out = 84 * 4;
    o.lines = 200;
    o.dst = bl_side(0x80000, 512, 256, 1, 6, 20);
    failed |= dma_case(o);
    /* the destination's own bytes kept in one component of three */
    o.fill = 0, o.shuffle = 1, o.keep = 1;
    o.cs = 1, o.ns = 2, o.nd = 3;
    o.sel[0] = 1, o.sel[1] = 6, o.sel[2] = 0;
    o.width = 150;
    o.line_in = 300, o.line_out = 450;
    o.lines = 160;
    o.src = pitch_side(0x3000, 310);
    o.dst = bl_side(0x80000, 512, 256, 0, 5, 3);
    failed |= dma_case(o);
    /* into slice 3 of a volume, blocks 2 GOBs high and 2 slices deep: the second slab of blocks, its rows of blocks
       (16 KiB, the quarter) one a band */
    o = (GpuDmaOp){0};
    o.multi = 1;
    o.width = o.line_in = o.line_out = 400;
    o.lines = 150;
    o.src = pitch_side(0x1000, 416);
    o.dst = bl_side(0x40000, 512, 160, 1, 48, 5);
    o.dst.bd = 1, o.dst.z = 3;
    failed |= dma_case(o);
    vk_dma_stats(&copies, &bands, &bytes);
    /* a copy that fits goes in one piece */
    o = (GpuDmaOp){0};
    o.width = o.line_in = o.line_out = 1001;
    o.lines = 1;
    o.src = pitch_side(0x100, 0);
    o.dst = pitch_side(0x80003, 0);
    failed |= dma_case(o);
    u64 one_copies, one_bands, one_bytes;
    vk_dma_stats(&one_copies, &one_bands, &one_bytes);
    g_vk.alloc_cap = cap;
    if (failed || copies - copies0 != 9 || bands - bands0 < 9 * 2 || one_copies != copies + 1 || one_bands != bands)
        fprintf(stderr, "dma bands: failed %d, %llu copies in %llu bands, then %llu copy in %llu bands\n", failed,
                (unsigned long long)(copies - copies0), (unsigned long long)(bands - bands0),
                (unsigned long long)(one_copies - copies), (unsigned long long)(one_bands - bands));
    CHECK(!failed && copies - copies0 == 9 && bands - bands0 >= 9 * 2);
    CHECK(one_copies == copies + 1 && one_bands == bands && one_bytes > bytes);
    return 0;
}

/* Targets bigger than one allocation (g_vk.alloc_cap, lowered to 64 KiB for the write-back) read back in parts: a
   block linear 256x128 RGBA8 one and a pitch linear one of the same size in bands of rows, a pitch linear array of
   12 layers of 64x32 two layers a part, and multisampled ones, block linear of 256x128 samples and pitch linear of
   192x128 (bands of 21 rows: a part ends between a pixel's two rows of samples), each part's samples unpacked alone:
   every sample once, into an image no bigger than the part (an image past the cap goes in pieces, which lavapipe draws
   into in another layout than it copies from). Each is loaded from guest memory holding a pattern and drawn red only
   (a masked clear), so memory gets red 128 in every pixel's first byte and keeps the pattern in the others, each where
   the guest's layout has it. They used to stay on the GPU, memory unchanged */
static int flush_parts_case(u64 gva, u64 cpu, u32 width_reg, u32 h, u32 layout, u32 layers, u32 layer_bytes, u32 msaa) {
    GpuChannel* ch = calloc(1, sizeof *ch);
    const u64 cap = g_vk.alloc_cap, size = (u64)layer_bytes * layers;
    CHECK(ch);
    u32* r = ch->r3d;
    u8* got = (u8*)malloc(size);
    CHECK(got);
    for (u64 i = 0; i < size; i++) /* below 64 in the red bytes */
        GP(cpu)[i] = (u8)(i % 4 ? i * 7 + (i >> 10) * 3 + 1 : (i * 5 + (i >> 9)) & 0x3F);
    r[0x574] = msaa;
    r[0x487] = 1;
    r[0x200] = (u32)(gva >> 32);
    r[0x201] = (u32)gva;
    r[0x202] = width_reg;
    r[0x203] = h;
    r[0x204] = 0xD5; /* A8B8G8R8 */
    r[0x205] = layout;
    r[0x206] = layers;
    r[0x207] = layer_bytes >> 2;
    float red[4] = {0.5f, 0, 0, 0};
    memcpy(&r[0x360], red, sizeof red);
    for (u32 l = 0; l < layers; l++) engine_clear(ch, (1 << 2) | (l << 10));
    vk_render_end();
    const Surf* s = vk_rt_find(gva);
    CHECK(s && s->msaa == msaa);
    u64 samples = msaa ? (u64)(s->w << ds_msaa_log2_x(msaa)) * (s->h << ds_msaa_log2_y(msaa)) * layers : 0;
    u64 unpacked = vk_ms_unpacked();
    g_vk.alloc_cap = 64 << 10;
    vk_rt_flush(gva);
    g_vk.alloc_cap = cap;
    unpacked = vk_ms_unpacked() - unpacked;
    CHECK(!gmem_read(gva, got, size));
    for (u64 i = 0; i < size; i++) {
        u8 want = (u8)(i % 4 ? i * 7 + (i >> 10) * 3 + 1 : 128);
        if (i % 4 ? got[i] != want : abs((int)got[i] - 128) > 1) {
            fprintf(stderr, "write-back in parts of layout %#x, %u layers, msaa %u: byte %llu is %u, not %u\n", layout,
                    layers, msaa, (unsigned long long)i, got[i], want);
            free(got);
            free(ch);
            return 1;
        }
    }
    free(got);
    free(ch);
    if (unpacked != samples) {
        fprintf(stderr, "write-back in parts of layout %#x, %u layers, msaa %u: %llu samples unpacked, not %llu\n",
                layout, layers, msaa, (unsigned long long)unpacked, (unsigned long long)samples);
        return 1;
    }
    return 0;
}
static int check_flush_parts(void) {
    const u64 cpu = HEAP_BASE + 0xC00000, gva = 0xB000000ull;
    CHECK(mem_map(cpu, 0x100000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gva, cpu, 0x100000);
    CHECK(!flush_parts_case(gva, cpu, 256, 128, 0, 1, 256 * 128 * 4, 0)); /* block linear, 1 GOB high */
    CHECK(!flush_parts_case(gva + 0x40000, cpu + 0x40000, 1024, 128, 0x1000, 1, 1024 * 128, 0)); /* pitch linear */
    CHECK(!flush_parts_case(gva + 0x80000, cpu + 0x80000, 256, 32, 0x1000, 12, 256 * 32, 0));    /* 12 layers */
    /* 2x2 samples a pixel (registers in samples): each band of rows' samples drawn into the guest's layout of them */
    CHECK(!flush_parts_case(gva + 0xC0000, cpu + 0xC0000, 256, 128, 0, 1, 256 * 128 * 4, DS_MSAA_2X2));
    CHECK(!flush_parts_case(gva + 0xE0000, cpu + 0xE0000, 768, 128, 0x1000, 1, 768 * 128, DS_MSAA_2X2));
    CHECK(mem_unmap(cpu, 0x100000) == 0);
    return 0;
}

/* A VIC job bigger than one buffer (g_vk.alloc_cap, lowered to 64 KiB here) composed in bands of the target's rows,
   each staging the rows of every plane its rows read, against the CPU reference (vic_ref.c), on the DMA test's twin
   regions: block linear NV12 in (blocks of 2 GOBs), two slots over a background, the later one through the matrix,
   into RGBA pitch linear rows whose pitch isn't a multiple of 4 (the shader's unaligned build), from an odd row with
   a slot from an odd row; and into NV12 block linear, luma blocks of 1 GOB and chroma of 2 (bands of 32 rows, more
   than a quarter of the cap each). The twins are compared whole: the planes' other bytes keep theirs */
static int vic_bands_case(int nv12) {
    enum { W = 256, H = 192 };
    static const int m601[3][4] = {{596, 0, 818, -(298 * 64 + 409 * 512)},
                                   {596, -200, -416, -(298 * 64) + (100 + 208) * 512},
                                   {596, 1032, 0, -(298 * 64 + 516 * 512)}};
    static const u32 bg[4] = {1023, 512, 0, 1023};
    for (int k = 0; k < 2; k++) {
        u8* t = GP(k ? DMA_B : DMA_A);
        VicJob j = {0};
        for (u32 i = 0; i < DMA_SPAN; i++) t[i] = (u8)(i * 13 + (i >> 9) * 7 + 5);
        j.fmt = nv12 ? 0x44 : 0x1F;
        j.sw = W, j.sh = H;
        j.tw = W, j.ty0 = nv12 ? 2 : 1, j.th = nv12 ? 188 : 190;
        memcpy(j.bg, bg, sizeof j.bg);
        if (nv12) {
            j.oy = (VicPlane){t + 0x60000, 4 * 24 * 512, W, H, 1, 0};
            j.oc = (VicPlane){t + 0x70000, 4 * 6 * 1024, W, H / 2, 1, 1};
        } else
            j.oy = (VicPlane){t + 0x20000, 1042ull * H, 1042, H, 0, 0};
        for (u32 s = 0; s < 2; s++) {
            VicSlot* sl = &j.slots[s];
            sl->on = 1;
            sl->alpha = 0x3FF, sl->clamp_lo = 64, sl->clamp_hi = 0x3FF - 64;
            sl->mat_shift = 1;
            memcpy(sl->mat, m601, sizeof m601);
            sl->iy = (VicPlane){t, 4 * 12 * 1024, W, H, 1, 1};
            sl->ic = (VicPlane){t + 0x10000, 4 * 6 * 1024, W, H / 2, 1, 1};
        }
        j.slots[0].dx0 = 4, j.slots[0].dy0 = 2, j.slots[0].w = 240, j.slots[0].h = 180;
        j.slots[1].sx0 = 10, j.slots[1].sy0 = nv12 ? 6 : 5, j.slots[1].dx0 = 40, j.slots[1].dy0 = nv12 ? 34 : 33;
        j.slots[1].w = 150, j.slots[1].h = 120, j.slots[1].mat_enable = 1;
        if (k)
            vic_compose_ref(&j);
        else if (!g_renderer->vic_compose(&j)) {
            fprintf(stderr, "vic bands: the %s job was refused\n", nv12 ? "NV12" : "RGBA");
            return 1;
        }
    }
    guest_done(DMA_A, DMA_SPAN);
    if (memcmp(GP(DMA_A), GP(DMA_B), DMA_SPAN)) {
        u32 i = 0;
        while (GP(DMA_A)[i] == GP(DMA_B)[i]) i++;
        fprintf(stderr, "vic bands: the %s job's byte %#x is %u, the reference's %u\n", nv12 ? "NV12" : "RGBA", i,
                GP(DMA_A)[i], GP(DMA_B)[i]);
        return 1;
    }
    return 0;
}
static int check_vic_bands(void) {
    const u64 cap = g_vk.alloc_cap;
    int failed;
    g_vk.alloc_cap = 64 << 10;
    failed = vic_bands_case(0) || vic_bands_case(1);
    g_vk.alloc_cap = cap;
    CHECK(!failed);
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
    guest_done(DMA_A, DMA_SPAN);
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
    CHECK(!blit_case(bl_side(0x1000, 128 * 4, 64, 2, 0, 0), bl_side(0x80000, 64 * 4, 32, 1, 0, 0), 4, 5, 3, 21, 9, 42,
                     18, 0));
    /* pitch scaled up into block linear, 2 bytes a pixel */
    CHECK(!blit_case(pitch_plane(0x3000, 70, 31 * 2, 12), bl_side(0x80000, 100 * 2, 40, 0, 0, 0), 2, 7, 1, 90, 33, 31,
                     12, 0));
    /* block linear into pitch, 16 bytes a pixel, a source whose rows end inside a 16 byte run */
    CHECK(!blit_case(bl_side(0x1000, 37 * 16, 24, 1, 0, 0), pitch_plane(0x90003, 40 * 16, 40 * 16, 24), 16, 2, 2, 30,
                     20, 37, 24, 0));
    /* pitch to pitch, a byte a pixel */
    CHECK(!blit_case(pitch_plane(0x101, 77, 77, 30), pitch_plane(0x90001, 99, 99, 15), 1, 3, 4, 95, 11, 77, 30, 0));
    /* 8 bytes a pixel, block linear 3 GOBs a block high */
    CHECK(!blit_case(bl_side(0x2000, 50 * 8, 48, 3, 0, 0), bl_side(0xA0000, 40 * 8, 20, 1, 0, 0), 8, 1, 0, 39, 20, 50,
                     48, 0));
    /* RGBA32F pitch into RGBA16F block linear, an odd window scaled down */
    CHECK(!blit_case(pitch_plane(0x3000, 40 * 16, 40 * 16, 20), bl_side(0x80000, 64 * 8, 32, 1, 0, 0), 16, 5, 3, 21, 9,
                     40, 20, GPU_BLIT_FLOAT(4, 4, 4, 2, 0)));
    /* R16F block linear into RGBA32F pitch: green and blue 0, alpha 1 */
    CHECK(!blit_case(bl_side(0x1000, 33 * 2, 24, 1, 0, 0), pitch_plane(0x90000, 40 * 16, 40 * 16, 24), 2, 2, 1, 37, 20,
                     33, 24, GPU_BLIT_FLOAT(1, 2, 4, 4, 0)));
    /* RGBX16F into RG32F, and R32F into R16F an odd number of pixels wide (half words left at the rows' ends) */
    CHECK(!blit_case(bl_side(0x1000, 30 * 8, 16, 0, 0, 0), bl_side(0xA0000, 50 * 8, 20, 1, 0, 0), 8, 1, 2, 45, 17, 30,
                     16, GPU_BLIT_FLOAT(4, 2, 2, 4, 1)));
    CHECK(!blit_case(pitch_plane(0x101, 77 * 4, 77 * 4, 30), pitch_plane(0x90002, 99 * 2, 99 * 2, 15), 4, 3, 4, 95, 11,
                     77, 30, GPU_BLIT_FLOAT(1, 4, 1, 2, 0)));
    return 0;
}

/* Blits bigger than one allocation (g_vk.alloc_cap, lowered to 64 KiB here) go in bands of the rectangle's rows,
   each reading the source rows its rows sample, against the CPU reference: block linear scaled down into block
   linear, bands of 16 rows sampling the source's rows of blocks of 32 anywhere in them; pitch linear scaled up into
   pitch linear; RGBA32F pitch linear into RGBA16F block linear from a row inside a row of blocks, a row of blocks
   more than a quarter of the cap. They used to be refused, the destination left with the bytes it had */
static int check_blit_bands(void) {
    const u64 cap = g_vk.alloc_cap;
    int failed = 0;
    g_vk.alloc_cap = 64 << 10;
    failed |= blit_case(bl_side(0x1000, 128 * 4, 256, 2, 0, 0), bl_side(0x80000, 128 * 4, 256, 1, 0, 0), 4, 9, 30, 100,
                        200, 128, 256, 0);
    failed |= blit_case(pitch_plane(0x101, 300, 75 * 4, 150), pitch_plane(0x60001, 520, 120 * 4, 240), 4, 3, 2, 110,
                        230, 75, 150, 0);
    failed |= blit_case(pitch_plane(0x3000, 64 * 16, 64 * 16, 100), bl_side(0x80000, 64 * 8, 128, 1, 0, 0), 16, 0, 5,
                        64, 120, 64, 100, GPU_BLIT_FLOAT(4, 4, 4, 2, 0));
    g_vk.alloc_cap = cap;
    CHECK(!failed);
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
    vk_lock();
    vk_render_end();
    vk_unlock();
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
/* A copy's source written where the write tracking can't see (through the alias pool's file, as a save state's restore
   writes): the next copy, in a later submission, takes the new bytes. The source is big enough for the buffer cache to
   keep a buffer of its own, which the tracking would vouch for (one game's upload ring was written past the tracking,
   and its lightmap atlas copied with stale bytes) */
static int check_dma_untracked_source(void) {
    const u64 size = 0x40000, src = HEAP_BASE + 0x800000, dst = src + size, gsrc = 0x380000000ull,
              gdst = 0x390000000ull;
    GpuDmaOp o;
    MemBlock b;
    u8* bytes;
    CHECK(mem_map(src, 2 * size, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gsrc, src, size);
    gmmu_map(gdst, dst, size);
    vk_lock();
    int tracking = vk_written_since(gmem_host(gsrc, size), size, 0) >= 0;
    vk_unlock();
    if (!tracking || !mem_pool_active() || mem_alias_begin(src, size)) {
        printf("skip: an untracked copy source needs write tracking and the alias pool\n");
        return 0;
    }
    memset(&o, 0, sizeof o);
    o.width = o.line_in = o.line_out = (u32)size;
    o.lines = 1;
    o.src = pitch_side(gsrc, 0);
    o.dst = pitch_side(gdst, 0);
    guest_done(src, size); /* the GPU's writes there first, as the guest's fence has them */
    memset(GP(src), 0x11, size);
    g_renderer->mem_may_have_changed();
    CHECK(g_renderer->dma_copy(&o));
    guest_done(dst, (u32)size);
    CHECK(all_bytes(GP(dst), (u32)size, 0x11));
    /* the same copy again: the tracking reports the first fill and protects its pages, the baseline the third copy's
       check counts from */
    g_renderer->mem_may_have_changed();
    CHECK(g_renderer->dma_copy(&o));
    guest_done(dst, (u32)size);
    CHECK(all_bytes(GP(dst), (u32)size, 0x11));
    mem_query(src, &b);
    CHECK(b.pool && (bytes = malloc(size)));
    memset(bytes, 0x22, size);
    CHECK(mem_restore_write(b.pool - 1 + (src - b.addr), bytes, size) == 0 && GP(src)[size - 1] == 0x22);
    free(bytes);
    g_renderer->mem_may_have_changed();
    CHECK(g_renderer->dma_copy(&o));
    if (!all_bytes(GP(dst), (u32)size, 0x22))
        fprintf(stderr, "untracked copy source: the copy took %#x, the source holds 0x22\n", GP(dst)[0]);
    guest_done(dst, (u32)size);
    CHECK(all_bytes(GP(dst), (u32)size, 0x22));
    mem_alias_end(src, size);
    puts("dma: a source written past the write tracking is copied as it is now");
    return 0;
}

/* A job's checks see the CPU's writes from before it began (vk_ww_stream), not those a later submission makes while it
   runs: those are the later job's to see. The program writes a range both jobs read (a big buffer bound whole, each
   job reading its own part) and submits again while the first runs, which checks the range after the write: the
   second must take the new bytes. Stamped with the newest epoch, the first job's check vouched for writes it never
   looked for, and the second reused its copy unchecked (one game's compute inputs, with the tracking's own scans
   saying nothing was written since the copy) */
static u32 g_later_done;
static u64 g_later_cpu, g_later_size;
static void later_submission(void* arg) { /* the program's thread: its writes, then its submission */
    (void)arg;
    memset(GP(g_later_cpu), 0x22, g_later_size);
    g_renderer->mem_may_have_changed();
    __atomic_store_n(&g_later_done, 1, __ATOMIC_RELEASE);
    plat_wake_word(&g_later_done);
}
/* whether the strict buffer of [gva, gva + size) holds want in every byte, read back from the GPU into *got (vk_lock
   held) */
static int strict_bytes(u64 gva, u64 size, u8 want, u8* got) {
    VkBuffer b, rb;
    u64 off;
    u8* result;
    if (!vk_buf_get_strict(gva, size, &b, &off) || !(result = vk_readback_buffer(size, &rb))) return 0;
    VkBufferCopy c = {off, 0, size};
    vk_render_end();
    rec_CmdCopyBuffer(vk_cmd(), b, rb, 1, &c);
    vk_finish();
    vk_readback_ready();
    *got = result[0];
    return all_bytes(result, (u32)size, want);
}
static int check_stream_epoch(void) {
    const u64 size = 0x40000, cpu = HEAP_BASE + 0x1800000, gva = 0x3a0000000ull;
    VkBuffer b;
    u64 off;
    u8 got = 0;
    CHECK(mem_map(cpu, size, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gva, cpu, size);
    memset(GP(cpu), 0x11, size);
    g_renderer->mem_may_have_changed();
    vk_lock();
    int tracking = vk_written_since(gmem_host(gva, size), size, 0) >= 0;
    /* big enough for a buffer of its own, which the tracking vouches for */
    CHECK(vk_buf_get_strict(gva, size, &b, &off));
    vk_unlock();
    if (!tracking) {
        printf("skip: a job's write epoch needs write tracking\n");
        CHECK(mem_unmap(cpu, size) == 0);
        return 0;
    }
    /* the first job, in a new epoch, checks the range: the epoch's scan of it is taken in */
    g_renderer->mem_may_have_changed();
    g_renderer->stream_run(1);
    vk_lock();
    CHECK(strict_bytes(gva, size, 0x11, &got));
    vk_unlock();
    /* the program writes it and submits the second job, while the first runs */
    g_later_cpu = cpu;
    g_later_size = size;
    __atomic_store_n(&g_later_done, 0, __ATOMIC_RELAXED);
    CHECK(plat_thread(later_submission, NULL, 0) == 0);
    while (!__atomic_load_n(&g_later_done, __ATOMIC_ACQUIRE)) plat_wait_word(&g_later_done, 0, -1);
    /* the first job reads the range again */
    vk_lock();
    CHECK(vk_buf_get_strict(gva, size, &b, &off));
    vk_unlock();
    g_renderer->stream_run(0);
    /* the second job, which the writes are for */
    g_renderer->stream_run(1);
    vk_lock();
    int fresh = strict_bytes(gva, size, 0x22, &got);
    vk_unlock();
    g_renderer->stream_run(0);
    if (!fresh)
        fprintf(stderr, "stream epoch: the second job read %#x, the program wrote 0x22 before its submission\n", got);
    CHECK(fresh);
    CHECK(mem_unmap(cpu, size) == 0);
    puts("stream epoch: a later job takes the writes made for it while an earlier one ran");
    return 0;
}

/* A copy into a block linear surface moves its tiles only, the blocks of its lines' GOB columns in each row of blocks:
   the rest of the surface keeps its bytes and their owner. vk_dma_stats counts what each copy reads and writes. On
   the DMA test's twins, against the CPU reference, ops one after another from the same bytes; alias[i]: op i writes
   through a second GPU address of the GPU's twin. From op quiet on nothing lands: the GPU's results the copies read
   are copied on the GPU, however many owners they have */
#define DMA_GA2 0x260000000ull
static int dma_tiles_case(const char* what, const GpuDmaOp* ops, const int* alias, u32 n, u64 want, u32 quiet) {
    u64 c0, b0, m0, c1, b1, m1, lands = 0;
    for (int k = 0; k < 2; k++) {
        u8* t = GP(k ? DMA_B : DMA_A);
        for (u32 i = 0; i < DMA_SPAN; i++) t[i] = (u8)(i * 37 + (i >> 9) * 11 + 7);
    }
    vk_dma_stats(&c0, &b0, &m0);
    for (u32 i = 0; i < n; i++) {
        GpuDmaOp g = ops[i], c = ops[i];
        if (i == quiet) lands = own_landings();
        g.src.addr += DMA_GA, g.dst.addr += alias && alias[i] ? DMA_GA2 : DMA_GA;
        c.src.addr += DMA_GB, c.dst.addr += DMA_GB;
        CHECK(g_renderer->dma_copy(&g));
        dma_copy_ref(&c);
    }
    vk_dma_stats(&c1, &b1, &m1);
    if (quiet < n && own_landings() != lands) {
        fprintf(stderr, "dma tiles, %s: %llu landings from copy %u on\n", what,
                (unsigned long long)(own_landings() - lands), quiet);
        return 1;
    }
    guest_done(DMA_A, DMA_SPAN);
    if (memcmp(GP(DMA_A), GP(DMA_B), DMA_SPAN) || m1 - m0 != want) {
        u32 i = 0;
        while (i < DMA_SPAN && GP(DMA_A)[i] == GP(DMA_B)[i]) i++;
        fprintf(stderr, "dma tiles, %s: %llu bytes moved, %llu meant; first byte apart %#x of %#x\n", what,
                (unsigned long long)(m1 - m0), (unsigned long long)want, i, DMA_SPAN);
        return 1;
    }
    return 0;
}
/* A block linear source copied again and again from the buffer cache's copy of it, in later submissions: unchanged, it
   is neither read again on the CPU nor compared, the write tracking vouching for it (vk_buf_copy_src_read); written
   by the CPU, the next copy takes the new bytes. Only its tiles are moved. On twins of its own, which no engine wrote
   (pages an engine's result was published over become pool pages, compared by contents) */
static int dma_reused_source(void) {
    const u64 cpu = HEAP_BASE + 0x2400000, ga = 0x270000000ull, gb = 0x280000000ull, span = 0x100000;
    GpuDmaOp o = {0};
    u64 c0, b0, m0, c1, b1, m1, read[4];
    CHECK(mem_map(cpu, 2 * span, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(ga, cpu, span);
    gmmu_map(gb, cpu + span, span);
    vk_lock();
    int tracked = vk_written_since(gmem_host(ga, 0x80000), 0x80000, 0) >= 0 && !mem_pool_any(cpu, 0x80000);
    vk_unlock();
    for (int k = 0; k < 2; k++)
        for (u32 i = 0; i < span; i++) GP(cpu + k * span)[i] = (u8)(i * 29 + (i >> 9) * 7 + 3);
    /* 64 lines of 256 bytes from (520, 100) of a 2048 byte wide surface of blocks 16 GOBs high: rows of blocks 0 and
       1, GOB columns 8 to 12 */
    o.multi = 1;
    o.width = o.line_in = o.line_out = 256;
    o.lines = 64;
    o.src = bl_side(0, 2048, 256, 4, 520, 100);
    o.dst = pitch_side(0x80000, 256);
    vk_dma_stats(&c0, &b0, &m0);
    for (int pass = 0; pass < 4; pass++) {
        GpuDmaOp g = o, c = o;
        g.src.addr += ga, g.dst.addr += ga;
        c.src.addr += gb, c.dst.addr += gb;
        if (pass == 3) /* the CPU writes the source's first row of blocks, the copy's lines in it among them */
            for (int k = 0; k < 2; k++)
                for (u32 i = 0; i < 0x40000; i++) GP(cpu + k * span)[i] ^= 0x5A;
        g_renderer->mem_may_have_changed(); /* a later submission */
        CHECK(g_renderer->dma_copy(&g));
        dma_copy_ref(&c);
        read[pass] = vk_buf_copy_src_read();
        guest_done(cpu, span);
        if (memcmp(GP(cpu), GP(cpu + span), span)) {
            fprintf(stderr, "dma tiles, reused source: copy %d differs from the reference\n", pass);
            return 1;
        }
    }
    vk_dma_stats(&c1, &b1, &m1);
    /* each copy: the source's 2 x 5 blocks of 8 KiB, the destination's 64 lines of 256 bytes */
    if (m1 - m0 != 4 * (2 * 5 * 8192 + 64 * 256) || (tracked && (read[1] != read[0] || read[2] != read[0]))) {
        fprintf(stderr, "dma tiles, reused source: %llu bytes moved, %u meant; read on the CPU %llu, %llu, %llu\n",
                (unsigned long long)(m1 - m0), 4 * (2 * 5 * 8192 + 64 * 256), (unsigned long long)read[0],
                (unsigned long long)read[1], (unsigned long long)read[2]);
        return 1;
    }
    if (!tracked)
        printf("skip: a reused copy source is compared by contents here (no write tracking, or pool pages)\n");
    CHECK(mem_unmap(cpu, 2 * span) == 0);
    return 0;
}
static int check_dma_tiles(void) {
    GpuDmaOp o[3] = {{0}};
    int alias[3] = {0, 1, 0};
    gmmu_map(DMA_GA2, DMA_A, DMA_SPAN);
    /* 64 lines of 256 bytes into (520, 100) of a 2048 byte wide surface of blocks 16 GOBs high (8 KiB): its rows of
       blocks 0 and 1, GOB columns 8 to 12, 80 KiB of its 512 KiB; the source 63 lines of 272 bytes and one of 256 */
    o[0].multi = 1;
    o[0].width = o[0].line_in = o[0].line_out = 256;
    o[0].lines = 64;
    o[0].src = pitch_side(0x1000, 272);
    o[0].dst = bl_side(0x80000, 2048, 256, 4, 520, 100);
    CHECK(!dma_tiles_case("a small copy into a big surface", o, NULL, 1, 272 * 63 + 256 + 2 * 5 * 8192, 1));
    /* then 48 lines of 384 bytes into (600, 120), over part of the first's: GOB columns 9 to 15, both rows of blocks */
    o[1] = o[0];
    o[1].width = o[1].line_in = o[1].line_out = 384;
    o[1].lines = 48;
    o[1].src = pitch_side(0x8000, 400);
    o[1].dst.ox = 600, o[1].dst.oy = 120;
    CHECK(!dma_tiles_case("two copies over one destination in order", o, NULL, 2,
                          272 * 63 + 256 + 2 * 5 * 8192 + 400 * 47 + 384 + 2 * 7 * 8192, 1));
    /* the second through another GPU address of the same memory, and a third within the surface: 64 lines of 200
       bytes from (0, 0), the first's tiles beside them, to (1500, 140), GOB columns 23 to 26 of row of blocks 1 */
    o[2] = o[0];
    o[2].width = o[2].line_in = o[2].line_out = 200;
    o[2].src = bl_side(0x80000, 2048, 256, 4, 0, 0);
    o[2].dst.ox = 1500, o[2].dst.oy = 140;
    CHECK(!dma_tiles_case("an alias of the destination and a copy within it", o, alias, 3,
                          272 * 63 + 256 + 2 * 5 * 8192 + 400 * 47 + 384 + 2 * 7 * 8192 + 4 * 8192 + 4 * 8192, 1));
    /* 20 copies of 16 lines of 32 bytes, 96 bytes apart, into row of blocks 0 (GOB columns 0, 1, 3, 4 ... 28), each
       its owner, then 128 lines of 1984 bytes from (32, 0) over all of them: the owners' bytes it keeps are copied on
       the GPU, 16 owners at a time */
    GpuDmaOp many[21];
    for (u32 i = 0; i < 20; i++) {
        many[i] = (GpuDmaOp){0};
        many[i].multi = 1;
        many[i].width = many[i].line_in = many[i].line_out = 32;
        many[i].lines = 16;
        many[i].src = pitch_side(0x50000 + i * 0x200, 32);
        many[i].dst = bl_side(0x80000, 2048, 256, 4, i * 96, 0);
    }
    many[20] = many[0];
    many[20].width = many[20].line_in = many[20].line_out = 1984;
    many[20].lines = 128;
    many[20].src = pitch_side(0x1000, 1984);
    many[20].dst.ox = 32;
    CHECK(!dma_tiles_case("a copy over 20 owners", many, NULL, 21,
                          20 * (15 * 32 + 32 + 8192) + 127 * 1984 + 1984 + 32 * 8192, own_traps() ? 20 : 21));
    CHECK(!dma_reused_source());
    puts("dma tiles: a copy moves its tiles only, in order, through aliases, and a reused source is not read again");
    return 0;
}

/* Render targets over one another in the DMA test's region, as when a program frees an image and makes a bigger one
   there: a red 64 by 64 target 64 KiB into a blue 160 by 128 one cleared after it is its block columns 8 and 9 (pixels
   128 to 159 of every row). The newer one's pixels are on top in a DMA copy of it, and in guest memory once both are
   written back, together or the newer one alone. A target reaching past a copy's range is one whose memory a smaller
   surface took over: the copy takes what the program wrote there since */
/* A texture whose first level a render target holds, with a level of its own after it, takes the target's pixels in
   again after a clear and after a 2D blit onto the target: each moves the count tex_take_drawn passes over a texture
   by (vk_rt_take_gen), which a bind with nothing changed leaves alone */
static int check_take_after_writes(void) {
    const u64 gva = 0x4C0000000ull, cpu = HEAP_BASE + 0x500000, src = gva + 0x4000;
    const u32 rgba8 = 0x08 | 2u << 7 | 2u << 10 | 2u << 13 | 2u << 16;
    /* 16x16, blocks one GOB high, levels 0 and 1: level 0 is the target's 1 KiB */
    const u32 tic[8] = {rgba8, (u32)gva, (u32)(gva >> 32) | 3u << 21, 1u << 28, 15 | 1u << 23, 15, 0, 1u << 4};
    static const u8 red[4] = {255, 0, 0, 255}, blue[4] = {0, 0, 255, 255};
    GpuChannel* ch = calloc(1, sizeof *ch);
    u8 px[4];
    CHECK(ch);
    gmmu_map(gva, cpu, 0x10000);
    rt_clear(ch, gva, 16, 16, 0, 0);
    vk_lock();
    Surf* t = vk_tex_from_tic(tic);
    CHECK(t && !t->is_rt && t->levels == 2);
    CHECK(!read_pixel0(t, px) && !memcmp(px, red, 4));
    u64 gen = vk_rt_take_gen();
    CHECK(vk_tex_from_tic(tic) == t && vk_rt_take_gen() == gen); /* nothing changed: passed over */
    vk_unlock();
    rt_clear(ch, gva, 16, 16, 0, 1); /* a clear */
    CHECK(vk_rt_take_gen() != gen);
    vk_lock();
    CHECK(vk_tex_from_tic(tic) == t && !read_pixel0(t, px) && !memcmp(px, blue, 4));
    vk_unlock();
    rt_clear(ch, src, 16, 16, 0, 0); /* a 2D blit onto the target from a red one */
    Surf2D from = {.format = 0xD5, .depth = 1, .width = 16, .height = 16, .addr = src}, onto = from;
    onto.addr = gva;
    gen = vk_rt_take_gen();
    CHECK(g_renderer->blit(&from, &onto, 0, 0, 16, 16, 0, 0, 1, 1));
    CHECK(vk_rt_take_gen() != gen);
    vk_lock();
    CHECK(vk_tex_from_tic(tic) == t && !read_pixel0(t, px) && !memcmp(px, red, 4));
    vk_unlock();
    puts("drawn targets into textures: taken again after a clear and a blit onto the target, passed over between");
    free(ch);
    return 0;
}
/* A clear whose targets' group (DsState.gen[DS_TARGETS]) and the surface table are as the last draw's or clear's keeps
   its targets (read_targets, g_vk_targets_kept) and clears them; another target, or the table changed under the same
   registers (a texture made elsewhere), looks them up again */
static int check_targets_kept(void) {
    const u64 gva = 0x4C0020000ull, other = gva + 0x4000, cpu = HEAP_BASE + 0x520000;
    const u32 rgba8 = 0x08 | 2u << 7 | 2u << 10 | 2u << 13 | 2u << 16;
    const u32 tic[8] = {rgba8, (u32)(gva + 0x8000), (u32)((gva + 0x8000) >> 32) | 3u << 21, 0, 15 | 1u << 23, 15, 0, 0};
    static const u8 red[4] = {255, 0, 0, 255}, blue[4] = {0, 0, 255, 255};
    GpuChannel* ch = calloc(1, sizeof *ch);
    u8 px[4];
    CHECK(ch);
    gmmu_map(gva, cpu, 0x10000);
    rt_clear(ch, gva, 16, 16, 0, 0);
    u64 kept = g_vk_targets_kept;
    rt_clear(ch, gva, 16, 16, 0, 1); /* the same registers: kept */
    CHECK(g_vk_targets_kept == kept + 1);
    vk_lock();
    CHECK(!read_pixel0(vk_rt_find(gva), px) && !memcmp(px, blue, 4));
    vk_unlock();
    rt_clear(ch, other, 16, 16, 0, 0); /* another target: looked up */
    CHECK(g_vk_targets_kept == kept + 1);
    rt_clear(ch, gva, 16, 16, 0, 0); /* back: its group's bytes changed again, looked up */
    CHECK(g_vk_targets_kept == kept + 1);
    vk_lock();
    CHECK(vk_tex_from_tic(tic)); /* a surface entering the table */
    vk_unlock();
    rt_clear(ch, gva, 16, 16, 0, 1); /* the same registers over a changed table: looked up */
    CHECK(g_vk_targets_kept == kept + 1);
    rt_clear(ch, gva, 16, 16, 0, 0);
    CHECK(g_vk_targets_kept == kept + 2);
    vk_lock();
    CHECK(!read_pixel0(vk_rt_find(gva), px) && !memcmp(px, red, 4));
    CHECK(!read_pixel0(vk_rt_find(other), px) && !memcmp(px, red, 4));
    vk_unlock();
    puts("render targets kept: the last clear's set over the same table, looked up again after either changed");
    free(ch);
    return 0;
}
static int check_rt_overlap(void) {
    GpuChannel* ch = calloc(1, sizeof *ch);
    GpuDmaOp o;
    u64 readbacks;
    u8 px[4];
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
    guest_done(DMA_A + 0x40000, 160 * 128 * 4);
    CHECK(all_blue(GP(DMA_A + 0x40000), 160 * 128 * 4)); /* reading it lands it */
    vk_rt_before_write(DMA_GA, 0x14000);
    guest_done(DMA_A, 0x14000);
    CHECK(all_blue(GP(DMA_A), 0x14000));
    /* the newer one written back alone: the older one first, under it, and not again after */
    rt_clear(ch, DMA_GA + 0x10000, 64, 64, 3, 0);
    rt_clear(ch, DMA_GA, 160, 128, 4, 1);
    g_renderer->rt_flush(DMA_GA);
    vk_rt_before_write(DMA_GA, 0x14000);
    guest_done(DMA_A, 0x14000);
    CHECK(all_blue(GP(DMA_A), 0x14000));
    /* the older one written back alone: under the newer one, which keeps its pixels in a copy of it and goes on top
       when it is written back */
    rt_clear(ch, DMA_GA + 0x10000, 64, 64, 3, 0);
    rt_clear(ch, DMA_GA, 160, 128, 4, 1);
    g_renderer->rt_flush(DMA_GA + 0x10000);
    guest_done(DMA_A + 0x10000, 4);
    CHECK(GP(DMA_A + 0x10000)[0] == 255 && GP(DMA_A + 0x10000)[2] == 0); /* it landed */
    vk_lock();
    Surf* newer = vk_rt_find(DMA_GA);
    CHECK(newer);
    vk_rt_note_read(newer); /* a use of it, as a texture bind asks */
    CHECK(!read_pixel_at(newer, 140, 10, px) && px[0] == 0 && px[2] == 255);
    vk_unlock();
    CHECK(g_renderer->dma_copy(&o));
    guest_done(DMA_A + 0x40000, 160 * 128 * 4);
    CHECK(all_blue(GP(DMA_A + 0x40000), 160 * 128 * 4));
    vk_rt_before_write(DMA_GA, 0x14000);
    guest_done(DMA_A, 0x14000);
    CHECK(all_blue(GP(DMA_A), 0x14000));
    /* a 160 by 256 target, the program's own bytes over its memory after it, and a copy of the 160 by 128 surface */
    rt_clear(ch, DMA_GA, 160, 256, 4, 1);
    guest_done(DMA_A, 0x28000); /* the GPU's writes there first, as the guest's fence has them */
    memset(GP(DMA_A), 0x11, 0x28000);
    readbacks = g_vk_rt_readbacks;
    CHECK(g_renderer->dma_copy(&o));
    CHECK(g_vk_rt_readbacks == readbacks + 1);
    guest_done(DMA_A + 0x40000, 160 * 128 * 4);
    CHECK(all_bytes(GP(DMA_A + 0x40000), 160 * 128 * 4, 0x11));
    /* A render target and a newer buffer can use different GPU aliases of the
       same memory. A later partial read of the image must not restore its old
       pixels over the buffer written through the other alias. */
    const u64 alias = DMA_GA + 0x800000;
    gmmu_map(alias, DMA_A, DMA_SPAN);
    rt_clear(ch, alias, 160, 256, 4, 1);
    guest_done(DMA_B, 512); /* the GPU's writes there first, as the guest's fence has them */
    memset(GP(DMA_B), 0x6d, 512);
    o.src = pitch_side(DMA_GB, 512);
    o.dst = pitch_side(DMA_GA + 0x10000, 512);
    o.width = o.line_in = o.line_out = 512;
    o.lines = 1;
    CHECK(g_renderer->dma_copy(&o));
    o.src = pitch_side(alias + 0x1000, 512);
    o.dst = pitch_side(DMA_GA + 0x40000, 512);
    CHECK(g_renderer->dma_copy(&o));
    guest_done(DMA_A + 0x10000, 512);
    CHECK(all_bytes(GP(DMA_A + 0x10000), 512, 0x6d));
    guest_done(DMA_A, 512);
    CHECK(all_blue(GP(DMA_A), 512));
    /* Replacing a GPU mapping must resolve the old target against its old
       backing, before an image read can reach the replacement buffer. */
    rt_clear(ch, alias, 160, 256, 4, 1);
    guest_done(DMA_B, 0x28000); /* the GPU's writes there first, as the guest's fence has them */
    memset(GP(DMA_B), 0x37, 0x28000);
    gmmu_map(alias, DMA_B, DMA_SPAN);
    guest_done(DMA_A, 0x28000);
    CHECK(all_blue(GP(DMA_A), 0x28000));
    o.src = pitch_side(alias + 0x1000, 512);
    o.dst = pitch_side(DMA_GA + 0x40000, 512);
    CHECK(g_renderer->dma_copy(&o));
    guest_done(DMA_B, 0x28000);
    CHECK(all_bytes(GP(DMA_B), 0x28000, 0x37));
    guest_done(DMA_A + 0x40000, 512);
    CHECK(all_bytes(GP(DMA_A + 0x40000), 512, 0x37));
    /* Dirty metadata queries must leave GPU-owned pixels on the GPU, or, with unified memory, the pixels written in
       place (vk_uma_publish) not waited for. */
    const u64 tracked = DMA_GA + 0xe0000, tracked_cpu = DMA_A + 0xe0000;
    rt_clear(ch, tracked, 64, 64, 3, 1);
    vk_lock();
    int inplace = vk_uma_held(tracked_cpu, 0x4000);
    Surf* target = vk_rt_find(tracked);
    CHECK(target && vk_surf_publish(target));
    int tracking = vk_written_since(gmem_host(tracked, 0x4000), 0x4000, 0) >= 0;
    u64 landings = own_landings();
    vk_rt_drawn_full(target);
    (void)vk_rt_drawn_fresh(target);
    CHECK(own_landings() == landings);
    if (inplace)
        CHECK(own_lookup(tracked_cpu, 0x4000, NULL, 0) == 0 && vk_uma_pending_cpu(tracked_cpu, 0x4000));
    else if (own_traps())
        CHECK(own_lookup(tracked_cpu, 0x4000, NULL, 0) > 0);
    vk_unlock();
    guest_done(tracked_cpu + 16, 16); /* the GPU's writes there first, as the guest's fence has them */
    memset(GP(tracked_cpu + 16), 0x39, 16);
    g_renderer->mem_may_have_changed();
    vk_lock();
    if (tracking) CHECK(!vk_rt_drawn_fresh(target));
    vk_unlock();
    /* A target the program wrote part of after its draw, as when a game gives a target's memory to its command lists:
       a GPU write in its span writes it back, but for the pages the program wrote, which keep its bytes. The program
       wrote them after the GPU drew there. Without write tracking, the write-back can't tell them */
    rt_clear(ch, DMA_GA, 160, 256, 4, 1);
    guest_done(DMA_A + 0x8000, 0x2000); /* the GPU's writes there first, as the guest's fence has them */
    memset(GP(DMA_A + 0x8000), 0x5a, 0x2000);
    g_renderer->mem_may_have_changed();
    vk_rt_before_write(DMA_GA + 0x20000, 16);
    if (tracking) CHECK(all_bytes(GP(DMA_A + 0x8000), 0x2000, 0x5a));
    guest_done(DMA_A, 0x8000);
    CHECK(all_blue(GP(DMA_A), 0x8000));
    guest_done(DMA_A + 0xa000, 0x28000 - 0xa000);
    CHECK(all_blue(GP(DMA_A + 0xa000), 0x28000 - 0xa000));
    /* drawn over whole again: its next write-back is every pixel, the program's bytes no longer newer */
    rt_clear(ch, DMA_GA, 160, 256, 4, 1);
    vk_rt_before_write(DMA_GA + 0x20000, 16);
    guest_done(DMA_A, 0x28000);
    CHECK(all_blue(GP(DMA_A), 0x28000));
    /* A target loaded from memory and then changed by a copy, with no draw since, so no draw baseline: the pages the
       program wrote after the load keep its bytes too. The rest is the image, as it was loaded */
    guest_done(DMA_A, 0x28000); /* the GPU's writes there first, as the guest's fence has them */
    memset(GP(DMA_A), 0x21, 0x28000);
    g_renderer->mem_may_have_changed();
    vk_lock();
    vk_rt_reload(DMA_GA);
    Surf* copied = vk_rt_find(DMA_GA);
    CHECK(copied);
    vk_surf_wrote(copied);
    vk_unlock();
    guest_done(DMA_A + 0x10000, 0x1000); /* the GPU's writes there first, as the guest's fence has them */
    memset(GP(DMA_A + 0x10000), 0x5b, 0x1000);
    g_renderer->mem_may_have_changed();
    vk_rt_before_write(DMA_GA + 0x20000, 16);
    if (tracking) CHECK(all_bytes(GP(DMA_A + 0x10000), 0x1000, 0x5b));
    guest_done(DMA_A, 0x10000);
    CHECK(all_bytes(GP(DMA_A), 0x10000, 0x21));
    guest_done(DMA_A + 0x11000, 0x17000);
    CHECK(all_bytes(GP(DMA_A + 0x11000), 0x17000, 0x21));
    /* A target whose pages lie in two runs of guest memory, as a virtual pool maps them: the page of the second run
       that the program wrote keeps its bytes too */
    const u64 split = DMA_GA + 0xa00000;
    gmmu_map(split, DMA_B, 0x10000);
    gmmu_map(split + 0x10000, DMA_A + 0x40000, 0x10000);
    rt_clear(ch, split, 160, 128, 4, 1);
    guest_done(DMA_A + 0x41000, 0x1000); /* the GPU's writes there first, as the guest's fence has them */
    memset(GP(DMA_A + 0x41000), 0x6c, 0x1000);
    g_renderer->mem_may_have_changed();
    vk_rt_before_write(split + 0x13000, 16);
    if (tracking) CHECK(all_bytes(GP(DMA_A + 0x41000), 0x1000, 0x6c));
    guest_done(DMA_B, 0x10000);
    CHECK(all_blue(GP(DMA_B), 0x10000));
    guest_done(DMA_A + 0x40000, 0x1000);
    CHECK(all_blue(GP(DMA_A + 0x40000), 0x1000));
    guest_done(DMA_A + 0x42000, 0x2000);
    CHECK(all_blue(GP(DMA_A + 0x42000), 0x2000));
    free(ch);
    return 0;
}

/* Guest memory read in place whose host pages change (gpu/vk/uma.c). amdgpu takes every import's pages again in each
   submission and refuses all of it on pages gone or trapped since (EFAULT: the device is lost), so the import over
   pages the guest unmaps leaves the driver with the unmap, once the commands that read it ran, and before anything else
   is submitted, while an import elsewhere stays the same one, and one a DMA copy writes in place too; and one dropped
   from the table that a recorded command still reads leaves it with the unmap. A driver that takes
   no pages again (lavapipe) shows it in the imports held. Write tracking can't tell what was written on an imported
   page */
#define UMA_R  (HEAP_BASE + 0x1000000) /* mapped and unmapped here */
#define UMA_GR 0x4d0000000ull
#define UMA_E  (HEAP_BASE + 0x1200000) /* another import, two windows away */
#define UMA_GE 0x4d1000000ull
static int check_uma_page_changes(void) {
    VkBuffer r, e, again, rb;
    u64 roff, eoff, again_off;
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    CHECK(mem_map(UMA_R, 0x10000, MS_NORMAL, PERM_RW) == 0 && mem_map(UMA_E, 0x10000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GR, UMA_R, 0x10000);
    gmmu_map(UMA_GE, UMA_E, 0x10000);
    memset(GP(UMA_R), 0x44, 0x10000);
    memset(GP(UMA_E), 0x55, 0x10000);
    vk_lock();
    u8* result = vk_readback_buffer(768, &rb);
    CHECK(result);
    CHECK(vk_buf_get_input(UMA_GR + 0x100, 256, 4, &r, &roff) && vk_buf_get_input(UMA_GE + 0x100, 256, 4, &e, &eoff));
    CHECK(vk_uma_imports(UMA_R, 0x10000) == 1 && vk_uma_imports(UMA_E, 0x10000) == 1);
    VkBufferCopy c[3] = {{roff, 0, 256}, {eoff, 256, 256}, {eoff, 512, 256}};
    rec_CmdCopyBuffer(vk_cmd(), r, rb, 1, &c[0]);
    rec_CmdCopyBuffer(vk_cmd(), e, rb, 1, &c[1]);
    vk_unlock();
    CHECK(mem_unmap(UMA_R, 0x10000) == 0); /* while the copy that reads it is only recorded: the copy runs first */
    vk_lock();
    rec_CmdCopyBuffer(vk_cmd(), e, rb, 1, &c[2]); /* work of its own for a submission after the unmap */
    vk_finish();
    vk_readback_ready();
    CHECK(vk_uma_imports(UMA_R, 0x10000) == 0);
    for (u32 i = 0; i < 768; i++) CHECK(result[i] == (i < 256 ? 0x44 : 0x55));
    /* the import elsewhere: the one a lookup with nothing changed finds, not a slot the change emptied */
    CHECK(vk_buf_get_input(UMA_GE + 0x100, 256, 4, &again, &again_off) && again == e && again_off == eoff);
    CHECK(vk_uma_imports(UMA_E, 0x10000) == 1);
    vk_unlock();
    /* a DMA copy's destination is written in place (vk_uma_publish), after the copy recorded before it that reads it:
       the import stays, with no trap (where no one import can hold a result, it is trapped, and the imports over it go
       first: vulkan_uma_imports.inc) */
    CHECK(mem_map(UMA_R, 0x10000, MS_NORMAL, PERM_RW) == 0);
    memset(GP(UMA_R), 0x66, 0x10000);
    vk_lock();
    CHECK(vk_buf_get_input(UMA_GR + 0x1000, 256, 4, &r, &roff) && vk_uma_imports(UMA_R + 0x1000, 0x1000) == 1);
    c[0] = (VkBufferCopy){roff, 0, 256};
    rec_CmdCopyBuffer(vk_cmd(), r, rb, 1, &c[0]);
    vk_unlock();
    GpuDmaOp o = {.width = 256, .line_in = 256, .line_out = 256, .lines = 1};
    o.src = pitch_side(UMA_GE + 0x2000, 0);
    o.dst = pitch_side(UMA_GR + 0x1000, 0);
    CHECK(g_renderer->dma_copy(&o));
    vk_lock();
    rec_CmdCopyBuffer(vk_cmd(), e, rb, 1, &c[2]); /* work of its own for a submission after the trap */
    vk_finish();
    vk_readback_ready();
    CHECK(vk_uma_imports(UMA_R + 0x1000, 0x1000) == 1 && own_lookup(UMA_R + 0x1000, 256, NULL, 0) == 0);
    for (u32 i = 0; i < 256; i++) CHECK(result[i] == 0x66); /* read before the copy over it */
    vk_unlock();
    CHECK(!memcmp(GP(UMA_R) + 0x1000, GP(UMA_E) + 0x2000, 256) && GP(UMA_R)[0x1100] == 0x66);
    /* an import dropped from the table (a save state's load forgets them all) is the driver's until the copy recorded
       with it is done: its pages unmapped before that, the copy runs first and the import goes with the unmap. The
       bytes after the DMA copy's, in the import it kept (a window mapped in part takes no second import) */
    vk_lock();
    CHECK(vk_buf_get_input(UMA_GR + 0x1100, 256, 4, &r, &roff) && vk_uma_imports(UMA_R + 0x1100, 0x100) == 1);
    c[0] = (VkBufferCopy){roff, 0, 256};
    rec_CmdCopyBuffer(vk_cmd(), r, rb, 1, &c[0]);
    vk_uma_forget_all();
    CHECK(vk_uma_imports(UMA_R + 0x1100, 0x100) == 1); /* the copy still reads it */
    vk_unlock();
    CHECK(mem_unmap(UMA_R, 0x10000) == 0);
    vk_lock();
    CHECK(vk_buf_get_input(UMA_GE + 0x100, 256, 4, &e, &eoff));
    c[2].srcOffset = eoff;
    rec_CmdCopyBuffer(vk_cmd(), e, rb, 1, &c[2]); /* work of its own for a submission after the unmap */
    vk_finish();
    vk_readback_ready();
    CHECK(vk_uma_imports(UMA_R, 0x10000) == 0);
    for (u32 i = 0; i < 256; i++) CHECK(result[i] == 0x66);
    /* write tracking leaves an imported page alone: amdgpu takes it again, writable, at each submission, which a scan
       reports as a CPU write. It can't tell there, and still tells the pages beside it, whose memset above is taken in
       before the baseline: a write counts from the scan that finds it */
    vk_unlock();
    g_renderer->mem_may_have_changed();
    vk_lock();
    (void)vk_written_since(GP(UMA_E) + 0x8000, 0x1000, 0);
    u32 before = vk_ww_now();
    CHECK(vk_uma_imports(UMA_E, 0x1000) >= 1); /* the import made since, and the dropped one until a present reaps it */
    rec_CmdCopyBuffer(vk_cmd(), e, rb, 1, &c[2]);
    vk_finish();
    vk_unlock();
    g_renderer->mem_may_have_changed();
    vk_lock();
    int imported = vk_written_since(GP(UMA_E), 0x1000, before),
        beside = vk_written_since(GP(UMA_E) + 0x8000, 0x1000, before);
    vk_unlock();
    if (imported != -1 || beside == 1)
        fprintf(stderr, "write tracking since the baseline: %d on the imported page, %d beside it\n", imported, beside);
    CHECK(imported == -1 && beside != 1);
    CHECK(mem_unmap(UMA_E, 0x10000) == 0);
    printf("in place: an unmapped import leaves the driver with the unmap, one elsewhere stays, one a DMA copy writes "
           "stays, one dropped goes with an unmap, and write tracking leaves an imported page alone\n");
    return 0;
}

#include "vulkan_shader_invalidate.inc"
#include "vulkan_shared_clamp.inc"
#include "vulkan_index_draws.inc"
#include "vulkan_pass_breaks.inc"
#include "vulkan_storage_formats.inc"
#include "vulkan_storage_atomics.inc"
#include "vulkan_surface_loads.inc"
#include "vulkan_rect_textures.inc"
#include "vulkan_owned_readback.inc"
#include "vulkan_storage_suffix.inc"
#include "vulkan_pending_index.inc"
#include "vulkan_draw_storage_stages.inc"
#include "vulkan_mesh_inputs.inc"
#include "vulkan_vertex_input.inc"
#include "vulkan_frag_coord.inc"
#include "vulkan_vertex_restride.inc"
#include "vulkan_msaa.inc"
#include "vulkan_targetless_stores.inc"
#include "vulkan_storage_image_keep.inc"
#include "vulkan_uma_imports.inc"
#include "vulkan_write_tracking.inc"
#include "vulkan_tex_partial.inc"

#include "vulkan_descriptor_bench.inc"
#include "vulkan_nvn_storage.inc"
#include "vulkan_nvn_compat.inc"
#include "vulkan_nvn_copy_flags.inc"
#include "vulkan_nvn_copy_conversions.inc"
#include "vulkan_nvn_xfb.inc"
#include "vulkan_nvn_state.inc"
#include "vulkan_nvn_events.inc"
#include "vulkan_nvn_subroutines.inc"
#include "vulkan_nvn_tess.inc"
#include "vulkan_nvn_blend.inc"
#include "vulkan_nvn_advanced_native.inc"
#include "vulkan_nvn_advanced_shader.inc"
#include "vulkan_nvn_blend_equations.inc"
#include "vulkan_nvn_coverage.inc"
#include "vulkan_sparse_bench.inc"
#include "vulkan_storage_lookup_bench.inc"
#include "vulkan_storage_buffer_keep.inc"

/* key's row among the settings the runtime applies itself (options.c's), or NULL */
static const IniOpt* setting_row(const char* key) {
    for (int s = 0; s < g_ini_sections_count; s++)
        for (int t = 0; t < 3 && g_ini_sections[s].own && g_ini_sections[s].table[t]; t++) {
            const IniOpt* o = ini_find(g_ini_sections[s].table[t], key);
            if (o) return o;
        }
    return NULL;
}

/* A change of guest memory lands the pending buffer results only when one covers a guest page of it
   (vkr_mapping_begin): a change elsewhere leaves a result pending, as it changes none of its bytes, and a change over
   its guest page lands it first */
static int check_map_lands_covered(void) {
    const u64 gva = 0xab00000, cpu = HEAP_BASE + 0x5c00000;
    VkBuffer out;
    u64 at;
    if (vk_uma()) return 0;
    CHECK(mem_map(cpu, 0x1000, MS_NORMAL, PERM_RW) == 0); /* a result counts the guest pages memory holds */
    gmmu_map(gva, cpu, 0x1000);
    vk_lock();
    CHECK(vk_buf_copy_writable(gva, 0x1000, &out, &at, 1));
    vk_buf_writable_commit();
    vk_unlock();
    CHECK(vk_buf_pending(gva, 0x1000));
    g_gpu_mapping_begin(cpu + 0x10000, 0x10000, 1); /* fresh memory past it, as svcMapPhysicalMemory maps */
    g_gpu_mapping_end();
    CHECK(vk_buf_pending(gva, 0x1000));
    g_gpu_mapping_begin(cpu, 0x1000, 1);
    g_gpu_mapping_end();
    CHECK(!vk_buf_pending(gva, 0x1000));
    puts("A change of guest memory lands the buffer results over its pages, and only those");
    return 0;
}

int main(int argc, char** argv) {
    nvn_init();
    const u32 w = 32, h = 16;
    const u64 address = HEAP_BASE;
    const i32 full[4] = {0, 0, 0, 0}, crop[4] = {4, 2, 20, 10};
    char *root, *shot, *ppm;
    setvbuf(stdout, NULL, _IOLBF, 0); /* a CHECK's stderr line lands between whole lines in a log both go to */
    CHECK(!snx_startup_options(argc, argv));
    int nvn_benchmark = -1;
    u32 benchmark_sets = 0, benchmark_count = 16, benchmark_images = 0, benchmark_sparse = 0, benchmark_storage = 0;
    u32 benchmark_image = 0;
    u32 benchmark_buffer = 0;
    u32 benchmark_passes = 0;
    /* the runtime's startup flags (-H), its settings as it takes them (--vk_memory=2, --memory_traps=2,
       --vertex_input_dynamic_state=off), and the benchmarks'. Any other argument stops the run: one dropped unsaid ran
       the checks without it */
    for (int i = 1; i < argc; i++) {
        const char* eq = strchr(argv[i], '=');
        const IniOpt* o = NULL;
        unsigned flags = 0;
        char key[64];
        if (snx_cli_flag(argv[i], &flags) == 1) continue;
        if (!strncmp(argv[i], "--nvn-draw-bench=", 17)) {
            nvn_benchmark = (int)strtol(argv[i] + 17, NULL, 10);
            continue;
        }
        if (!strncmp(argv[i], "--bench-descriptors=", 20)) {
            benchmark_sets = (u32)strtoul(argv[i] + 20, NULL, 10);
            continue;
        }
        if (!strncmp(argv[i], "--descriptor-images=", 20)) {
            benchmark_images = (u32)strtoul(argv[i] + 20, NULL, 10);
            continue;
        }
        if (!strncmp(argv[i], "--descriptor-count=", 19)) {
            benchmark_count = (u32)strtoul(argv[i] + 19, NULL, 10);
            continue;
        }
        if (!strcmp(argv[i], "--bench-sparse-volume")) {
            benchmark_sparse = 1;
            continue;
        }
        if (!strcmp(argv[i], "--bench-sparse-texture")) {
            benchmark_sparse = 2;
            continue;
        }
        if (!strcmp(argv[i], "--bench-storage-lookup")) {
            benchmark_storage = 1;
            continue;
        }
        if (!strcmp(argv[i], "--bench-storage-image")) {
            benchmark_image = 1;
            continue;
        }
        if (!strcmp(argv[i], "--bench-storage-buffer")) {
            benchmark_buffer = 1;
            continue;
        }
        if (!strncmp(argv[i], "--bench-pass-breaks=", 20)) {
            benchmark_passes = (u32)strtoul(argv[i] + 20, NULL, 10);
            continue;
        }
        if (!strncmp(argv[i], "--", 2) && eq && (size_t)(eq - argv[i] - 2) < sizeof key) {
            snprintf(key, sizeof key, "%.*s", (int)(eq - argv[i] - 2), argv[i] + 2);
            o = setting_row(key);
        }
        if (!o) {
            fprintf(
                stderr,
                "snx-test-vulkan: %s is not an option it takes: -H, --KEY=VALUE of the runtime's settings, "
                "--bench-descriptors=N, --descriptor-images=N, --descriptor-count=N, --bench-sparse-volume, --bench-sparse-texture, --bench-storage-lookup, --bench-storage-image, --bench-storage-buffer, --bench-pass-breaks=N, --nvn-draw-bench=N\n",
                argv[i]);
            return 2;
        }
        ini_read(o, eq + 1);
    }
    root = snx_temp_dir(NULL);
    CHECK(root);
    snprintf(g_exedir, 1024, "%s/", root);
    /* the shader files (pipeline.c's shader_cache) this run's alone: the working directory's are every test's run
       there, whose pipelines the workers would make beside the checks, under the VIC test's validation layer too */
    snprintf(g_cachedir, sizeof g_cachedir, "%s/", root);
    shot = snx_path_join(root, "frame");
    ppm = snx_format("%s.ppm", shot);
    mem_init(); /* the real reservation and alias pool: the renderer traps what the GPU owns where the host can */
    CHECK(mem_map(HEAP_BASE, 0x800000, MS_NORMAL, PERM_RW) == 0);
    nv_init();                          /* color targets may import guest memory through the GPU page table */
    g_settings.vulkan_video_decode = 1; /* a Vulkan Video decode queue where the device has one (lavapipe has none) */
    video_init("Headless Vulkan fixture", "");
    CHECK(g_opt_headless && g_renderer_up && g_vk.dev && !g_vk.surface && !g_vk.swapchain && !g_vk.sc_count);
    if (getenv("SNX_TEST_RECORDER_ONLY")) { /* the recording thread's own sync alone, on the driver the loader picks */
        CHECK(!check_recorder_self_sync());
        vk_finish();
        vk_pipeline_quiesce();
        printf("PASS headless Vulkan recorder only: %u validation messages\n", g_vk_validation_messages);
        return 0;
    }
    CHECK(!check_recorder_self_sync());
    viw_install();
    if (g_settings.descriptor_buffers && !g_vk.has_descriptor_buffers) {
        puts("SKIP descriptor-buffer checks: the device does not support this path");
        vk_pipeline_quiesce(); /* the shader files' reading thread, before the driver's exit */
        CHECK(!snx_remove_tree(root));
        free(root);
        free(shot);
        free(ppm);
        return 77;
    }
    if (nvn_benchmark >= 0) {
        CHECK(nvn_benchmark <= 2);
        int result = bench_nvn_storage(nvn_benchmark, 256, 8);
        vk_pipeline_quiesce();
        CHECK(!snx_remove_tree(root));
        free(root);
        free(shot);
        free(ppm);
        return result;
    }
    if (benchmark_image) {
        int result = bench_storage_image();
        vk_pipeline_quiesce();
        CHECK(!snx_remove_tree(root));
        free(root);
        free(shot);
        free(ppm);
        return result;
    }
    if (benchmark_storage) {
        int result = bench_storage_lookup();
        vk_pipeline_quiesce();
        CHECK(!snx_remove_tree(root));
        free(root);
        free(shot);
        free(ppm);
        return result;
    }
    if (benchmark_buffer) {
        int result = bench_storage_buffer();
        vk_pipeline_quiesce();
        CHECK(!snx_remove_tree(root));
        free(root);
        free(shot);
        free(ppm);
        return result;
    }
    if (benchmark_sets) {
        int result = bench_descriptors(benchmark_sets, benchmark_count, benchmark_images);
        vk_pipeline_quiesce();
        CHECK(!snx_remove_tree(root));
        free(root);
        free(shot);
        free(ppm);
        return result;
    }
    if (benchmark_passes) {
        int result = bench_pass_breaks(120, benchmark_passes);
        vk_pipeline_quiesce();
        CHECK(!snx_remove_tree(root));
        free(root);
        free(shot);
        free(ppm);
        return result;
    }
    if (benchmark_sparse) {
        int result = benchmark_sparse == 2 ? bench_sparse_textures() : bench_sparse_volumes();
        vk_pipeline_quiesce();
        CHECK(!snx_remove_tree(root));
        free(root);
        free(shot);
        free(ppm);
        return result;
    }
    if (getenv("SNX_TEST_NVN_CASE")) {
        const char* name = getenv("SNX_TEST_NVN_CASE");
        int result;
        if (!strcmp(name, "events"))
            result = check_nvn_event_order();
        else if (!strcmp(name, "subroutines"))
            result = check_nvn_subroutine_calls();
        else if (!strcmp(name, "copies"))
            result = check_nvn_copy_conversions();
        else if (!strcmp(name, "advanced_blend"))
            result = check_nvn_advanced_native();
        else if (!strcmp(name, "advanced_shader"))
            result = check_nvn_advanced_shader();
        else if (!strcmp(name, "blend_equations"))
            result = check_nvn_blend_equations();
        else {
            fprintf(stderr, "Unknown NVN case: %s\n", name);
            return 2;
        }
        vk_lock();
        vk_finish();
        vk_unlock();
        vk_pipeline_quiesce();
        CHECK(!snx_remove_tree(root));
        free(root);
        free(shot);
        free(ppm);
        return result;
    }
    if (getenv("SNX_TEST_BUFFER_KEEP_ONLY")) { /* the writable storage buffers' check alone, for its mutants */
        CHECK(!check_storage_buffer_keep());
        vk_finish();
        vk_pipeline_quiesce();
        printf("PASS headless Vulkan writable storage buffers only: %u validation messages\n",
               g_vk_validation_messages);
        return 0;
    }
    if (getenv("SNX_TEST_MSAA_ONLY")) { /* the multisample checks alone */
        CHECK(!check_multisample_shader_gate());
        CHECK(!check_msaa());
        CHECK(!check_msaa_nvn());
        vk_pipeline_quiesce();
        printf("PASS headless Vulkan multisampling only: %u validation messages\n", g_vk_validation_messages);
        return 0;
    }
    if (getenv("SNX_TEST_VERTEX_INPUT_ONLY")) { /* the vertex input checks alone, for their mutants */
        CHECK(!check_vertex_input_layouts());
        CHECK(!check_vertex_input_file_key());
        vk_finish();
        vk_pipeline_quiesce();
        CHECK(!viw_report());
        printf("PASS headless Vulkan vertex input only: %u validation messages\n", g_vk_validation_messages);
        return 0;
    }
    if (getenv("SNX_TEST_PASS_BREAKS_ONLY")) { /* vulkan_pass_breaks.inc's checks alone */
        CHECK(!check_pass_breaks());
        vk_pipeline_quiesce();
        printf("PASS headless Vulkan pass breaks only: %u validation messages\n", g_vk_validation_messages);
        return 0;
    }
    if (getenv("SNX_TEST_TEX_PARTIAL_ONLY")) { /* vulkan_tex_partial.inc's checks, which run only so */
        const char* which = getenv("SNX_TEST_TEX_PARTIAL_ONLY");
        if (strcmp(which, "1") && strcmp(which, "no-astc")) {
            fprintf(stderr,
                    "SNX_TEST_TEX_PARTIAL_ONLY=%s names no set: 1 (every case) or no-astc (the ASTC ones left "
                    "out)\n",
                    which);
            return 2;
        }
        CHECK(!check_tex_partial(strcmp(which, "no-astc") != 0));
        vk_pipeline_quiesce();
        printf("PASS headless Vulkan texture changes only: %u validation messages\n", g_vk_validation_messages);
        return 0;
    }
    if (getenv("SNX_TEST_WW_ONLY")) { /* the write tracking's contract alone (vulkan_write_tracking.inc) */
        CHECK(!check_write_tracking());
        vk_pipeline_quiesce();
        printf("PASS headless Vulkan write tracking only: %u validation messages\n", g_vk_validation_messages);
        return 0;
    }
    if (getenv("SNX_TEST_UMA_ONLY") && strcmp(getenv("SNX_TEST_UMA_ONLY"), "1")) { /* one of them, by name */
        const char* name = getenv("SNX_TEST_UMA_ONLY");
        int ran = 0;
        if (!strcmp(name, "runtime_write") && ++ran) CHECK(!check_uma_runtime_write());
        if (!strcmp(name, "constants") && ++ran) CHECK(!check_uma_constants());
        if (!strcmp(name, "migrate") && ++ran) CHECK(!check_uma_migrate());
        if (!strcmp(name, "way") && ++ran) CHECK(!check_uma_way());
        if (!strcmp(name, "region_writes") && ++ran) CHECK(!check_uma_region_writes());
        if (!strcmp(name, "region_merge") && ++ran) CHECK(!check_uma_region_merge());
        if (!strcmp(name, "writes") && ++ran) CHECK(!check_uma_writes());
        if (!strcmp(name, "feedback") && ++ran) CHECK(!check_uma_feedback());
        if (!strcmp(name, "publish") && ++ran) CHECK(!check_uma_publish());
        if (!strcmp(name, "dispatch") && ++ran) CHECK(!check_uma_dispatch());
        if (!strcmp(name, "placed") && ++ran) CHECK(!check_uma_placed());
        if (!strcmp(name, "placed_view") && ++ran) CHECK(!check_uma_placed_view());
        if (!ran) {
            fprintf(stderr,
                    "SNX_TEST_UMA_ONLY=%s names no check: 1, runtime_write, constants, migrate, way, region_writes, "
                    "region_merge, writes, feedback, publish, dispatch, placed or placed_view\n",
                    name);
            return 2;
        }
        vk_pipeline_quiesce();
        printf("PASS headless Vulkan in place, %s: %u validation messages\n", name, g_vk_validation_messages);
        return 0;
    }
    if (getenv("SNX_TEST_UMA_ONLY")) { /* the checks of guest memory read in place alone, for a quick run on a GPU */
        CHECK(!check_uma_in_flight());
        CHECK(!check_uma_trap_ranges());
        CHECK(!check_uma_lookup_then_submit());
        CHECK(!check_uma_runtime_write());
        CHECK(!check_uma_constants());
        CHECK(!check_uma_writes());
        CHECK(!check_uma_feedback());
        CHECK(!check_uma_publish());
        CHECK(!check_uma_dispatch());
        CHECK(!check_uma_placed());
        CHECK(!check_uma_placed_view());
        CHECK(!check_uma_page_changes());
        CHECK(!check_uma());
        CHECK(!check_uma_way());
        CHECK(!check_uma_migrate());
        CHECK(!check_uma_region_writes());
        CHECK(!check_uma_region_merge());
        vk_pipeline_quiesce();
        printf("PASS headless Vulkan in place only: %u validation messages\n", g_vk_validation_messages);
        return 0;
    }
    if (getenv("SNX_TEST_VIC_ONLY")) { /* the validation test: VIC's bindings, none of the other checks' traffic */
        CHECK(!check_vic());
        CHECK(!check_vic_chain(shot));
        CHECK(!check_nvdec_vic());
        vk_pipeline_quiesce();
        printf("PASS headless Vulkan VIC only: %u validation messages\n", g_vk_validation_messages);
        return 0;
    }
    CHECK(!check_hash_copy());
    CHECK(!check_tex_view_refusal());
    CHECK(!check_staging_overflow()); /* first: it counts the staging blocks of a fresh device */
    CHECK(!check_rings_without_present());
    CHECK(!check_fresh_frame_kept());
    CHECK(!check_stats_gpu_time());
    CHECK(!check_staging_burst());
    CHECK(!check_chunk_after_done());
    CHECK(!check_small_uploads());
    CHECK(!check_big_uploads());
    CHECK(!check_stream_epoch());
    CHECK(!check_owned_readback());
    CHECK(!check_owned_writable());
    CHECK(!check_owned_in_place_write());
    CHECK(!check_owned_peek_identity());
    CHECK(!check_owned_whole_identity());
    CHECK(!check_storage_suffix());
    CHECK(!check_pending_index());
    CHECK(!check_draw_storage_stages());
    CHECK(!bench_nvn_storage(0, 32, 1));
    CHECK(!bench_nvn_storage(1, 32, 1));
    CHECK(!bench_nvn_storage(2, 32, 1));
    CHECK(!check_buffer_texture_rows());
    CHECK(!check_nvn_array_copies());
    CHECK(!check_nvn_copy_flags());
    CHECK(!check_nvn_stencil_masks());
    CHECK(!check_nvn_xfb());
    CHECK(!check_nvn_fixed_state());
    CHECK(!check_nvn_default_tessellation());
    CHECK(!check_nvn_advanced_blend());
    CHECK(!check_nvn_coverage());
    CHECK(!check_nvn_copy_conversions());
    CHECK(!check_nvn_event_order());
    CHECK(!check_nvn_subroutine_calls());
    CHECK(!check_nvn_advanced_native());
    CHECK(!check_nvn_advanced_shader());
    CHECK(!check_nvn_blend_equations());
    CHECK(!check_storage_image_formats());
    CHECK(!check_volume_surface_atomics());
    CHECK(!check_layered_surface_loads());
    CHECK(!check_rect_textures());
    CHECK(!check_viewport_orientation());
    CHECK(!check_index_draws());
    CHECK(!check_pass_breaks());
    CHECK(!check_mesh_inputs());
    CHECK(!check_frag_coord_origin());
    CHECK(!check_vertex_restride());
    CHECK(!check_vertex_input_layouts());
    CHECK(!check_vertex_input_file_key());
    CHECK(!check_targetless_stores());
    CHECK(!check_storage_image_keep());
    CHECK(!check_storage_buffer_keep());
    CHECK(!check_packed_depth());
    CHECK(!check_astc_mode());
    CHECK(!check_depth_array());
    CHECK(!check_depth_writeback());
    CHECK(!check_big_volume());
    CHECK(!check_srgb_write());
    CHECK(!check_clear_load_op());
    CHECK(!check_sparse_while_submitting());
    CHECK(!check_multisample_shader_gate());
    CHECK(!check_msaa());
    CHECK(!check_msaa_nvn());
    CHECK(!check_direct_spirv());
    CHECK(!check_flush_subnormal());
    CHECK(!check_saturate());
    CHECK(!check_sampler_reduction());
    CHECK(!check_shader_invalidation());
    CHECK(!check_shared_clamp());
    CHECK(!check_vic());
    CHECK(!check_vic_chain(shot));
    CHECK(!check_nvdec_vic());
    CHECK(!check_dma());
    CHECK(!check_dma_owners());
    CHECK(!check_dma_bands());
    CHECK(!check_flush_parts());
    CHECK(!check_vic_bands());
    CHECK(!check_dma_texture());
    CHECK(!check_sparse_volume());
    CHECK(!check_sparse_view());
    CHECK(!check_sparse_tiles());
    CHECK(!check_sparse_copy());
    CHECK(!check_resident_volume());
    CHECK(!check_sparse_unbind_in_flight());
    CHECK(!check_sparse_resident_batched());
    CHECK(!check_resident_texture());
    CHECK(!check_resident_astc());
    CHECK(!check_resident_texture_in_flight());
    CHECK(!check_sparse_pieces_batched());
    CHECK(!check_dma_untracked_source());
    CHECK(!check_dma_tiles());
    CHECK(!check_map_lands_covered());
    CHECK(!check_blit());
    CHECK(!check_blit_bands());
    CHECK(!check_take_after_writes());
    CHECK(!check_targets_kept());
    CHECK(!check_uma_in_flight());
    CHECK(!check_uma_trap_ranges());
    CHECK(!check_uma_lookup_then_submit());
    CHECK(!check_uma_runtime_write());
    CHECK(!check_uma_constants());
    CHECK(!check_uma_writes());
    CHECK(!check_uma_feedback());
    CHECK(!check_uma_publish());
    CHECK(!check_uma_dispatch());
    CHECK(!check_uma_placed());
    CHECK(!check_uma_placed_view());
    CHECK(!check_uma_page_changes());
    CHECK(!check_rt_overlap());
    CHECK(!check_uma());
    CHECK(!check_uma_way());
    CHECK(!check_uma_migrate());
    CHECK(!check_uma_region_writes());
    CHECK(!check_uma_region_merge());
    CHECK(!check_write_tracking());
    CHECK(!check_descriptor_room());
    /* the pipeline workers: pipeline_threads when given, else all the CPUs but two, two to twelve */
    CHECK(snx_vk_pipeline_workers(0, 8) == 6 && snx_vk_pipeline_workers(0, 12) == 10 &&
          snx_vk_pipeline_workers(0, 4) == 2 && snx_vk_pipeline_workers(0, 0) == 2 &&
          snx_vk_pipeline_workers(0, 64) == 12);
    CHECK(snx_vk_pipeline_workers(5, 12) == 5 && snx_vk_pipeline_workers(32, 4) == 32 &&
          snx_vk_pipeline_workers(1, 64) == 1);
    /* FFmpeg takes the device for Vulkan Video exactly when it has the decode queue */
    AVBufferRef* video_device = g_renderer->nvdec_hw_device();
    CHECK(!video_device == (g_vk.video_family == ~0u));
    av_buffer_unref(&video_device);
    CHECK(g_renderer->method_barrier);
    u32 epoch = vk_ww_epoch();
    u64 finishes = g_vk_finishes;
    for (int kind = 0; kind <= 4; kind++) g_renderer->method_barrier(kind);
    CHECK(vk_ww_epoch() == epoch); /* no barrier expires cached reads: their checks see writes (vkr_method_barrier) */
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
    vk_lock();
    vk_finish();
    vk_unlock();
    vk_pipeline_quiesce();
    CHECK(!viw_report());
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
