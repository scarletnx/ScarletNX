/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#define _POSIX_C_SOURCE 200809L
#include "nx/runtime/driver/setup.h"
#include "common/utilities/common.h"
#include "common/utilities/ini_lines.h"
#include "common/utilities/language_tag.h"
#include "common/utilities/setup.h"
#include "common/utilities/users.h"
#include <stdlib.h>
#include <string.h>

#define SETUP_LINE 4200 /* an answer, a path's room */

/* the setup under way: where it reads and writes, the data folder (a portable copy's once that is picked), and the
   step it asks of how many */
typedef struct {
    FILE *in, *out;
    const char* program_dir;
    char* data;
    int step, steps;
} Ask;

/* the next line of in, the spaces around it left out: 0, else 1 when in has ended, which out says */
static int answer(Ask* a, char* line, size_t size) {
    size_t n, start = 0;
    fflush(a->out);
    if (!fgets(line, (int)size, a->in)) {
        fputs("\nThe setup stops here: no more answers. It asks again the next time.\n", a->out);
        return 1;
    }
    n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t'))
        line[--n] = 0;
    while (line[start] == ' ' || line[start] == '\t') ++start;
    memmove(line, line + start, n - start + 1);
    return 0;
}
/* one of the choices printed, 1 to count, into *choice, Enter the fallback: 0, else 1 when in has ended */
static int choose(Ask* a, int count, int fallback, int* choice) {
    char line[64], *end;
    long n;
    for (;;) {
        fprintf(a->out, "  [%d] ", fallback);
        if (answer(a, line, sizeof line)) return 1;
        n = line[0] ? strtol(line, &end, 10) : fallback;
        if (!line[0] || (!*end && n >= 1 && n <= count)) {
            *choice = (int)n;
            return 0;
        }
        fprintf(a->out, "  %s: answer 1 to %d, or Enter for %d\n", line, count, fallback);
    }
}
/* y or n into *yes, Enter no: 0, else 1 when in has ended */
static int yes_no(Ask* a, const char* question, int* yes) {
    char line[16];
    for (;;) {
        fprintf(a->out, "  %s [y/N] ", question);
        if (answer(a, line, sizeof line)) return 1;
        if (!line[0] || !strcmp(line, "n") || !strcmp(line, "N") || !strcmp(line, "no")) {
            *yes = 0;
            return 0;
        }
        if (!strcmp(line, "y") || !strcmp(line, "Y") || !strcmp(line, "yes")) {
            *yes = 1;
            return 0;
        }
        fprintf(a->out, "  %s: answer y or n\n", line);
    }
}
/* a folder's path, from the root: 0 with it in *path (owned), else 1 when in has ended */
static int folder_answer(Ask* a, const char* question, char** path) {
    char line[SETUP_LINE];
    for (;;) {
        fprintf(a->out, "  %s: ", question);
        if (answer(a, line, sizeof line)) return 1;
        *path = line[0] ? snx_absolute(line) : NULL;
        if (*path && snx_is_dir(*path)) return 0;
        fprintf(a->out, "  %s is not a folder\n", line[0] ? line : "(nothing)");
        free(*path);
    }
}
static void heading(Ask* a, const char* title) { fprintf(a->out, "[%d/%d] %s\n", ++a->step, a->steps, title); }
static void shown(const char* path, char* out, size_t size) { snx_home_short(path, out, size); }

/* the launcher's files in the data folder's config folder: app.ini, folders.txt */
static char* menu_file(const Ask* a, const char* name) {
    char *config = snx_menu_dir(a->data, SNX_USER_CONFIG), *path = config ? snx_path_join(config, name) : NULL;
    free(config);
    return path;
}
/* app.ini's value of key in section, owned; NULL for none */
static char* app_value(const Ask* a, const char* section, const char* key) {
    char *path = menu_file(a, "app.ini"), *text, *k, *v, *found = NULL;
    size_t size;
    SnxIni ini;
    text = path && snx_is_file(path) ? (char*)snx_read_file(path, &size) : NULL;
    if (text) snx_ini_start(&ini, text);
    while (text && snx_ini_next(&ini, &k, &v))
        if (!strcmp(ini.section, section) && !strcmp(k, key) && v[0]) {
            free(found);
            found = snx_strdup(v);
        }
    free(text);
    free(path);
    return found;
}

