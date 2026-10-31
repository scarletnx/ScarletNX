/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Tests of the PICA200 command processor (src/3ds/gpu/pica.c) and the software pipeline behind its draws
   (shader.c, draw.c, raster.c, texture.c): command lists written into the board's FCRAM and run from their
   physical address. The expected values come from the layouts Azahar documents (CommandHeader in pica_core.h,
   PackedAttribute's float24 packing, pica_types.h's float24, nihstro's instruction encoding, the tiled framebuffer
   of sw_framebuffer.cpp), worked out by hand here. */
#include "3ds/gpu/pica.h"
#include "3ds/gpu/shader.h"
#include "3ds/gpu/texture.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FCRAM 0x20000000u

static int failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

/* a command list being written: [value, header] pairs, extra words after their header, 8-byte aligned */
typedef struct {
    uint32_t w[256];
    unsigned n;
} List;
static void cmd(List* l, uint32_t id, uint32_t mask, bool group, const uint32_t* v, unsigned count) {
    unsigned i;
    l->w[l->n++] = v[0];
    l->w[l->n++] = id | (mask << 16) | ((count - 1) << 20) | (group ? 1u << 31 : 0);
    for (i = 1; i < count; ++i) l->w[l->n++] = v[i];
    if (l->n % 2) l->w[l->n++] = 0;
}
static void put(List* l, uint32_t id, uint32_t value) { cmd(l, id, 15, false, &value, 1); }

static void on_interrupt(void* c) { ++*(unsigned*)c; }

static SnxCtrPicaResult run(SnxCtrPica* p, SnxCtrMemory* m, uint32_t pa, const List* l, const SnxCtrPicaHost* h) {
    if (snx_ctr_phys_write(m, pa, l->w, l->n * 4) != SNX_CTR_MEM_OK) return SNX_CTR_PICA_FAULT;
    return snx_ctr_pica_run(p, m, pa, l->n * 4, h);
}

static void float24(void) {
    check(snx_ctr_f24(0x3f0000) == 1.0f && snx_ctr_f24(0xc00000) == -2.0f && snx_ctr_f24(0x3e0000) == 0.5f,
          "float24 normals: bias 63, mantissa above bit 16");
    check(snx_ctr_f24(0) == 0.0f && signbit(snx_ctr_f24(0x800000)), "float24 zeros keep their sign");
    /* exponent 0, mantissa 0x8000: 0.5 * 2^(1-63), renormalized into a float32 normal */
    check(snx_ctr_f24(0x008000) == ldexpf(1.0f, -63), "float24 denormals are converted exactly");
    check(isinf(snx_ctr_f24(0x7f0000)) && isnan(snx_ctr_f24(0x7f0001)), "an all-ones exponent is inf or NaN");
    check(snx_ctr_pica_mul(INFINITY, 0.0f) == 0.0f && isnan(snx_ctr_pica_mul(NAN, 1.0f)) &&
              snx_ctr_pica_mul(3.0f, -2.0f) == -6.0f,
          "PICA multiplication: inf * 0 is 0, NaN stays NaN");
}

static void writes(SnxCtrMemory* m) {
    SnxCtrPica* p = calloc(1, sizeof *p);
    List l = {{0}, 0};
    const uint32_t seq[3] = {0x11, 0x22, 0x33}, oob = 0xffffffffu, masked = 0xaabbccddu;
    snx_ctr_pica_init(p);
    check(p->regs[SNX_PICA_IRQ_AUTOSTOP] == 1 && p->regs[SNX_PICA_IRQ_MASK] == 0xfffffff0u &&
              p->regs[SNX_PICA_IRQ_COMPARE] == 0x12345678u &&
              p->regs[SNX_PICA_GS + SNX_PICA_SH_INPUT_CONFIG] == 0xa0000001u,
          "registers start as GSP leaves them");
    p->regs[0x100] = 0x01020304u;
    cmd(&l, 0x100, 5, false, &masked, 1); /* bytes 0 and 2 */
    cmd(&l, 0x101, 15, true, seq, 3);     /* 0x101, 0x102, 0x103 */
    cmd(&l, 0x2ff, 15, true, seq, 3);     /* 0x2ff, then two writes past the register file */
    cmd(&l, 0x300, 15, false, &oob, 1);
    check(run(p, m, FCRAM, &l, NULL) == SNX_CTR_PICA_OK, "a list of plain writes runs");
    check(p->regs[0x100] == 0x01bb03ddu, "the parameter mask enables bytes");
    check(p->regs[0x101] == 0x11 && p->regs[0x102] == 0x22 && p->regs[0x103] == 0x33,
          "grouped extra words go to consecutive registers");
    check(p->regs[0x2ff] == 0x11, "writes past the register file are ignored");
    snx_ctr_pica_free(p);
    free(p);
}

