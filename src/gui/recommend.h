/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GUI_RECOMMEND_H
#define SNX_GUI_RECOMMEND_H
#include <stddef.h>
/* Recommended Settings: what scarletnx --probe says of this computer, a line at a time, and the settings its rules
   give, each with the numbers that decided it */
#define REC_THREADS 3 /* the numbers of threads the probe tries at most */
typedef struct {
    char step[16]; /* the step the probe is in: gpu, pipelines, memory; done once it ended */
    char gpu[128], gpu_driver[192], gpu_type[16];
    char gpu_error[256], pipelines_error[256], ram_error[128];
    int bc, astc, budget_from_driver;
    long heap_mb, budget_mb, ram_mb; /* -1 until said */
    int auto_threads, threads[REC_THREADS], n_threads;
    double rates[REC_THREADS]; /* new pipelines a second, each number of threads' */
    int cpus, cores;
    char ended[192]; /* why the probe stopped before its last step, in the launcher's words: "" while it runs, or once
                        it said done */
} RecProbe;
/* p cleared: nothing said yet */
void rec_probe_start(RecProbe* p);
/* one line of the probe's output into p: "probe: STEP" or key=value; an unknown key is left out */
void rec_probe_line(RecProbe* p, const char* line);
/* whether the probe gave what each rule needs */
int rec_gpu_tested(const RecProbe* p);
int rec_pipelines_tested(const RecProbe* p);
int rec_memory_tested(const RecProbe* p);
/* how far the probe went, for the screen that waits on it: each step, GPU and its memory, Pipelines and RAM, waiting,
   running or done with what it found (or stopped, with p's ended), between "; " */
void rec_progress(const RecProbe* p, char* out, size_t size);

/* the settings the rules give, in the order Recommended Settings shows them */
enum { REC_ASTC_BC, REC_PIPELINE_THREADS, REC_MEMORY_FIT, REC_COUNT };
typedef struct {
    const char* key; /* the setting's key in the game's .ini */
    int tested;      /* the probe gave what the rule needs; when not, value is "" and why says why */
    char value[16];  /* the value the rule gives, as the .ini writes it */
    char why[384];   /* the numbers that decided it, translated */
} RecRow;
/* the rules on p, memory_gb the game's Memory (GB) setting (the pool memory_fit compares with) */
void rec_rules(const RecProbe* p, int memory_gb, RecRow rows[REC_COUNT]);
/* the budget under which ASTC as BC7 is proposed, and the memory_fit pool for memory_gb, in MB */
#define REC_ASTC_BC_BUDGET_MB 8192
long rec_memory_fit_under_mb(int memory_gb);

/* recommended.ini, beside settings.ini: [recommended] the values Apply gave, which Settings marks [recommended] while a
   row's pick is that value; [proposed] each row's value the last test gave, applied or not; [tested] when and what it
   saw. Only the launcher writes it */
#define REC_FILE "recommended.ini"
typedef struct {
    char applied[REC_COUNT][16];  /* [recommended], "" for a key not there */
    char proposed[REC_COUNT][16]; /* [proposed], "" for a key not there */
    char date[24];                /* [tested]'s date, "" before a test */
} RecFile;
/* how a row stands against the game's settings, effective the value they have (the pick, else the default) and
   proposed the value the last test gave it ([proposed], "" for none): not tested; that value already; a value the
   player kept, the test giving what it gave last time; proposed, its box checked to start with */
enum { REC_ROW_UNTESTED, REC_ROW_SAME, REC_ROW_KEPT, REC_ROW_PROPOSED };
int rec_row_state(const RecRow* row, const char* effective, const char* proposed);
extern const char* const rec_keys[REC_COUNT]; /* each row's key, in the rows' order */
/* the step the probe is in, as the screen names it ("Pipelines"); "the start" before its first */
const char* rec_step_name(const RecProbe* p);
/* the index of key among the rows (REC_ASTC_BC...), -1 for another */
int rec_key(const char* key);
/* path into f: 0, an empty f where there is no file; nonzero, f empty and why the system's words, for one there that
   cannot be read */
int rec_file_read(RecFile* f, const char* path, char* why, size_t size);
/* f, and what p saw on build, into path, staged beside it and put in place: 0, else nonzero with the system's words in
   why */
int rec_file_write(const RecFile* f, const RecProbe* p, const char* build, const char* path, char* why, size_t size);
#endif
