/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* extension_bans.txt read, matched against the device and said (extension_bans.h) */
#include "nx/gpu/vk/extension_bans.h"
#include <stdio.h>
#include <string.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#define BAN_TEXT  65536 /* the table's bytes, the copy kept here */
#define BAN_LINES 192
#define BAN_GPUS  4

/* how a driver numbers its versions in driverVersion */
enum { VERSION_VULKAN, VERSION_NVIDIA, VERSION_INTEL_WINDOWS, VERSION_MOLTENVK };
/* a version bound's comparison: none, then >= and > for a lower bound, < and <= for an upper one */
enum { BOUND_NONE, BOUND_FROM, BOUND_AFTER, BOUND_BEFORE, BOUND_UP_TO };

typedef struct {
    uint32_t line;                                   /* in the file, from 1 */
    uint32_t at[VK_BAN_FIELDS], size[VK_BAN_FIELDS]; /* each field in g_extension_bans.text */
    uint32_t driver, low, high, systems, gpus[BAN_GPUS], ngpus;
    uint32_t low_at, low_size, high_at, high_size; /* the bounds' versions as the table writes them */
    unsigned char off, low_bound, high_bound, extension, claimed;
    unsigned char ignored, forced; /* gone over: by vulkan_ban_ignore, by the extension's setting set on */
} Ban;

#define EXTENSION_SETTINGS 160
/* the extension settings as vk_extension_settings found them, for vk_extension_settings_say */
enum { SETTING_AUTO, SETTING_ON, SETTING_OFF };
static struct {
    const char* name[EXTENSION_SETTINGS];
    unsigned char value[EXTENSION_SETTINGS], had[EXTENSION_SETTINGS], core[EXTENSION_SETTINGS];
    uint32_t count;
} g_extension_settings;

static struct {
    char text[BAN_TEXT];
    Ban bans[BAN_LINES];
    uint32_t count;
    char device[256], driver_name[256], driver_info[256];
    uint32_t driver_id, driver_version, device_id, system;
    char ignore[512]; /* vulkan_ban_ignore's value, as given */
} g_extension_bans;

/* the table's names and the log's for each VkDriverId */
static const char* const driver_names[] = {
    [VK_DRIVER_ID_AMD_PROPRIETARY] = "amd-proprietary",
    [VK_DRIVER_ID_AMD_OPEN_SOURCE] = "amdvlk",
    [VK_DRIVER_ID_MESA_RADV] = "radv",
    [VK_DRIVER_ID_NVIDIA_PROPRIETARY] = "nvidia",
    [VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS] = "intel-windows",
    [VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA] = "anv",
    [VK_DRIVER_ID_IMAGINATION_PROPRIETARY] = "powervr",
    [VK_DRIVER_ID_QUALCOMM_PROPRIETARY] = "qualcomm",
    [VK_DRIVER_ID_ARM_PROPRIETARY] = "mali",
    [VK_DRIVER_ID_GOOGLE_SWIFTSHADER] = "swiftshader",
    [VK_DRIVER_ID_GGP_PROPRIETARY] = "ggp",
    [VK_DRIVER_ID_BROADCOM_PROPRIETARY] = "broadcom",
    [VK_DRIVER_ID_MESA_LLVMPIPE] = "lavapipe",
    [VK_DRIVER_ID_MOLTENVK] = "moltenvk",
    [VK_DRIVER_ID_COREAVI_PROPRIETARY] = "coreavi",
    [VK_DRIVER_ID_JUICE_PROPRIETARY] = "juice",
    [VK_DRIVER_ID_VERISILICON_PROPRIETARY] = "verisilicon",
    [VK_DRIVER_ID_MESA_TURNIP] = "turnip",
    [VK_DRIVER_ID_MESA_V3DV] = "v3dv",
    [VK_DRIVER_ID_MESA_PANVK] = "panvk",
    [VK_DRIVER_ID_SAMSUNG_PROPRIETARY] = "samsung",
    [VK_DRIVER_ID_MESA_VENUS] = "venus",
    [VK_DRIVER_ID_MESA_DOZEN] = "dozen",
    [VK_DRIVER_ID_MESA_NVK] = "nvk",
    [VK_DRIVER_ID_IMAGINATION_OPEN_SOURCE_MESA] = "powervr-mesa",
    [VK_DRIVER_ID_MESA_HONEYKRISP] = "honeykrisp",
    [VK_DRIVER_ID_VULKAN_SC_EMULATION_ON_VULKAN] = "vulkansc",
    [VK_DRIVER_ID_MESA_KOSMICKRISP] = "kosmickrisp",
    [VK_DRIVER_ID_MESA_GFXSTREAM] = "gfxstream",
    [VK_DRIVER_ID_APE_SOFT] = "apesoft",
};
static const char* const driver_labels[] = {
    [VK_DRIVER_ID_AMD_PROPRIETARY] = "AMD's proprietary driver",
    [VK_DRIVER_ID_AMD_OPEN_SOURCE] = "AMDVLK",
    [VK_DRIVER_ID_MESA_RADV] = "RADV",
    [VK_DRIVER_ID_NVIDIA_PROPRIETARY] = "NVIDIA's driver",
    [VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS] = "Intel's Windows driver",
    [VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA] = "ANV",
    [VK_DRIVER_ID_IMAGINATION_PROPRIETARY] = "Imagination's PowerVR driver",
    [VK_DRIVER_ID_QUALCOMM_PROPRIETARY] = "Qualcomm's driver",
    [VK_DRIVER_ID_ARM_PROPRIETARY] = "Arm's Mali driver",
    [VK_DRIVER_ID_GOOGLE_SWIFTSHADER] = "SwiftShader",
    [VK_DRIVER_ID_GGP_PROPRIETARY] = "GGP's driver",
    [VK_DRIVER_ID_BROADCOM_PROPRIETARY] = "Broadcom's driver",
    [VK_DRIVER_ID_MESA_LLVMPIPE] = "lavapipe",
    [VK_DRIVER_ID_MOLTENVK] = "MoltenVK",
    [VK_DRIVER_ID_COREAVI_PROPRIETARY] = "CoreAVI's driver",
    [VK_DRIVER_ID_JUICE_PROPRIETARY] = "Juice's driver",
    [VK_DRIVER_ID_VERISILICON_PROPRIETARY] = "VeriSilicon's driver",
    [VK_DRIVER_ID_MESA_TURNIP] = "Turnip",
    [VK_DRIVER_ID_MESA_V3DV] = "v3dv",
    [VK_DRIVER_ID_MESA_PANVK] = "PanVK",
    [VK_DRIVER_ID_SAMSUNG_PROPRIETARY] = "Samsung's Xclipse driver",
    [VK_DRIVER_ID_MESA_VENUS] = "Venus",
    [VK_DRIVER_ID_MESA_DOZEN] = "Dozen",
    [VK_DRIVER_ID_MESA_NVK] = "NVK",
    [VK_DRIVER_ID_IMAGINATION_OPEN_SOURCE_MESA] = "Imagination's Mesa driver",
    [VK_DRIVER_ID_MESA_HONEYKRISP] = "Honeykrisp",
    [VK_DRIVER_ID_VULKAN_SC_EMULATION_ON_VULKAN] = "Vulkan SC emulation",
    [VK_DRIVER_ID_MESA_KOSMICKRISP] = "KosmicKrisp",
    [VK_DRIVER_ID_MESA_GFXSTREAM] = "gfxstream",
    [VK_DRIVER_ID_APE_SOFT] = "APE Soft's driver",
};
#define DRIVERS (sizeof driver_names / sizeof *driver_names)
/* the table's system names, SNX_VK_BAN_*'s bits in order, and the log's */
static const char* const system_names[] = {"linux", "android", "windows", "macos",
                                           "ios",   "freebsd", "openbsd", "netbsd"};
