/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Run edenprobe's card and report (gui/probe.c): a new run's words, with the build's machine= when the NRO knows the
   word, and its card, the mode file holding them and no earlier report, journal or crash list; the crash list counted
   and cleared; a report's counts and suites, a line still being written, and what the box says with and without a
   summary */
#include "gui/probe.h"
#include "common/utilities/common.h"
#include <assert.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* the card of the game made in folder, and path in it */
static char* in_card(const char* folder, const char* path) {
    char* full = snx_format("%s/.scarletnx/recompiled/edenprobe/edenprobe_data/sdmc/edenprobe/%s", folder, path);
    snx_path_native(full);
    return full;
}
static void put(const char* folder, const char* path, const char* text) {
    char *full = in_card(folder, path), *dir = snx_dirname(full);
    assert(!snx_mkdirs(dir) && !snx_write_file(full, text, strlen(text)));
    free(dir);
    free(full);
}
static int there(const char* folder, const char* path) {
    char* full = in_card(folder, path);
    int is = snx_path_exists(full);
    free(full);
    return is;
}
static char* text_of(const char* folder, const char* path) {
    char* full = in_card(folder, path);
    size_t n;
    char* text = (char*)snx_read_file(full, &n);
    free(full);
    return text;
}
/* 1 when text holds word as edenprobe's mode file reads one: any case, anything not a letter between words */
static int has_word(const char* text, const char* word) {
    size_t n = strlen(word), i, at = 0;
    for (;;) {
        while (text[at] && !isalpha((unsigned char)text[at])) at++;
        if (!text[at]) return 0;
        for (i = 0; i < n && tolower((unsigned char)text[at + i]) == word[i]; i++) {}
        if (i == n && !isalpha((unsigned char)text[at + n])) return 1;
        while (isalpha((unsigned char)text[at])) at++;
    }
}
#define START   "{\"type\":\"note\",\"text\":\"edenprobe 0.9.0 (v0.9.0) starting\"}\n"
#define CASE(s) "{\"type\":\"case\",\"suite\":\"" s "\",\"name\":\"a case\",\"tier\":\"exact\",\"result\":\"pass\"}\n"
#define FAIL(s) "{\"type\":\"case\",\"suite\":\"" s "\",\"name\":\"a case\",\"tier\":\"exact\",\"result\":\"fail\"}\n"
#define CRASHED(s)                                                                                                     \
    "{\"type\":\"case\",\"suite\":\"" s "\",\"name\":\"a case\",\"tier\":\"none\",\"result\":\"skip\",\"reason\":"     \
    "\"crashed\"}\n"

