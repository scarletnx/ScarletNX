/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/error_kind.h"
#include "common/utilities/common.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* each kind's name, its panel's line and second line, and the actions that fix it, the first the one in focus */
typedef struct {
    const char *name, *line, *hint;
    int fix[3];
} Kind;
static const Kind g_kinds[SNX_ERROR_KINDS] = {
    [SNX_ERROR_INTERNAL] = {"internal",
                            "ScarletNX hit an error of its own.",
                            "Share Log sends it to us.",
                            {SNX_ACTION_SHARE_LOG}},
    [SNX_ERROR_SETTING] = {"setting",
                           "A setting games need is off.",
                           "Turn it on: Open Settings.",
                           {SNX_ACTION_OPEN_SETTINGS, SNX_ACTION_TRY_AGAIN, SNX_ACTION_SHARE_LOG}},
    [SNX_ERROR_SETTING_VALUE] = {"setting-value",
                                 "A setting has a value it can't take.",
                                 "Pick another: Open Settings.",
                                 {SNX_ACTION_OPEN_SETTINGS, SNX_ACTION_SHARE_LOG}},
    [SNX_ERROR_SDK32] = {"sdk32",
                         "This version does not run yet.",
                         "Its update may: Updates and DLCs.",
                         {SNX_ACTION_ADDONS, SNX_ACTION_SHARE_LOG}},
    [SNX_ERROR_NOT_YET] = {"not-yet",
                           "This game needs what we lack yet.",
                           "Share Log tells us what.",
                           {SNX_ACTION_SHARE_LOG}},
    [SNX_ERROR_GAME_STOPPED] = {"game-stopped",
                                "The game stopped itself.",
                                "Share Log tells us why.",
                                {SNX_ACTION_SHARE_LOG, SNX_ACTION_TRY_AGAIN}},
    [SNX_ERROR_CRASH] = {"crash",
                         "The game crashed.",
                         "Share Log tells us where.",
                         {SNX_ACTION_SHARE_LOG, SNX_ACTION_TRY_AGAIN}},
    [SNX_ERROR_GPU_OLD] = {"gpu-old",
                           "This GPU has no Vulkan 1.3.",
                           "Another driver may: GPU Drivers.",
                           {SNX_ACTION_GPU_DRIVERS, SNX_ACTION_SHARE_LOG}},
    [SNX_ERROR_GPU_START] = {"gpu-start",
                             "The GPU driver could not start.",
                             "Another driver may: GPU Drivers.",
                             {SNX_ACTION_GPU_DRIVERS, SNX_ACTION_TRY_AGAIN, SNX_ACTION_SHARE_LOG}},
    [SNX_ERROR_GPU_LOST] = {"gpu-lost",
                            "The GPU stopped answering.",
                            "Its driver reset it.",
                            {SNX_ACTION_TRY_AGAIN, SNX_ACTION_GPU_DRIVERS, SNX_ACTION_SHARE_LOG}},
    [SNX_ERROR_MEMORY] = {"memory",
                          "Not enough memory.",
                          "Close other apps, then try again.",
                          {SNX_ACTION_TRY_AGAIN, SNX_ACTION_SHARE_LOG}},
    [SNX_ERROR_CODE_OLD] = {"code-old",
                            "The game must be recompiled.",
                            "This ScarletNX made other code.",
                            {SNX_ACTION_RECOMPILE, SNX_ACTION_SHARE_LOG}},
    [SNX_ERROR_CODE_MISSING] = {"code-missing",
                                "The recompiled code is not there.",
                                "Recompile it from its dump.",
                                {SNX_ACTION_RECOMPILE}},
    [SNX_ERROR_DUMP_MISSING] = {"dump-missing",
                                "The game's dump is not there.",
                                "Locate it, or play it again.",
                                {SNX_ACTION_LOCATE}},
    [SNX_ERROR_KEYS] = {"keys",
                        "Keys for this game are missing.",
                        "Install them: Install Keys.",
                        {SNX_ACTION_INSTALL_KEYS}},
    [SNX_ERROR_DISK] = {"disk",
                        "The disk is full.",
                        "Free some room, then try again.",
                        {SNX_ACTION_OPEN_FOLDER, SNX_ACTION_TRY_AGAIN}},
    [SNX_ERROR_SAVE] = {"save",
                        "The save could not be read.",
                        "Its folder: Open Folder.",
                        {SNX_ACTION_OPEN_FOLDER, SNX_ACTION_SHARE_LOG}},
    [SNX_ERROR_NETWORK] = {"network",
                           "The network did not answer.",
                           "Check it: Open Settings.",
                           {SNX_ACTION_OPEN_SETTINGS, SNX_ACTION_TRY_AGAIN}},
    [SNX_ERROR_KILLED] = {"killed",
                          "The system stopped the game.",
                          "Close other apps, then try again.",
                          {SNX_ACTION_TRY_AGAIN, SNX_ACTION_SHARE_LOG}},
    [SNX_ERROR_BAD_FILE] = {"bad-file", "ScarletNX cannot open this file.", "Details says why.", {SNX_ACTION_NONE}},
    [SNX_ERROR_RECOMPILE] = {"recompile",
                             "The recompile failed.",
                             "Share Log tells us why.",
                             {SNX_ACTION_TRY_AGAIN, SNX_ACTION_SHARE_LOG}},
};
static const char* const g_action_names[SNX_ACTIONS] = {
    "none",      "open-settings", "gpu-drivers", "addons", "recompile", "locate",    "install-keys", "open-folder",
    "try-again", "share-log",     "details",     "close",  "open-log",  "copy-text", "back"};
