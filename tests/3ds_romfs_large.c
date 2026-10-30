/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A RomFS built from a host tree, its file data past 2 GiB, read through the on-demand path tree.h documents: a file
   that is skipped or cannot be mapped keeps fd -1 and romfs.c reopens and seeks it. The view normally maps the file,
   so the test forces fd -1 and map NULL. The marker is written with the C library's own seek, not snx_seek, so an
   offset bug they share cannot hide. */
#include "3ds/services/romfs.h"
#include "common/filesystem/tree.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "support/sparse_file.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { HEADER_SIZE = 40 }; /* the level-3 header's own size, which its first word holds */

static int run(const char* root) {
    static const unsigned char pattern[16] = {0x53, 0x4e, 0x58, 0x2d, 0x52, 0x4f, 0x4d, 0x46,
                                              0x53, 0x2d, 0x32, 0x47, 0x49, 0x42, 0x21, 0x00};
    const uint64_t file_size = (2ull << 30) + 0x4000;
    /* past the 32-bit signed range of a Windows long, which the on-demand read must not narrow its offset to */
    assert(file_size - sizeof pattern > 0x7fffffffull);
    char* directory = snx_path_join(root, "romfs");
    char* path = directory ? snx_path_join(directory, "big.bin") : NULL;
    assert(directory && path && !snx_mkdirs(directory));
    free(directory);
    FILE* file = snx_fopen(path, "w+b");
    free(path);
    assert(file);
    if (!test_sparse_resize(file, file_size)) {
        assert(!fclose(file));
        puts("sparse files are not supported here");
        return 77;
    }
    test_seek_at(file, file_size - sizeof pattern);
    assert(fwrite(pattern, 1, sizeof pattern, file) == sizeof pattern);
    assert(!fclose(file));
    FsTree tree = {0};
    assert(fs_tree_init(&tree, root) == FS_TREE_OK);
    SnxCtrRomfs* view = NULL;
    assert(snx_ctr_romfs_open(&tree, &view) && view);
    const uint64_t size = snx_ctr_romfs_size(view);
    assert(size > file_size && size < (4ull << 30));
    unsigned char header[HEADER_SIZE];
    assert(snx_ctr_romfs_read(view, 0, header, sizeof header));
    assert(snx_ld_le32(header) == HEADER_SIZE);
    FsNode* node = NULL;
    pm_lock(&tree.lock);
    assert(fs_tree_lookup(&tree, NULL, "romfs/big.bin", &node) == FS_TREE_OK && node);
    /* The read must leave fd and map as it found them: it opens and seeks the host file itself */
    int mapped_fd = node->fd;
    const uint8_t* mapped = node->map;
    node->fd = -1;
    node->map = NULL;
    pm_unlock(&tree.lock);
    unsigned char tail[sizeof pattern];
    assert(snx_ctr_romfs_read(view, size - sizeof tail, tail, sizeof tail) && !memcmp(tail, pattern, sizeof tail));
    pm_lock(&tree.lock);
    assert(node->fd == -1 && !node->map);
    node->fd = mapped_fd;
    node->map = mapped;
    pm_unlock(&tree.lock);
    snx_ctr_romfs_free(view);
    fs_tree_destroy(&tree);
    puts("a tree-backed RomFS reads known bytes past 2 GiB through its on-demand path");
    return 0;
}
int main(void) {
    char* root = snx_temp_dir(NULL);
    assert(root);
    int status = run(root);
    assert(!snx_remove_tree(root));
    free(root);
    return status;
}
