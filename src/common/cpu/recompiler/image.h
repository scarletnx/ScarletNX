/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CODE_IMAGE_H
#define SNX_CODE_IMAGE_H
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include <stdatomic.h>
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
    /* the functions of the module whose calls go through their dispatch slot, where a hook of the runtime's can take
       them (nx/nn/hook.h), sorted, as offsets like the functions'. The NX loader puts here the functions whose
       dynamic symbol a hook names (nx/loader/hook_routes.h); every other call is a direct call */
    const SnxU64* routed;
    size_t routed_count;
    const SnxFde* fde;
    size_t fde_count;
    int atomic_words; /* image words may be updated while JIT workers read them */
    const _Atomic uint64_t* generation;
} SnxCodeImage;

static inline uint32_t snx_code_image_word(const SnxCodeImage* image, size_t offset) {
    if (image->atomic_words) {
        uint32_t word = atomic_load_explicit((const _Atomic uint32_t*)(const void*)(image->image + offset),
                                             memory_order_seq_cst);
        return snx_ld_le32(&word);
    }
    return snx_ld_le32(image->image + offset);
}
#endif
