/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/logzip.h"
#include "common/utilities/common.h"
#include "common/utilities/logscan.h"
#include <libdeflate.h>
#include <miniz.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* text, with its homes as ~, deflated by c into the zip under name, last changed at when: 0, or the miniz error */
static mz_zip_error add(mz_zip_archive* zip, struct libdeflate_compressor* c, const char* name, const char* text,
                        size_t n, time_t when) {
    size_t shown, bound, packed;
    char* tilde = snx_logscan_tilde(text, n, &shown);
    unsigned char* out = tilde ? malloc((bound = libdeflate_deflate_compress_bound(c, shown))) : NULL;
    mz_bool done = 0;
    packed = out ? libdeflate_deflate_compress(c, tilde, shown, out, bound) : 0;
    if (packed) /* miniz writes the zip around the deflate, its size and CRC-32 as given */
        done =
            mz_zip_writer_add_mem_ex_v2(zip, name, out, packed, NULL, 0, MZ_DEFAULT_LEVEL | MZ_ZIP_FLAG_COMPRESSED_DATA,
                                        shown, (mz_uint32)libdeflate_crc32(0, tilde, shown), &when, NULL, 0, NULL, 0);
    free(out);
    free(tilde);
    return done ? MZ_ZIP_NO_ERROR : packed ? mz_zip_get_last_error(zip) : MZ_ZIP_ALLOC_FAILED;
}

int gui_logs_zip(const char* path, const char* summary, const GuiZipFile* files, size_t count, char* error,
                 size_t size) {
    mz_zip_archive zip;
    struct libdeflate_compressor* c = libdeflate_alloc_compressor(6);
    mz_zip_error failed = MZ_ZIP_NO_ERROR;
    void* bytes = NULL;
    size_t length = 0, i, left = 0;
    char* text = snx_strdup(summary);
    error[0] = 0;
    mz_zip_zero_struct(&zip);
    if (!c || !mz_zip_writer_init_heap(&zip, 0, (size_t)1 << 20)) {
        snprintf(error, size, "%s could not be made: %s", path,
                 c ? mz_zip_get_error_string(mz_zip_get_last_error(&zip)) : "no memory for libdeflate's compressor");
        libdeflate_free_compressor(c);
        free(text);
        return 1;
    }
    for (i = 0; i < count && !failed; ++i) {
        size_t n = 0;
        unsigned char* data = snx_read_file(files[i].path, &n);
        char why[256], *more;
        if (data) { /* miniz's own failure ends the zip: it says nothing of what it leaves behind */
            failed = add(&zip, c, files[i].name, (const char*)data, n, (time_t)snx_file_time(files[i].path));
            if (failed)
                snprintf(error, size, "%s could not be made: %s: %s", path, files[i].path,
                         mz_zip_get_error_string(failed));
            free(data);
            continue;
        }
        more = snx_format("%s%sNot in the zip: %s (%s)\n", text, left++ ? "" : "\n", files[i].path,
                          snx_os_error(why, sizeof why));
        free(text);
        text = more;
    }
    if (!failed && (failed = add(&zip, c, "summary.txt", text, strlen(text), time(NULL))) != MZ_ZIP_NO_ERROR)
        snprintf(error, size, "%s could not be made: summary.txt: %s", path, mz_zip_get_error_string(failed));
    if (!failed && !mz_zip_writer_finalize_heap_archive(&zip, &bytes, &length)) {
        failed = mz_zip_get_last_error(&zip);
        snprintf(error, size, "%s could not be made: %s", path, mz_zip_get_error_string(failed));
    }
    if (!failed && snx_write_file(path, bytes, length)) {
        char why[256];
        snprintf(error, size, "%s could not be written: %s", path, snx_os_error(why, sizeof why));
    }
    mz_zip_writer_end(&zip);
    mz_free(bytes);
    libdeflate_free_compressor(c);
    free(text);
    return error[0] != 0;
}
