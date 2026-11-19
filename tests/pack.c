/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* scarletnx --pack (src/nx/runtime/driver/pack.c) on synthetic NCAs (tests/hac_synth.h) under synthetic keys: one
   encrypted with its key area, one with a title key that a ticket gives, and a DNCA. Each packed NCA must be the plain
   form of its input byte for byte, in an NSP and in an XCI that hactool and the dump reader open with no keys. No
   dumps or console keys */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/utilities/common.h"
#include "nx/runtime/driver/container.h"
#include "nx/runtime/driver/pack.h"
#if defined(_WIN32) && defined(__clang__)
/* hactool's utils.h redeclares the MinGW CRT's imported fseeko64. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winconsistent-dllimport"
#endif
#include "nx/runtime/driver/hactool/aes.h"
#include "nx/runtime/driver/hactool/hfs0.h"
#include "nx/runtime/driver/hactool/nca.h"
#include "nx/runtime/driver/hactool/pfs0.h"
#include "nx/runtime/driver/hactool/sha.h"
#include "nx/runtime/driver/hactool/xci.h"
#if defined(_WIN32) && defined(__clang__)
#pragma clang diagnostic pop
#endif

static unsigned g_checks;
static char g_said[2048];

static void check(int good, const char* expression, int line) {
    g_checks++;
    if (!good) {
        fprintf(stderr, "pack.c:%d: check failed: %s\n", line, expression);
        exit(90);
    }
}
#define CHECK(value) check((value) != 0, #value, __LINE__)

#include "hac_synth.h"

#define TITLE 0x0100000000AC0000ULL

/* synthetic keys: the header key's XTS halves differ, as OpenSSL requires */
static const char prod_keys[] = "header_key = 0123456789abcdef0123456789abcdeffedcba9876543210fedcba9876543210\n"
                                "key_area_key_application_00 = 00112233445566778899aabbccddeeff\n"
                                "titlekek_00 = 0f1e2d3c4b5a69788796a5b4c3d2e1f0\n";
static const unsigned char header_key[32] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45,
                                             0x67, 0x89, 0xab, 0xcd, 0xef, 0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54,
                                             0x32, 0x10, 0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10};
