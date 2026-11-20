/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* NCZ, the NCAs of nsz's NSZ and XCZ dumps (src/nx/runtime/driver/hactool/ncz.c), made here as nsz writes them from
   synthetic NCAs encrypted with synthetic keys (tests/ncz_synth.h), in blocks of three sizes and as one zstd stream:
   each read as the NCA it holds byte for byte, whole, at its edges and at offsets of any alignment, from memory and
   from a file; a solid stream decoded again from its start only for a read behind it that no page it keeps holds, and
   the line that says so; an encrypted program NCA read through snx_hac.h from an NSP and a gamecard, from the file and
   mapped, and extracted, as its NCA is; and each flaw of a header, a block or a stream refused with where it is and
   why. No dumps or console keys */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(_WIN32) && defined(__clang__)
/* The pinned upstream header redeclares the MinGW CRT's imported fseeko64. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winconsistent-dllimport"
#endif
#include "nx/runtime/driver/hactool/nca.h"
#include "nx/runtime/driver/hactool/hfs0.h"
#include "nx/runtime/driver/hactool/pfs0.h"
#include "nx/runtime/driver/hactool/xci.h"
#include "nx/runtime/driver/hactool/sha.h"
#include "nx/runtime/driver/hactool/storage.h"
#include "nx/runtime/driver/hactool/ncz.h"
#include "nx/runtime/driver/hactool/snx_hac.h"
#if defined(_WIN32) && defined(__clang__)
#pragma clang diagnostic pop
#endif

/* the checks made, the refusals and restarts seen, and the synthetic keys (never a console's) */
static struct {
    unsigned checks, refusals, restarts;
    char restart_line[256];
    unsigned char header_key[0x20], key_area_key[0x10], section_keys[0x40];
} g_test;

static void check(int good, const char* expression, int line) {
    g_test.checks++;
    if (!good) {
        fprintf(stderr, "hactool_ncz.c:%d: check failed: %s\n", line, expression);
        exit(90);
    }
}
#define CHECK(value) check((value) != 0, #value, __LINE__)

#include "hac_synth.h"
#include "ncz_synth.h"

#define NCZ_DIR "hactool_ncz.tmp"
#define BYTES   0x60123 /* the synthetic NCA of the byte checks: its tail ends inside a block of every size */

