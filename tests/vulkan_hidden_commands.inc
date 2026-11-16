/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* vk_debug_hide_commands (the vulkan_headless_hidden_commands test): the check of each extension's commands after the
   device is made (gpu/vk/device.c) takes the hidden ones as missing, as from a driver that lists an extension without
   one of them, and the renderer runs on a device made again without that extension. log is what the renderer's start
   wrote. Each hidden command this knows is of an extension the first device would have had on where the driver lists
   it, its feature is there and its setting's first line says on; one that would not is said and left out, and with
   none left the run skips (77). Every other check then runs on the device made again */
static const struct {
    const char* command;
    const char* extension;
} g_hidden_known[] = {{"vkCmdSetDepthClampRangeEXT", VK_EXT_DEPTH_CLAMP_CONTROL_EXTENSION_NAME},
                      {"vkCmdBeginConditionalRenderingEXT", VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME}};

/* the line of log that has a, b after it in the same line too where b is given */
static const char* hidden_line(const char* log, const char* a, const char* b) {
    for (const char* at = strstr(log, a); at; at = strstr(at + 1, a)) {
        const char* end = strchr(at, '\n');
        const char* found = b ? strstr(at, b) : at;
        if (found && (!end || found < end)) return at;
    }
    return NULL;
}

/* 0 when each hidden command's extension went off as said, 77 when none would have been on, 2 for a command this
   knows not */
static int check_hidden_commands(const char* log) {
    const char* hide = g_settings.vulkan_hidden_commands;
    char line[512];
    int proven = 0;
    SnxVkExtensions listed;
    CHECK(snx_vk_device_extensions(g_vk.phys, &listed));
    VkPhysicalDeviceDepthClampControlFeaturesEXT clamp = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLAMP_CONTROL_FEATURES_EXT};
    VkPhysicalDeviceConditionalRenderingFeaturesEXT cond = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    void* chain[] = {&f2, snx_vk_extensions_has(&listed, VK_EXT_DEPTH_CLAMP_CONTROL_EXTENSION_NAME) ? &clamp : NULL,
                     snx_vk_extensions_has(&listed, VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME) ? &cond : NULL};
    snx_vk_chain(chain, 3);
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &f2);
    for (size_t i = 0; i < sizeof g_hidden_known / sizeof *g_hidden_known; i++) {
        const char *command = g_hidden_known[i].command, *extension = g_hidden_known[i].extension;
        if (!snx_vk_hide_has(hide, command)) continue;
        int feature = !strcmp(extension, VK_EXT_DEPTH_CLAMP_CONTROL_EXTENSION_NAME) ? clamp.depthClampControl != 0
                                                                                    : cond.conditionalRendering != 0;
        snprintf(line, sizeof line, "vulkan: %s: ", extension);
        const char* setting = strstr(log, line); /* the first device's line */
        if (!snx_vk_extensions_has(&listed, extension) || !feature || !setting ||
            strncmp(setting + strlen(line), "on (", 4)) {
            printf("hidden %s: %s would not be on (listed %d, feature %d, setting line \"%.60s\"): left out\n", command,
                   extension, snx_vk_extensions_has(&listed, extension), feature, setting ? setting : "none");
            continue;
        }
        snprintf(line, sizeof line, "lists %s without %s, which it requires", extension, command);
        CHECK(hidden_line(log, line,
                          "(vk_debug_hide_commands hides it from the check): the device is made again "
                          "without it"));
        snprintf(line, sizeof line, "vulkan: %s is off on ", extension);
        char why[256];
        snprintf(why, sizeof why,
                 "its driver lists it without %s, which it requires (vk_debug_hide_commands hides from "
                 "the check what the driver has)",
                 command);
        CHECK(hidden_line(log, line, why));
        snprintf(line, sizeof line, "vulkan: %s: off (its driver lists it without %s", extension, command);
        CHECK(hidden_line(log, line, NULL));
        for (u32 k = 0; k < g_vk.dev_ext_n; k++) CHECK(strcmp(g_vk.dev_ext[k], extension));
        CHECK(strcmp(extension, VK_EXT_DEPTH_CLAMP_CONTROL_EXTENSION_NAME) || !g_vk.has_depth_clamp_range);
        CHECK(strcmp(extension, VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME) || !g_vk.has_cond_render);
        printf("hidden %s: %s is off on the device made again, as its lines say\n", command, extension);
        proven++;
    }
    snx_vk_extensions_free(&listed);
    /* every name the list has is one this knows */
    for (const char *at = hide, *end; *at; at = end) {
        while (*at == ',' || *at == ' ') at++;
        for (end = at; *end && *end != ',' && *end != ' ';) end++;
        int known = end == at;
        for (size_t i = 0; i < sizeof g_hidden_known / sizeof *g_hidden_known && !known; i++)
            known = strlen(g_hidden_known[i].command) == (size_t)(end - at) &&
                    !strncmp(g_hidden_known[i].command, at, (size_t)(end - at));
        if (!known) {
            fprintf(stderr, "vk_debug_hide_commands=%s: %.*s is none of the commands this test knows\n", hide,
                    (int)(end - at), at);
            return 2;
        }
    }
    if (!proven) {
        puts("SKIP hidden commands: none of their extensions would be on with this driver");
        return 77;
    }
    snprintf(line, sizeof line, "vulkan: vk_debug_hide_commands=%s hides 0 commands of the extensions %s is made with",
             hide, g_vk.props.deviceName);
    CHECK(hidden_line(log, line, NULL));
    return 0;
}
