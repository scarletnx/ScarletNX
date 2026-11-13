/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/board/board.h"
#include "3ds/loader/ctr_image.h"
#include "3ds/loader/ctr_loader.h"
#include "3ds/loader/smdh.h"
#include "3ds/aot/ctr_module.h"
#include "3ds/runtime/main.h"
#include "3ds/ipc/ctr_ipc.h"
#include "3ds/input/input.h"
#include "3ds/board/clock.h"
#include "3ds/services/uds.h"
#include "common/utilities/ascii.h"
#include "common/utilities/cli.h"
#include "common/utilities/common.h"
#include "common/platform/file.h"
#include "common/utilities/game_ini.h"
#include "common/utilities/ini_lines.h"
#include "snx_settings.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SNX_SOURCE_DIR
#define SNX_SOURCE_DIR "."
#endif
#ifndef SNX_BUILD_DIR
#define SNX_BUILD_DIR NULL
#endif

/* the common options scarletnx-3ds takes (common/utilities/cli.h): all but --frames, --shot and --headless, as the
   3DS runtime has no frame count, screenshot or window-less run of its own */
static const unsigned ctr_cli =
    SNX_CLI_BIT(SNX_CLI_EXTRACT) | SNX_CLI_BIT(SNX_CLI_EXTRACT_CONTAINER) | SNX_CLI_BIT(SNX_CLI_RECOMPILE) |
    SNX_CLI_BIT(SNX_CLI_GAME) | SNX_CLI_BIT(SNX_CLI_UPDATE) | SNX_CLI_BIT(SNX_CLI_DATA) | SNX_CLI_BIT(SNX_CLI_OUTDIR) |
    SNX_CLI_BIT(SNX_CLI_OUTPUT) | SNX_CLI_BIT(SNX_CLI_KEYS) | SNX_CLI_BIT(SNX_CLI_SECONDS) |
    SNX_CLI_BIT(SNX_CLI_SCRIPT) | SNX_CLI_BIT(SNX_CLI_FULLSCREEN) | SNX_CLI_BIT(SNX_CLI_HELP);
static void usage(FILE* out) {
    fputs(
        "Usage: scarletnx-3ds [OPTIONS] game\n"
        "       scarletnx-3ds [OPTIONS] --game=recompiled/<title ID>\n"
        "       <title ID> [OPTIONS]   (the game's copy of the runtime, in its folder)\n"
        "       scarletnx-3ds [--model old|new] --board-info\n"
        "  game                   An application CIA, cartridge image or NCCH read where it lies, recompiled if\n"
        "                         needed, then played: a .cia, a .cci or .3ds cartridge image, or a .cxi, .app or\n"
        "                         .hbb NCCH. The game's folder is recompiled/<its title ID>, as a Switch game's:\n"
        "                         its module <title ID>_code.so, its copy of the runtime <title ID>, which plays it\n"
        "                         from then on, and <title ID>.ini, which names the game (source=) and the console\n"
        "                         (model=)\n",
        out);
    snx_cli_help(out, ctr_cli);
    fputs(
        "3DS options:\n"
        "  --model old|new        The console to model. Without it, the one the game was played on before; on its\n"
        "                         first run a New 3DS for a game that is New 3DS exclusive or asks for the New\n"
        "                         3DS's 178 MiB memory mode, an Old 3DS for any other\n"
        "  --board-info           Print the board of the console --model names, and stop\n"
        "  --icon                 Write the game's SMDH icon as RGBA into --out DIR (icon.rgba, what Games draws),\n"
        "                         with -x and --data as the launcher asks for it\n"
        "  --out DIR              Where --icon writes it\n"
        "  --max-blocks N         Stop after N native blocks (default: run until the game exits or its window\n"
        "                         closes)\n"
        "  --ipc-stubs L          With --ctr_auto_stub_ipc=0, still answer a service command the runtime lacks with\n"
        "                         success for the services in L: all, or a comma list of srv, fs, apt, ptm, hid,\n"
        "                         gsp, cfg, dsp, system. Given more than once, the lists add up\n"
        "  --KEY=VALUE            A setting of the 3DS display or debug table (g_ctr_display_ini, g_ctr_debug_ini\n"
        "                         in src/nx/runtime/settings.txt): --ctr_separate_windows=1 puts each screen in a\n"
        "                         window of its own, --ctr_top_resolution=2 and --ctr_bottom_resolution=2 render\n"
        "                         them at twice the console's, --ctr_ipc_trace=1 logs each service request,\n"
        "                         --ctr_jit_trace=1 each block compiled while the game runs, --ctr_auto_stub_ipc=0\n"
        "                         stops at a service command the runtime lacks. And the Switch's --vk_memory=N, as\n"
        "                         it means there: 0 has the GPU work on a copy of the RAM in its own memory unless\n"
        "                         it shares the system's, 1 on the copy, 2 on the RAM in place. --async_pipelines=1\n"
        "                         skips what a new pipeline draws while it is made, on --pipeline_threads=N threads\n"
        "                         (0: all the computer's but two, two to twelve). --vk_pipeline_cache=0 neither\n"
        "                         reads nor writes the pipelines kept between runs (shader_cache of the game's\n"
        "                         folder). --gamepad_swap_ab=1 swaps a\n"
        "                         gamepad's A and B, --gamepad_deadzone=N is its sticks' dead zone in percent of\n"
        "                         their travel (default 20), --gamepad_map=MAP the buttons its controls send\n"
        "                         (south:B east:A ..., the Switch's names: Plus is Start, Minus Select). --touch=N\n"
        "                         is what reaches the touch screen: 2 a finger or the mouse's left button (the\n"
        "                         default), 1 a finger, 0 nothing. --vsync=0 shows the newest image at the\n"
        "                         display's next refresh, or at once, instead of each in turn at its refresh (the\n"
        "                         default 1). --fullscreen=1 starts the window fullscreen, but for --seconds and\n"
        "                         --max-blocks runs (F11 or Alt+Enter switches it while the game runs).\n"
        "                         --anisotropy=N (2 to 16) filters a mipmapped texture the game filters linearly\n"
        "                         anisotropically (default 0: as the game asks). --fps_limit=N runs the game at\n"
        "                         N/60 of the console's pace (the 3DS shows 60 frames a second), 0 as fast as it\n"
        "                         goes, game the console's (the default), and F4 turns it off and on.\n"
        "                         --aot_threads=N compiles the game's code on N threads (1 to 1024; the default 0:\n"
        "                         the computer's cores with a quarter left free, or --aot_threads_most=N), as the\n"
        "                         Switch's\n"
        "\n"
        "On the 3DS, --update takes an update's or a DLC's CIA (a bare NCCH for an update), given again for each.\n"
        "It is recorded beside the game in its folder and checked in its .ini's addons=, as a Switch game's are,\n"
        "then the game is played with it. An update or a DLC given alone, as game, is recorded beside its game,\n"
        "made already, the same way. --keys takes the 3DS AES keys in Azahar's aes_keys.txt format, to decrypt an\n"
        "encrypted game with (default ~/.3ds/aes_keys.txt). The user's data folder keeps work/ in its cache folder.\n"
        "--script=ms:buttons,... is the setting ctr_script.\n"
        "\n"
        "Without --extract-container nothing of the game is extracted: its ExeFS and RomFS are read from the game\n"
        "itself, mapped read-only. An extraction and a compiled module already there are used as they are: the\n"
        "ARM11 code is compiled again only when its module is missing or no longer matches the build or the code.\n"
        "An encrypted game is decrypted once, with the keys, into a copy beside the title's folder\n"
        "(<folder>.decrypted.cia, <folder>.decrypted.cci), and read from there. A cartridge image whose game\n"
        "partition carries the game's update is played as that update. The game plays with the update and DLCs its\n"
        "addons= checks, each read where its record names it (<title ID>_<its entry>.ini, source=). One whose file\n"
        "is not there is said and left out. A game played from an extraction plays with none.\n",
        out);
}
/* the extensions a game of this system may have. The loader reads the file's own header, so this is a guard against a
   mistyped path and the GUI's filter, not the identification: .app and .hbb are NCCHs a manual and the Homebrew
   Launcher keep, .cci and .3ds cartridge images */
