/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The functions of common.h the recompiler's core calls: allocation, the error line, paths, files and folders,
   temporary folders, the clock in seconds, the CPU count, a thread's name and the instruction cache. A file of their
   own, so the recompiler's library can take them without the rest of common.c, which needs other libraries
   (libdeflate for a control.nacp's titles) */

#ifdef __linux__
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700
/* the names beyond POSIX, which older glibc and musl define only with this */
#define _DEFAULT_SOURCE
#endif
#ifdef __APPLE__
/* Darwin's own names beside POSIX's (getmntinfo, pthread_setname_np) */
#define _DARWIN_C_SOURCE
#endif
#include "common/utilities/common.h"
#include "common/utilities/ascii.h"
#include "common/utilities/bits.h"
#include "common/platform/time.h"
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h> /* the rights of a temporary folder (snx_temp_dir) */
#include <wchar.h>
#else
#include <dirent.h>
#include <pthread.h> /* a thread's name (snx_thread_name) */
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/param.h>
#include <sys/mount.h> /* getmntinfo: the mount points a tree removal stops at */
#endif
#ifdef __FreeBSD__
#include <sys/sysctl.h> /* the CPU count (snx_cpu_count) */
#include <pthread_np.h> /* pthread_set_name_np */
#endif
#ifdef __linux__
#include <sys/prctl.h> /* PR_SET_NAME: a thread's name, without _GNU_SOURCE's pthread_setname_np */
#endif
#ifdef __APPLE__
#include <libkern/OSCacheControl.h> /* sys_icache_invalidate */
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
int snx_appendf(char* out, size_t size, int at, const char* fmt, ...) {
    if (!size) return 0;
    if (at < 0) at = 0;
    if ((size_t)at >= size - 1) return (int)(size - 1);
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + at, size - (size_t)at, fmt, ap);
    va_end(ap);
    if (n < 0) {
        out[at] = 0;
        return at;
    }
    if ((size_t)at + (size_t)n < size) return at + n;
    if (size >= 4) memcpy(out + size - 4, "...", 3);
    return (int)(size - 1);
}
void snx_path_native(char* path) {
#ifdef _WIN32
    for (; path && *path; ++path)
        if (*path == '/') *path = '\\';
#else
    (void)path;
#endif
}
/* snx_error's sink, snx_error_sink's: NULL for stderr */
static struct {
    void (*error_sink)(const char* line);
} g_base;
void snx_error_sink(void (*sink)(const char* line)) {
    g_base.error_sink = sink;
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
int snx_error(const char* fmt, ...) {
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    if (g_base.error_sink) {
        vsnprintf(line, sizeof line, fmt, ap);
        g_base.error_sink(line);
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
wchar_t* snx_wide(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    wchar_t* p;
    if (!n) return NULL;
    p = (wchar_t*)snx_alloc((size_t)n * sizeof(*p));
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, p, n);
    return p;
}
char* snx_utf8(const wchar_t* s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    if (!n) return NULL;
    char* p = (char*)snx_alloc((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, s, -1, p, n, NULL, NULL);
    return p;
}
static DWORD attributes(const char* path) {
    wchar_t* p = snx_wide(path);
    DWORD a = p ? GetFileAttributesW(p) : INVALID_FILE_ATTRIBUTES;
    free(p);
    return a;
}
#endif
FILE* snx_fopen(const char* path, const char* mode) {
#ifdef _WIN32
    wchar_t *p = snx_wide(path), *m = snx_wide(mode);
    FILE* f = p && m ? _wfopen(p, m) : NULL;
    free(p);
    free(m);
    return f;
#else
    return fopen(path, mode);
#endif
}
char* snx_absolute(const char* path) {
#ifdef _WIN32
    wchar_t *p = snx_wide(path), *result, *final_path;
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
        out = snx_utf8(final_path);
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
    out = snx_utf8(result);
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
/* the reason a folder is not made stays the system's last (snx_os_error): the look at what is there sets another */
static int mkdir_one(const char* path) {
#ifdef _WIN32
    wchar_t* p = snx_wide(path);
    int ok = p && CreateDirectoryW(p, NULL);
    DWORD error = GetLastError();
    free(p);
    if (ok || snx_is_dir(path)) return 0;
    SetLastError(error);
#else
    int error;
    if (!mkdir(path, 0700)) return 0;
    error = errno;
    if (snx_is_dir(path)) return 0;
    errno = error;
#endif
    return 1;
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
    wchar_t* wp = snx_wide(pattern);
    HANDLE h = wp ? FindFirstFileW(wp, &data) : INVALID_HANDLE_VALUE;
    free(pattern);
    free(wp);
    if (h == INVALID_HANDLE_VALUE) return 1;
    do {
        if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
        char* name = snx_utf8(data.cFileName);
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
    wchar_t* p = snx_wide(path);
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
int snx_write_file(const char* path, const void* data, size_t size) {
    FILE* f = snx_fopen(path, "wb");
    if (!f) return 1;
    int result = fwrite(data, 1, size, f) != size;
    return fclose(f) != 0 || result;
}
char* snx_temp_root(void) {
#ifdef _WIN32
    wchar_t temp[32768];
    return GetTempPathW(32768, temp) ? snx_utf8(temp) : NULL;
#else
    const char* root = getenv("TMPDIR");
    return snx_strdup(root && *root ? root : "/tmp");
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
        wp = snx_wide(candidate);
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
