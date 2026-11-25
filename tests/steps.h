/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The step functions of the instruction tests, each word one function u64 step(CPU*) that returns the next pc, by the
   back end the program is built with: LLVM's (llvm_steps.h), or with SNX_TEST_NATIVE_STEPS the host's native one
   (native_steps.h). A program that says nothing else of either is built twice, with one and with the other, and holds
   both to the same values */
#ifndef SNX_TEST_STEPS_H
#define SNX_TEST_STEPS_H

#include <fenv.h>
#include <stdio.h>
#include <string.h>

#ifdef SNX_TEST_NATIVE_STEPS
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
static inline GuestFn steps_function(const Steps* s, size_t index) { return s->fns[index]; }
/* the back end's name */
static inline const char* steps_name(void) { return snx_native_name(); }
#define steps_host_mode native_steps_host_mode

#else
#include "llvm_steps.h"

typedef LlvmSteps Steps;
typedef LlvmStep Step;

enum { STEPS_HOST = LLVM_STEPS_HOST, STEPS_PORTABLE = LLVM_STEPS_PORTABLE, STEPS_GENERIC = LLVM_STEPS_GENERIC };
#define STEPS_LEVELS(levels) (levels)
#define STEPS_FORMS(forms)   (forms)
#define STEPS_REQUIRE()      ((void)0)

#define steps_begin        llvm_steps_begin
#define steps_begin_opt    llvm_steps_begin_opt
#define steps_add          llvm_steps_add
#define steps_add_function llvm_steps_add_function
#define steps_load         llvm_steps_load
#define steps_dump         llvm_steps_dump
#define steps_end          llvm_steps_end
static inline u64 step_run(Step step, CPU* c) { return step(c); }
static inline Step steps_at(const Steps* s, size_t index) { return s->steps[index]; }
static inline GuestFn steps_function(const Steps* s, size_t index) {
    GuestFn function;
    memcpy(&function, &s->steps[index], sizeof function);
    return function;
}
static inline const char* steps_name(void) { return "LLVM"; }
/* the rounding mode of the host's a step is run under: any, as the code's entry puts back the one it assumes */
static inline int steps_host_mode(int mode) { return mode; }
#endif
#endif
