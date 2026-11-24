/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Eden room of ldn=room (common/network/net_room.h): joined at start, before the game runs, at the .ini's room=
   (host, or host:port) as the user playing (users.ini's nickname), with the password of users/online.ini, unless
   airplane mode is on. A room that refuses stops the game and says why: the game would only find nobody. From then on the game's datagram
   sockets are the room's (bsd.c) and ldn's networks go through it (ldn.c); at exit the room hears this console
   leave */
#include "nx/hle/services/network/network.h"
#include "nx/hle/hle.h"
#include "nx/runtime/main.h"
#include "nx/runtime/platform/tracy.h"
#include "common/utilities/common.h"

/* room=host[:port]: 0, or -1 with why */
static int room_address(const char* text, char* host, size_t host_size, u16* port, char* why, size_t why_size) {
    const char* colon = strrchr(text, ':');
    size_t n = colon ? (size_t)(colon - text) : strlen(text);
    *port = SNX_ROOM_PORT;
    if (!n || n >= host_size) {
        snprintf(why, why_size, "room=%s names no host (room=host or host:port)", text);
        return -1;
    }
    memcpy(host, text, n);
    host[n] = 0;
    if (colon) {
        char* end;
        unsigned long p = strtoul(colon + 1, &end, 10);
        if (!colon[1] || *end || !p || p > 65535) {
            snprintf(why, why_size, "room=%s: %s is no port, 1 to 65535", text, colon + 1);
            return -1;
        }
        *port = (u16)p;
    }
    return 0;
}

void network_room_init(void) {
    char host[256], password[256], why[512], *dir;
    const SnxUser* user = snx_users_selected();
    SnxRoom* room;
    u16 port;
    int r, refused;
    if (g_opt_ldn != SNX_OPT_LDN_ROOM) return;
    if (g_settings.airplane_mode) { /* a console in airplane mode is in no network, the room's neither */
        LOG("room: airplane_mode=1 keeps this console out of the Eden room\n");
        return;
    }
    if (!g_settings.eden_room[0]) fatal("ldn=room needs the Eden room's address in the .ini: room=host or room=host:port");
    if (room_address(g_settings.eden_room, host, sizeof host, &port, why, sizeof why)) fatal("%s", why);
    dir = snx_users_dir(g_exedir[0] ? g_exedir : ".");
    if (!dir) fatal("Out of memory finding the users folder");
    r = snx_users_room_password(dir, password, sizeof password, why, sizeof why);
    free(dir);
    if (r) fatal("The Eden room's password: %s", why);
    u64 t0 = snx_tracy_attached() ? plat_ns() : 0;
    r = snx_room_join(&room, host, port, snx_users_room_nickname(user), password, g_exename, g_hle.kernel.title_id, "",
                      &refused);
    if (t0) SNX_PLOT("net room join ms", (double)(plat_ns() - t0) / 1e6); /* no underflow on attach or detach */
    if (r) {
        SNX_ZONE_T(z_net_room, "hle: net room error", "join refused: %s", snx_room_why_text(refused));
    }
    if (r) /* the user by number: the nickname can name the player, and players attach the log to public reports */
        fatal("The Eden room at %s:%u, as user %d: %s (errno %d)", host, port, snx_users_selected_number(),
              snx_room_why_text(refused), -r);
    network_set_room(room);
}
void network_room_leave(void) {
    SnxRoom* room = network_room();
    if (!room) return;
    /* the game's threads may still open sockets on it as the process ends: it stays, out of the room, never freed */
    snx_room_ref(room);
    snx_room_leave(room);
}
