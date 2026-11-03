/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gnuabi/process/process.h"
#include "gnuabi/runtime/execution.h"
#include "gnuabi/runtime/main.h"
#include "common/utilities/common.h"
#include "common/cpu/recompiler/snx_recompiler.h"
#include <openssl/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#endif

/* Guest calls are host calls in the generated code, so the guest's call depth is the host thread's: the guest runs on
   a thread of its own with a stack of this size, which the host only commits as it is used */
#define GUEST_STACK ((size_t)1 << 30)

static const char usage[] =
    "Usage: scarletnx-gnuabi [options] [--] ELF [arguments...]\n"
    "Runs Linux AArch64 or AArch32 (A32/Thumb) programs, static or dynamically linked, through the ScarletNX\n"
    "recompiler.\n"
    "\n"
    "  -L DIR, --sysroot=DIR  The guest's root: a guest path names DIR's file when DIR has it, the host's\n"
    "                         otherwise, as with QEMU's -L. The default is linux/ in the data folder\n"
    "  --isolated             No host files: /tmp in memory, the program alone as /program\n"
    "  --jit-cache=DIR        Where compiled code is kept between runs (default: the user's cache folder)\n"
    "  --no-jit-cache         Keep no compiled code between runs\n"
    "  --jit-opt=LEVEL        The JIT's LLVM optimization level: 0, 1, 2 (the default), 3, s or z\n"
    "  --verbose-jit          Say what the JIT compiles\n"
    "  --strace               Write every system call, its arguments and its result to stderr, with the\n"
    "                         process id\n"
    "  --sample-pc            Write the guest's program counter to stderr once a second (a loop with no\n"
    "                         system call; not on Windows)\n"
    "  --host-page=N          Work as a host with N-byte pages: a power of two from the host's page to\n"
    "                         1048576 (not on Windows)\n"
    "  -E VAR=VALUE, -U VAR   Set or remove a variable of the guest's environment (the host's otherwise)\n"
    "  -0 ARGV0               The program's argv[0] (default: ELF)\n";

static int run_status;
static GnuProcess* process;

/* the guest's thread: the process ends here, in a forked child too, where it is the only thread */
static void guest_main(void) {
    gnu_start_sampler(process);
    run_status = gnu_runtime_run(process);
    if (process->faulted && process->diagnostic[0]) fprintf(stderr, "scarletnx-gnuabi: %s\n", process->diagnostic);
    OPENSSL_thread_stop(); /* its state for this thread (an execve's random bytes): it ends the process, not itself */
    gnu_spawn_exit(process);
}
#ifdef _WIN32
static DWORD WINAPI guest_thread(LPVOID unused) {
    (void)unused;
    guest_main();
    return 0;
}
#else
static void* guest_thread(void* unused) {
    (void)unused;
    guest_main();
    return NULL;
}
#endif

/* the environment: the host's, with -E and -U applied in order */
static char** environment(char** changes, size_t change_count) {
#ifdef _WIN32
    extern char** _environ;
    char** host = _environ;
#else
    extern char** environ;
    char** host = environ;
#endif
    size_t count = 0, i, j;
    char** env;
    while (host && host[count]) ++count;
    env = calloc(count + change_count + 1, sizeof *env);
    if (!env) return NULL;
    for (i = 0; i < count; ++i) env[i] = host[i];
    for (j = 0; j < change_count; ++j) {
        const char* change = changes[j];
        int set = change[0] == '+';
        const char* name = change + 1;
        size_t n = set ? (size_t)(strchr(name, '=') - name) : strlen(name);
        for (i = 0; i < count; ++i)
            if (!strncmp(env[i], name, n) && env[i][n] == '=') break;
        if (set) {
            if (i == count) ++count;
            env[i] = (char*)name;
        } else if (i < count) {
            memmove(env + i, env + i + 1, (count - i) * sizeof *env);
            --count;
        }
    }
    env[count] = NULL;
    return env;
}

/* linux/ of the data folder games/ and work/ are in (snx_data_dir), as the 3DS runtime finds work/3ds. An older
   version's SNX_LINUX_SYSROOT (docs/DEPRECATIONS.md) still names another for this version, said. Removed with
   tests/architecture.py's ENV_DEPRECATED row */