static const char* const game_extensions[] = {".cia", ".cci", ".3ds", ".cxi", ".app", ".hbb",
                                              ".bcci", ".bcxi", ".bcia", ".zcci", ".zcxi", ".zcia"};
static bool game_path(const char* path) {
    size_t n = strlen(path);
    for (size_t i = 0; i < sizeof game_extensions / sizeof *game_extensions; ++i) {
        const char* e = game_extensions[i];
        size_t length = strlen(e);
        if (n > length && path[n - length] == '.') {
            bool found = true;
            for (size_t k = 1; found && k < length; ++k)
                if (snx_to_lower(path[n - length + k]) != e[k]) found = false;
            if (found) return true;
        }
    }
    return false;
}
static bool visible_path(const char* path) {
    for (const unsigned char* p = (const unsigned char*)path; *p; ++p)
        if (*p < 32 || *p == 127) return false;
    return true;
}
/* work/3ds of the data folder the Switch driver works in (snx_data_dir), which the user's keeps in its cache folder
   (snx_work_dir); resolved, as the loader refuses a link on the way to its output */
static char* work_folder(const char* repo, const char* argv0) {
    char *exe = repo ? NULL : snx_executable(argv0), *dir = exe ? snx_dirname(exe) : NULL;
    char* data = repo ? snx_strdup(repo) : snx_data_dir(dir);
    char* checkout = repo ? NULL : snx_checkout(SNX_SOURCE_DIR, SNX_BUILD_DIR, dir);
    char *work = data ? snx_work_dir(data) : snx_strdup("work"), *folder = snx_path_join(work, "3ds");
    char* resolved = snx_absolute(folder);
    if (!resolved) {
        char why[256];
        fprintf(stderr, "scarletnx-3ds: cannot resolve %s: %s\n", folder, snx_os_error(why, sizeof why));
    }
    if (checkout && data) snx_checkout_left(checkout, data);
    free(checkout);
    free(folder);
    free(work);
    free(data);
    free(dir);
    free(exe);
    return resolved;
}
/* ~/.3ds/aes_keys.txt (%USERPROFILE%\.3ds on Windows), where the 3DS keys are kept as the Switch's are in ~/.switch, in
   Azahar's format; NULL with no home */
static char* default_keys(void) {
#ifdef _WIN32
    const char* home = getenv("USERPROFILE");
    if (!home || !home[0]) home = getenv("HOME");
#else
    const char* home = getenv("HOME");
#endif
    char* folder = home && home[0] ? snx_path_join(home, ".3ds") : NULL;
    char* keys = folder ? snx_path_join(folder, "aes_keys.txt") : NULL;
    free(folder);
    return keys;
}
/* whether the file is a z3ds one, by its magic: what Azahar's launcher keeps a game in */
static bool is_z3ds(const char* path) {
    unsigned char magic[4];
    FILE* f = path ? fopen(path, "rb") : NULL;
    bool z = f && fread(magic, 1, sizeof magic, f) == sizeof magic && !memcmp(magic, "Z3DS", 4);
    if (f) fclose(f);
    return z;
}
/* The game the driver reads: a z3ds game unpacked once into <folder>.unpacked.<its container's extension>, then an
   encrypted game decrypted once into <folder>.decrypted.<its extension>, both beside the title's folder, published
   only complete, and read from then on. *path is the caller's */
static int readable_game(const char* input, const char* folder, const char* keys, char** path) {
    char *unpacked = NULL, *suffix = snx_format(".decrypted.%s", strrchr(input, '.') ? strrchr(input, '.') + 1 : "game");
    char* decrypted = suffix ? snx_format("%s%s", folder, suffix) : NULL;
    char *parent = snx_dirname(folder), *found = NULL;
    char error[512], why[256];
    int status = 1;
    *path = NULL;
    if (!decrypted || !parent) goto done;
    if (is_z3ds(input)) {
        const char* ext = strrchr(input, '.');
        unpacked = snx_format("%s.unpacked.%s", folder, ext && ext[1] == 'z' ? ext + 2 : "game");
        if (!unpacked) goto done;
        if (snx_is_file(unpacked)) {
            printf("Using the unpacked game %s\n", unpacked);
        } else {
            SnxCtrExtractResult made = snx_ctr_unzstd(input, unpacked, error, sizeof error);
            if (made != SNX_CTR_EXTRACT_OK) {
                fprintf(stderr, "scarletnx-3ds: %s\n", error);
                status = made == SNX_CTR_EXTRACT_UNSUPPORTED ? 3 : 1;
                goto done;
            }
            printf("Unpacked %s into %s\n", input, unpacked);
        }
        input = unpacked;
        free(suffix);
        suffix = snx_format(".decrypted.%s", strrchr(input, '.') ? strrchr(input, '.') + 1 : "game");
        free(decrypted);
        decrypted = suffix ? snx_format("%s%s", folder, suffix) : NULL;
        if (!decrypted) goto done;
    }
    if (snx_is_file(decrypted)) {
        printf("Using the decrypted copy %s\n", decrypted);
        *path = decrypted, decrypted = NULL;
        status = 0;
        goto done;
    }
    if (snx_mkdirs(parent)) {
        fprintf(stderr, "scarletnx-3ds: cannot create %s: %s\n", parent, snx_os_error(why, sizeof why));
        goto done;
    }
    if (!keys) keys = found = default_keys();
    bool encrypted = false;
    SnxCtrExtractResult result = snx_ctr_decrypt(input, keys, decrypted, &encrypted, error, sizeof error);
    if (result != SNX_CTR_EXTRACT_OK) {
        fprintf(stderr, "scarletnx-3ds: %s\n", error);
        status = result == SNX_CTR_EXTRACT_UNSUPPORTED ? 3 : 1;
        goto done;
    }
    if (encrypted) printf("Decrypted %s into %s\n", input, decrypted);
    *path = encrypted ? decrypted : snx_strdup(input);
    if (encrypted) decrypted = NULL;
    status = *path ? 0 : 1;
done:
    free(found);
    free(parent);
    free(decrypted);
    free(unpacked);
    free(suffix);
    return status;
}
/* what an extraction writes into its folder, beside which the title's state (native/, sdmc/, frames/, audio.wav) is
   kept */
static const char* const extraction_parts[] = {
    "exefs",   "romfs",        "contents", "cia-header.bin", "certificates.bin", "ticket.bin",
    "tmd.bin", "metadata.bin", "ncch.bin", "exheader.bin",   "manifest.ini"};
