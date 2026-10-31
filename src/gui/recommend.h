/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GUI_RECOMMEND_H
#define SNX_GUI_RECOMMEND_H
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
} RecProbe;
/* p cleared: nothing said yet */
void rec_probe_start(RecProbe* p);
/* one line of the probe's output into p: "probe: STEP" or key=value; an unknown key is left out */
void rec_probe_line(RecProbe* p, const char* line);
/* whether the probe gave what each rule needs */
int rec_gpu_tested(const RecProbe* p);
int rec_pipelines_tested(const RecProbe* p);
int rec_memory_tested(const RecProbe* p);

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
#endif
