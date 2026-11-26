/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "vita/loader/dump.h"
#include "vita/loader/keys.h"
#include "vita/loader/license.h"
#include "vita/loader/pfs.h"
#include "vita/loader/selfcrypt.h"
#include "common/platform/file.h"
#include "common/utilities/common.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

[[gnu::format(printf, 3, 4)]] static void say(const VitaDumpImport* d, bool error, const char* fmt, ...) {
    char line[SNX_PATH_SIZE + 512];
    va_list list;
    va_start(list, fmt);
    vsnprintf(line, sizeof line, fmt, list);
    va_end(list);
    if (d->say) d->say(d->context, line, error);
}
bool vita_dump_encrypted(const char* app) { return vita_pfs_present(app); }

/* the bytes of the files under path, its sce_pfs/ and its top sce_sys/package/ left out (top: path is a content's
   folder; system: path is its sce_sys/) */
static uint64_t content_bytes(const char* path, bool top, bool system) {
    char** names = NULL;
    size_t count = 0;
    uint64_t total = 0;
    if (snx_list_dir(path, &names, &count)) return 0;
    for (size_t i = 0; i < count; ++i) {
        if (!strcmp(names[i], "sce_pfs") || (system && !strcmp(names[i], "package"))) continue;
        char* entry = snx_path_join(path, names[i]);
        if (entry && snx_is_dir(entry))
            total += content_bytes(entry, false, top && !strcmp(names[i], "sce_sys"));
        else if (entry)
            total += snx_file_size(entry);
        free(entry);
    }
    snx_free_strings(names, count);
    return total;
}
uint64_t vita_dump_bytes(const char* app, const char* update, const char* title, unsigned* addons) {
    char *full = snx_absolute(app), *parent = full ? snx_dirname(full) : NULL,
         *root = parent ? snx_dirname(parent) : NULL;
    char addcont[SNX_PATH_SIZE], **names = NULL;
    size_t count = 0;
    uint64_t total = content_bytes(app, true, false) + (update ? content_bytes(update, true, false) : 0);
    *addons = 0;
    if (root && !snx_path_format(addcont, sizeof addcont, "%s/addcont/%s", root, title) &&
        !snx_list_dir(addcont, &names, &count))
        for (size_t i = 0; i < count; ++i) {
            char* source = snx_path_join(addcont, names[i]);
            if (source && vita_pfs_present(source)) total += content_bytes(source, true, false), ++*addons;
            free(source);
        }
    snx_free_strings(names, count);
    free(root);
    free(parent);
    free(full);
    return total;
}

/* the content's CONTENT_ID from its sce_sys/param.sfo, owned; NULL when it has none */
static char* content_id(const char* folder) {
    char path[SNX_PATH_SIZE];
    size_t size = 0;
    unsigned char* sfo =
        snx_path_format(path, sizeof path, "%s/sce_sys/param.sfo", folder) ? NULL : snx_read_file(path, &size);
    char* id = sfo ? snx_sfo_string(sfo, size, "CONTENT_ID") : NULL;
    free(sfo);
    return id;
}
/* the license in dir, a folder of .rif files: its one, or of several the one for content (its CONTENT_ID). 1 with
   *license and its path, 0 when dir has none, -1 with error */
static int license_in(const char* dir, const char* content, VitaLicense* license, char* path, size_t path_size,
                      char* error, size_t error_size) {
    char** names = NULL;
    size_t count = 0, rifs = 0;
    int found = 0;
    if (!snx_is_dir(dir) || snx_list_dir(dir, &names, &count)) return 0;
    for (size_t i = 0; i < count && found >= 0; ++i) {
        size_t n = strlen(names[i]);
        if (n < 5 || strcmp(names[i] + n - 4, ".rif")) continue;
        VitaLicense candidate;
        char at[SNX_PATH_SIZE];
        ++rifs;
        if (snx_path_format(at, sizeof at, "%s/%s", dir, names[i]) ||
            !vita_license_read(at, &candidate, error, error_size)) {
            found = -1;
            break;
        }
        if (rifs == 1 || (content && !strcmp(candidate.content_id, content))) {
            *license = candidate;
            snprintf(path, path_size, "%s", at);
            found = 1;
        }
    }
    snx_free_strings(names, count);
    if (found > 0 && rifs > 1 && (!content || strcmp(license->content_id, content))) {
        snprintf(error, error_size, "%s has %zu licenses and none is for %s", dir, rifs,
                 content ? content : "the content");
        found = -1;
    }
    return found;
}