static const char* const system_labels[] = {"Linux", "Android", "Windows", "macOS",
                                            "iOS",   "FreeBSD", "OpenBSD", "NetBSD"};
#define SYSTEMS (sizeof system_names / sizeof *system_names)
static const char header[] =
    "key | action | driver | versions | system | devices | breaks | source | retest | checked | override";

uint32_t vk_ban_system(void) {
#if defined(__ANDROID__)
    return SNX_VK_BAN_ANDROID;
#elif defined(_WIN32)
    return SNX_VK_BAN_WINDOWS;
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    return SNX_VK_BAN_IOS;
#elif defined(__APPLE__)
    return SNX_VK_BAN_MACOS;
#elif defined(__FreeBSD__)
    return SNX_VK_BAN_FREEBSD;
#elif defined(__OpenBSD__)
    return SNX_VK_BAN_OPENBSD;
#elif defined(__NetBSD__)
    return SNX_VK_BAN_NETBSD;
#elif defined(__linux__)
    return SNX_VK_BAN_LINUX;
#else
    return 0;
#endif
}

const char* vk_ban_driver_name(uint32_t id) { return id < DRIVERS ? driver_names[id] : NULL; }
uint32_t vk_ban_driver_id(const char* name) {
    for (uint32_t id = 1; id < DRIVERS; id++)
        if (driver_names[id] && !strcmp(driver_names[id], name)) return id;
    return 0;
}

static int version_kind(uint32_t id) {
    return id == VK_DRIVER_ID_NVIDIA_PROPRIETARY          ? VERSION_NVIDIA
           : id == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS ? VERSION_INTEL_WINDOWS
           : id == VK_DRIVER_ID_MOLTENVK                  ? VERSION_MOLTENVK
                                                          : VERSION_VULKAN;
}

void vk_driver_version_text(uint32_t id, uint32_t v, char* out, size_t n) {
    switch (version_kind(id)) {
        case VERSION_NVIDIA: snprintf(out, n, "%u.%u.%02u", v >> 22, v >> 14 & 0xffu, v >> 6 & 0xffu); break;
        case VERSION_INTEL_WINDOWS: snprintf(out, n, "%u.%u", v >> 14, v & 0x3fffu); break;
        case VERSION_MOLTENVK: snprintf(out, n, "%u.%u.%u", v / 10000u, v / 100u % 100u, v % 100u); break;
        default: /* VK_MAKE_VERSION's fields: a major of 10 bits, as Qualcomm's 512 needs */
            snprintf(out, n, "%u.%u.%u", v >> 22, v >> 12 & 0x3ffu, v & 0xfffu);
    }
}

/* the text from at, size bytes of the copy, into out */
static void copy_text(uint32_t at, uint32_t size, char* out, size_t n) {
    if (!n) return;
    size_t take = size < n - 1 ? size : n - 1;
    memcpy(out, g_extension_bans.text + at, take);
    out[take] = 0;
}
static int field_is(const Ban* b, int field, const char* word) {
    return b->size[field] == strlen(word) && !memcmp(g_extension_bans.text + b->at[field], word, b->size[field]);
}

