/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project
   Copyright 2016 Citra Emulator Project
   APT's shared font, the block GetSharedFont passes to the application. Ported from Azahar (GPL-2.0-or-later) at
   azahar-emu/azahar a4b335e: core/hle/service/apt/apt.cpp (LoadSharedFont, DecompressLZ11, and the relocation address
   in GetSharedFont) and core/hle/service/apt/bcfnt/bcfnt.cpp (RelocateSharedFont). The font is the shared font
   system title's stand-in (cmake/ctr_system_archives.cmake), a RomFS holding cbf_std.bcfnt.lz: LZ11-compressed CFNT.
   The block is an 0x80-byte header (status, region, decompressed size) and then the font, its magic changed CFNT ->
   CFNU and its CFNT pointers relocated to the block's address. Only the standard font has a stand-in, so the region
   is always JPN's, where Azahar picks the font by the CFG region. */
#include "3ds/services/font.h"
#include "3ds/runtime/internal.h"
#include "3ds/services/archives.h"
#include "3ds/services/romfs.h"
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"
#include <stdlib.h>
#include <string.h>

#define FONT_FILE "cbf_std.bcfnt.lz"

enum {
    BLOCK_STATUS = 0,
    BLOCK_REGION = 4,
    BLOCK_SIZE = 8,
    BLOCK_FONT = 0x80, /* the CFNT starts here */
    STATUS_LOADED = 2,
    REGION_JPN = 1,
    LZ11_TAG = 0x11,
    /* the CFNT header: its own size (u16) and its section count (u32) */
    CFNT_HEADER_SIZE = 6,
    CFNT_SECTIONS = 0x10,
    CFNT_MIN_SIZE = 0x14,
    /* a section: its magic, its size (u32, the 8-byte header included), then its payload */
    SECTION_SIZE = 4,
    SECTION_PAYLOAD = 8,
    /* FINF's pointers name payloads, 8 bytes past the start of the section that each one lies in */
    FINF_TGLP = 0x10,
    FINF_CWDH = 0x14,
    FINF_CMAP = 0x18,
    FINF_SIZE = 0x1c,
    /* a CMAP or CWDH link names the payload of the next section of its chain, or is 0 at the end */
    CMAP_NEXT = 0x10,
    CMAP_SIZE = 0x14,
    CWDH_NEXT = 0x0c,
    CWDH_SIZE = 0x10,
    TGLP_SHEETS = 0x1c,
    TGLP_SIZE = 0x20
};

/* LZ11, as the 3DS carries it: 0x11, the decompressed size in three bytes, then flag bytes whose bits take a literal
   (0) or a back reference (1) with 2, 3 or 4 bytes of length and distance. It fills `want` bytes of `out`. */
static bool lz11(const unsigned char* src, uint32_t size, unsigned char* out, uint32_t want) {
    uint32_t at = 4, used = 0;
    while (used < want) {
        if (at >= size) return false;
        uint32_t flags = src[at++];
        for (uint32_t mask = 0x80; mask && used < want; mask >>= 1) {
            if (at >= size) return false;
            if (!(flags & mask)) {
                out[used++] = src[at++];
                continue;
            }
            uint32_t lead = src[at++], kind = lead >> 4, length, distance;
            if (size - at < (kind == 0 ? 2u : kind == 1 ? 3u : 1u)) return false;
            if (kind == 0) {
                length = ((lead & 15u) << 4 | src[at] >> 4) + 0x11;
                distance = ((src[at] & 15u) << 8 | src[at + 1]) + 1;
                at += 2;
            } else if (kind == 1) {
                length = ((lead & 15u) << 12 | (uint32_t)src[at] << 4 | src[at + 1] >> 4) + 0x111;
                distance = ((src[at + 1] & 15u) << 8 | src[at + 2]) + 1;
                at += 3;
            } else {
                length = kind + 1;
                distance = ((lead & 15u) << 8 | src[at]) + 1;
                at += 1;
            }
            if (distance > used || length > want - used) return false;
            for (uint32_t i = 0; i < length; ++i) out[used + i] = out[used - distance + i];
            used += length;
        }
    }
    return true;
}
static void move_pointer(unsigned char* field, uint32_t delta) { snx_st_le32(field, snx_ld_le32(field) + delta); }
/* The first pass of Azahar's RelocateSharedFont, over the `size` bytes of block that hold the font: it finds the base
   the file was built for from the sections' real offsets, and checks each section against the block, so a corrupt font
   fails here and relocate never reads or writes outside it. */
