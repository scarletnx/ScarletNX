/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/game_ini.h"
#include "common/utilities/ini_lines.h"
#include "common/utilities/ascii.h"
#include "common/platform/file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* snx_game_ini(const char* folder, const char* name) {
    char *own = snx_user_path(folder, SNX_USER_CONFIG), *base = snx_path_join(own, name);
    char* path = snx_format("%s.ini", base);
    free(own);
    free(base);
    return path;
}
/* key's value in an .ini's text, the last one's, as the runtime reads it: in the text, NULL for none */
static char* text_value(char* text, const char* key) {
    SnxIni ini;
    char *k, *value, *found = NULL;
    snx_ini_start(&ini, text);
    while (snx_ini_next(&ini, &k, &value))
        if (!strcmp(k, key)) found = value;
    return found;
}
char* snx_ini_value(const char* path, const char* key) {
    size_t size;
    char *text = (char*)snx_read_file(path, &size), *value = text ? text_value(text, key) : NULL;
    char* copy = value && *value ? snx_strdup(value) : NULL;
    free(text);
    return copy;
}
/* a line, from line to end, that sets key= */
static int key_line(const char* line, const char* end, const char* key) {
    size_t n = strlen(key);
    while (line < end && (*line == ' ' || *line == '\t')) line++;
    if ((size_t)(end - line) < n || strncmp(line, key, n)) return 0;
    for (line += n; line < end && (*line == ' ' || *line == '\t'); line++) continue;
    return line < end && *line == '=';
}
/* a line, from line to end, that starts [Game] */
static int game_line(const char* line, const char* end) {
    while (line < end && (*line == ' ' || *line == '\t')) line++;
    while (end > line && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) end--;
    return end - line == 6 && !strncmp(line, "[Game]", 6);
}
int snx_ini_record(const char* path, const char* key, const char* value) {
    char *dir = snx_dirname(path), *tmp = snx_format("%s.tmp", path), *out = NULL, *text, *copy, *had;
    const char *line, *end, *next;
    size_t size = 0, n = 0, len = strlen(key) + strlen(value);
    int placed = 0, rc = 0;
    if (value[strcspn(value, "\r\n")]) {
        rc = snx_error("%s=%s has a line break, which the game's settings cannot keep", key, value);
        goto done;
    }
    text = (char*)snx_read_file(path, &size);
    if (!text && snx_path_exists(path)) {
        rc = snx_error("cannot read %s to record %s= in it", path, key);
        goto done;
    }
    if (text) {
        copy = snx_strdup(text);
        had = text_value(copy, key);
        placed = had && !strcmp(had, value);
        free(copy);
        if (placed) {
            free(text);
            goto done;
        }
    }
    out = (char*)snx_alloc(size + len + 24);
    for (line = text; line && line < text + size; line = next) {
        end = (const char*)memchr(line, '\n', (size_t)(text + size - line));
        next = end ? end + 1 : text + size;
        if (!end) end = next;
        if (key_line(line, end, key)) continue;
        memcpy(out + n, line, (size_t)(next - line));
        n += (size_t)(next - line);
        if (next == end) out[n++] = '\n'; /* the file's last line, without a newline */
        if (!placed && game_line(line, end)) {
            n += (size_t)sprintf(out + n, "%s=%s\n", key, value);
            placed = 1;
        }
    }
    free(text);
    if (!placed) n += (size_t)sprintf(out + n, "%s[Game]\n%s=%s\n", n ? "\n" : "", key, value);
    if (snx_mkdirs(dir) || snx_write_file(tmp, out, n) || plat_replace_file(tmp, path)) {
        snx_remove_tree(tmp);
        rc = snx_error("cannot record %s=%s in %s", key, value, path);
    }
done:
    free(dir);
    free(tmp);
    free(out);
    return rc;
}
int snx_ini_has(const char* path, const char* key) {
    size_t size = 0;
    char* text = (char*)snx_read_file(path, &size);
    const char *line, *end, *next;
    int found = 0;
    for (line = text; line && line < text + size && !found; line = next) {
        end = (const char*)memchr(line, '\n', (size_t)(text + size - line));
        next = end ? end + 1 : text + size;
        found = key_line(line, end ? end : next, key);
    }
    free(text);
    return found;
}
int snx_ini_drop(const char* path, const char* key) {
    size_t size = 0, n = 0;
    char *text = (char*)snx_read_file(path, &size), *out, *tmp;
    const char *line, *end, *next;
    int dropped = 0, rc = 0;
    if (!text) return snx_path_exists(path) ? snx_error("cannot read %s to take %s= out of it", path, key) : 0;
    out = (char*)snx_alloc(size + 1);
    for (line = text; line < text + size; line = next) {
        end = (const char*)memchr(line, '\n', (size_t)(text + size - line));
        next = end ? end + 1 : text + size;
        if (key_line(line, end ? end : next, key)) {
            dropped = 1;
            continue;
        }
        memcpy(out + n, line, (size_t)(next - line));
        n += (size_t)(next - line);
    }
    tmp = snx_format("%s.tmp", path);
    if (dropped && (snx_write_file(tmp, out, n) || plat_replace_file(tmp, path))) {
        snx_remove_tree(tmp);
        rc = snx_error("cannot take %s= out of %s", key, path);
    }
    free(tmp);
    free(out);
    free(text);
    return rc;
}
char* snx_game_source(const char* folder, const char* name, int* recorded) {
    char *ini = snx_game_ini(folder, name), *source = snx_ini_value(ini, "source"), path[SNX_PATH_SIZE];
    free(ini);
    if (!source) { /* a console driver's, in the folder: the user's config folder is elsewhere (snx_user_path) */
        ini = snx_format("%s/%s.ini", folder, name);
        source = snx_ini_value(ini, "source");
        free(ini);
    }
    if (recorded) *recorded = source != NULL;
    if (source) return source;
    if (snx_path_format(path, sizeof path, "%s/%s.source", folder, name)) return NULL;
    if (recorded) *recorded = snx_is_link(path) || snx_path_exists(path);
    return snx_strdup(path);
}
char* snx_extracted_part(const char* path, int romfs) {
    static const char* const names[2][2] = {{"exefs", "ExeFS"}, {"romfs", "RomFS"}};
    for (int i = 0; i < 2; i++) {
        char* part = snx_path_join(path, names[romfs != 0][i]);
        if (part && snx_is_dir(part)) return part;
        free(part);
    }
    return NULL;
}
int snx_extracted_game(const char* path) {
    char *exefs = path && snx_is_dir(path) ? snx_extracted_part(path, 0) : NULL, *main;
    size_t n;
    int found;
    main = exefs ? snx_path_join(exefs, "main") : NULL;
    found = main && snx_is_file(main);
    free(main);
    free(exefs);
    n = found ? strlen(path) : 0;
    while (n && (path[n - 1] == '/' || path[n - 1] == '\\')) n--;
    if (n > 5 && !strncmp(path + n - 5, "_data", 5)) { /* a recompiled game's own, beside its stamp */
        char* stamp = snx_format("%.*s.abi", (int)(n - 5), path);
        found = !stamp || !snx_is_file(stamp);
        free(stamp);
    }
    return found;
}
const char* snx_source_kind(const char* source) {
    static const char* const dumps[] = {".xci", ".nsp", ".dxci", ".dnsp", ".nro"};
    size_t i;
    if (!source || snx_is_dir(source)) return "folder";
    if (snx_is_file(source)) return "dump";
    for (i = 0; i < sizeof dumps / sizeof *dumps; i++) /* not there: by its name */
        if (snx_ends_with_nocase(source, dumps[i])) return "dump";
    return "folder";
}
int snx_record_source(const char* folder, const char* name, const char* dump) {
    char* ini = snx_game_ini(folder, name);
    int rc = snx_ini_record(ini, "source", dump);
    free(ini);
    return rc;
}
size_t snx_addons_parse(const char* text, const char* game, SnxAddon* list, size_t max) {
    char word[32];
    size_t count = 0, n, i, update = (size_t)-1;
    SnxU64 own, id;
    if (!snx_title_parse(game, &own)) return 0;
    while (text && *text) {
        while (*text == ',' || *text == ' ' || *text == '\t') text++;
        for (n = 0; text[n] && text[n] != ',' && text[n] != ' ' && text[n] != '\t'; n++) continue;
        if (n >= 16 && n < sizeof word) {
            for (i = 0; i < n; i++) word[i] = i < 16 ? (char)snx_to_upper(text[i]) : text[i];
            word[n] = 0;
            if (snx_addon_entry_id(own, word, &id)) {
                for (i = 0; i < count && list[i].id != id; i++) continue;
                if (i < count && snx_addon_kind(own, id) == SNX_ADDON_UPDATE)
                    i = update; /* one update: the last one listed */
                if (i == update && update != (size_t)-1)
                    memcpy(list[i].entry, word, n + 1);
                else if (i == count && count < max) {
                    if (snx_addon_kind(own, id) == SNX_ADDON_UPDATE) update = count;
                    memcpy(list[count].entry, word, n + 1);
                    list[count++].id = id;
                }
            }
        }
        text += n;
    }
    return count;
}
char* snx_addons_text(const SnxAddon* list, size_t count) {
    char* text = (char*)snx_alloc(count * 32 + 1);
    text[0] = 0;
    for (size_t i = 0; i < count; i++) {
        if (i) strcat(text, ",");
        strcat(text, list[i].entry);
    }
    return text;
}
int snx_addon_check(const char* folder, const char* name, const char* entry) {
    SnxAddon* list = (SnxAddon*)snx_alloc(SNX_ADDONS_MAX * sizeof *list);
    char *ini = snx_game_ini(folder, name), *text = snx_ini_value(ini, "addons");
    size_t count = snx_addons_parse(text, name, list, SNX_ADDONS_MAX), i;
    SnxU64 game = 0, id = 0;
    int rc;
    free(text);
    if (strlen(name) >= 16) { /* the game's title ID, its name's first 16 digits */
        char own[17];
        memcpy(own, name, 16);
        own[16] = 0;
        if (!snx_title_parse(own, &game) || (name[16] && name[16] != '_')) game = 0;
    }
    snx_addon_entry_id(game, entry, &id);
    for (i = 0; i < count && list[i].id != id; i++) continue;
    if (i == count && count < SNX_ADDONS_MAX) list[count++].id = id;
    if (i < count && strlen(entry) < sizeof list[i].entry) strcpy(list[i].entry, entry);
    text = snx_addons_text(list, count);
    rc = snx_ini_record(ini, "addons", text);
    free(text);
    free(ini);
    free(list);
    return rc;
}

