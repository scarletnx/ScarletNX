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
    SPV_OPT_GONE = 2,     /* its instruction was taken out */
    /* what dead code elimination may do with an instruction */
    SPV_OPT_KEEP = 0,       /* it has an effect, or is gone already */
    SPV_OPT_RESULT = 1,     /* it only makes its result: it goes when nothing uses the result */
    SPV_OPT_LOCAL = 2,      /* a Function variable: it goes when nothing uses it and no store to it is left */
    SPV_OPT_LOCAL_STORE = 3 /* a store to one: it goes when nothing uses the variable */
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
    for (size_t i = SPV_OPT_HEADER; i < words && !first; i += module[i] >> 16) {
        uint32_t width = module[i] >> 16, op = module[i] & 0xffff;
        assert(width && width <= words - i);
        if (op == SPV_OP_EXT_INST_IMPORT && width == 2 + sizeof glsl_name / sizeof *glsl_name &&
            !memcmp(module + i + 2, glsl_name, sizeof glsl_name))
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
    if (any) {
        /* the instructions that stay, moved down a run at a time; a name or a decoration of what went goes too */
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
        words = to;
    }
    free(uses);
    return words;
}
