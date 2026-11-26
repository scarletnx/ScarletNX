/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A renderer's background compiles (common/gpu/vk/jobs.c): the workers pipeline_threads asks for, each queued job
   made once and taken once, a test's hold keeping them from new jobs, a queued job dropped as they stop */
#include "common/gpu/vk/jobs.h"
#include <stdio.h>
#include <string.h>

static int g_failed;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "vk_jobs: %s:%d: %s\n", __FILE__, __LINE__, #x);                                           \
            g_failed = 1;                                                                                              \
        }                                                                                                              \
    } while (0)

static int g_made[SNX_VK_JOBS], g_dropped[SNX_VK_JOBS];
static void make(void* owner, unsigned job) {
    (void)owner;
    ++g_made[job];
}
static void drop(void* owner, unsigned job) {
    (void)owner;
    ++g_dropped[job];
}

int main(void) {
    static SnxVkJobs jobs = {.make = make, .who = "vk_jobs", .thread = "test jobs"};
    if (!SDL_Init(0)) return 77;
    CHECK(snx_vk_jobs_start(&jobs, 3) == 3 && snx_vk_jobs_start(&jobs, 5) == 3); /* started once */
    /* three jobs, each made once and taken once, their slots free again */
    int a = snx_vk_jobs_free(&jobs);
    CHECK(a == 0);
    snx_vk_jobs_queue(&jobs, (unsigned)a);
    int b = snx_vk_jobs_free(&jobs), c;
    CHECK(b == 1);
    snx_vk_jobs_queue(&jobs, (unsigned)b);
    c = snx_vk_jobs_free(&jobs);
    snx_vk_jobs_queue(&jobs, (unsigned)c);
    snx_vk_jobs_wait(&jobs, false, drop);
    CHECK(!snx_vk_jobs_busy(&jobs) && g_made[0] == 1 && g_made[1] == 1 && g_made[2] == 1);
    CHECK(snx_vk_jobs_take(&jobs, 0) && !snx_vk_jobs_take(&jobs, 0) && snx_vk_jobs_free(&jobs) == 0);
    CHECK(snx_vk_jobs_take(&jobs, 1) && snx_vk_jobs_take(&jobs, 2));
    /* held, a queued job waits; let go, it is made */
    snx_vk_jobs_hold(&jobs, true);
    snx_vk_jobs_queue(&jobs, 0);
    CHECK(snx_vk_jobs_busy(&jobs) && g_made[0] == 1);
    snx_vk_jobs_hold(&jobs, false);
    snx_vk_jobs_wait(&jobs, false, drop);
    CHECK(g_made[0] == 2 && snx_vk_jobs_take(&jobs, 0));
    /* held as they stop: the queued job dropped, never made */
    snx_vk_jobs_hold(&jobs, true);
    snx_vk_jobs_queue(&jobs, 5);
    snx_vk_jobs_stop(&jobs, drop);
    CHECK(g_dropped[5] == 1 && g_made[5] == 0 && !jobs.count && !jobs.lock);
    SDL_Quit();
    if (!g_failed) puts("vk_jobs: ok");
    return g_failed;
}
