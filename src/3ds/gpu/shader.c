/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Azahar video_core/shader/shader_interpreter.cpp (RunInterpreter) and video_core/pica/shader_unit.cpp
   (GPL-2.0-or-later, Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project) at azahar-emu/azahar
   a4b335e. Instruction fields from nihstro include/nihstro/shader_bytecode.h (BSD-3-Clause):

   Copyright 2014 Tony Wasserka
   All rights reserved.
   Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
   following conditions are met: Redistributions of source code must retain the above copyright notice, this list of
   conditions and the following disclaimer. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided
   with the distribution. Neither the name of the owner nor the names of its contributors may be used to endorse or
   promote products derived from this software without specific prior written permission.
   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
   INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
   SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
   IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. */
#include "3ds/gpu/shader.h"
#include "3ds/gpu/pica_internal.h"
#include <math.h>
#include <string.h>

#define PROGRAM_WORDS 4096u

/* boost::circular_buffer: pushing onto a full stack drops its oldest entry */
typedef struct {
    uint32_t else_address, end_address;
} IfEntry;
typedef struct {
    uint32_t end_address, return_address;
} CallEntry;
typedef struct {
    uint32_t entry_address, end_address;
    uint8_t downcounter, increment, previous_al;
} LoopEntry;
#define PUSH(stack, count, capacity, entry)                                                                            \
    do {                                                                                                               \
        if ((count) == (capacity)) {                                                                                   \
            memmove(&(stack)[0], &(stack)[1], sizeof((stack)[0]) * ((capacity) - 1));                                  \
            --(count);                                                                                                 \
        }                                                                                                              \
        (stack)[(count)++] = (entry);                                                                                  \
    } while (0)

static const float ones[4] = {1, 1, 1, 1};
static float discard[4];

/* SourceRegister: 0x00-0x0f inputs, 0x10-0x1f temporaries, 0x20-0x7f float uniforms. A uniform addressed through
   a0.x, a0.y or aL wraps at 128, an offset outside a signed byte counts as 0, and past the 96 uniforms reads ones */
static const float* source(const SnxCtrShaderSetup* s, const SnxCtrShaderUnit* u, unsigned reg, unsigned address) {
    if (reg < 0x10) return u->input[reg];
    if (reg < 0x20) return u->temporary[reg - 0x10];
    unsigned index = reg - 0x20;
    if (address) {
        int32_t offset = u->address[address - 1];
        if (offset < -128 || offset > 127) offset = 0;
        index = (unsigned)((int32_t)index + offset) & 0x7f;
        if (index >= 96) return ones;
    }
    return s->f[index];
}

/* the swizzle pattern's selectors for source n (1-3) and its negation bit */
static void swizzled(const float* v, uint32_t pattern, unsigned n, float out[4]) {
    static const unsigned select_at[3] = {5, 14, 23}, negate_at[3] = {4, 13, 22};
    unsigned base = select_at[n - 1];
    bool negate = (pattern >> negate_at[n - 1]) & 1;
    for (unsigned i = 0; i < 4; ++i) {
        /* component 0 is the selector's top two bits */
        float x = v[snx_bits(pattern, base + (3 - i) * 2, 2)];
        out[i] = negate ? -x : x;
    }
}
static bool enabled(uint32_t pattern, unsigned i) { return (pattern & (0x8u >> i)) != 0; }

static float* destination(SnxCtrShaderUnit* u, unsigned reg) {
    if (reg < 0x10) return u->output[u->output_bank][reg];
    if (reg < 0x20) return u->temporary[reg - 0x10];
    return discard;
}

/* a run of the program: the unit, and the IF, CALL and LOOP scopes open. The decoder (decode-pica.inc, from
   pica.decode) calls the trans_ function of each instruction with this as its context */
typedef struct {
    const SnxCtrShaderSetup* s;
    SnxCtrShaderUnit* u;
    SnxCtrEmitter* emitter;
    uint32_t pc;
    bool stop, is_break;
    IfEntry ifs[8];
    CallEntry calls[4];
    LoopEntry loops[4];
    unsigned if_count, call_count, loop_count;
} Run;
typedef Run DisasContext;
#include "common/utilities/decodetree.h"
/* the generated extractors take a context that they leave unused, as QEMU's do */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "decode-pica.inc"
#pragma GCC diagnostic pop