static bool holds_extraction(const char* folder) {
    char* exheader = snx_path_join(folder, "exheader.bin");
    bool found = snx_is_file(exheader);
    free(exheader);
    return found;
}
/* The game extracted into folder: published there, or, when the folder is there already with the title's state but
   no extraction (the game was read where it lies before), extracted beside it and moved into it */
static SnxCtrExtractResult extract_into(const char* input, const char* folder, char** published, char* error,
                                        size_t error_size) {
    /* anything but a folder of its own (nothing, a link, a file) is the extractor's to take or refuse */
    if (snx_is_link(folder) || !snx_is_dir(folder))
        return snx_ctr_extract(input, folder, NULL, published, error, error_size);
    char* staging = snx_format("%s.extracting", folder);
    char *done = NULL, **names = NULL;
    size_t count = 0;
    SnxCtrExtractResult result = SNX_CTR_EXTRACT_IO;
    char why[256];
    if (snx_path_exists(staging) && snx_remove_tree(staging)) {
        snprintf(error, error_size, "cannot remove %s: %s", staging, snx_os_error(why, sizeof why));
        goto out;
    }
    result = snx_ctr_extract(input, staging, NULL, &done, error, error_size);
    if (result != SNX_CTR_EXTRACT_OK) goto out;
    result = SNX_CTR_EXTRACT_IO;
    if (snx_list_dir(done, &names, &count)) {
        snprintf(error, error_size, "cannot list %s: %s", done, snx_os_error(why, sizeof why));
        goto out;
    }
    for (size_t i = 0; i < count; ++i) {
        char *from = snx_path_join(done, names[i]), *to = snx_path_join(folder, names[i]);
        /* only a stopped extraction's part can be in the way: the state has other names */
        bool moved = !(snx_path_exists(to) && snx_remove_tree(to)) && !plat_replace_file(from, to);
        if (!moved) snprintf(error, error_size, "cannot move %s to %s: %s", from, to, snx_os_error(why, sizeof why));
        free(from);
        free(to);
        if (!moved) goto out;
    }
    *published = snx_absolute(folder);
    if (*published)
        result = SNX_CTR_EXTRACT_OK;
    else
        snprintf(error, error_size, "cannot resolve %s: %s", folder, snx_os_error(why, sizeof why));
out:
    if (done && snx_path_exists(done)) snx_remove_tree(done);
    snx_free_strings(names, count);
    free(done);
    free(staging);
    return result;
}
/* The default: nothing extracted. The game is mapped and checked as the extraction checks it, its ExeFS and RomFS
   read where they lie, and folder keeps the title's state alone: an extraction an earlier --extract-container left
   there is taken away, as the game has all of it */
static int play_game(const SnxCtrSource* game, const char* folder, SnxCtrModel model, uint64_t max_blocks, double seconds,
                    unsigned flags, uint32_t ipc_stubs, const char* runtime, char* error, size_t error_size) {
    char *state = NULL, why[256];
    int status = 1;
    if (snx_is_link(folder))
        snprintf(why, sizeof why, "it is a link");
    else if (snx_mkdirs(folder) || !(state = snx_absolute(folder)))
        snx_os_error(why, sizeof why);
    if (!state) {
        fprintf(stderr, "scarletnx-3ds: cannot create the title's folder %s: %s\n", folder, why);
        goto done;
    }
    printf("The title's folder is %s\n", state);
    if (holds_extraction(state)) {
        printf("Removing the extraction in %s, the game reads its own files\n", state);
        for (size_t i = 0; i < sizeof extraction_parts / sizeof *extraction_parts; ++i) {
            char* part = snx_path_join(state, extraction_parts[i]);
            bool failed = snx_path_exists(part) && snx_remove_tree(part);
            if (failed) fprintf(stderr, "scarletnx-3ds: cannot remove %s: %s\n", part, snx_os_error(why, sizeof why));
            free(part);
            if (failed) goto done;
        }
    }
    status = snx_ctr_aot_run(state, game, model, max_blocks, seconds, flags, ipc_stubs, runtime, error, error_size);
    if (status) fprintf(stderr, "scarletnx-3ds: %s\n", error);
done:
    free(state);
    return status;
}
/* --icon: the game's SMDH icon as RGBA into out, icon.rgba, which Games draws a dump's row by, as the Switch driver's
   -x --icon writes its icon.jpg. An encrypted game is decrypted first, into work/3ds, as a run of it is */
static int write_icon(const char* input, const char* out, const char* keys, const char* repo, const char* argv0) {
    char error[512];
    char *work = work_folder(repo, argv0), *leaf = input ? snx_dump_name(input) : NULL,
         *kept = work && leaf ? snx_path_join(work, leaf) : NULL, *readable = NULL;
    SnxCtrSource* source = NULL;
    unsigned char rgba[48 * 48 * 4];
    uint64_t size = 0;
    const unsigned char* smdh;
    int status = 1;
    if (!input || !out || !kept) {
        fprintf(stderr, "scarletnx-3ds: --icon takes the game's file and --out DIR\n");
        goto done;
    }
    {
        int read = readable_game(input, kept, keys, &readable);
        if (read) {
            status = read; /* 3: a format this build does not take, as a run of it says */
            goto done;
        }
    }
    if (snx_ctr_source_open(readable, &source, error, sizeof error) != SNX_CTR_EXTRACT_OK) {
        fprintf(stderr, "scarletnx-3ds: %s\n", error);
        goto done;
    }
    smdh = snx_ctr_source_exefs(source, "icon", &size);
    if (!smdh || snx_ctr_smdh_icon(smdh, (size_t)size, rgba)) {
        fprintf(stderr, "scarletnx-3ds: %s has no SMDH icon to write\n", input);
        goto done;
    }
    {
        char* file = snx_path_join(out, "icon.rgba");
        if (!file || snx_mkdirs(out) || snx_write_file(file, rgba, sizeof rgba)) {
            char why[256];
            snx_os_error(why, sizeof why);
            fprintf(stderr, "scarletnx-3ds: cannot write %s: %s\n", file ? file : "the icon", why);
            free(file);
            goto done;
        }
        free(file);
    }
    status = 0;
done:
    snx_ctr_source_close(source);
    free(readable);
    free(kept);
    free(leaf);
    free(work);
    return status;
}
/* The game played, extracted or compiled as the options say, in its folder extracted (freed): an extraction there
   already, else the game at input, which source is opened (NULL when it is not) */
