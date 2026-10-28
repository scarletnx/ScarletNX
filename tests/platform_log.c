/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The log (platform/plat.c): nothing on disk before the game's module is loaded, the run before's log moved into
   logs/ by its date and logs/ kept to the last runs, the lines from before then at the top of the log (only the first
   ones when they don't all fit), the time stamp only at the start of a line, a cut line still ending its line, the
   per-thread redirect, LOG_LIMIT, fatal, and crash mode reaching the log */
#include "nx/runtime/platform/plat.c"
#include <setjmp.h>
#include <stdlib.h>

const SnxModule* g_mod;
char g_statedir[1024], g_exename[256];
static u64 g_clock;
static jmp_buf g_exit_jump;
static char g_box[256];
u64 plat_ns(void) { return g_clock += 10000000u; }
void plat_exit_now(int code) { longjmp(g_exit_jump, code + 1); }
void win_fatal_box(const char* msg) { snprintf(g_box, sizeof g_box, "%s", msg); }

static unsigned checks, failures;
#define CHECK(c, what)                                                                                                 \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            failures++;                                                                                                \
            printf("FAIL line %d: %s\n", __LINE__, what);                                                              \
        }                                                                                                              \
    } while (0)

static char g_text[65536];
/* read back in text mode, as the log, the console and the redirect are written: on Windows each "\n" is "\r\n" on
   disk */
static const char* slurp(const char* path) {
    FILE* f = fopen(path, "r");
    size_t n = 0;
    g_text[0] = 0;
    if (f) {
        n = fread(g_text, 1, sizeof g_text - 1, f);
        fclose(f);
    }
    g_text[n] = 0;
    return g_text;
}
static int count(const char* text, const char* what) {
    int n = 0;
    for (; (text = strstr(text, what)) != NULL; text++) n++;
    return n;
}

