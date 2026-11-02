/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The launcher's logs (gui/log.h): nothing without a folder; with one, each line of what it says after the time and
   its pid in the day's scarletnx-console-YYYY-MM-DD.log, the days past GUI_LOG_DAYS deleted as a day starts and the
   versions before's scarletnx-console.log left, a fatal error in scarletnx-error.log too; two programs writing at
   once keep their lines whole, each named by its pid; a run log held by one launcher is seen taken by another. */
#include "gui/log.h"
#include "common/utilities/common.h"
#include "common/utilities/logscan.h"
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static unsigned long own_pid(void) {
#ifdef _WIN32
    return (unsigned long)GetCurrentProcessId();
#else
    return (unsigned long)getpid();
#endif
}
/* line is "YYYY-MM-DD HH:MM:SS [pid] text": the text after it, and the pid in *pid; NULL when it is not */
static const char* stamped(const char* line, unsigned long* pid) {
    static const char shape[] = "dddd-dd-dd dd:dd:dd [";
    char* end;
    for (size_t i = 0; shape[i]; ++i)
        if (shape[i] == 'd' ? !isdigit((unsigned char)line[i]) : line[i] != shape[i]) return NULL;
    *pid = strtoul(line + sizeof shape - 1, &end, 10);
    return end[0] == ']' && end[1] == ' ' ? end + 2 : NULL;
}
/* the file's lines, each checked stamped by pid (0: any), their texts joined by '|' into out; how many */
static int lines_of(const char* path, unsigned long pid, char* out, size_t size) {
    size_t length = 0;
    char* text = (char*)snx_read_file(path, &length);
    int n = 0;
    out[0] = 0;
    if (!text) return -1;
    for (char *line = text, *end; *line; line = end + 1) {
        unsigned long by = 0;
        const char* said;
        end = strchr(line, '\n');
        assert(end); /* every line ends */
        *end = 0;
        said = stamped(line, &by);
        assert(said && (!pid || by == pid));
        if (n) strncat(out, "|", size - strlen(out) - 1);
        strncat(out, said, size - strlen(out) - 1);
        ++n;
    }
    free(text);
    return n;
}

