/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/program_start.h"
#include <stdio.h>
#include <string.h>

void snx_program_start_unknown(SnxProgramStart* facts) {
    memset(facts, 0, sizeof *facts);
    facts->program = facts->programs = facts->previous = -1;
    facts->user_kept = facts->updated = facts->dlcs = facts->storages = facts->recompile = -1;
}
static SnxStartRow* row(SnxStartRow* rows, int* n, const char* label) {
    SnxStartRow* r = &rows[(*n)++];
    snprintf(r->label, sizeof r->label, "%s", label);
    r->value[0] = 0;
    return r;
}
/* the end of a row's value, to go on writing it */
static char* tail(SnxStartRow* r, size_t* left) {
    size_t n = strlen(r->value);
    *left = sizeof r->value - n;
    return r->value + n;
}
int snx_program_start_rows(const SnxProgramStart* f, SnxStartRow rows[SNX_START_ROWS]) {
    SnxStartRow* r;
    size_t left;
    int n = 0;
    r = row(rows, &n, "PROGRAM");
    if (f->program < 0)
        snprintf(r->value, sizeof r->value, "?");
    else if (f->programs > 0)
        snprintf(r->value, sizeof r->value, "%d of %d", f->program, f->programs);
    else
        snprintf(r->value, sizeof r->value, "%d of ?", f->program);
    r = row(rows, &n, "STARTED BY");
    if (f->previous < 0)
        snprintf(r->value, sizeof r->value, "?");
    else
        snprintf(r->value, sizeof r->value, "program %d%s", f->previous,
                 f->previous == f->program ? " (restarted)" : "");
    r = row(rows, &n, "USER");
    snprintf(r->value, sizeof r->value, "%s", f->user[0] ? f->user : "?");
    if (f->user[0] && f->user_number > 0) snprintf(tail(r, &left), left, " (user %d)", f->user_number);
    if (f->user_kept == 1 && f->previous >= 0) snprintf(tail(r, &left), left, ", kept from program %d", f->previous);
    r = row(rows, &n, "VERSION");
    snprintf(r->value, sizeof r->value, "%s", f->version[0] ? f->version : "?");
    if (f->updated != 0)
        snprintf(tail(r, &left), left, " (update %s)", f->updated > 0 && f->update[0] ? f->update : "?");
    r = row(rows, &n, "DLC");
    snprintf(r->value, sizeof r->value, "%s", f->dlcs < 0 ? "?" : !f->dlcs ? "none" : f->dlc[0] ? f->dlc : "?");
    if (f->storages >= 0) {
        r = row(rows, &n, "HANDED OVER");
        if (!f->storages)
            snprintf(r->value, sizeof r->value, "none");
        else
            snprintf(r->value, sizeof r->value, "%d storage%s, %llu KB", f->storages, f->storages == 1 ? "" : "s",
                     (f->storage_bytes + 1023) / 1024);
    }
    r = row(rows, &n, "GPU");
    snprintf(r->value, sizeof r->value, "%s", f->gpu[0] ? f->gpu : "?");
    r = row(rows, &n, "TITLE ID");
    if (f->title_id)
        snprintf(r->value, sizeof r->value, "%016llX", f->title_id);
    else
        snprintf(r->value, sizeof r->value, "?");
    r = row(rows, &n, "BUILD");
    snprintf(r->value, sizeof r->value, "%s", f->build[0] ? f->build : "?");
    if (f->recompile == 0) {
        r = row(rows, &n, "RECOMPILE");
        if (f->program >= 0)
            snprintf(r->value, sizeof r->value, "not needed, program %d is made", f->program);
        else
            snprintf(r->value, sizeof r->value, "not needed");
    }
    return n;
}
