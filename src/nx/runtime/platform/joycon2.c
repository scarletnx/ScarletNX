/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Joy-Con 2 host (joycon2.h). Three threads share g_joycon2 under its lock:
   - the connecting thread finds controllers and opens their links (a connection can take seconds, so the links that
     are up don't wait for it);
   - the host thread owns the open links and the controllers' sessions: it writes each one's state, pad and mouse
     motion, and closes a link that went down or whose setup failed;
   - the main thread (joycon2_poll) makes the ready ones gamepads, takes their motion, and writes the vibration and
     lights it wants. It frees a device's slot once the host thread closed its link */
#include "nx/runtime/platform/joycon2.h"
#include "nx/runtime/platform/joycon2_att.h"
#include "nx/runtime/platform/joycon2_known.h"
#include "nx/runtime/platform/joycon2_sdl.h"
#include "nx/runtime/main.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "common/platform/time.h"
#include "common/utilities/ascii.h"
#include "common/utilities/common.h"
#include "snx_settings.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JC2_DEVICES     8
#define JC2_KNOWN       16
#define JC2_CONNECT_MS  20000 /* the kernel connects once the controller advertises: time to hold its sync button */
#define JC2_DIRECT_NS   10000000000ull /* with no scanner: a known controller tried at most this often, */
#define JC2_BACKOFF_MAX 3              /* and twice as long after each failure in a row, up to 8 times (80 s): the
                                          public notes say a controller connected to over and over ignores the host
                                          for minutes */
#define JC2_STALE_NS    100000000ull   /* a vibration the main thread hasn't renewed for this long stops */
#define JC2_STABLE_NS   30000000000ull /* a link up this long after the setup: its failures in a row end */
#define JC2_HOLD_NS     60000000000ull /* a controller the dialog disconnected is not connected again this long */

typedef struct {
    u8 address[6]; /* as written, most significant first; all zero: the slot is free */
    int random;    /* its address type */
    Jc2Link link;
    Jc2Session session;
    u64 up_ns; /* the host thread's: when the setup ended, 0 before */
    /* the host thread's, read by the main thread under the lock */
    int state; /* the session's, JC2_FAILED once the link went down */
    int side;  /* JC2_SIDE_*, the session's */
    Jc2Pad pad;
    i32 mouse[2]; /* the sensor's motion the main thread has not taken */
    /* the main thread's, read by the host thread under the lock */
    Jc2Vibration rumble;
    Jc2Vibration rumble_right; /* a Pro Controller 2's right actuator's */
    u64 rumble_ns; /* when it was last given */
    u8 led;
    u64 ready_ns; /* when the main thread first saw it ready */
    int had;      /* it had a gamepad */
    int gamepad;  /* its jc2_pad (a pair's for both halves), 0 for none */
    int kind;     /* that gamepad's JC2_PAD_* */
    char who[40]; /* the player identity it carries (its first gamepad's), kept through joins and splits */
    int closing;  /* the main thread's: the dialog disconnects it, the host thread closes its link */
} Jc2Device;
typedef struct {
    u8 address[6];
    int side;
    char serial[17];
    int type;          /* the address type that connected: -1 not known yet, 0 public, 1 random */
    unsigned failures; /* in a row, which lengthen the wait before the next try */
    u64 tried_ns;      /* the last try */
    /* the dialog disconnected it then: held off until held_until, and the adverts heard meanwhile (logged, as what a
       controller does after its link closes is not known) */
    u64 held_since, held_until;
    unsigned held_adverts;
} Jc2Known;
static struct {
    PMutex lock;
    _Atomic int started; /* joycon2_start ran: the lock is made */
    _Atomic int run;     /* the threads run while it is set */
    int scanner; /* a scanner reports the adverts; else the known ones are tried in turn */
    u64 sync_until; /* plat_ns's: the sync window's end */
    Jc2Device dev[JC2_DEVICES];
    Jc2Known known[JC2_KNOWN];
    int known_n;
    /* the controllers the controller applet's dialog heard and asks to connect, for the connecting thread */
    struct {
        u8 address[6];
        int random, side, syncing;
    } asked[JC2_DEVICES];
    int asked_n;
    int connecting; /* the connecting thread is on one of them */
    _Atomic int listening; /* the dialog's scan listens, and the known controllers aren't tried in turn meanwhile */
} g_joycon2;

/* ---- the controllers the runtime knows: joycon2.txt, a line each, "AA:BB:CC:DD:EE:FF L SERIAL" (L or R a Joy-Con 2's
   side, P a Pro Controller 2), in the user's config folder (every game's), else the game's */
static void known_path(char* out, size_t n) {
    char* dir = snx_user_dir(SNX_USER_CONFIG);
    char* path = dir ? snx_path_join(dir, "joycon2.txt") : NULL;
    if (path)
        snprintf(out, n, "%s", path);
    else
        snprintf(out, n, "%sjoycon2.txt", g_confdir); /* which ends with its separator */
    free(path);
    free(dir);
}
static void known_load(void) {
    char path[1100];
    Jc2KnownLine lines[JC2_KNOWN];
    int n;
    known_path(path, sizeof path);
    n = joycon2_known_load(path, lines, JC2_KNOWN); /* what it does not read, and a file it cannot, said */
    g_joycon2.known_n = 0;
    for (int i = 0; i < n; i++) {
        Jc2Known k = {0};
        memcpy(k.address, lines[i].address, 6);
        memcpy(k.serial, lines[i].serial, sizeof k.serial);
        k.side = lines[i].side == 'P' ? JC2_SIDE_PRO : lines[i].side == 'R' ? JC2_SIDE_RIGHT : JC2_SIDE_LEFT;
        k.type = -1;
        g_joycon2.known[g_joycon2.known_n++] = k;
    }
    LOG("joycon2: %d known controllers in %s\n", g_joycon2.known_n, path);
}
static void known_save(void) {
    char path[1100];
    Jc2KnownLine lines[JC2_KNOWN];
    known_path(path, sizeof path);
    for (int i = 0; i < g_joycon2.known_n; i++) {
        memcpy(lines[i].address, g_joycon2.known[i].address, 6);
        memcpy(lines[i].serial, g_joycon2.known[i].serial, sizeof lines[i].serial);
        lines[i].side = g_joycon2.known[i].side == JC2_SIDE_PRO     ? 'P'
                        : g_joycon2.known[i].side == JC2_SIDE_RIGHT ? 'R'
                                                                    : 'L';
    }
    joycon2_known_save(path, lines, g_joycon2.known_n); /* said when it cannot */
}
static int known_index(const u8 address[6]) { /* under the lock */
    for (int i = 0; i < g_joycon2.known_n; i++)
        if (!memcmp(g_joycon2.known[i].address, address, 6)) return i;
    return -1;
}

/* ---- finding controllers. NOT WRITTEN: scan_start says so, and the connecting thread only tries the known
   controllers' addresses in turn (direct_tick). The design:
   - Linux: BlueZ's D-Bus API, which an unprivileged user may call (org.bluez.Adapter1.SetDiscoveryFilter with
     Transport "le", StartDiscovery; the ManufacturerData[0x0553] of each org.bluez.Device1 that InterfacesAdded and
     PropertiesChanged report goes to jc2_advert, then host_found). Through libsystemd's sd-bus, opened at run time
     (dlopen), so the runtime gains no link dependency. Discovery stops while a controller connects (an LE scan blocks
     a new LE connection on many adapters). bluetoothd must not connect, pair or trust the device itself: the link is
     our own socket (joycon2_link.c), and the device is never paired.
   - Windows: WinRT's BluetoothLEAdvertisementWatcher (active scan, manufacturer data 0x0553), then
     BluetoothLEDevice.FromBluetoothAddressAsync, GattSession with MaintainConnection, and the characteristics by their
     fixed UUIDs (joycon2_proto.h's handles have them), ThroughputOptimized connection parameters (joycon2cpp does the
     same); in a C++/WinRT file of its own behind this same interface.
   Neither pairs: the controller drops a link whose host starts SMP, and a pairing would overwrite its console's entry */
static int scan_start(void) { return -1; }

/* the device slot that has address, else -1 and a free one in *free (-1 for none). Under the lock */
static int device_of(const u8 address[6], int* free) {
    static const u8 none[6] = {0};
    *free = -1;
    for (int i = 0; i < JC2_DEVICES; i++) {
        if (!memcmp(g_joycon2.dev[i].address, address, 6)) return i;
        if (*free < 0 && !memcmp(g_joycon2.dev[i].address, none, 6)) *free = i;
    }
    return -1;
}
/* a controller to connect to (its advert seen, or a known one's turn): taken when it is known, or waits to sync while
   the window is open. The connecting thread's. 0 when it connected, -1 when it didn't, 1 when it isn't taken */
static int host_found(const u8 address[6], int random, const Jc2Advert* a, u64 now) {
    int free;
    pm_lock(&g_joycon2.lock);
    int k = known_index(address);
    int taken = device_of(address, &free) >= 0 || free < 0 || (k < 0 && !(a->syncing && now < g_joycon2.sync_until)) ||
                (k >= 0 && now < g_joycon2.known[k].held_until); /* held off since the dialog disconnected it */
    pm_unlock(&g_joycon2.lock);
    if (taken) return 1;
    Jc2Link link;
    u64 began = plat_ns();
    if (jc2_link_open(&link, address, random, JC2_CONNECT_MS)) {
        /* ENOSYS from the socket is an HCI status the kernel has no errno for */
        LOG("joycon2: no connection to %02X:%02X:%02X:%02X:%02X:%02X (%s address): %s (errno %d, after %u ms)\n",
            address[0], address[1], address[2], address[3], address[4], address[5], random ? "random" : "public",
            link.err ? strerror(link.err) : "the MTU exchange got no answer", link.err,
            (unsigned)((plat_ns() - began) / 1000000u));
        return -1;
    }
    pm_lock(&g_joycon2.lock);
    if (k >= 0) g_joycon2.known[k].type = random;
    Jc2Device* d = &g_joycon2.dev[free];
    *d = (Jc2Device){0};
    memcpy(d->address, address, 6);
    d->random = random;
    d->link = link;
    d->side = a->product == JC2_PID_PRO     ? JC2_SIDE_PRO
              : a->product == JC2_PID_RIGHT ? JC2_SIDE_RIGHT
                                            : JC2_SIDE_LEFT;
    jc2_session_start(&d->session, d->side);
    d->state = JC2_SETUP;
    d->rumble = (Jc2Vibration){0.0f, 160.0f, 0.0f, 320.0f};
    d->rumble_right = d->rumble;
    int pro = d->side == JC2_SIDE_PRO;
    pm_unlock(&g_joycon2.lock);
    LOG("joycon2: connected to %s %s %02X:%02X:%02X:%02X:%02X:%02X, MTU %u\n", k >= 0 ? "known" : "new",
        pro ? "Pro Controller 2" : "Joy-Con 2",
        address[0], address[1], address[2], address[3], address[4], address[5], (unsigned)link.mtu);
    return 0;
}
/* with no scanner: each known controller not connected is tried in turn, every JC2_DIRECT_NS and twice as long after
   each failure in a row, by the address type that connected before (unknown at first: public, then random at once).
   It answers while it advertises: after a button press, or while its sync button is held */
static void direct_tick(void) {
    for (int k = 0; k < JC2_KNOWN; k++) {
        u64 now = plat_ns();
        int free;
        pm_lock(&g_joycon2.lock);
        if (k >= g_joycon2.known_n) {
            pm_unlock(&g_joycon2.lock);
            return;
        }
        Jc2Known* c = &g_joycon2.known[k];
        u64 wait = JC2_DIRECT_NS << (c->failures < JC2_BACKOFF_MAX ? c->failures : JC2_BACKOFF_MAX);
        int due = (!c->tried_ns || now >= c->tried_ns + wait) && device_of(c->address, &free) < 0 &&
                  now >= c->held_until; /* one the dialog disconnected waits out its hold */
        u8 address[6];
        memcpy(address, c->address, 6);
        int type = c->type;
        Jc2Advert a = {0};
        a.product = c->side == JC2_SIDE_PRO ? JC2_PID_PRO : c->side == JC2_SIDE_RIGHT ? JC2_PID_RIGHT : JC2_PID_LEFT;
        if (due) c->tried_ns = now ? now : 1;
        pm_unlock(&g_joycon2.lock);
        if (!due) continue;
        int r = host_found(address, type > 0, &a, now);
        if (r < 0 && type < 0) r = host_found(address, 1, &a, plat_ns());
        if (r < 0) {
            pm_lock(&g_joycon2.lock);
            if (k < g_joycon2.known_n) g_joycon2.known[k].failures++;
            pm_unlock(&g_joycon2.lock);
        }
    }
}
/* the time an adapter gets to leave the dialog's scan before a connect: some refuse a connect started while it ends */
#define JC2_SETTLE_NS 500000000ull
/* the controllers the dialog's scan heard, each tried once at once: no wait between tries while it listens */
static void asked_tick(void) {
    for (;;) {
        pm_lock(&g_joycon2.lock);
        if (!g_joycon2.asked_n) {
            pm_unlock(&g_joycon2.lock);
            return;
        }
        u8 address[6];
        memcpy(address, g_joycon2.asked[0].address, 6);
        int random = g_joycon2.asked[0].random;
        Jc2Advert a = {0};
        a.product = g_joycon2.asked[0].side == JC2_SIDE_PRO     ? JC2_PID_PRO
                    : g_joycon2.asked[0].side == JC2_SIDE_RIGHT ? JC2_PID_RIGHT
                                                                : JC2_PID_LEFT;
        a.syncing = g_joycon2.asked[0].syncing;
        g_joycon2.asked_n--;
        memmove(g_joycon2.asked, g_joycon2.asked + 1, sizeof g_joycon2.asked[0] * (size_t)g_joycon2.asked_n);
        g_joycon2.connecting = 1;
        pm_unlock(&g_joycon2.lock);
        plat_sleep_ns(JC2_SETTLE_NS); /* the adapter leaves the scan the dialog stopped before a connect starts */
        host_found(address, random, &a, plat_ns());
        pm_lock(&g_joycon2.lock);
        g_joycon2.connecting = 0;
        pm_unlock(&g_joycon2.lock);
    }
}
static void connect_thread(void* arg) {
    (void)arg;
    while (atomic_load_explicit(&g_joycon2.run, memory_order_acquire)) {
        asked_tick();
        if (!g_joycon2.scanner && !atomic_load_explicit(&g_joycon2.listening, memory_order_acquire)) direct_tick();
        plat_sleep_ns(200000000);
    }
}

/* ---- the host thread */
static void on_notify(void* user, u16 handle, const u8* p, size_t n) {
    Jc2Device* d = (Jc2Device*)user;
    Jc2Transport t = {&d->link, jc2_link_write, jc2_link_notify};
    jc2_session_notify(&d->session, &t, handle, p, n, plat_ns());
}
/* one controller's turn: its notifications, its session's tick, and its state to the main thread. now is read after
   the notifications, whose replies on_notify stamped */
static void device_turn(Jc2Device* d) {
    Jc2Transport t = {&d->link, jc2_link_write, jc2_link_notify};
    int up = 1, got;
    while ((got = jc2_link_poll(&d->link, 0, on_notify, d)) > 0) {}
    if (got < 0) up = 0;
    int why = up ? 0 : d->link.err; /* the kernel's reason the link went down (jc2_link_poll's) */
    u64 now = plat_ns();
    pm_lock(&g_joycon2.lock);
    int closing = d->closing;
    Jc2Vibration v = d->rumble, v2 = d->rumble_right;
    if (d->rumble_ns + JC2_STALE_NS < now) /* the main thread stopped giving it */
        v.amp_low = v.amp_high = v2.amp_low = v2.amp_high = 0.0f;
    jc2_session_rumble(&d->session, &v);
    jc2_session_rumble2(&d->session, &v2);
    if (d->led) jc2_session_led(&d->session, d->led);
    pm_unlock(&g_joycon2.lock);
    int state = up && !closing ? jc2_session_tick(&d->session, &t, now) : JC2_FAILED;
    i32 mouse[2];
    jc2_session_mouse(&d->session, mouse);
    pm_lock(&g_joycon2.lock);
    int k = known_index(d->address);
    if (state == JC2_READY && d->state != JC2_READY) {
        LOG("joycon2: %s %s ready, firmware %u.%u.%u\n",
            d->session.side == JC2_SIDE_PRO     ? "Pro Controller 2"
            : d->session.side == JC2_SIDE_RIGHT ? "right Joy-Con 2"
                                                : "left Joy-Con 2",
            d->session.info.serial, d->session.version[0], d->session.version[1], d->session.version[2]);
        d->up_ns = now ? now : 1;
        if (k < 0 && g_joycon2.known_n < JC2_KNOWN) { /* synced now: known from now on */
            Jc2Known* n = &g_joycon2.known[g_joycon2.known_n++];
            *n = (Jc2Known){0};
            memcpy(n->address, d->address, 6);
            n->side = d->session.side;
            n->type = d->random;
            memcpy(n->serial, d->session.info.serial, sizeof n->serial);
            known_save();
        }
    }
    /* failures in a row: a failed setup, or a link that dropped soon after it (a controller that refuses a host with
       no bond would); a link that stays up ends them */
    int stable = d->up_ns && now >= d->up_ns + JC2_STABLE_NS;
    if (k >= 0 && stable) g_joycon2.known[k].failures = 0;
    if (state == JC2_FAILED && closing) { /* the dialog's Disconnect: only the link closes, no command is sent */
        LOG("joycon2: %02X:%02X:%02X:%02X:%02X:%02X disconnected by the controller applet's dialog, held off %u s\n",
            d->address[0], d->address[1], d->address[2], d->address[3], d->address[4], d->address[5],
            (unsigned)(JC2_HOLD_NS / 1000000000ull));
        jc2_link_close(&d->link);
    } else if (state == JC2_FAILED) { /* the main thread frees the slot once the link is closed */
        WARN("joycon2: Joy-Con 2 %02X:%02X:%02X:%02X:%02X:%02X %s%s%s\n", d->address[0], d->address[1], d->address[2],
             d->address[3], d->address[4], d->address[5], up ? "failed its setup" : "disconnected",
             !up && why ? ": " : "", !up && why ? strerror(why) : "");
        if (k >= 0 && !stable) g_joycon2.known[k].failures++;
        jc2_link_close(&d->link);
    }
    d->state = state;
    d->side = d->session.side;
    d->pad = d->session.pad;
    d->mouse[0] += mouse[0];
    d->mouse[1] += mouse[1];
    pm_unlock(&g_joycon2.lock);
}
static void host_thread(void* arg) {
    (void)arg;
    while (atomic_load_explicit(&g_joycon2.run, memory_order_acquire)) {
        int turn[JC2_DEVICES], n = 0;
        pm_lock(&g_joycon2.lock); /* the connecting thread fills a free slot, the main thread frees a closed one */
        for (int i = 0; i < JC2_DEVICES; i++)
            if (g_joycon2.dev[i].link.fd >= 0 && g_joycon2.dev[i].state != JC2_FAILED) turn[n++] = i;
        pm_unlock(&g_joycon2.lock);
        for (int i = 0; i < n; i++) device_turn(&g_joycon2.dev[turn[i]]);
        plat_sleep_ns(n ? 1000000 : 50000000);
    }
    pm_lock(&g_joycon2.lock);
    for (int i = 0; i < JC2_DEVICES; i++) jc2_link_close(&g_joycon2.dev[i].link);
    pm_unlock(&g_joycon2.lock);
}

void joycon2_start(void) {
    pm_init(&g_joycon2.lock);
    for (int i = 0; i < JC2_DEVICES; i++) g_joycon2.dev[i].link.fd = -1;
    atomic_store_explicit(&g_joycon2.started, 1, memory_order_release);
    if (!g_settings.joy_con_2) return;
    known_load();
    g_joycon2.scanner = !scan_start();
    if (!g_joycon2.scanner && !g_joycon2.known_n) {
        LOG("joycon2: this build finds no Joy-Con 2 itself (no scanner yet), and joycon2.txt names none\n");
        return;
    }
    if (!g_joycon2.scanner)
        LOG("joycon2: no scanner yet: the %d controllers of joycon2.txt are tried every %llu s or more, each while it "
            "advertises (a button pressed, or its sync button held)\n",
            g_joycon2.known_n, (unsigned long long)(JC2_DIRECT_NS / 1000000000ull));
    atomic_store_explicit(&g_joycon2.run, 1, memory_order_release);
    if (plat_thread(host_thread, NULL, 0) || plat_thread(connect_thread, NULL, 0)) {
        atomic_store_explicit(&g_joycon2.run, 0, memory_order_release);
        WARN("joycon2: no thread for the Joy-Con 2 host\n");
    }
}
void joycon2_stop(void) { atomic_store_explicit(&g_joycon2.run, 0, memory_order_release); }
int joycon2_running(void) { return atomic_load_explicit(&g_joycon2.run, memory_order_acquire); }
void joycon2_listening(int on) { atomic_store_explicit(&g_joycon2.listening, on, memory_order_release); }
int joycon2_connecting(void) {
    if (!joycon2_running()) return 0;
    pm_lock(&g_joycon2.lock);
    int busy = g_joycon2.asked_n > 0 || g_joycon2.connecting;
    pm_unlock(&g_joycon2.lock);
    return busy;
}
int joycon2_has(const u8 address[6]) {
    int free;
    if (!joycon2_running()) return 0;
    pm_lock(&g_joycon2.lock);
    int i = device_of(address, &free);
    pm_unlock(&g_joycon2.lock);
    return i >= 0;
}
int joycon2_held(const u8 address[6], int wake) {
    if (!joycon2_running()) return 0;
    u64 now = plat_ns();
    pm_lock(&g_joycon2.lock);
    int k = known_index(address), held = k >= 0 && now < g_joycon2.known[k].held_until;
    if (held) {
        Jc2Known* c = &g_joycon2.known[k];
        unsigned n = ++c->held_adverts;
        if (n <= 3 || n % 50 == 0) /* what it does once its link closed: whether it advertises, and how */
            LOG("joycon2: %s held off since its disconnect, advert %u heard %llu ms after it%s\n", c->serial, n,
                (unsigned long long)((now - c->held_since) / 1000000u), wake ? ", asking its console to wake" : "");
    }
    pm_unlock(&g_joycon2.lock);
    return held;
}
int joycon2_disconnect(u32 gamepad_id) {
    int closed = 0;
    if (!joycon2_running() || !gamepad_id) return 0;
    u64 now = plat_ns();
    pm_lock(&g_joycon2.lock);
    for (int i = 0; i < JC2_DEVICES; i++) { /* a pair's two halves both */
        Jc2Device* d = &g_joycon2.dev[i];
        if (!d->gamepad || jc2_pad_id(d->gamepad) != gamepad_id) continue;
        d->closing = 1;
        closed++;
        int k = known_index(d->address);
        if (k >= 0) {
            g_joycon2.known[k].held_since = now ? now : 1;
            g_joycon2.known[k].held_until = now + JC2_HOLD_NS;
            g_joycon2.known[k].held_adverts = 0;
        }
    }
    pm_unlock(&g_joycon2.lock);
    return closed;
}
int joycon2_known(const u8 address[6]) {
    if (!joycon2_running()) return 0;
    pm_lock(&g_joycon2.lock);
    int k = known_index(address);
    pm_unlock(&g_joycon2.lock);
    return k >= 0;
}
void joycon2_connect(const u8 address[6], int random, int side, int syncing) {
    if (!joycon2_running()) return;
    pm_lock(&g_joycon2.lock);
    if (syncing) g_joycon2.sync_until = plat_ns() + 30000000000ull; /* a new one, which the user confirmed */
    int k = known_index(address);
    if (k >= 0 && g_joycon2.known[k].held_until) { /* taken back from its hold by the dialog's Connect */
        LOG("joycon2: %s taken back after its disconnect, %u adverts heard while held off\n",
            g_joycon2.known[k].serial, g_joycon2.known[k].held_adverts);
        g_joycon2.known[k].held_until = 0;
    }
    if (g_joycon2.asked_n < JC2_DEVICES) {
        memcpy(g_joycon2.asked[g_joycon2.asked_n].address, address, 6);
        g_joycon2.asked[g_joycon2.asked_n].random = random;
        g_joycon2.asked[g_joycon2.asked_n].side = side;
        g_joycon2.asked[g_joycon2.asked_n].syncing = syncing;
        g_joycon2.asked_n++;
    }
    pm_unlock(&g_joycon2.lock);
}
void joycon2_sync(u64 seconds) {
    if (!atomic_load_explicit(&g_joycon2.started, memory_order_acquire)) return;
    pm_lock(&g_joycon2.lock);
    g_joycon2.sync_until = plat_ns() + seconds * 1000000000ull;
    pm_unlock(&g_joycon2.lock);
}

/* ---- the main thread: gamepads */
static Jc2Device* half(int gamepad, int side) {
    for (int i = 0; i < JC2_DEVICES; i++)
        if (g_joycon2.dev[i].gamepad == gamepad && g_joycon2.dev[i].side == side) return &g_joycon2.dev[i];
    return NULL;
}
static void set_gamepad(int i, int gamepad, int kind) {
    g_joycon2.dev[i].gamepad = gamepad;
    g_joycon2.dev[i].kind = kind;
    g_joycon2.dev[i].had |= gamepad != 0;
}
/* a controller that left: its gamepad goes (the other half of a pair becomes a single at once), and its slot is
   free once its link is closed */
static void drop(Jc2Device* d) {
    int gamepad = d->gamepad;
    if (gamepad) {
        jc2_pad_detach(gamepad);
        for (int i = 0; i < JC2_DEVICES; i++)
            if (g_joycon2.dev[i].gamepad == gamepad) set_gamepad(i, 0, 0);
    }
    if (d->link.fd < 0) {
        *d = (Jc2Device){0};
        d->link.fd = -1;
    }
}
/* the gamepads the ready controllers make (jc2_join_next): a pair, or a single */
static void attach(u64 now) {
    Jc2Join j[JC2_DEVICES];
    int a, b, act;
    for (int i = 0; i < JC2_DEVICES; i++) {
        Jc2Device* d = &g_joycon2.dev[i];
        if (d->state == JC2_READY && !d->ready_ns) d->ready_ns = now ? now : 1;
        j[i] = (Jc2Join){d->state == JC2_READY, d->side, d->ready_ns, d->had, d->gamepad, d->kind};
    }
    while ((act = jc2_join_next(j, JC2_DEVICES, g_settings.pair_joy_cons, now, &a, &b)) != JC2_JOIN_NONE) {
        int kind = act == JC2_JOIN_PAIR                          ? JC2_PAD_PAIR
                   : g_joycon2.dev[a].side == JC2_SIDE_PRO   ? JC2_PAD_PRO
                   : g_joycon2.dev[a].side == JC2_SIDE_RIGHT ? JC2_PAD_RIGHT
                                                             : JC2_PAD_LEFT;
        /* the identity the new gamepad carries: a half's that already had a player (the one with the lower player),
           else the first half's own serial, so a pair takes its single's player and a split half its pair's */
        char who[40] = "";
        int pa = g_joycon2.dev[a].gamepad ? jc2_pad_player(g_joycon2.dev[a].gamepad) : -1;
        int pb = act == JC2_JOIN_PAIR && g_joycon2.dev[b].gamepad ? jc2_pad_player(g_joycon2.dev[b].gamepad) : -1;
        int from = act == JC2_JOIN_PAIR && pb >= 0 && (pa < 0 || pb < pa) ? b : a;
        if (g_joycon2.dev[from].who[0])
            snprintf(who, sizeof who, "%s", g_joycon2.dev[from].who);
        else if (act == JC2_JOIN_PAIR && g_joycon2.dev[from == a ? b : a].who[0])
            snprintf(who, sizeof who, "%s", g_joycon2.dev[from == a ? b : a].who);
        else
            snprintf(who, sizeof who, "jc2:%s", g_joycon2.dev[a].session.info.serial);
        if (act == JC2_JOIN_PAIR) /* each half's single goes */
            for (int k = 0; k < 2; k++) {
                int x = k ? b : a;
                if (g_joycon2.dev[x].gamepad) jc2_pad_detach(g_joycon2.dev[x].gamepad);
                set_gamepad(x, 0, 0);
            }
        int gamepad = jc2_pad_attach(kind);
        if (!gamepad) { /* SDL made none: again on the next poll */
            WARN("joycon2: SDL made no gamepad for a Joy-Con 2: %s\n", SDL_GetError());
            return;
        }
        jc2_pad_who(gamepad, who);
        snprintf(g_joycon2.dev[a].who, sizeof g_joycon2.dev[a].who, "%s", who);
        if (act == JC2_JOIN_PAIR) snprintf(g_joycon2.dev[b].who, sizeof g_joycon2.dev[b].who, "%s", who);
        set_gamepad(a, gamepad, kind);
        if (act == JC2_JOIN_PAIR) set_gamepad(b, gamepad, kind);
        j[a] = (Jc2Join){1, g_joycon2.dev[a].side, g_joycon2.dev[a].ready_ns, 1, gamepad, kind};
        if (act == JC2_JOIN_PAIR) j[b] = (Jc2Join){1, g_joycon2.dev[b].side, g_joycon2.dev[b].ready_ns, 1, gamepad, kind};
    }
}
void joycon2_poll(void) {
    static const u8 leds[8] = {0x1, 0x3, 0x7, 0xf, 0x9, 0x5, 0xd, 0x6}; /* hid's player lights */
    if (!g_settings.joy_con_2 || !atomic_load_explicit(&g_joycon2.started, memory_order_acquire)) return;
    u64 now = plat_ns();
    pm_lock(&g_joycon2.lock);
    for (int i = 0; i < JC2_DEVICES; i++)
        if (g_joycon2.dev[i].state == JC2_FAILED && (g_joycon2.dev[i].gamepad || g_joycon2.dev[i].link.fd < 0))
            drop(&g_joycon2.dev[i]);
    attach(now);
    for (int gamepad = 1; gamepad <= JC2_PADS; gamepad++) {
        Jc2Device *l = half(gamepad, JC2_SIDE_LEFT), *r = half(gamepad, JC2_SIDE_RIGHT);
        Jc2Vibration want[2];
        Jc2Device* pro = half(gamepad, JC2_SIDE_PRO);
        if (pro) { /* a Pro Controller 2: fed as the left, both its actuators */
            jc2_pad_feed(gamepad, &pro->pad, NULL, NULL, NULL);
            jc2_pad_rumble(gamepad, want);
            int player = jc2_pad_player(gamepad);
            pro->rumble = want[0];
            pro->rumble_right = want[1];
            pro->rumble_ns = now;
            pro->led = player >= 0 ? leds[player] : 0;
            continue;
        }
        if (!l && !r) continue;
        jc2_pad_feed(gamepad, l ? &l->pad : NULL, r ? &r->pad : NULL, l ? l->mouse : NULL, r ? r->mouse : NULL);
        if (l) l->mouse[0] = l->mouse[1] = 0;
        if (r) r->mouse[0] = r->mouse[1] = 0;
        jc2_pad_rumble(gamepad, want);
        int player = jc2_pad_player(gamepad);
        u8 led = player >= 0 ? leds[player] : 0;
        if (l) l->rumble = want[0], l->rumble_ns = now, l->led = led;
        if (r) r->rumble = want[1], r->rumble_ns = now, r->led = led;
    }
    pm_unlock(&g_joycon2.lock);
}
