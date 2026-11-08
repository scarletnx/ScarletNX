/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The host system under the native bridge (host.h). Each call has a branch for each host: Linux, FreeBSD, macOS and
   Windows x64, written from each system's documented API (Linux's man-pages, FreeBSD's manual, Apple's
   documentation of the Mach and BSD calls, Microsoft's Win32 documentation) */
#if defined(__linux__)
#define _GNU_SOURCE /* MAP_FIXED_NOREPLACE, process_vm_readv, gettid */
#endif
#include "android/bridge/host.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <pthread.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/epoll.h>
#include <sys/uio.h>
#elif defined(__FreeBSD__) || defined(__APPLE__)
#include <sys/event.h>
#include <sys/time.h>
#endif
#if defined(__FreeBSD__)
#include <pthread_np.h>
#include <sys/thr.h>
#endif
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif
#endif

/* Linux's epoll numbers, which bionic's are */
enum {
    L_EPOLL_CTL_ADD = 1,
    L_EPOLL_CTL_DEL = 2,
    L_EPOLL_CTL_MOD = 3,
    L_EPOLLIN = 0x1,
    L_EPOLLPRI = 0x2,
    L_EPOLLOUT = 0x4,
    L_EPOLLERR = 0x8,
    L_EPOLLHUP = 0x10,
    L_EPOLLRDHUP = 0x2000,
};
#define L_EPOLLONESHOT (1u << 30)
#define L_EPOLLET      (1u << 31)

#ifdef _WIN32
/* an epoll set: a CRT descriptor of NUL that the guest closes, and the sockets it watches */
typedef struct {
    int fd; /* -1 for a free slot */
    size_t count, capacity;
    struct {
        int fd;
        uint32_t events;
        uint64_t data;
    } * items;
} WinSet;
#endif

#if defined(__FreeBSD__) || defined(_WIN32)
static struct {
#if defined(__FreeBSD__)
    /* host_copy_checked's pipe: write(2) of a bad address fails with EFAULT */
    pthread_mutex_t copy_lock;
    int copy_pipe[2];
    bool copy_ready;
#else
    pthread_mutex_t sets_lock; /* the epoll sets */
    WinSet* sets;
    size_t set_count;
#endif
} g_host = {
#if defined(__FreeBSD__)
    .copy_lock = PTHREAD_MUTEX_INITIALIZER,
#else
    .sets_lock = PTHREAD_MUTEX_INITIALIZER,
#endif
};
#endif

/* ---- ids ---- */

uint32_t host_tid(void) {
#if defined(__linux__)
    return (uint32_t)gettid();
#elif defined(__FreeBSD__)
    return (uint32_t)pthread_getthreadid_np();
#elif defined(__APPLE__)
    uint64_t id = 0;
    pthread_threadid_np(NULL, &id);
    return (uint32_t)id; /* a 64-bit id, unique in the system, whose low half is the guest's tid */
#elif defined(_WIN32)
    return (uint32_t)GetCurrentThreadId();
#endif
}

uint32_t host_pid(void) {
#ifdef _WIN32
    return (uint32_t)GetCurrentProcessId();
#else
    return (uint32_t)getpid();
#endif
}

/* ---- address space ---- */

void* host_reserve(uint64_t address, size_t size, bool exact) {
    void* want = (void*)(uintptr_t)address;
#ifdef _WIN32
    void* p = VirtualAlloc(want, size, MEM_RESERVE, PAGE_NOACCESS);
    if (!p && !exact && want) p = VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS);
    return p;
#else
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif
#if defined(__linux__) && defined(MAP_FIXED_NOREPLACE)
    if (exact) flags |= MAP_FIXED_NOREPLACE;
#elif defined(__FreeBSD__)
    /* MAP_GUARD: addresses alone, which a later MAP_FIXED map fills. MAP_EXCL: fail where the range is mapped */
    flags = MAP_GUARD;
    if (exact) flags |= MAP_FIXED | MAP_EXCL;
#endif
    void* p = mmap(want, size, PROT_NONE, flags, -1, 0);
    if (p == MAP_FAILED) return NULL;
    /* a Linux older than 4.17 and macOS take the address as a hint */
    if (exact && p != want) {
        munmap(p, size);
        return NULL;
    }
    return p;
#endif
}

