/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The guest filesystem: the virtual nodes (/proc, /dev, /tmp in memory, the files the runtime adds), and with a sysroot
   (gnu_vfs_set_root) the host's files through it (hostfs/hostfs.c). Without one, paths never reach host
   open/stat/unlink APIs. ABI constants/layout follow Linux asm-generic UAPI; host errors are translated. */
#include "gnuabi/filesystem/internal.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#define host_read  _read
#define host_write _write
#else
#include <unistd.h>
#define host_read  read
#define host_write write
#endif

void gnu_vfs_put16(unsigned char* p, uint16_t v) {
    for (unsigned i = 0; i < 2; ++i) p[i] = (unsigned char)(v >> (i * 8));
}
void gnu_vfs_put32(unsigned char* p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (i * 8));
}
void gnu_vfs_put64(unsigned char* p, uint64_t v) {
    for (unsigned i = 0; i < 8; ++i) p[i] = (unsigned char)(v >> (i * 8));
}
static uint64_t get64(const unsigned char* p) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (i * 8);
    return v;
}
static char* copy_string(const char* source) {
    size_t size = strlen(source) + 1;
    char* copy = (char*)malloc(size);
    if (copy) memcpy(copy, source, size);
    return copy;
}
static int host_errno(void) {
    /* errno numbering differs on Windows; never return -errno directly. */
    switch (errno) {
        case EBADF: return -GNU_EBADF;
        case EINVAL: return -GNU_EINVAL;
        case EINTR: return -GNU_EINTR;
        case EAGAIN: return -GNU_EAGAIN;
        case ENOMEM: return -GNU_ENOMEM;
        case EACCES: return -GNU_EACCES;
        case ENOSPC: return -GNU_ENOSPC;
        case EPIPE: return -GNU_EPIPE;
        default: return -GNU_EIO;
    }
}
GnuNode* gnu_vfs_node(GnuVfsState* s, GnuNode* parent, const char* name, enum GnuNodeKind kind, unsigned mode,
                      int readonly) {
    GnuNode* node = (GnuNode*)calloc(1, sizeof *node);
    if (!node) return NULL;
    node->name = copy_string(name);
    if (!node->name) {
        free(node);
        return NULL;
    }
    node->parent = parent;
    node->kind = kind;
    node->mode = mode;
    node->readonly = readonly;
    node->inode = ++s->next_inode;
    node->allocated_next = s->allocated;
    s->allocated = node;
    if (parent) {
        GnuNode** next = &parent->children;
        while (*next) next = &(*next)->sibling;
        *next = node;
    }
    return node;
}
static GnuDescriptor* descriptor(GnuVfsState* s, int fd) {
    if (fd < 0 || fd >= GNU_VFS_FD_MAX || !s->descriptors[fd].file) return NULL;
    return &s->descriptors[fd];
}
static void release_file(GnuOpen* f) {
    if (--f->references) return;
    if (f->ops && f->ops->release) f->ops->release(f->object);
    if (f->node->kind == GNU_NODE_HOST) gnu_hostfs_release(f);
    free(f->snapshot);
    free(f);
}
static void drop_descriptor(GnuDescriptor* d) {
    if (d->file) release_file(d->file);
    d->file = NULL;
    d->flags = 0;
}

/* how many times this host thread holds a table's lock */
static _Thread_local struct {
    unsigned depth;
} t_vfs;
static void vfs_lock(GnuVfsState* s) {
    gnu_mutex_lock(&s->lock);
    t_vfs.depth++;
}
static void vfs_unlock(GnuVfsState* s) {
    t_vfs.depth--;
    gnu_mutex_unlock(&s->lock);
}
void gnu_vfs_lock(GnuVfs* vfs) { vfs_lock(vfs->state); }
void gnu_vfs_unlock(GnuVfs* vfs) { vfs_unlock(vfs->state); }
void gnu_vfs_forked(GnuVfs* vfs) {
    t_vfs.depth = 0;
    gnu_mutex_init(&vfs->state->lock, 1);
}
/* around a host call that can block: the lock goes, and a reference keeps file open, as Linux's file reference keeps
   it during a read that another thread's close() doesn't end */