static int play(const char* input, SnxCtrSource* source, char* extracted, SnxCtrModel model, uint64_t max_blocks,
                double seconds, bool recompile, bool extract_only, bool extract_container, uint32_t ipc_stubs,
                const char* runtime) {
    char error[1024]; /* the run's last line: a GX command's stop carries why the GPU did not start */
    unsigned flags = recompile ? SNX_CTR_RECOMPILE | SNX_CTR_COMPILE_ONLY : 0;
    if (source && !extract_container && !extract_only) { /* the game read where it lies, the folder keeping its state */
        int status =
            play_game(source, extracted, model, max_blocks, seconds, flags, ipc_stubs, runtime, error, sizeof error);
        snx_ctr_source_close(source);
        free(extracted);
        return status;
    }
    snx_ctr_source_close(source);
    /* like the Switch driver: a finished extraction is used again, the extractor publishes only complete ones */
    if (holds_extraction(extracted))
        printf("Using the extraction in %s\n", extracted);
    else {
        printf("Extracting %s\n", input);
        char* published = NULL;
        SnxCtrExtractResult result = extract_into(input, extracted, &published, error, sizeof error);
        free(extracted);
        if (result != SNX_CTR_EXTRACT_OK) {
            fprintf(stderr, "scarletnx-3ds: %s\n", error);
            return result == SNX_CTR_EXTRACT_UNSUPPORTED ? 3 : 1;
        }
        extracted = published;
        printf("Extracted %s\n", extracted);
    }
    if (extract_only) {
        free(extracted);
        return 0;
    }
    int status =
        snx_ctr_aot_run(extracted, NULL, model, max_blocks, seconds, flags, ipc_stubs, runtime, error, sizeof error);
    free(extracted);
    if (status) fprintf(stderr, "scarletnx-3ds: %s\n", error);
    return status;
}
/* The last part of path, a separator at its end dropped, and ".exe" with it on Windows; the caller's to free */
static char* last_part(const char* path) {
    size_t n = strlen(path), cut;
    while (n > 1 && (path[n - 1] == '/' || path[n - 1] == '\\')) --n;
    for (cut = n; cut && path[cut - 1] != '/' && path[cut - 1] != '\\'; --cut) continue;
    char* name = snx_format("%.*s", (int)(n - cut), path + cut);
    size_t length = name ? strlen(name) : 0;
    if (length > 4 && name[length - 4] == '.' && snx_to_lower(name[length - 3]) == 'e' &&
        snx_to_lower(name[length - 2]) == 'x' && snx_to_lower(name[length - 1]) == 'e')
        name[length - 4] = 0;
    return name;
}
bool snx_ctr_title_name(const char* path) {
    char* name = last_part(path);
    bool title = name && strlen(name) == 16 && !strncmp(name, "0004", 4);
    for (size_t i = 0; title && i < 16; ++i) title = snx_is_hex(name[i]);
    free(name);
    return title;
}
/* The game's folder, named title (its title ID): in outdir, else the data folder's recompiled/ as the Switch driver
   makes its games (snx_recompiled_game, which an older version's games/ still serves). Resolved, NULL with a line saying
   why */
/* What the game's .ini (<folder>/<title ID>.ini) says: the game it plays (source=), *source the caller's, NULL
   without one, and the console (model=), *model kept without one, *named (when given) whether it names one. False
   only when it cannot be read */
static bool read_game_ini_model(const char* folder, char** source, SnxCtrModel* model, bool* named) {
    char *path = snx_ctr_game_file(folder, ".ini"), *key, *value;
    size_t size;
    char* text = path ? (char*)snx_read_file(path, &size) : NULL;
    bool read = text || (path && !snx_path_exists(path));
    *source = NULL;
    SnxIni ini;
    if (text) snx_ini_start(&ini, text);
    while (text && snx_ini_next(&ini, &key, &value))
        if (!strcmp(key, "source") && *value) {
            free(*source);
            *source = snx_strdup(value);
        } else if (!strcmp(key, "model") && (!strcmp(value, "old") || !strcmp(value, "new"))) {
            *model = !strcmp(value, "old") ? SNX_CTR_OLD : SNX_CTR_NEW;
            if (named) *named = true;
        }
    free(text);
    free(path);
    return read;
}
static bool read_game_ini(const char* folder, char** source, SnxCtrModel* model) {
    return read_game_ini_model(folder, source, model, NULL);
}
/* The console a game played for the first time runs on, when --model names none: a New 3DS for a title its icon data
   marks New 3DS exclusive, which an Old 3DS refuses, or whose ExHeader asks for the 178 MiB memory mode, which only a
   New 3DS has; else an Old 3DS. *why says what asked for a New 3DS, NULL for an Old 3DS */
static SnxCtrModel model_of(const SnxCtrSource* source, const char** why) {
    uint64_t size = 0;
    const unsigned char* icon = snx_ctr_source_exefs(source, "icon", &size);
    *why = NULL;
    if (icon && snx_ctr_smdh_new3ds_only(icon, (size_t)size))
        *why = "its icon data marks it a New 3DS exclusive";
    else if (snx_ctr_exheader_new3ds_memory(snx_ctr_source_exheader(source)))
        *why = "its ExHeader asks for the New 3DS's 178 MiB memory mode";
    return *why ? SNX_CTR_NEW : SNX_CTR_OLD;
}
/* whether a line of the game's .ini sets one of the keys this file writes, which the new values replace */
static bool game_ini_own_line(const char* line, size_t n) {
    static const char* const keys[] = {"source", "model", "system"};
    size_t i;
    while (n && (*line == ' ' || *line == '\t')) line++, n--;
    for (i = 0; i < sizeof keys / sizeof keys[0]; ++i) {
        size_t k = strlen(keys[i]), at;
        if (n <= k || strncmp(line, keys[i], k)) continue;
        for (at = k; at < n && (line[at] == ' ' || line[at] == '\t'); ++at) continue;
        if (at < n && line[at] == '=') return true;
    }
    return false;
}
/* The keys this file writes, as they are now. The game's .ini's other lines stay, so a setting a player added by hand is
   not lost when a game is played again */
static bool write_game_ini(const char* folder, const char* source, SnxCtrModel model) {
    char *path = snx_ctr_game_file(folder, ".ini"), *full = snx_absolute(source);
    size_t size = 0, n = 0, tail_n = 0;
    char* old = path && full ? (char*)snx_read_file(path, &size) : NULL;
    char* tail =
        full ? snx_format("source=%s\nmodel=%s\nsystem=3ds\n", full, model == SNX_CTR_OLD ? "old" : "new") : NULL;
    bool read = full && (!path || old || !snx_path_exists(path));
    char* text = read && tail ? (char*)snx_alloc(size + strlen(tail) + 2) : NULL;
    if (tail) tail_n = strlen(tail);
    if (text) {
        const char* at = old;
        for (; at && at < old + size;) {
            const char* end = (const char*)memchr(at, '\n', (size_t)(old + size - at));
            size_t length = (size_t)((end ? end + 1 : old + size) - at);
            if (!game_ini_own_line(at, length)) {
                memcpy(text + n, at, length);
                n += length;
            }
            at += length;
        }
        if (n && text[n - 1] != '\n') text[n++] = '\n';
        memcpy(text + n, tail, tail_n + 1);
    }
    bool written = path && text && !snx_write_file(path, text, n + tail_n);
    if (!written) {
        char why[256];
        fprintf(stderr, "scarletnx-3ds: cannot write %s: %s\n", path ? path : "the game's .ini",
                snx_os_error(why, sizeof why));
    }
    free(tail);
    free(text);
    free(old);
    free(full);
    free(path);
    return written;
}
/* An older version kept the game's state (saves, SD card) in work/3ds/<the game's name>: moved into the game's new
   folder the first time, which is made then */
