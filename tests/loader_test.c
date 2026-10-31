/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/loader/loader.h"
#include "nx/runtime/driver/game_name.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static char* scratch;
static unsigned char image_bytes[1024], file_bytes[4096], baseline[4096];

static void check(int condition, const char* expression, unsigned line) {
    checks++;
    if (!condition) {
        fprintf(stderr, "loader test line %u: %s (scratch %s)\n", line, expression, scratch ? scratch : "none");
        exit(1);
    }
}
#define CHECK(x) check(!!(x), #x, __LINE__)

static void put32(unsigned char* p, SnxU32 value) {
    unsigned i;
    for (i = 0; i < 4; i++) p[i] = (unsigned char)(value >> (i * 8));
}

static void put64(unsigned char* p, SnxU64 value) {
    put32(p, (SnxU32)value);
    put32(p + 4, (SnxU32)(value >> 32));
}

static void dynamic(unsigned index, unsigned tag, SnxU64 value) {
    put64(image_bytes + 0x100 + index * 16, tag);
    put64(image_bytes + 0x108 + index * 16, value);
}

static void rich_image(void) {
    unsigned i, at;
    memset(image_bytes, 0, sizeof image_bytes);
    put32(image_bytes + 4, 0x80);
    put32(image_bytes + 0x20, 0xd65f03c0UL);
    put32(image_bytes + 0x40, 0xd65f03c0UL);
    put32(image_bytes + 0x50, 0xd65f03c0UL);
    memcpy(image_bytes + 0x80, "MOD0", 4);
    put32(image_bytes + 0x84, 0x100 - 0x80);
    put32(image_bytes + 0x88, 0x400 - 0x80);
    put32(image_bytes + 0x8c, 0x440 - 0x80);
    put32(image_bytes + 0x90, 0x280 - 0x80);
    put32(image_bytes + 0x94, 0x29c - 0x80);
    dynamic(0, 4, 0x220);
    dynamic(1, 5, 0x200);
    dynamic(2, 6, 0x1a0);
    dynamic(3, 10, 0x20);
    dynamic(4, 11, 24);
    dynamic(5, 7, 0x240);
    dynamic(6, 8, 48);
    memcpy(image_bytes + 0x200, "\0old\0entry\0imported\0", 20);
    for (i = 1; i <= 3; i++) {
        at = 0x1a0 + i * 24;
        put32(image_bytes + at, i == 1 ? 1 : i == 2 ? 5 : 11);
        image_bytes[at + 4] = 2;
        image_bytes[at + 6] = i == 3 ? 0 : 1;
        put64(image_bytes + at + 8, i == 3 ? 0 : 0x20);
        put64(image_bytes + at + 16, 0x10);
    }
    put32(image_bytes + 0x220, 1);
    put32(image_bytes + 0x224, 4);
    for (i = 0; i < 2; i++) {
        put64(image_bytes + 0x240 + i * 24, 0x390 + i * 8);
        put64(image_bytes + 0x248 + i * 24, 1027);
        put64(image_bytes + 0x250 + i * 24, 0x30);
    }
    image_bytes[0x280] = 1;
    image_bytes[0x281] = 0x1b;
    image_bytes[0x282] = 3;
    image_bytes[0x283] = 0x3b;
    put32(image_bytes + 0x284, 0x2c0 - 0x284);
    put32(image_bytes + 0x288, 2);
    for (i = 0; i < 2; i++) {
        put32(image_bytes + 0x28c + i * 8, (SnxU32)(0x40 + i * 16) - 0x280);
        put32(image_bytes + 0x290 + i * 8, 0x2e0 + i * 0x20 - 0x280);
        at = 0x2e0 + i * 0x20;
        put32(image_bytes + at, 16);
        put32(image_bytes + at + 4, at + 4 - 0x2c0);
        put32(image_bytes + at + 8, (SnxU32)(0x40 + i * 16) - (at + 8));
        put32(image_bytes + at + 12, 16);
    }
    put32(image_bytes + 0x2c0, 16);
    image_bytes[0x2c8] = 1;
    memcpy(image_bytes + 0x2c9, "zR", 3);
    image_bytes[0x2cc] = 1;
    image_bytes[0x2cd] = 0x78;
    image_bytes[0x2ce] = 30;
    image_bytes[0x2cf] = 1;
    image_bytes[0x2d0] = 0x1b;
}