bool host_commit(void* address, size_t size) {
#ifdef _WIN32
    return VirtualAlloc(address, size, MEM_COMMIT, PAGE_READWRITE) == address;
#else
    return mmap(address, size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == address;
#endif
}

void* host_lazy(size_t size) {
#ifdef _WIN32
    /* committed whole: Windows charges the commit at once, and gives the pages at their first touch */
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif
    void* p = mmap(NULL, size, PROT_READ | PROT_WRITE, flags, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

void host_unmap(void* address, size_t size) {
#ifdef _WIN32
    (void)size;
    VirtualFree(address, 0, MEM_RELEASE);
#else
    munmap(address, size);
#endif
}

bool host_protect(void* address, size_t size, int access) {
#ifdef _WIN32
    DWORD old, mode = access & HOST_WRITE ? PAGE_READWRITE : access & HOST_READ ? PAGE_READONLY : PAGE_NOACCESS;
    return VirtualProtect(address, size, mode, &old) != 0;
#else
    int prot = (access & HOST_READ ? PROT_READ : 0) | (access & HOST_WRITE ? PROT_WRITE : 0);
    return mprotect(address, size, prot) == 0;
#endif
}

/* ---- the guest's memory, checked ---- */

bool host_copy_checked(void* to, const void* from, size_t size) {
    if (!size) return true;
#if defined(__linux__)
    struct iovec local = {to, size}, remote = {(void*)(uintptr_t)from, size};
    /* the kernel reads from and writes into the remote side: the local one is checked the same way */
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)size;
#elif defined(__APPLE__)
    /* mach_vm_read_overwrite copies within the task, and fails (KERN_INVALID_ADDRESS, KERN_PROTECTION_FAILURE) where
       either side is not mapped with the access */
    mach_vm_size_t done = 0;
    return mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)from, (mach_vm_size_t)size,
                                  (mach_vm_address_t)(uintptr_t)to, &done) == KERN_SUCCESS &&
           done == size;
#elif defined(__FreeBSD__)
    /* through a pipe, a page at a time: write(2) reads from, and fails with EFAULT where it can't; read(2) writes to,
       and fails with EFAULT where it can't (what it leaves in the pipe is drained) */
    pthread_mutex_lock(&g_host.copy_lock);
    if (!g_host.copy_ready) {
        g_host.copy_ready = pipe2(g_host.copy_pipe, O_CLOEXEC | O_NONBLOCK) == 0;
        if (!g_host.copy_ready) {
            pthread_mutex_unlock(&g_host.copy_lock);
            return false;
        }
    }
    bool ok = true;
    for (size_t at = 0; at < size && ok;) {
        size_t n = size - at < 4096 ? size - at : 4096;
        ok = write(g_host.copy_pipe[1], (const char*)from + at, n) == (ssize_t)n;
        if (ok) ok = read(g_host.copy_pipe[0], (char*)to + at, n) == (ssize_t)n;
        if (!ok) {
            char drain[4096];
            while (read(g_host.copy_pipe[0], drain, sizeof drain) > 0) {}
        }
        at += n;
    }
    pthread_mutex_unlock(&g_host.copy_lock);
    return ok;
#elif defined(_WIN32)
    /* ReadProcessMemory reads from in the process, and writes into the caller's buffer with the kernel's check: it
       fails where either side is not mapped with the access */
    SIZE_T done = 0;
    return ReadProcessMemory(GetCurrentProcess(), from, to, size, &done) && done == size;
#endif
}

/* ---- descriptors ---- */

bool host_is_socket(int fd) {
#ifdef _WIN32
    int type = 0, size = sizeof type;
    return fd >= 0 && getsockopt((SOCKET)(unsigned)fd, SOL_SOCKET, SO_TYPE, (char*)&type, &size) == 0;
#else
    struct stat s;
    return fstat(fd, &s) == 0 && S_ISSOCK(s.st_mode);
#endif
}

#ifdef _WIN32
static WinSet* win_set(int fd) {
    for (size_t i = 0; i < g_host.set_count; ++i)
        if (g_host.sets[i].fd == fd && fd >= 0) return &g_host.sets[i];
    return NULL;
}
#endif

int host_close(int fd) {
#ifdef _WIN32
    pthread_mutex_lock(&g_host.sets_lock);
    WinSet* set = win_set(fd);
    if (set) {
        free(set->items);
        *set = (WinSet){0};
        set->fd = -1;
    }
    pthread_mutex_unlock(&g_host.sets_lock);
    if (!set && host_is_socket(fd)) {
        if (closesocket((SOCKET)(unsigned)fd) == 0) return 0;
        errno = EBADF;
        return -1;
    }
    return _close(fd);
#else
    return close(fd);
#endif
}

