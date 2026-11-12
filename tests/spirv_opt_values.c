/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* spv_opt_values (common/utilities/spirv_opt.h) on small modules written here word by word: which instruction goes
   for an equal one, what then reads the one that stays, and what must not pair. Each module is only what the pass
   reads of one (the header, declarations, functions); the types are opaque, so nothing here is folded. spirv-val
   sees the pass's work on whole programs where the runtime translates them. */
#include "common/utilities/spirv_opt.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum {
    NAME = 5,
    LINE = 8,
    EXT_INST_IMPORT = 11,
    EXT_INST = 12,
    TYPE_VOID = 19,
    TYPE_INT = 21,
    FUNCTION = 54,
    FUNCTION_END = 56,
    FUNCTION_CALL = 57,
    VARIABLE = 59,
    IMAGE_TEXEL_POINTER = 60,
    LOAD = 61,
    STORE = 62,
    COPY_MEMORY = 63,
    ACCESS_CHAIN = 65,
    IN_BOUNDS_ACCESS_CHAIN = 66,
    ARRAY_LENGTH = 68,
    DECORATE = 71,
    GROUP_DECORATE = 74,
    GROUP_MEMBER_DECORATE = 75,
    VECTOR_SHUFFLE = 79,
    COMPOSITE_EXTRACT = 81,
    COMPOSITE_INSERT = 82,
    COPY_OBJECT = 83,
    SAMPLED_IMAGE = 86,
    IMAGE_SAMPLE_IMPLICIT_LOD = 87,
    IMAGE_SAMPLE_EXPLICIT_LOD = 88,
    IMAGE_READ = 98,
    IMAGE_WRITE = 99,
    UNDEF = 1,
    I_ADD = 128,
    F_ADD = 129,
    I_SUB = 130,
    F_MUL = 133,
    SELECT = 169,
    CONTROL_BARRIER = 224,
    ATOMIC_I_ADD = 234,
    PHI = 245,
    LOOP_MERGE = 246,
    SELECTION_MERGE = 247,
    LABEL = 248,
    BRANCH = 249,
    BRANCH_CONDITIONAL = 250,
    SWITCH = 251,
    RETURN = 253,
    RETURN_VALUE = 254,
    DECORATE_ID = 332,
    GROUP_NON_UNIFORM_BALLOT = 339, /* an opcode the pass has no layout for */
    INPUT = 1,
    UNIFORM = 2,
    OUTPUT = 3,
    PRIVATE = 6,
    FUNCTION_STORAGE = 7,
    PUSH_CONSTANT = 9,
    STORAGE_BUFFER = 12,
    BUFFER_BLOCK = 3,
    NO_CONTRACTION = 42,
    /* ids: the declarations', then the functions' */
    T = 1,     /* a type */
    GLSL = 2,  /* the GLSL.std.450 import */
    OTHER = 3, /* another extended set's import */
    OUT = 4,   /* an Output variable */
    X = 5,     /* values from outside the functions: constants */
    IMAGE = 6, /* a UniformConstant variable */
    F = 7,
    L0 = 8,
    L1 = 9,
    L2 = 10,
    L3 = 11,
    Y = 12,
    IN = 13,   /* an Input variable */
    UBO = 14,  /* a Uniform variable */
    PUSH = 15, /* a PushConstant variable */
    SSBO = 16, /* a StorageBuffer variable */
    T2 = 17,   /* another type */
    Z = 18,
    F2 = 19,
    WIDE = 20, /* a 64-bit integer type, in the modules that declare it */
    V0 = 35,   /* a Function variable */
    A = 60,    /* the functions' values */
    B = 61,
    C = 62,
    D = 63,
    E = 64,
    G = 65,
    H = 66,
    I = 67,
    BOUND = 100
};