static unsigned block_begin(GnuVfsState* s, GnuOpen* file) {
    unsigned held = t_vfs.depth;
    file->references++;
    while (t_vfs.depth) vfs_unlock(s);
    return held;
}
static void block_end(GnuVfsState* s, GnuOpen* file, unsigned held) {
    while (held--) vfs_lock(s);
    release_file(file);
}
static int may_block(const GnuOpen* f) {
    return f->ops || f->node->kind == GNU_NODE_STDIO || f->node->kind == GNU_NODE_HOST;
}
static int install(GnuVfsState* s, GnuOpen* file, unsigned flags, int minimum) {
    if (minimum < 0 || minimum >= GNU_VFS_FD_MAX) return -GNU_EINVAL;
    for (int fd = minimum; fd < GNU_VFS_FD_MAX; ++fd)
        if (!s->descriptors[fd].file) {
            s->descriptors[fd].file = file;
            s->descriptors[fd].flags = flags & 1;
            ++file->references;
            return fd;
        }
    return -GNU_EMFILE;
}
static int node_path(GnuNode* node, char* destination, size_t capacity) {
    const GnuNode* chain[GNU_VFS_PATH_MAX / 2];
    size_t count = 0, size = 1;
    while (node && node->parent) {
        if (count >= sizeof chain / sizeof chain[0]) return -GNU_ENAMETOOLONG;
        chain[count++] = node;
        size += strlen(node->name) + 1;
        node = node->parent;
    }
    if (size > capacity) return -GNU_ENAMETOOLONG;
    char* p = destination;
    *p++ = '/';
    while (count) {
        const char* name = chain[--count]->name;
        size_t n = strlen(name);
        memcpy(p, name, n);
        p += n;
        if (count) *p++ = '/';
    }
    *p = 0;
    return 0;
}
static int fd_directory(const GnuNode* n) {
    return n && n->parent && !strcmp(n->name, "fd") &&
           (!strcmp(n->parent->name, "dev") || !strcmp(n->parent->name, "self"));
}
static GnuNode* child(GnuNode* parent, const char* name) {
    for (GnuNode* n = parent->children; n; n = n->sibling)
        if (!n->deleted && !strcmp(n->name, name)) return n;
    return NULL;
}
static int resolve(GnuVfsState* s, int dirfd, const char* path, int follow, GnuNode** out, int* fdlink) {
    size_t length = strlen(path);
    if (!length) return -GNU_ENOENT;
    if (length >= GNU_VFS_PATH_MAX) return -GNU_ENAMETOOLONG;
    GnuNode* node = s->root;
    if (path[0] != '/') {
        if (dirfd == GNU_AT_FDCWD)
            node = s->cwd;
        else {
            GnuDescriptor* d = descriptor(s, dirfd);
            if (!d) return -GNU_EBADF;
            node = d->file->node;
        }
        if (node->kind != GNU_NODE_DIR) return -GNU_ENOTDIR;
        if (!(node->mode & 0100)) return -GNU_EACCES;
    }
    if (fdlink) *fdlink = -1;
    const char* p = path;
    while (*p) {
        while (*p == '/') ++p;
        if (!*p) break;
        if (node->kind != GNU_NODE_DIR) return -GNU_ENOTDIR;
        if (!(node->mode & 0100)) return -GNU_EACCES;
        const char* start = p;
        while (*p && *p != '/') ++p;
        size_t size = (size_t)(p - start);
        if (size > 255) return -GNU_ENAMETOOLONG;
        char name[256];
        memcpy(name, start, size);
        name[size] = 0;
        const char* after = p;
        while (*after == '/') ++after;
        int final = !*after;
        if (!strcmp(name, ".")) continue;
        if (!strcmp(name, "..")) {
            if (node->parent) node = node->parent;
            continue;
        }
        if (fd_directory(node)) {
            char* end = NULL;
            long fd = strtol(name, &end, 10);
            if (!*name || *end || fd < 0 || fd >= GNU_VFS_FD_MAX || !descriptor(s, (int)fd)) return -GNU_ENOENT;
            node = s->descriptors[fd].file->node;
            if (final && fdlink) *fdlink = (int)fd;
        } else {
            /* /proc/<guest pid> is equivalent to /proc/self. */
            if (node->parent == s->root && !strcmp(node->name, "proc")) {
                char pid[32];
                snprintf(pid, sizeof pid, "%u", s->pid);
                if (!strcmp(name, pid)) strcpy(name, "self");
            }
            node = child(node, name);
            if (!node) return -GNU_ENOENT;
            if (node->kind == GNU_NODE_LINK && (!final || follow)) {
                if (!s->executable) return -GNU_ENOENT;
                node = s->executable;
            }
        }
        if (final && *p == '/' && node->kind != GNU_NODE_DIR) return -GNU_ENOTDIR;
    }
    *out = node;
    return 0;
}
static int parent_path(GnuVfsState* s, int dirfd, const char* path, GnuNode** parent, char name[256]) {
    size_t size = strlen(path);
    if (!size) return -GNU_ENOENT;
    if (size >= GNU_VFS_PATH_MAX) return -GNU_ENAMETOOLONG;
    while (size > 1 && path[size - 1] == '/') --size;
    size_t first = size;
    while (first && path[first - 1] != '/') --first;
    size_t n = size - first;
    if (!n || n > 255) return n ? -GNU_ENAMETOOLONG : -GNU_EINVAL;
    memcpy(name, path + first, n);
    name[n] = 0;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return -GNU_EINVAL;
    char prefix[GNU_VFS_PATH_MAX];
    if (!first)
        strcpy(prefix, ".");
    else {
        memcpy(prefix, path, first);
        prefix[first] = 0;
    }
    int error = resolve(s, dirfd, prefix, 1, parent, NULL);
    if (error) return error;
    return (*parent)->kind == GNU_NODE_DIR ? 0 : -GNU_ENOTDIR;
}
int gnu_vfs_init(GnuVfs* vfs) {
    vfs->state = (GnuVfsState*)calloc(1, sizeof *vfs->state);
    if (!vfs->state) return -GNU_ENOMEM;
    GnuVfsState* s = vfs->state;
    gnu_mutex_init(&s->lock, 1);
    s->pid = 1000;
    s->root = gnu_vfs_node(s, NULL, "", GNU_NODE_DIR, 0755, 1);
    s->cwd = s->root;
    if (!s->root || !gnu_vfs_node(s, s->root, "tmp", GNU_NODE_DIR, 01777, 0) || gnu_devfs_init(s) ||
        gnu_procfs_init(s)) {
        gnu_vfs_destroy(vfs);
        return -GNU_ENOMEM;
    }
    for (int fd = 0; fd < 3; ++fd) {
        char name[32];
        snprintf(name, sizeof name, "stdio-%d", fd);
        GnuNode* n = gnu_vfs_node(s, NULL, name, GNU_NODE_STDIO, 0666, 0);
        GnuOpen* f = (GnuOpen*)calloc(1, sizeof *f);
        if (!n || !f) {
            free(f);
            gnu_vfs_destroy(vfs);
            return -GNU_ENOMEM;
        }
        n->host_fd = fd;
        f->node = n;
        f->flags = fd ? GNU_O_WRONLY : 0;
        f->fdlink = -1;
        install(s, f, 0, fd);
#if defined(_WIN32)
        _setmode(fd, _O_BINARY);
#endif
    }
    return 0;
}
void gnu_vfs_destroy(GnuVfs* vfs) {
    GnuVfsState* s = vfs->state;
    if (!s) return;
    /* the guest closes the process's standard descriptors with close(); tearing the namespace down leaves them */
    for (int fd = 0; fd < GNU_VFS_FD_MAX; ++fd)
        if (s->descriptors[fd].file && s->descriptors[fd].file->node->kind == GNU_NODE_HOST &&
            s->descriptors[fd].file->host >= 0 && s->descriptors[fd].file->host <= 2 && !s->descriptors[fd].file->path)
            s->descriptors[fd].file->host = -1;
    for (int fd = 0; fd < GNU_VFS_FD_MAX; ++fd) drop_descriptor(&s->descriptors[fd]);
    GnuNode* n = s->allocated;
    while (n) {
        GnuNode* next = n->allocated_next;
        free(n->name);
        free(n->bytes);
        free(n);
        n = next;
    }
    free(s->cmdline);
    free(s->executable_path);
    free(s->sysroot);
    free(s->cwd_path);
    gnu_mutex_destroy(&s->lock);
    free(s);
    vfs->state = NULL;
}
int gnu_vfs_set_process(GnuVfs* vfs, uint32_t pid, const char* exe, int argc, const char* const* argv) {
    if (!vfs->state || !exe || argc < 0) return -GNU_EINVAL;
    if (exe[0] != '/' || strlen(exe) >= GNU_VFS_PATH_MAX) return -GNU_EINVAL;
    size_t length = 0;
    for (int i = 0; i < argc; ++i) {
        size_t n = strlen(argv[i]) + 1;
        if (n > GNU_VFS_FILE_MAX - length) return -GNU_E2BIG;
        length += n;
    }
    unsigned char* cmdline = (unsigned char*)malloc(length ? length : 1);
    char* path = copy_string(exe);
    if (!cmdline || !path) {
        free(cmdline);
        free(path);
        return -GNU_ENOMEM;
    }
    size_t offset = 0;
    for (int i = 0; i < argc; ++i) {
        size_t n = strlen(argv[i]) + 1;
        memcpy(cmdline + offset, argv[i], n);
        offset += n;
    }
    GnuVfsState* s = vfs->state;
    free(s->cmdline);
    free(s->executable_path);
    s->cmdline = cmdline;
    s->cmdline_size = length;
    s->executable_path = path;
    s->pid = pid;
    return 0;
}
void gnu_vfs_set_pid(GnuVfs* vfs, uint32_t pid) { vfs->state->pid = pid; }
int gnu_vfs_set_executable(GnuVfs* vfs, const void* bytes, size_t length) {
    GnuVfsState* s = vfs->state;
    if (!s) return -GNU_EINVAL;
    if (!s->executable) {
        /* Import at a guest path so readlink(/proc/self/exe) resolves in this namespace. */
        const char* p = s->executable_path ? s->executable_path : "/program";
        GnuNode* parent = s->root;
        for (;;) {
            while (*p == '/') ++p;
            const char* start = p;
            while (*p && *p != '/') ++p;
            size_t size = (size_t)(p - start);
            if (!size || size > 255) return -GNU_EINVAL;
            char name[256];
            memcpy(name, start, size);
            name[size] = 0;
            if (!strcmp(name, ".") || !strcmp(name, "..")) return -GNU_EINVAL;
            while (*p == '/') ++p;
            int final = !*p;
            GnuNode* node = child(parent, name);
            if (!node) node = gnu_vfs_node(s, parent, name, final ? GNU_NODE_FILE : GNU_NODE_DIR, 0555, 1);
            if (!node) return -GNU_ENOMEM;
            if (final) {
                if (node->kind != GNU_NODE_FILE) return -GNU_EISDIR;
                node->readonly = 1;
                node->mode = 0555;
                s->executable = node;
                break;
            }
            if (node->kind != GNU_NODE_DIR) return -GNU_ENOTDIR;
            parent = node;
        }
    }
    int error = gnu_tmpfs_resize(s->executable, length);
    if (error) return error;
    if (length) memcpy(s->executable->bytes, bytes, length);
    return 0;
}
static int open_file(GnuVfsState* s, GnuMemory* memory, int dirfd, const char* path, unsigned flags, unsigned mode) {
    if (flags & GNU_O_PATH) flags &= GNU_O_PATH | GNU_O_DIRECTORY | GNU_O_NOFOLLOW | GNU_O_CLOEXEC;
    if ((flags & GNU_O_ACCMODE) == GNU_O_ACCMODE) return -GNU_EINVAL;
    if (flags & 4194304u) return -GNU_ENOTSUP; /* O_TMPFILE is not implemented. */
    int available = 0;
    for (int fd = 0; fd < GNU_VFS_FD_MAX; ++fd)
        if (!s->descriptors[fd].file) {
            available = 1;
            break;
        }
    if (!available) return -GNU_EMFILE;
    char virtual_path[GNU_VFS_PATH_MAX];
    if (s->sysroot) {
        GnuHostPath* at = (GnuHostPath*)malloc(sizeof *at);
        GnuOpen* opened = NULL;
        int r;
        if (!at) return -GNU_ENOMEM;
        r = gnu_hostfs_resolve(s, dirfd, path, (flags & GNU_O_NOFOLLOW) ? 0 : GNU_HOST_FOLLOW, at);
        if (!r && at->is_virtual) {
            /* /proc, /dev and the added files: the virtual nodes', by their absolute path */
            strcpy(virtual_path, at->guest);
            path = virtual_path;
            dirfd = GNU_AT_FDCWD;
            free(at);
        } else {
            free(at);
            r = gnu_hostfs_open(s, dirfd, path, flags, mode, &opened);
            if (r < 0) return r;
            if (r == 0) {
                int fd = install(s, opened, (flags & GNU_O_CLOEXEC) ? 1 : 0, 0);
                if (fd < 0) {
                    gnu_hostfs_release(opened);
                    free(opened);
                }
                return fd;
            }
        }
    }
    GnuNode* node = NULL;
    int fdlink = -1;
    int error = resolve(s, dirfd, path, !(flags & GNU_O_NOFOLLOW), &node, &fdlink);
    if (error == -GNU_ENOENT && (flags & GNU_O_CREAT)) {
        size_t length = strlen(path);
        if (length && path[length - 1] == '/') return -GNU_EISDIR;
        GnuNode* parent;
        char name[256];
        error = parent_path(s, dirfd, path, &parent, name);
        if (error) return error;
        if (parent->deleted) return -GNU_ENOENT;
        if (parent->readonly) return -GNU_EROFS;
        if (!(parent->mode & 0200)) return -GNU_EACCES;
        node = gnu_vfs_node(s, parent, name, GNU_NODE_FILE, mode & 0777u, 0);
        if (!node) return -GNU_ENOMEM;
    } else if (error)
        return error;
    else if ((flags & (GNU_O_CREAT | GNU_O_EXCL)) == (GNU_O_CREAT | GNU_O_EXCL))
        return -GNU_EEXIST;
    if (node->kind == GNU_NODE_LINK && !(flags & GNU_O_PATH)) return -GNU_ELOOP;
    if (node->kind == GNU_NODE_OBJECT) return -GNU_ENXIO; /* Linux: a socket's /proc/self/fd entry can't be opened */
    if (fdlink >= 0 && (flags & GNU_O_NOFOLLOW) && !(flags & GNU_O_PATH)) return -GNU_ELOOP;
    if ((flags & GNU_O_DIRECTORY) && node->kind != GNU_NODE_DIR) return -GNU_ENOTDIR;
    if (node->kind == GNU_NODE_DIR && ((flags & GNU_O_ACCMODE) || (flags & GNU_O_TRUNC))) return -GNU_EISDIR;
    if (node->readonly && (flags & GNU_O_ACCMODE) && !(flags & GNU_O_PATH)) return -GNU_EACCES;
    if (!(flags & GNU_O_PATH)) {
        unsigned access = flags & GNU_O_ACCMODE;
        if (access != GNU_O_WRONLY && !(node->mode & 0400)) return -GNU_EACCES;
        if (access != 0 && !(node->mode & 0200)) return -GNU_EACCES;
    }
    if ((flags & GNU_O_TRUNC) && !(flags & GNU_O_PATH) && node->kind == GNU_NODE_FILE) {
        if (node->readonly) return -GNU_EACCES;
        error = gnu_tmpfs_resize(node, 0);
        if (error) return error;
    }
    GnuOpen* file = (GnuOpen*)calloc(1, sizeof *file);
    if (!file) return -GNU_ENOMEM;
    file->node = node;
    file->flags = flags & ~(GNU_O_CREAT | GNU_O_EXCL | GNU_O_TRUNC | GNU_O_CLOEXEC);
    file->fdlink = (flags & GNU_O_PATH) && (flags & GNU_O_NOFOLLOW) ? fdlink : -1;
    if (node->kind == GNU_NODE_PROC && !(flags & GNU_O_PATH) && !memory) return -GNU_EACCES; /* gnu_vfs_load */
    if (node->kind == GNU_NODE_PROC && !(flags & GNU_O_PATH)) {
        error = gnu_procfs_snapshot(s, memory, node->subtype, &file->snapshot, &file->snapshot_size);
        if (error) {
            free(file);
            return error;
        }
    }
    int fd = install(s, file, (flags & GNU_O_CLOEXEC) ? 1 : 0, 0);
    if (fd < 0) {
        free(file->snapshot);
        free(file);
    }
    return fd;
}
static int64_t file_read(GnuOpen* file, void* data, size_t length, uint64_t position) {
    GnuNode* node = file->node;
    if ((file->flags & GNU_O_PATH) || (file->flags & GNU_O_ACCMODE) == GNU_O_WRONLY) return -GNU_EBADF;
    if (file->ops) return file->ops->read ? file->ops->read(file->object, file->flags, data, length) : -GNU_EINVAL;
    if (node->kind == GNU_NODE_DIR) return -GNU_EISDIR;
    if (node->kind == GNU_NODE_DEVICE) return gnu_devfs_read(node->subtype, data, length);
    if (node->kind == GNU_NODE_STDIO) {
        size_t chunk = length > INT_MAX ? INT_MAX : length;
        int64_t count = host_read(node->host_fd, data, (unsigned)chunk);
        return count < 0 ? host_errno() : count;
    }
    const unsigned char* bytes = node->kind == GNU_NODE_PROC ? file->snapshot : node->bytes;
    size_t size = node->kind == GNU_NODE_PROC ? file->snapshot_size : node->size;
    if (position >= size) return 0;
    if (length > size - (size_t)position) length = size - (size_t)position;
    if (length) memcpy(data, bytes + (size_t)position, length);
    return (int64_t)length;
}
static int64_t pread_locked(GnuVfs* vfs, int fd, void* destination, size_t length, uint64_t offset) {
    GnuDescriptor* d = descriptor(vfs->state, fd);
    if (!d) return -GNU_EBADF;
    if (d->file->node->kind == GNU_NODE_HOST) {
        if ((d->file->flags & GNU_O_PATH) || (d->file->flags & GNU_O_ACCMODE) == GNU_O_WRONLY) return -GNU_EBADF;
        return gnu_hostfs_read(d->file, destination, length, 1, offset);
    }
    if (d->file->node->kind == GNU_NODE_STDIO || d->file->ops) return -GNU_ESPIPE;
    return file_read(d->file, destination, length, offset);
}
static int64_t file_write(GnuOpen* file, const void* data, size_t length, uint64_t position, int positional) {
    GnuNode* node = file->node;
    if ((file->flags & GNU_O_PATH) || !(file->flags & GNU_O_ACCMODE)) return -GNU_EBADF;
    if (file->ops) return file->ops->write ? file->ops->write(file->object, file->flags, data, length) : -GNU_EINVAL;
    if (node->kind == GNU_NODE_DIR) return -GNU_EISDIR;
    if (node->kind == GNU_NODE_DEVICE) return (int64_t)length;
    if (node->kind == GNU_NODE_STDIO) {
        size_t chunk = length > INT_MAX ? INT_MAX : length;
        int64_t count = host_write(node->host_fd, data, (unsigned)chunk);
        return count < 0 ? host_errno() : count;
    }
    if (node->readonly) return -GNU_EACCES;
    if (file->flags & GNU_O_APPEND) position = node->size;
    if (!length) return 0;
    if (position > GNU_VFS_FILE_MAX || length > GNU_VFS_FILE_MAX - position) return -GNU_EFBIG;
    size_t end = (size_t)position + length;
    if (end > node->size) {
        int error = gnu_tmpfs_resize(node, end);
        if (error) return error;
    }
    memcpy(node->bytes + (size_t)position, data, length);
    if (!positional) file->offset = position + length;
    return (int64_t)length;
}
static int64_t io(GnuVfsState* s, GnuMemory* memory, int fd, uint64_t address, uint64_t length, int write,
                  int positional, uint64_t position) {
    GnuDescriptor* d = descriptor(s, fd);
    if (!d) return -GNU_EBADF;
    if (length > INT64_MAX || length > SIZE_MAX) return -GNU_EINVAL;
    if (positional && (d->file->node->kind == GNU_NODE_STDIO || d->file->ops)) return -GNU_ESPIPE;
    if (positional && (int64_t)position < 0) return -GNU_EINVAL;
    unsigned char dummy = 0;
    /* Linux null/zero discard writes, and null reads do not touch the buffer. */
    if (d->file->node->kind == GNU_NODE_DEVICE && (write || d->file->node->subtype == GNU_DEV_NULL))
        return write ? file_write(d->file, &dummy, (size_t)length, 0, 0)
                     : file_read(d->file, &dummy, (size_t)length, 0);
    void* data =
        length ? gnu_memory_access(memory, address, (size_t)length, write ? GNU_PROT_READ : GNU_PROT_WRITE) : &dummy;
    if (!data) return -GNU_EFAULT;
    uint64_t offset = positional ? position : d->file->offset;
    GnuOpen* file = d->file;
    if (file->node->kind == GNU_NODE_HOST) {
        /* the host descriptor keeps the position, which a forked child shares */
        if (file->flags & GNU_O_PATH) return -GNU_EBADF;
        if (write ? !(file->flags & GNU_O_ACCMODE) : (file->flags & GNU_O_ACCMODE) == GNU_O_WRONLY)
            return -GNU_EBADF;
        unsigned held = block_begin(s, file);
        int64_t r = write ? gnu_hostfs_write(file, data, (size_t)length, positional, position)
                          : gnu_hostfs_read(file, data, (size_t)length, positional, position);
        block_end(s, file, held);
        return r;
    }
    unsigned held = may_block(file) ? block_begin(s, file) : 0;
    int64_t result = write ? file_write(file, data, (size_t)length, offset, positional)
                           : file_read(file, data, (size_t)length, offset);
    if (may_block(file)) block_end(s, file, held);
    if (result > 0 && !positional && !write && file->node->kind != GNU_NODE_DEVICE &&
        file->node->kind != GNU_NODE_STDIO && !file->ops)
        file->offset += (uint64_t)result;
    return result;
}
static int64_t vectored_io(GnuVfsState* s, GnuMemory* memory, int fd, uint64_t vectors, uint64_t count, int write) {
    if (!descriptor(s, fd)) return -GNU_EBADF;
    if (count > 1024) return -GNU_EINVAL;
    unsigned char entries[1024 * 16];
    int error = gnu_memory_read(memory, vectors, entries, (size_t)count * 16);
    if (error) return error;
    uint64_t total_length = 0;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t n = get64(entries + (size_t)i * 16 + 8);
        if (n > INT64_MAX - total_length) return -GNU_EINVAL;
        total_length += n;
    }
    GnuOpen* file = descriptor(s, fd)->file;
    if (file->ops) {
        /* one call for all the vectors: a datagram socket sends them as one datagram, and receives one into them */
        if (total_length > GNU_VFS_FILE_MAX) total_length = GNU_VFS_FILE_MAX;
        unsigned char* bounce = (unsigned char*)malloc(total_length ? (size_t)total_length : 1);
        if (!bounce) return -GNU_ENOMEM;
        size_t used = 0;
        for (uint64_t i = 0; write && i < count && used < total_length; ++i) {
            uint64_t address = get64(entries + (size_t)i * 16), length = get64(entries + (size_t)i * 16 + 8);
            if (length > total_length - used) length = total_length - used;
            if (length && (error = gnu_memory_read(memory, address, bounce + used, (size_t)length))) {
                free(bounce);
                return error;
            }
            used += (size_t)length;
        }
        unsigned held = block_begin(s, file);
        int64_t result =
            write ? file_write(file, bounce, used, 0, 0) : file_read(file, bounce, (size_t)total_length, 0);
        block_end(s, file, held);
        size_t left = result > 0 && !write ? (size_t)result : 0, at = 0;
        for (uint64_t i = 0; i < count && left; ++i) {
            uint64_t address = get64(entries + (size_t)i * 16), length = get64(entries + (size_t)i * 16 + 8);
            if (length > left) length = left;
            if (length && (error = gnu_memory_write(memory, address, bounce + at, (size_t)length))) {
                free(bounce);
                return error;
            }
            at += (size_t)length;
            left -= (size_t)length;
        }
        free(bounce);
        return result;
    }
    int64_t total = 0;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t address = get64(entries + (size_t)i * 16), length = get64(entries + (size_t)i * 16 + 8);
        int64_t result = io(s, memory, fd, address, length, write, 0, 0);
        if (result < 0) return total ? total : result;
        total += result;
        if ((uint64_t)result < length) break;
    }
    return total;
}
static int64_t seek(GnuVfsState* s, int fd, int64_t offset, unsigned whence) {
    GnuDescriptor* d = descriptor(s, fd);
    if (!d) return -GNU_EBADF;
    GnuOpen* f = d->file;
    GnuNode* n = f->node;
    if (f->flags & GNU_O_PATH) return -GNU_EBADF;
    if (n->kind == GNU_NODE_HOST) return gnu_hostfs_seek(f, offset, whence);
    if (n->kind == GNU_NODE_STDIO || f->ops) return -GNU_ESPIPE;
    if (whence > 2) return -GNU_EINVAL;
    if (n->kind == GNU_NODE_DEVICE) return 0;
    uint64_t base = whence == 0 ? 0 : whence == 1 ? f->offset : n->kind == GNU_NODE_PROC ? f->snapshot_size : n->size;
    uint64_t magnitude = offset < 0 ? (uint64_t)(-(offset + 1)) + 1 : (uint64_t)offset;
    if ((offset < 0 && magnitude > base) || (offset >= 0 && magnitude > INT64_MAX - base)) return -GNU_EINVAL;
    f->offset = offset < 0 ? base - magnitude : base + magnitude;
    return (int64_t)f->offset;
}
static unsigned node_type(const GnuNode* n) {
    switch (n->kind) {
        case GNU_NODE_DIR: return 0040000;
        case GNU_NODE_LINK: return 0120000;
        case GNU_NODE_DEVICE:
        case GNU_NODE_STDIO: return 0020000;
        default: return 0100000;
    }
}
int gnu_stat_put(GnuMemory* memory, uint64_t address, const GnuStat* st) {
    unsigned char bytes[128] = {0};
    gnu_vfs_put64(bytes, st->dev);
    gnu_vfs_put64(bytes + 8, st->inode);
    gnu_vfs_put32(bytes + 16, st->mode);
    gnu_vfs_put32(bytes + 20, st->nlink);
    gnu_vfs_put32(bytes + 24, st->uid);
    gnu_vfs_put32(bytes + 28, st->gid);
    gnu_vfs_put64(bytes + 32, st->rdev);
    gnu_vfs_put64(bytes + 48, st->size);
    gnu_vfs_put32(bytes + 56, st->blksize);
    gnu_vfs_put64(bytes + 64, st->blocks);
    gnu_vfs_put64(bytes + 72, (uint64_t)st->atime);
    gnu_vfs_put64(bytes + 80, st->atime_ns);
    gnu_vfs_put64(bytes + 88, (uint64_t)st->mtime);
    gnu_vfs_put64(bytes + 96, st->mtime_ns);
    gnu_vfs_put64(bytes + 104, (uint64_t)st->ctime);
    gnu_vfs_put64(bytes + 112, st->ctime_ns);
    /* exact 128-byte asm-generic stat layout */
    return gnu_memory_write(memory, address, bytes, sizeof bytes);
}
/* struct statx: every basic field is there (STATX_BASIC_STATS), the birth time is not */
int gnu_statx_put(GnuMemory* memory, uint64_t address, const GnuStat* st) {
    unsigned char bytes[256] = {0};
    gnu_vfs_put32(bytes, 0x7ffu);
    gnu_vfs_put32(bytes + 4, st->blksize);
    gnu_vfs_put32(bytes + 16, st->nlink);
    gnu_vfs_put32(bytes + 20, st->uid);
    gnu_vfs_put32(bytes + 24, st->gid);
    gnu_vfs_put16(bytes + 28, (uint16_t)st->mode);
    gnu_vfs_put64(bytes + 32, st->inode);
    gnu_vfs_put64(bytes + 40, st->size);
    gnu_vfs_put64(bytes + 48, st->blocks);
    gnu_vfs_put64(bytes + 64, (uint64_t)st->atime);
    gnu_vfs_put32(bytes + 72, st->atime_ns);
    gnu_vfs_put64(bytes + 96, (uint64_t)st->ctime);
    gnu_vfs_put32(bytes + 104, st->ctime_ns);
    gnu_vfs_put64(bytes + 112, (uint64_t)st->mtime);
    gnu_vfs_put32(bytes + 120, st->mtime_ns);
    gnu_vfs_put32(bytes + 128, (uint32_t)(st->rdev >> 8) & 0xfff);
    gnu_vfs_put32(bytes + 132, (uint32_t)(st->rdev & 0xff) | (uint32_t)(st->rdev >> 12 & 0xfff00));
    gnu_vfs_put32(bytes + 136, (uint32_t)(st->dev >> 8) & 0xfff);
    gnu_vfs_put32(bytes + 140, (uint32_t)(st->dev & 0xff) | (uint32_t)(st->dev >> 12 & 0xfff00));
    return gnu_memory_write(memory, address, bytes, sizeof bytes);
}
static int node_stat(GnuVfsState* s, GnuNode* node, int fdlink, GnuStat* st) {
    uint64_t size = node->size;
    unsigned type = node_type(node);
    if (fdlink >= 0) type = 0120000;
    if (type == 0120000) {
        if (fdlink >= 0 && node != s->executable) {
            char target[GNU_VFS_PATH_MAX];
            const GnuOpen* f = s->descriptors[fdlink].file;
            if (node->kind == GNU_NODE_STDIO)
                snprintf(target, sizeof target, "pipe:[%d]", 10000 + node->host_fd);
            else if (node->kind == GNU_NODE_HOST && f && f->path)
                snprintf(target, sizeof target, "%s", f->path);
            else if (node->kind == GNU_NODE_HOST)
                snprintf(target, sizeof target, "pipe:[%llu]", (unsigned long long)(f ? f->inode : 0));
            else {
                int error = node_path(node, target, sizeof target);
                if (error) return error;
            }
            size = strlen(target) + (node->deleted ? 10 : 0);
        } else
            size = s->executable_path ? strlen(s->executable_path) : 0;
    }
    if (type == 0040000) size = 4096;
    memset(st, 0, sizeof *st);
    st->dev = 1;
    st->inode = node->inode;
    st->mode = type | (fdlink >= 0 ? 0777 : node->mode);
    st->nlink = node->deleted ? 0 : type == 0040000 ? 2 : 1;
    st->uid = 1000;
    st->gid = 1000;
    if (type == 0020000) {
        unsigned minor = node->kind != GNU_NODE_DEVICE   ? 3
                         : node->subtype == GNU_DEV_NULL ? 3
                         : node->subtype == GNU_DEV_ZERO ? 5
                         : !strcmp(node->name, "random") ? 8
                                                         : 9;
        st->rdev = 0x100u + minor;
    }
    st->size = size;
    st->blksize = 4096;
    st->blocks = (size + 511) / 512;
    /* Fixed deterministic guest times. */
    return 0;
}
static int stat_write(GnuVfsState* s, GnuMemory* memory, GnuNode* node, uint64_t address, int fdlink, int extended) {
    GnuStat st;
    int error = node_stat(s, node, fdlink, &st);
    if (error) return error;
    return extended ? gnu_statx_put(memory, address, &st) : gnu_stat_put(memory, address, &st);
}
static int readlink_path(GnuVfsState* s, int dirfd, const char* path, char output[GNU_VFS_PATH_MAX]) {
    GnuNode* node = NULL;
    int fdlink = -1;
    int error = 0;
    if (!*path) {
        GnuDescriptor* d = descriptor(s, dirfd);
        if (!d) return -GNU_EBADF;
        node = d->file->node;
        fdlink = d->file->fdlink;
    } else
        error = resolve(s, dirfd, path, 0, &node, &fdlink);
    if (error) return error;
    if (fdlink >= 0) {
        if (node == s->executable) {
            if (!s->executable_path) return -GNU_ENOENT;
            strncpy(output, s->executable_path, GNU_VFS_PATH_MAX - 1);
            output[GNU_VFS_PATH_MAX - 1] = 0;
            return 0;
        }
        if (node->kind == GNU_NODE_STDIO) {
            snprintf(output, GNU_VFS_PATH_MAX, "pipe:[%d]", 10000 + node->host_fd);
            return 0;
        }
        if (node->kind == GNU_NODE_HOST && s->descriptors[fdlink].file) {
            const GnuOpen* f = s->descriptors[fdlink].file;
            if (f->path)
                snprintf(output, GNU_VFS_PATH_MAX, "%s", f->path);
            else
                snprintf(output, GNU_VFS_PATH_MAX, "%s:[%llu]", f->host_type == 0010000 ? "pipe" : "anon_inode",
                         (unsigned long long)f->inode);
            return 0;
        }
        if (node->kind == GNU_NODE_OBJECT && s->descriptors[fdlink].file && s->descriptors[fdlink].file->ops) {
            const GnuOpen* f = s->descriptors[fdlink].file;
            snprintf(output, GNU_VFS_PATH_MAX, f->ops->file_type ? "%s:[%llu]" : "%s", f->ops->link,
                     (unsigned long long)f->inode);
            return 0;
        }
        error = node_path(node, output, GNU_VFS_PATH_MAX);
        if (error) return error;
        if (node->deleted) {
            size_t n = strlen(output);
            if (n + 11 >= GNU_VFS_PATH_MAX) return -GNU_ENAMETOOLONG;
            strcat(output, " (deleted)");
        }
        return 0;
    }
    if (node->kind != GNU_NODE_LINK) return -GNU_EINVAL;
    if (!s->executable_path) return -GNU_ENOENT;
    if (strlen(s->executable_path) >= GNU_VFS_PATH_MAX) return -GNU_ENAMETOOLONG;
    strcpy(output, s->executable_path);
    return 0;
}
static int64_t directory_entries(GnuVfsState* s, GnuMemory* memory, int fd, uint64_t address, uint64_t count) {
    GnuDescriptor* d = descriptor(s, fd);
    if (!d) return -GNU_EBADF;
    GnuOpen* f = d->file;
    GnuNode* dir = f->node;
    if (f->flags & GNU_O_PATH) return -GNU_EBADF;
    if (count > INT_MAX) count = INT_MAX;
    unsigned char* destination = (unsigned char*)gnu_memory_access(memory, address, (size_t)count, GNU_PROT_WRITE);
    if (count && !destination) return -GNU_EFAULT;
    if (dir->kind == GNU_NODE_HOST) return gnu_hostfs_getdents(f, destination, (size_t)count);
    if (dir->kind != GNU_NODE_DIR) return -GNU_ENOTDIR;
    size_t used = 0;
    uint64_t ordinal = 0;
    for (unsigned index = 0;; ++index) {
        const char* name = NULL;
        GnuNode* node = NULL;
        char number[32];
        int is_link = 0;
        if (index == 0) {
            name = ".";
            node = dir;
        } else if (index == 1) {
            name = "..";
            node = dir->parent ? dir->parent : dir;
        } else if (fd_directory(dir)) {
            unsigned target = index - 2;
            if (target >= GNU_VFS_FD_MAX) break;
            if (!s->descriptors[target].file) continue;
            snprintf(number, sizeof number, "%u", target);
            name = number;
            node = s->descriptors[target].file->node;
            is_link = 1;
        } else {
            unsigned which = index - 2;
            node = dir->children;
            while (node) {
                if (!node->deleted) {
                    if (!which) break;
                    --which;
                }
                node = node->sibling;
            }
            if (!node) break;
            name = node->name;
        }
        if (ordinal++ < f->offset) continue;
        size_t length = (19 + strlen(name) + 1 + 7) & ~(size_t)7;
        if (length > count - used) {
            if (!used) return -GNU_EINVAL;
            break;
        }
        unsigned char* record = destination + used;
        memset(record, 0, length);
        gnu_vfs_put64(record, node->inode);
        gnu_vfs_put64(record + 8, ordinal);
        gnu_vfs_put16(record + 16, (uint16_t)length);
        unsigned type = is_link ? 0120000 : node_type(node);
        record[18] = (unsigned char)(type >> 12);
        strcpy((char*)record + 19, name);
        used += length;
        f->offset = ordinal;
    }
    return (int64_t)used;
}
static int64_t fcntl_file(GnuVfsState* s, int fd, int command, uint64_t argument) {
    GnuDescriptor* d = descriptor(s, fd);
    if (!d) return -GNU_EBADF;
    switch (command) {
        case 0:
        case 1030:
            if (argument >= GNU_VFS_FD_MAX) return -GNU_EINVAL;
            return install(s, d->file, command == 1030, (int)argument);
        case 1: return d->flags;
        case 2: d->flags = (unsigned)argument & 1; return 0;
        case 3: return d->file->flags;
        case 4:
            if (d->file->node->kind == GNU_NODE_HOST) return gnu_hostfs_set_flags(d->file, (unsigned)argument);
            d->file->flags = (d->file->flags & ~(GNU_O_APPEND | GNU_O_NONBLOCK)) |
                             ((unsigned)argument & (GNU_O_APPEND | GNU_O_NONBLOCK));
            return 0;
        default: return -GNU_EINVAL;
    }
}
/* the ioctls every descriptor has. FIONBIO is O_NONBLOCK, which object descriptors (sockets) read on each call */
static int64_t ioctl_file(GnuVfsState* s, GnuMemory* memory, int fd, uint32_t request, uint64_t argument) {
    GnuDescriptor* d = descriptor(s, fd);
    if (!d) return -GNU_EBADF;
    unsigned char word[4];
    int error;
    switch (request) {
        case 0x5421: /* FIONBIO */
            if ((error = gnu_memory_read(memory, argument, word, 4))) return error;
            if (word[0] | word[1] | word[2] | word[3])
                d->file->flags |= GNU_O_NONBLOCK;
            else
                d->file->flags &= ~(unsigned)GNU_O_NONBLOCK;
            return 0;
        case 0x5451: d->flags = 1; return 0; /* FIOCLEX */
        case 0x5450: d->flags = 0; return 0; /* FIONCLEX */
        case 0x541B: {                       /* FIONREAD */
            int bytes = 0;
            GnuOpen* f = d->file;
            if (f->ops) {
                if (!f->ops->readable_bytes) return -GNU_ENOTTY;
                if ((error = f->ops->readable_bytes(f->object, &bytes))) return error;
            } else if (f->node->kind == GNU_NODE_FILE)
                bytes = f->offset < f->node->size ? (int)(f->node->size - f->offset) : 0;
            else if (f->node->kind == GNU_NODE_HOST)
                return gnu_hostfs_ioctl(f, memory, request, argument);
            else
                return -GNU_ENOTTY;
            gnu_vfs_put32(word, (uint32_t)bytes);
            return gnu_memory_write(memory, argument, word, 4);
        }
    }
    if (d->file->node->kind == GNU_NODE_HOST) return gnu_hostfs_ioctl(d->file, memory, request, argument);
    return -GNU_ENOTTY;
}
static int object_stat(GnuMemory* memory, const GnuOpen* f, uint64_t address) {
    unsigned char bytes[128] = {0};
    gnu_vfs_put64(bytes, 8); /* sockfs and anon_inodefs live on devices of their own */
    gnu_vfs_put64(bytes + 8, f->inode);
    gnu_vfs_put32(bytes + 16, f->ops->file_type | (f->ops->file_type ? 0777 : 0600));
    gnu_vfs_put32(bytes + 20, 1);
    gnu_vfs_put32(bytes + 24, 1000);
    gnu_vfs_put32(bytes + 28, 1000);
    gnu_vfs_put32(bytes + 56, 4096);
    return gnu_memory_write(memory, address, bytes, sizeof bytes);
}
static int install_object_locked(GnuVfs* vfs, const GnuObjectOps* ops, void* object, unsigned flags, int cloexec) {
    GnuVfsState* s = vfs->state;
    if (!s->objects && !(s->objects = gnu_vfs_node(s, NULL, "object", GNU_NODE_OBJECT, 0777, 0))) {
        if (ops->release) ops->release(object);
        return -GNU_ENOMEM;
    }
    GnuOpen* file = (GnuOpen*)calloc(1, sizeof *file);
    if (!file) {
        if (ops->release) ops->release(object);
        return -GNU_ENOMEM;
    }
    file->node = s->objects;
    file->flags = GNU_O_RDWR | (flags & GNU_O_NONBLOCK);
    file->fdlink = -1;
    file->ops = ops;
    file->object = object;
    file->inode = ++s->next_inode;
    int fd = install(s, file, cloexec ? 1 : 0, 0);
    if (fd < 0) {
        if (ops->release) ops->release(object);
        free(file);
    }
    return fd;
}
static void* object_locked(GnuVfs* vfs, int fd, const GnuObjectOps* ops, unsigned* flags) {
    GnuDescriptor* d = descriptor(vfs->state, fd);
    if (!d || !d->file->ops || (ops && d->file->ops != ops)) return NULL;
    if (flags) *flags = d->file->flags;
    return d->file->object;
}
static int fd_info_locked(GnuVfs* vfs, int fd, GnuFdInfo* out) {
    GnuDescriptor* d = descriptor(vfs->state, fd);
    memset(out, 0, sizeof *out);
    out->host_fd = -1;
    if (!d) return -GNU_EBADF;
    out->ops = d->file->ops;
    out->object = d->file->object;
    out->flags = d->file->flags;
    out->identity = d->file;
    if (d->file->node->kind == GNU_NODE_STDIO)
        out->host_fd = d->file->node->host_fd;
    else if (d->file->node->kind == GNU_NODE_HOST && d->file->host >= 0 && d->file->host_type != 0100000 &&
             d->file->host_type != 0040000)
        out->host_fd = d->file->host; /* a pipe, a terminal: the host waits on it */
    else if (!d->file->ops)
        out->regular = 1;
    return 0;
}
int gnu_vfs_add_file(GnuVfs* vfs, const char* path, const void* data, size_t size) {
    GnuVfsState* s = vfs->state;
    GnuNode* parent = s->root;
    const char* p = path;
    for (;;) {
        while (*p == '/') ++p;
        const char* start = p;
        while (*p && *p != '/') ++p;
        size_t n = (size_t)(p - start);
        if (!n || n > 255) return -GNU_EINVAL;
        char name[256];
        memcpy(name, start, n);
        name[n] = 0;
        while (*p == '/') ++p;
        int final = !*p;
        GnuNode* node = child(parent, name);
        if (!node) node = gnu_vfs_node(s, parent, name, final ? GNU_NODE_FILE : GNU_NODE_DIR, final ? 0444 : 0755, 1);
        if (!node) return -GNU_ENOMEM;
        if (final) {
            if (node->kind != GNU_NODE_FILE) return -GNU_EISDIR;
            int error = gnu_tmpfs_resize(node, size);
            if (error) return error;
            if (size) memcpy(node->bytes, data, size);
            return 0;
        }
        if (node->kind != GNU_NODE_DIR) return -GNU_ENOTDIR;
        parent = node;
    }
}
/* the host-backed namespace's working folder is a guest path: a virtual folder's is its path in the tree */
static int set_cwd_path(GnuVfsState* s, GnuNode* node) {
    char path[GNU_VFS_PATH_MAX];
    char* copy;
    int error;
    if (!s->sysroot) return 0;
    if ((error = node_path(node, path, sizeof path))) return error;
    if (!(copy = copy_string(path))) return -GNU_ENOMEM;
    free(s->cwd_path);
    s->cwd_path = copy;
    return 0;
}
static int descriptor_stat(GnuVfsState* s, GnuMemory* memory, GnuDescriptor* d, uint64_t address, int extended) {
    GnuStat st;
    int error;
    if (d->file->ops) return object_stat(memory, d->file, address);
    if (d->file->node->kind == GNU_NODE_HOST) {
        if ((error = gnu_hostfs_fstat(d->file, &st))) return error;
        if (!d->file->path && !st.inode) st.inode = d->file->inode;
        return extended ? gnu_statx_put(memory, address, &st) : gnu_stat_put(memory, address, &st);
    }
    return stat_write(s, memory, d->file->node, address, d->file->fdlink, extended);
}
static bool syscall_locked(GnuVfs* vfs, GnuMemory* memory, uint64_t number, const uint64_t arguments[6],
                           int64_t* result) {
    GnuVfsState* s = vfs->state;
    char path[GNU_VFS_PATH_MAX], virtual_path[GNU_VFS_PATH_MAX];
    GnuNode* node = NULL;
    GnuDescriptor* d;
    int error = 0, fdlink = -1;
    uint64_t a[6];
    memcpy(a, arguments, sizeof a);
    virtual_path[0] = 0;
    if (s->sysroot) {
        if (gnu_hostfs_syscall(s, memory, number, a, virtual_path, result)) return true;
        /* a virtual node, named by its absolute path from here on */
        if (virtual_path[0] && number != 49) a[0] = (uint64_t)(int64_t)GNU_AT_FDCWD;
        /* the descriptor calls only host files have */
        if (number == 44 || number == 52 || number == 55 || number == 82 || number == 83 ||
            (number == 88 && !a[1])) {
            d = descriptor(s, (int)a[0]);
            if (!d)
                *result = -GNU_EBADF;
            else if (d->file->node->kind == GNU_NODE_HOST)
                *result = gnu_hostfs_fd_call(d->file, memory, number, a);
            else
                *result = number == 82 || number == 83 ? 0 : number == 44 ? -GNU_ENOSYS : -GNU_EPERM;
            return true;
        }
    }
    switch (number) {
        case 17:
            error = node_path(s->cwd, path, sizeof path);
            if (error) {
                *result = error;
                return true;
            }
            if (strlen(path) + 1 > a[1]) {
                *result = -GNU_ERANGE;
                return true;
            }
            error = gnu_memory_write(memory, a[0], path, strlen(path) + 1);
            *result = error ? error : (int64_t)strlen(path) + 1;
            return true;
        case 23:
            d = descriptor(s, (int)a[0]);
            *result = d ? install(s, d->file, 0, 0) : -GNU_EBADF;
            return true;
        case 24:
            d = descriptor(s, (int)a[0]);
            if (!d) {
                *result = -GNU_EBADF;
                return true;
            }
            if (a[0] == a[1] || (a[2] & ~GNU_O_CLOEXEC)) {
                *result = -GNU_EINVAL;
                return true;
            }
            if (a[1] >= GNU_VFS_FD_MAX) {
                *result = -GNU_EBADF;
                return true;
            }
            drop_descriptor(&s->descriptors[a[1]]);
            s->descriptors[a[1]].file = d->file;
            s->descriptors[a[1]].flags = a[2] ? 1 : 0;
            ++d->file->references;
            *result = (int64_t)a[1];
            return true;
        case 25: *result = fcntl_file(s, (int)a[0], (int)a[1], a[2]); return true;
        case 29: *result = ioctl_file(s, memory, (int)a[0], (uint32_t)a[1], a[2]); return true;
        case 34:
        case 35:
        case 48:
        case 49:
        case 56:
        case 78:
        case 79:
        case 291:
        case 439:
            if (virtual_path[0]) {
                strcpy(path, virtual_path);
                break;
            }
            error = gnu_memory_read_string(memory, number == 49 ? a[0] : a[1], path, sizeof path);
            if (error) {
                *result = error;
                return true;
            }
            break;
        case 46:
            d = descriptor(s, (int)a[0]);
            if (!d) {
                *result = -GNU_EBADF;
                return true;
            }
            if ((d->file->flags & GNU_O_PATH) || !(d->file->flags & GNU_O_ACCMODE)) {
                *result = -GNU_EBADF;
                return true;
            }
            if (d->file->node->kind == GNU_NODE_HOST) {
                *result = (int64_t)a[1] < 0 ? -GNU_EINVAL : gnu_hostfs_truncate(d->file, a[1]);
                return true;
            }
            if (d->file->node->kind != GNU_NODE_FILE || (int64_t)a[1] < 0) {
                *result = -GNU_EINVAL;
                return true;
            }
            *result = a[1] > GNU_VFS_FILE_MAX ? -GNU_EFBIG : gnu_tmpfs_resize(d->file->node, (size_t)a[1]);
            return true;
        case 50:
            d = descriptor(s, (int)a[0]);
            if (!d)
                *result = -GNU_EBADF;
            else if (d->file->node->kind == GNU_NODE_HOST) {
                char* copy = d->file->path && d->file->host_type == 0040000 ? copy_string(d->file->path) : NULL;
                if (!copy)
                    *result = d->file->host_type == 0040000 ? -GNU_ENOMEM : -GNU_ENOTDIR;
                else {
                    free(s->cwd_path);
                    s->cwd_path = copy;
                    *result = 0;
                }
            } else if (d->file->node->kind != GNU_NODE_DIR)
                *result = -GNU_ENOTDIR;
            else {
                s->cwd = d->file->node;
                *result = set_cwd_path(s, d->file->node);
            }
            return true;
        case 57:
            d = descriptor(s, (int)a[0]);
            if (!d)
                *result = -GNU_EBADF;
            else {
                drop_descriptor(d);
                *result = 0;
            }
            return true;
        case 61: *result = directory_entries(s, memory, (int)a[0], a[1], a[2]); return true;
        case 62: *result = seek(s, (int)a[0], (int64_t)a[1], (unsigned)a[2]); return true;
        case 63:
        case 64:
        case 67:
        case 68:
            *result =
                io(s, memory, (int)a[0], a[1], a[2], number == 64 || number == 68, number == 67 || number == 68, a[3]);
            return true;
        case 65:
        case 66: *result = vectored_io(s, memory, (int)a[0], a[1], a[2], number == 66); return true;
        case 80:
            d = descriptor(s, (int)a[0]);
            *result = !d ? -GNU_EBADF : descriptor_stat(s, memory, d, a[1], 0);
            return true;
        default: return false;
    }
    switch (number) {
        case 56: *result = open_file(s, memory, (int)a[0], path, (unsigned)a[2], (unsigned)a[3]); return true;
        case 34: {
            GnuNode* parent;
            char name[256];
            error = parent_path(s, (int)a[0], path, &parent, name);
            if (!error && child(parent, name)) error = -GNU_EEXIST;
            if (!error && parent->deleted) error = -GNU_ENOENT;
            if (!error && parent->readonly) error = -GNU_EROFS;
            if (!error && !(parent->mode & 0200)) error = -GNU_EACCES;
            if (!error && !gnu_vfs_node(s, parent, name, GNU_NODE_DIR, (unsigned)a[2] & 0777u, 0)) error = -GNU_ENOMEM;
            *result = error;
            return true;
        }
        case 35:
            if (a[2] & ~GNU_AT_REMOVEDIR) {
                *result = -GNU_EINVAL;
                return true;
            }
            if (a[2] & GNU_AT_REMOVEDIR) {
                size_t length = strlen(path);
                while (length > 1 && path[length - 1] == '/') --length;
                size_t start = length;
                while (start && path[start - 1] != '/') --start;
                if (length - start == 1 && path[start] == '.') {
                    *result = -GNU_EINVAL;
                    return true;
                }
                if (length - start == 2 && path[start] == '.' && path[start + 1] == '.') {
                    *result = -GNU_ENOTEMPTY;
                    return true;
                }
            }
            error = resolve(s, (int)a[0], path, 0, &node, &fdlink);
            if (!error && (!node->parent || node->parent->readonly || fdlink >= 0)) error = -GNU_EROFS;
            if (!error && (a[2] & GNU_AT_REMOVEDIR) && node->kind != GNU_NODE_DIR) error = -GNU_ENOTDIR;
            if (!error && !(a[2] & GNU_AT_REMOVEDIR) && node->kind == GNU_NODE_DIR) error = -GNU_EISDIR;
            if (!error && node->kind == GNU_NODE_DIR)
                for (GnuNode* c = node->children; c; c = c->sibling)
                    if (!c->deleted) {
                        error = -GNU_ENOTEMPTY;
                        break;
                    }
            if (!error) node->deleted = 1;
            *result = error;
            return true;
        case 48:
        case 439:
            if (a[2] & ~7u || (number == 439 && (a[3] & ~(GNU_AT_SYMLINK_NOFOLLOW | 512u)))) {
                *result = -GNU_EINVAL;
                return true;
            }
            error = resolve(s, (int)a[0], path, number != 439 || !(a[3] & GNU_AT_SYMLINK_NOFOLLOW), &node, NULL);
            if (!error && (a[2] & 2) && node->readonly) error = -GNU_EACCES;
            if (!error && (a[2] & 1) && !(node->mode & 0111)) error = -GNU_EACCES;
            *result = error;
            return true;
        case 49:
            error = resolve(s, GNU_AT_FDCWD, path, 1, &node, NULL);
            if (!error && node->kind != GNU_NODE_DIR) error = -GNU_ENOTDIR;
            if (!error) s->cwd = node;
            if (!error) error = set_cwd_path(s, node);
            *result = error;
            return true;
        case 78: {
            if (!a[3]) {
                *result = -GNU_EINVAL;
                return true;
            }
            char target[GNU_VFS_PATH_MAX];
            error = readlink_path(s, (int)a[0], path, target);
            if (error) {
                *result = error;
                return true;
            }
            size_t size = strlen(target);
            if (size > a[3]) size = (size_t)a[3];
            error = gnu_memory_write(memory, a[2], target, size);
            *result = error ? error : (int64_t)size;
            return true;
        }
        case 79:
        case 291: {
            /* newfstatat(dirfd, path, buf, flags) and statx(dirfd, path, flags, mask, buf) */
            uint64_t flags = number == 79 ? a[3] : a[2], buffer = number == 79 ? a[2] : a[4];
            int extended = number == 291;
            if (flags & ~(uint64_t)(GNU_AT_SYMLINK_NOFOLLOW | GNU_AT_EMPTY_PATH | 2048u | (extended ? 0x6000u : 0))) {
                *result = -GNU_EINVAL;
                return true;
            }
            if (!*path && (flags & GNU_AT_EMPTY_PATH)) {
                if ((int)a[0] == GNU_AT_FDCWD)
                    node = s->cwd; /* the host-backed namespace's is hostfs.c's "." */
                else {
                    d = descriptor(s, (int)a[0]);
                    if (!d)
                        error = -GNU_EBADF;
                    else {
                        *result = descriptor_stat(s, memory, d, buffer, extended);
                        return true;
                    }
                }
            } else
                error = resolve(s, (int)a[0], path, !(flags & GNU_AT_SYMLINK_NOFOLLOW), &node, &fdlink);
            *result = error ? error
                            : stat_write(s, memory, node, buffer,
                                         (!*path || (flags & GNU_AT_SYMLINK_NOFOLLOW)) ? fdlink : -1, extended);
            return true;
        }
        default: return false;
    }
}