static bool condition(const SnxCtrShaderUnit* u, const arg_cond* a) {
    bool x = a->refx == u->cc[0], y = a->refy == u->cc[1];
    switch (a->cond) {
        case 0: return x || y;
        case 1: return x && y;
        case 2: return x;
        default: return y;
    }
}
static bool uniform_bool(const Run* r, unsigned id) { return (r->s->b >> id) & 1; }

/* ---- the arithmetic instructions (formats 1, 1i and 1c) and MAD (formats 5 and 5i) ---- */
/* the sources of an arithmetic instruction, swizzled, its destination, and its operand descriptor. inverted: the I
   forms, whose address register offsets src2 */
typedef struct {
    float a[4], b[4];
    float* d;
    uint32_t pattern;
} Operands;
static Operands operands(Run* r, const arg_arith* x, bool inverted) {
    Operands o;
    o.pattern = r->s->swizzle[x->desc];
    swizzled(source(r->s, r->u, x->src1, inverted ? 0 : x->addr), o.pattern, 1, o.a);
    swizzled(source(r->s, r->u, x->src2, inverted ? x->addr : 0), o.pattern, 2, o.b);
    o.d = destination(r->u, x->dest);
    return o;
}

/* an instruction that writes each enabled component of the destination with value, of the components a and b */
#define COMPONENTWISE(NAME, INVERTED, value)                                                                           \
    static bool trans_##NAME(Run* r, arg_##NAME* x) {                                                                  \
        Operands o = operands(r, x, INVERTED);                                                                         \
        for (unsigned i = 0; i < 4; ++i) {                                                                             \
            float a = o.a[i], b = o.b[i];                                                                              \
            (void)b;                                                                                                   \
            if (enabled(o.pattern, i)) o.d[i] = value;                                                                 \
        }                                                                                                              \
        return true;                                                                                                   \
    }
COMPONENTWISE(ADD, false, a + b)
COMPONENTWISE(MUL, false, snx_ctr_pica_mul(a, b))
COMPONENTWISE(FLR, false, floorf(a))
/* MAX and MIN keep hardware's NaN results: max(0, NaN) is NaN, max(NaN, 0) is 0 */
COMPONENTWISE(MAX, false, a > b ? a : b)
COMPONENTWISE(MIN, false, a < b ? a : b)
COMPONENTWISE(MOV, false, a)
COMPONENTWISE(SGE, false, a >= b ? 1.0f : 0.0f)
COMPONENTWISE(SGEI, true, a >= b ? 1.0f : 0.0f)
COMPONENTWISE(SLT, false, a < b ? 1.0f : 0.0f)
COMPONENTWISE(SLTI, true, a < b ? 1.0f : 0.0f)

/* the dot product of the first n components (DPH and DPHI: with the w of src1 1) into each enabled component */
static bool dot(Run* r, const arg_arith* x, bool inverted, unsigned n, bool homogeneous) {
    Operands o = operands(r, x, inverted);
    float sum = 0.0f;
    if (homogeneous) o.a[3] = 1.0f;
    for (unsigned i = 0; i < n; ++i) sum = sum + snx_ctr_pica_mul(o.a[i], o.b[i]);
    for (unsigned i = 0; i < 4; ++i)
        if (enabled(o.pattern, i)) o.d[i] = sum;
    return true;
}
static bool trans_DP3(Run* r, arg_DP3* x) { return dot(r, x, false, 3, false); }
static bool trans_DP4(Run* r, arg_DP4* x) { return dot(r, x, false, 4, false); }
static bool trans_DPH(Run* r, arg_DPH* x) { return dot(r, x, false, 4, true); }
static bool trans_DPHI(Run* r, arg_DPHI* x) { return dot(r, x, true, 4, true); }

/* RCP, RSQ, EX2 and LG2: the result of the first component goes to every enabled component */
#define SCALAR(NAME, value)                                                                                            \
    static bool trans_##NAME(Run* r, arg_##NAME* x) {                                                                  \
        Operands o = operands(r, x, false);                                                                            \
        float a = o.a[0], result = value;                                                                              \
        for (unsigned i = 0; i < 4; ++i)                                                                               \
            if (enabled(o.pattern, i)) o.d[i] = result;                                                                \
        return true;                                                                                                   \
    }
SCALAR(RCP, 1.0f / a)
SCALAR(RSQ, 1.0f / sqrtf(a))
SCALAR(EX2, exp2f(a))
SCALAR(LG2, log2f(a))