static uint32_t le32(const unsigned char* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t next(uint32_t* seed) {
    *seed = *seed * 1103515245U + 12345U;
    return *seed >> 8;
}

/* Runs of zeros and text, which zstd compresses, and noise from 0x10000 to 0x20000, which it does not: the blocks
   inside it are kept as they are */
static void fill(unsigned char* p, size_t size, uint32_t seed) {
    for (size_t at = 0, run; at < size; at += run) {
        run = 0x100 + next(&seed) % 0x3000;
        if (run > size - at) run = size - at;
        if (next(&seed) % 2)
            memset(p + at, 0, run);
        else
            for (size_t i = 0; i < run; i++) p[at + i] = (unsigned char)"synthetic text "[(at + i) % 15];
    }
    for (size_t i = 0x10000; i < 0x20000 && i < size; i++) p[i] = (unsigned char)next(&seed);
}

/* The sections of the synthetic NCA, from first to size: AES-CTR, none, an empty AesCtrEx run as nsz ends a patch's
   runs with, two more runs (the second ending off an AES block's edge), then AES-CTR with another key. Each counter
   differs in its last byte, where nsz puts an AesCtrEx run's generation */
static unsigned bytes_parts(ncz_part_t* p, uint64_t first, uint64_t size) {
    static const uint64_t ends[5] = {0xC000, 0x18000, 0x18000, 0x1F010, 0x30008};
    static const uint64_t crypto[6] = {3, 1, 4, 4, 4, 3};
    memset(p, 0, 6 * sizeof *p);
    for (unsigned i = 0; i < 6; i++) {
        p[i].offset = i ? p[i - 1].offset + p[i - 1].size : first;
        p[i].size = (i < 5 ? ends[i] : size) - p[i].offset;
        p[i].crypto = crypto[i];
        for (unsigned k = 0; k < 16; k++) p[i].key[k] = (unsigned char)((i == 5 ? 0x80 : 0x10) + k);
        for (unsigned k = 0; k < 8; k++) p[i].counter[k] = (unsigned char)(0x21 + k);
        p[i].counter[7] = (unsigned char)i;
    }
    return 6;
}

static void reads_as(storage_t* s, const blob_t* nca, uint64_t offset, uint64_t size, unsigned char* got) {
    if (size > nca->size - offset) size = nca->size - offset;
    memset(got, 0xA5, (size_t)size);
    if (!storage_read(s, offset, got, size) || memcmp(got, nca->data + offset, (size_t)size)) {
        fprintf(stderr, "hactool_ncz.c: the NCA read 0x%llx+0x%llx differs (%s)\n", (unsigned long long)offset,
                (unsigned long long)size, snx_hac_message());
        CHECK(0);
    }
}

/* the NCA whole, then across 0x4000, blocks of every size, sections and AES blocks, its last byte, and 200 reads of
   any size at any offset */
static void reads_match(storage_t* s, const blob_t* nca, uint32_t seed) {
    static const uint64_t edges[][2] = {{0, 0x200},   {0x3FF0, 0x20},    {0x3FFF, 2},        {0x4000, 1},
                                        {0x7FFB, 10}, {0xBFF7, 0x11},    {0x17FFF, 2},       {0x1F00D, 7},
                                        {0x30005, 5}, {0xFFF0, 0x10020}, {0x20000, 0x20000}, {0x13FFF, 0x4002}};
    unsigned char* got = zalloc(nca->size);
    CHECK(s->size == nca->size);
    reads_as(s, nca, 0, nca->size, got);
    for (size_t i = 0; i < sizeof edges / sizeof *edges; i++) reads_as(s, nca, edges[i][0], edges[i][1], got);
    reads_as(s, nca, nca->size - 1, 1, got);
    for (unsigned k = 0; k < 200; k++) {
        uint64_t size = next(&seed) % 0x9000 + 1, offset = next(&seed) % nca->size;
        reads_as(s, nca, offset, size, got);
    }
    free(got);
}

/* the synthetic NCA, its sections from first, as an NCZ of blocks of 1 << exponent bytes (0: one stream), read from
   memory and from a file at an odd offset */
static void bytes_match(unsigned exponent, uint64_t first) {
    blob_t nca = {zalloc(BYTES), BYTES}, ncz;
    ncz_part_t parts[6];
    ncz_blocks_t blocks = {0};
    unsigned count = bytes_parts(parts, first, nca.size);
    char why[256] = "";
    unsigned char pad[0x1234] = {0};
    fill(nca.data, nca.size, exponent + 7);
    ncz_encrypt(nca.data, parts, count);
    ncz = ncz_build(&nca, parts, count, exponent, &blocks);
    CHECK(exponent != 14 || (blocks.stored && blocks.compressed)); /* both kinds of block */
    ncz_t* z = ncz_open(storage_memory_new(ncz.data, ncz.size, 0, ncz.size), why, sizeof why);
    if (!z) fprintf(stderr, "hactool_ncz.c: %s\n", why);
    CHECK(z != NULL);
    CHECK(ncz_kind(z) == (exponent ? NCZ_BLOCK : NCZ_SOLID));
    reads_match(ncz_storage(z), &nca, exponent + 1);
    ncz_free(z);
    FILE* file = tmpfile(); /* inside a container, at an offset no NCA starts at */
    CHECK(file != NULL);
    CHECK(fwrite(pad, 1, sizeof pad, file) == sizeof pad && fwrite(ncz.data, 1, ncz.size, file) == ncz.size);
    CHECK(!fflush(file));
    z = ncz_open(storage_file_new(file, sizeof pad, ncz.size), why, sizeof why);
    CHECK(z != NULL);
    reads_match(ncz_storage(z), &nca, exponent + 2);
    ncz_free(z);
    CHECK(!fclose(file));
    printf("hactool ncz: %s, sections from 0x%llx, 0x%zx bytes as an NCA of 0x%zx, read whole and 213 ways\n",
           exponent ? "blocks" : "a solid stream", (unsigned long long)first, ncz.size, nca.size);
    if (exponent)
        printf("  blocks of 2^%u: %u kept as they are, %u compressed\n", exponent, blocks.stored, blocks.compressed);
    free(ncz.data);
    free(nca.data);
}

/* ------------------------------------------------------------------ a solid stream read behind itself */

static void restarted(const char* line, void* data) {
    (void)data;
    g_test.restarts++;
    snprintf(g_test.restart_line, sizeof g_test.restart_line, "%s", line);
}

/* A stream of 33 pages and 16 bytes (a page is 1 MiB, 32 are kept): forward reads decode it once, a read of a page
   kept decodes nothing, and a read of page 0 once page 32 took its place decodes it again from its start, said once;
   the pages after it are ahead of the stream again */
static void solid_restart(void) {
    const uint64_t page = 0x100000;
    blob_t nca = {zalloc(0x4000 + 33 * page + 0x10), 0x4000 + 33 * page + 0x10}, ncz;
    ncz_part_t part = {0};
    unsigned char got[32];
    char why[256] = "";
    part.offset = 0x4000;
    part.size = nca.size - 0x4000;
    part.crypto = 3;
    memset(part.key, 0x6B, 16);
    memset(part.counter, 0x2C, 8);
    for (unsigned k = 0; k < 34; k++) snprintf((char*)nca.data + 0x4000 + k * page, 16, "page %u", k);
    ncz_encrypt(nca.data, &part, 1);
    ncz = ncz_build(&nca, &part, 1, 0, NULL);
    snx_hac_on_restart(restarted, NULL);
    ncz_t* z = ncz_open(storage_memory_new(ncz.data, ncz.size, 0, ncz.size), why, sizeof why);
    CHECK(z != NULL);
    storage_t* s = ncz_storage(z);
    for (unsigned k = 0; k <= 32; k++) reads_as(s, &nca, 0x4000 + k * page, 16, got);
    reads_as(s, &nca, 0x4000 + 32 * page + 3, 16, got); /* kept */
    CHECK(g_test.restarts == 0);
    reads_as(s, &nca, 0x4000, 16, got); /* page 0, whose place page 32 took */
    CHECK(g_test.restarts == 1);
    CHECK(!strcmp(g_test.restart_line,
                  "a read at 0x4000 of a solid NCZ's NCA lies before the 0x2104000 its stream reached: "
                  "0x100000 bytes decoded again from its start"));
    reads_as(s, &nca, 0x4000 + page, 16, got); /* ahead of the stream again */
    reads_as(s, &nca, nca.size - 0x10, 0x10, got);
    CHECK(g_test.restarts == 1);
    snx_hac_on_restart(NULL, NULL);
    reads_as(s, &nca, 0x4000 + 2 * page, 16, got); /* behind it, said to no one */
    CHECK(g_test.restarts == 1);
    ncz_free(z);
    printf("hactool ncz: a solid stream of 33 MiB, read behind itself once, said once\n");
    free(ncz.data);
    free(nca.data);
}

/* ------------------------------------------------------------------ an encrypted program, through snx_hac.h */

static void synthetic_keys(void) {
    for (unsigned i = 0; i < sizeof g_test.header_key; i++) g_test.header_key[i] = (unsigned char)(0x11 + i);
    for (unsigned i = 0; i < sizeof g_test.key_area_key; i++) g_test.key_area_key[i] = (unsigned char)(0xA0 + i);
    for (unsigned i = 0; i < sizeof g_test.section_keys; i++) g_test.section_keys[i] = (unsigned char)(0x31 + 3 * i);
}

static void hex(char* out, const unsigned char* key, size_t size) {
    for (size_t i = 0; i < size; i++) sprintf(out + 2 * i, "%02x", key[i]);
}

static SnxHac* keyed_hac(void) {
    char a[80], b[80];
    SnxHac* hac = snx_hac_new();
    CHECK(hac != NULL);
    hex(a, g_test.header_key, sizeof g_test.header_key);
    hex(b, g_test.key_area_key, sizeof g_test.key_area_key);
    CHECK(snx_hac_set_key(hac, "header_key", a) && snx_hac_set_key(hac, "key_area_key_application_00", b));
    return hac;
}

static void write_file(const char* path, const void* data, size_t size) {
    FILE* file = fopen(path, "wb");
    CHECK(file != NULL);
    CHECK(fwrite(data, 1, size, file) == size);
    CHECK(!fclose(file));
}

/* An NCA3 program: its header in XTS with the synthetic header key, its key area with the key-area key, and two
   AES-CTR sections from 0x4000, as a game's lie: an ExeFS (main.npdm, main) and a RomFS whose image is *image. parts
   gets its NCZ's sections as nsz lists them: each with the key area's AES-CTR key and its counter's upper half */
static blob_t program_nca(ncz_part_t parts[2], blob_t* image, blob_t* code) {
    nca_header_t header = {0};
    blob_t npdm = {zalloc(0x80), 0x80}, big = {zalloc(0x2A123), 0x2A123}, text = {zalloc(0x3456), 0x3456};
    blob_t sections[2], body, pfs0, nca;
    nca_fs_header_t *fs0 = &header.fs_headers[0], *fs1 = &header.fs_headers[1];
    memcpy(npdm.data, "META", 4);
    *code = (blob_t){zalloc(0x5321), 0x5321};
    fill(code->data, code->size, 41);
    fill(big.data, big.size, 43);
    fill(text.data, text.size, 47);
    romfs_file_t exefs[2] = {{"main.npdm", npdm}, {"main", *code}};
    romfs_file_t romfs[3] = {{"head.bin", text}, {"big.bin", big}, {"tail.bin", text}};
    pfs0 = partition_build(MAGIC_PFS0, exefs, 2, 1);
    sections[0] = pfs0_section_build(&pfs0, &fs0->pfs0_superblock);
    *image = romfs_build(romfs, 3);
    sections[1] = ivfc_build(image, &fs1->romfs_superblock.ivfc_header);
    fs0->_0x0 = fs1->_0x0 = 2;
    fs0->partition_type = PARTITION_PFS0;
    fs0->fs_type = FS_TYPE_PFS0;
    fs1->partition_type = PARTITION_ROMFS;
    fs1->fs_type = FS_TYPE_ROMFS;
    fs0->crypt_type = fs1->crypt_type = CRYPT_CTR;
    put64(fs0->section_ctr, 0x0000003300000002ULL);
    put64(fs1->section_ctr, 0x0000004400000003ULL);
    section_entry(&header, 0, 0x4000, sections[0].size);
    section_entry(&header, 1, 0x4000 + sections[0].size, sections[1].size);
    body.size = 0x4000 - 0xC00 + sections[0].size + sections[1].size;
    body.data = zalloc(body.size);
    memset(parts, 0, 2 * sizeof *parts);
    for (unsigned i = 0, at = 0x4000; i < 2; at += (unsigned)sections[i].size, i++) {
        memcpy(body.data + at - 0xC00, sections[i].data, sections[i].size);
        parts[i].offset = at;
        parts[i].size = sections[i].size;
        parts[i].crypto = 3;
        memcpy(parts[i].key, g_test.section_keys + 0x20, 16);
        for (unsigned j = 0; j < 8; j++) parts[i].counter[j] = header.fs_headers[i].section_ctr[7 - j];
        ncz_ctr(&parts[i], at, body.data + at - 0xC00, sections[i].size);
        free(sections[i].data);
    }
    header.magic = MAGIC_NCA3;
    header.content_type = NCACONTENTTYPE_PROGRAM;
    header.fixed_key_generation = 0xFF;
    header.title_id = 0x0100000000AB0000ULL;
    header.nca_size = 0xC00 + body.size;
    aes_ctx_t* aes = new_aes_ctx(g_test.key_area_key, 16, AES_MODE_ECB);
    aes_encrypt(aes, header.encrypted_keys, g_test.section_keys, sizeof g_test.section_keys);
    free_aes_ctx(aes);
    nca.size = 0xC00 + body.size;
    nca.data = zalloc(nca.size);
    nca_header_swap(&header); /* little-endian, as a file has it */
    aes = new_aes_ctx(g_test.header_key, 32, AES_MODE_XTS);
    aes_xts_encrypt(aes, nca.data, &header, 0xC00, 0, 0x200);
    free_aes_ctx(aes);
    memcpy(nca.data + 0xC00, body.data, body.size);
    free(body.data);
    free(pfs0.data);
    free(npdm.data);
    free(big.data);
    free(text.data);
    return nca;
}

static int file_is(const char* path, const blob_t* content) {
    FILE* file = fopen(path, "rb");
    unsigned char* got = zalloc(content->size + 1);
    int same =
        file && fread(got, 1, content->size + 1, file) == content->size && !memcmp(got, content->data, content->size);
    if (file) CHECK(!fclose(file));
    free(got);
    return same;
}

/* the program's NCZ (blocks of 1 << exponent bytes, 0 one stream) beside its NCA, in an NSP or a gamecard: listed with
   its NCA's size, its header read as the NCA's, the NCA whole through it, its ExeFS and RomFS read as the NCA's from
   the file and mapped, and both extracted */
static void program_reads(unsigned exponent, int xci) {
    static const char* const extracted[] = {"exefs/main.npdm", "exefs/main", "romfs/head.bin", "romfs/big.bin",
                                            "romfs/tail.bin"};
    ncz_part_t parts[2];
    blob_t image, code, nca = program_nca(parts, &image, &code), ncz = ncz_build(&nca, parts, 2, exponent, NULL);
    romfs_file_t files[3] = {{"padding.bin", {(unsigned char*)"x", 1}},
                             {"0123456789abcdef0123456789abcdef.nca", nca},
                             {"fedcba9876543210fedcba9876543210.ncz", ncz}};
    blob_t dump = xci ? gamecard_build(files, 3, 0) : partition_build(MAGIC_PFS0, files, 3, 0x203);
    const char* path = xci ? NCZ_DIR "/game.xcz" : NCZ_DIR "/game.nsz";
    const SnxHacEntry *entries, *exe_a, *exe_b;
    SnxHacNca a, b;
    size_t entry = 0;
    unsigned char* got = zalloc(nca.size + 1);
    write_file(path, dump.data, dump.size);
    SnxHac* hac = keyed_hac();
    CHECK(snx_hac_open(hac, path, xci) == 0);
    CHECK(snx_hac_entries(hac, 0, &entries) == 3);
    CHECK(entries[1].ncz == SNX_HAC_NCZ_NONE && entries[1].size == nca.size && entries[1].nca_size == nca.size);
    CHECK(entries[2].ncz == (exponent ? SNX_HAC_NCZ_BLOCK : SNX_HAC_NCZ_SOLID));
    CHECK(entries[2].size == ncz.size && entries[2].nca_size == nca.size && ncz.size < nca.size);
    CHECK(snx_hac_ncz_error(hac, 0, &entry) == NULL);
    CHECK(!snx_hac_nca(hac, 0, 1, &a) && !snx_hac_nca(hac, 0, 2, &b));
    CHECK(b.type == SNX_HAC_PROGRAM && b.title_id == 0x0100000000AB0000ULL && !memcmp(&a, &b, sizeof a));
    CHECK(!snx_hac_read(hac, 0, 2, got, nca.size) && !memcmp(got, nca.data, nca.size));
    CHECK(snx_hac_read(hac, 0, 2, got, nca.size + 1) == -1);
    for (int mapped = 0; mapped < 2; mapped++) {
        if (mapped) CHECK(!snx_hac_map(hac, 0, dump.data, dump.size));
        SnxHacFs *fa = snx_hac_fs_open(hac, 0, 1, -1, 0), *fb = snx_hac_fs_open(hac, 0, 2, -1, 0);
        CHECK(fa && fb && snx_hac_fs_romfs_size(fb) == image.size && snx_hac_fs_romfs_size(fa) == image.size);
        memset(got, 0, image.size);
        CHECK(!snx_hac_fs_read_romfs(fb, 0, got, image.size) && !memcmp(got, image.data, image.size));
        CHECK(!snx_hac_fs_read_romfs(fb, 0x3001, got, 0x12345) && !memcmp(got, image.data + 0x3001, 0x12345));
        CHECK(snx_hac_fs_files(fa, &exe_a) == 2 && snx_hac_fs_files(fb, &exe_b) == 2);
        for (size_t i = 0; i < 2; i++) {
            CHECK(!strcmp(exe_a[i].name, exe_b[i].name) && exe_a[i].size == exe_b[i].size);
            CHECK(!snx_hac_fs_read(fb, i, 0, got, (size_t)exe_b[i].size));
            CHECK(!strcmp(exe_b[i].name, "main") ? exe_b[i].size == code.size && !memcmp(got, code.data, code.size)
                                                 : !memcmp(got, "META", 4));
        }
        snx_hac_fs_free(fa);
        snx_hac_fs_free(fb);
    }
    CHECK(!snx_hac_extract(hac, 0, 2, -1, 0, NCZ_DIR "/exefs", NCZ_DIR "/romfs")); /* as scarletnx -x extracts it */
    CHECK(file_is(NCZ_DIR "/exefs/main", &code));
    for (size_t i = 0; i < sizeof extracted / sizeof *extracted; i++) {
        char at[64];
        snprintf(at, sizeof at, NCZ_DIR "/%s", extracted[i]);
        CHECK(!remove(at));
    }
    CHECK(!rmdir(NCZ_DIR "/exefs") && !rmdir(NCZ_DIR "/romfs"));
    snx_hac_free(hac);
    CHECK(!remove(path));
    printf("hactool ncz: an encrypted program's %s NCZ in %s read and extracted as its NCA\n",
           exponent ? "block" : "solid", xci ? "a gamecard (XCZ)" : "an NSP (NSZ)");
    free(got);
    free(dump.data);
    free(ncz.data);
    free(nca.data);
    free(image.data);
    free(code.data);
}

/* ------------------------------------------------------------------ flaws */

/* ncz (size bytes) refused by ncz_open with says */
static void refused(const unsigned char* data, size_t size, const char* says) {
    char why[256] = "";
    g_test.refusals++;
    unsigned char* copy = zalloc(size);
    memcpy(copy, data, size);
    ncz_t* z = ncz_open(storage_memory_new(copy, size, 0, size), why, sizeof why);
    if (z || strcmp(why, says)) fprintf(stderr, "hactool_ncz.c: ncz_open said \"%s\", not \"%s\"\n", why, says);
    CHECK(z == NULL && !strcmp(why, says));
    free(copy);
}

/* What ncz_open refuses in a header, a table or a block list, said with where and what is there */
static void header_flaws(void) {
    blob_t nca = {zalloc(BYTES), BYTES}, block, solid;
    ncz_part_t parts[6];
    unsigned count = bytes_parts(parts, 0xC00, nca.size);
    const size_t body = 0x4010 + 6 * 0x40, sizes = body + 0x18;
    char says[256];
    fill(nca.data, nca.size, 5);
    ncz_encrypt(nca.data, parts, count);
    block = ncz_build(&nca, parts, count, 14, NULL);
    solid = ncz_build(&nca, parts, count, 0, NULL);
    unsigned char* b = zalloc(block.size);
#define FLAW(setup, text)                                                                                              \
    do {                                                                                                               \
        memcpy(b, block.data, block.size);                                                                             \
        setup;                                                                                                         \
        refused(b, block.size, text);                                                                                  \
    } while (0)
    refused(solid.data, 0x4008, "the NCZ is 0x4008 bytes, short of the 0x4010 its NCA's header and NCZSECTN take");
    FLAW(b[0x4000] = 'X', "0x4000 holds 58 43 5a 53 45 43 54 4e, not NCZSECTN: no NCZ nsz wrote");
    FLAW(put64(b + 0x4008, 0), "its section count, at 0x4008, is 0: an NCZ lists 1 to 1048576");
    FLAW(put64(b + 0x4008, 0x100001), "its section count, at 0x4008, is 1048577: an NCZ lists 1 to 1048576");
    snprintf(says, sizeof says, "its 65536 sections take 0x400000 bytes from 0x4010, past the NCZ's 0x%zx", block.size);
    FLAW(put64(b + 0x4008, 0x10000), says);
    FLAW(put64(b + 0x4090, 0x18010), "section 2 (its entry at 0x4090) starts at 0x18010 of the NCA, and the one before "
                                     "it ends at 0x18000: nsz lays them one after another");
    FLAW((put64(b + 0x4018, 0x1400), put64(b + 0x4050, 0x2000), put64(b + 0x4058, 0x16000)),
         "section 1 (its entry at 0x4050) starts at 0x2000 of the NCA, inside the 0x4000 bytes an NCZ keeps as they "
         "are, which only its first section may start in");
    FLAW(put64(b + 0x4158, UINT64_MAX),
         "section 5 (its entry at 0x4150) has offset 0x30008 and size 0xffffffffffffffff, past 2^64");
    FLAW((put64(b + 0x4008, 1), put64(b + 0x4018, 0x1800)),
         "its sections end at 0x2400 of the NCA, inside the 0x4000 bytes it keeps as they are: nothing of it is "
         "compressed");
    FLAW(b[body + 8] = 3, "its NCZBLOCK at 0x4190 is version 3 type 1, and nsz writes version 2 type 1");
    FLAW(b[body + 9] = 0, "its NCZBLOCK at 0x4190 is version 2 type 0, and nsz writes version 2 type 1");
    FLAW(b[body + 11] = 13, "its NCZBLOCK at 0x4190 gives blocks of 2^13 bytes, and nsz's are 2^14 to 2^32");
    FLAW(b[body + 11] = 33, "its NCZBLOCK at 0x4190 gives blocks of 2^33 bytes, and nsz's are 2^14 to 2^32");
    FLAW(put64(b + body + 16, BYTES - 0x4000 + 1),
         "its NCZBLOCK at 0x4190 says 0x5c124 bytes decompress, and its sections make 0x5c123 past 0x4000");
    FLAW(put32(b + body + 12, 23),
         "its NCZBLOCK at 0x4190 has 23 blocks, and 0x5c123 bytes in blocks of 0x4000 make 24");
    snprintf(says, sizeof says, "its 2147483647 blocks' sizes take 0x1fffffffc bytes from 0x41a8, past the NCZ's 0x%zx",
             block.size);
    FLAW(put32(b + body + 12, 0x7FFFFFFF), says);
    FLAW(put32(b + sizes, 0x4001), "block 0's stored size, at 0x41a8, is 0x4001, and the block holds 0x4000 bytes: nsz "
                                   "stores a block in 1 to as many bytes as it holds");
    FLAW(put32(b + sizes + 4, 0), "block 1's stored size, at 0x41ac, is 0x0, and the block holds 0x4000 bytes: nsz "
                                  "stores a block in 1 to as many bytes as it holds");
    FLAW(put32(b + sizes + 23 * 4, 0x124), "block 23's stored size, at 0x4204, is 0x124, and the block holds 0x123 "
                                           "bytes: nsz stores a block in 1 to as many bytes as it holds");
    snprintf(says, sizeof says, "its blocks end at 0x%zx, past the NCZ's 0x%zx bytes: the file is cut short",
             block.size, block.size - 1);
    refused(block.data, block.size - 1, says);
    refused(block.data, body + 0x10, "its NCZBLOCK at 0x4190 is cut short: the NCZ ends at 0x41a0");
    refused(solid.data, body,
            "its section table ends at 0x4190, where the NCZ does: no blocks or zstd stream after it");
#undef FLAW
    printf("hactool ncz: %u flaws of a header, its sections or its blocks refused, each said\n", g_test.refusals);
    free(b);
    free(block.data);
    free(solid.data);
    free(nca.data);
}

/* a read of ncz (size bytes) at offset that fails, saying what holds both parts of says, the first at its start */
static void read_fails(const unsigned char* data, size_t size, uint64_t offset, const char* starts, const char* has) {
    char why[256] = "";
    unsigned char got[16];
    ncz_t* z = ncz_open(storage_memory_new(data, size, 0, size), why, sizeof why);
    CHECK(z != NULL);
    for (int again = 0; again < 2; again++) { /* the same failure the second time: it starts over */
        CHECK(!storage_read(ncz_storage(z), offset, got, sizeof got));
        if (strncmp(snx_hac_message(), starts, strlen(starts)) || !strstr(snx_hac_message(), has))
            fprintf(stderr, "hactool_ncz.c: the read said \"%s\"\n", snx_hac_message());
        CHECK(!strncmp(snx_hac_message(), starts, strlen(starts)) && strstr(snx_hac_message(), has));
    }
    ncz_free(z);
}

/* What a read finds wrong in a block or a stream, said with the block or the offsets and what zstd says: a block that
   is no zstd frame, one whose frames hold fewer bytes than the block, a stream cut short, and one that is no stream */
static void data_flaws(void) {
    blob_t nca = {zalloc(BYTES), BYTES}, block, solid;
    ncz_part_t parts[6];
    unsigned count = bytes_parts(parts, 0xC00, nca.size);
    const size_t body = 0x4010 + 6 * 0x40, sizes = body + 0x18;
    size_t at = sizes + 24 * 4, stored = 0, b, made;
    unsigned char zeros[0x4000] = {0}, frame[0x100];
    uint32_t seed = 13;
    char says[256];
    fill(nca.data, nca.size, 9);
    for (size_t i = 0x4000; i < 0x8000; i++) nca.data[i] = (unsigned char)(next(&seed) & 0x0F); /* block 0: half */
    ncz_encrypt(nca.data, parts, count);
    block = ncz_build(&nca, parts, count, 14, NULL);
    solid = ncz_build(&nca, parts, count, 0, NULL);
    /* a frame of 0x10 bytes fewer than a block, which a compressed block's bytes take with a skippable frame after */
    made = ZSTD_compress(frame, sizeof frame, zeros, sizeof zeros - 0x10, 3);
    CHECK(!ZSTD_isError(made));
    for (b = 0; b < 24; b++, at += stored) { /* the first block zstd compressed to room for both */
        stored = le32(block.data + sizes + 4 * b);
        if (stored < 0x4000 && stored >= made + 8) break;
    }
    CHECK(b == 0);          /* the half noise one */
    block.data[at] ^= 0xFF; /* its frame's magic */
    snprintf(says, sizeof says,
             "NCZ block %zu, 0x%zx bytes at 0x%zx (the NCA's 0x%zx on), does not decompress: zstd says ", b, stored, at,
             0x4000 + b * 0x4000);
    read_fails(block.data, block.size, 0x4000 + b * 0x4000 + 5, says, "Unknown frame descriptor");
    block.data[at] ^= 0xFF;
    { /* the frame, and a skippable frame over the rest of the block's bytes */
        memcpy(block.data + at, frame, made);
        put32(block.data + at + made, 0x184D2A50);
        put32(block.data + at + made + 4, (uint32_t)(stored - made - 8));
        snprintf(says, sizeof says,
                 "NCZ block %zu, 0x%zx bytes at 0x%zx (the NCA's 0x%zx on), decompresses to 0x3ff0 "
                 "bytes, and the block holds 0x4000",
                 b, stored, at, 0x4000 + b * 0x4000);
        read_fails(block.data, block.size, 0x4000 + b * 0x4000, says, "");
    }
    snprintf(says, sizeof says, "The NCZ's zstd stream ends with the file, at 0x%zx, and its NCA's bytes from 0x",
             solid.size - 0x40);
    read_fails(solid.data, solid.size - 0x40, BYTES - 0x10, says, " are still to come: the NCZ is cut short");
    solid.data[body] ^= 0xFF; /* the stream's magic */
    read_fails(solid.data, solid.size, 0x5000, "The NCZ's zstd stream fails at 0x",
               ", the NCA's 0x4000: zstd says Unknown frame descriptor");
    printf("hactool ncz: a block and a stream that do not decompress, and one cut short, each said\n");
    free(block.data);
    free(solid.data);
    free(nca.data);
}

/* An NSZ with an NCZ whose header cannot be read: listed as one, its why and entry given, and its NCA unread */
static void broken_entry(void) {
    blob_t nca = {zalloc(BYTES), BYTES}, ncz;
    ncz_part_t parts[6];
    unsigned count = bytes_parts(parts, 0xC00, nca.size);
    const SnxHacEntry* entries;
    SnxHacNca info;
    size_t entry = 9;
    fill(nca.data, nca.size, 11);
    ncz = ncz_build(&nca, parts, count, 0, NULL);
    memcpy(ncz.data + 0x4000, "NCZSECTX", 8);
    romfs_file_t files[2] = {{"0123456789abcdef0123456789abcdef.cnmt.nca", {(unsigned char*)"meta", 4}},
                             {"aaaabbbbccccddddeeeeffff00001111.NCZ", ncz}};
    blob_t nsp = partition_build(MAGIC_PFS0, files, 2, 0x203);
    write_file(NCZ_DIR "/broken.nsz", nsp.data, nsp.size);
    SnxHac* hac = keyed_hac();
    CHECK(snx_hac_open(hac, NCZ_DIR "/broken.nsz", 0) == 0);
    CHECK(snx_hac_entries(hac, 0, &entries) == 2 && entries[0].ncz == SNX_HAC_NCZ_NONE);
    CHECK(entries[1].ncz == SNX_HAC_NCZ_BROKEN && entries[1].nca_size == ncz.size);
    CHECK(!strcmp(snx_hac_ncz_error(hac, 0, &entry), "0x4000 holds 4e 43 5a 53 45 43 54 58, not NCZSECTN: no NCZ nsz "
                                                     "wrote") &&
          entry == 1);
    CHECK(snx_hac_nca(hac, 0, 1, &info) == -1);
    CHECK(strstr(snx_hac_message(), ".NCZ cannot be read as the NCA it compresses: 0x4000 holds") != NULL);
    snx_hac_free(hac);
    CHECK(!remove(NCZ_DIR "/broken.nsz"));
    printf("hactool ncz: an NSZ's NCZ whose header cannot be read, named, its NCA never read\n");
    free(nsp.data);
    free(ncz.data);
    free(nca.data);
}

int main(void) {
#if defined(_WIN32)
    _mkdir(NCZ_DIR);
#else
    mkdir(NCZ_DIR, 0700);
#endif
    synthetic_keys();
    bytes_match(0, 0xC00);
    bytes_match(14, 0xC00);
    bytes_match(16, 0x6000); /* a gap before the first section, its bytes as they are */
    bytes_match(0, 0x6000);
    bytes_match(20, 0x4000); /* one block, shorter than its size */
    solid_restart();
    program_reads(0, 0);
    program_reads(14, 0);
    program_reads(14, 1);
    program_reads(0, 1);
    header_flaws();
    data_flaws();
    broken_entry();
    CHECK(!rmdir(NCZ_DIR));
    printf("PASS %u hactool ncz checks; synthetic NCAs and keys, no dumps\n", g_test.checks);
    return 0;
}
