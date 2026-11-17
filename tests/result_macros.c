/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The R_ result helpers (nx/hle/result.h), built as strict ISO C23: what each returns, that each argument is evaluated
   once, and that the statement macros stay one statement under an if without braces */
#include "nx/hle/result.h"
#include <assert.h>
#include <stdio.h>

static int calls;

static unsigned int counted(unsigned int rc) {
    calls++;
    return rc;
}

static int succeed(void) {
    R_SUCCEED();
}
static int throw_rc(unsigned int rc) {
    R_THROW(counted(rc));
}
static int unless(int ok, unsigned int rc) {
    R_UNLESS(counted((unsigned int)ok) != 0, counted(rc));
    return 7;
}
static int try_rc(unsigned int rc) {
    R_TRY(counted(rc));
    return 7;
}
/* two R_TRYs in one function and one nested in an else: the temporary is scoped to its own block */
static int try_twice(unsigned int first, unsigned int second, int skip) {
    int snx_unrelated = 3;
    R_TRY(counted(first));
    if (skip)
        snx_unrelated = 5;
    else
        R_TRY(counted(second));
    return snx_unrelated;
}
/* an unsigned function, as the kernel's svc_ functions are */
static unsigned int try_unsigned(unsigned int rc) {
    R_TRY(rc);
    R_UNLESS(rc == 0, 1u);
    R_SUCCEED();
}
/* the if/else binding: each macro must leave the else to its own if */
static int branch(int a, int b) {
    if (a)
        R_UNLESS(b, RC(1, 2));
    else
        R_THROW(RC(3, 4));
    R_SUCCEED();
}

int main(void) {
    assert(RC(1, 123) == 0xF601);
    assert(RC(1, 33) == 0x4201);
    assert(R_SUCCEEDED(RC_OK) && !R_FAILED(RC_OK));
    assert(R_FAILED(RC(2, 1)) && !R_SUCCEEDED(RC(2, 1)));

    assert(succeed() == 0);

    calls = 0;
    assert(throw_rc(0xEA01) == 0xEA01 && calls == 1);

    calls = 0;
    assert(unless(1, 0xE401) == 7 && calls == 1); /* rc isn't evaluated when cond holds */
    calls = 0;
    assert(unless(0, 0xE401) == 0xE401 && calls == 2);

    calls = 0;
    assert(try_rc(0) == 7 && calls == 1);
    calls = 0;
    assert(try_rc(0xCC01) == 0xCC01 && calls == 1);

    calls = 0;
    assert(try_twice(0, 0, 0) == 3 && calls == 2);
    calls = 0;
    assert(try_twice(0xD001, 0, 0) == 0xD001 && calls == 1);
    calls = 0;
    assert(try_twice(0, 0x10801, 0) == 0x10801 && calls == 2);
    calls = 0;
    assert(try_twice(0, 0x10801, 1) == 5 && calls == 1);

    assert(try_unsigned(0) == 0 && try_unsigned(0x20801) == 0x20801);

    assert(branch(1, 1) == 0);
    assert(branch(1, 0) == RC(1, 2));
    assert(branch(0, 1) == RC(3, 4));

    puts("result_macros: ok");
    return 0;
}
