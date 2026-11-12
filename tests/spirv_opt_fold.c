/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What spv_opt_values (common/utilities/spirv_opt.h) folds: an integer or boolean instruction on constants, one an
   operand decides or leaves as it is, a select and a bitcast that are one of their operands. Each module here is a
   function that loads a value of each type, makes one instruction of them and of constants, and stores its result:
   the store then reads a constant, an operand, or the instruction that stayed. tests/spirv_opt_values.c has the
   numbering. */
#include "common/utilities/spirv_opt.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum {
    TYPE_VOID = 19,
    TYPE_BOOL = 20,
    TYPE_INT = 21,
    CONSTANT_TRUE = 41,
    CONSTANT_FALSE = 42,
    CONSTANT = 43,
    CONSTANT_NULL = 46,
    SPEC_CONSTANT = 50,
    FUNCTION = 54,
    FUNCTION_END = 56,
    VARIABLE = 59,
    LOAD = 61,
    STORE = 62,
    DECORATE = 71,
    BITCAST = 124,
    S_NEGATE = 126,
    I_ADD = 128,
    I_SUB = 130,
    I_MUL = 132,
    LOGICAL_EQUAL = 164,
    LOGICAL_NOT_EQUAL = 165,
    LOGICAL_OR = 166,
    LOGICAL_AND = 167,
    LOGICAL_NOT = 168,
    SELECT = 169,
    I_EQUAL = 170,
    I_NOT_EQUAL = 171,
    U_GREATER_THAN = 172,
    S_GREATER_THAN = 173,
    U_GREATER_THAN_EQUAL = 174,
    S_GREATER_THAN_EQUAL = 175,
    U_LESS_THAN = 176,
    S_LESS_THAN = 177,
    U_LESS_THAN_EQUAL = 178,
    S_LESS_THAN_EQUAL = 179,
    SHIFT_RIGHT_LOGICAL = 194,
    SHIFT_RIGHT_ARITHMETIC = 195,
    SHIFT_LEFT_LOGICAL = 196,
    BITWISE_OR = 197,
    BITWISE_XOR = 198,
    BITWISE_AND = 199,
    NOT = 200,
    BIT_FIELD_INSERT = 201,
    BIT_FIELD_S_EXTRACT = 202,
    BIT_FIELD_U_EXTRACT = 203,
    LABEL = 248,
    RETURN = 253,
    GROUP_NON_UNIFORM_BALLOT = 339, /* an opcode the pass has no layout for */
    NO_CONTRACTION = 42,
    /* ids: the types */
    U = 1,     /* the 32-bit unsigned integer */
    S = 2,     /* the 32-bit signed integer */
    BOOL = 3,  /* the boolean */
    F = 4,     /* a type the pass folds nothing of */
    SHORT = 5, /* a 16-bit integer */
    OUT = 6,
    IN = 7,
    FN = 8,
    L0 = 9,
    /* the constants every module here declares */
    U0 = 10,
    U1 = 11,
    ONES = 12, /* 0xffffffff */
    YES = 13,
    NO = 14,
    SPEC = 15,   /* a specialization constant of U */
    NIL = 16,    /* U's null constant */
    SHORT0 = 17, /* a 16-bit zero */
    F0 = 18,     /* a constant of F, its bits zero */
    S0 = 19,     /* a signed zero */
    KA = 20,     /* constants of U with a module's own values */
    KB = 21,
    KC = 22,
    /* the values the function loads */
    X = 30,  /* of U */
    Y = 31,  /* of U */
    XS = 32, /* of S */
    XB = 33, /* of BOOL */
    XF = 34, /* of F */
    P = 40,  /* instructions before the one a module is about */
    Q = 41,
    R = 42, /* the instruction's result */
    BOUND = 64,
    /* what a case expects of the store */
    STAYS = 0, /* the instruction is left */
    IS = 1,    /* it reads this id */
    VALUE = 2  /* it reads a constant of this type and value */
};

typedef struct {
    uint32_t words[512];
    size_t count;
} Module;
static int g_failures;

