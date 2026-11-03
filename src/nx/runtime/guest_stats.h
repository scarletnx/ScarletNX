/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the game's threads cost the host, in lines of the log at the renderer's cadence (its first present, then every
   600): each guest thread's time on the host's processors and the whole process's, the SVCs by number, and the calls
   the dispatch table had no code for (RtAPI.miss). The lines are the same whichever way the game's code runs, so a run
   of each compares line by line. Each guest thread adds to counters of its own (by kt_ix), which no other thread
   writes, so the paths that count stay as uncontended as they were; the log sums them */
#ifndef SNX_GUEST_STATS_H
#define SNX_GUEST_STATS_H
#include "common/cpu/aarch64/cpu.h"
#include "common/runtime/interface.h"

/* the counters' slots: a guest thread's by its kt_ix (1 to KTHREAD_MAX), 0 for a host thread that makes an SVC */
#define GSTAT_SLOTS 257
/* the SVC numbers counted apart: those of RtAPI.svcs, and one for every number past them */
#define GSTAT_SVCS  (SNX_SVC_COUNT + 1)
enum {
    GSTAT_MISS, /* RtAPI.miss: a call that the dispatch table had no code for, which the JIT then looks up */
    GSTAT_COUNTS
};

/* thread ix made SVC #imm */
void guest_stats_svc(u32 ix, u32 imm);
/* thread ix did one of GSTAT_* once */
void guest_stats_add(u32 ix, unsigned what);
/* the calling host thread runs guest thread ix from here: its time on the processors counts from now. Again for ix,
   a later thread of that slot */
void guest_stats_thread_start(u32 ix);
/* from the calling host thread, which ran guest thread ix, as it ends: its time so far goes into the window */
void guest_stats_thread_end(u32 ix);

/* one window: from the call before to this one */
typedef struct {
    u64 ns, presents; /* its length, and the presents in it */
    u64 process_ns;   /* the whole process's time on the processors */
    u64 cpu_ns;       /* the game's threads', thread_ns summed */
    /* the game's threads that ran in it, and how many of them the system gives no time on the processors for */
    unsigned threads, no_clock;
    u64 thread_ns[GSTAT_SLOTS]; /* each slot's threads' time on the processors */
    u64 svc[GSTAT_SVCS], svcs;  /* the SVCs by number, and all of them */
    u64 count[GSTAT_COUNTS];
} GstatWindow;
/* the window that ends at now_ns, after presents presents in all, into *w, and the next one starts there: 0, or -1
   for the first call (or one at the same now_ns), which only starts one */
int guest_stats_window(u64 now_ns, u64 presents, GstatWindow* w);
/* the window's lines, from the renderer at its first present and every 600 after */
void guest_stats_log(u64 now_ns, u64 presents);
#endif
