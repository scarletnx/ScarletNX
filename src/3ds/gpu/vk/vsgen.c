/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Azahar video_core/shader/generator/glsl_shader_decompiler.cpp (the control flow analysis, and what
   each instruction computes, written here as SPIR-V instead of GLSL), glsl_shader_gen.cpp (GenerateVertexShader
   without a geometry shader) and shader_gen.cpp (PicaVSConfigState, the semantic maps) (GPL-2.0-or-later, Copyright
   2017-2026 Citra Emulator Project / Azahar Emulator Project) at azahar-emu/azahar 49a1e8c, in C. The instruction
   fields are nihstro's shader_bytecode.h as gpu/shader.c reads them (BSD-3-Clause, Copyright 2014 Tony Wasserka; its
   notice is in shader.c). Multiplication is the host's (Azahar's sanitize_mul off), and RCP and RSQ skip a zero or
   negative operand as Azahar's do then. The pass-through shader for a vertex the CPU made is glsl_shader_gen.cpp's
   GenerateTrivialVertexShader. */
#include "3ds/gpu/vk/vsgen.h"
#include "3ds/gpu/pica_internal.h"
#include "common/utilities/bits.h"
#include "common/utilities/spirv_emit.h"
#include <stdlib.h>
#include <string.h>

#define PROGRAM_END 4096u

/* what an instruction does to the flow of the program, which the control flow analysis and the walks over a range
   read (step_of) */
typedef enum {
    STEP_NEXT,    /* on to the next instruction */
    STEP_END,     /* END */
    STEP_JUMP,    /* JMPC and JMPU: to dest, or on */
    STEP_CALL,    /* CALL: the range dest to dest + num, then on */
    STEP_CALL_IF, /* CALLC and CALLU */
    STEP_IF,      /* IFC and IFU: the body up to dest, or the else part, dest to dest + num */
    STEP_LOOP,    /* LOOP: the body up to dest, inclusive */
    STEP_UNKNOWN  /* BREAK, BREAKC, and an opcode with no instruction: Azahar does not translate them either */
} StepKind;
typedef struct {
    StepKind kind;
    uint32_t dest, num;
} Step;

/* the decoder that tools/decodetree makes from gpu/pica.decode: decode_pica(c, word) calls the trans_ function of the
   word's instruction, which gives its step and, when c has a Gen, compiles the instruction at c->offset */
typedef struct Gen Gen;
typedef struct {
    Gen* g;
    uint32_t offset;
    Step step;
} Decode;
typedef Decode DisasContext;
#include "common/utilities/decodetree.h"
/* the generated extractors take a context that they leave unused, as QEMU's do */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "decode-pica.inc"
#pragma GCC diagnostic pop
static Step step_of(uint32_t instr) {
    Decode c = {NULL, 0, {STEP_UNKNOWN, 0, 0}};
    decode_pica(&c, instr);
    return c.step;
}

/* ---- the control flow analysis: subroutines, the ranges a CALL, IF or LOOP names, each with its exit and the
   addresses JMPs inside it reach ---- */
typedef enum { EXIT_UNDETERMINED, EXIT_RETURN, EXIT_CONDITIONAL, EXIT_END } Exit;
typedef struct {
    uint32_t begin, end;
    Exit exit;
    uint32_t* labels; /* sorted, unique */
    unsigned label_count;
} Sub;
typedef struct {
    uint32_t begin, end;
    Exit exit;
} Scanned;
typedef struct {
    const uint32_t* code;
    Sub* subs;
    unsigned sub_count, sub_capacity;
    Scanned* scans;
    unsigned scan_count, scan_capacity;
    const char* fail;
} Flow;
typedef struct {
    uint32_t* items;
    unsigned count, capacity;
} Labels;
static bool label_add(Labels* l, uint32_t v) {
    unsigned at = 0;
    while (at < l->count && l->items[at] < v) ++at;
    if (at < l->count && l->items[at] == v) return true;
    if (l->count == l->capacity) {
        unsigned capacity = l->capacity ? l->capacity * 2 : 16;
        uint32_t* grown = realloc(l->items, capacity * sizeof *grown);
        if (!grown) return false;
        l->items = grown, l->capacity = capacity;
    }
    memmove(l->items + at + 1, l->items + at, (l->count - at) * sizeof *l->items);
    l->items[at] = v;
    ++l->count;
    return true;
}
static Sub* find_sub(const Flow* f, uint32_t begin, uint32_t end) {
    for (unsigned i = 0; i < f->sub_count; ++i)
        if (f->subs[i].begin == begin && f->subs[i].end == end) return &f->subs[i];
    return NULL;
}
static Exit parallel(Exit a, Exit b) {
    if (a == EXIT_UNDETERMINED) return b;
    if (b == EXIT_UNDETERMINED) return a;
    return a == b ? a : EXIT_CONDITIONAL;
}
static Exit series(Exit a, Exit b) {
    if (a == EXIT_UNDETERMINED) return EXIT_UNDETERMINED;
    if (a == EXIT_RETURN) return b;
    if (b == EXIT_UNDETERMINED || b == EXIT_END) return EXIT_END;
    return EXIT_CONDITIONAL;
}
static const Sub* add_sub(Flow* f, uint32_t begin, uint32_t end);
static Exit scan(Flow* f, uint32_t begin, uint32_t end, Labels* labels) {
    for (unsigned i = 0; i < f->scan_count; ++i)
        if (f->scans[i].begin == begin && f->scans[i].end == end) return f->scans[i].exit;
    if (f->scan_count == f->scan_capacity) {
        unsigned capacity = f->scan_capacity ? f->scan_capacity * 2 : 64;
        Scanned* grown = realloc(f->scans, capacity * sizeof *grown);
        if (!grown) {
            f->fail = "out of memory";
            return EXIT_UNDETERMINED;
        }
        f->scans = grown, f->scan_capacity = capacity;
    }
    unsigned slot = f->scan_count++;
    f->scans[slot] = (Scanned){begin, end, EXIT_UNDETERMINED};
    Exit result = EXIT_RETURN;
    for (uint32_t offset = begin; offset != end && offset < PROGRAM_END && !f->fail; ++offset) {
        Step st = step_of(f->code[offset]);
        if (st.kind == STEP_END) {
            result = EXIT_END;
            goto done;
        }
        if (st.kind == STEP_JUMP) {
            if (!label_add(labels, st.dest)) f->fail = "out of memory";
            Exit no_jump = scan(f, offset + 1, end, labels);
            Exit jump = scan(f, st.dest, end, labels);
            result = parallel(no_jump, jump);
            goto done;
        }
        if (st.kind == STEP_CALL) {
            const Sub* call = add_sub(f, st.dest, st.dest + st.num);
            if (!call) return EXIT_UNDETERMINED;
            if (call->exit == EXIT_END) {
                result = EXIT_END;
                goto done;
            }
            Exit exit = call->exit;
            result = series(exit, scan(f, offset + 1, end, labels));
            goto done;
        }
        if (st.kind == STEP_LOOP) {
            const Sub* loop = add_sub(f, offset + 1, st.dest + 1);
            if (!loop) return EXIT_UNDETERMINED;
            if (loop->exit == EXIT_END) {
                result = EXIT_END;
                goto done;
            }
            Exit exit = loop->exit;
            result = series(exit, scan(f, st.dest + 1, end, labels));
            goto done;
        }
        if (st.kind == STEP_CALL_IF) {
            const Sub* call = add_sub(f, st.dest, st.dest + st.num);
            if (!call) return EXIT_UNDETERMINED;
            Exit exit = call->exit;
            result = series(parallel(exit, EXIT_RETURN), scan(f, offset + 1, end, labels));
            goto done;
        }
        if (st.kind == STEP_IF) {
            const Sub* if_sub = add_sub(f, offset + 1, st.dest);
            if (!if_sub) return EXIT_UNDETERMINED;
            Exit if_exit = if_sub->exit, else_exit = EXIT_RETURN;
            if (st.num) {
                const Sub* else_sub = add_sub(f, st.dest, st.dest + st.num);
                if (!else_sub) return EXIT_UNDETERMINED;
                else_exit = else_sub->exit;
            }
            Exit both = parallel(if_exit, else_exit);
            if (both == EXIT_END) {
                result = EXIT_END;
                goto done;
            }
            result = series(both, scan(f, st.dest + st.num, end, labels));
            goto done;
        }
    }
done:
    /* the scans array may have grown (moved) during the recursion: find the slot again by its index */
    f->scans[slot].exit = result;
    return result;
}
static const Sub* add_sub(Flow* f, uint32_t begin, uint32_t end) {
    const Sub* known = find_sub(f, begin, end);
    if (known) return known;
    if (f->fail) return NULL;
    Labels labels = {0};
    Exit exit = scan(f, begin, end, &labels);
    if (f->fail) {
        free(labels.items);
        return NULL;
    }
    if (exit == EXIT_UNDETERMINED) {
        free(labels.items);
        f->fail = "a recursive function";
        return NULL;
    }
    if (f->sub_count == f->sub_capacity) {
        unsigned capacity = f->sub_capacity ? f->sub_capacity * 2 : 32;
        Sub* grown = realloc(f->subs, capacity * sizeof *grown);
        if (!grown) {
            free(labels.items);
            f->fail = "out of memory";
            return NULL;
        }
        f->subs = grown, f->sub_capacity = capacity;
    }
    /* kept in (begin, end) order, as Azahar's std::set orders them */
    unsigned at = 0;
    while (at < f->sub_count && (f->subs[at].begin < begin || (f->subs[at].begin == begin && f->subs[at].end < end)))
        ++at;
    memmove(f->subs + at + 1, f->subs + at, (f->sub_count - at) * sizeof *f->subs);
    f->subs[at] = (Sub){begin, end, exit, labels.items, labels.count};
    ++f->sub_count;
    return &f->subs[at];
}
static void flow_free(Flow* f) {
    for (unsigned i = 0; i < f->sub_count; ++i) free(f->subs[i].labels);
    free(f->subs);
    free(f->scans);
}
/* ---- the SPIR-V of the program: each subroutine a function returning whether the program ended in it, the
   registers Private variables they share (as the GLSL's globals were) ---- */
