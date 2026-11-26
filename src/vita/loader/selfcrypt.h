/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VITA_SELFCRYPT_H
#define SNX_VITA_SELFCRYPT_H
#include "vita/loader/keys.h"
#include <stdbool.h>

/* A game's eboot.bin and modules stay SELFs whose segments are encrypted once its files are out of its PFS. As Vita3K
   reads one (packages/src/sce_utils.cpp, decrypt_fself and get_segments, after TeamMolecule's sceutils): the
   klicensee decrypted with the NPDRM key of the SELF's key revision opens the metadata's key block, which the app
   SELF key of that revision and system version opens again; the metadata it holds gives each segment's AES-128-CTR key
   and counter. The file is rewritten in place, each segment decrypted and marked plain (encryption 2), which the
   loader reads (vita/loader/image.c): the SELF's layout, its headers and compressed segments stay as they are */

/* 1 when the SELF at path was decrypted, 0 when it was plain already, -1 with error naming the file (shown, what the
   messages call it: its path in the dump), the key or the
   field: not a SELF, a SELF of another kind than an app's, a key keys lacks, metadata that does not decrypt (another
   content's klicensee) */
int vita_self_decrypt(const char* path, const char* shown, const unsigned char klicensee[16], const VitaKeys* keys,
                      char* error, size_t error_size);
/* whether name is one a SELF has in a game: eboot.bin, *.suprx, *.self, *.skprx (Vita3K's decrypt_selfs) */
bool vita_self_name(const char* name);
#endif
