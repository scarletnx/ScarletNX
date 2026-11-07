/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* bionic's POSIX side as HLE: files and FILE, memory maps, sockets, time, the process, signals and system calls.
   bionic's numbers and layouts are Linux's (AArch64's). Where the host's differ, the adapter converts them: struct
   stat, the open and fcntl flags, sysconf's names, epoll_event, addrinfo and sigset_t on every host, and on FreeBSD,
   macOS and Windows also the socket families, levels, options and addresses (BSD's have sa_len), the poll events, the
   mmap flags and struct dirent. The conversions are the GNU ABI's (gnuabi/abi/linux_file.h, linux_net.h) and the
   network stack's (common/network/net_host.h). Windows has no POSIX C library: its CRT, winpthreads and Winsock
   give what they have, and the rest is a Win32 version or an error (docs/android/NATIVE.md, "Hosts"). The rest is
   PASS */
#if defined(__linux__)
#define _GNU_SOURCE
#endif
#include "android/bridge/bridge.h"
#include "android/bridge/hle.h"
#include "android/bridge/hle_assets.h"
#include "android/bridge/hle_impl.h"
#include "android/bridge/hle_signal.h"
#include "android/bridge/host.h"
#include "android/bridge/hle_proc.h"
#include "common/network/net_host.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "gnuabi/abi/errno/errno.h"
#include "gnuabi/abi/linux_file.h"
#include "gnuabi/abi/linux_net.h"
#include "gnuabi/kernel/kernel.h"
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <wchar.h>
#ifdef _WIN32
#include <netioapi.h> /* if_nametoindex */
#include <direct.h>
#include <dirent.h>
#include <io.h>
#include <process.h>
#include <sys/utime.h>
#include <unistd.h>
#else
#include <arpa/inet.h>
#include <dirent.h>
#include <net/if.h>
#include <netdb.h>
#include <poll.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <syslog.h>
#include <unistd.h>
#include <utime.h>
#endif
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <xlocale.h> /* strftime_l */
#endif
#if defined(__linux__)
#include <malloc.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/statfs.h>
#endif

typedef uint64_t (*Pass8)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

/* -1, and the guest's errno a Linux number */
static void fail_errno(CPU* c, int linux_error) {
    hle_set_errno(linux_error);
    X(0) = (uint64_t)-1;
}
/* a function the host has no equal of: ENOSYS */
static void refused(CPU* c, void* data) {
    (void)data;
    fail_errno(c, GNU_ENOSYS);
}
/* a host result of an int-returning call as bionic's ssize_t or int: sign-extended, as an int in x0 is not */
static uint64_t wide_int(int64_t r) {
    return (uint64_t)r;
}

static struct {
    bool online; /* hle_set_network's */
    /* the objects of the host's C library that are no constant address on every host (Windows' environ is a
       function's, FreeBSD has no timezone or daylight): copies, set by hle_posix_init and tzset */
    char** environment;
    int64_t timezone_;
    int32_t daylight_;
    char* zone_names[2]; /* tzname's */
    /* FILE: bionic's __sF[3] (152 bytes each on LP64) stand for stdin, stdout and stderr */
    unsigned char sF[3 * 152];
    uint64_t guest_stdin, guest_stdout, guest_stderr;
} g_posix;
enum { BIONIC_FILE_SIZE = 152 };
/* the thread's results that bionic's functions return in static storage */
static _Thread_local struct {
    struct {
        uint64_t name, aliases;
        int32_t addrtype, length;
        uint64_t list;
        char text[256];
        uint64_t alias_list[1];
        uint64_t addresses[9];
        uint8_t bytes[8][4];
    } host; /* gethostbyname's hostent */
#if !defined(__linux__)
    unsigned char dirent[280]; /* readdir's */
    struct {
        uint64_t name, password;
        uint32_t uid, gid;
        uint64_t gecos, dir, shell;
    } passwd; /* getpwuid's */
#endif
#ifdef _WIN32
    unsigned char tm[56]; /* gmtime's and localtime's */
#endif
} t_posix;

static void read_timezone(void) {
    tzset();
#if defined(_WIN32)
    long seconds = 0;
    int daylight = 0;
    _get_timezone(&seconds);
    _get_daylight(&daylight);
    g_posix.timezone_ = seconds;
    g_posix.daylight_ = daylight;
#elif defined(__FreeBSD__)
    /* FreeBSD has neither: the offset of local time now, and whether the zone has a daylight time */
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    g_posix.timezone_ = -(int64_t)local.tm_gmtoff + (local.tm_isdst > 0 ? 3600 : 0);
    g_posix.daylight_ = tzname[1] && tzname[1][0] && strcmp(tzname[0], tzname[1]);
#else
    g_posix.timezone_ = timezone;
    g_posix.daylight_ = daylight;
#endif
}

static void read_zone_names(void) {
#ifdef _WIN32
    g_posix.zone_names[0] = _tzname[0];
    g_posix.zone_names[1] = _tzname[1];
#else
    g_posix.zone_names[0] = tzname[0];
    g_posix.zone_names[1] = tzname[1];
#endif
}

void hle_posix_init(void) {
    uint64_t base = (uint64_t)(uintptr_t)g_posix.sF;
    g_posix.guest_stdin = base;
    g_posix.guest_stdout = base + BIONIC_FILE_SIZE;
    g_posix.guest_stderr = base + 2 * BIONIC_FILE_SIZE;
#ifdef _WIN32
    g_posix.environment = _environ;
#else
    extern char** environ;
    g_posix.environment = environ;
#endif
    read_timezone();
    read_zone_names();
}

FILE* hle_file(uint64_t guest) {
    uint64_t base = (uint64_t)(uintptr_t)g_posix.sF;
    if (guest >= base && guest < base + sizeof g_posix.sF) {
        uint64_t i = (guest - base) / BIONIC_FILE_SIZE;
        return i == 0 ? stdin : i == 1 ? stdout : stderr;
    }
    return (FILE*)(uintptr_t)guest;
}

/* ---- FILE ---- */

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
#ifdef _WIN32
/* long is 32 bits on Windows, off_t and bionic's long are 64: the 64-bit seeks */
static int file_seek(FILE* f, int64_t offset, int whence) {
    return _fseeki64(f, offset, whence);
}
static int64_t file_tell(FILE* f) {
    return _ftelli64(f);
}
#undef fseeko
#undef ftello
#define fseek  file_seek
#define fseeko file_seek
#define ftell  file_tell
#define ftello file_tell
#endif
/* the wide character ones only where wchar_t is bionic's 32 bits */
#ifdef _WIN32
#define WIDE_FILE_CALLS(F)
#else
#define WIDE_FILE_CALLS(F) F(fputwc, 1) F(getwc, 0) F(ungetwc, 1)
#endif
#define FILE_CALLS(F)                                                                                                  \
    F(fclose, 0) F(feof, 0) F(ferror, 0) F(clearerr, 0) F(fflush, 0) F(fgets, 2) F(fileno, 0) F(fputc, 1) F(fputs, 1)  \
    F(fread, 3) F(fwrite, 3) F(fseek, 0) F(fseeko, 0) F(ftell, 0) F(ftello, 0) F(getc, 0) F(ungetc, 1) F(rewind, 0)   \
    F(setbuf, 0) F(freopen, 2) F(pclose, 0) WIDE_FILE_CALLS(F)
#define FILE_CALL_OBJECT(name, k) static const FileCall fc_##name = {(void*)name, k};
FILE_CALLS(FILE_CALL_OBJECT)
#define FILE_CALL_ENTRY(name, k) {#name, file_pass, (void*)&fc_##name},

/* __fread_chk(buffer, size, count, file, buffer_size) and __fwrite_chk: fread and fwrite, with bionic's check that
   size * count bytes fit the buffer */
static bool fits(uint64_t size, uint64_t count, uint64_t room) {
    return !size || count <= room / size;
}
static void g_fread_chk(CPU* c, void* data) {
    (void)data;
    if (!fits(X(1), X(2), X(4))) {
        fprintf(stderr, "scarletnx-android: FORTIFY: fread: buffer overflow\n");
        abort();
    }
    X(0) = fread(PTR(0), X(1), X(2), hle_file(X(3)));
}
static void g_fwrite_chk(CPU* c, void* data) {
    (void)data;
    if (!fits(X(1), X(2), X(4))) {
        fprintf(stderr, "scarletnx-android: FORTIFY: fwrite: buffer overflow\n");
        abort();
    }
    X(0) = fwrite(PTR(0), X(1), X(2), hle_file(X(3)));
}

/* getdelim(line, size, delimiter, file) and getline(line, size, file), POSIX's, on every host (Windows' C runtime has
   neither): the bytes up to the delimiter and it into *line, which realloc grows (*size its room): their count, or
   -1 at the end of the file before a byte, and on an error */
static int64_t read_delimited(char** line, size_t* size, int delimiter, FILE* f) {
    if (!line || !size || !f) {
        hle_set_errno(GNU_EINVAL);
        return -1;
    }
    size_t room = *line ? *size : 0, n = 0;
    for (int ch; (ch = getc(f)) != EOF;) {
        if (n + 2 > room) {
            size_t grown = room < 64 ? 128 : room * 2;
            char* p = realloc(*line, grown);
            if (!p) {
                hle_set_errno(GNU_ENOMEM);
                return -1;
            }
            *line = p;
            *size = room = grown;
        }
        (*line)[n++] = (char)ch;
        if (ch == (unsigned char)delimiter) break;
    }
    if (!n) return -1;
    (*line)[n] = 0;
    return (int64_t)n;
}
static void g_getdelim(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)read_delimited(PTR(0), PTR(1), (int)X(2), hle_file(X(3)));
}
static void g_getline(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)read_delimited(PTR(0), PTR(1), '\n', hle_file(X(2)));
}

