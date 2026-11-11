/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The extension bans (gpu/vk/extension_bans.c), with no GPU: the table's errors, each named with its line and value;
   each driver's numbering of versions and every kind of bound at its edges; systems and GPUs; the filter taking a
   banned extension out of a stand-in device's list and leaving one a setting owns; the features asked for, said only
   where the device has them; the "off" lines nothing asked for, said; the log lines naming the key, the device, its
   driver and version, and the line; and the table the build carries, read whole, against the drivers its lines are
   about; and vulkan_ban_ignore read from a game's .ini, a key the table has going over its ban, said with the ban, and
   one it has not refused with the nearest keys it has.
   Its other uses, for retesting a ban (the workspace's tools/vk-ban-retest.py reads their lines):
     --devices [--table FILE]                     this machine's Vulkan devices, and the lines that hold for each
     --match DRIVER VERSION [SYSTEM [DEVICEID]] [--table FILE]   the lines that hold for a driver given by hand
     --check FILE                                 FILE read as the table: its first error, or that it reads */
#include "nx/gpu/vk/extension_bans.h"
#include "nx/runtime/ini.h"
#include "snx_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    unsigned checks, failures, nsaid;
    char said[32][2048];
    int warned[32];
    char table[70000];            /* a table read from a file */
    char many[16000], big[70000]; /* a table of too many lines, and one of too many bytes */
} g_test;
#define CHECK(c, what)                                                                                                 \
    do {                                                                                                               \
        g_test.checks++;                                                                                               \
        if (!(c)) {                                                                                                    \
            g_test.failures++;                                                                                         \
            printf("FAIL line %d: %s\n", __LINE__, what);                                                              \
        }                                                                                                              \
    } while (0)

static void say(const char* line, int warn) {
    if (g_test.nsaid < 32) {
        snprintf(g_test.said[g_test.nsaid], sizeof g_test.said[0], "%s", line);
        g_test.warned[g_test.nsaid++] = warn;
    }
}
/* the said line with every part, -1 for none */
static int said_with(const char* a, const char* b, const char* c) {
    for (unsigned i = 0; i < g_test.nsaid; i++)
        if (strstr(g_test.said[i], a) && (!b || strstr(g_test.said[i], b)) && (!c || strstr(g_test.said[i], c)))
            return (int)i;
    return -1;
}
static void say_ini(const char* line) { say(line, 1); }
/* how many said lines name a */
static unsigned count_with(const char* a) {
    unsigned n = 0;
    for (unsigned i = 0; i < g_test.nsaid; i++) n += strstr(g_test.said[i], a) != NULL;
    return n;
}
static void show_said(void) {
    for (unsigned i = 0; i < g_test.nsaid; i++)
        printf("  said%s: %s\n", g_test.warned[i] ? " (warn)" : "", g_test.said[i]);
}

#define HEAD "key | action | driver | versions | system | devices | breaks | source | retest | checked | override\n"
#define NV(major, minor, patch)   ((major) << 22 | (minor) << 14 | (patch) << 6)
#define INTEL_WIN(major, minor)   ((major) << 14 | (minor))
#define MESA(major, minor, patch) VK_MAKE_VERSION(major, minor, patch)

static void device(uint32_t id, uint32_t version, uint32_t gpu, uint32_t system) {
    VkBanDevice d = {"AMD Radeon RX 6800 (RADV NAVI21)", "radv", "Mesa 26.0.4", id, version, gpu, system};
    vk_bans_device(&d);
}

