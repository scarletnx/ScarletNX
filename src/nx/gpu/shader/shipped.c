/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/shader/shipped.h"
#include "nx/gpu/shader/shader.h"
#include <string.h>

#define NVN_WRAP     0x12345678u /* a graphics program's wrapper, 0x30 bytes before its SPH */
#define NVN_FILE     0x19866891u /* a bare NVN shader file: the control section, then the SPH at NVN_FILE_SPH */
#define NVN_FILE_SPH 0xB50u
#define SPH_BYTES    0x50u

static u32 le32(const u8* p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | (u32)p[3]; }
static u16 le16(const u8* p) { return (u16)(p[0] | p[1] << 8); }

int shipped_kind(const u8* head, size_t n) {
    if (n < 4) return SHIPPED_SKIP;
    if (!memcmp(head, "Yaz0", 4)) return SHIPPED_YAZ0;
    /* BNSH, BFSHA, agl's SHARCB, SARC, a program's own wrapper or a bare NVN file */
    if (!memcmp(head, "BNSH", 4) || !memcmp(head, "FSHA", 4) || !memcmp(head, "BAHS", 4) || !memcmp(head, "SARC", 4) ||
        le32(head) == NVN_WRAP || le32(head) == NVN_FILE)
        return SHIPPED_SCAN;
    return SHIPPED_SKIP;
}

static int ends_with(const char* s, const char* end);
int shipped_name_skips(const char* name) {
    /* formats of their own, none a container of other files: sound (bars, bfstm, bfwav, bwav, bfsar, Wwise's bnk and
       wem), textures and pictures, models (bfres keeps its shaders in a bfsha), collision (kcl), parameters (byaml),
       rumble (bnvib), messages, fonts, movies and text. And zstd streams (zs), which the walk doesn't decompress: one
       game packs nearly all of its 304,251 files so, and opening each cost the walk 29 seconds */
    static const char* const ends[] = {".bars",  ".barslist", ".bfstm", ".bfwav", ".bwav", ".bfsar", ".bnk",   ".wem",
                                       ".bntx",  ".png",      ".jpg",   ".jpeg",  ".gif",  ".bmp",   ".bfres", ".kcl",
                                       ".byaml", ".byml",     ".bnvib", ".msbt",  ".msbp", ".bffnt", ".ttf",   ".otf",
                                       ".mp4",   ".webm",     ".bk2",   ".usm",   ".ogg",  ".opus",  ".wav",   ".txt",
                                       ".json",  ".xml",      ".csv",   ".zs"};
    for (size_t k = 0; k < sizeof ends / sizeof *ends; k++)
        if (ends_with(name, ends[k])) return 1;
    return 0;
}
size_t shipped_yaz0_size(const u8* in, size_t n) {
    return n >= 16 && !memcmp(in, "Yaz0", 4) ? (size_t)be32(in + 4) : 0;
}
/* Yaz0's stream: groups of a flag byte and eight items, its bits from the top: 1 a byte as it is, 0 a copy of bytes
   already made, two bytes (the count's nibble and the distance's twelve bits) or three (a count from 18) */
size_t shipped_yaz0(const u8* in, size_t n, u8* out, size_t want) {
    size_t at = 16, made = 0, total = shipped_yaz0_size(in, n);
    if (want > total) want = total;
    while (made < want && at < n) {
        u32 flags = in[at++];
        for (int bit = 0; bit < 8 && made < want && at < n; bit++, flags <<= 1) {
            if (flags & 0x80u) {
                out[made++] = in[at++];
                continue;
            }
            if (n - at < 2) return made;
            size_t dist = (((size_t)in[at] & 0xFu) << 8 | in[at + 1]) + 1, count = in[at] >> 4;
            at += 2;
            if (!count) {
                if (at >= n) return made;
                count = (size_t)in[at++] + 0x12;
            } else
                count += 2;
            if (dist > made) return made; /* before the start: a stream that isn't Yaz0's */
            /* a byte at a time: the copy reads what it writes when the distance is under the count */
            for (; count && made < want; count--, made++) out[made] = out[made - dist];
        }
    }
    return made;
}

/* SARC's header (0x14 bytes), SFAT's (0xC) and its nodes (0x10 each), then SFNT's header (8) and the names. The
   Switch's are little-endian, which the byte order mark says */
