/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* tests/config_files.h's situations for the launcher's settings files (gui/): settings.ini and a game's own picks as
   its Settings read and write them (settings.c), app.ini, folders.txt and removed.txt (main.c), a theme.ini (theme.c and
   the theme editor), recent.txt (recent.c) and joycon2.txt (bluetooth.c). What a reading says is what the launcher
   shows or logs: its message, the Settings' note, its log */
#define SNX_GUI_NO_MAIN
#include "gui/main.c"
#include "config_files.h"

/* the runtime's settings as scarletnx --settings lists them, a few */
static const char list[] =
    "Display\tdocked\t1\tint\t0\t1\tTV mode\tTV mode\t0=Off|1=On\n"
    "Display\tlanguage\t1\tint\t0\t17\tGame language\tLanguage\t\n"
    "Controllers\tgamepad_rumble\t100\tint\t0\t100\tHow strongly\tVibration\t0=Off|50=50%|100=100%\n";
static App g_app;
static char g_console[SNX_PATH_SIZE];
static size_t g_logged; /* the launcher's log read so far */

/* what the launcher said since: its message, its note and its log's new lines */
static void said_since(void) {
    size_t size = 0;
    char* text = (char*)snx_read_file(g_console, &size);
    if (g_app.message[0]) config_say("%s", g_app.message);
    if (g_app.editor.note[0]) config_say("%s", g_app.editor.note);
    if (text && size > g_logged) config_say("%s", text + g_logged);
    g_logged = text ? size : 0;
    g_app.message[0] = g_app.editor.note[0] = 0;
    free(text);
}

/* ---- settings.ini and a game's own picks ---- */
static int settings_any(const char* dir, char* state, size_t size, const char* name, int game) {
    GuiSettings s = {0};
    char *path = snx_path_join(dir, name), old[1024];
    const char* v;
    if (game ? gui_settings_read_game(&s, list) : gui_settings_read(&s, list)) return 1;
    gui_settings_mark(&s);
    if (gui_settings_load(&s, path, old, sizeof old)) config_say("%s: %s", path, old); /* the log, the note */
    const char* docked = gui_settings_value(&s, "docked");
    v = gui_settings_value(&s, "language");
    snprintf(state, size, "language %s docked %s", v ? v : "-", docked ? docked : "-");
    gui_settings_free(&s);
    free(path);
    return 0;
}
static int settings_save_any(const char* dir, const char* name, int game) {
    GuiSettings s = {0};
    char *path = snx_path_join(dir, name), old[1024];
    int rc;
    if (game ? gui_settings_read_game(&s, list) : gui_settings_read(&s, list)) return 1;
    gui_settings_mark(&s);
    gui_settings_load(&s, path, old, sizeof old);
    rc = !gui_settings_set(&s, "language", "9") || gui_settings_save(&s, path);
    if (rc) config_say("%s could not be written", path);
    gui_settings_free(&s);
    free(path);
    return rc;
}
static int settings_load(const char* dir, char* state, size_t size) {
    return settings_any(dir, state, size, "settings.ini", 0);
}
static int settings_save(const char* dir) { return settings_save_any(dir, "settings.ini", 0); }
static int picks_load(const char* dir, char* state, size_t size) {
    return settings_any(dir, state, size, "game.picks", 1);
}
static int picks_save(const char* dir) { return settings_save_any(dir, "game.picks", 1); }

/* ---- app.ini ---- */
static void app_fresh(void) {
    games_defaults(&g_app);
    g_app.side_box = 0;
    g_app.language = 0;
    g_app.animations_off = false;
    g_app.dumps_shallow = g_app.dumps_off = g_app.recompiled_off = g_app.probe_full = false;
    g_app.background_mode = 0;
    g_app.gpu_driver[0] = g_app.freedreno[0] = 0;
}
static int app_ini_load(const char* dir, char* state, size_t size) {
    free(g_app.app_file);
    g_app.app_file = snx_path_join(dir, "app.ini");
    app_fresh();
    app_load(&g_app);
    said_since();
    snprintf(state, size, "theme %s side %s language %d animations %d", g_app.theme.id, side_words[g_app.side_box],
             g_app.language, !g_app.animations_off);
    return 0;
}
static int app_ini_save(const char* dir) {
    int rc;
    free(g_app.app_file);
    g_app.app_file = snx_path_join(dir, "app.ini");
    g_app.side_box = 2;
    rc = app_save(&g_app);
    said_since();
    return rc;
}