/* each table's first error: its line and what it says */
static void check_errors(void) {
    char *many = g_test.many, *big = g_test.big;
    const struct {
        const char* text;
        const char* error;
    } cases[] = {
        {"", "extension_bans.txt has no header"},
        {"# only a comment\n\n", "extension_bans.txt has no header"},
        {"key | action\n", "extension_bans.txt:1: the first line that is not a comment is \"key | action\""},
        {"# c\n\n" HEAD "VK_EXT_x | off | nvidia | * | any | *\n",
         "extension_bans.txt:4: 6 fields, the header names 11"},
        {HEAD "VK_EXT_x | off | nvidia | * | any | * | b | s | headless: | never | - |\n",
         "extension_bans.txt:2: 12 fields"},
        {HEAD "VK_EXT-x | off | nvidia | * | any | * | b | s | headless: | never | -\n",
         "extension_bans.txt:2: key \"VK_EXT-x\" is not a Vulkan name"},
        {HEAD "a..b | off | nvidia | * | any | * | b | s | headless: | never | -\n", "key \"a..b\" is not"},
        {HEAD ".feature | off | nvidia | * | any | * | b | s | headless: | never | -\n", "key \".feature\" is not"},
        {HEAD "VK_X.Feature | off | nvidia | * | any | * | b | s | headless: | never | -\n", "key \"VK_X.Feature\""},
        {HEAD "VK_EXT_x | ban | nvidia | * | any | * | b | s | headless: | never | -\n",
         "action \"ban\" is not off or warn"},
        {HEAD "VK_EXT_x | off | geforce | * | any | * | b | s | headless: | never | -\n",
         "driver \"geforce\" is none of the drivers"},
        {HEAD "VK_EXT_x | off | nvidia | 580.119.02 | any | * | b | s | headless: | never | -\n",
         "versions \"580.119.02\" are not *, one bound"},
        {HEAD "VK_EXT_x | off | nvidia | < 580.x | any | * | b | s | headless: | never | -\n",
         "\"580.x\" is not numbers between dots"},
        {HEAD "VK_EXT_x | off | nvidia | < 580. | any | * | b | s | headless: | never | -\n",
         "\"580.\" is not numbers between dots"},
        {HEAD "VK_EXT_x | off | nvidia | < 1024.0.0 | any | * | b | s | headless: | never | -\n",
         "\"1024.0.0\" is not NVIDIA's A.B[.C[.D]]"},
        {HEAD "VK_EXT_x | off | nvidia | < 580.119.02.64 | any | * | b | s | headless: | never | -\n",
         "\"580.119.02.64\" is not NVIDIA's"},
        {HEAD "VK_EXT_x | off | intel-windows | < 1.2.3 | any | * | b | s | headless: | never | -\n",
         "\"1.2.3\" is not Intel's four numbers or their last two"},
        {HEAD "VK_EXT_x | off | intel-windows | < 100.16384 | any | * | b | s | headless: | never | -\n",
         "\"100.16384\" is not Intel's"},
        {HEAD "VK_EXT_x | off | moltenvk | < 1.100.0 | any | * | b | s | headless: | never | -\n",
         "\"1.100.0\" is not MoltenVK's A.B[.C]"},
        {HEAD "VK_EXT_x | off | radv | < 26 | any | * | b | s | headless: | never | -\n",
         "\"26\" is not A.B[.C] of VK_MAKE_VERSION"},
        {HEAD "VK_EXT_x | off | qualcomm | < 512.1024.0 | any | * | b | s | headless: | never | -\n",
         "\"512.1024.0\" is not A.B[.C]"},
        {HEAD "VK_EXT_x | off | radv | >= 26.0 >= 27.0 | any | * | b | s | headless: | never | -\n",
         "versions \">= 26.0 >= 27.0\" are not *"},
        {HEAD "VK_EXT_x | off | radv | < 26.0 >= 25.0 | any | * | b | s | headless: | never | -\n",
         "versions \"< 26.0 >= 25.0\" are not *"},
        {HEAD "VK_EXT_x | off | radv | >= 25.0 < 26.0 < 27.0 | any | * | b | s | headless: | never | -\n", "are not *"},
        {HEAD "VK_EXT_x | off | radv | >= 27.0 < 26.0 | any | * | b | s | headless: | never | -\n",
         "versions \">= 27.0 < 26.0\" hold for no version"},
        {HEAD "VK_EXT_x | off | radv | > 26.0.0 < 26.0.1 | any | * | b | s | headless: | never | -\n",
         "hold for no version"},
        {HEAD "VK_EXT_x | off | radv | < 0.0.0 | any | * | b | s | headless: | never | -\n", "hold for no version"},
        {HEAD "VK_EXT_x | off | radv | * | linx | * | b | s | headless: | never | -\n", "system \"linx\" is not any"},
        {HEAD "VK_EXT_x | off | radv | * | linux,dos | * | b | s | headless: | never | -\n", "system \"linux,dos\""},
        {HEAD "VK_EXT_x | off | radv | * | , | * | b | s | headless: | never | -\n", "system is empty"},
        {HEAD "VK_EXT_x | off | radv | * | any | 6020200 | b | s | headless: | never | -\n",
         "devices \"6020200\" are not * or up to 4 deviceIDs"},
        {HEAD "VK_EXT_x | off | radv | * | any | 0x60202G0 | b | s | headless: | never | -\n", "devices \"0x60202G0\""},
        {HEAD "VK_EXT_x | off | radv | * | any | 0x1,0x2,0x3,0x4,0x5 | b | s | headless: | never | -\n",
         "devices \"0x1,0x2,0x3,0x4,0x5\" are not * or up to 4"},
        {HEAD "VK_EXT_x | off | radv | * | any | * |  | s | headless: | never | -\n",
         "breaks: what goes wrong is empty"},
        {HEAD "VK_EXT_x | off | radv | * | any | * | b | - | headless: | never | -\n",
         "source: where it is known from is empty"},
        {HEAD "VK_EXT_x | off | radv | * | any | * | b | s | edenprobe: | never | -\n", "retest \"edenprobe:\" is not"},
        {HEAD "VK_EXT_x | off | radv | * | any | * | b | s | new:headless: | never | -\n", "retest \"new:headless:\""},
        {HEAD "VK_EXT_x | off | radv | * | any | * | b | s | rerun it | never | -\n", "retest \"rerun it\" is not"},
        {HEAD "VK_EXT_x | off | radv | * | any | * | b | s | headless: | yesterday | -\n",
         "checked \"yesterday\" is not never, or a date"},
        {HEAD "VK_EXT_x | off | radv | * | any | * | b | s | headless: | 2026-11-07 | -\n", "checked \"2026-11-07\""},
        {HEAD "VK_EXT_x | off | radv | * | any | * | b | s | headless: | 2026/11/07 broken | -\n", "checked \"2026/11"},
        {HEAD "VK_EXT_x | off | radv | * | any | * | b | s | headless: | never | vertex_input\n",
         "override \"vertex_input\" is not - or a setting's key=value"},
        {HEAD "VK_EXT_x | off | radv | * | any | * | b | s | headless: | never | key=\n", "override \"key=\""},
        {HEAD "VK_EXT_x | warn | radv | * | any | * | b | s | headless: | never | key=on\n",
         "override \"key=on\" is for an off line"},
        {many, "extension_bans.txt:194: past the 192 bans this reader holds"},
        {big, "bytes, past the 65535 this reader holds"},
    };
    size_t at = (size_t)snprintf(many, sizeof g_test.many, "%s", HEAD);
    for (int i = 0; i < 193; i++)
        at += (size_t)snprintf(many + at, sizeof g_test.many - at,
                               "VK_EXT_x | off | radv | * | any | * | b | s | headless: | never | -\n");
    memset(big, '#', sizeof g_test.big - 1);
    big[sizeof g_test.big - 1] = 0;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        char err[512] = "", what[1200];
        int ok = vk_bans_load(cases[i].text, err, sizeof err);
        snprintf(what, sizeof what, "case %zu: wanted \"%s\", got %d \"%s\"", i, cases[i].error, ok, err);
        CHECK(!ok && strstr(err, cases[i].error), what);
    }
    /* comments, blank lines, spaces and CRLF endings read */
    char err[512] = "";
    CHECK(vk_bans_load("  # a comment\r\n\r\n" HEAD "\t  \r\n"
                       "VK_EXT_x|off|radv|*|any|*|b|s|headless:|never|-\r\n",
                       err, sizeof err),
          err);
    device(VK_DRIVER_ID_MESA_RADV, MESA(26u, 0u, 4u), 0x73bf, SNX_VK_BAN_LINUX);
    CHECK(vk_bans_holding(0) == 5 && vk_ban_find("VK_EXT_x") == 5, "a CRLF table's line 5 holds");
}

static void check_versions(void) {
    static const struct {
        uint32_t id, v;
        const char* text;
    } cases[] = {{VK_DRIVER_ID_NVIDIA_PROPRIETARY, NV(580u, 119u, 2u), "580.119.02"},
                 {VK_DRIVER_ID_NVIDIA_PROPRIETARY, NV(575u, 64u, 5u) | 3u, "575.64.05"},
                 {VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS, INTEL_WIN(101u, 4502u), "101.4502"},
                 {VK_DRIVER_ID_MESA_RADV, MESA(26u, 1u, 8u), "26.1.8"},
                 {VK_DRIVER_ID_QUALCOMM_PROPRIETARY, MESA(512u, 676u, 0u), "512.676.0"},
                 {VK_DRIVER_ID_MOLTENVK, 10211u, "1.2.11"},
                 {VK_DRIVER_ID_ARM_PROPRIETARY, MESA(48u, 1u, 0u), "48.1.0"}};
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        char got[32], what[128], why[256];
        uint32_t parsed = 0;
        vk_driver_version_text(cases[i].id, cases[i].v, got, sizeof got);
        snprintf(what, sizeof what, "driver %u version %#x is %s, not %s", (unsigned)cases[i].id, (unsigned)cases[i].v,
                 cases[i].text, got);
        CHECK(!strcmp(got, cases[i].text), what);
        /* and the text reads back as the driver numbers it, but NVIDIA's fourth number, which the text leaves out */
        uint32_t want = cases[i].id == VK_DRIVER_ID_NVIDIA_PROPRIETARY ? cases[i].v & ~63u : cases[i].v;
        CHECK(vk_ban_parse_version(cases[i].id, cases[i].text, &parsed, why, sizeof why) && parsed == want, what);
    }
    uint32_t v = 0;
    char why[256];
    CHECK(vk_ban_parse_version(VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS, "31.0.101.4502", &v, why, sizeof why) &&
              v == INTEL_WIN(101u, 4502u),
          "Intel's four numbers read as their last two");
    CHECK(vk_ban_parse_version(VK_DRIVER_ID_NVIDIA_PROPRIETARY, "580.119.02.1", &v, why, sizeof why) &&
              v == (NV(580u, 119u, 2u) | 1u),
          "NVIDIA's fourth number");
    /* every VkDriverId the header has up to the reserved ones, named and named back */
    for (uint32_t id = VK_DRIVER_ID_AMD_PROPRIETARY; id <= VK_DRIVER_ID_APE_SOFT; id++) {
        char what[64];
        snprintf(what, sizeof what, "driver ID %u has a name", (unsigned)id);
        CHECK(vk_ban_driver_name(id) && vk_ban_driver_id(vk_ban_driver_name(id)) == id, what);
    }
    CHECK(!vk_ban_driver_name(0) && !vk_ban_driver_name(VK_DRIVER_ID_APE_SOFT + 1) && !vk_ban_driver_id("geforce"),
          "no name for what is no driver");
}

