/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/setup.h"
#include "common/utilities/common.h"
#include "common/utilities/ini_lines.h"
#include "common/utilities/users.h"
#include "common/utilities/ascii.h"
#include "common/utilities/endian.h"
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#define SETUP_FILE     "setup.ini"
#define SETUP_KEYS_MAX (1 << 20) /* a prod.keys is a few kilobytes, a title.keys some more: past this it is not one */

const int snx_setup_since[SNX_SETUP_STEPS] = {1, 1, 1, 1, 1};
const SnxNotice snx_notices[SNX_NOTICES] = {
    {SNX_TR_NOOP("Game settings"), SNX_TR_NOOP("removed or renamed keys in a game's .ini"),
     SNX_TR_NOOP("under [Deprecated]: Clean Old Settings takes them"), "`.ini` key", SNX_NOTICE_ALL},
    {SNX_TR_NOOP("An option"), "--keep-c", "--keep-ir", "`--keep-c`", SNX_NOTICE_ALL},
    {SNX_TR_NOOP("An installed copy's files"), "~/.local/share/scarletnx: gui/, games/<Game>.ini, work/",
     SNX_TR_NOOP("the scarletnx folders of ~/.config, ~/.local/state and ~/.cache"), "An installed copy's files",
     SNX_NOTICE_LINUX},
    {SNX_TR_NOOP("Games"), SNX_TR_NOOP("games/<Game>, <Game>_data/ side by side"),
     SNX_TR_NOOP("games/<id>/, a folder each"), "A game laid out beside the others", SNX_NOTICE_ALL},
    {SNX_TR_NOOP("The launcher's files"), SNX_TR_NOOP("gui/ in the data folder"),
     SNX_TR_NOOP("the config and state folders"), "The menu's `gui/`", SNX_NOTICE_ALL},
    {SNX_TR_NOOP("A portable copy"), SNX_TR_NOOP("games/ beside the program"),
     SNX_TR_NOOP(".scarletnx/ beside the program"), "A portable copy's `games/`", SNX_NOTICE_ALL},
    {SNX_TR_NOOP("The launcher's files"), SNX_TR_NOOP("the app's own files"),
     SNX_TR_NOOP("the data folder's config/ and state/"), "Android: the menu's", SNX_NOTICE_ANDROID},
    {SNX_TR_NOOP("A build tree's files"), SNX_TR_NOOP("games/, work/ and the keys in its checkout"),
     SNX_TR_NOOP("the user's data folder"), "A build tree's checkout", SNX_NOTICE_ALL},
    {SNX_TR_NOOP("Account saves"), "<id>_data/save/user", "<id>_data/save/user.<uid>", "Account saves in",
     SNX_NOTICE_ALL},
    {SNX_TR_NOOP("Recompiled games"), SNX_TR_NOOP("games/ in the data folder"),
     SNX_TR_NOOP("recompiled/, and dumps/ for the dumps"), "The data folder's `games/`", SNX_NOTICE_ALL},
    {SNX_TR_NOOP("The launcher's folders of games"), SNX_TR_NOOP("a line of folders.txt, a path alone"),
     SNX_TR_NOOP("each folder in the recompiled or the dumps library, as its games say"), "A line of `folders.txt`",
     SNX_NOTICE_ALL},
    {SNX_TR_NOOP("The launcher's Text size"), SNX_TR_NOOP("text_size = in app.ini"),
     SNX_TR_NOOP("ui_scale =, shown as UI scale"), "`text_size =` in `app.ini`", SNX_NOTICE_ALL},
    {SNX_TR_NOOP("A theme's lists"), SNX_TR_NOOP("[body] gap and scroll in its theme.ini"),
     SNX_TR_NOOP("nothing: ui.txt lays out every list"), "A theme's `[body] gap` and `[body] scroll`", SNX_NOTICE_ALL},
    {SNX_TR_NOOP("A theme's screens"), SNX_TR_NOOP("[head] place, [tips] place and [body] sizing in its theme.ini"),
     SNX_TR_NOOP("nothing: ui.txt lays out every screen"), "A theme's `[head] place`", SNX_NOTICE_ALL},
    {SNX_TR_NOOP("Options"), "--recomp, --repo, --debug, -r --out, --interp-only",
     "--recompile, --data, --no-setup, -r --name, --jit-only", "[cli] the options' names", SNX_NOTICE_ALL},
    {SNX_TR_NOOP("SLP vectorizer"), SNX_TR_NOOP("LLVM's, on in the recompile and the JIT"),
     SNX_TR_NOOP("off, as LLVM 23 can break a module with it: Settings > Debug turns it on"), "LLVM's SLP vectorizer",
     SNX_NOTICE_ALL},
    {SNX_TR_NOOP("The launcher's UI scale"), SNX_TR_NOOP("Auto, and 0.25, 0.5 and 0.75 of it"),
     SNX_TR_NOOP("1x to 4x: the number Auto gave that screen"), "`ui_scale = auto`", SNX_NOTICE_ALL},
    {SNX_TR_NOOP("ASTC as BC7 and Memory fit"), SNX_TR_NOOP("0 was Off"),
     SNX_TR_NOOP("0 is Auto, the default; Off is 2"), "`astc_bc = 0` and `memory_fit = 0`", SNX_NOTICE_ALL}};