/* A version as driver id numbers it, from text's size bytes, into *v: 1, or 0 with why */
static int parse_version(uint32_t id, const char* text, uint32_t size, uint32_t* v, char* why, size_t n) {
    uint32_t part[4] = {0}, parts = 0, at = 0;
    while (at < size) {
        uint64_t value = 0;
        uint32_t digits = 0;
        while (at < size && text[at] >= '0' && text[at] <= '9' && digits < 10)
            value = value * 10 + (uint32_t)(text[at++] - '0'), digits++;
        if (!digits || value > 0xffffffffu || parts == 4 || (at < size && text[at] != '.') ||
            (at < size && at + 1 == size)) {
            snprintf(why, n, "\"%.*s\" is not numbers between dots", (int)size, text);
            return 0;
        }
        part[parts++] = (uint32_t)value;
        at += at < size; /* the dot */
    }
    switch (version_kind(id)) {
        case VERSION_NVIDIA:
            if (parts < 2 || part[0] > 1023 || part[1] > 255 || part[2] > 255 || part[3] > 63) {
                snprintf(why, n, "\"%.*s\" is not NVIDIA's A.B[.C[.D]], A to 1023, B and C to 255, D to 63", (int)size,
                         text);
                return 0;
            }
            *v = part[0] << 22 | part[1] << 14 | part[2] << 6 | part[3];
            return 1;
        case VERSION_INTEL_WINDOWS:
            /* the driver gives the last two of its four numbers */
            if ((parts != 2 && parts != 4) || part[parts - 2] > 0x3ffffu || part[parts - 1] > 0x3fffu) {
                snprintf(
                    why, n,
                    "\"%.*s\" is not Intel's four numbers or their last two, the third to 262143 and the fourth to "
                    "16383",
                    (int)size, text);
                return 0;
            }
            *v = part[parts - 2] << 14 | part[parts - 1];
            return 1;
        case VERSION_MOLTENVK:
            if (parts < 2 || parts > 3 || part[0] > 429495 || part[1] > 99 || part[2] > 99) {
                snprintf(why, n, "\"%.*s\" is not MoltenVK's A.B[.C], B and C to 99", (int)size, text);
                return 0;
            }
            *v = part[0] * 10000 + part[1] * 100 + part[2];
            return 1;
        default:
            if (parts < 2 || parts > 3 || part[0] > 1023 || part[1] > 1023 || part[2] > 4095) {
                snprintf(why, n, "\"%.*s\" is not A.B[.C] of VK_MAKE_VERSION, A and B to 1023, C to 4095", (int)size,
                         text);
                return 0;
            }
            *v = part[0] << 22 | part[1] << 12 | part[2];
            return 1;
    }
}

int vk_ban_parse_version(uint32_t id, const char* text, uint32_t* v, char* why, size_t n) {
    return parse_version(id, text, (uint32_t)strlen(text), v, why, n);
}

/* versions' text into b's bounds: 1, or 0 with why */
static int parse_versions(Ban* b, char* why, size_t n) {
    const char* text = g_extension_bans.text + b->at[VK_BAN_VERSIONS];
    uint32_t size = b->size[VK_BAN_VERSIONS], at = 0;
    if (size == 1 && *text == '*') return 1;
    for (int bounds = 0; at < size; bounds++) {
        int op = text[at] == '<'   ? (at + 1 < size && text[at + 1] == '=' ? BOUND_UP_TO : BOUND_BEFORE)
                 : text[at] == '>' ? (at + 1 < size && text[at + 1] == '=' ? BOUND_FROM : BOUND_AFTER)
                                   : BOUND_NONE;
        int lower = op == BOUND_FROM || op == BOUND_AFTER;
        if (op == BOUND_NONE || bounds == 2 || (bounds == 1 && (lower || b->high_bound))) {
            snprintf(why, n,
                     "versions \"%.*s\" are not *, one bound (<, <=, >= or > and a version) or a lower one then an "
                     "upper one",
                     (int)size, text);
            return 0;
        }
        at += op == BOUND_UP_TO || op == BOUND_FROM ? 2 : 1;
        while (at < size && text[at] == ' ') at++;
        uint32_t start = at, v;
        while (at < size && text[at] != ' ') at++;
        if (!parse_version(b->driver, text + start, at - start, &v, why, n)) return 0;
        if (lower)
            b->low_bound = (unsigned char)op, b->low = v, b->low_at = b->at[VK_BAN_VERSIONS] + start,
            b->low_size = at - start;
        else
            b->high_bound = (unsigned char)op, b->high = v, b->high_at = b->at[VK_BAN_VERSIONS] + start,
            b->high_size = at - start;
        while (at < size && text[at] == ' ') at++;
    }
    /* the first version and the last each bound holds for */
    int64_t first = b->low_bound == BOUND_AFTER ? (int64_t)b->low + 1 : b->low_bound ? (int64_t)b->low : 0;
    int64_t last = b->high_bound == BOUND_BEFORE  ? (int64_t)b->high - 1
                   : b->high_bound == BOUND_UP_TO ? (int64_t)b->high
                                                  : (int64_t)0xffffffffu;
    if (first > last) {
        snprintf(why, n, "versions \"%.*s\" hold for no version", (int)size, text);
        return 0;
    }
    return 1;
}

/* a list's next item from *at in text's size bytes, separated by commas: its start and size, 0 past the end */
static int next_item(const char* text, uint32_t size, uint32_t* at, uint32_t* start, uint32_t* item) {
    while (*at < size && (text[*at] == ' ' || text[*at] == ',')) ++*at;
    if (*at >= size) return 0;
    *start = *at;
    while (*at < size && text[*at] != ',') ++*at;
    *item = *at - *start;
    while (*item && text[*start + *item - 1] == ' ') --*item;
    return 1;
}

static int parse_systems(Ban* b, char* why, size_t n) {
    const char* text = g_extension_bans.text + b->at[VK_BAN_SYSTEM];
    uint32_t size = b->size[VK_BAN_SYSTEM], at = 0, start, item;
    if (field_is(b, VK_BAN_SYSTEM, "any")) return 1;
    while (next_item(text, size, &at, &start, &item)) {
        uint32_t s = 0;
        while (s < SYSTEMS && (item != strlen(system_names[s]) || memcmp(text + start, system_names[s], item))) s++;
        if (s == SYSTEMS) {
            snprintf(why, n,
                     "system \"%.*s\" is not any or a list of linux, android, windows, macos, ios, freebsd, openbsd "
                     "and netbsd",
                     (int)size, text);
            return 0;
        }
        b->systems |= 1u << s;
    }
    if (!b->systems) {
        snprintf(why, n, "system is empty: any, or the systems it holds on");
        return 0;
    }
    return 1;
}

