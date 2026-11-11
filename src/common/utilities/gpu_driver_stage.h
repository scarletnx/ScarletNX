/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A GPU driver package (gpu_driver.h) loaded beside a library of its own name. Android's linker gives a library asked
   for by name the one the process has under that name already, and on Android 16 the system's vulkan.adreno.so is
   there as an app starts, so a package of that name (Qualcomm's own) would never be opened. Such a package loads from
   a copy in its folder's unique/, each library under a name no library has: its own with the case of its last
   letter changed, the same length, so the names are written over in place in each copy's dynamic section
   (elf_dynamic.h) and no other byte of a library changes. No Android header: the tests run it on any Linux */
#ifndef SNX_COMMON_GPU_DRIVER_STAGE_H
#define SNX_COMMON_GPU_DRIVER_STAGE_H
#include "common/utilities/elf_dynamic.h"
#include <stddef.h>
#ifdef __linux__ /* Android too */
#include <sys/stat.h>
#endif

/* the folder of a package's renamed copies, in the package's own */
#define SNX_GPU_DRIVER_UNIQUE_DIR "unique"
/* name with the case of its last letter before ".so" changed (vulkan.adreno.so is vulkan.adrenO.so, mapper@4.0.so
   mappeR@4.0.so) into out: 1, else 0 for a name that does not end in .so, has no letter before it or no room in out */
int snx_gpu_driver_unique_name(const char* name, char* out, size_t size);
/* the name the library name goes by in a package's copies, names and count as snx_gpu_driver_stage gave them: name
   itself for a library the package has none of */
const char* snx_gpu_driver_staged_name(const SnxElfRename* names, size_t count, const char* name);
#ifdef __linux__
/* the library at path written whole into the file fd is open on, each of its library names that is one of names' from
   written as that one's to (snx_elf_rename): how many names that was, with the library's size and times in st; -1 with
   why saying the library and what is wrong, or the system's reason */
int snx_gpu_driver_copy(const char* path, int fd, const SnxElfRename* names, size_t count, struct stat* st, char* why,
                        size_t why_size);
/* every library of the package in the folder dir (its files named *.so, but those starting with '.') copied into
   dir/unique/ under its unique name, the names in each copy's dynamic section written over to match. A copy as old
   and as long as its library is left as it is: a package is copied once. 1 with the names in *names, count of them,
   for the caller to free; else 0 with why saying which file or folder, and the system's reason */
int snx_gpu_driver_stage(const char* dir, SnxElfRename** names, size_t* count, char* why, size_t why_size);
#endif
#endif