typedef struct {
    uint32_t words[1024];
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
/* the header and the declarations every module here has; a module's own decorations follow, then function() */
static void begin(Module* m) {
    m->count = 0;
    m->words[m->count++] = 0x07230203;
    m->words[m->count++] = 0x00010600;
    m->words[m->count++] = 0;
    m->words[m->count++] = BOUND;
    m->words[m->count++] = 0;
    put(m, EXT_INST_IMPORT, 5, (uint32_t)GLSL, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0u);  /* "GLSL.std.450" */
    put(m, EXT_INST_IMPORT, 5, (uint32_t)OTHER, 0x4c534c47u, 0x6474732eu, 0x3135342eu, 0u); /* "GLSL.std.451" */
    put(m, TYPE_VOID, 1, (uint32_t)T);
    put(m, TYPE_VOID, 1, (uint32_t)T2);
}
static void function(Module* m) {
    put(m, VARIABLE, 3, (uint32_t)T, (uint32_t)OUT, (uint32_t)OUTPUT);
    put(m, VARIABLE, 3, (uint32_t)T, (uint32_t)IMAGE, 0u);
    put(m, VARIABLE, 3, (uint32_t)T, (uint32_t)IN, (uint32_t)INPUT);
    put(m, VARIABLE, 3, (uint32_t)T, (uint32_t)UBO, (uint32_t)UNIFORM);
    put(m, VARIABLE, 3, (uint32_t)T, (uint32_t)PUSH, (uint32_t)PUSH_CONSTANT);
    put(m, VARIABLE, 3, (uint32_t)T, (uint32_t)SSBO, (uint32_t)STORAGE_BUFFER);
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
/* the nth instruction of opcode op in m (from 0), or a row of zeros */
static const uint32_t* find(const Module* m, size_t words, uint32_t op, int nth) {
    static const uint32_t none[8];
    for (size_t i = 5; i < words; i += m->words[i] >> 16)
        if ((m->words[i] & 0xffff) == op && !nth--) return m->words + i;
    return none;
}
/* the pass over a copy of m: what it leaves in *out, and its word count */
static size_t run(const Module* m, Module* out) {
    *out = *m;
    return spv_opt_values(out->words, out->count);
}
/* whether the pass left m as it was */
static int untouched(const Module* m) {
    Module o;
    size_t left = run(m, &o);
    return left == m->count && !memcmp(o.words, m->words, m->count * sizeof *m->words);
}
/* a function of one block: first, then between when it is an opcode (its operands are X), then second; whether
   second went and the store reads first's result */
static int pairs(const uint32_t* first, const uint32_t* second, uint32_t between) {
    Module m, o;
    begin(&m);
    function(&m);
    memcpy(m.words + m.count, first, (first[0] >> 16) * sizeof *first);
    m.count += first[0] >> 16;
    if (between) put(&m, between, 3, (uint32_t)X, (uint32_t)X, (uint32_t)X);
    memcpy(m.words + m.count, second, (second[0] >> 16) * sizeof *second);
    m.count += second[0] >> 16;
    put(&m, STORE, 2, (uint32_t)OUT, second[2]);
    end(&m);
    size_t left = run(&m, &o);
    if (left == m.count && !memcmp(o.words, m.words, m.count * sizeof *m.words)) return 0;
    return left == m.count - (second[0] >> 16) && count(&o, left, second[0] & 0xffff, second[2]) == 0 &&
                   find(&o, left, STORE, 0)[2] == first[2]
               ? 1
               : -1;
}
/* two instructions of opcode op, results A and B, with the n operand words given: pairs() of them */
static int twice(uint32_t between, uint32_t op, int n, ...) {
    uint32_t a[12], b[12];
    va_list operands;
    a[0] = b[0] = (uint32_t)(n + 3) << 16 | op;
    a[1] = b[1] = T;
    a[2] = A;
    b[2] = B;
    va_start(operands, n);
    for (int i = 0; i < n; i++) a[3 + i] = b[3 + i] = va_arg(operands, uint32_t);
    va_end(operands);
    return pairs(a, b, between);
}

int main(void) {
    Module m, o;
    size_t left;

    /* in one block: the second of two equal sums goes and the store reads the first */
    begin(&m);
    put(&m, NAME, 2, (uint32_t)A, 0x00000061u);
    put(&m, NAME, 2, (uint32_t)B, 0x00000062u);
    function(&m);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)A, (uint32_t)X, (uint32_t)Y);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)B, (uint32_t)X, (uint32_t)Y);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)B);
    end(&m);
    left = run(&m, &o);
    check(left == m.count - 5 - 3 && count(&o, left, I_ADD, A) == 1 && count(&o, left, I_ADD, 0) == 1,
          "the second of two equal sums is taken out, with its name");
    check(find(&o, left, STORE, 0)[1] == OUT && find(&o, left, STORE, 0)[2] == A, "the store reads the first sum");
    check(count(&o, left, NAME, 0) == 1 && find(&o, left, NAME, 0)[1] == A, "the first sum's name stays");
    check(o.words[3] == BOUND && !memcmp(o.words, m.words, 5 * sizeof *m.words), "the header stays as it was");
    check(count(&o, left, RETURN, 0) == 1 && count(&o, left, FUNCTION_END, 0) == 1 && count(&o, left, VARIABLE, 0) == 6,
          "the rest of the module is there");
    check(spv_opt_values(o.words, left) == left, "a second pass finds nothing");

    /* what differs in its opcode, its type, an operand or a literal is not equal */
    begin(&m);
    function(&m);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)A, (uint32_t)X, (uint32_t)Y);
    put(&m, I_ADD, 4, (uint32_t)T2, (uint32_t)B, (uint32_t)X, (uint32_t)Y);
    put(&m, I_SUB, 4, (uint32_t)T, (uint32_t)C, (uint32_t)X, (uint32_t)Y);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)D, (uint32_t)X, (uint32_t)Z);
    put(&m, COMPOSITE_EXTRACT, 4, (uint32_t)T, (uint32_t)E, (uint32_t)X, 1u);
    put(&m, COMPOSITE_EXTRACT, 4, (uint32_t)T, (uint32_t)G, (uint32_t)X, 2u);
    put(&m, COMPOSITE_EXTRACT, 5, (uint32_t)T, (uint32_t)H, (uint32_t)X, 1u, 0u);
    end(&m);
    check(untouched(&m), "another type, opcode, operand, literal or width: the module is as it was");
    check(twice(0, COMPOSITE_EXTRACT, 2, (uint32_t)X, 1u) == 1, "two extracts of one index are equal");

    /* the two operands of an integer or logical instruction that commutes, in either order; no other's */
    static const uint32_t either[] = {128, 132, 197, 198, 199, 170, 171, 164, 165, 166, 167};
    static const uint32_t ordered[] = {129, 130, 131, 133, 134, 163, 168, 172, 180, 194, 196};
    for (size_t k = 0; k < sizeof either / sizeof *either + sizeof ordered / sizeof *ordered; k++) {
        int commutes = k < sizeof either / sizeof *either;
        uint32_t op = commutes ? either[k] : ordered[k - sizeof either / sizeof *either];
        uint32_t a[5] = {5u << 16 | op, T, A, X, Y}, b[5] = {5u << 16 | op, T, B, Y, X};
        int got = pairs(a, b, 0);
        if (got != commutes) fprintf(stderr, "opcode %u with its operands swapped: %d\n", op, got);
        check(got == commutes,
              "operands in the other order are equal for the integer and logical opcodes that commute");
    }
    {
        uint32_t a[6] = {6u << 16 | SELECT, T, A, X, Y, Z}, b[6] = {6u << 16 | SELECT, T, B, Y, X, Z};
        check(pairs(a, b, 0) == 0, "three operands are never read in another order");
    }

    /* an equal instruction in a block that dominates: A in L0 reaches all; C in L1 reaches neither L2 nor L3 */
    begin(&m);
    function(&m);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)A, (uint32_t)X, (uint32_t)Y);
    put(&m, SELECTION_MERGE, 2, (uint32_t)L3, 0u);
    put(&m, BRANCH_CONDITIONAL, 3, (uint32_t)X, (uint32_t)L1, (uint32_t)L2);
    put(&m, LABEL, 1, (uint32_t)L1);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)B, (uint32_t)X, (uint32_t)Y);
    put(&m, F_MUL, 4, (uint32_t)T, (uint32_t)C, (uint32_t)X, (uint32_t)Y);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)B);
    put(&m, BRANCH, 1, (uint32_t)L3);
    put(&m, LABEL, 1, (uint32_t)L2);
    put(&m, F_MUL, 4, (uint32_t)T, (uint32_t)D, (uint32_t)X, (uint32_t)Y);
    put(&m, BRANCH, 1, (uint32_t)L3);
    put(&m, LABEL, 1, (uint32_t)L3);
    put(&m, F_MUL, 4, (uint32_t)T, (uint32_t)E, (uint32_t)X, (uint32_t)Y);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)G, (uint32_t)X, (uint32_t)Y);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)G);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)E);
    end(&m);
    left = run(&m, &o);
    check(left == m.count - 10 && count(&o, left, I_ADD, A) == 1 && count(&o, left, I_ADD, 0) == 1,
          "a sum equal to one in the block that dominates goes, from a branch's side and from after the branch");
    check(count(&o, left, F_MUL, C) == 1 && count(&o, left, F_MUL, D) == 1 && count(&o, left, F_MUL, E) == 1,
          "a product in one side of a branch replaces neither the other side's nor the one after them");
    check(find(&o, left, STORE, 0)[2] == A && find(&o, left, STORE, 1)[2] == A && find(&o, left, STORE, 2)[2] == E,
          "each store reads what is left");

    /* the same through a switch's cases, with a case's literal that numbers a label (L3 is 11): the block after
       the switch (L2) is dominated by L0 alone */
    begin(&m);
    function(&m);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)A, (uint32_t)X, (uint32_t)Y);
    put(&m, SELECTION_MERGE, 2, (uint32_t)L2, 0u);
    put(&m, SWITCH, 4, (uint32_t)X, (uint32_t)L2, (uint32_t)L3, (uint32_t)L1);
    put(&m, LABEL, 1, (uint32_t)L1);
    put(&m, F_MUL, 4, (uint32_t)T, (uint32_t)C, (uint32_t)X, (uint32_t)Y);
    put(&m, BRANCH, 1, (uint32_t)L2);
    put(&m, LABEL, 1, (uint32_t)L2);
    put(&m, F_MUL, 4, (uint32_t)T, (uint32_t)D, (uint32_t)X, (uint32_t)Y);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)B, (uint32_t)X, (uint32_t)Y);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)D);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)B);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)C);
    end(&m);
    left = run(&m, &o);
    check(left == m.count - 5 && count(&o, left, F_MUL, 0) == 2 && count(&o, left, I_ADD, 0) == 1,
          "a switch's case does not dominate what its default also reaches; the block before the switch does");
    /* a case alone reaches its block: what the switch's block computed is there, what the default's did is not */
    begin(&m);
    function(&m);
    put(&m, SELECTION_MERGE, 2, (uint32_t)L3, 0u);
    put(&m, SWITCH, 4, (uint32_t)X, (uint32_t)L1, 7u, (uint32_t)L2);
    put(&m, LABEL, 1, (uint32_t)L1);
    put(&m, F_MUL, 4, (uint32_t)T, (uint32_t)C, (uint32_t)X, (uint32_t)Y);
    put(&m, BRANCH, 1, (uint32_t)L3);
    put(&m, LABEL, 1, (uint32_t)L2);
    put(&m, F_MUL, 4, (uint32_t)T, (uint32_t)D, (uint32_t)X, (uint32_t)Y);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)D);
    put(&m, BRANCH, 1, (uint32_t)L3);
    put(&m, LABEL, 1, (uint32_t)L3);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)C);
    end(&m);
    check(untouched(&m), "a switch's default does not dominate its case");

    /* two ways into a loop (L0 to L1 and to L2, which branch to each other): neither of its blocks dominates the
       other, which takes a second walk of the blocks to see */
    begin(&m);
    function(&m);
    put(&m, BRANCH_CONDITIONAL, 3, (uint32_t)X, (uint32_t)L1, (uint32_t)L3);
    put(&m, LABEL, 1, (uint32_t)L1);
    put(&m, F_MUL, 4, (uint32_t)T, (uint32_t)A, (uint32_t)X, (uint32_t)Y);
    put(&m, BRANCH, 1, (uint32_t)L2);
    put(&m, LABEL, 1, (uint32_t)L3);
    put(&m, BRANCH, 1, (uint32_t)Z);
    put(&m, LABEL, 1, (uint32_t)L2);
    put(&m, F_MUL, 4, (uint32_t)T, (uint32_t)B, (uint32_t)X, (uint32_t)Y);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)B);
    put(&m, BRANCH, 1, (uint32_t)Z);
    put(&m, LABEL, 1, (uint32_t)Z);
    put(&m, F_MUL, 4, (uint32_t)T, (uint32_t)C, (uint32_t)X, (uint32_t)Y);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)C);
    put(&m, BRANCH, 1, (uint32_t)L2);
    put(&m, FUNCTION_END, 0);
    check(untouched(&m), "a block entered from two sides is dominated by neither");

    /* more instructions than the table's first size holds (their second operands are numbers past the bound) */
    begin(&m);
    function(&m);
    for (uint32_t k = 0; k < 60; k++) put(&m, I_ADD, 4, (uint32_t)T, 40 + k, (uint32_t)X, 1000 + k / 2);
    put(&m, STORE, 2, (uint32_t)OUT, 99u);
    end(&m);
    left = run(&m, &o);
    check(left == m.count - 30 * 5 && count(&o, left, I_ADD, 0) == 30 && find(&o, left, STORE, 0)[2] == 98,
          "of sixty sums, two by two equal, thirty go");
    /* A table half full, where looking one instruction up walks past others: 128 instructions of which no two are
       equal, each differing from another in one thing only (the second operand, the first, the type, the order
       of a difference's operands). The numbers are only such that the walks happen. */
    begin(&m);
    m.words[3] = 1000; /* the bound, for the results below */
    function(&m);
    for (uint32_t k = 0; k < 32; k++) {
        put(&m, I_SUB, 4, (uint32_t)T, 200 + 4 * k, (uint32_t)X, 1000 + k);
        put(&m, I_SUB, 4, (uint32_t)T, 201 + 4 * k, 1000 + k, (uint32_t)X);
        put(&m, I_SUB, 4, (uint32_t)T2, 202 + 4 * k, (uint32_t)X, 1000 + k);
        put(&m, I_SUB, 4, (uint32_t)T, 203 + 4 * k, (uint32_t)Y, 1000 + k);
    }
    end(&m);
    check(untouched(&m), "instructions that share a table's walk and differ in one operand or in their type stay");
    check(twice(0, I_ADD, 1, (uint32_t)X) == 1, "a sum short of an operand pairs by the words it has");
    /* two instructions the table keeps side by side (ids 21 and 28 are picked for that): each is compared whole */
    begin(&m);
    function(&m);
    put(&m, I_SUB, 4, (uint32_t)T, (uint32_t)A, (uint32_t)X, (uint32_t)Y);
    put(&m, I_SUB, 4, 21u, (uint32_t)B, (uint32_t)X, (uint32_t)Y);
    end(&m);
    check(untouched(&m), "differences of two types stay, in one slot of the table too");
    begin(&m);
    function(&m);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)A, (uint32_t)X, (uint32_t)Y);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)B, 28u, (uint32_t)X);
    end(&m);
    check(untouched(&m), "sums that share one operand of two stay, in one slot of the table too");

    /* a loop: the body's sum equal to the header's goes, and the header's phi, which read it from the body before
       the walk reached the body, reads the header's */
    begin(&m);
    function(&m);
    put(&m, F_MUL, 4, (uint32_t)T, (uint32_t)A, (uint32_t)X, (uint32_t)Y);
    put(&m, BRANCH, 1, (uint32_t)L1);
    put(&m, LABEL, 1, (uint32_t)L1);
    put(&m, PHI, 6, (uint32_t)T, (uint32_t)C, (uint32_t)X, (uint32_t)L0, (uint32_t)D, (uint32_t)L2);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)B, (uint32_t)X, (uint32_t)Y);
    put(&m, LOOP_MERGE, 3, (uint32_t)L3, (uint32_t)L2, 0u);
    put(&m, BRANCH_CONDITIONAL, 3, (uint32_t)X, (uint32_t)L2, (uint32_t)L3);
    put(&m, LABEL, 1, (uint32_t)L2);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)D, (uint32_t)X, (uint32_t)Y);
    put(&m, F_MUL, 4, (uint32_t)T, (uint32_t)E, (uint32_t)X, (uint32_t)Y);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)E);
    put(&m, BRANCH, 1, (uint32_t)L1);
    put(&m, LABEL, 1, (uint32_t)L3);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)C);
    end(&m);
    left = run(&m, &o);
    check(left == m.count - 10 && count(&o, left, I_ADD, B) == 1 && count(&o, left, F_MUL, A) == 1,
          "a loop's body takes what its header and the block before the loop computed");
    check(find(&o, left, PHI, 0)[5] == B && find(&o, left, PHI, 0)[3] == X && find(&o, left, PHI, 0)[4] == L0 &&
              find(&o, left, PHI, 0)[6] == L2,
          "a phi reads the header's sum where it read the body's, and its labels stay");
    check(find(&o, left, STORE, 0)[2] == A, "the body's store reads the product from before the loop");

    /* a block nothing reaches is dominated by none and dominates none; inside it the rule holds */
    begin(&m);
    function(&m);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)A, (uint32_t)X, (uint32_t)Y);
    put(&m, BRANCH, 1, (uint32_t)L2);
    put(&m, LABEL, 1, (uint32_t)L1);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)B, (uint32_t)X, (uint32_t)Y);
    put(&m, F_MUL, 4, (uint32_t)T, (uint32_t)C, (uint32_t)X, (uint32_t)Y);
    put(&m, F_MUL, 4, (uint32_t)T, (uint32_t)D, (uint32_t)X, (uint32_t)Y);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)D);
    put(&m, BRANCH, 1, (uint32_t)L2);
    put(&m, LABEL, 1, (uint32_t)L2);
    put(&m, F_MUL, 4, (uint32_t)T, (uint32_t)E, (uint32_t)X, (uint32_t)Y);
    end(&m);
    left = run(&m, &o);
    check(left == m.count - 5 && count(&o, left, I_ADD, 0) == 2 && count(&o, left, F_MUL, C) == 1 &&
              count(&o, left, F_MUL, E) == 1 && find(&o, left, STORE, 0)[2] == C,
          "a block nothing reaches pairs with no other block, only within itself");

    /* two functions share nothing */
    begin(&m);
    function(&m);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)A, (uint32_t)X, (uint32_t)Y);
    end(&m);
    put(&m, FUNCTION, 4, (uint32_t)T, (uint32_t)F2, 0u, (uint32_t)T);
    put(&m, LABEL, 1, (uint32_t)L1);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)B, (uint32_t)X, (uint32_t)Y);
    put(&m, BRANCH, 1, (uint32_t)L2);
    put(&m, LABEL, 1, (uint32_t)L2);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)C, (uint32_t)X, (uint32_t)Y);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)C);
    end(&m);
    left = run(&m, &o);
    check(left == m.count - 5 && count(&o, left, I_ADD, A) == 1 && count(&o, left, I_ADD, B) == 1 &&
              find(&o, left, STORE, 0)[2] == B,
          "a function's sum is not another function's; the second function's blocks have their own dominators");

    /* decorations: equal ones pair and the one that goes takes its own with it; different ones do not pair */
    begin(&m);
    put(&m, DECORATE, 2, (uint32_t)A, (uint32_t)NO_CONTRACTION);
    put(&m, DECORATE, 2, (uint32_t)B, (uint32_t)NO_CONTRACTION);
    put(&m, DECORATE, 2, (uint32_t)C, (uint32_t)NO_CONTRACTION);
    put(&m, DECORATE, 2, (uint32_t)E, 0u);
    put(&m, DECORATE, 2, (uint32_t)G, 0u);
    put(&m, DECORATE, 2, (uint32_t)G, (uint32_t)NO_CONTRACTION);
    function(&m);
    put(&m, F_MUL, 4, (uint32_t)T, (uint32_t)A, (uint32_t)X, (uint32_t)Y);
    put(&m, F_MUL, 4, (uint32_t)T, (uint32_t)B, (uint32_t)X, (uint32_t)Y);
    put(&m, F_ADD, 4, (uint32_t)T, (uint32_t)C, (uint32_t)X, (uint32_t)Y);
    put(&m, F_ADD, 4, (uint32_t)T, (uint32_t)D, (uint32_t)X, (uint32_t)Y);
    put(&m, F_ADD, 4, (uint32_t)T, (uint32_t)E, (uint32_t)X, (uint32_t)Y);
    put(&m, F_ADD, 4, (uint32_t)T, (uint32_t)G, (uint32_t)X, (uint32_t)Y);
    put(&m, F_ADD, 4, (uint32_t)T, (uint32_t)H, (uint32_t)X, (uint32_t)Y);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)B);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)H);
    end(&m);
    left = run(&m, &o);
    check(left == m.count - 5 - 3 - 5 && count(&o, left, F_MUL, A) == 1 && count(&o, left, F_MUL, 0) == 1 &&
              count(&o, left, DECORATE, 0) == 5,
          "two products decorated alike pair, and the second's decoration goes with it");
    check(count(&o, left, F_ADD, 0) == 4 && !count(&o, left, F_ADD, H) && find(&o, left, STORE, 1)[2] == D,
          "sums that differ in a decoration stay, each its own; an undecorated one pairs with the undecorated");
    /* one kind of decoration with two operands is two decorations */
    begin(&m);
    put(&m, DECORATE, 3, (uint32_t)A, 44u, (uint32_t)X);
    put(&m, DECORATE, 3, (uint32_t)B, 44u, (uint32_t)Y);
    function(&m);
    put(&m, F_MUL, 4, (uint32_t)T, (uint32_t)A, (uint32_t)X, (uint32_t)Y);
    put(&m, F_MUL, 4, (uint32_t)T, (uint32_t)B, (uint32_t)X, (uint32_t)Y);
    end(&m);
    check(untouched(&m), "two products whose decorations differ in an operand stay");
    /* past 32 kinds of decoration the pass tells no more apart: what has one of those neither goes nor stands for
       another */
    begin(&m);
    for (uint32_t k = 0; k < 32; k++) put(&m, DECORATE, 2, (uint32_t)Z, 100u + k);
    put(&m, DECORATE, 2, (uint32_t)A, 200u);
    put(&m, DECORATE, 2, (uint32_t)D, 200u);
    function(&m);
    put(&m, F_MUL, 4, (uint32_t)T, (uint32_t)A, (uint32_t)X, (uint32_t)Y);
    put(&m, F_MUL, 4, (uint32_t)T, (uint32_t)B, (uint32_t)X, (uint32_t)Y);
    put(&m, F_MUL, 4, (uint32_t)T, (uint32_t)C, (uint32_t)X, (uint32_t)Y);
    put(&m, F_MUL, 4, (uint32_t)T, (uint32_t)D, (uint32_t)X, (uint32_t)Y);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)C);
    end(&m);
    left = run(&m, &o);
    check(left == m.count - 5 && count(&o, left, F_MUL, A) == 1 && count(&o, left, F_MUL, D) == 1 &&
              find(&o, left, STORE, 0)[2] == B,
          "a 33rd kind of decoration keeps its instruction apart; the undecorated ones around it pair");
    /* a decoration with operands, in any of its forms, keeps its instruction apart */
    static const uint32_t with_operands[] = {DECORATE, DECORATE_ID, 5632, GROUP_DECORATE, GROUP_MEMBER_DECORATE};
    for (size_t k = 0; k < sizeof with_operands / sizeof *with_operands; k++) {
        for (uint32_t which = A; which <= B; which++) {
            begin(&m);
            if (with_operands[k] == GROUP_DECORATE || with_operands[k] == GROUP_MEMBER_DECORATE)
                put(&m, with_operands[k], 3, (uint32_t)Z, (uint32_t)X, which);
            else
                put(&m, with_operands[k], 3, which, 44u, (uint32_t)X);
            function(&m);
            put(&m, F_MUL, 4, (uint32_t)T, (uint32_t)A, (uint32_t)X, (uint32_t)Y);
            put(&m, F_MUL, 4, (uint32_t)T, (uint32_t)B, (uint32_t)X, (uint32_t)Y);
            put(&m, F_MUL, 4, (uint32_t)T, (uint32_t)C, (uint32_t)X, (uint32_t)Y);
            put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)C);
            end(&m);
            left = run(&m, &o);
            if (left != m.count - 5)
                fprintf(stderr, "decoration opcode %u: %zu of %zu\n", with_operands[k], left, m.count);
            check(left == m.count - 5 && count(&o, left, F_MUL, A) == 1 && count(&o, left, F_MUL, B) == 1 &&
                      find(&o, left, STORE, 0)[2] == (which == A ? B : A),
                  "an instruction whose decoration has operands neither goes nor stands for another");
        }
    }

    /* an instruction of no known layout: whatever its words number stays, and it is never numbered itself */
    begin(&m);
    function(&m);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)A, (uint32_t)X, (uint32_t)Y);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)B, (uint32_t)X, (uint32_t)Y);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)C, (uint32_t)X, (uint32_t)Y);
    put(&m, GROUP_NON_UNIFORM_BALLOT, 4, (uint32_t)T, (uint32_t)D, (uint32_t)X, (uint32_t)B);
    put(&m, GROUP_NON_UNIFORM_BALLOT, 4, (uint32_t)T, (uint32_t)E, (uint32_t)X, (uint32_t)B);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)C);
    end(&m);
    left = run(&m, &o);
    check(left == m.count - 5 && count(&o, left, I_ADD, B) == 1 && !count(&o, left, I_ADD, C) &&
              count(&o, left, GROUP_NON_UNIFORM_BALLOT, 0) == 2 && find(&o, left, GROUP_NON_UNIFORM_BALLOT, 0)[4] == B,
          "a sum an unknown instruction names stays; two equal unknown instructions stay");
    check(twice(0, EXT_INST, 3, (uint32_t)OTHER, 50u, (uint32_t)X) == 0, "another extended set's instructions stay");
    check(twice(0, ARRAY_LENGTH, 2, (uint32_t)X, 1u) == 0 && twice(0, 123, 2, (uint32_t)X, 1u) == 0,
          "an array's length and a cast with a storage class, whose literals the pass does not list, stay");

    /* an opcode's class is asked once a module, but for the ones that hang on an instruction's words: another
       set's instruction after GLSL.std.450's, a volatile load after a plain one, a sum cut short after a whole one */
    begin(&m);
    function(&m);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)A, (uint32_t)X, (uint32_t)Y);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)B, (uint32_t)X, (uint32_t)Y);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)C, (uint32_t)X, (uint32_t)Y);
    put(&m, EXT_INST, 5, (uint32_t)T, (uint32_t)D, (uint32_t)GLSL, 31u, (uint32_t)X);
    put(&m, EXT_INST, 5, (uint32_t)T, (uint32_t)E, (uint32_t)OTHER, 31u, (uint32_t)B);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)C);
    end(&m);
    left = run(&m, &o);
    check(left == m.count - 5 && count(&o, left, I_ADD, B) == 1 && find(&o, left, STORE, 0)[2] == A,
          "another set's instruction keeps what it names, after a GLSL.std.450 one in the module too");
    begin(&m);
    function(&m);
    put(&m, LOAD, 3, (uint32_t)T, (uint32_t)A, (uint32_t)IN);
    put(&m, LOAD, 4, (uint32_t)T, (uint32_t)B, (uint32_t)IN, 1u);
    put(&m, LOAD, 4, (uint32_t)T, (uint32_t)C, (uint32_t)IN, 1u);
    end(&m);
    check(untouched(&m), "two volatile loads stay two, after a plain load in the module too");
    begin(&m);
    function(&m);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)A, (uint32_t)X, (uint32_t)Y);
    put(&m, I_ADD, 1, (uint32_t)T);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)B, (uint32_t)X, (uint32_t)Y);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)B);
    end(&m);
    left = run(&m, &o);
    check(left == m.count - 5 && count(&o, left, I_ADD, A) == 1 && find(&o, left, STORE, 0)[2] == A,
          "a sum cut short of its result is left where it is, after a whole sum in the module too");

    /* what only makes a result but is not a value of its operands alone */
    check(twice(0, UNDEF, 0) == 0, "two undefined values stay two");
    check(twice(0, IMAGE_TEXEL_POINTER, 3, (uint32_t)IMAGE, (uint32_t)X, (uint32_t)X) == 0, "texel pointers stay");
    check(twice(0, IMAGE_READ, 2, (uint32_t)IMAGE, (uint32_t)X) == 0, "two reads of a storage image stay");
    check(twice(0, PHI, 2, (uint32_t)X, (uint32_t)L0) == 0, "two phis stay");
    check(twice(0, LOAD, 2, (uint32_t)IN, 1u) == 0, "two volatile loads stay");
    check(twice(0, EXT_INST, 4, (uint32_t)GLSL, 35u, (uint32_t)X, (uint32_t)OUT) == 0 &&
              twice(0, EXT_INST, 4, (uint32_t)GLSL, 51u, (uint32_t)X, (uint32_t)OUT) == 0,
          "GLSL.std.450's Modf and Frexp, which write through their pointer, stay");
    check(twice(0, EXT_INST, 3, (uint32_t)GLSL, 76u, (uint32_t)IN) == 0 &&
              twice(0, EXT_INST, 4, (uint32_t)GLSL, 77u, (uint32_t)IN, (uint32_t)X) == 0 &&
              twice(0, EXT_INST, 4, (uint32_t)GLSL, 78u, (uint32_t)IN, (uint32_t)X) == 0,
          "GLSL.std.450's interpolations, which read through their pointer, stay");
    check(twice(0, EXT_INST, 3, (uint32_t)GLSL, 75u, (uint32_t)X) == 1 &&
              twice(0, EXT_INST, 3, (uint32_t)GLSL, 79u, (uint32_t)X) == 1 &&
              twice(0, EXT_INST, 5, (uint32_t)GLSL, 50u, (uint32_t)X, (uint32_t)Y, (uint32_t)Z) == 1,
          "GLSL.std.450's other instructions pair");
    check(twice(0, EXT_INST, 2, (uint32_t)GLSL, 31u) == 1, "an extended instruction's operands may be none");

    /* implicit derivatives and OpSampledImage: within a block only */
    static const uint32_t in_block[] = {SAMPLED_IMAGE, IMAGE_SAMPLE_IMPLICIT_LOD, 89, 91, 93, 105, 207, 211, 215};
    static const uint32_t anywhere[] = {IMAGE_SAMPLE_EXPLICIT_LOD, 90, 92, 94, 95, 96, 100, 104, 106, 206, 216};
    for (size_t k = 0; k < sizeof in_block / sizeof *in_block + sizeof anywhere / sizeof *anywhere; k++) {
        int local = k < sizeof in_block / sizeof *in_block;
        uint32_t op = local ? in_block[k] : anywhere[k - sizeof in_block / sizeof *in_block];
        begin(&m);
        function(&m);
        put(&m, op, 4, (uint32_t)T, (uint32_t)A, (uint32_t)X, (uint32_t)Y);
        put(&m, op, 4, (uint32_t)T, (uint32_t)B, (uint32_t)X, (uint32_t)Y);
        put(&m, BRANCH, 1, (uint32_t)L1);
        put(&m, LABEL, 1, (uint32_t)L1);
        put(&m, op, 4, (uint32_t)T, (uint32_t)C, (uint32_t)X, (uint32_t)Y);
        put(&m, op, 4, (uint32_t)T, (uint32_t)D, (uint32_t)X, (uint32_t)Y);
        put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)B);
        put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)D);
        end(&m);
        left = run(&m, &o);
        int ok = op >= 206 && (op < 207 || op > 215) /* 206 and 216 make no result the pass knows: they stay */
                     ? left == m.count
                     : local ? left == m.count - 10 && count(&o, left, op, C) == 1 && find(&o, left, STORE, 1)[2] == C
                             : left == m.count - 15 && count(&o, left, op, 0) == 1 && find(&o, left, STORE, 1)[2] == A;
        if (!ok) fprintf(stderr, "opcode %u: %zu words left of %zu\n", op, left, m.count);
        check(ok, "a sample or a derivative with implicit derivatives, and a sampled image, pair in their own block "
                  "only; the explicit ones, a fetch, a gather and a query from a block that dominates");
    }

    /* loads of memory the module cannot write: from a block that dominates, through equal access chains too */
    static const uint32_t constant[] = {IN, IMAGE, UBO, PUSH};
    static const uint32_t chains[] = {0, ACCESS_CHAIN, IN_BOUNDS_ACCESS_CHAIN, COPY_OBJECT};
    for (size_t k = 0; k < sizeof constant / sizeof *constant * (sizeof chains / sizeof *chains); k++) {
        uint32_t p = constant[k / 4], chain = chains[k % 4];
        begin(&m);
        put(&m, DECORATE, 2, (uint32_t)Z, (uint32_t)NO_CONTRACTION); /* any decoration but BufferBlock */
        function(&m);
        if (chain) put(&m, chain, 4, (uint32_t)T, (uint32_t)C, p, (uint32_t)X);
        put(&m, LOAD, 3, (uint32_t)T, (uint32_t)A, chain ? (uint32_t)C : p);
        put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)X);
        put(&m, BRANCH, 1, (uint32_t)L1);
        put(&m, LABEL, 1, (uint32_t)L1);
        if (chain) put(&m, chain, 4, (uint32_t)T, (uint32_t)D, p, (uint32_t)X);
        put(&m, LOAD, 3, (uint32_t)T, (uint32_t)B, chain ? (uint32_t)D : p);
        put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)B);
        end(&m);
        left = run(&m, &o);
        if (left != m.count - 4 - (chain ? 5 : 0)) fprintf(stderr, "variable %u, chain %u\n", p, chain);
        check(left == m.count - 4 - (chain ? 5 : 0) && count(&o, left, LOAD, 0) == 1 &&
                  find(&o, left, STORE, 1)[2] == A,
              "a second load of an Input, UniformConstant, Uniform or PushConstant variable goes, across a store and "
              "a block, the pointer the variable or an equal chain or copy of it");
    }
    /* a pointer made any other way (a select of two variables) is not known to be constant */
    begin(&m);
    function(&m);
    put(&m, SELECT, 5, (uint32_t)T, (uint32_t)C, (uint32_t)IN, (uint32_t)IN, (uint32_t)IN);
    put(&m, LOAD, 3, (uint32_t)T, (uint32_t)A, (uint32_t)C);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)X);
    put(&m, LOAD, 3, (uint32_t)T, (uint32_t)B, (uint32_t)C);
    end(&m);
    check(untouched(&m), "a load through a pointer that is no chain of a constant variable does not cross a store");
    /* nor is a chain of a variable the module may write */
    begin(&m);
    function(&m);
    put(&m, ACCESS_CHAIN, 4, (uint32_t)T, (uint32_t)C, (uint32_t)OUT, (uint32_t)X);
    put(&m, LOAD, 3, (uint32_t)T, (uint32_t)A, (uint32_t)C);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)X);
    put(&m, LOAD, 3, (uint32_t)T, (uint32_t)B, (uint32_t)C);
    end(&m);
    check(untouched(&m), "a load through a chain of an Output variable does not cross a store");
    /* a module with a BufferBlock may write under Uniform */
    begin(&m);
    put(&m, DECORATE, 2, (uint32_t)T2, (uint32_t)BUFFER_BLOCK);
    function(&m);
    put(&m, LOAD, 3, (uint32_t)T, (uint32_t)A, (uint32_t)UBO);
    put(&m, STORE, 2, (uint32_t)UBO, (uint32_t)X);
    put(&m, LOAD, 3, (uint32_t)T, (uint32_t)B, (uint32_t)UBO);
    put(&m, LOAD, 3, (uint32_t)T, (uint32_t)C, (uint32_t)IN);
    put(&m, LOAD, 3, (uint32_t)T, (uint32_t)D, (uint32_t)IN);
    end(&m);
    left = run(&m, &o);
    check(left == m.count - 4 && count(&o, left, LOAD, B) == 1 && !count(&o, left, LOAD, D),
          "with a BufferBlock declared a Uniform load does not cross a store; an Input's still does");

    /* loads of memory the module may write: in one block, up to what may write */
    static const uint32_t written[] = {OUT, SSBO, V0};
    for (size_t k = 0; k < sizeof written / sizeof *written; k++) {
        uint32_t p = written[k];
        uint32_t a[4] = {4u << 16 | LOAD, T, A, p}, b[4] = {4u << 16 | LOAD, T, B, p};
        check(pairs(a, b, 0) == 1, "a second load of an Output, a buffer or a local in the block goes");
        /* a store, a copy, a call, an atomic, an image write, a barrier, an unknown instruction */
        static const uint32_t writers[] = {
            STORE, COPY_MEMORY, FUNCTION_CALL, ATOMIC_I_ADD, IMAGE_WRITE, CONTROL_BARRIER, GROUP_NON_UNIFORM_BALLOT,
            227,   242,         225,           218,          5380};
        for (size_t n = 0; n < sizeof writers / sizeof *writers; n++) {
            int got = pairs(a, b, writers[n]);
            if (got) fprintf(stderr, "variable %u across opcode %u: %d\n", p, writers[n], got);
            check(!got, "a load does not cross an instruction that may write");
        }
        /* what only makes a result writes nothing: an undefined value, a phi, an image read, a sum */
        static const uint32_t readers[] = {UNDEF, PHI, IMAGE_READ, I_ADD, IMAGE_TEXEL_POINTER};
        for (size_t n = 0; n < sizeof readers / sizeof *readers; n++) {
            int got = pairs(a, b, readers[n]);
            if (got != 1) fprintf(stderr, "variable %u across opcode %u: %d\n", p, readers[n], got);
            check(got == 1, "a load crosses an instruction that only makes a result");
        }
        begin(&m);
        function(&m);
        put(&m, LOAD, 3, (uint32_t)T, (uint32_t)A, p);
        put(&m, BRANCH, 1, (uint32_t)L1);
        put(&m, LABEL, 1, (uint32_t)L1);
        put(&m, LOAD, 3, (uint32_t)T, (uint32_t)B, p);
        end(&m);
        check(untouched(&m), "a load of memory the module may write does not cross a block's start");
    }
    /* GLSL.std.450: Modf and Frexp write through their pointer, the others write nothing */
    for (uint32_t number = 34; number <= 52; number++) {
        begin(&m);
        function(&m);
        put(&m, LOAD, 3, (uint32_t)T, (uint32_t)A, (uint32_t)OUT);
        put(&m, EXT_INST, 6, (uint32_t)T, (uint32_t)C, (uint32_t)GLSL, number, (uint32_t)X, (uint32_t)OUT);
        put(&m, LOAD, 3, (uint32_t)T, (uint32_t)B, (uint32_t)OUT);
        end(&m);
        left = run(&m, &o);
        check(number == 35 || number == 51 ? left == m.count : left == m.count - 4,
              "a load crosses a GLSL.std.450 instruction, but not Modf or Frexp");
    }
    {
        uint32_t a[5] = {5u << 16 | LOAD, T, A, OUT, 2}, b[5] = {5u << 16 | LOAD, T, B, OUT, 2};
        uint32_t c[4] = {4u << 16 | LOAD, T, B, OUT};
        check(pairs(a, b, 0) == 1 && pairs(a, c, 0) == 0,
              "loads with a memory operand pair when it is the same, and not with a load that has none");
    }

    /* a literal names no id: B, of the number each literal below has, is replaced by A only where it is an id */
    begin(&m);
    put(&m, VARIABLE, 3, (uint32_t)T, (uint32_t)OUT, (uint32_t)OUTPUT);
    put(&m, FUNCTION, 4, (uint32_t)T, (uint32_t)F, 0u, (uint32_t)T);
    put(&m, LABEL, 1, (uint32_t)L0);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)A, (uint32_t)X, (uint32_t)Y);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)B, (uint32_t)X, (uint32_t)Y);
    put(&m, LINE, 3, (uint32_t)B, (uint32_t)B, (uint32_t)B);
    put(&m, COMPOSITE_EXTRACT, 4, (uint32_t)T, (uint32_t)C, (uint32_t)B, (uint32_t)B);
    put(&m, VECTOR_SHUFFLE, 5, (uint32_t)T, (uint32_t)D, (uint32_t)B, (uint32_t)B, (uint32_t)B);
    put(&m, COMPOSITE_INSERT, 5, (uint32_t)T, (uint32_t)E, (uint32_t)B, (uint32_t)B, (uint32_t)B);
    put(&m, EXT_INST, 5, (uint32_t)T, (uint32_t)G, (uint32_t)GLSL, (uint32_t)B, (uint32_t)B);
    put(&m, IMAGE_SAMPLE_EXPLICIT_LOD, 6, (uint32_t)T, (uint32_t)H, (uint32_t)B, (uint32_t)B, (uint32_t)B, (uint32_t)B);
    put(&m, LOAD, 4, (uint32_t)T, (uint32_t)I, (uint32_t)OUT, (uint32_t)B);
    put(&m, STORE, 3, (uint32_t)OUT, (uint32_t)B, (uint32_t)B);
    put(&m, COPY_MEMORY, 3, (uint32_t)OUT, (uint32_t)B, (uint32_t)B);
    put(&m, IMAGE_WRITE, 5, (uint32_t)B, (uint32_t)B, (uint32_t)B, (uint32_t)B, (uint32_t)B);
    put(&m, LOOP_MERGE, 3, (uint32_t)L1, (uint32_t)L2, (uint32_t)B);
    put(&m, BRANCH, 1, (uint32_t)L1);
    put(&m, LABEL, 1, (uint32_t)L1);
    put(&m, SELECTION_MERGE, 2, (uint32_t)L2, (uint32_t)B);
    put(&m, BRANCH_CONDITIONAL, 5, (uint32_t)B, (uint32_t)L2, (uint32_t)L2, (uint32_t)B, (uint32_t)B);
    put(&m, LABEL, 1, (uint32_t)L2);
    put(&m, SELECTION_MERGE, 2, (uint32_t)L3, 0u);
    put(&m, SWITCH, 4, (uint32_t)B, (uint32_t)L3, (uint32_t)B, (uint32_t)L3);
    put(&m, LABEL, 1, (uint32_t)L3);
    put(&m, RETURN_VALUE, 1, (uint32_t)B);
    put(&m, FUNCTION_END, 0);
    put(&m, FUNCTION, 4, (uint32_t)T, (uint32_t)F2, (uint32_t)B, (uint32_t)T);
    put(&m, LABEL, 1, (uint32_t)Z);
    put(&m, VARIABLE, 4, (uint32_t)T, (uint32_t)V0, (uint32_t)B, (uint32_t)B);
    end(&m);
    left = run(&m, &o);
    check(left == m.count - 5 && !count(&o, left, I_ADD, B), "the second sum goes");
    {
        const uint32_t* w;
        check(find(&o, left, FUNCTION, 1)[3] == B, "a function's control is a literal");
        w = find(&o, left, VARIABLE, 1);
        check(w[3] == B && w[4] == A, "a variable's storage class is a literal, its initializer an id");
        w = find(&o, left, LINE, 0);
        check(w[1] == B && w[2] == B && w[3] == B, "a line's file, line and column are left");
        w = find(&o, left, COMPOSITE_EXTRACT, 0);
        check(w[3] == A && w[4] == B, "an extract reads the first sum, and its index is a literal");
        w = find(&o, left, VECTOR_SHUFFLE, 0);
        check(w[3] == A && w[4] == A && w[5] == B, "a shuffle's vectors are ids, its components literals");
        w = find(&o, left, COMPOSITE_INSERT, 0);
        check(w[3] == A && w[4] == A && w[5] == B, "an insert's object and composite are ids, its index a literal");
        w = find(&o, left, EXT_INST, 0);
        check(w[4] == B && w[5] == A, "an extended instruction's number is a literal, its operands ids");
        w = find(&o, left, IMAGE_SAMPLE_EXPLICIT_LOD, 0);
        check(w[3] == A && w[4] == A && w[5] == B && w[6] == A,
              "a sample's image, coordinate and what follows its operands mask are ids; the mask is a literal");
        check(find(&o, left, LOAD, 0)[4] == B, "a load's memory operand is a literal");
        w = find(&o, left, STORE, 0);
        check(w[1] == OUT && w[2] == A && w[3] == B, "a store's value is an id, its memory operand a literal");
        w = find(&o, left, COPY_MEMORY, 0);
        check(w[2] == A && w[3] == B, "a copy's source is an id, its memory operand a literal");
        w = find(&o, left, IMAGE_WRITE, 0);
        check(w[1] == A && w[2] == A && w[3] == A && w[4] == B && w[5] == A,
              "an image write's operands are ids on both sides of its mask");
        check(find(&o, left, LOOP_MERGE, 0)[3] == B && find(&o, left, SELECTION_MERGE, 0)[2] == B,
              "a merge's control is a literal");
        w = find(&o, left, BRANCH_CONDITIONAL, 0);
        check(w[1] == A && w[4] == B && w[5] == B, "a branch's condition is an id, its weights literals");
        w = find(&o, left, SWITCH, 0);
        check(w[1] == A && w[2] == L3 && w[3] == B && w[4] == L3,
              "a switch's selector is an id, a case's value a literal");
        check(find(&o, left, RETURN_VALUE, 0)[1] == A, "a returned value is an id");
    }
    /* and in the instructions that are ids all through */
    begin(&m);
    function(&m);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)A, (uint32_t)X, (uint32_t)Y);
    put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)B, (uint32_t)X, (uint32_t)Y);
    put(&m, FUNCTION_CALL, 5, (uint32_t)T, (uint32_t)C, (uint32_t)F2, (uint32_t)B, (uint32_t)B);
    put(&m, ATOMIC_I_ADD, 6, (uint32_t)T, (uint32_t)D, (uint32_t)OUT, (uint32_t)B, (uint32_t)B, (uint32_t)B);
    put(&m, CONTROL_BARRIER, 3, (uint32_t)B, (uint32_t)B, (uint32_t)B);
    put(&m, PHI, 4, (uint32_t)T, (uint32_t)E, (uint32_t)B, (uint32_t)L0);
    put(&m, IMAGE_READ, 6, (uint32_t)T, (uint32_t)G, (uint32_t)B, (uint32_t)B, (uint32_t)B, (uint32_t)B);
    put(&m, EXT_INST, 6, (uint32_t)T, (uint32_t)H, (uint32_t)GLSL, 35u, (uint32_t)B, (uint32_t)B);
    put(&m, 227, 5, (uint32_t)T, (uint32_t)I, (uint32_t)OUT, (uint32_t)B, (uint32_t)B);
    put(&m, 242, 6, (uint32_t)T, (uint32_t)(I + 1), (uint32_t)OUT, (uint32_t)B, (uint32_t)B, (uint32_t)B);
    put(&m, 225, 2, (uint32_t)B, (uint32_t)B);
    end(&m);
    left = run(&m, &o);
    check(find(&o, left, 227, 0)[4] == A && find(&o, left, 227, 0)[5] == A && find(&o, left, 242, 0)[6] == A &&
              find(&o, left, 225, 0)[1] == A && find(&o, left, 225, 0)[2] == A,
          "the first and the last of the atomics and a memory barrier read the first sum");
    {
        const uint32_t* w = find(&o, left, FUNCTION_CALL, 0);
        check(left == m.count - 5 && w[3] == F2 && w[4] == A && w[5] == A, "a call's arguments read the first sum");
        w = find(&o, left, ATOMIC_I_ADD, 0);
        check(w[3] == OUT && w[4] == A && w[5] == A && w[6] == A, "an atomic's scope, semantics and value do");
        w = find(&o, left, CONTROL_BARRIER, 0);
        check(w[1] == A && w[2] == A && w[3] == A, "a barrier's operands do");
        check(find(&o, left, PHI, 0)[3] == A, "a phi's value does");
        w = find(&o, left, IMAGE_READ, 0);
        check(w[3] == A && w[4] == A && w[5] == B && w[6] == A, "an image read's operands do, but its mask");
        w = find(&o, left, EXT_INST, 0);
        check(w[4] == 35 && w[5] == A && w[6] == A, "Modf's operands do");
    }

    /* a switch on a 64-bit selector has cases of three words: a module that could hold one is left alone */
    for (int declared = 0; declared < 4; declared++) {
        begin(&m);
        put(&m, TYPE_INT, 3, (uint32_t)WIDE, declared & 1 ? 64u : 32u, 0u);
        function(&m);
        put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)A, (uint32_t)X, (uint32_t)Y);
        put(&m, I_ADD, 4, (uint32_t)T, (uint32_t)B, (uint32_t)X, (uint32_t)Y);
        if (declared & 2) {
            put(&m, SELECTION_MERGE, 2, (uint32_t)L1, 0u);
            if (declared & 1)
                put(&m, SWITCH, 5, (uint32_t)X, (uint32_t)L1, 1u, 0u, (uint32_t)L1); /* a case of three words */
            else
                put(&m, SWITCH, 4, (uint32_t)X, (uint32_t)L1, 1u, (uint32_t)L1);
            put(&m, LABEL, 1, (uint32_t)L1);
        }
        end(&m);
        left = run(&m, &o);
        check(declared == 3 ? left == m.count && !memcmp(o.words, m.words, m.count * sizeof *m.words)
                            : left == m.count - 5,
              "a module with a 64-bit integer type and a switch is left as it is; with a 32-bit one, or no switch, "
              "it is not");
    }

    /* nothing to do */
    begin(&m);
    function(&m);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)X);
    end(&m);
    check(untouched(&m), "a function with nothing to number is left as it is");
    begin(&m);
    check(untouched(&m), "a module with no function is left as it is");
    uint32_t part[3] = {0x07230203, 0x00010600, 0};
    check(spv_opt_values(part, 3) == 3, "less than a header is left as it is");
    printf("spirv opt values: %d checks failed\n", g_failures);
    return g_failures != 0;
}