typedef struct {
    const VitaDumpImport* d;
    const char* what;
} Note;
static void left_out(void* context, const char* path) {
    const Note* n = context;
    say(n->d, false, "%s/%s is not in its files.db, and the import leaves it out", n->what, path);
}
static void went(void* context, uint64_t done, uint64_t total) {
    const Note* n = context;
    if (n->d->progress) n->d->progress(n->d->context, done, total);
}
/* every SELF under rel in root decrypted in place, named in the messages by its path in source: false with error */
static bool decrypt_selfs(const char* root, const char* rel, const char* source, const unsigned char klicensee[16],
                          const VitaKeys* keys, unsigned* done, char* names, size_t names_size, char* error,
                          size_t error_size) {
    char here[SNX_PATH_SIZE], **list = NULL;
    size_t count = 0;
    bool ok = true;
    if (snx_path_format(here, sizeof here, "%s%s%s", root, rel[0] ? "/" : "", rel) || snx_list_dir(here, &list, &count))
        return true;
    for (size_t i = 0; ok && i < count; ++i) {
        char inner[SNX_PATH_SIZE], full[SNX_PATH_SIZE];
        /* full, longer than inner, says when it is too long; inner, the path the messages name, keeps / as files.db's */
        if (snx_path_format(full, sizeof full, "%s/%s", here, list[i])) continue;
        snprintf(inner, sizeof inner, "%s%s%s", rel, rel[0] ? "/" : "", list[i]);
        if (snx_is_dir(full)) {
            ok = decrypt_selfs(root, inner, source, klicensee, keys, done, names, names_size, error, error_size);
            continue;
        }
        if (!vita_self_name(list[i])) continue;
        char shown[SNX_PATH_SIZE];
        snprintf(shown, sizeof shown, "%s of %s", inner, source);
        int r = vita_self_decrypt(full, shown, klicensee, keys, error, error_size);
        if (r < 0) ok = false;
        if (r > 0) {
            size_t at = strlen(names);
            snprintf(names + at, names_size - at, "%s%s", at ? ", " : "", inner);
            ++*done;
        }
    }
    snx_free_strings(list, count);
    return ok;
}
/* source decrypted into place/leaf through place/.leaf.decrypting, its SELFs too when selfs, then renamed into place */
static bool decrypt_content(const VitaDumpImport* d, const VitaKeys* keys, const char* source, const char* place,
                            const char* leaf, const VitaLicense* license, bool selfs, char* error, size_t error_size) {
    char target[SNX_PATH_SIZE], stage[SNX_PATH_SIZE], decrypted[1024] = "";
    if (snx_path_format(target, sizeof target, "%s/%s", place, leaf) ||
        snx_path_format(stage, sizeof stage, "%s/.%s.decrypting", place, leaf)) {
        snprintf(error, error_size, "%s/%s is too long a path", place, leaf);
        return false;
    }
    if (snx_mkdirs(place)) {
        snprintf(error, error_size, "cannot make %s", place);
        return false;
    }
    if (snx_path_exists(stage) && snx_remove_tree(stage)) {
        snprintf(error, error_size, "cannot remove %s, an import's folder an earlier one left", stage);
        return false;
    }
    Note note = {d, source};
    VitaPfsCount count;
    unsigned selfs_done = 0;
    bool ok =
        vita_pfs_decrypt(source, stage, license->klicensee, keys, left_out, went, &note, &count, error, error_size) &&
        (!selfs || decrypt_selfs(stage, "", source, license->klicensee, keys, &selfs_done, decrypted, sizeof decrypted,
                                 error, error_size));
    if (ok && snx_path_exists(target) && snx_remove_tree(target)) {
        snprintf(error, error_size, "cannot remove %s, which the import replaces", target);
        ok = false;
    }
    if (ok && plat_replace_file(stage, target)) {
        char why[256];
        snprintf(error, error_size, "cannot move %s to %s: %s", stage, target, snx_os_error(why, sizeof why));
        ok = false;
    }
    if (!ok) {
        if (snx_path_exists(stage)) snx_remove_tree(stage);
        return false;
    }
    say(d, false,
        "%s: %u files decrypted, %u plain files copied, %u empty, %u folders (%.1f MiB), every sector checked%s%s%s",
        target, count.decrypted, count.copied, count.empty, count.folders, (double)count.bytes / (1024.0 * 1024.0),
        selfs_done ? "; SELFs decrypted: " : "", decrypted,
        count.left_out ? "; some of the dump's files left out" : "");
    return true;
}
static void keys_note(void* context, const char* line) { say(context, false, "%s", line); }
static bool kept(const char* target) { return snx_is_dir(target) && !snx_is_link(target) && !vita_pfs_present(target); }

