/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 32-bit bridge's bionic functions whose arguments have ILP32's layouts (arm32.h): directories, memory maps,
   time, the process, the rest of the strings, ctype's tables, wide characters, the environment, the waits on
   descriptors, the names of the network, zlib with its z_stream and SHA-1. The ones whose arguments are integers and
   bytes go to the 64-bit bridge's handlers (arm32_shim.c) */
#define _GNU_SOURCE
#include "android/bridge/arm32.h"
#include "android/bridge/hle_impl.h"
#include "common/utilities/endian.h"
#include <ctype.h>
#include "gnuabi/abi/linux_file.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include <wchar.h>
#include <wctype.h>
#include <zlib.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#if defined(__linux__) || defined(__FreeBSD__)
#include <sys/eventfd.h>
#endif

#define R(n)       (c->r[n])
#define P(n)       (arm32_host(c->r[n]))
#define S(n)       ((const char*)arm32_host(c->r[n]))
#define HANDLER(n) static void n(SnxArmCPU* c, void* data)
#define UNUSED     (void)data
#define SLICE_MS   10

static uint32_t rd32(uint32_t a) { return snx_ld_le32(arm32_mem + a); }
static void wr32(uint32_t a, uint32_t v) { snx_st_le32(arm32_mem + a, v); }
static void set64(SnxArmCPU* c, uint64_t v) {
    c->r[0] = (uint32_t)v;
    c->r[1] = (uint32_t)(v >> 32);
}
static double arg_double(const SnxArmCPU* c, unsigned first) {
    uint64_t bits = arm32_word(c, first) | (uint64_t)arm32_word(c, first + 1) << 32;
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}
static float arg_float(const SnxArmCPU* c, unsigned n) {
    uint32_t bits = arm32_word(c, n);
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}
static void ret_double(SnxArmCPU* c, double d) {
    uint64_t bits;
    memcpy(&bits, &d, sizeof bits);
    set64(c, bits);
}
static void ret_float(SnxArmCPU* c, float f) { memcpy(&c->r[0], &f, sizeof f); }
static uint32_t inside(const void* p) { return p ? arm32_guest(p) : 0; }
static uint32_t fail(int linux_error) {
    arm32_set_errno_linux(linux_error);
    return UINT32_MAX;
}
enum { L_EPERM = 1, L_ENOENT = 2, L_EBADF = 9, L_ENOMEM = 12, L_EINVAL = 22, L_ENOSYS = 38, L_EINTR = 4,
       L_ERANGE = 34 };

/* ---- directories: a DIR is a guest block holding the dirent that readdir gives, found to its host DIR ---- */

/* bionic's 32-bit struct dirent: d_ino (8), d_off (8), d_reclen (2), d_type (1), d_name (256): 280 bytes */
enum { DIRENT = 280, MAX_DIRS = 256 };
static struct {
    pthread_mutex_t lock;
    uint32_t guest[MAX_DIRS];
    DIR* host[MAX_DIRS];
} dirs = {.lock = PTHREAD_MUTEX_INITIALIZER};
static DIR* host_dir(uint32_t g) {
    DIR* d = NULL;
    pthread_mutex_lock(&dirs.lock);
    for (unsigned i = 0; i < MAX_DIRS && !d; ++i)
        if (dirs.guest[i] == g) d = dirs.host[i];
    pthread_mutex_unlock(&dirs.lock);
    return d;
}
HANDLER(f_opendir) {
    UNUSED;
    DIR* d = opendir(S(0));
    if (!d) {
        arm32_set_errno(errno);
        R(0) = 0;
        return;
    }
    uint32_t g = arm32_malloc(DIRENT);
    pthread_mutex_lock(&dirs.lock);
    unsigned i = 0;
    while (i < MAX_DIRS && dirs.guest[i]) ++i;
    if (i < MAX_DIRS && g) {
        dirs.guest[i] = g;
        dirs.host[i] = d;
    }
    pthread_mutex_unlock(&dirs.lock);
    if (i == MAX_DIRS || !g) {
        closedir(d);
        arm32_free(g);
        R(0) = fail(24); /* EMFILE */
        R(0) = 0;
        return;
    }
    R(0) = g;
}
HANDLER(f_readdir) {
    UNUSED;
    DIR* d = host_dir(R(0));
    struct dirent* e = d ? readdir(d) : NULL;
    if (!e) {
        R(0) = 0;
        return;
    }
    uint8_t* o = P(0);
    memset(o, 0, DIRENT);
    uint64_t ino = (uint64_t)e->d_ino;
    memcpy(o, &ino, 8);
    snx_st_le16(o + 16, DIRENT);
    o[18] = e->d_type; /* DT_* are the same numbers */
    snprintf((char*)o + 19, 256, "%s", e->d_name);
    R(0) = R(0);
}
HANDLER(f_closedir) {
    UNUSED;
    uint32_t g = R(0);
    DIR* d = NULL;
    pthread_mutex_lock(&dirs.lock);
    for (unsigned i = 0; i < MAX_DIRS && !d; ++i)
        if (dirs.guest[i] == g) {
            d = dirs.host[i];
            dirs.guest[i] = 0;
        }
    pthread_mutex_unlock(&dirs.lock);
    R(0) = d ? (uint32_t)closedir(d) : fail(L_EBADF);
    if (d) arm32_free(g);
}
HANDLER(f_rewinddir) {
    UNUSED;
    DIR* d = host_dir(R(0));
    if (d) rewinddir(d);
}
HANDLER(f_dirfd) {
    UNUSED;
    DIR* d = host_dir(R(0));
    R(0) = d ? (uint32_t)dirfd(d) : fail(L_EBADF);
}

/* getcwd(buffer, size): a guest buffer of its own for NULL, as bionic gives */
HANDLER(f_getcwd) {
    UNUSED;
    uint32_t size = R(1) ? R(1) : 4096, buffer = R(0) ? R(0) : arm32_malloc(size);
    if (!buffer || !getcwd((char*)arm32_mem + buffer, size)) {
        arm32_set_errno(errno);
        if (!R(0)) arm32_free(buffer);
        R(0) = 0;
        return;
    }
    R(0) = buffer;
}
/* utime(path, times): utimbuf is two 32-bit time_t */
HANDLER(f_utime) {
    UNUSED;
    struct utimbuf t;
    if (R(1)) {
        t.actime = (time_t)(int32_t)rd32(R(1));
        t.modtime = (time_t)(int32_t)rd32(R(1) + 4);
    }
    R(0) = utime(S(0), R(1) ? &t : NULL) ? (arm32_set_errno(errno), UINT32_MAX) : 0;
}
/* sendfile(out, in, offset, count): a copy through a buffer, at *offset when it is given */
HANDLER(f_sendfile) {
    UNUSED;
    int out = (int)R(0), in = (int)R(1);
    uint32_t offset = R(2), count = R(3);
    char buffer[65536];
    uint32_t done = 0;
    off_t at = offset ? (off_t)(int32_t)rd32(offset) : 0;
    while (done < count) {
        size_t n = count - done < sizeof buffer ? count - done : sizeof buffer;
        ssize_t got = offset ? pread(in, buffer, n, at) : read(in, buffer, n);
        if (got <= 0) break;
        ssize_t put = write(out, buffer, (size_t)got);
        if (put <= 0) break;
        done += (uint32_t)put;
        at += put;
    }
    if (offset) wr32(offset, (uint32_t)at);
    R(0) = done;
}
/* readv and writev(fd, iov, count): iovec is a 32-bit base and length */
HANDLER(f_readv) {
    bool write_ = data != NULL;
    uint32_t iov = R(1), count = R(2), total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t base = rd32(iov + 8 * i), len = rd32(iov + 8 * i + 4);
        ssize_t n = write_ ? write((int)R(0), arm32_host(base), len) : read((int)R(0), arm32_host(base), len);
        if (n < 0) {
            if (!total) {
                arm32_set_errno(errno);
                R(0) = UINT32_MAX;
                return;
            }
            break;
        }
        total += (uint32_t)n;
        if ((uint32_t)n < len) break;
    }
    R(0) = total;
}

/* fcntl(fd, command, argument): the descriptor's flags in ARM's numbers; the locks of struct flock (16 bytes) and
   flock64 (24 bytes) */