int gnu_vfs_set_root(GnuVfs* vfs, const char* sysroot) {
    GnuVfsState* s = vfs->state;
    char cwd[GNU_VFS_PATH_MAX], guest[GNU_VFS_PATH_MAX];
    size_t n;
    GnuNode* tmp;
    if (!s || !sysroot) return -GNU_EINVAL;
    n = strlen(sysroot);
    if (n >= GNU_VFS_PATH_MAX) return -GNU_ENAMETOOLONG;
    free(s->sysroot);
    if (!(s->sysroot = copy_string(sysroot))) return -GNU_ENOMEM;
    for (size_t i = 0; i < n; ++i)
        if (s->sysroot[i] == '\\') s->sysroot[i] = '/';
    while (n > 0 && s->sysroot[n - 1] == '/') s->sysroot[--n] = 0; /* "/" is "": the host itself */
    if (!s->host && !(s->host = gnu_vfs_node(s, NULL, "host", GNU_NODE_HOST, 0777, 0))) return -GNU_ENOMEM;
    /* the host's /tmp, which the processes the guest starts share, instead of one in this process's memory */
    if ((tmp = child(s->root, "tmp")) && tmp->kind == GNU_NODE_DIR) tmp->deleted = 1;
#ifdef _WIN32
    if (!_getcwd(cwd, sizeof cwd)) strcpy(cwd, "/");
#else
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "/");
#endif
    gnu_hostfs_guest_path(s, cwd, guest, sizeof guest);
    free(s->cwd_path);
    if (!(s->cwd_path = copy_string(guest))) return -GNU_ENOMEM;
    /* standard input, output and error as host files: a file, a pipe or a terminal, as the host has them */
    for (int fd = 0; fd < 3; ++fd) {
        GnuOpen* f;
        if (!s->descriptors[fd].file || s->descriptors[fd].file->node->kind != GNU_NODE_STDIO) continue;
        f = gnu_hostfs_wrap(s, fd, fd ? GNU_O_WRONLY : 0, NULL, NULL);
        if (!f) return -GNU_ENOMEM;
        f->inode = ++s->next_inode;
        drop_descriptor(&s->descriptors[fd]);
        s->descriptors[fd].file = f;
        f->references = 1;
    }
    return 0;
}

