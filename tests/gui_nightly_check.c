/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gui/nightly_check.c: the newest nightly from GitHub's answer for scarletnx/nightly's latest release, its changes as a
   notice says them (by area, the links' text kept, cut past GUI_NIGHTLY_CHANGES with the rest counted), GitHub's own
   error said; and whether it is newer than the build that runs, a nightly's by its day, another's by its commit's */
#include "gui/nightly_check.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char latest_json[] =
    "{\"tag_name\": \"nightly-2026-11-27-abc1234\", \"name\": \"Nightly 2026-11-27 (abc1234)\","
    " \"html_url\": \"https://github.com/scarletnx/nightly/releases/tag/nightly-2026-11-27-abc1234\","
    " \"body\": \"Built from main at [`abc1234`](https://github.com/o/r/commit/abc1234abc1234abc1234abc1234abc1234abc1).\\n"
    "\\n## Downloads\\n\\n- **Linux**: make the AppImage executable\\n\\n"
    "## Changes since the previous nightly\\n\\n[`1111111...abc1234`](https://x): 3 pull requests, by area.\\n\\n"
    "**gpu**\\n\\n- Keep a dispatch out of the mirror ([#2879](https://github.com/o/r/pull/2879), 2 commits, by a)\\n\\n"
    "**vk**\\n\\n- A \\\\[bracket\\\\] in a title ([#2880](https://github.com/o/r/pull/2880), 1 commit, by b)\\n\\n"
    "**Other**\\n\\n- a commit ([`1234567`](https://github.com/o/r/commit/1234567))\\n\\n"
    "<details>\\n<summary>Source and licenses</summary>\\n- not a change\\n</details>\\n\"}";

int main(void) {
    GuiNightly n;
    char why[300];
    assert(gui_nightly_parse(latest_json, sizeof latest_json - 1, &n, why, sizeof why));
    assert(!strcmp(n.tag, "nightly-2026-11-27-abc1234") && !strcmp(n.name, "Nightly 2026-11-27 (abc1234)"));
    assert(!strncmp(n.url, "https://github.com/scarletnx/nightly/releases/tag/", 50));
    /* the changes by area, as plain text: the links' text kept, their targets and markdown's marks left out */
    assert(n.count == 3 && n.more == 0);
    assert(!strcmp(n.changes[0], "[gpu] Keep a dispatch out of the mirror (#2879, 2 commits, by a)"));
    assert(!strcmp(n.changes[1], "[vk] A [bracket] in a title (#2880, 1 commit, by b)"));
    assert(!strcmp(n.changes[2], "[Other] a commit (1234567)"));
    /* past GUI_NIGHTLY_CHANGES the rest counted, with those the notes' own "And N more" says */
    {
        char big[8192];
        int at =
            snprintf(big, sizeof big, "{\"tag_name\": \"nightly-2026-11-27-abc1234\", \"body\": \"x\\n## Changes\\n");
        for (int i = 0; i < GUI_NIGHTLY_CHANGES + 3; ++i)
            at += snprintf(big + at, sizeof big - (size_t)at, "- c%d\\n", i);
        snprintf(big + at, sizeof big - (size_t)at, "\\nAnd [40 more](https://x).\\n\"}");
        assert(gui_nightly_parse(big, strlen(big), &n, why, sizeof why));
        assert(n.count == GUI_NIGHTLY_CHANGES && n.more == 43 && !strcmp(n.changes[0], "c0"));
    }
    /* notes with no changes: none, still a nightly */
    {
        const char* none = "{\"tag_name\": \"nightly-2026-11-27-abc1234\", \"body\": null}";
        assert(gui_nightly_parse(none, strlen(none), &n, why, sizeof why) && n.count == 0);
    }
    /* GitHub's own error said, and an answer that is no object */
    {
        const char* limit = "{\"message\": \"API rate limit exceeded for 1.2.3.4.\"}";
        assert(!gui_nightly_parse(limit, strlen(limit), &n, why, sizeof why));
    }
    assert(strstr(why, "API rate limit exceeded") && strstr(why, "GitHub"));
    assert(!gui_nightly_parse("<html>", 6, &n, why, sizeof why) && strstr(why, "6 bytes"));
    /* newer: a nightly's build by its day, the latest of its own day too (made after it), never itself */
    assert(gui_nightly_newer("nightly-2026-11-27-abc1234", "nightly-2026-11-20", "1111111", "2026-11-19") == 1);
    assert(gui_nightly_newer("nightly-2026-11-20-abc1234", "nightly-2026-11-20", "1111111", "2026-11-19") == 1);
    assert(gui_nightly_newer("nightly-2026-11-20-abc1234", "nightly-2026-11-20", "abc1234", "2026-11-19") == 0);
    assert(gui_nightly_newer("nightly-2026-11-19-abc1234", "nightly-2026-11-20", "1111111", "2026-11-19") == 0);
    /* another build (main's, a pull request's, one's own) by its commit's day, one of its day maybe without it; its own
       commit never newer */
    assert(gui_nightly_newer("nightly-2026-11-27-abc1234", "main", "1111111", "2026-11-26") == 1);
    assert(gui_nightly_newer("nightly-2026-11-27-abc1234", "main", "1111111", "2026-11-27") == 0);
    assert(gui_nightly_newer("nightly-2026-11-27-abc1234", "main", "2222222-dirty", "2026-11-27") == 0);
    assert(gui_nightly_newer("nightly-2026-11-27-abc1234", "dravee/x", "abc1234", "2026-11-20") == 0);
    /* what cannot be said: a tag of another shape, a build with no day */
    assert(gui_nightly_newer("main-abc1234", "main", "1111111", "2026-11-26") == GUI_NIGHTLY_NO_TAG);
    assert(gui_nightly_newer("nightly", "main", "1111111", "2026-11-26") == GUI_NIGHTLY_NO_TAG);
    assert(gui_nightly_newer("nightly-2026-11-27-ABC1234", "main", "1111111", "2026-11-26") == GUI_NIGHTLY_NO_TAG);
    assert(gui_nightly_newer("nightly-2026-11-27-abc1234", "main", "unknown", "unknown") == GUI_NIGHTLY_NO_DAY);
    assert(gui_nightly_newer("nightly-2026-11-27-abc1234", "nightly-2026-11", "1111111", "unknown") ==
           GUI_NIGHTLY_NO_DAY);
    puts("gui_nightly_check: ok");
    return 0;
}
