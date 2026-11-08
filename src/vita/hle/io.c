/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SceIofilemgr and SceLibKernel's sceIo*: the game's files. app0: is the application's folder as the VPK holds it (the
   game folder's app/, with the update's files in patch/ over it), read-only on the console and here; ux0: the memory card, the game folder's ux0/, which the game
   writes its data to (ux0:data/...); savedata0: the game's save data, mounted for it as the console mounts it
   (VitaProcess.savedata, in ux0/). A path leaving its device ("..") is refused */
#if !defined(_WIN32) && !defined(_FILE_OFFSET_BITS)
#define _FILE_OFFSET_BITS 64
#endif
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE /* fseeko */
#endif
#include "vita/hle/hle.h"
#include "vita/hle/psarc.h"
#include "vita/runtime/watch.h"
#include "common/utilities/endian.h"
#include "common/utilities/common.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define SCE_O_RDONLY 0x0001u
#define SCE_O_WRONLY 0x0002u
#define SCE_O_APPEND 0x0100u
#define SCE_O_CREAT  0x0200u
#define SCE_O_TRUNC  0x0400u
#define SCE_O_EXCL   0x0800u
#define ENOENT 2u
#define EBADF  9u
#define EACCES 13u
#define EEXIST 17u
#define ENOTDIR 20u
#define EISDIR 21u
#define EINVAL 22u
#define EMFILE 24u

typedef struct {
    char** names;
    size_t count, next;
    char* path; /* the folder's host path, for each entry's stat */
} Directory;

/* the archives mounted (sceFiosArchiveMountSync) and the files open in them: a handle of an archive's file is
   ARCHIVE_FH plus its place here. sceIoOpen and fopen check this first, same as the FIOS2 calls do, since a file
   the game expects to find by its plain path may live only inside a mounted .psarc */
#define ARCHIVE_MOUNTS 16
#define ARCHIVE_FILES 64
#define ARCHIVE_FH 0x10000u
#define ARCHIVE_MOUNT_FH 0x20000u
/* the console's ttys (tty0:, sdtty0:, ...): pseudo-descriptors, one per device. libc.suprx's openStdio opens tty0:
   for stdin, tty1: for stdout and tty2: for stderr, and refuses to initialize stdio if any of them fails. Placed
   above ARCHIVE_MOUNT_FH so no descriptor space overlaps, and above VITA_FILES so a tty fd never matches a real
   FILE* slot. 10 devices covers tty0: through tty9: (and their sdtty: forms) */
#define TTY_FH 0x30000u
#define TTY_COUNT 10u
static struct {
    VitaPsarc* archive;
    char point[256]; /* the mount point, without its device and leading slashes */
} archive_mounts[ARCHIVE_MOUNTS];
static struct {
    VitaPsarc* archive;
    int entry;
    uint64_t position;
} archive_files[ARCHIVE_FILES];
static const char* strip_device(const char* name) {
    const char* colon = strchr(name, ':');
    if (colon) name = colon + 1;
    while (*name == '/') ++name;
    return name;
}
static bool equal_n(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}
/* the archive entry a path names: under a mount point, else by the path as the archive's own */
static bool archive_lookup(const char* name, VitaPsarc** archive, int* entry) {
    const char* n = strip_device(name);
    for (unsigned i = 0; i < ARCHIVE_MOUNTS; ++i) {
        VitaPsarc* a = archive_mounts[i].archive;
        if (!a) continue;
        size_t length = strlen(archive_mounts[i].point);
        int e = -1;
        if (!length)
            e = vita_psarc_find(a, n);
        else if (strlen(n) > length && n[length] == '/' && equal_n(n, archive_mounts[i].point, length))
            e = vita_psarc_find(a, n + length + 1);
        if (e < 0) e = vita_psarc_find(a, n);
        if (e >= 0) {
            *archive = a, *entry = e;
            return true;
        }
    }
    return false;
}
static int archive_slot(uint32_t fh) {
    return fh >= ARCHIVE_FH && fh < ARCHIVE_FH + ARCHIVE_FILES && archive_files[fh - ARCHIVE_FH].archive
               ? (int)(fh - ARCHIVE_FH)
               : -1;
}
/* The tty device number a path names (0 through 9), or -1 for a name that isn't one. Both "ttyN:" and "sdttyN:" are
   recognized, matching what the Vita's libc and libsmart open at init */
static int tty_device(const char* name) {
    const char* p = name;
    if (!strncmp(p, "sdtty", 5))
        p += 5;
    else if (!strncmp(p, "tty", 3))
        p += 3;
    else
        return -1;
    return p[0] >= '0' && p[0] <= '9' && p[1] == ':' ? p[0] - '0' : -1;
}
/* the tty pseudo-descriptor fd's device, or -1 for a non-tty fd */
static int tty_slot(uint32_t fd) {
    return fd >= TTY_FH && fd < TTY_FH + TTY_COUNT ? (int)(fd - TTY_FH) : -1;
}

