/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Format reference: Azahar Z3DSWriteIOFile and Z3DSReadIOFile (common/file_derived.{h,cpp}, GPL-3.0-or-later,
   read, not ported) over zstd's seekable format. */
/* A z3ds game, what Azahar's launcher keeps a game in to take less disk: a 0x20-byte header whose magic is
   "Z3DS", then what metadata it carries, then the game as a zstd seekable stream — the game's bytes in zstd
   frames, a seek table after them as a skippable frame. A streaming decompressor takes the frames in order and
   skips the table, so the plain game is unpacked once into the container the loader reads, published only
   complete, and read from then on as that container is. */
#include "3ds/loader/internal.h"
#include <zstd.h>
#include <stdlib.h>
#include <string.h>

#define Z3DS_HEADER 0x20u
#define Z3DS_CHUNK (4u * 1024u * 1024u)
SnxCtrExtractResult snx_ctr_unzstd(const char* input, const char* output, char* error, size_t error_size) {
    CtrReader r = {.error = error, .error_size = error_size};
    if (error_size) error[0] = 0;
    FILE* in = NULL;
    FILE* out = NULL;
    char* stage = NULL;
    unsigned char* chunk = NULL, *plain = NULL; /* the stream's bytes in, the game's out: not one buffer */
    ZSTD_DStream* z = NULL;
    uint64_t written = 0, fed = 0;
    bool ok = false;
    if (!input || !(in = fopen(input, "rb"))) {
        ctr_fail(&r, SNX_CTR_EXTRACT_IO, "cannot open the game");
        goto done;
    }
    r.size = snx_file_size(input);
    unsigned char header[Z3DS_HEADER];
    if (r.size < Z3DS_HEADER || fread(header, 1, sizeof header, in) != sizeof header ||
        memcmp(header, "Z3DS", 4)) {
        ctr_fail(&r, SNX_CTR_EXTRACT_UNSUPPORTED, "the game is not a z3ds file");
        goto done;
    }
    if (header[8] != 1 || header[9]) {
        ctr_fail(&r, SNX_CTR_EXTRACT_UNSUPPORTED, "unsupported z3ds version %u", header[8]);
        goto done;
    }
    uint64_t header_size = snx_ld_le16(header + 0xA), metadata_size = snx_ld_le32(header + 0xC),
             compressed = snx_ld_le64(header + 0x10), uncompressed = snx_ld_le64(header + 0x18);
    /* the stream lies whole after the header and its metadata, and the game is no larger than a card is */
    if (header_size < Z3DS_HEADER || !ctr_fits(r.size, header_size, metadata_size) ||
        header_size + metadata_size + compressed != r.size || !uncompressed || uncompressed > CTR_CARD_LIMIT) {
        ctr_fail(&r, SNX_CTR_EXTRACT_FORMAT, "invalid z3ds header or sizes");
        goto done;
    }
    if (snx_seek(in, header_size + metadata_size)) {
        ctr_fail(&r, SNX_CTR_EXTRACT_IO, "cannot read the z3ds stream");
        goto done;
    }
    stage = snx_format("%s.partial", output);
    if (!stage || !(out = fopen(stage, "wb"))) {
        ctr_fail(&r, SNX_CTR_EXTRACT_IO, "cannot create %s", stage ? stage : output);
        goto done;
    }
    chunk = malloc(Z3DS_CHUNK);
    plain = malloc(Z3DS_CHUNK);
    z = ZSTD_createDStream();
    if (!chunk || !plain || !z || ZSTD_isError(ZSTD_initDStream(z))) {
        ctr_fail(&r, SNX_CTR_EXTRACT_LIMIT, "zstd's streaming decompressor cannot start");
        goto done;
    }
    for (;;) {
        ZSTD_inBuffer in_buffer = {chunk, 0, 0};
        if (fed < compressed) {
            uint64_t want = compressed - fed < Z3DS_CHUNK ? compressed - fed : Z3DS_CHUNK;
            in_buffer.size = fread(chunk, 1, (size_t)want, in);
            if (in_buffer.size != want) {
                ctr_fail(&r, SNX_CTR_EXTRACT_IO, "cannot read the z3ds stream");
                goto done;
            }
            fed += in_buffer.size;
        }
        ZSTD_outBuffer out_buffer = {plain, Z3DS_CHUNK, 0};
        size_t result = ZSTD_decompressStream(z, &out_buffer, &in_buffer);
        if (ZSTD_isError(result)) {
            ctr_fail(&r, SNX_CTR_EXTRACT_FORMAT, "the z3ds stream is not a whole zstd one: %s",
                     ZSTD_getErrorName(result));
            goto done;
        }
        if (out_buffer.pos) {
            if (written + out_buffer.pos > uncompressed) {
                ctr_fail(&r, SNX_CTR_EXTRACT_FORMAT, "the z3ds stream holds more than its header says");
                goto done;
            }
            if (fwrite(plain, 1, out_buffer.pos, out) != out_buffer.pos) {
                ctr_fail(&r, SNX_CTR_EXTRACT_IO, "cannot write the unpacked game");
                goto done;
            }
            written += out_buffer.pos;
        }
        if (out_buffer.pos) continue; /* the decompressor made room: feed it or pump it again */
        if (fed < compressed) continue; /* more of the stream to feed */
        if (fed == compressed && in_buffer.pos == in_buffer.size) {
            /* everything fed and nothing more came out: the game is whole when its header's size says so, and
               otherwise the stream ends before it does (the seek table's footer, a skippable frame, is the tail
               the decompressor skips, and waits behind it) */
            if (written == uncompressed) break;
            ctr_fail(&r, SNX_CTR_EXTRACT_FORMAT,
                     "the z3ds stream ends with %llu of the %llu bytes of the game its header says",
                     (unsigned long long)written, (unsigned long long)uncompressed);
            goto done;
        }
    }
    if (fclose(out)) {
        out = NULL;
        ctr_fail(&r, SNX_CTR_EXTRACT_IO, "cannot write the unpacked game");
        goto done;
    }
    out = NULL;
    if (snx_replace(stage, output)) {
        ctr_fail(&r, SNX_CTR_EXTRACT_IO, "cannot move %s to %s", stage, output);
        goto done;
    }
    ok = true;
done:
    if (out) fclose(out);
    if (!ok && stage) remove(stage);
    ZSTD_freeDStream(z);
    free(chunk);
    free(plain);
    free(stage);
    if (in) fclose(in);
    return ok ? SNX_CTR_EXTRACT_OK : r.result;
}
