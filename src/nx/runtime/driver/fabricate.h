/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An extracted game (exefs/, romfs/, and control.nacp and icon.jpg when it has them) made into fabricated NCAs for
   scarletnx --pack (fabricate.c): a Program, a Control and a Meta NCA, each a plain NCA3 whose sections are not
   encrypted. What a dump would carry and the folder does not is a stub: the header's signatures are zero, a missing
   control.nacp is a stub one, the content meta is made up, and each content ID is the SHA-256 of its NCA's header */
#ifndef SNX_FABRICATE_H
#define SNX_FABRICATE_H

#include "common/utilities/common.h"
#include <stdint.h>

typedef struct SnxFab SnxFab;

/* bytes written over what was written before, at their offset from the start of the NCAs */
typedef struct {
    SnxU64 at;
    unsigned char* bytes;
    size_t size;
} SnxFabPatch;

/* 1 when folder is an extracted game's: an ExeFS with main (snx_exefs_folder), else 0 */
int snx_fab_is_game(const char* folder);
/* The NCAs folder makes, laid out but not read: main.npdm (for the title ID) and the sizes of the files alone. name is
   the stub control.nacp's title, NULL for the folder's own name. say: what it is made of, said. NULL, said why */
SnxFab* snx_fab_plan(const char* folder, const char* name, int say);
void snx_fab_free(SnxFab* fab);
SnxU64 snx_fab_title(const SnxFab* fab);
/* the NCAs, in the order snx_fab_write writes them: Program, Control, Meta */
size_t snx_fab_count(const SnxFab* fab);
SnxU64 snx_fab_size(const SnxFab* fab, size_t nca);
/* its file name, <content ID>.nca or .cnmt.nca: all zeros until snx_fab_write has written it, the same length */
const char* snx_fab_name(const SnxFab* fab, size_t nca);
/* The NCAs, one after the other, through write (0 to go on), reading each file once. What is known only once the
   data behind it is read (the hash tables before it, each NCA's header) is written as zeros, and comes back as
   patches. 0, else nonzero, said why */
int snx_fab_write(SnxFab* fab, int (*write)(const void* bytes, size_t size, void* data), void* data);
/* the patches the last snx_fab_write left, which the caller writes over its output; fab keeps them */
size_t snx_fab_patches(const SnxFab* fab, const SnxFabPatch** patches);

/* A PFS0 (hfs0 0) or HFS0 (1) header of count files, names[i] of sizes[i] bytes, one after the other, padded to a
   multiple of align. An HFS0's entry hashes the first hashed[i] bytes of its file, hashes[i] the SHA-256 of them;
   hashed and hashes NULL for a PFS0. malloc'd, its size in *size, or NULL */
unsigned char* snx_partition_header(const char* const* names, const SnxU64* sizes, const uint32_t* hashed,
                                    const unsigned char (*hashes)[32], size_t count, int hfs0, SnxU64 align,
                                    size_t* size);

#endif
