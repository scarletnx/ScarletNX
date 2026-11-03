/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The software keyboard a 3DS game starts through APT, driven as the console's keyboard library drives it (libctru's
   aptLaunchLibraryApplet and swkbdInputText copy its steps): PrepareToStartLibraryApplet, IsRegistered until the
   applet is there, a Request for the capture block and its Response, StartLibraryApplet with the configuration and
   the text block, NotifyToWait, then the WakeupByExit the game wakes with, which carries the configuration with the
   answer, the text in the block. Each answer is checked in the reply's bytes and in the log line that says it */
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

#define SWKBD_CONFIG      0x300000u /* the game's SoftwareKeyboardConfig */
#define SWKBD_CAPTURE     0x300400u /* its CaptureBufferInfo */
#define SWKBD_RECEIVED    0x300800u /* where Receive and Glance put a parameter's buffer */
#define SWKBD_CHECKED     0x301000u /* the configuration the game's own check sends back */
#define SWKBD_TEXT        0x302000u /* the text block */
#define SWKBD_TEXT_SIZE   0x1000u
#define SWKBD_APPLICATION 0x300u
#define SWKBD_KEYBOARD    0x401u
enum { CONFIG_BUTTONS = 4, CONFIG_RULE = 8, CONFIG_FILTERS = 0x18, CONFIG_MAX = 0x20, CONFIG_INITIAL = 0x120 };
enum { CONFIG_RESULT = 0x138, CONFIG_TEXT_OFFSET = 0x144, CONFIG_TEXT_LENGTH = 0x148, CONFIG_CALLBACK = 0x14c };

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
    uint32_t init[] = {0x20080, SWKBD_APPLICATION, 0}, ready[] = {0x430040, SWKBD_APPLICATION},
             receive[] = {0xd0080, SWKBD_APPLICATION, 0};
    uint32_t unlock[] = {0x4b00c2, 7, 4, 0, 0x10402, SWKBD_CAPTURE};
    assert(!call(p, apt, init, 3) && !call(p, apt, ready, 2));
    static_output(p, 0, SWKBD_RECEIVED);
    ctr_request(p, receive, 3);
    ctr_send(p, apt, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1) && ctr_command_word(p, 3) == 1);
    ctr_put(p, SWKBD_CAPTURE, 0x10);
    static_output(p, 0, SWKBD_RECEIVED);
    assert(!call(p, apt, unlock, 6) && p->apt_phase == 4);
    return apt;
}
static uint32_t registered(SnxCtrProcess* p, uint32_t apt, uint32_t id) {
    uint32_t words[] = {0x90040, id};
    assert(!call(p, apt, words, 2) && ctr_command_word(p, 0) == 0x90080);
    return ctr_command_word(p, 2);
}
/* Glance (0xe) or Receive (0xd): the reply's words 1 to 6 in out, the buffer at SWKBD_RECEIVED */
static void parameter(SnxCtrProcess* p, uint32_t apt, uint32_t command, uint32_t out[7]) {
    uint32_t words[] = {command << 16 | 0x80, SWKBD_APPLICATION, 0x400};
    static_output(p, 0x400, SWKBD_RECEIVED);
    ctr_call(p, apt, words, 3, SNX_CTR_SVC_RETURN, 0);
    for (unsigned i = 0; i < 7; ++i) out[i] = ctr_command_word(p, i);
}
static uint32_t send_to_keyboard(SnxCtrProcess* p, uint32_t apt, uint32_t signal, uint32_t address, uint32_t size) {
    uint32_t words[] = {0xc0104, SWKBD_APPLICATION, SWKBD_KEYBOARD, signal, size, 0, 0, (size << 14) | 2, address};
    return call(p, apt, words, 9);
}
/* a configuration as libctru's swkbdInit makes one (buttons - 1, an input rule, the most characters), with no
   initial text */