/* guest path to host path, the caller's to free; NULL for a device or path the runtime doesn't give, *writable for ux0: */
static char* host_path_of(VitaProcess* p, char* path, bool* writable) {
    char* colon = strchr(path, ':');
    if (!colon) return NULL;
    *colon = 0;
    const char* root;
    bool app = false;
    *writable = false;
    if (!strcmp(path, "app0"))
        root = p->app, app = true;
    else if (!strcmp(path, "ux0") || !strcmp(path, "uma0")) {
        root = p->ux0;
        *writable = true;
    } else if (!strcmp(path, "savedata0")) {
        root = p->savedata;
        *writable = true;
    } else
        return NULL;
    const char* rest = colon + 1;
    while (*rest == '/') ++rest;
    /* each part relative and not ".." */
    for (const char* q = rest; *q;) {
        const char* slash = strchr(q, '/');
        size_t n = slash ? (size_t)(slash - q) : strlen(q);
        if (n == 2 && q[0] == '.' && q[1] == '.') return NULL;
        if (!slash) break;
        q = slash + 1;
    }
    if (!*rest) return snx_strdup(root);
    /* the update's file or folder, when it has the path */
    char* patched = app ? snx_path_join(p->patch, rest) : NULL;
    if (patched && snx_path_exists(patched)) return patched;
    free(patched);
    return snx_path_join(root, rest);
}
static char* host_path(VitaProcess* p, uint32_t address, bool* writable) {
    char path[1024];
    return vita_string(p, address, path, sizeof path) ? host_path_of(p, path, writable) : NULL;
}
char* vita_io_host_path(VitaProcess* p, uint32_t address) {
    bool writable;
    return host_path(p, address, &writable);
}
static int file_index(VitaProcess* p, uint32_t fd, bool directory) {
    if (fd < 1 || fd > VITA_FILES || !p->files[fd - 1].host || p->files[fd - 1].directory != directory) return -1;
    return (int)fd - 1;
}
static uint32_t new_file(VitaProcess* p, void* host, int directory) {
    for (unsigned i = 0; i < VITA_FILES; ++i)
        if (!p->files[i].host) {
            p->files[i] = (VitaFile){host, directory};
            return i + 1;
        }
    return 0;
}
static int io_seek(FILE* f, int64_t offset, int whence) {
#ifdef _WIN32
    return _fseeki64(f, offset, whence);
#else
    return fseeko(f, (off_t)offset, whence);
#endif
}
static int64_t io_tell(FILE* f) {
#ifdef _WIN32
    return _ftelli64(f);
#else
    return (int64_t)ftello(f);
#endif
}
static void close_directory(Directory* d) {
    snx_free_strings(d->names, d->count);
    free(d->path);
    free(d);
}

static uint32_t open_host(VitaProcess* p, char* path, bool writable, uint32_t flags) {
    bool write = (flags & SCE_O_WRONLY) || (flags & (SCE_O_CREAT | SCE_O_TRUNC | SCE_O_APPEND));
    uint32_t result;
    if (snx_is_dir(path))
        result = SCE_ERRNO(EISDIR);
    else if (write && !writable)
        result = SCE_ERRNO(EACCES);
    else if ((flags & SCE_O_EXCL) && (flags & SCE_O_CREAT) && snx_path_exists(path))
        result = SCE_ERRNO(EEXIST);
    else if (!write && !snx_is_file(path))
        result = SCE_ERRNO(ENOENT);
    else {
        bool exists = snx_is_file(path);
        if (write && !exists && !(flags & SCE_O_CREAT)) {
            free(path);
            return SCE_ERRNO(ENOENT);
        }
        const char* mode = !write                  ? "rb"
                           : (flags & SCE_O_TRUNC) || !exists ? ((flags & SCE_O_RDONLY) ? "w+b" : "wb")
                           : (flags & SCE_O_APPEND) ? "ab"
                                                    : "r+b";
        FILE* f = snx_fopen(path, mode);
        result = !f ? SCE_ERRNO(ENOENT) : new_file(p, f, 0);
        if (f && !result) {
            fclose(f);
            result = SCE_ERRNO(EMFILE);
        }
    }
    free(path);
    return result;
}
/* a free archive_files slot for a path found in a mounted archive, or 0 if none: ARCHIVE_FH plus the slot, the same
   descriptor space sceFiosFHOpenSync hands out, so vita_io_file and archive_slot both recognize it */
