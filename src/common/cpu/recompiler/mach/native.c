/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* This host's native back end (native.h): the code generator whose code this machine runs */
#include "common/cpu/recompiler/mach/native.h"
#include "common/cpu/recompiler/arm64/arm64_codegen.h"
#include "common/cpu/recompiler/x64/x64_cache.h"
#include "common/cpu/recompiler/x64/x64_codegen.h"
#include <string.h>

int snx_native_available(void) { return snx_x64_available() || snx_arm64_available(); }

const char* snx_native_name(void) { return snx_x64_available() ? "x86-64" : snx_arm64_available() ? "arm64" : "none"; }

int snx_native_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                      unsigned char* made, unsigned char** object, size_t* size, char* error, size_t error_size) {
    if (snx_x64_available())
        return snx_x64_object(codegen, first, last, options, made, object, size, error, error_size);
    if (snx_arm64_available())
        return snx_arm64_object(codegen, first, last, options, snx_arm64_features(), made, object, size, error,
                                error_size);
    memset(made, 0, last - first);
    *object = NULL;
    *size = 0;
    return 0;
}

int snx_native_cached_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                             unsigned char* made, unsigned char** object, size_t* size, int* reused, char* error,
                             size_t error_size) {
    if (snx_x64_available())
        return snx_x64_cached_object(codegen, first, last, options, made, object, size, reused, error, error_size);
    *reused = 0;
    return snx_native_object(codegen, first, last, options, made, object, size, error, error_size);
}
