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