int snx_notice_here(int notice) {
    int systems = snx_notices[notice].systems;
#if defined(__ANDROID__)
    return systems == SNX_NOTICE_ALL || systems == SNX_NOTICE_ANDROID;
#elif defined(SNX_XDG)
    return systems == SNX_NOTICE_ALL || systems == SNX_NOTICE_LINUX;
#else
    return systems == SNX_NOTICE_ALL;
#endif
}

char* snx_setup_file(const char* data) {
    char *config = snx_menu_dir(data, SNX_USER_CONFIG), *path = config ? snx_path_join(config, SETUP_FILE) : NULL;
    free(config);
    return path;
}
/* a whole number from 0 to 1000000, else -1 */
static long setup_number(const char* value) {
    char* end;
    long n = strtol(value, &end, 10);
    return *value >= '0' && *value <= '9' && !*end && n <= 1000000 ? n : -1;
}
int snx_setup_read(const char* path, SnxSetup* setup, char* why, size_t why_size) {
    size_t size, n = 0;
    char *key, *value, unknown[256];
    long number;
    int rc = 0;
    SnxIni ini;
    setup->version = setup->notices = 0;
    if (!snx_is_file(path)) return 0;
    char* text = (char*)snx_read_file(path, &size);
    if (!text) {
        snprintf(why, why_size, "%s could not be read", path);
        return -1;
    }
    unknown[0] = 0;
    snx_ini_start(&ini, text);
    while (snx_ini_next(&ini, &key, &value)) {
        int version = !strcmp(key, "version");
        if (strcmp(ini.section, "setup") || (!version && strcmp(key, "notices"))) {
            if (n < sizeof unknown)
                n += (size_t)snprintf(unknown + n, sizeof unknown - n, "%s[%s] %s", n ? ", " : "", ini.section, key);
            continue;
        }
        number = setup_number(value);
        if (number < 0) {
            snprintf(why, why_size, "%s: %s = %s is not a whole number", path, key, value);
            rc = -1;
            continue;
        }
        *(version ? &setup->version : &setup->notices) = (int)number;
    }
    /* a newer version's file may hold what this one does not know */
    if (unknown[0] && setup->version <= SNX_SETUP_VERSION && !rc) {
        snprintf(why, why_size, "%s: %s is not the setup's: version, notices", path, unknown);
        rc = -1;
    }
    free(text);
    return rc;
}
int snx_setup_done(const char* path, const SnxSetup* setup, char* why, size_t why_size) {
    char *dir = snx_dirname(path), *staged = snx_format("%s.new", path), *text;
    text = snx_format("; ScarletNX's setup (docs/USAGE.md#setup): the version of the steps it asked and the notices it "
                      "showed.\n; Delete this file, or run scarletnx --setup, to go through it again\n"
                      "[setup]\nversion = %d\nnotices = %d\n",
                      setup->version > SNX_SETUP_VERSION ? setup->version : SNX_SETUP_VERSION,
                      setup->notices > SNX_NOTICES ? setup->notices : SNX_NOTICES);
    int failed = !dir || snx_mkdirs(dir) || snx_write_file(staged, text, strlen(text)) || snx_replace(staged, path);
    if (failed) {
        remove(staged);
        snprintf(why, why_size, "%s could not be written", path);
    }
    free(text);
    free(staged);
    free(dir);
    return failed ? -1 : 0;
}
int snx_setup_asks(const SnxSetup* setup, int step) { return snx_setup_since[step] > setup->version; }
int snx_setup_here(int step) {
#if defined(__ANDROID__) || defined(SNX_IOS)
    return step != SNX_SETUP_INSTALL;
#else
    (void)step;
    return 1;
#endif
}
int snx_setup_shows(const SnxSetup* setup, int notice) { return notice >= setup->notices && snx_notice_here(notice); }
int snx_setup_due(const SnxSetup* setup) {
    for (int i = 0; i < SNX_SETUP_STEPS; ++i)
        if (snx_setup_asks(setup, i)) return 1;
    for (int i = 0; i < SNX_NOTICES; ++i)
        if (snx_setup_shows(setup, i)) return 1;
    return 0;
}

