/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime's startup: which game to run, its recompiled code, its folders and settings, the console's parts in the
   order they depend on each other, the game's main thread; then the frontend's loop at the display's rate until the
   run ends, saving what outlives it */
#include "nx/runtime/main.h"
#include "common/utilities/build_info.h"
#include "common/utilities/common.h"
#include "common/utilities/ini_lines.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/nvn_trace.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/filesystem/fs.h"
#include "nx/hle/hle.h"
#include "nx/hle/kernel/kernel.h"
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
#include "nx/runtime/rtapi.h"
#include "nx/runtime/startup.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define MODULE_SUFFIX "_code.dll"
#else
#define MODULE_SUFFIX "_code.so"
#endif

char g_datadir[1024], g_exename[256], g_exedir[1024];
char g_confdir[1024], g_statedir[1024], g_cachedir[1024];
volatile int g_quit;
int (*snx_menu)(const char* self);
const SnxModule* g_mod;
GuestFn* g_disp;

/* A build that links a game's code into the runtime defines it; none does yet (src/nx/CMakeLists.txt) */
const SnxModule* snx_module_static(void) __attribute__((weak));

typedef const SnxModule* (*ModuleFn)(void);
typedef const SnxArmModule* (*ArmModuleFn)(void);

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

static const SnxModule g_other_version; /* abi 0: a 32-bit module of another version, refused as such below */

static const SnxModule* load_module(const char* path) {
    void *lib, *sym;
    ModuleFn get;
    ArmModuleFn get_arm;
    const SnxModule* m;
    if (snx_module_static) return snx_module_static();
    if (!snx_is_file(path))
        fatal("Cannot find the recompiled code: %s. A copy of the runtime runs the game it is named after, with "
              "that code beside it, or --game=PATH names the game (--game=games/Game)",
              path);
    lib = plat_dl_open(path);
    if (!lib) fatal("%s could not be loaded: %s", path, plat_dl_error());
    if ((sym = plat_dl_sym(lib, "snx_module")) != NULL) {
        memcpy(&get, &sym, sizeof get); /* ISO C has no conversion from an object pointer to a function's */
        m = get();
    } else if ((sym = plat_dl_sym(lib, "snx_module_arm")) != NULL) {
        memcpy(&get_arm, &sym, sizeof get_arm);
        m = arm_module(get_arm());
        if (!m) m = &g_other_version;
    } else
        fatal("%s is not a ScarletNX game module", path);
    if (!m) fatal("%s could not be loaded: it describes no game", path);
    return m;
}