static uint32_t archive_open(const char* name) {
    VitaPsarc* archive;
    int entry;
    if (!archive_lookup(name, &archive, &entry)) return 0;
    for (unsigned i = 0; i < ARCHIVE_FILES; ++i) {
        if (archive_files[i].archive) continue;
        archive_files[i] = (typeof(archive_files[i])){archive, entry, 0};
        return ARCHIVE_FH + i;
    }
    return 0;
}
uint32_t vita_io_open(VitaProcess* p, uint32_t address, uint32_t flags) {
    char debug_name[512] = "<unreadable>";
    vita_string(p, address, debug_name, sizeof debug_name);
    uint32_t result;
    /* the console's ttys first: libc.suprx opens tty0: (stdin), tty1: (stdout), then tty2: (stderr) if the first
       succeeded, and stops if any fails. They are not files, so they get pseudo-descriptors and bypass both the
       archive lookup and host path resolution. Done before the archive path since tty0: is often opened read-only and
       would otherwise fall into the archive lookup where no archive holds a "tty0:" entry */
    int tty = tty_device(debug_name);
    if (tty >= 0) {
        result = TTY_FH + (uint32_t)tty;
        printf("vita: sceIoOpen(\"%s\", flags 0x%x) -> 0x%x (tty)\n", debug_name, flags, result);
        return result;
    }
    if ((flags & SCE_O_RDONLY) == SCE_O_RDONLY && !(flags & (SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC))) {
        char name[1100];
        if (vita_string(p, address, name, sizeof name)) {
            char full[1100];
            snprintf(full, sizeof full, strchr(name, ':') ? "%s" : "app0:%s", name);
            uint32_t fh = archive_open(full);
            if (fh) return fh;
        }
    }
    bool writable;
    char* path = host_path(p, address, &writable);
    result = path ? open_host(p, path, writable, flags) : SCE_ERRNO(ENOENT);
    printf("vita: sceIoOpen(\"%s\", flags 0x%x) -> 0x%x\n", debug_name, flags, result);
    return result;
}
static uint32_t io_open(VitaProcess* p, VitaThread* t) { return vita_io_open(p, vita_arg(p, t, 0), vita_arg(p, t, 1)); }
FILE* vita_io_file(VitaProcess* p, uint32_t fd) {
    int i = file_index(p, fd, false);
    return i < 0 ? NULL : p->files[i].host;
}
uint32_t vita_io_close(VitaProcess* p, uint32_t fd) {
    if (tty_slot(fd) >= 0) return 0; /* a tty pseudo-descriptor: nothing to close, the host's console stays open */
    int slot = archive_slot(fd);
    if (slot >= 0) {
        archive_files[slot].archive = NULL;
        return 0;
    }
    int i = file_index(p, fd, false);
    if (i < 0) return SCE_ERRNO(EBADF);
    fclose(p->files[i].host);
    p->files[i] = (VitaFile){0};
    return 0;
}
static uint32_t io_close(VitaProcess* p, VitaThread* t) { return vita_io_close(p, vita_arg(p, t, 0)); }
static uint32_t io_read(VitaProcess* p, VitaThread* t) {
    uint32_t fd = vita_arg(p, t, 0), buffer = vita_arg(p, t, 1), size = vita_arg(p, t, 2);
    /* a tty has no real input here: EOF (0), as a terminal with nothing typed. tty0: is stdin, and its read returning
       0 lets libc see end-of-file rather than an error the game would report as "cannot initialize" */
    if (tty_slot(fd) >= 0) return 0;
    int slot = archive_slot(fd);
    if (slot >= 0) {
        if (!size) return 0;
        if (!vita_mapped(p, buffer, size)) return SCE_ERRNO(EINVAL);
        size_t got = 0;
        vita_watch_release(buffer, size);
        if (!vita_psarc_read(archive_files[slot].archive, archive_files[slot].entry, archive_files[slot].position,
                             vita_ptr(p, buffer), size, &got))
            return SCE_ERRNO(EINVAL);
        archive_files[slot].position += got;
        return (uint32_t)got;
    }
    int i = file_index(p, fd, false);
    if (i < 0) return SCE_ERRNO(EBADF);
    if (!size) return 0;
    if (!vita_mapped(p, buffer, size)) return SCE_ERRNO(EINVAL);
    vita_watch_release(buffer, size); /* the system writes it: a watched page would refuse it (EFAULT) */
    return (uint32_t)fread(vita_ptr(p, buffer), 1, size, p->files[i].host);
}
static uint32_t io_write(VitaProcess* p, VitaThread* t) {
    uint32_t fd = vita_arg(p, t, 0), buffer = vita_arg(p, t, 1), size = vita_arg(p, t, 2);
    int tty = tty_slot(fd);
    if (tty >= 0) {
        /* tty2: is stderr, everything else is stdout. Written straight to the host's console, the same place the
           "newlib's stdout and stderr" fallback below puts a game's own printf */
        if (!size) return 0;
        if (!vita_mapped(p, buffer, size)) return SCE_ERRNO(EINVAL);
        FILE* out = tty == 2 ? stderr : stdout;
        size_t wrote = fwrite(vita_ptr(p, buffer), 1, size, out);
        fflush(out);
        return (uint32_t)wrote;
    }
    /* a mounted archive is read-only: refuse it here instead of letting the fd fall through to the console's stdout
       path below (an archive fd is >= ARCHIVE_FH, which is > VITA_FILES) */
    if (archive_slot(fd) >= 0) return SCE_ERRNO(EACCES);
    if (!size) return 0;
    if (!vita_mapped(p, buffer, size)) return SCE_ERRNO(EINVAL);
    /* the newlib's stdout and stderr (descriptors the runtime never opens): the game's log */
    int i = file_index(p, fd, false);
    if (i < 0) {
        if (fd > VITA_FILES) {
            fwrite(vita_ptr(p, buffer), 1, size, stdout);
            fflush(stdout);
            return size;
        }
        return SCE_ERRNO(EBADF);
    }
    return (uint32_t)fwrite(vita_ptr(p, buffer), 1, size, p->files[i].host);
}
/* sceIoPread(fd, buffer, size, SceOff offset): the offset in the stack's first two words; the file's position kept */
static uint32_t io_pread(VitaProcess* p, VitaThread* t) {
    uint32_t fd = vita_arg(p, t, 0), buffer = vita_arg(p, t, 1), size = vita_arg(p, t, 2);
    int64_t offset = (int64_t)((uint64_t)vita_arg(p, t, 4) | (uint64_t)vita_arg(p, t, 5) << 32);
    if (tty_slot(fd) >= 0) return 0; /* a tty has no position: as if past the end */
    int slot = archive_slot(fd);
    if (slot >= 0) {
        if (!size) return 0;
        if (offset < 0 || !vita_mapped(p, buffer, size)) return SCE_ERRNO(EINVAL);
        size_t got = 0;
        vita_watch_release(buffer, size);
        if (!vita_psarc_read(archive_files[slot].archive, archive_files[slot].entry, (uint64_t)offset,
                             vita_ptr(p, buffer), size, &got))
            return SCE_ERRNO(EINVAL);
        return (uint32_t)got;
    }
    int i = file_index(p, fd, false);
    if (i < 0) return SCE_ERRNO(EBADF);
    if (!size) return 0;
    if (!vita_mapped(p, buffer, size)) return SCE_ERRNO(EINVAL);
    FILE* f = p->files[i].host;
    int64_t was = io_tell(f);
    if (io_seek(f, offset, SEEK_SET)) return SCE_ERRNO(EINVAL);
    vita_watch_release(buffer, size);
    uint32_t got = (uint32_t)fread(vita_ptr(p, buffer), 1, size, f);
    (void)io_seek(f, was, SEEK_SET);
    return got;
}
static uint32_t io_sync_by_fd(VitaProcess* p, VitaThread* t) {
    if (tty_slot(vita_arg(p, t, 0)) >= 0) return 0;    /* nothing buffered to flush: writes go straight to the host */
    if (archive_slot(vita_arg(p, t, 0)) >= 0) return 0; /* an archive entry: nothing buffered to flush */
    int i = file_index(p, vita_arg(p, t, 0), false);
    if (i < 0) return SCE_ERRNO(EBADF);
    fflush(p->files[i].host);
    return 0;
}
/* sceIoLseek(fd, SceOff offset, whence): the offset in r2 and r3, whence on the stack */
static uint32_t io_lseek(VitaProcess* p, VitaThread* t) {
    uint32_t fd = vita_arg(p, t, 0);
    int64_t offset = (int64_t)((uint64_t)vita_arg(p, t, 2) | (uint64_t)vita_arg(p, t, 3) << 32);
    int whence = (int)vita_arg(p, t, 4);
    if (tty_slot(fd) >= 0) /* a character device has no position */
        return vita_return64(t, (uint64_t)(int64_t)(int32_t)SCE_ERRNO(EINVAL));
    int slot = archive_slot(fd);
    if (slot >= 0) {
        uint64_t size = vita_psarc_size(archive_files[slot].archive, archive_files[slot].entry);
        int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)archive_files[slot].position : (int64_t)size;
        int64_t to = base + offset;
        if (whence < 0 || whence > 2 || to < 0) return vita_return64(t, (uint64_t)(int64_t)(int32_t)SCE_ERRNO(EINVAL));
        archive_files[slot].position = (uint64_t)to;
        return vita_return64(t, (uint64_t)to);
    }
    int i = file_index(p, fd, false);
    if (i < 0) return vita_return64(t, (uint64_t)(int64_t)(int32_t)SCE_ERRNO(EBADF));
    if (whence < 0 || whence > 2 || io_seek(p->files[i].host, offset, whence))
        return vita_return64(t, (uint64_t)(int64_t)(int32_t)SCE_ERRNO(EINVAL));
    return vita_return64(t, (uint64_t)io_tell(p->files[i].host));
}
static uint32_t io_lseek32(VitaProcess* p, VitaThread* t) {
    uint32_t fd = vita_arg(p, t, 0);
    int32_t offset = (int32_t)vita_arg(p, t, 1);
    int whence = (int)vita_arg(p, t, 2);
    if (tty_slot(fd) >= 0) return SCE_ERRNO(EINVAL); /* a character device has no position */
    int slot = archive_slot(fd);
    if (slot >= 0) {
        uint64_t size = vita_psarc_size(archive_files[slot].archive, archive_files[slot].entry);
        int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)archive_files[slot].position : (int64_t)size;
        int64_t to = base + offset;
        if (whence < 0 || whence > 2 || to < 0) return SCE_ERRNO(EINVAL);
        archive_files[slot].position = (uint64_t)to;
        return (uint32_t)to;
    }
    int i = file_index(p, fd, false);
    if (i < 0) return SCE_ERRNO(EBADF);
    if (whence < 0 || whence > 2 || io_seek(p->files[i].host, offset, whence))
        return SCE_ERRNO(EINVAL);
    return (uint32_t)io_tell(p->files[i].host);
}
static uint32_t io_remove(VitaProcess* p, VitaThread* t) {
    bool writable;
    char* path = host_path(p, vita_arg(p, t, 0), &writable);
    uint32_t result = !path ? SCE_ERRNO(ENOENT) : !writable ? SCE_ERRNO(EACCES) : remove(path) ? SCE_ERRNO(ENOENT) : 0;
    free(path);
    return result;
}
static uint32_t io_mkdir(VitaProcess* p, VitaThread* t) {
    bool writable;
    char* path = host_path(p, vita_arg(p, t, 0), &writable);
    uint32_t result = !path       ? SCE_ERRNO(ENOENT)
                      : !writable ? SCE_ERRNO(EACCES)
                      : snx_path_exists(path) ? SCE_ERRNO(EEXIST)
                      : snx_mkdirs(path)      ? SCE_ERRNO(ENOENT)
                                              : 0;
    free(path);
    return result;
}
static uint32_t io_rmdir(VitaProcess* p, VitaThread* t) {
    bool writable;
    char* path = host_path(p, vita_arg(p, t, 0), &writable);
    uint32_t result = !path ? SCE_ERRNO(ENOENT)
                      : !writable ? SCE_ERRNO(EACCES)
                      : !snx_is_dir(path) ? SCE_ERRNO(ENOTDIR)
                      : snx_remove_empty_dir(path) ? SCE_ERRNO(EEXIST)
                                                   : 0;
    free(path);
    return result;
}
static uint32_t io_rename(VitaProcess* p, VitaThread* t) {
    bool from_writable, to_writable;
    char *from = host_path(p, vita_arg(p, t, 0), &from_writable), *to = host_path(p, vita_arg(p, t, 1), &to_writable);
    uint32_t result = !from || !to ? SCE_ERRNO(ENOENT)
                      : !from_writable || !to_writable ? SCE_ERRNO(EACCES)
                      : snx_replace(from, to) ? SCE_ERRNO(ENOENT)
                                              : 0;
    free(from);
    free(to);
    return result;
}
/* SceDateTime: year, month, day, hour, minute, second (16 bits each), microseconds */
static void date_time(VitaProcess* p, uint32_t at, time_t when) {
    struct tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &when);
