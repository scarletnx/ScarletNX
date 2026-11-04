/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A mod's code patches, each made for one build of a module: an IPS or IPS32 file named after the module's build ID
   (exefs/<build id>.ips) and IPSwitch text (exefs/<name>.pchtxt) that names it with @nsobid-. Both address the module
   as its NSO header, SNX_PATCH_HEADER bytes, then its image from the first segment's memory offset, the way Eden's
   ips_layer.cpp (GPL-3.0-or-later) reads them, which this follows; a byte that falls in the header is the header's,
   and nothing takes it */
#ifndef SNX_PATCHES_H
#define SNX_PATCHES_H
#include "common/utilities/common.h"

#define SNX_PATCH_HEADER 0x100

/* the .ips and .pchtxt files of the played mods' exefs/ (snx_mods_played, off as mods_off= has it), each mod in the
   order they are laid and its files by name, into *paths (snx_free_strings): their count */
size_t snx_mods_patches(const char* folder, const char* off, char*** paths);
/* the same for a program with mods of its own in own (NULL for none), mods.h's snx_mods_played_of */
size_t snx_mods_patches_of(const char* folder, const char* own, const char* off, char*** paths);
/* 1 when the patch at path is for the module built as build_id: a .ips by its name (the build ID's hex, any case, its
   ending zeros left out or not), a .pchtxt by its @nsobid; 0 for another build or another file; -1 when a .pchtxt
   cannot be read, error saying why */
int snx_patch_for(const char* path, const unsigned char build_id[32], char* error, size_t error_size);
/* each of the count patches at paths made for the module built as build_id (snx_patch_for) written over its image,
   size bytes, in their order, applied[i] set for each written (applied may be NULL): 0, else nonzero with error
   naming the patch and what in it */
int snx_patch_module(unsigned char* image, size_t size, const unsigned char build_id[32], char* const* paths,
                     size_t count, unsigned char* applied, char* error, size_t error_size);
/* the patch at path written over image, size bytes: 0, else nonzero with error naming the file and what in it */
int snx_patch_apply(const char* path, unsigned char* image, size_t size, char* error, size_t error_size);
/* the same from a patch's bytes: an IPS or IPS32 patch (by its magic), or IPSwitch text; error without the file */
int snx_ips_apply(const unsigned char* ips, size_t ips_size, unsigned char* image, size_t size, char* error,
                  size_t error_size);
int snx_pchtxt_apply(const char* text, size_t text_size, unsigned char* image, size_t size, char* error,
                     size_t error_size);
/* IPSwitch text's @nsobid: 1 with its 32 bytes (the hex given, zeros after it), 0 for none or one not hex */
int snx_pchtxt_build_id(const char* text, size_t text_size, unsigned char build_id[32]);
#endif
