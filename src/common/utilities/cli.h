/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The options every driver of the program shares (docs/user/usage.md, "Common options"): one table of their names,
   short letters, older names, values and help lines. A driver gives the mask of the ones it can do, matches each
   argument against the table first and parses its own options after. Each value option takes its value as
   --option VALUE or --option=VALUE, an older name is said once with its name now, and a common option the driver
   does not take is refused in the same words by every driver */
#ifndef SNX_COMMON_CLI_H
#define SNX_COMMON_CLI_H
#include <stddef.h>
#include <stdio.h>

typedef enum {
    SNX_CLI_EXTRACT = 1,       /* -x, --extract */
    SNX_CLI_EXTRACT_CONTAINER, /* --extract-container */
    SNX_CLI_RECOMPILE,         /* -r, --recompile (--recomp) */
    SNX_CLI_GAME,              /* --game DIR */
    SNX_CLI_UPDATE,            /* --update PATH (--patch), given once for each */
    SNX_CLI_DATA,              /* --data DIR (--repo) */
    SNX_CLI_OUTDIR,            /* --outdir DIR */
    SNX_CLI_OUTPUT,            /* --output DIR */
    SNX_CLI_KEYS,              /* --keys FILE */
    SNX_CLI_SECONDS,           /* --seconds N */
    SNX_CLI_FRAMES,            /* --frames N */
    SNX_CLI_SHOT,              /* --shot PATH */
    SNX_CLI_SCRIPT,            /* --script SPEC */
    SNX_CLI_FULLSCREEN,        /* -f, --fullscreen[=0|1] */
    SNX_CLI_HEADLESS,          /* -H, --headless */
    SNX_CLI_HELP,              /* -h, --help */
    SNX_CLI_COUNT
} SnxCliId;
/* an option's bit in a driver's mask of the options it takes */
#define SNX_CLI_BIT(id) (1u << (id))
/* snx_cli_match's results that are no option: an argument malformed (a value missing or wrong), and a common option
   the driver does not take. 0 is an argument that is no common option, the driver's own or a positional one */
#define SNX_CLI_ERROR   (-1)
#define SNX_CLI_REFUSED (-2)

/* a common option as given: its id, its value (the text after = or the next argument, "1" or "0" for -f and
   --fullscreen=0|1, NULL for another flag) and that value as a number for --seconds and --frames */
typedef struct {
    SnxCliId id;
    const char* value;
    double number;
} SnxCliArg;

/* argv[*i] as a common option of the driver program (its name, "scarletnx-agb", for the messages), which takes the
   options of the mask accepted. Returns its id with *out filled and *i on its last argument (the value when it is
   apart), 0 for no common option, or SNX_CLI_ERROR or SNX_CLI_REFUSED with the line to say in error ("scarletnx-agb:
   --data requires a directory", "scarletnx-agb does not take --update"). An older name is said once
   (snx_option_renamed) and taken as the name now */
int snx_cli_match(int argc, char** argv, int* i, unsigned accepted, const char* program, SnxCliArg* out, char* error,
                  size_t size);

/* the common options of one run, as a driver that takes them all through snx_cli_take collects them */
typedef struct {
    const char* program;
    unsigned accepted;
    unsigned flags;                   /* the bits of the flags given: -x, --extract-container, -r, -H, -h */
    int fullscreen;                   /* 1 or 0 for -f and --fullscreen=0|1, -1 for not given */
    double seconds;                   /* --seconds, 0 for none */
    long frames;                      /* --frames, -1 for none */
    const char* value[SNX_CLI_COUNT]; /* the last value of each path or text option: --game, --data, --shot ... */
    const char** updates;             /* each --update, as given (argv's), update_count of them */
    int update_count;
} SnxCli;
void snx_cli_init(SnxCli* cli, const char* program, unsigned accepted);
/* snx_cli_match on argv[*i], its option recorded in cli: its id, 0, SNX_CLI_ERROR or SNX_CLI_REFUSED as there */
int snx_cli_take(SnxCli* cli, int argc, char** argv, int* i, char* error, size_t size);
void snx_cli_free(SnxCli* cli);

/* the column a driver's own options' help starts at, so its lines line up with the common ones */
#define SNX_CLI_HELP_COLUMN 25
/* the help lines of the common options of the mask accepted, in the table's order, after a line that says the two
   forms of a value. Every driver's --help shows them with the same words and the same column */
void snx_cli_help(FILE* out, unsigned accepted);

/* argv with each value option of the mask ids given as --option VALUE joined into one --option=VALUE, up to a "--"
   (the rest as given): for a reader that takes only the = form, a game runtime's settings. One allocation, the
   pointers and the joined text, NULL-terminated, its count in *joined_argc; free() it */
char** snx_cli_joined(int argc, char** argv, unsigned ids, int* joined_argc);
#endif
