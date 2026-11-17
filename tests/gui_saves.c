/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A game's saves out of ScarletNX and back in (src/gui/saves.c): Eden's zips as its Android app (the Java zip writer's
   data descriptors, deflate at level 0) and its Qt Data Manager (a member for each folder) write them, imported file
   by file into a save's last commit; an export imported into an empty place the same byte for byte; the save there
   before in the trash with its note; and each refusal naming what the zip holds and what was asked */
#include "gui/saves.h"
#include "common/utilities/common.h"
#include "common/utilities/zip.h"
#include <assert.h>
#include <libdeflate.h>
#include <miniz.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* a member of a zip made here, as the zip writers of Eden's two frontends lay one out: stored (method 0) or deflated
   at level (8), with a data descriptor after it and no sizes in its local header (Java's ZipOutputStream) or not */
typedef struct {
    const char* name;
    const void* data;
    size_t size;
    int method, level;
    bool descriptor;
} Member;
static void le16(unsigned char** p, unsigned v) {
    *(*p)++ = (unsigned char)v;
    *(*p)++ = (unsigned char)(v >> 8);
}
static void le32(unsigned char** p, uint32_t v) {
    le16(p, (unsigned)(v & 0xffff));
    le16(p, (unsigned)(v >> 16));
}
/* the count members into a zip at path */
static void zip_make(const char* path, const Member* m, size_t count) {
    size_t cap = 1 << 16, i, n;
    unsigned char *zip, *p, *central, *c;
    uint32_t* offsets = calloc(count ? count : 1, sizeof *offsets);
    uint32_t* packed = calloc(count ? count : 1, sizeof *packed);
    uint32_t* crcs = calloc(count ? count : 1, sizeof *crcs);
    for (i = 0; i < count; ++i) cap += m[i].size * 2 + 256;
    zip = p = malloc(cap);
    central = c = malloc(1 << 16);
    assert(offsets && packed && crcs && zip && central);
    for (i = 0; i < count; ++i) {
        size_t name = strlen(m[i].name);
        unsigned flags = 0x0800u | (m[i].descriptor ? 8u : 0u);
        unsigned char* data = malloc(m[i].size * 2 + 64);
        assert(data);
        crcs[i] = libdeflate_crc32(0, m[i].data, m[i].size);
        if (m[i].method == 8) {
            struct libdeflate_compressor* d = libdeflate_alloc_compressor(m[i].level);
            assert(d);
            n = libdeflate_deflate_compress(d, m[i].data, m[i].size, data, m[i].size * 2 + 64);
            assert(n);
            libdeflate_free_compressor(d);
        } else {
            if (m[i].size) memcpy(data, m[i].data, m[i].size);
            n = m[i].size;
        }
        packed[i] = (uint32_t)n;
        offsets[i] = (uint32_t)(p - zip);
        le32(&p, 0x04034b50u);
        le16(&p, 20);
        le16(&p, flags);
        le16(&p, (unsigned)m[i].method);
        le16(&p, 0);
        le16(&p, 0x21); /* 1980-01-01 */
        le32(&p, m[i].descriptor ? 0 : crcs[i]);
        le32(&p, m[i].descriptor ? 0 : packed[i]);
        le32(&p, m[i].descriptor ? 0 : (uint32_t)m[i].size);
        le16(&p, (unsigned)name);
        le16(&p, 0);
        memcpy(p, m[i].name, name);
        p += name;
        if (n) memcpy(p, data, n);
        p += n;
        if (m[i].descriptor) {
            le32(&p, 0x08074b50u);
            le32(&p, crcs[i]);
            le32(&p, packed[i]);
            le32(&p, (uint32_t)m[i].size);
        }
        free(data);
        le32(&c, 0x02014b50u);
        le16(&c, 20);
        le16(&c, 20);
        le16(&c, flags);
        le16(&c, (unsigned)m[i].method);
        le16(&c, 0);
        le16(&c, 0x21);
        le32(&c, crcs[i]);
        le32(&c, packed[i]);
        le32(&c, (uint32_t)m[i].size);
        le16(&c, (unsigned)name);
        le16(&c, 0);
        le16(&c, 0);
        le16(&c, 0);
        le16(&c, 0);
        le32(&c, m[i].name[name - 1] == '/' ? 0x10u : 0);
        le32(&c, offsets[i]);
        memcpy(c, m[i].name, name);
        c += name;
    }
    n = (size_t)(c - central);
    memcpy(p, central, n);
    {
        uint32_t at = (uint32_t)(p - zip);
        p += n;
        le32(&p, 0x06054b50u);
        le16(&p, 0);
        le16(&p, 0);
        le16(&p, (unsigned)count);
        le16(&p, (unsigned)count);
        le32(&p, (uint32_t)n);
        le32(&p, at);
        le16(&p, 0);
    }
    assert(!snx_write_file(path, zip, (size_t)(p - zip)));
    free(central);
    free(zip);
    free(crcs);
    free(packed);
    free(offsets);
}
/* a file of n bytes at path, its folders made */
static void put(const char* path, const void* data, size_t n) {
    char* folder = snx_dirname(path);
    assert(folder && !snx_mkdirs(folder));
    assert(!snx_write_file(path, data, n));
    free(folder);
}
static void put_text(const char* path, const char* text) { put(path, text, strlen(text)); }
/* the file at path holds the n bytes at data */
static bool holds(const char* path, const void* data, size_t n) {
    size_t size = 0;
    unsigned char* got = snx_read_file(path, &size);
    bool same = got && size == n && (!n || !memcmp(got, data, n));
    free(got);
    return same;
}
static bool holds_text(const char* path, const char* text) { return holds(path, text, strlen(text)); }
/* the folders a and b hold the same names, folders as folders, and files of the same bytes, all the way down */
static bool same_tree(const char* a, const char* b) {
    char **x = NULL, **y = NULL;
    size_t nx = 0, ny = 0, i;
    bool same = !snx_list_dir(a, &x, &nx) && !snx_list_dir(b, &y, &ny) && nx == ny;
    for (i = 0; same && i < nx; ++i) {
        char *p = snx_path_join(a, x[i]), *q = snx_path_join(b, y[i]);
        size_t n = 0;
        unsigned char* data;
        same = !strcmp(x[i], y[i]) && snx_is_dir(p) == snx_is_dir(q);
        if (same && snx_is_dir(p))
            same = same_tree(p, q);
        else if (same) {
            data = snx_read_file(p, &n);
            same = data && holds(q, data, n);
            free(data);
        }
        free(p);
        free(q);
    }
    snx_free_strings(x, nx);
    snx_free_strings(y, ny);
    return same;
}
/* path joined to the parts after it, as the code joins them */
static char* at(const char* path, const char* a, const char* b) {
    char *first = snx_path_join(path, a), *both = b && first ? snx_path_join(first, b) : first;
    if (both != first) free(first);
    assert(both);
    return both;
}
/* the one entry of folder, joined to it; NULL when it holds none or more than one */
static char* only_entry(const char* folder, const char* not_ending) {
    char **names = NULL, *found = NULL;
    size_t count = 0, i, kept = 0;
    if (snx_list_dir(folder, &names, &count)) return NULL;
    for (i = 0; i < count; ++i) {
        size_t n = strlen(names[i]), e = strlen(not_ending);
        if (n >= e && !strcmp(names[i] + n - e, not_ending)) continue;
        if (!kept++) found = snx_path_join(folder, names[i]);
    }
    snx_free_strings(names, count);
    if (kept == 1) return found;
    free(found);
    return NULL;
}
/* a place gui_saves_nx_place or gui_saves_nx_find gave: its strings */
static void place_free(GuiSaveNx* place) {
    free((void*)place->folder);
    free((void*)place->trash);
    free((void*)place->name);
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

#define UID  "0123456789abcdef0011223344556677"
#define GAME 0x0100ABCD12340000ull
static const unsigned char size_record[16] = {0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0};

int main(void) {
    char *root = snx_temp_dir(NULL), *data, *trash, *folder, *zip, *path, *path2, error[8192];
    unsigned char bytes[70000], pattern[1024];
    GuiCleaned done = {0};
    GuiSavesDone result = {0};
    SnxU64* titles = NULL;
    size_t count = 0, i;
    unsigned seed = 12345;
    assert(root);
    for (i = 0; i < sizeof bytes; ++i) {
        seed = seed * 1103515245u + 12345u;
        bytes[i] = (unsigned char)(seed >> 16);
    }
    for (i = 0; i < sizeof pattern; ++i) pattern[i] = (unsigned char)i;
    data = snx_path_join(root, "data");
    trash = snx_path_join(data, "trash");
    folder = at(data, "save/user." UID, "0100ABCD12340000");

    /* Eden's Android export of one game: <title ID>/ and its files only, deflated at level 0 with data descriptors;
       its .yuzu_save_size left out, the files in the save's last commit and nothing else beside it */
    {
        static const char text[] = "a game's own save file\n";
        Member m[] = {{"0100ABCD12340000/save.bin", bytes, 5000, 8, 0, true},
                      {"0100ABCD12340000/sub/deep.txt", text, sizeof text - 1, 8, 6, true},
                      {"0100ABCD12340000/.yuzu_save_size", size_record, sizeof size_record, 8, 0, true}};
        GuiSaveNx save = {GAME, folder, trash, "0100ABCD12340000.user." UID};
        zip = snx_path_join(root, "android.zip");
        zip_make(zip, m, 3);
        assert(!gui_saves_eden_titles(zip, zip, "", &titles, &count, error, sizeof error) && count == 1 &&
               titles[0] == GAME);
        free(titles);
        assert(!gui_saves_eden_import(&done, zip, zip, "", &save, 1, &result, error, sizeof error));
        assert(result.saves == 1 && result.files == 2 && result.bytes == 5023 && !result.backups && !result.backup[0]);
        path = at(folder, "snx-journaled", NULL);
        assert(snx_is_file(path) && snx_file_size(path) == 0);
        free(path);
        path = at(folder, "0", "save.bin");
        assert(holds(path, bytes, 5000));
        free(path);
        path = at(folder, "0", "sub");
        path2 = at(path, "deep.txt", NULL);
        assert(holds_text(path2, text));
        free(path2);
        free(path);
        path = at(folder, "0", ".yuzu_save_size");
        assert(!snx_path_exists(path));
        free(path);
        path = at(folder, "1", NULL);
        assert(!snx_path_exists(path)); /* the game's start makes its working copy */
        free(path);
        path = at(data, "save/user." UID, ".0100ABCD12340000.importing");
        assert(!snx_path_exists(path));
        free(path);
        assert(!snx_path_exists(trash)); /* there was no save before: nothing to keep */
        assert(done.done == done.count && strstr(done.text, "Left out: Eden's record of the save's size") &&
               strstr(done.text, "Imported 2 files (5023 bytes) of 0100ABCD12340000 from ") &&
               strstr(done.text, "there was no save before"));
        gui_cleaned_free(&done);
        free(zip);
    }

    /* Qt's export of every game's: a member for each folder, stored and deflated; one game taken, the save there
       before (save_layout=game's place) and its note to the trash, the other game said and left */
    {
        static const char a[] = "the other game's", b[] = "the game asked for";
        char* game = at(root, "games/g", "g_data");
        char* own = at(game, "save", "user." UID);
        char *note = snx_format("%s.version", own), *mark, *backup;
        Member m[] = {{"0100AAAA00000000/", "", 0, 0, 0, false},
                      {"0100AAAA00000000/a.bin", a, sizeof a - 1, 0, 0, false},
                      {"0100abcd12340000/", "", 0, 0, 0, false},
                      {"0100abcd12340000/empty/", "", 0, 0, 0, false},
                      {"0100abcd12340000/b.bin", b, sizeof b - 1, 8, 6, false}};
        char* game_trash = at(game, "trash", NULL);
        GuiSaveNx save = {GAME, own, game_trash, "game.user." UID};
        zip = snx_path_join(root, "qt.zip");
        zip_make(zip, m, 5);
        assert(!gui_saves_eden_titles(zip, zip, "", &titles, &count, error, sizeof error) && count == 2 &&
               titles[0] == 0x0100AAAA00000000ull && titles[1] == GAME);
        free(titles);
        path = at(own, "0", "old.bin");
        put_text(path, "the save before");
        free(path);
        path = at(own, "1", "old.bin");
        put_text(path, "the save before, worked on");
        free(path);
        mark = at(own, "snx-journaled", NULL);
        put_text(mark, "");
        put_text(note, "program=0100ABCD12340000\n");
        result = (GuiSavesDone){0};
        assert(!gui_saves_eden_import(&done, zip, zip, "", &save, 1, &result, error, sizeof error));
        path = at(own, "0", "b.bin");
        assert(holds_text(path, b));
        free(path);
        path = at(own, "0", "empty");
        assert(snx_is_dir(path));
        free(path);
        path = at(own, "0", "old.bin");
        assert(!snx_path_exists(path));
        free(path);
        assert(!snx_path_exists(note));
        backup = only_entry(game_trash, ".version");
        assert(result.saves == 1 && result.backups == 1 && backup && !strcmp(result.backup, backup));
        assert(backup && !strncmp(snx_path_leaf(backup), "game.user." UID ".", strlen("game.user." UID ".")));
        path = at(backup, "0", "old.bin");
        assert(holds_text(path, "the save before"));
        free(path);
        path = snx_format("%s.version", backup);
        assert(holds_text(path, "program=0100ABCD12340000\n"));
        free(path);
        assert(strstr(done.text, "Not imported: it holds the saves of 1 other games too, 0100AAAA00000000") &&
               strstr(done.text, "the save there before is in ") && strstr(done.text, snx_path_leaf(backup)));
        gui_cleaned_free(&done);
        free(backup);
        free(mark);
        free(note);
        free(game_trash);
        free(own);
        free(game);
        free(zip);
    }

    /* an export imported into an empty place: the same last commit byte for byte, its folders (one empty), a file of
       no bytes, one that does not deflate smaller and a name with a dot inside 0/ (the game's) included; the working
       copy not exported */
    {
        char *a = at(root, "rt/a", "0100ABCD12340000"), *b = at(root, "rt/b", "0100ABCD12340000");
        char* out = at(root, "rt", "out.zip");
        GuiSaveNx save = {GAME, a, trash, "rt"}, back = {GAME, b, trash, "rt"};
        path = at(a, "snx-journaled", NULL);
        put_text(path, "");
        free(path);
        path = at(a, "0", "random.dat");
        put(path, bytes, sizeof bytes);
        free(path);
        path = at(a, "0", "pattern.dat");
        put(path, pattern, sizeof pattern);
        free(path);
        path = at(a, "0", "empty.dat");
        put(path, "", 0);
        free(path);
        path = at(a, "0", ".game_hidden");
        put_text(path, "the game's own dot name");
        free(path);
        path = at(a, "0", "dir/nested/deep.txt");
        put_text(path, "deep");
        free(path);
        path = at(a, "0", "emptydir");
        assert(!snx_mkdirs(path));
        free(path);
        path = at(a, "1", "work.dat");
        put_text(path, "not committed");
        free(path);
        assert(!gui_saves_eden_export(&done, out, &save, 1, error, sizeof error));
        path = snx_format("%s.part", out);
        assert(snx_is_file(out) && !snx_path_exists(path));
        free(path);
        assert(has_member(out, "0100ABCD12340000/") && has_member(out, "0100ABCD12340000/emptydir/") &&
               has_member(out, "0100ABCD12340000/dir/nested/deep.txt") &&
               has_member(out, "0100ABCD12340000/.game_hidden") && !has_member(out, "0100ABCD12340000/work.dat"));
        assert(strstr(done.text, "Exported 5 files (71051 bytes) from 1 of 1 saves"));
        gui_cleaned_free(&done);
        assert(!gui_saves_eden_import(&done, out, out, "", &back, 1, &result, error, sizeof error));
        path = at(a, "0", NULL);
        path2 = at(b, "0", NULL);
        assert(same_tree(path, path2));
        free(path2);
        free(path);
        gui_cleaned_free(&done);

        /* as Eden reads it back: a miniz reader opens it and finds the bytes, the CRC checked */
        {
            mz_zip_archive reader;
            size_t n = 0;
            void* got;
            mz_zip_zero_struct(&reader);
            unsigned char* file = snx_read_file(out, &n);
            assert(file && mz_zip_reader_init_mem(&reader, file, n, 0));
            got = mz_zip_reader_extract_file_to_heap(&reader, "0100ABCD12340000/random.dat", &n, 0);
            assert(got && n == sizeof bytes && !memcmp(got, bytes, n));
            mz_free(got);
            mz_zip_reader_end(&reader);
            free(file);
        }

        /* a save as the versions before kept one: the folder's own files, not a sync tool's dot name at its top; a
           commit cut short: its working copy; two saves, one not there: the one there alone */
        {
            char *flat = at(root, "rt/flat", "0100ABCD12340000"), *cut = at(root, "rt/cut", "0100ABCD12340000");
            char* none = at(root, "rt/none", "0100AAAA00000000");
            GuiSaveNx two[2] = {{0x0100AAAA00000000ull, none, trash, "none"}, {GAME, flat, trash, "flat"}};
            GuiSaveNx cut_save = {GAME, cut, trash, "cut"};
            path = at(flat, "file.sav", NULL);
            put_text(path, "flat");
            free(path);
            path = at(flat, ".stfolder", NULL);
            assert(!snx_mkdirs(path));
            free(path);
            assert(!gui_saves_eden_export(&done, out, two, 2, error, sizeof error));
            assert(has_member(out, "0100ABCD12340000/file.sav") && !has_member(out, "0100ABCD12340000/.stfolder/") &&
                   !has_member(out, "0100AAAA00000000/"));
            assert(strstr(done.text, "Not exported: no save there") &&
                   strstr(done.text, "Left out: a name that starts with a dot") &&
                   strstr(done.text, "Exported 1 files (4 bytes) from 1 of 2 saves"));
            gui_cleaned_free(&done);
            path = at(cut, "snx-journaled", NULL);
            put_text(path, "");
            free(path);
            path = at(cut, "1", "work.sav");
            put_text(path, "cut short");
            free(path);
            assert(!gui_saves_eden_export(&done, out, &cut_save, 1, error, sizeof error));
            assert(has_member(out, "0100ABCD12340000/work.sav") && strstr(done.text, "Exported from its working copy"));
            gui_cleaned_free(&done);
            free(none);
            free(cut);
            free(flat);
        }

        /* nothing to export: said with the title ID and the folder, and the file there before as it was */
        {
            char* none = at(root, "rt/none", "0100AAAA00000000");
            GuiSaveNx empty = {0x0100AAAA00000000ull, none, trash, "none"};
            put_text(out, "the file there before");
            assert(gui_saves_eden_export(&done, out, &empty, 1, error, sizeof error));
            path = snx_format("0100AAAA00000000 has no save in %s", none);
            assert(!strcmp(error, path));
            free(path);
            assert(holds_text(out, "the file there before"));
            path = snx_format("%s.part", out); /* the zip begun for it, gone */
            assert(!snx_path_exists(path));
            free(path);
            gui_cleaned_free(&done);
            free(none);
        }
        free(out);
        free(b);
        free(a);
    }

    /* refusals, each naming what the zip holds or what is wrong with it; nothing written for any */
    {
        char* place = at(root, "refused", "0100ABCD12340000");
        GuiSaveNx save = {GAME, place, trash, "refused"};
        static const char x[] = "x";
        zip = snx_path_join(root, "refused.zip");

        put_text(zip, "not a zip at all, some text");
        assert(gui_saves_eden_import(&done, zip, zip, "", &save, 1, &result, error, sizeof error));
        path = snx_format("%s is no export of saves as Eden makes one: ", zip);
        assert(!strncmp(error, path, strlen(path)));
        free(path);

        {
            Member m[] = {{"save.bin", x, 1, 0, 0, false}};
            zip_make(zip, m, 1);
            assert(gui_saves_eden_import(&done, zip, zip, "", &save, 1, &result, error, sizeof error));
            path = snx_format("%s is no export of saves as Eden makes one: save.bin is not in a folder named by a "
                              "game's title ID, 16 hex digits",
                              zip);
            assert(!strcmp(error, path));
            free(path);
        }
        { /* Eden's layout of a save it calls the future one leaves its 0/ at the zip's top */
            Member m[] = {{"0/save.bin", x, 1, 0, 0, false}};
            zip_make(zip, m, 1);
            assert(gui_saves_eden_titles(zip, zip, "", &titles, &count, error, sizeof error) && !titles && !count);
            assert(strstr(error, ": 0/save.bin is not in a folder named by a game's title ID"));
        }
        {
            Member m[] = {{"0100AAAA00000000/../x", x, 1, 0, 0, false}};
            zip_make(zip, m, 1);
            assert(gui_saves_eden_import(&done, zip, zip, "", &save, 1, &result, error, sizeof error));
            path = snx_format("%s is no export of saves as Eden makes one: its member 0100AAAA00000000/../x is no "
                              "path inside a folder",
                              zip);
            assert(!strcmp(error, path));
            free(path);
        }
        zip_make(zip, NULL, 0);
        assert(gui_saves_eden_import(&done, zip, zip, "", &save, 1, &result, error, sizeof error));
        path = snx_format("%s is an empty zip: Eden's export of saves holds a folder for each game", zip);
        assert(!strcmp(error, path));
        free(path);
        {
            Member m[] = {{"0100AAAA00000000/x", x, 1, 0, 0, false}};
            zip_make(zip, m, 1);
            assert(gui_saves_eden_import(&done, zip, zip, "", &save, 1, &result, error, sizeof error));
            path = snx_format("%s holds the saves of 0100AAAA00000000, not of 0100ABCD12340000", zip);
            assert(!strcmp(error, path));
            free(path);
            /* a copy of the file picked: its messages name the file picked */
            assert(gui_saves_eden_import(&done, zip, "the picked.zip", "", &save, 1, &result, error, sizeof error));
            assert(!strcmp(error, "the picked.zip holds the saves of 0100AAAA00000000, not of 0100ABCD12340000"));
        }
        {
            Member m[] = {{"0100000000010000/x", x, 1, 0, 0, false}, {"0100000000020000/x", x, 1, 0, 0, false},
                          {"0100000000030000/x", x, 1, 0, 0, false}, {"0100000000040000/x", x, 1, 0, 0, false},
                          {"0100000000050000/x", x, 1, 0, 0, false}, {"0100000000060000/x", x, 1, 0, 0, false}};
            zip_make(zip, m, 6);
            assert(gui_saves_eden_import(&done, zip, zip, "", &save, 1, &result, error, sizeof error));
            path = snx_format("%s holds the saves of 0100000000010000, 0100000000020000, 0100000000030000, "
                              "0100000000040000 and 2 more, not of 0100ABCD12340000",
                              zip);
            assert(!strcmp(error, path));
            free(path);
        }
        path = at(root, "refused", NULL);
        assert(!snx_path_exists(path)); /* not even the folder above the save */
        free(path);
        gui_cleaned_free(&done);
        free(zip);
        free(place);
    }

    /* the trash cannot be made (under a file, which no user, root neither, can make a folder in): the save there
       before as it was, the import's own folder gone, and why said with the path */
    {
        char *blocked = at(root, "a file", NULL), *bad_trash = at(blocked, "trash", NULL);
        char* keep = at(root, "kept", "0100ABCD12340000");
        Member m[] = {{"0100ABCD12340000/new.bin", "new", 3, 0, 0, false}};
        GuiSaveNx save = {GAME, keep, bad_trash, "kept"};
        put_text(blocked, "a file, not a folder");
        path = at(keep, "0", "old.bin");
        put_text(path, "kept");
        free(path);
        zip = snx_path_join(root, "kept.zip");
        zip_make(zip, m, 1);
        assert(gui_saves_eden_import(&done, zip, zip, "", &save, 1, &result, error, sizeof error));
        path = snx_format("the trash, %s, could not be made: ", bad_trash);
        assert(!strncmp(error, path, strlen(path)));
        free(path);
        path = at(keep, "0", "old.bin");
        assert(holds_text(path, "kept"));
        free(path);
        path = at(keep, "0", "new.bin");
        assert(!snx_path_exists(path));
        free(path);
        path = at(root, "kept", ".0100ABCD12340000.importing");
        assert(!snx_path_exists(path));
        free(path);
        assert(done.count && strstr(done.text, "Not imported: the trash, "));
        gui_cleaned_free(&done);
        free(zip);
        free(keep);
        free(bad_trash);
        free(blocked);
    }

    /* the name an export's file takes: what it holds and when in local time, the letters no system takes in a name
       as _ */
    {
        char name[256], stamp[32], want[256];
        time_t when = 1794931200; /* 2026-11-17 */
        assert(strftime(stamp, sizeof stamp, "%Y-%m-%d %H-%M", localtime(&when)));
        gui_saves_file_name(name, sizeof name, "A: game/with \"bad\" <letters>|?*\\\t", (SnxU64)when, "zip");
        snprintf(want, sizeof want, "A_ game_with _bad_ _letters______ %s.zip", stamp);
        assert(!strcmp(name, want));
    }

    /* where a user's Switch saves are: save_layout=global's data folder, a game's own folder while only it holds one
       (an older version's place), every save of the user listed once in title ID order, and where a title's goes */
    {
        char *nx = at(root, "nx", NULL), *nx_data = at(nx, "data", NULL), *games = at(nx, "recompiled", NULL);
        char* shared_a = at(nx_data, "save/user." UID, "0100AAAA00000000");
        char* shared_d = at(nx_data, "save/user." UID, "0100DDDD00000000");
        char* own_b = at(games, "0100BBBB00000000/0100BBBB00000000_data", "save/user." UID);
        char *game_a = at(games, "0100AAAA00000000", NULL), *game_b = at(games, "0100BBBB00000000", NULL);
        char *game_c = at(games, "0100CCCC00000000", NULL), *own_c = at(game_c, "0100CCCC00000000_data", NULL);
        char* trash_b = at(games, "0100BBBB00000000/0100BBBB00000000_data", "trash");
        char *shared_c = at(nx_data, "save/user." UID, "0100CCCC00000000"), *nx_trash = at(nx_data, "trash", NULL);
        char *notes = at(nx_data, "save/user." UID, "notes"), *own_a = at(game_a, "0100AAAA00000000_data", NULL);
        char* shared_e = at(nx_data, "save/user." UID, "0100EEEE00000000");
        const char* folders[1] = {games};
        GuiSaveNx place;
        GuiSavesNx list;
        assert(!snx_mkdirs(shared_a) && !snx_mkdirs(shared_d) && !snx_mkdirs(own_b) && !snx_mkdirs(own_c) &&
               !snx_mkdirs(notes) && !snx_mkdirs(own_a));
        assert(gui_saves_nx_place(game_a, "0100AAAA00000000", nx_data, UID, true, &place) &&
               place.title == 0x0100AAAA00000000ull && !strcmp(place.folder, shared_a) &&
               !strcmp(place.trash, nx_trash) && !strcmp(place.name, "0100AAAA00000000.user." UID));
        place_free(&place);
        assert(gui_saves_nx_place(game_b, "0100BBBB00000000", nx_data, UID, true, &place) &&
               !strcmp(place.folder, own_b) && !strcmp(place.trash, trash_b));
        place_free(&place);
        assert(gui_saves_nx_place(game_c, "0100CCCC00000000", nx_data, UID, true, &place) &&
               !strcmp(place.folder, shared_c));
        place_free(&place);
        path = at(game_c, "0100CCCC00000000_data/save", "user." UID);
        assert(gui_saves_nx_place(game_c, "0100CCCC00000000", nx_data, UID, false, &place) &&
               !strcmp(place.folder, path));
        free(path);
        place_free(&place);
        assert(gui_saves_nx_place(game_a, "0100AAAA00000000", nx_data, UID, false, &place) &&
               !strcmp(place.folder, shared_a)); /* save_layout=game, only the data folder's holding it */
        place_free(&place);
        assert(!gui_saves_nx_place(game_c, "a homebrew", nx_data, UID, true, &place));
        gui_saves_nx_list(nx_data, folders, 1, UID, true, &list);
        assert(list.count == 3 && list.at[0].title == 0x0100AAAA00000000ull && !strcmp(list.at[0].folder, shared_a) &&
               list.at[1].title == 0x0100BBBB00000000ull && !strcmp(list.at[1].folder, own_b) &&
               list.at[2].title == 0x0100DDDD00000000ull && !strcmp(list.at[2].folder, shared_d));
        gui_saves_nx_free(&list);
        gui_saves_nx_list(nx_data, folders, 1, UID, false, &list);
        assert(list.count == 2 && !strcmp(list.at[0].folder, shared_a) && !strcmp(list.at[1].folder, own_b));
        gui_saves_nx_free(&list);
        assert(gui_saves_nx_find(nx_data, folders, 1, UID, true, 0x0100EEEE00000000ull, &place) &&
               !strcmp(place.folder, shared_e) && !strcmp(place.trash, nx_trash));
        place_free(&place);
        assert(!gui_saves_nx_find(nx_data, folders, 1, UID, false, 0x0100EEEE00000000ull, &place));
        free(shared_e);
        free(own_a);
        free(notes);
        free(nx_trash);
        free(shared_c);
        free(trash_b);
        free(own_c);
        free(game_c);
        free(game_b);
        free(game_a);
        free(own_b);
        free(shared_d);
        free(shared_a);
        free(games);
        free(nx_data);
        free(nx);
    }

    /* a zip of every console's: the Switch's under switch/, as Eden's, the other consoles' folders left to them; one
       written with that folder read back the same; one without it, or read as Eden's own, refused saying why */
    {
        char *every = at(root, "every.zip", NULL), *a = at(root, "every/a", "0100ABCD12340000");
        char* b = at(root, "every/b", "0100ABCD12340000");
        GuiSaveNx save = {GAME, a, trash, "every"}, back = {GAME, b, trash, "every"};
        GuiSavesZip* writing;
        size_t kept = 0, files = 0;
        SnxU64 took = 0;
        path = at(a, "0", "data.bin");
        put(path, bytes, 3000);
        free(path);
        path = at(a, "snx-journaled", NULL);
        put_text(path, "");
        free(path);
        writing = gui_saves_zip_begin(every, &done, error, sizeof error);
        assert(writing && !gui_saves_eden_add(writing, GUI_SAVES_SWITCH, &save, 1, &kept, error, sizeof error) &&
               kept == 1);
        assert(!gui_saves_zip_end(writing, true, &files, &took, error, sizeof error) && files == 1 && took == 3000);
        assert(has_member(every, "switch/0100ABCD12340000/") && has_member(every, "switch/0100ABCD12340000/data.bin"));
        assert(!gui_saves_eden_titles(every, every, GUI_SAVES_SWITCH, &titles, &count, error, sizeof error) &&
               count == 1 && titles[0] == GAME);
        free(titles);
        assert(!gui_saves_eden_import(&done, every, every, GUI_SAVES_SWITCH, &back, 1, &result, error, sizeof error));
        path = at(a, "0", NULL);
        path2 = at(b, "0", NULL);
        assert(same_tree(path, path2));
        free(path2);
        free(path);
        gui_cleaned_free(&done);
        {
            Member m[] = {{"switch/0100ABCD12340000/x", "x", 1, 0, 0, false}, {"3ds/a game/y", "y", 1, 0, 0, false}};
            zip_make(every, m, 2);
            assert(!gui_saves_eden_titles(every, every, GUI_SAVES_SWITCH, &titles, &count, error, sizeof error) &&
                   count == 1 && titles[0] == GAME);
            free(titles);
            assert(gui_saves_eden_titles(every, every, "", &titles, &count, error, sizeof error));
            path =
                snx_format("%s is no export of saves as Eden makes one: switch/0100ABCD12340000/x is not in a folder "
                           "named by a game's title ID, 16 hex digits",
                           every);
            assert(!strcmp(error, path));
            free(path);
        }
        {
            Member m[] = {{"3ds/a game/y", "y", 1, 0, 0, false}};
            zip_make(every, m, 1);
            assert(gui_saves_eden_titles(every, every, GUI_SAVES_SWITCH, &titles, &count, error, sizeof error));
            path = snx_format("%s holds no saves of the Switch: it has no switch/ folder", every);
            assert(!strcmp(error, path));
            free(path);
        }
        free(b);
        free(a);
        free(every);
    }

    /* the GBA's and the DS's: each game's <name>.sav, as mGBA and melonDS keep one; the games of a console found by
       their .ini; one in place of another's by gui_saves_put; a zip of them, a .sav each, read back by the games'
       names; an empty .sav and a member that is none refused */
    {
        char *raw = at(root, "raw", NULL), *lib = at(raw, "recompiled", NULL), *raw_trash = at(raw, "trash", NULL);
        char *a = at(lib, "A Game", NULL), *b = at(lib, "B Game", NULL), *c = at(lib, "C Game", NULL);
        char *d = at(lib, "0100DDDD00000000", NULL), *zipped = at(raw, "gba.zip", NULL),
             *picked = at(raw, "in.sav", NULL);
        const char* folders[1] = {lib};
        static const unsigned char sram[8] = {1, 2, 3, 4, 5, 6, 7, 8}, other[4] = {9, 9, 9, 9};
        GuiSavesRaw list;
        GuiSavesZip* writing;
        size_t kept = 0, files = 0;
        SnxU64 took = 0;
        path = at(a, "A Game.ini", NULL);
        put_text(path, "[Game]\nsystem=gba\n");
        free(path);
        path = at(a, "A Game.sav", NULL);
        put(path, sram, sizeof sram);
        free(path);
        path = at(b, "B Game.ini", NULL);
        put_text(path, "[Game]\nsystem=gba\n");
        free(path);
        path = at(c, "C Game.ini", NULL);
        put_text(path, "[Game]\nsystem=nds\n");
        free(path);
        path = at(c, "C Game.sav", NULL);
        put(path, other, sizeof other);
        free(path);
        assert(!snx_mkdirs(d));
        gui_saves_raw_list(raw_trash, folders, 1, SNX_SYSTEM_GBA, false, &list);
        assert(list.count == 2 && !strcmp(list.at[0].name, "A Game") && !strcmp(list.at[1].name, "B Game"));
        path = at(a, "A Game.sav", NULL);
        assert(!strcmp(list.at[0].file, path) && !strcmp(list.at[0].trash, raw_trash));
        free(path);
        gui_saves_raw_free(&list);
        gui_saves_raw_list(raw_trash, folders, 1, SNX_SYSTEM_GBA, true, &list);
        assert(list.count == 1 && !strcmp(list.at[0].name, "A Game"));
        gui_saves_raw_free(&list);
        gui_saves_raw_list(raw_trash, folders, 1, SNX_SYSTEM_NDS, false, &list);
        assert(list.count == 1 && !strcmp(list.at[0].name, "C Game"));
        gui_saves_raw_free(&list);

        /* a .sav picked in place of A Game's: the one there to the trash */
        gui_saves_raw_list(raw_trash, folders, 1, SNX_SYSTEM_GBA, false, &list);
        put(picked, other, sizeof other);
        result = (GuiSavesDone){0};
        assert(!gui_saves_raw_import(&done, picked, picked, &list.at[0], &result, error, sizeof error));
        assert(holds(list.at[0].file, other, sizeof other) && result.saves == 1 && result.bytes == 4 &&
               result.backups == 1 && holds(result.backup, sram, sizeof sram));
        path = snx_format("%s.importing", list.at[0].file);
        assert(!snx_path_exists(path));
        free(path);
        gui_cleaned_free(&done);
        put(picked, "", 0);
        assert(gui_saves_raw_import(&done, picked, "the picked.sav", &list.at[0], &result, error, sizeof error));
        assert(!strcmp(error, "the picked.sav is empty: a save holds the save memory's bytes") &&
               holds(list.at[0].file, other, sizeof other));
        gui_cleaned_free(&done);

        /* GBA's games into a zip of every console's under gba/, B Game with no save said; back into a library with
           none, by name, a .sav of no game of it said and left */
        put(list.at[0].file, sram, sizeof sram);
        writing = gui_saves_zip_begin(zipped, &done, error, sizeof error);
        assert(writing && !gui_saves_raw_add(writing, GUI_SAVES_GBA, list.at, list.count, &kept, error, sizeof error) &&
               kept == 1 && strstr(done.text, "Not exported: no save there"));
        assert(!gui_saves_zip_end(writing, true, &files, &took, error, sizeof error) && files == 1 && took == 8);
        assert(has_member(zipped, "gba/A Game.sav") && gui_saves_zip_has(zipped, GUI_SAVES_GBA) &&
               !gui_saves_zip_has(zipped, GUI_SAVES_NDS));
        assert(gui_saves_raw_there(zipped, GUI_SAVES_GBA, list.at, list.count) == 1);
        gui_cleaned_free(&done);
        path = at(a, "A Game.sav", NULL);
        assert(!snx_remove_tree(path));
        free(path);
        assert(!gui_saves_raw_there(zipped, GUI_SAVES_GBA, list.at, list.count));
        result = (GuiSavesDone){0};
        assert(!gui_saves_raw_import_zip(&done, zipped, zipped, GUI_SAVES_GBA, list.at, list.count, &result, error,
                                         sizeof error));
        assert(holds(list.at[0].file, sram, sizeof sram) && result.saves == 1 && !result.backups);
        gui_cleaned_free(&done);
        {
            Member m[] = {{"Z Game.sav", sram, 8, 0, 0, false}, {"A Game.sav", other, 4, 8, 6, false}};
            zip_make(zipped, m, 2);
            assert(!gui_saves_raw_import_zip(&done, zipped, zipped, "", list.at, list.count, &result, error,
                                             sizeof error));
            assert(holds(list.at[0].file, other, sizeof other) &&
                   strstr(done.text, "Not imported: no game of the library is named Z Game"));
            gui_cleaned_free(&done);
        }
        {
            Member m[] = {{"A Game.sav", sram, 8, 0, 0, false}, {"notes/readme.txt", "x", 1, 0, 0, false}};
            zip_make(zipped, m, 2);
            assert(gui_saves_raw_import_zip(&done, zipped, "the picked.zip", "", list.at, list.count, &result, error,
                                            sizeof error));
            assert(!strcmp(error, "the picked.zip holds notes/readme.txt, which is no .sav at its top as each game's "
                                  "save goes"));
            assert(holds(list.at[0].file, other, sizeof other)); /* nothing written: the whole zip checked first */
            gui_cleaned_free(&done);
        }
        {
            Member m[] = {{"switch/0100ABCD12340000/x", "x", 1, 0, 0, false}};
            zip_make(zipped, m, 1);
            assert(gui_saves_raw_import_zip(&done, zipped, "the picked.zip", GUI_SAVES_GBA, list.at, list.count,
                                            &result, error, sizeof error));
            assert(!strcmp(error, "the picked.zip holds no .sav in its folder gba/"));
            gui_cleaned_free(&done);
        }
        gui_saves_raw_free(&list);
        free(picked);
        free(zipped);
        free(d);
        free(c);
        free(b);
        free(a);
        free(raw_trash);
        free(lib);
        free(raw);
    }

    /* the 3DS's, as Checkpoint lays its backups out: saves/0x<unique ID> <title>/<backup>/ and extdata/ the same; the
       newest backup taken; only into a game that has run (savedata.format), the one ExtSaveData of its SD card */
    {
        char *ctr = at(root, "ctr", NULL), *lib = at(ctr, "a", NULL), *lib2 = at(ctr, "b", NULL);
        char *ctr_trash = at(ctr, "trash", NULL), *game = at(lib, "0004000000123400", NULL);
        char *game2 = at(lib2, "0004000000123400", NULL), *zipped = at(ctr, "checkpoint.zip", NULL);
        char* ext =
            at(game, "sdmc/Nintendo 3DS/00000000000000000000000000000000/00000000000000000000000000000000/extdata",
               "00000000/00001234/user");
        char* ext2 = at(game2,
                        "sdmc/Nintendo 3DS/00000000000000000000000000000000/00000000000000000000000000000000/"
                        "extdata",
                        "00000000/00001234/user");
        const char* folders[1] = {lib};
        GuiSaveCtr one, other;
        GuiSavesCtr list;
        GuiSavesZip* writing;
        size_t kept = 0, files = 0;
        SnxU64 took = 0;
        path = at(game, "0004000000123400.ini", NULL);
        put_text(path, "[Game]\nsystem=3ds\n");
        free(path);
        path = at(game, "savedata", "game.bin");
        put_text(path, "3ds save");
        free(path);
        path = at(game, "savedata.format", NULL);
        put(path, pattern, 20);
        free(path);
        path = at(ext, "x.bin", NULL);
        put_text(path, "extra");
        free(path);
        assert(gui_saves_ctr_place(game, "0004000000123400", "A: Game.", ctr_trash, &one) && one.unique == 0x1234 &&
               one.extdata_count == 1 && !strcmp(one.extdata, ext));
        gui_saves_ctr_list(ctr_trash, folders, 1, true, &list);
        assert(list.count == 1 && !strcmp(list.at[0].name, "0004000000123400"));
        gui_saves_ctr_free(&list);

        /* out: a backup each, its folder named as Checkpoint names it ("A: Game." its .: as spaces, trailing ones off) */
        writing = gui_saves_zip_begin(zipped, &done, error, sizeof error);
        assert(writing &&
               !gui_saves_ctr_add(writing, GUI_SAVES_3DS, &one, 1, "20261117-120000", &kept, error, sizeof error) &&
               kept == 1);
        assert(!gui_saves_zip_end(writing, true, &files, &took, error, sizeof error) && files == 2);
        assert(has_member(zipped, "3ds/saves/0x01234 A  Game/20261117-120000/game.bin") &&
               has_member(zipped, "3ds/extdata/0x01234 A  Game/20261117-120000/x.bin"));
        gui_cleaned_free(&done);

        /* in, to the same game in another library that has run: both in place, what was there to the trash */
        path = at(game2, "savedata", "old.bin");
        put_text(path, "old save");
        free(path);
        path = at(ext2, "old.bin", NULL);
        put_text(path, "old extra");
        free(path);
        path = at(game2, "savedata.format", NULL);
        put(path, pattern, 20);
        free(path);
        assert(gui_saves_ctr_place(game2, "0004000000123400", "A Game", ctr_trash, &other));
        result = (GuiSavesDone){0};
        assert(!gui_saves_ctr_import(&done, zipped, zipped, GUI_SAVES_3DS, &other, 1, &result, error, sizeof error));
        path = at(game2, "savedata", "game.bin");
        assert(holds_text(path, "3ds save"));
        free(path);
        path = at(ext2, "x.bin", NULL);
        assert(holds_text(path, "extra"));
        free(path);
        path = at(game2, "savedata", "old.bin");
        assert(!snx_path_exists(path) && result.saves == 1 && result.files == 2 && result.backups == 2);
        free(path);
        gui_cleaned_free(&done);

        /* two backups: the newest; one with a file out of the layout, or of another game only: refused, nothing written */
        {
            Member m[] = {{"saves/0x01234 A Game/20250101-000000/game.bin", "older", 5, 0, 0, false},
                          {"saves/0x01234 A Game/20261117-130000/game.bin", "newest", 6, 8, 6, false}};
            zip_make(zipped, m, 2);
            assert(!gui_saves_ctr_import(&done, zipped, zipped, "", &other, 1, &result, error, sizeof error));
            path = at(game2, "savedata", "game.bin");
            assert(holds_text(path, "newest"));
            free(path);
            gui_cleaned_free(&done);
        }
        {
            Member m[] = {{"saves/0x01234 A Game/20261117-140000/game.bin", "x", 1, 0, 0, false},
                          {"readme.txt", "x", 1, 0, 0, false}};
            zip_make(zipped, m, 2);
            assert(gui_saves_ctr_import(&done, zipped, "the picked.zip", "", &other, 1, &result, error, sizeof error));
            assert(!strcmp(error, "the picked.zip is no export of saves as Checkpoint lays them out: readme.txt is not "
                                  "in saves/ or extdata/, in a title's folder 0x<its unique ID> and one of its "
                                  "backups"));
            path = at(game2, "savedata", "game.bin");
            assert(holds_text(path, "newest"));
            free(path);
        }
        {
            Member m[] = {{"saves/0x0AAAA Other/20261117-140000/game.bin", "x", 1, 0, 0, false}};
            zip_make(zipped, m, 1);
            assert(gui_saves_ctr_import(&done, zipped, "the picked.zip", "", &other, 1, &result, error, sizeof error));
            assert(!strcmp(error, "the picked.zip holds the backups of 0x0AAAA, not of A Game's, 0x01234"));
        }
        /* an ExtSaveData backup into a game with none of its own yet, or with two, which one taking it cannot tell:
           refused, each saying what to do */
        {
            char* game3 = at(ctr, "c/0004000000123400", NULL);
            char* sd = at(game3, "sdmc/Nintendo 3DS/00000000000000000000000000000000/00000000000000000000000000000000",
                          "extdata");
            GuiSaveCtr third;
            GuiSavesCtr only = {NULL, 0};
            Member m[] = {{"extdata/0x01234 A Game/20261117-140000/x.bin", "x", 1, 0, 0, false}};
            path = at(game3, "savedata.format", NULL);
            put(path, pattern, 20);
            free(path);
            zip_make(zipped, m, 1);
            assert(gui_saves_ctr_place(game3, "0004000000123400", "C Game", ctr_trash, &third) && !third.extdata_count);
            assert(gui_saves_ctr_import(&done, zipped, "the picked.zip", "", &third, 1, &result, error, sizeof error));
            assert(!strcmp(error,
                           "C Game has no extra data yet (0 folders): its first run makes it. Play it once, then "
                           "import"));
            only.at = (GuiSaveCtr*)malloc(sizeof third);
            assert(only.at);
            only.at[0] = third;
            only.count = 1;
            gui_saves_ctr_free(&only);
            path = at(sd, "00000000/00001234/user", "a.bin");
            put_text(path, "a");
            free(path);
            path = at(sd, "00000000/00005678/user", "b.bin");
            put_text(path, "b");
            free(path);
            assert(gui_saves_ctr_place(game3, "0004000000123400", "C Game", ctr_trash, &third) &&
                   third.extdata_count == 2 && !third.extdata);
            assert(gui_saves_ctr_import(&done, zipped, "the picked.zip", "", &third, 1, &result, error, sizeof error));
            assert(!strcmp(error,
                           "C Game has 2 folders of extra data in its SD card, and the backup's goes in one: move "
                           "the others out, then import"));
            only.at = (GuiSaveCtr*)malloc(sizeof third);
            assert(only.at);
            only.at[0] = third;
            only.count = 1;
            gui_saves_ctr_free(&only);
            gui_cleaned_free(&done);
            free(sd);
            free(game3);
        }
        /* a game that has not run: its save is made by its first run, which the import says to do first */
        path = at(game2, "savedata.format", NULL);
        assert(!snx_remove_tree(path));
        {
            Member m[] = {{"saves/0x01234 A Game/20261117-140000/game.bin", "x", 1, 0, 0, false}};
            zip_make(zipped, m, 1);
            assert(gui_saves_ctr_import(&done, zipped, "the picked.zip", "", &other, 1, &result, error, sizeof error));
            path2 = snx_format("A Game has not run yet: its first run makes its save (%s), which an import goes into, "
                               "else the game formats it over what came in. Play it once, then import",
                               path);
            assert(!strcmp(error, path2));
            free(path2);
        }
        free(path);
        gui_cleaned_free(&done);
        {
            GuiSavesCtr two = {&one, 1}, three = {&other, 1};
            GuiSaveCtr* a = (GuiSaveCtr*)malloc(sizeof one);
            GuiSaveCtr* b = (GuiSaveCtr*)malloc(sizeof other);
            assert(a && b);
            *a = one;
            *b = other;
            two.at = a;
            three.at = b;
            gui_saves_ctr_free(&two);
            gui_saves_ctr_free(&three);
        }
        free(ext2);
        free(ext);
        free(zipped);
        free(game2);
        free(game);
        free(ctr_trash);
        free(lib2);
        free(lib);
        free(ctr);
    }

    /* the Vita's: ux0/user/00/savedata/<ID> decrypted, as Vita3K keeps it, a folder <ID>/ in a zip; one copied from
       the console still encrypted (sce_pfs/) refused, as is a member out of an ID's folder and another game's alone */
    {
        char *psv = at(root, "psv", NULL), *lib = at(psv, "a", NULL), *lib2 = at(psv, "b", NULL);
        char *psv_trash = at(psv, "trash", NULL), *game = at(lib, "PCSE00001", NULL),
             *game2 = at(lib2, "PCSE00001", NULL);
        char* zipped = at(psv, "vita.zip", NULL);
        const char* folders[1] = {lib};
        GuiSavePsv one, other;
        GuiSavesPsv list;
        GuiSavesZip* writing;
        size_t kept = 0, files = 0;
        SnxU64 took = 0;
        path = at(game, "PCSE00001.ini", NULL);
        put_text(path, "[Game]\nsystem=vita\n");
        free(path);
        assert(gui_saves_psv_place(game, "PCSE00001", psv_trash, &one));
        path = at(game, "ux0/user/00/savedata", "PCSE00001");
        assert(!strcmp(one.folder, path));
        free(path);
        path = at(one.folder, "SAVEDATA.BIN", NULL);
        put_text(path, "vita save");
        free(path);
        path = at(one.folder, "sce_sys/sdslot.dat", NULL);
        put_text(path, "slots");
        free(path);
        gui_saves_psv_list(psv_trash, folders, 1, true, &list);
        assert(list.count == 1 && !strcmp(list.at[0].name, "PCSE00001"));
        gui_saves_psv_free(&list);
        writing = gui_saves_zip_begin(zipped, &done, error, sizeof error);
        assert(writing && !gui_saves_psv_add(writing, GUI_SAVES_VITA, &one, 1, &kept, error, sizeof error) &&
               kept == 1);
        assert(!gui_saves_zip_end(writing, true, &files, &took, error, sizeof error) && files == 2);
        assert(has_member(zipped, "vita/PCSE00001/SAVEDATA.BIN") &&
               has_member(zipped, "vita/PCSE00001/sce_sys/sdslot.dat"));
        gui_cleaned_free(&done);
        /* into the same game elsewhere, never run: made; again: the one there to the trash */
        assert(gui_saves_psv_place(game2, "PCSE00001", psv_trash, &other));
        result = (GuiSavesDone){0};
        assert(!gui_saves_psv_import(&done, zipped, zipped, GUI_SAVES_VITA, &other, 1, false, &result, error,
                                     sizeof error));
        path = at(other.folder, "sce_sys/sdslot.dat", NULL);
        assert(holds_text(path, "slots") && result.saves == 1 && result.files == 2 && !result.backups);
        free(path);
        assert(!gui_saves_psv_import(&done, zipped, zipped, GUI_SAVES_VITA, &other, 1, false, &result, error,
                                     sizeof error) &&
               result.backups == 1);
        gui_cleaned_free(&done);
        {
            Member m[] = {{"PCSE00001/sce_pfs/files.db", "x", 1, 0, 0, false}};
            zip_make(zipped, m, 1);
            assert(gui_saves_psv_import(&done, zipped, "the picked.zip", "", &other, 1, false, &result, error,
                                        sizeof error));
            assert(!strcmp(error, "the picked.zip holds PCSE00001's save as the console keeps it, encrypted "
                                  "(PCSE00001/sce_pfs/files.db): only a decrypted one goes in, as Vita3K keeps one"));
        }
        {
            Member m[] = {{"SAVEDATA.BIN", "x", 1, 0, 0, false}};
            zip_make(zipped, m, 1);
            assert(gui_saves_psv_import(&done, zipped, "the picked.zip", "", &other, 1, false, &result, error,
                                        sizeof error));
            assert(!strcmp(error, "the picked.zip is no export of Vita saves: SAVEDATA.BIN is not in a folder named by "
                                  "its save's ID, as ux0:user/00/savedata/<ID> is"));
        }
        {
            Member m[] = {{"PCSE00002/SAVEDATA.BIN", "x", 1, 0, 0, false}};
            zip_make(zipped, m, 1);
            assert(gui_saves_psv_import(&done, zipped, "the picked.zip", "", &other, 1, false, &result, error,
                                        sizeof error));
            assert(!strcmp(error, "the picked.zip holds the saves of PCSE00002, not of PCSE00001"));
            assert(gui_saves_psv_import(&done, zipped, "the picked.zip", "", &other, 1, true, &result, error,
                                        sizeof error));
            assert(!strcmp(error, "none of the saves in the picked.zip, PCSE00002, is of a Vita game of the library"));
            path = at(other.folder, "SAVEDATA.BIN", NULL);
            assert(holds_text(path, "vita save"));
            free(path);
        }
        gui_cleaned_free(&done);
        {
            GuiSavePsv* a = (GuiSavePsv*)malloc(2 * sizeof one);
            GuiSavesPsv both = {a, 2};
            assert(a);
            a[0] = one;
            a[1] = other;
            gui_saves_psv_free(&both);
        }
        free(zipped);
        free(game2);
        free(game);
        free(psv_trash);
        free(lib2);
        free(lib);
        free(psv);
    }

#ifndef _WIN32 /* a link Windows makes only with developer mode or an administrator */
    /* a save folder that is a link: refused, naming what it links to, the link and that folder as they were */
    {
        char *target = at(root, "elsewhere", NULL), *linked = at(root, "linked", "0100ABCD12340000");
        Member m[] = {{"0100ABCD12340000/new.bin", "new", 3, 0, 0, false}};
        GuiSaveNx save = {GAME, linked, trash, "linked"};
        path = at(target, "mine.bin", NULL);
        put_text(path, "mine");
        free(path);
        path = at(root, "linked", NULL);
        assert(!snx_mkdirs(path));
        free(path);
        assert(!snx_symlink(target, linked));
        zip = snx_path_join(root, "linked.zip");
        zip_make(zip, m, 1);
        assert(gui_saves_eden_import(&done, zip, zip, "", &save, 1, &result, error, sizeof error));
        path = snx_format("%s is a link to %s: put the save's files there by hand", linked, target);
        assert(!strcmp(error, path));
        free(path);
        path = at(linked, "mine.bin", NULL);
        assert(snx_is_link(linked) && holds_text(path, "mine"));
        free(path);
        path = at(target, "new.bin", NULL);
        assert(!snx_path_exists(path));
        free(path);
        gui_cleaned_free(&done);
        free(zip);
        free(linked);
        free(target);
    }
#endif

    assert(!snx_remove_tree(root));
    free(folder);
    free(trash);
    free(data);
    free(root);
    puts("gui_saves: ok");
    return 0;
}
