/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Recommended Settings' rules (recommend.h), on what scarletnx --probe said (vk/probe.h, runtime/main.c's snx_probe).
   Each rule's numbers come from players' logs and this probe's runs (#888):
   - ASTC as BC7 where the GPU samples BCn but not ASTC and its budget is under 8192 MB: the only logs that ran past
     their budget were of 8 GB cards (budgets of 7079 to 7143 MB), and BC7 takes a quarter of RGBA8's memory;
   - fewer pipeline threads than Auto only when they make at least 95% of the best rate: with less, the answer
     changed from run to run on the same machine, and more workers still gain a little in a game (pipeline.c);
   - Memory fit where the memory available is under the game's pool, what the rest of ScarletNX takes at least
     (svc_mem.c's FIT_OTHER_LEAST, 700 MB) and a GB of room, as the memory available moves between the test and the
     game */
#include "gui/recommend.h"
#include "gui/i18n.h"
#include "common/utilities/common.h"
#include "common/utilities/ini_lines.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REC_THREADS_SHARE 0.95 /* the share of the best rate fewer threads must make */
#define REC_FIT_OTHER_MB  700  /* svc_mem.c's FIT_OTHER_LEAST */
#define REC_FIT_ROOM_MB   1024

void rec_probe_start(RecProbe* p) {
    memset(p, 0, sizeof *p);
    p->heap_mb = p->budget_mb = p->ram_mb = -1;
    p->bc = p->astc = -1;
}

static void rec_text(char* out, size_t size, const char* value) { snprintf(out, size, "%s", value); }

/* "6:206,10:227" into p's threads and rates */
static void rec_pipelines(RecProbe* p, const char* list) {
    p->n_threads = 0;
    while (*list && p->n_threads < REC_THREADS) {
        char* end;
        long threads = strtol(list, &end, 10);
        double rate;
        if (end == list || *end != ':' || threads < 1) break;
        rate = strtod(end + 1, &end);
        if (rate <= 0) break;
        p->threads[p->n_threads] = (int)threads;
        p->rates[p->n_threads++] = rate;
        if (*end != ',') break;
        list = end + 1;
    }
}

void rec_probe_line(RecProbe* p, const char* line) {
    const char* eq = strchr(line, '=');
    char key[32];
    size_t n;
    if (!strncmp(line, "probe: ", 7)) {
        rec_text(p->step, sizeof p->step, line + 7);
        return;
    }
    if (!eq || (n = (size_t)(eq - line)) >= sizeof key) return;
    memcpy(key, line, n);
    key[n] = 0;
    ++eq;
    if (!strcmp(key, "gpu"))
        rec_text(p->gpu, sizeof p->gpu, eq);
    else if (!strcmp(key, "gpu_driver"))
        rec_text(p->gpu_driver, sizeof p->gpu_driver, eq);
    else if (!strcmp(key, "gpu_type"))
        rec_text(p->gpu_type, sizeof p->gpu_type, eq);
    else if (!strcmp(key, "gpu_error"))
        rec_text(p->gpu_error, sizeof p->gpu_error, eq);
    else if (!strcmp(key, "gpu_bc"))
        p->bc = atoi(eq) != 0;
    else if (!strcmp(key, "gpu_astc"))
        p->astc = atoi(eq) != 0;
    else if (!strcmp(key, "gpu_heap_mb"))
        p->heap_mb = atol(eq);
    else if (!strcmp(key, "gpu_budget_mb"))
        p->budget_mb = atol(eq);
    else if (!strcmp(key, "gpu_budget_from"))
        p->budget_from_driver = !strcmp(eq, "driver");
    else if (!strcmp(key, "pipelines_auto"))
        p->auto_threads = atoi(eq);
    else if (!strcmp(key, "pipelines"))
        rec_pipelines(p, eq);
    else if (!strcmp(key, "pipelines_error"))
        rec_text(p->pipelines_error, sizeof p->pipelines_error, eq);
    else if (!strcmp(key, "cpus"))
        p->cpus = atoi(eq);
    else if (!strcmp(key, "cores"))
        p->cores = atoi(eq);
    else if (!strcmp(key, "ram_available_mb"))
        p->ram_mb = atol(eq);
    else if (!strcmp(key, "ram_error"))
        rec_text(p->ram_error, sizeof p->ram_error, eq);
}

int rec_gpu_tested(const RecProbe* p) { return !p->gpu_error[0] && p->bc >= 0 && p->astc >= 0 && p->budget_mb >= 0; }
int rec_pipelines_tested(const RecProbe* p) {
    return !p->pipelines_error[0] && p->n_threads > 0 && p->auto_threads > 0;
}
int rec_memory_tested(const RecProbe* p) { return p->ram_mb >= 0; }

/* the probe's steps in the order it takes them, as it names them and as the screen does */
static const char* const rec_steps[] = {"gpu", "pipelines", "memory", "done"};
static const char* const rec_step_names[] = {SNX_TR_NOOP("GPU and its memory"), SNX_TR_NOOP("Pipelines"),
                                             SNX_TR_NOOP("RAM")};
const char* rec_step_name(const RecProbe* p) {
    int i;
    for (i = 0; i < 3; i++)
        if (!strcmp(p->step, rec_steps[i])) return snx_tr(rec_step_names[i]);
    if (!p->step[0]) return snx_tr("the start");
    return p->step; /* done, or a step a newer probe names */
}
void rec_progress(const RecProbe* p, char* out, size_t size) {
    size_t at = 0;
    int now = -1, i;
    for (i = 0; i < 4; i++)
        if (!strcmp(p->step, rec_steps[i])) now = i;
    out[0] = 0;
    for (i = 0; i < 3 && at < size; i++) {
        char found[320];
        const char* state;
        if (i < now) { /* done: what the step found */
            state = snx_tr("done");
            if (i == 0 && p->gpu_error[0])
                snprintf(found, sizeof found, ", %s", p->gpu_error);
            else if (i == 0)
                snprintf(found, sizeof found, snx_tr(", %s, %ld MB budget"), p->gpu, p->budget_mb);
            else if (i == 1 && p->pipelines_error[0])
                snprintf(found, sizeof found, ", %s", p->pipelines_error);
            else if (i == 1) {
                size_t f = 0;
                int t;
                found[0] = 0;
                for (t = 0; t < p->n_threads && f < sizeof found; t++)
                    f += (size_t)snprintf(found + f, sizeof found - f, snx_tr(", %d threads %.0f a second"),
                                          p->threads[t], p->rates[t]);
            } else if (p->ram_error[0])
                snprintf(found, sizeof found, ", %s", p->ram_error);
            else
                snprintf(found, sizeof found, snx_tr(", %ld MB available"), p->ram_mb);
        } else {
            state = i == now ? (p->ended[0] ? snx_tr("stopped") : snx_tr("running")) : snx_tr("waiting");
            snprintf(found, sizeof found, "%s%s", i == now && p->ended[0] ? ", " : "", i == now ? p->ended : "");
        }
        at +=
            (size_t)snprintf(out + at, size - at, "%s%s: %s%s", i ? "; " : "", snx_tr(rec_step_names[i]), state, found);
    }
}

/* why a step said nothing: the probe stopped before it, or ? for a probe that said done without it */
static const char* rec_ended(const RecProbe* p) { return p->ended[0] ? p->ended : "?"; }

long rec_memory_fit_under_mb(int memory_gb) {
    /* the application pool memory_gb gives the game (svc_mem.c's pool): the console's 3.2 GB below 6 */
    long pool = memory_gb >= 8 ? 6964 : memory_gb >= 6 ? 4916 : 3285;
    return pool + REC_FIT_OTHER_MB + REC_FIT_ROOM_MB;
}

static void rec_astc_bc(const RecProbe* p, RecRow* r) {
    if (!rec_gpu_tested(p)) {
        snprintf(r->why, sizeof r->why, snx_tr("Not tested: %s"), p->gpu_error[0] ? p->gpu_error : rec_ended(p));
        return;
    }
    r->tested = 1;
    rec_text(r->value, sizeof r->value, "0");
    if (p->astc)
        snprintf(r->why, sizeof r->why, "%s",
                 snx_tr("This GPU samples ASTC itself: ASTC as BC7 does nothing here (Off)"));
    else if (!p->bc)
        snprintf(r->why, sizeof r->why, "%s",
                 snx_tr("This GPU samples neither BCn nor ASTC: ASTC as BC7 cannot work here (Off)"));
    else if (p->budget_mb >= REC_ASTC_BC_BUDGET_MB)
        snprintf(r->why, sizeof r->why,
                 snx_tr("This GPU samples BCn but not ASTC, and its budget is %ld MB: from %d MB up, ASTC textures "
                        "stay exact (Off)"),
                 p->budget_mb, REC_ASTC_BC_BUDGET_MB);
    else {
        rec_text(r->value, sizeof r->value, "1");
        snprintf(r->why, sizeof r->why,
                 snx_tr("This GPU samples BCn but not ASTC, and its budget is %ld MB, under %d MB: ASTC textures as "
                        "BC7 take a quarter of the memory, close to exact (On)"),
                 p->budget_mb, REC_ASTC_BC_BUDGET_MB);
    }
}

static void rec_pipeline_threads(const RecProbe* p, RecRow* r) {
    double best = 0, auto_rate = 0;
    int i, pick = -1, best_threads = 0;
    if (!rec_pipelines_tested(p)) {
        snprintf(r->why, sizeof r->why, snx_tr("Not tested: %s"),
                 p->pipelines_error[0] ? p->pipelines_error
                 : p->gpu_error[0]     ? p->gpu_error
                                       : rec_ended(p));
        return;
    }
    for (i = 0; i < p->n_threads; i++) {
        if (p->rates[i] > best) {
            best = p->rates[i];
            best_threads = p->threads[i];
        }
        if (p->threads[i] == p->auto_threads) auto_rate = p->rates[i];
    }
    /* the fewest threads within the share of the best */
    for (i = 0; i < p->n_threads; i++)
        if (p->rates[i] >= best * REC_THREADS_SHARE && (pick < 0 || p->threads[i] < p->threads[pick])) pick = i;
    r->tested = 1;
    if (p->threads[pick] == p->auto_threads) {
        rec_text(r->value, sizeof r->value, "0");
        if (p->n_threads == 1)
            snprintf(r->why, sizeof r->why,
                     snx_tr("Auto makes %d threads, as many as the cores: %.0f pipelines a second (Auto)"),
                     p->auto_threads, auto_rate);
        else
            snprintf(
                r->why, sizeof r->why,
                snx_tr("Auto makes %d threads: %.0f pipelines a second, and no fewer threads make %d%% of the best, "
                       "%.0f with %d (Auto)"),
                p->auto_threads, auto_rate, (int)(REC_THREADS_SHARE * 100), best, best_threads);
        return;
    }
    snprintf(r->value, sizeof r->value, "%d", p->threads[pick]);
    if (p->threads[pick] < p->auto_threads)
        snprintf(r->why, sizeof r->why,
                 snx_tr("%d threads make %.0f pipelines a second, %.0f%% of the best (%.0f with %d), and leave the "
                        "game's own threads more room than Auto's %d"),
                 p->threads[pick], p->rates[pick], p->rates[pick] / best * 100, best, best_threads, p->auto_threads);
    else
        snprintf(
            r->why, sizeof r->why,
            snx_tr("%d threads make %.0f pipelines a second, where Auto's %d make %.0f, %.0f%% of that: Auto stops "
                   "at 12"),
            p->threads[pick], p->rates[pick], p->auto_threads, auto_rate,
            p->rates[pick] > 0 ? auto_rate / p->rates[pick] * 100 : 0);
}

static void rec_memory_fit(const RecProbe* p, int memory_gb, RecRow* r) {
    long under = rec_memory_fit_under_mb(memory_gb);
    if (!rec_memory_tested(p)) {
        snprintf(r->why, sizeof r->why, snx_tr("Not tested: %s"), p->ram_error[0] ? p->ram_error : rec_ended(p));
        return;
    }
    r->tested = 1;
    if (p->ram_mb < under) {
        rec_text(r->value, sizeof r->value, "1");
        snprintf(r->why, sizeof r->why,
                 snx_tr("%ld MB of RAM available, under %ld MB: the game sees only what the device can still give it "
                        "(On)"),
                 p->ram_mb, under);
    } else {
        rec_text(r->value, sizeof r->value, "0");
        snprintf(r->why, sizeof r->why, snx_tr("%ld MB of RAM available: Memory fit is for less than %ld MB (Off)"),
                 p->ram_mb, under);
    }
}

/* a and b the same value: the same number when both are one, else the same text (settings.c's same_value) */
static int rec_same(const char* a, const char* b) {
    char *a_end, *b_end;
    double x = strtod(a, &a_end), y = strtod(b, &b_end);
    if (a_end != a && !*a_end && b_end != b && !*b_end) return x == y;
    return !strcmp(a, b);
}
int rec_row_state(const RecRow* row, const char* effective, const char* proposed) {
    if (!row->tested) return REC_ROW_UNTESTED;
    if (rec_same(row->value, effective)) return REC_ROW_SAME;
    return proposed[0] && rec_same(row->value, proposed) ? REC_ROW_KEPT : REC_ROW_PROPOSED;
}

const char* const rec_keys[REC_COUNT] = {"astc_bc", "pipeline_threads", "memory_fit"};
int rec_key(const char* key) {
    int i;
    for (i = 0; i < REC_COUNT; i++)
        if (!strcmp(key, rec_keys[i])) return i;
    return -1;
}
void rec_rules(const RecProbe* p, int memory_gb, RecRow rows[REC_COUNT]) {
    const char* const* keys = rec_keys;
    int i;
    for (i = 0; i < REC_COUNT; i++) {
        memset(&rows[i], 0, sizeof rows[i]);
        rows[i].key = keys[i];
    }
    rec_astc_bc(p, &rows[REC_ASTC_BC]);
    rec_pipeline_threads(p, &rows[REC_PIPELINE_THREADS]);
    rec_memory_fit(p, memory_gb, &rows[REC_MEMORY_FIT]);
}

int rec_file_read(RecFile* f, const char* path, char* why, size_t size) {
    size_t n;
    char *text, *key, *value;
    SnxIni ini;
    memset(f, 0, sizeof *f);
    text = (char*)snx_read_file(path, &n);
    if (!text) {
        if (!snx_is_file(path)) return 0;
        snx_os_error(why, size);
        return 1;
    }
    snx_ini_start(&ini, text);
    while (snx_ini_next(&ini, &key, &value)) {
        int k = rec_key(key);
        if (!strcmp(ini.section, "tested") && !strcmp(key, "date")) rec_text(f->date, sizeof f->date, value);
        if (k < 0) continue;
        if (!strcmp(ini.section, "recommended"))
            rec_text(f->applied[k], sizeof f->applied[k], value);
        else if (!strcmp(ini.section, "proposed"))
            rec_text(f->proposed[k], sizeof f->proposed[k], value);
    }
    free(text);
    return 0;
}

int rec_file_write(const RecFile* f, const RecProbe* p, const char* build, const char* path, char* why, size_t size) {
    char text[4096], *folder, *staged;
    size_t at;
    int i, failed;
    at = (size_t)snprintf(
        text, sizeof text,
        "; what Recommended Settings tested, proposed and applied: Settings shows [recommended] beside "
        "a row while its pick is the\n; value under [recommended]. Only the launcher writes this "
        "file\n[recommended]\n");
    for (i = 0; i < REC_COUNT && at < sizeof text; i++)
        if (f->applied[i][0])
            at += (size_t)snprintf(text + at, sizeof text - at, "%s = %s\n", rec_keys[i], f->applied[i]);
    if (at < sizeof text)
        at +=
            (size_t)snprintf(text + at, sizeof text - at, "[proposed]\n; every row of the last test, applied or not\n");
    for (i = 0; i < REC_COUNT && at < sizeof text; i++)
        if (f->proposed[i][0])
            at += (size_t)snprintf(text + at, sizeof text - at, "%s = %s\n", rec_keys[i], f->proposed[i]);
    if (at < sizeof text)
        at += (size_t)snprintf(text + at, sizeof text - at,
                               "[tested]\ndate = %s\nbuild = %s\ngpu = %s\ngpu_driver = %s\nformats = BCn %d, ASTC %d\n"
                               "budget_mb = %ld\ncpus = %d\ncores = %d\nram_available_mb = %ld\npipelines_auto = %d\n"
                               "pipelines = ",
                               f->date, build, p->gpu[0] ? p->gpu : p->gpu_error, p->gpu_driver, p->bc, p->astc,
                               p->budget_mb, p->cpus, p->cores, p->ram_mb, p->auto_threads);
    for (i = 0; i < p->n_threads && at < sizeof text; i++)
        at += (size_t)snprintf(text + at, sizeof text - at, "%s%d:%.0f", i ? ", " : "", p->threads[i], p->rates[i]);
    if (at < sizeof text) at += (size_t)snprintf(text + at, sizeof text - at, "\n");
    if (at >= sizeof text) {
        snprintf(why, size, "its text is longer than %u bytes", (unsigned)sizeof text);
        return 1;
    }
    folder = snx_dirname(path);
    staged = snx_format("%s.new", path);
    /* staged beside it, then put in place: a crash mid-write leaves the old file */
    failed = !folder || !staged || snx_mkdirs(folder) || snx_write_file(staged, text, at) || snx_replace(staged, path);
    if (failed) {
        snx_os_error(why, size);
        if (staged) remove(staged);
    }
    free(staged);
    free(folder);
    return failed;
}
