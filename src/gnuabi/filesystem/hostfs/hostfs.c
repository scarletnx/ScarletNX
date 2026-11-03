/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The host-backed guest namespace, as QEMU's -L gives it: a guest path names the file at that path under the sysroot
   when there is one, and the host's file at that path otherwise. Unlike QEMU, the path is resolved here a component
   at a time, so a symlink in the sysroot whose target is absolute (lib64 -> /lib) stays in the sysroot instead of
   reaching the host's /lib, as it would under chroot. /proc and /dev stay the virtual nodes (procfs.c, devfs.c), as
   do the files the runtime adds (gnu_vfs_add_file).
   Guest flags, modes and structures are Linux asm-generic; they are translated to the host's, never passed on. */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "gnuabi/filesystem/internal.h"
#include "common/utilities/endian.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <sys/utime.h>
#else
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>
#endif

static int fail(void) { return gnu_host_errno(errno); }

/* ---- the host calls, by the host path ---- */

#ifdef _WIN32
/* Windows takes UTF-16 paths; a guest path's slashes are fine there */
static wchar_t* wide(const char* path) {
    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    wchar_t* w = n > 0 ? (wchar_t*)malloc((size_t)n * sizeof *w) : NULL;
    if (w && !MultiByteToWideChar(CP_UTF8, 0, path, -1, w, n)) {
        free(w);
        w = NULL;
    }
    return w;
}
typedef struct _stat64 HostStat;
static int host_lstat(const char* path, HostStat* st) {
    wchar_t* w = wide(path);
    int r = w ? _wstat64(w, st) : -1;
    free(w);
    return r;
}
#define host_fstat _fstat64
#else
typedef struct stat HostStat;
static int host_lstat(const char* path, HostStat* st) { return lstat(path, st); }
#define host_fstat fstat
#endif

static unsigned host_type(unsigned mode) {
#ifdef _WIN32
    if ((mode & _S_IFMT) == _S_IFDIR) return 0040000;
    if ((mode & _S_IFMT) == _S_IFCHR) return 0020000;
    if ((mode & _S_IFMT) == _S_IFIFO) return 0010000;
    return 0100000;
#else
    if (S_ISDIR(mode)) return 0040000;
    if (S_ISLNK(mode)) return 0120000;
    if (S_ISCHR(mode)) return 0020000;
    if (S_ISBLK(mode)) return 0060000;
    if (S_ISFIFO(mode)) return 0010000;
    if (S_ISSOCK(mode)) return 0140000;
    return 0100000;
#endif
}

static void from_host(const HostStat* h, GnuStat* st) {
    *st = (GnuStat){0};
    st->dev = (uint64_t)h->st_dev;
    st->inode = (uint64_t)h->st_ino;
    st->mode = host_type((unsigned)h->st_mode) | ((unsigned)h->st_mode & 07777);
    st->nlink = (uint32_t)h->st_nlink;
    st->uid = (uint32_t)h->st_uid;
    st->gid = (uint32_t)h->st_gid;
    st->rdev = (uint64_t)h->st_rdev;
    st->size = (uint64_t)h->st_size;
#ifdef _WIN32
    st->blksize = 4096;
    st->blocks = (st->size + 511) / 512;
    st->atime = h->st_atime;
    st->mtime = h->st_mtime;
    st->ctime = h->st_ctime;
#else
    st->blksize = (uint32_t)h->st_blksize;
    st->blocks = (uint64_t)h->st_blocks;
#if defined(__APPLE__)
    st->atime = h->st_atimespec.tv_sec;
    st->atime_ns = (uint32_t)h->st_atimespec.tv_nsec;
    st->mtime = h->st_mtimespec.tv_sec;
    st->mtime_ns = (uint32_t)h->st_mtimespec.tv_nsec;
    st->ctime = h->st_ctimespec.tv_sec;
    st->ctime_ns = (uint32_t)h->st_ctimespec.tv_nsec;
#else
    st->atime = h->st_atim.tv_sec;
    st->atime_ns = (uint32_t)h->st_atim.tv_nsec;
    st->mtime = h->st_mtim.tv_sec;
    st->mtime_ns = (uint32_t)h->st_mtim.tv_nsec;
    st->ctime = h->st_ctim.tv_sec;
    st->ctime_ns = (uint32_t)h->st_ctim.tv_nsec;
#endif
#endif
}

/* readlink of a host path, NUL-terminated; the length or a guest errno */
static int host_readlink(const char* path, char* target, size_t capacity) {
#ifdef _WIN32
    (void)path;
    (void)target;
    (void)capacity;
    return -GNU_EINVAL; /* the host follows its links itself */
#else
    ssize_t n = readlink(path, target, capacity - 1);
    if (n < 0) return fail();
    if ((size_t)n >= capacity - 1) return -GNU_ENAMETOOLONG;
    target[n] = 0;
    return (int)n;
#endif
}

/* ---- guest paths ---- */

/* "/c/Users" is C:\Users on Windows, as MSYS2 names drives; everything else keeps its name */
static void host_name(const char* guest, char* host, size_t capacity) {
#ifdef _WIN32
    if (guest[0] == '/' && ((guest[1] >= 'a' && guest[1] <= 'z') || (guest[1] >= 'A' && guest[1] <= 'Z')) &&
        (guest[2] == '/' || !guest[2])) {
        snprintf(host, capacity, "%c:/%s", guest[1], guest[2] ? guest + 3 : "");
        return;
    }
#endif
    snprintf(host, capacity, "%s", guest);
}

int gnu_hostfs_guest_path(GnuVfsState* s, const char* host, char* guest, size_t capacity) {
    size_t n = s->sysroot ? strlen(s->sysroot) : 0;
    char copy[GNU_VFS_PATH_MAX];
    if (strlen(host) >= sizeof copy) return -GNU_ENAMETOOLONG;
    strcpy(copy, host);
    for (size_t i = 0; copy[i]; ++i)
        if (copy[i] == '\\') copy[i] = '/';
    if (n && !strncmp(copy, s->sysroot, n) && (copy[n] == '/' || !copy[n])) {
        snprintf(guest, capacity, "%s", copy[n] ? copy + n : "/");
        return 0;
    }
#ifdef _WIN32
    if (copy[0] && copy[1] == ':') {
        snprintf(guest, capacity, "/%c%s", copy[0] | 0x20, copy + 2);
        return 0;
    }
#endif
    snprintf(guest, capacity, "%s", copy);
    return 0;
}