static bool trans_MOVA(Run* r, arg_MOVA* x) {
    Operands o = operands(r, x, false);
    for (unsigned i = 0; i < 2; ++i)
        if (enabled(o.pattern, i)) r->u->address[i] = to_s32(o.a[i]);
    return true;
}

/* DST, LITP and DSTI: not implemented in Azahar either */
static bool trans_DST(Run* r, arg_DST* x) { return (void)r, (void)x, true; }
static bool trans_LITP(Run* r, arg_LITP* x) { return (void)r, (void)x, true; }
static bool trans_DSTI(Run* r, arg_DSTI* x) { return (void)r, (void)x, true; }

static bool compare(unsigned op, float a, float b, bool old) {
    switch (op) {
        case 0: return a == b;
        case 1: return a != b;
        case 2: return a < b;
        case 3: return a <= b;
        case 4: return a > b;
        case 5: return a >= b;
        default: return old; /* unknown compare modes leave the code as it was */
    }
}
static bool trans_CMP(Run* r, arg_CMP* x) {
    uint32_t pattern = r->s->swizzle[x->desc];
    float a[4], b[4];
    swizzled(source(r->s, r->u, x->src1, x->addr), pattern, 1, a);
    swizzled(source(r->s, r->u, x->src2, 0), pattern, 2, b);
    r->u->cc[0] = compare(x->cmpx, a[0], b[0], r->u->cc[0]);
    r->u->cc[1] = compare(x->cmpy, a[1], b[1], r->u->cc[1]);
    return true;
}

/* MAD and MADI (inverted: src3 takes the address register) */
static bool multiply_add(Run* r, const arg_mad* x, bool inverted) {
    uint32_t pattern = r->s->swizzle[x->desc];
    float a[4], b[4], c[4];
    swizzled(source(r->s, r->u, x->src1, 0), pattern, 1, a);
    swizzled(source(r->s, r->u, x->src2, inverted ? 0 : x->addr), pattern, 2, b);
    swizzled(source(r->s, r->u, x->src3, inverted ? x->addr : 0), pattern, 3, c);
    float* d = destination(r->u, x->dest);
    for (unsigned i = 0; i < 4; ++i)
        if (enabled(pattern, i)) d[i] = snx_ctr_pica_mul(a[i], b[i]) + c[i];
    return true;
}
static bool trans_MAD(Run* r, arg_MAD* x) { return multiply_add(r, x, false); }
static bool trans_MADI(Run* r, arg_MADI* x) { return multiply_add(r, x, true); }

/* ---- flow control. A jump sets pc to the instruction before its target, as the run goes on to pc + 1 ---- */
static bool trans_NOP(Run* r, arg_NOP* x) { return (void)r, (void)x, true; }
static bool trans_END(Run* r, arg_END* x) {
    (void)x;
    r->stop = true;
    return true;
}
static bool trans_JMPC(Run* r, arg_JMPC* x) {
    if (condition(r->u, x)) r->pc = x->dest - 1;
    return true;
}
static bool trans_JMPU(Run* r, arg_JMPU* x) {
    if (uniform_bool(r, x->id) == !(x->num & 1)) r->pc = x->dest - 1;
    return true;
}
static void call(Run* r, uint32_t dest, uint32_t num) {
    CallEntry e = {dest + num, r->pc + 1};
    PUSH(r->calls, r->call_count, 4u, e);
    r->pc = dest - 1;
}
static bool trans_CALL(Run* r, arg_CALL* x) {
    call(r, x->dest, x->num);
    return true;
}
static bool trans_CALLC(Run* r, arg_CALLC* x) {
    if (condition(r->u, x)) call(r, x->dest, x->num);
    return true;
}
static bool trans_CALLU(Run* r, arg_CALLU* x) {
    if (uniform_bool(r, x->id)) call(r, x->dest, x->num);
    return true;
}
/* the body runs on to the else part (dest), which ends at dest + num; a condition not met skips to the else part */
static void branch(Run* r, bool taken, uint32_t dest, uint32_t num) {
    if (taken) {
        IfEntry e = {dest, dest + num};
        PUSH(r->ifs, r->if_count, 8u, e);
    } else
        r->pc = dest - 1;
}
static bool trans_IFU(Run* r, arg_IFU* x) {
    branch(r, uniform_bool(r, x->id), x->dest, x->num);
    return true;
}
static bool trans_IFC(Run* r, arg_IFC* x) {
    branch(r, condition(r->u, x), x->dest, x->num);
    return true;
}
static bool trans_LOOP(Run* r, arg_LOOP* x) {
    const uint8_t* param = r->s->i[x->id];
    r->u->address[2] = param[1];
    /* do_loop reads aL after it is set, so previous_aL is the new value, as in Azahar */
    LoopEntry e = {r->pc + 1, (uint32_t)x->dest + 1, param[0], param[2], (uint8_t)r->u->address[2]};
    PUSH(r->loops, r->loop_count, 4u, e);
    return true;
}
static bool trans_BREAK(Run* r, arg_BREAK* x) {
    (void)x;
    r->is_break = true;
    return true;
}
static bool trans_BREAKC(Run* r, arg_BREAKC* x) {
    if (condition(r->u, x)) r->is_break = true;
    return true;
}
static bool trans_EMIT(Run* r, arg_EMIT* x) {
    (void)x;
    SnxCtrEmitter* emitter = r->emitter;
    if (emitter && emitter->emit) {
        emitter->emit(emitter->context, emitter->vertex_id, emitter->prim_emit, emitter->winding,
                      r->u->output[r->u->output_bank]);
        r->u->output_bank = !r->u->output_bank;
    }
    return true;
}
static bool trans_SETEMIT(Run* r, arg_SETEMIT* x) {
    if (r->emitter) {
        r->emitter->vertex_id = (unsigned)x->vertex;
        r->emitter->prim_emit = x->prim != 0;
        r->emitter->winding = x->winding != 0;
    }
    return true;
}

