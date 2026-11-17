/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The .ini reader the game's settings and the GUI's themes share (src/common/utilities/ini_lines.c) */
#include "common/utilities/ini_lines.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* each key of text as section/key=value, one a line */
static void read_all(const char* text, size_t size, char* out, size_t room) {
    static char copy[512];
    SnxIni ini;
    char *key, *value;
    size_t used = 0;
    assert(size < sizeof copy);
    memcpy(copy, text, size);
    copy[size] = 0;
    out[0] = 0;
    snx_ini_start(&ini, copy);
    while (snx_ini_next(&ini, &key, &value)) {
        int n = sprintf(out + used, "%s/%s=%s\n", ini.section, key, value);
        assert(n > 0 && used + (size_t)n < room);
        used += (size_t)n;
    }
}
#define CHECK(text, expect)                                                                                            \
    do {                                                                                                               \
        char got[512];                                                                                                 \
        read_all(text, sizeof text - 1, got, sizeof got);                                                              \
        if (strcmp(got, expect)) {                                                                                     \
            printf("%s\n--- read as ---\n%s--- not ---\n%s", text, got, expect);                                       \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

/* the lines the reading leaves, as the caller is told them: line: text: what that does */
static char g_left[512];
static void left(void* ctx, int line, const char* text, const char* does) {
    size_t used = strlen(g_left);
    assert(ctx == g_left);
    snprintf(g_left + used, sizeof g_left - used, "%d: %s: %s\n", line, text, does);
}
static int check_left(void) {
    static char text[] =
        "a=1\n\nnothing here\n[open\nb=2\n[0123456789abcdef0123456789abcdef]\nc=3\n; note\n[ok]\nd=4\n";
    static const char expect[] =
        "3: nothing here: has no = and sets nothing\n"
        "4: [open: opens a section with no ]: the keys after it are read as of no section\n"
        "6: [0123456789abcdef0123456789abcdef]: names a section longer than 31 bytes: the keys after it are read as "
        "of no section\n";
    SnxIni ini;
    char *key, *value;
    snx_ini_start(&ini, text);
    ini.left = left;
    ini.ctx = g_left;
    while (snx_ini_next(&ini, &key, &value)) {}
    if (strcmp(g_left, expect)) {
        printf("the lines left, told as\n%s--- not ---\n%s", g_left, expect);
        return 1;
    }
    return 0;
}

int main(void) {
    if (check_left()) return 1;
    /* keys before a section, sections, comments, blank lines, a line without = */
    CHECK("a = 1\n[one]\n; note\n# note\n\nnothing here\nb=2\n[two]\nc= 3 ", "/a=1\none/b=2\ntwo/c=3\n");
    /* spaces, tabs and a last CR around a key and its value; = inside a value; quotes are the caller's */
    CHECK("\t key \t=\t \"x = y\" \r\n", "/key=\"x = y\"\n");
    CHECK("k=\r\n=v\n", "/k=\n/=v\n");
    /* a CR inside a value stays: only the line's last one goes */
    CHECK("k=a\rb\r\n", "/k=a\rb\n");
    /* a UTF-8 mark at the start is left out, anywhere else it is text */
    CHECK("\xEF\xBB\xBF[s]\nk=v\n\xEF\xBB\xBFx=y\n", "s/k=v\ns/\xEF\xBB\xBFx=y\n");
    /* [ without ], and a name of 32 bytes or more: the keys after it are in no section */
    CHECK("[s]\n[open\nk=1\n[s]\n[0123456789abcdef0123456789abcdef]\nk=2\n[0123456789abcdef0123456789abcde]\nk=3\n",
          "/k=1\n/k=2\n0123456789abcdef0123456789abcde/k=3\n");
    /* the text ends at its first NUL */
    CHECK("a=1\nb=2\0c=3\n", "/a=1\n/b=2\n");
    CHECK("", "");
    puts("ini_lines passed");
    return 0;
}
