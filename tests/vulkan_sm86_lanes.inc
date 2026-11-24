/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An Ounce compute program that reads its warp's other lanes, through the renderer's own translation
   (vk_translate_spirv), which says what the device's subgroups give the stage: a combined add has no subgroup operation
   in its module where the compute stage has none of 32 lanes, and has them where it does; a reduction over the warp is
   refused there, with the instruction, the lanes the device has and the 32 a warp has, and is translated where the stage
   has them. No extracted shaders or game data are used. */
static u32* sm86_lanes_module(const Sm86Word* words, u32 n, VkShaderStageFlags stages, size_t* count, ShaderInfo* si) {
    u8 code[16 * 32] = {0};
    for (u32 i = 0; i < n; i++) sm86_put(code, i, words[i]);
    sm86_put(code, n, sm86_exit());
    sm86_put(code, n + 1, sm86_bra(n + 1, n + 1, 7, 0));
    *si = (ShaderInfo){0};
    si->cs_local[0] = 32, si->cs_local[1] = si->cs_local[2] = 1;
    const VkShaderStageFlags was = g_vk_subgroup_stages;
    const int sm86_was = g_shader_sm86;
    g_vk_subgroup_stages = stages;
    g_shader_sm86 = 1;
    u32* spv = vk_translate_spirv(code, 16 * (n + 2), ST_CS, si, count, NULL, NULL);
    g_shader_sm86 = sm86_was;
    g_vk_subgroup_stages = was;
    return spv;
}
static int sm86_lanes_subgroups(const u32* spv, size_t count) {
    for (size_t at = 5; at < count && spv[at] >> 16; at += spv[at] >> 16)
        if ((spv[at] & 0xffff) == 17 && spv[at + 1] >= 61 && spv[at + 1] <= 68) return 1;
    return 0;
}
static int check_sm86_lanes(void) {
    const Sm86Word ballot[2] = {
        {{0x0000000000047886ull, 0x000fe400038e0100ull}}, /* VOTEU.ANY UR4 = the running lanes */
        {{0x0000000400157d09ull, 0x000e220008000000ull}}, /* POPC R21, UR4, which nothing reads */
    };
    const Sm86Word reduction[1] = {{{0x00000000010673c4ull, 0x000e220000014000ull}}}; /* REDUX UR6, R1 */
    const VkShaderStageFlags with = g_vk_subgroup_stages | VK_SHADER_STAGE_COMPUTE_BIT,
                             without = g_vk_subgroup_stages & ~(VkShaderStageFlags)VK_SHADER_STAGE_COMPUTE_BIT;
    size_t count = 0;
    ShaderInfo si;
    u32* spv = sm86_lanes_module(ballot, 2, without, &count, &si);
    CHECK(spv && !sm86_lanes_subgroups(spv, count));
    free(spv);
    spv = sm86_lanes_module(ballot, 2, with, &count, &si);
    CHECK(spv && sm86_lanes_subgroups(spv, count));
    free(spv);
    spv = sm86_lanes_module(reduction, 1, with, &count, &si);
    CHECK(spv);
    free(spv);
    spv = sm86_lanes_module(reduction, 1, without, &count, &si);
    char lanes[32], note[32];
    snprintf(lanes, sizeof lanes, " (%u lanes of 32)", g_vk_subgroup_lanes);
    const int said =
        !spv && si.unsupported && strstr(si.first_unsupported, "REDUX at 0") && !strcmp(vk_lane_note(note, &si), lanes);
    if (!said)
        fprintf(stderr, "sm86 lanes: a reduction without a warp: %s%s\n", spv ? "translated" : si.first_unsupported,
                note);
    CHECK(said);
    ShaderInfo other = {0};
    snprintf(other.first_unsupported, sizeof other.first_unsupported, "missing exit");
    CHECK(!*vk_lane_note((char[32]){0}, &other));
    printf(
        "PASS sm86 lanes: a ballot is translated with and without compute subgroups, a reduction refused without: %s\n",
        si.first_unsupported);
    return 0;
}