int snx_setup_skip, snx_setup_again;
static struct {
    const char *prod_keys_override, *title_keys_override;
} g_setup;
SnxSetupKeysOverride snx_setup_keys_override_get(void) {
    SnxSetupKeysOverride current = {g_setup.prod_keys_override, g_setup.title_keys_override};
    return current;
}
void snx_setup_keys_override_set(const char* prod, const char* title) {
    g_setup.prod_keys_override = prod;
    g_setup.title_keys_override = title;
}
int snx_setup_options(int argc, char** argv, char* why, size_t why_size) {
    int n = 1, skips = 0, debug = 0;
    snx_setup_skip = snx_setup_again = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--")) { /* the game's own arguments, as they are */
            while (i < argc) argv[n++] = argv[i++];
            break;
        }
        if (!strcmp(argv[i], "--no-setup"))
            debug = 1;
        else if (!strcmp(argv[i], "--debug")) {
            snx_option_renamed("--debug", "--no-setup");
            debug = 1;
        } else if (!strcmp(argv[i], "--skip"))
            ++skips;
        else if (!strcmp(argv[i], "--setup"))
            snx_setup_again = 1;
        else
            argv[n++] = argv[i];
    }
    argv[n] = NULL;
    if (skips == 1) {
        snprintf(why, why_size, "--skip leaves the setup out given twice, --skip --skip, which is --no-setup now");
        return -1;
    }
    if (skips) snx_option_renamed("--skip --skip", "--no-setup");
    snx_setup_skip = debug || skips;
    if (snx_setup_skip && snx_setup_again) {
        snprintf(why, why_size, "--setup goes through the setup, and %s leaves it out: one or the other",
                 debug ? "--no-setup" : "--skip --skip");
        return -1;
    }
    return n;
}

int snx_setup_uid(uint64_t uid[2]) {
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof bytes) != 1) return -1;
    uid[0] = snx_ld_be64(bytes);
    uid[1] = snx_ld_be64(bytes + 8);
    /* the text is uid[0], then uid[1], in hex: its 13th digit (bits 12 to 15 of uid[0]) a UUID's version, 4 for one
       made of random bits, and the 17th (the top of uid[1]) its variant, RFC 4122's 10 in the two high bits */
    uid[0] = (uid[0] & ~(uint64_t)0xF000) | 0x4000;
    uid[1] = (uid[1] & ~((uint64_t)3 << 62)) | ((uint64_t)2 << 62);
    return 0;
}
int snx_setup_first_saves(const char* games) {
    static const uint64_t first[2] = {SNX_USERS_FIRST_UID0, SNX_USERS_FIRST_UID1};
    char **names = NULL, text[33];
    size_t count = 0;
    int found = 0;
    if (!games || snx_list_dir(games, &names, &count)) return 0;
    snx_users_uid_text(first, text);
    for (size_t i = 0; i < count; ++i) {
        char save[SNX_PATH_SIZE];
        found += !snx_path_format(save, sizeof save, "%s/%s/%s_data/save/user.%s", games, names[i], names[i], text) &&
                 snx_is_dir(save);
    }
    snx_free_strings(names, count);
    return found;
}

int snx_setup_first_uid(const char* games, uint64_t uid[2]) {
    int saves = snx_setup_first_saves(games);
    if (saves) { /* they stay its own */
        uid[0] = SNX_USERS_FIRST_UID0;
        uid[1] = SNX_USERS_FIRST_UID1;
        return saves;
    }
    return snx_setup_uid(uid);
}