/* the notices left: what an older version did that this one does otherwise, and OK */
static int notices(Ask* a, const SnxSetup* setup) {
    char line[16];
    int i, any = 0;
    for (i = 0; i < SNX_NOTICES; ++i) {
        const SnxNotice* n = &snx_notices[i];
        if (!snx_setup_shows(setup, i)) continue;
        if (!any++)
            fprintf(a->out, "%s\n", setup->version ? "Changed since the last version" : "Changed in older versions");
        fprintf(a->out, "  %s\n    before  %s\n    now     %s\n", n->what, n->before, n->now);
    }
    if (!any) return 0;
    fputs("  Not moved. See docs/DEPRECATIONS.md\n[Enter] OK ", a->out);
    return answer(a, line, sizeof line);
}
/* the system's language among the launcher's, by their tags' fit (language_tag.h) to LANG's locale (pt_BR.UTF-8): the
   same language and region, else the language for another one; else the first, English. 1-based */
static int language_auto(int count) {
    const char *lang = getenv("LANG"), *name;
    int i, best = 1, how = 0;
    for (i = 0; lang && i < count; ++i) {
        int fit = snx_language_fit(snx_menu_language(i, &name), lang);
        if (fit > how) {
            best = i + 1;
            how = fit;
        }
    }
    return best;
}
/* the launcher's language: the one it has (app.ini's, else the system's) or another, kept in *picked (1-based) for
   app.ini once the install's folder is known; 0 for the one it has, which app.ini keeps as it is */
static int language(Ask* a, int* picked) {
    const char* name;
    int count = 0, now, rc;
    char* set = app_value(a, "app", "language");
    heading(a, "Language");
    fputs(" ", a->out);
    while (snx_menu_language(count, &name)) {
        fprintf(a->out, " %d) %s", count + 1, name);
        ++count;
    }
    now = language_auto(count);
    for (*picked = 0; set && *picked < count; ++*picked) /* its tag, or an older form of it (pt_BR) */
        if (snx_language_fit(snx_menu_language(*picked, &name), set) == 3) now = *picked + 1;
    free(set);
    fputc('\n', a->out);
    rc = choose(a, count, now, picked);
    if (*picked == now) *picked = 0;
    return rc;
}
/* installed (the user's folders) or portable (a folder beside the program): the data folder from then on */
static int install(Ask* a) {
    static const int kinds[] = {SNX_USER_CONFIG, SNX_USER_DATA, SNX_USER_STATE, SNX_USER_CACHE};
    static const char* const names[] = {"settings", "data", "logs", "caches"};
    char *portable = snx_portable_dir(a->program_dir), path[SETUP_LINE];
    int i, choice, rc = 0;
    heading(a, "Install Type");
    fputs("  Installed:", a->out);
    for (i = 0; i < 4; ++i) {
        char* dir = snx_menu_dir(NULL, kinds[i]); /* Logs and Caches as Folders picked them */
        shown(dir ? dir : "(no home folder)", path, sizeof path);
        fprintf(a->out, "%s %s %s", i ? "," : "", names[i], path);
        free(dir);
    }
    shown(portable ? portable : "(none)", path, sizeof path);
    fprintf(a->out, "\n  Portable: everything in %s, beside the program\n  1) Installed  2) Portable\n", path);
    for (;;) {
        int there = portable && snx_is_dir(portable);
        if (choose(a, 2, there ? 2 : 1, &choice)) {
            rc = 1;
            break;
        }
        if (choice == 1 && there)
            fprintf(a->out, "  %s is there, so this copy is portable: move it away first, then answer 1\n", path);
        else if (choice == 2 && !there && (!portable || snx_mkdirs(portable)))
            fprintf(a->out, "  %s could not be made\n", path);
        else
            break;
    }
    if (!rc && choice == 2) { /* its folders from now on */
        free(a->data);
        a->data = snx_strdup(portable);
    }
    free(portable);
    return rc;
}
/* where the launcher puts the games: app.ini's recompiled =, else games = as an older version wrote it, else the data
   folder's recompiled/ */
