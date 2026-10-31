/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "sparse_file.h"
#include "common/utilities/common.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
static void die_windows(const char* what, DWORD error) {
    fprintf(stderr, "%s: Windows error %u\n", what, (unsigned)error);
    abort();
}
#endif
static void die(const char* what) {
    perror(what);
    abort();
}

bool test_sparse_resize(FILE* f, uint64_t size) {
#if defined(_WIN32)
    HANDLE handle = (HANDLE)_get_osfhandle(_fileno(f));
    if (handle == INVALID_HANDLE_VALUE) die("sparse fixture handle");
    DWORD returned = 0;
    if (!DeviceIoControl(handle, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &returned, NULL)) {
        DWORD error = GetLastError();
        if (error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED) return false;
        die_windows("cannot mark sparse fixture", error);
    }
    /* _chsize_s would write the extension as zero blocks, so a 4 GiB fixture would really occupy 4 GiB */
    LARGE_INTEGER end = {.QuadPart = (LONGLONG)size}, here = {.QuadPart = 0};
    /* a zero move from FILE_CURRENT saves the current position in here, and the last call moves back to it */
    if (fflush(f) || !SetFilePointerEx(handle, here, &here, FILE_CURRENT) ||
        !SetFilePointerEx(handle, end, NULL, FILE_BEGIN) || !SetEndOfFile(handle) ||
        !SetFilePointerEx(handle, here, NULL, FILE_BEGIN))
        die_windows("cannot resize sparse fixture", GetLastError());
#else
    if (ftruncate(fileno(f), (off_t)size)) {
        if (errno == ENOSYS || errno == EOPNOTSUPP) return false;
        die("cannot resize sparse fixture");
    }
#endif
    return true;
}

void test_seek_at(FILE* f, uint64_t at) {
#if defined(_WIN32)
    assert(!_fseeki64(f, (long long)at, SEEK_SET));
#else
    assert(!fseeko(f, (off_t)at, SEEK_SET));
#endif
}

bool test_map_open(TestMap* map, const char* path, uint64_t size) {
    *map = (TestMap){0};
    if (size > SIZE_MAX) return false;
#if defined(_WIN32)
    FILE* f = snx_fopen(path, "rb");
    if (!f) die("map fixture");
    HANDLE file = (HANDLE)_get_osfhandle(_fileno(f));
    if (file == INVALID_HANDLE_VALUE) die("map fixture handle");
    HANDLE mapping = CreateFileMappingW(file, NULL, PAGE_READONLY, (DWORD)(size >> 32), (DWORD)size, NULL);
    if (!mapping) {
        DWORD error = GetLastError();
        fclose(f);
        if (error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED) return false;
        die_windows("cannot map sparse fixture", error);
    }
    void* base = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, (SIZE_T)size);
    DWORD error = GetLastError();
    CloseHandle(mapping);
    fclose(f);
    if (!base) die_windows("cannot map sparse fixture", error);
#else
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("map fixture");
    void* base = mmap(NULL, (size_t)size, PROT_READ, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        if (errno == ENOSYS || errno == EOPNOTSUPP) {
            close(fd);
            return false;
        }
        die("cannot map sparse fixture");
    }
    close(fd);
#endif
    *map = (TestMap){(const unsigned char*)base, (size_t)size};
    return true;
}

void test_map_close(TestMap* map) {
    if (!map->base) return;
#if defined(_WIN32)
    if (!UnmapViewOfFile(map->base)) die_windows("cannot unmap sparse fixture", GetLastError());
#else
    if (munmap((void*)map->base, map->size)) die("cannot unmap sparse fixture");
#endif
    *map = (TestMap){0};
}
