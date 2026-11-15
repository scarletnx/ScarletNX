/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Controllers > Bluetooth's controllers (gui/bluetooth.h) over the scan's fake backend (common/platform/ble_scan.h):
   adverts heard and aged, joycon2.txt read as the runtime reads it, a controller synced and forgotten with the file's
   other lines kept, the runtime's states. No Bluetooth: the fake backend alone, joycon2.txt in a temporary folder */
#include "gui/bluetooth.h"
#include "common/utilities/common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* a Joy-Con 2's advert: right or left, waiting to sync (its console's address all zero) or paired with one */
static SnxBleAdvert joycon2(uint8_t last, int right, int syncing) {
    static const uint8_t data[16] = {0x01, 0x00, 0x03, 0x7e, 0x05, 0x67, 0x20, 0x00, 0x01, 0x80};
    SnxBleAdvert a;
    memset(&a, 0, sizeof a);
    a.address[0] = 0x98;
    a.address[1] = 0xb6;
    a.address[5] = last;
    a.random = 0;
    a.rssi = -60;
    a.company = SNX_BLE_NINTENDO;
    memcpy(a.data, data, sizeof data);
    a.data[5] = right ? 0x66 : 0x67;
    if (!syncing) memcpy(a.data + 10, "\x11\x22\x33\x44\x55\x66", 6);
    a.size = 16;
    return a;
}
static char* text_of(const char* path) {
    size_t n = 0;
    char *bytes = (char*)snx_read_file(path, &n), *text;
    if (!bytes) return NULL;
    text = snx_format("%.*s", (int)n, bytes);
    free(bytes);
    return text;
}
static void file_is(const char* path, const char* want) {
    char* text = text_of(path);
    if (!text || strcmp(text, want)) {
        fprintf(stderr, "gui_bluetooth: %s is\n%s\nnot\n%s\n", path, text ? text : "(none)", want);
        assert(0);
    }
    free(text);
}

