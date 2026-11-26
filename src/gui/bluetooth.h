/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Controllers > Bluetooth's controllers, without SDL: the Joy-Con 2 and Pro Controller 2 the runtime knows, and those
   a scan hears (common/platform/ble_scan.h). The runtime, another process, connects them itself
   (src/nx/runtime/platform/joycon2.c, setting joycon2). The two share joycon2.txt of the user's config folder, a line
   each, "AA:BB:CC:DD:EE:FF L" (R for a right Joy-Con 2, P for a Pro Controller 2), a serial after it when the runtime
   wrote it. Sync adds a line for a controller heard waiting to sync
   (its sync button held), Forget takes its lines out; the other lines stay as they are. Nothing here connects, pairs
   or trusts a controller */
#ifndef SNX_GUI_BLUETOOTH_H
#define SNX_GUI_BLUETOOTH_H
#include <stddef.h>
#include <stdint.h>
#include "common/platform/ble_scan.h"

#define GUI_BT_MAX       24    /* the rows */
#define GUI_BT_KNOWN     16    /* joycon2.c's JC2_KNOWN: the runtime reads no more */
#define GUI_BT_HEARD_MS  10000 /* a controller not heard this long is no longer nearby */
#define GUI_BT_SEARCH_MS 30000 /* a search's length: Search starts it, and it stops by itself */
#define GUI_BT_SAID_MS   5000  /* the runtime's states this old are no longer its */
/* a controller's state: the scan's (away, nearby, waiting to sync), or the runtime's while a game connects it */
enum { GUI_BT_AWAY, GUI_BT_NEARBY, GUI_BT_WAITING, GUI_BT_CONNECTING, GUI_BT_READY, GUI_BT_FAILED };
typedef struct {
    uint8_t address[6];
    int right;         /* a right Joy-Con 2, else a left one */
    int pro;           /* a Pro Controller 2 */
    char serial[17];   /* "" not known */
    int known;         /* joycon2.txt has it */
    int heard;         /* the scan's state: GUI_BT_AWAY, GUI_BT_NEARBY or GUI_BT_WAITING */
    uint64_t heard_ms; /* its last advert, 0 none */
    int rssi;          /* that advert's, dBm; 0 not said */
    int said;          /* the runtime's state, GUI_BT_AWAY for none */
    uint64_t said_ms;
    int battery_mv; /* the runtime's; 0 not said */
} GuiBtDevice;
typedef struct {
    GuiBtDevice dev[GUI_BT_MAX]; /* the known ones first, as joycon2.txt has them, then the others as heard */
    int count;
    /* what the last read says: why joycon2.txt cannot be read, or its lines that name no controller, which stay
       (config_file.h), once a run; "" for nothing */
    char said[512];
} GuiBt;

/* joycon2.txt at path read again: the rows it names known (with their side and serial), the others not; a row neither
   known nor heard goes. How many it knows; -1 when it is there but does not read, said in bt->said */
int gui_bt_read(GuiBt* bt, const char* path);
/* an advert heard at now: a Joy-Con 2's updates its row, or adds one (none past GUI_BT_MAX). 1 when it was one */
int gui_bt_heard(GuiBt* bt, const SnxBleAdvert* advert, uint64_t now_ms);
/* time passing: a row not heard for GUI_BT_HEARD_MS is away, and goes unless it is known; the runtime's states go
   after GUI_BT_SAID_MS */
void gui_bt_tick(GuiBt* bt, uint64_t now_ms);
/* the state a row shows: the runtime's while it says one, else the scan's */
int gui_bt_state(const GuiBtDevice* d);
/* the runtime's states, its joycon2-state.txt's text (a line each: "AA:BB:CC:DD:EE:FF connecting|ready|failed [mV]"),
   said at now. NOT WRITTEN by the runtime yet: the page reads the file when it is there. How many lines it took */
int gui_bt_said(GuiBt* bt, const char* text, uint64_t now_ms);
/* row i, waiting to sync, added to joycon2.txt at path (read again first, written whole and then moved over it). 0,
   else -1 with why: not waiting, known already, GUI_BT_KNOWN known, or the file not written */
int gui_bt_sync(GuiBt* bt, int i, const char* path, char* why, size_t size);
/* row i's lines taken out of joycon2.txt at path: 0, else -1 with why (not known, the file not written) */
int gui_bt_forget(GuiBt* bt, int i, const char* path, char* why, size_t size);
#endif
