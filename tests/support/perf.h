/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the performance tests share: the machine's state before a timing, a skip or a failure that follows it, a
   clock that counts only this thread, and the median and spread a ratio is judged by. A performance test's numbers
   are only comparable inside one run (a ratio of two workloads timed together), so nothing here reads a baseline
   except perf_baseline. */
#ifndef SNX_TEST_PERF_H
#define SNX_TEST_PERF_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int strict;         /* CI or SNX_PERF_REQUIRE: a timing that cannot run is a failure, never a skip */
    int processors;     /* online processors, 0 when unknown */
    double load1;       /* the last minute's run queue length, 0 when unknown */
    uint64_t memory_mb; /* free memory, 0 when unknown */
} PerfEnvironment;

typedef enum { PERF_RUN, PERF_SKIP, PERF_FAIL } PerfDecision;

/* A block of measured ratios: its median, the interquartile spread, a 95% lower bound for a warning and a 99.9%
   lower bound for a failure, both estimated from that spread. A warning may be variance and says so; a failure needs
   the strict bound in two independent blocks */
typedef struct {
    double ratio;
    double spread;
    double lower;        /* 95%: the warning line */
    double lower_strict; /* 99.9%: the failure line and the canary */
    double upper;
} PerfResult;

/* How an annotation is shown: a GitHub Actions warning or error, or a plain line away from GitHub Actions */
typedef enum { PERF_NOTICE, PERF_WARN, PERF_ERROR } PerfLevel;

/* Reads the environment (CI, SNX_PERF_REQUIRE, SNX_PERF_SKIP) and the machine's processors, load and free memory */
PerfEnvironment perf_environment(void);
/* Why this machine has too little of something a timing needs, or NULL when it has enough. It is a heuristic: one
   processor more than the run queue, and enough free memory for a test's few buffers */
const char* perf_resource_refusal(const PerfEnvironment* env);
/* PERF_RUN; PERF_SKIP with the reason printed, which a local run allows; or PERF_FAIL with it printed, which CI and
   SNX_PERF_REQUIRE allow: a skip in CI would hide that the CI machine or its build lost what the test needs */
PerfDecision perf_decide(const PerfEnvironment* env, const char* test, const char* reason);
/* The same for a measurement whose samples disagree: up to 50% of the median locally skips, up to 70% in CI fails */
PerfDecision perf_decide_noise(const PerfEnvironment* env, const char* test, double spread);

/* This thread's CPU time, which other processes' work does not add to, and a monotonic clock */
uint64_t perf_thread_cpu_ns(void);
uint64_t perf_now_ns(void);
/* Keeps the timing thread on one kind of core where the OS lets a process ask: a performance core on Apple silicon,
   the processor it already runs on under Linux. Without it a sample can span a performance and an efficiency core,
   and the ratio of two samples taken a moment apart moves by more than the code under test */
void perf_prepare_thread(void);

/* The middle sample (the lower of the two middle ones for an even count), and the interquartile spread: the third
   quartile minus the first, over the median. An outlier at either end is left out, so one slow sample cannot decide
   whether a machine was stable */
double perf_median(const double* values, size_t count);
double perf_spread(const double* values, size_t count);
/* The median, spread and 95% lower bound of a block of ratios */
PerfResult perf_result(const double* ratios, size_t count);

/* The four numbers on the line `key reference warn_percent fail_percent ceiling` of a baseline file, which may have
   comments. The warning and failure ratios are the reference raised by those percentages. 1 when found, 0 when the
   key is absent, -1 when the file cannot be read or its line is not four numbers */
int perf_baseline4(const char* path, const char* key, double* reference, double* warn_percent, double* fail_percent,
                   double* ceiling);

/* A GitHub Actions annotation (`::warning title=...::...`) when GITHUB_ACTIONS is set, which leaves a passing run
   green, or a plain line elsewhere */
void perf_annotation(PerfLevel level, const char* test, const char* message);

#endif
