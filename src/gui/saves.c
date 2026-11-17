/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/saves.h"
#include "common/platform/file.h"
#include "common/utilities/zip.h"
#include "gui/i18n.h"
#include <libdeflate.h>
#include <miniz.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- what every console's format shares */

/* a free name in trash for the backup of name: <name>.<UTC time>, a number after it rather than over one there, its
   note's free too; NULL when a hundred of this second are there */
static char* backup_place(const char* trash, const char* name) {
    char when[32];
    time_t now = time(NULL);
    const struct tm* utc = gmtime(&now);
    if (!utc || !strftime(when, sizeof when, "%Y%m%d-%H%M%S", utc)) snprintf(when, sizeof when, "%lld", (long long)now);
    for (int i = 1; i < 100; ++i) {
        char* leaf = i == 1 ? snx_format("%s.%s", name, when) : snx_format("%s.%s-%d", name, when, i);
        char* place = leaf ? snx_path_join(trash, leaf) : NULL;
        char* note = place ? snx_format("%s" SNX_SAVE_NOTE, place) : NULL;
        bool free_name = note && !snx_path_exists(place) && !snx_path_exists(note);
        free(note);
        free(leaf);
        if (free_name) return place;
        free(place);
    }
    return NULL;
}
int gui_saves_put(GuiCleaned* done, const char* staged, const char* save, const char* trash, const char* name,
                  char* backup, size_t backup_size, char* error, size_t size) {
    char *note = snx_format("%s" SNX_SAVE_NOTE, save), *place = NULL, *place_note = NULL, what[4400], why[256];
    bool moved = false, noted = false, had;
    int rc = 1;
    backup[0] = 0;
    error[0] = 0;
    if (snx_is_link(save)) { /* what it names would stay as it is, and a folder take the link's place */
        char* target = snx_link_target(save);
        snprintf(error, size, snx_tr("%s is a link to %s: put the save's files there by hand"), save,
                 target ? target : "?");
        free(target);
        goto out;
    }
    if (snx_is_dir(save) && !snx_remove_empty_dir(save))
        gui_cleaned_add(done, true, save, snx_tr("Removed: an empty folder, no save"));
    had = snx_path_exists(save);
    if (had || snx_is_file(note)) {
        const char* first = had ? save : note;
        if (snx_mkdirs(trash))
            snprintf(error, size, snx_tr("the trash, %s, could not be made: %s"), trash, snx_os_error(why, sizeof why));
        else if (!snx_same_disk(first, trash))
            snprintf(error, size, snx_tr("the trash, %s, is on another disk than %s, which would be copied there"),
                     trash, first);
        else if (!(place = backup_place(trash, name)))
            snprintf(error, size, snx_tr("the trash, %s, has a hundred backups of %s from this second"), trash, name);
        else if (had && plat_replace_file(save, place))
            snprintf(error, size, snx_tr("%s could not be moved to the trash, %s: %s"), save, place,
                     snx_os_error(why, sizeof why));
        else {
            moved = had;
            place_note = snx_format("%s" SNX_SAVE_NOTE, place);
            if (snx_is_file(note) && plat_replace_file(note, place_note))
                snprintf(error, size, snx_tr("%s could not be moved to the trash, %s: %s"), note, place_note,
                         snx_os_error(why, sizeof why));
            else
                noted = snx_is_file(place_note);
        }
        if (error[0]) goto out;
    }
    if (plat_replace_file(staged, save)) {
        snprintf(error, size, snx_tr("%s could not take the place of %s: %s"), staged, save,
                 snx_os_error(why, sizeof why));
        goto out;
    }
    rc = 0;
    if (moved) {
        snprintf(backup, backup_size, "%s", place);
        snprintf(what, sizeof what, snx_tr("Moved to the trash, %s, before the new save took its place"), place);
        gui_cleaned_add(done, true, save, what);
    }
    if (noted) {
        snprintf(what, sizeof what, snx_tr("Moved to the trash, %s, with the save it notes"), place_note);
        gui_cleaned_add(done, true, note, what);
        if (!moved) snprintf(backup, backup_size, "%s", place_note);
    }
out:
    if (rc) { /* the save before back where it was, and what was put together for nothing */
        if (noted && plat_replace_file(place_note, note))
            snprintf(error + strlen(error), size - strlen(error), snx_tr("; its note stays in the trash, %s"),
                     place_note);
        if (moved && plat_replace_file(place, save))
            snprintf(error + strlen(error), size - strlen(error),
                     snx_tr("; the save there before stays in the trash, %s"), place);
        if (snx_path_exists(staged)) snx_remove_tree(staged);
    }
    free(place_note);
    free(place);
    free(note);
    return rc;
}

