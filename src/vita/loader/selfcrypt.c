/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "vita/loader/selfcrypt.h"
#include "vita/loader/crypto.h"
#include "common/utilities/ascii.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the SCE header and the SELF's after it (Vita3K's SceHeader and SelfHeader, packages/include/packages/sce_types.h) */
#define SCE_MAGIC       0x00454353u /* "SCE\0" */
#define SCE_KEY_REV     0x09u
#define SCE_TYPE        0x0au /* 1 a SELF */
#define SCE_METADATA    0x0cu
#define SCE_HEADER_SIZE 0x10u
#define SELF_APPINFO    0x38u
#define SELF_ELF        0x40u
#define SELF_SEGMENTS   0x58u
#define SELF_END        0x78u
#define APP_TYPE        0x0cu /* AppInfoHeader's self_type: 8 an app's */
#define APP_SYSTEM      0x10u
#define APP_SELF        8u
/* a segment's entry: offset, size, compression, then 1 when encrypted and 2 when plain */
#define SEG_OFFSET      0x00u
#define SEG_SIZE        0x08u
#define SEG_PLAIN       0x18u
#define SEG_ENTRY       0x20u
/* the metadata, after its 0x30 bytes: the key block (key, 16 zeros, IV, 16 zeros) under the SELF key, then its header,
   sections and keys under the block's key */
#define META_SKIP       0x30u
#define META_BLOCK      0x40u
#define META_HEADER     0x20u
#define META_SECTIONS   0x0cu
#define META_KEYS       0x10u
#define SECTION_SIZE    0x30u
#define SEC_OFFSET      0x00u
#define SEC_SIZE        0x08u
#define SEC_SEGMENT     0x14u
#define SEC_CRYPT       0x20u /* 3 AES-128-CTR */
#define SEC_KEY         0x24u
#define SEC_IV          0x28u
/* the system versions the key revision 0 keys are for (Vita3K's register_keys) */
#define SYSTEM_169      0x16920000000ull
#define SYSTEM_180      0x18000000000ull