static void check(int pass, const char* what) {
    if (pass) return;
    fprintf(stderr, "FAIL: %s\n", what);
    g_failures++;
}
static void put(Module* m, uint32_t op, int n, ...) {
    va_list operands;
    m->words[m->count++] = (uint32_t)(n + 1) << 16 | op;
    va_start(operands, n);
    for (int i = 0; i < n; i++) m->words[m->count++] = va_arg(operands, uint32_t);
    va_end(operands);
}
/* the header, the types, the constants (KA, KB and KC of U with the values given) and the function up to its loads */
static void begin(Module* m, uint32_t ka, uint32_t kb, uint32_t kc) {
    m->count = 0;
    m->words[m->count++] = 0x07230203;
    m->words[m->count++] = 0x00010600;
    m->words[m->count++] = 0;
    m->words[m->count++] = BOUND;
    m->words[m->count++] = 0;
    put(m, TYPE_INT, 3, (uint32_t)U, 32u, 0u);
    put(m, TYPE_INT, 3, (uint32_t)S, 32u, 1u);
    put(m, TYPE_BOOL, 1, (uint32_t)BOOL);
    put(m, TYPE_VOID, 1, (uint32_t)F);
    put(m, TYPE_INT, 3, (uint32_t)SHORT, 16u, 0u);
    put(m, CONSTANT, 3, (uint32_t)U, (uint32_t)U0, 0u);
    put(m, CONSTANT, 3, (uint32_t)U, (uint32_t)U1, 1u);
    put(m, CONSTANT, 3, (uint32_t)U, (uint32_t)ONES, 0xffffffffu);
    put(m, CONSTANT_TRUE, 2, (uint32_t)BOOL, (uint32_t)YES);
    put(m, CONSTANT_FALSE, 2, (uint32_t)BOOL, (uint32_t)NO);
    put(m, SPEC_CONSTANT, 3, (uint32_t)U, (uint32_t)SPEC, 0u);
    put(m, CONSTANT_NULL, 2, (uint32_t)U, (uint32_t)NIL);
    put(m, CONSTANT, 3, (uint32_t)SHORT, (uint32_t)SHORT0, 0u);
    put(m, CONSTANT, 3, (uint32_t)F, (uint32_t)F0, 0u);
    put(m, CONSTANT, 3, (uint32_t)S, (uint32_t)S0, 0u);
    put(m, CONSTANT, 3, (uint32_t)U, (uint32_t)KA, ka);
    put(m, CONSTANT, 3, (uint32_t)U, (uint32_t)KB, kb);
    put(m, CONSTANT, 3, (uint32_t)U, (uint32_t)KC, kc);
    put(m, VARIABLE, 3, (uint32_t)F, (uint32_t)OUT, 3u);
    put(m, VARIABLE, 3, (uint32_t)F, (uint32_t)IN, 6u);
    put(m, FUNCTION, 4, (uint32_t)F, (uint32_t)FN, 0u, (uint32_t)F);
    put(m, LABEL, 1, (uint32_t)L0);
    put(m, LOAD, 3, (uint32_t)U, (uint32_t)X, (uint32_t)IN);
    put(m, LOAD, 3, (uint32_t)U, (uint32_t)Y, (uint32_t)OUT);
    put(m, LOAD, 3, (uint32_t)S, (uint32_t)XS, (uint32_t)IN);
    put(m, LOAD, 3, (uint32_t)BOOL, (uint32_t)XB, (uint32_t)IN);
    put(m, LOAD, 3, (uint32_t)F, (uint32_t)XF, (uint32_t)IN);
}
static void end(Module* m) {
    put(m, RETURN, 0);
    put(m, FUNCTION_END, 0);
}
/* the instruction of m whose result (its third word) is id, or a row of zeros */
static const uint32_t* result(const Module* m, size_t words, uint32_t id) {
    static const uint32_t none[8];
    for (size_t i = 5; i < words; i += m->words[i] >> 16)
        if ((m->words[i] >> 16) >= 3 && (m->words[i] & 0xffff) != STORE && m->words[i + 2] == id) return m->words + i;
    return none;
}
/* the value the nth store of m (from 0) writes */
static uint32_t stored(const Module* m, size_t words, int nth) {
    for (size_t i = 5; i < words; i += m->words[i] >> 16)
        if ((m->words[i] & 0xffff) == STORE && !nth--) return m->words[i + 2];
    return 0;
}
/* whether id is declared, before the function, a constant of type with these bits */
static int constant(const Module* m, size_t words, uint32_t id, uint32_t type, uint32_t bits) {
    for (size_t i = 5; i < words && (m->words[i] & 0xffff) != FUNCTION; i += m->words[i] >> 16) {
        const uint32_t* w = m->words + i;
        uint32_t op = w[0] & 0xffff;
        if ((w[0] >> 16) < 3 || w[2] != id || w[1] != type) continue;
        if (op == CONSTANT && w[0] >> 16 == 4) return w[3] == bits;
        if (op == CONSTANT_TRUE || op == CONSTANT_FALSE) return (uint32_t)(op == CONSTANT_TRUE) == bits;
        if (op == CONSTANT_NULL) return !bits;
    }
    return 0;
}
/* The pass over m, whose last instruction before the store of R makes R with width words: whether the store now
   reads what expect says (IS: the id want; VALUE: a constant of type and bits want; STAYS: R, with m as it was).
   A constant the module did not have is R itself, declared in the room its instruction left. */
static int outcome(const Module* m, uint32_t width, int expect, uint32_t type, uint32_t want) {
    Module o = *m;
    size_t left = spv_opt_values(o.words, o.count);
    uint32_t read = stored(&o, left, 0);
    if (expect == STAYS) return left == m->count && !memcmp(o.words, m->words, m->count * sizeof *m->words);
    int gone = (result(&o, left, R)[0] & 0xffff) != (result(m, m->count, R)[0] & 0xffff);
    if (expect == IS) return read == want && gone && left == m->count - width;
    if (!constant(&o, left, read, type, want)) return 0;
    return read == R ? left == m->count - width + (type == BOOL ? 3 : 4) : gone && left == m->count - width;
}
/* one instruction of n operands (ids) on KA, KB and KC of the values given, and its outcome */
static int instruction(uint32_t ka, uint32_t kb, uint32_t kc, int expect, uint32_t want_type, uint32_t want,
                       uint32_t op, uint32_t type, int n, uint32_t a, uint32_t b, uint32_t c) {
    Module m;
    begin(&m, ka, kb, kc);
    if (n == 1) put(&m, op, 3, type, (uint32_t)R, a);
    if (n == 2) put(&m, op, 4, type, (uint32_t)R, a, b);
    if (n == 3) put(&m, op, 5, type, (uint32_t)R, a, b, c);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
    end(&m);
    int ok = outcome(&m, (uint32_t)n + 3, expect, want_type, want);
    if (!ok)
        fprintf(stderr, "opcode %u of type %u on %u (%#x), %u (%#x), %u (%#x): expected %s %#x\n", op, type, a, ka, b,
                kb, c, kc,
                expect == STAYS ? "to stay, not"
                : expect == IS  ? "id"
                                : "value",
                want);
    return ok;
}

