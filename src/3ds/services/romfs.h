/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_ROMFS_H
#define SNX_CTR_ROMFS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "common/filesystem/tree.h"
typedef struct SnxCtrRomfs SnxCtrRomfs;
/* Builds the CTR level-3 view over the tree's "romfs" child, shared read-only
   and always open like NX's romfs/exefs. The tree is owned by the process and
   outlives the view; missing romfs/ is genuinely absent (*out stays NULL).
   No game is reopened. */
bool snx_ctr_romfs_open(FsTree* tree, SnxCtrRomfs** out);
/* The view over a level-3 image read where it lies (snx_ctr_source_romfs), which
   outlives the view: reads copy from it, and every entry's timestamp is mtime,
   the game's (UTC Unix milliseconds). image NULL: the empty RomFS (a game with
   none), as an empty romfs/ folder serializes. False for an image too small
   or too large */
bool snx_ctr_romfs_open_image(const unsigned char* image, uint64_t size, uint64_t mtime, SnxCtrRomfs** out);
void snx_ctr_romfs_free(SnxCtrRomfs* fs);
uint64_t snx_ctr_romfs_size(const SnxCtrRomfs* fs);
bool snx_ctr_romfs_read(SnxCtrRomfs* fs, uint64_t offset, void* data, size_t size);
/* The file that path names (relative, '/'-separated UTF-8) in a level-3 image of size bytes, as
   snx_ctr_romfs_open_image reads one: the offset in the image of its data and the data's length. False for a folder,
   a path that names nothing, or data outside the image. */
bool snx_ctr_romfs_file(const unsigned char* image, uint64_t size, const char* path, uint64_t* data, uint64_t* length);
/* FSUSER ControlArchive GET_TIMESTAMP over the RomFS view. path16 is the raw
   bounded UTF-16LE request input (NUL-terminated within bytes, <= 12 bytes).
   Returns the backing file's real host modification time as a 3DS datetime:
   milliseconds since 1900-01-01 00:00:00 UTC. */
typedef enum { SNX_CTR_TIME_OK = 0, SNX_CTR_TIME_NOTFOUND, SNX_CTR_TIME_INVALID } SnxCtrTimeResult;
SnxCtrTimeResult snx_ctr_romfs_time(SnxCtrRomfs* fs, const unsigned char* path16, size_t bytes, uint64_t* datetime);
/* Bounded UTF-16LE archive path to UTF-8 with '/' separators (also accepts
   '\\'); the result is NUL-terminated. False: malformed or too long. */
bool snx_ctr_utf16_path(const unsigned char* path16, size_t bytes, char* out, size_t out_size);
#endif