#else
    gmtime_r(&when, &tm);
#endif
    uint16_t fields[6] = {(uint16_t)(tm.tm_year + 1900), (uint16_t)(tm.tm_mon + 1), (uint16_t)tm.tm_mday,
                          (uint16_t)tm.tm_hour,          (uint16_t)tm.tm_min,       (uint16_t)tm.tm_sec};
    unsigned char* b = vita_ptr(p, at);
    for (unsigned i = 0; i < 6; ++i) snx_st_le16(b + i * 2, fields[i]);
    vita_write32(p, at + 12, 0);
}
/* SceIoStat (88 bytes): st_mode, st_attr, st_size (64-bit, at 8), the creation, access and modification times */
static bool stat_into(VitaProcess* p, const char* path, uint32_t out) {
    struct stat s;
    if (stat(path, &s)) return false;
    bool directory = S_ISDIR(s.st_mode);
    memset(vita_ptr(p, out), 0, 88);
    vita_write32(p, out, directory ? 0x1000u | 0x1ffu : 0x2000u | 0x1b6u); /* SCE_S_IFDIR or SCE_S_IFREG, and rw */
    vita_write32(p, out + 4, directory ? 0x10u | 7u : 0x20u | 6u);          /* SCE_SO_IFDIR or SCE_SO_IFREG */
    vita_write64(p, out + 8, directory ? 0 : (uint64_t)s.st_size);
    date_time(p, out + 16, s.st_ctime);
    date_time(p, out + 32, s.st_atime);
    date_time(p, out + 48, s.st_mtime);
    return true;
}
static uint32_t io_getstat(VitaProcess* p, VitaThread* t) {
    /* try a mounted archive first, with the same device defaulting the FIOS2 path uses (app0: when the game gives no
       device); fall through to the host filesystem when nothing is mounted at that path */
    char name[1100];
    if (vita_string(p, vita_arg(p, t, 0), name, sizeof name)) {
        char full[1100];
        snprintf(full, sizeof full, strchr(name, ':') ? "%s" : "app0:%s", name);
        VitaPsarc* archive;
        int entry;
        if (archive_lookup(full, &archive, &entry)) {
            uint32_t out = vita_arg(p, t, 1);
            memset(vita_ptr(p, out), 0, 88);
            vita_write32(p, out, 0x2000u | 0x1b6u);
            vita_write32(p, out + 4, 0x20u | 6u);
            vita_write64(p, out + 8, vita_psarc_size(archive, entry));
            return 0;
        }
    }
    bool writable;
    char* path = host_path(p, vita_arg(p, t, 0), &writable);
    uint32_t result = path && stat_into(p, path, vita_arg(p, t, 1)) ? 0 : SCE_ERRNO(ENOENT);
    free(path);
    return result;
}
static uint32_t io_getstat_by_fd(VitaProcess* p, VitaThread* t) {
    uint32_t fd = vita_arg(p, t, 0), out = vita_arg(p, t, 1);
    if (tty_slot(fd) >= 0) {
        /* a character device: no size, mode SCE_S_IFCHR */
        memset(vita_ptr(p, out), 0, 88);
        vita_write32(p, out, 0x3000u | 0x1b6u);
        vita_write32(p, out + 4, 0x30u | 6u);
        time_t now = time(NULL);
        date_time(p, out + 16, now);
        date_time(p, out + 32, now);
        date_time(p, out + 48, now);
        return 0;
    }
    int slot = archive_slot(fd);
    int64_t size;
    if (slot >= 0)
        size = (int64_t)vita_psarc_size(archive_files[slot].archive, archive_files[slot].entry);
    else {
        int i = file_index(p, fd, false);
        if (i < 0) return SCE_ERRNO(EBADF);
        FILE* f = p->files[i].host;
        int64_t at = io_tell(f);
        size = io_seek(f, 0, SEEK_END) ? 0 : io_tell(f);
        io_seek(f, at, SEEK_SET);
    }
    memset(vita_ptr(p, out), 0, 88);
    vita_write32(p, out, 0x2000u | 0x1b6u);
    vita_write32(p, out + 4, 0x20u | 6u);
    vita_write64(p, out + 8, (uint64_t)size);
    time_t now = time(NULL);
    date_time(p, out + 16, now);
    date_time(p, out + 32, now);
    date_time(p, out + 48, now);
    return 0;
}
static uint32_t io_dopen(VitaProcess* p, VitaThread* t) {
    bool writable;
    char* path = host_path(p, vita_arg(p, t, 0), &writable);
    if (!path || !snx_is_dir(path)) {
        free(path);
        return SCE_ERRNO(ENOENT);
    }
    Directory* d = calloc(1, sizeof *d);
    if (!d || snx_list_dir(path, &d->names, &d->count)) {
        free(d);
        free(path);
        return SCE_ERRNO(ENOENT);
    }
    d->path = path;
    uint32_t fd = new_file(p, d, 1);
    if (!fd) {
        close_directory(d);
        return SCE_ERRNO(EMFILE);
    }
    return fd;
}
/* SceIoDirent (352 bytes): its SceIoStat, then d_name[256]. 1 for an entry, 0 past the last */
static uint32_t io_dread(VitaProcess* p, VitaThread* t) {
    int i = file_index(p, vita_arg(p, t, 0), true);
    uint32_t out = vita_arg(p, t, 1);
    if (i < 0) return SCE_ERRNO(EBADF);
    Directory* d = p->files[i].host;
    while (d->next < d->count) {
        const char* name = d->names[d->next++];
        char* path = snx_path_join(d->path, name);
        bool ok = path && stat_into(p, path, out);
        free(path);
        if (!ok) continue;
        memset(vita_ptr(p, out + 88), 0, 264);
        snprintf((char*)vita_ptr(p, out + 88), 256, "%s", name);
        return 1;
    }
    return 0;
}
static uint32_t io_dclose(VitaProcess* p, VitaThread* t) {
    int i = file_index(p, vita_arg(p, t, 0), true);
    if (i < 0) return SCE_ERRNO(EBADF);
    close_directory(p->files[i].host);
    p->files[i] = (VitaFile){0};
    return 0;
}
void vita_io_close_all(VitaProcess* p) {
    for (unsigned i = 0; i < VITA_FILES; ++i) {
        if (!p->files[i].host) continue;
        if (p->files[i].directory)
            close_directory(p->files[i].host);
        else
            fclose(p->files[i].host);
        p->files[i] = (VitaFile){0};
    }
}


