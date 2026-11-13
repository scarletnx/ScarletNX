/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The blocks a run dispatches: the compiled module's where memory holds the code they were compiled from, and for
   anything else (code the offline pass did not find, code the game made) a block compiled when it is first reached,
   which the next compile starts from when the code is the image's. */
#ifndef SNX_NDS_RECOMP_H
#define SNX_NDS_RECOMP_H
#include "common/cpu/arm/arm.h"
#include "nds/board/nds.h"

typedef struct NdsRecomp NdsRecomp;
/* module: the compiled module's path, loaded when it is there and is this image's and this build's (what it says
   otherwise is in note). NULL when out of memory */
NdsRecomp* nds_recomp_new(const NdsRom* rom, const char* module, char* note, size_t note_size);
void nds_recomp_free(NdsRecomp* recomp);
bool nds_recomp_has_module(const NdsRecomp* recomp);
/* The block at pc for the code memory holds there now, and how many instructions it runs at most. NULL with the reason
   in error when it cannot be compiled */
SnxArmBlock nds_recomp_block(NdsRecomp* recomp, NdsBoard* board, uint32_t pc, int thumb, unsigned* instructions,
                             char* error, size_t error_size);
/* the blocks compiled at run time: all of them, and the ones of the image's own code */
void nds_recomp_counts(const NdsRecomp* recomp, uint64_t* compiled, uint64_t* hinted, uint64_t* from_module);
/* how many of the blocks the run reached are Thumb */
uint32_t nds_recomp_thumb(const NdsRecomp* recomp);
/* The image's blocks this run compiled, added to the hints at path: how many were new there */
size_t nds_recomp_save_hints(NdsRecomp* recomp, const char* path);
/* whether the module at path is this build's and rom's, read without loading it */
bool nds_module_current(const char* path, const NdsRom* rom);
#endif
