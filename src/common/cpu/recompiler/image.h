/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CODE_IMAGE_H
#define SNX_CODE_IMAGE_H
#include "common/utilities/common.h"
typedef struct {
    SnxU64 start, size;
} SnxFde;
/* Borrowed executable bytes and optional discovery hints. No file-format metadata. */
typedef struct {
    const unsigned char* image;
    size_t image_size;
    SnxU64 base, text_off, text_size;
    const SnxU64 *functions, *relptrs;
    size_t function_count, relptr_count;
    /* the functions the module exports (its dynamic symbols, sorted, as offsets like the functions'): calls to them
       go through their dispatch slot, where a hook of the runtime's can take them (nx/hle/hook/hook.h) */
    const SnxU64* exported;
    size_t exported_count;
    const SnxFde* fde;
    size_t fde_count;
} SnxCodeImage;
#endif