bool vita_dump_import(const VitaDumpImport* d, char* error, size_t error_size) {
    VitaKeys keys;
    VitaLicense license;
    char *full = snx_absolute(d->app), *parent = full ? snx_dirname(full) : NULL,
         *root = parent ? snx_dirname(parent) : NULL;
    char *game_id = content_id(d->app), path[SNX_PATH_SIZE], from[SNX_PATH_SIZE] = "", place[SNX_PATH_SIZE];
    char why[1024] = "";
    bool ok = full && root;
    error[0] = 0;
    /* the first line, before anything can stop it: what a launcher's queue shows the import's progress and end under */
    say(d, false, "Decrypting %s into %s, the keys from %s", d->app, d->folder, d->keys ? d->keys : "~/.vita");
    if (!ok) snprintf(error, error_size, "cannot resolve %s", d->app);
    if (ok && !vita_keys_load(d->keys, &keys, keys_note, (void*)d, why, sizeof why)) {
        snprintf(error, error_size, "%s is encrypted, and %s", d->app, why);
        ok = false;
    }
    /* the game's license: its work.bin, the dump's license/app/<title ID>/, else the zRIF given */
    int found = 0;
    if (ok && !snx_path_format(path, sizeof path, "%s/sce_sys/package/work.bin", d->app) && snx_is_file(path)) {
        found = vita_license_read(path, &license, why, sizeof why) ? 1 : -1;
        snprintf(from, sizeof from, "%s", path);
    } else if (ok) {
        char dir[SNX_PATH_SIZE];
        found = snx_path_format(dir, sizeof dir, "%s/license/app/%s", root, d->title)
                    ? 0
                    : license_in(dir, game_id, &license, from, sizeof from, why, sizeof why);
    }
    if (ok && found > 0 && d->zrif)
        say(d, false, "--zrif is not read: the dump has the game's license, %s (%s)", from, license.content_id);
    if (ok && !found && d->zrif) {
        found = vita_license_zrif(d->zrif, &license, why, sizeof why) ? 1 : -1;
        snprintf(from, sizeof from, "the zRIF given");
    }
    if (ok && !found)
        snprintf(why, sizeof why,
                 "it has no license: no %s/sce_sys/package/work.bin, no %s/license/app/%s/*.rif and no --zrif", d->app,
                 root, d->title);
    if (ok) ok = found > 0;
    if (!ok && !error[0]) snprintf(error, error_size, "%s is encrypted, and %s", d->app, why);
    if (ok && game_id && strcmp(game_id, license.content_id))
        say(d, false, "%s's license is for %s, and its param.sfo says %s", d->app, license.content_id, game_id);
    /* the game */
    if (ok && snx_path_format(place, sizeof place, "%s/app", d->folder)) {
        snprintf(error, error_size, "%s/app is too long a path", d->folder);
        ok = false;
    }
    if (ok && kept(place)) {
        say(d, false, "Using the decryption in %s", place);
    } else if (ok) {
        say(d, false, "Decrypting %s into %s, with the license %s (%s)", d->app, place, from, license.content_id);
        ok = decrypt_content(d, &keys, d->app, d->folder, "app", &license, true, why, sizeof why);
        if (!ok) snprintf(error, error_size, "%s", why);
    }
    /* its update, with the game's license */
    if (ok && d->patch && vita_pfs_present(d->patch)) {
        if (snx_path_format(place, sizeof place, "%s/patch", d->folder)) {
            snprintf(error, error_size, "%s/patch is too long a path", d->folder);
            ok = false;
        } else if (kept(place)) {
            say(d, false, "Using the decryption in %s", place);
        } else {
            say(d, false, "Decrypting the update %s into %s, with the game's license", d->patch, place);
            ok = decrypt_content(d, &keys, d->patch, d->folder, "patch", &license, true, why, sizeof why);
            if (!ok) snprintf(error, error_size, "%s", why);
        }
    }
    /* each add-on, with its own: one it cannot is said, and the game plays without it */
    char addcont[SNX_PATH_SIZE], **names = NULL;
    size_t count = 0;
    if (ok && !snx_path_format(addcont, sizeof addcont, "%s/addcont/%s", root, d->title) && snx_is_dir(addcont) &&
        !snx_list_dir(addcont, &names, &count)) {
        unsigned done = 0, wanted = 0;
        for (size_t i = 0; i < count; ++i) {
            char source[SNX_PATH_SIZE], dir[SNX_PATH_SIZE], target[SNX_PATH_SIZE], rif[SNX_PATH_SIZE];
            if (snx_path_format(source, sizeof source, "%s/%s", addcont, names[i]) || !vita_pfs_present(source))
                continue;
            ++wanted;
            if (snx_path_format(place, sizeof place, "%s/ux0/addcont/%s", d->folder, d->title) ||
                snx_path_format(target, sizeof target, "%s/%s", place, names[i]) ||
                snx_path_format(dir, sizeof dir, "%s/license/addcont/%s/%s", root, d->title, names[i]))
                continue;
            if (kept(target)) {
                say(d, false, "Using the add-on in %s", target);
                ++done;
                continue;
            }
            char* id = content_id(source);
            VitaLicense own;
            int found = license_in(dir, id, &own, rif, sizeof rif, why, sizeof why);
            if (found > 0) {
                say(d, false, "Decrypting the add-on %s into %s, with the license %s (%s)", source, target, rif,
                    own.content_id);
                found = decrypt_content(d, &keys, source, place, names[i], &own, false, why, sizeof why) ? 1 : -1;
            } else if (!found)
                snprintf(why, sizeof why, "it has no license in %s", dir);
            if (found > 0)
                ++done;
            else
                say(d, true, "the add-on %s is not imported: %s", source, why);
            free(id);
        }
        if (wanted)
            say(d, false, "%u of the dump's %u add-ons are in %s/ux0/addcont/%s", done, wanted, d->folder, d->title);
    }
    snx_free_strings(names, count);
    free(game_id);
    free(root);
    free(parent);
    free(full);
    return ok;
}
