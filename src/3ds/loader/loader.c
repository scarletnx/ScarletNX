/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/loader/internal.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#endif

bool ctr_fail(CtrReader* r, SnxCtrExtractResult result, const char* format, ...) {
    if (r->result == SNX_CTR_EXTRACT_OK) {
        va_list args;
        r->result = result;
        va_start(args, format);
        if (r->error_size) vsnprintf(r->error, r->error_size, format, args);
        va_end(args);
    }
    return false;
}
bool ctr_read(CtrReader* r, CtrSpan span, uint64_t at, void* output, size_t size) {
    if (!ctr_fits(r->size, span.offset, span.size) || !ctr_fits(span.size, at, size))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "truncated or out-of-range archive section");
    uint64_t offset = r->base + span.offset + at;
    if (r->map) {
        if (size) memcpy(output, r->map + offset, size);
        return true;
    }
    if (offset > INT64_MAX) return ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "file offset exceeds host limit");
#ifdef _WIN32
    int failed = _fseeki64(r->input, (int64_t)offset, SEEK_SET);
#else
    int failed = fseeko(r->input, (off_t)offset, SEEK_SET);
#endif
    if (failed || (size && fread(output, 1, size, r->input) != size))
        return ctr_fail(r, SNX_CTR_EXTRACT_IO, "cannot read CIA input");
    return true;
}
bool ctr_sha256(CtrReader* r, const void* data, size_t size, unsigned char hash[32]) {
    return EVP_Digest(data, size, hash, NULL, EVP_sha256(), NULL) ||
           ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "SHA-256 failed (out of memory)");
}
bool ctr_hash(CtrReader* r, CtrSpan span, unsigned char hash[32]) {
    unsigned char bytes[32768];
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    bool ok = context && EVP_DigestInit_ex(context, EVP_sha256(), NULL);
    for (uint64_t at = 0; ok && at < span.size;) {
        size_t n = span.size - at > sizeof bytes ? sizeof bytes : (size_t)(span.size - at);
        if (!ctr_read(r, span, at, bytes, n)) {
            EVP_MD_CTX_free(context);
            return false;
        }
        ok = EVP_DigestUpdate(context, bytes, n);
        at += n;
    }
    ok = ok && EVP_DigestFinal_ex(context, hash, NULL);
    EVP_MD_CTX_free(context);
    return ok || ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "SHA-256 failed (out of memory)");
}
bool ctr_hash_matches(CtrReader* r, CtrSpan span, const unsigned char expected[32]) {
    unsigned char hash[32];
    return ctr_hash(r, span, hash) &&
           (!memcmp(hash, expected, 32) || ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "archive SHA-256 mismatch"));
}
static bool budget(CtrReader* r, uint64_t bytes) {
    if (bytes > SNX_CTR_MAX_CONTENT - r->output_bytes || r->entries >= 100000)
        return ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "extraction exceeds 4 GiB / 100000-entry limit");
    r->output_bytes += bytes;
    r->entries++;
    return true;
}
bool ctr_write(CtrReader* r, const char* path, const void* data, size_t size) {
    if (!budget(r, size)) return false;
    FILE* out = snx_fopen(path, "wbx");
    if (!out) return ctr_fail(r, SNX_CTR_EXTRACT_IO, "cannot create new output file: %s", path);
    bool ok = !size || fwrite(data, 1, size, out) == size;
    if (fclose(out)) ok = false;
    return ok || ctr_fail(r, SNX_CTR_EXTRACT_IO, "cannot write output file: %s", path);
}
bool ctr_copy(CtrReader* r, CtrSpan span, const char* path, const unsigned char* expected) {
    if (!budget(r, span.size)) return false;
    FILE* out = snx_fopen(path, "wbx");
    if (!out) return ctr_fail(r, SNX_CTR_EXTRACT_IO, "cannot create new output file: %s", path);
    unsigned char bytes[32768], hash[32];
    bool check = expected && !r->quick; /* the copy is hashed only when there is a hash to check it against */
    EVP_MD_CTX* context = check ? EVP_MD_CTX_new() : NULL;
    bool hashed = !check || (context && EVP_DigestInit_ex(context, EVP_sha256(), NULL));
    bool ok = true;
    for (uint64_t at = 0; at < span.size && ok;) {
        size_t n = span.size - at > sizeof bytes ? sizeof bytes : (size_t)(span.size - at);
        ok = ctr_read(r, span, at, bytes, n);
        if (ok) {
            hashed = hashed && (!check || EVP_DigestUpdate(context, bytes, n));
            ok = fwrite(bytes, 1, n, out) == n;
        }
        at += n;
    }
    if (fclose(out)) ok = false;
    hashed = hashed && (!check || EVP_DigestFinal_ex(context, hash, NULL));
    EVP_MD_CTX_free(context);
    if (ok && !hashed) return ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "SHA-256 failed (out of memory)");
    if (ok && check && memcmp(hash, expected, 32))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "content SHA-256 mismatch");
    return ok || ctr_fail(r, SNX_CTR_EXTRACT_IO, "cannot copy archive section");
}
bool ctr_directory(CtrReader* r, const char* path) {
    if (!budget(r, 0)) return false;
    if (snx_path_exists(path) || snx_mkdirs(path))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "duplicate or invalid directory: %s", path);
    return true;
}
bool ctr_name(const char* name) {
    size_t size = strlen(name);
    if (!size || size > 255 || !strcmp(name, ".") || !strcmp(name, "..") || name[size - 1] == '.' ||
        name[size - 1] == ' ')
        return false;
    for (const unsigned char* p = (const unsigned char*)name; *p; ++p)
        if (*p < 32 || *p == 127 || strchr("/\\:<>\"|?*", *p)) return false;
    char base[8] = {0};
    size_t n = strcspn(name, ".");
    if (n <= 7) {
        for (size_t i = 0; i < n; ++i) base[i] = (char)snx_to_upper(name[i]);
        if (!strcmp(base, "CON") || !strcmp(base, "PRN") || !strcmp(base, "AUX") || !strcmp(base, "NUL") ||
            !strcmp(base, "CONIN$") || !strcmp(base, "CONOUT$") || !strcmp(base, "CLOCK$"))
            return false;
        if (!memcmp(base, "COM", 3) || !memcmp(base, "LPT", 3)) {
            if (n == 4 && base[3] >= '1' && base[3] <= '9') return false;
            /* Windows also recognizes superscript 1/2/3 as device numbers. */
            if (n == 5 && (unsigned char)base[3] == 0xc2 &&
                ((unsigned char)base[4] == 0xb9 || (unsigned char)base[4] == 0xb2 || (unsigned char)base[4] == 0xb3))
                return false;
        }
    }
    return true;
}
/* Check before snx_absolute: canonicalization must not hide existing links. */
/* a link at the top of the file system, where only root makes entries: the system's own, as macOS's /var, /tmp and
   /etc are into /private. Any deeper one may be planted, even one root owns (a test run as root plants that) */