static const char* const g_action_labels[SNX_ACTIONS] = {
    "",          "Open Settings", "GPU Drivers", "Updates and DLCs", "Recompile",
    "Locate...", "Install Keys",  "Open Folder", "Try Again",        "Share Log",
    "Details",   "Close",         "Open Log",    "Copy Text",        "Back"};

const char* snx_error_kind_name(int kind) { return kind >= 0 && kind < SNX_ERROR_KINDS ? g_kinds[kind].name : "?"; }
int snx_error_kind_of(const char* name) {
    for (int k = 0; k < SNX_ERROR_KINDS; ++k)
        if (!strcmp(g_kinds[k].name, name)) return k;
    return -1;
}
const char* snx_error_action_name(int action) {
    return action >= 0 && action < SNX_ACTIONS ? g_action_names[action] : "?";
}
int snx_error_action_of(const char* name) {
    for (int a = 0; a < SNX_ACTIONS; ++a)
        if (!strcmp(g_action_names[a], name)) return a;
    return -1;
}
const char* snx_error_action_label(int action, const SnxError* e) {
    if (action == SNX_ACTION_CLOSE && e && e->notice) return "OK";
    if (action == SNX_ACTION_CLOSE && e && e->from_launcher) return "Back to Menu";
    return action >= 0 && action < SNX_ACTIONS ? g_action_labels[action] : "?";
}
static const Kind* kind_of(const SnxError* e) {
    return &g_kinds[e->kind >= 0 && e->kind < SNX_ERROR_KINDS ? e->kind : SNX_ERROR_INTERNAL];
}
const char* snx_error_line(const SnxError* e) { return e->line[0] ? e->line : kind_of(e)->line; }
const char* snx_error_hint(const SnxError* e) { return e->hint[0] ? e->hint : kind_of(e)->hint; }

/* whether e has what action needs to be done */
static int can(const SnxError* e, int action) {
    switch (action) {
        case SNX_ACTION_OPEN_SETTINGS: return e->key[0] != 0;
        case SNX_ACTION_ADDONS:
        case SNX_ACTION_RECOMPILE:
        case SNX_ACTION_LOCATE: return e->program[0] != 0;
        case SNX_ACTION_OPEN_FOLDER: return e->folder[0] != 0;
        case SNX_ACTION_TRY_AGAIN: return e->program[0] || e->from_launcher;
        case SNX_ACTION_SHARE_LOG: return e->log[0] != 0;
        case SNX_ACTION_NONE: return 0;
        default: return 1;
    }
}
int snx_error_actions(const SnxError* e, int out[SNX_ERROR_ACTIONS_MAX]) {
    const Kind* k = kind_of(e);
    int n = 0;
    if (e->notice) { /* nothing of a game's to fix */
        out[0] = SNX_ACTION_CLOSE;
        return 1;
    }
    for (size_t i = 0; i < sizeof k->fix / sizeof k->fix[0]; ++i)
        if (can(e, k->fix[i])) out[n++] = k->fix[i];
    out[n++] = SNX_ACTION_DETAILS;
    out[n++] = SNX_ACTION_CLOSE;
    return n;
}

int snx_error_kind_of_message(const char* message) {
    return strstr(message, "out of memory") || strstr(message, "Out of memory") || strstr(message, "no memory") ||
                   strstr(message, "No memory")
               ? SNX_ERROR_MEMORY
               : SNX_ERROR_INTERNAL;
}