static char* default_sysroot(const char* argv0) {
    const char* named = getenv("SNX_LINUX_SYSROOT");
    char *exe, *dir, *data, *root, *checkout;
    if (named && named[0]) {
        root = snx_absolute(named);
        fprintf(stderr,
                "scarletnx-gnuabi: SNX_LINUX_SYSROOT=%s is deprecated, replaced by -L DIR and --sysroot=DIR: this "
                "run's root is %s (docs/DEPRECATIONS.md)\n",
                named, root ? root : named);
        return root;
    }
    exe = snx_executable(argv0);
    dir = exe ? snx_dirname(exe) : NULL;
    data = snx_data_dir(dir);
    root = data ? snx_path_join(data, "linux") : NULL;
    checkout = snx_checkout(SNX_SOURCE_DIR, SNX_BUILD_DIR, dir);
    if (checkout && data) snx_checkout_left(checkout, data);
    free(checkout);
    free(exe);
    free(dir);
    free(data);
    return root;
}

#ifndef _WIN32
/* --host-page=N's N, a power of two from the host's page to 1 MiB: 0 with *page set, else -1 with *own the host's */
static int host_page_of(const char* text, uint64_t* page, uint64_t* own) {
    long host = sysconf(_SC_PAGESIZE);
    char* end = NULL;
    uint64_t v;
    *own = host > (long)GNU_PAGE_SIZE ? (uint64_t)host : GNU_PAGE_SIZE;
    errno = 0;
    v = strtoull(text, &end, 10);
    if (errno || end == text || *end || v < *own || v > (1ull << 20) || (v & (v - 1))) return -1;
    *page = v;
    return 0;
}
#endif
/* --sample-pc and --host-page=N, POSIX's: 0, or -1 with why on stderr */
static int posix_options(int sample, const char* host_page) {
#ifdef _WIN32
    if (sample) fprintf(stderr, "scarletnx-gnuabi: --sample-pc has no Windows version: nothing samples the pc there\n");
    if (host_page)
        fprintf(stderr,
                "scarletnx-gnuabi: --host-page=%s has no Windows version: the runtime works in 4096-byte pages there\n",
                host_page);
    return sample || host_page ? -1 : 0;
#else
    uint64_t page = 0, own;
    if (host_page && host_page_of(host_page, &page, &own)) {
        fprintf(stderr,
                "scarletnx-gnuabi: --host-page=%s is not a power of two from the host's page, %llu bytes, to "
                "1048576\n",
                host_page, (unsigned long long)own);
        return -1;
    }
    gnu_sample_pc(sample);
    gnu_memory_host_page(page);
    return 0;
#endif
}
/* The variables an older version read for these options (docs/DEPRECATIONS.md): SNX_GNU_STRACE=1 is --strace,
   SNX_GNU_SAMPLE --sample-pc and SNX_HOST_PAGE=N --host-page=N for this version, where they did something before and
   the command line gives none of its own; each said, with what the run takes */
static void old_options(int* strace, int* sample, const char** host_page) {
    const char *trace = getenv("SNX_GNU_STRACE"), *sampled = getenv("SNX_GNU_SAMPLE"), *page = getenv("SNX_HOST_PAGE");
    if (trace) {
        fprintf(stderr, "scarletnx-gnuabi: SNX_GNU_STRACE=%s is deprecated, %s (docs/DEPRECATIONS.md)\n", trace,
                *strace           ? "and --strace is given: this run traces its system calls"
                : trace[0] == '1' ? "and this run reads it as --strace"
                                  : "and only a value starting with 1 did anything: this run traces no system call");
        if (trace[0] == '1') *strace = 1;
    }
#ifdef _WIN32
    if (sampled)
        fprintf(stderr,
                "scarletnx-gnuabi: SNX_GNU_SAMPLE=%s is deprecated, and did nothing on Windows: nothing samples "
                "the pc (docs/DEPRECATIONS.md)\n",
                sampled);
    if (page)
        fprintf(stderr,
                "scarletnx-gnuabi: SNX_HOST_PAGE=%s is deprecated, and did nothing on Windows: the runtime "
                "works in 4096-byte pages (docs/DEPRECATIONS.md)\n",
                page);
    (void)sample, (void)host_page;
#else
    uint64_t v, own;
    if (sampled) {
        fprintf(stderr,
                "scarletnx-gnuabi: SNX_GNU_SAMPLE=%s is deprecated, and this run reads it as --sample-pc "
                "(docs/DEPRECATIONS.md)\n",
                sampled);
        *sample = 1;
    }
    if (!page) return;
    if (*host_page)
        fprintf(stderr,
                "scarletnx-gnuabi: SNX_HOST_PAGE=%s is deprecated, and --host-page=%s is given: this run takes "
                "%s (docs/DEPRECATIONS.md)\n",
                page, *host_page, *host_page);
    else if (host_page_of(page, &v, &own))
        fprintf(stderr,
                "scarletnx-gnuabi: SNX_HOST_PAGE=%s is deprecated, and not a power of two from the host's page, "
                "%llu bytes, to 1048576: the runtime works in %llu-byte pages (docs/DEPRECATIONS.md)\n",
                page, (unsigned long long)own, (unsigned long long)own);
    else {
        fprintf(stderr,
                "scarletnx-gnuabi: SNX_HOST_PAGE=%s is deprecated, and this run reads it as --host-page=%s "
                "(docs/DEPRECATIONS.md)\n",
                page, page);
        *host_page = page;
    }
#endif
}