int main(void) {
    char *dir = snx_temp_dir(NULL), *console, *error, path[4096], said[4096], today[64], want[64];
    assert(dir);
    { /* the day's name, by the local date */
        time_t now = time(NULL);
        struct tm* tm = localtime(&now);
        assert(tm && strftime(want, sizeof want, "scarletnx-console-%Y-%m-%d.log", tm));
        gui_log_console_name(today, sizeof today);
        assert(!strcmp(today, want));
    }
    console = snx_path_join(dir, today);
    error = snx_path_join(dir, SNX_ERROR_LOG);

    /* no folder: stderr alone, no file anywhere */
    gui_log_folder(NULL);
    gui_log_path(SNX_CONSOLE_LOG, path, sizeof path);
    assert(!path[0]);
    gui_log("before the folder");
    gui_log_fatal("a fatal error before the folder");
    assert(!snx_path_exists(console) && !snx_path_exists(error));

    /* a folder: each line stamped with the time and this pid, a text of several lines a stamp each, the ending newline
       given; a fatal error in both */
    gui_log_folder(dir);
    gui_log_path(SNX_CONSOLE_LOG, path, sizeof path);
    assert(!strcmp(path, console));
    gui_log_path(SNX_ERROR_LOG, path, sizeof path);
    assert(!strcmp(path, error));
    /* the day's first line: of 16 days before and the old single file, the 13 newest days stay with today's (14) */
    {
        char name[64], *old;
        int day, kept = 0;
        for (day = 1; day <= 16; day++) {
            snprintf(name, sizeof name, "scarletnx-console-2025-01-%02d.log", day);
            old = snx_path_join(dir, name);
            assert(!snx_write_file(old, "old\n", 4));
            free(old);
        }
        old = snx_path_join(dir, SNX_CONSOLE_LOG);
        assert(!snx_write_file(old, "the versions before's\n", 22));
        gui_log("first %d", 1);
        assert(snx_path_exists(old));
        free(old);
        for (day = 1; day <= 16; day++) {
            snprintf(name, sizeof name, "scarletnx-console-2025-01-%02d.log", day);
            old = snx_path_join(dir, name);
            assert(!!snx_path_exists(old) == (day > 3));
            kept += snx_path_exists(old);
            free(old);
        }
        assert(kept == GUI_LOG_DAYS - 1);
    }
    gui_log("two\nlines\n");
    gui_log("a\n\nblank between");
    gui_log_fatal("the window could not be made");
    assert(lines_of(console, own_pid(), said, sizeof said) == 7);
    assert(!strcmp(said, "first 1|two|lines|a||blank between|the window could not be made"));
    assert(lines_of(error, own_pid(), said, sizeof said) == 1 && !strcmp(said, "the window could not be made"));
    /* appended: a second start keeps the first's */
    gui_log("again");
    assert(lines_of(console, own_pid(), said, sizeof said) == 8 && strstr(said, "|again"));
    /* a program's lines as Windows ends them, \r\n: their \r left out, so every line ends as the others do */
    gui_log("a driver's\r\nWindows lines\r\n");
    assert(lines_of(console, own_pid(), said, sizeof said) == 10 && strstr(said, "|again|a driver's|Windows lines"));
    {
        size_t size = 0;
        char* text = (char*)snx_read_file(console, &size);
        assert(text && !memchr(text, '\r', size));
        free(text);
    }

    /* a folder that is not there: nothing made, the line on stderr with why */
    {
        char* gone = snx_path_join(dir, "not there");
        char* file = snx_path_join(gone, SNX_CONSOLE_LOG);
        gui_log_folder(gone);
        gui_log("lost");
        assert(!snx_path_exists(file));
        free(file);
        free(gone);
        gui_log_folder(dir);
    }
#ifndef _WIN32
    /* two programs at once: every line whole, 300 from each, each named by its own pid */
    {
        enum { LINES = 300 };
        char long_text[200];
        unsigned long child_pid, by;
        int status, mine = 0, theirs = 0;
        size_t length = 0;
        char* text;
        pid_t child;
        memset(long_text, 'x', sizeof long_text - 1);
        long_text[sizeof long_text - 1] = 0;
        int null = open("/dev/null", O_WRONLY), saved = dup(STDERR_FILENO); /* their stderr copies left out */
        assert(!remove(console) && null >= 0 && saved >= 0);
        fflush(NULL);
        assert(dup2(null, STDERR_FILENO) >= 0);
        child = fork();
        assert(child >= 0);
        for (int i = 0; i < LINES; ++i) gui_log("%s %d %s", child ? "parent" : "child", i, long_text);
        if (!child) _exit(0);
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
        assert(dup2(saved, STDERR_FILENO) >= 0);
        close(saved);
        close(null);
        child_pid = (unsigned long)child;
        text = (char*)snx_read_file(console, &length);
        assert(text);
        for (char *line = text, *end; *line; line = end + 1) {
            const char* rest;
            end = strchr(line, '\n');
            assert(end);
            *end = 0;
            rest = stamped(line, &by);
            assert(rest && strlen(rest) > sizeof long_text &&
                   !strcmp(rest + strlen(rest) - strlen(long_text), long_text));
            if (by == own_pid()) {
                assert(!strncmp(rest, "parent ", 7));
                ++mine;
            } else {
                assert(by == child_pid && !strncmp(rest, "child ", 6));
                ++theirs;
            }
        }
        assert(mine == LINES && theirs == LINES);
        free(text);
    }
#endif
    /* a run log held by a launcher (snx_lock_file): another holder finds it taken, its bytes free to write and read;
       let go, it is free again; a folder not there says so */
    {
        char *held = snx_path_join(dir, "last-run.log"), *gone = snx_path_join(dir, "not there/last-run.log");
        intptr_t lock, again;
        size_t size = 0;
        char* text;
        assert(!snx_lock_file(held, &lock) && snx_path_exists(held));
        assert(snx_lock_file(held, &again) == 1);
        assert(!snx_write_file(held, "run output\n", 11));
        text = (char*)snx_read_file(held, &size);
        assert(text && size == 11);
        free(text);
#ifndef _WIN32
        {
            pid_t child;
            int status;
            fflush(NULL);
            child = fork(); /* another program: it sees the lock too */
            assert(child >= 0);
            if (!child) _exit(snx_lock_file(held, &again) == 1 ? 0 : 1);
            assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
        }
#endif
        snx_unlock_file(lock);
        assert(!snx_lock_file(held, &again));
        snx_unlock_file(again);
        assert(snx_lock_file(gone, &again) == -1);
        free(gone);
        free(held);
    }
    { /* how a game's run ended, from its log: a crash, an error, an exit, and a log that isn't there */
        char *log = snx_path_join(dir, "game.log"), what[512], lines[2048];
        static const char crash[] = "[   0.00] ScarletNX - 0100000000001000\n[   3.00]\n"
                                    "*** Crash (signal 11) at host pc=0x7f0000001234, access to 0x10 ***\n"
                                    "[   3.00]   invalid memory access\n";
        static const char fatal[] = "[   0.00] ScarletNX - 0100000000001000\n"
                                    "[   0.58] FATAL: GPU driver Turnip did not load: the system's driver took its "
                                    "place\n";
        static const char done[] = "[   0.00] ScarletNX - 0100000000001000\n[  10.00] Exit.\n";
        assert(!snx_write_file(log, crash, sizeof crash - 1));
        assert(gui_log_game_end(log, what, sizeof what, lines, sizeof lines) == SNX_LOG_CRASH);
        assert(!strcmp(what, "crash (signal 11)") && strstr(lines, "invalid memory access\n"));
        assert(!snx_write_file(log, fatal, sizeof fatal - 1));
        assert(gui_log_game_end(log, what, sizeof what, lines, sizeof lines) == SNX_LOG_FATAL);
        assert(!strcmp(what, "GPU driver Turnip did not load: the system's driver took its place"));
        assert(!snx_write_file(log, done, sizeof done - 1));
        assert(gui_log_game_end(log, what, sizeof what, lines, sizeof lines) == SNX_LOG_NONE);
        assert(!snx_remove_tree(log));
        assert(gui_log_game_end(log, what, sizeof what, lines, sizeof lines) == -1);
        assert(!strncmp(what, log, strlen(log)) && strstr(what, " did not open: ") && !lines[0]);
        free(log);
    }
    gui_log_folder(NULL);
    free(console);
    free(error);
    assert(!snx_remove_tree(dir));
    free(dir);
    puts("gui_log: stamped lines, a fatal error in both logs, two writers at once, how a game's run ended");
    return 0;
}