static int parse_gpus(Ban* b, char* why, size_t n) {
    const char* text = g_extension_bans.text + b->at[VK_BAN_DEVICES];
    uint32_t size = b->size[VK_BAN_DEVICES], at = 0, start, item;
    if (field_is(b, VK_BAN_DEVICES, "*")) return 1;
    while (next_item(text, size, &at, &start, &item)) {
        uint32_t id = 0, i = 2;
        int hex = item > 2 && item <= 10 && text[start] == '0' && text[start + 1] == 'x';
        for (; hex && i < item; i++) {
            char c = text[start + i];
            int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (digit < 0) hex = 0;
            id = id << 4 | (uint32_t)(digit & 15);
        }
        if (!hex || b->ngpus == BAN_GPUS) {
            snprintf(why, n, "devices \"%.*s\" are not * or up to %d deviceIDs, each 0x and lowercase hex", (int)size,
                     text, BAN_GPUS);
            return 0;
        }
        b->gpus[b->ngpus++] = id;
    }
    if (!b->ngpus) {
        snprintf(why, n, "devices is empty: *, or the deviceIDs it holds on");
        return 0;
    }
    return 1;
}

/* key's characters: an extension's or a feature's, an enum's, a struct's member or a call's name */
static int parse_key(Ban* b, char* why, size_t n) {
    const char* key = g_extension_bans.text + b->at[VK_BAN_KEY];
    uint32_t size = b->size[VK_BAN_KEY], dots = 0, lower = 0, ok = size > 0;
    for (uint32_t i = 0; i < size && ok; i++) {
        char c = key[i];
        if (c == '.')
            ok = ++dots == 1 && i > 0 && i + 1 < size && key[i + 1] >= 'a' && key[i + 1] <= 'z';
        else
            ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        lower |= c >= 'a' && c <= 'z';
    }
    if (!ok || !((key[0] >= 'a' && key[0] <= 'z') || (key[0] >= 'A' && key[0] <= 'Z'))) {
        snprintf(why, n, "key \"%.*s\" is not a Vulkan name: an extension, feature, state, format or call", (int)size,
                 key);
        return 0;
    }
    /* VK_EXT_custom_border_color: an extension's name has lowercase letters after VK_, an enum's none */
    b->extension = size > 3 && !memcmp(key, "VK_", 3) && lower && !dots;
    return 1;
}

static int parse_retest(const Ban* b, char* why, size_t n) {
    static const char* const kinds[] = {"headless:", "edenprobe:", "new:headless:", "new:edenprobe:"};
    const char* text = g_extension_bans.text + b->at[VK_BAN_RETEST];
    uint32_t size = b->size[VK_BAN_RETEST];
    for (int k = 0; k < 4; k++) {
        size_t kind = strlen(kinds[k]);
        /* vulkan_headless with no arguments is a case; an edenprobe suite and a case to write need their words */
        if (size >= kind && !memcmp(text, kinds[k], kind) && (k == 0 || size > kind)) return 1;
    }
    snprintf(why, n, "retest \"%.*s\" is not headless:ARGS, edenprobe:SUITE, new:headless:WHAT or new:edenprobe:WHAT",
             (int)size, text);
    return 0;
}

static int parse_checked(const Ban* b, char* why, size_t n) {
    const char* text = g_extension_bans.text + b->at[VK_BAN_CHECKED];
    uint32_t size = b->size[VK_BAN_CHECKED];
    if (field_is(b, VK_BAN_CHECKED, "never")) return 1;
    int ok = size > 11 && text[10] == ' ';
    for (int i = 0; i < 10 && ok; i++) ok = i == 4 || i == 7 ? text[i] == '-' : text[i] >= '0' && text[i] <= '9';
    if (!ok) snprintf(why, n, "checked \"%.*s\" is not never, or a date YYYY-MM-DD and what was seen", (int)size, text);
    return ok;
}

static int parse_override(const Ban* b, char* why, size_t n) {
    const char* text = g_extension_bans.text + b->at[VK_BAN_OVERRIDE];
    uint32_t size = b->size[VK_BAN_OVERRIDE], at = 0;
    if (field_is(b, VK_BAN_OVERRIDE, "-")) return 1;
    while (at < size &&
           ((text[at] >= 'a' && text[at] <= 'z') || (text[at] >= '0' && text[at] <= '9') || text[at] == '_'))
        at++;
    if (!at || at + 1 >= size || text[at] != '=') {
        snprintf(why, n, "override \"%.*s\" is not - or a setting's key=value", (int)size, text);
        return 0;
    }
    if (!b->off) {
        snprintf(why, n, "override \"%.*s\" is for an off line: a warn line turns nothing off", (int)size, text);
        return 0;
    }
    return 1;
}

/* the line at text (its line number number) cut into b's fields at the bars, each without the spaces around it: how
   many fields it has */
static uint32_t split(Ban* b, uint32_t at, uint32_t end) {
    uint32_t fields = 0;
    while (at <= end) {
        uint32_t stop = at;
        while (stop < end && g_extension_bans.text[stop] != '|') stop++;
        uint32_t start = at, last = stop;
        while (start < last && (g_extension_bans.text[start] == ' ' || g_extension_bans.text[start] == '\t')) start++;
        while (last > start && (g_extension_bans.text[last - 1] == ' ' || g_extension_bans.text[last - 1] == '\t' ||
                                g_extension_bans.text[last - 1] == '\r'))
            last--;
        if (fields < VK_BAN_FIELDS) b->at[fields] = start, b->size[fields] = last - start;
        fields++;
        at = stop + 1;
    }
    return fields;
}