/* the dispatch table: a function for each code word the recompiler compiled one for */
static void dispatch_init(void) {
    u64 words, i;
    g_code_lo = g_mod->code_lo;
    g_code_hi = g_mod->code_hi;
    if (g_code_hi <= g_code_lo) return;
    words = (g_code_hi - g_code_lo) / 4;
    if (words > ((size_t)-1) / sizeof *g_disp) fatal("The game's code range is too large");
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
        return;
    }
    snprintf(out, cap, "%s", g_exedir);
    if (n) {
        snprintf(here, sizeof here, "%.*s", (int)(n - 1), g_exedir);
        moved = snx_user_path(here, kind);
        if (moved && strcmp(moved, here)) {
            if (snx_mkdirs(moved))
                WARN("%s cannot be created, so the game's %s stay beside it\n", moved, what);
            else
                snprintf(out, cap, "%s/", moved);
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

/* nothing is moved: a game an older version laid out beside the others (its folder named otherwise) runs where it
   is, each start saying where its folder goes */
static void say_old_layout(void) {
    char* full = snx_absolute(g_exedir[0] ? g_exedir : ".");
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
        nvn_trace_report();
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
        saved = 1;
    }
    pm_unlock(&lock);
}

/* ---- the frontend's loop ---- */

#define STALL_NS ((u64)15000000000u) /* this long without a new frame is worth a line and a dump */

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

static void frontend_loop(void) {
    const u64 period = 1000000000u / VI_REFRESH_HZ;
    u64 tick = 0, next = plat_ns(), now, changed = next;
    int frames = 0, stalls = 0;
    while (!g_quit) {
        int shown;
        video_poll();
        hid_update();
        vi_vsync_tick();
        if (++tick % 60 == 0) ini_poll(0);
        frontend_frame();
        /* here and not on the GPU thread, where writing the pipeline cache would hold up every channel */
        if (tick % 300 == 0) {
            save_hints();
            if (g_renderer->save) g_renderer->save(0);
        }
        if (tick % 900 == 0) write_threads(tick / VI_REFRESH_HZ);
        now = plat_ns();
        shown = __atomic_load_n(&g_frames, __ATOMIC_RELAXED);
        if (shown != frames) {
            frames = shown;
            changed = now;
        } else if (now - changed >= STALL_NS && stalls < 4) {
            stalls++;
            changed = now;
            WARN("No new frame for %llu seconds (%d shown, %llu GPU submissions): the game's threads\n",
                 (unsigned long long)(STALL_NS / 1000000000u), frames, (unsigned long long)g_gpu_submits);
            kernel_dump_threads();
            gpu_report_stall();
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
    plat_exit_now(0);
}

/* ---- startup ---- */

/* the launcher's menu, for a program started with nothing to run under one of its own names, with no game code */
static int open_menu(const char* argv0) {
    char* self = strpbrk(argv0, "/\\") && snx_is_file(argv0) ? snx_absolute(argv0) : snx_executable(argv0);
    int status = snx_menu(self ? self : argv0);
    free(self);
    return status;
}

int snx_main(int argc, char** argv) {
    char module[sizeof g_exedir + sizeof g_exename + 16], ini[sizeof g_confdir + sizeof g_exename + 8];
    const char *game, *title;
    const SnxModule* mod;
    int status, i;
    hle_init(); /* before anything, the options included, touches g_hle */
    status = snx_startup_options(argc, argv);
    if (status) return status;
    if (argc == 2 && !strcmp(argv[1], "--settings")) {
        ini_list(g_ini_sections, g_ini_sections_count);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--settings=recompile")) {
        ini_list(g_launch_sections, g_launch_sections_count);
        return 0;
    }
    if (g_opt_gdb && snx_gdb_start(g_opt_gdb_stop, SNX_GDB_PORT)) return 2; /* before any other thread */
    game = game_option(argc, argv);
    refuse_dumps(argc, argv, game);
    name_game(game ? game : program_path(argv[0]));
    snprintf(module, sizeof module, "%s%s%s", g_exedir, g_exename, MODULE_SUFFIX);
    if (argc == 1 && !snx_module_static && snx_menu && snx_launcher_name(g_exename) && !snx_is_file(module))
        return open_menu(argv[0]);
    /* the code before anything is written: a mistyped --game leaves no .ini and no log behind */
    mod = load_module(module);
    if (mod->abi != SNX_ABI)
        fatal("The recompiled code is from another ScarletNX version: recompile the game with this one (%s)",
              module);
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
    ini_open(ini);
    snprintf(g_datadir, sizeof g_datadir, "%s%s_data/", g_exedir, g_exename);
    hints_init();
    plat_install_crash_handler();
    clock_start();
    g_mod = mod; /* from here the log is a file, in the state folder */
    title = mod->title && mod->title[0] ? mod->title : g_exename;
    /* the header the log summary reads (common/utilities/logscan.c): no folder in it, since players attach the log to
       public reports and the folder names their account */
    LOG("ScarletNX - %s\n", title);
    LOG("build: %s\n", snx_build_info());
    {
        char sys[384];
        plat_system_info(sys, sizeof sys);
        LOG("system: %s\n", sys);
    }
    say_old_layout();

    fs_init(); /* the npdm is read through it, and mem_init's layout comes before */
    mem_init();
    kernel_init();
    dispatch_init();
    if (g_arm_mod) arm_dispatch_init();
    load_hints();
    if (loader_load()) fatal("Could not load the game executables. See the .log for details.");
    kernel_load_symbols();
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
    video_init(title);
    if (g_settings.profiler) {
#if defined(_WIN32)
        plat_prof_start();
        LOG("profiler: on, profile.txt every 10 seconds\n");
#else
        LOG("the CPU profiler is Windows only, profile=1 does nothing here\n");
#endif
    }
    kernel_run_main(mod->mods[0].base + mod->mods[0].text_off, mod->main_stack, (int)mod->main_prio,
                    (int)mod->main_core);
    frontend_loop();
    return 0;
}
