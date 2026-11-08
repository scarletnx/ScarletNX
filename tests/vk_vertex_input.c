/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* vertex_input_dynamic_state (gpu/vk/vertex_input.c), with no GPU: its values, the setting read from a game's .ini of
   a version without it and of one with it, the ban extension_bans.txt holds for stand-in devices (NVIDIA's before
   580.119.02, on over it), auto off on Android whatever the driver (Qualcomm's and Turnip there, Qualcomm's elsewhere
   keeping it), and the log line of every case naming the device, the driver, its version, the value given, the path in
   effect and the ban where one decides */
#include "nx/gpu/vk/vertex_input.h"
#include "nx/gpu/vk/extension_bans.h"
#include "nx/runtime/ini.h"
#include "snx_settings.h"
#include <stdio.h>
#include <string.h>
#include "volk.h"

static struct {
    unsigned checks, failures;
    char said[512]; /* ini_read's last line */
} g_test;
#define CHECK(c, what)                                                                                                 \
    do {                                                                                                               \
        g_test.checks++;                                                                                               \
        if (!(c)) {                                                                                                    \
            g_test.failures++;                                                                                         \
            printf("FAIL line %d: %s\n", __LINE__, what);                                                              \
        }                                                                                                              \
    } while (0)

static void say(const char* line) { snprintf(g_test.said, sizeof g_test.said, "%s", line); }

/* a game's .ini read into the Graphics table as the runtime reads its rows (ini_read), its comments and other
   sections left to their own tables */
static void read_graphics(const char* text) {
    char line[256];
    int graphics = 0;
    while (*text) {
        size_t n = strcspn(text, "\n");
        snprintf(line, sizeof line, "%.*s", (int)n, text);
        text += n + (text[n] == '\n');
        if (line[0] == '[') graphics = !strcmp(line, "[Graphics]");
        char* eq = strchr(line, '=');
        if (!graphics || line[0] == ';' || !eq) continue;
        *eq = 0;
        const IniOpt* o = ini_find(g_graphics_ini, line);
        if (o) ini_read(o, eq + 1);
    }
}

static void check_values(void) {
    static const struct {
        const char* text;
        int mode;
    } cases[] = {{"auto", SNX_VK_VI_AUTO},
                 {"on", SNX_VK_VI_ON},
                 {"off", SNX_VK_VI_OFF},
                 {"Auto", -1},
                 {"OFF", -1},
                 {"1", -1},
                 {"0", -1},
                 {"", -1},
                 {"of", -1},
                 {"auto ", -1},
                 {"automatic", -1}};
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        char what[96];
        snprintf(what, sizeof what, "\"%s\" reads as %d", cases[i].text, cases[i].mode);
        CHECK(vk_vi_mode(cases[i].text) == cases[i].mode, what);
    }
}

/* the [Graphics] of a .ini that a version without the key wrote, as it wrote it */
static const char g_old_ini[] =
    "[Display]\n"
    "fullscreen=0\n"
    "\n"
    "[Graphics]\n"
    "; The scale of the game's 3D rendering, 0.25 to 16: 1 = the console's, 2 = double, 0.5 = half\n"
    "resolution=1\n"
    "; 0 = skips shadow maps\n"
    "shadows=1\n"
    "vsync=1\n"
    "anisotropy=0\n"
    "; 1 = H.264 movies decode on the GPU with Vulkan Video (FFmpeg's Vulkan decoder), where the GPU and its driver\n"
    ";   have an H.264 decode queue. Experimental and untested. 0 = FFmpeg decodes them on the CPU. VP9 movies always\n"
    ";   decode on the CPU\n"
    "vulkan_video=0\n"
    "\n"
    "[Performance]\n"
    "fps_limit=game\n";

