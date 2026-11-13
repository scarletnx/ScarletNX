/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/spirv_opt.h"
#include "common/utilities/spirv_emit.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

enum {
    SPV_OPT_HEADER = 5, /* a module's words before its first instruction */
    SPV_OPT_OP_NOP = 0, /* what an instruction taken out is made, with its width, until the module is compacted */
    SPV_OPT_OP_UNDEF = 1,
    SPV_OPT_OP_LINE = 8,
    SPV_OPT_OP_COPY_MEMORY = 63,
    SPV_OPT_OP_IN_BOUNDS_ACCESS_CHAIN = 66,
    SPV_OPT_OP_ARRAY_LENGTH = 68,
    SPV_OPT_OP_GROUP_DECORATE = 74,
    SPV_OPT_OP_GROUP_MEMBER_DECORATE = 75,
    SPV_OPT_OP_COPY_OBJECT = 83,
    SPV_OPT_OP_CONSTANT_NULL = 46,
    SPV_OPT_OP_IMAGE_SAMPLE_PROJ_EXPLICIT_LOD = 92,
    SPV_OPT_OP_IMAGE_SAMPLE_PROJ_DREF_IMPLICIT_LOD = 93,
    SPV_OPT_OP_IMAGE_SAMPLE_PROJ_DREF_EXPLICIT_LOD = 94,
    SPV_OPT_OP_GENERIC_CAST_TO_PTR_EXPLICIT = 123,
    SPV_OPT_OP_FWIDTH_COARSE = 215, /* the last of the derivatives, which start at OpDPdx */
    SPV_OPT_OP_NO_LINE = 317,
    SPV_OPT_OP_DECORATE_ID = 332,
    SPV_OPT_OP_TERMINATE_INVOCATION = 4416,
    SPV_OPT_OP_DECORATE_STRING = 5632,
    SPV_OPT_VOLATILE = 1,   /* OpLoad's memory operand */
    SPV_OPT_GLSL_MODF = 35, /* the two of GLSL.std.450 that write through a pointer operand */
    SPV_OPT_GLSL_FREXP = 51,
    SPV_OPT_GLSL_INTERPOLATE_AT_CENTROID = 76, /* the three that read an input through a pointer operand */
    SPV_OPT_GLSL_INTERPOLATE_AT_OFFSET = 78,
    SPV_OPT_DECORATION_BUFFER_BLOCK = 3, /* a block under Uniform that the module may write */
    SPV_OPT_SWEEPS = 16, /* the most dead code elimination walks the functions: what is left then stays */
    /* what a pass knows of an id */
    SPV_OPT_VARIABLE = 1, /* a Function variable */
    SPV_OPT_GONE = 2,     /* its instruction was taken out */
    /* and, as bits, what value numbering knows of one */
    SPV_OPT_PINNED = 4,    /* an instruction of no known layout may name it, or a decoration with operands does:
                              its instruction is not numbered */
    SPV_OPT_READ_ONLY = 8, /* a pointer into memory no instruction of the module writes */
    SPV_OPT_INT = 16,      /* a type: a 32-bit integer of either sign */
    SPV_OPT_BOOL = 32,     /* a type: the boolean */
    SPV_OPT_CONSTANT = 64, /* a constant of one word, true, false or null, and no specialization constant */
    /* what dead code elimination may do with an instruction */
    SPV_OPT_KEEP = 0,        /* it has an effect, or is gone already */
    SPV_OPT_RESULT = 1,      /* it only makes its result: it goes when nothing uses the result */
    SPV_OPT_LOCAL = 2,       /* a Function variable: it goes when nothing uses it and no store to it is left */
    SPV_OPT_LOCAL_STORE = 3, /* a store to one: it goes when nothing uses the variable */
    /* what value numbering does with an instruction: value_class */
    SPV_OPT_UNKNOWN = 0,
    SPV_OPT_PLAIN = 1,
    SPV_OPT_PURE = 2,
    SPV_OPT_IN_BLOCK = 3,
    SPV_OPT_LOADS = 4,
    /* what an instruction folds to: fold */
    SPV_OPT_FOLD_NONE = 0,
    SPV_OPT_FOLD_ID = 1,   /* an id it always equals */
    SPV_OPT_FOLD_VALUE = 2 /* a constant of its type */
};

/* "GLSL.std.450" as a module's words hold it, four characters a word from the low byte, on any host */
static const uint32_t spv_opt_glsl_name[] = {0x4c534c47, 0x6474732e, 0x3035342e, 0};

/* The opcodes under 256 whose instructions have a type and a result and no effect but the result, a bit each: OpUndef;
   OpExtInst and OpLoad (only_result looks at their operands); the access chains, OpArrayLength and
   OpImageTexelPointer; the vector and composite instructions; the image samples, fetches, gathers, reads and queries
   (not OpImageWrite, 99); the conversions; the arithmetic; the tests, the logical ones and OpSelect; the comparisons;
   the shifts and bit instructions; the derivatives; and OpPhi */
#define SPV_OPT_SPAN(first, last, word)                                                                                \
    ((first) / 32 > (word) || (last) / 32 < (word)                                                                     \
         ? 0u                                                                                                          \
         : (0xffffffffu >> (31 - ((last) / 32 > (word) ? 31 : (last) % 32))) &                                         \
               (0xffffffffu << ((first) / 32 < (word) ? 0 : (first) % 32)))
#define SPV_OPT_RESULT_ONLY(word)                                                                                      \
    (SPV_OPT_SPAN(1, 1, word) | SPV_OPT_SPAN(12, 12, word) | SPV_OPT_SPAN(60, 61, word) | SPV_OPT_SPAN(65, 66, word) | \
     SPV_OPT_SPAN(68, 68, word) | SPV_OPT_SPAN(77, 84, word) | SPV_OPT_SPAN(86, 98, word) |                            \
     SPV_OPT_SPAN(100, 107, word) | SPV_OPT_SPAN(109, 124, word) | SPV_OPT_SPAN(126, 152, word) |                      \
     SPV_OPT_SPAN(154, 191, word) | SPV_OPT_SPAN(194, 205, word) | SPV_OPT_SPAN(207, 215, word) |                      \
     SPV_OPT_SPAN(245, 245, word))
static const uint32_t result_only[8] = {SPV_OPT_RESULT_ONLY(0), SPV_OPT_RESULT_ONLY(1), SPV_OPT_RESULT_ONLY(2),
                                        SPV_OPT_RESULT_ONLY(3), SPV_OPT_RESULT_ONLY(4), SPV_OPT_RESULT_ONLY(5),
                                        SPV_OPT_RESULT_ONLY(6), SPV_OPT_RESULT_ONLY(7)};

/* whether the instruction at w (width words, opcode op) only makes its result: by its opcode, and for an OpExtInst
   when it is GLSL.std.450's (the import glsl) and neither Modf nor Frexp, for an OpLoad when its memory is not
   volatile */
static int only_result(const uint32_t* w, uint32_t width, uint32_t op, uint32_t glsl) {
    if (op >= 256 || !(result_only[op / 32] >> (op % 32) & 1) || width < 3) return 0;
    if (op == SPV_OP_EXT_INST)
        return width >= 5 && w[3] == glsl && w[4] != SPV_OPT_GLSL_MODF && w[4] != SPV_OPT_GLSL_FREXP;
    if (op == SPV_OP_LOAD) return width < 5 || !(w[4] & SPV_OPT_VOLATILE);
    return 1;
}

/* Which operand words of an instruction of opcode op are ids: all but the word *literal (0 for none) and, where *more
   is 0, the words after it. A literal is an index, a mask, a weight or an extended set's instruction; the words after
   an image operands mask or an instruction's number are ids again. An opcode this does not know gives every word: a
   literal of its own is then counted as the id of its number, which can only keep that id's instruction */
static void operand_ids(uint32_t op, uint32_t* literal, uint32_t* more) {
    *literal = 0;
    *more = 1;
    switch (op) {
        case SPV_OP_FUNCTION: *literal = 3; break; /* its control mask */
        case SPV_OP_EXT_INST: *literal = 4; break;
        case SPV_OP_IMAGE_WRITE: *literal = 4; break;
        case SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD:
        case SPV_OP_IMAGE_SAMPLE_EXPLICIT_LOD:
        case SPV_OP_IMAGE_SAMPLE_PROJ_IMPLICIT_LOD:
        case SPV_OPT_OP_IMAGE_SAMPLE_PROJ_EXPLICIT_LOD:
        case SPV_OP_IMAGE_FETCH:
        case SPV_OP_IMAGE_READ: *literal = 5; break;
        case SPV_OP_IMAGE_SAMPLE_DREF_IMPLICIT_LOD:
        case SPV_OP_IMAGE_SAMPLE_DREF_EXPLICIT_LOD:
        case SPV_OPT_OP_IMAGE_SAMPLE_PROJ_DREF_IMPLICIT_LOD:
        case SPV_OPT_OP_IMAGE_SAMPLE_PROJ_DREF_EXPLICIT_LOD:
        case SPV_OP_IMAGE_GATHER:
        case SPV_OP_IMAGE_DREF_GATHER: *literal = 6; break;
        case SPV_OP_SELECTION_MERGE:
            *literal = 2;
            *more = 0;
            break;
        case SPV_OP_STORE:
        case SPV_OP_LOOP_MERGE:
        case SPV_OP_SWITCH: /* its selector and default; the cases' labels are never taken out */
            *literal = 3;
            *more = 0;
            break;
        case SPV_OP_LOAD:
        case SPV_OP_COMPOSITE_EXTRACT:
        case SPV_OP_BRANCH_CONDITIONAL:
            *literal = 4;
            *more = 0;
            break;
        case SPV_OP_VECTOR_SHUFFLE:
        case SPV_OP_COMPOSITE_INSERT:
            *literal = 5;
            *more = 0;
            break;
        default: break;
    }
}

/* The module without the instructions a pass made OpNop: those that stay, moved down a run at a time, and a name or
   a decoration of an id whose instruction went (is[id] is SPV_OPT_GONE) goes too. Returns the words left. */
