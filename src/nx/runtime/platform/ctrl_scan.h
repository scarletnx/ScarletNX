/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Bluetooth scan of the controller applet's dialog: it listens only while the dialog shows, for the Joy-Con 2
   adverts (ble_scan.h), and stops when the dialog closes. A Joy-Con 2 the runtime knows is connected as soon as it is
   heard, with no wait between tries. A new one, waiting to sync, is shown in the dialog and connected only once the
   user confirms it. The connecting is the caller's (the Joy-Con 2 host, joycon2.h): the scan only says which address
   to connect to. Listening stops for each connect, as an LE scan holds a new LE connection up on many adapters, and
   starts again once the host has no connect going on. Not thread safe: the main thread opens, polls and closes it */
#ifndef SNX_CTRL_SCAN_H
#define SNX_CTRL_SCAN_H
#include "common/cpu/aarch64/cpu.h"
#include "common/platform/ble_scan.h"
#include "nx/runtime/platform/ctrl_applet.h"

typedef struct {
    u8 address[6];
    int random;
    CtrlFound found; /* as the dialog shows it */
    int connected;   /* the connect was asked for it, and not seen to fail yet */
    int tries;       /* the connects asked for it while the dialog shows */
} CtrlHeard;
/* the connects a known controller gets while the dialog shows, before the scan gives the known ones back to the
   host's own tries */
#define CTRL_SCAN_TRIES 4
typedef struct {
    SnxBleScan* scan;      /* NULL when closed */
    char why[128];         /* why it doesn't listen (no Bluetooth, BlueZ missing, the adapter off), "" when it does */
    CtrlHeard heard[CTRL_FOUND];
    int n;
    int stopped; /* listening stopped for a connect */
    int gave_up; /* every known controller heard used its tries: the scan stopped connecting them */
} CtrlScan;
/* what the caller gives the scan: whether the runtime knows address, the connect of a controller (side: 0 a left
   Joy-Con 2, 1 a right one, 2 a Pro Controller 2), whether a connect still goes on, whether the host holds a
   controller (NULL: a connect asked for counts as held), and whether a known one heard (its advert's wake flag) is
   held off since the dialog disconnected it (NULL: none is), which the scan then doesn't connect */
typedef struct {
    int (*known)(void* user, const u8 address[6]);
    void (*connect)(void* user, const u8 address[6], int random, int side, int syncing);
    int (*busy)(void* user);
    void* user;
    int (*has)(void* user, const u8 address[6]);
    int (*held)(void* user, const u8 address[6], int wake);
} CtrlScanHost;

/* listening on scan (the system's, or a test's fake), taken over: 0, else -1 with why said. NULL scan: why given */
int ctrl_scan_open(CtrlScan* s, SnxBleScan* scan, const char* why);
/* the adverts heard since the last poll: known controllers connected at once, the rest kept for the dialog. A connect
   that failed is asked again when the controller is heard again, CTRL_SCAN_TRIES times at most. The controllers
   heard, as the dialog shows them, into found (CTRL_FOUND at most): how many */
int ctrl_scan_poll(CtrlScan* s, const CtrlScanHost* host, CtrlFound* found);
/* the user confirmed the new controller the dialog shows at index, or took back one held off: it is connected */
void ctrl_scan_take(CtrlScan* s, const CtrlScanHost* host, int index);
void ctrl_scan_close(CtrlScan* s); /* listening off, the scan closed and the controllers heard forgotten */
#endif
