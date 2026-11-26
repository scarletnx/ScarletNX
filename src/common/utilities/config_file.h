/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The one rule of every settings file the project writes and reads (docs/user/settings.md, "How a settings file is
   read"): a key, a section or a line this version does not know changes nothing it reads, stays in the file, and is
   named once a run, by its name alone, as its value can be a secret. A file that is there and cannot be read is said
   with the system's reason and never written over, and a written file takes the old one's place whole or not at all */
#ifndef SNX_CONFIG_FILE_H
#define SNX_CONFIG_FILE_H
#include <stddef.h>

/* the file at path, read whole into *text (malloc'd, NUL-terminated, size bytes): 1; 0 with *text NULL for none there;
   -1 with *text NULL for one that is there and cannot be read, why "<path> cannot be read (<the system's reason>)" */
int snx_config_read(const char* path, char** text, size_t* size, char* why, size_t why_size);
/* text (size bytes) in path's place through path and suffix (".new") beside it, path's folder made: 0; -1 with path as it
   was and why "<path> could not be written (<reason>)", and for a path that is there and cannot be read, as nothing that
   was not read is written over, "<path> cannot be read (<reason>): nothing is written over it" */
int snx_config_write(const char* path, const char* suffix, const void* text, size_t size, char* why, size_t why_size);

#define SNX_CONFIG_NAMED 8 /* the keys, or lines, a reading names; the others are counted */
/* what a reading of a file left for its own reader, or another version's */
typedef struct {
    char key[SNX_CONFIG_NAMED][80];    /* "name" or "[section] name", or "line 4" for what names none */
    int line[SNX_CONFIG_NAMED];        /* the lines with no = */
    char twice[SNX_CONFIG_NAMED][160]; /* the keys given twice, as said */
    int keys, lines, twice_count;      /* how many, past SNX_CONFIG_NAMED too */
    int secret; /* set by the reader of a file of secrets: a key is named only in small letters, digits and _ */
} SnxConfigOthers;
/* a key the reader does not take, on line line of the file, in section ("" for none): its name, or its line where the
   text before the = is no key's name (letters, digits, '_', '-' and '.', 40 at most; in a file of secrets small letters,
   digits and '_', 32 at most), which could be a secret */
void snx_config_other_key(SnxConfigOthers* others, const char* section, const char* key, int line);
/* a line of it with no = */
void snx_config_other_line(SnxConfigOthers* others, int line);
/* a key of the reader's given again on line line, first on first_line: the values of both lines, or NULL for a secret's,
   which no message says */
void snx_config_twice(SnxConfigOthers* others, const char* section, const char* key, const char* first, int first_line,
                      const char* last, int line);
/* what others says of file that this run has not said yet, "" for nothing: "<file>: this version does not read a, [x] b
   and 2 more, which stay as they are; line 4 has no = and sets nothing; a is given twice, a=1 on line 2 and a=3 on line
   5: the last is taken". Once a run for each name of a file */
int snx_config_others_said(const SnxConfigOthers* others, const char* file, char* out, size_t size);
/* 1 the first time what is asked of file in this run, 0 after */
int snx_config_first(const char* file, const char* what);
/* as a run that starts: nothing said yet (a test's) */
void snx_config_forget(void);

/* a key of a file another program owns, which a reader looks at: its section, its key, its value, its line */
typedef void (*SnxConfigTake)(void* ctx, const char* section, const char* key, const char* value, int line);
/* each key of the file at path given to take, in order: 1; 0 for none there; -1 for one that cannot be read, said once
   a run (snx_error) with the system's reason. Its owner names what it does not take: a reader of some of its keys
   does not */
int snx_config_keys(const char* path, SnxConfigTake take, void* ctx);
/* whether the reader of a file takes key of section */
typedef int (*SnxConfigOwns)(void* ctx, const char* section, const char* key);
/* text's lines with a key the reader does not take, each byte for byte as it was, for the end of a file that is written
   again from what was read: top 1 those before the first section, 0 the others, each section's under its head, -1 all of
   them with no head (a file whose keys name their settings whatever their section). Owned, "" for none; NULL out of
   memory */
char* snx_config_kept(const char* text, size_t size, int top, SnxConfigOwns owns, void* ctx);
#endif
