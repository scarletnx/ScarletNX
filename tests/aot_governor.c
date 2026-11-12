/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A recompile's threads when the setting aot_threads leaves them open (common.h's SnxAotGovernor): 1 to start, one
   more a tick while the memory measured from the units holds it and the system does not say memory is short, each
   decision said with its numbers; the pressure stall file's "some" line; and the threads auto_workers.h opens from
   readings of the test's own. Run by ctest as aot_governor */
#define _POSIX_C_SOURCE 200809L
#include "common/cpu/recompiler/auto_workers.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <string.h>

#define MIB(n) ((SnxU64)(n) << 20)

static int g_failed; /* the checks that failed */

static void check(int ok, const char* what) {
    if (ok) return;
    fprintf(stderr, "aot_governor: %s\n", what);
    ++g_failed;
}
/* a tick on a reading in MiB: whether it opened as wanted and said want ("" for nothing) */
static void tick(SnxAotGovernor* g, const char* what, unsigned footprint, unsigned held, unsigned available, int known,
                 int pressure, unsigned active, size_t done, size_t left, int want_open, const char* want) {
    SnxAotReading r = {0};
    char line[768] = "x";
    int opened;
    r.footprint = MIB(footprint);
    r.held = MIB(held);
    r.available = MIB(available);
    r.available_known = known;
    r.pressure = pressure;
    r.active = active;
    r.done = done;
    r.left = left;
    opened = snx_aot_governor_tick(g, &r, "the test's signal", line, sizeof line);
    if (opened == want_open && !strcmp(line, want)) return;
    fprintf(stderr, "aot_governor: %s: %s and said\n  %s\nnot %s and\n  %s\n", what, opened ? "opened" : "opened none",
            line[0] ? line : "(nothing)", want_open ? "opened" : "opened none", want[0] ? want : "(nothing)");
    ++g_failed;
}

