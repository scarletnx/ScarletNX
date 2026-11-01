/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gpu/shader/shipped.c on containers built here: NVN's program wrapper and SPH, a bare NVN shader file, a BNSH's code
   records that name programs' constants, a SARC that names its entries, and Yaz0 streams of literals and of copies
   that overlap what they write. An Ounce game's programs, found by their ends, and its zstd files */
#include "nx/gpu/shader/shipped.h"
#include "nx/gpu/shader/shader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void put32(u8* p, u32 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}
static void put16(u8* p, u32 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}
static void be32(u8* p, u32 v) {
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}
/* an SPH's first word: its type (1 VTG, 2 pixel), version 3, and its shader type */
static u32 sph(u32 type, u32 shader) { return type | 3u << 5 | shader << 10; }
/* a program at d + at: the wrapper, then the SPH's first word */
static void program(u8* d, size_t at, u32 word) {
    put32(d + at, 0x12345678u);
    put32(d + at + 0x30, word);
}

static void put64(u8* p, u64 v) {
    put32(p, (u32)v);
    put32(p + 4, (u32)(v >> 32));
}

/* each program's stage, SPH offset, and constants (their offset, 0 for none, and size) */
typedef struct {
    int n, stage[8];
    size_t at[8], consts[8], nconsts[8];
    const u8* base;
    int stop_after;
} Seen;
static int seen(void* ctx, const u8* p, size_t avail, int stage, const u8* consts, size_t nconsts) {
    Seen* s = ctx;
    (void)avail;
    if (s->n < 8) {
        s->stage[s->n] = stage;
        s->at[s->n] = (size_t)(p - s->base);
        s->consts[s->n] = consts ? (size_t)(consts - s->base) : 0;
        s->nconsts[s->n] = nconsts;
    }
    s->n++;
    return s->stop_after && s->n >= s->stop_after;
}

static void programs(void) {
    static u8 d[0x1000];
    Seen s = {0};
    memset(d, 0, sizeof d);
    program(d, 0x000, sph(1, 1));    /* VertexB */
    program(d, 0x100, sph(2, 5));    /* pixel */
    program(d, 0x200, sph(1, 0));    /* VertexA */
    program(d, 0x300, sph(1, 4));    /* geometry */
    program(d, 0x400, sph(2, 1));    /* a pixel header with a vertex type: no SPH */
    program(d, 0x500, 1u | 2u << 5); /* version 2: no SPH */
    program(d, 0x708, sph(2, 5));    /* 8 past a multiple of 16, as an executable's array of bytes may be */
    put32(d + 0x600, 0x12345679u);   /* a compute program's wrapper: not a graphics one */
    s.base = d;
    CHECK(shipped_programs(d, sizeof d, seen, &s) == 5);
    CHECK(s.n == 5);
    CHECK(s.stage[0] == ST_VS && s.at[0] == 0x30);
    CHECK(s.stage[1] == ST_FS && s.at[1] == 0x130);
    CHECK(s.stage[2] == SHIPPED_VERTEX_A && s.at[2] == 0x230);
    CHECK(s.stage[3] == ST_GS && s.at[3] == 0x330);
    CHECK(s.stage[4] == ST_FS && s.at[4] == 0x738);
    /* each's 1 stops the scan after that program */
    memset(&s, 0, sizeof s);
    s.base = d;
    s.stop_after = 2;
    CHECK(shipped_programs(d, sizeof d, seen, &s) == 1);
    CHECK(s.n == 2);
    /* a program whose SPH would run past the end isn't one */
    memset(&s, 0, sizeof s);
    s.base = d;
    CHECK(shipped_programs(d, 0x30 + 0x40, seen, &s) == 0);
}

