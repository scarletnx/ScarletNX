/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gpu_driver_stage.h. A library is mapped privately, its names written in the mapping and the mapping written to the
   copy: only the pages of its string table are ever copied in memory, whatever the library's size */
#ifdef __linux__
#define _GNU_SOURCE /* O_CLOEXEC, futimens and st_mtim under a strict dialect */
#endif
#include "common/utilities/gpu_driver_stage.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef __linux__
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

int snx_gpu_driver_unique_name(const char* name, char* out, size_t size) {
    size_t n = strlen(name);
    if (n <= 3 || strcmp(name + n - 3, ".so") || n >= size) return 0;
    memcpy(out, name, n + 1);
    for (size_t i = n - 3; i-- > 0;) {
        if (out[i] >= 'a' && out[i] <= 'z') {
            out[i] = (char)(out[i] - 'a' + 'A');
            return 1;
        }
        if (out[i] >= 'A' && out[i] <= 'Z') {
            out[i] = (char)(out[i] - 'A' + 'a');
            return 1;
        }
    }
    return 0;
}

const char* snx_gpu_driver_staged_name(const SnxElfRename* names, size_t count, const char* name) {
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(names[i].from, name)) return names[i].to;
    return name;
}

#ifdef __linux__
/* dir's file name is a library to copy, with the name its copy takes: 1, 0 for a file that is none, -1 with why for
   one whose name has no room */
static int library_of(const char* dir, const char* name, SnxElfRename* out, char* why, size_t why_size) {
    char path[4096], unique[SNX_ELF_NAME_MAX];
    struct stat st;
    size_t n = strlen(name);
    if (name[0] == '.' || n <= 3 || strcmp(name + n - 3, ".so")) return 0;
    if (n >= SNX_ELF_NAME_MAX) {
        snprintf(why, why_size, "the library %s has a name of %zu letters, and %d is the most", name, n,
                 SNX_ELF_NAME_MAX - 1);
        return -1;
    }
    if (!snx_gpu_driver_unique_name(name, unique, sizeof unique)) return 0;
    if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path || stat(path, &st) || !S_ISREG(st.st_mode))
        return 0;
    snprintf(out->from, sizeof out->from, "%s", name);
    snprintf(out->to, sizeof out->to, "%s", unique);
    return 1;
}