struct Gen {
    const SnxCtrShaderSetup* setup;
    const SnxCtrVsConfig* config;
    Flow* flow;
    SpvEmitter* s;
    uint32_t* sub_ids; /* the function of each of flow's subs */
    unsigned current;  /* the sub being written */
    /* the calls between subs (caller, callee), to refuse a recursion the analysis let through, as GLSL did */
    unsigned (*calls)[2];
    size_t call_count, call_capacity;
    bool open;         /* the block being written has no terminator yet */
    /* the sub being written when it has labels: its jump target variable and its switch's merge block */
    uint32_t jmp_to, switch_merge;
    uint32_t t_bool, t_bool4, t_int, t_uint, t_float, t_vec2, t_vec3, t_vec4, t_uvec4;
    uint32_t pica, data;                   /* the uniform blocks */
    uint32_t in_reg[16], tmp[16], out[16]; /* Private vec4 registers, the first two made when they are read */
    uint32_t addr[3];                      /* a0.x, a0.y and aL, Private int */
    uint32_t cc[2];                        /* the conditional codes, Private bool */
    const char* fail;
};

static uint32_t sconst(Gen* g, int32_t v) { return spv_emit_const_i32(g->s, v); }
static uint32_t uconst(Gen* g, uint32_t v) { return spv_emit_const_u32(g->s, v); }
static uint32_t fconst(Gen* g, float v) { return spv_emit_const_f32(g->s, v); }
static uint32_t vec4_const(Gen* g, float x, float y, float z, float w) {
    uint32_t parts[4] = {fconst(g, x), fconst(g, y), fconst(g, z), fconst(g, w)};
    return spv_emit_const_composite(g->s, g->t_vec4, parts, 4);
}
static uint32_t load(Gen* g, uint32_t type, uint32_t pointer) { return SPV(g->s, SPV_OP_LOAD, type, pointer); }
static void store(Gen* g, uint32_t pointer, uint32_t value) { SPV_VOID(g->s, SPV_OP_STORE, pointer, value); }
static uint32_t extract(Gen* g, uint32_t type, uint32_t composite, uint32_t index) {
    return SPV(g->s, SPV_OP_COMPOSITE_EXTRACT, type, composite, index);
}
static uint32_t splat4(Gen* g, uint32_t type, uint32_t v) {
    return SPV(g->s, SPV_OP_COMPOSITE_CONSTRUCT, type, v, v, v, v);
}
static uint32_t private_vec4(Gen* g) { return spv_emit_variable(g->s, SPV_STORAGE_PRIVATE, g->t_vec4); }
/* uniforms.f[index], uniforms.i[index] and uniforms.b */
static uint32_t uniform_f(Gen* g, uint32_t index) {
    uint32_t pointer = spv_emit_type_pointer(g->s, SPV_STORAGE_UNIFORM, g->t_vec4);
    return load(g, g->t_vec4, SPV(g->s, SPV_OP_ACCESS_CHAIN, pointer, g->pica, sconst(g, 2), index));
}
static uint32_t uniform_i(Gen* g, unsigned index) {
    uint32_t pointer = spv_emit_type_pointer(g->s, SPV_STORAGE_UNIFORM, g->t_uvec4);
    uint32_t element = SPV(g->s, SPV_OP_ACCESS_CHAIN, pointer, g->pica, sconst(g, 1), sconst(g, (int32_t)index));
    return load(g, g->t_uvec4, element);
}
static uint32_t uniform_b(Gen* g) {
    uint32_t pointer = spv_emit_type_pointer(g->s, SPV_STORAGE_UNIFORM, g->t_uint);
    return load(g, g->t_uint, SPV(g->s, SPV_OP_ACCESS_CHAIN, pointer, g->pica, sconst(g, 0)));
}

/* structured control flow: a branch closing the block being written (when it is open), a block placed after it
   (falling into it from an open block, as the GLSL's code fell through), a return */
static void jump(Gen* g, uint32_t target) {
    if (g->open) SPV_VOID(g->s, SPV_OP_BRANCH, target);
    g->open = false;
}
static void place(Gen* g, uint32_t label) {
    jump(g, label);
    spv_emit_label(g->s, label);
    g->open = true;
}
static void ret(Gen* g, bool value) {
    SPV_VOID(g->s, SPV_OP_RETURN_VALUE, spv_emit_const_bool(g->s, value));
    g->open = false;
}
/* if (cond) {: the then block is placed, and the merge block returned for place() to close it */
static uint32_t if_begin(Gen* g, uint32_t cond) {
    uint32_t then = spv_emit_id(g->s), merge = spv_emit_id(g->s);
    SPV_VOID(g->s, SPV_OP_SELECTION_MERGE, merge, SPV_SELECTION_CONTROL_NONE);
    SPV_VOID(g->s, SPV_OP_BRANCH_CONDITIONAL, cond, then, merge);
    g->open = false;
    place(g, then);
    return merge;
}

