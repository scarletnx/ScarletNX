/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* support/frozen.c: a file written and read back the same, and every file that is not one refused with its line and
   what is wrong there, never read as a reference that holds less than it says. The files are made in the working
   directory and removed */
#include "support/frozen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t g_failures;

static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}
static void put(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    if (!f || fputs(text, f) < 0) {
        fprintf(stderr, "FAIL: cannot write %s\n", path);
        exit(1);
    }
    fclose(f);
}

#define HEADER                                                                                                         \
    "# a test's frozen reference\n# generator 3\n# args --profile a9 --seed 0x5\n# items 2 1\n"                        \
    "# inputs 00000000000000ff\n"
#define HASHES "0123456789abcdef\nfedcba9876543210\n0000000000000001\n"

/* text, read: refused with an error naming path:line and holding want */
static void refused(const char* text, unsigned generator, const char* line, const char* want) {
    static const char path[] = "frozen_file_bad.txt";
    char error[700], where[64];
    SnxFrozen f;
    put(path, text);
    int rc = snx_frozen_read(&f, path, generator, error, sizeof error);
    snprintf(where, sizeof where, "%s:%s: ", path, line);
    if (rc != -1 || !strstr(error, where) || !strstr(error, want)) {
        fprintf(stderr, "FAIL: a file refused at line %s with \"%s\": %s (%s)\n", line, want,
                rc ? error : "it was read", rc ? "another error" : "no error");
        ++g_failures;
    }
    if (f.hashes) {
        fprintf(stderr, "FAIL: a file refused at line %s keeps its hashes\n", line);
        ++g_failures;
    }
    remove(path);
}

int main(void) {
    static const char path[] = "frozen_file_good.txt";
    char error[700] = "";
    SnxFrozen out = {0}, in;
    uint64_t hashes[3] = {0x0123456789abcdefull, 0xfedcba9876543210ull, 1};

    /* written and read back */
    strcpy(out.what, "a test's frozen reference");
    out.generator = 3;
    strcpy(out.args, "--profile a9 --seed 0x5");
    strcpy(out.recorded, "LLVM 23.1.6 -O0, scarletnx 0123456789");
    out.inputs = 0xff;
    out.parts = 2, out.items[0] = 2, out.items[1] = 1;
    out.hashes = hashes;
    check(!snx_frozen_write(&out, path, error, sizeof error), error);
    check(!snx_frozen_read(&in, path, 3, error, sizeof error), error);
    check(!strcmp(in.what, out.what) && in.generator == 3 && !strcmp(in.args, out.args) &&
              !strcmp(in.recorded, out.recorded) && in.inputs == 0xff && in.parts == 2 && in.items[0] == 2 &&
              in.items[1] == 1,
          "the header read back is not the one written");
    check(in.hashes && snx_frozen_hash(&in, 0, 0) == hashes[0] && snx_frozen_hash(&in, 0, 1) == hashes[1] &&
              snx_frozen_hash(&in, 1, 0) == hashes[2],
          "the hashes read back are not the ones written");
    check(snx_frozen_line(&in, 0, 0) == 7 && snx_frozen_line(&in, 1, 0) == 9, "the lines of the hashes");
    snx_frozen_free(&in);
    /* a generator other than the test's */
    check(snx_frozen_read(&in, path, 4, error, sizeof error) == -1 && strstr(error, "frozen_file_good.txt:7: ") &&
              strstr(error, "generator 3, the test's is 4"),
          "a file of another generator is not refused naming both");
    remove(path);
    /* a checkout that writes CRLF, and no recorded line */
    put(path, "# a test's frozen reference\r\n# generator 3\r\n# args --profile a9 --seed 0x5\r\n# items 2 1\r\n"
              "# inputs 00000000000000ff\r\n0123456789abcdef\r\nfedcba9876543210\r\n0000000000000001\r\n");
    check(!snx_frozen_read(&in, path, 3, error, sizeof error) && !strcmp(in.args, "--profile a9 --seed 0x5") &&
              snx_frozen_hash(&in, 1, 0) == 1 && !*in.recorded,
          *error ? error : "a file of CRLF lines is not read as the same");
    snx_frozen_free(&in);
    remove(path);
    check(snx_frozen_read(&in, "frozen_file_none.txt", 3, error, sizeof error) == -1 &&
              strstr(error, "frozen_file_none.txt: cannot read it"),
          "a file that is not there");

    /* what is not a frozen file */
    refused("", 3, "0", "no hashes");
    refused("generator 3\n", 3, "1", "is no title");
    refused(HEADER "# seed 5\n" HASHES, 3, "6", "seed is no key");
    refused(HEADER "# generator 3\n" HASHES, 3, "6", "generator a second time");
    refused("# t\n# generator x\n", 3, "2", "generator x is not a whole number");
    refused("# t\n# generator 3\n# args a\n# items 2 1\n" HASHES, 3, "5", "a hash before the header's inputs line");
    refused("# t\n# generator 3\n# args a\n# items \n", 3, "4", "items with no count");
    refused("# t\n# generator 3\n# args a\n# items 1 2 3 4 5\n", 3, "4", "items of more than 4 parts");
    refused("# t\n# generator 3\n# args a\n# items 2 -1\n", 3, "4", "items -1: a count of at most");
    refused("# t\n# inputs 00ff\n", 3, "2", "inputs 00ff is not 16 lowercase hexadecimal digits");
    refused("# t\n#generator 3\n", 3, "2", "is not \"# key value\"");
    refused(HEADER "0123456789abcde\n", 3, "6", "is not a hash of 16 lowercase hexadecimal digits");
    refused(HEADER "0123456789ABCDEF\n", 3, "6", "is not a hash of 16 lowercase hexadecimal digits");
    refused(HEADER "0123456789abcdef0\n", 3, "6", "is not a hash of 16 lowercase hexadecimal digits");
    refused(HEADER "0123456789abcdef\n\n", 3, "7", "\"\" is not a hash");
    refused(HEADER "0123456789abcdef\nfedcba9876543210\n", 3, "7", "2 hashes, where the items line counts 3");
    refused(HEADER HASHES "0000000000000002\n", 3, "9", "a hash past the 3 the items line counts");
    refused(HEADER "0123456789abcdef\n# recorded x\n", 3, "7", "after the hashes: the header comes first");
    refused(HEADER "0123456789abcdef\nfedcba9876543210\n0000000000000001", 3, "8", "or none at its end");
    refused(HEADER, 3, "5", "no hashes");

    /* the args' words, and the hash's form */
    char args[] = "--profile  a9 --seed 0x5 ", *argv[8];
    int n = snx_frozen_split(args, argv, 8);
    check(n == 4 && !strcmp(argv[0], "--profile") && !strcmp(argv[1], "a9") && !strcmp(argv[3], "0x5"),
          "the args split at their spaces");
    static const unsigned char bytes[9] = {1, 0, 0, 0, 0, 0, 0, 0, 2};
    check(snx_frozen_bytes(7, bytes, 9) == snx_frozen_mix(snx_frozen_mix(snx_frozen_mix(7, 1), 2), 9),
          "bytes are little-endian words, the last ones and the count after");
    check(snx_frozen_bytes(7, bytes, 8) != snx_frozen_bytes(7, bytes, 9), "the count of bytes counts");
    for (uint64_t v = 0; v < 4096; ++v)
        if (snx_frozen_mix(5, v) == snx_frozen_mix(5, v + 1)) {
            check(0, "two values mixed into one hash");
            break;
        }
    printf("%zu failures\n", g_failures);
    return g_failures != 0;
}