static const unsigned char area_key[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                           0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
static const unsigned char titlekek[16] = {0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78,
                                           0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0};
static const unsigned char rights[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0xAC, 0x00, 0x00,
                                         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static void said_line(const char* line) { snprintf(g_said, sizeof g_said, "%s", line); }

static blob_t text_blob(const char* text) {
    blob_t b = {zalloc(strlen(text)), strlen(text)};
    memcpy(b.data, text, b.size);
    return b;
}

static void ecb_encrypt(const unsigned char key[16], const unsigned char in[16], unsigned char out[16]) {
    aes_ctx_t* aes = new_aes_ctx(key, 16, AES_MODE_ECB);
    aes_encrypt(aes, out, in, 16);
    free_aes_ctx(aes);
}

/* nca (a DNCA) made an encrypted NCA3 of master key 0, its sections AES-CTR under body: the key in its key area, or
   the title key of rights when title. *plain takes what the packer must give back: the header as it is before the
   header key seals it, and the sections plain */
static void encrypt(blob_t* nca, const unsigned char body[16], int title, blob_t* plain) {
    nca_header_t header;
    memcpy(&header, nca->data, sizeof header);
    nca_header_swap(&header);
    header.magic = MAGIC_NCA3;
    header.crypto_type = header.crypto_type2 = 0;
    header.fixed_key_generation = 0;
    header.kaek_ind = 0;
    if (title)
        memcpy(header.rights_id, rights, 16);
    else
        ecb_encrypt(area_key, body, header.encrypted_keys[2]);
    for (unsigned i = 0; i < 4; i++)
        if (header.section_entries[i].media_start_offset) {
            header.fs_headers[i].crypt_type = CRYPT_CTR;
            for (unsigned j = 0; j < 8; j++) header.fs_headers[i].section_ctr[j] = (uint8_t)(0x10 * i + j + 1);
        }
    nca_header_t sealed_from = header;
    nca_header_swap(&header);
    memcpy(nca->data, &header, 0xC00);
    plain->size = nca->size;
    plain->data = zalloc(nca->size);
    memcpy(plain->data, nca->data, nca->size);
    for (unsigned i = 0; i < 4; i++) {
        if (!sealed_from.section_entries[i].media_start_offset) continue;
        size_t start = (size_t)sealed_from.section_entries[i].media_start_offset * 0x200;
        size_t end = (size_t)sealed_from.section_entries[i].media_end_offset * 0x200;
        unsigned char iv[16];
        CHECK(end <= nca->size && start < end);
        for (unsigned j = 0; j < 8; j++) iv[j] = sealed_from.fs_headers[i].section_ctr[7 - j];
        put64(iv + 8, 0);
        for (unsigned j = 0; j < 8; j++) iv[8 + j] = (unsigned char)((uint64_t)(start >> 4) >> (56 - 8 * j));
        unsigned char* out = zalloc(end - start);
        aes_ctx_t* aes = new_aes_ctx(body, 16, AES_MODE_CTR);
        aes_setiv(aes, iv, 16);
        aes_encrypt(aes, out, nca->data + start, end - start);
        free_aes_ctx(aes);
        CHECK(memcmp(out, nca->data + start, end - start)); /* it was encrypted */
        memcpy(nca->data + start, out, end - start);
        free(out);
    }
    unsigned char sealed[0xC00];
    aes_ctx_t* aes = new_aes_ctx(header_key, 32, AES_MODE_XTS);
    aes_xts_encrypt(aes, sealed, nca->data, sizeof sealed, 0, 0x200);
    free_aes_ctx(aes);
    memcpy(nca->data, sealed, sizeof sealed);
}

/* a common ticket for rights, its title key key sealed with titlekek */
static blob_t ticket(const unsigned char key[16]) {
    blob_t b = {zalloc(0x2C0), 0x2C0};
    put32(b.data, 0x10004);
    ecb_encrypt(titlekek, key, b.data + 0x180);
    memcpy(b.data + 0x2A0, rights, 16);
    return b;
}

#define NAME_PROGRAM "0123456789abcdef0123456789abcdef.nca"
#define NAME_CONTROL "1123456789abcdef0123456789abcdef.nca"
#define NAME_DATA    "2123456789abcdef0123456789abcdef.nca"

typedef struct {
    const char* name;
    blob_t plain;
} Expected;

static void write_blob(const char* dir, const char* name, blob_t blob) {
    char* path = snx_path_join(dir, name);
    CHECK(path && !snx_write_file(path, blob.data, blob.size));
    free(path);
}

/* the NCAs and the ticket in folder, without the ticket when no_ticket; what the pack must give in expected */
static void make_folder(const char* folder, Expected expected[3], int no_ticket) {
    static const unsigned char body1[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    static const unsigned char body2[16] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    blob_t code = text_blob("synthetic main"), npdm = {zalloc(0x80), 0x80}, data = text_blob("synthetic romfs data");
    memcpy(npdm.data, "META", 4);
    romfs_file_t exefs[2] = {{"main.npdm", npdm}, {"main", code}}, romfs[1] = {{"data.bin", data}};
    blob_t program = dnca_build(NCACONTENTTYPE_PROGRAM, TITLE, exefs, 2, romfs, 1);
    blob_t nacp = {zalloc(0x4000), 0x4000};
    strcpy((char*)nacp.data, "Synthetic Game");
    romfs_file_t control_files[1] = {{"control.nacp", nacp}};
    blob_t control = dnca_build(NCACONTENTTYPE_CONTROL, TITLE, NULL, 0, control_files, 1);
    blob_t other = text_blob("synthetic data");
    romfs_file_t data_files[1] = {{"data.bin", other}};
    blob_t dnca = dnca_build(NCACONTENTTYPE_DATA, TITLE, NULL, 0, data_files, 1);
    CHECK(snx_mkdirs(folder) == 0);
    encrypt(&program, body1, 0, &expected[0].plain);
    encrypt(&control, body2, 1, &expected[1].plain);
    expected[0].name = NAME_PROGRAM;
    expected[1].name = NAME_CONTROL;
    expected[2].name = NAME_DATA;
    expected[2].plain.size = dnca.size;
    expected[2].plain.data = zalloc(dnca.size);
    memcpy(expected[2].plain.data, dnca.data, dnca.size);
    memcpy(expected[2].plain.data + 0x200, "NCA3", 4); /* a DNCA's plain NCA3 */
    write_blob(folder, NAME_PROGRAM, program);
    write_blob(folder, NAME_CONTROL, control);
    write_blob(folder, NAME_DATA, dnca);
    if (!no_ticket) {
        blob_t t = ticket(body2);
        write_blob(folder, "0100000000ac0000000000000000000000.tik", t);
        free(t.data);
    }
    write_blob(folder, "notes.txt", other); /* neither an NCA nor a ticket: left out */
    free(program.data);
    free(control.data);
    free(dnca.data);
    free(code.data);
    free(npdm.data);
    free(data.data);
    free(nacp.data);
    free(other.data);
}

static int pack(const char* input, const char* out, const char* keys, const char* data, int xci) {
    char* argv[10];
    int n = 0;
    argv[n++] = (char*)input;
    argv[n++] = "--out";
    argv[n++] = (char*)out;
    argv[n++] = "--keys";
    argv[n++] = (char*)keys;
    argv[n++] = "--data";
    argv[n++] = (char*)data;
    if (xci) argv[n++] = "--xci";
    argv[n] = NULL;
    g_said[0] = 0;
    return snx_pack(n, argv, NULL);
}

/* the NSP at path holds expected's NCAs, in name order, each its plain bytes */
static void check_nsp(const char* path, const Expected expected[3]) {
    size_t size;
    unsigned char* nsp = snx_read_file(path, &size);
    CHECK(nsp && size > 0x10 && !memcmp(nsp, "PFS0", 4));
    uint32_t count = snx_ld_le32(nsp + 4), strings = snx_ld_le32(nsp + 8);
    CHECK(count == 3);
    size_t data = 0x10 + count * 0x18 + strings;
    CHECK(data % 0x20 == 0);
    for (uint32_t i = 0; i < count; i++) {
        const unsigned char* e = nsp + 0x10 + i * 0x18;
        uint64_t at = snx_ld_le64(e), n = snx_ld_le64(e + 8);
        const char* name = (const char*)nsp + 0x10 + count * 0x18 + snx_ld_le32(e + 16);
        CHECK(!strcmp(name, expected[i].name));
        CHECK(n == expected[i].plain.size && data + at + n <= size);
        CHECK(!memcmp(nsp + data + at, expected[i].plain.data, n));
    }
    free(nsp);
}

/* the XCI at path, opened by hactool with no keys (its root's hash checked), holds expected's NCAs, each plain with
   its first page's hash in the secure partition */
static void check_xci(const char* path, const Expected expected[3]) {
    SnxHac* hac = snx_hac_new();
    CHECK(hac != NULL);
    int container = snx_hac_open(hac, path, 1);
    CHECK(container >= 0);
    const SnxHacEntry* entries;
    CHECK(snx_hac_entries(hac, container, &entries) == 3);
    for (size_t i = 0; i < 3; i++) {
        CHECK(!strcmp(entries[i].name, expected[i].name) && entries[i].size == expected[i].plain.size);
        unsigned char* got = zalloc((size_t)entries[i].size);
        CHECK(!snx_hac_read(hac, container, i, got, (size_t)entries[i].size));
        CHECK(!memcmp(got, expected[i].plain.data, (size_t)entries[i].size));
        free(got);
        SnxHacNca info;
        CHECK(!snx_hac_nca(hac, container, i, &info) && info.decrypted && info.title_id == TITLE);
    }
    snx_hac_free(hac);
    /* the secure partition's entries hash each NCA's first 0x200 bytes, and the header names the card's pages */
    size_t size;
    unsigned char* xci = snx_read_file(path, &size);
    CHECK(xci && size % 0x200 == 0 && !memcmp(xci + 0x100, "HEAD", 4));
    CHECK(snx_ld_le64(xci + 0x118) == size / 0x200 - 1);
    uint64_t root = snx_ld_le64(xci + 0x130), root_size = snx_ld_le64(xci + 0x138);
    unsigned char hash[32];
    sha256_hash_buffer(hash, xci + root, (size_t)root_size);
    CHECK(!memcmp(hash, xci + 0x140, 32));
    const unsigned char* top = xci + root;
    CHECK(snx_ld_le32(top + 4) == 3);
    const unsigned char* secure_entry = top + 0x10 + 2 * 0x40;
    uint64_t secure = root + root_size + snx_ld_le64(secure_entry);
    CHECK(snx_ld_le32(xci + 0x104) == secure / 0x200);
    sha256_hash_buffer(hash, xci + secure, snx_ld_le32(secure_entry + 20));
    CHECK(!memcmp(hash, secure_entry + 32, 32));
    for (uint32_t i = 0; i < 3; i++) {
        const unsigned char* e = xci + secure + 0x10 + i * 0x40;
        CHECK(snx_ld_le32(e + 20) == 0x200);
        sha256_hash_buffer(hash, expected[i].plain.data, 0x200);
        CHECK(!memcmp(hash, e + 32, 32));
    }
    free(xci);
}

int main(void) {
    char* scratch = snx_temp_dir(NULL);
    CHECK(scratch != NULL);
    char *keys = snx_path_join(scratch, "prod.keys"), *folder = snx_path_join(scratch, "Game NCAs"),
         *nsp = snx_path_join(scratch, "game.nsp"), *xci = snx_path_join(scratch, "game.xci"),
         *again = snx_path_join(scratch, "again.nsp"), *bare = snx_path_join(scratch, "no ticket"),
         *bare_out = snx_path_join(scratch, "no ticket.nsp"), *bare_part = snx_path_join(scratch, "no ticket.nsp.part");
    CHECK(!snx_write_file(keys, prod_keys, sizeof prod_keys - 1));
    Expected expected[3], unused[3];
    make_folder(folder, expected, 0);
    snx_error_sink(said_line);

    /* a folder into an NSP, and into an XCI */
    CHECK(!pack(folder, nsp, keys, scratch, 0));
    check_nsp(nsp, expected);
    printf("PASS folder into an NSP of plain NCAs\n");
    CHECK(!pack(folder, xci, keys, scratch, 1));
    check_xci(xci, expected);
    printf("PASS folder into an XCI of plain NCAs\n");

    /* with xci_header_key, the gamecard info is encrypted under it: AES-128-CBC, the IV the header's reversed */
    {
        static const char with_xci[] = "header_key = 0123456789abcdef0123456789abcdeffedcba9876543210fedcba9876543210\n"
                                       "key_area_key_application_00 = 00112233445566778899aabbccddeeff\n"
                                       "titlekek_00 = 0f1e2d3c4b5a69788796a5b4c3d2e1f0\n"
                                       "xci_header_key = 2468ace013579bdf2468ace013579bdf\n";
        static const unsigned char xci_key[16] = {0x24, 0x68, 0xac, 0xe0, 0x13, 0x57, 0x9b, 0xdf,
                                                  0x24, 0x68, 0xac, 0xe0, 0x13, 0x57, 0x9b, 0xdf};
        char *keys2 = snx_path_join(scratch, "xci.keys"), *xci2 = snx_path_join(scratch, "info.xci");
        CHECK(!snx_write_file(keys2, with_xci, sizeof with_xci - 1));
        CHECK(!pack(folder, xci2, keys2, scratch, 1));
        check_xci(xci2, expected);
        size_t size;
        unsigned char *card = snx_read_file(xci2, &size), iv[16], info[0x70];
        CHECK(card && size > 0x200);
        for (int i = 0; i < 16; i++) iv[i] = card[0x120 + 15 - i];
        aes_ctx_t* aes = new_aes_ctx(xci_key, 16, AES_MODE_CBC);
        aes_setiv(aes, iv, 16);
        aes_decrypt(aes, info, card + 0x190, 0x70);
        free_aes_ctx(aes);
        CHECK(snx_ld_le64(info) == 2 && snx_ld_le32(info + 8) == 0x00A10011u && info[0x24] == 0);
        CHECK(snx_ld_le64(info + 0x30) == 0x0100000000000816ull);
        free(card);
        free(keys2);
        free(xci2);
    }
    printf("PASS the gamecard info encrypted under xci_header_key\n");

    /* the dump reader takes the NSP with no keys: its program's title */
    {
        SnxDump dump;
        SnxU64 title = 0, game = 0;
        CHECK(!snx_dump_title(&dump, nsp, NULL, NULL, scratch, 0, &title, &game, NULL));
        CHECK(title == TITLE && dump.program.found && dump.program.info.decrypted);
        snx_dump_close(&dump);
    }

    /* a dump packed again: plain already, its NCAs copied as they are, into the same NSP */
    CHECK(pack(nsp, nsp, keys, scratch, 0)); /* there already, and the input besides */
    CHECK(strstr(g_said, "is there already") != NULL);
    CHECK(!pack(xci, again, keys, scratch, 0));
    {
        size_t a_size, b_size;
        unsigned char *a = snx_read_file(nsp, &a_size), *b = snx_read_file(again, &b_size);
        CHECK(a && b && a_size == b_size && !memcmp(a, b, a_size));
        free(a);
        free(b);
    }
    printf("PASS an XCI packed again into the same NSP\n");

    /* a title key no ticket gives: refused, said, nothing left */
    make_folder(bare, unused, 1);
    CHECK(pack(bare, bare_out, keys, scratch, 0));
    CHECK(strstr(g_said, NAME_CONTROL) != NULL && strstr(g_said, "title key") != NULL);
    CHECK(!snx_path_exists(bare_out) && !snx_path_exists(bare_part));
    printf("PASS a missing title key refused\n");

    snx_error_sink(NULL);
    for (int i = 0; i < 3; i++) {
        free(expected[i].plain.data);
        free(unused[i].plain.data);
    }
    CHECK(!snx_remove_tree(scratch));
    free(keys);
    free(folder);
    free(nsp);
    free(xci);
    free(again);
    free(bare);
    free(bare_out);
    free(bare_part);
    free(scratch);
    printf("PASS %u pack checks; synthetic NCAs, no console keys\n", g_checks);
    return 0;
}
