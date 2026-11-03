/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The native bridge's entry points (native.h) */
#include "android/bridge/native.h"
#include "android/bridge/fault.h"
#include "android/bridge/bridge.h"
#include "android/bridge/hle.h"
#include "android/bridge/linker.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool android_native_start(const char* library_dir, char* error, size_t error_size) {
    /* compiled code is kept in the user's cache, by a hash of the code */
    char* cache_root = snx_user_dir(SNX_USER_CACHE);
    char* cache = cache_root ? snx_path_join(cache_root, "android-jit") : NULL;
    free(cache_root);
    bool ok = bridge_init(cache, error, error_size);
    free(cache);
    if (!ok) return false;
    hle_libc_init();
    hle_code_init();
    if (library_dir) linker_set_library_dir(library_dir);
    return true;
}

int android_native_test(const char* library, const char* const* symbols, unsigned count) {
    char error[1024];
    char* dir = snx_dirname(library);
    if (!android_native_start(dir, error, sizeof error)) {
        fprintf(stderr, "scarletnx-android: %s\n", error);
        free(dir);
        return 1;
    }
    free(dir);
    fault_install();
    GuestLibrary* l = linker_load(library, error, sizeof error);
    if (!l) {
        fprintf(stderr, "scarletnx-android: dlopen failed: %s\n", error);
        return 1;
    }
    printf("loaded %s\n", linker_name(l));
    for (unsigned i = 0; i < count; ++i) {
        uint64_t f = linker_symbol(l, symbols[i]);
        if (!f) {
            printf("%s: no such symbol\n", symbols[i]);
            continue;
        }
        uint64_t result = bridge_call(f, NULL, 0);
        printf("%s() = 0x%llx\n", symbols[i], (unsigned long long)result);
    }
    return 0;
}
