/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VITA_KEYS_FILES_H
#define SNX_VITA_KEYS_FILES_H
#include <stddef.h>

/* The console's keys an encrypted Vita dump's import decrypts with (vita/loader/dump.h), as the user keeps them: a file
   for each key, its raw bytes, in ~/.vita (%USERPROFILE%\.vita on Windows), the folder and the naming vitakeys and
   vitatool use (<what>-key-<n>, <what>-iv-<n>). None of them is in ScarletNX; docs/platforms/vita/README.md says what
   each is and where it is published. The PFS ones open the game's files, the NPDRM and app ones its SELFs */
typedef enum {
    SNX_VITA_KEY_PFS_HMAC0,     /* pfs-hmac-key-0, 20 bytes: each file's sector masks */
    SNX_VITA_KEY_PFS_HMAC1,     /* pfs-hmac-key-1, 20 bytes: the files' integrity secret */
    SNX_VITA_KEY_PFS_IV0,       /* pfs-iv-0, 16 bytes: the same */
    SNX_VITA_KEY_PFS_CONTRACT0, /* pfs-contract-key-0, 16 bytes: the files' key, from the klicensee */
    SNX_VITA_KEY_NPDRM0,        /* npdrm-key-0 and npdrm-iv-0: an app SELF of key revision 0 or 1 */
    SNX_VITA_KEY_NPDRM0_IV,
    SNX_VITA_KEY_NPDRM1, /* npdrm-key-1 and npdrm-iv-1: of key revision 2 and up */
    SNX_VITA_KEY_NPDRM1_IV,
    SNX_VITA_KEY_APP0_100, /* app-key-0-100 (32 bytes), app-iv-0-100: an app SELF of revision 0 for system 1.692 or older */
    SNX_VITA_KEY_APP0_100_IV,
    SNX_VITA_KEY_APP0_180, /* app-key-0-180, app-iv-0-180: of revision 0 for system 1.80 and up */
    SNX_VITA_KEY_APP0_180_IV,
    SNX_VITA_KEY_APP1, /* app-key-1 to app-key-5 (32 bytes) and app-iv-1 to app-iv-5: of that key revision */
    SNX_VITA_KEY_APP1_IV,
    SNX_VITA_KEY_APP2,
    SNX_VITA_KEY_APP2_IV,
    SNX_VITA_KEY_APP3,
    SNX_VITA_KEY_APP3_IV,
    SNX_VITA_KEY_APP4,
    SNX_VITA_KEY_APP4_IV,
    SNX_VITA_KEY_APP5,
    SNX_VITA_KEY_APP5_IV,
    SNX_VITA_KEY_COUNT
} SnxVitaKey;

/* the key's file name in the folder, and its size in bytes; NULL and 0 past the last */
const char* snx_vita_key_file(int key);
size_t snx_vita_key_size(int key);
/* the key whose file is named name, -1 for none */
int snx_vita_key_named(const char* name);
/* ~/.vita (%USERPROFILE%\.vita on Windows), where Install Keys puts the files and the Vita driver reads them; NULL with
   no home. The caller's to free */
char* snx_vita_keys_dir(void);
#endif
