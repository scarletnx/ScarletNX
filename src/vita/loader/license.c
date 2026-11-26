/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "vita/loader/license.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include <miniz.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The license's fields this reads: the content ID at 0x10 and the klicensee at 0x50, the account ID at 0x08 (Vita3K's
   SceNpDrmLicense, packages/include/packages/license.h) */
#define RIF_ACCOUNT    0x08u
#define RIF_CONTENT_ID 0x10u
#define RIF_KEY        0x50u

bool vita_license_parse(const unsigned char* rif, size_t size, const char* what, VitaLicense* license, char* error,
                        size_t error_size) {
    *license = (VitaLicense){0};
    if (size != VITA_LICENSE_SIZE) {
        snprintf(error, error_size, "%s is %zu bytes, not the %u of a Vita license%s", what, size, VITA_LICENSE_SIZE,
                 size == 0x98 ? " (0x98 is a PSP game's)" : "");
        return false;
    }
    size_t n = 0;
    while (n < 0x30 && rif[RIF_CONTENT_ID + n] >= 0x21 && rif[RIF_CONTENT_ID + n] < 0x7f) ++n;
    if (n < 0x10 || (n < 0x30 && rif[RIF_CONTENT_ID + n])) {
        snprintf(error, error_size, "%s has no content ID at 0x10 (its first %zu bytes are text): it is no license",
                 what, n);
        return false;
    }
    memcpy(license->content_id, rif + RIF_CONTENT_ID, n);
    memcpy(license->klicensee, rif + RIF_KEY, sizeof license->klicensee);
    license->account = snx_ld_le64(rif + RIF_ACCOUNT);
    return true;
}
bool vita_license_read(const char* path, VitaLicense* license, char* error, size_t error_size) {
    size_t size = 0;
    unsigned char* rif = snx_read_file(path, &size);
    if (!rif) {
        char why[256];
        snprintf(error, error_size, "cannot read the license %s: %s", path, snx_os_error(why, sizeof why));
        *license = (VitaLicense){0};
        return false;
    }
    bool ok = vita_license_parse(rif, size, path, license, error, error_size);
    free(rif);
    return ok;
}

/* The zRIF's dictionary, which its stream is deflated against (zlib's FDICT, id 0x627d1d5d): 880 zeros, then the parts
   content IDs and licenses share. From pkg2zip (pkg2zip_zrif.c), public domain */
#define ZRIF_DICT_ZEROS 880u
static const char zrif_dict_tail[] =
    "00009\0\0\0\0\0\0\0\0\0\0\0"
    "000060000700008\0"
    "000030000400005"
    "0_00-ADDCONT00002-PCSG00000000001-PCSE000-PCSF000-PCSC000-PCSD000-PCSA000-PCSB000\0"
    "\x01\0\x01\0\x01\0\x02\xef\xcd\xab\x89gE#\x01";
#define ZRIF_DICT_SIZE (ZRIF_DICT_ZEROS + sizeof zrif_dict_tail - 1)
static_assert(ZRIF_DICT_SIZE == 1024, "the zRIF dictionary is 1024 bytes");
#define ZRIF_DICT_ID 0x627d1d5du

static int base64_value(int c) {
    return c >= 'A' && c <= 'Z'   ? c - 'A'
           : c >= 'a' && c <= 'z' ? c - 'a' + 26
           : c >= '0' && c <= '9' ? c - '0' + 52
           : c == '+' || c == '-' ? 62
           : c == '/' || c == '_' ? 63
                                  : -1;
}
bool vita_license_zrif(const char* text, VitaLicense* license, char* error, size_t error_size) {
    *license = (VitaLicense){0};
    unsigned char raw[1024];
    size_t n = 0, length = strlen(text);
    while (length && (text[length - 1] == ' ' || text[length - 1] == '\n' || text[length - 1] == '\r')) --length;
    while (length && text[length - 1] == '=') --length;
    uint32_t bits = 0;
    unsigned count = 0;
    for (size_t i = 0; i < length; ++i) {
        int v = base64_value((unsigned char)text[i]);
        if (v < 0) {
            snprintf(error, error_size, "the zRIF has '%c' at character %zu, which is not base64", text[i], i + 1);
            return false;
        }
        bits = bits << 6 | (uint32_t)v;
        if ((count += 6) >= 8) {
            if (n == sizeof raw) {
                snprintf(error, error_size, "the zRIF is longer than the %zu bytes a license deflates to", sizeof raw);
                return false;
            }
            count -= 8;
            raw[n++] = (unsigned char)(bits >> count);
        }
    }
    /* zlib's header (RFC 1950): deflate, its check, the dictionary's id; the Adler-32 of the license after the stream */
    if (n < 2 + 4 + 4 || (raw[0] << 8 | raw[1]) % 31 || (raw[0] & 0xf) != 8) {
        snprintf(error, error_size, "the zRIF's %zu bytes do not start a zlib stream", n);
        return false;
    }
    if (!(raw[1] & 0x20) || snx_ld_be32(raw + 2) != ZRIF_DICT_ID) {
        snprintf(error, error_size, "the zRIF does not use the zRIF dictionary (0x%08x)%s", ZRIF_DICT_ID,
                 raw[1] & 0x20 ? "" : ": it names none");
        return false;
    }
    unsigned char* window = calloc(1, ZRIF_DICT_SIZE + VITA_LICENSE_SIZE + 1);
    tinfl_decompressor* inflator = malloc(sizeof *inflator);
    bool ok = window && inflator;
    if (ok) {
        memcpy(window + ZRIF_DICT_ZEROS, zrif_dict_tail, sizeof zrif_dict_tail - 1);
        tinfl_init(inflator);
        size_t in = n - 6 - 4, out = VITA_LICENSE_SIZE + 1;
        tinfl_status status = tinfl_decompress(inflator, raw + 6, &in, window, window + ZRIF_DICT_SIZE, &out,
                                               TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        if (status != TINFL_STATUS_DONE)
            snprintf(error, error_size, "the zRIF's stream does not inflate (miniz's status %d)", (int)status),
                ok = false;
        else if (mz_adler32(1, window + ZRIF_DICT_SIZE, out) != snx_ld_be32(raw + 6 + in))
            snprintf(error, error_size, "the zRIF fails its Adler-32 (0x%08x, its stream's 0x%08x)",
                     (unsigned)mz_adler32(1, window + ZRIF_DICT_SIZE, out), snx_ld_be32(raw + 6 + in)),
                ok = false;
        else
            ok = vita_license_parse(window + ZRIF_DICT_SIZE, out, "the zRIF's license", license, error, error_size);
    } else
        snprintf(error, error_size, "out of memory");
    free(inflator);
    free(window);
    return ok;
}