static size_t compact(uint32_t* module, size_t words, uint32_t bound, const uint8_t* is) {
    size_t to = SPV_OPT_HEADER, run = SPV_OPT_HEADER;
    for (size_t i = SPV_OPT_HEADER; i <= words;) {
        uint32_t width = i < words ? module[i] >> 16 : 0, op = i < words ? module[i] & 0xffff : SPV_OPT_OP_NOP;
        int names = width >= 2 && module[i + 1] < bound && is[module[i + 1]] == SPV_OPT_GONE &&
                    (op == SPV_OP_NAME || op == SPV_OP_DECORATE || op == SPV_OPT_OP_DECORATE_ID ||
                     op == SPV_OPT_OP_DECORATE_STRING);
        if (op == SPV_OPT_OP_NOP || names) {
            if (to != run) memmove(module + to, module + run, (i - run) * sizeof *module);
            to += i - run;
            run = i + width;
        }
        if (i == words) break;
        i += width;
    }
    return to;
}

size_t spv_opt_dead_code(uint32_t* module, size_t words) {
    if (words <= SPV_OPT_HEADER) return words;
    uint32_t bound = module[3], glsl = 0;
    size_t first = 0; /* the first OpFunction: what comes before it is the module's declarations */
    for (size_t i = SPV_OPT_HEADER; i < words && !first; i += module[i] >> 16) {
        uint32_t width = module[i] >> 16, op = module[i] & 0xffff;
        assert(width && width <= words - i);
        if (op == SPV_OP_EXT_INST_IMPORT && width == 2 + sizeof spv_opt_glsl_name / sizeof *spv_opt_glsl_name &&
            !memcmp(module + i + 2, spv_opt_glsl_name, sizeof spv_opt_glsl_name))
            glsl = module[i + 1];
        if (op == SPV_OP_FUNCTION) first = i;
    }
    if (!first) return words;
    size_t count = 0; /* the functions' instructions */
    for (size_t i = first; i < words; i += module[i] >> 16) {
        assert(module[i] >> 16 && module[i] >> 16 <= words - i);
        count++;
    }
    /* one block: each id's uses by the instructions left and, for a Function variable, the stores to it left; where
       each instruction of the functions starts and what the pass may do with it (SPV_OPT_KEEP and the others, with
       where its ids are: operand_ids' answer, decoded once); and what each id is, a byte */
    size_t ids = bound;
    assert(count <= SIZE_MAX / 16 && ids <= SIZE_MAX / 32);
    uint32_t* uses = calloc(1, (2 * ids + 2 * count) * sizeof *uses + ids);
    if (!uses) abort();
    uint32_t *stores = uses + bound, *at = stores + bound, *how = at + count;
    uint8_t* is = (uint8_t*)(how + count);
    uint32_t n = 0;
    for (size_t i = first; i < words; i += module[i] >> 16, n++) {
        const uint32_t* w = module + i;
        uint32_t width = w[0] >> 16, op = w[0] & 0xffff, kind = SPV_OPT_KEEP, from, literal, more;
        operand_ids(op, &literal, &more);
        if (op == SPV_OP_VARIABLE && width >= 4 && w[3] == SPV_STORAGE_FUNCTION && w[2] < bound) {
            is[w[2]] = SPV_OPT_VARIABLE;
            kind = SPV_OPT_LOCAL;
            from = 4; /* its initializer */
        } else if (op == SPV_OP_STORE && width >= 3 && w[1] < bound && is[w[1]] == SPV_OPT_VARIABLE) {
            stores[w[1]]++;
            kind = SPV_OPT_LOCAL_STORE;
            from = 2; /* the value: a store is no use of the variable it writes */
        } else if (only_result(w, width, op, glsl) && w[2] < bound) {
            kind = SPV_OPT_RESULT;
            from = 3; /* an instruction with a result does not use it, nor its type */
        } else
            from = 1; /* one that stays may count both */
        at[n] = (uint32_t)i;
        how[n] = kind | from << 4 | literal << 8 | more << 12;
        if (literal && !more) width = width < literal ? width : literal;
        for (uint32_t k = from; k < width; k++)
            if (k != literal && w[k] < bound) uses[w[k]]++;
    }
    /* Backwards, so that what an instruction used is looked at after the instruction went. A variable's last read
       may go after the stores that follow it in the words were passed: those go in one more sweep */
    int again = 1, any = 0;
    for (int sweep = 0; again && sweep < SPV_OPT_SWEEPS; sweep++) {
        again = 0;
        for (n = (uint32_t)count; n-- > 0;) {
            uint32_t kind = how[n] & 15;
            if (kind == SPV_OPT_KEEP) continue;
            uint32_t* w = module + at[n];
            if (kind == SPV_OPT_LOCAL_STORE) {
                if (uses[w[1]]) continue;
                stores[w[1]]--;
            } else {
                if (uses[w[2]] || (kind == SPV_OPT_LOCAL && stores[w[2]])) continue;
                is[w[2]] = SPV_OPT_GONE;
            }
            uint32_t width = w[0] >> 16, literal = how[n] >> 8 & 15;
            if (literal && !(how[n] >> 12)) width = width < literal ? width : literal;
            for (uint32_t k = how[n] >> 4 & 15; k < width; k++) {
                if (k == literal || w[k] >= bound) continue;
                uses[w[k]]--;
                if (!uses[w[k]] && is[w[k]] == SPV_OPT_VARIABLE && stores[w[k]]) again = 1;
            }
            how[n] = SPV_OPT_KEEP;
            w[0] = (w[0] & 0xffff0000u) | SPV_OPT_OP_NOP;
            any = 1;
        }
    }
    if (any) words = compact(module, words, bound, is);
    free(uses);
    return words;
}

/* What spv_opt_values does with an instruction of a function: SPV_OPT_UNKNOWN, it has no layout for it, so every
   operand word may be an id and the instruction may write anything; SPV_OPT_PLAIN, it knows which words are ids and
   that is all; and the ones it numbers, which only make their result from their operands: SPV_OPT_PURE wherever an
   equal one dominates, SPV_OPT_IN_BLOCK within a block (an image sample or a derivative that takes the fragment's
   implicit derivatives, which a block under another branch does not share; an OpSampledImage, which its block
   alone may use), and SPV_OPT_LOADS, a load that is not volatile. */
static uint32_t value_class(const uint32_t* w, uint32_t width, uint32_t op, uint32_t glsl) {
    if (op < 256 && (result_only[op / 32] >> (op % 32) & 1)) {
        if (width < 3 || op == SPV_OPT_OP_ARRAY_LENGTH || op == SPV_OPT_OP_GENERIC_CAST_TO_PTR_EXPLICIT)
            return SPV_OPT_UNKNOWN; /* each has a literal operand_ids does not list */
        if (op == SPV_OP_EXT_INST) {
            if (width < 5 || w[3] != glsl) return SPV_OPT_UNKNOWN;
            return w[4] == SPV_OPT_GLSL_MODF || w[4] == SPV_OPT_GLSL_FREXP ||
                           (w[4] >= SPV_OPT_GLSL_INTERPOLATE_AT_CENTROID && w[4] <= SPV_OPT_GLSL_INTERPOLATE_AT_OFFSET)
                       ? SPV_OPT_PLAIN
                       : SPV_OPT_PURE;
        }
        if (op == SPV_OP_LOAD) {
            if (width < 4) return SPV_OPT_UNKNOWN;
            return width >= 5 && (w[4] & SPV_OPT_VOLATILE) ? SPV_OPT_PLAIN : SPV_OPT_LOADS;
        }
        if (op == SPV_OPT_OP_UNDEF || op == SPV_OP_IMAGE_TEXEL_POINTER || op == SPV_OP_IMAGE_READ || op == SPV_OP_PHI)
            return SPV_OPT_PLAIN;
        if (op == SPV_OP_SAMPLED_IMAGE || op == SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD ||
            op == SPV_OP_IMAGE_SAMPLE_DREF_IMPLICIT_LOD || op == SPV_OP_IMAGE_SAMPLE_PROJ_IMPLICIT_LOD ||
            op == SPV_OPT_OP_IMAGE_SAMPLE_PROJ_DREF_IMPLICIT_LOD || op == SPV_OP_IMAGE_QUERY_LOD ||
            (op >= SPV_OP_DPDX && op <= SPV_OPT_OP_FWIDTH_COARSE))
            return SPV_OPT_IN_BLOCK;
        return SPV_OPT_PURE;
    }
    switch (op) {
        case SPV_OPT_OP_NOP:
        case SPV_OPT_OP_LINE:
        case SPV_OP_FUNCTION:
        case SPV_OP_FUNCTION_PARAMETER:
        case SPV_OP_FUNCTION_END:
        case SPV_OP_FUNCTION_CALL:
        case SPV_OP_VARIABLE:
        case SPV_OP_STORE:
        case SPV_OPT_OP_COPY_MEMORY:
        case SPV_OP_IMAGE_WRITE:
        case SPV_OP_EMIT_VERTEX:
        case SPV_OP_END_PRIMITIVE:
        case SPV_OP_CONTROL_BARRIER:
        case SPV_OP_MEMORY_BARRIER:
        case SPV_OP_LOOP_MERGE:
        case SPV_OP_SELECTION_MERGE:
        case SPV_OP_LABEL:
        case SPV_OP_BRANCH:
        case SPV_OP_BRANCH_CONDITIONAL:
        case SPV_OP_SWITCH:
        case SPV_OP_KILL:
        case SPV_OP_RETURN:
        case SPV_OP_RETURN_VALUE:
        case SPV_OP_UNREACHABLE:
        case SPV_OPT_OP_NO_LINE:
        case SPV_OPT_OP_TERMINATE_INVOCATION:
        case SPV_OP_DEMOTE_TO_HELPER_INVOCATION: return SPV_OPT_PLAIN;
        default: /* the atomics: a pointer, a scope, the memory semantics and a value, ids all */
            return op >= SPV_OP_ATOMIC_LOAD && op <= SPV_OP_ATOMIC_XOR ? SPV_OPT_PLAIN : SPV_OPT_UNKNOWN;
    }
}