static const char g_table[] =
    "# a table of every kind of line\n" HEAD
    "VK_EXT_a | off | nvidia | < 580.119.02 | any | * | breaks a | src a | headless: | never | -\n"
    "VK_EXT_b | off | radv | >= 25.0.3 < 26.1 | linux | * | breaks b | src b | headless:--x=1 | never | -\n"
    "VK_EXT_c | warn | radv | * | any | * | breaks c | src c | new:headless:a case | never | -\n"
    "featureD | off | radv | <= 26.0.99 | any | 0x73bf,0x1636 | breaks d | src d | new:edenprobe:d | 2026-11-07 broken "
    "on 26.0.4 | -\n"
    "VK_EXT_e | off | radv | > 20.0.0 | android, linux | * | breaks e | src e | edenprobe:coherence | never | e=on\n"
    "featureF | warn | radv | * | any | * | breaks f | src f | headless: | never | -\n"
    "featureG | off | radv | * | windows | * | breaks g | src g | headless: | never | -\n"
    "VK_EXT_b | off | radv | * | any | * | breaks b again | src b2 | headless: | never | -\n"
    "VK_EXT_h | off | moltenvk | < 1.2.11 | any | * | breaks h | src h | headless: | never | -\n"
    "VK_EXT_i | off | intel-windows | < 27.20.100.9684 | any | * | breaks i | src i | headless: | never | -\n"
    "VK_EXT_j | off | radv | >= 26.0.0 | any | * | breaks j | src j | headless: | never | -\n";

