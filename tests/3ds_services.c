/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/runtime/internal.h"
#include "3ds/ipc/ctr_ipc.h"
#include "common/utilities/endian.h"
#include "common/utilities/users.h"
#include "support/3ds_guest.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t endpoint(SnxCtrProcess* p, uint32_t srv, const char* name) {
    uint32_t words[5] = {0x50100, 0, 0, (uint32_t)strlen(name), 1};
    for (unsigned i = 0; name[i]; ++i) words[1 + i / 4] |= (uint32_t)(unsigned char)name[i] << ((i & 3) * 8);
    ctr_request(p, words, 5);
    ctr_send(p, srv, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x50042 && !ctr_command_word(p, 1) && ctr_command_word(p, 2) == 0x10);
    return ctr_command_word(p, 3);
}
static void static_output(SnxCtrProcess* p, unsigned size, uint32_t address) {
    ctr_put(p, CTR_COMMAND + 256, (size << 14) | 2);
    ctr_put(p, CTR_COMMAND + 260, address);
}
#define FONT_LINEAR_END (CTR_LINEAR + 0x08000000u) /* the old linear heap ends here */
#define FONT_ALIAS      0x1d000000u
enum {
    BLOCK_STATUS = 0,
    BLOCK_REGION = 4,
    BLOCK_SIZE = 8,
    BLOCK_FONT = 0x80,
    CFNT_FILE_SIZE = 0xc,
    CFNT_HEADER_SIZE = 6,
    CFNT_SECTIONS = 0x10,
    SECTION_SIZE = 4,
    SECTION_PAYLOAD = 8,
    FINF_TGLP = 0x10,
    FINF_CWDH = 0x14,
    FINF_CMAP = 0x18,
    CMAP_NEXT = 0x10,
    CWDH_NEXT = 0x0c,
    TGLP_SHEET_SIZE = 0x0c,
    TGLP_SHEET_COUNT = 0x10,
    TGLP_SHEETS = 0x1c,
    TGLP_HEADER = 0x20,
    STATUS_LOADED = 2,
    REGION_JPN = 1,
    GET_SHARED_FONT_REQUEST = 0x440000,
    GET_SHARED_FONT_REPLY = 0x440082,
    CHAIN_MAX = 8
};
static void get_shared_font(SnxCtrProcess* p, uint32_t apt) {
    uint32_t command[] = {GET_SHARED_FONT_REQUEST};
    ctr_request(p, command, 1);
    ctr_send(p, apt, SNX_CTR_SVC_RETURN, 0);
}
static void map_block(SnxCtrProcess* p, uint32_t handle, uint32_t address, unsigned permissions) {
    uint32_t* r = p->cpu.state.cpu.r;
    r[0] = handle;
    r[1] = address;
    r[2] = permissions;
    r[3] = 0;
    assert(snx_ctr_svc(p, 0x1f) == SNX_CTR_SVC_RETURN && !r[0]);
}
static void unmap_block(SnxCtrProcess* p, uint32_t handle, uint32_t address) {
    uint32_t* r = p->cpu.state.cpu.r;
    r[0] = handle;
    r[1] = address;
    assert(snx_ctr_svc(p, 0x20) == SNX_CTR_SVC_RETURN && !r[0]);
}
/* A service block with pages maps at address 0 at the linear heap address of those pages, as Azahar's SharedMemory::Map
   maps each block a service makes. The mapping is then removed. */