/* all of the size bytes at data into fd: 0, else -1 with errno */
static int write_all(int fd, const unsigned char* data, size_t size) {
    for (size_t done = 0; done < size;) {
        ssize_t n = write(fd, data + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            if (!n) errno = EIO;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

int snx_gpu_driver_copy(const char* path, int fd, const SnxElfRename* names, size_t count, struct stat* st, char* why,
                        size_t why_size) {
    char reason[400];
    unsigned char* image;
    int in = open(path, O_RDONLY | O_CLOEXEC), renamed;
    if (in < 0 || fstat(in, st)) {
        snprintf(why, why_size, "the library %s did not open: %s", path, strerror(errno));
        if (in >= 0) close(in);
        return -1;
    }
    if (st->st_size <= 0 || (uintmax_t)st->st_size > SIZE_MAX) {
        snprintf(why, why_size, "the library %s has %lld bytes, which is no library", path, (long long)st->st_size);
        close(in);
        return -1;
    }
    image = (unsigned char*)mmap(NULL, (size_t)st->st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, in, 0);
    if (image == MAP_FAILED) {
        snprintf(why, why_size, "the library %s (%lld bytes) was not mapped: %s", path, (long long)st->st_size,
                 strerror(errno));
        close(in);
        return -1;
    }
    close(in);
    renamed = snx_elf_rename(image, (size_t)st->st_size, names, count, reason, sizeof reason);
    if (renamed < 0)
        snprintf(why, why_size, "the library %s %s", path, reason);
    else if (write_all(fd, image, (size_t)st->st_size)) {
        snprintf(why, why_size, "the library %s (%lld bytes) was not written to its copy: %s", path,
                 (long long)st->st_size, strerror(errno));
        renamed = -1;
    }
    munmap(image, (size_t)st->st_size);
    return renamed;
}

/* the library dir/from copied to to in dir/unique/, its names written over; one already copied left: 1, else 0 with
   why */
static int stage_one(const char* dir, const SnxElfRename* names, size_t count, size_t i, char* why, size_t why_size) {
    char source[4096], target[4096], temp[4096];
    struct stat st, have;
    int out, ok;
    if (snprintf(source, sizeof source, "%s/%s", dir, names[i].from) >= (int)sizeof source ||
        snprintf(target, sizeof target, "%s/%s/%s", dir, SNX_GPU_DRIVER_UNIQUE_DIR, names[i].to) >=
            (int)sizeof target ||
        snprintf(temp, sizeof temp, "%s/%s/.%s.tmp", dir, SNX_GPU_DRIVER_UNIQUE_DIR, names[i].to) >= (int)sizeof temp) {
        snprintf(why, why_size, "the path of the library %s in %s is over %zu letters", names[i].from, dir,
                 sizeof source - 1);
        return 0;
    }
    if (!stat(source, &st) && !stat(target, &have) && have.st_size == st.st_size &&
        have.st_mtim.tv_sec == st.st_mtim.tv_sec && have.st_mtim.tv_nsec == st.st_mtim.tv_nsec)
        return 1;
    /* through another name, so a copy cut short is never taken for the library; with the library's times, which tell
       the next run that this copy is of this library */
    out = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (out < 0) {
        snprintf(why, why_size, "the library %s was not copied to %s: %s", source, target, strerror(errno));
        return 0;
    }
    ok = snx_gpu_driver_copy(source, out, names, count, &st, why, why_size) >= 0;
    if (ok) {
        const struct timespec times[2] = {st.st_atim, st.st_mtim};
        ok = !futimens(out, times) && !fsync(out);
        if (close(out)) ok = 0;
        if (ok && rename(temp, target)) ok = 0;
        if (!ok) snprintf(why, why_size, "the library %s was not copied to %s: %s", source, target, strerror(errno));
    } else
        close(out);
    if (!ok) unlink(temp);
    return ok;
}

int snx_gpu_driver_stage(const char* dir, SnxElfRename** names, size_t* count, char* why, size_t why_size) {
    char unique[4096];
    DIR* d = opendir(dir);
    struct dirent* e;
    SnxElfRename one, *list = NULL;
    size_t n = 0, room = 0;
    int r = 0;
    *names = NULL;
    *count = 0;
    if (!d) {
        snprintf(why, why_size, "the folder %s was not listed: %s", dir, strerror(errno));
        return 0;
    }
    for (;;) {
        errno = 0;
        e = readdir(d);
        if (!e) {
            if (errno) {
                snprintf(why, why_size, "the folder %s was not listed to its end: %s", dir, strerror(errno));
                r = -1;
            }
            break;
        }
        r = library_of(dir, e->d_name, &one, why, why_size);
        if (r < 0) break;
        if (!r) continue;
        if (n == room) {
            SnxElfRename* more = (SnxElfRename*)realloc(list, (room + 16) * sizeof *list);
            if (!more) {
                snprintf(why, why_size, "no memory for the names of %zu libraries", n + 1);
                r = -1;
                break;
            }
            list = more;
            room += 16;
        }
        list[n++] = one;
    }
    closedir(d);
    /* by name, so that a package is copied, and one refused is said, the same way wherever its folder is */
    for (size_t i = 1; r >= 0 && i < n; ++i)
        for (size_t k = i; k > 0 && strcmp(list[k - 1].from, list[k].from) > 0; --k) {
            one = list[k];
            list[k] = list[k - 1];
            list[k - 1] = one;
        }
    /* two libraries apart by that one letter's case alone would be copied over each other */
    for (size_t a = 0; r >= 0 && a < n; ++a)
        for (size_t b = 0; b < n; ++b)
            if (!strcmp(list[a].to, list[b].from)) {
                snprintf(why, why_size, "the libraries %s and %s take each other's name in %s/%s", list[a].from,
                         list[b].from, dir, SNX_GPU_DRIVER_UNIQUE_DIR);
                r = -1;
                break;
            }
    if (r >= 0 && snprintf(unique, sizeof unique, "%s/%s", dir, SNX_GPU_DRIVER_UNIQUE_DIR) >= (int)sizeof unique) {
        snprintf(why, why_size, "the path of %s in the folder %s is over %zu letters", SNX_GPU_DRIVER_UNIQUE_DIR, dir,
                 sizeof unique - 1);
        r = -1;
    }
    if (r >= 0 && mkdir(unique, 0700) && errno != EEXIST) {
        snprintf(why, why_size, "the folder %s was not made: %s", unique, strerror(errno));
        r = -1;
    }
    for (size_t i = 0; r >= 0 && i < n; ++i)
        if (!stage_one(dir, list, n, i, why, why_size)) r = -1;
    if (r < 0) {
        free(list);
        return 0;
    }
    *names = list;
    *count = n;
    return 1;
}
#endif
