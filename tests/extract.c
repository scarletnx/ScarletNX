/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* scarletnx --extract (src/nx/runtime/driver/extract.c) with hactool in its process, on synthetic decrypted dumps:
   NSPs and gamecards of DNCAs (tests/hac_synth.h), read in place. Each scenario runs in a child, whose output is
   checked: what it says, that no key reaches it, and that a failure, an exit of hactool's included, leaves nothing.
   No dumps or console keys */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/utilities/common.h"
#include "nx/runtime/driver/extract.h"
#include "nx/runtime/driver/container.h"
#if defined(_WIN32) && defined(__clang__)
/* hactool's utils.h redeclares the MinGW CRT's imported fseeko64. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winconsistent-dllimport"
#endif
#include "nx/runtime/driver/hactool/hfs0.h"
#include "nx/runtime/driver/hactool/nca.h"
#include "nx/runtime/driver/hactool/pfs0.h"
#include "nx/runtime/driver/hactool/sha.h"
#include "nx/runtime/driver/hactool/xci.h"
#if defined(_WIN32) && defined(__clang__)
#pragma clang diagnostic pop
#endif

static unsigned checks;

static void check(int good, const char* expression, int line) {
    checks++;
    if (!good) {
        fprintf(stderr, "extract.c:%d: check failed: %s\n", line, expression);
        exit(90);
    }
}
#define CHECK(value) check((value) != 0, #value, __LINE__)

#include "hac_synth.h"

#define TITLE 0x0100000000AB0000ULL
#define SECRET "0123456789abcdef0123456789abcdef"

/* synthetic, not console keys, with lines extraction leaves out: a value of an odd length, and one no key has */
static const char prod_keys[] = "# synthetic\n"
                                "header_key = " SECRET SECRET "\n"
                                "key_area_key_application_00 = " SECRET "\n"
                                "not_a_key = " SECRET "\n"
                                "titlekek_00 = abc\n";
static const char user_titles[] = "A0A0A0A0A0A0A0A0A0A0A0A0A0A0A0A0 = " SECRET "\n";

static void set_env(const char* name, const char* value) {
#if defined(_WIN32)
    CHECK(!_putenv_s(name, value));
#else
    CHECK(!setenv(name, value, 1));
#endif
}

static blob_t text_blob(const char* text) {
    blob_t b = {zalloc(strlen(text)), strlen(text)};
    memcpy(b.data, text, b.size);
    return b;
}

/* the game's NCAs, as named in a container, and a ticket */
typedef struct {
    romfs_file_t files[6];
    unsigned count;
} Game;

static void game_add(Game* game, const char* name, blob_t content) {
    CHECK(game->count < 6);
    game->files[game->count].name = name;
    game->files[game->count++].content = content;
}

static blob_t npdm(void) {
    blob_t b = {zalloc(0x80), 0x80};
    memcpy(b.data, "META", 4);
    return b;
}

static blob_t nacp(void) {
    blob_t b = {zalloc(0x4000), 0x4000};
    strcpy((char*)b.data, "Synthetic Game");
    return b;
}

/* a common ticket of the version that stores its title key as is */
static blob_t ticket(void) {
    blob_t b = {zalloc(0x400), 0x400};
    put32(b.data, 0x10004);
    memset(b.data + 4 + 0x100 + 0x3c + 0x40, 0x5A, 16);
    memset(b.data + 4 + 0x100 + 0x3c + 0x160, 0xA5, 16);
    return b;
}

static blob_t program(uint64_t title, int with_main) {
    blob_t code = text_blob("synthetic main"), npdm_file = npdm(), data = text_blob("synthetic romfs data");
    romfs_file_t exefs[2] = {{"main.npdm", npdm_file}, {"main", code}};
    romfs_file_t romfs[1] = {{"data.bin", data}};
    blob_t nca = dnca_build(NCACONTENTTYPE_PROGRAM, title, exefs, with_main ? 2 : 1, romfs, 1);
    free(code.data);
    free(npdm_file.data);
    free(data.data);
    return nca;
}

/* the game's icon as a Control NCA holds it: a JPEG's first bytes and some more, under a language past the first */
static blob_t icon(void) { return text_blob("\xff\xd8\xff synthetic icon"); }

static blob_t control(uint64_t title) {
    blob_t file = nacp(), picture = icon();
    romfs_file_t romfs[2] = {{"control.nacp", file}, {"icon_Japanese.dat", picture}};
    blob_t nca = dnca_build(NCACONTENTTYPE_CONTROL, title, NULL, 0, romfs, 2);
    free(file.data);
    free(picture.data);
    return nca;
}

