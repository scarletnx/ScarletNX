/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A start's mark (common/utilities/run_mark.h): made held at a start, deleted at the end; another start finds one no
   program holds, says its text, pid and start, and deletes it, and leaves one held, other files and a mark being made;
   the older iOS app's logs/.running; a folder longer than the 1024 bytes the iOS mark had. A look at a folder's marks
   gives each, held or not, and deletes none. On POSIX, a child that ends with exit() leaves none, one that ends with
   _exit (a kill, a crash) leaves its own for the next start */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* fork and waitpid, which strict C99 leaves undeclared */
#endif
#include "common/utilities/common.h"
#include "common/utilities/run_mark.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

static unsigned checks;
static void check(int good, const char* expression, int line) {
    ++checks;
    if (!good) {
        fprintf(stderr, "run_mark.c:%d: check failed: %s\n", line, expression);
        exit(1);
    }
}
#define CHECK(value) check((value) != 0, #value, __LINE__)

/* what a start found, a copy of the last mark said and how many */
typedef struct {
    int count, opened, held;
    long long pid, started;
    char text[256], left[2048];
} Seen;
static void seen(void* data, const SnxRunMark* mark) {
    Seen* s = (Seen*)data;
    ++s->count;
    s->opened = mark->text != NULL;
    s->held += mark->held;
    s->pid = mark->pid;
    s->started = mark->started;
    snprintf(s->text, sizeof s->text, "%s", mark->text ? mark->text : "");
    snprintf(s->left, sizeof s->left, "%s", mark->left ? mark->left : "");
}
static void write_text(const char* folder, const char* name, const char* text) {
    char* path = snx_path_join(folder, name);
    CHECK(!snx_write_file(path, text, strlen(text)));
    free(path);
}
static int there(const char* folder, const char* name) {
    char* path = snx_path_join(folder, name);
    int is = snx_path_exists(path);
    free(path);
    return is;
}
/* this process's own mark's name */
static void own_name(char* out, size_t size) { snprintf(out, size, ".running-%lld", (long long)getpid()); }

static void made_and_ended(const char* folder) {
    Seen s = {0};
    char why[2048], own[64], *path, *text;
    size_t size = 0;
    long long pid = 0, started = 0;
    own_name(own, sizeof own);
    CHECK(!snx_run_mark_held());
    CHECK(snx_run_mark_start(folder, seen, &s, why, sizeof why) == 0 && !why[0] && s.count == 0);
    CHECK(snx_run_mark_held() && there(folder, own));
    /* its text: the pid and the local time, then that time in seconds */
    path = snx_path_join(folder, own);
    text = (char*)snx_read_file(path, &size);
    CHECK(text && sscanf(text, "pid %lld, started ", &pid) == 1 && pid == (long long)getpid());
    CHECK(strchr(text, '\n') && sscanf(strchr(text, '\n') + 1, "%lld", &started) == 1 && started > 1700000000ll);
    free(text);
    /* a second start of this process: its mark stays, the folder is not read again */
    write_text(folder, ".running-77", "pid 77, started 2026-01-01 00:00:00\n1767225600\n");
    CHECK(snx_run_mark_start(folder, seen, &s, why, sizeof why) == 1 && s.count == 0 && there(folder, ".running-77"));
    snx_run_mark_end();
    CHECK(!snx_run_mark_held() && !snx_path_exists(path));
    snx_run_mark_end(); /* again: nothing */
    CHECK(!snx_path_exists(path));
    free(path);
    /* the next start says .running-77, a start with no exit, and deletes it */
    CHECK(snx_run_mark_start(folder, seen, &s, why, sizeof why) == 0 && s.count == 1);
    CHECK(s.pid == 77 && s.started == 1767225600ll && !strcmp(s.text, "pid 77, started 2026-01-01 00:00:00"));
    CHECK(!s.left[0] && !there(folder, ".running-77") && there(folder, own));
    snx_run_mark_end();
}

/* one held by another program is left, and so are other files and a mark being made; logs/.running, the older iOS
   app's mark, is said with the pid of its text */
