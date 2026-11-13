/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */

#ifdef __linux__
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700
/* the names beyond POSIX, which older glibc and musl define only with this */
#define _DEFAULT_SOURCE
#endif
#ifdef __APPLE__
/* the POSIX names above would hide Darwin's own (the sysctl and dyld types) */
#define _DARWIN_C_SOURCE
#endif
#include "common/utilities/common.h"
#include "common/utilities/ascii.h"
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"
#include "common/utilities/ini_lines.h"
#include "common/platform/time.h"
#include "common/platform/vm.h"
#include <libdeflate.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include <direct.h>
#include <io.h>
#include <wchar.h>
#include <psapi.h>
#include <tlhelp32.h> /* the process's threads (snx_threads) */
#include <winioctl.h> /* a link's reparse data (snx_link_target) */
#else
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h> /* flock */
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h> /* a thread's CPU clock (snx_thread_clock) */
#include <unistd.h>
#endif

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/param.h>
#include <sys/mount.h> /* getmntinfo: the mount points a tree removal stops at */
#endif
#ifdef __FreeBSD__
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h> /* struct kinfo_proc */
#include <pthread_np.h> /* pthread_set_name_np */
#include <sys/rtprio.h> /* a thread's idle class (snx_thread_priority_low) */
#endif
#ifdef __linux__
#include <sys/prctl.h>    /* PR_SET_NAME: a thread's name, without _GNU_SOURCE's pthread_setname_np */
#include <sys/resource.h> /* a thread's nice (snx_thread_priority_low) */
#include <sys/syscall.h>  /* SYS_gettid */
#include <sys/vfs.h>      /* statfs and f_type (snx_tmpfs_check) */
#endif
#ifdef __FreeBSD__
#include <sys/mount.h> /* statfs and f_fstypename (snx_tmpfs_check) */
#endif
#ifdef __APPLE__
#include <pthread/qos.h> /* a thread's QoS class (snx_thread_priority_low) */
#include <TargetConditionals.h>
#include <libkern/OSCacheControl.h> /* sys_icache_invalidate */
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <sys/sysctl.h> /* sysctlbyname: the physical cores (snx_physical_cores) */
#include <sys/utsname.h> /* uname: the kernel version, which says whether KosmicKrisp runs */
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif
#endif

void* snx_alloc(size_t size) {
    void* p = malloc(size ? size : 1);
    if (!p) {
        fputs("scarletnx: out of memory\n", stderr);
        exit(1);
    }
    return p;
}
void* snx_realloc(void* ptr, size_t size) {
    void* p = realloc(ptr, size ? size : 1);
    if (!p) {
        fputs("scarletnx: out of memory\n", stderr);
        exit(1);
    }
    return p;
}
char* snx_strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* p = (char*)snx_alloc(n);
    memcpy(p, s, n);
    return p;
}
char* snx_format(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return snx_strdup("");
    char* p = (char*)snx_alloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(p, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return p;
}
void snx_path_native(char* path) {
#ifdef _WIN32
    for (; path && *path; ++path)
        if (*path == '/') *path = '\\';
#else
    (void)path;
#endif
}
int snx_path_format(char* out, size_t size, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, size, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= size) {
        if (n < 0)
            snx_error("cannot make a path of %s (%s)", fmt, strerror(errno));
        else
            snx_error("the path %.80s... is %d bytes, over the %zu it has room for", size ? out : fmt, n,
                      size ? size - 1 : 0);
        if (size) *out = 0;
        return -1;
    }
    snx_path_native(out);
    return 0;
}
/* snx_error's sink, snx_error_sink's: NULL for stderr; the kinds whose folder app.ini names wrongly, and the options
   given by an older name, said once */
static struct {
    void (*error_sink)(const char* line);
    unsigned said_kinds;
    const char* renamed[16];
    size_t renamed_count;
    volatile int abort_requested; /* snx_abort_request's, from another thread than the one that asks */
} g_common;
void snx_option_renamed(const char* old, const char* now) {
    for (size_t i = 0; i < g_common.renamed_count; ++i)
        if (!strcmp(g_common.renamed[i], old)) return;
    if (g_common.renamed_count < sizeof g_common.renamed / sizeof *g_common.renamed)
        g_common.renamed[g_common.renamed_count++] = old;
    snx_error("%s is %s now: this run takes it as %s (docs/user/deprecations.md)", old, now, now);
}

void snx_error_sink(void (*sink)(const char* line)) {
    g_common.error_sink = sink;
}
int snx_error(const char* fmt, ...) {
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    if (g_common.error_sink) {
        vsnprintf(line, sizeof line, fmt, ap);
        g_common.error_sink(line);
    } else {
        fputs("scarletnx: ", stderr);
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
    }
    va_end(ap);
    return 1;
}
char* snx_os_error(char* out, size_t size) {
#ifdef _WIN32
    /* the wide text: FormatMessageA's is in the system's code page, which every reader of out takes for UTF-8 */
    DWORD code = GetLastError();
    WCHAR text[512];
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, text,
                             (DWORD)(sizeof text / sizeof *text), NULL);
    snx_windows_reason((const uint16_t*)text, n, (unsigned long)code, out, size);
#else
    if (size) snprintf(out, size, "%s", strerror(errno));
#endif
    return out;
}
char* snx_windows_reason(const uint16_t* text, size_t n, unsigned long code, char* out, size_t size) {
    char tail[40];
    size_t at = 0, room, i;
    if (!size) return out;
    for (i = 0; i < n && text[i]; ++i) {}
    n = i;
    while (n && (text[n - 1] == '\n' || text[n - 1] == '\r' || text[n - 1] == ' ' || text[n - 1] == '.' ||
                 text[n - 1] == 0x3002)) /* the ideographic full stop too */
        --n;
    if (!n) {
        snprintf(out, size, "Windows error %lu", code);
        return out;
    }
    snprintf(tail, sizeof tail, " (Windows error %lu)", code);
    room = size - 1 > strlen(tail) ? size - 1 - strlen(tail) : size - 1; /* the code after it where both fit */
    for (i = 0; i < n; ++i) {
        uint32_t c = text[i];
        unsigned char utf8[4];
        size_t k;
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && text[i + 1] >= 0xDC00 && text[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (uint32_t)(text[++i] - 0xDC00);
        else if (c >= 0xD800 && c < 0xE000)
            c = 0xFFFD;
        if (c < 0x80) {
            utf8[0] = (unsigned char)c;
            k = 1;
        } else if (c < 0x800) {
            utf8[0] = (unsigned char)(0xC0 | c >> 6);
            utf8[1] = (unsigned char)(0x80 | (c & 0x3F));
            k = 2;
        } else if (c < 0x10000) {
            utf8[0] = (unsigned char)(0xE0 | c >> 12);
            utf8[1] = (unsigned char)(0x80 | (c >> 6 & 0x3F));
            utf8[2] = (unsigned char)(0x80 | (c & 0x3F));
            k = 3;
        } else {
            utf8[0] = (unsigned char)(0xF0 | c >> 18);
            utf8[1] = (unsigned char)(0x80 | (c >> 12 & 0x3F));
            utf8[2] = (unsigned char)(0x80 | (c >> 6 & 0x3F));
            utf8[3] = (unsigned char)(0x80 | (c & 0x3F));
            k = 4;
        }
        if (at + k > room) break;
        memcpy(out + at, utf8, k);
        at += k;
    }
    snprintf(out + at, size - at, "%s", strlen(tail) < size - at ? tail : "");
    return out;
}
char* snx_path_join(const char* a, const char* b) {
    size_t n = strlen(a), m = strlen(b);
    char* p = (char*)snx_alloc(n + m + 2);
    memcpy(p, a, n);
    if (n && !snx_is_separator(a[n - 1])) p[n++] = '/';
    memcpy(p + n, b, m + 1);
    snx_path_native(p);
    return p;
}
char* snx_dirname(const char* path) {
    char* p = snx_strdup(path);
    size_t n = strlen(p);
    while (n > 1 && snx_is_separator(p[n - 1])) p[--n] = 0;
    while (n && !snx_is_separator(p[n - 1])) --n;
    if (!n) {
        free(p);
        return snx_strdup(".");
    }
    while (n > 1 && snx_is_separator(p[n - 1])) --n;
#ifdef _WIN32
    if (n == 2 && p[1] == ':') ++n;
#endif
    p[n] = 0;
    return p;
}
#ifdef _WIN32
static wchar_t* wide(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    wchar_t* p;
    if (!n) return NULL;
    p = (wchar_t*)snx_alloc((size_t)n * sizeof(*p));
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, p, n);
    return p;
}
static char* utf8(const wchar_t* s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    if (!n) return NULL;
    char* p = (char*)snx_alloc((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, s, -1, p, n, NULL, NULL);
    return p;
}
static DWORD attributes(const char* path) {
    wchar_t* p = wide(path);
    DWORD a = p ? GetFileAttributesW(p) : INVALID_FILE_ATTRIBUTES;
    free(p);
    return a;
}
#endif
FILE* snx_fopen(const char* path, const char* mode) {
#ifdef _WIN32
    wchar_t *p = wide(path), *m = wide(mode);
    FILE* f = p && m ? _wfopen(p, m) : NULL;
    free(p);
    free(m);
    return f;
#else
    return fopen(path, mode);
#endif
}
int snx_log_lines(const char* path, const char* text) {
    char stamp[64];
    time_t now = time(NULL);
    struct tm local = {0};
    size_t lines = 1, n = 0, w;
    const char *c, *line;
    char* out;
    int failed;
#ifdef _WIN32
    unsigned long pid = (unsigned long)GetCurrentProcessId();
    FILE* f;
    localtime_s(&local, &now);
#else
    unsigned long pid = (unsigned long)getpid();
    int fd;
    localtime_r(&now, &local);
#endif
    w = strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &local);
    snprintf(stamp + w, sizeof stamp - w, " [%lu] ", pid);
    w = strlen(stamp);
    for (c = text; *c; ++c) lines += *c == '\n';
    out = (char*)snx_alloc(strlen(text) + lines * w + 2);
    for (line = text; *line;) { /* each line its stamp, its newline */
        const char* end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line), kept = len;
        if (kept && line[kept - 1] == '\r') --kept; /* a Windows program's \r\n: the line ends as the others do */
        memcpy(out + n, stamp, w);
        memcpy(out + n + w, line, kept);
        n += w + kept;
        out[n++] = '\n';
        line += len + (end != NULL);
    }
#ifdef _WIN32
    f = snx_fopen(path, "ab");
    failed = !f || fwrite(out, 1, n, f) != n;
    if (f && fclose(f)) failed = 1;
#else
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    failed = fd < 0 || write(fd, out, n) != (ssize_t)n;
    if (fd >= 0) {
        int saved = errno;
        close(fd);
        errno = saved;
    }
#endif
    free(out);
    return failed ? -1 : 0;
}
/* snx_lock_file's lock, on a file made when it is not there with make, else only on one that is */
static int lock_file(const char* path, int make, intptr_t* lock) {
#ifdef _WIN32
    wchar_t* p = wide(path);
    OVERLAPPED past = {0}; /* not far: windef.h defines it */
    HANDLE h = p ? CreateFileW(p, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, make ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
                 : INVALID_HANDLE_VALUE;
    DWORD failed = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    free(p);
    if (h == INVALID_HANDLE_VALUE) {
        errno = failed == ERROR_FILE_NOT_FOUND || failed == ERROR_PATH_NOT_FOUND ? ENOENT : EACCES;
        SetLastError(failed); /* snx_os_error's reason */
        return -1;
    }
    past.OffsetHigh = 0x7fffffffu; /* past any end, so the bytes are never what is locked */
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &past)) {
        DWORD error = GetLastError();
        CloseHandle(h);
        if (error == ERROR_LOCK_VIOLATION || error == ERROR_IO_PENDING) return 1;
        errno = EACCES;
        return -1;
    }
    *lock = (intptr_t)h;
    return 0;
#else
    int fd = open(path, O_RDWR | O_CLOEXEC | (make ? O_CREAT : 0), 0644);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB)) {
        int error = errno;
        close(fd);
        errno = error;
        return error == EWOULDBLOCK ? 1 : -1;
    }
    *lock = fd;
    return 0;
#endif
}
int snx_lock_file(const char* path, intptr_t* lock) { return lock_file(path, 1, lock); }
int snx_lock_existing(const char* path, intptr_t* lock) { return lock_file(path, 0, lock); }
void snx_unlock_file(intptr_t lock) {
#ifdef _WIN32
    CloseHandle((HANDLE)lock); /* its lock with it */
#else
    close((int)lock);
#endif
}
int snx_path_is_full(const char* path) {
#ifdef _WIN32
    if (snx_is_alpha(path[0]) && path[1] == ':' && (path[2] == '\\' || path[2] == '/')) return 1;
    return path[0] == '\\' && path[1] == '\\';
#else
    return path[0] == '/';
#endif
}
const char* snx_path_leaf(const char* path) {
    const char* leaf = path;
    for (const char* p = path; *p; ++p)
        if (snx_is_any_separator(*p)) leaf = p + 1;
    return leaf;
}
char* snx_game_folder(const char* repo, const char* outdir, const char* argv0, const char* title, int make) {
    char *exe = repo || outdir ? NULL : snx_executable(argv0), *dir = exe ? snx_dirname(exe) : NULL;
    char* data = outdir ? NULL : repo ? snx_strdup(repo) : snx_data_dir(dir);
    char* folder = outdir ? snx_path_join(outdir, title) : data ? snx_recompiled_game(data, title) : NULL;
    char* resolved = folder && !(make && snx_mkdirs(folder)) ? snx_absolute(folder) : NULL;
    free(folder);
    free(data);
    free(dir);
    free(exe);
    return resolved;
}
int snx_parse_seconds(const char* text, double* seconds) {
    char* end = NULL;
    return text && (*seconds = strtod(text, &end)) > 0 && !*end && *seconds <= 1e9;
}
char* snx_absolute(const char* path) {
#ifdef _WIN32
    wchar_t *p = wide(path), *result, *final_path;
    char *out, *joined;
    DWORD n, error;
    HANDLE handle;
    if (!p) return NULL;
    n = GetFullPathNameW(p, 0, NULL, NULL);
    if (!n) {
        free(p);
        return NULL;
    }
    result = (wchar_t*)snx_alloc((size_t)n * sizeof(*result));
    if (!GetFullPathNameW(p, n, result, NULL)) {
        free(p);
        free(result);
        return NULL;
    }
    free(p);
    handle = CreateFileW(result, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS, NULL);
    error = GetLastError();
    if (handle != INVALID_HANDLE_VALUE) {
        n = GetFinalPathNameByHandleW(handle, NULL, 0, FILE_NAME_NORMALIZED);
        final_path = (wchar_t*)snx_alloc(((size_t)n + 1) * sizeof(*final_path));
        if (!n || !GetFinalPathNameByHandleW(handle, final_path, n + 1, FILE_NAME_NORMALIZED)) {
            free(final_path);
            free(result);
            CloseHandle(handle);
            return NULL;
        }
        CloseHandle(handle);
        free(result);
        out = utf8(final_path);
        free(final_path);
        if (!out) return NULL;
        if (!strncmp(out, "\\\\?\\UNC\\", 8)) {
            joined = snx_format("\\\\%s", out + 8);
            free(out);
            return joined;
        }
        if (!strncmp(out, "\\\\?\\", 4)) memmove(out, out + 4, strlen(out + 4) + 1);
        return out;
    }
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
        free(result);
        return NULL;
    }
    out = utf8(result);
    free(result);
    if (!out) return NULL;
    n = (DWORD)strlen(out);
    while (n > 3 && snx_is_separator(out[n - 1])) out[--n] = 0;
    char* parent = snx_dirname(out);
    if (!strcmp(parent, out)) {
        free(out);
        free(parent);
        return NULL;
    }
    char* resolved = snx_absolute(parent);
    free(parent);
    if (!resolved) {
        free(out);
        return NULL;
    }
    char* leaf = out;
    for (joined = out; *joined; ++joined)
        if (snx_is_separator(*joined)) leaf = joined + 1;
    joined = snx_path_join(resolved, leaf);
    free(resolved);
    free(out);
    return joined;
#else
    char *cwd, *out, *parent, *result;
    size_t n = 256;
    if (path[0] == '/')
        out = snx_strdup(path);
    else {
        for (;;) {
            cwd = (char*)snx_alloc(n);
            if (getcwd(cwd, n)) break;
            free(cwd);
            if (errno != ERANGE || n > SIZE_MAX / 2) return NULL;
            n *= 2;
        }
        out = snx_path_join(cwd, path);
        free(cwd);
    }
    size_t len = strlen(out);
    while (len > 1 && out[len - 1] == '/') out[--len] = 0;
    char* resolved = realpath(out, NULL);
    if (resolved) {
        free(out);
        return resolved;
    }
    if (errno != ENOENT && errno != ENOTDIR) {
        free(out);
        return NULL;
    }
    /* Resolve existing ancestors too, then normalize a not-yet-created suffix.
       In particular, missing/.. must never be mistaken for a newly owned output. */
    parent = snx_dirname(out);
    if (!strcmp(parent, out)) {
        free(parent);
        free(out);
        return NULL;
    }
    resolved = snx_absolute(parent);
    free(parent);
    if (!resolved) {
        free(out);
        return NULL;
    }
    char* base = strrchr(out, '/');
    base = base ? base + 1 : out;
    if (!strcmp(base, ".") || !*base)
        result = snx_strdup(resolved);
    else if (!strcmp(base, ".."))
        result = snx_dirname(resolved);
    else
        result = snx_path_join(resolved, base);
    free(out);
    free(resolved);
    return result;
