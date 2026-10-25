/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime's startup: which game to run, its recompiled code, its folders and settings, the console's parts in the
   order they depend on each other, the shaders made before the game draws (prepare_wait), the game's main thread;
   then the frontend's loop at the display's rate until the run ends, saving what outlives it */
#include "nx/runtime/main.h"
#include "common/cpu/recompiler/llvm/gdb_jit.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/utilities/build_info.h"
#include "common/utilities/common.h"
#include "common/utilities/ini_lines.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/nvn/nvn_direct.h"
#include "nx/gpu/nvn_trace.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/filesystem/fs.h"
#include "nx/hle/hle.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/hle/hook/hook.h"
#include "nx/hle/services/hid.h"
#include "nx/runtime/guest_jit.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/hle/services/services.h"
#include "nx/hle/services/vi.h"
#include "nx/memory/mem.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/gdbstub.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/options.h"
#include "nx/runtime/pause.h"
#include "nx/runtime/state.h"
#include "nx/runtime/rtapi.h"
#include "nx/runtime/startup.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/hud.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/overlay.h"
#include "nx/runtime/platform/prepare_screen.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/tracy.h"
#include "nx/runtime/platform/ui.h"
#include "nx/runtime/platform/unattended.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
/* iOS loads no code that was not shipped with the app: the game's code is a pack of objects (recompile --target ios)
   that the JIT loader places in memory a debugger made executable (load_pack) */
#if defined(__APPLE__) && TARGET_OS_IOS
#define MODULE_PACK 1
#include "common/cpu/recompiler/llvm/code_loader.h"
#include "common/cpu/recompiler/llvm/objects_pack.h"
/* src/gui/apple.m's (gui/apple.h pulls in SDL's headers, which this part of the runtime is not built with) */
void gui_apple_stats(char* out, size_t size);
#include "common/cpu/recompiler/llvm/rtlib_object.h"
#endif

#if defined(_WIN32)
#define MODULE_SUFFIX "_code.dll"
#elif defined(MODULE_PACK)
#define MODULE_SUFFIX "_code.snxobj"
#else
#define MODULE_SUFFIX "_code.so"
#endif

char g_datadir[1024], g_exename[256], g_exedir[1024];
u64 g_addon;
char g_addondir[1024], g_gamename[256];
char g_confdir[1024], g_statedir[1024], g_cachedir[1024];
volatile int g_quit;
int (*snx_menu)(const char* self);
void (*snx_menu_run_theme)(const char* folder, SnxRunTheme* out);
const SnxModule* g_mod;
const SnxVariants* g_variants;
GuestFn* g_disp;

/* A build that links a game's code into the runtime defines it; none does yet (src/nx/CMakeLists.txt) */
const SnxModule* snx_module_static(void) __attribute__((weak));

typedef const SnxModule* (*ModuleFn)(void);
typedef const SnxArmModule* (*ArmModuleFn)(void);
typedef const SnxVariants* (*VariantsFn)(void);

static int is_separator(char c) { return c == '/' || c == '\\'; }

/* ---- the game to run ---- */

/* a console dump, by its extension (after the last dot, past any separator), which the driver prepares: the runtime
   runs games already recompiled */
static int is_dump(const char* path) {
    static const char* const kinds[] = {"xci", "xcz", "nsp", "nsz", "nca", "nro", "nso"};
    const char* dot = strrchr(path, '.');
    size_t i, k;
    if (!dot || strchr(dot, '/') || strchr(dot, '\\')) return 0;
    for (k = 0; k < sizeof kinds / sizeof *kinds; k++) {
        for (i = 0; kinds[k][i] && dot[1 + i] && (dot[1 + i] | 0x20) == kinds[k][i]; i++) continue;
        if (!kinds[k][i] && !dot[1 + i]) return 1;
    }
    return 0;
}

/* --game=PATH, the last before a "--" */
static const char* game_option(int argc, char** argv) {
    const char* game = NULL;
    int i;
    for (i = 1; i < argc && strcmp(argv[i], "--"); i++)
        if (!strncmp(argv[i], "--game=", 7)) game = argv[i] + 7;
    return game;
}

static void refuse_dumps(int argc, char** argv, const char* game) {
    const char* dump = game && is_dump(game) ? game : NULL;
    int i;
    for (i = 1; !dump && i < argc && strcmp(argv[i], "--"); i++)
        if (argv[i][0] != '-' && is_dump(argv[i])) dump = argv[i];
    if (dump)
        fatal("%s is a dump, and this runs games already recompiled. Open the dump without --game "
              "(scarletnx Game.nsp) to prepare and play it, or point --game at a recompiled game's folder",
              dump);
}

static void drop_suffix(char* name, const char* suffix) {
    size_t n = strlen(name), s = strlen(suffix);
    if (n > s && !strcmp(name + n - s, suffix)) name[n - s] = 0;
}

/* g_exedir and g_exename from the path of the game or of the runtime's copy named after it: a folder is the game of
   its own name in it (games/<id>/ is games/<id>/<id>), and the program's suffixes and the code module's are dropped,
   so any of them names the same game */
static void name_game(const char* from) {
    char path[1024], base[256];
    size_t n, cut;
    snprintf(path, sizeof path, "%s", from);
    n = strlen(path);
    while (n > 1 && is_separator(path[n - 1])) path[--n] = 0;
    if (plat_is_dir(path)) {
        for (cut = n; cut && !is_separator(path[cut - 1]); cut--) continue;
        snprintf(base, sizeof base, "%s", path + cut);
        snprintf(path + n, sizeof path - n, "/%s", base);
        n = strlen(path);
    }
    for (cut = n; cut && !is_separator(path[cut - 1]); cut--) continue;
    snprintf(g_exename, sizeof g_exename, "%s", path + cut);
    drop_suffix(g_exename, ".exe");
    drop_suffix(g_exename, ".EXE");
    drop_suffix(g_exename, ".AppImage");
    drop_suffix(g_exename, "_code.so");
    drop_suffix(g_exename, "_code.dll");
    drop_suffix(g_exename, "_code.snxobj");
    path[cut] = 0;
#if defined(_WIN32)
    snprintf(g_exedir, sizeof g_exedir, "%s", path);
#else
    /* a bare name is in the current folder: "./" keeps the library loader from searching its path for the module */
    snprintf(g_exedir, sizeof g_exedir, "%s", cut ? path : "./");
#endif
}

