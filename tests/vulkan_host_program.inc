/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Host programs (Renderer.host_program, vk/host_program.c): a vertex and a fragment program given as SPIR-V, compiled
   by glslc from GLSL with their resources at shader.h's bindings, drawn by a DsState that names them in
   DsPrograms.code[] (DS_HOST_PROGRAM) as the GL layer's draws do. The vertex program reads attribute 0, the fragment
   program writes the color of a uniform block bound as constant buffer 3 of its stage */

/* glslc's binding map: every block at constant buffer slot 3 of its stage's set */
static int host_program_bind(void* user, const GlslcResource* resource, uint32_t* set, uint32_t* binding) {
    int stage = *(const int*)user;
    if (resource->kind != SNX_GLSLC_REFLECT_UNIFORM_BLOCK) return -1;
    *set = (uint32_t)shader_vk_set(stage);
    *binding = SNX_VKB_CB + 3;
    return 0;
}
/* source as SPIR-V of stage (ST_VS or ST_FS) into words (capacity of them): its word count, 0 on an error (said) */
static size_t host_program_spirv(int stage, const char* source, u32* words, size_t capacity) {
    size_t size = snx_glslc_workspace_size();
    void* memory = malloc(size);
    GlslcWorkspace workspace;
    static GlslcResult result;
    GlslcOptions options = {.stage = stage == ST_VS ? SNX_GLSLC_VERTEX : SNX_GLSLC_FRAGMENT,
                            .bind = host_program_bind,
                            .bind_user = &stage};
    size_t made = 0;
    if (memory && !snx_glslc_workspace_init(&workspace, memory, size, &result) &&
        !snx_glslc_compile(&workspace, &options, source, strlen(source), words, capacity, &result))
        made = result.words;
    else
        fprintf(stderr, "host program: glslc: %s\n", result.error);
    free(memory);
    return made;
}

static int check_host_program(void) {
    static const char vs[] = "#version 300 es\nlayout(location = 0) in vec4 position;\n"
                             "void main() { gl_Position = position; }\n";
    static const char fs[] = "#version 300 es\nprecision mediump float;\nuniform Color { vec4 color; };\n"
                             "out vec4 frag;\nvoid main() { frag = color; }\n";
    const u64 gpu = 0x4c6000000ull, cpu = HEAP_BASE + 0x720000;
    const u64 position = gpu + 0x2000, color = gpu + 0x3000, target = gpu + 0x8000;
    const f32 triangle[3][4] = {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}}, red[4] = {1, 0, 0, 0.5f};
    const f32 transform[6] = {4, 4, 0.5f, 4, 4, 0.5f};
    static u32 words[2][4096];
    size_t vs_words = host_program_spirv(ST_VS, vs, words[0], 4096), fs_words = host_program_spirv(ST_FS, fs, words[1], 4096);
    CHECK(vs_words && fs_words);
    ShaderInfo vs_info = {.in_mask = 1}, fs_info = {.cbuf_mask = 1u << 3};
    fs_info.cbuf_max[3] = sizeof red;
    fs_info.sph[18] = 15; /* the header's output map: color target 0's four components */
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 121;
    gmmu_map(gpu, cpu, 0x10000);
    CHECK(!gmem_write(position, triangle, sizeof triangle));
    CHECK(!gmem_write(color, red, sizeof red));
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
    u32 vs_id = vk_host_program(ST_VS, words[0], vs_words, &vs_info);
    u32 fs_id = vk_host_program(ST_FS, words[1], fs_words, &fs_info);
    CHECK(vs_id && fs_id && vs_id != fs_id);
    CHECK(vk_host_program(ST_FS, words[1], fs_words, &fs_info) == fs_id); /* the same module, the same id */
    int is_depth;
    rt_clear_format(channel, target, 8, 8, 0, 1, nvn_rt_format(NVN_FORMAT_RGBA32F, &is_depth));
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    DsState* state = &channel->ds;
    state->prog.code[1] = DS_HOST_PROGRAM | vs_id;
    state->prog.code[5] = DS_HOST_PROGRAM | fs_id;
    state->gen[DS_PROGRAMS] = ds_generation_next();
    state->bind.cb_addr[4][3] = color;
    state->bind.cb_size[4][3] = sizeof red;
    state->gen[DS_BIND] = ds_generation_next();
    DrawCall call = {.topology = 4, .count = 3, .instances = 1};
    CHECK(!nvn_state_pixels(state, &call, &channel->owner, target, 64));
    /* a vertex program the renderer wasn't given: the draw is skipped, the target keeps its clear */
    u64 skipped = g_vk_skipped;
    state->prog.code[1] = DS_HOST_PROGRAM | 4000;
    state->gen[DS_PROGRAMS] = ds_generation_next();
    CHECK(!nvn_state_pixels(state, &call, &channel->owner, target, 0));
    CHECK(g_vk_skipped > skipped);
    vk_unlock();
    free(channel);
    puts("PASS host programs: a GLSL vertex and fragment program given as SPIR-V draw a triangle over the target in "
         "a uniform block's color; a program the renderer wasn't given is skipped");
    return 0;
}
