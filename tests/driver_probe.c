/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* scarletnx --probe (src/nx/runtime/driver/probe.c) with this program as the recompiler and as the game it makes. "-r"
   copies this program into the game's folder; the copy, run with -H, plays the next launch of the plan
   $SNX_PROBE_TEST_PLAN names (a line a launch): writes report lines and a journal record as edenprobe does, then exits,
   is killed or hangs. Each call goes on a line of $SNX_PROBE_TEST_LOG. A clean run, the game made once, an NRO that
   changed, a crash the next launch goes on from, a hang, no report, a launch that adds nothing, the launch cap, an end
   after the summary, the settings passed on, a recompile that fails, an earlier run's card, words edenprobe did not
   know, and the command line's refusals */
#define _POSIX_C_SOURCE 200809L
#include "nx/runtime/driver/probe.h"
#include "common/utilities/common.h"
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define GAME "edenprobe.exe"
#else
#include <time.h>
#define GAME "edenprobe"
#endif

/* the checks made, and the error lines a run said (snx_error_sink) */
static struct {
    unsigned checks;
    char said[4096];
} g_driver_probe;
static void check(int good, const char* expression, int line) {
    ++g_driver_probe.checks;
    if (!good) {
        fprintf(stderr, "driver_probe.c:%d: check failed: %s\n", line, expression);
        exit(90);
    }
}
#define CHECK(value) check((value) != 0, #value, __LINE__)
static void set_env(const char* name, const char* value) {
#ifdef _WIN32
    CHECK(!_putenv_s(name, value));
#else
    CHECK(!setenv(name, value, 1));
#endif
}
static char* slurp(const char* path) {
    size_t n = 0;
    unsigned char* data = snx_is_file(path) ? snx_read_file(path, &n) : NULL;
    char* text = (char*)snx_alloc(n + 1);
    if (data) memcpy(text, data, n);
    text[n] = 0;
    free(data);
    return text;
}
static void append(const char* path, const char* text) {
    FILE* f = snx_fopen(path, "ab");
    CHECK(f != NULL);
    CHECK(fputs(text, f) >= 0);
    CHECK(!fclose(f));
}
static int occurrences(const char* text, const char* lit) {
    int n = 0;
    for (const char* at = strstr(text, lit); at; at = strstr(at + 1, lit)) n++;
    return n;
}

/* ============================================================ the fake recompiler and game */
static int fake_recompile(int argc, char** argv) {
    char *self = snx_executable(argv[0]), *data, *folder, *game;
    const char* failing = getenv("SNX_PROBE_TEST_RECOMPILE");
    append(getenv("SNX_PROBE_TEST_LOG"), "recompile\n");
    CHECK(argc == 6 && !strcmp(argv[3], "edenprobe") && !strcmp(argv[4], "--data") && snx_is_file(argv[2]));
    if (failing && !strcmp(failing, "fail")) return 2;
    data = snx_path_join(argv[5], "recompiled");
    folder = snx_path_join(data, "edenprobe");
    game = snx_path_join(folder, GAME);
    CHECK(!snx_mkdirs(folder));
    CHECK(!snx_copy_file(self, game));
    CHECK(!snx_make_executable(game));
    if (failing && !strcmp(failing, "made")) return 2; /* the game made, the exit a failure all the same */
    free(self);
    free(data);
    free(folder);
    free(game);
    return 0;
}
static void case_line(const char* report, const char* suite, const char* result, const char* reason) {
    char *text = slurp(report), *line;
    int k = occurrences(text, "\"type\":\"case\"") + 1;
    line = snx_format(
        "{\"type\":\"case\",\"suite\":\"%s\",\"name\":\"case %d\",\"tier\":\"exact\",\"result\":\"%s\"%s%s%s,"
        "\"detail\":\"\"}\n",
        suite, k, result, reason ? ",\"reason\":\"" : "", reason ? reason : "", reason ? "\"" : "");
    append(report, line);
    free(line);
    free(text);
}
static void pause_seconds(unsigned seconds) {
#ifdef _WIN32
    Sleep(seconds * 1000);
#else
    struct timespec t = {(time_t)seconds, 0};
    nanosleep(&t, NULL);
#endif
}
/* one launch of the plan: tokens "pass=N", "fail=SUITE", "crashed=SUITE", "summary=P,F,S,C", "start=SUITE:CASE" (a
   journal record of a case begun; "cut=" one cut short), "ignored" (a mode note of words edenprobe does not know), "none" (no report at all),
   then "exit=N", "crash" or "hang" */