/* where guest path g is: under the sysroot when it is there, on the host otherwise. 0 missing, 1 sysroot, 2 host */
static int locate(GnuVfsState* s, const char* g, char* host, size_t capacity, HostStat* st) {
    snprintf(host, capacity, "%s%s", s->sysroot, g);
    if (!host_lstat(host, st)) return 1;
    host_name(g, host, capacity);
    if (!host_lstat(host, st)) return 2;
    return 0;
}

/* the guest path a descriptor is the folder of, for a relative path */
static int descriptor_folder(GnuVfsState* s, int fd, char* out) {
    GnuDescriptor* d = fd >= 0 && fd < GNU_VFS_FD_MAX && s->descriptors[fd].file ? &s->descriptors[fd] : NULL;
    GnuNode* node;
    const GnuNode* chain[64];
    size_t count = 0;
    if (!d) return -GNU_EBADF;
    if (d->file->node->kind == GNU_NODE_HOST) {
        if (!d->file->path || d->file->host_type != 0040000) return -GNU_ENOTDIR;
        snprintf(out, GNU_VFS_PATH_MAX, "%s", d->file->path);
        return 0;
    }
    node = d->file->node;
    if (node->kind != GNU_NODE_DIR) return -GNU_ENOTDIR;
    out[0] = 0;
    while (node && node->parent && count < 64) {
        chain[count++] = node;
        node = node->parent;
    }
    while (count) {
        size_t used = strlen(out);
        snprintf(out + used, GNU_VFS_PATH_MAX - used, "/%s", chain[--count]->name);
    }
    if (!out[0]) strcpy(out, "/");
    return 0;
}

static GnuNode* virtual_child(GnuNode* parent, const char* name) {
    for (GnuNode* n = parent ? parent->children : NULL; n; n = n->sibling)
        if (!n->deleted && !strcmp(n->name, name)) return n;
    return NULL;
}

/* a file the runtime added at guest path g (gnu_vfs_add_file), which is seen instead of the host's */
static int added_file(GnuVfsState* s, const char* g) {
    GnuNode* current = s->root;
    const char* p = g;
    char name[256];
    while (current) {
        const char* start;
        size_t n;
        while (*p == '/') ++p;
        if (!*p) return current->kind == GNU_NODE_FILE;
        start = p;
        while (*p && *p != '/') ++p;
        n = (size_t)(p - start);
        if (n >= sizeof name) return 0;
        memcpy(name, start, n);
        name[n] = 0;
        current = virtual_child(current, name);
    }
    return 0;
}