void gui_saves_file_name(char* out, size_t size, const char* what, SnxU64 when, const char* extension) {
    char stamp[32];
    time_t at = (time_t)when;
    const struct tm* local = localtime(&at);
    size_t i;
    if (!local || !strftime(stamp, sizeof stamp, "%Y-%m-%d %H-%M", local))
        snprintf(stamp, sizeof stamp, "%llu", (unsigned long long)when);
    snprintf(out, size, "%s %s.%s", what, stamp, extension);
    for (i = 0; out[i]; ++i)
        if ((unsigned char)out[i] < 32 || strchr("\\/:*?\"<>|", out[i])) out[i] = '_';
}

/* ---- the zip an export writes */

struct GuiSavesZip {
    mz_zip_archive zip;
    FILE* file;
    struct libdeflate_compressor* deflate;
    GuiCleaned* done;
    char *path, *part;
    size_t files;
    SnxU64 bytes;
};
/* miniz's writes, at an offset of the zip's file */
static size_t zip_write(void* opaque, mz_uint64 at, const void* data, size_t n) {
    FILE* f = (FILE*)opaque;
    if (snx_tell(f) != (int64_t)at && snx_seek(f, at)) return 0;
    return fwrite(data, 1, n, f);
}
GuiSavesZip* gui_saves_zip_begin(const char* path, GuiCleaned* done, char* error, size_t size) {
    GuiSavesZip* z = (GuiSavesZip*)calloc(1, sizeof *z);
    char why[256];
    error[0] = 0;
    if (!z) {
        snprintf(error, size, "%s", snx_tr("out of memory"));
        return NULL;
    }
    z->done = done;
    z->path = snx_strdup(path);
    z->part = snx_format("%s.part", path);
    z->deflate = libdeflate_alloc_compressor(6);
    z->file = z->deflate ? snx_fopen(z->part, "wb") : NULL;
    if (!z->file) {
        snprintf(error, size, snx_tr("%s could not be made: %s"), z->part,
                 z->deflate ? snx_os_error(why, sizeof why) : snx_tr("no memory for libdeflate's compressor"));
    } else {
        mz_zip_zero_struct(&z->zip);
        z->zip.m_pWrite = zip_write;
        z->zip.m_pIO_opaque = z->file;
        if (mz_zip_writer_init_v2(&z->zip, 0, 0)) return z;
        snprintf(error, size, snx_tr("%s could not be made: %s"), z->part,
                 mz_zip_get_error_string(mz_zip_get_last_error(&z->zip)));
        fclose(z->file);
        z->file = NULL;
        snx_remove_tree(z->part);
    }
    libdeflate_free_compressor(z->deflate);
    free(z->part);
    free(z->path);
    free(z);
    return NULL;
}
int gui_saves_zip_end(GuiSavesZip* z, bool keep, size_t* files, SnxU64* bytes, char* error, size_t size) {
    char why[256];
    bool failed = !keep || error[0];
    if (keep && !failed && !mz_zip_writer_finalize_archive(&z->zip)) {
        snprintf(error, size, snx_tr("%s could not be made: %s"), z->part,
                 mz_zip_get_error_string(mz_zip_get_last_error(&z->zip)));
        failed = true;
    }
    mz_zip_writer_end(&z->zip);
    if (fclose(z->file) && !failed) {
        snprintf(error, size, snx_tr("%s could not be written: %s"), z->part, snx_os_error(why, sizeof why));
        failed = true;
    }
    if (!failed && plat_replace_file(z->part, z->path)) {
        snprintf(error, size, snx_tr("%s could not take the place of %s: %s"), z->part, z->path,
                 snx_os_error(why, sizeof why));
        failed = true;
    }
    if (failed && snx_path_exists(z->part)) snx_remove_tree(z->part);
    *files = z->files;
    *bytes = z->bytes;
    libdeflate_free_compressor(z->deflate);
    free(z->part);
    free(z->path);
    free(z);
    return failed;
}
/* file into the zip as name, deflated when that makes it smaller; 0, else nonzero with why in error */
static int zip_file(GuiSavesZip* z, const char* file, const char* name, char* error, size_t size) {
    size_t n = 0, bound, packed = 0;
    unsigned char *data = snx_read_file(file, &n), *out = NULL;
    MZ_TIME_T when = (MZ_TIME_T)snx_file_time(file);
    char why[256], what[4400];
    mz_bool added;
    if (!data) {
        snprintf(error, size, snx_tr("%s could not be read: %s"), file, snx_os_error(why, sizeof why));
        return 1;
    }
    if (n) {
        bound = libdeflate_deflate_compress_bound(z->deflate, n);
        out = (unsigned char*)malloc(bound);
        packed = out ? libdeflate_deflate_compress(z->deflate, data, n, out, bound) : 0;
    }
    if (packed && packed < n) /* miniz writes the zip around the deflate, its size and CRC-32 as given */
        added = mz_zip_writer_add_mem_ex_v2(&z->zip, name, out, packed, NULL, 0,
                                            MZ_DEFAULT_LEVEL | MZ_ZIP_FLAG_COMPRESSED_DATA, n,
                                            (mz_uint32)libdeflate_crc32(0, data, n), &when, NULL, 0, NULL, 0);
    else
        added = mz_zip_writer_add_mem_ex_v2(&z->zip, name, data, n, NULL, 0, MZ_NO_COMPRESSION, 0, 0, &when, NULL, 0,
                                            NULL, 0);
    free(out);
    free(data);
    if (!added) {
        snprintf(error, size, snx_tr("%s could not go into the zip: %s"), file,
                 mz_zip_get_error_string(mz_zip_get_last_error(&z->zip)));
        return 1;
    }
    ++z->files;
    z->bytes += n;
    snprintf(what, sizeof what, snx_tr("Exported as %s"), name);
    gui_cleaned_add(z->done, true, file, what);
    return 0;
}
/* folder's folders and files into the zip under prefix (a folder's name and the folders down to it, ending in /), each
   folder an entry of its own as Qt's export has them, so one with nothing in it comes back too; at a save's top as
   the versions before kept one (flat), not the names SNX_SAVE_HIDDEN takes for a sync tool's */
