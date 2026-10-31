/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* vita/loader/inflate.c and vpk.c, on streams and a zip another encoder made (miniz's): raw DEFLATE and zlib read to
   exactly their size, a stream of another size, a cut one and a zlib one whose Adler-32 is wrong refused; a VPK with one
   member stored and one deflated read by name and extracted whole, and a member whose CRC-32 doesn't match refused */
#include "vita/loader/inflate.h"
#include "vita/loader/vpk.h"
#include "common/utilities/common.h"
#include <assert.h>
#include <miniz.h>
#include <stdlib.h>
#include <string.h>

#define SIZE (1u << 20)

int main(void) {
    unsigned char *data = malloc(SIZE), *packed = malloc(SIZE + 4096), *out = malloc(SIZE + 1);
    size_t raw, zlib, i;
    mz_ulong room = SIZE + 4096;
    assert(data && packed && out);
    for (i = 0; i < SIZE; ++i) data[i] = (unsigned char)(i % 251 < 40 ? i * 2654435761u >> 24 : i % 7);
    /* raw DEFLATE, as a VPK's member */
    raw = tdefl_compress_mem_to_mem(packed, SIZE + 4096, data, SIZE, TDEFL_MAX_PROBES_MASK);
    assert(raw && raw < SIZE);
    assert(vita_inflate(packed, raw, out, SIZE) && !memcmp(out, data, SIZE));
    assert(!vita_inflate(packed, raw, out, SIZE - 1) && !vita_inflate(packed, raw, out, SIZE + 1));
    assert(!vita_inflate(packed, raw / 2, out, SIZE));
    /* zlib, as a compressed SELF segment: its header and Adler-32 are checked */
    assert(mz_compress2(packed, &room, data, SIZE, MZ_BEST_COMPRESSION) == MZ_OK);
    zlib = (size_t)room;
    assert(vita_zlib_inflate(packed, zlib, out, SIZE) && !memcmp(out, data, SIZE));
    packed[zlib - 1] ^= 1;
    assert(!vita_zlib_inflate(packed, zlib, out, SIZE));
    packed[zlib - 1] ^= 1;
    packed[0] = 0; /* no zlib header */
    assert(!vita_zlib_inflate(packed, zlib, out, SIZE));
    {
        char *dir = snx_temp_dir(NULL), *vpk = snx_path_join(dir, "game.vpk"), *folder = snx_path_join(dir, "app");
        char *eboot = snx_path_join(folder, "eboot.bin"), *member = snx_path_join(folder, "data/level.bin"), error[512];
        static const char text[] = "an eboot stored as it is";
        mz_zip_archive zip = {0};
        void* bytes;
        size_t n;
        unsigned char *read, *file;
        assert(dir && mz_zip_writer_init_heap(&zip, 0, 0));
        assert(mz_zip_writer_add_mem(&zip, "eboot.bin", text, sizeof text - 1, MZ_NO_COMPRESSION));
        assert(mz_zip_writer_add_mem(&zip, "data/level.bin", data, SIZE, MZ_BEST_COMPRESSION));
        assert(mz_zip_writer_finalize_heap_archive(&zip, &bytes, &n) && !snx_write_file(vpk, bytes, n));
        mz_zip_writer_end(&zip);
        {
            unsigned char *at = bytes, *end = at + n - (sizeof text - 1); /* the stored eboot's bytes, one changed */
            char* bad = snx_path_join(dir, "bad.vpk");
            while (at <= end && memcmp(at, text, sizeof text - 1)) ++at;
            assert(at <= end);
            at[3] ^= 1;
            assert(bad && !snx_write_file(bad, bytes, n));
            assert(!vita_vpk_member(bad, "eboot.bin", &n, error, sizeof error) && strstr(error, "CRC"));
            free(bad);
        }
        mz_free(bytes);
        read = vita_vpk_member(vpk, "data/level.bin", &n, error, sizeof error);
        assert(read && n == SIZE && !memcmp(read, data, SIZE));
        free(read);
        assert(vita_vpk_extract(vpk, folder, error, sizeof error));
        file = snx_read_file(eboot, &n);
        assert(file && n == sizeof text - 1 && !memcmp(file, text, n));
        free(file);
        file = snx_read_file(member, &n);
        assert(file && n == SIZE && !memcmp(file, data, SIZE));
        free(file);
        assert(!snx_remove_tree(dir));
        free(member);
        free(eboot);
        free(folder);
        free(vpk);
        free(dir);
    }
    free(out);
    free(packed);
    free(data);
    puts("vita_inflate: raw and zlib streams to their size, the wrong ones refused, a VPK read and extracted");
    return 0;
}
