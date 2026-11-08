/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The drivers' common options (common/utilities/cli.h): both forms of a value, the short letters, the older names
   said once, a missing value and an option a driver does not take refused in one wording, --update given again,
   the joined arguments of a runtime that reads only the = form, and the help lines, the same for every mask */
#include "common/utilities/cli.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    int failures;
    char said[16][256]; /* the error lines snx_error said, the renamed options' */
    int said_count;
} g_test;

#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            ++g_test.failures;                                                                                         \
        }                                                                                                              \
    } while (0)

static void said(const char* line) {
    if (g_test.said_count < 16) snprintf(g_test.said[g_test.said_count++], sizeof g_test.said[0], "%s", line);
}

#define ALL (SNX_CLI_BIT(SNX_CLI_COUNT) - 2u)

/* the arguments of one match, NULL-ended: its result, *i after it and the arg */
static int match(unsigned accepted, int* i, SnxCliArg* arg, char* error, size_t size, char** argv) {
    int argc = 0;
    while (argv[argc]) ++argc;
    *i = 0;
    error[0] = 0;
    return snx_cli_match(argc, argv, i, accepted, "scarletnx-test", arg, error, size);
}

/* the help of a mask, into out */
static void help(unsigned accepted, char* out, size_t size) {
    FILE* file = tmpfile();
    size_t n = 0;
    if (file) {
        snx_cli_help(file, accepted);
        rewind(file);
        n = fread(out, 1, size - 1, file);
        fclose(file);
    }
    out[n] = 0;
}

