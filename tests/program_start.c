/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The rows of a program a collection's program started (common/utilities/program_start.c), which the runtime's
   loading screen and the launcher's run screen draw: each fact said, and each one not known said as "?", never a
   value in its place; a row a screen can't say (what was handed over, a recompile) left out. The title ID is a made
   up one of no game */
#include "common/utilities/program_start.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct {
    SnxStartRow row[SNX_START_ROWS];
    int count;
} g_rows; /* the rows made last */
static const char* value(const char* label) {
    for (int i = 0; i < g_rows.count; ++i)
        if (!strcmp(g_rows.row[i].label, label)) return g_rows.row[i].value;
    return NULL;
}
static void make(const SnxProgramStart* f) { g_rows.count = snx_program_start_rows(f, g_rows.row); }
static SnxProgramStart known(void) {
    SnxProgramStart f;
    snx_program_start_unknown(&f);
    f.program = 1;
    f.programs = 4;
    f.previous = 0;
    snprintf(f.user, sizeof f.user, "%s", "Player");
    f.user_number = 1;
    f.user_kept = 1;
    snprintf(f.version, sizeof f.version, "%s", "1.2.1");
    f.updated = 1;
    snprintf(f.update, sizeof f.update, "%s", "v196608");
    f.dlcs = 2;
    snprintf(f.dlc, sizeof f.dlc, "%s", "DLC 1, DLC 2");
    f.storages = 1;
    f.storage_bytes = 16384;
    snprintf(f.gpu, sizeof f.gpu, "%s", "A GPU");
    f.title_id = 0x0100000000010001ull;
    snprintf(f.build, sizeof f.build, "%s", "ScarletNX main abc1234");
    f.recompile = 0;
    return f;
}

int main(void) {
    static const char* const order[] = {"PROGRAM",     "STARTED BY", "USER",     "VERSION", "DLC",
                                        "HANDED OVER", "GPU",        "TITLE ID", "BUILD",   "RECOMPILE"};
    SnxProgramStart f = known();
    int i;
    /* every fact known: every row, in its order */
    make(&f);
    assert(g_rows.count == SNX_START_ROWS);
    for (i = 0; i < g_rows.count; ++i) assert(!strcmp(g_rows.row[i].label, order[i]));
    assert(!strcmp(value("PROGRAM"), "1 of 4"));
    assert(!strcmp(value("STARTED BY"), "program 0"));
    assert(!strcmp(value("USER"), "Player (user 1), kept from program 0"));
    assert(!strcmp(value("VERSION"), "1.2.1 (update v196608)"));
    assert(!strcmp(value("DLC"), "DLC 1, DLC 2"));
    assert(!strcmp(value("HANDED OVER"), "1 storage, 16 KB"));
    assert(!strcmp(value("GPU"), "A GPU"));
    assert(!strcmp(value("TITLE ID"), "0100000000010001"));
    assert(!strcmp(value("BUILD"), "ScarletNX main abc1234"));
    assert(!strcmp(value("RECOMPILE"), "not needed, program 1 is made"));
    /* the same program again (RestartProgram), a user picked again, no update, no DLC, nothing handed over, two
       storages of 1 byte (rounded up to a KB) */
    f.previous = 1;
    f.user_kept = 0;
    f.updated = 0;
    f.dlcs = 0;
    f.storages = 0;
    make(&f);
    assert(!strcmp(value("STARTED BY"), "program 1 (restarted)"));
    assert(!strcmp(value("USER"), "Player (user 1)"));
    assert(!strcmp(value("VERSION"), "1.2.1"));
    assert(!strcmp(value("DLC"), "none") && !strcmp(value("HANDED OVER"), "none"));
    f.storages = 2;
    f.storage_bytes = 2;
    make(&f);
    assert(!strcmp(value("HANDED OVER"), "2 storages, 1 KB"));
    /* each fact not known, one at a time: "?" in its place */
    f = known();
    f.programs = -1;
    make(&f);
    assert(!strcmp(value("PROGRAM"), "1 of ?"));
    f.program = -1;
    make(&f);
    assert(!strcmp(value("PROGRAM"), "?") && !strcmp(value("RECOMPILE"), "not needed"));
    f = known();
    f.previous = -1;
    make(&f);
    assert(!strcmp(value("STARTED BY"), "?") && !strcmp(value("USER"), "Player (user 1)")); /* kept from ?: not said */
    f = known();
    f.user[0] = 0;
    make(&f);
    assert(!strcmp(value("USER"), "?, kept from program 0"));
    f = known();
    f.user_number = 0;
    f.user_kept = -1;
    make(&f);
    assert(!strcmp(value("USER"), "Player"));
    f = known();
    f.version[0] = 0;
    make(&f);
    assert(!strcmp(value("VERSION"), "? (update v196608)"));
    f.update[0] = 0;
    make(&f);
    assert(!strcmp(value("VERSION"), "? (update ?)"));
    f.updated = -1;
    snprintf(f.version, sizeof f.version, "%s", "2.0");
    make(&f);
    assert(!strcmp(value("VERSION"), "2.0 (update ?)"));
    f = known();
    f.dlcs = -1;
    make(&f);
    assert(!strcmp(value("DLC"), "?"));
    f.dlcs = 3;
    f.dlc[0] = 0;
    make(&f);
    assert(!strcmp(value("DLC"), "?"));
    f = known();
    f.gpu[0] = 0;
    f.title_id = 0;
    f.build[0] = 0;
    make(&f);
    assert(!strcmp(value("GPU"), "?") && !strcmp(value("TITLE ID"), "?") && !strcmp(value("BUILD"), "?"));
    /* rows a screen can't say: the launcher's no HANDED OVER, the runtime's no RECOMPILE, nor a recompile's own step */
    f = known();
    f.storages = -1;
    f.recompile = -1;
    make(&f);
    assert(g_rows.count == SNX_START_ROWS - 2 && !value("HANDED OVER") && !value("RECOMPILE"));
    f.recompile = 1;
    make(&f);
    assert(!value("RECOMPILE"));
    /* nothing known at all: "?" in every row the screens can say */
    snx_program_start_unknown(&f);
    make(&f);
    assert(g_rows.count == SNX_START_ROWS - 2);
    for (i = 0; i < g_rows.count; ++i)
        assert(!strcmp(g_rows.row[i].value, strcmp(g_rows.row[i].label, "VERSION") ? "?" : "? (update ?)"));
    printf("program_start: rows known, each one not known as ?, rows left out passed\n");
    return 0;
}