int snx_keys_text(const char* data, size_t size) {
    size_t at = 0;
    int keys = 0;
    if (!data || memchr(data, 0, size)) return 0;
    while (at < size) {
        const char *line = data + at, *end = (const char*)memchr(line, '\n', size - at);
        size_t n = end ? (size_t)(end - line) : size - at, i = 0, name, hex = 0;
        while (i < n && (line[i] == ' ' || line[i] == '\t')) ++i;
        for (name = i; i < n && (snx_is_alnum(line[i]) || line[i] == '_'); ++i) {}
        name = i - name;
        while (i < n && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (name && i < n && line[i] == '=') {
            for (++i; i < n && (line[i] == ' ' || line[i] == '\t'); ++i) {}
            for (; i < n && snx_is_hex(line[i]); ++i) ++hex;
            while (i < n && snx_is_space(line[i])) ++i;
            keys += hex >= 2 && i == n;
        }
        at += n + 1;
    }
    return keys > 0;
}
/* ~/.switch (%USERPROFILE%\.switch on Windows), where hactool and other tools keep the keys; NULL with no home */
static char* keys_home(void) {
#ifdef _WIN32
    const char* home = getenv("USERPROFILE");
    if (!home || !home[0]) home = getenv("HOME");
#else
    const char* home = getenv("HOME");
#endif
    return home && home[0] ? snx_path_join(home, ".switch") : NULL;
}
char* snx_keys_found(const char* data, const char* name) {
    const char* override;
    char* home;
    if (!strcmp(name, "prod.keys"))
        override = g_setup.prod_keys_override;
    else if (!strcmp(name, "title.keys"))
        override = g_setup.title_keys_override;
    else
        override = NULL;
    if (override) return snx_is_file(override) ? snx_strdup(override) : NULL;
    char* path = data ? snx_path_join(data, name) : NULL;
#ifdef SNX_IOS
    free(path); /* the data folder keeps them in keys/ */
    path = data ? snx_path_join(data, "keys") : NULL;
    if (path) {
        char* file = snx_path_join(path, name);
        free(path);
        path = file;
    }
#endif
    if (path && snx_is_file(path)) return path;
    free(path);
    home = keys_home();
    path = home ? snx_path_join(home, name) : NULL;
    free(home);
    if (path && snx_is_file(path)) return path;
    free(path);
    return NULL;
}
char* snx_keys_dir(const char* data) {
#ifdef SNX_XDG
    char* user = snx_user_data_dir();
    int own = user && data && !strcmp(user, data);
    free(user);
    if (own) return snx_strdup(data);
#elif defined(SNX_IOS)
    /* HOME is the app's container root, which it cannot write to: the keys go in the data folder's keys/ */
    if (data) return snx_path_join(data, "keys");
#else
    (void)data;
#endif
    return keys_home();
}
int snx_keys_put(const char* path, const void* data, size_t size, char* why, size_t why_size) {
    char *dir = snx_dirname(path), *temporary = dir && !snx_mkdirs(dir) ? snx_temp_dir(dir) : NULL;
    char* staged = temporary ? snx_path_join(temporary, "keys") : NULL;
    int failed = !staged || snx_write_file(staged, data, size);
#ifndef _WIN32
    if (!failed) failed = chmod(staged, 0600) != 0;
#endif
    if (!failed) failed = snx_replace(staged, path);
    if (failed) snx_os_error(why, why_size); /* before the cleanup's calls set another */
    if (temporary) snx_remove_tree(temporary);
    free(staged);
    free(temporary);
    free(dir);
    return failed;
}
/* folder's name, a keys file, into dir: 0, 1 when folder has none, else -1 with why */
static int keys_copy(const char* folder, const char* dir, const char* name, char* why, size_t why_size) {
    char *from = snx_path_join(folder, name), *to = snx_path_join(dir, name), *data = NULL, reason[256];
    size_t size = 0;
    int rc = -1;
    if (!snx_is_file(from)) {
        rc = 1;
        goto done;
    }
    if (snx_file_size(from) > SETUP_KEYS_MAX || !(data = (char*)snx_read_file(from, &size)))
        snprintf(why, why_size, "%s could not be read as keys", from);
    else if (!snx_keys_text(data, size))
        snprintf(why, why_size, "%s is not a %s: no line name = hex", from, name);
    else if (snx_keys_put(to, data, size, reason, sizeof reason))
        snprintf(why, why_size, "%s could not be written: %s", to, reason);
    else
        rc = 0;
done:
    free(data);
    free(to);
    free(from);
    return rc;
}
int snx_keys_install(const char* folder, const char* dir, int* titles, char* why, size_t why_size) {
    int rc = keys_copy(folder, dir, "prod.keys", why, why_size);
    *titles = 0;
    if (rc > 0) snprintf(why, why_size, "%s has no prod.keys", folder);
    if (rc) return -1;
    rc = keys_copy(folder, dir, "title.keys", why, why_size);
    *titles = !rc;
    return rc < 0 ? -1 : 0;
}

void snx_home_short(const char* path, char* out, size_t size) {
#ifndef _WIN32
    const char* home = getenv("HOME");
    size_t n = home ? strlen(home) : 0;
    while (n > 1 && home[n - 1] == '/') --n;
    if (n > 1 && !strncmp(path, home, n) && path[n] == '/') {
        snprintf(out, size, "~%s", path + n);
        return;
    }
#endif
    snprintf(out, size, "%s", path);
}

/* the line at line, n bytes before its newline, as snx_ini_next reads it: 1 for a section's head, its name into
   section; 2 for a key, its name into key; 0 for any other */
static int ini_line(const char* line, size_t n, char* section, size_t section_size, char* key, size_t key_size) {
    size_t i = 0, end;
    while (i < n && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i < n && line[i] == '[') {
        for (end = i + 1; end < n && line[end] != ']'; ++end) {}
        section[0] = 0;
        if (end < n && end - i - 1 < section_size) {
            memcpy(section, line + i + 1, end - i - 1);
            section[end - i - 1] = 0;
        }
        return 1;
    }
    if (i == n || line[i] == ';' || line[i] == '#') return 0;
    for (end = i; end < n && line[end] != '='; ++end) {}
    if (end == n) return 0;
    while (end > i && (line[end - 1] == ' ' || line[end - 1] == '\t')) --end;
    if (end - i >= key_size) return 0;
    memcpy(key, line + i, end - i);
    key[end - i] = 0;
    return 2;
}
/* text written at path through a file beside it: 0, else -1 with why */
static int put_text(const char* path, const char* text, size_t size, char* why, size_t why_size) {
    char *dir = snx_dirname(path), *staged = snx_format("%s.new", path);
    int failed = !dir || snx_mkdirs(dir) || snx_write_file(staged, text, size) || snx_replace(staged, path);
    if (failed) {
        remove(staged);
        snprintf(why, why_size, "%s could not be written", path);
    }
    free(staged);
    free(dir);
    return failed ? -1 : 0;
}
int snx_ini_put(const char* path, const char* section, const char* key, const char* value, char* why, size_t why_size) {
    size_t size = 0, at = 0, n, out_n = 0, last = (size_t)-1;
    char *text = snx_is_file(path) ? (char*)snx_read_file(path, &size) : (char*)snx_strdup(""), line_key[64];
    char here[64], *entry = snx_format("%s = %s\n", key, value);
    int put = 0;
    if (!text) {
        snprintf(why, why_size, "%s could not be read", path);
        free(entry);
        return -1;
    }
    char* out = (char*)snx_alloc(size + strlen(section) + strlen(entry) + 8);
    here[0] = 0;
    /* each line as it was, but key's in section, which becomes entry; with none there, entry goes after the section's
       last key, or in a section of its own at the end */
    while (at < size) {
        const char* end = (const char*)memchr(text + at, '\n', size - at);
        n = end ? (size_t)(end - (text + at)) + 1 : size - at;
        int kind = ini_line(text + at, n - (end != NULL), here, sizeof here, line_key, sizeof line_key);
        if (!put && kind == 2 && !strcmp(here, section) && !strcmp(line_key, key)) {
            memcpy(out + out_n, entry, strlen(entry));
            out_n += strlen(entry);
            put = 1;
        } else {
            memcpy(out + out_n, text + at, n);
            out_n += n;
            if (!end) out[out_n++] = '\n';
            if (kind && !strcmp(here, section)) last = out_n;
        }
        at += n;
    }
    if (!put && last != (size_t)-1) { /* after the section's last line */
        memmove(out + last + strlen(entry), out + last, out_n - last);
        memcpy(out + last, entry, strlen(entry));
        out_n += strlen(entry);
    } else if (!put)
        out_n += (size_t)sprintf(out + out_n, "[%s]\n%s", section, entry);
    int rc = put_text(path, out, out_n, why, why_size);
    free(out);
    free(entry);
    free(text);
    return rc;
}
int snx_lines_add(const char* path, const char* line, char* why, size_t why_size) {
    size_t size = 0, n = strlen(line);
    char* text = snx_is_file(path) ? (char*)snx_read_file(path, &size) : (char*)snx_strdup("");
    if (!text) {
        snprintf(why, why_size, "%s could not be read", path);
        return -1;
    }
    for (size_t at = 0; at < size;) {
        const char* end = (const char*)memchr(text + at, '\n', size - at);
        size_t len = end ? (size_t)(end - (text + at)) : size - at;
        if (len == n && !memcmp(text + at, line, n)) {
            free(text);
            return 0;
        }
        at += len + 1;
    }
    char* out = (char*)snx_alloc(size + n + 2);
    memcpy(out, text, size);
    if (size && text[size - 1] != '\n') out[size++] = '\n';
    memcpy(out + size, line, n);
    out[size + n] = '\n';
    int rc = put_text(path, out, size + n + 1, why, why_size);
    free(out);
    free(text);
    return rc;
}
