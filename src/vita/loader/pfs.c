/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "vita/loader/pfs.h"
#include "vita/loader/crypto.h"
#include "common/utilities/ascii.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE       0x400u
#define SECTOR     0x8000u
#define SIGNATURES 50u /* a signature page's: (PAGE - its 16-byte header) / 20 */
#define ORDER      10u /* files.db's B-tree: 9 entries a page, and a child more */
#define ENTRIES    9u
#define NO_PAGE    0xffffffffu
#define DEPTH      64u /* the folders a path goes down at most: a deeper one is a loop */

/* files.db's header, its first page (psvpfsparser's sce_ng_pfs_header_t) */
#define DB_VERSION    0x08u
#define DB_IMAGE_SPEC 0x0cu /* 1 a game's, 4 an add-on's; 2 and 3 savedata and the add-ons' root, keyed to a console */
#define DB_KEY_ID     0x0eu
#define DB_PAGE_SIZE  0x10u
#define DB_ORDER      0x14u
#define DB_ROOT       0x18u
#define DB_SALT       0x1cu /* 0 before version 4 */
#define DB_TAIL       0x28u
#define DB_ROOT_ICV   0x38u
#define DB_HEADER_ICV 0x4cu
#define DB_SIGNED     0x160u /* the bytes its ICV covers, the ICV and the RSA signature after it zeroed */
/* a page: its header, each entry's parent folder and name, each entry's index, type and size, the ICVs */
#define PG_PARENT     0x00u
#define PG_TYPE       0x04u /* 1 a page with children */
#define PG_COUNT      0x08u
#define PG_NAMES      0x10u  /* 72 each: parent u32, name[68] */
#define PG_INFOS      0x298u /* 16 each: index u32, type u16, 0, size u32, 0 */
#define PG_ICVS       0x338u /* 20 each, 10 */
#define NAME          68u
/* unicv.db: a header page, then each file's table, SCEIFTBL, a page and its signature pages */
#define TB_VERSION    0x08u
#define TB_PAGE       0x0cu
#define TB_MAX        0x10u
#define TB_SECTORS    0x14u
#define TB_SECTOR     0x18u
#define TB_PAD        0x1cu
#define TB_DBSEED     0x34u /* the mask's seed in a version 2 table */
/* the entries' types (psvpfsparser's sce_ng_pfs_file_types): 0x8000 a folder, 0x4000 a file kept plain */
#define T_NONE        0x0000u
#define T_FILE        0x0001u
#define T_SYS_RW      0x0006u
#define T_SYS_RO      0x0007u
#define T_PLAIN_RW    0x4006u
#define T_PLAIN_RO    0x4007u
#define T_DIR         0x8000u
#define T_SYS_DIR     0x8006u
#define T_ACID_DIR    0x9004u
static_assert(PG_ICVS + ORDER * 20u == PAGE, "a files.db page ends with its ICVs");
static_assert(16u + SIGNATURES * 20u <= PAGE, "a signature page holds 50");

typedef struct {
    char name[NAME + 1];
    uint16_t type;
    uint32_t parent, index, size;
} Entry;
typedef struct {
    uint32_t index, entry; /* a folder's index, its entry */
} Folder;
typedef struct {
    uint32_t page, sectors, version, at; /* at: the table's page's offset in unicv.db */
} Table;
/* the decryption's state: the two databases read whole, their entries and tables in one block */
typedef struct {
    VitaCrypto crypto;
    unsigned char key[16]; /* the files' AES key: the klicensee decrypted with pfs_contract_key0 */
    const unsigned char *hmac0, *hmac1, *iv0;
    uint32_t salt, pages;
    unsigned char *db, *icv;
    size_t db_size, icv_size;
    void* block;
    Entry* entries;
    Folder* folders;
    Table* tables;
    uint32_t entry_count, folder_count, table_count;
    const char* source;
    char* error;
    size_t error_size;
} Pfs;

[[gnu::format(printf, 2, 3)]] static bool fail(Pfs* p, const char* fmt, ...) {
    va_list list;
    va_start(list, fmt);
    vsnprintf(p->error, p->error_size, fmt, list);
    va_end(list);
    return false;
}
bool vita_pfs_present(const char* folder) {
    char path[SNX_PATH_SIZE];
    return !snx_path_format(path, sizeof path, "%s/sce_pfs/files.db", folder) && snx_is_file(path);
}