/* ---- epoll sets ---- */

int host_epoll_create(bool cloexec) {
#if defined(__linux__)
    return epoll_create1(cloexec ? EPOLL_CLOEXEC : 0);
#elif defined(__FreeBSD__) || defined(__APPLE__)
    int q = kqueue();
    if (q >= 0 && cloexec) fcntl(q, F_SETFD, FD_CLOEXEC);
    return q;
#elif defined(_WIN32)
    (void)cloexec;
    int fd = _open("NUL", _O_RDONLY | _O_NOINHERIT);
    if (fd < 0) return -1;
    pthread_mutex_lock(&g_host.sets_lock);
    WinSet* set = NULL;
    for (size_t i = 0; i < g_host.set_count && !set; ++i)
        if (g_host.sets[i].fd < 0) set = &g_host.sets[i];
    if (!set) {
        WinSet* grown = realloc(g_host.sets, (g_host.set_count + 1) * sizeof *grown);
        if (grown) {
            g_host.sets = grown;
            set = &g_host.sets[g_host.set_count++];
        }
    }
    if (set) *set = (WinSet){fd, 0, 0, NULL};
    pthread_mutex_unlock(&g_host.sets_lock);
    if (!set) {
        _close(fd);
        errno = ENOMEM;
        return -1;
    }
    return fd;
#endif
}

#if defined(__FreeBSD__) || defined(__APPLE__)
/* one filter of fd on or off, ENOENT ignored for one off that was never on */
static int kq_filter(int set, int fd, short filter, bool on, uint32_t events, uint64_t data) {
    struct kevent k;
    unsigned short flags = on ? EV_ADD | EV_ENABLE : EV_DELETE;
    if (on && (events & L_EPOLLET)) flags |= EV_CLEAR;
    if (on && (events & L_EPOLLONESHOT)) flags |= EV_DISPATCH;
    EV_SET(&k, (uintptr_t)fd, filter, flags, 0, 0, (void*)(uintptr_t)data);
    int r = kevent(set, &k, 1, NULL, 0, NULL);
    return r < 0 && !on && errno == ENOENT ? 1 : r;
}
#endif

int host_epoll_ctl(int set, int op, int fd, uint32_t events, uint64_t data) {
#if defined(__linux__)
    struct epoll_event e = {0};
    e.events = events;
    e.data.u64 = data;
    return epoll_ctl(set, op, fd, op == EPOLL_CTL_DEL ? NULL : &e);
#elif defined(__FreeBSD__) || defined(__APPLE__)
    if (op != L_EPOLL_CTL_ADD && op != L_EPOLL_CTL_MOD && op != L_EPOLL_CTL_DEL) {
        errno = EINVAL;
        return -1;
    }
    bool on = op != L_EPOLL_CTL_DEL;
    bool read = on && (events & (L_EPOLLIN | L_EPOLLPRI | L_EPOLLRDHUP)), write = on && (events & L_EPOLLOUT);
    int r = kq_filter(set, fd, EVFILT_READ, read, events, data);
    if (r < 0) return -1;
    int w = kq_filter(set, fd, EVFILT_WRITE, write, events, data);
    if (w < 0) return -1;
    if (!on && r == 1 && w == 1) {
        errno = ENOENT;
        return -1;
    }
    return 0;
#elif defined(_WIN32)
    pthread_mutex_lock(&g_host.sets_lock);
    WinSet* s = win_set(set);
    int r = -1, error = EBADF;
    if (s) {
        size_t i = 0;
        while (i < s->count && s->items[i].fd != fd) ++i;
        bool found = i < s->count;
        if (op == L_EPOLL_CTL_ADD && found) error = EEXIST;
        else if (op != L_EPOLL_CTL_ADD && !found) error = ENOENT;
        else if (op == L_EPOLL_CTL_DEL) {
            s->items[i] = s->items[--s->count];
            r = 0;
        } else {
            if (!found && s->count == s->capacity) {
                size_t capacity = s->capacity ? s->capacity * 2 : 8;
                void* grown = realloc(s->items, capacity * sizeof *s->items);
                if (grown) {
                    s->items = grown;
                    s->capacity = capacity;
                }
            }
            if (found || s->count < s->capacity) {
                if (!found) s->count++;
                s->items[i].fd = fd;
                s->items[i].events = events;
                s->items[i].data = data;
                r = 0;
            } else
                error = ENOMEM;
        }
    }
    pthread_mutex_unlock(&g_host.sets_lock);
    if (r) errno = error;
    return r;
#endif
}

