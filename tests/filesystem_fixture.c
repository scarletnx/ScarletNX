/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Native fixture runner for the existing filesystem harnesses; no Python dependency. */
#include "common/utilities/common.h"
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

typedef struct {
    const char *path, *data;
    size_t size;
} Fixture;

static const Fixture files[] = {{"data/drive/seed.bin", "initial", 7},
                                {"data/drive/folder/nested.bin", "nested", 6},
                                {"data/drive/folder/sub/deep.bin", "deep", 4},
                                {"data/drive/partial/a.bin", "first", 5},
                                {"data/drive/partial/blocked/known.bin", "known", 5},
                                {"data/root/root.txt", "root", 4},
                                {"data/romfs/asset.bin", "asset", 5},
                                {"data/romfs.bin", "raw", 3},
                                {"hardlinks/original.bin", "shared", 6},
                                {"service-data/sdmc/seed.bin", "seed", 4},
                                {"service-data/romfs/asset.bin", "romfs test", 10},
                                {"service-data/romfs/empty.bin", "", 0},
                                {"service-data/romfs/zulu.bin", "tail", 4},
#if !defined(_WIN32) /* names past ASCII: the RomFS name hash takes their bytes unsigned */
                                {"service-data/romfs/dir/\xc3\xa9t\xc3\xa9.bin", "summer", 6},
                                {"service-data/romfs/mu/\xce\xbc.bin", "mu", 2},
#endif
                                {"service-data/root/root.txt", "root", 4},
                                {"raw-data/romfs.bin", "RAW\0ROMFS\xff bytes", 16}};

static int fixtures(const char* scratch) {
    static const char* directories[] = {"data/drive/empty", "without-root"};
    char *path, *parent, *original, *alias;
    size_t i;
    int result;
    for (i = 0; i < sizeof files / sizeof files[0]; i++) {
        path = snx_path_join(scratch, files[i].path);
        parent = snx_dirname(path);
        result = snx_mkdirs(parent) || snx_write_file(path, files[i].data, files[i].size);
        free(parent);
        free(path);
        if (result) return 1;
    }
    for (i = 0; i < sizeof directories / sizeof directories[0]; i++) {
        path = snx_path_join(scratch, directories[i]);
        result = snx_mkdirs(path);
        free(path);
        if (result) return 1;
    }
    original = snx_path_join(scratch, "hardlinks/original.bin");
    alias = snx_path_join(scratch, "hardlinks/alias.bin");
#if defined(_WIN32)
    result = !CreateHardLinkA(alias, original, NULL);
#else
    result = link(original, alias) != 0;
#endif
    free(original);
    free(alias);
    return result;
}

int main(int argc, char** argv) {
    static const char token[] = "scarletnx-native-fixture";
    const char* command[5];
    char *parent, *scratch, *marker;
    unsigned char* check;
    size_t size;
    int result;
    if (argc != 2 && argc != 3) return snx_error("Usage: filesystem_fixture HARNESS [raw]");
    parent = snx_temp_dir(NULL);
    if (!parent) return snx_error("Cannot create filesystem test scratch directory");
    scratch = snx_path_join(parent, "scarletnx-fs-tree-fixture");
    marker = snx_path_join(parent, "fixture-owner");
    result = snx_write_file(marker, token, sizeof token - 1);
    if (!result) result = fixtures(scratch);
    if (!result) {
        char* test_marker;
        test_marker = snx_path_join(scratch, ".snx-fs-test");
        result = snx_write_file(test_marker, token, sizeof token - 1);
        free(test_marker);
    }
    if (!result) {
        command[0] = argv[1];
        command[1] = scratch;
        command[2] = token;
        command[3] = argc == 3 ? argv[2] : NULL;
        command[4] = NULL;
        result = snx_run(command, scratch, NULL, NULL);
    }
    check = snx_read_file(marker, &size);
    if (!check || size != sizeof token - 1 || memcmp(check, token, size) || snx_is_link(parent)) {
        snx_error("Refusing to remove unverified filesystem test scratch: %s", parent);
        result = 1;
    } else if (snx_remove_tree(parent))
        result = snx_error("Cannot remove filesystem test scratch");
    free(check);
    free(marker);
    free(scratch);
    free(parent);
    return result ? 1 : 0;
}
