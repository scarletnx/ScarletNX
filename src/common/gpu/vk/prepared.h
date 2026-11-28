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

/* ---- a console's prepared shaders (prepare_shaders): its file read whole as the game starts, then appended to as its
   renderer makes a shader or a pipeline, each record flushed as it is added ---- */
#define SNX_PREP_PACKED 0x100u /* a record type's bit: an SnxPrepPacked, the head, then one zstd frame of the body */
#define SNX_PREP_MAX    (64u << 20) /* past this size the file takes no more */
#define SNX_PREP_LEVEL  1           /* zstd's level for a body packed alone, as the Switch packs its shaders */
typedef struct {
    uint32_t head, raw, packed, pad; /* the head's bytes, kept as they are; the body's, and its frame's */
} SnxPrepPacked;
typedef struct {
    FILE* f;         /* appended to; NULL once it takes no more, or where it could not be written */
    const char* who; /* before each line: "3ds gpu" */
    bool pack;       /* compress_prepared_shaders: each record's body packed on its own where that makes it smaller */
    bool pack_said;  /* a body that could not be packed was said */
    uint64_t size;
    uint32_t records; /* read and added */
    char path[1200];
} SnxPrepFile;
/* path read: with the header of magic, version and key, each record handed to each in order (a packed one unpacked,
   its type without SNX_PREP_PACKED), until each refuses one (false, which each says) or one is cut short or changed;
   the file is then cut there. One of another key starts over, said with what differs. Then open to append to. The
   records read */
uint32_t snx_prep_open(SnxPrepFile* p, const char* path, const char* magic, uint32_t version, const char* key,
                       bool (*each)(void* ctx, uint32_t type, const uint8_t* payload, uint32_t len), void* ctx);
/* a record of type added: its head as it is, then its body, packed alone where p->pack makes it smaller. 0, or -1 when
   the file takes no more (the first time said with why) */
int snx_prep_add(SnxPrepFile* p, uint32_t type, const void* head, size_t head_n, const void* body, size_t body_n);
void snx_prep_close(SnxPrepFile* p);
#endif
