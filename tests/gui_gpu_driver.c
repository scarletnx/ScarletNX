/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gui/gpu_driver.c: a package's zip added as a folder of the drivers folder, and each one refused with what it is,
   nothing written; the packages listed, a broken one too; one removed; the option a run is passed, the game's own or
   the launcher's (common/utilities/game_ini.c's snx_ini_has and snx_ini_drop); Freedreno Settings' presets */
#include "gui/gpu_driver.h"
#include "common/utilities/common.h"
#include "common/utilities/game_ini.h"
#include <assert.h>
#include <miniz.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define META                                                                                                           \
    "{\"schemaVersion\": 1, \"name\": \"Turnip test\", \"vendor\": \"Mesa\", \"driverVersion\": \"Vulkan 1.4.335\", "  \
    "\"minApi\": 28, \"libraryName\": \"libvulkan_freedreno.so\"}"

/* a zip of the named entries, in memory: name, then its text, NULL after the last; size in *size */
static void* zip_of(size_t* size, const char* const* entries) {
    mz_zip_archive z = {0};
    void* data = NULL;
    assert(mz_zip_writer_init_heap(&z, 0, 0));
    for (; entries[0]; entries += 2)
        assert(mz_zip_writer_add_mem(&z, entries[0], entries[1], strlen(entries[1]), MZ_BEST_SPEED));
    assert(mz_zip_writer_finalize_heap_archive(&z, &data, size));
    mz_zip_writer_end(&z);
    return data;
}
static int add(const char* const* entries, const char* file, const char* dir, int api, char* id, char* said) {
    size_t size = 0;
    void* zip = zip_of(&size, entries);
    int r = gui_gpu_driver_add(zip, size, file, dir, api, id, 100, said, 600);
    mz_free(zip);
    return r;
}
static int has(const char* text, const char* part) { return strstr(text, part) != NULL; }
/* the file at path is size bytes of data */
static int holds(const char* path, const void* data, size_t size) {
    size_t n = 0;
    unsigned char* text = snx_read_file(path, &n);
    int same = text && n == size && !memcmp(text, data, size);
    free(text);
    return same;
}
/* name's CRC-32 in the zip's central directory, one bit of it changed */
static void bad_crc(unsigned char* zip, size_t size, const char* name) {
    size_t i, n = strlen(name);
    for (i = 0; i + 46 + n <= size; ++i)
        if (!memcmp(zip + i, "PK\1\2", 4) && zip[i + 28] == n && !zip[i + 29] && !memcmp(zip + i + 46, name, n)) {
            zip[i + 16] ^= 1;
            return;
        }
    assert(!"no such entry");
}
static int left_over(const char* dir) {
    char** names = NULL;
    size_t n = 0, i;
    int found = 0;
    snx_list_dir(dir, &names, &n);
    for (i = 0; i < n; ++i) found |= names[i][0] == '.';
    snx_free_strings(names, n);
    return found;
}

