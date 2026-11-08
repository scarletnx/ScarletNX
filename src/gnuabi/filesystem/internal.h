/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GNU_VFS_INTERNAL_H
#define SNX_GNU_VFS_INTERNAL_H
#include "gnuabi/filesystem/vfs.h"
#include "gnuabi/abi/errno/errno.h"
#include "gnuabi/kernel/kernel.h"
#include "gnuabi/abi/linux_file.h" /* GnuStat and the open flags */
#define GNU_VFS_PATH_MAX 4096
#define GNU_VFS_FD_MAX   256
#define GNU_VFS_FILE_MAX (64u * 1024u * 1024u)

enum GnuNodeKind {
    GNU_NODE_DIR,
    GNU_NODE_FILE,
    GNU_NODE_DEVICE,
    GNU_NODE_PROC,
    GNU_NODE_LINK,
    GNU_NODE_STDIO,
    GNU_NODE_OBJECT, /* the node every object descriptor shares (vfs.h's GnuObjectOps) */
    GNU_NODE_HOST    /* the node every host file, directory and pipe shares (hostfs.c) */
};
enum GnuDeviceKind { GNU_DEV_NULL, GNU_DEV_ZERO, GNU_DEV_RANDOM };
enum GnuProcKind { GNU_PROC_MAPS, GNU_PROC_CMDLINE, GNU_PROC_STATUS, GNU_PROC_CPUINFO };
enum {
    GNU_AT_FDCWD = -100,
    GNU_AT_SYMLINK_NOFOLLOW = 256,
    GNU_AT_REMOVEDIR = 512,
    GNU_AT_EMPTY_PATH = 4096
};
typedef struct GnuNode {
    struct GnuNode *parent, *children, *sibling, *allocated_next;
    char* name;
    enum GnuNodeKind kind;
    unsigned mode;
    unsigned subtype;
    uint64_t inode;
    unsigned char* bytes;
    size_t size, capacity;
    int readonly, deleted, host_fd;
} GnuNode;
typedef struct GnuOpen {
    GnuNode* node;
    uint64_t offset;
    unsigned flags, references;
    int fdlink; /* O_PATH|O_NOFOLLOW descriptor for a /dev/fd or proc fd symlink. */
    unsigned char* snapshot;
    size_t snapshot_size;
    const GnuObjectOps* ops; /* an object's descriptor */
    void* object;
    uint64_t inode;
    /* a host file, directory or pipe (node kind GNU_NODE_HOST) */
    int host;                /* its host descriptor; -1 for a directory or an O_PATH descriptor, which use path */
    char* path;              /* its guest path, NULL for a pipe */
    char* host_path;         /* its host path, NULL for a pipe */
    unsigned host_type;      /* st_mode's type bits */
    unsigned char* entries;  /* a directory's listing, read at the first getdents64 (hostfs.c) */
    size_t entries_size;
} GnuOpen;
typedef struct GnuDescriptor {
    GnuOpen* file;
    unsigned flags;
} GnuDescriptor;
typedef struct GnuVfsState {
    GnuNode *root, *allocated, *cwd, *executable, *objects;
    GnuDescriptor descriptors[GNU_VFS_FD_MAX];
    uint64_t next_inode;
    uint32_t pid;
    char* executable_path;
    unsigned char* cmdline;
    size_t cmdline_size;
    /* the host-backed namespace (gnu_vfs_set_root): the sysroot, the guest working folder, the host files' node.
       sysroot is NULL for the isolated namespace, where nothing reaches the host */
    char* sysroot;
    char* cwd_path;
    GnuNode* host;
    /* the guest's threads share the table: each call holds this (recursive) lock, and lets it go around a host call
       that can block (a pipe, a terminal, a socket), with a reference that keeps the open file */
    GnuMutex lock;
} GnuVfsState;

/* a GnuStat (abi/linux_file.h) written out as the asm-generic stat or as statx */
int gnu_stat_put(GnuMemory* memory, uint64_t address, const GnuStat* st);
int gnu_statx_put(GnuMemory* memory, uint64_t address, const GnuStat* st);
/* gnu_host_errno: the kernel's (gnuabi/kernel/kernel.h) */

GnuNode* gnu_vfs_node(GnuVfsState* state, GnuNode* parent, const char* name, enum GnuNodeKind kind, unsigned mode,
                      int readonly);
int gnu_devfs_init(GnuVfsState* state);
int64_t gnu_devfs_read(unsigned kind, void* bytes, size_t length);
int gnu_tmpfs_resize(GnuNode* node, size_t size);
int gnu_procfs_init(GnuVfsState* state);
int gnu_procfs_snapshot(GnuVfsState* state, GnuMemory* memory, unsigned kind, unsigned char** bytes, size_t* length);
/* hostfs.c: guest paths through the sysroot (QEMU's -L), then the host. A path resolves to a host path, or to the
   virtual nodes (/proc, /dev and the files the runtime adds) as an absolute guest path for resolve() */
enum { GNU_HOST_FOLLOW = 1, GNU_HOST_CREATE = 2 };
typedef struct GnuHostPath {
    int is_virtual;   /* guest names a virtual node: resolve it there */
    int exists;       /* host names something (otherwise only its folder exists: GNU_HOST_CREATE) */
    unsigned type;    /* st_mode's type bits of what exists */
    char guest[GNU_VFS_PATH_MAX];
    char host[GNU_VFS_PATH_MAX * 2];
} GnuHostPath;
int gnu_hostfs_resolve(GnuVfsState* state, int dirfd, const char* path, unsigned flags, GnuHostPath* out);
/* the guest path for a host path: inside the sysroot without its prefix, otherwise as it is */
int gnu_hostfs_guest_path(GnuVfsState* state, const char* host, char* guest, size_t capacity);
int gnu_hostfs_open(GnuVfsState* state, int dirfd, const char* path, unsigned flags, unsigned mode, GnuOpen** out);
GnuOpen* gnu_hostfs_wrap(GnuVfsState* state, int host, unsigned flags, const char* path, const char* host_path);
int64_t gnu_hostfs_read(GnuOpen* file, void* data, size_t length, int positional, uint64_t position);
int64_t gnu_hostfs_write(GnuOpen* file, const void* data, size_t length, int positional, uint64_t position);
int64_t gnu_hostfs_seek(GnuOpen* file, int64_t offset, unsigned whence);
int gnu_hostfs_fstat(GnuOpen* file, GnuStat* st);
int64_t gnu_hostfs_getdents(GnuOpen* file, unsigned char* destination, size_t count);
int gnu_hostfs_truncate(GnuOpen* file, uint64_t size);
int gnu_hostfs_set_flags(GnuOpen* file, unsigned flags);
int64_t gnu_hostfs_ioctl(GnuOpen* file, GnuMemory* memory, uint32_t request, uint64_t argument);
int64_t gnu_hostfs_fd_call(GnuOpen* file, GnuMemory* memory, uint64_t number, const uint64_t args[6]);
void gnu_hostfs_release(GnuOpen* file);
/* the path syscalls in the host-backed namespace. false when the call is not one, or when its path names a virtual
   node: then virtual_path holds that node's absolute guest path, for the virtual nodes' code to use instead */
bool gnu_hostfs_syscall(GnuVfsState* state, GnuMemory* memory, uint64_t number, const uint64_t args[6],
                        char virtual_path[GNU_VFS_PATH_MAX], int64_t* result);
#endif