static void rule(void) {
    SnxAotGovernor g;
    char line[768];
    snx_aot_governor_start(&g, 4, 0, MIB(200), "the test's signal", line, sizeof line);
    check(!strcmp(line,
                  "aot_threads=0 (Auto): 1 thread to start, 200 MiB in use before the first unit; once that unit has "
                  "ended one more opens each second while the memory holds it, up to the 4 the cores give with a "
                  "quarter left free. The system's memory pressure: the test's signal"),
          "the first line says the thread it starts on, the memory before the first unit, the most and the signal");
    check(g.threads == 1 && g.most == 4 && g.base == MIB(200) && !g.unit, "the control starts on 1 thread");
    /* the first unit is under way: what it takes is measured, and nothing opens whatever the room */
    tick(&g, "the first unit under way", 350, 0, 50000, 1, 0, 1, 0, 9, 0, "");
    tick(&g, "the first unit grown", 500, 0, 50000, 1, 0, 1, 0, 9, 0, "");
    check(g.unit == MIB(300), "a unit under way took the footprint over the one before the first unit");
    /* it ended: a thread opens while the system can still give the unit of the new thread and one more */
    tick(&g, "room for two units", 500, 0, 600, 1, 0, 1, 1, 8, 1,
         "aot_threads=0 (Auto): thread 2 of 4 opens: 500 MiB in use, 200 MiB of them before the first unit and 0 MiB "
         "finished units' code, so a unit under way took 300 MiB at most; the system can still give 600 MiB, the 1 "
         "thread open may take 0 MiB more, and 2 units more are 600 MiB");
    /* the thread just opened has not taken its unit yet: that is still to come, and counts against the room */
    tick(&g, "a MiB short of the open threads' rest and two units", 500, 0, 899, 1, 0, 2, 1, 7, 0,
         "aot_threads=0 (Auto): stays on 2 threads of 4: 500 MiB in use, 200 MiB of them before the first unit and 0 "
         "MiB finished units' code, so a unit under way took 300 MiB at most; the system can still give 899 MiB, the "
         "2 threads open may take 300 MiB more, and 2 units more are 600 MiB");
    tick(&g, "the same reason again is not said again", 500, 0, 700, 1, 0, 2, 1, 7, 0, "");
    tick(&g, "exactly the room", 500, 0, 900, 1, 0, 2, 1, 7, 1,
         "aot_threads=0 (Auto): thread 3 of 4 opens: 500 MiB in use, 200 MiB of them before the first unit and 0 MiB "
         "finished units' code, so a unit under way took 300 MiB at most; the system can still give 900 MiB, the 2 "
         "threads open may take 300 MiB more, and 2 units more are 600 MiB");
    /* the system says memory is short: nothing opens, whatever the room, and it is said once */
    tick(&g, "pressure", 1100, 0, 50000, 1, 1, 3, 2, 6, 0,
         "aot_threads=0 (Auto): stays on 3 threads of 4: the system says memory is short (the test's signal)");
    tick(&g, "pressure still", 1100, 0, 50000, 1, 1, 3, 2, 6, 0, "");
    /* a unit that grows past any seen raises what a unit takes: 3 under way over 1340 MiB of units' memory */
    tick(&g, "a unit grew, and no room for two of them", 1640, 100, 800, 1, 0, 3, 2, 6, 0,
         "aot_threads=0 (Auto): stays on 3 threads of 4: 1640 MiB in use, 200 MiB of them before the first unit and "
         "100 MiB finished units' code, so a unit under way took 446 MiB at most; the system can still give 800 MiB, "
         "the 3 threads open may take 0 MiB more, and 2 units more are 893 MiB");
    check(g.unit == MIB(1340) / 3, "the finished units' code is not a unit's memory");
    /* the code the finished units keep is the recompile's own: the open threads are short of their most again */
    check(g.threads == 3, "no room: the threads stay");
    tick(&g, "a MiB short for the last thread", 1300, 400, 1533, 1, 0, 3, 5, 3, 0, "");
    tick(&g, "the last thread", 1300, 400, 1534, 1, 0, 3, 5, 3, 1,
         "aot_threads=0 (Auto): thread 4 of 4 opens: 1300 MiB in use, 200 MiB of them before the first unit and 400 "
         "MiB finished units' code, so a unit under way took 446 MiB at most; the system can still give 1534 MiB, the "
         "3 threads open may take 639 MiB more, and 2 units more are 893 MiB");
    tick(&g, "the most the cores give", 1300, 400, 50000, 1, 0, 4, 5, 3, 0, "");
    check(g.threads == 4, "never more threads than the cores give");

    /* no unit left to take: a thread more would find none */
    snx_aot_governor_start(&g, 4, 0, MIB(200), "s", line, sizeof line);
    tick(&g, "no unit left", 500, 0, 50000, 1, 0, 1, 1, 0, 0, "");
    /* a footprint under the one before the first unit is no unit's memory, and wraps nothing */
    tick(
        &g, "a footprint that fell", 100, 0, 899, 1, 0, 1, 1, 5, 0,
        "aot_threads=0 (Auto): stays on 1 thread of 4: 100 MiB in use, 200 MiB of them before the first unit and 0 MiB "
        "finished units' code, so a unit under way took 300 MiB at most; the system can still give 899 MiB, the 1 "
        "thread open may take 300 MiB more, and 2 units more are 600 MiB");
}

