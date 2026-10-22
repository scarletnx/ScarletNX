/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Switch container extraction with hactool in this process (container.h): each NCA read where it lies in the XCI or
   NSP, the ExeFS and RomFS written straight into the output. Nothing hactool prints reaches the console but its last
   error, keys left out */
#include "nx/runtime/driver/extract.h"
#include "nx/runtime/driver/container.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *base, *update, *name, *out, *keys, *title_keys;
    int icon; /* --icon: the game's icon alone */
} ExtractOptions;

typedef struct {
    char *base, *update, *out;
    char *exefs, *romfs;
    int made_out;
    SnxDump dump;
    /* the progress row: what the extraction will have written, and when it was said last */
    SnxU64 total, written;
    double said_at;
    int said;
} ExtractState;

static void extract_help(void) {
    puts("Usage: scarletnx -x BASE.xci|BASE.nsp|BASE.dxci|BASE.dnsp [options]\n"
         "  --update FILE       Apply a same-title update during extraction, or add a DLC's RomFS image\n"
         "  --name NAME         Output name (default: base dump stem)\n"
         "  --out DIR           Output directory (default: REPO/work/NAME)\n"
         "  --keys FILE         prod.keys (default: REPO, then ~/.switch)\n"
         "  --title-keys FILE   title.keys (same discovery order; optional)\n"
         "  --icon              The game's icon alone, icon.jpg into --out DIR (what Games shows), nothing extracted\n"
         "  --help              Show this help");
    puts("REPO is the data folder: --repo, else a portable copy's or the user's (scarletnx --help).");
#ifdef SNX_XDG
    puts("The user's data folder keeps work/ in its cache folder, $XDG_CACHE_HOME/scarletnx (~/.cache/scarletnx).");
#endif
    puts("Decrypted content needs no keys. Refuses output containing exefs. A DLC's dump given alone is extracted as\n"
         "its RomFS image, system/archives/<its title ID>.romfs, where the game's aoc:u finds it.");
}

static char* text_copy(const char* text, size_t size) {
    char* copy;
    if (size == SIZE_MAX) return NULL;
    copy = (char*)malloc(size + 1);
    if (copy) {
        memcpy(copy, text, size);
        copy[size] = 0;
    }
    return copy;
}

static int option_is(const char* arg, const char* name) {
    size_t size;
    size = strlen(name);
    return !strncmp(arg, name, size) && (!arg[size] || arg[size] == '=');
}

static int parse_options(int argc, char** argv, ExtractOptions* options) {
    int i, positional;
    const char **slot, *value;
    memset(options, 0, sizeof *options);
    positional = 0;
    for (i = 0; i < argc; i++) {
        if (!positional && (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))) {
            extract_help();
            return 2;
        }
        if (!positional && !strcmp(argv[i], "--")) {
            positional = 1;
            continue;
        }
        if (!positional && !strcmp(argv[i], "--icon")) {
            options->icon = 1;
            continue;
        }
        if (positional || argv[i][0] != '-' || !argv[i][1]) {
            if (options->base) return snx_error("Extraction accepts one base dump");
            options->base = argv[i];
            continue;
        }
        slot = NULL;
        if (option_is(argv[i], "--update"))
            slot = &options->update;
        else if (option_is(argv[i], "--name"))
            slot = &options->name;
        else if (option_is(argv[i], "--out"))
            slot = &options->out;
        else if (option_is(argv[i], "--keys"))
            slot = &options->keys;
        else if (option_is(argv[i], "--title-keys"))
            slot = &options->title_keys;
        if (!slot) return snx_error("Unknown extraction option: %s", argv[i]);
        value = strchr(argv[i], '=');
        if (value)
            value++;
        else {
            if (i + 1 == argc || !strncmp(argv[i + 1], "--", 2))
                return snx_error("Extraction option requires a value: %s", argv[i]);
            value = argv[++i];
        }
        if (!value[0]) return snx_error("Extraction option has an empty value");
        *slot = value;
    }
    if (!options->base) {
        extract_help();
        return 1;
    }
    return 0;
}