static uint32_t to_host_flags(uint32_t f) {
    int h = (int)(f & 3);
    if (f & 02000) h |= O_APPEND;
    if (f & 04000) h |= O_NONBLOCK;
    return (uint32_t)h;
}
static uint32_t from_host_flags(int h) {
    uint32_t f = (uint32_t)(h & O_ACCMODE);
    if (h & O_APPEND) f |= 02000;
    if (h & O_NONBLOCK) f |= 04000;
    return f;
}
HANDLER(f_fcntl) {
    UNUSED;
    int fd = (int)R(0);
    uint32_t cmd = R(1), arg = R(2);
    int r;
    switch (cmd) {
    case 0: r = fcntl(fd, F_DUPFD, (int)arg); break;
    case 1030: r = fcntl(fd, F_DUPFD_CLOEXEC, (int)arg); break;
    case 1: r = fcntl(fd, F_GETFD); break;
    case 2: r = fcntl(fd, F_SETFD, (int)arg & FD_CLOEXEC); break;
    case 3:
        r = fcntl(fd, F_GETFL);
        if (r >= 0) r = (int)from_host_flags(r);
        break;
    case 4: r = fcntl(fd, F_SETFL, (int)to_host_flags(arg)); break;
    case 5: case 6: case 7: case 12: case 13: case 14: {
        bool wide = cmd >= 12;
        struct flock l = {0};
        short type = (short)snx_ld_le16(arm32_mem + arg);
        l.l_type = type == 0 ? F_RDLCK : type == 1 ? F_WRLCK : F_UNLCK;
        l.l_whence = (short)snx_ld_le16(arm32_mem + arg + 2);
        if (wide) {
            int64_t start, len;
            memcpy(&start, arm32_mem + arg + 8, 8);
            memcpy(&len, arm32_mem + arg + 16, 8);
            l.l_start = (off_t)start;
            l.l_len = (off_t)len;
        } else {
            l.l_start = (off_t)(int32_t)rd32(arg + 4);
            l.l_len = (off_t)(int32_t)rd32(arg + 8);
        }
        int hcmd = cmd == 5 || cmd == 12 ? F_GETLK : cmd == 6 || cmd == 13 ? F_SETLK : F_SETLKW;
        r = fcntl(fd, hcmd, &l);
        if (!r && hcmd == F_GETLK) {
            snx_st_le16(arm32_mem + arg, l.l_type == F_RDLCK ? 0 : l.l_type == F_WRLCK ? 1 : 2);
            wr32(arg + (wide ? 24 : 12), (uint32_t)l.l_pid);
        }
        break;
    }
    default: r = -1, errno = EINVAL; break;
    }
    R(0) = r < 0 ? (arm32_set_errno(errno), UINT32_MAX) : (uint32_t)r;
}

/* poll(fds, count, timeout) and epoll_wait(epfd, events, max, timeout): the 64-bit bridge's (pollfd and ARM's
   epoll_event have AArch64's layouts), in slices so that the thread's signals run */
static void waited(SnxArmCPU* c, const char* name, unsigned timeout_arg) {
    int32_t timeout = (int32_t)R(timeout_arg);
    uint64_t x[4] = {(uint64_t)(int64_t)(int32_t)R(0), R(1) ? (uint64_t)(uintptr_t)arm32_host(R(1)) : 0, R(2), 0};
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        int32_t left = timeout;
        if (timeout > 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            int64_t spent = (int64_t)(now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
            left = spent >= timeout ? 0 : (int32_t)(timeout - spent);
        }
        x[timeout_arg] = (uint64_t)(int64_t)(left < 0 || left > SLICE_MS ? SLICE_MS : left);
        int32_t r = (int32_t)arm32_call64(name, x, timeout_arg + 1, NULL);
        if (r != 0 || left == 0) {
            R(0) = (uint32_t)r;
            return;
        }
        if (__atomic_load_n(&arm32_pending_signals, __ATOMIC_RELAXED)) {
            arm32_signals_run(c);
            R(0) = fail(L_EINTR);
            return;
        }
    }
}
HANDLER(f_poll) {
    UNUSED;
    waited(c, "poll", 2);
}
HANDLER(f_epoll_wait) {
    UNUSED;
    waited(c, "epoll_wait", 3);
}
/* select(count, read, write, except, timeout): fd_set's bits are the host's; timeval is two 32-bit words */
HANDLER(f_select) {
    UNUSED;
    uint32_t tv = arm32_word(c, 4);
    struct timeval t, *tp = NULL;
    if (tv) {
        t.tv_sec = (time_t)(int32_t)rd32(tv);
        t.tv_usec = (suseconds_t)(int32_t)rd32(tv + 4);
        tp = &t;
    }
    int r = select((int)R(0), R(1) ? P(1) : NULL, R(2) ? P(2) : NULL, R(3) ? P(3) : NULL, tp);
    R(0) = r < 0 ? (arm32_set_errno(errno), UINT32_MAX) : (uint32_t)r;
}

/* ---- memory maps ---- */

HANDLER(f_mprotect) {
    UNUSED;
    R(0) = 0; /* the window is readable and writable, and code compiles wherever it is */
}
/* madvise(address, length, advice): MADV_DONTNEED's pages read as zeros after it, as on Linux */
HANDLER(f_madvise) {
    UNUSED;
    if (R(2) == 4 && R(0) && R(1)) memset(P(0), 0, R(1));
    R(0) = 0;
}
/* mremap(old, old_size, new_size, flags): a smaller map in place, a larger one moved when MREMAP_MAYMOVE allows */
HANDLER(f_mremap) {
    UNUSED;
    uint32_t old = R(0), old_size = (R(1) + 4095) & ~4095u, size = (R(2) + 4095) & ~4095u;
    if (size <= old_size) {
        if (size < old_size) arm32_unmap(old + size, old_size - size);
        R(0) = old;
        return;
    }
    if (!(R(3) & 1)) {
        R(0) = fail(L_ENOMEM);
        return;
    }
    uint32_t n = arm32_map(size);
    if (!n) {
        R(0) = fail(L_ENOMEM);
        return;
    }
    memcpy(arm32_mem + n, arm32_mem + old, old_size);
    arm32_unmap(old, old_size);
    R(0) = n;
}

/* ---- time ---- */

/* bionic's 32-bit struct tm: 9 ints, tm_gmtoff, tm_zone */
static void get_tm(uint32_t at, struct tm* tm) {
    memset(tm, 0, sizeof *tm);
    int* f[9] = {&tm->tm_sec, &tm->tm_min, &tm->tm_hour, &tm->tm_mday, &tm->tm_mon,
                 &tm->tm_year, &tm->tm_wday, &tm->tm_yday, &tm->tm_isdst};
    for (unsigned i = 0; i < 9; ++i) *f[i] = (int32_t)rd32(at + 4 * i);
    tm->tm_gmtoff = (long)(int32_t)rd32(at + 36);
}
static void put_tm(uint32_t at, const struct tm* tm) {
    int32_t v[9] = {tm->tm_sec, tm->tm_min, tm->tm_hour, tm->tm_mday, tm->tm_mon,
                    tm->tm_year, tm->tm_wday, tm->tm_yday, tm->tm_isdst};
    for (unsigned i = 0; i < 9; ++i) wr32(at + 4 * i, (uint32_t)v[i]);
    wr32(at + 36, (uint32_t)tm->tm_gmtoff);
    wr32(at + 40, 0);
}
HANDLER(f_mktime) {
    UNUSED;
    struct tm tm;
    get_tm(R(0), &tm);
    time_t t = mktime(&tm);
    put_tm(R(0), &tm);
    R(0) = (uint32_t)t;
}
HANDLER(f_localtime_r) {
    struct tm tm;
    time_t t = (time_t)(int32_t)rd32(R(0));
    if (!(data ? gmtime_r(&t, &tm) : localtime_r(&t, &tm))) {
        R(0) = 0;
        return;
    }
    put_tm(R(1), &tm);
    R(0) = R(1);
}
HANDLER(f_strftime) {
    UNUSED;
    struct tm tm;
    get_tm(R(3), &tm);
    R(0) = (uint32_t)strftime(P(0), R(1), S(2), &tm);
}
HANDLER(f_clock) {
    UNUSED;
    R(0) = (uint32_t)clock();
}
HANDLER(f_clock_getres) {
    UNUSED;
    if (R(1)) {
        wr32(R(1), 0);
        wr32(R(1) + 4, 1);
    }
    R(0) = 0;
}
/* getrusage(who, usage): two timevals of 32-bit words, then 14 longs */
HANDLER(f_getrusage) {
    UNUSED;
    struct rusage u;
    if (getrusage(R(0) == (uint32_t)-1 ? RUSAGE_CHILDREN : RUSAGE_SELF, &u)) {
        R(0) = fail(L_EINVAL);
        return;
    }
    memset(P(1), 0, 72);
    wr32(R(1), (uint32_t)u.ru_utime.tv_sec);
    wr32(R(1) + 4, (uint32_t)u.ru_utime.tv_usec);
    wr32(R(1) + 8, (uint32_t)u.ru_stime.tv_sec);
    wr32(R(1) + 12, (uint32_t)u.ru_stime.tv_usec);
    wr32(R(1) + 16, (uint32_t)u.ru_maxrss);
    R(0) = 0;
}

