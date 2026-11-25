/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Eden room of ldn=room (room.c): the room= address and its port, users/online.ini's room_password (a file others
   may read refused, a mistake of it or a section refused, another version's key named and kept, none at all an empty
   password), what stops the game before any connection (ldn=room without room=), airplane mode, which keeps the console
   out of the room, and a room that refuses the console */
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
static char g_logged[1024];
void logf_(int level, const char* format, ...) {
    size_t n = strlen(g_logged);
    va_list args;
    (void)level;
    va_start(args, format);
    vsnprintf(g_logged + n, sizeof g_logged - n, format, args);
    va_end(args);
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
    /* a section, or a mistake of the key: refused, as it was, what it has named */
    put("[room]\nroom_password=s3cret\n", 0600);
    CHECK(set_refused("new", "has [room]"));
    put("room_pasword=1\n", 0600);
    CHECK(set_refused("new", "has room_pasword"));
    CHECK(set_refused("", "has room_pasword"));
    snx_remove_tree(DIR);
}
/* online.ini's bytes as they are in a file, on Windows too */
static void put_bytes(const char* bytes, size_t size, int mode) {
    CHECK(snx_write_file(DIR "/online.ini", bytes, size) == 0);
#ifndef _WIN32
    CHECK(chmod(DIR "/online.ini", (mode_t)mode) == 0);
#else
    (void)mode;
#endif
}
#define PUT_BYTES(text, mode) put_bytes(text, sizeof text - 1, mode)
static int file_is(const char* want) {
    size_t size;
    char* text = (char*)snx_read_file(DIR "/online.ini", &size);
    int same = text && size == strlen(want) && !memcmp(text, want, size);
    if (!same) fprintf(stderr, "room_service: online.ini is \"%s\", not \"%s\"\n", text ? text : "(none)", want);
    free(text);
    return same;
}
#define COMMENT "; the password of the Eden room ldn=room joins (docs/user/settings.md)\n"
/* a key of another version or program in online.ini (a sign-in's, whose value is a secret): the password is read past
   it, the key named and never its value; the password set, changed and taken off leaves every other line byte for byte */
static void others_kept(void) {
    static const char said[] = "users/online.ini: this version reads room_password (%s) and no other key: ";
    static const char nul[] = "a=1\0room_password=x\n";
    static const char before[] = "# another's\r\nsign_in = FAKE-TOKEN\r\n\r\nuser_name=x";
    char password[256], why[512], want[512];
    snx_remove_tree(DIR);
    CHECK(snx_mkdirs(DIR) == 0);
    put("sign_in=FAKE-TOKEN\nroom_password=s3cret\nuser_name=x\n", 0600);
    strcpy(why, "x");
    CHECK(snx_users_room_password(DIR, password, sizeof password, why, sizeof why) == 0 && !strcmp(password, "s3cret"));
    snprintf(want, sizeof want, said, "set");
    strcat(want, "sign_in, user_name");
    if (strcmp(why, want)) fprintf(stderr, "room_service: said \"%s\", not \"%s\"\n", why, want);
    CHECK(!strcmp(why, want) && !strstr(why, "FAKE"));
    put("sign_in=FAKE-TOKEN\n", 0600);
    CHECK(snx_users_room_password(DIR, password, sizeof password, why, sizeof why) == 0 && !password[0]);
    snprintf(want, sizeof want, said, "none");
    strcat(want, "sign_in");
    CHECK(!strcmp(why, want));
    /* nothing to say: nothing said */
    put("room_password=s3cret\n", 0600);
    strcpy(why, "x");
    CHECK(snx_users_room_password(DIR, password, sizeof password, why, sizeof why) == 0 && !why[0]);
    /* what is no key's name could be a secret's text: its line. Past eight, how many */
    put("FAKEtoken==\naaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa=1\nk1=1\nk2=1\nk3=1\nk4=1\nk5=1\nk6=1\nk7=1\nk8=1\n", 0600);
    CHECK(snx_users_room_password(DIR, password, sizeof password, why, sizeof why) == 0);
    CHECK(strstr(why, ": line 1, line 2, k1, k2, k3, k4, k5, k6 and 2 more") && !strstr(why, "FAKE"));
    /* set after the other lines, then in its place; taken off, the others alone; the file its owner's alone */
    put_bytes(before, strlen(before), 0644);
    CHECK(snx_users_room_password_set(DIR, "new", why, sizeof why) == 0 && online_mode() == 0600);
    snprintf(want, sizeof want, "%s\n" COMMENT "room_password=new\n", before);
    CHECK(file_is(want) && !snx_is_file(DIR "/online.ini.new"));
    CHECK(snx_users_room_password(DIR, password, sizeof password, why, sizeof why) == 0 && !strcmp(password, "new"));
    CHECK(snx_users_room_password_set(DIR, "newer", why, sizeof why) == 0);
    snprintf(want, sizeof want, "%s\n" COMMENT "room_password=newer\n", before);
    CHECK(file_is(want));
    CHECK(snx_users_room_password_set(DIR, "", why, sizeof why) == 0 && online_mode() == 0600);
    snprintf(want, sizeof want, "%s\n", before);
    CHECK(file_is(want));
    CHECK(snx_users_room_password_set(DIR, "", why, sizeof why) == 0 && file_is(want));
    /* in the middle and twice: once, where the first was */
    PUT_BYTES("\xEF\xBB\xBFroom_password = old\nb=2\n room_password=older\r\nc=3\n", 0600);
    CHECK(snx_users_room_password_set(DIR, "new", why, sizeof why) == 0);
    CHECK(file_is(COMMENT "room_password=new\nb=2\nc=3\n"));
    /* a comment of its owner's stays too; only blank lines left, the file goes */
    PUT_BYTES("; mine\nroom_password=x\n", 0600);
    CHECK(snx_users_room_password_set(DIR, "", why, sizeof why) == 0 && file_is("; mine\n"));
    PUT_BYTES("\n \t\r\nroom_password=x\n\n", 0600);
    CHECK(snx_users_room_password_set(DIR, "", why, sizeof why) == 0 && !snx_is_file(DIR "/online.ini"));
    /* no text, which its reading stops in: refused, as it was */
    PUT_BYTES(nul, 0600);
    CHECK(set_refused("new", "its byte 3 is NUL"));
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
    CHECK(snx_mkdirs("room_service_app/" SNX_PORTABLE_DIR "/users") == 0);
    CHECK(snx_write_file("room_service_app/" SNX_PORTABLE_DIR "/users/online.ini", "sign_in=FAKE-TOKEN\n", 19) == 0);
#ifndef _WIN32
    CHECK(chmod("room_service_app/" SNX_PORTABLE_DIR "/users/online.ini", 0600) == 0);
#endif
    g_opt_ldn = SNX_OPT_LDN_ROOM;
    g_stop[0] = 0;
    g_logged[0] = 0;
    if (!setjmp(g_stopped)) {
        network_room_init();
        CHECK(!"a room that refused the password started");
    }
    CHECK(strstr(g_stop, "as user 1: the room's password is another") && network_room() == NULL);
    if (!strstr(g_logged, "sign_in")) fprintf(stderr, "room_service: logged: %s\n", g_logged);
    CHECK(!strcmp(g_logged, "room: users/online.ini: this version reads room_password (none) and no other key: "
                            "sign_in\n"));
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
    others_kept();
    stops();
    refused();
    printf("room_service: %u checks passed\n", checks);
    return 0;
}