#endif
}
char* snx_executable(const char* argv0) {
#ifdef _WIN32
    wchar_t buf[32768];
    DWORD n = GetModuleFileNameW(NULL, buf, 32768);
    if (n && n < 32768) return utf8(buf);
#elif defined(__linux__)
    char buf[8192];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0 && (size_t)n < sizeof(buf) - 1) {
        buf[n] = 0;
        return snx_strdup(buf);
    }
#elif defined(__APPLE__)
    char buf[8192];
    uint32_t size = sizeof(buf);
    char* resolved;
    if (!_NSGetExecutablePath(buf, &size) && (resolved = realpath(buf, NULL)) != NULL) {
        char* path = snx_strdup(resolved);
        free(resolved);
        return path;
    }
#elif defined(__FreeBSD__)
    /* FreeBSD mounts no /proc by default: the kernel's own record of the process's file */
    char buf[8192];
    size_t size = sizeof buf;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
    if (!sysctl(mib, 4, buf, &size, NULL, 0) && size > 1 && buf[0] == '/') return snx_strdup(buf);
#endif
    if (!argv0) return NULL; /* a caller that has no argv[0] (NULL) takes the system's answer alone */
    {
        /* the program's own file, where snx_find_tool keeps the name of a link to it */
        char *found = snx_find_tool(argv0), *file = found ? snx_absolute(found) : NULL;
        free(found);
        return file;
    }
}
int snx_is_file(const char* path) {
#ifdef _WIN32
    DWORD a = attributes(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return !stat(path, &st) && S_ISREG(st.st_mode);
#endif
}
int snx_is_dir(const char* path) {
#ifdef _WIN32
    DWORD a = attributes(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
#endif
}
int snx_is_link(const char* path) {
#ifdef _WIN32
    DWORD a = attributes(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    struct stat st;
    return !lstat(path, &st) && S_ISLNK(st.st_mode);
#endif
}
int snx_path_exists(const char* path) {
#ifdef _WIN32
    return attributes(path) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return !lstat(path, &st);
#endif
}
static int mkdir_one(const char* path) {
#ifdef _WIN32
    wchar_t* p = wide(path);
    int ok = p && CreateDirectoryW(p, NULL);
    free(p);
    return !ok && !snx_is_dir(path);
#else
    return mkdir(path, 0700) && !snx_is_dir(path);
#endif
}
int snx_mkdirs(const char* path) {
    if (snx_is_dir(path)) return 0;
    char* parent = snx_dirname(path);
    if (!strcmp(parent, path)) {
        free(parent);
        return 1;
    }
    int result = snx_mkdirs(parent);
    free(parent);
    return result || mkdir_one(path);
}
void snx_free_strings(char** strings, size_t count) {
    for (size_t i = 0; i < count; ++i) free(strings[i]);
    free(strings);
}
int snx_list_dir(const char* path, char*** names, size_t* count) {
    char** list = NULL;
    size_t n = 0, cap = 0;
    int error = 0;
#ifdef _WIN32
    WIN32_FIND_DATAW data;
    char* pattern = snx_path_join(path, "*");
    wchar_t* wp = wide(pattern);
    HANDLE h = wp ? FindFirstFileW(wp, &data) : INVALID_HANDLE_VALUE;
    free(pattern);
    free(wp);
    if (h == INVALID_HANDLE_VALUE) return 1;
    do {
        if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
        char* name = utf8(data.cFileName);
        if (!name) {
            error = 1;
            break;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            list = (char**)snx_realloc(list, cap * sizeof(*list));
        }
        list[n++] = name;
    } while (FindNextFileW(h, &data));
    if (!error && GetLastError() != ERROR_NO_MORE_FILES) error = 1;
    FindClose(h);
#else
    DIR* dir = opendir(path);
    struct dirent* ent;
    if (!dir) return 1;
    for (;;) {
        errno = 0;
        ent = readdir(dir);
        if (!ent) {
            error = errno != 0;
            break;
        }
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            list = (char**)snx_realloc(list, cap * sizeof(*list));
        }
        list[n++] = snx_strdup(ent->d_name);
    }
    closedir(dir);
#endif
    if (error) {
        snx_free_strings(list, n);
        return 1;
    }
    if (n > 1) qsort(list, n, sizeof(*list), snx_compare_strings);
    *names = list;
    *count = n;
    return 0;
}
static int remove_one(const char* path, int dir) {
#ifdef _WIN32
    wchar_t* p = wide(path);
    int ok = p && (dir ? RemoveDirectoryW(p) : DeleteFileW(p));
    free(p);
    return !ok;
#else
    return (dir ? rmdir(path) : unlink(path)) != 0;
#endif
}
int snx_remove_empty_dir(const char* path) { return !snx_is_dir(path) || snx_is_link(path) || remove_one(path, 1); }
static int remove_tree(const char* path) {
    char **names, *p;
    size_t n;
    int rc = 0;
    int dir = snx_is_dir(path);
    if (dir && !snx_is_link(path)) {
        if (snx_list_dir(path, &names, &n)) return 1;
        for (size_t i = 0; i < n; ++i) {
            p = snx_path_join(path, names[i]);
            if (remove_tree(p)) rc = 1;
            free(p);
        }
        snx_free_strings(names, n);
    }
#ifndef _WIN32
    if (snx_is_link(path)) dir = 0;
#endif
    return remove_one(path, dir) || rc;
}
char* snx_mount_in(const char* mountinfo, const char* root) {
    size_t length = strlen(root);
    const char* line = mountinfo;
    while (line && *line) {
        const char *stop = strchr(line, '\n'), *field = line, *last;
        int spaces = 0;
        if (!stop) stop = line + strlen(line);
        while (field < stop && spaces < 4) /* past the first four fields */
            if (*field++ == ' ') spaces++;
        for (last = field; last < stop && *last != ' '; last++) continue;
        if (spaces == 4 && last > field) { /* the fifth, its spaces and backslashes written \040 and \134 */
            size_t n = (size_t)(last - field), j = 0, k;
            char* point = (char*)snx_alloc(n + 1);
            for (k = 0; k < n; k++) {
                if (field[k] == '\\' && k + 3 < n && field[k + 1] >= '0' && field[k + 1] <= '3' &&
                    field[k + 2] >= '0' && field[k + 2] <= '7' && field[k + 3] >= '0' && field[k + 3] <= '7') {
                    point[j++] = (char)((field[k + 1] - '0') << 6 | (field[k + 2] - '0') << 3 | (field[k + 3] - '0'));
                    k += 3;
                } else
                    point[j++] = field[k];
            }
            point[j] = 0;
            if (!strncmp(point, root, length) && (!point[length] || point[length] == '/')) return point;
            free(point);
        }
        line = *stop ? stop + 1 : NULL;
    }
    return NULL;
}
char* snx_mount_within(const char* path) {
    char *root = snx_absolute(path), *found = NULL;
    if (!root) return NULL;
#if defined(__linux__) || defined(__ANDROID__)
    {
        size_t size;
        char* text = (char*)snx_read_file("/proc/self/mountinfo", &size);
        found = text ? snx_mount_in(text, root) : NULL;
        free(text);
    }
#elif defined(__APPLE__) || defined(__FreeBSD__)
    {
        struct statfs* mounts;
        size_t length = strlen(root);
        int n = getmntinfo(&mounts, MNT_NOWAIT), i;
        for (i = 0; i < n && !found; i++)
            if (!strncmp(mounts[i].f_mntonname, root, length) &&
                (!mounts[i].f_mntonname[length] || mounts[i].f_mntonname[length] == '/'))
                found = snx_strdup(mounts[i].f_mntonname);
    }
#endif
    free(root);
    return found;
}
int snx_remove_tree(const char* path) {
    char* mount = snx_is_dir(path) && !snx_is_link(path) ? snx_mount_within(path) : NULL;
    if (mount) { /* nothing removed: what is mounted there is another disk's, or another folder's */
        snx_error("cannot remove %s: %s is a mount point, another disk or folder mounted there, whose files would go "
                  "with it. Unmount it first",
                  path, mount);
        free(mount);
        return 1;
    }
    return remove_tree(path);
}
unsigned char* snx_read_file(const char* path, size_t* size) {
    FILE* f = snx_fopen(path, "rb");
    size_t cap = 8192, n = 0, got;
    if (!f) return NULL;
    unsigned char* p = (unsigned char*)snx_alloc(cap + 1);
    for (;;) {
        got = fread(p + n, 1, cap - n, f);
        n += got;
        if (n < cap) break;
        if (cap > (SIZE_MAX - 1) / 2) {
            free(p);
            fclose(f);
            return NULL;
        }
        cap *= 2;
        p = (unsigned char*)snx_realloc(p, cap + 1);
    }
    if (ferror(f)) {
        free(p);
        fclose(f);
        return NULL;
    }
    fclose(f);
    p[n] = 0;
    *size = n;
    return p;
}
int snx_file_has_record(const char* path, const void* magic, size_t magic_size, size_t size,
                        int (*accept)(const unsigned char* record, const void* context), const void* context) {
    /* read a MiB at a time, the last size - 1 bytes kept for a record that runs into the next */
    size_t cap = ((size_t)1 << 20) + size, have = 0, got;
    FILE* f = magic_size && magic_size <= size ? snx_fopen(path, "rb") : NULL;
    int found = 0;
    if (!f) return 0;
    unsigned char* p = (unsigned char*)snx_alloc(cap);
    do {
        got = fread(p + have, 1, cap - have, f);
        have += got;
        if (have >= size) {
            unsigned char *at = p, *end = p + have - size + 1;
            while (!found && (at = (unsigned char*)memchr(at, *(const unsigned char*)magic, (size_t)(end - at)))) {
                found = !memcmp(at, magic, magic_size) && accept(at, context);
                ++at;
            }
            memmove(p, p + have - (size - 1), size - 1);
            have = size - 1;
        }
    } while (!found && got);
    fclose(f);
    free(p);
    return found;
}
int snx_write_file(const char* path, const void* data, size_t size) {
    FILE* f = snx_fopen(path, "wb");
    if (!f) return 1;
    int result = fwrite(data, 1, size, f) != size;
    return fclose(f) != 0 || result;
}
int snx_write_changed(const char* path, const void* data, size_t size) {
    size_t n;
    unsigned char* old = snx_read_file(path, &n);
    int same = old && n == size && !memcmp(old, data, size);
    free(old);
    return same ? 0 : snx_write_file(path, data, size);
}
int snx_ppm_write(FILE* f, uint32_t width, uint32_t height, const unsigned char* pixels, ptrdiff_t pixel_step,
                  ptrdiff_t row_step, int order) {
    if (fprintf(f, "P6\n%u %u\n255\n", (unsigned)width, (unsigned)height) < 0) return -1;
    unsigned char* line = width ? (unsigned char*)malloc((size_t)width * 3) : NULL;
    if (width && !line) return -1;
    int rc = 0;
    for (uint32_t y = 0; y < height && !rc; ++y) {
        const unsigned char* row = pixels + (ptrdiff_t)y * row_step;
        for (uint32_t x = 0; x < width; ++x) {
            const unsigned char* p = row + (ptrdiff_t)x * pixel_step;
            unsigned char* out = line + (size_t)x * 3;
            if (order == SNX_PPM_WORD) {
                uint32_t word;
                memcpy(&word, p, sizeof word);
                out[0] = (unsigned char)word;
                out[1] = (unsigned char)(word >> 8);
                out[2] = (unsigned char)(word >> 16);
            } else {
                out[0] = p[order == SNX_PPM_BGRA ? 2 : 0];
                out[1] = p[1];
                out[2] = p[order == SNX_PPM_BGRA ? 0 : 2];
            }
        }
        rc = fwrite(line, 3, width, f) != width ? -1 : 0;
    }
    free(line);
    return rc;
}
int snx_copy_file(const char* src, const char* dst) {
    unsigned char buf[65536];
    FILE* in = snx_fopen(src, "rb");
    size_t n;
    int rc = 0;
    if (!in) return 1;
    /* Avoid truncating the input when recompiling an existing *_data directory. */
    FILE* out = snx_fopen(dst, "rb");
    if (out) {
        unsigned char other[65536];
        int same = 1;
        do {
            n = fread(buf, 1, sizeof(buf), in);
            size_t m = fread(other, 1, sizeof(other), out);
            if (n != m || memcmp(buf, other, n)) {
                same = 0;
                break;
            }
        } while (n);
        if (ferror(in) || ferror(out)) same = 0;
        fclose(out);
        if (same) {
            fclose(in);
            return 0;
        }
        rewind(in);
    }
    out = snx_fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return 1;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) != 0) {
        if (fwrite(buf, 1, n, out) != n) {
            rc = 1;
            break;
        }
    }
    if (ferror(in)) rc = 1;
    fclose(in);
    if (fclose(out)) rc = 1;
    return rc;
}
int snx_seek(FILE* f, SnxU64 offset) {
#ifdef _WIN32
    return (offset >> 63) != 0 || _fseeki64(f, offset, SEEK_SET) != 0;
#else
    if ((SnxU64)(off_t)offset != offset || (off_t)offset < 0) return 1;
    return fseeko(f, (off_t)offset, SEEK_SET) != 0;
#endif
}
int64_t snx_tell(FILE* f) {
#ifdef _WIN32
    return _ftelli64(f);
#else
    return (int64_t)ftello(f);
#endif
}
int snx_cut(FILE* f, SnxU64 size) {
#ifdef _WIN32
    return (size >> 63) != 0 || _chsize_s(_fileno(f), (long long)size) ? -1 : 0;
#else
    if ((SnxU64)(off_t)size != size || (off_t)size < 0) return -1;
    return ftruncate(fileno(f), (off_t)size) ? -1 : 0;
#endif
}
int snx_copy_file_range(const char* src, SnxU64 offset, SnxU64 size, const char* dst) {
    unsigned char buf[65536];
    FILE *in = snx_fopen(src, "rb"), *out;
    size_t n;
    int rc = 0;
    if (!in) return 1;
    if (snx_seek(in, offset) || !(out = snx_fopen(dst, "wb"))) {
        fclose(in);
        return 1;
    }
    while (size) {
        n = size < sizeof(buf) ? (size_t)size : sizeof(buf);
        if (fread(buf, 1, n, in) != n || fwrite(buf, 1, n, out) != n) {
            rc = 1;
            break;
        }
        size -= n;
    }
    fclose(in);
    if (fclose(out)) rc = 1;
    return rc;
}
int snx_link_file(const char* src, const char* dst) {
#ifdef _WIN32
    wchar_t *a = wide(src), *b = wide(dst);
    int ok = a && b && CreateHardLinkW(b, a, NULL);
    free(a);
    free(b);
    return !ok;
#else
    return link(src, dst) != 0;
#endif
}
char* snx_link_target(const char* path) {
#ifdef _WIN32
    /* the reparse data of a symbolic link or a junction: its print name, else its substitute name without \??\ */
    wchar_t *p = path ? wide(path) : NULL, *name = NULL;
    HANDLE h = p ? CreateFileW(p, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                               FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL)
                 : INVALID_HANDLE_VALUE;
    unsigned char* data =
        h != INVALID_HANDLE_VALUE ? (unsigned char*)snx_alloc(MAXIMUM_REPARSE_DATA_BUFFER_SIZE) : NULL;
    DWORD got = 0;
    char* target = NULL;
    free(p);
    if (!data) return NULL;
    if (DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, NULL, 0, data, MAXIMUM_REPARSE_DATA_BUFFER_SIZE, &got, NULL) &&
        got >= 16) {
        uint32_t tag = snx_ld_le32(data);
        /* after the tag, the data's size and a reserved half: the names' offsets and sizes, a symbolic link's flags,
           then the names, offsets from there */
        size_t names = tag == IO_REPARSE_TAG_SYMLINK ? 20 : tag == IO_REPARSE_TAG_MOUNT_POINT ? 16 : 0;
        size_t substitute = snx_ld_le16(data + 8), substitute_size = snx_ld_le16(data + 10);
        size_t print = snx_ld_le16(data + 12), print_size = snx_ld_le16(data + 14);
        size_t at = print_size ? print : substitute, size = print_size ? print_size : substitute_size;
        if (names && size && size % 2 == 0 && names + at + size <= got) {
            name = (wchar_t*)snx_alloc(size + sizeof(wchar_t));
            memcpy(name, data + names + at, size);
            name[size / 2] = 0;
            target = utf8(!print_size && !wcsncmp(name, L"\\??\\", 4) ? name + 4 : name);
        }
    }
    CloseHandle(h);
    free(data);
    free(name);
    return target;