static const char* const g_system_words[] = {"switch", "3ds", "vita", "gba"};
const char* snx_system_word(SnxSystem system) { return g_system_words[system]; }
SnxSystem snx_game_system(const char* folder, const char* name) {
    char *ini = snx_format("%s/%s.ini", folder, name), *word = snx_ini_value(ini, "system"), *model = NULL,
         *card = snx_path_join(folder, "ux0");
    SnxSystem system = SNX_SYSTEM_SWITCH;
    for (size_t i = 0; word && i < sizeof(g_system_words) / sizeof(g_system_words[0]); ++i)
        if (!strcmp(word, g_system_words[i])) system = (SnxSystem)i;
    if (!word && (model = snx_ini_value(ini, "model")) != NULL)
        system = SNX_SYSTEM_3DS;
    else if (!word && snx_is_dir(card))
        system = SNX_SYSTEM_VITA;
    free(card);
    free(model);
    free(word);
    free(ini);
    return system;
}
int snx_record_system(const char* folder, const char* name, SnxSystem system) {
    char* ini = snx_format("%s/%s.ini", folder, name);
    int rc = snx_ini_record(ini, "system", snx_system_word(system));
    free(ini);
    return rc;
}
/* 1 when the files at a and b hold the same bytes */
static int same_bytes(const char* a, const char* b) {
    FILE *x = fopen(a, "rb"), *y = fopen(b, "rb");
    unsigned char p[16384], q[16384];
    size_t n;
    int same = x && y;
    while (same && (n = fread(p, 1, sizeof p, x)) > 0) same = fread(q, 1, n, y) == n && !memcmp(p, q, n);
    same = same && !ferror(x) && fread(q, 1, 1, y) == 0 && !ferror(y);
    if (x) fclose(x);
    if (y) fclose(y);
    return same;
}
int snx_place_runtime(const char* runtime, const char* copy, char* error, size_t error_size) {
    char *own = snx_absolute(runtime), *mine = snx_absolute(copy), *staged = snx_format("%s.tmp", copy), why[256];
    int rc = 0;
    if (!own || !mine || !staged) {
        snprintf(error, error_size, "out of memory for the game's runtime");
        rc = -1;
    } else if (strcmp(own, mine) && !(snx_is_file(copy) && same_bytes(runtime, copy))) {
        if (!snx_copy_file(runtime, staged) && !snx_make_executable(staged) && !plat_replace_file(staged, copy))
            rc = 1;
        else {
            snprintf(error, error_size, "cannot copy the runtime %s to %s: %s", runtime, copy,
                     snx_os_error(why, sizeof why));
            if (snx_is_file(staged)) (void)remove(staged);
            rc = -1;
        }
    }
    free(staged);
    free(mine);
    free(own);
    return rc;
}

