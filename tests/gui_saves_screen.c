/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The launcher's Saves (gui/main.c's saves_*): the menu's box of them, a game's box's Export Saves and Import Saves,
   the picker's answers taken as the system's would come, and what each says: a game's save out as Eden's zip where the
   picker said, back in after the question a save there asks, Cancel leaving it, every save of the user out and in, and
   each refusal with its values (a run going, no save, a picker that could not open) */
#define SNX_GUI_NO_MAIN
#include "gui/main.c"
#include "common/utilities/zip.h"
#include <assert.h>

#define GAME_ID "0100ABCD12340000"

/* a file of text at path, its folders made */
static void put_text(const char* path, const char* text) {
    char* folder = snx_dirname(path);
    assert(folder && !snx_mkdirs(folder) && !snx_write_file(path, text, strlen(text)));
    free(folder);
}
/* the file at path holds text */
static bool holds_text(const char* path, const char* text) {
    size_t size = 0;
    char* got = (char*)snx_read_file(path, &size);
    bool same = got && size == strlen(text) && !memcmp(got, text, size);
    free(got);
    return same;
}
/* the zip at path has a member of that name */
static bool has_member(const char* path, const char* name) {
    char error[512];
    uint32_t index;
    SnxZip* zip = snx_zip_open(path, error, sizeof error);
    bool has = zip && snx_zip_find(zip, name, &index);
    snx_zip_close(zip);
    return has;
}
/* the system picker's own answer to what was opened, which with no picker here (SDL_FILE_DIALOG_DRIVER=none) is that
   it could not open: waited for, then dropped */
static void own_answer_dropped(App* app) {
    SDL_Event own;
    Uint64 until = SDL_GetTicks() + 2000;
    int got = 0;
    while (!got && SDL_GetTicks() < until) {
        SDL_PumpEvents();
        got = SDL_PeepEvents(&own, 1, SDL_GETEVENT, app->saves_picked, app->saves_picked) == 1;
        if (!got) SDL_Delay(1);
    }
    assert(got && own.user.code);
    SDL_free(own.user.data1);
    SDL_free(own.user.data2);
}
/* what under path could not be removed, a line each with the system's reason: a file the test left open shows */
static void removal_said(const char* path) {
    char** names = NULL;
    size_t count = 0, i;
    char why[256];
    if (snx_is_dir(path) && !snx_list_dir(path, &names, &count)) {
        for (i = 0; i < count; ++i) {
            char* inner = snx_path_join(path, names[i]);
            removal_said(inner);
            free(inner);
        }
        snx_free_strings(names, count);
    }
    if (snx_path_exists(path) && snx_remove_tree(path))
        fprintf(stderr, "gui_saves_screen: %s could not be removed: %s\n", path, snx_os_error(why, sizeof why));
}
/* name set where getenv reads it as well as SDL: on Windows the C runtime keeps a copy of its own */
static void set_env(const char* name, const char* value) {
#ifdef _WIN32
    assert(!_putenv_s(name, value));
#endif
    assert(!SDL_setenv_unsafe(name, value, 1));
}
/* the row of the game's box that does item (ROW_*) */
static int box_row(const App* app, int item) {
    for (int i = 0; i < addons_rows(app); ++i)
        if (addons_row(app, i) == item) return i;
    return -1;
}