static bool is_folder(uint32_t type) { return type == T_DIR || type == T_SYS_DIR || type == T_ACID_DIR; }
static bool is_plain(uint32_t type) { return type == T_PLAIN_RW || type == T_PLAIN_RO; }
static bool known_type(uint32_t type) {
    return type == T_NONE || type == T_FILE || type == T_SYS_RW || type == T_SYS_RO || is_plain(type) ||
           is_folder(type);
}
/* the salt a table's keys come from: its page, after files.db's salt when it has one */
static size_t salt_of(const Pfs* p, uint32_t page, unsigned char out[8]) {
    if (!p->salt) {
        snx_st_le32(out, page);
        return 4;
    }
    snx_st_le32(out, p->salt), snx_st_le32(out + 4, page);
    return 8;
}
/* the integrity secret of the table at page (0 for files.db): pfs_iv0's AES-CBC of the salt's HMAC under
   pfs_hmac_key1, its 4 bytes past the block XORed with the block encrypted again (psvpfsparser's generate_secret_np) */
static bool secret_of(Pfs* p, uint32_t page, unsigned char secret[20]) {
    unsigned char salt[8], combo[20], block[16], pad[16];
    size_t n = salt_of(p, page, salt);
    if (!vita_hmac_sha1(&p->crypto, p->hmac1, 20, salt, n, combo) ||
        !vita_crypt(&p->crypto, EVP_aes_128_cbc(), true, p->key, p->iv0, combo, block, 16) ||
        !vita_crypt(&p->crypto, EVP_aes_128_ecb(), true, p->key, NULL, block, pad, 16))
        return fail(p, "OpenSSL failed deriving the secret of page %u", page);
    memcpy(secret, block, 16);
    for (unsigned i = 0; i < 4; ++i) secret[16 + i] = combo[16 + i] ^ pad[i];
    return true;
}
/* the mask a table's sector IVs are XORed with: the HMAC of its salt (of its seed, in a version 2 table) under
   pfs_hmac_key0 */
static bool mask_of(Pfs* p, const Table* t, unsigned char mask[16]) {
    unsigned char salt[8], digest[20];
    size_t n = salt_of(p, t->page, salt);
    bool ok = t->version > 1 ? vita_hmac_sha1(&p->crypto, p->hmac0, 20, p->icv + t->at + TB_DBSEED, 20, digest)
                             : vita_hmac_sha1(&p->crypto, p->hmac0, 20, salt, n, digest);
    memcpy(mask, digest, 16);
    return ok || fail(p, "OpenSSL failed deriving the mask of unicv.db's page %u", t->page);
}
/* a files.db page's ICV: version 5's HMAC of the page past its parent, else the chain of HMACs of its entries' */
static bool page_icv(Pfs* p, const unsigned char* page, const unsigned char secret[20], unsigned char icv[20]) {
    if (snx_ld_le32(p->db + DB_VERSION) == 5) return vita_hmac_sha1(&p->crypto, secret, 20, page + 4, PAGE - 4, icv);
    uint32_t n = snx_ld_le32(page + PG_COUNT) + (snx_ld_le32(page + PG_TYPE) > 0);
    unsigned char chain[40] = {0};
    for (uint32_t i = 0; i < n && i < ORDER; ++i) {
        memcpy(chain + 20, page + PG_ICVS + i * 20, 20);
        if (!vita_hmac_sha1(&p->crypto, secret, 20, chain, 40, chain)) return false;
    }
    memcpy(icv, chain, 20);
    return true;
}
static int folder_order(const void* a, const void* b) {
    uint32_t x = ((const Folder*)a)->index, y = ((const Folder*)b)->index;
    return (x > y) - (x < y);
}
static int table_order(const void* a, const void* b) {
    uint32_t x = ((const Table*)a)->page, y = ((const Table*)b)->page;
    return (x > y) - (x < y);
}
static const Entry* folder_entry(const Pfs* p, uint32_t index) {
    Folder key = {index, 0};
    const Folder* f = bsearch(&key, p->folders, p->folder_count, sizeof *p->folders, folder_order);
    return f ? &p->entries[f->entry] : NULL;
}
static const Table* table_at(const Pfs* p, uint32_t page) {
    Table key = {page, 0, 0, 0};
    return bsearch(&key, p->tables, p->table_count, sizeof *p->tables, table_order);
}