static int fake_game(int argc, char** argv) {
    const char* sdmc = "edenprobe_data/sdmc/edenprobe";
    char *runs = snx_path_join(sdmc, "runs/abc1234"), *report = snx_path_join(runs, "report.jsonl"),
         *counter = snx_path_join(runs, "launches"), *journal = snx_path_join(runs, "journal"),
         *mode = snx_path_join(sdmc, "mode"), *plan = slurp(getenv("SNX_PROBE_TEST_PLAN")), *seen, *line, *token;
    int number, i, end = 0, code = 0;
    {
        char* called = snx_format("game");
        for (i = 1; i < argc; i++) {
            char* more = snx_format("%s %s", called, argv[i]);
            free(called);
            called = more;
        }
        seen = slurp(mode);
        line = snx_format("%s | mode: %s", called, seen);
        append(getenv("SNX_PROBE_TEST_LOG"), line);
        free(called);
        free(seen);
        free(line);
    }
    CHECK(!snx_mkdirs(runs));
    seen = slurp(counter);
    number = atoi(seen);
    free(seen);
    seen = snx_format("%d", number + 1);
    CHECK(!snx_write_file(counter, seen, strlen(seen)));
    free(seen);
    /* the plan's line for this launch */
    line = plan;
    for (i = 0; i < number && line; i++) line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL;
    if (!line || !*line) return 3;
    if (strchr(line, '\n')) *strchr(line, '\n') = 0;
    if (!strstr(line, "none")) {
        char* last = snx_path_join(sdmc, "last-run");
        CHECK(!snx_write_file(last, "runs/abc1234\n", 13));
        free(last);
        if (!number) append(report, "{\"type\":\"note\",\"text\":\"edenprobe 0.9.0 (abc1234) starting\"}\n");
    }
    for (token = strtok(line, " "); token; token = strtok(NULL, " ")) {
        if (!strncmp(token, "pass=", 5))
            for (i = atoi(token + 5); i > 0; i--) case_line(report, "cpu", "pass", NULL);
        else if (!strncmp(token, "fail=", 5))
            case_line(report, token + 5, "fail", NULL);
        else if (!strncmp(token, "crashed=", 8))
            case_line(report, token + 8, "skip", "crashed");
        else if (!strncmp(token, "summary=", 8)) {
            int p = 0, f = 0, s = 0, c = 0;
            char* summary;
            CHECK(sscanf(token + 8, "%d,%d,%d,%d", &p, &f, &s, &c) == 4);
            summary = snx_format("{\"type\":\"summary\",\"passed\":%d,\"failed\":%d,\"skipped\":%d,\"crashed\":%d}\n",
                                 p, f, s, c);
            append(report, summary);
            free(summary);
        } else if (!strncmp(token, "start=", 6) || !strncmp(token, "cut=", 4)) {
            /* "start <suite> <pass> <case>" and its tab, or cut short before it; the plan writes the case's spaces
               as _ */
            int whole = token[0] == 's';
            char *colon = strchr(token, ':'), *record, *k;
            *colon = 0;
            for (k = colon + 1; *k; k++)
                if (*k == '_') *k = ' ';
            record = snx_format("start %s 0 %s%s", token + (whole ? 6 : 4), colon + 1, whole ? "\t\n" : "");
            append(journal, record);
            free(record);
        } else if (!strcmp(token, "ignored"))
            append(report, "{\"type\":\"note\",\"text\":\"mode: quick, from the mode file; 1 unknown word ignored: "
                           "display\"}\n");
        else if (!strncmp(token, "exit=", 5)) {
            end = 1;
            code = atoi(token + 5);
        } else if (!strcmp(token, "crash"))
            end = 2;
        else if (!strcmp(token, "hang"))
            end = 3;
    }
    free(runs);
    free(report);
    free(counter);
    free(journal);
    free(mode);
    free(plan);
    if (end == 3) pause_seconds(30);
    if (end == 2) {
#ifdef _WIN32
        return 3;
#else
        raise(SIGKILL); /* no core file left behind, as a SIGSEGV would */
#endif
    }
    return code;
}