/* setvbuf(f, buffer, mode, size): bionic's modes (_IOFBF 0, _IOLBF 1, _IONBF 2) as the host's */
static void g_setvbuf(CPU* c, void* data) {
    (void)data;
    int mode = X(2) == 0 ? _IOFBF : X(2) == 1 ? _IOLBF : _IONBF;
    X(0) = wide_int(setvbuf(hle_file(X(0)), PTR(1), mode, (size_t)X(3)));
}
/* fopen's and fdopen's mode: bionic's 'e' (close-on-exec) is a letter Windows' CRT refuses */
static const char* file_mode(const char* mode, char out[16]) {
#ifdef _WIN32
    size_t n = 0;
    for (; *mode && n + 2 < 16; ++mode)
        if (*mode != 'e') out[n++] = *mode;
    out[n++] = 'b'; /* bytes as they are, as on POSIX */
    out[n] = 0;
    return out;
#else
    (void)out;
    return mode;
#endif
}
static void g_fopen(CPU* c, void* data) {
    (void)data;
    int own = hle_proc_open(STR(0)); /* the app's own /proc files (hle_proc.h) */
    if (own != -2) {
        X(0) = (uint64_t)(uintptr_t)(own < 0 ? NULL : fdopen(own, "r"));
        return;
    }
    char mode[16];
    X(0) = (uint64_t)(uintptr_t)fopen(STR(0), file_mode(STR(1), mode));
}
static void g_fdopen(CPU* c, void* data) {
    (void)data;
    char mode[16];
    X(0) = (uint64_t)(uintptr_t)fdopen((int)X(0), file_mode(STR(1), mode));
}
static void g_rename(CPU* c, void* data) {
    (void)data;
#ifdef _WIN32
    /* POSIX's rename replaces the target, which Windows' refuses */
    if (MoveFileExA(STR(0), STR(1), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) X(0) = 0;
    else fail_errno(c, GetLastError() == ERROR_FILE_NOT_FOUND ? GNU_ENOENT : GNU_EACCES);
#else
    X(0) = wide_int(rename(STR(0), STR(1)));
#endif
}

static void g_putchar(CPU* c, void* data) {
    (void)data;
    X(0) = wide_int(putchar((int)X(0)));
}
static void g_puts(CPU* c, void* data) {
    (void)data;
    X(0) = wide_int(puts(STR(0)));
}
/* bionic's: "message: " when there is one, then strerror's text */
static void g_perror(CPU* c, void* data) {
    (void)data;
    char unknown[32];
    const char* text = hle_error_text(*hle_errno_location(), unknown, sizeof unknown);
    bool message = X(0) && STR(0)[0];
    fprintf(stderr, "%s%s%s\n", message ? STR(0) : "", message ? ": " : "", text);
}
/* a process the app would start: none, as an app's sandbox mostly refuses */
static void g_popen(CPU* c, void* data) {
    (void)data;
    hle_set_errno(GNU_ENOSYS);
    X(0) = 0;
}

/* ---- the open flags ---- */

static int open_flags(uint64_t guest) {
#if defined(__linux__) && defined(__x86_64__)
    /* those whose AArch64 values differ from x86-64's */
    int flags = (int)guest & ~(0x4000 | 0x8000 | 0x10000 | 0x20000);
    if (guest & 0x4000) flags |= O_DIRECTORY;
    if (guest & 0x8000) flags |= O_NOFOLLOW;
    if (guest & 0x10000) flags |= O_DIRECT;
    return flags; /* O_LARGEFILE (0x20000) is implied on a 64-bit host */
#elif defined(__linux__)
    return (int)guest;
#elif defined(_WIN32)
    return gnu_open_flags_to_host((unsigned)guest) | O_BINARY | (guest & GNU_O_CLOEXEC ? O_NOINHERIT : 0);
#else
    return gnu_open_flags_to_host((unsigned)guest);
#endif
}
#ifndef _WIN32 /* Windows reports no status flags */
static uint64_t guest_flags(int host) {
#if defined(__linux__) && defined(__x86_64__)
    uint64_t flags = (uint64_t)(unsigned)host & ~(uint64_t)(O_DIRECTORY | O_NOFOLLOW | O_DIRECT | 0x8000);
    if (host & O_DIRECTORY) flags |= 0x4000;
    if (host & O_NOFOLLOW) flags |= 0x8000;
    if (host & O_DIRECT) flags |= 0x10000;
    return flags | 0x20000;
#elif defined(__linux__)
    return (uint64_t)(unsigned)host;
#else
    return gnu_open_flags_from_host(host);
#endif
}
#endif
static void g_open(CPU* c, void* data) {
    (void)data;
    int own = hle_proc_open(STR(0));
    if (own != -2) {
        X(0) = wide_int(own);
        return;
    }
    X(0) = wide_int(open(STR(0), open_flags(X(1)), (int)X(2)));
}
static void g_open_2(CPU* c, void* data) {
    (void)data;
    int own = hle_proc_open(STR(0));
    if (own != -2) {
        X(0) = wide_int(own);
        return;
    }
    X(0) = wide_int(open(STR(0), open_flags(X(1))));
}

/* fcntl: Linux's commands. F_DUPFD to F_SETFL are 0 to 4 on every POSIX host */
enum {
    L_F_DUPFD = 0,
    L_F_GETFD = 1,
    L_F_SETFD = 2,
    L_F_GETFL = 3,
    L_F_SETFL = 4,
    L_F_GETLK = 5,
    L_F_SETLK = 6,
    L_F_SETLKW = 7,
    L_F_DUPFD_CLOEXEC = 1030,
};
#if !defined(__linux__) && !defined(_WIN32)
/* Linux's struct flock on AArch64 {short type, short whence, off_t start, off_t len, pid_t pid}, and its lock types
   (F_RDLCK 0, F_WRLCK 1, F_UNLCK 2), as the host's */
static int lock_command(CPU* c, int fd, int cmd) {
    unsigned char* g = PTR(2);
    struct flock l = {0};
    int16_t type = (int16_t)snx_ld_le16(g);
    l.l_type = type == 0 ? F_RDLCK : type == 1 ? F_WRLCK : F_UNLCK;
    l.l_whence = (short)snx_ld_le16(g + 2);
    l.l_start = (off_t)snx_ld_le64(g + 8);
    l.l_len = (off_t)snx_ld_le64(g + 16);
    int r = fcntl(fd, cmd == L_F_GETLK ? F_GETLK : cmd == L_F_SETLK ? F_SETLK : F_SETLKW, &l);
    if (!r && cmd == L_F_GETLK) {
        snx_st_le16(g, (uint16_t)(l.l_type == F_RDLCK ? 0 : l.l_type == F_WRLCK ? 1 : 2));
        snx_st_le16(g + 2, (uint16_t)l.l_whence);
        snx_st_le64(g + 8, (uint64_t)l.l_start);
        snx_st_le64(g + 16, (uint64_t)l.l_len);
        snx_st_le32(g + 24, (uint32_t)l.l_pid);
    }
    return r;
}
#endif
static void g_fcntl(CPU* c, void* data) {
    (void)data;
    int fd = (int)X(0), cmd = (int)X(1);
#if defined(__linux__)
    if (cmd == F_GETFL) {
        int r = fcntl(fd, F_GETFL);
        X(0) = r < 0 ? (uint64_t)-1 : guest_flags(r);
    } else if (cmd == F_SETFL) {
        X(0) = wide_int(fcntl(fd, F_SETFL, open_flags(X(2))));
    } else {
        X(0) = wide_int(fcntl(fd, cmd, X(2)));
    }
#elif defined(_WIN32)
    /* the CRT's descriptors and Winsock's sockets: O_NONBLOCK of a socket, and the duplicates of a file */
    switch (cmd) {
    case L_F_GETFD:
    case L_F_SETFD: X(0) = 0; break;
    case L_F_GETFL: X(0) = GNU_O_RDWR | GNU_O_LARGEFILE; break;
    case L_F_SETFL:
        if (host_is_socket(fd)) {
            u_long on = (X(2) & GNU_O_NONBLOCK) != 0;
            if (ioctlsocket((SOCKET)(unsigned)fd, FIONBIO, &on)) {
                fail_errno(c, -snx_net_errno_from_host(WSAGetLastError()));
                return;
            }
        }
        X(0) = 0;
        break;
    case L_F_DUPFD:
    case L_F_DUPFD_CLOEXEC: X(0) = wide_int(_dup(fd)); break;
    default: fail_errno(c, GNU_EINVAL); break;
    }
#else
    switch (cmd) {
    case L_F_DUPFD:
    case L_F_GETFD:
    case L_F_SETFD: X(0) = wide_int(fcntl(fd, cmd, (int)X(2))); break;
    case L_F_DUPFD_CLOEXEC: X(0) = wide_int(fcntl(fd, F_DUPFD_CLOEXEC, (int)X(2))); break;
    case L_F_GETFL: {
        int r = fcntl(fd, F_GETFL);
        X(0) = r < 0 ? (uint64_t)-1 : guest_flags(r);
        break;
    }
    case L_F_SETFL: X(0) = wide_int(fcntl(fd, F_SETFL, open_flags(X(2)) & (O_APPEND | O_NONBLOCK))); break;
    case L_F_GETLK:
    case L_F_SETLK:
    case L_F_SETLKW: X(0) = wide_int(lock_command(c, fd, cmd)); break;
    default: fail_errno(c, GNU_EINVAL); break;
    }
#endif
}

/* ioctl: Linux's requests. Elsewhere the few an app makes of a descriptor: FIONREAD, FIONBIO, FIOCLEX, FIONCLEX
   and TIOCGWINSZ (struct winsize is four shorts on every host) */
enum { L_TIOCGWINSZ = 0x5413, L_FIONREAD = 0x541b, L_FIONBIO = 0x5421, L_FIONCLEX = 0x5450, L_FIOCLEX = 0x5451 };
static void g_ioctl(CPU* c, void* data) {
    (void)data;
    int fd = (int)X(0);
#if defined(__linux__)
    X(0) = wide_int(ioctl(fd, (unsigned long)X(1), X(2)));
#elif defined(_WIN32)
    if ((X(1) == L_FIONREAD || X(1) == L_FIONBIO) && host_is_socket(fd)) {
        u_long value = 0;
        if (X(1) == L_FIONBIO) value = *(const int32_t*)PTR(2) != 0;
        if (ioctlsocket((SOCKET)(unsigned)fd, X(1) == L_FIONREAD ? FIONREAD : FIONBIO, &value)) {
            fail_errno(c, -snx_net_errno_from_host(WSAGetLastError()));
            return;
        }
        if (X(1) == L_FIONREAD) snx_st_le32(PTR(2), (uint32_t)value);
        X(0) = 0;
        return;
    }
    fail_errno(c, GNU_ENOTTY);
#else
    switch (X(1)) {
    case L_FIONREAD: X(0) = wide_int(ioctl(fd, FIONREAD, PTR(2))); break;
    case L_FIONBIO: X(0) = wide_int(ioctl(fd, FIONBIO, PTR(2))); break;
    case L_TIOCGWINSZ: X(0) = wide_int(ioctl(fd, TIOCGWINSZ, PTR(2))); break;
    case L_FIOCLEX: X(0) = wide_int(fcntl(fd, F_SETFD, FD_CLOEXEC)); break;
    case L_FIONCLEX: X(0) = wide_int(fcntl(fd, F_SETFD, 0)); break;
    default: fail_errno(c, GNU_ENOTTY); break;
    }
#endif
}

/* ---- struct stat: AArch64's (asm-generic), 128 bytes, from the host's (gnuabi/abi/linux_file.h) ---- */

static void put_stat(CPU* c, int r, uint64_t guest, const GnuHostStat* s) {
    if (!r) {
        GnuStat st;
        gnu_stat_from_host(s, &st);
        unsigned char out[128];
        gnu_stat_encode(out, &st);
        memcpy((void*)(uintptr_t)guest, out, sizeof out);
    }
    X(0) = wide_int(r);
}
/* stat of one of the app's own /proc files: true when it was one, its result in c */
static bool proc_stat(CPU* c, GnuHostStat* s) {
    int own = hle_proc_open(STR(0));
    if (own == -2) return false;
#ifdef _WIN32
    int r = own < 0 ? -1 : _fstat64(own, s);
#else
    int r = own < 0 ? -1 : fstat(own, s);
#endif
    if (own >= 0) host_close(own);
    put_stat(c, r, X(1), s);
    return true;
}
#ifdef _WIN32
static void g_access(CPU* c, void* data);
#endif
/* access and readlink, with the app's own /proc files */
static void g_access_proc(CPU* c, void* data) {
    int own = hle_proc_open(STR(0));
    if (own != -2) {
        if (own >= 0) host_close(own);
        X(0) = wide_int(own < 0 ? -1 : 0);
        return;
    }
#ifdef _WIN32
    g_access(c, data);
#else
    (void)data;
    X(0) = wide_int(access(STR(0), (int)X(1)));
#endif
}
static void g_readlink_proc(CPU* c, void* data) {
    long own = hle_proc_readlink(STR(0), PTR(1), (size_t)X(2));
    if (own != -2) {
        X(0) = (uint64_t)(int64_t)own;
        return;
    }
#ifdef _WIN32
    refused(c, data);
#else
    (void)data;
    X(0) = wide_int(readlink(STR(0), PTR(1), (size_t)X(2)));
#endif
}
static void g_stat(CPU* c, void* data) {
    (void)data;
    GnuHostStat s;
    if (proc_stat(c, &s)) return;
#ifdef _WIN32
    int r = _stat64(STR(0), &s);
#else
    int r = stat(STR(0), &s);
#endif
    put_stat(c, r, X(1), &s);
}
static void g_lstat(CPU* c, void* data) {
    (void)data;
    GnuHostStat s;
    if (proc_stat(c, &s)) return;
#ifdef _WIN32
    int r = _stat64(STR(0), &s); /* the host follows its links itself */
#else
    int r = lstat(STR(0), &s);
#endif
    put_stat(c, r, X(1), &s);
}
static void g_fstat(CPU* c, void* data) {
    (void)data;
    GnuHostStat s;
#ifdef _WIN32
    int r = _fstat64((int)X(0), &s);
#else
    int r = fstat((int)X(0), &s);
#endif
    put_stat(c, r, X(1), &s);
}

#if !defined(__linux__)
/* Linux's struct statfs on AArch64 (120 bytes): type, block size, blocks, free, available, files, free files, the
   id, the name length, the fragment size, the flags */
static void put_statfs(uint64_t guest, uint64_t bsize, uint64_t blocks, uint64_t bfree, uint64_t bavail,
                       uint64_t files, uint64_t ffree, uint64_t namelen) {
    unsigned char out[120] = {0};
    snx_st_le64(out, 0xef53); /* ext4's magic: an app reads it as a local disk */
    snx_st_le64(out + 8, bsize);
    snx_st_le64(out + 16, blocks);
    snx_st_le64(out + 24, bfree);
    snx_st_le64(out + 32, bavail);
    snx_st_le64(out + 40, files);
    snx_st_le64(out + 48, ffree);
    snx_st_le64(out + 64, namelen);
    snx_st_le64(out + 72, bsize);
    memcpy((void*)(uintptr_t)guest, out, sizeof out);
}
static void g_statfs(CPU* c, void* data) {
    (void)data;
#ifdef _WIN32
    ULARGE_INTEGER available, total, free_bytes;
    if (!GetDiskFreeSpaceExA(STR(0), &available, &total, &free_bytes)) {
        fail_errno(c, GNU_ENOENT);
        return;
    }
    put_statfs(X(1), 4096, total.QuadPart / 4096, free_bytes.QuadPart / 4096, available.QuadPart / 4096, 0, 0, 255);
    X(0) = 0;
#else
    struct statvfs s;
    int r = statvfs(STR(0), &s);
    if (!r)
        put_statfs(X(1), s.f_frsize, s.f_blocks, s.f_bfree, s.f_bavail, s.f_files, s.f_ffree, s.f_namemax);
    X(0) = wide_int(r);
#endif
}

/* readdir: bionic's struct dirent (d_ino, d_off, d_reclen, d_type, d_name[256]: 280 bytes), from the host's */
static void g_readdir(CPU* c, void* data) {
    (void)data;
    struct dirent* d = readdir(PTR(0));
    if (!d) {
        X(0) = 0;
        return;
    }
    memset(t_posix.dirent, 0, sizeof t_posix.dirent);
    snx_st_le64(t_posix.dirent, (uint64_t)d->d_ino);
    snx_st_le16(t_posix.dirent + 16, sizeof t_posix.dirent);
#if defined(_WIN32)
    t_posix.dirent[18] = 0; /* DT_UNKNOWN: the reader stats it */
#else
    t_posix.dirent[18] = d->d_type; /* the DT_ values are the same on every POSIX host */
#endif
    size_t n = strlen(d->d_name);
    memcpy(t_posix.dirent + 19, d->d_name, n < 255 ? n : 255);
    X(0) = (uint64_t)(uintptr_t)t_posix.dirent;
}
#endif

#ifdef _WIN32
/* ---- Windows: the POSIX file functions its CRT has in other forms ---- */

static void g_access(CPU* c, void* data) {
    (void)data;
    X(0) = wide_int(_access(STR(0), (int)X(1) & 6)); /* no X_OK: what exists runs */
}
static void g_mkdir(CPU* c, void* data) {
    (void)data;
    X(0) = wide_int(_mkdir(STR(0)));
}
static void g_pipe(CPU* c, void* data) {
    (void)data;
    X(0) = wide_int(_pipe(PTR(0), 65536, _O_BINARY));
}
static void g_lseek(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)_lseeki64((int)X(0), (int64_t)X(1), (int)X(2));
}
static void g_fsync(CPU* c, void* data) {
    (void)data;
    X(0) = wide_int(_commit((int)X(0)));
}
static void g_ftruncate(CPU* c, void* data) {
    (void)data;
    errno_t e = _chsize_s((int)X(0), (int64_t)X(1));
    if (e) fail_errno(c, -gnu_host_errno(e));
    else X(0) = 0;
}
static void g_truncate(CPU* c, void* data) {
    (void)data;
    int fd = _open(STR(0), _O_WRONLY | _O_BINARY);
    if (fd < 0) {
        X(0) = (uint64_t)-1;
        return;
    }
    errno_t e = _chsize_s(fd, (int64_t)X(1));
    _close(fd);
    if (e) fail_errno(c, -gnu_host_errno(e));
    else X(0) = 0;
}
static void g_pread(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)host_pread((int)X(0), PTR(1), (size_t)X(2), X(3));
}
/* read and write of a socket go to Winsock, whose sockets the CRT doesn't know */
static void g_read(CPU* c, void* data) {
    (void)data;
    int fd = (int)X(0);
    unsigned n = X(2) > 0x7fffffff ? 0x7fffffff : (unsigned)X(2);
    if (host_is_socket(fd)) {
        int r = recv((SOCKET)(unsigned)fd, PTR(1), (int)n, 0);
        if (r < 0) fail_errno(c, -snx_net_errno_from_host(WSAGetLastError()));
        else X(0) = (uint64_t)r;
        return;
    }
    X(0) = wide_int(_read(fd, PTR(1), n));
}
static void g_write(CPU* c, void* data) {
    (void)data;
    int fd = (int)X(0);
    unsigned n = X(2) > 0x7fffffff ? 0x7fffffff : (unsigned)X(2);
    if (host_is_socket(fd)) {
        int r = send((SOCKET)(unsigned)fd, PTR(1), (int)n, 0);
        if (r < 0) fail_errno(c, -snx_net_errno_from_host(WSAGetLastError()));
        else X(0) = (uint64_t)r;
        return;
    }
    X(0) = wide_int(_write(fd, PTR(1), n));
}
static void g_realpath(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(uintptr_t)_fullpath(PTR(1), STR(0), X(1) ? 4096 : 0);
}
static void g_utime(CPU* c, void* data) {
    (void)data;
    /* bionic's utimbuf is two 64-bit times, as __utimbuf64 */
    X(0) = wide_int(_utime64(STR(0), PTR(1)));
}
/* flock: LockFileEx of the whole file, LOCK_SH 1, LOCK_EX 2, LOCK_NB 4, LOCK_UN 8 */
static void g_flock(CPU* c, void* data) {
    (void)data;
    HANDLE h = (HANDLE)_get_osfhandle((int)X(0));
    OVERLAPPED all = {0};
    int op = (int)X(1);
    BOOL ok = op & 8 ? UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &all)
                     : LockFileEx(h, (op & 2 ? LOCKFILE_EXCLUSIVE_LOCK : 0) | (op & 4 ? LOCKFILE_FAIL_IMMEDIATELY : 0),
                                  0, MAXDWORD, MAXDWORD, &all);
    if (ok) X(0) = 0;
    else fail_errno(c, h == INVALID_HANDLE_VALUE ? GNU_EBADF : GNU_EAGAIN);
}
static void g_setenv(CPU* c, void* data) {
    (void)data;
    /* setenv(name, value, 0) keeps a variable that is there: the guest's own, found in the environment */
    size_t n = strlen(STR(0));
    bool there = false;
    for (char** e = _environ; e && *e && !there; ++e) there = !strncmp(*e, STR(0), n) && (*e)[n] == '=';
    if (!X(2) && there) {
        X(0) = 0;
        return;
    }
    X(0) = _putenv_s(STR(0), STR(1)) ? (uint64_t)-1 : 0;
}
static void g_getcwd(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(uintptr_t)_getcwd(PTR(0), (int)X(1));
}
#endif