#else
    struct stat st;
    size_t room = path && !lstat(path, &st) && S_ISLNK(st.st_mode) && st.st_size > 0 ? (size_t)st.st_size + 1 : 0;
    char* target = room ? (char*)snx_alloc(room) : NULL;
    ssize_t n = target ? readlink(path, target, room) : -1;
    if (n < 0 || (size_t)n >= room) { /* gone, or longer than when it was looked at */
        free(target);
        return NULL;
    }
    target[n] = 0;
    return target;
#endif
}
int snx_symlink(const char* target, const char* link) {
#ifdef _WIN32
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 2
#endif
    wchar_t *a = wide(target), *b = wide(link);
    int ok = a && b && CreateSymbolicLinkW(b, a, SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
    free(a);
    free(b);
    return !ok;
#else
    return symlink(target, link) != 0;
#endif
}
/* the first of leaves that exists in program's folders: beside it, then ../Frameworks of an app's Contents/MacOS */
static char* program_file(const char* program, const char* const* leaves, size_t count) {
    char* dir = program ? snx_dirname(program) : NULL;
    char* path = NULL;
    for (size_t i = 0; !path && dir && i < count; ++i) {
        char* found = snx_path_join(dir, leaves[i]);
        if (found && snx_is_file(found)) path = found;
        else free(found);
    }
    free(dir);
    return path;
}
/* the first file of folder whose name holds word and ends with ending: an ICD JSON's name differs between Mesa's
   Homebrew package and the Vulkan SDK */
static char* folder_file(const char* folder, const char* word, const char* ending) {
    char** names = NULL;
    size_t count = 0, i;
    char* path = NULL;
    if (!folder || snx_list_dir(folder, &names, &count)) return NULL;
    for (i = 0; !path && i < count; ++i) {
        char* found;
        if (!strstr(names[i], word) || !snx_ends_with_nocase(names[i], ending)) continue;
        found = snx_path_join(folder, names[i]);
        if (found && snx_is_file(found)) path = found;
        else free(found);
    }
    snx_free_strings(names, count);
    return path;
}
/* rel's file under prefix, NULL when the prefix has none */
static char* prefix_file(const char* prefix, const char* rel) {
    char* path = snx_path_join(prefix, rel);
    if (path && !snx_is_file(path)) {
        free(path);
        path = NULL;
    }
    return path;
}
/* the newer version name first: each number of the name compared (1.4.9.0 before 1.4.10.0), then the text */
static int newer_first(const void* a, const void* b) {
    const char* x = *(const char* const*)a;
    const char* y = *(const char* const*)b;
    while (*x || *y) {
        unsigned long nx = 0, ny = 0;
        int digits = 0;
        while (*x >= '0' && *x <= '9') {
            nx = nx * 10 + (unsigned long)(*x++ - '0');
            digits = 1;
        }
        while (*y >= '0' && *y <= '9') {
            ny = ny * 10 + (unsigned long)(*y++ - '0');
            digits = 1;
        }
        if (nx != ny) return nx > ny ? -1 : 1;
        if (*x == '.') ++x;
        if (*y == '.') ++y;
        if (!*x && !*y) break;
        if (!digits) break;
    }
    return strcmp(x, y) < 0 ? 1 : strcmp(x, y) > 0 ? -1 : 0; /* the newer text first */
}
/* the folders a Vulkan SDK is installed in under sdks, the newest version first: <version>/macOS, then <version> for
   an older layout */
static char** sdk_roots(const char* sdks, size_t* count) {
    char** versions = NULL;
    char** roots = NULL;
    size_t n = 0, cap = 0, i;
    *count = 0;
    if (!sdks || !sdks[0] || snx_list_dir(sdks, &versions, &n)) return NULL;
    if (n > 1) qsort(versions, n, sizeof *versions, newer_first);
    for (i = 0; i < n; ++i) {
        char* version = snx_path_join(sdks, versions[i]);
        char* macos = version ? snx_path_join(version, "macOS") : NULL;
        const char* two[2] = {macos, version};
        for (int k = 0; k < 2; ++k) {
            if (!two[k] || !snx_is_dir(two[k])) continue;
            if (*count == cap) {
                cap = cap ? cap * 2 : 4;
                roots = (char**)snx_realloc(roots, cap * sizeof *roots);
            }
            roots[(*count)++] = snx_strdup(two[k]);
        }
        free(macos);
        free(version);
    }
    snx_free_strings(versions, n);
    return roots;
}
/* the ICD JSONs under prefix's share/vulkan/icd.d (Homebrew's Mesa, an SDK installed for the whole Mac) */
static char* prefix_icd(const char* prefix) {
    char* dir = snx_path_join(prefix, "share/vulkan/icd.d");
    char* icd = folder_file(dir, "kosmickrisp", ".json");
    free(dir);
    return icd;
}
static char* sdk_icd(const char* sdks) {
    size_t count = 0, i;
    char** roots = sdk_roots(sdks, &count);
    char* path = NULL;
    for (i = 0; !path && i < count; ++i) {
        char* dir = snx_path_join(roots[i], "share/vulkan/icd.d");
        path = folder_file(dir, "kosmickrisp", ".json");
        free(dir);
    }
    snx_free_strings(roots, count);
    return path;
}
/* the library leaf under a Vulkan SDK's lib (its loader, or MoltenVK's) */
static char* sdk_library(const char* sdks, const char* leaf) {
    size_t count = 0, i;
    char** roots = sdk_roots(sdks, &count);
    char* path = NULL;
    for (i = 0; !path && i < count; ++i) {
        char* rel = snx_path_join("lib", leaf);
        path = rel ? prefix_file(roots[i], rel) : NULL;
        free(rel);
    }
    snx_free_strings(roots, count);
    return path;
}
/* lib/leaf installed on the Mac: under the first prefix that has it, else in the newest Vulkan SDK */
static char* installed_library(const SnxVulkanPlaces* at, const char* leaf) {
    char* rel = snx_path_join("lib", leaf);
    char* path = NULL;
    for (size_t i = 0; rel && !path && i < sizeof at->prefixes / sizeof *at->prefixes; ++i)
        if (at->prefixes[i]) path = prefix_file(at->prefixes[i], rel);
    free(rel);
    return path ? path : sdk_library(at->sdks, leaf);
}
/* KosmicKrisp's ICD JSON installed on the Mac: under the first prefix that has one, else in the newest Vulkan SDK */
static char* installed_icd(const SnxVulkanPlaces* at) {
    char* path = NULL;
    for (size_t i = 0; !path && i < sizeof at->prefixes / sizeof *at->prefixes; ++i)
        if (at->prefixes[i]) path = prefix_icd(at->prefixes[i]);
    return path ? path : sdk_icd(at->sdks);
}
/* out, with text past what it holds, cut to size */
static void append(char* out, size_t size, const char* format, ...) {
    size_t n = strlen(out);
    va_list ap;
    if (n + 1 >= size) return;
    va_start(ap, format);
    vsnprintf(out + n, size - n, format, ap);
    va_end(ap);
}
/* the folders a search of sub (lib, share/vulkan/icd.d) went through, for its reason */
static void looked_in(const SnxVulkanPlaces* at, const char* sub, char* out, size_t size) {
    int prefixes = 0;
    snprintf(out, size, "beside the program or in its app's Frameworks");
    for (size_t i = 0; i < sizeof at->prefixes / sizeof *at->prefixes; ++i)
        if (at->prefixes[i]) append(out, size, "%s %s/%s", prefixes++ ? " or" : ", in", at->prefixes[i], sub);
    if (at->sdks && at->sdks[0]) append(out, size, ", or in a Vulkan SDK under %s", at->sdks);
}
/* the drivers an app carries, found beside the program (a game's runtime copy, snx_deploy_vulkan_driver) or in its
   bundle's Frameworks (../Frameworks from Contents/MacOS); MoltenVK also as the framework an iOS app holds */
static const char* const moltenvk_own[] = {"libMoltenVK.dylib", "../Frameworks/libMoltenVK.dylib",
                                           "Frameworks/MoltenVK.framework/MoltenVK"};
static const char* const kosmickrisp_own[] = {"libkosmickrisp_icd.json", "../Frameworks/libkosmickrisp_icd.json"};
static const char* const loader_own[] = {"libvulkan.1.dylib", "../Frameworks/libvulkan.1.dylib"};
char* snx_vulkan_driver(const char* program) {
#ifdef __APPLE__
    return program_file(program, moltenvk_own, sizeof moltenvk_own / sizeof *moltenvk_own);
#else
    (void)program;
    return NULL;
#endif
}
char* snx_vulkan_kosmickrisp_icd(const char* program) {
#ifdef __APPLE__
    return program_file(program, kosmickrisp_own, sizeof kosmickrisp_own / sizeof *kosmickrisp_own);
#else
    (void)program;
    return NULL;
#endif
}
void snx_vulkan_driver_free(SnxVulkanDriver* driver) {
    free(driver->library);
    free(driver->icd);
    *driver = (SnxVulkanDriver){0};
}
/* the driver d names, for a log line: its library, and the ICD the loader takes */
static void described(const SnxVulkanPlaces* at, const SnxVulkanDriver* d, char* out, size_t size) {
    if (d->icd)
        snprintf(out, size, "%s, ICD %s, through the Vulkan loader %s", d->name, d->icd, d->library);
    else if (d->library)
        snprintf(out, size, "%s at %s", d->name, d->library);
    else
        snprintf(out, size, "%s", at->system_driver ? at->system_driver : "the system's Vulkan loader");
}
/* the files of pick, a driver's name, found; 1, else 0 with the reason */
static int found_driver(const SnxVulkanPlaces* at, const char* pick, SnxVulkanDriver* out, char* reason,
                        size_t reason_size) {
    char where[1024];
    *out = (SnxVulkanDriver){0};
    if (!strcmp(pick, "moltenvk")) {
        out->library = program_file(at->program, moltenvk_own, sizeof moltenvk_own / sizeof *moltenvk_own);
        if (!out->library) out->library = installed_library(at, "libMoltenVK.dylib");
        if (!out->library) {
            looked_in(at, "lib", where, sizeof where);
            snprintf(reason, reason_size, "MoltenVK was not found: not %s", where);
            return 0;
        }
        out->name = "MoltenVK";
        return 1;
    }
    if (at->no_kosmickrisp) {
        snprintf(reason, reason_size, "%s", at->no_kosmickrisp);
        return 0;
    }
    out->icd = program_file(at->program, kosmickrisp_own, sizeof kosmickrisp_own / sizeof *kosmickrisp_own);
    if (!out->icd) out->icd = installed_icd(at);
    if (!out->icd) {
        looked_in(at, "share/vulkan/icd.d", where, sizeof where);
        snprintf(reason, reason_size, "KosmicKrisp was not found: no ICD %s", where);
        return 0;
    }
    out->library = program_file(at->program, loader_own, sizeof loader_own / sizeof *loader_own);
    if (!out->library) out->library = installed_library(at, "libvulkan.1.dylib");
    if (!out->library) {
        looked_in(at, "lib", where, sizeof where);
        snprintf(reason, reason_size, "KosmicKrisp needs the Vulkan loader, and none was found %s", where);
        snx_vulkan_driver_free(out);
        return 0;
    }
    out->name = "KosmicKrisp";
    return 1;
}
/* auto's driver: the one the environment names (VK_DRIVER_FILES), else KosmicKrisp where the Mac has it, else the
   MoltenVK the app carries, else the system's loader. playing names it, and because says why auto took it */
static void auto_driver(const SnxVulkanPlaces* at, SnxVulkanDriver* out, char* playing, size_t playing_size,
                        char* because, size_t because_size) {
    char cannot[2048] = "";
    *out = (SnxVulkanDriver){0};
    because[0] = 0;
    if (at->system_driver) {
        out->name = "the system's";
        snprintf(playing, playing_size, "%s", at->system_driver);
        return;
    }
    if (at->driver_files) {
        out->library = program_file(at->program, loader_own, sizeof loader_own / sizeof *loader_own);
        if (!out->library) out->library = installed_library(at, "libvulkan.1.dylib");
        out->name = "the environment's";
        snprintf(playing, playing_size, "the driver %s names, through the Vulkan loader %s", at->driver_files,
                 out->library ? out->library : "SDL finds");
        snprintf(because, because_size, "vulkan_driver=auto leaves the driver the environment names");
        return;
    }
    if (found_driver(at, "kosmickrisp", out, cannot, sizeof cannot)) {
        described(at, out, playing, playing_size);
        snprintf(because, because_size, "vulkan_driver=auto takes KosmicKrisp where the Mac has it");
        return;
    }
    out->library = program_file(at->program, moltenvk_own, sizeof moltenvk_own / sizeof *moltenvk_own);
    out->name = out->library ? "MoltenVK" : "the system's";
    described(at, out, playing, playing_size);
    snprintf(because, because_size, "vulkan_driver=auto%s, and %s", out->library ? "" : ", the app carries no MoltenVK",
             cannot);
}
int snx_vulkan_driver_find(const SnxVulkanPlaces* at, const char* pick, SnxVulkanDriver* out, char* reason,
                           size_t reason_size) {
    char playing[2048], because[2560];
    int asked = pick && pick[0] && strcmp(pick, "auto");
    *out = (SnxVulkanDriver){0};
    reason[0] = 0;
    if (asked) {
        if (strcmp(pick, "moltenvk") && strcmp(pick, "kosmickrisp")) {
            snprintf(reason, reason_size, "vulkan_driver=%s is not auto, moltenvk or kosmickrisp", pick);
            return 0;
        }
        if (at->system_driver) {
            snprintf(reason, reason_size, "%s is a driver of macOS",
                     strcmp(pick, "moltenvk") ? "KosmicKrisp" : "MoltenVK");
        } else if (found_driver(at, pick, out, reason, reason_size)) {
            described(at, out, playing, sizeof playing);
            snprintf(reason, reason_size, "%s (vulkan_driver=%s)", playing, pick);
            return 1;
        }
    }
    /* auto, or what plays in place of a driver the Mac has not, said beside the reason */
    auto_driver(at, out, playing, sizeof playing, because, sizeof because);
    if (asked) {
        out->instead = 1;
        append(reason, reason_size, ": %s plays instead", playing);
    } else if (because[0]) {
        snprintf(reason, reason_size, "%s (%s)", playing, because);
    }
    return 1;
}
int snx_vulkan_kosmickrisp_runs(const char* darwin, char* why, size_t why_size) {
    char* end = NULL;
    long major = darwin ? strtol(darwin, &end, 10) : 0;
    if (!darwin || end == darwin) {
        snprintf(why, why_size, "KosmicKrisp needs macOS 26 or later, and this Mac's kernel version \"%s\" says none",
                 darwin ? darwin : "");
        return 0;
    }
    /* Darwin 25 is macOS 26 */
    if (major < 25) {
        snprintf(why, why_size, "KosmicKrisp needs macOS 26 or later (Darwin 25), and this Mac runs Darwin %s", darwin);
        return 0;
    }
    return 1;
}
int snx_vulkan_driver_choose(const char* program, const char* pick, SnxVulkanDriver* out, char* reason,
                             size_t reason_size) {
    SnxVulkanPlaces at = {.program = program};
#if defined(__APPLE__) && defined(SNX_IOS)
    at.no_kosmickrisp = "KosmicKrisp is a driver of macOS, not of iOS";
#elif defined(__APPLE__)
    const char* home = getenv("HOME");
    char* sdks = home && home[0] ? snx_path_join(home, "VulkanSDK") : NULL;
    /* the Vulkan loader's own variables, which name its driver: auto leaves the driver they name */
    const char* files = getenv("VK_DRIVER_FILES");
    const char* files_name = "VK_DRIVER_FILES";
    char driver_files[4096], gate[512];
    at.prefixes[0] = "/opt/homebrew";
    at.prefixes[1] = "/usr/local";
    at.sdks = sdks;
    if (!files || !files[0]) {
        files = getenv("VK_ICD_FILENAMES");
        files_name = "VK_ICD_FILENAMES";
    }
    if (files && files[0]) {
        snprintf(driver_files, sizeof driver_files, "%s=%s", files_name, files);
        at.driver_files = driver_files;
    }
#ifdef __aarch64__
    struct utsname system;
    if (uname(&system)) {
        char error[256];
        snprintf(gate, sizeof gate, "KosmicKrisp needs macOS 26 or later, and uname could not say this Mac's: %s",
                 snx_os_error(error, sizeof error));
        at.no_kosmickrisp = gate;
    } else if (!snx_vulkan_kosmickrisp_runs(system.release, gate, sizeof gate)) {
        at.no_kosmickrisp = gate;
    }
#else
    (void)gate;
    at.no_kosmickrisp = "KosmicKrisp runs on Apple silicon, and this build is for Intel Macs";
#endif
#elif defined(__ANDROID__)
    at.system_driver = "the driver package gpu_driver names, else Android's own";
#else
    at.system_driver = "the system's Vulkan loader";
#endif
    int found = snx_vulkan_driver_find(&at, pick, out, reason, reason_size);
#if defined(__APPLE__) && !defined(SNX_IOS)
    free(sdks);
#endif
    return found;
}
/* from put in folder under leaf, a second name where the disk allows one, else a copy: a read-only app image, and an
   app macOS translocated, take no second name of the library they carry. A name of another app's library is broken
   rather than written through, which would change that app's library and signature too. 0, or 1 with the reason said */
static int copy_beside(const char* from, const char* folder, const char* leaf) {
    char error[256];
    char* to = snx_path_join(folder, leaf);
    int rc = 0;
    if (!to) return snx_error("cannot name the Vulkan driver %s in %s", leaf, folder);
    if (snx_is_file(to) && snx_remove_tree(to))
        rc = snx_error("cannot remove the Vulkan driver at %s: %s", to, snx_os_error(error, sizeof error));
    else if (snx_link_file(from, to) && snx_copy_file(from, to))
        rc = snx_error("cannot put the Vulkan driver %s in %s: %s", from, folder, snx_os_error(error, sizeof error));
    free(to);
    return rc;
}
int snx_deploy_vulkan_driver(const char* program, const char* folder) {
    int rc = 0;
#if defined(__APPLE__) && !defined(SNX_IOS)
    /* KosmicKrisp: the JSON the loader reads, the driver it names and the loader itself go beside it. A build that
       carries none has nothing to put (a checkout's, and an app built without SNX_KOSMICKRISP) */
    char* icd = snx_vulkan_kosmickrisp_icd(program);
    if (icd) {
        static const char* const parts[] = {"libvulkan_kosmickrisp.dylib", "libvulkan.1.dylib"};
        char* dir = snx_dirname(icd);
        for (size_t i = 0; i < sizeof parts / sizeof* parts; ++i) {
            char* from = dir ? snx_path_join(dir, parts[i]) : NULL;
            int rc_here = !from || !snx_is_file(from)
                              ? snx_error("the KosmicKrisp driver %s, which %s carries, is not there", parts[i], program)
                              : copy_beside(from, folder, parts[i]);
            if (!rc) rc = rc_here;
            free(from);
        }
        /* the JSON last: its presence in a game's folder says that the driver and its loader are there too */
        if (!rc) rc = copy_beside(icd, folder, "libkosmickrisp_icd.json");
        free(dir);
        free(icd);
    }
#endif
    {
        char* from = snx_vulkan_driver(program);
        int rc_here = from ? copy_beside(from, folder, "libMoltenVK.dylib") : 0;
        /* the system's loader serves everywhere else, and a build that carries none (a checkout's) leaves the system's */
        if (!rc) rc = rc_here;
        free(from);
    }
    return rc;
}
int snx_path_within(const char* path, const char* parent) {
    char *a = snx_absolute(path), *b = snx_absolute(parent);
    size_t i = 0;
    int same = 1;
    if (!a || !b) {
        free(a);
        free(b);
        return 1;
    }
    size_t n = strlen(b);
    while (n > 1 && snx_is_separator(b[n - 1])) --n;
    while (i < n) {
#ifdef _WIN32
        if (!(snx_is_separator(a[i]) && snx_is_separator(b[i])) && snx_to_lower(a[i]) != snx_to_lower(b[i]))
            same = 0;
#else
        if (a[i] != b[i]) same = 0;
#endif
        if (!same || !a[i]) break;
        ++i;
    }
    same = same && i == n && (!a[i] || snx_is_separator(a[i]) || (n == 1 && snx_is_separator(b[0])));
    free(a);
    free(b);
    return same;
}
static int copy_tree(const char* src, const char* dst, unsigned depth) {
    char **names, *a, *b;
    size_t n;
    int rc = 0;
    if (depth > 128 || snx_is_link(src) || snx_is_link(dst)) return snx_error("invalid copy tree: %s", src);
    if (!snx_is_dir(src)) return snx_copy_file(src, dst);
    if (snx_mkdirs(dst) || snx_list_dir(src, &names, &n)) return 1;
    for (size_t i = 0; i < n && !rc; ++i) {
        a = snx_path_join(src, names[i]);
        b = snx_path_join(dst, names[i]);
        rc = copy_tree(a, b, depth + 1);
        free(a);
        free(b);
    }
    snx_free_strings(names, n);
    return rc;
}
int snx_copy_tree(const char* src, const char* dst) {
    if (snx_path_within(dst, src)) return snx_error("copy destination is inside source: %s", dst);
    return copy_tree(src, dst, 0);
}
int snx_make_executable(const char* path) {
#ifdef _WIN32
    (void)path;
    return 0;
#else
    return chmod(path, 0755) != 0;
#endif
}
SnxU64 snx_file_size(const char* path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA st;
    wchar_t* p = wide(path);
    int ok = p && GetFileAttributesExW(p, GetFileExInfoStandard, &st);
    free(p);
    return ok ? ((SnxU64)st.nFileSizeHigh << 32) | st.nFileSizeLow : 0;
#else
    struct stat st;
    return stat(path, &st) || st.st_size < 0 ? 0 : (SnxU64)st.st_size;
#endif
}
#ifdef _WIN32
/* FILETIME counts 100 ns from 1601, 11644473600 s before 1970 */
#define SNX_FILETIME_1970 116444736000000000ull
#endif
SnxU64 snx_file_time(const char* path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA st;
    wchar_t* p = wide(path);
    int ok = p && GetFileAttributesExW(p, GetFileExInfoStandard, &st);
    SnxU64 t = ok ? ((SnxU64)st.ftLastWriteTime.dwHighDateTime << 32) | st.ftLastWriteTime.dwLowDateTime : 0;
    free(p);
    return t > SNX_FILETIME_1970 ? (t - SNX_FILETIME_1970) / 10000000ull : 0;
#else
    struct stat st;
    return stat(path, &st) || st.st_mtime < 0 ? 0 : (SnxU64)st.st_mtime;
#endif
}
int snx_set_file_time(const char* path, SnxU64 seconds) {
#ifdef _WIN32
    wchar_t* p = wide(path);
    HANDLE file = p ? CreateFileW(p, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
                    : INVALID_HANDLE_VALUE;
    SnxU64 t = seconds * 10000000ull + SNX_FILETIME_1970;
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)t;
    ft.dwHighDateTime = (DWORD)(t >> 32);
    int ok = file != INVALID_HANDLE_VALUE && SetFileTime(file, NULL, &ft, &ft);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        CloseHandle(file);
        SetLastError(error); /* the reason SetFileTime gave, for snx_os_error */
    }
    free(p);
    return !ok;
#else
    struct timespec times[2];
    times[0].tv_sec = times[1].tv_sec = (time_t)seconds;
    times[0].tv_nsec = times[1].tv_nsec = 0;
    return utimensat(AT_FDCWD, path, times, 0) != 0;
#endif
}
SnxU64 snx_free_space(const char* path) {
#ifdef _WIN32
    wchar_t* p = wide(path);
    ULARGE_INTEGER avail;
    int ok = p && GetDiskFreeSpaceExW(p, &avail, NULL, NULL);
    free(p);
    return ok ? (SnxU64)avail.QuadPart : 0;
#else
    struct statvfs st;
    return statvfs(path, &st) ? 0 : (SnxU64)st.f_bavail * st.f_frsize;
#endif
}
int snx_disk_room(const char* path, SnxU64* free_bytes, SnxU64* total) {
#ifdef _WIN32
    wchar_t* p = wide(path);
    ULARGE_INTEGER avail, all;
    int ok = p && GetDiskFreeSpaceExW(p, &avail, &all, NULL);
    free(p);
    if (!ok) return -1;
    *free_bytes = (SnxU64)avail.QuadPart;
    *total = (SnxU64)all.QuadPart;
#else
    struct statvfs st;
    if (statvfs(path, &st)) return -1;
    *free_bytes = (SnxU64)st.f_bavail * st.f_frsize;
    *total = (SnxU64)st.f_blocks * st.f_frsize;
#endif
    return 0;
}
/* path, or the nearest folder above it that is there */
static char* existing_folder(const char* path) {
    char *at = snx_absolute(path), *up;
    while (at && !snx_is_dir(at)) {
        up = snx_dirname(at);
        if (!strcmp(up, at)) {
            free(up);
            break;
        }
        free(at);
        at = up;
    }
    return at;
}
int snx_same_disk(const char* a, const char* b) {
    char *x = existing_folder(a), *y = existing_folder(b);
    int same = 0;
#ifdef _WIN32
    wchar_t *wx = x ? wide(x) : NULL, *wy = y ? wide(y) : NULL, vx[MAX_PATH + 1], vy[MAX_PATH + 1];
    same = wx && wy && GetVolumePathNameW(wx, vx, MAX_PATH + 1) && GetVolumePathNameW(wy, vy, MAX_PATH + 1) &&
           !_wcsicmp(vx, vy);
    free(wx);
    free(wy);
#else
    struct stat sx, sy;
    same = x && y && !stat(x, &sx) && !stat(y, &sy) && sx.st_dev == sy.st_dev;
#endif
    free(x);
    free(y);
    return same;
}
#ifdef _WIN32
static int file_id(const char* path, BY_HANDLE_FILE_INFORMATION* info) {
    wchar_t* w = wide(path);
    HANDLE file = w ? CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS, NULL)
                    : INVALID_HANDLE_VALUE;
    int ok = file != INVALID_HANDLE_VALUE && GetFileInformationByHandle(file, info);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    free(w);
    return ok;
}
#endif
int snx_same_file(const char* a, const char* b) {
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION x, y;
    return file_id(a, &x) && file_id(b, &y) && x.dwVolumeSerialNumber == y.dwVolumeSerialNumber &&
           x.nFileIndexHigh == y.nFileIndexHigh && x.nFileIndexLow == y.nFileIndexLow;
#else
    struct stat x, y;
    return !stat(a, &x) && !stat(b, &y) && x.st_dev == y.st_dev && x.st_ino == y.st_ino;
#endif
}
/* The per-user data folder, an installed build's and a build tree's: $XDG_DATA_HOME/scarletnx or
   ~/.local/share/scarletnx, ~/Library/Application Support/scarletnx on macOS, %APPDATA%\scarletnx on Windows. These are
   the folders SDL_GetPrefPath("scarletnx", ...) gives the GUI; the app's Documents on iOS. NULL when the environment
   names no home */
