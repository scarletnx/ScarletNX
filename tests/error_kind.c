/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The kinds of error and their actions (common/utilities/error_kind.c): every kind's lines fit the narrowest panel, the
   actions a kind lists are those its error can do, the one that fixes it first and Details and Close last; the record's
   file round trips, refuses a key or a kind it does not know and a value past its field; the run's "error: " lines */
#include "common/utilities/error_kind.h"
#include "common/utilities/common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int listed(const int* list, int n, int action) {
    for (int i = 0; i < n; ++i)
        if (list[i] == action) return 1;
    return 0;
}

int main(int argc, char** argv) {
    int out[SNX_ERROR_ACTIONS_MAX], n;
    char text[512];
    const char* dir = argc > 1 ? argv[1] : ".";
    /* every kind: a name that reads back, a line and a second line within the panel's width, ASCII */
    for (int k = 0; k < SNX_ERROR_KINDS; ++k) {
        SnxError e = {.kind = k};
        const char* line = snx_error_line(&e);
        const char* hint = snx_error_hint(&e);
        if (strlen(line) > SNX_ERROR_LINE_COLS || strlen(hint) > SNX_ERROR_LINE_COLS)
            fprintf(stderr, "kind %s: \"%s\" / \"%s\" past %d columns\n", snx_error_kind_name(k), line, hint,
                    SNX_ERROR_LINE_COLS);
        assert(strlen(line) <= SNX_ERROR_LINE_COLS && strlen(hint) <= SNX_ERROR_LINE_COLS && line[0] && hint[0]);
        for (const char* c = line; *c; ++c) assert((unsigned char)*c >= 0x20 && (unsigned char)*c < 0x7F);
        assert(snx_error_kind_of(snx_error_kind_name(k)) == k);
        /* everything an error can carry: the kind's own actions, then Details and Close */
        snprintf(e.key, sizeof e.key, "hooks");
        snprintf(e.program, sizeof e.program, "/games/G/G");
        snprintf(e.folder, sizeof e.folder, "/games/G");
        snprintf(e.log, sizeof e.log, "/games/G/G.log");
        n = snx_error_actions(&e, out);
        assert(n >= 2 && n <= SNX_ERROR_ACTIONS_MAX);
        assert(out[n - 2] == SNX_ACTION_DETAILS && out[n - 1] == SNX_ACTION_CLOSE);
        for (int i = 0; i < n; ++i) assert(out[i] > SNX_ACTION_NONE && out[i] < SNX_ACTION_OPEN_LOG);
    }
    assert(snx_error_kind_of("no-such") == -1 && !strcmp(snx_error_kind_name(SNX_ERROR_KINDS), "?"));
    /* the fix first, and only what the error can do: Open Settings needs the key, Share Log the log */
    {
        SnxError e = {.kind = SNX_ERROR_SETTING, .key = "hooks", .log = "/l.log"};
        n = snx_error_actions(&e, out);
        assert(out[0] == SNX_ACTION_OPEN_SETTINGS && listed(out, n, SNX_ACTION_SHARE_LOG));
        assert(!listed(out, n, SNX_ACTION_TRY_AGAIN)); /* nothing to run again: no program, no launcher */
        e.key[0] = 0;
        e.log[0] = 0;
        n = snx_error_actions(&e, out);
        assert(n == 2 && out[0] == SNX_ACTION_DETAILS);
        e.from_launcher = 1;
        n = snx_error_actions(&e, out);
        assert(n == 3 && out[0] == SNX_ACTION_TRY_AGAIN);
        assert(!strcmp(snx_error_action_label(SNX_ACTION_CLOSE, &e), "Back to Menu"));
        e.from_launcher = 0;
        assert(!strcmp(snx_error_action_label(SNX_ACTION_CLOSE, &e), "Close"));
    }
    {
        SnxError e = {.kind = SNX_ERROR_GPU_LOST, .program = "/g/G", .log = "/g/G.log"};
        n = snx_error_actions(&e, out);
        assert(n == 5 && out[0] == SNX_ACTION_TRY_AGAIN && out[1] == SNX_ACTION_GPU_DRIVERS &&
               out[2] == SNX_ACTION_SHARE_LOG);
    }
    /* a line of its own over the kind's */
    {
        SnxError e = {.kind = SNX_ERROR_SETTING, .line = "Native hooks is off."};
        assert(!strcmp(snx_error_line(&e), "Native hooks is off.") &&
               !strcmp(snx_error_hint(&e), "Turn it on: Open Settings."));
    }
    assert(snx_error_kind_of_message("vulkan: out of memory for command buffers") == SNX_ERROR_MEMORY);
    assert(snx_error_kind_of_message("nvn: no memory for 3 subroutine programs") == SNX_ERROR_MEMORY);
    assert(snx_error_kind_of_message("vulkan: cannot load the ASTC shader") == SNX_ERROR_INTERNAL);
    /* the record: written, read back the same; a line break in a value is a space */
    {
        SnxError e = {.kind = SNX_ERROR_SAVE,
                      .from_launcher = 1,
                      .key = "k",
                      .message = "two\nlines",
                      .game = "A GAME",
                      .program = "/p q/G",
                      .folder = "/p q",
                      .log = "/p q/G.log"},
                 back;
        char path[1100], why[1400];
        snprintf(path, sizeof path, "%s/error-kind-test.txt", dir);
        assert(!snx_error_write(&e, path, why, sizeof why));
        if (snx_error_read(&back, path, why, sizeof why)) fprintf(stderr, "the record did not read back: %s\n", why);
        assert(!snx_error_read(&back, path, why, sizeof why));
        assert(back.kind == SNX_ERROR_SAVE && back.from_launcher == 1 && !strcmp(back.message, "two lines") &&
               !strcmp(back.program, "/p q/G") && !strcmp(back.log, "/p q/G.log") && !strcmp(back.game, "A GAME"));
        /* refused, each with its name and value */
        static const struct {
            const char *text, *said;
        } bad[] = {
            {"kind=sideways\n", "kind=sideways is no kind of error"},
            {"colour=red\n", "\"colour=red\" is no key"},
            {"from_launcher=2\n", "from_launcher=2 is no 0 or 1"},
        };
        for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
            assert(!snx_write_file(path, bad[i].text, strlen(bad[i].text)));
            why[0] = 0;
            assert(snx_error_read(&back, path, why, sizeof why));
            if (!strstr(why, bad[i].said)) fprintf(stderr, "case %zu: %s\n", i, why);
            assert(strstr(why, bad[i].said));
        }
        {
            char longer[400] = "key=";
            memset(longer + 4, 'k', 100);
            strcpy(longer + 104, "\n");
            assert(!snx_write_file(path, longer, strlen(longer)));
            assert(snx_error_read(&back, path, why, sizeof why) && strstr(why, "key is 100 bytes, past the 63"));
        }
        remove(path);
        assert(snx_error_read(&back, path, why, sizeof why) && strstr(why, "could not be read"));
    }
    /* the run's lines, and their reading back */
    {
        SnxError e = {.kind = SNX_ERROR_SETTING, .key = "hooks"};
        SnxErrorHeard h;
        snx_error_said(&e, 1, text, sizeof text);
        assert(!strcmp(text, "error: setting shown=1 key=hooks"));
        assert(snx_error_heard(text, &h) == 1 && h.kind == SNX_ERROR_SETTING && h.shown && !strcmp(h.key, "hooks") &&
               h.action == SNX_ACTION_NONE);
        snx_error_action_said(&e, SNX_ACTION_OPEN_SETTINGS, text, sizeof text);
        assert(!strcmp(text, "error: action open-settings key=hooks"));
        assert(snx_error_heard(text, &h) == 1 && h.kind == -1 && h.action == SNX_ACTION_OPEN_SETTINGS &&
               !strcmp(h.key, "hooks"));
        e = (SnxError){.kind = SNX_ERROR_CRASH};
        snx_error_said(&e, 0, text, sizeof text);
        assert(!strcmp(text, "error: crash shown=0") && snx_error_heard("error: crash shown=0\r\n", &h) == 1 &&
               h.kind == SNX_ERROR_CRASH && !h.shown);
        assert(snx_error_heard("log: written to x", &h) == 0);
        assert(snx_error_heard("error: sideways shown=1", &h) == -1 && strstr(h.why, "\"sideways\" is no kind"));
        assert(snx_error_heard("error: action fly", &h) == -1 && strstr(h.why, "\"fly\" is no action"));
        assert(snx_error_heard("error: crash colour=red", &h) == -1 && strstr(h.why, "\"colour=red\" is no part"));
    }
    /* the launcher's --open values */
    {
        SnxError e = {.kind = SNX_ERROR_SETTING, .key = "hooks", .program = "/g/G"};
        snx_error_open_value(&e, SNX_ACTION_OPEN_SETTINGS, text, sizeof text);
        assert(!strcmp(text, "settings:hooks"));
        snx_error_open_value(&e, SNX_ACTION_RECOMPILE, text, sizeof text);
        assert(!strcmp(text, "recompile:/g/G"));
        snx_error_open_value(&e, SNX_ACTION_SHARE_LOG, text, sizeof text);
        assert(!text[0]);
    }
    /* a console's stop: noted with its kind and said on stderr; none shown where nobody is at the window; the launcher's
       run carried */
    {
        const SnxError* n;
        assert(!snx_error_noted());
        snx_error_note(SNX_ERROR_BAD_FILE, "scarletnx-test", "%s is %d bytes", "x.gba", 3);
        n = snx_error_noted();
        assert(n && n->kind == SNX_ERROR_BAD_FILE && !strcmp(n->message, "x.gba is 3 bytes") && !n->from_launcher);
        snx_error_note_game("A GAME");
        snx_error_launched(1);
        n = snx_error_noted();
        assert(!strcmp(n->game, "A GAME") && n->from_launcher && snx_error_from_launcher());
        snx_error_note_quiet(1);
        assert(!snx_error_noted());
    }
    puts("error_kind: ok");
    return 0;
}