static void g_close(CPU* c, void* data) {
    (void)data;
    X(0) = wide_int(host_close((int)X(0)));
}

/* ---- memory maps: executable ones go into the code window, where the JIT finds them ---- */

/* Linux's mmap flags (bionic's) */
enum {
    L_MAP_SHARED = 0x1,
    L_MAP_PRIVATE = 0x2,
    L_MAP_FIXED = 0x10,
    L_MAP_ANONYMOUS = 0x20,
    L_MAP_NORESERVE = 0x4000,
    L_MAP_FIXED_NOREPLACE = 0x100000,
    L_PROT_EXEC = 0x4,
};
static void* map_failed(void) {
    return (void*)(uintptr_t)-1; /* MAP_FAILED on every host */
}

#ifdef _WIN32
/* Windows' mmap of what an app maps: anonymous memory and private copies of files (VirtualAlloc, then the file's
   bytes read in), shared maps of files (a file mapping's view). An address the app fixes must be free or in the code
   window */
static void* win_mmap(void* address, size_t size, int prot, int flags, int fd, uint64_t offset) {
    DWORD access = prot & 2 ? PAGE_READWRITE : prot & 1 ? PAGE_READONLY : PAGE_NOACCESS;
    if ((flags & L_MAP_SHARED) && !(flags & L_MAP_ANONYMOUS)) {
        HANDLE file = (HANDLE)_get_osfhandle(fd);
        HANDLE mapping = CreateFileMappingA(file, NULL, prot & 2 ? PAGE_READWRITE : PAGE_READONLY, 0, 0, NULL);
        if (!mapping) return map_failed();
        void* view = MapViewOfFileEx(mapping, prot & 2 ? FILE_MAP_WRITE : FILE_MAP_READ, (DWORD)(offset >> 32),
                                     (DWORD)offset, size, (flags & L_MAP_FIXED) ? address : NULL);
        CloseHandle(mapping);
        return view ? view : map_failed();
    }
    void* p = NULL;
    if (address && (flags & L_MAP_FIXED)) {
        /* a range in a reservation (the code window) commits; a free one is reserved first */
        p = VirtualAlloc(address, size, MEM_COMMIT, PAGE_READWRITE);
        if (!p) p = VirtualAlloc(address, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p) memset(p, 0, size);
    } else
        p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) return map_failed();
    if (!(flags & L_MAP_ANONYMOUS) && host_pread(fd, p, size, offset) < 0) {
        VirtualFree(p, 0, MEM_RELEASE);
        return map_failed();
    }
    DWORD old;
    if (access != PAGE_READWRITE) VirtualProtect(p, size, access, &old);
    return p;
}
static int win_munmap(void* address, size_t size) {
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery(address, &m, sizeof m)) return -1;
    if (m.Type == MEM_MAPPED) return UnmapViewOfFile(m.AllocationBase) ? 0 : -1;
    /* a whole allocation is released; a part of one gives its pages back and keeps its addresses */
    if (m.AllocationBase == address && m.RegionSize >= size) return VirtualFree(address, 0, MEM_RELEASE) ? 0 : -1;
    return VirtualFree(address, size, MEM_DECOMMIT) ? 0 : -1;
}
#endif

