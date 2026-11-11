/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The native nn::ngc (src/nx/nn/ngc.c) on a stand-in CPU: the SDK's checks of Initialize, Mask and Check in their
   order, with their Results, the object as Initialize leaves it, a text that comes back as it was with none counted,
   the encodings refused, and Finalize */
#include "nx/nn/ngc.c"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

static CPU cpu;
static u8 mem[0x1000];
#define SELF  0x100u /* the object */
#define TEXT  0x200u
#define COUNT 0x300u
#define OUT   0x320u
#define WORDS 0x380u

int main(void) {
    CPU* c = &cpu;
    cpu.mem = mem;
    /* before Initialize, and its checks in order */
    strcpy((char*)mem + TEXT, "hello");
    CHECK(nnngc_mask(c, SELF, COUNT, TEXT, 0) == NGC_NOT_INITIALIZED);
    CHECK(nnngc_check(c, SELF, OUT, 0, WORDS, 1) == NGC_NOT_INITIALIZED);
    CHECK(nnngc_initialize(c, SELF, 0, 0, 1) == NGC_NULL);
    CHECK(nnngc_initialize(c, SELF, 0x10000, 0x1ffff, 1) == NGC_SIZE);
    memset(mem + SELF, 0xee, 0x30);
    mem[SELF + OBJ_INITIALIZED] = 0;
    CHECK(nnngc_initialize(c, SELF, 0x10000, 0x20000, 3) == 0);
    CHECK(mem[SELF + OBJ_LANGUAGE] == 1 && mem[SELF + OBJ_INITIALIZED] == 1);
    CHECK(snx_ld_le32(mem + SELF + 0xc) == 0 && snx_ld_le32(mem + SELF + 0x14) == 0);
    CHECK(snx_ld_le64(mem + SELF + OBJ_WORK) == 0x10000 && snx_ld_le64(mem + SELF + OBJ_WORK_SIZE) == 0x20000);
    CHECK(nnngc_initialize(c, SELF, 0x10000, 0x20000, 1) == NGC_ALREADY_INITIALIZED);
    CHECK(nnngc_get_content_version(c, SELF) == 1);

    /* a text: unchanged, none counted; no count asked for; a null text; bad UTF-8 */
    snx_st_le32(mem + COUNT, 77);
    CHECK(nnngc_mask(c, SELF, COUNT, TEXT, 0) == 0 && snx_ld_le32(mem + COUNT) == 0);
    CHECK(!strcmp((char*)mem + TEXT, "hello"));
    CHECK(nnngc_mask(c, SELF, 0, TEXT, 0) == 0);
    CHECK(nnngc_mask(c, SELF, COUNT, 0, 0) == NGC_NULL);
    strcpy((char*)mem + TEXT, "caf\xc3\xa9 \xe3\x81\x82 \xf0\x9f\x98\x80");
    CHECK(nnngc_mask(c, SELF, COUNT, TEXT, 0) == 0);
    strcpy((char*)mem + TEXT, "bad \xc3(");
    CHECK(nnngc_mask(c, SELF, COUNT, TEXT, 0) == NGC_ENCODING);
    strcpy((char*)mem + TEXT, "bad \xff");
    CHECK(nnngc_mask(c, SELF, COUNT, TEXT, 0) == NGC_ENCODING);
    /* UTF-16: a pair is fine, a low surrogate alone is not */
    const u16 good[] = {'h', 0xd83d, 0xde00, 0}, bad[] = {'h', 0xde00, 0};
    for (int i = 0; i < 4; i++) snx_st_le16(mem + TEXT + 2 * i, good[i]);
    CHECK(nnngc_mask16(c, SELF, COUNT, TEXT, 0) == 0);
    for (int i = 0; i < 3; i++) snx_st_le16(mem + TEXT + 2 * i, bad[i]);
    CHECK(nnngc_mask16(c, SELF, COUNT, TEXT, 0) == NGC_ENCODING);

    /* words: a 0 for each, at most 16, the outputs and the list not null */
    strcpy((char*)mem + TEXT, "word");
    for (int i = 0; i < 3; i++) snx_st_le64(mem + WORDS + 8 * i, TEXT);
    memset(mem + OUT, 0xee, 16);
    CHECK(nnngc_check(c, SELF, OUT, 0, WORDS, 3) == 0);
    CHECK(snx_ld_le32(mem + OUT) == 0 && snx_ld_le32(mem + OUT + 8) == 0 && mem[OUT + 12] == 0xee);
    CHECK(nnngc_check(c, SELF, 0, 0, WORDS, 3) == NGC_NULL && nnngc_check(c, SELF, OUT, 0, 0, 3) == NGC_NULL);
    CHECK(nnngc_check(c, SELF, OUT, 0, WORDS, 17) == NGC_SIZE);

    /* Finalize, and the destructor of an object no longer initialized */
    nnngc_finalize(c, SELF);
    CHECK(mem[SELF + OBJ_INITIALIZED] == 0 && nnngc_mask(c, SELF, COUNT, TEXT, 0) == NGC_NOT_INITIALIZED);
    nnngc_finalize(c, SELF);
    printf("nn_ngc: %u checks passed\n", checks);
    return 0;
}
