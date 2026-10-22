/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Eden room of ldn=room (room.c): the room= address and its port, users/online.ini's room_password (a file others
   may read refused, any other key or a section refused, none at all an empty password), what stops the game before
   any connection (ldn=room without room=), and airplane mode, which keeps the console out of the room */
#include "nx/hle/services/network/network.h"
#include <setjmp.h>
#include <stdarg.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "nx/hle/services/network/room.c"
#include "nx/hle/hle.c"

char g_exedir[1024], g_exename[256];
int g_opt_ldn;
static SnxRoom* g_room_set;
void network_set_room(SnxRoom* room) { g_room_set = room; }
SnxRoom* network_room(void) { return g_room_set; }
void logf_(int level, const char* format, ...) {
    (void)level;
    (void)format;
}
static jmp_buf g_stopped;
static char g_stop[512];
void fatal(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(g_stop, sizeof g_stop, format, args);
    va_end(args);
    longjmp(g_stopped, 1);
}

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "room_service.c:%d: check failed: %s\n", __LINE__, #x);                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

#define DIR "room_service_users"
static void put(const char* text, int mode) {
    FILE* f = fopen(DIR "/" ONLINE_FILE, "w");
    CHECK(f != NULL);
    fputs(text, f);
    fclose(f);
#ifndef _WIN32
    CHECK(chmod(DIR "/" ONLINE_FILE, (mode_t)mode) == 0);
#else
    (void)mode;
#endif
}

static void addresses(void) {
    char host[256], why[512];
    u16 port;
    CHECK(room_address("room.example", host, sizeof host, &port, why, sizeof why) == 0);
    CHECK(!strcmp(host, "room.example") && port == SNX_ROOM_PORT);
    CHECK(room_address("192.168.0.5:5000", host, sizeof host, &port, why, sizeof why) == 0);
    CHECK(!strcmp(host, "192.168.0.5") && port == 5000);
    CHECK(room_address("host:", host, sizeof host, &port, why, sizeof why) == -1 && strstr(why, "is no port"));
    CHECK(room_address("host:0", host, sizeof host, &port, why, sizeof why) == -1);
    CHECK(room_address("host:65536", host, sizeof host, &port, why, sizeof why) == -1);
    CHECK(room_address("host:12ab", host, sizeof host, &port, why, sizeof why) == -1);
    CHECK(room_address(":5000", host, sizeof host, &port, why, sizeof why) == -1 && strstr(why, "names no host"));
}
static void passwords(void) {
    char password[256], why[512];
    snx_remove_tree(DIR);
    CHECK(snx_mkdirs(DIR) == 0);
    /* no file: no password */
    strcpy(password, "x");
    CHECK(room_password(DIR, password, sizeof password, why, sizeof why) == 0 && !password[0]);
    put("; the rooms\nroom_password=s3cret pass\n", 0600);
    CHECK(room_password(DIR, password, sizeof password, why, sizeof why) == 0 && !strcmp(password, "s3cret pass"));
#ifndef _WIN32
    put("room_password=s3cret\n", 0644); /* others may read it */
    CHECK(room_password(DIR, password, sizeof password, why, sizeof why) == -1 && strstr(why, "chmod 600"));
#endif
    put("room_pasword=s3cret\n", 0600);
    CHECK(room_password(DIR, password, sizeof password, why, sizeof why) == -1);
    CHECK(strstr(why, "room_pasword is not a setting: room_password"));
    put("[room]\nroom_password=s3cret\n", 0600);
    CHECK(room_password(DIR, password, sizeof password, why, sizeof why) == -1 &&
          strstr(why, "[room] is not a section"));
    {
        char text[300];
        memset(text, 'p', sizeof text);
        memcpy(text, "room_password=", 14);
        text[sizeof text - 2] = '\n';
        text[sizeof text - 1] = 0;
        put(text, 0600);
        CHECK(room_password(DIR, password, 16, why, sizeof why) == -1 && strstr(why, "longer than 15 bytes"));
    }
    snx_remove_tree(DIR);
}
static void stops(void) {
    g_opt_ldn = SNX_OPT_LDN_LAN;
    network_room_init(); /* nothing to join */
    CHECK(network_room() == NULL);
    /* airplane mode keeps the console out of the room, as out of any network: nobody at port 9 is even asked */
    g_opt_ldn = SNX_OPT_LDN_ROOM;
    g_settings.airplane_mode = 1;
    strcpy(g_settings.eden_room, "127.0.0.1:9");
    g_stop[0] = 0;
    if (!setjmp(g_stopped)) network_room_init();
    CHECK(network_room() == NULL && !g_stop[0]); /* not stopped: the join that would fail never ran */
    g_settings.airplane_mode = 0;
    g_opt_ldn = SNX_OPT_LDN_ROOM;
    g_settings.eden_room[0] = 0;
    if (!setjmp(g_stopped)) {
        network_room_init();
        CHECK(!"ldn=room without room= started");
    }
    CHECK(strstr(g_stop, "room=host or room=host:port"));
    strcpy(g_settings.eden_room, "host:99999");
    if (!setjmp(g_stopped)) {
        network_room_init();
        CHECK(!"a room at no port started");
    }
    CHECK(strstr(g_stop, "is no port") && network_room() == NULL);
    g_opt_ldn = SNX_OPT_LDN_LAN;
}

void pm_init(PMutex* m) { (void)m; }

int main(void) {
    hle_init();
    g_hle.kernel.title_id = 0; /* none, as the test was written for */
    addresses();
    passwords();
    stops();
    printf("room_service: %u checks passed\n", checks);
    return 0;
}
