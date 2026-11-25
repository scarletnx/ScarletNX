/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* tests/config_files.h's situations for the settings files of common/utilities: the console's users (users.ini), the
   room's password (online.ini), the setup's (setup.ini) and the game's .ini as the driver and the launcher record a
   key in it (game_ini.c) */
#include "config_files.h"
#include "common/utilities/game_ini.h"
#include "common/utilities/users.h"
#ifdef SNX_CONFIG_SETUP
#include "common/utilities/setup.h"
#endif

/* ---- users/users.ini ---- */
static int users_load(const char* dir, char* state, size_t size) {
    char *users = snx_path_join(dir, "users"), why[1024];
    int rc = snx_users_load(users, 1, why, sizeof why), n = 0;
    if (rc) config_say("%s", why);
    if (snx_users_notes()[0]) config_say("%s", snx_users_notes());
    for (int i = 0; i < g_user_count && (size_t)n < size; ++i)
        n += snprintf(state + n, size - (size_t)n, "%s%s mii %d", i ? ", " : "", g_users[i].nickname, g_users[i].mii);
    free(users);
    return rc != 0;
}
static int users_save(const char* dir) {
    char *users = snx_path_join(dir, "users"), why[1024];
    int rc = snx_users_rename(users, 1, "Renamed", why, sizeof why);
    if (rc) config_say("%s", why);
    free(users);
    return rc;
}

/* ---- users/online.ini ---- */
static int online_load(const char* dir, char* state, size_t size) {
    char *users = snx_path_join(dir, "users"), password[256], why[1024], said[1024];
    SnxConfigOthers others;
    int rc = snx_users_room_password(users, password, sizeof password, &others, why, sizeof why);
    if (rc)
        config_say("%s", why);
    else if (snx_config_others_said(&others, SNX_USERS_ONLINE, said, sizeof said))
        config_say("%s", said);
    snprintf(state, size, "%s", rc ? "" : password[0] ? "a password" : "none"); /* never the secret itself */
    free(users);
    return rc != 0;
}
static int online_save(const char* dir) {
    char *users = snx_path_join(dir, "users"), why[1024];
    int rc = snx_users_room_password_set(users, "n3w", why, sizeof why);
    if (rc) config_say("%s", why);
    free(users);
    return rc;
}

#ifdef SNX_CONFIG_SETUP
/* ---- config/setup.ini ---- */
static SnxSetup g_setup_read;
static int setup_load(const char* dir, char* state, size_t size) {
    char *path = snx_path_join(dir, "setup.ini"), why[1024] = "";
    int rc = snx_setup_read(path, &g_setup_read, why, sizeof why);
    if (why[0]) config_say("%s", why);
    snprintf(state, size, "version %d notices %d", g_setup_read.version, g_setup_read.notices);
    free(path);
    return rc != 0;
}
static int setup_save(const char* dir) {
    char *path = snx_path_join(dir, "setup.ini"), why[1024];
    SnxSetup setup = g_setup_read;
    int rc = snx_setup_done(path, &setup, why, sizeof why);
    if (rc) config_say("%s", why);
    free(path);
    return rc;
}
#endif

/* ---- a game's .ini as the driver and the launcher record source= in it ---- */
static int record_load(const char* dir, char* state, size_t size) {
    char *path = snx_path_join(dir, "game.ini"), *value = snx_ini_value(path, "source");
    snprintf(state, size, "source %s", value ? value : "none");
    free(value);
    free(path);
    return 0;
}
static int record_save(const char* dir) {
    char* path = snx_path_join(dir, "game.ini");
    int rc = snx_ini_record(path, "source", "/new dump.nsp");
    free(path);
    return rc;
}

