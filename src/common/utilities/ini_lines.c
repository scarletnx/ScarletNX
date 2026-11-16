/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/ini_lines.h"
#include <stdio.h>
#include <string.h>

static char* trim(char* s) {
    while (*s == ' ' || *s == '\t') ++s;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
    return s;
}

void snx_ini_start(SnxIni* ini, char* text) {
    ini->at = strncmp(text, "\xEF\xBB\xBF", 3) ? text : text + 3;
    ini->section[0] = 0;
    ini->line = 0;
    ini->left = NULL;
    ini->ctx = NULL;
}
static void left(const SnxIni* ini, const char* text, const char* does) {
    if (ini->left) ini->left(ini->ctx, ini->line, text, does);
}

int snx_ini_next(SnxIni* ini, char** key, char** value) {
    while (ini->at) {
        char* s = ini->at;
        ini->at = strchr(s, '\n');
        if (ini->at) *ini->at++ = 0;
        ini->line++;
        s = trim(s);
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') {
            char* end = strchr(s, ']');
            size_t n = end ? (size_t)(end - s - 1) : sizeof ini->section;
            if (n >= sizeof ini->section) {
                char longer[112];
                snprintf(longer, sizeof longer,
                         "names a section longer than %u bytes: the keys after it are read as of no section",
                         (unsigned)sizeof ini->section - 1);
                left(ini, s, end ? longer : "opens a section with no ]: the keys after it are read as of no section");
                n = 0;
            }
            memcpy(ini->section, s + 1, n);
            ini->section[n] = 0;
            continue;
        }
        char* equals = strchr(s, '=');
        if (!equals) {
            left(ini, s, "has no = and sets nothing");
            continue;
        }
        *equals = 0;
        *key = trim(s);
        *value = trim(equals + 1);
        return 1;
    }
    return 0;
}
