/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* hactool as scarletnx --extract calls it, in its own process (snx_hac.c): the containers opened once, each NCA read
   where it lies in them, and the ExeFS and RomFS written straight into the extraction. Plain C99, for the driver;
   hactool's own headers stay inside hactool_lib */
#ifndef SNX_HAC_H
#define SNX_HAC_H

#include <stddef.h>
#include <stdint.h>

typedef struct SnxHac SnxHac;

/* a file of a container: an NSP's, or one in an XCI's secure partition */
typedef struct {
    const char* name;
    uint64_t offset, size; /* in the container's file */
} SnxHacEntry;

enum { SNX_HAC_OTHER, SNX_HAC_PROGRAM, SNX_HAC_CONTROL, SNX_HAC_DATA };

/* what an NCA's header says */
typedef struct {
    int type;  /* SNX_HAC_PROGRAM, SNX_HAC_CONTROL, SNX_HAC_DATA (Data or PublicData: a DLC's) or SNX_HAC_OTHER */
    int patch; /* it has a Patch RomFS (BKTR) section: an update's */
    uint64_t title_id;
    /* what reading it takes, for what the extraction says: never a key itself */
    int content;      /* the header's content type: 0 Program, 1 Meta, 2 Control, 3 Manual, 4 Data, 5 PublicData */
    int decrypted;    /* a decrypted NCA (DNCA), which needs no keys */
    int generation;   /* the master key generation its keys derive from */
    int title_key;    /* encrypted with a title key: 1 when a ticket of the dump or title.keys gives it, -1 when none
                         does; 0 for an NCA with its keys in its key area */
    int keys;         /* 1 when prod.keys gave the key of its generation that opens its key area or title key */
    int newest;       /* the newest generation whose key of that kind prod.keys gave, -1 for none */
    uint32_t sdk;     /* the SDK it was built with, major.minor.micro.relstep a byte each from the top */
} SnxHacNca;

SnxHac* snx_hac_new(void);
void snx_hac_free(SnxHac* hac);

/* a prod.keys line: 1 when name is a key's, else 0. Its value must have the key's length in hex digits */
int snx_hac_set_key(SnxHac* hac, const char* name, const char* value);
/* after the last key: what derives from them */
void snx_hac_derive_keys(SnxHac* hac);
void snx_hac_add_title_key(SnxHac* hac, const unsigned char rights_id[16], const unsigned char key[16]);
/* the newest master key generation whose key area keys the keyset has, derived or given: -1 for none */
int snx_hac_newest_generation(SnxHac* hac);

/* a container to read NCAs in place from, XCI or NSP: its number for the calls below, or -1. Two at most */
int snx_hac_open(SnxHac* hac, const char* path, int xci);
size_t snx_hac_entries(SnxHac* hac, int container, const SnxHacEntry** entries);
/* the first size bytes of an entry: 0, or -1 */
int snx_hac_read(SnxHac* hac, int container, size_t entry, void* buffer, size_t size);
/* 0, or -1 when its header cannot be decrypted (the keys are wrong or missing) */
int snx_hac_nca(SnxHac* hac, int container, size_t entry, SnxHacNca* nca);
/* the NCA's ExeFS into exefs_dir and its RomFS into romfs_dir (either NULL to leave it), an update's over the base NCA
   (base_container -1 for none): 0, or -1 */
int snx_hac_extract(SnxHac* hac, int container, size_t entry, int base_container, size_t base_entry,
                    const char* exefs_dir, const char* romfs_dir);

/* The file systems of a program NCA read where they lie, the way the runtime reads a game it did not extract: the
   ExeFS's files and the RomFS image, an update's over the base NCA (base_container -1 for none). They stay open until
   snx_hac_fs_free, and read from the containers' files, or from their mappings (snx_hac_map). Reads are not thread
   safe: their decryption contexts are shared */
typedef struct SnxHacFs SnxHacFs;
/* the container's file as data (size bytes) maps it, for the file systems opened afterwards to read from: 0, or -1
   when one of its entries lies past size. The mapping outlives them */
int snx_hac_map(SnxHac* hac, int container, const void* data, uint64_t size);
/* NULL when the NCA cannot be read */
SnxHacFs* snx_hac_fs_open(SnxHac* hac, int container, size_t entry, int base_container, size_t base_entry);
void snx_hac_fs_free(SnxHacFs* fs);
/* the ExeFS's files, their offsets inside its section; 0 without an ExeFS */
size_t snx_hac_fs_files(SnxHacFs* fs, const SnxHacEntry** files);
/* size bytes at offset of an ExeFS file: 0, or -1 */
int snx_hac_fs_read(SnxHacFs* fs, size_t file, uint64_t offset, void* buffer, size_t size);
/* the RomFS image's size, 0 without one */
uint64_t snx_hac_fs_romfs_size(const SnxHacFs* fs);
/* size bytes of the RomFS image at offset: 0, or -1 */
int snx_hac_fs_read_romfs(SnxHacFs* fs, uint64_t offset, void* buffer, size_t size);

/* The content meta (a .cnmt file) of a Meta NCA, the one file of its PFS0 section ending in .cnmt, malloc'd into
   *data (size bytes): 0, or -1 when the NCA cannot be read or holds none */
int snx_hac_cnmt(SnxHac* hac, int container, size_t entry, unsigned char** data, size_t* size);

/* tick(written, data) as hactool writes the extraction, written the bytes so far */
void snx_hac_progress(void (*tick)(uint64_t written, void* data), void* data);
/* cleanup(data) before an exit hactool takes on a dump it cannot read, which ends the process */
void snx_hac_on_exit(void (*cleanup)(void* data), void* data);
/* what hactool said last on its error stream, runs of 16 or more hex digits (possible keys) left out, or "" */
const char* snx_hac_message(void);

#endif
