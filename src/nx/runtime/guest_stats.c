/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the game's threads cost the host (guest_stats.h) */
#include "nx/runtime/guest_stats.h"
#include "common/utilities/common.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/runtime/nce/nce.h"
#include "nx/runtime/platform/log.h"
#include <stdio.h>
#include <string.h>

#define GSTAT_TOP_THREADS 5  /* the busiest threads a line names */
#define GSTAT_TOP_SVCS    6  /* the SVCs a line names, the most made first */
#define GSTAT_NAME        32 /* a family's name, with its 0 */
typedef char snx_check_gstat_top[GSTAT_TOP_THREADS <= GSTAT_FAMILIES && GSTAT_TOP_SVCS <= GSTAT_FAMILIES ? 1 : -1];

/* a slot's counters, which only its thread adds to (slot 0: any host thread's) */
typedef struct {
    u64 svc[GSTAT_SVCS];
    u64 count[GSTAT_COUNTS];
    u64 hook[GSTAT_FAMILIES];
    char pad[64]; /* the next slot's counters on another cache line, wherever the array starts */
} GstatCounters;

/* a slot's time on the processors, under the lock */
typedef struct {
    u64 clock; /* its thread's (snx_thread_clock), 0 for none */
    u64 base;  /* its thread's time where the window began, 0 for a thread that began in it */
    u64 ended; /* the time in the window of the slot's threads that ended in it */
    int live;  /* a thread runs in the slot (guest_stats_thread_start), with a clock or not */
} GstatClock;

static struct {
    GstatCounters slot[GSTAT_SLOTS];
    PMutex lock; /* the clocks, the families, and the window */
    GstatClock clock[GSTAT_SLOTS];
    char family[GSTAT_FAMILIES][GSTAT_NAME];
    unsigned families;
    /* where the window began: when, the presents then, the process's time then, and the counters' sums then */
    u64 since_ns, since_presents, since_process_ns;
    u64 svc[GSTAT_SVCS], count[GSTAT_COUNTS], hook[GSTAT_FAMILIES];
} g_guest_stats = {.lock = PMUTEX_INIT};

/* one more in a counter of slot ix. Only its own thread writes a guest thread's slot, so a load and a store do, with no
   locked add on the paths that count; slot 0's are any host thread's */
static void bump(u32 ix, u64* n) {
    if (ix)
        __atomic_store_n(n, __atomic_load_n(n, __ATOMIC_RELAXED) + 1, __ATOMIC_RELAXED);
    else
        __atomic_add_fetch(n, 1, __ATOMIC_RELAXED);
}
void guest_stats_svc(u32 ix, u32 imm) {
    bump(ix, &g_guest_stats.slot[ix].svc[imm < SNX_SVC_COUNT ? imm : SNX_SVC_COUNT]);
}
void guest_stats_add(u32 ix, unsigned what) { bump(ix, &g_guest_stats.slot[ix].count[what]); }
void guest_stats_hook(u32 ix, unsigned family) { bump(ix, &g_guest_stats.slot[ix].hook[family]); }
unsigned guest_stats_family(const char* name) {
    unsigned i;
    pm_lock(&g_guest_stats.lock);
    for (i = 0; i < g_guest_stats.families && strcmp(g_guest_stats.family[i], name); i++) {}
    if (i == g_guest_stats.families && i < GSTAT_FAMILIES) {
        snprintf(g_guest_stats.family[i], GSTAT_NAME, "%s", i < GSTAT_FAMILIES - 1 ? name : "others");
        g_guest_stats.families++;
    }
    if (i == GSTAT_FAMILIES) i = GSTAT_FAMILIES - 1;
    pm_unlock(&g_guest_stats.lock);
    return i;
}
const char* guest_stats_family_name(unsigned family) { return g_guest_stats.family[family]; }

/* the time slot ix's thread has run since the slot's base, which moves to now; 0 for a thread whose clock no longer
   reads (it ended without guest_stats_thread_end), whose clock then goes, said */
static u64 clock_take(u32 ix) {
    GstatClock* c = &g_guest_stats.clock[ix];
    u64 ns;
    if (!c->clock) return 0;
    if (snx_thread_cpu_ns(c->clock, &ns)) {
        LOG("cpu: the clock of the thread in slot %u reads no more (it ended without guest_stats_thread_end): "
            "its %.2f s on the processors up to the window's start count, and what it ran after them does not\n",
            ix, (double)c->base / 1e9);
        snx_thread_clock_close(c->clock);
        c->clock = 0;
        c->live = 0;
        return 0;
    }
    if (ns < c->base) ns = c->base;
    ns -= c->base;
    c->base += ns;
    return ns;
}

