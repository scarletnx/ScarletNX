/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */

#ifdef __linux__
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700
/* MAP_ANONYMOUS (snx_vm_reserve) is POSIX only since 2024: older glibc and musl define it with this */
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
#include <unistd.h>
#endif

#ifdef __FreeBSD__
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h> /* struct kinfo_proc */
#endif
#ifdef __APPLE__
#include <TargetConditionals.h>
#include <libkern/OSCacheControl.h> /* sys_icache_invalidate */
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <sys/sysctl.h> /* sysctlbyname */
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
    snx_error("%s is %s now: this run takes it as %s (docs/DEPRECATIONS.md)", old, now, now);
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
    DWORD code = GetLastError();
    DWORD n = size ? FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, out,
                                    (DWORD)size, NULL)
                   : 0;
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ' || out[n - 1] == '.')) out[--n] = 0;
    if (!n && size) snprintf(out, size, "Windows error %lu", (unsigned long)code);
#else
    if (size) snprintf(out, size, "%s", strerror(errno));
#endif
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
int snx_lock_file(const char* path, intptr_t* lock) {
#ifdef _WIN32
    wchar_t* p = wide(path);
    OVERLAPPED past = {0}; /* not far: windef.h defines it */
    HANDLE h = p ? CreateFileW(p, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)
                 : INVALID_HANDLE_VALUE;
    free(p);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EACCES;
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
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
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
#endif
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
int snx_remove_tree(const char* path) {
    char **names, *p;
    size_t n;
    int rc = 0;
    int dir = snx_is_dir(path);
    if (dir && !snx_is_link(path)) {
        if (snx_list_dir(path, &names, &n)) return 1;
        for (size_t i = 0; i < n; ++i) {
            p = snx_path_join(path, names[i]);
            if (snx_remove_tree(p)) rc = 1;
            free(p);
        }
        snx_free_strings(names, n);
    }
#ifndef _WIN32
    if (snx_is_link(path)) dir = 0;
#endif
    return remove_one(path, dir) || rc;
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
char* snx_vulkan_driver(const char* program) {
#ifdef __APPLE__
    /* beside the program: the copy a game's runtime is deployed with (snx_deploy_vulkan_driver), which is outside the
       app that carries the library; then the app bundle's Frameworks, ../Frameworks from its Contents/MacOS; then the
       framework an iOS app holds in the bundle itself */
    static const char* const own[] = {"libMoltenVK.dylib", "../Frameworks/libMoltenVK.dylib",
                                      "Frameworks/MoltenVK.framework/MoltenVK"};
    char* dir = snx_dirname(program);
    char* path = NULL;
    for (size_t i = 0; !path && dir && i < sizeof own / sizeof* own; ++i) {
        char* found = snx_path_join(dir, own[i]);
        if (found && snx_is_file(found)) path = found;
        else free(found);
    }
    free(dir);
    return path;
#else
    (void)program;
    return NULL;
#endif
}
int snx_deploy_vulkan_driver(const char* program, const char* folder) {
    char *from = snx_vulkan_driver(program), why[256];
    int rc = 0;
    /* the system's loader serves everywhere else, and a build that carries none (a checkout's) leaves the system's */
    if (!from) return 0;
    char* to = snx_path_join(folder, "libMoltenVK.dylib");
    if (!to) {
        snx_error("cannot name the Vulkan driver's place in %s", folder);
        free(from);
        return 1;
    }
    /* A second name where the disk allows one, so each game does not hold a copy, else a copy. A read-only app image,
       and an app macOS translocated, take no second name of the library they carry, which is why the copy follows a
       link that failed. A name of another app's library is broken rather than written through, which would change that
       app's library and signature too */
    if (snx_is_file(to) && snx_remove_tree(to))
        rc = snx_error("cannot remove the Vulkan driver at %s: %s", to, snx_os_error(why, sizeof why));
    else if (snx_link_file(from, to) && snx_copy_file(from, to))
        rc = snx_error("cannot put the Vulkan driver %s in %s: %s", from, folder, snx_os_error(why, sizeof why));
    free(to);
    free(from);
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
int snx_replace(const char* src, const char* dst) {
#ifdef _WIN32
    wchar_t *a = wide(src), *b = wide(dst);
    int ok = a && b && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    free(a);
    free(b);
    return !ok;
#else
    return rename(src, dst) != 0;
#endif
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
/* $var/scarletnx, else ~/fallback/scarletnx: the XDG spec ignores a relative path */
static char* home_folder(const char* var, const char* fallback) {
    const char* base = var ? getenv(var) : NULL;
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
    return home_folder("XDG_DATA_HOME", ".local/share");
#endif
}
static const char* const kind_names[] = {NULL, "config", "state", "cache"};
char* snx_user_dir(int kind) {
    char *data, *dir;
#ifdef SNX_XDG
    if (kind == SNX_USER_CONFIG) return home_folder("XDG_CONFIG_HOME", ".config");
    if (kind == SNX_USER_STATE) return home_folder("XDG_STATE_HOME", ".local/state");
    if (kind == SNX_USER_CACHE) return home_folder("XDG_CACHE_HOME", ".cache");
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
        printf("scarletnx: %s was left by an older version: this one keeps it in %s (docs/DEPRECATIONS.md)\n", old,
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
                "they are moved there (docs/DEPRECATIONS.md)\n",
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
                    "scarletnx: %s was left by an older version: this one keeps it in %s (docs/DEPRECATIONS.md)\n", old,
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
char* snx_temp_dir(const char* parent) {
    char* path;
#ifdef _WIN32
    wchar_t* wp;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    path = parent ? snx_strdup(parent) : snx_temp_root();
    if (!path) return NULL;
    /* Key material must remain private even when the selected temp parent is shared. */
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)", SDDL_REVISION_1,
                                                              &descriptor, NULL)) {
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
double snx_seconds(void) {
#ifdef _WIN32
    LARGE_INTEGER frequency, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
#endif
}
unsigned snx_cpu_count(void) {
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwNumberOfProcessors ? info.dwNumberOfProcessors : 1;
#elif defined(__linux__) || defined(__APPLE__)
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
unsigned snx_cpu_cores(void) {
#ifdef _WIN32
    DWORD size = 0, at;
    unsigned n = 0;
    char* buf;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &size);
    buf = size ? malloc(size) : NULL;
    if (buf &&
        GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &size))
        for (at = 0; at < size; at += ((PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + at))->Size) n++;
    free(buf);
    return n ? n : snx_cpu_count();
#elif defined(__APPLE__) || defined(__FreeBSD__)
#if defined(__APPLE__)
    const char* name = "hw.physicalcpu";
#else
    const char* name = "kern.smp.cores";
#endif
    int n = 0;
    size_t len = sizeof n;
    return sysctlbyname(name, &n, &len, NULL, 0) == 0 && n > 0 ? (unsigned)n : snx_cpu_count();
#elif defined(__linux__)
    /* each CPU's (package, core) pair in sysfs, counted once */
    enum { MOST = 1024 };
    unsigned long pairs[MOST];
    unsigned n = 0, i, k;
    DIR* d = opendir("/sys/devices/system/cpu");
    struct dirent* e;
    while (d && (e = readdir(d)) && n < MOST) {
        char path[320];
        unsigned long package = 0, core = 0, pair;
        FILE* f;
        int got = 0;
        if (strncmp(e->d_name, "cpu", 3) || e->d_name[3] < '0' || e->d_name[3] > '9') continue;
        snprintf(path, sizeof path, "/sys/devices/system/cpu/%s/topology/physical_package_id", e->d_name);
        if ((f = fopen(path, "r"))) {
            got += fscanf(f, "%lu", &package) == 1;
            fclose(f);
        }
        snprintf(path, sizeof path, "/sys/devices/system/cpu/%s/topology/core_id", e->d_name);
        if ((f = fopen(path, "r"))) {
            got += fscanf(f, "%lu", &core) == 1;
            fclose(f);
        }
        if (got != 2) continue;
        pair = package << 20 | core;
        for (k = 0; k < n && pairs[k] != pair; k++) continue;
        if (k == n) pairs[n++] = pair;
    }
    if (d) closedir(d);
    i = snx_cpu_count();
    return n && n <= i ? n : i;
#else
    return snx_cpu_count();
#endif
}
/* Code arenas (code_loader.c) reserve their address space once and give each page access only when it is used: the
   loader writes into read-write pages and makes them executable before anything jumps there */
void snx_abort_request(int on) { g_common.abort_requested = on; }
int snx_abort_requested(void) { return g_common.abort_requested; }
size_t snx_vm_page_size(void) {
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwPageSize;
#else
    long size = sysconf(_SC_PAGESIZE);
    return size > 0 ? (size_t)size : 4096;
#endif
}
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
    return (SnxU64)resident * snx_vm_page_size();
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS info;
    return GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof info) ? (SnxU64)info.WorkingSetSize : 0;
#elif defined(__FreeBSD__)
    struct kinfo_proc proc;
    size_t size = sizeof proc;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)getpid()};
    if (sysctl(mib, 4, &proc, &size, NULL, 0) || size != sizeof proc) return 0;
    return (SnxU64)proc.ki_rssize * snx_vm_page_size();
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
    if (host_page_size(mach_host_self(), &page) != KERN_SUCCESS ||
        host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &count) != KERN_SUCCESS)
        return 0;
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
#else
    (void)bytes;
    return 0;
