/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The step functions of the instruction tests, each word one function u64 step(CPU*) that returns the next pc, by the
   host's native back end (native_steps.h) */
#ifndef SNX_TEST_STEPS_H
#define SNX_TEST_STEPS_H

#include <fenv.h>
#include <stdio.h>
#include <string.h>

#include "native_steps.h"

typedef NativeSteps Steps;
typedef NativeStep Step;

enum { STEPS_HOST = NATIVE_STEPS_HOST, STEPS_PORTABLE = NATIVE_STEPS_PORTABLE, STEPS_GENERIC = NATIVE_STEPS_GENERIC };
/* the optimization levels and forms of the host a test takes one after another: a native back end has one level, and
   another form where it has features to go without */
#define STEPS_LEVELS(levels) 1
#define STEPS_FORMS(forms)   (native_steps_forms() < (forms) ? native_steps_forms() : (forms))
/* at the start of main: a host with no native back end has nothing to run */
#define STEPS_REQUIRE()                                                                                                \
    do {                                                                                                               \
        if (!native_steps_available()) {                                                                               \
            puts("SKIP: this host has no native back end");                                                            \
            return 77;                                                                                                 \
        }                                                                                                              \
    } while (0)

#define steps_begin        native_steps_begin
#define steps_begin_opt    native_steps_begin_opt
#define steps_add          native_steps_add
#define steps_add_function native_steps_add_function
#define steps_load         native_steps_load
#define steps_dump         native_steps_dump
#define steps_end          native_steps_end
#define step_run           native_step_run
static inline Step steps_at(const Steps* s, size_t index) { return (Step){s, index}; }
static inline GuestFn steps_function(const Steps* s, size_t index) { return s->entries[index].function; }
/* the back end's name */
static inline const char* steps_name(void) { return snx_native_name(); }

#endif