int main(void) {
    char *root = snx_temp_dir(NULL), *folder, *nro, *text, why[512], said[700], words[160];
    int cleared = -1;
    GuiProbeReport r;
    assert(root);
    folder = snx_path_join(root, "probe");
    nro = gui_probe_nro(folder);

    /* a player watches the run: quick, a script's run, would keep edenprobe's window black to its end, and noprogress
       its running screen off; has_word finds them where edenprobe would */
    assert(has_word("Quick notime", "quick") && has_word("cpu,quick", "quick") && !has_word("quicker", "quick") &&
           has_word("x noprogress", "noprogress") && !has_word("progress", "noprogress"));
    assert(!has_word(GUI_PROBE_QUICK, "quick") && !has_word(GUI_PROBE_FULL, "quick"));
    assert(!has_word(GUI_PROBE_QUICK, "noprogress") && !has_word(GUI_PROBE_FULL, "noprogress"));
    /* both modes ask through the SDK's functions: the runtime serves neither fsp-srv nor time:u, and a probe that asks
       by IPC stops in sm at its start */
    assert(has_word(GUI_PROBE_QUICK, "hooks") && has_word(GUI_PROBE_FULL, "hooks"));

    /* the words: none without the NRO, said with its path; quick's or full's alone for a probe from before machine=;
       with machine=scarletnx@<the build's commit> for one that knows it, the commit left out when it is unknown or no
       version edenprobe takes */
    assert(gui_probe_words(folder, 0, "1a2b3c4", words, sizeof words, why, sizeof why) && strstr(why, nro) &&
           strstr(why, " cannot be read: "));
    assert(!snx_mkdirs(folder) && !snx_write_file(nro, "NRO0 mode", 9));
    assert(!gui_probe_words(folder, 0, "1a2b3c4", words, sizeof words, why, sizeof why) &&
           !strcmp(words, GUI_PROBE_QUICK));
    assert(!snx_write_file(nro, "NRO0 mode machine=", 18));
    assert(!gui_probe_words(folder, 0, "1a2b3c4", words, sizeof words, why, sizeof why) &&
           !strcmp(words, GUI_PROBE_QUICK " machine=scarletnx@1a2b3c4"));
    assert(!gui_probe_words(folder, 1, "1a2b3c4-dirty", words, sizeof words, why, sizeof why) &&
           !strcmp(words, GUI_PROBE_FULL " machine=scarletnx@1a2b3c4-dirty"));
    assert(!gui_probe_words(folder, 0, "unknown", words, sizeof words, why, sizeof why) &&
           !strcmp(words, GUI_PROBE_QUICK " machine=scarletnx"));
    assert(!gui_probe_words(folder, 0, "a b", words, sizeof words, why, sizeof why) &&
           !strcmp(words, GUI_PROBE_QUICK " machine=scarletnx"));
    assert(!gui_probe_words(folder, 0, "123456789012345678901234567890123", words, sizeof words, why, sizeof why) &&
           !strcmp(words, GUI_PROBE_QUICK " machine=scarletnx"));
    assert(!gui_probe_words(folder, 0, "", words, sizeof words, why, sizeof why) &&
           !strcmp(words, GUI_PROBE_QUICK " machine=scarletnx"));

    /* nothing run yet: no report; the card made, the mode file holding the words */
    assert(!gui_probe_report(folder) && !gui_probe_read(folder, &r) && !gui_probe_crashes(folder));
    assert(!gui_probe_start(folder, GUI_PROBE_QUICK " machine=scarletnx@1a2b3c4", &cleared, why, sizeof why) &&
           cleared == 0);
    text = text_of(folder, "mode");
    assert(text && !strcmp(text, GUI_PROBE_QUICK " machine=scarletnx@1a2b3c4\n"));
    free(text);

    /* a run that ended inside a case: read; its crash list counted, then cleared */
    put(folder, "last-run", "runs/v0.9.0\n");
    put(folder, "crashes", "gpu a case\nindex another case\n");
    put(folder, "runs/v0.9.0/report.jsonl", START CASE("cpu") FAIL("astc") "{\"type\":\"case\",\"suite\":\"gpu\"");
    assert(gui_probe_read(folder, &r) && !r.summary && r.cases == 2 && !strcmp(r.build, "0.9.0 (v0.9.0)"));
    gui_probe_said(&r, said, sizeof said);
    assert(!strcmp(said, "edenprobe 0.9.0 (v0.9.0): no summary, 2 cases reported"));
    assert(gui_probe_crashes(folder) == 2);
    assert(!gui_probe_clear(folder, &cleared, why, sizeof why) && cleared == 2 && !there(folder, "crashes"));
    assert(there(folder, "runs/v0.9.0/report.jsonl") && there(folder, "last-run")); /* Continue's run stays */
    assert(!gui_probe_clear(folder, &cleared, why, sizeof why) && cleared == 0);

    /* the run ended: its counts and suites, the most first, then by name; the next press starts over, fs kept */
    put(folder, "runs/v0.9.0/report.jsonl",
        START CASE("cpu") FAIL("astc") FAIL("tex8k") FAIL("tex8k") FAIL("bcn") CRASHED("index") CRASHED("blend")
            CRASHED("index") "{\"type\":\"summary\",\"passed\":1059,\"failed\":4,\"skipped\":8,\"crashed\":3}\n");
    put(folder, "fs/kept", "x");
    assert(gui_probe_read(folder, &r) && r.summary && r.cases == 8 && r.passed == 1059 && r.crashed == 3);
    gui_probe_said(&r, said, sizeof said);
    assert(!strcmp(said, "edenprobe 0.9.0 (v0.9.0): 1059 passed, 4 failed (tex8k 2, astc 1, bcn 1), 8 skipped, 3 "
                         "crashed (index 2, blend 1)"));
    /* a new run, full: no earlier report, journal or crash list; the fs family's files kept */
    put(folder, "crashes", "gpu a case\n");
    assert(!gui_probe_start(folder, GUI_PROBE_FULL, &cleared, why, sizeof why) && cleared == 1);
    assert(!there(folder, "runs") && !there(folder, "crashes") && !there(folder, "last-run") &&
           there(folder, "fs/kept"));
    assert(!gui_probe_report(folder));
    text = text_of(folder, "mode");
    assert(text && !strcmp(text, GUI_PROBE_FULL "\n"));
    free(text);

    /* a report without its first note: the build unknown, said as ? */
    put(folder, "last-run", "runs/x");
    put(folder, "runs/x/report.jsonl",
        CASE("cpu") "{\"type\":\"summary\",\"passed\":1,\"failed\":0,\"skipped\":0,\"crashed\":0}\n");
    assert(gui_probe_read(folder, &r));
    gui_probe_said(&r, said, sizeof said);
    assert(!strcmp(said, "edenprobe ?: 1 passed, 0 failed, 0 skipped, 0 crashed"));

    /* a card that cannot be made: said with the path */
    {
        char* blocked = snx_path_join(root, "a file");
        assert(!snx_write_file(blocked, "x", 1));
        assert(gui_probe_start(blocked, GUI_PROBE_QUICK, &cleared, why, sizeof why) && strstr(why, "the mode file ") &&
               strstr(why, "a file"));
        free(blocked);
    }
    assert(!snx_remove_tree(root));
    free(nro);
    free(folder);
    free(root);
    puts("gui_probe: ok");
    return 0;
}