/* a DLC's Data NCA: a RomFS of its own */
static blob_t dlc(uint64_t title) {
    blob_t data = text_blob("synthetic dlc data");
    romfs_file_t romfs[1] = {{"dlc.bin", data}};
    blob_t nca = dnca_build(NCACONTENTTPYE_PUBLICDATA, title, NULL, 0, romfs, 1);
    free(data.data);
    return nca;
}

/* a Meta NCA: a content meta (CNMT) of type (0x81 an update, 0x82 add-on content) for title, of the application, which
   lists one content of content_type (1 Program, 2 Data), its ID content */
static blob_t meta_nca(int type, uint64_t title, uint64_t application, const unsigned char content[16],
                       int content_type) {
    blob_t cnmt = {zalloc(0x20 + 0x10 + 0x38), 0x20 + 0x10 + 0x38};
    romfs_file_t files[1] = {{type == 0x81 ? "Patch_x.cnmt" : "AddOnContent_x.cnmt", cnmt}};
    blob_t nca;
    put64(cnmt.data, title);
    cnmt.data[0xC] = (unsigned char)type;
    cnmt.data[0xE] = 0x10; /* the extended header: the application's ID first */
    cnmt.data[0x10] = 1;   /* one content */
    put64(cnmt.data + 0x20, application);
    memcpy(cnmt.data + 0x30 + 0x20, content, 16);
    cnmt.data[0x30 + 0x36] = (unsigned char)content_type;
    nca = dnca_build(NCACONTENTTYPE_META, title, files, 1, NULL, 0);
    free(cnmt.data);
    return nca;
}

/* an NCA whose header is encrypted, which the keys would have to open */
static blob_t encrypted_nca(void) {
    blob_t b = {zalloc(0xC00), 0xC00};
    memcpy(b.data + 0x200, "XXXX", 4);
    return b;
}

static void write_blob(const char* path, blob_t blob) {
    CHECK(!snx_write_file(path, blob.data, blob.size));
    free(blob.data);
}

static void write_container(const char* path, Game* game, const char* kind) {
    if (!strcmp(kind, "nsp"))
        write_blob(path, partition_build(MAGIC_PFS0, game->files, game->count, 0x203));
    else
        write_blob(path, gamecard_build(game->files, game->count, !strcmp(kind, "xci-keyarea")));
    for (unsigned i = 0; i < game->count; i++) free(game->files[i].content.data);
    game->count = 0;
}

static int file_is(const char* dir, const char* name, const void* content, size_t size) {
    char* path = snx_path_join(dir, name);
    size_t got_size = 0;
    unsigned char* got = path ? snx_read_file(path, &got_size) : NULL;
    int same = got && got_size == size && !memcmp(got, content, size);
    free(got);
    free(path);
    return same;
}

/* Extract game container off: the dump read in place, as the runtime and -r read it (container.h), mapped (here read
   into memory) for the file systems to read from: the ExeFS's files, a file of the RomFS and the control.nacp. Nothing
   is written. 0, or 1 after saying what differed */
