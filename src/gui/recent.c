/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/recent.h"
#include "common/utilities/common.h"
#include "common/platform/file.h"
#include "common/utilities/config_file.h"
#include "gui/model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* fits its field, and one field of a line: no tab or line break */
static int fits(const char* text, size_t size) { return strlen(text) < size && !strpbrk(text, "\t\r\n"); }
static void name_of(const char* path, char* out, size_t size) {
    const char *base = snx_path_leaf(path), *dot = strrchr(base, '.');
    size_t n = dot && dot != base ? (size_t)(dot - base) : strlen(base);
    if (n >= size) n = size - 1;
    memcpy(out, base, n);
    out[n] = 0;
}
int gui_recent_add(GuiRecent* recent, const char* path, int driver, int prepared) {
    GuiRecentGame game;
    int i;
    if ((driver != GUI_SWITCH && driver != GUI_LINUX && driver != GUI_3DS && driver != GUI_VITA && driver != GUI_GBA &&
         driver != GUI_NDS && driver != GUI_ANDROID) ||
        !fits(path, sizeof game.path))
        return 1;
    memset(&game, 0, sizeof game);
    strcpy(game.path, path);
    name_of(path, game.name, sizeof game.name);
    game.driver = driver;
    game.prepared = prepared; /* a game made, whichever console's, or its dump or package */
    for (i = 0; i < recent->count; ++i)
        if (!strcmp(recent->game[i].path, path)) {
            if (recent->game[i].driver == driver) strcpy(game.name, recent->game[i].name); /* the run's name stays */
            gui_recent_remove(recent, i);
            break;
        }
    if (recent->count == GUI_RECENT_MAX) recent->count--;
    memmove(recent->game + 1, recent->game, (size_t)recent->count * sizeof *recent->game);
    recent->game[0] = game;
    recent->count++;
    return 0;
}
void gui_recent_remove(GuiRecent* recent, int index) {
    if (index < 0 || index >= recent->count) return;
    memmove(recent->game + index, recent->game + index + 1, (size_t)(recent->count - index - 1) * sizeof *recent->game);
    recent->count--;
}
int gui_recent_rename(GuiRecent* recent, const char* path, const char* name) {
    int i;
    for (i = 0; i < recent->count; ++i)
        if (!strcmp(recent->game[i].path, path)) {
            if (!*name || !fits(name, sizeof recent->game[i].name) || !strcmp(recent->game[i].name, name)) return 0;
            strcpy(recent->game[i].name, name);
            return 1;
        }
    return 0;
}
int gui_recent_made(GuiRecent* recent, int index, const char* path) {
    int i;
    if (index < 0 || index >= recent->count || !fits(path, sizeof recent->game[index].path)) return 0;
    for (i = 0; i < recent->count; ++i)
        if (i != index && (!strcmp(recent->game[i].path, path) || snx_same_file(recent->game[i].path, path))) {
            gui_recent_remove(recent, i < index ? index : i); /* the program's own entry, when newer, stays */
            if (i < index) return 1;
            break;
        }
    strcpy(recent->game[index].path, path);
    recent->game[index].prepared = 1;
    return 1;
}
void gui_recent_load(GuiRecent* recent, const char* file) {
    char *data, *line, *next, *field[4];
    size_t size, at = 0;
    int i, n, number = 0;
    GuiRecentGame* game;
    SnxConfigOthers others = {0};
    recent->count = 0;
    recent->other[0] = recent->said[0] = 0;
    if (snx_config_read(file, &data, &size, recent->said, sizeof recent->said) <= 0) return; /* said, or none */
    for (line = data; line && *line; line = next) {
        size_t length;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        ++number;
        length = strlen(line);
        if (*line && line[strlen(line) - 1] == '\r') line[strlen(line) - 1] = 0;
        field[0] = line;
        for (n = 1; n < 4 && (field[n] = strchr(field[n - 1], '\t')) != NULL; ++n) *field[n]++ = 0;
        if (n < 4 || strchr(field[3], '\t') ||
            (strcmp(field[0], "switch") && strcmp(field[0], "linux") && strcmp(field[0], "3ds") &&
             strcmp(field[0], "vita") && strcmp(field[0], "gba") && strcmp(field[0], "nds") &&
             strcmp(field[0], "android")) ||
            (strcmp(field[1], "file") && strcmp(field[1], "games"))) {
            /* a line of another version's: kept as it was, tabs put back, and named by its line */
            for (i = 1; i < n; ++i) field[i][-1] = '\t';
            if (*line && at + length + 2 < sizeof recent->other) {
                at += (size_t)snprintf(recent->other + at, sizeof recent->other - at, "%s\n", line);
                snx_config_other_key(&others, "", "", number);
            }
            continue;
        }
        if (recent->count == GUI_RECENT_MAX) continue;
        game = &recent->game[recent->count];
        if (!fits(field[2], sizeof game->path) || !fits(field[3], sizeof game->name) || !snx_is_file(field[2]))
            continue;
        for (i = 0; i < recent->count && strcmp(recent->game[i].path, field[2]); ++i);
        if (i < recent->count) continue;
        memset(game, 0, sizeof *game);
        strcpy(game->path, field[2]);
        if (*field[3])
            strcpy(game->name, field[3]);
        else
            name_of(game->path, game->name, sizeof game->name);
        game->driver = field[0][0] == 's'   ? GUI_SWITCH
                       : field[0][0] == '3' ? GUI_3DS
                       : field[0][0] == 'v' ? GUI_VITA
                       : field[0][0] == 'g' ? GUI_GBA
                       : field[0][0] == 'n' ? GUI_NDS
                       : field[0][0] == 'a' ? GUI_ANDROID
                                            : GUI_LINUX;
        game->prepared = field[1][0] == 'g';
        recent->count++;
    }
    snx_config_others_said(&others, file, recent->said, sizeof recent->said);
    free(data);
}
int gui_recent_save(const GuiRecent* recent, const char* file) {
    char *text, why[1024];
    size_t size, at;
    int i, failed;
    size = 1 + strlen(recent->other);
    for (i = 0; i < recent->count; ++i) size += strlen(recent->game[i].path) + strlen(recent->game[i].name) + 16;
    text = (char*)snx_alloc(size);
    at = 0;
    for (i = 0; i < recent->count; ++i)
        at += (size_t)sprintf(text + at, "%s\t%s\t%s\t%s\n",
                              recent->game[i].driver == GUI_SWITCH ? "switch"
                              : recent->game[i].driver == GUI_3DS  ? "3ds"
                              : recent->game[i].driver == GUI_VITA ? "vita"
                              : recent->game[i].driver == GUI_GBA  ? "gba"
                              : recent->game[i].driver == GUI_NDS  ? "nds"
                              : recent->game[i].driver == GUI_ANDROID ? "android"
                                                                   : "linux",
                              recent->game[i].prepared ? "games" : "file", recent->game[i].path, recent->game[i].name);
    at += (size_t)sprintf(text + at, "%s", recent->other); /* another version's lines, as they were */
    /* staged beside it, then put in place: a crash mid-write leaves the old list; one that cannot be read stays */
    failed = snx_config_write(file, ".new", text, at, why, sizeof why);
    free(text);
    return failed;
}