/* ---- SceLibFios2, the synchronous calls: a handle is a descriptor of the table above. The game's paths without a device
   are app0:'s. Only the first calls are logged, for the paths the game uses ---- */
#define FIOS_ERROR_NO_ENTRY 0x80af0002u /* a guess: the game tests for not 0 */
#define FIOS_O_READ   0x01u
#define FIOS_O_WRITE  0x02u
#define FIOS_O_APPEND 0x04u
#define FIOS_O_CREAT  0x08u
#define FIOS_O_TRUNC  0x10u
static bool fios_name(VitaProcess* p, uint32_t address, char* out, size_t size) {
    char raw[1024];
    if (!vita_string(p, address, raw, sizeof raw)) return false;
    snprintf(out, size, strchr(raw, ':') ? "%s" : "app0:%s", raw);
    return true;
}
static uint64_t arg64(VitaProcess* p, VitaThread* t, unsigned n) {
    return (uint64_t)vita_arg(p, t, n) | (uint64_t)vita_arg(p, t, n + 1) << 32;
}
static bool fios_stat(VitaProcess* p, uint32_t address, bool* directory, uint64_t* size) {
    char name[1100];
    bool writable;
    if (!fios_name(p, address, name, sizeof name)) return false;
    VitaPsarc* archive;
    int entry;
    if (archive_lookup(name, &archive, &entry)) {
        *directory = false, *size = vita_psarc_size(archive, entry);
        return true;
    }
    char* path = host_path_of(p, name, &writable);
    struct stat s;
    bool found = path && !stat(path, &s);
    if (found) *directory = snx_is_dir(path), *size = *directory ? 0 : (uint64_t)s.st_size;
    free(path);
    return found;
}
/* sceFiosInitialize(params), sceFiosIOFilterAdd(index, callback, context): nothing to set up, and the filter's
   callback is not called */