static void configure(SnxCtrProcess* p, uint32_t buttons_m1, uint32_t rule, uint32_t most) {
    unsigned char zero[0x400] = {0};
    assert(snx_ctr_write_ram(p->space, SWKBD_CONFIG, zero, sizeof zero) == SNX_CTR_MEM_OK);
    ctr_put(p, SWKBD_CONFIG + CONFIG_BUTTONS, buttons_m1);
    ctr_put(p, SWKBD_CONFIG + CONFIG_RULE, rule);
    ctr_put(p, SWKBD_CONFIG + CONFIG_MAX, most);
    for (uint32_t at = CONFIG_INITIAL; at <= 0x12c; at += 4) ctr_put(p, SWKBD_CONFIG + at, 0xffffffffu);
    ctr_put(p, SWKBD_CONFIG + 0x134, 5);           /* version */
    ctr_put(p, SWKBD_CONFIG + CONFIG_RESULT, ~0u); /* SWKBD_NONE */
}
static void initial_text(SnxCtrProcess* p, const char* text) {
    unsigned char bytes[64] = {0};
    for (unsigned i = 0; text[i]; ++i) bytes[i * 2] = (unsigned char)text[i];
    assert(snx_ctr_write_ram(p->space, SWKBD_TEXT, bytes, sizeof bytes) == SNX_CTR_MEM_OK);
    ctr_put(p, SWKBD_CONFIG + CONFIG_INITIAL, 0);
}
/* the log read last, and the stderr it was swapped from */
static struct {
    char text[8192];
    int saved;
} g_swkbd_log;
#ifndef _WIN32
static const char g_log_path[] = "3ds_swkbd.log";
#endif
/* the runtime's stderr, its log, read where fd 2 can be swapped for a file */
static void log_start(void) {
#ifndef _WIN32
    fflush(stderr);
    int fd = open(g_log_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    g_swkbd_log.saved = dup(2);
    assert(g_swkbd_log.saved >= 0 && fd >= 0 && dup2(fd, 2) == 2);
    close(fd);
#endif
}
static const char* log_end(void) {
    g_swkbd_log.text[0] = 0;
#ifndef _WIN32
    fflush(stderr);
    assert(dup2(g_swkbd_log.saved, 2) == 2);
    close(g_swkbd_log.saved);
    FILE* f = fopen(g_log_path, "rb");
    assert(f);
    size_t n = fread(g_swkbd_log.text, 1, sizeof g_swkbd_log.text - 1, f);
    fclose(f);
    remove(g_log_path);
    g_swkbd_log.text[n] = 0;
    fputs(g_swkbd_log.text, stderr); /* where it would have gone */
#endif
    return g_swkbd_log.text;
}
static void log_has(const char* said, const char* wanted) {
#ifndef _WIN32
    if (!strstr(said, wanted)) fprintf(stderr, "the log lacks: %s\n", wanted);
    assert(strstr(said, wanted));
#else
    (void)said, (void)wanted;
#endif
}
/* PrepareToStartLibraryApplet, IsRegistered, the capture block's Request and Response, then StartLibraryApplet with
   the text block: the reply's result */
static uint32_t launch(SnxCtrProcess* p, uint32_t apt, uint32_t block) {
    uint32_t prepare[] = {0x180040, SWKBD_KEYBOARD}, out[7];
    assert(!call(p, apt, prepare, 2) && ctr_command_word(p, 0) == 0x180040);
    assert(registered(p, apt, SWKBD_KEYBOARD) == 1 && registered(p, apt, 0x400) == 1 && !registered(p, apt, 0x402));
    unsigned char info[0x20] = {0};
    snx_st_le32(info, 0x8a000); /* a capture of both screens */
    assert(snx_ctr_write_ram(p->space, SWKBD_CAPTURE, info, sizeof info) == SNX_CTR_MEM_OK);
    assert(!send_to_keyboard(p, apt, 2, SWKBD_CAPTURE, sizeof info));
    parameter(p, apt, 0xe, out);
    assert(out[0] == 0xe0104 && !out[1] && out[2] == SWKBD_KEYBOARD && out[3] == 3 && !out[4] && out[5] == 0x10 &&
           out[6]);
    parameter(p, apt, 0xd, out);
    assert(out[0] == 0xd0104 && !out[1] && out[2] == SWKBD_KEYBOARD && out[3] == 3 && out[6]);
    uint32_t capture = snx_ctr_handle_object(p, out[6]);
    assert(capture && p->objects[capture].type == CTR_OBJ_SHARED && p->objects[capture].data.shared.size == 0x8a000);
    uint32_t* r = p->cpu.state.cpu.r;
    r[0] = out[6], r[1] = 0, r[2] = 3, r[3] = 0x10000000; /* mapped where its pages are, as the library copies to it */
    assert(snx_ctr_svc(p, 0x1f) == SNX_CTR_SVC_RETURN && !r[0]);
    r[0] = out[6], r[1] = snx_ctr_linear_address(p->objects[capture].data.shared.physical);
    assert(snx_ctr_svc(p, 0x20) == SNX_CTR_SVC_RETURN && !r[0]);
    assert(!snx_ctr_handle_close(p, out[6]));
    uint32_t start[] = {0x1e0084, SWKBD_KEYBOARD, 0x400, 0, block, (0x400u << 14) | 2, SWKBD_CONFIG};
    return call(p, apt, start, 7);
}
/* the WakeupByExit the game wakes with: its configuration at SWKBD_RECEIVED, the keyboard closed */
static void woken(SnxCtrProcess* p, uint32_t apt) {
    uint32_t ready[] = {0x430040, SWKBD_APPLICATION}, out[7];
    assert(!call(p, apt, ready, 2));
    parameter(p, apt, 0xd, out);
    assert(out[0] == 0xd0104 && !out[1] && out[2] == SWKBD_KEYBOARD && out[3] == 0xa && out[4] == 0x400);
    assert(out[5] == 0x10 && !out[6] && ctr_command_word(p, 7) == ((0x400u << 14) | 2));
    assert(!registered(p, apt, SWKBD_KEYBOARD) && registered(p, apt, SWKBD_APPLICATION) == 1);
    parameter(p, apt, 0xd, out);
    assert(out[0] == 0xd0040 && out[1] == 0xc8a0cfef); /* NoData: it was taken */
}
/* the answer the configuration gives: its result, its text's place and length, and the block's text */
static void answered(SnxCtrProcess* p, uint32_t result, const char* text) {
    unsigned char bytes[64];
    uint32_t n = (uint32_t)strlen(text);
    assert(ctr_word(p, SWKBD_RECEIVED + CONFIG_RESULT) == result);
    assert(!ctr_word(p, SWKBD_RECEIVED + CONFIG_TEXT_OFFSET) &&
           (ctr_word(p, SWKBD_RECEIVED + CONFIG_TEXT_LENGTH) & 0xffff) == n);
    assert(snx_ctr_read_ram(p->space, SWKBD_TEXT, bytes, sizeof bytes) == SNX_CTR_MEM_OK);
    for (uint32_t i = 0; i <= n; ++i) assert(snx_ld_le16(bytes + i * 2) == (unsigned char)text[i]);
    assert(ctr_word(p, SWKBD_RECEIVED + 0x134) == 5); /* the rest is the game's configuration as it sent it */
}
int main(void) {
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = ctr_image(code, sizeof code, 0x3000);
    SnxCtrProcess* p = snx_ctr_process_new(&image, SNX_CTR_OLD);
    assert(p);
    g_settings.ctr_answer_unknown_commands = 0; /* every command the keyboard needs is one APT has */
    g_settings.keyboard_text[0] = 0;
    uint32_t apt = started(p);
    uint32_t* r = p->cpu.state.cpu.r;
    r[0] = 3, r[1] = SWKBD_TEXT, r[2] = SWKBD_TEXT_SIZE, r[3] = 3; /* the text block, as swkbdInputText makes it */
    assert(snx_ctr_svc(p, 0x1e) == SNX_CTR_SVC_RETURN && !r[0]);
    uint32_t block = r[1], out[7];

    /* the game's initial text, Cancel and OK, not empty: OK (D1Click1) with it */
    configure(p, 1, 1, 8);
    initial_text(p, "Ann");
    log_start();
    assert(!launch(p, apt, block));
    const char* said = log_end();
    woken(p, apt);
    answered(p, 2, "Ann");
    log_has(said, "3ds swkbd: the game opens the software keyboard: type 0, buttons 1, input rule 1, filters 0x0, up "
                  "to 8 characters, language 0 (0: the system's)");
    log_has(said, "3ds swkbd: the software keyboard answers \"Ann\" (the game's text) with OK, as no box asks the "
                  "player");

    /* no initial text: the console's user name, its length in the log for it, cut to the game's 4 */
    configure(p, 1, 2, 4);
    log_start();
    assert(!launch(p, apt, block));
    said = log_end();
    woken(p, apt);
    answered(p, 2, "Play");
    log_has(said, "3ds swkbd: the user name is cut from 6 characters to the game's 4");
    log_has(said, "answers the console's user name (4 characters) with OK");
    assert(!strstr(said, "Play"));

    /* a fixed length the user name is short of: the left button, Cancel (D1Click0), with no text */
    configure(p, 1, 4, 8);
    log_start();
    assert(!launch(p, apt, block));
    said = log_end();
    woken(p, apt);
    answered(p, 1, "");
    log_has(said, "3ds swkbd: the software keyboard is cancelled: the console's user name (6 characters) is 6 "
                  "characters, the game asks for exactly 8");

    /* swkbd_text first, over the game's text; one button: D0Click */
    snprintf(g_settings.keyboard_text, sizeof g_settings.keyboard_text, "%s", "Zed");
    configure(p, 0, 0, 10);
    initial_text(p, "Ann");
    log_start();
    assert(!launch(p, apt, block));
    said = log_end();
    woken(p, apt);
    answered(p, 0, "Zed");
    log_has(said, "answers \"Zed\" (swkbd_text) with OK");
    g_settings.keyboard_text[0] = 0;

    /* a filter the text breaks: an @, three buttons: the left one (D2Click0) */
    configure(p, 2, 0, 10);
    ctr_put(p, SWKBD_CONFIG + CONFIG_FILTERS, 2);
    initial_text(p, "a@b");
    assert(!launch(p, apt, block));
    woken(p, apt);
    answered(p, 3, "");

    /* the game's own check: the text comes to it in a Message first; it closes the keyboard: BannedInput */
    configure(p, 1, 0, 8);
    ctr_put(p, SWKBD_CONFIG + CONFIG_FILTERS, 0x20);
    initial_text(p, "Bob");
    assert(!launch(p, apt, block));
    parameter(p, apt, 0xd, out);
    assert(out[2] == SWKBD_KEYBOARD && out[3] == 5 && out[4] == 0x400 &&
           ctr_word(p, SWKBD_RECEIVED + CONFIG_RESULT) == 2);
    assert(registered(p, apt, SWKBD_KEYBOARD) == 1);
    unsigned char checked[0x400];
    assert(snx_ctr_read_ram(p->space, SWKBD_RECEIVED, checked, sizeof checked) == SNX_CTR_MEM_OK);
    snx_st_le32(checked + CONFIG_CALLBACK, 1);
    snx_st_le16(checked + 0x150, 'N'), snx_st_le16(checked + 0x152, 'o');
    assert(snx_ctr_write_ram(p->space, SWKBD_CHECKED, checked, sizeof checked) == SNX_CTR_MEM_OK);
    log_start();
    assert(!send_to_keyboard(p, apt, 5, SWKBD_CHECKED, 0x400));
    said = log_end();
    woken(p, apt);
    assert(ctr_word(p, SWKBD_RECEIVED + CONFIG_RESULT) == 30 &&
           !(ctr_word(p, SWKBD_RECEIVED + CONFIG_TEXT_LENGTH) & 0xffff));
    log_has(said, "the game's own check refuses the text (\"No\") and closes the keyboard: the keyboard closes with "
                  "BannedInput (30)");

    /* it takes the text: OK stays, the text with it */
    initial_text(p, "Bob");
    assert(!launch(p, apt, block));
    parameter(p, apt, 0xd, out);
    assert(out[3] == 5);
    assert(snx_ctr_read_ram(p->space, SWKBD_RECEIVED, checked, sizeof checked) == SNX_CTR_MEM_OK);
    snx_st_le32(checked + CONFIG_CALLBACK, 0);
    assert(snx_ctr_write_ram(p->space, SWKBD_CHECKED, checked, sizeof checked) == SNX_CTR_MEM_OK);
    assert(!send_to_keyboard(p, apt, 5, SWKBD_CHECKED, 0x400));
    woken(p, apt);
    answered(p, 2, "Bob");

    /* one parameter at a time: the WakeupByExit waiting stops another start until the game takes it */
    configure(p, 0, 0, 8);
    assert(!launch(p, apt, block));
    uint32_t prepare[] = {0x180040, SWKBD_KEYBOARD};
    assert(call(p, apt, prepare, 2) == 0xc8a0cc02); /* ParameterPresent */
    woken(p, apt);
    answered(p, 0, "Player");

    /* a library applet the runtime does not have answers as an unknown command does: it stops this run */
    uint32_t other[] = {0x180040, 0x402};
    ctr_call(p, apt, other, 2, SNX_CTR_SVC_UNSUPPORTED, 0);
    assert(!registered(p, apt, 0x402) && !registered(p, apt, SWKBD_KEYBOARD));
    assert(!snx_ctr_handle_close(p, block));
    snx_ctr_process_free(p);
    puts("3DS software keyboard: start, capture block, answers, checks, the game's own check and the wakeup passed");
    return 0;
}