void gnu_vfs_close_on_exec(GnuVfs* vfs) {
    GnuVfsState* s = vfs->state;
    vfs_lock(s);
    for (int fd = 0; fd < GNU_VFS_FD_MAX; ++fd)
        if (s->descriptors[fd].file && (s->descriptors[fd].flags & 1)) drop_descriptor(&s->descriptors[fd]);
    vfs_unlock(s);
}

static int pipe_locked(GnuVfs* vfs, unsigned flags, int fds[2]) {
    GnuVfsState* s = vfs->state;
    int host[2], guest[2] = {-1, -1};
    if (flags & ~(unsigned)(GNU_O_CLOEXEC | GNU_O_NONBLOCK | 040000 /* O_DIRECT */)) return -GNU_EINVAL;
    if (!s->host && !(s->host = gnu_vfs_node(s, NULL, "host", GNU_NODE_HOST, 0777, 0))) return -GNU_ENOMEM;
#ifdef _WIN32
    if (_pipe(host, 65536, _O_BINARY | _O_NOINHERIT)) return gnu_host_errno(errno);
#else
    if (pipe(host)) return gnu_host_errno(errno);
    for (int i = 0; i < 2; ++i) {
        (void)fcntl(host[i], F_SETFD, FD_CLOEXEC);
        if (flags & GNU_O_NONBLOCK) (void)fcntl(host[i], F_SETFL, fcntl(host[i], F_GETFL) | O_NONBLOCK);
    }
#endif
    for (int i = 0; i < 2; ++i) {
        GnuOpen* f = gnu_hostfs_wrap(s, host[i], (i ? GNU_O_WRONLY : 0) | (flags & GNU_O_NONBLOCK), NULL, NULL);
        int fd = f ? install(s, f, (flags & GNU_O_CLOEXEC) ? 1 : 0, 0) : -GNU_ENOMEM;
        if (f) {
            f->host_type = 0010000;
            f->inode = ++s->next_inode;
        }
        if (fd < 0) {
            if (f) {
                gnu_hostfs_release(f);
                free(f);
            } else
                close(host[i]);
            if (!i) close(host[1]);
            if (i) drop_descriptor(&s->descriptors[guest[0]]);
            return fd;
        }
        guest[i] = fd;
    }
    fds[0] = guest[0];
    fds[1] = guest[1];
    return 0;
}

