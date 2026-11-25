/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* NCZ: an NCA compressed by nsz (github.com/nicoboss/nsz), the content of its NSZ and XCZ dumps, read as the NCA it
   holds. Its layout, as nsz's Decompressor.py and BlockDecompressorReader.py read it:

   - 0 to 0x4000: the NCA's first 0x4000 bytes as they are (its header, still encrypted).
   - 0x4000: "NCZSECTN", a 64-bit count, then that many sections of 0x40 bytes: the NCA offset and size, the crypto
     type, 8 zero bytes, the AES key and the counter, whose first 8 bytes are the upper half of the AES-CTR counter.
   - Then "NCZBLOCK", its version (2), type (1), a zero byte, the block size as a power of 2 (14 to 32), a 32-bit block
     count, the 64-bit size decompressed (the NCA's past 0x4000) and each block's stored size, 32 bits each, then the
     blocks one after another: one stored at its whole size is as is, a smaller one a zstd frame ("block", nsz -B).
     Else one zstd stream ("solid", nsz -S).

   What decompresses is the NCA from 0x4000 to its end: the sections one after another (a gap before the first is
   bytes as they are; a first that starts before 0x4000 goes on from there), those of crypto type 3 (AES-CTR) and 4
   (AesCtrEx, one section a run) decrypted. They are encrypted again here, the counter of a byte its section's upper
   half followed by its NCA offset / 16, as nsz's AESCTR.seek sets it; any other type is as it is. */
#ifndef HACTOOL_NCZ_H
#define HACTOOL_NCZ_H

#include <stddef.h>
#include "storage.h"

#define NCZ_HEADER_SIZE 0x4000 /* the NCA's bytes an NCZ keeps as they are */

enum { NCZ_SOLID = 1, NCZ_BLOCK = 2 };

typedef struct ncz ncz_t;

/* The NCZ whose bytes packed holds, which it takes (freed with it, also when it fails): NULL for one whose header
   cannot be read, with why, naming where in the NCZ and what is there, in why (size bytes). */
ncz_t *ncz_open(storage_t *packed, char *why, size_t size);
void ncz_free(ncz_t *ncz);
int ncz_kind(const ncz_t *ncz);
/* The NCA it holds: a read decompresses the blocks, or decodes the stream as far as, it covers, and encrypts again what
   was encrypted. Not thread safe: it keeps the blocks and stream pages last read, and a solid stream where it got to.
   It lives as long as the NCZ and is never storage_free'd. A failed read says why on stderr: where, in the NCZ and
   in its NCA, and what zstd or the file said */
storage_t *ncz_storage(ncz_t *ncz);
/* the same bytes read from packed from now on (the container mapped), which it takes, freeing the one it had */
void ncz_set_packed(ncz_t *ncz, storage_t *packed);
/* said(line, data) each time a solid NCZ's read lies before where its stream got to and is no page it keeps: its stream
   is decoded again from its start, which the line says with how far. NULL for none */
void ncz_on_restart(void (*said)(const char *line, void *data), void *data);

#endif