/* ---- the process ---- */

HANDLER(f__exit) {
    UNUSED;
    fflush(stdout);
    _exit((int)R(0));
}
HANDLER(f_enosys) {
    UNUSED;
    R(0) = fail(L_ENOSYS);
}
HANDLER(f_null_enosys) {
    UNUSED;
    arm32_set_errno_linux(L_ENOSYS);
    R(0) = 0;
}
HANDLER(f_eperm) {
    UNUSED;
    R(0) = fail(L_EPERM);
}
HANDLER(f_zero) {
    UNUSED;
    R(0) = 0;
}
HANDLER(f_getresuid) {
    UNUSED;
    uint32_t id = (uint32_t)getuid();
    for (unsigned i = 0; i < 3; ++i)
        if (R(i)) wr32(R(i), id);
    R(0) = 0;
}
HANDLER(f_getdtablesize) {
    UNUSED;
    R(0) = 1024;
}
/* getpwuid and getpwnam: the app's own user, bionic's 32-bit struct passwd (name, passwd, uid, gid, dir, shell) */
static uint32_t passwd_entry;
HANDLER(f_getpw) {
    UNUSED;
    if (!passwd_entry) {
        uint32_t p = arm32_malloc(24);
        wr32(p, arm32_strdup("u0_a100"));
        wr32(p + 4, 0);
        wr32(p + 8, (uint32_t)getuid());
        wr32(p + 12, (uint32_t)getgid());
        wr32(p + 16, arm32_strdup("/"));
        wr32(p + 20, arm32_strdup("/system/bin/sh"));
        passwd_entry = p;
    }
    R(0) = passwd_entry;
}

/* ---- the environment: the guest's own, as an environ array in its window ---- */

static pthread_mutex_t env_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t environ_word; /* the object environ: the array's address */
static char** env;
static size_t env_count;
static void env_publish(void) {
    uint32_t old = rd32(environ_word);
    uint32_t a = arm32_malloc((uint32_t)(env_count + 1) * 4);
    for (size_t i = 0; i < env_count; ++i) wr32(a + 4 * (uint32_t)i, arm32_strdup(env[i]));
    wr32(a + 4 * (uint32_t)env_count, 0);
    wr32(environ_word, a);
    (void)old; /* the old strings stay: the guest may hold their pointers */
}
static ssize_t env_find(const char* name) {
    size_t n = strlen(name);
    for (size_t i = 0; i < env_count; ++i)
        if (!strncmp(env[i], name, n) && env[i][n] == '=') return (ssize_t)i;
    return -1;
}
HANDLER(f_getenv) {
    UNUSED;
    pthread_mutex_lock(&env_lock);
    ssize_t i = env_find(S(0));
    uint32_t r = 0;
    if (i >= 0) {
        uint32_t array = rd32(environ_word);
        r = rd32(array + 4 * (uint32_t)i) + (uint32_t)strlen(S(0)) + 1;
    }
    pthread_mutex_unlock(&env_lock);
    R(0) = r;
}
HANDLER(f_setenv) {
    UNUSED;
    const char* name = S(0);
    if (!name || !*name || strchr(name, '=')) {
        R(0) = fail(L_EINVAL);
        return;
    }
    pthread_mutex_lock(&env_lock);
    ssize_t i = env_find(name);
    if (i < 0 || R(2)) {
        size_t n = strlen(name) + strlen(S(1)) + 2;
        char* entry = malloc(n);
        snprintf(entry, n, "%s=%s", name, S(1));
        if (i >= 0) {
            free(env[i]);
            env[i] = entry;
        } else {
            env = realloc(env, (env_count + 1) * sizeof *env);
            env[env_count++] = entry;
        }
        env_publish();
    }
    pthread_mutex_unlock(&env_lock);
    R(0) = 0;
}
HANDLER(f_unsetenv) {
    UNUSED;
    pthread_mutex_lock(&env_lock);
    ssize_t i = env_find(S(0));
    if (i >= 0) {
        free(env[i]);
        env[i] = env[--env_count];
        env_publish();
    }
    pthread_mutex_unlock(&env_lock);
    R(0) = 0;
}
HANDLER(f_setlocale) {
    UNUSED;
    static uint32_t c_locale;
    if (!c_locale) c_locale = arm32_strdup("C");
    R(0) = c_locale;
}

/* ---- strings ---- */

HANDLER(f_memmem) {
    UNUSED;
    R(0) = inside(memmem(P(0), R(1), P(2), R(3)));
}
HANDLER(f_strcasestr) {
    UNUSED;
    R(0) = inside(strcasestr(S(0), S(1)));
}
HANDLER(f_strpbrk) {
    UNUSED;
    R(0) = inside(strpbrk(S(0), S(1)));
}
HANDLER(f_strlcpy) {
    UNUSED;
    R(0) = (uint32_t)hle_strlcpy(P(0), S(1), R(2));
}
HANDLER(f_strlcat) {
    UNUSED;
    size_t n = strnlen(P(0), R(2));
    R(0) = n == R(2) ? (uint32_t)(n + strlen(S(1))) : (uint32_t)(n + hle_strlcpy((char*)P(0) + n, S(1), R(2) - n));
}
HANDLER(f_strnlen) {
    UNUSED;
    R(0) = (uint32_t)strnlen(S(0), R(1));
}
HANDLER(f_strndup) {
    UNUSED;
    size_t n = strnlen(S(0), R(1));
    uint32_t g = arm32_malloc((uint32_t)n + 1);
    if (g) {
        memcpy(arm32_mem + g, S(0), n);
        arm32_mem[g + n] = 0;
    }
    R(0) = g;
}
/* strsep(&string, delimiters) */
HANDLER(f_strsep) {
    UNUSED;
    uint32_t s = rd32(R(0));
    if (!s) {
        R(0) = 0;
        return;
    }
    char* p = (char*)arm32_mem + s;
    size_t n = strcspn(p, S(1));
    wr32(R(0), p[n] ? s + (uint32_t)n + 1 : 0);
    p[n] = 0;
    R(0) = s;
}
/* strtok_r(string, delimiters, &save), and strtok with a save of its own */
static uint32_t tok(uint32_t s, const char* delimiters, uint32_t save) {
    if (!s) s = rd32(save);
    if (!s) return 0;
    char* p = (char*)arm32_mem + s;
    p += strspn(p, delimiters);
    if (!*p) {
        wr32(save, 0);
        return 0;
    }
    size_t n = strcspn(p, delimiters);
    uint32_t start = arm32_guest(p);
    wr32(save, p[n] ? start + (uint32_t)n + 1 : 0);
    p[n] = 0;
    return start;
}
HANDLER(f_strtok_r) {
    UNUSED;
    R(0) = tok(R(0), S(1), R(2));
}
HANDLER(f_strtok) {
    UNUSED;
    static uint32_t save;
    if (!save) save = arm32_malloc(4);
    R(0) = tok(R(0), S(1), save);
}
HANDLER(f_strtoul) {
    UNUSED;
    char* end;
    unsigned long long v = strtoull(S(0), &end, (int)R(2));
    if (R(1)) wr32(R(1), inside(end));
    R(0) = v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}
HANDLER(f_strtoll) {
    char* end;
    uint64_t v = data ? (uint64_t)strtoull(S(0), &end, (int)R(2)) : (uint64_t)strtoll(S(0), &end, (int)R(2));
    if (R(1)) wr32(R(1), inside(end));
    set64(c, v);
}
HANDLER(f_strtof) {
    UNUSED;
    char* end;
    float f = strtof(S(0), &end);
    if (R(1)) wr32(R(1), inside(end));
    ret_float(c, f);
}
HANDLER(f_strcoll) {
    UNUSED;
    R(0) = (uint32_t)strcmp(S(0), S(1));
}
HANDLER(f_strxfrm) {
    UNUSED;
    size_t n = strlen(S(1));
    if (R(2)) hle_strlcpy(P(0), S(1), R(2));
    R(0) = (uint32_t)n;
}
/* strerror(errno): a guest copy of each one's text, made once */
HANDLER(f_strerror) {
    UNUSED;
    static uint32_t texts[200];
    uint32_t e = R(0);
    if (e < 200 && texts[e]) {
        R(0) = texts[e];
        return;
    }
    char text[128];
    hle_strerror_r((int)e, text, sizeof text);
    uint32_t g = arm32_strdup(text);
    if (e < 200) texts[e] = g;
    R(0) = g;
}
HANDLER(f_strerror_r) {
    UNUSED;
    hle_strerror_r((int)R(0), P(1), R(2));
    R(0) = 0;
}

