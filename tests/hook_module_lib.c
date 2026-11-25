/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Hook modules from libraries (nn/module_lib.h, nx/loader/hook_libs.h, nn/snx_hook_module.h), of no game:
   the libraries the test suite builds from tests/hook_module_lib_mod.c, loaded as the runtime loads them.
   - the folder the hook_modules_dir setting names, loaded once: its module library added, a file that isn't a
     library passed over, and the library hooks_off names never loaded (the process never has it: plat_dl_loaded).
   - refused, each with its reason in the log: another ABI major, a newer minor, no entry point, an entry point that
     refuses the runtime, a module named otherwise than its file, a file name hooks_off can't name, a module without a
     CPU hook, a module named as one in the list already, a file that isn't there.
   - the module as the runtime's: its title, build ID, code check and CPU hook, after the compiled-in list.
     hook_modules_select applies it to its game, not to another build or with its check failing, and a library's
     module turns no hook family off.
   - the host's log and setting read. tests/hook_lib_cpu.c runs CPU hooks' handlers.
   --load PATH loads one library and prints what the runtime made of it: a module built elsewhere, checked with no
   game (docs/platforms/nx/hooks.md, Modules in libraries) */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/platform/dl.h"
#include "nx/gpu/nvn/nvn_cmd.h"
#include "nx/gpu/renderer.h"
#include "nx/nn/module.h"
#include "nx/nn/module_lib.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"

#ifndef HX_LIBS
#error HX_LIBS names the folder the test's libraries are built in
#endif

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

#define MAIN 0x10000ull

/* ---- the runtime around the hooks: guest memory, the game, the log */
static u8 memory[1 << 20];
u8* g_mem = memory;
static SnxModInfo g_mods[1] = {{"main",
                                MAIN,
                                0x10000,
                                {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef},
                                0,
                                0x1000,
                                0x1000,
                                0x1000,
                                0x2000,
                                0x1000,
                                0,
                                0}};
static SnxModule g_module = {.nmods = 1, .mods = g_mods, .title_id = 0x0100000000abc000ull};
const SnxModule* g_mod = &g_module;
Settings g_settings;
/* options.c's, with one setting of the test's */
int snx_setting_text(const char* key, char* out, size_t size) {
    if (strcmp(key, "hx_setting") || size < sizeof "hx_value") return 0;
    memcpy(out, "hx_value", sizeof "hx_value");
    return 1;
}
/* what the libraries' CPU hooks reach (tests/hook_lib_cpu.c tests them): none of this test's has one */
void hook_run_original(CPU* c, u64 addr) { (void)c, (void)addr; }
int mem_user_mapped(u64 addr, u64 size) { return addr < sizeof memory && size <= sizeof memory - addr; }
int nvn_cmd_record_upscale(CPU* c, u64 cb, const NvnUpscaleRequest* r) { return (void)c, (void)cb, (void)r, 0; }
/* what the host's 1.1 functions reach: no renderer, a console's screen */
const Renderer* g_renderer;
void vi2_buffer_max_size(u32 size[2]);
void vi2_buffer_max_size(u32 size[2]) { size[0] = 1920, size[1] = 1080; }

static char g_log[16384];
void logf_(int lvl, const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    size_t n = strlen(g_log);
    va_list b;
    va_copy(b, a);
    vsnprintf(g_log + n, sizeof g_log - n, fmt, b);
    va_end(b);
    if (getenv("SNX_TEST_LOG") || !lvl) vprintf(fmt, a);
    va_end(a);
}
static int said(const char* text) { return strstr(g_log, text) != NULL; }
static int times(const char* text) {
    int n = 0;
    for (const char* p = g_log; (p = strstr(p, text)) != NULL; p++) n++;
    return n;
}

/* a compiled-in module of another title, first in the list */
static const HookModule g_compiled = {"compiled", "a compiled-in module", 0x0100000000def000ull, NULL, NULL, NULL, 0};
const HookModule* const g_hook_modules[] = {&g_compiled, NULL};

static int off(const char* name) { return !strcmp(name, "hx_off"); }

/* whether the process has the library at path loaded (plat_dl_loaded): Windows names a loaded module's path with
   backslashes */
static int loaded(const char* path) {
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
#ifdef _WIN32
    for (char* c = p; *c; c++)
        if (*c == '/') *c = '\\';
#endif
    return plat_dl_loaded(p) != NULL;
}

static const char* lib(const char* name) {
    static char path[1024];
    snprintf(path, sizeof path, "%s/%s%s", HX_LIBS, name, HX_SUFFIX);
    return path;
}

