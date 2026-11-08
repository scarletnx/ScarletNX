/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the indexed filesystem needs from the host, shared by the NX and CTR runtimes: folders, and files kept
   mapped read-only for the whole run. No guest type enters here, so sizes are <stdint.h>'s */
#ifndef SNX_FS_HOST_H
#define SNX_FS_HOST_H
#include <stddef.h>
#include <stdint.h>

#include "common/platform/thread.h" /* PMutex and the pm_* calls */

#if defined(_WIN32)
/* the UTF-8 path as a malloc'd wide string for the W calls; NULL when it isn't valid UTF-8 (or out of memory) */
wchar_t* snx_host_wpath(const char* text);
#endif

int plat_mkdir(const char* path); /* 0, or -1 */

/* Opens a read-only file for the rest of the run and maps it shared, so its pages are the host's page cache, read on
   first touch. size is the file's. An empty file keeps *fd with *data NULL. 0, or -1 with *fd = -1 and *data = NULL
   (it can't be opened or mapped, it is too large to map, or keeping it would leave too few descriptors for the files
   opened on demand) */
int plat_file_share(const char* path, uint64_t size, int* fd, const uint8_t** data);
void plat_file_unshare(int fd, const uint8_t* data, uint64_t size);
#endif
