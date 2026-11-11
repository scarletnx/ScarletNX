/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* common/utilities/gpu_driver_stage.c: the name a package's library is copied under, and, on Linux, a package of
   libraries made here (elf_image.h) copied into its unique/ folder: each copy apart from its library by the names
   alone, as old as it, the libraries themselves untouched, nothing but libraries copied; a copy made once, and again
   when its library changes; and each package that cannot be copied refused whole, saying which file and why */
#ifdef __linux__
#define _GNU_SOURCE /* utimensat and st_mtim under a strict dialect */
#endif
#include "common/utilities/gpu_driver_stage.h"
#include "common/utilities/common.h"
#include "elf_image.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static void unique_names(void) {
    static const char* const pairs[][2] = {{"vulkan.adreno.so", "vulkan.adrenO.so"},
                                           {"libgsl.so", "libgsL.so"},
                                           {"mapper@4.0.so", "mappeR@4.0.so"},
                                           {"libGSL.so", "libGSl.so"},
                                           {"a.so", "A.so"},
                                           {"not-9.so", "noT-9.so"}};
    static const char* const none[] = {"123.so", ".so", "so", "", "libgsl.so.1", "libgsl.SO", "libgsl", "@.4.so"};
    const SnxElfRename names[] = {{"libgsl.so", "libgsL.so"}, {"vulkan.adreno.so", "vulkan.adrenO.so"}};
    char out[SNX_ELF_NAME_MAX], back[SNX_ELF_NAME_MAX];
    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; ++i) {
        assert(snx_gpu_driver_unique_name(pairs[i][0], out, sizeof out) && !strcmp(out, pairs[i][1]));
        /* as long as the name, and the name again when asked of itself: never a third name */
        assert(strlen(out) == strlen(pairs[i][0]));
        assert(snx_gpu_driver_unique_name(out, back, sizeof back) && !strcmp(back, pairs[i][0]));
    }
    for (size_t i = 0; i < sizeof none / sizeof none[0]; ++i) {
        strcpy(out, "untouched");
        assert(!snx_gpu_driver_unique_name(none[i], out, sizeof out));
    }
    assert(!snx_gpu_driver_unique_name("libgsl.so", out, 9) && snx_gpu_driver_unique_name("libgsl.so", out, 10));
    assert(!strcmp(snx_gpu_driver_staged_name(names, 2, "vulkan.adreno.so"), "vulkan.adrenO.so"));
    assert(!strcmp(snx_gpu_driver_staged_name(names, 2, "libgsl.so"), "libgsL.so"));
    assert(!strcmp(snx_gpu_driver_staged_name(names, 2, "libc.so"), "libc.so"));
    assert(!strcmp(snx_gpu_driver_staged_name(NULL, 0, "libc.so"), "libc.so"));
}

#ifdef __linux__
#define ROOM 4096
typedef struct {
    char* dir;                             /* the package's folder */
    unsigned char driver[ROOM], gsl[ROOM]; /* its two libraries, as written */
    size_t driver_size, gsl_size;
} Package;

static char* in(const char* dir, const char* name) {
    char* path = snx_path_join(dir, name);
    assert(path);
    return path;
}
static void put(const char* dir, const char* name, const void* data, size_t size) {
    char* path = in(dir, name);
    assert(!snx_write_file(path, data, size));
    free(path);
}
/* dir/name's bytes are data's but for differ of them */
static void same_but(const char* dir, const char* name, const unsigned char* data, size_t size, size_t differ) {
    char* path = in(dir, name);
    size_t got = 0, n = 0;
    unsigned char* have = snx_read_file(path, &got);
    assert(have && got == size);
    for (size_t i = 0; i < size; ++i) n += have[i] != data[i];
    assert(n == differ);
    free(have);
    free(path);
}
static struct stat stat_of(const char* dir, const char* name) {
    struct stat st;
    char* path = in(dir, name);
    assert(!stat(path, &st));
    free(path);
    return st;
}
static int there(const char* dir, const char* name) {
    char* path = in(dir, name);
    int is = snx_path_exists(path);
    free(path);
    return is;
}
/* dir/name's times made those of st */
static void set_times(const char* dir, const char* name, const struct stat* st, long later) {
    struct timespec times[2] = {st->st_atim, st->st_mtim};
    char* path = in(dir, name);
    times[1].tv_sec += later;
    assert(!utimensat(AT_FDCWD, path, times, 0));
    free(path);
}
/* a package as a vendor's own is: a driver of the system's name that needs a library of the system's name too, with
   what a package has beside them and what a folder may: files that are no library, by their names, never copied */
