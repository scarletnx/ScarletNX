/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VITA_PFS_H
#define SNX_VITA_PFS_H
#include "vita/loader/keys.h"
#include <stdbool.h>
#include <stdint.h>

/* A dumped game's or add-on's folder as the console keeps it (ux0:app/<title ID>, ux0:patch/<title ID>,
   ux0:addcont/<title ID>/<its ID>): its files encrypted under its license's klicensee, sce_pfs/files.db the tree of
   their names, types and sizes, sce_pfs/unicv.db each file's table of 32 KiB sectors, an HMAC-SHA1 of each. The scheme
   is the one psvpfsparser reads (motoharu-gosuto's psvpfstools, Vita3K's copy in its external/): files.db's B-tree of
   1 KiB pages, an entry's index the page of its file's table in unicv.db, the files' key the klicensee decrypted with
   pfs_contract_key0, a sector AES-128-CBC from an IV of its offset XORed with a mask HMAC-SHA1 of the table's page
   under pfs_hmac_key0, its last partial block XORed with the encrypted last whole one; the integrity secret an AES-CBC
   of an HMAC-SHA1 of the page under pfs_hmac_key1, from pfs_iv0. Nothing of psvpfsparser's code is here: it carries no
   license */

/* What a decryption did, for the import's lines */
typedef struct {
    unsigned decrypted, copied, empty, folders, left_out; /* files and folders, and those of the dump not in files.db */
    uint64_t bytes;
} VitaPfsCount;
/* told each file of the dump that files.db does not list, which the decryption leaves out, by its path in source */
typedef void (*VitaPfsNote)(void* context, const char* path);
/* told how far the decryption went: the bytes of files.db's files done of all of them, every 8 MiB and at the end */
typedef void (*VitaPfsProgress)(void* context, uint64_t done, uint64_t total);

/* whether folder holds an encrypted content: its sce_pfs/files.db */
bool vita_pfs_present(const char* folder);
/* source's files decrypted into dest (made; its files and folders by their names in the dump), every sector checked
   against its signature first, files.db's pages and header against theirs. False with error naming the file, the
   sector or the page, and why: the klicensee another content's (files.db's header does not verify), a key keys lacks,
   a file files.db lists that the dump does not have, a sector changed. dest may then hold some of the files */
bool vita_pfs_decrypt(const char* source, const char* dest, const unsigned char klicensee[16], const VitaKeys* keys,
                      VitaPfsNote note, VitaPfsProgress progress, void* context, VitaPfsCount* count, char* error,
                      size_t error_size);
#endif
