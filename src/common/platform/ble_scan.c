/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A scan over its backend (ble_scan.h), the fake backend, and the Joy-Con 2's advert. No OS here */
#include "common/platform/ble_scan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct SnxBleScan {
    const SnxBleBackend* backend;
    void* self;
    int listening;
};
SnxBleScan* snx_ble_scan_make(const SnxBleBackend* backend, void* self) {
    SnxBleScan* scan = (SnxBleScan*)calloc(1, sizeof *scan);
    if (!scan) return NULL;
    scan->backend = backend;
    scan->self = self;
    return scan;
}
const char* snx_ble_scan_name(const SnxBleScan* scan) { return scan->backend->name; }
int snx_ble_scan_start(SnxBleScan* scan, uint16_t company, char* why, size_t size) {
    if (scan->listening) return 0;
    if (size) why[0] = 0;
    if (scan->backend->start(scan->self, company, why, size)) return -1;
    scan->listening = 1;
    return 0;
}
int snx_ble_scan_listening(const SnxBleScan* scan) { return scan->listening; }
int snx_ble_scan_poll(SnxBleScan* scan, SnxBleAdvert* out, int max, char* why, size_t size) {
    int n;
    if (!scan->listening) return 0;
    n = scan->backend->poll(scan->self, out, max, why, size);
    if (n < 0) snx_ble_scan_stop(scan); /* a backend that failed listens no more */
    return n;
}
void snx_ble_scan_stop(SnxBleScan* scan) {
    if (!scan->listening) return;
    scan->backend->stop(scan->self);
    scan->listening = 0;
}
void snx_ble_scan_close(SnxBleScan* scan) {
    if (!scan) return;
    snx_ble_scan_stop(scan);
    scan->backend->close(scan->self);
    free(scan);
}

/* ---- the fake backend */
#define FAKE_HEARD 64
typedef struct {
    char fail[128];
    int listening, starts, count;
    SnxBleAdvert heard[FAKE_HEARD];
} Fake;
static int fake_start(void* self, uint16_t company, char* why, size_t size) {
    Fake* f = (Fake*)self;
    (void)company;
    if (f->fail[0]) {
        snprintf(why, size, "%s", f->fail);
        return -1;
    }
    f->listening = 1;
    f->starts++;
    f->count = 0;
    return 0;
}
static int fake_poll(void* self, SnxBleAdvert* out, int max, char* why, size_t size) {
    Fake* f = (Fake*)self;
    int n = f->count < max ? f->count : max;
    (void)why;
    (void)size;
    memcpy(out, f->heard, (size_t)n * sizeof *out);
    memmove(f->heard, f->heard + n, (size_t)(f->count - n) * sizeof *f->heard);
    f->count -= n;
    return n;
}
static void fake_stop(void* self) {
    Fake* f = (Fake*)self;
    f->listening = 0;
    f->count = 0;
}
static void fake_close(void* self) { free(self); }
static const SnxBleBackend fake_backend = {"fake", fake_start, fake_poll, fake_stop, fake_close};
SnxBleScan* snx_ble_fake_open(const char* fail) {
    Fake* f = (Fake*)calloc(1, sizeof *f);
    SnxBleScan* scan = f ? snx_ble_scan_make(&fake_backend, f) : NULL;
    if (!scan) {
        free(f);
        return NULL;
    }
    snprintf(f->fail, sizeof f->fail, "%s", fail ? fail : "");
    return scan;
}
int snx_ble_fake_hear(SnxBleScan* scan, const SnxBleAdvert* advert) {
    Fake* f = (Fake*)scan->self;
    if (scan->backend != &fake_backend) return -1;
    if (!f->listening) return 0; /* nobody listened */
    if (f->count >= FAKE_HEARD) return -1;
    f->heard[f->count++] = *advert;
    return 0;
}
int snx_ble_fake_starts(const SnxBleScan* scan) {
    return scan->backend == &fake_backend ? ((const Fake*)scan->self)->starts : 0;
}

/* ---- a Joy-Con 2's advert */
int snx_ble_joycon2(const SnxBleAdvert* a, SnxBleJoyCon2* out) {
    const uint8_t* p = a->data;
    unsigned product;
    int i;
    if (a->company != SNX_BLE_NINTENDO || a->size < 16 || p[0] != 0x01 || p[1] != 0x00 || p[2] != 0x03 ||
        (p[3] | p[4] << 8) != 0x057e)
        return 0;
    product = (unsigned)(p[5] | p[6] << 8);
    if (product != 0x2066u && product != 0x2067u && product != 0x2069u) return 0;
    out->right = product == 0x2066u;
    out->pro = product == 0x2069u;
    out->wake = (p[9] & 0x80) != 0;
    out->syncing = 1;
    for (i = 10; i < 16; i++) out->syncing &= !p[i];
    return 1;
}
void snx_ble_address_text(const uint8_t a[6], char out[18]) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
}
static int hex(char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                  : -1;
}
int snx_ble_address_parse(const char* s, uint8_t address[6]) {
    uint8_t a[6];
    int i;
    for (i = 0; i < 6; i++, s += 3) {
        int hi = hex(s[0]), lo = hi < 0 ? -1 : hex(s[1]);
        if (hi < 0 || lo < 0 || (i < 5 && s[2] != ':')) return 0;
        a[i] = (uint8_t)(hi << 4 | lo);
    }
    memcpy(address, a, 6);
    return 1;
}