/* An AppImage runs the runtime from its mount: the copy named after the game is the AppImage file. APPIMAGE and APPDIR
   are inherited by the processes it starts, so they count only for a runtime inside that mount */
static const char* program_path(const char* argv0) {
    const char *image = getenv("APPIMAGE"), *dir = getenv("APPDIR");
    if (image && image[0] && dir && dir[0] && !strncmp(argv0, dir, strlen(dir))) return image;
    return argv0;
}

/* ---- the game's code ---- */

#ifdef MODULE_PACK
/* The pack's objects loaded into an arena of the JIT loader's, after the runtime library this app embeds (the pack has
   none: its host had none for this machine). Each of the arena's regions is what the two loads take of it
   (snx_code_need), not a share of twice the file: a pack whose code passes five eighths of that fits, and an arena
   whose every page is prepared (ios_arena's debugger) costs the memory the code uses. The module's descriptor is its
   one export. A debugger the arena needs is asked for by the loader, which says what to enable when there is none */
static const SnxModule* load_pack(const char* path) {
    SnxLinkInput rtlib, *inputs;
    SnxCodeLoader* loader;
    const char* names[1];
    SnxU64 address = 0;
    ModuleFn get;
    size_t size = 0, count = 0, need[3], rtlib_need[3], k;
    unsigned char* data = (unsigned char*)snx_read_file(path, &size);
    char error[1024];
    if (!data) fatal("%s could not be read", path);
    inputs = snx_objects_unpack(data, size, "game code", &count);
    if (!inputs) fatal("%s is not a pack of recompiled code: recompile the game with --target ios", path);
    memset(&rtlib, 0, sizeof(rtlib));
    rtlib.data = snx_rtlib_object;
    rtlib.size = snx_rtlib_object_size;
    rtlib.name = "rtlib";
    if (snx_code_need(&rtlib, 1, rtlib_need, error, sizeof(error)) ||
        snx_code_need(inputs, count, need, error, sizeof(error)))
        fatal("The game's code cannot be loaded: %s", error);
    for (k = 0; k < 3; ++k) need[k] += rtlib_need[k];
    LOG("game: a code arena of %.1f MiB of code, %.1f MiB of read-only data and %.1f MiB of data for its %.1f MB of "
        "objects\n",
        (double)need[0] / 1048576.0, (double)need[1] / 1048576.0, (double)need[2] / 1048576.0, (double)size / 1e6);
    loader = snx_code_loader_new_regions(need, error, sizeof(error));
    if (!loader) fatal("The game's code cannot be loaded: %s", error);
    names[0] = "snx_module";
    if (snx_code_load(loader, &rtlib, 1, NULL, 0, NULL, error, sizeof(error)) ||
        snx_code_load(loader, inputs, count, names, 1, &address, error, sizeof(error)))
        fatal("The game's code cannot be loaded: %s", error);
    free(inputs);
    free(data); /* the loaded code is a copy */
    {
        SnxU64 variants = 0;
        if (snx_code_lookup(loader, "snx_variants", &variants)) { /* a global of the pack, in the loader's table */
            VariantsFn get_variants = (VariantsFn)(uintptr_t)variants;
            g_variants = get_variants();
            if (!g_variants || g_variants->version != SNX_VARIANTS_VERSION)
                fatal("%s has a runtime variant table of another version", path);
        }
    }
    get = (ModuleFn)(uintptr_t)address;
    return get();
}
#endif

static const SnxModule* load_module(const char* path) {
    void *lib, *sym;
    ModuleFn get;
    ArmModuleFn get_arm;
    VariantsFn get_variants;
    const SnxArmModule* arm;
    const SnxModule* m;
    if (snx_module_static) return snx_module_static();
    if (!snx_is_file(path))
        fatal("Cannot find the recompiled code: %s. A copy of the runtime runs the game it is named after, with "
              "that code beside it, or --game=PATH names the game (--game=recompiled/Game)",
              path);
#ifdef MODULE_PACK
    m = load_pack(path);
    if (!m) fatal("%s could not be loaded: it describes no game", path);
    return m;
#endif
    lib = plat_dl_open(path);
    if (!lib) fatal("%s could not be loaded: %s", path, plat_dl_error());
    if ((sym = plat_dl_sym(lib, "snx_module")) != NULL) {
        memcpy(&get, &sym, sizeof get); /* ISO C has no conversion from an object pointer to a function's */
        m = get();
    } else if ((sym = plat_dl_sym(lib, "snx_module_arm")) != NULL) {
        memcpy(&get_arm, &sym, sizeof get_arm);
        arm = get_arm();
        /* its own version, which SNX_ABI's range leaves alone. The path in a line of its own: a fatal's text is what
           reports are grouped by */
        if (arm && arm->abi != SNX_ARM_MODULE_ABI) {
            WARN("game: its code is %s\n", path);
            fatal("The recompiled code is from another ScarletNX version (32-bit ABI %08x, where this one runs %08x): "
                  "recompile the game with this one",
                  (unsigned)arm->abi, (unsigned)SNX_ARM_MODULE_ABI);
        }
        m = arm_module(arm);
    } else
        fatal("%s is not a ScarletNX game module", path);
    if (!m) fatal("%s could not be loaded: it describes no game", path);
    if ((sym = plat_dl_sym(lib, "snx_variants")) != NULL) {
        memcpy(&get_variants, &sym, sizeof get_variants);
        g_variants = get_variants();
        if (!g_variants || g_variants->version != SNX_VARIANTS_VERSION)
            fatal("%s has a runtime variant table of another version", path);
    }
    return m;
}