/* ---- folders.txt and removed.txt ---- */
static int folders_ini_load(const char* dir, char* state, size_t size) {
    size_t n = 0;
    free(g_app.folders_file);
    g_app.folders_file = snx_path_join(dir, "folders.txt");
    for (int i = 0; i < g_app.folder_count; ++i) free(g_app.folders[i]);
    g_app.folder_count = 0;
    folders_load(&g_app);
    said_since();
    for (int i = 0; i < g_app.folder_count && n < size; ++i)
        n +=
            (size_t)snprintf(state + n, size - n, "%s %s; ", library_words[g_app.folders_library[i]], g_app.folders[i]);
    return 0;
}
static int folders_ini_save(const char* dir) {
    int rc;
    free(g_app.folders_file);
    g_app.folders_file = snx_path_join(dir, "folders.txt");
    folder_keep(&g_app, "/another/folder", GUI_LIBRARY_DUMPS, false);
    rc = folders_save(&g_app);
    said_since();
    return rc;
}
static int removed_load(const char* dir, char* state, size_t size) {
    removed_open(&g_app, snx_path_join(dir, "removed.txt"));
    said_since();
    snprintf(state, size, "%zu removed", g_app.removed_count);
    return 0;
}
static int removed_write(const char* dir) {
    int rc;
    removed_open(&g_app, snx_path_join(dir, "removed.txt"));
    removed_add(&g_app, "/another/program");
    rc = removed_save(&g_app);
    said_since();
    return rc;
}

/* ---- a theme.ini ---- */
static int theme_ini_load(const char* dir, char* state, size_t size) {
    char *themes = snx_path_join(dir, "themes"), said[2048];
    const char* dirs[1] = {themes};
    GuiTheme t;
    gui_theme_load(&t, dirs, 1, "mine", said, sizeof said);
    if (said[0]) config_say("%s", said);
    snprintf(state, size, "name %s ink %02x%02x%02x format %lu", t.name, t.ink.r, t.ink.g, t.ink.b,
             (unsigned long)t.format);
    free(themes);
    return 0;
}
static int theme_ini_save(const char* dir) {
    char *themes = snx_path_join(dir, "themes"), *path = snx_path_join(themes, "mine/theme.ini"), said[2048];
    const char* dirs[1] = {themes};
    size_t size = 0;
    char* text = (char*)snx_read_file(path, &size);
    GuiThemeFile file = {0};
    GuiEditor e;
    GuiTheme t;
    int rc;
    memset(&e, 0, sizeof e);
    gui_theme_load(&t, dirs, 1, "mine", said, sizeof said);
    if (text) gui_theme_file_read(&file, text, size);
    gui_editor_start(&e, &t, &file, "mine");
    e.theme.ink.r = 0x55;
    rc = gui_editor_save(&e, themes);
    if (rc) config_say("%s", e.note);
    gui_editor_stop(&e);
    gui_theme_file_free(&file);
    free(text);
    free(path);
    free(themes);
    return rc;
}

/* ---- last-run.ini, the record of the run the launcher started last ---- */
static int last_run_load(const char* dir, char* state, size_t size) {
    GuiLastRun run;
    char *path = snx_path_join(dir, "last-run.ini"), why[1024];
    int rc = gui_last_run_take(path, &run, why, sizeof why);
    if (rc < 0 || (rc > 0 && why[0])) config_say("%s", why); /* the launcher's log */
    snprintf(state, size, "%s started %lld", run.name ? run.name : "-", (long long)run.started);
    gui_last_run_free(&run);
    free(path);
    return 0;
}
static int last_run_save(const char* dir) {
    GuiLastRun run = {0};
    char *path = snx_path_join(dir, "last-run.ini"), why[1024];
    int rc;
    run.started = 1700000100;
    run.name = (char*)"Another game";
    rc = gui_last_run_write(path, &run, why, sizeof why);
    if (rc) config_say("%s", why);
    free(path);
    return rc;
}

