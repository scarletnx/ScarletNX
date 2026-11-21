/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Bluetooth LE adverts, as a scan hears them: the peripherals nearby, each one's address and the manufacturer data of
   one company. A scan only listens: it never connects, pairs or trusts a device. Its backends:
   - the system's (snx_ble_system_open, its own library, snx_ble_system): Linux's BlueZ over D-Bus (ble_bluez.c);
     Windows' WinRT BluetoothLEAdvertisementWatcher designed in ble_none.c, not written; none elsewhere;
   - a fake one (snx_ble_fake_open), whose adverts the caller gives: the tests'.
   Not thread safe: one thread opens, polls and closes a scan */
#ifndef SNX_PLATFORM_BLE_SCAN_H
#define SNX_PLATFORM_BLE_SCAN_H
#include <stddef.h>
#include <stdint.h>

#define SNX_BLE_DATA 32 /* the manufacturer data kept, after the company id */
typedef struct {
    uint8_t address[6]; /* as written, the most significant first */
    int random;         /* the address type: 1 random, 0 public, -1 not said */
    int rssi;           /* dBm; 0 not said */
    char name[32];      /* the advertised name, UTF-8; "" none */
    uint16_t company;   /* the manufacturer data's company id */
    uint8_t data[SNX_BLE_DATA];
    size_t size;
} SnxBleAdvert;

typedef struct SnxBleScan SnxBleScan;
/* a backend's own: what each call of the scan does */
typedef struct {
    const char* name;
    /* listening for LE adverts with company's manufacturer data on: 0, else nonzero with why */
    int (*start)(void* self, uint16_t company, char* why, size_t size);
    /* the adverts heard since the last poll, up to max, without waiting: how many, -1 when the backend failed (why) */
    int (*poll)(void* self, SnxBleAdvert* out, int max, char* why, size_t size);
    void (*stop)(void* self); /* listening off: a connection that follows isn't held up by it */
    void (*close)(void* self);
} SnxBleBackend;
/* a scan over a backend and its state; NULL when out of memory */
SnxBleScan* snx_ble_scan_make(const SnxBleBackend* backend, void* self);
const char* snx_ble_scan_name(const SnxBleScan* scan);
int snx_ble_scan_start(SnxBleScan* scan, uint16_t company, char* why, size_t size);
int snx_ble_scan_listening(const SnxBleScan* scan);
int snx_ble_scan_poll(SnxBleScan* scan, SnxBleAdvert* out, int max, char* why, size_t size);
void snx_ble_scan_stop(SnxBleScan* scan);
void snx_ble_scan_close(SnxBleScan* scan); /* stopped first; NULL does nothing */

/* the system's scan, NULL with why when it has none (no Bluetooth, no BlueZ, no libsystemd, a system without a
   backend). Opening it asks nothing of the adapter: start does */
SnxBleScan* snx_ble_system_open(char* why, size_t size);

/* the fake scan: the adverts snx_ble_fake_hear gives, which a poll returns while it listens (heard while it doesn't,
   they are dropped, as a radio's). fail: start refuses with that reason ("" to start) */
SnxBleScan* snx_ble_fake_open(const char* fail);
int snx_ble_fake_hear(SnxBleScan* scan, const SnxBleAdvert* advert); /* 0, else -1 with no room (64 at most) */
int snx_ble_fake_starts(const SnxBleScan* scan);                     /* how many times it was started */

/* ---- a Joy-Con 2's or a Pro Controller 2's advert (the protocol: src/nx/runtime/platform/joycon2_proto.h, whose
   jc2_advert this agrees with, tests/joycon2.c): the console maker's company id, then 01 00 03, the vendor id 057E, the
   product id (2067 left, 2066 right, 2069 the Pro Controller 2), 00 01, flags (bit 7: it asks its console to wake),
   and the address of the console it is paired with, all zero while it waits to sync */
#define SNX_BLE_NINTENDO 0x0553u
typedef struct {
    int right;   /* the product id is the right Joy-Con's */
    int pro;     /* the product id is the Pro Controller 2's */
    int syncing; /* it names no console: its sync button is held */
    int wake;    /* its flags ask the console to wake */
} SnxBleJoyCon2;
int snx_ble_joycon2(const SnxBleAdvert* advert, SnxBleJoyCon2* out); /* 0 when it is none of them */
/* "AA:BB:CC:DD:EE:FF" into out (18 bytes), and back: 1 when s starts with one */
void snx_ble_address_text(const uint8_t address[6], char out[18]);
int snx_ble_address_parse(const char* s, uint8_t address[6]);
#endif