static void unknown(void) {
    SnxAotGovernor g;
    char line[768];
    /* a system that says no footprint: nothing is measured, the cores alone decide, and the lines say so */
    snx_aot_governor_start(&g, 3, 0, 0, "none on this system", line, sizeof line);
    check(!strcmp(line,
                  "aot_threads=0 (Auto): 1 thread to start, the system does not say what the process takes; once that "
                  "unit has ended one more opens each second while the memory holds it, up to the 3 the cores give "
                  "with a quarter left free. The system's memory pressure: none on this system"),
          "the first line of a system that says no footprint");
    tick(&g, "no footprint, before the first unit ended", 0, 0, 0, 1, -1, 1, 0, 9, 0, "");
    tick(&g, "no footprint", 0, 0, 0, 1, -1, 1, 1, 9, 1,
         "aot_threads=0 (Auto): thread 2 of 3 opens: the system does not say what the process takes: the cores alone "
         "decide");
    /* a footprint, and no word of what the system can still give */
    snx_aot_governor_start(&g, 3, 0, MIB(200), "none on this system", line, sizeof line);
    tick(&g, "no available memory", 500, 0, 123, 0, -1, 1, 1, 9, 1,
         "aot_threads=0 (Auto): thread 2 of 3 opens: the system does not say what memory it can still give: the cores "
         "alone decide");
    /* no pressure signal is not pressure: the room alone decides */
    tick(&g, "no pressure signal, and room", 500, 0, 900, 1, -1, 2, 1, 9, 1,
         "aot_threads=0 (Auto): thread 3 of 3 opens: 500 MiB in use, 200 MiB of them before the first unit and 0 MiB "
         "finished units' code, so a unit under way took 300 MiB at most; the system can still give 900 MiB, the 2 "
         "threads open may take 300 MiB more, and 2 units more are 600 MiB");
    /* one core: nothing to open, and the line says the one thread */
    snx_aot_governor_start(&g, 1, 0, MIB(200), "s", line, sizeof line);
    check(!strcmp(line, "aot_threads=0 (Auto): 1 thread, what the cores give with a quarter left free"),
          "the line of a machine whose cores give 1 thread");
    tick(&g, "one core", 500, 0, 50000, 1, 0, 1, 1, 9, 0, "");
    snx_aot_governor_start(&g, 0, 0, MIB(200), "s", line, sizeof line);
    check(g.most == 1 && g.threads == 1, "no cores said is 1 thread");
    /* a ceiling the setting aot_threads_most gives is said as its, not the cores' */
    snx_aot_governor_start(&g, 3, 1, MIB(200), "s", line, sizeof line);
    check(!strcmp(line,
                  "aot_threads=0 (Auto): 1 thread to start, 200 MiB in use before the first unit; once that unit "
                  "has ended one more opens each second while the memory holds it, up to the 3 aot_threads_most=3 "
                  "gives. The system's memory pressure: s"),
          "the first line names aot_threads_most's ceiling");
    snx_aot_governor_start(&g, 1, 1, MIB(200), "s", line, sizeof line);
    check(!strcmp(line, "aot_threads=0 (Auto): 1 thread, what aot_threads_most=1 gives"), "aot_threads_most=1's line");
}
/* the threads from the two settings: a count as given, Auto's ceiling aot_threads_most's or the cores', and a most
   given beside a count said with both values */
static void threads_of_settings(void) {
    char note[160] = "unset";
    check(snx_aot_threads(6, 0, note, sizeof note) == 6 && !note[0], "aot_threads=6 is 6, nothing said");
    check(snx_aot_threads(0, 3, note, sizeof note) == 3 && !note[0], "Auto with aot_threads_most=3 opens 3 at most");
    check(snx_aot_threads(0, 0, note, sizeof note) == snx_build_threads() && !note[0], "Auto alone: the cores'");
    check(snx_aot_threads(6, 2, note, sizeof note) == 6 &&
              !strcmp(note, "aot_threads_most=2 bounds Auto only: aot_threads=6 compiles on 6 threads"),
          "a most beside a count changes nothing, said with both values");
    check(snx_aot_threads(0, 5, NULL, 0) == 5, "no note asked");
}

