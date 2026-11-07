/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ZBIC's decoder: zstd 1.5.7's own decompressor (BSD-3-Clause, the sources snx_zstd builds, cmake/dependencies.cmake)
   built once more in this file, as zstd's single-file decoder (build/single_file_libs/zstddeclib-in.c) takes it, every
   symbol of it local to the file, with two differences:
   - a frame's magic number is "ZBIC" (0x4349425A) instead of zstd's
   - an FSE table's header (FSE_readNCount: the literals' Huffman weights and the sequences' three tables) is read by
     zbic_read_ncount instead of zstd's reader, which is built out of the way under another name

   The header of an FSE table: byte 0's bit 7 says whether counts of -1 ("less than one") appear, its bits 0-6 how many
   bytes follow. Those bytes hold one number, the last byte first: the reader keeps a value of at least 2^40 while
   bytes remain, appending the next byte below it whenever a division leaves it smaller. Out of that number come, by
   division: the largest symbol less one (modulo 52), the table's log less 5 (modulo 8), and, modulo 2^log, the
   cumulative count past the largest symbol, less one (with -1 counts, where every count is kept plus one, less the
   largest symbol plus two). The cumulative counts at 0 to P, P the power of two above the largest symbol, are known at
   both ends, 0 and that last one, and the ones between are decoded by binary interpolative coding: in zbic_order's
   order, each interval [a, b] of more than one step gives the count at its middle, the one at a plus the next number
   modulo (the count at b less the count at a, plus one), unless the counts at a and b are equal, which fills the
   interval with that count and takes nothing. A last step over [0, 1] takes a number nothing uses. A symbol's count
   is the difference of the cumulative counts after and before it (less one with -1 counts), and the magnitudes add up
   to 2^log. Every byte of the header must be used

   The format as kinnay's zbic decodes it (github.com/kinnay/zbic, GPL-2.0, whose zstd.c is Atmosphere-NX's with ZBIC
   support): this reader was written after reading that one, and gives the same bytes as it on the segments of two
   Ounce games. None of its code is here, and its table of intervals is zbic_order's traversal */
#include "zbic.h"

#define DEBUGLEVEL 0
#define MEM_MODULE
#undef XXH_NAMESPACE
#define XXH_NAMESPACE snx_zbic_
#undef XXH_PRIVATE_API
#define XXH_PRIVATE_API
#undef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#define ZSTD_LEGACY_SUPPORT 0
#define ZSTD_STRIP_ERROR_STRINGS
#define ZSTD_TRACE           0
#define ZSTD_DISABLE_ASM     1
/* zstd's API local to this file, beside snx_zstd's */
#define ZSTDLIB_VISIBLE      static
#define ZSTDLIB_HIDDEN       static
#define ZSTDERRORLIB_VISIBLE static
#define ZSTDERRORLIB_HIDDEN  static
#include "zbic_names.h"

/* zstd's FSE header reader, out of the way */
#define FSE_readNCount      snx_zbic_zstd_read_ncount
#define FSE_readNCount_bmi2 snx_zbic_zstd_read_ncount_bmi2
#define ZSTD_DEPS_NEED_MALLOC
#include "common/zstd_deps.h"
#include "common/debug.c"
#include "common/entropy_common.c"
#undef FSE_readNCount
#undef FSE_readNCount_bmi2

#define ZBIC_FILL      ((U64)1 << 40) /* the number's value is kept at least this while bytes remain */
#define ZBIC_MAX_POWER 256            /* the power of two above the largest symbol is at most this */

typedef struct {
    const BYTE* bytes; /* the header: bytes[1] to bytes[left] are still to be read, the last first */
    size_t left;
    U64 value;
} ZbicNumber;

static void zbic_fill(ZbicNumber* n) {
    while (n->left && n->value < ZBIC_FILL) n->value = n->value << 8 | n->bytes[n->left--];
}
/* the next digit of radix radix */
static U64 zbic_take(ZbicNumber* n, U64 radix) {
    U64 digit = n->value % radix;
    n->value /= radix;
    zbic_fill(n);
    return digit;
}
/* the intervals of [a, b] whose middles are decoded, in order: an interval, then its right half, then its left half.
   steps[i] is {middle, a, b}; the count of steps after them */
static unsigned zbic_order(unsigned a, unsigned b, unsigned short (*steps)[3], unsigned n) {
    unsigned middle;
    if (b - a < 2) return n;
    middle = (a + b) / 2;
    steps[n][0] = (unsigned short)middle;
    steps[n][1] = (unsigned short)a;
    steps[n][2] = (unsigned short)b;
    n = zbic_order(middle, b, steps, n + 1);
    return zbic_order(a, middle, steps, n);
}

