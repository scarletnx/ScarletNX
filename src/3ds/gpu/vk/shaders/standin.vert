// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The PICA200's vertex shader unit as one vertex shader that runs the program from a buffer: what a draw uses while
// the shader decompiled for its program (vsgen.c) is still being made with async_pipelines, so that the draw is drawn
// rather than skipped (vk/render.c). The flow control is gpu/shader.c's, ported from Azahar's shader interpreter
// (video_core/shader/shader_interpreter.cpp), with its IF, CALL and LOOP stacks; the arithmetic, the registers' first
// values and the vertex built from the outputs are vsgen.c's, ported from Azahar's GLSL decompiler and shader generator
// (GPL-2.0-or-later, Citra Emulator Project / Azahar Emulator Project): multiplication is the host's, RCP and RSQ leave
// the destination alone for a zero operand (RSQ: one not above zero), and a loop leaves aL where its last step put it.
// Instruction fields are nihstro's (BSD-3-Clause, Copyright 2014 Tony Wasserka; its notice is in gpu/shader.c).
// Its interface is vsgen.h's, with every input register a float vec4 (render.c gives the integer attributes scaled
// formats), the program in set 0 binding 6, and the config in push constants (StandInVertex in render.c).
// CLIP declares gl_ClipDistance, as vsgen.c does for a device that has it; render.c adds SignedZeroInfNanPreserve where
// the device has it, as vsgen.c declares it.
#version 450

layout(location = 0) in vec4 inputs[16];
layout(location = 0) out vec4 primary_color;
layout(location = 1) out vec2 texcoord0;
layout(location = 2) out vec2 texcoord1;
layout(location = 3) out vec2 texcoord2;
layout(location = 4) out float texcoord0_w;
layout(location = 5) out vec4 normquat;
layout(location = 6) out vec3 view;
#ifdef CLIP
out gl_PerVertex {
    vec4 gl_Position;
    float gl_ClipDistance[2];
};
#endif

layout(std140, set = 0, binding = 0) uniform VsPica {
    uint b;
    uvec4 i[4];
    vec4 f[96];
};
layout(std140, set = 0, binding = 1) uniform VsData {
    uint enable_clip1;
    uint flip_viewport;
    vec4 clip_coef;
};
layout(std430, set = 0, binding = 6) readonly buffer Program {
    uint code[4096];
    uint swizzle[128];
};
// after the fragment stand-in's: the program's entry, the output registers' attribute indices (a byte each, 16 for one
// the mask leaves out), the rasterizer's semantic map words and their count, each input register's size (four bits
// each, 0 for 4) and the switches (bit 0: the CPU made the vertex, as vsgen.c's passthrough)
layout(push_constant) uniform StandInVertex {
    layout(offset = 128) uint main_offset;
    uint num_outputs;
    uint output_map[4];
    uint attributes[7];
    uint total;
    uint input_sizes[2];
    uint switches;
};

const uint PROGRAM_END = 4096u, STEPS = 65536u;

vec4 reg_in[16], tmp[16], outv[16];
int addr[3];
bool cc[2];

uint field(uint word, uint shift, uint bits) { return (word >> shift) & ((1u << bits) - 1u); }

