/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Whole files mapped read-only (file.h) */
#include "common/platform/file.h"
#include <stdlib.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
/* ask_size: the size comes from the file; else size is trusted */
static int file_map(PFileMap* m, const char* path, int ask_size, uint64_t size) {
    *m = (PFileMap){0};
    wchar_t* w = snx_host_wpath(path); /* NULL for a path that isn't UTF-8 */
    if (!w) return -1;
    /* a folder doesn't open without FILE_FLAG_BACKUP_SEMANTICS, so this is a file. Others may read it, or delete or
       rename it as on POSIX */
    HANDLE file = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (file == INVALID_HANDLE_VALUE) return -1;
    if (ask_size) {
        LARGE_INTEGER s;
        if (!GetFileSizeEx(file, &s) || s.QuadPart < 0) {
            CloseHandle(file);
            return -1;
        }
        size = (uint64_t)s.QuadPart;
    }
    if (size > (uint64_t)SIZE_MAX || size > (uint64_t)INT64_MAX) {
        CloseHandle(file);
        return -1;
    }
    if (!size) {
        CloseHandle(file);
        return 0;
    }
    HANDLE section = CreateFileMappingW(file, NULL, PAGE_READONLY, (DWORD)(size >> 32), (DWORD)size, NULL);
    void* view = section ? MapViewOfFile(section, FILE_MAP_READ, 0, 0, (SIZE_T)size) : NULL;
    if (section) CloseHandle(section); /* the view keeps the section and the file */
    CloseHandle(file);
    if (!view) return -1;
    m->data = (const uint8_t*)view;
    m->size = size;
    return 0;
}
static void file_unmap(const PFileMap* m) { UnmapViewOfFile(m->data); }
#else
/* stat_it: the size comes from fstat, and the file must be a regular one; else known is trusted */
static int file_map(PFileMap* m, const char* path, int stat_it, uint64_t known) {
    *m = (PFileMap){0};
    uint64_t size = known;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    if (stat_it) {
        struct stat st;
        if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0) {
            close(fd);
            return -1;
        }
        size = (uint64_t)st.st_size;
    }
    if (size > (uint64_t)SIZE_MAX || size > (uint64_t)INT64_MAX) {
        close(fd);
        return -1;
    }
    if (!size) {
        close(fd);
        return 0;
    }
    void* p = mmap(NULL, (size_t)size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd); /* the mapping keeps the file */
    if (p == MAP_FAILED) return -1;
    m->data = (const uint8_t*)p;
    m->size = size;
    return 0;
}
static void file_unmap(const PFileMap* m) { munmap((void*)m->data, (size_t)m->size); }
#endif

int plat_file_map(PFileMap* m, const char* path) { return file_map(m, path, 1, 0); }
int plat_file_map_sized(PFileMap* m, const char* path, uint64_t size) { return file_map(m, path, 0, size); }
void plat_file_unmap(PFileMap* m) {
    if (m->heap)
        free((void*)m->data);
    else if (m->data && !m->borrowed)
        file_unmap(m);
    *m = (PFileMap){0};
}
