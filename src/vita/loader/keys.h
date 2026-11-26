/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VITA_KEYS_H
#define SNX_VITA_KEYS_H
#include "common/utilities/vita_keys.h"
#include <stdbool.h>
#include <stddef.h>

/* The keys an encrypted dump's import decrypts with, read from the user's folder of them (common/utilities/vita_keys.h:
   a file for each, ~/.vita by default) */
typedef struct {
    unsigned char bytes[SNX_VITA_KEY_COUNT][32];
    unsigned have;  /* a bit for each SnxVitaKey the folder has */
    char path[512]; /* the folder, for the messages */
} VitaKeys;
static_assert(SNX_VITA_KEY_COUNT <= 32, "VitaKeys.have holds a bit for each key");

/* the keys of the folder at path: false with error naming the folder when it is not there, or a key's file and its size
   when it has another size than the key's. A key the folder lacks is only said when it is needed (vita_key) */
bool vita_keys_load(const char* path, VitaKeys* keys, char* error, size_t error_size);
/* the key, or NULL with error naming its file, its size, what needs it and the folder that lacks it */
const unsigned char* vita_key(const VitaKeys* keys, SnxVitaKey key, const char* purpose, char* error,
                              size_t error_size);
#endif