#if !defined(_WIN32) && !defined(SNX_IOS)
/* xdg/scarletnx (xdg an XDG variable's value, NULL for none), else ~/fallback/scarletnx: the XDG spec ignores a
   relative path */
static char* home_folder(const char* xdg, const char* fallback) {
    const char* base = xdg;
    if (base && base[0] == '/') return snx_path_join(base, "scarletnx");
    base = getenv("HOME");
    if (!base || !base[0]) return NULL;
    char* parent = snx_path_join(base, fallback);
    char* path = snx_path_join(parent, "scarletnx");
    free(parent);
    return path;
}
#endif
char* snx_user_data_dir(void) {
#ifdef _WIN32
    const char* base = getenv("APPDATA");
    return base && base[0] ? snx_path_join(base, "scarletnx") : NULL;
#elif defined(SNX_IOS)
    /* the app's Documents, which the Files app and the Finder show the user (UIFileSharingEnabled, dist/apple) */
    const char* home = getenv("HOME");
    return home && home[0] ? snx_path_join(home, "Documents") : NULL;
#elif defined(__APPLE__)
    return home_folder(NULL, "Library/Application Support");
#else
    return home_folder(getenv("XDG_DATA_HOME"), ".local/share");
#endif
}
static const char* const kind_names[] = {NULL, "config", "state", "cache"};
char* snx_user_dir(int kind) {
    char *data, *dir;
#ifdef SNX_XDG
    if (kind == SNX_USER_CONFIG) return home_folder(getenv("XDG_CONFIG_HOME"), ".config");
    if (kind == SNX_USER_STATE) return home_folder(getenv("XDG_STATE_HOME"), ".local/state");
    if (kind == SNX_USER_CACHE) return home_folder(getenv("XDG_CACHE_HOME"), ".cache");
#endif
#ifdef SNX_IOS
    /* the Files app shows Documents: what is not the player's to move (settings, state, caches) is in Library, which the system makes */
    if (kind != SNX_USER_DATA) {
        const char* home = getenv("HOME");
        char* base = home && home[0] ? snx_path_join(home, "Library/scarletnx") : NULL;
        dir = base ? snx_path_join(base, kind_names[kind]) : NULL;
        free(base);
        return dir;
    }
#endif
    data = snx_user_data_dir();
    if (!data || kind == SNX_USER_DATA) return data;
    dir = snx_path_join(data, kind_names[kind]);
    free(data);
    return dir;
}
/* the same folder, each resolved: one reached through a link is still it */
static int same_folder(const char* a, const char* b) { return snx_path_within(a, b) && snx_path_within(b, a); }
int snx_app_dir(const char* folder) {
    char *user = snx_user_data_dir(), *full = snx_absolute(folder);
    const char* base = full;
    int app = full && user && same_folder(full, user);
    for (const char* s = full; s && *s; ++s)
        if (snx_is_separator(*s) && s[1]) base = s + 1;
    if (full && !app)
        app = !strncmp(base, SNX_PORTABLE_DIR, strlen(SNX_PORTABLE_DIR)) &&
              (!base[strlen(SNX_PORTABLE_DIR)] || snx_is_separator(base[strlen(SNX_PORTABLE_DIR)]));
#ifdef __ANDROID__
    if (full && !app) app = same_folder(full, SNX_ANDROID_DIR);
#endif
    free(user);
    free(full);
    return app;
}
char* snx_app_kind_default(const char* app, int kind) {
    char* user = snx_user_data_dir();
    int own = user && same_folder(app, user);
    free(user);
    if (own) return snx_user_dir(kind);
    return kind == SNX_USER_DATA ? snx_strdup(app) : snx_path_join(app, kind_names[kind]);
}
/* the folder of kind the app.ini in config names in [folders] (Settings > App > Folders' Logs and Caches): 1 with *dir
   a full path, -1 with *dir a value that is not one, 0 for none (no file, no key, an empty one) */
static int picked_kind(const char* config, int kind, char** dir) {
    size_t size;
    char *path = snx_path_join(config, "app.ini"), *text = path ? (char*)snx_read_file(path, &size) : NULL, *key,
         *value;
    const char* wanted = kind == SNX_USER_STATE ? "state" : "cache";
    int found = 0;
    SnxIni ini;
    *dir = NULL;
    if (text) snx_ini_start(&ini, text);
    while (text && snx_ini_next(&ini, &key, &value))
        if (!strcmp(ini.section, "folders") && !strcmp(key, wanted)) { /* the last one, as the launcher reads it */
            free(*dir);
            *dir = *value ? snx_strdup(value) : NULL;
            found = !*value ? 0 : snx_path_is_full(value) ? 1 : -1;
        }
    free(text);
    free(path);
    return found;
}
char* snx_app_kind_dir(const char* app, int kind) {
    char *dir = snx_app_kind_default(app, kind), *picked;
    if (kind != SNX_USER_STATE && kind != SNX_USER_CACHE) return dir;
    char* config = snx_app_kind_default(app, SNX_USER_CONFIG);
    int found = config ? picked_kind(config, kind, &picked) : 0;
    if (found > 0) {
        free(dir);
        dir = picked;
        picked = NULL;
    } else if (found < 0 && !(g_common.said_kinds & 1u << kind)) {
        g_common.said_kinds |= 1u << kind;
        snx_error("%s/app.ini: [folders] %s = %s is not a full path: the %s go in %s", config,
                  kind == SNX_USER_STATE ? "state" : "cache", picked, kind == SNX_USER_STATE ? "logs" : "caches",
                  dir ? dir : "(none)");
    }
    if (found) free(picked);
    free(config);
    return dir;
}
/* the data folder (snx_app_dir) full, an absolute path, is in, from full itself up; NULL for none */
static char* app_of(const char* full) {
    char *app = snx_strdup(full), *up;
    while (app && !snx_app_dir(app)) {
        up = snx_dirname(app);
        if (!strcmp(up, app)) {
            free(up);
            up = NULL;
        }
        free(app);
        app = up;
    }
    return app;
}
char* snx_user_path(const char* path, int kind) {
    char *full = kind == SNX_USER_DATA ? NULL : snx_absolute(path), *app = full ? app_of(full) : NULL;
    char *root = NULL, *moved = NULL;
    const char* rest;
    if (app) root = snx_app_kind_dir(app, kind);
    /* a config folder that is the data folder divides nothing */
    if (root && !same_folder(root, app)) {
        rest = full + strlen(app);
        while (snx_is_separator(*rest)) ++rest;
        moved = *rest ? snx_path_join(root, rest) : snx_strdup(root);
    }
    free(full);
    free(app);
    free(root);
    return moved ? moved : snx_strdup(path);
}
/* snx_menu_dir, with the folder Folders picked (snx_app_kind_dir) or without it (snx_app_kind_default) */
static char* menu_dir(const char* folder, int kind, char* (*of)(const char* app, int kind)) {
    char *full = folder ? snx_absolute(folder) : NULL, *app = full ? app_of(full) : NULL;
    if (!app) app = snx_user_data_dir(); /* any other folder's: the user's */
    char* dir = app ? of(app, kind) : snx_user_dir(kind);
    free(app);
    free(full);
    return dir;
}
char* snx_menu_dir(const char* folder, int kind) { return menu_dir(folder, kind, snx_app_kind_dir); }
char* snx_menu_kind_default(const char* folder, int kind) { return menu_dir(folder, kind, snx_app_kind_default); }
int snx_left_behind(const char* old, const char* now) { return strcmp(old, now) && snx_path_exists(old); }
int snx_launcher_name(const char* name) {
    static const char own[] = "scarletnx";
    size_t i;
    for (i = 0; own[i]; ++i)
        if (snx_to_lower(name[i]) != own[i]) return 0;
    return !name[i] || name[i] == '-' || name[i] == '.';
}
const SnxGameFile snx_game_files[SNX_GAME_FILES] = {{".log", SNX_USER_STATE},
                                                    {"threads.txt", SNX_USER_STATE},
                                                    {".ini", SNX_USER_CONFIG},
                                                    {"shader_cache/vk_pipelines.bin", SNX_USER_CACHE}};
