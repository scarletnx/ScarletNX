/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gui/gpu_driver_fetch.h. Eden's repositories, a release's fields and which one is the latest, the newest-first order
   and the recommendation by Adreno model are Eden's (DriverFetcherFragment.kt: repoList, Release.fromJson, driverMap,
   parseAdrenoModel); NX-Mali, the recommendation by Mali model and the proprietary mark are ScarletNX's */
#include "gui/gpu_driver_fetch.h"
#include "common/utilities/ascii.h"
#include "gui/i18n.h"
#include <yyjson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const GuiDriverRepo gui_driver_repos[GUI_DRIVER_REPOS] = {
    {"Mr. Purple Turnip", "MrPurple666/purple-turnip", 0, 0, GUI_DRIVER_ADRENO},
    {"GameHub Adreno 8xx", "crueter/GameHub-8Elite-Drivers", 0, 0, GUI_DRIVER_ADRENO},
    {"KIMCHI Turnip", "K11MCH1/AdrenoToolsDrivers", 1, 1, GUI_DRIVER_ADRENO},
    {"Weab-Chan Freedreno", "Weab-chan/freedreno_turnip-CI", 0, 0, GUI_DRIVER_ADRENO},
    {"Whitebelyash Turnip", "whitebelyash/freedreno_turnip-CI", 0, 1, GUI_DRIVER_ADRENO},
    {"NX-Mali", "Slopinator99/NX-Mali", 0, 0, GUI_DRIVER_MALI}};

/* whether text has part, in any case */
static int has_nocase(const char* text, const char* part) {
    size_t n = strlen(part);
    for (; *text; ++text) {
        size_t i;
        for (i = 0; i < n && text[i] && snx_to_lower((unsigned char)text[i]) == snx_to_lower((unsigned char)part[i]);
             ++i) {}
        if (i == n) return 1;
    }
    return 0;
}
int gui_driver_proprietary(const char* repo_path, const char* asset_name) {
    return !strcmp(repo_path, "crueter/GameHub-8Elite-Drivers") || has_nocase(asset_name, "qualcomm");
}

/* a string field's text into out, cut to its room; "" when it has none */
static void text_of(yyjson_val* o, const char* key, char* out, size_t size) {
    const char* v = yyjson_get_str(yyjson_obj_get(o, key));
    snprintf(out, size, "%s", v ? v : "");
}
static int newest_first(const void* a, const void* b) {
    return strcmp(((const GuiDriverRelease*)b)->published, ((const GuiDriverRelease*)a)->published);
}
int gui_driver_releases_parse(const char* json, size_t n, const GuiDriverRepo* repo, GuiDriverReleases* out, char* why,
                              size_t size) {
    yyjson_doc* doc = yyjson_read(json, n, 0);
    yyjson_val *root = yyjson_doc_get_root(doc), *item;
    size_t releases = 0, assets = 0, r = 0, a = 0, i, count;
    int latest_found = 0;
    memset(out, 0, sizeof *out);
    if (!yyjson_is_arr(root)) {
        const char* message = yyjson_get_str(yyjson_obj_get(root, "message"));
        if (message) /* GitHub's own error: its rate limit, a repository gone */
            snprintf(why, size, snx_tr("GitHub says of %s: %s"), repo->path, message);
        else
            snprintf(why, size, snx_tr("%s's releases are not GitHub's list of them (%zu bytes)"), repo->path, n);
        yyjson_doc_free(doc);
        return 0;
    }
    yyjson_arr_foreach(root, i, count, item) {
        if (!yyjson_is_obj(item)) continue;
        ++releases;
        assets += yyjson_arr_size(yyjson_obj_get(item, "assets"));
    }
    /* one block: the releases, then the assets */
    if (releases > ((size_t)-1 - 1) / 2 / sizeof(GuiDriverRelease) ||
        assets > ((size_t)-1 - 1) / 2 / sizeof(GuiDriverAsset))
        out->releases = NULL;
    else
        out->releases =
            (GuiDriverRelease*)calloc(1, releases * sizeof(GuiDriverRelease) + assets * sizeof(GuiDriverAsset) + 1);
    if (!out->releases) {
        snprintf(why, size, snx_tr("no memory for %s's %zu releases and %zu assets"), repo->path, releases, assets);
        yyjson_doc_free(doc);
        return 0;
    }
    out->count = releases;
    yyjson_arr_foreach(root, i, count, item) {
        yyjson_val* asset;
        size_t k, assets_here;
        GuiDriverRelease* rel;
        if (!yyjson_is_obj(item)) continue;
        rel = &out->releases[r++];
        text_of(item, "tag_name", rel->tag, sizeof rel->tag);
        text_of(item, repo->tag_titles ? "tag_name" : "name", rel->title, sizeof rel->title);
        if (!rel->title[0]) snprintf(rel->title, sizeof rel->title, "%s", rel->tag);
        text_of(item, "body", rel->body, sizeof rel->body);
        text_of(item, "published_at", rel->published, sizeof rel->published);
        rel->prerelease = yyjson_is_true(yyjson_obj_get(item, "prerelease"));
        rel->latest = !rel->prerelease && !latest_found;
        latest_found |= rel->latest;
        rel->first = a;
        yyjson_arr_foreach(yyjson_obj_get(item, "assets"), k, assets_here, asset) {
            GuiDriverAsset* d = &gui_driver_assets(out)[a];
            yyjson_val* bytes = yyjson_obj_get(asset, "size");
            text_of(asset, "name", d->name, sizeof d->name);
            text_of(asset, "browser_download_url", d->url, sizeof d->url);
            if (!d->name[0] || strncmp(d->url, "https://", 8)) continue; /* none to download: not listed */
            d->size = yyjson_is_num(bytes) && yyjson_get_num(bytes) > 0 ? (unsigned long long)yyjson_get_num(bytes) : 0;
            d->proprietary = gui_driver_proprietary(repo->path, d->name);
            ++a;
        }
        rel->count = a - rel->first;
    }
    yyjson_doc_free(doc);
    out->assets_count = a;
    if (repo->by_time) { /* the assets stay where they are: each release keeps its own first and count */
        qsort(out->releases, out->count, sizeof *out->releases, newest_first);
    }
    return 1;
}
void gui_driver_releases_free(GuiDriverReleases* r) {
    free(r->releases);
    memset(r, 0, sizeof *r);
}