static int parse_ban(Ban* b, char* why, size_t n) {
    static const char* const empty[] = {[VK_BAN_BREAKS] = "breaks: what goes wrong",
                                        [VK_BAN_SOURCE] = "source: where it is known from"};
    if (!parse_key(b, why, n)) return 0;
    if (!field_is(b, VK_BAN_ACTION, "off") && !field_is(b, VK_BAN_ACTION, "warn")) {
        snprintf(why, n, "action \"%.*s\" is not off or warn", (int)b->size[VK_BAN_ACTION],
                 g_extension_bans.text + b->at[VK_BAN_ACTION]);
        return 0;
    }
    b->off = field_is(b, VK_BAN_ACTION, "off");
    b->driver = 1;
    while (b->driver < DRIVERS && !(driver_names[b->driver] && field_is(b, VK_BAN_DRIVER, driver_names[b->driver])))
        b->driver++;
    if (b->driver == DRIVERS) {
        snprintf(why, n, "driver \"%.*s\" is none of the drivers the table's head names", (int)b->size[VK_BAN_DRIVER],
                 g_extension_bans.text + b->at[VK_BAN_DRIVER]);
        return 0;
    }
    if (!parse_versions(b, why, n) || !parse_systems(b, why, n) || !parse_gpus(b, why, n)) return 0;
    for (int f = VK_BAN_BREAKS; f <= VK_BAN_SOURCE; f++)
        if (!b->size[f] || field_is(b, f, "-")) {
            snprintf(why, n, "%s is empty", empty[f]);
            return 0;
        }
    return parse_retest(b, why, n) && parse_checked(b, why, n) && parse_override(b, why, n);
}

int vk_bans_load(const char* text, char* err, size_t n) {
    size_t size = strlen(text);
    uint32_t at = 0, line = 0;
    int headed = 0;
    char why[400];
    g_extension_bans.count = 0;
    g_extension_bans.ignore[0] = 0;
    if (size >= BAN_TEXT) {
        snprintf(err, n, "extension_bans.txt is %zu bytes, past the %d this reader holds", size, BAN_TEXT - 1);
        return 0;
    }
    memcpy(g_extension_bans.text, text, size + 1);
    while (at < size) {
        uint32_t end = at, first;
        while (end < size && text[end] != '\n') end++;
        line++;
        for (first = at; first < end && (text[first] == ' ' || text[first] == '\t' || text[first] == '\r');) first++;
        if (first == end || text[first] == '#') {
            at = end + 1;
            continue;
        }
        Ban b = {.line = line};
        uint32_t fields = split(&b, first, end);
        if (!headed) {
            /* the header's names, each as the table writes it */
            char seen[sizeof header + 64] = "";
            for (uint32_t f = 0; f < fields && f < VK_BAN_FIELDS; f++)
                snprintf(seen + strlen(seen), sizeof seen - strlen(seen), "%s%.*s", f ? " | " : "", (int)b.size[f],
                         text + b.at[f]);
            if (fields != VK_BAN_FIELDS || strcmp(seen, header)) {
                snprintf(err, n,
                         "extension_bans.txt:%u: the first line that is not a comment is \"%.*s\", not the "
                         "header \"%s\"",
                         line, (int)(end - first), text + first, header);
                return 0;
            }
            headed = 1;
        } else if (fields != VK_BAN_FIELDS) {
            snprintf(err, n, "extension_bans.txt:%u: %u fields, the header names %d", line, fields, VK_BAN_FIELDS);
            return 0;
        } else if (!parse_ban(&b, why, sizeof why)) {
            snprintf(err, n, "extension_bans.txt:%u: %s", line, why);
            return 0;
        } else if (g_extension_bans.count == BAN_LINES) {
            snprintf(err, n, "extension_bans.txt:%u: past the %d bans this reader holds", line, BAN_LINES);
            return 0;
        } else
            g_extension_bans.bans[g_extension_bans.count++] = b;
        at = end + 1;
    }
    if (!headed) {
        snprintf(err, n, "extension_bans.txt has no header \"%s\"", header);
        return 0;
    }
    return 1;
}

void vk_bans_device(const VkBanDevice* d) {
    snprintf(g_extension_bans.device, sizeof g_extension_bans.device, "%s", d->device);
    snprintf(g_extension_bans.driver_name, sizeof g_extension_bans.driver_name, "%s", d->driver_name);
    snprintf(g_extension_bans.driver_info, sizeof g_extension_bans.driver_info, "%s", d->driver_info);
    g_extension_bans.driver_id = d->driver_id;
    g_extension_bans.driver_version = d->driver_version;
    g_extension_bans.device_id = d->device_id;
    g_extension_bans.system = d->system;
    for (uint32_t i = 0; i < g_extension_bans.count; i++) g_extension_bans.bans[i].claimed = 0;
}

static int holds(const Ban* b) {
    uint32_t v = g_extension_bans.driver_version, gpu = !b->ngpus;
    for (uint32_t i = 0; i < b->ngpus; i++) gpu |= b->gpus[i] == g_extension_bans.device_id;
    return b->driver == g_extension_bans.driver_id && gpu && (!b->systems || (b->systems & g_extension_bans.system)) &&
           (b->low_bound != BOUND_FROM || v >= b->low) && (b->low_bound != BOUND_AFTER || v > b->low) &&
           (b->high_bound != BOUND_BEFORE || v < b->high) && (b->high_bound != BOUND_UP_TO || v <= b->high);
}

static const Ban* ban_at(uint32_t line) {
    for (uint32_t i = 0; i < g_extension_bans.count; i++)
        if (g_extension_bans.bans[i].line == line) return &g_extension_bans.bans[i];
    return NULL;
}

void vk_ban_field(uint32_t line, int field, char* out, size_t n) {
    const Ban* b = ban_at(line);
    if (b && field >= 0 && field < VK_BAN_FIELDS)
        copy_text(b->at[field], b->size[field], out, n);
    else if (n)
        snprintf(out, n, "(extension_bans.txt has no line %u with field %d)", line, field);
}

