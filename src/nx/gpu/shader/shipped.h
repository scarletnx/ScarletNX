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
   container. An Ounce game's programs (SM86, SPH version 4) have no wrapper: they are found by their last
   instruction instead (shipped_ounce_programs), and its RomFS keeps its shader containers as zstd streams (.zs),
   which the walk decompresses for such a game alone. Nothing here reads files or keeps state */
#ifndef SNX_SHADER_SHIPPED_H
#define SNX_SHADER_SHIPPED_H
#include "common/cpu/aarch64/cpu.h"
#include <stddef.h>

/* what a file's first bytes say it is: scanned whole, decompressed first, or of no use */
enum { SHIPPED_SKIP, SHIPPED_SCAN, SHIPPED_YAZ0, SHIPPED_ZSTD };
#define SHIPPED_HEAD 16 /* the bytes of a file's start read: shipped_kind looks at 4, all of them say it was changed */
int shipped_kind(const u8* head, size_t n);

/* whether a file's name (the RomFS's) ends in a format that holds no shader program (sound, textures, models,
   fonts, video, text): its first bytes then aren't read */
int shipped_name_skips(const char* name);
/* For an Ounce game's walk: whether a file's name says it holds shader programs its first bytes don't tell, so its
   name-skip and first-byte checks are passed by: 1 for a zstd stream of a shader container or a SARC (".bfsha.zs",
   ".bnsh.zs", ".sharcb.zs", ".sarc.zs", ".genvb.zs"), 2 for a shader binary of its own format (".bin" with "shader" in its last
   part, sead's), else 0 */
int shipped_name_ounce(const char* name);

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
/* a stage's name as the log says it (shipped_sph_stage's, SHIPPED_VERTEX_A included, SHIPPED_OUNCE ignored) */
const char* shipped_stage_name(int stage);
/* each program in d[0, n) to each, in order while it returns 0: its SPH, the bytes from there to the end of d, its
   stage (shipped_sph_stage's, SHIPPED_VERTEX_A included), and its constants, the bytes the game's driver binds as
   constant buffer 1 (the compiler's jump tables of BRX among them), when a BNSH's code record names its control
   section (NULL and 0 otherwise). Returns how many each took (returned 0 for) */
typedef int (*ShippedEach)(void* ctx, const u8* sph, size_t avail, int stage, const u8* consts, size_t nconsts);
u32 shipped_programs(const u8* d, size_t n, ShippedEach each, void* ctx);
/* An Ounce game's programs in d[0, n), handed to each as shipped_programs does, with SHIPPED_OUNCE in
   their stage (ST_VS to ST_FS), and a VertexA as SHIPPED_OUNCE_VERTEX_A. Each one ends in an
   unconditional branch to itself after its EXIT, and starts after its 0x80-byte SPH of version 4: from that branch,
   at every 8 bytes, back over the words that look like instructions to the nearest such SPH */
#define SHIPPED_OUNCE 0x100
#define SHIPPED_OUNCE_VERTEX_A (SHIPPED_OUNCE | 0x80)
/* the stage of the SPH (version 4) at sph, as shipped_sph_stage's of version 3 (SHIPPED_VERTEX_A for a VertexA) */
int shipped_ounce_stage(const u8* sph);
u32 shipped_ounce_programs(const u8* d, size_t n, ShippedEach each, void* ctx);
#endif