static char* dump_name(const char* path) {
    const char *p, *base, *dot;
    base = path;
    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    dot = strrchr(base, '.');
    if (!dot || dot == base) dot = base + strlen(base);
    return text_copy(base, (size_t)(dot - base));
}

static int prepare_paths(ExtractState* state, const ExtractOptions* options, const SnxPaths* paths) {
    char *name, *work, *target, why[256];
    int made_out, xci;
    state->base = snx_absolute(options->base);
    if (options->update) state->update = snx_absolute(options->update);
    if (!state->base || (options->update && !state->update))
        return snx_error("Cannot resolve %s", state->base ? options->update : options->base);
    if (!snx_is_file(state->base) || (state->update && !snx_is_file(state->update)))
        return snx_error("%s is not a file", snx_is_file(state->base) ? state->update : state->base);
    if (snx_container_kind(state->base, &xci) || (state->update && snx_container_kind(state->update, &xci))) return 1;
    name = options->name ? text_copy(options->name, strlen(options->name)) : dump_name(state->base);
    if (!name) return snx_error("Out of memory resolving extraction output");
    if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..") || strpbrk(name, "/\\:")) {
        free(name);
        return snx_error("Extraction name must be a single nonempty filename");
    }
    /* only when --out names none: play_dump names one, and has said where an older build's extractions are */
    work = options->out ? NULL : snx_work_dir(paths->repo);
    target = work ? snx_path_join(work, name) : NULL;
    free(work);
    free(name);
    if (!options->out && !target) return snx_error("Out of memory resolving extraction output");
    state->out = snx_absolute(options->out ? options->out : target);
    if (!state->out) {
        int r = snx_error("Cannot resolve %s", options->out ? options->out : target);
        free(target);
        return r;
    }
    free(target);
    state->exefs = snx_path_join(state->out, "exefs");
    state->romfs = snx_path_join(state->out, "romfs");
    if (!state->exefs || !state->romfs) return snx_error("Out of memory resolving extraction output");
    if (snx_is_file(state->exefs) || snx_is_dir(state->exefs) || snx_is_link(state->exefs))
        return snx_error("Output already contains exefs: %s", state->out);
    if (snx_is_file(state->out)) return snx_error("%s is a file, not a folder", state->out);
    made_out = !snx_is_dir(state->out);
    if (snx_mkdirs(state->out)) return snx_error("Cannot create %s: %s", state->out, snx_os_error(why, sizeof why));
    state->made_out = made_out;
    return 0;
}


/* "  extracted 1229 of 4096 MB (30%)", ending in a carriage return while hactool writes: a terminal and the launcher's
   console write the next over it, the last one ends the row */
static void progress_say(ExtractState* state, int last) {
    SnxU64 done = state->written > state->total ? state->total : state->written;
    printf("  extracted %lu of %lu MB (%d%%)%s", (unsigned long)(done >> 20), (unsigned long)(state->total >> 20),
           state->total ? (int)(done * 100 / state->total) : 100, last ? "\n" : "\r");
    fflush(stdout);
    state->said = 1;
}

/* once a second at most: hactool writes in pieces of a few megabytes */
static void progress_tick(uint64_t written, void* data) {
    ExtractState* state = (ExtractState*)data;
    double now = snx_seconds();
    state->written = written;
    if (now - state->said_at < 1.0) return;
    state->said_at = now;
    progress_say(state, 0);
}

