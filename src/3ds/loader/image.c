/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/loader/ctr_image.h"
#include "3ds/loader/ctr_loader.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool read_exact(const char* root, const char* name, void* out, size_t size) {
    char* path = snx_path_join(root, name);
    FILE* f = !snx_is_link(path) && snx_is_file(path) ? snx_fopen(path, "rb") : NULL;
    free(path);
    if (!f) return false;
    bool ok = fread(out, 1, size, f) == size && fgetc(f) == EOF && !ferror(f);
    if (fclose(f)) ok = false;
    return ok;
}
void snx_ctr_image_free(SnxCtrImage* image) {
    if (image) {
        free(image->code);
        *image = (SnxCtrImage){0};
    }
}
/* the image an ExHeader describes, its expanded .code read exact and bounded by code(source, out, size) */
static bool image_parse(const unsigned char* ex, bool (*code)(const void*, unsigned char*, size_t),
                        const void* source, SnxCtrImage* image, char* error, size_t error_size) {
    image->stack_size = snx_ld_le32(ex + 0x1c);
    image->bss_size = snx_ld_le32(ex + 0x3c);
    image->title_id = (uint64_t)snx_ld_le32(ex + 0x200) | (uint64_t)snx_ld_le32(ex + 0x204) << 32;
    memcpy(image->name, ex, sizeof image->name);
    image->main_priority = ex[0x20f];
    image->new_features = ex[0x20c] & 3;
    image->new_mode = ex[0x20d] & 15;
    image->old_mode = ex[0x20e] >> 4;
    for (unsigned i = 0; i < 28; ++i) image->kernel_caps[i] = snx_ld_le32(ex + 0x370 + i * 4);
    const char* reason = "invalid ExHeader main thread priority";
    if (image->main_priority > 63) goto fail;
    const unsigned offsets[] = {0x10, 0x20, 0x30};
    uint64_t sizes[3], total = 0;
    reason = "invalid extracted process segment/BSS/stack layout";
    for (unsigned i = 0; i < 3; ++i) {
        const unsigned char* p = ex + offsets[i];
        SnxCtrSegment* s = &image->segment[i];
        *s = (SnxCtrSegment){snx_ld_le32(p), snx_ld_le32(p + 4), snx_ld_le32(p + 8)};
        uint64_t bytes = (uint64_t)s->pages * 4096;
        sizes[i] = bytes + (i == 2 ? ((uint64_t)image->bss_size + 4095) & ~4095ull : 0);
        /* Match extraction: some homebrew .data byte counts include BSS. */
        uint64_t initialized = s->bytes;
        if (i == 2 && initialized > bytes && initialized >= image->bss_size) initialized -= image->bss_size;
        if (initialized > bytes ||
            (sizes[i] && ((s->address & 4095) || s->address < 0x100000 || (uint64_t)s->address + sizes[i] > 0x4000000)))
            goto fail;
        for (unsigned j = 0; j < i; ++j)
            if (sizes[i] && sizes[j] && s->address < (uint64_t)image->segment[j].address + sizes[j] &&
                image->segment[j].address < (uint64_t)s->address + sizes[i])
                goto fail;
        total += bytes;
    }
    if (!image->segment[0].bytes || (image->segment[0].bytes & 3) || total > 0x4000000 || !image->stack_size ||
        image->stack_size > 0x800000)
        goto fail;
    image->code_size = (size_t)total;
    image->code = malloc(image->code_size);
    reason = "cannot read exact bounded expanded .code";
    if (!image->code || !code(source, image->code, image->code_size)) goto fail;
    static const char domain[] = "ScarletNX CTR image identity v1";
    EVP_MD_CTX* hash = EVP_MD_CTX_new();
    unsigned length = 0;
    bool hashed = hash && EVP_DigestInit_ex(hash, EVP_sha256(), NULL) == 1 &&
                  EVP_DigestUpdate(hash, domain, sizeof domain) == 1 && EVP_DigestUpdate(hash, ex, 0x800) == 1 &&
                  EVP_DigestUpdate(hash, image->code, image->code_size) == 1 &&
                  EVP_DigestFinal_ex(hash, image->identity, &length) == 1 && length == sizeof image->identity;
    EVP_MD_CTX_free(hash);
    reason = "cannot hash the image with OpenSSL";
    if (!hashed) goto fail;
    return true;
fail:
    if (error_size) snprintf(error, error_size, "%s", reason);
    snx_ctr_image_free(image);
    return false;
}
static bool extracted_code(const void* directory, unsigned char* out, size_t size) {
    return read_exact(directory, "exefs/.code", out, size);
}
/* why an extraction's folder is refused: a link is never followed */
static const char* not_folder(const char* path) {
    return snx_is_link(path) ? "is a link" : !snx_is_dir(path) ? "is not a folder" : NULL;
}
bool snx_ctr_image_load(const char* directory, SnxCtrImage* image, char* error, size_t error_size) {
    if (error_size) error[0] = 0;
    if (!image) return false;
    *image = (SnxCtrImage){0};
    if (!directory) {
        snprintf(error, error_size, "no extraction folder given");
        return false;
    }
    unsigned char ex[0x800];
    char* exefs = snx_path_join(directory, "exefs");
    const char *why = not_folder(directory), *refused = directory;
    if (!why && (why = not_folder(exefs))) refused = exefs;
    if (why) snprintf(error, error_size, "%s %s", refused, why);
    free(exefs);
    if (why) return false;
    if (!read_exact(directory, "exheader.bin", ex, sizeof ex)) {
        snprintf(error, error_size, "%s/exheader.bin is not a file of %u bytes", directory, (unsigned)sizeof ex);
        return false;
    }
    return image_parse(ex, extracted_code, directory, image, error, error_size);
}
/* the .code snx_ctr_source_open expanded: exactly the size the ExHeader's pages give */
static bool source_code(const void* source, unsigned char* out, size_t size) {
    size_t have;
    const unsigned char* code = snx_ctr_source_code(source, &have);
    if (have != size) return false;
    memcpy(out, code, size);
    return true;
}
bool snx_ctr_image_from_source(const SnxCtrSource* source, SnxCtrImage* image, char* error, size_t error_size) {
    if (error_size) error[0] = 0;
    if (!image) return false;
    *image = (SnxCtrImage){0};
    if (!source) {
        if (error_size) snprintf(error, error_size, "no game to read the image from");
        return false;
    }
    return image_parse(snx_ctr_source_exheader(source), source_code, source, image, error, error_size);
}