static bool scan(const unsigned char* block, uint32_t size, uint32_t* base) {
    if (size < BLOCK_FONT + CFNT_MIN_SIZE || memcmp(block + BLOCK_FONT, "CFNT", 4)) return false;
    const unsigned char* cfnt = block + BLOCK_FONT;
    uint32_t at = BLOCK_FONT + snx_ld_le16(cfnt + CFNT_HEADER_SIZE), blocks = snx_ld_le32(cfnt + CFNT_SECTIONS);
    uint32_t first_cmap = 0, first_cwdh = 0, first_tglp = 0, finf_cmap = 0, finf_cwdh = 0, finf_tglp = 0;
    for (uint32_t i = 0; i < blocks; ++i) {
        if (at > size || size - at < SECTION_PAYLOAD) return false;
        const unsigned char* section = block + at;
        uint32_t bytes = snx_ld_le32(section + SECTION_SIZE);
        if (bytes < SECTION_PAYLOAD || bytes > size - at) return false;
        if (!memcmp(section, "FINF", 4)) {
            if (bytes < FINF_SIZE) return false;
            finf_tglp = snx_ld_le32(section + FINF_TGLP) - SECTION_PAYLOAD;
            finf_cwdh = snx_ld_le32(section + FINF_CWDH) - SECTION_PAYLOAD;
            finf_cmap = snx_ld_le32(section + FINF_CMAP) - SECTION_PAYLOAD;
        } else if (!memcmp(section, "CMAP", 4)) {
            if (bytes < CMAP_SIZE) return false;
            if (!first_cmap) first_cmap = at;
        } else if (!memcmp(section, "CWDH", 4)) {
            if (bytes < CWDH_SIZE) return false;
            if (!first_cwdh) first_cwdh = at;
        } else if (!memcmp(section, "TGLP", 4)) {
            if (bytes < TGLP_SIZE) return false;
            if (!first_tglp) first_tglp = at;
        }
        at += bytes;
    }
    *base = finf_cmap - first_cmap;
    return first_cmap && first_cwdh && first_tglp && *base == finf_cwdh - first_cwdh && *base == finf_tglp - first_tglp;
}
/* The second pass: every pointer (FINF's three, the CMAP and CWDH next links when set, TGLP's sheet data) moves by
   `delta`, the new address minus the base scan found. */
static void relocate(unsigned char* block, uint32_t delta) {
    const unsigned char* cfnt = block + BLOCK_FONT;
    uint32_t at = BLOCK_FONT + snx_ld_le16(cfnt + CFNT_HEADER_SIZE), blocks = snx_ld_le32(cfnt + CFNT_SECTIONS);
    for (uint32_t i = 0; i < blocks; ++i) {
        unsigned char* section = block + at;
        if (!memcmp(section, "FINF", 4)) {
            move_pointer(section + FINF_TGLP, delta);
            move_pointer(section + FINF_CWDH, delta);
            move_pointer(section + FINF_CMAP, delta);
        } else if (!memcmp(section, "CMAP", 4)) {
            if (snx_ld_le32(section + CMAP_NEXT)) move_pointer(section + CMAP_NEXT, delta);
        } else if (!memcmp(section, "CWDH", 4)) {
            if (snx_ld_le32(section + CWDH_NEXT)) move_pointer(section + CWDH_NEXT, delta);
        } else if (!memcmp(section, "TGLP", 4)) {
            move_pointer(section + TGLP_SHEETS, delta);
        }
        at += snx_ld_le32(section + SECTION_SIZE);
    }
}
/* The first GetSharedFont: the stand-in decompressed straight into the block, relocated for the address the block is
   mapped at, and written to its pages. That address is the linear heap address of the pages, as Azahar relocates the
   font (apt.cpp, GetSharedFont). The pages are handed out in order, so the address depends on what was reserved before
   this call: with only the main thread's TLS page reserved, the block lies at 0x18001000 on an Old 3DS and at
   0x1bc01000 on a New 3DS. The stand-in's block is 0x2fe000 bytes, so it ends below 0x1c000000 on both. The steps that
   can fail come before the pages are reserved. */
static bool load(SnxCtrProcess* p, uint32_t* handle) {
    uint64_t at = 0, bytes = 0;
    if (!snx_ctr_romfs_file(snx_ctr_shared_font, snx_ctr_shared_font_size, FONT_FILE, &at, &bytes) || bytes < 4 ||
        bytes > UINT32_MAX || snx_ctr_shared_font[at] != LZ11_TAG)
        return false;
    const unsigned char* file = snx_ctr_shared_font + at;
    uint32_t file_size = (uint32_t)bytes;
    uint32_t font_size = snx_ld_le32(file) >> 8, used = BLOCK_FONT + font_size;
    uint32_t size = (uint32_t)snx_align_up(used, 4096);
    unsigned char* block = calloc(1, size);
    if (!block) return false;
    uint32_t base = 0, served = 0;
    if (!lz11(file, file_size, block + BLOCK_FONT, font_size) || !scan(block, used, &base) ||
        snx_ctr_object_create(p, CTR_OBJ_SHARED, &served)) {
        free(block);
        return false;
    }
    uint32_t physical = snx_ctr_shared_pages(p, size);
    if (!physical) {
        free(block);
        (void)snx_ctr_handle_close(p, served);
        return false;
    }
    relocate(block, snx_ctr_linear_address(physical) - base);
    memcpy(block + BLOCK_FONT, "CFNU", 4); /* CFNT -> CFNU, as APT serves it */
    snx_st_le32(block + BLOCK_STATUS, STATUS_LOADED);
    snx_st_le32(block + BLOCK_REGION, REGION_JPN);
    snx_st_le32(block + BLOCK_SIZE, font_size);
    /* the range lies in FCRAM, which snx_ctr_shared_pages checked, and device() accepts a range inside one region */
    (void)snx_ctr_phys_write(p->memory, physical, block, size);
    free(block);
    uint32_t id = snx_ctr_handle_object(p, served);
    CtrObject* object = &p->objects[id];
    object->data.shared.physical = physical;
    object->data.shared.size = size;
    object->data.shared.permissions = SNX_CTR_READ; /* GetSharedFont is called to read it */
    ++object->refs;                                 /* APT keeps the block while the process lives */
    p->apt_font = id;
    *handle = served;
    return true;
}
bool snx_ctr_font_shared(SnxCtrProcess* p, uint32_t* address, uint32_t* handle) {
    *address = 0;
    *handle = 0;
    if (!p->apt_font) {
        if (!load(p, handle)) return false;
    } else if (snx_ctr_handle_add(p, p->apt_font, handle)) {
        return false;
    }
    *address = snx_ctr_linear_address(p->objects[p->apt_font].data.shared.physical);
    return true;
}
