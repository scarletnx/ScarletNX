/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* frozen.c: a frozen reference, what a reference that is going away gave a test, kept as text in
   tests/data/<test>_frozen/<config>.txt: a header of "# key value" lines, then a 64-bit hash an item, 16 lowercase hex
   digits a line, part after part in the order the test runs them.
     # <what the file holds, one line>
     # generator N      the test's generator of inputs; a file of another is refused
     # args OPTIONS     the options the test drew its inputs with, as its command line takes them
     # items N...       how many items each part has (A32 blocks, then Thumb ones; functions; ...)
     # inputs HEX       a 64-bit digest of every input drawn, which catches a generator changed but not renumbered
     # recorded TEXT    what recorded it (the reference and the commit), for a reader only
   A test records one with --record FILE where the reference is built and checks its own code against it with
   --frozen FILE */
#ifndef SNX_TEST_FROZEN_H
#define SNX_TEST_FROZEN_H
#include <stddef.h>
#include <stdint.h>

#define SNX_FROZEN_PARTS 4
#define SNX_FROZEN_LINE  8192 /* a header line's bytes, its newline among them */

typedef struct {
    char path[1024];
    unsigned generator;
    char what[256], args[SNX_FROZEN_LINE], recorded[256];
    uint64_t inputs;
    size_t parts, items[SNX_FROZEN_PARTS];
    size_t first_line; /* the file's line of the first hash */
    uint64_t* hashes;  /* every part's, one after another */
} SnxFrozen;

/* the hash an item's results are kept as: each value at its width (a byte, a word of 32 or 64 bits), never a struct's
   bytes or a host's address, so every host and machine makes the same one. One value changed alone always changes it:
   each step is a bijection of h and of v */
static inline uint64_t snx_frozen_mix(uint64_t h, uint64_t v) {
    h = (h ^ v) * 0xBF58476D1CE4E5B9ull;
    return h ^ (h >> 29);
}
/* bytes, eight at a time as a little-endian word, the last ones and the count after */
uint64_t snx_frozen_bytes(uint64_t h, const void* bytes, size_t size);

/* PATH read into f: 0, or -1 with why in error (PATH:LINE and what it found). A generator other than the test's is
   refused, both named */
int snx_frozen_read(SnxFrozen* f, const char* path, unsigned generator, char* error, size_t size);
void snx_frozen_free(SnxFrozen* f);
/* item index of part's hash, and the file's line it is on */
uint64_t snx_frozen_hash(const SnxFrozen* f, size_t part, size_t index);
size_t snx_frozen_line(const SnxFrozen* f, size_t part, size_t index);
/* args split at its spaces into argv (at most max), in place: how many */
int snx_frozen_split(char* args, char** argv, int max);

/* f's header and its hashes written to PATH, through PATH.tmp: 0, or -1 with why in error */
int snx_frozen_write(const SnxFrozen* f, const char* path, char* error, size_t size);

#endif