/* ============================================================ the runs */
static void sink(const char* line) {
    size_t n = strlen(g_driver_probe.said);
    snprintf(g_driver_probe.said + n, sizeof g_driver_probe.said - n, "%s\n", line);
}
typedef struct {
    char *self, *root, *nro, *folder, *plan, *log, *out_path;
    char *out, *calls; /* the last run's output and calls */
} Test;
/* the run of plan (a line a launch) with settings, quiet and launches: its result, with t->out, said and t->calls */
static int run(Test* t, const char* plan, const char* const* settings, double quiet, int launches) {
    SnxProbe p;
    FILE* out;
    int rc;
    CHECK(!snx_write_file(t->plan, plan, strlen(plan)));
    CHECK(!snx_write_file(t->log, "", 0));
    out = snx_fopen(t->out_path, "wb");
    CHECK(out != NULL);
    p.self = t->self;
    p.nro = t->nro;
    p.mode = SNX_PROBE_QUICK;
    p.folder = t->folder;
    p.settings = settings;
    p.quiet = quiet;
    p.launches = launches;
    p.out = out;
    g_driver_probe.said[0] = 0;
    rc = snx_probe_run(&p);
    CHECK(!fclose(out));
    free(t->out);
    free(t->calls);
    t->out = slurp(t->out_path);
    t->calls = slurp(t->log);
    return rc;
}
static char* in_folder(const Test* t, const char* rest) {
    char* path = snx_path_join(t->folder, rest);
    return path;
}
#define HAS(text, lit) CHECK(strstr((text), (lit)) != NULL)
#ifdef _WIN32
#define CRASHED "ended with exit 3"
#else
#define CRASHED "ended by signal 9 (SIGKILL)"
#endif