static const Sub* sub(Gen* g, uint32_t begin, uint32_t end) {
    const Sub* s = find_sub(g->flow, begin, end);
    if (!s && !g->fail) g->fail = "a subroutine the analysis did not find";
    return s;
}
static void call_sub(Gen* g, const Sub* s) {
    if (!s) return;
    if (g->call_count == g->call_capacity) {
        size_t capacity = g->call_capacity ? g->call_capacity * 2 : 64;
        unsigned(*grown)[2] = realloc(g->calls, capacity * sizeof *grown);
        if (!grown) {
            g->fail = "out of memory";
            return;
        }
        g->calls = grown, g->call_capacity = capacity;
    }
    g->calls[g->call_count][0] = g->current;
    g->calls[g->call_count++][1] = (unsigned)(s - g->flow->subs);
    uint32_t ended = SPV(g->s, SPV_OP_FUNCTION_CALL, g->t_bool, g->sub_ids[s - g->flow->subs]);
    if (s->exit == EXIT_END)
        ret(g, true);
    else if (s->exit == EXIT_CONDITIONAL) {
        uint32_t merge = if_begin(g, ended);
        ret(g, true);
        place(g, merge);
    }
}
static uint32_t cc_bit(Gen* g, unsigned i, bool set) {
    uint32_t v = load(g, g->t_bool, g->cc[i]);
    return set ? v : SPV(g->s, SPV_OP_LOGICAL_NOT, g->t_bool, v);
}
static uint32_t condition(Gen* g, const arg_cond* x) {
    if (x->cond == 2) return cc_bit(g, 0, x->refx);
    if (x->cond == 3) return cc_bit(g, 1, x->refy);
    uint32_t a = cc_bit(g, 0, x->refx), b = cc_bit(g, 1, x->refy);
    return SPV(g->s, x->cond == 0 ? SPV_OP_LOGICAL_OR : SPV_OP_LOGICAL_AND, g->t_bool, a, b); /* any, all */
}
static uint32_t uniform_bool(Gen* g, uint32_t index, bool invert) {
    uint32_t masked = SPV(g->s, SPV_OP_BITWISE_AND, g->t_uint, uniform_b(g), uconst(g, 1u << index));
    return SPV(g->s, invert ? SPV_OP_I_EQUAL : SPV_OP_I_NOT_EQUAL, g->t_bool, masked, uconst(g, 0));
}
/* get_offset_register: uniforms.f[(base + offset) & 0x7f] with an offset outside -128..127 taken as 0, and 1.0 past
   the 96 registers (the load clamped to them and the result selected, which keeps it in bounds) */
static uint32_t offset_register(Gen* g, unsigned base, uint32_t offset) {
    uint32_t low = SPV(g->s, SPV_OP_S_GREATER_THAN_EQUAL, g->t_bool, offset, sconst(g, -128));
    uint32_t high = SPV(g->s, SPV_OP_S_LESS_THAN_EQUAL, g->t_bool, offset, sconst(g, 127));
    uint32_t fixed = SPV(g->s, SPV_OP_SELECT, g->t_int, SPV(g->s, SPV_OP_LOGICAL_AND, g->t_bool, low, high), offset,
                         sconst(g, 0));
    uint32_t sum = SPV(g->s, SPV_OP_I_ADD, g->t_int, sconst(g, (int32_t)base), fixed);
    uint32_t wrapped = SPV(g->s, SPV_OP_BITWISE_AND, g->t_int, sum, sconst(g, 0x7f));
    uint32_t index = SPV(g->s, SPV_OP_BITCAST, g->t_uint, wrapped);
    uint32_t inside = SPV(g->s, SPV_OP_U_LESS_THAN, g->t_bool, index, uconst(g, 96));
    uint32_t v = uniform_f(g, SPV_EXT(g->s, g->t_uint, SPV_GLSL_U_MIN, index, uconst(g, 95)));
    return SPV(g->s, SPV_OP_SELECT, g->t_vec4, splat4(g, g->t_bool4, inside), v, vec4_const(g, 1, 1, 1, 1));
}
static uint32_t temporary(Gen* g, unsigned i) {
    if (!g->tmp[i]) g->tmp[i] = private_vec4(g);
    return g->tmp[i];
}
/* a source register (0x00-0x0f inputs, 0x10-0x1f temporaries, 0x20-0x7f float uniforms) with its swizzle */
static uint32_t source(Gen* g, unsigned reg, unsigned address, uint32_t pattern, unsigned n) {
    static const unsigned select_at[3] = {5, 14, 23}, negate_at[3] = {4, 13, 22};
    uint32_t v;
    if (reg < 0x10) {
        if (!g->in_reg[reg]) g->in_reg[reg] = private_vec4(g);
        v = load(g, g->t_vec4, g->in_reg[reg]);
    } else if (reg < 0x20)
        v = load(g, g->t_vec4, temporary(g, reg - 0x10));
    else if (address)
        v = offset_register(g, reg - 0x20, load(g, g->t_int, g->addr[address - 1]));
    else
        v = uniform_f(g, sconst(g, (int32_t)(reg - 0x20)));
    unsigned sel[4];
    for (unsigned i = 0; i < 4; ++i) sel[i] = snx_bits(pattern, select_at[n - 1] + (3 - i) * 2, 2);
    if (sel[0] != 0 || sel[1] != 1 || sel[2] != 2 || sel[3] != 3)
        v = SPV(g->s, SPV_OP_VECTOR_SHUFFLE, g->t_vec4, v, v, sel[0], sel[1], sel[2], sel[3]);
    if (snx_bits(pattern, negate_at[n - 1], 1)) v = SPV(g->s, SPV_OP_F_NEGATE, g->t_vec4, v);
    return v;
}
/* the destination register's variable, 0 for an output the mask leaves out */
static uint32_t destination(Gen* g, unsigned reg) {
    if (reg < 0x10) return g->config->output_map[reg] < g->config->num_outputs ? g->out[g->config->output_map[reg]] : 0;
    if (reg < 0x20) return temporary(g, reg - 0x10);
    return 0;
}
/* SetDest: the enabled components of the destination get the value's same components, a scalar spread over them */
static void set_dest(Gen* g, uint32_t pattern, uint32_t var, uint32_t value, bool scalar) {
    unsigned mask = pattern & 0xf;
    if (!var || !mask) return;
    if (scalar) value = splat4(g, g->t_vec4, value);
    if (mask != 0xf) {
        uint32_t old = load(g, g->t_vec4, var), c[4];
        for (unsigned i = 0; i < 4; ++i) c[i] = pattern & (8u >> i) ? 4 + i : i;
        value = SPV(g->s, SPV_OP_VECTOR_SHUFFLE, g->t_vec4, old, value, c[0], c[1], c[2], c[3]);
    }
    store(g, var, value);
}
/* where the range goes on after the instruction at offset, what compile_instr returns */
static uint32_t next_offset(Gen* g, uint32_t offset) {
    Step st = step_of(g->setup->code[offset]);
    switch (st.kind) {
        case STEP_NEXT:
        case STEP_JUMP:
        case STEP_CALL_IF: return offset + 1;
        case STEP_END: return PROGRAM_END;
        case STEP_CALL: {
            const Sub* s = find_sub(g->flow, st.dest, st.dest + st.num);
            return s && s->exit == EXIT_END ? PROGRAM_END : offset + 1;
        }
        case STEP_IF: {
            uint32_t else_offset = st.dest, endif_offset = else_offset + st.num;
            if (!st.num) return else_offset;
            const Sub* if_sub = find_sub(g->flow, offset + 1, else_offset);
            const Sub* else_sub = find_sub(g->flow, else_offset, endif_offset);
            return if_sub && else_sub && if_sub->exit == EXIT_END && else_sub->exit == EXIT_END ? PROGRAM_END
                                                                                                : endif_offset;
        }
        case STEP_LOOP: {
            const Sub* s = find_sub(g->flow, offset + 1, st.dest + 1);
            return s && s->exit == EXIT_END ? PROGRAM_END : st.dest + 1;
        }
        default: return PROGRAM_END;
    }
}

/* ---- the trans_ functions of pica.decode's patterns. Each gives the instruction's step, then, when the context has a
   Gen, compiles it ---- */
static bool emits(Decode* c, StepKind kind, uint32_t dest, uint32_t num) {
    c->step = (Step){kind, dest, num};
    return c->g != NULL;
}

/* an arithmetic instruction's sources, swizzled (inverted: the I forms, whose address register offsets src2), and its
   destination's variable */