#ifndef _WIN32
/* bionic's flags as the host's (win_mmap reads Linux's itself) */
static int map_flags(int flags) {
#if defined(__linux__)
    return flags;
#else
    int h = (flags & L_MAP_SHARED ? MAP_SHARED : 0) | (flags & L_MAP_PRIVATE ? MAP_PRIVATE : 0) |
            (flags & (L_MAP_FIXED | L_MAP_FIXED_NOREPLACE) ? MAP_FIXED : 0) | (flags & L_MAP_ANONYMOUS ? MAP_ANON : 0);
#ifdef MAP_NORESERVE
    if (flags & L_MAP_NORESERVE) h |= MAP_NORESERVE;
#endif
#ifdef MAP_EXCL
    if (flags & L_MAP_FIXED_NOREPLACE) h |= MAP_EXCL;
#endif
    return h;
#endif
}
#endif
static void* host_mmap(void* hint, size_t size, int prot, int flags, int fd, uint64_t offset) {
#ifdef _WIN32
    return win_mmap(hint, size, prot, flags, fd, offset);
#elif !defined(__linux__) && !defined(MAP_EXCL)
    /* MAP_FIXED_NOREPLACE without MAP_EXCL (macOS): the address as a hint, which must hold */
    if (flags & L_MAP_FIXED_NOREPLACE) {
        void* p = mmap(hint, size, prot, map_flags(flags & ~(L_MAP_FIXED | L_MAP_FIXED_NOREPLACE)), fd, (off_t)offset);
        if (p != map_failed() && p != hint) {
            munmap(p, size);
            errno = EEXIST;
            return map_failed();
        }
        return p;
    }
    return mmap(hint, size, prot, map_flags(flags), fd, (off_t)offset);
#else
    return mmap(hint, size, prot, map_flags(flags), fd, (off_t)offset);
#endif
}
/* Linux's PROT_READ 1 and PROT_WRITE 2 as host.h's access */
static int host_access(int prot) {
    return (prot & 1 ? HOST_READ : 0) | (prot & 2 ? HOST_READ | HOST_WRITE : 0);
}
static void g_mmap(CPU* c, void* data) {
    (void)data;
    int prot = (int)X(2), flags = (int)X(3);
    void* hint = PTR(0);
    if ((prot & L_PROT_EXEC) && !(flags & L_MAP_FIXED)) {
        uint64_t at = bridge_window_reserve((X(1) + 0xffff) & ~(uint64_t)0xffff, 0x10000);
        if (!at) {
            X(0) = (uint64_t)(uintptr_t)map_failed();
            hle_set_errno(GNU_ENOMEM);
            return;
        }
        hint = (void*)(uintptr_t)at;
        flags |= L_MAP_FIXED;
    }
    /* the guest's code is never run as it is: its pages need reading, not executing */
    int host_prot = (prot & ~L_PROT_EXEC) | (prot & L_PROT_EXEC ? 1 : 0);
    void* r = host_mmap(hint, X(1), host_prot, flags, (int)X(4), X(5));
    if (r != map_failed() && (prot & L_PROT_EXEC)) bridge_add_code((uint64_t)(uintptr_t)r, X(1) & ~(uint64_t)3);
    /* a private anonymous mapping is recorded for MADV_DONTNEED (host.h), any other forgotten over its range */
    if (r != map_failed()) {
        if ((flags & L_MAP_ANONYMOUS) && !(flags & L_MAP_SHARED))
            host_anon_map((uint64_t)(uintptr_t)r, X(1), host_access(host_prot));
        else
            host_anon_unmap((uint64_t)(uintptr_t)r, X(1));
    }
    X(0) = (uint64_t)(uintptr_t)r;
}
static void g_mprotect(CPU* c, void* data) {
    (void)data;
    int prot = (int)X(2);
    if (prot & L_PROT_EXEC) bridge_add_code(X(0), X(1) & ~(uint64_t)3);
    int access = (prot & 1 || prot & L_PROT_EXEC ? HOST_READ : 0) | (prot & 2 ? HOST_READ | HOST_WRITE : 0);
    if (host_protect(PTR(0), X(1), access)) {
        host_anon_protect(X(0), X(1), access);
        X(0) = 0;
    } else {
        fail_errno(c, GNU_ENOMEM);
    }
}
static void g_munmap(CPU* c, void* data) {
    (void)data;
    host_anon_unmap(X(0), X(1));
#ifdef _WIN32
    if (win_munmap(PTR(0), X(1))) fail_errno(c, GNU_EINVAL);
    else X(0) = 0;
#else
    X(0) = wide_int(munmap(PTR(0), X(1)));
#endif
}
/* msync: Linux's MS_ASYNC 1, MS_INVALIDATE 2, MS_SYNC 4 */
static void g_msync(CPU* c, void* data) {
    (void)data;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION m;
    bool mapped = VirtualQuery(PTR(0), &m, sizeof m) && m.Type == MEM_MAPPED;
    X(0) = !mapped || FlushViewOfFile(PTR(0), X(1)) ? 0 : (uint64_t)-1;
#else
    int f = (int)X(2);
    int h = (f & 1 ? MS_ASYNC : 0) | (f & 2 ? MS_INVALIDATE : 0) | (f & 4 ? MS_SYNC : 0);
    X(0) = wide_int(msync(PTR(0), X(1), h));
#endif
}
/* mincore: one byte for each page, its low bit set for a page in memory */
static void g_mincore(CPU* c, void* data) {
    (void)data;
#ifdef _WIN32
    memset(PTR(2), 1, (X(1) + 4095) / 4096); /* each page as in memory: Windows doesn't tell */
    X(0) = 0;
#else
    int r = mincore(PTR(0), X(1), PTR(2));
    if (!r) {
        size_t pages = (X(1) + 4095) / 4096;
        unsigned char* v = PTR(2);
        for (size_t i = 0; i < pages; ++i) v[i] &= 1;
    }
    X(0) = wide_int(r);
#endif
}

/* mallinfo: bionic's has size_t fields, as glibc's mallinfo2, and comes back through x8, AArch64's register for a
   large result's address. Elsewhere the host's allocator tells none of it: zeros */
static void g_mallinfo(CPU* c, void* data) {
    (void)data;
#if defined(__linux__)
    struct mallinfo2 m = mallinfo2();
    memcpy((void*)(uintptr_t)X(8), &m, sizeof m);
#else
    memset((void*)(uintptr_t)X(8), 0, 10 * sizeof(size_t));
#endif
}

/* ---- epoll: the host's own readiness (host.h). epoll_event: 16 bytes on AArch64 ---- */

static void g_epoll_create(CPU* c, void* data) {
    (void)data;
    X(0) = wide_int(host_epoll_create(false));
}
static void g_epoll_create1(CPU* c, void* data) {
    (void)data;
    X(0) = wide_int(host_epoll_create((X(0) & GNU_O_CLOEXEC) != 0));
}
static void g_epoll_ctl(CPU* c, void* data) {
    (void)data;
    uint32_t events = 0;
    uint64_t d = 0;
    if (X(3)) {
        events = snx_ld_le32(PTR(3));
        d = snx_ld_le64((unsigned char*)PTR(3) + 8);
    }
    X(0) = wide_int(host_epoll_ctl((int)X(0), (int)X(1), (int)X(2), events, d));
}
static void g_epoll_wait(CPU* c, void* data) {
    (void)data;
    int most = (int)X(2);
    if (most <= 0 || most > 4096) {
        fail_errno(c, GNU_EINVAL);
        return;
    }
    HostEpollEvent events[64];
    int n = host_epoll_wait((int)X(0), events, most < 64 ? most : 64, (int)X(3));
    for (int i = 0; i < n; ++i) {
        unsigned char* out = (unsigned char*)PTR(1) + 16 * i;
        memset(out, 0, 16);
        snx_st_le32(out, events[i].events);
        snx_st_le64(out + 8, events[i].data);
    }
    X(0) = wide_int(n);
}

/* ---- poll: struct pollfd is {int fd, short events, short revents} everywhere; the events are Linux's ---- */

static void g_poll(CPU* c, void* data) {
    (void)data;
#if defined(__linux__)
    X(0) = wide_int(poll(PTR(0), (nfds_t)X(1), (int)X(2)));
#else
    size_t n = (size_t)X(1);
    if (n > 4096) {
        fail_errno(c, GNU_EINVAL);
        return;
    }
    unsigned char* g = PTR(0);
#ifdef _WIN32
    WSAPOLLFD f[4096];
#else
    struct pollfd f[4096];
#endif
    for (size_t i = 0; i < n; ++i) {
        int fd = (int)snx_ld_le32(g + 8 * i);
#ifdef _WIN32
        f[i].fd = fd < 0 || !host_is_socket(fd) ? INVALID_SOCKET : (SOCKET)(unsigned)fd; /* WSAPoll: sockets */
#else
        f[i].fd = fd;
#endif
        f[i].events = snx_net_poll_to_host(gnu_linux_poll_in(snx_ld_le16(g + 8 * i + 4)));
        f[i].revents = 0;
    }
#ifdef _WIN32
    int r = n ? WSAPoll(f, (ULONG)n, (int)X(2)) : (Sleep(X(2) < 0x80000000u ? (DWORD)X(2) : INFINITE), 0);
#else
    int r = poll(f, (nfds_t)n, (int)X(2));
#endif
    if (r >= 0)
        for (size_t i = 0; i < n; ++i)
            snx_st_le16(g + 8 * i + 6, (uint16_t)gnu_linux_poll_out(snx_net_poll_from_host(f[i].revents)));
    X(0) = wide_int(r);
#endif
}

/* ---- the network: ScarletNX's Network setting. Offline, only loopback and local sockets reach anything, as the
   Switch runtime's bsd service has it ---- */

void hle_set_network(bool on) {
    g_posix.online = on;
}

/* a host address the app may reach offline: loopback, a local (AF_UNIX) socket, or none given */
static bool reachable(const void* address, snx_socklen size) {
    if (g_posix.online || !address) return true;
    SnxSockAddr a;
    struct sockaddr head;
    memcpy(&head, address, size < (snx_socklen)sizeof head ? (size_t)size : sizeof head);
#ifdef AF_UNIX
    if (head.sa_family == AF_UNIX) return true;
#endif
    if (snx_net_addr_from_host(address, size, &a)) return false;
    if (a.family == SNX_AF_INET) return a.addr[0] == 127;
    static const uint8_t loopback6[16] = {[15] = 1};
    return a.family == SNX_AF_INET6 && !memcmp(a.addr, loopback6, 16);
}
static bool local_name(const char* name) {
    return !name || !strcmp(name, "localhost") || !strncmp(name, "127.", 4) || !strcmp(name, "::1");
}

/* -1 and the socket call's error: Winsock's own, errno elsewhere */
static void socket_failed(CPU* c) {
#ifdef _WIN32
    fail_errno(c, -snx_net_errno_from_host(WSAGetLastError()));
#else
    (void)c;
    X(0) = (uint64_t)-1; /* errno is the host's: the end of the call takes it */
#endif
}
static void socket_result(CPU* c, int64_t r) {
    if (r < 0) socket_failed(c);
    else X(0) = (uint64_t)r;
}