void vk_ban_scope(uint32_t line, char* out, size_t n) {
    const Ban* b = ban_at(line);
    size_t at;
    if (!b) {
        snprintf(out, n, "(extension_bans.txt has no ban at line %u)", line);
        return;
    }
    const char* g = g_extension_bans.text;
    at = (size_t)snprintf(out, n, "%s", driver_labels[b->driver]);
#define ADD(...) (at < n ? at += (size_t)snprintf(out + at, n - at, __VA_ARGS__) : 0)
    if (b->low_bound)
        ADD(" %s %.*s%s", b->low_bound == BOUND_FROM ? "from" : "after", (int)b->low_size, g + b->low_at,
            b->high_bound || b->low_bound == BOUND_AFTER ? "" : " on");
    if (b->high_bound)
        ADD(" %s %.*s",
            b->low_bound                    ? (b->high_bound == BOUND_BEFORE ? "to before" : "to")
            : b->high_bound == BOUND_BEFORE ? "before"
                                            : "up to",
            (int)b->high_size, g + b->high_at);
    for (uint32_t s = 0, first = 1; s < SYSTEMS; s++)
        if (b->systems >> s & 1) {
            ADD("%s%s", first ? " on " : ", ", system_labels[s]);
            first = 0;
        }
    for (uint32_t i = 0; i < b->ngpus; i++)
        ADD("%s%#x", i ? ", " : b->ngpus > 1 ? " on the GPUs " : " on the GPU ", b->gpus[i]);
#undef ADD
}

void vk_ban_why(uint32_t line, char* out, size_t n) {
    const Ban* b = ban_at(line);
    char scope[256];
    vk_ban_scope(line, scope, sizeof scope);
    if (b)
        snprintf(out, n, "%s %.*s (%.*s; extension_bans.txt:%u)", scope, (int)b->size[VK_BAN_BREAKS],
                 g_extension_bans.text + b->at[VK_BAN_BREAKS], (int)b->size[VK_BAN_SOURCE],
                 g_extension_bans.text + b->at[VK_BAN_SOURCE], line);
    else
        snprintf(out, n, "%s", scope);
}

/* the device as the log names it: "NVIDIA GeForce RTX 3060 (NVIDIA, 575.64.05, driver version 575.64.05)" */
static void device_text(char* out, size_t n) {
    char version[32];
    vk_driver_version_text(g_extension_bans.driver_id, g_extension_bans.driver_version, version, sizeof version);
    snprintf(out, n, "%s (%s, %s, driver version %s)", g_extension_bans.device, g_extension_bans.driver_name,
             g_extension_bans.driver_info, version);
}

enum { SAY_OFF, SAY_WARN, SAY_UNCLAIMED };
static void say_ban(const Ban* b, int how, VkBanSay say) {
    char line[2048], device[900], why[1100], key[160];
    copy_text(b->at[VK_BAN_KEY], b->size[VK_BAN_KEY], key, sizeof key);
    device_text(device, sizeof device);
    vk_ban_why(b->line, why, sizeof why);
    if (how == SAY_OFF)
        snprintf(line, sizeof line, "vulkan: %s is off on %s: %s", key, device, why);
    else if (how == SAY_WARN)
        snprintf(line, sizeof line, "vulkan: a known problem the extension bans do not turn off: %s on %s: %s", key,
                 device, why);
    else
        snprintf(line, sizeof line,
                 "vulkan: the extension bans turn %s off on %s, which this renderer does not ask "
                 "for: %s",
                 key, device, why);
    say(line, how == SAY_WARN);
}

static int key_is(const Ban* b, const char* key) { return field_is(b, VK_BAN_KEY, key); }
/* an off line that holds and that neither vulkan_ban_ignore nor its extension's setting goes over: what turns its key
   off */
static int bans(const Ban* b) { return b->off && !b->ignored && !b->forced && holds(b); }

/* the edit distance from a's size bytes to b's: insertions, deletions and substitutions, for the nearest keys */
static uint32_t distance(const char* a, uint32_t na, const char* b, uint32_t nb) {
    uint32_t row[161];
    if (na > 160) na = 160;
    if (nb > 160) nb = 160;
    for (uint32_t j = 0; j <= nb; j++) row[j] = j;
    for (uint32_t i = 1; i <= na; i++) {
        uint32_t diagonal = row[0];
        row[0] = i;
        for (uint32_t j = 1; j <= nb; j++) {
            uint32_t up = row[j], best = diagonal + (a[i - 1] != b[j - 1]);
            if (up + 1 < best) best = up + 1;
            if (row[j - 1] + 1 < best) best = row[j - 1] + 1;
            diagonal = up;
            row[j] = best;
        }
    }
    return row[nb];
}

/* the first line of each key, in the table's order: 1 for b, 0 for a later line of a key seen before */
static int first_of_key(uint32_t i) {
    const Ban* b = &g_extension_bans.bans[i];
    for (uint32_t k = 0; k < i; k++)
        if (b->size[VK_BAN_KEY] == g_extension_bans.bans[k].size[VK_BAN_KEY] &&
            !memcmp(g_extension_bans.text + b->at[VK_BAN_KEY],
                    g_extension_bans.text + g_extension_bans.bans[k].at[VK_BAN_KEY], b->size[VK_BAN_KEY]))
            return 0;
    return 1;
}