/* ---- recent.txt ---- */
static int recent_load(const char* dir, char* state, size_t size) {
    GuiRecent r = {0};
    char* path = snx_path_join(dir, "recent.txt");
    size_t n = 0;
    gui_recent_load(&r, path);
    if (r.said[0]) config_say("%s", r.said); /* the launcher's log */
    for (int i = 0; i < r.count && n < size; ++i) n += (size_t)snprintf(state + n, size - n, "%s; ", r.game[i].name);
    free(path);
    return 0;
}
static int recent_write(const char* dir) {
    GuiRecent r = {0};
    char *path = snx_path_join(dir, "recent.txt"), *game = snx_path_join(dir, "..");
    int rc;
    gui_recent_load(&r, path);
    gui_recent_rename(&r, r.count ? r.game[0].path : "", "Renamed");
    rc = gui_recent_save(&r, path);
    if (rc) config_say("%s could not be written", path);
    free(game);
    free(path);
    return rc;
}

/* ---- joycon2.txt ---- */
static int joycon_load(const char* dir, char* state, size_t size) {
    GuiBt bt = {0};
    char* path = snx_path_join(dir, "joycon2.txt");
    int known = gui_bt_read(&bt, path);
    if (bt.said[0]) config_say("%s", bt.said); /* the page's note */
    snprintf(state, size, "%d known", known);
    free(path);
    return 0;
}
static int joycon_save(const char* dir) {
    GuiBt bt = {0};
    char *path = snx_path_join(dir, "joycon2.txt"), why[512];
    int rc = gui_bt_read(&bt, path) < 1 || gui_bt_forget(&bt, 0, path, why, sizeof why);
    if (rc) config_say("%s: %s", path, why);
    free(path);
    return rc;
}

/* a file of lines, an entry each: what has no key there is no situation of it */
#define NO_KEYS(unknown, why)                                                                                          \
    .na = {[CONFIG_UNKNOWN_KEY] = unknown,                                                                             \
           [CONFIG_UNKNOWN_SECTION] = "a file of lines: no sections",                                                  \
           [CONFIG_NO_EQUALS] = "a file of lines: no =",                                                               \
           [CONFIG_REPEATED] = "a file of lines: no keys",                                                             \
           [CONFIG_BAD_VALUE] = why}