typedef struct {
    uint32_t pattern, a, b, d;
} Operands;
static Operands operands(Gen* g, const arg_arith* x, bool inverted) {
    Operands o;
    o.pattern = g->setup->swizzle[x->desc];
    o.a = source(g, x->src1, inverted ? 0 : x->addr, o.pattern, 1);
    o.b = source(g, x->src2, inverted ? x->addr : 0, o.pattern, 2);
    o.d = destination(g, x->dest);
    return o;
}
/* an arithmetic instruction that sets the destination to value, a scalar spread over the enabled components or a
   vec4, of o's sources */
#define SET(NAME, INVERTED, SCALAR, value)                                                                             \
    static bool trans_##NAME(Decode* c, arg_##NAME* x) {                                                               \
        if (!emits(c, STEP_NEXT, 0, 0)) return true;                                                                   \
        Gen* g = c->g;                                                                                                 \
        Operands o = operands(g, x, INVERTED);                                                                         \
        set_dest(g, o.pattern, o.d, value, SCALAR);                                                                    \
        return true;                                                                                                   \
    }
SET(ADD, false, false, SPV(g->s, SPV_OP_F_ADD, g->t_vec4, o.a, o.b))
SET(MUL, false, false, SPV(g->s, SPV_OP_F_MUL, g->t_vec4, o.a, o.b))
SET(FLR, false, false, SPV_EXT(g->s, g->t_vec4, SPV_GLSL_FLOOR, o.a))
SET(MAX, false, false, SPV_EXT(g->s, g->t_vec4, SPV_GLSL_F_MAX, o.a, o.b))
SET(MIN, false, false, SPV_EXT(g->s, g->t_vec4, SPV_GLSL_F_MIN, o.a, o.b))
SET(MOV, false, false, o.a)
SET(EX2, false, true, SPV_EXT(g->s, g->t_float, SPV_GLSL_EXP2, extract(g, g->t_float, o.a, 0)))
SET(LG2, false, true, SPV_EXT(g->s, g->t_float, SPV_GLSL_LOG2, extract(g, g->t_float, o.a, 0)))

static bool trans_DP3(Decode* c, arg_DP3* x) {
    if (!emits(c, STEP_NEXT, 0, 0)) return true;
    Gen* g = c->g;
    Operands o = operands(g, x, false);
    uint32_t a3 = SPV(g->s, SPV_OP_VECTOR_SHUFFLE, g->t_vec3, o.a, o.a, 0, 1, 2);
    uint32_t b3 = SPV(g->s, SPV_OP_VECTOR_SHUFFLE, g->t_vec3, o.b, o.b, 0, 1, 2);
    set_dest(g, o.pattern, o.d, SPV(g->s, SPV_OP_DOT, g->t_float, a3, b3), true);
    return true;
}
/* DP4, and DPH and DPHI (homogeneous): Azahar without sanitize_mul dots the sources as they are; DPH's w of 1.0 is
   the hardware's */
static bool dot4(Decode* c, const arg_arith* x, bool inverted, bool homogeneous) {
    if (!emits(c, STEP_NEXT, 0, 0)) return true;
    Gen* g = c->g;
    Operands o = operands(g, x, inverted);
    uint32_t a = o.a;
    if (homogeneous) a = SPV(g->s, SPV_OP_VECTOR_SHUFFLE, g->t_vec4, a, vec4_const(g, 0, 0, 0, 1), 0, 1, 2, 7);
    set_dest(g, o.pattern, o.d, SPV(g->s, SPV_OP_DOT, g->t_float, a, o.b), true);
    return true;
}
static bool trans_DP4(Decode* c, arg_DP4* x) { return dot4(c, x, false, false); }
static bool trans_DPH(Decode* c, arg_DPH* x) { return dot4(c, x, false, true); }
static bool trans_DPHI(Decode* c, arg_DPHI* x) { return dot4(c, x, true, true); }

/* RCP and RSQ: Azahar without sanitize_mul: a zero operand (RSQ: one not above zero) leaves the destination alone,
   which avoids NaN; a NaN one passes RCP's != as it did in GLSL */
static bool reciprocal(Decode* c, const arg_arith* x, bool square_root) {
    if (!emits(c, STEP_NEXT, 0, 0)) return true;
    Gen* g = c->g;
    SpvEmitter* s = g->s;
    Operands o = operands(g, x, false);
    uint32_t f = g->t_float, v = extract(g, f, o.a, 0);
    uint32_t ok = !square_root ? SPV(s, SPV_OP_F_UNORD_NOT_EQUAL, g->t_bool, v, fconst(g, 0))
                               : SPV(s, SPV_OP_F_ORD_GREATER_THAN, g->t_bool, v, fconst(g, 0));
    uint32_t merge = if_begin(g, ok);
    uint32_t value = !square_root ? SPV(s, SPV_OP_F_DIV, f, fconst(g, 1), v) : SPV_EXT(s, f, SPV_GLSL_INVERSE_SQRT, v);
    set_dest(g, o.pattern, o.d, value, true);
    place(g, merge);
    return true;
}
static bool trans_RCP(Decode* c, arg_RCP* x) { return reciprocal(c, x, false); }
static bool trans_RSQ(Decode* c, arg_RSQ* x) { return reciprocal(c, x, true); }

/* address_registers.xy = ivec2(a), each enabled one of x and y */
static bool trans_MOVA(Decode* c, arg_MOVA* x) {
    if (!emits(c, STEP_NEXT, 0, 0)) return true;
    Gen* g = c->g;
    Operands o = operands(g, x, false);
    for (unsigned i = 0; i < 2; ++i)
        if (o.pattern & (8u >> i))
            store(g, g->addr[i], SPV(g->s, SPV_OP_CONVERT_F_TO_S, g->t_int, extract(g, g->t_float, o.a, i)));
    return true;
}

/* SGE, SLT and their I forms: 1.0 where the comparison holds, else 0.0 */
static bool set_compare(Decode* c, const arg_arith* x, bool inverted, uint32_t compare) {
    if (!emits(c, STEP_NEXT, 0, 0)) return true;
    Gen* g = c->g;
    Operands o = operands(g, x, inverted);
    uint32_t mask = SPV(g->s, compare, g->t_bool4, o.a, o.b);
    uint32_t ones = vec4_const(g, 1, 1, 1, 1), zeros = vec4_const(g, 0, 0, 0, 0);
    set_dest(g, o.pattern, o.d, SPV(g->s, SPV_OP_SELECT, g->t_vec4, mask, ones, zeros), false);
    return true;
}
static bool trans_SGE(Decode* c, arg_SGE* x) { return set_compare(c, x, false, SPV_OP_F_ORD_GREATER_THAN_EQUAL); }
static bool trans_SGEI(Decode* c, arg_SGEI* x) { return set_compare(c, x, true, SPV_OP_F_ORD_GREATER_THAN_EQUAL); }
static bool trans_SLT(Decode* c, arg_SLT* x) { return set_compare(c, x, false, SPV_OP_F_ORD_LESS_THAN); }
static bool trans_SLTI(Decode* c, arg_SLTI* x) { return set_compare(c, x, true, SPV_OP_F_ORD_LESS_THAN); }

static bool trans_CMP(Decode* c, arg_CMP* x) {
    if (!emits(c, STEP_NEXT, 0, 0)) return true;
    Gen* g = c->g;
    /* ==, !=, <, <=, >, >= as GLSL compiles them (!= unordered) */
    static const uint32_t compare[6] = {SPV_OP_F_ORD_EQUAL,        SPV_OP_F_UNORD_NOT_EQUAL,
                                        SPV_OP_F_ORD_LESS_THAN,    SPV_OP_F_ORD_LESS_THAN_EQUAL,
                                        SPV_OP_F_ORD_GREATER_THAN, SPV_OP_F_ORD_GREATER_THAN_EQUAL};
    uint32_t pattern = g->setup->swizzle[x->desc];
    uint32_t a = source(g, x->src1, x->addr, pattern, 1), b = source(g, x->src2, 0, pattern, 2);
    /* the register the arithmetic format's destination field names (bits 21-25, the comparisons' low bits): the
       other arithmetic instructions' code makes its variable, which the module keeps */
    destination(g, (unsigned)(x->cmpx & 3) << 3 | (unsigned)x->cmpy);
    unsigned mode[2] = {(unsigned)x->cmpx, (unsigned)x->cmpy};
    if (mode[0] > 5 || mode[1] > 5) return true; /* an unknown compare mode: Azahar logs it and leaves the codes */
    for (unsigned i = 0; i < 2; ++i)
        store(g, g->cc[i],
              SPV(g->s, compare[mode[i]], g->t_bool, extract(g, g->t_float, a, i), extract(g, g->t_float, b, i)));
    return true;
}