int main(int argc, char** argv) {
    static SnxModule mod;
    char log[1200], err[1200], redir[1200], line[4000];
    const char *t, *p;
    FILE* r;
    int i, code, kept = 0, contiguous = 1;
    const char* dir = argc > 1 ? argv[1] : ".";
    char runs[1200], path[1400], stamp[32];
    const SnxU64 before = 1767225600; /* 2026-01-01 00:00:00 UTC */
    snprintf(log, sizeof log, "%s/LogTest.log", dir);
    snprintf(err, sizeof err, "%s/LogTest.stderr", dir);
    snprintf(redir, sizeof redir, "%s/LogTest.redirect", dir);
    snprintf(runs, sizeof runs, "%s/logs", dir);
    remove(log);
    snx_remove_tree(runs);
    if (!freopen(err, "w", stderr)) return 2;

    /* the run before's log, last written at a known time, and five older runs' in logs/ beside a file of the
       player's */
    {
        static const char* const older[] = {"2025-01-01_00-00-00.log",   "2025-01-03_00-00-00.log",
                                            "2025-01-03_00-00-00-2.log", "2025-01-04_00-00-00.log",
                                            "2025-01-05_00-00-00.log",   "notes.txt"};
        FILE* f = fopen(log, "w");
        CHECK(f && fputs("the run before\n", f) >= 0 && !fclose(f), "the run before's log was not written");
        CHECK(!snx_set_file_time(log, before), "the run before's log has no time");
        CHECK(!snx_mkdirs(runs), "logs/ was not made");
        for (i = 0; i < 6; i++) {
            snprintf(path, sizeof path, "%s/%s", runs, older[i]);
            f = fopen(path, "w");
            CHECK(f && !fclose(f), older[i]);
        }
    }

    /* before the module: stderr only, and kept for the log (60-byte lines: they don't all fit in the 4 KiB) */
    for (i = 0; i < 100; i++) LOG("early %02d ..........................................\n", i);
    CHECK(!strcmp(slurp(log), "the run before\n"), "the log was written before the module is loaded");
    g_mod = &mod;
    snprintf(g_statedir, sizeof g_statedir, "%s/", dir);
    snprintf(g_exename, sizeof g_exename, "LogTest");
    LOG("after the module\n");
    t = slurp(log);
    CHECK(!strncmp(t, "[", 1) && strstr(t, "] early 00 "), "the log does not start with the first early line");
    for (i = 0; i < 100; i++) {
        char tag[16];
        snprintf(tag, sizeof tag, "early %02d ", i);
        if (strstr(t, tag)) {
            if (kept != i) contiguous = 0;
            kept++;
        }
    }
    CHECK(kept > 10 && kept < 100 && contiguous, "the early lines kept are not the first ones without a gap");
    CHECK(strstr(t, "] after the module\n") != NULL, "the line after the module is missing");
    CHECK(!strstr(t, "the run before"), "the log kept the run before's lines");
    /* the run before's log in logs/, named by when it was last written (the local time), and the runs' logs there
       down to the 4 newest: with this run's, 5; the player's file stays */
    {
        time_t when = (time_t)before;
        struct tm tm;
        static const char* const gone[] = {"2025-01-01_00-00-00.log", "2025-01-03_00-00-00.log"};
        static const char* const stay[] = {"2025-01-03_00-00-00-2.log", "2025-01-04_00-00-00.log",
                                           "2025-01-05_00-00-00.log", "notes.txt"}; /* -2 is the later of a second */
        localtime_r(&when, &tm);
        strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", &tm);
        snprintf(path, sizeof path, "%s/%s.log", runs, stamp);
        CHECK(!strcmp(slurp(path), "the run before\n"), "the run before's log is not in logs/ by its date");
        for (i = 0; i < 2; i++) {
            snprintf(path, sizeof path, "%s/%s", runs, gone[i]);
            CHECK(!snx_path_exists(path), gone[i]);
        }
        for (i = 0; i < 4; i++) {
            snprintf(path, sizeof path, "%s/%s", runs, stay[i]);
            CHECK(snx_path_exists(path), stay[i]);
        }
    }

    /* a line too long for the buffer still ends its line */
    memset(line, 'x', sizeof line - 1);
    line[sizeof line - 1] = 0;
    LOG("%s\n", line);
    LOG("next line\n");
    /* one stamp for a line written in two parts */
    LOG("first part, ");
    LOG("second part\n");
    t = slurp(log);
    p = strstr(t, "xxxx");
    CHECK(p && strstr(p, "x\n[") && strstr(p, "] next line\n"), "a cut line was glued to the next");
    CHECK(strstr(t, "] first part, second part\n") && !strstr(t, "first part, ["), "a split line got two stamps");

    /* the redirect takes this thread's lines away from the log */
    r = fopen(redir, "w");
    tls_log_redirect = r;
    LOG("into the redirect\n");
    tls_log_redirect = NULL;
    fclose(r);
    CHECK(!strstr(slurp(log), "into the redirect"), "a redirected line reached the log");
    CHECK(!strcmp(slurp(redir), "into the redirect\n"), "the redirect did not get its line");

    for (i = 0; i < 5; i++) LOG_LIMIT(2, "limited\n");
    for (i = 0; i < 5; i++) WARN_LIMIT(3, "warned\n");
    t = slurp(log);
    CHECK(count(t, "limited") == 2 && count(t, "warned") == 3, "LOG_LIMIT or WARN_LIMIT let the wrong count through");

    /* fatal: the log, the box, and an exit with 1, even with a redirect set */
    r = fopen(redir, "w");
    tls_log_redirect = r;
    code = setjmp(g_exit_jump);
    if (!code) fatal("boom %d", 42);
    fclose(r);
    CHECK(code == 2, "fatal did not exit with 1");
    CHECK(!strcmp(g_box, "boom 42"), "fatal did not show its box");
    CHECK(strstr(slurp(log), "] FATAL: boom 42\n") != NULL, "fatal's line is not in the log");

    log_crash_mode();
    LOG("in crash mode\n");
    CHECK(strstr(slurp(log), "] in crash mode\n") != NULL, "a crash mode line is not in the log");
    CHECK(strstr(slurp(err), "early 99") && strstr(g_text, "in crash mode"), "stderr did not get every line");

    printf("%u checks, %u failed\n", checks, failures);
    return failures != 0;
}
