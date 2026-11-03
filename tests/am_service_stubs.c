/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* the game's data folder for am.c's tests: a control.nacp only when a test gives one; and the first user alone, as
   users.c has it without users.ini */
#include "nx/hle/filesystem/fs.h"
#include "nx/hle/services/services_int.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const u8* g_test_nacp;
extern u64 g_test_nacp_size;
const u8* g_test_nacp;
u64 g_test_nacp_size;

/* apm's side of the state getter's performance commands (misc.c): what SetCpuBoostMode last handed it */
extern u32 g_test_apm_boost;
u32 g_test_apm_boost = 0xEEEEu;
u32 apm_configuration(u32 mode) { return mode ? 0x92220009u : 0x00020003u; }
void apm_set_configuration(u32 mode, u32 config) {
    (void)mode;
    (void)config;
}
void apm_cpu_boost(u32 boost) { g_test_apm_boost = boost; }

/* the game's HtmlDocument content: a page that loads its script beside it, whose A button goes to a callback URL
   (its B button, assigned first, to another), and a page that assigns no A button */
u8* fs_html_document(const char* path, u64* size) {
    static const char* const files[][2] = {
        {"pages.htdocs/index.html", "<html><head><script src=\"main.js\"></script></head><body>Text</body></html>"},
        {"pages.htdocs/main.js",
         "var goBack = function() {\n  location.href = \"http://localhost/?ret=No\";\n};\n"
         "function goOn() { location.href = \"http://localhost/?ret=Yes\"; }\n"
         "window.nx.footer.setAssign('B', 'Back', goBack, {});\n"
         "window.nx.footer.setAssign('A', 'OK', goOn, {});\n"},
        {"plain.htdocs/index.html", "<html><script>window.nx.footer.setAssign('B', 'Back', x, {});</script></html>"}};
    size_t i;
    for (i = 0; i < sizeof files / sizeof files[0]; i++)
        if (!strcmp(path, files[i][0])) {
            size_t n = strlen(files[i][1]);
            u8* d = malloc(n);
            if (!d) return NULL;
            memcpy(d, files[i][1], n);
            *size = n;
            return d;
        }
    return NULL;
}

u8* fs_read_file(const char* path, u64* size) {
    u8* d;
    if (size) *size = 0;
    if (strcmp(path, "control.nacp") || !g_test_nacp) return NULL;
    d = malloc((size_t)g_test_nacp_size);
    if (!d) return NULL;
    memcpy(d, g_test_nacp, (size_t)g_test_nacp_size);
    if (size) *size = g_test_nacp_size;
    return d;
}

/* the users' array, which am reads only through acc_playing, under acc's lock: an answer of Player or of the fixed uid
   says it read the array unlocked */
const SnxUser* snx_users_selected(void) {
    static SnxUser first = {.nickname = "Player"};
    memcpy(first.uid, g_user_id, 16);
    return &first;
}
const u64 test_playing_uid[2] = {0x1111222233334444ull, 0x5555666677778888ull};
/* not users.c's "Player": a keyboard that answers it says which */
void acc_playing(u64 uid[2], char* nickname, size_t size) {
    if (uid) memcpy(uid, test_playing_uid, sizeof test_playing_uid);
    if (size) snprintf(nickname, size, "%s", "Nadia");
}
