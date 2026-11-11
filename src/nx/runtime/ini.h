/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A setting: its key, default, help, type, range and where its value goes (g_settings). The tables are generated from
   settings.txt (tools/settingsgen, settings.h): options.c writes the game's .ini from the tables in its sections (the
   unattended-run options' table is only read), each module reads its keys with ini_find and ini_read (ini.c), and the
   launcher lists its own and the recompiler's as the runtime lists the game's (ini_list_text) */
#ifndef SNX_INI_H
#define SNX_INI_H
#include <stddef.h>
#include <stdio.h>

enum {
    INI_INT,   /* an int at var, clamped to lo..hi */
    INI_FLOAT, /* a float at var, clamped to lo..hi */
    INI_TEXT   /* the text at var, size bytes with its NUL; then set, if any */
};
#define INI_STARTUP 1 /* read when the game starts: a reload while it runs leaves it alone (options.c's sections) */
/* the game's own, set in its menu (a control's place) or by the driver (its dump): the launcher's Settings leave it
   out */
#define INI_IN_GAME     2
/* the row does nothing in this build (the client the profiler rows need is a profiler build's): the menus show it
   greyed and pick nothing, and the runtime reads it and changes nothing. The generated tables set it for the rows a
   needs block names, resolving to 0 where the build has what they need */
#define INI_NEEDS_BUILD 4

/* a row a needs FEATURE block names: 0 where the build has FEATURE, INI_NEEDS_BUILD where it has none. One macro per
   feature, resolved by the build that compiles the table (settingsgen writes the block's feature in its place) */
#if defined(SNX_TRACY)
#define INI_NEEDS_TRACY 0
#else
#define INI_NEEDS_TRACY INI_NEEDS_BUILD
#endif

/* the systems a setting does something on, where not every one (settings.txt's only): one of these in flags, macOS
   without iOS, Linux the kernel (Android too), arm64 Linux the same on arm64 (NCE's hosts, nce_host.h). On another
   system the menus grey its row and say which (ini_only), and its value is read and written as anywhere */
#define INI_ONLY_MACOS       (1 << 8)
#define INI_ONLY_WINDOWS     (2 << 8)
#define INI_ONLY_ANDROID     (3 << 8)
#define INI_ONLY_LINUX       (4 << 8)
#define INI_ONLY_ARM64_LINUX (5 << 8)
#define INI_ONLY             (15 << 8)

typedef struct IniOpt IniOpt;
struct IniOpt {
    const char* key;  /* NULL ends a table */
    const char* def;  /* NULL: no default. A row of options.c's sections is written once set */
    const char* help; /* NULL: the help of the one before covers it */
    int type, flags;
    void* var;
    double lo, hi;
    /* INI_TEXT: reads the value. INI_INT and INI_FLOAT: called after each read into var (the raw text in v), or NULL */
    void (*set)(const IniOpt* o, const char* v);
    /* the menus' (the game's and the launcher's Settings), NULL where they show the key and the number: the label,
       and the words of lo, lo + step... between '|'. A word with '#' reads its value and the ones above it as the
       number plus shift in the '#''s place ("Off|#%"). An INI_TEXT's words are its values, value=word between '|' */
    const char* name;
    const char* words;
    int step, shift;
    size_t size; /* INI_TEXT: var's size */
};
/* a section of the .ini (a group of the launcher's Settings): up to three tables, a table with a page having a page of
   its own in the menus, listed after the section's other keys. own: the tables options.c applies itself; the others
   are applied through backend_ini, which takes the on-screen controls' lock */
typedef struct {
    const char* name;
    int own;
    const IniOpt* table[3];
    const char* page[3];
} IniSection;

const IniOpt* ini_find(const IniOpt* table, const char* key); /* key's entry in table, or NULL */
/* a command line's --key=value: its value, with its key in key (of size bytes); NULL for another argument, or for a
   key that does not fit */
const char* ini_arg(const char* arg, char* key, size_t size);
void ini_read(const IniOpt* o, const char* v); /* v into o's variable; a value that isn't a number leaves it */
/* whether o takes v as it is, for a value given where a wrong one must stop (the recompile's command line): 0, else -1
   with why in out (a number outside lo..hi, not a whole number for an INI_INT, no number at all, a text too long for
   var). ini_read clamps a number and leaves the rest, as a .ini reloaded while the game runs must */
int ini_check(const IniOpt* o, const char* v, char* out, size_t n);
/* where ini_read says a value it clamped or left (a line, without its newline): the runtime's log once options.c sets
   it, stderr until then (the launcher, the recompile) */
void ini_set_say(void (*say)(const char* line));
/* o's variable as ini_read reads it back. An INI_TEXT has none: its set keeps what it read */
void ini_format(const IniOpt* o, char* out, size_t n);
/* the settings of table whose variable is not at its default (a row without one left out), as key=value between ", ":
   with INI_CHANGED_DEFAULT each followed by " (default D)", with INI_CHANGED_NO_TEXT a text's by its key alone (a text
   is often a folder, which names the player's account). How many; out cut where it ends, NUL-terminated */
#define INI_CHANGED_DEFAULT 1
#define INI_CHANGED_NO_TEXT 2
int ini_changed(const IniOpt* table, int flags, char* out, size_t n);
/* where a setting with these flags does something, as its greyed row says it ("macOS only"): NULL for one that does
   on every system. The words are marked for scripts/build-languages.py: the launcher shows them in its language */
const char* ini_only_words(int flags);
/* ini_only_words on a system the setting does nothing on, this one: NULL where it does something here */
const char* ini_only(const IniOpt* o);
/* an INI_INT's value v as the menus read it: its word, or the number */
void ini_word(const IniOpt* o, int v, char* out, size_t n);
/* v a step down (dir -1) or up (1): words go round, numbers stop at lo and hi */
int ini_step(const IniOpt* o, int v, int dir);
/* the sections' settings but the INI_IN_GAME ones as the launcher reads them (gui/settings.h), a line each: section
   [/page], key, default, int | float | text, lowest, highest, help, name, the values value=word between '|', then
   ini_only's words (empty where the setting does something on this system), tabs between. Into out, NUL-terminated
   as it fits; the length it needs, without the NUL */
size_t ini_list_text(const IniSection* sections, int count, char* out, size_t size);

/* a key an older version used, which does nothing now (docs/user/deprecations.md): what became of it, NULL for another */
const char* ini_deprecated(const char* key);
/* a deprecated key whose values went as they are to another key: that key, NULL for another. A player's pick of it
   (the launcher's Settings) is carried over; the game's .ini also holds defaults an older version wrote, which the
   runtime reads as their own (options.c) */
const char* ini_renamed(const char* key);
/* a deprecated key's value, this run's (--key=value) else the file's, which the runtime maps to what replaced it:
   NULL when neither has it */
const char* ini_deprecated_value(const char* key);
/* the game's .ini's [Deprecated] section, its last: its head, then each key under what became of it. The launcher's
   Clean Old Settings takes it from its head to the next section or the end */
void ini_deprecated_head(FILE* f);
void ini_deprecated_key(FILE* f, const char* key, const char* value);
/* the line a run says about the n deprecated keys path has into out: 1, or 0 for none */
int ini_deprecated_note(char* out, size_t size, const char* path, const char* const* keys, int n);

#endif