/* the dispatch table: a function for each code word the recompiler compiled one for */
static void dispatch_init(void) {
    u64 words, i;
    g_code_lo = g_mod->code_lo;
    g_code_hi = g_mod->code_hi;
    if (g_code_hi <= g_code_lo) return;
    words = (g_code_hi - g_code_lo) / 4;
    if (words > SIZE_MAX / sizeof *g_disp) fatal("The game's code range is too large");
    g_disp = (GuestFn*)calloc((size_t)words, sizeof *g_disp);
    if (!g_disp)
        fatal("Out of memory for the game's dispatch table (%llu MiB)",
              (unsigned long long)(words * sizeof *g_disp >> 20));
    for (i = 0; i < g_mod->nentries; i++) {
        u64 a = g_mod->entry_addr[i];
        if (a >= g_code_lo && a < g_code_hi && !(a & 3)) g_disp[(a - g_code_lo) / 4] = g_mod->entry_fn[i];
    }
}

/* ---- folders ---- */

/* the folder of kind for the game's files of that kind: recorded, the one the file record names, when it is there;
   else beside the game, or the XDG folder for a game of an installed build's data folder */
static void kind_dir(char* out, size_t cap, int kind, const char* what, const char* recorded, const char* record) {
    char here[1024];
    size_t n = strlen(g_exedir);
    char *moved, *full;
    if (recorded && snx_is_dir(recorded)) {
        snprintf(out, cap, "%s/", recorded);
        snx_path_native(out);
        return;
    }
    snprintf(out, cap, "%s", g_exedir);
    if (n) {
        snprintf(here, sizeof here, "%.*s", (int)(n - 1), g_exedir);
        moved = snx_user_path(here, kind);
        if (moved && strcmp(moved, here)) {
            if (snx_mkdirs(moved))
                WARN("%s cannot be created, so the game's %s stay beside it\n", moved, what);
            else {
                snprintf(out, cap, "%s/", moved);
                snx_path_native(out);
            }
        }
        free(moved);
    }
    if (recorded) {
        full = snx_absolute(out[0] ? out : ".");
        WARN("%s names %s for the game's %s, and it is not there: they are in %s/\n", record, recorded, what,
             full ? full : out);
        free(full);
    }
}

/* the game's folders of each kind: those its driver recorded in <name>.folders beside it (snx_record_folders), which
   the environment the game runs in may not name (an AppImage's copy, without its portable home or config), else its
   environment's */
static void game_dirs(void) {
    static const char* const keys[] = {"config", "state", "cache"};
    char path[sizeof g_exedir + sizeof g_exename + 16], why[256], *text, *key, *value, *full;
    char* recorded[SNX_USER_CACHE + 1] = {NULL}; /* in text */
    const char* record;
    SnxIni ini;
    size_t size;
    int kind;
    snprintf(path, sizeof path, "%s%s.folders", g_exedir, g_exename);
    full = snx_absolute(path);
    record = full ? full : path;
    text = (char*)snx_read_file(path, &size);
    if (!text) snx_os_error(why, sizeof why);
    if (text) snx_ini_start(&ini, text);
    while (text && snx_ini_next(&ini, &key, &value)) {
        for (kind = SNX_USER_CONFIG; kind <= SNX_USER_CACHE && strcmp(key, keys[kind - SNX_USER_CONFIG]); ++kind)
            continue;
        if (kind <= SNX_USER_CACHE)
            recorded[kind] = value;
        else
            WARN("%s: %s=%s is none of config=, state= and cache=, the folders the game reads from it\n", record, key,
                 value);
    }
    kind_dir(g_confdir, sizeof g_confdir, SNX_USER_CONFIG, "settings", recorded[SNX_USER_CONFIG], record);
    kind_dir(g_statedir, sizeof g_statedir, SNX_USER_STATE, "log and threads.txt", recorded[SNX_USER_STATE], record);
    kind_dir(g_cachedir, sizeof g_cachedir, SNX_USER_CACHE, "caches", recorded[SNX_USER_CACHE], record);
    if (!text && snx_path_exists(path))
        WARN("cannot read %s (%s): the game's settings are in %s, its log in %s, its caches in %s\n", record, why,
             g_confdir, g_statedir, g_cachedir);
    free(text);
    free(full);
}

/* An update of the game whose folder it is in (snx_addon_name: <folder>/<folder>_<the update's title ID>) runs as
   that game: with its settings, ini set to the game's .ini, and its data folder, its saves the
   game's (g_datadir, set by the caller from base). Its own .ini names only the add-on's dump, source=, and its own data
   folder lies over the game's, both read by fs.c. base: the game's name, "" for no add-on */
static void addon_init(char* ini, size_t ini_size, char* base, size_t base_size) {
    char* dir = snx_absolute(g_exedir[0] ? g_exedir : ".");
    size_t n = dir ? strlen(dir) : 0, cut;
    base[0] = 0;
    while (n > 1 && is_separator(dir[n - 1])) dir[--n] = 0;
    for (cut = n; cut && !is_separator(dir[cut - 1]); cut--) continue;
    if (dir && snx_addon_name(dir + cut, g_exename, &g_addon)) {
        SnxU64 game = 0;
        snx_title_parse(dir + cut, &game);
        if (snx_addon_kind(game, g_addon) != SNX_ADDON_UPDATE)
            fatal("%s is a DLC's program, which an older version made: a DLC plays with its game, checked in the "
                  "game's list of updates and DLCs (addons= in its .ini). Play the DLC's dump again",
                  g_exename);
        snprintf(base, base_size, "%s", dir + cut);
        snprintf(g_addondir, sizeof g_addondir, "%s%s_data/", g_exedir, g_exename);
        snprintf(ini, ini_size, "%s%s.ini", g_confdir, base);
    }
    free(dir);
}

