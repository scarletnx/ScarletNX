/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A file of what a renderer made, kept for the next run: the Switch's shaders and pipelines (nx/gpu/vk/pipeline_file.c)
   and a console's prepared shaders (prepare_shaders). A header of a magic, a version and a key, then records: a type,
   the payload's bytes and its check (SNX_PREP_REC bytes), then the payload padded to 8. What every such file checks,
   packs and writes is here; what its records hold is its renderer's. It calls no Vulkan function */
#ifndef SNX_COMMON_VK_PREPARED_H
#define SNX_COMMON_VK_PREPARED_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define SNX_PREP_REC 16 /* a record's head: its type, its payload's bytes, its check */

/* a payload's check in steps: its type and size, whole 32-bit words of it (FNV-1a), and a last mix */
uint64_t snx_prep_start(uint32_t type, uint32_t size);
uint64_t snx_prep_mix(uint64_t h, const void* p, size_t n);
uint64_t snx_prep_end(uint64_t h);
/* a whole payload's check */
uint64_t snx_prep_check(uint32_t type, const void* p, uint32_t len);
/* the head of a record of the pieces' bytes padded with zeros to 8, its check returned. The check takes whole words of
   each piece, which is then a multiple of 4 bytes */
uint64_t snx_prep_head(uint8_t head[SNX_PREP_REC], uint32_t type, const void* const* piece, const size_t* len,
                       int pieces);
/* the record at at of a file's size bytes, its type and its payload's bytes: false where it is cut short, its size is
   no multiple of 8 or its check fails, which ends a file's reading */
bool snx_prep_record(const uint8_t* file, size_t at, size_t size, uint32_t* type, uint32_t* len);
/* the header of a file of magic (8 bytes), version and key, malloc'd, padded with zeros to a multiple of 8: NULL when
   memory runs out */
uint8_t* snx_prep_header(const char* magic, uint32_t version, const char* key, size_t* size);

/* what differs between a file's key (key, len bytes: name=value words, the ones with no = the build's name, a ; ending
   its guest part) and want: "name A in the file, B now" for each, "; " between, in out (cut to n), and the count */
int snx_prep_key_diff(char* out, size_t n, const char* key, size_t len, const char* want);

/* n bytes packed into one zstd frame at level, malloc'd with its padding to 8 zeroed, *packed the frame's bytes. NULL
   with *why NULL when the frame and the over bytes beside it would not be smaller than n; NULL with *why the reason
   when it cannot be packed (no memory, zstd's error) */
uint8_t* snx_prep_pack(const void* raw, size_t n, int level, size_t over, size_t* packed, const char** why);
/* a frame of packed bytes unpacked into raw: true when it gives n bytes; else false, why (where given) its reason */
bool snx_prep_unpack(void* raw, size_t n, const void* frame, size_t packed, char* why, size_t why_n);

/* the process's id, in the name of the file written beside another */
unsigned snx_prep_pid(void);
/* path written whole by write (false when a write failed, errno its reason) into a file beside it, path.PID.tmp,
   which then replaces it, so a run that has path open or mapped keeps the one it has; before, where given, runs
   between the two (an unmap: a mapped file cannot be replaced everywhere). 0, or -1 with why the first failure's
   reason and the file beside it removed */
int snx_prep_replace(const char* path, bool (*write)(FILE* f, void* ctx), void (*before)(void* ctx), void* ctx,
                     char* why, size_t why_n);

#endif