static void runs(Test* t) {
    char *report = in_folder(t, "report.jsonl"), *summary = in_folder(t, "summary.txt"), *text,
         *sdmc = in_folder(t, ".scarletnx/recompiled/edenprobe/edenprobe_data/sdmc/edenprobe"), *path;
    const char* const gpu[] = {"--gpu_driver=pkg", NULL};

    /* a clean run: the game made, the mode file written, the counts and the failing suite said */
    CHECK(run(t, "pass=2 fail=cpu summary=2,1,0,0 exit=0\n", NULL, 30, 17) == 0);
    HAS(t->out, "probe: recompiling edenprobe.nro\n");
    HAS(t->out, "probe: edenprobe 0.9.0 (abc1234), sha256 ");
    HAS(t->out, ", mode \"quick notime cpu graphics\", 1 launch, settings: defaults\n");
    HAS(t->out, "probe: 2 passed, 1 failed (cpu 1), 0 skipped, 0 crashed\n");
    HAS(t->out, "probe: 3 cases\n");
    CHECK(occurrences(t->calls, "recompile\n") == 1);
    HAS(t->calls, "game -H | mode: quick notime cpu graphics\n");
    text = slurp(report);
    CHECK(occurrences(text, "\"type\":\"case\"") == 3);
    free(text);
    text = slurp(summary);
    HAS(text, "probe: 2 passed, 1 failed (cpu 1), 0 skipped, 0 crashed\n");
    CHECK(!strstr(text, "cases\n")); /* the result, not the progress */
    free(text);
    CHECK(!strlen(g_driver_probe.said));

    /* the same NRO and program: the game is not made again, and the last run's count is the whole */
    CHECK(run(t, "pass=3 summary=3,0,0,0 exit=0\n", NULL, 30, 17) == 0);
    CHECK(!strstr(t->calls, "recompile"));
    HAS(t->out, "probe: 3 of 3 cases\n");

    /* another NRO: made again */
    append(t->nro, "a newer build");
    CHECK(run(t, "pass=1 summary=1,0,0,0 exit=0\n", NULL, 30, 17) == 0);
    CHECK(occurrences(t->calls, "recompile\n") == 1);
    HAS(t->out, "probe: 1 case\n"); /* that NRO's whole is not known yet */

    /* a crash inside a case: the next launch goes on, the case named, the crashed suite said */
    CHECK(run(t, "pass=2 start=gpu:draws_a_quad crash\ncrashed=gpu fail=cpu fail=astc fail=cpu summary=2,3,0,1\n", NULL,
              30, 17) == 0);
    HAS(t->out, "probe: launch 1 " CRASHED " inside gpu case \"draws a quad\"; launching again (2 of at most 17)\n");
    HAS(t->out, ", 2 launches, settings: defaults\n");
    HAS(t->out, "probe: 2 passed, 3 failed (cpu 2, astc 1), 0 skipped, 1 crashed (gpu 1)\n"); /* the most first */
    CHECK(occurrences(t->calls, "game -H") == 2);

    /* a journal record cut short is no record: no case named */
    CHECK(run(t, "pass=1 cut=gpu:half_written crash\npass=1 summary=2,0,0,0\n", NULL, 30, 17) == 0);
    HAS(t->out, "probe: launch 1 " CRASHED "; launching again (2 of at most 17)\n");

    /* a hang: ended after the quiet time, then launched again */
    CHECK(run(t, "pass=1 hang\npass=1 summary=2,0,0,0\n", NULL, 1, 17) == 0);
    HAS(t->out,
        "probe: launch 1 was ended after 1 s with no new line in the report; launching again (2 of at most 17)\n");
    HAS(t->out, "probe: 2 passed, 0 failed, 0 skipped, 0 crashed\n");

    /* no report */
    CHECK(run(t, "none exit=1\n", NULL, 30, 17) == 1);
    HAS(g_driver_probe.said, "launch 1 ended with exit 1 and left no report: ");
    HAS(g_driver_probe.said, "launch-1.log says why");
    CHECK(!snx_path_exists(report)); /* not the last run's */

    /* a launch that adds no case */
    CHECK(run(t, "pass=1 crash\ncrash\n", NULL, 30, 17) == 1);
    HAS(g_driver_probe.said,
        "launch 2 " CRASHED " and added no case to the report: edenprobe would only end the same way again");
    HAS(t->out, "probe: no summary: 1 case reported before the run ended\n");
    HAS(t->out, ", 2 launches, settings: defaults\n");

    /* the launch cap */
    CHECK(run(t, "pass=1 crash\npass=1 crash\npass=1 crash\npass=1 crash\n", NULL, 30, 3) == 1);
    HAS(g_driver_probe.said, "3 launches and still no summary in the report: the last " CRASHED);
    CHECK(occurrences(t->calls, "game -H") == 3);
    path = in_folder(t, "launch-3.log");
    CHECK(snx_is_file(path));
    free(path);

    /* an end after the summary: the run is whole, the end said */
    CHECK(run(t, "pass=1 summary=1,0,0,0 crash\n", NULL, 30, 17) == 0);
    HAS(t->out, "probe: the program " CRASHED " after its summary: ");
    /* an earlier run's launch logs went */
    path = in_folder(t, "launch-3.log");
    CHECK(!snx_path_exists(path));
    free(path);

    /* the GPU driver picked goes to every launch */
    CHECK(run(t, "pass=1 crash\npass=1 summary=2,0,0,0\n", gpu, 30, 17) == 0);
    CHECK(occurrences(t->calls, "game -H --gpu_driver=pkg | ") == 2);
    HAS(t->out, ", 2 launches, settings: gpu_driver=pkg\n");

    /* an earlier run's card: its report, journal and crash list go, the fs family's files stay */
    path = snx_path_join(sdmc, "crashes");
    CHECK(!snx_write_file(path, "gpu case 1\n", 11));
    free(path);
    path = snx_path_join(sdmc, "runs/old");
    CHECK(!snx_mkdirs(path));
    free(path);
    path = snx_path_join(sdmc, "fs/kept");
    CHECK(!snx_mkdirs(path));
    free(path);
    CHECK(run(t, "pass=1 summary=1,0,0,0 ignored\n", NULL, 30, 17) == 0);
    path = snx_path_join(sdmc, "crashes");
    CHECK(!snx_path_exists(path));
    free(path);
    path = snx_path_join(sdmc, "runs/old");
    CHECK(!snx_path_exists(path));
    free(path);
    path = snx_path_join(sdmc, "fs/kept");
    CHECK(snx_is_dir(path));
    free(path);
    HAS(t->out, "probe: edenprobe did not know words of the mode: 1 unknown word ignored: display\n");

    /* a recompile that fails */
    append(t->nro, "another build");
    set_env("SNX_PROBE_TEST_RECOMPILE", "fail");
    CHECK(run(t, "pass=1 summary=1,0,0,0\n", NULL, 30, 17) == 1);
    HAS(g_driver_probe.said, "the recompile ended with exit 2 without making ");
    HAS(g_driver_probe.said, "recompile.log says why");
    CHECK(!strstr(t->calls, "game"));
    append(t->nro, "a third build");
    set_env("SNX_PROBE_TEST_RECOMPILE", "made");
    CHECK(run(t, "pass=1 summary=1,0,0,0\n", NULL, 30, 17) == 1);
    HAS(g_driver_probe.said, "the recompile ended with exit 2: ");
    CHECK(!strstr(t->calls, "game"));
    set_env("SNX_PROBE_TEST_RECOMPILE", "");
    CHECK(run(t, "pass=1 summary=1,0,0,0\n", NULL, 30, 17) == 0);
    HAS(t->calls, "recompile\n");

    free(report);
    free(summary);
    free(sdmc);
}

