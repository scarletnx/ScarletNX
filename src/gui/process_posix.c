/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#define _GNU_SOURCE
#ifndef __FreeBSD__
#define _POSIX_C_SOURCE 200809L
#endif
#ifdef __NetBSD__
#define _NETBSD_SOURCE /* sysctl's KERN_PROC2, which the POSIX name above hides */
#endif
#ifdef __APPLE__
#define _DARWIN_C_SOURCE /* libproc's types, which the POSIX name above hides */
#endif
#include "gui/gui_process.h"
#include "common/utilities/common.h"
#include "gui/i18n.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#ifdef __FreeBSD__
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#endif
#ifdef __NetBSD__
#include <sys/proc.h>
#include <sys/sysctl.h>
#endif
#ifdef __APPLE__
#include <TargetConditionals.h>
#if !TARGET_OS_IPHONE
#include <libproc.h>
#include <mach/mach_time.h>
#endif
#endif

typedef struct {
    pid_t pid, session;
    uint64_t resident;
    double cpu;
    bool live;
} Sample;
#if defined(__APPLE__) && TARGET_OS_IPHONE
/* iOS starts no process: the driver is this program's own (snx_driver), run on a thread of its own with the
   output it prints (this process's stdout and stderr, for as long as it runs) in the pipe the menu reads */
#define GUI_DRIVER_THREAD 1
int snx_driver(int argc, char** argv);
#endif
struct GuiProcess {
    pid_t pid;
    int output, status;
    bool exited, eof;
    bool low; /* the tree's priority under this program's (gui_process_low), the next start's too */
    GuiCpuBase cpu_base, tree_base;
#ifdef GUI_DRIVER_THREAD
    pthread_t thread;
    int argc, saved[2], writer;
    char** argv;
    _Atomic int done;
#endif
};
static double seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}
/* Session membership survives a driver's death/reparenting and includes the
   separate process groups made by ScarletNX's extraction/recompilation stages. */
static Sample* snapshot(size_t* count) {
    *count = 0;
#if defined(__linux__)
    DIR* dir = opendir("/proc");
    if (!dir) return NULL;
    Sample* samples = NULL;
    size_t capacity = 0;
    struct dirent* entry;
    long ticks = sysconf(_SC_CLK_TCK), page = sysconf(_SC_PAGESIZE);
    if (ticks <= 0 || page <= 0) {
        closedir(dir);
        return NULL;
    }
    while ((entry = readdir(dir))) {
        char* end;
        long pid = strtol(entry->d_name, &end, 10);
        if (*end || pid <= 0) continue;
        char path[80], buffer[4096];
        snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        size_t n = fread(buffer, 1, sizeof(buffer) - 1, f);
        fclose(f);
        buffer[n] = 0;
        /* comm may contain spaces and parentheses. Fields start after the LAST ')'. */
        char* fields = strrchr(buffer, ')');
        if (!fields) continue;
        Sample sample = {.pid = (pid_t)pid};
        char* save = NULL;
        char* token = strtok_r(fields + 1, " ", &save);
        int field = 3;
        for (; token && field <= 24; token = strtok_r(NULL, " ", &save), ++field) {
            if (field == 3)
                sample.live = *token != 'Z' && *token != 'X';
            else if (field == 6)
                sample.session = (pid_t)strtol(token, NULL, 10);
            else if (field >= 14 && field <= 17) {
                long long value = strtoll(token, NULL, 10);
                if (value > 0) sample.cpu += (double)value / (double)ticks;
            } else if (field == 24) {
                long long rss = strtoll(token, NULL, 10);
                if (rss > 0) sample.resident = (uint64_t)rss * (uint64_t)page;
            }
        }
        if (field <= 24) continue;
        if (*count == capacity) {
            size_t next = capacity ? capacity * 2 : 64;
            Sample* resized = realloc(samples, next * sizeof(*samples));
            if (!resized) {
                free(samples);
                closedir(dir);
                return NULL;
            }
            samples = resized;
            capacity = next;
        }
        samples[(*count)++] = sample;
    }
    closedir(dir);
    return samples;
#elif defined(__FreeBSD__)
    /* one entry per process: KERN_PROC_ALL has one per thread, each repeating its memory and its children's times */
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC};
    const u_int names = sizeof mib / sizeof *mib;
    size_t size = 0;
    struct kinfo_proc* entries = NULL;
    /* The process list can grow between the size query and the actual read. */
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (sysctl(mib, names, NULL, &size, NULL, 0)) return NULL;
        size += 16 * sizeof(*entries);
        entries = malloc(size);
        if (!entries) return NULL;
        if (!sysctl(mib, names, entries, &size, NULL, 0)) break;
        free(entries);
        entries = NULL;
        if (errno != ENOMEM) return NULL;
    }
    if (!entries) return NULL;
    *count = size / sizeof(*entries);
    Sample* samples = calloc(*count, sizeof(*samples));
    if (samples)
        for (size_t i = 0; i < *count; ++i) {
            const struct kinfo_proc* k = &entries[i];
            samples[i].pid = k->ki_pid;
            samples[i].session = k->ki_sid;
            samples[i].live = k->ki_stat != SZOMB;
            samples[i].resident = (uint64_t)k->ki_rssize * (uint64_t)sysconf(_SC_PAGESIZE);
            const struct rusage* r[] = {&k->ki_rusage, &k->ki_rusage_ch};
            for (int j = 0; j < 2; ++j)
                samples[i].cpu += (double)r[j]->ru_utime.tv_sec + (double)r[j]->ru_stime.tv_sec +
                                  (double)(r[j]->ru_utime.tv_usec + r[j]->ru_stime.tv_usec) / 1e6;
        }
    free(entries);
    return samples;