void snx_ctr_shader_run(const SnxCtrShaderSetup* s, SnxCtrShaderUnit* u, unsigned entry, SnxCtrEmitter* emitter) {
    Run r = {.s = s, .u = u, .emitter = emitter, .pc = entry};
    while (!r.stop) {
        r.is_break = false;
        const uint32_t old_pc = r.pc;
        /* the last word of program memory always ends the program (Azahar's guard against unterminated geometry
           shaders). A word that matches no pattern does nothing */
        decode_pica(&r, r.pc < PROGRAM_WORDS - 1 ? s->code[r.pc] : 0x22u << 26 /* END */);
        ++r.pc;
        /* the scopes close in the order CALL, IF, LOOP. Several CALL scopes may end at once; a JMP at the end of a
           scope is never taken, hence the comparisons with old_pc + 1 */
        uint32_t next = old_pc + 1;
        for (unsigned i = 0; i < 4; ++i) {
            if (!r.call_count || r.calls[r.call_count - 1].end_address != next) break;
            /* hardware does not update the program counter when it closes the fourth scope at once */
            if (i < 3) {
                r.pc = r.calls[r.call_count - 1].return_address;
                next = r.pc;
            }
            --r.call_count;
        }
        if (r.if_count && r.ifs[r.if_count - 1].else_address == old_pc + 1) {
            r.pc = r.ifs[r.if_count - 1].end_address;
            --r.if_count;
        }
        if (r.loop_count && (r.loops[r.loop_count - 1].end_address == old_pc + 1 || r.is_break)) {
            LoopEntry* l = &r.loops[r.loop_count - 1];
            u->address[2] += l->increment;
            if (!r.is_break && l->downcounter--)
                r.pc = l->entry_address;
            else {
                r.pc = l->end_address;
                /* aL is restored only inside an enclosing loop */
                if (r.loop_count > 1) u->address[2] = l->previous_al;
                --r.loop_count;
            }
        }
    }
}

void snx_ctr_shader_load(const uint32_t* regs, unsigned block, SnxCtrShaderUnit* u, const float (*buffer)[4]) {
    unsigned max = regs[block + SNX_PICA_SH_INPUT_CONFIG] & 0xf;
    uint64_t map = ((uint64_t)regs[block + SNX_PICA_SH_ATTR_MAP_HIGH] << 32) | regs[block + SNX_PICA_SH_ATTR_MAP_LOW];
    for (unsigned attr = 0; attr <= max; ++attr) memcpy(u->input[(map >> (attr * 4)) & 15], buffer[attr], 16);
}

void snx_ctr_shader_store(const uint32_t* regs, unsigned block, const SnxCtrShaderUnit* u, float (*buffer)[4]) {
    uint32_t mask = regs[block + SNX_PICA_SH_OUTPUT_MASK] & 0xffff;
    unsigned n = 0;
    for (unsigned reg = 0; reg < 16; ++reg)
        if (mask & (1u << reg)) memcpy(buffer[n++], u->output[u->output_bank][reg], 16);
}
