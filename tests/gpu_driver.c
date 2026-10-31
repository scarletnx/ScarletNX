/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* common/utilities/gpu_driver.c: a driver package's meta.json in the shapes released packages have, and each one
   refused with the value said; a package's folder name from its zip's; an arm64 library told from others; whether a
   device loads packages, said with its values; whether a process mapped a package's library; Freedreno's variables */
#include "common/utilities/gpu_driver.h"
#include "common/utilities/common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int meta_of(const char* text, SnxGpuDriverMeta* meta, char* why) {
    return snx_gpu_driver_meta(text, strlen(text), meta, why, 512);
}
static int has(const char* text, const char* part) { return strstr(text, part) != NULL; }

int main(void) {
    SnxGpuDriverMeta m;
    SnxGpuDriverHost host;
    SnxFreedrenoVar vars[SNX_FREEDRENO_MAX];
    char why[512], id[128], text[SNX_FREEDRENO_TEXT], *big, *dir;
    unsigned char elf[64];
    /* the shapes released packages have: a Mesa build's, with driverVersion as free text, and a Qualcomm one's */
    assert(
        meta_of("{\"schemaVersion\": 1, \"name\": \"Turnip test\", \"description\": \"Built from Mesa\", "
                "\"author\": \"someone\", \"packageVersion\": \"1\", \"vendor\": \"Mesa\", "
                "\"driverVersion\": \"Vulkan 1.4.335\", \"minApi\": 28, \"libraryName\": \"libvulkan_freedreno.so\"}",
                &m, why));
    assert(!strcmp(m.name, "Turnip test") && !strcmp(m.library, "libvulkan_freedreno.so") && m.min_api == 28);
    assert(!strcmp(m.vendor, "Mesa") && !strcmp(m.driver_version, "Vulkan 1.4.335") && !strcmp(m.author, "someone"));
    assert(meta_of("{\"name\": \"Blob\", \"vendor\": \"Qualcomm\", \"driverVersion\": \"0.842.8\", \"minApi\": 35, "
                   "\"libraryName\": \"vulkan.adreno.so\", \"packageVersion\": 2}",
                   &m, why));
    assert(!strcmp(m.package_version, "2") && !m.description[0] && m.min_api == 35); /* a number as its text */
    /* what it must have, said by its key, and each wrong value said as it is */
    assert(!meta_of("{\"name\": \"A\", \"minApi\": 28}", &m, why) && has(why, "has no libraryName"));
    assert(!meta_of("{\"libraryName\": \"a.so\", \"minApi\": 28}", &m, why) && has(why, "has no name"));
    assert(!meta_of("{\"name\": \"A\", \"libraryName\": \"a.so\"}", &m, why) && has(why, "has no minApi"));
    assert(!meta_of("{\"name\": \"A\", \"libraryName\": \"a.so\", \"minApi\": \"28\"}", &m, why) &&
           has(why, "minApi is \"28\""));
    assert(!meta_of("{\"name\": \"A\", \"libraryName\": \"a.so\", \"minApi\": 28.5}", &m, why) &&
           has(why, "minApi is 28.5"));
    assert(!meta_of("{\"name\": \"A\", \"libraryName\": \"lib/a.so\", \"minApi\": 28}", &m, why) &&
           has(why, "\"lib/a.so\""));
    assert(!meta_of("{\"name\": \"A\", \"libraryName\": \"a.json\", \"minApi\": 28}", &m, why) &&
           has(why, "\"a.json\""));
    assert(!meta_of("{\"name\": [1, 2], \"libraryName\": \"a.so\", \"minApi\": 28}", &m, why) &&
           has(why, "name is [1,2]"));
    assert(!meta_of("{\"name\": \"\", \"libraryName\": \"a.so\", \"minApi\": 28}", &m, why) &&
           has(why, "name is empty"));
    assert(!meta_of("[1, 2]", &m, why) && has(why, "not a JSON object"));
    assert(!meta_of("{\"name\": \"A\",, }", &m, why) && has(why, "not JSON near byte 14 of 16"));
    assert(!snx_gpu_driver_meta("{\"name\"\0}", 10, &m, why, sizeof why) && has(why, "near byte 7 of 10"));
    big = (char*)malloc(SNX_GPU_DRIVER_META_MAX + 2);
    memset(big, ' ', SNX_GPU_DRIVER_META_MAX + 1);
    big[SNX_GPU_DRIVER_META_MAX + 1] = 0;
    assert(!meta_of(big, &m, why) && has(why, "500001 bytes, over the 500000"));
    memset(big, 'x', 300);
    memcpy(big, "{\"name\": \"", 10);
    strcpy(big + 300, "\", \"libraryName\": \"a.so\", \"minApi\": 28}");
    assert(!meta_of(big, &m, why) && has(why, "name is 290 bytes, over the 255"));
    free(big);
    /* a package's folder name from its zip's */
    assert(snx_gpu_driver_id("Turnip_v26.0.0_R8.zip", id, sizeof id, why, sizeof why) &&
           !strcmp(id, "Turnip_v26.0.0_R8"));
    assert(snx_gpu_driver_id("/sdcard/Download/turnip_T30-toasted.adpkg.ZIP", id, sizeof id, why, sizeof why) &&
           !strcmp(id, "turnip_T30-toasted"));
    assert(snx_gpu_driver_id("C:\\drivers\\My Driver (1).zip", id, sizeof id, why, sizeof why) &&
           !strcmp(id, "My_Driver__1_"));
    assert(snx_gpu_driver_id("..hidden.zip", id, sizeof id, why, sizeof why) && !strcmp(id, "hidden"));
    assert(!snx_gpu_driver_id("/x/.zip", id, sizeof id, why, sizeof why) &&
           has(why, "\"/x/.zip\" makes no folder name"));
    assert(snx_gpu_driver_id_valid("Turnip_v26.0.0_R8") && !snx_gpu_driver_id_valid("../x") &&
           !snx_gpu_driver_id_valid(".x") && !snx_gpu_driver_id_valid("a b") && !snx_gpu_driver_id_valid(""));
    /* an arm64 library, and others said by what they are */
    memset(elf, 0, sizeof elf);
    memcpy(elf, "\177ELF\2\1\1", 7);
    elf[18] = 183;
    assert(snx_gpu_driver_elf(elf, sizeof elf, why, sizeof why));
    elf[18] = 62;
    assert(!snx_gpu_driver_elf(elf, sizeof elf, why, sizeof why) && has(why, "64-bit library for machine 62"));
    elf[4] = 1;
    elf[18] = 40;
    assert(!snx_gpu_driver_elf(elf, sizeof elf, why, sizeof why) && has(why, "32-bit library for machine 40"));
    assert(!snx_gpu_driver_elf((const unsigned char*)"PK\3\4", 4, why, sizeof why) && has(why, "starts 50 4b 03 04"));
    /* the devices that load packages, and what each other one is said to lack */
    host.system = "Android", host.arch = "arm64", host.api = 36, host.kgsl = 1, host.built = 1;
    assert(snx_gpu_driver_loads(&host, why, sizeof why) &&
           !strcmp(why, "This device: Android API 36, arm64, Adreno GPU (/dev/kgsl-3d0): custom drivers load."));
    host.api = 27;
    assert(!snx_gpu_driver_loads(&host, why, sizeof why) && has(why, "Android API 27") && has(why, "API 28) or newer"));
    host.api = 36, host.kgsl = 0;
    assert(!snx_gpu_driver_loads(&host, why, sizeof why) && has(why, "no /dev/kgsl-3d0 (the GPU is not an Adreno)"));
    host.system = "Linux", host.arch = "x86_64", host.api = 0, host.built = 0;
    assert(!snx_gpu_driver_loads(&host, why, sizeof why) && has(why, "This device: Linux x86_64:") &&
           has(why, "Games here use the system driver."));
    host.system = "Android", host.arch = "x86_64", host.api = 36, host.kgsl = 0;
    assert(!snx_gpu_driver_loads(&host, why, sizeof why) && has(why, "Android API 36, x86_64"));
    snx_gpu_driver_host(&host); /* this machine's, whatever it is: named, and loading only where it can */
    assert(host.system && host.arch && (snx_gpu_driver_loads(&host, why, sizeof why) == 0 || host.built));
    /* the package's library mapped from its folder, or from unique/ in it; another folder's or the system's is not */
    {
        const char* maps = "7f00000000-7f00001000 r--p 00000000 fd:00 123  /system/lib64/libvulkan.so\n"
                           "7f00002000-7f00003000 r-xp 00001000 fd:00 456  /vendor/lib64/hw/vulkan.adreno.so\n"
                           "7f00004000-7f00005000 rw-p 00000000 00:00 0    [anon:linker_alloc]\n";
        const char* ours =
            "7f00000000-7f00001000 r--p 00000000 fd:00 789  /data/user/0/org.scarletnx/files/.local/share"
            "/scarletnx/gpu_drivers/Turnip/libvulkan_freedreno.so\n";
        const char* unique = "7f0-7f1 r-xp 0 fd:00 1 /d/gpu_drivers/Blob/unique/vulkan.adrenO.so (deleted)";
        assert(
            !snx_gpu_driver_mapped(maps, "/data/user/0/org.scarletnx/files/.local/share/scarletnx/gpu_drivers/Turnip"));
        assert(
            snx_gpu_driver_mapped(ours, "/data/user/0/org.scarletnx/files/.local/share/scarletnx/gpu_drivers/Turnip/"));
        assert(
            !snx_gpu_driver_mapped(ours, "/data/user/0/org.scarletnx/files/.local/share/scarletnx/gpu_drivers/Turn"));
        assert(snx_gpu_driver_mapped(unique, "/d/gpu_drivers/Blob") && !snx_gpu_driver_mapped("", "/d"));
    }
#ifndef _WIN32
    /* the folder by the kernel's name for it, not the run's (Android's /data/data for /data/user/0) */
    {
        char *root = snx_temp_dir(NULL), *real = snx_path_join(root, "real"), *alias = snx_path_join(root, "alias"),
             *unique = snx_path_join(real, "Turnip/unique"), *lib = snx_path_join(unique, "libvulkan_freedreno.so"),
             *other = snx_path_join(real, "Other"), *ours = snx_path_join(alias, "Turnip"), *maps;
        assert(root && !snx_mkdirs(unique) && !snx_mkdirs(other) && !snx_write_file(lib, "", 0));
        assert(!snx_symlink(real, alias));
        maps = snx_format("7f0-7f1 r-xp 0 fe:3e 1 %s/libvulkan_freedreno.so\n7f1-7f2 r--p 0 fe:3e 2 %s (deleted)",
                          other, lib);
        assert(maps && snx_gpu_driver_mapped(maps, ours));
        *strchr(maps, '\n') = 0; /* another folder's library alone */
        assert(!snx_gpu_driver_mapped(maps, ours));
        assert(!snx_remove_tree(root));
        free(maps);
        free(ours);
        free(other);
        free(lib);
        free(unique);
        free(alias);
        free(real);
        free(root);
    }
#endif
    /* Freedreno's variables: NAME=value, ';' between, spaces round them left out, a name twice its later value */
    assert(snx_freedreno_parse("TU_DEBUG=sysmem,nobin; FD_RD_DUMP = enable ;", vars, SNX_FREEDRENO_MAX, why,
                               sizeof why) == 2);
    assert(!strcmp(vars[0].name, "TU_DEBUG") && !strcmp(vars[0].value, "sysmem,nobin"));
    assert(!strcmp(vars[1].name, "FD_RD_DUMP") && !strcmp(vars[1].value, "enable"));
    assert(snx_freedreno_parse("TU_DEBUG=sysmem;TU_DEBUG=noconform", vars, SNX_FREEDRENO_MAX, why, sizeof why) == 1 &&
           !strcmp(vars[0].value, "noconform"));
    assert(snx_freedreno_parse("", vars, SNX_FREEDRENO_MAX, why, sizeof why) == 0);
    assert(snx_freedreno_parse("TU_BREADCRUMBS=", vars, SNX_FREEDRENO_MAX, why, sizeof why) == 1 && !vars[0].value[0]);
    assert(snx_freedreno_parse("LD_PRELOAD=/x.so", vars, SNX_FREEDRENO_MAX, why, sizeof why) == -1 &&
           has(why, "\"LD_PRELOAD\" is not one of Freedreno's variables"));
    assert(snx_freedreno_parse("TU_DEBUG", vars, SNX_FREEDRENO_MAX, why, sizeof why) == -1 &&
           has(why, "TU_DEBUG has no value"));
    assert(snx_freedreno_parse("TU_debug=x", vars, SNX_FREEDRENO_MAX, why, sizeof why) == -1 && has(why, "TU_debug"));
    assert(snx_freedreno_parse("TU_A=1;TU_B=2;TU_C=3", vars, 2, why, sizeof why) == -1 &&
           has(why, "TU_C is variable 3, over the 2 a run sets"));
    assert(snx_freedreno_name_valid("IR3_SHADER_DEBUG") && snx_freedreno_name_valid("FD_DEV_FEATURES") &&
           !snx_freedreno_name_valid("TU_") && !snx_freedreno_name_valid("MESA_DEBUG"));
    assert(snx_freedreno_parse("TU_DEBUG=sysmem;FD_RD_DUMP_FRAMES=0-100", vars, SNX_FREEDRENO_MAX, why, sizeof why) ==
           2);
    assert(snx_freedreno_text(vars, 2, text, sizeof text) && !strcmp(text, "TU_DEBUG=sysmem;FD_RD_DUMP_FRAMES=0-100"));
    assert(!snx_freedreno_text(vars, 2, text, 10) && !text[0]);
    /* the drivers folder, in the user's data folder */
    dir = snx_gpu_drivers_dir();
    assert(!dir || (strlen(dir) > strlen(SNX_GPU_DRIVERS_DIR) &&
                    !strcmp(dir + strlen(dir) - strlen(SNX_GPU_DRIVERS_DIR), SNX_GPU_DRIVERS_DIR)));
    free(dir);
    puts("gpu_driver: ok");
    return 0;
}
