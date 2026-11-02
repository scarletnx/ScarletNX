/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Format references: Azahar ncch_container.h/.cpp and loader/ncch.cpp,
   68202bc4ff7edf9d7280ec0dcd6572b7cd1ae26e, GPL-3.0-or-later. */
#include "3ds/loader/internal.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

void ctr_ncch_free(CtrNcch* n) {
    free(n->code);
    n->code = NULL;
}
static bool parse(CtrReader* r, const CtrSource* s, CtrNcch* n, bool overlay) {
    unsigned char *h = n->header, *exheader = n->exheader, *exefs = n->exefs;
    const CtrSpan boot = overlay ? s->update : s->boot; /* the update NCCH lives in its partition, not the game's */
    if (!ctr_read(r, boot, 0, h, sizeof n->header)) return false;
    if (memcmp(h + 0x100, "NCCH", 4)) return ctr_fail(r, SNX_CTR_EXTRACT_UNSUPPORTED, "boot content is not NCCH");
    if (h[0x18e] || h[0x18c] != 1)
        return ctr_fail(r, SNX_CTR_EXTRACT_UNSUPPORTED, "unsupported NCCH media unit/platform");
    if (!(h[0x18f] & 4)) return ctr_fail(r, SNX_CTR_EXTRACT_UNSUPPORTED, "encrypted NCCH is not supported");
    if (!(h[0x18d] & 2) || ((h[0x18d] >> 2) & 7))
        return ctr_fail(r, SNX_CTR_EXTRACT_UNSUPPORTED, "boot NCCH is not an executable application");
    /* An NCCH's content size and section sizes are in media units of 0x200 bytes, except a proto image's (an early
       tool's), which counts bytes and matches the file it is in. Azahar's NCCHContainer decides the same way
       (is_proto) and reads a proto's ExHeader as any other. */
    unsigned units = snx_ld_le32(h + 0x104);
    uint64_t unit = 512;
    if ((uint64_t)units * unit != boot.size) {
        if (units != boot.size)
            return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "NCCH size does not match its container");
        unit = 1;
    }
    n->proto = unit == 1;
    uint64_t wanted = overlay ? ctr_update_title(s->title_id) : s->title_id;
    if (snx_ld_le64(h + 0x118) != wanted || snx_ld_le32(h + 0x180) != (n->proto ? 0 : 0x400))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "NCCH size/title/ExHeader mismatch");
    CtrSpan regions[4];
    const unsigned offsets[] = {0x190, 0x198, 0x1a0, 0x1b0};
    for (size_t i = 0; i < 4; ++i) {
        uint64_t at = (uint64_t)snx_ld_le32(h + offsets[i]) * unit;
        uint64_t size = (uint64_t)snx_ld_le32(h + offsets[i] + 4) * unit;
        if ((size && at < 0xa00) || !ctr_fits(boot.size, at, size))
            return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid NCCH section range");
        regions[i] = (CtrSpan){boot.offset + at, size};
        for (size_t j = 0; size && j < i; ++j)
            if (regions[j].size && regions[i].offset < regions[j].offset + regions[j].size &&
                regions[j].offset < regions[i].offset + size)
                return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "overlapping NCCH sections");
    }
    CtrSpan exe = regions[2], rom = regions[3];
    if (!exe.size || (!!rom.size == !!(h[0x18f] & 2)))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "missing ExeFS or inconsistent RomFS flag");
    uint64_t hash_size = (uint64_t)snx_ld_le32(h + 0x1a8) * unit;
    if (hash_size < sizeof n->exefs || hash_size > exe.size ||
        !ctr_hash_matches(r, (CtrSpan){exe.offset, hash_size}, h + 0x1c0))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid ExeFS superblock hash/range");
    if (!ctr_read(r, boot, 0x200, exheader, sizeof n->exheader)) return false;
    unsigned char hash[32];
    if (!ctr_sha256(r, exheader, 0x400, hash)) return false;
    if (memcmp(hash, h + 0x160, 32) || snx_ld_le64(exheader + 0x200) != wanted)
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "ExHeader hash/program ID mismatch");
    if (n->proto) { /* Azahar gives a proto's application the usual priority and the application resource limit */
        if (!exheader[0x20f]) exheader[0x20f] = 0x30;
        exheader[0x36f] = 0; /* the ARM11 local capabilities' resource limit category, at 0x200 + 0x16F */
    }
    CtrSegment* segments = n->segments;
    const unsigned segment_offsets[] = {0x10, 0x20, 0x30};
    uint64_t expected = 0, minimum = 0;
    uint32_t bss = n->bss = snx_ld_le32(exheader + 0x3c), stack = n->stack = snx_ld_le32(exheader + 0x1c);
    for (size_t i = 0; i < 3; ++i) {
        const unsigned char* p = exheader + segment_offsets[i];
        CtrSegment* s = &segments[i];
        *s = (CtrSegment){snx_ld_le32(p), snx_ld_le32(p + 4), snx_ld_le32(p + 8)};
        uint64_t length = (uint64_t)s->pages * 4096;
        uint64_t virtual_length = length + (i == 2 ? ((uint64_t)bss + 4095) / 4096 * 4096 : 0);
        /* Some homebrew makers report .data size INCLUDING BSS, while pages
           describe only initialized data. Never materialize BSS from .code. */
        uint64_t initialized = s->bytes;
        if (i == 2 && initialized > length && initialized >= bss) initialized -= bss;
        if (initialized > length || (virtual_length && ((s->address & 4095) || s->address < 0x100000 ||
            !ctr_fits(0x4000000, s->address, virtual_length))))
            return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid ExHeader segment/BSS range");
        for (size_t j = 0; virtual_length && j < i; ++j) {
            uint64_t end = (uint64_t)segments[j].address + (uint64_t)segments[j].pages * 4096;
            uint64_t this_end = (uint64_t)s->address + virtual_length;
            if (segments[j].pages && s->address < end && segments[j].address < this_end)
                return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "overlapping process segments");
        }
        minimum = expected + initialized;
        expected += length;
    }
    if (!segments[0].bytes || !segments[0].pages || expected > CTR_CODE_LIMIT || !stack || stack > 0x800000)
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid code/stack/BSS sizes");
    if (!ctr_read(r, exe, 0, exefs, sizeof n->exefs)) return false;
    char(*names)[9] = n->names;
    CtrSpan* files = n->files;
    int code_index = -1;
    for (size_t i = 0; i < 10; ++i) {
        const unsigned char* p = exefs + i * 16;
        if (!*p) {
            for (size_t k = 0; k < 16; ++k)
                if (p[k]) return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid empty ExeFS entry");
            continue;
        }
        bool terminated = false;
        for (size_t k = 0; k < 8; ++k) {
            if (p[k] >= 128 || (terminated && p[k]))
                return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid ExeFS name encoding/padding");
            if (!p[k]) terminated = true;
        }
        memcpy(names[i], p, 8);
        if (!ctr_name(names[i])) return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "unsafe ExeFS filename");
        uint64_t at = (uint64_t)snx_ld_le32(p + 8) + 0x200, size = snx_ld_le32(p + 12);
        if (!ctr_fits(exe.size, at, size)) return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "ExeFS file exceeds section");
        files[i] = (CtrSpan){exe.offset + at, size};
        for (size_t j = 0; j < i; ++j)
            if (*names[j] && (!strcmp(names[i], names[j]) ||
                              (files[i].size && files[j].size && files[i].offset < files[j].offset + files[j].size &&
                               files[j].offset < files[i].offset + size)))
                return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "duplicate or overlapping ExeFS files");
        if (!strcmp(names[i], ".code")) code_index = (int)i;
    }
    if (code_index < 0 || files[code_index].size > CTR_CODE_LIMIT || !files[code_index].size)
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "missing or oversized ExeFS .code");
    n->code_index = code_index;
    size_t stored_size = (size_t)files[code_index].size;
    unsigned char* stored = malloc(stored_size);
    n->code = calloc(1, (size_t)expected);
    n->code_size = (size_t)expected;
    bool ok = stored && n->code ? ctr_read(r, files[code_index], 0, stored, stored_size)
                                : ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "out of memory");
    size_t expanded = stored_size;
    /* A dump made with a tool that expands .code and leaves the ExHeader's compressed flag set holds plain code: a
       stream with no BLZ footer is taken as that when it fits the segments, as the console's loader would not have
       run it either */
    bool compressed = exheader[0xd] & 1;
    if (ok && compressed &&
        (stored_size < 8 || (uint64_t)stored_size + snx_ld_le32(stored + stored_size - 4) > expected ||
         !snx_ctr_blz(stored, stored_size, n->code, stored_size + snx_ld_le32(stored + stored_size - 4)))) {
        if (stored_size > expected)
            ok = ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid ExeFS BLZ stream");
        else
            compressed = false;
    } else if (ok && compressed)
        expanded += snx_ld_le32(stored + stored_size - 4);
    if (ok && !compressed) {
        if (stored_size > expected)
            ok = ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "code exceeds process segment sizes");
        else {
            memset(n->code, 0, n->code_size); /* a failed expansion may have written some */
            memcpy(n->code, stored, stored_size);
        }
    }
    free(stored);
    if (!ok) return false;
    if (expanded < minimum) return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "code is shorter than process segments");
    /* each ExeFS file's hash, which the extraction checks as it copies the file */
    for (size_t i = 0; i < 10; ++i)
        if (*names[i] && !r->quick && !ctr_hash_matches(r, files[i], exefs + 0x1e0 - i * 32)) return false;
    n->rom = rom;
    if (rom.size) {
        uint64_t rom_hash_size = (uint64_t)snx_ld_le32(h + 0x1b8) * unit;
        if (!rom_hash_size || rom_hash_size > rom.size ||
            !ctr_hash_matches(r, (CtrSpan){rom.offset, rom_hash_size}, h + 0x1e0))
            return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid RomFS superblock hash/range");
        if (!ctr_romfs_check(r, rom, NULL, &n->level3)) return false;
    }
    return true;
}
bool ctr_ncch_parse(CtrReader* r, const CtrSource* s, CtrNcch* n, bool overlay) {
    memset(n, 0, sizeof *n);
    if (parse(r, s, n, overlay)) return true;
    ctr_ncch_free(n);
    return false;
}
/* the container the game was read from, and whether the game is a cartridge's update, recorded for a player's
   reference; the image loader ignores it, as it ignores the rest of the manifest */
