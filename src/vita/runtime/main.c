/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* scarletnx-vita: a VPK extracted into its game's folder, recompiled/<title ID>/ as a Switch or 3DS game's (its app0:
   in app/, its memory card in ux0/, the module <title ID>_code.so, the hints, a copy of the runtime named after the
   title that plays it), compiled when the module is missing or no longer matches, then played. A game dumped as a
   folder (a console's ux0:app/<title ID>, its update in ux0:patch/<title ID>) is linked there instead: app/ and patch/
   name the dump's folders, whose files stay where they are */
#include "vita/runtime/main.h"
#include "common/utilities/game_log.h"
#include "common/utilities/tracy.h"
#include "common/utilities/menu_window.h"
#include "vita/aot/module.h"
#include "vita/gpu/present.h"
#include "vita/hle/hle.h"
#include "vita/loader/vpk.h"
#include "vita/platform/input.h"
#include "vita/runtime/process.h"
#include "common/cpu/recompiler/llvm/jit_settings.h"
#include "common/utilities/build_info.h"
#include "common/utilities/cli.h"
#include "common/utilities/common.h"
#include "common/utilities/error_kind.h"
#include "common/platform/file.h"
#include "common/utilities/game_ini.h"
#include "common/utilities/ini_lines.h"
#include "common/utilities/vpad_consoles.h"
#include "common/utilities/vpad_sdl.h"
#include "common/utilities/console_settings.h"
#include "snx_settings.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* the common options scarletnx-vita takes (common/utilities/cli.h): a VPK is always extracted, and the Vita runtime
   has no keys, frame count or window-less run */
static const unsigned vita_cli = SNX_CLI_BIT(SNX_CLI_EXTRACT) | SNX_CLI_BIT(SNX_CLI_RECOMPILE) |
                                 SNX_CLI_BIT(SNX_CLI_GAME) | SNX_CLI_BIT(SNX_CLI_UPDATE) | SNX_CLI_BIT(SNX_CLI_DATA) |
                                 SNX_CLI_BIT(SNX_CLI_OUTDIR) | SNX_CLI_BIT(SNX_CLI_OUTPUT) |
                                 SNX_CLI_BIT(SNX_CLI_SECONDS) | SNX_CLI_BIT(SNX_CLI_SHOT) |
                                 SNX_CLI_BIT(SNX_CLI_SCRIPT) | SNX_CLI_BIT(SNX_CLI_FULLSCREEN) |
                                 SNX_CLI_BIT(SNX_CLI_HELP) | SNX_CLI_BIT(SNX_CLI_FROM_LAUNCHER) |
                                 SNX_CLI_RECOMPILE_OPTIONS;
static void usage(FILE* out) {
    fputs(
        "Usage: scarletnx-vita [OPTIONS] game.vpk\n"
        "       scarletnx-vita [OPTIONS] app/<title ID>   (a dumped game's folder)\n"
        "       scarletnx-vita [OPTIONS] --game=recompiled/<title ID>\n"
        "       <title ID> [OPTIONS]   (the game's copy of the runtime, in its folder)\n"
        "  game.vpk               Extract the VPK into its game's folder (recompiled/<its title ID> of the data\n"
        "                         folder), compile its code if needed, then play. The folder holds app/ (app0:),\n"
        "                         ux0/ (the memory card), the module <title ID>_code.so and the copy of the runtime\n"
        "                         that plays it\n",
        out);
    snx_cli_help(out, vita_cli);
    fputs(
        "Vita options:\n"
        "  --gpu-driver=NAME      The Vulkan driver package to run with (Android arm64), --freedreno-env=NAME=value;...\n"
        "                         its Freedreno variables, as the Switch's.\n"
        "  --controls=0|1|2       The on-screen controls: 1 shown, 0 hidden, 2 (Auto, the default) shown on a touch\n"
        "                         screen while no gamepad is connected. --controls-scale=N, --controls-snap=0|1,\n"
        "                         --controls-rel-center=0|1, --controls-dpad-slide=0|1, --controls-haptics=0|1 and\n"
        "                         --controls-<ID>-shown=0|1 (ID one of a for Circle, b Cross, x Triangle, y Square,\n"
        "                         l, r, plus for Start, minus for Select, dpad, stick_l, stick_r) as a Switch game's\n"
        "  --touch=0|1|2          The front panel's touch: 2 (default) the fingers and the mouse's left button, 1 the\n"
        "                         fingers, 0 none\n"
        "  --KEY=VALUE            A Vita debug setting: --vita-hle-trace=1 logs each import the runtime lacks (2\n"
        "                         every call), --vita-gxm-trace=1 each scene. And the Switch's --aot-threads=N:\n"
        "                         the game's code compiles on N threads (1 to 1024; the default 0: the computer's\n"
        "                         cores with a quarter left free, or --aot-threads-most=N), the same as --jobs N.\n"
        "                         KEY is the key with a - for each _; the key as written, the older spelling, is still\n"
        "                         read, and said once\n"
        "\n"
        "On the Vita, -O is the level of the blocks LLVM compiles: those this computer's native back end does not make,\n"
        "and every one with --keep-ir. --chunk N puts N instructions in a chunk of LLVM's, whole blocks (default: 512\n"
        "blocks a chunk). --jit-cache FILE is the list of the blocks a run compiled at run time that discovery starts\n"
        "from, another game folder's <title ID>.hints (default: the game's own). --jit-only makes a module of no blocks,\n"
        "the run-time compiler compiling every one, and --keep-ir has LLVM compile them all and keeps each chunk's IR in\n"
        "<title ID>_build of the game's folder.\n"
        "\n"
        "F2, or Android's Back, edits the on-screen controls' layout: a control dragged moves, and one tapped is\n"
        "sized from the bar; Done, Esc, F2 or Back ends it, and <title ID>.ini keeps it. Back twice quits.\n"
        "\n"
        "On the Vita, --update is the dumped game's update folder (default: patch/<title ID> beside its app/<title\n"
        "ID>), --shot saves PATH.ppm, and --script=ms:buttons,... is the setting vita_script.\n",
        out);
}
static bool vpk_path(const char* path) {
    size_t n = strlen(path);
    return n >= 4 && path[n - 4] == '.' && tolower((unsigned char)path[n - 3]) == 'v' &&
           tolower((unsigned char)path[n - 2]) == 'p' && tolower((unsigned char)path[n - 1]) == 'k';
}
static char* last_part(const char* path) {
    size_t n = strlen(path), cut;
    while (n > 1 && (path[n - 1] == '/' || path[n - 1] == '\\')) --n;
    for (cut = n; cut && path[cut - 1] != '/' && path[cut - 1] != '\\'; --cut) continue;
    char* name = snx_format("%.*s", (int)(n - cut), path + cut);
    size_t length = name ? strlen(name) : 0;
    if (length > 4 && !strcmp(name + length - 4, ".exe")) name[length - 4] = 0;
    return name;
}
bool snx_vita_title_name(const char* path) {
    char* name = last_part(path);
    bool title = vita_title_id(name);
    free(name);
    return title;
}
/* the game's folder this program is the copy of: its own folder when named after a title ID with a module beside it */
static char* own_game(const char* argv0) {
    char *name = last_part(argv0), *exe = name && vita_title_id(name) ? snx_executable(argv0) : NULL;
    char* dir = exe ? snx_dirname(exe) : NULL;
    free(exe);
    free(name);
    return dir;
}
static char* game_folder(const char* repo, const char* outdir, const char* argv0, const char* title) {
    char *exe = repo || outdir ? NULL : snx_executable(argv0), *dir = exe ? snx_dirname(exe) : NULL;
    char* data = outdir ? NULL : repo ? snx_strdup(repo) : snx_data_dir(dir);
    char* folder = outdir ? snx_path_join(outdir, title) : data ? snx_recompiled_game(data, title) : NULL;
    char* resolved = folder ? snx_absolute(folder) : NULL;
    if (!resolved) fprintf(stderr, "scarletnx-vita: cannot resolve the game's folder for %s\n", title);
    free(folder);
    free(data);
    free(dir);
    free(exe);
    return resolved;
}
/* the VPK into folder/app: into a staging folder first, renamed into place once complete */
static bool extract(const char* vpk, const char* folder) {
    char *app = snx_path_join(folder, "app"), *stage = snx_path_join(folder, ".app.extracting"), error[512];
    bool ok = app && stage;
    if (ok && snx_is_dir(app)) {
        printf("Using the extraction in %s\n", app);
    } else if (ok) {
        printf("Extracting %s into %s\n", vpk, app);
        if (snx_path_exists(stage)) snx_remove_tree(stage);
        ok = vita_vpk_extract(vpk, stage, error, sizeof error);
        if (!ok) fprintf(stderr, "scarletnx-vita: %s\n", error);
        if (ok && plat_replace_file(stage, app)) {
            fprintf(stderr, "scarletnx-vita: cannot move %s to %s\n", stage, app);
            ok = false;
        }
        if (!ok && stage && snx_path_exists(stage)) snx_remove_tree(stage);
    }
    free(app);
    free(stage);
    return ok;
}
/* folder/name naming target, the dump's folder: a link, else a copy */
static bool link_dump(const char* folder, const char* name, const char* target) {
    char *link = snx_path_join(folder, name), *full = snx_absolute(target);
    bool ok = link && full;
    if (ok && snx_is_link(link)) {
        char* old = snx_absolute(link);
        bool same = old && !strcmp(old, full);
        free(old);
        if (!same) ok = !snx_remove_tree(link);
    }
    if (ok && !snx_path_exists(link)) {
        printf("Linking %s to %s\n", link, full);
        if (snx_symlink(full, link)) {
            printf("Cannot link it: copying %s instead\n", full);
            ok = !snx_copy_tree(full, link);
        }
    }
    if (!ok) fprintf(stderr, "scarletnx-vita: cannot give %s the dump's %s\n", folder, target);
    free(link);
    free(full);
    return ok;
}
/* a dumped game's update: patch/<title ID> beside its app/<title ID> */
static char* dump_patch(const char* app, const char* title) {
    char *full = snx_absolute(app), *parent = full ? snx_dirname(full) : NULL, *root = parent ? snx_dirname(parent) : NULL;
    char made[SNX_PATH_SIZE];
    char* patch = root && !snx_path_format(made, sizeof made, "%s/patch/%s", root, title) ? snx_strdup(made) : NULL;
    free(full);
    free(parent);
    free(root);
    if (patch) {
        char* eboot = snx_path_join(patch, "eboot.bin");
        bool there = eboot && snx_is_file(eboot);
        free(eboot);
        if (!there) {
            free(patch);
            patch = NULL;
        }
    }
    return patch;
}
/* the folder's copy of the runtime, named after the game, as a Switch game's (snx_place_runtime), and system=vita in
   its .ini, which the launcher picks the driver by */
static void deploy_runtime(const char* folder, const char* runtime) {
#ifdef _WIN32
    char* copy = vita_game_file(folder, ".exe");
#else
    char* copy = vita_game_file(folder, "");
#endif
    char *ini = vita_game_file(folder, ".ini"), error[512];
    int placed = runtime && copy ? snx_place_runtime(runtime, copy, error, sizeof error) : 0;
    if (placed == 1) printf("The game's runtime is %s\n", copy);
    if (placed < 0) fprintf(stderr, "scarletnx-vita: %s\n", error);
    if (ini) (void)snx_ini_record(ini, "system", snx_system_word(SNX_SYSTEM_VITA)); /* it says why it could not */
    free(ini);
    free(copy);
}
static int play(const char* folder, const char* ini, bool recompile, double seconds, const char* shot,
                const char* runtime, const SnxCliRecompile* asked) {
    VitaImage image;
    VitaNative native = {0};
    VitaProcess* process = NULL;
    /* the update's eboot.bin when there is one */
    char error[512], eboot[SNX_PATH_SIZE], *module = vita_game_file(folder, VITA_MODULE_SUFFIX);
    int status = 1;
    bool made = !snx_path_format(eboot, sizeof eboot, "%s/patch/eboot.bin", folder);
    if (made && !snx_is_file(eboot)) made = !snx_path_format(eboot, sizeof eboot, "%s/app/eboot.bin", folder);
    if (!made || !module || !vita_image_load(eboot, &image, error, sizeof error)) {
        if (made) fprintf(stderr, "scarletnx-vita: %s\n", module ? error : "out of memory"); /* else said */
        free(module);
        return 1;
    }
    /* Load the game's bundled system modules before compiling. Their code shares the guest address space and
       their exports are dispatched as guest functions by NID, not replaced with fake success returns. */
    static const struct {
        const char* file;
        bool deferred; /* started by the game's own sceKernelLoadStartModule, when it makes one */
    } libraries[] = {{"libc.suprx", false},   {"libfios2.suprx", false}, {"libsmart.suprx", false},
                     {"libult.suprx", false}, {"libfios3.suprx", false}, {"steroid.suprx", true}};
    for (unsigned i = 0; i < sizeof libraries / sizeof *libraries; ++i) {
        char path[SNX_PATH_SIZE];
        int n = snprintf(path, sizeof path, "%s/patch/sce_module/%s", folder, libraries[i].file);
        if (n < 0 || (size_t)n >= sizeof path) continue;
        if (!snx_is_file(path)) {
            n = snprintf(path, sizeof path, "%s/app/sce_module/%s", folder, libraries[i].file);
            if (n < 0 || (size_t)n >= sizeof path) continue;
        }
        if (!snx_is_file(path)) {
            n = snprintf(path, sizeof path, "%s/sce_module/%s", folder, libraries[i].file);
            if (n < 0 || (size_t)n >= sizeof path || !snx_is_file(path)) continue;
        }
        VitaImage library;
        uint32_t variable_base = image.variable_count;
        uint32_t base = 0x71000000u + i * 0x01000000u;
        if (!vita_image_load_at(path, &library, base, variable_base, error, sizeof error)) {
            fprintf(stderr, "scarletnx-vita: cannot load %s: %s\n", libraries[i].file, error);
            vita_image_free(&image);
            free(module);
            return 1;
        }
        if (!vita_image_merge(&image, &library, libraries[i].deferred, error, sizeof error)) {
            fprintf(stderr, "scarletnx-vita: cannot link %s: %s\n", libraries[i].file, error);
            vita_image_free(&library);
            vita_image_free(&image);
            free(module);
            return 1;
        }
        printf("Loaded %s: %u exports, %u imports at 0x%08x\n", libraries[i].file, library.export_count,
               library.import_count, base);
        vita_image_free(&library);
    }
    printf("Module %s: %u imports, %u exports, module_start 0x%08x\n", image.name,
           (unsigned)image.import_count, (unsigned)image.export_count, (unsigned)image.module_start);
    /* An existing module that matches is played as it is, else it is compiled first: decided from its file, before it
       is loaded, which happens once */
    bool current = !recompile && vita_module_current(module, &image);
    if (!current && !asked->quiet) printf("Recompiling %s (Vita AArch32)\n", image.name);
    if ((!current && vita_aot_compile(folder, &image, asked, error, sizeof error)) ||
        !vita_native_open(module, &image, &native, error, sizeof error)) {
        snx_error_note(current ? SNX_ERROR_CODE_OLD : SNX_ERROR_RECOMPILE, "scarletnx-vita", "%s", error);
        goto done;
    }
    size_t hint_count = 0;
    uint32_t* hints = vita_hints_read(folder, &hint_count);
    if (current) {
        /* the blocks earlier runs compiled at run time (the hints) that the module lacks: the run-time compiler
           compiles them again, and a recompile (-r) starts from them, as the 3DS driver's */
        size_t missing = 0;
        for (size_t i = 0; i < hint_count; ++i) missing += !vita_module_block(native.module, hints[i]);
        if (missing)
            printf("%zu blocks earlier runs compiled at run time aren't in the game's module, which a recompile (-r) "
                   "starts from\n",
                   missing);
    } else {
        /* the hints the module has, each once: one discovery refused (not code after all) doesn't ask for a compile
           again */
        size_t kept = 0;
        for (size_t i = 0; i < hint_count; ++i)
            if (vita_module_block(native.module, hints[i])) hints[kept++] = hints[i];
        if (kept < hint_count) printf("%zu hints aren't code: dropped\n", hint_count - kept);
        if (hint_count) vita_hints_write(folder, hints, kept);
    }
    free(hints);
    deploy_runtime(folder, runtime);
    if (recompile) {
        status = 0;
        goto done;
    }
    process = vita_process_new(&image, folder, error, sizeof error);
    if (!process) {
        snx_error_note(SNX_ERROR_BAD_FILE, "scarletnx-vita", "%s", error);
        goto done;
    }
    process->module = native.module;
    char sfo_path[SNX_PATH_SIZE], *title = NULL;
    size_t sfo_size = 0;
    unsigned char* sfo = snx_path_format(sfo_path, sizeof sfo_path, "%s/%s/sce_sys/param.sfo", folder,
                                         snx_is_dir(process->patch) ? "patch" : "app")
                             ? NULL
                             : snx_read_file(sfo_path, &sfo_size);
    title = sfo ? snx_sfo_string(sfo, sfo_size, "TITLE") : NULL;
    /* the window's title as the 3DS driver's and a Switch game's: the game's name, then the build; the log's line with
       the day its commit was made */
    char* header = snx_format("%s - %s", title ? title : image.name, snx_build_info());
    char run[SNX_RUN_LOG_LINE];
    printf("ScarletNX - %s (Vita)\nbuild: %s\n", title ? title : image.name, snx_build_log_line());
    snx_run_log_line(run, (long long)time(NULL));
    printf("%s\n", run);
    vita_present_init(header ? header : image.name, ini, shot);
    free(header);
    free(sfo);
    free(title);
    /* a window someone plays at, not a timed run: without the device it would show nothing */
    if (vita_present_why()[0] && !(seconds > 0)) {
        snx_error_note(SNX_ERROR_GPU_START, "scarletnx-vita", "no Vulkan device for the game's window: %s",
                       vita_present_why());
        vita_present_free();
        status = 1;
        goto done;
    }
    status = vita_run(process, seconds);
    if (vita_present_lost(error, sizeof error)) {
        snx_error_note(SNX_ERROR_GPU_LOST, "scarletnx-vita", "%s", error);
        if (!status) status = 1;
    }
    vita_present_shot(process);
    vita_gxm_free(process); /* the renderer, before the device it draws on */
    vita_present_free();
    snx_tracy_shutdown();       /* a capture's lines before the last */
    if (!status) puts("Exit."); /* a run that ended well says so last, as every driver's */
done:
    snx_tracy_shutdown(); /* a stop before the end: the capture keeps what came */
    vita_process_free(process);
    vita_native_close(&native);
    vita_image_free(&image);
    free(module);
    return status;
}
int snx_vita_main(int argc, char** argv) {
    const char* input = NULL;
    snx_tool_console();
    setvbuf(stdout, NULL, _IONBF, 0);
    /* VITA_LOCKSTEP and VITA_LOCKSTEP_STEPS, which an older version read (docs/user/deprecations.md): for this version still
       vita_lockstep and vita_lockstep_steps (0 steps, which compared none, is vita_lockstep=-1), under the command
       line's own, said once it is read. Removed with tests/architecture.py's ENV_DEPRECATED rows */
    const char *old_at = getenv("VITA_LOCKSTEP"), *old_steps = getenv("VITA_LOCKSTEP_STEPS");
    int old_none = old_steps && !strtoull(old_steps, NULL, 0);
    if (old_at) ini_read(ini_find(g_vita_debug_ini, "vita_lockstep"), old_at);
    if (old_none)
        g_settings.vita_lockstep = -1;
    else if (old_steps)
        ini_read(ini_find(g_vita_debug_ini, "vita_lockstep_steps"), old_steps);
    int old_lockstep = g_settings.vita_lockstep, old_lockstep_steps = g_settings.vita_lockstep_steps;
    SnxCli cli;
    vpad_console(&snx_vpad_vita);
    snx_vpad_sdl_settings(SNX_SYSTEM_VITA);
    snx_cli_init(&cli, "scarletnx-vita", vita_cli);
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        char error[256];
        /* the common options first (common/utilities/cli.h): --script is the setting vita_script */
        int common = snx_cli_take(&cli, argc, argv, &i, error, sizeof error);
        if (common < 0 || common == SNX_CLI_HELP) {
            if (common < 0)
                fprintf(stderr, "%s\n", error);
            else
                usage(stdout);
            snx_cli_free(&cli);
            return common < 0 ? 2 : 0;
        }
        if (common == SNX_CLI_SCRIPT) ini_read(ini_find(g_vita_run_ini, "vita_script"), cli.value[SNX_CLI_SCRIPT]);
        /* the Switch's settings, as --aot-threads= is: the last given counts */
        if (common == SNX_CLI_OPT) ini_read(ini_find(g_recompile_ini, "opt"), cli.value[SNX_CLI_OPT]);
        if (common == SNX_CLI_JOBS) g_settings.aot_threads = cli.jobs;
        if (common) continue;
        /* the on-screen controls' and the front panel's settings, read with the game's own once its folder is known */
        int controls = vpad_arg(a, error, sizeof error);
        if (controls < 0) {
            fprintf(stderr, "scarletnx-vita: %s\n", error);
            snx_cli_free(&cli);
            return 2;
        }
        if (controls) continue;
        /* one of the Switch's settings a Vita game does not take, said with why */
        if (snx_console_arg(SNX_SYSTEM_VITA, a, error, sizeof error) < 0) {
            fprintf(stderr, "scarletnx-vita: %s\n", error);
            snx_cli_free(&cli);
            return 2;
        }
        if (!strncmp(a, "--", 2) && strchr(a, '=')) {
            /* the Vita debug and run tables', --press-len= and --shot-every= being vita_press_len and vita_shot_every
               (a Switch game's are g_unattended_ini's); the Switch's it takes are vpad_arg's above */
            char key[64];
            const char* value = settings_arg(a, key, sizeof key);
            const IniOpt* o = NULL;
            /* the recompile's threads, the Switch's setting: a value it does not take stops here, as there */
            if (value && (!strcmp(key, "aot_threads") || !strcmp(key, "aot_threads_most"))) {
                o = ini_find(g_recompile_ini, key);
                if (ini_check(o, value, error, sizeof error)) {
                    fprintf(stderr, "scarletnx-vita: %s\n", error);
                    snx_cli_free(&cli);
                    return 2;
                }
            } else if (value) {
                o = ini_find(g_vita_run_ini, !strcmp(key, "press_len")    ? "vita_press_len"
                                             : !strcmp(key, "shot_every") ? "vita_shot_every"
                                                                          : key);
                if (!o) o = ini_find(g_vita_debug_ini, key);
            }
            if (o)
                ini_read(o, value);
            else {
                fprintf(stderr, "scarletnx-vita: unknown setting: %s\n", a);
                snx_cli_free(&cli);
                return 2;
            }
        } else if (a[0] == '-' || input) {
            fprintf(stderr, "scarletnx-vita: unexpected argument: %s\n", a);
            usage(stderr);
            snx_cli_free(&cli);
            return 2;
        } else
            input = a;
    }
    /* the common options' values: --update is the dumped game's update folder, one */
    if (cli.update_count > 1) {
        fputs("scarletnx-vita: --update is given once, the dumped game's update folder\n", stderr);
        snx_cli_free(&cli);
        return 2;
    }
    const char *output = cli.value[SNX_CLI_OUTPUT], *outdir = cli.value[SNX_CLI_OUTDIR];
    const char *repo = cli.value[SNX_CLI_DATA], *game = cli.value[SNX_CLI_GAME], *shot = cli.value[SNX_CLI_SHOT];
    const char* patch = cli.update_count ? cli.updates[0] : NULL;
    bool extract_only = cli.flags & SNX_CLI_BIT(SNX_CLI_EXTRACT);
    /* the recompile's options, for the game's code, which -x makes none of */
    SnxCliRecompile asked = snx_cli_recompile(&cli);
    if (asked.given && extract_only) {
        fprintf(stderr, "scarletnx-vita: %s is the recompile's, and -x compiles no code\n", asked.given);
        snx_cli_free(&cli);
        return 2;
    }
    bool recompile = cli.flags & SNX_CLI_BIT(SNX_CLI_RECOMPILE);
    double seconds = cli.seconds;
    /* a stop shows its panel where someone is at the window: not a recompile, whose launcher says it, nor a timed run */
    snx_error_note_quiet(!!(cli.flags & SNX_CLI_BIT(SNX_CLI_RECOMPILE)) || seconds > 0);
    snx_error_launched(cli.value[SNX_CLI_FROM_LAUNCHER] != NULL);
    snx_cli_free(&cli);
    if (old_at)
        snx_error("VITA_LOCKSTEP=%s is deprecated, %s: vita_lockstep=%d (docs/user/deprecations.md)", old_at,
                  g_settings.vita_lockstep != old_lockstep ? "and the command line gives vita_lockstep first"
                  : old_none                               ? "and VITA_LOCKSTEP_STEPS=0 compared none"
                                                           : "and this run reads it as --vita-lockstep",
                  g_settings.vita_lockstep);
    if (old_none)
        snx_error("VITA_LOCKSTEP_STEPS=%s is deprecated, and compared no instruction group, which "
                  "vita_lockstep_steps=0 does not mean (no limit): vita_lockstep=%d (docs/user/deprecations.md)",
                  old_steps, g_settings.vita_lockstep);
    else if (old_steps)
        snx_error("VITA_LOCKSTEP_STEPS=%s is deprecated, %s: vita_lockstep_steps=%d (docs/user/deprecations.md)", old_steps,
                  g_settings.vita_lockstep_steps != old_lockstep_steps
                      ? "and the command line gives vita_lockstep_steps first"
                      : "and this run reads it as --vita-lockstep-steps",
                  g_settings.vita_lockstep_steps);
    char *runtime = snx_executable(argv[0]), *folder = game ? snx_strdup(game) : input ? NULL : own_game(argv[0]);
    int status = 1;
    if (!input && !folder) {
        usage(stderr);
        free(runtime);
        return 2;
    }
    if (input) {
        bool dump = snx_is_dir(input);
        if (!dump && (!vpk_path(input) || !snx_is_file(input))) {
            fprintf(stderr, "scarletnx-vita: %s is neither a .vpk file nor a dumped game's folder\n", input);
            free(runtime);
            return 2;
        }
        char error[512] = "no sce_sys/param.sfo";
        size_t size = 0;
        char sfo_path[SNX_PATH_SIZE];
        unsigned char* sfo = dump ? (snx_path_format(sfo_path, sizeof sfo_path, "%s/sce_sys/param.sfo", input)
                                         ? NULL
                                         : snx_read_file(sfo_path, &size))
                                  : vita_vpk_member(input, "sce_sys/param.sfo", &size, error, sizeof error);
        char* title = sfo ? snx_sfo_string(sfo, size, "TITLE_ID") : NULL;
        free(sfo);
        if (!vita_title_id(title)) {
            fprintf(stderr, "scarletnx-vita: %s has no title ID in its param.sfo%s%s\n", input, sfo ? "" : ": ",
                    sfo ? "" : error);
            free(title);
            free(runtime);
            return 2;
        }
        folder = output ? snx_strdup(output) : game_folder(repo, outdir, argv[0], title);
        char* update = !dump ? NULL : patch ? snx_strdup(patch) : dump_patch(input, title);
        free(title);
        bool ready = folder && !snx_mkdirs(folder) &&
                     (dump ? link_dump(folder, "app", input) && (!update || link_dump(folder, "patch", update))
                           : extract(input, folder));
        if (ready && update) printf("The update is %s\n", update);
        free(update);
        if (!ready) goto done;
        if (extract_only) {
            status = 0;
            goto done;
        }
    }
    char *ini = vita_game_file(folder, ".ini"), *name = last_part(folder), logged[1600];
    /* the game's log, before any thread of the run's: every line from here in <name>.log too */
    if (name && snx_game_log_start(folder, name, &g_settings.log_size_limit, logged, sizeof logged))
        fprintf(stderr, "scarletnx-vita: %s: the run's lines are on the terminal alone\n", logged);
    vpad_set_say(vita_present_say);
    if (name) vpad_settings(folder, name, argc, argv);
    if (name) snx_menu_window_open(SNX_SYSTEM_VITA, folder, name);       /* the game's menu: Back, F2 or a pad's Home */
    if (name && snx_tracy_console(folder, name, seconds > 0))
        fputs("scarletnx-vita: tracy: no capture this run; it goes on without one\n", stderr);
    if (snx_cli_fullscreen_flag(argc, argv)) g_settings.fullscreen = 1;  /* -f's over the settings' */
    if (g_settings.button_map[0]) vita_input_map(g_settings.button_map); /* the input's to read, as the 3DS's */
    free(name);
    snx_jit_settings("scarletnx-vita", g_settings.slp_vectorizer, g_settings.perf_map, g_settings.gdb_jit_symbols);
    status = play(folder, ini, recompile, seconds, shot, runtime, &asked);
    free(ini);
done:
    free(folder);
    free(runtime);
    return status;
}