#elif defined(__NetBSD__)
    /* one entry per process (KERN_PROC2), its children's times in p_uctime. The sixth name is how many entries fit:
       0 asks for the size, which can grow before the read as it does on FreeBSD */
    int mib[] = {CTL_KERN, KERN_PROC2, KERN_PROC_ALL, 0, (int)sizeof(struct kinfo_proc2), 0};
    const u_int names = sizeof mib / sizeof *mib;
    size_t size = 0;
    struct kinfo_proc2* entries = NULL;
    for (int attempt = 0; attempt < 4; ++attempt) {
        mib[5] = 0;
        if (sysctl(mib, names, NULL, &size, NULL, 0)) return NULL;
        size += 16 * sizeof(*entries);
        entries = malloc(size);
        if (!entries) return NULL;
        mib[5] = (int)(size / sizeof(*entries));
        if (!sysctl(mib, names, entries, &size, NULL, 0)) break;
        free(entries);
        entries = NULL;
        if (errno != ENOMEM) return NULL;
    }
    if (!entries) return NULL;
    *count = size / sizeof(*entries);
    Sample* samples = calloc(*count, sizeof(*samples));
    if (samples)
        for (size_t i = 0; i < *count; ++i) {
            const struct kinfo_proc2* k = &entries[i];
            samples[i].pid = k->p_pid;
            samples[i].session = k->p_sid;
            samples[i].live = k->p_realstat != SZOMB && k->p_realstat != SDEAD;
            samples[i].resident = (uint64_t)k->p_vm_rssize * (uint64_t)sysconf(_SC_PAGESIZE);
            samples[i].cpu = (double)k->p_uutime_sec + (double)k->p_ustime_sec + (double)k->p_uctime_sec +
                             (double)((uint64_t)k->p_uutime_usec + k->p_ustime_usec + k->p_uctime_usec) / 1e6;
        }
    free(entries);
    return samples;
#elif defined(__APPLE__) && !TARGET_OS_IPHONE
    /* a zombie has no task to describe and is left out, which counts it as gone */
    int listed = proc_listallpids(NULL, 0);
    pid_t* pids;
    mach_timebase_info_data_t timebase;
    if (listed <= 0 || mach_timebase_info(&timebase)) return NULL;
    listed += 16; /* room for the processes started since */
    pids = malloc((size_t)listed * sizeof(*pids));
    if (!pids) return NULL;
    listed = proc_listallpids(pids, listed * (int)sizeof(*pids));
    Sample* samples = listed > 0 ? calloc((size_t)listed, sizeof(*samples)) : NULL;
    for (int i = 0; samples && i < listed; ++i) {
        struct proc_taskallinfo info;
        if (pids[i] <= 0 || proc_pidinfo(pids[i], PROC_PIDTASKALLINFO, 0, &info, sizeof info) != sizeof info) continue;
        Sample* sample = &samples[(*count)++];
        sample->pid = pids[i];
        sample->session = getsid(pids[i]);
        sample->live = true;
        sample->resident = info.ptinfo.pti_resident_size;
        /* the task's times are in Mach ticks, which are not nanoseconds on Apple silicon */
        sample->cpu =
            (double)(info.ptinfo.pti_total_user + info.ptinfo.pti_total_system) * timebase.numer / timebase.denom / 1e9;
    }
    free(pids);
    return samples;
