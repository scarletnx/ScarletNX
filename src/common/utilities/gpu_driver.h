/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Vulkan driver packages a player adds, in libadrenotools' format (its tools/ADPKG.md): a zip of a meta.json and the
   driver's libraries, each kept in a folder of its own under the drivers folder, which a game's run loads in place of
   the system's driver on Android arm64 with an Adreno, a Mali or an Xclipse GPU (nx/runtime/platform/gpu_driver_load.c). What
   is read and checked the same way by the launcher, which adds them (gui/gpu_driver.c), and by a run, which loads one:
   their meta.json, their folder's name, whether this device loads them, whether a run's process mapped one, and the
   Freedreno variables a run sets for Mesa's Turnip. No SDL, no Android headers: the tests run them anywhere */
#ifndef SNX_COMMON_GPU_DRIVER_H
#define SNX_COMMON_GPU_DRIVER_H
#include <stddef.h>

/* the most bytes of a meta.json, as Eden's GpuDriverMetadata reads it: a package's is a few hundred */
#define SNX_GPU_DRIVER_META_MAX 500000
/* the folder each package's folder is in, under the user's data folder (snx_gpu_drivers_dir) */
#define SNX_GPU_DRIVERS_DIR     "gpu_drivers"
/* the oldest Android a package loads on: 9, API 28, whose linker has in its own symbol table the namespace functions
   the loader calls (nx/runtime/platform/gpu_driver_android.h) */
#define SNX_GPU_DRIVER_MIN_API  28

/* a package's meta.json: name, libraryName and minApi it must have, the rest as it has them ("" when not) */
typedef struct {
    char name[256];
    char description[1024];
    char author[256];
    char vendor[64];
    char driver_version[128]; /* driverVersion: free text ("Vulkan 1.4.335", "0.842.8") */
    char package_version[64]; /* packageVersion */
    char library[128];        /* libraryName: the driver's library, a file of the package */
    int min_api;              /* minApi: the oldest Android API level it runs on */
} SnxGpuDriverMeta;
/* meta.json's text, n bytes, into meta: 1, else 0 with why saying what is wrong and the value it has */
int snx_gpu_driver_meta(const char* text, size_t n, SnxGpuDriverMeta* meta, char* why, size_t size);
/* the drivers folder: SNX_GPU_DRIVERS_DIR in the user's data folder, which on Android is in the app's own storage, as
   the linker loads libraries only from there. Owned, NULL when the environment names no home */
char* snx_gpu_drivers_dir(void);
/* a package's folder name from its zip's file name (a path, a content URI's last part): the name without .zip or
   .adpkg.zip, each character but letters, digits, '.', '-' and '_' made '_'. 1, else 0 with why saying so */
int snx_gpu_driver_id(const char* file, char* id, size_t size, char* why, size_t why_size);
/* 1 for a folder name a package can have, which a run's gpu_driver names: what snx_gpu_driver_id makes */
int snx_gpu_driver_id_valid(const char* id);
/* the first bytes of a library, n of them (64 at least for a whole ELF header): 1 for an arm64 ELF, else 0 with why
   saying what it is */
int snx_gpu_driver_elf(const unsigned char* head, size_t n, char* why, size_t size);

/* what decides whether this device loads a package */
typedef struct {
    const char* system; /* "Android", "Linux", "Windows", "macOS", "iOS", "FreeBSD"... */
    const char* arch;   /* "arm64", "x86_64"... */
    int api;            /* Android's API level, 0 elsewhere */
    int kgsl;           /* /dev/kgsl-3d0 is there: an Adreno GPU's kernel driver */
    int mali;           /* /dev/mali0 or /dev/mali is there: a Mali GPU's (kbase's) */
    int amdgpu;         /* a render node's kernel driver is amdgpu, the name Mesa's RADV asks of it: on Android an
                           Xclipse GPU's (Samsung's sgpu answers by it) */
    char render[96];    /* that node and its driver, "/dev/dri/renderD128, kernel driver amdgpu"; without one, the
                           first render node there is with its driver, or with why it did not answer; "" for none */
    int built;          /* the build carries the packages' loader: Android arm64 */
} SnxGpuDriverHost;
/* this device's; on Android with no Adreno or Mali, its render nodes asked too (snx_gpu_driver_render_node) */
void snx_gpu_driver_host(SnxGpuDriverHost* host);
/* host's amdgpu and render from /dev/dri's render nodes, each opened to ask DRM its kernel driver's name; none where
   there is no DRM (a system other than Linux and Android) */
void snx_gpu_driver_render_node(SnxGpuDriverHost* host);
/* what a device that loads no package lacks, for the few words a row has; SNX_GPU_DRIVER_LOADS when it loads them */
typedef enum {
    SNX_GPU_DRIVER_LOADS,
    SNX_GPU_DRIVER_NOT_ANDROID, /* another system: a package takes the place of Android's Vulkan loader's driver */
    SNX_GPU_DRIVER_NOT_ARM64,   /* the packages' only ABI */
    SNX_GPU_DRIVER_NO_GPU,      /* no Adreno, no Mali and no Xclipse: no package drives the GPU */
    SNX_GPU_DRIVER_OLD_ANDROID, /* under SNX_GPU_DRIVER_MIN_API */
    SNX_GPU_DRIVER_NOT_BUILT    /* this build carries no loader for them */
} SnxGpuDriverLack;
SnxGpuDriverLack snx_gpu_driver_lack(const SnxGpuDriverHost* host);
/* 1 when host loads packages (snx_gpu_driver_lack); what says so either way, with its values: "This device: Android
   API 36, arm64, Adreno GPU (/dev/kgsl-3d0): custom drivers load." or the reason they do not */
int snx_gpu_driver_loads(const SnxGpuDriverHost* host, char* what, size_t size);
/* what a run says when the package it loaded, folder id and meta.json's name, lists no Vulkan device: the package and
   host's GPU, which it does not drive (Turnip on an Xclipse, RADV on an Adreno) */
void snx_gpu_driver_no_device(const char* id, const char* name, const SnxGpuDriverHost* host, char* out, size_t size);
/* 1 when maps, /proc/self/maps' text, has a library mapped from the folder dir or a folder in it, by dir's name or by
   another the kernel gives it: the package a run asked for is the driver its Vulkan loader opened, not the system's in
   its place */
int snx_gpu_driver_mapped(const char* maps, const char* dir);

/* Freedreno's variables (Mesa's freedreno and Turnip read them as the driver starts: docs.mesa3d.org/drivers/
   freedreno.html), as a run's freedreno_env holds them: NAME=value, a ';' between two. A name is capitals, digits and
   '_' starting TU_, FD_ or IR3_; a value has no ';' */
#define SNX_FREEDRENO_MAX  16
#define SNX_FREEDRENO_TEXT 512 /* the setting's room */
typedef struct {
    char name[48];
    char value[192];
} SnxFreedrenoVar;
/* text's variables into vars, max at most: how many, else -1 with why saying which one is wrong and why. A name twice:
   the later value */
int snx_freedreno_parse(const char* text, SnxFreedrenoVar* vars, int max, char* why, size_t size);
/* 1 for a name a variable can have */
int snx_freedreno_name_valid(const char* name);
/* count variables as the setting writes them, into out: 1, else 0 when out has no room */
int snx_freedreno_text(const SnxFreedrenoVar* vars, int count, char* out, size_t size);
#endif
