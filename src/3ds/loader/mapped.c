/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A game read where it lies: the file itself mapped read-only and shared for as long as it is open, checked once as the
   extraction checks it, its ExeFS files and RomFS level 3 handed out as views into the mapping. Nothing is written */
#include "3ds/loader/internal.h"
#include "common/filesystem/host.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <sys/stat.h>
#endif

struct SnxCtrSource {
    int fd;
    const unsigned char* map;
    uint64_t size, mtime;
    uint64_t base; /* where the game lies in the mapped file: a bundle's entry (ctr_tar_game), else 0, and the
                      spans the parsed source holds are relative to it */
    CtrSource source;
    struct SnxCtrSource* update; /* a standalone update title laid over this game, NULL without one */
    CtrDlc dlc;                  /* a DLC file's contents (source is then its CIA) */
    struct SnxCtrSource* dlcs[SNX_CTR_MAX_DLC]; /* the DLC files given with the game, each a source of its own */
    size_t dlc_count;
};
/* the host's modification time of path, UTC Unix milliseconds, as fs_tree_mtime reads it; 0 when it can't say */
static uint64_t modified(const char* path) {
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &attributes)) return 0;
    ULARGE_INTEGER time = {.LowPart = attributes.ftLastWriteTime.dwLowDateTime,
                           .HighPart = attributes.ftLastWriteTime.dwHighDateTime};
    return time.QuadPart < 116444736000000000ull ? 0 : (time.QuadPart - 116444736000000000ull) / 10000;
#else
    struct stat st;
    if (stat(path, &st) || st.st_mtime < 0) return 0;
#if defined(__APPLE__)
    return (uint64_t)st.st_mtime * 1000 + (uint64_t)st.st_mtimespec.tv_nsec / 1000000;
#else
    return (uint64_t)st.st_mtime * 1000 + (uint64_t)st.st_mtim.tv_nsec / 1000000;
