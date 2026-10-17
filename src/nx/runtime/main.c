/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
#define _GNU_SOURCE
/* startup: load GAME_code.dll/.so, set up memory + services + main thread, then run the frontend
   loop (window, input, vsync). */
#include "nx/runtime/main.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/options.h"
#include "nx/runtime/gdbstub.h"
#include "nx/runtime/guest_jit.h"
#include "nx/runtime/rtapi.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/nvn_trace.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/filesystem/fs.h"
#include "nx/hle/services/hid.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/hle/services/services.h"
#include "nx/hle/services/vi.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const SnxModule* g_mod;
GuestFn* g_disp;
char g_datadir[1024], g_exename[256], g_exedir[1024], g_confdir[1024], g_statedir[1024], g_cachedir[1024];
volatile int g_quit;
int g_frames;
/* ============================================================ code module */
typedef const SnxModule* (*ModFn)(void);
#if !defined(_WIN32)
__attribute__((weak)) const SnxModule* snx_module_static(void);
#else
const SnxModule* snx_module_static(void) __attribute__((weak));
#endif
/* the game module this runtime runs, named after it */
static void code_path(char* p, size_t size) {
#if defined(_WIN32)
    snprintf(p, size, "%s%s_code.dll", g_exedir, g_exename);
#else
    snprintf(p, size, "%s%s_code.so", g_exedir, g_exename);
#endif
}
int (*snx_menu)(const char* self);
/* Started with nothing to run and no game of its name: a double-clicked AppImage or scarletnx.exe, or the desktop
   entry. The menu opens instead of the error about a missing game, and runs games with this program. In an AppImage
   argv[0] is the launcher in its mount's bin/ (sharun's), which sets up the bundled libraries for whatever it runs
   from there: that is the program when argv[0] names one, the real executable otherwise (a PATH lookup) */
static int open_menu(const char* argv0) {
    int named = (strchr(argv0, '/') || strchr(argv0, '\\')) && snx_is_file(argv0);
    char* self = named ? snx_absolute(argv0) : snx_executable(argv0);
    int result = snx_menu(self);
    free(self);
    return result;
}
static int load_code(void) {
    if (snx_module_static) {
        g_mod = snx_module_static();
        return g_mod ? 0 : -1;
    }
    char p[1200];
    code_path(p, sizeof p);
    FILE* probe = fopen(p, "rb");
    if (!probe) {
        WARN("Cannot find the recompiled code: %s\n", p);
        WARN("The runtime runs the game it is named after, from the game's folder: a copy named Game next to "
             "Game_code.so runs that game. Or name the game with --game=PATH, for example "
             "--game=games/Game.\n");
        return -1;
    }
    fclose(probe);
    void* h = plat_dl_open(p);
    if (!h) { /* it's there but doesn't load: a library it needs is missing, or it was built for another system */
        WARN("Cannot load the recompiled code: %s\n%s\n", p, plat_dl_error());
        return -1;
    }
    ModFn f = (ModFn)plat_dl_sym(h, "snx_module");
    if (!f) { /* a 32-bit game's (nx/runtime/arm.c), whose own version arm_module checks */
        const SnxArmModule* (*arm)(void) = (const SnxArmModule* (*)(void))plat_dl_sym(h, "snx_module_arm");
        if (arm) {
            static SnxModule other; /* abi 0: from another version, which snx_main reports */
            g_mod = arm_module(arm());
            if (!g_mod) g_mod = &other;
            return 0;
        }
        WARN("%s is not a ScarletNX module\n", p);
        return -1;
    }
    g_mod = f();
    return g_mod ? 0 : -1;
}

/* the folder one kind of the game's files goes in, with its separator: beside the game, or for a game in an installed
   build's data folder the XDG folder of that kind (utilities/common.h) */
static void user_folder(char* dir, size_t size, const char* games, int kind) {
    char* moved = games ? snx_user_path(games, kind) : NULL;
    int apart = moved && strcmp(moved, games);
    if (apart && snx_mkdirs(moved)) {
        WARN("cannot create %s, so the game's files of that kind stay beside it\n", moved);
        apart = 0;
    }
    if (apart)
        snprintf(dir, size, "%s/", moved);
    else
        snprintf(dir, size, "%s", g_exedir);
    free(moved);
}
/* the settings, the log and threads.txt, and the pipeline cache. A game an older version laid out beside the others
   runs where it is, a line in the log saying where its folder goes each start */