static void psi(void) {
    unsigned avg10 = 99;
    SnxU64 total = 99;
    check(snx_psi_some("some avg10=0.00 avg60=0.00 avg300=0.00 total=81173861\n"
                       "full avg10=0.00 avg60=0.00 avg300=0.00 total=60292331\n",
                       &avg10, &total) &&
              !avg10 && total == 81173861u,
          "a pressure stall file at rest");
    check(snx_psi_some("some avg10=12.05 avg60=3.40 avg300=0.71 total=5\n", &avg10, &total) && avg10 == 1205 &&
              total == 5,
          "avg10 in hundredths of a percent");
    check(snx_psi_some("full avg10=7.00 avg60=0.00 avg300=0.00 total=1\nsome avg10=0.01 avg60=0.00 avg300=0.00 "
                       "total=2\n",
                       &avg10, &total) &&
              avg10 == 1 && total == 2,
          "the some line, not the full one before it");
    avg10 = 77;
    check(!snx_psi_some("full avg10=7.00 avg60=0.00 avg300=0.00 total=1\n", &avg10, &total) && avg10 == 77,
          "no some line");
    check(!snx_psi_some("", &avg10, &total) && !snx_psi_some("some avg10=x\n", &avg10, &total), "no numbers");
    /* memory is short from a tenth of the time stalled, not from any stall: what a quiet desktop read while a file
       was read is not pressure */
    check(!snx_psi_short("some avg10=0.36 avg60=0.10 avg300=0.02 total=83452141\n", &avg10) && avg10 == 36,
          "a stall of 0.36% is not memory pressure");
    check(!snx_psi_short("some avg10=9.99 avg60=0.00 avg300=0.00 total=1\n", &avg10) && avg10 == 999,
          "just under a tenth of the time");
    check(snx_psi_short("some avg10=10.00 avg60=0.00 avg300=0.00 total=1\n", &avg10) == 1 && avg10 == 1000,
          "a tenth of the time stalled is memory pressure");
    check(snx_psi_short("some avg10=57.20 avg60=9.00 avg300=1.00 total=1\n", &avg10) == 1, "more than a tenth");
    check(snx_psi_short("full avg10=57.20 avg60=9.00 avg300=1.00 total=1\n", &avg10) == -1,
          "no some line is no signal, not pressure");
}

/* ---- auto_workers.h on a queue and readings of the test's own: what opens follows the readings alone ---- */

#define LINES 16
typedef struct {
    const SnxAotReading* script; /* a tick's reading each, the first the one before the first unit */
    unsigned ticks;              /* how many it holds: the queue has no unit left once they are read */
    unsigned read;               /* how many were read, by the control's thread alone */
    int stop;                    /* atomic: no unit left */
    unsigned started;            /* atomic: the threads that ran the worker */
    char lines[LINES][768];
    unsigned said;
} Fake;

static void fake_worker(void* arg) {
    Fake* f = (Fake*)arg;
    __atomic_add_fetch(&f->started, 1, __ATOMIC_SEQ_CST);
    while (!__atomic_load_n(&f->stop, __ATOMIC_SEQ_CST)) plat_sleep_ns(1000000); /* a unit of 1 ms */
}
static void fake_state(void* arg, SnxAotReading* r) {
    Fake* f = (Fake*)arg;
    r->active = 1; /* the script's memory is one unit's, whatever the threads */
    r->done = 1;
    r->held = 0;
    r->left = __atomic_load_n(&f->stop, __ATOMIC_SEQ_CST) ? 0 : 1000;
}
static void fake_read(void* arg, SnxAotReading* r, char* pressure, size_t size) {
    Fake* f = (Fake*)arg;
    snprintf(pressure, size, "the test's signal");
    if (f->read < f->ticks) {
        r->footprint = f->script[f->read].footprint;
        r->available = f->script[f->read].available;
        r->available_known = f->script[f->read].available_known;
        r->pressure = f->script[f->read].pressure;
    }
    if (++f->read >= f->ticks) __atomic_store_n(&f->stop, 1, __ATOMIC_SEQ_CST);
}
static void fake_say(void* arg, const char* line) {
    Fake* f = (Fake*)arg;
    if (f->said < LINES) snprintf(f->lines[f->said], sizeof f->lines[f->said], "%s", line);
    ++f->said;
}
static unsigned run_fake(Fake* f, const SnxAotReading* script, unsigned ticks, unsigned most, int quiet) {
    Fake zero = {0};
    SnxAutoWorkers w = {0};
    *f = zero;
    f->script = script;
    f->ticks = ticks;
    w.worker = fake_worker;
    w.state = fake_state;
    w.arg = f;
    w.most = most;
    w.read = fake_read;
    w.say = fake_say;
    w.quiet = quiet;
    w.tick_ms = 10;
    return snx_run_workers_auto(&w);
}