static void uploads(SnxCtrMemory* m) {
    SnxCtrPica* p = calloc(1, sizeof *p);
    List l = {{0}, 0};
    /* x 1.0, y 2.0, z -2.0, w 0.5 packed as PackedAttribute reads it: w:z.hi, z.lo:y.hi, y.lo:x */
    const uint32_t f24[3] = {0x3e0000c0u, 0x00004000u, 0x003f0000u};
    const uint32_t code[3] = {0xa, 0xb, 0xc}, lut[2] = {0x55, 0x66};
    uint32_t f32[4];
    float v[4] = {4.0f, 3.0f, 2.0f, 1.0f}; /* w, z, y, x as the words come */
    unsigned i;
    for (i = 0; i < 4; ++i) memcpy(&f32[i], &v[i], 4);
    snx_ctr_pica_init(p);
    put(&l, SNX_PICA_VS + SNX_PICA_SH_UNIFORM_SETUP, 5);
    cmd(&l, SNX_PICA_VS + SNX_PICA_SH_UNIFORM_VALUE, 15, false, f24, 3);
    put(&l, SNX_PICA_VS + SNX_PICA_SH_UNIFORM_SETUP, (1u << 31) | 95);
    cmd(&l, SNX_PICA_VS + SNX_PICA_SH_UNIFORM_VALUE, 15, false, f32, 4);
    cmd(&l, SNX_PICA_VS + SNX_PICA_SH_UNIFORM_VALUE, 15, false, f32, 4); /* index 96: dropped */
    put(&l, SNX_PICA_VS + SNX_PICA_SH_PROGRAM_OFFSET, 0);
    cmd(&l, SNX_PICA_VS + SNX_PICA_SH_PROGRAM_WORD, 15, false, code, 3);
    put(&l, SNX_PICA_VS + SNX_PICA_SH_BOOL, 0x7fff8001u);
    put(&l, SNX_PICA_VS + SNX_PICA_SH_INT + 2, 0x04030201u);
    put(&l, SNX_PICA_LIGHTING_LUT_CONFIG, (3u << 8) | 0x10);
    cmd(&l, SNX_PICA_LIGHTING_LUT_DATA, 15, false, lut, 2);
    put(&l, SNX_PICA_FOG_LUT_OFFSET, 127);
    cmd(&l, SNX_PICA_FOG_LUT_DATA, 15, false, lut, 2);
    check(run(p, m, FCRAM, &l, NULL) == SNX_CTR_PICA_OK, "an upload list runs");
    check(p->vs.f[5][0] == 1.0f && p->vs.f[5][1] == 2.0f && p->vs.f[5][2] == -2.0f && p->vs.f[5][3] == 0.5f,
          "a float24 uniform is three words");
    check(p->vs.f[95][0] == 1.0f && p->vs.f[95][3] == 4.0f, "a float32 uniform is four words, x last");
    check((p->regs[SNX_PICA_VS + SNX_PICA_SH_UNIFORM_SETUP] & 0x7f) == 96, "the uniform index stops past 95");
    check(!memcmp(p->gs.f[5], p->vs.f[5], sizeof p->vs.f[5]), "uniforms reach the shared unit");
    check(p->vs.code[0] == 0xa && p->vs.code[2] == 0xc && p->gs.code[1] == 0xb &&
              p->regs[SNX_PICA_VS + SNX_PICA_SH_PROGRAM_OFFSET] == 3,
          "program words are uploaded to both units, the offset advances");
    check(p->vs.b == 0x8001 && p->gs.b == 0x8001, "16 bool uniforms");
    check(p->vs.i[2][0] == 1 && p->vs.i[2][3] == 4 && p->gs.i[2][1] == 2, "int uniforms are x y z w bytes");
    check(p->lighting_lut[3][0x10] == 0x55 && p->lighting_lut[3][0x11] == 0x66 &&
              (p->regs[SNX_PICA_LIGHTING_LUT_CONFIG] & 0xff) == 0x12,
          "lighting LUT writes advance the index");
    check(p->fog_lut[127] == 0x55 && p->fog_lut[0] == 0x66, "the fog LUT index wraps at 128");

    /* unit 3 exclusive: the vertex unit's uploads stay its own */
    l.n = 0;
    put(&l, SNX_PICA_GS_EXCLUSIVE, 1);
    put(&l, SNX_PICA_VS + SNX_PICA_SH_PROGRAM_OFFSET, 0);
    cmd(&l, SNX_PICA_VS + SNX_PICA_SH_PROGRAM_WORD, 15, false, lut, 1);
    check(run(p, m, FCRAM, &l, NULL) == SNX_CTR_PICA_OK && p->vs.code[0] == 0x55 && p->gs.code[0] == 0xa,
          "an exclusive geometry unit keeps its program");
    snx_ctr_pica_free(p);
    free(p);
}

