/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx-shaderperf's shared state: the configuration main.c reads from the command line, and the results gpu.c measures
   for main.c to print */
#ifndef SNX_SHADERPERF_BENCH_H
#define SNX_SHADERPERF_BENCH_H
#include "reflect.h"
#include "stats.h"
#include "texfmt.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SP_MAX_SHADERS 16
#define SP_MAX_ARGS 256
#define SP_MAX_RTS 8

/* "target=value" arguments: a binding "S.B", a push constant offset or member name, a spec constant id or name, a
   render target "rtN" or "depth" */
typedef struct {
    const char* target;
    const char* value;
} SpArg;

typedef enum { SP_ORDER_INTERLEAVE, SP_ORDER_SHUFFLE, SP_ORDER_SEQUENTIAL } SpOrder;

typedef struct {
    /* the shaders measured: each a SPIR-V file, "path#entry" picks an entry point */
    const char* shaders[SP_MAX_SHADERS];
    size_t shader_count;
    const char* entry;
    /* the other stage of a graphics pipeline, "path#entry": a vertex shader for all fragment shaders or one each */
    const char* vs[SP_MAX_SHADERS];
    size_t vs_count;
    const char* fs;
    /* device */
    const char* loader;
    const char* device; /* index or a part of the name */
    bool validate, robust, driver_cache, compute_queue;
    const char* extensions[32];
    size_t extension_count;
    /* problem size: the grid of a compute shader (workgroups from its local size) or the framebuffer */
    uint32_t width, height, depth;
    uint32_t dispatch[3];
    bool have_dispatch;
    /* graphics */
    uint32_t samples;
    const char* rt_formats[SP_MAX_RTS];
    const char* depth_format;
    bool depth_test, depth_write, clear;
    const char* blend; /* none, alpha, add */
    const char* topology;
    uint32_t vertices, instances;
    const char* index; /* none, seq, random */
    const char* vertex_fill;
    /* resources */
    SpArg binds[SP_MAX_ARGS];
    size_t bind_count;
    SpArg pushes[SP_MAX_ARGS];
    size_t push_count;
    const char* push_file;
    SpArg specs[SP_MAX_ARGS];
    size_t spec_count;
    SpArg dumps[SP_MAX_ARGS];
    size_t dump_count;
    uint64_t default_buffer; /* bytes of a buffer the command line doesn't size */
    uint32_t runtime_count;  /* descriptors of a runtime array */
    /* timing */
    uint32_t runs, min_runs, max_runs, warmup, reps;
    bool reps_auto;
    double sample_ms, warmup_ms, min_time_ms, max_time_ms, target_rse, sleep_ms;
    SpOrder order;
    bool overlap;
    int outliers;
    uint64_t seed;
    uint32_t subgroup_size;
    bool full_subgroups;
    /* queries */
    bool pipeline_stats, exec_stats, check, smi, calibrate;
    const char* exec_ir_dir;
    const char* counters; /* comma-separated names, or "all" */
    uint32_t counter_runs;
    uint32_t compile_runs;
    /* output */
    const char* json_path; /* "-" for stdout */
    const char* csv_path;
    bool quiet, samples_out;
} SpConfig;

typedef struct {
    char name[128];
    char value[64];
    char desc[192];
} SpExecStat;

typedef struct {
    char name[64], desc[192];
    uint32_t subgroup_size;
    VkShaderStageFlags stages;
    SpExecStat* stats;
    size_t stat_count;
} SpExec;

typedef struct {
    char name[128], unit[24], category[64];
    SpStats stats;
} SpCounter;

typedef struct {
    char target[32];
    uint64_t hash;
    size_t bytes;
} SpHash;

/* one shader's results */
typedef struct {
    const char* label;
    const char* path;
    SrModule mod;
    uint32_t* words;
    size_t size;
    double* samples;  /* ns per repetition, one a submission */
    double* totals;   /* ns of a submission, timestamps */
    double* wall;     /* ns from the submit to the fence, the CPU's clock */
    double* latency;  /* ns from the submit to the GPU's first timestamp (calibrated), NAN without */
    size_t sample_count, sample_cap;
    SpStats time, total, wall_stats, latency_stats;
    double* compile; /* ms of each pipeline creation */
    SpStats compile_stats;
    SpExec* execs;
    size_t exec_count;
    const char* stat_names[16];
    uint64_t pipeline_stats[16]; /* per repetition, of the last submission */
    size_t pipeline_stat_count;
    SpCounter* counters;
    size_t counter_count;
    SpHash hashes[SP_MAX_ARGS];
    size_t hash_count;
    uint64_t invocations; /* shader invocations a repetition: compute threads, fragments of the framebuffer or vertices */
    uint32_t dispatch[3];
} SpShader;

typedef struct {
    char name[256], driver[128], driver_info[128], api[32], driver_version[32];
    uint32_t vendor, device_id, type;
    double timestamp_period;
    uint32_t timestamp_bits, subgroup_size;
    bool perf_query, exec_props, calibrated, stats_query;
    char time_domains[128];
} SpDeviceInfo;

typedef struct {
    SpConfig cfg;
    SpShader shaders[SP_MAX_SHADERS];
    size_t shader_count;
    SpDeviceInfo dev;
    uint32_t reps; /* the repetitions a submission ran */
    char smi_before[256], smi_after[256];
    char** warnings;
    size_t warning_count, warning_cap;
    char** resources; /* what each binding was given, one line each */
    size_t resource_count, resource_cap;
    double elapsed_ms;
} SpBench;

void sp_warn(SpBench* b, const char* fmt, ...);
void sp_note_resource(SpBench* b, const char* fmt, ...);
/* parses a byte size with an optional K, M or G suffix (powers of 1024) */
bool sp_parse_size(const char* s, uint64_t* out);

/* gpu.c: lists the devices; prints the device's information; lists its performance counters; lists the formats with
   their support. Each returns the exit status */
int sp_gpu_list_devices(const SpConfig* cfg);
int sp_gpu_info(SpBench* b);
int sp_gpu_list_counters(SpBench* b);
int sp_gpu_list_formats(SpBench* b);
/* runs the whole benchmark: false on an error, which it printed */
bool sp_gpu_run(SpBench* b);
#endif
