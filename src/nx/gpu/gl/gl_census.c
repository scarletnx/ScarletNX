/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL call census (gl_call_census): which OpenGL functions a game that draws through its own GL driver calls, how
   often, and from which frame, to size a GL front end over the runtime's own objects (#950). Such a driver is a module
   of the game that exports each GL entry point by name (_glDrawArrays) beside eglGetProcAddress, and the game calls
   GL through the pointers eglGetProcAddress answers, so each call goes through the function's dispatch slot. The slot
   of every _gl function of that module takes a counter, which then runs what the slot held, as nvn_loader.c takes the
   NVN functions' slots: the driver still does everything it did. eglGetProcAddress is wrapped too (hooks.hook routes
   it), so an address it answers that no export names is counted by the name the game asked for.

   A call whose return address lies in the driver is the driver's own use of its export, counted apart. The frame of a
   call is the runtime's count of frames presented when it is made (g_frames). The file, a row for each function called
   by the most calls, is written again every SNX_GL_CENSUS_EVERY frames by the thread whose call finds it due, and as
   the game ends */
#include "nx/gpu/gl/gl_census.h"
#include "common/platform/file.h"
#include "common/platform/thread.h"
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
#include <string.h>

#define GL_CENSUS_SLOTS 8192 /* a power of two, over three times the ~2470 GL functions a driver exports */
#define GL_CENSUS_NAME  64   /* the longest GL name a driver exports has 49 characters */

/* a function the census took, by its entry. first and last are 1 + the frame of the game's first and last call, 0
   before one. Its counters are atomic: any thread calls it */
typedef struct {
    u64 addr; /* 0: a free entry */
    GuestFn own;
    u64 calls, driver_calls, frame_calls, most;
    u32 frames, first, last, asked; /* asked: named by the game's eglGetProcAddress, not by an export */
    char name[GL_CENSUS_NAME];
} GlCensusFn;
/* a row of the file: a function's counts as they were read, before the rows are sorted */
typedef struct {
    u64 calls, driver_calls, most;
    u32 frames, first, last, fn;
} GlCensusRow;

/* fn: GL_CENSUS_SLOTS functions by address, then as many rows, one block, NULL while the census is off. The lock
   orders the table's inserts, the slots taken and the file */
static struct {
    PMutex lock;
    GlCensusFn* fn;
    GuestFn getproc_own;
    u64 driver_lo, driver_hi;
    u32 taken, no_code, full, next_write, failed;
    u64 asked, answered, unnamed; /* eglGetProcAddress: names asked, answered, and at an address no export names */
    char driver[32];
} g_gl_census = {.lock = PMUTEX_INIT};

static u32 slot_of(u64 addr) { return (u32)((addr >> 2) * 2654435761u) & (GL_CENSUS_SLOTS - 1); }
static GlCensusFn* census_find(u64 addr) {
    GlCensusFn* fn = g_gl_census.fn;
    for (u32 n = 0, i = slot_of(addr); n < GL_CENSUS_SLOTS; n++, i = (i + 1) & (GL_CENSUS_SLOTS - 1)) {
        u64 a = __atomic_load_n(&fn[i].addr, __ATOMIC_ACQUIRE);
        if (a == addr) return &fn[i];
        if (!a) return NULL;
    }
    return NULL;
}

static u32 frame_now(void) { return (u32)__atomic_load_n(&g_frames, __ATOMIC_RELAXED); }