static int in_place(const char* base, const SnxPaths* paths) {
    SnxDump dump;
    SnxHacFs* fs;
    const SnxHacEntry* files;
    unsigned char *map, *got, buffer[32];
    size_t map_size = 0, size = 0, count, i, found = 0;
    const char* why;
    blob_t meta = npdm(), want = nacp();
    CHECK(!snx_dump_open(&dump, base, NULL, NULL, NULL, paths->repo, 0));
    map = snx_read_file(base, &map_size);
    CHECK(map != NULL);
    CHECK(!snx_hac_map(dump.hac, dump.base, map, map_size));
    fs = snx_dump_fs(&dump);
    CHECK(fs != NULL);
    count = snx_hac_fs_files(fs, &files);
    CHECK(count == 2);
    for (i = 0; i < count; i++) {
        if (!strcmp(files[i].name, "main")) {
            CHECK(files[i].size == 14 && !snx_hac_fs_read(fs, i, 0, buffer, 14));
            CHECK(!memcmp(buffer, "synthetic main", 14));
            CHECK(!snx_hac_fs_read(fs, i, 10, buffer, 4) && !memcmp(buffer, "main", 4));
            CHECK(snx_hac_fs_read(fs, i, 10, buffer, 5) == -1); /* past its end */
            found++;
        } else if (!strcmp(files[i].name, "main.npdm")) {
            got = (unsigned char*)malloc((size_t)files[i].size);
            CHECK(got && files[i].size == meta.size && !snx_hac_fs_read(fs, i, 0, got, meta.size));
            CHECK(!memcmp(got, meta.data, meta.size));
            free(got);
            found++;
        }
    }
    CHECK(found == 2);
    got = snx_romfs_file(fs, "data.bin", &size);
    CHECK(got && size == 20 && !memcmp(got, "synthetic romfs data", 20));
    free(got);
    CHECK(snx_romfs_file(fs, "missing.bin", &size) == NULL);
    got = snx_dump_nacp(&dump, &size, &why);
    CHECK(got && size == want.size && !memcmp(got, want.data, want.size));
    free(got);
    {
        blob_t picture = icon();
        got = snx_dump_icon(&dump, &size, &why);
        CHECK(got && size == picture.size && !memcmp(got, picture.data, picture.size));
        free(got);
        free(picture.data);
    }
    snx_hac_fs_free(fs);
    snx_dump_close(&dump);
    { /* the icon alone, for a game made before -x kept one */
        blob_t picture = icon();
        char *dir = snx_dirname(base), *out = dir ? snx_path_join(dir, "icon alone") : NULL;
        CHECK(out && !snx_mkdirs(out) && !snx_extract_icon(base, paths, out));
        CHECK(file_is(out, "icon.jpg", picture.data, picture.size));
        CHECK(snx_extract_icon(base, paths, "/nonexistent/folder") != 0); /* said, nothing written */
        { /* -x --icon, as the launcher asks for a dump's: the icon into --out, made, and nothing else */
            char *by_cli = snx_path_join(dir, "icon by cli/deeper"), *args[5] = {"--icon", "--out", by_cli, "--", NULL};
            char* exefs = snx_path_join(by_cli, "exefs");
            args[4] = (char*)base;
            CHECK(by_cli && exefs && !snx_extract(5, args, paths) &&
                  file_is(by_cli, "icon.jpg", picture.data, picture.size));
            CHECK(!snx_path_exists(exefs));
            args[1] = "--";
            args[2] = (char*)base;
            CHECK(snx_extract(3, args + 0, paths) != 0); /* no --out: said */
            free(exefs);
            free(by_cli);
        }
        free(picture.data);
        free(out);
        free(dir);
    }
    free(map);
    free(meta.data);
    free(want.data);
    puts("Read in place");
    return 0;
}