int gnu_vfs_load(GnuVfs* vfs, int dirfd, const char* path, unsigned char** bytes, size_t* size, char* guest,
                 size_t guest_capacity) {
    GnuVfsState* s = vfs->state;
    GnuDescriptor* d;
    GnuStat st;
    unsigned char* data = NULL;
    size_t used = 0, capacity = 0;
    int error, fd = open_file(s, NULL, dirfd, path, 0, 0);
    if (fd < 0) return fd;
    d = &s->descriptors[fd];
    if (d->file->node->kind == GNU_NODE_HOST) {
        error = gnu_hostfs_fstat(d->file, &st);
        snprintf(guest, guest_capacity, "%s", d->file->path ? d->file->path : path);
    } else {
        error = node_stat(s, d->file->node, -1, &st);
        if (!error) error = node_path(d->file->node, guest, guest_capacity);
    }
    if (!error && (st.mode & 0170000) != 0100000) error = -GNU_EACCES;
    if (!error && !(st.mode & 0111)) error = -GNU_EACCES;
    if (!error && st.size > 0x10000000ull) error = -GNU_ENOMEM;
    while (!error) {
        int64_t n;
        if (used == capacity) {
            unsigned char* bigger;
            capacity = capacity ? capacity * 2 : (size_t)(st.size ? st.size + 1 : 65536);
            if (!(bigger = (unsigned char*)realloc(data, capacity))) {
                error = -GNU_ENOMEM;
                break;
            }
            data = bigger;
        }
        n = d->file->node->kind == GNU_NODE_HOST ? gnu_hostfs_read(d->file, data + used, capacity - used, 1, used)
                                                 : file_read(d->file, data + used, capacity - used, used);
        if (n < 0) error = (int)n;
        if (n <= 0) break;
        used += (size_t)n;
    }
    drop_descriptor(d);
    if (error) {
        free(data);
        return error;
    }
    *bytes = data;
    *size = used;
    return 0;
}

