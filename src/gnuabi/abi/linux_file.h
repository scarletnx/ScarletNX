/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Linux's file ABI against the host's, for each runtime whose guest is Linux (the GNU ABI's VFS, the Android bridge,
   whose bionic has Linux's numbers): the asm-generic struct stat that AArch64 has (128 bytes), a host stat as it,
   and the open flags (asm-generic, AArch64's) as the host's and back. Only bytes and host types: no guest memory */
#ifndef SNX_GNU_ABI_LINUX_FILE_H
#define SNX_GNU_ABI_LINUX_FILE_H
#include "common/utilities/endian.h"
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

/* What a stat call reports, written out as the asm-generic stat (or as statx by the VFS) */
typedef struct GnuStat {
    uint64_t dev, inode, rdev, size, blocks;
    uint32_t mode, nlink, uid, gid, blksize;
    int64_t atime, mtime, ctime;
    uint32_t atime_ns, mtime_ns, ctime_ns;
} GnuStat;

/* the guest's struct stat, 128 bytes */
static inline void gnu_stat_encode(unsigned char bytes[128], const GnuStat* st) {
    for (int i = 0; i < 128; ++i) bytes[i] = 0;
    snx_st_le64(bytes, st->dev);
    snx_st_le64(bytes + 8, st->inode);
    snx_st_le32(bytes + 16, st->mode);
    snx_st_le32(bytes + 20, st->nlink);
    snx_st_le32(bytes + 24, st->uid);
    snx_st_le32(bytes + 28, st->gid);
    snx_st_le64(bytes + 32, st->rdev);
    snx_st_le64(bytes + 48, st->size);
    snx_st_le32(bytes + 56, st->blksize);
    snx_st_le64(bytes + 64, st->blocks);
    snx_st_le64(bytes + 72, (uint64_t)st->atime);
    snx_st_le64(bytes + 80, st->atime_ns);
    snx_st_le64(bytes + 88, (uint64_t)st->mtime);
    snx_st_le64(bytes + 96, st->mtime_ns);
    snx_st_le64(bytes + 104, (uint64_t)st->ctime);
    snx_st_le64(bytes + 112, st->ctime_ns);
}

/* the host's struct stat: Windows' 64-bit one */
#ifdef _WIN32
typedef struct _stat64 GnuHostStat;
#else
typedef struct stat GnuHostStat;
#endif

/* a host st_mode's file type as Linux's S_IFMT bits */
static inline unsigned gnu_host_file_type(unsigned mode) {
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

static inline void gnu_stat_from_host(const GnuHostStat* h, GnuStat* st) {
    *st = (GnuStat){0};
    st->dev = (uint64_t)h->st_dev;
    st->inode = (uint64_t)h->st_ino;
    st->mode = gnu_host_file_type((unsigned)h->st_mode) | ((unsigned)h->st_mode & 07777);
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

/* the guest's open flags (asm-generic, which AArch64 uses) */
enum {
    GNU_O_ACCMODE = 3,
    GNU_O_WRONLY = 1,
    GNU_O_RDWR = 2,
    GNU_O_CREAT = 64,
    GNU_O_EXCL = 128,
    GNU_O_NOCTTY = 256,
    GNU_O_TRUNC = 512,
    GNU_O_APPEND = 1024,
    GNU_O_NONBLOCK = 2048,
    GNU_O_DSYNC = 4096,
    GNU_O_DIRECTORY = 16384,
    GNU_O_NOFOLLOW = 32768,
    GNU_O_LARGEFILE = 131072,
    GNU_O_CLOEXEC = 524288,
    GNU_O_SYNC = 1052672,
    GNU_O_PATH = 2097152,
};

/* the guest's open flags as the host's, each that the host has; the others are left out (O_LARGEFILE is implied on a
   64-bit host). Windows' are its CRT's: the access mode, O_CREAT, O_EXCL, O_TRUNC and O_APPEND */
static inline int gnu_open_flags_to_host(unsigned flags) {
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
#ifndef _WIN32
    if (flags & GNU_O_NOCTTY) h |= O_NOCTTY;
    if (flags & GNU_O_NONBLOCK) h |= O_NONBLOCK;
    if (flags & GNU_O_NOFOLLOW) h |= O_NOFOLLOW;
    if (flags & GNU_O_DIRECTORY) h |= O_DIRECTORY;
    if (flags & GNU_O_CLOEXEC) h |= O_CLOEXEC;
#ifdef O_SYNC
    if ((flags & GNU_O_SYNC) == GNU_O_SYNC) h |= O_SYNC;
    else
#endif
#ifdef O_DSYNC
    if (flags & GNU_O_DSYNC) h |= O_DSYNC;
#endif
#endif
    return h;
}

/* the host's status flags (fcntl's F_GETFL) as the guest's */
static inline unsigned gnu_open_flags_from_host(int h) {
    unsigned flags = (h & O_ACCMODE) == O_WRONLY ? GNU_O_WRONLY : (h & O_ACCMODE) == O_RDWR ? GNU_O_RDWR : 0;
    if (h & O_APPEND) flags |= GNU_O_APPEND;
#ifndef _WIN32
    if (h & O_NONBLOCK) flags |= GNU_O_NONBLOCK;
#ifdef O_SYNC
    if ((h & O_SYNC) == O_SYNC) flags |= GNU_O_SYNC;
#endif
#endif
    return flags | GNU_O_LARGEFILE;
}
#endif