/* The guest's address (Linux's layout) as the host's: 0, or a Linux errno. On Linux they are the same bytes */
static int addr_to_host(const void* guest, uint64_t length, struct sockaddr_storage* out, snx_socklen* size) {
    *out = (struct sockaddr_storage){0};
    if (length > sizeof *out) length = sizeof *out;
    if (!guest) {
        *size = 0;
        return length ? GNU_EFAULT : 0;
    }
#if defined(__linux__)
    memcpy(out, guest, (size_t)length);
    *size = (snx_socklen)length;
    return 0;
#else
    if (length < 2) return GNU_EINVAL;
    unsigned char b[GNU_LINUX_SOCKADDR_MAX] = {0};
    memcpy(b, guest, length < sizeof b ? (size_t)length : sizeof b);
    SnxSockAddr a;
    int r = gnu_linux_addr_decode(b, length, &a);
    if (r == 1) {
#ifdef _WIN32
        return SNX_EAFNOSUPPORT;
#else
        /* AF_UNIX: the path after the family, which BSD's sockaddr_un holds 104 bytes of */
        struct sockaddr_un u = {0};
        size_t n = (size_t)length - 2;
        if (n >= sizeof u.sun_path) return SNX_ENAMETOOLONG;
        u.sun_family = AF_UNIX;
        memcpy(u.sun_path, (const char*)guest + 2, n);
        *size = (snx_socklen)(offsetof(struct sockaddr_un, sun_path) + n);
#if defined(__APPLE__) || defined(__FreeBSD__)
        u.sun_len = (unsigned char)*size;
#endif
        memcpy(out, &u, sizeof u);
        return 0;
#endif
    }
    if (r < 0) return -r;
    r = snx_net_addr_to_host(&a, out, size);
    return r < 0 ? -r : 0;
#endif
}
#if !defined(__linux__)
/* the host's address into the guest's (address, *length): what fits of it, and its whole size in *length */
static void addr_to_guest(const struct sockaddr_storage* host, snx_socklen size, uint64_t address, uint64_t length) {
    if (!address || !length) return;
    uint32_t room = snx_ld_le32((const void*)(uintptr_t)length);
    unsigned char b[sizeof(struct sockaddr_storage)] = {0};
    uint32_t n = 0;
    SnxSockAddr a;
    struct sockaddr head;
    memcpy(&head, host, sizeof head);
#ifndef _WIN32
    if (head.sa_family == AF_UNIX) {
        struct sockaddr_un u;
        memcpy(&u, host, sizeof u);
        size_t path = size > (snx_socklen)offsetof(struct sockaddr_un, sun_path)
                          ? (size_t)size - offsetof(struct sockaddr_un, sun_path)
                          : 0;
        if (path > sizeof u.sun_path) path = sizeof u.sun_path;
        snx_st_le16(b, 1);
        memcpy(b + 2, u.sun_path, path);
        n = (uint32_t)(2 + path);
    } else
#endif
        if (!snx_net_addr_from_host((const struct sockaddr*)host, size, &a))
        n = gnu_linux_addr_encode(&a, b);
    memcpy((void*)(uintptr_t)address, b, room < n ? room : n);
    snx_st_le32((void*)(uintptr_t)length, n);
}
#endif

#if !defined(__linux__)
/* Linux's SOCK_NONBLOCK and SOCK_CLOEXEC, in socket()'s type */
enum { L_SOCK_NONBLOCK = 0x800, L_SOCK_CLOEXEC = 0x80000, L_SOCK_TYPE_MASK = 0xf };
/* a new socket's flags, which the host's type may not take */
static void socket_setup(int fd, uint64_t type) {
#ifdef _WIN32
    if (type & L_SOCK_NONBLOCK) {
        u_long on = 1;
        ioctlsocket((SOCKET)(unsigned)fd, FIONBIO, &on);
    }
#else
    if (type & L_SOCK_NONBLOCK) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    if (type & L_SOCK_CLOEXEC) fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int on = 1; /* macOS: no MSG_NOSIGNAL, a write to a closed peer must not raise SIGPIPE */
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
#endif
}
static bool socket_args(uint64_t domain, uint64_t type, int* host_domain, int* host_type) {
    int d = gnu_linux_domain_in((uint32_t)domain);
    *host_domain = d < 0 ? -1 : snx_net_domain_to_host(d);
    *host_type = snx_net_socktype_to_host((int)(type & L_SOCK_TYPE_MASK));
    return *host_domain >= 0 && *host_type >= 0;
}
static void g_socket(CPU* c, void* data) {
    (void)data;
    int d, t;
    if (!socket_args(X(0), X(1), &d, &t)) {
        fail_errno(c, SNX_EAFNOSUPPORT);
        return;
    }
#ifdef _WIN32
    snx_net_init();
    SOCKET s = socket(d, t, (int)X(2));
    if (s == INVALID_SOCKET) {
        socket_failed(c);
        return;
    }
    int fd = (int)(unsigned)s; /* a socket's handle fits 32 bits on 64-bit Windows */
#else
    int fd = socket(d, t, (int)X(2));
    if (fd < 0) {
        socket_failed(c);
        return;
    }
#endif
    socket_setup(fd, X(1));
    X(0) = (uint64_t)fd;
}
static void g_socketpair(CPU* c, void* data) {
    (void)data;
#ifdef _WIN32
    fail_errno(c, SNX_EOPNOTSUPP); /* Winsock has no socketpair */
#else
    int d, t, fds[2];
    if (!socket_args(X(0), X(1), &d, &t)) {
        fail_errno(c, SNX_EAFNOSUPPORT);
        return;
    }
    if (socketpair(d, t, (int)X(2), fds)) {
        socket_failed(c);
        return;
    }
    socket_setup(fds[0], X(1));
    socket_setup(fds[1], X(1));
    memcpy(PTR(3), fds, sizeof fds);
    X(0) = 0;
#endif
}
static void g_bind(CPU* c, void* data) {
    (void)data;
    struct sockaddr_storage a;
    snx_socklen n;
    int e = addr_to_host(PTR(1), X(2), &a, &n);
    if (e) fail_errno(c, e);
    else socket_result(c, bind((int)X(0), (struct sockaddr*)&a, n));
}
static void g_listen(CPU* c, void* data) {
    (void)data;
    socket_result(c, listen((int)X(0), (int)X(1)));
}
static void g_accept(CPU* c, void* data) {
    (void)data;
    struct sockaddr_storage a;
    snx_socklen n = sizeof a;
#ifdef _WIN32
    SOCKET s = accept((SOCKET)X(0), (struct sockaddr*)&a, &n);
    int64_t fd = s == INVALID_SOCKET ? -1 : (int64_t)(unsigned)s;
#else
    int64_t fd = accept((int)X(0), (struct sockaddr*)&a, &n);
#endif
    if (fd >= 0) {
        socket_setup((int)fd, 0);
        addr_to_guest(&a, n, X(1), X(2));
    }
    socket_result(c, fd);
}
static void g_shutdown(CPU* c, void* data) {
    (void)data;
    socket_result(c, shutdown((int)X(0), (int)X(1))); /* SHUT_RD 0 to SHUT_RDWR 2, and Winsock's SD_ the same */
}
static int msg_flags(uint64_t f) {
    return snx_net_msg_to_host(gnu_linux_msg_in(f));
}
static void g_send(CPU* c, void* data) {
    (void)data;
    socket_result(c, send((int)X(0), PTR(1), (int)X(2), msg_flags(X(3))));
}
static void g_recv(CPU* c, void* data) {
    (void)data;
    socket_result(c, recv((int)X(0), PTR(1), (int)X(2), msg_flags(X(3))));
}
static void g_recvfrom(CPU* c, void* data) {
    (void)data;
    struct sockaddr_storage a = {0};
    snx_socklen n = sizeof a;
    int64_t r = recvfrom((int)X(0), PTR(1), (int)X(2), msg_flags(X(3)), X(4) ? (struct sockaddr*)&a : NULL,
                         X(4) ? &n : NULL);
    if (r >= 0 && X(4)) addr_to_guest(&a, n, X(4), X(5));
    socket_result(c, r);
}
static void g_getname(CPU* c, bool peer) {
    struct sockaddr_storage a = {0};
    snx_socklen n = sizeof a;
    int r = peer ? getpeername((int)X(0), (struct sockaddr*)&a, &n) : getsockname((int)X(0), (struct sockaddr*)&a, &n);
    if (!r) addr_to_guest(&a, n, X(1), X(2));
    socket_result(c, r);
}
static void g_getsockname(CPU* c, void* data) {
    (void)data;
    g_getname(c, false);
}
static void g_getpeername(CPU* c, void* data) {
    (void)data;
    g_getname(c, true);
}
/* a guest option as the host's level and name, with its value's layout (linux_net.h): 0 or a Linux errno */
static int option_to_host(uint64_t level, uint64_t name, int* hl, int* hn, int* layout, int* snx_level,
                          int* snx_name) {
    if (gnu_linux_sockopt_in(level, name, snx_level, snx_name, layout)) return SNX_ENOPROTOOPT;
    return snx_net_sockopt_to_host(*snx_level, *snx_name, hl, hn) ? SNX_ENOPROTOOPT : 0;
}
static void g_setsockopt(CPU* c, void* data) {
    (void)data;
    int hl, hn, layout, sl, sn;
    int e = option_to_host(X(1), X(2), &hl, &hn, &layout, &sl, &sn);
    if (e) {
        fail_errno(c, e);
        return;
    }
    const unsigned char* v = PTR(3);
    size_t size = (size_t)X(4);
    if (layout == GNU_LAYOUT_MREQN && size > 8) size = 8; /* ip_mreqn: its interface index goes */
    if (layout == GNU_LAYOUT_IFADDR && size >= 8) {      /* ip_mreq(n): the address */
        v += 4;
        size = 4;
    }
#ifdef _WIN32
    /* Winsock's timeouts are a DWORD of milliseconds, the guest's a timeval of two 64-bit numbers */
    DWORD ms = 0;
    if (sl == SNX_SOL_SOCKET && (sn == SNX_SO_RCVTIMEO || sn == SNX_SO_SNDTIMEO) && size >= 16) {
        ms = (DWORD)(snx_ld_le64(v) * 1000 + snx_ld_le64(v + 8) / 1000);
        v = (const unsigned char*)&ms;
        size = sizeof ms;
    }
    socket_result(c, setsockopt((SOCKET)X(0), hl, hn, (const char*)v, (int)size));
#else
    socket_result(c, setsockopt((int)X(0), hl, hn, v, (socklen_t)size));
#endif
}
static void g_getsockopt(CPU* c, void* data) {
    (void)data;
    int hl, hn, layout, sl, sn;
    int e = option_to_host(X(1), X(2), &hl, &hn, &layout, &sl, &sn);
    if (e) {
        fail_errno(c, e);
        return;
    }
    snx_socklen size = (snx_socklen)snx_ld_le32(PTR(4));
#ifdef _WIN32
    if (sl == SNX_SOL_SOCKET && (sn == SNX_SO_RCVTIMEO || sn == SNX_SO_SNDTIMEO) && size >= 16) {
        DWORD ms = 0;
        int n = sizeof ms;
        if (getsockopt((SOCKET)X(0), hl, hn, (char*)&ms, &n)) {
            socket_failed(c);
            return;
        }
        snx_st_le64(PTR(3), ms / 1000);
        snx_st_le64((unsigned char*)PTR(3) + 8, (uint64_t)(ms % 1000) * 1000);
        snx_st_le32(PTR(4), 16);
        X(0) = 0;
        return;
    }
#endif
    int r = getsockopt((int)X(0), hl, hn, PTR(3), &size);
    if (!r) {
        /* SO_ERROR and SO_TYPE: the host's numbers as the guest's */
        if (sl == SNX_SOL_SOCKET && sn == SNX_SO_ERROR && size >= 4) {
            int32_t v = (int32_t)snx_ld_le32(PTR(3));
            snx_st_le32(PTR(3), (uint32_t)(v ? -snx_net_errno_from_host(v) : 0));
        } else if (sl == SNX_SOL_SOCKET && sn == SNX_SO_TYPE && size >= 4)
            snx_st_le32(PTR(3), (uint32_t)snx_net_socktype_from_host((int)snx_ld_le32(PTR(3))));
        snx_st_le32(PTR(4), (uint32_t)size);
    }
    socket_result(c, r);
}

/* Linux's msghdr on AArch64 (56 bytes): name 0, namelen 8, iov 16, iovlen 24, control 32, controllen 40, flags 48.
   Control messages are not passed: their layout is the host's own (recvmsg reports none) */
