/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VITA_VPK_H
#define SNX_VITA_VPK_H
#include <stdbool.h>
#include <stddef.h>

/* A VPK is a zip of the application's folder (app0:): eboot.bin, sce_sys/param.sfo and the game's files. Members are
   stored or deflated; zip64 and encryption are refused. */

/* One member's bytes, by its name in the zip ("sce_sys/param.sfo"), *size of them, the caller's to free. NULL with
   error filled when the VPK can't be read or has no such member */
unsigned char* vita_vpk_member(const char* vpk, const char* name, size_t* size, char* error, size_t error_size);
/* Every member into folder (made, with its subfolders): a name leaving the folder (absolute, "..", a drive) is
   refused, as is any member that fails its CRC. False with error filled */
bool vita_vpk_extract(const char* vpk, const char* folder, char* error, size_t error_size);
/* A Vita title ID as param.sfo's TITLE_ID has them: four capital letters and five digits (PCSE00001) */
bool vita_title_id(const char* text);
#endif