/* ---- the record's file ---- */
/* its keys, in the order written; each a text field of SnxError but kind and from_launcher */
typedef struct {
    const char* key;
    size_t at, size; /* the field's offset in SnxError and its bytes; size 0 for an int */
} Field;
#define TEXT(name) {#name, offsetof(SnxError, name), sizeof((SnxError){0}.name)}
static const Field g_fields[] = {
    {"kind", offsetof(SnxError, kind), 0},
    {"from_launcher", offsetof(SnxError, from_launcher), 0},
    TEXT(key),
    TEXT(line),
    TEXT(hint),
    TEXT(message),
    TEXT(game),
    TEXT(program),
    TEXT(folder),
    TEXT(log),
};
#undef TEXT

/* value into out from n as a line of the file, its line breaks as spaces: the length after it, or size when it does
   not fit */
static size_t put_value(char* out, size_t n, size_t size, const char* value) {
    for (; n < size && *value; ++value) out[n++] = *value == '\n' || *value == '\r' ? ' ' : *value;
    return *value ? size : n;
}
size_t snx_error_text(const SnxError* e, char* out, size_t size) {
    size_t n = 0;
    for (size_t i = 0; i < sizeof g_fields / sizeof g_fields[0] && n < size; ++i) {
        const Field* f = &g_fields[i];
        const char* value;
        if (!f->size) {
            int v = *(const int*)(const void*)((const char*)e + f->at);
            value = i == 0 ? snx_error_kind_name(v) : v ? "1" : "0";
        } else
            value = (const char*)e + f->at;
        n = put_value(out, n, size, f->key);
        n = put_value(out, n, size, "=");
        n = put_value(out, n, size, value);
        if (n < size) out[n++] = '\n'; /* the line's end, which a value's breaks are not */
    }
    if (n >= size) return 0;
    out[n] = 0;
    return n;
}
int snx_error_write(const SnxError* e, const char* path, char* why, size_t why_size) {
    char text[sizeof(SnxError) + 256];
    size_t n = snx_error_text(e, text, sizeof text);
    if (!n || snx_write_file(path, text, n)) {
        char os[256];
        snprintf(why, why_size, "%s could not be written: %s", path, snx_os_error(os, sizeof os));
        return 1;
    }
    return 0;
}
int snx_error_read(SnxError* e, const char* path, char* why, size_t why_size) {
    size_t size = 0;
    char* text = (char*)snx_read_file(path, &size);
    int failed = 0;
    *e = (SnxError){0};
    if (!text) {
        char os[256];
        snprintf(why, why_size, "%s could not be read: %s", path, snx_os_error(os, sizeof os));
        return 1;
    }
    for (char *line = text, *next; !failed && line < text + size && *line; line = next) {
        char* eq;
        size_t i;
        next = strchr(line, '\n');
        next = next ? next + 1 : text + size;
        if (next > line && next[-1] == '\n') next[-1] = 0;
        if (next - 1 > line && next[-2] == '\r') next[-2] = 0;
        if (!*line) continue;
        eq = strchr(line, '=');
        for (i = 0; eq && i < sizeof g_fields / sizeof g_fields[0]; ++i)
            if ((size_t)(eq - line) == strlen(g_fields[i].key) && !strncmp(line, g_fields[i].key, (size_t)(eq - line)))
                break;
        if (!eq || i == sizeof g_fields / sizeof g_fields[0]) {
            snprintf(why, why_size, "%s: \"%.64s\" is no key of an error's record", path, line);
            failed = 1;
        } else if (!g_fields[i].size) {
            int v = i == 0 ? snx_error_kind_of(eq + 1) : !strcmp(eq + 1, "1") ? 1 : !strcmp(eq + 1, "0") ? 0 : -1;
            if (v < 0) {
                snprintf(why, why_size, "%s: %s=%.64s is no %s", path, g_fields[i].key, eq + 1,
                         i == 0 ? "kind of error" : "0 or 1");
                failed = 1;
            } else
                *(int*)(void*)((char*)e + g_fields[i].at) = v;
        } else if (strlen(eq + 1) >= g_fields[i].size) {
            snprintf(why, why_size, "%s: %s is %zu bytes, past the %zu its field holds", path, g_fields[i].key,
                     strlen(eq + 1), g_fields[i].size - 1);
            failed = 1;
        } else
            memcpy((char*)e + g_fields[i].at, eq + 1, strlen(eq + 1) + 1);
    }
    free(text);
    return failed;
}