vec4 source(uint reg, uint address, uint pattern, uint n) {
    vec4 v;
    if (reg < 0x10u)
        v = reg_in[reg];
    else if (reg < 0x20u)
        v = tmp[reg - 0x10u];
    else if (address != 0u) {
        // get_offset_register: an offset outside a signed byte counts as 0, the index wraps at 128, past 96 reads ones
        int offset = addr[address - 1u];
        if (offset < -128 || offset > 127) offset = 0;
        uint index = uint(int(reg - 0x20u) + offset) & 0x7fu;
        v = index < 96u ? f[index] : vec4(1.0);
    } else
        v = f[reg - 0x20u];
    uint base = n == 1u ? 5u : n == 2u ? 14u : 23u;
    vec4 s = vec4(v[field(pattern, base + 6u, 2u)], v[field(pattern, base + 4u, 2u)], v[field(pattern, base + 2u, 2u)],
                  v[field(pattern, base, 2u)]);
    return field(pattern, base - 1u, 1u) != 0u ? -s : s;
}
// SetDest: the enabled components of the destination (an output the mask keeps, or a temporary)
void set_dest(uint reg, uint pattern, vec4 value) {
    bvec4 on = bvec4((pattern & 8u) != 0u, (pattern & 4u) != 0u, (pattern & 2u) != 0u, (pattern & 1u) != 0u);
    if (reg < 0x10u) {
        uint o = field(output_map[reg >> 2], (reg & 3u) * 8u, 8u);
        if (o < num_outputs) outv[o] = mix(outv[o], value, on);
    } else if (reg < 0x20u)
        tmp[reg - 0x10u] = mix(tmp[reg - 0x10u], value, on);
}
bool condition(uint instr) {
    bool x = field(instr, 25u, 1u) == uint(cc[0]), y = field(instr, 24u, 1u) == uint(cc[1]);
    switch (field(instr, 22u, 2u)) {
        case 0u: return x || y;
        case 1u: return x && y;
        case 2u: return x;
        default: return y;
    }
}
bool uniform_bool(uint id) { return ((b >> id) & 1u) != 0u; }
bool compare(uint op, float a, float c, bool old) {
    switch (op) {
        case 0u: return a == c;
        case 1u: return a != c;
        case 2u: return a < c;
        case 3u: return a <= c;
        case 4u: return a > c;
        case 5u: return a >= c;
        default: return old; // an unknown compare mode leaves the code
    }
}

// the scopes open, gpu/shader.c's: pushing onto a full stack drops its oldest entry
uvec2 ifs[8];   // else, end
uvec2 calls[4]; // end, return
uvec4 loops[4]; // entry, end, downcounter, increment
uint if_count, call_count, loop_count;
void push_if(uvec2 e) {
    if (if_count == 8u) {
        for (uint k = 0u; k < 7u; ++k) ifs[k] = ifs[k + 1u];
        --if_count;
    }
    ifs[if_count++] = e;
}
void push_call(uvec2 e) {
    if (call_count == 4u) {
        for (uint k = 0u; k < 3u; ++k) calls[k] = calls[k + 1u];
        --call_count;
    }
    calls[call_count++] = e;
}
void push_loop(uvec4 e) {
    if (loop_count == 4u) {
        for (uint k = 0u; k < 3u; ++k) loops[k] = loops[k + 1u];
        --loop_count;
    }
    loops[loop_count++] = e;
}

