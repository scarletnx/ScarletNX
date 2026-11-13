/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gui/gpu_driver_fetch.c: a repository's releases from GitHub's list of them, its latest marked as GitHub marks it,
   titled by name or by tag and ordered as Eden orders them; the assets each has, those with no link to download left
   out, a GPU maker's own marked; GitHub's own error said; NX-Mali's release, as GitHub lists it; and the
   recommendation by GPU name, Eden's by Adreno model and NX-Mali by Mali model (while GUI_DRIVER_MALI_RECOMMENDED),
   found among a repository's releases */
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
    /* Eden's five for an Adreno, KIMCHI's titled by tag and newest first; NX-Mali for a Mali */
    assert(!strcmp(kimchi->path, "K11MCH1/AdrenoToolsDrivers") && kimchi->tag_titles && kimchi->by_time);
    assert(!strcmp(gui_driver_repos[1].path, "crueter/GameHub-8Elite-Drivers"));
    assert(!strcmp(gui_driver_repos[4].path, "whitebelyash/freedreno_turnip-CI") && gui_driver_repos[4].by_time);
    for (int i = 0; i < 5; ++i) assert(gui_driver_repos[i].gpu == GUI_DRIVER_ADRENO);
    assert(!strcmp(gui_driver_repos[5].path, "Slopinator99/NX-Mali") && !strcmp(gui_driver_repos[5].name, "NX-Mali") &&
           gui_driver_repos[5].gpu == GUI_DRIVER_MALI && !gui_driver_repos[5].tag_titles &&
           !gui_driver_repos[5].by_time);
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
    /* NX-Mali's release as GitHub lists it: titled by its name, the latest, its package's zip */
    {
        static const char nx_mali_json[] =
            "[{\"tag_name\": \"beta\", \"name\": \"NX-Mali 0.0.1\", \"prerelease\": false,"
            "  \"body\": \"First release of the WIP driver for Mali devices\","
            "  \"published_at\": \"2026-11-13T15:57:50Z\","
            "  \"assets\": [{\"name\": \"NX-Mali_0.0.1.zip\", \"size\": 4404914, \"browser_download_url\":"
            "    \"https://github.com/Slopinator99/NX-Mali/releases/download/beta/NX-Mali_0.0.1.zip\"}]}]";
        assert(
            gui_driver_releases_parse(nx_mali_json, strlen(nx_mali_json), &gui_driver_repos[5], &r, why, sizeof why));
        assert(r.count == 1 && r.assets_count == 1 && !strcmp(r.releases[0].title, "NX-Mali 0.0.1") &&
               r.releases[0].latest && !gui_driver_assets(&r)[0].proprietary &&
               !strcmp(gui_driver_assets(&r)[0].name, "NX-Mali_0.0.1.zip"));
        /* recommended on a Mali it drives only with the switch on; on an Adreno or another Mali never */
        assert(gui_driver_recommended_release("Mali-G615 MC6", 5, &r) == (GUI_DRIVER_MALI_RECOMMENDED ? 0 : -1));
        assert(gui_driver_recommended_release("Mali-G710 MC10", 5, &r) == -1 &&
               gui_driver_recommended_release("Adreno (TM) 740", 5, &r) == -1);
        gui_driver_releases_free(&r);
    }
    /* the models, and the recommendation for a GPU's name: Eden's by Adreno model, NX-Mali by Mali model */
    assert(gui_adreno_model("Adreno (TM) 830") == 830 && gui_adreno_model("Adreno (TM) A32") == 32);
    assert(gui_adreno_model("Adreno (TM) 610") == 610 && !gui_adreno_model("Mali-G78") &&
           !gui_adreno_model("Adreno (TM) 6xx") && !gui_adreno_model("Turnip Adreno (TM) 830"));
    assert(gui_mali_model("Mali-G615 MC6") == 615 && gui_mali_model("Mali-G78") == 78 &&
           gui_mali_model("Mali-G78AE") == 78 && gui_mali_model("Mali-G715-Immortalis MC11") == 715 &&
           gui_mali_model("Mali-G52 r1") == 52);
    assert(!gui_mali_model("Mali-T880") && !gui_mali_model("Mali-G") && !gui_mali_model("Mali-G7x") &&
           !gui_mali_model("Adreno (TM) 830") && !gui_mali_model("Immortalis-G715"));
    assert(!strcmp(gui_driver_recommended("Adreno (TM) 830"), "GameHub Adreno 8xx") &&
           !strcmp(gui_driver_recommended("Adreno (TM) 610"), "Mr. Purple T30") &&
           !strcmp(gui_driver_recommended("Adreno (TM) 613"), "Mr. Purple T30") &&
           !strcmp(gui_driver_recommended("Adreno (TM) 618"), "Mr. Purple EOL-24.3.4") &&
           !strcmp(gui_driver_recommended("Adreno (TM) 740"), "Mr. Purple T23") &&
           !strcmp(gui_driver_recommended("Adreno (TM) A32"), "KIMCHI Latest") &&
           !strcmp(gui_driver_recommended(""), "Unsupported") &&
           !strcmp(gui_driver_recommended("Adreno (TM) 950"), "Unsupported"));
    /* the Malis NX-Mali names as its own, recommended only with the switch on; other Malis never, and an Adreno of the
       G615's number by Adreno's table */
    {
        static const char* const driven[] = {"Mali-G52",
                                             "Mali-G57 MC2",
                                             "Mali-G68 MC4",
                                             "Mali-G77 MC9",
                                             "Mali-G78AE",
                                             "Mali-G615 MC6",
                                             "Mali-G715-Immortalis MC11"};
        for (size_t i = 0; i < sizeof driven / sizeof *driven; ++i)
            assert(!strcmp(gui_driver_recommended(driven[i]), GUI_DRIVER_MALI_RECOMMENDED ? "NX-Mali" : "Unsupported"));
        assert(!strcmp(gui_driver_recommended("Mali-G710 MC10"), "Unsupported") &&
               !strcmp(gui_driver_recommended("Mali-G76 MC4"), "Unsupported") &&
               !strcmp(gui_driver_recommended("Mali-G610 MC6"), "Unsupported") &&
               !strcmp(gui_driver_recommended("Adreno (TM) 615"), "Mr. Purple EOL-24.3.4"));
    }
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
        assert(gui_driver_recommended_release("Adreno (TM) 740", 0, &r) == 1 &&
               gui_driver_recommended_release("Adreno (TM) 650", 0, &r) == 2);
        assert(gui_driver_recommended_release("Adreno (TM) 618", 0, &r) == -1); /* EOL-24.3.4, not listed */
        /* a low A6xx, which EOL-24.3.4 gives Vulkan 1.0: T30 */
        assert(
            gui_driver_recommended_release("Adreno (TM) 610", 0, &r) >= 0 &&
            !strcmp(r.releases[gui_driver_recommended_release("Adreno (TM) 610", 0, &r)].tag, "vturnip_mrpurple_T30"));
        assert(gui_driver_recommended_release("Adreno (TM) 830", 0, &r) == -1 &&
               gui_driver_recommended_release("Adreno (TM) 950", 0, &r) == -1);
        gui_driver_releases_free(&r);
        assert(gui_driver_releases_parse(kimchi_json, strlen(kimchi_json), kimchi, &r, why, sizeof why));
        assert(gui_driver_recommended_release("Adreno (TM) 705", 2, &r) >= 0 &&
               !strcmp(r.releases[gui_driver_recommended_release("Adreno (TM) 705", 2, &r)].tag, "v25.2.0-rc.05"));
        assert(gui_driver_recommended_release("Adreno (TM) A32", 2, &r) >= 0 &&
               r.releases[gui_driver_recommended_release("Adreno (TM) A32", 2, &r)].latest);
        assert(gui_driver_recommended_release("Adreno (TM) 705", 0, &r) == -1); /* named for another repository */
        gui_driver_releases_free(&r);
    }
    puts("gui_gpu_driver_fetch: ok");
    return 0;
}