int gui_adreno_model(const char* gpu_name) {
    const char* p;
    int model = 0;
    /* the format Eden reads: Adreno (TM) <model>, an A before the model on Adreno's A series */
    if (strncmp(gpu_name, "Adreno ", 7)) return 0;
    p = strchr(gpu_name + 7, ' ');
    if (!p) return 0;
    ++p;
    if (*p == 'A') ++p;
    if (!snx_is_digit((unsigned char)*p)) return 0;
    for (; snx_is_digit((unsigned char)*p) && model < 100000; ++p) model = model * 10 + (*p - '0');
    return *p && *p != ' ' ? 0 : model;
}
int gui_mali_model(const char* gpu_name) {
    const char* p = gpu_name + 6;
    int model = 0;
    /* Mali-G<model>, then the end, " MC<cores>", "-Immortalis" or the AE of an automotive part */
    if (strncmp(gpu_name, "Mali-G", 6) || !snx_is_digit((unsigned char)*p)) return 0;
    for (; snx_is_digit((unsigned char)*p) && model < 100000; ++p) model = model * 10 + (*p - '0');
    return !*p || *p == ' ' || *p == '-' || !strncmp(p, "AE", 2) ? model : 0;
}
/* the picks by model, each as the repository its release is in and a part of that release's tag, NULL for the
   repository's latest. Adreno's are Eden's (DriverFetcherFragment.kt); 605 to 613 have no multiview in hardware:
   EOL-24.3.4 then says Vulkan 1.0, short of what ScarletNX needs, and T30 emulates it and says 1.4. The Malis are
   those NX-Mali says Vulkan 1.3 on (its api_version_for_arch: arch 10 and up, the command-stream ones): the G610 and
   the G710, the G615 and the G715. On its job-manager parts (the G52, G57, G68, G77 and G78) it says 1.1, which
   device_score refuses, so none is recommended there */
static const struct {
    int gpu, lo, hi;
    const char *driver, *repo, *tag;
} g_recommended[] = {{GUI_DRIVER_ADRENO, 10, 99, "KIMCHI Latest", "K11MCH1/AdrenoToolsDrivers", NULL},
                     {GUI_DRIVER_ADRENO, 600, 613, "Mr. Purple T30", "MrPurple666/purple-turnip", "T30"},
                     {GUI_DRIVER_ADRENO, 614, 639, "Mr. Purple EOL-24.3.4", "MrPurple666/purple-turnip", "EOL-24.3.4"},
                     {GUI_DRIVER_ADRENO, 640, 699, "Mr. Purple T19", "MrPurple666/purple-turnip", "T19"},
                     {GUI_DRIVER_ADRENO, 700, 710, "KIMCHI 25.2.0_r5", "K11MCH1/AdrenoToolsDrivers", "v25.2.0-rc.05"},
                     {GUI_DRIVER_ADRENO, 711, 799, "Mr. Purple T23", "MrPurple666/purple-turnip", "T23"},
                     {GUI_DRIVER_ADRENO, 800, 899, "GameHub Adreno 8xx", "crueter/GameHub-8Elite-Drivers", NULL},
                     {GUI_DRIVER_MALI, 610, 610, "NX-Mali", "Slopinator99/NX-Mali", NULL},
                     {GUI_DRIVER_MALI, 615, 615, "NX-Mali", "Slopinator99/NX-Mali", NULL},
                     {GUI_DRIVER_MALI, 710, 710, "NX-Mali", "Slopinator99/NX-Mali", NULL},
                     {GUI_DRIVER_MALI, 715, 715, "NX-Mali", "Slopinator99/NX-Mali", NULL}};
#define GUI_DRIVER_PICKS (sizeof g_recommended / sizeof g_recommended[0])
/* gpu_name's pick in g_recommended, GUI_DRIVER_PICKS for none */
static size_t recommended_of(const char* gpu_name) {
    int adreno = gui_adreno_model(gpu_name), mali = gui_mali_model(gpu_name);
    size_t i;
    for (i = 0; i < GUI_DRIVER_PICKS; ++i) {
        int model = g_recommended[i].gpu == GUI_DRIVER_ADRENO ? adreno : mali;
        if (model >= g_recommended[i].lo && model <= g_recommended[i].hi) break;
    }
    return i;
}
const char* gui_driver_recommended(const char* gpu_name) {
    size_t i = recommended_of(gpu_name);
    return i < GUI_DRIVER_PICKS ? g_recommended[i].driver : SNX_TR_NOOP("Unsupported");
}
int gui_driver_recommended_release(const char* gpu_name, int repo, const GuiDriverReleases* list) {
    size_t i = recommended_of(gpu_name), k;
    if (i == GUI_DRIVER_PICKS || repo < 0 || repo >= GUI_DRIVER_REPOS ||
        strcmp(gui_driver_repos[repo].path, g_recommended[i].repo))
        return -1;
    for (k = 0; k < list->count; ++k)
        if (g_recommended[i].tag ? has_nocase(list->releases[k].tag, g_recommended[i].tag) : list->releases[k].latest)
            return (int)k;
    return -1;
}
