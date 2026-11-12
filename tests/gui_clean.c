/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Clean Old Settings (src/gui/clean.c): a game's .ini without its [Deprecated] section and nothing else changed, the
   .ini files of a folder, and an entry for each file, done or not; Move Old Files' moves, and Delete All, which never
   deletes a save or the console's keys */
#include "gui/clean.h"
#include "common/utilities/common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32 /* chmod and symlink */
#include <sys/stat.h>
#include <unistd.h>
#endif

/* text cleaned: what is left as a string */
static const char* cleaned(const char* text) {
    static char buffer[1024];
    size_t n = strlen(text), kept;
    assert(n < sizeof buffer);
    memcpy(buffer, text, n + 1);
    kept = gui_clean_deprecated(buffer, n);
    buffer[kept] = 0;
    return buffer;
}
static char* read_text(const char* path) {
    size_t size;
    return (char*)snx_read_file(path, &size);
}
static void write_text(const char* path, const char* text) { assert(!snx_write_file(path, text, strlen(text))); }
/* the entries as "+file|what" lines */
/* text, made with /, in the system's separators as the paths it names are */
static char* native(char* text) {
    snx_path_native(text);
    return text;
}
static const char* entries(const GuiCleaned* done, const char* base) {
    static char out[8192];
    char file[4400], what[4400];
    const char* at = done->text;
    bool ok;
    size_t n = 0, skip = strlen(base);
    out[0] = 0;
    while (at) {
        at = gui_cleaned_entry(at, &ok, file, what, sizeof file);
        n += (size_t)snprintf(out + n, sizeof out - n, "%c%s|%s\n", ok ? '+' : '!',
                              strncmp(file, base, skip) ? file : file + skip, what);
    }
    return out;
}

