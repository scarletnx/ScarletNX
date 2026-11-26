/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/bluetooth.h"
#include "common/platform/file.h"
#include "common/utilities/common.h"
#include "common/utilities/config_file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* a line of joycon2.txt as joycon2.c's known_load takes it: the address, then L or R (a Joy-Con 2's side) or P (a Pro
   Controller 2), then the serial if any. n is the line's length with its newline, if it has one. 1 when it is one,
   its kind in *kind: 0 L, 1 R, 2 P */
static int entry(const char* line, size_t n, uint8_t address[6], int* kind, char serial[17]) {
    char text[128], side = 0;
    if (n < 19 || n >= sizeof text) return 0;
    memcpy(text, line, n);
    text[n] = 0;
    serial[0] = 0;
    if (!snx_ble_address_parse(text, address) || sscanf(text + 17, " %c %16s", &side, serial) < 1 ||
        (side != 'L' && side != 'R' && side != 'P'))
        return 0;
    *kind = side == 'P' ? 2 : side == 'R';
    return 1;
}
static int find(const GuiBt* bt, const uint8_t address[6]) {
    for (int i = 0; i < bt->count; i++)
        if (!memcmp(bt->dev[i].address, address, 6)) return i;
    return -1;
}
static GuiBtDevice* add(GuiBt* bt, const uint8_t address[6]) {
    GuiBtDevice* d;
    if (bt->count >= GUI_BT_MAX) return NULL;
    d = &bt->dev[bt->count++];
    memset(d, 0, sizeof *d);
    memcpy(d->address, address, 6);
    return d;
}
/* the next line of text at *at: its start, its length with the newline; NULL at the end */
static const char* next_line(const char* text, size_t size, size_t* at, size_t* n) {
    const char* start = text + *at;
    const char* end;
    if (*at >= size) return NULL;
    end = memchr(start, '\n', size - *at);
    *n = end ? (size_t)(end - start) + 1 : size - *at;
    *at += *n;
    return start;
}
int gui_bt_read(GuiBt* bt, const char* path) {
    size_t size = 0, at = 0, n;
    char* text = NULL;
    const char* line;
    GuiBt was = *bt;
    SnxConfigOthers others = {0};
    int known = 0, number = 0;
    bt->said[0] = 0;
    if (path && snx_config_read(path, &text, &size, bt->said, sizeof bt->said) < 0) return -1;
    /* a line that names no controller, another version's or a hand's: kept by a write (sync, forget), named */
    for (size_t k = 0; text && (line = next_line(text, size, &k, &n)) != NULL;) {
        uint8_t address[6];
        int right;
        char serial[17];
        ++number;
        if (line[strspn(line, " \t\r\n")] && !entry(line, n, address, &right, serial))
            snx_config_other_key(&others, "", "", number);
    }
    if (path) snx_config_others_said(&others, path, bt->said, sizeof bt->said);
    bt->count = 0;
    /* the known ones first, in the file's order, keeping what the scan heard of them */
    while (text && known < GUI_BT_KNOWN && (line = next_line(text, size, &at, &n))) {
        uint8_t address[6];
        int right, w;
        char serial[17];
        GuiBtDevice* d;
        if (!entry(line, n, address, &right, serial) || find(bt, address) >= 0) continue;
        w = find(&was, address);
        d = add(bt, address);
        if (!d) break;
        if (w >= 0) *d = was.dev[w];
        d->known = 1;
        d->right = right == 1;
        d->pro = right == 2;
        snprintf(d->serial, sizeof d->serial, "%s", serial);
        known++;
    }
    /* then the others heard */
    for (int i = 0; i < was.count; i++)
        if (was.dev[i].heard != GUI_BT_AWAY && find(bt, was.dev[i].address) < 0 && bt->count < GUI_BT_MAX) {
            bt->dev[bt->count] = was.dev[i];
            bt->dev[bt->count++].known = 0;
        }
    free(text);
    return known;
}
int gui_bt_heard(GuiBt* bt, const SnxBleAdvert* advert, uint64_t now_ms) {
    SnxBleJoyCon2 j;
    GuiBtDevice* d;
    int i;
    if (!snx_ble_joycon2(advert, &j)) return 0;
    i = find(bt, advert->address);
    d = i >= 0 ? &bt->dev[i] : add(bt, advert->address);
    if (!d) return 1;
    if (!d->known) { /* a known one's kind is joycon2.txt's */
        d->right = j.right;
        d->pro = j.pro;
    }
    d->heard = j.syncing ? GUI_BT_WAITING : GUI_BT_NEARBY;
    d->heard_ms = now_ms ? now_ms : 1;
    d->rssi = advert->rssi;
    return 1;
}
void gui_bt_tick(GuiBt* bt, uint64_t now_ms) {
    int i, kept = 0;
    for (i = 0; i < bt->count; i++) {
        GuiBtDevice* d = &bt->dev[i];
        if (d->heard != GUI_BT_AWAY && now_ms >= d->heard_ms + GUI_BT_HEARD_MS) d->heard = GUI_BT_AWAY;
        if (d->said != GUI_BT_AWAY && now_ms >= d->said_ms + GUI_BT_SAID_MS) d->said = GUI_BT_AWAY;
        if (d->known || d->heard != GUI_BT_AWAY || d->said != GUI_BT_AWAY) bt->dev[kept++] = *d;
    }
    bt->count = kept;
}
int gui_bt_state(const GuiBtDevice* d) { return d->said != GUI_BT_AWAY ? d->said : d->heard; }
int gui_bt_said(GuiBt* bt, const char* text, uint64_t now_ms) {
    static const char* const states[] = {"connecting", "ready", "failed"};
    size_t size = text ? strlen(text) : 0, at = 0, n;
    const char* line;
    int taken = 0;
    while ((line = next_line(text, size, &at, &n))) {
        char word[16] = "";
        int mv = 0, s, i;
        uint8_t address[6];
        char copy[96];
        if (n >= sizeof copy) continue;
        memcpy(copy, line, n);
        copy[n] = 0;
        if (n < 19 || !snx_ble_address_parse(copy, address) || sscanf(copy + 17, " %15s %d", word, &mv) < 1) continue;
        for (s = 0; s < 3 && strcmp(word, states[s]); s++) {}
        i = find(bt, address);
        if (s == 3 || i < 0) continue; /* the runtime connects only known ones: a row the page has */
        bt->dev[i].said = GUI_BT_CONNECTING + s;
        bt->dev[i].said_ms = now_ms ? now_ms : 1;
        bt->dev[i].battery_mv = mv > 0 ? mv : 0;
        taken++;
    }
    return taken;
}
/* text written to path whole, then moved over it: 0, else -1 with why */
static int write_whole(const char* path, const char* text, size_t n, char* why, size_t size) {
    char* tmp = snx_format("%s.new", path);
    int failed = !tmp || snx_write_file(tmp, text, n) || plat_replace_file(tmp, path);
    if (failed) {
        snprintf(why, size, "%s could not be written", path);
        if (tmp) remove(tmp);
    }
    free(tmp);
    return failed ? -1 : 0;
}
int gui_bt_sync(GuiBt* bt, int i, const char* path, char* why, size_t size) {
    size_t n = 0, at = 0, len;
    char *text, *out, address[18];
    const char* line;
    int known = 0, failed;
    GuiBtDevice d;
    if (i < 0 || i >= bt->count || bt->dev[i].heard != GUI_BT_WAITING) {
        snprintf(why, size, "%s", "it is not waiting to sync: hold its sync button");
        return -1;
    }
    d = bt->dev[i];
    if (gui_bt_read(bt, path) < 0) {
        snprintf(why, size, "%s does not read", path);
        return -1;
    }
    i = find(bt, d.address);
    if (i >= 0 && bt->dev[i].known) {
        snprintf(why, size, "%s", "it is known already");
        return -1;
    }
    text = (char*)snx_read_file(path, &n);
    while (text && (line = next_line(text, n, &at, &len))) {
        uint8_t a[6];
        int right;
        char serial[17];
        known += entry(line, len, a, &right, serial);
    }
    if (known >= GUI_BT_KNOWN) {
        snprintf(why, size, "%d controllers are known already, the most a game reads: forget one first", known);
        free(text);
        return -1;
    }
    snx_ble_address_text(d.address, address);
    out = snx_format("%.*s%s%s %c\n", (int)n, text ? text : "", n && text[n - 1] != '\n' ? "\n" : "", address,
                     d.pro ? 'P' : d.right ? 'R' : 'L');
    failed = !out || write_whole(path, out, strlen(out), why, size);
    free(out);
    free(text);
    gui_bt_read(bt, path);
    return failed ? -1 : 0;
}
int gui_bt_forget(GuiBt* bt, int i, const char* path, char* why, size_t size) {
    size_t n = 0, at = 0, len, kept = 0;
    char* text;
    const char* line;
    uint8_t address[6];
    int failed;
    if (i < 0 || i >= bt->count || !bt->dev[i].known) {
        snprintf(why, size, "%s", "it is not known");
        return -1;
    }
    memcpy(address, bt->dev[i].address, 6);
    text = (char*)snx_read_file(path, &n);
    if (!text) {
        snprintf(why, size, "%s does not read", path);
        return -1;
    }
    /* every line kept but those of its address, in place */
    while ((line = next_line(text, n, &at, &len))) {
        uint8_t a[6];
        int right;
        char serial[17];
        if (entry(line, len, a, &right, serial) && !memcmp(a, address, 6)) continue;
        memmove(text + kept, line, len);
        kept += len;
    }
    failed = write_whole(path, text, kept, why, size);
    free(text);
    gui_bt_read(bt, path);
    return failed;
}