#elif defined(__APPLE__)
    /* iOS starts no processes (gui_process_start): there is none to count */
    return NULL;
#else
#error "scarletnx-gui process accounting currently supports Linux, FreeBSD, NetBSD, macOS and Windows"
#endif
}
GuiProcess* gui_process_create(void) {
    GuiProcess* process = calloc(1, sizeof(*process));
    if (process) {
        process->output = -1;
        process->exited = process->eof = true;
    }
    return process;
}
#ifdef GUI_DRIVER_THREAD
/* LLVM's passes recurse deeply: a thread's default stack on iOS (512 KiB) is not enough */
#define GUI_DRIVER_STACK ((size_t)64 << 20)
static void* driver_thread(void* data) {
    GuiProcess* process = data;
    int status = snx_driver(process->argc, process->argv);
    fflush(stdout);
    fflush(stderr);
    /* the process's own output back first, then the pipe's end: closing the writer is the menu's end of file */
    dup2(process->saved[0], STDOUT_FILENO);
    dup2(process->saved[1], STDERR_FILENO);
    close(process->saved[0]);
    close(process->saved[1]);
    close(process->writer);
    for (int i = 0; i < process->argc; ++i) free(process->argv[i]);
    free(process->argv);
    process->status = status;
    atomic_store(&process->done, 1);
    return NULL;
}
static bool driver_start(GuiProcess* process, const char* const* argv, char* error, size_t size) {
    int output[2] = {-1, -1};
    pthread_attr_t attributes;
    int argc = 0, saved = 0;
    while (argv[argc]) ++argc;
    if (pipe(output) || fcntl(output[0], F_SETFL, O_NONBLOCK)) goto failed;
    process->saved[0] = dup(STDOUT_FILENO);
    process->saved[1] = dup(STDERR_FILENO);
    saved = 1;
    if (process->saved[0] < 0 || process->saved[1] < 0) goto failed;
    process->argv = calloc((size_t)argc + 1, sizeof(*process->argv));
    if (!process->argv) goto failed;
    for (process->argc = 0; process->argc < argc; ++process->argc)
        if (!(process->argv[process->argc] = strdup(argv[process->argc]))) goto failed;
    process->writer = output[1];
    fflush(NULL);
    if (dup2(output[1], STDOUT_FILENO) < 0 || dup2(output[1], STDERR_FILENO) < 0) goto failed;
    atomic_store(&process->done, 0);
    snx_abort_request(0);
    if (pthread_attr_init(&attributes) || pthread_attr_setstacksize(&attributes, GUI_DRIVER_STACK) ||
        pthread_create(&process->thread, &attributes, driver_thread, process)) {
        dup2(process->saved[0], STDOUT_FILENO);
        dup2(process->saved[1], STDERR_FILENO);
        errno = EAGAIN;
        goto failed;
    }
    pthread_attr_destroy(&attributes);
    process->output = output[0];
    process->status = 0;
    process->exited = process->eof = false;
    return true;
failed:
    {
        int saved_errno = errno;
        for (int i = 0; i < 2; ++i) {
            if (output[i] >= 0) close(output[i]);
            if (saved && process->saved[i] >= 0) close(process->saved[i]);
        }
        if (process->argv) {
            for (int i = 0; i < process->argc; ++i) free(process->argv[i]);
            free(process->argv);
            process->argv = NULL;
        }
        snprintf(error, size, snx_tr("Cannot start the run: %s"), strerror(saved_errno));
        return false;
    }
}
#endif
/* this program's priority, and a low one: ten steps of nice under it, the lowest at most */
static int own_nice(void) {
    int n;
    errno = 0;
    n = getpriority(PRIO_PROCESS, 0);
    return errno ? 0 : n;
}
static int low_nice(void) {
    int n = own_nice() + 10;
    return n < PRIO_MAX - 1 ? n : PRIO_MAX - 1;
}
bool gui_process_start(GuiProcess* process, const char* const* argv, char* error, size_t size) {
    int output[2] = {-1, -1}, failure[2] = {-1, -1};
    int previous_status;
    if (!gui_process_finished(process, &previous_status)) {
        snprintf(error, size, "%s", snx_tr("A run is already going"));
        return false;
    }
#ifdef GUI_DRIVER_THREAD
    /* an iOS app may not start a process: fork fails there */
    (void)output;
    (void)failure;
    return driver_start(process, argv, error, size);
#endif
    if (pipe(output) || pipe(failure)) goto failed;
    for (int i = 0; i < 2; ++i)
        if (fcntl(output[i], F_SETFD, FD_CLOEXEC) || fcntl(failure[i], F_SETFD, FD_CLOEXEC)) goto failed;
    if (fcntl(output[0], F_SETFL, O_NONBLOCK)) goto failed;
    long max_fd = sysconf(_SC_OPEN_MAX);
    if (max_fd < 0) max_fd = 1024;
    int nice_to = process->low ? low_nice() : 0;
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) goto failed;
    if (!pid) {
        close(output[0]);
        close(failure[0]);
        int null = open("/dev/null", O_RDONLY);
        if (setsid() < 0 || null < 0 || dup2(null, STDIN_FILENO) < 0 || dup2(output[1], STDOUT_FILENO) < 0 ||
            dup2(output[1], STDERR_FILENO) < 0)
            goto child_failed;
        /* low before it starts: every thread and process it makes takes the priority it has */
        if (process->low && setpriority(PRIO_PROCESS, 0, nice_to) < 0) goto child_failed;
        if (null > STDERR_FILENO) close(null);
        if (output[1] > STDERR_FILENO) close(output[1]);
        /* Do not leak SDL/display descriptors into a game. Keep only the
           close-on-exec error channel in addition to the three stdio handles. */
        if (failure[1] != 3) {
            if (dup2(failure[1], 3) < 0) goto child_failed;
            close(failure[1]);
            failure[1] = 3;
        }
        if (fcntl(3, F_SETFD, FD_CLOEXEC)) goto child_failed;
#if defined(__linux__) && defined(SYS_close_range)
        if (syscall(SYS_close_range, 4u, ~0u, 0u))
#endif
            for (int fd = 4; fd < max_fd; ++fd) close(fd);
        execv(argv[0], (char* const*)argv);
    child_failed:
        {
            int saved = errno;
            (void)write(failure[1], &saved, sizeof(saved));
            _exit(127);
        }
    }
    close(output[1]);
    output[1] = -1;
    close(failure[1]);
    failure[1] = -1;
    int child_error = 0;
    ssize_t n;
    do { n = read(failure[0], &child_error, sizeof(child_error)); } while (n < 0 && errno == EINTR);
    close(failure[0]);
    failure[0] = -1;
    if (n != 0) {
        if (n < 0) {
            child_error = errno;
            kill(pid, SIGKILL);
        }
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        errno = child_error;
        goto failed;
    }
    process->pid = pid;
    process->output = output[0];
    process->status = 0;
    process->exited = process->eof = false;
    return true;