/* files.db: the header's checks and ICV, each page's ICV against its parent's, every entry. 1, else 0 with the error
   said, or -1 when the header's ICV is not the klicensee's */
static int read_db(Pfs* p, const char* path) {
    const unsigned char* h = p->db;
    if (p->db_size < 2 * PAGE || memcmp(h, "SCENGPFS", 8))
        return fail(p, "%s is no files.db (%zu bytes, magic \"%.8s\")", path, p->db_size,
                    p->db_size >= 8 ? (const char*)h : "");
    uint32_t version = snx_ld_le32(h + DB_VERSION), spec = snx_ld_le16(h + DB_IMAGE_SPEC);
    if (version < 3 || version > 5)
        return fail(p, "%s is version %u, and psvpfsparser's reading has 3 to 5", path, version);
    if (spec != 1 && spec != 4)
        return fail(p, "%s is %s (image %u): only a game's (1) and an add-on's (4) decrypt with a license", path,
                    spec == 2   ? "savedata"
                    : spec == 3 ? "the add-ons' root, keyed to its console"
                                : "an image of no known kind",
                    spec);
    if (snx_ld_le16(h + DB_KEY_ID))
        return fail(p, "%s has key ID %u, and games have 0", path, snx_ld_le16(h + DB_KEY_ID));
    if (snx_ld_le32(h + DB_PAGE_SIZE) != PAGE || snx_ld_le32(h + DB_ORDER) != ORDER)
        return fail(p, "%s has pages of %u bytes and order %u, not %u and %u", path, snx_ld_le32(h + DB_PAGE_SIZE),
                    snx_ld_le32(h + DB_ORDER), PAGE, ORDER);
    if (snx_ld_le64(h + DB_TAIL) != p->db_size - PAGE || p->db_size % PAGE)
        return fail(p, "%s is %zu bytes, and its header says %llu after its first page", path, p->db_size,
                    (unsigned long long)snx_ld_le64(h + DB_TAIL));
    p->salt = snx_ld_le32(h + DB_SALT);
    p->pages = (uint32_t)((p->db_size - PAGE) / PAGE);
    unsigned char secret[20], signed_part[DB_SIGNED] = {0}, icv[20];
    if (!secret_of(p, 0, secret)) return false;
    memcpy(signed_part, h, DB_HEADER_ICV);
    if (!vita_hmac_sha1(&p->crypto, secret, 20, signed_part, sizeof signed_part, icv)) return fail(p, "OpenSSL failed");
    if (memcmp(icv, h + DB_HEADER_ICV, 20)) return -1;
    uint32_t root = snx_ld_le32(h + DB_ROOT);
    if (root >= p->pages) return fail(p, "%s's root is page %u of its %u", path, root, p->pages);
    for (uint32_t page = 0; page < p->pages; ++page) {
        const unsigned char* g = p->db + PAGE + (size_t)page * PAGE;
        uint32_t parent = snx_ld_le32(g + PG_PARENT), count = snx_ld_le32(g + PG_COUNT);
        if (snx_ld_le32(g + PG_TYPE) > 1 || count > ENTRIES)
            return fail(p, "%s's page %u is of type %u with %u entries: no B-tree page", path, page,
                        snx_ld_le32(g + PG_TYPE), count);
        if (!page_icv(p, g, secret, icv)) return fail(p, "OpenSSL failed");
        bool good = false;
        if (page == root)
            good = !memcmp(icv, h + DB_ROOT_ICV, 20);
        else if (parent < p->pages)
            for (unsigned i = 0; i < ORDER && !good; ++i)
                good = !memcmp(icv, p->db + PAGE + (size_t)parent * PAGE + PG_ICVS + i * 20, 20);
        if (!good)
            return fail(p, "%s's page %u does not verify against %s: the file is damaged", path, page,
                        page == root        ? "the header"
                        : parent < p->pages ? "its parent's"
                                            : "a parent it does not have");
        for (uint32_t i = 0; i < count; ++i) {
            Entry* e = &p->entries[p->entry_count];
            const unsigned char *name = g + PG_NAMES + i * 72 + 4, *info = g + PG_INFOS + i * 16;
            *e = (Entry){.type = snx_ld_le16(info + 4),
                         .parent = snx_ld_le32(g + PG_NAMES + i * 72),
                         .index = snx_ld_le32(info),
                         .size = snx_ld_le32(info + 8)};
            size_t length = strnlen((const char*)name, NAME);
            if (!known_type(e->type))
                return fail(p, "%s's page %u: %.*s is of type 0x%04x, which psvpfsparser's reading does not have", path,
                            page, (int)length, name, e->type);
            if (e->type == T_NONE && !e->size) continue; /* a name with nothing behind it */
            if (e->type == T_NONE) e->type = T_FILE;     /* as psvpfsparser takes one: an encrypted file */
            memcpy(e->name, name, length);
            if (length == NAME || !length || strpbrk(e->name, "/\\") || !strcmp(e->name, ".") || !strcmp(e->name, ".."))
                return fail(p, "%s's page %u has an entry named \"%.*s\", which is no file's name", path, page,
                            (int)length, name);
            if (is_folder(e->type)) p->folders[p->folder_count++] = (Folder){e->index, p->entry_count};
            ++p->entry_count;
        }
    }
    qsort(p->folders, p->folder_count, sizeof *p->folders, folder_order);
    for (uint32_t i = 1; i < p->folder_count; ++i)
        if (p->folders[i].index == p->folders[i - 1].index)
            return fail(p, "%s has two folders of index %u: %s and %s", path, p->folders[i].index,
                        p->entries[p->folders[i - 1].entry].name, p->entries[p->folders[i].entry].name);
    return 1;
}
/* unicv.db: its header, then each table's page and its signature pages, a page of zeros between them */
static bool read_icv(Pfs* p, const char* path) {
    const unsigned char* h = p->icv;
    if (p->icv_size < PAGE || p->icv_size % PAGE || memcmp(h, "SCEIRODB", 8))
        return fail(p, "%s is no unicv.db (%zu bytes)", path, p->icv_size);
    if ((snx_ld_le32(h + 8) != 1 && snx_ld_le32(h + 8) != 2) || snx_ld_le32(h + 12) != PAGE ||
        snx_ld_le64(h + 24) != p->icv_size - PAGE)
        return fail(p, "%s's header says version %u, pages of %u bytes and %llu bytes after it, not 1 or 2, %u and %zu",
                    path, snx_ld_le32(h + 8), snx_ld_le32(h + 12), (unsigned long long)snx_ld_le64(h + 24), PAGE,
                    p->icv_size - PAGE);
    static const unsigned char zero[PAGE];
    for (uint32_t page = 1; (size_t)page * PAGE < p->icv_size;) {
        const unsigned char* t = h + (size_t)page * PAGE;
        if (!memcmp(t, zero, PAGE)) {
            ++page;
            continue;
        }
        if (memcmp(t, "SCEIFTBL", 8))
            return fail(p, "%s's page %u is \"%.8s\", not a file's table (SCEIFTBL)", path, page, (const char*)t);
        uint32_t version = snx_ld_le32(t + TB_VERSION), sectors = snx_ld_le32(t + TB_SECTORS);
        if ((version != 1 && version != 2) || snx_ld_le32(t + TB_PAGE) != PAGE ||
            snx_ld_le32(t + TB_MAX) != SIGNATURES || snx_ld_le32(t + TB_SECTOR) != SECTOR || snx_ld_le32(t + TB_PAD))
            return fail(p,
                        "%s's table at page %u is version %u, %u signatures a page and sectors of %u bytes, not 1 or "
                        "2, %u and %u",
                        path, page, version, snx_ld_le32(t + TB_MAX), snx_ld_le32(t + TB_SECTOR), SIGNATURES, SECTOR);
        uint32_t signature_pages = (sectors + SIGNATURES - 1) / SIGNATURES;
        if ((size_t)(page + 1u + signature_pages) * PAGE > p->icv_size)
            return fail(p, "%s's table at page %u has %u sectors, whose signatures run past the file", path, page,
                        sectors);
        for (uint32_t s = 0; s < signature_pages; ++s) {
            const unsigned char* g = t + (size_t)(1 + s) * PAGE;
            uint32_t n = s + 1 < signature_pages ? SIGNATURES : sectors - s * SIGNATURES;
            if (snx_ld_le32(g) != 16u + SIGNATURES * 20u || snx_ld_le32(g + 4) != 20 || snx_ld_le32(g + 8) != n ||
                snx_ld_le32(g + 12))
                return fail(p,
                            "%s's page %u, signatures of the table at page %u, says %u signatures of %u bytes, not %u "
                            "of 20",
                            path, page + 1 + s, page, snx_ld_le32(g + 8), snx_ld_le32(g + 4), n);
        }
        p->tables[p->table_count++] = (Table){page, sectors, version, page * PAGE};
        page += 1 + signature_pages;
    }
    return true;
}
/* e's path in files.db's names, its folders' from the root: false for a folder files.db lacks, or a loop */
static bool entry_path(Pfs* p, const Entry* e, char* out, size_t size) {
    const Entry* chain[DEPTH];
    unsigned depth = 0;
    for (uint32_t parent = e->parent; parent;) {
        const Entry* f = folder_entry(p, parent);
        if (!f) return fail(p, "files.db's %s is in folder %u, which files.db does not have", e->name, parent);
        if (depth == DEPTH) return fail(p, "files.db's %s is %u folders down: its folders loop", e->name, DEPTH);
        chain[depth++] = f;
        parent = f->parent;
    }
    size_t at = 0;
    out[0] = 0;
    while (depth--) at += (size_t)snprintf(out + at, at < size ? size - at : 0, "%s/", chain[depth]->name);
    at += (size_t)snprintf(out + at, at < size ? size - at : 0, "%s", e->name);
    return at < size || fail(p, "files.db's %s is too deep a path", e->name);
}
/* rel, files.db's path, as the dump has it, each part found by its name in any case (files.db's case and the dump's
   may differ), into found, relative to source: 1; -1 when the dump lacks a part, found then the parts it has and
   files.db's names after them, the error said; 0 with the error said for two in different cases or too long a path */
