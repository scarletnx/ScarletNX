/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* scarletnx-vita: a VPK extracted into its game's folder, recompiled/<title ID>/ as a Switch or 3DS game's (its app0:
   in app/, its memory card in ux0/, the module <title ID>_code.so, the hints, a copy of the runtime named after the
   title that plays it), compiled when the module is missing or no longer matches, then played. A game dumped as a
   folder (a console's ux0:app/<title ID>, its update in ux0:patch/<title ID>) is linked there instead: app/ and patch/
   name the dump's folders, whose files stay where they are */
#include "vita/runtime/main.h"
#include "vita/aot/module.h"
#include "vita/gpu/present.h"
#include "vita/hle/hle.h"
#include "vita/loader/vpk.h"
#include "vita/runtime/process.h"
#include "common/utilities/build_info.h"
#include "common/utilities/common.h"
#include "common/utilities/game_ini.h"
#include "common/utilities/ini_lines.h"
#include "snx_settings.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE* out) {
    fputs("Usage: scarletnx-vita [OPTIONS] game.vpk\n"
          "       scarletnx-vita [OPTIONS] app/<title ID>   (a dumped game's folder)\n"
          "       scarletnx-vita [OPTIONS] --game=recompiled/<title ID>\n"
          "       <title ID> [OPTIONS]   (the game's copy of the runtime, in its folder)\n"
          "  game.vpk        Extract the VPK into its game's folder (recompiled/<its title ID> of the data folder),\n"
          "                  compile its code if needed, then play. The folder holds app/ (app0:), ux0/ (the\n"
          "                  memory card), the module <title ID>_code.so and the copy of the runtime that plays it\n"
          "  --game=DIR      Play the game in its folder DIR, as its copy of the runtime does\n"
          "  --patch DIR     The dumped game's update (default: patch/<title ID> beside its app/<title ID>)\n"
          "  -x, --extract   Extract only\n"
          "  -r, --recompile Compile again, without playing\n"
          "  --data DIR      The data folder (default: a portable copy's folder, else the user's data folder)\n"
          "  --outdir DIR    The folder of the games, in which the game's folder is made (default recompiled/)\n"
          "  --output DIR    The game's folder itself\n"
          "  --seconds N     Stop after N seconds, reporting where each thread is\n"
          "  --shot=PATH     Save the last frame shown to PATH.ppm when the run ends\n"
          "  --KEY=VALUE     A Vita debug setting:\n"
          "                  --vita_hle_trace=1 logs each import the runtime lacks (2 every call),\n"
          "                  --vita_gxm_trace=1 each scene, --script=ms:buttons,... (vita_script) presses buttons\n",
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
        if (ok && snx_replace(stage, app)) {
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
static int play(const char* folder, bool recompile, double seconds, const char* shot, const char* runtime) {
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
    printf("Module %s: %u imports, module_start 0x%08x\n", image.name, (unsigned)image.import_count,
           (unsigned)image.module_start);
    bool opened = !recompile && vita_native_open(module, &image, &native, error, sizeof error);
    /* the blocks earlier runs compiled at run time (the hints) that the module lacks: the run-time compiler compiles
       them again, and a recompile (-r) starts from them, as the 3DS driver's. An existing module that matches is
       played as it is */
    size_t hint_count = 0, missing = 0;
    uint32_t* hints = opened ? vita_hints_read(folder, &hint_count) : NULL;
    for (size_t i = 0; i < hint_count; ++i) missing += !vita_module_block(native.module, hints[i]);
    free(hints);
    if (missing)
        printf("%zu blocks earlier runs compiled at run time aren't in the game's module, which a recompile (-r) "
               "starts from\n",
               missing);
    if (!opened) {
        printf("Compiling the game's code\n");
        if (vita_aot_compile(folder, &image, error, sizeof error) ||
            !vita_native_open(module, &image, &native, error, sizeof error)) {
            fprintf(stderr, "scarletnx-vita: %s\n", error);
            goto done;
        }
        /* the hints the module has, each once: one discovery refused (not code after all) doesn't ask for a compile
           again */
        hints = vita_hints_read(folder, &hint_count);
        size_t kept = 0;
        for (size_t i = 0; i < hint_count; ++i)
            if (vita_module_block(native.module, hints[i])) hints[kept++] = hints[i];
        if (kept < hint_count) printf("%zu hints aren't code: dropped\n", hint_count - kept);
        if (hint_count) vita_hints_write(folder, hints, kept);
        free(hints);
    }
    deploy_runtime(folder, runtime);
    if (recompile) {
        status = 0;
        goto done;
    }
    process = vita_process_new(&image, folder, error, sizeof error);
    if (!process) {
        fprintf(stderr, "scarletnx-vita: %s\n", error);
        goto done;
    }
    process->module = native.module;
    char sfo_path[SNX_PATH_SIZE], *title = NULL;
    size_t sfo_size = 0;
    unsigned char* sfo = snx_path_format(sfo_path, sizeof sfo_path, "%s/%s/sce_sys/param.sfo", folder,
                                         snx_is_dir(process->patch) ? "patch" : "app")
                             ? NULL
                             : snx_read_file(sfo_path, &sfo_size);
    title = sfo ? vita_sfo_string(sfo, sfo_size, "TITLE") : NULL;
    /* the window's title as the 3DS driver's and a Switch game's: the game's name, then the build */
    char* header = snx_format("%s - %s", title ? title : image.name, snx_build_info());
    printf("Game: %s\n", header ? header : image.name);
    vita_present_init(header ? header : image.name, shot);
    free(header);
    free(sfo);
    free(title);
    status = vita_run(process, seconds);
    vita_present_shot(process);
    vita_gxm_free(process); /* the renderer, before the device it draws on */
    vita_present_free();
done:
    vita_process_free(process);
    vita_native_close(&native);
    vita_image_free(&image);
    free(module);
    return status;
}
static bool parse_seconds(const char* text, double* seconds) {
    char* end = NULL;
    return text && (*seconds = strtod(text, &end)) > 0 && !*end && *seconds <= 1e9;
}
int snx_vita_main(int argc, char** argv) {
    const char *input = NULL, *output = NULL, *outdir = NULL, *repo = NULL, *game = NULL, *shot = NULL, *patch = NULL;
    bool extract_only = false, recompile = false;
    double seconds = 0;
    snx_tool_console();
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "-x") || !strcmp(a, "--extract"))
            extract_only = true;
        else if (!strcmp(a, "-r") || !strcmp(a, "--recompile") || !strcmp(a, "--recomp")) {
            if (!strcmp(a, "--recomp")) snx_option_renamed("--recomp", "--recompile");
            recompile = true;
        } else if (!strcmp(a, "--output") || !strcmp(a, "--outdir") || !strcmp(a, "--repo") || !strcmp(a, "--data") ||
                   !strcmp(a, "--patch")) {
            if (!strcmp(a, "--repo")) snx_option_renamed("--repo", "--data");
            if (++i == argc || !*argv[i]) {
                fprintf(stderr, "scarletnx-vita: %s requires a directory\n", a);
                return 2;
            }
            *(!strcmp(a, "--output")   ? &output
              : !strcmp(a, "--outdir") ? &outdir
              : !strcmp(a, "--patch")  ? &patch
                                       : &repo) = argv[i];
        } else if (!strncmp(a, "--game=", 7) && a[7])
            game = a + 7;
        else if (!strncmp(a, "--shot=", 7) && a[7])
            shot = a + 7;
        else if (!strcmp(a, "--seconds") || !strncmp(a, "--seconds=", 10)) {
            if (!parse_seconds(a[9] ? a + 10 : ++i < argc ? argv[i] : NULL, &seconds)) {
                fputs("scarletnx-vita: --seconds requires a positive number\n", stderr);
                return 2;
            }
        } else if (!strncmp(a, "--", 2) && strchr(a, '=')) {
            /* the Vita debug table's, --script= and --press_len= being vita_script and vita_press_len (a Switch
               game's are g_unattended_ini's, whose script the Switch runtime parses), and vulkan_video (which the
               Vita's AVC decoder doesn't take yet) */
            char key[64];
            size_t length = (size_t)(strchr(a, '=') - a - 2);
            const IniOpt* o = NULL;
            if (length < sizeof key) {
                memcpy(key, a + 2, length);
                key[length] = 0;
                o = ini_find(g_vita_debug_ini, !strcmp(key, "script")      ? "vita_script"
                                               : !strcmp(key, "press_len") ? "vita_press_len"
                                                                           : key);
                if (!o) o = ini_find(g_debug_ini, key);
                if (!o && !strcmp(key, "vulkan_video")) o = ini_find(g_graphics_ini, key);
            }
            if (!o) {
                fprintf(stderr, "scarletnx-vita: unknown setting: %s\n", a);
                return 2;
            }
            ini_read(o, strchr(a, '=') + 1);
        } else if (a[0] == '-' || input) {
            fprintf(stderr, "scarletnx-vita: unexpected argument: %s\n", a);
            usage(stderr);
            return 2;
        } else
            input = a;
    }
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
        char* title = sfo ? vita_sfo_string(sfo, size, "TITLE_ID") : NULL;
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
    status = play(folder, recompile, seconds, shot, runtime);
done:
    free(folder);
    free(runtime);
    return status;
}
#ifndef SNX_MULTICALL
int main(int argc, char** argv) { return snx_vita_main(argc, argv); }
#endif
