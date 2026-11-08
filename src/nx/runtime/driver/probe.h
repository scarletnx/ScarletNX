/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* scarletnx --probe: edenprobe (github.com/scarletnx/edenprobe), a homebrew program of hardware probes, recompiled and
   run headless in a folder of its own, with none of the player's settings, saves or users, launched again after a
   crash until its report reaches its summary, and its counts said (docs/EDENPROBE.md) */
#ifndef SNX_DRIVER_PROBE_H
#define SNX_DRIVER_PROBE_H
#include <stdio.h>

/* the mode file's words of --probe=quick and --probe=full: never the families that read the player's network,
   accounts or hands (net, service, pad, listen), the ones that may end or hang the program (fatal, hang), the game
   scene or the heavy variants; no times, which differ from one machine to the next */
#define SNX_PROBE_QUICK    "quick notime cpu graphics"
#define SNX_PROBE_FULL     "quick notime cpu graphics fs video audio kernel display"
/* launches at most: the first and 16 after a crash, as edenprobe's own runner relaunches */
#define SNX_PROBE_LAUNCHES 17
/* seconds a launch may go without a new line in the report before it is ended, as scripts/edenprobe.py waits */
#define SNX_PROBE_QUIET    600

typedef struct {
    const char* self;            /* the program that recompiles the NRO: this one */
    const char* nro;             /* edenprobe.nro */
    const char* mode;            /* the mode file's words */
    const char* folder;          /* the probe's folder, made when it is not there, kept between runs */
    const char* const* settings; /* each launch's --KEY=VALUE, NULL after the last; NULL for none */
    double quiet;                /* seconds without a new report line that end a launch */
    int launches;                /* at most */
    FILE* out;                   /* where the progress and the result go */
} SnxProbe;

/* The run. Its folder keeps the game made from the NRO (made again when the NRO or this program changes), and gets
   report.jsonl, summary.txt, recompile.log and each launch's output, launch-N.log. 0 once the report reached its
   summary, whatever its cases; 1 otherwise, said with snx_error */
int snx_probe_run(const SnxProbe* probe);
/* scarletnx --probe=quick|full [--probe-nro FILE] [--gpu_driver=ID] [--freedreno_env=TEXT]: the run in probe/ of the
   state folder. The exit code: 0, 1 for a run that did not end, 2 for options refused */
int snx_probe_main(int argc, char** argv);
#endif
