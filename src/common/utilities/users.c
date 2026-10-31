/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/users.h"
#include "common/utilities/common.h"
#include "common/utilities/ini_lines.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define USERS_FILE       "users.ini"
#define USERS_PICTURE    0x20000 /* nn::account's largest profile image */
#define USERS_FIRST_NAME "Player"

SnxUser g_users[SNX_USERS_MAX];
int g_user_count;
static int g_user_selected;            /* g_users' index of the user the game runs as */
static unsigned char* g_user_pictures; /* every user's picture, one after another */

void snx_users_uid_text(const uint64_t uid[2], char text[33]) {
    snprintf(text, 33, "%016llx%016llx", (unsigned long long)uid[0], (unsigned long long)uid[1]);
}
/* 32 hex digits, uid[0] the first 16 as snx_users_uid_text writes them */
static int users_uid_parse(const char* text, uint64_t uid[2]) {
    int d;
    uid[0] = uid[1] = 0;
    for (int i = 0; i < 32; i++) {
        char c = text[i];
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f')
            d = (c | 0x20) - 'a' + 10;
        else
            return -1;
        uid[i / 16] = uid[i / 16] << 4 | (uint64_t)d;
    }
    return text[32] ? -1 : 0;
}

const SnxUser* snx_users_find(const uint64_t uid[2]) {
    for (int i = 0; i < g_user_count; i++)
        if (g_users[i].uid[0] == uid[0] && g_users[i].uid[1] == uid[1]) return &g_users[i];
    return NULL;
}
const SnxUser* snx_users_selected(void) { return &g_users[g_user_selected]; }
int snx_users_selected_number(void) { return g_user_selected + 1; }
const unsigned char* snx_users_picture(const SnxUser* user, uint32_t* size) {
    *size = user->picture_size;
    return user->picture_size ? g_user_pictures + user->picture_at : NULL;
}

/* the first byte of nickname the rule refuses, or its length when it refuses none: a space at either end, a control
   character (C0, DEL, C1), or bytes that are no UTF-8 (cut short, an overlong form, a surrogate, past U+10FFFF) */
static size_t nickname_refused_at(const char* nickname) {
    const unsigned char* p = (const unsigned char*)nickname;
    size_t n = strlen(nickname), i = 0;
    if (n && p[0] == ' ') return 0;
    while (i < n) {
        unsigned c = p[i], code, follow, k;
        if (c < 0x80) {
            if (c < 0x20 || c == 0x7F) return i;
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF)
            follow = 1, code = c & 0x1F;
        else if (c >= 0xE0 && c <= 0xEF)
            follow = 2, code = c & 0x0F;
        else if (c >= 0xF0 && c <= 0xF4)
            follow = 3, code = c & 0x07;
        else
            return i;
        if (n - i <= follow) return i;
        for (k = 1; k <= follow; k++) {
            if ((p[i + k] & 0xC0) != 0x80) return i;
            code = code << 6 | (p[i + k] & 0x3F);
        }
        if ((follow == 2 && code < 0x800) || (follow == 3 && (code < 0x10000 || code > 0x10FFFF)) ||
            (code >= 0xD800 && code <= 0xDFFF) || (code >= 0x80 && code <= 0x9F))
            return i;
        i += follow + 1;
    }
    return n && p[n - 1] == ' ' ? n - 1 : n;
}
int snx_users_nickname_valid(const char* nickname) {
    size_t n = strlen(nickname);
    return n >= SNX_USERS_NICKNAME_MIN && n <= SNX_USERS_NICKNAME_MAX && nickname_refused_at(nickname) == n;
}
const char* snx_users_room_nickname(const SnxUser* user) { return user->nickname; }
/* appended to why: what is wrong with nickname, its length or its first byte the rule refuses, never the nickname
   itself: a game's stop carries why to the log, which players attach to public reports */
static void nickname_refused(char* why, size_t why_size, const char* nickname) {
    size_t at = strlen(why), n = strlen(nickname), i;
    if (n < SNX_USERS_NICKNAME_MIN || n > SNX_USERS_NICKNAME_MAX)
        snprintf(why + at, why_size - at, "has %zu bytes: ", n);
    else {
        i = nickname_refused_at(nickname);
        snprintf(why + at, why_size - at, "has 0x%02x at byte %zu: ", (unsigned)(unsigned char)nickname[i], i + 1);
    }
    at = strlen(why);
    snprintf(why + at, why_size - at,
             "the account service keeps %d to %d bytes of UTF-8 with no control character nor a space at either end",
             SNX_USERS_NICKNAME_MIN, SNX_USERS_NICKNAME_MAX);
}

