/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The add-on content of a game: a CIA of title type 0x0004008c, whose TMD lists contents that are NCCHs holding only
   a RomFS, as 3dbrew's NCCH and TMD pages describe them. Each present content's header and RomFS are checked as an
   application's NCCH are, and its level 3 is handed out as a view into the mapping. */
#include "3ds/loader/internal.h"
#include <stdlib.h>
#include <string.h>

/* one content's NCCH, at span in the file: unencrypted, of this title, its RomFS (when it has one) checked */
static bool parse_content(CtrReader* r, const CtrSource* s, CtrSpan span, CtrDlcContent* out) {
    unsigned char h[0x200];
    if (span.size < sizeof h || !ctr_read(r, span, 0, h, sizeof h))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid DLC content");
    if (memcmp(h + 0x100, "NCCH", 4)) return ctr_fail(r, SNX_CTR_EXTRACT_UNSUPPORTED, "a DLC content is not an NCCH");
    if (!(h[0x18f] & 4)) return ctr_fail(r, SNX_CTR_EXTRACT_UNSUPPORTED, "an encrypted NCCH is not supported");
    if (h[0x18e] || h[0x18c] != 1)
        return ctr_fail(r, SNX_CTR_EXTRACT_UNSUPPORTED, "unsupported NCCH media unit/platform");
    if ((uint64_t)snx_ld_le32(h + 0x104) * 512 != span.size || snx_ld_le64(h + 0x118) != s->title_id)
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "a DLC content's size or title does not match its container");
    uint64_t at = (uint64_t)snx_ld_le32(h + 0x1b0) * 512, size = (uint64_t)snx_ld_le32(h + 0x1b4) * 512;
    if ((size && at < 0x200) || !ctr_fits(span.size, at, size) || !!size == !!(h[0x18f] & 2))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid DLC RomFS range or flag");
    out->level3 = (CtrSpan){0, 0};
    if (!size) return true;
    uint64_t hashed = (uint64_t)snx_ld_le32(h + 0x1b8) * 512;
    CtrSpan rom = {span.offset + at, size};
    if (!hashed || hashed > size || !ctr_hash_matches(r, (CtrSpan){rom.offset, hashed}, h + 0x1e0))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid RomFS superblock hash/range");
    return ctr_romfs_check(r, rom, NULL, &out->level3);
}
bool ctr_dlc_load(CtrReader* r, const CtrSource* s, CtrDlc* d) {
    *d = (CtrDlc){0};
    d->contents = calloc(s->chunks, sizeof *d->contents);
    if (!d->contents) return ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "out of memory");
    uint64_t at = snx_align_up(s->tmd.offset + s->tmd.size, 64);
    for (size_t i = 0; i < s->chunks; ++i) {
        const unsigned char* chunk = s->metadata + s->body + 0x9c4 + i * 0x30;
        unsigned index = snx_ld_be16(chunk + 4);
        if (!(s->header[0x20 + (index >> 3)] & (0x80u >> (index & 7)))) continue;
        CtrDlcContent* c = &d->contents[d->count];
        CtrSpan span = {at, snx_ld_be64(chunk + 8)};
        at += span.size;
        if (!parse_content(r, s, span, c)) {
            ctr_dlc_free(d);
            return false;
        }
        c->index = (uint16_t)index;
        c->type = snx_ld_be16(chunk + 6);
        c->id = snx_ld_be32(chunk);
        c->size = span.size;
        ++d->count;
    }
    return true;
}
void ctr_dlc_free(CtrDlc* d) {
    free(d->contents);
    *d = (CtrDlc){0};
}