static void control(SnxCtrMemory* m) {
    SnxCtrPica* p = calloc(1, sizeof *p);
    unsigned interrupts = 0;
    SnxCtrPicaHost host = {&interrupts, on_interrupt, snx_ctr_pica_draw, snx_ctr_pica_draw_immediate, NULL};
    List l = {{0}, 0}, sub = {{0}, 0};
    const uint32_t attr[3] = {0x3e0000c0u, 0x00004000u, 0x003f0000u};
    snx_ctr_pica_init(p);

    /* P3D: a byte of the request equal to the same byte of the compare value; autostop ends the list */
    put(&l, SNX_PICA_IRQ_REQUEST, 0x00000099u);
    put(&l, 0x110, 1);
    put(&l, SNX_PICA_IRQ_REQUEST, 0x00000078u);
    put(&l, 0x111, 1);
    check(run(p, m, FCRAM, &l, &host) == SNX_CTR_PICA_OK, "an interrupting list runs");
    check(interrupts == 1 && p->regs[0x110] == 1 && p->regs[0x111] == 0,
          "a matching request raises P3D and stops the list");

    /* a jump: the second channel's list replaces the rest of this one */
    l.n = 0;
    put(&sub, 0x112, 7);
    put(&l, SNX_PICA_CMDBUF_SIZE + 1, sub.n * 4 / 8);
    put(&l, SNX_PICA_CMDBUF_ADDR + 1, (FCRAM + 0x1000) / 8);
    put(&l, SNX_PICA_CMDBUF_TRIGGER + 1, 1);
    put(&l, 0x113, 1);
    check(snx_ctr_phys_write(m, FCRAM + 0x1000, sub.w, sub.n * 4) == SNX_CTR_MEM_OK, "the sublist is written");
    check(run(p, m, FCRAM, &l, &host) == SNX_CTR_PICA_OK && p->regs[0x112] == 7 && p->regs[0x113] == 0,
          "a command buffer trigger jumps and does not return");
    l.n = 0;
    put(&l, SNX_PICA_CMDBUF_SIZE, 1);
    put(&l, SNX_PICA_CMDBUF_ADDR, 0x08000000u / 8); /* nothing at 0x08000000 on the board */
    put(&l, SNX_PICA_CMDBUF_TRIGGER, 1);
    check(run(p, m, FCRAM, &l, &host) == SNX_CTR_PICA_FAULT, "a jump outside memory faults");

    /* a draw with a geometry shader whose unit is not configured as one stops the list and says why */
    l.n = 0;
    put(&l, SNX_PICA_USE_GS, 2);
    put(&l, SNX_PICA_TRIGGER_DRAW, 1);
    put(&l, SNX_PICA_USE_GS, 0);
    check(run(p, m, FCRAM, &l, &host) == SNX_CTR_PICA_UNSUPPORTED && p->stop_register == SNX_PICA_TRIGGER_DRAW &&
              !strcmp(p->stop_reason, "the geometry shader's unit is not in geometry mode") && (p->regs[SNX_PICA_USE_GS] & 3) == 2,
          "a geometry shader draw stops");
    p->regs[SNX_PICA_USE_GS] = 0;

    /* default attributes, then immediate mode with two input attributes */
    l.n = 0;
    put(&l, SNX_PICA_DEFAULT_ATTR_INDEX, 3);
    cmd(&l, SNX_PICA_DEFAULT_ATTR_VALUE, 15, false, attr, 3);
    put(&l, SNX_PICA_MAX_INPUT_ATTR, 1);
    put(&l, SNX_PICA_DEFAULT_ATTR_INDEX, 15);
    cmd(&l, SNX_PICA_DEFAULT_ATTR_VALUE, 15, false, attr, 3);
    p->regs[0x1c6] = 1; /* lighting off, as libctru and citro3d leave it */
    check(run(p, m, FCRAM, &l, &host) == SNX_CTR_PICA_OK && p->assembler.index == 0, "one attribute is no vertex");
    check(p->default_attributes[3][0] == 1.0f && p->default_attributes[3][3] == 0.5f &&
              p->regs[SNX_PICA_DEFAULT_ATTR_INDEX] == 15,
          "a default attribute is stored and the index advances");
    l.n = 0;
    cmd(&l, SNX_PICA_DEFAULT_ATTR_VALUE, 15, false, attr, 3);
    check(run(p, m, FCRAM, &l, &host) == SNX_CTR_PICA_OK && p->assembler.index == 1 &&
              p->immediate_vertex[1][1] == 2.0f,
          "the last attribute of an immediate vertex draws it into the primitive assembler");

    /* GPU::SubmitCmdList: a list outside memory raises P3D and runs nothing */
    interrupts = 0;
    check(snx_ctr_pica_run(p, m, 0x08000000u, 16, &host) == SNX_CTR_PICA_OK && interrupts == 1,
          "an invalid list raises P3D");
    snx_ctr_pica_free(p);
    free(p);
}

