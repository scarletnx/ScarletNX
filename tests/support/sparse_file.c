/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "sparse_file.h"
#include <errno.h>
#include <stdlib.h>
#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#include <winioctl.h>
#else
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
