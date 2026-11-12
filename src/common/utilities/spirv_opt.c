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

/* What the instruction at w always equals, when its type is a 32-bit integer or the boolean and its operands say so
   (they were replaced already): SPV_OPT_FOLD_VALUE with the constant's bits in *out when they are all constants, or
   when one of them decides (x & 0, x * 0, x - x, x ^ x, false && x, true || x); SPV_OPT_FOLD_ID with an operand in
   *out when the others leave it as it is (x + 0, x - 0, x | 0, x ^ 0, x << 0, x * 1, x & ~0, x & x, x | x,
   true && x, false || x, x && x), only when that operand has the instruction's type. Of any type: a select whose
   condition is a constant or whose two values are one id, and a bitcast to the type its operand has, or of a
   bitcast from that type. A shift by 32 or more and a field past bit 32 are left: the module gives them no value.
   module is for the instruction that makes a bitcast's operand, def[] says where; is, type_of and value are by id. */
static uint32_t fold(const uint32_t* module, const uint32_t* w, uint32_t width, uint32_t op, uint32_t bound,
                     const uint8_t* is, const uint32_t* type_of, const uint32_t* value, const uint32_t* def,
                     uint32_t* out) {
    uint32_t t = w[1], a = width > 3 && w[3] < bound ? w[3] : 0, b = width > 4 && w[4] < bound ? w[4] : 0;
    uint32_t c = width > 5 && w[5] < bound ? w[5] : 0;
    /* a, b, c: the operands that are ids the pass has tables for (0 is none: no id is 0) */
    int ka = is[a] & SPV_OPT_CONSTANT, kb = is[b] & SPV_OPT_CONSTANT, kc = is[c] & SPV_OPT_CONSTANT;
    int ia = ka && is[type_of[a]] & SPV_OPT_INT, ib = kb && is[type_of[b]] & SPV_OPT_INT;
    int ba = ka && is[type_of[a]] & SPV_OPT_BOOL, bb = kb && is[type_of[b]] & SPV_OPT_BOOL;
    uint32_t va = value[a], vb = value[b], vc = value[c], v;
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
        if (ia && is[t] & SPV_OPT_INT) {
            *out = va;
            return SPV_OPT_FOLD_VALUE;
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
        if (ia && ib) {
            if (shift && vb >= 32) return SPV_OPT_FOLD_NONE;
            switch (op) {
                case SPV_OP_I_ADD: v = va + vb; break;
                case SPV_OP_I_SUB: v = va - vb; break;
                case SPV_OP_I_MUL: v = va * vb; break;
                case SPV_OP_BITWISE_AND: v = va & vb; break;
                case SPV_OP_BITWISE_OR: v = va | vb; break;
                case SPV_OP_BITWISE_XOR: v = va ^ vb; break;
                case SPV_OP_SHIFT_LEFT_LOGICAL: v = va << vb; break;
                case SPV_OP_SHIFT_RIGHT_LOGICAL: v = va >> vb; break;
                default: v = va >> vb | (va >> 31 && vb ? 0xffffffffu << (32 - vb) : 0); break; /* arithmetic */
            }
            *out = v;
            return SPV_OPT_FOLD_VALUE;
        }
        /* one constant, k, and the other operand, x: k on the right, or on the left of what commutes */
        uint32_t x = ib ? a : ia && (sum || product) ? b : 0, k = ib ? vb : va;
        if (x) {
            if ((k == 0 && product) || (k == 0xffffffffu && op == SPV_OP_BITWISE_OR)) {
                *out = k;
                return SPV_OPT_FOLD_VALUE;
            }
            if ((k == 0 && !product) || (k == 1 && op == SPV_OP_I_MUL) ||
                (k == 0xffffffffu && op == SPV_OP_BITWISE_AND)) {
                if (type_of[x] != t) return SPV_OPT_FOLD_NONE;
                *out = x;
                return SPV_OPT_FOLD_ID;
            }
        }
        if (a && a == b && (op == SPV_OP_I_SUB || op == SPV_OP_BITWISE_XOR)) {
            *out = 0;
            return SPV_OPT_FOLD_VALUE;
        }
        if (a && a == b && (op == SPV_OP_BITWISE_AND || op == SPV_OP_BITWISE_OR) && type_of[a] == t) {
            *out = a;
            return SPV_OPT_FOLD_ID;
        }
        return SPV_OPT_FOLD_NONE;
    }
    if (is[t] & SPV_OPT_INT && width == 4 && ia && (op == SPV_OP_NOT || op == SPV_OP_S_NEGATE)) {
        *out = op == SPV_OP_NOT ? ~va : 0u - va;
        return SPV_OPT_FOLD_VALUE;
    }
    if (is[t] & SPV_OPT_INT && width == 6 && ia && ib && kc && is[type_of[c]] & SPV_OPT_INT &&
        (op == SPV_OP_BIT_FIELD_U_EXTRACT || op == SPV_OP_BIT_FIELD_S_EXTRACT)) {
        if (vb > 32 || vc > 32 - vb) return SPV_OPT_FOLD_NONE;
        v = vc ? va >> vb & 0xffffffffu >> (32 - vc) : 0;
        if (op == SPV_OP_BIT_FIELD_S_EXTRACT && vc && vc < 32 && v >> (vc - 1)) v |= 0xffffffffu << vc;
        *out = v;
        return SPV_OPT_FOLD_VALUE;
    }
    if (is[t] & SPV_OPT_BOOL && width == 5 && ia && ib && op >= SPV_OP_I_EQUAL && op <= SPV_OP_S_LESS_THAN_EQUAL) {
        uint32_t sa = va ^ 0x80000000u, sb = vb ^ 0x80000000u; /* in the signed order */
        switch (op) {
            case SPV_OP_I_EQUAL: v = va == vb; break;
            case SPV_OP_I_NOT_EQUAL: v = va != vb; break;
            case SPV_OP_U_GREATER_THAN: v = va > vb; break;
            case SPV_OP_S_GREATER_THAN: v = sa > sb; break;
            case SPV_OP_U_GREATER_THAN_EQUAL: v = va >= vb; break;
            case SPV_OP_S_GREATER_THAN_EQUAL: v = sa >= sb; break;
            case SPV_OP_U_LESS_THAN: v = va < vb; break;
            case SPV_OP_S_LESS_THAN: v = sa < sb; break;
            case SPV_OP_U_LESS_THAN_EQUAL: v = va <= vb; break;
            default: v = sa <= sb; break;
        }
        *out = v;
        return SPV_OPT_FOLD_VALUE;
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
        if (!x || type_of[x] != t) return SPV_OPT_FOLD_NONE;
        *out = x;
        return SPV_OPT_FOLD_ID;
    }
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
    size_t blocks = 0, edges = 0, numbered = 0;
    int switches = 0;
    for (size_t i = first; i < words; i += module[i] >> 16) {
        uint32_t width = module[i] >> 16, op = module[i] & 0xffff;
        assert(width && width <= words - i);
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
        } else if (value_class(module + i, width, op, glsl) >= SPV_OPT_PURE) {
            assert(module[i + 2] < bound);
            numbered++;
        }
    }
    /* a switch on a 64-bit selector has two words a case: its targets are not where this reads them */
    if (!blocks || !numbered || (switches && wide)) return words;
    size_t slots = 16, known = 16; /* the two tables: the instructions kept, and the constants */
    while (slots < 2 * numbered) slots *= 2;
    while (known < 2 * (constants + numbered)) known *= 2;
    /* One block. By id: what replaces it, its decorations (a bit each), its block when it is a label (from 1), its
       type, its bits when it is a constant, where its instruction is, and what it is, a byte. By block: its immediate dominator, its number in the post order (from 1; 0 when nothing
       reaches it), where its successors and its predecessors start, the post order, and the stack that walks it. By
       edge: the block it goes to, and the predecessors' lists. By slot of the table: the instruction, its block and
       its scope. By slot of the constants' table: the id. By numbered instruction: the id of the constant it was
       made. */
    size_t ids = bound;
    assert(ids <= SIZE_MAX / 64 && blocks <= SIZE_MAX / 64 && edges <= SIZE_MAX / 64 && slots <= SIZE_MAX / 64 &&
           known <= SIZE_MAX / 64);
    uint32_t* replace =
        calloc(1, (6 * ids + 7 * blocks + 2 + 2 * edges + 3 * slots + known + numbered) * sizeof *replace + ids);
    if (!replace) abort();
    uint32_t *sign = replace + ids, *block_of = sign + ids, *type_of = block_of + ids, *value = type_of + ids;
    uint32_t *def = value + ids, *idom = def + ids, *order = idom + blocks;
    uint32_t *succ = order + blocks, *pred = succ + blocks + 1, *sequence = pred + blocks + 1;
    uint32_t *stack = sequence + blocks, *to = stack + 2 * blocks, *from = to + edges;
    uint32_t *slot_at = from + edges, *slot_block = slot_at + slots, *slot_scope = slot_block + slots;
    uint32_t *constant = slot_scope + slots, *made = constant + known, nmade = 0;
    uint8_t* is = (uint8_t*)(made + numbered);

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
            uint32_t s = constant_slot(constant, (uint32_t)known - 1, type_of, value, w[1], value[w[2]]);
            if (!constant[s]) constant[s] = w[2];
        }
    }

    /* the blocks and their edges; what an instruction of no known layout might name stays */
    uint32_t nblocks = 0, nedges = 0;
    int entry = 0, phis = 0;
    for (size_t i = first; i < words; i += module[i] >> 16) {
        const uint32_t* w = module + i;
        uint32_t width = w[0] >> 16, op = w[0] & 0xffff;
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
        } else if (value_class(w, width, op, glsl) == SPV_OPT_UNKNOWN) {
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
    for (size_t i = first; i < words; i += module[i] >> 16) {
        uint32_t* w = module + i;
        uint32_t width = w[0] >> 16, op = w[0] & 0xffff, kind = value_class(w, width, op, glsl), literal, more;
        if (kind == SPV_OPT_UNKNOWN) {
            epoch++;
            continue;
        }
        value_operands(op, &literal, &more);
        uint32_t last = literal && !more && literal < width ? literal : width;
        for (uint32_t k = 1; k < last; k++)
            if (k != literal && w[k] < bound && replace[w[k]]) w[k] = replace[w[k]];
        if (op < 256 && (result_only[op / 32] >> (op % 32) & 1) && w[1] < bound && w[2] < bound) {
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
        if (kind == SPV_OPT_PURE && !sign[w[2]] && w[1] < bound)
            how = fold(module, w, width, op, bound, is, type_of, value, def, &folded);
        if (how == SPV_OPT_FOLD_VALUE) {
            uint32_t k = constant_slot(constant, (uint32_t)known - 1, type_of, value, w[1], folded);
            if (constant[k]) {
                folded = constant[k];
                how = SPV_OPT_FOLD_ID;
            } else { /* no constant of the module has the value: the instruction's id becomes one, declared at the end */
                constant[k] = w[2];
                value[w[2]] = folded;
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
        for (; slot_at[s]; s = (s + 1) & mask) {
            const uint32_t* r = module + slot_at[s];
            if (slot_scope[s] != scope || r[0] != w[0] || r[1] != w[1] || sign[r[2]] != sign[w[2]]) continue;
            if (memcmp(r + 3, w + 3, (width - 3) * sizeof *w) && !(swap && r[3] == w[4] && r[4] == w[3])) continue;
            if (!dominates(idom, order, slot_block[s], block)) continue;
            same = r;
            break;
        }
        if (!same) {
            slot_at[s] = (uint32_t)i;
            slot_block[s] = block;
            slot_scope[s] = scope;
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
