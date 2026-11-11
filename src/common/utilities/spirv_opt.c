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
    SPV_OPT_OP_IMAGE_SAMPLE_PROJ_EXPLICIT_LOD = 92,
    SPV_OPT_OP_IMAGE_SAMPLE_PROJ_DREF_IMPLICIT_LOD = 93,
    SPV_OPT_OP_IMAGE_SAMPLE_PROJ_DREF_EXPLICIT_LOD = 94,
    SPV_OPT_OP_DECORATE_ID = 332,
    SPV_OPT_OP_DECORATE_STRING = 5632,
    SPV_OPT_VOLATILE = 1,   /* OpLoad's memory operand */
    SPV_OPT_GLSL_MODF = 35, /* the two of GLSL.std.450 that write through a pointer operand */
    SPV_OPT_GLSL_FREXP = 51,
    SPV_OPT_SWEEPS = 16, /* the most dead code elimination walks the functions: what is left then stays */
    /* what a pass knows of an id */
    SPV_OPT_VARIABLE = 1, /* a Function variable */
    SPV_OPT_GONE = 2      /* its instruction was taken out */
};

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

size_t spv_opt_dead_code(uint32_t* module, size_t words) {
    /* "GLSL.std.450" as a module's words hold it, four characters a word from the low byte, on any host */
    static const uint32_t glsl_name[] = {0x4c534c47, 0x6474732e, 0x3035342e, 0};
    if (words <= SPV_OPT_HEADER) return words;
    uint32_t bound = module[3], glsl = 0;
    size_t first = 0; /* the first OpFunction: what comes before it is the module's declarations */
    for (size_t i = SPV_OPT_HEADER; i < words; i += module[i] >> 16) {
        uint32_t width = module[i] >> 16, op = module[i] & 0xffff;
        assert(width && width <= words - i);
        if (op == SPV_OP_EXT_INST_IMPORT && width == 2 + sizeof glsl_name / sizeof *glsl_name &&
            !memcmp(module + i + 2, glsl_name, sizeof glsl_name))
            glsl = module[i + 1];
        if (op == SPV_OP_FUNCTION && !first) first = i;
    }
    if (!first) return words;
    /* one block: each id's uses by the instructions left and, for a Function variable, the stores to it left; where
       each instruction of the functions starts (as many as their words at most); and what each id is, a byte */
    size_t body = words - first, ids = bound;
    assert(body <= SIZE_MAX / 8 && ids <= SIZE_MAX / 32);
    uint32_t* uses = calloc(1, (2 * ids + body) * sizeof *uses + ids);
    if (!uses) abort();
    uint32_t *stores = uses + bound, *at = stores + bound, count = 0;
    uint8_t* is = (uint8_t*)(at + body);
    for (size_t i = first; i < words; i += module[i] >> 16) {
        const uint32_t* w = module + i;
        uint32_t width = w[0] >> 16, op = w[0] & 0xffff, from, literal, more;
        at[count++] = (uint32_t)i;
        operand_ids(op, &literal, &more);
        if (op == SPV_OP_VARIABLE && width >= 4 && w[3] == SPV_STORAGE_FUNCTION && w[2] < bound) {
            is[w[2]] = SPV_OPT_VARIABLE;
            from = 4; /* its initializer */
        } else if (op == SPV_OP_STORE && width >= 3 && w[1] < bound && is[w[1]] == SPV_OPT_VARIABLE) {
            stores[w[1]]++;
            from = 2; /* the value: a store is no use of the variable it writes */
        } else {
            /* an instruction with a result does not use it, nor its type; one that stays may count both */
            from = only_result(w, width, op, glsl) ? 3 : 1;
        }
        if (literal && !more) width = width < literal ? width : literal;
        for (uint32_t k = from; k < width; k++)
            if (k != literal && w[k] < bound) uses[w[k]]++;
    }
    /* Backwards, so that what an instruction used is looked at after the instruction went. A variable's last read
       may go after the stores that follow it in the words were passed: those go in one more sweep */
    size_t gone = 0;
    int again = 1;
    for (int sweep = 0; again && sweep < SPV_OPT_SWEEPS; sweep++) {
        again = 0;
        for (uint32_t n = count; n-- > 0;) {
            uint32_t* w = module + at[n];
            uint32_t width = w[0] >> 16, op = w[0] & 0xffff, from, literal, more;
            if (op == SPV_OP_VARIABLE && width >= 4 && w[3] == SPV_STORAGE_FUNCTION && w[2] < bound) {
                if (uses[w[2]] || stores[w[2]]) continue;
                is[w[2]] = SPV_OPT_GONE;
                from = 4;
            } else if (op == SPV_OP_STORE && width >= 3 && w[1] < bound && is[w[1]] == SPV_OPT_VARIABLE) {
                if (uses[w[1]]) continue;
                stores[w[1]]--;
                from = 2;
            } else if (only_result(w, width, op, glsl) && w[2] < bound && !uses[w[2]]) {
                is[w[2]] = SPV_OPT_GONE;
                from = 3;
            } else
                continue;
            operand_ids(op, &literal, &more);
            if (literal && !more) width = width < literal ? width : literal;
            for (uint32_t k = from; k < width; k++) {
                if (k == literal || w[k] >= bound) continue;
                uses[w[k]]--;
                if (!uses[w[k]] && is[w[k]] == SPV_OPT_VARIABLE && stores[w[k]]) again = 1;
            }
            gone += w[0] >> 16;
            w[0] = (w[0] & 0xffff0000u) | SPV_OPT_OP_NOP;
        }
    }
    if (gone) {
        size_t to = SPV_OPT_HEADER;
        for (size_t i = SPV_OPT_HEADER; i < words;) {
            uint32_t width = module[i] >> 16, op = module[i] & 0xffff;
            /* a name or a decoration of what went goes too */
            int names = width >= 2 && module[i + 1] < bound && is[module[i + 1]] == SPV_OPT_GONE &&
                        (op == SPV_OP_NAME || op == SPV_OP_DECORATE || op == SPV_OPT_OP_DECORATE_ID ||
                         op == SPV_OPT_OP_DECORATE_STRING);
            if (op != SPV_OPT_OP_NOP && !names) {
                memmove(module + to, module + i, width * sizeof *module);
                to += width;
            }
            i += width;
        }
        words = to;
    }
    free(uses);
    return words;
}
