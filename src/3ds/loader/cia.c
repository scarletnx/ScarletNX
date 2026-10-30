/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Format reference: Azahar CIAContainer, Ticket, TitleMetadata at
   68202bc4ff7edf9d7280ec0dcd6572b7cd1ae26e (GPL-3.0-or-later).
   New bounds-checked streaming implementation; signatures are NOT authenticated. */
#include "3ds/loader/internal.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static bool bounded(CtrReader* r, CtrSpan span) {
    return ctr_fits(r->size, span.offset, span.size) ||
           ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "CIA section exceeds input size");
}
static int compare_ids(const void* a, const void* b) {
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return (x > y) - (x < y);
}
bool ctr_cia_load(CtrReader* r, CtrSource* s) {
    if (r->size > SNX_CTR_MAX_CONTENT) return ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "CIA exceeds 4 GiB input limit");
    s->header = malloc(0x2020);
    if (!s->header) return ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "out of memory");
    if (!ctr_read(r, (CtrSpan){0, r->size}, 0, s->header, 0x2020)) return false;
    const unsigned char* h = s->header;
    s->kind = CTR_KIND_CIA;
    if (snx_ld_le32(h) != 0x2020 || snx_ld_le16(h + 4) || snx_ld_le16(h + 6))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "unsupported CIA header/type/version");
    s->cert = (CtrSpan){0x2040, snx_ld_le32(h + 8)};
    s->ticket = (CtrSpan){snx_align_up(s->cert.offset + s->cert.size, 64), snx_ld_le32(h + 12)};
    s->tmd = (CtrSpan){snx_align_up(s->ticket.offset + s->ticket.size, 64), snx_ld_le32(h + 16)};
    CtrSpan content = {snx_align_up(s->tmd.offset + s->tmd.size, 64), snx_ld_le64(h + 24)};
    if (!bounded(r, s->cert) || !bounded(r, s->ticket) || !bounded(r, s->tmd) || !bounded(r, content))
        return false;
    s->meta = (CtrSpan){snx_align_up(content.offset + content.size, 64), snx_ld_le32(h + 20)};
    if (s->meta.size && (!bounded(r, s->meta) || s->meta.size < 0x400))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid CIA metadata size");
    if (s->tmd.size > CTR_METADATA_LIMIT || s->ticket.size > CTR_METADATA_LIMIT)
        return ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "CIA metadata exceeds 16 MiB limit");
    unsigned char sig[4];
    if (!ctr_read(r, s->ticket, 0, sig, sizeof sig)) return false;
    size_t ticket_body = ctr_signed_body(snx_ld_be32(sig));
    if (!ticket_body || !ctr_fits(s->ticket.size, ticket_body, 0x164))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid ticket signature/body size");
    unsigned char ticket_id[8], rights[8];
    if (!ctr_read(r, s->ticket, ticket_body + 0x9c, ticket_id, sizeof ticket_id) ||
        !ctr_read(r, s->ticket, ticket_body + 0x164, rights, sizeof rights))
        return false;
    uint32_t rights_size = snx_ld_be32(rights + 4);
    if (rights_size < 8 || !ctr_fits(s->ticket.size, ticket_body + 0x164, rights_size))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid ticket content-index size");
    s->metadata = malloc((size_t)s->tmd.size);
    if (!s->metadata) return ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "out of memory");
    if (!ctr_read(r, s->tmd, 0, s->metadata, (size_t)s->tmd.size)) return false;
    if (s->tmd.size < 4 || !(s->body = ctr_signed_body(snx_ld_be32(s->metadata))) ||
        !ctr_fits(s->tmd.size, s->body, 0x9c4))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid TMD signature/body size");
    const unsigned char* body = s->metadata + s->body;
    s->title_id = snx_ld_be64(body + 0x4c);
    s->version = snx_ld_be16(body + 0x9c);
    s->chunks = snx_ld_be16(body + 0x9e);
    uint16_t boot = snx_ld_be16(body + 0xa0);
    if (s->title_id != snx_ld_be64(ticket_id))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "ticket/TMD title ID mismatch");
    if ((s->title_id >> 32) != 0x00040000)
        return ctr_fail(r, SNX_CTR_EXTRACT_UNSUPPORTED,
                        "only application CIAs are supported; not updates/DLC/system titles");
    if (!s->chunks || !ctr_fits(s->tmd.size, s->body + 0x9c4, s->chunks * 0x30))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid TMD content count");
    const unsigned char* chunks = body + 0x9c4;
    unsigned char hash[32], covered[65536] = {0}, indices[8192] = {0}, present[8192] = {0};
    if (!ctr_sha256(r, body + 0xc4, 64 * 0x24, hash)) return false;
    if (memcmp(hash, body + 0xa4, 32)) return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "TMD content-info SHA-256 mismatch");
    for (size_t i = 0; i < 64; ++i) {
        const unsigned char* info = body + 0xc4 + i * 0x24;
        size_t first = snx_ld_be16(info), count = snx_ld_be16(info + 2);
        if (!count) continue;
        if (first >= s->chunks || count > s->chunks - first)
            return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "TMD content-info range exceeds chunk table");
        for (size_t j = first; j < first + count; ++j) {
            if (covered[j]) return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "overlapping TMD content-info ranges");
            covered[j] = 1;
        }
        if (!ctr_sha256(r, chunks + first * 0x30, count * 0x30, hash)) return false;
        if (memcmp(hash, info + 4, 32)) return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "TMD chunk-table SHA-256 mismatch");
    }
    uint32_t* ids = malloc(s->chunks * sizeof *ids);
    if (!ids) return ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "out of memory");
    bool ok = true, have_boot = false;
    uint64_t at = 0;
    for (size_t i = 0; i < s->chunks && ok; ++i) {
        const unsigned char* chunk = chunks + i * 0x30;
        unsigned index = snx_ld_be16(chunk + 4), bit = 0x80u >> (index & 7);
        ids[i] = snx_ld_be32(chunk);
        if (!covered[i] || (indices[index >> 3] & bit)) {
            ok = ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "uncovered or duplicate TMD content index");
            break;
        }
        indices[index >> 3] |= (unsigned char)bit;
        if (!(h[0x20 + (index >> 3)] & bit)) continue;
        present[index >> 3] |= (unsigned char)bit;
        uint64_t size = snx_ld_be64(chunk + 8);
        if (!size || !ctr_fits(content.size, at, size)) {
            ok = ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid CIA content length");
            break;
        }
        if (snx_ld_be16(chunk + 6) & 1) {
            ok = ctr_fail(r, SNX_CTR_EXTRACT_UNSUPPORTED, "encrypted CIA content is not supported");
            break;
        }
        if (index == boot) {
            s->boot = (CtrSpan){content.offset + at, size};
            have_boot = true;
        }
        at += size;
    }
    if (ok) {
        qsort(ids, s->chunks, sizeof *ids, compare_ids);
        for (size_t i = 1; i < s->chunks; ++i)
            if (ids[i] == ids[i - 1]) {
                ok = ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "duplicate TMD content ID");
                break;
            }
    }
    free(ids);
    if (!ok) return false;
    if (!have_boot || at != content.size || memcmp(present, h + 0x20, sizeof present))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "CIA bitmap/content sizes or boot content do not match TMD");
    return r->quick || (s->input_hashed = ctr_hash(r, (CtrSpan){0, r->size}, s->input_hash));
}
bool ctr_cia_contents(CtrReader* r, const CtrSource* s, const char* stage) {
    char* path = stage ? snx_path_join(stage, "contents") : NULL;
    bool ok = !stage || ctr_directory(r, path);
    free(path);
    uint64_t at = snx_align_up(s->tmd.offset + s->tmd.size, 64);
    for (size_t i = 0; i < s->chunks && ok; ++i) {
        const unsigned char* chunk = s->metadata + s->body + 0x9c4 + i * 0x30;
        unsigned index = snx_ld_be16(chunk + 4);
        if (!(s->header[0x20 + (index >> 3)] & (0x80u >> (index & 7)))) continue;
        CtrSpan span = {at, snx_ld_be64(chunk + 8)};
        if (!stage) { /* the content's hash, which the copy checks; none in a quick read */
            unsigned char hash[32];
            if (!r->quick)
                ok = ctr_hash(r, span, hash) &&
                     (!memcmp(hash, chunk + 16, 32) || ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "content SHA-256 mismatch"));
            at += span.size;
            continue;
        }
        char* name = snx_format("contents/%08" PRIx32 ".app", snx_ld_be32(chunk));
        path = snx_path_join(stage, name);
        ok = ctr_copy(r, span, path, chunk + 16);
        free(name);
        free(path);
        at += span.size;
    }
    if (!stage) return ok;
    path = snx_path_join(stage, "cia-header.bin");
    ok = ok && ctr_write(r, path, s->header, 0x2020);
    free(path);
    const CtrSpan spans[] = {s->cert, s->ticket, s->tmd, s->meta};
    const char* names[] = {"certificates.bin", "ticket.bin", "tmd.bin", "metadata.bin"};
    for (size_t i = 0; i < 4 && ok; ++i) {
        if (!spans[i].size) continue;
        path = snx_path_join(stage, names[i]);
        ok = ctr_copy(r, spans[i], path, NULL);
        free(path);
    }
    return ok;
}
void ctr_cia_free(CtrSource* s) {
    free(s->header);
    free(s->metadata);
}