static uint32_t fios_ok(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 0;
}
static uint32_t fios_is_initialized(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 1;
}
/* sceFiosStatSync(attr, path, SceFiosStat*): the size at 0, then three dates, the flags at 32 */
static uint32_t fios_stat_sync(VitaProcess* p, VitaThread* t) {
    static unsigned shown;
    bool directory = false;
    uint64_t size = 0;
    bool found = fios_stat(p, vita_arg(p, t, 1), &directory, &size);
    uint32_t out = vita_arg(p, t, 2);
    if (shown < 8) {
        char raw[256] = "";
        vita_string(p, vita_arg(p, t, 1), raw, sizeof raw);
        printf("vita: sceFiosStatSync(\"%s\") -> %s\n", raw, found ? "found" : "missing"), ++shown;
    }
    if (!found) return FIOS_ERROR_NO_ENTRY;
    if (out && vita_mapped(p, out, 72)) {
        memset(vita_ptr(p, out), 0, 72);
        vita_write64(p, out, size);
        vita_write32(p, out + 32, directory ? 1u : 0u);
    }
    return 0;
}
/* sceFiosFHOpenSync(attr, SceFiosFH* out, path, params): the flags at the params' first word */
static uint32_t fios_fh_open_sync(VitaProcess* p, VitaThread* t) {
    static unsigned shown;
    char name[1100];
    uint32_t out = vita_arg(p, t, 1), params = vita_arg(p, t, 3);
    uint32_t want = params && vita_mapped(p, params, 4) ? vita_read32(p, params) : FIOS_O_READ;
    uint32_t flags = 0;
    if (!fios_name(p, vita_arg(p, t, 2), name, sizeof name)) return FIOS_ERROR_NO_ENTRY;
    VitaPsarc* archive;
    int entry;
    if (archive_lookup(name, &archive, &entry)) {
        for (unsigned i = 0; i < ARCHIVE_FILES; ++i) {
            if (archive_files[i].archive) continue;
            archive_files[i] = (typeof(archive_files[i])){archive, entry, 0};
            if (out && vita_mapped(p, out, 4)) vita_write32(p, out, ARCHIVE_FH + i);
            if (shown < 8) printf("vita: sceFiosFHOpenSync(\"%s\") -> archive entry %d\n", name, entry), ++shown;
            return 0;
        }
        return FIOS_ERROR_NO_ENTRY;
    }
    if (!(want & (FIOS_O_READ | FIOS_O_WRITE))) want |= FIOS_O_READ;
    if (want & FIOS_O_READ) flags |= SCE_O_RDONLY;
    if (want & FIOS_O_WRITE) flags |= SCE_O_WRONLY;
    if (want & FIOS_O_APPEND) flags |= SCE_O_APPEND;
    if (want & FIOS_O_CREAT) flags |= SCE_O_CREAT;
    if (want & FIOS_O_TRUNC) flags |= SCE_O_TRUNC;
    char label[1100]; /* host_path_of cuts its path at the colon */
    snprintf(label, sizeof label, "%s", name);
    bool writable = false;
    char* path = host_path_of(p, name, &writable);
    uint32_t fd = path ? open_host(p, path, writable, flags) : SCE_ERRNO(ENOENT);
    if (shown < 8) printf("vita: sceFiosFHOpenSync(\"%s\", flags 0x%x) -> 0x%x\n", label, want, fd), ++shown;
    if (out && vita_mapped(p, out, 4)) vita_write32(p, out, fd & 0x80000000u ? 0 : fd);
    return fd & 0x80000000u ? FIOS_ERROR_NO_ENTRY : 0;
}
static uint32_t fios_fh_close_sync(VitaProcess* p, VitaThread* t) {
    int slot = archive_slot(vita_arg(p, t, 1));
    if (slot >= 0) {
        archive_files[slot].archive = NULL;
        return 0;
    }
    return vita_io_close(p, vita_arg(p, t, 1)) ? FIOS_ERROR_NO_ENTRY : 0;
}
/* a read of length bytes at the file's place, or at offset when with_offset: the count, or -1 */
static uint32_t fios_read_at(VitaProcess* p, VitaThread* t, uint32_t fd, uint32_t buffer, uint64_t length,
                             bool with_offset, uint64_t offset) {
    uint32_t size = length > UINT32_MAX ? UINT32_MAX : (uint32_t)length;
    if (tty_slot(fd) >= 0) return vita_return64(t, 0); /* a tty: EOF, as io_read */
    int slot = archive_slot(fd);
    if (slot >= 0) {
        if (size && !vita_mapped(p, buffer, size)) return vita_return64(t, UINT64_MAX);
        uint64_t at = with_offset ? offset : archive_files[slot].position;
        size_t got = 0;
        vita_watch_release(buffer, size);
        if (size && !vita_psarc_read(archive_files[slot].archive, archive_files[slot].entry, at, vita_ptr(p, buffer),
                                     size, &got))
            return vita_return64(t, UINT64_MAX);
        if (!with_offset) archive_files[slot].position = at + got;
        return vita_return64(t, got);
    }
    FILE* f = vita_io_file(p, fd);
    if (!f || (size && !vita_mapped(p, buffer, size))) return vita_return64(t, UINT64_MAX);
    int64_t was = io_tell(f);
    if (with_offset && io_seek(f, (int64_t)offset, SEEK_SET)) return vita_return64(t, UINT64_MAX);
    vita_watch_release(buffer, size);
    size_t count = size ? fread(vita_ptr(p, buffer), 1, size, f) : 0;
    if (with_offset) io_seek(f, was, SEEK_SET);
    return vita_return64(t, count);
}
/* sceFiosFHReadSync(attr, fh, buffer, SceFiosSize length) */
static uint32_t fios_fh_read_sync(VitaProcess* p, VitaThread* t) {
    return fios_read_at(p, t, vita_arg(p, t, 1), vita_arg(p, t, 2), arg64(p, t, 4), false, 0);
}
/* sceFiosFHPreadSync(attr, fh, buffer, SceFiosSize length, SceFiosOffset offset) */
static uint32_t fios_fh_pread_sync(VitaProcess* p, VitaThread* t) {
    return fios_read_at(p, t, vita_arg(p, t, 1), vita_arg(p, t, 2), arg64(p, t, 4), true, arg64(p, t, 6));
}
static int64_t fios_size_of(FILE* f) {
    int64_t at = io_tell(f), size = io_seek(f, 0, SEEK_END) ? 0 : io_tell(f);
    io_seek(f, at, SEEK_SET);
    return size;
}
/* sceFiosFHGetSize(fh), sceFiosFHTell(fh), sceFiosFHSeek(fh, SceFiosOffset offset, whence): sizes and places as 64 bits */
static uint32_t fios_fh_get_size(VitaProcess* p, VitaThread* t) {
    if (tty_slot(vita_arg(p, t, 0)) >= 0) return vita_return64(t, 0); /* a character device: size 0 */
    int slot = archive_slot(vita_arg(p, t, 0));
    if (slot >= 0) return vita_return64(t, vita_psarc_size(archive_files[slot].archive, archive_files[slot].entry));
    FILE* f = vita_io_file(p, vita_arg(p, t, 0));
    return vita_return64(t, f ? (uint64_t)fios_size_of(f) : UINT64_MAX);
}
static uint32_t fios_fh_tell(VitaProcess* p, VitaThread* t) {
    if (tty_slot(vita_arg(p, t, 0)) >= 0) return vita_return64(t, 0);
    int slot = archive_slot(vita_arg(p, t, 0));
    if (slot >= 0) return vita_return64(t, archive_files[slot].position);
    FILE* f = vita_io_file(p, vita_arg(p, t, 0));
    return vita_return64(t, f ? (uint64_t)io_tell(f) : UINT64_MAX);
}
static uint32_t fios_fh_seek(VitaProcess* p, VitaThread* t) {
    if (tty_slot(vita_arg(p, t, 0)) >= 0) return vita_return64(t, UINT64_MAX); /* not seekable */
    int slot = archive_slot(vita_arg(p, t, 0));
    if (slot >= 0) {
        uint64_t size = vita_psarc_size(archive_files[slot].archive, archive_files[slot].entry);
        uint32_t whence = vita_arg(p, t, 4);
        int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)archive_files[slot].position : (int64_t)size;
        int64_t to = base + (int64_t)arg64(p, t, 2);
        if (whence > 2 || to < 0) return vita_return64(t, UINT64_MAX);
        archive_files[slot].position = (uint64_t)to;
        return vita_return64(t, (uint64_t)to);
    }
    FILE* f = vita_io_file(p, vita_arg(p, t, 0));
    uint32_t whence = vita_arg(p, t, 4);
    if (!f || whence > 2 || io_seek(f, (int64_t)arg64(p, t, 2), (int)whence)) return vita_return64(t, UINT64_MAX);
    return vita_return64(t, (uint64_t)io_tell(f));
}
/* an archive of the game's: its host path and the archive opened, NULL for neither */
static VitaPsarc* fios_open_archive(VitaProcess* p, uint32_t address, char* label, size_t label_size) {
    char name[1100];
    bool writable = false;
    if (!fios_name(p, address, name, sizeof name)) return NULL;
    snprintf(label, label_size, "%s", name);
    char* path = host_path_of(p, name, &writable);
    VitaPsarc* a = path ? vita_psarc_open(path) : NULL;
    free(path);
    return a;
}
/* sceFiosArchiveGetMountBufferSizeSync(attr, path, params): the size of the buffer the mount is given, which the
   runtime does not use. The table of contents is what FIOS2 keeps in it */