static const char* kind_name(CtrKind kind) {
    return kind == CTR_KIND_CIA ? "cia" : kind == CTR_KIND_CCI ? "cci" : "ncch";
}
bool ctr_ncch_write(CtrReader* r, const CtrSource* s, const CtrNcch* n, const char* stage) {
    const CtrSegment* segments = n->segments;
    char* path = snx_path_join(stage, "exefs");
    bool ok = ctr_directory(r, path);
    free(path);
    for (size_t i = 0; i < 10 && ok; ++i) {
        if (!*n->names[i]) continue;
        char file[SNX_PATH_SIZE];
        if (snx_path_format(file, sizeof file, "%s/exefs/%s", stage, n->names[i]))
            return ctr_fail(r, SNX_CTR_EXTRACT_IO, "an ExeFS file's path is over %d bytes", SNX_PATH_SIZE - 1);
        const unsigned char* file_hash = n->exefs + 0x1e0 - i * 32;
        if ((int)i == n->code_index)
            ok = ctr_hash_matches(r, n->files[i], file_hash) && ctr_write(r, file, n->code, n->code_size);
        else
            ok = ctr_copy(r, n->files[i], file, file_hash);
    }
    if (!ok) return false;
    path = snx_path_join(stage, "ncch.bin");
    ok = ctr_write(r, path, n->header, sizeof n->header);
    free(path);
    path = snx_path_join(stage, "exheader.bin");
    ok = ok && ctr_write(r, path, n->exheader, sizeof n->exheader);
    free(path);
    if (!ok) return false;
    if (n->rom.size) {
        CtrSpan level3;
        if (!ctr_romfs_check(r, n->rom, stage, &level3)) return false;
    } else {
        path = snx_path_join(stage, "romfs");
        ok = ctr_directory(r, path);
        free(path);
        if (!ok) return false;
    }
    char input_hash[80] = "", code_hash[65];
    unsigned char hash[32];
    if (!ctr_sha256(r, n->code, n->code_size, hash)) return false;
    for (size_t i = 0; i < 32; ++i) {
        if (s->input_hashed) snprintf(input_hash + 13 + i * 2, 3, "%02x", s->input_hash[i]);
        snprintf(code_hash + i * 2, 3, "%02x", hash[i]);
    }
    if (s->input_hashed) { /* the input's hash is taken only when it names the output (snx_ctr_extract) */
        memcpy(input_hash, "input_sha256=", 13);
        input_hash[77] = '\n';
        input_hash[78] = 0;
    }
    char* manifest =
        snx_format("format=1\nstate=extracted\ncontainer=%s\noverlay=%u\ntitle_id=%016" PRIx64 "\n"
                   "title_version=%u\n%s"
                   "code_sha256=%s\nentry=0x%08" PRIx32 "\nstack_size=0x%08" PRIx32 "\nbss_size=0x%08" PRIx32 "\n"
                   "text_address=0x%08" PRIx32 "\ntext_pages=%" PRIu32 "\ntext_bytes=%" PRIu32 "\n"
                   "ro_address=0x%08" PRIx32 "\nro_pages=%" PRIu32 "\nro_bytes=%" PRIu32 "\n"
                   "data_address=0x%08" PRIx32 "\ndata_pages=%" PRIu32 "\ndata_bytes=%" PRIu32 "\n",
                   kind_name(s->kind), s->overlay ? 1 : 0, s->title_id, (unsigned)s->version, input_hash, code_hash,
                   segments[0].address, n->stack,
                   n->bss, segments[0].address, segments[0].pages, segments[0].bytes, segments[1].address,
                   segments[1].pages, segments[1].bytes, segments[2].address, segments[2].pages, segments[2].bytes);
    path = snx_path_join(stage, "manifest.ini");
    ok = ctr_write(r, path, manifest, strlen(manifest));
    free(path);
    free(manifest);
    return ok;
}
/* The game extracted into stage: the NCCH the container names for it, which is the update's when a cartridge
   carries one, as the running game's ExHeader and code are */
bool ctr_ncch_extract(CtrReader* r, const CtrSource* s, const char* stage) {
    CtrNcch n;
    if (!ctr_ncch_parse(r, s, &n, s->overlay)) return false;
    bool ok = ctr_ncch_write(r, s, &n, stage);
    ctr_ncch_free(&n);
    return ok;
}
