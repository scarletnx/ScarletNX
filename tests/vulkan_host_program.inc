/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Host programs (Renderer.host_program, vk/host_program.c): a GL program linked by the GL layer (common/gpu/gl's link,
   glslc) in the renderer's rules for this device (gpu/gl/gl_host_program.c: each stage's sets, the constant buffers
   bound as storage buffers, the windows with their pushed offsets), each stage given as SPIR-V with the ShaderInfo
   the layer makes, drawn by a DsState that names them in DsPrograms.code[] (DS_HOST_PROGRAM) as the GL layer's draws
   do. The fragment program reads a uniform block's column-major and row-major matrices, an array by an index from
   the default block, and a scalar after them, and writes them as its color; drawn again with the block's bytes
   elsewhere, which with windows lands in the same window at an offset of its own (render.c cb_window_place) */

static u32 host_gl_shader(u32 ctx, u32 type, const char* text) {
    u32 s = snx_gl_create_shader(ctx, type);
    memcpy(snx_gl_shader_source(ctx, s, strlen(text)), text, strlen(text));
    snx_gl_compile_shader(ctx, s);
    return s;
}
/* pixel 0's four floats of the 8x8 RGBA32F target at target */
static int host_program_pixel(u64 target, f32 rgba[4]) {
    Surf* surface = vk_rt_find(target);
    VkBuffer buffer;
    u8* pixels = vk_readback_buffer(8 * 8 * 16, &buffer);
    CHECK(surface && pixels);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {8, 8, 1}};
    vk_surf_use(vk_cmd(), surface, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), buffer, 1, &copy);
    vk_finish();
    vk_readback_ready();
    memcpy(rgba, pixels, 16);
    return 0;
}

