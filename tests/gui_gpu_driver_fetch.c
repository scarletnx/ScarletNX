/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gui/gpu_driver_fetch.c: a repository's releases from GitHub's list of them, its latest marked as GitHub marks it,
   titled by name or by tag and ordered as Eden orders them; the assets each has, those with no link to download left
   out, a GPU maker's own marked, radv-xclipse's that are not for Android left out; GitHub's own error said; and the
   recommendation by model, found among a repository's releases, NX-Mali's from 0.0.3 on */
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
           !strcmp(gui_driver_recommended(610), "Mr. Purple T30") &&
           !strcmp(gui_driver_recommended(613), "Mr. Purple T30") &&
           !strcmp(gui_driver_recommended(618), "Mr. Purple EOL-24.3.4") &&
           !strcmp(gui_driver_recommended(740), "Mr. Purple T23") &&
           !strcmp(gui_driver_recommended(32), "KIMCHI Latest") && !strcmp(gui_driver_recommended(0), "Unsupported") &&
           !strcmp(gui_driver_recommended(950), "Unsupported"));
    /* the recommendation among a repository's releases: by a part of its tag, else its latest; another repository's,
       none, or a release the list lacks gives -1 */
    {
        static const char purple_json[] =
            "[{\"tag_name\": \"vturnip_mrpurple_T30\", \"name\": \"T30\", \"prerelease\": false, \"assets\": []},"
            " {\"tag_name\": \"vturnip_mrpurple_T23-toasted.adpkg\", \"name\": \"T23\", \"prerelease\": false,"
            "  \"assets\": []},"
            " {\"tag_name\": \"vturnip_mrpurple-T19-toasted.adpkg\", \"name\": \"T19\", \"prerelease\": false,"
            "  \"assets\": []}]";
        static const char kimchi_json[] =
            "[{\"tag_name\": \"v26.0.0-rc05\", \"name\": \"26 r5\", \"prerelease\": false, \"assets\": []},"
            " {\"tag_name\": \"v25.2.0-rc.05\", \"name\": \"05152025\", \"prerelease\": false, \"assets\": []}]";
        assert(gui_driver_releases_parse(purple_json, strlen(purple_json), purple, &r, why, sizeof why));
        assert(gui_driver_recommended_release(740, 0, &r) == 1 && gui_driver_recommended_release(650, 0, &r) == 2);
        assert(gui_driver_recommended_release(618, 0, &r) == -1); /* EOL-24.3.4, not listed */
        /* a low A6xx, which EOL-24.3.4 gives Vulkan 1.0: T30 */
        assert(gui_driver_recommended_release(610, 0, &r) >= 0 &&
               !strcmp(r.releases[gui_driver_recommended_release(610, 0, &r)].tag, "vturnip_mrpurple_T30"));
        assert(gui_driver_recommended_release(830, 0, &r) == -1 && gui_driver_recommended_release(950, 0, &r) == -1);
        gui_driver_releases_free(&r);
        assert(gui_driver_releases_parse(kimchi_json, strlen(kimchi_json), kimchi, &r, why, sizeof why));
        assert(gui_driver_recommended_release(705, 2, &r) >= 0 &&
               !strcmp(r.releases[gui_driver_recommended_release(705, 2, &r)].tag, "v25.2.0-rc.05"));
        assert(gui_driver_recommended_release(32, 2, &r) >= 0 &&
               r.releases[gui_driver_recommended_release(32, 2, &r)].latest);
        assert(gui_driver_recommended_release(705, 0, &r) == -1); /* named for another repository */
        gui_driver_releases_free(&r);
    }
    /* radv-xclipse's release as GitHub lists it: titled by its tag, its builds for Linux and Bannerlator left out */
    {
        static const char radv_json[] =
            "[{\"tag_name\": \"v1.9.0\", \"name\": \"RADV Xclipse (Based on Mesa 27.0.0-devel)\","
            "  \"prerelease\": false, \"assets\": ["
            "   {\"name\": \"radv-xclipse-27.0.0-devel-cc87fdf.zip\", \"browser_download_url\": \"https://a\"},"
            "   {\"name\": \"radv-xclipse-glibc-27.0.0-devel-cc87fdf.zip\", \"browser_download_url\": \"https://b\"},"
            "   {\"name\": \"radv-xclipse-wayland-27.0.0-devel-cc87fdf.zip\", \"browser_download_url\": \"https://c\"}"
            "]}]";
        assert(gui_driver_releases_parse(radv_json, strlen(radv_json), &gui_driver_repos[6], &r, why, sizeof why));
        assert(r.assets_count == 1 && !strcmp(r.releases[0].title, "v1.9.0") &&
               !strcmp(gui_driver_assets(&r)[0].name, "radv-xclipse-27.0.0-devel-cc87fdf.zip"));
        gui_driver_releases_free(&r);
    }
    /* a Mali's and an Xclipse's models; NX-Mali's latest recommended on a Mali it gives 1.3 from 0.0.3 on, by the
       version in its title, and radv-xclipse's on an Xclipse */
    assert(gui_gpu_model("Mali-G615 MC6") == 10615 && gui_gpu_model("Samsung Xclipse 940") == 20940 &&
           gui_gpu_model("Adreno (TM) 615") == 615 && !strcmp(gui_driver_recommended(10078), "Unsupported"));
    {
        static const struct {
            const char* title;
            int model, repo, recommended;
        } picks[] = {{"NX-Mali 0.0.2", 10615, 5, -1}, {"NX-Mali 0.0.10", 10615, 5, 0}, {"v1.9.0", 20940, 6, 0}};
        for (size_t i = 0; i < sizeof picks / sizeof *picks; ++i) {
            char json[160];
            snprintf(json, sizeof json,
                     "[{\"tag_name\": \"%s\", \"name\": \"%s\", \"prerelease\": false, \"assets\": []}]",
                     picks[i].title, picks[i].title);
            assert(
                gui_driver_releases_parse(json, strlen(json), &gui_driver_repos[picks[i].repo], &r, why, sizeof why));
            assert(gui_driver_recommended_release(picks[i].model, picks[i].repo, &r) == picks[i].recommended);
            gui_driver_releases_free(&r);
        }
    }
    puts("gui_gpu_driver_fetch: ok");
    return 0;
}