static void bare_file(void) {
    static u8 d[0xB50 + 0x100];
    Seen s = {0};
    memset(d, 0, sizeof d);
    put32(d, 0x19866891u);
    put32(d + 0xB50, sph(2, 5));
    program(d, 0x400, sph(1, 1)); /* inside the control section: a bare file holds one program, at 0xB50 */
    s.base = d;
    CHECK(shipped_kind(d, SHIPPED_HEAD) == SHIPPED_SCAN);
    CHECK(shipped_programs(d, sizeof d, seen, &s) == 1);
    CHECK(s.n == 1 && s.stage[0] == ST_FS && s.at[0] == 0xB50);
}

/* A BNSH at 0x100 of 0x3000 bytes: a code record names the program at 0x1000 (0x200 bytes of code area, its control
   section at 0x200 puts its constants at 0x100 of them), another names the one at 0x1200 with a control section whose
   code area size disagrees, and the one at 0x1400 has no record. Only the first is handed its constants */
static void bnsh(void) {
    static u8 d[0x3100];
    const size_t b = 0x100;
    Seen s = {0};
    memset(d, 0, sizeof d);
    memcpy(d + b, "BNSH", 4);
    put32(d + b + 0x1C, 0x3000);
    put64(d + b + 0x108, 0x200); /* the record at 0x100: control section, code area, its bytes */
    put64(d + b + 0x110, 0x1000);
    put32(d + b + 0x118, 0x200);
    put32(d + b + 0x200, 0x98761234u);
    put32(d + b + 0x200 + 0x700, 0x100);
    put32(d + b + 0x200 + 0x704, 0x200);
    put32(d + b + 0x200 + 0x708, 0x30);
    put64(d + b + 0x148, 0xA00); /* the record at 0x140 */
    put64(d + b + 0x150, 0x1200);
    put32(d + b + 0x158, 0x200);
    put32(d + b + 0xA00, 0x98761234u);
    put32(d + b + 0xA00 + 0x700, 0x100);
    put32(d + b + 0xA00 + 0x704, 0x300);
    put32(d + b + 0xA00 + 0x708, 0x30);
    program(d, b + 0x1000, sph(1, 1));
    program(d, b + 0x1200, sph(2, 5));
    program(d, b + 0x1400, sph(2, 5));
    s.base = d;
    CHECK(shipped_programs(d, sizeof d, seen, &s) == 3);
    CHECK(s.at[0] == b + 0x1030 && s.consts[0] == b + 0x1100 && s.nconsts[0] == 0x100);
    CHECK(s.at[1] == b + 0x1230 && !s.consts[1] && !s.nconsts[1]);
    CHECK(s.at[2] == b + 0x1430 && !s.consts[2] && !s.nconsts[2]);
}

