/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GNU_VFS_H
#define SNX_GNU_VFS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "gnuabi/memory/memory.h"

typedef struct GnuVfs {
    struct GnuVfsState* state;
} GnuVfs;
int gnu_vfs_init(GnuVfs* vfs);
void gnu_vfs_destroy(GnuVfs* vfs);
int gnu_vfs_set_process(GnuVfs* vfs, uint32_t pid, const char* exe, int argc, const char* const* argv);
void gnu_vfs_set_pid(GnuVfs* vfs, uint32_t pid); /* a forked child's: /proc/<pid> */
/* The only imported host file is the executable already validated by the ELF loader. */
int gnu_vfs_set_executable(GnuVfs* vfs, const void* bytes, size_t length);
/* The host-backed namespace: guest paths name the sysroot's files, then the host's (QEMU's -L), the working folder
   is the host's, and standard input, output and error are the host's descriptors. Without it (gnu_vfs_init alone)
   the namespace is isolated: /tmp in memory, and no host file but the program */
int gnu_vfs_set_root(GnuVfs* vfs, const char* sysroot);
/* the guest path of a host path: under the sysroot without its prefix, otherwise as it is */
int gnu_hostfs_program_path(GnuVfs* vfs, const char* host, char* guest, size_t capacity);
/* execve: the descriptors marked close-on-exec go */
void gnu_vfs_close_on_exec(GnuVfs* vfs);
/* pipe2: a host pipe, its read end in fds[0]. flags: O_CLOEXEC, O_NONBLOCK */
int gnu_vfs_pipe(GnuVfs* vfs, unsigned flags, int fds[2]);
/* execve's file: the whole of the regular, executable file at path (its symlinks followed), and its guest path */
int gnu_vfs_load(GnuVfs* vfs, int dirfd, const char* path, unsigned char** bytes, size_t* size, char* guest,
                 size_t guest_capacity);
/* the host descriptor guest descriptor fd is backed by (a file, a pipe, standard input), or -1 */
int gnu_vfs_host_descriptor(GnuVfs* vfs, int fd);
/* the host path a guest path names (its symlinks followed), for a host process to open */
int gnu_vfs_host_path(GnuVfs* vfs, const char* guest, char* host, size_t capacity);
/* the guest's working folder, and setting it (a process started by another copy of the runtime) */
const char* gnu_vfs_cwd(GnuVfs* vfs);
int gnu_vfs_set_cwd(GnuVfs* vfs, const char* guest);
/* host descriptor host as guest descriptor fd (a process started by another copy of the runtime) */
int gnu_vfs_adopt(GnuVfs* vfs, int fd, int host);
/* A vfork child's descriptor table is a copy of its parent's: the parent's is kept aside, its files still open,
   until the child execs or exits, and then it is the process's again */
struct GnuVfsTable;
struct GnuVfsTable* gnu_vfs_save_table(GnuVfs* vfs);
void gnu_vfs_restore_table(GnuVfs* vfs, struct GnuVfsTable* table);
/* the descriptor table's lock, held across a host fork (thread.c) */
void gnu_vfs_lock(GnuVfs* vfs);
void gnu_vfs_unlock(GnuVfs* vfs);
/* a fork's child: the lock again, unlocked (the child's thread is not its owner: its id changed) */
void gnu_vfs_forked(GnuVfs* vfs);
bool gnu_vfs_syscall(GnuVfs* vfs, GnuMemory* memory, uint64_t number, const uint64_t args[6], int64_t* result);
int64_t gnu_vfs_pread(GnuVfs* vfs, int fd, void* destination, size_t length, uint64_t offset);

/* Descriptors backed by another subsystem's object (network/: sockets, eventfd, epoll). read(), write(), close(),
   dup(), fcntl() and fstat() reach them through the descriptor table like files. The calls get the open file's
   flags, so O_NONBLOCK set with fcntl() or FIONBIO applies to the next call. */
typedef struct GnuObjectOps {
    const char* link;   /* readlink() of /proc/self/fd/N: "socket" gives "socket:[inode]" */
    unsigned file_type; /* st_mode's type bits: 0140000 for a socket, 0 for an anonymous inode */
    int64_t (*read)(void* object, unsigned flags, void* data, size_t size);
    int64_t (*write)(void* object, unsigned flags, const void* data, size_t size);
    int (*readable_bytes)(void* object, int* bytes); /* FIONREAD, NULL when it has none */
    void (*release)(void* object);                   /* the last descriptor is closed */
} GnuObjectOps;
/* installs a descriptor at the lowest free number, or -GNU_EMFILE (then the object is released) */
int gnu_vfs_install_object(GnuVfs* vfs, const GnuObjectOps* ops, void* object, unsigned flags, int cloexec);
/* the object behind fd when it is of kind ops, and the open file's flags; NULL otherwise */
void* gnu_vfs_object(GnuVfs* vfs, int fd, const GnuObjectOps* ops, unsigned* flags);
/* what poll() needs to know about a descriptor */
typedef struct GnuFdInfo {
    const GnuObjectOps* ops; /* an object's kind, or NULL */
    void* object;
    int host_fd; /* the host descriptor of stdio, -1 for the others */
    int regular; /* a file, directory or device: always ready */
    unsigned flags;
    const void* identity; /* the open file, which tells a reused descriptor number from the one registered */
} GnuFdInfo;
int gnu_vfs_fd_info(GnuVfs* vfs, int fd, GnuFdInfo* out); /* 0 or -GNU_EBADF */
/* a read-only file the runtime provides (/etc/resolv.conf), its directories made as needed */
int gnu_vfs_add_file(GnuVfs* vfs, const char* path, const void* data, size_t size);
#endif