/* what the game plays with, for its window's title: its update by its version ("Update 1.2.0") and the DLCs found,
   each by its number past the base ID its control.nacp gives ("DLC 1", else its title ID), four at most and how many
   more. "" for none */
static void addons_line(char* out, size_t size) {
    const u64* ids;
    size_t count = fs_dlcs(&ids), i, n;
    u64 base = am_aoc_base_id();
    char version[16];
    out[0] = 0;
    if (g_addon) {
        am_display_version(version);
        snprintf(out, size, "Update %s", version);
    }
    for (i = 0; i < count && i < 4; i++) {
        n = strlen(out);
        if (ids[i] > base && ids[i] - base < 0x800)
            snprintf(out + n, size - n, "%sDLC %u", n ? ", " : "", (unsigned)(ids[i] - base));
        else
            snprintf(out + n, size - n, "%sDLC %016llX", n ? ", " : "", (unsigned long long)ids[i]);
    }
    n = strlen(out);
    if (count > 4) snprintf(out + n, size - n, " and %u more DLCs", (unsigned)(count - 4));
}

/* nothing is moved: a game an older version laid out beside the others (its folder named otherwise) runs where it
   is, each start saying where its folder goes */
static void say_old_layout(void) {
    char* full;
    if (g_addon) return; /* an update, in its game's folder */
    full = snx_absolute(g_exedir[0] ? g_exedir : ".");
    size_t n = full ? strlen(full) : 0, cut;
    for (;;) { /* "games/", "games/." and "games" alike */
        while (n > 1 && is_separator(full[n - 1])) full[--n] = 0;
        if (n > 2 && full[n - 1] == '.' && is_separator(full[n - 2]))
            full[n -= 2] = 0;
        else
            break;
    }
    for (cut = n; cut && !is_separator(full[cut - 1]); cut--) continue;
    if (full && strcmp(full + cut, g_exename))
        WARN("%s/%s and its files were left by an older version: this one keeps them in %s/%s/ "
             "(docs/DEPRECATIONS.md says how to move them)\n",
             full, g_exename, full, g_exename);
    free(full);
}

/* ---- the program's metadata ---- */

static u32 rd32le(const u8* p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

/* exefs/main.npdm's MMU flags (its byte at 0x0C): the address space type, which decides the layout mem_init sizes
   its blocks in and the loader places the code at. The npdm is read through the fs, which fs_init has just brought
   up. A 64-bit program of type 1 is the console's pre-2.0 36-bit one, and mem_layout36's comment says what the
   layout has to guarantee for it: what its SDK marshals into an IPC buffer descriptor drops every address bit above
   36, and the 39-bit layout's stack and heap, above 64 GiB, came back as addresses with no memory behind them */
static void npdm_layout(void) {
    u64 size = 0;
    unsigned char* d = fs_read_file("exefs/main.npdm", &size);
    if (!d) return;
    if (size >= 0x80 && size <= 0x4000 && !memcmp(d, "META", 4)) {
        unsigned flags = d[0x0c];
        if ((flags & 1) && ((flags >> 1) & 7) == 1) {
            mem_layout36();
            LOG("npdm: a 36-bit address space: 64 GiB, the code region at a fixed 128 MiB\n");
        }
    }
    free(d);
}

/* exefs/main.npdm: the system resource size (newer SDKs set up their memory allocator only with one) and the cores
   and priorities its kernel capabilities allow, from the first ThreadInfo one (svcGetInfo 0 and 1, svcCreateThread's
   checks) */
static void read_npdm(void) {
    u64 size = 0;
    u8* d = fs_read_file("exefs/main.npdm", &size);
    u32 aci, aci_size, caps, caps_size, i;
    if (!d) return;
    if (size < 0x80 || size > 0x4000 || memcmp(d, "META", 4)) {
        WARN("npdm: exefs/main.npdm is not one, the defaults apply\n");
        free(d);
        return;
    }
    g_hle.kernel.sysres_size = rd32le(d + 0x14);
    aci = rd32le(d + 0x70);
    aci_size = rd32le(d + 0x74);
    if (aci < size && aci_size >= 0x38 && aci_size <= size - aci && !memcmp(d + aci, "ACI0", 4)) {
        caps = rd32le(d + aci + 0x30);
        caps_size = rd32le(d + aci + 0x34);
        for (i = 0; caps < aci_size && caps_size <= aci_size - caps && i + 4 <= caps_size; i += 4) {
            u32 cap = rd32le(d + aci + caps + i), lowest, highest, min_core, max_core;
            if ((cap & 0xF) != 0x7) continue; /* ThreadInfo: three ones, then a zero */
            lowest = cap >> 4 & 63;
            highest = cap >> 10 & 63;
            min_core = cap >> 16 & 255;
            max_core = cap >> 24 & 255;
            if (highest > lowest || min_core > max_core || max_core >= NCORES) {
                WARN("npdm: its thread capability %08x is not one the kernel takes, the defaults apply\n", cap);
                break;
            }
            g_hle.svcmem.core_mask = (((u64)2 << max_core) - 1) & ~(((u64)1 << min_core) - 1);
            g_hle.svcmem.prio_mask = (lowest == 63 ? ~(u64)0 : ((u64)2 << lowest) - 1) & ~(((u64)1 << highest) - 1);
            break;
        }
    }
    LOG("npdm: system resource %llu MB, cores %llx, priorities %llx\n", (unsigned long long)(g_hle.kernel.sysres_size >> 20),
        (unsigned long long)g_hle.svcmem.core_mask, (unsigned long long)g_hle.svcmem.prio_mask);
    free(d);
}

/* ---- exit ---- */

void snx_save_before_exit(void) {
    static PMutex lock = PMUTEX_INIT;
    static int saved;
    pm_lock(&lock);
    if (!saved) {
        save_hints();
        if (g_renderer && g_renderer->save) g_renderer->save(1);
        svc_core_exit();
        jit_report();
        nvn_trace_report();
        saved = 1;
    }
    pm_unlock(&lock);
}

/* ---- the frontend's loop ---- */

/* this long without a new frame is worth a line and a dump; and without progress on the shader loading screen, a
   line (prepare_wait) */
#define STALL_NS ((u64)15000000000u)

static void write_threads(u64 second) {
    char path[sizeof g_statedir + 16];
    FILE* f;
    snprintf(path, sizeof path, "%sthreads.txt", g_statedir);
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "second %llu, frames %d\n", (unsigned long long)second, __atomic_load_n(&g_frames, __ATOMIC_RELAXED));
    tls_log_redirect = f; /* this thread's lines only: the others keep the log */
    kernel_dump_threads();
    tls_log_redirect = NULL;
    fclose(f);
}

