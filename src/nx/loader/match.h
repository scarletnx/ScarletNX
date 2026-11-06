/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Names for the functions of the game's own modules that export none, from the sdk module's exports: the SDK links
   the same compiled library (its zlib, libpng, the C library) into a game's main as into the sdk module, so a function
   of main whose code is an exported sdk function's, word for word once the bits a link moves are masked, gets that
   name. Read only: the names go to the module dump that the tools read (tools/symbols.py names crash report addresses
   and a profile's recompiled functions from it), never to the hooks or the code */
#ifndef SNX_LOADER_MATCH_H
#define SNX_LOADER_MATCH_H

#include "nx/loader/loader.h"

/* a signature's length in instructions, and so the shortest function named: shorter code (a call wrapper, a
   constructor, a deleting destructor) is too often the same in functions of different names */
#define SNX_MATCH_MIN_WORDS 32

typedef struct {
    SnxU64 addr;   /* guest address, a function start of a module that does not export it */
    SnxU32 words;  /* instructions matched: the confidence */
    SnxU32 symbol; /* the export's index in the sdk module's symbols, which names it */
} SnxMatch;

typedef struct {
    size_t sdk;        /* the sdk module's index in the program */
    size_t signatures; /* sdk exports long enough to sign, aliases counted once */
    size_t skipped;    /* sdk exports not signed: under SNX_MATCH_MIN_WORDS, or a constructor or destructor */
    size_t ambiguous;  /* function starts whose code two different sdk functions share: left unnamed */
    size_t repeated;   /* function starts left unnamed as their signature matched another start of the module too */
} SnxMatchStats;

/* the matches of a 64-bit program with an sdk module, sorted by address, in *out (malloc'd, free it; NULL with 0
   when none) */
size_t snx_match_program(const SnxProgram* program, SnxMatch** out, SnxMatchStats* stats);

#endif