int gnu_hostfs_resolve(GnuVfsState* s, int dirfd, const char* path, unsigned flags, GnuHostPath* out) {
    char pending[GNU_VFS_PATH_MAX * 2], rest[GNU_VFS_PATH_MAX * 2], target[GNU_VFS_PATH_MAX];
    char* g = out->guest;
    size_t length = strlen(path);
    int links = 0, error, where, slash;
    HostStat st;
    memset(out, 0, sizeof *out);
    if (!length) return -GNU_ENOENT;
    if (length >= GNU_VFS_PATH_MAX) return -GNU_ENAMETOOLONG;
    slash = path[length - 1] == '/';
    if (path[0] == '/')
        strcpy(pending, path);
    else {
        char base[GNU_VFS_PATH_MAX];
        if (dirfd == GNU_AT_FDCWD)
            snprintf(base, sizeof base, "%s", s->cwd_path);
        else if ((error = descriptor_folder(s, dirfd, base)))
            return error;
        snprintf(pending, sizeof pending, "%s/%s", base, path);
    }
    g[0] = 0; /* the canonical guest path so far, "" for the root */
    for (;;) {
        char* p = pending;
        char name[256];
        size_t n;
        int final;
        while (*p == '/') ++p;
        if (!*p) break;
        {
            const char* start = p;
            while (*p && *p != '/') ++p;
            n = (size_t)(p - start);
            if (n > 255) return -GNU_ENAMETOOLONG;
            memcpy(name, start, n);
            name[n] = 0;
        }
        while (*p == '/') ++p;
        final = !*p;
        memmove(pending, p, strlen(p) + 1); /* what is left after this component */
        if (!strcmp(name, ".")) continue;
        if (!strcmp(name, "..")) {
            char* cut = strrchr(g, '/');
            if (cut) *cut = 0;
            continue;
        }
        /* /proc and /dev are the virtual nodes, except the links that lead back to the host: the program
           (/proc/self/exe) and a host descriptor's file (/proc/self/fd/N, /dev/fd/N) */
        if (!g[0] && (!strcmp(name, "proc") || !strcmp(name, "dev"))) {
            char whole[GNU_VFS_PATH_MAX * 2];
            snprintf(whole, sizeof whole, "/%s%s%s", name, final ? "" : "/", pending);
            {
                char pid[32], self_exe[64];
                const char* tail = NULL;
                snprintf(pid, sizeof pid, "/proc/%u/", s->pid);
                snprintf(self_exe, sizeof self_exe, "%sexe", pid);
                if (!strncmp(whole, "/proc/self/exe", 14) && (!whole[14] || whole[14] == '/'))
                    tail = whole + 14;
                else if (!strncmp(whole, self_exe, strlen(self_exe)) &&
                         (!whole[strlen(self_exe)] || whole[strlen(self_exe)] == '/'))
                    tail = whole + strlen(self_exe);
                if (tail && s->executable_path && (*tail || (flags & GNU_HOST_FOLLOW))) {
                    if (++links > 40) return -GNU_ELOOP;
                    snprintf(rest, sizeof rest, "%s%s", s->executable_path, tail);
                    strcpy(pending, rest);
                    g[0] = 0;
                    continue;
                }
                tail = !strncmp(whole, "/proc/self/fd/", 14) ? whole + 14
                       : !strncmp(whole, "/dev/fd/", 8)      ? whole + 8
                                                             : NULL;
                if (tail) {
                    char* end;
                    long fd = strtol(tail, &end, 10);
                    if (end != tail && (!*end || *end == '/') && fd >= 0 && fd < GNU_VFS_FD_MAX &&
                        s->descriptors[fd].file && s->descriptors[fd].file->node->kind == GNU_NODE_HOST &&
                        s->descriptors[fd].file->path && (*end || (flags & GNU_HOST_FOLLOW))) {
                        if (++links > 40) return -GNU_ELOOP;
                        snprintf(rest, sizeof rest, "%s%s", s->descriptors[fd].file->path, end);
                        strcpy(pending, rest);
                        g[0] = 0;
                        continue;
                    }
                }
            }
            if (strlen(whole) >= GNU_VFS_PATH_MAX) return -GNU_ENAMETOOLONG;
            strcpy(g, whole);
            out->is_virtual = 1;
            return 0;
        }
        if (strlen(g) + 1 + n >= GNU_VFS_PATH_MAX) return -GNU_ENAMETOOLONG;
        strcat(g, "/");
        strcat(g, name);
        if (added_file(s, g)) {
            if (!final) return -GNU_ENOTDIR;
            out->is_virtual = 1;
            return 0;
        }
        where = locate(s, g, out->host, sizeof out->host, &st);
        if (!where) {
            if (!final) return -GNU_ENOENT;
            if (!(flags & GNU_HOST_CREATE)) return -GNU_ENOENT;
            /* made in the tree its folder is in */
            {
                char folder[GNU_VFS_PATH_MAX];
                HostStat fs;
                char* cut;
                strcpy(folder, g);
                cut = strrchr(folder, '/');
                *cut = 0;
                snprintf(out->host, sizeof out->host, "%s%s", s->sysroot, folder[0] ? folder : "/");
                if (!host_lstat(out->host, &fs))
                    snprintf(out->host, sizeof out->host, "%s%s", s->sysroot, g);
                else
                    host_name(g, out->host, sizeof out->host);
            }
            return 0;
        }
        if (host_type((unsigned)st.st_mode) == 0120000 && (!final || (flags & GNU_HOST_FOLLOW))) {
            int r = host_readlink(out->host, target, sizeof target);
            char* cut;
            if (r < 0) return r;
            if (++links > 40) return -GNU_ELOOP;
            snprintf(rest, sizeof rest, "%s%s%s", target, pending[0] ? "/" : "", pending);
            if (strlen(rest) >= sizeof pending) return -GNU_ENAMETOOLONG;
            strcpy(pending, rest);
            if (target[0] == '/')
                g[0] = 0;
            else {
                cut = strrchr(g, '/');
                *cut = 0;
            }
            continue;
        }
        if (!final && host_type((unsigned)st.st_mode) != 0040000) return -GNU_ENOTDIR;
    }
    /* the root, or the last component found (looked up again: a ".." may have come after it) */
    if (!g[0]) {
        strcpy(g, "/");
        if (!locate(s, "", out->host, sizeof out->host, &st)) {
            host_name("/", out->host, sizeof out->host);
            if (host_lstat(out->host, &st)) return -GNU_ENOENT;
        }
    } else if (!locate(s, g, out->host, sizeof out->host, &st))
        return -GNU_ENOENT;
    out->exists = 1;
    out->type = host_type((unsigned)st.st_mode);
    if (slash && out->type != 0040000 && out->type != 0120000) return -GNU_ENOTDIR;
    return 0;
}

/* ---- host files as guest descriptors ---- */

static int host_open_flags(unsigned flags) {
    int h;
    switch (flags & GNU_O_ACCMODE) {
        case GNU_O_WRONLY: h = O_WRONLY; break;
        case GNU_O_RDWR: h = O_RDWR; break;
        default: h = O_RDONLY; break;
    }
    if (flags & GNU_O_CREAT) h |= O_CREAT;
    if (flags & GNU_O_EXCL) h |= O_EXCL;
    if (flags & GNU_O_TRUNC) h |= O_TRUNC;
    if (flags & GNU_O_APPEND) h |= O_APPEND;
#ifdef _WIN32
    h |= O_BINARY | O_NOINHERIT;
#else
    /* the guest's close-on-exec is the descriptor table's: the host's descriptors never reach a host exec */
    h |= O_CLOEXEC | O_NOCTTY;
    if (flags & GNU_O_NONBLOCK) h |= O_NONBLOCK;
    if (flags & GNU_O_NOFOLLOW) h |= O_NOFOLLOW;
#ifdef O_DSYNC
    if (flags & GNU_O_DSYNC) h |= O_DSYNC;
#endif
#endif
    return h;
}

static char* duplicate(const char* text) {
    char* copy = text ? (char*)malloc(strlen(text) + 1) : NULL;
    if (copy) strcpy(copy, text);
    return copy;
}

GnuOpen* gnu_hostfs_wrap(GnuVfsState* s, int host, unsigned flags, const char* path, const char* host_path) {
    GnuOpen* file = (GnuOpen*)calloc(1, sizeof *file);
    HostStat st;
    if (!file) return NULL;
    file->path = duplicate(path);
    file->host_path = duplicate(host_path);
    if ((path && !file->path) || (host_path && !file->host_path)) {
        free(file->path);
        free(file->host_path);
        free(file);
        return NULL;
    }
    file->node = s->host;
    file->host = host;
    file->flags = flags;
    file->fdlink = -1;
    file->host_type = host >= 0 && !host_fstat(host, &st) ? host_type((unsigned)st.st_mode) : 0100000;
    return file;
}

