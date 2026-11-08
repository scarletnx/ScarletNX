/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The debugger tracing the process as common.c reads it (snx_debugger), which the stats box and F1's help name: the
   tracer in a Linux /proc status text, TracerPid's (none, one, the line first or last, a word in another line that ends
   the same, no number); this test, which nothing traces, has none; and on Linux and FreeBSD a child it traces
   (PTRACE_TRACEME, PT_TRACE_ME) has the test for its debugger, by the name the system keeps for it. Elsewhere the
   child isn't made */
#include "common/utilities/common.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#if defined(__linux__) || defined(__FreeBSD__)
#include <errno.h>
#include <stdlib.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#define TRACED_CHILD 1
/* the child asks to be traced by its parent; the parent lets it go on from a stop */
#if defined(__linux__)
#define TRACE_ME()    ptrace(PTRACE_TRACEME, 0, NULL, NULL)
#define TRACE_GO(pid) ptrace(PTRACE_CONT, (pid), NULL, NULL)
#else
#define TRACE_ME()    ptrace(PT_TRACE_ME, 0, NULL, 0)
#define TRACE_GO(pid) ptrace(PT_CONTINUE, (pid), (caddr_t)1, 0)
#endif
#endif

/* a Linux status text with a tracer: /proc/self/status as a run under gdb has it */
static const char g_traced[] = "Name:\tscarletnx\nUmask:\t0022\nState:\tS (sleeping)\nTgid:\t4242\nNgid:\t0\n"
                               "Pid:\t4242\nPPid:\t4200\nTracerPid:\t31337\nUid:\t1000\t1000\t1000\t1000\n";

#ifdef TRACED_CHILD
/* the name the system keeps for this process, as a debugger's is read: Linux's comm, FreeBSD's p_comm */
static void own_name(char* name, size_t size) {
#if defined(__linux__)
    FILE* f = fopen("/proc/self/comm", "r");
    assert(f && fgets(name, (int)size, f));
    fclose(f);
    name[strcspn(name, "\n")] = 0;
#else
    snprintf(name, size, "%.19s", getprogname());
#endif
}
/* a child that asks to be traced by the test: what it reads of its debugger, "refused" when the system says no */
static int traced_child(char* name, size_t size) {
    int pipe_fd[2], status = 0;
    ssize_t got;
    pid_t child;
    assert(!pipe(pipe_fd));
    child = fork();
    assert(child >= 0);
    if (!child) {
        char said[SNX_DEBUGGER_NAME + 16];
        char read_name[SNX_DEBUGGER_NAME] = {0};
        close(pipe_fd[0]);
        if (TRACE_ME())
            snprintf(said, sizeof said, "refused %d", errno);
        else if (snx_debugger(read_name, sizeof read_name))
            snprintf(said, sizeof said, "traced %s", read_name);
        else
            snprintf(said, sizeof said, "none");
        if (write(pipe_fd[1], said, strlen(said) + 1) < 0) _exit(2);
        _exit(0);
    }
    close(pipe_fd[1]);
    got = read(pipe_fd[0], name, size - 1);
    close(pipe_fd[0]);
    name[got > 0 ? got : 0] = 0;
    /* a traced child that stops on a signal (none is sent to it) would wait here: it goes on until it exits */
    while (waitpid(child, &status, 0) == child && WIFSTOPPED(status)) TRACE_GO(child);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
#endif

int main(void) {
    char name[SNX_DEBUGGER_NAME] = "untouched";
    /* the tracer's id where the line says one, 0 where it says none, and for a text without it */
    assert(snx_status_tracer(g_traced) == 31337);
    assert(snx_status_tracer("Name:\tscarletnx\nTracerPid:\t0\nUid:\t1000\n") == 0);
    assert(snx_status_tracer("Name:\tscarletnx\nPid:\t4242\n") == 0);
    assert(snx_status_tracer("") == 0);
    /* the line first, and last without its newline */
    assert(snx_status_tracer("TracerPid:\t77\nName:\tx\n") == 77);
    assert(snx_status_tracer("Name:\tx\nTracerPid:\t78") == 78);
    /* a line whose word only ends the same isn't it: the line of its own is read */
    assert(snx_status_tracer("Name:\tMyTracerPid:\t5\nTracerPid:\t9\n") == 9);
    assert(snx_status_tracer("Name:\tMyTracerPid:\t5\n") == 0);
    /* no number after it */
    assert(snx_status_tracer("TracerPid:\t\nUid:\t1000\n") == 0);
    assert(snx_status_tracer("TracerPid:\tx\n") == 0);
    /* nothing traces the test: none, and the name left as it was */
    if (snx_debugger(name, sizeof name)) {
        fprintf(stderr, "the test is traced by \"%s\": run it without a debugger\n", name);
        return 1;
    }
    assert(!strcmp(name, "untouched"));
#ifdef TRACED_CHILD
    {
        char said[SNX_DEBUGGER_NAME + 16], own[SNX_DEBUGGER_NAME], want[SNX_DEBUGGER_NAME + 16];
        int rc = traced_child(said, sizeof said);
        own_name(own, sizeof own);
        snprintf(want, sizeof want, "traced %s", own);
        if (!strncmp(said, "refused", 7)) {
            printf("the system refused a traced child (%s, errno): its debugger is not read here\n", said);
        } else if (rc != 0 || strcmp(said, want)) {
            fprintf(stderr, "a child the test traces read \"%s\" (exit %d), not \"%s\"\n", said, rc, want);
            return 1;
        }
    }
#endif
    puts("process_debugger passed");
    return 0;
}
