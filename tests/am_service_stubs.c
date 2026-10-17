/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* the game's data folder for am.c's tests: a control.nacp only when a test gives one */
#include "nx/hle/filesystem/fs.h"
#include <stdlib.h>
#include <string.h>

extern const u8* g_test_nacp;
extern u64 g_test_nacp_size;
const u8* g_test_nacp;
u64 g_test_nacp_size;

u8* fs_read_file(const char* path, u64* size) {
    u8* d;
    if (size) *size = 0;
    if (strcmp(path, "control.nacp") || !g_test_nacp) return NULL;
    d = malloc((size_t)g_test_nacp_size);
    if (!d) return NULL;
    memcpy(d, g_test_nacp, (size_t)g_test_nacp_size);
    if (size) *size = g_test_nacp_size;
    return d;
}