static size_t literal_block(unsigned char* out, const unsigned char* in, size_t size) {
    size_t used, rest;
    used = 1;
    out[0] = (unsigned char)((size < 15 ? size : 15) << 4);
    if (size >= 15) {
        rest = size - 15;
        while (rest >= 255) {
            out[used++] = 255;
            rest -= 255;
        }
        out[used++] = (unsigned char)rest;
    }
    memcpy(out + used, in, size);
    return used + size;
}

static size_t build_nso(unsigned text_size, unsigned ro_off, unsigned ro_size, unsigned data_off, unsigned data_size,
                        unsigned compressed) {
    unsigned offsets[3], sizes[3], i;
    size_t at, stored;
    memset(file_bytes, 0, sizeof file_bytes);
    memcpy(file_bytes, "NSO0", 4);
    put32(file_bytes + 12, compressed);
    put32(file_bytes + 0x3c, 0x40);
    for (i = 0; i < 32; i++) file_bytes[0x40 + i] = (unsigned char)i;
    offsets[0] = 0;
    offsets[1] = ro_off;
    offsets[2] = data_off;
    sizes[0] = text_size;
    sizes[1] = ro_size;
    sizes[2] = data_size;
    at = 0x100;
    for (i = 0; i < 3; i++) {
        put32(file_bytes + 0x10 + i * 16, (SnxU32)at);
        put32(file_bytes + 0x14 + i * 16, offsets[i]);
        put32(file_bytes + 0x18 + i * 16, sizes[i]);
        if (compressed & (1U << i))
            stored = literal_block(file_bytes + at, image_bytes + offsets[i], sizes[i]);
        else {
            memcpy(file_bytes + at, image_bytes + offsets[i], sizes[i]);
            stored = sizes[i];
        }
        put32(file_bytes + 0x60 + i * 4, (SnxU32)stored);
        at += stored;
    }
    return at;
}

static char* fixture(const char* name, const void* bytes, size_t size) {
    char* path;
    path = snx_path_join(scratch, name);
    CHECK(path != NULL);
    CHECK(!snx_write_file(path, bytes, size));
    return path;
}

static void expect_rich(const SnxModuleImage* m) {
    CHECK(m->image_size == 1024 && !memcmp(m->image, image_bytes, 1024));
    CHECK(m->text_off == 0 && m->text_size == 0x100 && m->ro_off == 0x100 && m->ro_size == 0x280);
    CHECK(m->data_off == 0x380 && m->data_size == 0x80 && m->bss == 0x40 && m->size == 0x1000);
    CHECK(m->build_id[0] == 0 && m->build_id[31] == 31);
    CHECK(m->has_mod0 && m->mod0.dynamic == 0x100 && m->mod0.bss_start == 0x400 && m->mod0.bss_end == 0x440);
    CHECK(m->symbol_count == 1 && m->symbols[0].value == 0x20 && !strcmp(m->symbols[0].name, "entry"));
    CHECK(m->import_count == 1 && !strcmp(m->imports[0], "imported"));
    CHECK(m->function_count == 3 && m->functions[0] == 0x20 && m->functions[1] == 0x40 && m->functions[2] == 0x50);
    CHECK(m->routed_count == 0); /* entry is exported, but no hook names it: its calls stay direct */
    CHECK(m->relptr_count == 1 && m->relptrs[0] == 0x30);
    CHECK(m->fde_count == 2 && m->fde[0].start == 0x40 && m->fde[0].size == 16 && m->fde[1].start == 0x50);
    CHECK(m->eh_error == NULL);
}

