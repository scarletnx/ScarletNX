/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/driver/game_name.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* snx_game_title(const char* name, const SnxNpdm* npdm, const char* input) {
    const char *start, *s;
    char *out, *dot;
    size_t n = 0;
    int input_name = 0;
    if (!name && npdm->name[0] && strcmp(npdm->name, "Application")) name = npdm->name;
    if (!name) {
        start = input;
        for (s = input; *s; ++s)
            if (*s == '/' || *s == '\\') start = s + 1;
        name = start;
        input_name = 1;
    }
    out = (char*)snx_alloc(strlen(name) + 1);
    /* Preserve UTF-8 names; exclude path delimiters and C/OS metacharacters. */
    for (s = name; *s; ++s)
        if ((unsigned char)*s >= 128 || isalnum((unsigned char)*s) || *s == '-' || *s == '_' || *s == ' ' ||
            (input_name && *s == '.'))
            out[n++] = *s;
    out[n] = 0;
    if (input_name && (dot = strrchr(out, '.')) != NULL) *dot = 0;
    n = strlen(out);
    while (n && (out[n - 1] == ' ' || out[n - 1] == '.')) out[--n] = 0;
    if (!n) {
        free(out);
        return snx_strdup("Game");
    }
    return out;
}
char* snx_game_shown(const char* name, const SnxProgram* program) {
    char shown[0x200];
    SnxU64 id;
    /* a title ID names the game's files, as the driver names a dump's game read in place; nobody calls it that */
    if ((!name || snx_title_parse(name, &id)) && snx_nacp_name(program->nacp, program->nacp_size, shown, sizeof shown))
        return snx_strdup(shown);
    return snx_game_title(name, &program->npdm, program->entry_path);
}
char* snx_game_name(const char* name, const SnxNpdm* npdm, const char* input) {
    char id[17];
    if (name || !snx_title_text(npdm->title_id, id)) return snx_game_title(name, npdm, input);
    return snx_strdup(id);
}
int snx_record_folders(const char* folder, const char* name) {
    static const char* const keys[] = {"config", "state", "cache"};
    char *base = snx_path_join(folder, name), *record = snx_format("%s.folders", base), *text = snx_strdup("");
    char *moved, *user, *line;
    int kind, rc = 0;
    for (kind = SNX_USER_CONFIG; !rc && kind <= SNX_USER_CACHE; ++kind) {
        moved = snx_user_path(folder, kind);
        user = snx_menu_dir(NULL, kind); /* the user's, or the one Folders picked */
        if (user && strcmp(moved, folder) && snx_path_within(moved, user)) {
            if (moved[strcspn(moved, "\r\n")])
                rc = snx_error("the folder %s has a line break, which %s cannot keep", moved, record);
            else if (snx_mkdirs(moved))
                rc = snx_error("cannot create %s, the folder of the game's %s files", moved,
                               keys[kind - SNX_USER_CONFIG]);
            else {
                line = snx_format("%s%s=%s\n", text, keys[kind - SNX_USER_CONFIG], moved);
                free(text);
                text = line;
            }
        }
        free(moved);
        free(user);
    }
    if (!rc && *text && snx_write_changed(record, text, strlen(text)))
        rc = snx_error("cannot write %s", record);
    else if (!rc && !*text && snx_path_exists(record) && snx_remove_tree(record))
        rc = snx_error("cannot remove %s, which names folders this game no longer keeps its files in", record);
    free(base);
    free(record);
    free(text);
    return rc;
}
