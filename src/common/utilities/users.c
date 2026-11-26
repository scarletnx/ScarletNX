/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/users.h"
#include "common/utilities/common.h"
#include "common/platform/file.h"
#include "common/utilities/config_file.h"
#include "common/utilities/ini_lines.h"
#include "common/utilities/mii_db.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define USERS_FILE       "users.ini"
#define USERS_PICTURE    0x20000 /* nn::account's largest profile image */
#define USERS_FIRST_NAME "Player"
#define ONLINE_FILE      "online.ini"
#define ONLINE_PASSWORD  255 /* a room password's bytes, at most */
#define ONLINE_COMMENT   "; the password of the Eden room ldn=room joins (docs/user/settings.md)"

SnxUser g_users[SNX_USERS_MAX];
int g_user_count;
static int g_user_selected;            /* g_users' index of the user the game runs as */
static unsigned char* g_user_pictures; /* every user's picture, one after another */
static char g_user_notes[1024];        /* snx_users_notes */

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

/* user_data's 2 * SNX_USERS_DATA hex digits into data, an empty one zeros: 0, else -1 */
static int users_data_parse(const char* text, unsigned char data[SNX_USERS_DATA]) {
    memset(data, 0, SNX_USERS_DATA);
    if (!text[0]) return 0;
    if (strlen(text) != 2 * SNX_USERS_DATA) return -1;
    for (int i = 0; i < 2 * SNX_USERS_DATA; i++) {
        char c = text[i];
        int d = c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : -1;
        if (d < 0) return -1;
        data[i / 2] = (unsigned char)(data[i / 2] << 4 | d);
    }
    return 0;
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
/* why the users.ini text is no list of users, or NULL with their count: its users are its [user1] to [userN], in that
   order, each once. Another section is another version's or program's, whose keys are kept and named as any key a
   user does not take, and so are the keys before [user1] */
static const char* users_lines(const char* text, int* count) {
    const char* line = text;
    *count = 0;
    while (*line) {
        const char* end = strchr(line, '\n');
        size_t n = end ? (size_t)(end - line) : strlen(line);
        while (n && (*line == ' ' || *line == '\t')) line++, n--;
        while (n && (line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) n--;
        if (n && *line == '[') {
            int user = n >= 2 && line[n - 1] == ']' ? users_section(line + 1, n - 2) : 0;
            if (n < 2 || line[n - 1] != ']') return "a section line has no ]";
            if (user && user != *count + 1) return "the users are [user1], [user2]... in order, once each";
            *count += user != 0;
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
    text = snx_format("; The console's users (docs/user/usage.md#users), up to 8 in the console's order\n"
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
    if (snx_is_file(path) && plat_replace_file(path, old)) {
        snprintf(why, why_size, "%s could not be kept as %s", path, old);
        remove(staged);
        goto done;
    }
    if (plat_replace_file(staged, path)) {
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
/* [user<number>]'s key=value in users.ini of dir, the file's other lines as they were; without users.ini user 1's
   starts it (snx_users_start). 0, else -1 with why: nothing changed */
static int users_set_key(const char* dir, int number, const char* key, const char* value, char* why, size_t why_size) {
    char *path = snx_path_join(dir, USERS_FILE), *staged = snx_format("%s.new", path), *text = NULL, *out = NULL;
    const char *line, *end, *p, *q;
    size_t size = 0, len, n, cut = 0, cut_end = 0, after = 0, key_len = strlen(key);
    int section = 0, rc = -1;
    if (number < 1 || number > g_user_count) {
        snprintf(why, why_size, "there is no user %d: %d user%s", number, g_user_count, g_user_count > 1 ? "s" : "");
        goto done;
    }
    if (!snx_path_exists(path)) {
        if (number == 1 && !snx_users_start(dir, g_users[0].uid, g_users[0].nickname, why, why_size))
            rc = strcmp(key, "nickname") || strcmp(value, g_users[0].nickname)
                     ? users_set_key(dir, number, key, value, why, why_size)
                     : 0;
        if (number != 1) snprintf(why, why_size, "%s is not there: only user 1 is", path);
        goto done;
    }
    if (snx_config_read(path, &text, &size, why, why_size) <= 0) {
        if (!text && snx_path_exists(path)) goto done; /* said */
        snprintf(why, why_size, "%s is not there", path);
        goto done;
    }
    /* [userN]'s key= line, else where one goes: after the section's last line that is not empty */
    for (line = text; *line; line += len) {
        end = strchr(line, '\n');
        len = end ? (size_t)(end - line) + 1 : strlen(line);
        for (p = line; *p == ' ' || *p == '\t'; ++p) {}
        if (*p == '[' && (q = strchr(p, ']')) != NULL && q < line + len) {
            section = users_section(p + 1, (size_t)(q - p - 1));
            if (section == number) after = (size_t)(line - text) + len;
            continue;
        }
        if (section != number) continue;
        if (*p && *p != '\r' && *p != '\n') after = (size_t)(line - text) + len;
        if (cut_end) continue;
        for (q = p; *q && *q != '=' && *q != '\n'; ++q) {}
        for (n = (size_t)(q - p); n && (p[n - 1] == ' ' || p[n - 1] == '\t'); --n) {}
        if (*q == '=' && n == key_len && !strncmp(p, key, key_len)) {
            cut = (size_t)(line - text);
            cut_end = cut + len;
        }
    }
    if (!after) {
        snprintf(why, why_size, "%s has no [user%d]", path, number);
        goto done;
    }
    if (!cut_end) cut = cut_end = after;
    out = snx_format("%.*s%s%s=%s\n%s", (int)cut, text, cut && text[cut - 1] != '\n' ? "\n" : "", key, value,
                     text + cut_end);
    /* written beside it first: a full disk leaves users.ini as it was */
    if (!out || snx_write_file(staged, out, strlen(out))) {
        snprintf(why, why_size, "%s could not be written", staged);
        remove(staged);
        goto done;
    }
    if (plat_replace_file(staged, path)) {
        snprintf(why, why_size, "%s could not be put in place of %s", staged, path);
        remove(staged);
        goto done;
    }
    rc = 0;
done:
    free(out);
    free(text);
    free(staged);
    free(path);
    return rc;
}
int snx_users_set_picture(const char* dir, int number, const unsigned char* data, uint32_t size, char* why,
                          size_t why_size) {
    char name[40], *path, *staged;
    unsigned char* block;
    size_t total = size;
    int rc = -1, i;
    if (number < 1 || number > g_user_count) {
        snprintf(why, why_size, "there is no user %d: %d user%s", number, g_user_count, g_user_count > 1 ? "s" : "");
        return -1;
    }
    if (size > USERS_PICTURE) {
        snprintf(why, why_size, "a picture of %u bytes is past the %u a user's picture holds", (unsigned)size,
                 (unsigned)USERS_PICTURE);
        return -1;
    }
    snx_users_uid_text(g_users[number - 1].uid, name);
    strcat(name, ".jpg");
    path = snx_path_join(dir, name);
    staged = snx_format("%s.new", path);
    /* written beside it first: a full disk leaves the picture as it was */
    if (snx_mkdirs(dir) || snx_write_file(staged, data, size)) {
        snprintf(why, why_size, "%s could not be written", staged);
        remove(staged);
        goto done;
    }
    if (plat_replace_file(staged, path)) {
        snprintf(why, why_size, "%s could not be put in place of %s", staged, path);
        remove(staged);
        goto done;
    }
    /* the block of every picture made again, this user's the new one */
    for (i = 0; i < g_user_count; i++)
        if (i != number - 1) total += g_users[i].picture_size;
    block = total ? (unsigned char*)snx_alloc(total) : NULL;
    total = 0;
    for (i = 0; i < g_user_count; i++) {
        const unsigned char* from = i == number - 1 ? data : g_user_pictures + g_users[i].picture_at;
        uint32_t n = i == number - 1 ? size : g_users[i].picture_size;
        if (n) memcpy(block + total, from, n);
        g_users[i].picture_at = (uint32_t)total;
        g_users[i].picture_size = n;
        total += n;
    }
    free(g_user_pictures);
    g_user_pictures = block;
    rc = 0;
done:
    free(staged);
    free(path);
    return rc;
}
/* the users from index i out, those after it a place up, the one playing the same user */
static void users_drop(int i) {
    memmove(&g_users[i], &g_users[i + 1], sizeof g_users[0] * (size_t)(g_user_count - i - 1));
    memset(&g_users[--g_user_count], 0, sizeof g_users[0]);
    if (g_user_selected > i) g_user_selected--;
}
int snx_users_remove(const char* dir, int number, char* why, size_t why_size) {
    char *path = snx_path_join(dir, USERS_FILE), *staged = snx_format("%s.new", path), *text = NULL, *out = NULL, *at;
    const char *line, *end, *p, *q;
    size_t size = 0, len;
    int section = 0, rc = -1, k;
    if (number < 1 || number > g_user_count) {
        snprintf(why, why_size, "there is no user %d: %d user%s", number, g_user_count, g_user_count > 1 ? "s" : "");
        goto done;
    }
    if (g_users[number - 1].unsaved) {
        users_drop(number - 1);
        rc = 0;
        goto done;
    }
    text = (char*)snx_read_file(path, &size);
    if (!text) {
        snprintf(why, why_size, "%s could not be read", path);
        goto done;
    }
    /* the file again, [userN]'s lines left out and the sections after it numbered a place down */
    out = (char*)snx_alloc(size + 1);
    at = out;
    for (line = text; *line; line += len) {
        end = strchr(line, '\n');
        len = end ? (size_t)(end - line) + 1 : strlen(line);
        for (p = line; *p == ' ' || *p == '\t'; ++p) {}
        if (*p == '[' && (q = strchr(p, ']')) != NULL && q < line + len) {
            section = users_section(p + 1, (size_t)(q - p - 1));
            if (section > number) {
                k = snprintf(at, size + 1 - (size_t)(at - out), "%.*s[user%d]%.*s", (int)(p - line), line, section - 1,
                             (int)(line + len - q - 1), q + 1);
                at += k;
                continue;
            }
        }
        if (section == number) continue;
        memcpy(at, line, len);
        at += len;
    }
    *at = 0;
    /* written beside it first: a full disk leaves users.ini as it was */
    if (snx_write_file(staged, out, strlen(out))) {
        snprintf(why, why_size, "%s could not be written", staged);
        remove(staged);
        goto done;
    }
    if (plat_replace_file(staged, path)) {
        snprintf(why, why_size, "%s could not be put in place of %s", staged, path);
        remove(staged);
        goto done;
    }
    users_drop(number - 1);
    rc = 0;
done:
    free(out);
    free(text);
    free(staged);
    free(path);
    return rc;
}
/* whether a line of from to to sets key */
static int users_has_key(const char* from, const char* to, const char* key) {
    const char *line, *p, *end;
    size_t n = strlen(key);
    for (line = from; line < to; line = end ? end + 1 : to) {
        end = memchr(line, '\n', (size_t)(to - line));
        for (p = line; *p == ' ' || *p == '\t'; ++p) {}
        if (!strncmp(p, key, n) && (p[n] == '=' || p[n] == ' ' || p[n] == '\t')) return 1;
    }
    return 0;
}
int snx_users_move(const char* dir, int number, int position, char* why, size_t why_size) {
    char *path = snx_path_join(dir, USERS_FILE), *staged = snx_format("%s.new", path), *text = NULL, *out = NULL, *more,
         hex[33];
    const char *line, *end, *p, *q, *start[SNX_USERS_MAX + 1];
    size_t size = 0, len;
    int order[SNX_USERS_MAX], count = 0, rc = -1, i, j, selected = 0;
    SnxUser moved[SNX_USERS_MAX];
    if (number < 1 || number > g_user_count || position < 1 || position > g_user_count) {
        snprintf(why, why_size, "user %d to place %d: there are %d user%s", number, position, g_user_count,
                 g_user_count > 1 ? "s" : "");
        goto done;
    }
    if (number == position) {
        rc = 0;
        goto done;
    }
    if (snx_users_save(dir, g_user_count, why, why_size)) goto done;
    text = (char*)snx_read_file(path, &size);
    if (!text) {
        snprintf(why, why_size, "%s could not be read", path);
        goto done;
    }
    /* where each [userN] starts, and where the file ends */
    for (line = text; *line; line += len) {
        end = strchr(line, '\n');
        len = end ? (size_t)(end - line) + 1 : strlen(line);
        for (p = line; *p == ' ' || *p == '\t'; ++p) {}
        if (*p == '[' && (q = strchr(p, ']')) != NULL && q < line + len && count < SNX_USERS_MAX &&
            users_section(p + 1, (size_t)(q - p - 1)) == count + 1)
            start[count++] = line;
    }
    start[count] = text + strlen(text);
    if (count != g_user_count) {
        snprintf(why, why_size, "%s has %d users, where %d were read", path, count, g_user_count);
        goto done;
    }
    for (i = 0, j = 0; i < count; i++) /* the new order, by the old indices */
        if (i != number - 1) order[j++] = i;
    memmove(order + position, order + position - 1, sizeof order[0] * (size_t)(count - position));
    order[position - 1] = number - 1;
    /* user 1 may go without a uid or a nickname line (it has the ones it always had): written when it leaves. A
       header line ends in a newline: the one that could end the file is [user1]'s alone, and it doesn't move */
    out = snx_format("%.*s", (int)(start[0] - text), text);
    for (j = 0; out && j < count; j++) {
        const char *from = start[order[j]], *to = start[order[j] + 1], *body = memchr(from, '\n', (size_t)(to - from));
        const SnxUser* u = &g_users[order[j]];
        int uid = 1, nickname = 1;
        body = body ? body + 1 : to;
        for (p = from; *p == ' ' || *p == '\t'; ++p) {}
        q = strchr(p, ']');
        if (j) {
            uid = users_has_key(body, to, "uid");
            nickname = users_has_key(body, to, "nickname");
        }
        snx_users_uid_text(u->uid, hex);
        more = snx_format("%s%.*s[user%d]%.*s%s%s%s%s%s%s%.*s%s", out, (int)(p - from), from, j + 1,
                          (int)(body - q - 1), q + 1, uid ? "" : "uid=", uid ? "" : hex, uid ? "" : "\n",
                          nickname ? "" : "nickname=", nickname ? "" : u->nickname, nickname ? "" : "\n",
                          (int)(to - body), body, to > body && to[-1] != '\n' ? "\n" : "");
        free(out);
        out = more;
    }
    /* written beside it first: a full disk leaves users.ini as it was */
    if (!out || snx_write_file(staged, out, strlen(out))) {
        snprintf(why, why_size, "%s could not be written", staged);
        remove(staged);
        goto done;
    }
    if (plat_replace_file(staged, path)) {
        snprintf(why, why_size, "%s could not be put in place of %s", staged, path);
        remove(staged);
        goto done;
    }
    for (j = 0; j < count; j++) {
        moved[j] = g_users[order[j]];
        if (order[j] == g_user_selected) selected = j;
    }
    memcpy(g_users, moved, sizeof g_users[0] * (size_t)count);
    g_user_selected = selected;
    rc = 0;
done:
    free(out);
    free(text);
    free(staged);
    free(path);
    return rc;
}
int snx_users_begin(const uint64_t uid[2], char* why, size_t why_size) {
    SnxUser* u;
    if (g_user_count >= SNX_USERS_MAX) {
        snprintf(why, why_size, "there are %d users, the most there can be", g_user_count);
        return -1;
    }
    if ((!uid[0] && !uid[1]) || snx_users_find(uid)) {
        snprintf(why, why_size, "a uid of zero, or a user's already, is no new user's");
        return -1;
    }
    u = &g_users[g_user_count++];
    memset(u, 0, sizeof *u);
    u->uid[0] = uid[0];
    u->uid[1] = uid[1];
    strcpy(u->nickname, USERS_FIRST_NAME);
    u->unsaved = 1;
    return 0;
}
int snx_users_save(const char* dir, int number, char* why, size_t why_size) {
    char *path = snx_path_join(dir, USERS_FILE), *staged = snx_format("%s.new", path), *text = NULL, *out = NULL, *more;
    char hex[33], data[2 * SNX_USERS_DATA + 1];
    size_t size = 0;
    int rc = -1, i, any, k;
    if (number < 1 || number > g_user_count) {
        snprintf(why, why_size, "there is no user %d: %d user%s", number, g_user_count, g_user_count > 1 ? "s" : "");
        goto done;
    }
    for (i = 0; i < number && !g_users[i].unsaved; i++) {}
    if (i == number) {
        rc = 0; /* all of them in users.ini already */
        goto done;
    }
    if (!snx_is_file(path) && snx_users_start(dir, g_users[0].uid, g_users[0].nickname, why, why_size)) goto done;
    text = (char*)snx_read_file(path, &size);
    if (!text) {
        snprintf(why, why_size, "%s could not be read", path);
        goto done;
    }
    out = snx_format("%s%s", text, size && text[size - 1] != '\n' ? "\n" : "");
    for (; out && i < number; i++) {
        snx_users_uid_text(g_users[i].uid, hex);
        for (k = 0, any = 0; k < SNX_USERS_DATA; k++) {
            snprintf(data + 2 * k, 3, "%02x", g_users[i].user_data[k]);
            any |= g_users[i].user_data[k];
        }
        more = snx_format("%s[user%d]\nuid=%s\nnickname=%s\n%s%s%s", out, i + 1, hex, g_users[i].nickname,
                          any ? "user_data=" : "", any ? data : "", any ? "\n" : "");
        free(out);
        out = more;
    }
    /* written beside it first: a full disk leaves users.ini as it was */
    if (!out || snx_write_file(staged, out, strlen(out))) {
        snprintf(why, why_size, "%s could not be written", staged);
        remove(staged);
        goto done;
    }
    if (plat_replace_file(staged, path)) {
        snprintf(why, why_size, "%s could not be put in place of %s", staged, path);
        remove(staged);
        goto done;
    }
    for (i = 0; i < number; i++) g_users[i].unsaved = 0;
    rc = 0;
done:
    free(out);
    free(text);
    free(staged);
    free(path);
    return rc;
}
int snx_users_rename(const char* dir, int number, const char* nickname, char* why, size_t why_size) {
    if (!snx_users_nickname_valid(nickname)) {
        snprintf(why, why_size, "the nickname ");
        nickname_refused(why, why_size, nickname);
        return -1;
    }
    if (number >= 1 && number <= g_user_count && g_users[number - 1].unsaved) { /* in users.ini with it */
        char old[sizeof g_users[0].nickname];
        memcpy(old, g_users[number - 1].nickname, sizeof old);
        snprintf(g_users[number - 1].nickname, sizeof g_users[number - 1].nickname, "%s", nickname);
        if (!snx_users_save(dir, number, why, why_size)) return 0;
        memcpy(g_users[number - 1].nickname, old, sizeof old);
        return -1;
    }
    if (users_set_key(dir, number, "nickname", nickname, why, why_size)) return -1;
    snprintf(g_users[number - 1].nickname, sizeof g_users[number - 1].nickname, "%s", nickname);
    return 0;
}
int snx_users_set_data(const char* dir, int number, const unsigned char data[SNX_USERS_DATA], char* why,
                       size_t why_size) {
    char hex[2 * SNX_USERS_DATA + 1];
    int any = 0;
    for (int i = 0; i < SNX_USERS_DATA; i++) {
        snprintf(hex + 2 * i, 3, "%02x", data[i]);
        any |= data[i];
    }
    if (number >= 1 && number <= g_user_count && g_users[number - 1].unsaved) { /* in users.ini with it */
        unsigned char old[SNX_USERS_DATA];
        memcpy(old, g_users[number - 1].user_data, sizeof old);
        memcpy(g_users[number - 1].user_data, data, SNX_USERS_DATA);
        if (!snx_users_save(dir, number, why, why_size)) return 0;
        memcpy(g_users[number - 1].user_data, old, sizeof old);
        return -1;
    }
    if (users_set_key(dir, number, "user_data", any ? hex : "", why, why_size)) return -1;
    memcpy(g_users[number - 1].user_data, data, SNX_USERS_DATA);
    return 0;
}
int snx_users_set(const char* dir, int number, const char* key, const char* value, char* why, size_t why_size) {
    int mii = -1;
    if (!strcmp(key, "nickname")) return snx_users_rename(dir, number, value, why, why_size);
    if (!strcmp(key, "mii")) {
        if ((mii = snx_mii_db_find(value)) < 0) {
            snprintf(why, why_size, "mii=%s is no Mii of the database", value);
            return -1;
        }
    } else if (!strcmp(key, "picture")) {
        if (strcmp(value, "mii") && strcmp(value, "file")) {
            snprintf(why, why_size, "picture=%s is neither mii nor file", value);
            return -1;
        }
    } else {
        snprintf(why, why_size, "%s is not a setting: nickname, mii, picture", key);
        return -1;
    }
    if (users_set_key(dir, number, key, value, why, why_size)) return -1;
    if (mii >= 0)
        g_users[number - 1].mii = (uint8_t)mii;
    else
        g_users[number - 1].picture_mii = !strcmp(value, "mii");
    return 0;
}
const char* snx_users_notes(void) { return g_user_notes; }
/* user i's Mii and picture from what users.ini said (NULL: nothing), a line in g_user_notes for a user with a value
   that is not taken: each such value as read and the one in effect. A value users.ini does not give leaves no line,
   and neither ever stops anything */
static void users_mii(int i, const char* mii, const char* picture) {
    SnxUser* u = &g_users[i];
    size_t at = strlen(g_user_notes);
    int found = mii ? snx_mii_db_find(mii) : -1,
        word = picture && (!strcmp(picture, "mii") || !strcmp(picture, "file"));
    int no_mii = mii && found < 0, no_picture = picture && !word;
    u->mii = (uint8_t)(found < 0 ? 0 : found);
    u->picture_mii = word && !strcmp(picture, "mii"); /* the Mii's face only asked for: the file else, or Wimsy */
    if (!no_mii && !no_picture) return;
    at += (size_t)snprintf(g_user_notes + at, sizeof g_user_notes - at, "users: [user%d]", i + 1);
    if (at < sizeof g_user_notes && no_mii)
        at += (size_t)snprintf(g_user_notes + at, sizeof g_user_notes - at, " mii=%s, no Mii of the database: %s%s",
                               mii, snx_mii_db_name(u->mii), no_picture ? ";" : "");
    if (at < sizeof g_user_notes && no_picture)
        at += (size_t)snprintf(g_user_notes + at, sizeof g_user_notes - at, " picture=%s, neither mii nor file: %s",
                               picture, u->picture_mii ? "mii" : "file");
    if (at < sizeof g_user_notes) snprintf(g_user_notes + at, sizeof g_user_notes - at, "\n");
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
/* a line of users.ini the reading leaves (it has no =), for what the load names */
static void users_left(void* ctx, int line, const char* text, const char* does) {
    (void)text;
    (void)does;
    snx_config_other_line((SnxConfigOthers*)ctx, line);
}
/* what a reading of users.ini at path said of its keys, into g_user_notes after what is there */
static void users_note(const char* fmt, ...) {
    size_t at = strlen(g_user_notes);
    va_list ap;
    va_start(ap, fmt);
    if (at < sizeof g_user_notes) vsnprintf(g_user_notes + at, sizeof g_user_notes - at, fmt, ap);
    va_end(ap);
}
int snx_users_load(const char* dir, int selected, char* why, size_t why_size) {
    char *path = snx_path_join(dir, USERS_FILE), *text = NULL, *key, *value, said[1024];
    const char* bad;
    size_t size;
    SnxIni ini;
    SnxConfigOthers others = {0};
    int count = 1, i, j, n, rc = -1, k;
    /* each user's mii= and picture= as read: NULL for none (pointers into text); and each key's line, 0 for none */
    const char *mii_read[SNX_USERS_MAX] = {0}, *picture_read[SNX_USERS_MAX] = {0};
    static const char* const keys[] = {"uid", "nickname", "user_data", "mii", "picture"};
    int seen[SNX_USERS_MAX][5] = {{0}};
    const char* was[SNX_USERS_MAX][5] = {{0}}; /* the value of each key's line, in text */
    g_user_notes[0] = 0;
    free(g_user_pictures);
    g_user_pictures = NULL;
    memset(g_users, 0, sizeof g_users);
    g_user_count = 1;
    if (snx_config_read(path, &text, &size, why, why_size) < 0) goto done;
    if (text && (bad = users_lines(text, &count)) != NULL) {
        snprintf(why, why_size, "%s: %s", path, bad);
        goto done;
    }
    if (text) {
        snx_ini_start(&ini, text);
        ini.left = users_left;
        ini.ctx = &others;
        while (snx_ini_next(&ini, &key, &value)) {
            SnxUser* u;
            n = users_section(ini.section, strlen(ini.section));
            for (k = 0; k < 5 && strcmp(key, keys[k]); ++k) {}
            if (!n || k == 5) { /* another version's or program's: kept, and named */
                snx_config_other_key(&others, ini.section, key, ini.line);
                continue;
            }
            if (seen[n - 1][k]) /* the last line's, as every settings file takes a key given twice */
                snx_config_twice(&others, ini.section, key, was[n - 1][k], seen[n - 1][k], value, ini.line);
            seen[n - 1][k] = ini.line;
            was[n - 1][k] = value;
            u = &g_users[n - 1];
            if (k == 0) {
                uint64_t uid[2];
                if (users_uid_parse(value, uid) || (!uid[0] && !uid[1])) {
                    snprintf(why, why_size, "%s: [user%d] uid=%s is not 32 hex digits, nor zero", path, n, value);
                    goto done;
                }
                u->uid[0] = uid[0];
                u->uid[1] = uid[1];
            } else if (k == 1) {
                if (!snx_users_nickname_valid(value)) {
                    snprintf(why, why_size, "%s: [user%d] nickname= ", path, n);
                    nickname_refused(why, why_size, value);
                    goto done;
                }
                memcpy(u->nickname, value, strlen(value) + 1);
            } else if (k == 2) {
                if (users_data_parse(value, u->user_data)) {
                    snprintf(why, why_size, "%s: [user%d] user_data=%.40s is not %d hex digits, nor empty", path, n,
                             value, 2 * SNX_USERS_DATA);
                    goto done;
                }
            } else if (k == 3) {
                mii_read[n - 1] = value;
            } else {
                picture_read[n - 1] = value;
            }
        }
        if (snx_config_others_said(&others, path, said, sizeof said)) users_note("users: %s\n", said);
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
    for (i = 0; !rc && i < g_user_count; i++) users_mii(i, mii_read[i], picture_read[i]);
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

/* a room password's file: text, made with mode 0600 whatever the umask, so nobody else reads it, not for a moment */
static int write_private(const char* path, const char* text) {
#ifdef _WIN32 /* the user's profile is private to the user */
    return snx_write_file(path, text, strlen(text));
#else
    size_t left = strlen(text);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600), failed;
    if (fd < 0) return 1;
    failed = fchmod(fd, 0600) != 0; /* one left over with another mode */
    while (!failed && left) {
        ssize_t n = write(fd, text, left);
        if (n > 0) {
            text += n;
            left -= (size_t)n;
        } else if (n < 0 && errno != EINTR)
            failed = 1;
    }
    return (close(fd) != 0) | failed;
#endif
}
/* a key that starts with room_ is a mistake of room_password, never another version's */
static int online_mistake(const char* key) { return !strncmp(key, "room_", 5); }
int snx_users_room_password(const char* dir, char* password, size_t size, SnxConfigOthers* others, char* why,
                            size_t why_size) {
    char *path = snx_path_join(dir, ONLINE_FILE), *text = NULL, *key, *value;
    SnxConfigOthers none = {0};
    size_t length;
    SnxIni ini;
    int rc = -1, line = 0;
    password[0] = why[0] = 0;
    if (!others) others = &none;
    memset(others, 0, sizeof *others);
    others->secret = 1;
    if (!path) {
        snprintf(why, why_size, "out of memory");
        goto done;
    }
#ifndef _WIN32
    {
        struct stat st;
        if (snx_is_file(path) && (stat(path, &st) || (st.st_mode & 077))) {
            snprintf(why, why_size, "%s can be read by other users of this machine: chmod 600 it", path);
            goto done;
        }
    }
#endif
    rc = snx_config_read(path, &text, &length, why, why_size);
    if (rc <= 0) { /* none there: 0; one that cannot be read, said: -1 */
        rc = -(rc < 0);
        goto done;
    }
    rc = -1;
    snx_ini_start(&ini, text);
    ini.left = users_left;
    ini.ctx = others;
    while (snx_ini_next(&ini, &key, &value)) {
        if (ini.section[0]) {
            snprintf(why, why_size, "%s: [%s] is not a section: the file has none", path, ini.section);
            goto done;
        }
        if (strcmp(key, "room_password")) {
            if (online_mistake(key)) {
                snprintf(why, why_size, "%s: %s is not a setting: room_password", path, key);
                goto done;
            }
            snx_config_other_key(others, "", key, ini.line);
            continue;
        }
        if (strlen(value) >= size) {
            snprintf(why, why_size, "%s: room_password is longer than %u bytes", path, (unsigned)size - 1);
            goto done;
        }
        if (line) snx_config_twice(others, "", key, NULL, line, NULL, ini.line); /* the last is taken */
        line = ini.line;
        strcpy(password, value);
    }
    rc = 0;
done:
    free(text);
    free(path);
    return rc;
}
/* online.ini's text of size bytes, which it can set room_password in: 1, else 0 with why. Not one with a section, a key
   that is a mistake of room_password or a NUL, which a reading stops at */
static int online_settable(const char* path, const char* text, size_t size, char* why, size_t why_size) {
    const char* nul = (const char*)memchr(text, 0, size);
    char *copy, *key, *value;
    SnxIni ini;
    int only = 1;
    if (nul) {
        snprintf(why, why_size, "%s is not text, its byte %zu is NUL: nothing is written to it", path,
                 (size_t)(nul - text));
        return 0;
    }
    copy = snx_strdup(text);
    if (!copy) {
        snprintf(why, why_size, "out of memory");
        return 0;
    }
    snx_ini_start(&ini, copy);
    while (only && snx_ini_next(&ini, &key, &value))
        if (ini.section[0] || (strcmp(key, "room_password") && online_mistake(key))) {
            snprintf(why, why_size, "%s has %s%s%s, which is not room_password: it stays as it is", path,
                     ini.section[0] ? "[" : "", ini.section[0] ? ini.section : key, ini.section[0] ? "]" : "");
            only = 0;
        }
    free(copy);
    return only;
}
/* the line of the next room_password ini reads, 0 for none, its value in *value */
static int online_password_line(SnxIni* ini, char** value) {
    char* key;
    while (snx_ini_next(ini, &key, value))
        if (!strcmp(key, "room_password")) return ini->line;
    return 0;
}
/* online.ini's text with password as its room_password, or with none for "", owned and NUL-terminated; NULL out of
   memory. Every other line stays byte for byte, and in place: the password, under its comment, goes where the first of
   its lines was, else at the end. *kept: whether a line that is not blank is left beside it */
static char* online_text(const char* text, const char* password, int* kept) {
    size_t size = strlen(text), out = 0, n;
    char *copy = snx_strdup(text), *value;
    char* set = *password ? snx_format(ONLINE_COMMENT "\nroom_password=%s\n", password) : snx_strdup("");
    char* made = set ? (char*)malloc(size + strlen(set) + 2) : NULL; /* and a newline before it, and the NUL */
    const char *line = text, *end;
    SnxIni ini;
    int number = 1, next, placed = 0;
    *kept = 0;
    if (!copy || !made) {
        free(made);
        made = NULL;
        goto done;
    }
    snx_ini_start(&ini, copy);
    next = online_password_line(&ini, &value);
    for (; *line; line = end, ++number) {
        end = strchr(line, '\n');
        end = end ? end + 1 : line + strlen(line);
        n = strcspn(line, "\r\n");
        if (number == next || (n == sizeof ONLINE_COMMENT - 1 && !memcmp(line, ONLINE_COMMENT, n))) {
            if (number == next) next = online_password_line(&ini, &value);
            if (!placed) {
                memcpy(made + out, set, strlen(set));
                out += strlen(set);
                placed = 1;
            }
            continue;
        }
        memcpy(made + out, line, (size_t)(end - line));
        out += (size_t)(end - line);
        *kept |= line[strspn(line, " \t\r")] != '\n' && line[strspn(line, " \t\r")] != 0;
    }
    if (!placed) {
        if (*set && out && made[out - 1] != '\n') made[out++] = '\n';
        memcpy(made + out, set, strlen(set));
        out += strlen(set);
    }
    made[out] = 0;
done:
    free(set);
    free(copy);
    return made;
}
int snx_users_room_password_set(const char* dir, const char* password, char* why, size_t why_size) {
    char *path = snx_path_join(dir, ONLINE_FILE), *staged = NULL, *was = NULL, *text = NULL, *again, *value, *c;
    size_t was_size = 0;
    SnxIni ini;
    int rc = -1, kept, same;
    if (!path || !(staged = snx_format("%s.new", path))) {
        snprintf(why, why_size, "out of memory");
        goto done;
    }
    if (strlen(password) > ONLINE_PASSWORD) {
        snprintf(why, why_size, "a room's password is at most %d bytes, not %u", ONLINE_PASSWORD,
                 (unsigned)strlen(password));
        goto done;
    }
    for (c = (char*)password; *c && (unsigned char)*c >= ' ' && *c != 0x7f; ++c) {}
    if (*c) {
        snprintf(why, why_size, "a room's password has no control character, not byte %#x", (unsigned char)*c);
        goto done;
    }
    was = snx_is_file(path) ? (char*)snx_read_file(path, &was_size) : snx_strdup("");
    if (!was) {
        snprintf(why, why_size, "%s could not be read", path);
        goto done;
    }
    if (!online_settable(path, was, was_size, why, why_size)) goto done;
    text = online_text(was, password, &kept);
    if (!text) {
        snprintf(why, why_size, "out of memory");
        goto done;
    }
    if (!*password && !kept) { /* none, and nothing else in it: the file away */
        if (snx_is_file(path) && remove(path)) {
            snprintf(why, why_size, "%s could not be taken away", path);
            goto done;
        }
        rc = 0;
        goto done;
    }
    /* what its reader reads back: not the spaces at either end, nor a value that is no value of it */
    if (*password) {
        again = snx_strdup(text);
        if (!again) {
            snprintf(why, why_size, "out of memory");
            goto done;
        }
        snx_ini_start(&ini, again);
        same = online_password_line(&ini, &value) && !strcmp(value, password);
        free(again);
        if (!same) {
            snprintf(why, why_size, "a room's password has no space at either end");
            goto done;
        }
    }
    if (snx_mkdirs(dir)) {
        snprintf(why, why_size, "%s could not be made", dir);
        goto done;
    }
    /* written beside it first: a full disk leaves the one there */
    if (write_private(staged, text) || plat_replace_file(staged, path)) {
        snprintf(why, why_size, "%s could not be written", path);
        remove(staged);
        goto done;
    }
    rc = 0;
done:
    free(was);
    free(text);
    free(staged);
    free(path);
    return rc;
}
