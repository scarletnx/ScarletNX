/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL call census (src/nx/gpu/gl/gl_census.c), with the runtime stubbed to two modules, their symbols and a
   dispatch table. Empty, the setting takes no slot. Set, each _gl function the driver module exports counts the
   game's calls in its slot and still runs its own code (its result reaches the caller); one with no recompiled code
   and one of another module are left as they were; a call whose return address lies in the driver counts apart; an
   address eglGetProcAddress answers (through its lookup's tail call) that no export names is counted by the name the
   game asked for, and a stub it answers, which jumps to the export of its name, counts the game's call once: at the
   stub, the export's call behind it as the driver's. The file holds each function's calls, frames, calls a frame, most in one frame, first and last
   frame, by calls; it is written again once SNX_GL_CENSUS_EVERY frames went by, and not before */
#include "nx/gpu/gl/gl_census.c"
#include <assert.h>
#include <stdio.h>

#define MAIN    0x10000u
#define DRIVER  0x20000u
#define END     0x30000u
#define GETPROC (DRIVER + 0x100)
#define DRAW    (DRIVER + 0x200)
#define CLEAR   (DRIVER + 0x300)
#define NOCODE  (DRIVER + 0x400)
#define HIDDEN  (DRIVER + 0x500) /* a function no export names, which eglGetProcAddress answers */
#define LOOKUP  (DRIVER + 0x600) /* eglGetProcAddress's lookup, which it tail-calls */
#define ELEMS   (DRIVER + 0x280)
#define STUB    (DRIVER + 0x700) /* the stub eglGetProcAddress answers for glDrawElements: it jumps to ELEMS */
#define MAINGL  (MAIN + 0x80)    /* a _gl export of another module than the driver */
#define NAME    0x8000u          /* where the game puts the name it asks for */

static _Atomic GuestFn slots[(END - MAIN) / 4];
static u8 guest[0x40000];
u8* g_mem = guest;
_Atomic GuestFn* g_disp = slots;
u64 g_code_lo = MAIN, g_code_hi = END;
static const SnxModInfo modules[2] = {{.name = "main", .base = MAIN, .size = DRIVER - MAIN},
                                      {.name = "driver", .base = DRIVER, .size = END - DRIVER}};
static SnxModule module = {.nmods = 2, .mods = modules};
const SnxModule* g_mod = &module;
const SnxArmModule* g_arm_mod;
Settings g_settings;
_Atomic int g_frames;

static const struct {
    const char* name;
    u64 addr;
} symbols[] = {{"_glMain", MAINGL}, {"eglGetProcAddress", GETPROC}, {"_glDrawArrays", DRAW}, {"_glDrawElements", ELEMS},
               {"_glClear", CLEAR}, {"_glNoCode", NOCODE},          {"_nv001rmt", LOOKUP}};
int kernel_symbol(u32 i, u64* addr, const char** name) {
    if (i >= sizeof symbols / sizeof symbols[0]) return 0;
    *addr = symbols[i].addr, *name = symbols[i].name;
    return 1;
}
u64 kernel_find_symbol_exact(const char* name) {
    for (size_t i = 0; i < sizeof symbols / sizeof symbols[0]; i++)
        if (!strcmp(symbols[i].name, name)) return symbols[i].addr;
    return 0;
}
int mem_user_string(char* dst, u64 addr, u64 cap) {
    for (u64 i = 0; i < cap && addr + i < sizeof guest; i++)
        if (!(dst[i] = (char)guest[addr + i])) return (int)(i + 1);
    return 0;
}
GuestFn nce_slot(GuestFn f) { return f; }
int nce_slot_taken(u64 addr, const char* what) { return (void)addr, (void)what, 0; }
void jit_slot_taken(u64 addr, GuestFn fn) { (void)addr, (void)fn; }
int hook_routed(const char* name) { return !strcmp(name, "eglGetProcAddress"); }
void logf_(int lvl, const char* fmt, ...) { (void)lvl, (void)fmt; }
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
int pm_trylock(PMutex* m) { return (void)m, 1; }
void fatal(const char* fmt, ...) {
    fprintf(stderr, "fatal: %s\n", fmt);
    exit(1);
}

/* ---- the driver's own code: each returns to x30 */
static unsigned draws, clears, hidden, getprocs;
static void own_draw(CPU* RESTRICT c, u64 entry) {
    (void)entry;
    draws++;
    c->x[0] = 0x5a;
    c->pc = c->x[30];
}
static void own_clear(CPU* RESTRICT c, u64 entry) {
    (void)entry;
    clears++;
    c->pc = c->x[30];
}
static void own_hidden(CPU* RESTRICT c, u64 entry) {
    (void)entry;
    hidden++;
    c->pc = c->x[30];
}
/* eglGetProcAddress leaves for its lookup through a register, as the driver's does (a tail call) */
static void own_getproc(CPU* RESTRICT c, u64 entry) {
    (void)entry;
    getprocs++;
    c->pc = LOOKUP;
}
static void own_lookup(CPU* RESTRICT c, u64 entry) {
    const char* name = (const char*)&guest[c->x[0]];
    (void)entry;
    c->x[0] = !strcmp(name, "glGenBuffers")     ? HIDDEN
              : !strcmp(name, "glClear")        ? CLEAR
              : !strcmp(name, "glDrawElements") ? STUB
                                                : 0;
    c->pc = c->x[30];
}
static unsigned elements;
static void own_elements(CPU* RESTRICT c, u64 entry) {
    (void)entry;
    elements++;
    c->pc = c->x[30];
}
/* the driver's stub: a jump through its context's table to the export, the game's return address kept */
static void own_stub(CPU* RESTRICT c, u64 entry) {
    (void)entry;
    c->pc = ELEMS;
}
static void own_main(CPU* RESTRICT c, u64 entry) {
    (void)entry;
    c->pc = c->x[30];
}
static _Atomic GuestFn* slot(u64 addr) { return &slots[(addr - MAIN) / 4]; }

/* a call of addr returning to lr, as the dispatch loop runs it */
static CPU cpu;
static u64 call(u64 addr, u64 lr) {
    u64 pc;
    cpu.x[30] = lr;
    cpu.pc = addr;
    while ((pc = cpu.pc) != lr) snx_lookup(&cpu, pc)(&cpu, pc);
    return cpu.x[0];
}
static u64 ask(const char* name) {
    snprintf((char*)&guest[NAME], 64, "%s", name);
    cpu.x[0] = NAME;
    return call(GETPROC, MAIN + 0x10);
}
static void calls(u64 addr, u64 lr, int n) {
    while (n--) call(addr, lr);
}

/* the file's line that starts with the function's name and a tab, or "" */
static char text[4096];
static const char* row(const char* name) {
    size_t n = strlen(name);
    for (const char* at = text; *at; at = strchr(at, '\n') + 1) {
        if (!strncmp(at, name, n) && at[n] == '\t') {
            static char line[256];
            size_t len = (size_t)(strchr(at, '\n') - at);
            assert(len < sizeof line);
            memcpy(line, at, len);
            line[len] = 0;
            return line;
        }
        if (!strchr(at, '\n')) break;
    }
    return "";
}
static int read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    size_t n;
    if (!f) return 0;
    n = fread(text, 1, sizeof text - 1, f);
    text[n] = 0;
    fclose(f);
    return 1;
}

