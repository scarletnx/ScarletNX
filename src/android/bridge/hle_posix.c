/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* bionic's POSIX side as HLE: files and FILE, memory maps, sockets, time, the process, signals and system calls.
   Where bionic's AArch64 types or constants differ from the host's (struct stat, the open flags, sysconf's names,
   epoll_event, addrinfo, sigset_t, the system call numbers), the adapter converts them. The rest is PASS */
#define _GNU_SOURCE
#include "android/bridge/bridge.h"
#include "android/bridge/hle.h"
#include "android/bridge/hle_impl.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <malloc.h>
#include <syslog.h>
#include <wchar.h>
#include <net/if.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pwd.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

typedef uint64_t (*Pass8)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static void fail_errno(CPU* c, int error) {
    errno = error;
    X(0) = (uint64_t)-1;
}

/* ---- FILE: bionic's __sF[3] (152 bytes each on LP64) stand for stdin, stdout and stderr ---- */

enum { BIONIC_FILE_SIZE = 152 };
static unsigned char bionic_sF[3 * BIONIC_FILE_SIZE];
static uint64_t guest_stdin = (uint64_t)(uintptr_t)&bionic_sF[0];
static uint64_t guest_stdout = (uint64_t)(uintptr_t)&bionic_sF[BIONIC_FILE_SIZE];
static uint64_t guest_stderr = (uint64_t)(uintptr_t)&bionic_sF[2 * BIONIC_FILE_SIZE];

FILE* hle_file(uint64_t guest) {
    uint64_t base = (uint64_t)(uintptr_t)bionic_sF;
    if (guest >= base && guest < base + sizeof bionic_sF) {
        uint64_t i = (guest - base) / BIONIC_FILE_SIZE;
        return i == 0 ? stdin : i == 1 ? stdout : stderr;
    }
    return (FILE*)(uintptr_t)guest;
}