typedef struct {
    uint64_t name;
    uint32_t namelen;
    uint64_t iov, iovlen, control, controllen;
    uint32_t flags;
} GuestMsg;
static GuestMsg read_msg(const unsigned char* m) {
    GuestMsg g = {snx_ld_le64(m), snx_ld_le32(m + 8), snx_ld_le64(m + 16), snx_ld_le64(m + 24),
                  snx_ld_le64(m + 32), snx_ld_le64(m + 40), snx_ld_le32(m + 48)};
    return g;
}
/* the iovec's bytes in one buffer (Windows' sendmsg is WSASendMsg, which this leaves out): its size, or -1 */
static int64_t gather(const GuestMsg* g, unsigned char** out) {
    size_t total = 0;
    for (uint64_t i = 0; i < g->iovlen; ++i) total += snx_ld_le64((const unsigned char*)(uintptr_t)g->iov + 16 * i + 8);
    *out = malloc(total ? total : 1);
    if (!*out) return -1;
    size_t at = 0;
    for (uint64_t i = 0; i < g->iovlen; ++i) {
        const unsigned char* v = (const unsigned char*)(uintptr_t)g->iov + 16 * i;
        size_t n = snx_ld_le64(v + 8);
        memcpy(*out + at, (const void*)(uintptr_t)snx_ld_le64(v), n);
        at += n;
    }
    return (int64_t)total;
}
static void scatter(const GuestMsg* g, const unsigned char* bytes, size_t size) {
    size_t at = 0;
    for (uint64_t i = 0; i < g->iovlen && at < size; ++i) {
        const unsigned char* v = (const unsigned char*)(uintptr_t)g->iov + 16 * i;
        size_t n = snx_ld_le64(v + 8);
        if (n > size - at) n = size - at;
        memcpy((void*)(uintptr_t)snx_ld_le64(v), bytes + at, n);
        at += n;
    }
}
static void g_sendmsg(CPU* c, void* data) {
    (void)data;
    GuestMsg g = read_msg(PTR(1));
    struct sockaddr_storage a;
    snx_socklen n = 0;
    if (g.name) {
        int e = addr_to_host((const void*)(uintptr_t)g.name, g.namelen, &a, &n);
        if (e) {
            fail_errno(c, e);
            return;
        }
        if (!reachable(&a, n)) {
            fail_errno(c, SNX_ENETUNREACH);
            return;
        }
    }
    unsigned char* bytes;
    int64_t size = gather(&g, &bytes);
    if (size < 0) {
        fail_errno(c, GNU_ENOMEM);
        return;
    }
    socket_result(c, sendto((int)X(0), (const void*)bytes, (int)size, msg_flags(X(2)),
                            g.name ? (struct sockaddr*)&a : NULL, n));
    free(bytes);
}
static void g_recvmsg(CPU* c, void* data) {
    (void)data;
    unsigned char* m = PTR(1);
    GuestMsg g = read_msg(m);
    size_t total = 0;
    for (uint64_t i = 0; i < g.iovlen; ++i) total += snx_ld_le64((const unsigned char*)(uintptr_t)g.iov + 16 * i + 8);
    unsigned char* bytes = malloc(total ? total : 1);
    if (!bytes) {
        fail_errno(c, GNU_ENOMEM);
        return;
    }
    struct sockaddr_storage a = {0};
    snx_socklen n = sizeof a;
    int64_t r = recvfrom((int)X(0), (void*)bytes, (int)total, msg_flags(X(2)), g.name ? (struct sockaddr*)&a : NULL,
                         g.name ? &n : NULL);
    if (r >= 0) {
        scatter(&g, bytes, (size_t)r);
        if (g.name) {
            snx_st_le32(m + 8, g.namelen);
            addr_to_guest(&a, n, g.name, (uint64_t)(uintptr_t)(m + 8));
        }
        snx_st_le64(m + 40, 0); /* no control messages */
        snx_st_le32(m + 48, (uint64_t)r > total ? 0x20 : 0); /* MSG_TRUNC */
    }
    free(bytes);
    socket_result(c, r);
}
#endif

static void g_connect(CPU* c, void* data) {
    (void)data;
    struct sockaddr_storage a;
    snx_socklen n;
    int e = addr_to_host(PTR(1), X(2), &a, &n);
    if (e) {
        fail_errno(c, e);
        return;
    }
    if (!reachable(&a, n)) {
        fail_errno(c, SNX_ENETUNREACH);
        return;
    }
#ifdef _WIN32
    int r = connect((SOCKET)X(0), (struct sockaddr*)&a, n);
    /* a non-blocking connect that has begun: Winsock says it would block, POSIX that it is in progress */
    if (r && WSAGetLastError() == WSAEWOULDBLOCK) {
        fail_errno(c, SNX_EINPROGRESS);
        return;
    }
    socket_result(c, r);
#else
    socket_result(c, connect((int)X(0), (struct sockaddr*)&a, n));
#endif
}
static void g_sendto(CPU* c, void* data) {
    (void)data;
    struct sockaddr_storage a;
    snx_socklen n = 0;
    if (X(4)) {
        int e = addr_to_host(PTR(4), X(5), &a, &n);
        if (e) {
            fail_errno(c, e);
            return;
        }
    }
    if (X(4) && !reachable(&a, n)) {
        fail_errno(c, SNX_ENETUNREACH);
        return;
    }
#if defined(__linux__)
    socket_result(c, sendto((int)X(0), PTR(1), X(2), (int)X(3), X(4) ? (struct sockaddr*)&a : NULL, n));
#else
    socket_result(c, sendto((int)X(0), PTR(1), (int)X(2), msg_flags(X(3)), X(4) ? (struct sockaddr*)&a : NULL, n));
#endif
}
/* the FORTIFY forms (_FORTIFY_SOURCE, which the NDK turns on): the plain call after bionic's check that the buffer
   holds the length. __sendto_chk(fd, buffer, length, buffer_size, flags, address, address_length), which send() is
   too, __recvfrom_chk(fd, buffer, length, buffer_size, flags, address, address_length), which recv() is too, and
   __readlink_chk(path, buffer, size, buffer_size) */
static void fortify_check(uint64_t length, uint64_t room, const char* what) {
    if (length <= room) return;
    fprintf(stderr, "scarletnx-android: FORTIFY: %s: a length of %llu bytes, its buffer %llu\n", what,
            (unsigned long long)length, (unsigned long long)room);
    abort();
}
static void g_sendto_chk(CPU* c, void* data) {
    fortify_check(X(2), X(3), "sendto");
    X(3) = X(4);
    X(4) = X(5);
    X(5) = X(6);
    g_sendto(c, data);
}
static void g_recvfrom_chk(CPU* c, void* data) {
    fortify_check(X(2), X(3), "recvfrom");
    X(3) = X(4);
    X(4) = X(5);
    X(5) = X(6);
#if defined(__linux__)
    hle_pass(c, (void*)recvfrom);
    (void)data;
#else
    g_recvfrom(c, data);
#endif
}
static void g_readlink_chk(CPU* c, void* data) {
    fortify_check(X(2), X(3), "readlink");
    g_readlink_proc(c, data);
}
#if defined(__linux__)
static void g_sendmsg(CPU* c, void* data) {
    (void)data;
    const struct msghdr* m = PTR(1);
    if (m && m->msg_name && !reachable(m->msg_name, m->msg_namelen)) {
        fail_errno(c, SNX_ENETUNREACH);
        return;
    }
    socket_result(c, sendmsg((int)X(0), m, (int)X(2)));
}
#endif

/* ---- names: through the network stack (net.h), whose flags and errors are bionic's (BSD's values) ---- */

/* bionic's addrinfo: flags, family, socktype, protocol, addrlen, canonname, addr, next */
typedef struct GuestAddrinfo {
    int32_t flags, family, socktype, protocol;
    uint32_t addrlen;
    char* canonname;
    void* addr;
    struct GuestAddrinfo* next;
} GuestAddrinfo;

