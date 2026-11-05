/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_TEST_3DS_VK_PATH_H
#define SNX_TEST_3DS_VK_PATH_H
/* Where a 3DS GPU test's work reads and writes the RAM (gpu/vk/vk.h's snx_ctr_vk_path), so the suite runs each path
   on any device: SNX_TEST_CTR_VK_PATH is "copy" or "in-place"; unset or empty, the device's own choice. */
#include "3ds/gpu/vk/vk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the path asked for, before the device starts; any other value ends the test, status 2 */
static inline SnxCtrVkPath test_ctr_vk_path(void) {
    const char* asked = getenv("SNX_TEST_CTR_VK_PATH");
    SnxCtrVkPath path = SNX_CTR_VK_AUTO;
    if (asked && !strcmp(asked, "copy"))
        path = SNX_CTR_VK_COPY;
    else if (asked && !strcmp(asked, "in-place"))
        path = SNX_CTR_VK_IN_PLACE;
    else if (asked && *asked) {
        fprintf(stderr, "SNX_TEST_CTR_VK_PATH=%s is neither copy nor in-place\n", asked);
        exit(2);
    }
    snx_ctr_vk_path(path);
    return path;
}
/* once the device started: the path asked for is the one it took, else the test ends, status 1 */
static inline void test_ctr_vk_path_taken(SnxCtrVkPath path) {
    if (path == SNX_CTR_VK_AUTO || snx_ctr_vk_copies() == (path == SNX_CTR_VK_COPY)) return;
    fprintf(stderr, "the device's work is %s, where SNX_TEST_CTR_VK_PATH asked for %s\n",
            snx_ctr_vk_copies() ? "on a copy of the RAM" : "on the RAM in place",
            path == SNX_CTR_VK_COPY ? "copy" : "in-place");
    exit(1);
}
#endif
