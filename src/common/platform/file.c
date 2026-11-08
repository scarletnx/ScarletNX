/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Reads at an offset, folders, and UTF-8 paths on Windows (file.h) */
#include "common/platform/file.h"
#include <errno.h>
#include <stdlib.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
/* paths are UTF-8 everywhere else. A path that isn't valid UTF-8 names nothing: NULL, rather than a path with
   replacement characters that could name another file. Freed with free */
wchar_t* snx_host_wpath(const char* text) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t* wide = malloc((size_t)n * sizeof *wide);
    if (!wide) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, n) != n) {
        free(wide);
        return NULL;
    }
    return wide;
}
#endif

int64_t plat_pread_fd(int fd, void* buf, uint64_t size, uint64_t off) {
    if (size > (uint64_t)INT64_MAX || off > (uint64_t)INT64_MAX - size || (uint64_t)(size_t)size != size) {
        errno = EINVAL;
        return -1;
    }
    uint64_t got = 0;
#if defined(_WIN32)
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    /* ReadFile of 1 GiB at most at a time, at the OVERLAPPED's offset */
    while (got < size) {
        OVERLAPPED o = {0};
        DWORD want = size - got > (1u << 30) ? (1u << 30) : (DWORD)(size - got), n = 0;
        uint64_t at = off + got;
        o.Offset = (DWORD)at;
        o.OffsetHigh = (DWORD)(at >> 32);
        if (!ReadFile(h, (char*)buf + got, want, &n, &o)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            errno = EIO;
            return -1;
        }
        if (!n) break;
        got += n;
    }
#else
    /* pread may return less than asked (past 2 GiB on Linux, or interrupted): go on until done or at the end */
    while (got < size) {
        ssize_t r = pread(fd, (char*)buf + got, (size_t)(size - got), (off_t)(off + got));
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break;
        got += (uint64_t)r;
    }
#endif
    return (int64_t)got;
}
int64_t plat_pread(FILE* f, void* buf, uint64_t size, uint64_t off) {
#if defined(_WIN32)
    return plat_pread_fd(_fileno(f), buf, size, off);
#else
    return plat_pread_fd(fileno(f), buf, size, off);
#endif
}

#if defined(_WIN32)
int plat_is_dir(const char* path) {
    wchar_t* wide = snx_host_wpath(path);
    DWORD a = wide ? GetFileAttributesW(wide) : INVALID_FILE_ATTRIBUTES;
    free(wide);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
int plat_mkdir(const char* path) {
    wchar_t* wide = snx_host_wpath(path);
    int ok = wide && CreateDirectoryW(wide, NULL);
    free(wide);
    return ok ? 0 : -1;
}
#else
int plat_is_dir(const char* path) {
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}
int plat_mkdir(const char* path) { return mkdir(path, 0777) ? -1 : 0; }
#endif
