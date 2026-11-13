/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gui/gpu_driver_fetch.c: a repository's releases from GitHub's list of them, its latest marked as GitHub marks it,
   titled by name or by tag and ordered as Eden orders them; the assets each has, those with no link to download left
   out, a GPU maker's own marked; GitHub's own error said; NX-Mali's release and radv-xclipse's, as GitHub lists them,
   radv-xclipse's builds for Linux and for Bannerlator left out; and the recommendation by GPU name, Eden's by Adreno
   model, NX-Mali by Mali model and RADV Xclipse by Xclipse model, found among a repository's releases */
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
    /* Eden's five for an Adreno, KIMCHI's titled by tag and newest first; NX-Mali for a Mali; radv-xclipse for an
       Xclipse, titled by tag, as each of its releases has the same name */
    assert(!strcmp(kimchi->path, "K11MCH1/AdrenoToolsDrivers") && kimchi->tag_titles && kimchi->by_time);
    assert(!strcmp(gui_driver_repos[1].path, "crueter/GameHub-8Elite-Drivers"));
    assert(!strcmp(gui_driver_repos[4].path, "whitebelyash/freedreno_turnip-CI") && gui_driver_repos[4].by_time);
    for (int i = 0; i < 5; ++i) assert(gui_driver_repos[i].gpu == GUI_DRIVER_ADRENO);
    assert(!strcmp(gui_driver_repos[5].path, "Slopinator99/NX-Mali") && !strcmp(gui_driver_repos[5].name, "NX-Mali") &&
           gui_driver_repos[5].gpu == GUI_DRIVER_MALI && !gui_driver_repos[5].tag_titles &&
           !gui_driver_repos[5].by_time);
    assert(!strcmp(gui_driver_repos[6].path, "JimVulkan/radv-xclipse") &&
           !strcmp(gui_driver_repos[6].name, "RADV Xclipse") && gui_driver_repos[6].gpu == GUI_DRIVER_XCLIPSE &&
           gui_driver_repos[6].tag_titles && !gui_driver_repos[6].by_time);
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
    /* a package for this app: every asset but radv-xclipse's builds for Linux (glibc) and for Bannerlator (Wayland) */
    assert(gui_driver_android_package("JimVulkan/radv-xclipse", "radv-xclipse-27.0.0-devel-cc87fdf.zip") &&
           gui_driver_android_package("JimVulkan/radv-xclipse", "radv-xclipse-26.2.1-b14b877.zip"));
    assert(!gui_driver_android_package("JimVulkan/radv-xclipse", "radv-xclipse-glibc-27.0.0-devel-cc87fdf.zip") &&
           !gui_driver_android_package("JimVulkan/radv-xclipse", "radv-xclipse-wayland-27.0.0-devel-cc87fdf.zip") &&
           !gui_driver_android_package("JimVulkan/radv-xclipse", "radv-xclipse-") &&
           !gui_driver_android_package("JimVulkan/radv-xclipse", "NOTICE.txt"));
    assert(gui_driver_android_package("Slopinator99/NX-Mali", "NX-Mali_0.0.1.zip") &&
           gui_driver_android_package("K11MCH1/AdrenoToolsDrivers", "radv-xclipse-glibc-27.0.0-devel-cc87fdf.zip"));
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
        /* its latest recommended on a Mali it gives 1.3; on one it gives 1.1, and on an Adreno, never */
        assert(gui_driver_recommended_release("Mali-G615 MC6", 5, &r) == 0 &&
               gui_driver_recommended_release("Mali-G710 MC10", 5, &r) == 0);
        assert(gui_driver_recommended_release("Mali-G78 MC14", 5, &r) == -1 &&
               gui_driver_recommended_release("Adreno (TM) 740", 5, &r) == -1);
        gui_driver_releases_free(&r);
    }
    /* radv-xclipse's three newest releases as GitHub lists them (each body cut to its first line): titled by tag, the
       latest the first that is no prerelease, each with its Android package alone */
    {
        static const char radv_json[] =
            "[{\"tag_name\": \"v1.9.0\", \"name\": \"RADV Xclipse (Based on Mesa 27.0.0-devel)\","
            "  \"prerelease\": false, \"body\": \"RADV for Samsung Xclipse GPUs, build `cc87fdf`.\","
            "  \"published_at\": \"2026-11-11T22:39:02Z\","
            "  \"assets\": [{\"name\": \"radv-xclipse-27.0.0-devel-cc87fdf.zip\", \"size\": 4396585,"
            "    \"browser_download_url\": \"https://github.com/JimVulkan/radv-xclipse/releases/download/v1.9.0/"
            "radv-xclipse-27.0.0-devel-cc87fdf.zip\"},"
            "   {\"name\": \"radv-xclipse-glibc-27.0.0-devel-cc87fdf.zip\", \"size\": 4673254,"
            "    \"browser_download_url\": \"https://github.com/JimVulkan/radv-xclipse/releases/download/v1.9.0/"
            "radv-xclipse-glibc-27.0.0-devel-cc87fdf.zip\"},"
            "   {\"name\": \"radv-xclipse-wayland-27.0.0-devel-cc87fdf.zip\", \"size\": 4474943,"
            "    \"browser_download_url\": \"https://github.com/JimVulkan/radv-xclipse/releases/download/v1.9.0/"
            "radv-xclipse-wayland-27.0.0-devel-cc87fdf.zip\"}]},"
            " {\"tag_name\": \"v1.9.0-RC1\", \"name\": \"RADV Xclipse (Based on Mesa 27.0.0-devel)\","
            "  \"prerelease\": true, \"body\": \"RADV for Samsung Xclipse GPUs, release candidate 1.\","
            "  \"published_at\": \"2026-10-28T20:29:14Z\","
            "  \"assets\": [{\"name\": \"radv-xclipse-27.0.0-devel-fe9b894.zip\", \"size\": 4385836,"
            "    \"browser_download_url\": \"https://github.com/JimVulkan/radv-xclipse/releases/download/v1.9.0-RC1/"
            "radv-xclipse-27.0.0-devel-fe9b894.zip\"},"
            "   {\"name\": \"radv-xclipse-wayland-27.0.0-devel-fe9b894.zip\", \"size\": 4463103,"
            "    \"browser_download_url\": \"https://github.com/JimVulkan/radv-xclipse/releases/download/v1.9.0-RC1/"
            "radv-xclipse-wayland-27.0.0-devel-fe9b894.zip\"}]},"
            " {\"tag_name\": \"v1.8.0\", \"name\": \"RADV Xclipse (Based on Mesa 27.0.0-devel)\","
            "  \"prerelease\": false, \"body\": \"RADV for Samsung Xclipse GPUs.\","
            "  \"published_at\": \"2026-10-19T22:27:02Z\","
            "  \"assets\": [{\"name\": \"radv-xclipse-27.0.0-devel-269238e.zip\", \"size\": 4383848,"
            "    \"browser_download_url\": \"https://github.com/JimVulkan/radv-xclipse/releases/download/v1.8.0/"
            "radv-xclipse-27.0.0-devel-269238e.zip\"},"
            "   {\"name\": \"radv-xclipse-wayland-27.0.0-devel-269238e.zip\", \"size\": 4460784,"
            "    \"browser_download_url\": \"https://github.com/JimVulkan/radv-xclipse/releases/download/v1.8.0/"
            "radv-xclipse-wayland-27.0.0-devel-269238e.zip\"}]}]";
        const GuiDriverAsset* a;
        assert(gui_driver_releases_parse(radv_json, strlen(radv_json), &gui_driver_repos[6], &r, why, sizeof why));
        assert(r.count == 3 && r.assets_count == 3);
        assert(!strcmp(r.releases[0].title, "v1.9.0") && r.releases[0].latest && !r.releases[0].prerelease &&
               !strcmp(r.releases[1].title, "v1.9.0-RC1") && r.releases[1].prerelease && !r.releases[1].latest &&
               !strcmp(r.releases[2].title, "v1.8.0") && !r.releases[2].latest);
        for (size_t k = 0; k < r.count; ++k) {
            a = &gui_driver_assets(&r)[r.releases[k].first];
            assert(r.releases[k].count == 1 && !a->proprietary && !strncmp(a->name, "radv-xclipse-27.0.0-devel-", 26));
        }
        a = &gui_driver_assets(&r)[r.releases[0].first];
        assert(!strcmp(a->name, "radv-xclipse-27.0.0-devel-cc87fdf.zip") && a->size == 4396585ull &&
               !strcmp(a->url, "https://github.com/JimVulkan/radv-xclipse/releases/download/v1.9.0/"
                               "radv-xclipse-27.0.0-devel-cc87fdf.zip"));
        /* its latest recommended on an Xclipse it drives; on one it does not, a Mali, or another repository, never */
        assert(gui_driver_recommended_release("Samsung Xclipse 940", 6, &r) == 0 &&
               gui_driver_recommended_release("Samsung Xclipse 530", 6, &r) == 0);
        assert(gui_driver_recommended_release("Samsung Xclipse 930", 6, &r) == -1 &&
               gui_driver_recommended_release("Mali-G615 MC6", 6, &r) == -1 &&
               gui_driver_recommended_release("Samsung Xclipse 940", 5, &r) == -1);
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
    /* Samsung's driver's names; RADV's own on an Xclipse names its chip's codename, not its model (radv-xclipse's
       radv_physical_device.c: "<libdrm's marketing name> (RADV <codename>)") */
    assert(gui_xclipse_model("Samsung Xclipse 940") == 940 && gui_xclipse_model("Samsung Xclipse 530") == 530 &&
           gui_xclipse_model("Samsung Xclipse 960 r1") == 960);
    assert(!gui_xclipse_model("Samsung Xclipse") && !gui_xclipse_model("Samsung Xclipse 9x0") &&
           !gui_xclipse_model("Xclipse 940") && !gui_xclipse_model("AMD Unknown (RADV MAGELLAN CARL)") &&
           !gui_xclipse_model("Adreno (TM) 940") && !gui_adreno_model("Samsung Xclipse 940") &&
           !gui_mali_model("Samsung Xclipse 940"));
    assert(!strcmp(gui_driver_recommended("Adreno (TM) 830"), "GameHub Adreno 8xx") &&
           !strcmp(gui_driver_recommended("Adreno (TM) 610"), "Mr. Purple T30") &&
           !strcmp(gui_driver_recommended("Adreno (TM) 613"), "Mr. Purple T30") &&
           !strcmp(gui_driver_recommended("Adreno (TM) 618"), "Mr. Purple EOL-24.3.4") &&
           !strcmp(gui_driver_recommended("Adreno (TM) 740"), "Mr. Purple T23") &&
           !strcmp(gui_driver_recommended("Adreno (TM) A32"), "KIMCHI Latest") &&
           !strcmp(gui_driver_recommended(""), "Unsupported") &&
           !strcmp(gui_driver_recommended("Adreno (TM) 950"), "Unsupported"));
    /* NX-Mali on the Malis it gives Vulkan 1.3 (its command-stream parts); not on those it gives 1.1, nor on one it
       does not drive, and an Adreno of the G615's number by Adreno's table */
    {
        static const char* const at_13[] = {"Mali-G610 MC6", "Mali-G615 MC6", "Mali-G710 MC10",
                                            "Mali-G715-Immortalis MC11"};
        static const char* const at_11[] = {"Mali-G52", "Mali-G57 MC2", "Mali-G68 MC4", "Mali-G77 MC9", "Mali-G78AE"};
        for (size_t i = 0; i < sizeof at_13 / sizeof *at_13; ++i)
            assert(!strcmp(gui_driver_recommended(at_13[i]), "NX-Mali"));
        for (size_t i = 0; i < sizeof at_11 / sizeof *at_11; ++i)
            assert(!strcmp(gui_driver_recommended(at_11[i]), "Unsupported"));
        assert(!strcmp(gui_driver_recommended("Mali-G76 MC4"), "Unsupported") &&
               !strcmp(gui_driver_recommended("Adreno (TM) 615"), "Mr. Purple EOL-24.3.4"));
    }
    /* RADV Xclipse on the Xclipses it drives (radv-xclipse's README's Compatibility); not on another model, nor on an
       Adreno or a Mali of the same number */
    {
        static const char* const driven[] = {"Samsung Xclipse 530", "Samsung Xclipse 540", "Samsung Xclipse 550",
                                             "Samsung Xclipse 920", "Samsung Xclipse 940", "Samsung Xclipse 950",
                                             "Samsung Xclipse 960"};
        for (size_t i = 0; i < sizeof driven / sizeof *driven; ++i)
            assert(!strcmp(gui_driver_recommended(driven[i]), "RADV Xclipse"));
        assert(!strcmp(gui_driver_recommended("Samsung Xclipse 930"), "Unsupported") &&
               !strcmp(gui_driver_recommended("Samsung Xclipse 970"), "Unsupported") &&
               !strcmp(gui_driver_recommended("Samsung Xclipse 500"), "Unsupported") &&
               !strcmp(gui_driver_recommended("Mali-G920"), "Unsupported") &&
               !strcmp(gui_driver_recommended("AMD Unknown (RADV VANGOGHLITE)"), "Unsupported"));
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