static void check_linear_block(SnxCtrProcess* p, uint32_t handle, unsigned permissions) {
    uint32_t physical = p->objects[snx_ctr_handle_object(p, handle)].data.shared.physical;
    uint32_t linear = CTR_LINEAR + (physical - CTR_PHYS_BASE), mapped_pa = 0;
    unsigned mapped_permissions = 0;
    map_block(p, handle, 0, permissions);
    assert(snx_ctr_query(p->space, linear, &mapped_pa, &mapped_permissions) == SNX_CTR_MEM_OK &&
           mapped_pa == physical && mapped_permissions == permissions);
    unmap_block(p, handle, linear);
}
/* Each link of a CMAP or CWDH chain is 0 or names another section of the chain, and one link ends it */
static void check_chain(const unsigned char* cfnt, const uint32_t* sections, uint32_t count, uint32_t link_at,
                        uint32_t base) {
    uint32_t ends = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t link = snx_ld_le32(cfnt + sections[i] + link_at);
        bool known = !link;
        ends += !link;
        for (uint32_t j = 0; j < count; ++j) known = known || link == base + sections[j] + SECTION_PAYLOAD;
        assert(known);
    }
    assert(ends == 1);
}
/* GetSharedFont (0x44) serves the stand-in's block and a handle to it. The block lies at the linear heap address of
   its pages, which come after the application pool, and the SDK maps it by passing address 0. A first call that cannot
   make the block reserves nothing. A later call serves another handle to the same block, which APT keeps. */