static size_t zbic_read_ncount(short* normalizedCounter, unsigned* maxSVPtr, unsigned* tableLogPtr,
                               const void* headerBuffer, size_t hbSize) {
    const BYTE* const header = (const BYTE*)headerBuffer;
    U32 cumulative[ZBIC_MAX_POWER + 1];
    unsigned short steps[ZBIC_MAX_POWER][3];
    ZbicNumber number;
    unsigned lowProb, maxSymbol, tableLog, power, nSteps, i;
    U32 remaining, previous;
    U64 total;
    if (!hbSize) return ERROR(srcSize_wrong);
    lowProb = header[0] >> 7;
    number.bytes = header;
    number.left = header[0] & 0x7F;
    number.value = 0;
    if (number.left + 1 > hbSize) return ERROR(srcSize_wrong);
    zbic_fill(&number);
    maxSymbol = (unsigned)zbic_take(&number, 52) + 1;
    if (maxSymbol > *maxSVPtr) return ERROR(maxSymbolValue_tooSmall);
    tableLog = (unsigned)zbic_take(&number, 8) + 5;
    if (tableLog > FSE_TABLELOG_ABSOLUTE_MAX) return ERROR(tableLog_tooLarge);
    total = zbic_take(&number, (U64)1 << tableLog) + (lowProb ? maxSymbol + 2 : 1);
    for (power = 2; power <= maxSymbol; power <<= 1) {}
    if (power > ZBIC_MAX_POWER / 2) return ERROR(corruption_detected);
    ZSTD_memset(cumulative, 0, sizeof cumulative);
    cumulative[power] = (U32)total;
    nSteps = zbic_order(0, power, steps, 0);
    steps[nSteps][0] = 0;
    steps[nSteps][1] = 0;
    steps[nSteps][2] = 1;
    nSteps++;
    for (i = 0; i < nSteps; i++) {
        unsigned middle = steps[i][0], a = steps[i][1], b = steps[i][2], k;
        if (cumulative[a] == cumulative[b]) {
            for (k = a + 1; k < b; k++) cumulative[k] = cumulative[a];
            continue;
        }
        if (cumulative[b] < cumulative[a]) return ERROR(corruption_detected);
        cumulative[middle] = cumulative[a] + (U32)zbic_take(&number, (U64)(cumulative[b] - cumulative[a]) + 1);
    }
    /* the counts: after the last step, cumulative[0] holds the number nothing uses, so the first count is from 0 */
    remaining = 1u << tableLog;
    previous = 0;
    for (i = 0; i <= maxSymbol; i++) {
        long count = (long)cumulative[i + 1] - (long)previous - (long)lowProb;
        U32 magnitude = (U32)(count < 0 ? -count : count);
        previous = cumulative[i + 1];
        if (count < -1 || magnitude > remaining) return ERROR(corruption_detected);
        normalizedCounter[i] = (short)count;
        remaining -= magnitude;
    }
    if (remaining || number.left) return ERROR(corruption_detected);
    *maxSVPtr = maxSymbol;
    *tableLogPtr = tableLog;
    return (size_t)(header[0] & 0x7F) + 1;
}
static size_t zbic_read_ncount_bmi2(short* normalizedCounter, unsigned* maxSVPtr, unsigned* tableLogPtr,
                                    const void* headerBuffer, size_t hbSize, int bmi2) {
    (void)bmi2;
    return zbic_read_ncount(normalizedCounter, maxSVPtr, tableLogPtr, headerBuffer, hbSize);
}

/* the rest of the decoder, with ZBIC's reader and magic */
#define FSE_readNCount      zbic_read_ncount
#define FSE_readNCount_bmi2 zbic_read_ncount_bmi2
#include "zstd.h"
#undef ZSTD_MAGICNUMBER
#define ZSTD_MAGICNUMBER 0x4349425A
#include "common/error_private.c"
#include "common/fse_decompress.c"
#include "common/zstd_common.c"
#include "decompress/huf_decompress.c"
#include "decompress/zstd_ddict.c"
#include "decompress/zstd_decompress.c"
#include "decompress/zstd_decompress_block.c"

int snx_zbic_is(const void* src, size_t size) { return size >= 4 && MEM_readLE32(src) == ZSTD_MAGICNUMBER; }

long snx_zbic_decompress(void* dst, size_t dst_size, const void* src, size_t src_size) {
    size_t written = ZSTD_decompress(dst, dst_size, src, src_size);
    return ZSTD_isError(written) || written > 0x7FFFFFFF ? -1 : (long)written;
}