/* the files and folders under path and their bytes, added to the counts */
static void tree_count(const char* path, unsigned long* files, unsigned long* folders, SnxU64* bytes) {
    char** names = NULL;
    size_t count = 0, i;
    if (snx_list_dir(path, &names, &count)) return;
    for (i = 0; i < count; i++) {
        char* entry = snx_path_join(path, names[i]);
        if (entry && snx_is_dir(entry)) {
            ++*folders;
            tree_count(entry, files, folders, bytes);
        } else if (entry) {
            ++*files;
            *bytes += snx_file_size(entry);
        }
        free(entry);
    }
    snx_free_strings(names, count);
}
/* what the extraction wrote: the ExeFS's modules by name, the RomFS's files, folders and size */
static void say_extracted(ExtractState* state) {
    char** names = NULL;
    size_t count = 0, i;
    unsigned long files = 0, folders = 0;
    SnxU64 bytes = 0;
    char size[32];
    if (!snx_list_dir(state->exefs, &names, &count)) {
        tree_count(state->exefs, &files, &folders, &bytes);
        snx_size_text(bytes, size, sizeof size);
        printf("  ExeFS: %lu file%s, %s:", files, files == 1 ? "" : "s", size);
        for (i = 0; i < count; i++) printf("%s %s", i ? "," : "", names[i]);
        puts("");
        snx_free_strings(names, count);
    }
    files = folders = 0;
    bytes = 0;
    if (snx_is_dir(state->romfs)) {
        tree_count(state->romfs, &files, &folders, &bytes);
        snx_size_text(bytes, size, sizeof size);
        printf("  RomFS: %lu file%s in %lu folder%s, %s\n", files, files == 1 ? "" : "s", folders + 1,
               folders ? "s" : "", size);
    } else
        puts("  RomFS: none");
    fflush(stdout);
}

/* the game's control.nacp beside exefs/, where the loader reads the game's name from, out of the Control NCA's RomFS.
   A game without one, or one that cannot be read, keeps the name of its file, a line saying why */
static void control_nacp(ExtractState* state) {
    char* to = snx_path_join(state->out, "control.nacp");
    const char* why = NULL;
    size_t size = 0;
    unsigned char* nacp = to ? snx_dump_nacp(&state->dump, &size, &why) : NULL;
    char name[0x200], version[0x20], title[17];
    if (!to)
        why = "out of memory";
    else if (nacp && snx_write_file(to, nacp, size))
        why = "its control.nacp could not be written beside exefs/";
    if (why) printf("The game keeps the name of its file: %s\n", why);
    if (nacp && snx_nacp_name(nacp, size, name, sizeof name)) { /* what it is, from its own control data */
        snx_title_text(state->dump.program.title_id, title);
        printf("  The game: %s", name);
        if (snx_nacp_version(nacp, size, version, sizeof version)) printf(", version %s", version);
        printf(" (%s%s)\n", title, state->dump.control.found && state->dump.control.container == state->dump.update
                                        ? ", its name and version from the update"
                                        : "");
    }
    fflush(stdout);
    free(nacp);
    free(to);
}

/* A DLC's RomFS image as aoc:u and OpenDataStorageByDataId find it, <out>/system/archives/<its title ID>.romfs:
   written beside it, then moved over it */
/* the game's icon beside control.nacp, icon.jpg, which Games shows on its row: the Control NCA's, a JPEG. A game
   without one shows none there, a line saying why */
