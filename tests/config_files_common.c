/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* tests/config_files.h's situations for the settings files of common/utilities: the console's users (users.ini), the
   room's password (online.ini), the setup's (setup.ini) and the game's .ini as the driver and the launcher record a
   key in it (game_ini.c) */
#include "config_files.h"
#include "common/utilities/game_ini.h"
#include "common/utilities/users.h"
#include "nx/runtime/platform/joycon2_known.h"
#include "3ds/runtime/game_record.h"
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

/* ---- a game's <name>.folders, which the driver records and the runtime and the launcher read ---- */
static int folders_load(const char* dir, char* state, size_t size) {
    char *path = snx_path_join(dir, "G.folders"), *dirs[3], said[1024];
    int read = snx_folders_read(path, dirs, said, sizeof said);
    if (said[0]) config_say("%s", said);
    snprintf(state, size, "config %s state %s cache %s", dirs[0] ? dirs[0] : "-", dirs[1] ? dirs[1] : "-",
             dirs[2] ? dirs[2] : "-");
    for (int i = 0; i < 3; ++i) free(dirs[i]);
    free(path);
    return read < 0;
}

/* ---- settings.ini as the drivers' probes read it, and app.ini's [folders] as every program reads it ---- */
static void probe_take(void* ctx, const char* section, const char* key, const char* value, int line) {
    char* state = (char*)ctx;
    size_t n = strlen(state);
    (void)line;
    snprintf(state + n, 256 - n, "[%s] %s=%s; ", section, key, value);
}
static int probes_load(const char* dir, char* state, size_t size) {
    char *path = snx_path_join(dir, "settings.ini"), seen[256] = "";
    int read = snx_config_keys(path, probe_take, seen);
    snprintf(state, size, "%s", seen);
    free(path);
    return read < 0;
}
static int folders_kind_load(const char* dir, char* state, size_t size) {
    char* logs = snx_app_kind_dir(dir, SNX_USER_STATE);
    snprintf(state, size, "logs in %s", logs ? logs : "-");
    free(logs);
    return 0;
}
#ifdef SNX_CONFIG_SETUP
static int folders_kind_save(const char* dir) {
    char *path = snx_path_join(dir, "config/app.ini"), why[1024];
    int rc = snx_ini_put(path, "folders", "cache", "/elsewhere", why, sizeof why);
    if (rc) config_say("%s", why);
    free(path);
    return rc;
}
#endif

/* ---- joycon2.txt as the runtime reads and writes it ---- */
static int joycon_runtime_load(const char* dir, char* state, size_t size) {
    char* path = snx_path_join(dir, "joycon2.txt");
    Jc2KnownLine lines[8];
    int n = joycon2_known_load(path, lines, 8);
    snprintf(state, size, "%d known", n);
    free(path);
    return 0;
}
static int joycon_runtime_save(const char* dir) {
    char* path = snx_path_join(dir, "joycon2.txt");
    Jc2KnownLine line = {{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02}, 'R', "SERIAL"};
    int rc = joycon2_known_save(path, &line, 1);
    free(path);
    return rc;
}