int main(int argc, char** argv) {
    static GuestFn before[sizeof slots / sizeof slots[0]];
    const char* path;
    assert(argc == 2);
    path = argv[1];
    remove(path);
    cpu.mem = guest;
    cpu.disp = slots;
    cpu.code_lo = MAIN;
    cpu.code_hi = END;
    *slot(GETPROC) = own_getproc;
    *slot(DRAW) = own_draw;
    *slot(CLEAR) = own_clear;
    *slot(ELEMS) = own_elements;
    *slot(STUB) = own_stub;
    *slot(HIDDEN) = own_hidden;
    *slot(LOOKUP) = own_lookup;
    *slot(MAINGL) = own_main;
    *slot(MAIN + 0x10) = own_main;

    /* empty: nothing taken */
    memcpy(before, slots, sizeof slots);
    gl_census_init();
    assert(!memcmp(before, slots, sizeof slots) && !g_gl_census.fn);
    gl_census_report();
    assert(!read_file(path));

    snprintf(g_settings.gl_call_census, sizeof g_settings.gl_call_census, "%s", path);
    gl_census_init();
    assert(*slot(DRAW) == counted && *slot(CLEAR) == counted && *slot(GETPROC) == getproc);
    assert(*slot(MAINGL) == own_main && !*slot(NOCODE) && *slot(LOOKUP) == own_lookup);
    assert(*slot(ELEMS) == counted && *slot(STUB) == own_stub);
    assert(g_gl_census.taken == 3 && g_gl_census.no_code == 1);

    /* frame 0: the game resolves two names, one at an address no export names; calls each */
    assert(ask("glGenBuffers") == HIDDEN && *slot(HIDDEN) == counted);
    assert(ask("glClear") == CLEAR && ask("glNothing") == 0);
    assert(getprocs == 3 && g_gl_census.unnamed == 1 && g_gl_census.taken == 4);
    /* a stub answered for a name whose export the census took: the game's call counts once, at the stub */
    assert(ask("glDrawElements") == STUB && *slot(STUB) == counted && g_gl_census.unnamed == 2);
    calls(HIDDEN, MAIN + 0x20, 2);
    calls(CLEAR, MAIN + 0x20, 1);
    /* frame 1, and the driver calling its own export */
    g_frames = 1;
    calls(DRAW, MAIN + 0x30, 3);
    calls(CLEAR, MAIN + 0x30, 1);
    calls(CLEAR, DRIVER + 0x40, 1);
    calls(STUB, MAIN + 0x30, 3);
    /* frame 2 */
    g_frames = 2;
    calls(DRAW, MAIN + 0x30, 4);
    cpu.x[0] = 0;
    assert(call(DRAW, MAIN + 0x30) == 0x5a); /* the driver's result reaches the game */
    assert(draws == 8 && clears == 3 && hidden == 2 && elements == 3);
    assert(!read_file(path)); /* not due before SNX_GL_CENSUS_EVERY frames */

    gl_census_report();
    assert(read_file(path));
    fputs(text, stdout);
    assert(strstr(text, "# gl_call_census: the game's calls to the GL functions of its driver (driver): 5 taken, 4 "
                        "called, 15 calls, 2 frames\n"));
    assert(strstr(text, "# eglGetProcAddress: 4 names asked, 3 answered, 2 at an address no export names\n"));
    assert(strstr(text, "\nfunction\tcalls\tframes\tper_frame\tmost_in_a_frame\tfirst_frame\tlast_frame\tdriver_calls\t"
                        "named_by\nglDrawArrays\t"));
    assert(!strcmp(row("glDrawArrays"), "glDrawArrays\t8\t2\t4.00\t5\t1\t2\t0\texport"));
    assert(!strcmp(row("glClear"), "glClear\t2\t2\t1.00\t1\t0\t1\t1\texport"));
    assert(!strcmp(row("glGenBuffers"), "glGenBuffers\t2\t1\t2.00\t2\t0\t0\t0\teglGetProcAddress"));
    assert(strstr(text, "glClear\t") < strstr(text, "glGenBuffers\t")); /* tied by calls: by name */
    assert(!strcmp(row("glDrawElements"), "glDrawElements\t3\t1\t3.00\t3\t1\t1\t0\teglGetProcAddress"));
    assert(!strstr(strstr(text, "glDrawElements\t") + 1, "glDrawElements\t")); /* its export's calls are the driver's */
    assert(census_find(ELEMS)->driver_calls == 3 && !census_find(ELEMS)->calls);
    assert(!*row("glNoCode") && !*row("glMain"));

    /* written again once the frames go past SNX_GL_CENSUS_EVERY, then not before as many more */
    remove(path);
    g_frames = SNX_GL_CENSUS_EVERY + 100;
    calls(DRAW, MAIN + 0x30, 1);
    assert(read_file(path) && !strcmp(row("glDrawArrays"), "glDrawArrays\t9\t3\t3.00\t5\t1\t700\t0\texport"));
    remove(path);
    g_frames = 2 * SNX_GL_CENSUS_EVERY + 99;
    calls(DRAW, MAIN + 0x30, 1);
    assert(!read_file(path));
    g_frames = 2 * SNX_GL_CENSUS_EVERY + 100;
    calls(DRAW, MAIN + 0x30, 1);
    assert(read_file(path) && !strcmp(row("glDrawArrays"), "glDrawArrays\t11\t5\t2.20\t5\t1\t1300\t0\texport"));
    remove(path);
    puts("gl_census: ok");
    return 0;
}