static void control_icon(ExtractState* state) {
    char* to = snx_path_join(state->out, "icon.jpg");
    const char* why = NULL;
    size_t size = 0;
    unsigned char* icon = to ? snx_dump_icon(&state->dump, &size, &why) : NULL;
    if (!to)
        why = "out of memory";
    else if (icon && snx_write_file(to, icon, size))
        why = "its icon.jpg could not be written beside exefs/";
    if (why) printf("Games shows no icon for the game: %s\n", why);
    fflush(stdout);
    free(icon);
    free(to);
}
static int extract_aoc(ExtractState* state) {
    const SnxNcaRef* aoc = &state->dump.aoc;
    char text[17], *folder = snx_path_join(state->out, "system/archives"), *name, *path = NULL, *tmp = NULL;
    unsigned char* chunk = (unsigned char*)malloc(1 << 22);
    SnxHacFs* fs = NULL;
    SnxU64 size, at;
    size_t n;
    FILE* file = NULL;
    int failed = !folder || !chunk;
    snx_title_text(aoc->title_id, text);
    name = snx_format("%s.romfs", text);
    if (!failed) {
        path = snx_path_join(folder, name);
        tmp = snx_format("%s.tmp", path);
    }
    printf("Extracting the DLC %s\n", text);
    fflush(stdout);
    if (!failed && snx_nca_keys(aoc, "The DLC's Data NCA")) failed = 1;
    if (failed || snx_mkdirs(folder) || !(fs = snx_dump_aoc_fs(&state->dump))) {
        failed = 1;
        goto done;
    }
    size = snx_hac_fs_romfs_size(fs);
    if (!size) {
        failed = snx_error("The DLC's Data NCA has no RomFS");
        goto done;
    }
    state->total = size;
    state->said_at = snx_seconds();
    file = snx_fopen(tmp, "wb");
    failed = !file;
    for (at = 0; !failed && at < size; at += n) {
        n = size - at < (SnxU64)(1 << 22) ? (size_t)(size - at) : (size_t)1 << 22;
        failed = snx_hac_fs_read_romfs(fs, at, chunk, n) || fwrite(chunk, 1, n, file) != n;
        if (!failed) progress_tick(at + n, state);
    }
    if (file && fclose(file)) failed = 1;
    if (state->said) progress_say(state, 1);
    if (failed || snx_replace(tmp, path)) {
        failed = snx_error("Cannot write the DLC's RomFS image %s", path);
        if (snx_is_file(tmp)) snx_remove_tree(tmp);
    } else {
        char said[32];
        snx_size_text(size, said, sizeof said);
        printf("  its RomFS image, %s, as system/archives/%s\n", said, name);
    }
done:
    snx_hac_fs_free(fs);
    free(chunk);
    free(folder);
    free(name);
    free(path);
    free(tmp);
    return failed;
}

static int extract_files(ExtractState* state, const ExtractOptions* options, const SnxPaths* paths) {
    const SnxNcaRef *program = &state->dump.program, *patch = &state->dump.patch;
    char* main_path;
    SnxU64 title, game;
    int failed;
    /* a game, an add-on over it, or a dump of a DLC alone */
    if (state->update ? snx_dump_open(&state->dump, state->base, state->update, options->keys, options->title_keys,
                                      paths->repo, 1)
                      : snx_dump_title(&state->dump, state->base, options->keys, options->title_keys, paths->repo, 1,
                                       &title, &game, NULL))
        return 1;
    if (!program->found && patch->found)
        return snx_error("%s is an update, which goes over its game: give the game's dump, with this one as --update",
                         state->base);
    if (!program->found) return extract_aoc(state);
    if (state->dump.aoc.found && extract_aoc(state)) return 1;
    /* the keys each NCA picked needs, said before anything is written */
    if (snx_nca_keys(program, "The game's Program NCA") ||
        (patch->found && snx_nca_keys(patch, "The update's Program NCA")))
        return 1;
    /* the base's program NCA: its RomFS is most of it, and an update's RomFS is about as big */
    state->total = program->size;
    state->written = 0;
    state->said = 0;
    state->said_at = snx_seconds(); /* a quick one says nothing */
    snx_hac_progress(progress_tick, state);
    puts(patch->found ? "Extracting the base with the update applied" : "Extracting the base game");
    fflush(stdout);
    { /* what is used, under the step's line */
        char size[32], other[32], title[17];
        snx_size_text(program->size, size, sizeof size);
        printf("  using the game's Program NCA (%s)", size);
        if (patch->found) {
            snx_size_text(patch->size, other, sizeof other);
            snx_title_text(patch->title_id, title);
            printf(", with %s %s (%s) over it", patch->container == state->dump.base ? "the update bundled with it"
                                                                                     : "the update",
                   title, other);
        }
        puts(state->dump.control.found ? "" : ", and no Control NCA");
    }
    failed = patch->found ? snx_hac_extract(state->dump.hac, patch->container, patch->entry, program->container,
                                            program->entry, state->exefs, state->romfs)
                          : snx_hac_extract(state->dump.hac, program->container, program->entry, -1, 0, state->exefs,
                                            state->romfs);
    snx_hac_progress(NULL, NULL);
    if (state->said) progress_say(state, 1);
    if (failed) return snx_hac_error("The Program NCA cannot be extracted");
    main_path = snx_path_join(state->exefs, "main");
    if (!main_path) return snx_error("Out of memory checking the extraction");
    failed = !snx_is_file(main_path);
    free(main_path);
    if (failed) return snx_hac_error("Extracted exefs has no main; verify that the update matches the base game");
    say_extracted(state);
    control_nacp(state);
    control_icon(state);
    return 0;
}