static int zip_tree(GuiSavesZip* z, const char* folder, const char* prefix, bool flat, char* error, size_t size) {
    char** names = NULL;
    size_t count = 0, i;
    MZ_TIME_T when = (MZ_TIME_T)snx_file_time(folder);
    char why[256];
    int rc = 0;
    if (!mz_zip_writer_add_mem_ex_v2(&z->zip, prefix, NULL, 0, NULL, 0, MZ_NO_COMPRESSION, 0, 0, &when, NULL, 0, NULL,
                                     0)) {
        snprintf(error, size, snx_tr("%s could not go into the zip: %s"), folder,
                 mz_zip_get_error_string(mz_zip_get_last_error(&z->zip)));
        return 1;
    }
    if (snx_list_dir(folder, &names, &count)) {
        snprintf(error, size, snx_tr("%s could not be read: %s"), folder, snx_os_error(why, sizeof why));
        return 1;
    }
    for (i = 0; i < count && !rc; ++i) {
        char *path = snx_path_join(folder, names[i]), *name;
        if (flat && SNX_SAVE_HIDDEN(names[i])) {
            gui_cleaned_add(z->done, true, path,
                            snx_tr("Left out: a name that starts with a dot beside a save's files is a sync tool's or "
                                   "the system's"));
            free(path);
            continue;
        }
        name = snx_format("%s%s", prefix, names[i]);
        if (snx_is_dir(path)) {
            char* deeper = snx_format("%s/", name);
            rc = zip_tree(z, path, deeper, false, error, size);
            free(deeper);
        } else if (snx_is_file(path))
            rc = zip_file(z, path, name, error, size);
        free(name);
        free(path);
    }
    snx_free_strings(names, count);
    return rc;
}
/* the files under folder, its folders' too; flat as zip_tree's */
static size_t files_in(const char* folder, bool flat) {
    char** names = NULL;
    size_t count = 0, i, files = 0;
    if (snx_list_dir(folder, &names, &count)) return 0;
    for (i = 0; i < count; ++i) {
        char* path = flat && SNX_SAVE_HIDDEN(names[i]) ? NULL : snx_path_join(folder, names[i]);
        if (path && snx_is_dir(path))
            files += files_in(path, false);
        else if (path && snx_is_file(path))
            ++files;
        free(path);
    }
    snx_free_strings(names, count);
    return files;
}

