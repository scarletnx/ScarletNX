/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The system's scan on Linux (ble_scan.h): BlueZ's D-Bus API, which an unprivileged user may call, through
   libsystemd's sd-bus opened at run time (no link dependency; its declarations here, as sd-bus.h has them since
   systemd 237). Start: the first org.bluez.Adapter1 of GetManagedObjects, SetDiscoveryFilter {Transport: "le",
   DuplicateData: true}, StartDiscovery. Each org.bluez.Device1 that InterfacesAdded reports, or whose RSSI or
   ManufacturerData PropertiesChanged says, with the company's manufacturer data, is an advert. Devices BlueZ kept from
   before the start are only remembered (their data may be old) until a signal says they were heard. Stop:
   StopDiscovery (closing the bus would end it too: BlueZ ends a client's discovery when it leaves the bus).
   Never Connect, Pair, Trust or RemoveDevice: the runtime connects with its own socket (joycon2_link.c), unpaired.
   NOT RUN: nothing here was tried on a system bus (the tests link a fake in its place) */
#include "common/platform/ble_scan.h"
#include "common/platform/dl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- sd-bus, the part used */
typedef struct sd_bus sd_bus;
typedef struct sd_bus_message sd_bus_message;
typedef struct sd_bus_slot sd_bus_slot;
typedef struct {
    const char* name;
    const char* message;
    int need_free;
} sd_bus_error;
typedef int (*sd_bus_message_handler_t)(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
typedef struct {
    void* lib;
    int (*open_system)(sd_bus** ret);
    sd_bus* (*flush_close_unref)(sd_bus* bus);
    int (*call_method)(sd_bus* bus, const char* destination, const char* path, const char* interface,
                       const char* member, sd_bus_error* ret_error, sd_bus_message** reply, const char* types, ...);
    int (*new_method_call)(sd_bus* bus, sd_bus_message** m, const char* destination, const char* path,
                           const char* interface, const char* member);
    int (*open_container)(sd_bus_message* m, char type, const char* contents);
    int (*close_container)(sd_bus_message* m);
    int (*append)(sd_bus_message* m, const char* types, ...);
    int (*call)(sd_bus* bus, sd_bus_message* m, uint64_t usec, sd_bus_error* ret_error, sd_bus_message** reply);
    sd_bus_message* (*message_unref)(sd_bus_message* m);
    int (*enter_container)(sd_bus_message* m, char type, const char* contents);
    int (*exit_container)(sd_bus_message* m);
    int (*read)(sd_bus_message* m, const char* types, ...);
    int (*read_array)(sd_bus_message* m, char type, const void** ptr, size_t* size);
    int (*skip)(sd_bus_message* m, const char* types);
    int (*peek_type)(sd_bus_message* m, char* type, const char** contents);
    const char* (*get_path)(sd_bus_message* m);
    int (*match_signal)(sd_bus* bus, sd_bus_slot** ret, const char* sender, const char* path, const char* interface,
                        const char* member, sd_bus_message_handler_t callback, void* userdata);
    sd_bus_slot* (*slot_unref)(sd_bus_slot* slot);
    int (*process)(sd_bus* bus, sd_bus_message** r);
    void (*error_free)(sd_bus_error* e);
} SdBus;
static int sd_load(SdBus* s, char* why, size_t size) {
    static const char* const names[] = {"sd_bus_open_system",
                                        "sd_bus_flush_close_unref",
                                        "sd_bus_call_method",
                                        "sd_bus_message_new_method_call",
                                        "sd_bus_message_open_container",
                                        "sd_bus_message_close_container",
                                        "sd_bus_message_append",
                                        "sd_bus_call",
                                        "sd_bus_message_unref",
                                        "sd_bus_message_enter_container",
                                        "sd_bus_message_exit_container",
                                        "sd_bus_message_read",
                                        "sd_bus_message_read_array",
                                        "sd_bus_message_skip",
                                        "sd_bus_message_peek_type",
                                        "sd_bus_message_get_path",
                                        "sd_bus_match_signal",
                                        "sd_bus_slot_unref",
                                        "sd_bus_process",
                                        "sd_bus_error_free"};
    void* f[sizeof names / sizeof names[0]];
    size_t i;
    s->lib = plat_dl_open("libsystemd.so.0");
    if (!s->lib) {
        snprintf(why, size, "no libsystemd.so.0, which BlueZ is reached through: %s", plat_dl_error());
        return -1;
    }
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!(f[i] = plat_dl_sym(s->lib, names[i]))) {
            snprintf(why, size, "libsystemd.so.0 has no %s (systemd 237 or later has)", names[i]);
            plat_dl_close(s->lib);
            s->lib = NULL;
            return -1;
        }
    /* a symbol's address as the function it is (POSIX: dlsym's pointers convert) */
    memcpy(&s->open_system, &f[0], sizeof f[0]);
    memcpy(&s->flush_close_unref, &f[1], sizeof f[0]);
    memcpy(&s->call_method, &f[2], sizeof f[0]);
    memcpy(&s->new_method_call, &f[3], sizeof f[0]);
    memcpy(&s->open_container, &f[4], sizeof f[0]);
    memcpy(&s->close_container, &f[5], sizeof f[0]);
    memcpy(&s->append, &f[6], sizeof f[0]);
    memcpy(&s->call, &f[7], sizeof f[0]);
    memcpy(&s->message_unref, &f[8], sizeof f[0]);
    memcpy(&s->enter_container, &f[9], sizeof f[0]);
    memcpy(&s->exit_container, &f[10], sizeof f[0]);
    memcpy(&s->read, &f[11], sizeof f[0]);
    memcpy(&s->read_array, &f[12], sizeof f[0]);
    memcpy(&s->skip, &f[13], sizeof f[0]);
    memcpy(&s->peek_type, &f[14], sizeof f[0]);
    memcpy(&s->get_path, &f[15], sizeof f[0]);
    memcpy(&s->match_signal, &f[16], sizeof f[0]);
    memcpy(&s->slot_unref, &f[17], sizeof f[0]);
    memcpy(&s->process, &f[18], sizeof f[0]);
    memcpy(&s->error_free, &f[19], sizeof f[0]);
    return 0;
}

/* ---- the backend */
#define BLUEZ_DEVICES 64
#define BLUEZ_HEARD   64
typedef struct {
    char path[96]; /* "" a free entry */
    SnxBleAdvert advert;
    int has_address, has_data;
} BluezDevice;
typedef struct {
    SdBus sd;
    sd_bus* bus;
    sd_bus_slot *added, *changed;
    char adapter[64];
    uint16_t company;
    int listening;
    unsigned evict; /* the slot taken next when every one holds a controller's data */
    BluezDevice dev[BLUEZ_DEVICES];
    SnxBleAdvert heard[BLUEZ_HEARD];
    int count;
} Bluez;
static void say(char* why, size_t size, const char* what, int r, const sd_bus_error* e) {
    snprintf(why, size, "BlueZ %s: %s", what, e && e->message ? e->message : e && e->name ? e->name : strerror(-r));
}
/* the device at path, else (make) a slot for it: a free one, else one of a device without the company's data (a phone,
   earbuds), else the next in turn, so a controller that shows up late always gets one. Its address from the path
   (".../dev_AA_BB_CC_DD_EE_FF") until its Address property says it: a slot taken back loses what it had, and a
   PropertiesChanged of its RSSI alone names no address */
static BluezDevice* device_at(Bluez* b, const char* path, int make) {
    BluezDevice *free_one = NULL, *other = NULL, *d;
    const char* dev = strstr(path, "/dev_");
    char text[18];
    for (int i = 0; i < BLUEZ_DEVICES; i++) {
        if (!strcmp(b->dev[i].path, path)) return &b->dev[i];
        if (!free_one && !b->dev[i].path[0]) free_one = &b->dev[i];
        if (!other && !b->dev[i].has_data) other = &b->dev[i];
    }
    if (!make || strlen(path) >= sizeof b->dev[0].path) return NULL;
    d = free_one ? free_one : other ? other : &b->dev[b->evict++ % BLUEZ_DEVICES];
    memset(d, 0, sizeof *d);
    snprintf(d->path, sizeof d->path, "%s", path);
    d->advert.random = -1;
    if (dev && strlen(dev + 5) >= 17) {
        snprintf(text, sizeof text, "%.17s", dev + 5);
        for (int i = 2; i < 17; i += 3) text[i] = ':';
        d->has_address = snx_ble_address_parse(text, d->advert.address);
    }
    return d;
}
/* the variant at m's position entered: its signature in *contents. >0 when it was */
static int enter_variant(Bluez* b, sd_bus_message* m, const char** contents) {
    char type;
    if (b->sd.peek_type(m, &type, contents) <= 0 || type != 'v') return 0;
    return b->sd.enter_container(m, 'v', *contents) > 0;
}
/* ManufacturerData's a{qv}, each v an ay: the company's into the advert */
static int read_data(Bluez* b, sd_bus_message* m, BluezDevice* d) {
    int found = 0;
    if (b->sd.enter_container(m, 'a', "{qv}") <= 0) return 0;
    while (b->sd.enter_container(m, 'e', "qv") > 0) {
        uint16_t company = 0;
        const char* contents = NULL;
        if (b->sd.read(m, "q", &company) > 0 && enter_variant(b, m, &contents)) {
            const void* bytes = NULL;
            size_t n = 0;
            if (contents && !strcmp(contents, "ay") && b->sd.read_array(m, 'y', &bytes, &n) >= 0 &&
                company == b->company) {
                d->advert.company = company;
                d->advert.size = n < SNX_BLE_DATA ? n : SNX_BLE_DATA;
                if (d->advert.size) memcpy(d->advert.data, bytes, d->advert.size);
                d->has_data = found = 1;
            } else if (!(contents && !strcmp(contents, "ay")))
                b->sd.skip(m, contents);
            b->sd.exit_container(m);
        }
        b->sd.exit_container(m);
    }
    b->sd.exit_container(m);
    return found;
}
/* a Device1's a{sv} into d: 1 when it says the device was heard (its RSSI or its company's data) */
static int read_device(Bluez* b, sd_bus_message* m, BluezDevice* d) {
    int heard = 0;
    if (b->sd.enter_container(m, 'a', "{sv}") <= 0) return 0;
    while (b->sd.enter_container(m, 'e', "sv") > 0) {
        const char *key = NULL, *contents = NULL, *s = NULL;
        if (b->sd.read(m, "s", &key) > 0 && enter_variant(b, m, &contents)) {
            int16_t rssi;
            if (!strcmp(key, "Address") && !strcmp(contents, "s") && b->sd.read(m, "s", &s) > 0)
                d->has_address = snx_ble_address_parse(s, d->advert.address);
            else if (!strcmp(key, "AddressType") && !strcmp(contents, "s") && b->sd.read(m, "s", &s) > 0)
                d->advert.random = !strcmp(s, "random");
            else if (!strcmp(key, "Name") && !strcmp(contents, "s") && b->sd.read(m, "s", &s) > 0)
                snprintf(d->advert.name, sizeof d->advert.name, "%s", s);
            else if (!strcmp(key, "RSSI") && !strcmp(contents, "n") && b->sd.read(m, "n", &rssi) > 0) {
                d->advert.rssi = rssi;
                heard = 1;
            } else if (!strcmp(key, "ManufacturerData") && !strcmp(contents, "a{qv}"))
                heard |= read_data(b, m, d);
            else
                b->sd.skip(m, contents);
            b->sd.exit_container(m);
        }
        b->sd.exit_container(m);
    }
    b->sd.exit_container(m);
    return heard;
}
static void heard(Bluez* b, const BluezDevice* d) {
    if (!b->listening || !d->has_address || !d->has_data || b->count >= BLUEZ_HEARD) return;
    for (int i = 0; i < b->count; i++)
        if (!memcmp(b->heard[i].address, d->advert.address, 6)) {
            b->heard[i] = d->advert; /* the newest */
            return;
        }
    b->heard[b->count++] = d->advert;
}
/* a{sa{sv}}: the interfaces of the object at path; a Device1 into the cache, and heard when fresh. An Adapter1 named
   when there is none yet */
static void read_interfaces(Bluez* b, sd_bus_message* m, const char* path, int fresh) {
    if (b->sd.enter_container(m, 'a', "{sa{sv}}") <= 0) return;
    while (b->sd.enter_container(m, 'e', "sa{sv}") > 0) {
        const char* iface = NULL;
        if (b->sd.read(m, "s", &iface) > 0) {
            BluezDevice* d = !strcmp(iface, "org.bluez.Device1") ? device_at(b, path, 1) : NULL;
            if (d) {
                if (read_device(b, m, d) && fresh) heard(b, d);
            } else {
                if (!strcmp(iface, "org.bluez.Adapter1") && !b->adapter[0] && strlen(path) < sizeof b->adapter)
                    snprintf(b->adapter, sizeof b->adapter, "%s", path);
                b->sd.skip(m, "a{sv}");
            }
        }
        b->sd.exit_container(m);
    }
    b->sd.exit_container(m);
}
static int on_added(sd_bus_message* m, void* user, sd_bus_error* e) {
    Bluez* b = (Bluez*)user;
    const char* path = NULL;
    (void)e;
    if (b->sd.read(m, "o", &path) > 0 && path) read_interfaces(b, m, path, 1);
    return 0;
}
static int on_changed(sd_bus_message* m, void* user, sd_bus_error* e) {
    Bluez* b = (Bluez*)user;
    const char *iface = NULL, *path = b->sd.get_path(m);
    BluezDevice* d;
    (void)e;
    if (!path || b->sd.read(m, "s", &iface) <= 0 || !iface || strcmp(iface, "org.bluez.Device1")) return 0;
    d = device_at(b, path, 1);
    if (d && read_device(b, m, d)) heard(b, d);
    return 0;
}
/* GetManagedObjects: the adapter, and the devices BlueZ has kept, remembered but not heard */
static int managed(Bluez* b, char* why, size_t size) {
    sd_bus_error e = {NULL, NULL, 0};
    sd_bus_message* reply = NULL;
    int r = b->sd.call_method(b->bus, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", &e,
                              &reply, "");
    if (r < 0) {
        say(why, size, "has no objects (is bluetoothd running?)", r, &e);
        b->sd.error_free(&e);
        return -1;
    }
    if (b->sd.enter_container(reply, 'a', "{oa{sa{sv}}}") > 0) {
        while (b->sd.enter_container(reply, 'e', "oa{sa{sv}}") > 0) {
            const char* path = NULL;
            if (b->sd.read(reply, "o", &path) > 0 && path) read_interfaces(b, reply, path, 0);
            b->sd.exit_container(reply);
        }
        b->sd.exit_container(reply);
    }
    b->sd.message_unref(reply);
    if (!b->adapter[0]) {
        snprintf(why, size, "%s", "BlueZ has no Bluetooth adapter");
        return -1;
    }
    return 0;
}
/* SetDiscoveryFilter {Transport: "le", DuplicateData: true} */
static int filter(Bluez* b, char* why, size_t size) {
    sd_bus_error e = {NULL, NULL, 0};
    sd_bus_message *m = NULL, *reply = NULL;
    int r = b->sd.new_method_call(b->bus, &m, "org.bluez", b->adapter, "org.bluez.Adapter1", "SetDiscoveryFilter");
    if (r >= 0) r = b->sd.open_container(m, 'a', "{sv}");
    if (r >= 0) r = b->sd.open_container(m, 'e', "sv");
    if (r >= 0) r = b->sd.append(m, "s", "Transport");
    if (r >= 0) r = b->sd.open_container(m, 'v', "s");
    if (r >= 0) r = b->sd.append(m, "s", "le");
    if (r >= 0) r = b->sd.close_container(m);
    if (r >= 0) r = b->sd.close_container(m);
    if (r >= 0) r = b->sd.open_container(m, 'e', "sv");
    if (r >= 0) r = b->sd.append(m, "s", "DuplicateData");
    if (r >= 0) r = b->sd.open_container(m, 'v', "b");
    if (r >= 0) r = b->sd.append(m, "b", 1);
    if (r >= 0) r = b->sd.close_container(m);
    if (r >= 0) r = b->sd.close_container(m);
    if (r >= 0) r = b->sd.close_container(m);
    if (r >= 0) r = b->sd.call(b->bus, m, 0, &e, &reply);
    if (r < 0) say(why, size, "refused the LE filter", r, &e);
    b->sd.error_free(&e);
    if (reply) b->sd.message_unref(reply);
    if (m) b->sd.message_unref(m);
    return r < 0 ? -1 : 0;
}
static int bluez_start(void* self, uint16_t company, char* why, size_t size) {
    Bluez* b = (Bluez*)self;
    sd_bus_error e = {NULL, NULL, 0};
    int r;
    b->company = company;
    b->count = 0;
    if (!b->bus && (r = b->sd.open_system(&b->bus)) < 0) {
        b->bus = NULL;
        say(why, size, "is out of reach (no system bus)", r, NULL);
        return -1;
    }
    if (!b->added &&
        ((r = b->sd.match_signal(b->bus, &b->added, "org.bluez", NULL, "org.freedesktop.DBus.ObjectManager",
                                 "InterfacesAdded", on_added, b)) < 0 ||
         (r = b->sd.match_signal(b->bus, &b->changed, "org.bluez", NULL, "org.freedesktop.DBus.Properties",
                                 "PropertiesChanged", on_changed, b)) < 0)) {
        say(why, size, "signals cannot be heard", r, NULL);
        return -1;
    }
    if ((!b->adapter[0] && managed(b, why, size)) || filter(b, why, size)) return -1;
    r = b->sd.call_method(b->bus, "org.bluez", b->adapter, "org.bluez.Adapter1", "StartDiscovery", &e, NULL, "");
    if (r < 0) {
        say(why, size, "did not start a search (is Bluetooth on?)", r, &e);
        b->sd.error_free(&e);
        return -1;
    }
    b->listening = 1;
    return 0;
}
static int bluez_poll(void* self, SnxBleAdvert* out, int max, char* why, size_t size) {
    Bluez* b = (Bluez*)self;
    int r, n, rounds = 0;
    while ((r = b->sd.process(b->bus, NULL)) > 0 && ++rounds < 256) {} /* the signals queued, without waiting */
    if (r < 0) {
        say(why, size, "connection lost", r, NULL);
        return -1;
    }
    n = b->count < max ? b->count : max;
    memcpy(out, b->heard, (size_t)n * sizeof *out);
    memmove(b->heard, b->heard + n, (size_t)(b->count - n) * sizeof *b->heard);
    b->count -= n;
    return n;
}
static void bluez_stop(void* self) {
    Bluez* b = (Bluez*)self;
    sd_bus_error e = {NULL, NULL, 0};
    if (!b->listening) return;
    b->listening = 0;
    b->count = 0;
    b->sd.call_method(b->bus, "org.bluez", b->adapter, "org.bluez.Adapter1", "StopDiscovery", &e, NULL, "");
    b->sd.error_free(&e);
}
static void bluez_close(void* self) {
    Bluez* b = (Bluez*)self;
    if (b->added) b->sd.slot_unref(b->added);
    if (b->changed) b->sd.slot_unref(b->changed);
    if (b->bus) b->sd.flush_close_unref(b->bus); /* BlueZ ends what discovery the client left on */
    plat_dl_close(b->sd.lib);
    free(b);
}
static const SnxBleBackend bluez_backend = {"BlueZ", bluez_start, bluez_poll, bluez_stop, bluez_close};
SnxBleScan* snx_ble_system_open(char* why, size_t size) {
    Bluez* b = (Bluez*)calloc(1, sizeof *b);
    SnxBleScan* scan;
    if (!b) {
        snprintf(why, size, "%s", "out of memory");
        return NULL;
    }
    if (sd_load(&b->sd, why, size)) {
        free(b);
        return NULL;
    }
    scan = snx_ble_scan_make(&bluez_backend, b);
    if (!scan) {
        snprintf(why, size, "%s", "out of memory");
        plat_dl_close(b->sd.lib);
        free(b);
    }
    return scan;
}