static void check_files(void) {
    const IniOpt* o = ini_find(g_graphics_ini, "vertex_input_dynamic_state");
    CHECK(o && o->type == INI_TEXT && o->def && !strcmp(o->def, "auto") && (o->flags & INI_STARTUP),
          "vertex_input_dynamic_state is a startup text of the Graphics table, auto by default");
    CHECK(o && o->words && !strcmp(o->words, "auto=Auto|on=On|off=Off"), "the menus offer auto, on and off");
    read_graphics(g_old_ini);
    CHECK(!strcmp(g_settings.vertex_input_dynamic_state, "auto") &&
              vk_vi_mode(g_settings.vertex_input_dynamic_state) == SNX_VK_VI_AUTO,
          "a .ini without the key reads as auto");
    CHECK(g_settings.resolution_scale == 1.0f && g_settings.shadows == 1, "the old file's other Graphics rows read");
    read_graphics("[Graphics]\nvertex_input_dynamic_state=off\n");
    CHECK(vk_vi_mode(g_settings.vertex_input_dynamic_state) == SNX_VK_VI_OFF, "vertex_input_dynamic_state=off reads");
    read_graphics("[Graphics]\nvertex_input_dynamic_state=on\n");
    CHECK(vk_vi_mode(g_settings.vertex_input_dynamic_state) == SNX_VK_VI_ON, "vertex_input_dynamic_state=on reads");
    /* a value the setting doesn't take reaches the device's check as written, which stops the game naming it */
    read_graphics("[Graphics]\nvertex_input_dynamic_state=Off\n");
    CHECK(!strcmp(g_settings.vertex_input_dynamic_state, "Off") && vk_vi_mode("Off") < 0,
          "vertex_input_dynamic_state=Off reaches the check as Off");
    /* past the text's room: said with the value it has, which is the last one read */
    read_graphics("[Graphics]\nvertex_input_dynamic_state=auto\n");
    g_test.said[0] = 0;
    read_graphics("[Graphics]\nvertex_input_dynamic_state=automatically_on\n");
    CHECK(!strcmp(g_settings.vertex_input_dynamic_state, "auto"), "a value too long for the text leaves auto");
    CHECK(strstr(g_test.said, "vertex_input_dynamic_state's value is longer than 15 bytes") &&
              strstr(g_test.said, "\"auto\""),
          "and says so with the value it has");
    if (!strstr(g_test.said, "\"auto\"")) printf("  said: %s\n", g_test.said);
}

#define NV(major, minor, patch) ((major) << 22 | (minor) << 14 | (patch) << 6)