static Package package(const char* root, const char* name) {
    const ElfImage driver = {.soname = "vulkan.adreno.so",
                             .needed = {"libgsl.so", "libc.so"},
                             .versions = {"libgsl.so", "libc.so"},
                             .symbols = {"HMI"}};
    const ElfImage gsl = {.soname = "libgsl.so", .needed = {"libc.so"}, .symbols = {"gsl_memory_alloc_pure"}};
    Package p = {0};
    char* sub;
    p.dir = in(root, name);
    assert(!snx_mkdirs(p.dir));
    p.driver_size = elf_image(&driver, p.driver, sizeof p.driver).size;
    p.gsl_size = elf_image(&gsl, p.gsl, sizeof p.gsl).size;
    put(p.dir, "vulkan.adreno.so", p.driver, p.driver_size);
    put(p.dir, "libgsl.so", p.gsl, p.gsl_size);
    put(p.dir, "meta.json", "{}", 2);
    put(p.dir, ".hidden.so", "no library", 10);
    put(p.dir, "123.so", "no library", 10);
    put(p.dir, "notes.so.txt", "no library", 10);
    sub = in(p.dir, "folder.so");
    assert(!snx_mkdirs(sub));
    free(sub);
    return p;
}
/* the package is copied: its two libraries' names, by name */
static void staged(const Package* p) {
    SnxElfRename* names = NULL;
    size_t count = 0;
    char why[600] = "";
    assert(snx_gpu_driver_stage(p->dir, &names, &count, why, sizeof why) && !why[0] && count == 2);
    assert(!strcmp(names[0].from, "libgsl.so") && !strcmp(names[0].to, "libgsL.so"));
    assert(!strcmp(names[1].from, "vulkan.adreno.so") && !strcmp(names[1].to, "vulkan.adrenO.so"));
    free(names);
}
/* the package is refused, with nothing for the caller to free: why, 600 bytes of room */
static const char* refused(const char* dir, char* why) {
    SnxElfRename* names = (SnxElfRename*)1;
    size_t count = 9;
    why[0] = 0;
    assert(!snx_gpu_driver_stage(dir, &names, &count, why, 600) && !names && !count);
    return why;
}