char* snx_work_dir(const char* data_dir) {
    char *old = snx_path_join(data_dir, "work"), *work = snx_user_path(old, SNX_USER_CACHE);
    if (snx_left_behind(old, work))
        printf("scarletnx: %s was left by an older version: this one keeps it in %s (docs/user/deprecations.md)\n", old,
               work);
    free(old);
    return work;
}
char* snx_recompiled_dir(const char* data_dir) { return snx_path_join(data_dir, "recompiled"); }
char* snx_recompiled_game(const char* data_dir, const char* name) {
    char *dir = snx_recompiled_dir(data_dir), *now = dir ? snx_path_join(dir, name) : NULL;
    free(dir);
    if (!now || snx_is_dir(now)) return now;
    char* old_dir = snx_path_join(data_dir, "games");
    char* old = old_dir ? snx_path_join(old_dir, name) : NULL;
    free(old_dir);
    if (old && snx_is_dir(old)) {
        free(now);
        return old;
    }
    free(old);
    return now;
}
void snx_recompiled_left(const char* data_dir) {
    char *old = snx_path_join(data_dir, "games"), *now = snx_recompiled_dir(data_dir);
    if (old && now && snx_left_behind(old, now))
        fprintf(stderr,
                "scarletnx: %s was left by an older version: new games go to %s, and the ones in it still run until "
                "they are moved there (docs/user/deprecations.md)\n",
                old, now);
    free(old);
    free(now);
}
char* snx_portable_dir(const char* program_dir) {
    const char *appimage = getenv("APPIMAGE"), *appdir = getenv("APPDIR");
    char *dir = NULL, *copy;
    /* an AppImage's programs run from its mount: the copy is the AppImage file. APPIMAGE is inherited, so it counts
       only for a program inside the mount */
    if (appimage && appimage[0] == '/' && appdir && appdir[0] && program_dir && snx_path_within(program_dir, appdir))
        dir = snx_dirname(appimage);
    else if (program_dir)
        dir = snx_strdup(program_dir);
    copy = dir ? snx_path_join(dir, SNX_PORTABLE_DIR) : NULL;
    free(dir);
    return copy;
}
char* snx_data_dir(const char* program_dir) {
    char* copy = snx_portable_dir(program_dir);
    if (copy && snx_is_dir(copy)) return copy;
    free(copy);
    return snx_user_data_dir();
}
char* snx_checkout(const char* checkout, const char* build, const char* program_dir) {
    char* path = checkout ? snx_path_join(checkout, "CMakeLists.txt") : NULL;
    /* a copy installed from that build (cmake --install) has none, even where the checkout still is */
    int ours = path && snx_is_file(path) &&
               (!build || !program_dir || (snx_is_dir(build) && snx_path_within(program_dir, build)));
    free(path);
    return ours ? snx_strdup(checkout) : NULL;
}
const char* const snx_checkout_files[SNX_CHECKOUT_FILES] = {"games", "work", "prod.keys", "title.keys", "linux"};
char* snx_checkout_now(const char* data, int file) {
    if (!strcmp(snx_checkout_files[file], "games")) return snx_recompiled_dir(data);
    char* path = snx_path_join(data, snx_checkout_files[file]);
    if (strcmp(snx_checkout_files[file], "work")) return path;
    char* now = snx_user_path(path, SNX_USER_CACHE);
    free(path);
    return now;
}
void snx_checkout_left(const char* checkout, const char* data) {
    char *old, *now;
    if (same_folder(checkout, data)) return; /* the data folder's own older files: snx_recompiled_left, snx_work_dir */
    for (int i = 0; i < SNX_CHECKOUT_FILES; ++i) {
        old = snx_path_join(checkout, snx_checkout_files[i]);
        now = snx_checkout_now(data, i);
        if (old && now && snx_left_behind(old, now))
            fprintf(stderr,
                    "scarletnx: %s was left by an older version: this one keeps it in %s (docs/user/deprecations.md)\n", old,
                    now);
        free(old);
        free(now);
    }
}
char* snx_dump_name(const char* input) {
    const char *base = input, *p;
    size_t n = 0;
    for (p = input; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    const char* end = strrchr(base, '.');
    if (!end || end == base) end = base + strlen(base);
    char* name = (char*)snx_alloc((size_t)(end - base) + 1);
    for (p = base; p < end; ++p)
        if ((unsigned char)*p >= 128 || snx_is_alnum(*p) || *p == '-' || *p == '_' || *p == ' ')
            name[n++] = *p;
    while (n && name[n - 1] == ' ') --n;
    name[n] = 0;
    if (!n) {
        free(name);
        return snx_strdup("Game");
    }
    return name;
}
const char* snx_folder_unfit(const char* folder, int run) {
    char* probe;
#if defined(ST_NOEXEC) && !defined(__ANDROID__) /* there a game's code loads from a copy in the cache */
    struct statvfs st;
#endif
    if (!snx_path_exists(folder)) return "is not there";
    if (!snx_is_dir(folder)) return "is not a folder";
    /* made and taken away: the folder's rights, as the system applies them (ACLs, a read-only mount) */
    probe = snx_temp_dir(folder);
    if (!probe) return "cannot be written";
    snx_remove_tree(probe);
    free(probe);
#if defined(ST_NOEXEC) && !defined(__ANDROID__)
    if (run && !statvfs(folder, &st) && (st.f_flag & ST_NOEXEC)) return "is on a disk that runs no programs (noexec)";
#else
    (void)run;
#endif
    return NULL;
}
char* snx_temp_root(void) {
#ifdef _WIN32
    wchar_t temp[32768];
    return GetTempPathW(32768, temp) ? utf8(temp) : NULL;
#else
    const char* root = getenv("TMPDIR");
    return snx_strdup(root && *root ? root : "/tmp");
#endif
}
char* snx_tmpfs_work(SnxU64 title) {
    char text[17], *root, *work;
    if (!snx_title_text(title, text) || !(root = snx_temp_root())) return NULL;
    work = snx_path_join(root, "scarletnx/work");
    char* path = snx_path_join(work, text);
    free(work);
    free(root);
    return path;
}
int snx_tmpfs_check(const char* path, char* fs, size_t fs_size) {
#if defined(__linux__)
    struct statfs s;
    if (statfs(path, &s)) {
        if (fs && fs_size) snprintf(fs, fs_size, "unknown");
        return -1;
    }
    if (fs && fs_size) {
        const char* name = NULL;
        switch ((unsigned long)s.f_type) {
            case 0x01021994UL: name = "tmpfs"; break;   /* TMPFS_MAGIC */
            case 0x858458F6UL: name = "ramfs"; break;   /* RAMFS_MAGIC */
            case 0x2FC12FC1UL: name = "zfs"; break;     /* ZFS_SUPER_MAGIC */
            case 0xEF53UL: name = "ext4"; break;        /* EXT4_SUPER_MAGIC */
            case 0x9123683EUL: name = "btrfs"; break;   /* BTRFS_SUPER_MAGIC */
            case 0x58465342UL: name = "xfs"; break;     /* XFS_SUPER_MAGIC */
            case 0x794C7630UL: name = "overlay"; break; /* OVERLAYFS_SUPER_MAGIC */
            case 0x6969UL: name = "nfs"; break;         /* NFS_SUPER_MAGIC */
            case 0x65735546UL: name = "fuse"; break;    /* FUSE_SUPER_MAGIC */
            default: break;
        }
        if (name)
            snprintf(fs, fs_size, "%s", name);
        else
            snprintf(fs, fs_size, "filesystem 0x%lx", (unsigned long)s.f_type);
    }
    return s.f_type == 0x01021994UL;
#elif defined(__FreeBSD__)
    struct statfs s;
    if (statfs(path, &s)) {
        if (fs && fs_size) snprintf(fs, fs_size, "unknown");
        return -1;
    }
    if (fs && fs_size) snprintf(fs, fs_size, "%s", s.f_fstypename);
    return !strcmp(s.f_fstypename, "tmpfs");
#else
    (void)path;
    if (fs && fs_size) snprintf(fs, fs_size, "unknown");
    return -1; /* macOS and Windows have no tmpfs to name */
#endif
}
char* snx_temp_dir(const char* parent) {
    char* path;
#ifdef _WIN32
    wchar_t* wp;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    path = parent ? snx_strdup(parent) : snx_temp_root();
    if (!path) return NULL;
    /* Key material must remain private even when the selected temp parent is shared. The folder is the user's and
       the system's alone. The user is named by its own SID: Wine cannot apply the owner-rights SID (OW), makes the
       folder with no rights at all and answers Access denied; OW stays as the fallback if the SID cannot be read. */
    wchar_t sddl[256];
    wchar_t* user = NULL;
    HANDLE token = NULL;
    char why[512] = "";
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        union {
            TOKEN_USER user;
            unsigned char bytes[256];
        } info;
        DWORD size = 0;
        if (!GetTokenInformation(token, TokenUser, &info, sizeof(info), &size) ||
            !ConvertSidToStringSidW(info.user.User.Sid, &user))
            snx_os_error(why, sizeof why);
        CloseHandle(token);
    } else
        snx_os_error(why, sizeof why);
    if (!user)
        snx_error("cannot read the user's SID (%s): the temp folder names the owner (OW), which Wine cannot apply",
                  why);
    swprintf(sddl, 256, L"D:P(A;OICI;FA;;;%ls)(A;OICI;FA;;;SY)", user ? user : L"OW");
    if (user) LocalFree(user);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, NULL)) {
        free(path);
        return NULL;
    }
    SECURITY_ATTRIBUTES security = {0};
    security.nLength = sizeof(security);
    security.lpSecurityDescriptor = descriptor;
    for (unsigned i = 0; i < 10000; ++i) {
        char* name =
            snx_format(".snx-%lu-%lu-%u", (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount(), i);
        char* candidate = snx_path_join(path, name);
        free(name);
        wp = wide(candidate);
        int ok = wp && CreateDirectoryW(wp, &security);
        free(wp);
        if (ok) {
            LocalFree(descriptor);
            free(path);
            return candidate;
        }
        free(candidate);
        if (GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
    LocalFree(descriptor);
    free(path);
    return NULL;
#else
    char* root = parent ? NULL : snx_temp_root();
    path = snx_path_join(parent ? parent : root, ".snx-XXXXXX");
    free(root);
    if (!mkdtemp(path)) {
        free(path);
        return NULL;
    }
    return path;
#endif
}
double snx_seconds(void) { return (double)plat_ns() / 1e9; }
unsigned snx_cpu_count(void) {
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwNumberOfProcessors ? info.dwNumberOfProcessors : 1;
#elif defined(__linux__) || defined(__APPLE__) || defined(__NetBSD__)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (unsigned)n : 1;
#elif defined(__FreeBSD__)
    int ncpu = 0;
    size_t len = sizeof(ncpu);
    int mib[2] = {CTL_HW, HW_NCPU};
    if (sysctl(mib, 2, &ncpu, &len, NULL, 0) == -1) return 1;
    return ncpu;
#endif
}
/* The physical cores, where the system says them: how many a whole thread can use before two share one core's
   throughput. 0 where it does not say (Windows, an ARM board's /proc/cpuinfo) */
static unsigned snx_physical_cores(void) {
#if defined(__APPLE__)
    int cores = 0;
    size_t len = sizeof(cores);
    return sysctlbyname("hw.physicalcpu", &cores, &len, NULL, 0) ? 0 : cores > 0 ? (unsigned)cores : 0;
#elif defined(__linux__)
    /* /proc/cpuinfo names each processor in a block, blank lines between, its "physical id" and "core id": a pair
       is one physical core, and the threads of a hyperthread share its pair. Where a block names neither id (ARM)
       none count, and the caller takes the logical count instead */
    unsigned pair[512][2], n = 0, cores = 0, phys = 0, core = 0;
    char line[256];
    int have = 0, saw = 0;
    FILE* f = fopen("/proc/cpuinfo", "r");
    if (!f) return 0;
    for (;;) {
        int full = fgets(line, sizeof line, f) != NULL;
        char* colon = full ? strchr(line, ':') : NULL;
        if (!colon) {
            if (saw && have) { /* a processor's block ended, at a blank line or the file's end */
                int found = 0, i;
                for (i = 0; i < (int)n; ++i)
                    if (pair[i][0] == phys && pair[i][1] == core) found = 1;
                if (!found && n < sizeof pair / sizeof *pair) {
                    pair[n][0] = phys;
                    pair[n][1] = core;
                    ++n;
                    ++cores;
                }
            }
            saw = 0;
            have = 0;
            if (!full) break;
        } else {
            char* field = line;
            while (*field == ' ' || *field == '\t') ++field;
            if (!strncmp(field, "physical id", 11)) {
                phys = (unsigned)strtoul(colon + 1, NULL, 10);
                saw = 1;
            } else if (!strncmp(field, "core id", 7)) {
                core = (unsigned)strtoul(colon + 1, NULL, 10);
                have = 1;
                saw = 1;
            }
        }
    }
    fclose(f);
    return cores;
#else
    return 0;
#endif
}
/* The threads an AOT pass (a game's recompile) is given when its count is left open, the recompile setting's Auto: a
   core each, the physical cores where the system says them, else the processors, capped so a part of the machine
   stays free for the rest of it. A pass that took every core would make the computer it shares stutter for the
   minutes a large game's recompile runs and heat a phone, and an LLVM pass gains little from a hyperthread anyway:
   two threads on one physical core only split its throughput. The cap is a reserve of one whole core, or a quarter
   of the machine where that is more, which is at most 75% rounded down: a machine of 4 keeps a thread off, 6 and 8
   keep two, 16 keep four, 32 keep eight. Never 0, and never more than the online processors */
unsigned snx_build_threads(void) {
    unsigned logical = snx_cpu_count(), cores = snx_physical_cores();
    unsigned jobs = (cores ? cores : logical) * 75u / 100u;
    if (jobs < 1u) jobs = 1u;
    if (jobs > logical) jobs = logical;
    return jobs;
}
/* what the units under way take: the footprint less the recompile's own and the finished units' code */
static SnxU64 aot_units(const SnxAotGovernor* g, const SnxAotReading* r) {
    SnxU64 own = g->base + r->held;
    return r->footprint > own ? r->footprint - own : 0;
}
unsigned snx_aot_threads(int threads, int most, char* note, size_t size) {
    if (size) note[0] = 0;
    if (threads > 0) {
        if (most > 0)
            snprintf(note, size, "aot_threads_most=%d bounds Auto only: aot_threads=%d compiles on %d threads", most,
                     threads, threads);
        return (unsigned)threads;
    }
    return most > 0 ? (unsigned)most : snx_build_threads();
}
void snx_aot_governor_start(SnxAotGovernor* g, unsigned most, int most_key, SnxU64 footprint, const char* pressure,
                            char* line, size_t size) {
    SnxAotGovernor start = {0};
    char own[96], ceiling[64];
    start.most = most ? most : 1u;
    start.threads = 1u;
    start.base = footprint;
    *g = start;
    if (footprint)
        snprintf(own, sizeof own, "%llu MiB in use before the first unit", (unsigned long long)(footprint >> 20));
    else
        snprintf(own, sizeof own, "the system does not say what the process takes");
    if (most_key)
        snprintf(ceiling, sizeof ceiling, "aot_threads_most=%u gives", g->most);
    else
        snprintf(ceiling, sizeof ceiling, "the cores give with a quarter left free");
    if (g->most == 1u)
        snprintf(line, size, "aot_threads=0 (Auto): 1 thread, what %s", ceiling);
    else
        snprintf(line, size,
                 "aot_threads=0 (Auto): 1 thread to start, %s; once that unit has ended one more opens each second "
                 "while the memory holds it, up to the %u %s. The system's memory pressure: %s",
                 own, g->most, ceiling, pressure);
}
int snx_aot_governor_tick(SnxAotGovernor* g, const SnxAotReading* r, const char* pressure, char* line, size_t size) {
    SnxU64 units = aot_units(g, r), open_take, more = 0;
    char memory[384];
    int measured = r->footprint && g->base, room = 1;
    line[0] = 0;
    if (measured && r->active && units / r->active > g->unit) g->unit = units / r->active;
    if (g->threads >= g->most || !r->left || !r->done) return 0;
    if (r->pressure > 0) {
        if (g->said != 2)
            snprintf(line, size,
                     "aot_threads=0 (Auto): stays on %u thread%s of %u: the system says memory is short (%s)",
                     g->threads, g->threads == 1u ? "" : "s", g->most, pressure);
        g->said = 2;
        return 0;
    }
    if (measured && r->available_known) {
        /* each thread open takes a unit's most sooner or later: what they have not taken yet is still to come */
        open_take = (SnxU64)g->threads * g->unit;
        if (open_take > units) more = open_take - units;
        room = r->available >= more + 2u * g->unit;
        snprintf(memory, sizeof memory,
                 "%llu MiB in use, %llu MiB of them before the first unit and %llu MiB finished units' code, so a "
                 "unit under way took %llu MiB at most; the system can still give %llu MiB, the %u thread%s open may "
                 "take %llu MiB more, and 2 units more are %llu MiB",
                 (unsigned long long)(r->footprint >> 20), (unsigned long long)(g->base >> 20),
                 (unsigned long long)(r->held >> 20), (unsigned long long)(g->unit >> 20),
                 (unsigned long long)(r->available >> 20), g->threads, g->threads == 1u ? "" : "s",
                 (unsigned long long)(more >> 20), (unsigned long long)(2u * g->unit >> 20));
    } else
        snprintf(memory, sizeof memory, "the system does not say %s: the cores alone decide",
                 measured ? "what memory it can still give" : "what the process takes");
    if (!room) {
        if (g->said != 1)
            snprintf(line, size, "aot_threads=0 (Auto): stays on %u thread%s of %u: %s", g->threads,
                     g->threads == 1u ? "" : "s", g->most, memory);
        g->said = 1;
        return 0;
    }
    g->said = 0;
    ++g->threads;
    snprintf(line, size, "aot_threads=0 (Auto): thread %u of %u opens: %s", g->threads, g->most, memory);
    return 1;
}
void snx_aot_read(SnxAotReading* r, char* pressure, size_t size) {
    r->footprint = snx_footprint();
    r->available = 0;
    r->available_known = snx_memory_available(&r->available);
    r->pressure = snx_memory_pressure(pressure, size);
}
/* Code arenas (code_loader.c) reserve their address space once and give each page access only when it is used: the
   loader writes into read-write pages and makes them executable before anything jumps there */
void snx_abort_request(int on) { g_common.abort_requested = on; }
int snx_abort_requested(void) { return g_common.abort_requested; }
SnxU64 snx_footprint(void) {
#if defined(__APPLE__)
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
        return info.phys_footprint;
    return 0;
#elif defined(__linux__)
    unsigned long long pages = 0, resident = 0;
    FILE* f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    if (fscanf(f, "%llu %llu", &pages, &resident) != 2) resident = 0;
    fclose(f);
    return (SnxU64)resident * plat_mem_page();
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS info;
    return GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof info) ? (SnxU64)info.WorkingSetSize : 0;
#elif defined(__FreeBSD__)
    struct kinfo_proc proc;
    size_t size = sizeof proc;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)getpid()};
    if (sysctl(mib, 4, &proc, &size, NULL, 0) || size != sizeof proc) return 0;
    return (SnxU64)proc.ki_rssize * plat_mem_page();
