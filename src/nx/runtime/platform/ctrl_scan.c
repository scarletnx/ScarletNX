/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/platform/ctrl_scan.h"
#include <stdio.h>
#include <string.h>

int ctrl_scan_open(CtrlScan* s, SnxBleScan* scan, const char* why) {
    *s = (CtrlScan){0};
    if (!scan) {
        snprintf(s->why, sizeof s->why, "%s", why && why[0] ? why : "no Bluetooth scan on this system");
        return -1;
    }
    if (snx_ble_scan_start(scan, SNX_BLE_NINTENDO, s->why, sizeof s->why)) {
        snx_ble_scan_close(scan);
        if (!s->why[0]) snprintf(s->why, sizeof s->why, "the Bluetooth scan did not start");
        return -1;
    }
    s->scan = scan;
    return 0;
}
/* the controller heard at address, a new entry when there is room: NULL for none */
static CtrlHeard* heard_at(CtrlScan* s, const u8 address[6]) {
    for (int i = 0; i < s->n; i++)
        if (!memcmp(s->heard[i].address, address, 6)) return &s->heard[i];
    if (s->n == CTRL_FOUND) return NULL;
    s->heard[s->n] = (CtrlHeard){0};
    memcpy(s->heard[s->n].address, address, 6);
    return &s->heard[s->n++];
}
/* listening stopped, then the connect asked for */
static void connect(CtrlScan* s, const CtrlScanHost* host, CtrlHeard* h) {
    h->connected = 1;
    h->tries++;
    if (s->scan) snx_ble_scan_stop(s->scan);
    s->stopped = 1;
    host->connect(host->user, h->address, h->random, h->found.pro ? 2 : h->found.right, h->found.syncing);
}
int ctrl_scan_poll(CtrlScan* s, const CtrlScanHost* host, CtrlFound* found) {
    SnxBleAdvert adverts[16];
    int n = 0, i;
    if (s->scan && s->stopped && !host->busy(host->user)) { /* the connects done: listening again */
        int left = 0;
        s->stopped = 0;
        /* a connect that failed: asked again when the controller is heard again, while it has tries left */
        for (i = 0; i < s->n; i++) {
            CtrlHeard* h = &s->heard[i];
            if (h->connected && host->has && !host->has(host->user, h->address)) h->connected = 0;
            if (h->found.held) continue; /* only its Connect item takes it back */
            if (h->found.known && !h->connected && h->tries < CTRL_SCAN_TRIES) left = 1;
            if (h->found.known && !h->connected && h->tries >= CTRL_SCAN_TRIES) s->gave_up = 1;
        }
        if (s->gave_up && !left) { /* the host's own tries in turn take the known ones back */
            snprintf(s->why, sizeof s->why, "the known Joy-Con 2 were heard but did not connect: they are tried in "
                                           "turn now, press a button on each");
            snx_ble_scan_close(s->scan);
            s->scan = NULL;
        } else if (snx_ble_scan_start(s->scan, SNX_BLE_NINTENDO, s->why, sizeof s->why)) {
            snx_ble_scan_close(s->scan);
            s->scan = NULL;
            if (!s->why[0]) snprintf(s->why, sizeof s->why, "the Bluetooth scan did not start again");
        }
    }
    if (s->scan) {
        n = snx_ble_scan_poll(s->scan, adverts, 16, s->why, sizeof s->why);
        if (n < 0) { /* the backend failed: the dialog says why, nothing more is heard */
            snx_ble_scan_close(s->scan);
            s->scan = NULL;
            n = 0;
        }
    }
    for (i = 0; i < n; i++) {
        SnxBleJoyCon2 jc;
        CtrlHeard* h;
        if (!snx_ble_joycon2(&adverts[i], &jc) || !(h = heard_at(s, adverts[i].address))) continue;
        h->random = adverts[i].random > 0;
        h->found.right = jc.right;
        h->found.pro = jc.pro;
        h->found.syncing = jc.syncing;
        h->found.known = host->known(host->user, h->address);
        h->found.held = h->found.known && host->held && host->held(host->user, h->address, jc.wake);
        if (h->found.held) continue;
        if (h->found.known && !h->connected && h->tries < CTRL_SCAN_TRIES) connect(s, host, h); /* known: at once */
    }
    for (i = 0; i < s->n; i++) found[i] = s->heard[i].found;
    return s->n;
}
void ctrl_scan_take(CtrlScan* s, const CtrlScanHost* host, int index) {
    CtrlHeard* h;
    if (index < 0 || index >= s->n) return;
    h = &s->heard[index];
    h->found.asked = 1;
    if (h->found.held) { /* taken back: the host's connect ends its hold */
        h->found.held = 0;
        h->connected = 0;
        h->tries = 0;
    }
    if (!h->connected) connect(s, host, h);
}
void ctrl_scan_close(CtrlScan* s) {
    snx_ble_scan_close(s->scan); /* stopped first */
    *s = (CtrlScan){0};
}
