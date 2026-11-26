/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/game_log.h"
#include "common/platform/thread.h"
#include "common/platform/time.h"
#include "common/utilities/common.h"
#include "common/utilities/run_log.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <poll.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

#define CHUNK       4096 /* what a copy reads at once */
#define WAITS       1000 /* a sync's naps of 2 ms: 2 seconds */
#define CRASH_WAITS 100

#if defined(_WIN32)
static const int FATAL[] = {SIGSEGV, SIGILL, SIGFPE, SIGABRT};
#else
static const int FATAL[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
#endif
#define FATALS (sizeof FATAL / sizeof *FATAL)

typedef struct {             /* each stream's copy writes its own: a cache line each */
    alignas(64) int from; /* the pipe's end the copy reads */
    int terminal;        /* the descriptor stdout or stderr had, -1 for none */
    _Atomic int to;      /* terminal while it takes the copy's writes, -1 once it refused one */
    _Atomic int copying; /* a chunk read and not yet all written */
    _Atomic int done;    /* the copy read the pipe's end */
} GameLogStream;

static struct {
    GameLogStream streams[2]; /* stdout's, then stderr's where it was another file */
    int count;
    int started;
    FILE* stream;     /* the log's, open to append: only its descriptor is written */
    _Atomic int file; /* that descriptor, -1 before it is made or once it refused a write */
    char path[1400];
    PMutex lock; /* the file's lines, its size and its cuts, between the copies */
    PCond ended; /* a copy's end, under lock */
    uint64_t t0;
    _Atomic int mid_line; /* the file's last byte ends no line */
    long size;
    SnxRunLogCut cut;
    const int* limit_mib;
    _Atomic int full;
#if defined(_WIN32)
    void (*before[FATALS])(int);
#else
    struct sigaction before[FATALS];
#endif
} g_game_log = {.file = -1, .lock = PMUTEX_INIT, .cut = SNX_RUN_LOG_CUT_INIT};

/* all of text to fd, with write (async-signal-safe): 0, or -1 when fd refused it */
static int put(int fd, const char* text, size_t n) {
    while (n) {
#if defined(_WIN32)
        int w = _write(fd, text, n > 0x40000000u ? 0x40000000u : (unsigned)n);
#else
        ssize_t w = write(fd, text, n);
        if (w < 0 && errno == EINTR) continue;
#endif
        if (w <= 0) return -1;
        text += w;
        n -= (size_t)w;
    }
    return 0;
}
static int dup_onto(int fd, int onto) {
#if defined(_WIN32)
    return _dup2(fd, onto);
#else
    return dup2(fd, onto) < 0 ? -1 : 0;
#endif
}
static void close_fd(int fd) {
#if defined(_WIN32)
    _close(fd);
#else
    close(fd);
#endif
}
static void nap(void) {
#if defined(_WIN32)
    Sleep(2);
#else
    struct timespec two_ms = {0, 2000000};
    nanosleep(&two_ms, NULL);
#endif
}
/* "[%7.2f] " of the seconds since the log's start, the hundredths rounded, without stdio: the crash's too */
static size_t stamp(char* out, uint64_t now) {
    uint64_t cs = ((now > g_game_log.t0 ? now - g_game_log.t0 : 0) + 5000000u) / 10000000u;
    char digits[24];
    size_t n = 0, at = 0;
    digits[n++] = (char)('0' + cs % 10), cs /= 10;
    digits[n++] = (char)('0' + cs % 10), cs /= 10;
    digits[n++] = '.';
    do digits[n++] = (char)('0' + cs % 10), cs /= 10;
    while (cs);
    out[at++] = '[';
    for (size_t pad = n; pad < 7; ++pad) out[at++] = ' ';
    while (n) out[at++] = digits[--n];
    out[at++] = ']';
    out[at++] = ' ';
    return at;
}
/* the pipe holds bytes no copy read yet */
static int pending(int fd) {
#if defined(_WIN32)
    DWORD avail = 0;
    return PeekNamedPipe((HANDLE)_get_osfhandle(fd), NULL, 0, NULL, &avail, NULL) && avail > 0;
#else
    struct pollfd p = {.fd = fd, .events = POLLIN};
    return poll(&p, 1, 0) > 0 && (p.revents & POLLIN);
#endif
}
/* every copy has written all the pipes held: seen twice 2 ms apart, as a copy's read and its copying flag are two
   steps */
static int idle(void) {
    for (int i = 0; i < g_game_log.count; ++i) {
        GameLogStream* s = &g_game_log.streams[i];
        if (atomic_load_explicit(&s->done, memory_order_acquire)) continue;
        if (atomic_load_explicit(&s->copying, memory_order_acquire) || pending(s->from)) return 0;
    }
    return 1;
}
static void wait_idle(int waits) {
    for (int i = 0, quiet = 0; i < waits && quiet < 2; ++i) {
        quiet = idle() ? quiet + 1 : 0;
        if (quiet < 2) nap();
    }
}
/* stderr's terminal (or the one both share), while it takes writes */
static int terminal_err(void) {
    return atomic_load_explicit(&g_game_log.streams[g_game_log.count - 1].to, memory_order_acquire);
}

/* past log_max: the cut (run_log.h), its failure said in the file and on the terminal. Under lock */
static void cut_when_full(int file) {
    if (!g_game_log.limit_mib || g_game_log.cut.cuts < 0) return;
    long limit = (long)atomic_load_explicit((const _Atomic int*)g_game_log.limit_mib, memory_order_relaxed) << 20;
    if (limit <= 0 || g_game_log.size <= limit) return;
    char said[320];
    int before = g_game_log.cut.cuts;
    g_game_log.size = snx_run_log_cut(g_game_log.path, limit, g_game_log.t0, &g_game_log.cut, said, sizeof said);
    if (said[0]) {
        int err = terminal_err();
        (void)put(file, said, strlen(said));
        atomic_store_explicit(&g_game_log.mid_line, 0, memory_order_relaxed);
        if (err >= 0) (void)put(err, said, strlen(said));
    } else if (!before && g_game_log.cut.cuts == 1)
        atomic_store_explicit(&g_game_log.full, (int)(limit >> 20), memory_order_release);
}
/* text into the file, each line stamped at its start. Under lock */
static void lines(const char* text, size_t n) {
    char out[2 * CHUNK];
    size_t used = 0;
    int file = atomic_load_explicit(&g_game_log.file, memory_order_relaxed);
    if (file < 0) return;
    while (n || used) {
        size_t piece = 0;
        if (n) {
            const char* end = memchr(text, '\n', n);
            piece = end ? (size_t)(end - text) + 1 : n;
            if (piece > CHUNK) piece = CHUNK;
        }
        if (used && (!n || used + 32 + piece > sizeof out)) {
            if (put(file, out, used)) { /* the disk refused: the terminal keeps the lines from here */
                char said[1700], why[256];
                int err = terminal_err();
                snprintf(said, sizeof said,
                         "log: cannot write %s (%s): the run's lines from here go to the terminal "
                         "alone\n",
                         g_game_log.path, snx_os_error(why, sizeof why));
                if (err >= 0) (void)put(err, said, strlen(said));
                atomic_store_explicit(&g_game_log.file, -1, memory_order_release);
                return;
            }
            g_game_log.size += (long)used;
            used = 0;
        }
        if (!n) break;
        if (!atomic_load_explicit(&g_game_log.mid_line, memory_order_relaxed)) used += stamp(out + used, plat_ns());
        memcpy(out + used, text, piece);
        used += piece;
        atomic_store_explicit(&g_game_log.mid_line, text[piece - 1] != '\n', memory_order_relaxed);
        text += piece;
        n -= piece;
    }
    cut_when_full(file);
}

/* a stream's copy: the pipe to its terminal and the file, to the pipe's end */
static void copy(void* arg) {
    GameLogStream* s = arg;
    char buf[CHUNK];
#if !defined(_WIN32)
    sigset_t gone; /* a terminal that went (a closed pipe) is this write's error, not the process's end */
    sigemptyset(&gone);
    sigaddset(&gone, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &gone, NULL);
#endif
    for (;;) {
#if defined(_WIN32)
        int n = _read(s->from, buf, sizeof buf);
#else
        ssize_t n = read(s->from, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
#endif
        if (n <= 0) break;
        atomic_store_explicit(&s->copying, 1, memory_order_release);
        int to = atomic_load_explicit(&s->to, memory_order_relaxed);
        char said[1700] = "", why[256];
        if (to >= 0 && put(to, buf, (size_t)n)) {
            snprintf(said, sizeof said,
                     "log: the terminal refused the run's output (%s): from here %s alone has the lines\n",
                     snx_os_error(why, sizeof why), g_game_log.path);
            atomic_store_explicit(&s->to, -1, memory_order_release);
        }
        pm_lock(&g_game_log.lock);
        if (said[0]) lines(said, strlen(said));
        lines(buf, (size_t)n);
        pm_unlock(&g_game_log.lock);
        atomic_store_explicit(&s->copying, 0, memory_order_release);
    }
    pm_lock(&g_game_log.lock);
    atomic_store_explicit(&s->done, 1, memory_order_release);
    pc_broadcast(&g_game_log.ended);
    pm_unlock(&g_game_log.lock);
}

/* the process's end: the terminals back on 1 and 2, the pipes' only writers, so each copy reads its pipe to the end */
static void end(void) {
    fflush(stdout);
    fflush(stderr);
    for (int i = 0; i < g_game_log.count; ++i) {
        int back = g_game_log.streams[i].terminal;
#if defined(_WIN32)
        if (back < 0) back = _open("NUL", _O_WRONLY);
#else
        if (back < 0) back = open("/dev/null", O_WRONLY | O_CLOEXEC);
#endif
        (void)dup_onto(back, i ? 2 : 1);
        if (g_game_log.count == 1) (void)dup_onto(back, 2);
    }
    int ended = 1;
    uint64_t until = plat_ns() + 2000000000u;
    pm_lock(&g_game_log.lock);
    for (int i = 0; i < g_game_log.count && ended; ++i)
        for (uint64_t now; !atomic_load_explicit(&g_game_log.streams[i].done, memory_order_acquire);) {
            if ((now = plat_ns()) >= until) {
                ended = 0;
                break;
            }
            pc_wait(&g_game_log.ended, &g_game_log.lock, (int64_t)(until - now));
        }
    pm_unlock(&g_game_log.lock);
    if (!ended) /* a process the run started holds a pipe's end still */
        fprintf(stderr, "log: the run's last lines did not all reach %s in 2 seconds: it can lack them\n",
                g_game_log.path);
}

void snx_game_log_crash(const char* text, size_t size) {
    int saved = errno, file = atomic_load_explicit(&g_game_log.file, memory_order_acquire);
    if (file < 0 || !g_game_log.started) {
        (void)put(2, text, size);
        errno = saved;
        return;
    }
    wait_idle(CRASH_WAITS);
    char out[CHUNK];
    size_t n = 0;
    if (atomic_load_explicit(&g_game_log.mid_line, memory_order_relaxed)) out[n++] = '\n';
    n += stamp(out + n, plat_ns());
    size_t take = size < sizeof out - n ? size : sizeof out - n;
    memcpy(out + n, text, take);
    (void)put(file, out, n + take);
    int err = terminal_err();
    if (err >= 0) (void)put(err, text, size);
    errno = saved;
}

#if defined(_WIN32)
static void on_fatal(int sig) {
#else
static void on_fatal(int sig, siginfo_t* info, void* context) {
#endif
    char line[48] = "*** Crash: signal ", number[12];
    size_t n = strlen(line), k = 0, at = 0;
    for (unsigned v = (unsigned)sig; k == 0 || v; v /= 10) number[k++] = (char)('0' + v % 10);
    while (k) line[n++] = number[--k];
    line[n++] = '\n';
    snx_game_log_crash(line, n);
    while (at < FATALS && FATAL[at] != sig) ++at;
    if (at == FATALS) return;
#if defined(_WIN32)
    signal(sig, g_game_log.before[at] == SIG_ERR ? SIG_DFL : g_game_log.before[at]);
    raise(sig);
#else
    /* the handler before this one, called as the system would; with none, the default's end, by the signal raised
       again, which comes at the return (a fault's or one raised alike) */
    const struct sigaction* before = &g_game_log.before[at];
    if ((before->sa_flags & SA_SIGINFO) && before->sa_sigaction) {
        sigaction(sig, before, NULL);
        before->sa_sigaction(sig, info, context);
    } else if (!(before->sa_flags & SA_SIGINFO) && before->sa_handler != SIG_DFL && before->sa_handler != SIG_IGN) {
        sigaction(sig, before, NULL);
        before->sa_handler(sig);
    } else {
        struct sigaction ends = {0};
        ends.sa_handler = SIG_DFL;
        sigemptyset(&ends.sa_mask);
        sigaction(sig, &ends, NULL);
        raise(sig);
    }
#endif
}

/* a pipe whose ends no process the run starts inherits: 0, or -1 */
static int make_pipe(int ends[2]) {
#if defined(_WIN32)
    return _pipe(ends, 1 << 16, _O_BINARY | _O_NOINHERIT);
#else
    if (pipe(ends)) return -1;
    fcntl(ends[0], F_SETFD, FD_CLOEXEC);
    fcntl(ends[1], F_SETFD, FD_CLOEXEC);
    return 0;
#endif
}
/* stdout's and stderr's descriptors are one file (a terminal, the launcher's pipe): one pipe keeps their order */
static int same_file(void) {
#if defined(_WIN32)
    intptr_t out = _get_osfhandle(1), err = _get_osfhandle(2);
    return out != -1 && out == err;
#else
    struct stat out, err;
    return !fstat(1, &out) && !fstat(2, &err) && out.st_dev == err.st_dev && out.st_ino == err.st_ino;
#endif
}

int snx_game_log_start(const char* folder, const char* name, const int* limit_mib, char* why, size_t size) {
    char kept[1700], dir[1400], said[256];
    int ends[2][2] = {{-1, -1}, {-1, -1}};
    if (g_game_log.started) {
        snprintf(why, size, "the game log %s is made already: a process has one", g_game_log.path);
        return -1;
    }
    if (!folder || !name || snx_path_format(g_game_log.path, sizeof g_game_log.path, "%s/%s.log", folder, name) ||
        snx_path_format(dir, sizeof dir, "%s/logs", folder)) {
        snprintf(why, size, "the game log of %s in %s: no such path", name ? name : "(no name)",
                 folder ? folder : "(no folder)");
        return -1;
    }
#if !defined(_WIN32)
    /* a descriptor of the three that is closed gets the null device, so neither the log nor a pipe takes it */
    for (int fd = 0; fd < 3; ++fd)
        if (fcntl(fd, F_GETFD) < 0 && open("/dev/null", fd ? O_WRONLY : O_RDONLY) != fd) {
            snprintf(why, size, "descriptor %d is closed and the null device cannot take it: %s", fd,
                     snx_os_error(said, sizeof said));
            return -1;
        }
#endif
    snx_run_log_keep(g_game_log.path, dir, kept, sizeof kept);
    FILE* f = snx_fopen(g_game_log.path, "wb");
    if (f) fclose(f);
    g_game_log.stream = f ? snx_fopen(g_game_log.path, "ab") : NULL; /* each write at the end, a cut's too */
    if (!g_game_log.stream) {
        snprintf(why, size, "cannot make the game log %s: %s", g_game_log.path, snx_os_error(said, sizeof said));
        return -1;
    }
#if !defined(_WIN32)
    fcntl(fileno(g_game_log.stream), F_SETFD, FD_CLOEXEC);
#endif
    g_game_log.t0 = plat_ns();
    g_game_log.limit_mib = limit_mib;
    pc_init(&g_game_log.ended);
    fflush(stdout);
    fflush(stderr);
    g_game_log.count = same_file() ? 1 : 2;
    atomic_store_explicit(&g_game_log.file, fileno(g_game_log.stream), memory_order_release);
    for (int i = 0; i < g_game_log.count; ++i) {
        GameLogStream* s = &g_game_log.streams[i];
        if (make_pipe(ends[i])) {
            snprintf(why, size, "cannot make the game log's pipe: %s", snx_os_error(said, sizeof said));
            goto failed;
        }
        s->from = ends[i][0];
#if defined(_WIN32)
        s->terminal = _dup(i ? 2 : 1);
#else
        s->terminal = fcntl(i ? 2 : 1, F_DUPFD_CLOEXEC, 3);
#endif
        atomic_store_explicit(&s->to, s->terminal, memory_order_relaxed);
        if (plat_thread(copy, s, 256 << 10)) {
            snprintf(why, size, "cannot start the game log's copy: %s", snx_os_error(said, sizeof said));
            goto failed;
        }
    }
#if defined(_WIN32)
    int moved = _dup2(ends[0][1], 1) || _dup2(ends[g_game_log.count - 1][1], 2);
#else
    int moved = dup2(ends[0][1], 1) < 0 || dup2(ends[g_game_log.count - 1][1], 2) < 0;
#endif
    if (moved) { /* stdout back on its terminal, if it went: nothing redirected */
        snprintf(why, size, "cannot put stdout and stderr through the game log: %s", snx_os_error(said, sizeof said));
        if (g_game_log.streams[0].terminal >= 0) (void)dup_onto(g_game_log.streams[0].terminal, 1);
        goto failed;
    }
    for (int i = 0; i < g_game_log.count; ++i) close_fd(ends[i][1]);
    /* each line on its way at its end (a full buffer would hold a crash's last lines); Windows has no line buffering */
#if defined(_WIN32)
    setvbuf(stdout, NULL, _IONBF, 0);
#else
    setvbuf(stdout, NULL, _IOLBF, BUFSIZ);
#endif
    g_game_log.started = 1;
    atexit(end);
    for (size_t i = 0; i < FATALS; ++i) {
#if defined(_WIN32)
        g_game_log.before[i] = signal(FATAL[i], on_fatal);
#else
        struct sigaction ours = {0};
        ours.sa_sigaction = on_fatal;
        ours.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&ours.sa_mask);
        sigaction(FATAL[i], &ours, &g_game_log.before[i]);
#endif
    }
    if (kept[0]) fputs(kept, stderr);
    char* full = snx_absolute(g_game_log.path);
    fprintf(stderr, SNX_LOG_WRITTEN "%s\n", full ? full : g_game_log.path);
    free(full);
    return 0;
failed:
    atomic_store_explicit(&g_game_log.file, -1, memory_order_release);
    for (int i = 0; i < 2; ++i) /* a copy started reads its pipe's end and stops */
        if (ends[i][1] >= 0) close_fd(ends[i][1]);
    fclose(g_game_log.stream);
    g_game_log.stream = NULL;
    return -1;
}

const char* snx_game_log_path(void) { return g_game_log.started ? g_game_log.path : NULL; }
void snx_game_log_sync(void) {
    if (!g_game_log.started) return;
    fflush(stdout);
    fflush(stderr);
    wait_idle(WAITS);
}
int snx_game_log_full_take(void) { return atomic_exchange_explicit(&g_game_log.full, 0, memory_order_acq_rel); }
