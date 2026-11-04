/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Fixed native work for the sampler's independent overhead and sample-loss checks. */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
typedef struct { pthread_t thread; uint64_t iterations, seed, result; } Worker;
static void* work(void* argument) {
    Worker* worker = argument;
    uint64_t value = worker->seed;
    for (uint64_t i = 0; i < worker->iterations; ++i) {
        value ^= value << 13;
        value ^= value >> 7;
        value ^= value << 17;
    }
    worker->result = value;
    return NULL;
}
static double seconds(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (double)time.tv_sec + (double)time.tv_nsec * 1e-9;
}
int main(int argc, char** argv) {
    uint64_t iterations = argc > 1 ? strtoull(argv[1], NULL, 10) : 40000000;
    unsigned threads = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 4;
    if (!iterations || !threads || threads > 256) return 2;
    Worker* workers = calloc(threads, sizeof *workers);
    if (!workers) return 2;
    double start = seconds();
    for (unsigned i = 0; i < threads; ++i) {
        workers[i].iterations = iterations;
        workers[i].seed = 0x9e3779b97f4a7c15ull + i;
        if (pthread_create(&workers[i].thread, NULL, work, &workers[i])) return 2;
    }
    uint64_t checksum = 0;
    for (unsigned i = 0; i < threads; ++i) {
        if (pthread_join(workers[i].thread, NULL)) return 2;
        checksum += workers[i].result;
    }
    double elapsed = seconds() - start;
    free(workers);
    printf("{\"elapsed_ns\":%.0f,\"checksum\":\"%016llx\"}\n", elapsed * 1e9, (unsigned long long)checksum);
    fflush(stdout);
    if (argc > 3 && !strcmp(argv[3], "--immediate")) _exit(0);
    return 0;
}
