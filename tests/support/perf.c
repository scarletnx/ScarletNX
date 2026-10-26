/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#define _DEFAULT_SOURCE 1 /* getloadavg, and the clock ids, in a build that keeps extensions on */
#if defined(__linux__)
#define _GNU_SOURCE 1 /* sched_setaffinity and sched_getcpu */
#endif
#include "perf.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <pthread/qos.h>
#elif defined(__linux__)
#include <sched.h>
#endif
#endif

void perf_prepare_thread(void) {
#if defined(__APPLE__)
    /* CPUTime counts wall time only while the thread runs, so a sample on an efficiency core is slower without being
       less accurate. The interactive class keeps a timing thread on the performance cores and does not raise its
       priority above other work */
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#elif defined(__linux__)
    /* The processor this thread is on now. Being there is not a guarantee of speed, but staying there removes the
       migration between cores of different widths that makes two samples of the same work differ */
    int processor = sched_getcpu();
    if (processor >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET((unsigned)processor, &set);
        sched_setaffinity(0, sizeof set, &set);
    }
#endif
}

static int env_on(const char* name) {
    const char* value = getenv(name);
    return value && value[0] && strcmp(value, "0") && strcmp(value, "false") && strcmp(value, "no");
}

uint64_t perf_thread_cpu_ns(void) {
#ifdef _WIN32
    FILETIME creation, exit, kernel, user;
    ULARGE_INTEGER k, u;
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) return perf_now_ns();
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    return (k.QuadPart + u.QuadPart) * 100u; /* 100 ns units */
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts)) return perf_now_ns();
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
#endif
}

uint64_t perf_now_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER frequency, count;
    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&count)) return 0;
    return (uint64_t)((double)count.QuadPart * 1e9 / (double)frequency.QuadPart);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
#endif
}

PerfEnvironment perf_environment(void) {
    PerfEnvironment env;
    memset(&env, 0, sizeof env);
    env.strict = env_on("SNX_PERF_REQUIRE") || env_on("CI");
#ifdef _WIN32
    {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        env.processors = (int)info.dwNumberOfProcessors;
    }
    {
        MEMORYSTATUSEX status;
        status.dwLength = sizeof status;
        if (GlobalMemoryStatusEx(&status)) env.memory_mb = status.ullAvailPhys >> 20;
    }
#else
    {
        long processors = sysconf(_SC_NPROCESSORS_ONLN);
        long pages = -1;
        long page_size;
        if (processors > 0) env.processors = (int)processors;
#if defined(__linux__)
        {
            /* MemAvailable is what a new allocation can get without swapping, which MemFree alone understates when
               page cache is holding most of the machine */
            FILE* meminfo = fopen("/proc/meminfo", "r");
            char line[128];
            if (meminfo) {
                while (fgets(line, sizeof line, meminfo)) {
                    unsigned long long kb;
                    if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) {
                        env.memory_mb = kb >> 10;
                        break;
                    }
                }
                fclose(meminfo);
            }
        }
#endif
#ifdef _SC_AVPHYS_PAGES
        pages = sysconf(_SC_AVPHYS_PAGES);
#endif
        page_size = sysconf(_SC_PAGESIZE);
        if (!env.memory_mb && pages > 0 && page_size > 0) env.memory_mb = (uint64_t)pages * (uint64_t)page_size >> 20;
#if defined(__APPLE__)
        if (!env.memory_mb) {
            vm_statistics64_data_t statistics;
            mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
            if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&statistics, &count) ==
                KERN_SUCCESS) {
                uint64_t free_pages = (uint64_t)statistics.free_count + statistics.inactive_count;
                env.memory_mb = (free_pages * (uint64_t)page_size) >> 20;
            }
        }
#endif
        {
            double load;
            if (getloadavg(&load, 1) == 1) env.load1 = load;
        }
    }
#endif
    return env;
}

const char* perf_resource_refusal(const PerfEnvironment* env) {
    static char reason[192];
    if (env->processors > 0 && env->processors < 2) {
        snprintf(reason, sizeof reason, "%d online processor: a timing needs two, so that one can stay idle",
                 env->processors);
        return reason;
    }
    if (!env->strict && env->load1 > 0 && env->processors > 0 && env->load1 > (double)env->processors - 1.0) {
        snprintf(reason, sizeof reason, "load average %.2f on %d processors leaves no processor idle for a timing",
                 env->load1, env->processors);
        return reason;
    }
    if (env->memory_mb && env->memory_mb < 128) {
        snprintf(reason, sizeof reason, "%llu MiB of memory available: a timing needs 128 MiB",
                 (unsigned long long)env->memory_mb);
        return reason;
    }
    return NULL;
}

