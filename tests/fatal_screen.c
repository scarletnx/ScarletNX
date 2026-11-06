/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What a fatal error's box says (runtime/platform/fatal_screen.c): the hint for each known error, the text cut into
   the box's rows at spaces, a long word cut, UTF-8 counted as characters, those in the font's bytes, and Share Log's
   file, its homes as ~ */
#include "nx/runtime/platform/fatal_screen.h"
#include "common/utilities/common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char rows[8][FATAL_ROW];

/* size bytes of lines of 128 ('a's, then a line break), the last ending in "end\n"; malloc'd */
static char* lines(size_t size) {
    char* text = (char*)malloc(size);
    size_t i;
    assert(text && size >= 4);
    memset(text, 'a', size);
    for (i = 127; i < size; i += 128) text[i] = '\n';
    memcpy(text + size - 4, "end\n", 4);
    return text;
}

int main(void) {
    int n;
    assert(strstr(fatal_hint("vulkan: the GPU device was lost (driver reset or crash)"), "driver reset it"));
    assert(strstr(fatal_hint("Unsupported instruction 0x1ee0f800 at 0x80021c4"), "does not translate"));
    assert(strstr(fatal_hint("Vulkan could not start (see the log). ScarletNX needs a GPU and driver with Vulkan 1.3."),
                  "Vulkan 1.3"));
    assert(strstr(fatal_hint("kernel: out of memory mapping 0x10000000 bytes"), "ran out of memory"));
    assert(!fatal_hint("svc 0x7f is not implemented"));

    /* at spaces, never past the width */
    n = fatal_wrap("vulkan: the GPU device was lost (driver reset or crash)", 20, rows, 8);
    assert(n == 4);
    assert(!strcmp(rows[0], "vulkan: the GPU"));
    assert(!strcmp(rows[1], "device was lost"));
    assert(!strcmp(rows[2], "(driver reset or"));
    assert(!strcmp(rows[3], "crash)"));
    n = fatal_wrap("a b", 20, rows, 8);
    assert(n == 1 && !strcmp(rows[0], "a b"));
    /* a word as wide as the row fills it, a longer one is cut */
    n = fatal_wrap("abcdefghij klm", 10, rows, 8);
    assert(n == 2 && !strcmp(rows[0], "abcdefghij") && !strcmp(rows[1], "klm"));
    n = fatal_wrap("abcdefghijklmno", 10, rows, 8);
    assert(n == 2 && !strcmp(rows[0], "abcdefghij") && !strcmp(rows[1], "klmno"));
    /* UTF-8: characters, not bytes, fill a row; one is never split */
    n = fatal_wrap("\xE2\x96\xA0\xE2\x96\xA0\xE2\x96\xA0 ab", 4, rows, 8);
    assert(n == 2 && !strcmp(rows[0], "\xE2\x96\xA0\xE2\x96\xA0\xE2\x96\xA0") && !strcmp(rows[1], "ab"));
    /* spaces at the edges and runs of them make no empty rows */
    n = fatal_wrap("   one     two   ", 3, rows, 8);
    assert(n == 2 && !strcmp(rows[0], "one") && !strcmp(rows[1], "two"));
    assert(fatal_wrap("", 10, rows, 8) == 0 && fatal_wrap("     ", 10, rows, 8) == 0);
    /* the count goes past max, the rows past it are not written */
    strcpy(rows[1], "kept");
    n = fatal_wrap("aa bb cc", 2, rows, 1);
    assert(n == 3 && !strcmp(rows[0], "aa") && !strcmp(rows[1], "kept"));
    /* the font's bytes: CP437 for what it has, '?' for the rest and for bytes that are no UTF-8 */
    {
        char out[32];
        fatal_cp437("caf\xC3\xA9 \xE2\x96\xA0 \xE6\x97\xA5", out, sizeof out);
        assert(!strcmp(out, "caf\x82 \xFE ?"));
        fatal_cp437("a\x80"
                    "b\xC3",
                    out, sizeof out);
        assert(!strcmp(out, "a?b?"));
        fatal_cp437("abcdef", out, 4);
        assert(!strcmp(out, "abc"));
    }
    /* Share Log's file, as the launcher's: the build, the error and the game's log, its last 4 MiB of a bigger one
       from a line's start; a log not there yet says so, and a folder that cannot take it fails with the path it
       tried and why; each home folder as ~ */
    {
        enum { BIG = (4 << 20) + 128, CUT = (4 << 20) + 100 }; /* the cut at a line's start, then inside one */
        char *root = snx_temp_dir(NULL), *log = snx_path_join(root, "0100ABCD12345000.log"), saved[1024], *big;
        char* text;
        size_t n = 0;
        assert(root && log && !snx_write_file(log, "[  0.10] svc: hello\n", 20));
        assert(!fatal_share_log(root, "ScarletNX main abc1234 (clang 23, Linux x86_64)",
                                "Unsupported instruction e78f1000", log, saved, sizeof saved));
        text = (char*)snx_read_file(saved, &n);
        assert(text && strstr(saved, "scarletnx.log") && !strncmp(text, "ScarletNX main abc1234", 22));
        assert(strstr(text, "\nMessage: Unsupported instruction e78f1000\n") && strstr(text, log) &&
               strstr(text, "svc: hello"));
        free(text);
        assert(!snx_remove_tree(log));
        assert(!fatal_share_log(root, "build", "message", log, saved, sizeof saved));
        text = (char*)snx_read_file(saved, &n);
        assert(text && strstr(text, "(not written yet)"));
        free(text);
        /* the last 4 MiB from a line's start: cut where a line starts, all of them */
        big = lines(BIG);
        big[99] = 'b'; /* the head that goes */
        assert(!snx_write_file(log, big, BIG));
        assert(!fatal_share_log(root, "build", "message", log, saved, sizeof saved));
        text = (char*)snx_read_file(saved, &n);
        /* the log's own part, after its header: the header names the log's path, which may hold a 'b' */
        assert(text && n < (size_t)(4 << 20) + 1000 && n >= (size_t)(4 << 20) && strstr(text, "end\n") &&
               strstr(text, " ==\n") && !strchr(strstr(text, " ==\n"), 'b'));
        free(text);
        free(big);
        /* cut inside a line: from the next one, to the log's end, nothing of the line cut */
        big = lines(CUT);
        big[110] = 'b'; /* after the cut, in the line it cuts */
        assert(!snx_write_file(log, big, CUT));
        assert(!fatal_share_log(root, "build", "message", log, saved, sizeof saved));
        text = (char*)snx_read_file(saved, &n);
        {
            const char* body = text ? strstr(text, " ==\n") : NULL;
            assert(body);
            body += 4;
            assert((size_t)(text + n - body) == CUT - 128 + 1 && !memcmp(body, big + 128, CUT - 128) &&
                   !strcmp(body + CUT - 128, "\n") && !strchr(body, 'b'));
        }
        free(text);
        free(big);
        /* a home folder's path the cut runs through goes whole, the account's name with it: "sers\Some One\" is no
           home to the tilde */
        {
            static const char home[] = "[  0.30] fs: sdmc in C:\\Users\\Some One\\AppData\\Roaming\\scarletnx\\sdmc\n";
            size_t at = (size_t)(strstr(home, "sers\\") - home), size = at + (4 << 20);
            const char* body;
            big = lines(size);
            memcpy(big, home, sizeof home - 1);
            assert(!snx_write_file(log, big, size));
            assert(!fatal_share_log(root, "build", "message", log, saved, sizeof saved));
            text = (char*)snx_read_file(saved, &n);
            body = text ? strstr(text, " ==\n") : NULL;
            assert(body && !strstr(text, "Some One") && !strstr(text, "sers\\") &&
                   !memcmp(body + 4, big + sizeof home - 1, size - (sizeof home - 1)));
            free(text);
            free(big);
        }
        /* each home folder as ~, in the game's log and in the error: the account's name, spaces and all, is the
           player's, Windows' and a POSIX system's alike */
        {
            static const char homes[] =
                "[  0.20] fs: sdmc in C:\\Users\\Some One\\AppData\\Roaming\\scarletnx\\sdmc\r\n"
                "[  0.20] fs: saves in /home/some one/.local/share/scarletnx/saves\n";
            assert(!snx_write_file(log, homes, sizeof homes - 1));
            assert(!fatal_share_log(root, "build", "Cannot open /home/some one/.local/share/scarletnx/keys", log, saved,
                                    sizeof saved));
            text = (char*)snx_read_file(saved, &n);
            assert(text && strstr(text, "\nMessage: Cannot open ~/.local/share/scarletnx/keys\n") &&
                   strstr(text, "] fs: sdmc in ~\\AppData\\Roaming\\scarletnx\\sdmc\r\n") &&
                   strstr(text, "] fs: saves in ~/.local/share/scarletnx/saves\n") && !strstr(text, "Some One") &&
                   !strstr(text, "some one"));
            free(text);
        }
        free(log);
        log = snx_path_join(root, "no/such/folder");
        text = snx_path_join("no", "such/folder"); /* in the system's separators, as the folder said is */
        assert(fatal_share_log(log, "build", "message", NULL, saved, sizeof saved) && strstr(saved, text));
        free(text);
        text = snx_path_join(log, "scarletnx.log"); /* and why after it, the system's reason */
        assert(!strncmp(saved, text, strlen(text)) && !strncmp(saved + strlen(text), ": ", 2) &&
               saved[strlen(text) + 2]);
        assert(fatal_share_log(NULL, "build", "message", NULL, saved, sizeof saved));
        free(text);
        free(log);
        assert(!snx_remove_tree(root));
        free(root);
    }
    puts("fatal screen: hints, rows, the font's bytes and Share Log's file");
    return 0;
}