int main(void) {
    static char recent_seed[SNX_PATH_SIZE + 64], recent_newer[2 * SNX_PATH_SIZE + 128], recent_line[SNX_PATH_SIZE + 64];
    static char theme_kept[64];
    char *root = snx_temp_dir(NULL), *home = snx_path_join(root, "home"), *game = snx_path_join(root, "game.nsp");
    char *logs = snx_path_join(root, "logs"), name[64];
    int rc;
    if (!root || snx_mkdirs(home) || snx_mkdirs(logs) || snx_write_file(game, "x", 1)) return 2;
    SDL_setenv_unsafe("HOME", home, 1);
    SDL_setenv_unsafe("XDG_CONFIG_HOME", home, 1);
    SDL_setenv_unsafe("XDG_DATA_HOME", home, 1);
    SDL_setenv_unsafe("XDG_STATE_HOME", home, 1);
    if (!initialize(&g_app, NULL)) return 2;
    gui_log_folder(logs);
    gui_log_console_name(name, sizeof name);
    snprintf(g_console, sizeof g_console, "%s/%s", logs, name);
    { /* the ink a theme over YoRHa keeps for one it cannot read */
        const GuiTheme* yorha = gui_theme_builtin("yorha");
        snprintf(theme_kept, sizeof theme_kept, "it keeps #%02X%02X%02X", yorha->ink.r, yorha->ink.g, yorha->ink.b);
    }
    snprintf(recent_seed, sizeof recent_seed, "switch\tfile\t%s\tA game\n", game);
    snprintf(recent_line, sizeof recent_line, "snxnewer\tfile\t%s\t" CONFIG_SECRET, game);
    snprintf(recent_newer, sizeof recent_newer, "%s%s\n", recent_seed, recent_line);
    {
        const ConfigFile files[] = {
            {.id = "settings.ini",
             .name = "settings.ini",
             .seed = "; what the launcher's Settings changed\n[Display]\ndocked=0\nlanguage=3\n",
             .newer =
                 "; what the launcher's Settings changed\n[Display]\ndocked=0\nlanguage=3\nsnx_newer_key=" CONFIG_SECRET
                 "\n",
             .newer_line = "snx_newer_key=" CONFIG_SECRET,
             .newer_key = "snx_newer_key",
             .section = "[Display]\ndocked=0\nlanguage=3\n[snx newer]\nsnx_section_key=" CONFIG_SECRET "\n",
             .section_head = "[snx newer]",
             .section_line = "snx_section_key=" CONFIG_SECRET,
             .section_key = "snx_section_key",
             .stray = "[Display]\ndocked=0\nstray " CONFIG_SECRET "\nlanguage=3\n",
             .stray_line = 3,
             .repeated = "[Display]\ndocked=0\nlanguage=2\nlanguage=3\n",
             .repeated_key = "language",
             .repeated_first = "2",
             .repeated_last = "3",
             .repeated_as = "[Display]\ndocked=0\nlanguage=3\n",
             .bad = "[Display]\ndocked=7\nlanguage=3\n",
             .bad_given = "docked=7",
             .bad_kept = "docked=1 now",
             .load = settings_load,
             .save = settings_save,
             .staged = ".new"},
            {.id = "game.picks launcher",
             .name = "game.picks",
             .seed = "[Display]\nlanguage=3\n",
             .newer = "[Display]\nlanguage=3\nsnx_newer_key=" CONFIG_SECRET "\n",
             .newer_line = "snx_newer_key=" CONFIG_SECRET,
             .newer_key = "snx_newer_key",
             .section = "[Display]\nlanguage=3\n[snx newer]\nsnx_section_key=" CONFIG_SECRET "\n",
             .section_head = "[snx newer]",
             .section_line = "snx_section_key=" CONFIG_SECRET,
             .section_key = "snx_section_key",
             .stray = "[Display]\nstray " CONFIG_SECRET "\nlanguage=3\n",
             .stray_line = 2,
             .repeated = "[Display]\nlanguage=2\nlanguage=3\n",
             .repeated_key = "language",
             .repeated_first = "2",
             .repeated_last = "3",
             .repeated_as = "[Display]\nlanguage=3\n",
             .bad = "[Display]\ndocked=7\nlanguage=3\n",
             .bad_given = "docked=7",
             .bad_kept = "docked=Global now",
             .load = picks_load,
             .save = picks_save,
             .staged = ".new"},
            {.id = "app.ini",
             .name = "app.ini",
             .seed = "; Settings > App\n[app]\ntheme = default\nside_box = on\nanimations = 0\n",
             .newer =
                 "; Settings > App\n[app]\ntheme = default\nside_box = on\nanimations = 0\nsnx_newer_key = " CONFIG_SECRET
                 "\n",
             .newer_line = "snx_newer_key = " CONFIG_SECRET,
             .newer_key = "snx_newer_key",
             .section =
                 "[app]\ntheme = default\nside_box = on\nanimations = 0\n[snx newer]\nsnx_section_key = " CONFIG_SECRET
                 "\n",
             .section_head = "[snx newer]",
             .section_line = "snx_section_key = " CONFIG_SECRET,
             .section_key = "snx_section_key",
             .stray = "[app]\ntheme = default\nstray " CONFIG_SECRET "\nside_box = on\nanimations = 0\n",
             .stray_line = 3,
             .repeated = "[app]\ntheme = default\nside_box = off\nside_box = on\nanimations = 0\n",
             .repeated_key = "side_box",
             .repeated_first = "off",
             .repeated_last = "on",
             .repeated_as = "[app]\ntheme = default\nside_box = on\nanimations = 0\n",
             .bad = "[app]\ntheme = default\nside_box = sideways\nanimations = 0\n",
             .bad_given = "side_box = sideways",
             .bad_kept = "keeps auto",
             .load = app_ini_load,
             .save = app_ini_save,
             .staged = ".new"},
            {.id = "theme.ini",
             .name = "themes/mine/theme.ini",
             .seed = "[theme]\nformat = 1\nname = Mine\nbase = yorha\n[colors]\nink = #102030\n",
             .newer =
                 "[theme]\nformat = 1\nname = Mine\nbase = yorha\n[colors]\nink = #102030\nsnx_newer_key = " CONFIG_SECRET
                 "\n",
             .newer_line = "snx_newer_key = " CONFIG_SECRET,
             .newer_key = "snx_newer_key",
             .section =
                 "[theme]\nformat = 1\nname = Mine\nbase = yorha\n[colors]\nink = #102030\n[snx newer]\nsnx_section_key = " CONFIG_SECRET
                 "\n",
             .section_head = "[snx newer]",
             .section_line = "snx_section_key = " CONFIG_SECRET,
             .section_key = "snx_section_key",
             .stray =
                 "[theme]\nformat = 1\nname = Mine\nbase = yorha\n[colors]\nstray " CONFIG_SECRET "\nink = #102030\n",
             .stray_line = 6,
             .repeated = "[theme]\nformat = 1\nname = Mine\nbase = yorha\n[colors]\nink = #405060\nink = #102030\n",
             .repeated_key = "ink",
             .repeated_first = "#405060",
             .repeated_last = "#102030",
             .repeated_as = "[theme]\nformat = 1\nname = Mine\nbase = yorha\n[colors]\nink = #102030\n",
             .bad = "[theme]\nformat = 1\nname = Mine\nbase = yorha\n[colors]\nink = #10203G\n",
             .bad_given = "ink = #10203G",
             .bad_kept = theme_kept,
             .load = theme_ini_load,
             .save = theme_ini_save,
             .staged = ".new",
             .exempt = {[CONFIG_ABSENT] = "a theme app.ini picks that is not there is said, with the theme the menu "
                                          "shows instead"}},
            {.id = "last-run.ini",
             .name = "last-run.ini",
             .seed = "; the run the menu started last\nstarted = 1700000000\nname = A game\n",
             .newer =
                 "; the run the menu started last\nstarted = 1700000000\nname = A game\nsnx_newer_key = " CONFIG_SECRET
                 "\n",
             .newer_line = "snx_newer_key = " CONFIG_SECRET,
             .newer_key = "snx_newer_key",
             .section = "started = 1700000000\nname = A game\n[snx newer]\nsnx_section_key = " CONFIG_SECRET "\n",
             .section_head = "[snx newer]",
             .section_line = "snx_section_key = " CONFIG_SECRET,
             .section_key = "snx_section_key",
             .stray = "started = 1700000000\nstray " CONFIG_SECRET "\nname = A game\n",
             .stray_line = 2,
             .repeated = "started = 1700000000\nname = Old\nname = A game\n",
             .repeated_key = "name",
             .repeated_first = "Old",
             .repeated_last = "A game",
             .repeated_as = "started = 1700000000\nname = A game\n",
             .bad = "started = soon\nname = A game\n",
             .bad_given = "started = soon",
             .bad_kept = "no run",
             .load = last_run_load,
             .save = last_run_save,
             .staged = ".new",
             .consumed = "the record is taken out as it is read, and the next run's written whole"},
            {.id = "folders.txt",
             .name = "folders.txt",
             .seed = "dumps /some/folder\n",
             .load = folders_ini_load,
             .save = folders_ini_save,
             .staged = ".new",
             NO_KEYS("every line is a folder: one in an older version's form is read as one",
                     "a line is a folder, which is not looked for")},
            {.id = "removed.txt",
             .name = "removed.txt",
             .seed = "/a/game/program\n",
             .load = removed_load,
             .save = removed_write,
             .staged = ".new",
             NO_KEYS("every line is a program's path", "a line is a program's path, kept while it is not there")},
            {.id = "recent.txt",
             .name = "recent.txt",
             .seed = recent_seed,
             .newer = recent_newer,
             .newer_line = recent_line,
             .newer_key = "line 2",
             .load = recent_load,
             .save = recent_write,
             .staged = ".new",
             NO_KEYS(NULL, "a game no longer there leaves the list, as the menu shows it")},
            {.id = "joycon2.txt",
             .name = "joycon2.txt",
             .seed = "AA:BB:CC:DD:EE:01 L\n",
             .newer = "AA:BB:CC:DD:EE:01 L\nsnx newer " CONFIG_SECRET "\n",
             .newer_line = "snx newer " CONFIG_SECRET,
             .newer_key = "line 2",
             .load = joycon_load,
             .save = joycon_save,
             .staged = ".new",
             NO_KEYS(NULL, "a line is a controller's address and side")},
        };
        rc = config_files_run(files, sizeof files / sizeof *files);
    }
    gui_log_folder(NULL);
    SDL_Quit();
    snx_remove_tree(root);
    free(logs);
    free(game);
    free(home);
    free(root);
    return rc;
}