int host_epoll_wait(int set, HostEpollEvent* out, int most, int timeout_ms) {
    if (most <= 0) {
        errno = EINVAL;
        return -1;
    }
#if defined(__linux__)
    struct epoll_event events[64];
    int n = epoll_wait(set, events, most < 64 ? most : 64, timeout_ms);
    for (int i = 0; i < n; ++i) {
        out[i].events = events[i].events;
        out[i].data = events[i].data.u64;
    }
    return n;
#elif defined(__FreeBSD__) || defined(__APPLE__)
    struct kevent events[64];
    struct timespec t = {timeout_ms / 1000, (long)(timeout_ms % 1000) * 1000000};
    int n = kevent(set, NULL, 0, events, most < 64 ? most : 64, timeout_ms < 0 ? NULL : &t);
    for (int i = 0; i < n; ++i) {
        uint32_t e = events[i].filter == EVFILT_READ ? L_EPOLLIN : events[i].filter == EVFILT_WRITE ? L_EPOLLOUT : 0;
        if (events[i].flags & EV_EOF) e |= L_EPOLLHUP | (events[i].filter == EVFILT_READ ? L_EPOLLRDHUP : 0);
        if (events[i].flags & EV_ERROR) e |= L_EPOLLERR;
        out[i].events = e;
        out[i].data = (uint64_t)(uintptr_t)events[i].udata;
    }
    return n;
#elif defined(_WIN32)
    WSAPOLLFD fds[64];
    uint64_t data[64];
    ULONG count = 0;
    pthread_mutex_lock(&g_host.sets_lock);
    WinSet* s = win_set(set);
    if (s)
        for (size_t i = 0; i < s->count && count < 64; ++i) {
            if (!host_is_socket(s->items[i].fd)) continue; /* WSAPoll takes sockets alone */
            uint32_t e = s->items[i].events;
            fds[count].fd = (SOCKET)(unsigned)s->items[i].fd;
            fds[count].events = (short)((e & L_EPOLLIN ? POLLRDNORM : 0) | (e & L_EPOLLOUT ? POLLWRNORM : 0));
            fds[count].revents = 0;
            data[count++] = s->items[i].data;
        }
    pthread_mutex_unlock(&g_host.sets_lock);
    if (!s) {
        errno = EBADF;
        return -1;
    }
    if (!count) {
        Sleep(timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms);
        return 0;
    }
    int n = WSAPoll(fds, count, timeout_ms);
    if (n < 0) {
        errno = EINVAL;
        return -1;
    }
    int k = 0;
    for (ULONG i = 0; i < count && k < most; ++i) {
        short r = fds[i].revents;
        if (!r) continue;
        out[k].events = (r & (POLLRDNORM | POLLRDBAND) ? L_EPOLLIN : 0) | (r & POLLWRNORM ? L_EPOLLOUT : 0) |
                        (r & POLLERR ? L_EPOLLERR : 0) | (r & POLLHUP ? L_EPOLLHUP : 0);
        out[k++].data = data[i];
    }
    return k;
#endif
}

/* ---- the guest's private anonymous mappings (host.h) ---- */

typedef struct {
    uint64_t start, end;
    int access;
} AnonPart;
static struct {
    pthread_mutex_t lock;
    AnonPart* parts; /* disjoint, by start */
    size_t count, capacity;
} anon = {.lock = PTHREAD_MUTEX_INITIALIZER};

static bool anon_room(void);
/* [start, end) out of the record, the parts that cross its ends cut there (anon.lock held). A part that holds the
   whole range is the only one it meets: its two pieces take its place and one more */
