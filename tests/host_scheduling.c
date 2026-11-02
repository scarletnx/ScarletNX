/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Exercise real kernel controls in disposable children. Policies never escape into the CTest process. */
#include "nx/runtime/platform/scheduling.c"
#include <assert.h>
#include <stdarg.h>
#include <sys/wait.h>

void logf_(int level, const char* fmt, ...) {
    (void)level;
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

#if defined(SYS_sched_getattr) && defined(SYS_sched_setattr)
static void run_mode(int mode, int batch) {
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        if (batch) {
            struct sched_param param = {0};
            assert(!sched_setscheduler(0, SCHED_BATCH, &param));
        }
        LinuxSchedAttr before, after;
        if (sched_read(&before)) _exit(77);
        cpu_set_t affinity_before, affinity_after;
        assert(!sched_getaffinity(0, sizeof affinity_before, &affinity_before));
        g_settings.cpu_scheduling = mode;
        snx_thread_performance(SNX_WORK_GPU);
        snx_thread_performance((SnxThreadWork)SNX_WORK_COUNT);
        assert(!sched_read(&after));
        assert(!sched_getaffinity(0, sizeof affinity_after, &affinity_after));
        assert(CPU_EQUAL(&affinity_before, &affinity_after));
        assert(after.policy == before.policy && after.priority == before.priority);
        assert(after.deadline == before.deadline && after.period == before.period);
        if (!mode || batch) {
            assert(!memcmp(&before, &after, sizeof before));
        } else {
            /* A restricted host may reject a feature, but must leave that feature unchanged. */
            assert(after.util_min == 1024 || after.util_min == before.util_min);
            assert(after.util_max == 1024 || after.util_max == before.util_max);
            assert(after.nice <= before.nice);
            assert(after.runtime == before.runtime || (mode == 2 && after.runtime == 100000));
            if (mode == 1) assert(after.nice == before.nice);
        }
        fprintf(stderr, "mode %d batch %d: floor %u slice %llu nice %d\n", mode, batch,
                after.util_min, after.runtime, after.nice);
        _exit(0);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 77);
}

int main(void) {
    LinuxSchedAttr attr;
    if (sched_read(&attr)) return 77;
    run_mode(0, 0);
    run_mode(1, 0);
    run_mode(2, 0);
    run_mode(2, 1);
    puts("host_scheduling: ok");
    return 0;
}

#else
int main(void) {
    puts("SKIP host scheduling controls: system call declarations unavailable");
    return 77;
}
#endif