#endif
#endif
}
void snx_ctr_source_close(SnxCtrSource* c) {
    if (!c) return;
    snx_ctr_source_close(c->update);
    for (size_t i = 0; i < c->dlc_count; ++i) snx_ctr_source_close(c->dlcs[i]);
    ctr_dlc_free(&c->dlc);
    ctr_source_free(&c->source);
    plat_file_unshare(c->fd, c->map, c->size);
    free(c);
}
static SnxCtrExtractResult open_file(const char* input, bool dlc, SnxCtrSource** out, char* error,
                                     size_t error_size) {
    /* read where it lies every launch: no whole-data hashes (CtrReader.quick) */
    CtrReader r = {.error = error, .error_size = error_size, .quick = true};
    SnxCtrSource* c = calloc(1, sizeof *c);
    *out = NULL;
    if (error_size) error[0] = 0;
    if (!c) {
        ctr_fail(&r, SNX_CTR_EXTRACT_LIMIT, "out of memory");
        return r.result;
    }
    c->fd = -1;
    if (!input || !snx_is_file(input)) {
        ctr_fail(&r, SNX_CTR_EXTRACT_IO, "cannot open the game");
        goto done;
    }
    c->size = snx_file_size(input);
    /* an empty file holds no game: the host cannot map one, and the extraction refuses it as malformed data, not
       as a host failure, so a read where it lies says the same */
    if (!c->size) {
        ctr_fail(&r, SNX_CTR_EXTRACT_FORMAT, "the game is empty");
        goto done;
    }
    /* the largest game the loader reads: a CIA keeps the extraction's input limit, and a cartridge image is only
       mapped and read, so it may be as large as a card is */
    if (c->size > CTR_CARD_LIMIT) {
        ctr_fail(&r, SNX_CTR_EXTRACT_LIMIT, "the game exceeds the 8 GiB input limit");
        goto done;
    }
    const uint8_t* view = NULL;
    if (plat_file_share(input, c->size, &c->fd, &view) || !view) {
        ctr_fail(&r, SNX_CTR_EXTRACT_IO, "cannot map the game");
        goto done;
    }
    c->map = view;
    c->mtime = modified(input);
    r.map = c->map;
    r.size = c->size;
    /* every check as the extraction makes it, over the mapping: the NCCHs' headers and superblocks' own hashes, and
       a CIA's each content's; nothing of the whole data is hashed, as a launch costs no seconds on it */
    if (!(dlc ? ctr_dlc_source_load(&r, &c->source, &c->dlc) : ctr_source_load(&r, &c->source))) goto done;
    /* a CIA's contents' hashes, which the extraction checks as it copies them; a cartridge image and a bare NCCH
       have no content table, and their own headers' and superblocks' hashes are already checked */
    if (c->source.kind == CTR_KIND_CIA && !ctr_cia_contents(&r, &c->source, NULL)) goto done;
    c->base = r.base; /* the container may be a bundle's entry: the views below read from it */
done:
    if (r.result == SNX_CTR_EXTRACT_OK)
        *out = c;
    else
        snx_ctr_source_close(c);
    return r.result;
}
static bool is_update(const SnxCtrSource* c) { return (c->source.title_id >> 32) == 0x0004000e; }
SnxCtrExtractResult snx_ctr_source_open(const char* input, SnxCtrSource** out, char* error, size_t error_size) {
    SnxCtrExtractResult result = open_file(input, false, out, error, error_size);
    if (result == SNX_CTR_EXTRACT_OK && is_update(*out)) {
        snx_ctr_source_close(*out);
        *out = NULL;
        snprintf(error, error_size, "%s is an update: give it with --update beside the game it updates", input);
        return SNX_CTR_EXTRACT_UNSUPPORTED;
    }
    return result;
}
SnxCtrExtractResult snx_ctr_source_open_update(const char* input, const char* update, SnxCtrSource** out, char* error,
                                               size_t error_size) {
    SnxCtrSource* patch = NULL;
    SnxCtrExtractResult result = snx_ctr_source_open(input, out, error, error_size);
    if (result != SNX_CTR_EXTRACT_OK) return result;
    result = open_file(update, false, &patch, error, error_size);
    if (result == SNX_CTR_EXTRACT_OK && !is_update(patch)) {
        snprintf(error, error_size, "%s is not an update", update);
        result = SNX_CTR_EXTRACT_FORMAT;
    } else if (result == SNX_CTR_EXTRACT_OK && patch->source.title_id != ctr_update_title((*out)->source.title_id)) {
        snprintf(error, error_size, "%s is the update of the title %08" PRIX64 ", not of this game's %08" PRIX64, update,
                 patch->source.title_id & 0xFFFFFFFFull, (*out)->source.title_id & 0xFFFFFFFFull);
        result = SNX_CTR_EXTRACT_FORMAT;
    }
    if (result != SNX_CTR_EXTRACT_OK) {
        snx_ctr_source_close(patch);
        snx_ctr_source_close(*out);
        *out = NULL;
        return result;
    }
    (*out)->update = patch;
    return SNX_CTR_EXTRACT_OK;
}
uint64_t snx_ctr_source_title(const SnxCtrSource* c) { return c->source.title_id; }
uint64_t snx_ctr_source_mtime(const SnxCtrSource* c) {
    return c->update && c->update->mtime > c->mtime ? c->update->mtime : c->mtime;
}
/* the game's NCCH: a cartridge's update when it carries one, as the console runs that one's ExHeader and code */
static const CtrNcch* game_ncch(const SnxCtrSource* c) {
    return c->source.overlay ? &c->source.over : &c->source.base;
}
const unsigned char* snx_ctr_source_exheader(const SnxCtrSource* c) {
    return c->update ? snx_ctr_source_exheader(c->update) : game_ncch(c)->exheader;
}
const unsigned char* snx_ctr_source_code(const SnxCtrSource* c, size_t* size) {
    if (c->update) return snx_ctr_source_code(c->update, size);
    const CtrNcch* n = game_ncch(c);
    *size = n->code_size;
    return n->code;
}
const unsigned char* snx_ctr_source_exefs(const SnxCtrSource* c, const char* name, uint64_t* size) {
    if (c->update) {
        const unsigned char* file = snx_ctr_source_exefs(c->update, name, size);
        if (file) return file;
    }
    const CtrNcch* n = game_ncch(c);
    /* an ExeFS file's bytes are in the mapping, at its absolute offset in the file */
    for (size_t i = 0; i < 10; ++i)
        if (*n->names[i] && !strcmp(n->names[i], name)) {
            *size = n->files[i].size;
            return c->map + c->base + n->files[i].offset;
        }
    *size = 0;
    return NULL;
}
const unsigned char* snx_ctr_source_romfs(const SnxCtrSource* c, uint64_t* size) {
    if (c->update) {
        const unsigned char* image = snx_ctr_source_romfs(c->update, size);
        if (image) return image;
    }
    const CtrNcch* n = game_ncch(c);
    *size = n->level3.size;
    return *size ? c->map + c->base + n->level3.offset : NULL;
}
SnxCtrExtractResult snx_ctr_source_add_dlc(SnxCtrSource* game, const char* input, char* error, size_t error_size) {
    SnxCtrSource* dlc = NULL;
    SnxCtrExtractResult result = open_file(input, true, &dlc, error, error_size);
    if (result != SNX_CTR_EXTRACT_OK) return result;
    /* a game's add-on content shares its unique ID (the title ID's bits 8 to 31) */
    if ((dlc->source.title_id ^ game->source.title_id) & 0xFFFFFF00ull) {
        snprintf(error, error_size, "%s is the DLC of the title %016" PRIX64 ", not of this game's %016" PRIX64, input,
                 dlc->source.title_id, game->source.title_id);
        result = SNX_CTR_EXTRACT_FORMAT;
    } else if (game->dlc_count == SNX_CTR_MAX_DLC) {
        snprintf(error, error_size, "a game takes %d DLC files at most", SNX_CTR_MAX_DLC);
        result = SNX_CTR_EXTRACT_LIMIT;
    }
    for (size_t i = 0; result == SNX_CTR_EXTRACT_OK && i < game->dlc_count; ++i)
        if (game->dlcs[i]->source.title_id == dlc->source.title_id) {
            snprintf(error, error_size, "%s is a DLC given twice", input);
            result = SNX_CTR_EXTRACT_FORMAT;
        }
    if (result != SNX_CTR_EXTRACT_OK) {
        snx_ctr_source_close(dlc);
        return result;
    }
    game->dlcs[game->dlc_count++] = dlc;
    return SNX_CTR_EXTRACT_OK;
}
size_t snx_ctr_source_dlc_count(const SnxCtrSource* c) { return c->dlc_count; }
static const SnxCtrSource* dlc_of(const SnxCtrSource* c, uint64_t title) {
    for (size_t i = 0; i < c->dlc_count; ++i)
        if (c->dlcs[i]->source.title_id == title) return c->dlcs[i];
    return NULL;
}
bool snx_ctr_source_dlc_title(const SnxCtrSource* c, size_t i, SnxCtrDlcTitle* out) {
    const SnxCtrSource* d = i < c->dlc_count ? c->dlcs[i] : NULL;
    if (!d) return false;
    uint64_t size = 0;
    for (size_t k = 0; k < d->dlc.count; ++k) size += d->dlc.contents[k].size;
    *out = (SnxCtrDlcTitle){d->source.title_id, size, d->source.version, (uint32_t)d->dlc.count};
    return true;
}
bool snx_ctr_source_dlc_find(const SnxCtrSource* c, uint64_t title, SnxCtrDlcTitle* out) {
    for (size_t i = 0; i < c->dlc_count; ++i)
        if (c->dlcs[i]->source.title_id == title) return snx_ctr_source_dlc_title(c, i, out);
    return false;
}
bool snx_ctr_source_dlc_content(const SnxCtrSource* c, uint64_t title, size_t i, SnxCtrDlcContent* out) {
    const SnxCtrSource* d = dlc_of(c, title);
    if (!d || i >= d->dlc.count) return false;
    const CtrDlcContent* k = &d->dlc.contents[i];
    *out = (SnxCtrDlcContent){k->index, k->type, k->id, k->size};
    return true;
}
const unsigned char* snx_ctr_source_dlc_romfs(const SnxCtrSource* c, uint64_t title, unsigned index, uint64_t* size,
                                              bool* found) {
    const SnxCtrSource* d = dlc_of(c, title);
    *size = 0;
    *found = false;
    for (size_t i = 0; d && i < d->dlc.count; ++i)
        if (d->dlc.contents[i].index == index) {
            const CtrSpan* rom = &d->dlc.contents[i].level3;
            *found = true;
            *size = rom->size;
            return rom->size ? d->map + d->base + rom->offset : NULL;
        }
    return NULL;
}