static int check_host_program(void) {
    static const char vs[] = "#version 300 es\nlayout(location = 0) in vec4 position;\n"
                             "void main() { gl_Position = position; }\n";
    static const char fs[] = "#version 300 es\nprecision highp float;\nuniform vec4 pick;\n"
                             "uniform Data { mat3 m; layout(row_major) mat2 r; mat2 pair[3]; float s; };\n"
                             "out vec4 frag;\n"
                             "void main() {\n"
                             "  frag = vec4(m[1].y, r[1].x * 100.0 + r[0].y, pair[int(pick.x)][1].y, s * pick.w);\n"
                             "}\n";
    const u64 gpu = 0x4c6000000ull, cpu = HEAP_BASE + 0x720000;
    const u64 position = gpu + 0x2000, data = gpu + 0x3000, pick = gpu + 0x3100, again = gpu + 0x3400;
    const u64 target = gpu + 0x8000;
    const f32 triangle[3][4] = {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}}, picked[4] = {2, 0, 0, 3};
    const f32 transform[6] = {4, 4, 0.5f, 4, 4, 0.5f};
    /* Data's std140 bytes, word j holding j + 0.5 (100 more in the second draw's): m[1].y is word 5, r[1].x (row 0,
       column 1) word 13 and r[0].y (row 1, column 0) word 16, pair[2][1].y (32-byte elements from byte 80) word 41, s
       word 44 */
    f32 words[48], more[48], rgba[4];
    for (u32 j = 0; j < 48; j++) words[j] = (f32)j + 0.5f, more[j] = words[j] + 100;
    SnxGlLayout layout = gl_host_program_layout();
    snx_gl_program_host(&(SnxGlProgramHost){.stage_sets = 1, .layout = layout});
    u32 ctx = snx_gl_context_new(SNX_GL_API_ES, 30, 0), name = snx_gl_create_program(ctx);
    snx_gl_attach_shader(ctx, name, host_gl_shader(ctx, 0x8b31, vs));
    snx_gl_attach_shader(ctx, name, host_gl_shader(ctx, 0x8b30, fs));
    snx_gl_link_program(ctx, name);
    const SnxGlProgram* program = snx_gl_program(name);
    if (!program || !program->linked) fprintf(stderr, "host program: %s\n", snx_gl_program_info_log(ctx, name));
    CHECK(program && program->linked);
    ShaderInfo vs_info, fs_info;
    CHECK(!gl_host_shader_info(program, &layout, 0, &vs_info) && !gl_host_shader_info(program, &layout, 1, &fs_info));
    CHECK(fs_info.cb_window == (g_spirv_translate.cb_windows != 0));
    u32 slot = 0;
    for (u32 i = 0; i < program->binding_count; i++) {
        const SnxGlBinding* b = &snx_gl_program_bindings(program)[i];
        if (b->kind == SNX_GLSLC_REFLECT_UNIFORM_BLOCK && b->name) slot = b->binding - layout.block_base;
    }
    CHECK(slot >= 1 && slot < DS_CBUFS && (fs_info.cbuf_mask >> slot & 1));
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 121;
    gmmu_map(gpu, cpu, 0x10000);
    CHECK(!gmem_write(position, triangle, sizeof triangle));
    CHECK(!gmem_write(data, words, sizeof words));
    CHECK(!gmem_write(again, more, sizeof more));
    CHECK(!gmem_write(pick, picked, sizeof picked));
    u32* r = channel->r3d;
    r[0x850] = 1; /* the fragment program's slot on */
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
    r[0x7c0] = (u32)((position + sizeof triangle - 1) >> 32);
    r[0x7c1] = (u32)(position + sizeof triangle - 1);
    vk_lock();
    u32 vs_id = vk_host_program(ST_VS, snx_gl_program_spirv(program, 0), program->spirv_words[0], &vs_info);
    u32 fs_id = vk_host_program(ST_FS, snx_gl_program_spirv(program, 1), program->spirv_words[1], &fs_info);
    CHECK(vs_id && fs_id && vs_id != fs_id);
    /* the same module, the same id */
    CHECK(vk_host_program(ST_FS, snx_gl_program_spirv(program, 1), program->spirv_words[1], &fs_info) == fs_id);
    int is_depth;
    rt_clear_format(channel, target, 8, 8, 0, 1, nvn_rt_format(NVN_FORMAT_RGBA32F, &is_depth));
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    DsState* state = &channel->ds;
    state->prog.code[1] = DS_HOST_PROGRAM | vs_id;
    state->prog.code[5] = DS_HOST_PROGRAM | fs_id;
    state->gen[DS_PROGRAMS] = ds_generation_next();
    state->bind.cb_addr[4][0] = pick;
    state->bind.cb_size[4][0] = sizeof picked;
    state->bind.cb_addr[4][slot] = data;
    state->bind.cb_size[4][slot] = sizeof words;
    state->gen[DS_BIND] = ds_generation_next();
    DrawCall call = {.topology = 4, .count = 3, .instances = 1};
    DrawClear clear = {.color_mask = 15};
    vk_render_clear(state, &clear, &channel->owner);
    vk_render_draw(state, &call, &channel->owner);
    vk_render_end();
    CHECK(!host_program_pixel(target, rgba));
    if (rgba[0] != 5.5f || rgba[1] != 1366.5f || rgba[2] != 41.5f || rgba[3] != 133.5f)
        fprintf(stderr, "host program: pixel (%g, %g, %g, %g), not (5.5, 1366.5, 41.5, 133.5)\n", (double)rgba[0],
                (double)rgba[1], (double)rgba[2], (double)rgba[3]);
    CHECK(rgba[0] == 5.5f && rgba[1] == 1366.5f && rgba[2] == 41.5f && rgba[3] == 133.5f);
    /* two draws in one frame, the second with the block's bytes elsewhere and the default block's range wider: with
       windows, each bank's second copy lands in the window of its first at an offset of its own */
    vk_render_clear(state, &clear, &channel->owner);
    vk_render_draw(state, &call, &channel->owner);
    state->bind.cb_addr[4][slot] = again;
    state->bind.cb_size[4][0] = 64;
    state->gen[DS_BIND] = ds_generation_next();
    vk_render_draw(state, &call, &channel->owner);
    vk_render_end();
    CHECK(!host_program_pixel(target, rgba));
    if (rgba[0] != 105.5f || rgba[1] != 11466.5f || rgba[2] != 141.5f || rgba[3] != 433.5f)
        fprintf(stderr, "host program: again, pixel (%g, %g, %g, %g), not (105.5, 11466.5, 141.5, 433.5)\n",
                (double)rgba[0], (double)rgba[1], (double)rgba[2], (double)rgba[3]);
    CHECK(rgba[0] == 105.5f && rgba[1] == 11466.5f && rgba[2] == 141.5f && rgba[3] == 433.5f);
    /* a vertex program the renderer wasn't given: the draw is skipped, the target keeps its clear */
    u64 skipped = g_vk_skipped;
    state->prog.code[1] = DS_HOST_PROGRAM | 4000;
    state->gen[DS_PROGRAMS] = ds_generation_next();
    vk_render_draw(state, &call, &channel->owner);
    vk_render_end();
    CHECK(g_vk_skipped > skipped);
    vk_unlock();
    free(channel);
    snx_gl_program_host(NULL);
    printf("PASS host programs: a GL program linked by the layer in this device's rules (%s) draws a uniform block's "
           "matrices, an array by an index and a scalar; a program the renderer wasn't given is skipped\n",
           g_spirv_translate.cb_windows ? "constant buffers in windows" : "constant buffers bound one by one");
    return 0;
}
