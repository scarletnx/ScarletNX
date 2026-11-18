/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL call census (gl_call_census): which OpenGL functions a game that draws through its own GL driver calls, how
   often, and from which frame, to size a GL front end over the runtime's own objects (#950). Such a driver is a module
   of the game that exports each GL entry point by name (_glDrawArrays) beside eglGetProcAddress, and the game calls
   GL through the pointers eglGetProcAddress answers, so each call goes through the function's dispatch slot. The slot
   of every _gl function of that module takes a counter, which then runs what the slot held, as nvn_loader.c takes the
   NVN functions' slots: the driver still does everything it did. eglGetProcAddress is wrapped too (hooks.hook routes
   it), so an address it answers that no export names is counted by the name the game asked for. A driver answers
   stubs there, each jumping through the current context's table to the export of its name: the game's call is the
   stub's, and the export's calls behind it are the driver's.

   A call whose return address lies in the driver is the driver's own use of its export, counted apart. The frame of a
   call is the runtime's count of frames presented when it is made (g_frames). The file, a row for each function called
   by the most calls, is written again every SNX_GL_CENSUS_EVERY frames by the thread whose call finds it due, and as
   the game ends */
#include "nx/gpu/gl/gl_census.h"
#include "common/platform/file.h"
#include "common/platform/thread.h"
#include "common/utilities/bits.h"
#include "common/utilities/common.h"
#include "nx/hle/hook/hook.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/memory/mem.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/guest_jit.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/main.h"
#include "nx/runtime/nce/nce.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

#define GL_CENSUS_SLOTS 8192 /* a power of two, over three times the ~2470 GL functions a driver exports */
#define GL_CENSUS_NAME  64   /* the longest GL name a driver exports has 49 characters */

/* a function the census took, by its entry. first and last are 1 + the frame of the game's first and last call, 0
   before one. Its counters are atomic: any thread calls it */
typedef struct {
    _Atomic u64 addr; /* 0: a free entry */
    GuestFn own;
    _Atomic u64 calls, driver_calls, frame_calls, most;
    _Atomic u32 frames, first, last;
    u32 asked;  /* named by the game's eglGetProcAddress, not by an export */
    _Atomic u32 behind; /* an export reached through the stub eglGetProcAddress answered for its name: its calls are the
                   driver's */
    char name[GL_CENSUS_NAME];
} GlCensusFn;
/* a row of the file: a function's counts as they were read, before the rows are sorted */
typedef struct {
    u64 calls, driver_calls, most;
    u32 frames, first, last, fn;
} GlCensusRow;

/* fn: GL_CENSUS_SLOTS functions by address, then as many rows, then the exports by name (each one's index in fn plus
   one, 0 for none), one block, NULL while the census is off. The lock orders the table's inserts, the slots taken and
   the file */
static struct {
    PMutex lock;
    GlCensusFn* fn;
    GuestFn getproc_own;
    u64 driver_lo, driver_hi;
    u32 taken, no_code, full, failed;
    _Atomic u32 next_write;
    /* eglGetProcAddress: names asked, answered, and at an address no export names */
    _Atomic u64 asked, answered;
    u64 unnamed;
    char driver[32];
} g_gl_census = {.lock = PMUTEX_INIT};

static u32 slot_of(u64 addr) { return (u32)((addr >> 2) * 2654435761u) & (GL_CENSUS_SLOTS - 1); }
static GlCensusFn* census_find(u64 addr) {
    GlCensusFn* fn = g_gl_census.fn;
    for (u32 n = 0, i = slot_of(addr); n < GL_CENSUS_SLOTS; n++, i = (i + 1) & (GL_CENSUS_SLOTS - 1)) {
        u64 a = atomic_load_explicit(&fn[i].addr, memory_order_acquire);
        if (a == addr) return &fn[i];
        if (!a) return NULL;
    }
    return NULL;
}

static u16* by_name(void) { return (u16*)((GlCensusRow*)(g_gl_census.fn + GL_CENSUS_SLOTS) + GL_CENSUS_SLOTS); }
static u32 name_slot(const char* name) {
    return (u32)snx_fnv1a(SNX_FNV_BASIS, name, strlen(name)) & (GL_CENSUS_SLOTS - 1);
}
/* the export the census took by name, NULL for none */
static GlCensusFn* export_named(const char* name) {
    const u16* names = by_name();
    for (u32 n = 0, i = name_slot(name); n < GL_CENSUS_SLOTS && names[i]; n++, i = (i + 1) & (GL_CENSUS_SLOTS - 1))
        if (!strcmp(g_gl_census.fn[names[i] - 1].name, name)) return &g_gl_census.fn[names[i] - 1];
    return NULL;
}