static char* recompiled_folder(Ask* a) {
    char* games = app_value(a, "folders", "recompiled");
    if (!games || !games[0]) {
        free(games);
        games = app_value(a, "folders", "games");
    }
    if (!games || !games[0]) {
        free(games);
        games = snx_recompiled_dir(a->data);
    }
    return games;
}
/* where the launcher puts the games, and the folders it lists them from: kept, or one changed or added at a time */
static int folders(Ask* a) {
    char *app = menu_file(a, "app.ini"), *list = menu_file(a, "folders.txt"), *picked = NULL, *games, *text;
    char line[SETUP_LINE], why[SETUP_LINE + 64], *at;
    size_t size;
    int choice, rc = 0;
    heading(a, "Folders");
    for (;;) {
        games = recompiled_folder(a);
        shown(games, line, sizeof line);
        fprintf(a->out, "  Recompiled: %s\n  Dumps:", line);
        text = list && snx_is_file(list) ? (char*)snx_read_file(list, &size) : NULL;
        fputs(text && text[0] ? "\n" : " none yet\n", a->out);
        for (at = text; at && *at; at += strcspn(at, "\n") + (at[strcspn(at, "\n")] != 0))
            fprintf(a->out, "    %.*s\n", (int)strcspn(at, "\n"), at);
        free(text);
        free(games);
        fputs("  1) Keep  2) Change recompiled folder  3) Add dumps folder\n", a->out);
        if (choose(a, 3, 1, &choice)) {
            rc = 1;
            break;
        }
        if (choice == 1) break;
        if (folder_answer(a, choice == 2 ? "Recompiled folder" : "Dumps folder", &picked)) {
            rc = 1;
            break;
        }
        if (choice == 2 && snx_folder_unfit(picked, 1))
            fprintf(a->out, "  Not changed: %s %s\n", picked, snx_folder_unfit(picked, 1));
        else if (choice == 2 ? !app || snx_ini_put(app, "folders", "recompiled", picked, why, sizeof why)
                             : !list || snx_lines_add(list, picked, why, sizeof why))
            fprintf(a->out, "  %s\n", app && list ? why : "No config folder: no home folder names one");
        free(picked);
        picked = NULL;
    }
    free(list);
    free(app);
    return rc;
}
/* the keys the driver reads, and where they are installed from a folder of them */
static int keys(Ask* a) {
    char *prod, *titles, *dir = snx_keys_dir(a->data), line[SETUP_LINE], why[SETUP_LINE + 64], *folder;
    int choice, installed_titles, rc = 0;
    heading(a, "Keys");
    for (;;) {
        prod = snx_keys_found(a->data, "prod.keys");
        titles = snx_keys_found(a->data, "title.keys");
        shown(prod ? prod : "", line, sizeof line);
        fprintf(a->out, "  prod.keys %s%s\n", prod ? "found, " : "missing", line);
        shown(titles ? titles : "", line, sizeof line);
        fprintf(a->out, "  title.keys %s%s\n", titles ? "found, " : "missing: only some .nsp need it", line);
        fputs(prod ? "  1) Use these  2) Choose folder  3) Later\n" : "  1) Choose folder  2) Later\n", a->out);
        if (choose(a, prod ? 3 : 2, prod ? 1 : 2, &choice)) rc = 1;
        if (!rc && prod) --choice; /* 0: these */
        free(prod);
        free(titles);
        if (rc || choice != 1) break;
        if (folder_answer(a, "Folder with prod.keys", &folder)) {
            rc = 1;
            break;
        }
        if (!dir)
            fputs("  No home folder: nowhere to install them\n", a->out);
        else if (snx_keys_install(folder, dir, &installed_titles, why, sizeof why))
            fprintf(a->out, "  Not installed: %s\n", why);
        else {
            shown(dir, line, sizeof line);
            fprintf(a->out, "  Installed prod.keys%s in %s\n", installed_titles ? " and title.keys" : "", line);
        }
        free(folder);
    }
    free(dir);
    return rc;
}
/* the first user: users.ini kept as it is, or started over with one user, its uid the fixed one while games have
   saves of it, else a random one */