static void check_matching(void) {
    char err[512] = "", text[512];
    CHECK(vk_bans_load(g_table, err, sizeof err), err);
    /* one driver, its versions at each bound's edge */
    static const struct {
        uint32_t id, v, gpu, system;
        const char* key;
        uint32_t line;
    } cases[] = {
        {VK_DRIVER_ID_NVIDIA_PROPRIETARY, NV(580u, 119u, 1u), 0, SNX_VK_BAN_WINDOWS, "VK_EXT_a", 3},
        {VK_DRIVER_ID_NVIDIA_PROPRIETARY, NV(580u, 118u, 255u) | 63u, 0, SNX_VK_BAN_LINUX, "VK_EXT_a", 3},
        {VK_DRIVER_ID_NVIDIA_PROPRIETARY, NV(580u, 119u, 2u), 0, SNX_VK_BAN_LINUX, "VK_EXT_a", 0},
        {VK_DRIVER_ID_NVIDIA_PROPRIETARY, NV(580u, 119u, 2u) | 1u, 0, SNX_VK_BAN_LINUX, "VK_EXT_a", 0},
        {VK_DRIVER_ID_MESA_NVK, NV(470u, 1u, 1u), 0, SNX_VK_BAN_LINUX, "VK_EXT_a", 0},
        {VK_DRIVER_ID_NVIDIA_PROPRIETARY, NV(470u, 1u, 1u), 0, 0, "VK_EXT_a", 3}, /* any takes an unnamed system */
        {VK_DRIVER_ID_MESA_RADV, MESA(25u, 0u, 3u), 0, SNX_VK_BAN_LINUX, "VK_EXT_b", 4},
        {VK_DRIVER_ID_MESA_RADV, MESA(25u, 0u, 2u), 0, SNX_VK_BAN_LINUX, "VK_EXT_b", 10},
        {VK_DRIVER_ID_MESA_RADV, MESA(26u, 0u, 4095u), 0, SNX_VK_BAN_LINUX, "VK_EXT_b", 4},
        {VK_DRIVER_ID_MESA_RADV, MESA(26u, 1u, 0u), 0, SNX_VK_BAN_LINUX, "VK_EXT_b", 10},
        {VK_DRIVER_ID_MESA_RADV, MESA(25u, 0u, 3u), 0, SNX_VK_BAN_WINDOWS, "VK_EXT_b", 10},
        {VK_DRIVER_ID_MESA_RADV, MESA(26u, 0u, 99u), 0x73bf, SNX_VK_BAN_LINUX, "featureD", 6},
        {VK_DRIVER_ID_MESA_RADV, MESA(26u, 0u, 100u), 0x73bf, SNX_VK_BAN_LINUX, "featureD", 0},
        {VK_DRIVER_ID_MESA_RADV, MESA(26u, 0u, 4u), 0x1636, SNX_VK_BAN_LINUX, "featureD", 6},
        {VK_DRIVER_ID_MESA_RADV, MESA(26u, 0u, 4u), 0x1637, SNX_VK_BAN_LINUX, "featureD", 0},
        {VK_DRIVER_ID_MESA_RADV, MESA(20u, 0u, 1u), 0, SNX_VK_BAN_ANDROID, "VK_EXT_e", 7},
        {VK_DRIVER_ID_MESA_RADV, MESA(20u, 0u, 0u), 0, SNX_VK_BAN_ANDROID, "VK_EXT_e", 0},
        {VK_DRIVER_ID_MESA_RADV, MESA(20u, 0u, 1u), 0, SNX_VK_BAN_FREEBSD, "VK_EXT_e", 0},
        {VK_DRIVER_ID_MESA_RADV, MESA(20u, 0u, 1u), 0, 0, "VK_EXT_e", 0}, /* a list takes no unnamed system */
        {VK_DRIVER_ID_MESA_RADV, MESA(26u, 0u, 4u), 0, SNX_VK_BAN_WINDOWS, "featureG", 9},
        {VK_DRIVER_ID_MESA_RADV, MESA(26u, 0u, 4u), 0, SNX_VK_BAN_LINUX, "featureG", 0},
        {VK_DRIVER_ID_MESA_RADV, MESA(26u, 0u, 4u), 0, SNX_VK_BAN_LINUX, "featureF", 0}, /* a warning bans nothing */
        {VK_DRIVER_ID_MOLTENVK, 10210u, 0, SNX_VK_BAN_MACOS, "VK_EXT_h", 11},
        {VK_DRIVER_ID_MOLTENVK, 10211u, 0, SNX_VK_BAN_MACOS, "VK_EXT_h", 0},
        {VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS, INTEL_WIN(100u, 9683u), 0, SNX_VK_BAN_WINDOWS, "VK_EXT_i", 12},
        {VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS, INTEL_WIN(100u, 9684u), 0, SNX_VK_BAN_WINDOWS, "VK_EXT_i", 0},
        {VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS, INTEL_WIN(101u, 0u), 0, SNX_VK_BAN_WINDOWS, "VK_EXT_i", 0},
        {VK_DRIVER_ID_MESA_RADV, MESA(26u, 0u, 0u), 0, SNX_VK_BAN_LINUX, "VK_EXT_j", 13},
        {VK_DRIVER_ID_MESA_RADV, MESA(25u, 99u, 99u), 0, SNX_VK_BAN_LINUX, "VK_EXT_j", 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        char what[160];
        device(cases[i].id, cases[i].v, cases[i].gpu, cases[i].system);
        uint32_t got = vk_ban_find(cases[i].key);
        snprintf(what, sizeof what, "case %zu: %s holds at line %u, not %u", i, cases[i].key, (unsigned)cases[i].line,
                 (unsigned)got);
        CHECK(got == cases[i].line, what);
    }
    /* each line's driver and versions as the log says them */
    static const struct {
        uint32_t line;
        const char* scope;
    } scopes[] = {{3, "NVIDIA's driver before 580.119.02"},
                  {4, "RADV from 25.0.3 to before 26.1 on Linux"},
                  {5, "RADV"},
                  {6, "RADV up to 26.0.99 on the GPUs 0x73bf, 0x1636"},
                  {7, "RADV after 20.0.0 on Linux, Android"},
                  {9, "RADV on Windows"},
                  {11, "MoltenVK before 1.2.11"},
                  {12, "Intel's Windows driver before 27.20.100.9684"},
                  {13, "RADV from 26.0.0 on"}};
    for (size_t i = 0; i < sizeof scopes / sizeof *scopes; i++) {
        char what[400];
        vk_ban_scope(scopes[i].line, text, sizeof text);
        snprintf(what, sizeof what, "line %u reads \"%s\", not \"%s\"", (unsigned)scopes[i].line, text,
                 scopes[i].scope);
        CHECK(!strcmp(text, scopes[i].scope), what);
    }
    vk_ban_why(6, text, sizeof text);
    CHECK(!strcmp(text, "RADV up to 26.0.99 on the GPUs 0x73bf, 0x1636 breaks d (src d; extension_bans.txt:6)"), text);
    vk_ban_field(4, VK_BAN_RETEST, text, sizeof text);
    CHECK(!strcmp(text, "headless:--x=1"), text);
    vk_ban_field(6, VK_BAN_CHECKED, text, sizeof text);
    CHECK(!strcmp(text, "2026-11-07 broken on 26.0.4"), text);
    vk_ban_field(7, VK_BAN_OVERRIDE, text, sizeof text);
    CHECK(!strcmp(text, "e=on"), text);
    vk_ban_field(2, VK_BAN_KEY, text, sizeof text);
    CHECK(strstr(text, "has no line 2"), "the header is no ban");
    vk_ban_scope(99, text, sizeof text);
    CHECK(strstr(text, "has no ban at line 99"), "a line past the table says so");
}

/* a stand-in device's extensions through the filter, then the features asked for and the lines nothing asked for */
static void check_filter(void) {
    static const char* const names[] = {"VK_KHR_swapchain", "VK_EXT_b", "VK_EXT_c", "VK_EXT_e", "VK_EXT_j"};
    VkExtensionProperties list[5];
    SnxVkExtensions e = {list, 5};
    char err[512] = "";
    for (int i = 0; i < 5; i++) snprintf(list[i].extensionName, sizeof list[i].extensionName, "%s", names[i]);
    CHECK(vk_bans_load(g_table, err, sizeof err), err);
    device(VK_DRIVER_ID_MESA_RADV, MESA(26u, 0u, 4u), 0x73bf, SNX_VK_BAN_LINUX);
    g_test.nsaid = 0;
    uint32_t removed = vk_bans_filter(&e, say);
    int b = 0, c = 0, ext_e = 0, j = 0;
    for (uint32_t i = 0; i < e.count; i++) {
        b |= !strcmp(list[i].extensionName, "VK_EXT_b");
        c |= !strcmp(list[i].extensionName, "VK_EXT_c");
        ext_e |= !strcmp(list[i].extensionName, "VK_EXT_e");
        j |= !strcmp(list[i].extensionName, "VK_EXT_j");
    }
    CHECK(
        removed == 2 && e.count == 3 && !b && !j && c && ext_e && !strcmp(list[0].extensionName, "VK_KHR_swapchain"),
        "the filter takes out VK_EXT_b and VK_EXT_j, keeping the warned one, the overridden one and the rest in order");
    int off = said_with("vulkan: VK_EXT_b is off on AMD Radeon RX 6800 (RADV NAVI21) (radv, Mesa 26.0.4, driver "
                        "version 26.0.4): RADV from 25.0.3 to before 26.1 on Linux breaks b (src b; "
                        "extension_bans.txt:4)",
                        NULL, NULL);
    CHECK(off >= 0 && !g_test.warned[off], "VK_EXT_b's line says it is off, with the device, its version and why");
    CHECK(said_with("VK_EXT_b", "breaks b again", NULL) < 0, "a second line on an extension already out says nothing");
    int warned = said_with("vulkan: a known problem the extension bans do not turn off: VK_EXT_c on AMD Radeon",
                           "driver version 26.0.4): RADV breaks c (src c; extension_bans.txt:5)", NULL);
    CHECK(warned >= 0 && g_test.warned[warned], "VK_EXT_c's warning is a warning");
    CHECK(said_with("featureF on", "RADV breaks f (src f; extension_bans.txt:8)", NULL) >= 0,
          "a feature's warning comes with the filter");
    CHECK(said_with("VK_EXT_e", NULL, NULL) < 0 && said_with("featureD", NULL, NULL) < 0,
          "the filter leaves an overridden extension and an off feature unsaid");
    CHECK(g_test.nsaid == 4, "the filter says four lines");
    if (g_test.nsaid != 4) show_said();
    /* nothing asked yet: the overridden extension and the feature are said as the bans nothing asked for */
    g_test.nsaid = 0;
    vk_bans_unclaimed(say);
    CHECK(said_with("vulkan: the extension bans turn VK_EXT_e off on AMD Radeon",
                    "which this renderer does not ask for: RADV after 20.0.0 on Linux, Android breaks e (src e; "
                    "extension_bans.txt:7)",
                    NULL) >= 0 &&
              said_with("featureD off on", "does not ask for", NULL) >= 0 && g_test.nsaid == 2,
          "the lines nothing asked for are said");
    if (g_test.nsaid != 2) show_said();
    /* asked for: a feature the device lacks is claimed unsaid, one it has is said off; the owner reads its own */
    g_test.nsaid = 0;
    CHECK(vk_ban("featureD", 0, say) == 0 && g_test.nsaid == 0, "a feature the device lacks bans nothing unsaid");
    CHECK(vk_ban("featureG", 1, say) == 0 && g_test.nsaid == 0, "a ban that does not hold says nothing");
    CHECK(vk_ban_find("VK_EXT_e") == 7, "the setting's code finds its ban");
    vk_bans_unclaimed(say);
    CHECK(g_test.nsaid == 0, "nothing left unclaimed once asked");
    if (g_test.nsaid) show_said();
    CHECK(vk_ban("featureD", 1, say) == 6 &&
              said_with("vulkan: featureD is off on AMD Radeon RX 6800",
                        "RADV up to 26.0.99 on the GPUs 0x73bf, 0x1636 breaks d (src d; extension_bans.txt:6)",
                        NULL) >= 0,
          "a feature the device has is said off");
    /* the lines that hold, in order */
    uint32_t holding[16], n = 0;
    for (uint32_t line = vk_bans_holding(0); line && n < 16; line = vk_bans_holding(line)) holding[n++] = line;
    CHECK(n == 7 && holding[0] == 4 && holding[1] == 5 && holding[2] == 6 && holding[3] == 7 && holding[4] == 8 &&
              holding[5] == 10 && holding[6] == 13,
          "the lines that hold, 4 5 6 7 8 10 13");
    /* a new device starts unclaimed */
    device(VK_DRIVER_ID_MESA_RADV, MESA(26u, 0u, 4u), 0x73bf, SNX_VK_BAN_LINUX);
    g_test.nsaid = 0;
    vk_bans_unclaimed(say);
    CHECK(said_with("VK_EXT_e", NULL, NULL) >= 0, "a new device's lines are unclaimed again");
}

/* the extension settings (vk_extension_settings): off takes its extension out, on goes over its bans, auto leaves it to
   them, and each is said with its value in effect and why; a value not auto, on or off, and off for a needed extension
   the driver has, are refused */
static void check_settings(void) {
    static const char* const names[] = {"VK_KHR_swapchain", "VK_EXT_b", "VK_EXT_c", "VK_EXT_e", "VK_EXT_j", "VK_EXT_k"};
    VkExtensionProperties list[6];
    SnxVkExtensions e = {list, 6};
    char err[512] = "";
    for (int i = 0; i < 6; i++) snprintf(list[i].extensionName, sizeof list[i].extensionName, "%s", names[i]);
    CHECK(vk_bans_load(g_table, err, sizeof err), err);
    device(VK_DRIVER_ID_MESA_RADV, MESA(26u, 0u, 4u), 0x73bf, SNX_VK_BAN_LINUX);
    const VkExtensionSetting settings[] = {{"VK_KHR_swapchain", "auto", 0}, {"VK_EXT_b", "on", 0},
                                           {"VK_EXT_c", "auto", 13},        {"VK_EXT_e", "auto", 0},
                                           {"VK_EXT_j", "auto", 0},         {"VK_EXT_k", "off", 0},
                                           {"VK_EXT_z", "on", 0},           {"VK_EXT_y", "off", 0}};
    const VkExtensionNeed need[] = {{"VK_KHR_swapchain", "the window is drawn with it"}, {"VK_EXT_y", "y is needed"}};
    const VkExtensionSetting maybe[] = {{"VK_EXT_c", "maybe", 0}};
    CHECK(!vk_extension_settings(&e, maybe, 1, NULL, 0, err, sizeof err) &&
              !strcmp(err, "vk_ext_c=maybe is not auto, on or off"),
          "a value not auto, on or off is refused, with the setting's key");
    const VkExtensionSetting no_window[] = {{"VK_KHR_swapchain", "off", 0}};
    CHECK(!vk_extension_settings(&e, no_window, 1, need, 2, err, sizeof err) &&
              !strcmp(err, "vk_khr_swapchain=off turns off VK_KHR_swapchain, which ScarletNX can't run without on this "
                           "device: the window is drawn with it") &&
              e.count == 6,
          "off for a needed extension the driver has is refused, the list left whole");
    CHECK(vk_extension_settings(&e, settings, 8, need, 2, err, sizeof err), err);
    CHECK(e.count == 5, "off takes VK_EXT_k out; off for a needed one the driver has not (VK_EXT_y) is no matter");
    g_test.nsaid = 0;
    CHECK(vk_bans_filter(&e, say) == 1 && e.count == 4, "the bans take out VK_EXT_j alone: on goes over VK_EXT_b's");
    CHECK(vk_ban_find("VK_EXT_b") == 0, "and vk_ban_find finds no ban on it either");
    g_test.nsaid = 0;
    vk_extension_settings_say(&e, VK_API_VERSION_1_3, say);
    CHECK(g_test.nsaid == 8, "a line for each setting");
    CHECK(said_with("vulkan: VK_KHR_swapchain: on (auto: the driver has it, and no ban holds)", NULL, NULL) >= 0,
          "auto with no ban: on");
    int over = said_with("vulkan: VK_EXT_b: on (vk_ext_b=on goes over the ban: RADV from 25.0.3 to before 26.1 on "
                         "Linux breaks b (src b; extension_bans.txt:4))",
                         NULL, NULL);
    CHECK(over >= 0 && g_test.warned[over], "on over a ban: a warning naming the ban");
    CHECK(said_with("vulkan: VK_EXT_c: on (auto: the driver has it, and no ban holds; core in Vulkan 1.3, which the "
                    "renderer runs this device at)",
                    NULL, NULL) >= 0,
          "an extension core in the device's version says so");
    CHECK(said_with("vulkan: VK_EXT_e: on (auto, with a ban that e applies: ", "breaks e", NULL) >= 0,
          "a ban another setting applies leaves the extension on, naming that setting");
    CHECK(said_with("vulkan: VK_EXT_j: off (auto, as the ban holds: ", "breaks j", NULL) >= 0, "auto under a ban: off");
    CHECK(said_with("vulkan: VK_EXT_k: off (vk_ext_k=off)", NULL, NULL) >= 0, "off: off");
    CHECK(said_with("vulkan: VK_EXT_z: off (the driver has it not, which vk_ext_z=on can't change)", NULL, NULL) >= 0,
          "on for one the driver has not: off");
    CHECK(said_with("vulkan: VK_EXT_y: off (vk_ext_y=off, and the driver has it not)", NULL, NULL) >= 0,
          "off for one the driver has not says both");
    if (g_test.nsaid != 8) show_said();
}

/* the table the build carries, read whole, against the drivers its lines are about */
static void check_shipped(void) {
    char err[512] = "", text[1200];
    CHECK(vk_bans_load(snx_vk_extension_bans_text, err, sizeof err), err);
    static const struct {
        uint32_t id, v, gpu, system;
        const char* key;
        int banned;
        const char* scope;
    } cases[] = {
        {VK_DRIVER_ID_NVIDIA_PROPRIETARY, NV(575u, 64u, 5u), 0, SNX_VK_BAN_LINUX, "VK_EXT_vertex_input_dynamic_state",
         1, "NVIDIA's driver before 580.119.02"},
        {VK_DRIVER_ID_NVIDIA_PROPRIETARY, NV(580u, 119u, 2u), 0, SNX_VK_BAN_LINUX, "VK_EXT_vertex_input_dynamic_state",
         0, NULL},
        {VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS, INTEL_WIN(99u, 16383u), 0, SNX_VK_BAN_WINDOWS,
         "VK_EXT_vertex_input_dynamic_state", 1, "Intel's Windows driver before 27.20.100.0"},
        {VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS, INTEL_WIN(101u, 4502u), 0, SNX_VK_BAN_WINDOWS,
         "VK_EXT_vertex_input_dynamic_state", 0, NULL},
        {VK_DRIVER_ID_QUALCOMM_PROPRIETARY, MESA(512u, 800u, 48u), 0, SNX_VK_BAN_ANDROID,
         "VK_EXT_vertex_input_dynamic_state", 1, "Qualcomm's driver on Android"},
        {VK_DRIVER_ID_QUALCOMM_PROPRIETARY, MESA(512u, 800u, 48u), 0, SNX_VK_BAN_WINDOWS,
         "VK_EXT_vertex_input_dynamic_state", 0, NULL},
        {VK_DRIVER_ID_MESA_TURNIP, MESA(26u, 0u, 0u), 0, SNX_VK_BAN_ANDROID, "VK_EXT_vertex_input_dynamic_state", 0,
         NULL},
        {VK_DRIVER_ID_ARM_PROPRIETARY, MESA(47u, 1u, 0u), 0, SNX_VK_BAN_ANDROID, "VK_EXT_vertex_input_dynamic_state", 1,
         "Arm's Mali driver before 48.0.0"},
        {VK_DRIVER_ID_ARM_PROPRIETARY, MESA(47u, 1u, 0u), 0, SNX_VK_BAN_ANDROID,
         "VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE", 1, "Arm's Mali driver before 48.0.0"},
        {VK_DRIVER_ID_ARM_PROPRIETARY, MESA(48u, 0u, 0u), 0, SNX_VK_BAN_ANDROID,
         "VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE", 0, NULL},
        {VK_DRIVER_ID_MOLTENVK, 10211u, 0, SNX_VK_BAN_MACOS, "VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE", 1,
         "MoltenVK"},
        {VK_DRIVER_ID_MESA_KOSMICKRISP, MESA(26u, 0u, 0u), 0, SNX_VK_BAN_MACOS,
         "VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE", 0, NULL},
        {VK_DRIVER_ID_QUALCOMM_PROPRIETARY, MESA(512u, 676u, 0u), 0, SNX_VK_BAN_ANDROID,
         "shaderDenormFlushToZeroFloat32", 1, "Qualcomm's driver"},
        {VK_DRIVER_ID_MESA_TURNIP, MESA(26u, 0u, 0u), 0, SNX_VK_BAN_ANDROID, "shaderDenormFlushToZeroFloat32", 0, NULL},
        {VK_DRIVER_ID_NVIDIA_PROPRIETARY, NV(590u, 48u, 1u), 0, SNX_VK_BAN_LINUX, "VK_KHR_present_id2", 1,
         "NVIDIA's driver before 595.0.0"},
        {VK_DRIVER_ID_NVIDIA_PROPRIETARY, NV(595u, 0u, 0u), 0, SNX_VK_BAN_LINUX, "VK_KHR_present_id2", 0, NULL},
        {VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS, INTEL_WIN(101u, 4502u), 0, SNX_VK_BAN_WINDOWS, "shaderFloat16", 1,
         "Intel's Windows driver"},
        {VK_DRIVER_ID_QUALCOMM_PROPRIETARY, MESA(512u, 800u, 48u), 0x6020200, SNX_VK_BAN_ANDROID,
         "VK_EXT_blend_operation_advanced", 1, "Qualcomm's driver on the GPUs 0x6020200, 0x7000201"},
        {VK_DRIVER_ID_QUALCOMM_PROPRIETARY, MESA(512u, 800u, 48u), 0x43050a01, SNX_VK_BAN_ANDROID,
         "VK_EXT_blend_operation_advanced", 0, NULL},
        {VK_DRIVER_ID_MESA_TURNIP, MESA(26u, 0u, 0u), 0, SNX_VK_BAN_ANDROID, "VK_SHADER_STAGE_GEOMETRY_BIT", 1,
         "Turnip"},
        {VK_DRIVER_ID_MESA_TURNIP, MESA(26u, 0u, 0u), 0, SNX_VK_BAN_LINUX, "VK_SHADER_STAGE_GEOMETRY_BIT", 1, "Turnip"},
        {VK_DRIVER_ID_QUALCOMM_PROPRIETARY, MESA(512u, 800u, 48u), 0, SNX_VK_BAN_ANDROID,
         "VK_SHADER_STAGE_GEOMETRY_BIT", 0, NULL},
        {VK_DRIVER_ID_MESA_RADV, MESA(26u, 2u, 6u), 0, SNX_VK_BAN_LINUX, "VK_SHADER_STAGE_GEOMETRY_BIT", 0, NULL},
        {VK_DRIVER_ID_MESA_TURNIP, MESA(26u, 2u, 99u), 0, SNX_VK_BAN_ANDROID, "OpConvertSToF", 1, "Turnip"},
        {VK_DRIVER_ID_MESA_TURNIP, MESA(26u, 2u, 99u), 0, SNX_VK_BAN_ANDROID, "OpConvertUToF", 1, "Turnip"},
        {VK_DRIVER_ID_QUALCOMM_PROPRIETARY, MESA(512u, 800u, 48u), 0, SNX_VK_BAN_ANDROID, "OpConvertSToF", 0, NULL},
        {VK_DRIVER_ID_MESA_RADV, MESA(26u, 2u, 6u), 0, SNX_VK_BAN_LINUX, "OpConvertUToF", 0, NULL},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        char what[400];
        device(cases[i].id, cases[i].v, cases[i].gpu, cases[i].system);
        uint32_t line = vk_ban_find(cases[i].key);
        if (line) vk_ban_scope(line, text, sizeof text);
        snprintf(what, sizeof what, "case %zu: %s banned %d, got line %u (%s)", i, cases[i].key, cases[i].banned,
                 (unsigned)line, line ? text : "none");
        CHECK(cases[i].banned ? line && !strcmp(text, cases[i].scope) : !line, what);
    }
    /* the line docs/dev/development.md shows, as the filter says it for an NVIDIA driver before 595 */
    {
        VkExtensionProperties list[2] = {{"VK_KHR_present_id2", 1}, {"VK_KHR_present_wait2", 1}};
        SnxVkExtensions e = {list, 2};
        VkBanDevice d = {"NVIDIA GeForce RTX 3060", "NVIDIA", "590.48.01",     VK_DRIVER_ID_NVIDIA_PROPRIETARY,
                         NV(590u, 48u, 1u),         0x2503,   SNX_VK_BAN_LINUX};
        vk_bans_device(&d);
        g_test.nsaid = 0;
        CHECK(vk_bans_filter(&e, say) == 1 && e.count == 1 && !strcmp(list[0].extensionName, "VK_KHR_present_wait2"),
              "present_id2 leaves an NVIDIA 590's list, present_wait2 stays");
        CHECK(said_with("vulkan: VK_KHR_present_id2 is off on NVIDIA GeForce RTX 3060 (NVIDIA, 590.48.01, driver "
                        "version 590.48.01): NVIDIA's driver before 595.0.0 hangs games at their first frames with "
                        "present_id2 and present_wait2 (590.48, with Smooth Motion on) (DXVK dxvk_device_info.cpp:610, "
                        "issue 5507; extension_bans.txt:",
                        NULL, NULL) >= 0,
              "the log line docs/dev/development.md shows");
        if (said_with("VK_KHR_present_id2", NULL, NULL) < 0) show_said();
    }
    /* subgroup operations leave Turnip's geometry stage as device.c asks, the line naming the driver and its version;
       a device that does not list the stage is told nothing */
    {
        VkBanDevice d = {"Turnip Adreno (TM) 740", "turnip Mesa driver", "Mesa 26.0.3",     VK_DRIVER_ID_MESA_TURNIP,
                         MESA(26u, 0u, 3u),        0x43050a01,           SNX_VK_BAN_ANDROID};
        vk_bans_device(&d);
        g_test.nsaid = 0;
        CHECK(vk_ban("VK_SHADER_STAGE_GEOMETRY_BIT", 0, say) == 0 && g_test.nsaid == 0,
              "a Turnip that does not list the geometry stage is told nothing");
        CHECK(vk_ban("VK_SHADER_STAGE_GEOMETRY_BIT", 1, say) != 0, "Turnip's geometry stage is banned");
        CHECK(said_with("vulkan: VK_SHADER_STAGE_GEOMETRY_BIT is off on Turnip Adreno (TM) 740 (turnip Mesa driver, "
                        "Mesa 26.0.3, driver version 26.0.3): Turnip lists the geometry stage for subgroup operations",
                        "wrong ballot and shuffle results", "Eden PR 4344 (b846577037") >= 0,
              "the line says the stage, the device, the driver's version and why");
        if (said_with("VK_SHADER_STAGE_GEOMETRY_BIT", NULL, NULL) < 0) show_said();
    }
    /* native ASTC on Turnip: a warning at the filter with the device, its version and the way around, nothing off */
    {
        VkExtensionProperties list[1] = {{"VK_KHR_swapchain", 1}};
        SnxVkExtensions e = {list, 1};
        VkBanDevice d = {"Turnip Adreno (TM) 610", "turnip Mesa driver", "Mesa 26.3.0-devel",
                         VK_DRIVER_ID_MESA_TURNIP, MESA(26u, 2u, 99u),   0x6010001,
                         SNX_VK_BAN_ANDROID};
        vk_bans_device(&d);
        g_test.nsaid = 0;
        CHECK(vk_bans_filter(&e, say) == 0 && e.count == 1, "a warn line takes no extension out");
        int w = said_with("vulkan: a known problem the extension bans do not turn off: textureCompressionASTC_LDR on "
                          "Turnip Adreno (TM) 610 (turnip Mesa driver, Mesa 26.3.0-devel, driver version 26.2.99): "
                          "Turnip decodes an illegal or HDR ASTC block as transparent black",
                          "native_astc=0", "#1655");
        CHECK(w >= 0 && g_test.warned[w], "native ASTC on Turnip is a warning naming native_astc=0 and the issue");
        if (w < 0) show_said();
        CHECK(vk_ban_find("textureCompressionASTC_LDR") == 0, "and turns native ASTC off nowhere");
    }
    /* this machine's drivers, RADV and lavapipe: no "off" line holds; RADV's warnings are the lifted bans */
    static const struct {
        uint32_t id, v;
    } here[] = {{VK_DRIVER_ID_MESA_RADV, MESA(26u, 2u, 6u)}, {VK_DRIVER_ID_MESA_LLVMPIPE, MESA(26u, 2u, 6u)}};
    for (size_t i = 0; i < 2; i++) {
        device(here[i].id, here[i].v, 0x73bf, SNX_VK_BAN_LINUX);
        for (uint32_t line = vk_bans_holding(0); line; line = vk_bans_holding(line)) {
            char action[8], what[160];
            vk_ban_field(line, VK_BAN_ACTION, action, sizeof action);
            snprintf(what, sizeof what, "line %u holds for %s 26.2.6 as %s", (unsigned)line,
                     vk_ban_driver_name(here[i].id), action);
            CHECK(!strcmp(action, "warn"), what);
        }
    }
}

/* a game's .ini read into the Graphics table as the runtime reads its rows (ini_read) */
static void read_graphics(const char* text) {
    char line[700];
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

/* vulkan_ban_ignore: its row, a .ini with a key the table has and one with a key it has not, and what each does */
static void check_ignore(void) {
    char err[1024] = "";
    const IniOpt* o = ini_find(g_graphics_ini, "vulkan_ban_ignore");
    CHECK(o && o->type == INI_TEXT && o->def && !o->def[0] && (o->flags & INI_STARTUP) &&
              sizeof g_settings.vulkan_ban_ignore == 512,
          "vulkan_ban_ignore is a startup text of the Graphics table, empty by default");
    /* a key the table has: present_id2 stays in an NVIDIA 590's list, and the log says over which ban */
    read_graphics("[Display]\nfullscreen=0\n[Graphics]\nvulkan_ban_ignore=VK_KHR_present_id2\n");
    CHECK(!strcmp(g_settings.vulkan_ban_ignore, "VK_KHR_present_id2"), "vulkan_ban_ignore reads from the .ini");
    CHECK(vk_bans_load(snx_vk_extension_bans_text, err, sizeof err), err);
    CHECK(vk_bans_ignore(g_settings.vulkan_ban_ignore, err, sizeof err), err);
    VkExtensionProperties list[2] = {{"VK_KHR_present_id2", 1}, {"VK_KHR_present_wait2", 1}};
    SnxVkExtensions e = {list, 2};
    VkBanDevice nv = {"NVIDIA GeForce RTX 3060", "NVIDIA", "590.48.01",     VK_DRIVER_ID_NVIDIA_PROPRIETARY,
                      NV(590u, 48u, 1u),         0x2503,   SNX_VK_BAN_LINUX};
    vk_bans_device(&nv);
    g_test.nsaid = 0;
    vk_bans_ignored(say);
    CHECK(vk_bans_filter(&e, say) == 0 && e.count == 2, "the ban vulkan_ban_ignore goes over takes nothing out");
    int over = said_with("vulkan: VK_KHR_present_id2 is not banned on NVIDIA GeForce RTX 3060 (NVIDIA, 590.48.01, "
                         "driver version 590.48.01): vulkan_ban_ignore=VK_KHR_present_id2 goes over the ban: NVIDIA's "
                         "driver before 595.0.0 hangs games",
                         "extension_bans.txt:", NULL);
    CHECK(over >= 0 && g_test.warned[over], "the key gone over is a warning with the value and the ban");
    CHECK(said_with("VK_KHR_present_id2 is off", NULL, NULL) < 0, "and not said off");
    vk_bans_unclaimed(say);
    CHECK(count_with("VK_KHR_present_id2") == 1, "one line names present_id2 in all");
    if (count_with("VK_KHR_present_id2") != 1) show_said();
    /* the same key where no ban on it holds: said plainly, no warning */
    VkBanDevice nv595 = nv;
    nv595.driver_version = NV(595u, 0u, 0u);
    nv595.driver_info = "595.0.00";
    vk_bans_device(&nv595);
    g_test.nsaid = 0;
    vk_bans_ignored(say);
    CHECK(g_test.nsaid == 1 && !g_test.warned[0] &&
              said_with("vulkan: VK_KHR_present_id2 is not banned on NVIDIA GeForce RTX 3060 (NVIDIA, 595.0.00, driver "
                        "version 595.0.00), with or without vulkan_ban_ignore=VK_KHR_present_id2: no off line on it "
                        "holds there",
                        NULL, NULL) >= 0,
          "a key no ban holds for says so");
    if (g_test.nsaid != 1) show_said();
    /* the setting's own ban: dynamic vertex input on NVIDIA 575 holds no more */
    CHECK(vk_bans_ignore("VK_EXT_vertex_input_dynamic_state", err, sizeof err), err);
    VkBanDevice nv575 = nv;
    nv575.driver_version = NV(575u, 64u, 5u);
    vk_bans_device(&nv575);
    CHECK(vk_ban_find("VK_EXT_vertex_input_dynamic_state") == 0, "vulkan_ban_ignore goes over an owned ban too");
    CHECK(vk_bans_ignore("", err, sizeof err) && vk_ban_find("VK_EXT_vertex_input_dynamic_state") != 0,
          "an empty vulkan_ban_ignore lets every ban hold again");

    /* a key the table has not: refused with the key and the nearest ones it has */
    read_graphics("[Graphics]\nvulkan_ban_ignore=VK_KHR_present_id\n");
    CHECK(!strcmp(g_settings.vulkan_ban_ignore, "VK_KHR_present_id"), "the wrong key reads from the .ini as written");
    CHECK(!vk_bans_ignore(g_settings.vulkan_ban_ignore, err, sizeof err) &&
              strstr(err, "vulkan_ban_ignore=VK_KHR_present_id: VK_KHR_present_id is no key of the extension bans; "
                          "the nearest it has: VK_KHR_present_id2,"),
          err);
    CHECK(!vk_bans_ignore("shaderFloat16, VK_EXT_custom_border_colour", err, sizeof err) &&
              strstr(err, ": VK_EXT_custom_border_colour is no key of the extension bans; the nearest it has: "
                          "VK_EXT_custom_border_color,"),
          err);
    char longer[600];
    memset(longer, 'a', sizeof longer - 1);
    longer[sizeof longer - 1] = 0;
    CHECK(!vk_bans_ignore(longer, err, sizeof err) &&
              strstr(err, "vulkan_ban_ignore is 599 bytes, past the 511 it holds"),
          err);

    /* the stand-in table: an off feature and an off extension gone over, a warning's key, separators */
    CHECK(vk_bans_load(g_table, err, sizeof err), err);
    CHECK(vk_bans_ignore(" featureD ,, VK_EXT_b,featureF ", err, sizeof err), err);
    VkExtensionProperties radv[2] = {{"VK_EXT_b", 1}, {"VK_EXT_j", 1}};
    SnxVkExtensions er = {radv, 2};
    device(VK_DRIVER_ID_MESA_RADV, MESA(26u, 0u, 4u), 0x73bf, SNX_VK_BAN_LINUX);
    g_test.nsaid = 0;
    vk_bans_ignored(say);
    CHECK(said_with("vulkan: featureD is not banned on", "goes over the ban: RADV up to 26.0.99", NULL) >= 0 &&
              said_with("vulkan: VK_EXT_b is not banned on",
                        "vulkan_ban_ignore= featureD ,, VK_EXT_b,featureF  goes over the ban: RADV from 25.0.3",
                        NULL) >= 0 &&
              said_with("vulkan: featureF is not banned on", "no off line on it holds there", NULL) >= 0 &&
              g_test.nsaid == 3,
          "each key of the list said once, with the value as given");
    if (g_test.nsaid != 3) show_said();
    g_test.nsaid = 0;
    CHECK(vk_bans_filter(&er, say) == 1 && er.count == 1 && !strcmp(radv[0].extensionName, "VK_EXT_b"),
          "the filter leaves VK_EXT_b, which both its lines ban, and takes VK_EXT_j");
    CHECK(vk_ban("featureD", 1, say) == 0 && said_with("featureD is off", NULL, NULL) < 0, "featureD stays on unsaid");
    vk_bans_unclaimed(say);
    CHECK(said_with("VK_EXT_b", NULL, NULL) < 0 && said_with("featureD", NULL, NULL) < 0,
          "a ban gone over is no unclaimed one");
    CHECK(vk_bans_load(g_table, err, sizeof err) && vk_ban_find("featureD") == 6,
          "a new load lets every ban hold again");
}

/* text as a whole file */
static int read_table(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        printf("error\t%s can't be opened\n", path);
        return 0;
    }
    size_t n = fread(g_test.table, 1, sizeof g_test.table - 1, f);
    int more = fgetc(f) != EOF;
    fclose(f);
    g_test.table[n] = 0;
    if (more) printf("error\t%s is past the %zu bytes this reads\n", path, sizeof g_test.table - 1);
    return !more;
}

/* the lines that hold for the device given, a line each: ban, line, key, action, whether the device has the extension
   (yes, no, or - for a feature), retest, checked, override, why */
static void print_holding(const SnxVkExtensions* e) {
    for (uint32_t line = vk_bans_holding(0); line; line = vk_bans_holding(line)) {
        char f[VK_BAN_FIELDS][600], why[1200];
        for (int i = 0; i < VK_BAN_FIELDS; i++) vk_ban_field(line, i, f[i], sizeof f[i]);
        vk_ban_why(line, why, sizeof why);
        int extension = !strncmp(f[VK_BAN_KEY], "VK_", 3) && !strchr(f[VK_BAN_KEY], '.') &&
                        strpbrk(f[VK_BAN_KEY], "abcdefghijklmnopqrstuvwxyz");
        const char* has = !extension || !e ? "-" : snx_vk_extensions_has(e, f[VK_BAN_KEY]) ? "yes" : "no";
        printf("ban\t%u\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", (unsigned)line, f[VK_BAN_KEY], f[VK_BAN_ACTION], has,
               f[VK_BAN_RETEST], f[VK_BAN_CHECKED], f[VK_BAN_OVERRIDE], why);
    }
}

static int load_for_use(const char* path) {
    char err[512];
    if (path && !read_table(path)) return 0;
    if (!vk_bans_load(path ? g_test.table : snx_vk_extension_bans_text, err, sizeof err)) {
        printf("error\t%s\n", err);
        return 0;
    }
    return 1;
}

/* this machine's devices, each with the lines that hold for it */
static int run_devices(const char* path) {
    if (!load_for_use(path)) return 2;
    if (volkInitialize() != VK_SUCCESS) {
        printf("error\tno Vulkan loader\n");
        return 2;
    }
    SnxVkInstanceInfo info = {.app_name = "snx-test-vk-bans", .portability = true};
    SnxVkInstance made;
    VkResult r = snx_vk_instance_create(&info, &made);
    if (r != VK_SUCCESS) {
        printf("error\tvkCreateInstance: %s\n", snx_vk_result_str(r));
        return 2;
    }
    VkPhysicalDevice phys[16];
    uint32_t n = 16;
    r = vkEnumeratePhysicalDevices(made.instance, &n, phys);
    if (r < 0) n = 0;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceDriverProperties dp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 pp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &dp};
        vkGetPhysicalDeviceProperties2(phys[i], &pp);
        char version[32];
        const char* driver = vk_ban_driver_name((uint32_t)dp.driverID);
        vk_driver_version_text((uint32_t)dp.driverID, pp.properties.driverVersion, version, sizeof version);
        printf("device\t%u\t%s\t%s\t%s\t%s\t%s\t%#x\n", (unsigned)i, pp.properties.deviceName, driver ? driver : "?",
               version, dp.driverName, dp.driverInfo, (unsigned)pp.properties.deviceID);
        VkBanDevice d = {pp.properties.deviceName,    dp.driverName,          dp.driverInfo,  (uint32_t)dp.driverID,
                         pp.properties.driverVersion, pp.properties.deviceID, vk_ban_system()};
        SnxVkExtensions e;
        int listed = snx_vk_device_extensions(phys[i], &e);
        vk_bans_device(&d);
        print_holding(listed ? &e : NULL);
        if (listed) snx_vk_extensions_free(&e);
    }
    if (!n) printf("error\tno Vulkan device\n");
    snx_vk_instance_free(&made);
    return n ? 0 : 2;
}

