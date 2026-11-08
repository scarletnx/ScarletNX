/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The replace of a file or folder by another one beside it (file.h) */
#include "common/platform/file.h"
#include <stdlib.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

/* src is beside dst, so it is never a copy across volumes. src stays on failure: a caller that moves a folder of saves
   must not lose it, and one that wrote a temporary file removes it itself */
int plat_replace_file(const char* src, const char* dst) {
#if defined(_WIN32)
    wchar_t *a = snx_host_wpath(src), *b = snx_host_wpath(dst);
    int ok = a && b && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    free(a);
    free(b);
    return ok ? 0 : -1;
#else
    return rename(src, dst) ? -1 : 0;
#endif
}
