/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* glibc's _Fork */
#endif
#include "common/utilities/error_spawn.h"
#include "common/utilities/error_sdl.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* the program and the record, and the record's text and the argument naming it, made when it is shown: no allocation */
static struct {
    char program[4096];
    char record[4096];
    char argument[sizeof SNX_ERROR_SCREEN + 4096];
    char text[sizeof(SnxError) + 256];
} g_error_spawn;

int snx_error_spawn_prepare(const char* program, const char* record, char* why, size_t why_size) {
    if (strlen(program) >= sizeof g_error_spawn.program || strlen(record) >= sizeof g_error_spawn.record) {
        snprintf(why, why_size, "the error panel's program %s or its record %s is past the %zu bytes a path has here",
                 program, record, sizeof g_error_spawn.program - 1);
        return 1;
    }
    memcpy(g_error_spawn.program, program, strlen(program) + 1);
    memcpy(g_error_spawn.record, record, strlen(record) + 1);
    memcpy(g_error_spawn.argument, SNX_ERROR_SCREEN, sizeof SNX_ERROR_SCREEN - 1);
    memcpy(g_error_spawn.argument + sizeof SNX_ERROR_SCREEN - 1, record, strlen(record) + 1);
    return 0;
}
int snx_error_spawn_ready(void) { return g_error_spawn.program[0] != 0; }

/* the action a status of the panel's process says, -1 for one that is none */
static int action_of(int status) {
    int action = status - SNX_ERROR_EXIT_BASE;
    return action > SNX_ACTION_NONE && action < SNX_ACTIONS ? action : -1;
}
/* text at out, cut to it, for a reason made with no formatting */
static void put_text(char* out, size_t size, const char* text) {
    size_t n = strlen(text) < size ? strlen(text) : size ? size - 1 : 0;
    if (!size) return;
    memcpy(out, text, n);
    out[n] = 0;
}
/* text, then the decimal digits of v, at out */
static void put_number(char* out, size_t size, const char* text, long v) {
    char digits[24];
    size_t n = 0, t = strlen(text);
    unsigned long u = v < 0 ? 0ul - (unsigned long)v : (unsigned long)v;
    do digits[n++] = (char)('0' + u % 10);
    while ((u /= 10) && n < sizeof digits);
    if (t + n + 2 > size) return;
    memcpy(out, text, t);
    if (v < 0) out[t++] = '-';
    for (size_t i = 0; i < n; ++i) out[t + i] = digits[n - 1 - i];
    out[t + n] = 0;
}

#ifdef _WIN32
int snx_error_spawn(const SnxError* e, char* why, size_t why_size) {
    size_t n = snx_error_text(e, g_error_spawn.text, sizeof g_error_spawn.text);
    wchar_t program[4096], record[4096], command[8400];
    STARTUPINFOW si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    HANDLE f;
    DWORD wrote = 0, status = 0;
    if (!snx_error_spawn_ready() || !n || !MultiByteToWideChar(CP_UTF8, 0, g_error_spawn.program, -1, program, 4096) ||
        !MultiByteToWideChar(CP_UTF8, 0, g_error_spawn.record, -1, record, 4096)) {
        put_text(why, why_size, "the error panel's process is not prepared");
        return -1;
    }
    f = CreateFileW(record, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE || !WriteFile(f, g_error_spawn.text, (DWORD)n, &wrote, NULL) || wrote != n) {
        put_number(why, why_size, "the error's record could not be written: Windows error ", (long)GetLastError());
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        return -1;
    }
    CloseHandle(f);
    _snwprintf(command, sizeof command / sizeof *command, L"\"%ls\" \"--error-screen=%ls\"", program, record);
    command[sizeof command / sizeof *command - 1] = 0;
    if (!CreateProcessW(program, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        put_number(why, why_size, "the error panel's process did not start: Windows error ", (long)GetLastError());
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &status);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (action_of((int)status) < 0) {
        put_number(why, why_size, "the error panel's process showed nothing: its status ", (long)status);
        return -1;
    }
    return action_of((int)status);
}
#else
extern char** environ;
int snx_error_spawn(const SnxError* e, char* why, size_t why_size) {
    size_t n = snx_error_text(e, g_error_spawn.text, sizeof g_error_spawn.text), done = 0;
    char* argv[] = {g_error_spawn.program, g_error_spawn.argument, NULL};
    int fd, status = 0;
    pid_t pid;
    if (!snx_error_spawn_ready() || !n) {
        put_text(why, why_size, "the error panel's process is not prepared");
        return -1;
    }
    fd = open(g_error_spawn.record, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    while (fd >= 0 && done < n) {
        ssize_t w = write(fd, g_error_spawn.text + done, n - done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        done += (size_t)w;
    }
    if (fd >= 0) close(fd);
    if (fd < 0 || done < n) {
        put_number(why, why_size, "the error's record could not be written: errno ", errno);
        return -1;
    }
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    pid = _Fork(); /* no atfork handlers, which a crash's handler must not run */
#else
    pid = fork();
#endif
    if (pid == 0) {
        execve(g_error_spawn.program, argv, environ);
        _exit(127);
    }
    if (pid < 0) {
        put_number(why, why_size, "the error panel's process did not start: errno ", errno);
        return -1;
    }
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) {
            put_number(why, why_size, "the error panel's process could not be waited for: errno ", errno);
            return -1;
        }
    if (!WIFEXITED(status) || action_of(WEXITSTATUS(status)) < 0) {
        put_number(why, why_size, "the error panel's process showed nothing: its status ",
                   WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status));
        return -1;
    }
    return action_of(WEXITSTATUS(status));
}
#endif