int main(void) {
    char *root = snx_temp_dir(NULL), *dir, *path, *ini, *option, id[100], said[600];
    GuiGpuDriver* list;
    size_t n;
    int i;
    assert(root);
    dir = snx_path_join(root, SNX_GPU_DRIVERS_DIR);
    /* a package as released: meta.json, its library (an arm64 ELF header, NULs in it) deflated and another library
       stored */
    {
        static const char extra[] = "a library the zip stores as it is";
        mz_zip_archive z = {0};
        void* zip = NULL;
        size_t size = 0;
        unsigned char lib[64] = {0};
        memcpy(lib, "\177ELF\2\1\1", 7);
        lib[18] = 183;
        assert(mz_zip_writer_init_heap(&z, 0, 0));
        assert(mz_zip_writer_add_mem(&z, "meta.json", META, strlen(META), MZ_BEST_SPEED));
        assert(mz_zip_writer_add_mem(&z, "libvulkan_freedreno.so", lib, sizeof lib, MZ_BEST_SPEED));
        assert(mz_zip_writer_add_mem(&z, "libextra.so", extra, sizeof extra - 1, MZ_NO_COMPRESSION));
        assert(mz_zip_writer_finalize_heap_archive(&z, &zip, &size));
        mz_zip_writer_end(&z);
        /* added: its folder named after the zip, its files in it, nothing left beside it */
        assert(gui_gpu_driver_add(zip, size, "/sdcard/Download/Turnip_v32.zip", dir, 36, id, sizeof id, said,
                                  sizeof said) == GUI_DRIVER_ADDED);
        assert(!strcmp(id, "Turnip_v32") &&
               !strcmp(said, "Added Turnip test (Vulkan 1.4.335), in gpu_drivers/Turnip_v32"));
        path = snx_format("%s/Turnip_v32/libvulkan_freedreno.so", dir);
        assert(holds(path, lib, sizeof lib));
        free(path);
        path = snx_format("%s/Turnip_v32/libextra.so", dir);
        assert(holds(path, extra, sizeof extra - 1));
        free(path);
        assert(!left_over(dir));
        /* the same package again: there already, said so */
        assert(gui_gpu_driver_add(zip, size, "Turnip_v32.zip", dir, 36, id, sizeof id, said, sizeof said) ==
               GUI_DRIVER_THERE);
        assert(!strcmp(said, "Already added: Turnip test (gpu_drivers/Turnip_v32)"));
        /* away from Android (api 0) minApi is not checked */
        assert(gui_gpu_driver_add(zip, size, "Copy.adpkg.zip", dir, 0, id, sizeof id, said, sizeof said) ==
                   GUI_DRIVER_ADDED &&
               !strcmp(id, "Copy"));
        /* a member whose bytes don't give the CRC-32 the zip says: refused with both, nothing written */
        bad_crc(zip, size, "libvulkan_freedreno.so");
        assert(gui_gpu_driver_add(zip, size, "Bad_crc.zip", dir, 36, id, sizeof id, said, sizeof said) ==
               GUI_DRIVER_REFUSED);
        assert(has(said, "Not added: Bad_crc.zip's libvulkan_freedreno.so does not unpack (its CRC-32 is ") &&
               has(said, " where the zip says "));
        assert(!left_over(dir));
        mz_free(zip);
        /* its library for another machine: refused, saying which */
        lib[18] = 62;
        memset(&z, 0, sizeof z);
        assert(mz_zip_writer_init_heap(&z, 0, 0));
        assert(mz_zip_writer_add_mem(&z, "meta.json", META, strlen(META), MZ_BEST_SPEED));
        assert(mz_zip_writer_add_mem(&z, "libvulkan_freedreno.so", lib, sizeof lib, MZ_BEST_SPEED));
        assert(mz_zip_writer_finalize_heap_archive(&z, &zip, &size));
        mz_zip_writer_end(&z);
        assert(gui_gpu_driver_add(zip, size, "x86.zip", dir, 36, id, sizeof id, said, sizeof said) ==
               GUI_DRIVER_REFUSED);
        assert(!strcmp(said, "Not added: x86.zip's libvulkan_freedreno.so is a 64-bit library for machine 62, not "
                             "arm64's (64-bit, machine 183)"));
        assert(!left_over(dir));
        mz_free(zip);
    }
    {
        /* another package under a folder name taken: refused, naming what is there */
        const char* const other[] = {"meta.json", "{\"name\": \"Other\", \"minApi\": 28, \"libraryName\": \"x.so\"}",
                                     "x.so", "\177ELF", NULL};
        assert(add(other, "Turnip_v32.zip", dir, 36, id, said) == GUI_DRIVER_REFUSED);
        assert(!strcmp(said, "Not added: gpu_drivers/Turnip_v32 holds Turnip test Vulkan 1.4.335 already; remove it "
                             "first") &&
               !id[0]);
    }
    {
        /* each package refused says what is wrong with it, and leaves nothing */
        const char* const nested[] = {"turnip/meta.json", META, NULL};
        const char* const folder[] = {"turnip/", "", "meta.json", META, NULL};
        const char* const escape[] = {"meta.json", META, "../evil.so", "x", NULL};
        const char* const no_meta[] = {"libvulkan_freedreno.so", "\177ELF", NULL};
        const char* const no_lib[] = {"meta.json", META, NULL};
        const char* const bad_meta[] = {"meta.json", "{\"name\": \"A\", \"libraryName\": \"a.so\"}", "a.so", "x", NULL};
        const char* const new_api[] = {"meta.json", "{\"name\": \"New\", \"minApi\": 99, \"libraryName\": \"a.so\"}",
                                       "a.so", "\177ELF", NULL};
        assert(add(nested, "n.zip", dir, 36, id, said) == GUI_DRIVER_REFUSED &&
               !strcmp(said, "Not added: n.zip's turnip/meta.json is not a file at the zip's top; a driver's files are "
                             "at the zip's top"));
        assert(add(folder, "f.zip", dir, 36, id, said) == GUI_DRIVER_REFUSED && has(said, "turnip/ is a folder"));
        assert(add(escape, "e.zip", dir, 36, id, said) == GUI_DRIVER_REFUSED && has(said, "../evil.so"));
        assert(add(no_meta, "m.zip", dir, 36, id, said) == GUI_DRIVER_REFUSED &&
               !strcmp(said, "Not added: m.zip has no meta.json at its top"));
        assert(add(no_lib, "l.zip", dir, 36, id, said) == GUI_DRIVER_REFUSED &&
               !strcmp(said, "Not added: l.zip has no libvulkan_freedreno.so, the library its meta.json names"));
        assert(add(bad_meta, "b.zip", dir, 36, id, said) == GUI_DRIVER_REFUSED &&
               !strcmp(said, "Not added: b.zip: meta.json has no minApi"));
        assert(add(new_api, "a.zip", dir, 36, id, said) == GUI_DRIVER_REFUSED &&
               !strcmp(said, "Not added: New needs Android API 99; this device has API 36"));
        assert(!gui_gpu_driver_add("PK", 2, "/x/notzip.zip", dir, 36, id, sizeof id, said, sizeof said) &&
               has(said, "Not added: notzip.zip is not a zip ("));
        assert(!left_over(dir));
    }
    /* the packages listed by name; a folder whose meta.json does not read is listed with why */
    path = snx_format("%s/broken", dir);
    assert(!snx_mkdirs(path));
    free(path);
    path = snx_format("%s/broken/meta.json", dir);
    assert(!snx_write_file(path, "{\"name\": 1", 10));
    free(path);
    list = gui_gpu_drivers_list(dir, &n);
    assert(n == 3 && list);
    assert(!strcmp(list[0].id, "broken") && has(list[0].error, "not JSON near byte 10 of 10"));
    assert(!strcmp(list[1].id, "Copy") && !strcmp(list[1].meta.name, "Turnip test") && !list[1].error[0]);
    assert(!strcmp(list[2].id, "Turnip_v32"));
    free(list);
    /* removed: gone, then said not there; a name that is no folder's refused */
    assert(gui_gpu_driver_remove(dir, "Copy", said, sizeof said) && !strcmp(said, "Removed gpu_drivers/Copy"));
    assert(!gui_gpu_driver_remove(dir, "Copy", said, sizeof said) && has(said, "Copy is not there"));
    assert(!gui_gpu_driver_remove(dir, "../x", said, sizeof said) &&
           !strcmp(said, "Not removed: ../x is no driver's folder name"));
    list = gui_gpu_drivers_list(dir, &n);
    assert(n == 2);
    free(list);
    /* a run's option: the game's own key= wins, an empty one too (the system's driver); else the launcher's */
    ini = snx_path_join(root, "Game.ini");
    option = gui_run_option("gpu_driver", ini, "Turnip_v32"); /* no .ini yet */
    assert(option && !strcmp(option, "--gpu_driver=Turnip_v32"));
    free(option);
    assert(!snx_ini_record(ini, "addons", "") && !snx_ini_has(ini, "gpu_driver"));
    assert(!snx_ini_record(ini, "gpu_driver", "") && snx_ini_has(ini, "gpu_driver"));
    assert(!gui_run_option("gpu_driver", ini, "Turnip_v32") && !gui_run_option("freedreno_env", ini, ""));
    assert(!snx_ini_drop(ini, "gpu_driver") && !snx_ini_has(ini, "gpu_driver") && snx_ini_has(ini, "addons"));
    assert(!snx_ini_drop(ini, "gpu_driver")); /* none: as it was */
    option = gui_run_option("freedreno_env", ini, "TU_DEBUG=sysmem");
    assert(option && !strcmp(option, "--freedreno_env=TU_DEBUG=sysmem"));
    free(option);
    free(ini);
    /* Freedreno Settings' presets: Eden's nine, each a text freedreno_env takes, the last setting none */
    for (i = 0; gui_freedreno_presets[i].name; ++i) {
        SnxFreedrenoVar vars[SNX_FREEDRENO_MAX];
        assert(snx_freedreno_parse(gui_freedreno_presets[i].variables, vars, SNX_FREEDRENO_MAX, said, sizeof said) >=
               0);
    }
    assert(i == 9 && !gui_freedreno_presets[8].variables[0]);
    assert(!snx_remove_tree(root));
    free(dir);
    free(root);
    puts("gui_gpu_driver: ok");
    return 0;
}