static void user_folders(void) {
    char* games = snx_absolute(g_exedir[0] ? g_exedir : ".");
    const char* folder = games ? games + strlen(games) : NULL;
    while (folder && folder > games && folder[-1] != '/' && folder[-1] != '\\') --folder; /* the folder's own name */
    user_folder(g_confdir, sizeof g_confdir, games, SNX_USER_CONFIG);
    user_folder(g_statedir, sizeof g_statedir, games, SNX_USER_STATE);
    user_folder(g_cachedir, sizeof g_cachedir, games, SNX_USER_CACHE);
    if (folder && strcmp(folder, g_exename)) { /* not in a folder of its own name */
        char id[17];
        WARN(
            "%s/%s and its files were left by an older version: this one keeps them in %s/%s/ (docs/DEPRECATIONS.md)\n",
            games, g_exename, games, snx_title_text(g_mod->title_id, id) ? id : g_exename);
    }
    free(games);
}

static void build_dispatch(void) {
    g_code_lo = g_mod->code_lo;
    g_code_hi = g_mod->code_hi;
    u64 n = (g_code_hi - g_code_lo) / 4;
    g_disp = calloc(n ? n : 1, sizeof(GuestFn));
    if (!g_disp) fatal("Out of memory for the table of the game's functions");
    for (u32 i = 0; i < g_mod->nentries; i++) {
        u64 a = g_mod->entry_addr[i];
        if (a >= g_code_lo && a < g_code_hi) g_disp[(a - g_code_lo) / 4] = g_mod->entry_fn[i];
    }
}

static void services_init(void) {
    svc_core_init();
    svc_hid_init();
    svc_fs_init();
    svc_vi_init();
    nv_init();
    svc_pl_init();
}

static void split_path(const char* argv0) {
    char p[1024];
    strncpy(p, argv0 ? argv0 : "", 1023);
    p[1023] = 0;
    /* --game=games/Catherine/ names games/Catherine */
    for (size_t n = strlen(p); n > 1 && (p[n - 1] == '/' || p[n - 1] == '\\'); n--) p[n - 1] = 0;
    char *s1 = strrchr(p, '/'), *s2 = strrchr(p, '\\'), *s = s1 > s2 ? s1 : s2;
    const char* base = s ? s + 1 : p;
    /* --game=games/0100ABCD12345000 names the game in that folder, games/0100ABCD12345000/0100ABCD12345000 */
    size_t n = strlen(p), b = strlen(base);
    if (b && n + 1 + b < sizeof p && snx_is_dir(p)) {
        memmove(p + n + 1, base, b + 1);
        p[n] = '/';
        s = p + n;
        base = s + 1;
    }
    strncpy(g_exename, base, 255);
    char* dot = strrchr(g_exename, '.');
    if (dot && (!strcmp(dot, ".exe") || !strcmp(dot, ".EXE") || !strcmp(dot, ".AppImage"))) *dot = 0;
    /* --game=games/Catherine_code.so names the same game as --game=games/Catherine */
    n = strlen(g_exename);
    if (n > 8 && (!strcmp(g_exename + n - 8, "_code.so"))) g_exename[n - 8] = 0;
    if (n > 9 && (!strcmp(g_exename + n - 9, "_code.dll"))) g_exename[n - 9] = 0;
    if (s) {
        s[1] = 0;
        strncpy(g_exedir, p, 1023);
    } else {
#if defined(_WIN32)
        g_exedir[0] = 0;
#else
        strcpy(g_exedir, "./"); /* a bare file name would make dlopen search the library path, not this folder */
#endif
    }
}

/* main.npdm: the SystemResourceSize (newer SDKs only init their VM allocator when > 0), and the cores and
   priorities the kernel capabilities of its ACI0 give the process */
static void read_npdm(void) {
    FILE* f = fs_open_read("exefs/main.npdm", NULL);
    u8 h[0x4000];
    size_t n = 0;
    if (f) {
        n = fread(h, 1, sizeof h, f);
        fclose(f);
    }
    if (n >= 0x80 && !memcmp(h, "META", 4)) {
        u32 v, aci, acisz;
        memcpy(&v, h + 0x14, 4);
        g_sysres_size = v;
        memcpy(&aci, h + 0x70, 4);
        memcpy(&acisz, h + 0x74, 4);
        u32 ko = 0, ks = 0;
        if (acisz >= 0x38 && aci <= n && acisz <= n - aci && !memcmp(h + aci, "ACI0", 4)) {
            memcpy(&ko, h + aci + 0x30, 4);
            memcpy(&ks, h + aci + 0x34, 4);
        }
        for (u32 o = 0; ks <= acisz && ko <= acisz - ks && o + 4 <= ks; o += 4) {
            u32 cap;
            memcpy(&cap, h + aci + ko + o, 4);
            if ((cap & 0xF) != 0x7) continue; /* ThreadInfo: the lowest and highest priority and core */
            u32 lo_prio = (cap >> 4) & 63, hi_prio = (cap >> 10) & 63, min_core = (cap >> 16) & 255,
                max_core = (cap >> 24) & 255;
            if (hi_prio > lo_prio || min_core > max_core || max_core >= NCORES) break;
            g_core_mask = (~0ull >> (63 - max_core)) & (~0ull << min_core);
            g_prio_mask = (~0ull >> (63 - lo_prio)) & (~0ull << hi_prio);
            break;
        }
    }
    LOG("npdm: system resource %.1f MB, cores %llx, priorities %llx\n", g_sysres_size / 1048576.0,
        (unsigned long long)g_core_mask, (unsigned long long)g_prio_mask);
}

