/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Azahar video_core/shader/generator/glsl_shader_decompiler.cpp (the control flow analysis, and what
   each instruction computes, written here as SPIR-V instead of GLSL), glsl_shader_gen.cpp (GenerateVertexShader
   without a geometry shader) and shader_gen.cpp (PicaVSConfigState, the semantic maps) (GPL-2.0-or-later, Copyright
   2017-2026 Citra Emulator Project / Azahar Emulator Project) at azahar-emu/azahar 49a1e8c, in C. The instruction
   fields are nihstro's shader_bytecode.h as gpu/shader.c reads them (BSD-3-Clause, Copyright 2014 Tony Wasserka; its
   notice is in shader.c). Multiplication is the host's (Azahar's sanitize_mul off), and RCP and RSQ skip a zero or
   negative operand as Azahar's do then. */
#include "3ds/gpu/vk/vsgen.h"
#include "3ds/gpu/pica_internal.h"
#include "common/utilities/bits.h"
#include "common/utilities/spirv_emit.h"
#include <stdlib.h>
#include <string.h>

#define PROGRAM_END 4096u

enum {
    OP_ADD = 0x00, OP_DP3 = 0x01, OP_DP4 = 0x02, OP_DPH = 0x03, OP_EX2 = 0x05, OP_LG2 = 0x06, OP_MUL = 0x08,
    OP_SGE = 0x09, OP_SLT = 0x0a, OP_FLR = 0x0b, OP_MAX = 0x0c, OP_MIN = 0x0d, OP_RCP = 0x0e, OP_RSQ = 0x0f,
    OP_MOVA = 0x12, OP_MOV = 0x13, OP_DPHI = 0x18, OP_SGEI = 0x1a, OP_SLTI = 0x1b, OP_NOP = 0x21, OP_END = 0x22,
    OP_CALL = 0x24, OP_CALLC = 0x25, OP_CALLU = 0x26, OP_IFU = 0x27, OP_IFC = 0x28, OP_LOOP = 0x29, OP_EMIT = 0x2a,
    OP_SETEMIT = 0x2b, OP_JMPC = 0x2c, OP_JMPU = 0x2d, OP_CMP = 0x2e, OP_MADI = 0x30, OP_MAD = 0x38
};
static unsigned opcode(uint32_t instr) { return instr >> 26; }
static unsigned effective(unsigned op) {
    if ((op & ~7u) == OP_MAD) return OP_MAD;
    if ((op & ~7u) == OP_MADI) return OP_MADI;
    if ((op & ~1u) == OP_CMP) return OP_CMP;
    return op;
}
static bool arithmetic(unsigned op) { return op <= 0x0f || op == OP_MOVA || op == OP_MOV || (op & ~1u) == OP_CMP; }
static bool inverted_arithmetic(unsigned op) { return op >= 0x18 && op <= 0x1b; }
/* the flow control fields */
static uint32_t dest_offset(uint32_t i) { return snx_bits(i, 10, 12); }
static uint32_t num_instructions(uint32_t i) { return snx_bits(i, 0, 8); }

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
        uint32_t instr = f->code[offset];
        unsigned op = opcode(instr);
        if (op == OP_END) {
            result = EXIT_END;
            goto done;
        }
        if (op == OP_JMPC || op == OP_JMPU) {
            if (!label_add(labels, dest_offset(instr))) f->fail = "out of memory";
            Exit no_jump = scan(f, offset + 1, end, labels);
            Exit jump = scan(f, dest_offset(instr), end, labels);
            result = parallel(no_jump, jump);
            goto done;
        }
        if (op == OP_CALL) {
            const Sub* call = add_sub(f, dest_offset(instr), dest_offset(instr) + num_instructions(instr));
            if (!call) return EXIT_UNDETERMINED;
            if (call->exit == EXIT_END) {
                result = EXIT_END;
                goto done;
            }
            Exit exit = call->exit;
            result = series(exit, scan(f, offset + 1, end, labels));
            goto done;
        }
        if (op == OP_LOOP) {
            const Sub* loop = add_sub(f, offset + 1, dest_offset(instr) + 1);
            if (!loop) return EXIT_UNDETERMINED;
            if (loop->exit == EXIT_END) {
                result = EXIT_END;
                goto done;
            }
            Exit exit = loop->exit;
            result = series(exit, scan(f, dest_offset(instr) + 1, end, labels));
            goto done;
        }
        if (op == OP_CALLC || op == OP_CALLU) {
            const Sub* call = add_sub(f, dest_offset(instr), dest_offset(instr) + num_instructions(instr));
            if (!call) return EXIT_UNDETERMINED;
            Exit exit = call->exit;
            result = series(parallel(exit, EXIT_RETURN), scan(f, offset + 1, end, labels));
            goto done;
        }
        if (op == OP_IFU || op == OP_IFC) {
            const Sub* if_sub = add_sub(f, offset + 1, dest_offset(instr));
            if (!if_sub) return EXIT_UNDETERMINED;
            Exit if_exit = if_sub->exit, else_exit = EXIT_RETURN;
            if (num_instructions(instr)) {
                const Sub* else_sub = add_sub(f, dest_offset(instr), dest_offset(instr) + num_instructions(instr));
                if (!else_sub) return EXIT_UNDETERMINED;
                else_exit = else_sub->exit;
            }
            Exit both = parallel(if_exit, else_exit);
            if (both == EXIT_END) {
                result = EXIT_END;
                goto done;
            }
            result = series(both, scan(f, dest_offset(instr) + num_instructions(instr), end, labels));
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
typedef struct {
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
} Gen;

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
    SPV_VOID(g->s, SPV_OP_SELECTION_MERGE, merge, 0);
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
static uint32_t condition(Gen* g, uint32_t instr) {
    unsigned op = snx_bits(instr, 22, 2);
    if (op == 2) return cc_bit(g, 0, snx_bits(instr, 25, 1));
    if (op == 3) return cc_bit(g, 1, snx_bits(instr, 24, 1));
    uint32_t x = cc_bit(g, 0, snx_bits(instr, 25, 1)), y = cc_bit(g, 1, snx_bits(instr, 24, 1));
    return SPV(g->s, op == 0 ? SPV_OP_LOGICAL_OR : SPV_OP_LOGICAL_AND, g->t_bool, x, y); /* any, all */
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
    uint32_t instr = g->setup->code[offset];
    unsigned raw = opcode(instr), op = effective(raw);
    if (arithmetic(raw) || inverted_arithmetic(raw) || op == OP_MAD || op == OP_MADI) return offset + 1;
    switch (raw) {
        case OP_END: return PROGRAM_END;
        case OP_CALL: {
            const Sub* s = find_sub(g->flow, dest_offset(instr), dest_offset(instr) + num_instructions(instr));
            return s && s->exit == EXIT_END ? PROGRAM_END : offset + 1;
        }
        case OP_JMPC:
        case OP_JMPU:
        case OP_CALLC:
        case OP_CALLU:
        case OP_NOP:
        case OP_EMIT:
        case OP_SETEMIT: return offset + 1;
        case OP_IFC:
        case OP_IFU: {
            uint32_t else_offset = dest_offset(instr), endif_offset = else_offset + num_instructions(instr);
            if (!num_instructions(instr)) return else_offset;
            const Sub* if_sub = find_sub(g->flow, offset + 1, else_offset);
            const Sub* else_sub = find_sub(g->flow, else_offset, endif_offset);
            return if_sub && else_sub && if_sub->exit == EXIT_END && else_sub->exit == EXIT_END ? PROGRAM_END
                                                                                                : endif_offset;
        }
        case OP_LOOP: {
            const Sub* s = find_sub(g->flow, offset + 1, dest_offset(instr) + 1);
            return s && s->exit == EXIT_END ? PROGRAM_END : dest_offset(instr) + 1;
        }
        default: return PROGRAM_END;
    }
}
static void compile_arithmetic(Gen* g, uint32_t instr, unsigned raw) {
    unsigned op = effective(raw);
    bool inverted = inverted_arithmetic(raw);
    uint32_t pattern = g->setup->swizzle[snx_bits(instr, 0, 7)];
    unsigned address = snx_bits(instr, 19, 2);
    unsigned reg1 = inverted ? snx_bits(instr, 14, 5) : snx_bits(instr, 12, 7);
    unsigned reg2 = inverted ? snx_bits(instr, 7, 7) : snx_bits(instr, 7, 5);
    uint32_t a = source(g, reg1, inverted ? 0 : address, pattern, 1);
    uint32_t b = source(g, reg2, inverted ? address : 0, pattern, 2);
    uint32_t d = destination(g, snx_bits(instr, 21, 5));
    uint32_t f = g->t_float, v4 = g->t_vec4;
    SpvEmitter* s = g->s;
    switch (op) {
        case OP_ADD: set_dest(g, pattern, d, SPV(s, SPV_OP_F_ADD, v4, a, b), false); break;
        case OP_MUL: set_dest(g, pattern, d, SPV(s, SPV_OP_F_MUL, v4, a, b), false); break;
        case OP_FLR: set_dest(g, pattern, d, SPV_EXT(s, v4, SPV_GLSL_FLOOR, a), false); break;
        case OP_MAX: set_dest(g, pattern, d, SPV_EXT(s, v4, SPV_GLSL_F_MAX, a, b), false); break;
        case OP_MIN: set_dest(g, pattern, d, SPV_EXT(s, v4, SPV_GLSL_F_MIN, a, b), false); break;
        case OP_DP3: {
            uint32_t a3 = SPV(s, SPV_OP_VECTOR_SHUFFLE, g->t_vec3, a, a, 0, 1, 2);
            uint32_t b3 = SPV(s, SPV_OP_VECTOR_SHUFFLE, g->t_vec3, b, b, 0, 1, 2);
            set_dest(g, pattern, d, SPV(s, SPV_OP_DOT, f, a3, b3), true);
            break;
        }
        case OP_DP4:
        case OP_DPH:
        case OP_DPHI:
            /* Azahar without sanitize_mul dots the sources as they are; DPH's w of 1.0 is the hardware's */
            if (op != OP_DP4) a = SPV(s, SPV_OP_VECTOR_SHUFFLE, v4, a, vec4_const(g, 0, 0, 0, 1), 0, 1, 2, 7);
            set_dest(g, pattern, d, SPV(s, SPV_OP_DOT, f, a, b), true);
            break;
        case OP_RCP:
        case OP_RSQ: {
            /* Azahar without sanitize_mul: a zero operand (RSQ: one not above zero) leaves the destination alone,
               which avoids NaN; a NaN one passes RCP's != as it did in GLSL */
            uint32_t x = extract(g, f, a, 0);
            uint32_t ok = op == OP_RCP ? SPV(s, SPV_OP_F_UNORD_NOT_EQUAL, g->t_bool, x, fconst(g, 0))
                                       : SPV(s, SPV_OP_F_ORD_GREATER_THAN, g->t_bool, x, fconst(g, 0));
            uint32_t merge = if_begin(g, ok);
            uint32_t value = op == OP_RCP ? SPV(s, SPV_OP_F_DIV, f, fconst(g, 1), x)
                                          : SPV_EXT(s, f, SPV_GLSL_INVERSE_SQRT, x);
            set_dest(g, pattern, d, value, true);
            place(g, merge);
            break;
        }
        case OP_MOVA:
            /* address_registers.xy = ivec2(a), each enabled one of x and y */
            for (unsigned i = 0; i < 2; ++i)
                if (pattern & (8u >> i))
                    store(g, g->addr[i], SPV(s, SPV_OP_CONVERT_F_TO_S, g->t_int, extract(g, f, a, i)));
            break;
        case OP_MOV: set_dest(g, pattern, d, a, false); break;
        case OP_SGE:
        case OP_SGEI:
        case OP_SLT:
        case OP_SLTI: {
            uint32_t compare = op == OP_SGE || op == OP_SGEI ? SPV_OP_F_ORD_GREATER_THAN_EQUAL : SPV_OP_F_ORD_LESS_THAN;
            uint32_t mask = SPV(s, compare, g->t_bool4, a, b);
            uint32_t ones = vec4_const(g, 1, 1, 1, 1), zeros = vec4_const(g, 0, 0, 0, 0);
            set_dest(g, pattern, d, SPV(s, SPV_OP_SELECT, v4, mask, ones, zeros), false);
            break;
        }
        case OP_CMP: {
            /* ==, !=, <, <=, >, >= as GLSL compiles them (!= unordered) */
            static const uint32_t compare[6] = {SPV_OP_F_ORD_EQUAL,     SPV_OP_F_UNORD_NOT_EQUAL,
                                                SPV_OP_F_ORD_LESS_THAN, SPV_OP_F_ORD_LESS_THAN_EQUAL,
                                                SPV_OP_F_ORD_GREATER_THAN, SPV_OP_F_ORD_GREATER_THAN_EQUAL};
            unsigned mode[2] = {snx_bits(instr, 24, 3), snx_bits(instr, 21, 3)};
            if (mode[0] > 5 || mode[1] > 5) break; /* an unknown compare mode: Azahar logs it and leaves the codes */
            for (unsigned i = 0; i < 2; ++i)
                store(g, g->cc[i], SPV(s, compare[mode[i]], g->t_bool, extract(g, f, a, i), extract(g, f, b, i)));
            break;
        }
        case OP_EX2: set_dest(g, pattern, d, SPV_EXT(s, f, SPV_GLSL_EXP2, extract(g, f, a, 0)), true); break;
        case OP_LG2: set_dest(g, pattern, d, SPV_EXT(s, f, SPV_GLSL_LOG2, extract(g, f, a, 0)), true); break;
        default: g->fail = "an arithmetic instruction Azahar does not translate either"; break;
    }
}
static uint32_t compile_instr(Gen* g, uint32_t offset) {
    uint32_t instr = g->setup->code[offset];
    unsigned raw = opcode(instr), op = effective(raw);
    if (arithmetic(raw) || inverted_arithmetic(raw)) {
        compile_arithmetic(g, instr, raw);
        return offset + 1;
    }
    if (op == OP_MAD || op == OP_MADI) {
        bool inverted = op == OP_MADI;
        uint32_t pattern = g->setup->swizzle[snx_bits(instr, 0, 5)];
        unsigned address = snx_bits(instr, 22, 2);
        unsigned reg2 = inverted ? snx_bits(instr, 12, 5) : snx_bits(instr, 10, 7);
        unsigned reg3 = inverted ? snx_bits(instr, 5, 7) : snx_bits(instr, 5, 5);
        uint32_t a = source(g, snx_bits(instr, 17, 5), 0, pattern, 1);
        uint32_t b = source(g, reg2, inverted ? 0 : address, pattern, 2);
        uint32_t c = source(g, reg3, inverted ? address : 0, pattern, 3);
        uint32_t product = SPV(g->s, SPV_OP_F_MUL, g->t_vec4, a, b);
        set_dest(g, pattern, destination(g, snx_bits(instr, 24, 5)), SPV(g->s, SPV_OP_F_ADD, g->t_vec4, product, c),
                 false);
        return offset + 1;
    }
    switch (raw) {
        case OP_END: ret(g, true); break;
        case OP_JMPC:
        case OP_JMPU: {
            uint32_t cond = raw == OP_JMPC ? condition(g, instr)
                                           : uniform_bool(g, snx_bits(instr, 22, 4), num_instructions(instr) & 1);
            if (!g->jmp_to) {
                /* a range the analysis took from its cache without the labels: the GLSL had no loop to break */
                g->fail = "a jump in a subroutine without labels";
                break;
            }
            uint32_t merge = if_begin(g, cond);
            store(g, g->jmp_to, uconst(g, dest_offset(instr)));
            jump(g, g->switch_merge);
            place(g, merge);
            break;
        }
        case OP_CALL:
        case OP_CALLC:
        case OP_CALLU: {
            uint32_t merge = 0;
            if (raw == OP_CALLC)
                merge = if_begin(g, condition(g, instr));
            else if (raw == OP_CALLU)
                merge = if_begin(g, uniform_bool(g, snx_bits(instr, 22, 4), false));
            call_sub(g, sub(g, dest_offset(instr), dest_offset(instr) + num_instructions(instr)));
            if (merge) place(g, merge);
            break;
        }
        case OP_NOP: break;
        case OP_IFC:
        case OP_IFU: {
            uint32_t cond = raw == OP_IFC ? condition(g, instr) : uniform_bool(g, snx_bits(instr, 22, 4), false);
            uint32_t else_offset = dest_offset(instr), endif_offset = else_offset + num_instructions(instr);
            uint32_t then = spv_emit_id(g->s), merge = spv_emit_id(g->s);
            uint32_t otherwise = num_instructions(instr) ? spv_emit_id(g->s) : merge;
            SPV_VOID(g->s, SPV_OP_SELECTION_MERGE, merge, 0);
            SPV_VOID(g->s, SPV_OP_BRANCH_CONDITIONAL, cond, then, otherwise);
            g->open = false;
            place(g, then);
            call_sub(g, sub(g, offset + 1, else_offset));
            if (num_instructions(instr)) {
                jump(g, merge);
                place(g, otherwise);
                call_sub(g, sub(g, else_offset, endif_offset));
            }
            place(g, merge);
            break;
        }
        case OP_LOOP: {
            /* aL = i.y; for (uint n = 0u; n <= i.x; aL += i.z, ++n) */
            uint32_t i = uniform_i(g, snx_bits(instr, 22, 2));
            store(g, g->addr[2], SPV(g->s, SPV_OP_BITCAST, g->t_int, extract(g, g->t_uint, i, 1)));
            uint32_t counter = spv_emit_local(g->s, g->t_uint);
            store(g, counter, uconst(g, 0));
            uint32_t header = spv_emit_id(g->s), body = spv_emit_id(g->s), next = spv_emit_id(g->s);
            uint32_t merge = spv_emit_id(g->s);
            place(g, header);
            uint32_t more = SPV(g->s, SPV_OP_U_LESS_THAN_EQUAL, g->t_bool, load(g, g->t_uint, counter),
                                extract(g, g->t_uint, i, 0));
            SPV_VOID(g->s, SPV_OP_LOOP_MERGE, merge, next, 0);
            SPV_VOID(g->s, SPV_OP_BRANCH_CONDITIONAL, more, body, merge);
            g->open = false;
            place(g, body);
            call_sub(g, sub(g, offset + 1, dest_offset(instr) + 1));
            place(g, next);
            uint32_t step = SPV(g->s, SPV_OP_BITCAST, g->t_int, extract(g, g->t_uint, i, 2));
            store(g, g->addr[2], SPV(g->s, SPV_OP_I_ADD, g->t_int, load(g, g->t_int, g->addr[2]), step));
            store(g, counter, SPV(g->s, SPV_OP_I_ADD, g->t_uint, load(g, g->t_uint, counter), uconst(g, 1)));
            jump(g, header);
            place(g, merge);
            break;
        }
        case OP_EMIT:
        case OP_SETEMIT: break; /* a geometry shader's: nothing in a vertex shader */
        default: g->fail = "an instruction Azahar does not translate either"; break;
    }
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
    SPV_VOID(sp, SPV_OP_LOOP_MERGE, loop_merge, next, 0);
    jump(g, dispatch);
    place(g, dispatch);
    operands[0] = load(g, g->t_uint, g->jmp_to);
    operands[1] = fallback;
    for (unsigned k = 0; k < labels.count; ++k) {
        operands[2 + 2 * k] = labels.items[k];
        operands[3 + 2 * k] = spv_emit_id(sp);
    }
    SPV_VOID(sp, SPV_OP_SELECTION_MERGE, g->switch_merge, 0);
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
/* SanitizeVertex and the outputs from the 24 semantic slots' values, the function ended and the entry point declared */
static uint32_t emit_outputs(Gen* g, uint32_t entry, const uint32_t* v) {
    SpvEmitter* s = g->s;
    const SnxCtrVsConfig* c = g->config;
    uint32_t f = g->t_float, v4 = g->t_vec4;
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
/* The program of a draw whose vertices the host formed (a geometry shader's): main reads the 24 semantic slots from
   six vec4 inputs, locations 0 to 5 */
static uint32_t generate_passthrough(Gen* g) {
    SpvEmitter* s = g->s;
    uint32_t entry = spv_emit_function(s, 0, spv_emit_type_void(s), NULL, 0, NULL);
    g->open = true;
    uint32_t v[24];
    for (unsigned i = 0; i < 6; ++i) {
        uint32_t in = load(g, g->t_vec4, spv_emit_location(s, SPV_STORAGE_INPUT, g->t_vec4, i));
        for (unsigned c = 0; c < 4; ++c) v[i * 4 + c] = extract(g, g->t_float, in, c);
    }
    return emit_outputs(g, entry, v);
}
/* main: the input registers read from their attributes, the registers' first values, the program run, and EmitVtx */
static uint32_t generate_main(Gen* g, uint32_t main_sub) {
    SpvEmitter* s = g->s;
    const SnxCtrVsConfig* c = g->config;
    uint32_t v4 = g->t_vec4;
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
    SPV(s, SPV_OP_FUNCTION_CALL, g->t_bool, main_sub);

    uint32_t outputs[16], v[24];
    for (unsigned i = 0; i < c->num_outputs; ++i) outputs[i] = load(g, v4, g->out[i]);
    /* RasterizerRegs::VSOutputAttributes: position 0-3, quaternion 4-7, color 8-11, texcoord0 12-13, texcoord1
       14-15, texcoord0_w 16, view 18-20, texcoord2 22-23 */
    for (unsigned slot = 0; slot < 24; ++slot) v[slot] = semantic(g, outputs, slot);
    return emit_outputs(g, entry, v);
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
static bool passthrough_spirv(const SnxCtrVsConfig* c, bool preserve_infnan, uint32_t** words, size_t* count,
                              const char** why) {
    SpvEmitter s;
    spv_emit_init(&s, 0);
    s.version = 0x00010300; /* SPIR-V 1.3 */
    spv_emit_glsl450_import(&s);
    spv_emit_shader_capability(&s);
    Gen g = {.config = c, .s = &s};
    declare(&g);
    uint32_t entry = generate_passthrough(&g);
    if (preserve_infnan) {
        spv_emit_extension(&s, "SPV_KHR_float_controls");
        spv_emit_capability(&s, SPV_CAPABILITY_SIGNED_ZERO_INF_NAN_PRESERVE);
        spv_emit_execution_mode(&s, entry, SPV_MODE_SIGNED_ZERO_INF_NAN_PRESERVE, 1, 32);
    }
    *words = spv_emit_finish(&s, count);
    spv_emit_destroy(&s);
    if (!*words) *why = "the host is out of memory";
    return *words != NULL;
}
bool snx_ctr_vs_spirv(const SnxCtrShaderSetup* setup, const SnxCtrVsConfig* c, bool preserve_infnan, uint32_t** words,
                      size_t* count, const char** why) {
    if (c->passthrough) return passthrough_spirv(c, preserve_infnan, words, count, why);
    Flow flow = {.code = setup->code};
    if (!add_sub(&flow, c->main_offset, PROGRAM_END) && !flow.fail) flow.fail = "a program the analysis lost";
    if (!flow.fail && find_sub(&flow, c->main_offset, PROGRAM_END)->exit != EXIT_END)
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
    g.sub_ids = malloc(flow.sub_count * sizeof *g.sub_ids);
    if (!g.sub_ids) g.fail = "out of memory";
    for (unsigned i = 0; i < flow.sub_count && g.sub_ids; ++i) g.sub_ids[i] = spv_emit_id(&s);
    if (!g.fail) {
        declare(&g);
        generate(&g);
    }
    if (!g.fail) {
        uint32_t entry = generate_main(&g, g.sub_ids[find_sub(&flow, c->main_offset, PROGRAM_END) - flow.subs]);
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