/* what main's options made, once they are copied or the runtime stops before the guest runs */
static void options_free(char** changes, size_t change_count, char* sysroot) {
    for (size_t i = 0; i < change_count; i++) free(changes[i]);
    free(changes);
    free(sysroot);
}

int snx_gnuabi_main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    const char *argv0 = NULL, *cache_option = NULL, *opt = "2";
    char *sysroot = NULL, *cache = NULL;
    char** changes = calloc((size_t)argc + 1, sizeof *changes);
    size_t change_count = 0;
    /* a process another copy of this runtime started (Windows' execve): its descriptors and working folder */
    int adopted_fd[64], adopted_count = 0;
    unsigned long long adopted_host[64];
    const char* cwd = NULL;
    int first = 1, isolated = 0, no_cache = 0, verbose = 0, strace = 0, sample = 0;
    const char* host_page = NULL; /* --host-page's */
    if (!changes) return 2;
    while (first < argc && argv[first][0] == '-') {
        const char* option = argv[first];
        const char* value = first + 1 < argc ? argv[first + 1] : NULL;
        if (!strcmp(option, "--help") || !strcmp(option, "-h")) {
            fputs(usage, stdout);
            options_free(changes, change_count, sysroot);
            return 0;
        }
        if (!strcmp(option, "--")) {
            first++;
            break;
        }
        if (!strncmp(option, "--sysroot=", 10)) {
            free(sysroot);
            sysroot = snx_absolute(option + 10);
        } else if (!strcmp(option, "-L") && value) {
            free(sysroot);
            sysroot = snx_absolute(value);
            first++;
        } else if (!strcmp(option, "--isolated"))
            isolated = 1;
        else if (!strcmp(option, "--jit")) {
            /* the only mode */
        } else if (!strncmp(option, "--jit-cache=", 12))
            cache_option = option + 12;
        else if (!strcmp(option, "--no-jit-cache"))
            no_cache = 1;
        else if (!strncmp(option, "--jit-opt=", 10))
            opt = option + 10;
        else if (!strcmp(option, "--verbose-jit"))
            verbose = 1;
        else if (!strcmp(option, "--strace"))
            strace = 1;
        else if (!strcmp(option, "--sample-pc"))
            sample = 1;
        else if (!strncmp(option, "--host-page=", 12))
            host_page = option + 12;
        else if (!strcmp(option, "-E") && value && strchr(value, '=')) {
            char* change = snx_format("+%s", value);
            changes[change_count++] = change;
            first++;
        } else if (!strcmp(option, "-U") && value) {
            changes[change_count++] = snx_format("-%s", value);
            first++;
        } else if (!strncmp(option, "--fd=", 5) && adopted_count < 64 &&
                   sscanf(option + 5, "%d:%llu", &adopted_fd[adopted_count], &adopted_host[adopted_count]) == 2) {
            adopted_count++;
        } else if (!strcmp(option, "--cwd") && value) {
            cwd = value;
            first++;
        } else if (!strcmp(option, "-0") && value) {
            argv0 = value;
            first++;
        } else {
            fprintf(stderr, "scarletnx-gnuabi: unknown option %s (--help lists them)\n", option);
            options_free(changes, change_count, sysroot);
            return 2;
        }
        first++;
    }
    if (first == argc) {
        fputs(usage, stderr);
        options_free(changes, change_count, sysroot);
        return 2;
    }
    /* what an older version read (docs/DEPRECATIONS.md): still these options for this version, under the command
       line's own, each said. Removed with tests/architecture.py's ENV_DEPRECATED rows */
    old_options(&strace, &sample, &host_page);
    if (posix_options(sample, host_page)) {
        options_free(changes, change_count, sysroot);
        return 2;
    }
    gnu_syscall_trace(strace);
    /* the JIT's branch profile an older version read, replaced by ir_branch_profile, a setting of scarletnx's runtime
       and recompile only */
    const char* old_profile = getenv("SNX_LLVM_BRANCH_PROFILE");
    if (old_profile)
        fprintf(stderr,
                "scarletnx-gnuabi: SNX_LLVM_BRANCH_PROFILE=%s is deprecated, replaced by ir_branch_profile, a setting "
                "of scarletnx's runtime and recompile and not of this one: its JIT runs without branch weights "
                "(docs/DEPRECATIONS.md)\n",
                old_profile);
    if (!isolated && !sysroot) sysroot = default_sysroot(argv[0]);
    if (!no_cache) {
        if (cache_option)
            cache = snx_absolute(cache_option);
        else {
            char* user = snx_menu_dir(NULL, SNX_USER_CACHE);
            cache = user ? snx_path_join(user, "linux-jit") : NULL;
            free(user);
        }
    }
    process = calloc(1, sizeof *process);
    if (!process) return 2;
    char error[1024] = {0};
    if (gnu_jit_init(&process->jit, cache, error, sizeof error)) {
        fprintf(stderr, "scarletnx-gnuabi: %s\n", error);
        return 2;
    }
    process->jit.verbose = verbose;
    snx_recompiler_set_verbose(process->jit.recompiler, verbose);
    if (snx_recompiler_set_optimization(process->jit.recompiler, opt[0]) || opt[1]) {
        fprintf(stderr, "scarletnx-gnuabi: --jit-opt takes 0, 1, 2, 3, s or z\n");
        return 2;
    }
    char** env = isolated ? NULL : environment(changes, change_count);
    static const char* const isolated_environment[] = {"PATH=/bin", "LANG=C", NULL};
    const char** guest_argv = calloc((size_t)(argc - first) + 1, sizeof *guest_argv);
    if (!guest_argv) return 2;
    for (int i = first; i < argc; i++) guest_argv[i - first] = argv[i];
    if (argv0) guest_argv[0] = argv0;
    /* what the copies of this runtime a guest's execve starts get (Windows): this one's choices */
    {
        const char* forward[8];
        int n = 0;
        if (strace) forward[n++] = "--strace";
        char *root_option = sysroot ? snx_format("--sysroot=%s", sysroot) : NULL, *cache_forward = NULL,
             *opt_option = snx_format("--jit-opt=%s", opt);
        forward[n++] = isolated ? "--isolated" : root_option ? root_option : "--isolated";
        forward[n++] = cache ? (cache_forward = snx_format("--jit-cache=%s", cache)) : "--no-jit-cache";
        forward[n++] = opt_option;
        gnu_spawn_set_options(n, forward);
        free(root_option);
        free(cache_forward);
        free(opt_option);
    }
    if (gnu_process_init(process, argv[first], argc - first, guest_argv,
                         env ? (const char* const*)env : isolated_environment, isolated ? NULL : sysroot, error,
                         sizeof error)) {
        fprintf(stderr, "scarletnx-gnuabi: %s\n", error);
        return 2;
    }
    /* copied by now: the arguments and the environment to the guest's stack, the sysroot to the file system, the cache
       folder to the recompiler. This thread only waits from here, and never ends (the guest's ends the process), so
       OpenSSL's state for it, from the stack's random bytes, goes now too */
    options_free(changes, change_count, sysroot);
    free(env);
    free(guest_argv);
    free(cache);
    OPENSSL_thread_stop();
    if (cwd && !isolated) gnu_vfs_set_cwd(&process->vfs, cwd);
    for (int i = 0; i < adopted_count; i++) {
#ifdef _WIN32
        int host = _open_osfhandle((intptr_t)adopted_host[i], 0);
#else
        int host = (int)adopted_host[i];
#endif
        if (host >= 0) gnu_vfs_adopt(&process->vfs, adopted_fd[i], host);
    }
#ifdef _WIN32
    HANDLE thread = CreateThread(NULL, GUEST_STACK, guest_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!thread) {
        char why[256];
        fprintf(stderr, "scarletnx-gnuabi: cannot start the guest thread: %s\n", snx_os_error(why, sizeof why));
        return 2;
    }
    WaitForSingleObject(thread, INFINITE);
#else
    pthread_attr_t attributes;
    pthread_t thread;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, GUEST_STACK);
    int failed = pthread_create(&thread, &attributes, guest_thread, NULL);
    if (failed) {
        fprintf(stderr, "scarletnx-gnuabi: cannot start the guest thread: %s\n", strerror(failed));
        return 2;
    }
    pthread_join(thread, NULL);
#endif
    return run_status; /* not reached: the guest's thread ends the process */
}