/* DST, LITP and DSTI */
static bool untranslated_arithmetic(Decode* c) {
    if (emits(c, STEP_NEXT, 0, 0)) c->g->fail = "an arithmetic instruction Azahar does not translate either";
    return true;
}
static bool trans_DST(Decode* c, arg_DST* x) { return (void)x, untranslated_arithmetic(c); }
static bool trans_LITP(Decode* c, arg_LITP* x) { return (void)x, untranslated_arithmetic(c); }
static bool trans_DSTI(Decode* c, arg_DSTI* x) { return (void)x, untranslated_arithmetic(c); }

/* MAD and MADI (inverted: src3 takes the address register) */
static bool multiply_add(Decode* c, const arg_mad* x, bool inverted) {
    if (!emits(c, STEP_NEXT, 0, 0)) return true;
    Gen* g = c->g;
    uint32_t pattern = g->setup->swizzle[x->desc];
    uint32_t a = source(g, x->src1, 0, pattern, 1);
    uint32_t b = source(g, x->src2, inverted ? 0 : x->addr, pattern, 2);
    uint32_t d = source(g, x->src3, inverted ? x->addr : 0, pattern, 3);
    uint32_t product = SPV(g->s, SPV_OP_F_MUL, g->t_vec4, a, b);
    set_dest(g, pattern, destination(g, x->dest), SPV(g->s, SPV_OP_F_ADD, g->t_vec4, product, d), false);
    return true;
}
static bool trans_MAD(Decode* c, arg_MAD* x) { return multiply_add(c, x, false); }
static bool trans_MADI(Decode* c, arg_MADI* x) { return multiply_add(c, x, true); }

/* ---- flow control ---- */
static bool trans_NOP(Decode* c, arg_NOP* x) { return (void)x, emits(c, STEP_NEXT, 0, 0), true; }
/* a geometry shader's: nothing in a vertex shader */
static bool trans_EMIT(Decode* c, arg_EMIT* x) { return (void)x, emits(c, STEP_NEXT, 0, 0), true; }
static bool trans_SETEMIT(Decode* c, arg_SETEMIT* x) { return (void)x, emits(c, STEP_NEXT, 0, 0), true; }
static bool trans_END(Decode* c, arg_END* x) {
    (void)x;
    if (emits(c, STEP_END, 0, 0)) ret(c->g, true);
    return true;
}
/* BREAK and BREAKC */
static bool untranslated(Decode* c) {
    if (emits(c, STEP_UNKNOWN, 0, 0)) c->g->fail = "an instruction Azahar does not translate either";
    return true;
}
static bool trans_BREAK(Decode* c, arg_BREAK* x) { return (void)x, untranslated(c); }
static bool trans_BREAKC(Decode* c, arg_BREAKC* x) { return (void)x, untranslated(c); }

static void jump_to(Gen* g, uint32_t cond, uint32_t dest) {
    if (!g->jmp_to) {
        /* a range the analysis took from its cache without the labels: the GLSL had no loop to break */
        g->fail = "a jump in a subroutine without labels";
        return;
    }
    uint32_t merge = if_begin(g, cond);
    store(g, g->jmp_to, uconst(g, dest));
    jump(g, g->switch_merge);
    place(g, merge);
}
static bool trans_JMPC(Decode* c, arg_JMPC* x) {
    if (emits(c, STEP_JUMP, x->dest, x->num)) jump_to(c->g, condition(c->g, x), x->dest);
    return true;
}
static bool trans_JMPU(Decode* c, arg_JMPU* x) {
    if (emits(c, STEP_JUMP, x->dest, x->num)) jump_to(c->g, uniform_bool(c->g, x->id, x->num & 1), x->dest);
    return true;
}

static bool trans_CALL(Decode* c, arg_CALL* x) {
    if (emits(c, STEP_CALL, x->dest, x->num)) call_sub(c->g, sub(c->g, x->dest, x->dest + x->num));
    return true;
}
static void call_if(Gen* g, uint32_t cond, uint32_t dest, uint32_t num) {
    uint32_t merge = if_begin(g, cond);
    call_sub(g, sub(g, dest, dest + num));
    place(g, merge);
}
static bool trans_CALLC(Decode* c, arg_CALLC* x) {
    if (emits(c, STEP_CALL_IF, x->dest, x->num)) call_if(c->g, condition(c->g, x), x->dest, x->num);
    return true;
}
static bool trans_CALLU(Decode* c, arg_CALLU* x) {
    if (emits(c, STEP_CALL_IF, x->dest, x->num)) call_if(c->g, uniform_bool(c->g, x->id, false), x->dest, x->num);
    return true;
}

/* the body (up to dest) when cond holds, else the else part (dest to dest + num) */
static void if_else(Decode* c, uint32_t cond, uint32_t dest, uint32_t num) {
    Gen* g = c->g;
    uint32_t else_offset = dest, endif_offset = else_offset + num;
    uint32_t then = spv_emit_id(g->s), merge = spv_emit_id(g->s);
    uint32_t otherwise = num ? spv_emit_id(g->s) : merge;
    SPV_VOID(g->s, SPV_OP_SELECTION_MERGE, merge, SPV_SELECTION_CONTROL_NONE);
    SPV_VOID(g->s, SPV_OP_BRANCH_CONDITIONAL, cond, then, otherwise);
    g->open = false;
    place(g, then);
    call_sub(g, sub(g, c->offset + 1, else_offset));
    if (num) {
        jump(g, merge);
        place(g, otherwise);
        call_sub(g, sub(g, else_offset, endif_offset));
    }
    place(g, merge);
}
static bool trans_IFC(Decode* c, arg_IFC* x) {
    if (emits(c, STEP_IF, x->dest, x->num)) if_else(c, condition(c->g, x), x->dest, x->num);
    return true;
}
static bool trans_IFU(Decode* c, arg_IFU* x) {
    if (emits(c, STEP_IF, x->dest, x->num)) if_else(c, uniform_bool(c->g, x->id, false), x->dest, x->num);
    return true;
}

/* aL = i.y; for (uint n = 0u; n <= i.x; aL += i.z, ++n) */
static bool trans_LOOP(Decode* c, arg_LOOP* x) {
    if (!emits(c, STEP_LOOP, x->dest, 0)) return true;
    Gen* g = c->g;
    uint32_t i = uniform_i(g, x->id);
    store(g, g->addr[2], SPV(g->s, SPV_OP_BITCAST, g->t_int, extract(g, g->t_uint, i, 1)));
    uint32_t counter = spv_emit_local(g->s, g->t_uint);
    store(g, counter, uconst(g, 0));
    uint32_t header = spv_emit_id(g->s), body = spv_emit_id(g->s), next = spv_emit_id(g->s);
    uint32_t merge = spv_emit_id(g->s);
    place(g, header);
    uint32_t more = SPV(g->s, SPV_OP_U_LESS_THAN_EQUAL, g->t_bool, load(g, g->t_uint, counter),
                        extract(g, g->t_uint, i, 0));
    SPV_VOID(g->s, SPV_OP_LOOP_MERGE, merge, next, SPV_LOOP_CONTROL_NONE);
    SPV_VOID(g->s, SPV_OP_BRANCH_CONDITIONAL, more, body, merge);
    g->open = false;
    place(g, body);
    call_sub(g, sub(g, c->offset + 1, (uint32_t)x->dest + 1));
    place(g, next);
    uint32_t step = SPV(g->s, SPV_OP_BITCAST, g->t_int, extract(g, g->t_uint, i, 2));
    store(g, g->addr[2], SPV(g->s, SPV_OP_I_ADD, g->t_int, load(g, g->t_int, g->addr[2]), step));
    store(g, counter, SPV(g->s, SPV_OP_I_ADD, g->t_uint, load(g, g->t_uint, counter), uconst(g, 1)));
    jump(g, header);
    place(g, merge);
    return true;
}

