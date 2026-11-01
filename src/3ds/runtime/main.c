/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/board/board.h"
#include "3ds/loader/ctr_loader.h"
#include "3ds/aot/ctr_module.h"
#include "3ds/runtime/main.h"
#include "3ds/ipc/ctr_ipc.h"
#include "3ds/services/uds.h"
#include "common/utilities/ascii.h"
#include "common/utilities/common.h"
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

static void usage(FILE* out) {
    fputs(
        "Usage: scarletnx-3ds [OPTIONS] game\n"
        "       scarletnx-3ds [OPTIONS] --game=recompiled/<title ID>\n"
        "       <title ID> [OPTIONS]   (the game's copy of the runtime, in its folder)\n"
        "       scarletnx-3ds [--model old|new] --board-info\n"
        "  game            An application CIA, cartridge image or NCCH read where it lies, recompiled if needed,\n"
        "                  then played: a .cia, a .cci or .3ds cartridge image, or a .cxi, .app or .hbb NCCH. The\n"
        "                  game's folder is recompiled/<its title ID>, as a Switch game's: its module\n"
        "                  <title ID>_code.so, its copy of the runtime <title ID>, which plays it from then on, and\n"
        "                  <title ID>.ini, which names the game (source=) and the console (model=)\n"
        "  --game=DIR      Play the game in its folder DIR, as its copy of the runtime does\n"
        "  --extract-container\n"
        "                  Extract the game into its folder first and play from the extraction\n"
        "  -x, --extract   Extract only\n"
        "  -r, --recompile Recompile without playing\n"
        "  --model old|new The console to model (default old)\n"
        "  --keys FILE     The 3DS AES keys, in Azahar's aes_keys.txt format, to decrypt an encrypted game\n"
        "                  with (default ~/.3ds/aes_keys.txt)\n"
        "  --repo DIR      The data folder (default: a portable copy's folder, else the user's data\n"
        "  --data DIR      The data folder (default: a portable copy's folder, else the user's data\n"
        "                  folder, as scarletnx picks it; the user's keeps work/ in its cache folder)\n"
        "  --outdir DIR    The folder of the games, in which the game's folder is made (default recompiled/ of\n"
        "                  the data folder)\n"
        "  --output DIR    The game's folder itself: its compiled module, SD card and any extraction (default\n"
        "                  <outdir>/<the title ID>)\n"
        "  --max-blocks N  Stop after N native blocks (default: run until the game exits or its window\n"
        "                  closes)\n"
        "  --seconds N     Stop after N seconds, reporting where each thread is (--seconds=N too)\n"
        "  --KEY=VALUE     A setting of the 3DS debug table (g_ctr_debug_ini in src/nx/runtime/settings.txt):\n"
        "                  --ctr_ipc_trace=1 logs each service request, --ctr_jit_trace=1 each block\n"
        "                  compiled while the game runs, --script=ms:buttons,... (ctr_script) presses\n"
        "                  buttons, --ctr_auto_stub_ipc=0 stops at a service command the runtime lacks\n"
        "  --ipc-stubs L   With --ctr_auto_stub_ipc=0, still answer a service command the runtime lacks with\n"
        "                  success for the services in L: all, or a comma list of srv, fs, apt, ptm, hid, gsp,\n"
        "                  cfg, dsp, system. Given more than once, the lists add up\n"
        "Without --extract-container nothing of the game is extracted: its ExeFS and RomFS are read from the\n"
        "game itself, mapped read-only. An extraction and a compiled module already there are used as they\n"
        "are: the ARM11 code is compiled again only when its module is missing or no longer matches the\n"
        "build or the code. An encrypted game is decrypted once, with the keys, into a copy beside the title's\n"
        "folder (<folder>.decrypted.cia, <folder>.decrypted.cci), and read from there. A cartridge image whose\n"
        "game partition carries the game's update is played as that update.\n",
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
        bool moved = !(snx_path_exists(to) && snx_remove_tree(to)) && !snx_replace(from, to);
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
/* The game played, extracted or compiled as the options say, in its folder extracted (freed): an extraction there
   already, else the game at input, which source is opened (NULL when it is not) */
static int play(const char* input, SnxCtrSource* source, char* extracted, SnxCtrModel model, uint64_t max_blocks,
                double seconds, bool recompile, bool extract_only, bool extract_container, uint32_t ipc_stubs,
                const char* runtime) {
    char error[512];
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
   without one, and the console (model=), *model kept without one. False only when it cannot be read */
static bool read_game_ini(const char* folder, char** source, SnxCtrModel* model) {
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
        } else if (!strcmp(key, "model") && (!strcmp(value, "old") || !strcmp(value, "new")))
            *model = !strcmp(value, "old") ? SNX_CTR_OLD : SNX_CTR_NEW;
    free(text);
    free(path);
    return read;
}
static bool write_game_ini(const char* folder, const char* source, SnxCtrModel model) {
    char *path = snx_ctr_game_file(folder, ".ini"), *full = snx_absolute(source);
    char* text =
        full ? snx_format("source=%s\nmodel=%s\nsystem=3ds\n", full, model == SNX_CTR_OLD ? "old" : "new") : NULL;
    bool written = path && text && !snx_write_file(path, text, strlen(text));
    if (!written) {
        char why[256];
        fprintf(stderr, "scarletnx-3ds: cannot write %s: %s\n", path ? path : "the game's .ini",
                snx_os_error(why, sizeof why));
    }
    free(text);
    free(full);
    free(path);
    return written;
}
/* An older version kept the game's state (saves, SD card) in work/3ds/<the game's name>: moved into the game's new
   folder the first time, which is made then */
static void move_old_folder(const char* old, const char* folder) {
    if (!old || snx_is_link(old) || !snx_is_dir(old) || snx_path_exists(folder)) return;
    char *parent = snx_dirname(folder), why[256];
    if (parent && !snx_mkdirs(parent) && !snx_replace(old, folder))
        printf("Moved the game's saves and SD card from %s to %s\n", old, folder);
    else
        fprintf(stderr, "scarletnx-3ds: the game's saves stay in %s, it cannot move to %s: %s\n", old, folder,
                snx_os_error(why, sizeof why));
    free(parent);
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
int snx_ctr_main(int argc, char** argv) {
    SnxCtrModel model = SNX_CTR_OLD;
    const char *input = NULL, *output = NULL, *outdir = NULL, *repo = NULL, *keys = NULL, *game = NULL;
    bool info = false, positional = false, extract_only = false, recompile = false, budget_set = false;
    bool extract_container = false, model_set = false;
    uint64_t max_blocks = 0; /* no budget */
    double seconds = 0;      /* no time limit */
    uint32_t ipc_stubs = 0;  /* with ctr_auto_stub_ipc off, every unknown service command stops the run */
    SnxCtrBoard board;
    snx_tool_console();
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* SNX_3DS_UDS_PACKET_TRACE, which an older version read (docs/DEPRECATIONS.md): "1" is still ctr_uds_trace=1 for
       this version, under the command line's --ctr_uds_trace. Removed with tests/architecture.py's ENV_DEPRECATED
       row */
    const char* old_trace = getenv("SNX_3DS_UDS_PACKET_TRACE");
    if (old_trace && !strcmp(old_trace, "1")) g_settings.ctr_uds_trace = 1;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (!positional && !strcmp(arg, "--"))
            positional = true;
        else if (!positional && (!strcmp(arg, "--help") || !strcmp(arg, "-h"))) {
            usage(stdout);
            return 0;
        } else if (!positional && !strcmp(arg, "--board-info"))
            info = true;
        else if (!positional && (!strcmp(arg, "-x") || !strcmp(arg, "--extract")))
            extract_only = true;
        else if (!positional && !strcmp(arg, "--extract-container"))
            extract_container = true;
        else if (!positional && (!strcmp(arg, "-r") || !strcmp(arg, "--recompile") || !strcmp(arg, "--recomp"))) {
            if (!strcmp(arg, "--recomp")) snx_option_renamed("--recomp", "--recompile");
            recompile = true;
        } else if (!positional && (!strcmp(arg, "--output") || !strcmp(arg, "--outdir") || !strcmp(arg, "--repo") ||
                                   !strcmp(arg, "--data"))) {
            if (!strcmp(arg, "--repo")) snx_option_renamed("--repo", "--data");
            if (++i == argc || !*argv[i]) {
                fprintf(stderr, "scarletnx-3ds: %s requires a directory\n", arg);
                return 2;
            }
            *(!strcmp(arg, "--output") ? &output : !strcmp(arg, "--outdir") ? &outdir : &repo) = argv[i];
        } else if (!positional && !strncmp(arg, "--game=", 7)) {
            if (!arg[7]) {
                fputs("scarletnx-3ds: --game= requires the game's folder\n", stderr);
                return 2;
            }
            game = arg + 7;
        } else if (!positional && !strcmp(arg, "--keys")) {
            if (++i == argc || !*argv[i]) {
                fputs("scarletnx-3ds: --keys requires a file\n", stderr);
                return 2;
            }
            keys = argv[i];
        } else if (!positional && (!strcmp(arg, "--max-blocks") || !strncmp(arg, "--max-blocks=", 13))) {
            if (!parse_blocks(arg[12] ? arg + 13 : ++i < argc ? argv[i] : NULL, &max_blocks)) {
                fputs("scarletnx-3ds: --max-blocks requires a positive number\n", stderr);
                return 2;
            }
            budget_set = true;
        } else if (!positional && (!strcmp(arg, "--seconds") || !strncmp(arg, "--seconds=", 10))) {
            if (!snx_parse_seconds(arg[9] ? arg + 10 : ++i < argc ? argv[i] : NULL, &seconds)) {
                fputs("scarletnx-3ds: --seconds requires a positive number\n", stderr);
                return 2;
            }
            budget_set = true;
        } else if (!positional && !strcmp(arg, "--ipc-stubs")) {
            if (++i == argc || !snx_ctr_ipc_stub_mask(argv[i], &ipc_stubs)) {
                fputs("scarletnx-3ds: --ipc-stubs requires all or a comma list of srv, fs, apt, ptm, hid, gsp, cfg, "
                      "dsp, system, am\n",
                      stderr);
                return 2;
            }
        } else if (!positional && !strcmp(arg, "--model")) {
            if (++i == argc || (strcmp(argv[i], "old") && strcmp(argv[i], "new"))) {
                fputs("scarletnx-3ds: --model requires old or new\n", stderr);
                return 2;
            }
            model = !strcmp(argv[i], "old") ? SNX_CTR_OLD : SNX_CTR_NEW;
            model_set = true;
        } else if (!positional && !strncmp(arg, "--", 2) && strchr(arg, '=')) {
            /* a setting of the 3DS debug table (g_ctr_debug_ini, src/nx/runtime/settings.txt), --key=value; --script=
               is ctr_script, a Switch game's name for it */
            char key[64];
            size_t length = (size_t)(strchr(arg, '=') - arg - 2);
            const IniOpt* o = NULL;
            if (length < sizeof key) {
                memcpy(key, arg + 2, length);
                key[length] = 0;
                o = ini_find(g_ctr_debug_ini, !strcmp(key, "script") ? "ctr_script" : key);
            }
            if (!o) {
                fprintf(stderr, "scarletnx-3ds: unknown setting: %s\n", arg);
                return 2;
            }
            ini_read(o, strchr(arg, '=') + 1);
        } else if ((!positional && arg[0] == '-') || input) {
            fprintf(stderr, "scarletnx-3ds: unexpected argument: %s\n", arg);
            usage(stderr);
            return 2;
        } else
            input = arg;
    }
    if (old_trace)
        snx_error("SNX_3DS_UDS_PACKET_TRACE=%s is deprecated, %s: ctr_uds_trace=%d (docs/DEPRECATIONS.md)", old_trace,
                  strcmp(old_trace, "1")     ? "and only 1 did anything"
                  : g_settings.ctr_uds_trace ? "and this run reads it as --ctr_uds_trace=1"
                                             : "and the command line gives ctr_uds_trace first",
                  g_settings.ctr_uds_trace);
    /* the game's own copy of the runtime plays it, or --game= names it */
    char* folder = game ? snx_strdup(game) : input || info ? NULL : own_game(argv[0]);
    /* --ipc-stubs only changes a run: with an option that does not run the game it is a mistake to report */
    if ((!input && !info && !folder) || (folder && (input || output || outdir || keys)) ||
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
    if (folder) {
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
            SnxCtrExtractResult opened = snx_ctr_source_open(named, &source, error, sizeof error);
            if (opened != SNX_CTR_EXTRACT_OK) {
                fprintf(stderr, "scarletnx-3ds: %s\n", error);
                free(runtime);
                free(folder);
                return opened == SNX_CTR_EXTRACT_UNSUPPORTED ? 3 : 1;
            }
        }
        status = play(named, source, folder, model, max_blocks, seconds, recompile, extract_only, extract_container,
                      ipc_stubs, runtime);
        free(named);
        free(runtime);
        return status;
    }
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
    SnxCtrExtractResult opened = snx_ctr_source_open(readable, &source, error, sizeof error);
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
    if (folder && !model_set) { /* the console the game was played on before, when --model names none */
        char* recorded = NULL;
        (void)read_game_ini(folder, &recorded, &model);
        free(recorded);
    }
    bool made = folder && !extract_only && !snx_is_link(folder) && !snx_mkdirs(folder);
    if (!folder || (made && !write_game_ini(folder, readable, model))) {
        snx_ctr_source_close(source);
        free(folder);
        free(readable);
        free(runtime);
        return 1;
    }
    status = play(readable, source, folder, model, max_blocks, seconds, recompile, extract_only, extract_container,
                  ipc_stubs, runtime);
    free(readable);
    free(runtime);
    return status;
}