/* players point --game at the dump itself (--game=ARMS.xci), and the runtime then looks for ARMS.xci_code.so. It only
   runs recompiled code, so name what is missing instead of the .so it would have looked for */
static int is_dump(const char* path) {
    static const char* const ext[] = {".xci", ".xcz", ".nsp", ".nsz", ".nca", ".nro", ".nso"};
    const char* dot = strrchr(path, '.');
    if (!dot || strpbrk(dot, "/\\")) return 0;
    for (size_t i = 0; i < sizeof ext / sizeof *ext; i++) {
        size_t k = 0;
        while (ext[i][k] && (dot[k] | 0x20) == ext[i][k]) k++;
        if (!ext[i][k] && !dot[k]) return 1;
    }
    return 0;
}

void snx_save_before_exit(void) {
    static PMutex lock = PMUTEX_INIT;
    static int done;
    pm_lock(&lock);
    if (!done) {
        done = 1;
        save_hints();
        /* the process ends without shutting the renderer down, so it saves what it keeps across runs (the pipeline
           cache) here */
        if (g_renderer && g_renderer->save) g_renderer->save();
        nvn_trace_report();
        jit_report();
    }
    pm_unlock(&lock);
}

int snx_main(int argc, char** argv) {
    int startup = snx_startup_options(argc, argv);
    if (startup) return startup;
    if (argc == 2 && !strcmp(argv[1], "--settings")) { /* what the menu's Settings offer */
        ini_list();
        return 0;
    }
    /* Start the helper before creating any host threads. */
    if (g_opt_gdb && snx_gdb_start(g_opt_gdb_stop, SNX_GDB_PORT)) return 2;
    /* In an AppImage argv[0] points into its mount (APPDIR). APPIMAGE is the file that was run, so a copy named after
       the game (games/Catherine or games/Catherine.AppImage) finds games/Catherine_code.so like the bare runtime does.
       Both variables are inherited, so they only count when this runtime is the one inside the mount */
    const char *appimage = getenv("APPIMAGE"), *appdir = getenv("APPDIR");
    int in_appimage =
        appimage && *appimage && appdir && *appdir && argv[0] && !strncmp(argv[0], appdir, strlen(appdir));
    /* --game=PATH names the game the way the runtime's own name does (games/Catherine runs games/Catherine_code.so
       with games/Catherine_data/), so one runtime or AppImage runs any game without a copy named after it */
    /* the dump can be named with --game or on its own */
    const char *game = NULL, *dump = NULL;
    for (int i = 1; i < argc && strcmp(argv[i], "--"); i++)
        if (!strncmp(argv[i], "--game=", 7) && argv[i][7])
            game = argv[i] + 7;
        else if (argv[i][0] != '-' && is_dump(argv[i]))
            dump = argv[i];
    if (game && is_dump(game)) dump = game;
    /* a dump names no game: the runtime keeps its own name. The fatal error goes to stderr and the box only, since the
       log is only created once a game's code has loaded */
    split_path(game && !dump ? game : in_appimage ? appimage : argv[0]);
    if (dump)
        fatal("%s is a game dump. ScarletNX does not run dumps: it runs a game recompiled from one. Open the dump "
              "without --game to extract, recompile and play it (scarletnx Game.nsp), or point --game at a game "
              "recompiled from it, for example --game=games/Game.",
              dump);
    /* the launcher started bare opens the menu; a game's copy without its code says so, as below */
    if (argc == 1 && !snx_module_static && snx_menu && snx_launcher_name(g_exename)) {
        char p[1200];
        code_path(p, sizeof p);
        if (!snx_is_file(p)) return open_menu(argv[0]);
    }
    /* loaded before the .ini is opened, which writes one: a mistyped --game leaves no stray settings file */
    if (load_code()) fatal("Could not load the game's recompiled code.");
    user_folders(); /* before the log's first line */
    if (g_mod->abi != SNX_ABI) fatal("The recompiled code is from another ScarletNX version.");
    /* a 32-bit game's address space is below 4 GiB, laid out as the console lays it out, before anything reads it */
    if (g_arm_mod) mem_layout32(g_arm_mod->aspace);
    /* --key=value sets a setting for this run, over the .ini. The value is used where it is, so a long --script
       isn't cut */
    for (int i = 1; i < argc && strcmp(argv[i], "--"); i++) {
        const char* e = strchr(argv[i], '=');
        if (!strcmp(argv[i], "-v"))
            ini_override("log", "2");
        else if (!strncmp(argv[i], "--", 2) && e && e - argv[i] - 2 < 64) {
            char k[64];
            memcpy(k, argv[i] + 2, (size_t)(e - argv[i] - 2));
            k[e - argv[i] - 2] = 0;
            ini_override(k, e + 1);
        }
    }
    /* -f, --fullscreen[=1] or --fullscreen=0, the last of them as startup.c read it, sets this run's fullscreen over the
       .ini's (after the loop, which saw the --fullscreen= ones as they came); without any the .ini says */
    if (g_cli_fullscreen >= 0) ini_override("fullscreen", g_cli_fullscreen ? "1" : "0");
    char ini[1400];
    snprintf(ini, sizeof ini, "%s%s.ini", g_confdir, g_exename);
    ini_open(ini);
    snprintf(g_datadir, sizeof g_datadir, "%s%s_data/", g_exedir, g_exename);
    hints_init();
    plat_install_crash_handler();
    clock_start();
    /* the game as it is shown, its files being named by its title ID: the menu takes its recent game's name here */
    LOG("ScarletNX - %s\n", g_mod->title ? g_mod->title : g_exename);
    g_title_id = g_mod->title_id;
    fs_init();
    mem_init();
    kernel_init();
    build_dispatch();
    if (g_arm_mod) arm_dispatch_init();
    load_hints();
    if (loader_load()) fatal("Could not load the game executables from %sexefs/", g_datadir);
    kernel_load_symbols();
    nvn_trace_init();
    /* after the trace, which says whether calls must go through the slots it wraps. The JIT compiles A64: a 32-bit
       game's blocks are arm.c's, and its hints are A32 block keys the JIT must not warm-compile */
    if (!g_arm_mod) jit_init();
    read_npdm();
    services_init();
    romfs_init();
    gpu_init();
    video_init(g_mod->title ? g_mod->title : g_exename);
    if (g_opt_prof) {
#if defined(_WIN32)
        plat_prof_start();
        LOG("profile: active (profile.txt every 10 s)\n");
#else
        LOG("profile: the CPU profiler is Windows only, profile=1 does nothing here\n");
#endif
    }
    kernel_run_main(g_mod->mods[0].base + g_mod->mods[0].text_off, g_mod->main_stack ? g_mod->main_stack : 0x100000,
                    (int)g_mod->main_prio, (int)g_mod->main_core);
    /* 60Hz frontend loop */
    u64 next = plat_ns();
    int tick = 0;
    while (!g_quit) {
        video_poll();
        hid_update();
        vi_vsync_tick();
        if (tick % 60 == 0) ini_poll(0);
        frontend_frame();
        if (++tick % 300 == 0) {
            save_hints();
            /* the pipeline cache, when it grew: here, as a file write on the GPU thread would hold up every channel */
            if (g_renderer && g_renderer->save) g_renderer->save();
        }
        /* dump thread states to threads.txt every 15s, useful when something hangs */
        if (tick % 900 == 0) {
            char p[1200];
            snprintf(p, sizeof p, "%sthreads.txt", g_statedir);
            FILE* f = fopen(p, "w");
            if (f) {
                fprintf(f, "second %d, frames %d\n", tick / 60, __atomic_load_n(&g_frames, __ATOMIC_RELAXED));
                tls_log_redirect = f;
                kernel_dump_threads();
                tls_log_redirect = NULL;
                fclose(f);
            }
        }
        {
            static int last_frames = -1, still = 0, dumps = 0;
            int frames = __atomic_load_n(&g_frames, __ATOMIC_RELAXED); /* counted by the presenting thread */
            if (frames != last_frames) {
                last_frames = frames;
                still = 0;
            } else if (++still == 60 * 15 && dumps < 4) {
                dumps++;
                still = 0;
                LOG("warning: %d s without a new image (frames=%d, GPU submits=%llu)\n", 15, frames,
                    (unsigned long long)g_gpu_submits);
                kernel_dump_threads();
            }
        }
        next += 1000000000ull / VI_REFRESH_HZ;
        u64 now = plat_ns();
        if (next > now)
            plat_sleep_ns((i64)(next - now));
        else if (now - next > 100000000ull)
            next = now;
    }
    ini_poll(1); /* what changed in the game since the last poll (F4, the menu) is saved: it runs once a second */
    snx_save_before_exit();
    LOG("Exit.\n");
    plat_exit_now(0); /* the guest threads are still running: they aren't joined */
}