static int host_descriptor_locked(GnuVfs* vfs, int fd) {
    GnuDescriptor* d = descriptor(vfs->state, fd);
    if (!d) return -1;
    if (d->file->node->kind == GNU_NODE_HOST) return d->file->host;
    if (d->file->node->kind == GNU_NODE_STDIO) return d->file->node->host_fd;
    return -1;
}

int gnu_hostfs_program_path(GnuVfs* vfs, const char* host, char* guest, size_t capacity) {
    return gnu_hostfs_guest_path(vfs->state, host, guest, capacity);
}

int gnu_vfs_host_path(GnuVfs* vfs, const char* guest, char* host, size_t capacity) {
    GnuHostPath* at = (GnuHostPath*)malloc(sizeof *at);
    int error;
    if (!at) return -GNU_ENOMEM;
    error = gnu_hostfs_resolve(vfs->state, GNU_AT_FDCWD, guest, GNU_HOST_FOLLOW, at);
    if (!error && at->is_virtual) error = -GNU_ENOENT;
    if (!error) snprintf(host, capacity, "%s", at->host);
    free(at);
    return error;
}

const char* gnu_vfs_cwd(GnuVfs* vfs) { return vfs->state->cwd_path ? vfs->state->cwd_path : "/"; }

int gnu_vfs_set_cwd(GnuVfs* vfs, const char* guest) {
    GnuVfsState* s = vfs->state;
    char* copy;
    if (!s->sysroot) return -GNU_EINVAL;
    if (!(copy = copy_string(guest))) return -GNU_ENOMEM;
    free(s->cwd_path);
    s->cwd_path = copy;
    return 0;
}