static uint32_t compile_instr(Gen* g, uint32_t offset) {
    Decode c = {g, offset, {STEP_UNKNOWN, 0, 0}};
    if (!decode_pica(&c, g->setup->code[offset])) g->fail = "an instruction Azahar does not translate either";
    return next_offset(g, offset);
}
/* the instructions from begin up to end (to the program's end when end is before begin), compiled or, with emit
   false, only walked for where they end. Where an instruction goes on depends on its offset alone, so a walk longer
   than the program is one that would never end */
static uint32_t walk(Gen* g, uint32_t begin, uint32_t end, bool emit) {
    uint32_t pc = begin, limit = begin > end || end > PROGRAM_END ? PROGRAM_END : end;
    for (unsigned steps = 0; pc < limit && !g->fail; ++steps) {
        if (steps == PROGRAM_END) {
            g->fail = "a program whose decompilation would not end";
            break;
        }
        pc = emit ? compile_instr(g, pc) : next_offset(g, pc);
    }
    return pc;
}
/* a subroutine with JMPs: while (true) { switch (jmp_to) { case label: ... } }, its cases falling through in the
   order of their labels to the default, which returns false. The labels found after a case's range join the set as
   Azahar's std::set grows; they are found first, since OpSwitch lists its cases before them */
static void compile_labelled(Gen* g, const Sub* s) {
    Labels labels = {0};
    bool ok = label_add(&labels, s->begin);
    for (unsigned k = 0; k < s->label_count && ok; ++k) ok = label_add(&labels, s->labels[k]);
    for (unsigned k = 0; k < labels.count && ok && !g->fail; ++k) {
        uint32_t next = k + 1 < labels.count ? labels.items[k + 1] : s->end;
        uint32_t end = walk(g, labels.items[k], next, false);
        if (end > next && end != PROGRAM_END) ok = label_add(&labels, end);
    }
    uint32_t* operands = ok ? malloc((2 + 2 * (size_t)labels.count) * sizeof *operands) : NULL;
    if (!operands) {
        free(labels.items);
        if (!g->fail) g->fail = "out of memory";
        return;
    }
    SpvEmitter* sp = g->s;
    g->jmp_to = spv_emit_local(sp, g->t_uint);
    store(g, g->jmp_to, uconst(g, s->begin));
    uint32_t header = spv_emit_id(sp), dispatch = spv_emit_id(sp), next = spv_emit_id(sp);
    uint32_t loop_merge = spv_emit_id(sp), fallback = spv_emit_id(sp);
    g->switch_merge = spv_emit_id(sp);
    place(g, header);
    SPV_VOID(sp, SPV_OP_LOOP_MERGE, loop_merge, next, SPV_LOOP_CONTROL_NONE);
    jump(g, dispatch);
    place(g, dispatch);
    operands[0] = load(g, g->t_uint, g->jmp_to);
    operands[1] = fallback;
    for (unsigned k = 0; k < labels.count; ++k) {
        operands[2 + 2 * k] = labels.items[k];
        operands[3 + 2 * k] = spv_emit_id(sp);
    }
    SPV_VOID(sp, SPV_OP_SELECTION_MERGE, g->switch_merge, SPV_SELECTION_CONTROL_NONE);
    spv_emit_void_op(sp, SPV_OP_SWITCH, operands, 2 + 2 * (size_t)labels.count);
    g->open = false;
    for (unsigned k = 0; k < labels.count && !g->fail; ++k) {
        uint32_t label = labels.items[k], next_label = k + 1 < labels.count ? labels.items[k + 1] : s->end;
        place(g, operands[3 + 2 * k]);
        uint32_t end = walk(g, label, next_label, true);
        if (end > next_label && end != PROGRAM_END) {
            store(g, g->jmp_to, uconst(g, end));
            jump(g, g->switch_merge);
        }
    }
    place(g, fallback);
    ret(g, false);
    place(g, g->switch_merge);
    jump(g, next);
    place(g, next);
    jump(g, header);
    place(g, loop_merge);
    ret(g, false);
    g->jmp_to = g->switch_merge = 0;
    free(operands);
    free(labels.items);
}
/* whether the calls leave no cycle: Kahn's sort of the subs, each taken once nothing still to be taken calls it */
static bool acyclic(Gen* g) {
    unsigned n = g->flow->sub_count, *pending = calloc(n, sizeof *pending), *first = calloc(n + 1, sizeof *first);
    unsigned *callees = malloc((g->call_count + 1) * sizeof *callees), *ready = malloc(n * sizeof *ready);
    bool result = false;
    if (!pending || !first || !callees || !ready) {
        g->fail = "out of memory";
        goto done;
    }
    /* the callees of each caller, together: first[i] .. first[i + 1] */
    for (size_t k = 0; k < g->call_count; ++k) ++first[g->calls[k][0] + 1], ++pending[g->calls[k][1]];
    for (unsigned i = 0; i < n; ++i) first[i + 1] += first[i];
    for (size_t k = 0; k < g->call_count; ++k) callees[first[g->calls[k][0]]++] = g->calls[k][1];
    for (unsigned i = n; i > 0; --i) first[i] = first[i - 1];
    first[0] = 0;
    unsigned taken = 0, count = 0;
    for (unsigned i = 0; i < n; ++i)
        if (!pending[i]) ready[count++] = i;
    while (taken < count) {
        unsigned i = ready[taken++];
        for (unsigned k = first[i]; k < first[i + 1]; ++k)
            if (!--pending[callees[k]]) ready[count++] = callees[k];
    }
    result = taken == n;
done:
    free(pending), free(first), free(callees), free(ready);
    return result;
}
static void generate(Gen* g) {
    for (unsigned i = 0; i < g->flow->sub_count && !g->fail; ++i) {
        const Sub* s = &g->flow->subs[i];
        g->current = i;
        spv_emit_function(g->s, g->sub_ids[i], g->t_bool, NULL, 0, NULL);
        g->open = true;
        if (s->label_count)
            compile_labelled(g, s);
        else {
            walk(g, s->begin, s->end, true);
            if (g->open) ret(g, false); /* the range ran out (or, at an unreachable end, the value it never gives) */
        }
        spv_emit_function_end(g->s);
    }
    if (!g->fail && !acyclic(g)) g->fail = "a recursive function";
}