static void check_shared_font(SnxCtrProcess* p, uint32_t srv, uint32_t apt) {
    uint32_t held[CTR_HANDLES], held_count = 0;
    while (!snx_ctr_handle_add(p, snx_ctr_handle_object(p, srv), &held[held_count])) ++held_count;
    uint32_t shared_before = p->physical_shared;
    unsigned objects_before = ctr_object_count(p);
    get_shared_font(p, apt);
    assert(ctr_command_word(p, 1) == 0xffffffffu && p->physical_shared == shared_before &&
           ctr_object_count(p) == objects_before);
    for (uint32_t i = 0; i < held_count; ++i) assert(!snx_ctr_handle_close(p, held[i]));
    get_shared_font(p, apt);
    assert(ctr_command_word(p, 0) == GET_SHARED_FONT_REPLY && !ctr_command_word(p, 1) && !ctr_command_word(p, 3));
    uint32_t font_address = ctr_command_word(p, 2), font_handle = ctr_command_word(p, 4);
    uint32_t font_id = snx_ctr_handle_object(p, font_handle);
    assert(font_address && font_id && p->objects[font_id].type == CTR_OBJ_SHARED);
    uint32_t font_physical = p->objects[font_id].data.shared.physical,
             font_total = p->objects[font_id].data.shared.size;
    assert(font_address == CTR_LINEAR + (font_physical - CTR_PHYS_BASE));
    assert(font_physical >= p->physical_end && font_address + font_total <= FONT_LINEAR_END);
    assert(p->objects[font_id].data.shared.permissions == SNX_CTR_READ);
    assert(p->objects[font_id].refs == 2); /* the served handle and APT's own */
    map_block(p, font_handle, 0, SNX_CTR_READ);
    assert(p->objects[font_id].refs == 3); /* the mapping takes its own */
    uint32_t font_pa = 0;
    unsigned font_permissions = 0;
    assert(snx_ctr_query(p->space, font_address, &font_pa, &font_permissions) == SNX_CTR_MEM_OK &&
           font_pa == font_physical && font_permissions == SNX_CTR_READ);
    unsigned char* font = malloc(font_total);
    assert(font && snx_ctr_read_ram(p->space, font_address, font, font_total) == SNX_CTR_MEM_OK);
    /* the header, then a CFNU whose sections fill the decompressed size exactly */
    const unsigned char* cfnt = font + BLOCK_FONT;
    uint32_t font_size = snx_ld_le32(font + BLOCK_SIZE), sections = snx_ld_le32(cfnt + CFNT_SECTIONS);
    assert(snx_ld_le32(font + BLOCK_STATUS) == STATUS_LOADED && snx_ld_le32(font + BLOCK_REGION) == REGION_JPN &&
           !memcmp(cfnt, "CFNU", 4));
    assert(font_size && BLOCK_FONT + font_size <= font_total && snx_ld_le32(cfnt + CFNT_FILE_SIZE) == font_size);
    uint32_t cmaps[CHAIN_MAX], cwdhs[CHAIN_MAX], cmap_count = 0, cwdh_count = 0, finf = 0, tglp = 0;
    uint32_t cursor = snx_ld_le16(cfnt + CFNT_HEADER_SIZE);
    for (uint32_t i = 0; i < sections; ++i) {
        assert(font_size - cursor >= SECTION_PAYLOAD);
        const unsigned char* section = cfnt + cursor;
        uint32_t bytes = snx_ld_le32(section + SECTION_SIZE);
        assert(bytes >= SECTION_PAYLOAD && bytes <= font_size - cursor);
        if (!memcmp(section, "FINF", 4)) {
            finf = cursor;
        } else if (!memcmp(section, "TGLP", 4) && !tglp) {
            tglp = cursor;
        } else if (!memcmp(section, "CMAP", 4)) {
            assert(cmap_count < CHAIN_MAX);
            cmaps[cmap_count++] = cursor;
        } else if (!memcmp(section, "CWDH", 4)) {
            assert(cwdh_count < CHAIN_MAX);
            cwdhs[cwdh_count++] = cursor;
        }
        cursor += bytes;
    }
    assert(cursor == font_size && finf && tglp && cmap_count && cwdh_count);
    /* every pointer names its section's payload at the address the block is mapped at, and the chains end */
    uint32_t base = font_address + BLOCK_FONT;
    assert(snx_ld_le32(cfnt + finf + FINF_TGLP) == base + tglp + SECTION_PAYLOAD);
    assert(snx_ld_le32(cfnt + finf + FINF_CWDH) == base + cwdhs[0] + SECTION_PAYLOAD);
    assert(snx_ld_le32(cfnt + finf + FINF_CMAP) == base + cmaps[0] + SECTION_PAYLOAD);
    check_chain(cfnt, cmaps, cmap_count, CMAP_NEXT, base);
    check_chain(cfnt, cwdhs, cwdh_count, CWDH_NEXT, base);
    uint32_t sheets_address = snx_ld_le32(cfnt + tglp + TGLP_SHEETS);
    assert(sheets_address >= base + tglp + TGLP_HEADER);
    uint64_t sheets_end = (uint64_t)(sheets_address - base) +
                          (uint64_t)snx_ld_le32(cfnt + tglp + TGLP_SHEET_SIZE) *
                              snx_ld_le16(cfnt + tglp + TGLP_SHEET_COUNT);
    assert(sheets_end <= font_size);
    free(font);
    get_shared_font(p, apt);
    assert(ctr_command_word(p, 0) == GET_SHARED_FONT_REPLY && !ctr_command_word(p, 1) &&
           ctr_command_word(p, 2) == font_address && !ctr_command_word(p, 3));
    uint32_t font_handle2 = ctr_command_word(p, 4);
    assert(font_handle2 && font_handle2 != font_handle && snx_ctr_handle_object(p, font_handle2) == font_id);
    assert(p->objects[font_id].refs == 4);
    map_block(p, font_handle2, FONT_ALIAS, SNX_CTR_READ);
    assert(p->objects[font_id].refs == 5);
    unsigned char alias[4];
    assert(snx_ctr_read_ram(p->space, FONT_ALIAS + BLOCK_FONT, alias, sizeof alias) == SNX_CTR_MEM_OK &&
           !memcmp(alias, "CFNU", 4));
    assert(snx_ctr_query(p->space, FONT_ALIAS, &font_pa, NULL) == SNX_CTR_MEM_OK && font_pa == font_physical);
    unmap_block(p, font_handle, font_address);
    assert(snx_ctr_query(p->space, font_address, NULL, NULL) == SNX_CTR_MEM_UNMAPPED);
    unmap_block(p, font_handle2, FONT_ALIAS);
    assert(snx_ctr_query(p->space, FONT_ALIAS, NULL, NULL) == SNX_CTR_MEM_UNMAPPED);
    assert(p->objects[font_id].refs == 3);
    assert(!snx_ctr_handle_close(p, font_handle) && !snx_ctr_handle_close(p, font_handle2));
    assert(p->objects[font_id].refs == 1); /* APT keeps the block */
    get_shared_font(p, apt);
    assert(ctr_command_word(p, 2) == font_address && snx_ctr_handle_object(p, ctr_command_word(p, 4)) == font_id);
    assert(!snx_ctr_handle_close(p, ctr_command_word(p, 4)) && p->objects[font_id].refs == 1);
}
int main(void) {
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = ctr_image(code, sizeof code, 4096);
    for (unsigned model = 0; model < 2; ++model) {
        SnxCtrProcess* p = snx_ctr_process_new(&image, (SnxCtrModel)model);
        assert(p);
        assert(snx_ctr_memory_profile(p->memory)->model == (SnxCtrModel)model);
        uint32_t srv;
        assert(!snx_ctr_session_create(p, SNX_CTR_SERVICE_SRV, &srv));
        uint32_t reg[] = {0x10002, 0x20, 0xffffffff};
        ctr_request(p, reg, 3);
        ctr_send(p, srv, SNX_CTR_SVC_RETURN, 0);
        uint32_t apt = endpoint(p, srv, "APT:S"), args[6] = {0x10040, 0};
        ctr_request(p, args, 2);
        ctr_send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 0) == 0x100c2 && !ctr_command_word(p, 1) && !ctr_command_word(p, 2) &&
               !ctr_command_word(p, 3) && !ctr_command_word(p, 4));
        uint32_t lock = ctr_command_word(p, 5), lock_id = snx_ctr_handle_object(p, lock);
        assert(lock_id == p->apt_lock && p->objects[lock_id].type == CTR_OBJ_MUTEX);
        assert(!snx_ctr_handle_close(p, apt));
        apt = endpoint(p, srv, "APT:S");
        ctr_request(p, args, 2);
        ctr_send(p, apt, SNX_CTR_SVC_RETURN, 0);
        uint32_t other_lock = ctr_command_word(p, 5);
        assert(other_lock != lock && snx_ctr_handle_object(p, other_lock) == lock_id);
        assert(!snx_ctr_handle_close(p, other_lock));
        uint32_t* r = p->cpu.state.cpu.r;
        for (unsigned i = 0; i < 2; ++i) {
            r[0] = lock;
            r[2] = r[3] = UINT32_MAX;
            assert(snx_ctr_svc(p, 0x24) == SNX_CTR_SVC_RETURN && !r[0]);
        }
        assert(p->objects[lock_id].data.mutex.depth == 2);
        for (unsigned i = 0; i < 2; ++i) {
            r[0] = lock;
            assert(snx_ctr_svc(p, 0x14) == SNX_CTR_SVC_RETURN && !r[0]);
        }
        r[0] = lock;
        assert(snx_ctr_svc(p, 0x14) == SNX_CTR_SVC_RETURN && r[0] == 0xd8e0041f);
        assert(!p->objects[lock_id].data.mutex.owner);
        uint32_t init[] = {0x20080, 0x300, 0};
        ctr_request(p, init, 3);
        assert(snx_ctr_protect(p->space, SNX_CTR_INITIAL_TLS, 4096, 1) == SNX_CTR_MEM_OK);
        ctr_send(p, apt, SNX_CTR_SVC_RETURN, 0xe0e01bf5);
        assert(!p->apt_phase);
        assert(snx_ctr_protect(p->space, SNX_CTR_INITIAL_TLS, 4096, 3) == SNX_CTR_MEM_OK);
        uint32_t duplicates[CTR_HANDLES], n = 0;
        while (n < CTR_HANDLES - 4) assert(!snx_ctr_handle_add(p, snx_ctr_handle_object(p, srv), &duplicates[n++]));
        unsigned before = ctr_object_count(p);
        ctr_request(p, init, 3);
        ctr_send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 1) == 0xd8600413 && !p->apt_phase && ctr_object_count(p) == before);
        for (unsigned i = 0; i < n; ++i) assert(!snx_ctr_handle_close(p, duplicates[i]));
        ctr_request(p, init, 3);
        ctr_send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 0) == 0x20043 && !ctr_command_word(p, 1) && ctr_command_word(p, 2) == 0x04000000);
        uint32_t notification = ctr_command_word(p, 3), parameter = ctr_command_word(p, 4);
        assert(snx_ctr_handle_object(p, notification) == p->apt_notification);
        assert(snx_ctr_handle_object(p, parameter) == p->apt_parameter);
        assert(p->apt_notification != p->apt_parameter && p->apt_phase == 1);
        ctr_request(p, init, 3);
        ctr_send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 1) == 0xc8a0cffc);
        uint32_t ready[] = {0x430040, 0x300};
        ctr_request(p, ready, 2);
        ctr_send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(p->apt_phase == 2);
        uint32_t receive[] = {0xd0080, 0x300, 256};
        static_output(p, 256, 0x301f80);
        unsigned char fill[128], unchanged[128];
        memset(fill, 0x55, sizeof fill);
        assert(snx_ctr_write_ram(p->space, 0x301f80, fill, sizeof fill) == SNX_CTR_MEM_OK);
        ctr_request(p, receive, 3);
        ctr_send(p, apt, SNX_CTR_SVC_RETURN, 0xe0e01bf5);
        assert(p->apt_phase == 2 && ctr_command_word(p, 0) == receive[0]);
        assert(snx_ctr_read_ram(p->space, 0x301f80, unchanged, 128) == SNX_CTR_MEM_OK && !memcmp(fill, unchanged, 128));
        static_output(p, 256, 0x300800);
        ctr_request(p, receive, 3);
        ctr_send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 0) == 0xd0104 && !ctr_command_word(p, 1) && !ctr_command_word(p, 2) &&
               ctr_command_word(p, 3) == 1 && !ctr_command_word(p, 4));
        assert(ctr_command_word(p, 5) == 0x10 && !ctr_command_word(p, 6) && ctr_command_word(p, 7) == 0x400002 &&
               ctr_command_word(p, 8) == 0x300800);
        assert(p->apt_phase == 3);
        uint32_t utility[] = {0x4b00c2, 7, 4, 1, 0x10402, 0x300700};
        ctr_put(p, 0x300700, 0x10);
        static_output(p, 1, 0x300710);
        ctr_request(p, utility, 6);
        ctr_send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(p->apt_phase == 4 && ctr_command_word(p, 0) == 0x4b0082);
        utility[1] = 4;
        utility[2] = 1;
        utility[4] = 0x4402;
        ctr_put(p, 0x300700, 0);
        p->shell_closed = 1;
        ctr_request(p, utility, 6);
        ctr_send(p, apt, SNX_CTR_SVC_UNSUPPORTED, 0);
        assert(ctr_command_word(p, 0) == utility[0]);
        p->shell_closed = 0;
        ctr_request(p, utility, 6);
        ctr_send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 0) == 0x4b0082);
        ctr_request(p, ready, 2);
        ctr_send(p, apt, SNX_CTR_SVC_UNSUPPORTED, 0);
        assert(p->apt_phase == 4);
        ctr_request(p, receive, 3);
        ctr_send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 1) == 0xc8a0cfef && p->apt_phase == 4);
        uint32_t query[] = {0x1020000};
        ctr_request(p, query, 1);
        ctr_send(p, apt, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 2) == model);
        check_shared_font(p, srv, apt);
        uint32_t ptm = endpoint(p, srv, "ptm:sysm");
        query[0] = 0x40a0000;
        ctr_request(p, query, 1);
        ctr_send(p, ptm, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 2) == model);
        uint32_t config[] = {0x8180040, 1};
        ctr_request(p, config, 2);
        ctr_send(p, ptm, SNX_CTR_SVC_UNSUPPORTED, 0);
        config[1] = 0;
        ctr_request(p, config, 2);
        ctr_send(p, ptm, SNX_CTR_SVC_RETURN, 0);
        assert(!ctr_command_word(p, 1));
        /* cfg: GetConfig of the sound output mode (libctru's ndsp reads it), stereo, into a writable mapped buffer.
           All three names are served, as Azahar does; another block stops */
        uint32_t cfg = endpoint(p, srv, "cfg:i");
        ctr_put(p, 0x300000, 0xffffffffu);
        uint32_t sound[] = {0x10082, 1, 0x70001, 1 << 4 | 0xc, 0x300000};
        ctr_request(p, sound, 5);
        ctr_send(p, cfg, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 0) == 0x10042 && !ctr_command_word(p, 1) &&
               ctr_command_word(p, 2) == (1 << 4 | 0xc) && ctr_command_word(p, 3) == 0x300000);
        unsigned char mode = 0;
        assert(snx_ctr_read_ram(p->space, 0x300000, &mode, 1) == SNX_CTR_MEM_OK && mode == 1);
        sound[1] = 4;
        /* Console names follow the selected ScarletNX profile and obey the
           3DS ten-character field without touching its trailing reserved words. */
        char old_nickname[sizeof g_users[0].nickname];
        memcpy(old_nickname, g_users[0].nickname, sizeof old_nickname);
        strcpy(g_users[0].nickname, "Mario_Test_Long");
        uint32_t username[] = {0x10082, 0x1c, 0xa0000, 0x1c << 4 | 0xc, 0x300000};
        ctr_request(p, username, 5);
        ctr_send(p, cfg, SNX_CTR_SVC_RETURN, 0);
        assert(!ctr_command_word(p, 1));
        unsigned char name_bytes[0x1c];
        assert(snx_ctr_read_ram(p->space, 0x300000, name_bytes, sizeof name_bytes) == SNX_CTR_MEM_OK);
        for (unsigned i = 0; i < 10; ++i)
            assert(name_bytes[i * 2] == (unsigned char)"Mario_Test"[i] && !name_bytes[i * 2 + 1]);
        for (unsigned i = 20; i < sizeof name_bytes; ++i) assert(!name_bytes[i]);
        memcpy(g_users[0].nickname, old_nickname, sizeof old_nickname);
        sound[3] = 4 << 4 | 0xc;
        ctr_request(p, sound, 5);
        ctr_send(p, cfg, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 1) == 0xd90103ec); /* InvalidSize */
        sound[1] = 1;
        sound[2] = 0x50005; /* the stereo camera settings: 32 bytes, not 1 */
        sound[3] = 1 << 4 | 0xc;
        ctr_request(p, sound, 5);
        ctr_send(p, cfg, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 1) == 0xd90103ec);
        sound[2] = 0x12345678; /* a block no console has */
        ctr_request(p, sound, 5);
        ctr_send(p, cfg, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 1) == 0xd90103fa); /* NotFound */
        assert(!snx_ctr_handle_close(p, cfg) && endpoint(p, srv, "cfg:u") && endpoint(p, srv, "cfg:s"));
        /* am:app on a console with no add-on content: one content info for a DLC title, none listed, a title ID that
           is no DLC's refused (Azahar am.cpp) */
        uint32_t am = endpoint(p, srv, "am:app");
        uint32_t dlc_count[] = {0x100100c0, 1, 0x00095000, 0x0004008c};
        ctr_request(p, dlc_count, 4);
        ctr_send(p, am, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 0) == 0x10010080 && !ctr_command_word(p, 1) && ctr_command_word(p, 2) == 1);
        dlc_count[3] = 0x00040000;
        ctr_request(p, dlc_count, 4);
        ctr_send(p, am, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 1) == 0xe0e0801f && !ctr_command_word(p, 2));
        uint32_t dlc_list[] = {0x10030142, 0, 1, 0x00095000, 0x0004008c, 0, 0xc, 0x300000};
        ctr_request(p, dlc_list, 8);
        ctr_send(p, am, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 0) == 0x10030080 && !ctr_command_word(p, 1) && !ctr_command_word(p, 2));
        assert(!snx_ctr_handle_close(p, am));
        uint32_t hid = endpoint(p, srv, "hid:USER"), hid_args[] = {0xa0000};
        n = 0;
        while (n < CTR_HANDLES && !snx_ctr_handle_add(p, snx_ctr_handle_object(p, srv), &duplicates[n])) ++n;
        assert(n > 5);
        for (unsigned i = 0; i < 5; ++i) assert(!snx_ctr_handle_close(p, duplicates[--n]));
        before = ctr_object_count(p);
        ctr_request(p, hid_args, 1);
        ctr_send(p, hid, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 1) == 0xd8600413 && !p->hid_objects[0] && ctr_object_count(p) == before);
        for (unsigned i = 0; i < n; ++i) assert(!snx_ctr_handle_close(p, duplicates[i]));
        ctr_request(p, hid_args, 1);
        ctr_send(p, hid, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 0) == 0xa0047 && ctr_command_word(p, 2) == 0x14000000);
        uint32_t shared = ctr_command_word(p, 3);
        for (unsigned i = 0; i < 6; ++i)
            assert(snx_ctr_handle_object(p, ctr_command_word(p, 3 + i)) == p->hid_objects[i]);
        r[0] = shared;
        r[1] = 0x10000000;
        r[2] = 1;
        r[3] = 0x10000000;
        assert(snx_ctr_svc(p, 0x1f) == SNX_CTR_SVC_RETURN && !r[0]);
        uint32_t physical;
        unsigned permissions;
        assert(snx_ctr_query(p->space, 0x10000000, &physical, &permissions) == SNX_CTR_MEM_OK &&
               physical >= p->physical_end && permissions == 1); /* past the application's pool */
        assert(snx_ctr_write_ram(p->space, 0x10000000, fill, 1) == SNX_CTR_MEM_PERMISSION);
        check_linear_block(p, shared, SNX_CTR_READ);
        for (unsigned i = 0; i < 6; ++i) {
            assert(!snx_ctr_handle_close(p, ctr_command_word(p, 3 + i)));
            /* the device retains its resources; the shared page mapped above is also kept by its mapping */
            assert(p->objects[p->hid_objects[i]].refs == (i == 0 ? 2u : 1u));
        }
        ctr_request(p, hid_args, 1);
        ctr_send(p, hid, SNX_CTR_SVC_RETURN, 0);
        for (unsigned i = 0; i < 6; ++i) {
            assert(snx_ctr_handle_object(p, ctr_command_word(p, 3 + i)) == p->hid_objects[i]);
            assert(!snx_ctr_handle_close(p, ctr_command_word(p, 3 + i)));
        }
        uint32_t fs = endpoint(p, srv, "fs:USER");
        reg[0] = 0x8010002;
        ctr_request(p, reg, 3);
        ctr_send(p, fs, SNX_CTR_SVC_RETURN, 0);
        uint32_t archive[] = {0x80c00c2, 9, 1, 1, 0x4002, 0x300700};
        ctr_request(p, archive, 6);
        ctr_send(p, fs, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 0) == 0x80c0040 &&
               ctr_command_word(p, 1) == 0xc8804478); /* genuinely no SD backing */
        archive[1] = 3;
        ctr_request(p, archive, 6);
        ctr_send(p, fs, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 0) == 0x80c0040 &&
               ctr_command_word(p, 1) == 0xc8804464); /* no configured RomFS backing */
        uint32_t gsp = endpoint(p, srv, "gsp::Gpu"), gsp_event = 0;
        assert(!snx_ctr_object_create(p, CTR_OBJ_EVENT, &gsp_event));
        uint32_t relay[] = {0x00130042, 1, 0, gsp_event};
        ctr_request(p, relay, 4);
        ctr_send(p, gsp, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 0) == 0x00130082 && ctr_command_word(p, 3) == 0x10);
        uint32_t gsp_block = ctr_command_word(p, 4);
        check_linear_block(p, gsp_block, SNX_CTR_READ | SNX_CTR_WRITE);
        assert(!snx_ctr_handle_close(p, gsp_block) && !snx_ctr_handle_close(p, gsp_event) &&
               !snx_ctr_handle_close(p, gsp));
        snx_ctr_process_free(p);
    }
    puts("APT startup, real lock/events, grouped handles, checked buffers, PTM/cfg/HID and absent-SD behavior passed");
    return 0;
}