int gnu_hostfs_open(GnuVfsState* s, int dirfd, const char* path, unsigned flags, unsigned mode, GnuOpen** out) {
    GnuHostPath at;
    int host = -1, error;
    unsigned resolve = (flags & GNU_O_NOFOLLOW) ? 0 : GNU_HOST_FOLLOW;
    if ((flags & GNU_O_CREAT) && !(flags & GNU_O_PATH)) resolve |= GNU_HOST_CREATE;
    /* O_CREAT|O_EXCL never follows a final symlink */
    if ((flags & (GNU_O_CREAT | GNU_O_EXCL)) == (GNU_O_CREAT | GNU_O_EXCL)) resolve &= ~(unsigned)GNU_HOST_FOLLOW;
    if ((error = gnu_hostfs_resolve(s, dirfd, path, resolve, &at))) return error;
    if (at.is_virtual) return 1;
    if (at.exists && (flags & (GNU_O_CREAT | GNU_O_EXCL)) == (GNU_O_CREAT | GNU_O_EXCL)) return -GNU_EEXIST;
    if (at.exists && at.type == 0120000 && !(flags & GNU_O_PATH)) return -GNU_ELOOP;
    if ((flags & GNU_O_DIRECTORY) && at.exists && at.type != 0040000) return -GNU_ENOTDIR;
    if (at.exists && at.type == 0040000) {
        /* a folder is its path: listed, stat'ed and used as a base by name, which every host can do */
        if ((flags & GNU_O_ACCMODE) && !(flags & GNU_O_PATH)) return -GNU_EISDIR;
#ifndef _WIN32
        if (!(flags & GNU_O_PATH) && access(at.host, R_OK | X_OK)) return fail();
#endif
    } else if (flags & GNU_O_PATH) {
        if (!at.exists) return -GNU_ENOENT;
    } else {
#ifdef _WIN32
        wchar_t* w = wide(at.host);
        host = w ? _wopen(w, host_open_flags(flags), (mode & 0200) ? _S_IREAD | _S_IWRITE : _S_IREAD) : -1;
        free(w);
#else
        host = open(at.host, host_open_flags(flags), (mode_t)(mode & 07777));
#endif
        if (host < 0) return fail();
    }
    *out = gnu_hostfs_wrap(s, host, flags & ~(GNU_O_CREAT | GNU_O_EXCL | GNU_O_TRUNC | GNU_O_CLOEXEC), at.guest,
                           at.host);
    if (!*out) {
        if (host >= 0) close(host);
        return -GNU_ENOMEM;
    }
    if (host < 0) (*out)->host_type = at.exists ? at.type : 0100000;
    return 0;
}

int64_t gnu_hostfs_read(GnuOpen* f, void* data, size_t length, int positional, uint64_t position) {
    int64_t n;
    if (f->host_type == 0040000) return -GNU_EISDIR;
    if (f->host < 0) return -GNU_EBADF;
    if (length > 0x7ffff000) length = 0x7ffff000;
#ifdef _WIN32
    if (positional) {
        __int64 old = _lseeki64(f->host, 0, SEEK_CUR);
        if (old < 0 || _lseeki64(f->host, (__int64)position, SEEK_SET) < 0) return -GNU_ESPIPE;
        n = _read(f->host, data, (unsigned)length);
        _lseeki64(f->host, old, SEEK_SET);
    } else
        n = _read(f->host, data, (unsigned)length);
#else
    n = positional ? pread(f->host, data, length, (off_t)position) : read(f->host, data, length);
#endif
    return n < 0 ? fail() : n;
}

int64_t gnu_hostfs_write(GnuOpen* f, const void* data, size_t length, int positional, uint64_t position) {
    int64_t n;
    if (f->host_type == 0040000) return -GNU_EISDIR;
    if (f->host < 0) return -GNU_EBADF;
    if (length > 0x7ffff000) length = 0x7ffff000;
#ifdef _WIN32
    if (positional) {
        __int64 old = _lseeki64(f->host, 0, SEEK_CUR);
        if (old < 0 || _lseeki64(f->host, (__int64)position, SEEK_SET) < 0) return -GNU_ESPIPE;
        n = _write(f->host, data, (unsigned)length);
        _lseeki64(f->host, old, SEEK_SET);
    } else
        n = _write(f->host, data, (unsigned)length);
#else
    n = positional ? pwrite(f->host, data, length, (off_t)position) : write(f->host, data, length);
#endif
    return n < 0 ? fail() : n;
}

int64_t gnu_hostfs_seek(GnuOpen* f, int64_t offset, unsigned whence) {
    int64_t r;
    if (whence > 2) return -GNU_EINVAL;
    if (f->host_type == 0040000) {
        /* a folder's position is an index into its listing (getdents64's d_off) */
        if (whence != 0 || offset < 0) return -GNU_EINVAL;
        f->offset = (uint64_t)offset;
        return offset;
    }
    if (f->host < 0) return -GNU_EBADF;
    if (f->host_type == 0010000 || f->host_type == 0140000) return -GNU_ESPIPE;
#ifdef _WIN32
    r = _lseeki64(f->host, offset, (int)whence);
#else
    r = lseek(f->host, (off_t)offset, (int)whence);
#endif
    return r < 0 ? fail() : r;
}

int gnu_hostfs_fstat(GnuOpen* f, GnuStat* st) {
    HostStat h;
    if (f->host >= 0 ? host_fstat(f->host, &h) : !f->host_path ? (errno = EBADF, -1) : host_lstat(f->host_path, &h))
        return fail();
    from_host(&h, st);
    return 0;
}