/* a SARC of two entries: a.bfres (no shaders) and b.bnsh, aligned to 0x100 past the data offset */
static size_t sarc(u8* d, size_t size, const char* second) {
    size_t sfat = 0x14, nodes = sfat + 0xC, sfnt = nodes + 2 * 16, names = sfnt + 8, data = 0x100;
    memset(d, 0, size);
    memcpy(d, "SARC", 4);
    put16(d + 4, 0x14);
    put16(d + 6, 0xFEFF);
    put32(d + 8, (u32)size);
    put32(d + 0xC, (u32)data);
    put16(d + 0x10, 0x100);
    memcpy(d + sfat, "SFAT", 4);
    put16(d + sfat + 4, 0xC);
    put16(d + sfat + 6, 2);
    put32(d + sfat + 8, 0x65);
    put32(d + nodes + 4, 0x01000000u); /* named, at 0 */
    put32(d + nodes + 8, 0);
    put32(d + nodes + 12, 0x40);
    put32(d + nodes + 16 + 4, 0x01000000u | 2u); /* named, at 8 */
    put32(d + nodes + 16 + 8, 0x100);
    put32(d + nodes + 16 + 12, 0x300);
    memcpy(d + sfnt, "SFNT", 4);
    put16(d + sfnt + 4, 8);
    memcpy(d + names, "a.bfres", 8);
    memcpy(d + names + 8, second, strlen(second) + 1);
    memcpy(d + data + 0x100, "BNSH", 4);
    program(d, data + 0x200, sph(2, 5));
    return data + 0x300;
}
static void archive(void) {
    static u8 d[0x400];
    size_t n = sarc(d, sizeof d, "b.bnsh"), off, size;
    const char* name;
    Seen s = {0};
    CHECK(n <= sizeof d);
    CHECK(shipped_kind(d, SHIPPED_HEAD) == SHIPPED_SCAN);
    CHECK(shipped_sarc_head(d, n) == 0x100);
    CHECK(shipped_sarc_count(d, n) == 2);
    CHECK(!shipped_sarc_entry(d, n, 1, &off, &size, &name));
    CHECK(off == 0x200 && size == 0x200 && !strcmp(name, "b.bnsh"));
    CHECK(shipped_sarc_entry(d, n, 2, &off, &size, &name) == -1);
    CHECK(shipped_sarc_entry(d, 0x150, 1, &off, &size, &name) == -1); /* its data past what was read */
    CHECK(shipped_sarc_wants(d, n) == 1);
    CHECK(shipped_sarc_wants(d, 0x100) == 1);         /* its tables alone, as a Yaz0 one is read */
    CHECK(shipped_sarc_wants(d, 0x30) == -1);         /* its tables past what was read */
    put32(d + 0x14 + 0xC + 16 + 4, 0x02000000u | 2u); /* a name after one that hashes alike: still a name */
    CHECK(!shipped_sarc_entry(d, n, 1, &off, &size, &name) && !strcmp(name, "b.bnsh"));
    CHECK(shipped_sarc_wants(d, n) == 1);
    s.base = d;
    CHECK(shipped_programs(d, n, seen, &s) == 1 && s.at[0] == 0x330); /* scanned as it is */
    sarc(d, sizeof d, "b.bflyt");
    CHECK(shipped_sarc_wants(d, n) == 0);
    sarc(d, sizeof d, "b.szs"); /* an archive in it: not followed */
    CHECK(shipped_sarc_wants(d, n) == 0);
    sarc(d, sizeof d, "b.sharcb");
    CHECK(shipped_sarc_wants(d, n) == 1);
}

/* a Yaz0 stream of d's n bytes, every byte a literal: a flag byte of all ones before each eight */
static size_t yaz0_literals(const u8* d, size_t n, u8* out) {
    size_t at = 16, i;
    memcpy(out, "Yaz0", 4);
    be32(out + 4, (u32)n);
    memset(out + 8, 0, 8);
    for (i = 0; i < n; i++) {
        if (i % 8 == 0) out[at++] = 0xFF;
        out[at++] = d[i];
    }
    return at;
}
static void yaz0(void) {
    static u8 d[0x400], z[0x500], back[0x400];
    size_t n = sarc(d, sizeof d, "b.bnsh"), zn = yaz0_literals(d, n, z);
    CHECK(shipped_kind(z, SHIPPED_HEAD) == SHIPPED_YAZ0);
    CHECK(shipped_yaz0_size(z, zn) == n);
    CHECK(shipped_yaz0(z, zn, back, n) == n && !memcmp(back, d, n));
    CHECK(shipped_yaz0(z, zn, back, 0x20) == 0x20); /* a part, from the start */
    /* decompressed as far as its tables, as the walk does: they say it holds a shader container */
    CHECK(shipped_yaz0(z, zn, back, shipped_sarc_head(d, n)) == 0x100 && shipped_sarc_wants(back, 0x100) == 1);
    CHECK(shipped_yaz0(z, 0x30, back, n) < n); /* the stream cut short: what it holds */
    {
        /* "ab", then a copy of 10 from 2 back (two bytes: count 10 - 2 = 8) and one of 0x14 from 1 back (three
           bytes: count 0x14 - 0x12 = 2): copies that read what they write */
        static const u8 s[] = {'Y', 'a', 'z', '0', 0,    0,   0,   32,   0,    0,    0,    0,
                               0,   0,   0,   0,   0xC0, 'a', 'b', 0x80, 0x01, 0x00, 0x00, 0x02};
        static const char want[] = "ababababababbbbbbbbbbbbbbbbbbbbb";
        u8 out[32];
        CHECK(shipped_yaz0(s, sizeof s, out, sizeof out) == 32 && !memcmp(out, want, 32));
        /* a copy from before the start is no Yaz0: it stops there */
        static const u8 bad[] = {'Y', 'a', 'z', '0', 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0x80, 'a', 0x10, 0x05};
        CHECK(shipped_yaz0(bad, sizeof bad, out, 8) == 1);
    }
}