static int locate(Pfs* p, const char* rel, char* found, size_t size) {
    char part[NAME + 1];
    size_t at = 0;
    int result = 1;
    found[0] = 0;
    for (const char* s = rel; *s;) {
        size_t n = strcspn(s, "/");
        snprintf(part, sizeof part, "%.*s", (int)n, s);
        char here[SNX_PATH_SIZE], there[SNX_PATH_SIZE];
        if (snx_path_format(here, sizeof here, "%s%s%s", p->source, at ? "/" : "", found) ||
            snx_path_format(there, sizeof there, "%s/%s", here, part))
            return fail(p, "%s/%s is too long a path", p->source, rel);
        const char* name = part;
        char** names = NULL;
        size_t count = 0, match = 0;
        if (result > 0 && !snx_path_exists(there)) {
            if (!snx_list_dir(here, &names, &count))
                for (size_t i = 0; i < count; ++i) {
                    bool same = strlen(names[i]) == n;
                    for (size_t j = 0; same && j < n; ++j)
                        same = snx_to_lower((unsigned char)names[i][j]) == snx_to_lower((unsigned char)part[j]);
                    if (same && match++ == 0) name = names[i];
                }
            if (match > 1) {
                snx_free_strings(names, count);
                return fail(p, "files.db's %s is %zu files of %s, in different cases", rel, match, here);
            }
            if (!match) {
                fail(p, "files.db lists %s, which the dump does not have (no %s in %s)", rel, part, here);
                result = -1;
            }
        }
        at += (size_t)snprintf(found + at, at < size ? size - at : 0, "%s%s", at ? "/" : "", name);
        snx_free_strings(names, count);
        s += n + (s[n] == '/');
    }
    return at < size ? result : fail(p, "%s/%s is too long a path", p->source, rel);
}
/* the file's sectors checked against their signatures and decrypted (a plain file's only checked), from from to to */
static bool decrypt_file(Pfs* p, const Entry* e, const Table* t, const char* rel, const char* from, const char* to,
                         unsigned char* buffer) {
    uint64_t on_disk = snx_file_size(from);
    uint32_t sectors = (uint32_t)(((uint64_t)e->size + SECTOR - 1) / SECTOR);
    if (on_disk != e->size)
        return fail(p, "%s/%s is %llu bytes, and files.db says %u", p->source, rel, (unsigned long long)on_disk,
                    e->size);
    if (t->sectors != sectors)
        return fail(p, "%s/%s is %u bytes, %u sectors, and its table (unicv.db's page %u) has %u", p->source, rel,
                    e->size, sectors, t->page, t->sectors);
    unsigned char secret[20], mask[16];
    if (!secret_of(p, t->page, secret) || !mask_of(p, t, mask)) return false;
    FILE *in = snx_fopen(from, "rb"), *out = in ? snx_fopen(to, "wb") : NULL;
    bool ok = in && out;
    if (!ok) {
        char why[256];
        fail(p, "cannot %s %s: %s", in ? "write" : "read", in ? to : from, snx_os_error(why, sizeof why));
    }
    for (uint32_t s = 0; ok && s < sectors; ++s) {
        size_t n = s + 1 < sectors ? SECTOR : e->size - (size_t)s * SECTOR;
        unsigned char index[4], key[20], signature[20];
        snx_st_le32(index, s);
        if (fread(buffer, 1, n, in) != n) {
            ok = fail(p, "cannot read %s/%s's sector %u", p->source, rel, s);
            break;
        }
        const unsigned char* expected =
            p->icv + t->at + (size_t)(1 + s / SIGNATURES) * PAGE + 16 + (size_t)(s % SIGNATURES) * 20;
        if (!vita_hmac_sha1(&p->crypto, secret, 20, index, 4, key) ||
            !vita_hmac_sha1(&p->crypto, key, 20, buffer, n, signature)) {
            ok = fail(p, "OpenSSL failed");
            break;
        }
        if (memcmp(signature, expected, 20)) {
            ok = fail(p,
                      "%s/%s's sector %u (bytes 0x%llx to 0x%llx) does not match its signature in unicv.db: the "
                      "dump is damaged there",
                      p->source, rel, s, (unsigned long long)s * SECTOR, (unsigned long long)s * SECTOR + n);
            break;
        }
        if (!is_plain(e->type)) {
            /* the IV: the sector's offset, little-endian, XOR the mask; the tail past the last whole block XORed with
               that block encrypted (the IV when there is none) */
            unsigned char iv[16] = {0}, last[16], pad[16];
            size_t whole = n & ~(size_t)15;
            snx_st_le64(iv, (uint64_t)s * SECTOR);
            for (unsigned i = 0; i < 16; ++i) iv[i] ^= mask[i];
            memcpy(last, whole ? buffer + whole - 16 : iv, 16);
            if (!vita_crypt(&p->crypto, EVP_aes_128_cbc(), false, p->key, iv, buffer, buffer, whole) ||
                ((n & 15) && !vita_crypt(&p->crypto, EVP_aes_128_ecb(), true, p->key, NULL, last, pad, 16))) {
                ok = fail(p, "OpenSSL failed decrypting %s/%s", p->source, rel);
                break;
            }
            for (size_t i = whole; i < n; ++i) buffer[i] ^= pad[i - whole];
        }
        if (fwrite(buffer, 1, n, out) != n) {
            char why[256];
            ok = fail(p, "cannot write %s: %s", to, snx_os_error(why, sizeof why));
        }
    }
    if (out && fclose(out) && ok) {
        char why[256];
        ok = fail(p, "cannot write %s: %s", to, snx_os_error(why, sizeof why));
    }
    if (in) fclose(in);
    return ok;
}