/* the scan: the fake backend's adverts while it listens, none before or after; a start refused says why */
static void scan(void) {
    SnxBleScan* s = snx_ble_fake_open("");
    SnxBleAdvert a = joycon2(1, 0, 1), out[4];
    char why[128] = "";
    assert(s && !strcmp(snx_ble_scan_name(s), "fake") && !snx_ble_scan_listening(s));
    assert(!snx_ble_fake_hear(s, &a) && snx_ble_scan_poll(s, out, 4, why, sizeof why) == 0); /* nobody listens */
    assert(!snx_ble_scan_start(s, SNX_BLE_NINTENDO, why, sizeof why) && snx_ble_scan_listening(s));
    assert(!snx_ble_scan_start(s, SNX_BLE_NINTENDO, why, sizeof why) && snx_ble_fake_starts(s) == 1); /* once */
    for (int i = 0; i < 6; i++) assert(!snx_ble_fake_hear(s, &a));
    assert(snx_ble_scan_poll(s, out, 4, why, sizeof why) == 4 && snx_ble_scan_poll(s, out, 4, why, sizeof why) == 2);
    assert(snx_ble_scan_poll(s, out, 4, why, sizeof why) == 0);
    assert(!snx_ble_fake_hear(s, &a));
    snx_ble_scan_stop(s);
    assert(!snx_ble_scan_listening(s) && snx_ble_scan_poll(s, out, 4, why, sizeof why) == 0); /* dropped */
    snx_ble_scan_close(s);
    snx_ble_scan_close(NULL);
    s = snx_ble_fake_open("Bluetooth is off");
    assert(s && snx_ble_scan_start(s, SNX_BLE_NINTENDO, why, sizeof why) && !strcmp(why, "Bluetooth is off"));
    assert(!snx_ble_scan_listening(s) && snx_ble_fake_starts(s) == 0);
    snx_ble_scan_close(s);
}
/* the Joy-Con 2's advert: side, waiting to sync, and what is not one */
static void adverts(void) {
    SnxBleAdvert a = joycon2(1, 1, 1);
    SnxBleJoyCon2 j;
    uint8_t address[6];
    char text[18];
    assert(snx_ble_joycon2(&a, &j) && j.right && j.syncing);
    a = joycon2(1, 0, 0);
    assert(snx_ble_joycon2(&a, &j) && !j.right && !j.syncing);
    a.company = 0x004c; /* another company's */
    assert(!snx_ble_joycon2(&a, &j));
    a = joycon2(1, 0, 1);
    a.data[5] = 0x69; /* a Pro Controller 2's product id: one too, neither side */
    a.data[9] = 0x00;
    assert(snx_ble_joycon2(&a, &j) && j.pro && !j.right && j.syncing && !j.wake);
    a.data[9] = 0x81; /* its flags asking its console to wake */
    assert(snx_ble_joycon2(&a, &j) && j.wake);
    a.data[5] = 0x73; /* another of the console maker's pads: none */
    assert(!snx_ble_joycon2(&a, &j));
    a = joycon2(1, 0, 1);
    a.size = 15;
    assert(!snx_ble_joycon2(&a, &j));
    assert(snx_ble_address_parse("98:b6:00:00:00:0A L", address) && address[0] == 0x98 && address[5] == 0x0a);
    snx_ble_address_text(address, text);
    assert(!strcmp(text, "98:B6:00:00:00:0A"));
    assert(!snx_ble_address_parse("98:B6:00:00:00", address) && !snx_ble_address_parse("98-B6-00-00-00-0A", address));
}
/* heard, aged, read, synced, forgotten; the runtime's states */
static void rows(const char* dir) {
    char *path = snx_path_join(dir, "joycon2.txt"), why[256] = "";
    GuiBt bt;
    SnxBleAdvert a;
    memset(&bt, 0, sizeof bt);
    assert(gui_bt_read(&bt, path) == 0 && bt.count == 0); /* no file: none known */
    /* a left one waiting to sync, a right one paired with its console, and another company's device */
    a = joycon2(1, 0, 1);
    assert(gui_bt_heard(&bt, &a, 1000) == 1);
    a = joycon2(2, 1, 0);
    assert(gui_bt_heard(&bt, &a, 1500) == 1);
    a.company = 0x004c;
    assert(gui_bt_heard(&bt, &a, 1500) == 0);
    assert(bt.count == 2 && gui_bt_state(&bt.dev[0]) == GUI_BT_WAITING && !bt.dev[0].right && !bt.dev[0].known);
    assert(gui_bt_state(&bt.dev[1]) == GUI_BT_NEARBY && bt.dev[1].right && bt.dev[1].rssi == -60);
    /* only one waiting to sync is synced: the right one, paired with its console, is refused */
    assert(gui_bt_sync(&bt, 1, path, why, sizeof why) == -1 && strstr(why, "sync button"));
    assert(!snx_path_exists(path));
    assert(gui_bt_sync(&bt, 0, path, why, sizeof why) == 0);
    file_is(path, "98:B6:00:00:00:01 L\n");
    assert(bt.count == 2 && bt.dev[0].known && gui_bt_state(&bt.dev[0]) == GUI_BT_WAITING);
    assert(gui_bt_sync(&bt, 0, path, why, sizeof why) == -1 && strstr(why, "known already"));
    /* time passes: the unknown one goes, the known one stays, away */
    gui_bt_tick(&bt, 1500 + GUI_BT_HEARD_MS);
    assert(bt.count == 1 && bt.dev[0].known && gui_bt_state(&bt.dev[0]) == GUI_BT_AWAY);
    /* a file as the runtime and the player left it: a serial, a comment, a line it refuses, no newline at the end. The
       side is the file's, whatever the advert says; the serial and the other lines stay */
    assert(!snx_write_file(
        path, "# mine\n98:B6:00:00:00:03 R SER123\nnonsense\n98:b6:00:00:00:04 X\n98:B6:00:00:00:01 L ",
        strlen("# mine\n98:B6:00:00:00:03 R SER123\nnonsense\n98:b6:00:00:00:04 X\n98:B6:00:00:00:01 L ")));
    assert(gui_bt_read(&bt, path) == 2 && bt.count == 2);
    assert(bt.dev[0].address[5] == 3 && bt.dev[0].right && !strcmp(bt.dev[0].serial, "SER123"));
    assert(bt.dev[1].address[5] == 1 && !bt.dev[1].right && !bt.dev[1].serial[0]);
    a = joycon2(5, 1, 1);
    assert(gui_bt_heard(&bt, &a, 20000) && bt.count == 3 && gui_bt_sync(&bt, 2, path, why, sizeof why) == 0);
    file_is(path, "# mine\n98:B6:00:00:00:03 R SER123\nnonsense\n98:b6:00:00:00:04 X\n98:B6:00:00:00:01 L \n"
                  "98:B6:00:00:00:05 R\n");
    /* forgotten: its line alone goes, the comment and the line refused stay */
    assert(gui_bt_forget(&bt, 0, path, why, sizeof why) == 0);
    file_is(path, "# mine\nnonsense\n98:b6:00:00:00:04 X\n98:B6:00:00:00:01 L \n98:B6:00:00:00:05 R\n");
    assert(bt.count == 2 && bt.dev[0].address[5] == 1 && bt.dev[1].address[5] == 5 && bt.dev[1].known);
    assert(gui_bt_forget(&bt, 1, path, why, sizeof why) == 0 && bt.count == 2 && !bt.dev[1].known); /* still heard */
    assert(gui_bt_forget(&bt, 1, path, why, sizeof why) == -1 && strstr(why, "not known"));
    /* the runtime's states for the rows it names, until they are old; a line of no state or of no row is left */
    assert(gui_bt_said(&bt, "98:B6:00:00:00:01 ready 3912\n98:B6:00:00:00:05 ready\n98:B6:00:00:00:01 asleep\nx\n",
                       21000) == 2);
    assert(gui_bt_state(&bt.dev[0]) == GUI_BT_READY && bt.dev[0].battery_mv == 3912);
    assert(gui_bt_said(&bt, "98:B6:00:00:00:01 failed", 22000) == 1 && gui_bt_state(&bt.dev[0]) == GUI_BT_FAILED);
    assert(gui_bt_said(&bt, "98:B6:00:00:00:01 connecting\n", 23000) == 1);
    assert(gui_bt_state(&bt.dev[0]) == GUI_BT_CONNECTING && !bt.dev[0].battery_mv);
    gui_bt_tick(&bt, 23000 + GUI_BT_SAID_MS);
    assert(gui_bt_state(&bt.dev[0]) == GUI_BT_AWAY);
    /* a Pro Controller 2 waiting to sync: its row says so, its line has P, and it reads back as one. An old file's L
       and R lines read as before beside it */
    a = joycon2(6, 0, 1);
    a.data[5] = 0x69;
    assert(gui_bt_heard(&bt, &a, 30000) == 1);
    {
        int i;
        for (i = 0; i < bt.count && bt.dev[i].address[5] != 6; i++) {}
        assert(i < bt.count && bt.dev[i].pro && !bt.dev[i].right && gui_bt_state(&bt.dev[i]) == GUI_BT_WAITING);
        assert(gui_bt_sync(&bt, i, path, why, sizeof why) == 0);
    }
    file_is(path, "# mine\nnonsense\n98:b6:00:00:00:04 X\n98:B6:00:00:00:01 L \n98:B6:00:00:00:06 P\n");
    assert(gui_bt_read(&bt, path) == 2 && bt.dev[1].address[5] == 6 && bt.dev[1].pro && bt.dev[1].known);
    assert(!bt.dev[0].pro && !bt.dev[0].right);
    assert(gui_bt_forget(&bt, 1, path, why, sizeof why) == 0);
    file_is(path, "# mine\nnonsense\n98:b6:00:00:00:04 X\n98:B6:00:00:00:01 L \n");
    free(path);
}
/* at most GUI_BT_KNOWN known: the runtime reads no more, and a 17th is refused */
static void most(const char* dir) {
    char *path = snx_path_join(dir, "most.txt"), why[256] = "", line[32];
    GuiBt bt;
    SnxBleAdvert a;
    FILE* f = fopen(path, "w");
    assert(f);
    for (int i = 0; i < GUI_BT_KNOWN; i++) fprintf(f, "98:B6:00:00:01:%02X %c\n", i, i & 1 ? 'R' : 'L');
    fclose(f);
    memset(&bt, 0, sizeof bt);
    assert(gui_bt_read(&bt, path) == GUI_BT_KNOWN);
    a = joycon2(0x40, 0, 1);
    assert(gui_bt_heard(&bt, &a, 1000) && bt.count == GUI_BT_KNOWN + 1);
    assert(gui_bt_sync(&bt, GUI_BT_KNOWN, path, why, sizeof why) == -1 && strstr(why, "forget one first"));
    snprintf(line, sizeof line, "%d", GUI_BT_KNOWN);
    assert(strstr(why, line));
    /* rows past GUI_BT_MAX are not added */
    for (int i = 0; i < 40; i++) {
        a = joycon2((uint8_t)(0x50 + i), i & 1, 1);
        gui_bt_heard(&bt, &a, 2000);
    }
    assert(bt.count == GUI_BT_MAX);
    free(path);
}
int main(void) {
    char* dir = snx_temp_dir(NULL);
    assert(dir);
    scan();
    adverts();
    rows(dir);
    most(dir);
    assert(!snx_remove_tree(dir));
    free(dir);
    puts("gui_bluetooth: ok");
    return 0;
}
