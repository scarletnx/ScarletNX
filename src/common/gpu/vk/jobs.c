/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/gpu/vk/jobs.h"
#include "common/gpu/vk/vk_common.h"
#include "common/utilities/common.h"
#include <stdint.h>
#include <stdio.h>

/* A worker: makes the queued jobs one by one, until the renderer stops them; low at the system's low priority */
static void work(SnxVkJobs* j, bool low) {
    char why[256];
    if (low && snx_thread_priority_low(why, sizeof why) &&
        !atomic_exchange_explicit(&j->low_refused, 1, memory_order_relaxed))
        fprintf(stderr,
                "%s: a pipeline worker asked for %s, the system's low priority: %s (said for the first one refused)\n",
                j->who, snx_thread_priority_low_name(), why);
    SDL_LockMutex(j->lock);
    while (!j->stop) {
        int job = -1;
        for (unsigned i = 0; i < SNX_VK_JOBS && job < 0 && !j->held; ++i)
            if (j->state[i] == SNX_VK_JOB_QUEUED) job = (int)i;
        if (job < 0) {
            SDL_WaitCondition(j->queued, j->lock);
            continue;
        }
        j->state[job] = SNX_VK_JOB_MAKING;
        SDL_UnlockMutex(j->lock);
        j->make(j->owner, (unsigned)job);
        SDL_LockMutex(j->lock);
        j->state[job] = SNX_VK_JOB_DONE;
        SDL_BroadcastCondition(j->done);
    }
    SDL_UnlockMutex(j->lock);
}
static int SDLCALL work_normal(void* j) {
    work(j, false);
    return 0;
}
static int SDLCALL work_low(void* j) {
    work(j, true);
    return 0;
}

unsigned snx_vk_jobs_start(SnxVkJobs* j, int threads) {
    if (j->tried) return j->count;
    j->tried = true;
    unsigned n = (unsigned)snx_vk_pipeline_workers(threads, SDL_GetNumLogicalCPUCores());
    if (n > SNX_VK_WORKERS) n = SNX_VK_WORKERS;
    unsigned low = (unsigned)snx_vk_pipeline_low_workers((int)n);
    if ((j->lock = SDL_CreateMutex()) && (j->queued = SDL_CreateCondition()) && (j->done = SDL_CreateCondition()))
        while (j->count < n) {
            SDL_Thread* t = SDL_CreateThread(j->count >= n - low ? work_low : work_normal, j->thread, j);
            if (!t) break;
            j->workers[j->count++] = t;
        }
    if (!j->count)
        fprintf(stderr,
                "%s: async_pipelines=1, but no pipeline worker could start (%s): the draws wait for their pipelines, "
                "as with async_pipelines=0\n",
                j->who, SDL_GetError());
    return j->count;
}

int snx_vk_jobs_free(SnxVkJobs* j) {
    int job = -1;
    SDL_LockMutex(j->lock);
    for (unsigned i = 0; i < SNX_VK_JOBS && job < 0; ++i)
        if (j->state[i] == SNX_VK_JOB_FREE) job = (int)i;
    SDL_UnlockMutex(j->lock);
    return job;
}

void snx_vk_jobs_queue(SnxVkJobs* j, unsigned job) {
    SDL_LockMutex(j->lock);
    j->state[job] = SNX_VK_JOB_QUEUED;
    SDL_SignalCondition(j->queued);
    SDL_UnlockMutex(j->lock);
}

bool snx_vk_jobs_take(SnxVkJobs* j, unsigned job) {
    SDL_LockMutex(j->lock);
    bool done = j->state[job] == SNX_VK_JOB_DONE;
    if (done) j->state[job] = SNX_VK_JOB_FREE;
    SDL_UnlockMutex(j->lock);
    return done;
}

void snx_vk_jobs_wait(SnxVkJobs* j, bool stopping, void (*drop)(void* owner, unsigned job)) {
    if (!j->count) return;
    SDL_LockMutex(j->lock);
    for (unsigned i = 0; i < SNX_VK_JOBS; ++i) {
        if (stopping && j->state[i] == SNX_VK_JOB_QUEUED) {
            if (drop) drop(j->owner, i);
            j->state[i] = SNX_VK_JOB_FREE;
        }
        while (j->state[i] == SNX_VK_JOB_QUEUED || j->state[i] == SNX_VK_JOB_MAKING)
            SDL_WaitCondition(j->done, j->lock);
    }
    SDL_UnlockMutex(j->lock);
}

void snx_vk_jobs_hold(SnxVkJobs* j, bool held) {
    if (j->lock) SDL_LockMutex(j->lock);
    j->held = held;
    if (j->queued) SDL_BroadcastCondition(j->queued);
    if (j->lock) SDL_UnlockMutex(j->lock);
}

void snx_vk_jobs_stop(SnxVkJobs* j, void (*drop)(void* owner, unsigned job)) {
    if (j->count) {
        snx_vk_jobs_wait(j, true, drop);
        SDL_LockMutex(j->lock);
        j->stop = true;
        SDL_BroadcastCondition(j->queued);
        SDL_UnlockMutex(j->lock);
        for (unsigned i = 0; i < j->count; ++i) SDL_WaitThread(j->workers[i], NULL);
        j->count = 0;
    }
    if (j->queued) SDL_DestroyCondition(j->queued);
    if (j->done) SDL_DestroyCondition(j->done);
    if (j->lock) SDL_DestroyMutex(j->lock);
    j->queued = j->done = NULL;
    j->lock = NULL;
}

bool snx_vk_jobs_done(const SnxVkJobs* j, unsigned job) { return j->state[job] == SNX_VK_JOB_DONE; }

void snx_vk_jobs_feed(SnxVkJobs* j, SnxVkFeed* f, bool all, void (*fill)(void* owner, unsigned job, size_t item),
                      void (*made)(void* owner, unsigned job, size_t item)) {
    for (;;) {
        for (unsigned i = 0; i < SNX_VK_JOBS; ++i)
            if (f->item[i] && snx_vk_jobs_take(j, i)) {
                made(j->owner, i, f->item[i] - 1);
                f->item[i] = 0;
                --f->making;
            }
        while (f->next < f->count && f->making < (all ? SNX_VK_JOBS : SNX_VK_JOBS / 2)) {
            int i = snx_vk_jobs_free(j);
            if (i < 0) break;
            fill(j->owner, (unsigned)i, f->next);
            f->item[i] = (uint32_t)++f->next;
            ++f->making;
            snx_vk_jobs_queue(j, (unsigned)i);
        }
        if (!all || !f->making) return;
        snx_vk_jobs_wait(j, false, NULL);
    }
}

bool snx_vk_jobs_busy(SnxVkJobs* j) {
    bool busy = false;
    if (!j->lock) return false;
    SDL_LockMutex(j->lock);
    for (unsigned i = 0; i < SNX_VK_JOBS && !busy; ++i)
        busy = j->state[i] == SNX_VK_JOB_QUEUED || j->state[i] == SNX_VK_JOB_MAKING;
    SDL_UnlockMutex(j->lock);
    return busy;
}