/* qsort(base, count, size, compare) and bsearch(key, base, count, size, compare): the guest's compare called for
   each pair, through the host's sort */
static _Thread_local uint32_t t_compare;
static int compare(const void* a, const void* b) {
    uint32_t args[2] = {arm32_guest(a), arm32_guest(b)};
    return (int32_t)arm32_call(t_compare, args, 2, NULL);
}
HANDLER(f_qsort) {
    UNUSED;
    uint32_t saved = t_compare;
    t_compare = R(3);
    if (R(1) && R(2)) qsort(P(0), R(1), R(2), compare);
    t_compare = saved;
}
HANDLER(f_bsearch) {
    UNUSED;
    uint32_t saved = t_compare;
    t_compare = arm32_word(c, 4);
    void* r = R(2) && R(3) ? bsearch(P(0), P(1), R(2), R(3), compare) : NULL;
    t_compare = saved;
    R(0) = inside(r);
}
/* div and ldiv(numerator, denominator): a struct of 8 bytes, which AAPCS returns in the memory that r0 names */
HANDLER(f_div) {
    UNUSED;
    int32_t n = (int32_t)R(1), d = (int32_t)R(2);
    wr32(R(0), d ? (uint32_t)(n / d) : 0);
    wr32(R(0) + 4, d ? (uint32_t)(n % d) : 0);
}

/* ---- ctype: bionic's tables, and the functions ---- */

static uint32_t ctype_word, tolower_word, toupper_word;
static void make_ctype(void) {
    /* _ctype_: 257 bytes from EOF on: _U 1, _L 2, _N 4, _S 8, _P 0x10, _C 0x20, _X 0x40, _B 0x80 */
    uint32_t t = arm32_malloc(260);
    arm32_mem[t] = 0;
    for (int ch = 0; ch < 256; ++ch) {
        uint8_t f = 0;
        if (ch < 128) {
            if (isupper(ch)) f |= 0x01;
            if (islower(ch)) f |= 0x02;
            if (isdigit(ch)) f |= 0x04;
            if (isspace(ch)) f |= 0x08;
            if (ispunct(ch)) f |= 0x10;
            if (iscntrl(ch)) f |= 0x20;
            if (isxdigit(ch) && !isdigit(ch)) f |= 0x40;
            if (ch == ' ') f |= 0x80;
        }
        arm32_mem[t + 1 + (uint32_t)ch] = f;
    }
    ctype_word = arm32_malloc(4);
    wr32(ctype_word, t);
    /* _tolower_tab_ and _toupper_tab_: 257 shorts from EOF on */
    uint32_t lo = arm32_malloc(2 * 257), up = arm32_malloc(2 * 257);
    snx_st_le16(arm32_mem + lo, 0xffff);
    snx_st_le16(arm32_mem + up, 0xffff);
    for (int ch = 0; ch < 256; ++ch) {
        snx_st_le16(arm32_mem + lo + 2 + 2 * (uint32_t)ch, (uint16_t)(ch < 128 ? tolower(ch) : ch));
        snx_st_le16(arm32_mem + up + 2 + 2 * (uint32_t)ch, (uint16_t)(ch < 128 ? toupper(ch) : ch));
    }
    tolower_word = arm32_malloc(4);
    toupper_word = arm32_malloc(4);
    wr32(tolower_word, lo);
    wr32(toupper_word, up);
}
#define CTYPE(name) \
    HANDLER(f_##name) { \
        UNUSED; \
        int ch = (int32_t)R(0); \
        R(0) = ch >= 0 && ch < 128 && name(ch) ? 1 : 0; \
    }
CTYPE(isalnum)
CTYPE(isalpha)
CTYPE(isdigit)
CTYPE(isxdigit)
CTYPE(isspace)
CTYPE(isupper)
CTYPE(islower)
CTYPE(ispunct)
CTYPE(isprint)
CTYPE(isgraph)
CTYPE(iscntrl)

/* ---- wide characters: wchar_t is 32 bits on Android and on the hosts ---- */

HANDLER(f_wcslen) {
    UNUSED;
    R(0) = (uint32_t)wcslen(P(0));
}
HANDLER(f_wcscmp) {
    UNUSED;
    R(0) = (uint32_t)wcscmp(P(0), P(1));
}
HANDLER(f_wcsncmp) {
    UNUSED;
    R(0) = (uint32_t)wcsncmp(P(0), P(1), R(2));
}
HANDLER(f_wcscpy) {
    UNUSED;
    wcscpy(P(0), P(1));
}
HANDLER(f_wmemchr) {
    UNUSED;
    R(0) = inside(wmemchr(P(0), (wchar_t)R(1), R(2)));
}
HANDLER(f_wmemcmp) {
    UNUSED;
    R(0) = (uint32_t)wmemcmp(P(0), P(1), R(2));
}
HANDLER(f_wmemcpy) {
    UNUSED;
    memmove(P(0), P(1), (size_t)R(2) * 4);
}
HANDLER(f_wmemset) {
    UNUSED;
    wmemset(P(0), (wchar_t)R(1), R(2));
}
HANDLER(f_towlower) {
    UNUSED;
    R(0) = R(0) < 128 ? (uint32_t)tolower((int)R(0)) : R(0);
}
HANDLER(f_towupper) {
    UNUSED;
    R(0) = R(0) < 128 ? (uint32_t)toupper((int)R(0)) : R(0);
}
/* mbrtowc and wcrtomb in UTF-8, as bionic's */
HANDLER(f_mbrtowc) {
    UNUSED;
    const unsigned char* s = P(1);
    uint32_t n = R(2);
    if (!s) {
        R(0) = 0;
        return;
    }
    if (!n) {
        R(0) = (uint32_t)-2;
        return;
    }
    uint32_t len = s[0] < 0x80 ? 1 : (s[0] & 0xe0) == 0xc0 ? 2 : (s[0] & 0xf0) == 0xe0 ? 3 : (s[0] & 0xf8) == 0xf0 ? 4 : 0;
    if (!len) {
        R(0) = fail(84); /* EILSEQ */
        return;
    }
    if (len > n) {
        R(0) = (uint32_t)-2;
        return;
    }
    uint32_t w = len == 1 ? s[0] : (uint32_t)(s[0] & (0x7f >> len));
    for (uint32_t i = 1; i < len; ++i) w = w << 6 | (s[i] & 0x3f);
    if (R(0)) wr32(R(0), w);
    R(0) = w ? len : 0;
}
HANDLER(f_wcrtomb) {
    UNUSED;
    uint32_t w = R(1);
    if (!R(0)) {
        R(0) = 1;
        return;
    }
    unsigned char* o = P(0);
    uint32_t n = w < 0x80 ? 1 : w < 0x800 ? 2 : w < 0x10000 ? 3 : 4;
    if (n == 1) {
        o[0] = (unsigned char)w;
    } else {
        for (uint32_t i = n - 1; i > 0; --i, w >>= 6) o[i] = (unsigned char)(0x80 | (w & 0x3f));
        o[0] = (unsigned char)((0xf00 >> n) | w);
    }
    R(0) = n;
}
HANDLER(f_wcsxfrm) {
    UNUSED;
    size_t n = wcslen(P(1));
    if (R(2)) {
        size_t m = n < R(2) ? n : R(2) - 1;
        memmove(P(0), P(1), m * 4);
        wr32(R(0) + 4 * (uint32_t)m, 0);
    }
    R(0) = (uint32_t)n;
}
HANDLER(f_wcsftime) {
    UNUSED;
    /* the format's characters as bytes, then the result as wide characters */
    char format[256], out[512];
    size_t i = 0;
    for (const uint32_t* w = P(2); w && *w && i < sizeof format - 1; ++w) format[i++] = *w < 128 ? (char)*w : '?';
    format[i] = 0;
    struct tm tm;
    get_tm(R(3), &tm);
    size_t n = strftime(out, sizeof out, format, &tm);
    if (n >= R(1)) {
        R(0) = 0;
        return;
    }
    for (size_t k = 0; k < n; ++k) wr32(R(0) + 4 * (uint32_t)k, (unsigned char)out[k]);
    wr32(R(0) + 4 * (uint32_t)n, 0);
    R(0) = (uint32_t)n;
}
HANDLER(f_getwc) {
    UNUSED;
    FILE* f = arm32_host_file(R(0));
    int ch = f ? getc(f) : EOF;
    R(0) = ch == EOF ? UINT32_MAX : (uint32_t)ch;
}
HANDLER(f_putwc) {
    UNUSED;
    FILE* f = arm32_host_file(R(1));
    R(0) = f && R(0) < 128 && putc((int)R(0), f) != EOF ? R(0) : UINT32_MAX;
}
HANDLER(f_ungetwc) {
    UNUSED;
    FILE* f = arm32_host_file(R(1));
    R(0) = f && R(0) < 128 && ungetc((int)R(0), f) != EOF ? R(0) : UINT32_MAX;
}