void run() {
    uint pc = main_offset;
    if_count = call_count = loop_count = 0u;
    for (uint steps = 0u; steps < STEPS; ++steps) {
        uint instr = pc < PROGRAM_END - 1u ? code[pc] : 0x22u << 26; // the last word always ends the program
        uint op = instr >> 26;
        uint old_pc = pc;
        bool stop = false;
        if (op >= 0x30u) {
            // MAD (111) and MADI (110): src3 or src2 takes the address register
            bool inverted = (op >> 3) == 6u;
            uint dest = field(instr, 24u, 5u), address = field(instr, 22u, 2u), desc = field(instr, 0u, 5u);
            uint src1 = field(instr, 17u, 5u);
            uint src2 = inverted ? field(instr, 12u, 5u) : field(instr, 10u, 7u);
            uint src3 = inverted ? field(instr, 5u, 7u) : field(instr, 5u, 5u);
            uint pattern = swizzle[desc];
            vec4 a = source(src1, 0u, pattern, 1u);
            vec4 c = source(src2, inverted ? 0u : address, pattern, 2u);
            vec4 d = source(src3, inverted ? address : 0u, pattern, 3u);
            set_dest(dest, pattern, a * c + d);
        } else if ((op >> 1) == 0x17u) {
            // CMP
            uint address = field(instr, 19u, 2u), pattern = swizzle[field(instr, 0u, 7u)];
            vec4 a = source(field(instr, 12u, 7u), address, pattern, 1u);
            vec4 c = source(field(instr, 7u, 5u), 0u, pattern, 2u);
            uint mx = field(instr, 24u, 3u), my = field(instr, 21u, 3u);
            if (mx <= 5u && my <= 5u) {
                cc[0] = compare(mx, a.x, c.x, cc[0]);
                cc[1] = compare(my, a.y, c.y, cc[1]);
            }
        } else if (op < 0x20u) {
            // the arithmetic formats: 1 (src1 takes the address register) and 1i (0x18-0x1b: src2 takes it)
            bool inverted = op >= 0x18u;
            uint dest = field(instr, 21u, 5u), address = field(instr, 19u, 2u), desc = field(instr, 0u, 7u);
            uint src1 = inverted ? field(instr, 14u, 5u) : field(instr, 12u, 7u);
            uint src2 = inverted ? field(instr, 7u, 7u) : field(instr, 7u, 5u);
            uint pattern = swizzle[desc];
            vec4 a = source(src1, inverted ? 0u : address, pattern, 1u);
            vec4 c = source(src2, inverted ? address : 0u, pattern, 2u);
            switch (op) {
                case 0x00u: set_dest(dest, pattern, a + c); break;                         // ADD
                case 0x01u: set_dest(dest, pattern, vec4(dot(a.xyz, c.xyz))); break;      // DP3
                case 0x02u: set_dest(dest, pattern, vec4(dot(a, c))); break;              // DP4
                case 0x03u: case 0x18u: set_dest(dest, pattern, vec4(dot(vec4(a.xyz, 1.0), c))); break; // DPH, DPHI
                case 0x05u: set_dest(dest, pattern, vec4(exp2(a.x))); break;              // EX2
                case 0x06u: set_dest(dest, pattern, vec4(log2(a.x))); break;              // LG2
                case 0x08u: set_dest(dest, pattern, a * c); break;                         // MUL
                case 0x09u: case 0x1au: set_dest(dest, pattern, vec4(greaterThanEqual(a, c))); break; // SGE, SGEI
                case 0x0au: case 0x1bu: set_dest(dest, pattern, vec4(lessThan(a, c))); break;          // SLT, SLTI
                case 0x0bu: set_dest(dest, pattern, floor(a)); break;                      // FLR
                case 0x0cu: set_dest(dest, pattern, max(a, c)); break;                     // MAX
                case 0x0du: set_dest(dest, pattern, min(a, c)); break;                     // MIN
                case 0x0eu: if (a.x != 0.0) set_dest(dest, pattern, vec4(1.0 / a.x)); break; // RCP
                case 0x0fu: if (a.x > 0.0) set_dest(dest, pattern, vec4(inversesqrt(a.x))); break; // RSQ
                case 0x12u:                                                                // MOVA
                    if ((pattern & 8u) != 0u) addr[0] = int(a.x);
                    if ((pattern & 4u) != 0u) addr[1] = int(a.y);
                    break;
                case 0x13u: set_dest(dest, pattern, a); break;                             // MOV
                default: break; // DST, LITP, DSTI: vsgen.c does not translate them, and the draw is never made
            }
        } else {
            uint dest = field(instr, 10u, 12u), num = field(instr, 0u, 8u);
            switch (op) {
                case 0x22u: stop = true; break; // END
                case 0x24u: push_call(uvec2(dest + num, pc + 1u)); pc = dest - 1u; break; // CALL
                case 0x25u: if (condition(instr)) { push_call(uvec2(dest + num, pc + 1u)); pc = dest - 1u; } break;
                case 0x26u: if (uniform_bool(field(instr, 22u, 4u))) { push_call(uvec2(dest + num, pc + 1u)); pc = dest - 1u; } break;
                case 0x27u: case 0x28u: { // IFU, IFC: the body runs on to the else part (dest), which ends at dest + num
                    bool taken = op == 0x27u ? uniform_bool(field(instr, 22u, 4u)) : condition(instr);
                    if (taken)
                        push_if(uvec2(dest, dest + num));
                    else
                        pc = dest - 1u;
                    break;
                }
                case 0x29u: { // LOOP: aL = i.y, the body up to dest i.x + 1 times, aL += i.z after each
                    uvec4 p = i[field(instr, 22u, 2u)];
                    addr[2] = int(p.y);
                    push_loop(uvec4(pc + 1u, dest + 1u, p.x, p.z));
                    break;
                }
                case 0x2cu: if (condition(instr)) pc = dest - 1u; break; // JMPC
                case 0x2du: if (uniform_bool(field(instr, 22u, 4u)) == ((num & 1u) == 0u)) pc = dest - 1u; break; // JMPU
                default: break; // NOP, EMIT, SETEMIT; BREAK and BREAKC, which vsgen.c does not translate
            }
        }
        if (stop) return;
        ++pc;
        // the scopes close in the order CALL, IF, LOOP; several CALLs may end at once
        uint next = old_pc + 1u;
        for (uint k = 0u; k < 4u; ++k) {
            if (call_count == 0u || calls[call_count - 1u].x != next) break;
            if (k < 3u) { // hardware does not move the program counter when it closes the fourth at once
                pc = calls[call_count - 1u].y;
                next = pc;
            }
            --call_count;
        }
        if (if_count != 0u && ifs[if_count - 1u].x == old_pc + 1u) {
            pc = ifs[if_count - 1u].y;
            --if_count;
        }
        if (loop_count != 0u && loops[loop_count - 1u].y == old_pc + 1u) {
            addr[2] += int(loops[loop_count - 1u].w);
            if (loops[loop_count - 1u].z != 0u) {
                --loops[loop_count - 1u].z;
                pc = loops[loop_count - 1u].x;
            } else {
                pc = loops[loop_count - 1u].y;
                --loop_count;
            }
        }
    }
}