/* the paths of the dump the decryption took, to tell the ones it leaves out */
typedef struct {
    char* text;
    size_t used, room;
    size_t* at;
    size_t count, slots;
} Paths;
static bool paths_add(Paths* s, const char* path) {
    size_t n = strlen(path) + 1;
    if (s->used + n > s->room) {
        size_t room = (s->room ? s->room * 2 : 4096) + n;
        char* text = realloc(s->text, room);
        if (!text) return false;
        s->text = text, s->room = room;
    }
    if (s->count == s->slots) {
        size_t slots = s->slots ? s->slots * 2 : 256;
        size_t* at = realloc(s->at, slots * sizeof *at);
        if (!at) return false;
        s->at = at, s->slots = slots;
    }
    memcpy(s->text + s->used, path, n);
    s->at[s->count++] = s->used;
    s->used += n;
    return true;
}
static int path_order(const void* a, const void* b) { return strcmp(*(const char* const*)a, *(const char* const*)b); }
/* whether path is one of the count paths of sorted, in strcmp's order */
static bool paths_has(const char* const* sorted, size_t count, const char* path) {
    return bsearch(&path, sorted, count, sizeof *sorted, path_order) != NULL;
}
/* each file under rel that the decryption did not take, told: sce_pfs/ and the package's sce_sys/package/ are no
   files of the content, nor a folder's sce_sys/ with a keystone of its own below the root (another content's) */