int gnu_vfs_adopt(GnuVfs* vfs, int fd, int host) {
    GnuVfsState* s = vfs->state;
    GnuOpen* f;
    if (fd < 0 || fd >= GNU_VFS_FD_MAX) return -GNU_EBADF;
    if (!s->host && !(s->host = gnu_vfs_node(s, NULL, "host", GNU_NODE_HOST, 0777, 0))) return -GNU_ENOMEM;
    if (!(f = gnu_hostfs_wrap(s, host, GNU_O_RDWR, NULL, NULL))) return -GNU_ENOMEM;
    f->inode = ++s->next_inode;
    drop_descriptor(&s->descriptors[fd]);
    s->descriptors[fd].file = f;
    s->descriptors[fd].flags = 0;
    f->references = 1;
    return 0;
}

struct GnuVfsTable {
    GnuDescriptor descriptors[GNU_VFS_FD_MAX];
    char* cwd_path;
};

struct GnuVfsTable* gnu_vfs_save_table(GnuVfs* vfs) {
    GnuVfsState* s = vfs->state;
    struct GnuVfsTable* t = (struct GnuVfsTable*)calloc(1, sizeof *t);
    if (!t) return NULL;
    memcpy(t->descriptors, s->descriptors, sizeof t->descriptors);
    for (int fd = 0; fd < GNU_VFS_FD_MAX; ++fd)
        if (t->descriptors[fd].file) ++t->descriptors[fd].file->references;
    t->cwd_path = s->cwd_path ? copy_string(s->cwd_path) : NULL;
    return t;
}

