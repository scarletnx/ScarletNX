/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The common options of the drivers (cli.h): their table, the match of an argument against it, and their help */
#include "common/utilities/cli.h"
#include "common/utilities/common.h"
#include <stdlib.h>
#include <string.h>

/* a common option: its long name, its short letter (0 for none), the name an older version gave it (NULL for
   none), its value's name in the help (NULL for a flag), what a missing value is said to need, and its help, a \n
   where a line breaks */
typedef struct {
    const char* name;
    char letter;
    const char* old;
    const char* value;
    const char* needs;
    const char* help;
} CliOption;
static const CliOption options[SNX_CLI_COUNT] = {
    [SNX_CLI_EXTRACT] = {"--extract", 'x', NULL, NULL, NULL, "Extract the game, without playing it"},
    [SNX_CLI_EXTRACT_CONTAINER] = {"--extract-container", 0, NULL, NULL, NULL,
                                   "Extract the game into its folder and play it from there (without it the game\n"
                                   "is read where it lies)"},
    [SNX_CLI_RECOMPILE] = {"--recompile", 'r', "--recomp", NULL, NULL, "Recompile the game, without playing it"},
    [SNX_CLI_GAME] = {"--game", 0, NULL, "DIR", "the game's folder",
                      "Play the game in its folder DIR (recompiled/<name>), as its copy of the runtime does"},
    [SNX_CLI_UPDATE] = {"--update", 0, "--patch", "PATH", "a path",
                        "An update or a DLC of the game, played with it. Give it once for each"},
    [SNX_CLI_DATA] = {"--data", 0, "--repo", "DIR", "a directory",
                      "The data folder (default: a portable copy's folder, else the user's data folder)"},
    [SNX_CLI_OUTDIR] = {"--outdir", 0, NULL, "DIR", "a directory",
                        "The folder of the games, in which the game's folder is made (default: recompiled/\n"
                        "of the data folder)"},
    [SNX_CLI_OUTPUT] = {"--output", 0, NULL, "DIR", "a directory",
                        "The game's folder itself (default: <outdir>/<the game's name>)"},
    [SNX_CLI_KEYS] = {"--keys", 0, NULL, "FILE", "a file",
                      "The console's keys, to decrypt the game with (the platform's page says where they\n"
                      "are found without it)"},
    [SNX_CLI_SECONDS] = {"--seconds", 0, NULL, "N", "a positive number", "Stop after N seconds"},
    [SNX_CLI_FRAMES] = {"--frames", 0, NULL, "N", "a whole number", "Stop after N frames"},
    [SNX_CLI_SHOT] = {"--shot", 0, NULL, "PATH", "a path", "Save the screen to PATH when the run ends"},
    [SNX_CLI_SCRIPT] = {"--script", 0, NULL, "SPEC", "a script",
                        "Press buttons at times of the run, MS:BUTTONS,... (the platform's page names\n"
                        "the buttons)"},
    [SNX_CLI_FULLSCREEN] = {"--fullscreen", 'f', NULL, NULL, NULL, "Start fullscreen, for this run (=0: windowed)"},
    [SNX_CLI_HEADLESS] = {"--headless", 'H', NULL, NULL, NULL, "Run without a window"},
    [SNX_CLI_HELP] = {"--help", 'h', NULL, NULL, NULL, "Show this help"},
};

/* arg's length up to its '=', or all of it */
static size_t name_length(const char* arg) {
    const char* equal = strchr(arg, '=');
    return equal ? (size_t)(equal - arg) : strlen(arg);
}
/* whether arg, up to its '=', is name */
static int named(const char* arg, const char* name) {
    return name && name_length(arg) == strlen(name) && !strncmp(arg, name, strlen(name));
}
/* the id of the common option arg names, with *old set when by its older name; 0 for none */
static int find(const char* arg, int* old) {
    *old = 0;
    if (arg[0] != '-' || !arg[1]) return 0;
    for (int id = 1; id < SNX_CLI_COUNT; ++id) {
        const CliOption* o = &options[id];
        if (arg[1] != '-') {
            if (o->letter && arg[1] == o->letter && !arg[2]) return id;
        } else if (named(arg, o->name))
            return id;
        else if (named(arg, o->old)) {
            *old = 1;
            return id;
        }
    }
    return 0;
}