static void left_out(Pfs* p, const char* const* taken, size_t taken_count, const char* rel, VitaPfsNote note,
                     void* context, VitaPfsCount* count) {
    char here[SNX_PATH_SIZE], **names = NULL;
    size_t n = 0;
    if (snx_path_format(here, sizeof here, "%s%s%s", p->source, rel[0] ? "/" : "", rel) ||
        snx_list_dir(here, &names, &n))
        return;
    for (size_t i = 0; i < n; ++i) {
        char inner[SNX_PATH_SIZE], full[SNX_PATH_SIZE], keystone[SNX_PATH_SIZE];
        if (snx_path_format(inner, sizeof inner, "%s%s%s", rel, rel[0] ? "/" : "", names[i]) ||
            snx_path_format(full, sizeof full, "%s/%s", here, names[i]))
            continue;
        if (snx_is_dir(full)) {
            bool skip = !strcmp(names[i], "sce_pfs") || !strcmp(inner, "sce_sys/package") ||
                        (!strcmp(names[i], "sce_sys") && rel[0] &&
                         !snx_path_format(keystone, sizeof keystone, "%s/keystone", full) && snx_is_file(keystone));
            if (!skip) left_out(p, taken, taken_count, inner, note, context, count);
        } else if (!paths_has(taken, taken_count, inner)) {
            ++count->left_out;
            if (note) note(context, inner);
        }
    }
    snx_free_strings(names, n);
}

