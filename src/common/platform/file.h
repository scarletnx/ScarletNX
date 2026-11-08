/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The host's files, the same for every driver: reads at an offset, read-only maps, the replace of a file by another,
   folders, and files shared with the host's page cache for the whole run. Paths are UTF-8 everywhere. No guest type
   enters here, so sizes are <stdint.h>'s. Each group is an object file of its own (file.c, file_map.c, file_share.c,
   file_replace.c), so a test that stubs one group links the others */
#ifndef SNX_PLATFORM_FILE_H
#define SNX_PLATFORM_FILE_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#if defined(_WIN32)
/* the UTF-8 path as a malloc'd wide string for the W calls; NULL when it isn't valid UTF-8 (or out of memory) */
wchar_t* snx_host_wpath(const char* text);
#endif

/* ---- file.c */
/* Reads size bytes at off of the descriptor fd without moving its position, going on after a short read until done or
   at the end of the file. The count read, short only at the end of the file, or -1 with errno (EBADF, EINVAL, EIO on
   Windows; pread's elsewhere) */
int64_t plat_pread_fd(int fd, void* buf, uint64_t size, uint64_t off);
int64_t plat_pwrite_fd(int fd, const void* buf, uint64_t size, uint64_t off); /* pwrite(2) whole, as plat_pread_fd */
int64_t plat_pread(FILE* f, void* buf, uint64_t size, uint64_t off); /* plat_pread_fd of f's descriptor */
int plat_is_dir(const char* path);
int plat_mkdir(const char* path); /* 0, or -1 */

/* ---- file_map.c */
typedef struct {
    const uint8_t* data; /* NULL for an empty file */
    uint64_t size;
    int borrowed; /* set by fs_map_file for a view kept for the whole run: plat_file_unmap then only clears this */
    int heap;     /* set by fs_map_file for a file read out of the game's dump into memory: plat_file_unmap frees it */
} PFileMap;
/* the whole file mapped read-only. 0 (an empty file too), or -1 */
int plat_file_map(PFileMap* m, const char* path);
/* the same for a file whose length is already known from an index: size is trusted, the host isn't asked */
int plat_file_map_sized(PFileMap* m, const char* path, uint64_t size);
void plat_file_unmap(PFileMap* m);

/* ---- file_share.c */
/* Opens a read-only file for the rest of the run and maps it shared, so its pages are the host's page cache, read on
   first touch. size is the file's. An empty file keeps *fd with *data NULL. 0, or -1 with *fd = -1 and *data = NULL
   (it can't be opened or mapped, it is too large to map, or keeping it would leave too few descriptors for the files
   opened on demand) */
int plat_file_share(const char* path, uint64_t size, int* fd, const uint8_t** data);
void plat_file_unshare(int fd, const uint8_t* data, uint64_t size);

/* ---- file_replace.c */
/* Moves src over dst atomically (a file or a folder, on one volume), so dst is always either the old or the new one:
   rename, or MoveFileExW replacing dst and returning once the move is on the disk. 0, or -1 with dst untouched and
   src left where it was, which the caller removes or keeps */
int plat_replace_file(const char* src, const char* dst);
#endif