/* a host function with a FILE* among its arguments */
typedef struct {
    void* fn;
    unsigned file;
} FileCall;
static void file_pass(CPU* c, void* data) {
    const FileCall* f = data;
    uint64_t a[8];
    memcpy(a, c->x, sizeof a);
    a[f->file] = (uint64_t)(uintptr_t)hle_file(a[f->file]);
    X(0) = ((Pass8)f->fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
}
#define FILE_CALLS(F)                                                                                                  \
    F(fclose, 0) F(feof, 0) F(ferror, 0) F(clearerr, 0) F(fflush, 0) F(fgets, 2) F(fileno, 0) F(fputc, 1) F(fputs, 1)  \
    F(fputwc, 1) F(fread, 3) F(fwrite, 3) F(fseek, 0) F(fseeko, 0) F(ftell, 0) F(ftello, 0) F(getc, 0) F(getwc, 0)     \
    F(ungetc, 1) F(ungetwc, 1) F(rewind, 0) F(setbuf, 0) F(setvbuf, 0) F(freopen, 2) F(pclose, 0)
#define FILE_CALL_OBJECT(name, k) static const FileCall fc_##name = {(void*)name, k};
FILE_CALLS(FILE_CALL_OBJECT)
#define FILE_CALL_ENTRY(name, k) {#name, file_pass, (void*)&fc_##name},

static void g_putchar(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(int64_t)putchar((int)X(0));
}
static void g_puts(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(int64_t)puts(STR(0));
}
static void g_perror(CPU* c, void* data) {
    (void)data;
    perror(STR(0));
}
/* a process the app would start: none, as an app's sandbox mostly refuses */
static void g_popen(CPU* c, void* data) {
    (void)data;
    errno = ENOSYS;
    X(0) = 0;
}

/* ---- the open flags: those whose AArch64 values differ from x86-64's ---- */

static int open_flags(uint64_t guest) {
#if defined(__x86_64__)
    int flags = (int)guest & ~(0x4000 | 0x8000 | 0x10000 | 0x20000);
    if (guest & 0x4000) flags |= O_DIRECTORY;
    if (guest & 0x8000) flags |= O_NOFOLLOW;
    if (guest & 0x10000) flags |= O_DIRECT;
    return flags; /* O_LARGEFILE (0x20000) is implied on a 64-bit host */
#else
    return (int)guest;
#endif
}
static uint64_t guest_flags(int host) {
#if defined(__x86_64__)
    uint64_t flags = (uint64_t)(unsigned)host & ~(uint64_t)(O_DIRECTORY | O_NOFOLLOW | O_DIRECT | 0x8000);
    if (host & O_DIRECTORY) flags |= 0x4000;
    if (host & O_NOFOLLOW) flags |= 0x8000;
    if (host & O_DIRECT) flags |= 0x10000;
    return flags | 0x20000;
#else
    return (uint64_t)(unsigned)host;
#endif
}
static void g_open(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(int64_t)open(STR(0), open_flags(X(1)), (mode_t)X(2));
}
static void g_open_2(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(int64_t)open(STR(0), open_flags(X(1)));
}
static void g_fcntl(CPU* c, void* data) {
    (void)data;
    int fd = (int)X(0), cmd = (int)X(1);
    if (cmd == F_GETFL) {
        int r = fcntl(fd, F_GETFL);
        X(0) = r < 0 ? (uint64_t)-1 : guest_flags(r);
    } else if (cmd == F_SETFL) {
        X(0) = (uint64_t)(int64_t)fcntl(fd, F_SETFL, open_flags(X(2)));
    } else {
        X(0) = (uint64_t)(int64_t)fcntl(fd, cmd, X(2));
    }
}
static void g_ioctl(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(int64_t)ioctl((int)X(0), (unsigned long)X(1), X(2));
}

/* ---- struct stat: AArch64's (asm-generic), 128 bytes ---- */

static void put_stat(uint64_t guest, const struct stat* s) {
    unsigned char out[128] = {0};
    uint64_t q;
    uint32_t w;
    int32_t i;
#define PUT64(off, v) (q = (uint64_t)(v), memcpy(out + (off), &q, 8))
#define PUT32(off, v) (w = (uint32_t)(v), memcpy(out + (off), &w, 4))
    PUT64(0, s->st_dev);
    PUT64(8, s->st_ino);
    PUT32(16, s->st_mode);
    PUT32(20, s->st_nlink);
    PUT32(24, s->st_uid);
    PUT32(28, s->st_gid);
    PUT64(32, s->st_rdev);
    PUT64(48, s->st_size);
    i = (int32_t)s->st_blksize;
    memcpy(out + 56, &i, 4);
    PUT64(64, s->st_blocks);
    PUT64(72, s->st_atim.tv_sec);
    PUT64(80, s->st_atim.tv_nsec);
    PUT64(88, s->st_mtim.tv_sec);
    PUT64(96, s->st_mtim.tv_nsec);
    PUT64(104, s->st_ctim.tv_sec);
    PUT64(112, s->st_ctim.tv_nsec);
#undef PUT64
#undef PUT32
    memcpy((void*)(uintptr_t)guest, out, sizeof out);
}
static void g_stat(CPU* c, void* data) {
    (void)data;
    struct stat s;
    int r = stat(STR(0), &s);
    if (!r) put_stat(X(1), &s);
    X(0) = (uint64_t)(int64_t)r;
}
static void g_lstat(CPU* c, void* data) {
    (void)data;
    struct stat s;
    int r = lstat(STR(0), &s);
    if (!r) put_stat(X(1), &s);
    X(0) = (uint64_t)(int64_t)r;
}
static void g_fstat(CPU* c, void* data) {
    (void)data;
    struct stat s;
    int r = fstat((int)X(0), &s);
    if (!r) put_stat(X(1), &s);
    X(0) = (uint64_t)(int64_t)r;
}

/* ---- memory maps: executable ones go into the code window, where the JIT finds them ---- */

static void g_mmap(CPU* c, void* data) {
    (void)data;
    int prot = (int)X(2), flags = (int)X(3);
    void* hint = PTR(0);
    if ((prot & PROT_EXEC) && !(flags & MAP_FIXED)) {
        uint64_t at = bridge_window_reserve((X(1) + 0xffff) & ~(uint64_t)0xffff, 0x10000);
        if (!at) {
            X(0) = (uint64_t)(uintptr_t)MAP_FAILED;
            errno = ENOMEM;
            return;
        }
        hint = (void*)(uintptr_t)at;
        flags |= MAP_FIXED;
    }
    /* the guest's code is never run as it is: its pages need reading, not executing */
    void* r = mmap(hint, X(1), (prot & ~PROT_EXEC) | (prot & PROT_EXEC ? PROT_READ : 0), flags, (int)X(4), (off_t)X(5));
    if (r != MAP_FAILED && (prot & PROT_EXEC)) bridge_add_code((uint64_t)(uintptr_t)r, X(1) & ~(uint64_t)3);
    X(0) = (uint64_t)(uintptr_t)r;
}
static void g_mprotect(CPU* c, void* data) {
    (void)data;
    int prot = (int)X(2);
    if (prot & PROT_EXEC) bridge_add_code(X(0), X(1) & ~(uint64_t)3);
    X(0) = (uint64_t)(int64_t)mprotect(PTR(0), X(1), (prot & ~PROT_EXEC) | (prot & PROT_EXEC ? PROT_READ : 0));
}

/* mallinfo: bionic's has size_t fields, as glibc's mallinfo2, and comes back through x8, AArch64's register for a
   large result's address */
static void g_mallinfo(CPU* c, void* data) {
    (void)data;
    struct mallinfo2 m = mallinfo2();
    memcpy((void*)(uintptr_t)X(8), &m, sizeof m);
}

/* ---- epoll_event: 12 bytes, packed, on x86-64; 16 on AArch64 ---- */

static void g_epoll_ctl(CPU* c, void* data) {
    (void)data;
    struct epoll_event e = {0};
    if (X(3)) {
        uint32_t events;
        uint64_t d;
        memcpy(&events, PTR(3), 4);
        memcpy(&d, (unsigned char*)PTR(3) + 8, 8);
        e.events = events;
        e.data.u64 = d;
    }
    X(0) = (uint64_t)(int64_t)epoll_ctl((int)X(0), (int)X(1), (int)X(2), X(3) ? &e : NULL);
}
static void g_epoll_wait(CPU* c, void* data) {
    (void)data;
    int most = (int)X(2);
    if (most <= 0 || most > 4096) {
        fail_errno(c, EINVAL);
        return;
    }
    struct epoll_event events[4096];
    int n = epoll_wait((int)X(0), events, most, (int)X(3));
    for (int i = 0; i < n; ++i) {
        unsigned char* out = (unsigned char*)PTR(1) + 16 * i;
        uint32_t ev = events[i].events;
        uint64_t d = events[i].data.u64;
        memset(out, 0, 16);
        memcpy(out, &ev, 4);
        memcpy(out + 8, &d, 8);
    }
    X(0) = (uint64_t)(int64_t)n;
}

/* ---- addrinfo: bionic's has ai_canonname before ai_addr, glibc's after ---- */

typedef struct GuestAddrinfo {
    int32_t flags, family, socktype, protocol;
    uint32_t addrlen;
    char* canonname;
    struct sockaddr* addr;
    struct GuestAddrinfo* next;
} GuestAddrinfo;

static GuestAddrinfo* to_guest(const struct addrinfo* a) {
    if (!a) return NULL;
    GuestAddrinfo* g = calloc(1, sizeof *g + a->ai_addrlen);
    if (!g) return NULL;
    g->flags = a->ai_flags;
    g->family = a->ai_family;
    g->socktype = a->ai_socktype;
    g->protocol = a->ai_protocol;
    g->addrlen = a->ai_addrlen;
    g->canonname = a->ai_canonname ? strdup(a->ai_canonname) : NULL;
    g->addr = a->ai_addr ? (struct sockaddr*)(g + 1) : NULL;
    if (a->ai_addr) memcpy(g + 1, a->ai_addr, a->ai_addrlen);
    g->next = to_guest(a->ai_next);
    return g;
}
static void g_getaddrinfo(CPU* c, void* data) {
    (void)data;
    struct addrinfo hints = {0}, *result = NULL;
    const GuestAddrinfo* h = PTR(2);
    if (h) {
        hints.ai_flags = h->flags;
        hints.ai_family = h->family;
        hints.ai_socktype = h->socktype;
        hints.ai_protocol = h->protocol;
    }
    int r = getaddrinfo(STR(0), STR(1), h ? &hints : NULL, &result);
    GuestAddrinfo* out = r ? NULL : to_guest(result);
    if (result) freeaddrinfo(result);
    memcpy(PTR(3), &out, sizeof out);
    X(0) = (uint64_t)(int64_t)r;
}
static void g_freeaddrinfo(CPU* c, void* data) {
    (void)data;
    for (GuestAddrinfo* g = PTR(0); g;) {
        GuestAddrinfo* next = g->next;
        free(g->canonname);
        free(g);
        g = next;
    }
}

/* ---- sysconf: bionic's names ---- */

static void g_sysconf(CPU* c, void* data) {
    (void)data;
    long r;
    switch (X(0)) {
    case 0x00: r = sysconf(_SC_ARG_MAX); break;
    case 0x05: r = sysconf(_SC_CHILD_MAX); break;
    case 0x06: r = sysconf(_SC_CLK_TCK); break;
    case 0x0b: r = sysconf(_SC_OPEN_MAX); break;
    case 0x27:
    case 0x28: r = 4096; break; /* _SC_PAGESIZE, _SC_PAGE_SIZE: the guest's pages */
    case 0x60: r = sysconf(_SC_NPROCESSORS_CONF); break;
    case 0x61: r = sysconf(_SC_NPROCESSORS_ONLN); break;
    case 0x62: r = sysconf(_SC_PHYS_PAGES) * (sysconf(_SC_PAGESIZE) / 4096); break;
    case 0x63: r = sysconf(_SC_AVPHYS_PAGES) * (sysconf(_SC_PAGESIZE) / 4096); break;
    case 0x64: r = 1; break; /* _SC_MONOTONIC_CLOCK */
    default:
        fprintf(stderr, "scarletnx-android: sysconf(0x%llx): unknown, -1\n", (unsigned long long)X(0));
        errno = EINVAL;
        r = -1;
        break;
    }
    X(0) = (uint64_t)(int64_t)r;
}
static void g_getpagesize(CPU* c, void* data) {
    (void)data;
    X(0) = 4096;
}

/* getauxval: an AArch64 CPU of Armv8.2 with the features the recompiler runs */
static void g_getauxval(CPU* c, void* data) {
    (void)data;
    enum { AT_PAGESZ_ = 6, AT_HWCAP_ = 16, AT_HWCAP2_ = 26 };
    switch (X(0)) {
    case AT_PAGESZ_: X(0) = 4096; break;
    case AT_HWCAP_:
        /* FP, ASIMD, AES, PMULL, SHA1, SHA2, CRC32, ATOMICS, FPHP, ASIMDHP, ASIMDRDM, LRCPC, ASIMDDP: the
           recompiler's android target (a64.profile) */
        X(0) = 1u << 0 | 1u << 1 | 1u << 3 | 1u << 4 | 1u << 5 | 1u << 6 | 1u << 7 | 1u << 8 | 1u << 9 | 1u << 10 |
               1u << 12 | 1u << 15 | 1u << 20;
        break;
    case AT_HWCAP2_: X(0) = 0; break;
    default: X(0) = 0; errno = ENOENT; break;
    }
}

/* uname: the host's, as an AArch64 machine */
static void g_uname(CPU* c, void* data) {
    (void)data;
    struct utsname* u = PTR(0);
    int r = uname(u);
    if (!r) snprintf(u->machine, sizeof u->machine, "aarch64");
    X(0) = (uint64_t)(int64_t)r;
}

static void g_arc4random(CPU* c, void* data) {
    (void)data;
    uint32_t v = 0;
    if (getrandom(&v, sizeof v, 0) != sizeof v) v = (uint32_t)random();
    X(0) = v;
}
static void g_arc4random_buf(CPU* c, void* data) {
    (void)data;
    for (size_t done = 0; done < X(1);) {
        ssize_t n = getrandom((unsigned char*)PTR(0) + done, X(1) - done, 0);
        if (n <= 0) break;
        done += (size_t)n;
    }
}

/* ---- what an app may not do in its sandbox: fork, exec, ptrace ---- */
static void refused(CPU* c, void* data) {
    (void)data;
    fail_errno(c, ENOSYS);
}
static void g_ptrace(CPU* c, void* data) {
    (void)data;
    fail_errno(c, EPERM);
}

/* ---- signals: bionic's sigset_t on LP64 is one unsigned long, signals 1 to 64 ---- */

static void g_sigemptyset(CPU* c, void* data) {
    (void)data;
    memset(PTR(0), 0, 8);
    X(0) = 0;
}
static void g_sigfillset(CPU* c, void* data) {
    (void)data;
    memset(PTR(0), 0xff, 8);
    X(0) = 0;
}
static void set_bit(CPU* c, bool on) {
    int sig = (int)X(1);
    if (sig < 1 || sig > 64) {
        fail_errno(c, EINVAL);
        return;
    }
    uint64_t set;
    memcpy(&set, PTR(0), 8);
    set = on ? set | (1ull << (sig - 1)) : set & ~(1ull << (sig - 1));
    memcpy(PTR(0), &set, 8);
    X(0) = 0;
}
static void g_sigaddset(CPU* c, void* data) {
    (void)data;
    set_bit(c, true);
}
static void g_sigdelset(CPU* c, void* data) {
    (void)data;
    set_bit(c, false);
}
static void host_set(uint64_t guest, sigset_t* out) {
    sigemptyset(out);
    uint64_t set;
    memcpy(&set, (const void*)(uintptr_t)guest, 8);
    for (int s = 1; s <= 64; ++s)
        if ((set >> (s - 1)) & 1) sigaddset(out, s);
}
static void g_pthread_sigmask(CPU* c, void* data) {
    (void)data;
    sigset_t set, old;
    if (X(1)) host_set(X(1), &set);
    int r = pthread_sigmask((int)X(0), X(1) ? &set : NULL, &old);
    if (!r && X(2)) {
        uint64_t g = 0;
        for (int s = 1; s <= 64; ++s)
            if (sigismember(&old, s) == 1) g |= 1ull << (s - 1);
        memcpy(PTR(2), &g, 8);
    }
    X(0) = (uint64_t)(int64_t)r;
}
static void g_signalfd(CPU* c, void* data) {
    (void)data;
    sigset_t set;
    host_set(X(1), &set);
    X(0) = (uint64_t)(int64_t)signalfd((int)X(0), &set, (int)X(3));
}

/* The guest's signal handlers, recorded: the bridge does not deliver signals to the guest yet. bionic's struct
   sigaction on AArch64: the handler, sa_mask (8 bytes), sa_flags (int), sa_restorer */
static uint64_t handlers[65][4];
static void g_sigaction(CPU* c, void* data) {
    (void)data;
    int sig = (int)X(0);
    if (sig < 1 || sig > 64) {
        fail_errno(c, EINVAL);
        return;
    }
    if (X(2)) memcpy(PTR(2), handlers[sig], 32);
    if (X(1)) memcpy(handlers[sig], PTR(1), 32);
    X(0) = 0;
}
static void g_signal(CPU* c, void* data) {
    (void)data;
    int sig = (int)X(0);
    if (sig < 1 || sig > 64) {
        X(0) = (uint64_t)-1; /* SIG_ERR */
        errno = EINVAL;
        return;
    }
    uint64_t old = handlers[sig][0];
    handlers[sig][0] = X(1);
    X(0) = old;
}

/* ---- syscall(): AArch64's numbers ---- */

static void g_syscall(CPU* c, void* data) {
    (void)data;
    long r;
    switch (X(0)) {
    case 98: r = syscall(SYS_futex, X(1), X(2), X(3), X(4), X(5), X(6)); break;
    case 113: r = syscall(SYS_clock_gettime, X(1), X(2)); break;
    case 123: r = syscall(SYS_sched_getaffinity, X(1), X(2), X(3)); break;
    case 131: r = syscall(SYS_tgkill, X(1), X(2), X(3)); break;
    case 167: r = syscall(SYS_prctl, X(1), X(2), X(3), X(4), X(5)); break;
    case 168: r = syscall(SYS_getcpu, X(1), X(2), X(3)); break;
    case 172: r = getpid(); break;
    case 178: r = gettid(); break;
    case 233: r = syscall(SYS_madvise, X(1), X(2), X(3)); break;
    case 278: r = syscall(SYS_getrandom, X(1), X(2), X(3)); break;
    case 283: r = syscall(SYS_membarrier, X(1), X(2), X(3)); break;
    default:
        fprintf(stderr, "scarletnx-android: syscall(%llu): not answered, ENOSYS\n", (unsigned long long)X(0));
        errno = ENOSYS;
        r = -1;
        break;
    }
    X(0) = (uint64_t)(int64_t)r;
}

#define P(name) {#name, hle_pass, (void*)name}

const HleSymbol hle_posix[] = {
    {"__sF", NULL, bionic_sF},
    {"stdin", NULL, &guest_stdin},
    {"stdout", NULL, &guest_stdout},
    {"stderr", NULL, &guest_stderr},
    {"environ", NULL, &environ},
    FILE_CALLS(FILE_CALL_ENTRY)
    P(fopen), P(fdopen), P(tmpfile), P(remove), P(rename),
    {"tmpnam", refused, NULL},
    {"putchar", g_putchar, NULL},
    {"puts", g_puts, NULL},
    {"perror", g_perror, NULL},
    {"popen", g_popen, NULL},
    {"open", g_open, NULL},
    {"__open_2", g_open_2, NULL},
    {"fcntl", g_fcntl, NULL},
    {"ioctl", g_ioctl, NULL},
    {"stat", g_stat, NULL},
    {"lstat", g_lstat, NULL},
    {"fstat", g_fstat, NULL},
    {"fstat64", g_fstat, NULL},
    P(statfs), P(access), P(chmod), P(fchmod), P(close), P(dup), P(dup2), P(flock), P(fsync), P(ftruncate),
    P(lseek), P(lseek64), P(mkdir), P(pipe), P(pread), P(read), P(readlink), P(realpath), P(rmdir), P(truncate),
    P(unlink), P(utime), P(write), P(isatty), P(getcwd), P(opendir), P(readdir), P(closedir),
    P(inotify_init1), P(inotify_add_watch),
    {"mmap", g_mmap, NULL},
    {"mprotect", g_mprotect, NULL},
    P(munmap), P(madvise), P(msync), P(mincore),
    {"mallinfo", g_mallinfo, NULL},
    P(epoll_create),
    {"epoll_ctl", g_epoll_ctl, NULL},
    {"epoll_wait", g_epoll_wait, NULL},
    P(poll), P(select),
    P(socket), P(socketpair), P(bind), P(connect), P(listen), P(accept), P(shutdown), P(send), P(sendto),
    P(sendmsg), P(recv), P(recvfrom), P(recvmsg), P(getsockname), P(getpeername), P(getsockopt), P(setsockopt),
    {"getaddrinfo", g_getaddrinfo, NULL},
    {"freeaddrinfo", g_freeaddrinfo, NULL},
    P(getnameinfo), P(gethostbyname), P(gethostname), P(inet_addr), P(inet_aton), P(inet_ntoa), P(inet_ntop),
    P(inet_pton), P(if_nametoindex),
    P(clock), P(clock_getres), P(clock_gettime), P(gettimeofday), P(time), P(nanosleep), P(usleep), P(sleep),
    P(gmtime), P(gmtime_r), P(localtime), P(localtime_r), P(mktime), P(strftime), P(strftime_l), P(tzset), P(alarm),
    P(getpid), P(getppid), P(gettid), P(getuid), P(geteuid), P(getrlimit), P(getrusage), P(sysinfo), P(getenv),
    P(setenv), P(getpwuid), P(getpwuid_r), P(prctl), P(setpriority), P(sched_yield), P(sched_get_priority_max),
    P(sched_get_priority_min), P(getentropy), P(openlog), P(closelog),
    P(sem_init), P(sem_destroy), P(sem_post), P(sem_wait), P(sem_timedwait),
    {"sysconf", g_sysconf, NULL},
    {"getpagesize", g_getpagesize, NULL},
    {"getauxval", g_getauxval, NULL},
    {"uname", g_uname, NULL},
    {"arc4random", g_arc4random, NULL},
    {"arc4random_buf", g_arc4random_buf, NULL},
    {"fork", refused, NULL},
    {"execl", refused, NULL},
    {"system", refused, NULL},
    {"waitpid", refused, NULL},
    {"ptrace", g_ptrace, NULL},
    {"sigemptyset", g_sigemptyset, NULL},
    {"sigfillset", g_sigfillset, NULL},
    {"sigaddset", g_sigaddset, NULL},
    {"sigdelset", g_sigdelset, NULL},
    {"pthread_sigmask", g_pthread_sigmask, NULL},
    {"sigprocmask", g_pthread_sigmask, NULL},
    {"signalfd", g_signalfd, NULL},
    {"sigaction", g_sigaction, NULL},
    {"signal", g_signal, NULL},
    {"syscall", g_syscall, NULL},
    {"tzname", NULL, &tzname},
    {"timezone", NULL, &timezone},
    {"daylight", NULL, &daylight},
    {"optarg", NULL, &optarg},
    {"optind", NULL, &optind},
    {"opterr", NULL, &opterr},
    {"optopt", NULL, &optopt},
    P(getopt), P(getopt_long),
    {"", NULL, NULL},
};