#else
    return 0;
#endif
}
SnxU64 snx_process_cpu_ns(void) {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    /* 100 ns ticks */
    return (((SnxU64)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) +
            ((SnxU64)user.dwHighDateTime << 32 | user.dwLowDateTime)) *
           100;
#else
    struct timespec t;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t)) return 0;
    return (SnxU64)t.tv_sec * 1000000000u + (SnxU64)t.tv_nsec;
#endif
}
long snx_status_tracer(const char* status) {
    static const char key[] = "TracerPid:";
    const char* p;
    for (p = status; (p = strstr(p, key)); p += sizeof key - 1)
        if (p == status || p[-1] == '\n') { /* the line's own start, not a word inside another line */
            char* end;
            long pid = strtol(p + sizeof key - 1, &end, 10); /* past the tab */
            return end != p + sizeof key - 1 && pid > 0 ? pid : 0;
        }
    return 0;
}
int snx_debugger(char* name, size_t size) {
#if defined(__linux__)
    char status[4096], path[64], comm[SNX_DEBUGGER_NAME];
    size_t got;
    long pid;
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    got = fread(status, 1, sizeof status - 1, f); /* TracerPid is among the first lines */
    fclose(f);
    status[got] = 0;
    if (!(pid = snx_status_tracer(status))) return 0;
    snprintf(name, size, "?"); /* Android's /proc hides other users' processes */
    snprintf(path, sizeof path, "/proc/%ld/comm", pid);
    if ((f = fopen(path, "r"))) {
        if (fgets(comm, sizeof comm, f)) {
            comm[strcspn(comm, "\n")] = 0;
            if (comm[0]) snprintf(name, size, "%s", comm);
        }
        fclose(f);
    }
    return 1;
#elif defined(__FreeBSD__)
    struct kinfo_proc proc;
    size_t got = sizeof proc;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)getpid()};
    if (sysctl(mib, 4, &proc, &got, NULL, 0) || got != sizeof proc || proc.ki_tracer <= 0) return 0;
    snprintf(name, size, "?");
    mib[3] = proc.ki_tracer;
    got = sizeof proc;
    if (!sysctl(mib, 4, &proc, &got, NULL, 0) && got == sizeof proc && proc.ki_comm[0])
        snprintf(name, size, "%s", proc.ki_comm);
    return 1;
#elif defined(__APPLE__)
    struct kinfo_proc proc = {0};
    size_t got = sizeof proc;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)getpid()};
    if (sysctl(mib, 4, &proc, &got, NULL, 0) || !(proc.kp_proc.p_flag & P_TRACED)) return 0;
    snprintf(name, size, "?");
    return 1;
#elif defined(_WIN32)
    if (!IsDebuggerPresent()) return 0;
    snprintf(name, size, "?");
    return 1;
#else
    (void)name;
    (void)size;
    return 0;
#endif
}
SnxU64 snx_thread_clock(void) {
#if defined(_WIN32)
    HANDLE h = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
        return 0;
    return (SnxU64)(uintptr_t)h;
#elif defined(__APPLE__)
    return (SnxU64)pthread_mach_thread_np(pthread_self()); /* the library's own right: nothing to let go of */
#else
    clockid_t id;
    SnxU64 clock = 0;
    if (pthread_getcpuclockid(pthread_self(), &id)) return 0;
    memcpy(&clock, &id, sizeof id); /* a thread's clock is never CLOCK_REALTIME, 0 */
    return clock;
#endif
}
int snx_thread_cpu_ns(SnxU64 clock, SnxU64* ns) {
#if defined(_WIN32)
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes((HANDLE)(uintptr_t)clock, &created, &exited, &kernel, &user)) return -1;
    *ns = (((SnxU64)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) +
           ((SnxU64)user.dwHighDateTime << 32 | user.dwLowDateTime)) *
          100; /* 100 ns ticks */
    return 0;
#elif defined(__APPLE__)
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (thread_info((thread_act_t)clock, THREAD_BASIC_INFO, (thread_info_t)&info, &count) != KERN_SUCCESS) return -1;
    *ns = ((SnxU64)info.user_time.seconds + (SnxU64)info.system_time.seconds) * 1000000000u +
          ((SnxU64)info.user_time.microseconds + (SnxU64)info.system_time.microseconds) * 1000u;
    return 0;
#else
    clockid_t id;
    struct timespec t;
    memcpy(&id, &clock, sizeof id);
    if (clock_gettime(id, &t)) return -1;
    *ns = (SnxU64)t.tv_sec * 1000000000u + (SnxU64)t.tv_nsec;
    return 0;
#endif
}
void snx_thread_clock_close(SnxU64 clock) {
#if defined(_WIN32)
    if (clock) CloseHandle((HANDLE)(uintptr_t)clock);
#else
    (void)clock;
#endif
}
void snx_thread_name(const char* name) {
    char cut[64]; /* Apple refuses a name past 63 bytes, where the others cut it */
    snprintf(cut, sizeof cut, "%s", name);
#if defined(_WIN32)
    typedef HRESULT(WINAPI * DescribeFn)(HANDLE, PCWSTR);
    DescribeFn describe =
        (DescribeFn)(void (*)(void))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription");
    wchar_t wide[64];
    if (describe && MultiByteToWideChar(CP_UTF8, 0, cut, -1, wide, 64)) describe(GetCurrentThread(), wide);
#elif defined(__APPLE__)
    pthread_setname_np(cut); /* the calling thread's alone */
#elif defined(__linux__)
    prctl(PR_SET_NAME, (unsigned long)cut, 0, 0, 0);
#elif defined(__FreeBSD__)
    pthread_set_name_np(pthread_self(), cut);
#endif
}
#if defined(__APPLE__)
static const char* qos_name(qos_class_t c) {
    switch (c) {
        case QOS_CLASS_USER_INTERACTIVE: return "user-interactive";
        case QOS_CLASS_USER_INITIATED: return "user-initiated";
        case QOS_CLASS_DEFAULT: return "default";
        case QOS_CLASS_UTILITY: return "utility";
        case QOS_CLASS_BACKGROUND: return "background";
        default: return "unspecified";
    }
}
#endif
int snx_thread_priority_low(char* why, size_t size) {
#if defined(_WIN32)
    char reason[256];
    if (SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL)) return 0;
    snx_os_error(reason, sizeof reason);
    snprintf(why, size, "SetThreadPriority below normal: %s; it runs at thread priority %d (normal is 0)", reason,
             GetThreadPriority(GetCurrentThread()));
    return -1;
#elif defined(__APPLE__)
    int e = pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    if (!e) return 0;
    snprintf(why, size, "pthread_set_qos_class_self_np utility: %s; it runs in the %s QoS class", strerror(e),
             qos_name(qos_class_self()));
    return -1;
#elif defined(__linux__)
    /* a thread's id given to them, getpriority and setpriority take that thread alone (Android's too); the process's
       id would name its first thread */
    id_t tid = (id_t)syscall(SYS_gettid);
    int now, want;
    errno = 0;
    now = getpriority(PRIO_PROCESS, tid);
    if (now == -1 && errno) {
        snprintf(why, size, "getpriority of thread %u: %s; its nice is not known", (unsigned)tid, strerror(errno));
        return -1;
    }
    want = now + SNX_THREAD_LOW_NICE < 19 ? now + SNX_THREAD_LOW_NICE : 19;
    if (want == now) {
        snprintf(why, size, "nice %d is the lowest there is; it runs at nice %d", now, now);
        return -1;
    }
    if (setpriority(PRIO_PROCESS, tid, want)) {
        snprintf(why, size, "setpriority nice %d: %s; it runs at nice %d", want, strerror(errno), now);
        return -1;
    }
    return 0;
#elif defined(__FreeBSD__)
    /* setpriority's nice is the whole process's here; the idle class is a thread's own */
    struct rtprio idle = {RTP_PRIO_IDLE, RTP_PRIO_MIN}, now = {0, 0};
    const char* in;
    int e;
    if (!rtprio_thread(RTP_SET, 0, &idle)) return 0;
    e = errno;
    in = rtprio_thread(RTP_LOOKUP, 0, &now)           ? "a class rtprio_thread does not say"
         : RTP_PRIO_BASE(now.type) == RTP_PRIO_NORMAL ? "the timeshare class"
         : RTP_PRIO_BASE(now.type) == RTP_PRIO_IDLE   ? "the idle class"
                                                      : "a realtime class";
    snprintf(why, size,
             "rtprio_thread idle %d: %s (root, the idletime group with mac_priority, or "
             "security.bsd.unprivileged_idprio=1 allow it); it runs in %s",
             RTP_PRIO_MIN, strerror(e), in);
    return -1;
#else
    snprintf(why, size, "this system has no lower priority for one thread here; it runs at the process's own");
    return -1;
#endif
}
#define LOW_NICE_NAME_(n) "nice +" #n
#define LOW_NICE_NAME(n)  LOW_NICE_NAME_(n)
const char* snx_thread_priority_low_name(void) {
#if defined(_WIN32)
    return "below normal";
#elif defined(__APPLE__)
    return "the utility QoS class";
#elif defined(__linux__)
    return LOW_NICE_NAME(SNX_THREAD_LOW_NICE);
#elif defined(__FreeBSD__)
    return "the idle class";
#else
    return "none on this system";
#endif
}
int snx_threads(SnxThreadInfo* out, int max) {
    int n = 0;
#if defined(_WIN32)
    /* the system's threads, of every process: those of this one. GetThreadDescription is Windows 10 1607's */
    typedef HRESULT(WINAPI * DescriptionFn)(HANDLE, PWSTR*);
    DescriptionFn description =
        (DescriptionFn)(void (*)(void))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
    DWORD self = GetCurrentProcessId();
    THREADENTRY32 e;
    HANDLE all = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    BOOL more;
    if (all == INVALID_HANDLE_VALUE) return -1;
    e.dwSize = sizeof e;
    for (more = Thread32First(all, &e); more; e.dwSize = sizeof e, more = Thread32Next(all, &e)) {
        FILETIME created, exited, kernel, user;
        HANDLE h;
        if (e.th32OwnerProcessID != self) continue;
        h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, e.th32ThreadID);
        if (!h) continue; /* it ended since the snapshot */
        if (GetThreadTimes(h, &created, &exited, &kernel, &user)) {
            if (n < max) {
                PWSTR wide = NULL;
                /* the start in 100 ns ticks reaches bit 57; an id reused at another start differs in those */
                SnxU64 start = (SnxU64)created.dwHighDateTime << 32 | created.dwLowDateTime;
                out[n].id = start ^ (SnxU64)e.th32ThreadID << 40;
                out[n].cpu_ns = (((SnxU64)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) +
                                 ((SnxU64)user.dwHighDateTime << 32 | user.dwLowDateTime)) *
                                100;
                out[n].name[0] = 0;
                if (description && SUCCEEDED(description(h, &wide)) && wide) {
                    char full[256];
                    size_t len = 0;
                    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, full, sizeof full, NULL, NULL)) len = strlen(full);
                    /* cut to the room between two characters, not inside one */
                    if (len >= sizeof out[n].name) {
                        len = sizeof out[n].name - 1;
                        while (len && (full[len] & 0xC0) == 0x80) len--;
                    }
                    memcpy(out[n].name, full, len);
                    out[n].name[len] = 0;
                    LocalFree(wide);
                }
            }
            n++;
        }
        CloseHandle(h);
    }
    CloseHandle(all);
    return n;
#elif defined(__APPLE__)
    thread_act_array_t list;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) return -1;
    for (mach_msg_type_number_t i = 0; i < count; i++) {
        thread_basic_info_data_t b;
        thread_identifier_info_data_t id;
        mach_msg_type_number_t cb = THREAD_BASIC_INFO_COUNT, ci = THREAD_IDENTIFIER_INFO_COUNT;
        if (thread_info(list[i], THREAD_BASIC_INFO, (thread_info_t)&b, &cb) == KERN_SUCCESS &&
            thread_info(list[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&id, &ci) == KERN_SUCCESS) {
            if (n < max) {
                pthread_t p = pthread_from_mach_thread_np(list[i]);
                out[n].id = id.thread_id; /* a new thread never takes an old one's */
                out[n].cpu_ns = ((SnxU64)b.user_time.seconds + (SnxU64)b.system_time.seconds) * 1000000000u +
                                ((SnxU64)b.user_time.microseconds + (SnxU64)b.system_time.microseconds) * 1000u;
                out[n].name[0] = 0;
                if (p && pthread_getname_np(p, out[n].name, sizeof out[n].name)) out[n].name[0] = 0;
            }
            n++;
        }
        mach_port_deallocate(mach_task_self(), list[i]); /* each port in the list is a reference of ours */
    }
    vm_deallocate(mach_task_self(), (vm_address_t)list, count * sizeof *list);
    return n;
#elif defined(__linux__)
    /* /proc/self/task/<tid>/stat: "tid (name) state ...", the times in ticks at fields 14 and 15 and the start at 22.
       The name can hold spaces and ')', so the fields count from the last ')' */
    long ticks = sysconf(_SC_CLK_TCK);
    DIR* dir = opendir("/proc/self/task");
    struct dirent* e;
    if (!dir) return -1;
    if (ticks <= 0) ticks = 100;
    while ((e = readdir(dir))) {
        char path[64], text[512], *name, *end;
        unsigned long long user, system, start;
        long tid = strtol(e->d_name, &end, 10);
        int fd;
        ssize_t got;
        if (tid <= 0 || *end) continue; /* "." and ".." */
        snprintf(path, sizeof path, "/proc/self/task/%ld/stat", tid);
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue; /* it ended since the folder was read */
        got = read(fd, text, sizeof text - 1);
        close(fd);
        if (got <= 0) continue;
        text[got] = 0;
        name = strchr(text, '(');
        end = strrchr(text, ')');
        if (!name || !end || end < name ||
            sscanf(end + 1,
                   " %*c %*d %*d %*d %*d %*d %*u %*lu %*lu %*lu %*lu %llu %llu %*ld %*ld %*ld %*ld %*ld %*ld %llu",
                   &user, &system, &start) != 3)
            continue;
        if (n < max) {
            size_t len = (size_t)(end - name - 1);
            if (len >= sizeof out[n].name) len = sizeof out[n].name - 1;
            /* a tid is under 2^22 (PID_MAX_LIMIT); the start in ticks keeps a reused one apart */
            out[n].id = (SnxU64)start << 22 | (SnxU64)tid;
            out[n].cpu_ns = (SnxU64)(user + system) * (1000000000u / (SnxU64)ticks);
            memcpy(out[n].name, name + 1, len);
            out[n].name[len] = 0;
        }
        n++;
    }
    closedir(dir);
    return n;
#elif defined(__FreeBSD__)
    /* a kinfo_proc for each thread, with room for some that start between the size's call and the list's; more
       than that asks again */
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID | KERN_PROC_INC_THREAD, (int)getpid()};
    struct kinfo_proc* procs = NULL;
    size_t size = 0;
    for (int tries = 0; !procs; tries++) {
        if (tries == 4 || sysctl(mib, 4, NULL, &size, NULL, 0)) return -1;
        size += 16 * sizeof *procs;
        if (!(procs = malloc(size))) return -1;
        if (sysctl(mib, 4, procs, &size, NULL, 0)) {
            int full = errno == ENOMEM;
            free(procs);
            procs = NULL;
            if (!full) return -1;
        }
    }
    n = (int)(size / sizeof *procs);
    for (int i = 0; i < n && i < max; i++) {
        out[i].id = (SnxU64)procs[i].ki_tid;
        out[i].cpu_ns = (SnxU64)procs[i].ki_runtime * 1000u; /* microseconds */
        snprintf(out[i].name, sizeof out[i].name, "%s%s", procs[i].ki_tdname, procs[i].ki_moretdname);
    }
    free(procs);
    return n;
