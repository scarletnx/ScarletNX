/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* spv_opt_dead_code (common/utilities/spirv_opt.h) on small modules written here word by word: what it takes out,
   what it must leave, and that what is left is the rest of the module, in its order. Each module is only what the
   pass reads of one (the header, declarations, a function); spirv-val sees the pass's work on whole programs where
   the runtime translates them. */
#include "common/utilities/spirv_opt.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum {
    NAME = 5,
    EXT_INST_IMPORT = 11,
    EXT_INST = 12,
    TYPE_VOID = 19,
    FUNCTION = 54,
    FUNCTION_END = 56,
    VARIABLE = 59,
    LOAD = 61,
    STORE = 62,
    DECORATE = 71,
    VECTOR_SHUFFLE = 79,
    COMPOSITE_EXTRACT = 81,
    COMPOSITE_INSERT = 82,
    IMAGE_FETCH = 95,
    IMAGE_WRITE = 99,
    I_ADD = 128,
    F_MUL = 133,
    SELECT = 169,
    ATOMIC_I_ADD = 234,
    PHI = 245,
    LOOP_MERGE = 246,
    SELECTION_MERGE = 247,
    LABEL = 248,
    BRANCH = 249,
    BRANCH_CONDITIONAL = 250,
    SWITCH = 251,
    RETURN = 253,
    DECORATE_ID = 332,
    GROUP_NON_UNIFORM_BALLOT = 339, /* an opcode the pass has no layout for */
    DECORATE_STRING = 5632,
    FUNCTION_STORAGE = 7,
    NO_CONTRACTION = 42,
    /* ids: the declarations', which the pass never takes out, then the function's */
    T = 1,     /* a type */
    GLSL = 2,  /* the GLSL.std.450 import */
    OTHER = 3, /* another extended set's import */
    OUT = 4,   /* an Output variable */
    X = 5,     /* a value from outside the function: a constant */
    IMAGE = 6,
    F = 7,
    L0 = 8,
    L1 = 9,
    L2 = 10,
    Y = 11,  /* a second outside value, which nothing but an instruction with an effect reads */
    V0 = 35, /* Function variables, numbered as GLSL.std.450's Modf (35) and Fma (50) are */
    V1 = 50,
    A = 60, /* the function's values */
    B = 61,
    C = 62,
    D = 63,
    BOUND = 100
};

typedef struct {
    uint32_t words[256];
    size_t count;
} Module;
static int g_failures;

static void check(int pass, const char* what) {
    if (pass) return;
    fprintf(stderr, "FAIL: %s\n", what);
    g_failures++;
}
/* an instruction of opcode op and its n operand words */
static void put(Module* m, uint32_t op, int n, ...) {
    va_list operands;
    m->words[m->count++] = (uint32_t)(n + 1) << 16 | op;
    va_start(operands, n);
    for (int i = 0; i < n; i++) m->words[m->count++] = va_arg(operands, uint32_t);
    va_end(operands);
}
/* the header and the declarations every module here has, up to its function's first label */
static void begin(Module* m) {
    m->count = 0;
    m->words[m->count++] = 0x07230203;
    m->words[m->count++] = 0x00010600;
    m->words[m->count++] = 0;
    m->words[m->count++] = BOUND;
    m->words[m->count++] = 0;
    put(m, EXT_INST_IMPORT, 5, (uint32_t)GLSL, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0u); /* "GLSL.std.450" */
    put(m, EXT_INST_IMPORT, 5, (uint32_t)OTHER, 0x4c534c47u, 0x6474732eu, 0x3135342eu, 0u); /* "GLSL.std.451" */
    put(m, TYPE_VOID, 1, (uint32_t)T);
    put(m, VARIABLE, 3, (uint32_t)T, (uint32_t)OUT, 3u);
}
static void function(Module* m) {
    put(m, FUNCTION, 4, (uint32_t)T, (uint32_t)F, 0u, (uint32_t)T);
    put(m, LABEL, 1, (uint32_t)L0);
}
static void end(Module* m) {
    put(m, RETURN, 0);
    put(m, FUNCTION_END, 0);
}
/* how many instructions of opcode op m has, and with result the one whose third word (a result, after a type) is it */
static int count(const Module* m, size_t words, uint32_t op, uint32_t result) {
    int found = 0;
    for (size_t i = 5; i < words; i += m->words[i] >> 16)
        if ((m->words[i] & 0xffff) == op && (!result || m->words[i + 2] == result)) found++;
    return found;
}
/* the pass over a copy of m: what it leaves in *out, and its word count */
static size_t run(const Module* m, Module* out) {
    *out = *m;
    return spv_opt_dead_code(out->words, out->count);
}