/* nihstro encodings: format 1 (opcode, dest 21..25, src1 12..18, src2 7..11, operand descriptor 0..6) */
static uint32_t op1(unsigned op, unsigned dest, unsigned src1, unsigned src2, unsigned desc) {
    return (uint32_t)op << 26 | dest << 21 | src1 << 12 | src2 << 7 | desc;
}
/* a swizzle pattern: all four components enabled, both sources unswizzled */
#define XYZW 0x6c36fu
#define END  (0x22u << 26)

static void shaders(void) {
    SnxCtrShaderSetup* s = calloc(1, sizeof *s);
    SnxCtrShaderUnit u;
    memset(&u, 0, sizeof u);
    s->swizzle[0] = XYZW;
    s->swizzle[1] = 0x6c368u | (1u << 4);                           /* x only, src1 negated */
    s->f[0][0] = 2, s->f[0][1] = 3, s->f[0][2] = 4, s->f[0][3] = 5; /* c0 */
    s->code[0] = op1(0x08, 0x10, 0x20, 0x00, 0);                    /* mul r0, c0, v0 */
    s->code[1] = op1(0x00, 0x00, 0x10, 0x00, 0);                    /* add o0, r0, v0 */
    s->code[2] = op1(0x02, 0x01, 0x10, 0x00, 0);                    /* dp4 o1, r0, v0 */
    s->code[3] = op1(0x13, 0x02, 0x00, 0x00, 1);                    /* mov o2.x, -v0 */
    s->code[4] = END;
    u.input[0][0] = 1, u.input[0][1] = 1, u.input[0][2] = 1, u.input[0][3] = 2;
    snx_ctr_shader_run(s, &u, 0, NULL);
    check(u.temporary[0][0] == 2 && u.temporary[0][3] == 10, "mul with a float uniform");
    check(u.output[0][0][0] == 3 && u.output[0][0][1] == 4 && u.output[0][0][2] == 5 && u.output[0][0][3] == 12, "add");
    check(u.output[0][1][0] == 29 && u.output[0][1][3] == 29, "dp4 writes the dot product to every component");
    check(u.output[0][2][0] == -1 && u.output[0][2][1] == 0, "a negated source, one destination component");

    /* LOOP over i0 = (count 2, start 0, step 1): three passes add 1 to r1 each */
    memset(&u, 0, sizeof u);
    memset(s->code, 0, sizeof s->code);
    s->f[1][0] = s->f[1][1] = s->f[1][2] = s->f[1][3] = 1; /* c1 */
    s->i[0][0] = 2, s->i[0][1] = 0, s->i[0][2] = 1;
    s->code[0] = (0x29u << 26) | (1u << 10);     /* loop i0, end at 1 */
    s->code[1] = op1(0x00, 0x11, 0x21, 0x11, 0); /* add r1, c1, r1 */
    s->code[2] = END;
    snx_ctr_shader_run(s, &u, 0, NULL);
    check(u.temporary[1][0] == 3 && u.address[2] == 3, "a loop runs count + 1 times and steps aL");

    /* IFU on b0: the else branch when it is clear */
    memset(&u, 0, sizeof u);
    memset(s->code, 0, sizeof s->code);
    s->b = 0;
    s->code[0] = (0x27u << 26) | (2u << 10) | 1u; /* ifu b0, else at 2, 1 instruction after */
    s->code[1] = op1(0x13, 0x12, 0x21, 0x00, 0);  /* mov r2, c1 (then) */
    s->swizzle[2] = XYZW | 1u << 4;
    s->code[2] = op1(0x13, 0x12, 0x21, 0x00, 2); /* mov r2, -c1 (else) */
    s->code[3] = END;
    snx_ctr_shader_run(s, &u, 0, NULL);
    check(u.temporary[2][0] == -1, "ifu takes the else branch with its bool clear");
    s->b = 1;
    memset(&u, 0, sizeof u);
    snx_ctr_shader_run(s, &u, 0, NULL);
    check(u.temporary[2][0] == 1, "ifu takes the then branch and skips the else one");
    free(s);
}

static void texels(SnxCtrMemory* m) {
    /* an 8x8 I8 texture: texel (x, y) in memory order holds its Morton index */
    unsigned char tile[64];
    uint8_t c[4];
    for (unsigned i = 0; i < 64; ++i) tile[i] = (unsigned char)i;
    check(snx_ctr_phys_write(m, FCRAM + 0x8000, tile, 64) == SNX_CTR_MEM_OK, "the texture is written");
    check(snx_ctr_texel(m, FCRAM + 0x8000, 8, SNX_CTR_TEX_I8, 3, 2, c) && c[0] == 13 && c[2] == 13 && c[3] == 255,
          "I8 texels are Z-ordered in their tile");
    /* (7, 6) is texel 61, the high nibble of byte 30 (0x1e) */
    check(snx_ctr_texel(m, FCRAM + 0x8000, 8, SNX_CTR_TEX_A4, 7, 6, c) && c[0] == 0 && c[3] == 0x11,
          "A4 reads the high nibble of odd texels");
    check(snx_ctr_texel(m, FCRAM + 0x8000, 8, SNX_CTR_TEX_IA4, 2, 0, c) && c[0] == 0x00 && c[3] == 0x44,
          "IA4: intensity high, alpha low");
    /* ETC1, individual mode: both halves 4-bit (8, 4, 2) -> (0x88, 0x44, 0x22), table 0, all modifiers +2 */
    uint64_t block = (uint64_t)0x88 << 56 | (uint64_t)0x44 << 48 | (uint64_t)0x22 << 40;
    snx_ctr_etc1(block, 1, 1, c);
    check(c[0] == 0x8a && c[1] == 0x46 && c[2] == 0x24, "ETC1 individual mode with its smallest modifier");
    snx_ctr_etc1(block | 1u << (16 + 5) | 1u << 5, 1, 1, c); /* texel 4 * 1 + 1: large modifier, negated */
    check(c[0] == 0x80 && c[1] == 0x3c && c[2] == 0x1a, "ETC1 negates and selects the large modifier per texel");
}

