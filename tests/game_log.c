/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A console's game log (common/utilities/game_log.c), each case a process of its own, as a run is (this program again,
   with the case's name): 8 threads' lines at once, all in the file once and stamped, and on the terminal, through one
   pipe (stdout and stderr one file) and two; a crash (a signal; a handler's last line, then _exit) whose last line is
   the log's; a terminal that refuses (a pipe nobody reads) or is not there (closed), the file keeping every line; the
   cut past log_max keeping the run's start and its last line; and the 4 runs before in logs/ */
#include "common/platform/thread.h"
#include "common/platform/time.h"
#include "common/utilities/common.h"
#include "common/utilities/game_log.h"
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

#define THREADS   8
#define LINES     400
#define CUT_LINES 30000

static unsigned g_checks, g_failures;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        g_checks++;                                                                                                    \
        if (!(c)) {                                                                                                    \
            g_failures++;                                                                                              \
            printf("FAIL game_log.c:%d: ", __LINE__);                                                                  \
            printf(__VA_ARGS__);                                                                                       \
            putchar('\n');                                                                                             \
        }                                                                                                              \
    } while (0)

/* ---- the cases, each in its own process */
static int g_limit = 16;
static _Atomic int g_running;
static void lines(void* arg) {
    int t = (int)(intptr_t)arg;
    for (int i = 0; i < LINES; ++i) fprintf(i & 1 ? stderr : stdout, "thread %d line %d\n", t, i);
    atomic_fetch_sub_explicit(&g_running, 1, memory_order_acq_rel);
}
static int start(const char* dir) {
    char why[1600];
    if (snx_game_log_start(dir, "a game", &g_limit, why, sizeof why)) {
        fprintf(stderr, "game_log: %s\n", why);
        return 1;
    }
    return 0;
}
static int run_case(const char* name, const char* dir) {
    if (!strcmp(name, "threads")) {
        if (start(dir)) return 1;
        atomic_store_explicit(&g_running, THREADS, memory_order_release);
        for (int t = 0; t < THREADS; ++t)
            if (plat_thread(lines, (void*)(intptr_t)t, 0)) return 1;
        while (atomic_load_explicit(&g_running, memory_order_acquire)) plat_yield();
        puts("Exit.");
        return 0;
    }
    if (!strcmp(name, "signal") || !strcmp(name, "handler")) {
#if !defined(_WIN32)
        struct rlimit none = {0, 0}; /* no core file of a crash on purpose */
        setrlimit(RLIMIT_CORE, &none);
#endif
        if (start(dir)) return 1;
        for (int i = 0; i < 50; ++i) printf("a line before the crash, %d\n", i);
        fprintf(stderr, "the last line before the crash\n");
        if (!strcmp(name, "handler")) { /* as the 3DS's data abort ends a run: its line, then _exit, from a handler */
            static const char line[] = "3ds: guest data abort at 0x000001234 in the block at 0x00100000\n";
            snx_game_log_crash(line, sizeof line - 1);
            _exit(1);
        }
        raise(SIGSEGV);
        return 0; /* never: the signal ends the process */
    }
    if (!strcmp(name, "refused") || !strcmp(name, "closed")) {
        int ends[2];
#if defined(_WIN32)
        if (!strcmp(name, "closed") || _pipe(ends, 4096, _O_BINARY)) return 1;
        _close(ends[0]);
        _dup2(ends[1], 1), _dup2(ends[1], 2);
#else
        if (!strcmp(name, "closed"))
            close(1), close(2);
        else {
            if (pipe(ends)) return 1;
            close(ends[0]); /* nobody reads: each write of the terminal's side fails */
            dup2(ends[1], 1), dup2(ends[1], 2);
        }
#endif
        if (start(dir)) return 1;
        for (int i = 0; i < LINES; ++i) printf("no terminal, line %d\n", i);
        puts("Exit.");
        return 0;
    }
    if (!strcmp(name, "cut")) {
        g_limit = 1;
        if (start(dir)) return 1;
        for (int i = 0; i < CUT_LINES; ++i)
            printf("cut line %06d .................................................................................\n",
                   i);
        snx_game_log_sync();
        printf("full=%d then %d\n", snx_game_log_full_take(), snx_game_log_full_take());
        return 0;
    }
    if (!strcmp(name, "run")) {
        if (start(dir)) return 1;
        printf("run %s\n", getenv("SNX_TEST_RUN") ? getenv("SNX_TEST_RUN") : "?");
        return 0;
    }
    fprintf(stderr, "game_log: no case %s\n", name);
    return 2;
}

/* ---- the checks, in the first process */
static char* g_self;
/* this program on case name in dir, its stdout into dir's file out and its stderr into err, the same one, or both kept
   as they are (out NULL) */
