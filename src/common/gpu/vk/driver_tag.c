/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The names of each driver's cache files (vk_common.h), apart from the rest of vk_common.c so that a test of the
   files links them without the Vulkan loader */
#include "common/gpu/vk/vk_common.h"
#include "common/utilities/ascii.h"
#include <stdio.h>

void snx_vk_driver_tag(char* out, size_t n, uint32_t driver_id, uint32_t vendor, uint32_t device, const char* package) {
    int w = snprintf(out, n, "d%u-%04x-%08x", (unsigned)driver_id, (unsigned)vendor, (unsigned)device);
    if (w < 0 || (size_t)w >= n || !package || !*package) return;
    size_t at = (size_t)w;
    out[at++] = '-';
    /* a package's folder name as a file name keeps it: any other letter is an underscore */
    for (; *package && at + 1 < n; package++, at++) {
        char c = *package;
        out[at] = snx_is_alnum(c) || c == '.' || c == '_' || c == '-' ? c : '_';
    }
    out[at] = 0;
}
void snx_vk_driver_path(char* out, size_t n, const char* dir, const char* name, const char* tag, const char* ext) {
    snprintf(out, n, "%s/%s.%s%s", dir, name, tag, ext);
}