/* an Ounce program at d + at: its 128-byte header of version 4 (type, shader type), n instructions that look like
   SM70+ code (their control bits set), then EXIT and the branch to itself that ends every program */
static size_t ounce_program(u8* d, size_t at, u32 type, u32 shader, u32 n) {
    put32(d + at, type | 4u << 5 | shader << 10);
    size_t i = at + 0x80;
    for (u32 k = 0; k < n; k++, i += 16) {
        put64(d + i, 0x0000000000007802ull + k); /* a MOV's opcode, any operands */
        put64(d + i + 8, 0x000fe20000000000ull);
    }
    put64(d + i, 0x000000000000794dull); /* EXIT */
    put64(d + i + 8, 0x000fea0003800000ull);
    put64(d + i + 16, 0xfffffff000007947ull); /* BRA to itself */
    put64(d + i + 24, 0x000fc0000383ffffull);
    return i + 32;
}
static void ounce_programs(void) {
    static u8 d[0x1000];
    memset(d, 0, sizeof d);
    ounce_program(d, 0x100, 1, 1, 5); /* a vertex program */
    ounce_program(d, 0x400, 2, 5, 9); /* a fragment program */
    /* an EXIT and a branch to itself with no header before the code: no program */
    put64(d + 0x800, 0x000000000000794dull);
    put64(d + 0x808, 0x000fea0003800000ull);
    put64(d + 0x810, 0xfffffff000007947ull);
    put64(d + 0x818, 0x000fc0000383ffffull);
    Seen s = {0};
    s.base = d;
    CHECK(shipped_ounce_programs(d, sizeof d, seen, &s) == 2);
    CHECK(s.n == 2);
    CHECK(s.at[0] == 0x100 && s.stage[0] == (SHIPPED_OUNCE | ST_VS));
    CHECK(s.at[1] == 0x400 && s.stage[1] == (SHIPPED_OUNCE | ST_FS));
    /* the Switch 1 search finds none of them */
    Seen m = {0};
    m.base = d;
    CHECK(shipped_programs(d, sizeof d, seen, &m) == 0);
    /* a zstd stream, and the names an Ounce game's walk reads whatever their first bytes say */
    u8 zs[16] = {0x28, 0xb5, 0x2f, 0xfd};
    CHECK(shipped_kind(zs, sizeof zs) == SHIPPED_ZSTD);
    CHECK(shipped_name_ounce("Shader/Mii.bnsh.zs") == 1);
    CHECK(shipped_name_ounce("a/shader_pack.bin") == 2);
    CHECK(shipped_name_ounce("model.bfres.zs") == 0);
    CHECK(shipped_name_ounce("tex/a.Nin_Ounce_NVN2.bntx.zs") == 0); /* the platform's tag alone says nothing */
}
static void names(void) {
    CHECK(shipped_name_skips("BGM_Title.bfstm"));
    CHECK(shipped_name_skips("course_model.bfres"));
    CHECK(shipped_name_skips("a.bars"));
    CHECK(!shipped_name_skips("course_model.szs"));
    CHECK(!shipped_name_skips("agl_resource.sarc"));
    CHECK(!shipped_name_skips("Mii.bfsha"));
    CHECK(!shipped_name_skips("shader"));  /* a name with no extension: its bytes say */
    CHECK(!shipped_name_skips("x.barsx")); /* the whole extension, not its start */
}

int main(void) {
    names();
    programs();
    bare_file();
    bnsh();
    archive();
    yaz0();
    ounce_programs();
    if (failures) return 1;
    puts("shipped shaders: ok");
    return 0;
}