/* the [userN] of section, 1 to 8, or 0 */
static int users_section(const char* section, size_t n) {
    if (n != 5 || strncmp(section, "user", 4) || section[4] < '1' || section[4] > '0' + SNX_USERS_MAX) return 0;
    return section[4] - '0';
}
/* why the users.ini text is no list of users, or NULL with their count: each line a comment, a key=value, or a [userN],
   these [user1] to [userN] in that order, each once */
static const char* users_lines(const char* text, int* count) {
    const char* line = text;
    *count = 0;
    while (*line) {
        const char* end = strchr(line, '\n');
        size_t n = end ? (size_t)(end - line) : strlen(line);
        while (n && (*line == ' ' || *line == '\t')) line++, n--;
        while (n && (line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) n--;
        if (n && *line == '[') {
            if (n < 2 || line[n - 1] != ']') return "a section line has no ]";
            if (!users_section(line + 1, n - 2)) return "a section is not [user1] to [user8]";
            if (users_section(line + 1, n - 2) != *count + 1)
                return "the users are [user1], [user2]... in order, once each";
            ++*count;
        } else if (n && *line != ';' && *line != '#') {
            if (!memchr(line, '=', n)) return "a line is neither a [userN], a key=value nor a comment";
            if (!*count) return "a key comes before [user1]";
        }
        if (!end) break;
        line = end + 1;
    }
    return *count ? NULL : "no [user1]";
}

/* users/<uid>.jpg of each user, read into one block: the bytes the account service keeps, as a program stored them
   (a JPEG from the console's editor), at most 128 KiB, the most a program takes; an empty file or none is no picture */
static int users_read_pictures(const char* dir, char* why, size_t why_size) {
    unsigned char* data[SNX_USERS_MAX];
    size_t size[SNX_USERS_MAX], total = 0;
    int read = 0, rc = 0;
    for (int i = 0; i < g_user_count && !rc; i++) {
        char name[40];
        snx_users_uid_text(g_users[i].uid, name);
        strcat(name, ".jpg");
        char* path = snx_path_join(dir, name);
        size[i] = 0;
        data[i] = snx_is_file(path) ? snx_read_file(path, &size[i]) : NULL;
        read++;
        if (snx_is_file(path) && !data[i]) {
            snprintf(why, why_size, "%s could not be read", path);
            rc = -1;
        } else if (data[i] && size[i] > USERS_PICTURE) {
            snprintf(why, why_size, "%s is %zu bytes, past the %u a user's picture holds", path, size[i],
                     (unsigned)USERS_PICTURE);
            rc = -1;
        }
        if (!data[i]) size[i] = 0;
        total += size[i];
        free(path);
    }
    if (!rc && total) {
        g_user_pictures = (unsigned char*)snx_alloc(total);
        total = 0;
        for (int i = 0; i < g_user_count; i++) {
            g_users[i].picture_at = (uint32_t)total;
            g_users[i].picture_size = (uint32_t)size[i];
            if (size[i]) memcpy(g_user_pictures + total, data[i], size[i]);
            total += size[i];
        }
    }
    for (int i = 0; i < read; i++) free(data[i]);
    return rc;
}

char* snx_users_dir(const char* folder) {
    char *data = snx_menu_dir(folder, SNX_USER_DATA), *dir = data ? snx_path_join(data, "users") : NULL;
    free(data);
    return dir;
}
int snx_users_start(const char* dir, const uint64_t uid[2], const char* nickname, char* why, size_t why_size) {
    char* path = snx_path_join(dir, USERS_FILE);
    char *old = snx_format("%s.old", path), *staged = snx_format("%s.new", path), *text = NULL, hex[33];
    int rc = -1;
    if (!snx_users_nickname_valid(nickname)) {
        snprintf(why, why_size, "the nickname ");
        nickname_refused(why, why_size, nickname);
        goto done;
    }
    if (!uid[0] && !uid[1]) {
        snprintf(why, why_size, "a uid of zero is no user's");
        goto done;
    }
    snx_users_uid_text(uid, hex);
    text = snx_format("; The console's users (docs/USAGE.md#users), up to 8 in the console's order\n"
                      "[user1]\nuid=%s\nnickname=%s\n",
                      hex, nickname);
    if (snx_mkdirs(dir)) {
        snprintf(why, why_size, "%s could not be made", dir);
        goto done;
    }
    /* written beside it first: a full disk leaves users.ini as it was */
    if (snx_write_file(staged, text, strlen(text))) {
        snprintf(why, why_size, "%s could not be written", staged);
        remove(staged);
        goto done;
    }
    if (snx_is_file(path) && snx_replace(path, old)) {
        snprintf(why, why_size, "%s could not be kept as %s", path, old);
        remove(staged);
        goto done;
    }
    if (snx_replace(staged, path)) {
        snprintf(why, why_size, "%s could not be put in place of %s, whose users are in %s", staged, path, old);
        goto done;
    }
    rc = 0;
done:
    free(text);
    free(staged);
    free(old);
    free(path);
    return rc;
}
int snx_users_rename(const char* dir, int number, const char* nickname, char* why, size_t why_size) {
    char *path = snx_path_join(dir, USERS_FILE), *staged = snx_format("%s.new", path), *text = NULL, *out = NULL;
    const char *line, *end, *p, *q;
    size_t size = 0, len, n, cut = 0, cut_end = 0, after = 0;
    int section = 0, rc = -1;
    if (!snx_users_nickname_valid(nickname)) {
        snprintf(why, why_size, "the nickname ");
        nickname_refused(why, why_size, nickname);
        goto done;
    }
    if (number < 1 || number > g_user_count) {
        snprintf(why, why_size, "there is no user %d: %d user%s", number, g_user_count, g_user_count > 1 ? "s" : "");
        goto done;
    }
    if (!snx_is_file(path)) {
        if (number == 1) rc = snx_users_start(dir, g_users[0].uid, nickname, why, why_size);
        if (number != 1) snprintf(why, why_size, "%s is not there: only user 1 is", path);
        goto done;
    }
    text = (char*)snx_read_file(path, &size);
    if (!text) {
        snprintf(why, why_size, "%s could not be read", path);
        goto done;
    }
    /* [userN]'s nickname= line, else where one goes after its section line */
    for (line = text; *line; line += len) {
        end = strchr(line, '\n');
        len = end ? (size_t)(end - line) + 1 : strlen(line);
        for (p = line; *p == ' ' || *p == '\t'; ++p) {}
        if (*p == '[' && (q = strchr(p, ']')) != NULL && q < line + len) {
            section = users_section(p + 1, (size_t)(q - p - 1));
            if (section == number) after = (size_t)(line - text) + len;
            continue;
        }
        if (section != number || cut_end) continue;
        for (q = p; *q && *q != '=' && *q != '\n'; ++q) {}
        for (n = (size_t)(q - p); n && (p[n - 1] == ' ' || p[n - 1] == '\t'); --n) {}
        if (*q == '=' && n == 8 && !strncmp(p, "nickname", 8)) {
            cut = (size_t)(line - text);
            cut_end = cut + len;
        }
    }
    if (!after) {
        snprintf(why, why_size, "%s has no [user%d]", path, number);
        goto done;
    }
    if (!cut_end) cut = cut_end = after;
    out = snx_format("%.*s%snickname=%s\n%s", (int)cut, text, cut && text[cut - 1] != '\n' ? "\n" : "", nickname,
                     text + cut_end);
    /* written beside it first: a full disk leaves users.ini as it was */
    if (!out || snx_write_file(staged, out, strlen(out))) {
        snprintf(why, why_size, "%s could not be written", staged);
        remove(staged);
        goto done;
    }
    if (snx_replace(staged, path)) {
        snprintf(why, why_size, "%s could not be put in place of %s", staged, path);
        remove(staged);
        goto done;
    }
    snprintf(g_users[number - 1].nickname, sizeof g_users[number - 1].nickname, "%s", nickname);
    rc = 0;
done:
    free(out);
    free(text);
    free(staged);
    free(path);
    return rc;
}
/* user 1 alone, as without users.ini: what a users.ini that is refused leaves */
static void users_reset(void) {
    free(g_user_pictures);
    g_user_pictures = NULL;
    memset(g_users, 0, sizeof g_users);
    g_users[0].uid[0] = SNX_USERS_FIRST_UID0;
    g_users[0].uid[1] = SNX_USERS_FIRST_UID1;
    strcpy(g_users[0].nickname, USERS_FIRST_NAME);
    g_user_count = 1;
    g_user_selected = 0;
}
int snx_users_load(const char* dir, int selected, char* why, size_t why_size) {
    char *path = snx_path_join(dir, USERS_FILE), *text, *key, *value;
    const char* bad;
    size_t size;
    SnxIni ini;
    int count = 1, i, j, n, rc = -1;
    free(g_user_pictures);
    g_user_pictures = NULL;
    memset(g_users, 0, sizeof g_users);
    g_user_count = 1;
    text = snx_is_file(path) ? (char*)snx_read_file(path, &size) : NULL;
    if (snx_is_file(path) && !text) {
        snprintf(why, why_size, "%s could not be read", path);
        goto done;
    }
    if (text && (bad = users_lines(text, &count)) != NULL) {
        snprintf(why, why_size, "%s: %s", path, bad);
        goto done;
    }
    if (text) {
        snx_ini_start(&ini, text);
        while (snx_ini_next(&ini, &key, &value)) {
            SnxUser* u;
            n = users_section(ini.section, strlen(ini.section));
            u = &g_users[n - 1]; /* users_lines saw every key under a [userN] */
            if (!strcmp(key, "uid")) {
                uint64_t uid[2];
                if (users_uid_parse(value, uid) || (!uid[0] && !uid[1])) {
                    snprintf(why, why_size, "%s: [user%d] uid=%s is not 32 hex digits, nor zero", path, n, value);
                    goto done;
                }
                u->uid[0] = uid[0];
                u->uid[1] = uid[1];
            } else if (!strcmp(key, "nickname")) {
                if (!snx_users_nickname_valid(value)) {
                    snprintf(why, why_size, "%s: [user%d] nickname= ", path, n);
                    nickname_refused(why, why_size, value);
                    goto done;
                }
                memcpy(u->nickname, value, strlen(value) + 1);
            } else {
                snprintf(why, why_size, "%s: [user%d] %s is not a setting: uid, nickname", path, n, key);
                goto done;
            }
        }
    }
    /* user 1 has the uid users.ini gives it, else the one it always had; "Player" unless it names it */
    if (!g_users[0].uid[0] && !g_users[0].uid[1]) {
        g_users[0].uid[0] = SNX_USERS_FIRST_UID0;
        g_users[0].uid[1] = SNX_USERS_FIRST_UID1;
    }
    if (!g_users[0].nickname[0]) strcpy(g_users[0].nickname, USERS_FIRST_NAME);
    for (i = 1; i < count; i++) {
        if (!g_users[i].uid[0] && !g_users[i].uid[1]) {
            snprintf(why, why_size, "%s: [user%d] has no uid", path, i + 1);
            goto done;
        }
        if (!g_users[i].nickname[0]) {
            snprintf(why, why_size, "%s: [user%d] has no nickname", path, i + 1);
            goto done;
        }
        for (j = 0; j < i; j++)
            if (g_users[j].uid[0] == g_users[i].uid[0] && g_users[j].uid[1] == g_users[i].uid[1]) {
                snprintf(why, why_size, "%s: [user%d] has the uid of [user%d]", path, i + 1, j + 1);
                goto done;
            }
    }
    g_user_count = count;
    if (selected < 1 || selected > g_user_count) {
        snprintf(why, why_size, "user=%d, and %s has %d user%s", selected, text ? path : "no users.ini", g_user_count,
                 g_user_count == 1 ? "" : "s");
        goto done;
    }
    g_user_selected = selected - 1;
    rc = users_read_pictures(dir, why, why_size);
done:
    if (rc) users_reset(); /* nothing half read stays: the caller stops the game */
    free(text);
    free(path);
    return rc;
}
int snx_users_choose(const char* dir, const char* uid_text, const char* old_number, int* old, char* why,
                     size_t why_size) {
    uint64_t uid[2];
    char* end;
    long number;
    *old = 0;
    if (uid_text && uid_text[0]) {
        if (users_uid_parse(uid_text, uid) || (!uid[0] && !uid[1])) {
            snprintf(why, why_size, "user_uid=%.40s is not 32 hex digits, nor zero", uid_text);
            users_reset();
            return -1;
        }
        if (snx_users_load(dir, 1, why, why_size)) return -1;
        for (int i = 0; i < g_user_count; i++)
            if (g_users[i].uid[0] == uid[0] && g_users[i].uid[1] == uid[1]) {
                g_user_selected = i;
                return 0;
            }
        snprintf(why, why_size, "user_uid=%s is no user's: users.ini of %s has %d user%s", uid_text, dir, g_user_count,
                 g_user_count == 1 ? "" : "s");
        users_reset();
        return -1;
    }
    if (old_number && old_number[0]) {
        number = strtol(old_number, &end, 10);
        if (*end || end == old_number || number < 1 || number > SNX_USERS_MAX) {
            snprintf(why, why_size, "user=%.40s, which user_uid replaces, is not a number from 1 to %d", old_number,
                     SNX_USERS_MAX);
            users_reset();
            return -1;
        }
        if (snx_users_load(dir, (int)number, why, why_size)) return -1;
        *old = (int)number;
        return 0;
    }
    return snx_users_load(dir, 1, why, why_size);
}
