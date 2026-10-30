/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The level-3 view takes an image of up to 4 GiB (SNX_CTR_MAX_CONTENT) and refuses one byte more. The image outlives
   the view, so it is a sparse file of exactly the size the view is told about, mapped whole. The header and the last
   bytes are read back through the reader, so the mapping really covers the size the view records. */
#include "3ds/services/romfs.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "support/sparse_file.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { HEADER_SIZE = 0x28 }; /* the level-3 header's own size, which its first word holds */

static int run(const char* root) {
    const uint64_t gib = 0x40000000ull;
    const uint64_t extent = 4 * gib + 1;
    char* path = snx_path_join(root, "image.bin");
    assert(path);
    FILE* file = snx_fopen(path, "wb");
    assert(file);
    if (!test_sparse_resize(file, extent)) {
        assert(!fclose(file));
        puts("sparse files are not supported here");
        free(path);
        return 77;
    }
    unsigned char header[HEADER_SIZE] = {0};
    snx_st_le32(header, HEADER_SIZE);
    assert(fwrite(header, 1, sizeof header, file) == sizeof header);
    assert(!fclose(file));
    TestMap map;
    bool mapped = test_map_open(&map, path, extent);
    free(path);
    if (!mapped) {
        puts("a file of this size cannot be mapped here");
        return 77;
    }
    assert(map.base && map.size == (size_t)extent);
    /* one byte past the runtime's policy: refused, and no view is handed out */
    SnxCtrRomfs* view = NULL;
    assert(!snx_ctr_romfs_open_image(map.base, 4 * gib + 1, 0, &view) && !view);
    /* the bound itself, with a backing image that really is that large */
    assert(snx_ctr_romfs_open_image(map.base, 4 * gib, 0, &view) && view && snx_ctr_romfs_size(view) == 4 * gib);
    unsigned char got[4] = {0xff, 0xff, 0xff, 0xff};
    assert(snx_ctr_romfs_read(view, 0, got, sizeof got));
    assert(snx_ld_le32(got) == HEADER_SIZE);
    /* the mapping covers the whole extent the view records: a hole reads as zero at its end */
    assert(snx_ctr_romfs_read(view, 4 * gib - sizeof got, got, sizeof got));
    assert(!snx_ld_le32(got));
    snx_ctr_romfs_free(view);
    /* a size between 1 GiB and the cap is accepted */
    assert(snx_ctr_romfs_open_image(map.base, gib + 1, 0, &view) && view && snx_ctr_romfs_size(view) == gib + 1);
    snx_ctr_romfs_free(view);
    /* too small for the header is still refused */
    assert(!snx_ctr_romfs_open_image(map.base, HEADER_SIZE - 1, 0, &view) && !view);
    test_map_close(&map);
    puts("the CTR level-3 view takes a mapped 4 GiB image and refuses one byte more");
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
