/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/clean.h"
#include "common/utilities/common.h"
#include "common/platform/file.h"
#include "gui/i18n.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* a line from at up to its \n (or end): its length without a CR before that */
static size_t line_of(const char* at, const char* end, const char** next) {
    const char* eol = memchr(at, '\n', (size_t)(end - at));
    size_t n = (size_t)((eol ? eol : end) - at);
    *next = eol ? eol + 1 : end;
    return n && at[n - 1] == '\r' ? n - 1 : n;
}
static int blank(const char* at, size_t n) {
    while (n && (*at == ' ' || *at == '\t')) ++at, --n;
    return !n;
}
static int header(const char* at, size_t n) {
    while (n && (*at == ' ' || *at == '\t')) ++at, --n;
    return n && *at == '[';
}
size_t gui_clean_deprecated(char* text, size_t size) {
    const char *at = text, *end = text + size, *next;
    char *out = text, *blanks = NULL; /* where the blank lines before this one start, in out */
    int found = 0;
    while (at < end) {
        size_t n = line_of(at, end, &next);
        if (n == 12 && !memcmp(at, "[Deprecated]", 12)) { /* it, the lines up to the next section and the blanks */
            found = 1;
            if (blanks) out = blanks;
            blanks = NULL;
            for (at = next; at < end && !header(at, line_of(at, end, &next)); at = next) {}
            continue;
        }
        if (!blank(at, n))
            blanks = NULL;
        else if (!blanks)
            blanks = out;
        memmove(out, at, (size_t)(next - at));
        out += next - at;
        at = next;
    }
    if (!found) return size;
    if (out < end) *out = 0;
    return (size_t)(out - text);
}

void gui_cleaned_add(GuiCleaned* done, bool ok, const char* file, const char* what) {
    size_t n = done->text ? strlen(done->text) : 0, add = strlen(file) + strlen(what) + 4, i;
    char* grown = (char*)realloc(done->text, n + add + 1);
    if (!grown) return;
    done->text = grown;
    snprintf(grown + n, add + 1, "%c\t%s\t%s\n", ok ? '+' : '!', file, what);
    for (i = n + 2; i < n + add - 1; ++i) /* a tab or a line's end in a name would split the entry */
        if (grown[i] == '\n' || (grown[i] == '\t' && i != n + 2 + strlen(file))) grown[i] = ' ';
    done->count++;
    done->done += ok;
}
const char* gui_cleaned_entry(const char* at, bool* ok, char* file, char* what, size_t size) {
    const char *tab, *eol;
    if (!at || !*at) return NULL;
    *ok = at[0] == '+';
    tab = strchr(at + 2, '\t');
    eol = strchr(at, '\n');
    snprintf(file, size, "%.*s", (int)(tab - at - 2), at + 2);
    snprintf(what, size, "%.*s", (int)(eol - tab - 1), tab + 1);
    return eol[1] ? eol + 1 : NULL;
}
void gui_cleaned_free(GuiCleaned* done) {
    free(done->text);
    done->text = NULL;
    done->count = done->done = 0;
}