int vk_bans_ignore(const char* list, char* err, size_t n) {
    uint32_t size = (uint32_t)strlen(list), at = 0, start, item;
    if (size >= sizeof g_extension_bans.ignore) {
        snprintf(err, n, "vulkan_ban_ignore is %u bytes, past the %zu it holds", size,
                 sizeof g_extension_bans.ignore - 1);
        return 0;
    }
    memcpy(g_extension_bans.ignore, list, size + 1);
    for (uint32_t i = 0; i < g_extension_bans.count; i++) g_extension_bans.bans[i].ignored = 0;
    while (next_item(list, size, &at, &start, &item)) {
        int found = 0;
        for (uint32_t i = 0; i < g_extension_bans.count; i++) {
            Ban* b = &g_extension_bans.bans[i];
            if (b->size[VK_BAN_KEY] == item && !memcmp(g_extension_bans.text + b->at[VK_BAN_KEY], list + start, item))
                b->ignored = 1, found = 1;
        }
        if (found) continue;
        /* the three keys nearest to it, the table's first of equal ones */
        uint32_t near[3] = {0}, cost[3] = {~0u, ~0u, ~0u};
        for (uint32_t i = 0; i < g_extension_bans.count; i++) {
            const Ban* b = &g_extension_bans.bans[i];
            if (!first_of_key(i)) continue;
            uint32_t d = distance(list + start, item, g_extension_bans.text + b->at[VK_BAN_KEY], b->size[VK_BAN_KEY]);
            for (int k = 0; k < 3; k++)
                if (d < cost[k]) {
                    for (int m = 2; m > k; m--) cost[m] = cost[m - 1], near[m] = near[m - 1];
                    cost[k] = d, near[k] = i;
                    break;
                }
        }
        size_t w = (size_t)snprintf(err, n, "vulkan_ban_ignore=%s: %.*s is no key of the extension bans%s", list,
                                    (int)item, list + start, g_extension_bans.count ? "; the nearest it has:" : "");
        for (int k = 0; k < 3 && cost[k] != ~0u && w < n; k++) {
            const Ban* b = &g_extension_bans.bans[near[k]];
            w += (size_t)snprintf(err + w, n - w, "%s %.*s", k ? "," : "", (int)b->size[VK_BAN_KEY],
                                  g_extension_bans.text + b->at[VK_BAN_KEY]);
        }
        return 0;
    }
    return 1;
}

void vk_bans_ignored(VkBanSay say) {
    char line[2048], device[900], why[1100], key[160];
    device_text(device, sizeof device);
    for (uint32_t i = 0; i < g_extension_bans.count; i++) {
        const Ban* b = &g_extension_bans.bans[i];
        if (!b->ignored || !first_of_key(i)) continue;
        uint32_t over = 0; /* the first off line on its key that holds, which the setting goes over */
        for (uint32_t k = i; k < g_extension_bans.count && !over; k++) {
            const Ban* o = &g_extension_bans.bans[k];
            if (o->off && holds(o) && o->size[VK_BAN_KEY] == b->size[VK_BAN_KEY] &&
                !memcmp(g_extension_bans.text + o->at[VK_BAN_KEY], g_extension_bans.text + b->at[VK_BAN_KEY],
                        b->size[VK_BAN_KEY]))
                over = o->line;
        }
        copy_text(b->at[VK_BAN_KEY], b->size[VK_BAN_KEY], key, sizeof key);
        if (over) {
            vk_ban_why(over, why, sizeof why);
            snprintf(line, sizeof line, "vulkan: %s is not banned on %s: vulkan_ban_ignore=%s goes over the ban: %s",
                     key, device, g_extension_bans.ignore, why);
        } else
            snprintf(line, sizeof line,
                     "vulkan: %s is not banned on %s, with or without vulkan_ban_ignore=%s: no off line on it holds "
                     "there",
                     key, device, g_extension_bans.ignore);
        say(line, over != 0);
    }
}

uint32_t vk_ban_find(const char* key) {
    uint32_t first = 0;
    for (uint32_t i = 0; i < g_extension_bans.count; i++) {
        Ban* b = &g_extension_bans.bans[i];
        if (!key_is(b, key) || !holds(b)) continue;
        b->claimed = 1;
        if (bans(b) && !first) first = b->line;
    }
    return first;
}

uint32_t vk_ban(const char* key, int have, VkBanSay say) {
    uint32_t line = vk_ban_find(key);
    if (!line || !have) return 0;
    say_ban(ban_at(line), SAY_OFF, say);
    return line;
}

uint32_t vk_bans_filter(SnxVkExtensions* e, VkBanSay say) {
    uint32_t removed = 0;
    for (uint32_t i = 0; i < g_extension_bans.count; i++) {
        Ban* b = &g_extension_bans.bans[i];
        if (!holds(b)) continue;
        if (!b->extension) { /* a feature: its warning now, an off line when the renderer asks */
            if (!b->off) say_ban(b, SAY_WARN, say), b->claimed = 1;
            continue;
        }
        if (b->off && (b->ignored || b->forced)) { /* gone over: vk_bans_ignored or vk_extension_settings_say says so */
            b->claimed = 1;
            continue;
        }
        uint32_t at = 0;
        while (at < e->count && !key_is(b, e->list[at].extensionName)) at++;
        int has = at < e->count, owned = !field_is(b, VK_BAN_OVERRIDE, "-");
        b->claimed = !(has && owned); /* the setting's own code reads an overridable one: vk_ban_find */
        if (!has || owned) continue;
        if (!b->off) {
            say_ban(b, SAY_WARN, say);
            continue;
        }
        memmove(&e->list[at], &e->list[at + 1], (e->count - at - 1) * sizeof *e->list);
        e->count--;
        removed++;
        say_ban(b, SAY_OFF, say);
    }
    return removed;
}

void vk_bans_unclaimed(VkBanSay say) {
    for (uint32_t i = 0; i < g_extension_bans.count; i++) {
        const Ban* b = &g_extension_bans.bans[i];
        if (!b->claimed && bans(b)) say_ban(b, SAY_UNCLAIMED, say);
    }
}

uint32_t vk_bans_holding(uint32_t line) {
    for (uint32_t i = 0; i < g_extension_bans.count; i++)
        if (g_extension_bans.bans[i].line > line && holds(&g_extension_bans.bans[i]))
            return g_extension_bans.bans[i].line;
    return 0;
}

/* ---- the extension settings ---- */