static u32 frame_now(void) { return (u32)atomic_load_explicit(&g_frames, memory_order_relaxed); }

/* the file again, with the counts as they are now. Lock held */
static void census_write(void) {
    GlCensusFn* fn = g_gl_census.fn;
    GlCensusRow* rows = (GlCensusRow*)(fn + GL_CENSUS_SLOTS);
    u32 n = 0, frames = frame_now(), i, j;
    u64 calls = 0;
    char part[sizeof g_settings.gl_call_census + 8];
    FILE* f;
    for (i = 0; i < GL_CENSUS_SLOTS; i++) {
        GlCensusRow r = {atomic_load_explicit(&fn[i].calls, memory_order_relaxed),
                         atomic_load_explicit(&fn[i].driver_calls, memory_order_relaxed),
                         atomic_load_explicit(&fn[i].most, memory_order_relaxed),
                         atomic_load_explicit(&fn[i].frames, memory_order_relaxed),
                         atomic_load_explicit(&fn[i].first, memory_order_relaxed),
                         atomic_load_explicit(&fn[i].last, memory_order_relaxed),
                         i};
        u64 now = atomic_load_explicit(&fn[i].frame_calls, memory_order_relaxed);
        if (!r.calls) continue; /* the game's calls: a function only the driver called is the driver's business */
        if (now > r.most) r.most = now; /* the frame still going */
        calls += r.calls;
        /* by calls, most first, then by name: an insertion, as a few hundred functions are called */
        for (j = n; j > 0 && (rows[j - 1].calls < r.calls ||
                              (rows[j - 1].calls == r.calls && strcmp(fn[rows[j - 1].fn].name, fn[i].name) > 0));
             j--)
            rows[j] = rows[j - 1];
        rows[j] = r;
        n++;
    }
    snprintf(part, sizeof part, "%s.part", g_settings.gl_call_census);
    f = snx_fopen(part, "wb"); /* the same bytes on every host: a tab-separated table read by tools */
    if (!f) {
        if (!g_gl_census.failed) WARN("gl census: %s can't be written: no table\n", part);
        g_gl_census.failed = 1;
        return;
    }
    fprintf(f,
            "# gl_call_census: the game's calls to the GL functions of its driver (%s): %u taken, %u called, %llu "
            "calls, %u frames\n",
            g_gl_census.driver, g_gl_census.taken, n, (unsigned long long)calls, frames);
    fprintf(f, "# eglGetProcAddress: %llu names asked, %llu answered, %llu at an address no export names\n",
            (unsigned long long)atomic_load_explicit(&g_gl_census.asked, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_gl_census.answered, memory_order_relaxed),
            (unsigned long long)g_gl_census.unnamed);
    fputs("function\tcalls\tframes\tper_frame\tmost_in_a_frame\tfirst_frame\tlast_frame\tdriver_calls\tnamed_by\n", f);
    for (i = 0; i < n; i++) {
        const GlCensusRow* r = &rows[i];
        fprintf(f, "%s\t%llu\t%u\t%.2f\t%llu\t%u\t%u\t%llu\t%s\n", fn[r->fn].name, (unsigned long long)r->calls,
                r->frames, (double)r->calls / (double)r->frames, (unsigned long long)r->most, r->first - 1, r->last - 1,
                (unsigned long long)r->driver_calls, fn[r->fn].asked ? "eglGetProcAddress" : "export");
    }
    if (fclose(f) || plat_replace_file(part, g_settings.gl_call_census)) {
        if (!g_gl_census.failed) WARN("gl census: %s can't be written: no table\n", g_settings.gl_call_census);
        g_gl_census.failed = 1;
    }
}

