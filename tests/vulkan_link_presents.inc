/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */

/* A link's optimized pipeline is made once the link was drawn in PIPE_OPT_PRESENTS presents (pipeline.c's
   vk_pipeline_bound), not as soon as the link is: each optimized one is the pipeline compiled again, and a first run
   queued one for every link, those of a scene drawn once too. Its draws keep the link's stand-in, which binds the
   optimized pipeline once a worker made it */
static int check_link_optimized_after_presents(void) {
    if (!g_vk.has_gpl || g_vk.has_shader_objects || g_vk.gpl_hold || !vk_rec_threaded()) {
        printf("SKIP links optimized after %u presents: %s\n", PIPE_OPT_PRESENTS,
               !g_vk.has_gpl             ? "pipelines are not linked from parts"
               : g_vk.has_shader_objects ? "draws bind shader objects"
               : g_vk.gpl_hold           ? "every link stays (SNX_TEST_GPL=linked)"
                                         : "no recording thread");
        return 0;
    }
    size_t words = 0;
    u32* code = snx_test_spirv_graphics(0, &words);
    CHECK(code);
    /* modules of their own: the parts are new, and the pipeline is a link of them */
    VkShaderModule vs = vk_module_spv(code, words);
    VkShaderModule fs = vk_module_spv(snx_null_fs_spv, snx_null_fs_spv_size / sizeof *snx_null_fs_spv);
    free(code);
    CHECK(vs && fs);
    PipeKey k = {.vs = vs,
                 .fs = fs,
                 .topo_class = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                 .ncolor = 1,
                 .color = {VK_FORMAT_R8G8B8A8_UNORM}};
    k.blend[0].mask = 15;
    vk_pipeline_quiesce();
    u64 made[4], hits[4], make_ms[4], links, link_ms, opts, opt_ms, links_now, opts_now;
    vk_pipeline_lib_stats(made, hits, make_ms, &links, &link_ms, &opts, &opt_ms);
    const u64 asked = vk_pipeline_opt_asked();
    vk_lock();
    VkPipeline s = vk_pipeline_get_deferred(&k);
    vk_unlock();
    CHECK(s);
    VkPipeline link = vk_pipeline_resolve(s); /* the worker's link, waited for */
    CHECK(link && link != s);
    vk_pipeline_lib_stats(made, hits, make_ms, &links_now, &link_ms, &opts_now, &opt_ms);
    CHECK(links_now == links + 1 && opts_now == opts);
    /* bound twice in each of the presents before the last: each counts once, and the link stays */
    vk_lock();
    for (u32 i = 0; i + 1 < PIPE_OPT_PRESENTS; i++) {
        vk_pipeline_bound(s);
        vk_pipeline_bound(s);
        g_vk_present++;
    }
    vk_unlock();
    CHECK(vk_pipeline_opt_asked() == asked);
    /* a worker that took the link's optimized pipeline at once has replaced the link by now (lavapipe makes one in
       milliseconds) */
    for (int i = 0; i < 50 && vk_pipeline_ready(s) == link; i++) plat_sleep_ns(10000000);
    vk_pipeline_lib_stats(made, hits, make_ms, &links_now, &link_ms, &opts_now, &opt_ms);
    if (vk_pipeline_ready(s) != link || opts_now != opts)
        fprintf(stderr, "links optimized after %u presents: the link drawn in %u was optimized (%llu optimized made)\n",
                PIPE_OPT_PRESENTS, PIPE_OPT_PRESENTS - 1, (unsigned long long)(opts_now - opts));
    CHECK(vk_pipeline_ready(s) == link && opts_now == opts);
    /* the last present: its optimized pipeline is asked for, made, and the stand-in binds it */
    vk_lock();
    vk_pipeline_bound(s);
    vk_unlock();
    CHECK(vk_pipeline_opt_asked() == asked + 1);
    for (int i = 0; i < 1000 && vk_pipeline_ready(s) == link; i++) plat_sleep_ns(10000000);
    vk_pipeline_lib_stats(made, hits, make_ms, &links_now, &link_ms, &opts_now, &opt_ms);
    CHECK(vk_pipeline_ready(s) != link && opts_now == opts + 1);
    printf("PASS links optimized after %u presents: the link drawn in %u stayed, the one drawn in %u was optimized\n",
           PIPE_OPT_PRESENTS, PIPE_OPT_PRESENTS - 1, PIPE_OPT_PRESENTS);
    return 0;
}

/* With the blending as dynamic state (g_vk.dyn_blend, vk_pipe_blend_key), draws of one program and state that blend
   differently take one pipeline, or one set of shader objects: the same handle, where each blend made its own */
static int check_blend_one_pipeline(void) {
    if (!g_vk.dyn_blend) {
        puts("SKIP one pipeline for every blend: the blending is no dynamic state on this device");
        return 0;
    }
    size_t words = 0;
    u32* code = snx_test_spirv_graphics(0, &words);
    CHECK(code);
    VkShaderModule vs = vk_module_spv(code, words);
    if (vs) vk_so_keep_spirv(vs, code, words);
    free(code);
    const size_t fs_words = snx_null_fs_spv_size / sizeof *snx_null_fs_spv;
    VkShaderModule fs = vk_module_spv(snx_null_fs_spv, fs_words);
    CHECK(vs && fs);
    vk_so_keep_spirv(fs, snx_null_fs_spv, fs_words);
    PipeKey a = {.vs = vs,
                 .fs = fs,
                 .topo_class = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                 .ncolor = 1,
                 .color = {VK_FORMAT_R8G8B8A8_UNORM}},
            b;
    a.blend[0].mask = 15;
    b = a;
    b.blend[0].en = 1;
    b.blend[0].sc = VK_BLEND_FACTOR_SRC_ALPHA;
    b.blend[0].dc = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    b.blend[0].mask = 7;
    /* the pipelines compared, not the handles a read gets: a stand-in where it is still being made, the pipeline once
       it is (vk_pipeline_ready), so a's read and b's may differ for one pipeline; and the link stays (gpl_hold), as its
       optimized pipeline would take its place between the two. The workers' done between the reads, as a slow
       device's are */
    const int hold = g_vk.gpl_hold;
    g_vk.gpl_hold = 1;
    vk_lock();
    VkPipeline pa = vk_pipeline_get_deferred(&a);
    vk_unlock();
    vk_pipeline_quiesce();
    vk_lock();
    VkPipeline pb = vk_pipeline_get_deferred(&b);
    vk_unlock();
    CHECK(pa && pb);
    VkPipeline ra = vk_pipeline_resolve(pa), rb = vk_pipeline_resolve(pb);
    g_vk.gpl_hold = hold;
    if (ra != rb)
        fprintf(stderr, "one pipeline for every blend: two blends of one draw state made two pipelines, %p and %p\n",
                (void*)ra, (void*)rb);
    CHECK(ra && ra == rb); /* made, with the blend as dynamic state */
    puts("PASS one pipeline for every blend: two blends of one draw state bind one pipeline");
    return 0;
}
