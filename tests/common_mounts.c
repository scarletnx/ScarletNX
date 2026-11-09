/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_remove_tree never goes through a mount point (common.c): a folder an extracted game was mounted over (a bind
   mount of its RomFS into a game's data folder) would have its files removed with the game's. The mountinfo reader,
   by text, and on Linux a real mount point refused with nothing removed */
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #x);                                      \
            g_failed = 1;                                                                                              \
        }                                                                                                              \
    } while (0)

#ifdef __linux__ /* the refusal said, which only a Linux run sees: /proc is its mount point */
static char g_said[2048];
static void said(const char* line) { snprintf(g_said, sizeof g_said, "%s", line); }
#endif

/* found is the mount point snx_mount_in gives root in text, or none when NULL */
static void expect(const char* text, const char* root, const char* found) {
    char* got = snx_mount_in(text, root);
    if (found ? !got || strcmp(got, found) : got != NULL)
        fprintf(stderr, "%s in mountinfo: got %s, expected %s\n", root, got ? got : "none", found ? found : "none");
    CHECK(found ? got && !strcmp(got, found) : got == NULL);
    free(got);
}

int main(void) {
    static const char text[] = "22 1 0:21 / / rw,relatime shared:1 - zfs zroot rw\n"
                               "40 22 0:21 /dravee/MKW/base/romfs /home/u/games/G/G_data/romfs rw shared:1 - zfs "
                               "zroot/home rw\n"
                               "41 22 0:30 / /home/u/my\\040games/romfs rw - zfs pool rw\n"
                               "short line\n"
                               "42 22 0:31 / /mnt/back\\134slash rw - ext4 /dev/sdb1 rw";
    expect(text, "/home/u/games/G/G_data", "/home/u/games/G/G_data/romfs");
    expect(text, "/home/u/games/G/G_data/romfs", "/home/u/games/G/G_data/romfs");
    expect(text, "/home/u/games/G/G_data/romfs/sub", NULL); /* inside a mount: the tree is that disk's own */
    expect(text, "/home/u/games/G/G_dat", NULL);
    expect(text, "/home/u/my games", "/home/u/my games/romfs");
    expect(text, "/mnt/back\\slash", "/mnt/back\\slash");
    expect(text, "/home/u/elsewhere", NULL);
    expect("", "/home", NULL);
    {
        char *root = snx_temp_dir(NULL), *tree = root ? snx_path_join(root, "tree/sub") : NULL, *file;
        CHECK(tree && !snx_mkdirs(tree));
        file = tree ? snx_path_join(tree, "file") : NULL;
        CHECK(file && !snx_write_file(file, "x", 1));
        CHECK(snx_mount_within(root) == NULL && !snx_remove_tree(root) && !snx_path_exists(root));
        free(file);
        free(tree);
        free(root);
    }
#ifdef __linux__
    { /* /proc is a mount point on every Linux system: refused before anything in it is touched */
        char* found = snx_mount_within("/proc");
        CHECK(found && !strcmp(found, "/proc"));
        free(found);
        snx_error_sink(said);
        CHECK(snx_remove_tree("/proc") != 0 && snx_is_dir("/proc/self"));
        snx_error_sink(NULL);
        CHECK(strstr(g_said, "cannot remove /proc: /proc is a mount point") != NULL);
    }
#endif
    if (!g_failed) puts("PASS mount points stop a tree's removal");
    return g_failed;
}