/* ---- a collection's programs */
#if defined(_WIN32)
#define PROGRAM_CODE "_code.dll"
#elif defined(SNX_IOS)
#define PROGRAM_CODE "_code.snxobj"
#else
#define PROGRAM_CODE "_code.so"
#endif
char* snx_program_code(const char* folder, const char* name) { return snx_format("%s/%s" PROGRAM_CODE, folder, name); }
static int program_made(const char* folder, const char* name) {
    char* code = snx_program_code(folder, name);
    int made = snx_is_file(code);
    free(code);
    return made;
}
void snx_game_update(const char* folder, const char* name, char update[32]) {
    char *ini = snx_game_ini(folder, name), *text = snx_ini_value(ini, "addons");
    SnxAddon* list = (SnxAddon*)snx_alloc(SNX_ADDONS_MAX * sizeof *list);
    size_t count = snx_addons_parse(text, name, list, SNX_ADDONS_MAX), i;
    SnxU64 own = 0;
    update[0] = 0;
    snx_title_parse(name, &own);
    for (i = 0; i < count; i++)
        if (snx_addon_kind(own, list[i].id) == SNX_ADDON_UPDATE) memcpy(update, list[i].entry, sizeof list[i].entry);
    free(list);
    free(text);
    free(ini);
}
/* snx_program_played with the game's update already read */
static int played_with(const char* folder, const char* name, const char* update, int index, char* out, size_t size) {
    if (update[0] && !snx_program_name(name, update, index, out, size) && program_made(folder, out)) return 1;
    if (snx_program_name(name, NULL, index, out, size)) {
        if (size) out[0] = 0;
        return 0;
    }
    return program_made(folder, out);
}
int snx_program_played(const char* folder, const char* name, int index, char* out, size_t size) {
    char update[32];
    snx_game_update(folder, name, update);
    return played_with(folder, name, update, index, out, size);
}
int snx_game_programs(const char* folder, const char* name, unsigned char made[SNX_PROGRAMS_MAX]) {
    char program[SNX_PATH_SIZE], update[32];
    int count = 0, i;
    /* once for every index: reading it resolves the .ini's path through each folder above the game, which on
       Windows opens each of them */
    snx_game_update(folder, name, update);
    for (i = 0; i < SNX_PROGRAMS_MAX; i++) {
        made[i] = (unsigned char)played_with(folder, name, update, i, program, sizeof program);
        count += made[i];
    }
    return count;
}
/* the made programs' indices as a message names them: "0-3", "0, 1 and 3" */
static void programs_text(const unsigned char* made, char* out, size_t size) {
    int i, count = 0, last = -1, from_zero = 1, k = 0;
    size_t n = 0;
    out[0] = 0;
    for (i = 0; i < SNX_PROGRAMS_MAX; i++)
        if (made[i]) {
            from_zero = from_zero && i == last + 1;
            last = i;
            count++;
        }
    if (count > 2 && from_zero) {
        snprintf(out, size, "0-%d", last);
        return;
    }
    for (i = 0; i < SNX_PROGRAMS_MAX && n < size; i++)
        if (made[i]) {
            n += (size_t)snprintf(out + n, size - n, "%s%d", !k ? "" : k == count - 1 ? " and " : ", ", i);
            k++;
        }
}
int snx_program_pick(const char* folder, const char* name, int index, char* out, size_t size, char* why,
                     size_t why_size) {
    unsigned char made[SNX_PROGRAMS_MAX];
    char list[256];
    if (why_size) why[0] = 0;
    if (index >= 0 && index < SNX_PROGRAMS_MAX && snx_program_played(folder, name, index, out, size)) return index;
    snx_game_programs(folder, name, made);
    programs_text(made, list, sizeof list);
    snprintf(why, why_size, "program %d is not one of this application's programs %s: it starts program 0", index,
             list[0] ? list : "(none made)");
    snx_program_played(folder, name, 0, out, size);
    return 0;
}

