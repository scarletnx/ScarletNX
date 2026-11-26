/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* tests/config_files.h's situations for the game's .ini and its own picks as the runtime reads and writes them
   (nx/runtime/options.c): included by options_tv_mode.c, whose stand-ins of the runtime it runs over, and run as
   snx-test-options-tv-mode --config-files */
#include "config_files.h"

/* what options.c logged since, to config_say */
static void options_said(void) {
    if (g_test.said[0]) config_say("%s", g_test.said);
    g_test.said[0] = 0;
}
static int ini_load(const char* dir, char* state, size_t size) {
    char* path = snx_path_join(dir, "game.ini");
    g_test.write = 1;
    g_test.said[0] = 0;
    ini_open(path);
    options_said();
    snprintf(state, size, "volume %d", g_settings.volume);
    free(path);
    return 0;
}
/* the runtime writes the whole .ini as it opens it (a pick of the game's menu goes to its picks): written, or not */
static int ini_save(const char* dir) {
    char *path = snx_path_join(dir, "game.ini"), *text;
    size_t size;
    int failed;
    g_test.write = 1;
    g_test.said[0] = 0;
    ini_open(path);
    options_said();
    text = (char*)snx_read_file(path, &size);
    failed = !text || !strstr(text, ": ScarletNX settings\n");
    if (failed) config_say("%s was not written", path);
    free(text);
    free(path);
    return failed;
}
/* the picks beside a game's .ini of its own */
static int picks_load_ini(const char* dir, char* state, size_t size) {
    char* path = snx_path_join(dir, "game.ini");
    g_test.write = 1;
    if (!snx_is_file(path)) snx_write_file(path, "[Audio]\nvolume=100\n", 19);
    g_test.said[0] = 0;
    ini_open(path);
    options_said();
    snprintf(state, size, "volume %d", g_settings.volume);
    free(path);
    return 0;
}
static int picks_save_ini(const char* dir) {
    char *path = snx_path_join(dir, "game.ini"), *picks = snx_path_join(dir, "game.picks"), *after;
    size_t size;
    int failed;
    g_test.write = 1;
    if (!snx_is_file(path)) snx_write_file(path, "[Audio]\nvolume=100\n", 19);
    ini_open(path);
    g_test.said[0] = 0;
    ini_set("fps_limit", "45");
    ini_poll(1);
    options_said();
    after =
        (char*)snx_read_file(picks, &size); /* a failure is warned once a run, which a reading before may have been */
    failed = !after || !strstr(after, "\nfps_limit=45\n");
    if (failed) config_say("%s was not written", picks);
    free(after);
    free(picks);
    free(path);
    return failed;
}

/* one file a process: a change the menu could not write waits for the next write, which the next file's would take */
static int config_files_options(const char* id) {
    static const ConfigFile files[] = {
        {.id = "game.ini",
         .name = "game.ini",
         .seed = "[Audio]\nvolume=40\n",
         .newer = "[Audio]\nvolume=40\nsnx_newer_key=" CONFIG_SECRET "\n",
         .newer_line = "snx_newer_key=" CONFIG_SECRET,
         .newer_key = "snx_newer_key",
         /* a key names its setting whatever its section: one of a section this version does not have is kept, its line
            as it was, under [Other] */
         .section = "[Audio]\nvolume=40\n[snx newer]\nsnx_section_key=" CONFIG_SECRET "\n",
         .section_line = "snx_section_key=" CONFIG_SECRET,
         .section_key = "snx_section_key",
         .stray = "[Audio]\nstray " CONFIG_SECRET "\nvolume=40\n",
         .stray_line = 2,
         .repeated = "[Audio]\nvolume=30\nvolume=40\n",
         .repeated_key = "volume",
         .repeated_first = "30",
         .repeated_last = "40",
         .repeated_as = "[Audio]\nvolume=40\n",
         .bad = "[Audio]\nvolume=loud\n",
         .bad_given = "volume=loud",
         .bad_kept = "it keeps",
         .load = ini_load,
         .save = ini_save,
         .staged = ".tmp"},
        {.id = "game.picks",
         .name = "game.picks",
         .seed = "[Audio]\nvolume=40\n",
         .newer = "[Audio]\nvolume=40\nsnx_newer_key=" CONFIG_SECRET "\n",
         .newer_line = "snx_newer_key=" CONFIG_SECRET,
         .newer_key = "snx_newer_key",
         .section = "[Audio]\nvolume=40\n[snx newer]\nsnx_section_key=" CONFIG_SECRET "\n",
         .section_line = "snx_section_key=" CONFIG_SECRET,
         .section_key = "snx_section_key",
         .stray = "[Audio]\nstray " CONFIG_SECRET "\nvolume=40\n",
         .stray_line = 2,
         .repeated = "[Audio]\nvolume=30\nvolume=40\n",
         .repeated_key = "volume",
         .repeated_first = "30",
         .repeated_last = "40",
         .repeated_as = "[Audio]\nvolume=40\n",
         .bad = "[Audio]\nvolume=loud\n",
         .bad_given = "volume=loud",
         .bad_kept = "it keeps",
         .load = picks_load_ini,
         .save = picks_save_ini,
         .staged = ".tmp"},
    };
    for (size_t i = 0; i < sizeof files / sizeof *files; ++i)
        if (!strcmp(files[i].id, id)) return config_files_run(&files[i], 1);
    fprintf(stderr, "options_tv_mode: --config-files %s: no such file (game.ini, game.picks)\n", id);
    return 2;
}
