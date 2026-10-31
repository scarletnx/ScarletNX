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
#include "common/utilities/common.h"
#include "common/utilities/ini_lines.h"
#ifndef _WIN32
#include <sys/stat.h>
#endif

#define ONLINE_FILE "online.ini"

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

/* online.ini's room_password in the users folder dir, into password ("" without the file): 0, or -1 with why. The
   file holds a secret, so one other users of this machine may read is refused */
static int room_password(const char* dir, char* password, size_t size, char* why, size_t why_size) {
    char *path = snx_path_join(dir, ONLINE_FILE), *text = NULL, *key, *value;
    size_t length;
    SnxIni ini;
    int rc = -1;
    password[0] = 0;
    if (!path) {
        snprintf(why, why_size, "out of memory");
        goto done;
    }
    if (!snx_is_file(path)) {
        rc = 0;
        goto done;
    }
#ifndef _WIN32
    {
        struct stat st;
        if (stat(path, &st) || (st.st_mode & 077)) {
            snprintf(why, why_size, "%s can be read by other users of this machine: chmod 600 it", path);
            goto done;
        }
    }
#endif
    text = (char*)snx_read_file(path, &length);
    if (!text) {
        snprintf(why, why_size, "%s could not be read", path);
        goto done;
    }
    snx_ini_start(&ini, text);
    while (snx_ini_next(&ini, &key, &value)) {
        if (ini.section[0]) {
            snprintf(why, why_size, "%s: [%s] is not a section: the file has none", path, ini.section);
            goto done;
        }
        if (strcmp(key, "room_password")) {
            snprintf(why, why_size, "%s: %s is not a setting: room_password", path, key);
            goto done;
        }
        if (strlen(value) >= size) {
            snprintf(why, why_size, "%s: room_password is longer than %u bytes", path, (unsigned)size - 1);
            goto done;
        }
        strcpy(password, value);
    }
    rc = 0;
done:
    free(text);
    free(path);
    return rc;
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
    r = room_password(dir, password, sizeof password, why, sizeof why);
    free(dir);
    if (r) fatal("The Eden room's password: %s", why);
    r = snx_room_join(&room, host, port, snx_users_room_nickname(user), password, g_exename, g_hle.kernel.title_id, "",
                      &refused);
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