// the rasterizer's semantic slot (PicaGSConfigState::GetSemanticMaps): the last output attribute and component that
// gives it, 1.0 where none does
float semantic(uint slot) {
    uint found_attr = 16u, found_comp = 0u;
    for (uint a = 0u; a < total; ++a)
        for (uint comp = 0u; comp < 4u; ++comp)
            if (field(attributes[a], comp * 8u, 5u) == slot) {
                found_attr = a;
                found_comp = comp;
            }
    return found_attr < num_outputs ? outv[found_attr][found_comp] : 1.0;
}

void main() {
    bool passthrough = (switches & 1u) != 0u;
    float v[24];
    if (passthrough) {
        for (uint k = 0u; k < 24u; ++k) v[k] = inputs[k / 4u][k % 4u];
    } else {
        for (uint r = 0u; r < 16u; ++r) {
            // the components the attribute lacks read as 0, 0, 0, 1
            uint size = field(input_sizes[r >> 3], (r & 7u) * 4u, 4u);
            vec4 x = inputs[r];
            if (size == 1u) x = vec4(x.x, 0.0, 0.0, 1.0);
            else if (size == 2u) x = vec4(x.xy, 0.0, 1.0);
            else if (size == 3u) x = vec4(x.xyz, 1.0);
            reg_in[r] = x;
            tmp[r] = vec4(0.0, 0.0, 0.0, 1.0);
            outv[r] = vec4(0.0, 0.0, 0.0, 1.0);
        }
        addr[0] = addr[1] = addr[2] = 0;
        cc[0] = cc[1] = false;
        run();
        for (uint k = 0u; k < 24u; ++k) v[k] = semantic(k);
    }
    // SanitizeVertex: z flushed to 0 when z / w is in (0, 1e-6), to -w when it is in (-1.00001, -1)
    float x = v[0], y = v[1], z = v[2], w = v[3];
    float ndc_z = z / w;
    if (ndc_z > 0.0 && ndc_z < 0.000001) z = 0.0;
    if (ndc_z < -1.0 && ndc_z > -1.00001) z = -w;
    if (flip_viewport != 0u) y = -y;
    gl_Position = vec4(x, y, -z, w);
#ifdef CLIP
    gl_ClipDistance[0] = -z;
    gl_ClipDistance[1] = enable_clip1 != 0u ? dot(clip_coef, vec4(x, y, z, w)) : 0.0;
#endif
    primary_color = min(abs(vec4(v[8], v[9], v[10], v[11])), vec4(1.0));
    texcoord0 = vec2(v[12], v[13]);
    texcoord1 = vec2(v[14], v[15]);
    texcoord2 = vec2(v[22], v[23]);
    texcoord0_w = v[16];
    normquat = vec4(v[4], v[5], v[6], v[7]);
    view = vec3(v[18], v[19], v[20]);
}