/* ---- the Switch's: where ScarletNX keeps a save, and Eden's zip */

#define EDEN_SIZE_FILE "/.yuzu_save_size" /* Eden's record of a save's size, in its title ID's folder */

void gui_saves_nx_free(GuiSavesNx* list) {
    for (size_t i = 0; i < list->count; ++i) {
        free((void*)list->at[i].folder);
        free((void*)list->at[i].trash);
        free((void*)list->at[i].name);
    }
    free(list->at);
    list->at = NULL;
    list->count = 0;
}
/* out's strings, owned: the save's folder, its trash and its name there */
static void nx_set(GuiSaveNx* out, SnxU64 title, char* folder, char* trash, const char* uid) {
    out->title = title;
    out->folder = folder;
    out->trash = trash;
    out->name = snx_format("%016llX.user.%s", (unsigned long long)title, uid);
}
int gui_saves_nx_place(const char* game, const char* name, const char* root, const char* uid, bool global,
                       GuiSaveNx* out) {
    SnxU64 title;
    char *data, *own, *own_trash, *shared, *shared_trash;
    bool shared_one;
    if (!snx_title_parse(name, &title)) return 0;
    data = snx_format("%s/%s_data", game, name);
    own = snx_format("%s/save/user.%s", data, uid);
    own_trash = snx_path_join(data, "trash");
    shared = snx_format("%s/save/user.%s/%s", root, uid, name);
    shared_trash = snx_path_join(root, "trash");
    snx_path_native(own);
    snx_path_native(shared);
    /* the layout's own place, but while only the other's holds the save, which the runtime plays where it is */
    shared_one = global ? snx_is_dir(shared) || !snx_is_dir(own) : !snx_is_dir(own) && snx_is_dir(shared);
    if (shared_one) {
        nx_set(out, title, shared, shared_trash, uid);
        free(own);
        free(own_trash);
    } else {
        nx_set(out, title, own, own_trash, uid);
        free(shared);
        free(shared_trash);
    }
    free(data);
    return 1;
}
/* list holds title already */
static bool nx_has(const GuiSavesNx* list, SnxU64 title) {
    for (size_t i = 0; i < list->count; ++i)
        if (list->at[i].title == title) return true;
    return false;
}
static void nx_push(GuiSavesNx* list, const GuiSaveNx* save) {
    list->at = (GuiSaveNx*)snx_realloc(list->at, (list->count + 1) * sizeof *list->at);
    list->at[list->count++] = *save;
}
static int by_save_title(const void* a, const void* b) {
    SnxU64 x = ((const GuiSaveNx*)a)->title, y = ((const GuiSaveNx*)b)->title;
    return x < y ? -1 : x > y;
}
/* the folder of folders that holds the game named name, as a game folder (<name>/<name>_data/), NULL for none */
static const char* nx_game_folder(const char* const* folders, size_t count, const char* name, char** game) {
    for (size_t i = 0; i < count; ++i) {
        char* data = snx_format("%s/%s/%s_data", folders[i], name, name);
        bool has = data && snx_is_dir(data);
        free(data);
        if (has) {
            *game = snx_path_join(folders[i], name);
            return folders[i];
        }
    }
    *game = NULL;
    return NULL;
}
int gui_saves_nx_find(const char* root, const char* const* folders, size_t count, const char* uid, bool global,
                      SnxU64 title, GuiSaveNx* out) {
    char name[17], *game;
    int found = 0;
    if (!snx_title_text(title, name)) return 0;
    if (nx_game_folder(folders, count, name, &game)) found = gui_saves_nx_place(game, name, root, uid, global, out);
    free(game);
    if (!found && global && root) {
        char* shared = snx_format("%s/save/user.%s/%s", root, uid, name);
        snx_path_native(shared);
        nx_set(out, title, shared, snx_path_join(root, "trash"), uid);
        found = 1;
    }
    return found;
}
void gui_saves_nx_list(const char* root, const char* const* folders, size_t count, const char* uid, bool global,
                       GuiSavesNx* list) {
    char **names = NULL, *users = root ? snx_format("%s/save/user.%s", root, uid) : NULL;
    size_t n = 0, i, f;
    GuiSaveNx save;
    SnxU64 title;
    list->at = NULL;
    list->count = 0;
    if (users && !snx_list_dir(users, &names, &n))
        for (i = 0; i < n; ++i)
            if (snx_title_parse(names[i], &title) && !nx_has(list, title) &&
                gui_saves_nx_find(root, folders, count, uid, global, title, &save))
                nx_push(list, &save);
    snx_free_strings(names, n);
    for (f = 0; f < count; ++f) {
        names = NULL;
        n = 0;
        if (snx_list_dir(folders[f], &names, &n)) continue;
        for (i = 0; i < n; ++i) {
            char* game = snx_path_join(folders[f], names[i]);
            char* own = snx_format("%s/%s_data/save/user.%s", game, names[i], uid);
            if (snx_title_parse(names[i], &title) && !nx_has(list, title) && own && snx_is_dir(own) &&
                gui_saves_nx_place(game, names[i], root ? root : "", uid, global, &save))
                nx_push(list, &save);
            free(own);
            free(game);
        }
        snx_free_strings(names, n);
    }
    if (list->count > 1) qsort(list->at, list->count, sizeof *list->at, by_save_title);
    free(users);
}