static bool system_link(const char* path) {
#ifdef _WIN32
    (void)path;
    return false;
#else
    struct stat st;
    return path[0] == '/' && !strchr(path + 1, '/') && !lstat(path, &st) && st.st_uid == 0;
#endif
}
/* The container by its own header, not its name, as Azahar's Loader::IdentifyFile reads it: an NCSD's magic at
   0x100, a bare NCCH's at 0x100, a CIA's at 0. A file that is none of them is refused as the container it is not */
static bool container(CtrReader* r, CtrSource* s) {
    unsigned char h[0x2020];
    size_t have = r->size < sizeof h ? (size_t)r->size : sizeof h;
    if (have < 0x200 || !ctr_read(r, (CtrSpan){0, r->size}, 0, h, have))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "truncated or out-of-range archive section");
    /* a bundle first: the game is one of its entries, and everything after reads that entry as the game */
    bool tar = false;
    if (!ctr_tar_game(r, &tar)) return false;
    if (tar) {
        have = r->size < sizeof h ? (size_t)r->size : sizeof h;
        if (have < 0x200 || !ctr_read(r, (CtrSpan){0, r->size}, 0, h, have))
            return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "truncated or out-of-range archive section");
    }
    if (!memcmp(h + 0x100, "NCSD", 4)) return ctr_ncsd_load(r, s);
    if (!memcmp(h + 0x100, "NCCH", 4)) return ctr_ncch_load(r, s);
    if (have >= 0x2020 && snx_ld_le32(h) == 0x2020) return ctr_cia_load(r, s);
    return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT,
                    "the game is not a CIA, a cartridge image (a CCI or a .3ds) or an NCCH");
}
void ctr_source_free(CtrSource* s) {
    ctr_ncch_free(&s->base);
    ctr_ncch_free(&s->over);
    if (s->kind == CTR_KIND_CIA) ctr_cia_free(s);
    memset(s, 0, sizeof *s);
}
bool ctr_source_load(CtrReader* r, CtrSource* s) {
    memset(s, 0, sizeof *s);
    if (container(r, s) && ctr_ncch_parse(r, s, &s->base, false) &&
        (!s->overlay || ctr_ncch_parse(r, s, &s->over, true)))
        return r->result == SNX_CTR_EXTRACT_OK;
    ctr_source_free(s);
    return false;
}
static bool no_links(const char* path) {
    char* part = snx_strdup(path);
    bool ok = true;
    for (size_t i = 0; ok; ++i) {
        if (!part[i] || part[i] == '/' || part[i] == '\\') {
            char saved = part[i];
            part[i] = 0;
            if (*part && snx_is_link(part) && !system_link(part)) ok = false;
            part[i] = saved;
            if (!saved) break;
        }
    }
    free(part);
    return ok;
}
SnxCtrExtractResult snx_ctr_extract(const char* input, const char* destination, const char* work, char** published,
                                    char* error, size_t error_size) {
    CtrReader r = {.error = error, .error_size = error_size};
    CtrSource s = {0};
    char *target = NULL, *parent = NULL, *stage = NULL, generated[SNX_PATH_SIZE];
    if (published) *published = NULL;
    if (error_size) error[0] = 0;
    if (!input || !snx_is_file(input) || !(r.input = snx_fopen(input, "rb"))) {
        ctr_fail(&r, SNX_CTR_EXTRACT_IO, "cannot open CIA input");
        goto done;
    }
#ifdef _WIN32
    if (_fseeki64(r.input, 0, SEEK_END) || _ftelli64(r.input) < 0) {
#else
    if (fseeko(r.input, 0, SEEK_END) || ftello(r.input) < 0) {
#endif
        ctr_fail(&r, SNX_CTR_EXTRACT_IO, "cannot measure CIA input");
        goto done;
    }
#ifdef _WIN32
    r.size = (uint64_t)_ftelli64(r.input);
#else
    r.size = (uint64_t)ftello(r.input);
#endif
    /* the input's own hash only names a default output, and checks the input did not change under the extraction:
       with an output given it is not taken (the contents' and ExeFS files' hashes still are, as they are copied) */
    r.quick = destination != NULL;
    bool input_hashed = !r.quick;
    if (!ctr_source_load(&r, &s)) goto done;
    r.quick = false;
    if (!destination) {
        char hex[65];
        for (size_t i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", s.input_hash[i]);
        if (snx_path_format(generated, sizeof generated, "%s/%016" PRIx64 "-%s", work ? work : "work/3ds", s.title_id,
                            hex)) {
            ctr_fail(&r, SNX_CTR_EXTRACT_IO, "the output path is over %d bytes", SNX_PATH_SIZE - 1);
            goto done;
        }
        destination = generated;
    }
    if (!*destination || !no_links(destination) || !(target = snx_absolute(destination))) {
        ctr_fail(&r, SNX_CTR_EXTRACT_IO, "invalid output path or symlink ancestor");
        goto done;
    }
    for (const unsigned char* p = (const unsigned char*)target; *p; ++p) {
        if (*p < 32 || *p == 127) {
            ctr_fail(&r, SNX_CTR_EXTRACT_IO, "output path may not contain control characters");
            goto done;
        }
    }
    if (snx_path_exists(target)) {
        ctr_fail(&r, SNX_CTR_EXTRACT_EXISTS, "output already exists; no cache reuse or overwrite: %s", target);
        goto done;
    }
    parent = snx_dirname(target);
    if (snx_mkdirs(parent) || !(stage = snx_temp_dir(parent))) {
        ctr_fail(&r, SNX_CTR_EXTRACT_IO, "cannot create private staging directory");
        goto done;
    }
    if ((s.kind == CTR_KIND_CIA && !ctr_cia_contents(&r, &s, stage)) || !ctr_ncch_extract(&r, &s, stage)) goto done;
    /* Detect normal concurrent input edits, including after content validation. */
    if (input_hashed && !ctr_hash_matches(&r, (CtrSpan){0, r.size}, s.input_hash)) goto done;
    if (snx_path_exists(target) || snx_replace(stage, target)) {
        ctr_fail(&r, SNX_CTR_EXTRACT_IO, "cannot publish extraction without overwriting an existing output");
        goto done;
    }
    free(stage);
    stage = NULL;
    if (published) {
        *published = target;
        target = NULL;
    }
done:
    if (stage) snx_remove_tree(stage);
    if (r.input) fclose(r.input);
    ctr_source_free(&s);
    if (target) free(target);
    if (parent) free(parent);
    if (stage) free(stage);
    return r.result;
}
