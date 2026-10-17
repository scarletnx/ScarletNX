/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_TOOL_COMMON_H
#define SNX_TOOL_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* C90 implementation; fixed-width types come from the host SDK. */
typedef uint32_t SnxU32;
typedef uint64_t SnxU64;
typedef struct {
    const char* repo; /* the data folder: games/, work/, the keys */
    const char* build;
    const char* executable;
    const char* source; /* the checkout the build came from (snx_checkout), to build tools in; NULL without one */
} SnxPaths;

void* snx_alloc(size_t size);
void* snx_realloc(void* ptr, size_t size);
char* snx_strdup(const char* s);
char* snx_format(const char* fmt, ...);
int snx_error(const char* fmt, ...);
char* snx_path_join(const char* a, const char* b);
char* snx_dirname(const char* path);
char* snx_absolute(const char* path);
char* snx_executable(const char* argv0);
int snx_is_file(const char* path);
int snx_is_dir(const char* path);
int snx_is_link(const char* path);
/* Any existing directory entry, including dangling symlinks and special files. */
int snx_path_exists(const char* path);
int snx_mkdirs(const char* path);
int snx_remove_tree(const char* path);
int snx_copy_file(const char* src, const char* dst);
int snx_copy_tree(const char* src, const char* dst);
/* dst a second name of the file src, on the same disk: nonzero when dst is there already, or the disk or system takes
   none (another volume, FAT), for the caller to copy instead */
int snx_link_file(const char* src, const char* dst);
int snx_path_within(const char* path, const char* parent);
int snx_replace(const char* src, const char* dst);
int snx_list_dir(const char* path, char*** names, size_t* count);
void snx_free_strings(char** strings, size_t count);
unsigned char* snx_read_file(const char* path, size_t* size);
int snx_write_file(const char* path, const void* data, size_t size);
int snx_write_changed(const char* path, const void* data, size_t size);
char* snx_temp_dir(const char* parent);
char* snx_user_data_dir(void);
/* Systems that follow the XDG Base Directory spec, where settings, state and caches have folders of their own */
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__ANDROID__)
#define SNX_XDG 1
#endif
enum { SNX_USER_DATA, SNX_USER_CONFIG, SNX_USER_STATE, SNX_USER_CACHE };
/* A portable copy's data folder: this folder beside the AppImage or the program */
#define SNX_PORTABLE_DIR ".scarletnx"
/* iOS: the app (dist/apple), which starts no other process and keeps its data in its Documents folder */
#ifdef __APPLE__
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#define SNX_IOS 1
#endif
#endif
/* An older version's data folder on Android, the shared storage's: a game there still runs as one of a data folder */
#define SNX_ANDROID_DIR "/sdcard/ScarletNX"
/* The user's folder for one kind of file: $XDG_CONFIG_HOME/scarletnx (~/.config), $XDG_STATE_HOME/scarletnx
   (~/.local/state) or $XDG_CACHE_HOME/scarletnx (~/.cache), a relative path ignored. Without SNX_XDG config/, state/
   and cache/ of the data folder. NULL when the environment names no home */
char* snx_user_dir(int kind);
/* 1 for a data folder that keeps each kind of file apart: the user's, a portable copy's (SNX_PORTABLE_DIR), Android's.
   Another folder's games (--repo, --outdir) keep their files in their folders */
int snx_app_dir(const char* folder);
/* Such a data folder's folder for one kind of file: the user's as snx_user_dir says, another's config/, state/ and
   cache/ in it; the data folder itself for SNX_USER_DATA */
char* snx_app_kind_dir(const char* app, int kind);
/* A path in a data folder (snx_app_dir), moved to the same place in its folder of kind: games/X/X.ini of the data
   folder is games/X/X.ini of the config folder. Any other path (another --repo, an --outdir) is returned as it is */
char* snx_user_path(const char* path, int kind);
/* The folder of kind of the launcher's own files (settings.ini and gamecontrollerdb.txt in config, recent.txt in
   state) for a program in folder: the data folder's (snx_app_dir) that folder is in, else the user's. The launcher
   names its data folder, a game its own folder, so a game finds the files of the launcher whose games/ it is in */
char* snx_menu_dir(const char* folder, int kind);
/* 1 while what an older build left at old is still there, for the caller to say each run where it is and that this
   build keeps it at now. Nothing is moved: the player does that */
int snx_left_behind(const char* old, const char* now);
/* 1 for a name the program has as the launcher, not as a game's copy: scarletnx, a model's (scarletnx-mariko), the
   AppImage's (ScarletNX-x86_64), in any case, without .exe or .AppImage. Started bare under another name it is a
   game's copy, which says what is missing and never opens the menu */
int snx_launcher_name(const char* name);
/* A game's own files in its folder, where an older build kept them, and the kind of folder this one keeps each in: its
   log and threads.txt, its settings, its pipeline cache. A name starting with '.' follows the game's name; the others
   the folder has once, for all its games. The runtime says so for a game it runs, the menu for every game */
