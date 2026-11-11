/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An ELF64 shared object's dynamic section, as the loader of GPU driver packages needs it (nx/runtime/platform/
   gpu_driver_load.h): the library names a file carries, written over at their length so that a copy loads beside a
   library of the same name, and a symbol of an image the system loaded, found in its own tables where dlsym does not
   answer (the dynamic linker's). Both in this machine's byte order. No system header: the tests run them anywhere */
#ifndef SNX_COMMON_ELF_DYNAMIC_H
#define SNX_COMMON_ELF_DYNAMIC_H
#include <stddef.h>

/* a library name's room, its 0 too */
#define SNX_ELF_NAME_MAX 128
/* a library's name and the one that takes its place, as long as it */
typedef struct {
    char from[SNX_ELF_NAME_MAX];
    char to[SNX_ELF_NAME_MAX];
} SnxElfRename;
/* image, the size bytes of an ELF64 file: each library name of its dynamic section (its DT_SONAME, its DT_NEEDED, the
   files of its version needs) that is the from of one of names, count of them, written over with that one's to. How
   many it wrote, 0 when the file names none of them; -1 with why saying what in the file, or which pair, is not as it
   must be: the image may then have some of its names written, and is not to be used */
int snx_elf_rename(unsigned char* image, size_t size, const SnxElfRename* names, size_t count, char* why,
                   size_t why_size);
/* the address of the function or object name that the ELF64 image loaded with its header at base defines, looked up in
   its DT_GNU_HASH table; NULL when it defines none, or base holds no such image */
void* snx_elf_loaded_symbol(const void* base, const char* name);
#endif