int main(void) {
    char error[256];
    SnxCliArg arg;
    int i;
    snx_error_sink(said);
    /* both forms of each value option, the value apart taking the next argument */
    {
        char *a[] = {"--data", "dir", NULL}, *b[] = {"--data=dir", NULL};
        CHECK(match(ALL, &i, &arg, error, sizeof error, a) == SNX_CLI_DATA && i == 1 && !strcmp(arg.value, "dir"),
              "--data dir: %s", error);
        CHECK(match(ALL, &i, &arg, error, sizeof error, b) == SNX_CLI_DATA && i == 0 && !strcmp(arg.value, "dir"),
              "--data=dir: %s", error);
    }
    {
        char *a[] = {"--seconds", "2.5", NULL}, *b[] = {"--seconds=0", NULL}, *c[] = {"--frames=12", NULL};
        CHECK(match(ALL, &i, &arg, error, sizeof error, a) == SNX_CLI_SECONDS && arg.number == 2.5, "--seconds 2.5");
        CHECK(match(ALL, &i, &arg, error, sizeof error, b) == SNX_CLI_ERROR &&
                  !strcmp(error, "scarletnx-test: --seconds requires a positive number, not 0"),
              "--seconds=0: %s", error);
        CHECK(match(ALL, &i, &arg, error, sizeof error, c) == SNX_CLI_FRAMES && arg.number == 12, "--frames=12");
    }
    /* the short letters, and the flags that take no value but --fullscreen's 0 or 1 */
    {
        char *x[] = {"-x", NULL}, *f[] = {"-f", NULL}, *f0[] = {"--fullscreen=0", NULL};
        char* f2[] = {"--fullscreen=2", NULL};
        char *h[] = {"--headless=1", NULL}, *rx[] = {"-rx", NULL}, *own[] = {"--model", "old", NULL};
        CHECK(match(ALL, &i, &arg, error, sizeof error, x) == SNX_CLI_EXTRACT && !arg.value, "-x");
        CHECK(match(ALL, &i, &arg, error, sizeof error, f) == SNX_CLI_FULLSCREEN && !strcmp(arg.value, "1"), "-f");
        CHECK(match(ALL, &i, &arg, error, sizeof error, f0) == SNX_CLI_FULLSCREEN && !strcmp(arg.value, "0"),
              "--fullscreen=0");
        CHECK(match(ALL, &i, &arg, error, sizeof error, f2) == SNX_CLI_ERROR, "--fullscreen=2");
        CHECK(match(ALL, &i, &arg, error, sizeof error, h) == SNX_CLI_ERROR &&
                  !strcmp(error, "scarletnx-test: --headless takes no value"),
              "--headless=1: %s", error);
        /* a driver's own combined letters and options are its own to parse */
        CHECK(match(ALL, &i, &arg, error, sizeof error, rx) == 0, "-rx");
        CHECK(match(ALL, &i, &arg, error, sizeof error, own) == 0, "--model");
    }
    /* a missing value, and an option as the value apart, which is the value left out */
    {
        char *a[] = {"--outdir", NULL}, *b[] = {"--keys", "--data", "d", NULL}, *c[] = {"--game=", NULL};
        CHECK(match(ALL, &i, &arg, error, sizeof error, a) == SNX_CLI_ERROR &&
                  !strcmp(error, "scarletnx-test: --outdir requires a directory"),
              "--outdir: %s", error);
        CHECK(match(ALL, &i, &arg, error, sizeof error, b) == SNX_CLI_ERROR &&
                  !strcmp(error, "scarletnx-test: --keys requires a file"),
              "--keys --data: %s", error);
        CHECK(match(ALL, &i, &arg, error, sizeof error, c) == SNX_CLI_ERROR, "--game=");
    }
    /* an option the driver does not take, in one wording */
    {
        char *a[] = {"--update=u.cia", NULL}, *b[] = {"-H", NULL};
        unsigned some = SNX_CLI_BIT(SNX_CLI_DATA);
        CHECK(match(some, &i, &arg, error, sizeof error, a) == SNX_CLI_REFUSED &&
                  !strcmp(error, "scarletnx-test does not take --update"),
              "--update refused: %s", error);
        CHECK(match(some, &i, &arg, error, sizeof error, b) == SNX_CLI_REFUSED &&
                  !strcmp(error, "scarletnx-test does not take -H"),
              "-H refused: %s", error);
    }
    /* the older names, said once each and taken as the names now */
    {
        char *a[] = {"--repo", "r", NULL}, *b[] = {"--patch=p", NULL}, *c[] = {"--recomp", NULL};
        CHECK(match(ALL, &i, &arg, error, sizeof error, a) == SNX_CLI_DATA && !strcmp(arg.value, "r"), "--repo");
        CHECK(match(ALL, &i, &arg, error, sizeof error, a) == SNX_CLI_DATA, "--repo again");
        CHECK(match(ALL, &i, &arg, error, sizeof error, b) == SNX_CLI_UPDATE && !strcmp(arg.value, "p"), "--patch");
        CHECK(match(ALL, &i, &arg, error, sizeof error, c) == SNX_CLI_RECOMPILE, "--recomp");
        CHECK(g_test.said_count == 3, "%d renamed lines, not 3", g_test.said_count);
        CHECK(g_test.said_count > 0 && strstr(g_test.said[0], "--repo is --data now"), "said: %s", g_test.said[0]);
        CHECK(g_test.said_count > 1 && strstr(g_test.said[1], "--patch is --update now"), "said: %s", g_test.said[1]);
    }
    /* a run's options together: --update given again, each kept */
    {
        char* argv[] = {"prog",       "--update", "a.cia", "--update=b.cia", "-r",   "--game", "g",
                        "--fullscreen=0", "--seconds=3", "own",    NULL};
        int argc = (int)(sizeof argv / sizeof *argv) - 1, own = 0;
        SnxCli cli;
        snx_cli_init(&cli, "scarletnx-test", ALL);
        for (i = 1; i < argc; ++i) {
            int id = snx_cli_take(&cli, argc, argv, &i, error, sizeof error);
            CHECK(id >= 0, "%s: %s", argv[i], error);
            if (!id) ++own;
        }
        CHECK(cli.update_count == 2 && !strcmp(cli.updates[0], "a.cia") && !strcmp(cli.updates[1], "b.cia"),
              "%d updates", cli.update_count);
        CHECK(cli.flags == SNX_CLI_BIT(SNX_CLI_RECOMPILE), "flags %#x", cli.flags);
        CHECK(cli.fullscreen == 0 && cli.seconds == 3 && cli.frames == -1 && own == 1, "values");
        CHECK(cli.value[SNX_CLI_GAME] && !strcmp(cli.value[SNX_CLI_GAME], "g"), "--game g");
        snx_cli_free(&cli);
    }
    /* joined for a reader of the = form, up to the "--" */
    {
        char* argv[] = {"prog", "--seconds", "5", "--shot=s", "--keys", "k", "--script", "--", "--seconds", "9", NULL};
        int argc = (int)(sizeof argv / sizeof *argv) - 1, n = 0;
        unsigned ids = SNX_CLI_BIT(SNX_CLI_SECONDS) | SNX_CLI_BIT(SNX_CLI_SHOT) | SNX_CLI_BIT(SNX_CLI_SCRIPT);
        char** joined = snx_cli_joined(argc, argv, ids, &n);
        static const char* const want[] = {"prog", "--seconds=5", "--shot=s", "--keys", "k", "--script",
                                           "--",   "--seconds",   "9"};
        CHECK(n == 9 && !joined[n], "%d joined", n);
        for (int k = 0; k < n && k < 9; ++k) CHECK(!strcmp(joined[k], want[k]), "joined %d: %s", k, joined[k]);
        free(joined);
    }
    /* the help: each mask's lines are the full table's, the same words in the same column */
    {
        static char all[8192], some[8192];
        help(ALL, all, sizeof all);
        CHECK(strstr(all, "\n  -x, --extract          Extract the game"), "-x's line:\n%s", all);
        CHECK(strstr(all, "\n  --update PATH          An update"), "--update's line:\n%s", all);
        CHECK(strstr(all, "\n  -f, --fullscreen[=0|1] Start fullscreen"), "-f's line:\n%s", all);
        unsigned masks[] = {SNX_CLI_BIT(SNX_CLI_DATA) | SNX_CLI_BIT(SNX_CLI_HELP),
                            SNX_CLI_BIT(SNX_CLI_UPDATE) | SNX_CLI_BIT(SNX_CLI_KEYS) | SNX_CLI_BIT(SNX_CLI_OUTDIR),
                            SNX_CLI_BIT(SNX_CLI_EXTRACT_CONTAINER) | SNX_CLI_BIT(SNX_CLI_SCRIPT)};
        for (size_t m = 0; m < sizeof masks / sizeof *masks; ++m) {
            help(masks[m], some, sizeof some);
            int lines = 0;
            for (char* line = strtok(some, "\n"); line; line = strtok(NULL, "\n"), ++lines)
                CHECK(strstr(all, line), "mask %#x: \"%s\" is not the full table's", masks[m], line);
            CHECK(lines >= 3, "mask %#x: %d lines", masks[m], lines);
            CHECK(!strstr(some, "--seconds"), "mask %#x shows --seconds", masks[m]);
        }
    }
    snx_error_sink(NULL);
    if (g_test.failures) return 1;
    puts("cli: ok");
    return 0;
}
