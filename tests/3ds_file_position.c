/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_seek and snx_tell address a file past 2 GiB, which a Windows long (32 bits) cannot: what is written at a wide
   offset reads back there, the position reports it, and an offset beyond the signed 64-bit range is refused without
   moving the file. A sparse file keeps the cost at a few blocks. */
#include "common/utilities/common.h"
#include "support/sparse_file.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int run(const char* root) {
    static const unsigned char marker[] = "wide-seek";
    const uint64_t at = (3ull << 30) + 123;
    char* path = snx_path_join(root, "wide.bin");
    assert(path);
    FILE* f = snx_fopen(path, "w+b");
    free(path);
    assert(f);
    if (!test_sparse_resize(f, at + 4096)) {
        assert(!fclose(f));
        puts("sparse files are not supported here");
        return 77;
    }
    unsigned char got[sizeof marker];
    assert(!snx_seek(f, at) && snx_tell(f) == (int64_t)at);
    assert(fwrite(marker, 1, sizeof marker, f) == sizeof marker);
    assert(snx_tell(f) == (int64_t)(at + sizeof marker));
    assert(!snx_seek(f, at) && fread(got, 1, sizeof got, f) == sizeof got && !memcmp(got, marker, sizeof marker));
    assert(snx_tell(f) == (int64_t)(at + sizeof marker));
    assert(!snx_seek(f, 0) && snx_tell(f) == 0);
    assert(snx_seek(f, UINT64_MAX) && snx_tell(f) == 0);
    assert(!fclose(f));
    puts("snx_seek and snx_tell address a file past 2 GiB");
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