bool vita_self_name(const char* name) {
    size_t n = strlen(name);
    static const char* const ends[] = {".suprx", ".self", ".skprx"};
    if (!strcmp(name, "eboot.bin")) return true;
    for (size_t e = 0; e < sizeof ends / sizeof *ends; ++e) {
        size_t m = strlen(ends[e]);
        bool same = n > m;
        for (size_t i = 0; same && i < m; ++i) same = snx_to_lower((unsigned char)name[n - m + i]) == ends[e][i];
        if (same) return true;
    }
    return false;
}
static int failed(char* error, size_t error_size, const char* path, const char* why) {
    snprintf(error, error_size, "%s: %s", path, why);
    return -1;
}
int vita_self_decrypt(const char* path, const char* shown, const unsigned char klicensee[16], const VitaKeys* keys,
                      char* error, size_t error_size) {
    size_t size = 0;
    unsigned char* f = snx_read_file(path, &size);
    char why[512] = "", system_why[256];
    if (!f) {
        snprintf(error, error_size, "cannot read %s: %s", path, snx_os_error(system_why, sizeof system_why));
        return -1;
    }
    int result = -1;
    VitaCrypto crypto = {0};
    if (size < SELF_END || snx_ld_le32(f) != SCE_MAGIC || snx_ld_le16(f + SCE_TYPE) != 1) {
        snprintf(why, sizeof why, "it is no SELF (%zu bytes, magic 0x%08x, type %u)", size,
                 size >= 4 ? snx_ld_le32(f) : 0, size >= SELF_END ? snx_ld_le16(f + SCE_TYPE) : 0);
        goto done;
    }
    uint64_t header_size = snx_ld_le64(f + SCE_HEADER_SIZE), meta = (uint64_t)snx_ld_le32(f + SCE_METADATA) + META_SKIP;
    uint64_t app = snx_ld_le64(f + SELF_APPINFO), elf = snx_ld_le64(f + SELF_ELF),
             segments = snx_ld_le64(f + SELF_SEGMENTS);
    if (elf > size || size - elf < 52 || app > size || size - app < 32) {
        snprintf(why, sizeof why, "its ELF header (0x%llx) or app info (0x%llx) is past its %zu bytes",
                 (unsigned long long)elf, (unsigned long long)app, size);
        goto done;
    }
    unsigned count = snx_ld_le16(f + elf + 44), encrypted = 0;
    if (segments > size || (size - segments) / SEG_ENTRY < count) {
        snprintf(why, sizeof why, "its %u segments' entries at 0x%llx are past its %zu bytes", count,
                 (unsigned long long)segments, size);
        goto done;
    }
    for (unsigned i = 0; i < count; ++i) encrypted += snx_ld_le64(f + segments + i * SEG_ENTRY + SEG_PLAIN) == 1;
    if (!encrypted) {
        result = 0;
        goto done;
    }
    uint32_t type = snx_ld_le32(f + app + APP_TYPE), revision = f[SCE_KEY_REV];
    uint64_t system = snx_ld_le64(f + app + APP_SYSTEM);
    if (type != APP_SELF) {
        snprintf(why, sizeof why, "it is a SELF of type 0x%x, not an app's (8), which only the console's own keys open",
                 type);
        goto done;
    }
    SnxVitaKey name = revision == 0 && system <= SYSTEM_169   ? SNX_VITA_KEY_APP0_100
                      : revision == 0 && system >= SYSTEM_180 ? SNX_VITA_KEY_APP0_180
                      : revision >= 1 && revision <= 5        ? (SnxVitaKey)(SNX_VITA_KEY_APP1 + 2 * (revision - 1))
                                                              : SNX_VITA_KEY_COUNT;
    if (name == SNX_VITA_KEY_COUNT) {
        snprintf(why, sizeof why,
                 "it is of key revision %u for system 0x%llx, which has no app SELF key (revision 0 up to 0x%llx and "
                 "from 0x%llx, 1 to 5)",
                 revision, (unsigned long long)system, SYSTEM_169, SYSTEM_180);
        goto done;
    }
    char purpose[SNX_PATH_SIZE + 64];
    snprintf(purpose, sizeof purpose, "%s (key revision %u)", shown, revision);
    SnxVitaKey np_name = revision >= 2 ? SNX_VITA_KEY_NPDRM1 : SNX_VITA_KEY_NPDRM0;
    const unsigned char* np = vita_key(keys, np_name, purpose, error, error_size);
    const unsigned char* np_iv = np ? vita_key(keys, (SnxVitaKey)(np_name + 1), purpose, error, error_size) : NULL;
    const unsigned char* self_key = np_iv ? vita_key(keys, name, purpose, error, error_size) : NULL;
    const unsigned char* self_iv = self_key ? vita_key(keys, (SnxVitaKey)(name + 1), purpose, error, error_size) : NULL;
    if (!self_iv) {
        free(f);
        return -1;
    }
    if (header_size > size || meta > header_size || header_size - meta < META_BLOCK + META_HEADER) {
        snprintf(why, sizeof why, "its metadata at 0x%llx is past its header's 0x%llx bytes", (unsigned long long)meta,
                 (unsigned long long)header_size);
        goto done;
    }
    if (!vita_crypto_open(&crypto)) {
        snprintf(why, sizeof why, "OpenSSL has no AES");
        goto done;
    }
    unsigned char key[16], block1[META_BLOCK], block[META_BLOCK];
    size_t rest = (size_t)(header_size - meta - META_BLOCK) & ~(size_t)15;
    unsigned char* m = malloc(rest ? rest : 1);
    static const unsigned char zero[16];
    bool ok = m && vita_crypt(&crypto, EVP_aes_128_cbc(), false, np, np_iv, klicensee, key, 16) &&
              vita_crypt(&crypto, EVP_aes_128_cbc(), false, key, np_iv, f + meta, block1, META_BLOCK) &&
              vita_crypt(&crypto, EVP_aes_256_cbc(), false, self_key, self_iv, block1, block, META_BLOCK);
    if (ok && (memcmp(block + 16, zero, 16) || memcmp(block + 48, zero, 16))) {
        snprintf(why, sizeof why,
                 "its metadata does not decrypt with the license's klicensee and %s: the license is "
                 "another content's, or the key is not the console's",
                 snx_vita_key_file(name));
        free(m);
        goto done;
    }
    ok = ok && vita_crypt(&crypto, EVP_aes_128_cbc(), false, block, block + 32, f + meta + META_BLOCK, m, rest);
    uint32_t sections = ok ? snx_ld_le32(m + META_SECTIONS) : 0, key_count = ok ? snx_ld_le32(m + META_KEYS) : 0;
    if (ok &&
        (sections > 64 || key_count > 256 || META_HEADER + (size_t)sections * SECTION_SIZE + key_count * 16u > rest)) {
        snprintf(why, sizeof why, "its metadata gives %u sections and %u keys, past its %zu bytes", sections, key_count,
                 rest);
        free(m);
        goto done;
    }
    const unsigned char* vault = m + META_HEADER + (size_t)sections * SECTION_SIZE;
    for (uint32_t s = 0; ok && s < sections; ++s) {
        const unsigned char* sec = m + META_HEADER + (size_t)s * SECTION_SIZE;
        uint32_t segment = snx_ld_le32(sec + SEC_SEGMENT), k = snx_ld_le32(sec + SEC_KEY),
                 v = snx_ld_le32(sec + SEC_IV);
        uint64_t offset = snx_ld_le64(sec + SEC_OFFSET), length = snx_ld_le64(sec + SEC_SIZE);
        if (snx_ld_le32(sec + SEC_CRYPT) != 3 || segment >= count) continue;
        unsigned char* entry = f + segments + (size_t)segment * SEG_ENTRY;
        /* the section of an encrypted segment: its offset and size the segment's */
        if (snx_ld_le64(entry + SEG_PLAIN) != 1 || snx_ld_le64(entry + SEG_OFFSET) != offset ||
            snx_ld_le64(entry + SEG_SIZE) != length)
            continue;
        if (k >= key_count || v >= key_count || offset > size || length > size - offset) {
            snprintf(why, sizeof why,
                     "segment %u's section gives key %u and counter %u of %u, at 0x%llx, %llu bytes "
                     "of its %zu",
                     segment, k, v, key_count, (unsigned long long)offset, (unsigned long long)length, size);
            ok = false;
            break;
        }
        ok = vita_crypt(&crypto, EVP_aes_128_ctr(), false, vault + k * 16u, vault + v * 16u, f + offset, f + offset,
                        (size_t)length);
        if (ok) snx_st_le64(entry + SEG_PLAIN, 2), --encrypted;
    }
    free(m);
    if (ok && encrypted)
        snprintf(why, sizeof why, "%u of its segments have no key in its metadata", encrypted), ok = false;
    else if (!ok && !why[0])
        snprintf(why, sizeof why, "OpenSSL failed decrypting it");
    if (ok && snx_write_file(path, f, size))
        snprintf(why, sizeof why, "cannot write it back: %s", snx_os_error(system_why, sizeof system_why)), ok = false;
    if (ok) result = 1;
done:
    if (result < 0 && why[0]) failed(error, error_size, shown, why);
    vita_crypto_close(&crypto);
    free(f);
    return result;
}
