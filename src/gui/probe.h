/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Settings > App > GPU Drivers' Run edenprobe: edenprobe (github.com/scarletnx/edenprobe), a homebrew program of
   hardware probes, played as a game from probe/ of the state folder. Its data folder there is a portable one
   (SNX_PORTABLE_DIR), so the run finds none of the player's settings, saves or users. What the menu needs of it,
   without SDL: the paths, the card made ready for a press, and the report read (docs/dev/edenprobe.md) */
#ifndef SNX_GUI_PROBE_H
#define SNX_GUI_PROBE_H
#include <stddef.h>

/* the mode file's words of edenprobe Mode's Quick and Full: never the families that read the player's network,
   accounts or hands (net, service, pad, listen), those that may end or hang the program (fatal, hang), the game scene
   or the heavy variants; no times, which differ from one machine to the next. Never edenprobe's quick either, a
   script's run, which opens no screen: the window stays black to the end. Without it the player sees edenprobe's own
   screens, its start screen (these families chosen, begun after ten seconds), the running screen, then the results,
   left by themselves after ten seconds with nothing pressed. hooks: the runtime serves none of the services of nn::fs,
   time, pl, ngc and nsd, which the probe then asks through the SDK's functions (from 0.13.0) */
#define GUI_PROBE_QUICK      "notime cpu graphics hooks"
#define GUI_PROBE_FULL       "notime cpu graphics fs video audio kernel display hooks"
/* launches after the first one when one ends before the summary, as edenprobe's own runner relaunches */
#define GUI_PROBE_RELAUNCHES 16
#define GUI_PROBE_SUITES     128 /* edenprobe 0.9.0 has 90 */

typedef struct {
    char build[64]; /* the first note's: "0.9.0 (v0.9.0)" from "edenprobe 0.9.0 (v0.9.0) starting" */
    int cases, summary;
    long passed, failed, skipped, crashed; /* the summary's */
    struct {
        char name[48];
        int failed, crashed;
    } suite[GUI_PROBE_SUITES]; /* the suites with a failure or a crash, in the report's order */
    int suites;
} GuiProbeReport;

/* in folder, the probe's: the run's data folder (folder/.scarletnx); the NRO it is made from (folder/edenprobe.nro,
   whose name names the game); the report the last launch wrote (the folder sdmc:/edenprobe/last-run names), NULL when
   there is none. Owned */
char* gui_probe_data(const char* folder);
char* gui_probe_nro(const char* folder);
char* gui_probe_report(const char* folder);
/* a new run's mode file words, full or quick, into words: with machine=scarletnx@<commit> after them when folder's NRO
   knows the word (it carries "machine="), so edenprobe's ban list skips as banned the cases that end ScarletNX every
   time, as scripts/edenprobe.py writes it; commit is the build's (snx_build_commit), left out when it is "unknown" or
   no version edenprobe takes (1 to 32 letters, digits, dots, dashes and underscores). 0, or -1 with why when the NRO
   cannot be read */
int gui_probe_words(const char* folder, int full, const char* commit, char* words, size_t size, char* why,
                    size_t why_size);
/* Run edenprobe's card: a new run, no report, journal or crash list of an earlier one left (so a case reported as
   crashed ended this run's program), the mode file holding words; the cases the crash list it took away held into
   *cleared. 0, or -1 with why saying what and the system's reason */
int gui_probe_start(const char* folder, const char* words, int* cleared, char* why, size_t size);
/* how many cases the crash list holds: those a run skips as crashed */
int gui_probe_crashes(const char* folder);
/* Clear Crash List: the crash list taken away, the cases it held into *cleared; 0, or -1 with why */
int gui_probe_clear(const char* folder, int* cleared, char* why, size_t size);
/* the report the last launch wrote: 1 with r filled, 0 when there is none */
int gui_probe_read(const char* folder, GuiProbeReport* r);
/* its counts: "edenprobe 0.9.0 (v0.9.0): 1059 passed, 2 failed (tex8k 2), 8 skipped, 0 crashed", the suites with the
   most first; without its summary, the cases it reported */
void gui_probe_said(const GuiProbeReport* r, char* out, size_t size);
#endif