static uint32_t fios_archive_buffer_size_sync(VitaProcess* p, VitaThread* t) {
    static unsigned shown;
    char label[1100] = "";
    VitaPsarc* a = fios_open_archive(p, vita_arg(p, t, 1), label, sizeof label);
    uint64_t size = a ? ((uint64_t)vita_psarc_toc_length(a) + 0xfffu & ~0xfffull) + 0x1000u : UINT64_MAX;
    if (shown < 8) printf("vita: sceFiosArchiveGetMountBufferSizeSync(\"%s\") -> %" PRId64 "\n", label, (int64_t)size), ++shown;
    vita_psarc_close(a);
    return vita_return64(t, size);
}
/* sceFiosArchiveMountSync(attr, SceFiosFH* out, path, mount point, SceFiosBuffer (two words), params). The first calls
   are logged with the words after the mount point, to check this */
static uint32_t fios_archive_mount_sync(VitaProcess* p, VitaThread* t) {
    static unsigned shown;
    char label[1100] = "", point[256] = "";
    uint32_t out = vita_arg(p, t, 1), point_address = vita_arg(p, t, 3);
    if (point_address) vita_string(p, point_address, point, sizeof point);
    VitaPsarc* a = fios_open_archive(p, vita_arg(p, t, 2), label, sizeof label);
    unsigned slot = 0;
    while (slot < ARCHIVE_MOUNTS && archive_mounts[slot].archive) ++slot;
    if (shown < 8)
        printf("vita: sceFiosArchiveMountSync(\"%s\" at \"%s\", 0x%x 0x%x 0x%x) -> %s\n", label, point, vita_arg(p, t, 4),
               vita_arg(p, t, 5), vita_arg(p, t, 6), a && slot < ARCHIVE_MOUNTS ? "mounted" : "failed"), ++shown;
    if (!a || slot == ARCHIVE_MOUNTS) {
        vita_psarc_close(a);
        return FIOS_ERROR_NO_ENTRY;
    }
    archive_mounts[slot].archive = a;
    snprintf(archive_mounts[slot].point, sizeof archive_mounts[slot].point, "%s", strip_device(point));
    if (out && vita_mapped(p, out, 4)) vita_write32(p, out, ARCHIVE_MOUNT_FH + slot);
    return 0;
}
static uint32_t fios_archive_unmount_sync(VitaProcess* p, VitaThread* t) {
    uint32_t fh = vita_arg(p, t, 1);
    if (fh >= ARCHIVE_MOUNT_FH && fh < ARCHIVE_MOUNT_FH + ARCHIVE_MOUNTS && archive_mounts[fh - ARCHIVE_MOUNT_FH].archive) {
        vita_psarc_close(archive_mounts[fh - ARCHIVE_MOUNT_FH].archive);
        archive_mounts[fh - ARCHIVE_MOUNT_FH].archive = NULL;
    }
    return 0;
}
/* sceFiosFileExistsSync(attr, path), sceFiosDirectoryExistsSync(attr, path), sceFiosFileGetSizeSync(attr, path) */
static uint32_t fios_file_exists_sync(VitaProcess* p, VitaThread* t) {
    bool directory = false;
    uint64_t size = 0;
    return fios_stat(p, vita_arg(p, t, 1), &directory, &size) && !directory;
}
static uint32_t fios_directory_exists_sync(VitaProcess* p, VitaThread* t) {
    bool directory = false;
    uint64_t size = 0;
    return fios_stat(p, vita_arg(p, t, 1), &directory, &size) && directory;
}
static uint32_t fios_file_get_size_sync(VitaProcess* p, VitaThread* t) {
    bool directory = false;
    uint64_t size = 0;
    return vita_return64(t, fios_stat(p, vita_arg(p, t, 1), &directory, &size) && !directory ? size : UINT64_MAX);
}