static int child(const char* name, const char* dir, const char* out, const char* err) {
    char* redirect = !out                ? snx_strdup("")
                     : !strcmp(out, err) ? snx_format("> \"%s/%s\" 2>&1", dir, out)
                                         : snx_format("> \"%s/%s\" 2> \"%s/%s\"", dir, out, dir, err);
    char* command = snx_format("\"%s\" %s \"%s\" %s", g_self, name, dir, redirect);
    free(redirect);
#if defined(_WIN32)
    char* whole = snx_format("\"%s\"", command); /* cmd.exe drops the first and last quote */
    free(command);
    command = whole;
#endif
    int status = system(command);
    free(command);
    return status;
}
static char* slurp(const char* dir, const char* file, size_t* n) {
    char* path = snx_path_join(dir, file);
    size_t size = 0;
    char* text = path ? (char*)snx_read_file(path, &size) : NULL;
    free(path);
    if (n) *n = text ? size : 0;
    return text;
}
static int count(const char* text, const char* what) {
    int n = 0;
    for (const char* at = text; at && (at = strstr(at, what)) != NULL; at += strlen(what)) n++;
    return n;
}
/* the text's last line, without its end */
static void last_line(const char* text, char* out, size_t size) {
    size_t n = strlen(text);
    while (n && (text[n - 1] == '\n' || text[n - 1] == '\r')) --n;
    size_t from = n;
    while (from && text[from - 1] != '\n') --from;
    snprintf(out, size, "%.*s", (int)(n - from), text + from);
}
/* each line of text starts with the "[seconds] " stamp */
static int stamped(const char* text) {
    for (const char* line = text; *line;) {
        const char *end = strchr(line, '\n'), *closing = strstr(line, "] ");
        if (!end) end = line + strlen(line);
        if (line[0] != '[' || !closing || closing > end) return 0;
        line = *end ? end + 1 : end;
    }
    return 1;
}
static char* fresh(const char* root, const char* name) {
    char* dir = snx_path_join(root, name);
    if (dir) (void)snx_remove_tree(dir);
    if (!dir || snx_mkdirs(dir)) return NULL;
    return dir;
}
static void threads(const char* root, const char* name, const char* term, const char* term2) {
    char* dir = fresh(root, name);
    char *out = NULL, *err = NULL, line[64];
    CHECK(dir && child("threads", dir, term, term2 ? term2 : term) == 0, "%s: the case failed", name);
    char* log = dir ? slurp(dir, "a game.log", NULL) : NULL;
    if (dir) out = slurp(dir, term, NULL), err = term2 ? slurp(dir, term2, NULL) : NULL;
    CHECK(log && stamped(log), "%s: a line of the log has no stamp", name);
    int missing = 0, twice = 0, lost = 0;
    for (int t = 0; log && out && t < THREADS; ++t)
        for (int i = 0; i < LINES; ++i) {
            snprintf(line, sizeof line, "] thread %d line %d\n", t, i);
            int n = count(log, line);
            missing += n == 0, twice += n > 1;
            snprintf(line, sizeof line, "thread %d line %d\n", t, i);
            lost += count(out, line) + (err ? count(err, line) : 0) != 1;
        }
    CHECK(!missing && !twice, "%s: of %d lines, %d not in the log and %d twice", name, THREADS * LINES, missing, twice);
    CHECK(!lost, "%s: %d lines not once on the terminal", name, lost);
    CHECK(log && strstr(log, "log: written to ") && count(log, "] Exit.\n") == 1, "%s: the log's first and last lines",
          name);
    CHECK(out && !strstr(out, "] thread"), "%s: the terminal's lines are stamped", name);
    free(log), free(out), free(err), free(dir);
}
static void crash(const char* root, const char* name, const char* last) {
    char* dir = fresh(root, name);
    char before[64], end[256];
    CHECK(dir && child(name, dir, "term.txt", "term.txt") != 0, "%s: the crash ended the run well", name);
    char *log = dir ? slurp(dir, "a game.log", NULL) : NULL, *term = dir ? slurp(dir, "term.txt", NULL) : NULL;
    int lines = 0;
    for (int i = 0; log && i < 50; ++i) {
        snprintf(before, sizeof before, "] a line before the crash, %d\n", i);
        lines += count(log, before) == 1;
    }
    CHECK(lines == 50, "%s: %d of the 50 lines before the crash in the log", name, lines);
    CHECK(log && count(log, "] the last line before the crash\n") == 1, "%s: the last line before the crash", name);
    if (log) last_line(log, end, sizeof end);
    CHECK(log && strstr(end, "] ") && !strcmp(strstr(end, "] ") + 2, last), "%s: the log ends with \"%s\", not \"%s\"",
          name, log ? end : "(no log)", last);
    CHECK(term && strstr(term, last), "%s: the terminal lacks \"%s\"", name, last);
    free(log), free(term), free(dir);
}
static void quiet(const char* root, const char* name) {
    char* dir = fresh(root, name);
    char line[64];
    CHECK(dir && child(name, dir, NULL, NULL) == 0, "%s: the case failed", name);
    char* log = dir ? slurp(dir, "a game.log", NULL) : NULL;
    int lines = 0;
    for (int i = 0; log && i < LINES; ++i) {
        snprintf(line, sizeof line, "] no terminal, line %d\n", i);
        lines += count(log, line) == 1;
    }
    CHECK(lines == LINES, "%s: %d of the %d lines in the log", name, lines, LINES);
    CHECK(log && count(log, "] Exit.\n") == 1, "%s: the log lacks the run's Exit.", name);
    if (!strcmp(name, "refused"))
        CHECK(log && count(log, "] log: the terminal refused the run's output (") == 1,
              "%s: the log does not say the terminal refused", name);
    free(log), free(dir);
}
static void cut(const char* root) {
    char* dir = fresh(root, "cut");
    char end[256];
    size_t n = 0;
    CHECK(dir && child("cut", dir, "term.txt", "term.txt") == 0, "cut: the case failed");
    char *log = dir ? slurp(dir, "a game.log", &n) : NULL, *term = dir ? slurp(dir, "term.txt", NULL) : NULL;
    CHECK(log && n <= (1 << 20) && n >= (1 << 19) - 4096,
          "cut: the log holds %zu bytes, past log_max=1 MiB or under half", n);
    CHECK(log && strstr(log, "log: written to ") && strstr(log, "] cut line 000000 "),
          "cut: the run's start is gone from the log");
    CHECK(log && count(log, "log: reached log_max=1 MiB: ") >= 1, "cut: no line says what the cut took");
    if (log) last_line(log, end, sizeof end);
    CHECK(log && strstr(end, "] full=1 then 0"), "cut: the log ends \"%s\", not with the first cut's full=1 once", end);
    CHECK(log && strstr(log, "] cut line 029999 "), "cut: the last line is gone from the log");
    CHECK(term && count(term, "cut line ") == CUT_LINES, "cut: the terminal lacks lines");
    free(log), free(term), free(dir);
}
static void runs(const char* root) {
    char* dir = fresh(root, "runs");
    char word[16], **names = NULL;
    size_t listed = 0, kept = 0;
    for (int run = 1; dir && run <= 6; ++run) {
        snprintf(word, sizeof word, "%d", run);
#if defined(_WIN32)
        _putenv_s("SNX_TEST_RUN", word);
#else
        setenv("SNX_TEST_RUN", word, 1);
#endif
        CHECK(child("run", dir, "term.txt", "term.txt") == 0, "runs: run %d failed", run);
    }
    char* log = dir ? slurp(dir, "a game.log", NULL) : NULL;
    CHECK(log && count(log, "] run 6\n") == 1, "runs: the log is not the last run's");
    char* logs = dir ? snx_path_join(dir, "logs") : NULL;
    int seen[7] = {0};
    if (logs && !snx_list_dir(logs, &names, &listed))
        for (size_t i = 0; i < listed; ++i) {
            char* name = snx_format("logs/%s", names[i]);
            char* text = name ? slurp(dir, name, NULL) : NULL;
            for (int run = 1; text && run <= 6; ++run) {
                snprintf(word, sizeof word, "] run %d\n", run);
                seen[run] += count(text, word);
            }
            kept += strlen(names[i]) > 4 && !strcmp(names[i] + strlen(names[i]) - 4, ".log");
            free(text), free(name);
        }
    CHECK(kept == 4, "runs: logs/ keeps %zu runs' logs, not the 4 before this one", kept);
    CHECK(!seen[1] && seen[2] == 1 && seen[3] == 1 && seen[4] == 1 && seen[5] == 1 && !seen[6],
          "runs: logs/ holds runs 1-6 %d %d %d %d %d %d times, not the 4 before the last once", seen[1], seen[2],
          seen[3], seen[4], seen[5], seen[6]);
    snx_free_strings(names, listed);
    free(logs), free(log), free(dir);
}

int main(int argc, char** argv) {
    if (argc == 3) return run_case(argv[1], argv[2]);
    g_self = snx_executable(argv[0]);
    char *root = snx_temp_root(), *dir = root ? snx_temp_dir(root) : NULL;
    CHECK(g_self && dir, "no folder for the cases");
    if (!g_self || !dir) return 1;
    threads(dir, "one-pipe", "term.txt", NULL);
    threads(dir, "two-pipes", "out.txt", "err.txt");
    crash(dir, "signal", "*** Crash: signal 11");
    crash(dir, "handler", "3ds: guest data abort at 0x000001234 in the block at 0x00100000");
    quiet(dir, "refused");
#if !defined(_WIN32)
    quiet(dir, "closed");
#endif
    cut(dir);
    runs(dir);
    if (!g_failures) (void)snx_remove_tree(dir);
    printf("game_log: %u checks, %u failed%s%s\n", g_checks, g_failures, g_failures ? "; the cases' files in " : "",
           g_failures ? dir : "");
    free(dir), free(root), free(g_self);
    return g_failures != 0;
}