static int cleanup(ExtractState* state, int result) {
    if (result && state->made_out && state->out && snx_remove_tree(state->out))
        snx_error("Could not remove incomplete extraction output: %s", state->out);
    if (!result && state->out) printf("Done: %s\n", state->out);
    snx_dump_close(&state->dump);
    free(state->base);
    free(state->update);
    free(state->out);
    free(state->exefs);
    free(state->romfs);
    memset(state, 0, sizeof *state);
    return result;
}

/* hactool exits on a dump it cannot read: what it said, and the incomplete output removed first */
static void hactool_exit(void* data) {
    ExtractState* state = (ExtractState*)data;
    if (state->said) puts("");
    snx_hac_error("hactool stopped on the dump");
    cleanup(state, 1);
}

int snx_extract(int argc, char** argv, const SnxPaths* paths) {
    ExtractOptions options;
    ExtractState state;
    int result;
    result = parse_options(argc, argv, &options);
    if (result) return result == 2 ? 0 : 1;
    if (!paths || !paths->repo) return snx_error("Extraction requires the repository path");
    if (options.icon) { /* nothing else read or written */
        if (!options.out) return snx_error("--icon writes icon.jpg into --out DIR, and none is given");
        if (snx_mkdirs(options.out)) return snx_error("cannot create %s for the icon", options.out);
        return snx_extract_icon(options.base, paths, options.out) ? 1 : 0;
    }
    memset(&state, 0, sizeof state);
    result = prepare_paths(&state, &options, paths);
    if (!result) {
        snx_hac_on_exit(hactool_exit, &state);
        result = extract_files(&state, &options, paths);
        snx_hac_on_exit(NULL, NULL);
    }
    return cleanup(&state, result);
}
int snx_extract_icon(const char* dump, const SnxPaths* paths, const char* out) {
    ExtractState state;
    SnxU64 title, game;
    unsigned char* icon = NULL;
    const char* why = NULL;
    char* to = snx_path_join(out, "icon.jpg");
    size_t size = 0;
    int result;
    memset(&state, 0, sizeof state);
    state.base = snx_absolute(dump);
    if (!state.base || !to)
        result = snx_error("Cannot resolve %s", dump);
    else if (!snx_is_file(state.base))
        result = snx_error("%s is not a file", state.base);
    else {
        snx_hac_on_exit(hactool_exit, &state);
        result = snx_dump_title(&state.dump, state.base, NULL, NULL, paths->repo, 0, &title, &game, NULL);
        if (!result) icon = snx_dump_icon(&state.dump, &size, &why);
        snx_hac_on_exit(NULL, NULL);
        if (!result && !icon)
            result = snx_error("Games shows no icon for %s: %s", state.base, why);
        else if (!result && snx_write_file(to, icon, size))
            result = snx_error("Games shows no icon for %s: %s cannot be written", state.base, to);
    }
    free(icon);
    free(to);
    return cleanup(&state, result);
}
int snx_extract_title(const char* dump, const SnxPaths* paths, SnxU64* title, SnxU64* game, SnxU64* version) {
    ExtractState state;
    int result;
    memset(&state, 0, sizeof state);
    state.base = snx_absolute(dump);
    if (!state.base)
        result = snx_error("Cannot resolve %s", dump);
    else if (!snx_is_file(state.base))
        result = snx_error("%s is not a file", state.base);
    else {
        snx_hac_on_exit(hactool_exit, &state);
        result = snx_dump_title(&state.dump, state.base, NULL, NULL, paths->repo, 0, title, game, version);
        snx_hac_on_exit(NULL, NULL);
    }
    return cleanup(&state, result);
}
