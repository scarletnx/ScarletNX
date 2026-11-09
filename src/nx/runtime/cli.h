/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CLI_H
#define SNX_CLI_H
#include "common/utilities/cli.h"
#include "common/utilities/common.h"
#include <string.h>

enum {
    SNX_EXTRACT = 1,
    SNX_RECOMP = 2,
    SNX_HEADLESS = 4,
    SNX_GDB = 8,
    SNX_STOP = 16,
    SNX_FULLSCREEN = 32,
    SNX_WINDOWED = 64, /* --fullscreen=0: windowed over the .ini's fullscreen=1 */
    SNX_TMPFS = 128,   /* the driver's --tmpfs (the launcher's Extract to tmpfs), which snx_cli_flag leaves to it */
    /* the driver's --extract-container (the launcher's Extract dump): a dump played is extracted into its
       game's data folder, instead of read in place. snx_cli_flag leaves it to the driver too */
    SNX_EXTRACT_CONTAINER = 256,
    /* --tracy-capture: record this run with tracy-capture (SNX_TRACY builds only) */
    SNX_TRACY_CAPTURE = 512,
    /* --tracy: start the Tracy client for a GUI connection (SNX_TRACY builds only) */
    SNX_TRACY_CLIENT = 1024
};
/* Shared by the driver and standalone game runtime (startup.c), which refuses SNX_EXTRACT and SNX_RECOMP: only the
   driver extracts and recompiles. The long names and the single letters are the common options' (common/utilities/
   cli.h), -1 for one malformed (--headless=1, --fullscreen=2). Windows accepts individual short options, but
   deliberately not POSIX-style clusters. */
static int snx_cli_flag(const char* arg, unsigned* flags) {
    unsigned bits = 0;
    const char* p;
    char* one[1] = {(char*)arg};
    char error[160];
    int at = 0;
    SnxCliArg given;
    int common = snx_cli_match(1, one, &at,
                               SNX_CLI_BIT(SNX_CLI_EXTRACT) | SNX_CLI_BIT(SNX_CLI_RECOMPILE) |
                                   SNX_CLI_BIT(SNX_CLI_HEADLESS) | SNX_CLI_BIT(SNX_CLI_FULLSCREEN),
                               "scarletnx", &given, error, sizeof error);
    if (common == SNX_CLI_ERROR)
        return -1;
    else if (common == SNX_CLI_EXTRACT)
        bits = SNX_EXTRACT;
    else if (common == SNX_CLI_RECOMPILE)
        bits = SNX_RECOMP;
    else if (common == SNX_CLI_HEADLESS)
        bits = SNX_HEADLESS;
    else if (common == SNX_CLI_FULLSCREEN)
        bits = given.value[0] == '1' ? SNX_FULLSCREEN : SNX_WINDOWED;
    else if (!strcmp(arg, "--tracy-capture"))
        bits = SNX_TRACY_CAPTURE;
    else if (!strcmp(arg, "--tracy"))
        bits = SNX_TRACY_CLIENT;
    else if (arg[0] == '-' && arg[1] && arg[1] != '-') {
#ifdef _WIN32
        if (arg[2]) return 0;
#endif
        for (p = arg + 1; *p; ++p) {
            switch (*p) {
                case 'x': bits |= SNX_EXTRACT; break;
                case 'r': bits |= SNX_RECOMP; break;
                case 'H': bits |= SNX_HEADLESS; break;
                case 'f': bits |= SNX_FULLSCREEN; break;
#ifndef _WIN32
                case 'S': bits |= SNX_GDB; break;
                case 's': bits |= SNX_GDB | SNX_STOP; break;
#endif
                default: return 0;
            }
        }
    } else
        return 0;
    /* the last of -f and --fullscreen=0 wins */
    if (bits & SNX_FULLSCREEN) *flags &= ~(unsigned)SNX_WINDOWED;
    if (bits & SNX_WINDOWED) *flags &= ~(unsigned)SNX_FULLSCREEN;
    *flags |= bits;
    return 1;
}
#endif