failed:
    {
        int saved = errno;
        for (int i = 0; i < 2; ++i) {
            if (output[i] >= 0) close(output[i]);
            if (failure[i] >= 0) close(failure[i]);
        }
        snprintf(error, size, snx_tr("Cannot launch driver: %s"), strerror(saved));
        return false;
    }
}
int gui_process_read(GuiProcess* process, char* buffer, size_t size) {
    if (process->output < 0) return -1;
    ssize_t n = read(process->output, buffer, size);
    if (n > 0) return (int)n;
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
    close(process->output);
    process->output = -1;
    process->eof = true;
    return -1;
}
bool gui_process_finished(GuiProcess* process, int* status) {
#ifdef GUI_DRIVER_THREAD
    if (!process->exited) {
        if (!atomic_load(&process->done)) return false;
        pthread_join(process->thread, NULL);
        process->exited = true;
    }
    if (!process->eof) return false;
    *status = process->status;
    return true;
#endif
    if (!process->exited) {
        int result;
        pid_t waited = waitpid(process->pid, &result, WNOHANG);
        if (waited == process->pid) {
            process->status = WIFEXITED(result) ? WEXITSTATUS(result) : 128 + WTERMSIG(result);
            process->exited = true;
        }
    }
    if (!process->exited || !process->eof) return false;
    if (process->pid) {
        size_t count;
        Sample* samples = snapshot(&count);
        if (!samples) return false;
        bool live = false;
        for (size_t i = 0; i < count; ++i)
            if (samples[i].session == process->pid && samples[i].live) live = true;
        free(samples);
        if (live) return false;
        /* Do not keep an old session ID that could later be reused by an
           unrelated process while the menu is idle. */
        process->pid = 0;
    }
    *status = process->status;
    return true;
}
GuiUsage gui_process_usage(GuiProcess* process) {
    GuiUsage usage = {0};
    size_t count;
    Sample* samples = snapshot(&count);
    if (!samples) return usage;
    double cpu = 0, now = seconds();
    for (size_t i = 0; i < count; ++i)
        if (samples[i].pid == getpid() || (process->pid && samples[i].session == process->pid)) {
            usage.available = true;
            usage.memory_bytes += samples[i].resident;
            /* Waited descendants move into their parent's child CPU counters,
               then into the GUI's counters when the driver is reaped. */
            cpu += samples[i].cpu;
        }
    usage.cpu_percent = gui_cpu_percent(&process->cpu_base, cpu, now);
    free(samples);
    return usage;
}
GuiUsage gui_process_usage_of(GuiProcess* process) {
    GuiUsage usage = {0};
    size_t count;
    Sample* samples = snapshot(&count);
    double cpu = 0;
    if (!samples) return usage;
    usage.available = true;
    for (size_t i = 0; i < count; ++i)
        if (process->pid && samples[i].session == process->pid) {
            usage.memory_bytes += samples[i].resident;
            cpu += samples[i].cpu;
        }
    usage.cpu_percent = gui_cpu_percent(&process->tree_base, cpu, seconds());
    free(samples);
    return usage;
}
#ifndef GUI_DRIVER_THREAD
/* every thread of the tree at nice (Linux keeps it a thread's; the BSDs and macOS a process's): 0, else the first
   errno the system gave, one gone meanwhile being none */