/* every mode on a banned driver and a driver without a ban, with the extension, without its feature and without it */
static void check_choices(void) {
    static const char* const modes[] = {"auto", "on", "off"};
    static const struct {
        VkViDevice d;
        const char* version;
        int banned;
        const char* ban; /* how the log names the ban that holds */
    } devices[] = {
        {{"NVIDIA GeForce RTX 3060", "NVIDIA", "575.64.05", VK_DRIVER_ID_NVIDIA_PROPRIETARY, NV(575u, 64u, 5u), 2, 0,
          NULL},
         "575.64.05",
         1,
         "NVIDIA's driver before 580.119.02 breaks dynamic vertex input ("},
        {{"AMD Radeon RX 6700 XT (RADV NAVI22)", "radv", "Mesa 26.1.8", VK_DRIVER_ID_MESA_RADV,
          VK_MAKE_VERSION(26u, 1u, 8u), 2, 0, NULL},
         "26.1.8",
         0,
         NULL},
        /* Android: auto off whatever the driver; the same Qualcomm driver off Android keeps it */
        {{"Adreno (TM) 830", "Qualcomm Technologies Inc. Adreno Vulkan Driver", "Driver Build: 2fbb164246",
          VK_DRIVER_ID_QUALCOMM_PROPRIETARY, VK_MAKE_VERSION(512u, 800u, 48u), 2, 1, NULL},
         "512.800.48",
         0,
         NULL},
        {{"Turnip Adreno (TM) 830", "turnip Mesa driver", "Mesa 26.0.0", VK_DRIVER_ID_MESA_TURNIP,
          VK_MAKE_VERSION(26u, 0u, 0u), 2, 1, NULL},
         "26.0.0",
         0,
         NULL},
        {{"Adreno (TM) 830", "Qualcomm Technologies Inc. Adreno Vulkan Driver", "Driver Build: 2fbb164246",
          VK_DRIVER_ID_QUALCOMM_PROPRIETARY, VK_MAKE_VERSION(512u, 800u, 48u), 2, 0, NULL},
         "512.800.48",
         0,
         NULL},
    };
    char err[512] = "";
    CHECK(vk_bans_load(snx_vk_extension_bans_text, err, sizeof err), err);
    for (size_t d = 0; d < sizeof devices / sizeof *devices; d++)
        for (int has = 0; has <= 2; has++)
            for (int mode = SNX_VK_VI_AUTO; mode <= SNX_VK_VI_OFF; mode++) {
                VkViDevice dev = devices[d].d;
                /* the ban as the device asks the table for it */
                VkBanDevice bd = {dev.device,
                                  dev.driver_name,
                                  dev.driver_info,
                                  dev.driver_id,
                                  dev.driver_version,
                                  0,
                                  dev.android ? SNX_VK_BAN_ANDROID : SNX_VK_BAN_LINUX};
                char line[2048], what[2200], value[48], ban[1200];
                int warn = -1;
                vk_bans_device(&bd);
                uint32_t ban_line = vk_ban_find("VK_EXT_vertex_input_dynamic_state");
                if (ban_line) vk_ban_why(ban_line, ban, sizeof ban);
                CHECK(!ban_line == !devices[d].banned, "the table bans the devices it names");
                dev.ban = ban_line ? ban : NULL;
                dev.has = has;
                int dynamic = vk_vi_choose(mode, &dev, line, sizeof line, &warn);
                int android = dev.android && mode == SNX_VK_VI_AUTO;
                int want =
                    mode != SNX_VK_VI_OFF && has == 2 && (mode == SNX_VK_VI_ON || (!devices[d].banned && !android));
                snprintf(what, sizeof what, "%s, has %d, %s: dynamic %d, warn %d: %s", dev.device, has, modes[mode],
                         dynamic, warn, line);
                CHECK(dynamic == want, what);
                CHECK(warn == (mode == SNX_VK_VI_ON && has < 2), what);
                snprintf(value, sizeof value, "vertex_input_dynamic_state=%s", modes[mode]);
                CHECK(strstr(line, value) && strstr(line, dev.device) && strstr(line, dev.driver_name) &&
                          strstr(line, dev.driver_info) && strstr(line, devices[d].version),
                      what);
                CHECK(strstr(line, dynamic ? "as dynamic state" : "a pipeline of its own") &&
                          !strstr(line, dynamic ? "a pipeline of its own" : "as dynamic state"),
                      what);
                /* a ban is named where it decides, or where on goes over it, with what breaks and the table's line */
                int names_ban = devices[d].ban && strstr(line, devices[d].ban) && strstr(line, "extension_bans.txt:");
                CHECK(names_ban == (devices[d].banned && has == 2 && mode != SNX_VK_VI_OFF && !android), what);
                CHECK(!strstr(line, "over the ban: ") == !(devices[d].banned && dynamic), what);
                /* Android is named where it decides */
                CHECK((strstr(line, "off on Android, as Eden") != NULL) == (android && has == 2), what);
                if (has < 2 && mode != SNX_VK_VI_OFF)
                    CHECK(strstr(line, has ? "without its vertexInputDynamicState" : "has no VK_EXT_vertex_input") !=
                              NULL,
                          what);
            }
}

int main(void) {
    ini_set_say(say);
    check_values();
    check_files();
    check_choices();
    printf("%s: %u checks, %u failed\n", g_test.failures ? "FAIL" : "PASS", g_test.checks, g_test.failures);
    return g_test.failures != 0;
}