/* ---- the run's "error: " lines ---- */
void snx_error_said(const SnxError* e, int shown, char* out, size_t size) {
    snprintf(out, size, SNX_ERROR_SAID "%s shown=%d%s%s", snx_error_kind_name(e->kind), shown ? 1 : 0,
             e->key[0] ? " key=" : "", e->key);
}
void snx_error_action_said(const SnxError* e, int action, char* out, size_t size) {
    snprintf(out, size, SNX_ERROR_SAID "action %s%s%s", snx_error_action_name(action), e->key[0] ? " key=" : "",
             e->key);
}
int snx_error_heard(const char* line, SnxErrorHeard* heard) {
    char word[64];
    size_t n;
    *heard = (SnxErrorHeard){.kind = -1};
    if (strncmp(line, SNX_ERROR_SAID, strlen(SNX_ERROR_SAID))) return 0;
    line += strlen(SNX_ERROR_SAID);
    if (!strncmp(line, "action ", 7)) {
        line += 7;
        n = strcspn(line, " \r\n");
        snprintf(word, sizeof word, "%.*s", (int)(n < sizeof word ? n : sizeof word - 1), line);
        heard->action = snx_error_action_of(word);
        if (heard->action <= SNX_ACTION_NONE) {
            snprintf(heard->why, sizeof heard->why, "\"%.64s\" is no action of the error's panel", word);
            return -1;
        }
    } else {
        n = strcspn(line, " \r\n");
        snprintf(word, sizeof word, "%.*s", (int)(n < sizeof word ? n : sizeof word - 1), line);
        heard->kind = snx_error_kind_of(word);
        if (heard->kind < 0) {
            snprintf(heard->why, sizeof heard->why, "\"%.64s\" is no kind of error", word);
            return -1;
        }
    }
    line += n;
    while (*line == ' ') {
        ++line;
        n = strcspn(line, " \r\n");
        if (!strncmp(line, "shown=", 6) && n == 7 && (line[6] == '0' || line[6] == '1'))
            heard->shown = line[6] == '1';
        else if (!strncmp(line, "key=", 4) && n > 4 && n - 4 < sizeof heard->key)
            snprintf(heard->key, sizeof heard->key, "%.*s", (int)(n - 4), line + 4);
        else {
            snprintf(heard->why, sizeof heard->why, "\"%.*s\" is no part of an \"error: \" line",
                     (int)(n < 64 ? n : 64), line);
            return -1;
        }
        line += n;
    }
    return 1;
}

/* the console's stop noted last (snx_error_note), and whether nobody is at its window */
static struct {
    SnxError e;
    int noted, quiet, from_launcher;
} g_error_note;
void snx_error_note(int kind, const char* program, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_error_note.e.message, sizeof g_error_note.e.message, fmt, ap);
    va_end(ap);
    g_error_note.e.kind = kind >= 0 && kind < SNX_ERROR_KINDS ? kind : SNX_ERROR_INTERNAL;
    g_error_note.noted = 1;
    fprintf(stderr, "%s: %s\n", program, g_error_note.e.message);
}
void snx_error_note_game(const char* game) { snprintf(g_error_note.e.game, sizeof g_error_note.e.game, "%s", game); }
void snx_error_note_quiet(int quiet) { g_error_note.quiet = quiet; }
const SnxError* snx_error_noted(void) {
    g_error_note.e.from_launcher = g_error_note.from_launcher;
    return g_error_note.noted && !g_error_note.quiet ? &g_error_note.e : NULL;
}
void snx_error_launched(int from_launcher) { g_error_note.from_launcher = from_launcher; }
int snx_error_from_launcher(void) { return g_error_note.from_launcher; }

void snx_error_open_value(const SnxError* e, int action, char* out, size_t size) {
    switch (action) {
        case SNX_ACTION_OPEN_SETTINGS: snprintf(out, size, "settings:%s", e->key); break;
        case SNX_ACTION_GPU_DRIVERS: snprintf(out, size, "drivers"); break;
        case SNX_ACTION_INSTALL_KEYS: snprintf(out, size, "keys"); break;
        case SNX_ACTION_ADDONS: snprintf(out, size, "addons:%s", e->program); break;
        case SNX_ACTION_RECOMPILE: snprintf(out, size, "recompile:%s", e->program); break;
        case SNX_ACTION_LOCATE: snprintf(out, size, "locate:%s", e->program); break;
        default:
            if (size) out[0] = 0;
            break;
    }
}