/* the lines that hold for a driver given by hand */
static int run_match(int argc, char** argv, const char* path) {
    static const char* const systems[] = {"linux", "android", "windows", "macos",
                                          "ios",   "freebsd", "openbsd", "netbsd"};
    uint32_t id = argc > 0 ? vk_ban_driver_id(argv[0]) : 0, v = 0, system = vk_ban_system(), gpu = 0;
    char why[256];
    if (!id || argc < 2 || !vk_ban_parse_version(id, argv[1], &v, why, sizeof why)) {
        printf("error\t--match takes a driver the table names, its version in its numbering (%s), a system and a "
               "deviceID\n",
               id ? why : "the driver first");
        return 2;
    }
    if (argc > 2) {
        system = 0;
        for (uint32_t s = 0; s < 8; s++)
            if (!strcmp(argv[2], systems[s])) system = 1u << s;
        if (!system) {
            printf("error\tsystem %s is none of linux, android, windows, macos, ios, freebsd, openbsd and netbsd\n",
                   argv[2]);
            return 2;
        }
    }
    if (argc > 3) gpu = (uint32_t)strtoul(argv[3], NULL, 16);
    if (!load_for_use(path)) return 2;
    char version[32];
    vk_driver_version_text(id, v, version, sizeof version);
    printf("device\t-\t(given)\t%s\t%s\t%s\t-\t%#x\n", argv[0], version, argc > 2 ? argv[2] : "this system",
           (unsigned)gpu);
    VkBanDevice d = {"(given)", argv[0], argv[1], id, v, gpu, system};
    vk_bans_device(&d);
    print_holding(NULL);
    return 0;
}

int main(int argc, char** argv) {
    const char* table = NULL;
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--table")) table = argv[i + 1];
    if (argc > 1 && !strcmp(argv[1], "--devices")) return run_devices(table);
    if (argc > 1 && !strcmp(argv[1], "--match")) {
        int n = 0;
        while (2 + n < argc && strcmp(argv[2 + n], "--table")) n++;
        return run_match(n, argv + 2, table);
    }
    if (argc == 3 && !strcmp(argv[1], "--check")) {
        if (!load_for_use(argv[2])) return 1;
        printf("ok\t%s reads\n", argv[2]);
        return 0;
    }
    if (argc > 1) {
        printf("error\tunknown arguments: --devices, --match DRIVER VERSION [SYSTEM [DEVICEID]], --check FILE, and "
               "--table FILE with the first two\n");
        return 2;
    }
    check_errors();
    check_versions();
    check_matching();
    check_filter();
    check_settings();
    check_shipped();
    ini_set_say(say_ini);
    check_ignore();
    printf("%s: %u checks, %u failed\n", g_test.failures ? "FAIL" : "PASS", g_test.checks, g_test.failures);
    return g_test.failures != 0;
}