static GuestAddrinfo* to_guest(const SnxAddrInfo* a) {
    GuestAddrinfo *first = NULL, **tail = &first;
    for (; a; a = a->next) {
        unsigned char b[GNU_LINUX_SOCKADDR_MAX];
        uint32_t n = gnu_linux_addr_encode(&a->addr, b);
        GuestAddrinfo* g = calloc(1, sizeof *g + n);
        if (!g) break;
        g->flags = a->flags;
        g->family = a->family;
        g->socktype = a->socktype;
        g->protocol = a->protocol;
        g->addrlen = n;
        g->canonname = a->canonname ? snx_strdup(a->canonname) : NULL;
        g->addr = n ? (void*)(g + 1) : NULL;
        memcpy(g + 1, b, n);
        *tail = g;
        tail = &g->next;
    }
    return first;
}
static void g_getaddrinfo(CPU* c, void* data) {
    (void)data;
    SnxAddrInfo hints = {0}, *result = NULL;
    const GuestAddrinfo* h = PTR(2);
    if (h) {
        hints.flags = h->flags;
        hints.family = h->family;
        hints.socktype = h->socktype;
        hints.protocol = h->protocol;
    }
    /* offline, a name that isn't this machine's doesn't resolve: no DNS server is asked, and the answer is Android's
       without a network, "No address associated with hostname" */
    int r = local_name(STR(0)) || g_posix.online ? snx_net_getaddrinfo(STR(0), STR(1), h ? &hints : NULL, &result)
                                                 : SNX_EAI_NODATA;
    GuestAddrinfo* out = r ? NULL : to_guest(result);
    snx_net_freeaddrinfo(result);
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
static void g_gai_strerror(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(uintptr_t)snx_net_gai_strerror((int)X(0));
}
/* getnameinfo(addr, addrlen, host, hostlen, serv, servlen, flags) */
static void g_getnameinfo(CPU* c, void* data) {
    (void)data;
    unsigned char b[GNU_LINUX_SOCKADDR_MAX] = {0};
    memcpy(b, PTR(0), X(1) < sizeof b ? (size_t)X(1) : sizeof b);
    SnxSockAddr a;
    if (gnu_linux_addr_decode(b, X(1), &a)) {
        X(0) = (uint64_t)(int64_t)SNX_EAI_FAMILY;
        return;
    }
    X(0) = (uint64_t)(int64_t)snx_net_getnameinfo(&a, PTR(2), (size_t)X(3), PTR(4), (size_t)X(5), (int)X(6));
}
/* gethostbyname: bionic's hostent {name, aliases, addrtype (int), length (int), addr_list}, of this thread, for a
   name's IPv4 addresses. h_errno is left as it is */
/* getifaddrs(list): the loopback interface, lo, alone, in bionic's struct ifaddrs (next, name, flags, addr, netmask,
   the broadcast or destination address, data) and Linux's sockaddr_in. An app that looks for its network's broadcast
   addresses (a game's LAN search) finds none, as on a device without Wi-Fi; one block, which freeifaddrs frees */
static void g_getifaddrs(CPU* c, void* data) {
    (void)data;
    enum { ENTRY = 56, NAME = ENTRY, ADDR = NAME + 8, MASK = ADDR + 16, SIZE = MASK + 16 };
    unsigned char* block = calloc(1, SIZE);
    if (!block || !X(0)) {
        free(block);
        hle_set_errno(GNU_ENOMEM);
        X(0) = (uint64_t)-1;
        return;
    }
    memcpy(block + NAME, "lo", 3);
    static const unsigned char loopback[8] = {2, 0, 0, 0, 127, 0, 0, 1};  /* AF_INET, port 0, 127.0.0.1 */
    static const unsigned char netmask[8] = {2, 0, 0, 0, 255, 0, 0, 0};   /* 255.0.0.0 */
    memcpy(block + ADDR, loopback, sizeof loopback);
    memcpy(block + MASK, netmask, sizeof netmask);
    uint64_t name = (uint64_t)(uintptr_t)(block + NAME), addr = (uint64_t)(uintptr_t)(block + ADDR);
    uint64_t mask = (uint64_t)(uintptr_t)(block + MASK);
    uint32_t flags = 0x1 | 0x8 | 0x40; /* IFF_UP, IFF_LOOPBACK, IFF_RUNNING */
    memcpy(block + 8, &name, 8);
    memcpy(block + 16, &flags, 4);
    memcpy(block + 24, &addr, 8);
    memcpy(block + 32, &mask, 8);
    uint64_t list = (uint64_t)(uintptr_t)block;
    memcpy(PTR(0), &list, 8);
    X(0) = 0;
}
static void g_freeifaddrs(CPU* c, void* data) {
    (void)data;
    free(PTR(0));
}

static void g_gethostbyname(CPU* c, void* data) {
    (void)data;
    SnxAddrInfo hints = {0}, *result = NULL;
    hints.family = SNX_AF_INET;
    hints.socktype = SNX_SOCK_STREAM;
    if (!local_name(STR(0)) && !g_posix.online) {
        X(0) = 0;
        return;
    }
    if (snx_net_getaddrinfo(STR(0), NULL, &hints, &result)) {
        X(0) = 0;
        return;
    }
    size_t n = 0;
    for (const SnxAddrInfo* a = result; a && n < 8; a = a->next)
        if (a->addr.family == SNX_AF_INET) {
            memcpy(t_posix.host.bytes[n], a->addr.addr, 4);
            t_posix.host.addresses[n] = (uint64_t)(uintptr_t)t_posix.host.bytes[n];
            ++n;
        }
    t_posix.host.addresses[n] = 0;
    snx_net_freeaddrinfo(result);
    snprintf(t_posix.host.text, sizeof t_posix.host.text, "%s", STR(0));
    t_posix.host.alias_list[0] = 0;
    t_posix.host.name = (uint64_t)(uintptr_t)t_posix.host.text;
    t_posix.host.aliases = (uint64_t)(uintptr_t)t_posix.host.alias_list;
    t_posix.host.addrtype = SNX_AF_INET;
    t_posix.host.length = 4;
    t_posix.host.list = (uint64_t)(uintptr_t)t_posix.host.addresses;
    X(0) = n ? (uint64_t)(uintptr_t)&t_posix.host : 0;
}
#if !defined(__linux__)
/* inet_ntop and inet_pton take a family: bionic's (Linux's) number as the host's */
static void g_inet_ntop(CPU* c, void* data) {
    (void)data;
    int d = gnu_linux_domain_in((uint32_t)X(0));
    X(0) = (uint64_t)(uintptr_t)inet_ntop(d < 0 ? -1 : snx_net_domain_to_host(d), PTR(1), PTR(2), (snx_socklen)X(3));
}
static void g_inet_pton(CPU* c, void* data) {
    (void)data;
    int d = gnu_linux_domain_in((uint32_t)X(0));
    X(0) = wide_int(inet_pton(d < 0 ? -1 : snx_net_domain_to_host(d), STR(1), PTR(2)));
}
#endif
#ifdef _WIN32
static void g_inet_aton(CPU* c, void* data) {
    (void)data;
    X(0) = wide_int(inet_pton(AF_INET, STR(0), PTR(1)) == 1);
}
#endif

/* ---- sysconf: bionic's names ---- */

static int64_t host_conf(int name) {
#ifdef _WIN32
    SYSTEM_INFO s;
    GetSystemInfo(&s);
    MEMORYSTATUSEX m = {0};
    m.dwLength = sizeof m;
    switch (name) {
    case 0x00: return 32768; /* _SC_ARG_MAX */
    case 0x05: return 999;   /* _SC_CHILD_MAX */
    case 0x06: return 100;   /* _SC_CLK_TCK */
    case 0x0b: return 8192;  /* _SC_OPEN_MAX */
    case 0x60:
    case 0x61: return (int64_t)s.dwNumberOfProcessors;
    case 0x62: return GlobalMemoryStatusEx(&m) ? (int64_t)(m.ullTotalPhys / 4096) : -1;
    case 0x63: return GlobalMemoryStatusEx(&m) ? (int64_t)(m.ullAvailPhys / 4096) : -1;
    }
    return -1;
#else
    int64_t page = sysconf(_SC_PAGESIZE);
    switch (name) {
    case 0x00: return sysconf(_SC_ARG_MAX);
    case 0x05: return sysconf(_SC_CHILD_MAX);
    case 0x06: return sysconf(_SC_CLK_TCK);
    case 0x0b: return sysconf(_SC_OPEN_MAX);
    case 0x60: return sysconf(_SC_NPROCESSORS_CONF);
    case 0x61: return sysconf(_SC_NPROCESSORS_ONLN);
    case 0x62: return sysconf(_SC_PHYS_PAGES) * (page / 4096);
    case 0x63:
#ifdef _SC_AVPHYS_PAGES
        return sysconf(_SC_AVPHYS_PAGES) * (page / 4096);
#else
        return sysconf(_SC_PHYS_PAGES) * (page / 4096) / 2; /* macOS and FreeBSD don't say: half */
#endif
    }
    return -1;
#endif
}
static void g_sysconf(CPU* c, void* data) {
    (void)data;
    int64_t r;
    switch (X(0)) {
    case 0x00:
    case 0x05:
    case 0x06:
    case 0x0b:
    case 0x60:
    case 0x61:
    case 0x62:
    case 0x63: r = host_conf((int)X(0)); break;
    case 0x27:
    case 0x28: r = 4096; break; /* _SC_PAGESIZE, _SC_PAGE_SIZE: the guest's pages */
    case 0x64: r = 1; break;    /* _SC_MONOTONIC_CLOCK */
    default:
        fprintf(stderr, "scarletnx-android: sysconf(0x%llx): unknown, -1\n", (unsigned long long)X(0));
        hle_set_errno(GNU_EINVAL);
        r = -1;
        break;
    }
    X(0) = (uint64_t)r;
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
    default: X(0) = 0; hle_set_errno(GNU_ENOENT); break;
    }
}

/* ---- time: bionic's clock() counts microseconds; struct tm has tm_gmtoff and tm_zone after the nine ints ---- */

static void g_clock(CPU* c, void* data) {
    (void)data;
    clock_t t = clock();
    X(0) = t == (clock_t)-1 ? (uint64_t)-1 : (uint64_t)((int64_t)t * (1000000 / CLOCKS_PER_SEC));
}
#ifdef _WIN32
/* Windows' struct tm is the nine ints alone: bionic's (56 bytes) has the offset from UTC and the zone's name after */
static void tm_to_guest(const struct tm* t, void* out, int64_t offset, const char* zone) {
    unsigned char b[56] = {0};
    memcpy(b, t, 9 * sizeof(int));
    snx_st_le64(b + 40, (uint64_t)offset);
    snx_st_le64(b + 48, (uint64_t)(uintptr_t)zone);
    memcpy(out, b, sizeof b);
}
static void g_gmtime_r(CPU* c, void* data) {
    (void)data;
    struct tm t;
    if (_gmtime64_s(&t, (const __time64_t*)PTR(0))) {
        X(0) = 0;
        return;
    }
    tm_to_guest(&t, PTR(1), 0, "GMT");
}
static void g_gmtime(CPU* c, void* data) {
    (void)data;
    X(1) = (uint64_t)(uintptr_t)t_posix.tm;
    g_gmtime_r(c, NULL);
    if (X(0)) X(0) = (uint64_t)(uintptr_t)t_posix.tm;
}
static void g_localtime_r(CPU* c, void* data) {
    (void)data;
    struct tm t;
    if (_localtime64_s(&t, (const __time64_t*)PTR(0))) {
        X(0) = 0;
        return;
    }
    read_timezone();
    tm_to_guest(&t, PTR(1), -g_posix.timezone_ + (t.tm_isdst > 0 ? 3600 : 0), _tzname[t.tm_isdst > 0]);
}
static void g_localtime(CPU* c, void* data) {
    (void)data;
    X(1) = (uint64_t)(uintptr_t)t_posix.tm;
    g_localtime_r(c, NULL);
    if (X(0)) X(0) = (uint64_t)(uintptr_t)t_posix.tm;
}
static void g_mktime(CPU* c, void* data) {
    (void)data;
    struct tm t;
    memcpy(&t, PTR(0), 9 * sizeof(int));
    __time64_t r = _mktime64(&t);
    memcpy(PTR(0), &t, 9 * sizeof(int));
    X(0) = (uint64_t)r;
}
static void g_strftime(CPU* c, void* data) {
    (void)data;
    struct tm t;
    memcpy(&t, PTR(3), 9 * sizeof(int));
    X(0) = strftime(PTR(0), (size_t)X(1), STR(2), &t);
}
#endif
static void g_tzset(CPU* c, void* data) {
    (void)data;
    (void)c;
    read_timezone();
    read_zone_names();
}

/* ---- the user: on Linux the host's passwd (glibc's layout is bionic's); elsewhere an Android app's, as bionic's
   getpwuid gives it ---- */
#if !defined(__linux__)
static uint64_t app_passwd(uint64_t uid) {
    static const char name[] = "u0_a100", none[] = "", dir[] = "/data", shell[] = "/system/bin/sh";
    t_posix.passwd.name = (uint64_t)(uintptr_t)name;
    t_posix.passwd.password = (uint64_t)(uintptr_t)none;
    t_posix.passwd.uid = t_posix.passwd.gid = (uint32_t)uid;
    t_posix.passwd.gecos = (uint64_t)(uintptr_t)none;
    t_posix.passwd.dir = (uint64_t)(uintptr_t)dir;
    t_posix.passwd.shell = (uint64_t)(uintptr_t)shell;
    return (uint64_t)(uintptr_t)&t_posix.passwd;
}
static void g_getpwuid(CPU* c, void* data) {
    (void)data;
    X(0) = app_passwd(X(0));
}
/* getpwuid_r(uid, pwd, buffer, size, result) */
static void g_getpwuid_r(CPU* c, void* data) {
    (void)data;
    memcpy(PTR(1), (const void*)(uintptr_t)app_passwd(X(0)), sizeof t_posix.passwd);
    memcpy(PTR(4), &c->x[1], sizeof(uint64_t));
    X(0) = 0;
}
#endif

/* ---- what an app may not do in its sandbox: fork, exec, ptrace ---- */
static void g_ptrace(CPU* c, void* data) {
    (void)data;
    fail_errno(c, GNU_EPERM);
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
        fail_errno(c, GNU_EINVAL);
        return;
    }
    uint64_t set = snx_ld_le64(PTR(0));
    set = on ? set | (1ull << (sig - 1)) : set & ~(1ull << (sig - 1));
    snx_st_le64(PTR(0), set);
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
static void g_sigismember(CPU* c, void* data) {
    (void)data;
    int sig = (int)X(1);
    if (sig < 1 || sig > 64) {
        fail_errno(c, GNU_EINVAL);
        return;
    }
    X(0) = snx_ld_le64(PTR(0)) >> (sig - 1) & 1;
}
#if defined(__linux__)
static void host_set(uint64_t guest, sigset_t* out) {
    sigemptyset(out);
    uint64_t set = snx_ld_le64((const void*)(uintptr_t)guest);
    for (int s = 1; s <= 64; ++s)
        if ((set >> (s - 1)) & 1) sigaddset(out, s);
}
static void g_signalfd(CPU* c, void* data) {
    (void)data;
    sigset_t set;
    host_set(X(1), &set);
    X(0) = wide_int(signalfd((int)X(0), &set, (int)X(3)));
}
#endif

/* ---- syscall() and the bionic functions that are one system call: the GNU ABI runtime's kernel ---- */