/* the host threads blocked on a mutex or waiting on a condition variable: the caller as an offset from pm_lock and the
   mutex as one from pm_wait_base, which the runtime's symbols name (nm, addr2line). The watchdog's own report of a
   deadlock, where no thread notes what it waits for in game terms */
static void report_lock_waits(void) {
    PmWait w[64];
    int n = pm_waits(w, 64), i;
    for (i = 0; i < n; i++)
        WARN("  host thread %llu %s mutex base%+lld, called from pm_lock%+lld\n", (unsigned long long)w[i].tid,
             w[i].cond ? "waits on a condition with" : "waits for", (long long)((intptr_t)w[i].m - (intptr_t)pm_wait_base()),
             (long long)((intptr_t)w[i].at - (intptr_t)pm_lock));
}
static void frontend_loop(void) {
    const u64 period = 1000000000u / VI_REFRESH_HZ;
    u64 tick = 0, next = plat_ns(), now, changed = next, rate_at = next;
    int frames = 0, stalls = 0, rate_frames = 0;
    while (!g_quit) {
        int shown, paused;
        SNX_ZONE(z_frontend, "frontend tick");
        video_poll();
        /* the game's input and its display's vsync are guest work, which a pause holds back with the rest */
        paused = game_paused();
        if (!paused) {
            hid_update();
            vi_vsync_tick();
        }
        if (++tick % 60 == 0) ini_poll(0);
        frontend_frame();
        SNX_ZONE_END(z_frontend);
#if defined(SNX_TRACY)
        SNX_PLOT("guest memory", used_memory());
        {
            /* the io plots' bases */
            static u64 was_read, was_written, last_ns;
            u64 read = __atomic_load_n(&g_fs_bytes_read, __ATOMIC_RELAXED);
            u64 written = __atomic_load_n(&g_fs_bytes_written, __ATOMIC_RELAXED);
            u64 now_ns = plat_ns();
            double secs = last_ns ? (double)(now_ns - last_ns) / 1e9 : 0.0;
            if (secs > 0) {
                SNX_PLOT("io read MB/s", (double)(read - was_read) / 1048576.0 / secs);
                SNX_PLOT("io write MB/s", (double)(written - was_written) / 1048576.0 / secs);
            }
            was_read = read;
            was_written = written;
            last_ns = now_ns;
        }
#endif
        pause_unattended();
        state_poll();
        /* here and not on the GPU thread, where writing the pipeline cache would hold up every channel */
        if (tick % 300 == 0) {
            save_hints();
            if (g_renderer->save) g_renderer->save(0);
        }
        if (tick % 900 == 0) write_threads(tick / VI_REFRESH_HZ);
        now = plat_ns();
        shown = __atomic_load_n(&g_frames, __ATOMIC_RELAXED);
        /* the pace the game is shown at, in the log every five seconds: a phone has no overlay to read it from */
        if (now - rate_at >= 5000000000u) {
            LOG("perf: %.1f frames a second over %.1f s (%d shown in all)\n",
                (double)(shown - rate_frames) * 1e9 / (double)(now - rate_at), (double)(now - rate_at) / 1e9, shown);
            rate_at = now;
            rate_frames = shown;
#ifdef MODULE_PACK
            {
                char line[320];
                gui_apple_stats(line, sizeof line);
                LOG("%s\n", line);
            }
#endif
        }
        /* a paused game shows nothing new, and isn't stalled; nor is one that waits for the keyboard's box */
        if (shown != frames || paused || overlay_keyboard_shown()) {
            frames = shown;
            changed = now;
        } else if (now - changed >= STALL_NS && stalls < 4) {
            stalls++;
            changed = now;
            WARN("No new frame for %llu seconds (%d shown, %llu GPU submissions): the game's threads\n",
                 (unsigned long long)(STALL_NS / 1000000000u), frames, (unsigned long long)g_gpu_submits);
            kernel_dump_threads();
            gpu_report_stall();
            report_lock_waits();
        }
        /* paced at the display's rate, from a schedule: one far behind (a stop in the debugger) starts again */
        next += period;
        if (now > next + 100000000u)
            next = now;
        else if (next > now)
            plat_sleep_ns((i64)(next - now));
    }
    ini_poll(1); /* the settings the game's menu changed, even over an edit of the file */
    snx_save_before_exit();
    LOG("Exit.\n");
    snx_tracy_shutdown();
    plat_exit_now(0);
}

/* The shaders and pipelines the renderer makes before the game's first thread runs (prepare_shaders 2, the
   renderer's prepare): a screen in the launcher's loading look says how far they are, until they are made, a press
   starts the game at once (the rest is made in the background as it runs), the run quits (the window closed, Esc),
   or an unattended run's time limit passes. Then the game starts, its window black until it draws. It shows past a
   quarter second only, so a run with little to make doesn't flash it */