/* operand_ids for every instruction value_class knows: a variable's storage class and a line's numbers are literals
   too, which dead code elimination has no need to tell from ids */
static void value_operands(uint32_t op, uint32_t* literal, uint32_t* more) {
    operand_ids(op, literal, more);
    if (op == SPV_OP_VARIABLE) *literal = 3;
    if (op == SPV_OPT_OP_COPY_MEMORY || op == SPV_OPT_OP_LINE) {
        *literal = op == SPV_OPT_OP_LINE ? 1 : 3;
        *more = 0;
    }
}

/* whether block a dominates block b, a of b's function or of one before it: b's dominators are a walk up idom, each
   later in the post order, which numbers a function after the ones before it */
static int dominates(const uint32_t* idom, const uint32_t* order, uint32_t a, uint32_t b) {
    if (a == b) return 1;
    if (!order[b]) return 0; /* a block nothing reaches; from one that is a, the walk below never starts */
    while (order[b] < order[a]) b = idom[b];
    return a == b;
}

/* the two operands of these may be read in either order */
static int commutes(uint32_t op, uint32_t width) {
    if (width != 5) return 0;
    return op == SPV_OP_I_ADD || op == SPV_OP_I_MUL || op == SPV_OP_BITWISE_OR || op == SPV_OP_BITWISE_XOR ||
           op == SPV_OP_BITWISE_AND || op == SPV_OP_I_EQUAL || op == SPV_OP_I_NOT_EQUAL ||
           (op >= SPV_OP_LOGICAL_EQUAL && op <= SPV_OP_LOGICAL_AND);
}

/* the instruction of width words that makes x with no decoration, module + def[x], or NULL: def[x] is 0 for an id
   the walk has not met, and the header's first word has no width a caller asks for */
static const uint32_t* made_by(const uint32_t* module, uint32_t x, uint32_t width, const uint32_t* def,
                               const uint32_t* sign) {
    const uint32_t* w = module + def[x];
    return w[0] >> 16 == width && w[2] == x && !sign[x] ? w : NULL;
}

/* x as y + k with k an integer constant: y, and k in *k, of the y + k, k + y or y - k that makes x; else x itself and
   0. Two terms of one y differ by a constant (spirv-opt's merge of add and sub) */
static uint32_t term(const uint32_t* module, uint32_t x, uint32_t bound, const uint8_t* is, const uint32_t* type_of,
                     const uint32_t* value, const uint32_t* def, const uint32_t* sign, uint32_t* k) {
    const uint32_t* w = made_by(module, x, 5, def, sign);
    uint32_t op = w ? w[0] & 0xffff : 0, a = w && w[3] < bound ? w[3] : 0, b = w && w[4] < bound ? w[4] : 0;
    int ka = a && is[a] & SPV_OPT_CONSTANT && is[type_of[a]] & SPV_OPT_INT;
    int kb = b && is[b] & SPV_OPT_CONSTANT && is[type_of[b]] & SPV_OPT_INT;
    *k = 0;
    if ((op != SPV_OP_I_ADD && op != SPV_OP_I_SUB) || !a || !b || ka == kb) return x;
    if (kb) {
        *k = op == SPV_OP_I_SUB ? 0u - value[b] : value[b];
        return a;
    }
    if (op != SPV_OP_I_ADD) return x;
    *k = value[a];
    return b;
}

/* p as a test of a 32-bit integer against 0: that integer, with *zero 1 for == 0 and 0 for != 0, from the OpIEqual or
   OpINotEqual of it and a zero (on either side) that makes p, or from the OpLogicalNot of one; else 0 */
static uint32_t zero_test(const uint32_t* module, uint32_t p, uint32_t bound, const uint8_t* is,
                          const uint32_t* type_of, const uint32_t* value, const uint32_t* def, const uint32_t* sign,
                          uint32_t* zero) {
    const uint32_t* w = made_by(module, p, 4, def, sign);
    uint32_t flip = 0;
    if (w && (w[0] & 0xffff) == SPV_OP_LOGICAL_NOT && w[3] < bound) {
        p = w[3];
        flip = 1;
    }
    w = made_by(module, p, 5, def, sign);
    uint32_t op = w ? w[0] & 0xffff : 0, a = w && w[3] < bound ? w[3] : 0, b = w && w[4] < bound ? w[4] : 0;
    if ((op != SPV_OP_I_EQUAL && op != SPV_OP_I_NOT_EQUAL) || !a || !b) return 0;
    int za = is[a] & SPV_OPT_CONSTANT && is[type_of[a]] & SPV_OPT_INT && !value[a];
    int zb = is[b] & SPV_OPT_CONSTANT && is[type_of[b]] & SPV_OPT_INT && !value[b];
    uint32_t s = zb ? a : za ? b : 0;
    if (!s || !(is[type_of[s]] & SPV_OPT_INT)) return 0;
    *zero = (op == SPV_OP_I_EQUAL) ^ flip;
    return s;
}

/* whether every bit of the integer p that may be 1 is 1 in q: p is q, p is an & of q, q an | of p, or p an & and q an
   | of one operand */
static int bits_within(const uint32_t* module, uint32_t p, uint32_t q, const uint32_t* def, const uint32_t* sign) {
    const uint32_t *both = made_by(module, p, 5, def, sign), *either = made_by(module, q, 5, def, sign);
    if (p == q) return 1;
    if (both && (both[0] & 0xffff) != SPV_OP_BITWISE_AND) both = NULL;
    if (either && (either[0] & 0xffff) != SPV_OP_BITWISE_OR) either = NULL;
    if (both && (both[3] == q || both[4] == q)) return 1;
    if (either && (either[3] == p || either[4] == p)) return 1;
    return both && either &&
           (both[3] == either[3] || both[3] == either[4] || both[4] == either[3] || both[4] == either[4]);
}

/* whether the boolean p implies q, both tests against 0: s != 0 implies t != 0 when the bits of s are within t's,
   and s == 0 implies t == 0 when t's are within s's ((x & m) != 0 implies x != 0) */
static int implies(const uint32_t* module, uint32_t p, uint32_t q, uint32_t bound, const uint8_t* is,
                   const uint32_t* type_of, const uint32_t* value, const uint32_t* def, const uint32_t* sign) {
    uint32_t zp, zq, sp = zero_test(module, p, bound, is, type_of, value, def, sign, &zp);
    uint32_t sq = sp ? zero_test(module, q, bound, is, type_of, value, def, sign, &zq) : 0;
    if (!sq || zp != zq) return 0;
    return zp ? bits_within(module, sq, sp, def, sign) : bits_within(module, sp, sq, def, sign);
}

/* What the instruction at w always equals, when its type is a 32-bit integer or the boolean and its operands say so
   (they were replaced already), beyond what known_bits computes of it (an integer whose bits are all known):
   SPV_OPT_FOLD_VALUE with the constant's bits in *out for a product of two constants, a difference of two terms of
   one value ((y + k1) - (y + k2) is k1 - k2, x - x is 0), x ^ x and the logical instructions on constants (false && x,
   true || x); SPV_OPT_FOLD_ID with an operand in *out when the others leave it as it is (x + 0, x - 0, x | 0, x ^ 0,
   x << 0, x * 1, x & x, x | x, true && x, false || x, x && x) or when one operand of an && or || implies the other (p
   && q is p, p || q is q), only when that operand has the instruction's type. Of any type: a select whose condition is
   a constant or whose two values are one id, and a bitcast to the type its operand has, or of a bitcast from that
   type. module is for the instructions that make the operands, def[] says where, and an instruction with a
   decoration (sign[]) is read as a value of no known form; is, type_of and value are by id. */
