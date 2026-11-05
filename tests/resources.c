/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the log's lines on the process's resources compute (runtime/platform/resources.c). The threads line: nothing at
   the first call; then the three threads with the most time on the processors in the window, most first, a thread
   new in the window by all its time, one whose id came back with less time than the window had by all its time too,
   "?" for one with no name, "none ran" for a window where no thread ran, nothing for a thread that a full window did
   not keep, the count of every thread listed past the ones kept, and a line cut to its room */
#include "nx/runtime/platform/resources.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define S 1000000000ull

static SnxThreadInfo thread(SnxU64 id, double seconds, const char* name) {
    SnxThreadInfo t = {0};
    t.id = id;
    t.cpu_ns = (SnxU64)(seconds * 1e9 + 0.5);
    snprintf(t.name, sizeof t.name, "%s", name);
    return t;
}
static void expect(const char* got, const char* want) {
    if (strcmp(got, want)) {
        fprintf(stderr, "got  \"%s\"\nwant \"%s\"\n", got, want);
        assert(0);
    }
}

static struct {
    ThreadsWindow window;
    SnxThreadInfo many[THREADS_SEEN + 2];
} g_test;

int main(void) {
    char line[512];
    ThreadsWindow* w = &g_test.window;
    SnxThreadInfo first[] = {thread(1, 1.0, "GPU"), thread(2, 0.5, "audio"), thread(4, 5.0, "gone"),
                             thread(5, 0.25, "")};
    SnxThreadInfo second[] = {thread(1, 3.0, "GPU"),         thread(2, 0.6, "audio"), thread(3, 0.7, "pipelines"),
                              thread(4, 0.2, "took 4's id"), thread(5, 0.25, ""),     thread(6, 0.0, "new, idle")};
    SnxThreadInfo third[] = {thread(1, 3.0, "GPU"), thread(5, 0.5, "")};
    SnxThreadInfo quiet[] = {thread(1, 3.0, "GPU"), thread(5, 0.5, "")};

    line[0] = 0;
    assert(!threads_line_text(w, first, 4, 10 * S, line, sizeof line) && !line[0]);
    assert(threads_line_text(w, second, 6, 15 * S, line, sizeof line));
    expect(line, "threads: the most CPU in the last 5.0 s, of 6 threads: GPU 2.00 s, pipelines 0.70 s, took 4's id "
                 "0.20 s");
    assert(threads_line_text(w, third, 2, 20 * S, line, sizeof line));
    expect(line, "threads: the most CPU in the last 5.0 s, of 2 threads: ? 0.25 s");
    assert(threads_line_text(w, quiet, 2, 25 * S, line, sizeof line));
    expect(line, "threads: the most CPU in the last 5.0 s, of 2 threads: none ran");

    /* a full window, then a thread it could not keep: past the table, so none of its time is the window's */
    for (unsigned i = 0; i < THREADS_SEEN + 2; i++) g_test.many[i] = thread(100 + i, 1.0, "many");
    assert(threads_line_text(w, g_test.many, THREADS_SEEN + 2, 30 * S, line, sizeof line));
    assert(strstr(line, "of 514 threads: many 1.00 s, many 1.00 s, many 1.00 s"));
    g_test.many[THREADS_SEEN - 1] = thread(9999, 4.0, "unkept");
    g_test.many[0].cpu_ns += S / 2;
    assert(threads_line_text(w, g_test.many, THREADS_SEEN, 35 * S, line, sizeof line));
    expect(line, "threads: the most CPU in the last 5.0 s, of 512 threads: many 0.50 s");

    /* the line cut to its room, ended */
    memset(line, 'x', sizeof line);
    assert(threads_line_text(w, second, 6, 40 * S, line, 24));
    assert(strlen(line) == 23 && !strncmp(line, "threads: the most CPU i", 23));
    printf("resources: the threads lines are right\n");
    return 0;
}