/* a full-viewport triangle (clipped down to the square), coloured by its vertices through TEV stage 0 */
static void render(SnxCtrMemory* m) {
    enum { FB = FCRAM + 0x20000, VERTICES = FCRAM + 0x10000, TEXTURE = FCRAM + 0x30000, W = 16, H = 16 };
    SnxCtrPica* p = calloc(1, sizeof *p);
    unsigned interrupts = 0;
    SnxCtrPicaHost host = {&interrupts, on_interrupt, snx_ctr_pica_draw, snx_ctr_pica_draw_immediate, NULL};
    List l = {{0}, 0};
    /* position, colour, texture coordinate */
    float vertices[3][10] = {{-1, -1, -0.5f, 1, 1, 0.6f, 0.25f, 1, 0, 0},
                             {3, -1, -0.5f, 1, 1, 0.6f, 0.25f, 1, 2, 0},
                             {-1, 3, -0.5f, 1, 1, 0.6f, 0.25f, 1, 0, 2}};
    const uint32_t program[] = {op1(0x13, 0, 0, 0, 0), op1(0x13, 1, 1, 0, 0), op1(0x13, 2, 2, 0, 0), END};
    unsigned char fb[W * H * 4], texture[64];
    memset(fb, 0xee, sizeof fb);
    memset(texture, 0x80, sizeof texture);
    check(snx_ctr_phys_write(m, VERTICES, vertices, sizeof vertices) == SNX_CTR_MEM_OK &&
              snx_ctr_phys_write(m, FB, fb, sizeof fb) == SNX_CTR_MEM_OK &&
              snx_ctr_phys_write(m, TEXTURE, texture, sizeof texture) == SNX_CTR_MEM_OK,
          "the draw's buffers are written");
    snx_ctr_pica_init(p);
    put(&l, SNX_PICA_VS + SNX_PICA_SH_PROGRAM_OFFSET, 0);
    cmd(&l, SNX_PICA_VS + SNX_PICA_SH_PROGRAM_WORD, 15, false, program, 4);
    put(&l, SNX_PICA_VS + SNX_PICA_SH_SWIZZLE_OFFSET, 0);
    put(&l, SNX_PICA_VS + SNX_PICA_SH_SWIZZLE_WORD, XYZW);
    put(&l, SNX_PICA_VS + SNX_PICA_SH_INPUT_CONFIG, 2 | 0xa0u << 24); /* three input attributes */
    put(&l, SNX_PICA_VS + SNX_PICA_SH_ATTR_MAP_LOW, 0x210);
    put(&l, SNX_PICA_VS + SNX_PICA_SH_OUTPUT_MASK, 7);
    put(&l, 0x04f, 3);           /* vs_output_total */
    put(&l, 0x050, 0x03020100u); /* position */
    put(&l, 0x051, 0x0b0a0908u); /* colour */
    put(&l, 0x052, 0x1f1f0d0cu); /* texture coordinate 0 */
    put(&l, 0x041, 0x420000u);   /* viewport half width and height 8.0 */
    put(&l, 0x043, 0x420000u);
    put(&l, 0x1c6, 1); /* lighting off */
    for (unsigned s = 1; s < 6; ++s) put(&l, s < 4 ? 0x0c0 + s * 8 : 0x0f0 + (s - 4) * 8, 0x000f000fu); /* previous */
    /* framebuffer: RGBA8, 16x16, colour writes on, no depth */
    put(&l, 0x11d, FB / 8);
    put(&l, 0x117, 0);
    put(&l, 0x11e, W | (H - 1) << 12);
    put(&l, 0x113, 15);
    put(&l, 0x107, 0xf00);
    put(&l, 0x102, 3); /* logic op copy, blending off */
    /* vertex arrays: three float attributes of 4, 4 and 2 elements in one loader of 40-byte vertices */
    put(&l, 0x200, VERTICES >> 3);
    put(&l, 0x201, 0x7ff);
    put(&l, 0x202, 2u << 28);
    put(&l, 0x203, 0);
    put(&l, 0x204, 0x210);
    put(&l, 0x205, 3u << 28 | 40u << 16);
    put(&l, 0x228, 3);
    put(&l, SNX_PICA_TRIGGER_DRAW, 1);
    check(run(p, m, FCRAM, &l, &host) == SNX_CTR_PICA_OK, "a vertex-coloured draw runs");
    check(snx_ctr_phys_read(m, FB, fb, sizeof fb) == SNX_CTR_MEM_OK, "the framebuffer is read back");
    unsigned wrong = 0;
    for (unsigned i = 0; i < W * H; ++i) /* RGBA8 is stored a, b, g, r */
        wrong += !(fb[i * 4] == 255 && fb[i * 4 + 1] == 64 && fb[i * 4 + 2] == 153 && fb[i * 4 + 3] == 255);
    if (wrong)
        fprintf(stderr, "%u wrong, pixel 0: %02x %02x %02x %02x, pixel 255: %02x %02x %02x %02x\n", wrong, fb[0], fb[1],
                fb[2], fb[3], fb[1020], fb[1021], fb[1022], fb[1023]);
    check(!wrong, "every pixel gets the interpolated vertex colour");

    /* the same triangle sampling a uniform I8 texture: TEV stage 0 takes texture 0's colour */
    l.n = 0;
    put(&l, 0x080, 1);            /* texture 0 on */
    put(&l, 0x082, 8 | 8u << 16); /* 8x8 */
    put(&l, 0x083, 0);            /* 2D, clamp to edge */
    put(&l, 0x085, TEXTURE / 8);
    put(&l, 0x08e, SNX_CTR_TEX_I8);
    put(&l, 0x0c0, 3); /* colour from texture 0, alpha from the vertex */
    put(&l, SNX_PICA_TRIGGER_DRAW, 1);
    check(run(p, m, FCRAM, &l, &host) == SNX_CTR_PICA_OK, "a textured draw runs");
    check(snx_ctr_phys_read(m, FB, fb, sizeof fb) == SNX_CTR_MEM_OK, "the framebuffer is read back");
    wrong = 0;
    for (unsigned i = 0; i < W * H; ++i)
        wrong += !(fb[i * 4] == 255 && fb[i * 4 + 1] == 0x80 && fb[i * 4 + 2] == 0x80 && fb[i * 4 + 3] == 0x80);
    check(!wrong, "every pixel gets the texture's intensity");

    /* blending: zero times the source plus one times the destination keeps the framebuffer */
    l.n = 0;
    put(&l, 0x0c0, 0);
    put(&l, 0x100, 1u << 8);             /* blending on */
    put(&l, 0x101, 1u << 20 | 1u << 28); /* add, source factors zero, destination factors one */
    put(&l, SNX_PICA_TRIGGER_DRAW, 1);
    check(run(p, m, FCRAM, &l, &host) == SNX_CTR_PICA_OK, "a blended draw runs");
    check(snx_ctr_phys_read(m, FB, fb, sizeof fb) == SNX_CTR_MEM_OK, "the framebuffer is read back");
    wrong = 0;
    for (unsigned i = 0; i < W * H; ++i) wrong += fb[i * 4 + 3] != 0x80;
    check(!wrong, "zero times the source plus the destination keeps the framebuffer");

    /* fragment lighting: one directional light along the normal the unmapped quaternion (all ones) rotates +z to,
       a diffuse colour past 1 in red and blue, the LUTs and attenuations off, TEV taking the primary fragment colour */
    l.n = 0;
    put(&l, 0x100, 0);                       /* blending off */
    put(&l, 0x140 + 2, 1023u << 20 | 1023u); /* light 0 diffuse */
    put(&l, 0x140 + 4, 0x3c00);              /* x = 1.0 (float16) */
    put(&l, 0x140 + 9, 1);                   /* directional */
    put(&l, 0x1c4, 0xff7bff00u);             /* LUTs, spot and distance attenuation off */
    put(&l, 0x1c6, 0);                       /* lighting on */
    put(&l, 0x0c0, 1);
    put(&l, SNX_PICA_TRIGGER_DRAW, 1);
    check(run(p, m, FCRAM, &l, &host) == SNX_CTR_PICA_OK, "a lit draw runs");
    check(snx_ctr_phys_read(m, FB, fb, sizeof fb) == SNX_CTR_MEM_OK, "the framebuffer is read back");
    wrong = 0;
    for (unsigned i = 0; i < W * H; ++i)
        wrong += !(fb[i * 4] == 255 && fb[i * 4 + 1] == 255 && fb[i * 4 + 2] == 0 && fb[i * 4 + 3] == 255);
    check(!wrong, "every pixel gets the saturated diffuse colour");
    snx_ctr_pica_free(p);
    free(p);
}