static int ends_ini(const char* name) {
    size_t n = strlen(name);
    return n > 4 && name[n - 4] == '.' && tolower((unsigned char)name[n - 3]) == 'i' &&
           tolower((unsigned char)name[n - 2]) == 'n' && tolower((unsigned char)name[n - 1]) == 'i';
}
/* 1 when the .ini at path has a [Deprecated] section; with done, written again without it */
static int clean_ini(GuiCleaned* done, const char* path) {
    size_t size = 0, kept;
    char *text, *staged, what[300];
    int found = 0;
    text = snx_is_file(path) ? (char*)snx_read_file(path, &size) : NULL;
    if (!text && snx_is_file(path) && done) {
        snprintf(what, sizeof what, snx_tr("Not read: %s"), strerror(errno));
        gui_cleaned_add(done, false, path, what);
    }
    kept = text ? gui_clean_deprecated(text, size) : size;
    if (text && kept != size) {
        ++found;
        if (done) { /* staged beside it, then put in place: a crash mid-write leaves it as it was */
            int failed;
            staged = snx_format("%s.new", path);
            failed = !staged || snx_write_file(staged, text, kept) || plat_replace_file(staged, path);
            if (failed) {
                snprintf(what, sizeof what, snx_tr("Not written without [Deprecated]: %s"), strerror(errno));
                if (staged) remove(staged);
            }
            gui_cleaned_add(done, !failed, path, failed ? what : snx_tr("[Deprecated] removed"));
            free(staged);
        }
    }
    free(text);
    return found;
}
int gui_clean_inis(GuiCleaned* done, const char* folder) {
    char **names = NULL, *path, *inside, *own;
    size_t count = 0, i;
    int found = 0;
    if (!folder || snx_list_dir(folder, &names, &count)) return 0;
    for (i = 0; i < count; ++i) {
        path = snx_path_join(folder, names[i]);
        if (ends_ini(names[i]))
            found += clean_ini(done, path);
        else if (path && snx_is_dir(path) && !snx_is_link(path)) { /* a game's folder: its own <name>.ini */
            own = snx_format("%s.ini", names[i]);
            inside = own ? snx_path_join(path, own) : NULL;
            if (inside && snx_is_file(inside)) found += clean_ini(done, inside);
            free(inside);
            free(own);
        }
        free(path);
    }
    snx_free_strings(names, count);
    return found;
}
/* whether name is one of only's (NULL: every name is) */
static bool among(const char* const* only, const char* name) {
    if (!only) return true;
    for (; *only; ++only)
        if (!strcmp(*only, name)) return true;
    return false;
}
/* from into to, in the folder now: moved, would be, or why not; 1 when it moves */
static int move_entry(GuiCleaned* done, const char* from, const char* to, const char* now, bool apply) {
    char what[4400], why[256];
    int ok = 0;
    if (snx_path_exists(to))
        snprintf(what, sizeof what, snx_tr("Stays: %s is there"), to);
    else if (!snx_same_disk(from, now))
        snprintf(what, sizeof what, snx_tr("Not moved: %s is on another disk"), now);
    else if (apply && plat_replace_file(from, to))
        snprintf(what, sizeof what, snx_tr("Not moved to %s: %s"), to, snx_os_error(why, sizeof why));
    else {
        snprintf(what, sizeof what, apply ? snx_tr("Moved to %s") : snx_tr("Moves to %s"), to);
        ok = 1;
    }
    gui_cleaned_add(done, ok, from, what);
    return ok;
}
int gui_move_save_note(GuiCleaned* done, const char* old, const char* now, bool apply) {
    char *from = snx_format("%s" SNX_SAVE_NOTE, old), *to = snx_format("%s" SNX_SAVE_NOTE, now);
    int moves = from && to && snx_is_file(from) ? move_entry(done, from, to, now, apply) : 0;
    free(from);
    free(to);
    return moves;
}
/* the folder a save's files are moved into before it takes the save's place: <save> and GUI_SAVE_MOVING */
static char* save_staged(const char* save) {
    size_t n = strlen(save);
    while (n > 1 && (save[n - 1] == '/' || save[n - 1] == '\\')) --n;
    return snx_format("%.*s" GUI_SAVE_MOVING, (int)n, save);
}
bool gui_save_old(const char* save) {
    char *mark = snx_path_join(save, SNX_SAVE_JOURNALED), *staged = save_staged(save), **names = NULL;
    size_t count = 0;
    bool old = mark && staged && snx_is_dir(save) && !snx_is_link(save) && !snx_path_exists(mark) &&
               !snx_list_dir(save, &names, &count) && (count || snx_is_dir(staged));
    snx_free_strings(names, count);
    free(staged);
    free(mark);
    return old;
}
int gui_move_save_committed(GuiCleaned* done, const char* save, bool apply) {
    char *staged = save_staged(save), *into = staged ? snx_path_join(staged, SNX_SAVE_COMMITTED) : NULL;
    char* mark = staged ? snx_path_join(staged, SNX_SAVE_JOURNALED) : NULL;
    char *last = snx_path_join(save, SNX_SAVE_COMMITTED), **names = NULL, *from, *to, what[4400], why[256];
    size_t count = 0, i, moved = 0;
    int moves = 0;
    bool ok = staged && into && mark && last && gui_save_old(save) && !snx_list_dir(save, &names, &count);
    if (!ok) goto out;
    if (apply && (snx_mkdirs(into) || (!snx_path_exists(mark) && snx_write_file(mark, "", 0)))) {
        snprintf(what, sizeof what, snx_tr("Not moved: %s could not be made: %s"), staged,
                 snx_os_error(why, sizeof why));
        gui_cleaned_add(done, false, save, what);
        goto out;
    }
    for (i = 0; ok && i < count; ++i) {
        from = snx_path_join(save, names[i]);
        to = snx_path_join(into, names[i]);
        if (!from || !to)
            ok = false;
        else if (snx_path_exists(to)) {
            snprintf(what, sizeof what, snx_tr("Stays: %s is there"), to);
            gui_cleaned_add(done, ok = false, from, what);
        } else if (apply && plat_replace_file(from, to)) {
            snprintf(what, sizeof what, snx_tr("Not moved to %s: %s"), to, snx_os_error(why, sizeof why));
            gui_cleaned_add(done, ok = false, from, what);
        } else
            moved = i + 1;
        free(from);
        free(to);
    }
    if (!ok) { /* the ones that moved go back: a save with part of its files in its folder is no save */
        for (i = 0; apply && i < moved; ++i) {
            from = snx_path_join(into, names[i]);
            to = snx_path_join(save, names[i]);
            if (from && to) plat_replace_file(from, to);
            free(from);
            free(to);
        }
        if (apply && !snx_remove_empty_dir(into) && !remove(mark)) snx_remove_empty_dir(staged);
        goto out;
    }
    for (i = 0; i < count; ++i) {
        from = snx_path_join(save, names[i]);
        to = snx_path_join(last, names[i]);
        if (from && to) {
            snprintf(what, sizeof what, apply ? snx_tr("Moved to %s") : snx_tr("Moves to %s"), to);
            gui_cleaned_add(done, true, from, what);
            ++moves;
        }
        free(from);
        free(to);
    }
    if (apply && (snx_remove_empty_dir(save) || plat_replace_file(staged, save))) {
        snprintf(what, sizeof what, snx_tr("Its files are in %s, which could not take its place: %s"), staged,
                 snx_os_error(why, sizeof why));
        gui_cleaned_add(done, false, save, what);
    } else if (!count) { /* a move cut short after its last file: only the folder was left to put in place */
        snprintf(what, sizeof what, apply ? snx_tr("Moved to %s") : snx_tr("Moves to %s"), save);
        gui_cleaned_add(done, true, staged, what);
        ++moves;
    }
out:
    snx_free_strings(names, count);
    free(last);
    free(mark);
    free(into);
    free(staged);
    return moves;
}
int gui_move_entries(GuiCleaned* done, const char* old, const char* now, const char* const* only, bool apply) {
    char **names = NULL, *from, *to, what[4400], why[256];
    const char* slash;
    size_t count = 0, i;
    int moves = 0;
    if (!snx_path_exists(old)) return 0;
    if (apply && snx_mkdirs(now)) {
        snprintf(what, sizeof what, snx_tr("Not moved: %s could not be made: %s"), now, snx_os_error(why, sizeof why));
        gui_cleaned_add(done, false, old, what);
        return 0;
    }
    if (!snx_is_dir(old) || snx_is_link(old)) { /* a file, or a link: by its own name in now */
        for (slash = old + strlen(old); slash > old && slash[-1] != '/' && slash[-1] != '\\';) --slash;
        to = snx_path_join(now, slash);
        if (to) moves = move_entry(done, old, to, now, apply);
        free(to);
        return moves;
    }
    if (snx_list_dir(old, &names, &count)) {
        snprintf(what, sizeof what, snx_tr("Not moved: it cannot be read: %s"), snx_os_error(why, sizeof why));
        gui_cleaned_add(done, false, old, what);
        return 0;
    }
    for (i = 0; i < count; ++i) {
        if (!among(only, names[i])) continue;
        from = snx_path_join(old, names[i]);
        to = snx_path_join(now, names[i]);
        if (from && to) moves += move_entry(done, from, to, now, apply);
        free(from);
        free(to);
    }
    snx_free_strings(names, count);
    if (apply && !snx_remove_empty_dir(old)) gui_cleaned_add(done, true, old, snx_tr("Removed: it was empty"));
    return moves;
}