static uint32_t fold(const uint32_t* module, const uint32_t* w, uint32_t width, uint32_t op, uint32_t bound,
                     const uint8_t* is, const uint32_t* type_of, const uint32_t* value, const uint32_t* def,
                     const uint32_t* sign, uint32_t* out) {
    uint32_t t = w[1], a = width > 3 && w[3] < bound ? w[3] : 0, b = width > 4 && w[4] < bound ? w[4] : 0;
    uint32_t c = width > 5 && w[5] < bound ? w[5] : 0;
    /* a, b, c: the operands that are ids the pass has tables for (0 is none: no id is 0) */
    int ka = is[a] & SPV_OPT_CONSTANT, kb = is[b] & SPV_OPT_CONSTANT;
    int ia = ka && is[type_of[a]] & SPV_OPT_INT, ib = kb && is[type_of[b]] & SPV_OPT_INT;
    int ba = ka && is[type_of[a]] & SPV_OPT_BOOL, bb = kb && is[type_of[b]] & SPV_OPT_BOOL;
    uint32_t va = value[a], vb = value[b];
    if (op == SPV_OP_SELECT && width == 6) {
        uint32_t pick = ba ? va ? b : c : b == c ? b : 0;
        if (!pick || type_of[pick] != t) return SPV_OPT_FOLD_NONE;
        *out = pick;
        return SPV_OPT_FOLD_ID;
    }
    if (op == SPV_OP_BITCAST && width == 4 && a) {
        const uint32_t* from = module + def[a]; /* the header's first word, an opcode of none, when def[a] is 0 */
        if (type_of[a] == t) {
            *out = a;
            return SPV_OPT_FOLD_ID;
        }
        if (from[0] == (4u << 16 | SPV_OP_BITCAST) && from[3] < bound && type_of[from[3]] == t) {
            *out = from[3];
            return SPV_OPT_FOLD_ID;
        }
        return SPV_OPT_FOLD_NONE;
    }
    if (is[t] & SPV_OPT_INT && width == 5) {
        int sum = op == SPV_OP_I_ADD || op == SPV_OP_BITWISE_OR || op == SPV_OP_BITWISE_XOR;
        int product = op == SPV_OP_I_MUL || op == SPV_OP_BITWISE_AND;
        int shift =
            op == SPV_OP_SHIFT_LEFT_LOGICAL || op == SPV_OP_SHIFT_RIGHT_LOGICAL || op == SPV_OP_SHIFT_RIGHT_ARITHMETIC;
        if (!sum && !product && !shift && op != SPV_OP_I_SUB) return SPV_OPT_FOLD_NONE;
        if (ia && ib && op == SPV_OP_I_MUL) { /* the known bits of a product are its low and high zeros alone */
            *out = va * vb;
            return SPV_OPT_FOLD_VALUE;
        }
        /* one constant, k, and the other operand, x: k on the right, or on the left of what commutes */
        uint32_t x = ib ? a : ia && (sum || product) ? b : 0, k = ib ? vb : va;
        if (x) {
            if ((k == 0 && !product) || (k == 1 && op == SPV_OP_I_MUL)) {
                if (type_of[x] != t) return SPV_OPT_FOLD_NONE;
                *out = x;
                return SPV_OPT_FOLD_ID;
            }
        }
        if (a && b && op == SPV_OP_I_SUB) {
            uint32_t ta, tb;
            if (term(module, a, bound, is, type_of, value, def, sign, &ta) ==
                term(module, b, bound, is, type_of, value, def, sign, &tb)) {
                *out = ta - tb;
                return SPV_OPT_FOLD_VALUE;
            }
        }
        if (a && a == b && op == SPV_OP_BITWISE_XOR) {
            *out = 0;
            return SPV_OPT_FOLD_VALUE;
        }
        if (a && a == b && (op == SPV_OP_BITWISE_AND || op == SPV_OP_BITWISE_OR) && type_of[a] == t) {
            *out = a;
            return SPV_OPT_FOLD_ID;
        }
        return SPV_OPT_FOLD_NONE;
    }
    if (is[t] & SPV_OPT_BOOL && width == 4 && ba && op == SPV_OP_LOGICAL_NOT) {
        *out = !va;
        return SPV_OPT_FOLD_VALUE;
    }
    if (is[t] & SPV_OPT_BOOL && width == 5 && op >= SPV_OP_LOGICAL_EQUAL && op <= SPV_OP_LOGICAL_AND) {
        int both = op == SPV_OP_LOGICAL_AND, either = op == SPV_OP_LOGICAL_OR;
        if (ba && bb) {
            *out = both ? va & vb : either ? va | vb : (va == vb) == (op == SPV_OP_LOGICAL_EQUAL);
            return SPV_OPT_FOLD_VALUE;
        }
        if (!both && !either) return SPV_OPT_FOLD_NONE;
        uint32_t x = bb ? a : ba ? b : 0, k = bb ? vb : va;
        if (x && k == (uint32_t)either) { /* false && x, true || x */
            *out = k;
            return SPV_OPT_FOLD_VALUE;
        }
        if (!x && a == b) x = a;
        if (!x && a && b) /* p implies q: p && q is p, p || q is q */
            x = implies(module, a, b, bound, is, type_of, value, def, sign)   ? either ? b : a
                : implies(module, b, a, bound, is, type_of, value, def, sign) ? either ? a : b
                                                                              : 0;
        if (!x || type_of[x] != t) return SPV_OPT_FOLD_NONE;
        *out = x;
        return SPV_OPT_FOLD_ID;
    }
    return SPV_OPT_FOLD_NONE;
}

/* the bits of a + b + carry (0 or 1) that are known, from those known of a (za zeros, oa ones) and of b: LLVM's
   KnownBits::computeForAddCarry. A bit is known where both operands' bits and the carry into it are: the carry is
   known where the sums with every unknown bit 1 and with every one 0 carry alike */
static void known_sum(uint32_t za, uint32_t oa, uint32_t zb, uint32_t ob, uint32_t carry, uint32_t* zero,
                      uint32_t* one) {
    uint32_t most = ~za + ~zb + carry, least = oa + ob + carry;
    uint32_t carry_zero = ~(most ^ za ^ zb), carry_one = least ^ oa ^ ob;
    uint32_t known = (za | oa) & (zb | ob) & (carry_zero | carry_one);
    *zero = ~most & known;
    *one = least & known;
}

/* x >> s with the sign bit copied in, s below 32 */
static uint32_t arithmetic_shift(uint32_t x, uint32_t s) {
    return x >> s | (x >> 31 && s ? 0xffffffffu << (32 - s) : 0);
}

/* What is known of the bits of the result of the instruction at w, whose type is a 32-bit integer, or the boolean
   (result_kind says which; a boolean is 0 or 1) of an integer comparison: *zero the bits known 0, *one those known 1,
   from what zeros and ones know of its 32-bit integer operands, nothing of an operand of another type nor of an id
   they know nothing of (a phi's, a load's). An unsigned comparison is decided where one operand's range, from its
   least to its most value, lies past the other's, and a signed one in the order the sign bit flipped gives; an
   integer compared with itself is decided too. A shift and a field are known only with a constant amount, offset
   and count. A boolean is known only as a constant, which fold takes the logical instructions and selects on. */
static void known_bits(const uint32_t* w, uint32_t width, uint32_t op, uint32_t bound, const uint8_t* is,
                       const uint32_t* type_of, uint32_t result_kind, const uint32_t* zeros, const uint32_t* ones,
                       uint32_t* zero, uint32_t* one) {
    const uint32_t kind = SPV_OPT_INT;
    int compare = op >= SPV_OP_I_EQUAL && op <= SPV_OP_S_LESS_THAN_EQUAL;
    *zero = *one = 0;
    if (result_kind != (compare ? SPV_OPT_BOOL : SPV_OPT_INT)) return;
    /* the operands of that kind; 0, no id, is one of none: its entries stay 0, nothing known */
    uint32_t a = width > 3 && w[3] < bound && is[type_of[w[3]]] & kind ? w[3] : 0;
    uint32_t b = width > 4 && w[4] < bound && is[type_of[w[4]]] & kind ? w[4] : 0;
    uint32_t c = width > 5 && w[5] < bound && is[type_of[w[5]]] & kind ? w[5] : 0;
    uint32_t d = width > 6 && w[6] < bound && is[type_of[w[6]]] & kind ? w[6] : 0;
    uint32_t za = zeros[a], oa = ones[a], zb = zeros[b], ob = ones[b], zc = zeros[c], oc = ones[c];
    int kb = (zb | ob) == 0xffffffffu, kc = (zc | oc) == 0xffffffffu, kd = (zeros[d] | ones[d]) == 0xffffffffu;
    switch (width == 5 ? op : width == 4 ? op | 0x10000u : width == 6 ? op | 0x20000u : op | 0x30000u) {
        case SPV_OP_BITWISE_AND:
            *zero = za | zb;
            *one = oa & ob;
            break;
        case SPV_OP_BITWISE_OR:
            *zero = za & zb;
            *one = oa | ob;
            break;
        case SPV_OP_BITWISE_XOR:
            *zero = (za & zb) | (oa & ob);
            *one = (za & ob) | (oa & zb);
            break;
        case SPV_OP_NOT | 0x10000u:
            *zero = oa;
            *one = za;
            break;
        case SPV_OP_BITCAST | 0x10000u:
        case SPV_OPT_OP_COPY_OBJECT | 0x10000u:
            *zero = za;
            *one = oa;
            break;
        case SPV_OP_I_ADD: known_sum(za, oa, zb, ob, 0, zero, one); break;
        case SPV_OP_I_SUB: known_sum(za, oa, ob, zb, 1, zero, one); break; /* a + ~b + 1 */
        case SPV_OP_S_NEGATE | 0x10000u: known_sum(0xffffffffu, 0, oa, za, 1, zero, one); break;
        case SPV_OP_I_MUL: { /* the low zeros of the two add up, and so do the high ones past 32 */
            uint32_t low = 0, high = 0;
            while (low < 32 && za >> low & 1) low++;
            for (uint32_t k = 0; k < 32 && zb >> k & 1 && low < 32; k++) low++;
            for (uint32_t k = 32; k-- > 0 && za >> k & 1;) high++;
            for (uint32_t k = 32; k-- > 0 && zb >> k & 1;) high++;
            *zero = (low < 32 ? (1u << low) - 1 : 0xffffffffu) | (high > 32 ? 0xffffffffu << (64 - high) : 0);
            break;
        }
        case SPV_OP_SHIFT_LEFT_LOGICAL:
            if (!kb || ob >= 32) break;
            *zero = za << ob | ((1u << ob) - 1);
            *one = oa << ob;
            break;
        case SPV_OP_SHIFT_RIGHT_LOGICAL:
            if (!kb || ob >= 32) break;
            *zero = za >> ob | ~(0xffffffffu >> ob);
            *one = oa >> ob;
            break;
        case SPV_OP_SHIFT_RIGHT_ARITHMETIC:
            if (!kb || ob >= 32) break;
            *zero = arithmetic_shift(za, ob);
            *one = arithmetic_shift(oa, ob);
            break;
        case SPV_OP_SELECT | 0x20000u: /* what both values know */
            *zero = zb & zc;
            *one = ob & oc;
            break;
        case SPV_OP_BIT_FIELD_U_EXTRACT | 0x20000u:
        case SPV_OP_BIT_FIELD_S_EXTRACT | 0x20000u: {
            if (!kb || !kc || oc > 32 || ob > 32 - oc) break;
            if (!oc) {
                *zero = 0xffffffffu;
                break;
            }
            uint32_t mask = 0xffffffffu >> (32 - oc), z = za >> ob & mask, o = oa >> ob & mask;
            if (op == SPV_OP_BIT_FIELD_U_EXTRACT || z >> (oc - 1) & 1) z |= ~mask;
            if (op == SPV_OP_BIT_FIELD_S_EXTRACT && o >> (oc - 1) & 1) o |= ~mask;
            *zero = z;
            *one = o;
            break;
        }
        case SPV_OP_BIT_FIELD_INSERT | 0x30000u: { /* base a, insert b, offset c, count d */
            uint32_t od = ones[d];
            if (width != 7 || !kc || !kd || od > 32 || oc > 32 - od) break;
            uint32_t mask = od ? 0xffffffffu >> (32 - od) << oc : 0;
            *zero = (za & ~mask) | (zb << (oc & 31) & mask);
            *one = (oa & ~mask) | (ob << (oc & 31) & mask);
            break;
        }
        default:
            if (compare && width == 5) {
                int truth = -1; /* undecided */
                if (a && a == b) {
                    truth = op == SPV_OP_I_EQUAL || op == SPV_OP_U_GREATER_THAN_EQUAL ||
                            op == SPV_OP_S_GREATER_THAN_EQUAL || op == SPV_OP_U_LESS_THAN_EQUAL ||
                            op == SPV_OP_S_LESS_THAN_EQUAL;
                } else if (op == SPV_OP_I_EQUAL || op == SPV_OP_I_NOT_EQUAL) {
                    if ((oa & zb) | (za & ob))
                        truth = op == SPV_OP_I_NOT_EQUAL; /* a bit known to differ */
                    else if (((za | oa) & (zb | ob)) == 0xffffffffu)
                        truth = op == SPV_OP_I_EQUAL; /* every bit known alike */
                } else {
                    if (op == SPV_OP_S_GREATER_THAN || op == SPV_OP_S_GREATER_THAN_EQUAL || op == SPV_OP_S_LESS_THAN ||
                        op == SPV_OP_S_LESS_THAN_EQUAL) {
                        uint32_t sign = 0x80000000u, ya = za, yb = zb;
                        za = (za & ~sign) | (oa & sign);
                        oa = (oa & ~sign) | (ya & sign);
                        zb = (zb & ~sign) | (ob & sign);
                        ob = (ob & ~sign) | (yb & sign);
                    }
                    /* a's least and most against b's: as a < b, or a <= b, with the operands swapped for > */
                    int greater = op == SPV_OP_U_GREATER_THAN || op == SPV_OP_S_GREATER_THAN ||
                                  op == SPV_OP_U_GREATER_THAN_EQUAL || op == SPV_OP_S_GREATER_THAN_EQUAL;
                    int equal = op == SPV_OP_U_GREATER_THAN_EQUAL || op == SPV_OP_S_GREATER_THAN_EQUAL ||
                                op == SPV_OP_U_LESS_THAN_EQUAL || op == SPV_OP_S_LESS_THAN_EQUAL;
                    uint32_t least_l = greater ? ob : oa, most_l = greater ? ~zb : ~za;
                    uint32_t least_r = greater ? oa : ob, most_r = greater ? ~za : ~zb;
                    if (equal ? most_l <= least_r : most_l < least_r) truth = 1;
                    if (equal ? least_l > most_r : least_l >= most_r) truth = 0;
                }
                if (truth >= 0) {
                    *one = (uint32_t)truth;
                    *zero = ~*one;
                }
            }
            break;
    }
}