/* the n letters at name, a folder's name: a title ID in 16 hex digits, in either case (Eden writes capitals, and its
   import reads either), into *id */
static bool eden_title(const char* name, size_t n, SnxU64* id) {
    SnxU64 v = 0;
    if (n != 16) return false;
    for (size_t i = 0; i < n; ++i) {
        int c = name[i] | 0x20, d;
        if (name[i] >= '0' && name[i] <= '9')
            d = name[i] - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else
            return false;
        v = v << 4 | (SnxU64)d;
    }
    *id = v;
    return true;
}
/* member i is the Switch's in a zip whose saves of it are under prefix: one of another console's in a zip of every
   console's is not, nor the prefix's own folder */
static bool eden_ours(const SnxZip* zip, uint32_t i, const char* prefix) {
    const char* name = snx_zip_name(zip, i);
    size_t n = strlen(prefix);
    return !strncmp(name, prefix, n) && name[n];
}
/* member i's title ID, its folder's (eden_open checked that it has one) */
static SnxU64 eden_member_title(const SnxZip* zip, uint32_t i, const char* prefix) {
    SnxU64 id = 0;
    eden_title(snx_zip_name(zip, i) + strlen(prefix), 16, &id);
    return id;
}
/* the zip at path, every member of it under prefix a path in a folder named by a title ID, and one at least; NULL with
   why in error */
