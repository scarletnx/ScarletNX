/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* UTF-8 paths on Windows (file.h): a file of its own, so a test that stands in for file.c's calls doesn't link file.c
   with every platform file that opens a path */
#include "common/platform/file.h"
#include <stdlib.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

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