static void workers(void) {
    /* {footprint, held, available, known, pressure}: the first is read before the first unit */
    static const SnxAotReading pressure[] = {
        {MIB(100), 0, MIB(9000), 1, 0, 0, 0, 0}, {MIB(100), 0, MIB(9000), 1, 0, 0, 0, 0},
        {MIB(100), 0, MIB(9000), 1, 0, 0, 0, 0}, {MIB(100), 0, MIB(9000), 1, 1, 0, 0, 0},
        {MIB(100), 0, MIB(9000), 1, 1, 0, 0, 0}, {MIB(100), 0, MIB(9000), 1, 0, 0, 0, 0},
    };
    static const SnxAotReading room[] = {
        {MIB(100), 0, MIB(9000), 1, 0, 0, 0, 0}, {MIB(400), 0, MIB(599), 1, 0, 0, 0, 0},
        {MIB(400), 0, MIB(600), 1, 0, 0, 0, 0},  {MIB(400), 0, MIB(899), 1, 0, 0, 0, 0},
        {MIB(400), 0, MIB(899), 1, 0, 0, 0, 0},
    };
    Fake f;
    unsigned opened = run_fake(&f, pressure, 6, 8, 0);
    check(opened == 4 && f.started == 4, "a thread for the start and one for each tick without pressure: 4");
    check(f.said == 5 && !strncmp(f.lines[0], "aot_threads=0 (Auto): 1 thread to start, 100 MiB in use", 55) &&
              !strncmp(f.lines[1], "aot_threads=0 (Auto): thread 2 of 8 opens: ", 43) &&
              !strncmp(f.lines[2], "aot_threads=0 (Auto): thread 3 of 8 opens: ", 43) &&
              !strcmp(f.lines[3], "aot_threads=0 (Auto): stays on 3 threads of 8: the system says memory is short "
                                  "(the test's signal)") &&
              !strncmp(f.lines[4], "aot_threads=0 (Auto): thread 4 of 8 opens: ", 43),
          "the first line, each thread opened and the pressure once");
    if (g_failed)
        for (unsigned i = 0; i < f.said && i < LINES; ++i) fprintf(stderr, "  %u: %s\n", i, f.lines[i]);
    opened = run_fake(&f, room, 5, 8, 0);
    check(opened == 2 && f.started == 2, "one thread more once two units fit, none while its own is still to come");
    check(f.said == 4 && !strncmp(f.lines[1], "aot_threads=0 (Auto): stays on 1 thread of 8: 400 MiB in use", 60) &&
              strstr(f.lines[1], "the system can still give 599 MiB") &&
              !strncmp(f.lines[2], "aot_threads=0 (Auto): thread 2 of 8 opens: ", 43) &&
              !strncmp(f.lines[3], "aot_threads=0 (Auto): stays on 2 threads of 8: ", 47) &&
              strstr(f.lines[3], "the 2 threads open may take 300 MiB more"),
          "the lines of the room's script");
    opened = run_fake(&f, pressure, 6, 1, 0);
    check(opened == 1 && f.started == 1 && f.said == 1, "the cores give 1 thread: 1, and its line");
    opened = run_fake(&f, pressure, 6, 8, 1);
    check(opened == 4 && !f.said, "quiet says nothing and opens the same");
}

int main(void) {
    rule();
    threads_of_settings();
    unknown();
    psi();
    workers();
    if (!g_failed) puts("aot_governor: ok");
    return g_failed != 0;
}
