/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Pipelines linked from parts (pipeline.c, g_vk.has_gpl), through the draws' own path (vk_pipeline_get_deferred and
   the handles the recording thread resolves): a new pipeline is handed as a stand-in that binds the link of its four
   libraries; a second pipeline that differs only in its target's format or its fragment program makes only the
   libraries of the parts that differ; the pipeline made whole replaces the link once a worker has made
   it, and the handle a draw kept binds it from then on; without the extension (has_gpl off) a new pipeline is made
   whole and no library is made. None of it may raise a validation message. The pixels a linked pipeline draws are
   every other check's, in the run that binds linked pipelines only (SNX_TEST_GPL=linked) */
#include "nx/gpu/vk/internal_spv.h"

static int check_pipeline_library(void) {
    if (!vk_rec_threaded()) {
        puts("SKIP pipeline libraries: draws go without the recording thread, which the stand-ins are for");
        return 0;
    }
    const int had = g_vk.has_gpl, held = g_vk.gpl_hold;
    const u32 validation = g_vk_validation_messages;
    VkShaderModule vs = vk_module_spv(snx_clear_vs_spv, sizeof snx_clear_vs_spv / sizeof *snx_clear_vs_spv);
    VkShaderModule fs =
        vk_module_spv(snx_clear_fs_float_spv, sizeof snx_clear_fs_float_spv / sizeof *snx_clear_fs_float_spv);
    CHECK(vs && fs);
    PipeKey k;
    memset(&k, 0, sizeof k); /* keys compare every byte */
    k.vs = vs;
    k.fs = fs;
    k.topo_class = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    k.polygon = VK_POLYGON_MODE_FILL;
    k.ncolor = 1;
    k.color[0] = VK_FORMAT_R8G8B8A8_UNORM;
    k.samples = VK_SAMPLE_COUNT_1_BIT;
    k.blend[0].mask = 0xF;
    k.blend[0].sc = k.blend[0].sa = VK_BLEND_FACTOR_ONE;
    k.blend[0].cop = k.blend[0].aop = VK_BLEND_OP_ADD;
    VkGplStats s0, s1;
    VkPipeline h, got;
    int pending;
    if (had) {
        /* held: a linked pipeline is never made whole, so each handle stays the link's */
        g_vk.gpl_hold = 1;
        vk_pipeline_library_stats(&s0);
        vk_lock();
        h = vk_pipeline_get_deferred(&k);
        vk_unlock();
        CHECK(h);
        got = vk_pipeline_resolve(h);
        CHECK(got && got != h);           /* the link, waited for at the bind */
        CHECK(vk_pipeline_ready(h) == h); /* not made whole */
        CHECK(vk_pipeline_now(h, &pending) == got && !pending);
        vk_pipeline_library_stats(&s1);
        CHECK(s1.parts == s0.parts + 4 && s1.links == s0.links + 1 && s1.whole == s0.whole);
        /* the same programs with two other target formats, and with no fragment program: one fragment output library
           for each of the first two, the empty fragment program's library and its output (no color writes) for the
           third. Formats, as blending may be dynamic state, in no key */
        PipeKey other[3] = {k, k, k};
        other[0].color[0] = VK_FORMAT_B8G8R8A8_UNORM;
        other[1].color[0] = VK_FORMAT_R16G16B16A16_SFLOAT;
        other[2].fs = VK_NULL_HANDLE;
        const u64 parts[3] = {1, 1, 2};
        for (int i = 0; i < 3; i++) {
            VkGplStats before, after;
            vk_pipeline_library_stats(&before);
            vk_lock();
            VkPipeline o = vk_pipeline_get_deferred(&other[i]);
            vk_unlock();
            CHECK(o && vk_pipeline_resolve(o) && vk_pipeline_resolve(o) != got);
            vk_pipeline_library_stats(&after);
            CHECK(after.parts == before.parts + parts[i] && after.links == before.links + 1);
        }
        /* let go: a new pipeline's link is made whole after it, and both the stand-in a draw kept and a new ask bind
           the pipeline made whole; on a CPU device (gpl_whole off) the link stays */
        g_vk.gpl_hold = 0;
        PipeKey later = k;
        later.color[0] = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        vk_pipeline_library_stats(&s0);
        vk_lock();
        h = vk_pipeline_get_deferred(&later);
        vk_unlock();
        CHECK(h && vk_pipeline_resolve(h));
        vk_pipeline_quiesce();
        vk_pipeline_library_stats(&s1);
        CHECK(s1.parts == s0.parts + 1 && s1.links == s0.links + 1 && !s1.whole_left);
        CHECK(s1.whole == s0.whole + (g_vk.gpl_whole ? 1u : 0u));
        got = vk_pipeline_ready(h);
        if (g_vk.gpl_whole) {
            CHECK(got != h && vk_pipeline_resolve(h) == got && vk_pipeline_now(h, &pending) == got && !pending);
            vk_lock();
            CHECK(vk_pipeline_get_deferred(&later) == got);
            vk_unlock();
        } else
            CHECK(got == h && vk_pipeline_resolve(h) && vk_pipeline_now(h, &pending) == vk_pipeline_resolve(h));
        printf("PASS pipeline libraries: linked from parts, a part made once for the pipelines that share it, made "
               "whole after\n");
    } else
        printf("SKIP pipeline libraries linked: the device doesn't link them (the log line says why)\n");
    /* without them: made whole, no library */
    g_vk.has_gpl = 0;
    PipeKey whole = k;
    whole.color[0] = VK_FORMAT_R16G16_SFLOAT;
    vk_pipeline_library_stats(&s0);
    vk_lock();
    h = vk_pipeline_get_deferred(&whole);
    vk_unlock();
    got = vk_pipeline_resolve(h);
    CHECK(got && vk_pipeline_ready(h) == got);
    vk_pipeline_library_stats(&s1);
    CHECK(s1.parts == s0.parts && s1.links == s0.links && s1.whole == s0.whole);
    g_vk.has_gpl = had;
    g_vk.gpl_hold = held;
    CHECK(g_vk_validation_messages == validation);
    puts("PASS pipelines made whole without pipeline libraries");
    return 0;
}