static void test_nso(void) {
    SnxModuleImage m;
    char error[256], *path;
    size_t size, n;
    unsigned compressed;
    rich_image();
    for (compressed = 0; compressed < 8; compressed++) {
        size = build_nso(0x100, 0x100, 0x280, 0x380, 0x80, compressed);
        path = fixture("fixture.nso", file_bytes, size);
        CHECK(!snx_load_nso(path, "main", &m, error, sizeof error));
        expect_rich(&m);
        snx_free_module(&m);
        free(path);
    }
    size = build_nso(0x100, 0x100, 0x280, 0x380, 0x80, 0);
    memcpy(baseline, file_bytes, size);
    path = snx_path_join(scratch, "fixture.nso");
    for (n = 0; n < size; n++) {
        CHECK(!snx_write_file(path, baseline, n));
        CHECK(snx_load_nso(path, NULL, &m, error, sizeof error) == -1);
        CHECK(m.image == NULL && m.symbol_count == 0 && error[0]);
    }
    memcpy(file_bytes, baseline, size);
    put32(file_bytes + 0x100 + 0x298, 0x7fffffffUL);
    CHECK(!snx_write_file(path, file_bytes, size));
    CHECK(!snx_load_nso(path, "partial", &m, error, sizeof error));
    CHECK(m.eh_error && m.fde_count == 1 && m.function_count == 2 && m.functions[1] == 0x40);
    snx_free_module(&m);
    /* DT_RELR at 0x320: the word at 0x3a0, then a bitmap of the words from 0x3a8 marking 0x3a8 and 0x3b8. The text
       pointers among them (0x20, 0x60) join RELA's 0x30, the one into the data (0x200) doesn't */
    memcpy(file_bytes, baseline, size);
    put64(file_bytes + 0x100 + 0x170, 35);
    put64(file_bytes + 0x100 + 0x178, 16);
    put64(file_bytes + 0x100 + 0x180, 36);
    put64(file_bytes + 0x100 + 0x188, 0x320);
    put64(file_bytes + 0x100 + 0x320, 0x3a0);
    put64(file_bytes + 0x100 + 0x328, 1 | 1 << 1 | 1 << 3);
    put64(file_bytes + 0x100 + 0x3a0, 0x20);
    put64(file_bytes + 0x100 + 0x3a8, 0x60);
    put64(file_bytes + 0x100 + 0x3b8, 0x200);
    CHECK(!snx_write_file(path, file_bytes, size));
    CHECK(!snx_load_nso(path, "relr", &m, error, sizeof error));
    CHECK(m.relptr_count == 3 && m.relptrs[0] == 0x20 && m.relptrs[1] == 0x30 && m.relptrs[2] == 0x60);
    snx_free_module(&m);
    put64(file_bytes + 0x100 + 0x178, 0x100);
    CHECK(!snx_write_file(path, file_bytes, size));
    CHECK(snx_load_nso(path, NULL, &m, error, sizeof error) == -1);
    CHECK(strstr(error, "RELR table") != NULL && !m.image);
    memcpy(file_bytes, baseline, size);
    put64(file_bytes + 0x100 + 0x148, 0);
    CHECK(!snx_write_file(path, file_bytes, size));
    CHECK(snx_load_nso(path, NULL, &m, error, sizeof error) == -1);
    CHECK(strstr(error, "symbol entry") != NULL && !m.image);
    memcpy(file_bytes, baseline, size);
    put32(file_bytes + 0x100 + 0x1b8, 0xffffffffUL);
    CHECK(!snx_write_file(path, file_bytes, size));
    CHECK(snx_load_nso(path, NULL, &m, error, sizeof error) == -1);
    CHECK(strstr(error, "symbol name") != NULL);
    memset(error, 0x7f, sizeof error);
    CHECK(!snx_write_file(path, "bad", 3));
    CHECK(snx_load_nso(path, NULL, &m, error, 3) == -1);
    CHECK(error[2] == 0 && error[3] == 0x7f);
    free(path);
    puts("PASS NSO segments, LZ4 variants, symbols/imports, RELA and RELR relocations, unwind and truncated bounds");
}

static unsigned nibble(char c) { return c <= '9' ? (unsigned)(c - '0') : (unsigned)(c - 'a' + 10); }

static void digest_bytes(unsigned char* out, const char* hex) {
    unsigned i;
    CHECK(strlen(hex) == 64);
    for (i = 0; i < 32; i++) out[i] = (unsigned char)((nibble(hex[i * 2]) << 4) | nibble(hex[i * 2 + 1]));
}

