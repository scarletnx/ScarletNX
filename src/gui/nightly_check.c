/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/nightly_check.h"
#include "gui/i18n.h"
#include <yyjson.h>
#include <stdio.h>
#include <string.h>

/* s starts with a day, YYYY-MM-DD */
static int is_day(const char* s) {
    static const char shape[] = "0000-00-00";
    for (int i = 0; i < 10; ++i)
        if (shape[i] == '0' ? s[i] < '0' || s[i] > '9' : s[i] != shape[i]) return 0;
    return 1;
}
static int is_hex7(const char* s) {
    for (int i = 0; i < 7; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}
int gui_nightly_newer(const char* tag, const char* branch, const char* commit, const char* committed) {
    bool nightly = branch && strlen(branch) == 18 && !strncmp(branch, "nightly-", 8) && is_day(branch + 8);
    const char* day = nightly ? branch + 8 : committed;
    if (!tag || strlen(tag) != 26 || strncmp(tag, "nightly-", 8) || !is_day(tag + 8) || tag[18] != '-' ||
        !is_hex7(tag + 19))
        return GUI_NIGHTLY_NO_TAG;
    if (!day || strlen(day) != 10 || !is_day(day)) return GUI_NIGHTLY_NO_DAY;
    if (commit && strlen(commit) >= 7 && !strncmp(commit, tag + 19, 7)) return 0; /* this build's own commit */
    return nightly ? strncmp(tag + 8, day, 10) >= 0 : strncmp(tag + 8, day, 10) > 0;
}

static void text_of(yyjson_val* o, const char* key, char* out, size_t size) {
    const char* v = yyjson_get_str(yyjson_obj_get(o, key));
    snprintf(out, size, "%s", v ? v : "");
}
/* a line of the notes as a notice shows it: markdown's escapes and links taken out, a link's text kept */
static void plain(const char* line, size_t n, char* out, size_t size) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < size; ++i) {
        char c = line[i];
        if (c == '\\' && i + 1 < n) { /* an escaped letter, as itself */
            out[o++] = line[++i];
        } else if (c == ']' && i + 1 < n && line[i + 1] == '(') { /* a link's end: its target left out */
            while (i < n && line[i] != ')') ++i;
        } else if (c != '[' && c != '*' && c != '`')
            out[o++] = c;
    }
    out[o] = 0;
}
/* the notes' changes: each line of its Changes section, under the area its bold line names, up to
   GUI_NIGHTLY_CHANGES, the rest counted, with those its "And N more" says */
static void changes_of(const char* body, GuiNightly* out) {
    const char* at = body ? strstr(body, "\n## Changes") : NULL;
    char area[64] = "";
    if (!at) return;
    at = strchr(at + 1, '\n');
    while (at && *at) {
        const char* line = at + 1;
        size_t n = strcspn(line, "\r\n");
        unsigned more = 0;
        at = line[n] ? line + n : NULL;
        if (!strncmp(line, "## ", 3) || !strncmp(line, "<details>", 9)) break;
        if (n > 4 && line[0] == '*' && line[1] == '*' && line[n - 1] == '*') {
            plain(line, n, area, sizeof area);
        } else if (n > 2 && line[0] == '-' && line[1] == ' ') {
            if (out->count < GUI_NIGHTLY_CHANGES) {
                char text[160];
                plain(line + 2, n - 2, text, sizeof text);
                snprintf(out->changes[out->count++], sizeof out->changes[0], "%s%s%s%s", area[0] ? "[" : "", area,
                         area[0] ? "] " : "", text);
            } else
                ++out->more;
        } else if (sscanf(line, "And [%u more]", &more) == 1)
            out->more += (int)more;
    }
}
int gui_nightly_parse(const char* json, size_t n, GuiNightly* out, char* why, size_t size) {
    yyjson_doc* doc = yyjson_read(json, n, 0);
    yyjson_val* root = doc ? yyjson_doc_get_root(doc) : NULL;
    *out = (GuiNightly){0};
    if (!yyjson_is_obj(root)) {
        snprintf(why, size, snx_tr("GitHub's answer is no JSON object (%zu bytes)"), n);
        yyjson_doc_free(doc);
        return 0;
    }
    text_of(root, "tag_name", out->tag, sizeof out->tag);
    if (!out->tag[0]) {
        const char* message = yyjson_get_str(yyjson_obj_get(root, "message"));
        snprintf(why, size, snx_tr("GitHub names no release: %s"), message ? message : snx_tr("its answer has no tag"));
        yyjson_doc_free(doc);
        return 0;
    }
    text_of(root, "name", out->name, sizeof out->name);
    text_of(root, "html_url", out->url, sizeof out->url);
    changes_of(yyjson_get_str(yyjson_obj_get(root, "body")), out);
    yyjson_doc_free(doc);
    return 1;
}