void guest_stats_thread_start(u32 ix) {
    GstatClock* c = &g_guest_stats.clock[ix];
    u64 clock = snx_thread_clock();
    pm_lock(&g_guest_stats.lock);
    if (c->live)
        LOG("cpu: the thread before in slot %u ended without guest_stats_thread_end: its %.2f s on the processors up "
            "to the window's start count, and what it ran after them does not\n",
            ix, (double)c->base / 1e9);
    if (c->clock) snx_thread_clock_close(c->clock);
    c->clock = clock;
    c->base = 0;
    c->live = 1;
    pm_unlock(&g_guest_stats.lock);
}
void guest_stats_thread_end(u32 ix) {
    GstatClock* c = &g_guest_stats.clock[ix];
    pm_lock(&g_guest_stats.lock);
    c->ended += clock_take(ix);
    if (c->clock) snx_thread_clock_close(c->clock);
    c->clock = 0;
    c->base = 0;
    c->live = 0;
    pm_unlock(&g_guest_stats.lock);
}

/* the counters' sums now: each slot's, read as their threads go on adding */
static void sums(u64 svc[GSTAT_SVCS], u64 count[GSTAT_COUNTS], u64 hook[GSTAT_FAMILIES]) {
    memset(svc, 0, GSTAT_SVCS * sizeof *svc);
    memset(count, 0, GSTAT_COUNTS * sizeof *count);
    memset(hook, 0, GSTAT_FAMILIES * sizeof *hook);
    for (u32 ix = 0; ix < GSTAT_SLOTS; ix++) {
        const GstatCounters* s = &g_guest_stats.slot[ix];
        for (u32 i = 0; i < GSTAT_SVCS; i++) svc[i] += __atomic_load_n(&s->svc[i], __ATOMIC_RELAXED);
        for (u32 i = 0; i < GSTAT_COUNTS; i++) count[i] += __atomic_load_n(&s->count[i], __ATOMIC_RELAXED);
        for (u32 i = 0; i < GSTAT_FAMILIES; i++) hook[i] += __atomic_load_n(&s->hook[i], __ATOMIC_RELAXED);
    }
}

int guest_stats_window(u64 now_ns, u64 presents, GstatWindow* w) {
    u64 svc[GSTAT_SVCS], count[GSTAT_COUNTS], hook[GSTAT_FAMILIES], process_ns = snx_process_cpu_ns();
    int first;
    memset(w, 0, sizeof *w);
    sums(svc, count, hook);
    pm_lock(&g_guest_stats.lock);
    first = !g_guest_stats.since_ns || now_ns <= g_guest_stats.since_ns;
    for (u32 ix = 0; ix < GSTAT_SLOTS; ix++) {
        GstatClock* c = &g_guest_stats.clock[ix];
        int live = c->live;
        u64 ns = clock_take(ix) + c->ended;
        c->ended = 0;
        if (first) continue;
        w->thread_ns[ix] = ns;
        w->cpu_ns += ns;
        if (ns || live) w->threads++;
        if (c->live && !c->clock) w->no_clock++; /* not one whose clock went in clock_take */
    }
    if (!first) {
        w->ns = now_ns - g_guest_stats.since_ns;
        w->presents = presents - g_guest_stats.since_presents;
        w->process_ns = process_ns - g_guest_stats.since_process_ns;
        for (u32 i = 0; i < GSTAT_SVCS; i++) w->svcs += w->svc[i] = svc[i] - g_guest_stats.svc[i];
        for (u32 i = 0; i < GSTAT_COUNTS; i++) w->count[i] = count[i] - g_guest_stats.count[i];
        for (u32 i = 0; i < GSTAT_FAMILIES; i++) w->hooks += w->hook[i] = hook[i] - g_guest_stats.hook[i];
        w->families = g_guest_stats.families;
    }
    g_guest_stats.since_ns = now_ns;
    g_guest_stats.since_presents = presents;
    g_guest_stats.since_process_ns = process_ns;
    memcpy(g_guest_stats.svc, svc, sizeof svc);
    memcpy(g_guest_stats.count, count, sizeof count);
    memcpy(g_guest_stats.hook, hook, sizeof hook);
    pm_unlock(&g_guest_stats.lock);
    return first ? -1 : 0;
}

/* the n largest of v[0..count) that are not 0, their indices into top, the largest first; how many */
static unsigned largest(const u64* v, u32 count, u32* top, unsigned n) {
    unsigned k = 0;
    for (u32 i = 0; i < count; i++) {
        unsigned j;
        if (!v[i] || (k == n && v[i] <= v[top[n - 1]])) continue;
        j = k < n ? k++ : n - 1;
        for (; j > 0 && v[top[j - 1]] < v[i]; j--) top[j] = top[j - 1];
        top[j] = i;
    }
    return k;
}