#else
    (void)out;
    (void)max;
    (void)n;
    return -1;
#endif
}
int snx_memory_available(SnxU64* bytes) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
    size_t left = os_proc_available_memory(); /* 0 in the simulator, which has no limit */
    if (!left) return 0;
    *bytes = left;
    return 1;
#elif defined(__APPLE__)
    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t page = 0;
    mach_port_t host = mach_host_self(); /* each call adds a reference to the port: given back below */
    kern_return_t ok = host_page_size(host, &page);
    if (ok == KERN_SUCCESS) ok = host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&vm, &count);
    mach_port_deallocate(mach_task_self(), host);
    if (ok != KERN_SUCCESS) return 0;
    *bytes = ((SnxU64)vm.free_count + vm.inactive_count + vm.purgeable_count) * page;
    return 1;
#elif defined(__linux__)
    char line[128];
    unsigned long long kib = 0;
    int found = 0;
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    while (!found && fgets(line, sizeof line, f)) found = sscanf(line, "MemAvailable: %llu kB", &kib) == 1;
    fclose(f);
    if (found) *bytes = (SnxU64)kib << 10;
    return found;
#elif defined(_WIN32)
    MEMORYSTATUSEX status;
    status.dwLength = sizeof status;
    if (!GlobalMemoryStatusEx(&status)) return 0;
    *bytes = status.ullAvailPhys;
    return 1;
#elif defined(__FreeBSD__)
    /* free and inactive pages, as macOS counts; ZFS's cache, which the system shrinks under pressure, is wired */
    u_int free_pages = 0, inactive = 0;
    size_t size = sizeof free_pages;
    if (sysctlbyname("vm.stats.vm.v_free_count", &free_pages, &size, NULL, 0) || size != sizeof free_pages) return 0;
    size = sizeof inactive;
    if (sysctlbyname("vm.stats.vm.v_inactive_count", &inactive, &size, NULL, 0) || size != sizeof inactive) return 0;
    *bytes = ((SnxU64)free_pages + inactive) * plat_mem_page();
    return 1;
#else
    (void)bytes;
    return 0;
#endif
}
int snx_psi_some(const char* text, unsigned* avg10, SnxU64* total) {
    const char* line = text;
    unsigned whole = 0, part = 0;
    unsigned long long us = 0;
    while (line && strncmp(line, "some ", 5)) {
        line = strchr(line, '\n');
        if (line) ++line;
    }
    /* the kernel prints each average as %lu.%02lu */
    if (!line || sscanf(line, "some avg10=%u.%2u avg60=%*u.%*u avg300=%*u.%*u total=%llu", &whole, &part, &us) != 3)
        return 0;
    *avg10 = whole * 100u + part;
    *total = us;
    return 1;
}
int snx_psi_short(const char* text, unsigned* avg10) {
    SnxU64 total = 0;
    if (!snx_psi_some(text, avg10, &total)) return -1;
    return *avg10 >= SNX_PSI_SHORT;
}
int snx_memory_pressure(char* words, size_t size) {
    char why[160];
#if defined(__linux__)
    char text[256];
    unsigned avg10 = 0;
    size_t n;
    int is_short;
    FILE* f = fopen("/proc/pressure/memory", "r");
    if (!f) {
        snprintf(words, size, "none this process can read (/proc/pressure/memory: %s)", snx_os_error(why, sizeof why));
        return -1;
    }
    n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    is_short = snx_psi_short(text, &avg10);
    if (is_short < 0) {
        snprintf(words, size, "none this process can read (/proc/pressure/memory has no \"some\" line)");
        return -1;
    }
    snprintf(words, size, "Linux's pressure stall information, some avg10=%u.%02u", avg10 / 100u, avg10 % 100u);
    return is_short;
#elif defined(_WIN32)
    HANDLE low = CreateMemoryResourceNotification(LowMemoryResourceNotification);
    BOOL set = FALSE;
    if (!low || !QueryMemoryResourceNotification(low, &set)) {
        snprintf(words, size, "none this process can read (Windows' low memory notification: %s)",
                 snx_os_error(why, sizeof why));
        if (low) CloseHandle(low);
        return -1;
    }
    CloseHandle(low);
    snprintf(words, size, "Windows' low memory notification is %s", set ? "set" : "clear");
    return set != FALSE;
#elif defined(__APPLE__)
    int level = 0;
    size_t length = sizeof level;
    if (sysctlbyname("kern.memorystatus_vm_pressure_level", &level, &length, NULL, 0) || length != sizeof level) {
        snprintf(words, size, "none this process can read (kern.memorystatus_vm_pressure_level: %s)",
                 snx_os_error(why, sizeof why));
        return -1;
    }
    snprintf(words, size, "the kernel's memory pressure level %d (%s)", level,
             level >= 4   ? "critical"
             : level >= 2 ? "warning"
                          : "normal");
    return level >= 2;
#else
    (void)why;
    snprintf(words, size, "none on this system");
    return -1;
#endif
}
int snx_memory_total(SnxU64* bytes) {
#ifdef _WIN32
    MEMORYSTATUSEX status;
    status.dwLength = sizeof status;
    if (!GlobalMemoryStatusEx(&status) || !status.ullTotalPhys) return 0;
    *bytes = status.ullTotalPhys;
    return 1;
#else
    long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGESIZE);
    if (pages <= 0 || page <= 0) return 0;
    *bytes = (SnxU64)pages * (SnxU64)page;
    return 1;
#endif
}
void snx_vm_flush_code(void* p, size_t size) {
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), p, size);
#elif defined(__APPLE__)
    /* the system's own: iOS links no compiler runtime for __builtin___clear_cache's __clear_cache */
    sys_icache_invalidate(p, size);
#else
    /* nothing on x86-64, whose instruction fetch sees stores; AArch64 cleans the data cache to the point of
       unification and invalidates the instruction cache over the range */
    __builtin___clear_cache((char*)p, (char*)p + size);
#endif
}
int snx_terminal(void) {
#ifdef _WIN32
    return _fileno(stdin) >= 0 && _fileno(stdout) >= 0 && _isatty(_fileno(stdin)) && _isatty(_fileno(stdout));
#else
    return isatty(fileno(stdin)) && isatty(fileno(stdout));
#endif
}
void snx_tool_console(void) {
#ifdef _WIN32
    /* The SDL runtime is a GUI executable. Reattach only for explicit CLI work,
       preserving any redirected handles supplied by its caller. */
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (_fileno(stdout) < 0) (void)freopen("CONOUT$", "w", stdout);
        if (_fileno(stderr) < 0) (void)freopen("CONOUT$", "w", stderr);
    }
#endif
}
/* file's path from the root, its folder resolved and its own name kept: a program found by name runs under that name,
   which can say what it does (scarletnx-3ds, a link to scarletnx) */
static char* tool_path(const char* file) {
    const char* name = file + strlen(file);
    while (name > file && !snx_is_separator(name[-1])) --name;
    char* dir = snx_dirname(file);
    char* absolute = snx_absolute(dir);
    free(dir);
    if (!absolute) return NULL;
    char* path = snx_path_join(absolute, name);
    free(absolute);
    return path;
}
char* snx_find_tool(const char* name) {
    const char *p, *end;
    char *dir, *file, *absolute;
    size_t n;
    int has_sep = 0;
    for (p = name; *p; ++p)
        if (snx_is_separator(*p)) has_sep = 1;
    if (has_sep) return snx_is_file(name) ? tool_path(name) : NULL;
    const char* env = getenv("PATH");
    if (!env) return NULL;
    p = env;
    do {
#ifdef _WIN32
        end = strchr(p, ';');
#else
        end = strchr(p, ':');
#endif
        n = end ? (size_t)(end - p) : strlen(p);
        dir = (char*)snx_alloc(n + 1);
        memcpy(dir, p, n);
        dir[n] = 0;
        file = snx_path_join(n ? dir : ".", name);
        free(dir);
#ifdef _WIN32
        if (!snx_is_file(file)) {
            char* next = snx_format("%s.exe", file);
            free(file);
            file = next;
        }
        if (snx_is_file(file)) {
            absolute = tool_path(file);
            free(file);
            return absolute;
        }
#else
        if (snx_is_file(file) && access(file, X_OK) == 0) {
            absolute = tool_path(file);
            free(file);
            return absolute;
        }
#endif
        free(file);
        p = end ? end + 1 : NULL;
    } while (p);
    return NULL;
}

/* limit keeps the last limit bytes (0 keeps all): a tool's error report is at its end, its listing is not */
static void append_output(char** text, size_t* used, const char* buf, size_t n, size_t limit) {
    size_t drop;
    if (limit && n > limit) {
        buf += n - limit;
        n = limit;
    }
    if (limit && *used + n > limit) {
        drop = *used + n - limit;
        memmove(*text, *text + drop, *used - drop);
        *used -= drop;
    }
    *text = (char*)snx_realloc(*text, *used + n + 1);
    memcpy(*text + *used, buf, n);
    *used += n;
    (*text)[*used] = 0;
}
#ifdef _WIN32
static wchar_t* command_line(const char* const* argv) {
    size_t cap = 1, slashes, used = 0;
    for (size_t i = 0; argv[i]; ++i) cap += 2 * strlen(argv[i]) + 4;
    char* out = (char*)snx_alloc(cap);
    for (size_t i = 0; argv[i]; ++i) {
        if (i) out[used++] = ' ';
        out[used++] = '"';
        slashes = 0;
        for (size_t n = 0; argv[i][n]; ++n) {
            if (argv[i][n] == '\\') {
                ++slashes;
                continue;
            }
            for (size_t k = 0; k < slashes * (argv[i][n] == '"' ? 2 : 1); ++k) out[used++] = '\\';
            slashes = 0;
            if (argv[i][n] == '"') out[used++] = '\\';
            out[used++] = argv[i][n];
        }
        for (size_t k = 0; k < slashes * 2; ++k) out[used++] = '\\';
        out[used++] = '"';
    }
    out[used] = 0;
    wchar_t* w = wide(out);
    free(out);
    return w;
}
static wchar_t* child_environment(const char* home) {
    wchar_t *env = GetEnvironmentStringsW(), *out;
    size_t cap = 1, used = 0, n;
    if (!env) return NULL;
    wchar_t* wh = wide(home);
    if (!wh) {
        FreeEnvironmentStringsW(env);
        return NULL;
    }
    for (wchar_t* p = env; *p; p += wcslen(p) + 1) cap += wcslen(p) + 1;
    cap += 2 * wcslen(wh) + 32;
    out = (wchar_t*)snx_alloc(cap * sizeof(*out));
    for (wchar_t* p = env; *p; p += wcslen(p) + 1) {
        if (!_wcsnicmp(p, L"HOME=", 5) || !_wcsnicmp(p, L"USERPROFILE=", 12) || !_wcsnicmp(p, L"XDG_CONFIG_HOME=", 16))
            continue;
        n = wcslen(p) + 1;
        memcpy(out + used, p, n * sizeof(*out));
        used += n;
    }
    wcscpy(out + used, L"HOME=");
    wcscat(out + used, wh);
    used += wcslen(out + used) + 1;
    wcscpy(out + used, L"USERPROFILE=");
    wcscat(out + used, wh);
    used += wcslen(out + used) + 1;
    out[used] = 0;
    free(wh);
    FreeEnvironmentStringsW(env);
    return out;
}
#endif
/* tick(data) once a second has gone by since the last one */
static void tick_due(double* ticked, void (*tick)(void*), void* data) {
    if (snx_seconds() - *ticked < 1) return;
    *ticked = snx_seconds();
    tick(data);
}
static int run_ticking(const char* const* argv, const char* cwd, const char* home, char** output, void (*tick)(void*),
                       void* data, size_t limit);
