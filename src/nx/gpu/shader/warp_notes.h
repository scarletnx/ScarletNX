/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What each instruction of a program reads and writes, noted as it is translated (warp_idioms.c's proof of a lone
   lane). Both translators note it, the SM86 one (sm86_translate.c) its uniform registers and predicates too. Internal
   to src/nx/gpu/shader/ */
#ifndef SNX_SHADER_WARP_NOTES_H
#define SNX_SHADER_WARP_NOTES_H
#include "common/utilities/bits.h"

/* The registers, predicates and flags each instruction reads and writes, noted as it is written: a word
   each, the instruction << 10 | the variable << 1 | 1 for a write. A guarded write's read of what it keeps is not one */
typedef struct {
    u32 *word, n, cap;
    u32 at;   /* the instruction being written, 0 before the first */
    int full; /* one did not fit in memory */
} ShNotes;
void shader_note(ShNotes* notes, u32 variable, u32 write);
/* Pseudo instructions a program that is no Maxwell one is handed to warp_idioms.c with (sm86_lone.c), past the opcodes of
   Maxwell's: their registers are at the places of an ALU instruction's (Ra at 8, Rb at 20, Rd at 0). LO_BREV reverses a
   mask's bits, LO_FLO_SH is the FLO.SH of such a one, the lowest running lane, and LO_ISETP_NE a comparison of a lane
   and the leader */
/* LO_MATCH is MATCH.ALL, whose predicate says that every lane of the warp has one address */
enum { LO_BREV = 0x7000, LO_FLO_SH, LO_ISETP_NE, LO_MATCH };
/* a run that only the lanes whose addresses were the same took, from from up to to: where they were not, each lane's own
   atomic of own_dst (a variable, 0xffffffff for none) took its place, and the program went on at to */
typedef struct {
    u32 from, to, own_dst;
} ShMatchRun;
/* what a pseudo program says of its instructions, each by its index: where it is in its own program, whether an atomic's
   address is the one a MATCH compared, whether one is a lane's own atomic where a MATCH did not hold, and whether a
   branch or EXIT leaves there, and the runs */
typedef struct {
    const u32* where;
    const u8 *same, *own, *leave;
    const ShMatchRun* run;
    u32 runs;
} ShLanePlan;

/* the SM86 translator's variables: the registers by number, the predicates, the uniform predicates and the uniform
   registers from here */
enum { SM86_NOTE_PRED = 255, SM86_NOTE_UPRED = SM86_NOTE_PRED + 7, SM86_NOTE_UREG = SM86_NOTE_UPRED + 7 };
#endif