static SnxZip* eden_open(const char* path, const char* shown, const char* prefix, char* error, size_t size) {
    char why[1024];
    SnxZip* zip = snx_zip_open(path, why, sizeof why);
    uint32_t i, n, ours = 0;
    size_t skip = strlen(prefix);
    error[0] = 0;
    if (!zip) {
        snprintf(error, size, snx_tr("%s is no export of saves as Eden makes one: %s"), shown, why);
        return NULL;
    }
    n = snx_zip_count(zip);
    for (i = 0; i < n && !error[0]; ++i) {
        const char* name = snx_zip_name(zip, i);
        const char* slash;
        SnxU64 id;
        if (!snx_zip_safe_name(name))
            snprintf(error, size,
                     snx_tr("%s is no export of saves as Eden makes one: its member %s is no path inside a folder"),
                     shown, name);
        if (error[0] || !eden_ours(zip, i, prefix)) continue;
        ++ours;
        slash = strchr(name + skip, '/');
        if (!slash || !eden_title(name + skip, (size_t)(slash - name - skip), &id))
            snprintf(error, size,
                     snx_tr("%s is no export of saves as Eden makes one: %s is not in a folder named by a game's title "
                            "ID, 16 hex digits"),
                     shown, name);
    }
    if (!error[0] && !ours && !prefix[0])
        snprintf(error, size, snx_tr("%s is an empty zip: Eden's export of saves holds a folder for each game"), shown);
    else if (!error[0] && !ours)
        snprintf(error, size, snx_tr("%s holds no saves of the Switch: it has no %s folder"), shown, prefix);
    if (!error[0]) return zip;
    snx_zip_close(zip);
    return NULL;
}
static int by_title(const void* a, const void* b) {
    SnxU64 x = *(const SnxU64*)a, y = *(const SnxU64*)b;
    return x < y ? -1 : x > y;
}
/* the zip's title IDs under prefix, each once, in order, into *titles (owned) and *count */
static void eden_list(const SnxZip* zip, const char* prefix, SnxU64** titles, size_t* count) {
    uint32_t n = snx_zip_count(zip), i;
    size_t kept = 0, found = 0;
    *titles = (SnxU64*)snx_alloc((n ? n : 1) * sizeof **titles);
    for (i = 0; i < n; ++i)
        if (eden_ours(zip, i, prefix)) (*titles)[found++] = eden_member_title(zip, i, prefix);
    qsort(*titles, found, sizeof **titles, by_title);
    for (i = 0; i < found; ++i)
        if (!kept || (*titles)[kept - 1] != (*titles)[i]) (*titles)[kept++] = (*titles)[i];
    *count = kept;
}
int gui_saves_eden_titles(const char* path, const char* shown, const char* prefix, SnxU64** titles, size_t* count,
                          char* error, size_t size) {
    SnxZip* zip = eden_open(path, shown, prefix, error, size);
    *titles = NULL;
    *count = 0;
    if (!zip) return 1;
    eden_list(zip, prefix, titles, count);
    snx_zip_close(zip);
    return 0;
}
/* the title IDs of titles past at most four of them, "A, B, C, D and 3 more", into out */
static void titles_text(const SnxU64* titles, size_t count, char* out, size_t size) {
    size_t n = 0, i;
    out[0] = 0;
    for (i = 0; i < count && i < 4 && n < size; ++i)
        n += (size_t)snprintf(out + n, size - n, "%s%016llX", i ? ", " : "", (unsigned long long)titles[i]);
    if (i < count && n < size) snprintf(out + n, size - n, snx_tr(" and %zu more"), count - i);
}
/* save's files out of the zip, title's folder under prefix, put together beside its folder and then in its place */
static int eden_import_one(GuiCleaned* done, SnxZip* zip, const char* path, const char* prefix, const GuiSaveNx* save,
                           GuiSavesDone* result, char* error, size_t size) {
    char *parent = snx_dirname(save->folder), *hidden = snx_format(".%s.importing", snx_path_leaf(save->folder));
    char* staged = parent && hidden ? snx_path_join(parent, hidden) : NULL;
    char* into = staged ? snx_path_join(staged, SNX_SAVE_COMMITTED) : NULL;
    char* mark = staged ? snx_path_join(staged, SNX_SAVE_JOURNALED) : NULL;
    char *last = snx_path_join(save->folder, SNX_SAVE_COMMITTED), backup[4400], what[4400], why[1024];
    uint32_t n = snx_zip_count(zip), i;
    size_t files = 0, skip = strlen(prefix);
    SnxU64 bytes = 0;
    int rc = 1;
    error[0] = 0;
    if (!mark || !last) {
        snprintf(error, size, "%s", snx_tr("out of memory"));
        goto out;
    }
    if (snx_path_exists(staged)) {
        if (snx_remove_tree(staged)) {
            snprintf(error, size, snx_tr("%s, left by an import cut short, could not be removed: %s"), staged,
                     snx_os_error(why, sizeof why));
            goto out;
        }
        gui_cleaned_add(done, true, staged, snx_tr("Removed: an import of this save cut short left it"));
    }
    /* the file that says the folder holds a last commit, first: the folders alone say nothing (fs.c's save_open) */
    if (snx_mkdirs(staged) || snx_write_file(mark, "", 0) || snx_mkdirs(into)) {
        snprintf(error, size, snx_tr("%s could not be made: %s"), staged, snx_os_error(why, sizeof why));
        goto out;
    }
    for (i = 0; i < n && !error[0]; ++i) {
        const char* name = snx_zip_name(zip, i);
        const char* rest = name + skip + 17; /* past the prefix, the title ID and its slash */
        size_t length;
        bool folder_entry;
        char *inner, *to, *shown;
        if (!eden_ours(zip, i, prefix) || eden_member_title(zip, i, prefix) != save->title) continue;
        length = strlen(rest);
        if (!length) continue; /* the title ID's folder itself */
        if (!strcmp(name + skip + 16, EDEN_SIZE_FILE)) {
            gui_cleaned_add(done, true, name,
                            snx_tr("Left out: Eden's record of the save's size, which the game never sees, of which "
                                   "this version keeps no file"));
            continue;
        }
        folder_entry = rest[length - 1] == '/';
        inner = snx_format("%.*s", (int)(length - folder_entry), rest);
        to = inner ? snx_path_join(into, inner) : NULL;
        shown = inner ? snx_path_join(last, inner) : NULL;
        free(inner);
        if (!to || !shown)
            snprintf(error, size, "%s", snx_tr("out of memory"));
        else if (folder_entry) { /* a folder of its own: one with nothing in it is the save's too */
            if (snx_mkdirs(to))
                snprintf(error, size, snx_tr("%s could not be made: %s"), to, snx_os_error(why, sizeof why));
        } else {
            char* folder = snx_dirname(to);
            if (!folder || snx_mkdirs(folder))
                snprintf(error, size, snx_tr("%s could not be made: %s"), folder ? folder : to,
                         snx_os_error(why, sizeof why));
            else if (!snx_zip_extract(zip, i, to, why, sizeof why))
                snprintf(error, size, snx_tr("%s could not be taken out of %s: %s"), name, path, why);
            else {
                ++files;
                bytes += snx_zip_size(zip, i);
                snprintf(what, sizeof what, snx_tr("Imported from %s in %s"), name, path);
                gui_cleaned_add(done, true, shown, what);
            }
            free(folder);
        }
        free(shown);
        free(to);
    }
    if (error[0]) {
        snx_remove_tree(staged);
        goto out;
    }
    if (gui_saves_put(done, staged, save->folder, save->trash, save->name, backup, sizeof backup, error, size))
        goto out;
    ++result->saves;
    result->files += files;
    result->bytes += bytes;
    if (backup[0] && !result->backups++) snprintf(result->backup, sizeof result->backup, "%s", backup);
    if (backup[0])
        snprintf(what, sizeof what,
                 snx_tr("Imported %zu files (%llu bytes) of %016llX from %s; the save there before is in %s"), files,
                 (unsigned long long)bytes, (unsigned long long)save->title, path, backup);
    else
        snprintf(what, sizeof what,
                 snx_tr("Imported %zu files (%llu bytes) of %016llX from %s; there was no save before"), files,
                 (unsigned long long)bytes, (unsigned long long)save->title, path);
    gui_cleaned_add(done, true, save->folder, what);
    rc = 0;
out:
    if (rc) {
        snprintf(what, sizeof what, snx_tr("Not imported: %s"), error);
        gui_cleaned_add(done, false, save->folder, what);
    }
    free(last);
    free(mark);
    free(into);
    free(staged);
    free(hidden);
    free(parent);
    return rc;
}
int gui_saves_eden_import(GuiCleaned* done, const char* path, const char* shown, const char* prefix,
                          const GuiSaveNx* saves, size_t count, GuiSavesDone* result, char* error, size_t size) {
    SnxZip* zip = eden_open(path, shown, prefix, error, size);
    SnxU64* titles = NULL;
    size_t found = 0, i, j, other = 0;
    char listed[256], what[4400];
    int rc = 1;
    if (!zip) return 1;
    eden_list(zip, prefix, &titles, &found);
    for (i = 0; i < count; ++i) {
        SnxU64 title = saves[i].title;
        if (!bsearch(&title, titles, found, sizeof *titles, by_title)) {
            titles_text(titles, found, listed, sizeof listed);
            snprintf(error, size, snx_tr("%s holds the saves of %s, not of %016llX"), shown, listed,
                     (unsigned long long)title);
            goto out;
        }
    }
    for (i = 0; i < found; ++i) { /* the zip's games not asked for, at titles' front */
        for (j = 0; j < count && saves[j].title != titles[i]; ++j) {}
        if (j == count) titles[other++] = titles[i];
    }
    for (i = 0; i < count; ++i)
        if (eden_import_one(done, zip, shown, prefix, &saves[i], result, error, size)) goto out;
    if (other) {
        titles_text(titles, other, listed, sizeof listed);
        snprintf(what, sizeof what, snx_tr("Not imported: it holds the saves of %zu other games too, %s"), other,
                 listed);
        gui_cleaned_add(done, false, shown, what);
    }
    rc = 0;
out:
    free(titles);
    snx_zip_close(zip);
    return rc;
}
/* where save's files are taken from: its last commit, else its working copy after a commit cut short, else the folder
   itself for a save as the versions before kept one (*flat); NULL, said in an entry, for none */
