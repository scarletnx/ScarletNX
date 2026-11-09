/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An Ounce's mesh draws (render.c's DrawCall.mesh, VK_EXT_mesh_shader). A mesh program alone writes one triangle that
   covers an 8x8 depth target at depth 0.5, and the draw's depth test keeps it. Then a task program launches as many
   mesh groups as its workgroup index, which counts from the draw's first workgroup (the push constant's x): the draw
   from 0 launches none, and the one from 1 launches a mesh program after it, whose triangle is at depth 0.25. The
   exact depths catch a pipeline without the mesh or task stage, sets or push constants the programs don't read, and
   a first workgroup lost on the way */

/* a task or mesh program at gva: its SPH, word 0 sph0 and word 13 of the output map omap13, and its code */
static int mesh_draw_program(u64 gva, u32 sph0, u32 omap13, const Sm86Word* code, u32 n) {
    u8 bytes[SM86_SPH_BYTES + 16 * 32] = {0};
    snx_st_le32(bytes, sph0);
    snx_st_le32(bytes + 13 * 4, omap13);
    for (u32 i = 0; i < n; i++) sm86_put(bytes + SM86_SPH_BYTES, i, code[i]);
    sm86_put(bytes + SM86_SPH_BYTES, n, sm86_bra(n, n, 7, 0)); /* the BRA to itself after the EXIT */
    return gmem_write(gva, bytes, SM86_SPH_BYTES + 16 * (n + 1));
}
/* a mesh program's code: one triangle (-1, -1), (3, -1), (-1, 3) at depth z, as 4 position words a vertex in groups of
   0x80 bytes (the vertex output area of a program of at most 32 vertices), its indices and the count 1 */