static void move_old_folder(const char* old, const char* folder) {
    if (!old || snx_is_link(old) || !snx_is_dir(old) || snx_path_exists(folder)) return;
    char *parent = snx_dirname(folder), why[256];
    if (parent && !snx_mkdirs(parent) && !plat_replace_file(old, folder))
        printf("Moved the game's saves and SD card from %s to %s\n", old, folder);
    else
        fprintf(stderr, "scarletnx-3ds: the game's saves stay in %s, it cannot move to %s: %s\n", old, folder,
                snx_os_error(why, sizeof why));
    free(parent);
}
/* ---- A game's update and DLCs, as a Switch game's are (common/utilities/game_ini.h): each recorded beside the game in
   its folder, <title ID>_<its entry>.ini naming its file (source=) and its data folder <title ID>_<its entry>_data,
   which the launcher lists, and checked in the game's addons=, one update at most ---- */
#define MAX_UPDATES (1 + SNX_CTR_MAX_DLC) /* --update given at most: an update and the DLCs a game takes */
static const char* addon_word(SnxU64 game, SnxU64 title) {
    return snx_addon_kind(game, title) == SNX_ADDON_UPDATE ? "update" : "DLC";
}
/* the game an update or a DLC of title is of: its title ID's low half (a DLC's 28 bits) under 0x00040000; 0 for a title
   that is neither */
static uint64_t addon_game(uint64_t title) {
    uint64_t game = 0x0004000000000000ull | (title & ((title >> 32) == 0x0004008cu ? 0x0FFFFFFFull : 0xFFFFFFFFull));
    return snx_addon_kind(game, title) == SNX_ADDON_NONE ? 0 : game;
}
/* The add-on title (version -1 for none known) of the game named name in folder, its file dump as the player gave it,
   recorded and checked. 0, or 1 with a line saying why */
static int record_addon(const char* folder, const char* name, uint64_t title, int32_t version, const char* dump) {
    SnxU64 game = 0;
    char entry[32], why[256];
    (void)snx_title_parse(name, &game);
    snx_addon_entry(game, title, version < 0 ? SNX_NO_VERSION : (SnxU64)version, entry);
    char *own = snx_format("%s_%s", name, entry), *full = snx_absolute(dump);
    char* data = own ? snx_format("%s_data", own) : NULL;
    char* where = data ? snx_path_join(folder, data) : NULL;
    int rc = 1;
    if (!full || !where)
        fprintf(stderr, "scarletnx-3ds: cannot resolve %s: %s\n", dump, snx_os_error(why, sizeof why));
    else if (snx_mkdirs(where))
        fprintf(stderr, "scarletnx-3ds: cannot create %s: %s\n", where, snx_os_error(why, sizeof why));
    else if (!snx_record_source(folder, own, full) && !snx_addon_check(folder, name, entry)) { /* each said why */
        printf("Recorded the %s %s beside its game in %s, checked in its addons=\n", addon_word(game, title), entry,
               folder);
        rc = 0;
    }
    free(where);
    free(data);
    free(full);
    free(own);
    return rc;
}
/* The update and DLCs the addons= of the game in folder checks, each read where its record names it, decrypted first
   as a game is: the update laid over source, each DLC beside it. One whose record or file is not there is said, with
   the path, and left out. 0, or the run's exit status with a line saying why */
static int apply_addons(const char* folder, SnxCtrSource* source, const char* keys, const char* repo,
                        const char* argv0) {
    char *name = last_part(folder), *ini = name ? snx_game_ini(folder, name) : NULL;
    char* text = ini ? snx_ini_value(ini, "addons") : NULL;
    SnxAddon* list = (SnxAddon*)snx_alloc(SNX_ADDONS_MAX * sizeof *list);
    size_t count = name ? snx_addons_parse(text, name, list, SNX_ADDONS_MAX) : 0;
    SnxU64 game = 0;
    int status = 0;
    char error[512];
    if (name) (void)snx_title_parse(name, &game);
    for (size_t i = 0; i < count && !status; ++i) {
        const char* what = addon_word(game, list[i].id);
        char *own = snx_format("%s_%s", name, list[i].entry), *readable = NULL, *where = NULL;
        int recorded = 0;
        char* dump = own ? snx_game_source(folder, own, &recorded) : NULL;
        if (!dump) { /* its path did not fit, said */
            status = 1;
        } else if (!recorded) {
            fprintf(stderr,
                    "scarletnx-3ds: the %s %s checked in addons= has no record (%s.ini names no file): this "
                    "run plays without it\n",
                    what, list[i].entry, own ? own : "its");
        } else if (!snx_is_file(dump)) {
            fprintf(stderr, "scarletnx-3ds: the %s %s checked in addons= is not at %s: this run plays without it\n",
                    what, list[i].entry, dump);
        } else {
            char *work = work_folder(repo, argv0), *leaf = snx_dump_name(dump);
            if (work && leaf) where = snx_path_join(work, leaf);
            free(leaf);
            free(work);
            status = where ? readable_game(dump, where, keys, &readable) : 1; /* said why */
            SnxCtrExtractResult result = SNX_CTR_EXTRACT_OK;
            if (!status)
                result = snx_addon_kind(game, list[i].id) == SNX_ADDON_UPDATE
                             ? snx_ctr_source_set_update(source, readable, error, sizeof error)
                             : snx_ctr_source_add_dlc(source, readable, error, sizeof error);
            if (result != SNX_CTR_EXTRACT_OK) {
                fprintf(stderr, "scarletnx-3ds: the %s %s checked in addons=: %s\n", what, list[i].entry, error);
                status = result == SNX_CTR_EXTRACT_UNSUPPORTED ? 3 : 1;
            } else if (!status)
                printf("Playing with the %s %s, read from %s\n", what, list[i].entry, readable);
        }
        free(where);
        free(readable);
        free(dump);
        free(own);
    }
    free(list);
    free(text);
    free(ini);
    free(name);
    return status;
}
/* An extraction's game plays with no update or DLC: the ones the game's addons= checks said, as this run leaves them
   out */
static void addons_unplayed(const char* folder) {
    char *name = last_part(folder), *ini = name ? snx_game_ini(folder, name) : NULL;
    char* text = ini ? snx_ini_value(ini, "addons") : NULL;
    SnxAddon* list = (SnxAddon*)snx_alloc(SNX_ADDONS_MAX * sizeof *list);
    size_t count = name ? snx_addons_parse(text, name, list, SNX_ADDONS_MAX) : 0;
    for (size_t i = 0; i < count; ++i)
        fprintf(stderr,
                "scarletnx-3ds: the game plays from its extraction, which reads no update or DLC: this run "
                "plays without %s, checked in addons= (play it without --extract-container to play with it)\n",
                list[i].entry);
    free(list);
    free(text);
    free(ini);
    free(name);
}
/* N of --max-blocks N or --max-blocks=N, false for anything but a positive number */
static bool parse_blocks(const char* text, uint64_t* blocks) {
    char* end = NULL;
    if (!text || text[0] < '0' || text[0] > '9') return false;
    errno = 0;
    unsigned long long n = strtoull(text, &end, 10);
    if (errno || *end || !n) return false;
    *blocks = (uint64_t)n;
    return true;
}
/* The folder of the game this program is the copy of, as a Switch game's copy is: its own folder when it is named
   after a title ID, or after the module beside it. NULL for the driver under its own names. The caller's to free */
