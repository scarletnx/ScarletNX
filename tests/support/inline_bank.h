/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VK_INLINE_BANK_H
#define SNX_VK_INLINE_BANK_H
#include <stdint.h>
#include <string.h>

/* Experiment only: CPU bytes identify a bank independently of the front end's shared inline generation. The
   shadow is ordinary CPU memory, never mapped staging. Its caller reserves at least size bytes. */
typedef struct {
    const uint8_t* source;
    uint64_t seen, version;
    uint32_t size, owner;
} VkInlineBankVersion;

static inline uint64_t vk_inline_bank_version(VkInlineBankVersion* entry, uint8_t* shadow, const uint8_t* source,
                                              uint32_t size, uint64_t generation, uint32_t owner) {
    if (entry->source == source && entry->seen == generation && entry->size == size && entry->owner == owner)
        return entry->version;
    if (!entry->version || entry->size != size || memcmp(shadow, source, size)) {
        memcpy(shadow, source, size);
        entry->version++;
        if (!entry->version) entry->version = 1;
    }
    entry->source = source;
    entry->seen = generation;
    entry->size = size;
    entry->owner = owner;
    return entry->version;
}
#endif