/* one scenario in this process, as scarletnx -x runs: its result */
static int scenario(const char* scratch, const char* mode) {
    SnxPaths paths;
    char *repo, *home, *switch_dir, *keys, *base, *update, *out, *exefs, *arguments[12];
    int count = 0, result;
    Game game = {{{0}}, 0};
    const char* kind = strstr(mode, "xci") ? (strstr(mode, "keyarea") ? "xci-keyarea" : "xci") : "nsp";
    repo = snx_path_join(scratch, "repo space & ()");
    home = snx_path_join(scratch, "original home");
    switch_dir = snx_path_join(home, ".switch");
    CHECK(!snx_mkdirs(repo) && !snx_mkdirs(switch_dir));
    memset(&paths, 0, sizeof paths);
    paths.repo = repo;
    set_env("HOME", home);
    set_env("USERPROFILE", home);
    /* the keys from the data folder, from ~/.switch, or named */
    keys = snx_path_join(!strcmp(mode, "homekeys") || !strncmp(mode, "explicit", 8) ? switch_dir : repo, "prod.keys");
    if (strcmp(mode, "encrypted-no-keys") && strcmp(mode, "explicit-missing-keys"))
        CHECK(!snx_write_file(keys, prod_keys, strlen(prod_keys)));
    {
        char* titles = snx_path_join(repo, "title.keys");
        CHECK(!snx_write_file(titles, user_titles, strlen(user_titles)));
        free(titles);
    }
    base = snx_path_join(scratch, !strcmp(mode, "renamed") ? "Base Game (&).XCI" : "Base Game (&).NSP");
    update = snx_path_join(scratch, "Update (&).nsp");
    out = snx_path_join(scratch, "out put");
    exefs = snx_path_join(out, "exefs");
    if (!strcmp(mode, "unsupported"))
        CHECK(!snx_write_file(base, "not a container at all", 22));
    else {
        game_add(&game, "1b2c.cnmt.nca", text_blob("a content meta NCA, never read"));
        game_add(&game, "a0a0.tik", ticket());
        if (!strcmp(mode, "encrypted-no-keys"))
            game_add(&game, "enc.nca", encrypted_nca());
        else if (!strcmp(mode, "update-alone"))
            game_add(&game, "patch.nca", program(TITLE | 0x800, 1)); /* an update's alone */
        else if (!strcmp(mode, "dlc-alone"))
            game_add(&game, "dlc.nca", dlc(TITLE + 0x1001));
        else
            game_add(&game, "program.nca", program(TITLE, strcmp(mode, "no-main")));
        if (strcmp(mode, "no-control") && strcmp(mode, "dlc-alone")) game_add(&game, "control.nca", control(TITLE));
        write_container(base, &game, kind);
    }
    if (!strcmp(mode, "hactool-exit")) { /* a gamecard whose root partition is not what its header hashed */
        blob_t card = gamecard_build(NULL, 0, 0);
        card.data[0xF000 + 0xC] ^= 1;
        write_blob(base, card);
    }
    if (!strncmp(mode, "inplace", 7)) {
        result = in_place(base, &paths);
        CHECK(!snx_path_exists(out)); /* nothing extracted */
        free(repo);
        free(home);
        free(switch_dir);
        free(keys);
        free(base);
        free(update);
        free(out);
        free(exefs);
        return result;
    }
    arguments[count++] = base;
    if (!strcmp(mode, "unmatched-update") || (!strncmp(mode, "dlc-", 4) && strcmp(mode, "dlc-alone"))) {
        if (!strcmp(mode, "unmatched-update"))
            game_add(&game, "patch.nca", program((TITLE + 0x1000) | 0x800, 1));
        else if (!strncmp(mode, "dlc-meta", 8)) { /* numbered otherwise: its content meta names its game */
            static const unsigned char content[16] = {0xab, 0xcd, 0xef, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
            game_add(&game, "abcdef0102030405060708090a0b0c0d.nca", dlc(0x0100000077770001ULL));
            game_add(&game, "f00d.cnmt.nca",
                     meta_nca(0x82, 0x0100000077770001ULL, !strcmp(mode, "dlc-meta-other") ? TITLE + 0x2000 : TITLE,
                              content, 2));
        } else
            game_add(&game, "dlc.nca", dlc(!strcmp(mode, "dlc-other") ? TITLE + 0x3001 : TITLE + 0x1002));
        write_container(update, &game, "nsp"); /* which frees what game holds */
        arguments[count++] = "--update";
        arguments[count++] = update;
    }
    if (!strncmp(mode, "explicit", 8)) {
        arguments[count++] = "--keys";
        arguments[count++] = keys;
    }
    if (!strcmp(mode, "named")) {
        arguments[count++] = "--name";
        arguments[count++] = "Named";
        free(out);
        out = snx_path_join(repo, "work/Named");
        free(exefs);
        exefs = snx_path_join(out, "exefs");
    } else {
        arguments[count++] = "--out";
        arguments[count++] = out;
    }
    if (!strcmp(mode, "existing")) CHECK(!snx_mkdirs(exefs));
    result = snx_extract(count, arguments, &paths);
    if (!result && !strncmp(mode, "dlc-", 4)) { /* the DLC's image where aoc:u finds it, the game's files beside */
        char* archives = snx_path_join(out, "system/archives");
        unsigned char* image;
        size_t size = 0;
        char* path = snx_format("%s/%s", archives, !strcmp(mode, "dlc-alone")  ? "0100000000AB1001.romfs"
                                                   : !strcmp(mode, "dlc-meta") ? "0100000077770001.romfs"
                                                                               : "0100000000AB1002.romfs");
        image = snx_read_file(path, &size);
        CHECK(image && size >= 0x50 && image[0] == 0x50 && !image[1]); /* a RomFS header: its size first */
        CHECK(strcmp(mode, "dlc-alone") || !snx_path_exists(exefs));
        CHECK(!strcmp(mode, "dlc-alone") || file_is(exefs, "main", "synthetic main", 14));
        free(image);
        free(path);
        free(archives);
    } else if (!result) {
        char* romfs = snx_path_join(out, "romfs");
        char* control_romfs = snx_path_join(out, "control.romfs");
        blob_t want = nacp(), meta = npdm();
        CHECK(file_is(exefs, "main", "synthetic main", 14) && file_is(exefs, "main.npdm", meta.data, meta.size));
        free(meta.data);
        CHECK(file_is(romfs, "data.bin", "synthetic romfs data", 20));
        blob_t picture = icon();
        if (strcmp(mode, "no-control"))
            CHECK(file_is(out, "control.nacp", want.data, want.size) &&
                  file_is(out, "icon.jpg", picture.data, picture.size)); /* the icon Games shows beside it */
        else
            CHECK(!file_is(out, "control.nacp", want.data, want.size) &&
                  !file_is(out, "icon.jpg", picture.data, picture.size));
        free(picture.data);
        CHECK(!snx_path_exists(control_romfs)); /* where control.nacp came out, gone */
        free(want.data);
        free(romfs);
        free(control_romfs);
    } else if (!strcmp(mode, "existing"))
        CHECK(snx_is_dir(exefs)); /* the output was there before: left as it was */
    else
        CHECK(!snx_path_exists(out)); /* an incomplete extraction is removed */
    free(repo);
    free(home);
    free(switch_dir);
    free(keys);
    free(base);
    free(update);
    free(out);
    free(exefs);
    return result;
}

int main(int argc, char** argv) {
    /* name, whether it extracts, and what it must say */
    static const struct {
        const char *mode, *says;
        int fails;
    } scenarios[] = {
        {"nsp", "Left out 1 malformed key lines", 0},
        {"xci", "Done: ", 0},
        {"xci-keyarea", "Done: ", 0},
        {"renamed", "Done: ", 0}, /* an NSP named .XCI: the header decides */
        {"homekeys", "Left out 1 malformed key lines", 0},
        {"explicit", "Left out 1 malformed key lines", 0},
        {"named", "Done: ", 0},
        {"no-control", "The game keeps the name of its file: it has no Control NCA", 0},
        {"unsupported", "(&).NSP is not an NSP/DNSP (PFS0) or an XCI/DXCI", 1},
        {"encrypted-no-keys", "Encrypted content needs prod.keys", 1},
        {"explicit-missing-keys", "prod.keys is not a file", 1},
        {"unmatched-update", "Update has no Program NCA matching the selected base game", 1},
        {"update-alone", "is an update, which goes over its game", 1},
        {"dlc-alone", "Extracting the DLC 0100000000AB1001", 0},
        {"dlc-base", "Extracting the DLC 0100000000AB1002", 0},
        {"dlc-other", "The DLC is not the selected base game's", 1},
        {"dlc-meta", "add-on content (a DLC) 0100000077770001, version 0, of the game 0100000000AB0000", 0},
        {"dlc-meta-other", "belongs to the game 0100000000AB2000, not to the selected base game 0100000000AB0000", 1},
        {"no-main", "Extracted exefs has no main", 1},
        {"existing", "Output already contains exefs", 1},
        {"hactool-exit", "hactool stopped on the dump", 1},
        {"inplace-nsp", "Read in place", 0},
        {"inplace-xci", "Read in place", 0},
        {"inplace-xci-keyarea", "Read in place", 0},
    };
    const char* command[5];
    char *self, *scratch, *directory, *output;
    size_t i;
    int result;
    if (argc == 4 && !strcmp(argv[1], "--scenario")) return scenario(argv[2], argv[3]) ? 1 : 0;
    CHECK(argc == 1);
    { /* what an NCA's missing keys are said as: none for a decrypted one, its master key's generation, its title key */
        SnxNcaRef nca;
        memset(&nca, 0, sizeof nca);
        nca.info.decrypted = 1;
        CHECK(!snx_nca_keys(&nca, "A decrypted NCA"));
        nca.info.decrypted = 0;
        nca.info.generation = 16;
        CHECK(snx_nca_keys(&nca, "An NCA of master key 16"));
        nca.info.keys = 1;
        CHECK(!snx_nca_keys(&nca, "An NCA with its keys"));
        nca.info.title_key = -1;
        CHECK(snx_nca_keys(&nca, "An NCA without its title key"));
    }
    self = snx_executable(argv[0]);
    scratch = snx_temp_dir(NULL);
    CHECK(self != NULL && scratch != NULL);
    for (i = 0; i < sizeof scenarios / sizeof scenarios[0]; i++) {
        directory = snx_path_join(scratch, scenarios[i].mode);
        CHECK(!snx_mkdirs(directory));
        command[0] = self;
        command[1] = "--scenario";
        command[2] = directory;
        command[3] = scenarios[i].mode;
        command[4] = NULL;
        output = NULL;
        result = snx_run(command, NULL, NULL, &output);
        CHECK(output != NULL);
        if ((result != 0) != scenarios[i].fails || !strstr(output, scenarios[i].says))
            fprintf(stderr, "scenario %s exited %d:\n%s", scenarios[i].mode, result, output);
        CHECK((result != 0) == scenarios[i].fails);
        CHECK(strstr(output, scenarios[i].says) != NULL);
        CHECK(strstr(output, SECRET) == NULL); /* no key, whatever hactool said */
        CHECK(strstr(output, "extracted ") == NULL); /* a quick one says nothing */
        printf("PASS extraction scenario %s\n", scenarios[i].mode);
        free(output);
        free(directory);
    }
    CHECK(!snx_remove_tree(scratch));
    free(scratch);
    free(self);
    printf("PASS %u extraction driver checks; synthetic dumps, no console keys\n", checks);
    return 0;
}
