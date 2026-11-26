/* pipeline_records (gpu/vk/pipeline_record.c), run with --pipeline-records=DIR: a graphics pipeline the driver fails on
   purpose (vk_pipeline_record_inner) is on disk before the driver is called, with the thread's note naming its folder,
   so a crash inside the call leaves it; the failure keeps it whole (pipeline.txt, device.txt and the SPIR-V of both
   stages), and the replay makes the same pipeline from it on a device of its own. A pipeline the driver makes leaves no
   record. The pipeline is the masked clear's (its push constants in both stages), made through volk's commands as the
   renderer makes its own */
#include "nx/gpu/vk/pipeline_record.h"
#include "nx/gpu/vk/internal_spv.h" /* snx_clear_vs_spv, snx_clear_fs_float_spv */

static struct {
    int calls, saw_record;
    char folder[1200], made_folder[1200];
    PFN_vkCreateGraphicsPipelines driver;
} g_records_check;

/* the record's folder from the thread's note, as a crash report would show it */
static void records_note_folder(char* out, size_t n) {
    const char* at = strstr(plat_thread_note_get(), "its record in ");
    snprintf(out, n, "%s", at ? at + 14 : "");
}
static VkResult VKAPI_CALL records_failing_make(VkDevice dev, VkPipelineCache cache, uint32_t count,
                                                const VkGraphicsPipelineCreateInfo* infos,
                                                const VkAllocationCallbacks* alloc, VkPipeline* out) {
    (void)dev, (void)cache, (void)infos, (void)alloc;
    g_records_check.calls++;
    records_note_folder(g_records_check.folder, sizeof g_records_check.folder);
    char* text = snx_path_join(g_records_check.folder, "pipeline.txt");
    g_records_check.saw_record = g_records_check.folder[0] && text && snx_is_file(text);
    free(text);
    for (uint32_t i = 0; i < count; i++) out[i] = VK_NULL_HANDLE;
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}
static VkResult VKAPI_CALL records_watched_make(VkDevice dev, VkPipelineCache cache, uint32_t count,
                                                const VkGraphicsPipelineCreateInfo* infos,
                                                const VkAllocationCallbacks* alloc, VkPipeline* out) {
    records_note_folder(g_records_check.made_folder, sizeof g_records_check.made_folder);
    return g_records_check.driver(dev, cache, count, infos, alloc, out);
}
/* whether the record's folder holds name */
static int records_has(const char* folder, const char* name) {
    char* path = snx_path_join(folder, name);
    int has = path && snx_is_file(path);
    if (!has) fprintf(stderr, "pipeline records: %s has no %s\n", folder, name);
    free(path);
    return has;
}
static int check_pipeline_records(void) {
    if (!g_settings.pipeline_records[0]) {
        puts("SKIP pipeline records: run without --pipeline-records");
        return 0;
    }
    CHECK(records_has(g_settings.pipeline_records, "device.txt")); /* the device, as the renderer made it */
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    CHECK(snx_vk_shader_module(g_vk.dev, snx_clear_vs_spv, snx_clear_vs_spv_size, &vs) == VK_SUCCESS);
    CHECK(snx_vk_shader_module(g_vk.dev, snx_clear_fs_float_spv, snx_clear_fs_float_spv_size, &fs) == VK_SUCCESS);
    VkPushConstantRange push = {.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, .size = 32};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    CHECK(vkCreatePipelineLayout(g_vk.dev, &li, NULL, &layout) == VK_SUCCESS);
    VkPipelineShaderStageCreateInfo st[2] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                              .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                              .module = vs,
                                              .pName = "main"},
                                             {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                              .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                              .module = fs,
                                              .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL,
                                                 .depthBiasClamp = 0.25f,
                                                 .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState att = {.colorWriteMask = 0xf};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1,
                                              .pAttachments = &att,
                                              .blendConstants = {0.5f, 0.25f, 1.0f / 3.0f, 1.0f}};
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    const VkFormat color = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &color};
    VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                       .pNext = &ri,
                                       .stageCount = 2,
                                       .pStages = st,
                                       .pVertexInputState = &vi,
                                       .pInputAssemblyState = &ia,
                                       .pViewportState = &vp,
                                       .pRasterizationState = &rs,
                                       .pMultisampleState = &ms,
                                       .pColorBlendState = &cb,
                                       .pDynamicState = &dy,
                                       .layout = layout,
                                       .basePipelineIndex = -1};
    VkPipeline p = VK_NULL_HANDLE;
    g_records_check.driver = vk_pipeline_record_inner(records_failing_make);
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, VK_NULL_HANDLE, 1, &gi, NULL, &p);
    vk_pipeline_record_inner(g_records_check.driver);
    CHECK(r == VK_ERROR_OUT_OF_DEVICE_MEMORY && g_records_check.calls == 1 && !p);
    if (!g_records_check.saw_record)
        fprintf(stderr, "pipeline records: no pipeline.txt in \"%s\" while the driver made it\n",
                g_records_check.folder);
    CHECK(g_records_check.saw_record);
    CHECK(!strcmp(plat_thread_note_get(), "")); /* the note as it was before the call */
    char vs_name[40], fs_name[40];
    snprintf(vs_name, sizeof vs_name, "%016llx.spv",
             (unsigned long long)snx_fnv1a(SNX_FNV_BASIS, snx_clear_vs_spv, (size_t)snx_clear_vs_spv_size));
    snprintf(fs_name, sizeof fs_name, "%016llx.spv",
             (unsigned long long)snx_fnv1a(SNX_FNV_BASIS, snx_clear_fs_float_spv, (size_t)snx_clear_fs_float_spv_size));
    CHECK(records_has(g_records_check.folder, "pipeline.txt") && records_has(g_records_check.folder, "device.txt") &&
          records_has(g_records_check.folder, vs_name) && records_has(g_records_check.folder, fs_name));
    /* the record alone makes the pipeline again: every state it was made with, read back */
    VkResult made = VK_ERROR_UNKNOWN;
    char why[1024];
    int replayed = vk_pipeline_replay(g_vk.instance, g_records_check.folder, &made, why, sizeof why);
    if (replayed) fprintf(stderr, "pipeline records: the replay of %s: %s\n", g_records_check.folder, why);
    CHECK(!replayed);
    if (made != VK_SUCCESS)
        fprintf(stderr, "pipeline records: the replay's driver answered %s\n", snx_vk_result_str(made));
    CHECK(made == VK_SUCCESS);
    /* a pipeline the driver makes: its record is taken away */
    g_records_check.driver = vk_pipeline_record_inner(records_watched_make);
    r = vkCreateGraphicsPipelines(g_vk.dev, VK_NULL_HANDLE, 1, &gi, NULL, &p);
    vk_pipeline_record_inner(g_records_check.driver);
    CHECK(r == VK_SUCCESS && p);
    CHECK(g_records_check.made_folder[0] && !snx_is_dir(g_records_check.made_folder));
    vkDestroyPipeline(g_vk.dev, p, NULL);
    vkDestroyPipelineLayout(g_vk.dev, layout, NULL);
    vkDestroyShaderModule(g_vk.dev, vs, NULL);
    vkDestroyShaderModule(g_vk.dev, fs, NULL);
    printf("pipeline_records: a failed pipeline kept whole in %s and made again by the replay; a made one's taken "
           "away\n",
           g_records_check.folder);
    return 0;
}