/* ---- the shader around the program ---- */
uint64_t snx_ctr_vs_hash(const SnxCtrShaderSetup* setup) {
    uint64_t h = SNX_FNV_BASIS;
    for (unsigned i = 0; i < PROGRAM_END; ++i) h = snx_fnv_step(h, setup->code[i]);
    for (unsigned i = 0; i < PROGRAM_END; ++i) h = snx_fnv_step(h, setup->swizzle[i]);
    return h;
}
void snx_ctr_vs_config(const SnxCtrPica* pica, SnxCtrVsConfig* c) {
    const uint32_t* regs = pica->regs;
    c->main_offset = regs[SNX_PICA_VS + SNX_PICA_SH_MAIN_OFFSET] & 0xffff;
    c->num_outputs = 0;
    uint32_t mask = regs[SNX_PICA_VS + SNX_PICA_SH_OUTPUT_MASK] & 0xffff;
    for (unsigned reg = 0; reg < 16; ++reg) c->output_map[reg] = mask >> reg & 1 ? (uint8_t)c->num_outputs++ : 16;
    c->vs_output_total = regs[0x4f] & 7;
    for (unsigned i = 0; i < 7; ++i) c->vs_output_attributes[i] = i < c->vs_output_total ? regs[0x50 + i] : 0;
}
void snx_ctr_vs_uniforms(const SnxCtrPica* pica, SnxCtrVsPica* u, SnxCtrVsData* d) {
    memset(u, 0, sizeof *u);
    memset(d, 0, sizeof *d);
    u->b = pica->vs.b & 0xffff;
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned k = 0; k < 4; ++k) u->i[i][k] = pica->vs.i[i][k];
    memcpy(u->f, pica->vs.f, sizeof u->f);
    d->enable_clip1 = pica->regs[0x47] & 1;
    for (unsigned k = 0; k < 4; ++k) d->clip_coef[k] = snx_ctr_f24(pica->regs[0x48 + k] & 0xffffff);
}

/* the rasterizer's semantic slot -> output attribute and component (PicaGSConfigState::GetSemanticMaps), 1.0 for a
   slot no output gives; outputs holds the output attributes' values */