/* the command line: what it refuses, and on a system of XDG folders a run in probe/ of the state folder */
static void command_line(Test* t) {
    char* missing = snx_path_join(t->root, "none.nro");
    const char* const lone[] = {t->self, "--probe", NULL};
    const char* const slow[] = {t->self, "--probe=slow", NULL};
    const char* const twice[] = {t->self, "--probe=quick", "--probe=full", NULL};
    const char* const other[] = {t->self, "--probe=quick", "--fast", NULL};
    const char* const no_nro[] = {t->self, "--probe=quick", NULL};
    const char* const empty[] = {t->self, "--probe=quick", "--probe-nro", NULL};
    const char* const gone[] = {t->self, "--probe=full", "--probe-nro", missing, NULL};
    const char* const no_mode[] = {t->self, "--probe-nro", t->nro, NULL};
    struct {
        const char* const* argv;
        const char* says;
    } refused[] = {
        {lone, "--probe=: quick (cpu and graphics) or full (also fs, video, audio, kernel and display)"},
        {slow, "--probe=slow: quick (cpu and graphics) or full"},
        {twice, "--probe=full: --probe is given once"},
        {other, "unknown option with --probe: --fast (it takes --probe-nro FILE, --gpu_driver=ID and"},
        {no_nro, "--probe needs --probe-nro FILE, the edenprobe.nro to run"},
        {empty, "--probe-nro requires a file"},
        {gone, "none.nro: no such file"},
        {no_mode, "--probe-nro goes with --probe=quick or --probe=full"},
    };
    size_t i;
    for (i = 0; i < sizeof refused / sizeof *refused; i++) {
        int argc = 0;
        while (refused[i].argv[argc]) argc++;
        g_driver_probe.said[0] = 0;
        CHECK(snx_probe_main(argc, (char**)refused[i].argv) == 2);
        if (!strstr(g_driver_probe.said, refused[i].says)) fprintf(stderr, "said: %s", g_driver_probe.said);
        HAS(g_driver_probe.said, refused[i].says);
    }
#ifdef SNX_XDG
    {
        char *state = snx_path_join(t->root, "state"), *config = snx_path_join(t->root, "config"),
             *data = snx_path_join(t->root, "data"), *report = snx_path_join(state, "scarletnx/probe/report.jsonl");
        const char* const whole[] = {t->self, "--probe=quick", "--probe-nro", t->nro, NULL};
        set_env("XDG_STATE_HOME", state);
        set_env("XDG_CONFIG_HOME", config);
        set_env("XDG_DATA_HOME", data);
        CHECK(!snx_write_file(t->plan, "pass=1 summary=1,0,0,0\n", 23));
        g_driver_probe.said[0] = 0;
        CHECK(snx_probe_main(4, (char**)whole) == 0);
        CHECK(snx_is_file(report));
        free(state);
        free(config);
        free(data);
        free(report);
    }
#endif
    free(missing);
}

int main(int argc, char** argv) {
    Test t = {0};
    if (argc > 1 && !strcmp(argv[1], "-r")) return fake_recompile(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "-H")) return fake_game(argc, argv);
    t.self = snx_executable(argv[0]);
    t.root = snx_temp_dir(NULL);
    CHECK(t.self && t.root);
    t.nro = snx_path_join(t.root, "edenprobe.nro");
    t.folder = snx_path_join(t.root, "probe");
    t.plan = snx_path_join(t.root, "plan.txt");
    t.log = snx_path_join(t.root, "calls.txt");
    t.out_path = snx_path_join(t.root, "out.txt");
    CHECK(!snx_write_file(t.nro, "NRO0", 4));
    set_env("SNX_PROBE_TEST_PLAN", t.plan);
    set_env("SNX_PROBE_TEST_LOG", t.log);
    snx_error_sink(sink);
    runs(&t);
    command_line(&t);
    snx_error_sink(NULL);
    CHECK(!snx_remove_tree(t.root));
    printf("driver_probe: %u checks passed\n", g_driver_probe.checks);
    free(t.self);
    free(t.root);
    free(t.nro);
    free(t.folder);
    free(t.plan);
    free(t.log);
    free(t.out_path);
    free(t.out);
    free(t.calls);
    return 0;
}