static bool ends_with(const char* name, const char* end) {
    size_t n = strlen(name), e = strlen(end);
    return n >= e && !strcmp(name + n - e, end);
}
static bool is_keys(const char* name) { return !strcmp(name, "prod.keys") || !strcmp(name, "title.keys"); }
/* a <name>_data/ with saves in it: its save/ or its SD card's sdmc/ */
static bool data_holds_saves(const char* data) {
    char *save = snx_path_join(data, "save"), *sdmc = snx_path_join(data, "sdmc");
    bool holds = (save && snx_is_dir(save)) || (sdmc && snx_is_dir(sdmc));
    free(save);
    free(sdmc);
    return holds;
}
/* how much a reason to keep weighs: a save over what cannot be read whole, over keys */
static int keep_weight(GuiKeep keep) {
    return keep == GUI_KEEP_SAVE || keep == GUI_KEEP_SAVES ? 4
           : keep == GUI_KEEP_UNREAD                       ? 3
           : keep == GUI_KEEP_LINK                         ? 2
           : keep == GUI_KEEP_KEYS                         ? 1
                                                           : 0;
}
/* keep, where and why (the system's, taken where it failed, for one not read) when they weigh more than kept's */
static void keep_note(GuiKept* kept, GuiKeep keep, const char* where, const char* why) {
    if (keep_weight(keep) <= keep_weight(kept->keep)) return;
    kept->keep = keep;
    snprintf(kept->where, sizeof kept->where, "%s", where);
    snprintf(kept->why, sizeof kept->why, "%s", why ? why : "");
}
/* folder and every folder in it, however deep: a <name>_data/ with saves ends the look; else the first folder it
   cannot list, or entry it cannot look at, or link to a folder, which it does not follow; else keys */
