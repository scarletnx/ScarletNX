/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Settings > App > GPU Drivers' Run edenprobe: edenprobe (github.com/scarletnx/edenprobe), a homebrew program of
   hardware probes, played as a game from probe/ of the state folder. Its data folder there is a portable one
   (SNX_PORTABLE_DIR), so the run finds none of the player's settings, saves or users. What the menu needs of it,
   without SDL: the paths, the card made ready for a press, and the report read (docs/EDENPROBE.md) */
#ifndef SNX_GUI_PROBE_H
#define SNX_GUI_PROBE_H
#include <stddef.h>

/* the mode file's words of edenprobe Mode's Quick and Full: never the families that read the player's network,
   accounts or hands (net, service, pad, listen), those that may end or hang the program (fatal, hang), the game scene
   or the heavy variants; no times, which differ from one machine to the next */
#define GUI_PROBE_QUICK      "quick notime cpu graphics"
#define GUI_PROBE_FULL       "quick notime cpu graphics fs video audio kernel display"
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
/* Run edenprobe's card: a new run, no report, journal or crash list of an earlier one left (so a case reported as
   crashed ended this run's program), the mode file written, full or quick; the cases the crash list it took away held
   into *cleared. 0, or -1 with why saying what and the system's reason */
int gui_probe_start(const char* folder, int full, int* cleared, char* why, size_t size);
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