/* a time in percent of the window's (never 0: guest_stats_window), and in s */
static double percent(u64 ns, u64 of) { return (double)ns * 100.0 / (double)of; }
static double seconds(u64 ns) { return (double)ns / 1e9; }

void guest_stats_log(u64 now_ns, u64 presents) {
    GstatWindow w;
    u32 top[GSTAT_FAMILIES]; /* the longest list a line gives */
    char list[640], per[32], clockless[96] = "";
    size_t at = 0;
    unsigned n;
    if (guest_stats_window(now_ns, presents, &w)) return;
    n = largest(w.thread_ns, GSTAT_SLOTS, top, GSTAT_TOP_THREADS);
    for (unsigned i = 0; i < n && at < sizeof list; i++) {
        char name[64];
        u64 id = kernel_thread_name(top[i], name, (int)sizeof name);
        at += (size_t)snprintf(list + at, sizeof list - at, "%s thread %llu%s%s%s %.2f s", i ? "," : "",
                               (unsigned long long)id, name[0] ? " '" : "", name, name[0] ? "'" : "",
                               seconds(w.thread_ns[top[i]]));
    }
    if (!n) snprintf(list, sizeof list, " none");
    if (w.presents)
        snprintf(per, sizeof per, "%.2f ms", (double)w.cpu_ns / 1e6 / (double)w.presents);
    else
        snprintf(per, sizeof per, "? ms");
    if (w.no_clock)
        snprintf(clockless, sizeof clockless, ", %u of them on a system that gives no thread's time", w.no_clock);
    LOG("cpu: %llu presents in %.2f s (%.1f fps): %u of the game's threads%s ran %.2f s on the host's processors "
        "(%.0f%% of one, %s a present), the whole process %.2f s (%.0f%%); the busiest:%s\n",
        (unsigned long long)w.presents, seconds(w.ns), (double)w.presents * 1e9 / (double)w.ns, w.threads, clockless,
        seconds(w.cpu_ns), percent(w.cpu_ns, w.ns), per, seconds(w.process_ns), percent(w.process_ns, w.ns), list);
    at = 0;
    n = largest(w.svc, GSTAT_SVCS, top, GSTAT_TOP_SVCS);
    for (unsigned i = 0; i < n && at < sizeof list; i++) {
        const char* name = top[i] < SNX_SVC_COUNT ? svc_name(top[i]) : NULL;
        char number[16];
        if (top[i] < SNX_SVC_COUNT)
            snprintf(number, sizeof number, "0x%02x", (unsigned)top[i]);
        else
            snprintf(number, sizeof number, "past 0x%02x", SNX_SVC_COUNT - 1);
        at += (size_t)snprintf(list + at, sizeof list - at, "%s %s%s%s %llu", i ? "," : "", number, name ? " " : "",
                               name ? name : "", (unsigned long long)w.svc[top[i]]);
    }
    if (!n) snprintf(list, sizeof list, " none");
    LOG("cpu: in those %.2f s, %llu SVCs (%.0f a second):%s; %llu calls the dispatch table had no code for "
        "(RtAPI.miss)\n",
        seconds(w.ns), (unsigned long long)w.svcs, (double)w.svcs * 1e9 / (double)w.ns, list,
        (unsigned long long)w.count[GSTAT_MISS]);
    if (!nce_on()) return;
    at = 0;
    for (unsigned i = 0, k = largest(w.hook, w.families, top, GSTAT_FAMILIES); i < k && at < sizeof list; i++)
        at += (size_t)snprintf(list + at, sizeof list - at, "%s %s %llu", i ? "," : "", guest_stats_family_name(top[i]),
                               (unsigned long long)w.hook[top[i]]);
    if (!at) snprintf(list, sizeof list, " none");
    {
        u64 outcalls = w.count[GSTAT_NCE_SVC] + w.count[GSTAT_NCE_TICKS] + w.hooks + w.count[GSTAT_NCE_TRAP];
        LOG("nce: in those %.2f s, %llu outcalls (%.0f a second): %llu SVCs, %llu counter reads, %llu calls of hooked "
            "functions (by family:%s), %llu returns of native runs; %llu native runs started, %llu kicks\n",
            seconds(w.ns), (unsigned long long)outcalls, (double)outcalls * 1e9 / (double)w.ns,
            (unsigned long long)w.count[GSTAT_NCE_SVC], (unsigned long long)w.count[GSTAT_NCE_TICKS],
            (unsigned long long)w.hooks, list, (unsigned long long)w.count[GSTAT_NCE_TRAP],
            (unsigned long long)w.count[GSTAT_NCE_RUN], (unsigned long long)w.count[GSTAT_NCE_KICK]);
    }
}
