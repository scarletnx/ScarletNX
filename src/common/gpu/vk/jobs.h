/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A renderer's background compiles, async_pipelines and pipeline_threads, the Switch's settings: threads that run each
   queued job's make while the renderer goes on, the draws that need it skipped until it is done. The jobs are slots
   whose states this keeps; what each makes and what it made are the renderer's, in its own array by the job's index,
   which a worker touches only between the job's queue and its done */
#ifndef SNX_COMMON_VK_JOBS_H
#define SNX_COMMON_VK_JOBS_H
#include <SDL3/SDL.h>
#include <stdatomic.h>
#include <stdbool.h>

#define SNX_VK_JOBS    64 /* the jobs in hand at once */
#define SNX_VK_WORKERS 32 /* pipeline_threads's most */
enum { SNX_VK_JOB_FREE, SNX_VK_JOB_QUEUED, SNX_VK_JOB_MAKING, SNX_VK_JOB_DONE };

typedef struct {
    /* the renderer's: its job's thing made, on a worker (owner the renderer's state, job the index) */
    void (*make)(void* owner, unsigned job);
    void* owner;
    const char* who;    /* before each line: "3ds gpu" */
    const char* thread; /* the workers' name: "3DS pipelines" */
    /* the workers, started at the first job; their lock and the conditions a job queued or done waits on */
    SDL_Thread* workers[SNX_VK_WORKERS];
    unsigned count;
    bool tried, stop, held;
    SDL_Mutex* lock;
    SDL_Condition *queued, *done;
    uint8_t state[SNX_VK_JOBS]; /* SNX_VK_JOB_*, under lock */
    _Atomic int low_refused;    /* a worker's low priority was refused, which the first one refused says */
} SnxVkJobs;

/* The workers, started the first time: threads (pipeline_threads) of them, or the common count, the ones past the
   first two at the system's low priority. How many there are; with none the renderer makes its things itself, as
   with async_pipelines=0, which the log says once */
unsigned snx_vk_jobs_start(SnxVkJobs* j, int threads);
/* a free job's index for the renderer to fill and queue, -1 with every one in hand */
int snx_vk_jobs_free(SnxVkJobs* j);
/* job, filled, to the workers */
void snx_vk_jobs_queue(SnxVkJobs* j, unsigned job);
/* whether a worker made job: then it is free again, what it made the renderer's to take from its array at once */
bool snx_vk_jobs_take(SnxVkJobs* j, unsigned job);
/* until no job is queued or being made. Stopping, the queued ones dropped first (drop, where given, frees what the
   renderer put in one) */
void snx_vk_jobs_wait(SnxVkJobs* j, bool stopping, void (*drop)(void* owner, unsigned job));
/* a test's hold: while held the workers take no job */
void snx_vk_jobs_hold(SnxVkJobs* j, bool held);
/* the workers stopped, after the queued jobs are dropped and the ones begun done; the done ones left for the renderer
   (snx_vk_jobs_done), then the lock and conditions freed */
void snx_vk_jobs_stop(SnxVkJobs* j, void (*drop)(void* owner, unsigned job));
/* whether a job is queued or being made */
bool snx_vk_jobs_busy(SnxVkJobs* j);
/* whether job is done, for the renderer's own collection as it stops */
bool snx_vk_jobs_done(const SnxVkJobs* j, unsigned job);
#endif