/* the setting's key: the extension's name in lowercase (settingsgen's) */
static void setting_key(const char* name, char* out, size_t n) {
    size_t i = 0;
    for (; name[i] && i + 1 < n; i++)
        out[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] - 'A' + 'a' : name[i]);
    if (n) out[i] = 0;
}

static int list_has(const SnxVkExtensions* e, const char* name) {
    for (uint32_t i = 0; i < e->count; i++)
        if (!strcmp(e->list[i].extensionName, name)) return 1;
    return 0;
}

int vk_extension_settings(SnxVkExtensions* e, const VkExtensionSetting* s, uint32_t count, const VkExtensionNeed* need,
                          uint32_t needs, char* err, size_t n) {
    char key[160];
    g_extension_settings.count = 0;
    for (uint32_t i = 0; i < g_extension_bans.count; i++) g_extension_bans.bans[i].forced = 0;
    if (count > EXTENSION_SETTINGS) {
        snprintf(err, n, "%u extension settings, past the %u the renderer keeps", count, EXTENSION_SETTINGS);
        return 0;
    }
    for (uint32_t k = 0; k < count; k++) {
        int value = !strcmp(s[k].value, "auto") ? SETTING_AUTO
                    : !strcmp(s[k].value, "on") ? SETTING_ON
                    : !strcmp(s[k].value, "off") ? SETTING_OFF
                                                 : -1;
        setting_key(s[k].name, key, sizeof key);
        if (value < 0) {
            snprintf(err, n, "%s=%s is not auto, on or off", key, s[k].value);
            return 0;
        }
        g_extension_settings.name[k] = s[k].name;
        g_extension_settings.value[k] = (unsigned char)value;
        g_extension_settings.had[k] = (unsigned char)list_has(e, s[k].name);
        g_extension_settings.core[k] = (unsigned char)(s[k].core > 0 && s[k].core < 256 ? s[k].core : 0);
        if (value == SETTING_ON)
            for (uint32_t i = 0; i < g_extension_bans.count; i++)
                if (key_is(&g_extension_bans.bans[i], s[k].name)) g_extension_bans.bans[i].forced = 1;
        if (value != SETTING_OFF || !g_extension_settings.had[k]) continue;
        for (uint32_t j = 0; j < needs; j++)
            if (!strcmp(need[j].name, s[k].name)) {
                snprintf(err, n, "%s=off turns off %s, which ScarletNX can't run without on this device: %s", key,
                         s[k].name, need[j].why);
                return 0;
            }
        uint32_t at = 0;
        while (strcmp(e->list[at].extensionName, s[k].name)) at++;
        memmove(&e->list[at], &e->list[at + 1], (e->count - at - 1) * sizeof *e->list);
        e->count--;
    }
    g_extension_settings.count = count;
    return 1;
}

/* the first off line on key that holds for the device, whatever goes over it: 0 for none */
static const Ban* held_off(const char* key) {
    for (uint32_t i = 0; i < g_extension_bans.count; i++) {
        const Ban* b = &g_extension_bans.bans[i];
        if (b->off && holds(b) && key_is(b, key)) return b;
    }
    return NULL;
}

void vk_extension_settings_say(const SnxVkExtensions* e, uint32_t api, VkBanSay say) {
    for (uint32_t k = 0; k < g_extension_settings.count; k++) {
        const char* name = g_extension_settings.name[k];
        int value = g_extension_settings.value[k], had = g_extension_settings.had[k], in = list_has(e, name);
        int core = g_extension_settings.core[k], warn = 0;
        const Ban* ban = held_off(name);
        char key[160], why[1100] = "", line[1600], over[200] = "", core_text[120] = "";
        setting_key(name, key, sizeof key);
        if (ban) vk_ban_why(ban->line, why, sizeof why);
        /* core in the version the renderer runs the device at: it has the feature as core whatever the setting */
        if (core && api >= ((uint32_t)(core / 10) << 22 | (uint32_t)(core % 10) << 12))
            snprintf(core_text, sizeof core_text, "; core in Vulkan %d.%d, which the renderer runs this device at",
                     core / 10, core % 10);
        if (value == SETTING_OFF)
            snprintf(line, sizeof line, "vulkan: %s: off (%s=off%s%s)", name, key,
                     had ? "" : ", and the driver has it not", core_text);
        else if (!had && value == SETTING_ON)
            snprintf(line, sizeof line, "vulkan: %s: off (the driver has it not, which %s=on can't change)", name, key);
        else if (!had)
            snprintf(line, sizeof line, "vulkan: %s: off (the driver has it not)", name);
        else if (value == SETTING_ON) {
            warn = ban != NULL;
            if (ban)
                snprintf(line, sizeof line, "vulkan: %s: on (%s=on goes over the ban: %s%s)", name, key, why,
                         core_text);
            else
                snprintf(line, sizeof line, "vulkan: %s: on (%s=on, and no ban holds%s)", name, key, core_text);
        } else if (!in)
            snprintf(line, sizeof line, "vulkan: %s: off (auto, as the ban holds: %s%s)", name, why, core_text);
        else if (!ban)
            snprintf(line, sizeof line, "vulkan: %s: on (auto: the driver has it, and no ban holds%s)", name,
                     core_text);
        else if (ban->ignored)
            snprintf(line, sizeof line, "vulkan: %s: on (auto, with vulkan_ban_ignore=%s going over the ban: %s%s)",
                     name, g_extension_bans.ignore, why, core_text);
        else {
            /* a ban another setting applies, as its override says (vertex_input_dynamic_state=on): the extension
               stays in the list for that setting's code to decide */
            vk_ban_field(ban->line, VK_BAN_OVERRIDE, over, sizeof over);
            char* eq = strchr(over, '=');
            if (eq) *eq = 0;
            snprintf(line, sizeof line, "vulkan: %s: on (auto, with a ban that %s applies: %s%s)", name, over, why,
                     core_text);
        }
        say(line, warn);
    }
}
