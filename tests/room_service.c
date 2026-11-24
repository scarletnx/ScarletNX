/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Eden room of ldn=room (room.c): the room= address and its port, users/online.ini's room_password (a file others
   may read refused, any other key or a section refused, none at all an empty password), what stops the game before
   any connection (ldn=room without room=), airplane mode, which keeps the console out of the room, and a room that
   refuses the console */
#include "nx/hle/services/network/network.h"
#include "fake_room.h"
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
    FILE* f = fopen(DIR "/online.ini", "w");
    CHECK(f != NULL);
    fputs(text, f);
    fclose(f);
#ifndef _WIN32
    CHECK(chmod(DIR "/online.ini", (mode_t)mode) == 0);
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
/* online.ini's mode, and its text ("" without it) */
static unsigned online_mode(void) {
#ifdef _WIN32
    return 0600;
#else
    struct stat st;
    return stat(DIR "/online.ini", &st) ? 0 : (unsigned)(st.st_mode & 07777);
#endif
}
/* whether setting password is refused with why naming what, the file's bytes as they were before */
static int set_refused(const char* password, const char* what) {
    char why[512];
    size_t before_size, after_size;
    char *before = (char*)snx_read_file(DIR "/online.ini", &before_size), *after;
    int refused = snx_users_room_password_set(DIR, password, why, sizeof why) == -1 && strstr(why, what) != NULL;
    after = (char*)snx_read_file(DIR "/online.ini", &after_size);
    refused &= before && after && before_size && before_size == after_size && !memcmp(before, after, before_size);
    free(before);
    free(after);
    return refused;
}
static void password_written(void) {
    char password[256], why[512], long_one[257];
    snx_remove_tree(DIR);
    /* kept 0600, whatever the umask and the mode it had, in the users folder made with it */
#ifndef _WIN32
    mode_t was = umask(0);
#endif
    CHECK(snx_users_room_password_set(DIR, "s3cret = pass; #1", why, sizeof why) == 0 && online_mode() == 0600);
    CHECK(snx_users_room_password(DIR, password, sizeof password, why, sizeof why) == 0 &&
          !strcmp(password, "s3cret = pass; #1"));
    put("room_password=old\n", 0644);
    CHECK(snx_users_room_password_set(DIR, "new", why, sizeof why) == 0 && online_mode() == 0600);
    CHECK(!snx_users_room_password(DIR, password, sizeof password, why, sizeof why) && !strcmp(password, "new"));
#ifndef _WIN32
    umask(was);
#endif
    CHECK(!snx_is_file(DIR "/online.ini.new"));
    /* text its reader would not give back, longer than a room takes, or with a control character: refused, as is */
    CHECK(snx_users_room_password_set(DIR, " new", why, sizeof why) == -1 && strstr(why, "space at either end"));
    CHECK(snx_users_room_password_set(DIR, "new ", why, sizeof why) == -1 && strstr(why, "space at either end"));
    CHECK(snx_users_room_password_set(DIR, "a\nroom_password=b", why, sizeof why) == -1 &&
          strstr(why, "control character"));
    memset(long_one, 'p', sizeof long_one - 1);
    long_one[sizeof long_one - 1] = 0;
    CHECK(snx_users_room_password_set(DIR, long_one, why, sizeof why) == -1 &&
          strstr(why, "at most 255 bytes, not 256"));
    long_one[255] = 0;
    CHECK(snx_users_room_password_set(DIR, long_one, why, sizeof why) == 0);
    CHECK(!snx_users_room_password(DIR, password, sizeof password, why, sizeof why) && strlen(password) == 255);
    CHECK(snx_users_room_password_set(DIR, "new", why, sizeof why) == 0);
    /* none: the file away, twice the same */
    CHECK(snx_users_room_password_set(DIR, "", why, sizeof why) == 0 && !snx_is_file(DIR "/online.ini"));
    CHECK(snx_users_room_password_set(DIR, "", why, sizeof why) == 0);
    CHECK(!snx_users_room_password(DIR, password, sizeof password, why, sizeof why) && !password[0]);
    /* a file with anything else: refused, as it was, what it has named */
    put("[room]\nroom_password=s3cret\n", 0600);
    CHECK(set_refused("new", "has [room]"));
    put("other=1\n", 0600);
    CHECK(set_refused("new", "has other"));
    CHECK(set_refused("", "has other"));
    snx_remove_tree(DIR);
}
static void passwords(void) {
    char password[256], why[512];
    snx_remove_tree(DIR);
    CHECK(snx_mkdirs(DIR) == 0);
    /* no file: no password */
    strcpy(password, "x");
    CHECK(snx_users_room_password(DIR, password, sizeof password, why, sizeof why) == 0 && !password[0]);
    put("; the rooms\nroom_password=s3cret pass\n", 0600);
    CHECK(snx_users_room_password(DIR, password, sizeof password, why, sizeof why) == 0 &&
          !strcmp(password, "s3cret pass"));
#ifndef _WIN32
    put("room_password=s3cret\n", 0644); /* others may read it */
    CHECK(snx_users_room_password(DIR, password, sizeof password, why, sizeof why) == -1 && strstr(why, "chmod 600"));
#endif
    put("room_pasword=s3cret\n", 0600);
    CHECK(snx_users_room_password(DIR, password, sizeof password, why, sizeof why) == -1);
    CHECK(strstr(why, "room_pasword is not a setting: room_password"));
    put("[room]\nroom_password=s3cret\n", 0600);
    CHECK(snx_users_room_password(DIR, password, sizeof password, why, sizeof why) == -1 &&
          strstr(why, "[room] is not a section"));
    {
        char text[300];
        memset(text, 'p', sizeof text);
        memcpy(text, "room_password=", 14);
        text[sizeof text - 2] = '\n';
        text[sizeof text - 1] = 0;
        put(text, 0600);
        CHECK(snx_users_room_password(DIR, password, 16, why, sizeof why) == -1 && strstr(why, "longer than 15 bytes"));
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

/* refused by the room (fake_room.h takes "secret" alone): the stop names the user by number, never by nickname, which
   players attach to public reports with the log. A portable folder keeps the player's own users/online.ini out */
static void refused(void) {
    FakeRoom* f = fake_room_start();
    snx_remove_tree("room_service_app");
    CHECK(snx_mkdirs("room_service_app/" SNX_PORTABLE_DIR) == 0);
    snprintf(g_exedir, sizeof g_exedir, "room_service_app/" SNX_PORTABLE_DIR);
    snprintf(g_settings.eden_room, sizeof g_settings.eden_room, "127.0.0.1:%u", fake_room_port(f));
    g_user_count = 1;
    strcpy(g_users[0].nickname, "Wanda Maximoff");
    g_opt_ldn = SNX_OPT_LDN_ROOM;
    g_stop[0] = 0;
    if (!setjmp(g_stopped)) {
        network_room_init();
        CHECK(!"a room that refused the password started");
    }
    CHECK(strstr(g_stop, "as user 1: the room's password is another") && network_room() == NULL);
    if (strstr(g_stop, "Wanda")) fprintf(stderr, "room_service: the nickname in the stop: %s\n", g_stop);
    CHECK(!strstr(g_stop, "Wanda"));
    g_opt_ldn = SNX_OPT_LDN_LAN;
    g_exedir[0] = 0;
    fake_room_stop(f);
    snx_remove_tree("room_service_app");
}

void pm_init(PMutex* m) { (void)m; }

int main(void) {
    hle_init();
    g_hle.kernel.title_id = 0; /* none, as the test was written for */
    addresses();
    passwords();
    password_written();
    stops();
    refused();
    printf("room_service: %u checks passed\n", checks);
    return 0;
}