int main(void) {
    App app = {0};
    char *root = snx_temp_dir(NULL), *users, *games, *game, *program, *save, *path, *out, *back, why[256], uid[33];
    char label[160];
    assert(root && SDL_Init(SDL_INIT_VIDEO));
    assert(SDL_CreateWindowAndRenderer("saves", 320, 240, 0, &app.window, &app.renderer));
    app.theme = *gui_theme_builtin("yorha");
    { /* the user's folders the test's own, never the user's: the data folder a game's save is placed by is theirs */
        char* xdg = snx_path_join(root, "xdg");
        set_env("HOME", root);
        set_env("APPDATA", root);
        /* USERPROFILE stays: Windows makes its Documents there when the export's picker asks for it (SDL_GetUserFolder),
           a folder no one may delete, and the data folder is APPDATA's */
        set_env("XDG_DATA_HOME", xdg);
        set_env("XDG_CONFIG_HOME", xdg);
        set_env("XDG_STATE_HOME", xdg);
        set_env("XDG_CACHE_HOME", xdg);
        free(xdg);
    }
    users = snx_path_join(root, "users");
    assert(!snx_users_load(users, 1, why, sizeof why)); /* Player, user 1 */
    snx_users_uid_text(snx_users_selected()->uid, uid);
    app.data_dir = snx_user_data_dir();
    assert(app.data_dir && snx_path_within(app.data_dir, root) && !snx_mkdirs(app.data_dir));
    games = snx_path_join(app.data_dir, "recompiled");
    app.games_dir = snx_strdup(games);
    game = snx_path_join(games, GAME_ID);
    program = snx_path_join(game, GAME_ID);
    put_text(program, "a recompiled game");
    path = snx_format("%s/%s_data", game, GAME_ID);
    assert(!snx_mkdirs(path));
    free(path);
    save = snx_format("%s/save/user.%s/%s", app.data_dir, uid, GAME_ID);
    snx_path_native(save);
    path = snx_path_join(save, "0/progress.bin");
    put_text(path, "level 7");
    free(path);
    path = snx_path_join(save, "snx-journaled");
    put_text(path, "");
    free(path);

    /* the menu's Saves: Export All Saves and Import All Saves, then the Switch's two, the user named over them */
    saves_open(&app);
    assert(app.picks_shown && app.picks_of == GUI_PICKS_SAVES && app.picks.count == 12);
    assert(!strcmp(pick_label(&app, 0), "Export All Saves") && !strcmp(pick_label(&app, 1), "Import All Saves") &&
           !strcmp(pick_label(&app, 2), "Switch: Export Saves") &&
           !strcmp(pick_label(&app, 3), "Switch: Import Saves") && !strcmp(pick_label(&app, 4), "3DS: Export Saves") &&
           !strcmp(pick_label(&app, 5), "3DS: Import Saves") && !strcmp(pick_label(&app, 6), "Vita: Export Saves") &&
           !strcmp(pick_label(&app, 7), "Vita: Import Saves") && !strcmp(pick_label(&app, 8), "GBA: Export Saves") &&
           !strcmp(pick_label(&app, 9), "GBA: Import Saves") && !strcmp(pick_label(&app, 10), "NDS: Export Saves") &&
           !strcmp(pick_label(&app, 11), "NDS: Import Saves") && !pick_label(&app, 12));
    assert(pick_icon(&app, 0) == GUI_ICON_SHARE_2 && pick_icon(&app, 1) == GUI_ICON_FOLDER_INPUT);
    {
        char question[GUI_PICKS_QUESTION];
        const char* tab;
        picks_question(&app, question, sizeof question, &tab);
        assert(!strcmp(tab, "Saves") && strstr(question, "The saves of Player, the user playing"));
    }
    app.picks_shown = false;

    /* the game's box: Export Saves and Import Saves after Open Folder, each with its icon */
    app.addons_game = snx_strdup(program);
    app.addons_title = snx_strdup("A Game");
    app.addons_shown = true;
    assert(box_row(&app, ROW_EXPORT_SAVES) == box_row(&app, ROW_OPEN_FOLDER) + 1 &&
           box_row(&app, ROW_IMPORT_SAVES) == box_row(&app, ROW_OPEN_FOLDER) + 2 &&
           box_row(&app, ROW_STATES) == box_row(&app, ROW_OPEN_FOLDER) + 3);
    addons_label(&app, box_row(&app, ROW_EXPORT_SAVES), label, sizeof label);
    assert(!strcmp(label + strlen(app.theme.marker), "Export Saves"));
    addons_label(&app, box_row(&app, ROW_IMPORT_SAVES), label, sizeof label);
    assert(!strcmp(label + strlen(app.theme.marker), "Import Saves"));
    assert(addons_icon(&app, box_row(&app, ROW_EXPORT_SAVES)) == GUI_ICON_SHARE_2 &&
           addons_icon(&app, box_row(&app, ROW_IMPORT_SAVES)) == GUI_ICON_FOLDER_INPUT);

    /* Export Saves: the box closed, the picker opened for the game; its answer a file, which gets Eden's zip of the
       game's last commit, said with what went where; the copy in the data folder gone */
    addons_press(&app, box_row(&app, ROW_EXPORT_SAVES));
    assert(!app.addons_shown && app.saves_game && !strcmp(app.saves_game, program) &&
           app.saves_doing == GUI_SAVES_ROW(GUI_SWITCH, false) && app.saves_targets.count == 1 &&
           !strcmp(app.saves_targets.at[0].folder, save));
    own_answer_dropped(&app);
    out = snx_path_join(root, "a game's saves.zip");
    saves_picked_answer(&app, out, NULL, false);
    assert(has_member(out, GAME_ID "/") && has_member(out, GAME_ID "/progress.bin"));
    path = snx_format("Export Saves: 1 files (1 KB) of A Game in %s", out);
    assert(!strcmp(app.message, path));
    free(path);
    path = snx_path_join(app.data_dir, GUI_SAVES_EXPORTING);
    assert(!snx_path_exists(path) && !app.saves_game && !app.saves_targets.count);
    free(path);

    /* Import Saves over the save there: asked first, naming the game, the user, the save's folder and the trash;
       Cancel leaves it, the file read in gone */
    path = snx_path_join(save, "0/progress.bin");
    put_text(path, "level 9, played on");
    app.addons_game = snx_strdup(program);
    app.addons_title = snx_strdup("A Game");
    app.addons_shown = true;
    addons_press(&app, box_row(&app, ROW_IMPORT_SAVES));
    own_answer_dropped(&app);
    saves_picked_answer(&app, out, NULL, false);
    assert(app.confirm.shown && app.confirm.ask == GUI_ASK_SAVES);
    back = snx_path_join(app.data_dir, "trash");
    {
        char* want =
            snx_format("Import Saves: A Game has a save of Player, in %s. It goes to the trash, %s, and the one "
                       "in %s takes its place.",
                       save, back, out);
        assert(!strcmp(app.ask_line, want));
        free(want);
    }
    ask_key(&app, GUI_CANCEL);
    assert(!app.confirm.shown && holds_text(path, "level 9, played on") && !app.saves_local);
    assert(strstr(app.message, "Import Saves: cancelled, nothing imported from ") && strstr(app.message, out));
    {
        char* copy = snx_path_join(app.data_dir, GUI_SAVES_IMPORTING);
        assert(!snx_path_exists(copy));
        free(copy);
    }

    /* again, Import taken: the zip's save in its place, the one before in the trash, said with both */
    app.addons_game = snx_strdup(program);
    app.addons_title = snx_strdup("A Game");
    app.addons_shown = true;
    addons_press(&app, box_row(&app, ROW_IMPORT_SAVES));
    own_answer_dropped(&app);
    saves_picked_answer(&app, out, NULL, false);
    assert(app.confirm.shown && app.confirm.ask == GUI_ASK_SAVES);
    ask_key(&app, GUI_RIGHT); /* Import, the second choice */
    ask_key(&app, GUI_SELECT);
    assert(!app.confirm.shown && holds_text(path, "level 7"));
    assert(!strncmp(app.message, "Import Saves: 1 files (1 KB) of A Game for Player; the save there before is in ",
                    strlen("Import Saves: 1 files (1 KB) of A Game for Player; the save there before is in ")));
    {
        char *backup = app.message + strlen("Import Saves: 1 files (1 KB) of A Game for Player; the save there "
                                            "before is in "),
             *kept = snx_path_join(backup, "0/progress.bin");
        assert(!strncmp(backup, back, strlen(back)) && holds_text(kept, "level 9, played on"));
        free(kept);
    }
    free(path);

    /* a run going: an import refused, saying why; an export goes on */
    app.menu.running = true;
    saves_start(&app, GUI_SAVES_ROW(GUI_SWITCH, true), NULL, NULL);
    assert(!strcmp(app.message, "Switch: Import Saves: a game runs, which could write over its saves: close it, then "
                                "import") &&
           !app.saves_targets.count);
    app.menu.running = false;

    /* the picker that could not open, or nothing picked: said, nothing kept for an answer */
    saves_start(&app, GUI_SAVES_ROW(GUI_SWITCH, false), NULL, NULL);
    own_answer_dropped(&app);
    saves_picked_answer(&app, NULL, "no portal", true);
    assert(!strcmp(app.message, "Switch: Export Saves: the file picker could not open: no portal") &&
           !app.saves_targets.count);
    app.confirm.shown = false; /* the failure's box */
    saves_start(&app, GUI_SAVES_ROW(GUI_SAVES_EVERY, false), NULL, NULL);
    own_answer_dropped(&app);
    saves_picked_answer(&app, NULL, NULL, false);
    assert(!strcmp(app.message, "Export All Saves: no file was picked."));

    /* Export All Saves: the Switch's under switch/; Import All Saves of it into a data folder with none, asking
       nothing, each save where its game plays it */
    {
        char *every = snx_path_join(root, "every.zip"), *was = app.data_dir, *other = snx_path_join(root, "other");
        char *moved = snx_format("%s/save/user.%s/%s/0/progress.bin", other, uid, GAME_ID), *list[1];
        saves_start(&app, GUI_SAVES_ROW(GUI_SAVES_EVERY, false), NULL, NULL);
        own_answer_dropped(&app);
        saves_picked_answer(&app, every, NULL, false);
        assert(has_member(every, "switch/" GAME_ID "/progress.bin"));
        path = snx_format("Export All Saves: 1 files (1 KB) of 1 saves of Player in %s", every);
        assert(!strcmp(app.message, path));
        free(path);
        app.data_dir = other;
        list[0] = app.games_dir;
        app.games_dir = NULL; /* no game of it in the library: save_layout=global places it by its title ID */
        saves_start(&app, GUI_SAVES_ROW(GUI_SAVES_EVERY, true), NULL, NULL);
        own_answer_dropped(&app);
        saves_picked_answer(&app, every, NULL, false);
        snx_path_native(moved);
        assert(!app.confirm.shown && holds_text(moved, "level 7"));
        assert(!strcmp(app.message, "Import All Saves: 1 files (1 KB) of " GAME_ID " for Player; there was no save "
                                    "before"));
        /* a zip that is none of every console's: refused naming it, nothing written */
        saves_start(&app, GUI_SAVES_ROW(GUI_SAVES_EVERY, true), NULL, NULL);
        own_answer_dropped(&app);
        saves_picked_answer(&app, out, NULL, false);
        path = snx_format("Import All Saves: %s holds none of the consoles' folders of an export of every save: "
                          "switch/, 3ds/, vita/, gba/, nds/",
                          out);
        assert(!strcmp(app.message, path));
        free(path);
        app.confirm.shown = false;
        app.games_dir = list[0];
        app.data_dir = was;
        free(moved);
        free(other);
        free(every);
    }

    /* a GBA game's .sav out as it is and back in over the one there; the GBA's every save in a zip, a .sav each, and
       in Export All Saves under gba/ beside the Switch's */
    {
        char *rom = snx_path_join(games, "A Rom"), *rom_program = snx_path_join(rom, "A Rom");
        char *sav = snx_path_join(rom, "A Rom.sav"), *ini = snx_path_join(rom, "A Rom.ini");
        char *out_sav = snx_path_join(root, "A Rom.sav"), *gba = snx_path_join(root, "gba.zip");
        char* every = snx_path_join(root, "every2.zip");
        put_text(rom_program, "a recompiled GBA game");
        put_text(ini, "[Game]\nsystem=gba\n");
        put_text(sav, "sram 1");
        saves_start(&app, GUI_SAVES_ROW(GUI_GBA, false), rom_program, "A Rom");
        assert(app.saves_raw[0].count == 1 && !strcmp(app.saves_filter.pattern, "sav"));
        own_answer_dropped(&app);
        saves_picked_answer(&app, out_sav, NULL, false);
        assert(holds_text(out_sav, "sram 1"));
        path = snx_format("Export Saves: 1 files (1 KB) of A Rom in %s", out_sav);
        assert(!strcmp(app.message, path));
        free(path);
        put_text(out_sav, "sram 2");
        saves_start(&app, GUI_SAVES_ROW(GUI_GBA, true), rom_program, "A Rom");
        own_answer_dropped(&app);
        saves_picked_answer(&app, out_sav, NULL, false);
        assert(app.confirm.shown && app.confirm.ask == GUI_ASK_SAVES && strstr(app.ask_line, sav));
        ask_key(&app, GUI_RIGHT);
        ask_key(&app, GUI_SELECT);
        assert(holds_text(sav, "sram 2") &&
               strstr(app.message, "Import Saves: 1 files (1 KB) of A Rom for Player; the save there before is in "));
        saves_start(&app, GUI_SAVES_ROW(GUI_GBA, false), NULL, NULL);
        own_answer_dropped(&app);
        saves_picked_answer(&app, gba, NULL, false);
        assert(has_member(gba, "A Rom.sav") && !has_member(gba, GAME_ID "/"));
        saves_start(&app, GUI_SAVES_ROW(GUI_SAVES_EVERY, false), NULL, NULL);
        own_answer_dropped(&app);
        saves_picked_answer(&app, every, NULL, false);
        assert(has_member(every, "gba/A Rom.sav") && has_member(every, "switch/" GAME_ID "/progress.bin"));
        path = snx_format("Export All Saves: 2 files (1 KB) of 2 saves of Player in %s", every);
        assert(!strcmp(app.message, path));
        free(path);
        /* the GBA's zip back in: A Rom's save over the one there, asked first */
        put_text(sav, "sram 3");
        saves_start(&app, GUI_SAVES_ROW(GUI_GBA, true), NULL, NULL);
        own_answer_dropped(&app);
        saves_picked_answer(&app, gba, NULL, false);
        assert(app.confirm.shown && strstr(app.ask_line, "GBA: Import Saves: 1 of the saves in "));
        ask_key(&app, GUI_RIGHT);
        ask_key(&app, GUI_SELECT);
        assert(holds_text(sav, "sram 2"));
        free(every);
        free(gba);
        free(out_sav);
        free(ini);
        free(sav);
        free(rom_program);
        free(rom);
    }

    /* a 3DS game's save out as Checkpoint lays a backup out and back in over the one there, after the question; one
       that has not run yet refused, saying what to do first */
    {
        char *ctr = snx_path_join(games, "0004000000123400"), *ctr_program = snx_path_join(ctr, "0004000000123400");
        char *ctr_ini = snx_path_join(ctr, "0004000000123400.ini"), *data = snx_path_join(ctr, "savedata/game.bin");
        char *format = snx_path_join(ctr, "savedata.format"), *ctr_zip = snx_path_join(root, "3ds.zip");
        SnxZip* zip;
        char why[256];
        uint32_t i;
        bool found = false;
        put_text(ctr_program, "a recompiled 3DS game");
        put_text(ctr_ini, "[Game]\nsystem=3ds\n");
        put_text(data, "3ds 1");
        put_text(format, "0123456789abcdef0123");
        saves_start(&app, GUI_SAVES_ROW(GUI_3DS, false), ctr_program, "A Game");
        assert(app.saves_ctr.count == 1 && !strcmp(app.saves_filter.pattern, "zip"));
        own_answer_dropped(&app);
        saves_picked_answer(&app, ctr_zip, NULL, false);
        zip = snx_zip_open(ctr_zip, why, sizeof why);
        for (i = 0; zip && i < snx_zip_count(zip); ++i)
            found = found || (!strncmp(snx_zip_name(zip, i), "saves/0x01234 A Game/", 21) &&
                              strstr(snx_zip_name(zip, i), "/game.bin"));
        snx_zip_close(zip);
        assert(found);
        put_text(data, "3ds 2");
        saves_start(&app, GUI_SAVES_ROW(GUI_3DS, true), ctr_program, "A Game");
        own_answer_dropped(&app);
        saves_picked_answer(&app, ctr_zip, NULL, false);
        assert(app.confirm.shown && app.confirm.ask == GUI_ASK_SAVES);
        ask_key(&app, GUI_RIGHT);
        ask_key(&app, GUI_SELECT);
        assert(holds_text(data, "3ds 1") && strstr(app.message, "Import Saves: 1 files (1 KB) of A Game for Player"));
        assert(!snx_remove_tree(format));
        saves_start(&app, GUI_SAVES_ROW(GUI_3DS, true), ctr_program, "A Game");
        own_answer_dropped(&app);
        saves_picked_answer(&app, ctr_zip, NULL, false);
        if (app.confirm.shown && app.confirm.ask == GUI_ASK_SAVES) { /* a save there: asked, then refused */
            ask_key(&app, GUI_RIGHT);
            ask_key(&app, GUI_SELECT);
        }
        path = snx_format("Import Saves: 0 saves imported, then: A Game has not run yet: its first run makes its save "
                          "(%s), which an import goes into, else the game formats it over what came in. Play it once, "
                          "then import",
                          format);
        assert(!strcmp(app.message, path) && holds_text(data, "3ds 1"));
        free(path);
        app.confirm.shown = false;
        free(ctr_zip);
        free(format);
        free(data);
        free(ctr_ini);
        free(ctr_program);
        free(ctr);
    }

    /* a Vita game's save out as <ID>/ in a zip, and back in over the one there after the question */
    {
        char *psv = snx_path_join(games, "PCSE00001"), *psv_program = snx_path_join(psv, "PCSE00001");
        char *psv_ini = snx_path_join(psv, "PCSE00001.ini"), *psv_zip = snx_path_join(root, "vita.zip");
        char* data = snx_path_join(psv, "ux0/user/00/savedata/PCSE00001/SAVEDATA.BIN");
        put_text(psv_program, "a recompiled Vita game");
        put_text(psv_ini, "[Game]\nsystem=vita\n");
        put_text(data, "vita 1");
        saves_start(&app, GUI_SAVES_ROW(GUI_VITA, false), psv_program, "A Vita Game");
        assert(app.saves_psv.count == 1);
        own_answer_dropped(&app);
        saves_picked_answer(&app, psv_zip, NULL, false);
        assert(has_member(psv_zip, "PCSE00001/SAVEDATA.BIN"));
        put_text(data, "vita 2");
        saves_start(&app, GUI_SAVES_ROW(GUI_VITA, true), psv_program, "A Vita Game");
        own_answer_dropped(&app);
        saves_picked_answer(&app, psv_zip, NULL, false);
        assert(app.confirm.shown && app.confirm.ask == GUI_ASK_SAVES);
        ask_key(&app, GUI_RIGHT);
        ask_key(&app, GUI_SELECT);
        assert(holds_text(data, "vita 1") && strstr(app.message, "Import Saves: 1 files (1 KB) of A Vita Game for "));
        free(data);
        free(psv_zip);
        free(psv_ini);
        free(psv_program);
        free(psv);
    }

    /* the Switch's export with no save of the user: said before any picker opens */
    {
        char* was = app.data_dir;
        app.data_dir = snx_path_join(root, "empty");
        free(app.games_dir);
        app.games_dir = NULL;
        saves_start(&app, GUI_SAVES_ROW(GUI_SWITCH, false), NULL, NULL);
        assert(!strcmp(app.message, "Switch: Export Saves: Player has no save of the Switch"));
        free(app.data_dir);
        app.data_dir = was;
    }

    saves_forget(&app);
    free(back);
    free(out);
    free(save);
    free(program);
    free(game);
    free(games);
    free(users);
    free(app.data_dir);
    SDL_DestroyRenderer(app.renderer);
    SDL_DestroyWindow(app.window);
    SDL_Quit();
    if (snx_remove_tree(root)) {
        removal_said(root);
        assert(!"the test's folder removed whole");
    }
    free(root);
    puts("gui_saves_screen: ok");
    return 0;
}