int snx_run(const char* const* argv, const char* cwd, const char* home, char** output) {
    return snx_run_ticking(argv, cwd, home, output, NULL, NULL);
}
int snx_run_all(const char* const* argv, const char* cwd, const char* home, char** output) {
    return run_ticking(argv, cwd, home, output, NULL, NULL, 0);
}
int snx_run_ticking(const char* const* argv, const char* cwd, const char* home, char** output, void (*tick)(void*),
                    void* data) {
    return run_ticking(argv, cwd, home, output, tick, data, 1048576);
}
static int run_ticking(const char* const* argv, const char* cwd, const char* home, char** output, void (*tick)(void*),
                       void* data, size_t limit) {
    char buf[8192];
    size_t used = 0;
    double ticked = snx_seconds();
#ifdef _WIN32
    PROCESS_INFORMATION pi;
    HANDLE rd = NULL, wr = NULL, input = INVALID_HANDLE_VALUE, handles[2];
    SIZE_T attr_size = 0;
    wchar_t *cmd = command_line(argv), *dir = cwd ? wide(cwd) : NULL;
    wchar_t* env = home ? child_environment(home) : NULL;
    DWORD got, code = 1;
    if (!output) {
        char* captured = NULL;
        free(cmd);
        free(dir);
        free(env);
        int result = run_ticking(argv, cwd, home, &captured, tick, data, limit);
        if (captured) {
            fputs(captured, stdout);
            free(captured);
        }
        return result;
    }
    STARTUPINFOEXW si = {0};
    si.StartupInfo.cb = sizeof(si);
    SECURITY_ATTRIBUTES sa = {0};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (output) {
        *output = snx_strdup("");
        if (!CreatePipe(&rd, &wr, &sa, 0)) {
            free(cmd);
            free(dir);
            free(env);
            return 1;
        }
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
        InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
        si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)snx_alloc(attr_size);
        if (input == INVALID_HANDLE_VALUE || !InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size)) {
            if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
            CloseHandle(rd);
            CloseHandle(wr);
            free(si.lpAttributeList);
            free(cmd);
            free(dir);
            free(env);
            return 1;
        }
        handles[0] = input;
        handles[1] = wr;
        if (!UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                       sizeof(handles), NULL, NULL)) {
            DeleteProcThreadAttributeList(si.lpAttributeList);
            free(si.lpAttributeList);
            CloseHandle(input);
            CloseHandle(rd);
            CloseHandle(wr);
            free(cmd);
            free(dir);
            free(env);
            return 1;
        }
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = input;
        si.StartupInfo.hStdOutput = wr;
        si.StartupInfo.hStdError = wr;
    }
    int ok = cmd && (!cwd || dir) && (!home || env) &&
         CreateProcessW(NULL, cmd, NULL, NULL, output != NULL,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT, env, dir,
                        &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(si.lpAttributeList);
    free(si.lpAttributeList);
    CloseHandle(input);
    free(cmd);
    free(dir);
    free(env);
    if (wr) CloseHandle(wr);
    if (!ok) {
        if (rd) CloseHandle(rd);
        return 127;
    }
    if (output) {
        for (;;) {
            DWORD ready = 0;
            if (tick) { /* a read waits only for what is there, so the tick keeps its second */
                tick_due(&ticked, tick, data);
                if (!PeekNamedPipe(rd, NULL, 0, NULL, &ready, NULL)) break; /* every writer gone */
                if (!ready) {
                    Sleep(50);
                    continue;
                }
            }
            if (!ReadFile(rd, buf, ready && ready < sizeof(buf) ? ready : sizeof(buf), &got, NULL) || !got) break;
            append_output(output, &used, buf, got, limit);
        }
        CloseHandle(rd);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
#else
    int fds[2] = {-1, -1}, status;
    pid_t waited;
    ssize_t got;
    if (output) {
        *output = snx_strdup("");
        if (pipe(fds)) return 1;
        fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    }
    pid_t pid = fork();
    if (!pid) {
        if (output) {
            close(fds[0]);
            dup2(fds[1], 1);
            dup2(fds[1], 2);
            close(fds[1]);
        }
        if (cwd && chdir(cwd)) _exit(126);
        if (home && (setenv("HOME", home, 1) || setenv("USERPROFILE", home, 1) || unsetenv("XDG_CONFIG_HOME")))
            _exit(126);
        execvp(argv[0], (char* const*)argv);
        _exit(127);
    }
    if (output) close(fds[1]);
    if (pid < 0) {
        if (output) close(fds[0]);
        return 1;
    }
    if (output) {
        for (;;) {
            if (tick) { /* a read waits only for what is there, so the tick keeps its second */
                struct pollfd ready;
                int ms = (int)((ticked + 1 - snx_seconds()) * 1000);
                ready.fd = fds[0];
                ready.events = POLLIN;
                ready.revents = 0;
                int any = poll(&ready, 1, ms > 0 ? ms : 0);
                tick_due(&ticked, tick, data);
                if (!any || (any < 0 && errno == EINTR)) continue;
            }
            got = read(fds[0], buf, sizeof(buf));
            if (got > 0)
                append_output(output, &used, buf, (size_t)got, limit);
            else if (got == 0 || errno != EINTR)
                break;
        }
        close(fds[0]);
    }
    for (;;) {
        waited = waitpid(pid, &status, tick && !output ? WNOHANG : 0);
        if (waited < 0 && errno == EINTR) continue;
        if (waited) break;
        poll(NULL, 0, 50); /* no output to wait on: the child's end is looked for, the tick kept */
        tick_due(&ticked, tick, data);
    }
    return waited < 0 ? 1 : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
#endif
}
/* a control.nacp's titles, one a language: its name (0x200 bytes), then its publisher. A NACP keeps 16 of them at its
   start as they are, or, when its TitleCompression byte is 1, a u16 size there and the titles in raw deflate, up to 32
   languages (firmware 20.0.0's) */
#define SNX_NACP_TITLE             0x300
#define SNX_NACP_TITLES            16
#define SNX_NACP_TITLES_PACKED     32
#define SNX_NACP_TITLE_COMPRESSION 0x3215
int snx_utf8_valid(const char* text, size_t n, char* hex, size_t hex_size) {
    const unsigned char* s = (const unsigned char*)text;
    size_t i = 0, want, k, at;
    unsigned long c;
    if (hex_size) hex[0] = 0;
    while (i < n) {
        if (s[i] < 0x80) {
            ++i;
            continue;
        }
        want = (s[i] & 0xe0) == 0xc0 ? 2 : (s[i] & 0xf0) == 0xe0 ? 3 : (s[i] & 0xf8) == 0xf0 ? 4 : 0;
        if (!want || want > n - i) break;
        c = s[i] & (0x7fu >> want);
        for (k = 1; k < want && (s[i + k] & 0xc0) == 0x80; ++k) c = c << 6 | (s[i + k] & 0x3fu);
        if (k < want ||
            c < (want == 2   ? 0x80ul
                 : want == 3 ? 0x800ul
                             : 0x10000ul) ||
            c > 0x10fffful || (c >= 0xd800ul && c <= 0xdffful))
            break;
        i += want;
    }
    if (i == n) return 1;
    for (i = 0, at = 0; i < n && i < 16 && at + 4 < hex_size; ++i, at = strlen(hex))
        snprintf(hex + at, hex_size - at, "%s%02x", i ? " " : "", (unsigned)s[i]);
    if (n > 16 && hex_size && strlen(hex) + 4 < hex_size) strcat(hex, "...");
    return 0;
}
/* one language's name (title, 0x200 bytes at most) into name: control characters left out, spaces trimmed, a sequence
   that name_size cuts dropped. 1 with a name, else 0 and "": its bytes in hex into bad (when given) when they are not
   UTF-8, which a name never shows as */
static int title_name(const unsigned char* title, char* name, size_t name_size, char* bad, size_t bad_size) {
    size_t i, n = 0;
    for (i = 0; i < 0x200 && title[i] && n + 1 < name_size; ++i)
        if (title[i] >= 0x20 && title[i] != 0x7f) name[n++] = (char)title[i];
    while (n && name[n - 1] == ' ') --n;
    /* a name cut in a UTF-8 sequence loses the sequence */
    i = n;
    while (i && ((unsigned char)name[i - 1] & 0xc0) == 0x80) --i;
    if (i && ((unsigned char)name[i - 1] & 0x80)) {
        unsigned char lead = (unsigned char)name[i - 1];
        size_t want = (lead & 0xe0) == 0xc0 ? 2 : (lead & 0xf0) == 0xe0 ? 3 : (lead & 0xf8) == 0xf0 ? 4 : 1;
        if (n - (i - 1) < want) n = i - 1;
    }
    name[n] = 0;
    if (!snx_utf8_valid(name, n, bad, bad ? bad_size : 0)) {
        name[0] = 0;
        return 0;
    }
    for (i = 0; name[i] == ' '; ++i) {}
    if (i) memmove(name, name + i, n - i + 1);
    return name[0] != 0;
}
int snx_nacp_name_why(const unsigned char* nacp, size_t size, char* name, size_t name_size, char* why,
                      size_t why_size) {
    unsigned char* unpacked = NULL;
    const unsigned char* titles = nacp;
    struct libdeflate_decompressor* inflater;
    size_t count = SNX_NACP_TITLES, language, packed, got = 0, bad_language = 0;
    char bad[64] = {0};
    int format = nacp && size > SNX_NACP_TITLE_COMPRESSION ? nacp[SNX_NACP_TITLE_COMPRESSION] : 0;
    if (why_size) why[0] = 0;
    if (!name_size) return 0;
    name[0] = 0;
    if (!nacp || size < SNX_NACP_TITLE) {
        if (why_size) snprintf(why, why_size, "it is %zu bytes, short of one language's title (768)", nacp ? size : 0);
        return 0;
    }
    if (format > 1) {
        if (why_size)
            snprintf(why, why_size,
                     "its TitleCompression is %d, where 0 (its titles as they are) and 1 (in deflate) are "
                     "the formats known",
                     format);
        return 0;
    }
    if (format == 1) { /* the titles region's u16 size, then the deflate */
        packed = snx_ld_le16(nacp);
        unpacked = (unsigned char*)malloc(SNX_NACP_TITLES_PACKED * SNX_NACP_TITLE);
        inflater = unpacked && packed <= SNX_NACP_TITLES * SNX_NACP_TITLE - 2 ? libdeflate_alloc_decompressor() : NULL;
        if (!inflater ||
            libdeflate_deflate_decompress(inflater, nacp + 2, packed, unpacked, SNX_NACP_TITLES_PACKED * SNX_NACP_TITLE,
                                          &got) != LIBDEFLATE_SUCCESS) {
            if (why_size)
                snprintf(why, why_size,
                         "its titles are in deflate (TitleCompression 1), %zu bytes that do not inflate to "
                         "%d languages' titles or fewer",
                         packed, SNX_NACP_TITLES_PACKED);
            libdeflate_free_decompressor(inflater);
            free(unpacked);
            return 0;
        }
        libdeflate_free_decompressor(inflater);
        titles = unpacked;
        size = got;
        count = SNX_NACP_TITLES_PACKED;
    }
    for (language = 0; language < count && (language + 1) * SNX_NACP_TITLE <= size && !name[0]; ++language)
        if (!title_name(titles + language * SNX_NACP_TITLE, name, name_size, bad_language ? NULL : bad, sizeof bad) &&
            bad[0] && !bad_language)
            bad_language = language + 1;
    if (!name[0] && why_size) {
        if (bad_language)
            snprintf(why, why_size, "the name of its language %zu is not UTF-8 (%s), and no other language has one",
                     bad_language - 1, bad);
        else
            snprintf(why, why_size, "it names the game in none of its %zu languages", language);
    }
    free(unpacked);
    return name[0] != 0;
}
int snx_nacp_name(const unsigned char* nacp, size_t size, char* name, size_t name_size) {
    return snx_nacp_name_why(nacp, size, name, name_size, NULL, 0);
}
char* snx_sfo_string(const unsigned char* sfo, size_t size, const char* key) {
    /* "\0PSF", version, the key table's offset, the data table's, the entries (16 bytes each from 20) */
    if (size < 20 || snx_ld_le32(sfo) != 0x46535000u) return NULL;
    uint32_t keys = snx_ld_le32(sfo + 8), data = snx_ld_le32(sfo + 12), count = snx_ld_le32(sfo + 16);
    if (keys > size || data > size || count > (size - 20) / 16) return NULL;
    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char* e = sfo + 20 + i * 16;
        uint32_t name = keys + snx_ld_le16(e), format = snx_ld_le16(e + 2), length = snx_ld_le32(e + 4),
                 at = data + snx_ld_le32(e + 12);
        if (name >= size || memchr(sfo + name, 0, size - name) == NULL || strcmp((const char*)sfo + name, key))
            continue;
        if (format != 0x0204 || at > size || length > size - at || !length) return NULL;
        char* value = malloc(length + 1);
        if (!value) return NULL;
        memcpy(value, sfo + at, length);
        value[length] = 0;
        return value;
    }
    return NULL;
}
SnxU64 snx_npdm_title(const unsigned char* npdm, size_t size) {
    if (!npdm || size < 0x80 || memcmp(npdm, "META", 4)) return SNX_NPDM_DEFAULT_TITLE;
    SnxU64 aci = snx_ld_le32(npdm + 0x70);
    if (!aci || aci > size || size - aci < 0x18 || memcmp(npdm + aci, "ACI0", 4)) return SNX_NPDM_DEFAULT_TITLE;
    return snx_ld_le64(npdm + aci + 0x10);
}
int snx_title_text(SnxU64 id, char* out) {
    out[0] = 0;
    if (!id || id == SNX_NPDM_DEFAULT_TITLE) return 0;
    sprintf(out, "%016llX", (unsigned long long)id);
    return 1;
}
int snx_title_parse(const char* text, SnxU64* id) {
    SnxU64 value = 0;
    int c;
    for (int i = 0; i < 16; ++i) {
        c = (unsigned char)text[i];
        if (!snx_is_hex(c) || snx_is_lower(c)) return 0; /* as snx_title_text writes it */
        value = value << 4 | (SnxU64)(c <= '9' ? c - '0' : c - 'A' + 10);
    }
    if (text[16]) return 0;
    *id = value;
    return 1;
}
/* 1 when id is the game's update or the patch of one of its other programs: an ID its entry gives a version */
static int update_of(SnxU64 game, SnxU64 id) {
    return snx_addon_kind(game, id) == SNX_ADDON_UPDATE ||
           (snx_addon_kind(game, id) == SNX_ADDON_PROGRAM && id - game > SNX_PROGRAMS_MAX);
}
int snx_addon_name(const char* base, const char* name, SnxU64* id) {
    size_t n = strlen(base);
    SnxU64 game;
    if (!snx_title_parse(base, &game) || strncmp(name, base, n) || name[n] != '_') return 0;
    return snx_addon_entry_id(game, name + n + 1, id);
}
int snx_addon_entry_id(SnxU64 game, const char* entry, SnxU64* id) {
    char text[17];
    SnxU64 value;
    size_t digits;
    if (strlen(entry) < 16) return 0;
    memcpy(text, entry, 16);
    text[16] = 0;
    if (!snx_title_parse(text, &value) || snx_addon_kind(game, value) == SNX_ADDON_NONE) return 0;
    if (entry[16]) { /* an update's version (its programs' too): v and up to ten digits */
        digits = strspn(entry + 17, "0123456789");
        if (entry[16] != 'v' || !digits || digits > 10 || entry[17 + digits] || !update_of(game, value)) return 0;
    }
    if (id) *id = value;
    return 1;
}
void snx_addon_entry(SnxU64 game, SnxU64 id, SnxU64 version, char entry[32]) {
    snx_title_text(id, entry);
    if (update_of(game, id) && version != SNX_NO_VERSION)
        sprintf(entry + 16, "v%llu", (unsigned long long)(version & 0xFFFFFFFFu));
}
SnxU64 snx_addon_version(const char* entry) {
    return strlen(entry) > 17 && entry[16] == 'v' ? (SnxU64)strtoull(entry + 17, NULL, 10) : SNX_NO_VERSION;
}
int snx_addon_kind(SnxU64 game, SnxU64 id) {
    if (!id || id == game) return SNX_ADDON_NONE;
    /* a 3DS application's (0x00040000 its ID's high half): its update has its low half under 0x0004000E, as Azahar's
       am makes an update's ID, and its add-on content its low half's 28 bits under 0x0004008C, as Azahar's game list
       finds a game's DLC; nothing else is its */
    if (game >> 32 == 0x00040000u) {
        if (id >> 32 == 0x0004000Eu && (id & 0xFFFFFFFFu) == (game & 0xFFFFFFFFu)) return SNX_ADDON_UPDATE;
        if (id >> 32 == 0x0004008Cu && (id & 0xFFFFFFFFu) == (game & 0x0FFFFFFFu)) return SNX_ADDON_DLC;
        return SNX_ADDON_NONE;
    }
    /* the console's rule: an application's patch is its ID with 0x800 added, its other programs follow its ID and
       their patches the patch's; any other add-on of it is content */
    if (id == (game | 0x800)) return SNX_ADDON_UPDATE;
    return snx_program_index(game, id) > 0 ? SNX_ADDON_PROGRAM : SNX_ADDON_DLC;
}
int snx_addon_own_program(SnxU64 game, SnxU64 id) {
    return game >> 32 != 0x00040000u && snx_addon_kind(game, id) == SNX_ADDON_UPDATE;
}
int snx_program_index(SnxU64 game, SnxU64 id) {
    SnxU64 update = game | 0x800, step;
    if (id == game || id == update) return 0;
    if (id > game && id - game < SNX_PROGRAMS_MAX && id < update) return (int)(id - game);
    step = id - update;
    return id > update && step < SNX_PROGRAMS_MAX ? (int)step : -1;
}
int snx_program_name(const char* game, const char* update, int index, char* out, size_t size) {
    SnxU64 own, id;
    char text[32];
    if (!snx_title_parse(game, &own) || index < 0 || index >= SNX_PROGRAMS_MAX) return 1;
    if (!index)
        return update && update[0] ? snx_path_format(out, size, "%s_%s", game, update)
                                   : snx_path_format(out, size, "%s", game);
    if (update && update[0]) { /* the update's ID and version, then its program's ID with that version */
        if (!snx_addon_entry_id(own, update, &id) || snx_addon_kind(own, id) != SNX_ADDON_UPDATE) return 1;
        snx_addon_entry(own, id + (SnxU64)index, snx_addon_version(update), text);
    } else
        snx_title_text(own + (SnxU64)index, text);
    return snx_path_format(out, size, "%s_%s", game, text);
}
SnxU64 snx_addon_guess(SnxU64 id) {
    /* a patch's ID is its application's with 0x800 (and a program's index); add-on content's commonly starts at its
       application's 0x1000 */
    return id & ~(SnxU64)(id & 0x800 ? 0x8FF : 0x1FFF);
}
SnxU64 snx_nacp_aoc_base(const unsigned char* nacp, size_t size, SnxU64 application) {
    /* AddOnContentBaseId: a u64 at 0x3070; the SDK's default, the application's ID less its low bits plus 0x1000 */
    SnxU64 base = nacp && size >= 0x3078 ? snx_ld_le64(nacp + 0x3070) : 0;
    return base ? base : (application & ~(SnxU64)0x1FFF) + 0x1000;
}
int snx_nacp_version(const unsigned char* nacp, size_t size, char* version, size_t version_size) {
    /* DisplayVersion: 0x10 bytes at 0x3060 */
    size_t n = 0;
    if (version_size) version[0] = 0;
    for (size_t i = 0; nacp && 0x3070 <= size && i < 0x10 && nacp[0x3060 + i] && n + 1 < version_size; ++i)
        if (nacp[0x3060 + i] > 0x20 && nacp[0x3060 + i] < 0x7f) version[n++] = (char)nacp[0x3060 + i];
    if (version_size) version[n] = 0;
    return n != 0;
}
int snx_ends_with_nocase(const char* text, const char* ending) {
    size_t n = strlen(text), e = strlen(ending);
    if (n < e) return 0;
    for (size_t i = 0; i < e; ++i)
        if (snx_to_lower(text[n - e + i]) != snx_to_lower(ending[i])) return 0;
    return 1;
}
