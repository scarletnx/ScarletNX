/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Format reference: Azahar NCCHContainer and Loader::AppLoader_NCCH at
   68202bc4ff7edf9d7280ec0dcd6572b7cd1ae26e, GPL-3.0-or-later, and 3dbrew's NCSD layout. */
/* A cartridge image, a CCI or a .3ds, as a console dump holds one: an NCSD header at 0, its magic at 0x100, and a
   table of eight partitions, each described by an offset and a size in media units of 0x200 bytes. The game is
   partition 0 (Azahar's NCSDContentIndex::Main; 1 is the manual, 2 a DLP child, 6 and 7 the updates), and it holds
   an NCCH exactly as a CIA's boot content does, so the loader reads that partition's NCCH where it lies and checks
   it as it checks any other.

   Azahar reads partition 0's NCCH and re-reads its header; the update is taken by its title ID here, which is the
   base game's with the update title's high bits (0x0004000e00000000, Azahar's UPDATE_TID_HIGH), so a New 3DS update
   in partition 6 and one in partition 7 are both found. A cartridge's update NCCH is then the game's ExHeader and
   code, as Azahar's overlay_ncch is, while the title ID stays the base NCCH's, as its ReadProgramId. */
#include "3ds/loader/internal.h"
#include <stdlib.h>
#include <string.h>

#define NCSD_MAGIC     0x100
#define NCSD_PARTITIONS 0x120
#define NCSD_PARTITION_COUNT 8
#define NCSD_MEDIA_UNIT 512u
/* whether the NCCH header at h is the application to play: the same executable application a CIA's boot content
   must be (ncch.c checks all of it again as it parses) */
static bool is_game(const unsigned char* h) {
    return !memcmp(h + 0x100, "NCCH", 4) && !h[0x18e] && h[0x18c] == 1 && (h[0x18d] & 2) && !((h[0x18d] >> 2) & 7);
}
/* a partition's bytes where they lie in the image; false when it is not there (size 0, *present false) or its
   table's offset and size do not fit in the image (*present true, malformed data) */
static bool partition_span(const unsigned char* ncsd, unsigned index, uint64_t image, CtrSpan* out, bool* present) {
    const unsigned char* p = ncsd + NCSD_PARTITIONS + index * 8;
    uint64_t at = (uint64_t)snx_ld_le32(p) * NCSD_MEDIA_UNIT, size = (uint64_t)snx_ld_le32(p + 4) * NCSD_MEDIA_UNIT;
    *present = size != 0;
    if (!*present || !at || !ctr_fits(image, at, size)) return false;
    *out = (CtrSpan){at, size};
    return true;
}
/* An NCCH's span inside its partition: its content size in media units, except a proto image's, which counts bytes
   and is the partition's size (Azahar's is_proto). The header is read first, so the partition only has to hold it */
static bool ncch_span(CtrReader* r, CtrSpan partition, uint64_t* title, CtrSpan* out) {
    unsigned char h[0x200];
    if (!ctr_read(r, partition, 0, h, sizeof h)) return false;
    unsigned units = snx_ld_le32(h + 0x104);
    uint64_t size = (uint64_t)units * NCSD_MEDIA_UNIT;
    if (size != partition.size && (size = units) != partition.size &&
        (size < 0x200 || !ctr_fits(partition.size, 0, size)))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "the NCCH's size does not match its NCSD partition");
    *out = (CtrSpan){partition.offset, size};
    *title = snx_ld_le64(h + 0x118);
    return true;
}
bool ctr_ncsd_load(CtrReader* r, CtrSource* s) {
    if (r->size > CTR_CARD_LIMIT) return ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "the cartridge image exceeds 8 GiB");
    unsigned char* ncsd = calloc(1, 0x200);
    if (!ncsd) return ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "out of memory");
    bool ok = ctr_read(r, (CtrSpan){0, r->size}, 0, ncsd, 0x200) && !memcmp(ncsd + NCSD_MAGIC, "NCSD", 4);
    if (!ok) ctr_fail(r, SNX_CTR_EXTRACT_UNSUPPORTED, "the cartridge image has no NCSD header");
    bool present = false;
    if (ok && !partition_span(ncsd, 0, r->size, &s->boot, &present))
        ok = ctr_fail(r, present ? SNX_CTR_EXTRACT_FORMAT : SNX_CTR_EXTRACT_UNSUPPORTED,
                      present ? "the cartridge's game partition (0) is not in the image"
                              : "the cartridge's game partition (0) is empty");
    unsigned char header[0x200] = {0};
    uint64_t base = 0;
    if (ok) ok = ctr_read(r, s->boot, 0, header, sizeof header) && is_game(header);
    if (!ok && r->result == SNX_CTR_EXTRACT_OK)
        ctr_fail(r, SNX_CTR_EXTRACT_UNSUPPORTED, "the cartridge's game partition (0) holds no game NCCH");
    if (ok) ok = ncch_span(r, s->boot, &base, &s->boot);
    if (ok) {
        s->kind = CTR_KIND_CCI;
        s->title_id = base;
        s->update = (CtrSpan){0, 0};
        /* the update, when the card carries one, by its title ID: partition 7 for a game's, 6 for a New 3DS one */
        for (unsigned index = 1; index < NCSD_PARTITION_COUNT; ++index) {
            CtrSpan part;
            bool there = false;
            uint64_t title = 0;
            if (!partition_span(ncsd, index, r->size, &part, &there)) continue;
            if (!ctr_read(r, part, 0, header, sizeof header) || !is_game(header) ||
                snx_ld_le64(header + 0x118) != ctr_update_title(base))
                continue;
            if (ncch_span(r, part, &title, &s->update)) s->overlay = true;
            break;
        }
    }
    free(ncsd);
    if (!ok || r->result != SNX_CTR_EXTRACT_OK) return false;
    return r->quick || (s->input_hashed = ctr_hash(r, (CtrSpan){0, r->size}, s->input_hash));
}
bool ctr_ncch_load(CtrReader* r, CtrSource* s) {
    s->kind = CTR_KIND_NCCH;
    s->boot = (CtrSpan){0, r->size};
    unsigned char header[0x200];
    if (!ctr_read(r, s->boot, 0, header, sizeof header) || r->size < 0x200)
        return ctr_fail(r, SNX_CTR_EXTRACT_UNSUPPORTED, "the file is too short to be an NCCH");
    if (!is_game(header))
        return ctr_fail(r, SNX_CTR_EXTRACT_UNSUPPORTED, "the file is not an executable application NCCH");
    unsigned units = snx_ld_le32(header + 0x104);
    uint64_t content = (uint64_t)units * NCSD_MEDIA_UNIT;
    if (content != r->size && units != r->size)
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "the NCCH's size does not match the file");
    s->title_id = snx_ld_le64(header + 0x118);
    return r->quick || (s->input_hashed = ctr_hash(r, (CtrSpan){0, r->size}, s->input_hash));
}
