/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VITA_KEYS_H
#define SNX_VITA_KEYS_H
#include "common/utilities/vita_keys.h"
#include <stdbool.h>
#include <stddef.h>

/* The keys an encrypted dump's import decrypts with, read from the user's (common/utilities/vita_keys.h): a folder,
   ~/.vita by default, its vita.keys and its key files, or a vita.keys given itself */
typedef struct {
    unsigned char bytes[SNX_VITA_KEY_COUNT][32];
    unsigned have;  /* a bit for each SnxVitaKey given */
    char path[512]; /* the folder or the file, for the messages */
} VitaKeys;
static_assert(SNX_VITA_KEY_COUNT <= 32, "VitaKeys.have holds a bit for each key");
/* told a line about the keys that stops nothing: a name in vita.keys that is no Vita key, a key vita.keys and its file
   give differently (vita.keys's taken) */
typedef void (*VitaKeysNote)(void* context, const char* line);

/* the keys at path: a folder's key files, then its vita.keys, whose lines win; or the vita.keys file path names. False
   with error for nothing there, a key file or line of another size than its key's (both sizes said), a line that is
   not name = hex or a key given twice. A key none of them gives is only said when it is needed (vita_key) */
bool vita_keys_load(const char* path, VitaKeys* keys, VitaKeysNote note, void* context, char* error, size_t error_size);
/* the key, or NULL with error naming it, its size, what needs it and where it was looked for */
const unsigned char* vita_key(const VitaKeys* keys, SnxVitaKey key, const char* purpose, char* error,
                              size_t error_size);
#endif
