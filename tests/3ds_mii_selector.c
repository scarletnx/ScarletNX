/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Mii selector a 3DS game starts through APT to have the player pick a Mii (a game's StreetPass Mii, say), driven
   as the console's applet library drives it (libctru's aptLaunchLibraryApplet and miiSelectorLaunch copy its steps):
   PrepareToStartLibraryApplet(0x402), IsRegistered until the applet is there, a Request for the capture block and
   its Response, StartLibraryApplet with the 0x104-byte configuration and no object, NotifyToWait, then the
   WakeupByExit the game wakes with, which carries the MiiResult. The Mii in it is read field by field as 3dbrew's Mii
   page lays them out, and its checksum computed here on its own */
#include "3ds/runtime/internal.h"
#include "3ds/ipc/ctr_ipc.h"
#include "common/utilities/endian.h"
#include "snx_settings.h"
#include "support/3ds_guest.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#define CONFIG      0x300000u /* the game's MiiConfig */
#define CAPTURE     0x300400u /* its CaptureBufferInfo */
#define RECEIVED    0x301000u /* where Receive and Glance put a parameter's buffer */
#define RECEIVE_CAP 0x1000u   /* as libctru's aptParameters takes one */
#define APPLICATION 0x300u
#define SELECTOR    0x402u
#define MII         (RECEIVED + 0x0c) /* the MiiResult's Mii */

static uint32_t endpoint(SnxCtrProcess* p, uint32_t srv, const char* name) {
    uint32_t words[5] = {0x50100, 0, 0, (uint32_t)strlen(name), 1};
    for (unsigned i = 0; name[i]; ++i) words[1 + i / 4] |= (uint32_t)(unsigned char)name[i] << ((i & 3) * 8);
    ctr_request(p, words, 5);
    ctr_send(p, srv, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x50042 && !ctr_command_word(p, 1));
    return ctr_command_word(p, 3);
}
static void static_output(SnxCtrProcess* p, unsigned size, uint32_t address) {
    ctr_put(p, CTR_COMMAND + 256, (size << 14) | 2);
    ctr_put(p, CTR_COMMAND + 260, address);
}
static uint32_t call(SnxCtrProcess* p, uint32_t apt, const uint32_t* words, unsigned count) {
    ctr_call(p, apt, words, count, SNX_CTR_SVC_RETURN, 0);
    return ctr_command_word(p, 1);
}
/* the application's startup: Initialize, NotifyToWait, the initial Wakeup, the startup transition released */
static uint32_t started(SnxCtrProcess* p) {
    uint32_t srv;
    assert(!snx_ctr_session_create(p, SNX_CTR_SERVICE_SRV, &srv));
    uint32_t reg[] = {0x10002, 0x20, 0xffffffff};
    ctr_call(p, srv, reg, 3, SNX_CTR_SVC_RETURN, 0);
    uint32_t apt = endpoint(p, srv, "APT:U");
    uint32_t init[] = {0x20080, APPLICATION, 0}, ready[] = {0x430040, APPLICATION},
             receive[] = {0xd0080, APPLICATION, 0};
    uint32_t unlock[] = {0x4b00c2, 7, 4, 0, 0x10402, CAPTURE};
    assert(!call(p, apt, init, 3) && !call(p, apt, ready, 2));
    static_output(p, 0, RECEIVED);
    ctr_request(p, receive, 3);
    ctr_send(p, apt, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1) && ctr_command_word(p, 3) == 1);
    ctr_put(p, CAPTURE, 0x10);
    static_output(p, 0, RECEIVED);
    assert(!call(p, apt, unlock, 6) && p->apt_phase == 4);
    return apt;
}
static uint32_t registered(SnxCtrProcess* p, uint32_t apt, uint32_t id) {
    uint32_t words[] = {0x90040, id};
    assert(!call(p, apt, words, 2) && ctr_command_word(p, 0) == 0x90080);
    return ctr_command_word(p, 2);
}
/* Glance (0xe) or Receive (0xd): the reply's words 0 to 6 in out, the buffer at RECEIVED */
static void parameter(SnxCtrProcess* p, uint32_t apt, uint32_t command, uint32_t out[7]) {
    uint32_t words[] = {command << 16 | 0x80, APPLICATION, RECEIVE_CAP};
    static_output(p, RECEIVE_CAP, RECEIVED);
    ctr_call(p, apt, words, 3, SNX_CTR_SVC_RETURN, 0);
    for (unsigned i = 0; i < 7; ++i) out[i] = ctr_command_word(p, i);
}
static void send(SnxCtrProcess* p, uint32_t apt, uint32_t signal, uint32_t address, uint32_t size,
                 SnxCtrSvcResult state) {
    uint32_t words[] = {0xc0104, APPLICATION, SELECTOR, signal, size, 0, 0, (size << 14) | 2, address};
    ctr_call(p, apt, words, 9, state, 0);
}
/* a configuration as libctru's miiSelectorInit and miiSelectorSetOptions make one: every Mii pickable, the title,
   the Cancel button and the Guest Miis as asked, the magic miiSelectorLaunch puts in */