char* snx_next_path(const char* folder, const char* name) { return snx_format("%s/%s.next", folder, name); }
int snx_next_write(const char* path, const SnxNext* next) {
    static const char digits[] = "0123456789abcdef";
    size_t total = 128 + strlen(next->program), i, j, n;
    char *text, *tmp;
    int rc;
    for (i = 0; i < next->count; i++) total += 16 + 2 * next->size[i];
    text = (char*)snx_alloc(total);
    n = (size_t)snprintf(text, total, "program=%s\nindex=%d\nprevious=%d\ncontext=%d\nserial=%lu\n", next->program,
                         next->index, next->previous, next->context, next->serial);
    for (i = 0; i < next->count; i++) {
        memcpy(text + n, "storage=", 8);
        n += 8;
        for (j = 0; j < next->size[i]; j++) {
            text[n++] = digits[next->storage[i][j] >> 4];
            text[n++] = digits[next->storage[i][j] & 15];
        }
        text[n++] = '\n';
    }
    tmp = snx_format("%s.tmp", path);
    rc = snx_write_file(tmp, text, n) || plat_replace_file(tmp, path);
    if (rc) {
        snx_error("cannot write %s, the hand-over to the program %s", path, next->program);
        if (snx_is_file(tmp)) snx_remove_tree(tmp);
    }
    free(tmp);
    free(text);
    return rc;
}
static int hex_digit(char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}
/* a whole number from 0 (from -1 when negative is set) to max, all of text: 1 with it in *value, else 0 */
static int whole(const char* text, int negative, int max, int* value) {
    char* end;
    long v;
    if (!*text) return 0;
    v = strtol(text, &end, 10);
    if (*end || v < (negative ? -1 : 0) || v > max) return 0;
    *value = (int)v;
    return 1;
}
int snx_next_read(const char* path, SnxNext* next, char* why, size_t why_size) {
    size_t size, line = 0;
    char *text = (char*)snx_read_file(path, &size), *at, *end, *eq;
    int fields = 0;
    memset(next, 0, sizeof *next);
    if (why_size) why[0] = 0;
    if (!text) return 0;
    for (at = text; at < text + size; at = end + 1) {
        end = memchr(at, '\n', (size_t)(text + size - at));
        if (!end) end = text + size;
        *end = 0;
        line++;
        if (!*at) continue;
        eq = strchr(at, '=');
        if (!eq) {
            snprintf(why, why_size, "%s line %zu, %.40s, is no key=value", path, line, at);
            break;
        }
        *eq++ = 0;
        if (!strcmp(at, "program") && *eq && strlen(eq) < sizeof next->program && !strpbrk(eq, "/\\")) {
            memcpy(next->program, eq, strlen(eq) + 1);
            fields |= 1;
        } else if (!strcmp(at, "index") && whole(eq, 0, SNX_PROGRAMS_MAX - 1, &next->index))
            fields |= 2;
        else if (!strcmp(at, "previous") && whole(eq, 1, SNX_PROGRAMS_MAX - 1, &next->previous))
            fields |= 4;
        else if (!strcmp(at, "context") && whole(eq, 0, 1, &next->context))
            fields |= 8;
        else if (!strcmp(at, "serial") && *eq >= '1' && *eq <= '9' && strspn(eq, "0123456789") == strlen(eq) &&
                 strlen(eq) < 10) {
            next->serial = strtoul(eq, NULL, 10);
            fields |= 16;
        } else if (!strcmp(at, "storage")) {
            size_t digits = strlen(eq), k;
            unsigned char* bytes;
            if (next->count == SNX_NEXT_STORAGES || digits % 2 || digits / 2 > SNX_NEXT_STORAGE_MAX) {
                snprintf(why, why_size,
                         "%s line %zu: a storage of %zu hex digits, past the %d storages of %d bytes "
                         "at most or not whole bytes",
                         path, line, digits, SNX_NEXT_STORAGES, SNX_NEXT_STORAGE_MAX);
                break;
            }
            bytes = (unsigned char*)snx_alloc(digits / 2 + 1);
            for (k = 0; k < digits / 2; k++) {
                int hi = hex_digit(eq[2 * k]), lo = hex_digit(eq[2 * k + 1]);
                if (hi < 0 || lo < 0) break;
                bytes[k] = (unsigned char)(hi << 4 | lo);
            }
            if (k < digits / 2) {
                free(bytes);
                snprintf(why, why_size, "%s line %zu: storage byte %zu, %.2s, is not hex", path, line, k, eq + 2 * k);
                break;
            }
            next->storage[next->count] = bytes;
            next->size[next->count++] = digits / 2;
        } else {
            snprintf(why, why_size,
                     "%s line %zu: %s=%.40s is none of program=, index= (0 to %d), previous= (-1 to %d), "
                     "context= (0 or 1), serial= (1 to 999999999) and storage=",
                     path, line, at, eq, SNX_PROGRAMS_MAX - 1, SNX_PROGRAMS_MAX - 1);
            break;
        }
    }
    free(text);
    if (!why[0] && fields != 31)
        snprintf(why, why_size, "%s lacks %s", path,
                 !(fields & 1)   ? "program="
                 : !(fields & 2) ? "index="
                 : !(fields & 4) ? "previous="
                 : !(fields & 8) ? "context="
                                 : "serial=");
    if (!why[0]) return 1;
    snx_next_free(next);
    return -1;
}
void snx_next_free(SnxNext* next) {
    size_t i;
    for (i = 0; i < next->count; i++) free(next->storage[i]);
    memset(next, 0, sizeof *next);
}
