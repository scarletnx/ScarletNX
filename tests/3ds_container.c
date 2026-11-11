/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A game read where it lies against its extraction (tests/3ds_container.py): snx_ctr_source_open accepts what the
   extraction accepts, and its ExeFS files, its image identity and every RomFS file read through the guest's view of
   the mapped CIA's level 3 are the extraction's bytes. Usage: probe CIA [EXTRACTED], or probe GAME --update UPDATE
   STATE, which prints what the game with the update laid over it reads, the RomFS SelfNCCH serves included (STATE:
   an empty folder for the process's tree). Exit 0, or 20 + the open's SnxCtrExtractResult when it refused */
#include "3ds/loader/ctr_loader.h"
#include "3ds/loader/ctr_image.h"
#include "3ds/runtime/internal.h"
#include "3ds/ipc/ctr_ipc.h"
#include "3ds/services/romfs.h"
#include "common/filesystem/tree.h"
#include "common/utilities/common.h"
#include "support/3ds_guest.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NONE UINT32_MAX
static uint32_t le32(const unsigned char* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const unsigned char* p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static unsigned char* view_bytes(SnxCtrRomfs* view, uint64_t* size) {
    *size = snx_ctr_romfs_size(view);
    unsigned char* image = malloc(*size ? (size_t)*size : 1);
    assert(image && snx_ctr_romfs_read(view, 0, image, (size_t)*size));
    return image;
}
/* UTF-16LE names here are ASCII or BMP/surrogate units; the host file is found by the UTF-8 the extraction wrote */
static void utf8(const unsigned char* name16, uint32_t bytes, char* out) {
    size_t n = 0;
    for (uint32_t i = 0; i < bytes; i += 2) {
        uint32_t cp = name16[i] | name16[i + 1] << 8;
        if (cp >= 0xd800 && cp < 0xdc00) {
            uint32_t low = name16[i + 2] | name16[i + 3] << 8;
            cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
            i += 2;
        }
        if (cp < 0x80)
            out[n++] = (char)cp;
        else if (cp < 0x800) {
            out[n++] = (char)(0xc0 | cp >> 6);
            out[n++] = (char)(0x80 | (cp & 63));
        } else if (cp < 0x10000) {
            out[n++] = (char)(0xe0 | cp >> 12);
            out[n++] = (char)(0x80 | (cp >> 6 & 63));
            out[n++] = (char)(0x80 | (cp & 63));
        } else {
            out[n++] = (char)(0xf0 | cp >> 18);
            out[n++] = (char)(0x80 | (cp >> 12 & 63));
            out[n++] = (char)(0x80 | (cp >> 6 & 63));
            out[n++] = (char)(0x80 | (cp & 63));
        }
    }
    out[n] = 0;
}
/* every file under the level-3 folder at dir checked against host/<its path>: the count of files */
static unsigned walk(const unsigned char* image, uint64_t size, uint32_t dir, const char* host) {
    uint32_t dirs = le32(image + 12), files = le32(image + 28), data = le32(image + 36);
    unsigned count = 0;
    char name[1024];
    for (uint32_t file = le32(image + dirs + dir + 12); file != NONE; file = le32(image + files + file + 4)) {
        const unsigned char* f = image + files + file;
        utf8(f + 32, le32(f + 28), name);
        char* path = snx_path_join(host, name);
        size_t length = 0;
        unsigned char* expected = snx_read_file(path, &length);
        uint64_t at = le64(f + 8), bytes = le64(f + 16);
        assert(expected && length == bytes && data + at + bytes <= size);
        assert(!memcmp(image + data + at, expected, length));
        free(expected);
        free(path);
        ++count;
    }
    for (uint32_t child = le32(image + dirs + dir + 8); child != NONE; child = le32(image + dirs + child + 4)) {
        utf8(image + dirs + child + 24, le32(image + dirs + child + 20), name);
        char* path = snx_path_join(host, name);
        assert(snx_is_dir(path));
        count += walk(image, size, child, path);
        free(path);
    }
    return count;
}
static unsigned host_files(const char* path) {
    if (!snx_is_dir(path)) return 1;
    char** names;
    size_t n;
    unsigned count = 0;
    assert(!snx_list_dir(path, &names, &n));
    for (size_t i = 0; i < n; ++i) {
        char* child = snx_path_join(path, names[i]);
        count += host_files(child);
        free(child);
    }
    snx_free_strings(names, n);
    return count;
}
static SnxCtrTimeResult stamp(SnxCtrRomfs* view, const char* ascii, uint64_t* datetime) {
    unsigned char path16[256] = {0};
    size_t n = strlen(ascii);
    for (size_t i = 0; i < n; ++i) path16[i * 2] = (unsigned char)ascii[i];
    return snx_ctr_romfs_time(view, path16, n * 2 + 2, datetime);
}
static void compare(const SnxCtrSource* game, const char* extracted) {
    char error[256];
    SnxCtrImage from_cia, from_files;
    assert(snx_ctr_image_from_source(game, &from_cia, error, sizeof error));
    assert(snx_ctr_image_load(extracted, &from_files, error, sizeof error));
    assert(!memcmp(from_cia.identity, from_files.identity, 32) && from_cia.code_size == from_files.code_size);
    assert(!memcmp(from_cia.code, from_files.code, from_cia.code_size));
    snx_ctr_image_free(&from_cia);
    snx_ctr_image_free(&from_files);
    /* the ExeFS: each file as stored, .code as expanded */
    char *exefs = snx_path_join(extracted, "exefs"), **names;
    size_t n, length;
    assert(!snx_list_dir(exefs, &names, &n));
    for (size_t i = 0; i < n; ++i) {
        char* path = snx_path_join(exefs, names[i]);
        unsigned char* expected = snx_read_file(path, &length);
        uint64_t size;
        size_t code_size;
        const unsigned char* got = !strcmp(names[i], ".code") ? snx_ctr_source_code(game, &code_size) : NULL;
        if (got)
            size = code_size;
        else
            got = snx_ctr_source_exefs(game, names[i], &size);
        assert(expected && got && size == length && !memcmp(got, expected, length));
        free(expected);
        free(path);
    }
    uint64_t none;
    assert(!snx_ctr_source_exefs(game, "missing", &none) && !none);
    snx_free_strings(names, n);
    free(exefs);
    /* the RomFS: the game's level 3 as the guest reads it, and the extraction's files */
    uint64_t level3_size, size;
    const unsigned char* level3 = snx_ctr_source_romfs(game, &level3_size);
    SnxCtrRomfs *direct, *indexed = NULL;
    assert(snx_ctr_romfs_open_image(level3, level3_size, snx_ctr_source_mtime(game), &direct) && direct);
    FsTree tree;
    assert(fs_tree_init(&tree, extracted) == FS_TREE_OK && snx_ctr_romfs_open(&tree, &indexed) && indexed);
    char* romfs = snx_path_join(extracted, "romfs");
    unsigned char* image = view_bytes(direct, &size);
    assert(size == (level3 ? level3_size : snx_ctr_romfs_size(indexed)));
    unsigned seen = walk(image, size, 0, romfs);
    if (!level3) { /* no RomFS: the empty image an empty romfs/ folder serializes, byte for byte */
        uint64_t other;
        unsigned char* serialized = view_bytes(indexed, &other);
        assert(other == size && !memcmp(serialized, image, size));
        free(serialized);
    }
    assert(seen == host_files(romfs)); /* and none the walk missed */
    uint64_t datetime = 0;
    assert(stamp(direct, "/", &datetime) == SNX_CTR_TIME_OK && datetime);
    if (seen) assert(stamp(direct, "/hello.txt", &datetime) == SNX_CTR_TIME_OK);
    if (seen) assert(stamp(direct, "/dir", &datetime) == SNX_CTR_TIME_OK);
    assert(stamp(direct, "/missing.txt", &datetime) == SNX_CTR_TIME_NOTFOUND);
    assert(stamp(direct, "/hello.txt/x", &datetime) == SNX_CTR_TIME_NOTFOUND);
    printf("%u RomFS files and the ExeFS read from the game match its extraction\n", seen);
    free(image);
    free(romfs);
    snx_ctr_romfs_free(direct);
    snx_ctr_romfs_free(indexed);
    fs_tree_destroy(&tree);
    pm_destroy(&tree.lock);
}
/* what the game with an update reads: its ExHeader name, the ExHeader's title, and which files its RomFS and its
   update's RomFS hold: the one the update names (UTF-16LE), the one the base names */
static bool has_name(const unsigned char* image, uint64_t size, const char* name) {
    size_t n = strlen(name);
    for (uint64_t at = 0; at + n * 2 <= size; ++at) {
        size_t i = 0;
        while (i < n && image[at + i * 2] == (unsigned char)name[i] && !image[at + i * 2 + 1]) ++i;
        if (i == n) return true;
    }
    return false;
}
static const char* holder(const unsigned char* image, uint64_t size) {
    bool update = image && has_name(image, size, "patched.txt"), base = image && has_name(image, size, "hello.txt");
    return update ? (base ? "both" : "update") : base ? "base" : "none";
}
/* the RomFS file SelfNCCH's path type (0 or 5) opens, as an updated game opens it at boot (FSUSER
   OpenFileDirectly), read whole through a link to it (FSFILE OpenLinkFile), which reads as the file does: which files
   it holds, or the open's result */
static const char* self_romfs(SnxCtrProcess* p, uint32_t fs, uint32_t type, char* text, size_t text_size) {
    const unsigned char path[12] = {(unsigned char)type};
    assert(snx_ctr_write_ram(p->space, 0x300040, path, sizeof path) == SNX_CTR_MEM_OK);
    uint32_t open[] = {0x8030204, 0, 3, 1, 1, 2, 12, 1, 0, 0x4802, 0x300020, 0x30002, 0x300040};
    ctr_call(p, fs, open, 13, SNX_CTR_SVC_RETURN, 0);
    if (ctr_command_word(p, 1)) {
        snprintf(text, text_size, "result-%08x", ctr_command_word(p, 1));
        return text;
    }
    uint32_t file = ctr_command_word(p, 3), link[] = {0x80c0000};
    ctr_call(p, file, link, 1, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    uint32_t linked = ctr_command_word(p, 3), size_request[] = {0x8040000};
    ctr_call(p, linked, size_request, 1, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    uint32_t size = ctr_command_word(p, 2);
    assert(size && size <= 0x1000);
    uint32_t read[] = {0x80200c2, 0, 0, size, (size << 4) | 12, 0x301000};
    ctr_call(p, linked, read, 6, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1) && ctr_command_word(p, 2) == size);
    unsigned char* image = malloc(size);
    assert(image && snx_ctr_read_ram(p->space, 0x301000, image, size) == SNX_CTR_MEM_OK);
    snprintf(text, text_size, "%s", holder(image, size));
    free(image);
    assert(!snx_ctr_handle_close(p, linked) && !snx_ctr_handle_close(p, file));
    return text;
}
static int with_update(const char* game_path, const char* update_path, const char* state) {
    char error[512];
    SnxCtrSource* game = NULL;
    SnxCtrExtractResult result = snx_ctr_source_open(game_path, &game, error, sizeof error);
    if (result == SNX_CTR_EXTRACT_OK) {
        result = snx_ctr_source_set_update(game, update_path, error, sizeof error);
        /* a second update is refused, the first kept */
        if (result == SNX_CTR_EXTRACT_OK)
            assert(snx_ctr_source_set_update(game, update_path, error, sizeof error) == SNX_CTR_EXTRACT_FORMAT &&
                   strstr(error, "second update"));
    }
    if (result != SNX_CTR_EXTRACT_OK) {
        assert(error[0]);
        snx_ctr_source_close(game);
        printf("refused: %s\n", error);
        return 20 + (int)result;
    }
    uint64_t size, update_size, icon;
    const unsigned char* image = snx_ctr_source_romfs(game, &size);
    const unsigned char* update = snx_ctr_source_update_romfs(game, &update_size);
    const unsigned char* exheader = snx_ctr_source_exheader(game);
    printf("title %016llx name %.8s exheader %016llx romfs %s update-romfs %s icon %d\n",
           (unsigned long long)snx_ctr_source_title(game), (const char*)exheader,
           (unsigned long long)le64(exheader + 0x200), holder(image, size), holder(update, update_size),
           snx_ctr_source_exefs(game, "icon", &icon) != NULL);
    /* the game run with them: SelfNCCH's RomFS (path type 0) and its update RomFS (path type 5) */
    unsigned char code[0x3000] = {0};
    SnxCtrImage process_image = ctr_image(code, sizeof code, 0x3000);
    SnxCtrProcess* p = snx_ctr_process_new(&process_image, SNX_CTR_OLD);
    assert(p && snx_ctr_process_mount_source(p, state, game));
    uint32_t fs;
    assert(!snx_ctr_session_create(p, SNX_CTR_SERVICE_FS, &fs));
    uint32_t init[] = {0x8010002, 0x20, 0xffffffff};
    ctr_call(p, fs, init, 3, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    char self[32], self_update[32];
    printf("self romfs %s update-romfs %s\n", self_romfs(p, fs, 0, self, sizeof self),
           self_romfs(p, fs, 5, self_update, sizeof self_update));
    snx_ctr_process_free(p);
    snx_ctr_source_close(game);
    return 0;
}
int main(int argc, char** argv) {
    if (argc == 5 && !strcmp(argv[2], "--update")) return with_update(argv[1], argv[3], argv[4]);
    if (argc < 2 || argc > 3) return 2;
    char error[512];
    SnxCtrSource* game = NULL;
    SnxCtrExtractResult result = snx_ctr_source_open(argv[1], &game, error, sizeof error);
    if (result != SNX_CTR_EXTRACT_OK) {
        assert(!game && error[0]);
        printf("refused: %s\n", error);
        return 20 + (int)result;
    }
    assert(snx_ctr_source_title(game) == 0x0004000001234500ull);
    if (argc == 3) compare(game, argv[2]);
    snx_ctr_source_close(game);
    return 0;
}