/* ---- SceFios2User, the kernel half of libfios2.suprx, which a game ships and the runtime runs as guest code (its
   exports shadow the calls above). The library asks the kernel to resolve each path through the overlays that a
   process added (the 02 calls, the process id first). Nothing here adds one, so the list is empty and a path resolves
   to itself. libfios2 uses the output buffer only when the call returns 0 ---- */
/* sceFiosKernelOverlayResolveWithRangeSync(pid, flags, in, out, capacity, min_order, max_order) */
static uint32_t fios_overlay_resolve(VitaProcess* p, VitaThread* t) {
    static unsigned shown;
    uint32_t out = vita_arg(p, t, 3), capacity = vita_arg(p, t, 4);
    char path[1024];
    if (!vita_string(p, vita_arg(p, t, 2), path, sizeof path)) return SCE_KERNEL_ERROR_ERROR;
    size_t size = strlen(path) + 1;
    if (size > capacity || !vita_mapped(p, out, (uint32_t)size)) return SCE_KERNEL_ERROR_ERROR;
    memcpy(vita_ptr(p, out), path, size);
    if (shown < 8) printf("vita: overlay resolve(\"%s\") -> itself\n", path), ++shown;
    return 0;
}
/* sceFiosKernelOverlayGetList(pid, min_order, max_order, ids, capacity, count*): the count is 0 */
static uint32_t fios_overlay_get_list(VitaProcess* p, VitaThread* t) {
    uint32_t count = vita_arg(p, t, 5);
    if (!count) return 0;
    if (!vita_mapped(p, count, 4)) return SCE_KERNEL_ERROR_ERROR;
    vita_write32(p, count, 0);
    return 0;
}
/* sceFiosKernelOverlayGetRecommendedScheduler(type, path, ...): libfios2 takes 1 as "use another scheduler", and
   with no overlay there is none, so the answer stays 0 */
static uint32_t fios_overlay_scheduler(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 0;
}

const VitaHle vita_hle_io[] = {
    {0xdf3352fcu, "sceFiosArchiveGetMountBufferSizeSync", fios_archive_buffer_size_sync},
    {0xc4822276u, "sceFiosArchiveMountSync", fios_archive_mount_sync},
    {0xb26dc24du, "sceFiosArchiveUnmountSync", fios_archive_unmount_sync},
    {0x61c4aac4u, "sceFiosOverlayResolveWithRangeSync02", fios_overlay_resolve},
    {0x1dd808d1u, "sceFiosOverlayGetList02", fios_overlay_get_list},
    {0xf5c1f928u, "sceFiosOverlayGetRecommendedScheduler02", fios_overlay_scheduler},
    {0x70321220u, "sceFiosOverlayThreadSetDisabled02", fios_ok},
    {0x774c2c05u, "sceFiosInitialize", fios_ok},
    {0xd55b8555u, "sceFiosIOFilterAdd", fios_ok},
    {0x2904b539u, "sceFiosTerminate", fios_ok},
    {0xf4f54e09u, "sceFiosIsInitialized", fios_is_initialized},
    {0xacbaf3e0u, "sceFiosStatSync", fios_stat_sync},
    {0xc3e7c3dbu, "sceFiosFHOpenSync", fios_fh_open_sync},
    {0x45182328u, "sceFiosFHCloseSync", fios_fh_close_sync},
    {0x76945919u, "sceFiosFHReadSync", fios_fh_read_sync},
    {0xe2805059u, "sceFiosFHPreadSync", fios_fh_pread_sync},
    {0xc5c26581u, "sceFiosFHGetSize", fios_fh_get_size},
    {0xd7f33130u, "sceFiosFHTell", fios_fh_tell},
    {0xa75f3c4au, "sceFiosFHSeek", fios_fh_seek},
    {0x233b070cu, "sceFiosFileExistsSync", fios_file_exists_sync},
    {0x726e01beu, "sceFiosDirectoryExistsSync", fios_directory_exists_sync},
    {0x789215c3u, "sceFiosFileGetSizeSync", fios_file_get_size_sync},
    {0x6c60ac61u, "sceIoOpen", io_open},
    {0xc70b8886u, "sceIoClose", io_close},
    {0xfdb32293u, "sceIoRead", io_read},
    {0x52315ad7u, "sceIoPread", io_pread},
    {0x16512f59u, "sceIoSyncByFd", io_sync_by_fd},
    {0x34efd876u, "sceIoWrite", io_write},
    {0x99ba173eu, "sceIoLseek", io_lseek},
    {0x49252b9bu, "sceIoLseek32", io_lseek32},
    {0xe20ed0f3u, "sceIoRemove", io_remove},
    {0x9670d39fu, "sceIoMkdir", io_mkdir},
    {0xe9f91ec8u, "sceIoRmdir", io_rmdir},
    {0xf737e369u, "sceIoRename", io_rename},
    {0xbca5b623u, "sceIoGetstat", io_getstat},
    {0x57f8cd25u, "sceIoGetstatByFd", io_getstat_by_fd},
    {0xa9283dd0u, "sceIoDopen", io_dopen},
    {0x9c8b6624u, "sceIoDread", io_dread},
    {0x422a221au, "sceIoDclose", io_dclose},
    {0, NULL, NULL},
};