static int tree_nice(const GuiProcess* process, int nice) {
    size_t count;
    Sample* samples = snapshot(&count);
    int failed = 0;
    if (!samples) return errno ? errno : ENOMEM;
    for (size_t i = 0; i < count; ++i) {
        if (samples[i].session != process->pid || !samples[i].live) continue;
#ifdef __linux__
        char path[64];
        struct dirent* entry;
        DIR* tasks;
        snprintf(path, sizeof path, "/proc/%ld/task", (long)samples[i].pid);
        if (!(tasks = opendir(path))) continue; /* gone */
        while ((entry = readdir(tasks))) {
            char* end;
            long tid = strtol(entry->d_name, &end, 10);
            if (*end || tid <= 0) continue;
            if (setpriority(PRIO_PROCESS, (id_t)tid, nice) < 0 && errno != ESRCH && !failed) failed = errno;
        }
        closedir(tasks);
#else
        if (setpriority(PRIO_PROCESS, (id_t)samples[i].pid, nice) < 0 && errno != ESRCH && !failed) failed = errno;
#endif
    }
    free(samples);
    return failed;
}
#endif
bool gui_process_low(GuiProcess* process, bool low, char* error, size_t size) {
    process->low = low;
#ifdef GUI_DRIVER_THREAD
    if (low) snprintf(error, size, "%s", snx_tr("the driver runs on a thread of the app, at the app's priority"));
    return !low;
#else
    int failed = process->pid ? tree_nice(process, low ? low_nice() : own_nice()) : 0;
    if (failed) snprintf(error, size, "%s", strerror(failed));
    return !failed;
#endif
}
void gui_process_terminate(GuiProcess* process) {
    if (!process) return;
#ifdef GUI_DRIVER_THREAD
    /* a thread cannot be killed: the driver is asked to stop (the recompiler looks between its units), and the menu
       waits for it to end */
    snx_abort_request(1);
    return;
#endif
    if (process->pid) {
        /* Freeze the session before killing it, so a compiler cannot fork a new
           worker between enumeration and cleanup. Three passes for a concurrent fork, then the kill. */
        for (int pass = 0; pass < 4; ++pass) {
            size_t count;
            Sample* samples = snapshot(&count);
            if (samples)
                for (size_t i = 0; i < count; ++i)
                    if (samples[i].session == process->pid) kill(samples[i].pid, pass < 3 ? SIGSTOP : SIGKILL);
            free(samples);
        }
        if (!process->exited) kill(process->pid, SIGKILL);
    }
}
void gui_process_destroy(GuiProcess* process) {
    if (!process) return;
#ifdef GUI_DRIVER_THREAD
    /* the app is ending: a driver still running ends with it */
    if (process->output >= 0) close(process->output);
    if (atomic_load(&process->done)) free(process);
    return;
#endif
    gui_process_terminate(process);
    if (process->pid && !process->exited)
        while (waitpid(process->pid, NULL, 0) < 0 && errno == EINTR) {}
    if (process->output >= 0) close(process->output);
    free(process);
}