/* What the instruction at w folds to by the bits known of it (zero and one, from known_bits) and of its operands:
   SPV_OPT_FOLD_VALUE with its bits in *out when all of them are known; SPV_OPT_FOLD_ID with an operand of the
   instruction's type in *out when the known bits leave that operand as it is: a & b where every bit of a that may be
   1 is known 1 in b, a | b where every bit of b that may be 1 is known 1 in a, and a field from bit 0 that keeps
   every bit of its base that may be 1 (or, signed, whose sign bit is the base's high bits, all known alike) */
static uint32_t fold_known(const uint32_t* w, uint32_t width, uint32_t op, uint32_t bound, const uint32_t* type_of,
                           const uint32_t* zeros, const uint32_t* ones, uint32_t zero, uint32_t one, uint32_t* out) {
    if ((zero | one) == 0xffffffffu) {
        *out = one;
        return SPV_OPT_FOLD_VALUE;
    }
    uint32_t t = w[1], a = width > 3 && w[3] < bound ? w[3] : 0, b = width > 4 && w[4] < bound ? w[4] : 0;
    uint32_t c = width > 5 && w[5] < bound ? w[5] : 0, keep = 0;
    if (!a || !b) return SPV_OPT_FOLD_NONE;
    if (op == SPV_OP_BITWISE_AND && width == 5)
        keep = !(~zeros[a] & ~ones[b]) ? a : !(~zeros[b] & ~ones[a]) ? b : 0;
    else if (op == SPV_OP_BITWISE_OR && width == 5)
        keep = !(~zeros[b] & ~ones[a]) ? a : !(~zeros[a] & ~ones[b]) ? b : 0;
    else if ((op == SPV_OP_BIT_FIELD_U_EXTRACT || op == SPV_OP_BIT_FIELD_S_EXTRACT) && width == 6 && c &&
             (zeros[b] | ones[b]) == 0xffffffffu && !ones[b] && (zeros[c] | ones[c]) == 0xffffffffu &&
             ones[c] - 1 < 32) {
        uint32_t n = ones[c]; /* the count, from 1 to 32, of a field from bit 0 */
        if (n == 32)
            keep = a;
        else if (op == SPV_OP_BIT_FIELD_U_EXTRACT)
            keep = ~zeros[a] >> n ? 0 : a;
        else { /* the bits from the field's sign bit up, all known 0 or all known 1 */
            uint32_t up = 0xffffffffu >> (n - 1);
            keep = zeros[a] >> (n - 1) == up || ones[a] >> (n - 1) == up ? a : 0;
        }
    }
    if (!keep || type_of[keep] != t) return SPV_OPT_FOLD_NONE;
    *out = keep;
    return SPV_OPT_FOLD_ID;
}

/* the operands an integer or logical instruction of opcode op takes (of GLSL.std.450, glsl_op its number), for the
   ones evaluate knows; 0 for the rest */
static uint32_t arity(uint32_t op, uint32_t glsl_op) {
    switch (op) {
        case SPV_OP_S_NEGATE:
        case SPV_OP_NOT:
        case SPV_OP_BITCAST:
        case SPV_OPT_OP_COPY_OBJECT:
        case SPV_OP_LOGICAL_NOT: return 1;
        case SPV_OP_SELECT:
        case SPV_OP_BIT_FIELD_U_EXTRACT:
        case SPV_OP_BIT_FIELD_S_EXTRACT: return 3;
        case SPV_OP_BIT_FIELD_INSERT: return 4;
        case SPV_OP_EXT_INST:
            return glsl_op == SPV_GLSL_S_ABS ? 1
                   : glsl_op == SPV_GLSL_U_MIN || glsl_op == SPV_GLSL_S_MIN || glsl_op == SPV_GLSL_U_MAX ||
                           glsl_op == SPV_GLSL_S_MAX
                       ? 2
                   : glsl_op == SPV_GLSL_U_CLAMP || glsl_op == SPV_GLSL_S_CLAMP ? 3
                                                                                : 0;
        default:
            return op == SPV_OP_I_ADD || op == SPV_OP_I_SUB || op == SPV_OP_I_MUL ||
                           (op >= SPV_OP_SHIFT_RIGHT_LOGICAL && op <= SPV_OP_BITWISE_AND) ||
                           (op >= SPV_OP_LOGICAL_EQUAL && op <= SPV_OP_LOGICAL_AND) ||
                           (op >= SPV_OP_I_EQUAL && op <= SPV_OP_S_LESS_THAN_EQUAL)
                       ? 2
                       : 0;
    }
}

/* a < b as signed 32-bit integers: the sign bit flipped orders them as unsigned ones */
static int signed_less(uint32_t a, uint32_t b) { return (a ^ 0x80000000u) < (b ^ 0x80000000u); }

/* What an instruction arity() knows (op, glsl_op) makes of the operand values v, into *out (a boolean is 0 or 1).
   0 where the module gives it no value: a shift by 32 or more, a field past bit 32, a clamp whose least is past its
   most, the absolute value of the least integer */