void gnu_vfs_restore_table(GnuVfs* vfs, struct GnuVfsTable* t) {
    GnuVfsState* s = vfs->state;
    for (int fd = 0; fd < GNU_VFS_FD_MAX; ++fd) drop_descriptor(&s->descriptors[fd]);
    memcpy(s->descriptors, t->descriptors, sizeof t->descriptors);
    if (t->cwd_path) {
        free(s->cwd_path);
        s->cwd_path = t->cwd_path;
    }
    free(t);
}
/* the calls the guest's threads make at once, each under the table's lock */
int64_t gnu_vfs_pread(GnuVfs* vfs, int fd, void* destination, size_t length, uint64_t offset) {
    vfs_lock(vfs->state);
    int64_t r = pread_locked(vfs, fd, destination, length, offset);
    vfs_unlock(vfs->state);
    return r;
}
int gnu_vfs_install_object(GnuVfs* vfs, const GnuObjectOps* ops, void* object, unsigned flags, int cloexec) {
    vfs_lock(vfs->state);
    int r = install_object_locked(vfs, ops, object, flags, cloexec);
    vfs_unlock(vfs->state);
    return r;
}
void* gnu_vfs_object(GnuVfs* vfs, int fd, const GnuObjectOps* ops, unsigned* flags) {
    vfs_lock(vfs->state);
    void* r = object_locked(vfs, fd, ops, flags);
    vfs_unlock(vfs->state);
    return r;
}
int gnu_vfs_fd_info(GnuVfs* vfs, int fd, GnuFdInfo* out) {
    vfs_lock(vfs->state);
    int r = fd_info_locked(vfs, fd, out);
    vfs_unlock(vfs->state);
    return r;
}
bool gnu_vfs_syscall(GnuVfs* vfs, GnuMemory* memory, uint64_t number, const uint64_t arguments[6], int64_t* result) {
    vfs_lock(vfs->state);
    bool r = syscall_locked(vfs, memory, number, arguments, result);
    vfs_unlock(vfs->state);
    return r;
}
int gnu_vfs_pipe(GnuVfs* vfs, unsigned flags, int fds[2]) {
    vfs_lock(vfs->state);
    int r = pipe_locked(vfs, flags, fds);
    vfs_unlock(vfs->state);
    return r;
}
int gnu_vfs_host_descriptor(GnuVfs* vfs, int fd) {
    vfs_lock(vfs->state);
    int r = host_descriptor_locked(vfs, fd);
    vfs_unlock(vfs->state);
    return r;
}
