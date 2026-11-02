/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_TRANSLATE_H
#define SNX_ANDROID_TRANSLATE_H
#include "android/dex/dex.h"
#include <stdbool.h>
#include <stddef.h>

/* DEX classes to JVM class files (version 52, with stack map frames), one class at a time.

   Each Dalvik register becomes up to five JVM locals of fixed types, one for each way the code reads it: int, float,
   long, double and java/lang/Object. Each local gets a value at the method's entry and keeps its type, so every
   branch target has the same frame. A value that the code needs as a narrower class gets a checkcast where it is
   used. A dataflow pass decides what the bytecode alone does not tell: whether an if-eqz or if-eq compares numbers
   or references, which array type an aget or aput reads, which register holds an object that new-instance made
   before its <init>, and when a constructor's this is initialized. docs/android/README.md has the details */

typedef struct {
    /* whether the class of descriptor ("Ljava/util/List;") is an interface. The translator asks for the classes a
       call names that the code does not call with invoke-interface, and that are not in the program. NULL takes
       each of them as a class */
    bool (*is_interface)(void* context, const char* descriptor);
    void* context;
    /* a method the translator cannot translate gets a body that throws VerifyError, and this hears of it (NULL: no
       one does) */
    void (*warn)(void* context, const char* message);
} DexTranslateOptions;

/* The app's code: its DEX files (classes.dex, classes2.dex and on), which the caller keeps open for the program's
   life. Opening it reads every method once, for the calls that the JVM needs a synthetic constructor for: R8
   inlines a trivial constructor, so a Dalvik constructor can call an ancestor's <init> that is not its direct
   superclass's, which ART allows and the JVM does not */
typedef struct DexProgram DexProgram;
DexProgram* dex_program_open(const Dex* dexes, uint32_t count, const DexTranslateOptions* options, char* error,
                             size_t error_size);
void dex_program_close(DexProgram* program);
/* the DEX file and class def of a class by its descriptor; false when the program has none */
bool dex_program_find(const DexProgram* program, const char* descriptor, uint32_t* dex, uint32_t* index);

/* the class file of DEX file `dex`'s class def `index`, *size bytes, the caller's to free. NULL with error filled
   when the class can't be translated at all (a bad class def, a constant pool past 65535 entries) */
unsigned char* dex_translate_class(const DexProgram* program, uint32_t dex, uint32_t index, size_t* size,
                                   char* error, size_t error_size);
/* the internal name of a class def's class ("java/lang/String"), into out; false when it does not fit */
bool dex_class_name(const Dex* dex, uint32_t index, char* out, size_t out_size);
#endif
