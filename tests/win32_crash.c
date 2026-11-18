/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* win32_crash.h: an unhandled exception filter that writes stderr straight and unwinds the crash's own context, so a
   heap or a stdio the crash broke does not take the report with it */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "win32_crash.h"
#include <stdio.h>
#include <string.h>

#define CRASH_FRAMES 32
/* the context's program counter and stack pointer */
#if defined(__x86_64__) || defined(_M_X64)
#define CONTEXT_PC Rip
#define CONTEXT_SP Rsp
#elif defined(__aarch64__) || defined(_M_ARM64)
#define CONTEXT_PC Pc
#define CONTEXT_SP Sp
#endif

static void say(const char* text) {
    DWORD wrote;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, (DWORD)strlen(text), &wrote, NULL);
}
/* the module at holds, its file's name in name; NULL for none */
static HMODULE module_of(ULONG_PTR at, char* name, int size) {
    HMODULE m = NULL;
    wchar_t path[MAX_PATH];
    DWORD n;
    name[0] = 0;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)at, &m))
        return NULL;
    n = GetModuleFileNameW(m, path, MAX_PATH);
    if (n && n < MAX_PATH) {
        const wchar_t* file = wcsrchr(path, L'\\');
        WideCharToMultiByte(CP_UTF8, 0, file ? file + 1 : path, -1, name, size, NULL, NULL);
    }
    return m;
}
static void widen(wchar_t* out, const char* in) {
    while ((*out++ = (unsigned char)*in++) != 0) {}
}
/* llvm-symbolizer's function and line for each of the program's offsets, on stderr: 1 when it ran */
static int symbolize(const ULONG_PTR* offsets, int count) {
    static wchar_t command[MAX_PATH + 96 + CRASH_FRAMES * 24];
    wchar_t exe[MAX_PATH];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE err;
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    int ran, i;
    if (!n || n >= MAX_PATH) return 0;
    widen(command, "llvm-symbolizer --pretty-print --relative-address --obj=\"");
    wcscat(command, exe);
    wcscat(command, L"\"");
    for (i = 0; i < count; ++i) {
        char number[24];
        snprintf(number, sizeof number, " 0x%llx", (unsigned long long)offsets[i]);
        widen(command + wcslen(command), number);
    }
    if (!DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_ERROR_HANDLE), GetCurrentProcess(), &err, 0, TRUE,
                         DUPLICATE_SAME_ACCESS))
        return 0;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = err;
    ran = CreateProcessW(NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi) != 0;
    if (ran) {
        WaitForSingleObject(pi.hProcess, 20000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    CloseHandle(err);
    return ran;
}
static LPTOP_LEVEL_EXCEPTION_FILTER g_before; /* the one before, mingw's CRT's: the process ends as it did */
static LONG WINAPI crash_said(EXCEPTION_POINTERS* e) {
    static LONG reporter;
    const EXCEPTION_RECORD* r = e->ExceptionRecord;
    char line[512], name[MAX_PATH];
    LONG me = (LONG)GetCurrentThreadId(), was = InterlockedCompareExchange(&reporter, me, 0);
    if (was == me) return g_before ? g_before(e) : EXCEPTION_CONTINUE_SEARCH; /* a crash in the report */
    if (was) Sleep(INFINITE); /* another thread is saying its crash, which ends the process */
    snprintf(line, sizeof line, "*** exception %08lx at %p", (unsigned long)r->ExceptionCode, r->ExceptionAddress);
    say(line);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        ULONG_PTR how = r->ExceptionInformation[0];
        const char* what = how == 1 ? "writing" : how == 8 ? "running" : "reading";
        snprintf(line, sizeof line, ", %s %#llx", what, (unsigned long long)r->ExceptionInformation[1]);
        say(line);
    }
    say("\n");
#ifdef CONTEXT_PC
    {
        static ULONG_PTR own[CRASH_FRAMES];
        CONTEXT c = *e->ContextRecord;
        HMODULE self = GetModuleHandleW(NULL);
        int owns = 0, i;
        for (i = 0; i < CRASH_FRAMES && c.CONTEXT_PC; ++i) {
            DWORD64 pc = c.CONTEXT_PC, sp = c.CONTEXT_SP, base = 0, frame = 0;
            ULONG_PTR at = (ULONG_PTR)(i ? pc - 1 : pc); /* past the first, a return address: its call is before it */
            HMODULE m = module_of(at, name, sizeof name);
            PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(pc, &base, NULL);
            void* data;
            snprintf(line, sizeof line, "  #%d %s+%#llx\n", i, m ? name : "?",
                     (unsigned long long)(m ? at - (ULONG_PTR)m : at));
            say(line);
            if (m == self) own[owns++] = at - (ULONG_PTR)m;
            if (f)
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, pc, f, &c, &data, &frame, NULL);
            else { /* a leaf: its return address on the top of the stack, or in the link register on ARM64 */
#if defined(__x86_64__) || defined(_M_X64)
                c.Rip = *(DWORD64*)(ULONG_PTR)c.Rsp;
                c.Rsp += 8;
#else
                c.Pc = c.Lr;
#endif
            }
            if (c.CONTEXT_PC == pc && c.CONTEXT_SP == sp) break; /* no way further up */
        }
        if (owns && !symbolize(own, owns))
            say("  no llvm-symbolizer on the PATH: the offsets are into this build's test program\n");
    }
#else
    (void)name;
    (void)module_of;
    (void)symbolize;
#endif
    return g_before ? g_before(e) : EXCEPTION_CONTINUE_SEARCH;
}
void snx_test_crash_said(void) { g_before = SetUnhandledExceptionFilter(crash_said); }
