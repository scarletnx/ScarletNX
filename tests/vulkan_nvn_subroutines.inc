/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/nvn/nvn_object.h"
#include "nx/gpu/nvn/nvn_state.h"
#include "gpu/nvn/nvn.hook.h"

/* The same fragment program calls two functions through a changing compiler-constant table. Relative CAL and
   absolute JCAL must both return to EXIT and retain exact RGBA values across cached-pipeline reuse. */
static int check_nvn_subroutine_calls(void) {
    const u64 gpu = 0x4e4000000ull, cpu = HEAP_BASE + 0x670000;
    const u64 position = gpu + 0x3000, constants = gpu + 0x4000, target = gpu + 0x8000;
    const u64 program = cpu + 0xa000, control = cpu + 0xb000, pairs = cpu + 0xb900;
    const u64 map = cpu + 0xc000, pointers = cpu + 0xc100, values = cpu + 0xc200;
    const u64 pool = cpu + 0xa100, command = cpu + 0xa200;
    const u64 vertex[] = {0xefd9ff800807ff00ull, 0xeff1ff800707ff00ull, 0xe30000000007000full};
    u64 fragment[] = {0,
                      0xe30000000007000full,
                      storage_mov(0, 0x3f800000),
                      storage_mov(1, 0),
                      storage_mov(2, 0),
                      storage_mov(3, 0x3f000000),
                      0xe32000000007000full,
                      storage_mov(0, 0),
                      storage_mov(1, 0x3f800000),
                      storage_mov(2, 0),
                      storage_mov(3, 0x3f000000),
                      0xe32000000007000full};
    const f32 center[4] = {0.125f, 0.125f, 0, 1}, transform[6] = {4, 4, 0.5f, 4, 4, 0.5f};
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 116;
    gmmu_map(gpu, cpu, 0x10000);
    CHECK(!gmem_write(position, center, sizeof center));
    CHECK(mesh_program(gpu, vertex, 3, 0));
    *NVN_OBJ(NvnPool, pool) =
        (NvnPool){.magic = NVN_MAGIC('P', 'O', 'O', 'L'), .memory = cpu, .size = 0x10000, .gpu = gpu};
    nvn_CommandBufferInitialize(NULL, command, 0);
    nvn_CommandBufferAddCommandMemory(NULL, command, pool, 0x5000, 0x1000);
    nvn_CommandBufferAddControlMemory(NULL, command, cpu + 0x6000, 0x1000);
    u32* r = channel->r3d;
    r[0x582] = (u32)(gpu >> 32);
    r[0x583] = (u32)gpu;
    r[0x850] = 1;
    r[0x851] = 0x1030;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 8u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    all_samples(r);
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 1u << 21 | 7u << 27;
    r[0x700] = 0x1000 | 16;
    r[0x701] = (u32)(position >> 32);
    r[0x702] = (u32)position;
    r[0x7c0] = (u32)((position + 15) >> 32);
    r[0x7c1] = (u32)(position + 15);
    vk_lock();
    int is_depth;
    rt_clear_format(channel, target, 8, 8, 0, 1, nvn_rt_format(NVN_FORMAT_RGBA32F, &is_depth));
    for (u32 absolute = 0; absolute < 2; absolute++) {
        fragment[0] = absolute ? 0xe220001000070020ull : 0xe26000100007002full;
        CHECK(mesh_program(gpu + 0x1030, fragment, 12, 1));
        g_renderer->method_barrier(5); /* instruction bytes changed at the same program address */
        memset(GP(control), 0, 0x900);
        gw32(control, 0x98761234);
        gw32(control + 4, 1);
        gw32(control + 8, 16);
        gw32(control + 0x700, 0x3000);
        gw32(control + 0x704, 0x3100);
        gw32(control + 0x708, 0x30);
        gw32(control + 0x714, NVN_SHADER_STAGE_FRAGMENT);
        GP(control)[0x7c0] = 1;
        gw64(pairs, gpu + 0x1000);
        gw64(pairs + 8, control);
        nvn_ProgramInitialize(NULL, program, 0);
        CHECK(nvn_ProgramSetShaders(NULL, program, 1, pairs));
        const u32 header[20] = {0xfabffabc, NVN_SHADER_STAGE_FRAGMENT,
                                1,          9,
                                80,         136,
                                1,          absolute,
                                0,          0,
                                2,          80,
                                88,         96,
                                1,          104,
                                120,        128,
                                136,        1};
        memset(GP(map), 0, 136);
        for (u32 i = 0; i < 20; i++) gw32(map + 4 * i, header[i]);
        gw32(map + 88, absolute ? 24 : 8);
        gw32(map + 92, absolute ? 80 : 64);
        gw32(map + 104, 2);
        gw32(map + 108, 1);
        gw32(map + 112, 0);
        gw32(map + 116, 0);
        gw32(map + 120, 7);
        gw32(map + 124, 19);
        gw32(map + 128, 0);
        gw32(map + 132, 1);
        gw64(pointers, map);
        CHECK(nvn_ProgramSetSubroutineLinkage(NULL, program, 1, pointers));
        channel->r3d_dirty = ~0u;
        mx_draw_state_sync(channel);
        DsState* state = &channel->ds;
        state->bind.cb_addr[ST_FS][1] = constants;
        state->bind.cb_size[ST_FS][1] = 256;
        state->gen[DS_BIND] = ds_generation_next();
        for (u32 round = 0; round < 4; round++) {
            u32 select = round & 1;
            gw32(values, select ? 19 : 7);
            nvn_CommandBufferBeginRecording(NULL, command);
            nvn_CommandBufferSetProgramSubroutines(NULL, command, program, NVN_SHADER_STAGE_FRAGMENT, 0, 1, values);
            u64 handle = nvn_CommandBufferEndRecording(NULL, command);
            gw32(values, 99); /* caller data can change after the record */
            NvnDraw* front = calloc(1, sizeof *front);
            CHECK(front);
            nvn_draw_init(front, 0x80000002u);
            for (u64 at = handle;;) {
                NvnRecHdr record;
                memcpy(&record, GP(at), sizeof record);
                u32 op = NVN_REC_OP(record.op);
                if (op == OP_END) break;
                if (op == OP_JUMP) {
                    at = gr64(at + 8);
                    continue;
                }
                CHECK(op == OP_SUBROUTINES);
                CHECK(nvn_draw_record(front, op, at + 8, record.size - 8));
                if (record.op & NVN_REC_LAST) break;
                at += record.size;
            }
            nvn_draw_free(front);
            free(front);
            DrawClear clear = {.color_mask = 15, .color = {0, 0, 0x3f800000, 0x3f800000}};
            vk_render_clear(state, &clear, &channel->owner);
            DrawCall call = {.topology = 0, .count = 1, .instances = 1};
            u64 drawn = g_vk_drawn;
            vk_render_draw(state, &call, &channel->owner);
            CHECK(g_vk_drawn == drawn + 1);
            vk_render_end();
            Surf* surface = vk_rt_find(target);
            VkBuffer buffer;
            u8* pixels = vk_readback_buffer(8 * 8 * 16, &buffer);
            CHECK(surface && pixels);
            VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                      .imageExtent = {8, 8, 1}};
            vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                        VK_ACCESS_2_TRANSFER_READ_BIT);
            rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
            vk_finish();
            vk_readback_ready();
            for (u32 i = 0; i < 64; i++) {
                u32 expected[4] = {0, 0, 0x3f800000, 0x3f800000};
                if (i == 4 * 8 + 4) {
                    expected[2] = 0;
                    expected[select] = 0x3f800000;
                    expected[3] = 0x3f000000;
                }
                for (u32 c = 0; c < 4; c++) {
                    u32 actual = snx_ld_le32(pixels + i * 16 + c * 4);
                    if (actual != expected[c])
                        fprintf(stderr, "NVN subroutine absolute%u select%u pixel%u component%u got%08x expected%08x\n",
                                absolute, select, i, c, actual, expected[c]);
                    CHECK(actual == expected[c]);
                }
            }
        }
    }
    vk_unlock();
    nvn_CommandBufferFinalize(NULL, command);
    nvn_ProgramFinalize(NULL, program);
    free(channel);
    puts("PASS NVN subroutine calls: linkage records, relative/absolute targets and cached selection exact=1");
    return 0;
}
