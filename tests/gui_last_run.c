/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The menu's Last Run record (gui/last_run.h): beside the run log, written whole and read back once, deleted as it is
   read; one that holds no start, or a line break in a path, refused and said. What the system says of a process's
   end, from MainActivity.exitFields' lines (a phone's low memory kill, as Android 16 gave it), and which reasons are
   Android's own stops. The kernel's OOM kill count from a memory.events' and a /proc/vmstat's text */
#include "gui/last_run.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static void check(int good, const char* expression, int line) {
    ++checks;
    if (!good) {
        fprintf(stderr, "gui_last_run.c:%d: check failed: %s\n", line, expression);
        exit(1);
    }
}
#define CHECK(value) check((value) != 0, #value, __LINE__)

static void record(const char* root) {
    char *log = snx_path_join(root, "last-run-2.log"), *path = gui_last_run_path(log), why[1024], *text;
    GuiLastRun run = {0}, back;
    size_t size;
    CHECK(path && !strcmp(path + strlen(path) - strlen("last-run-2.ini"), "last-run-2.ini"));
    CHECK(!gui_last_run_path("last-run") && !gui_last_run_path(NULL));
    /* none there */
    CHECK(gui_last_run_take(path, &back, why, sizeof why) == 0 && !back.program && back.cases == -1);
    /* a phone's Run edenprobe: its game's program, the probe's folder, its cases before this launch and its number */
    run.program = "/data/probe/.scarletnx/recompiled/edenprobe/edenprobe";
    run.name = "edenprobe";
    run.probe = "/data/probe";
    run.started = 1762989505;
    run.cases = 1456;
    run.launch = 2;
    CHECK(!gui_last_run_write(path, &run, why, sizeof why));
    text = (char*)snx_read_file(path, &size);
    CHECK(text && strstr(text, "\nstarted = 1762989505\n") && strstr(text, "\nprobe = /data/probe\n"));
    free(text);
    CHECK(gui_last_run_take(path, &back, why, sizeof why) == 1 && !snx_path_exists(path));
    CHECK(!strcmp(back.program, run.program) && !strcmp(back.name, "edenprobe") && !strcmp(back.probe, "/data/probe"));
    CHECK(back.started == 1762989505 && back.cases == 1456 && back.launch == 2);
    gui_last_run_free(&back);
    CHECK(gui_last_run_take(path, &back, why, sizeof why) == 0); /* read once */
    /* a computer's run: no program (its output names its log), no probe */
    run.program = run.probe = NULL;
    run.name = "a game.nsp";
    CHECK(!gui_last_run_write(path, &run, why, sizeof why));
    CHECK(gui_last_run_take(path, &back, why, sizeof why) == 1 && !back.program && !back.probe && back.cases == -1);
    CHECK(!strcmp(back.name, "a game.nsp"));
    gui_last_run_free(&back);
    /* a path with a line break would read back as another: refused, said with the path */
    run.name = "two\nlines";
    CHECK(gui_last_run_write(path, &run, why, sizeof why) < 0 && strstr(why, path) && strstr(why, "line break"));
    CHECK(!snx_path_exists(path));
    /* one with no start: said, and deleted all the same */
    CHECK(!snx_write_file(path, "name = x\n", 9));
    CHECK(gui_last_run_take(path, &back, why, sizeof why) < 0 && strstr(why, "holds no started =") &&
          !snx_path_exists(path));
    gui_last_run_free(&back);
    /* a folder that is not there: the write says where */
    {
        char* missing = snx_path_join(root, "not there/last-run.ini");
        run.name = "x";
        CHECK(gui_last_run_write(missing, &run, why, sizeof why) < 0 && strstr(why, "cannot be written"));
        free(missing);
    }
    free(path);
    free(log);
}

static void system_end(void) {
    GuiSystemEnd end;
    /* what the F8 gave on 2026-11-12 for the game's process, as exitFields writes it */
    gui_system_end_read("reason=3\nsaid=low memory\nprocess=org.scarletnx:game\nstatus=0\nimportance=100 foreground\n"
                        "pss=5138022\nrss=1992704\ndescription=\n",
                        &end);
    CHECK(end.known && end.reason == 3 && !strcmp(end.said, "low memory") &&
          !strcmp(end.process, "org.scarletnx:game"));
    CHECK(end.status == 0 && !strcmp(end.importance, "100 foreground") && end.pss == 5138022 && end.rss == 1992704);
    CHECK(!end.description[0] && !end.none[0]);
    gui_system_end_read("none=Android keeps no end of pid 99 since it started\n", &end);
    CHECK(!end.known && end.reason == -1 && end.pss == -1 &&
          !strcmp(end.none, "Android keeps no end of pid 99 since it "
                            "started"));
    gui_system_end_read(NULL, &end);
    CHECK(!end.known && strstr(end.none, "does not say"));
    gui_system_end_read("garbage", &end);
    CHECK(!end.known && end.none[0]);
    /* Android's own stops, and the ends that are not */
    CHECK(gui_system_stopped(3) && gui_system_stopped(6) && gui_system_stopped(9) && gui_system_stopped(13));
    CHECK(gui_system_stopped(14) && gui_system_stopped(16));
    CHECK(!gui_system_stopped(1) && !gui_system_stopped(2) && !gui_system_stopped(4) && !gui_system_stopped(5));
    CHECK(!gui_system_stopped(7) && !gui_system_stopped(10) && !gui_system_stopped(11) && !gui_system_stopped(-1));
}

static void oom(void) {
    char file[512];
    long long kills;
    CHECK(gui_oom_kills_in("low 0\nhigh 0\nmax 12\noom 3\noom_kill 2\noom_group_kill 0\n") == 2);
    CHECK(gui_oom_kills_in("pgmajfault 77\noom_kill 41\nnuma_pte_updates 0\n") == 41);
    CHECK(gui_oom_kills_in("oom 3\noom_group_kill 1\n") == -1 && gui_oom_kills_in("") == -1);
    /* this machine's: a count with where it is kept, or none and no file */
    kills = gui_oom_kills(NULL, file, sizeof file);
    CHECK(kills >= 0 ? file[0] != 0 : !file[0]);
    printf("gui_last_run: this machine's OOM kills: %lld%s%s\n", kills, file[0] ? " in " : "", file);
}

int main(void) {
    char* root = snx_temp_dir(NULL);
    CHECK(root != NULL);
    record(root);
    system_end();
    oom();
    CHECK(!snx_remove_tree(root));
    free(root);
    printf("gui_last_run: %u checks passed\n", checks);
    return 0;
}