int snx_cli_match(int argc, char** argv, int* i, unsigned accepted, const char* program, SnxCliArg* out, char* error,
                  size_t size) {
    const char* arg = argv[*i];
    int old;
    int id = find(arg, &old);
    if (!id) return 0;
    const CliOption* o = &options[id];
    int length = (int)name_length(arg);
    if (!(accepted & SNX_CLI_BIT(id))) {
        snprintf(error, size, "%s does not take %.*s", program, length, arg);
        return SNX_CLI_REFUSED;
    }
    if (old) snx_option_renamed(o->old, o->name);
    const char* equal = arg[1] == '-' ? strchr(arg, '=') : NULL;
    *out = (SnxCliArg){(SnxCliId)id, NULL, 0};
    if (id == SNX_CLI_FULLSCREEN) {
        if (equal && strcmp(equal + 1, "0") && strcmp(equal + 1, "1")) {
            snprintf(error, size, "%s: --fullscreen takes 0 or 1, not %s", program, equal + 1);
            return SNX_CLI_ERROR;
        }
        out->value = equal && equal[1] == '0' ? "0" : "1";
        return id;
    }
    if (!o->value) {
        if (equal) {
            snprintf(error, size, "%s: %.*s takes no value", program, length, arg);
            return SNX_CLI_ERROR;
        }
        return id;
    }
    /* a value apart is the next argument, unless that is an option: the value was left out */
    const char* value = equal              ? equal + 1
                        : *i + 1 < argc && argv[*i + 1][0] != '-' ? argv[*i + 1]
                                                                   : NULL;
    if (!value || !*value) {
        snprintf(error, size, "%s: %.*s requires %s", program, length, arg, o->needs);
        return SNX_CLI_ERROR;
    }
    if (!equal) ++*i;
    out->value = value;
    if (id == SNX_CLI_SECONDS && !snx_parse_seconds(value, &out->number)) {
        snprintf(error, size, "%s: --seconds requires a positive number, not %s", program, value);
        return SNX_CLI_ERROR;
    }
    if (id == SNX_CLI_FRAMES) {
        char* end;
        long frames = strtol(value, &end, 10);
        if (*end || frames < 0) {
            snprintf(error, size, "%s: --frames requires a whole number, not %s", program, value);
            return SNX_CLI_ERROR;
        }
        out->number = (double)frames;
    }
    return id;
}

void snx_cli_init(SnxCli* cli, const char* program, unsigned accepted) {
    *cli = (SnxCli){0};
    cli->program = program;
    cli->accepted = accepted;
    cli->fullscreen = -1;
    cli->frames = -1;
}
int snx_cli_take(SnxCli* cli, int argc, char** argv, int* i, char* error, size_t size) {
    SnxCliArg arg;
    int id = snx_cli_match(argc, argv, i, cli->accepted, cli->program, &arg, error, size);
    if (id <= 0) return id;
    if (id == SNX_CLI_FULLSCREEN)
        cli->fullscreen = arg.value[0] == '1';
    else if (id == SNX_CLI_SECONDS)
        cli->seconds = arg.number;
    else if (id == SNX_CLI_FRAMES)
        cli->frames = (long)arg.number;
    else if (id == SNX_CLI_UPDATE) {
        /* at most one for each argument */
        if (!cli->updates) cli->updates = (const char**)snx_alloc((size_t)argc * sizeof *cli->updates);
        cli->updates[cli->update_count++] = arg.value;
    } else if (arg.value)
        cli->value[id] = arg.value;
    else
        cli->flags |= SNX_CLI_BIT(id);
    return id;
}
void snx_cli_free(SnxCli* cli) {
    free((void*)cli->updates);
    cli->updates = NULL;
    cli->update_count = 0;
}

void snx_cli_help(FILE* out, unsigned accepted) {
    fputs("Common options, each value given as --option VALUE or --option=VALUE:\n", out);
    for (int id = 1; id < SNX_CLI_COUNT; ++id) {
        const CliOption* o = &options[id];
        if (!(accepted & SNX_CLI_BIT(id))) continue;
        char left[64], letter[5] = "";
        if (o->letter) snprintf(letter, sizeof letter, "-%c, ", o->letter);
        snprintf(left, sizeof left, "%s%s%s%s", letter, o->name,
                 id == SNX_CLI_FULLSCREEN ? "[=0|1]"
                 : o->value               ? " "
                                          : "",
                 o->value ? o->value : "");
        fprintf(out, "  %-*s ", SNX_CLI_HELP_COLUMN - 3, left);
        for (const char* h = o->help; *h; ++h) {
            fputc(*h, out);
            if (*h == '\n') fprintf(out, "%*s", SNX_CLI_HELP_COLUMN, "");
        }
        fputc('\n', out);
    }
}

char** snx_cli_joined(int argc, char** argv, unsigned ids, int* joined_argc) {
    /* the room: argc + 1 pointers, then each joined argument's text */
    size_t text = 0;
    int end = argc, count = 0;
    for (int i = 1; i < argc; ++i) {
        int old, id = find(argv[i], &old);
        if (!strcmp(argv[i], "--")) {
            end = i;
            break;
        }
        if (id && (ids & SNX_CLI_BIT(id)) && options[id].value && !strchr(argv[i], '=') && i + 1 < argc &&
            argv[i + 1][0] != '-') {
            text += strlen(argv[i]) + 1 + strlen(argv[i + 1]) + 1;
            ++i;
        }
    }
    char** joined = (char**)snx_alloc((size_t)(argc + 1) * sizeof *joined + text);
    char* at = (char*)(joined + argc + 1);
    for (int i = 0; i < argc; ++i) {
        int old, id = i && i < end ? find(argv[i], &old) : 0;
        if (id && (ids & SNX_CLI_BIT(id)) && options[id].value && !strchr(argv[i], '=') && i + 1 < argc &&
            argv[i + 1][0] != '-') {
            joined[count++] = at;
            at += sprintf(at, "%s=%s", argv[i], argv[i + 1]) + 1;
            ++i;
        } else
            joined[count++] = argv[i];
    }
    joined[count] = NULL;
    *joined_argc = count;
    return joined;
}
