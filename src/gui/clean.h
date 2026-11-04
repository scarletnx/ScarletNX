/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Settings > App's Clean Old Settings and Move Old Files, without SDL: the [Deprecated] section the runtime gives a
   game's .ini for the keys it no longer reads, taken out; what an older version left where this one no longer looks,
   moved where it keeps it. Every file is an entry of what was done, or of why it was not */
#ifndef SNX_GUI_CLEAN_H
#define SNX_GUI_CLEAN_H
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char* text;      /* an entry a line: '+' done or '!' not, a tab, the file, a tab, what was done or why not */
    int count, done; /* the entries, and the ones done */
} GuiCleaned;
/* text without its [Deprecated] sections, in place: the line [Deprecated] (a CR before its end allowed), every line
   after it up to the next [section] or the end, and the blank lines just before it. Its new size, size for none */
size_t gui_clean_deprecated(char* text, size_t size);
/* the .ini files directly in folder, and the one of each game's folder in it (<name>/<name>.ini), with a [Deprecated]
   section: how many. With done, each is written again without it, an entry each */
int gui_clean_inis(GuiCleaned* done, const char* folder);
/* an entry of the caller's: file, and what was done to it or why not */
void gui_cleaned_add(GuiCleaned* done, bool ok, const char* file, const char* what);
/* entry at's fields; the next entry, NULL after the last */
const char* gui_cleaned_entry(const char* at, bool* ok, char* file, char* what, size_t size);
void gui_cleaned_free(GuiCleaned* done);
/* Move Old Files, an entry at a time: each entry of the folder old (old itself when it is a file) into the folder now,
   made when it is not there. One already in now stays where it is, and one on another disk than now is not moved,
   each said with both paths; only, NULL-terminated, names the entries to move (NULL: every one). With apply false
   nothing is touched and the entries say what would move; with it, old goes once it is empty. How many move */
int gui_move_entries(GuiCleaned* done, const char* old, const char* now, const char* const* only, bool apply);
/* the note beside the save folder old (<old>.version, SNX_SAVE_NOTE), when there is one, to <now>.version beside the
   folder the save moves to, as gui_move_entries moves an entry and says it: one at the new place stays, and the
   old one with it. 1 when it moves (or would, without apply) */
int gui_move_save_note(GuiCleaned* done, const char* old, const char* now, bool apply);
#endif
