/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime's fatal with a kind of error (platform/log.h's fatal_as, fatal_given), for a test that has its own
   fatal: the message made, then that fatal with it, so a test that reads its fatal's text reads the same */
#include "nx/runtime/platform/log.h"
#include <stdarg.h>
#include <stdio.h>

void fatal_as(int kind, const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    (void)kind;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fatal("%s", msg);
}
void fatal_given(int status, const SnxError* given, const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    (void)status;
    (void)given;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fatal("%s", msg);
}