int main(void) {
    Module m, o;
    size_t left;

    /* a chain nothing reads goes whole, and a value an Output's store reads stays */
    begin(&m);
    put(&m, NAME, 2, (uint32_t)B, 0x00000062u);
    put(&m, DECORATE, 2, (uint32_t)B, (uint32_t)NO_CONTRACTION);
    put(&m, DECORATE_ID, 3, (uint32_t)B, 44u, (uint32_t)X);
    put(&m, DECORATE_STRING, 3, (uint32_t)B, 5635u, 0x00000062u);
    put(&m, DECORATE, 2, (uint32_t)C, (uint32_t)NO_CONTRACTION);
    function(&m);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)A, (uint32_t)X, (uint32_t)X);
    put(&m, F_MUL, 4, (uint32_t)T, (uint32_t)B, (uint32_t)A, (uint32_t)A);
    put(&m, F_MUL, 4, (uint32_t)T, (uint32_t)C, (uint32_t)X, (uint32_t)X);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)C);
    end(&m);
    left = run(&m, &o);
    check(left == m.count - 5 - 5 - 3 - 3 - 4 - 4,
          "a product nothing reads and the sum only it read are taken out, with the product's name and decorations");
    check(!count(&o, left, I_ADD, 0) && count(&o, left, F_MUL, C) == 1 && count(&o, left, F_MUL, 0) == 1,
          "the value an Output's store reads stays");
    check(!count(&o, left, NAME, 0) && count(&o, left, DECORATE, 0) == 1 && !count(&o, left, DECORATE_ID, 0) &&
              !count(&o, left, DECORATE_STRING, 0),
          "the name and the decorations of what went go, and a decoration of what stays stays");
    check(count(&o, left, STORE, 0) == 1 && count(&o, left, RETURN, 0) == 1 && count(&o, left, FUNCTION_END, 0) == 1,
          "the store, the return and the function's end stay");
    check(o.words[3] == BOUND && !memcmp(o.words, m.words, 5 * sizeof *m.words), "the header stays as it was");
    size_t again = spv_opt_dead_code(o.words, left);
    check(again == left, "a second pass finds nothing");

    /* nothing dead: the module comes back word for word */
    begin(&m);
    function(&m);
    put(&m, VARIABLE, 3, (uint32_t)T, (uint32_t)V0, (uint32_t)FUNCTION_STORAGE);
    put(&m, STORE, 2, (uint32_t)V0, (uint32_t)X);
    put(&m, LOAD, 3, (uint32_t)T, (uint32_t)A, (uint32_t)V0);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)A);
    end(&m);
    left = run(&m, &o);
    check(left == m.count && !memcmp(o.words, m.words, m.count * sizeof *m.words),
          "a variable that is read, its store and its load stay: the module is as it was");

    /* a Function variable that is only stored to goes with its stores and with what was made for them; its number
       as a literal (GLSL.std.450's Fma is 50, as V1) keeps nothing */
    begin(&m);
    function(&m);
    put(&m, VARIABLE, 3, (uint32_t)T, (uint32_t)V1, (uint32_t)FUNCTION_STORAGE);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)A, (uint32_t)X, (uint32_t)X);
    put(&m, STORE, 2, (uint32_t)V1, (uint32_t)A);
    put(&m, EXT_INST, 7, (uint32_t)T, (uint32_t)B, (uint32_t)GLSL, 50u, (uint32_t)X, (uint32_t)X, (uint32_t)X);
    put(&m, STORE, 2, (uint32_t)V1, (uint32_t)B);
    put(&m, EXT_INST, 7, (uint32_t)T, (uint32_t)C, (uint32_t)GLSL, 50u, (uint32_t)X, (uint32_t)X, (uint32_t)X);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)C);
    end(&m);
    left = run(&m, &o);
    check(!count(&o, left, VARIABLE, V1) && count(&o, left, STORE, 0) == 1 && !count(&o, left, I_ADD, 0) &&
              count(&o, left, EXT_INST, 0) == 1 && count(&o, left, EXT_INST, C) == 1,
          "a variable only stored to goes with its stores and their values, whatever literal has its number");
    check(left == m.count - 4 - 5 - 3 - 8 - 3, "23 words of the stored variable are taken out");

    /* a variable whose only read goes: the store after that read in the words goes in a later sweep */
    begin(&m);
    function(&m);
    put(&m, VARIABLE, 3, (uint32_t)T, (uint32_t)V0, (uint32_t)FUNCTION_STORAGE);
    put(&m, LOAD, 3, (uint32_t)T, (uint32_t)A, (uint32_t)V0);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)B, (uint32_t)A, (uint32_t)X);
    put(&m, STORE, 2, (uint32_t)V0, (uint32_t)X);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)X);
    end(&m);
    left = run(&m, &o);
    check(!count(&o, left, VARIABLE, V0) && !count(&o, left, LOAD, 0) && !count(&o, left, I_ADD, 0) &&
              count(&o, left, STORE, 0) == 1,
          "a variable whose only read went goes, with the store that follows the read");

    /* a variable with an initializer and no store or load goes, and what the initializer was does not matter */
    begin(&m);
    function(&m);
    put(&m, VARIABLE, 4, (uint32_t)T, (uint32_t)V0, (uint32_t)FUNCTION_STORAGE, (uint32_t)X);
    end(&m);
    left = run(&m, &o);
    check(left == m.count - 5, "a variable nothing stores to or reads goes with its initializer");

    /* what has an effect stays with its result unread: an atomic, an image write, GLSL.std.450's Modf and Frexp
       (they write through their pointer), another set's instruction, a volatile load, an opcode the pass does not
       know; and each keeps what it reads */
    begin(&m);
    function(&m);
    put(&m, VARIABLE, 3, (uint32_t)T, (uint32_t)V0, (uint32_t)FUNCTION_STORAGE);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)A, (uint32_t)X, (uint32_t)X);
    put(&m, ATOMIC_I_ADD, 6, (uint32_t)T, (uint32_t)B, (uint32_t)OUT, (uint32_t)X, (uint32_t)X, (uint32_t)A);
    put(&m, IMAGE_WRITE, 3, (uint32_t)IMAGE, (uint32_t)Y, (uint32_t)A);
    put(&m, EXT_INST, 6, (uint32_t)T, (uint32_t)C, (uint32_t)GLSL, 35u, (uint32_t)X, (uint32_t)V0);
    put(&m, EXT_INST, 6, (uint32_t)T, (uint32_t)(D + 3), (uint32_t)GLSL, 51u, (uint32_t)X, (uint32_t)V0);
    put(&m, EXT_INST, 5, (uint32_t)T, (uint32_t)D, (uint32_t)OTHER, 1u, (uint32_t)A);
    put(&m, LOAD, 4, (uint32_t)T, (uint32_t)(D + 1), (uint32_t)OUT, 1u);
    put(&m, GROUP_NON_UNIFORM_BALLOT, 4, (uint32_t)T, (uint32_t)(D + 2), (uint32_t)X, (uint32_t)A);
    end(&m);
    left = run(&m, &o);
    check(left == m.count && !memcmp(o.words, m.words, m.count * sizeof *m.words),
          "instructions with an effect stay with their results unread, and so does what they read");

    /* the same reads with no effect go: a load, an image fetch with an operands mask, a select, a phi */
    begin(&m);
    function(&m);
    put(&m, LOAD, 3, (uint32_t)T, (uint32_t)A, (uint32_t)OUT);
    put(&m, IMAGE_FETCH, 6, (uint32_t)T, (uint32_t)B, (uint32_t)IMAGE, (uint32_t)X, 2u, (uint32_t)X);
    put(&m, SELECT, 5, (uint32_t)T, (uint32_t)C, (uint32_t)X, (uint32_t)A, (uint32_t)B);
    put(&m, COMPOSITE_EXTRACT, 5, (uint32_t)T, (uint32_t)D, (uint32_t)C, (uint32_t)A, (uint32_t)A);
    put(&m, PHI, 4, (uint32_t)T, (uint32_t)(D + 1), (uint32_t)D, (uint32_t)L0);
    end(&m);
    left = run(&m, &o);
    check(left == m.count - 4 - 7 - 6 - 6 - 5,
          "a load, a fetch, a select, an extract and a phi nothing reads go; an extract's indices are no use of an id");

    /* a branch reads its condition, a switch its selector; their labels and literals read nothing */
    begin(&m);
    function(&m);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)A, (uint32_t)X, (uint32_t)X);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)B, (uint32_t)X, (uint32_t)X);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)C, (uint32_t)X, (uint32_t)X);
    put(&m, BRANCH_CONDITIONAL, 3, (uint32_t)A, (uint32_t)L1, (uint32_t)L2);
    put(&m, LABEL, 1, (uint32_t)L1);
    put(&m, SWITCH, 4, (uint32_t)B, (uint32_t)L2, (uint32_t)C, (uint32_t)L2);
    put(&m, LABEL, 1, (uint32_t)L2);
    put(&m, BRANCH, 1, (uint32_t)L2);
    put(&m, FUNCTION_END, 0);
    left = run(&m, &o);
    check(left == m.count - 5 && count(&o, left, I_ADD, A) == 1 && count(&o, left, I_ADD, B) == 1 &&
              !count(&o, left, I_ADD, C),
          "a branch's condition and a switch's selector stay; a value a switch's literal numbers goes");

    /* a literal names no id: A, of the number each literal below has, goes from under instructions that stay (an
       index, a memory operand, a shuffle's component, a merge's control, a branch's weight, a function's control) */
    begin(&m);
    put(&m, FUNCTION, 4, (uint32_t)T, (uint32_t)F, (uint32_t)A, (uint32_t)T);
    put(&m, LABEL, 1, (uint32_t)L0);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)A, (uint32_t)X, (uint32_t)X);
    put(&m, COMPOSITE_EXTRACT, 4, (uint32_t)T, (uint32_t)B, (uint32_t)X, (uint32_t)A);
    put(&m, STORE, 3, (uint32_t)OUT, (uint32_t)B, (uint32_t)A);
    put(&m, LOAD, 4, (uint32_t)T, (uint32_t)C, (uint32_t)OUT, (uint32_t)A);
    put(&m, VECTOR_SHUFFLE, 5, (uint32_t)T, (uint32_t)D, (uint32_t)C, (uint32_t)C, (uint32_t)A);
    put(&m, COMPOSITE_INSERT, 5, (uint32_t)T, (uint32_t)(D + 1), (uint32_t)D, (uint32_t)D, (uint32_t)A);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)(D + 1));
    put(&m, LOOP_MERGE, 3, (uint32_t)L1, (uint32_t)L2, (uint32_t)A);
    put(&m, BRANCH, 1, (uint32_t)L1);
    put(&m, LABEL, 1, (uint32_t)L1);
    put(&m, SELECTION_MERGE, 2, (uint32_t)L2, (uint32_t)A);
    put(&m, BRANCH_CONDITIONAL, 5, (uint32_t)X, (uint32_t)L2, (uint32_t)L2, (uint32_t)A, (uint32_t)A);
    put(&m, LABEL, 1, (uint32_t)L2);
    end(&m);
    left = run(&m, &o);
    check(left == m.count - 5 && !count(&o, left, I_ADD, 0),
          "a value goes whose number is only an index, a memory operand, a component, a control or a weight");
    /* the same under each image instruction's operands mask, with the ids on both sides of the mask read */
    static const uint32_t masked[] = {87, 88, 91, 92, 95, 98, 89, 90, 93, 94, 96, 97, IMAGE_WRITE};
    for (size_t k = 0; k < sizeof masked / sizeof *masked; k++) {
        int before = k < 6 ? 2 : 3; /* the operands before the mask: an image and a coordinate, and a third */
        begin(&m);
        function(&m);
        put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)A, (uint32_t)X, (uint32_t)X);
        put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)C, (uint32_t)X, (uint32_t)X);
        put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)D, (uint32_t)X, (uint32_t)X);
        if (masked[k] == IMAGE_WRITE)
            put(&m, IMAGE_WRITE, 5, (uint32_t)IMAGE, (uint32_t)X, (uint32_t)C, (uint32_t)A, (uint32_t)D);
        else if (before == 2)
            put(&m, masked[k], 6, (uint32_t)T, (uint32_t)B, (uint32_t)IMAGE, (uint32_t)C, (uint32_t)A, (uint32_t)D);
        else
            put(&m, masked[k], 7, (uint32_t)T, (uint32_t)B, (uint32_t)IMAGE, (uint32_t)X, (uint32_t)C, (uint32_t)A,
                (uint32_t)D);
        put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)B);
        end(&m);
        left = run(&m, &o);
        if (left != m.count - 5 || count(&o, left, I_ADD, 0) != 2)
            fprintf(stderr, "image instruction %u: %zu words left of %zu\n", masked[k], left, m.count);
        check(left == m.count - 5 && count(&o, left, I_ADD, C) == 1 && count(&o, left, I_ADD, D) == 1,
              "an image instruction reads the ids before and after its operands mask, and the mask names none");
    }

    /* a module of declarations alone, and less than a header, come back as they are */
    begin(&m);
    left = run(&m, &o);
    check(left == m.count, "a module with no function is left as it is");
    uint32_t part[3] = {0x07230203, 0x00010600, 0};
    check(spv_opt_dead_code(part, 3) == 3, "less than a header is left as it is");
    printf("spirv opt: %d checks failed\n", g_failures);
    return g_failures != 0;
}