/* --load: the module the runtime made of path, the last of the list */
static int print(const char* path) {
    if (!hook_module_lib_load(path, NULL)) {
        printf("%s", g_log);
        return 1;
    }
    const HookModule* m = NULL;
    u32 index = 0;
    for (u32 i = 0; hook_module_at(i); i++) m = hook_module_at(i), index = i;
    printf("%smodule %s (%s): title %016llx, module %s, build ID %s, %u code checks\n", g_log, m->name,
           m->what ? m->what : "", (unsigned long long)m->title, m->module ? m->module : "main",
           m->build_id ? m->build_id : "any", m->ncode);
    for (u32 i = 0; i < m->ncode; i++)
        printf("  check %u: %s+%llx \"%s\"\n", i, m->code[i].module ? m->code[i].module : "its module",
               (unsigned long long)m->code[i].offset, m->code[i].bytes);
    for (u32 k = 0; k < hook_lib_cpu_count(); k++) {
        const HookLibCpu* l = hook_lib_cpu(k);
        if (l->module == index)
            printf("  CPU hook %s: %s+%llx (the key is the game module's whole build ID and this offset)\n",
                   l->hook->name, l->in ? l->in : "main", (unsigned long long)l->offset);
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && !strcmp(argv[1], "--load")) return print(argv[2]);

    /* the folder: hx_good added, notes.txt passed over, hx_off never loaded */
    {
        FILE* f = fopen(HX_LIBS "/folder/notes.txt", "w");
        CHECK(f);
        fputs("not a library\n", f);
        fclose(f);
    }
    snprintf(g_settings.hook_modules_folder, sizeof g_settings.hook_modules_folder, "%s", HX_LIBS "/folder");
    hook_module_libs_load(off);
    hook_module_libs_load(off); /* once in a run: the second call loads nothing */
    CHECK(said("hooks: 1 module from the libraries in " HX_LIBS "/folder\n"));
    CHECK(times("module from the libraries") == 1 && times(": module hx_good (hook ABI") == 1);
    CHECK(!loaded(HX_LIBS "/folder/hx_off" HX_SUFFIX) && loaded(HX_LIBS "/folder/hx_good" HX_SUFFIX));
    CHECK(said("hooks: module hx_off is off (hooks=0 or hooks_off): its library "));
    CHECK(said("hx_good" HX_SUFFIX ": module hx_good (hook ABI 1.2), 1 CPU hook\n"));
    CHECK(!said("notes.txt"));

    /* the module as the runtime's, after the compiled-in one */
    const HookModule* m = hook_module_at(1);
    CHECK(hook_module_at(0) == &g_compiled && m && !hook_module_at(2));
    CHECK(!strcmp(m->name, "hx_good") && !strcmp(m->what, "a test module of no game") &&
          m->title == 0x0100000000abc000ull && !m->module && !strcmp(m->build_id, "0123456789abcdef"));
    CHECK(m->ncode == 1 && !m->code[0].module && m->code[0].offset == 0x40 && !strcmp(m->code[0].bytes, "de ad ?? ef"));
    CHECK(hook_lib_cpu_count() == 1 && hook_lib_cpu(0)->module == 1 && hook_lib_cpu(0)->offset == 0x80 &&
          !hook_lib_cpu(0)->in && !strcmp(hook_lib_cpu(0)->owner, "hx_good") &&
          !strcmp(hook_lib_cpu(0)->hook->name, "around"));

    /* the refusals, none of them added */
    static const struct {
        const char* file;
        const char* why;
    } k_refused[] = {
        {"hx_major", "refused: it is built for hook ABI 2.0, and the runtime has 1.2 (another major)"},
        {"hx_minor", "refused: it is built for hook ABI 1.3, and the runtime has 1.2 (a newer minor than the"},
        {"hx_no_entry", "refused: it has no snx_hook_module"},
        {"hx_null", "refused: its snx_hook_module refused this runtime (hook ABI 1.2)"},
        {"hx_misnamed", "refused: its module is named hx_other, not as its file"},
        {"hx_empty", "refused: it has 0 CPU hooks (1 to 32)"},
        {"hx-dash", "refused: its name has a character other than a to z, 0 to 9 and _"},
        {"hx_missing", "refused: it doesn't load: "},
    };
    for (size_t i = 0; i < sizeof k_refused / sizeof k_refused[0]; i++) {
        g_log[0] = 0;
        int added = hook_module_lib_load(lib(k_refused[i].file), off);
        if (added || !said(k_refused[i].why)) fprintf(stderr, "%s: %s", k_refused[i].file, g_log);
        CHECK(!added && said(k_refused[i].why));
    }
    g_log[0] = 0;
    CHECK(!hook_module_lib_load(lib("hx_null"), off) && said("hooks: hx_null: the runtime's hook ABI 1.2 is older"));
    g_log[0] = 0;
    CHECK(!hook_module_lib_load(HX_LIBS "/folder/hx_good" HX_SUFFIX, off) && said("another module is named hx_good"));
    CHECK(!hook_module_at(2));

    /* which game it applies to */
    memcpy(memory + MAIN + 0x40, "\xde\xad\xbe\xef", 4);
    g_log[0] = 0;
    hook_modules_select(&g_module, memory, off);
    CHECK(hook_module_applies(1) && !hook_module_applies(0) && said("hooks: module hx_good applies"));
    CHECK(hook_module_family_refused("compiled") && !hook_module_family_refused("hx_good"));
    g_mods[0].build_id[7] = 0xee;
    g_log[0] = 0;
    hook_modules_select(&g_module, memory, off);
    CHECK(!hook_module_applies(1) && said("main is another build than 0123456789abcdef"));
    CHECK(!hook_module_family_refused("hx_good")); /* a library's module has no family */
    g_mods[0].build_id[7] = 0xef;
    memory[MAIN + 0x43] = 0;
    g_log[0] = 0;
    hook_modules_select(&g_module, memory, off);
    CHECK(!hook_module_applies(1) && said("check 0 at main+40 differs at byte 3"));
    memory[MAIN + 0x43] = 0xef;
    g_log[0] = 0;
    hook_modules_select(&g_module, memory, off);
    CHECK(hook_module_applies(1));

    /* the host's log and setting read */
    const SnxHookHost* h = hook_module_lib_host();
    char value[16];
    CHECK(h->abi_major == SNX_HOOK_ABI_MAJOR && h->abi_minor == SNX_HOOK_ABI_MINOR && h->size == sizeof *h);
    g_log[0] = 0;
    h->log(SNX_HOOK_LOG_INFO, "hx: %d lines\n", 2);
    CHECK(said("hooks: hx: 2 lines\n") && !said("lines\n\n"));
    CHECK(h->setting("hx_setting", value, sizeof value) && !strcmp(value, "hx_value") &&
          !h->setting("hx_setting", value, 4) && !h->setting(NULL, value, sizeof value));
    puts("hook_module_lib: passed");
    return 0;
}