static void found(const char* folder) {
    Seen s = {0};
    char why[2048], *held = snx_path_join(folder, ".running-424242"), *unopened;
    intptr_t lock;
    write_text(folder, ".running-424242", "pid 424242, started 2026-02-02 02:02:02\n1770000000\n");
    CHECK(!snx_lock_file(held, &lock));
    write_text(folder, ".running-5.new", "pid 5, started 2026-03-03 03:03:03\n1772500000\n");
    write_text(folder, ".running-12x", "not a mark\n");
    write_text(folder, ".running-", "not a mark\n");
    write_text(folder, "notes.txt", "not a mark\n");
    write_text(folder, ".running", "pid 777, started 2026-11-06 10:00:00\n");
    CHECK(snx_run_mark_start(folder, seen, &s, why, sizeof why) == 0 && s.count == 1);
    CHECK(s.pid == 777 && s.started == 0 && !strcmp(s.text, "pid 777, started 2026-11-06 10:00:00"));
    CHECK(!there(folder, ".running") && there(folder, ".running-424242") && there(folder, ".running-5.new"));
    CHECK(there(folder, ".running-12x") && there(folder, ".running-") && there(folder, "notes.txt"));
    snx_run_mark_end();
    /* its holder gone, the next start says it */
    snx_unlock_file(lock);
    s.count = 0;
    CHECK(snx_run_mark_start(folder, seen, &s, why, sizeof why) == 0 && s.count == 1 && s.pid == 424242);
    CHECK(s.started == 1770000000ll && !there(folder, ".running-424242"));
    snx_run_mark_end();
    /* an empty one is a start that ended as it made its mark: said with no text */
    write_text(folder, ".running-31", "");
    s.count = 0;
    CHECK(snx_run_mark_start(folder, seen, &s, why, sizeof why) == 0 && s.count == 1 && s.pid == 31);
    CHECK(s.opened && !s.text[0] && s.started == 0 && !s.left[0] && !there(folder, ".running-31"));
    snx_run_mark_end();
    /* one that cannot be opened (a folder of that name, root or not): said with why, its end unknown, and left */
    unopened = snx_path_join(folder, ".running-55");
    CHECK(!snx_mkdirs(unopened));
    s.count = 0;
    CHECK(snx_run_mark_start(folder, seen, &s, why, sizeof why) == 0 && s.count == 1 && s.pid == 55 && !s.opened);
    CHECK(strstr(s.left, unopened) && strstr(s.left, "cannot be opened (") && snx_is_dir(unopened));
    snx_run_mark_end();
    CHECK(!snx_remove_tree(unopened));
    free(unopened);
    free(held);
}

/* a look gives each mark, the held one with held set and no text, the others as a start would, and deletes none: a
   launcher's at those of a game it started, which the game's next start says */
static void looked(const char* folder) {
    Seen s = {0};
    char why[2048], *held = snx_path_join(folder, ".running-616161"), *missing = snx_path_join(folder, "missing");
    intptr_t lock;
    write_text(folder, ".running-616161", "pid 616161, started 2026-11-12 20:18:25\n1762989505\n");
    CHECK(!snx_lock_file(held, &lock));
    write_text(folder, ".running-19590.new", "pid 19590, started 2026-11-12 20:18:25\n1762989505\n");
    CHECK(snx_run_mark_look(folder, seen, &s, why, sizeof why) == 0 && !why[0]);
    CHECK(s.count == 1 && s.held == 1 && s.pid == 616161 && !s.opened && !s.left[0]);
    snx_unlock_file(lock);
    write_text(folder, ".running-19590", "pid 19590, started 2026-11-12 20:18:25\n1762989505\n");
    s.count = s.held = 0;
    CHECK(snx_run_mark_look(folder, seen, &s, why, sizeof why) == 0 && s.count == 2 && !s.held);
    CHECK(there(folder, ".running-19590") && there(folder, ".running-616161") && there(folder, ".running-19590.new"));
    /* both again, as nothing was deleted; then the start that says and deletes them */
    s.count = 0;
    CHECK(snx_run_mark_look(folder, seen, &s, why, sizeof why) == 0 && s.count == 2);
    s.count = 0;
    CHECK(snx_run_mark_start(folder, seen, &s, why, sizeof why) == 0 && s.count == 2);
    CHECK(!there(folder, ".running-19590") && !there(folder, ".running-616161"));
    snx_run_mark_end();
    /* a folder that is not there: said */
    CHECK(snx_run_mark_look(missing, seen, &s, why, sizeof why) < 0 && strstr(why, missing));
    free(missing);
    free(held);
}

