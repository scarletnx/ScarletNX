/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An encrypted Vita dump's import (vita/loader/dump.c, keys.c, license.c, pfs.c, selfcrypt.c), on a dump this test
   makes with keys of its own: a game, two add-ons and their licenses, every file encrypted by the scheme psvpfsparser
   reads (files.db's B-tree of two levels, unicv.db's tables, one of 52 sectors over two signature pages, a version 2
   table's seed, files.db's salt), the eboot.bin and a module SELFs whose segment is deflated and encrypted under key
   revisions 2 and 0. The import gives the files' plaintext under the dump's names, the folder files.db lists and the
   dump lacks, an eboot the loader loads; leaves out what files.db does not list, saying it, and the add-on without a
   license, saying it; keeps a decryption already there and replaces a link to the dump, leaving the dump as it was.
   It refuses, naming the file and why: a license of another content, a changed sector, a missing key, a missing keys
   folder, no license at all, and a key file of another size. A zRIF (made by Python's zlib against the dictionary)
   decodes to its license, and the import takes it only when the dump has none of its own, saying so otherwise */
#include "vita/loader/dump.h"
#include "vita/loader/image.h"
#include "vita/loader/keys.h"
#include "vita/loader/license.h"
#include "vita/loader/selfcrypt.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include <assert.h>
#include <miniz.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE      0x400u
#define SECTOR    0x8000u
#define TITLE     "SNXT00001"
#define GAME      "SX0000-SNXT00001_00-SCARLETNXTESTAPP"
#define ADDON1_ID "SCARLETNXADDON01" /* an add-on's folder, its content ID's last 16 characters */
#define ADDON2_ID "SCARLETNXADDON02"
#define ADDON1    "SX0000-SNXT00001_00-" ADDON1_ID
#define ADDON2    "SX0000-SNXT00001_00-" ADDON2_ID
/* the license of GAME, its klicensee 0xa0..0xaf, as Python's zlib deflates it against the zRIF dictionary */
#define GAME_ZRIF "ePlifR1dQ+cHR4CM1Q32iwDbZwiyP9jZMcjHNQQo5Boc4hgQgM9/CxYuWrxk6bLlK1auWr1m7br1ozE+uAEAp5QX1Q=="

/* the test's state: its keys (each byte its key's number and position, none the console's), the contents' klicensees,
   the files' plaintext, and with VITA_DECRYPT_MAKE the folder each content's plaintext goes to */
static struct {
    VitaKeys keys;
    char plain[512];
    unsigned char game_key[16], addon_key[16], icon[37], addon[1000];
    unsigned char *big, *mixed, *eboot, *module;
    size_t eboot_size, module_size;
} g_vita_decrypt;
static void test_keys(void) {
    g_vita_decrypt.keys = (VitaKeys){.have = (1u << SNX_VITA_KEY_COUNT) - 1};
    snprintf(g_vita_decrypt.keys.path, sizeof g_vita_decrypt.keys.path, "the test's keys");
    for (unsigned k = 0; k < SNX_VITA_KEY_COUNT; ++k)
        for (unsigned i = 0; i < 32; ++i) g_vita_decrypt.keys.bytes[k][i] = (unsigned char)(k * 37 + i * 11 + 1);
}
/* the keys' folder at path, a file of raw bytes for each key, the one without left out (or one of other files beside
   them, as vitakeys has: no Vita key's) */
static void write_keys(const char* path, int without) {
    char file[SNX_PATH_SIZE];
    assert(!snx_mkdirs(path));
    for (int k = 0; k < SNX_VITA_KEY_COUNT; ++k) {
        snprintf(file, sizeof file, "%s/%s", path, snx_vita_key_file(k));
        if (k == without)
            remove(file);
        else
            assert(!snx_write_file(file, g_vita_decrypt.keys.bytes[k], snx_vita_key_size(k)));
    }
    snprintf(file, sizeof file, "%s/gpkg-key-2", path);
    assert(!snx_write_file(file, "another tool's", 14));
}

static void crypt(const EVP_CIPHER* cipher, int encrypt, const unsigned char* key, const unsigned char* iv,
                  const unsigned char* in, unsigned char* out, size_t n) {
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    int a = 0, b = 0;
    assert(c && EVP_CipherInit_ex(c, cipher, NULL, key, iv, encrypt) == 1 && EVP_CIPHER_CTX_set_padding(c, 0) == 1);
    assert(EVP_CipherUpdate(c, out, &a, in, (int)n) == 1 && EVP_CipherFinal_ex(c, out + a, &b) == 1);
    assert((size_t)(a + b) == n);
    EVP_CIPHER_CTX_free(c);
}
static void hmac(const unsigned char* key, size_t key_size, const void* data, size_t n, unsigned char out[20]) {
    size_t length = 0;
    assert(EVP_Q_mac(NULL, "HMAC", NULL, "SHA1", NULL, key, key_size, data, n, out, 20, &length) && length == 20);
}

/* ---- the PFS, as the console writes one ---- */
typedef struct {
    const char* name; /* in files.db */
    const char* disk; /* its path in the dump; NULL for a folder the dump lacks */
    uint16_t type;
    int parent; /* the item of its folder, -1 the root */
    const unsigned char* data;
    uint32_t size;
    uint32_t index; /* its table's page in unicv.db */
} Item;
typedef struct {
    unsigned version, spec, table_version;
    uint32_t salt;
} Layout;
static size_t salt_bytes(uint32_t files_salt, uint32_t page, unsigned char out[8]) {
    if (!files_salt) return snx_st_le32(out, page), 4;
    snx_st_le32(out, files_salt), snx_st_le32(out + 4, page);
    return 8;
}
static void secret(const unsigned char key[16], uint32_t files_salt, uint32_t page, unsigned char out[20]) {
    unsigned char salt[8], combo[20], block[16], pad[16];
    hmac(g_vita_decrypt.keys.bytes[SNX_VITA_KEY_PFS_HMAC1], 20, salt, salt_bytes(files_salt, page, salt), combo);
    crypt(EVP_aes_128_cbc(), 1, key, g_vita_decrypt.keys.bytes[SNX_VITA_KEY_PFS_IV0], combo, block, 16);
    crypt(EVP_aes_128_ecb(), 1, key, NULL, block, pad, 16);
    memcpy(out, block, 16);
    for (int i = 0; i < 4; ++i) out[16 + i] = combo[16 + i] ^ pad[i];
}
static void put(const char* root, const char* rel, const void* data, size_t n) {
    char* path = snx_path_join(root, rel);
    char* dir = snx_dirname(path);
    assert(path && dir && !snx_mkdirs(dir) && !snx_write_file(path, data, n));
    free(dir);
    free(path);
}
/* a files.db page's ICV, the chain of HMACs over its n ICVs */
static void page_icv(const unsigned char* page, unsigned n, const unsigned char s[20], unsigned char out[20]) {
    unsigned char chain[40] = {0};
    for (unsigned i = 0; i < n; ++i) {
        memcpy(chain + 20, page + 0x338 + i * 20, 20);
        hmac(s, 20, chain, 40, chain);
    }
    memcpy(out, chain, 20);
}
static void entry(unsigned char* page, unsigned slot, const Item* items, const Item* it) {
    snx_st_le32(page + 0x10 + slot * 72, it->parent < 0 ? 0 : items[it->parent].index);
    memcpy(page + 0x14 + slot * 72, it->name, strlen(it->name));
    snx_st_le32(page + 0x298 + slot * 16, it->index);
    snx_st_le16(page + 0x298 + slot * 16 + 4, it->type);
    snx_st_le32(page + 0x298 + slot * 16 + 8, it->size);
    for (unsigned i = 0; i < 20; ++i)
        page[0x338 + slot * 20 + i] = (unsigned char)(0x5a ^ (slot * 20 + i)); /* its own */
}
/* the content's folder root: its files encrypted under klicensee, sce_pfs/files.db and unicv.db. files.db is one page
   (count <= 9), or a root of one entry (the first item) over two leaves */
static void make_pfs(const char* root, Item* items, unsigned count, const unsigned char klicensee[16], Layout l) {
    unsigned char key[16];
    crypt(EVP_aes_128_ecb(), 0, g_vita_decrypt.keys.bytes[SNX_VITA_KEY_PFS_CONTRACT0], NULL, klicensee, key, 16);
    uint32_t page = 1;
    for (unsigned i = 0; i < count; ++i) {
        uint32_t sectors = (items[i].size + SECTOR - 1) / SECTOR;
        items[i].index = page;
        page += 1 + (sectors + 49) / 50;
        if (i == 0) ++page; /* a page of zeros between two tables */
    }
    size_t icv_size = (size_t)page * PAGE;
    unsigned char* icv = calloc(1, icv_size);
    assert(icv);
    memcpy(icv, "SCEIRODB", 8);
    snx_st_le32(icv + 8, 1), snx_st_le32(icv + 12, PAGE), snx_st_le32(icv + 16, ~0u), snx_st_le32(icv + 20, ~0u);
    snx_st_le64(icv + 24, icv_size - PAGE);
    for (unsigned i = 0; i < count; ++i) {
        Item* it = &items[i];
        if (it->type == 0) continue; /* an entry with nothing behind it has no table */
        uint32_t sectors = (it->size + SECTOR - 1) / SECTOR;
        unsigned char* t = icv + (size_t)it->index * PAGE;
        memcpy(t, "SCEIFTBL", 8);
        snx_st_le32(t + 8, l.table_version), snx_st_le32(t + 12, PAGE), snx_st_le32(t + 16, 50);
        snx_st_le32(t + 20, sectors), snx_st_le32(t + 24, SECTOR);
        for (unsigned b = 0; b < 20; ++b) t[0x34 + b] = (unsigned char)(0xc3 + b * 7 + i);
        if (!it->disk || it->type & 0x8000) continue;
        unsigned char s[20], mask[20], salt[8];
        secret(key, l.salt, it->index, s);
        if (l.table_version > 1)
            hmac(g_vita_decrypt.keys.bytes[SNX_VITA_KEY_PFS_HMAC0], 20, t + 0x34, 20, mask);
        else
            hmac(g_vita_decrypt.keys.bytes[SNX_VITA_KEY_PFS_HMAC0], 20, salt, salt_bytes(l.salt, it->index, salt),
                 mask);
        unsigned char* out = malloc(it->size ? it->size : 1);
        assert(out);
        memcpy(out, it->data, it->size);
        if (g_vita_decrypt.plain[0]) {
            char plain[SNX_PATH_SIZE];
            snprintf(plain, sizeof plain, "%s/%s", g_vita_decrypt.plain, snx_path_leaf(root));
            put(plain, it->disk, it->data, it->size);
        }
        for (uint32_t sec = 0; sec < sectors; ++sec) {
            size_t n = sec + 1 < sectors ? SECTOR : it->size - (size_t)sec * SECTOR;
            unsigned char* d = out + (size_t)sec * SECTOR;
            if (!(it->type & 0x4000)) {
                unsigned char iv[16] = {0}, pad[16];
                size_t whole = n & ~(size_t)15;
                snx_st_le64(iv, (uint64_t)sec * SECTOR);
                for (int b = 0; b < 16; ++b) iv[b] ^= mask[b];
                if (whole) crypt(EVP_aes_128_cbc(), 1, key, iv, d, d, whole);
                crypt(EVP_aes_128_ecb(), 1, key, NULL, whole ? d + whole - 16 : iv, pad, 16);
                for (size_t b = whole; b < n; ++b) d[b] ^= pad[b - whole];
            }
            unsigned char index[4], k[20];
            snx_st_le32(index, sec);
            hmac(s, 20, index, 4, k);
            unsigned char* g = t + (size_t)(1 + sec / 50) * PAGE;
            if (sec % 50 == 0)
                snx_st_le32(g, 0x3f8), snx_st_le32(g + 4, 20),
                    snx_st_le32(g + 8, sectors - sec < 50 ? sectors - sec : 50);
            hmac(k, 20, d, n, g + 16 + (sec % 50) * 20);
        }
        put(root, it->disk, out, it->size);
        free(out);
    }
    put(root, "sce_pfs/unicv.db", icv, icv_size);
    free(icv);
    /* files.db: the header, then the pages */
    unsigned pages = count <= 9 ? 1 : 3, root_page = pages - 1;
    size_t db_size = (size_t)(1 + pages) * PAGE;
    unsigned char* db = calloc(1, db_size);
    assert(db);
    unsigned char *h = db, s0[20];
    secret(key, l.salt, 0, s0);
    if (pages == 1) {
        unsigned char* g = db + PAGE;
        snx_st_le32(g, ~0u), snx_st_le32(g + 8, count);
        for (unsigned i = 0; i < count; ++i) entry(g, i, items, &items[i]);
        page_icv(g, count, s0, h + 0x38);
    } else {
        unsigned char *a = db + PAGE, *b = db + 2 * PAGE, *r = db + 3 * PAGE;
        unsigned left = (count - 1 + 1) / 2;
        snx_st_le32(a, root_page), snx_st_le32(a + 8, left);
        snx_st_le32(b, root_page), snx_st_le32(b + 8, count - 1 - left);
        snx_st_le32(r, ~0u), snx_st_le32(r + 4, 1), snx_st_le32(r + 8, 1);
        entry(r, 0, items, &items[0]);
        for (unsigned i = 1; i < count; ++i)
            entry(i <= left ? a : b, i <= left ? i - 1 : i - 1 - left, items, &items[i]);
        page_icv(a, left, s0, r + 0x338);
        page_icv(b, count - 1 - left, s0, r + 0x338 + 20);
        page_icv(r, 2, s0, h + 0x38);
    }
    memcpy(h, "SCENGPFS", 8);
    snx_st_le32(h + 8, l.version), snx_st_le16(h + 12, (uint16_t)l.spec), snx_st_le32(h + 16, PAGE);
    snx_st_le32(h + 20, 10), snx_st_le32(h + 24, root_page), snx_st_le32(h + 28, l.salt);
    snx_st_le64(h + 32, ~0ull), snx_st_le64(h + 40, db_size - PAGE);
    unsigned char signed_part[0x160] = {0};
    memcpy(signed_part, h, 0x4c);
    hmac(s0, 20, signed_part, sizeof signed_part, h + 0x4c);
    put(root, "sce_pfs/files.db", db, db_size);
    free(db);
}

/* ---- the SELFs ---- */
/* a module the loader takes: one segment at 0x81000000, its module info at 0 with no imports or exports */
#define MODULE_SIZE 0x300u
static void module_bytes(unsigned char* m, const char* name) {
    for (unsigned i = 0; i < MODULE_SIZE; ++i) m[i] = (unsigned char)(i * 13 + 5);
    memset(m, 0, 0x5c);
    memcpy(m + 4, name, strlen(name));
    snx_st_le32(m + 68, 0x101); /* module_start, Thumb */
}
/* a SELF of module under revision and system: its segment deflated, then encrypted; into *size bytes */
static unsigned char* make_self(const unsigned char* module, const unsigned char klicensee[16], unsigned revision,
                                uint64_t system, size_t* size) {
    unsigned char packed[MODULE_SIZE + 256];
    size_t packed_size = tdefl_compress_mem_to_mem(packed, sizeof packed, module, MODULE_SIZE,
                                                   TDEFL_WRITE_ZLIB_HEADER | TDEFL_MAX_PROBES_MASK);
    assert(packed_size && packed_size < sizeof packed);
    const uint32_t meta = 0x120, header = 0x150 + 0x40 + 0x70, data = header;
    *size = data + packed_size;
    unsigned char* f = calloc(1, *size);
    assert(f);
    snx_st_le32(f, 0x00454353u), snx_st_le32(f + 4, 3), f[8] = 0x40, f[9] = (unsigned char)revision;
    snx_st_le16(f + 10, 1), snx_st_le32(f + 12, meta), snx_st_le64(f + 16, header), snx_st_le64(f + 24, packed_size);
    snx_st_le64(f + 0x20, *size), snx_st_le64(f + 0x38, 0x80), snx_st_le64(f + 0x40, 0xa0);
    snx_st_le64(f + 0x48, 0xe0), snx_st_le64(f + 0x58, 0x100);
    snx_st_le64(f + 0x80, 0x2f00000000000001ull), snx_st_le32(f + 0x8c, 8), snx_st_le64(f + 0x90, system);
    unsigned char* e = f + 0xa0;
    memcpy(e,
           "\x7f"
           "ELF\x01\x01\x01",
           7);
    snx_st_le16(e + 16, 0xfe00), snx_st_le16(e + 18, 40), snx_st_le32(e + 20, 1), snx_st_le32(e + 24, 0);
    snx_st_le32(e + 28, 0x34), snx_st_le16(e + 40, 0x34), snx_st_le16(e + 42, 0x20), snx_st_le16(e + 44, 1);
    unsigned char* ph = f + 0xe0;
    snx_st_le32(ph, 1), snx_st_le32(ph + 4, data), snx_st_le32(ph + 8, 0x81000000u), snx_st_le32(ph + 16, MODULE_SIZE);
    snx_st_le32(ph + 20, MODULE_SIZE), snx_st_le32(ph + 24, 5), snx_st_le32(ph + 28, 0x1000);
    unsigned char* seg = f + 0x100;
    snx_st_le64(seg, data), snx_st_le64(seg + 8, packed_size), snx_st_le64(seg + 16, 2), snx_st_le64(seg + 24, 1);
    /* the metadata: its header, one section (segment 0, AES-128-CTR, keys 0 and 1), the two keys */
    unsigned char rest[0x70] = {0}, block[0x40] = {0}, segment_key[16], counter[16];
    for (int i = 0; i < 16; ++i)
        segment_key[i] = (unsigned char)(0x11 * i + revision), counter[i] = (unsigned char)(0xf0 - i);
    snx_st_le32(rest + 12, 1), snx_st_le32(rest + 16, 2);
    unsigned char* sec = rest + 0x20;
    snx_st_le64(sec, data), snx_st_le64(sec + 8, packed_size), snx_st_le32(sec + 16, 2), snx_st_le32(sec + 20, 0);
    snx_st_le32(sec + 32, 3), snx_st_le32(sec + 36, 0), snx_st_le32(sec + 40, 1), snx_st_le32(sec + 44, 2);
    memcpy(rest + 0x50, segment_key, 16), memcpy(rest + 0x60, counter, 16);
    for (int i = 0; i < 16; ++i) block[i] = (unsigned char)(0x21 + i), block[32 + i] = (unsigned char)(0x61 + i);
    int k = revision == 0 && system <= 0x16920000000ull ? SNX_VITA_KEY_APP0_100
            : revision == 0                             ? SNX_VITA_KEY_APP0_180
                                                        : SNX_VITA_KEY_APP1 + 2 * ((int)revision - 1);
    int npdrm = revision >= 2 ? SNX_VITA_KEY_NPDRM1 : SNX_VITA_KEY_NPDRM0;
    const unsigned char *np = g_vita_decrypt.keys.bytes[npdrm], *np_iv = g_vita_decrypt.keys.bytes[npdrm + 1];
    unsigned char predec[16], once[0x40];
    crypt(EVP_aes_128_cbc(), 0, np, np_iv, klicensee, predec, 16);
    crypt(EVP_aes_256_cbc(), 1, g_vita_decrypt.keys.bytes[k], g_vita_decrypt.keys.bytes[k + 1], block, once, 0x40);
    crypt(EVP_aes_128_cbc(), 1, predec, np_iv, once, f + meta + 0x30, 0x40);
    crypt(EVP_aes_128_cbc(), 1, block, block + 32, rest, f + meta + 0x70, sizeof rest);
    crypt(EVP_aes_128_ctr(), 1, segment_key, counter, packed, f + data, packed_size);
    return f;
}

/* ---- the dump ---- */
static unsigned char* make_sfo(const char* content, size_t* size) {
    static const char keys[] = "CONTENT_ID\0TITLE_ID\0";
    *size = 20 + 2 * 16 + sizeof keys + 4 + 48 + 12;
    unsigned char* s = calloc(1, *size);
    assert(s);
    uint32_t key_table = 20 + 32, data_table = key_table + (uint32_t)sizeof keys + 4;
    snx_st_le32(s, 0x46535000u), snx_st_le32(s + 4, 0x0101), snx_st_le32(s + 8, key_table),
        snx_st_le32(s + 12, data_table);
    snx_st_le32(s + 16, 2);
    snx_st_le16(s + 20, 0), snx_st_le16(s + 22, 0x0204), snx_st_le32(s + 24, 37), snx_st_le32(s + 28, 48);
    snx_st_le32(s + 32, 0);
    snx_st_le16(s + 36, 11), snx_st_le16(s + 38, 0x0204), snx_st_le32(s + 40, 10), snx_st_le32(s + 44, 12);
    snx_st_le32(s + 48, 48);
    memcpy(s + key_table, keys, sizeof keys);
    memcpy(s + data_table, content, 36), memcpy(s + data_table + 48, TITLE, 9);
    return s;
}
static void make_rif(const char* content, const unsigned char klicensee[16], unsigned char rif[512]) {
    memset(rif, 0, 512);
    memcpy(rif, "\0\1\0\1\0\1\0\2", 8);
    snx_st_le64(rif + 8, VITA_LICENSE_NONPDRM);
    memcpy(rif + 0x10, content, strlen(content));
    memcpy(rif + 0x50, klicensee, 16);
}
#define BIG   (51u * SECTOR + 100u)
#define MIXED 70000u
static void make_dump(const char* dump) {
    char app[SNX_PATH_SIZE];
    snprintf(app, sizeof app, "%s/app/" TITLE, dump);
    size_t sfo_size;
    unsigned char *sfo = make_sfo(GAME, &sfo_size), rif[512];
    Item items[] = {
        {"eboot.bin", "eboot.bin", 0x0001, -1, g_vita_decrypt.eboot, (uint32_t)g_vita_decrypt.eboot_size, 0},
        {"sce_sys", "sce_sys", 0x8000, -1, NULL, 0, 0},
        {"param.sfo", "sce_sys/param.sfo", 0x4006, 1, sfo, (uint32_t)sfo_size, 0},
        {"icon0.png", "sce_sys/icon0.png", 0x0006, 1, g_vita_decrypt.icon, sizeof g_vita_decrypt.icon, 0},
        {"sce_module", "sce_module", 0x8000, -1, NULL, 0, 0},
        {"libtest.suprx", "sce_module/libtest.suprx", 0x0001, 4, g_vita_decrypt.module,
         (uint32_t)g_vita_decrypt.module_size, 0},
        {"Data", "data", 0x8000, -1, NULL, 0, 0},
        {"big.bin", "data/big.bin", 0x0001, 6, g_vita_decrypt.big, BIG, 0},
        {"Mixed.BIN", "data/mixed.bin", 0x0007, 6, g_vita_decrypt.mixed, MIXED, 0},
        {"empty.txt", "data/empty.txt", 0x0001, 6, (const unsigned char*)"", 0, 0},
        {"save", NULL, 0x8000, -1, NULL, 0, 0},
        {"ghost", NULL, 0x0000, -1, NULL, 0, 0},
    };
    make_pfs(app, items, sizeof items / sizeof *items, g_vita_decrypt.game_key, (Layout){3, 1, 1, 0});
    put(app, "data/empty.txt", "", 0);
    put(app, "readme_extra.txt", "not in files.db", 15);
    make_rif(GAME, g_vita_decrypt.game_key, rif);
    put(app, "sce_sys/package/work.bin", rif, sizeof rif);
    free(sfo);
    /* the add-ons: the first with its license, the second without; the add-ons' root its own PFS, left alone */
    const char* addons[] = {ADDON1, ADDON2};
    for (int a = 0; a < 2; ++a) {
        char folder[SNX_PATH_SIZE];
        snprintf(folder, sizeof folder, "%s/addcont/" TITLE "/%s", dump, addons[a] + 20);
        sfo = make_sfo(addons[a], &sfo_size);
        Item addon[] = {
            {"sce_sys", "sce_sys", 0x8000, -1, NULL, 0, 0},
            {"param.sfo", "sce_sys/param.sfo", 0x4006, 0, sfo, (uint32_t)sfo_size, 0},
            {"addon.dat", "addon.dat", 0x0001, -1, g_vita_decrypt.addon, sizeof g_vita_decrypt.addon, 0},
        };
        make_pfs(folder, addon, 3, g_vita_decrypt.addon_key, (Layout){4, 4, 2, 0x1234});
        free(sfo);
    }
    char license[SNX_PATH_SIZE];
    snprintf(license, sizeof license, "%s/license/addcont/" TITLE "/" ADDON1_ID, dump);
    make_rif(ADDON1, g_vita_decrypt.addon_key, rif);
    put(license, "6488b73b912a753a492e2714e9b38bc7.rif", rif, sizeof rif);
    char root[SNX_PATH_SIZE];
    snprintf(root, sizeof root, "%s/addcont/" TITLE, dump);
    put(root, "sce_pfs/icv.db/0000.icv", "x", 1);
    put(root, "sce_sys/keystone", "k", 1);
}

/* ---- checks ---- */
typedef struct {
    char text[16384];
    unsigned errors;
} Said;
static void said(void* context, const char* line, bool error) {
    Said* s = context;
    size_t n = strlen(s->text);
    snprintf(s->text + n, sizeof s->text - n, "%s%s\n", error ? "ERROR " : "", line);
    s->errors += error;
}
static void same(const char* folder, const char* rel, const void* data, size_t n) {
    char* path = snx_path_join(folder, rel);
    size_t size = 0;
    unsigned char* got = snx_read_file(path, &size);
    if (!got || size != n || memcmp(got, data, n)) {
        fprintf(stderr, "%s: %zu bytes, not the %zu expected, or other bytes\n", path, got ? size : 0, n);
        abort();
    }
    free(got);
    free(path);
}
static bool there(const char* folder, const char* rel) {
    char* path = snx_path_join(folder, rel);
    bool is = snx_path_exists(path);
    free(path);
    return is;
}
static bool import(const char* dump, const char* folder, const char* keys, const char* zrif, Said* s, char* error,
                   size_t error_size) {
    char app[SNX_PATH_SIZE];
    snprintf(app, sizeof app, "%s/app/" TITLE, dump);
    *s = (Said){0};
    VitaDumpImport d = {app, NULL, TITLE, folder, keys, zrif, said, s};
    bool ok = vita_dump_import(&d, error, error_size);
    if (getenv("VITA_DECRYPT_VERBOSE")) printf("%s%s%s\n", s->text, ok ? "" : "FAILED: ", ok ? "" : error);
    return ok;
}
static void expect(bool ok, const char* error, const char* part) {
    if (ok || !strstr(error, part)) {
        fprintf(stderr, "expected a failure with \"%s\", got %s: %s\n", part, ok ? "success" : "failure", error);
        abort();
    }
}

static void keys_files(const char* dir) {
    VitaKeys k;
    char error[1024], path[SNX_PATH_SIZE], file[SNX_PATH_SIZE];
    snprintf(path, sizeof path, "%s/keys-check", dir);
    write_keys(path, SNX_VITA_KEY_COUNT);
    assert(vita_keys_load(path, &k, error, sizeof error) && k.have == g_vita_decrypt.keys.have);
    for (int key = 0; key < SNX_VITA_KEY_COUNT; ++key)
        assert(!memcmp(k.bytes[key], g_vita_decrypt.keys.bytes[key], snx_vita_key_size(key)));
    write_keys(path, SNX_VITA_KEY_NPDRM0_IV);
    assert(vita_keys_load(path, &k, error, sizeof error));
    assert(!vita_key(&k, SNX_VITA_KEY_NPDRM0_IV, "eboot.bin", error, sizeof error));
    assert(strstr(error, "/keys-check has no npdrm-iv-0 (a file of 16 bytes), which eboot.bin needs"));
    snprintf(file, sizeof file, "%s/app-key-3", path); /* a key of another size: named, its size and the key's */
    assert(!snx_write_file(file, "short", 5));
    expect(vita_keys_load(path, &k, error, sizeof error), error,
           "/keys-check/app-key-3 is 5 bytes, and the key app-key-3 is 32");
    snprintf(path, sizeof path, "%s/none", dir);
    expect(vita_keys_load(path, &k, error, sizeof error), error, "there are no Vita keys to decrypt it with in");
    assert(strstr(error, "/none (") && strstr(error, "Install Keys puts them in ~/.vita, a file for each key"));
    assert(snx_vita_key_named("pfs-contract-key-0") == SNX_VITA_KEY_PFS_CONTRACT0 &&
           snx_vita_key_named("gpkg-key-2") < 0);
}
static void licenses(void) {
    VitaLicense l;
    char error[512];
    unsigned char rif[512];
    make_rif(GAME, g_vita_decrypt.game_key, rif);
    assert(vita_license_parse(rif, 512, "work.bin", &l, error, sizeof error) && !strcmp(l.content_id, GAME));
    assert(!memcmp(l.klicensee, g_vita_decrypt.game_key, 16) && l.account == VITA_LICENSE_NONPDRM);
    expect(vita_license_parse(rif, 0x98, "x.rif", &l, error, sizeof error), error, "x.rif is 152 bytes, not the 512");
    assert(strstr(error, "PSP"));
    rif[0x10] = 1;
    expect(vita_license_parse(rif, 512, "x.rif", &l, error, sizeof error), error, "x.rif has no content ID at 0x10");
    assert(vita_license_zrif(GAME_ZRIF, &l, error, sizeof error) && !strcmp(l.content_id, GAME));
    assert(!memcmp(l.klicensee, g_vita_decrypt.game_key, 16));
    char zrif[] = GAME_ZRIF;
    zrif[10] = '*';
    expect(vita_license_zrif(zrif, &l, error, sizeof error), error, "the zRIF has '*' at character 11");
    snprintf(zrif, sizeof zrif, "%s", GAME_ZRIF);
    zrif[strlen(zrif) - 4] = zrif[strlen(zrif) - 4] == 'A' ? 'B' : 'A'; /* its Adler-32 */
    expect(vita_license_zrif(zrif, &l, error, sizeof error), error, "the zRIF fails its Adler-32");
}

int main(void) {
    test_keys();
    const char* make_only = getenv("VITA_DECRYPT_MAKE");
    for (int i = 0; i < 16; ++i)
        g_vita_decrypt.game_key[i] = (unsigned char)(0xa0 + i), g_vita_decrypt.addon_key[i] = (unsigned char)(0x3c ^ i);
    g_vita_decrypt.big = malloc(BIG + MIXED); /* the two files' plaintext, one after the other */
    g_vita_decrypt.mixed = g_vita_decrypt.big + BIG;
    assert(g_vita_decrypt.big && g_vita_decrypt.mixed);
    for (size_t i = 0; i < BIG; ++i) g_vita_decrypt.big[i] = (unsigned char)(i * 2654435761u >> 13);
    for (size_t i = 0; i < MIXED; ++i) g_vita_decrypt.mixed[i] = (unsigned char)(i % 253);
    for (size_t i = 0; i < sizeof g_vita_decrypt.icon; ++i) g_vita_decrypt.icon[i] = (unsigned char)(0x89 + i);
    for (size_t i = 0; i < sizeof g_vita_decrypt.addon; ++i) g_vita_decrypt.addon[i] = (unsigned char)(i * 7);
    unsigned char eboot_module[MODULE_SIZE], lib_module[MODULE_SIZE];
    module_bytes(eboot_module, "SnxTestGame");
    module_bytes(lib_module, "SnxTestLib");
    g_vita_decrypt.eboot =
        make_self(eboot_module, g_vita_decrypt.game_key, 2, 0x36000000000ull, &g_vita_decrypt.eboot_size);
    g_vita_decrypt.module =
        make_self(lib_module, g_vita_decrypt.game_key, 0, 0x16000000000ull, &g_vita_decrypt.module_size);

    if (make_only) { /* the dump alone, its keys and each file's plaintext in plain/, for psvpfsparser to read */
        char at[SNX_PATH_SIZE];
        snprintf(g_vita_decrypt.plain, sizeof g_vita_decrypt.plain, "%s/plain", make_only);
        snprintf(at, sizeof at, "%s/dump", make_only);
        make_dump(at);
        snprintf(at, sizeof at, "%s/keys", make_only);
        write_keys(at, SNX_VITA_KEY_COUNT);
        printf("vita_decrypt: the dump in %s/dump, its plaintext in %s\n", make_only, g_vita_decrypt.plain);
        return 0;
    }
    char *dir = snx_temp_dir(NULL), dump[SNX_PATH_SIZE], keys[SNX_PATH_SIZE], game[SNX_PATH_SIZE], error[2048];
    assert(dir);
    keys_files(dir);
    licenses();
    snprintf(dump, sizeof dump, "%s/dump", dir);
    snprintf(keys, sizeof keys, "%s/keys", dir);
    snprintf(game, sizeof game, "%s/games/" TITLE, dir);
    make_dump(dump);
    write_keys(keys, SNX_VITA_KEY_COUNT);
    Said s;
    char work[SNX_PATH_SIZE], moved[SNX_PATH_SIZE], fresh[SNX_PATH_SIZE];

    /* the import */
    assert(import(dump, game, keys, NULL, &s, error, sizeof error));
    char app[SNX_PATH_SIZE];
    snprintf(app, sizeof app, "%s/app", game);
    same(app, "data/big.bin", g_vita_decrypt.big, BIG);
    same(app, "data/mixed.bin", g_vita_decrypt.mixed, MIXED);
    same(app, "data/empty.txt", "", 0);
    same(app, "sce_sys/icon0.png", g_vita_decrypt.icon, sizeof g_vita_decrypt.icon);
    {
        size_t n;
        unsigned char* sfo = make_sfo(GAME, &n);
        same(app, "sce_sys/param.sfo", sfo, n);
        free(sfo);
    }
    assert(there(app, "save") && !there(app, "readme_extra.txt") && !there(app, "sce_pfs") &&
           !there(app, "sce_sys/package"));
    assert(!there(app, "ghost") && !there(game, ".app.decrypting"));
    assert(strstr(s.text, "readme_extra.txt is not in its files.db, and the import leaves it out"));
    assert(strstr(s.text, "SELFs decrypted: eboot.bin, sce_module/libtest.suprx"));
    {
        VitaImage image;
        char eboot[SNX_PATH_SIZE];
        snprintf(eboot, sizeof eboot, "%s/eboot.bin", app);
        if (!vita_image_load(eboot, &image, error, sizeof error)) {
            fprintf(stderr, "the decrypted eboot.bin does not load: %s\n", error);
            abort();
        }
        assert(image.segment_count == 1 && image.segment[0].address == 0x81000000u &&
               !strcmp(image.name, "SnxTestGame"));
        assert(!memcmp(image.segment[0].bytes, eboot_module, MODULE_SIZE));
        vita_image_free(&image);
        snprintf(eboot, sizeof eboot, "%s/sce_module/libtest.suprx", app);
        assert(vita_image_load(eboot, &image, error, sizeof error) &&
               !memcmp(image.segment[0].bytes, lib_module, MODULE_SIZE));
        vita_image_free(&image);
        assert(vita_self_decrypt(eboot, "libtest.suprx", g_vita_decrypt.game_key, &g_vita_decrypt.keys, error,
                                 sizeof error) == 0); /* plain already */
    }
    char addon[SNX_PATH_SIZE];
    snprintf(addon, sizeof addon, "%s/ux0/addcont/" TITLE "/" ADDON1_ID, game);
    same(addon, "addon.dat", g_vita_decrypt.addon, sizeof g_vita_decrypt.addon);
    assert(s.errors == 1 && strstr(s.text, "SCARLETNXADDON02 is not imported: it has no license in"));
    assert(strstr(s.text, "1 of the dump's 2 add-ons are in"));
    snprintf(addon, sizeof addon, "%s/ux0/addcont/" TITLE "/" ADDON2_ID, game);
    assert(!snx_path_exists(addon));

    /* again: the decryption kept, the add-on too */
    assert(import(dump, game, keys, NULL, &s, error, sizeof error));
    assert(strstr(s.text, "Using the decryption in") && strstr(s.text, "Using the add-on in") &&
           !strstr(s.text, "Decrypting"));

    /* a game folder whose app/ links the dump, as an earlier import made one: replaced, the dump as it was */
    char linked[SNX_PATH_SIZE], link[SNX_PATH_SIZE], source[SNX_PATH_SIZE];
    snprintf(linked, sizeof linked, "%s/linked/" TITLE, dir);
    snprintf(link, sizeof link, "%s/app", linked);
    snprintf(source, sizeof source, "%s/app/" TITLE, dump);
    assert(!snx_mkdirs(linked));
    if (!snx_symlink(source, link)) {
        size_t before_size = 0, after_size = 0;
        char big[SNX_PATH_SIZE];
        snprintf(big, sizeof big, "%s/data/big.bin", source);
        unsigned char* before = snx_read_file(big, &before_size);
        assert(import(dump, linked, keys, NULL, &s, error, sizeof error));
        assert(!snx_is_link(link) && snx_is_dir(link));
        same(link, "data/big.bin", g_vita_decrypt.big, BIG);
        unsigned char* after = snx_read_file(big, &after_size);
        assert(before && after && before_size == after_size && !memcmp(before, after, before_size));
        assert(vita_dump_encrypted(source));
        free(before);
        free(after);
    }

    /* a zRIF given for a dump with its own license: not read, said; the license from it once the work.bin is gone */
    snprintf(fresh, sizeof fresh, "%s/zrif-not/" TITLE, dir);
    assert(import(dump, fresh, keys, GAME_ZRIF, &s, error, sizeof error));
    assert(strstr(s.text, "--zrif is not read: the dump has the game's license, ") &&
           strstr(s.text, "/sce_sys/package/work.bin (" GAME ")"));
    snprintf(work, sizeof work, "%s/app/" TITLE "/sce_sys/package/work.bin", dump);
    snprintf(moved, sizeof moved, "%s/work.bin", dir);
    assert(!rename(work, moved));
    snprintf(fresh, sizeof fresh, "%s/nolicense/" TITLE, dir);
    expect(import(dump, fresh, keys, NULL, &s, error, sizeof error), error, "is encrypted, and it has no license: no ");
    assert(strstr(error, "*.rif and no --zrif"));
    assert(!there(fresh, "app"));
    snprintf(fresh, sizeof fresh, "%s/zrif/" TITLE, dir);
    assert(import(dump, fresh, keys, GAME_ZRIF, &s, error, sizeof error));
    snprintf(app, sizeof app, "%s/app", fresh);
    same(app, "data/mixed.bin", g_vita_decrypt.mixed, MIXED);
    assert(!rename(moved, work));

    /* another content's license */
    unsigned char rif[512], other[16];
    for (int i = 0; i < 16; ++i) other[i] = (unsigned char)(0x55 + i);
    make_rif(GAME, other, rif);
    put(dump, "app/" TITLE "/sce_sys/package/work.bin", rif, sizeof rif);
    snprintf(fresh, sizeof fresh, "%s/wrong/" TITLE, dir);
    expect(import(dump, fresh, keys, NULL, &s, error, sizeof error), error,
           "sce_pfs/files.db does not verify with the license's klicensee");
    assert(!there(fresh, "app") && !there(fresh, ".app.decrypting"));
    make_rif(GAME, g_vita_decrypt.game_key, rif);
    put(dump, "app/" TITLE "/sce_sys/package/work.bin", rif, sizeof rif);

    /* a key the keys' folder lacks, and no folder at all */
    write_keys(keys, SNX_VITA_KEY_PFS_CONTRACT0);
    snprintf(fresh, sizeof fresh, "%s/nokey/" TITLE, dir);
    expect(import(dump, fresh, keys, NULL, &s, error, sizeof error), error,
           "/keys has no pfs-contract-key-0 (a file of 16 bytes), which the files' key needs");
    write_keys(keys, SNX_VITA_KEY_APP2);
    expect(import(dump, fresh, keys, NULL, &s, error, sizeof error), error,
           "/keys has no app-key-2 (a file of 32 bytes), which");
    assert(strstr(error, "eboot.bin of ") && strstr(error, "/app/" TITLE " (key revision 2) needs") &&
           !there(fresh, "app"));
    snprintf(work, sizeof work, "%s/missing", dir);
    expect(import(dump, fresh, work, NULL, &s, error, sizeof error), error, "is encrypted, and there are no Vita keys");
    write_keys(keys, SNX_VITA_KEY_COUNT);

    /* a sector changed: its file and sector named */
    char big[SNX_PATH_SIZE];
    snprintf(big, sizeof big, "%s/app/" TITLE "/data/big.bin", dump);
    FILE* f = fopen(big, "r+b");
    assert(f && !fseek(f, 51l * SECTOR + 3, SEEK_SET));
    int byte = fgetc(f);
    assert(byte != EOF && !fseek(f, 51l * SECTOR + 3, SEEK_SET) && fputc(byte ^ 1, f) != EOF && !fclose(f));
    snprintf(fresh, sizeof fresh, "%s/changed/" TITLE, dir);
    expect(import(dump, fresh, keys, NULL, &s, error, sizeof error), error,
           "data/big.bin's sector 51 (bytes 0x198000 to 0x198064) does not match its signature in unicv.db");
    assert(!there(fresh, "app") && !there(fresh, ".app.decrypting"));

    /* the decrypted SELF's metadata under another klicensee */
    {
        size_t n;
        unsigned char* self = make_self(eboot_module, g_vita_decrypt.game_key, 2, 0x36000000000ull, &n);
        char path[SNX_PATH_SIZE];
        snprintf(path, sizeof path, "%s/other.self", dir);
        assert(!snx_write_file(path, self, n));
        assert(vita_self_decrypt(path, "other.self", other, &g_vita_decrypt.keys, error, sizeof error) < 0);
        assert(
            strstr(error, "other.self: its metadata does not decrypt with the license's klicensee and app-key-2"));
        free(self);
    }
    if (!getenv("VITA_DECRYPT_KEEP")) snx_remove_tree(dir);
    free(dir);
    free(g_vita_decrypt.big);
    free(g_vita_decrypt.eboot);
    free(g_vita_decrypt.module);
    puts("vita_decrypt: ok");
    return 0;
}