static void folder_keeps(const char* folder, GuiKept* kept) {
    char** names = NULL;
    char why[256];
    size_t count = 0, i;
    if (snx_list_dir(folder, &names, &count)) {
        keep_note(kept, GUI_KEEP_UNREAD, folder, snx_os_error(why, sizeof why));
        return;
    }
    for (i = 0; i < count && keep_weight(kept->keep) < 4; ++i) {
        char* path = snx_path_join(folder, names[i]);
        if (!path)
            keep_note(kept, GUI_KEEP_UNREAD, folder, "out of memory");
        else if (!snx_path_exists(path))
            keep_note(kept, GUI_KEEP_UNREAD, path, snx_os_error(why, sizeof why));
        else if (snx_is_link(path)) {
            if (snx_is_dir(path)) keep_note(kept, GUI_KEEP_LINK, path, NULL);
        } else if (snx_is_dir(path)) {
            if (ends_with(names[i], "_data") && data_holds_saves(path))
                keep_note(kept, GUI_KEEP_SAVES, path, NULL);
            else
                folder_keeps(path, kept);
        } else if (is_keys(names[i]))
            keep_note(kept, GUI_KEEP_KEYS, path, NULL);
        free(path);
    }
    snx_free_strings(names, count);
}
GuiKeep gui_old_keep(const char* path, GuiKept* out) {
    char *parent = snx_dirname(path), *grand = parent ? snx_dirname(parent) : NULL, *data = snx_format("%s_data", path);
    const char *name = path + strlen(path), *up;
    GuiKept kept = {GUI_KEEP_NONE, "", ""};
    while (name > path && name[-1] != '/' && name[-1] != '\\') --name;
    up = parent ? parent + strlen(parent) : NULL;
    while (up && up > parent && up[-1] != '/' && up[-1] != '\\') --up;
    if (up && !strcmp(up, "save") && grand && ends_with(grand, "_data"))
        keep_note(&kept, GUI_KEEP_SAVE, path, NULL); /* <name>_data/save/<kind> */
    else if (data && snx_is_dir(data) && data_holds_saves(data))
        keep_note(&kept, GUI_KEEP_SAVE, data, NULL); /* a game laid out beside others: its <name>_data/ */
    else if (snx_is_link(path) && snx_is_dir(path))
        keep_note(&kept, GUI_KEEP_LINK, path, NULL);
    else if (is_keys(name) && !snx_is_dir(path))
        keep_note(&kept, GUI_KEEP_KEYS, path, NULL);
    else {
        if (snx_is_dir(path)) folder_keeps(path, &kept);
        if (data && snx_is_link(data) && snx_is_dir(data)) /* a game's data, deleted with it */
            keep_note(&kept, GUI_KEEP_LINK, data, NULL);
        else if (data && snx_is_dir(data))
            folder_keeps(data, &kept);
    }
    free(data);
    free(grand);
    free(parent);
    if (out) *out = kept;
    return kept.keep;
}
void gui_delete_keep_save(GuiCleaned* done, const char* old) {
    gui_cleaned_add(done, false, old, snx_tr("Stays: a save, which Delete All never deletes: Move All moves it"));
}
int gui_delete_old(GuiCleaned* done, const char* old, bool apply) {
    char what[4800], why[256];
    GuiKept kept;
    char *mount, *data;
    bool beside;
    switch (gui_old_keep(old, &kept)) { /* a game laid out beside others: its program may be gone, its saves not */
        case GUI_KEEP_NONE: break;
        case GUI_KEEP_SAVE: gui_delete_keep_save(done, old); return 0;
        case GUI_KEEP_SAVES:
            snprintf(what, sizeof what,
                     snx_tr("Stays: it holds saves (%s), which Delete All never deletes: Move All moves it"),
                     kept.where);
            gui_cleaned_add(done, false, old, what);
            return 0;
        case GUI_KEEP_UNREAD:
            snprintf(what, sizeof what,
                     snx_tr("Stays: %s cannot be read (%s), so Delete All cannot tell it holds no save"), kept.where,
                     kept.why);
            gui_cleaned_add(done, false, old, what);
            return 0;
        case GUI_KEEP_LINK:
            snprintf(what, sizeof what,
                     snx_tr("Stays: %s is a link to a folder, which Delete All does not follow, so it cannot tell it "
                            "holds no save"),
                     kept.where);
            gui_cleaned_add(done, false, old, what);
            return 0;
        case GUI_KEEP_KEYS:
            if (!strcmp(kept.where, old))
                snprintf(what, sizeof what, "%s",
                         snx_tr("Stays: the console's keys, which Delete All never deletes: Move All moves them"));
            else
                snprintf(what, sizeof what,
                         snx_tr("Stays: it holds the console's keys (%s), which Delete All never deletes: Move All "
                                "moves them"),
                         kept.where);
            gui_cleaned_add(done, false, old, what);
            return 0;
    }
    if (!snx_path_exists(old)) return 0;
    mount = snx_is_dir(old) && !snx_is_link(old) ? snx_mount_within(old) : NULL;
    if (mount) {
        snprintf(what, sizeof what, snx_tr("Not deleted: %s is mounted inside it, whose files would go with it"),
                 mount);
        free(mount);
        gui_cleaned_add(done, false, old, what);
        return 0;
    }
    data = snx_format("%s_data", old); /* a game laid out beside others, with no save: its data too */
    beside = data && snx_is_dir(data) && !snx_is_link(data);
    if (apply && (snx_remove_tree(old) || (beside && snx_remove_tree(data)))) {
        snprintf(what, sizeof what, snx_tr("Not deleted: %s"), snx_os_error(why, sizeof why));
        gui_cleaned_add(done, false, old, what);
        free(data);
        return 0;
    }
    if (beside) { /* by its name: the page shows where */
        const char* name = data + strlen(data);
        while (name > data && name[-1] != '/' && name[-1] != '\\') --name;
        snprintf(what, sizeof what, apply ? snx_tr("Deleted, with %s") : snx_tr("Deletes it, with %s"), name);
    } else
        snprintf(what, sizeof what, "%s", apply ? snx_tr("Deleted") : snx_tr("Deletes it"));
    gui_cleaned_add(done, true, old, what);
    free(data);
    return 1;
}