/* a call of a function the census took: counted, then what its slot held runs it */
static void counted(CPU* RESTRICT c, u64 entry) {
    GlCensusFn* f = census_find(entry);
    u32 now, prev;
    if (!f) fatal("gl census: a call at %llx, where the census took no function", (unsigned long long)entry);
    if (c->x[30] - g_gl_census.driver_lo < g_gl_census.driver_hi - g_gl_census.driver_lo ||
        atomic_load_explicit(&f->behind, memory_order_relaxed)) {
        atomic_fetch_add_explicit(&f->driver_calls, 1, memory_order_relaxed);
        f->own(c, entry);
        return;
    }
    now = frame_now() + 1;
    prev = atomic_load_explicit(&f->last, memory_order_relaxed);
    /* the thread that moves last to a later frame closes the frame before it; a thread that read an earlier frame
       than another's leaves it */
    while (prev < now &&
           !atomic_compare_exchange_weak_explicit(&f->last, &prev, now, memory_order_relaxed, memory_order_relaxed)) {}
    if (prev < now) {
        u64 closed = atomic_exchange_explicit(&f->frame_calls, 0, memory_order_relaxed),
            most = atomic_load_explicit(&f->most, memory_order_relaxed);
        while (closed > most && !atomic_compare_exchange_weak_explicit(&f->most, &most, closed, memory_order_relaxed,
                                                                       memory_order_relaxed)) {}
        atomic_fetch_add_explicit(&f->frames, 1, memory_order_relaxed);
        if (!prev) atomic_store_explicit(&f->first, now, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&f->frame_calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&f->calls, 1, memory_order_relaxed);
    if (now > atomic_load_explicit(&g_gl_census.next_write, memory_order_relaxed) && pm_trylock(&g_gl_census.lock)) {
        if (now > atomic_load_explicit(&g_gl_census.next_write, memory_order_relaxed)) {
            atomic_store_explicit(&g_gl_census.next_write, now - 1 + SNX_GL_CENSUS_EVERY, memory_order_relaxed);
            census_write();
        }
        pm_unlock(&g_gl_census.lock);
    }
    f->own(c, entry);
}

/* the function at addr counted by name (asked: the name the game gave eglGetProcAddress): 1 when its slot holds the
   counter, now or from before; 0 when it can't (outside the game's code, no recompiled code, the table full), each
   counted for the log. Lock held */
static int census_take(u64 addr, const char* name, u32 asked) {
    _Atomic GuestFn* slot;
    GuestFn cur;
    GlCensusFn* fn = g_gl_census.fn;
    u32 i, n;
    if (addr < g_code_lo || addr >= g_code_hi || (addr & 3)) return 0;
    slot = &g_disp[(addr - g_code_lo) / 4];
    cur = nce_slot(atomic_load_explicit(
        slot, memory_order_relaxed)); /* under NCE, an empty slot is the function's own native code */
    if (cur == counted) return 1;
    if (!cur) {
        g_gl_census.no_code++;
        return 0;
    }
    for (n = 0, i = slot_of(addr); n < GL_CENSUS_SLOTS && atomic_load_explicit(&fn[i].addr, memory_order_relaxed); n++)
        i = (i + 1) & (GL_CENSUS_SLOTS - 1);
    if (n == GL_CENSUS_SLOTS) {
        WARN_LIMIT(4, "gl census: %s at %llx isn't counted: the table holds %d functions\n", name,
                   (unsigned long long)addr, GL_CENSUS_SLOTS);
        g_gl_census.full++;
        return 0;
    }
    fn[i].own = cur;
    fn[i].asked = asked;
    snprintf(fn[i].name, sizeof fn[i].name, "%s", name);
    if (!asked) {
        u16* names = by_name();
        u32 k = name_slot(fn[i].name);
        while (names[k]) k = (k + 1) & (GL_CENSUS_SLOTS - 1); /* fewer exports than slots: one is free */
        names[k] = (u16)(i + 1);
    }
    atomic_store_explicit(&fn[i].addr, addr, memory_order_release);
    atomic_store_explicit(slot, counted, memory_order_release);
    nce_slot_taken(addr, "gl census");
    jit_slot_taken(addr, counted);
    g_gl_census.taken++;
    return 1;
}

/* eglGetProcAddress(procname), the driver's own run to its return; an address it answers that the census has no
   function at is taken by the name asked */
static void getproc(CPU* RESTRICT c, u64 entry) {
    char name[GL_CENSUS_NAME];
    u64 lr = c->x[30], pc, found;
    int len = mem_user_string(name, c->x[0], sizeof name);
    g_gl_census.getproc_own(c, entry);
    while ((pc = c->pc) != lr) snx_lookup(c, pc)(c, pc); /* its lookup is a tail call */
    found = c->x[0];
    atomic_fetch_add_explicit(&g_gl_census.asked, 1, memory_order_relaxed);
    if (!found) return;
    atomic_fetch_add_explicit(&g_gl_census.answered, 1, memory_order_relaxed);
    pm_lock(&g_gl_census.lock);
    if (!census_find(found)) {
        if (len <= 1)
            LOG_LIMIT(16, "gl census: eglGetProcAddress answered %llx for %s, which isn't counted\n",
                      (unsigned long long)found,
                      len < 0 ? "a name in unmapped memory"
                      : len   ? "an empty name"
                              : "a name of over 63 characters");
        else if (census_take(found, name, 1)) {
            GlCensusFn* export = export_named(name);
            if (export) atomic_store_explicit(&export->behind, 1, memory_order_relaxed);
            g_gl_census.unnamed++;
            LOG_LIMIT(1,
                      "gl census: eglGetProcAddress answers %s at %llx, where the driver exports no function (a stub "
                      "of its own): the game's calls are counted there by the name asked%s\n",
                      name, (unsigned long long)found, export ? ", and the export's behind it as the driver's" : "");
        } else
            LOG_LIMIT(16,
                      "gl census: eglGetProcAddress answered %s at %llx, which isn't counted (outside the game's "
                      "code, or no recompiled code there)\n",
                      name, (unsigned long long)found);
    }
    pm_unlock(&g_gl_census.lock);
}

void gl_census_init(void) {
    const char* path = g_settings.gl_call_census;
    const char* name;
    u64 getproc_at, addr;
    _Atomic GuestFn* slot;
    u32 i;
    if (!path[0]) return;
    if (g_arm_mod) {
        WARN("gl census: gl_call_census=%s, but the game is 32-bit code, which the census doesn't take: no table\n",
             path);
        return;
    }
    getproc_at = kernel_find_symbol_exact("eglGetProcAddress");
    if (!getproc_at || getproc_at < g_code_lo || getproc_at >= g_code_hi || (getproc_at & 3)) {
        WARN("gl census: gl_call_census=%s, but no module of the game exports eglGetProcAddress in its code, so it "
             "has no GL driver to count: no table\n",
             path);
        return;
    }
    for (i = 0; i < g_mod->nmods; i++) {
        const SnxModInfo* m = &g_mod->mods[i];
        if (getproc_at - m->base < m->size) {
            g_gl_census.driver_lo = m->base;
            g_gl_census.driver_hi = m->base + m->size;
            snprintf(g_gl_census.driver, sizeof g_gl_census.driver, "%s", m->name ? m->name : "?");
        }
    }
    if (!g_gl_census.driver_hi) {
        WARN("gl census: gl_call_census=%s, but eglGetProcAddress at %llx lies in no module of the game: no table\n",
             path, (unsigned long long)getproc_at);
        return;
    }
    g_gl_census.fn = calloc(GL_CENSUS_SLOTS, sizeof(GlCensusFn) + sizeof(GlCensusRow) + sizeof(u16));
    if (!g_gl_census.fn) fatal("gl census: no memory for its table of %d functions", GL_CENSUS_SLOTS);
    pm_lock(&g_gl_census.lock);
    for (i = 0; kernel_symbol(i, &addr, &name); i++)
        if (name && addr - g_gl_census.driver_lo < g_gl_census.driver_hi - g_gl_census.driver_lo &&
            !strncmp(name, "_gl", 3))
            census_take(addr, name + 1, 0); /* glDrawArrays: the name the game asks eglGetProcAddress for */
    slot = &g_disp[(getproc_at - g_code_lo) / 4];
    g_gl_census.getproc_own = nce_slot(atomic_load_explicit(slot, memory_order_relaxed));
    if (g_gl_census.getproc_own) {
        atomic_store_explicit(slot, getproc, memory_order_release);
        nce_slot_taken(getproc_at, "gl census");
        jit_slot_taken(getproc_at, getproc);
    } else
        WARN("gl census: eglGetProcAddress at %llx has no recompiled code: the names the game asks it for aren't "
             "seen\n",
             (unsigned long long)getproc_at);
    atomic_store_explicit(&g_gl_census.next_write, SNX_GL_CENSUS_EVERY, memory_order_relaxed);
    pm_unlock(&g_gl_census.lock);
    if (!hook_routed("eglGetProcAddress"))
        WARN("gl census: the module's route list doesn't name eglGetProcAddress, so the census sees only its calls "
             "through function pointers and the PLT stubs the module did not bind. Recompile the game for its direct "
             "calls\n");
    LOG("gl census: counts the game's calls to the %u GL functions its driver (%s) exports with recompiled code (%u "
        "without are not counted); the table goes to %s every %d frames and as the game ends\n",
        g_gl_census.taken, g_gl_census.driver, g_gl_census.no_code, path, SNX_GL_CENSUS_EVERY);
}

void gl_census_report(void) {
    GlCensusFn* fn = g_gl_census.fn;
    u32 called = 0, i;
    u64 calls = 0;
    if (!fn) return;
    pm_lock(&g_gl_census.lock);
    census_write();
    for (i = 0; i < GL_CENSUS_SLOTS; i++) {
        u64 n = atomic_load_explicit(&fn[i].calls, memory_order_relaxed);
        called += n != 0;
        calls += n;
    }
    if (g_gl_census.failed)
        LOG("gl census: the game called %u of the %u GL functions taken, %llu calls in %u frames; %s can't be "
            "written\n",
            called, g_gl_census.taken, (unsigned long long)calls, frame_now(), g_settings.gl_call_census);
    else
        LOG("gl census: the game called %u of the %u GL functions taken, %llu calls in %u frames: the table is in %s\n",
            called, g_gl_census.taken, (unsigned long long)calls, frame_now(), g_settings.gl_call_census);
    pm_unlock(&g_gl_census.lock);
}
