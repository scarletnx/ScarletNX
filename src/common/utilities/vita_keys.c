/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/vita_keys.h"
#include "common/utilities/common.h"
#include <stdlib.h>
#include <string.h>

static const struct {
    const char* file;
    unsigned size;
} key_files[SNX_VITA_KEY_COUNT] = {
    [SNX_VITA_KEY_PFS_HMAC0] = {"pfs-hmac-key-0", 20}, [SNX_VITA_KEY_PFS_HMAC1] = {"pfs-hmac-key-1", 20},
    [SNX_VITA_KEY_PFS_IV0] = {"pfs-iv-0", 16},         [SNX_VITA_KEY_PFS_CONTRACT0] = {"pfs-contract-key-0", 16},
    [SNX_VITA_KEY_NPDRM0] = {"npdrm-key-0", 16},       [SNX_VITA_KEY_NPDRM0_IV] = {"npdrm-iv-0", 16},
    [SNX_VITA_KEY_NPDRM1] = {"npdrm-key-1", 16},       [SNX_VITA_KEY_NPDRM1_IV] = {"npdrm-iv-1", 16},
    [SNX_VITA_KEY_APP0_100] = {"app-key-0-100", 32},   [SNX_VITA_KEY_APP0_100_IV] = {"app-iv-0-100", 16},
    [SNX_VITA_KEY_APP0_180] = {"app-key-0-180", 32},   [SNX_VITA_KEY_APP0_180_IV] = {"app-iv-0-180", 16},
    [SNX_VITA_KEY_APP1] = {"app-key-1", 32},           [SNX_VITA_KEY_APP1_IV] = {"app-iv-1", 16},
    [SNX_VITA_KEY_APP2] = {"app-key-2", 32},           [SNX_VITA_KEY_APP2_IV] = {"app-iv-2", 16},
    [SNX_VITA_KEY_APP3] = {"app-key-3", 32},           [SNX_VITA_KEY_APP3_IV] = {"app-iv-3", 16},
    [SNX_VITA_KEY_APP4] = {"app-key-4", 32},           [SNX_VITA_KEY_APP4_IV] = {"app-iv-4", 16},
    [SNX_VITA_KEY_APP5] = {"app-key-5", 32},           [SNX_VITA_KEY_APP5_IV] = {"app-iv-5", 16},
};

const char* snx_vita_key_file(int key) { return key >= 0 && key < SNX_VITA_KEY_COUNT ? key_files[key].file : NULL; }
size_t snx_vita_key_size(int key) { return key >= 0 && key < SNX_VITA_KEY_COUNT ? key_files[key].size : 0; }
int snx_vita_key_named(const char* name) {
    for (int key = 0; name && key < SNX_VITA_KEY_COUNT; ++key)
        if (!strcmp(key_files[key].file, name)) return key;
    return -1;
}
char* snx_vita_keys_dir(void) {
#ifdef _WIN32
    const char* home = getenv("USERPROFILE");
    if (!home || !home[0]) home = getenv("HOME");
#else
    const char* home = getenv("HOME");
#endif
    return home && home[0] ? snx_path_join(home, ".vita") : NULL;
}
