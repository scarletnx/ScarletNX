/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What spv_opt_values (common/utilities/spirv_opt.h) folds: an integer or boolean instruction on constants, one an
   operand decides or leaves as it is, a select and a bitcast that are one of their operands, and what the bits known
   of its operands decide. Each module here is a function that loads a value of each type, makes one instruction of
   them and of constants, and stores its result: the store then reads a constant, an operand, or the instruction that
   stayed. The known bits are checked twice: on the shapes the Maxwell translator writes, and on modules made at
   random, run before and after the pass on every value of their unknown bits. tests/spirv_opt_values.c has the
   numbering. */
#include "common/utilities/spirv_opt.h"
#include <stdarg.h>
#include <stdint.h>
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
    COPY_OBJECT = 83,
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
    KA = 20,     /* constants of U with a module's own values: KA to KJ, ten at most */
    KB = 21,
    KC = 22,
    KD = 23,
    KE = 24,
    KF = 25,
    KG = 26,
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
/* the header, the types, the constants (n of U with the values k, from KA on) and the function up to its loads */
static void begin_with(Module* m, const uint32_t* k, int n) {
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
    for (int i = 0; i < n; i++) put(m, CONSTANT, 3, (uint32_t)U, (uint32_t)KA + (uint32_t)i, k[i]);
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
/* with KA, KB and KC */
static void begin(Module* m, uint32_t ka, uint32_t kb, uint32_t kc) {
    const uint32_t k[3] = {ka, kb, kc};
    begin_with(m, k, 3);
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

/* The value of each store of the function in m (words long), when X loads x, Y loads y and XB loads (x + y) & 1,
   into out (max of them); a boolean is 0 or 1. Returns the stores, or -1 when an instruction reads an id nothing
   defined before it or is one this does not run */
static int run(const uint32_t* m, size_t words, uint32_t x, uint32_t y, uint32_t* out, int max) {
    uint32_t value[BOUND] = {0};
    uint8_t set[BOUND] = {0};
    int stores = 0;
    for (size_t i = 5; i < words; i += m[i] >> 16) {
        const uint32_t* w = m + i;
        uint32_t width = w[0] >> 16, op = w[0] & 0xffff, v = 0;
        if (op == CONSTANT || op == CONSTANT_TRUE || op == CONSTANT_FALSE || op == CONSTANT_NULL) {
            value[w[2]] = op == CONSTANT ? w[3] : op == CONSTANT_TRUE;
            set[w[2]] = 1;
            continue;
        }
        if (op == LOAD) {
            value[w[2]] = w[2] == X ? x : w[2] == Y ? y : w[2] == XB ? (x + y) & 1 : 0;
            set[w[2]] = 1;
            continue;
        }
        if (op == STORE) {
            if (w[2] >= BOUND || !set[w[2]] || stores == max) return -1;
            out[stores++] = value[w[2]];
            continue;
        }
        if (op < BITCAST || op == FUNCTION || op == FUNCTION_END || op == LABEL || op == RETURN) {
            if (op == COPY_OBJECT) break;
            continue; /* the declarations, the function's frame */
        }
        uint32_t a = 0, b = 0, c = 0, d = 0;
        for (uint32_t k = 3; k < width; k++) {
            if (w[k] >= BOUND || !set[w[k]]) return -1;
            uint32_t o = value[w[k]];
            if (k == 3) a = o;
            if (k == 4) b = o;
            if (k == 5) c = o;
            if (k == 6) d = o;
        }
        switch (op) {
            case I_ADD: v = a + b; break;
            case I_SUB: v = a - b; break;
            case I_MUL: v = a * b; break;
            case S_NEGATE: v = 0u - a; break;
            case NOT: v = ~a; break;
            case BITWISE_AND: v = a & b; break;
            case BITWISE_OR: v = a | b; break;
            case BITWISE_XOR: v = a ^ b; break;
            case SHIFT_LEFT_LOGICAL: v = a << b; break;
            case SHIFT_RIGHT_LOGICAL: v = a >> b; break;
            case SHIFT_RIGHT_ARITHMETIC: v = a >> b | (a >> 31 && b ? 0xffffffffu << (32 - b) : 0); break;
            case BIT_FIELD_U_EXTRACT:
            case BIT_FIELD_S_EXTRACT:
                v = c ? a >> b & 0xffffffffu >> (32 - c) : 0;
                if (op == BIT_FIELD_S_EXTRACT && c && c < 32 && v >> (c - 1)) v |= 0xffffffffu << c;
                break;
            case BIT_FIELD_INSERT: {
                uint32_t mask = d ? 0xffffffffu >> (32 - d) << c : 0;
                v = (a & ~mask) | (b << (c & 31) & mask);
                break;
            }
            case SELECT: v = a ? b : c; break;
            case BITCAST: v = a; break;
            case LOGICAL_EQUAL: v = a == b; break;
            case LOGICAL_NOT_EQUAL: v = a != b; break;
            case LOGICAL_OR: v = a || b; break;
            case LOGICAL_AND: v = a && b; break;
            case LOGICAL_NOT: v = !a; break;
            case I_EQUAL: v = a == b; break;
            case I_NOT_EQUAL: v = a != b; break;
            case U_GREATER_THAN: v = a > b; break;
            case S_GREATER_THAN: v = (int32_t)a > (int32_t)b; break;
            case U_GREATER_THAN_EQUAL: v = a >= b; break;
            case S_GREATER_THAN_EQUAL: v = (int32_t)a >= (int32_t)b; break;
            case U_LESS_THAN: v = a < b; break;
            case S_LESS_THAN: v = (int32_t)a < (int32_t)b; break;
            case U_LESS_THAN_EQUAL: v = a <= b; break;
            case S_LESS_THAN_EQUAL: v = (int32_t)a <= (int32_t)b; break;
            default: return -1;
        }
        value[w[2]] = v;
        set[w[2]] = 1;
    }
    return stores;
}
/* the next number of a fixed sequence (xorshift32), so that every run makes the same modules */
static uint32_t g_seed = 0x2545f491u;
static uint32_t next(void) {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}
/* a mask of up to four bits at random places: the bits of a value the module does not know */
static uint32_t unknown_bits(void) {
    uint32_t mask = 0, n = next() % 5;
    for (uint32_t k = 0; k < n; k++) mask |= 1u << (next() % 32);
    return mask;
}
/* m before and after the pass store the same values for every x within the bits of xs and every y within those of
   ys (each a subset of the mask's bits, all of them) */
static int same_stores(const Module* m, uint32_t xs, uint32_t ys) {
    Module o = *m;
    size_t left = spv_opt_values(o.words, o.count);
    uint32_t x = 0, y = 0;
    do {
        do {
            uint32_t want[8], got[8];
            int n = run(m->words, m->count, x, y, want, 8);
            if (n < 0 || run(o.words, left, x, y, got, 8) != n || memcmp(want, got, (size_t)n * sizeof *want)) {
                fprintf(stderr, "x %#x y %#x: %d stores before the pass, after it %d\n", x, y, n,
                        run(o.words, left, x, y, got, 8));
                return 0;
            }
            y = (y - ys) & ys; /* the next subset of ys */
        } while (y);
        x = (x - xs) & xs;
    } while (x);
    return 1;
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
              instruction(5, 5, 77, STAYS, 0, 0, I_EQUAL, BOOL, 2, X, Y, 0),
          "a comparison with a value stays");
    for (size_t k = 0; k < sizeof comparisons / sizeof *comparisons; k++)
        check(instruction(5, 5, 77, VALUE, BOOL, comparisons[k].truth[2], comparisons[k].op, BOOL, 2, X, X, 0) &&
                  instruction(5, 5, 77, VALUE, BOOL, comparisons[k].truth[2], comparisons[k].op, BOOL, 2, XS, XS, 0),
              "an integer compared with itself is what 5 compared with 5 is");
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

    /* What the known bits of the operands decide. The Maxwell translator splits a constant buffer address made of a
       register shifted by 4 plus an offset into a vec4's index and a component: (x << 4) + 480 >> 2 & 3 has its two
       low bits known 0 */
    {
        const uint32_t k[] = {4, 480, 2, 3, 484};
        begin_with(&m, k, 5);
        put(&m, SHIFT_LEFT_LOGICAL, 4, (uint32_t)U, (uint32_t)P, (uint32_t)X, (uint32_t)KA);
        put(&m, I_ADD, 4, (uint32_t)U, (uint32_t)Q, (uint32_t)P, (uint32_t)KB);
        put(&m, SHIFT_RIGHT_LOGICAL, 4, (uint32_t)U, (uint32_t)(R + 1), (uint32_t)Q, (uint32_t)KC);
        put(&m, BITWISE_AND, 4, (uint32_t)U, (uint32_t)R, (uint32_t)(R + 1), (uint32_t)KD);
        put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
        end(&m);
        check(outcome(&m, 5, VALUE, U, 0), "the component of (x << 4) + 480 is 0");
        size_t sum = m.count - 2 - 3 - 5 - 5 - 5; /* the sum's first word: the end, the store, the mask, the shift */
        m.words[sum + 4] = (uint32_t)KE;          /* + 484 */
        check(outcome(&m, 5, VALUE, U, 1), "and of (x << 4) + 484 is 1");
        m.words[sum + 4] = (uint32_t)Y; /* (x << 4) + y */
        check(outcome(&m, 5, STAYS, 0, 0), "and a sum with a value stays");
    }
    /* one value whose bits two masks set, P = (x & KA) | KB (without the and when KA is ~0, nor the or when KB is 0:
       the pass would fold them), and an instruction of it and the constant KC, R */
    static const struct {
        uint32_t op, type, ka, kb, n;
        int expect;
        uint32_t want; /* the value, or the id it reads */
        const char* what;
    } known[] = {
        {BITWISE_AND, U, 0xf0, 0, 0x0f, VALUE, 0, "(x & 0xf0) & 0x0f is 0"},
        {BITWISE_AND, U, 0xf0, 0, 0xff, IS, P, "(x & 0xf0) & 0xff is x & 0xf0"},
        {BITWISE_AND, U, 0xf0, 0, 0x3f, STAYS, 0, "(x & 0xf0) & 0x3f stays"},
        {BITWISE_AND, U, 0xff, 0x10, 0x10, VALUE, 0x10, "a known 1 and 0x10 is 0x10"},
        {BITWISE_OR, U, 0xffffff00, 0xff, 0x0f, IS, P, "(x | 0xff) | 0x0f is x | 0xff"},
        {BITWISE_OR, U, 0xffffff00, 0x0f, 0xff, STAYS, 0, "(x | 0x0f) | 0xff stays"},
        {BITWISE_OR, U, 0xffffffff, 0xf0, 0xffffff0f, VALUE, 0xffffffff, "(x | 0xf0) | ~0xf0 is ~0"},
        {BITWISE_OR, U, 0xf0, 0, 0xffffff0f, STAYS, 0, "(x & 0xf0) | ~0xf0 stays"},
        {BITWISE_XOR, U, 0x0f, 0x30, 0x30, STAYS, 0, "a xor of a value stays"},
        {BITWISE_XOR, U, 0x0f, 0x30, 0x0f, STAYS, 0, "and with its unknown bits too"},
        {BITWISE_AND, U, 0xffffffff, 0x0f, 0xfffffff0, STAYS, 0, "(x | 0x0f) & ~0x0f stays"},
        {I_ADD, U, 0xfffffff0, 8, 0x10, STAYS, 0, "(x & ~15 | 8) + 16 stays"},
        {I_SUB, U, 0xfffffff0, 0, 1, STAYS, 0, "(x & ~15) - 1 stays"},
        {SHIFT_LEFT_LOGICAL, U, 0xffff, 0, 16, STAYS, 0, "(x & 0xffff) << 16 stays"},
        {SHIFT_LEFT_LOGICAL, U, 0xffff, 0, 31, STAYS, 0, "(x & 0xffff) << 31 stays"},
        {SHIFT_LEFT_LOGICAL, U, 0xffff0000, 0, 16, VALUE, 0, "(x & 0xffff0000) << 16 is 0"},
        {SHIFT_RIGHT_LOGICAL, U, 0xffff, 0, 16, VALUE, 0, "(x & 0xffff) >> 16 is 0"},
        {SHIFT_RIGHT_LOGICAL, U, 0xffff, 0, 15, STAYS, 0, "(x & 0xffff) >> 15 stays"},
        {SHIFT_RIGHT_ARITHMETIC, U, 0x7fffffff, 0, 31, VALUE, 0, "a value with its sign 0 >> 31 is 0"},
        {SHIFT_RIGHT_ARITHMETIC, U, 0x7fff0000, 0x80000000, 31, VALUE, 0xffffffff, "one with its sign 1 is ~0"},
        {SHIFT_RIGHT_ARITHMETIC, U, 0x7fff0000, 0x80000000, 16, STAYS, 0, "a negative value >> 16 stays"},
        {SHIFT_RIGHT_ARITHMETIC, U, 0xffffffff, 0, 31, STAYS, 0, "x >> 31 stays: its sign is unknown"},
        {U_LESS_THAN, BOOL, 0xff, 0, 0x100, VALUE, 1, "(x & 0xff) < 256"},
        {U_LESS_THAN, BOOL, 0xff, 0, 0xff, STAYS, 0, "(x & 0xff) < 255 stays"},
        {U_GREATER_THAN_EQUAL, BOOL, 0xff, 0x100, 0x100, VALUE, 1, "(x & 0xff | 0x100) >= 256"},
        {U_GREATER_THAN_EQUAL, BOOL, 0xff, 0x100, 0x101, STAYS, 0, "it may be 256"},
        {U_LESS_THAN_EQUAL, BOOL, 0xff, 0, 0xff, VALUE, 1, "(x & 0xff) <= 255"},
        {U_LESS_THAN_EQUAL, BOOL, 0xff, 0x10, 0x0f, VALUE, 0, "(x & 0xff | 0x10) <= 15 is false"},
        {U_GREATER_THAN, BOOL, 0xff, 0, 0xff, VALUE, 0, "(x & 0xff) > 255 is false"},
        {U_GREATER_THAN, BOOL, 0xff, 0x10, 0x0f, VALUE, 1, "(x & 0xff | 0x10) > 15"},
        {S_GREATER_THAN, BOOL, 0x7fffffff, 0, 0xffffffff, VALUE, 1, "a value with its sign 0 is > -1"},
        {S_GREATER_THAN_EQUAL, BOOL, 0x7fffffff, 0, 0, VALUE, 1, "and >= 0"},
        {S_LESS_THAN, BOOL, 0x7fffffff, 0, 0, VALUE, 0, "and not < 0"},
        {S_LESS_THAN, BOOL, 0xff, 0x80000000, 0, VALUE, 1, "one with its sign 1 is < 0"},
        {S_LESS_THAN, BOOL, 0xff, 0x80000000, 0xffffff00, VALUE, 1, "and < -256, from -2^31 to -2^31 + 255"},
        {S_LESS_THAN, BOOL, 0xff, 0x80000000, 0x80000010, STAYS, 0, "but may be < -2^31 + 16 or not"},
        {S_LESS_THAN_EQUAL, BOOL, 0xffffffff, 0, 0x7fffffff, VALUE, 1, "any x is <= INT_MAX"},
        {S_LESS_THAN_EQUAL, BOOL, 0xffffffff, 0, 0x7ffffffe, STAYS, 0, "x <= INT_MAX - 1 stays"},
        {U_GREATER_THAN_EQUAL, BOOL, 0xffffffff, 0, 0, VALUE, 1, "any x is >= 0 unsigned"},
        {U_LESS_THAN, BOOL, 0xffffffff, 0, 0xffffffff, STAYS, 0, "x < ~0 stays"},
        {I_EQUAL, BOOL, 0xfffffffe, 1, 0x10, VALUE, 0, "an odd value is not 16"},
        {I_NOT_EQUAL, BOOL, 0xfffffffe, 1, 0x10, VALUE, 1, "and differs from 16"},
        {I_EQUAL, BOOL, 0xfffffffe, 1, 0x11, STAYS, 0, "it may be 17"},
        {I_EQUAL, BOOL, 0xfe, 0, 1, VALUE, 0, "an even value is not 1"},
        {U_LESS_THAN_EQUAL, BOOL, 0xff, 0x10, 0x10, STAYS, 0, "(x & 0xff | 0x10) <= 16 stays: it may be 16"},
        {U_GREATER_THAN_EQUAL, BOOL, 0xff, 0x10, 0x10, VALUE, 1, "and it is >= 16"},
        {S_GREATER_THAN_EQUAL, BOOL, 0x7fffffff, 0, 0x7fffffff, STAYS, 0, "x & INT_MAX >= INT_MAX stays"},
        {S_GREATER_THAN, BOOL, 0x0f, 0x80000000, 0x80000000, STAYS, 0, "a negative value > INT_MIN stays"},
        {S_GREATER_THAN, BOOL, 0x0f, 0x80000000, 0x80000010, VALUE, 0, "but not > INT_MIN + 16"},
    };
    for (size_t k = 0; k < sizeof known / sizeof *known; k++) {
        const uint32_t c[] = {known[k].ka, known[k].kb, known[k].n};
        uint32_t p = (uint32_t)X;
        begin_with(&m, c, 3);
        if (known[k].ka != 0xffffffffu) {
            put(&m, BITWISE_AND, 4, (uint32_t)U, (uint32_t)Q, p, (uint32_t)KA);
            p = (uint32_t)Q;
        }
        if (known[k].kb) {
            put(&m, BITWISE_OR, 4, (uint32_t)U, (uint32_t)P, p, (uint32_t)KB);
            p = (uint32_t)P;
        }
        put(&m, known[k].op, 4, known[k].type, (uint32_t)R, p, (uint32_t)KC);
        put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
        end(&m);
        int ok = outcome(&m, 5, known[k].expect, known[k].type, known[k].want == P ? p : known[k].want);
        if (ok && known[k].expect == VALUE && known[k].type == BOOL) { /* the module's own true or false */
            o = m;
            left = spv_opt_values(o.words, o.count);
            ok = stored(&o, left, 0) == (known[k].want ? (uint32_t)YES : (uint32_t)NO);
        }
        if (!ok) fprintf(stderr, "known bits: %s\n", known[k].what);
        check(ok, "an instruction its operands' known bits decide, or leave");
    }
    /* the carries of a sum and a difference: (x << 4) + 8 has bit 3 known 1 and bits 0 to 2 known 0, (x << 4) + 24
       its bit 4 unknown, (x << 4) - 1 its four low bits 1, and -(x << 4) and (x << 2) * (y << 3) their low 4 and 5
       bits 0 */
    {
        static const struct {
            uint32_t op, shift, add, mask;
            int expect;
            uint32_t want;
        } carries[] = {
            {I_ADD, 4, 8, 15, VALUE, 8},    {I_ADD, 4, 24, 15, VALUE, 8},          {I_ADD, 4, 24, 31, STAYS, 0},
            {I_SUB, 4, 1, 15, VALUE, 15},   {I_SUB, 4, 1, 31, STAYS, 0},           {S_NEGATE, 4, 0, 15, VALUE, 0},
            {S_NEGATE, 4, 0, 31, STAYS, 0}, {I_MUL, 2, 3, 31, VALUE, 0},           {I_MUL, 2, 3, 63, STAYS, 0},
            {NOT, 4, 0, 15, VALUE, 15},     {BITCAST, 4, 0, 15, VALUE, 0},         {I_MUL, 1, 1, 3, VALUE, 0},
            {I_MUL, 1, 1, 7, STAYS, 0},     {BITWISE_XOR, 4, 0x0f, 15, VALUE, 15}, {BITWISE_XOR, 4, 0x0f, 31, STAYS, 0},
        };
        /* a product's high bits: below 2^16 times below 2^1 is below 2^17, and (x & 0xffff) * (y & 1) & 0xffff
           keeps it whole only where the second is 0 or 1 */
        {
            const uint32_t c[] = {0xffff, 1, 0xfffe0000, 0x1ffff, 3};
            begin_with(&m, c, 5);
            put(&m, BITWISE_AND, 4, (uint32_t)U, (uint32_t)P, (uint32_t)X, (uint32_t)KA);
            put(&m, BITWISE_AND, 4, (uint32_t)U, (uint32_t)Q, (uint32_t)Y, (uint32_t)KB);
            put(&m, I_MUL, 4, (uint32_t)U, (uint32_t)(R + 1), (uint32_t)P, (uint32_t)Q);
            put(&m, BITWISE_AND, 4, (uint32_t)U, (uint32_t)R, (uint32_t)(R + 1), (uint32_t)KC);
            put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
            end(&m);
            check(outcome(&m, 5, VALUE, U, 0), "(x & 0xffff) * (y & 1) has no bit from 17 up");
            size_t mask = m.count - 2 - 3 - 5;
            m.words[mask + 4] = (uint32_t)KD;
            check(outcome(&m, 5, IS, 0, R + 1), "and is its own low 17 bits");
            m.words[mask - 5 - 5 + 4] = (uint32_t)KE; /* y & 3 */
            check(outcome(&m, 5, STAYS, 0, 0), "(x & 0xffff) * (y & 3) may reach bit 17");
        }
        for (size_t k = 0; k < sizeof carries / sizeof *carries; k++) {
            const uint32_t c[] = {carries[k].shift, carries[k].add, carries[k].mask};
            begin_with(&m, c, 3);
            put(&m, SHIFT_LEFT_LOGICAL, 4, (uint32_t)U, (uint32_t)P, (uint32_t)X, (uint32_t)KA);
            if (carries[k].op == I_MUL && carries[k].add == 1) { /* (x << 1) * (y << 1): two low zeros */
                put(&m, SHIFT_LEFT_LOGICAL, 4, (uint32_t)U, (uint32_t)Q, (uint32_t)Y, (uint32_t)KB);
                put(&m, I_MUL, 4, (uint32_t)U, (uint32_t)(R + 1), (uint32_t)P, (uint32_t)Q);
            } else if (carries[k].op == I_MUL) {
                put(&m, SHIFT_LEFT_LOGICAL, 4, (uint32_t)U, (uint32_t)Q, (uint32_t)Y, (uint32_t)KB);
                put(&m, I_MUL, 4, (uint32_t)U, (uint32_t)(R + 1), (uint32_t)P, (uint32_t)Q);
            } else if (carries[k].op == S_NEGATE || carries[k].op == NOT || carries[k].op == BITCAST)
                put(&m, carries[k].op, 3, carries[k].op == BITCAST ? (uint32_t)S : (uint32_t)U, (uint32_t)(R + 1),
                    (uint32_t)P);
            else
                put(&m, carries[k].op, 4, (uint32_t)U, (uint32_t)(R + 1), (uint32_t)P, (uint32_t)KB);
            put(&m, BITWISE_AND, 4, (uint32_t)U, (uint32_t)R, (uint32_t)(R + 1), (uint32_t)KC);
            put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
            end(&m);
            int ok = outcome(&m, 5, carries[k].expect, U, carries[k].want);
            if (!ok) fprintf(stderr, "known bits: case %zu of the carries\n", k);
            check(ok, "the low bits of a sum, a difference, a negation, a product, a complement and a bitcast");
        }
    }
    /* a field: of a value masked to 16 bits, bits 16 up are 0 and bits 0 up are the value; signed, a field whose sign
       bit is known 0 is the value too */
    {
        static const struct {
            uint32_t op, mask, offset, count;
            int expect;
            uint32_t want;
        } fields_known[] = {
            {BIT_FIELD_U_EXTRACT, 0xffff, 16, 16, VALUE, 0}, {BIT_FIELD_U_EXTRACT, 0xffff, 0, 16, IS, P},
            {BIT_FIELD_U_EXTRACT, 0xffff, 0, 15, STAYS, 0},  {BIT_FIELD_U_EXTRACT, 0xffff, 8, 16, STAYS, 0},
            {BIT_FIELD_S_EXTRACT, 0x7fff, 0, 16, IS, P},     {BIT_FIELD_S_EXTRACT, 0xffff, 0, 16, STAYS, 0},
            {BIT_FIELD_S_EXTRACT, 0xffff, 16, 16, VALUE, 0}, {BIT_FIELD_U_EXTRACT, 0x7fffffff, 0, 32, IS, P},
            {BIT_FIELD_S_EXTRACT, 0xffff, 4, 0, VALUE, 0},
        };
        /* the bits past a field: 0 unsigned, the field's sign bit signed (known 1 here: bit 7 is) */
        {
            const uint32_t c[] = {0x80, 4, 4, 0xfffffff0};
            begin_with(&m, c, 4);
            put(&m, BIT_FIELD_U_EXTRACT, 5, (uint32_t)U, (uint32_t)(R + 1), (uint32_t)X, (uint32_t)KB, (uint32_t)KC);
            put(&m, BITWISE_AND, 4, (uint32_t)U, (uint32_t)R, (uint32_t)(R + 1), (uint32_t)KD);
            put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
            end(&m);
            check(outcome(&m, 5, VALUE, U, 0), "a 4-bit field of x has no bit from 4 up");
            begin_with(&m, c, 4);
            put(&m, BITWISE_OR, 4, (uint32_t)U, (uint32_t)P, (uint32_t)X, (uint32_t)KA);
            put(&m, BIT_FIELD_S_EXTRACT, 5, (uint32_t)U, (uint32_t)(R + 1), (uint32_t)P, (uint32_t)KB, (uint32_t)KC);
            put(&m, BITWISE_AND, 4, (uint32_t)U, (uint32_t)R, (uint32_t)(R + 1), (uint32_t)KD);
            put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
            end(&m);
            check(outcome(&m, 5, VALUE, U, 0xfffffff0), "a signed 4-bit field of x | 0x80 has every bit from 4 up");
        }
        for (size_t k = 0; k < sizeof fields_known / sizeof *fields_known; k++) {
            const uint32_t c[] = {fields_known[k].mask, fields_known[k].offset, fields_known[k].count};
            begin_with(&m, c, 3);
            put(&m, BITWISE_AND, 4, (uint32_t)U, (uint32_t)P, (uint32_t)X, (uint32_t)KA);
            put(&m, fields_known[k].op, 5, (uint32_t)U, (uint32_t)R, (uint32_t)P, (uint32_t)KB, (uint32_t)KC);
            put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
            end(&m);
            int ok = outcome(&m, 6, fields_known[k].expect, U, fields_known[k].want);
            if (!ok) fprintf(stderr, "known bits: case %zu of the fields\n", k);
            check(ok, "a field of a value with known bits");
        }
    }
    /* 0 > (x & 0x7fffffff) as a signed integer is false, and a select on it is its second value: the absolute value
       of what cannot be negative */
    {
        const uint32_t k[] = {0x7fffffff};
        begin_with(&m, k, 1);
        put(&m, BITWISE_AND, 4, (uint32_t)U, (uint32_t)P, (uint32_t)X, (uint32_t)KA);
        put(&m, BITCAST, 3, (uint32_t)S, (uint32_t)Q, (uint32_t)P);
        put(&m, S_GREATER_THAN, 4, (uint32_t)BOOL, (uint32_t)(R + 1), (uint32_t)S0, (uint32_t)Q);
        put(&m, I_SUB, 4, (uint32_t)U, (uint32_t)(R + 2), (uint32_t)U0, (uint32_t)P);
        put(&m, SELECT, 5, (uint32_t)U, (uint32_t)R, (uint32_t)(R + 1), (uint32_t)(R + 2), (uint32_t)P);
        put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
        end(&m);
        o = m;
        left = spv_opt_values(o.words, o.count);
        check(stored(&o, left, 0) == P && left == m.count - 5 - 6,
              "the absolute value of x & 0x7fffffff is x & 0x7fffffff, its comparison gone");
    }
    /* a field inserted into a value with its low byte known 1s: bits 0 and 1 stay 1, bits 2 and 3 are the field's */
    {
        const uint32_t k[] = {0xff, 3, 2, 0x3, 0xf};
        begin_with(&m, k, 5);
        put(&m, BITWISE_OR, 4, (uint32_t)U, (uint32_t)P, (uint32_t)X, (uint32_t)KA);
        put(&m, BITWISE_AND, 4, (uint32_t)U, (uint32_t)Q, (uint32_t)Y, (uint32_t)KB);
        put(&m, BIT_FIELD_INSERT, 6, (uint32_t)U, (uint32_t)(R + 1), (uint32_t)P, (uint32_t)Q, (uint32_t)KC,
            (uint32_t)KC);
        put(&m, BITWISE_AND, 4, (uint32_t)U, (uint32_t)R, (uint32_t)(R + 1), (uint32_t)KD);
        put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
        end(&m);
        check(outcome(&m, 5, VALUE, U, 3), "an insert at bit 2 keeps the two low 1s");
        m.words[m.count - 2 - 3 - 5 + 4] = (uint32_t)KE;
        check(outcome(&m, 5, STAYS, 0, 0), "and leaves bits 2 and 3 unknown");
    }
    {
        const uint32_t k[] = {4, 0xf0, 0x3fffffff, 30, 0xff};
        begin_with(&m, k, 5);
        put(&m, BIT_FIELD_INSERT, 6, (uint32_t)U, (uint32_t)(R + 1), (uint32_t)X, (uint32_t)U0, (uint32_t)KA,
            (uint32_t)KA);
        put(&m, BITWISE_AND, 4, (uint32_t)U, (uint32_t)R, (uint32_t)(R + 1), (uint32_t)KB);
        put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
        end(&m);
        check(outcome(&m, 5, VALUE, U, 0), "a zero inserted at bit 4 makes bits 4 to 7 0");
        begin_with(&m, k, 5);
        put(&m, BITWISE_OR, 4, (uint32_t)U, (uint32_t)P, (uint32_t)X, (uint32_t)KC);
        put(&m, BIT_FIELD_INSERT, 6, (uint32_t)U, (uint32_t)(R + 1), (uint32_t)P, (uint32_t)Y, (uint32_t)KD,
            (uint32_t)KA);
        put(&m, BITWISE_AND, 4, (uint32_t)U, (uint32_t)R, (uint32_t)(R + 1), (uint32_t)KC);
        put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
        end(&m);
        check(outcome(&m, 5, STAYS, 0, 0), "an insert past bit 32, which has no value, stays unknown");
    }
    /* the constant on the left of an and; a constant the pass made, whose bits it knows; a decorated instruction */
    {
        const uint32_t k[] = {0xf0, 0xff, 0x70, 0x81, 0x0f, 0x0e};
        begin_with(&m, k, 6);
        put(&m, BITWISE_AND, 4, (uint32_t)U, (uint32_t)P, (uint32_t)X, (uint32_t)KA);
        put(&m, BITWISE_AND, 4, (uint32_t)U, (uint32_t)R, (uint32_t)KB, (uint32_t)P);
        put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
        end(&m);
        check(outcome(&m, 5, IS, 0, P), "0xff & (x & 0xf0) is x & 0xf0");
        begin_with(&m, k, 6);
        put(&m, I_ADD, 4, (uint32_t)U, (uint32_t)Q, (uint32_t)KC, (uint32_t)KD); /* 0xf1, which no constant is */
        put(&m, BITWISE_AND, 4, (uint32_t)U, (uint32_t)P, (uint32_t)X, (uint32_t)Q);
        put(&m, BITWISE_AND, 4, (uint32_t)U, (uint32_t)R, (uint32_t)P, (uint32_t)KF);
        put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
        end(&m);
        o = m;
        left = spv_opt_values(o.words, o.count);
        check(constant(&o, left, Q, U, 0xf1) && stored(&o, left, 0) == U0,
              "x & (0x70 + 0x81) & 0x0e is 0: the sum made a constant whose bits are known");
        begin_with(&m, k, 6);
        put(&m, I_ADD, 4, (uint32_t)U, (uint32_t)Q, (uint32_t)KC, (uint32_t)KD);
        put(&m, BITWISE_OR, 4, (uint32_t)U, (uint32_t)P, (uint32_t)X, (uint32_t)Q);
        put(&m, BITWISE_AND, 4, (uint32_t)U, (uint32_t)R, (uint32_t)P, (uint32_t)U1);
        put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
        end(&m);
        o = m;
        left = spv_opt_values(o.words, o.count);
        check(stored(&o, left, 0) == U1, "and (x | (0x70 + 0x81)) & 1 is 1");
        /* a xor: of a known 1 with a 1 is 0, of a known 1 with a 0 is 1 */
        begin_with(&m, k, 6);
        put(&m, BITWISE_OR, 4, (uint32_t)U, (uint32_t)P, (uint32_t)X, (uint32_t)KE);
        put(&m, BITWISE_XOR, 4, (uint32_t)U, (uint32_t)Q, (uint32_t)P, (uint32_t)KE);
        put(&m, BITWISE_AND, 4, (uint32_t)U, (uint32_t)R, (uint32_t)Q, (uint32_t)KE);
        put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
        end(&m);
        check(outcome(&m, 5, VALUE, U, 0), "(x | 0x0f) ^ 0x0f has its low 4 bits 0");
        m.words[m.count - 2 - 3 - 5 - 5 + 4] = (uint32_t)KA;
        check(outcome(&m, 5, VALUE, U, 0x0f), "(x | 0x0f) ^ 0xf0 has them 1");
        Module d;
        begin_with(&m, k, 6);
        put(&m, BITWISE_AND, 4, (uint32_t)U, (uint32_t)P, (uint32_t)X, (uint32_t)KA);
        put(&m, BITWISE_AND, 4, (uint32_t)U, (uint32_t)R, (uint32_t)P, (uint32_t)KE);
        put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
        end(&m);
        check(outcome(&m, 5, VALUE, U, 0), "(x & 0xf0) & 0x0f is 0");
        memcpy(d.words, m.words, 5 * sizeof *m.words);
        d.count = 5;
        put(&d, DECORATE, 2, (uint32_t)R, (uint32_t)NO_CONTRACTION);
        memcpy(d.words + d.count, m.words + 5, (m.count - 5) * sizeof *m.words);
        d.count += m.count - 5;
        check(outcome(&d, 5, STAYS, 0, 0), "and stays decorated");
    }
    /* what nothing is known of: a phi's value, a load's, an operand of another kind or past the bound */
    {
        const uint32_t k[] = {0xf0, 0x0f};
        begin_with(&m, k, 2);
        put(&m, BITWISE_AND, 4, (uint32_t)U, (uint32_t)P, (uint32_t)X, (uint32_t)KA);
        put(&m, BITCAST, 3, (uint32_t)U, (uint32_t)Q, (uint32_t)XF);
        put(&m, BITWISE_AND, 4, (uint32_t)U, (uint32_t)R, (uint32_t)Q, (uint32_t)KB);
        put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R);
        end(&m);
        check(outcome(&m, 5, STAYS, 0, 0), "a bitcast of a float is unknown");
        m.words[m.count - 2 - 3 - 5 + 3] = (uint32_t)(BOUND + U0); /* past the bound by the number of a zero */
        check(outcome(&m, 5, STAYS, 0, 0), "and an operand past the bound");
    }
    /* Modules at random: two values whose unknown bits are a few random ones (P from x, Q from y), an instruction of
       them or of constants (R), a second one of R (R + 1), and a mask of that (R + 2), all stored; each module runs
       the same before and after the pass for every x and y of those bits */
    {
        static const uint32_t ops[] = {I_ADD,
                                       I_SUB,
                                       I_MUL,
                                       BITWISE_AND,
                                       BITWISE_OR,
                                       BITWISE_XOR,
                                       S_NEGATE,
                                       NOT,
                                       SHIFT_LEFT_LOGICAL,
                                       SHIFT_RIGHT_LOGICAL,
                                       SHIFT_RIGHT_ARITHMETIC,
                                       BIT_FIELD_U_EXTRACT,
                                       BIT_FIELD_S_EXTRACT,
                                       BIT_FIELD_INSERT,
                                       SELECT,
                                       BITCAST,
                                       I_EQUAL,
                                       I_NOT_EQUAL,
                                       U_GREATER_THAN,
                                       S_GREATER_THAN,
                                       U_GREATER_THAN_EQUAL,
                                       S_GREATER_THAN_EQUAL,
                                       U_LESS_THAN,
                                       S_LESS_THAN,
                                       U_LESS_THAN_EQUAL,
                                       S_LESS_THAN_EQUAL};
        int failed = 0, folded = 0;
        const int rounds = 4000;
        for (int round = 0; round < rounds && failed < 5; round++) {
            uint32_t xs = unknown_bits(), ys = unknown_bits(), k[10];
            /* (x & xs) | known 1s outside xs, the same of y, the 1s as many as a quarter, half or three quarters of
               the bits, or none, and xs and ys sometimes low or high bits only (a product's low zeros, a field's
               high ones); then shift amounts, field offsets and counts, masks */
            if (next() % 4 == 0) xs &= next() % 2 ? 0xffffu : 0xffff0000u;
            if (next() % 4 == 0) ys = next() % 2 ? ys << 4 : ys >> 4;
            k[0] = xs;
            k[1] = next() & ~xs & (next() % 4 == 0 ? 0 : next() % 3 == 0 ? next() & next() : next() | next());
            k[2] = ys;
            k[3] = next() & ~ys & (next() % 4 == 0 ? 0 : next() % 3 == 0 ? next() & next() : next() | next());
            uint32_t count = next() % 33, offset = next() % (33 - count);
            k[4] = next() % 32;
            k[5] = offset;
            k[6] = count;
            k[7] = next() & (next() | next());
            k[8] = next() % 4 ? next() : k[1] ^ k[3];
            k[9] = next() % 2 ? 1u << (next() % 32) : (next() & 0xff) << (next() % 24);
            begin_with(&m, k, 10);
            put(&m, BITWISE_AND, 4, (uint32_t)U, (uint32_t)(P + 8), (uint32_t)X, (uint32_t)KA);
            put(&m, BITWISE_OR, 4, (uint32_t)U, (uint32_t)P, (uint32_t)(P + 8), (uint32_t)KB);
            put(&m, BITWISE_AND, 4, (uint32_t)U, (uint32_t)(Q + 8), (uint32_t)Y, (uint32_t)KC);
            put(&m, BITWISE_OR, 4, (uint32_t)U, (uint32_t)Q, (uint32_t)(Q + 8), (uint32_t)(KC + 1));
            uint32_t ids[3] = {(uint32_t)R, (uint32_t)R + 1, (uint32_t)R + 2}, prev = (uint32_t)P;
            int bool_result = 0;
            for (int level = 0; level < 2 && !bool_result; level++) {
                uint32_t op = ops[next() % (sizeof ops / sizeof *ops)], id = ids[level];
                /* the other operand: Q, P itself, a constant, or the field or shift amounts */
                uint32_t other = next() % 4 == 0 ? prev : next() % 3 == 0 ? (uint32_t)KA + 8 : (uint32_t)Q;
                if (op == SHIFT_LEFT_LOGICAL || op == SHIFT_RIGHT_LOGICAL || op == SHIFT_RIGHT_ARITHMETIC)
                    put(&m, op, 4, (uint32_t)U, id, prev, (uint32_t)KA + 4);
                else if (op == BIT_FIELD_U_EXTRACT || op == BIT_FIELD_S_EXTRACT)
                    put(&m, op, 5, (uint32_t)U, id, prev, (uint32_t)KA + 5, (uint32_t)KA + 6);
                else if (op == BIT_FIELD_INSERT)
                    put(&m, op, 6, (uint32_t)U, id, prev, other, (uint32_t)KA + 5, (uint32_t)KA + 6);
                else if (op == SELECT)
                    put(&m, op, 5, (uint32_t)U, id, (uint32_t)XB, prev, other);
                else if (op == S_NEGATE || op == NOT || op == BITCAST)
                    put(&m, op, 3, op == BITCAST ? (uint32_t)S : (uint32_t)U, id, prev);
                else if (op >= I_EQUAL && op <= S_LESS_THAN_EQUAL) {
                    put(&m, op, 4, (uint32_t)BOOL, id, prev, other);
                    bool_result = 1;
                } else
                    put(&m, op, 4, (uint32_t)U, id, prev, other);
                put(&m, STORE, 2, (uint32_t)OUT, id);
                prev = id;
            }
            if (!bool_result) {
                put(&m, BITWISE_AND, 4, (uint32_t)U, (uint32_t)R + 2, prev, (uint32_t)KA + 7);
                put(&m, STORE, 2, (uint32_t)OUT, (uint32_t)R + 2);
            }
            end(&m);
            o = m;
            folded += spv_opt_values(o.words, o.count) != m.count;
            if (!same_stores(&m, xs, ys)) {
                fprintf(stderr, "known bits: round %d of the modules at random runs otherwise after the pass\n", round);
                failed++;
            }
        }
        check(!failed, "a module at random stores what it stored before the pass");
        if (folded < rounds / 4) fprintf(stderr, "known bits: the pass changed %d modules of %d\n", folded, rounds);
        check(folded >= rounds / 4, "and the pass changes a quarter of them at least");
    }
    printf("spirv opt fold: %d checks failed\n", g_failures);
    return g_failures != 0;
}
