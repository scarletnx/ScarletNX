/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_COMMON_ZIP_H
#define SNX_COMMON_ZIP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A zip read in place: a Vita VPK, an Android APK. Members are stored or deflated; zip64 and encryption are refused.
   The directory is read once, at open, and its names sorted for snx_zip_find. A zip is not safe to read from two
   threads at once: its reads share one file position */
typedef struct SnxZip SnxZip;

/* NULL with error filled when path is not a zip this reads */
SnxZip* snx_zip_open(const char* path, char* error, size_t error_size);
void snx_zip_close(SnxZip* zip);
/* its members, in the directory's order */
uint32_t snx_zip_count(const SnxZip* zip);
const char* snx_zip_name(const SnxZip* zip, uint32_t index);
uint32_t snx_zip_size(const SnxZip* zip, uint32_t index);
/* whether name ("res/layout/main.xml") is a member, its index in *index */
bool snx_zip_find(const SnxZip* zip, const char* name, uint32_t* index);
/* the member's bytes, *size of them (with one zero byte after them), the caller's to free. NULL with error filled
   when it can't be read or its CRC doesn't match */
unsigned char* snx_zip_read(SnxZip* zip, uint32_t index, size_t* size, char* error, size_t error_size);
/* one member's bytes by name, as snx_zip_read, opening and closing the zip */
unsigned char* snx_zip_member(const char* path, const char* name, size_t* size, char* error, size_t error_size);
/* a member's name as a path inside a folder: relative, '/' between parts, no "." or ".." part and no drive */
bool snx_zip_safe_name(const char* name);
#endif