enum { STEP_FIND, STEP_TRANSLATE, STEP_PIPES, STEP_KINDS };
/* the next row of the screen, a step of a kind first seen at work at *began (set to now_ms the first time) */
static SnxPrepareStep* prepare_step(SnxPrepareStep* st, int* n, u64* began, u64 now_ms, const char* label, u32 value,
                                    u32 total, int done) {
    SnxPrepareStep* s = &st[(*n)++];
    *s = (SnxPrepareStep){0};
    if (!*began) *began = now_ms;
    s->label = label;
    s->value = value;
    s->total = total;
    s->started_ms = *began; /* for the theme's time left */
    s->done = done;
    return s;
}
/* the screen's rows for p into st (STEP_KINDS of SNX_PREPARE_STEPS at most): their count */
static int prepare_steps(const RendererPrepare* p, SnxPrepareStep* st, u64 began[STEP_KINDS], u64 now_ms) {
    SnxPrepareStep* s;
    int n = 0;
    if (p->files) { /* the walk that finds the programs, then their translation */
        s = prepare_step(st, &n, &began[STEP_FIND], now_ms, "FINDING SHADERS", p->files_seen, p->files,
                         p->programs || !p->scanning);
        snprintf(s->detail, sizeof s->detail, "%s", "files checked");
    }
    if (p->programs) {
        s = prepare_step(st, &n, &began[STEP_TRANSLATE], now_ms, "PREPARING SHADERS", p->programs_done, p->programs,
                         !p->scanning);
        snprintf(s->detail, sizeof s->detail, "(%u new)", p->shipped);
    }
    if (p->pipes) {
        /* the pipelines of earlier runs, made from the shader cache: a player knows them by that name */
        s = prepare_step(st, &n, &began[STEP_PIPES], now_ms, "LOADING SHADER CACHE", p->pipes_made, p->pipes,
                         p->pipes_made >= p->pipes);
        snprintf(s->detail, sizeof s->detail, "%s", "from earlier sessions");
    }
    return n;
}
typedef char prepare_note_sizes[RENDERER_NOTE_SIZE == SNX_PREPARE_NOTE_SIZE ? 1 : -1]; /* the renderer's, handed on */
typedef char prepare_step_room[STEP_KINDS <= SNX_PREPARE_STEPS ? 1 : -1];
static void prepare_draw(const SnxRunTheme* look, const char* title, const SnxPrepareStep* st, int n,
                         const RendererPrepare* p, const char* foot) {
    ui_lock();
    prepare_screen_set(look, title, st, n, (const char (*)[SNX_PREPARE_NOTE_SIZE])p->note, p->notes, foot,
                       plat_ns() / 1000000u);
    ui_unlock();
    if (g_renderer->refresh) g_renderer->refresh();
}
static void prepare_wait(const char* title) {
    RendererPrepare p;
    SnxRunTheme theme = {0}, *look = NULL;
    SnxPrepareStep st[SNX_PREPARE_STEPS];
    u64 began[STEP_KINDS] = {0}, t0 = plat_ns(), moved = t0;
    u32 was = 0;
    int shown = 0, themed = 0, n = 0, stalls = 0;
    char ended[96] = "", how[96];
    if (g_settings.prepare_shaders != 2 || !g_renderer->prepare) return;
    g_renderer->prepare(&p);
    if (p.done) { /* nothing to make (shaders are recorded, or the renderer makes none) */
        unattended_start();
        return;
    }
    for (;;) {
        u64 now = plat_ns();
        u32 at;
        const char* press;
        g_renderer->prepare(&p);
        if (p.done) break;
        video_poll();
        if (g_quit) {
            snprintf(ended, sizeof ended, ", quit");
            break;
        }
        if ((press = backend_pressed()) != NULL) {
            snprintf(ended, sizeof ended, ", skipped by %s", press);
            break;
        }
        /* an unattended run's time limit, counted from here too: the game then has one of its own */
        if (g_settings.seconds > 0 && now - t0 >= (u64)g_settings.seconds * 1000000000u) {
            snprintf(ended, sizeof ended, ", cut short at the run's time limit");
            break;
        }
        /* a line after STALL_NS without progress, so that a pass that hangs leaves a trace */
        at = p.files_seen + p.programs_done + p.pipes_made;
        if (at != was) {
            was = at;
            moved = now;
        } else if (now - moved >= STALL_NS && stalls < 4) {
            stalls++;
            moved = now;
            WARN("prepare: no progress for %llu seconds (%u of %u files, %u of %u programs, %u of %u pipelines, the "
                 "shader files of earlier runs %s)\n",
                 (unsigned long long)(STALL_NS / 1000000000u), p.files_seen, p.files, p.programs_done, p.programs,
                 p.pipes_made, p.pipes, p.loaded ? "read" : "still being read");
        }
        n = prepare_steps(&p, st, began, now / 1000000u);
        if (now - t0 >= 250000000ull && n && backend_window_shown()) { /* not for a headless run */
            char* dir = !themed && snx_menu_run_theme ? backend_launcher_folder() : NULL;
            if (dir) {
                snx_menu_run_theme(dir, &theme);
                if (theme.ink != theme.page) look = &theme; /* else the YoRHa theme's, built in */
            }
            free(dir);
            themed = 1;
#if defined(__ANDROID__) /* no Esc there: back opens the menu, whose Exit quits */
            prepare_draw(look, title, st, n, &p,
                         "Any button or tap: start now. The rest is prepared while you play. Back: menu.");
#else
            prepare_draw(look, title, st, n, &p,
                         "Any key, button or tap: start now. The rest is prepared while you play. Esc: quit.");
#endif
            shown = 1;
        }
        plat_sleep_ns(16000000);
    }
    g_renderer->prepare(&p);
    if (shown && !g_quit) { /* the steps as they ended, a quarter second; then gone, the window black until the game
                               draws */
        n = prepare_steps(&p, st, began, plat_ns() / 1000000u);
        plat_sleep_ns(40000000); /* refresh's pace after the loop's last picture, so this one isn't dropped */
        prepare_draw(look, title, st, n, &p, "");
        plat_sleep_ns(250000000);
        ui_lock();
        prepare_screen_set(look, title, NULL, 0, NULL, 0, "", 0);
        ui_unlock();
        /* twice, refresh's pace apart (a picture each 33 ms at most), so that one dropped doesn't leave the steps up */
        for (int i = 0; i < 2 && g_renderer->refresh; i++) {
            g_renderer->refresh();
            plat_sleep_ns(40000000);
        }
    }
    if (shown)
        snprintf(how, sizeof how, "shown in %s's look", look ? look->name : "the built-in YoRHa theme");
    else
        snprintf(how, sizeof how, "%s",
                 backend_window_shown() ? "not shown, as it was quick" : "not shown, as nothing could be seen");
    LOG("prepare: %llu ms before the game %s (%u shaders translated from its files, %u of %u pipelines of earlier "
        "runs made), %s%s\n",
        (unsigned long long)((plat_ns() - t0) / 1000000u), g_quit ? "would have started" : "started", p.shipped,
        p.pipes_made, p.pipes, how, ended);
    unattended_start(); /* scripted presses and the timed exit count from the game's start */
}

