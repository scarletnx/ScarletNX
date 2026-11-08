/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The host side of the indexed filesystem: folders, UTF-8 paths on Windows, and files shared read-only with
   the host's page cache for the whole run */
#include "common/filesystem/host.h"
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

/* descriptors a shared file may not take: they stay for the files opened on demand (saves, the SD card, unmapped
   RomFS files) */
#define SHARE_RESERVE 256

#if defined(_WIN32)
/* paths are UTF-8 everywhere else. A path that isn't valid UTF-8 names nothing: NULL, rather than a path with
   replacement characters that could name another file. Freed with free */
wchar_t* snx_host_wpath(const char* text) {
    wchar_t* wide;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (n <= 0) return NULL;
    wide = malloc((size_t)n * sizeof *wide);
    if (!wide) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, n) != n) {
        free(wide);
        return NULL;
    }
    return wide;
}

int plat_mkdir(const char* path) {
    wchar_t* wide = snx_host_wpath(path);
    int ok = wide && CreateDirectoryW(wide, NULL);
    free(wide);
    return ok ? 0 : -1;
}

/* the CRT has room for 8192 descriptors, handed out lowest first */
#define CRT_DESCRIPTORS 8192
int plat_file_share(const char* path, uint64_t size, int* fd, const uint8_t** data) {
    HANDLE file, section;
    void* view = NULL;
    *fd = -1;
    *data = NULL;
    if ((uint64_t)(size_t)size != size || size > (uint64_t)INT64_MAX) return -1;
    wchar_t* wide = snx_host_wpath(path);
    if (!wide) return -1;
    /* others may read it, and delete or rename it as on POSIX: the view stays valid */
    file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    free(wide);
    if (file == INVALID_HANDLE_VALUE) return -1;
    int d = _open_osfhandle((intptr_t)file, _O_RDONLY | _O_BINARY);
    if (d < 0) {
        CloseHandle(file);
        return -1;
    }
    if (d + 1 + SHARE_RESERVE > CRT_DESCRIPTORS) {
        _close(d); /* closes the handle with it */
        return -1;
    }
    if (size) {
        section = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
        if (section) {
            view = MapViewOfFile(section, FILE_MAP_READ, 0, 0, (SIZE_T)size);
            CloseHandle(section); /* the view keeps it */
        }
        if (!view) {
            _close(d);
            return -1;
        }
    }
    *fd = d;
    *data = view;
    return 0;
}
void plat_file_unshare(int fd, const uint8_t* data, uint64_t size) {
    (void)size;
    if (data) UnmapViewOfFile(data);
    if (fd >= 0) _close(fd);
}
#else
int plat_mkdir(const char* path) { return mkdir(path, 0777) ? -1 : 0; }

/* the descriptors the shared files may use: the soft limit, and on macOS no more than OPEN_MAX (10240). Its hard limit is
   "unlimited" and kern.maxfilesperproc (61440) is what stops a process, but a game with 300000 files that mapped up to
   that left SDL unable to open its Vulkan window, with the descriptors still free */
static rlim_t descriptor_limit(const struct rlimit* rl) {
    rlim_t limit = rl->rlim_cur;
#if defined(__APPLE__)
    if (limit > 10240) limit = 10240;
#endif
    return limit;
}
/* a game's RomFS can have more files than the default soft limit of descriptors: raise it to the hard limit, once.
   If that fails the old limit stays, and the reserve check below refuses what doesn't fit */
static void raise_descriptor_limit(void) {
    static int done;
    struct rlimit rl;
    if (__atomic_exchange_n(&done, 1, __ATOMIC_ACQ_REL)) return;
    if (getrlimit(RLIMIT_NOFILE, &rl) || rl.rlim_cur == rl.rlim_max) return;
    rl.rlim_cur = rl.rlim_max;
    (void)setrlimit(RLIMIT_NOFILE, &rl);
}
/* whether keeping descriptor fd leaves the reserve free (descriptors are handed out lowest first) */
static int descriptor_fits(int fd) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl)) return 1;
    rlim_t limit = descriptor_limit(&rl);
    if (limit == RLIM_INFINITY) return 1;
    return (rlim_t)fd + 1 + SHARE_RESERVE <= limit;
}
int plat_file_share(const char* path, uint64_t size, int* fd, const uint8_t** data) {
    void* view = NULL;
    *fd = -1;
    *data = NULL;
    if ((uint64_t)(size_t)size != size || size > (uint64_t)INT64_MAX) return -1;
    raise_descriptor_limit();
    int d = open(path, O_RDONLY | O_CLOEXEC);
    if (d < 0) return -1;
    if (!descriptor_fits(d)) {
        close(d);
        return -1;
    }
    if (size) { /* paged in as the game reads it */
        view = mmap(NULL, (size_t)size, PROT_READ, MAP_SHARED, d, 0);
        if (view == MAP_FAILED) {
            close(d);
            return -1;
        }
    }
    *fd = d;
    *data = view;
    return 0;
}
void plat_file_unshare(int fd, const uint8_t* data, uint64_t size) {
    if (data) munmap((void*)data, (size_t)size);
    if (fd >= 0) close(fd);
}
#endif
