/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_LOADER_INTERNAL_H
#define SNX_CTR_LOADER_INTERNAL_H
#include "3ds/loader/ctr_loader.h"
#include "common/utilities/common.h"
#include "common/utilities/ascii.h"
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"
#include <openssl/evp.h>
#include <stdio.h>
#define CTR_METADATA_LIMIT (16u * 1024u * 1024u)
#define CTR_CODE_LIMIT     (64u * 1024u * 1024u)
/* a cartridge image is read where it lies and nothing of it is written, so it may be as large as a card: the
   biggest the console takes is 8 GiB */
#define CTR_CARD_LIMIT (8ull * 1024ull * 1024ull * 1024ull)
typedef struct {
    uint64_t offset, size;
} CtrSpan;
typedef struct {
    FILE* input;
    const unsigned char* map; /* the input mapped (snx_ctr_source_open), read in place of input */
    uint64_t base; /* where the game lies in the input: a bundle's entry (ctr_tar_game), else 0. Every span and
                      the whole-file reads are relative to it */
    uint64_t size, output_bytes;
    size_t entries;
    SnxCtrExtractResult result;
    char* error;
    size_t error_size;
    /* the whole-data hashes left out: the input's, each content's and each ExeFS file's, which cost seconds on a
       retail title every launch. The headers' and superblocks' own small hashes are still checked */
    bool quick;
} CtrReader;
typedef struct {
    uint32_t address, pages, bytes;
} CtrSegment;
/* The boot NCCH as ctr_ncch_parse checked it: its headers, the ExeFS files where they lie in the file (names[i][0] 0
   for an empty slot), the .code expanded to its segments' pages, and the RomFS section with its level 3 (size 0 for
   none). proto is an early tool's image, whose header's content size and section sizes count bytes, not media
   units, and whose ExHeader is still there */
typedef struct {
    unsigned char header[0x200], exheader[0x800], exefs[0x200];
    char names[10][9];
    CtrSpan files[10], rom, level3;
    int code_index;
    CtrSegment segments[3];
    uint32_t bss, stack;
    unsigned char* code;
    size_t code_size;
    bool proto;
} CtrNcch;
/* What the game file is, decided by its own header, not its name */
typedef enum {
    CTR_KIND_CIA,  /* an installable archive: a header, a certificate chain, a ticket, a TMD and the contents */
    CTR_KIND_CCI,  /* a cartridge image: an NCSD header and its partitions, each an NCCH */
    CTR_KIND_NCCH  /* a bare NCCH (.cxi, an .app, homebrew's .hbb) */
} CtrKind;
/* The game a container holds: its identity, the NCCHs the container names and the CIA's own tables. The game is the
   update NCCH when a cartridge carries an update partition whose ExHeader, code, ExeFS sections and RomFS are then
   the game's (overlay), as Azahar's overlay_ncch is, while the title ID the container is named by stays the base
   NCCH's, as the card is one game */
typedef struct {
    CtrKind kind;
    uint64_t title_id;
    uint16_t version;
    CtrSpan boot, update; /* the base and the update NCCH, where they lie in the file */
    unsigned char input_hash[32];
    bool input_hashed; /* input_hash taken: not in a quick read */
    bool overlay;      /* the update NCCH parsed and in use */
    /* a CIA's tables (kind CTR_KIND_CIA only) */
    unsigned char *header, *metadata;
    CtrSpan cert, ticket, tmd, meta;
    size_t chunks, body;
    CtrNcch base, over;
} CtrSource;
/* where a ticket's or TMD's body starts after its signature type, signature and padding: 0 for an unknown type */
static inline size_t ctr_signed_body(uint32_t signature) {
    switch (signature) {
        case 0x10000: case 0x10003: return 0x240;
        case 0x10001: case 0x10004: return 0x140;
        case 0x10002: case 0x10005: return 0x40;
        default: return 0;
    }
}
static inline bool ctr_fits(uint64_t total, uint64_t at, uint64_t size) { return at <= total && size <= total - at; }
/* an update title's ID from the base game's, as Azahar's UPDATE_TID_HIGH makes it: a cartridge's update partition
   and an installed update both hold this one */
static inline uint64_t ctr_update_title(uint64_t base) { return (base & 0xFFFFFFFFull) | 0x0004000e00000000ull; }
bool ctr_fail(CtrReader* r, SnxCtrExtractResult result, const char* format, ...);
bool ctr_read(CtrReader* r, CtrSpan span, uint64_t at, void* output, size_t size);
bool ctr_hash(CtrReader* r, CtrSpan span, unsigned char hash[32]);
bool ctr_sha256(CtrReader* r, const void* data, size_t size, unsigned char hash[32]);
bool ctr_hash_matches(CtrReader* r, CtrSpan span, const unsigned char expected[32]);
bool ctr_copy(CtrReader* r, CtrSpan span, const char* path, const unsigned char* expected);
bool ctr_write(CtrReader* r, const char* path, const void* data, size_t size);
bool ctr_directory(CtrReader* r, const char* path);
bool ctr_name(const char* name);
bool ctr_cia_load(CtrReader* r, CtrSource* s);
/* every present content copied into stage/contents with the CIA's sections, its hash checked; stage NULL checks
   the hashes alone */
bool ctr_cia_contents(CtrReader* r, const CtrSource* s, const char* stage);
void ctr_cia_free(CtrSource* s);
/* a cartridge image (a CCI, a .3ds): the NCSD header's game partition, and its update partition when it carries
   one; an image without a game partition is refused */
bool ctr_ncsd_load(CtrReader* r, CtrSource* s);
/* a bare NCCH filling the file (.cxi, an .app, homebrew's .hbb) */
bool ctr_ncch_load(CtrReader* r, CtrSource* s);
/* The container read and its NCCHs parsed: the game by its own header, every check the extraction makes. */
bool ctr_source_load(CtrReader* r, CtrSource* s);
void ctr_source_free(CtrSource* s);
/* Every check the extraction makes, nothing written. On failure n holds nothing to free. overlay is a cartridge's
   update NCCH: its title ID is the base's update title's, not the base NCCH's */
bool ctr_ncch_parse(CtrReader* r, const CtrSource* s, CtrNcch* n, bool overlay);
/* A parsed NCCH extracted into stage: its ExeFS (.code expanded), headers, RomFS files and manifest */
bool ctr_ncch_write(CtrReader* r, const CtrSource* s, const CtrNcch* n, const char* stage);
void ctr_ncch_free(CtrNcch* n);
bool ctr_ncch_extract(CtrReader* r, const CtrSource* s, const char* stage);
/* The RomFS section's level 3 and its metadata walked as the extraction walks it, its files copied into
   stage/romfs, or with stage NULL only checked */
bool ctr_romfs_check(CtrReader* r, CtrSpan romfs, const char* stage, CtrSpan* level3);
/* Whether the input is a tar bundle holding the game, and which bytes of it are the game's: a bundle is what
   Azahar's .bcci/.bcxi/.bcia is, a tar of a game file beside what a publisher's note is, and the game is its
   first entry whose name is a game's (a .cia, .cci, .3ds, .cxi, .app or .hbb). True, the reader continuing either
   way: not a tar (*is_tar false, the reader untouched), or a bundle whose game is at r->base and r->size (*is_tar
   true). False with the reader's error set: a malformed bundle, or one holding no game */
bool ctr_tar_game(CtrReader* r, bool* is_tar);
#endif
