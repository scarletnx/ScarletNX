/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700
/* MAP_ANONYMOUS (snx_vm_reserve) is POSIX only since 2024: older glibc and musl define it with this */
#define _DEFAULT_SOURCE
#ifdef __APPLE__
/* the POSIX names above would hide Darwin's own (the sysctl and dyld types) */
#define _DARWIN_C_SOURCE
#endif
#include "common/utilities/common.h"
#include <ctype.h>
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
#else
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
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
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
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
    int n;
    char* p;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return snx_strdup("");
    p = (char*)snx_alloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(p, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return p;
}
int snx_error(const char* fmt, ...) {
    va_list ap;
    fputs("scarletnx: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    return 1;
}
static int separator(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}
char* snx_path_join(const char* a, const char* b) {
    size_t n = strlen(a), m = strlen(b);
    char* p = (char*)snx_alloc(n + m + 2);
    memcpy(p, a, n);
    if (n && !separator(a[n - 1])) p[n++] = '/';
    memcpy(p + n, b, m + 1);
    return p;
}
char* snx_dirname(const char* path) {
    char* p = snx_strdup(path);
    size_t n = strlen(p);
    while (n > 1 && separator(p[n - 1])) p[--n] = 0;
    while (n && !separator(p[n - 1])) --n;
    if (!n) {
        free(p);
        return snx_strdup(".");
    }
    while (n > 1 && separator(p[n - 1])) --n;
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
    char* p;
    if (!n) return NULL;
    p = (char*)snx_alloc((size_t)n);
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
char* snx_absolute(const char* path) {
#ifdef _WIN32
    wchar_t *p = wide(path), *result, *final_path;
    char *out, *parent, *resolved, *leaf, *joined;
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
    while (n > 3 && separator(out[n - 1])) out[--n] = 0;
    parent = snx_dirname(out);
    if (!strcmp(parent, out)) {
        free(out);
        free(parent);
        return NULL;
    }
    resolved = snx_absolute(parent);
    free(parent);
    if (!resolved) {
        free(out);
        return NULL;
    }
    leaf = out;
    for (joined = out; *joined; ++joined)
        if (separator(*joined)) leaf = joined + 1;
    joined = snx_path_join(resolved, leaf);
    free(resolved);
    free(out);
    return joined;
#else
    char *cwd, *out, *resolved, *parent, *base, *result;
    size_t n = 256, len;
    if (path[0] == '/')
        out = snx_strdup(path);
    else {
        for (;;) {
            cwd = (char*)snx_alloc(n);
            if (getcwd(cwd, n)) break;
            free(cwd);
            if (errno != ERANGE || n > (size_t)-1 / 2) return NULL;
            n *= 2;
        }
        out = snx_path_join(cwd, path);
        free(cwd);
    }
    len = strlen(out);
    while (len > 1 && out[len - 1] == '/') out[--len] = 0;
    resolved = realpath(out, NULL);
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
    base = strrchr(out, '/');
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
    return snx_find_tool(argv0);
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
    char* parent;
    int result;
    if (snx_is_dir(path)) return 0;
    parent = snx_dirname(path);
    if (!strcmp(parent, path)) {
        free(parent);
        return 1;
    }
    result = snx_mkdirs(parent);
    free(parent);
    return result || mkdir_one(path);
}
static int compare_names(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }
void snx_free_strings(char** strings, size_t count) {
    size_t i;
    for (i = 0; i < count; ++i) free(strings[i]);
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
        char* name;
        if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
        name = utf8(data.cFileName);
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
    if (n > 1) qsort(list, n, sizeof(*list), compare_names);
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
int snx_remove_tree(const char* path) {
    char **names, *p;
    size_t i, n;
    int rc = 0;
    int dir = snx_is_dir(path);
    if (dir && !snx_is_link(path)) {
        if (snx_list_dir(path, &names, &n)) return 1;
        for (i = 0; i < n; ++i) {
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
    unsigned char* p;
    size_t cap = 8192, n = 0, got;
    if (!f) return NULL;
    p = (unsigned char*)snx_alloc(cap + 1);
    for (;;) {
        got = fread(p + n, 1, cap - n, f);
        n += got;
        if (n < cap) break;
        if (cap > ((size_t)-1 - 1) / 2) {
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
    int result;
    if (!f) return 1;
    result = fwrite(data, 1, size, f) != size;
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
    FILE *in = snx_fopen(src, "rb"), *out;
    size_t n;
    int rc = 0;
    if (!in) return 1;
    /* Avoid truncating the input when recompiling an existing *_data directory. */
    out = snx_fopen(dst, "rb");
    if (out) {
        unsigned char other[65536];
        int same = 1;
        do {
            size_t m;
            n = fread(buf, 1, sizeof(buf), in);
            m = fread(other, 1, sizeof(other), out);
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
int snx_path_within(const char* path, const char* parent) {
    char *a = snx_absolute(path), *b = snx_absolute(parent);
    size_t i = 0, n;
    int same = 1;
    if (!a || !b) {
        free(a);
        free(b);
        return 1;
    }
    n = strlen(b);
    while (n > 1 && separator(b[n - 1])) --n;
    while (i < n) {
#ifdef _WIN32
        if (!(separator(a[i]) && separator(b[i])) && tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            same = 0;
#else
        if (a[i] != b[i]) same = 0;
#endif
        if (!same || !a[i]) break;
        ++i;
    }
    same = same && i == n && (!a[i] || separator(a[i]) || (n == 1 && separator(b[0])));
    free(a);
    free(b);
    return same;
}
static int copy_tree(const char* src, const char* dst, unsigned depth) {
    char **names, *a, *b;
    size_t i, n;
    int rc = 0;
    if (depth > 128 || snx_is_link(src) || snx_is_link(dst)) return snx_error("invalid copy tree: %s", src);
    if (!snx_is_dir(src)) return snx_copy_file(src, dst);
    if (snx_mkdirs(dst) || snx_list_dir(src, &names, &n)) return 1;
    for (i = 0; i < n && !rc; ++i) {
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
/* The per-user data folder, an installed build's and a build tree's: $XDG_DATA_HOME/scarletnx or
   ~/.local/share/scarletnx, ~/Library/Application Support/scarletnx on macOS, %APPDATA%\scarletnx on Windows. These are
   the folders SDL_GetPrefPath("scarletnx", ...) gives the GUI; the app's Documents on iOS. NULL when the environment
   names no home */
#if !defined(_WIN32) && !defined(SNX_IOS)
/* $var/scarletnx, else ~/fallback/scarletnx: the XDG spec ignores a relative path */
static char* home_folder(const char* var, const char* fallback) {
    const char* base = var ? getenv(var) : NULL;
    char *parent, *path;
    if (base && base[0] == '/') return snx_path_join(base, "scarletnx");
    base = getenv("HOME");
    if (!base || !base[0]) return NULL;
    parent = snx_path_join(base, fallback);
    path = snx_path_join(parent, "scarletnx");
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
    const char *base = full, *s;
    int app = full && user && same_folder(full, user);
    for (s = full; s && *s; ++s)
        if (separator(*s) && s[1]) base = s + 1;
    if (full && !app)
        app = !strncmp(base, SNX_PORTABLE_DIR, strlen(SNX_PORTABLE_DIR)) &&
              (!base[strlen(SNX_PORTABLE_DIR)] || separator(base[strlen(SNX_PORTABLE_DIR)]));
#ifdef __ANDROID__
    if (full && !app) app = same_folder(full, SNX_ANDROID_DIR);
#endif
    free(user);
    free(full);
    return app;
}
char* snx_app_kind_dir(const char* app, int kind) {
    char* user = snx_user_data_dir();
    int own = user && same_folder(app, user);
    free(user);
    if (own) return snx_user_dir(kind);
    return kind == SNX_USER_DATA ? snx_strdup(app) : snx_path_join(app, kind_names[kind]);
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
        while (separator(*rest)) ++rest;
        moved = *rest ? snx_path_join(root, rest) : snx_strdup(root);
    }
    free(full);
    free(app);
    free(root);
    return moved ? moved : snx_strdup(path);
}
char* snx_menu_dir(const char* folder, int kind) {
    char *full = folder ? snx_absolute(folder) : NULL, *app = full ? app_of(full) : NULL;
    char* dir = app ? snx_app_kind_dir(app, kind) : snx_user_dir(kind);
    free(app);
    free(full);
    return dir;
}
int snx_left_behind(const char* old, const char* now) { return strcmp(old, now) && snx_path_exists(old); }
int snx_launcher_name(const char* name) {
    static const char own[] = "scarletnx";
    size_t i;
    for (i = 0; own[i]; ++i)
        if (tolower((unsigned char)name[i]) != own[i]) return 0;
    return !name[i] || name[i] == '-' || name[i] == '.';
}
const SnxGameFile snx_game_files[SNX_GAME_FILES] = {{".log", SNX_USER_STATE},
                                                    {"threads.txt", SNX_USER_STATE},
                                                    {".ini", SNX_USER_CONFIG},
                                                    {"shader_cache/vk_pipelines.bin", SNX_USER_CACHE}};
char* snx_work_dir(const char* data_dir) {
    char *old = snx_path_join(data_dir, "work"), *work = snx_user_path(old, SNX_USER_CACHE);
    if (snx_left_behind(old, work))
        printf("%s was left by an older version: this one keeps extractions in %s\n", old, work);
    free(old);
    return work;
}
char* snx_data_dir(const char* program_dir) {
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
    char* path = snx_path_join(data, snx_checkout_files[file]);
    char* now;
    if (strcmp(snx_checkout_files[file], "work")) return path;
    now = snx_user_path(path, SNX_USER_CACHE);
    free(path);
    return now;
}
void snx_checkout_left(const char* checkout, const char* data) {
    char *old, *now;
    int i;
    for (i = 0; i < SNX_CHECKOUT_FILES; ++i) {
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
    const char *base = input, *p, *end;
    char* name;
    size_t n = 0;
    for (p = input; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    end = strrchr(base, '.');
    if (!end || end == base) end = base + strlen(base);
    name = (char*)snx_alloc((size_t)(end - base) + 1);
    for (p = base; p < end; ++p)
        if ((unsigned char)*p >= 128 || isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == ' ')
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
char* snx_temp_dir(const char* parent) {
    char* path;
#ifdef _WIN32
    wchar_t temp[32768], *wp;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    SECURITY_ATTRIBUTES security;
    unsigned i;
    if (!parent) {
        if (!GetTempPathW(32768, temp)) return NULL;
        path = utf8(temp);
    } else
        path = snx_strdup(parent);
    /* Key material must remain private even when the selected temp parent is shared. */
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)", SDDL_REVISION_1,
                                                              &descriptor, NULL)) {
        free(path);
        return NULL;
    }
    memset(&security, 0, sizeof(security));
    security.nLength = sizeof(security);
    security.lpSecurityDescriptor = descriptor;
    for (i = 0; i < 10000; ++i) {
        char* name =
            snx_format(".snx-%lu-%lu-%u", (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount(), i);
        char* candidate = snx_path_join(path, name);
        int ok;
        free(name);
        wp = wide(candidate);
        ok = wp && CreateDirectoryW(wp, &security);
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
    if (!parent) {
        parent = getenv("TMPDIR");
        if (!parent || !*parent) parent = "/tmp";
    }
    path = snx_path_join(parent, ".snx-XXXXXX");
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
/* Code arenas (code_loader.c) reserve their address space once and give each page access only when it is used: the
   loader writes into read-write pages and makes them executable before anything jumps there */
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
#else
    /* nothing on x86-64, whose instruction fetch sees stores; AArch64 cleans the data cache to the point of
       unification and invalidates the instruction cache over the range */
    __builtin___clear_cache((char*)p, (char*)p + size);
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
char* snx_find_tool(const char* name) {
    const char *env, *p, *end;
    char *dir, *file, *absolute;
    size_t n;
    int has_sep = 0;
    for (p = name; *p; ++p)
        if (separator(*p)) has_sep = 1;
    if (has_sep) return snx_is_file(name) ? snx_absolute(name) : NULL;
    env = getenv("PATH");
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
            absolute = snx_absolute(file);
            free(file);
            return absolute;
        }
#else
        if (snx_is_file(file) && access(file, X_OK) == 0) {
            absolute = snx_absolute(file);
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
    size_t cap = 1, i, n, k, slashes, used = 0;
    char* out;
    wchar_t* w;
    for (i = 0; argv[i]; ++i) cap += 2 * strlen(argv[i]) + 4;
    out = (char*)snx_alloc(cap);
    for (i = 0; argv[i]; ++i) {
        if (i) out[used++] = ' ';
        out[used++] = '"';
        slashes = 0;
        for (n = 0; argv[i][n]; ++n) {
            if (argv[i][n] == '\\') {
                ++slashes;
                continue;
            }
            for (k = 0; k < slashes * (argv[i][n] == '"' ? 2 : 1); ++k) out[used++] = '\\';
            slashes = 0;
            if (argv[i][n] == '"') out[used++] = '\\';
            out[used++] = argv[i][n];
        }
        for (k = 0; k < slashes * 2; ++k) out[used++] = '\\';
        out[used++] = '"';
    }
    out[used] = 0;
    w = wide(out);
    free(out);
    return w;
}
static wchar_t* child_environment(const char* home) {
    wchar_t *env = GetEnvironmentStringsW(), *p, *out, *wh;
    size_t cap = 1, used = 0, n;
    if (!env) return NULL;
    wh = wide(home);
    if (!wh) {
        FreeEnvironmentStringsW(env);
        return NULL;
    }
    for (p = env; *p; p += wcslen(p) + 1) cap += wcslen(p) + 1;
    cap += 2 * wcslen(wh) + 32;
    out = (wchar_t*)snx_alloc(cap * sizeof(*out));
    for (p = env; *p; p += wcslen(p) + 1) {
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
    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa;
    HANDLE rd = NULL, wr = NULL, input = INVALID_HANDLE_VALUE, handles[2];
    SIZE_T attr_size = 0;
    wchar_t *cmd = command_line(argv), *dir = cwd ? wide(cwd) : NULL;
    wchar_t* env = home ? child_environment(home) : NULL;
    DWORD got, code = 1;
    int ok;
    if (!output) {
        char* captured = NULL;
        int result;
        free(cmd);
        free(dir);
        free(env);
        result = snx_run_ticking(argv, cwd, home, &captured, tick, data);
        if (captured) {
            fputs(captured, stdout);
            free(captured);
        }
        return result;
    }
    memset(&si, 0, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    memset(&sa, 0, sizeof(sa));
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
    ok = cmd && (!cwd || dir) && (!home || env) &&
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
    pid_t pid, waited;
    ssize_t got;
    if (output) {
        *output = snx_strdup("");
        if (pipe(fds)) return 1;
        fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    }
    pid = fork();
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
                int ms = (int)((ticked + 1 - snx_seconds()) * 1000), any;
                ready.fd = fds[0];
                ready.events = POLLIN;
                ready.revents = 0;
                any = poll(&ready, 1, ms > 0 ? ms : 0);
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
static SnxU64 le_get(const unsigned char* p, unsigned n) {
    SnxU64 value = 0;
    while (n--) value = value << 8 | p[n];
    return value;
}
SnxU64 snx_npdm_title(const unsigned char* npdm, size_t size) {
    SnxU64 aci;
    if (!npdm || size < 0x80 || memcmp(npdm, "META", 4)) return SNX_NPDM_DEFAULT_TITLE;
    aci = le_get(npdm + 0x70, 4);
    if (!aci || aci > size || size - aci < 0x18 || memcmp(npdm + aci, "ACI0", 4)) return SNX_NPDM_DEFAULT_TITLE;
    return le_get(npdm + aci + 0x10, 8);
}
int snx_title_text(SnxU64 id, char* out) {
    out[0] = 0;
    if (!id || id == SNX_NPDM_DEFAULT_TITLE) return 0;
    sprintf(out, "%08lX%08lX", (unsigned long)(id >> 32), (unsigned long)(id & 0xFFFFFFFFUL));
    return 1;
}