/* number with x0 to x5 (from x1 for syscall()), its result as bionic's: -1 and errno for an error */
static void kernel_result(CPU* c, long number, unsigned first) {
    const uint64_t arguments[6] = {X(first), X(first + 1), X(first + 2), X(first + 3), X(first + 4), X(first + 5)};
    bool known = true;
    long r = hle_kernel_syscall(number, arguments, c, &known);
    if (!known) fprintf(stderr, "scarletnx-android: syscall(%ld): not answered, ENOSYS\n", number);
    if (r < 0) {
        hle_set_errno((int)-r);
        r = -1;
    }
    X(0) = (uint64_t)(int64_t)r;
}
static void g_syscall(CPU* c, void* data) {
    (void)data;
    kernel_result(c, (long)X(0), 1);
}
/* a bionic function that is the system call in data, with its arguments */
static void kernel_function(CPU* c, void* data) {
    kernel_result(c, (long)(uintptr_t)data, 0);
}
#define K(name, number) {#name, kernel_function, (void*)(uintptr_t)(number)}
/* clock_nanosleep(): the error number itself */
static void g_clock_nanosleep(CPU* c, void* data) {
    (void)data;
    const uint64_t arguments[6] = {X(0), X(1), X(2), X(3), 0, 0};
    X(0) = (uint64_t)-hle_kernel_syscall(115, arguments, c, NULL);
}
/* usleep() and sleep(): the kernel's nanosleep, which a guest signal ends as Linux's does */
static void g_usleep(CPU* c, void* data) {
    (void)data;
    uint8_t t[16];
    snx_st_le64(t, X(0) / 1000000);
    snx_st_le64(t + 8, X(0) % 1000000 * 1000);
    const uint64_t arguments[6] = {(uint64_t)(uintptr_t)t, 0, 0, 0, 0, 0};
    long r = hle_kernel_syscall(101, arguments, c, NULL);
    if (r < 0) hle_set_errno((int)-r);
    X(0) = r < 0 ? (uint64_t)-1 : 0;
}
static void g_sleep(CPU* c, void* data) {
    (void)data;
    uint8_t t[16] = {0}, left[16] = {0};
    uint64_t seconds = (uint32_t)X(0);
    snx_st_le64(t, seconds);
    const uint64_t arguments[6] = {(uint64_t)(uintptr_t)t, (uint64_t)(uintptr_t)left, 0, 0, 0, 0};
    long r = hle_kernel_syscall(101, arguments, c, NULL);
    X(0) = r < 0 ? snx_ld_le64(left) + (snx_ld_le64(left + 8) ? 1 : 0) : 0;
}
/* getentropy(buf, length): at most 256 bytes, all of them, 0 or -1 */
static void g_getentropy(CPU* c, void* data) {
    (void)data;
    if (X(1) > 256) {
        fail_errno(c, GNU_EIO);
        return;
    }
    const uint64_t arguments[6] = {X(0), X(1), 0, 0, 0, 0};
    long r = hle_kernel_syscall(278, arguments, c, NULL);
    if (r < 0) hle_set_errno((int)-r);
    X(0) = r == (long)X(1) ? 0 : (uint64_t)-1;
}
static void g_arc4random(CPU* c, void* data) {
    (void)data;
    uint8_t v[4] = {0};
    const uint64_t arguments[6] = {(uint64_t)(uintptr_t)v, sizeof v, 0, 0, 0, 0};
    (void)hle_kernel_syscall(278, arguments, c, NULL);
    X(0) = snx_ld_le32(v);
}
static void g_arc4random_buf(CPU* c, void* data) {
    (void)data;
    const uint64_t arguments[6] = {X(0), X(1), 0, 0, 0, 0};
    (void)hle_kernel_syscall(278, arguments, c, NULL); /* the kernel's call gives the whole of it */
}
#ifdef _WIN32
/* openlog, closelog: syslog is the log's (hle_libc.c), with no connection to open */
static void nothing(CPU* c, void* data) {
    (void)c;
    (void)data;
}
#endif

#define P(name)          {#name, hle_pass, (void*)name}
#define PH(name, host)   {#name, hle_pass, (void*)host}
#define A(name, adapter) {#name, adapter, NULL}

const HleSymbol hle_posix[] = {
    {"__sF", NULL, g_posix.sF},
    {"stdin", NULL, &g_posix.guest_stdin},
    {"stdout", NULL, &g_posix.guest_stdout},
    {"stderr", NULL, &g_posix.guest_stderr},
    {"environ", NULL, &g_posix.environment},
    FILE_CALLS(FILE_CALL_ENTRY)
    A(__fread_chk, g_fread_chk), A(__fwrite_chk, g_fwrite_chk), A(getdelim, g_getdelim), A(getline, g_getline),
    A(fopen, g_fopen), A(fdopen, g_fdopen), A(rename, g_rename), A(setvbuf, g_setvbuf), P(tmpfile), P(remove),
    A(tmpnam, refused), A(putchar, g_putchar), A(puts, g_puts), A(perror, g_perror), A(popen, g_popen),
    A(open, g_open), A(__open_2, g_open_2), A(fcntl, g_fcntl), A(ioctl, g_ioctl),
    A(stat, g_stat), A(lstat, g_lstat), A(fstat, g_fstat), A(fstat64, g_fstat), A(close, g_close),
    P(chmod), P(dup), P(dup2), P(isatty), P(rmdir), P(unlink), P(opendir), P(closedir),
    /* the process's working folder, which the JVM's relative paths share, as Android's do */
    P(chdir),
#if defined(_WIN32)
    A(statfs, g_statfs), A(access, g_access_proc), A(fchmod, refused), A(flock, g_flock), A(fsync, g_fsync),
    A(ftruncate, g_ftruncate), A(lseek, g_lseek), A(lseek64, g_lseek), A(mkdir, g_mkdir), A(pipe, g_pipe),
    A(pread, g_pread), A(read, g_read), A(readlink, g_readlink_proc), A(realpath, g_realpath), A(truncate, g_truncate),
    A(utime, g_utime), A(write, g_write), A(getcwd, g_getcwd), A(readdir, g_readdir),
#else
    A(access, g_access_proc), P(fchmod), P(flock), P(fsync), P(ftruncate), P(lseek), P(mkdir), P(pipe), P(pread), P(read),
    A(readlink, g_readlink_proc), P(realpath), P(truncate), P(utime), P(write), P(getcwd),
#if defined(__linux__)
    P(statfs), P(lseek64), P(readdir),
#else
    A(statfs, g_statfs), PH(lseek64, lseek), A(readdir, g_readdir),
#endif
#endif
#if defined(__linux__)
    P(inotify_init1), P(inotify_add_watch),
#else
    A(inotify_init1, refused), A(inotify_add_watch, refused),
#endif
    A(mmap, g_mmap), A(mprotect, g_mprotect), A(munmap, g_munmap), K(madvise, 233), A(msync, g_msync),
    A(mincore, g_mincore), A(mallinfo, g_mallinfo),
    A(epoll_create, g_epoll_create), A(epoll_create1, g_epoll_create1), A(epoll_ctl, g_epoll_ctl),
    A(epoll_wait, g_epoll_wait), A(poll, g_poll),
#if defined(_WIN32)
    A(select, refused),
#else
    P(select), /* fd_set's bits and timeval's fields are where bionic has them on every POSIX host */
#endif
#if defined(__linux__)
    P(socket), P(socketpair), P(bind), P(listen), P(accept), P(shutdown), P(send), P(recv), P(recvfrom),
    P(recvmsg), P(getsockname), P(getpeername), P(getsockopt), P(setsockopt), P(inet_ntop), P(inet_pton),
    P(inet_aton),
#else
    A(socket, g_socket), A(socketpair, g_socketpair), A(bind, g_bind), A(listen, g_listen), A(accept, g_accept),
    A(shutdown, g_shutdown), A(send, g_send), A(recv, g_recv), A(recvfrom, g_recvfrom), A(recvmsg, g_recvmsg),
    A(getsockname, g_getsockname), A(getpeername, g_getpeername), A(getsockopt, g_getsockopt),
    A(setsockopt, g_setsockopt), A(inet_ntop, g_inet_ntop), A(inet_pton, g_inet_pton),
#ifdef _WIN32
    A(inet_aton, g_inet_aton),
#else
    P(inet_aton),
#endif
#endif
    A(connect, g_connect), A(sendto, g_sendto), A(sendmsg, g_sendmsg), A(gethostbyname, g_gethostbyname),
    A(__sendto_chk, g_sendto_chk), A(__recvfrom_chk, g_recvfrom_chk), A(__readlink_chk, g_readlink_chk),
    A(getaddrinfo, g_getaddrinfo), A(freeaddrinfo, g_freeaddrinfo), A(getifaddrs, g_getifaddrs),
    A(freeifaddrs, g_freeifaddrs), A(gai_strerror, g_gai_strerror),
    A(getnameinfo, g_getnameinfo), P(gethostname), P(inet_addr), P(inet_ntoa), P(if_nametoindex),
    A(clock, g_clock), K(clock_getres, 114), K(clock_gettime, 113), K(gettimeofday, 169), P(time), K(nanosleep, 101),
    A(clock_nanosleep, g_clock_nanosleep), A(usleep, g_usleep), A(sleep, g_sleep), A(tzset, g_tzset),
#ifdef _WIN32
    A(gmtime, g_gmtime), A(gmtime_r, g_gmtime_r), A(localtime, g_localtime), A(localtime_r, g_localtime_r),
    A(mktime, g_mktime), A(strftime, g_strftime), A(strftime_l, g_strftime), A(alarm, refused),
    A(setenv, g_setenv), A(openlog, nothing), A(closelog, nothing),
#else
    P(gmtime), P(gmtime_r), P(localtime), P(localtime_r), P(mktime), P(strftime), PH(strftime_l, strftime), P(alarm),
    P(setenv), P(openlog), P(closelog),
#endif
    K(getpid, 172), K(getppid, 173), K(gettid, 178), K(getuid, 174), K(geteuid, 175), K(getgid, 176),
    K(getegid, 177), K(getrlimit, 163), K(setrlimit, 164), K(prlimit, 261), K(getrusage, 165), K(sysinfo, 179),
    K(times, 153), K(uname, 160), K(prctl, 167), K(setpriority, 140), K(sched_yield, 124),
    K(sched_get_priority_max, 125), K(sched_get_priority_min, 126), K(sched_getscheduler, 120),
    K(sched_getparam, 121), K(sched_setscheduler, 119), K(sched_setparam, 118), K(getrandom, 278), P(getenv),
#if defined(__linux__)
    P(getpwuid), P(getpwuid_r),
#else
    A(getpwuid, g_getpwuid), A(getpwuid_r, g_getpwuid_r),
#endif
    A(getentropy, g_getentropy), A(sysconf, g_sysconf), A(getpagesize, g_getpagesize), A(getauxval, g_getauxval),
    A(arc4random, g_arc4random), A(arc4random_buf, g_arc4random_buf),
    A(fork, refused), A(execl, refused), A(system, refused), A(waitpid, refused), A(ptrace, g_ptrace),
    A(sigemptyset, g_sigemptyset), A(sigfillset, g_sigfillset), A(sigaddset, g_sigaddset),
    A(sigdelset, g_sigdelset), A(sigismember, g_sigismember),
    /* sigset64_t is sigset_t on LP64 */
    A(sigemptyset64, g_sigemptyset), A(sigfillset64, g_sigfillset), A(sigaddset64, g_sigaddset),
    A(sigdelset64, g_sigdelset), A(sigismember64, g_sigismember),
#if defined(__linux__)
    A(signalfd, g_signalfd),
#else
    A(signalfd, refused),
#endif
    A(syscall, g_syscall),
    {"tzname", NULL, g_posix.zone_names},
    {"timezone", NULL, &g_posix.timezone_},
    {"daylight", NULL, &g_posix.daylight_},
    {"optarg", NULL, &optarg},
    {"optind", NULL, &optind},
    {"opterr", NULL, &opterr},
    {"optopt", NULL, &optopt},
    P(getopt), P(getopt_long),
    {"", NULL, NULL},
};
