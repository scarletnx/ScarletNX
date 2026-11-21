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