#endif
}
void* snx_vm_reserve(size_t size) {
#ifdef _WIN32
    return VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS);
#else
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    void* p;
#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE; /* address space only: no swap is set aside for pages never committed */
#endif
    p = mmap(NULL, size, PROT_NONE, flags, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}
int snx_vm_protect(void* p, size_t size, int access) {
#ifdef _WIN32
    static const DWORD protection[] = {PAGE_NOACCESS, PAGE_READONLY, PAGE_READWRITE, PAGE_EXECUTE_READ};
    DWORD old;
    if (access == SNX_VM_NONE) return VirtualFree(p, size, MEM_DECOMMIT) ? 0 : -1;
    /* reserved pages are committed first: VirtualProtect only changes committed ones */
    if (!VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE)) return -1;
    return VirtualProtect(p, size, protection[access], &old) ? 0 : -1;
#else
    static const int protection[] = {PROT_NONE, PROT_READ, PROT_READ | PROT_WRITE, PROT_READ | PROT_EXEC};
    return mprotect(p, size, protection[access]);
#endif
}
void snx_vm_release(void* p, size_t size) {
#ifdef _WIN32
    (void)size;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, size);
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

/* Keep a bounded diagnostic tail: container tools can emit gigabytes of listings. */
static void append_output(char** text, size_t* used, const char* buf, size_t n) {
    size_t drop;
    if (n > 1048576) {
        buf += n - 1048576;
        n = 1048576;
    }
    if (*used + n > 1048576) {
        drop = *used + n - 1048576;
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
int snx_run(const char* const* argv, const char* cwd, const char* home, char** output) {
    return snx_run_ticking(argv, cwd, home, output, NULL, NULL);
}
int snx_run_ticking(const char* const* argv, const char* cwd, const char* home, char** output, void (*tick)(void*),
                    void* data) {
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
        int result = snx_run_ticking(argv, cwd, home, &captured, tick, data);
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
            append_output(output, &used, buf, got);
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
                append_output(output, &used, buf, (size_t)got);
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
int snx_nacp_name(const unsigned char* nacp, size_t size, char* name, size_t name_size) {
    /* 16 languages of 0x300 bytes, each a name of 0x200 then a publisher; American English first */
    size_t language, i, n = 0;
    const unsigned char* title;
    if (name_size) name[0] = 0;
    for (language = 0; nacp && name_size && language < 16 && (language + 1) * 0x300 <= size && !name[0]; ++language) {
        title = nacp + language * 0x300;
        for (i = 0, n = 0; i < 0x200 && title[i] && n + 1 < name_size; ++i)
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
        for (i = 0; name[i] == ' '; ++i) {}
        if (i) memmove(name, name + i, n - i + 1);
    }
    return name_size && name[0];
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
    if (entry[16]) { /* an update's version: v and up to ten digits */
        digits = strspn(entry + 17, "0123456789");
        if (entry[16] != 'v' || !digits || digits > 10 || entry[17 + digits] ||
            snx_addon_kind(game, value) != SNX_ADDON_UPDATE)
            return 0;
    }
    if (id) *id = value;
    return 1;
}
void snx_addon_entry(SnxU64 game, SnxU64 id, SnxU64 version, char entry[32]) {
    snx_title_text(id, entry);
    if (snx_addon_kind(game, id) == SNX_ADDON_UPDATE && version != SNX_NO_VERSION)
        sprintf(entry + 16, "v%llu", (unsigned long long)(version & 0xFFFFFFFFu));
}
SnxU64 snx_addon_version(const char* entry) {
    return strlen(entry) > 17 && entry[16] == 'v' ? (SnxU64)strtoull(entry + 17, NULL, 10) : SNX_NO_VERSION;
}
int snx_addon_kind(SnxU64 game, SnxU64 id) {
    /* the console's rule: an application's patch is its ID with 0x800 added; any other add-on of it is content */
    if (!id || id == game) return SNX_ADDON_NONE;
    return id == (game | 0x800) ? SNX_ADDON_UPDATE : SNX_ADDON_DLC;
}
SnxU64 snx_addon_guess(SnxU64 id) {
    /* a patch's ID is its application's with 0x800; add-on content's commonly starts at its application's 0x1000 */
    return id & ~(SnxU64)(id & 0x800 ? 0x800 : 0x1FFF);
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
#ifdef _WIN32
void* snx_library_open(const char* path, char* error, size_t error_size) {
    wchar_t* w = wide(path);
    HMODULE library = w ? LoadLibraryW(w) : NULL;
    if (!library && error_size)
        snprintf(error, error_size, "cannot load %s: error %lu", path, (unsigned long)GetLastError());
    free(w);
    return (void*)library;
}
void* snx_library_symbol(void* library, const char* name) {
    return (void*)(uintptr_t)GetProcAddress((HMODULE)library, name);
}
void snx_library_close(void* library) { FreeLibrary((HMODULE)library); }
#else
void* snx_library_open(const char* path, char* error, size_t error_size) {
    void* library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library && error_size) snprintf(error, error_size, "cannot load %s: %s", path, dlerror());
    return library;
}
void* snx_library_symbol(void* library, const char* name) { return dlsym(library, name); }
void snx_library_close(void* library) { dlclose(library); }
#endif