/* the file again, with the counts as they are now. Lock held */
static void census_write(void) {
    GlCensusFn* fn = g_gl_census.fn;
    GlCensusRow* rows = (GlCensusRow*)(fn + GL_CENSUS_SLOTS);
    u32 n = 0, frames = frame_now(), i, j;
    u64 calls = 0;
    char part[sizeof g_settings.gl_call_census + 8];
    FILE* f;
    for (i = 0; i < GL_CENSUS_SLOTS; i++) {
        GlCensusRow r = {__atomic_load_n(&fn[i].calls, __ATOMIC_RELAXED),
                         __atomic_load_n(&fn[i].driver_calls, __ATOMIC_RELAXED),
                         __atomic_load_n(&fn[i].most, __ATOMIC_RELAXED),
                         __atomic_load_n(&fn[i].frames, __ATOMIC_RELAXED),
                         __atomic_load_n(&fn[i].first, __ATOMIC_RELAXED),
                         __atomic_load_n(&fn[i].last, __ATOMIC_RELAXED),
                         i};
        u64 now = __atomic_load_n(&fn[i].frame_calls, __ATOMIC_RELAXED);
        if (!r.calls && !r.driver_calls) continue;
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
    f = snx_fopen(part, "w");
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
            (unsigned long long)__atomic_load_n(&g_gl_census.asked, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&g_gl_census.answered, __ATOMIC_RELAXED),
            (unsigned long long)g_gl_census.unnamed);
    fputs("function\tcalls\tframes\tper_frame\tmost_in_a_frame\tfirst_frame\tlast_frame\tdriver_calls\tnamed_by\n", f);
    for (i = 0; i < n; i++) {
        const GlCensusRow* r = &rows[i];
        if (r->calls)
            fprintf(f, "%s\t%llu\t%u\t%.2f\t%llu\t%u\t%u\t%llu\t%s\n", fn[r->fn].name, (unsigned long long)r->calls,
                    r->frames, (double)r->calls / (double)r->frames, (unsigned long long)r->most, r->first - 1,
                    r->last - 1, (unsigned long long)r->driver_calls, fn[r->fn].asked ? "eglGetProcAddress" : "export");
        else
            fprintf(f, "%s\t0\t0\t0\t0\t-\t-\t%llu\t%s\n", fn[r->fn].name, (unsigned long long)r->driver_calls,
                    fn[r->fn].asked ? "eglGetProcAddress" : "export");
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
    if (c->x[30] - g_gl_census.driver_lo < g_gl_census.driver_hi - g_gl_census.driver_lo) {
        __atomic_add_fetch(&f->driver_calls, 1, __ATOMIC_RELAXED);
        f->own(c, entry);
        return;
    }
    now = frame_now() + 1;
    prev = __atomic_load_n(&f->last, __ATOMIC_RELAXED);
    /* the thread that moves last to a later frame closes the frame before it; a thread that read an earlier frame
       than another's leaves it */
    while (prev < now && !__atomic_compare_exchange_n(&f->last, &prev, now, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    if (prev < now) {
        u64 closed = __atomic_exchange_n(&f->frame_calls, 0, __ATOMIC_RELAXED),
            most = __atomic_load_n(&f->most, __ATOMIC_RELAXED);
        while (closed > most &&
               !__atomic_compare_exchange_n(&f->most, &most, closed, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
        __atomic_add_fetch(&f->frames, 1, __ATOMIC_RELAXED);
        if (!prev) __atomic_store_n(&f->first, now, __ATOMIC_RELAXED);
    }
    __atomic_add_fetch(&f->frame_calls, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&f->calls, 1, __ATOMIC_RELAXED);
    if (now > __atomic_load_n(&g_gl_census.next_write, __ATOMIC_RELAXED) && pm_trylock(&g_gl_census.lock)) {
        if (now > g_gl_census.next_write) {
            __atomic_store_n(&g_gl_census.next_write, now - 1 + SNX_GL_CENSUS_EVERY, __ATOMIC_RELAXED);
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
    GuestFn *slot, cur;
    GlCensusFn* fn = g_gl_census.fn;
    u32 i, n;
    if (addr < g_code_lo || addr >= g_code_hi || (addr & 3)) return 0;
    slot = &g_disp[(addr - g_code_lo) / 4];
    cur = nce_slot(*slot); /* under NCE, an empty slot is the function's own native code */
    if (cur == counted) return 1;
    if (!cur) {
        g_gl_census.no_code++;
        return 0;
    }
    for (n = 0, i = slot_of(addr); n < GL_CENSUS_SLOTS && fn[i].addr; n++) i = (i + 1) & (GL_CENSUS_SLOTS - 1);
    if (n == GL_CENSUS_SLOTS) {
        WARN_LIMIT(4, "gl census: %s at %llx isn't counted: the table holds %d functions\n", name,
                   (unsigned long long)addr, GL_CENSUS_SLOTS);
        g_gl_census.full++;
        return 0;
    }
    fn[i].own = cur;
    fn[i].asked = asked;
    snprintf(fn[i].name, sizeof fn[i].name, "%s", name);
    __atomic_store_n(&fn[i].addr, addr, __ATOMIC_RELEASE);
    __atomic_store_n(slot, counted, __ATOMIC_RELEASE);
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
    __atomic_add_fetch(&g_gl_census.asked, 1, __ATOMIC_RELAXED);
    if (!found) return;
    __atomic_add_fetch(&g_gl_census.answered, 1, __ATOMIC_RELAXED);
    pm_lock(&g_gl_census.lock);
    if (!census_find(found)) {
        if (len <= 1)
            LOG_LIMIT(16, "gl census: eglGetProcAddress answered %llx for %s, which isn't counted\n",
                      (unsigned long long)found,
                      len < 0 ? "a name in unmapped memory"
                      : len   ? "an empty name"
                              : "a name of over 63 characters");
        else if (census_take(found, name, 1)) {
            g_gl_census.unnamed++;
            LOG_LIMIT(32,
                      "gl census: eglGetProcAddress answered %s at %llx, where the driver exports no function: "
                      "counted by that name\n",
                      name, (unsigned long long)found);
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
    GuestFn* slot;
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
    g_gl_census.fn = calloc(GL_CENSUS_SLOTS, sizeof(GlCensusFn) + sizeof(GlCensusRow));
    if (!g_gl_census.fn) fatal("gl census: no memory for its table of %d functions", GL_CENSUS_SLOTS);
    pm_lock(&g_gl_census.lock);
    for (i = 0; kernel_symbol(i, &addr, &name); i++)
        if (name && addr - g_gl_census.driver_lo < g_gl_census.driver_hi - g_gl_census.driver_lo &&
            !strncmp(name, "_gl", 3))
            census_take(addr, name + 1, 0); /* glDrawArrays: the name the game asks eglGetProcAddress for */
    slot = &g_disp[(getproc_at - g_code_lo) / 4];
    g_gl_census.getproc_own = nce_slot(*slot);
    if (g_gl_census.getproc_own) {
        __atomic_store_n(slot, getproc, __ATOMIC_RELEASE);
        nce_slot_taken(getproc_at, "gl census");
        jit_slot_taken(getproc_at, getproc);
    } else
        WARN("gl census: eglGetProcAddress at %llx has no recompiled code: the names the game asks it for aren't "
             "seen\n",
             (unsigned long long)getproc_at);
    g_gl_census.next_write = SNX_GL_CENSUS_EVERY;
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
        u64 n = __atomic_load_n(&fn[i].calls, __ATOMIC_RELAXED);
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