static int evaluate(uint32_t op, uint32_t glsl_op, const uint32_t* v, uint32_t* out) {
    uint32_t a = v[0], b = v[1], c = v[2], d = v[3], mask;
    switch (op) {
        case SPV_OP_I_ADD: *out = a + b; return 1;
        case SPV_OP_I_SUB: *out = a - b; return 1;
        case SPV_OP_I_MUL: *out = a * b; return 1;
        case SPV_OP_S_NEGATE: *out = 0u - a; return 1;
        case SPV_OP_NOT: *out = ~a; return 1;
        case SPV_OP_BITWISE_AND: *out = a & b; return 1;
        case SPV_OP_BITWISE_OR: *out = a | b; return 1;
        case SPV_OP_BITWISE_XOR: *out = a ^ b; return 1;
        case SPV_OP_SHIFT_LEFT_LOGICAL: *out = b < 32 ? a << b : 0; return b < 32;
        case SPV_OP_SHIFT_RIGHT_LOGICAL: *out = b < 32 ? a >> b : 0; return b < 32;
        case SPV_OP_SHIFT_RIGHT_ARITHMETIC: *out = b < 32 ? arithmetic_shift(a, b) : 0; return b < 32;
        case SPV_OP_BIT_FIELD_U_EXTRACT:
        case SPV_OP_BIT_FIELD_S_EXTRACT:
            if (c > 32 || b > 32 - c) return 0;
            mask = c ? 0xffffffffu >> (32 - c) : 0;
            *out = c ? a >> b & mask : 0;
            if (op == SPV_OP_BIT_FIELD_S_EXTRACT && c && *out >> (c - 1) & 1) *out |= ~mask;
            return 1;
        case SPV_OP_BIT_FIELD_INSERT: /* base a, insert b, offset c, count d */
            if (d > 32 || c > 32 - d) return 0;
            mask = d ? 0xffffffffu >> (32 - d) << c : 0;
            *out = (a & ~mask) | (b << (c & 31) & mask);
            return 1;
        case SPV_OP_SELECT: *out = a ? b : c; return 1;
        case SPV_OP_BITCAST:
        case SPV_OPT_OP_COPY_OBJECT: *out = a; return 1;
        case SPV_OP_LOGICAL_EQUAL:
        case SPV_OP_I_EQUAL: *out = a == b; return 1;
        case SPV_OP_LOGICAL_NOT_EQUAL:
        case SPV_OP_I_NOT_EQUAL: *out = a != b; return 1;
        case SPV_OP_LOGICAL_OR: *out = a | b; return 1;
        case SPV_OP_LOGICAL_AND: *out = a & b; return 1;
        case SPV_OP_LOGICAL_NOT: *out = !a; return 1;
        case SPV_OP_U_GREATER_THAN: *out = a > b; return 1;
        case SPV_OP_S_GREATER_THAN: *out = (uint32_t)signed_less(b, a); return 1;
        case SPV_OP_U_GREATER_THAN_EQUAL: *out = a >= b; return 1;
        case SPV_OP_S_GREATER_THAN_EQUAL: *out = (uint32_t)!signed_less(a, b); return 1;
        case SPV_OP_U_LESS_THAN: *out = a < b; return 1;
        case SPV_OP_S_LESS_THAN: *out = (uint32_t)signed_less(a, b); return 1;
        case SPV_OP_U_LESS_THAN_EQUAL: *out = a <= b; return 1;
        case SPV_OP_S_LESS_THAN_EQUAL: *out = (uint32_t)!signed_less(b, a); return 1;
        default: break;
    }
    switch (glsl_op) {
        case SPV_GLSL_S_ABS: *out = a >> 31 ? 0u - a : a; return a != 0x80000000u;
        case SPV_GLSL_U_MIN: *out = a < b ? a : b; return 1;
        case SPV_GLSL_S_MIN: *out = signed_less(a, b) ? a : b; return 1;
        case SPV_GLSL_U_MAX: *out = a < b ? b : a; return 1;
        case SPV_GLSL_S_MAX: *out = signed_less(a, b) ? b : a; return 1;
        case SPV_GLSL_U_CLAMP: *out = a < b ? b : a > c ? c : a; return b <= c;
        case SPV_GLSL_S_CLAMP: *out = signed_less(a, b) ? b : signed_less(c, a) ? c : a; return !signed_less(c, b);
        default: return 0;
    }
}

/* id, of the kind asked (SPV_OPT_INT or SPV_OPT_BOOL), as a value of few possible values: the id its values are a
   function of (its leaf), with those values in entry, one for each of the leaf's values in turn, and their count in
   *n. A constant is one value of no leaf (0); a value with a table (leaf, table, count) is its table; a boolean, or an
   integer with one or two bits not known (zeros, ones), is its own leaf, of its two or four values. Anything else, or
   of another kind, is no value of few (UINT32_MAX) */
static uint32_t few_of(uint32_t id, uint32_t kind, uint32_t bound, const uint8_t* is, const uint32_t* type_of,
                       const uint32_t* value, const uint32_t* zeros, const uint32_t* ones, const uint32_t* leaf,
                       const uint32_t* table, const uint8_t* count, uint32_t* entry, uint32_t* n) {
    kind = id < bound ? is[type_of[id]] & kind : 0;
    if (!kind) return UINT32_MAX;
    if (is[id] & SPV_OPT_CONSTANT) {
        entry[0] = entry[1] = entry[2] = entry[3] = value[id];
        *n = 1;
        return 0;
    }
    if (leaf[id]) {
        memcpy(entry, table + 4 * (size_t)id, 4 * sizeof *entry);
        *n = count[id];
        return leaf[id];
    }
    uint32_t unknown = kind & SPV_OPT_BOOL ? 1 : ~(zeros[id] | ones[id]), low = unknown & (0u - unknown);
    uint32_t high = unknown & ~low, base = kind & SPV_OPT_BOOL ? 0 : ones[id];
    if (!unknown || (high & (high - 1))) return UINT32_MAX; /* three bits or more */
    entry[0] = base;
    entry[1] = base | low;
    entry[2] = base | high;
    entry[3] = base | low | high;
    *n = high ? 4 : 2;
    return id;
}

/* What the instruction at w, a 32-bit integer or a boolean, is as a function of one value of few possible values
   (ucas's map rule): when each operand is a constant, a value with a table or a leaf (few_of), all of one leaf, the
   instruction is evaluated for each of the leaf's values. SPV_OPT_FOLD_VALUE with *out when every value gives one
   constant; SPV_OPT_FOLD_ID when the values are an operand's of the instruction's type, or the leaf's itself (it
   dominates the instruction: an operand was computed from it); else the instruction's table is kept in leaf, table and
   count for the ones that read it. An instruction evaluate gives no value for one of them is no value of few */
static uint32_t few_values(const uint32_t* w, uint32_t width, uint32_t op, uint32_t glsl, uint32_t bound,
                           const uint8_t* is, const uint32_t* type_of, const uint32_t* value, const uint32_t* zeros,
                           const uint32_t* ones, uint32_t* leaf, uint32_t* table, uint8_t* count, uint32_t* out) {
    uint32_t first = op == SPV_OP_EXT_INST ? 5 : 3, glsl_op = 0;
    /* most instructions have an operand of many values: a look at each before anything else */
    for (uint32_t k = first; k < width; k++) {
        uint32_t id = w[k], unknown = id < bound ? ~(zeros[id] | ones[id]) : 0;
        unknown &= unknown - 1; /* past its lowest bit not known, and below past the next */
        if (id >= bound ||
            (!(is[id] & SPV_OPT_CONSTANT) && !leaf[id] && !(is[type_of[id]] & SPV_OPT_BOOL) && unknown & (unknown - 1)))
            return SPV_OPT_FOLD_NONE;
    }
    if (op == SPV_OP_EXT_INST) {
        if (width < 5 || w[3] != glsl) return SPV_OPT_FOLD_NONE;
        glsl_op = w[4];
    }
    uint32_t operands = arity(op, glsl_op);
    if (!operands || width != first + operands) return SPV_OPT_FOLD_NONE;
    /* the kinds of the result and of the operands: logical instructions on booleans, comparisons of integers, a
       select's condition, the rest integers; a select or a copy of either */
    int logical = op >= SPV_OP_LOGICAL_EQUAL && op <= SPV_OP_LOGICAL_NOT;
    int compare = op >= SPV_OP_I_EQUAL && op <= SPV_OP_S_LESS_THAN_EQUAL,
        either = op == SPV_OP_SELECT || op == SPV_OPT_OP_COPY_OBJECT;
    uint32_t kind = is[w[1]] & (SPV_OPT_INT | SPV_OPT_BOOL);
    if (kind != (logical || compare ? SPV_OPT_BOOL : either ? kind : SPV_OPT_INT)) return SPV_OPT_FOLD_NONE;
    uint32_t entries[4][4], of[4], one = 0, n = 1, result[4] = {0}, own[4], m;
    for (uint32_t k = 0; k < operands; k++) {
        uint32_t want = logical                         ? SPV_OPT_BOOL
                        : op == SPV_OP_SELECT && k == 0 ? SPV_OPT_BOOL
                        : either                        ? kind
                                                        : SPV_OPT_INT;
        of[k] = few_of(w[first + k], want, bound, is, type_of, value, zeros, ones, leaf, table, count, entries[k], &m);
        if (of[k] == UINT32_MAX || (of[k] && one && of[k] != one)) return SPV_OPT_FOLD_NONE;
        if (of[k]) {
            one = of[k];
            n = m;
        }
    }
    for (uint32_t j = 0; j < n; j++) {
        uint32_t v[4] = {0};
        for (uint32_t k = 0; k < operands; k++) v[k] = entries[k][j];
        if (!evaluate(op, glsl_op, v, &result[j])) return SPV_OPT_FOLD_NONE;
    }
    uint32_t same = 1;
    for (uint32_t j = 1; j < n; j++) same &= result[j] == result[0];
    if (same) {
        *out = result[0];
        return SPV_OPT_FOLD_VALUE;
    }
    for (uint32_t k = 0; k < operands; k++)
        if (of[k] == one && type_of[w[first + k]] == w[1] && !memcmp(entries[k], result, n * sizeof *result)) {
            *out = w[first + k];
            return SPV_OPT_FOLD_ID;
        }
    if (type_of[one] == w[1] &&
        few_of(one, kind, bound, is, type_of, value, zeros, ones, leaf, table, count, own, &m) == one &&
        !memcmp(own, result, n * sizeof *result)) {
        *out = one;
        return SPV_OPT_FOLD_ID;
    }
    leaf[w[2]] = one;
    memcpy(table + 4 * (size_t)w[2], result, sizeof result);
    count[w[2]] = (uint8_t)n;
    return SPV_OPT_FOLD_NONE;
}

/* where the constant of type and value is in table (ids; 0 an empty slot), or the empty slot where it would go */
static uint32_t constant_slot(const uint32_t* table, uint32_t mask, const uint32_t* type_of, const uint32_t* value,
                              uint32_t type, uint32_t v) {
    uint32_t s = (type * 0x9e3779b1u ^ v * 0x85ebca6bu) >> 7 & mask;
    while (table[s] && (type_of[table[s]] != type || value[table[s]] != v)) s = (s + 1) & mask;
    return s;
}

