/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A game's DLC (tests/3ds_dlc.py): the game's CIA with a DLC CIA of its own add-on title laid beside it, read where
   they lie. Usage: probe GAME DLC OTHER_GAME_DLC. The DLC is refused when it is another game's or given twice, the
   NCCH archive serves a content's RomFS by its TMD index, and am:app lists the contents and the title */
#include "3ds/loader/ctr_loader.h"
#include "3ds/runtime/internal.h"
#include "3ds/ipc/ctr_ipc.h"
#include "support/3ds_guest.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DLC_TITLE 0x0004008c01234500ull
static uint32_t endpoint(SnxCtrProcess* p, uint32_t srv, const char* name) {
    uint32_t words[5] = {0x50100, 0, 0, (uint32_t)strlen(name), 1};
    for (unsigned i = 0; name[i]; ++i) words[1 + i / 4] |= (uint32_t)(unsigned char)name[i] << ((i & 3) * 8);
    ctr_request(p, words, 5);
    ctr_send(p, srv, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x50042 && !ctr_command_word(p, 1));
    return ctr_command_word(p, 3);
}
/* the NCCH archive's RomFS file of the DLC title at content index; the file handle, or 0 with the result in *result */
static uint32_t open_content(SnxCtrProcess* p, uint32_t fs, uint64_t title, uint32_t index, uint32_t section,
                             uint32_t* result) {
    ctr_put(p, 0x300020, (uint32_t)title);
    ctr_put(p, 0x300024, (uint32_t)(title >> 32));
    ctr_put(p, 0x300028, 1); /* the SD card */
    ctr_put(p, 0x30002c, 0);
    ctr_put(p, 0x300040, 0);
    ctr_put(p, 0x300044, index);
    ctr_put(p, 0x300048, section);
    ctr_put(p, 0x30004c, 0);
    ctr_put(p, 0x300050, 0);
    uint32_t open[] = {0x8030204, 0, 0x2345678a, 2, 16, 2, 0x14, 1, 0, (16u << 14) | 0x802, 0x300020,
                       (0x14u << 14) | 2, 0x300040};
    ctr_call(p, fs, open, 13, SNX_CTR_SVC_RETURN, 0);
    *result = ctr_command_word(p, 1);
    return *result ? 0 : ctr_command_word(p, 3);
}
/* whether a file's bytes (its whole RomFS level 3) hold the UTF-16LE name */
static bool holds(SnxCtrProcess* p, uint32_t file, const char* name) {
    uint32_t size_request[] = {0x8040000};
    ctr_call(p, file, size_request, 1, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    uint32_t size = ctr_command_word(p, 2);
    assert(size && size <= 0x1000);
    uint32_t read[] = {0x80200c2, 0, 0, size, (size << 4) | 12, 0x301000};
    ctr_call(p, file, read, 6, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1) && ctr_command_word(p, 2) == size);
    unsigned char* image = malloc(size);
    assert(image && snx_ctr_read_ram(p->space, 0x301000, image, size) == SNX_CTR_MEM_OK);
    size_t n = strlen(name);
    bool found = false;
    for (uint32_t at = 0; !found && at + n * 2 <= size; ++at) {
        size_t i = 0;
        while (i < n && image[at + i * 2] == (unsigned char)name[i] && !image[at + i * 2 + 1]) ++i;
        found = i == n;
    }
    free(image);
    return found;
}
int main(int argc, char** argv) {
    if (argc != 4) return 2;
    char error[512];
    SnxCtrSource* game = NULL;
    assert(snx_ctr_source_open(argv[1], &game, error, sizeof error) == SNX_CTR_EXTRACT_OK);
    /* the DLC of another game, and a game given as a DLC, are refused */
    assert(snx_ctr_source_add_dlc(game, argv[3], error, sizeof error) == SNX_CTR_EXTRACT_FORMAT && strstr(error, "not of"));
    assert(snx_ctr_source_add_dlc(game, argv[1], error, sizeof error) != SNX_CTR_EXTRACT_OK);
    assert(snx_ctr_source_dlc_count(game) == 0);
    assert(snx_ctr_source_add_dlc(game, argv[2], error, sizeof error) == SNX_CTR_EXTRACT_OK);
    assert(snx_ctr_source_add_dlc(game, argv[2], error, sizeof error) == SNX_CTR_EXTRACT_FORMAT &&
           strstr(error, "twice"));
    assert(snx_ctr_source_dlc_count(game) == 1);
    SnxCtrDlcTitle title;
    assert(snx_ctr_source_dlc_title(game, 0, &title) && title.title == DLC_TITLE && title.count == 2 && title.version == 1);
    SnxCtrDlcContent content;
    assert(snx_ctr_source_dlc_content(game, DLC_TITLE, 0, &content) && content.index == 0 && content.id == 0x2222);
    assert(snx_ctr_source_dlc_content(game, DLC_TITLE, 1, &content) && content.index == 3 && content.id == 0x3333);
    assert(!snx_ctr_source_dlc_content(game, DLC_TITLE, 2, &content));
    /* the game still reads as itself, not as the DLC */
    assert(snx_ctr_source_title(game) == 0x0004000001234500ull);

    unsigned char code[0x3000] = {0};
    SnxCtrImage image = ctr_image(code, sizeof code, 0x3000);
    SnxCtrProcess* p = snx_ctr_process_new(&image, SNX_CTR_OLD);
    assert(p);
    p->source = game;
    uint32_t fs;
    assert(!snx_ctr_session_create(p, SNX_CTR_SERVICE_FS, &fs));
    uint32_t init[] = {0x8010002, 0x20, 0xffffffff};
    ctr_call(p, fs, init, 3, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    uint32_t result;
    uint32_t zero = open_content(p, fs, DLC_TITLE, 0, 0, &result);
    assert(zero && !result && holds(p, zero, "first.txt") && !holds(p, zero, "second.txt"));
    uint32_t three = open_content(p, fs, DLC_TITLE, 3, 0, &result);
    assert(three && !result && holds(p, three, "second.txt") && !holds(p, three, "first.txt"));
    /* both open at once, each reading its own image, and a second open of the same content shares the view */
    assert(holds(p, zero, "first.txt"));
    uint32_t again = open_content(p, fs, DLC_TITLE, 0, 0, &result);
    assert(again && holds(p, again, "first.txt") && p->dlc_view_count == 2);
    assert(!open_content(p, fs, DLC_TITLE, 1, 0, &result) && result == 0xc8804478); /* no such content */
    assert(!snx_ctr_handle_close(p, zero) && !snx_ctr_handle_close(p, three) && !snx_ctr_handle_close(p, again));

    uint32_t srv;
    assert(!snx_ctr_session_create(p, SNX_CTR_SERVICE_SRV, &srv));
    uint32_t reg[] = {0x10002, 0x20, 0xffffffff};
    ctr_request(p, reg, 3);
    ctr_send(p, srv, SNX_CTR_SVC_RETURN, 0);
    uint32_t am = endpoint(p, srv, "am:app");
    uint32_t count[] = {0x100100c0, 1, (uint32_t)DLC_TITLE, (uint32_t)(DLC_TITLE >> 32)};
    ctr_request(p, count, 4);
    ctr_send(p, am, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x10010080 && !ctr_command_word(p, 1) && ctr_command_word(p, 2) == 2);
    uint32_t list[] = {0x10030142, 8, 1, (uint32_t)DLC_TITLE, (uint32_t)(DLC_TITLE >> 32), 0, (8u * 24 << 4) | 12,
                       0x302000};
    ctr_request(p, list, 8);
    ctr_send(p, am, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x10030082);
    assert(!ctr_command_word(p, 1) && ctr_command_word(p, 2) == 2);
    unsigned char entries[48];
    assert(snx_ctr_read_ram(p->space, 0x302000, entries, sizeof entries) == SNX_CTR_MEM_OK);
    assert(entries[0] == 0 && entries[1] == 0 && entries[4] == 0x22 && entries[5] == 0x22 && entries[16] == 3);
    assert(entries[24] == 3 && entries[28] == 0x33 && entries[29] == 0x33);
    uint32_t start[] = {0x10030142, 8, 1, (uint32_t)DLC_TITLE, (uint32_t)(DLC_TITLE >> 32), 1, (8u * 24 << 4) | 12,
                        0x302200};
    ctr_request(p, start, 8);
    ctr_send(p, am, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1) && ctr_command_word(p, 2) == 1); /* from the second */
    /* the title info: its ID, the sizes added up, its version */
    ctr_put(p, 0x302400, (uint32_t)DLC_TITLE);
    ctr_put(p, 0x302404, (uint32_t)(DLC_TITLE >> 32));
    uint32_t infos[] = {0x10050084, 1, 1, (8u << 4) | 10, 0x302400, (24u << 4) | 12, 0x302500};
    ctr_request(p, infos, 7);
    ctr_send(p, am, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    unsigned char info[24];
    assert(snx_ctr_read_ram(p->space, 0x302500, info, sizeof info) == SNX_CTR_MEM_OK);
    assert(info[0] == 0x00 && info[1] == 0x45 && info[2] == 0x23 && info[3] == 0x01 && info[4] == 0x8c && info[5] == 0);
    assert(info[16] == 1 && info[17] == 0);
    uint64_t size = 0;
    for (unsigned i = 0; i < 8; ++i) size |= (uint64_t)info[8 + i] << (8 * i);
    assert(size == title.size && size);
    snx_ctr_process_free(p);
    snx_ctr_source_close(game);
    puts("DLC contents served by the NCCH archive and am:app");
    return 0;
}