static char* own_game(const char* argv0) {
    char *name = last_part(argv0), *exe = name && strncmp(name, "scarletnx", 9) ? snx_executable(argv0) : NULL;
    char *dir = exe ? snx_dirname(exe) : NULL, module[SNX_PATH_SIZE];
    if (dir && !snx_ctr_title_name(name) &&
        (snx_path_format(module, sizeof module, "%s/%s%s", dir, name, SNX_CTR_MODULE_SUFFIX) || !snx_is_file(module))) {
        free(dir);
        dir = NULL;
    }
    free(exe);
    free(name);
    return dir;
}
/* The game in its folder (owned) played as its copy of the runtime plays it: the game its .ini names, an extraction
   the folder holds, else the game itself read where it lies, with the update and DLCs its addons= checks. runtime is
   owned too */
static int play_folder(char* folder, SnxCtrModel model, bool model_set, const char* repo, const char* argv0,
                       const char* keys, uint64_t max_blocks, double seconds, bool recompile, bool extract_only,
                       bool extract_container, uint32_t ipc_stubs, char* runtime) {
    SnxCtrSource* source = NULL;
    char error[512];
    int status;
    /* the game its .ini names: an extraction its folder holds, else the game itself */
    char* named = NULL;
    SnxCtrModel recorded = model;
    if (!read_game_ini(folder, &named, &recorded)) {
        fprintf(stderr, "scarletnx-3ds: cannot read the game's .ini in %s\n", folder);
        free(runtime);
        free(folder);
        return 1;
    }
    if (!model_set) model = recorded;
    if (holds_extraction(folder))
        extract_container = true;
    else if (!named) {
        fprintf(stderr,
                "scarletnx-3ds: %s holds no game: its .ini names no game (source=). Play the game again "
                "(scarletnx-3ds game.cia) to make its folder\n",
                folder);
        free(runtime);
        free(folder);
        return 2;
    } else if (!extract_container && !extract_only) {
        /* the game it names may be an encrypted one: decrypt it as the run that made the folder did, or reuse that
           run's copy, so the folder plays without the keys being asked for again */
        char *work = work_folder(repo, argv0), *leaf = snx_dump_name(named), *where = NULL, *readable = NULL;
        if (work && leaf) where = snx_path_join(work, leaf);
        free(leaf);
        free(work);
        int decrypted = where ? readable_game(named, where, keys, &readable) : 1;
        free(where);
        if (decrypted != 1) {
            free(named);
            if (decrypted) {
                free(readable);
                free(runtime);
                free(folder);
                return decrypted;
            }
            named = readable;
        }
        SnxCtrExtractResult opened = snx_ctr_source_open(named, &source, error, sizeof error);
        if (opened != SNX_CTR_EXTRACT_OK) {
            fprintf(stderr, "scarletnx-3ds: %s\n", error);
            free(named);
            free(runtime);
            free(folder);
            return opened == SNX_CTR_EXTRACT_UNSUPPORTED ? 3 : 1;
        }
        int added = apply_addons(folder, source, keys, repo, argv0); /* said why */
        if (added) {
            snx_ctr_source_close(source);
            free(named);
            free(runtime);
            free(folder);
            return added;
        }
    }
    if (extract_container) addons_unplayed(folder);
    status = play(named, source, folder, model, max_blocks, seconds, recompile, extract_only, extract_container,
                  ipc_stubs, runtime);
    free(named);
    free(runtime);
    return status;
}
int snx_ctr_main(int argc, char** argv) {
    SnxCtrModel model = SNX_CTR_OLD;
    const char *input = NULL, *icon_out = NULL;
    bool info = false, positional = false, budget_set = false, model_set = false, icon = false;
    uint64_t max_blocks = 0; /* no budget */
    uint32_t ipc_stubs = 0;  /* with ctr_auto_stub_ipc off, every unknown service command stops the run */
    SnxCtrBoard board;
    snx_tool_console();
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* SNX_3DS_UDS_PACKET_TRACE, which an older version read (docs/user/deprecations.md): "1" is still ctr_uds_trace=1 for
       this version, under the command line's --ctr_uds_trace. Removed with tests/architecture.py's ENV_DEPRECATED
       row */
    const char* old_trace = getenv("SNX_3DS_UDS_PACKET_TRACE");
    if (old_trace && !strcmp(old_trace, "1")) g_settings.ctr_uds_trace = 1;
    SnxCli cli;
    snx_cli_init(&cli, "scarletnx-3ds", ctr_cli);
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        char error[256];
        /* the common options first (common/utilities/cli.h): --script and --fullscreen are the settings ctr_script and
           fullscreen */
        int common = positional ? 0 : snx_cli_take(&cli, argc, argv, &i, error, sizeof error);
        if (common < 0 || common == SNX_CLI_HELP) {
            if (common < 0)
                fprintf(stderr, "%s\n", error);
            else
                usage(stdout);
            snx_cli_free(&cli);
            return common < 0 ? 2 : 0;
        }
        if (common == SNX_CLI_SCRIPT)
            ini_read(ini_find(g_ctr_debug_ini, "ctr_script"), cli.value[SNX_CLI_SCRIPT]);
        else if (common == SNX_CLI_FULLSCREEN)
            ini_read(ini_find(g_display_ini, "fullscreen"), cli.fullscreen ? "1" : "0");
        if (common) continue;
        if (!positional && !strcmp(arg, "--"))
            positional = true;
        else if (!positional && !strcmp(arg, "--board-info"))
            info = true;
        else if (!positional && !strcmp(arg, "--icon"))
            icon = true;
        else if (!positional && (!strcmp(arg, "--out") || !strncmp(arg, "--out=", 6))) {
            icon_out = arg[5] ? arg + 6 : ++i < argc ? argv[i] : NULL;
            if (!icon_out || !icon_out[0] || icon_out[0] == '-') {
                fputs("scarletnx-3ds: --out requires a folder\n", stderr);
                snx_cli_free(&cli);
                return 2;
            }
        } else if (!positional && (!strcmp(arg, "--max-blocks") || !strncmp(arg, "--max-blocks=", 13))) {
            if (!parse_blocks(arg[12] ? arg + 13 : ++i < argc ? argv[i] : NULL, &max_blocks)) {
                fputs("scarletnx-3ds: --max-blocks requires a positive number\n", stderr);
                snx_cli_free(&cli);
                return 2;
            }
            budget_set = true;
        } else if (!positional && !strcmp(arg, "--ipc-stubs")) {
            if (++i == argc || !snx_ctr_ipc_stub_mask(argv[i], &ipc_stubs)) {
                fputs("scarletnx-3ds: --ipc-stubs requires all or a comma list of srv, fs, apt, ptm, hid, gsp, cfg, "
                      "dsp, system, am\n",
                      stderr);
                snx_cli_free(&cli);
                return 2;
            }
        } else if (!positional && !strcmp(arg, "--model")) {
            if (++i == argc || (strcmp(argv[i], "old") && strcmp(argv[i], "new"))) {
                fputs("scarletnx-3ds: --model requires old or new\n", stderr);
                snx_cli_free(&cli);
                return 2;
            }
            model = !strcmp(argv[i], "old") ? SNX_CTR_OLD : SNX_CTR_NEW;
            model_set = true;
        } else if (!positional && !strncmp(arg, "--", 2) && strchr(arg, '=')) {
            /* a setting of the 3DS display or debug table (g_ctr_display_ini, g_ctr_debug_ini, in
               src/nx/runtime/settings.txt), or one of the Switch's the 3DS takes too (snx_ctr_shared_setting),
               --key=value. gamepad_map is the input's to read: its row's setter keeps the map for a Switch game's
               pads */
            char key[64];
            const char* value = ini_arg(arg, key, sizeof key);
            const IniOpt* o = NULL;
            bool map = false;
            /* the recompile's threads, the Switch's setting: a value it does not take stops here, as there */
            if (value && (!strcmp(key, "aot_threads") || !strcmp(key, "aot_threads_most"))) {
                o = ini_find(g_recompile_ini, key);
                if (ini_check(o, value, error, sizeof error)) {
                    fprintf(stderr, "scarletnx-3ds: %s\n", error);
                    snx_cli_free(&cli);
                    return 2;
                }
            } else if (value) {
                o = ini_find(g_ctr_display_ini, key);
                if (!o) o = ini_find(g_ctr_debug_ini, key);
                if (!o && snx_ctr_shared_setting(key)) o = ini_find(g_debug_ini, key);
                if (!o && snx_ctr_shared_setting(key)) o = ini_find(g_performance_ini, key);
                if (!o && snx_ctr_shared_setting(key)) o = ini_find(g_gamepad_ini, key);
                if (!o && snx_ctr_shared_setting(key)) o = ini_find(g_backend_ini, key);
                if (!o && snx_ctr_shared_setting(key)) o = ini_find(g_graphics_ini, key);
                if (!o && snx_ctr_shared_setting(key)) o = ini_find(g_display_ini, key);
                map = !strcmp(key, "gamepad_map");
            }
            if (map)
                snx_ctr_input_map(value);
            else if (!o) {
                fprintf(stderr, "scarletnx-3ds: unknown setting: %s\n", arg);
                snx_cli_free(&cli);
                return 2;
            } else
                ini_read(o, value);
        } else if ((!positional && arg[0] == '-') || input) {
            fprintf(stderr, "scarletnx-3ds: unexpected argument: %s\n", arg);
            usage(stderr);
            snx_cli_free(&cli);
            return 2;
        } else
            input = arg;
    }
    /* the common options' values, --update's checked for room */
    const char *output = cli.value[SNX_CLI_OUTPUT], *outdir = cli.value[SNX_CLI_OUTDIR];
    const char *repo = cli.value[SNX_CLI_DATA], *keys = cli.value[SNX_CLI_KEYS], *game = cli.value[SNX_CLI_GAME];
    bool extract_only = cli.flags & SNX_CLI_BIT(SNX_CLI_EXTRACT);
    bool recompile = cli.flags & SNX_CLI_BIT(SNX_CLI_RECOMPILE);
    bool extract_container = cli.flags & SNX_CLI_BIT(SNX_CLI_EXTRACT_CONTAINER);
    double seconds = cli.seconds; /* 0: no time limit */
    const char* updates[MAX_UPDATES]; /* --update's files, an update's or a DLC's each */
    unsigned update_count = 0;
    if (seconds) budget_set = true;
    if (cli.update_count > MAX_UPDATES) {
        fprintf(stderr, "scarletnx-3ds: --update is given %d times at most\n", MAX_UPDATES);
        snx_cli_free(&cli);
        return 2;
    }
    for (int u = 0; u < cli.update_count; ++u) updates[update_count++] = cli.updates[u];
    if (icon_out && !icon) {
        fputs("scarletnx-3ds: --out needs --icon\n", stderr);
        usage(stderr);
        snx_cli_free(&cli);
        return 2;
    }
    if (icon) {
        int status = 2;
        const char* refused = NULL;
        if (input && icon_out && !info && !game && !recompile && !extract_container && !update_count && !output &&
            !outdir && !budget_set && !ipc_stubs) {
            if (!visible_path(input) || !visible_path(icon_out))
                fputs("scarletnx-3ds: input/output paths may not contain control characters\n", stderr);
            else if ((refused = !game_path(input)         ? "is not a game of this system"
                                : !snx_path_exists(input) ? "does not exist"
                                : !snx_is_file(input)     ? "is not a file"
                                                          : NULL))
                fprintf(stderr, "scarletnx-3ds: %s %s\n", input, refused);
            else
                status = write_icon(input, icon_out, keys, repo, argv[0]);
        }
        if (status == 2) usage(stderr);
        snx_cli_free(&cli);
        return status;
    }
    snx_cli_free(&cli);
    /* the Switch's fps_limit, its text as the Switch reads it, the game's clock's pace (board/clock.h) */
    char said[192];
    int pace = snx_ctr_clock_pace();
    (void)snx_ctr_clock_parse(g_settings.fps_limit, &pace, said, sizeof said);
    if (said[0]) fprintf(stderr, "settings: %s\n", said);
    snx_ctr_clock_set_pace(pace);
    if (old_trace)
        snx_error("SNX_3DS_UDS_PACKET_TRACE=%s is deprecated, %s: ctr_uds_trace=%d (docs/user/deprecations.md)", old_trace,
                  strcmp(old_trace, "1")     ? "and only 1 did anything"
                  : g_settings.ctr_uds_trace ? "and this run reads it as --ctr_uds_trace=1"
                                             : "and the command line gives ctr_uds_trace first",
                  g_settings.ctr_uds_trace);
    /* the game's own copy of the runtime plays it, or --game= names it */
    char* folder = game ? snx_strdup(game) : input || info ? NULL : own_game(argv[0]);
    /* --ipc-stubs only changes a run: with an option that does not run the game it is a mistake to report */
    if ((!input && !info && !folder) || (folder && (input || output || outdir || keys || update_count)) ||
        (update_count && (info || output || extract_only || extract_container)) ||
        (info && (input || extract_only || extract_container || recompile || output || outdir || budget_set)) ||
        (extract_only && (budget_set || recompile)) || (recompile && budget_set) ||
        (ipc_stubs && (info || extract_only || recompile))) {
        free(folder);
        usage(stderr);
        return 2;
    }
    if (!snx_ctr_board_init(&board, model)) {
        free(folder);
        return 1;
    }
    if (info) {
        printf("3DS board: %s\nARM11 cores: %u\nBase clock: %llu Hz\nFCRAM: %u MiB\n", board.profile->name,
               board.profile->arm11_cores, SNX_CTR_BASE_CLOCK_HZ, board.profile->fcram_size / (1024u * 1024u));
        for (size_t i = 0; i < board.profile->region_count; ++i) {
            const SnxCtrRegion* r = &board.profile->regions[i];
            printf("  %-10s physical 0x%08" PRIx32 " size 0x%08" PRIx32 "\n", r->name, r->base, r->size);
        }
        snx_ctr_board_shutdown(&board);
        return 0;
    }
    snx_ctr_board_shutdown(&board);
    char *runtime = snx_executable(argv[0]), error[512];
    char* runtime_dir = runtime ? snx_dirname(runtime) : NULL;
    char* network_data = repo ? snx_strdup(repo) : snx_data_dir(runtime_dir);
    if (network_data) snx_ctr_uds_settings(network_data);
    free(network_data);
    free(runtime_dir);
    SnxCtrSource* source = NULL;
    int status;
    if (folder)
        return play_folder(folder, model, model_set, repo, argv[0], keys, max_blocks, seconds, recompile, extract_only,
                           extract_container, ipc_stubs, runtime);
    /* Do not let a host filename inject progress/launch banners into the GUI. */
    if (!visible_path(input) || (output && !visible_path(output)) || (outdir && !visible_path(outdir))) {
        fputs("scarletnx-3ds: input/output paths may not contain control characters\n", stderr);
        free(runtime);
        return 2;
    }
    const char* refused = !game_path(input)         ? "is not a game of this system"
                          : !snx_path_exists(input) ? "does not exist"
                          : !snx_is_file(input)     ? "is not a file"
                                                    : NULL;
    if (refused) {
        fprintf(stderr, "scarletnx-3ds: %s %s\n", input, refused);
        free(runtime);
        return 2;
    }
    /* The decryption is kept beside the game's folder named by --output, else in the data folder's work/3ds, where
       an older version kept the game's folder, named after the game */
    char* old = NULL;
    if (!output) {
        char *work = work_folder(repo, argv[0]), *name = snx_dump_name(input);
        old = work && name ? snx_path_join(work, name) : NULL;
        free(work);
        free(name);
        if (!old) { /* work_folder said why */
            free(runtime);
            return 1;
        }
    }
    char* readable = NULL;
    int decrypted = readable_game(input, output ? output : old, keys, &readable);
    if (decrypted) {
        free(old);
        free(runtime);
        return decrypted;
    }
    printf("Reading %s\n", readable);
    /* an update or a DLC: recorded beside its game, made already, and the game played with what its addons= checks */
    uint64_t title = 0;
    int32_t version = -1;
    SnxCtrExtractResult opened = snx_ctr_source_identify(readable, &title, &version, error, sizeof error);
    uint64_t of = opened == SNX_CTR_EXTRACT_OK ? addon_game(title) : 0;
    if (of) {
        char name[17], *named = NULL;
        SnxCtrModel ignored = model;
        snprintf(name, sizeof name, "%016" PRIX64, of);
        free(old);
        free(readable);
        if (update_count || output || extract_only || extract_container) {
            fprintf(stderr,
                    "scarletnx-3ds: %s is the %s %016" PRIX64 " of the game %s, read where it lies beside it: "
                    "it takes no --update, --output, -x or --extract-container\n",
                    input, addon_word(of, title), title, name);
            free(runtime);
            return 2;
        }
        folder = snx_game_folder(repo, outdir, argv[0], name, 0);
        if (!folder) {
            char why[256];
            fprintf(stderr, "scarletnx-3ds: cannot resolve the folder of %s: %s\n", name,
                    snx_os_error(why, sizeof why));
        } else if (!read_game_ini(folder, &named, &ignored) || (!named && !holds_extraction(folder))) {
            fprintf(stderr,
                    "scarletnx-3ds: %s is the %s %016" PRIX64 " of the game %s, which is not made in %s yet: "
                    "play the game first (scarletnx-3ds GAME), or give both (scarletnx-3ds GAME --update %s)\n",
                    input, addon_word(of, title), title, name, folder, input);
        } else if (!record_addon(folder, name, title, version, input)) {
            free(named);
            return play_folder(folder, model, model_set, repo, argv[0], keys, max_blocks, seconds, recompile,
                               extract_only, extract_container, ipc_stubs, runtime);
        }
        free(named);
        free(folder);
        free(runtime);
        return 1;
    }
    opened = snx_ctr_source_open(readable, &source, error, sizeof error);
    if (opened != SNX_CTR_EXTRACT_OK) {
        fprintf(stderr, "scarletnx-3ds: %s\n", error);
        free(readable);
        free(old);
        free(runtime);
        return opened == SNX_CTR_EXTRACT_UNSUPPORTED ? 3 : 1;
    }
    /* the game's folder: recompiled/<its title ID>, as the Switch driver names a game's */
    if (output)
        folder = snx_strdup(output); /* as given: the extractor refuses links on the way to it */
    else {
        char title[17];
        snprintf(title, sizeof title, "%016" PRIX64, snx_ctr_source_title(source));
        folder = snx_game_folder(repo, outdir, argv[0], title, 0);
        if (!folder) {
            char why[256];
            fprintf(stderr, "scarletnx-3ds: cannot resolve the folder of %s: %s\n", title,
                    snx_os_error(why, sizeof why));
        }
        if (folder) move_old_folder(old, folder);
    }
    free(old);
    if (folder && !model_set) {
        /* the console the game was played on before, when --model names none; for its first run, the one it asks for */
        char* recorded = NULL;
        bool named = false;
        (void)read_game_ini_model(folder, &recorded, &model, &named);
        free(recorded);
        const char* why = NULL;
        if (!named) model = model_of(source, &why);
        if (why) printf("Played on a New 3DS: %s (--model old to try an Old 3DS)\n", why);
    }
    bool made = folder && !extract_only && !snx_is_link(folder) && !snx_mkdirs(folder);
    /* the folder names the game the player picked, not the decrypted copy an encrypted one leaves in the cache: a
       launcher asks whether it already made a game from that dump, and the cache's path matches none */
    if (!folder || (made && !write_game_ini(folder, input, model))) {
        snx_ctr_source_close(source);
        free(folder);
        free(readable);
        free(runtime);
        return 1;
    }
    /* --update's each recorded beside the game as its own, then the game read with what its addons= checks */
    status = 0;
    char name[17];
    snprintf(name, sizeof name, "%016" PRIX64, snx_ctr_source_title(source));
    for (unsigned u = 0; u < update_count && !status; ++u) {
        char *work = work_folder(repo, argv[0]), *leaf = snx_dump_name(updates[u]), *copy = NULL;
        char* kept = work && leaf ? snx_path_join(work, leaf) : NULL;
        uint64_t its = 0;
        int32_t its_version = -1;
        status = kept ? readable_game(updates[u], kept, keys, &copy) : 1; /* said why */
        opened = status ? SNX_CTR_EXTRACT_OK : snx_ctr_source_identify(copy, &its, &its_version, error, sizeof error);
        if (opened != SNX_CTR_EXTRACT_OK) {
            fprintf(stderr, "scarletnx-3ds: %s: %s\n", updates[u], error);
            status = opened == SNX_CTR_EXTRACT_UNSUPPORTED ? 3 : 1;
        } else if (!status && snx_addon_kind(snx_ctr_source_title(source), its) == SNX_ADDON_NONE) {
            fprintf(stderr, "scarletnx-3ds: %s is not an update or a DLC of %s (%s): its title is %016" PRIX64 "\n",
                    updates[u], input, name, its);
            status = 1;
        } else if (!status) {
            status = record_addon(folder, name, its, its_version, updates[u]);
        }
        free(kept);
        free(copy);
        free(leaf);
        free(work);
    }
    if (!status && !extract_container && !extract_only)
        status = apply_addons(folder, source, keys, repo, argv[0]);
    else if (!status && extract_container)
        addons_unplayed(folder);
    if (status) {
        snx_ctr_source_close(source);
        free(folder);
        free(readable);
        free(runtime);
        return status;
    }
    status = play(readable, source, folder, model, max_blocks, seconds, recompile, extract_only, extract_container,
                  ipc_stubs, runtime);
    free(readable);
    free(runtime);
    return status;
}