/* a folder's entries, read once: {u64 inode, u8 type, u16 length, name NUL}..., in readdir's order */
static int read_listing(GnuOpen* f) {
    size_t used = 0, capacity = 4096;
    unsigned char* list = (unsigned char*)malloc(capacity);
    if (!list) return -GNU_ENOMEM;
#ifdef _WIN32
    {
        char pattern[GNU_VFS_PATH_MAX * 2 + 3];
        WIN32_FIND_DATAW found;
        HANDLE search;
        wchar_t* w;
        snprintf(pattern, sizeof pattern, "%s/*", f->host_path);
        w = wide(pattern);
        search = w ? FindFirstFileW(w, &found) : INVALID_HANDLE_VALUE;
        free(w);
        if (search == INVALID_HANDLE_VALUE) {
            free(list);
            return -GNU_ENOENT;
        }
        do {
            char name[1024];
            size_t n;
            unsigned type;
            if (!WideCharToMultiByte(CP_UTF8, 0, found.cFileName, -1, name, sizeof name, NULL, NULL)) continue;
            n = strlen(name);
            type = found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? 4 : 8;
            if (used + 11 + n + 1 > capacity) {
                unsigned char* bigger;
                while (used + 11 + n + 1 > capacity) capacity *= 2;
                if (!(bigger = (unsigned char*)realloc(list, capacity))) {
                    FindClose(search);
                    free(list);
                    return -GNU_ENOMEM;
                }
                list = bigger;
            }
            memset(list + used, 0, 8);
            snx_st_le64(list + used, (uint64_t)(used + 1));
            list[used + 8] = (unsigned char)type;
            snx_st_le16(list + used + 9, (uint16_t)n);
            memcpy(list + used + 11, name, n + 1);
            used += 11 + n + 1;
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
#else
    {
        DIR* dir = opendir(f->host_path);
        struct dirent* entry;
        if (!dir) {
            int error = fail();
            free(list);
            return error;
        }
        while ((entry = readdir(dir))) {
            size_t n = strlen(entry->d_name);
            unsigned type = 0;
#ifdef DT_UNKNOWN
            type = entry->d_type; /* Linux's DT_ values are the guest's */
#endif
            if (used + 11 + n + 1 > capacity) {
                unsigned char* bigger;
                while (used + 11 + n + 1 > capacity) capacity *= 2;
                if (!(bigger = (unsigned char*)realloc(list, capacity))) {
                    closedir(dir);
                    free(list);
                    return -GNU_ENOMEM;
                }
                list = bigger;
            }
            snx_st_le64(list + used, (uint64_t)entry->d_ino);
            list[used + 8] = (unsigned char)type;
            snx_st_le16(list + used + 9, (uint16_t)n);
            memcpy(list + used + 11, entry->d_name, n + 1);
            used += 11 + n + 1;
        }
        closedir(dir);
    }
#endif
    free(f->entries);
    f->entries = list;
    f->entries_size = used;
    return 0;
}

int64_t gnu_hostfs_getdents(GnuOpen* f, unsigned char* destination, size_t count) {
    size_t used = 0, at = 0;
    uint64_t index = 0;
    int error;
    if (f->host_type != 0040000) return -GNU_ENOTDIR;
    if (f->flags & GNU_O_PATH) return -GNU_EBADF;
    /* the listing is read again from the start, as rewinddir does */
    if ((!f->entries || !f->offset) && (error = read_listing(f))) return error;
    while (at < f->entries_size) {
        size_t n = snx_ld_le16(f->entries + at + 9);
        size_t length = (19 + n + 1 + 7) & ~(size_t)7;
        if (index++ >= f->offset) {
            unsigned char* record;
            if (length > count - used) {
                if (!used) return -GNU_EINVAL;
                break;
            }
            record = destination + used;
            memset(record, 0, length);
            memcpy(record, f->entries + at, 8);
            snx_st_le64(record + 8, index);
            snx_st_le16(record + 16, (uint16_t)length);
            record[18] = f->entries[at + 8];
            memcpy(record + 19, f->entries + at + 11, n + 1);
            used += length;
            f->offset = index;
        }
        at += 11 + n + 1;
    }
    return (int64_t)used;
}

int gnu_hostfs_truncate(GnuOpen* f, uint64_t size) {
    if (f->host < 0 || f->host_type != 0100000) return -GNU_EINVAL;
#ifdef _WIN32
    if (_chsize_s(f->host, (__int64)size)) return fail();
#else
    if (ftruncate(f->host, (off_t)size)) return fail();
#endif
    return 0;
}

int gnu_hostfs_set_flags(GnuOpen* f, unsigned flags) {
#ifndef _WIN32
    if (f->host >= 0) {
        int h = fcntl(f->host, F_GETFL);
        if (h < 0) return fail();
        h = (h & ~(O_NONBLOCK | O_APPEND)) | (flags & GNU_O_NONBLOCK ? O_NONBLOCK : 0) |
            (flags & GNU_O_APPEND ? O_APPEND : 0);
        if (fcntl(f->host, F_SETFL, h)) return fail();
    }
#endif
    f->flags = (f->flags & ~(GNU_O_APPEND | GNU_O_NONBLOCK)) | (flags & (GNU_O_APPEND | GNU_O_NONBLOCK));
    return 0;
}

int64_t gnu_hostfs_ioctl(GnuOpen* f, GnuMemory* memory, uint32_t request, uint64_t argument) {
    unsigned char bytes[64];
    if (f->host < 0) return -GNU_ENOTTY;
    switch (request) {
        case 0x5401: /* TCGETS: what isatty() asks. A terminal's settings as the host has them */
#ifdef _WIN32
            if (!_isatty(f->host)) return -GNU_ENOTTY;
            memset(bytes, 0, 60);
            snx_st_le32(bytes + 0, 0x500);  /* ICRNL | IXON */
            snx_st_le32(bytes + 4, 0x5);    /* OPOST | ONLCR */
            snx_st_le32(bytes + 8, 0xbf);   /* B38400 | CS8 | CREAD */
            snx_st_le32(bytes + 12, 0x8a3b); /* ISIG | ICANON | ECHO... */
            return gnu_memory_write(memory, argument, bytes, 60);
#else
        {
            struct termios t;
            if (tcgetattr(f->host, &t)) return fail();
            memset(bytes, 0, 60);
#ifdef __linux__
            /* Linux's termios is the same on every architecture but the c_cc size; the flags are the guest's */
            snx_st_le32(bytes + 0, (uint32_t)t.c_iflag);
            snx_st_le32(bytes + 4, (uint32_t)t.c_oflag);
            snx_st_le32(bytes + 8, (uint32_t)t.c_cflag);
            snx_st_le32(bytes + 12, (uint32_t)t.c_lflag);
            bytes[16] = (unsigned char)t.c_line;
            memcpy(bytes + 17, t.c_cc, sizeof t.c_cc < 19 ? sizeof t.c_cc : 19);
#else
            snx_st_le32(bytes + 0, 0x500);
            snx_st_le32(bytes + 4, 0x5);
            snx_st_le32(bytes + 8, 0xbf);
            snx_st_le32(bytes + 12, 0x8a3b);
#endif
            return gnu_memory_write(memory, argument, bytes, 60);
        }
#endif
        case 0x5413: { /* TIOCGWINSZ: four shorts, rows, columns, x and y pixels */
#ifdef _WIN32
            CONSOLE_SCREEN_BUFFER_INFO info;
            HANDLE h = (HANDLE)_get_osfhandle(f->host);
            if (!_isatty(f->host) || !GetConsoleScreenBufferInfo(h, &info)) return -GNU_ENOTTY;
            memset(bytes, 0, 8);
            snx_st_le16(bytes, (uint16_t)(info.srWindow.Bottom - info.srWindow.Top + 1));
            snx_st_le16(bytes + 2, (uint16_t)(info.srWindow.Right - info.srWindow.Left + 1));
#else
            struct winsize w;
            if (ioctl(f->host, TIOCGWINSZ, &w)) return fail();
            snx_st_le16(bytes, w.ws_row);
            snx_st_le16(bytes + 2, w.ws_col);
            snx_st_le16(bytes + 4, w.ws_xpixel);
            snx_st_le16(bytes + 6, w.ws_ypixel);
#endif
            return gnu_memory_write(memory, argument, bytes, 8);
        }
        case 0x541B: { /* FIONREAD */
            int available = 0;
#ifdef _WIN32
            if (f->host_type == 0010000) {
                DWORD n = 0;
                if (!PeekNamedPipe((HANDLE)_get_osfhandle(f->host), NULL, 0, NULL, &n, NULL)) return -GNU_EIO;
                available = (int)n;
            } else {
                __int64 at = _lseeki64(f->host, 0, SEEK_CUR), end = _filelengthi64(f->host);
                available = at >= 0 && end > at ? (int)(end - at) : 0;
            }
#else
            if (ioctl(f->host, FIONREAD, &available)) return fail();
#endif
            snx_st_le32(bytes, (uint32_t)available);
            return gnu_memory_write(memory, argument, bytes, 4);
        }
    }
    return -GNU_ENOTTY;
}

void gnu_hostfs_release(GnuOpen* f) {
    if (f->host >= 0) close(f->host);
    free(f->path);
    free(f->host_path);
    free(f->entries);
}

/* ---- the path syscalls ---- */

static int read_path(GnuMemory* memory, uint64_t address, char* path) {
    return gnu_memory_read_string(memory, address, path, GNU_VFS_PATH_MAX);
}

static int path_stat(GnuHostPath* at, GnuStat* st) {
    HostStat h;
    if (host_lstat(at->host, &h)) return fail();
    from_host(&h, st);
    return 0;
}

static int64_t host_statfs(GnuMemory* memory, const char* host, uint64_t address) {
    unsigned char bytes[120] = {0};
#ifdef _WIN32
    {
        ULARGE_INTEGER available, total, free_bytes;
        wchar_t* w = wide(host);
        BOOL ok = w && GetDiskFreeSpaceExW(w, &available, &total, &free_bytes);
        free(w);
        if (!ok) return -GNU_ENOENT;
        snx_st_le64(bytes + 8, 4096);
        snx_st_le64(bytes + 16, total.QuadPart / 4096);
        snx_st_le64(bytes + 24, free_bytes.QuadPart / 4096);
        snx_st_le64(bytes + 32, available.QuadPart / 4096);
        snx_st_le64(bytes + 64, 255);
        snx_st_le64(bytes + 72, 4096);
    }
#else
    {
        struct statvfs v;
        if (statvfs(host, &v)) return fail();
        snx_st_le64(bytes + 8, (uint64_t)v.f_bsize);
        snx_st_le64(bytes + 16, (uint64_t)v.f_blocks);
        snx_st_le64(bytes + 24, (uint64_t)v.f_bfree);
        snx_st_le64(bytes + 32, (uint64_t)v.f_bavail);
        snx_st_le64(bytes + 40, (uint64_t)v.f_files);
        snx_st_le64(bytes + 48, (uint64_t)v.f_ffree);
        snx_st_le64(bytes + 64, (uint64_t)v.f_namemax);
        snx_st_le64(bytes + 72, (uint64_t)v.f_frsize);
    }
#endif
    snx_st_le64(bytes, 0xEF53); /* f_type: ext4's magic, what a Linux program expects of a disk */
    return gnu_memory_write(memory, address, bytes, sizeof bytes);
}

static int64_t host_utimens(GnuMemory* memory, const char* host, uint64_t times) {
    int64_t t[4];
    if (times && gnu_memory_read(memory, times, t, sizeof t)) return -GNU_EFAULT;
    for (unsigned i = 0; i < 4; i++) t[i] = (int64_t)snx_le64((uint64_t)t[i]);
#ifdef _WIN32
    {
        struct __utimbuf64 u;
        wchar_t* w = wide(host);
        int r;
        u.actime = times ? t[0] : _time64(NULL);
        u.modtime = times ? t[2] : _time64(NULL);
        r = w ? _wutime64(w, times ? &u : NULL) : -1;
        free(w);
        return r ? fail() : 0;
    }
#else
    {
        struct timespec ts[2];
        for (int i = 0; i < 2 && times; ++i) {
            ts[i].tv_sec = (time_t)t[i * 2];
            /* UTIME_NOW and UTIME_OMIT are Linux's values; other hosts have their own */
            ts[i].tv_nsec = t[i * 2 + 1] == 0x3fffffff   ? UTIME_NOW
                            : t[i * 2 + 1] == 0x3ffffffe ? UTIME_OMIT
                                                         : (long)t[i * 2 + 1];
        }
        return utimensat(AT_FDCWD, host, times ? ts : NULL, AT_SYMLINK_NOFOLLOW) ? fail() : 0;
    }
#endif
}

static int host_mkdir(const char* host, unsigned mode) {
#ifdef _WIN32
    wchar_t* w = wide(host);
    int r = w ? _wmkdir(w) : -1;
    (void)mode;
    free(w);
#else
    int r = mkdir(host, (mode_t)(mode & 07777));
#endif
    return r ? fail() : 0;
}

static int host_remove(const char* host, int folder) {
#ifdef _WIN32
    wchar_t* w = wide(host);
    int r = w ? (folder ? _wrmdir(w) : _wunlink(w)) : -1;
    free(w);
#else
    int r = folder ? rmdir(host) : unlink(host);
#endif
    return r ? fail() : 0;
}

static int host_rename(const char* from, const char* to) {
#ifdef _WIN32
    wchar_t *a = wide(from), *b = wide(to);
    BOOL ok = a && b && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
    free(a);
    free(b);
    return ok ? 0 : -GNU_EACCES;
#else
    return rename(from, to) ? fail() : 0;
#endif
}

/* two paths (rename, link): both must be on the host */
static int two_paths(GnuVfsState* s, GnuMemory* memory, uint64_t dirfd1, uint64_t path1, unsigned flags1,
                     uint64_t dirfd2, uint64_t path2, GnuHostPath* from, GnuHostPath* to) {
    char path[GNU_VFS_PATH_MAX];
    int error;
    if ((error = read_path(memory, path1, path)) || (error = gnu_hostfs_resolve(s, (int)dirfd1, path, flags1, from)))
        return error;
    if ((error = read_path(memory, path2, path)) ||
        (error = gnu_hostfs_resolve(s, (int)dirfd2, path, GNU_HOST_CREATE, to)))
        return error;
    if (from->is_virtual || to->is_virtual) return -GNU_EXDEV;
    return 0;
}

bool gnu_hostfs_syscall(GnuVfsState* s, GnuMemory* memory, uint64_t number, const uint64_t a[6],
                        char virtual_path[GNU_VFS_PATH_MAX], int64_t* result) {
    char path[GNU_VFS_PATH_MAX];
    GnuHostPath* at;
    GnuStat st;
    int error = 0;
    unsigned flags = 0;
    uint64_t path_address = a[1];
    int dirfd = (int)a[0];
    virtual_path[0] = 0;
    switch (number) {
        case 17: { /* getcwd */
            size_t n = strlen(s->cwd_path) + 1;
            if (n > a[1])
                *result = -GNU_ERANGE;
            else
                *result = gnu_memory_write(memory, a[0], s->cwd_path, n) ? -GNU_EFAULT : (int64_t)n;
            return true;
        }
        case 36: /* symlinkat(target, newdirfd, linkpath) */
            path_address = a[2];
            dirfd = (int)a[1];
            flags = GNU_HOST_CREATE;
            break;
        case 37:  /* linkat */
        case 38:  /* renameat */
        case 276: /* renameat2 */
            break;
        case 49: /* chdir */
            path_address = a[0];
            dirfd = GNU_AT_FDCWD;
            flags = GNU_HOST_FOLLOW;
            break;
        case 43: /* statfs */
        case 45: /* truncate */
            path_address = a[0];
            dirfd = GNU_AT_FDCWD;
            flags = GNU_HOST_FOLLOW;
            break;
        case 34: flags = GNU_HOST_CREATE; break; /* mkdirat */
        case 35:                                 /* unlinkat */
        case 78: break;                          /* readlinkat */
        case 48:                                 /* faccessat */
        case 53:                                 /* fchmodat */
        case 452: flags = GNU_HOST_FOLLOW; break; /* fchmodat2 */
        case 439:                                 /* faccessat2 */
        case 54:                                  /* fchownat */
        case 79:                                  /* newfstatat */
            flags = (a[number == 439 ? 3 : number == 54 ? 4 : 3] & GNU_AT_SYMLINK_NOFOLLOW) ? 0 : GNU_HOST_FOLLOW;
            break;
        case 291: flags = (a[2] & GNU_AT_SYMLINK_NOFOLLOW) ? 0 : GNU_HOST_FOLLOW; break; /* statx */
        case 88:                                                                       /* utimensat */
            if (!a[1]) return false; /* futimens: the descriptor's */
            flags = (a[3] & GNU_AT_SYMLINK_NOFOLLOW) ? 0 : GNU_HOST_FOLLOW;
            break;
        default: return false;
    }
    if (number == 37 || number == 38 || number == 276) {
        GnuHostPath* pair = (GnuHostPath*)malloc(2 * sizeof *pair);
        if (!pair) {
            *result = -GNU_ENOMEM;
            return true;
        }
        error = two_paths(s, memory, a[0], a[1], number == 37 && (a[4] & 0x400) ? GNU_HOST_FOLLOW : 0, a[2], a[3],
                          &pair[0], &pair[1]);
        if (!error && !pair[0].exists) error = -GNU_ENOENT;
        if (!error && number == 276 && (a[4] & ~1ull)) error = -GNU_EINVAL; /* RENAME_NOREPLACE only */
        if (!error && (number == 37 || (number == 276 && (a[4] & 1))) && pair[1].exists) error = -GNU_EEXIST;
        if (!error && number == 37) {
#ifdef _WIN32
            wchar_t *from = wide(pair[0].host), *to = wide(pair[1].host);
            if (!from || !to || !CreateHardLinkW(to, from, NULL)) error = -GNU_EPERM;
            free(from);
            free(to);
#else
            if (link(pair[0].host, pair[1].host)) error = fail();
#endif
        } else if (!error)
            error = host_rename(pair[0].host, pair[1].host);
        free(pair);
        *result = error;
        return true;
    }
    if ((error = read_path(memory, path_address, path))) {
        *result = error;
        return true;
    }
    if (!path[0]) {
        /* AT_EMPTY_PATH names the descriptor itself, the working folder for AT_FDCWD. Otherwise the virtual nodes'
           code says what an empty name is */
        if (dirfd != GNU_AT_FDCWD || (number != 79 && number != 291) ||
            !(a[number == 79 ? 3 : 2] & GNU_AT_EMPTY_PATH))
            return false;
        strcpy(path, ".");
    }
    at = (GnuHostPath*)malloc(sizeof *at);
    if (!at) {
        *result = -GNU_ENOMEM;
        return true;
    }
    error = gnu_hostfs_resolve(s, dirfd, path, flags, at);
    if (!error && at->is_virtual) {
        strcpy(virtual_path, at->guest);
        free(at);
        if (number == 36) {
            *result = -GNU_EROFS;
            return true;
        }
        return false;
    }
    if (error) {
        *result = error;
        free(at);
        return true;
    }
    switch (number) {
        case 34: error = at->exists ? -GNU_EEXIST : host_mkdir(at->host, (unsigned)a[2]); break;
        case 35:
            if (a[2] & ~(uint64_t)GNU_AT_REMOVEDIR)
                error = -GNU_EINVAL;
            else if (!at->exists)
                error = -GNU_ENOENT;
            else if (a[2] & GNU_AT_REMOVEDIR)
                error = at->type != 0040000 ? -GNU_ENOTDIR : host_remove(at->host, 1);
            else
                error = at->type == 0040000 ? -GNU_EISDIR : host_remove(at->host, 0);
            break;
        case 36: {
            char target[GNU_VFS_PATH_MAX];
            if (at->exists)
                error = -GNU_EEXIST;
            else if (!(error = read_path(memory, a[0], target))) {
#ifdef _WIN32
                error = -GNU_EPERM;
#else
                if (symlink(target, at->host)) error = fail();
#endif
            }
            break;
        }
        case 43: error = (int)host_statfs(memory, at->host, a[1]); break;
        case 45:
            if ((int64_t)a[1] < 0)
                error = -GNU_EINVAL;
            else if (at->type == 0040000)
                error = -GNU_EISDIR;
            else {
#ifdef _WIN32
                wchar_t* w = wide(at->host);
                int fd = w ? _wopen(w, _O_WRONLY | _O_BINARY) : -1;
                free(w);
                if (fd < 0 || _chsize_s(fd, (__int64)a[1])) error = -GNU_EACCES;
                if (fd >= 0) _close(fd);
#else
                if (truncate(at->host, (off_t)a[1])) error = fail();
#endif
            }
            break;
        case 48:
        case 439:
            if (a[2] & ~7ull)
                error = -GNU_EINVAL;
            else {
#ifdef _WIN32
                wchar_t* w = wide(at->host);
                if (!w || _waccess(w, (int)(a[2] & 6))) error = fail();
                free(w);
#else
                if (access(at->host, (int)a[2])) error = fail();
#endif
            }
            break;
        case 49:
            if (at->type != 0040000)
                error = -GNU_ENOTDIR;
            else {
                char* copy = duplicate(at->guest);
                if (!copy)
                    error = -GNU_ENOMEM;
                else {
                    free(s->cwd_path);
                    s->cwd_path = copy;
                }
            }
            break;
        case 53:
        case 452: {
#ifdef _WIN32
            wchar_t* w = wide(at->host);
            if (!w || _wchmod(w, (a[2] & 0200) ? _S_IREAD | _S_IWRITE : _S_IREAD)) error = fail();
            free(w);
#else
            if (chmod(at->host, (mode_t)(a[2] & 07777))) error = fail();
#endif
            break;
        }
        case 54:
#ifndef _WIN32
            if (lchown(at->host, (uid_t)(uint32_t)a[2], (gid_t)(uint32_t)a[3])) error = fail();
#endif
            break;
        case 78: {
            char target[GNU_VFS_PATH_MAX];
            int n;
            if (!a[3]) {
                error = -GNU_EINVAL;
                break;
            }
            if (at->type != 0120000) {
                error = -GNU_EINVAL;
                break;
            }
            n = host_readlink(at->host, target, sizeof target);
            if (n < 0) {
                error = n;
                break;
            }
            if ((uint64_t)n > a[3]) n = (int)a[3];
            free(at);
            *result = gnu_memory_write(memory, a[2], target, (size_t)n) ? -GNU_EFAULT : n;
            return true;
        }
        case 79:
            if (a[3] & ~(uint64_t)(GNU_AT_SYMLINK_NOFOLLOW | GNU_AT_EMPTY_PATH | 2048u))
                error = -GNU_EINVAL;
            else if (!(error = path_stat(at, &st)))
                error = gnu_stat_put(memory, a[2], &st);
            break;
        case 291:
            if (!(error = path_stat(at, &st))) error = gnu_statx_put(memory, a[4], &st);
            break;
        case 88: error = (int)host_utimens(memory, at->host, a[2]); break;
    }
    free(at);
    *result = error;
    return true;
}

/* the descriptor calls of a host file: fstatfs, fchmod, fchown, fsync, fdatasync, futimens */
int64_t gnu_hostfs_fd_call(GnuOpen* f, GnuMemory* memory, uint64_t number, const uint64_t a[6]) {
    switch (number) {
        case 44: return f->host_path ? host_statfs(memory, f->host_path, a[1]) : -GNU_EINVAL;
        case 52:
#ifdef _WIN32
            return 0;
#else
            if (f->host >= 0 ? fchmod(f->host, (mode_t)(a[1] & 07777))
                             : f->host_path ? chmod(f->host_path, (mode_t)(a[1] & 07777)) : (errno = EBADF, -1))
                return fail();
            return 0;
#endif
        case 55:
#ifdef _WIN32
            return 0;
#else
            if (f->host >= 0 ? fchown(f->host, (uid_t)(uint32_t)a[1], (gid_t)(uint32_t)a[2])
                             : f->host_path ? lchown(f->host_path, (uid_t)(uint32_t)a[1], (gid_t)(uint32_t)a[2])
                                            : (errno = EBADF, -1))
                return fail();
            return 0;
#endif
        case 82:
        case 83:
            if (f->host < 0) return f->path ? 0 : -GNU_EINVAL;
#ifdef _WIN32
            return _commit(f->host) ? fail() : 0;
#else
            return fsync(f->host) ? fail() : 0;
#endif
        case 88: return f->host_path ? host_utimens(memory, f->host_path, a[2]) : -GNU_EINVAL;
    }
    return -GNU_ENOSYS;
}
