/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The host's native back end's step functions for the instruction tests (steps.h): each word one guest function of its
   own (snx_native_object, a JIT unit of one function, as the runtime's JIT makes them), entered at the word's pc and
   left through the runtime's miss with the pc the word goes on to, linked with the runtime library and loaded into this
   process, either as a module file for dlopen or LoadLibrary, as the AOT path loads a game's, or with the JIT's loader
   (code_loader.c). The words share one pc, so each is a unit and an object of its own; the functions of a codegen of
   the test's own are one unit.

   A word the back end has no code for has no step (native_steps_made), where steps_load fails naming the first, unless
   the test says it expects some (NativeSteps.partial) */
#ifndef SNX_TEST_NATIVE_STEPS_H
#define SNX_TEST_NATIVE_STEPS_H

#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/recompiler/loader/code_loader.h"
#include "common/cpu/recompiler/mach/native.h"

/* what of the host the back end may use (native_steps_begin's form) */
enum {
    NATIVE_STEPS_HOST,     /* this host's processor, as the recompiler builds for it */
    NATIVE_STEPS_PORTABLE, /* its baseline: x86-64 without BMI1, BMI2, LZCNT, AVX, FMA, F16C, AES-NI and SHA-NI, arm64
                              without every optional feature. Another machine's has none to go without: the same code */
    NATIVE_STEPS_GENERIC = NATIVE_STEPS_PORTABLE
};

typedef struct NativeUnit NativeUnit;
/* what a step has once loaded: its function, NULL where there is no code, its address, and its unit's snx_stale */
typedef struct {
    GuestFn function;
    u64 pc;
    u8* stale;
} NativeEntry;
typedef struct {
    int jit;     /* load with the JIT's loader, not as a module file */
    int form;    /* NATIVE_STEPS_HOST or NATIVE_STEPS_PORTABLE */
    int partial; /* words with no code are left without a step */
    size_t count, cap;
    NativeUnit* units; /* while steps are added */
    size_t nunits, cap_units;
    NativeEntry* entries; /* of each add, by index, filled by native_steps_load */
    SnxCodeLoader* loader;
    void** libraries; /* the module files loaded */
    size_t nlibraries;
    char* dir; /* the module files' */
} NativeSteps;

/* one step: its functions and the index of it */
typedef struct {
    const NativeSteps* steps;
    size_t index;
} NativeStep;

/* 1 where this host has a native back end (the tests skip where it has none) */
int native_steps_available(void);
/* 1 where the back end has another form than the host's (NATIVE_STEPS_PORTABLE) */
int native_steps_forms(void);

/* starts a set of steps: 0, or -1 with the reason in error. The optimization level says nothing to a native back end */
int native_steps_begin(NativeSteps* s, int jit, int form, char* error, size_t error_size);
int native_steps_begin_opt(NativeSteps* s, int jit, int form, char opt, char* error, size_t error_size);
/* word's step at pc for a guest with these A64 features (0: the Switch's); its index */
size_t native_steps_add(NativeSteps* s, u32 word, u64 pc, u64 features);
/* a function of codegen, at its index in the table as its entry F_<address>: a GuestFn, not a step. Consecutive
   functions of one codegen are one unit, which calls each other's directly */
size_t native_steps_add_function(NativeSteps* s, SnxCodegen* codegen, const SnxFunction* function);
/* compiles and loads the steps added, s->entries then holding them. 0, or -1 with the reason in error */
int native_steps_load(NativeSteps* s, char* error, size_t error_size);
/* Saves the object of the first unit as prefix.o. The caller must end these steps afterward */
int native_steps_dump(NativeSteps* s, const char* prefix, char* error, size_t error_size);
void native_steps_end(NativeSteps* s);

/* whether the back end has code for a step */
static inline int native_steps_made(const NativeSteps* s, size_t index) { return s->entries[index].function != NULL; }
/* runs a step from *c and returns the next pc: the pc it goes on to, or of a word that leaves the function (RET, UNDEF,
   BRK) CPU.pc. *c's pc, its dispatch table and runtime are as they were. A word that branches to itself returns its own
   pc, once */
u64 native_step_run(NativeStep step, CPU* c);

#endif