static void copies(const char* root) {
    Package p = package(root, "vendor");
    char *unique = in(p.dir, "unique"), **list = NULL, expect[600], why[600];
    struct stat driver, copy;
    size_t n = 0;
    staged(&p);
    /* each copy is its library with the names alone written over: the driver's own and the one it needs (its version
       need has that string), the other library's own */
    same_but(unique, "vulkan.adrenO.so", p.driver, p.driver_size, 2);
    same_but(unique, "libgsL.so", p.gsl, p.gsl_size, 1);
    /* the libraries themselves are as they were */
    same_but(p.dir, "vulkan.adreno.so", p.driver, p.driver_size, 0);
    same_but(p.dir, "libgsl.so", p.gsl, p.gsl_size, 0);
    /* nothing else is in the folder: no file that is no library, no file of a copy's making */
    assert(!snx_list_dir(unique, &list, &n) && n == 2);
    snx_free_strings(list, n);
    /* a copy is as old as its library, to the nanosecond */
    driver = stat_of(p.dir, "vulkan.adreno.so");
    copy = stat_of(unique, "vulkan.adrenO.so");
    assert(copy.st_mtim.tv_sec == driver.st_mtim.tv_sec && copy.st_mtim.tv_nsec == driver.st_mtim.tv_nsec);
    assert(copy.st_size == driver.st_size);
    /* a copy as old and as long as its library is not made again: what is written in it since stays */
    put(unique, "vulkan.adrenO.so", p.driver, p.driver_size);
    set_times(unique, "vulkan.adrenO.so", &driver, 0);
    staged(&p);
    same_but(unique, "vulkan.adrenO.so", p.driver, p.driver_size, 0);
    /* one of another length is */
    put(unique, "vulkan.adrenO.so", p.driver, p.driver_size - 1);
    set_times(unique, "vulkan.adrenO.so", &driver, 0);
    staged(&p);
    same_but(unique, "vulkan.adrenO.so", p.driver, p.driver_size, 2);
    /* and so is one whose library changed since, by a second or by a nanosecond */
    put(unique, "vulkan.adrenO.so", p.driver, p.driver_size);
    set_times(unique, "vulkan.adrenO.so", &driver, 0);
    set_times(p.dir, "vulkan.adreno.so", &driver, 1);
    staged(&p);
    same_but(unique, "vulkan.adrenO.so", p.driver, p.driver_size, 2);
    copy = stat_of(unique, "vulkan.adrenO.so");
    assert(copy.st_mtim.tv_sec == driver.st_mtim.tv_sec + 1);
    put(unique, "vulkan.adrenO.so", p.driver, p.driver_size);
    set_times(unique, "vulkan.adrenO.so", &copy, 0);
    copy.st_mtim.tv_nsec = copy.st_mtim.tv_nsec ? copy.st_mtim.tv_nsec - 1 : 1;
    set_times(p.dir, "vulkan.adreno.so", &copy, 0);
    staged(&p);
    same_but(unique, "vulkan.adrenO.so", p.driver, p.driver_size, 2);

    /* a file named as a library that is none: the package is refused, saying the file, and nothing of it is left */
    put(p.dir, "broken.so", "not a library", 13);
    snprintf(expect, sizeof expect, "the library %s/broken.so is not an ELF file: 13 bytes, the first four 6e 6f 74 20",
             p.dir);
    assert(!strcmp(refused(p.dir, why), expect));
    assert(!there(unique, "brokeN.so") && !there(unique, ".brokeN.so.tmp"));
    put(p.dir, "broken.so", "", 0);
    snprintf(expect, sizeof expect, "the library %s/broken.so has 0 bytes, which is no library", p.dir);
    assert(!strcmp(refused(p.dir, why), expect));
    {
        char* path = in(p.dir, "broken.so");
        assert(!remove(path));
        free(path);
    }
    staged(&p);
    /* a library written to a file its caller opened, as the copy of the system's Vulkan loader is: how many names */
    {
        const SnxElfRename name = {"libgsl.so", "libgsL.so"}, other = {"libvulkan.so", "libvulkaN.so"};
        char *path = in(p.dir, "libgsl.so"), *copy = in(p.dir, "copy");
        struct stat st;
        int fd = open(copy, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        assert(fd >= 0 && snx_gpu_driver_copy(path, fd, &name, 1, &st, why, sizeof why) == 1 && !close(fd));
        assert((size_t)st.st_size == p.gsl_size);
        same_but(p.dir, "copy", p.gsl, p.gsl_size, 1);
        fd = open(copy, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        assert(fd >= 0 && snx_gpu_driver_copy(path, fd, &other, 1, &st, why, sizeof why) == 0 && !close(fd));
        same_but(p.dir, "copy", p.gsl, p.gsl_size, 0);
        assert(!remove(copy));
        free(copy);
        free(path);
    }
    /* two libraries that would take each other's name */
    put(p.dir, "libgsL.so", p.gsl, p.gsl_size);
    snprintf(expect, sizeof expect, "the libraries libgsL.so and libgsl.so take each other's name in %s/unique", p.dir);
    assert(!strcmp(refused(p.dir, why), expect));
    free(unique);
    free(p.dir);
}

static void folders(const char* root) {
    Package p = package(root, "blocked");
    char *missing = in(root, "missing"), expect[600], name[200], why[600];
    /* no folder */
    snprintf(expect, sizeof expect, "the folder %s was not listed: %s", missing, strerror(ENOENT));
    assert(!strcmp(refused(missing, why), expect));
    /* where the copies go is a file */
    put(p.dir, "unique", "a file", 6);
    snprintf(expect, sizeof expect, "the library %s/libgsl.so was not copied to %s/unique/libgsL.so: %s", p.dir, p.dir,
             strerror(ENOTDIR));
    assert(!strcmp(refused(p.dir, why), expect));
    same_but(p.dir, "unique", (const unsigned char*)"a file", 6, 0);
    /* a library's name with no room */
    memset(name, 'x', 130);
    strcpy(name + 130, ".so");
    put(p.dir, name, p.gsl, p.gsl_size);
    snprintf(expect, sizeof expect, "the library %s has a name of 133 letters, and 127 is the most", name);
    assert(!strcmp(refused(p.dir, why), expect));
    free(missing);
    free(p.dir);
}
#endif

int main(void) {
    unique_names();
#ifdef __linux__
    {
        char* root = snx_temp_dir(NULL);
        assert(root);
        copies(root);
        folders(root);
        assert(!snx_remove_tree(root));
        free(root);
    }
    printf("gpu_driver_stage: ok\n");
#else
    printf("gpu_driver_stage: ok (a package's copies are made on Linux and Android only)\n");
#endif
    return 0;
}
