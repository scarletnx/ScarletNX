/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The JIT cache's key (jit_cache.h): the native back end makes every unit (jit_service.c), so the key names it, the
   version of its code and the host's features it makes the code for, beside the ABI, the runtime library, the decoder,
   the guest's features and the settings */
#include "common/cpu/recompiler/jit/jit_cache.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/recompiler/mach/native.h"
#include "common/cpu/recompiler/rtlib/rtlib_object.h"
#include "common/utilities/bits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* snx_jit_cache_key(char opt, int hook, int svc, SnxU64 guest) {
    const char* native = snx_native_name();
    const size_t n = strlen(native) + 256;
    char* key = (char*)malloc(n);
    if (!key) return NULL;
    const SnxU64 rtlib = snx_fnv1a(SNX_FNV_BASIS, snx_rtlib_object, (size_t)snx_rtlib_object_size);
    /* opt= and hook= as jit_cache_key.c has them: the launcher reads them (gui/aot.c) */
    snprintf(
        key, n,
        "abi=%d native=%s rtlib=%016llx native_code=%s dec=%016llx a64=%016llx opt=%c hook=%d svc=%d features=%08x",
        SNX_ABI, native, (unsigned long long)rtlib, snx_native_code(), (unsigned long long)snx_a64_decoder_hash(),
        (unsigned long long)snx_a64_features(guest), opt, hook, svc, snx_native_features());
    return key;
}