/* a folder past the 1024 bytes the iOS mark's path had: its mark is made and deleted there. Where the system takes no
   such path (Windows without its long paths), the start says the whole path */
static void long_folder(const char* root) {
    Seen s = {0};
    char why[4096], own[64], part[201], *folder = snx_strdup(root), *next, *path;
    int i, made;
    memset(part, 'm', sizeof part - 1);
    part[sizeof part - 1] = 0;
    for (i = 0; i < 6; ++i) {
        next = snx_path_join(folder, part);
        free(folder);
        folder = next;
    }
    CHECK(strlen(folder) > 1024 + 64);
    own_name(own, sizeof own);
    path = snx_path_join(folder, own);
    if (!snx_mkdirs(folder)) {
        made = snx_run_mark_start(folder, seen, &s, why, sizeof why);
#ifdef _WIN32 /* a program without the long paths' manifest: the folder made, its files refused */
        CHECK(made == 0 ? snx_is_file(path) : made < 0 && strstr(why, folder) && !snx_run_mark_held());
#else
        CHECK(made == 0 && snx_is_file(path));
#endif
        snx_run_mark_end();
        CHECK(!snx_path_exists(path));
    } else {
        made = snx_run_mark_start(folder, seen, &s, why, sizeof why);
        CHECK(made < 0 && strstr(why, folder) && !snx_run_mark_held());
        printf("run_mark: %s cannot be made here, and the start says it whole\n", folder);
    }
    free(path);
    free(folder);
}

#ifndef _WIN32
/* the parent's paths while a child runs: its exit() runs the leak check, which reads a global as reachable */
static const char* g_held[2];
/* a child that starts its mark in folder and ends with exit() (1) or _exit (0) */
static pid_t child(const char* folder, int clean) {
    pid_t pid = fork();
    if (pid == 0) {
        Seen s = {0};
        char why[2048];
        if (snx_run_mark_start(folder, seen, &s, why, sizeof why)) _exit(9);
        if (clean) exit(0);
        _exit(3);
    }
    return pid;
}
static void children(const char* folder) {
    Seen s = {0};
    char why[2048], name[64];
    int status = 0;
    pid_t pid = child(folder, 1);
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    snprintf(name, sizeof name, ".running-%lld", (long long)pid);
    CHECK(!there(folder, name));
    pid = child(folder, 0);
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 3);
    snprintf(name, sizeof name, ".running-%lld", (long long)pid);
    CHECK(there(folder, name));
    CHECK(snx_run_mark_start(folder, seen, &s, why, sizeof why) == 0 && s.count == 1 && s.pid == (long long)pid);
    CHECK(!there(folder, name));
    snx_run_mark_end();
}
#endif

int main(void) {
    char *root = snx_temp_dir(NULL), *folder;
    Seen s = {0};
    char why[2048];
    CHECK(root != NULL);
    folder = snx_path_join(root, "logs");
    CHECK(!snx_mkdirs(folder));
    made_and_ended(folder);
    found(folder);
    looked(folder);
    long_folder(root);
#ifndef _WIN32
    g_held[0] = root;
    g_held[1] = folder;
    children(folder);
#endif
    /* a folder that is not there: said, with no mark made */
    free(folder);
    folder = snx_path_join(root, "not there");
    CHECK(snx_run_mark_start(folder, seen, &s, why, sizeof why) < 0 && strstr(why, folder) && !snx_run_mark_held());
    free(folder);
    CHECK(!snx_remove_tree(root));
    free(root);
    printf("run_mark: %u checks passed\n", checks);
    return 0;
}