#define EMIT        (0x2au << 26)
#define SETEMIT(vertex, prim, winding) (0x2bu << 26 | (vertex) << 24 | (prim) << 23 | (winding) << 22)

/* a geometry shader in point mode turns one vertex into the full-viewport triangle of render(): it adds a corner
   offset (c0 to c2) to the position and passes the colour and texture coordinate through, one EMIT for each corner,
   the last completing the primitive */
static void geometry(SnxCtrMemory* m) {
    enum { FB = FCRAM + 0x20000, VERTICES = FCRAM + 0x10000, W = 16, H = 16 };
    SnxCtrPica* p = calloc(1, sizeof *p);
    unsigned interrupts = 0;
    SnxCtrPicaHost host = {&interrupts, on_interrupt, snx_ctr_pica_draw, snx_ctr_pica_draw_immediate, NULL};
    List l = {{0}, 0};
    float vertex[10] = {0, 0, -0.5f, 1, 1, 0.6f, 0.25f, 1, 0, 0};
    const uint32_t vs_program[] = {op1(0x13, 0, 0, 0, 0), op1(0x13, 1, 1, 0, 0), op1(0x13, 2, 2, 0, 0), END};
    const uint32_t gs_program[] = {
        SETEMIT(0, 0, 0), op1(0x00, 0, 0x20, 0, 0), op1(0x13, 1, 1, 0, 0), op1(0x13, 2, 2, 0, 0), EMIT,
        SETEMIT(1, 0, 0), op1(0x00, 0, 0x21, 0, 0), op1(0x13, 1, 1, 0, 0), op1(0x13, 2, 2, 0, 0), EMIT,
        SETEMIT(2, 1, 0), op1(0x00, 0, 0x22, 0, 0), op1(0x13, 1, 1, 0, 0), op1(0x13, 2, 2, 0, 0), EMIT, END};
    unsigned char fb[W * H * 4];
    memset(fb, 0xee, sizeof fb);
    check(snx_ctr_phys_write(m, VERTICES, vertex, sizeof vertex) == SNX_CTR_MEM_OK &&
              snx_ctr_phys_write(m, FB, fb, sizeof fb) == SNX_CTR_MEM_OK,
          "the geometry draw's buffers are written");
    snx_ctr_pica_init(p);
    p->gs.f[0][0] = -1, p->gs.f[0][1] = -1;
    p->gs.f[1][0] = 3, p->gs.f[1][1] = -1;
    p->gs.f[2][0] = -1, p->gs.f[2][1] = 3;
    put(&l, SNX_PICA_GS_EXCLUSIVE, 1);
    put(&l, SNX_PICA_USE_GS, 2);
    put(&l, SNX_PICA_VS + SNX_PICA_SH_PROGRAM_OFFSET, 0);
    cmd(&l, SNX_PICA_VS + SNX_PICA_SH_PROGRAM_WORD, 15, false, vs_program, 4);
    put(&l, SNX_PICA_VS + SNX_PICA_SH_SWIZZLE_OFFSET, 0);
    put(&l, SNX_PICA_VS + SNX_PICA_SH_SWIZZLE_WORD, XYZW);
    put(&l, SNX_PICA_VS + SNX_PICA_SH_INPUT_CONFIG, 2 | 0xa0u << 24);
    put(&l, SNX_PICA_VS + SNX_PICA_SH_ATTR_MAP_LOW, 0x210);
    put(&l, SNX_PICA_VS + SNX_PICA_SH_OUTPUT_MASK, 7);
    put(&l, SNX_PICA_GS + SNX_PICA_SH_PROGRAM_OFFSET, 0);
    cmd(&l, SNX_PICA_GS + SNX_PICA_SH_PROGRAM_WORD, 15, false, gs_program, 15);
    put(&l, SNX_PICA_GS + SNX_PICA_SH_SWIZZLE_OFFSET, 0);
    put(&l, SNX_PICA_GS + SNX_PICA_SH_SWIZZLE_WORD, XYZW);
    put(&l, SNX_PICA_GS + SNX_PICA_SH_INPUT_CONFIG, 2 | 0x08u << 24); /* three inputs: one vertex of three outputs */
    put(&l, SNX_PICA_GS + SNX_PICA_SH_ATTR_MAP_LOW, 0x210);
    put(&l, SNX_PICA_GS + SNX_PICA_SH_OUTPUT_MASK, 7);
    put(&l, 0x24a, 2); /* vertex outputs minus one, counted twice */
    put(&l, 0x251, 2);
    put(&l, 0x252, 0); /* point mode */
    put(&l, SNX_PICA_TOPOLOGY, 3u << 8); /* the geometry shader's primitives */
    put(&l, 0x04f, 3);
    put(&l, 0x050, 0x03020100u);
    put(&l, 0x051, 0x0b0a0908u);
    put(&l, 0x052, 0x1f1f0d0cu);
    put(&l, 0x041, 0x420000u);
    put(&l, 0x043, 0x420000u);
    put(&l, 0x1c6, 1);
    for (unsigned s = 1; s < 6; ++s) put(&l, s < 4 ? 0x0c0 + s * 8 : 0x0f0 + (s - 4) * 8, 0x000f000fu);
    put(&l, 0x11d, FB / 8);
    put(&l, 0x117, 0);
    put(&l, 0x11e, W | (H - 1) << 12);
    put(&l, 0x113, 15);
    put(&l, 0x107, 0xf00);
    put(&l, 0x102, 3);
    put(&l, 0x200, VERTICES >> 3);
    put(&l, 0x201, 0x7ff);
    put(&l, 0x202, 2u << 28);
    put(&l, 0x203, 0);
    put(&l, 0x204, 0x210);
    put(&l, 0x205, 3u << 28 | 40u << 16);
    put(&l, 0x228, 1);
    put(&l, SNX_PICA_TRIGGER_DRAW, 1);
    check(run(p, m, FCRAM, &l, &host) == SNX_CTR_PICA_OK, "a draw through a geometry shader runs");
    check(snx_ctr_phys_read(m, FB, fb, sizeof fb) == SNX_CTR_MEM_OK, "the geometry draw's framebuffer is read back");
    unsigned wrong = 0;
    for (unsigned i = 0; i < W * H; ++i)
        wrong += !(fb[i * 4] == 255 && fb[i * 4 + 1] == 64 && fb[i * 4 + 2] == 153 && fb[i * 4 + 3] == 255);
    check(!wrong, "the triangle a geometry shader makes of one vertex covers the viewport with the vertex colour");
    check(p->gs.b >> 15 & 1, "b15 is set after the geometry shader runs");
    /* fixed-primitive mode: the vertex shader's outputs land in the uniforms from start_index (c3 to c5), and the same
       triangle comes out of a program that reads them there */
    const uint32_t fixed_program[] = {
        op1(0x13, 0x10, 0x23, 0, 0), /* mov r0, c3 */
        SETEMIT(0, 0, 0), op1(0x00, 0, 0x20, 0x10, 0), op1(0x13, 1, 0x24, 0, 0), op1(0x13, 2, 0x25, 0, 0), EMIT,
        SETEMIT(1, 0, 0), op1(0x00, 0, 0x21, 0x10, 0), op1(0x13, 1, 0x24, 0, 0), op1(0x13, 2, 0x25, 0, 0), EMIT,
        SETEMIT(2, 1, 0), op1(0x00, 0, 0x22, 0x10, 0), op1(0x13, 1, 0x24, 0, 0), op1(0x13, 2, 0x25, 0, 0), EMIT, END};
    memset(fb, 0xee, sizeof fb);
    check(snx_ctr_phys_write(m, FB, fb, sizeof fb) == SNX_CTR_MEM_OK, "the framebuffer is cleared");
    l.n = 0;
    put(&l, SNX_PICA_GS + SNX_PICA_SH_PROGRAM_OFFSET, 0);
    cmd(&l, SNX_PICA_GS + SNX_PICA_SH_PROGRAM_WORD, 15, false, fixed_program, 16);
    put(&l, SNX_PICA_GS + SNX_PICA_SH_INPUT_CONFIG, 2 | 1u << 8 | 0x08u << 24); /* input to uniforms */
    put(&l, 0x252, 2 | 0u << 8 | 2u << 12 | 3u << 16);                          /* fixed: 1 vertex, stride 3, from c3 */
    put(&l, SNX_PICA_TRIGGER_DRAW, 1);
    check(run(p, m, FCRAM, &l, &host) == SNX_CTR_PICA_OK, "a draw through a fixed-primitive geometry shader runs");
    check(snx_ctr_phys_read(m, FB, fb, sizeof fb) == SNX_CTR_MEM_OK, "the framebuffer is read back");
    wrong = 0;
    for (unsigned i = 0; i < W * H; ++i)
        wrong += !(fb[i * 4] == 255 && fb[i * 4 + 1] == 64 && fb[i * 4 + 2] == 153 && fb[i * 4 + 3] == 255);
    check(!wrong, "a fixed-primitive geometry shader reads the vertex from its uniforms");
    /* an unknown mode, or a layout the pipeline does not take, stops the draw with a reason */
    l.n = 0;
    put(&l, 0x252, 3);
    put(&l, SNX_PICA_TRIGGER_DRAW, 1);
    check(run(p, m, FCRAM, &l, &host) == SNX_CTR_PICA_UNSUPPORTED && p->stop_reason, "an unknown geometry mode stops");
    snx_ctr_pica_free(p);
    free(p);
}


int main(void) {
    SnxCtrMemory* m = snx_ctr_memory_new(SNX_CTR_OLD);
    if (!m) return 1;
    float24();
    writes(m);
    uploads(m);
    control(m);
    shaders();
    texels(m);
    render(m);
    geometry(m);
    snx_ctr_memory_free(m);
    if (failures) return 1;
    puts("3ds pica: ok");
    return 0;
}