/* ---- libm's rest ---- */

#define MATH1(name) \
    HANDLER(m_##name) { \
        UNUSED; \
        ret_double(c, name(arg_double(c, 0))); \
    } \
    HANDLER(m_##name##f) { \
        UNUSED; \
        ret_float(c, name##f(arg_float(c, 0))); \
    }
MATH1(cosh)
MATH1(sinh)
MATH1(tanh)
MATH1(rint)
MATH1(trunc)
MATH1(exp2)
MATH1(log2)
MATH1(cbrt)
HANDLER(m_frexp) {
    UNUSED;
    int e;
    ret_double(c, frexp(arg_double(c, 0), &e));
    wr32(arm32_word(c, 2), (uint32_t)e);
}
HANDLER(m_ldexp) {
    UNUSED;
    ret_double(c, ldexp(arg_double(c, 0), (int)arm32_word(c, 2)));
}
HANDLER(m_ldexpf) {
    UNUSED;
    ret_float(c, ldexpf(arg_float(c, 0), (int)R(1)));
}
HANDLER(m_modff) {
    UNUSED;
    float i;
    ret_float(c, modff(arg_float(c, 0), &i));
    memcpy(P(1), &i, 4);
}
HANDLER(m_modf) {
    UNUSED;
    double i;
    ret_double(c, modf(arg_double(c, 0), &i));
    memcpy(arm32_host(arm32_word(c, 2)), &i, 8);
}
HANDLER(m_isnan) {
    UNUSED;
    R(0) = isnan(arg_double(c, 0)) ? 1 : 0;
}
HANDLER(m_isinf) {
    UNUSED;
    R(0) = isinf(arg_double(c, 0)) ? 1 : 0;
}
/* bionic's FP_INFINITE 1, FP_NAN 2, FP_NORMAL 4, FP_SUBNORMAL 8, FP_ZERO 16 */
static uint32_t classify(int k) {
    return k == FP_INFINITE ? 1 : k == FP_NAN ? 2 : k == FP_NORMAL ? 4 : k == FP_SUBNORMAL ? 8 : 16;
}
HANDLER(m_fpclassifyf) {
    UNUSED;
    R(0) = classify(fpclassify(arg_float(c, 0)));
}
HANDLER(m_fpclassifyd) {
    UNUSED;
    R(0) = classify(fpclassify(arg_double(c, 0)));
}

/* ---- the names of the network ---- */

/* Linux's AF_INET 2 and AF_INET6 10, the host's own */
static int host_family(uint32_t f) { return f == 2 ? AF_INET : f == 10 ? AF_INET6 : -1; }
HANDLER(n_inet_addr) {
    UNUSED;
    struct in_addr a;
    R(0) = inet_aton(S(0), &a) ? (uint32_t)a.s_addr : UINT32_MAX;
}
HANDLER(n_inet_aton) {
    UNUSED;
    struct in_addr a;
    int ok = inet_aton(S(0), &a);
    if (ok && R(1)) memcpy(P(1), &a.s_addr, 4);
    R(0) = (uint32_t)ok;
}
HANDLER(n_inet_ntoa) {
    UNUSED;
    static _Thread_local uint32_t buffer;
    if (!buffer) buffer = arm32_malloc(16);
    struct in_addr a = {(in_addr_t)R(0)};
    snprintf((char*)arm32_mem + buffer, 16, "%s", inet_ntoa(a));
    R(0) = buffer;
}
HANDLER(n_inet_ntop) {
    UNUSED;
    int f = host_family(R(0));
    R(0) = f >= 0 && inet_ntop(f, P(1), P(2), (socklen_t)R(3)) ? R(2) : (arm32_set_errno_linux(97), 0);
}
HANDLER(n_inet_pton) {
    UNUSED;
    int f = host_family(R(0));
    R(0) = f >= 0 ? (uint32_t)inet_pton(f, S(1), P(2)) : fail(97); /* EAFNOSUPPORT */
}
/* getaddrinfo(node, service, hints, result): the 64-bit bridge's (its hints and its list have AArch64's layout), its
   list copied into bionic's 32-bit struct addrinfo: flags, family, socktype, protocol, addrlen, canonname, addr,
   next */
HANDLER(n_getaddrinfo) {
    UNUSED;
    uint8_t hints[48] = {0};
    if (R(2))
        for (unsigned i = 0; i < 4; ++i) memcpy(hints + 4 * i, arm32_mem + R(2) + 4 * i, 4);
    uint64_t list = 0;
    uint64_t x[4] = {R(0) ? (uint64_t)(uintptr_t)P(0) : 0, R(1) ? (uint64_t)(uintptr_t)P(1) : 0,
                     R(2) ? (uint64_t)(uintptr_t)hints : 0, (uint64_t)(uintptr_t)&list};
    bool found;
    int32_t r = (int32_t)arm32_call64("getaddrinfo", x, 4, &found);
    if (!found) r = 4; /* EAI_FAIL */
    uint32_t first = 0, previous = 0;
    for (const uint8_t* a = (const uint8_t*)(uintptr_t)list; !r && a;) {
        uint32_t g = arm32_malloc(32);
        memset(arm32_mem + g, 0, 32);
        memcpy(arm32_mem + g, a, 16);
        uint32_t addrlen;
        memcpy(&addrlen, a + 16, 4);
        wr32(g + 16, addrlen);
        const char* canon;
        const void* addr;
        const uint8_t* next;
        memcpy(&canon, a + 24, sizeof canon);
        memcpy(&addr, a + 32, sizeof addr);
        memcpy(&next, a + 40, sizeof next);
        if (canon) wr32(g + 20, arm32_strdup(canon));
        if (addr && addrlen) {
            uint32_t sa = arm32_malloc(addrlen);
            memcpy(arm32_mem + sa, addr, addrlen);
            wr32(g + 24, sa);
        }
        if (previous) wr32(previous + 28, g);
        else first = g;
        previous = g;
        a = next;
    }
    if (list) {
        uint64_t y[1] = {list};
        arm32_call64("freeaddrinfo", y, 1, NULL);
    }
    if (R(3)) wr32(R(3), r ? 0 : first);
    R(0) = (uint32_t)r;
}
HANDLER(n_freeaddrinfo) {
    UNUSED;
    for (uint32_t a = R(0); a;) {
        uint32_t next = rd32(a + 28);
        arm32_free(rd32(a + 20));
        arm32_free(rd32(a + 24));
        arm32_free(a);
        a = next;
    }
}
HANDLER(n_gai_strerror) {
    UNUSED;
    static uint32_t text;
    if (!text) text = arm32_strdup("Name resolution failed");
    R(0) = text;
}
static uint32_t h_errno_word;
HANDLER(n_get_h_errno) {
    UNUSED;
    R(0) = h_errno_word;
}
HANDLER(n_gethostbyname) {
    UNUSED;
    wr32(h_errno_word, 1); /* HOST_NOT_FOUND */
    R(0) = 0;
}
/* getsockopt and setsockopt(fd, level, name, value, length): SO_RCVTIMEO and SO_SNDTIMEO take a timeval of two
   32-bit words, which the 64-bit bridge's take as two 64-bit ones */
static void sockopt(SnxArmCPU* c, bool set) {
    uint32_t level = R(1), name = R(2), value = R(3), length = arm32_word(c, 4);
    bool timeval = level == 1 && (name == 20 || name == 21);
    int64_t tv[2] = {0, 0};
    uint32_t tv_length = 16;
    if (timeval && set && value) {
        tv[0] = (int32_t)rd32(value);
        tv[1] = (int32_t)rd32(value + 4);
    }
    uint64_t x[5] = {(uint64_t)(int64_t)(int32_t)R(0), level, name,
                     timeval ? (uint64_t)(uintptr_t)tv : value ? (uint64_t)(uintptr_t)arm32_host(value) : 0,
                     set ? (timeval ? 16 : length) : (timeval ? (uint64_t)(uintptr_t)&tv_length
                                                             : (uint64_t)(uintptr_t)arm32_host(length))};
    R(0) = (uint32_t)arm32_call64(set ? "setsockopt" : "getsockopt", x, 5, NULL);
    if (timeval && !set && value) {
        wr32(value, (uint32_t)tv[0]);
        wr32(value + 4, (uint32_t)tv[1]);
        if (length) wr32(length, 8);
    }
}
HANDLER(n_getsockopt) {
    UNUSED;
    sockopt(c, false);
}
HANDLER(n_setsockopt) {
    UNUSED;
    sockopt(c, true);
}

/* ---- zlib: a guest z_stream (14 words: next_in, avail_in, total_in, next_out, avail_out, total_out, msg, state,
   zalloc, zfree, opaque, data_type, adler, reserved) is a host one by its address, its fields copied each call ---- */

typedef struct ZStream {
    uint32_t guest;
    z_stream host;
    struct ZStream* next;
} ZStream;
static ZStream* zstreams;
static pthread_mutex_t z_lock = PTHREAD_MUTEX_INITIALIZER;
static ZStream* z_of(uint32_t g, bool make) {
    pthread_mutex_lock(&z_lock);
    ZStream* z = zstreams;
    while (z && z->guest != g) z = z->next;
    if (!z && make && (z = calloc(1, sizeof *z))) {
        z->guest = g;
        z->next = zstreams;
        zstreams = z;
    }
    pthread_mutex_unlock(&z_lock);
    return z;
}
static void z_forget(uint32_t g) {
    pthread_mutex_lock(&z_lock);
    for (ZStream** at = &zstreams; *at; at = &(*at)->next)
        if ((*at)->guest == g) {
            ZStream* z = *at;
            *at = z->next;
            free(z);
            break;
        }
    pthread_mutex_unlock(&z_lock);
}
static void z_in(ZStream* z) {
    uint32_t g = z->guest;
    z->host.next_in = rd32(g) ? (Bytef*)arm32_host(rd32(g)) : Z_NULL;
    z->host.avail_in = rd32(g + 4);
    z->host.next_out = rd32(g + 12) ? (Bytef*)arm32_host(rd32(g + 12)) : Z_NULL;
    z->host.avail_out = rd32(g + 16);
}
static void z_out(ZStream* z) {
    uint32_t g = z->guest;
    wr32(g, inside(z->host.next_in));
    wr32(g + 4, z->host.avail_in);
    wr32(g + 8, (uint32_t)z->host.total_in);
    wr32(g + 12, inside(z->host.next_out));
    wr32(g + 16, z->host.avail_out);
    wr32(g + 20, (uint32_t)z->host.total_out);
    wr32(g + 24, 0); /* msg: none */
    wr32(g + 28, g); /* state: not NULL while the stream is open */
    wr32(g + 44, (uint32_t)z->host.data_type);
    wr32(g + 48, (uint32_t)z->host.adler);
}
HANDLER(z_init) {
    /* inflateInit_(s, version, size), inflateInit2_(s, bits, version, size), deflateInit_(s, level, version, size),
       deflateInit2_(s, level, method, bits, memLevel, strategy, version, size) */
    unsigned kind = (unsigned)(uintptr_t)data;
    z_forget(R(0));
    ZStream* z = z_of(R(0), true);
    if (!z) {
        R(0) = (uint32_t)Z_MEM_ERROR;
        return;
    }
    z_in(z);
    int r;
    switch (kind) {
    case 0: r = inflateInit(&z->host); break;
    case 1: r = inflateInit2(&z->host, (int)R(1)); break;
    case 2: r = deflateInit(&z->host, (int)R(1)); break;
    default:
        r = deflateInit2(&z->host, (int)R(1), (int)R(2), (int)R(3), (int)arm32_word(c, 4), (int)arm32_word(c, 5));
        break;
    }
    if (r != Z_OK) z_forget(R(0));
    else z_out(z);
    R(0) = (uint32_t)r;
}
HANDLER(z_call) {
    /* 0 inflate, 1 deflate, 2 inflateEnd, 3 deflateEnd, 4 inflateReset, 5 deflateReset */
    unsigned which = (unsigned)(uintptr_t)data;
    ZStream* z = z_of(R(0), false);
    if (!z) {
        R(0) = (uint32_t)Z_STREAM_ERROR;
        return;
    }
    z_in(z);
    int r;
    switch (which) {
    case 0: r = inflate(&z->host, (int)R(1)); break;
    case 1: r = deflate(&z->host, (int)R(1)); break;
    case 2: r = inflateEnd(&z->host); break;
    case 3: r = deflateEnd(&z->host); break;
    case 4: r = inflateReset(&z->host); break;
    default: r = deflateReset(&z->host); break;
    }
    z_out(z);
    if (which == 2 || which == 3) {
        wr32(R(0) + 28, 0);
        z_forget(R(0));
    }
    R(0) = (uint32_t)r;
}
HANDLER(z_crc32) {
    UNUSED;
    R(0) = (uint32_t)crc32(R(0), R(1) ? (const Bytef*)P(1) : Z_NULL, R(2));
}
HANDLER(z_adler32) {
    UNUSED;
    R(0) = (uint32_t)adler32(R(0), R(1) ? (const Bytef*)P(1) : Z_NULL, R(2));
}
/* uncompress and compress(dest, &destLen, source, sourceLen) */
HANDLER(z_whole) {
    uLongf n = rd32(R(1));
    int r = data ? compress(P(0), &n, P(2), R(3)) : uncompress(P(0), &n, P(2), R(3));
    wr32(R(1), (uint32_t)n);
    R(0) = (uint32_t)r;
}

/* ---- SHA-1 (bionic's SHA1Init, SHA1Update and SHA1Final): the state, the count and the block, 92 bytes ---- */

static uint32_t rol(uint32_t v, unsigned n) { return v << n | v >> (32 - n); }
static void sha1_block(uint32_t* h, const uint8_t* b) {
    uint32_t w[80];
    for (unsigned i = 0; i < 16; ++i) w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 | (uint32_t)b[4 * i + 2] << 8 | b[4 * i + 3];
    for (unsigned i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], bb = h[1], cc = h[2], d = h[3], e = h[4];
    for (unsigned i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) f = (bb & cc) | (~bb & d), k = 0x5a827999;
        else if (i < 40) f = bb ^ cc ^ d, k = 0x6ed9eba1;
        else if (i < 60) f = (bb & cc) | (bb & d) | (cc & d), k = 0x8f1bbcdc;
        else f = bb ^ cc ^ d, k = 0xca62c1d6;
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d, d = cc, cc = rol(bb, 30), bb = a, a = t;
    }
    h[0] += a, h[1] += bb, h[2] += cc, h[3] += d, h[4] += e;
}
HANDLER(sha1_init) {
    UNUSED;
    static const uint32_t start[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    memset(P(0), 0, 92);
    for (unsigned i = 0; i < 5; ++i) wr32(R(0) + 4 * i, start[i]);
}
static void sha1_update(uint32_t ctx, const uint8_t* p, uint64_t n) {
    uint32_t h[5];
    for (unsigned i = 0; i < 5; ++i) h[i] = rd32(ctx + 4 * i);
    uint64_t count = rd32(ctx + 20) | (uint64_t)rd32(ctx + 24) << 32; /* bytes */
    uint8_t* block = arm32_mem + ctx + 28;
    for (uint64_t i = 0; i < n; ++i) {
        block[count % 64] = p[i];
        if (++count % 64 == 0) sha1_block(h, block);
    }
    for (unsigned i = 0; i < 5; ++i) wr32(ctx + 4 * i, h[i]);
    wr32(ctx + 20, (uint32_t)count);
    wr32(ctx + 24, (uint32_t)(count >> 32));
}
HANDLER(sha1_update_) {
    UNUSED;
    sha1_update(R(0), P(1), R(2));
}
/* SHA1Final(digest, context) */
HANDLER(sha1_final) {
    UNUSED;
    uint32_t ctx = R(1);
    uint64_t bits = (rd32(ctx + 20) | (uint64_t)rd32(ctx + 24) << 32) * 8;
    uint8_t pad = 0x80, zero = 0, length[8];
    sha1_update(ctx, &pad, 1);
    while ((rd32(ctx + 20) % 64) != 56) sha1_update(ctx, &zero, 1);
    for (unsigned i = 0; i < 8; ++i) length[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(ctx, length, 8);
    uint8_t* out = P(0);
    for (unsigned i = 0; i < 5; ++i) {
        uint32_t v = rd32(ctx + 4 * i);
        out[4 * i] = (uint8_t)(v >> 24), out[4 * i + 1] = (uint8_t)(v >> 16), out[4 * i + 2] = (uint8_t)(v >> 8),
                out[4 * i + 3] = (uint8_t)v;
    }
}

/* ---- files and the process, the host's calls with ARM's numbers made the host's (the 64-bit bridge has no handler
   for these) ---- */

/* a host call's result, -1 with the host's errno made the guest's */
static uint32_t result(long r) {
    if (r < 0) {
        arm32_set_errno(errno);
        return UINT32_MAX;
    }
    return (uint32_t)r;
}
/* Linux's AT_FDCWD (-100) as the host's */
static int at_fd(uint32_t fd) { return (int32_t)fd == -100 ? AT_FDCWD : (int)(int32_t)fd; }
/* Linux's AT_ flags as the host's: AT_SYMLINK_NOFOLLOW (0x100), and 0x200, which is AT_REMOVEDIR for unlinkat and
   AT_EACCESS for faccessat (host_0x200). False for a bit not in allowed */
static bool at_flags(uint32_t linux_flags, uint32_t allowed, int host_0x200, int* host) {
    if (linux_flags & ~allowed) return false;
    *host = (linux_flags & 0x100 ? AT_SYMLINK_NOFOLLOW : 0) | (linux_flags & 0x200 ? host_0x200 : 0);
    return true;
}
/* the guest's open (arm32_shim.c, the 64-bit bridge's, which answers /proc/self as Android does) with these words */
static uint32_t guest_open(uint32_t path, uint32_t flags, uint32_t mode) {
    const uint32_t words[3] = {path, flags, mode};
    return arm32_call(arm32_hle_symbol("open"), words, 3, NULL);
}
/* open64(path, flags, mode): open, as bionic's 32-bit open64 is */
HANDLER(f_open64) {
    UNUSED;
    R(0) = guest_open(R(0), R(1), R(2));
}
/* openat(dirfd, path, flags, mode): open for AT_FDCWD or an absolute path, else the host's openat */
HANDLER(f_openat) {
    UNUSED;
    const char* path = S(1);
    if ((int32_t)R(0) == -100 || (path && path[0] == '/')) {
        R(0) = guest_open(R(1), R(2), R(3));
        return;
    }
    R(0) = result(openat(at_fd(R(0)), path, gnu_open_flags_to_host(R(2)), (mode_t)R(3)));
}
/* creat(path, mode): open with O_WRONLY | O_CREAT | O_TRUNC */
HANDLER(f_creat) {
    UNUSED;
    R(0) = guest_open(R(0), 01101, R(1));
}
/* pwrite(fd, buf, count, offset), and pread64 and pwrite64 (data 1) with the offset of 64 bits at words 4 and 5 */
HANDLER(f_pwrite) {
    UNUSED;
    R(0) = result(pwrite((int)R(0), P(1), R(2), (off_t)(int32_t)R(3)));
}
HANDLER(f_pread_write64) {
    int64_t offset = (int64_t)(arm32_word(c, 4) | (uint64_t)arm32_word(c, 5) << 32);
    R(0) = result(data ? pwrite((int)R(0), P(1), R(2), (off_t)offset) : pread((int)R(0), P(1), R(2), (off_t)offset));
}
/* faccessat(dirfd, path, mode, flags): AT_EACCESS (0x200) and AT_SYMLINK_NOFOLLOW (0x100) */
HANDLER(f_faccessat) {
    UNUSED;
    int flags;
    R(0) = at_flags(R(3), 0x300, AT_EACCESS, &flags) ? result(faccessat(at_fd(R(0)), S(1), (int)R(2), flags))
                                                     : fail(L_EINVAL);
}
/* unlinkat(dirfd, path, flags): AT_REMOVEDIR (0x200) */
HANDLER(f_unlinkat) {
    UNUSED;
    int flags;
    R(0) = at_flags(R(2), 0x200, AT_REMOVEDIR, &flags) ? result(unlinkat(at_fd(R(0)), S(1), flags)) : fail(L_EINVAL);
}
HANDLER(f_fchdir) {
    UNUSED;
    R(0) = result(fchdir((int)R(0)));
}
/* chown(path, owner, group): -1 leaves one as it is */
HANDLER(f_chown) {
    UNUSED;
    R(0) = result(chown(S(0), (uid_t)R(1), (gid_t)R(2)));
}
HANDLER(f_fdatasync) {
    UNUSED;
#if defined(__APPLE__)
    R(0) = result(fsync((int)R(0)));
#else
    R(0) = result(fdatasync((int)R(0)));
#endif
}
/* ftruncate64(fd, length): the length of 64 bits in r2 and r3 */
HANDLER(f_ftruncate64) {
    UNUSED;
    R(0) = result(ftruncate((int)R(0), (off_t)(int64_t)(R(2) | (uint64_t)R(3) << 32)));
}
HANDLER(f_symlink) {
    UNUSED;
    R(0) = result(symlink(S(0), S(1)));
}
HANDLER(f_link) {
    UNUSED;
    R(0) = result(link(S(0), S(1)));
}
/* pipe2(fds, flags): O_CLOEXEC (02000000) and O_NONBLOCK (04000), set on each end; the two descriptors into fds */
HANDLER(f_pipe2) {
    UNUSED;
    uint32_t flags = R(1);
    if (flags & ~(02000000u | 04000u)) {
        R(0) = fail(L_EINVAL);
        return;
    }
    int fds[2];
    if (pipe(fds)) {
        R(0) = result(-1);
        return;
    }
    for (int i = 0; i < 2; ++i) {
        if (flags & 02000000u) fcntl(fds[i], F_SETFD, FD_CLOEXEC);
        if (flags & 04000u) fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK);
        wr32(R(0) + 4 * (uint32_t)i, (uint32_t)fds[i]);
    }
    R(0) = 0;
}
HANDLER(f_umask) {
    UNUSED;
    R(0) = (uint32_t)umask((mode_t)(R(0) & 0777));
}
/* mkstemp(template): the X's of the guest's buffer made a name, the file made */
HANDLER(f_mkstemp) {
    UNUSED;
    R(0) = result(mkstemp(P(0)));
}
HANDLER(f_getpgrp) {
    UNUSED;
    R(0) = (uint32_t)getpgrp();
}
HANDLER(f_setsid) {
    UNUSED;
    R(0) = result(setsid());
}
/* getpriority(which, who): the nice value, which may be -1, so errno tells a failure */
HANDLER(f_getpriority) {
    UNUSED;
    errno = 0;
    int r = getpriority((int)R(0), (id_t)R(1));
    if (r == -1 && errno) {
        R(0) = result(-1);
        return;
    }
    R(0) = (uint32_t)r;
}
/* eventfd(initial, flags): EFD_SEMAPHORE (1), EFD_NONBLOCK (04000) and EFD_CLOEXEC (02000000). macOS has none */
HANDLER(f_eventfd) {
    UNUSED;
    if (R(1) & ~(1u | 04000u | 02000000u)) {
        R(0) = fail(L_EINVAL);
        return;
    }
#if defined(__linux__) || defined(__FreeBSD__)
    int flags =
        (R(1) & 1 ? EFD_SEMAPHORE : 0) | (R(1) & 04000 ? EFD_NONBLOCK : 0) | (R(1) & 02000000 ? EFD_CLOEXEC : 0);
    R(0) = result(eventfd(R(0), flags));
#else
    static bool said;
    if (!said) fprintf(stderr, "scarletnx-android: arm32: eventfd fails with ENOSYS: this host has no eventfd\n");
    said = true;
    R(0) = fail(L_ENOSYS);
#endif
}

/* ---- the table and the objects ---- */

#define F(n, f) {n, f, NULL}
#define FD(n, f, d) {n, f, (void*)(uintptr_t)(d)}
const Arm32Symbol arm32_posix_symbols[] = {
    F("opendir", f_opendir),
    F("open64", f_open64),
    F("openat", f_openat),
    F("creat", f_creat),
    F("pwrite", f_pwrite),
    FD("pread64", f_pread_write64, 0),
    FD("pwrite64", f_pread_write64, 1),
    F("faccessat", f_faccessat),
    F("unlinkat", f_unlinkat),
    F("fchdir", f_fchdir),
    F("chown", f_chown),
    F("fdatasync", f_fdatasync),
    F("ftruncate64", f_ftruncate64),
    F("symlink", f_symlink),
    F("link", f_link),
    F("pipe2", f_pipe2),
    F("umask", f_umask),
    F("mkstemp", f_mkstemp),
    F("getpgrp", f_getpgrp),
    F("setsid", f_setsid),
    F("getpriority", f_getpriority),
    F("eventfd", f_eventfd),
    F("readdir", f_readdir),
    F("closedir", f_closedir),
    F("rewinddir", f_rewinddir),
    F("dirfd", f_dirfd),
    F("getcwd", f_getcwd),
    F("utime", f_utime),
    F("sendfile", f_sendfile),
    FD("readv", f_readv, 0),
    FD("writev", f_readv, 1),
    F("fcntl", f_fcntl),
    F("poll", f_poll),
    F("epoll_wait", f_epoll_wait),
    F("select", f_select),
    F("mprotect", f_mprotect),
    F("madvise", f_madvise),
    F("mremap", f_mremap),
    F("mktime", f_mktime),
    FD("localtime_r", f_localtime_r, 0),
    FD("gmtime_r", f_localtime_r, 1),
    F("strftime", f_strftime),
    F("clock", f_clock),
    F("clock_getres", f_clock_getres),
    F("getrusage", f_getrusage),
    F("_exit", f__exit),
    F("fork", f_enosys),
    F("vfork", f_enosys),
    F("execl", f_enosys),
    F("execlp", f_enosys),
    F("execv", f_enosys),
    F("execvp", f_enosys),
    F("execve", f_enosys),
    F("system", f_enosys),
    F("waitpid", f_enosys),
    F("popen", f_null_enosys),
    F("pclose", f_enosys),
    F("ptrace", f_eperm),
    F("prctl", f_zero),
    F("setitimer", f_zero),
    F("setresuid", f_zero),
    F("setresgid", f_zero),
    F("getresuid", f_getresuid),
    F("getresgid", f_getresuid),
    F("getdtablesize", f_getdtablesize),
    F("getpwuid", f_getpw),
    F("getpwnam", f_getpw),
    F("getgrgid", f_null_enosys),
    F("getgrnam", f_null_enosys),
    F("getenv", f_getenv),
    F("setenv", f_setenv),
    F("unsetenv", f_unsetenv),
    F("setlocale", f_setlocale),
    F("memmem", f_memmem),
    F("strcasestr", f_strcasestr),
    F("strpbrk", f_strpbrk),
    F("strlcpy", f_strlcpy),
    F("strlcat", f_strlcat),
    F("strnlen", f_strnlen),
    F("strndup", f_strndup),
    F("strsep", f_strsep),
    F("strtok", f_strtok),
    F("strtok_r", f_strtok_r),
    F("strtoul", f_strtoul),
    FD("strtoll", f_strtoll, 0),
    FD("strtoull", f_strtoll, 1),
    FD("strtoimax", f_strtoll, 0),
    FD("strtoumax", f_strtoll, 1),
    F("strtof", f_strtof),
    F("strcoll", f_strcoll),
    F("strxfrm", f_strxfrm),
    F("strerror", f_strerror),
    F("strerror_r", f_strerror_r),
    F("qsort", f_qsort),
    F("bsearch", f_bsearch),
    F("div", f_div),
    F("ldiv", f_div),
    F("isalnum", f_isalnum),
    F("isalpha", f_isalpha),
    F("isdigit", f_isdigit),
    F("isxdigit", f_isxdigit),
    F("isspace", f_isspace),
    F("isupper", f_isupper),
    F("islower", f_islower),
    F("ispunct", f_ispunct),
    F("isprint", f_isprint),
    F("isgraph", f_isgraph),
    F("iscntrl", f_iscntrl),
    F("wcslen", f_wcslen),
    F("wcscmp", f_wcscmp),
    F("wcsncmp", f_wcsncmp),
    F("wcscoll", f_wcscmp),
    F("wcscpy", f_wcscpy),
    F("wmemchr", f_wmemchr),
    F("wmemcmp", f_wmemcmp),
    F("wmemcpy", f_wmemcpy),
    F("wmemmove", f_wmemcpy),
    F("wmemset", f_wmemset),
    F("towlower", f_towlower),
    F("towupper", f_towupper),
    F("mbrtowc", f_mbrtowc),
    F("wcrtomb", f_wcrtomb),
    F("wcsxfrm", f_wcsxfrm),
    F("wcsftime", f_wcsftime),
    F("getwc", f_getwc),
    F("putwc", f_putwc),
    F("ungetwc", f_ungetwc),
    F("cosh", m_cosh),
    F("coshf", m_coshf),
    F("sinh", m_sinh),
    F("sinhf", m_sinhf),
    F("tanh", m_tanh),
    F("tanhf", m_tanhf),
    F("rint", m_rint),
    F("rintf", m_rintf),
    F("trunc", m_trunc),
    F("truncf", m_truncf),
    F("exp2", m_exp2),
    F("exp2f", m_exp2f),
    F("log2", m_log2),
    F("log2f", m_log2f),
    F("cbrt", m_cbrt),
    F("cbrtf", m_cbrtf),
    F("frexp", m_frexp),
    F("ldexp", m_ldexp),
    F("ldexpf", m_ldexpf),
    F("modf", m_modf),
    F("modff", m_modff),
    F("isnan", m_isnan),
    F("isinf", m_isinf),
    F("__isnan", m_isnan),
    F("__isinf", m_isinf),
    F("__fpclassifyf", m_fpclassifyf),
    F("__fpclassifyd", m_fpclassifyd),
    F("__fpclassify", m_fpclassifyd),
    F("inet_addr", n_inet_addr),
    F("inet_aton", n_inet_aton),
    F("inet_ntoa", n_inet_ntoa),
    F("inet_ntop", n_inet_ntop),
    F("inet_pton", n_inet_pton),
    F("getaddrinfo", n_getaddrinfo),
    F("freeaddrinfo", n_freeaddrinfo),
    F("gai_strerror", n_gai_strerror),
    F("__get_h_errno", n_get_h_errno),
    F("gethostbyname", n_gethostbyname),
    F("gethostbyaddr", n_gethostbyname),
    F("getsockopt", n_getsockopt),
    F("setsockopt", n_setsockopt),
    F("sendmsg", f_enosys),
    F("recvmsg", f_enosys),
    FD("inflateInit_", z_init, 0),
    FD("inflateInit2_", z_init, 1),
    FD("deflateInit_", z_init, 2),
    FD("deflateInit2_", z_init, 3),
    FD("inflate", z_call, 0),
    FD("deflate", z_call, 1),
    FD("inflateEnd", z_call, 2),
    FD("deflateEnd", z_call, 3),
    FD("inflateReset", z_call, 4),
    FD("deflateReset", z_call, 5),
    F("crc32", z_crc32),
    F("adler32", z_adler32),
    FD("uncompress", z_whole, 0),
    FD("compress", z_whole, 1),
    F("SHA1Init", sha1_init),
    F("SHA1Update", sha1_update_),
    F("SHA1Final", sha1_final),
    {"", NULL, NULL},
};

static uint32_t page_size_word, nothrow_word, data_start_word;
uint32_t arm32_posix_object(const char* name) {
    if (!strcmp(name, "environ")) return environ_word;
    if (!strcmp(name, "_ctype_")) return ctype_word;
    if (!strcmp(name, "_tolower_tab_")) return tolower_word;
    if (!strcmp(name, "_toupper_tab_")) return toupper_word;
    if (!strcmp(name, "__page_size") || !strcmp(name, "PAGE_SIZE")) return page_size_word;
    if (!strcmp(name, "_ZSt7nothrow")) return nothrow_word;
    if (!strcmp(name, "__data_start") || !strcmp(name, "data_start")) return data_start_word;
    return 0;
}

void arm32_posix_init(void) {
    environ_word = arm32_malloc(4);
    static const char* const start[] = {"PATH=/system/bin:/system/xbin", "ANDROID_ROOT=/system",
                                        "ANDROID_DATA=/data", "EXTERNAL_STORAGE=/sdcard", "TMPDIR=/data/local/tmp"};
    for (size_t i = 0; i < sizeof start / sizeof *start; ++i) {
        env = realloc(env, (env_count + 1) * sizeof *env);
        env[env_count++] = strdup(start[i]);
    }
    env_publish();
    make_ctype();
    page_size_word = arm32_malloc(4);
    wr32(page_size_word, 4096);
    nothrow_word = arm32_malloc(4);
    data_start_word = arm32_malloc(16);
    memset(arm32_mem + data_start_word, 0, 16);
    h_errno_word = arm32_malloc(4);
}
