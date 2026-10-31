/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gui/gpu_driver_fetch.c: a repository's releases from GitHub's list of them, its latest marked as GitHub marks it,
   titled by name or by tag and ordered as Eden orders them; the assets each has, those with no link to download left
   out, a GPU maker's own marked; GitHub's own error said; and Eden's recommendation by Adreno model */
#include "gui/gpu_driver_fetch.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char releases_json[] =
    "[{\"tag_name\": \"v26.1.0-rc1\", \"name\": \"Turnip 26.1 rc1\", \"prerelease\": true, \"body\": \"Testing\","
    "  \"published_at\": \"2026-10-02T10:00:00Z\","
    "  \"assets\": [{\"name\": \"Turnip_v26.1.0_R1.zip\", \"size\": 3478359,"
    "    \"browser_download_url\": \"https://github.com/K11MCH1/AdrenoToolsDrivers/releases/download/v26.1.0-rc1/"
    "Turnip_v26.1.0_R1.zip\"}]},"
    " {\"tag_name\": \"v840\", \"name\": \"Qualcomm 840\", \"prerelease\": false, \"body\": \"From a device\","
    "  \"published_at\": \"2026-10-05T10:00:00Z\","
    "  \"assets\": [{\"name\": \"Qualcomm_840_adpkg.zip\", \"size\": 17385120,"
    "    \"browser_download_url\": \"https://github.com/K11MCH1/AdrenoToolsDrivers/releases/download/v840/"
    "Qualcomm_840_adpkg.zip\"},"
    "   {\"name\": \"notes.txt\", \"size\": 10, \"browser_download_url\": \"http://example.invalid/notes.txt\"}]},"
    " {\"tag_name\": \"v26.0.0-rc08\", \"name\": \"\", \"prerelease\": false, \"body\": \"Older\","
    "  \"published_at\": \"2026-01-08T01:03:08Z\", \"assets\": []}]";

int main(void) {
    GuiDriverReleases r;
    char why[300];
    const GuiDriverRepo* kimchi = &gui_driver_repos[2];
    const GuiDriverRepo* purple = &gui_driver_repos[0];
    /* Eden's five, KIMCHI's titled by tag and newest first */
    assert(!strcmp(kimchi->path, "K11MCH1/AdrenoToolsDrivers") && kimchi->tag_titles && kimchi->by_time);
    assert(!strcmp(gui_driver_repos[1].path, "crueter/GameHub-8Elite-Drivers"));
    assert(!strcmp(gui_driver_repos[4].path, "whitebelyash/freedreno_turnip-CI") && gui_driver_repos[4].by_time);
    assert(gui_driver_releases_parse(releases_json, strlen(releases_json), kimchi, &r, why, sizeof why));
    assert(r.count == 3 && r.assets_count == 2); /* the http:// link is not one to download */
    /* newest first: the 840 (the latest, the first no prerelease in GitHub's order), the rc, the older */
    assert(!strcmp(r.releases[0].title, "v840") && r.releases[0].latest && !r.releases[0].prerelease);
    assert(!strcmp(r.releases[1].title, "v26.1.0-rc1") && r.releases[1].prerelease && !r.releases[1].latest);
    assert(!strcmp(r.releases[2].title, "v26.0.0-rc08") && !r.releases[2].latest && !r.releases[2].count);
    assert(r.releases[0].count == 1 && gui_driver_assets(&r)[r.releases[0].first].proprietary &&
           gui_driver_assets(&r)[r.releases[0].first].size == 17385120ull);
    assert(r.releases[1].count == 1 && !gui_driver_assets(&r)[r.releases[1].first].proprietary &&
           !strcmp(gui_driver_assets(&r)[r.releases[1].first].name, "Turnip_v26.1.0_R1.zip") &&
           !strncmp(gui_driver_assets(&r)[r.releases[1].first].url, "https://github.com/K11MCH1/", 27));
    gui_driver_releases_free(&r);
    /* another repository's: titled by name (its tag when it has none), in GitHub's order */
    assert(gui_driver_releases_parse(releases_json, strlen(releases_json), purple, &r, why, sizeof why));
    assert(!strcmp(r.releases[0].title, "Turnip 26.1 rc1") && !strcmp(r.releases[1].title, "Qualcomm 840") &&
           !strcmp(r.releases[2].title, "v26.0.0-rc08"));
    gui_driver_releases_free(&r);
    /* GitHub's own error, said; what is no list of releases, said by its size */
    assert(!gui_driver_releases_parse("{\"message\": \"API rate limit exceeded for 1.2.3.4.\"}", 51, purple, &r, why,
                                      sizeof why) &&
           !strcmp(why, "GitHub says of MrPurple666/purple-turnip: API rate limit exceeded for 1.2.3.4.") &&
           !r.releases);
    assert(!gui_driver_releases_parse("<html>", 6, purple, &r, why, sizeof why) &&
           !strcmp(why, "MrPurple666/purple-turnip's releases are not GitHub's list of them (6 bytes)"));
    /* the GPU maker's own: every GameHub asset, and any named Qualcomm */
    assert(gui_driver_proprietary("crueter/GameHub-8Elite-Drivers", "adreno-driver-842.8.zip"));
    assert(gui_driver_proprietary("K11MCH1/AdrenoToolsDrivers", "qualcomm_837_adpkg.zip"));
    assert(!gui_driver_proprietary("whitebelyash/freedreno_turnip-CI", "mainline-turnip-V32.zip"));
    /* the model and Eden's recommendation for it */
    assert(gui_adreno_model("Adreno (TM) 830") == 830 && gui_adreno_model("Adreno (TM) A32") == 32);
    assert(gui_adreno_model("Adreno (TM) 610") == 610 && !gui_adreno_model("Mali-G78") &&
           !gui_adreno_model("Adreno (TM) 6xx") && !gui_adreno_model("Turnip Adreno (TM) 830"));
    assert(!strcmp(gui_driver_recommended(830), "GameHub Adreno 8xx") &&
           !strcmp(gui_driver_recommended(610), "Mr. Purple EOL-24.3.4") &&
           !strcmp(gui_driver_recommended(740), "Mr. Purple T23") &&
           !strcmp(gui_driver_recommended(32), "KIMCHI Latest") && !strcmp(gui_driver_recommended(0), "Unsupported") &&
           !strcmp(gui_driver_recommended(950), "Unsupported"));
    puts("gui_gpu_driver_fetch: ok");
    return 0;
}