static const ConfigFile files[] = {
    {.id = "users.ini",
     .name = "users/users.ini",
     .seed = "; the console's users\n[user1]\nnickname=Aogo\n",
     .newer = "; the console's users\n[user1]\nnickname=Aogo\nsnx_newer_key=" CONFIG_SECRET "\n",
     .newer_line = "snx_newer_key=" CONFIG_SECRET,
     .newer_key = "snx_newer_key",
     .section = "; the console's users\n[user1]\nnickname=Aogo\n[snx newer]\nsnx_section_key=" CONFIG_SECRET "\n",
     .section_head = "[snx newer]",
     .section_line = "snx_section_key=" CONFIG_SECRET,
     .section_key = "snx_section_key",
     .stray = "; the console's users\n[user1]\nnickname=Aogo\nstray " CONFIG_SECRET "\n",
     .stray_line = 4,
     .repeated = "[user1]\nnickname=Aogo\nnickname=Afugo\n",
     .repeated_key = "nickname",
     .repeated_first = "Aogo",
     .repeated_last = "Afugo",
     .repeated_as = "[user1]\nnickname=Afugo\n",
     .bad = "; the console's users\n[user1]\nnickname=Aogo\nmii=NoSuchMii\n",
     .bad_given = "NoSuchMii",
     .bad_kept = "Baguette",
     .load = users_load,
     .save = users_save,
     .staged = ".new"},
    {.id = "online.ini",
     .name = "users/online.ini",
     .secret = 1,
     .seed = "room_password=s3cret\n",
     .newer = "room_password=s3cret\nsign_in=" CONFIG_SECRET "\n",
     .newer_line = "sign_in=" CONFIG_SECRET,
     .newer_key = "sign_in",
     .section = "room_password=s3cret\n[snx newer]\nsnx_section_key=" CONFIG_SECRET "\n",
     .section_head = "[snx newer]",
     .section_line = "snx_section_key=" CONFIG_SECRET,
     .section_key = "snx_section_key",
     .stray = "room_password=s3cret\n" CONFIG_SECRET "\n",
     .stray_line = 2,
     .repeated = "room_password=f1rst\nroom_password=s3cret\n",
     .repeated_key = "room_password",
     .repeated_first = "f1rst",
     .repeated_last = "s3cret",
     .repeated_as = "room_password=s3cret\n",
     .load = online_load,
     .save = online_save,
     .staged = ".new",
     .na = {[CONFIG_BAD_VALUE] = "its one key's value is a secret, which no message says: one too long is refused by "
                                 "its length"},
     .exempt = {[CONFIG_UNKNOWN_SECTION] = "a section in a file of no sections is refused"}},
#ifdef SNX_CONFIG_SETUP
    {.id = "setup.ini",
     .name = "setup.ini",
     .seed = "[setup]\nversion = 1\nnotices = 2\n",
     .newer = "[setup]\nversion = 1\nnotices = 2\nsnx_newer_key = " CONFIG_SECRET "\n",
     .newer_line = "snx_newer_key = " CONFIG_SECRET,
     .newer_key = "snx_newer_key",
     .section = "[setup]\nversion = 1\nnotices = 2\n[snx newer]\nsnx_section_key = " CONFIG_SECRET "\n",
     .section_head = "[snx newer]",
     .section_line = "snx_section_key = " CONFIG_SECRET,
     .section_key = "snx_section_key",
     .stray = "[setup]\nversion = 1\nstray " CONFIG_SECRET "\nnotices = 2\n",
     .stray_line = 3,
     .repeated = "[setup]\nversion = 1\nnotices = 1\nnotices = 2\n",
     .repeated_key = "notices",
     .repeated_first = "1",
     .repeated_last = "2",
     .repeated_as = "[setup]\nversion = 1\nnotices = 2\n",
     .bad = "[setup]\nversion = 1\nnotices = many\n",
     .bad_given = "notices = many",
     .bad_kept = "notices = 0",
     .load = setup_load,
     .save = setup_save,
     .staged = ".new"},
#endif
    {.id = "game.ini record",
     .name = "game.ini",
     .seed = "; a game's settings\n[Display]\nlanguage=2\n[Game]\nsource=/old dump.nsp\n",
     .load = record_load,
     .save = record_save,
     .staged = ".tmp",
     .na = {[CONFIG_UNKNOWN_KEY] = "a probe of source=: the game's .ini is its runtime's (options.c)",
            [CONFIG_UNKNOWN_SECTION] = "the same",
            [CONFIG_NO_EQUALS] = "the same",
            [CONFIG_REPEATED] = "the same",
            [CONFIG_BAD_VALUE] = "source= takes any path"}},
};

int main(void) { return config_files_run(files, sizeof files / sizeof *files); }