static int sarc_tables(const u8* d, size_t n, u32* count, size_t* nodes, size_t* names, size_t* data) {
    if (n < 0x20 || memcmp(d, "SARC", 4) || le16(d + 6) != 0xFEFFu) return -1;
    size_t sfat = le16(d + 4);
    if (sfat > n - 0xC || memcmp(d + sfat, "SFAT", 4)) return -1;
    *count = le16(d + sfat + 6);
    *nodes = sfat + le16(d + sfat + 4);
    size_t sfnt = *nodes + (size_t)*count * 16;
    if (sfnt > n - 8 || memcmp(d + sfnt, "SFNT", 4)) return -1;
    *names = sfnt + le16(d + sfnt + 4);
    *data = le32(d + 0xC);
    return *data <= n ? 0 : -1;
}
size_t shipped_sarc_head(const u8* d, size_t n) { return n >= 0x10 && !memcmp(d, "SARC", 4) ? le32(d + 0xC) : 0; }
int shipped_sarc_count(const u8* d, size_t n) {
    u32 count;
    size_t nodes, names, data;
    return sarc_tables(d, n, &count, &nodes, &names, &data) ? -1 : (int)count;
}
/* entry i's place in the data and its name, from the tables alone */
static int sarc_node(const u8* d, size_t n, int i, u32* begin, u32* end, const char** name) {
    u32 count;
    size_t nodes, names, data;
    if (sarc_tables(d, n, &count, &nodes, &names, &data) || i < 0 || (u32)i >= count) return -1;
    const u8* node = d + nodes + (size_t)i * 16;
    u32 attr = le32(node + 4);
    *begin = le32(node + 8);
    *end = le32(node + 12);
    *name = "";
    /* named when its top byte isn't 0 (it counts the names before it that hash alike): the name at a multiple of 4 in
       SFNT's, ending before the data */
    if (attr >> 24) {
        size_t at = names + (size_t)(attr & 0xFFFFu) * 4;
        if (at >= data || !memchr(d + at, 0, data - at)) return -1;
        *name = (const char*)d + at;
    }
    return 0;
}
int shipped_sarc_entry(const u8* d, size_t n, int i, size_t* off, size_t* size, const char** name) {
    u32 begin, end;
    size_t data = shipped_sarc_head(d, n); /* within n once sarc_node read the tables */
    if (sarc_node(d, n, i, &begin, &end, name) || end < begin || end > n - data) return -1;
    *off = data + begin;
    *size = end - begin;
    return 0;
}
static int ends_with(const char* s, const char* end) {
    size_t a = strlen(s), b = strlen(end);
    return a >= b && !strcmp(s + a - b, end);
}
int shipped_sarc_wants(const u8* d, size_t n) {
    /* the shader containers only, not an archive in it: following those decompressed every course of one game,
       none of which held a shader */
    static const char* const ends[] = {".bnsh", ".bfsha", ".sharcb"};
    int count = shipped_sarc_count(d, n);
    if (count < 0) return -1;
    for (int i = 0; i < count; i++) { /* by the names alone: a Yaz0 one is decompressed as far as its tables to tell */
        u32 begin, end;
        const char* name;
        if (sarc_node(d, n, i, &begin, &end, &name)) return -1;
        for (size_t k = 0; k < sizeof ends / sizeof *ends; k++)
            if (ends_with(name, ends[k])) return 1;
    }
    return 0;
}

const char* shipped_stage_name(int stage) {
    static const char* const names[] = {
        "vertex", "tessellation control", "tessellation evaluation", "geometry", "fragment", "compute"};
    return stage == SHIPPED_VERTEX_A                                   ? "paired vertex"
           : stage >= 0 && stage < (int)(sizeof names / sizeof *names) ? names[stage]
                                                                       : "?";
}
/* the stage of an SPH's first word, of the version given (3 for Maxwell's, 4 for the Switch 2's native programs) */
static int sph_stage(u32 w, u32 version) {
    u32 type = w & 0x1Fu, shader = w >> 10 & 0xFu;
    if ((w >> 5 & 0x1Fu) != version) return -1;
    if (type == 2) return shader == 5 ? ST_FS : -1;
    if (type != 1) return -1;
    /* type 0 a VertexA, as one game's runs showed: each of its programs of that type has a VertexB in a 256-byte slot
       after it, and its draws found the shaders the pass merged from the two (gpu/vk/pipeline.c ship_dual) */
    return shader == 0   ? SHIPPED_VERTEX_A
           : shader == 1 ? ST_VS
           : shader == 2 ? ST_TCS
           : shader == 3 ? ST_TES
           : shader == 4 ? ST_GS
                         : -1;
}
int shipped_sph_stage(const u8* sph) { return sph_stage(le32(sph), 3); }
u32 shipped_programs(const u8* d, size_t n, ShippedEach each, void* ctx) {
    u32 handed = 0;
    if (n >= NVN_FILE_SPH + SPH_BYTES && le32(d) == NVN_FILE) { /* one program, after its control section */
        int stage = shipped_sph_stage(d + NVN_FILE_SPH);
        if (stage != -1 && !each(ctx, d + NVN_FILE_SPH, n - NVN_FILE_SPH, stage)) handed++;
        return handed;
    }
    /* the wrappers start code pool slots, 256-byte aligned in the containers, but an executable keeps a program as an
       array of bytes the compiler aligns to 8 (one game's were 8 past a multiple of 16), which the game copies into
       its pool: every word is looked at */
    for (size_t at = 0; n >= 0x30 + SPH_BYTES && at <= n - 0x30 - SPH_BYTES; at += 4) {
        int stage;
        if (le32(d + at) != NVN_WRAP || (stage = shipped_sph_stage(d + at + 0x30)) == -1) continue;
        if (each(ctx, d + at + 0x30, n - at - 0x30, stage)) return handed;
        handed++;
    }
    return handed;
}
/* the Switch 2's NVN: a 128-byte header whose first word is an SPH's of version 4, then the SM70+ code. The shader
   containers put each header at a 256-byte boundary with no wrapper before it */
int shipped_native_stage(const u8* header) { return sph_stage(le32(header), 4); }
u32 shipped_native_programs(const u8* d, size_t n, ShippedEach each, void* ctx) {
    u32 handed = 0;
    for (size_t at = 0; n > SHIPPED_NATIVE_HEADER && at < n - SHIPPED_NATIVE_HEADER; at += 0x100) {
        int stage = shipped_native_stage(d + at);
        if (stage == -1) continue;
        if (each(ctx, d + at, n - at, stage)) return handed;
        handed++;
    }
    return handed;
}
