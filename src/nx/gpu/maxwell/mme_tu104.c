/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Instruction format and semantics ported from Mesa a57603e485bd, src/nouveau/mme/mme_tu104.c and mme_tu104_sim.c:
   Copyright © 2022 Collabora Ltd. SPDX-License-Identifier: MIT
   Native loop and branch behavior cross-checked with eden-ns2's src/video_core/macro_turing.h, and the compute program
   selection and the data RAM shared with graphics from Eglantine's src/video_core/engines/kepler_compute.cpp:
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project */
#include "nx/gpu/maxwell/mme_tu104.h"
#include "nx/gpu/maxwell/engines_int.h"
#include "common/utilities/endian.h"
#include "common/utilities/bits.h"

static int fault(GpuChannel* ch, const char* reason) {
    WARN_LIMIT(32, "mme: TU104 channel %u at instruction %x: %s\n", ch->id, ch->mme.pc, reason);
    ch->mme.running = 0;
    return 0;
}
int mme_tu104_groups_grow(MmeTu104Group*** groups, u32* capacity) {
    u32 n = *capacity ? *capacity * 2 : 8;
    MmeTu104Group** p = realloc(*groups, (size_t)n * sizeof *p);
    if (!p) return -1;
    for (u32 i = *capacity; i < n; ++i) p[i] = NULL;
    *groups = p;
    *capacity = n;
    return 0;
}
/* The GPU thread owns the groups. An open TSG keeps its programs even without a channel reference. */
static void release_group(GpuChannel* ch) {
    u32 group = ch->tu104->group;
    ch->tu104->group = 0;
    if (!group) return;
    MmeTu104Group* owner = g_gpu.tu104[group - 1];
    if (--owner->refs || (owner->tsg && !owner->closed)) return;
    free(g_gpu.tu104[group - 1]);
    g_gpu.tu104[group - 1] = NULL;
}
void mme_tu104_tsg_close(u64 tsg) {
    for (u32 i = 0; i < g_gpu.tu104_capacity; ++i) {
        MmeTu104Group* group = g_gpu.tu104[i];
        if (!group || group->tsg != tsg) continue;
        group->closed = 1;
        if (!group->refs) {
            free(group);
            g_gpu.tu104[i] = NULL;
        }
        return;
    }
}
void mme_tu104_free(GpuChannel* ch) {
    if (!ch->tu104) return;
    release_group(ch);
    free(ch->tu104);
    ch->tu104 = NULL;
}
void mme_tu104_set_group(GpuChannel* ch, u64 tsg) {
    if (ch->tsg == tsg) return;
    if (ch->tu104) release_group(ch);
    ch->tsg = tsg;
}
static MmeTu104* state(GpuChannel* ch) {
    if (!ch->tu104) {
        ch->tu104 = calloc(1, sizeof *ch->tu104);
        if (!ch->tu104) fatal("mme: out of memory for TU104 channel %u\n", ch->id);
    }
    MmeTu104* t = ch->tu104;
    if (!t->group) {
        u32 empty = g_gpu.tu104_capacity;
        for (u32 i = 0; i < g_gpu.tu104_capacity; ++i) {
            if (!g_gpu.tu104[i]) {
                if (empty == g_gpu.tu104_capacity) empty = i;
            } else if (ch->tsg && g_gpu.tu104[i]->tsg == ch->tsg) {
                t->group = i + 1;
                ++g_gpu.tu104[i]->refs;
                return t;
            }
        }
        if (empty == g_gpu.tu104_capacity && mme_tu104_groups_grow(&g_gpu.tu104, &g_gpu.tu104_capacity))
            fatal("mme: out of memory for TU104 group table\n");
        MmeTu104Group* group = calloc(1, sizeof *group);
        if (!group) fatal("mme: out of memory for TU104 programs\n");
        group->tsg = ch->tsg;
        group->refs = 1;
        g_gpu.tu104[empty] = group;
        t->group = empty + 1;
    }
    return t;
}
static u32* registers(GpuChannel* ch, u32 engine) { return engine == 1 ? ch->r3d : ch->rcomp; }
/* Mesa numbers the low word first internally. The three uploaded words are in reverse order. */
static u32 field(const u32* code, u32 bit, u32 width) {
    u32 word = bit / 32, shift = bit % 32;
    u32 v = code[2 - word] >> shift;
    if (shift + width > 32) v |= code[1 - word] << (32 - shift);
    return v & ((1u << width) - 1);
}
static u32 source(GpuChannel* ch, u32 reg, u32 lane) {
    MmeTu104* t = ch->tu104;
    if (reg < 24) {
        if (!(t->initialized & (1u << reg))) fault(ch, "uninitialized register");
        return t->regs[reg];
    }
    switch (reg) {
        case 24: return 0;
        case 25: return (u32)(i32)(i16)t->imm[lane];
        case 26: return (u32)(i32)(i16)t->imm[1 - lane];
        case 27: return t->imm[0] << 16 | t->imm[1];
        case 28: return t->load[0];
        case 29: return t->load[1];
        default: fault(ch, "invalid source register"); return 0;
    }
}
static u32 parameter(GpuChannel* ch) {
    MmeTu104* t = ch->tu104;
    if (t->fifo_count) {
        if (t->fifo_next < t->fifo_count) return t->fifo[t->fifo_next++];
    } else if (ch->mme.next < ch->mme.np)
        return ch->mme_params[ch->mme.next++];
    fault(ch, "parameter FIFO underflow");
    return 0;
}
static int condition(u32 op, u32 x, u32 y) {
    switch (op) {
        case 17:
        case 25: return (i32)x < (i32)y;
        case 18:
        case 26: return x < y;
        case 19:
        case 27: return (i32)x <= (i32)y;
        case 20:
        case 28: return x <= y;
        default: return x == y;
    }
}
static u32 alu(GpuChannel* ch, u32 op, u32 x, u32 y, u32 lane) {
    GpuMme* m = &ch->mme;
    MmeTu104* t = ch->tu104;
    u32 imm = t->imm[lane], value;
    switch (op) {
        case 0:
            value = x + y;
            m->carry = value < x;
            return value;
        case 1: return x + y + m->carry;
        case 2:
            value = x - y;
            m->carry = value > x;
            return value;
        case 3: return x - y - m->carry;
        case 4:
        case 6: {
            u64 a = op == 4 ? (u64)(i64)(i32)x : x, b = op == 4 ? (u64)(i64)(i32)y : y;
            u64 product = a * b;
            m->carry = (u32)(product >> 32);
            return (u32)product;
        }
        case 5: return m->carry;
        case 7: /* The extended barrier makes READ_FIFOED's memory available as the parameter FIFO. */
            if (x != 0x1000 || y != 1) {
                fault(ch, "unknown extended operation");
                return 0;
            }
            if (t->fifo_dma[t->engine - 1]) {
                r3d_const_flush(ch);
                r3d_pending_flush();
                u32* r = registers(ch, t->engine);
                if (gmem_read(gpu_packed_va(r[0x154], r[0x155]), t->fifo, (u64)t->fifo_dma[t->engine - 1] * 4)) {
                    fault(ch, "unmapped FIFO DMA source");
                    return 0;
                }
                t->fifo_count = t->fifo_dma[t->engine - 1];
                snx_le32_array(t->fifo, t->fifo_count);
                t->fifo_next = t->fifo_dma[t->engine - 1] = 0;
            }
            return 0;
        case 8: return snx_clz32(x);
        case 9: return x << (y & 31);
        case 10: return x >> (y & 31);
        case 11: return (u32)((i32)x >> (y & 31));
        case 12: return x & y;
        case 13: return ~(x & y);
        case 14: return x | y;
        case 15: return x ^ y;
        case 16: {
            u32 dst = imm >> 10, bits = (imm >> 5) & 31, src = imm & 31, mask = (1u << bits) - 1;
            if (dst >= 32) {
                fault(ch, "invalid merge destination");
                return 0;
            }
            return (x & ~(mask << dst)) | (((y >> src) & mask) << dst);
        }
        case 17:
        case 18:
        case 19:
        case 20:
        case 21: return condition(op, x, y) ? ~0u : 0;
        case 22:
            value = (x + y) & 0xFFFF;
            if (value >= GPU_ENGINE_METHODS) {
                fault(ch, "state address outside registers");
                return 0;
            }
            return registers(ch, t->engine)[value];
        case 23:
            if (t->loop_count || imm < 2) {
                fault(ch, "invalid loop");
                return 0;
            }
            /* Native zero-count loops skip their FIFO reads, unlike Mesa's test simulator's MAX(1, x). */
            if (!x)
                t->next_pc = m->pc + imm;
            else {
                t->loop_count = x - 1;
                t->loop_start = m->pc + 1;
                t->loop_end = m->pc + imm - 1;
            }
            return 0;
        case 24: t->next_pc = m->pc + (imm & 0x7FFF); return 0;
        case 25:
        case 26:
        case 27:
        case 28:
        case 29:
            if (condition(op, x, y) == !!(imm & 0x8000)) {
                i32 offset = (i32)((imm & 0x1FFF) ^ 0x1000) - 0x1000;
                if (offset == -4096)
                    t->stop = 1;
                else {
                    t->next_pc = m->pc + (u32)offset;
                    /* Native back edges can cancel this instruction's fallthrough exit. */
                    if (imm & 0x4000) t->end_next = 0;
                }
            }
            return 0;
        case 30:
            if (x >= TU104_DRAM_WORDS) {
                fault(ch, "data RAM read outside bounds");
                return 0;
            }
            return t->dram[x];
        case 31:
            if (x >= TU104_DRAM_WORDS) {
                fault(ch, "data RAM write outside bounds");
                return 0;
            }
            t->dram[x] = y;
            return 0;
        default: fault(ch, "unknown ALU operation"); return 0;
    }
}
static u32 output_value(GpuChannel* ch, u32 selector) {
    MmeTu104* t = ch->tu104;
    switch (selector) {
        case 1: return t->alu[0];
        case 2: return t->alu[1];
        case 3: return t->load[0];
        case 4: return t->load[1];
        case 5: return t->imm[0];
        case 6: return t->imm[1];
        case 8: return t->imm[0] >> 12;
        case 9: return t->imm[1] >> 12;
        case 10: return t->imm[0] << 16 | t->imm[1];
        default: fault(ch, "invalid output selector"); return 0;
    }
}
/* instruction pc of p decoded from its words, after one of them changed */
static void decode(MmeTu104Program* p, u32 pc) {
    static const char predicates[16][5] = {"UUUU", "TTTT", "FFFF", "TTUU", "FFUU", "TFUU", "TUUU", "FUUU",
                                           "UUTT", "UUTF", "UUTU", "UUFT", "UUFF", "UUFU", "UUUT", "UUUF"};
    const u32* code = p->code + pc * 3;
    MmeTu104Ins in = {.pred = (u8)field(code, 5, 5), .mode = (u8)field(code, 1, 4), .end_next = (u8)field(code, 0, 1)};
    for (u32 lane = 0; lane < 2; ++lane) {
        in.op[lane] = (u8)field(code, 10 + 36 * lane, 5);
        in.dst[lane] = (u8)field(code, 15 + 36 * lane, 5);
        in.a[lane] = (u8)field(code, 20 + 36 * lane, 5);
        in.b[lane] = (u8)field(code, 25 + 36 * lane, 5);
        in.imm[lane] = (u16)field(code, 30 + 36 * lane, 16);
        in.out_method[lane] = (u8)field(code, 82 + 7 * lane, 3);
        in.out_emit[lane] = (u8)field(code, 85 + 7 * lane, 4);
    }
    for (u32 lane = 0; lane < 2; ++lane) {
        u32 reg = 28 + lane, out = 3 + lane;
        if (in.pred == reg || in.a[0] == reg || in.a[1] == reg || in.b[0] == reg || in.b[1] == reg ||
            in.out_method[0] == out || in.out_method[1] == out || in.out_emit[0] == out || in.out_emit[1] == out)
            in.load_mask |= (u8)(1u << lane);
    }
    for (u32 lane = 0; lane < 4; ++lane) {
        if (predicates[in.mode][lane] == 'F') in.off[0] |= (u8)(1u << lane);
        if (predicates[in.mode][lane] == 'T') in.off[1] |= (u8)(1u << lane);
    }
    p->ins[pc] = in;
}
static void step(GpuChannel* ch) {
    GpuMme* m = &ch->mme;
    MmeTu104* t = ch->tu104;
    MmeTu104Group* group = g_gpu.tu104[t->group - 1];
    MmeTu104Program* p = &group->program[t->program];
    if (m->pc >= TU104_INSTRUCTIONS || m->pc * 3 + 3 > p->words) {
        fault(ch, "instruction outside uploaded RAM");
        return;
    }
    const MmeTu104Ins* in = &p->ins[m->pc];
    t->end_next = in->end_next;
    t->next_pc = m->pc + 1;
    t->stop = 0;
    for (u32 lane = 0; lane < 2; ++lane) {
        t->imm[lane] = in->imm[lane];
        t->out_method[lane] = in->out_method[lane];
        t->out_emit[lane] = in->out_emit[lane];
    }
    for (u32 lane = 0; lane < 2; ++lane) t->load[lane] = in->load_mask & (1u << lane) ? parameter(ch) : 0;
    if (!m->running) return;
    u32 truth = in->mode ? source(ch, in->pred, 0) != 0 : 0;
    t->mask = ~(u32)in->off[truth] & 15;
    /* Both ALUs read the input register file. Capture only their four operands, not a copy of all registers. */
    u32 x[2] = {0}, y[2] = {0};
    for (u32 lane = 0; lane < 2 && m->running; ++lane)
        if (t->mask & (1u << lane)) {
            x[lane] = source(ch, in->a[lane], lane);
            y[lane] = source(ch, in->b[lane], lane);
        }
    for (u32 lane = 0; lane < 2 && m->running; ++lane) {
        t->alu[lane] = 0;
        if (!(t->mask & (1u << lane))) continue;
        t->alu[lane] = alu(ch, in->op[lane], x[lane], y[lane], lane);
        if (!m->running) return;
        if (in->dst[lane] < 24) {
            t->regs[in->dst[lane]] = t->alu[lane];
            t->initialized |= 1u << in->dst[lane];
        } else if (in->dst[lane] != 24) {
            fault(ch, "invalid destination register");
            return;
        }
    }
    t->output = 0;
}
/* The engine handlers keep MME DMA addresses as ordinary registers. Transfers use guest ranges, never PCIe memory. */
static int mme_dma(GpuChannel* ch, u32 engine, u32 method, u32 value) {
    MmeTu104* t = state(ch);
    u32* r = registers(ch, engine);
    r[method] = value;
    u32 addr = r[0x156];
    if (method == 0x157 || method == 0x159) {
        r3d_const_flush(ch);
        r3d_pending_flush();
        if (addr >= TU104_DRAM_WORDS || value > TU104_DRAM_WORDS - addr)
            return fault(ch, "data RAM DMA outside bounds");
        u32* words = &t->dram[addr];
        u64 address = gpu_packed_va(r[0x154], r[0x155]);
        if (method == 0x157) {
            if (gmem_read(address, words, (u64)value * 4)) return fault(ch, "unmapped RAM DMA source");
            snx_le32_array(words, value);
        } else {
            snx_le32_array(words, value);
            int error = gmem_write(address, words, (u64)value * 4);
            snx_le32_array(words, value);
            if (error) return fault(ch, "unmapped RAM DMA destination");
        }
    } else if (method == 0x158) {
        if (value > TU104_FIFO_WORDS) return fault(ch, "FIFO DMA outside bounds");
        t->fifo_dma[engine - 1] = value;
    } else if (method == 0x15C) {
        if (addr >= TU104_DRAM_WORDS) return fault(ch, "data RAM sync outside bounds");
        t->dram[addr] = value;
    } else if (method == 0x15A || method == 0x15B) {
        return fault(ch, "unsupported MME DMA operation");
    }
    return 1;
}
void mme_tu104_resume(GpuChannel* ch) {
    GpuMme* m = &ch->mme;
    MmeTu104* t = ch->tu104;
    while (m->running && !ch->park) {
        if (t->output == 2) {
            if (++m->steps > MME_STEPS) {
                fault(ch, "instruction limit exceeded");
                return;
            }
            step(ch);
            if (!m->running) return;
        }
        while (t->output < 2 && m->running && !ch->park) {
            u32 lane = t->output++;
            if (!(t->mask & (4u << lane))) continue;
            if (t->out_method[lane]) {
                u32 address = output_value(ch, t->out_method[lane]);
                m->method = address & 0xFFF;
                m->incr = address >> 12 & 15;
            }
            if (!m->running) return;
            if (t->out_emit[lane]) {
                u32 value = output_value(ch, t->out_emit[lane]), method = m->method;
                m->method += m->incr;
                if (!m->running) return;
                if (method >= GPU_ENGINE_METHODS) {
                    fault(ch, "method outside registers");
                    return;
                }
                if (method >= 0xE00)
                    registers(ch, t->engine)[method] = value;
                else if ((method >= 0x45 && method <= 0x48) || (method >= 0x154 && method <= 0x15C))
                    mme_tu104_method(ch, t->engine, method, value, 1);
                else if (t->engine == 1)
                    maxwell3d_method(ch, method, value, !(method >= 0x8E4 && method <= 0x8F3));
                else
                    compute_method(ch, method, value, 1);
            }
        }
        if (t->output < 2 || !m->running) return;
        if (m->exiting || t->stop) {
            m->running = 0;
            LOG_LIMIT(32, "mme: TU104 channel %u completed %u instructions with %u parameters\n", ch->id, m->steps,
                      m->next);
            return;
        }
        m->exiting = (int)t->end_next;
        if (t->loop_count && m->pc == t->loop_end) {
            --t->loop_count;
            t->next_pc = t->loop_start;
        }
        m->pc = t->next_pc;
    }
}
static void start(GpuChannel* ch, u32 engine, u32 slot) {
    MmeTu104* t = state(ch);
    MmeTu104Group* group = g_gpu.tu104[t->group - 1];
    t->program = engine == 2 && group->compute;
    MmeTu104Program* p = &group->program[t->program];
    ch->mme = (GpuMme){.pc = p->start[slot & 0x7F], .np = ch->mme_nparams, .running = 1};
    t->engine = engine;
    t->initialized = 0;
    t->fifo_count = t->fifo_next = 0;
    t->loop_count = 0;
    t->output = 2;
    mme_tu104_resume(ch);
}
int mme_tu104_method(GpuChannel* ch, u32 engine, u32 method, u32 value, int last) {
    if (method < 0xE00 && (method < 0x45 || method > 0x48) && (method < 0x154 || method > 0x15C)) return 0;
    if (engine == 1) r3d_const_flush(ch);
    if (method >= 0xE00) {
        MmeTu104* t = state(ch);
        if (!ch->mme_collecting) {
            ch->mme_collecting = 1;
            ch->mme_index = ((method - 0xE00) >> 1) & 0x7F;
            ch->mme_nparams = 0;
            t->overflow = 0;
        }
        if (ch->mme_nparams >= sizeof ch->mme_params / sizeof *ch->mme_params) {
            if (!t->overflow) fault(ch, "too many parameters");
            t->overflow = 1;
        } else {
            ch->mme_params[ch->mme_nparams++] = value;
        }
        registers(ch, engine)[method & 0xFFF] = value;
        if (last) {
            ch->mme_collecting = 0;
            if (!t->overflow) start(ch, engine, ch->mme_index);
        }
        return 1;
    }
    if (method >= 0x154 && method <= 0x15C) {
        mme_dma(ch, engine, method, value);
        return 1;
    }
    MmeTu104* t = state(ch);
    registers(ch, engine)[method] = value;
    MmeTu104Group* group = g_gpu.tu104[t->group - 1];
    if (engine == 2 && (method == 0x45 || method == 0x47)) group->compute = 1;
    MmeTu104Program* p = &group->program[engine == 2 && group->compute];
    switch (method) {
        case 0x45: p->upload = value < TU104_INSTRUCTIONS ? value * 3 : TU104_INSTRUCTIONS * 3; break;
        case 0x46:
            if (p->upload >= TU104_INSTRUCTIONS * 3) {
                fault(ch, "instruction upload outside RAM");
                break;
            }
            p->code[p->upload++] = value;
            if (p->upload > p->words) p->words = p->upload;
            decode(p, (p->upload - 1) / 3);
            break;
        case 0x47: p->bind = value; break;
        case 0x48:
            if (p->bind >= 0x80 || value >= TU104_INSTRUCTIONS) {
                fault(ch, "macro binding outside RAM");
                break;
            }
            p->start[p->bind++] = value;
            break;
    }
    return 1;
}