static void test_hash_lz4(void) {
    static const char* messages[3] = {"abc", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                                      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
    static const char* hashes[3] = {"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                                    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
                                    "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"};
    /* "ABCD", a 4-byte match 4 back, then the 8 literals an LZ4 block has to end with: the last match ends at least 5
       bytes and starts at least 12 bytes before the end of the block, which LZ4_decompress_safe holds a block to */
    static const unsigned char matched[16] = {0x40, 'A', 'B', 'C', 'D', 4,   0,   0x80,
                                              'E',  'F', 'G', 'H', 'I', 'J', 'K', 'L'};
    SnxModuleImage m;
    char error[256], *path;
    size_t size, length;
    unsigned i;
    for (i = 0; i < 3; i++) {
        memset(image_bytes, 0, sizeof image_bytes);
        length = strlen(messages[i]);
        memcpy(image_bytes, messages[i], length);
        size = build_nso((unsigned)length, (unsigned)length, 0, 128, 0, 1);
        put32(file_bytes + 12, 0x19);
        digest_bytes(file_bytes + 0xa0, hashes[i]);
        digest_bytes(file_bytes + 0xc0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        path = fixture("hash.nso", file_bytes, size);
        CHECK(!snx_load_nso(path, NULL, &m, error, sizeof error));
        CHECK(!memcmp(m.image, messages[i], length));
        snx_free_module(&m);
        file_bytes[0xa0] ^= 1;
        CHECK(!snx_write_file(path, file_bytes, size));
        CHECK(snx_load_nso(path, NULL, &m, error, sizeof error) == -1 && strstr(error, "SHA-256"));
        free(path);
    }
    memset(image_bytes, 0, sizeof image_bytes);
    build_nso(16, 16, 0, 16, 0, 0);
    put32(file_bytes + 12, 1);
    put32(file_bytes + 0x60, sizeof matched);
    put32(file_bytes + 0x20, 0x100 + sizeof matched);
    put32(file_bytes + 0x30, 0x100 + sizeof matched);
    memcpy(file_bytes + 0x100, matched, sizeof matched);
    size = 0x100 + sizeof matched;
    path = fixture("match.nso", file_bytes, size);
    CHECK(!snx_load_nso(path, NULL, &m, error, sizeof error));
    CHECK(!memcmp(m.image, "ABCDABCDEFGHIJKL", 16));
    snx_free_module(&m);
    /* a match reaching before the start of the segment (LZ4 decodes an offset of 0 in bounds, and leaves the
       content to the segment's hash) */
    file_bytes[0x105] = 5;
    CHECK(!snx_write_file(path, file_bytes, size));
    CHECK(snx_load_nso(path, NULL, &m, error, sizeof error) == -1 && strstr(error, "LZ4"));
    file_bytes[0x105] = 4;
    file_bytes[0x100] = 0x4f;
    CHECK(!snx_write_file(path, file_bytes, size));
    CHECK(snx_load_nso(path, NULL, &m, error, sizeof error) == -1 && strstr(error, "LZ4"));
    free(path);
    puts("PASS SHA-256 known answers, corrupt hashes and LZ4 back-reference bounds");
}

static size_t build_nro(void) {
    rich_image();
    memcpy(file_bytes, image_bytes, sizeof image_bytes);
    memcpy(file_bytes + 0x10, "NRO0", 4);
    put32(file_bytes + 0x18, 0x400);
    put32(file_bytes + 0x20, 0);
    put32(file_bytes + 0x24, 0x100);
    put32(file_bytes + 0x28, 0x100);
    put32(file_bytes + 0x2c, 0x280);
    put32(file_bytes + 0x30, 0x380);
    put32(file_bytes + 0x34, 0x80);
    put32(file_bytes + 0x38, 0x40);
    memset(file_bytes + 0x400, 0, 0x40);
    memcpy(file_bytes + 0x400, "ASET", 4);
    put64(file_bytes + 0x428, 0x38);
    put64(file_bytes + 0x430, 5);
    memcpy(file_bytes + 0x438, "ROMFS", 5);
    return 0x43d;
}

static void test_nro_npdm(void) {
    SnxModuleImage m;
    SnxProgram program;
    SnxNpdm npdm;
    unsigned char meta[0xa0];
    char error[256], *path;
    size_t size, i;
    static const unsigned offsets[] = {0x20, 0x24, 0x28, 0x30, 0x34, 0x1c};
    static const unsigned values[] = {4, 0x80, 0x104, 0x384, 0x7c, 2};
    size = build_nro();
    path = fixture("sample.NRO", file_bytes, size);
    CHECK(!snx_load_program(path, &program, error, sizeof error));
    CHECK(program.module_count == 1 && program.modules[0].is_nro && !strcmp(program.modules[0].name, "main"));
    CHECK(program.modules[0].base == SNX_CODE_BASE && program.romfs_size == 5 &&
          !memcmp(file_bytes + program.romfs_offset, "ROMFS", 5));
    CHECK(program.npdm.stack == 0x100000 && program.npdm.prio == 44 && program.npdm.is64);
    CHECK(program.npdm.title_id == 0x0100000000001000ull);
    snx_free_program(&program);
    /* ASET is optional and never part of the executable mapping. */
    CHECK(!snx_write_file(path, file_bytes, 0x400));
    CHECK(!snx_load_nro(path, &m, error, sizeof error));
    CHECK(!m.nacp_size && !m.romfs_size && m.image_size == 0x400 && m.size == 0x1000);
    snx_free_module(&m);
    for (i = 0; i < 0x80; ++i) {
        CHECK(!snx_write_file(path, file_bytes, i));
        CHECK(snx_load_nro(path, &m, error, sizeof error) == -1 && !m.image);
    }
    for (i = 0; i < sizeof offsets / sizeof *offsets; ++i) {
        size = build_nro();
        put32(file_bytes + offsets[i], values[i]);
        CHECK(!snx_write_file(path, file_bytes, size));
        CHECK(snx_load_nro(path, &m, error, sizeof error) == -1 && !m.image && strstr(error, "NRO"));
    }
    size = build_nro();
    put64(file_bytes + 0x430, (SnxU64)-1);
    CHECK(!snx_write_file(path, file_bytes, size));
    CHECK(snx_load_nro(path, &m, error, sizeof error) == -1 && strstr(error, "ASET"));
    size = build_nro();
    put32(file_bytes + 0x18, 0xffffffffUL);
    CHECK(!snx_write_file(path, file_bytes, size));
    CHECK(snx_load_nro(path, &m, error, sizeof error) == -1 && !m.image);
    free(path);
    memset(meta, 0, sizeof meta);
    memcpy(meta, "META", 4);
    meta[12] = 7;
    meta[14] = 37;
    meta[15] = 2;
    put32(meta + 0x1c, 0x200000);
    memcpy(meta + 0x20, "Synthetic Loader", 16);
    put32(meta + 0x70, 0x80);
    put32(meta + 0x74, 0x20);
    memcpy(meta + 0x80, "ACI0", 4);
    put64(meta + 0x90, 0x123456789abcdef0ull);
    CHECK(!snx_parse_npdm(meta, sizeof meta, &npdm, error, sizeof error));
    CHECK(npdm.stack == 0x200000 && npdm.prio == 37 && npdm.core == 2 && npdm.aspace == 3 && npdm.is64);
    CHECK(!strcmp(npdm.name, "Synthetic Loader") && npdm.title_id == 0x123456789abcdef0ull);
    path = fixture("npdm.bin", meta, sizeof meta);
    free(path);
    CHECK(snx_parse_npdm(meta, 0x7f, &npdm, error, sizeof error) == -1 && !npdm.stack);
    put32(meta + 0x70, 0xfffffff0UL);
    CHECK(snx_parse_npdm(meta, sizeof meta, &npdm, error, sizeof error) == -1);
    CHECK(!snx_parse_npdm(NULL, 0, &npdm, error, sizeof error) && !strcmp(npdm.name, "Application"));
    puts("PASS NRO assets/program loading and bounded NPDM metadata");
}

/* the name a game is shown by: an NRO's control.nacp in its assets, an ExeFS's beside it, American English's name,
   else the first language's that has one, trimmed and cut whole at a UTF-8 sequence; --name over it, the title
   without it. The files keep the title ID */
static void test_nacp(const char* root, const char* exefs) {
    static unsigned char nacp[0x600];
    char name[32], error[256], *path, *shown;
    SnxProgram program;
    memset(nacp, 0, sizeof nacp);
    memcpy(nacp + 0x300, "  Synth\xc3\xa9tique ", 15); /* the second language's; American English's is empty */
    CHECK(snx_nacp_name(nacp, sizeof nacp, name, sizeof name) && !strcmp(name, "Synth\xc3\xa9tique"));
    CHECK(snx_nacp_name(nacp, sizeof nacp, name, 9) && !strcmp(name, "Synth")); /* "  Synth\xc3": the \xc3 goes */
    CHECK(!snx_nacp_name(nacp, 0x300, name, sizeof name) && !name[0]);
    CHECK(!snx_nacp_name(NULL, 0, name, sizeof name) && !name[0]);
    memcpy(nacp, "First\x01", 6); /* American English's, a control character left out */
    CHECK(snx_nacp_name(nacp, sizeof nacp, name, sizeof name) && !strcmp(name, "First"));
    /* in an NRO's assets, after its RomFS */
    build_nro();
    put64(file_bytes + 0x418, 0x40);
    put64(file_bytes + 0x420, sizeof nacp);
    memcpy(file_bytes + 0x440, nacp, sizeof nacp);
    path = fixture("named.nro", file_bytes, 0x440 + sizeof nacp);
    CHECK(!snx_load_program(path, &program, error, sizeof error));
    CHECK(program.nacp_size == sizeof nacp && !memcmp(program.nacp, nacp, sizeof nacp) && program.romfs_size == 5);
    shown = snx_game_shown(NULL, &program);
    CHECK(!strcmp(shown, "First"));
    free(shown);
    shown = snx_game_shown("My Game", &program);
    CHECK(!strcmp(shown, "My Game"));
    free(shown);
    snx_free_program(&program);
    free(path);
    /* beside an ExeFS, where the extraction puts it; none: the title */
    CHECK(!snx_load_program(exefs, &program, error, sizeof error) && !program.nacp);
    shown = snx_game_shown(NULL, &program);
    CHECK(!strcmp(shown, "Synthetic Loader"));
    free(shown);
    snx_free_program(&program);
    path = snx_path_join(root, "control.nacp");
    CHECK(!snx_write_file(path, nacp, sizeof nacp));
    CHECK(!snx_load_program(exefs, &program, error, sizeof error) && program.nacp_size == sizeof nacp);
    shown = snx_game_shown(NULL, &program);
    CHECK(!strcmp(shown, "First"));
    free(shown);
    shown = snx_game_name(NULL, &program.npdm, program.entry_path);
    CHECK(!strcmp(shown, "123456789ABCDEF0")); /* the files: the title ID still */
    free(shown);
    snx_free_program(&program);
    CHECK(!snx_remove_tree(path));
    free(path);
    puts("PASS the name in an NRO's or an ExeFS's control.nacp");
}

/* snx_load_npdm finds the NPDM snx_load_program takes, without the modules; snx_game_name names the game's files by
   its title ID when it has one of its own, else as it is shown */
static void test_npdm_of(const char* root, const char* exefs) {
    SnxNpdm npdm;
    char error[256], *path, *name, *nro;
    size_t size;
    CHECK(!snx_load_npdm(root, &npdm, error, sizeof error) && npdm.prio == 37);
    CHECK(!snx_load_npdm(exefs, &npdm, error, sizeof error));
    CHECK(npdm.title_id == 0x123456789abcdef0ull);
    name = snx_game_name(NULL, &npdm, root);
    CHECK(!strcmp(name, "123456789ABCDEF0"));
    free(name);
    name = snx_game_title(NULL, &npdm, root);
    CHECK(!strcmp(name, "Synthetic Loader"));
    free(name);
    name = snx_game_name("My Game", &npdm, root); /* --name: the player's word */
    CHECK(!strcmp(name, "My Game"));
    free(name);
    npdm.title_id = SNX_NPDM_DEFAULT_TITLE; /* an NRO's, or an ExeFS's without NPDM: none of its own */
    name = snx_game_name(NULL, &npdm, root);
    CHECK(!strcmp(name, "Synthetic Loader"));
    free(name);
    npdm.title_id = 0;
    strcpy(npdm.name, "Application");
    name = snx_game_name(NULL, &npdm, "/dumps/Some Game v1.0.nro");
    CHECK(!strcmp(name, "Some Game v1.0"));
    free(name);
    path = snx_path_join(exefs, "main.npdm");
    CHECK(!snx_remove_tree(path));
    CHECK(!snx_load_npdm(root, &npdm, error, sizeof error) && npdm.title_id == SNX_NPDM_DEFAULT_TITLE);
    CHECK(!snx_write_file(path, "META", 4)); /* a broken one is refused, as snx_load_program refuses it */
    CHECK(snx_load_npdm(root, &npdm, error, sizeof error) == -1 && strstr(error, "NPDM"));
    CHECK(!snx_remove_tree(path));
    free(path);
    path = snx_path_join(scratch, "no-exefs");
    CHECK(!snx_mkdirs(path));
    CHECK(snx_load_npdm(path, &npdm, error, sizeof error) == -1 && strstr(error, "exefs/main"));
    free(path);
    size = build_nro();
    nro = fixture("homebrew.nro", file_bytes, size);
    CHECK(!snx_load_npdm(nro, &npdm, error, sizeof error) && npdm.title_id == SNX_NPDM_DEFAULT_TITLE);
    name = snx_game_name(NULL, &npdm, nro);
    CHECK(!strcmp(name, "homebrew"));
    free(name);
    free(nro);
}

/* MOD0's BSS end of a module of several GiB is more than 2 GiB past MOD0: read unsigned where the signed reading would
   land before the module, for that field only */
static void test_large_bss(void) {
    SnxModuleImage m;
    char error[256], *path;
    size_t size;
    rich_image();
    put32(image_bytes + 0x8c, 0x80000000UL);
    size = build_nso(0x100, 0x100, 0x280, 0x380, 0x80, 0);
    put32(file_bytes + 0x3c, 0x80000000UL);
    path = fixture("large.nso", file_bytes, size);
    CHECK(!snx_load_nso(path, NULL, &m, error, sizeof error));
    CHECK(m.mod0.bss_start == 0x400 && m.mod0.bss_end == 0x80000080UL && m.size == 0x80001000UL);
    CHECK(!strcmp(m.name, "large.nso"));
    snx_free_module(&m);
    put32(file_bytes + 0x3c, 0x40); /* the BSS end past the module */
    CHECK(!snx_write_file(path, file_bytes, size));
    CHECK(snx_load_nso(path, NULL, &m, error, sizeof error) == -1 && strstr(error, "BSS") && !m.image);
    put32(file_bytes + 0x3c, 0x80000000UL);
    put32(file_bytes + 0x100 + 0x94, 0x80000000UL); /* any other field stays signed */
    CHECK(!snx_write_file(path, file_bytes, size));
    CHECK(snx_load_nso(path, NULL, &m, error, sizeof error) == -1 && strstr(error, "MOD0"));
    free(path);
    puts("PASS MOD0's BSS end on a module of several GiB");
}

/* A 32-bit program's module: an ELF32 dynamic table without a hash table, 16-byte symbols and REL relocations, code
   addresses keeping their Thumb bit and those ending in binary 10 refused */
static void test_elf32(void) {
    SnxModuleImage m;
    char error[256], *path;
    size_t size;
    unsigned i;
    static const SnxU32 tags[8][2] = {{6, 0x160}, {5, 0x1a0}, {10, 0x10}, {11, 16},
                                      {17, 0x1c0}, {18, 16}, {19, 8}, {0, 0}};
    memset(image_bytes, 0, sizeof image_bytes);
    put32(image_bytes + 4, 0x100);
    memcpy(image_bytes + 0x100, "MOD0", 4);
    put32(image_bytes + 0x104, 0x20);
    put32(image_bytes + 0x108, 0x180);
    put32(image_bytes + 0x10c, 0x180);
    for (i = 0; i < 8; i++) {
        put32(image_bytes + 0x120 + i * 8, tags[i][0]);
        put32(image_bytes + 0x124 + i * 8, tags[i][1]);
    }
    for (i = 1; i < 4; i++) {
        static const SnxU32 names[4] = {0, 1, 7, 11}, values[4] = {0, 0x41, 0x20, 0x22};
        put32(image_bytes + 0x160 + i * 16, names[i]);
        put32(image_bytes + 0x164 + i * 16, values[i]);
        image_bytes[0x16c + i * 16] = i == 1 ? 0x12 : 2;
        image_bytes[0x16e + i * 16] = 1;
    }
    memcpy(image_bytes + 0x1a0, "\0thumb\0arm\0bad\0", 15);
    put32(image_bytes + 0x1c0, 0x200);
    put32(image_bytes + 0x1c4, 23);
    put32(image_bytes + 0x1c8, 0x204);
    put32(image_bytes + 0x1cc, 23);
    put32(image_bytes + 0x200, 0x81);
    put32(image_bytes + 0x204, 0x62);
    size = build_nso(0x100, 0x100, 0x100, 0x200, 0x80, 0);
    path = fixture("arm.nso", file_bytes, size);
    CHECK(!snx_load_nso32(path, "main", &m, error, sizeof error));
    CHECK(m.symbol_count == 3 && m.symbols[0].value == 0x20 && !strcmp(m.symbols[0].name, "arm"));
    CHECK(m.function_count == 2 && m.functions[0] == 0x20 && m.functions[1] == 0x41);
    CHECK(m.relptr_count == 1 && m.relptrs[0] == 0x81 && !m.fde_count && !m.eh_error);
    snx_free_module(&m);
    put32(file_bytes + 0x100 + 0x1c8, 0x2000); /* a target past the module */
    CHECK(!snx_write_file(path, file_bytes, size));
    CHECK(snx_load_nso32(path, "main", &m, error, sizeof error) == -1 && strstr(error, "exceeds module") && !m.image);
    put32(file_bytes + 0x100 + 0x1c8, 0x204);
    put32(file_bytes + 0x100 + 0x13c, 24); /* DT_SYMENT of ELF64 */
    CHECK(!snx_write_file(path, file_bytes, size));
    CHECK(snx_load_nso32(path, "main", &m, error, sizeof error) == -1 && strstr(error, "symbol entry"));
    free(path);
    puts("PASS ELF32 symbols, REL relocations and Thumb code addresses");
}

/* --dump-modules: the images and the index tools/moduledump.py reads */
static void test_dump(const char* root) {
    static const char expected[] = "program 1 1\nmodule rtld 8000000 1000 400\nsymbol 20 10 2 entry\n"
                                   "module main 8002000 1000 400\n";
    char error[256], *out, *path;
    unsigned char* text;
    size_t size;
    out = snx_path_join(scratch, "dump");
    CHECK(!snx_dump_program(root, out, error, sizeof error));
    path = snx_path_join(out, "modules.txt");
    text = snx_read_file(path, &size);
    CHECK(text && size > strlen(expected) && !strncmp((const char*)text, expected, strlen(expected)));
    free(text);
    free(path);
    path = snx_path_join(out, "subsdk2.bin");
    text = snx_read_file(path, &size);
    CHECK(text && size == 1024 && !memcmp(text, image_bytes, 1024));
    free(text);
    free(path);
    CHECK(snx_dump_program(scratch, out, error, sizeof error) == -1 && strstr(error, "exefs/main"));
    free(out);
    puts("PASS the module dump for tools");
}

static void test_program(void) {
    static const char* names[4] = {"rtld", "main", "subsdk2", "sdk"};
    SnxProgram program;
    char error[256], *root, *exefs, *romfs, *path, *npdm_path;
    unsigned char* meta;
    size_t size, meta_size;
    unsigned i;
    root = snx_path_join(scratch, "utf8-\xc3\xa9-\xe6\x97\xa5");
    exefs = snx_path_join(root, "ExeFS");
    romfs = snx_path_join(root, "RomFS");
    CHECK(!snx_mkdirs(exefs) && !snx_mkdirs(romfs));
    rich_image();
    size = build_nso(0x100, 0x100, 0x280, 0x380, 0x80, 7);
    for (i = 0; i < 4; i++) {
        path = snx_path_join(exefs, names[i]);
        CHECK(!snx_write_file(path, file_bytes, size));
        free(path);
    }
    npdm_path = snx_path_join(scratch, "npdm.bin");
    meta = snx_read_file(npdm_path, &meta_size);
    CHECK(meta != NULL);
    free(npdm_path);
    path = snx_path_join(exefs, "main.npdm");
    CHECK(!snx_write_file(path, meta, meta_size));
    free(meta);
    free(path);
    CHECK(!snx_load_program(root, &program, error, sizeof error));
    CHECK(program.module_count == 4 && program.npdm.prio == 37 && program.romfs_path != NULL);
    /* either spelling: on a case-insensitive file system (Windows, macOS) the lower-case one asked first exists */
    CHECK((strstr(program.exefs_path, "ExeFS") || strstr(program.exefs_path, "exefs")) &&
          (strstr(program.romfs_path, "RomFS") || strstr(program.romfs_path, "romfs")));
    for (i = 0; i < 4; i++) {
        CHECK(!strcmp(program.modules[i].name, names[i]));
        CHECK(program.modules[i].base == SNX_CODE_BASE + i * 0x2000);
        expect_rich(&program.modules[i]);
    }
    snx_free_program(&program);
    CHECK(!snx_load_program(exefs, &program, error, sizeof error));
    CHECK(program.module_count == 4 && program.root_path != NULL);
    snx_free_program(&program);
    test_dump(root);
    test_nacp(root, exefs);
    test_npdm_of(root, exefs);
    free(root);
    free(exefs);
    free(romfs);
    puts("PASS UTF-8 game/ExeFS paths, module order/bases and RomFS discovery");
}

static void test_mutations(void) {
    SnxModuleImage module;
    char error[256], *path;
    SnxU32 state;
    size_t size, offset;
    unsigned i, j;
    rich_image();
    size = build_nso(0x100, 0x100, 0x280, 0x380, 0x80, 0);
    memcpy(baseline, file_bytes, size);
    path = snx_path_join(scratch, "mutated.nso");
    state = 0x23498765;
    for (i = 0; i < 512; i++) {
        memcpy(file_bytes, baseline, size);
        for (j = 0; j < 4; j++) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            offset = 0x100 + (state & 1023);
            file_bytes[offset] ^= (unsigned char)(state >> 16);
        }
        CHECK(!snx_write_file(path, file_bytes, size));
        if (!snx_load_nso(path, NULL, &module, error, sizeof error))
            snx_free_module(&module);
        else
            CHECK(error[0] && module.image == NULL && module.symbols == NULL);
    }
    free(path);
    puts("PASS deterministic malformed MOD0/dynamic/unwind mutations");
}

int main(void) {
    scratch = snx_temp_dir(NULL);
    CHECK(scratch != NULL);
    test_nso();
    test_hash_lz4();
    test_nro_npdm();
    test_program();
    test_large_bss();
    test_elf32();
    test_mutations();
    CHECK(!snx_remove_tree(scratch));
    free(scratch);
    scratch = NULL;
    printf("PASS %u native loader checks\n", checks);
    return 0;
}
