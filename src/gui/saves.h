/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A game's saves out of ScarletNX into the file another emulator or a console's save tool keeps them in, and back in
   from one, without SDL: a format per console, on what they all share here. An import puts the save together beside
   its place first, in a folder whose name starts with a dot (the runtime and the launcher take such a name for no
   save's, SNX_SAVE_HIDDEN), then moves the save there before to the trash and the new one into its place, so what
   cannot be done leaves the save as it was. Every file is an entry of the report (gui/clean.h's), done or not and why.
   The Switch's is Eden's zip. The launcher's Export All Saves writes one zip of every console's, a folder each
   (GUI_SAVES_SWITCH), each in its console's format */
#ifndef SNX_GUI_SAVES_H
#define SNX_GUI_SAVES_H
#include "common/utilities/common.h"
#include "gui/clean.h"
#include <stdbool.h>
#include <stddef.h>

/* the folder of the Switch's saves in a zip of every console's */
#define GUI_SAVES_SWITCH "switch/"

/* The save put together in staged (a folder or a file) into the place of save. The one there before goes first to the
   folder trash (made when it is not there, on save's disk) as <name>.<UTC time>, a number after it rather than over
   one there, with its note (<save>.version, SNX_SAVE_NOTE) as <that>.version; a save that is an empty folder is removed
   instead. A save that is a link is refused: the folder it names would keep its files. 0, with where the save before
   went in backup ("" for none); else nonzero with why in error, the save and its note back where they were and staged
   removed */
int gui_saves_put(GuiCleaned* done, const char* staged, const char* save, const char* trash, const char* name,
                  char* backup, size_t backup_size, char* error, size_t size);
/* what a file an export writes is called by default, what it holds (a game's title, "Switch saves Player") and when
   (seconds since 1970, shown in local time), into out: "<what> <YYYY-MM-DD HH-MM>.<extension>", every letter a file's
   name cannot take on any system (\ / : * ? " < > | and the control characters) as _ */
void gui_saves_file_name(char* out, size_t size, const char* what, SnxU64 when, const char* extension);

/* A zip an export writes, one console's saves or several's, at <path>.part until it is whole */
typedef struct GuiSavesZip GuiSavesZip;
/* the zip begun for path, each file it takes an entry of done; NULL with why in error */
GuiSavesZip* gui_saves_zip_begin(const char* path, GuiCleaned* done, char* error, size_t size);
/* the zip ended: with keep, finished and moved to path, the files and bytes it took in *files and *bytes; else, or
   when that cannot be, its part removed and path as it was. 0, else nonzero with why in error (kept when error has
   one already). zip is freed */
int gui_saves_zip_end(GuiSavesZip* zip, bool keep, size_t* files, SnxU64* bytes, char* error, size_t size);

/* A Switch game's save of one user as ScarletNX keeps it (fs.c's save_folder): the game's title ID, the folder of its
   Account save, there or not, the trash a save there before goes to (on the same disk) and the name it takes there */
typedef struct {
    SnxU64 title;
    const char* folder;
    const char* trash;
    const char* name;
} GuiSaveNx;
/* saves of the Switch, each one's strings owned */
typedef struct {
    GuiSaveNx* at;
    size_t count;
} GuiSavesNx;
void gui_saves_nx_free(GuiSavesNx* list);
/* Where the user uid's (32 hex digits) Account save of the game named name, in its folder game, is: with global
   (save_layout=global) the data folder root's save/user.<uid>/<name>, its trash root's trash/; else
   <game>/<name>_data/save/user.<uid>, its trash <game>/<name>_data/trash/. The other layout's place while only it holds
   one, which the runtime plays where it is. 1 with it in out (its strings owned, as gui_saves_nx_free frees them), 0
   for a game with no title ID of its own (its name is none), which Eden's format has no place for */
int gui_saves_nx_place(const char* game, const char* name, const char* root, const char* uid, bool global,
                       GuiSaveNx* out);
/* every Switch save the user uid has, in title ID order: each folder of root's save/user.<uid>/ named by a title ID
   (save_layout=global's), and each game's own in the count folders of games (each game a folder <name>/ with
   <name>_data/ in it), as gui_saves_nx_place places them, each title once */
void gui_saves_nx_list(const char* root, const char* const* folders, size_t count, const char* uid, bool global,
                       GuiSavesNx* list);
/* where title's save of the user uid goes: the game's in folders when one of them has it (gui_saves_nx_place), else
   with global root's save/user.<uid>/<title ID>. 1 with it in out, 0 with neither (save_layout=game keeps a save in
   its game's folder) */
int gui_saves_nx_find(const char* root, const char* const* folders, size_t count, const char* uid, bool global,
                      SnxU64 title, GuiSaveNx* out);

/* Eden's export of saves (Android's of a game and of every game, Qt's Data Manager > Saves) is a zip with a folder for
   each game, named by its title ID in 16 hex digits, holding the files of the user's Account save as the game sees
   them, and .yuzu_save_size, Eden's record of the save's size. In a zip of every console's the same is under prefix
   (GUI_SAVES_SWITCH), and what is not under it is another console's; prefix "" for Eden's own. The title IDs the zip at
   path holds, each once, in order: 0 with *titles (the caller's to free) and *count; else nonzero with why in error,
   for a file that is no zip or a zip laid out otherwise. shown is the zip as messages name it: the file picked, when
   path is a copy of it */
int gui_saves_eden_titles(const char* path, const char* shown, const char* prefix, SnxU64** titles, size_t* count,
                          char* error, size_t size);
/* Each of the count saves into zip under prefix as Eden exports them: the files of its last commit
   (SNX_SAVE_COMMITTED), else of its working copy after a commit cut short, else of the folder itself for a save as
   the versions before kept one (not those SNX_SAVE_HIDDEN takes for a sync tool's), with their folders. A save not
   there or with no file is an entry that says so. How many went in into *kept. 0, else nonzero with why in error */
int gui_saves_eden_add(GuiSavesZip* zip, const char* prefix, const GuiSaveNx* saves, size_t count, size_t* kept,
                       char* error, size_t size);
/* gui_saves_eden_add's into a zip of their own at path, as Eden's: with none of them there, no zip is made. 0, else
   nonzero with why in error and path as it was */
int gui_saves_eden_export(GuiCleaned* done, const char* path, const GuiSaveNx* saves, size_t count, char* error,
                          size_t size);
/* what an import did: the saves put in place, their files and bytes, how many saves there before went to the trash
   and where the first of them went ("" for none) */
typedef struct {
    size_t saves, files, backups;
    SnxU64 bytes;
    char backup[SNX_PATH_SIZE];
} GuiSavesDone;
/* Each of the count saves out of the zip at path as Eden exports them (under prefix, gui_saves_eden_titles), put in
   its folder by gui_saves_put: the files of its title ID's folder as its last commit (SNX_SAVE_COMMITTED), which the
   game's next start copies into its working one, without .yuzu_save_size, of which this version keeps no file. The
   whole zip is checked before anything is written: a member outside a folder named by a title ID, or a save whose
   title ID has no folder there, is refused, naming what the zip holds; its other games are an entry. One save after
   the other, the first that cannot be put the last tried, what was done added to *result. 0, else nonzero with why
   in error */
int gui_saves_eden_import(GuiCleaned* done, const char* path, const char* shown, const char* prefix,
                          const GuiSaveNx* saves, size_t count, GuiSavesDone* result, char* error, size_t size);
#endif
