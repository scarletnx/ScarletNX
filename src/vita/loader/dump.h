/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VITA_DUMP_H
#define SNX_VITA_DUMP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* An encrypted dump of a game, as a console's memory card holds it and NoNpDrm leaves its licenses beside it:
   <dump>/app/<title ID>/ (sce_pfs/ and sce_sys/, its license sce_sys/package/work.bin), its update
   <dump>/patch/<title ID>/, its add-ons <dump>/addcont/<title ID>/<add-on ID>/, each one's license
   <dump>/license/addcont/<title ID>/<add-on ID>/<name>.rif. The import decrypts them into the game's folder as Vita3K
   installs a dump (interface.cpp's install_content, pkg.cpp's decrypt_install_nonpdrm): app/ from the game, patch/ from
   the update with the game's license, ux0/addcont/<title ID>/<add-on ID>/ from each add-on with its own; then the
   game's and the update's SELFs (vita/loader/selfcrypt.h). Each is decrypted into a folder beside it and renamed into
   place once whole. The dump is only read: its licenses are not copied */

/* each line the import says, error true for an add-on it could not import (the game's own failure is its result) */
typedef void (*VitaDumpSay)(void* context, const char* line, bool error);
typedef struct {
    const char* app;    /* the game's folder in the dump, <dump>/app/<title ID> */
    const char* patch;  /* its update's folder, NULL for none */
    const char* title;  /* its title ID */
    const char* folder; /* the game's folder, which app/, patch/ and ux0/addcont/ go into */
    const char* keys;   /* the Vita keys: their folder or its vita.keys (common/utilities/vita_keys.h) */
    const char* zrif;   /* the game's license as a zRIF, for a dump without one; NULL for none */
    VitaDumpSay say;
    /* how far the content being decrypted went (its files' bytes), each content from 0 after its "Decrypting" line */
    void (*progress)(void* context, uint64_t done, uint64_t total);
    void* context;
} VitaDumpImport;

/* whether the dumped game's folder is encrypted: sce_pfs/files.db in it */
bool vita_dump_encrypted(const char* app);
/* about what the import writes: the bytes of the files of app, of update (NULL for none) and of the dump's add-ons of
   title, their sce_pfs/ and sce_sys/package/ left out; *addons the add-ons counted */
uint64_t vita_dump_bytes(const char* app, const char* update, const char* title, unsigned* addons);
/* the import: false with error saying why the game (or its update) could not be. An app/ already decrypted there (a
   folder, no sce_pfs/ in it) is kept, and an add-on already there; a link app/ (an earlier import that linked the
   dump) or an encrypted copy is replaced */
bool vita_dump_import(const VitaDumpImport* d, char* error, size_t error_size);
#endif
