/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A 3DS game's files beside it, by the rule of every settings file (common/utilities/config_file.h): its record, the
   game's .ini (<title ID>.ini: source=, model=, system=3ds), and its own picks (ctr_model) */
#ifndef SNX_CTR_GAME_RECORD_H
#define SNX_CTR_GAME_RECORD_H
#include "3ds/board/board.h"

/* the record at path: the game it plays (source=, *source owned, NULL without one) and its console (model=, *model kept
   without one; *named, when given, whether it names one). A model= it does not take is said in said with the one kept.
   False only when it cannot be read, said in said */
bool snx_ctr_record_read(const char* path, char** source, SnxCtrModel* model, bool* named, char* said, size_t size);
/* the record at path with full_source as source=, model= and system=3ds, its other lines as they were, written beside
   it first, never over one that cannot be read: true, or false with why */
bool snx_ctr_record_write(const char* path, const char* full_source, SnxCtrModel model, char* why, size_t size);
/* the game's own picks at path: ctr_model, the last line's, owned into *console (NULL for none): 1, 0 for no file, -1 for
   one that cannot be read, said in said. Its other keys, a line with no = and ctr_model given twice named in said once a
   run, never by a value */
int snx_ctr_picks_console(const char* path, char** console, char* said, size_t size);
#endif