/* ---- a 3DS game's record and its own picks ---- */
static int ctr_record_load(const char* dir, char* state, size_t size) {
    char *path = snx_path_join(dir, "0004000000000001.ini"), *source = NULL, said[1024];
    SnxCtrModel model = SNX_CTR_OLD;
    bool read = snx_ctr_record_read(path, &source, &model, NULL, said, sizeof said);
    if (said[0]) config_say("%s", said);
    snprintf(state, size, "source %s model %d", source ? source : "-", (int)model);
    free(source);
    free(path);
    return !read;
}
static int ctr_record_save(const char* dir) {
    char *path = snx_path_join(dir, "0004000000000001.ini"), why[1024];
    int rc = !snx_ctr_record_write(path, "/new game.cia", SNX_CTR_NEW, why, sizeof why);
    if (rc) config_say("%s", why);
    free(path);
    return rc;
}
static int ctr_picks_load(const char* dir, char* state, size_t size) {
    char *path = snx_path_join(dir, "0004000000000001.picks"), *console, said[1024];
    int read = snx_ctr_picks_console(path, &console, said, sizeof said);
    if (said[0]) config_say("%s", said);
    snprintf(state, size, "ctr_model %s", console ? console : "-");
    free(console);
    free(path);
    return read < 0;
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
    {.id = "game.folders",
     .name = "G.folders",
     .seed = "config=/c\nstate=/s\ncache=/k\n",
     .newer = "config=/c\nstate=/s\ncache=/k\nsnx_newer_key=" CONFIG_SECRET "\n",
     .newer_line = "snx_newer_key=" CONFIG_SECRET,
     .newer_key = "snx_newer_key",
     .section = "config=/c\nstate=/s\ncache=/k\n[snx newer]\nsnx_section_key=" CONFIG_SECRET "\n",
     .section_line = "snx_section_key=" CONFIG_SECRET,
     .section_key = "snx_section_key",
     .stray = "config=/c\nstray " CONFIG_SECRET "\nstate=/s\ncache=/k\n",
     .stray_line = 2,
     .repeated = "config=/c\nstate=/old\nstate=/s\ncache=/k\n",
     .repeated_key = "state",
     .repeated_first = "/old",
     .repeated_last = "/s",
     .repeated_as = "config=/c\nstate=/s\ncache=/k\n",
     .load = folders_load,
     .na = {[CONFIG_BAD_VALUE] =
                "a folder is any path: one not there is said where the game makes it (main.c kind_dir)"}},
    {.id = "settings.ini probes",
     .name = "settings.ini",
     .seed = "[Network]\nnetwork=1\n",
     .load = probes_load,
     .na = {[CONFIG_UNKNOWN_KEY] = "a probe of the launcher's keys: the launcher names the others (settings.ini)",
            [CONFIG_UNKNOWN_SECTION] = "the same",
            [CONFIG_NO_EQUALS] = "the same",
            [CONFIG_REPEATED] = "the same: the last line's, as the launcher takes it",
            [CONFIG_BAD_VALUE] = "each probe says a value it does not take with the one in use: 3ds_uds, the NDS "
                                 "driver's ini_read, Android's network"}},
#ifdef SNX_CONFIG_SETUP
#ifdef _WIN32
#define CONFIG_PICKED_LOGS "C:/picked/logs" /* a full path there starts with its drive */
#else
#define CONFIG_PICKED_LOGS "/picked/logs"
#endif
    {.id = "app.ini folders",
     .name = "config/app.ini",
     .seed = "[app]\ntheme = yorha\n[folders]\nstate = " CONFIG_PICKED_LOGS "\n",
     .bad = "[app]\ntheme = yorha\n[folders]\nstate = relative/logs\n",
     .bad_given = "state = relative/logs",
     .bad_kept = "the logs go in",
     .load = folders_kind_load,
     .save = folders_kind_save,
     .staged = ".new",
     .na = {[CONFIG_UNKNOWN_KEY] = "a probe of [folders]: the launcher names the others (app.ini)",
            [CONFIG_UNKNOWN_SECTION] = "the same",
            [CONFIG_NO_EQUALS] = "the same",
            [CONFIG_REPEATED] = "the same: the last line's, as the launcher takes it"}},
#endif
    {.id = "joycon2.txt runtime",
     .name = "joycon2.txt",
     .seed = "AA:BB:CC:DD:EE:01 L\n",
     .newer = "AA:BB:CC:DD:EE:01 L\nsnx newer " CONFIG_SECRET "\n",
     .newer_line = "snx newer " CONFIG_SECRET,
     .newer_key = "line 2",
     .load = joycon_runtime_load,
     .save = joycon_runtime_save,
     .staged = ".new",
     .na = {[CONFIG_UNKNOWN_SECTION] = "a file of lines: no sections",
            [CONFIG_NO_EQUALS] = "a file of lines: no =",
            [CONFIG_REPEATED] = "a file of lines: no keys",
            [CONFIG_BAD_VALUE] = "a line is a controller's address and side"}},
    {.id = "3DS game.ini",
     .name = "0004000000000001.ini",
     .seed = "source=/a game.cia\nmodel=old\nsystem=3ds\n",
     .bad = "source=/a game.cia\nmodel=newest\nsystem=3ds\n",
     .bad_given = "model=newest",
     .bad_kept = "it keeps old",
     .newer = "source=/a game.cia\nmodel=old\nsystem=3ds\nsnx_newer_key=" CONFIG_SECRET "\n",
     .newer_line = "snx_newer_key=" CONFIG_SECRET,
     .newer_key = "snx_newer_key",
     .load = ctr_record_load,
     .save = ctr_record_save,
     .staged = ".new",
     .exempt = {[CONFIG_UNKNOWN_KEY] =
                    "a probe of source= and model=: the record's other keys are its game's settings, "
                    "which their reader names; a write keeps them"},
     .na = {[CONFIG_UNKNOWN_SECTION] = "the same",
            [CONFIG_NO_EQUALS] = "the same",
            [CONFIG_REPEATED] = "the same: the last line's"}},
    {.id = "3DS game.picks",
     .name = "0004000000000001.picks",
     .seed = "[3DS]\nctr_model=old\n",
     .newer = "[3DS]\nctr_model=old\nsnx_newer_key=" CONFIG_SECRET "\n",
     .newer_key = "snx_newer_key",
     .section = "[3DS]\nctr_model=old\n[snx newer]\nsnx_section_key=" CONFIG_SECRET "\n",
     .section_key = "snx_section_key",
     .stray = "[3DS]\nstray " CONFIG_SECRET "\nctr_model=old\n",
     .stray_line = 2,
     .repeated = "[3DS]\nctr_model=new\nctr_model=old\n",
     .repeated_key = "ctr_model",
     .repeated_first = "new",
     .repeated_last = "old",
     .repeated_as = "[3DS]\nctr_model=old\n",
     .load = ctr_picks_load,
     .na = {[CONFIG_BAD_VALUE] = "the driver says a ctr_model it does not take with the console it keeps "
                                 "(3ds_console.py)"}},
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