static u32 mesh_draw_triangle(Sm86Word* code, float z) {
    u32 n = 0, minus = 0xbf800000u, three = 0x40400000u, one = 0x3f800000u, depth;
    memcpy(&depth, &z, sizeof depth);
    code[n++] = sm86_mov_imm(1, minus);
    code[n++] = sm86_mov_imm(2, three);
    code[n++] = sm86_mov_imm(3, depth);
    code[n++] = sm86_mov_imm(4, one);
    static const u8 xy[3][2] = {{1, 1}, {2, 1}, {1, 2}}; /* the registers of each vertex's x and y */
    for (u32 v = 0; v < 3; v++) {
        const u8 regs[4] = {xy[v][0], xy[v][1], 3, 4};
        for (u32 k = 0; k < 4; k++)
            code[n++] = sm86_isbe(1, regs[k], 255, 4 * v + 0x80 * k, SM86_ISBE_B32, SM86_ISBE_ATTR, 1, 1);
    }
    code[n++] = sm86_mov_imm(5, 1);
    code[n++] = sm86_mov_imm(6, 2);
    static const u8 index_regs[3] = {255, 5, 6}; /* vertices 0 (RZ), 1 and 2 */
    for (u32 i = 0; i < 3; i++) code[n++] = sm86_isbe(1, index_regs[i], 255, 4 + i, SM86_ISBE_U8, SM86_ISBE_MAP, 0, 1);
    code[n++] = sm86_isbe(1, 5, 255, 3, SM86_ISBE_B32, SM86_ISBE_MAP, 0, 1); /* 1 primitive */
    code[n++] = sm86_exit();
    return n;
}
/* the depth target's 64 depths, each expected */
static int mesh_draw_depths(u64 target, float expected, const char* what) {
    Surf* surface = vk_rt_find(target);
    VkBuffer buffer;
    u8* depths = vk_readback_buffer(64 * 4, &buffer);
    CHECK(surface && depths);
    if (!surface || !depths) return 1;
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, .imageExtent = {8, 8, 1}};
    vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    vk_finish();
    vk_readback_ready();
    int bad = 0;
    for (u32 i = 0; i < 64; i++) {
        float got;
        memcpy(&got, depths + 4 * i, sizeof got);
        if (got != expected && !bad++)
            fprintf(stderr, "vulkan_headless: mesh draws (%s): depth %g at (%u, %u), not %g\n", what, (double)got,
                    i % 8, i / 8, (double)expected);
    }
    CHECK(!bad);
    return bad != 0;
}
static int check_mesh_draws(void) {
    if (!g_vk.has_mesh || !g_vk.has_task) {
        puts("SKIP mesh draws: mesh_draws is off, or the device has no mesh and task shaders with subgroups of 32");
        return 0;
    }
    /* both mesh stages see the sets of both (vk_set_stages), and the other stages see only their own */
    const VkShaderStageFlags both = VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_TASK_BIT_EXT;
    CHECK((vk_set_stages(shader_vk_set(ST_MS)) & both) == both && (vk_set_stages(shader_vk_set(ST_TS)) & both) == both);
    CHECK(!(vk_set_stages(shader_vk_set(ST_FS)) & both) && !(vk_set_stages(shader_vk_set(ST_GS)) & both));
    const u64 gpu = 0x4c8000000ull, cpu = HEAP_BASE + 0x6c0000, target = gpu + 0x8000;
    const u64 alone = gpu, task = gpu + 0x1000, after = gpu + 0x2000;
    gmmu_map(gpu, cpu, 0x10000);
    memset(GP(cpu), 0, 0x10000);
    Sm86Word code[32];
    u32 n = mesh_draw_triangle(code, 0.5f);
    const u32 position = 0xfu << 12;                                  /* the output map's bits 428 to 431 */
    CHECK(!mesh_draw_program(alone, 0x000c0481u, position, code, n)); /* alone: a vertex program's SPH */
    n = mesh_draw_triangle(code, 0.25f);
    CHECK(!mesh_draw_program(after, 0x020c0c81u, position, code, n)); /* after a task: an evaluation program's */
    /* the task program: its workgroup index is the count of mesh groups it launches */
    code[0] = sm86_isbe(0, 0, 255, 0, SM86_ISBE_B32, SM86_ISBE_ATTR, 1, 0);
    code[1] = sm86_isbe(1, 0, 255, 4, SM86_ISBE_B32, SM86_ISBE_ATTR, 0, 1);
    code[2] = sm86_exit();
    CHECK(!mesh_draw_program(task, 0x000c0481u, 0, code, 3));
    DsState* state = calloc(1, sizeof *state);
    CHECK(state);
    if (!state) return 1;
    DrawOwner owner = {0};
    state->owner = 0x80000000u;
    for (u32 g = 0; g < DS_GENS; g++) state->gen[g] = ds_generation_next(); /* no other draw's kept state */
    state->tgt.depth = (DsSurf){.gva = target, .width = 8, .height = 8, .format = 0x19, .layers = 1};
    state->vp.transform_on = 1;
    state->vp.scale[0][0] = state->vp.scale[0][1] = state->vp.translate[0][0] = state->vp.translate[0][1] = 4;
    state->vp.z_far[0] = 1;
    state->vp.swizzle[0] = 0x6420;
    state->rkey.sample_mask = ~0u; /* every sample, as the 3D engine starts (nvn_draw.c): pipelines apply it always */
    state->dyn.clip_height = 8;
    state->dyn.depth_mode = 1; /* near at 0: the positions' depths are the targets' */
    state->dyn.depth_test = state->dyn.depth_write = 1;
    state->dyn.depth_cmp = DS_CMP_LESS;
    state->prog.mesh = alone;
    state->prog.mesh_key = (DsMeshKey){
        .mesh_threads = 1, .mesh_payload = 0x80, .max_vertices = 3, .max_primitives = 1, .primitive_vertices = 3};
    int sm86 = g_shader_sm86;
    g_shader_sm86 = 1;
    vk_lock();
    DrawClear clear = {.aspects = 1, .depth = 1.0f};
    vk_render_clear(state, &clear, &owner);
    DrawCall call = {.mesh = 1, .topology = 4, .count = 1, .instances = 1};
    u64 drawn = g_vk_mesh_drawn;
    vk_render_draw(state, &call, &owner);
    vk_render_end();
    CHECK(g_vk_mesh_drawn == drawn + 1);
    CHECK(!mesh_draw_depths(target, 0.5f, "a mesh program alone"));
    /* the task and mesh programs: no mesh group from workgroup 0, one from workgroup 1 */
    state->prog.task = task;
    state->prog.mesh = after;
    state->prog.mesh_key = (DsMeshKey){.task_threads = 1,
                                       .task_payload = 0x20,
                                       .mesh_threads = 1,
                                       .mesh_payload = 0x20,
                                       .max_vertices = 3,
                                       .max_primitives = 1,
                                       .has_task = 1,
                                       .primitive_vertices = 3};
    state->gen[DS_PROGRAMS] = ds_generation_next();
    vk_render_draw(state, &call, &owner);
    vk_render_end();
    CHECK(!mesh_draw_depths(target, 0.5f, "a task program that launches no mesh group"));
    call.first = 1;
    vk_render_draw(state, &call, &owner);
    vk_render_end();
    CHECK(g_vk_mesh_drawn == drawn + 3);
    CHECK(!mesh_draw_depths(target, 0.25f, "a task program that launches one mesh group"));
    /* a mesh program's per-primitive SPH at the offset that its section gives, counted from the start of its code
       area (its own SPH included): generic 0.x per primitive */
    const u64 primitive = gpu + 0x3000, header = 0x200;
    n = mesh_draw_triangle(code, 0.5f);
    CHECK(!mesh_draw_program(primitive, 0x000c0481u, position, code, n));
    u8 sph[SM86_SPH_BYTES] = {0};
    snx_st_le32(sph, 0x010c1081u);
    snx_st_le32(sph + 13 * 4, 1u << 16);
    CHECK(!gmem_write(primitive + header, sph, sizeof sph));
    DsMeshKey key = {.mesh_threads = 1,
                     .mesh_payload = 0x80,
                     .max_vertices = 3,
                     .max_primitives = 1,
                     .primitive_vertices = 3,
                     .prim_sph = (u32)header};
    VkSh* shader = vk_shader_mesh(state, primitive, ST_MS, &key);
    CHECK(shader && !shader->failed && shader->si.prim_mask == 1 && shader->si.out_mask == 1);
    if (shader && shader->si.prim_mask != 1)
        fprintf(stderr, "vulkan_headless: a per-primitive SPH at %#llx: generics %#x per primitive, not 1\n",
                (unsigned long long)header, shader->si.prim_mask);
    /* a mesh program of 256 vertices with the position and all 32 generics: 33 locations of 16 bytes for each
       vertex, past the 32 KiB of maxMeshOutputMemorySize that a device has at least. It fails with the reason */
    const u64 large = gpu + 0x4000;
    CHECK(!mesh_draw_program(large, 0x000c0481u, 0xfffff000u, code, n));
    u8 omap[16];
    memset(omap, 0xff, sizeof omap);
    snx_st_le32(omap + 12, 0xffffu); /* generics 4 to 31: output map words 14 to 16, and bits 0 to 15 of word 17 */
    CHECK(!gmem_write(large + 14 * 4, omap, sizeof omap));
    key = (DsMeshKey){.mesh_threads = 1, .mesh_payload = 0x80, .max_vertices = 256, .max_primitives = 1,
                      .primitive_vertices = 3};
    shader = vk_shader_mesh(state, large, ST_MS, &key);
    CHECK(shader && shader->failed && strstr(shader->si.first_unsupported, "maxMeshOutputMemorySize"));
    if (shader && !strstr(shader->si.first_unsupported, "maxMeshOutputMemorySize"))
        fprintf(stderr, "vulkan_headless: a mesh program past the output limit: failed %d, \"%s\"\n", shader->failed,
                shader->si.first_unsupported);
    vk_unlock();
    g_shader_sm86 = sm86;
    free(state);
    puts("PASS mesh draws: a mesh program alone, task programs from their first workgroup, a per-primitive SPH, "
         "outputs past maxMeshOutputMemorySize, and the sets of the mesh stages exact=1");
    return 0;
}