static void configure(SnxCtrProcess* p, const char* title, unsigned cancel, unsigned guests, uint32_t magic) {
    unsigned char config[0x104] = {0};
    config[0] = (unsigned char)cancel;
    config[1] = (unsigned char)guests;
    for (unsigned i = 0; title[i]; ++i) snx_st_le16(config + 8 + i * 2, (unsigned char)title[i]);
    memset(config + 0x94, 1, 6);
    memset(config + 0x9a, 1, 100);
    snx_st_le32(config + 0x100, magic);
    assert(snx_ctr_write_ram(p->space, CONFIG, config, sizeof config) == SNX_CTR_MEM_OK);
}
/* the log read last, and the stderr it was swapped from */
static struct {
    char text[8192];
    int saved;
} g_mii_log;
#ifndef _WIN32
static const char g_log_path[] = "3ds_mii_selector.log";
#endif
static void log_start(void) {
#ifndef _WIN32
    fflush(stderr);
    int fd = open(g_log_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    g_mii_log.saved = dup(2);
    assert(g_mii_log.saved >= 0 && fd >= 0 && dup2(fd, 2) == 2);
    close(fd);
#endif
}
static const char* log_end(void) {
    g_mii_log.text[0] = 0;
#ifndef _WIN32
    fflush(stderr);
    assert(dup2(g_mii_log.saved, 2) == 2);
    close(g_mii_log.saved);
    FILE* f = fopen(g_log_path, "rb");
    assert(f);
    size_t n = fread(g_mii_log.text, 1, sizeof g_mii_log.text - 1, f);
    fclose(f);
    remove(g_log_path);
    g_mii_log.text[n] = 0;
    fputs(g_mii_log.text, stderr); /* where it would have gone */
#endif
    return g_mii_log.text;
}
static void log_has(const char* said, const char* wanted) {
#ifndef _WIN32
    if (!strstr(said, wanted)) fprintf(stderr, "the log lacks: %s\n", wanted);
    assert(strstr(said, wanted));
#else
    (void)said, (void)wanted;
#endif
}
/* PrepareToStartLibraryApplet, IsRegistered, the capture block's Request and Response: the capture block's object */
static uint32_t prepared(SnxCtrProcess* p, uint32_t apt) {
    uint32_t prepare[] = {0x180040, SELECTOR}, out[7];
    assert(!call(p, apt, prepare, 2) && ctr_command_word(p, 0) == 0x180040);
    assert(registered(p, apt, SELECTOR) == 1 && registered(p, apt, 0x400) == 1 && !registered(p, apt, 0x401));
    unsigned char info[0x20] = {0};
    snx_st_le32(info, 0x8a000); /* a capture of both screens */
    assert(snx_ctr_write_ram(p->space, CAPTURE, info, sizeof info) == SNX_CTR_MEM_OK);
    send(p, apt, 2, CAPTURE, sizeof info, SNX_CTR_SVC_RETURN);
    assert(!ctr_command_word(p, 1));
    parameter(p, apt, 0xd, out);
    assert(out[0] == 0xd0104 && !out[1] && out[2] == SELECTOR && out[3] == 3 && !out[4] && out[5] == 0x10 && out[6]);
    uint32_t capture = snx_ctr_handle_object(p, out[6]);
    assert(capture && p->objects[capture].type == CTR_OBJ_SHARED && p->objects[capture].data.shared.size == 0x8a000);
    assert(!snx_ctr_handle_close(p, out[6]));
    return capture;
}
/* StartLibraryApplet with the configuration and no object: the reply's result */
static uint32_t start(SnxCtrProcess* p, uint32_t apt, uint32_t size) {
    uint32_t words[] = {0x1e0084, SELECTOR, size, 0, 0, (size << 14) | 2, CONFIG};
    return call(p, apt, words, 7);
}
/* the WakeupByExit the game wakes with: the MiiResult at RECEIVED, the selector closed */
static void woken(SnxCtrProcess* p, uint32_t apt) {
    uint32_t ready[] = {0x430040, APPLICATION}, out[7];
    assert(!call(p, apt, ready, 2));
    parameter(p, apt, 0xd, out);
    assert(out[0] == 0xd0104 && !out[1] && out[2] == SELECTOR && out[3] == 0xa && out[4] == 0x84);
    assert(out[5] == 0x10 && !out[6] && ctr_command_word(p, 7) == ((RECEIVE_CAP << 14) | 2));
    assert(!registered(p, apt, SELECTOR) && registered(p, apt, APPLICATION) == 1);
    parameter(p, apt, 0xd, out);
    assert(out[0] == 0xd0040 && out[1] == 0xc8a0cfef); /* NoData: it was taken */
}
/* CRC-16/XMODEM, bit by bit (3dbrew's Mii, Checksum: the CRC of the 0x5c bytes and their 2 of padding) */
static unsigned crc_xmodem(const unsigned char* p, unsigned n) {
    unsigned crc = 0;
    for (unsigned i = 0; i < n * 8; ++i) {
        unsigned bit = (p[i / 8] >> (7 - i % 8)) & 1, top = crc >> 15 & 1;
        crc = (crc << 1 & 0xffff) ^ (top ^ bit ? 0x1021 : 0);
    }
    return crc;
}
/* a field of the Mii's little-endian word of size bytes at offset: bits from first */
static unsigned field(const unsigned char* mii, unsigned offset, unsigned size, unsigned first, unsigned bits) {
    uint32_t word = size == 4 ? snx_ld_le32(mii + offset) : size == 2 ? snx_ld_le16(mii + offset) : mii[offset];
    return word >> first & ((1u << bits) - 1);
}
/* the MiiResult: a Mii of the database picked, its fields as ScarletNX's own Mii has them, its checksum right */
static void answered(SnxCtrProcess* p) {
    unsigned char result[0x84], name[20] = {0}, author[20] = {0};
    assert(snx_ctr_read_ram(p->space, RECEIVED, result, sizeof result) == SNX_CTR_MEM_OK);
    assert(snx_ld_be32(result) == 0 && snx_ld_be32(result + 4) == 0 && snx_ld_be32(result + 8) == 0xffffffffu);
    const unsigned char* mii = result + 0x0c;
    assert(mii[0] == 3 && (mii[3] >> 4 & 7) == 3); /* the format's version, made on a 3DS */
    uint32_t id = snx_ld_be32(mii + 0xc);
    assert((id >> 31) && (id >> 28 & 1) && !(id >> 29 & 3)); /* not special, not temporary, not a DSi's */
    for (unsigned i = 0; i < 8; ++i) name[i * 2] = (unsigned char)"Baguette"[i];
    assert(!memcmp(mii + 0x1a, name, sizeof name));
    for (unsigned i = 0; i < 6; ++i) author[i * 2] = (unsigned char)"Player"[i]; /* the console's user name */
    assert(!memcmp(mii + 0x48, author, sizeof author));
    /* each field, by 3dbrew's table: its word's offset and size, its first bit and bits, its value */
    static const unsigned char want[][5] = {
        {0x18, 2, 0, 1, 1},   {0x18, 2, 1, 4, 0},   {0x18, 2, 5, 5, 0},   {0x18, 2, 10, 4, 0},  {0x18, 2, 14, 1, 1},
        {0x2e, 1, 0, 8, 50},  {0x2f, 1, 0, 8, 64},  {0x30, 1, 0, 1, 0},   {0x30, 1, 1, 4, 0},   {0x30, 1, 5, 3, 0},
        {0x31, 1, 0, 4, 0},   {0x31, 1, 4, 4, 0},   {0x32, 1, 0, 8, 112}, {0x33, 1, 0, 3, 2},   {0x33, 1, 3, 1, 0},
        {0x34, 4, 0, 6, 28},  {0x34, 4, 6, 3, 2},   {0x34, 4, 9, 4, 4},   {0x34, 4, 13, 3, 3},  {0x34, 4, 16, 5, 4},
        {0x34, 4, 21, 4, 2},  {0x34, 4, 25, 5, 12}, {0x38, 4, 0, 5, 1},   {0x38, 4, 5, 3, 2},   {0x38, 4, 8, 4, 4},
        {0x38, 4, 12, 3, 3},  {0x38, 4, 16, 5, 6},  {0x38, 4, 21, 4, 2},  {0x38, 4, 25, 5, 10}, {0x3c, 2, 0, 5, 1},
        {0x3c, 2, 5, 4, 4},   {0x3c, 2, 9, 5, 9},   {0x3e, 2, 0, 6, 22},  {0x3e, 2, 6, 3, 2},   {0x3e, 2, 9, 4, 4},
        {0x3e, 2, 13, 3, 3},  {0x40, 2, 0, 5, 13},  {0x40, 2, 5, 3, 0},   {0x42, 2, 0, 3, 0},   {0x42, 2, 3, 3, 2},
        {0x42, 2, 6, 4, 4},   {0x42, 2, 10, 5, 10}, {0x44, 2, 0, 4, 0},   {0x44, 2, 4, 3, 0},   {0x44, 2, 7, 4, 4},
        {0x44, 2, 11, 5, 10}, {0x46, 2, 0, 1, 0},   {0x46, 2, 1, 4, 4},   {0x46, 2, 5, 5, 2},   {0x46, 2, 10, 5, 20}};
    for (unsigned i = 0; i < sizeof want / sizeof *want; ++i) {
        unsigned got = field(mii, want[i][0], want[i][1], want[i][2], want[i][3]);
        if (got != want[i][4])
            fprintf(stderr, "the Mii's field at 0x%02x bit %u is %u, not %u\n", want[i][0], want[i][2], got,
                    want[i][4]);
        assert(got == want[i][4]);
    }
    assert(!mii[0x5c] && !mii[0x5d] && snx_ld_be16(mii + 0x5e) == crc_xmodem(mii, 0x5e));
    for (unsigned i = 0x6c; i < 0x84; ++i) assert(!result[i]); /* no Guest Mii's name */
}
int main(void) {
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = ctr_image(code, sizeof code, 0x3000);
    SnxCtrProcess* p = snx_ctr_process_new(&image, SNX_CTR_OLD);
    assert(p);
    g_settings.ctr_answer_unknown_commands = 0; /* every command the selector needs is one APT has */
    uint32_t apt = started(p), out[7];

    /* a game's own title, Cancel, the Guest Miis: the database's Mii, ScarletNX's own */
    configure(p, "Pick your Mii", 1, 1, 0x13de28cf);
    uint32_t capture = prepared(p, apt);
    log_start();
    assert(!start(p, apt, 0x104));
    const char* said = log_end();
    woken(p, apt);
    answered(p);
    log_has(said, "3ds mii selector: the game opens the Mii selector: title \"Pick your Mii\", Cancel 1, Guest Miis 1, "
                  "the Guest page first 0, the database's first Mii pickable 1");
    log_has(said, "3ds mii selector: the Mii selector answers with ScarletNX's own Mii, Baguette, as no box asks the "
                  "player");
    assert(!strstr(said, "magic"));

    /* again, as a game whose player is asked once more: the same capture block, the same Mii */
    configure(p, "", 0, 0, 0x13de28cf);
    assert(prepared(p, apt) == capture);
    assert(!start(p, apt, 0x104));
    woken(p, apt);
    answered(p);

    /* a configuration without the magic: the console's selector lists no Mii, this one answers and says so */
    configure(p, "", 0, 1, 0);
    (void)prepared(p, apt);
    log_start();
    assert(!start(p, apt, 0x104));
    said = log_end();
    woken(p, apt);
    answered(p);
    log_has(said, "3ds mii selector: the configuration's magic is 0x00000000, not 0x13de28cf: the console's selector "
                  "would list no Mii, this one answers with its own");

    /* one parameter at a time: the WakeupByExit waiting stops another start until the game takes it */
    configure(p, "", 0, 0, 0x13de28cf);
    (void)prepared(p, apt);
    assert(!start(p, apt, 0x104));
    uint32_t prepare[] = {0x180040, SELECTOR};
    assert(call(p, apt, prepare, 2) == 0xc8a0cc02); /* ParameterPresent */
    woken(p, apt);

    /* a configuration of another size, or a parameter other than the Request: the run stops, saying what it was */
    (void)prepared(p, apt);
    log_start();
    uint32_t words[] = {0x1e0084, SELECTOR, 0x100, 0, 0, (0x100u << 14) | 2, CONFIG};
    ctr_call(p, apt, words, 7, SNX_CTR_SVC_UNSUPPORTED, 0);
    send(p, apt, 5, CONFIG, 0x104, SNX_CTR_SVC_UNSUPPORTED);
    said = log_end();
    log_has(said, "3ds mii selector: the game starts the Mii selector with a 256-byte configuration: it takes 0x104 "
                  "bytes");
    log_has(said, "3ds apt: the game sends the Mii selector signal 5 with 260 bytes, which it does not take (a "
                  "Request with a 32-byte capture info)");
    assert(registered(p, apt, SELECTOR) == 1);
    parameter(p, apt, 0xd, out);
    assert(out[1] == 0xc8a0cfef); /* nothing came of either */
    assert(!start(p, apt, 0x104));
    woken(p, apt);
    answered(p);

    /* the system's Mii selector (0x202) is the same applet */
    uint32_t system[] = {0x180040, 0x202};
    assert(!call(p, apt, system, 2) && registered(p, apt, 0x202) == 1 && registered(p, apt, 0x200) == 1);
    uint32_t begin[] = {0x1e0084, 0x202, 0x104, 0, 0, (0x104u << 14) | 2, CONFIG};
    assert(!call(p, apt, begin, 7) && !registered(p, apt, 0x202));
    parameter(p, apt, 0xd, out);
    assert(out[2] == 0x202 && out[3] == 0xa && out[4] == 0x84);
    answered(p);
    snx_ctr_process_free(p);
    puts("3DS Mii selector: start, capture block, the Mii answered, its checksum, the magic and the wakeup passed");
    return 0;
}
