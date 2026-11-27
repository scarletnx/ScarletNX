/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The watch's faults (vita/runtime/watch.h) in a test that runs the renderer, taken as the runtime takes them
   (vita/runtime/run.c): a write to a page the watch protected, the test's own or a CPU device's threads' as they draw
   into imported memory, is counted and goes on; any other fault ends the test with its signal. On Windows the watch's
   own handler takes them */
#pragma once
#ifndef _WIN32
#include "vita/runtime/watch.h"
#include <signal.h>
#include <unistd.h>

static inline void vita_test_on_fault(int signal, siginfo_t* info, void* context) {
    (void)context;
    if (vita_watch_fault((uintptr_t)info->si_addr)) return;
    static const char said[] = "a fault the watch does not know\n";
    if (write(2, said, sizeof said - 1) < 0) _exit(3);
    _exit(signal);
}
/* SIGBUS too: macOS's, for a page whose protection refuses the write */
static inline void vita_test_take_faults(void) {
    struct sigaction action = {.sa_sigaction = vita_test_on_fault, .sa_flags = SA_SIGINFO | SA_NODEFER};
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
}
#else
static inline void vita_test_take_faults(void) {}
#endif