static void anon_cut(uint64_t start, uint64_t end) {
    for (size_t i = 0; i < anon.count; ++i) {
        AnonPart p = anon.parts[i];
        if (p.start < start && p.end > end) {
            if (!anon_room()) {
                anon.parts[i].end = start; /* no memory: the piece after end goes unrecorded */
                return;
            }
            memmove(anon.parts + i + 2, anon.parts + i + 1, (anon.count - i - 1) * sizeof *anon.parts);
            anon.parts[i] = (AnonPart){p.start, start, p.access};
            anon.parts[i + 1] = (AnonPart){end, p.end, p.access};
            ++anon.count;
            return;
        }
    }
    size_t kept = 0;
    for (size_t i = 0; i < anon.count; ++i) {
        AnonPart p = anon.parts[i];
        if (p.end <= start || p.start >= end) {
            anon.parts[kept++] = p;
            continue;
        }
        if (p.start < start) anon.parts[kept++] = (AnonPart){p.start, start, p.access};
        if (p.end > end) anon.parts[kept++] = (AnonPart){end, p.end, p.access};
    }
    anon.count = kept;
}
/* room for one more part (anon.lock held): false when memory runs out */
static bool anon_room(void) {
    if (anon.count < anon.capacity) return true;
    size_t capacity = anon.capacity ? anon.capacity * 2 : 64;
    AnonPart* parts = realloc(anon.parts, capacity * sizeof *parts);
    if (!parts) return false;
    anon.parts = parts;
    anon.capacity = capacity;
    return true;
}
/* [start, end) recorded with access (anon.lock held, the range cut out before) */
static void anon_insert(uint64_t start, uint64_t end, int access) {
    if (!anon_room()) return; /* unrecorded: its MADV_DONTNEED stays the host's */
    size_t at = 0;
    while (at < anon.count && anon.parts[at].start < start) ++at;
    memmove(anon.parts + at + 1, anon.parts + at, (anon.count - at) * sizeof *anon.parts);
    anon.parts[at] = (AnonPart){start, end, access};
    ++anon.count;
}

void host_anon_map(uint64_t address, uint64_t size, int access) {
    if (!size || address + size < address) return;
    pthread_mutex_lock(&anon.lock);
    anon_cut(address, address + size);
    anon_insert(address, address + size, access);
    pthread_mutex_unlock(&anon.lock);
}
void host_anon_unmap(uint64_t address, uint64_t size) {
    if (!size || address + size < address) return;
    pthread_mutex_lock(&anon.lock);
    anon_cut(address, address + size);
    pthread_mutex_unlock(&anon.lock);
}

/* the recorded parts of [start, end), copied out: their count (at most most) */
static size_t anon_parts_in(uint64_t start, uint64_t end, AnonPart* out, size_t most) {
    size_t n = 0;
    for (size_t i = 0; i < anon.count && n < most; ++i) {
        AnonPart p = anon.parts[i];
        if (p.end <= start || p.start >= end) continue;
        out[n++] = (AnonPart){p.start > start ? p.start : start, p.end < end ? p.end : end, p.access};
    }
    return n;
}

void host_anon_protect(uint64_t address, uint64_t size, int access) {
    if (!size || address + size < address) return;
    pthread_mutex_lock(&anon.lock);
    AnonPart parts[64];
    size_t n = anon_parts_in(address, address + size, parts, 64);
    for (size_t i = 0; i < n; ++i) {
        anon_cut(parts[i].start, parts[i].end);
        anon_insert(parts[i].start, parts[i].end, access);
    }
    pthread_mutex_unlock(&anon.lock);
}

/* fresh zero pages over [address, address + size) with access, the old ones freed */
static bool zero_pages(uint64_t address, uint64_t size, int access) {
#ifdef _WIN32
    void* p = (void*)(uintptr_t)address;
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery(p, &m, sizeof m) || m.Type != MEM_PRIVATE) return false;
    if (!VirtualFree(p, size, MEM_DECOMMIT)) return false;
    if (access == HOST_NONE) return true;
    return VirtualAlloc(p, size, MEM_COMMIT, access & HOST_WRITE ? PAGE_READWRITE : PAGE_READONLY) != NULL;
#else
    int prot = (access & HOST_READ ? PROT_READ : 0) | (access & HOST_WRITE ? PROT_WRITE : 0);
    return mmap((void*)(uintptr_t)address, size, prot, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) != MAP_FAILED;
#endif
}

int host_dontneed(uint64_t address, uint64_t size) {
    if (!size || address + size < address) return 0;
    pthread_mutex_lock(&anon.lock);
    int result = 0;
    for (uint64_t at = address, end = address + size; at < end;) {
        AnonPart parts[64];
        size_t n = anon_parts_in(at, end, parts, 64);
        if (!n) break;
        for (size_t i = 0; i < n; ++i)
            if (!zero_pages(parts[i].start, parts[i].end - parts[i].start, parts[i].access)) result = -1;
        at = parts[n - 1].end;
    }
    pthread_mutex_unlock(&anon.lock);
    return result;
}
