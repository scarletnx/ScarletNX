/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VITA_KEYS_H
#define SNX_VITA_KEYS_H
#include <stdbool.h>
#include <stddef.h>

/* The console's keys an encrypted game's import decrypts with, from the user's vita_keys.txt: lines name = hex, # for a
   comment. None of them is in ScarletNX; docs/platforms/vita/README.md says what each is and where it is found. The
   PFS ones open the game's files (sce_pfs/), the NPDRM and SELF ones its eboot.bin and modules once those are open. */
typedef enum {
    VITA_KEY_PFS_HMAC0,     /* pfs_hmac_key0, 20 bytes: a file's sector tweak */
    VITA_KEY_PFS_HMAC1,     /* pfs_hmac_key1, 20 bytes: the files' integrity secret */
    VITA_KEY_PFS_IV0,       /* pfs_iv0, 16 bytes */
    VITA_KEY_PFS_CONTRACT0, /* pfs_contract_key0, 16 bytes: the key the klicensee is turned into the files' key with */
    VITA_KEY_NPDRM0,        /* npdrm_key0, 16 bytes: an app's SELF of key revision 0 or 1 */
    VITA_KEY_NPDRM1,        /* npdrm_key1, 16 bytes: of key revision 2 and up */
    VITA_KEY_NPDRM_IV,      /* npdrm_iv, 16 bytes */
    VITA_KEY_SELF0_169, /* self_app_key0_169 and its iv: an app's SELF of key revision 0 made for system 1.692 or older */
    VITA_KEY_SELF0_169_IV,
    VITA_KEY_SELF0, /* self_app_key0 to self_app_key5 (32 bytes) and their ivs (16): an app's SELF of that key revision */
    VITA_KEY_SELF0_IV,
    VITA_KEY_SELF1,
    VITA_KEY_SELF1_IV,
    VITA_KEY_SELF2,
    VITA_KEY_SELF2_IV,
    VITA_KEY_SELF3,
    VITA_KEY_SELF3_IV,
    VITA_KEY_SELF4,
    VITA_KEY_SELF4_IV,
    VITA_KEY_SELF5,
    VITA_KEY_SELF5_IV,
    VITA_KEY_COUNT
} VitaKeyName;
#define VITA_KEYS_FILE "vita_keys.txt"
typedef struct {
    unsigned char bytes[VITA_KEY_COUNT][32];
    unsigned have;  /* a bit for each VitaKeyName the file gave */
    char path[512]; /* the file they came from, for the messages */
} VitaKeys;

/* The key's name in the file, and its size in bytes */
const char* vita_key_name(VitaKeyName key);
size_t vita_key_size(VitaKeyName key);
/* keys from the file at path: false with error naming the file and the line for a line that is not name = hex, a name
   it does not know, a value of another size or a key given twice. A key the file lacks is only said when it is needed
   (vita_key) */
bool vita_keys_load(const char* path, VitaKeys* keys, char* error, size_t error_size);
/* keys from text, the file's contents, named path in the messages */
bool vita_keys_parse(const char* text, size_t size, const char* path, VitaKeys* keys, char* error, size_t error_size);
/* the key, or NULL with error naming it, what it is for and the file that lacks it */
const unsigned char* vita_key(const VitaKeys* keys, VitaKeyName key, const char* purpose, char* error,
                              size_t error_size);
/* ~/.vita/vita_keys.txt (%USERPROFILE%\.vita on Windows), where Install Keys puts the file and the Vita driver reads
   it, as the 3DS's are in ~/.3ds; NULL with no home. The caller's to free */
char* vita_keys_default_path(void);
#endif
