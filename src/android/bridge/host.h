/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_HOST_H
#define SNX_ANDROID_HOST_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/platform/vm.h"

/* The host system under the native bridge (docs/platforms/android-apps/native.md, "Hosts"): the calls whose form differs between
   the hosts the bridge runs on, Linux, FreeBSD, macOS and Windows x64, each with a version for each of them. The
   bridge's other files call these, not the host's own API for the same work */

/* the process's id as the host numbers it; the calling thread's is thread.h's plat_tid */
uint32_t host_pid(void);

/* The address space is common/platform/vm.h's (plat_mem_*). An access: HOST_NONE, or HOST_READ with HOST_WRITE or
   not, which plat_mem_protect takes as they are */
enum { HOST_NONE = PLAT_MEM_NONE, HOST_READ = PLAT_MEM_READ, HOST_WRITE = PLAT_MEM_WRITE };

/* The guest's private anonymous mappings, with the access of each part (HOST_NONE, HOST_READ, with HOST_WRITE or
   not), which the HLE of mmap, mprotect and munmap keeps: host_anon_map records a range (over what it replaces), and
   host_anon_unmap forgets one (munmap, a file mapping over it). host_anon_protect changes the access of the parts that
   are recorded.
   host_dontneed gives Linux's MADV_DONTNEED to the recorded parts of a range: fresh zero pages with their access, the
   old ones freed. An allocator that hands such pages out again as zero memory (a garbage collector's) depends on it.
   The BSDs' and macOS's MADV_DONTNEED keep the contents, and Windows has no such advice. 0, or -1 */
void host_anon_map(uint64_t address, uint64_t size, int access);
void host_anon_unmap(uint64_t address, uint64_t size);
void host_anon_protect(uint64_t address, uint64_t size, int access);
int host_dontneed(uint64_t address, uint64_t size);
/* size bytes copied from one address to another, either of which may be unmapped or not allow the access: false, and
   nothing done, for such an address. The guest's memory as the kernel checks it (EFAULT, never a fault) */
bool host_copy_checked(void* to, const void* from, size_t size);

/* close(2) of any descriptor the guest has: a file, a socket (Windows' are not the CRT's), an epoll set */
int host_close(int fd);
/* whether fd is a socket (Windows: a SOCKET, whose numbers are not the CRT's descriptors) */
bool host_is_socket(int fd);

/* An epoll set over the host's own readiness API: epoll on Linux, kqueue on FreeBSD and macOS, WSAPoll over its
   sockets on Windows. op and events are Linux's numbers (EPOLL_CTL_ADD 1, EPOLLIN 0x1...), as bionic's are. On
   kqueue a descriptor's read and write readiness come as two events. On Windows only sockets are watched */
typedef struct {
    uint32_t events;
    uint64_t data;
} HostEpollEvent;
int host_epoll_create(bool cloexec); /* a descriptor, or -1 and errno */
int host_epoll_ctl(int set, int op, int fd, uint32_t events, uint64_t data); /* 0, or -1 and errno */
int host_epoll_wait(int set, HostEpollEvent* out, int most, int timeout_ms);  /* the count, or -1 and errno */
#endif