bool vita_pfs_decrypt(const char* source, const char* dest, const unsigned char klicensee[16], const VitaKeys* keys,
                      VitaPfsNote note, void* context, VitaPfsCount* count, char* error, size_t error_size) {
    Pfs pfs = {.source = source, .error = error, .error_size = error_size};
    Pfs* p = &pfs;
    Paths taken = {0};
    unsigned char* buffer = NULL;
    char path[SNX_PATH_SIZE];
    *count = (VitaPfsCount){0};
    const unsigned char* contract = vita_key(keys, VITA_KEY_PFS_CONTRACT0, "the files' key", error, error_size);
    p->hmac0 = contract ? vita_key(keys, VITA_KEY_PFS_HMAC0, "the files' sector masks", error, error_size) : NULL;
    p->hmac1 = p->hmac0 ? vita_key(keys, VITA_KEY_PFS_HMAC1, "the files' integrity secret", error, error_size) : NULL;
    p->iv0 = p->hmac1 ? vita_key(keys, VITA_KEY_PFS_IV0, "the files' integrity secret", error, error_size) : NULL;
    if (!p->iv0) return false;
    if (!vita_crypto_open(&p->crypto)) return fail(p, "OpenSSL has no AES or HMAC-SHA1");
    bool ok = vita_crypt(&p->crypto, EVP_aes_128_ecb(), false, contract, NULL, klicensee, p->key, 16) ||
              fail(p, "OpenSSL failed");
    if (ok && (snx_path_format(path, sizeof path, "%s/sce_pfs/files.db", source) ||
               !(p->db = snx_read_file(path, &p->db_size))))
        ok = fail(p, "cannot read %s/sce_pfs/files.db", source);
    if (ok && (snx_path_format(path, sizeof path, "%s/sce_pfs/unicv.db", source) ||
               !(p->icv = snx_read_file(path, &p->icv_size)))) {
        char icv[SNX_PATH_SIZE];
        bool other = !snx_path_format(icv, sizeof icv, "%s/sce_pfs/icv.db", source) && snx_path_exists(icv);
        ok = fail(p,
                  other ? "%s/sce_pfs has icv.db, not unicv.db: savedata or the add-ons' root, keyed to a console"
                        : "cannot read %s/sce_pfs/unicv.db",
                  source);
    }
    if (ok) {
        /* the entries and folders, at most a page's 9 each, and the tables, at most one a page, in one block */
        size_t entries = (p->db_size / PAGE) * ENTRIES, tables = p->icv_size / PAGE;
        size_t at_entries = tables * sizeof(Table), at_folders = at_entries + entries * sizeof(Entry);
        p->block = malloc(at_folders + entries * sizeof(Folder) + 1);
        buffer = malloc(SECTOR);
        if (!p->block || !buffer)
            ok = fail(p, "out of memory");
        else {
            p->tables = p->block;
            p->entries = (Entry*)((char*)p->block + at_entries);
            p->folders = (Folder*)((char*)p->block + at_folders);
        }
    }
    int db = ok && !snx_path_format(path, sizeof path, "%s/sce_pfs/files.db", source) ? read_db(p, path) : 0;
    if (ok && db < 0)
        ok = fail(p,
                  "%s/sce_pfs/files.db does not verify with the license's klicensee: the license is another "
                  "content's, or a console's own (its key encrypted to that console)",
                  source);
    else if (ok && !db)
        ok = false;
    if (ok && !snx_path_format(path, sizeof path, "%s/sce_pfs/unicv.db", source)) ok = read_icv(p, path);
    if (ok && snx_mkdirs(dest)) ok = fail(p, "cannot make %s", dest);
    /* the folders, the empty ones too, then the files */
    char rel[SNX_PATH_SIZE], found[SNX_PATH_SIZE], to[SNX_PATH_SIZE], from[SNX_PATH_SIZE];
    for (uint32_t i = 0; ok && i < p->entry_count; ++i) {
        const Entry* e = &p->entries[i];
        if (!is_folder(e->type)) continue;
        /* a folder files.db lists and the dump left out, being empty, is made by files.db's name */
        ok = entry_path(p, e, rel, sizeof rel) && locate(p, rel, found, sizeof found) != 0;
        if (ok && (snx_path_format(to, sizeof to, "%s/%s", dest, found) || snx_mkdirs(to)))
            ok = fail(p, "cannot make %s/%s", dest, found);
        if (ok) ++count->folders;
    }
    for (uint32_t i = 0; ok && i < p->entry_count; ++i) {
        const Entry* e = &p->entries[i];
        if (is_folder(e->type)) continue;
        ok = entry_path(p, e, rel, sizeof rel) && locate(p, rel, found, sizeof found) > 0;
        if (ok && (snx_path_format(from, sizeof from, "%s/%s", source, found) ||
                   snx_path_format(to, sizeof to, "%s/%s", dest, found)))
            ok = fail(p, "%s/%s is too long a path", dest, found);
        if (ok && !snx_is_file(from)) ok = fail(p, "files.db's file %s is a folder in the dump", rel);
        if (ok && !paths_add(&taken, found)) ok = fail(p, "out of memory");
        if (!ok) break;
        char* folder = snx_dirname(to);
        if (!folder || snx_mkdirs(folder)) ok = fail(p, "cannot make the folder of %s", to);
        free(folder);
        if (ok && !e->size) {
            ok = !snx_write_file(to, "", 0) || fail(p, "cannot write %s", to);
            ++count->empty;
            continue;
        }
        const Table* t = ok ? table_at(p, e->index) : NULL;
        if (ok && !t)
            ok = fail(p, "files.db's %s (index %u) has no table in unicv.db (no SCEIFTBL at page %u)", rel, e->index,
                      e->index);
        if (ok) ok = decrypt_file(p, e, t, found, from, to, buffer);
        if (ok) {
            if (is_plain(e->type))
                ++count->copied;
            else
                ++count->decrypted;
            count->bytes += e->size;
        }
    }
    if (ok) {
        const char** sorted = malloc((taken.count ? taken.count : 1) * sizeof *sorted);
        if (!sorted)
            ok = fail(p, "out of memory");
        else {
            for (size_t i = 0; i < taken.count; ++i) sorted[i] = taken.text + taken.at[i];
            qsort(sorted, taken.count, sizeof *sorted, path_order);
            left_out(p, sorted, taken.count, "", note, context, count);
            free(sorted);
        }
    }
    free(taken.text);
    free(taken.at);
    free(buffer);
    free(p->block);
    free(p->db);
    free(p->icv);
    vita_crypto_close(&p->crypto);
    return ok;
}