static uint32_t semantic(Gen* g, const uint32_t* outputs, unsigned slot) {
    const SnxCtrVsConfig* c = g->config;
    unsigned found_attr = 16, found_comp = 0;
    for (unsigned attr = 0; attr < c->vs_output_total; ++attr)
        for (unsigned comp = 0; comp < 4; ++comp)
            if (snx_bits(c->vs_output_attributes[attr], comp * 8, 5) == slot) found_attr = attr, found_comp = comp;
    if (found_attr < c->num_outputs) return extract(g, g->t_float, outputs[found_attr], found_comp);
    return fconst(g, 1.0f);
}
static uint32_t output_var(Gen* g, uint32_t type, uint32_t location) {
    return spv_emit_location(g->s, SPV_STORAGE_OUTPUT, type, location);
}
/* main: the input registers read from their attributes, the registers' first values, the program run, and EmitVtx */
static uint32_t generate_main(Gen* g, uint32_t main_sub) {
    SpvEmitter* s = g->s;
    const SnxCtrVsConfig* c = g->config;
    uint32_t f = g->t_float, v4 = g->t_vec4;
    uint32_t entry = spv_emit_function(s, 0, spv_emit_type_void(s), NULL, 0, NULL);
    g->open = true;
    for (unsigned i = 0; i < 16; ++i) {
        if (!g->in_reg[i]) continue;
        unsigned type = c->input_type[i] ? c->input_type[i] : SNX_CTR_VS_FLOAT;
        uint32_t integer = spv_emit_type_int(s, 32, type == SNX_CTR_VS_SINT);
        uint32_t typed = type == SNX_CTR_VS_FLOAT ? v4 : spv_emit_type_vector(s, integer, 4);
        uint32_t v = load(g, typed, spv_emit_location(s, SPV_STORAGE_INPUT, typed, i));
        if (type != SNX_CTR_VS_FLOAT)
            v = SPV(s, type == SNX_CTR_VS_SINT ? SPV_OP_CONVERT_S_TO_F : SPV_OP_CONVERT_U_TO_F, v4, v);
        /* the components the attribute lacks read as 0, 0, 0, 1, whatever the bound format held there */
        unsigned size = c->input_size[i];
        if (size && size < 4)
            v = SPV(s, SPV_OP_VECTOR_SHUFFLE, v4, v, vec4_const(g, 0, 0, 0, 1), 0, size > 1 ? 1 : 5, size > 2 ? 2 : 6,
                    7);
        store(g, g->in_reg[i], v);
    }
    uint32_t initial = vec4_const(g, 0, 0, 0, 1);
    for (unsigned i = 0; i < 16; ++i)
        if (g->tmp[i]) store(g, g->tmp[i], initial);
    for (unsigned i = 0; i < c->num_outputs; ++i) store(g, g->out[i], initial);
    for (unsigned i = 0; i < 3; ++i) store(g, g->addr[i], sconst(g, 0));
    for (unsigned i = 0; i < 2; ++i) store(g, g->cc[i], spv_emit_const_bool(s, false));
    if (!c->passthrough) SPV(s, SPV_OP_FUNCTION_CALL, g->t_bool, main_sub);

    uint32_t outputs[16], v[24];
    for (unsigned i = 0; i < c->num_outputs; ++i) outputs[i] = load(g, v4, g->out[i]);
    /* RasterizerRegs::VSOutputAttributes: position 0-3, quaternion 4-7, color 8-11, texcoord0 12-13, texcoord1
       14-15, texcoord0_w 16, view 18-20, texcoord2 22-23 */
    uint32_t made[6] = {0};
    if (c->passthrough)
        for (unsigned i = 0; i < 6; ++i) made[i] = load(g, v4, spv_emit_location(s, SPV_STORAGE_INPUT, v4, i));
    for (unsigned slot = 0; slot < 24; ++slot)
        v[slot] = c->passthrough ? extract(g, f, made[slot / 4], slot % 4) : semantic(g, outputs, slot);
    /* SanitizeVertex: z flushed to 0 when z / w is in (0, 1e-6), to -w when it is in (-1.00001, -1) */
    uint32_t x = v[0], y = v[1], z = v[2], w = v[3], t_bool = g->t_bool;
    uint32_t ndc_z = SPV(s, SPV_OP_F_DIV, f, z, w);
    uint32_t above = SPV(s, SPV_OP_F_ORD_GREATER_THAN, t_bool, ndc_z, fconst(g, 0.f));
    uint32_t below = SPV(s, SPV_OP_F_ORD_LESS_THAN, t_bool, ndc_z, fconst(g, 0.000001f));
    uint32_t near0 = SPV(s, SPV_OP_LOGICAL_AND, t_bool, above, below);
    z = SPV(s, SPV_OP_SELECT, f, near0, fconst(g, 0.f), z);
    uint32_t near1 = SPV(s, SPV_OP_LOGICAL_AND, t_bool, SPV(s, SPV_OP_F_ORD_LESS_THAN, t_bool, ndc_z, fconst(g, -1.f)),
                         SPV(s, SPV_OP_F_ORD_GREATER_THAN, t_bool, ndc_z, fconst(g, -1.00001f)));
    z = SPV(s, SPV_OP_SELECT, f, near1, SPV(s, SPV_OP_F_NEGATE, f, w), z);
    /* if (flip_viewport) vtx_pos.y = -vtx_pos.y */
    uint32_t data_uint = spv_emit_type_pointer(s, SPV_STORAGE_UNIFORM, g->t_uint);
    uint32_t flip = load(g, g->t_uint, SPV(s, SPV_OP_ACCESS_CHAIN, data_uint, g->data, sconst(g, 1)));
    y = SPV(s, SPV_OP_SELECT, f, SPV(s, SPV_OP_I_NOT_EQUAL, t_bool, flip, uconst(g, 0)), SPV(s, SPV_OP_F_NEGATE, f, y),
            y);
    uint32_t position = spv_emit_builtin(s, SPV_STORAGE_OUTPUT, v4, SPV_BUILT_IN_POSITION);
    store(g, position, SPV(s, SPV_OP_COMPOSITE_CONSTRUCT, v4, x, y, SPV(s, SPV_OP_F_NEGATE, f, z), w));
    if (c->clip_planes) {
        spv_emit_capability(s, SPV_CAPABILITY_CLIP_DISTANCE);
        uint32_t clip_type = spv_emit_type_array(s, f, 2);
        uint32_t clip = spv_emit_builtin(s, SPV_STORAGE_OUTPUT, clip_type, SPV_BUILT_IN_CLIP_DISTANCE);
        uint32_t element = spv_emit_type_pointer(s, SPV_STORAGE_OUTPUT, f);
        /* the fixed PICA clipping plane z <= 0, and enable_clip1 ? dot(clip_coef, vtx_pos) : 0.0 */
        store(g, SPV(s, SPV_OP_ACCESS_CHAIN, element, clip, sconst(g, 0)), SPV(s, SPV_OP_F_NEGATE, f, z));
        uint32_t data_vec4 = spv_emit_type_pointer(s, SPV_STORAGE_UNIFORM, v4);
        uint32_t coef = load(g, v4, SPV(s, SPV_OP_ACCESS_CHAIN, data_vec4, g->data, sconst(g, 2)));
        uint32_t enable = load(g, g->t_uint, SPV(s, SPV_OP_ACCESS_CHAIN, data_uint, g->data, sconst(g, 0)));
        uint32_t distance = SPV(s, SPV_OP_DOT, f, coef, SPV(s, SPV_OP_COMPOSITE_CONSTRUCT, v4, x, y, z, w));
        store(g, SPV(s, SPV_OP_ACCESS_CHAIN, element, clip, sconst(g, 1)),
              SPV(s, SPV_OP_SELECT, f, SPV(s, SPV_OP_I_NOT_EQUAL, t_bool, enable, uconst(g, 0)), distance,
                  fconst(g, 0.f)));
    }
    uint32_t v2 = g->t_vec2, v3 = g->t_vec3;
    uint32_t color = SPV_EXT(s, v4, SPV_GLSL_F_ABS, SPV(s, SPV_OP_COMPOSITE_CONSTRUCT, v4, v[8], v[9], v[10], v[11]));
    store(g, output_var(g, v4, 0), SPV_EXT(s, v4, SPV_GLSL_F_MIN, color, vec4_const(g, 1, 1, 1, 1)));
    store(g, output_var(g, v2, 1), SPV(s, SPV_OP_COMPOSITE_CONSTRUCT, v2, v[12], v[13]));
    store(g, output_var(g, v2, 2), SPV(s, SPV_OP_COMPOSITE_CONSTRUCT, v2, v[14], v[15]));
    store(g, output_var(g, v2, 3), SPV(s, SPV_OP_COMPOSITE_CONSTRUCT, v2, v[22], v[23]));
    store(g, output_var(g, f, 4), v[16]);
    store(g, output_var(g, v4, 5), SPV(s, SPV_OP_COMPOSITE_CONSTRUCT, v4, v[4], v[5], v[6], v[7]));
    store(g, output_var(g, v3, 6), SPV(s, SPV_OP_COMPOSITE_CONSTRUCT, v3, v[18], v[19], v[20]));
    spv_emit_void_op(s, SPV_OP_RETURN, NULL, 0);
    spv_emit_function_end(s);
    spv_emit_entry_str(s, SPV_STAGE_VERTEX, entry, "main", s->interface, s->interface_count);
    return entry;
}
/* the types, the uniform blocks and the registers every program has */
static void declare(Gen* g) {
    SpvEmitter* s = g->s;
    g->t_bool = spv_emit_type_bool(s);
    g->t_bool4 = spv_emit_type_vector(s, g->t_bool, 4);
    g->t_int = spv_emit_type_int(s, 32, true);
    g->t_uint = spv_emit_type_int(s, 32, false);
    g->t_float = spv_emit_type_float(s, 32);
    g->t_vec2 = spv_emit_type_vector(s, g->t_float, 2);
    g->t_vec3 = spv_emit_type_vector(s, g->t_float, 3);
    g->t_vec4 = spv_emit_type_vector(s, g->t_float, 4);
    g->t_uvec4 = spv_emit_type_vector(s, g->t_uint, 4);
    uint32_t stride = 16;
    uint32_t i_array = spv_emit_type_array(s, g->t_uvec4, 4), f_array = spv_emit_type_array(s, g->t_vec4, 96);
    spv_emit_decorate(s, i_array, SPV_DECORATION_ARRAY_STRIDE, &stride, 1);
    spv_emit_decorate(s, f_array, SPV_DECORATION_ARRAY_STRIDE, &stride, 1);
    /* vs_pica_data { uint b; uvec4 i[4]; vec4 f[96]; } and vs_data { bool enable_clip1; bool flip_viewport; vec4
       clip_coef; }, std140 (a bool is a 32-bit uint there) */
    uint32_t pica_members[3] = {g->t_uint, i_array, f_array}, pica_offsets[3] = {0, 16, 80};
    uint32_t pica = spv_emit_type_struct(s, pica_members, pica_offsets, 3);
    uint32_t data_members[3] = {g->t_uint, g->t_uint, g->t_vec4}, data_offsets[3] = {0, 4, 16};
    uint32_t data = spv_emit_type_struct(s, data_members, data_offsets, 3);
    spv_emit_decorate(s, pica, SPV_DECORATION_BLOCK, NULL, 0);
    spv_emit_decorate(s, data, SPV_DECORATION_BLOCK, NULL, 0);
    g->pica = spv_emit_resource(s, SPV_STORAGE_UNIFORM, pica, 0, 0);
    g->data = spv_emit_resource(s, SPV_STORAGE_UNIFORM, data, 0, 1);
    for (unsigned i = 0; i < g->config->num_outputs && i < 16; ++i) g->out[i] = private_vec4(g);
    for (unsigned i = 0; i < 3; ++i) g->addr[i] = spv_emit_variable(s, SPV_STORAGE_PRIVATE, g->t_int);
    for (unsigned i = 0; i < 2; ++i) g->cc[i] = spv_emit_variable(s, SPV_STORAGE_PRIVATE, g->t_bool);
}
bool snx_ctr_vs_spirv(const SnxCtrShaderSetup* setup, const SnxCtrVsConfig* c, bool preserve_infnan, uint32_t** words,
                      size_t* count, const char** why) {
    Flow flow = {.code = setup->code};
    if (!c->passthrough && !add_sub(&flow, c->main_offset, PROGRAM_END) && !flow.fail)
        flow.fail = "a program the analysis lost";
    if (!c->passthrough && !flow.fail && find_sub(&flow, c->main_offset, PROGRAM_END)->exit != EXIT_END)
        flow.fail = "a program that does not always end";
    if (flow.fail) {
        *why = flow.fail;
        flow_free(&flow);
        return false;
    }
    SpvEmitter s;
    spv_emit_init(&s, 0);
    s.version = 0x00010300; /* SPIR-V 1.3 */
    spv_emit_glsl450_import(&s);
    spv_emit_shader_capability(&s);
    Gen g = {.setup = setup, .config = c, .flow = &flow, .s = &s};
    g.sub_ids = flow.sub_count ? malloc(flow.sub_count * sizeof *g.sub_ids) : NULL;
    if (!g.sub_ids && flow.sub_count) g.fail = "out of memory";
    for (unsigned i = 0; i < flow.sub_count && g.sub_ids; ++i) g.sub_ids[i] = spv_emit_id(&s);
    if (!g.fail) {
        declare(&g);
        if (!c->passthrough) generate(&g);
    }
    if (!g.fail) {
        uint32_t entry =
            generate_main(&g, c->passthrough ? 0 : g.sub_ids[find_sub(&flow, c->main_offset, PROGRAM_END) - flow.subs]);
        if (preserve_infnan) {
            /* PICA programs rely on infinities and NaNs, which SPIR-V otherwise lets the driver assume away */
            spv_emit_extension(&s, "SPV_KHR_float_controls");
            spv_emit_capability(&s, SPV_CAPABILITY_SIGNED_ZERO_INF_NAN_PRESERVE);
            spv_emit_execution_mode(&s, entry, SPV_MODE_SIGNED_ZERO_INF_NAN_PRESERVE, 1, 32);
        }
    }
    free(g.sub_ids);
    free(g.calls);
    flow_free(&flow);
    uint32_t* module = g.fail ? NULL : spv_emit_finish(&s, count);
    spv_emit_destroy(&s);
    if (!module) {
        *why = g.fail;
        return false;
    }
    *words = module;
    return true;
}