int main(void) {
    /* the runtime's section, last after [Other]: it and the blank line before it go, the rest as it was */
    const char* game =
        "[Display]\nfullscreen=1\n\n[Other]\nlanguage=en\n\n"
        "[Deprecated]\n; keys an older version used, which do nothing now (docs/user/deprecations.md): delete "
        "them, or use Settings > App >\n;   Clean Old Settings in the launcher\n"
        "; renderer: Vulkan is the only renderer\nrenderer=opengl\n; touch_controls: now controls\n"
        "touch_controls=1\n";
    assert(!strcmp(cleaned(game), "[Display]\nfullscreen=1\n\n[Other]\nlanguage=en\n"));
    /* none: the size as it was, the text untouched */
    {
        char text[] = "[Other]\nx=1\n\n";
        assert(gui_clean_deprecated(text, strlen(text)) == strlen(text) && !strcmp(text, "[Other]\nx=1\n\n"));
    }
    /* Windows line ends; blanks of spaces and tabs; a section after it stays, as do the blank lines inside the rest */
    assert(!strcmp(cleaned("[A]\r\na=1\r\n \t\r\n\r\n[Deprecated]\r\nold=1\r\n[B]\r\n\r\nb=2\r\n"),
                   "[A]\r\na=1\r\n[B]\r\n\r\nb=2\r\n"));
    /* only that line exactly: other names, a space around it, are other sections */
    assert(!strcmp(cleaned("[deprecated]\nx=1\n[Deprecated] \ny=1\n [Deprecated]\nz=1\n"),
                   "[deprecated]\nx=1\n[Deprecated] \ny=1\n [Deprecated]\nz=1\n"));
    /* two of them, one first in the file, the last without a line end; an indented header ends one */
    assert(!strcmp(cleaned("[Deprecated]\na=1\n  [A]\nb=2\n\n[Deprecated]\nc=3"), "  [A]\nb=2\n"));
    assert(!strcmp(cleaned("[Deprecated]"), ""));

    char* root = snx_temp_dir(NULL);
    char *games = snx_path_join(root, "games"), *path, *text;
    GuiCleaned done = {0};
    assert(root && !snx_mkdirs(games));

    /* a folder's .ini files: the ones with the section counted, then cleaned, an entry each; the others untouched */
    path = snx_path_join(games, "First.ini");
    write_text(path, game);
    free(path);
    path = snx_path_join(games, "Second.INI");
    write_text(path, "[Deprecated]\nx=1\n");
    free(path);
    path = snx_path_join(games, "Clean.ini");
    write_text(path, "[Other]\nx=1\n");
    free(path);
    path = snx_path_join(games, "notes.txt");
    write_text(path, "[Deprecated]\nx=1\n");
    free(path);
    /* a game's folder: its own .ini, not another file of it, nor a folder that is not a game's */
    {
        char* folder = snx_path_join(games, "0100ABCD12345000");
        char* data = snx_path_join(games, "Second_data");
        assert(!snx_mkdirs(folder) && !snx_mkdirs(data));
        path = snx_path_join(folder, "0100ABCD12345000.ini");
        write_text(path, "[Deprecated]\nx=1\n");
        free(path);
        path = snx_path_join(folder, "other.ini");
        write_text(path, "[Deprecated]\nx=1\n");
        free(path);
        path = snx_path_join(data, "Second_data.ini");
        write_text(path, "[Display]\n");
        free(path);
        path = snx_path_join(data, "save.ini");
        write_text(path, "[Deprecated]\nx=1\n");
        free(path);
        free(folder);
        free(data);
    }
    assert(gui_clean_inis(NULL, games) == 3 && !done.count);
    assert(gui_clean_inis(NULL, NULL) == 0 && gui_clean_inis(NULL, root) == 0);
    {
        char* other = snx_path_join(root, "absent");
        assert(gui_clean_inis(&done, other) == 0 && !done.count);
        free(other);
    }
    assert(gui_clean_inis(&done, games) == 3 && done.count == 3 && done.done == 3);
    {
        const char* list = entries(&done, games);
        char *first = native(snx_strdup("+/First.ini|[Deprecated] removed\n")),
             *second = native(snx_strdup("+/Second.INI|[Deprecated] removed\n")),
             *own = native(snx_strdup("+/0100ABCD12345000/0100ABCD12345000.ini|[Deprecated] removed\n"));
        assert(strstr(list, first) && strstr(list, second) && strstr(list, own));
        free(first);
        free(second);
        free(own);
    }
    path = snx_path_join(games, "0100ABCD12345000/other.ini");
    text = read_text(path);
    assert(!strcmp(text, "[Deprecated]\nx=1\n"));
    free(text);
    free(path);
    path = snx_path_join(games, "First.ini");
    text = read_text(path);
    assert(!strcmp(text, "[Display]\nfullscreen=1\n\n[Other]\nlanguage=en\n"));
    free(text);
    free(path);
    path = snx_path_join(games, "Clean.ini");
    text = read_text(path);
    assert(!strcmp(text, "[Other]\nx=1\n"));
    free(text);
    free(path);
    path = snx_path_join(games, "notes.txt");
    text = read_text(path);
    assert(!strcmp(text, "[Deprecated]\nx=1\n"));
    free(text);
    free(path);
    path = snx_path_join(games, "First.ini.new");
    assert(!snx_path_exists(path)); /* the staged copy went in place */
    free(path);
    assert(gui_clean_inis(NULL, games) == 0); /* nothing left to take out */
    gui_cleaned_free(&done);

#ifdef SNX_XDG
    /* what cannot be done is an entry of its own, why in it, and the file stays */
    if (geteuid()) {
        char *locked = snx_path_join(root, "locked"), *ini = snx_path_join(locked, "Game.ini"), expected[512];
        assert(!snx_mkdirs(locked));
        write_text(ini, "[A]\n[Deprecated]\nx=1\n");
        assert(!chmod(locked, 0555));
        assert(gui_clean_inis(&done, locked) == 1);
        assert(done.count == 1 && !done.done);
        snprintf(expected, sizeof expected, "!/locked/Game.ini|Not written without [Deprecated]: Permission denied\n");
        assert(!strcmp(entries(&done, root), expected));
        text = read_text(ini);
        assert(text && !strcmp(text, "[A]\n[Deprecated]\nx=1\n"));
        free(text);
        assert(!chmod(locked, 0755));
        gui_cleaned_free(&done);
        free(locked);
        free(ini);
    }
#endif
    /* a tab or a line's end in a name keeps the entry whole */
    {
        char file[64], what[64];
        bool ok;
        const char* next;
        gui_cleaned_add(&done, true, "a\tb\nc", "d\te");
        gui_cleaned_add(&done, false, "f", "g");
        next = gui_cleaned_entry(done.text, &ok, file, what, sizeof file);
        assert(ok && !strcmp(file, "a b c") && !strcmp(what, "d e") && next);
        next = gui_cleaned_entry(next, &ok, file, what, sizeof file);
        assert(!ok && !strcmp(file, "f") && !strcmp(what, "g") && !next);
        assert(done.count == 2 && done.done == 1);
        gui_cleaned_free(&done);
        assert(!gui_cleaned_entry(NULL, &ok, file, what, sizeof file));
    }
    /* Move Old Files: each entry into the new folder, one already there left and said; nothing touched without
       apply, the new folder made with it, the old one gone once empty; only the entries named; a file by its name */
    {
        char *old = snx_path_join(root, "old"), *now = snx_path_join(root, "now"), *base = snx_path_join(root, "");
        char *a = snx_path_join(old, "a"), *b = snx_path_join(old, "b"), *kept = snx_path_join(now, "b");
        char *c = snx_format("%s/c", b), *moved = snx_path_join(now, "a");
        const char* only[] = {"x", NULL};
        assert(!snx_mkdirs(b) && !snx_mkdirs(kept));
        write_text(a, "a");
        write_text(c, "c");
        assert(gui_move_entries(&done, old, now, NULL, false) == 1);
        {
            char* said = native(snx_format("+old/a|Moves to %s/now/a\n!old/b|Stays: %s/now/b is there\n", root, root));
            assert(!strcmp(entries(&done, base), said) && done.count == 2 && done.done == 1);
            free(said);
        }
        assert(snx_is_file(a) && !snx_path_exists(moved)); /* nothing moved */
        gui_cleaned_free(&done);
        assert(gui_move_entries(&done, old, now, NULL, true) == 1);
        {
            char* said = native(snx_format("+old/a|Moved to %s/now/a\n!old/b|Stays: %s/now/b is there\n", root, root));
            assert(!strcmp(entries(&done, base), said));
            free(said);
        }
        assert(!snx_path_exists(a) && snx_is_file(moved) && snx_is_file(c) && snx_is_dir(old)); /* b keeps old */
        gui_cleaned_free(&done);
        assert(!snx_remove_tree(b)); /* empty now: it goes after its last entry */
        write_text(a, "again");
        assert(!snx_remove_tree(moved));
        assert(gui_move_entries(&done, old, now, NULL, true) == 1 && !snx_path_exists(old) && snx_is_file(moved));
        {
            char* said = native(snx_format("+old/a|Moved to %s/now/a\n+old|Removed: it was empty\n", root));
            assert(!strcmp(entries(&done, base), said));
            free(said);
        }
        gui_cleaned_free(&done);
        /* only x of x and y, into a folder not there yet; the old folder stays with y */
        assert(!snx_mkdirs(old));
        write_text(a, "");
        free(a);
        a = snx_path_join(old, "x");
        write_text(a, "x");
        free(c);
        c = snx_path_join(old, "y");
        write_text(c, "y");
        free(kept);
        kept = snx_path_join(root, "later");
        assert(gui_move_entries(&done, old, kept, only, false) == 1 && !snx_path_exists(kept));
        assert(gui_move_entries(&done, old, kept, only, true) == 1 && snx_is_dir(kept) && snx_is_file(c));
        assert(!snx_path_exists(a) && snx_is_dir(old) && done.count == 2 && done.done == 2);
        gui_cleaned_free(&done);
        /* a file, by its own name */
        assert(gui_move_entries(&done, c, kept, NULL, true) == 1 && !snx_path_exists(c));
        free(c);
        c = snx_path_join(kept, "y");
        assert(snx_is_file(c) && done.count == 1);
        gui_cleaned_free(&done);
        assert(!gui_move_entries(&done, "/nonexistent/snx-move", now, NULL, true) && !done.count); /* none there */
        /* a save folder's note beside it (SNX_SAVE_NOTE) to beside the folder it moves to: said and left without
           apply, moved with it; one there already stays, and so does the old; no note, nothing said */
        {
            char *note = snx_path_join(root, "save.version"), *saves = snx_path_join(root, "saves"),
                 *into = snx_path_join(saves, "title"), *noted = snx_format("%s.version", into);
            char* said;
            assert(!snx_mkdirs(saves));
            write_text(note, "program=0100000000ABC000\n");
            free(c);
            c = snx_path_join(root, "save");
            assert(gui_move_save_note(&done, c, into, false) == 1 && snx_is_file(note) && !snx_path_exists(noted));
            said = native(snx_format("+save.version|Moves to %s/saves/title.version\n", root));
            assert(!strcmp(entries(&done, base), said));
            free(said);
            gui_cleaned_free(&done);
            assert(gui_move_save_note(&done, c, into, true) == 1 && !snx_path_exists(note) && snx_is_file(noted));
            gui_cleaned_free(&done);
            write_text(note, "program=0100000000ABC800\n");
            assert(!gui_move_save_note(&done, c, into, true) && snx_is_file(note) && snx_is_file(noted));
            said = native(snx_format("!save.version|Stays: %s/saves/title.version is there\n", root));
            assert(!strcmp(entries(&done, base), said));
            free(said);
            gui_cleaned_free(&done);
            assert(!snx_remove_tree(note) && !gui_move_save_note(&done, c, into, true) && !done.count);
            assert(!snx_remove_tree(saves));
            free(noted);
            free(into);
            free(saves);
            free(note);
        }
        /* a save an older version kept, the game's own files in its folder, two of them folders named 0 and 1 (a game's
           slots): each into the save's 0/, its last commit as this version keeps a save, with the file that says so.
           Nothing touched without apply; then the save is not one of those any more, and nothing is left beside it */
        {
            char *save = snx_path_join(root, "title"), *zero = snx_path_join(save, "0/slot.bin"),
                 *one = snx_path_join(save, "1/slot.bin"), *top = snx_path_join(save, "system.bin"),
                 *staged = snx_path_join(root, "title.moving"), *mark = snx_path_join(save, SNX_SAVE_JOURNALED),
                 *moved = snx_path_join(save, SNX_SAVE_COMMITTED "/1/slot.bin"), *dir, *said;
            assert(!gui_save_old(save) && !snx_mkdirs(save)); /* not there */
            assert(!gui_save_old(save));                      /* a folder with nothing in it */
            dir = snx_dirname(zero);
            assert(!snx_mkdirs(dir));
            free(dir);
            dir = snx_dirname(one);
            assert(!snx_mkdirs(dir));
            free(dir);
            write_text(zero, "zero");
            write_text(one, "one");
            write_text(top, "top");
            assert(gui_save_old(save));
            assert(gui_move_save_committed(&done, save, false) == 3);
            said = native(snx_format("+title/0|Moves to %s/title/0/0\n+title/1|Moves to %s/title/0/1\n"
                                     "+title/system.bin|Moves to %s/title/0/system.bin\n",
                                     root, root, root));
            assert(!strcmp(entries(&done, base), said) && snx_is_file(top) && !snx_path_exists(staged));
            free(said);
            gui_cleaned_free(&done);
            assert(gui_move_save_committed(&done, save, true) == 3 && done.count == 3 && done.done == 3);
            gui_cleaned_free(&done);
            assert(snx_is_file(mark) && snx_is_file(moved) && !snx_path_exists(top) && !snx_path_exists(staged));
            {
                char* text = read_text(moved);
                assert(text && !strcmp(text, "one"));
                free(text);
            }
            free(moved);
            moved = snx_path_join(save, SNX_SAVE_COMMITTED "/0/slot.bin");
            assert(snx_is_file(moved) && !snx_path_exists(one));
            /* laid out now: not one of those, and a second move does nothing */
            assert(!gui_save_old(save) && !gui_move_save_committed(&done, save, true) && !done.count);
            /* a move cut short after its last file: the save's folder empty, its files beside it. The next one puts
               the folder in place */
            assert(!rename(save, staged) && !snx_mkdirs(save) && gui_save_old(save));
            assert(gui_move_save_committed(&done, save, true) == 1 && done.count == 1 && done.done == 1);
            gui_cleaned_free(&done);
            assert(snx_is_file(mark) && snx_is_file(moved) && !snx_path_exists(staged) && !gui_save_old(save));
            /* one of its files cannot move, its name there already from a move cut short: the save is as it was, the
               ones that moved back in it, and that file is the only entry */
            assert(!snx_remove_tree(save) && !snx_mkdirs(save));
            write_text(top, "top");
            free(moved);
            moved = snx_path_join(save, "a.bin");
            write_text(moved, "a");
            dir = snx_path_join(staged, SNX_SAVE_COMMITTED "/system.bin");
            free(zero);
            zero = snx_dirname(dir);
            assert(!snx_mkdirs(zero));
            write_text(dir, "an older move's");
            assert(!gui_move_save_committed(&done, save, true) && done.count == 1 && done.done == 0);
            said = native(snx_format("!title/system.bin|Stays: %s/title.moving/0/system.bin is there\n", root));
            assert(!strcmp(entries(&done, base), said));
            free(said);
            gui_cleaned_free(&done);
            assert(snx_is_file(moved) && snx_is_file(top) && snx_is_file(dir) && !snx_path_exists(mark));
            assert(!snx_remove_tree(save) && !snx_remove_tree(staged));
            free(dir);
            free(moved);
            free(mark);
            free(staged);
            free(top);
            free(one);
            free(zero);
            free(save);
        }
#ifdef SNX_XDG
        { /* another disk: the entry not moved, the folder on the other named */
            char* shm = snx_is_dir("/dev/shm") ? snx_temp_dir("/dev/shm") : NULL;
            if (shm && !snx_same_disk(shm, root)) {
                char* file = snx_path_join(shm, "far");
                char* said = snx_format("!%s|Not moved: %s/now is on another disk\n", file, root);
                write_text(file, "far");
                assert(!gui_move_entries(&done, shm, now, NULL, true) && !strcmp(entries(&done, base), said));
                assert(snx_is_file(file) && snx_is_dir(shm));
                gui_cleaned_free(&done);
                free(said);
                free(file);
            }
            if (shm) assert(!snx_remove_tree(shm));
            free(shm);
        }
#endif
        free(moved);
        free(c);
        free(kept);
        free(b);
        free(a);
        free(base);
        free(now);
        free(old);
    }
    /* Delete All: a save is never deleted, nor a game laid out beside others whose <name>_data/ holds its saves, a
       folder holding saves however deep, one it cannot read whole or the console's keys, each said with its path; the
       rest is, only with apply, said either way */
    {
        static const char* const files[] = {"gui/settings.ini",
                                            "work/tmp/a.o",
                                            "games/0100000000ABC000/0100000000ABC000_data/save/user.x/s",
                                            "games/0100000000ABC000/0100000000ABC000_data/exefs/main",
                                            "code/0100000000ABC000/0100000000ABC000_data/exefs/main",
                                            "beside/Game",
                                            "beside/Game_data/sdmc/notes.txt",
                                            "deep/a/b/c/d/e/x_data/save/s",
                                            "checkout/prod.keys",
                                            "keys/sub/title.keys",
                                            "laid/Old",
                                            "laid/Old_data/exefs/main"};
        static const char* const olds[] = {
            "gui", "work", "code", "laid/Old", "games", "beside/Game", "deep", "checkout/prod.keys", "keys"};
        char *base = snx_path_join(root, ""), *path, *said;
        size_t i;
        GuiKept kept;
        for (i = 0; i < sizeof files / sizeof files[0]; ++i) {
            char* dir;
            path = snx_path_join(root, files[i]);
            dir = snx_dirname(path);
            assert(dir && !snx_mkdirs(dir));
            write_text(path, "x");
            free(dir);
            free(path);
        }
#define KEEP(at) (path = snx_path_join(root, at), gui_old_keep(path, &kept), free(path), kept.keep)
#define WHERE(at)                                                                                                      \
    (path = native(snx_format("%s/%s", root, at)), strcmp(kept.where, path) ? (free(path), 0) : (free(path), 1))
        assert(KEEP("gui") == GUI_KEEP_NONE && KEEP("work") == GUI_KEEP_NONE && KEEP("code") == GUI_KEEP_NONE);
        assert(KEEP("games/0100000000ABC000/0100000000ABC000_data/save/user.x") == GUI_KEEP_SAVE);
        assert(KEEP("beside/Game") == GUI_KEEP_SAVE && WHERE("beside/Game_data"));
        assert(KEEP("games") == GUI_KEEP_SAVES && WHERE("games/0100000000ABC000/0100000000ABC000_data"));
        assert(KEEP("deep") == GUI_KEEP_SAVES && WHERE("deep/a/b/c/d/e/x_data")); /* six folders down */
        assert(KEEP("checkout/prod.keys") == GUI_KEEP_KEYS && KEEP("checkout") == GUI_KEEP_KEYS);
        assert(KEEP("keys") == GUI_KEEP_KEYS && WHERE("keys/sub/title.keys"));
        assert(gui_old_keep(base, NULL) == GUI_KEEP_SAVES); /* all of it: a save first */
        for (i = 0; i < sizeof olds / sizeof olds[0]; ++i) {
            path = snx_path_join(root, olds[i]);
            gui_delete_old(&done, path, false);
            free(path);
        }
        said = native(snx_format(
            "+gui|Deletes it\n+work|Deletes it\n+code|Deletes it\n+laid/Old|Deletes it, with Old_data\n"
            "!games|Stays: it holds saves (%s/games/0100000000ABC000/0100000000ABC000_data), which Delete All never "
            "deletes: Move All moves it\n"
            "!beside/Game|Stays: a save, which Delete All never deletes: Move All moves it\n"
            "!deep|Stays: it holds saves (%s/deep/a/b/c/d/e/x_data), which Delete All never deletes: Move All moves it\n"
            "!checkout/prod.keys|Stays: the console's keys, which Delete All never deletes: Move All moves them\n"
            "!keys|Stays: it holds the console's keys (%s/keys/sub/title.keys), which Delete All never deletes: Move "
            "All moves them\n",
            root, root, root));
        assert(!strcmp(entries(&done, base), said) && done.count == 9 && done.done == 4);
        free(said);
        path = snx_path_join(root, "gui/settings.ini");
        assert(snx_is_file(path)); /* nothing deleted without apply */
        free(path);
        gui_cleaned_free(&done);
        for (i = 0; i < sizeof olds / sizeof olds[0]; ++i) {
            path = snx_path_join(root, olds[i]);
            assert(gui_delete_old(&done, path, true) == (i < 4));
            assert(!snx_path_exists(path) == (i < 4));
            free(path);
        }
        path = snx_path_join(root, "laid/Old_data");
        assert(!snx_path_exists(path)); /* with its game */
        free(path);
        path = snx_path_join(root, "deep/a/b/c/d/e/x_data/save/s");
        assert(snx_is_file(path)); /* the deep save, there */
        free(path);
        said = native(snx_format("+gui|Deleted\n+work|Deleted\n+code|Deleted\n+laid/Old|Deleted, with Old_data\n"));
        assert(done.count == 9 && done.done == 4 && !strncmp(entries(&done, base), said, strlen(said)));
        free(said);
        gui_cleaned_free(&done);
        path = snx_path_join(root, "gone");
        assert(!gui_delete_old(&done, path, true) && !done.count); /* not there: nothing said */
        free(path);
#ifndef _WIN32
        /* a folder it cannot list and a link to a folder, which it does not follow: it cannot tell they hold no save,
           so they stay, the page naming them; root reads what its mode forbids, so that one is not tried as root */
        {
            char *locked = snx_path_join(root, "locked"), *sub = snx_path_join(root, "locked/sub"),
                 *linked = snx_path_join(root, "linked"), *out = snx_path_join(root, "linked/out"),
                 *target = snx_path_join(root, "target"), **names = NULL;
            size_t count = 0;
            assert(!snx_mkdirs(sub) && !snx_mkdirs(linked) && !snx_mkdirs(target) && !symlink(target, out));
            assert(KEEP("linked") == GUI_KEEP_LINK && !strcmp(kept.where, out));
            assert(!gui_delete_old(&done, linked, true) && snx_is_dir(linked) && snx_is_dir(target));
            said =
                snx_format("!%s|Stays: %s is a link to a folder, which Delete All does not follow, so it cannot tell "
                           "it holds no save\n",
                           linked, out);
            assert(!strcmp(entries(&done, ""), said));
            free(said);
            gui_cleaned_free(&done);
            assert(!chmod(sub, 0));
            if (snx_list_dir(sub, &names, &count)) {
                assert(KEEP("locked") == GUI_KEEP_UNREAD && !strcmp(kept.where, sub) && kept.why[0]);
                assert(!gui_delete_old(&done, locked, true) && snx_is_dir(sub));
                said = snx_format("!%s|Stays: %s cannot be read (%s), so Delete All cannot tell it holds no save\n",
                                  locked, sub, kept.why);
                assert(!strcmp(entries(&done, ""), said));
                free(said);
                gui_cleaned_free(&done);
            } else
                puts("gui_clean: the mode does not keep this user out (root): a folder not read is not tried");
            snx_free_strings(names, count);
            assert(!chmod(sub, 0755));
            free(target);
            free(out);
            free(linked);
            free(sub);
            free(locked);
        }
#endif
#undef WHERE
#undef KEEP
        free(base);
    }
    assert(!snx_remove_tree(root));
    free(games);
    free(root);
    puts("gui_clean passed");
    return 0;
}
