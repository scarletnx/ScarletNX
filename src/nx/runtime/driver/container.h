/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A Switch dump (XCI or NSP, or their decrypted DXCI and DNSP) opened with hactool in this process, its keys loaded
   and its NCAs picked as -x extracts them: what the extraction, the recompiler (a dump given to -r) and the runtime (a
   game whose dump is not extracted, read in place) share */
#ifndef SNX_CONTAINER_H
#define SNX_CONTAINER_H

#include "common/utilities/common.h"
#include "nx/runtime/driver/hactool/snx_hac.h"

/* an NCA where it lies: a container's number (snx_hac_open) and its entry, found 0 for none */
typedef struct {
    int found, container;
    size_t entry;
    SnxU64 size, title_id;
    SnxHacNca info; /* what its header says */
} SnxNcaRef;

typedef struct {
    SnxHac* hac;
    char *keys, *title_keys; /* where they were looked for */
    int has_keys;
    int say;          /* each container, its NCAs and content metas described as they are read, once (a bit a
                         container in said and said_meta) */
    unsigned said, said_meta;
    SnxU64 game;      /* the game the content picked is of: the base's own ID, or the one an add-on's content meta
                         names (else the usual numbering's, snx_addon_guess) */
    SnxU64 version;   /* the picked content's version, its content meta's: SNX_NO_VERSION without one */
    int base, update; /* the containers, either -1 without one; update is the add-on's (an update or a DLC) */
    /* the base's Program NCA, the update's (the patch an NSP of both has, else found 0), the Control NCA, the
       update's when it has one, a DLC's Data NCA (aoc), and the HtmlDocument NCA, the offline pages the web applet
       shows (the update's when it has one; found 0 without a content meta naming it) */
    SnxNcaRef program, patch, control, aoc, html;
    const char* opened[4]; /* the files of the containers opened, by their index, which the messages name */
} SnxDump;

/* prod.keys or title.keys: given, else in repo (NULL for none), else in ~/.switch; the path in repo when it is in
   neither. NULL when out of memory */
char* snx_find_keys(const char* given, const char* name, const char* repo);
/* 0 with *xci set for an NSP (0) or an XCI (1), else nonzero, said why */
int snx_container_kind(const char* path, int* xci);
/* 1 for a file whose header is an NSP's or an XCI's, else 0, saying nothing */
int snx_is_dump(const char* path);
/* what failed, with hactool's last error after it when it said one: 1 */
int snx_hac_error(const char* what);
/* base and addon, either NULL for none, opened and their NCAs picked, with the keys given or found (snx_find_keys, the
   ones given must be there). addon is an update (patch, with its control) or a DLC (aoc): the base's when base is
   given. say prints which dump is read. 0, else nonzero, said why; snx_dump_close either way */
int snx_dump_open(SnxDump* dump, const char* base, const char* addon, const char* keys, const char* title_keys,
                  const char* repo, int say);
/* path opened alone, with the keys as snx_dump_open takes them: a game's dump (program, and the patch it may have), whose title ID is its
   program's, or else an add-on's (patch or aoc, in update), its title ID the add-on's own and game the game it belongs
   to, as its content meta names it (snx_addon_kind(game, title) tells which), and its version in *version when version
   is not NULL (SNX_NO_VERSION without a content meta, or for a game). 0, else nonzero, said why; snx_dump_close either way */
int snx_dump_title(SnxDump* dump, const char* path, const char* keys, const char* title_keys, const char* repo, int say,
                   SnxU64* title, SnxU64* game, SnxU64* version);
void snx_dump_close(SnxDump* dump);
/* the program's ExeFS and RomFS read in place, an update's over the base, or NULL, said why. An update opened
   without its base has its ExeFS and no RomFS */
SnxHacFs* snx_dump_fs(SnxDump* dump);
/* a DLC's RomFS image (aoc) read in place, or NULL, said why */
SnxHacFs* snx_dump_aoc_fs(SnxDump* dump);
/* the HtmlDocument NCA's RomFS read in place (html-document/ in it), or NULL without one */
SnxHacFs* snx_dump_html_fs(SnxDump* dump);
/* 0 when the keys an NCA of dump picked needs are there, else nonzero with a line saying which are missing, how far
   dump's keys file goes and its path, what names the NCA ("The game's Program NCA"); no key itself is said */
int snx_nca_keys(const SnxDump* dump, const SnxNcaRef* nca, const char* what);
/* bytes as a size the player reads: "512 bytes", "35 KB", "4.1 GB" */
void snx_size_text(SnxU64 bytes, char* text, size_t size);
/* a file of a RomFS read in place, by its path (control.nacp in the Control NCA's), malloc'd, or NULL */
unsigned char* snx_romfs_file(SnxHacFs* fs, const char* name, size_t* size);
/* the Control NCA's control.nacp, malloc'd, or NULL with why in *why */
unsigned char* snx_dump_nacp(SnxDump* dump, size_t* size, const char** why);
/* the Control NCA's icon, a JPEG (icon_<language>.dat, the first of control.nacp's languages it has), malloc'd, or
   NULL with why in *why */
unsigned char* snx_dump_icon(SnxDump* dump, size_t* size, const char** why);

#endif