int main(void) {
    Module m, o;
    size_t left;

    /* an integer instruction on two constants is the constant */
    static const struct {
        uint32_t op, a, b, value;
    } binary[] = {
        {I_ADD, 5, 3, 8},
        {I_ADD, 0xffffffff, 2, 1},
        {I_ADD, 5, 100, 105},
        {BITWISE_AND, 0xff, 0xf0, 0xf0},
        {I_SUB, 3, 5, 0xfffffffe},
        {I_SUB, 5, 3, 2},
        {I_MUL, 5, 3, 15},
        {I_MUL, 0x80000001, 2, 2},
        {BITWISE_AND, 5, 3, 1},
        {BITWISE_OR, 5, 3, 7},
        {BITWISE_XOR, 5, 3, 6},
        {SHIFT_LEFT_LOGICAL, 5, 3, 40},
        {SHIFT_LEFT_LOGICAL, 3, 31, 0x80000000},
        {SHIFT_RIGHT_LOGICAL, 0x80000000, 31, 1},
        {SHIFT_RIGHT_LOGICAL, 0xf0, 4, 0xf},
        {SHIFT_RIGHT_ARITHMETIC, 0x80000000, 31, 0xffffffff},
        {SHIFT_RIGHT_ARITHMETIC, 0x80000000, 4, 0xf8000000},
        {SHIFT_RIGHT_ARITHMETIC, 0x80000000, 0, 0x80000000},
        {SHIFT_RIGHT_ARITHMETIC, 0x40000000, 30, 1},
    };
    for (size_t k = 0; k < sizeof binary / sizeof *binary; k++) {
        check(instruction(binary[k].a, binary[k].b, 77, VALUE, U, binary[k].value, binary[k].op, U, 2, KA, KB, 0),
              "an integer instruction on two constants is the constant");
        check(instruction(binary[k].a, binary[k].b, 77, VALUE, S, binary[k].value, binary[k].op, S, 2, KA, KB, 0),
              "and a signed result of unsigned constants has their bits");
    }
    /* a shift by 32 or more has no value in a module: it stays */
    static const uint32_t shifts[] = {SHIFT_LEFT_LOGICAL, SHIFT_RIGHT_LOGICAL, SHIFT_RIGHT_ARITHMETIC};
    for (size_t k = 0; k < sizeof shifts / sizeof *shifts; k++) {
        check(instruction(5, 32, 77, STAYS, 0, 0, shifts[k], U, 2, KA, KB, 0), "a shift by 32 stays");
        check(instruction(5, 0xffffffff, 77, STAYS, 0, 0, shifts[k], U, 2, KA, KB, 0), "a shift by more stays");
        check(instruction(1, 31, 77, VALUE, U, shifts[k] == SHIFT_LEFT_LOGICAL ? 0x80000000u : 0u, shifts[k], U, 2, KA,
                          KB, 0),
              "a shift by 31 is folded");
    }
    /* the instructions the pass does not fold stay, on constants too */
    check(instruction(6, 3, 77, STAYS, 0, 0, 134, U, 2, KA, KB, 0), "a division stays");
    check(instruction(6, 3, 77, STAYS, 0, 0, 137, U, 2, KA, KB, 0), "a remainder stays");
    check(instruction(6, 3, 2, STAYS, 0, 0, BIT_FIELD_INSERT, U, 3, KA, KB, KC), "a field insert of three stays");
    check(instruction(6, 3, 2, STAYS, 0, 0, I_ADD, U, 3, KA, KB, KC), "a sum with a third operand stays");
    check(instruction(6, 3, 2, STAYS, 0, 0, 129, U, 2, KA, KB, 0), "a float sum stays");
    /* one operand */
    check(instruction(5, 0, 0, VALUE, U, 0xfffffffa, NOT, U, 1, KA, 0, 0), "the complement of a constant");
    check(instruction(5, 0, 0, VALUE, U, 0xfffffffb, S_NEGATE, U, 1, KA, 0, 0), "the negation of a constant");
    check(instruction(0, 0, 0, VALUE, U, 0, S_NEGATE, U, 1, KA, 0, 0), "the negation of zero");
    check(instruction(5, 0, 0, STAYS, 0, 0, NOT, U, 1, X, 0, 0), "the complement of a value stays");
    check(instruction(5, 0, 0, STAYS, 0, 0, 205, U, 1, KA, 0, 0), "a bit count stays");
    /* a field of a constant */
    static const struct {
        uint32_t op, base, offset, count;
        int expect;
        uint32_t value;
    } fields[] = {
        {BIT_FIELD_U_EXTRACT, 0xabcd1234, 8, 8, VALUE, 0x12},
        {BIT_FIELD_U_EXTRACT, 0xabcd1234, 0, 32, VALUE, 0xabcd1234},
        {BIT_FIELD_U_EXTRACT, 0xabcd1234, 28, 4, VALUE, 0xa},
        {BIT_FIELD_U_EXTRACT, 0xabcd1234, 31, 1, VALUE, 1},
        {BIT_FIELD_U_EXTRACT, 0xabcd1234, 7, 0, VALUE, 0},
        {BIT_FIELD_U_EXTRACT, 0xabcd1234, 32, 0, VALUE, 0},
        {BIT_FIELD_U_EXTRACT, 0xabcd1234, 28, 5, STAYS, 0},
        {BIT_FIELD_U_EXTRACT, 0xabcd1234, 33, 0, STAYS, 0},
        {BIT_FIELD_U_EXTRACT, 0xabcd1234, 0, 33, STAYS, 0},
        {BIT_FIELD_U_EXTRACT, 0xabcd1234, 0xffffffff, 2, STAYS, 0},
        {BIT_FIELD_S_EXTRACT, 0xf0, 4, 4, VALUE, 0xffffffff},
        {BIT_FIELD_S_EXTRACT, 0x70, 4, 4, VALUE, 7},
        {BIT_FIELD_S_EXTRACT, 0x80, 4, 4, VALUE, 0xfffffff8},
        {BIT_FIELD_S_EXTRACT, 0xabcd1234, 0, 32, VALUE, 0xabcd1234},
        {BIT_FIELD_S_EXTRACT, 0xabcd1234, 31, 1, VALUE, 0xffffffff},
        {BIT_FIELD_S_EXTRACT, 0xabcd1234, 3, 0, VALUE, 0},
        {BIT_FIELD_S_EXTRACT, 0xabcd1234, 28, 5, STAYS, 0},
    };
    for (size_t k = 0; k < sizeof fields / sizeof *fields; k++)
        check(instruction(fields[k].base, fields[k].offset, fields[k].count, fields[k].expect, U, fields[k].value,
                          fields[k].op, U, 3, KA, KB, KC),
              "a field of a constant is the constant when it ends by bit 32");
    check(instruction(0xf0, 4, 4, STAYS, 0, 0, BIT_FIELD_U_EXTRACT, U, 3, X, KB, KC) &&
              instruction(0xf0, 4, 4, STAYS, 0, 0, BIT_FIELD_U_EXTRACT, U, 3, KA, X, KC) &&
              instruction(0xf0, 4, 4, STAYS, 0, 0, BIT_FIELD_U_EXTRACT, U, 3, KA, KB, X),
          "a field whose base, offset or count is a value stays");

    /* a comparison of two constants: 5 with 3, 3 with 5, 5 with 5, and -1 with 1, which the signed ones order the
       other way */
    static const struct {
        uint32_t op, truth[4];
    } comparisons[] = {
        {I_EQUAL, {0, 0, 1, 0}},           {I_NOT_EQUAL, {1, 1, 0, 1}},          {U_GREATER_THAN, {1, 0, 0, 1}},
        {S_GREATER_THAN, {1, 0, 0, 0}},    {U_GREATER_THAN_EQUAL, {1, 0, 1, 1}}, {S_GREATER_THAN_EQUAL, {1, 0, 1, 0}},
        {U_LESS_THAN, {0, 1, 0, 0}},       {S_LESS_THAN, {0, 1, 0, 1}},          {U_LESS_THAN_EQUAL, {0, 1, 1, 0}},
        {S_LESS_THAN_EQUAL, {0, 1, 1, 1}},
    };
    static const uint32_t pairs[4][2] = {{5, 3}, {3, 5}, {5, 5}, {0xffffffff, 1}};
    for (size_t k = 0; k < sizeof comparisons / sizeof *comparisons; k++)
        for (int p = 0; p < 4; p++)
            check(instruction(pairs[p][0], pairs[p][1], 77, VALUE, BOOL, comparisons[k].truth[p], comparisons[k].op,
                              BOOL, 2, KA, KB, 0),
                  "a comparison of two constants is true or false");
    check(instruction(5, 5, 77, STAYS, 0, 0, I_EQUAL, BOOL, 2, X, KB, 0) &&
              instruction(5, 5, 77, STAYS, 0, 0, I_EQUAL, BOOL, 2, KA, X, 0) &&
              instruction(5, 5, 77, STAYS, 0, 0, I_EQUAL, BOOL, 2, X, X, 0),
          "a comparison with a value stays");
    check(instruction(5, 5, 77, STAYS, 0, 0, 169 + 11, BOOL, 2, KA, KB, 0) &&
              instruction(5, 5, 77, STAYS, 0, 0, 169, BOOL, 2, KA, KB, 0),
          "the opcodes on either side of the integer comparisons stay");
    check(instruction(5, 5, 77, STAYS, 0, 0, I_EQUAL, BOOL, 2, YES, YES, 0), "a comparison of booleans stays");
    check(instruction(5, 5, 77, STAYS, 0, 0, I_EQUAL, F, 2, KA, KB, 0), "a comparison that makes no boolean stays");

    /* the logical instructions on constants */
    static const struct {
        uint32_t op, a, b, value;
    } logical[] = {
        {LOGICAL_AND, YES, YES, 1},      {LOGICAL_AND, YES, NO, 0},      {LOGICAL_AND, NO, YES, 0},
        {LOGICAL_AND, NO, NO, 0},        {LOGICAL_OR, YES, YES, 1},      {LOGICAL_OR, YES, NO, 1},
        {LOGICAL_OR, NO, YES, 1},        {LOGICAL_OR, NO, NO, 0},        {LOGICAL_EQUAL, YES, YES, 1},
        {LOGICAL_EQUAL, YES, NO, 0},     {LOGICAL_EQUAL, NO, NO, 1},     {LOGICAL_NOT_EQUAL, YES, YES, 0},
        {LOGICAL_NOT_EQUAL, NO, YES, 1}, {LOGICAL_NOT_EQUAL, NO, NO, 0},
    };
    for (size_t k = 0; k < sizeof logical / sizeof *logical; k++)
        check(
            instruction(0, 0, 0, VALUE, BOOL, logical[k].value, logical[k].op, BOOL, 2, logical[k].a, logical[k].b, 0),
            "a logical instruction on two constants is true or false");
    check(instruction(0, 0, 0, VALUE, BOOL, 0, LOGICAL_NOT, BOOL, 1, YES, 0, 0) &&
              instruction(0, 0, 0, VALUE, BOOL, 1, LOGICAL_NOT, BOOL, 1, NO, 0, 0),
          "the negation of a constant is the other one");
    check(instruction(0, 0, 0, STAYS, 0, 0, LOGICAL_NOT, BOOL, 1, XB, 0, 0), "the negation of a value stays");
    check(instruction(0, 0, 0, STAYS, 0, 0, LOGICAL_NOT, BOOL, 1, U0, 0, 0) &&
              instruction(0, 0, 0, STAYS, 0, 0, LOGICAL_AND, BOOL, 2, U0, U1, 0),
          "a logical instruction on integer constants stays");
    check(instruction(0, 0, 0, STAYS, 0, 0, 163, BOOL, 2, YES, YES, 0), "the opcode before the logical ones stays");
    check(instruction(0, 0, 0, STAYS, 0, 0, 154, BOOL, 1, YES, 0, 0), "another instruction of one boolean stays");
    check(instruction(0, 0, 0, STAYS, 0, 0, LOGICAL_AND, F, 2, YES, NO, 0) &&
              instruction(0, 0, 0, STAYS, 0, 0, LOGICAL_NOT, F, 1, YES, 0, 0),
          "a logical instruction that makes no boolean stays");

    /* an operand that leaves the other as it is, or decides: on the right, and on the left of what commutes */
    static const struct {
        uint32_t op, a, b;
        int expect;
        uint32_t want;
    } identities[] = {
        {I_ADD, X, U0, IS, X},
        {I_ADD, U0, X, IS, X},
        {I_ADD, X, U1, STAYS, 0},
        {I_ADD, X, X, STAYS, 0},
        {I_SUB, X, U0, IS, X},
        {I_SUB, U0, X, STAYS, 0},
        {I_SUB, X, X, VALUE, 0},
        {I_SUB, X, Y, STAYS, 0},
        {I_MUL, X, U1, IS, X},
        {I_MUL, U1, X, IS, X},
        {I_MUL, X, U0, VALUE, 0},
        {I_MUL, U0, X, VALUE, 0},
        {I_MUL, X, ONES, STAYS, 0},
        {I_MUL, X, X, STAYS, 0},
        {BITWISE_AND, X, ONES, IS, X},
        {BITWISE_AND, ONES, X, IS, X},
        {BITWISE_AND, X, U0, VALUE, 0},
        {BITWISE_AND, U0, X, VALUE, 0},
        {BITWISE_AND, X, U1, STAYS, 0},
        {BITWISE_AND, X, X, IS, X},
        {BITWISE_AND, X, Y, STAYS, 0},
        {BITWISE_OR, X, U0, IS, X},
        {BITWISE_OR, U0, X, IS, X},
        {BITWISE_OR, X, ONES, VALUE, 0xffffffff},
        {BITWISE_OR, ONES, X, VALUE, 0xffffffff},
        {BITWISE_OR, X, U1, STAYS, 0},
        {BITWISE_OR, X, X, IS, X},
        {BITWISE_XOR, X, U0, IS, X},
        {BITWISE_XOR, U0, X, IS, X},
        {BITWISE_XOR, X, ONES, STAYS, 0},
        {BITWISE_XOR, X, X, VALUE, 0},
        {SHIFT_LEFT_LOGICAL, X, U0, IS, X},
        {SHIFT_LEFT_LOGICAL, U0, X, STAYS, 0},
        {SHIFT_LEFT_LOGICAL, X, U1, STAYS, 0},
        {SHIFT_LEFT_LOGICAL, X, X, STAYS, 0},
        {SHIFT_RIGHT_LOGICAL, X, U0, IS, X},
        {SHIFT_RIGHT_LOGICAL, U0, X, STAYS, 0},
        {SHIFT_RIGHT_ARITHMETIC, X, U0, IS, X},
        {SHIFT_RIGHT_ARITHMETIC, U0, X, STAYS, 0},
        {SHIFT_RIGHT_ARITHMETIC, X, ONES, STAYS, 0},
        {I_ADD, X, NIL, IS, X},     /* the null constant is zero */
        {I_ADD, X, SPEC, STAYS, 0}, /* a specialization constant is not known */
        {I_ADD, X, SHORT0, STAYS, 0},
        {I_ADD, X, F0, STAYS, 0},
        {I_ADD, X, S0, IS, X}, /* a zero of the other sign is zero */
        {I_ADD, XS, U0, STAYS, 0},
        {I_ADD, XF, U0, STAYS, 0},
        {BITWISE_AND, XS, XS, STAYS, 0},
        {BITWISE_AND, XS, ONES, STAYS, 0},
        {I_MUL, XS, U0, VALUE, 0},
        {I_SUB, XS, XS, VALUE, 0},
    };
    for (size_t k = 0; k < sizeof identities / sizeof *identities; k++)
        check(instruction(5, 3, 77, identities[k].expect, U, identities[k].want, identities[k].op, U, 2,
                          identities[k].a, identities[k].b, 0),
              "an integer instruction that is one of its operands or a constant, whatever the other operand is");
    /* the result's type must be the operand's: a signed sum of an unsigned value and zero stays, of a signed one
       is the value; a result of a type that is no 32-bit integer stays */
    check(instruction(5, 3, 77, STAYS, 0, 0, I_ADD, S, 2, X, U0, 0), "a signed sum of an unsigned value stays");
    check(instruction(5, 3, 77, IS, 0, XS, I_ADD, S, 2, XS, U0, 0), "a signed sum of a signed value is the value");
    check(instruction(5, 3, 77, STAYS, 0, 0, I_ADD, SHORT, 2, X, U0, 0) &&
              instruction(5, 3, 77, STAYS, 0, 0, I_ADD, F, 2, KA, KB, 0) &&
              instruction(5, 3, 77, STAYS, 0, 0, I_ADD, BOOL, 2, KA, KB, 0) &&
              instruction(5, 3, 77, STAYS, 0, 0, I_EQUAL, U, 2, KA, KB, 0) &&
              instruction(5, 3, 77, STAYS, 0, 0, NOT, BOOL, 1, KA, 0, 0) &&
              instruction(5, 3, 77, STAYS, 0, 0, LOGICAL_NOT, U, 1, YES, 0, 0) &&
              instruction(5, 3, 2, STAYS, 0, 0, BIT_FIELD_U_EXTRACT, BOOL, 3, KA, KB, KC) &&
              instruction(5, 3, 77, STAYS, 0, 0, LOGICAL_AND, U, 2, YES, NO, 0),
          "an instruction whose type is not what it makes stays");
    check(instruction(5, 3, 77, VALUE, S, 0, I_MUL, S, 2, X, U0, 0),
          "a signed product by zero is a signed zero, which the module had");

    /* the boolean ones */
    static const struct {
        uint32_t op, a, b;
        int expect;
        uint32_t want;
    } truths[] = {
        {LOGICAL_AND, XB, YES, IS, XB},        {LOGICAL_AND, YES, XB, IS, XB},    {LOGICAL_AND, XB, NO, VALUE, 0},
        {LOGICAL_AND, NO, XB, VALUE, 0},       {LOGICAL_AND, XB, XB, IS, XB},     {LOGICAL_OR, XB, NO, IS, XB},
        {LOGICAL_OR, NO, XB, IS, XB},          {LOGICAL_OR, XB, YES, VALUE, 1},   {LOGICAL_OR, YES, XB, VALUE, 1},
        {LOGICAL_OR, XB, XB, IS, XB},          {LOGICAL_EQUAL, XB, XB, STAYS, 0}, {LOGICAL_EQUAL, XB, YES, STAYS, 0},
        {LOGICAL_NOT_EQUAL, XB, NO, STAYS, 0}, {LOGICAL_AND, XB, U1, STAYS, 0},   {LOGICAL_AND, X, YES, STAYS, 0},
        {LOGICAL_OR, X, X, STAYS, 0},
    };
    for (size_t k = 0; k < sizeof truths / sizeof *truths; k++)
        check(instruction(5, 3, 77, truths[k].expect, BOOL, truths[k].want, truths[k].op, BOOL, 2, truths[k].a,
                          truths[k].b, 0),
              "a logical instruction that is one of its operands or a constant");

    /* a select: its condition a constant, or its two values one id; of any type, when the value has it */
    check(instruction(5, 3, 77, IS, 0, X, SELECT, U, 3, YES, X, Y), "a select on true is its first value");
    check(instruction(5, 3, 77, IS, 0, Y, SELECT, U, 3, NO, X, Y), "a select on false is its second");
    check(instruction(5, 3, 77, IS, 0, XF, SELECT, F, 3, XB, XF, XF), "a select of one value twice is the value");
    check(instruction(5, 3, 77, IS, 0, KA, SELECT, U, 3, YES, KA, X), "a select may be a constant");
    check(instruction(5, 3, 77, STAYS, 0, 0, SELECT, U, 3, XB, X, Y), "a select on a value stays");
    check(instruction(5, 3, 77, STAYS, 0, 0, SELECT, U, 3, U1, X, Y), "a select on an integer constant stays");
    check(instruction(5, 3, 77, STAYS, 0, 0, SELECT, S, 3, YES, X, XS), "a select is not a value of another type");
    check(instruction(5, 3, 77, IS, 0, XS, SELECT, S, 3, NO, X, XS), "the value it picks is the one that counts");
    check(instruction(5, 3, 77, STAYS, 0, 0, SELECT, S, 3, XB, X, X), "nor the one value of another type");

    /* a bitcast: to the type its operand has, of a constant, or back from a bitcast */
    check(instruction(5, 3, 77, IS, 0, X, BITCAST, U, 1, X, 0, 0), "a bitcast to the operand's type is the operand");
    check(instruction(5, 3, 77, IS, 0, XF, BITCAST, F, 1, XF, 0, 0), "of any type");
    check(instruction(5, 3, 77, VALUE, S, 5, BITCAST, S, 1, KA, 0, 0), "a bitcast of a constant is one of the type");
    check(instruction(5, 3, 77, STAYS, 0, 0, BITCAST, F, 1, KA, 0, 0) &&
              instruction(5, 3, 77, STAYS, 0, 0, BITCAST, S, 1, F0, 0, 0) &&
              instruction(5, 3, 77, STAYS, 0, 0, BITCAST, S, 1, X, 0, 0),
          "a bitcast to or from another type, or of a value, stays");
    begin(&m, 5, 3, 77);
    put(&m, BITCAST, 3, (uint32_t)U, (uint32_t)P, (uint32_t)XF);
    put(&m, BITCAST, 3, (uint32_t)F, (uint32_t)R, (uint32_t)P);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
    end(&m);
    check(outcome(&m, 4, IS, 0, XF), "a bitcast back to the type the first one's operand had is that operand");
    begin(&m, 5, 3, 77);
    put(&m, BITCAST, 3, (uint32_t)U, (uint32_t)P, (uint32_t)XF);
    put(&m, BITCAST, 3, (uint32_t)S, (uint32_t)R, (uint32_t)P);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
    end(&m);
    check(outcome(&m, 4, STAYS, 0, 0), "a bitcast on to a third type stays");
    begin(&m, 5, 3, 77);
    put(&m, S_NEGATE, 3, (uint32_t)U, (uint32_t)P, (uint32_t)XS);
    put(&m, BITCAST, 3, (uint32_t)S, (uint32_t)R, (uint32_t)P);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
    end(&m);
    check(outcome(&m, 4, STAYS, 0, 0), "a bitcast of what is no bitcast stays");

    /* a constant the module did not have is the instruction's id, declared before the function */
    begin(&m, 5, 3, 77);
    put(&m, I_ADD, 4, (uint32_t)U, (uint32_t)R, (uint32_t)KA, (uint32_t)KB);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
    end(&m);
    o = m;
    left = spv_opt_values(o.words, o.count);
    {
        size_t fn = 5;
        while (fn < left && (o.words[fn] & 0xffff) != FUNCTION) fn += o.words[fn] >> 16;
        check(left == m.count - 1 && stored(&o, left, 0) == R && constant(&o, left, R, U, 8),
              "a sum of 5 and 3 is a new constant 8 under the sum's id, a word shorter");
        check(fn >= 9 && o.words[fn - 4] == (4u << 16 | CONSTANT) && o.words[fn - 3] == U && o.words[fn - 2] == R &&
                  o.words[fn - 1] == 8,
              "declared last before the function");
        check(!memcmp(o.words, m.words, (fn - 4) * sizeof *m.words) &&
                  !memcmp(o.words + fn, m.words + fn - 4, 27 * sizeof *m.words) &&
                  !memcmp(o.words + fn + 27, m.words + fn - 4 + 27 + 5, (left - fn - 27) * sizeof *m.words),
              "with the declarations before it, the function's start and what followed the sum as they were");
        check(spv_opt_values(o.words, left) == left, "a second pass finds nothing");
    }
    /* a boolean one is three words, and a second instruction of the same value reads the first's */
    begin(&m, 5, 3, 77);
    m.words[5 + 4 + 4 + 2 + 2 + 4 + 4 * 3] = 3u << 16 | TYPE_VOID; /* YES and NO, made a type each: no boolean */
    m.words[5 + 4 + 4 + 2 + 2 + 4 + 4 * 3 + 1] = (uint32_t)YES;    /* constant is declared */
    m.words[5 + 4 + 4 + 2 + 2 + 4 + 4 * 3 + 3] = 3u << 16 | TYPE_VOID;
    m.words[5 + 4 + 4 + 2 + 2 + 4 + 4 * 3 + 4] = (uint32_t)NO;
    put(&m, U_GREATER_THAN, 4, (uint32_t)BOOL, (uint32_t)P, (uint32_t)KA, (uint32_t)KB);
    put(&m, I_NOT_EQUAL, 4, (uint32_t)BOOL, (uint32_t)Q, (uint32_t)KB, (uint32_t)KA);
    put(&m, U_LESS_THAN, 4, (uint32_t)BOOL, (uint32_t)R, (uint32_t)KA, (uint32_t)KB);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)P);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)Q);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
    end(&m);
    o = m;
    left = spv_opt_values(o.words, o.count);
    check(left == m.count - 15 + 3 + 3 && constant(&o, left, P, BOOL, 1) && constant(&o, left, R, BOOL, 0),
          "two comparisons become the true and the false the module lacked, three words each");
    check(stored(&o, left, 0) == P && stored(&o, left, 1) == P && stored(&o, left, 2) == R,
          "and a third of the first one's value reads the first");
    {
        size_t fn = 5;
        while (fn < left && (o.words[fn] & 0xffff) != FUNCTION) fn += o.words[fn] >> 16;
        check(fn >= 11 && o.words[fn - 6] == (3u << 16 | CONSTANT_TRUE) && o.words[fn - 4] == P &&
                  o.words[fn - 3] == (3u << 16 | CONSTANT_FALSE) && o.words[fn - 2] == BOOL && o.words[fn - 1] == R,
              "the two are declared in the instructions' order, last before the function");
    }
    /* a constant made is one the instructions after it fold with, and the table of what is kept sees through it */
    begin(&m, 5, 3, 77);
    put(&m, I_ADD, 4, (uint32_t)U, (uint32_t)P, (uint32_t)KA, (uint32_t)KB);
    put(&m, I_MUL, 4, (uint32_t)U, (uint32_t)Q, (uint32_t)P, (uint32_t)KB);
    put(&m, I_ADD, 4, (uint32_t)U, (uint32_t)R, (uint32_t)Q, (uint32_t)X);
    put(&m, I_ADD, 4, (uint32_t)U, (uint32_t)(R + 1), (uint32_t)X, (uint32_t)Q);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)(R + 1));
    end(&m);
    o = m;
    left = spv_opt_values(o.words, o.count);
    check(left == m.count - 15 + 8 && constant(&o, left, P, U, 8) && constant(&o, left, Q, U, 24) &&
              stored(&o, left, 0) == R && result(&o, left, R)[3] == Q,
          "8 times 3 is 24, and the two sums of it and a value are one");
    /* an operand replaced by a fold is what the next instruction is numbered with */
    begin(&m, 5, 3, 77);
    put(&m, I_ADD, 4, (uint32_t)U, (uint32_t)P, (uint32_t)X, (uint32_t)U0);
    put(&m, I_MUL, 4, (uint32_t)U, (uint32_t)Q, (uint32_t)X, (uint32_t)Y);
    put(&m, I_MUL, 4, (uint32_t)U, (uint32_t)R, (uint32_t)P, (uint32_t)Y);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
    end(&m);
    o = m;
    left = spv_opt_values(o.words, o.count);
    check(left == m.count - 10 && stored(&o, left, 0) == Q, "a product of x + 0 is the product of x");
    /* in a second function: the constant still goes before the first */
    begin(&m, 5, 3, 77);
    end(&m);
    put(&m, FUNCTION, 4, (uint32_t)F, (uint32_t)(FN + 40), 0u, (uint32_t)F);
    put(&m, LABEL, 1, (uint32_t)(L0 + 40));
    put(&m, I_ADD, 4, (uint32_t)U, (uint32_t)R, (uint32_t)KA, (uint32_t)KB);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
    end(&m);
    o = m;
    left = spv_opt_values(o.words, o.count);
    check(left == m.count - 1 && constant(&o, left, R, U, 8) && stored(&o, left, 0) == R,
          "a constant made in a second function is declared before the first");

    /* what the pass keeps apart is not folded: a decorated result, one an unknown instruction names */
    begin(&m, 5, 3, 77);
    put(&m, I_ADD, 4, (uint32_t)U, (uint32_t)R, (uint32_t)X, (uint32_t)U0);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
    end(&m);
    o = m;
    {
        /* the same module with a decoration of R put before its first type */
        Module d;
        d.count = 0;
        memcpy(d.words, m.words, 5 * sizeof *m.words);
        d.count = 5;
        put(&d, DECORATE, 2, (uint32_t)R, (uint32_t)NO_CONTRACTION);
        memcpy(d.words + d.count, m.words + 5, (m.count - 5) * sizeof *m.words);
        d.count += m.count - 5;
        check(outcome(&m, 5, IS, 0, X) && outcome(&d, 5, STAYS, 0, 0), "a decorated sum with zero stays");
        /* and with a name instead: the sum goes, and its name with it */
        d.words[5] = 3u << 16 | 5; /* OpName */
        left = spv_opt_values(d.words, d.count);
        check(left == m.count - 5 && stored(&d, left, 0) == X && (d.words[5] & 0xffff) == TYPE_INT,
              "the name of a sum that was its operand goes with it");
    }
    begin(&m, 5, 3, 77);
    put(&m, I_ADD, 4, (uint32_t)U, (uint32_t)R, (uint32_t)X, (uint32_t)U0);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
    put(&m, GROUP_NON_UNIFORM_BALLOT, 4, (uint32_t)U, (uint32_t)P, (uint32_t)X, (uint32_t)R);
    end(&m);
    check(outcome(&m, 5, STAYS, 0, 0), "a sum with zero that an unknown instruction names stays");
    /* the constant the module has is the one a fold reads: 5 & 3 is the 1 declared */
    begin(&m, 5, 3, 77);
    put(&m, BITWISE_AND, 4, (uint32_t)U, (uint32_t)R, (uint32_t)KA, (uint32_t)KB);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
    end(&m);
    o = m;
    left = spv_opt_values(o.words, o.count);
    check(left == m.count - 5 && stored(&o, left, 0) == U1,
          "a fold to a value the module declares reads that constant");
    /* Constants the table keeps side by side (the numbers are picked for that): of one type and two values (21 and
       its complement), and of one value and two types (U and a second signed type, id 72) */
    check(instruction(21, 0, 0, VALUE, U, 0xffffffea, NOT, U, 1, KA, 0, 0),
          "the complement of 21 is no constant the module has, in 21's slot of the table too");
    {
        Module d;
        begin(&m, 5, 3, 77);
        put(&m, BITCAST, 3, 72u, (uint32_t)R, (uint32_t)KA);
        put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
        end(&m);
        memcpy(d.words, m.words, 5 * sizeof *m.words);
        d.words[3] = 80; /* the bound */
        d.count = 5;
        put(&d, TYPE_INT, 3, 72u, 32u, 1u);
        memcpy(d.words + d.count, m.words + 5, (m.count - 5) * sizeof *m.words);
        d.count += m.count - 5;
        check(outcome(&d, 4, VALUE, 72, 5), "5 of another type is no constant the module has, in 5's slot too");
    }
    /* more constants than the table's first size holds */
    {
        Module d;
        begin(&m, 5, 3, 77);
        put(&m, I_ADD, 4, (uint32_t)U, (uint32_t)R, (uint32_t)KA, (uint32_t)KB);
        put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
        end(&m);
        memcpy(d.words, m.words, 5 * sizeof *m.words);
        d.count = 5;
        put(&d, TYPE_INT, 3, (uint32_t)U, 32u, 0u);
        for (uint32_t k = 0; k < 20; k++) put(&d, CONSTANT, 3, (uint32_t)U, 44 + k, 100 + k);
        memcpy(d.words + d.count, m.words + 5 + 4, (m.count - 5 - 4) * sizeof *m.words);
        d.count += m.count - 5 - 4;
        check(outcome(&d, 5, VALUE, U, 8), "a module of 32 constants folds as one of 12 does");
    }
    /* an operand that is no id of the module (a number past its bound) is no constant and no value */
    check(instruction(5, 3, 77, STAYS, 0, 0, I_ADD, U, 2, 0x7fffffff, U0, 0) &&
              instruction(5, 3, 77, STAYS, 0, 0, I_ADD, U, 2, U0, 0x7fffffff, 0) &&
              instruction(5, 3, 77, STAYS, 0, 0, BIT_FIELD_U_EXTRACT, U, 3, KA, KB, 0x7fffffff),
          "an instruction with an operand past the bound stays");
    begin(&m, 5, 3, 77);
    put(&m, BITCAST, 3, (uint32_t)U, (uint32_t)P, 0x7fffffffu);
    put(&m, BITCAST, 3, (uint32_t)F, (uint32_t)R, (uint32_t)P);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
    end(&m);
    check(outcome(&m, 4, STAYS, 0, 0), "a bitcast of a bitcast of one stays");
    /* constants of the wrong kind for an instruction are not its constants */
    check(instruction(5, 3, 77, STAYS, 0, 0, I_ADD, U, 2, YES, NO, 0) &&
              instruction(5, 3, 77, STAYS, 0, 0, I_ADD, U, 2, KA, YES, 0) &&
              instruction(5, 3, 77, STAYS, 0, 0, I_ADD, U, 2, YES, KA, 0) &&
              instruction(5, 3, 77, STAYS, 0, 0, NOT, U, 1, YES, 0, 0) &&
              instruction(5, 3, 2, STAYS, 0, 0, BIT_FIELD_U_EXTRACT, U, 3, KA, KB, YES) &&
              instruction(5, 3, 77, STAYS, 0, 0, I_EQUAL, BOOL, 2, KA, YES, 0) &&
              instruction(5, 3, 77, STAYS, 0, 0, BITCAST, S, 1, YES, 0, 0),
          "an integer instruction on a boolean constant stays");
    check(instruction(5, 3, 77, STAYS, 0, 0, LOGICAL_AND, BOOL, 2, YES, U1, 0) &&
              instruction(5, 3, 77, STAYS, 0, 0, LOGICAL_OR, BOOL, 2, U0, NO, 0) &&
              instruction(5, 3, 77, STAYS, 0, 0, LOGICAL_EQUAL, BOOL, 2, YES, U1, 0),
          "a logical instruction on an integer constant stays");
    check(instruction(5, 3, 77, STAYS, 0, 0, I_ADD, U, 2, X, F0, 0) &&
              instruction(5, 3, 77, STAYS, 0, 0, I_MUL, U, 2, X, SHORT0, 0),
          "a constant of a type that is no 32-bit integer is no zero");
    /* a type whose id is past the bound is no type the pass knows */
    begin(&m, 5, 3, 77);
    put(&m, I_ADD, 4, 0x7fffffffu, (uint32_t)R, (uint32_t)KA, (uint32_t)KB);
    put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
    end(&m);
    check(outcome(&m, 5, STAYS, 0, 0), "an instruction of a type past the module's bound stays");
    printf("spirv opt fold: %d checks failed\n", g_failures);
    return g_failures != 0;
}
