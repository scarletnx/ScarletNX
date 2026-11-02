/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The shader programs a game ships in its RomFS and its executables, found before it runs them (gpu/shipped_pass.c
   walks them, gpu/vk/pipeline.c translates them at startup). NVN's compiled code is the same in every container the
   SDK and NintendoWare write (BNSH, the BFSHA that embeds BNSH, agl's SHARCB) and in the byte arrays executables
   embed: a 0x30-byte wrapper starting 0x12345678, then the 0x50-byte SPH and the program (docs/NVN_HLE.md), which the
   game copies into a code pool as it stands. Containers put the wrapper at a 256-byte boundary and an executable's
   arrays at any multiple of 8, so the programs are found by that wrapper at every 4-byte word of any uncompressed
   bytes, not by each container's tables. A bare NVN shader file (0x19866891, one game's) has its SPH at 0xB50.
   SARC archives are scanned as they are, Yaz0 (.szs) after decompressing them, when the SARC inside names a shader
   container. Nothing here reads files or keeps state */
#ifndef SNX_SHADER_SHIPPED_H
#define SNX_SHADER_SHIPPED_H
#include "common/cpu/aarch64/cpu.h"
#include <stddef.h>

/* what a file's first bytes say it is: scanned whole, decompressed first, or of no use */
enum { SHIPPED_SKIP, SHIPPED_SCAN, SHIPPED_YAZ0 };
#define SHIPPED_HEAD 16 /* the bytes of a file's start read: shipped_kind looks at 4, all of them say it was changed */
int shipped_kind(const u8* head, size_t n);

/* whether a file's name (the RomFS's) ends in a format that holds no shader program (sound, textures, models,
   fonts, video, text): its first bytes then aren't read */
int shipped_name_skips(const char* name);

/* Yaz0: the size its header gives (0 when it isn't Yaz0), and the stream in[0, n) decompressed into out until want
   bytes (at most the header's size): how many it made, fewer when the input or the stream ends first */
size_t shipped_yaz0_size(const u8* in, size_t n);
size_t shipped_yaz0(const u8* in, size_t n, u8* out, size_t want);

/* A SARC's entries, by index: their count, or -1 when d[0, n) is no SARC or its tables go past n. The bytes up to
   the data offset (shipped_sarc_head) hold the tables */
int shipped_sarc_count(const u8* d, size_t n);
size_t shipped_sarc_head(const u8* d, size_t n);
/* entry i's data (offset and size in d) and its name (NUL-terminated in d, "" for an entry without one): 0, or -1
   when it lies past n */
int shipped_sarc_entry(const u8* d, size_t n, int i, size_t* off, size_t* size, const char** name);
/* whether a SARC's names say it holds shader programs (a BNSH, BFSHA or SHARCB in it): 1, 0, or -1 when its tables go
   past n. The tables are all it reads, so d may end at the data offset */
int shipped_sarc_wants(const u8* d, size_t n);

/* the runtime's stage (ST_VS to ST_FS) of the program whose SPH is at sph (type 1 vertex, tessellation and geometry,
   2 pixel, version 3): SHIPPED_VERTEX_A for a VertexA, which runs merged with the VertexB a draw binds beside it,
   and -1 for bytes that are no SPH */
#define SHIPPED_VERTEX_A (-2)
int shipped_sph_stage(const u8* sph);
/* a stage's name as the log says it (shipped_sph_stage's, SHIPPED_VERTEX_A included) */
const char* shipped_stage_name(int stage);
/* each program in d[0, n) to each, in order while it returns 0: its SPH, the bytes from there to the end of d, and
   its stage (shipped_sph_stage's, SHIPPED_VERTEX_A included). Returns how many each took (returned 0 for) */
typedef int (*ShippedEach)(void* ctx, const u8* sph, size_t avail, int stage);
u32 shipped_programs(const u8* d, size_t n, ShippedEach each, void* ctx);

/* The Switch 2's native (SM70+) programs: a 128-byte header whose first word is an SPH of version 4, then 16-byte
   instructions. Its containers (BFSHA, BNSH) put each header at a 256-byte boundary with no wrapper. The stage of
   such a header (as shipped_sph_stage's, or -1), and each such program in d[0, n) to each, given its header */
#define SHIPPED_NATIVE_HEADER 128u
int shipped_native_stage(const u8* header);
u32 shipped_native_programs(const u8* d, size_t n, ShippedEach each, void* ctx);
#endif
