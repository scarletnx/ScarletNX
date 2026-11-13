/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* GPU Drivers' Download Driver, its lists as Eden's driver fetcher has them (DriverFetcherFragment.kt): the
   repositories it asks, each one's releases as GitHub's API lists them, the assets of each with their links, the
   ones that are a GPU maker's own drivers marked, and the driver recommended for a GPU model. No SDL, no
   network: the page fetches (gui/main.c) */
#ifndef SNX_GUI_GPU_DRIVER_FETCH_H
#define SNX_GUI_GPU_DRIVER_FETCH_H
#include <stddef.h>

enum { GUI_DRIVER_ADRENO, GUI_DRIVER_MALI, GUI_DRIVER_XCLIPSE };
typedef struct {
    const char* name; /* as the page shows it */
    const char* path; /* owner/repository on GitHub */
    int tag_titles;   /* a release named by its tag, not its title */
    int by_time;      /* its releases newest first, not in the API's order */
    int gpu;          /* the GPU its drivers are for */
    unsigned min;     /* the oldest release recommended, by its title's version, parts a thousand apart (0.0.3: 3) */
} GuiDriverRepo;
#define GUI_DRIVER_REPOS 8
extern const GuiDriverRepo gui_driver_repos[GUI_DRIVER_REPOS];

typedef struct {
    char name[160];
    char url[512];
    unsigned long long size;
    int proprietary; /* a GPU maker's own driver, taken from a device: no license lets anyone share it */
} GuiDriverAsset;
typedef struct {
    char title[160];
    char tag[128];
    char body[1200]; /* its notes, cut there */
    int prerelease;
    int latest; /* the first that is no prerelease, in the API's order, as GitHub marks its latest */
    char published[32];
    size_t first, count; /* its assets, in the list's assets */
} GuiDriverRelease;
/* one allocation: the releases, then every asset (gui_driver_assets) */
typedef struct {
    GuiDriverRelease* releases;
    size_t count;
    size_t assets_count;
} GuiDriverReleases;
/* r's assets, past its releases in their block */
static inline GuiDriverAsset* gui_driver_assets(const GuiDriverReleases* r) {
    return (GuiDriverAsset*)(void*)(r->releases + r->count);
}
/* GitHub's releases of repo (its /repos/<path>/releases answer, n bytes) into out: 1, else 0 with why saying what is
   wrong in it */
int gui_driver_releases_parse(const char* json, size_t n, const GuiDriverRepo* repo, GuiDriverReleases* out, char* why,
                              size_t size);
void gui_driver_releases_free(GuiDriverReleases* r);
/* 1 for an asset that is a GPU maker's own driver: every one of the GameHub repository's, and one named Qualcomm */
int gui_driver_proprietary(const char* repo_path, const char* asset_name);
/* the Adreno model of a GPU's Vulkan name ("Adreno (TM) 830": 830, "Adreno (TM) A32": 32), 0 for another GPU */
int gui_adreno_model(const char* gpu_name);
/* a GPU's model as the recommendation numbers it, from its Vulkan name: an Adreno's, a Mali's plus 10000 ("Mali-G615
   MC6": 10615), an Xclipse's plus 20000 ("Samsung Xclipse 940": 20940), 0 for another GPU */
int gui_gpu_model(const char* gpu_name);
/* the driver Eden recommends for that model, as its page says it ("GameHub Adreno 8xx"), "Unsupported" for none */
const char* gui_driver_recommended(int model);
/* that driver among repository repo's releases (gui_driver_repos' index), listed in list: its index there, -1 when the
   recommendation is another repository's, none, or a release list does not have */
int gui_driver_recommended_release(int model, int repo, const GuiDriverReleases* list);
#endif