#define SNX_GAME_FILES 4
typedef struct {
    const char* name;
    int kind;
} SnxGameFile;
extern const SnxGameFile snx_game_files[SNX_GAME_FILES];
/* work/ of a data folder, the drivers' extractions: the user's is in its cache folder. An older build's
   work/ in the data folder stays, a line on stdout saying so */
char* snx_work_dir(const char* data_dir);
/* The folder work/, games/ and the keys are in when a run names none (--repo), and the menu's games/: SNX_PORTABLE_DIR
   beside the AppImage or the program, when it is there (a portable copy); else the user's data folder. NULL when none
   can be named */
char* snx_data_dir(const char* program_dir);
/* The checkout a program run from its build tree (build; NULL: any program) was built from, for what only a checkout
   has: its sources, and what an older version kept in it. NULL for any other program */
char* snx_checkout(const char* checkout, const char* build, const char* program_dir);
/* What an older version kept in the checkout of a build tree's program, its data folder then: games/, work/, the
   keys and scarletnx-gnuabi's sysroot, linux/. snx_checkout_now names where this one keeps each for the data folder data; snx_checkout_left says so on
   stderr for each still there. Nothing is moved: the player does that (docs/DEPRECATIONS.md) */
#define SNX_CHECKOUT_FILES 5
extern const char* const snx_checkout_files[SNX_CHECKOUT_FILES];
char* snx_checkout_now(const char* data, int file);
void snx_checkout_left(const char* checkout, const char* data);
/* The name of a dump's extraction in work/: its file's name without the extension, kept to letters, digits, UTF-8,
   spaces, '-' and '_', trailing spaces cut; "Game" when nothing is left */
char* snx_dump_name(const char* input);
/* Why folder cannot take what the drivers write there, and with run the games run from it: "is not there", "is not a
   folder", "cannot be written" or "is on a disk that runs no programs (noexec)"; NULL when it can */
const char* snx_folder_unfit(const char* folder, int run);
SnxU64 snx_free_space(const char* path);
/* 1 when a and b are on one disk (one file system), each a folder or one still to be made under the nearest folder
   there: what goes into one takes the other's free space */
int snx_same_disk(const char* a, const char* b);
SnxU64 snx_file_size(const char* path);
int snx_make_executable(const char* path);
unsigned snx_cpu_count(void);
/* seconds on a monotonic clock, for durations */
double snx_seconds(void);
/* Address space for code the process writes and then runs: reserved without access, then given access in whole pages.
   snx_vm_protect commits the pages it makes accessible and decommits them with SNX_VM_NONE on Windows. 0 or -1 */
enum { SNX_VM_NONE, SNX_VM_READ, SNX_VM_READ_WRITE, SNX_VM_READ_EXECUTE };
size_t snx_vm_page_size(void);
void* snx_vm_reserve(size_t size); /* NULL when the system refuses */
int snx_vm_protect(void* p, size_t size, int access);
void snx_vm_release(void* p, size_t size);
/* Makes instructions written to [p, p + size) visible to instruction fetch, once the pages are executable. Another
   core needs nothing more when the pages were never executable before: it cannot hold old instructions for them */
void snx_vm_flush_code(void* p, size_t size);
void snx_tool_console(void);
char* snx_find_tool(const char* name);
/* argv is NULL terminated. No shell. Captured output is heap-owned by caller.
   home isolates HOME/USERPROFILE and removes XDG_CONFIG_HOME in the child. */
int snx_run(const char* const* argv, const char* cwd, const char* home, char** output);
/* snx_run calling tick(data) about once a second while the child runs: a long tool can say how far it is */
int snx_run_ticking(const char* const* argv, const char* cwd, const char* home, char** output, void (*tick)(void*),
                    void* data);
/* Foreground workflow stage with inherited stdio, interruption forwarding and diagnostics.
   A stopped POSIX child is aborted so a rerun can restart that stage. Not thread safe. */
int snx_run_stage(const char* const* argv, const char* cwd, const char* stage);
/* As above, but intentional debugger stops do not abort the game. */
int snx_run_debug_stage(const char* const* argv, const char* cwd, const char* stage);
FILE* snx_fopen(const char* path, const char* mode);

/* The game's name in a control.nacp: American English's, else the first language's that has one, in UTF-8 without
   control characters. 1 when it has one, else 0 and "" */
int snx_nacp_name(const unsigned char* nacp, size_t size, char* name, size_t name_size);
/* The title ID of a program without an NPDM (an NRO, an ExeFS without main.npdm): none of its own */
#define SNX_NPDM_DEFAULT_TITLE (((SnxU64)0x01000000UL << 32) | 0x1000)
/* The title ID a main.npdm's ACI0 gives its program, as the loader reads it, for one not loaded; SNX_NPDM_DEFAULT_TITLE
   for none */
SnxU64 snx_npdm_title(const unsigned char* npdm, size_t size);
/* id as a game's folder and files are named, 16 hex digits into out (17 bytes): 1, or 0 for an ID that is none of the
   game's own (0, SNX_NPDM_DEFAULT_TITLE) */
int snx_title_text(SnxU64 id, char* out);
#endif