static int user(Ask* a) {
    char *users = snx_users_dir(a->data), *path = users ? snx_path_join(users, "users.ini") : NULL, *games, *old;
    char why[512], line[SETUP_LINE], hex[33];
    uint64_t uid[2];
    int i, yes = 1, saves, rc = 0;
    if (!path) {
        heading(a, "First User");
        fputs("  No data folder: no home folder names one\n", a->out);
        free(users);
        return 1;
    }
    if (snx_is_file(path)) {
        if (snx_users_load(users, 1, why, sizeof why)) {
            heading(a, "users.ini");
            fprintf(a->out, "  %s\n", why);
        } else {
            fprintf(a->out, "[%d/%d] users.ini has %d user%s:", ++a->step, a->steps, g_user_count,
                    g_user_count == 1 ? "" : "s");
            for (i = 0; i < g_user_count; ++i) fprintf(a->out, "%s %s", i ? "," : "", g_users[i].nickname);
            fputc('\n', a->out);
        }
        if (yes_no(a, "Overwrite? (users.ini.old keeps it)", &yes)) rc = 1;
    } else
        heading(a, "First User");
    /* the saves where the launcher puts the games, else in the games/ an older version left, whose games still run */
    games = recompiled_folder(a);
    old = snx_path_join(a->data, "games");
    if (!snx_setup_first_saves(games, NULL) && snx_is_dir(old)) {
        free(games);
        games = old;
        old = NULL;
    }
    saves = !rc && yes ? snx_setup_first_uid(games, a->data, uid) : 0;
    free(games);
    free(old);
    if (saves < 0) {
        fputs("  The system's random number generator failed: no uid for a new user\n", a->out);
        rc = 1;
    }
    if (!rc && yes) {
        snx_users_uid_text(uid, hex);
        fprintf(a->out, "  uid %s%s", hex, saves ? ", the uid of the saves of " : ", random\n");
        if (saves) fprintf(a->out, "%d game%s\n", saves, saves == 1 ? "" : "s");
    }
    while (!rc && yes) {
        fprintf(a->out, "  Nickname, %d to %d of A-Z a-z 0-9 space . _ - [Player] ", SNX_USERS_NICKNAME_MIN,
                SNX_USERS_NICKNAME_MAX);
        if (answer(a, line, sizeof line)) {
            rc = 1;
            break;
        }
        if (!line[0]) strcpy(line, "Player");
        if (!snx_users_nickname_valid(line))
            fprintf(a->out, "  %s is not %d to %d of A-Z a-z 0-9 space . _ -\n", line, SNX_USERS_NICKNAME_MIN,
                    SNX_USERS_NICKNAME_MAX);
        else if (snx_users_start(users, uid, line, why, sizeof why)) {
            fprintf(a->out, "  %s\n", why);
            rc = 1;
        } else
            break;
    }
    free(path);
    free(users);
    return rc;
}

/* whether step is asked here: left by setup.ini, the language with the launcher's languages to pick from, the install
   where a copy can be portable (an app has its own storage) */
static int asked(const SnxSetup* setup, int step, int languages) {
    return snx_setup_asks(setup, step) && snx_setup_here(step) && (step != SNX_SETUP_LANGUAGE || languages);
}
int snx_setup_terminal(FILE* in, FILE* out, const char* program_dir, int all) {
    char *file, why[SETUP_LINE + 64], shown_path[SETUP_LINE];
    SnxSetup setup, ask;
    Ask a;
    int i, language_picked = 0, languages = 0, rc = 0;
    const char* name;
    memset(&a, 0, sizeof a);
    a.in = in;
    a.out = out;
    a.program_dir = program_dir;
    a.data = snx_data_dir(program_dir);
    file = a.data ? snx_setup_file(a.data) : NULL;
    if (!file) {
        fputs("The setup has nowhere to go: no home folder names a data folder\n", out);
        free(a.data);
        return 1;
    }
    if (snx_setup_read(file, &setup, why, sizeof why)) fprintf(out, "%s: the setup goes through it again\n", why);
    ask = setup;
    if (all) ask.version = ask.notices = 0;
    if (!snx_setup_due(&ask)) {
        free(file);
        free(a.data);
        return 0;
    }
    while (snx_menu_language && snx_menu_language(languages, &name)) ++languages;
    for (i = 0; i < SNX_SETUP_STEPS; ++i) a.steps += asked(&ask, i, languages);
    rc = notices(&a, &ask);
    if (!rc && asked(&ask, SNX_SETUP_LANGUAGE, languages)) rc = language(&a, &language_picked);
    if (!rc && asked(&ask, SNX_SETUP_INSTALL, languages)) rc = install(&a);
    if (!rc && asked(&ask, SNX_SETUP_FOLDERS, languages)) rc = folders(&a);
    if (!rc && asked(&ask, SNX_SETUP_KEYS, languages)) rc = keys(&a);
    if (!rc && asked(&ask, SNX_SETUP_USER, languages)) rc = user(&a);
    if (!rc && language_picked) { /* in the folder the install says */
        char* app = menu_file(&a, "app.ini");
        rc =
            !app || snx_ini_put(app, "app", "language", snx_menu_language(language_picked - 1, &name), why, sizeof why);
        if (rc) fprintf(out, "%s\n", app ? why : "No config folder: no home folder names one");
        free(app);
    }
    if (!rc) { /* where the install put the data folder */
        free(file);
        file = snx_setup_file(a.data);
        rc = !file || snx_setup_done(file, &setup, why, sizeof why);
        if (rc) fprintf(out, "%s\n", file ? why : "No config folder: no home folder names one");
    }
    if (!rc) {
        shown(file, shown_path, sizeof shown_path);
        fprintf(out, "Done: %s\n", shown_path);
    }
    free(file);
    free(a.data);
    return rc;
}