PerfDecision perf_decide(const PerfEnvironment* env, const char* test, const char* reason) {
    if (!env->strict && env_on("SNX_PERF_SKIP")) {
        printf("perf: %s: skipped (SNX_PERF_SKIP)\n", test);
        return PERF_SKIP;
    }
    if (!reason) return PERF_RUN;
    if (env->strict) {
        printf("perf: %s: FAIL: this machine cannot be timed: %s\n", test, reason);
        return PERF_FAIL;
    }
    printf("perf: %s: skipped: %s\n", test, reason);
    return PERF_SKIP;
}

PerfDecision perf_decide_noise(const PerfEnvironment* env, const char* test, double spread) {
    if (!env->strict && env_on("SNX_PERF_SKIP")) {
        printf("perf: %s: skipped (SNX_PERF_SKIP)\n", test);
        return PERF_SKIP;
    }
    if (spread <= 0.50) return PERF_RUN;
    if (env->strict && spread <= 0.70) return PERF_RUN;
    if (env->strict) {
        printf("perf: %s: FAIL: the middle samples differ by %.0f%% (the limit is 70%% in CI): the machine is too "
               "busy for a timing\n",
               test, spread * 100);
        return PERF_FAIL;
    }
    printf("perf: %s: skipped: the middle samples differ by %.0f%% (the limit is 50%%), so the machine is too busy "
           "for a timing\n",
           test, spread * 100);
    return PERF_SKIP;
}

double perf_median(const double* values, size_t count) {
    double sorted[1024];
    size_t i, j, n = count > 1024 ? 1024 : count;
    if (!n) return 0;
    for (i = 0; i < n; ++i) {
        double value = values[i];
        j = i;
        while (j && sorted[j - 1] > value) {
            sorted[j] = sorted[j - 1];
            --j;
        }
        sorted[j] = value;
    }
    return sorted[(n - 1) / 2];
}

double perf_spread(const double* values, size_t count) {
    double sorted[1024];
    double median;
    size_t i, j, n = count > 1024 ? 1024 : count;
    if (n < 2) return 0;
    for (i = 0; i < n; ++i) {
        double value = values[i];
        j = i;
        while (j && sorted[j - 1] > value) {
            sorted[j] = sorted[j - 1];
            --j;
        }
        sorted[j] = value;
    }
    median = sorted[(n - 1) / 2];
    if (median <= 0) return 0;
    return (sorted[(3 * n) / 4] - sorted[n / 4]) / median;
}

PerfResult perf_result(const double* ratios, size_t count) {
    PerfResult result;
    double se;
    result.ratio = perf_median(ratios, count);
    result.spread = perf_spread(ratios, count);
    /* the interquartile spread estimates 1.349 standard deviations, and the median's standard error is about 1.253
       of them over the square root of the count: 0.929 times the spread over the root. The lower bound is the
       median less 1.96 of that, so a tier needs a shift larger than the block's own noise */
    se = count ? 0.929 * result.spread / sqrt((double)count) : 0;
    result.lower = result.ratio - 1.96 * se;
    if (result.lower < 0) result.lower = 0;
    result.lower_strict = result.ratio - 3.29 * se;
    if (result.lower_strict < 0) result.lower_strict = 0;
    result.upper = result.ratio + 1.96 * se;
    return result;
}

void perf_annotation(PerfLevel level, const char* test, const char* message) {
    static const char* names[] = {"notice", "warning", "error"};
    unsigned which = (unsigned)level > 2 ? 2 : (unsigned)level;
    if (env_on("GITHUB_ACTIONS"))
        printf("::%s title=%s::%s\n", names[which], test, message);
    else
        printf("perf: %s: %s: %s\n", test, names[which], message);
    fflush(stdout);
}

int perf_baseline4(const char* path, const char* key, double* reference, double* warn_percent, double* fail_percent,
                   double* ceiling) {
    FILE* file = fopen(path, "r");
    char line[512];
    if (!file) return -1;
    while (fgets(line, sizeof line, file)) {
        char name[64];
        double r, w, f, c;
        int fields;
        char* start = line;
        while (*start == ' ' || *start == '\t') ++start;
        if (*start == '#' || *start == '\n' || !*start) continue;
        fields = sscanf(start, "%63s %lf %lf %lf %lf", name, &r, &w, &f, &c);
        if (fields < 1 || strcmp(name, key)) continue;
        if (fields != 5) {
            fclose(file);
            return -1;
        }
        *reference = r;
        *warn_percent = w;
        *fail_percent = f;
        *ceiling = c;
        fclose(file);
        return 1;
    }
    fclose(file);
    return 0;
}
