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
/* the forms of the host a test takes one after another: another where the back end has features to go without */
#define STEPS_FORMS(forms) (native_steps_forms() < (forms) ? native_steps_forms() : (forms))
/* at the start of main: a host with no native back end has nothing to run */
#define STEPS_REQUIRE()                                                                                                \
    do {                                                                                                               \
        if (!native_steps_available()) {                                                                               \
            puts("SKIP: this host has no native back end");                                                            \
            return 77;                                                                                                 \
        }                                                                                                              \
    } while (0)

#define steps_begin        native_steps_begin
#define steps_add          native_steps_add
#define steps_add_function native_steps_add_function
#define steps_load         native_steps_load
#define steps_dump         native_steps_dump
#define steps_end          native_steps_end
#define step_run           native_step_run
static inline Step steps_at(const Steps* s, size_t index) { return (Step){s, index}; }
static inline GuestFn steps_function(const Steps* s, size_t index) { return s->entries[index].function; }
/* a bench's arguments: more than most of them (argv[0] and --bench counted), or a level (one digit, 0 to 3) at
   level_at where the bench takes another argument now, is refused with what was given, LLVM's level having gone: 1,
   else 0. level_at 0 checks the count alone */
static inline int steps_bench_args(int argc, char** argv, int most, int level_at, const char* usage) {
    if (argc > most) {
        fprintf(stderr, "%s --bench: %d arguments, at most %d (a level went with LLVM); usage: %s\n", argv[0], argc - 2,
                most - 2, usage);
        return 1;
    }
    if (level_at && argc > level_at && argv[level_at][0] >= '0' && argv[level_at][0] <= '3' && !argv[level_at][1]) {
        fprintf(stderr, "%s --bench: %s where a level was, which went with LLVM; usage: %s\n", argv[0], argv[level_at],
                usage);
        return 1;
    }
    return 0;
}
/* the back end's name */
static inline const char* steps_name(void) { return snx_native_name(); }

#endif