/* ---- startup ---- */

/* the launcher's menu, for a program started with nothing to run under one of its own names, with no game code */
static int open_menu(const char* argv0) {
    char* self = strpbrk(argv0, "/\\") && snx_is_file(argv0) ? snx_absolute(argv0) : snx_executable(argv0);
    int status = snx_menu(self ? self : argv0);
    free(self);
    return status;
}

/* Key paths belong to the runtime as well as the driver: an unextracted game opens its dump again at startup. */
static int runtime_key_options(int* argc, char** argv, char** keys, char** title_keys) {
    int read = 1, write = 1;
    while (read < *argc && strcmp(argv[read], "--")) {
        const char *arg = argv[read], *value = NULL;
        char** slot = NULL;
        if (!strcmp(arg, "--keys")) slot = keys;
        else if (!strcmp(arg, "--title-keys")) slot = title_keys;
        else if (!strncmp(arg, "--keys=", 7)) { slot = keys; value = arg + 7; }
        else if (!strncmp(arg, "--title-keys=", 13)) { slot = title_keys; value = arg + 13; }
        if (!slot) {
            argv[write++] = argv[read++];
            continue;
        }
        if (!value) {
            if (read + 1 >= *argc || !argv[read + 1][0] || argv[read + 1][0] == '-')
                return snx_error("%s requires a file", arg);
            value = argv[++read];
        } else if (!value[0])
            return snx_error("%s requires a file", arg);
        {
            char* path = snx_absolute(value);
            if (!path || !snx_is_file(path)) {
                int result = snx_error("%s is not a file: %s", !path ? "cannot resolve key path" : arg,
                                       path ? path : value);
                free(path);
                return result;
            }
            free(*slot);
            *slot = path;
        }
        ++read;
    }
    while (read < *argc) argv[write++] = argv[read++];
    argv[write] = NULL;
    *argc = write;
    return 0;
}