static char* eden_source(GuiCleaned* done, const GuiSaveNx* save, bool* flat) {
    char *mark = snx_path_join(save->folder, SNX_SAVE_JOURNALED),
         *last = snx_path_join(save->folder, SNX_SAVE_COMMITTED);
    char* work = snx_path_join(save->folder, SNX_SAVE_WORKING);
    char* from = NULL;
    *flat = false;
    if (!snx_is_dir(save->folder))
        gui_cleaned_add(done, false, save->folder, snx_tr("Not exported: no save there"));
    else if (!snx_is_file(mark)) {
        *flat = true;
        from = snx_strdup(save->folder);
    } else if (snx_is_dir(last)) {
        from = last;
        last = NULL;
    } else if (snx_is_dir(work)) {
        gui_cleaned_add(done, true, save->folder,
                        snx_tr("Exported from its working copy (1): it has no last commit (0), a commit cut short that "
                               "the game's next start finishes from that copy"));
        from = work;
        work = NULL;
    } else
        gui_cleaned_add(done, false, save->folder,
                        snx_tr("Not exported: it holds neither a last commit (0) nor a working copy (1)"));
    free(work);
    free(last);
    free(mark);
    return from;
}
int gui_saves_eden_add(GuiSavesZip* zip, const char* prefix, const GuiSaveNx* saves, size_t count, size_t* kept,
                       char* error, size_t size) {
    char what[4400];
    size_t i;
    *kept = 0;
    for (i = 0; i < count; ++i) {
        bool flat;
        char *from = eden_source(zip->done, &saves[i], &flat), *folder;
        int rc;
        if (from && !files_in(from, flat)) {
            snprintf(what, sizeof what, snx_tr("Not exported: %s holds no file"), from);
            gui_cleaned_add(zip->done, false, saves[i].folder, what);
            free(from);
            from = NULL;
        }
        if (!from) continue;
        folder = snx_format("%s%016llX/", prefix, (unsigned long long)saves[i].title);
        rc = zip_tree(zip, from, folder, flat, error, size);
        free(folder);
        free(from);
        if (rc) return 1;
        ++*kept;
    }
    return 0;
}
int gui_saves_eden_export(GuiCleaned* done, const char* path, const GuiSaveNx* saves, size_t count, char* error,
                          size_t size) {
    GuiSavesZip* zip = gui_saves_zip_begin(path, done, error, size);
    size_t kept = 0, files = 0;
    SnxU64 bytes = 0;
    char what[4400];
    int failed;
    if (!zip) return 1;
    failed = gui_saves_eden_add(zip, "", saves, count, &kept, error, size);
    if (!failed && !kept) {
        if (count == 1)
            snprintf(error, size, snx_tr("%016llX has no save in %s"), (unsigned long long)saves[0].title,
                     saves[0].folder);
        else
            snprintf(error, size, snx_tr("None of the %zu games has a save to export"), count);
    }
    if (gui_saves_zip_end(zip, !failed && kept, &files, &bytes, error, size)) return 1;
    snprintf(what, sizeof what, snx_tr("Exported %zu files (%llu bytes) from %zu of %zu saves"), files,
             (unsigned long long)bytes, kept, count);
    gui_cleaned_add(done, true, path, what);
    return 0;
}