size_t spv_opt_values(uint32_t* module, size_t words) {
    if (words <= SPV_OPT_HEADER) return words;
    uint32_t bound = module[3], glsl = 0;
    size_t first = 0, constants = 0;
    int wide = 0, buffer_block = 0; /* a 64-bit integer type; a block the module may write under Uniform */
    for (size_t i = SPV_OPT_HEADER; i < words && !first; i += module[i] >> 16) {
        const uint32_t* w = module + i;
        uint32_t width = w[0] >> 16, op = w[0] & 0xffff;
        assert(width && width <= words - i);
        if (op == SPV_OP_EXT_INST_IMPORT && width == 2 + sizeof spv_opt_glsl_name / sizeof *spv_opt_glsl_name &&
            !memcmp(w + 2, spv_opt_glsl_name, sizeof spv_opt_glsl_name))
            glsl = w[1];
        if (op == SPV_OP_TYPE_INT && width >= 3 && w[2] > 32) wide = 1;
        if (op == SPV_OP_DECORATE && width >= 3 && w[2] == SPV_OPT_DECORATION_BUFFER_BLOCK) buffer_block = 1;
        if (op >= SPV_OP_CONSTANT_TRUE && op <= SPV_OPT_OP_CONSTANT_NULL) constants++;
        if (op == SPV_OP_FUNCTION) first = i;
    }
    if (!first) return words;
    size_t count = 0, blocks = 0, edges = 0, numbered = 0; /* numbered: at most, the ones that make a result */
    int switches = 0;
    for (size_t i = first; i < words; i += module[i] >> 16) {
        uint32_t width = module[i] >> 16, op = module[i] & 0xffff;
        assert(width && width <= words - i);
        count++;
        if (op < 256 && (result_only[op / 32] >> (op % 32) & 1)) numbered++;
        if (op == SPV_OP_LABEL) {
            assert(width == 2 && module[i + 1] < bound);
            blocks++;
        } else if (op == SPV_OP_BRANCH) {
            assert(width == 2);
            edges++;
        } else if (op == SPV_OP_BRANCH_CONDITIONAL) {
            assert(width >= 4);
            edges += 2;
        } else if (op == SPV_OP_SWITCH) {
            assert(width >= 3);
            edges += 1 + (width - 3) / 2;
            switches = 1;
        }
    }
    /* a switch on a 64-bit selector has two words a case: its targets are not where this reads them */
    if (!blocks || !numbered || (switches && wide)) return words;
    size_t slots = 16, known = 16; /* the two tables: the instructions kept, and the constants */
    while (slots < 2 * numbered) slots *= 2;
    while (known < 2 * (constants + numbered)) known *= 2;
    /* One block. By id: what replaces it, its decorations (a bit each), its block when it is a label (from 1), its
       type, its bits when it is a constant, where its instruction is, the bits of its value known 0 and those known 1
       (known_bits; a boolean is 0 or 1), and what it is, a byte. By block: its
       immediate dominator, its number in the post order (from 1; 0 when nothing reaches it), where its successors
       and its predecessors start, the post order, and the stack that walks it. By edge: the block it goes to, and
       the predecessors' lists. By slot of the table, three words side by side: the instruction, its block and its
       scope. By slot of the constants' table: the id. By numbered instruction: the id of the constant it was made.
       By id again: the leaf its values are a function of and those values, four words (few_values). By instruction:
       its value_class, a byte; and by id, how many values its table has, a byte. */
    size_t ids = bound;
    assert(ids <= SIZE_MAX / 64 && blocks <= SIZE_MAX / 64 && edges <= SIZE_MAX / 64 && slots <= SIZE_MAX / 64 &&
           known <= SIZE_MAX / 64);
    uint32_t* replace = calloc(
        1, (13 * ids + 7 * blocks + 2 + 2 * edges + 3 * slots + known + numbered) * sizeof *replace + 2 * ids + count);
    if (!replace) abort();
    uint32_t *sign = replace + ids, *block_of = sign + ids, *type_of = block_of + ids, *value = type_of + ids;
    uint32_t *def = value + ids, *zeros = def + ids, *ones = zeros + ids, *idom = ones + ids, *order = idom + blocks;
    uint32_t *succ = order + blocks, *pred = succ + blocks + 1, *sequence = pred + blocks + 1;
    uint32_t *stack = sequence + blocks, *to = stack + 2 * blocks, *from = to + edges;
    uint32_t *slot = from + edges, *constant = slot + 3 * slots, *made = constant + known, nmade = 0;
    uint32_t *leaf = made + numbered, *table = leaf + ids;
    uint8_t *is = (uint8_t*)(table + 4 * ids), *class_of = is + ids, *count_of = class_of + count;

    /* the declarations: which ids are decorated and how, the variables no instruction can write, and the integer
       and boolean types with their constants (a type comes before the constants of it) */
    uint32_t kinds[32], nkinds = 0;
    for (size_t i = SPV_OPT_HEADER; i < first; i += module[i] >> 16) {
        const uint32_t* w = module + i;
        uint32_t width = w[0] >> 16, op = w[0] & 0xffff;
        if (op == SPV_OP_DECORATE && width == 3 && w[1] < bound) {
            uint32_t k = 0;
            while (k < nkinds && kinds[k] != w[2]) k++;
            if (k == nkinds && nkinds < 32) kinds[nkinds++] = w[2];
            if (k < 32)
                sign[w[1]] |= 1u << k;
            else
                is[w[1]] |= SPV_OPT_PINNED;
        } else if ((op == SPV_OP_DECORATE || op == SPV_OPT_OP_DECORATE_ID || op == SPV_OPT_OP_DECORATE_STRING) &&
                   width >= 2 && w[1] < bound) {
            is[w[1]] |= SPV_OPT_PINNED; /* a decoration with operands: its instruction is left alone */
        } else if (op == SPV_OPT_OP_GROUP_DECORATE || op == SPV_OPT_OP_GROUP_MEMBER_DECORATE) {
            for (uint32_t k = 1; k < width; k++)
                if (w[k] < bound) is[w[k]] |= SPV_OPT_PINNED;
        } else if (op == SPV_OP_VARIABLE && width >= 4 && w[2] < bound &&
                   (w[3] == SPV_STORAGE_UNIFORM_CONSTANT || w[3] == SPV_STORAGE_INPUT ||
                    w[3] == SPV_STORAGE_PUSH_CONSTANT || (w[3] == SPV_STORAGE_UNIFORM && !buffer_block))) {
            is[w[2]] |= SPV_OPT_READ_ONLY;
        } else if (op == SPV_OP_TYPE_INT && width >= 3 && w[1] < bound && w[2] == 32) {
            is[w[1]] |= SPV_OPT_INT;
        } else if (op == SPV_OP_TYPE_BOOL && width >= 2 && w[1] < bound) {
            is[w[1]] |= SPV_OPT_BOOL;
        } else if (op >= SPV_OP_CONSTANT_TRUE && op <= SPV_OPT_OP_CONSTANT_NULL && width >= 3 && w[1] < bound &&
                   w[2] < bound) {
            type_of[w[2]] = w[1];
            /* a constant of one word, true, false or null: fold asks its type before it reads the value */
            int number = op == SPV_OP_CONSTANT && width == 4;
            if (!number && op != SPV_OP_CONSTANT_TRUE && op != SPV_OP_CONSTANT_FALSE && op != SPV_OPT_OP_CONSTANT_NULL)
                continue;
            is[w[2]] |= SPV_OPT_CONSTANT;
            value[w[2]] = number ? w[3] : op == SPV_OP_CONSTANT_TRUE;
            ones[w[2]] = value[w[2]]; /* known_bits reads those of an integer or a boolean alone */
            zeros[w[2]] = ~value[w[2]];
            uint32_t s = constant_slot(constant, (uint32_t)known - 1, type_of, value, w[1], value[w[2]]);
            if (!constant[s]) constant[s] = w[2];
        }
    }

    /* the blocks and their edges; what an instruction of no known layout might name stays */
    uint32_t nblocks = 0, nedges = 0, n = 0;
    uint8_t by_opcode[256] = {0};
    int entry = 0, phis = 0;
    for (size_t i = first; i < words; i += module[i] >> 16) {
        const uint32_t* w = module + i;
        uint32_t width = w[0] >> 16, op = w[0] & 0xffff, kind, literal, more;
        /* what value_class and value_operands say, in a byte that is never 0 (ids follow, or a literal has a
           place): the class, the literal's place, whether ids follow it. An opcode's answer is kept for the module's
           next instruction of it, when it does not hang on the instruction's words (an extended instruction's set,
           a load's memory operand, a result's missing words) */
        int fixed = op < 256 && op != SPV_OP_EXT_INST && op != SPV_OP_LOAD &&
                    (width >= 3 || !(result_only[op / 32] >> (op % 32) & 1));
        if (fixed && by_opcode[op]) {
            kind = by_opcode[op] & 7;
            class_of[n++] = by_opcode[op];
        } else {
            kind = value_class(w, width, op, glsl);
            value_operands(op, &literal, &more);
            class_of[n] = (uint8_t)(kind | literal << 3 | more << 6);
            if (fixed) by_opcode[op] = class_of[n];
            n++;
        }
        assert(kind < SPV_OPT_PURE || w[2] < bound);
        if (op == SPV_OP_FUNCTION) {
            entry = 1;
        } else if (op == SPV_OP_LABEL) {
            idom[nblocks] = entry ? nblocks : UINT32_MAX; /* a function's first block is its own dominator */
            entry = 0;
            succ[nblocks] = nedges;
            block_of[w[1]] = ++nblocks;
        } else if (op == SPV_OP_BRANCH) {
            to[nedges++] = w[1];
        } else if (op == SPV_OP_BRANCH_CONDITIONAL) {
            to[nedges++] = w[2];
            to[nedges++] = w[3];
        } else if (op == SPV_OP_SWITCH) {
            to[nedges++] = w[2];
            for (uint32_t k = 4; k < width; k += 2) to[nedges++] = w[k];
        } else if (op == SPV_OP_PHI) {
            phis = 1;
        } else if (kind == SPV_OPT_UNKNOWN) {
            for (uint32_t k = 1; k < width; k++)
                if (w[k] < bound) is[w[k]] |= SPV_OPT_PINNED;
        }
    }
    assert(nblocks == blocks && nedges == edges);
    succ[nblocks] = nedges;
    for (uint32_t e = 0; e < nedges; e++) {
        assert(to[e] < bound && block_of[to[e]]);
        to[e] = block_of[to[e]] - 1;
        pred[to[e] + 1]++;
    }
    for (uint32_t b = 0; b < nblocks; b++) pred[b + 1] += pred[b];
    for (uint32_t b = 0; b < nblocks; b++)
        for (uint32_t e = succ[b]; e < succ[b + 1]; e++) from[pred[to[e]] + stack[to[e]]++] = b;

    /* each function's post order from its first block, then its dominators (Cooper, Harvey and Kennedy's walk of
       the reverse post order until nothing changes) */
    uint32_t done = 0;
    for (uint32_t root = 0; root < nblocks; root++) {
        if (idom[root] != root) continue;
        uint32_t start = done, depth = 0;
        stack[0] = root;
        stack[1] = succ[root];
        order[root] = UINT32_MAX; /* on the stack */
        for (;;) {
            uint32_t b = stack[2 * depth];
            if (stack[2 * depth + 1] < succ[b + 1]) {
                uint32_t next = to[stack[2 * depth + 1]++];
                if (order[next]) continue;
                order[next] = UINT32_MAX;
                depth++;
                stack[2 * depth] = next;
                stack[2 * depth + 1] = succ[next];
                continue;
            }
            sequence[done] = b;
            order[b] = ++done;
            if (!depth--) break;
        }
        for (int changed = 1; changed;) {
            changed = 0;
            for (uint32_t k = done - 1; k-- > start;) {
                uint32_t b = sequence[k], found = UINT32_MAX;
                for (uint32_t e = pred[b]; e < pred[b + 1]; e++) {
                    uint32_t p = from[e], q = found;
                    if (idom[p] == UINT32_MAX) continue;
                    if (q != UINT32_MAX)
                        while (p != q) {
                            while (order[p] < order[q]) p = idom[p];
                            while (order[q] < order[p]) q = idom[q];
                        }
                    found = p;
                }
                if (found != idom[b]) {
                    idom[b] = found;
                    changed = 1;
                }
            }
        }
    }

    /* The walk. A block comes after the blocks that dominate it, so an instruction's operands were looked at before
       it, but for a phi's, which the end fixes. A load of memory the module may write is numbered up to the next
       instruction that may write or branch: epoch counts those. */
    uint32_t block = 0, epoch = 0;
    uint32_t mask = (uint32_t)slots - 1;
    int any = 0;
    n = 0;
    for (size_t i = first; i < words; i += module[i] >> 16) {
        uint32_t* w = module + i;
        uint32_t width = w[0] >> 16, op = w[0] & 0xffff, kind = class_of[n] & 7;
        uint32_t literal = class_of[n] >> 3 & 7, more = class_of[n] >> 6 & 1;
        n++;
        if (kind == SPV_OPT_UNKNOWN) {
            epoch++;
            continue;
        }
        int result = op < 256 && (result_only[op / 32] >> (op % 32) & 1); /* a type, a result, then operands */
        uint32_t last = literal && !more && literal < width ? literal : width;
        /* nothing to read before the first instruction went; a type and a result are never replaced */
        for (uint32_t k = result ? 3 : 1; any && k < last; k++)
            if (k != literal && w[k] < bound && replace[w[k]]) w[k] = replace[w[k]];
        if (result && w[1] < bound && w[2] < bound) {
            type_of[w[2]] = w[1];
            def[w[2]] = (uint32_t)i;
        }
        if (kind == SPV_OPT_PLAIN) {
            if (op == SPV_OP_LABEL) block = block_of[w[1]] - 1;
            if (!only_result(w, width, op, glsl)) epoch++; /* a block's last instruction is one of these */
            continue;
        }
        uint32_t scope = 0;
        if (kind == SPV_OPT_IN_BLOCK) scope = block + 1;
        if (kind == SPV_OPT_LOADS && !(w[3] < bound && is[w[3]] & SPV_OPT_READ_ONLY)) scope = epoch;
        if ((op == SPV_OP_ACCESS_CHAIN || op == SPV_OPT_OP_IN_BOUNDS_ACCESS_CHAIN || op == SPV_OPT_OP_COPY_OBJECT) &&
            width >= 4 && w[3] < bound && is[w[3]] & SPV_OPT_READ_ONLY)
            is[w[2]] |= SPV_OPT_READ_ONLY; /* a pointer into what its base points into */
        if (is[w[2]] & SPV_OPT_PINNED) continue;
        uint32_t folded = 0, how = SPV_OPT_FOLD_NONE;
        /* fold looks at a 32-bit integer or a boolean, a select and a bitcast: the rest are not worth its call */
        if (kind == SPV_OPT_PURE && !sign[w[2]] && w[1] < bound &&
            (is[w[1]] & (SPV_OPT_INT | SPV_OPT_BOOL) || op == SPV_OP_SELECT || op == SPV_OP_BITCAST))
            how = fold(module, w, width, op, bound, is, type_of, value, def, sign, &folded);
        if (how == SPV_OPT_FOLD_NONE && kind == SPV_OPT_PURE && !sign[w[2]] && w[1] < bound &&
            is[w[1]] & (SPV_OPT_INT | SPV_OPT_BOOL)) {
            uint32_t zero, one;
            known_bits(w, width, op, bound, is, type_of, is[w[1]] & (SPV_OPT_INT | SPV_OPT_BOOL), zeros, ones, &zero,
                       &one);
            how = fold_known(w, width, op, bound, type_of, zeros, ones, zero, one, &folded);
            if (how == SPV_OPT_FOLD_NONE)
                how = few_values(w, width, op, glsl, bound, is, type_of, value, zeros, ones, leaf, table, count_of,
                                 &folded);
            if (how == SPV_OPT_FOLD_NONE && leaf[w[2]] && is[w[1]] & SPV_OPT_INT) { /* what its values share */
                uint32_t all = 0xffffffffu, any = 0;
                for (uint32_t j = 0; j < count_of[w[2]]; j++) {
                    all &= table[4 * (size_t)w[2] + j];
                    any |= table[4 * (size_t)w[2] + j];
                }
                zero |= ~any;
                one |= all;
            }
            zeros[w[2]] = zero;
            ones[w[2]] = one;
        }
        if (how == SPV_OPT_FOLD_VALUE) {
            uint32_t k = constant_slot(constant, (uint32_t)known - 1, type_of, value, w[1], folded);
            if (constant[k]) {
                folded = constant[k];
                how = SPV_OPT_FOLD_ID;
            } else { /* no constant of the module has the value: the instruction's id becomes one, declared at the end */
                constant[k] = w[2];
                value[w[2]] = folded;
                ones[w[2]] = folded;
                zeros[w[2]] = ~folded;
                is[w[2]] |= SPV_OPT_CONSTANT;
                made[nmade++] = w[2];
                w[0] = (w[0] & 0xffff0000u) | SPV_OPT_OP_NOP;
                any = 1;
                continue;
            }
        }
        if (how == SPV_OPT_FOLD_ID) {
            replace[w[2]] = folded;
            is[w[2]] = SPV_OPT_GONE;
            w[0] = (w[0] & 0xffff0000u) | SPV_OPT_OP_NOP;
            any = 1;
            continue;
        }
        int swap = commutes(op, width);
        uint32_t lo = swap && w[4] < w[3] ? w[4] : w[3], hash = (w[0] ^ scope * 0x9e3779b1u) * 0x85ebca6bu ^ w[1];
        if (swap) {
            hash = (hash ^ lo) * 0x01000193u;
            hash = (hash ^ (w[3] ^ w[4] ^ lo)) * 0x01000193u;
        } else
            for (uint32_t k = 3; k < width; k++) hash = (hash ^ w[k]) * 0x01000193u;
        uint32_t s = (hash ^ hash >> 15) & mask;
        const uint32_t* same = NULL;
        for (; slot[3 * s]; s = (s + 1) & mask) {
            const uint32_t* r = module + slot[3 * s];
            if (slot[3 * s + 2] != scope || r[0] != w[0] || r[1] != w[1] || sign[r[2]] != sign[w[2]]) continue;
            if (memcmp(r + 3, w + 3, (width - 3) * sizeof *w) && !(swap && r[3] == w[4] && r[4] == w[3])) continue;
            if (!dominates(idom, order, slot[3 * s + 1], block)) continue;
            same = r;
            break;
        }
        if (!same) {
            slot[3 * s] = (uint32_t)i;
            slot[3 * s + 1] = block;
            slot[3 * s + 2] = scope;
        } else {
            replace[w[2]] = same[2];
            is[w[2]] = SPV_OPT_GONE;
            w[0] = (w[0] & 0xffff0000u) | SPV_OPT_OP_NOP;
            any = 1;
        }
    }
    if (any) {
        /* a phi may read, from a block that branches back to it, what was replaced after the walk passed it */
        for (size_t i = first; phis && i < words; i += module[i] >> 16) {
            uint32_t* w = module + i;
            if ((w[0] & 0xffff) != SPV_OP_PHI) continue;
            for (uint32_t k = 3; k < w[0] >> 16; k += 2)
                if (w[k] < bound && replace[w[k]]) w[k] = replace[w[k]];
        }
        words = compact(module, words, bound, is);
        if (nmade) {
            /* the constants made go before the first function, into room their instructions left: one is never
               wider than the instruction it was */
            size_t at = SPV_OPT_HEADER, room = 0;
            while ((module[at] & 0xffff) != SPV_OP_FUNCTION) at += module[at] >> 16;
            for (uint32_t k = 0; k < nmade; k++) room += is[type_of[made[k]]] & SPV_OPT_BOOL ? 3 : 4;
            memmove(module + at + room, module + at, (words - at) * sizeof *module);
            for (uint32_t k = 0; k < nmade; k++) {
                uint32_t id = made[k], type = type_of[id];
                if (is[type] & SPV_OPT_BOOL) {
                    module[at++] = 3u << 16 | (value[id] ? SPV_OP_CONSTANT_TRUE : SPV_OP_CONSTANT_FALSE);
                    module[at++] = type;
                    module[at++] = id;
                } else {
                    module[at++] = 4u << 16 | SPV_OP_CONSTANT;
                    module[at++] = type;
                    module[at++] = id;
                    module[at++] = value[id];
                }
            }
            words += room;
        }
    }
    free(replace);
    return words;
}