int snx_main(int argc, char** argv) {
    char module[sizeof g_exedir + sizeof g_exename + 16], ini[sizeof g_confdir + sizeof g_exename + 8];
    char base[sizeof g_exename];
    char *keys = NULL, *title_keys = NULL;
    const char *game, *title;
    const SnxModule* mod;
    int status, i;
    hle_init(); /* before anything, the options included, touches g_hle */
    /* The driver's dump-play options also belong to the runtime when it reads the dump in place. */
    status = runtime_key_options(&argc, argv, &keys, &title_keys);
    if (status) {
        status = 2;
        goto done;
    }
    status = snx_startup_options(argc, argv);
    if (status) goto done;
    if (argc == 2 && !strcmp(argv[1], "--settings")) {
        ini_list(g_ini_sections, g_ini_sections_count);
        status = 0;
        goto done;
    }
    if (argc == 2 && !strcmp(argv[1], "--settings=recompile")) {
        ini_list(g_launch_sections, g_launch_sections_count);
        status = 0;
        goto done;
    }
    if (g_opt_gdb && snx_gdb_start(g_opt_gdb_stop, SNX_GDB_PORT)) {
        status = 2;
        goto done; /* before any other thread */
    }
    if (argc == 2 && !strncmp(argv[1], "--settings=", 11)) {
        fprintf(stderr, "scarletnx: %s: not --settings (the game's) or --settings=recompile\n", argv[1]);
        status = 2;
        goto done;
    }
    game = game_option(argc, argv);
    refuse_dumps(argc, argv, game);
    name_game(game ? game : program_path(argv[0]));
    snprintf(module, sizeof module, "%s%s%s", g_exedir, g_exename, MODULE_SUFFIX);
    if (argc == 1 && !snx_module_static && snx_menu && snx_launcher_name(g_exename) && !snx_is_file(module)) {
        status = open_menu(argv[0]);
        goto done;
    }
    /* the game's errors in its log, where a fatal's "the line before" is: a game's process on Android has no stderr */
    snx_error_sink(log_error_line);
    /* the guest's address space before the game's code, which would take a share of a small one: a pack's arena with its
       write alias leaves an iOS app's 13 GiB or so no run of the 5 GiB the guest needs */
    mem_reserve();
    /* the code before anything is written: a mistyped --game leaves no .ini and no log behind */
    mod = load_module(module);
    if (mod->abi != SNX_ABI) {
        WARN("game: its code is %s\n", module);
        fatal("The recompiled code is from another ScarletNX version (ABI %u, where this one runs %u): recompile the "
              "game with this one",
              (unsigned)mod->abi, (unsigned)SNX_ABI);
    }
    game_dirs();
    if (g_arm_mod) mem_layout32(g_arm_mod->aspace); /* before anything sizes or reads guest memory */
    if (mod->title_id) g_hle.kernel.title_id = mod->title_id;
    /* this run's settings: each --key=value before a "--" (-v is log=2); -f and --fullscreen=0|1 through startup.c's
       record of the last given, which a --fullscreen=... here would only repeat */
    for (i = 1; i < argc && strcmp(argv[i], "--"); i++) {
        const char *arg = argv[i], *eq = strchr(arg, '=');
        char key[64];
        if (!strcmp(arg, "-v")) ini_override("log", "2");
        if (strncmp(arg, "--", 2) || !eq || (size_t)(eq - arg - 2) >= sizeof key || !strncmp(arg, "--game=", 7) ||
            !strncmp(arg, "--fullscreen=", 13))
            continue;
        memcpy(key, arg + 2, (size_t)(eq - arg - 2));
        key[eq - arg - 2] = 0;
        ini_override(key, eq + 1);
    }
    if (g_cli_fullscreen >= 0) ini_override("fullscreen", g_cli_fullscreen ? "1" : "0");
    snprintf(ini, sizeof ini, "%s%s.ini", g_confdir, g_exename);
    addon_init(ini, sizeof ini, base, sizeof base);
    ini_open(ini);
    snprintf(g_gamename, sizeof g_gamename, "%s", base[0] ? base : g_exename);
    snprintf(g_datadir, sizeof g_datadir, "%s%s_data/", g_exedir, g_gamename);
    hints_init();
    plat_install_crash_handler();
    clock_start();
    g_mod = mod; /* from here the log is a file, in the state folder */
    title = mod->title && mod->title[0] ? mod->title : g_exename;
#if defined(SNX_TRACY)
    if (g_settings.tracy_profiler) g_opt_tracy_client = 1; /* the .ini's Debug section asks for the client */
#endif
    /* the header the log summary reads (common/utilities/logscan.c): no folder in it, since players attach the log to
       public reports and the folder names their account */
    LOG("ScarletNX - %s\n", title);
    LOG("build: %s\n", snx_build_info());
    {
        char sys[384];
        plat_system_info(sys, sizeof sys);
        LOG("system: %s\n", sys);
    }
    {
        char list[HUD_DEBUG_MAX];
        if (options_debug(INI_CHANGED_DEFAULT, list, sizeof list))
            LOG("debug: %s; shown at the top of the picture while the game runs\n", list);
    }
    /* after the header, so the client's own lines do not come before it and a capture that cannot start still leaves
       a log: it is asked for by a setting, and a run without it is better than no run */
    if (snx_tracy_init(title)) WARN("tracy: no capture this run; it goes on without one\n");
    say_old_layout();
    /* before the JIT's first load (jit_init, arm.c's blocks, the GPU's macros), and only when asked: GDB stops the
       game to read each one */
    if (g_opt_gdb && g_settings.gdb_jit_symbols) {
        snx_gdb_jit_enable();
        LOG("gdb: the code the JIT compiles is named to GDB (gdb_jit=1): it stops the game for each compile while "
            "attached\n");
    }
    if (g_addon) LOG("game: the update of the game in %s, with its settings and saves\n", base);
    snx_ir_slp_vectorizer(g_settings.slp_vectorizer); /* before the JIT's first compile too */
    snx_ir_fpsr_flags(g_settings.fpsr_flags);
    snx_ir_volatile_memory(g_settings.volatile_load_stores);

    fs_init(keys, title_keys); /* the npdm is read through it, and mem_init's layout comes before */
    npdm_layout(); /* the address space type's layout, before mem_init sizes its blocks in it */
    free(keys);
    free(title_keys);
    keys = title_keys = NULL;
#ifdef MODULE_PACK
    g_settings.log_level = 2; /* a phone's log is the only way to see what a run did: everything, until it is steadier */
#endif
    mem_init();
    if (g_settings.memory_fit) { /* before the game allocates: what the process holds is the runtime's alone */
        SnxU64 available = 0, footprint = snx_footprint();
        mem_fit(snx_memory_available(&available) && footprint, available, footprint);
    }
    kernel_init();
    dispatch_init();
    if (g_arm_mod) arm_dispatch_init();
    load_hints();
    if (loader_load()) fatal("Could not load the game executables. See the .log for details.");
    kernel_load_symbols();
    hook_install(); /* before the trace, which wraps what a slot holds, and the JIT, which reads the slots */
    nvn_direct_init(); /* before the trace, which stands aside when it runs */
    nvn_trace_init();
    /* after the trace, which says whether calls must go through the slots it wraps. The JIT compiles A64: a 32-bit
       game's blocks are arm.c's, and its hints are A32 block keys the JIT must not warm-compile */
    if (!g_arm_mod) jit_init();
    read_npdm();
    svc_core_init();
    {
        char version[16];
        am_display_version(version);
        LOG("game: %016llx, version %s\n", (unsigned long long)g_hle.kernel.title_id, version);
    }
    svc_fs_init();
    svc_hid_init();
    svc_vi_init();
    svc_pl_init();
    nv_init();
    romfs_init();
    gpu_init();
    {
        char addons[256];
        addons_line(addons, sizeof addons);
        if (addons[0]) LOG("game: played with %s\n", addons);
        video_init(title, addons);
    }
    if (g_settings.profiler) {
#if defined(_WIN32)
        plat_prof_start();
        LOG("profiler: on, profile.txt every 10 seconds\n");
#else
        LOG("the CPU profiler is Windows only, profile=1 does nothing here\n");
#endif
    }
    prepare_wait(title);
    if (!g_quit)
        kernel_run_main(mod->mods[0].base + mod->mods[0].text_off, mod->main_stack, (int)mod->main_prio,
                        (int)mod->main_core);
    frontend_loop();
    status = 0;
done:
    free(keys);
    free(title_keys);
    return status;
}
