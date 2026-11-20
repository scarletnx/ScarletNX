/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The shared GL layer's shaders and programs (src/common/gpu/gl/gl_program.c), with no GPU: shaders made, given a
   source and compiled (a GLSL ES one, a desktop context's GLSL 1.20 and 3.30 core, one refused with its log and its
   line said), programs linked (glBindAttribLocation's location kept, the rest placed, the stages' outputs and inputs
   matched by name), the uniforms by GL's names with their locations, written and read back (a vector, a matrix
   transposed, an array from an element, a struct's members, a sampler's unit, a block's binding), GL's errors, what
   deletes when, a program pipeline's hidden program, the arena compacted under many sources, and a save state's copy
   loaded back whole and refused when a range, a location or a binding in it is wrong */
#include "common/gpu/gl/gl_program.h"
#include "glslc/glslc.h"
#include "common/gpu/gl/gl_state.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    VERTEX = 0x8b31,
    FRAGMENT = 0x8b30,
    COMPILE_STATUS = 0x8b81,
    DELETE_STATUS = 0x8b80,
    LINK_STATUS = 0x8b82,
    INFO_LOG_LENGTH = 0x8b84,
    SOURCE_LENGTH = 0x8b88,
    ACTIVE_UNIFORMS = 0x8b86,
    ACTIVE_ATTRIBUTES = 0x8b89,
    ACTIVE_UNIFORM_BLOCKS = 0x8a36,
    UNIFORM_BLOCK_BINDING = 0x8a3f,
    UNIFORM_BLOCK_DATA_SIZE = 0x8a40,
};

static char said[1024];
static unsigned changes, changed_last;
static void heard(const char* line) { snprintf(said, sizeof said, "%s", line); }
static void changed(uint32_t program) { changes++, changed_last = program; }

static uint32_t shader(uint32_t ctx, uint32_t type, const char* text) {
    uint32_t s = snx_gl_create_shader(ctx, type);
    char* to;
    assert(s);
    to = snx_gl_shader_source(ctx, s, strlen(text));
    assert(to);
    memcpy(to, text, strlen(text));
    snx_gl_compile_shader(ctx, s);
    return s;
}
static int32_t iv(uint32_t ctx, uint32_t s, uint32_t pname) {
    int32_t v = -99;
    assert(snx_gl_get_shaderiv(ctx, s, pname, &v));
    return v;
}
static int32_t piv(uint32_t ctx, uint32_t p, uint32_t pname) {
    int32_t v[3] = {-99, -99, -99};
    assert(snx_gl_get_programiv(ctx, p, pname, v) == 1);
    return v[0];
}
static uint32_t program(uint32_t ctx, uint32_t vs, uint32_t fs) {
    uint32_t p = snx_gl_create_program(ctx);
    assert(p);
    snx_gl_attach_shader(ctx, p, vs);
    snx_gl_attach_shader(ctx, p, fs);
    snx_gl_link_program(ctx, p);
    return p;
}
static void refused(const void* copy, size_t size, const char* text) {
    char why[256];
    assert(snx_gl_state_check(copy, size, why, sizeof why) < 0);
    if (!strstr(why, text)) {
        fprintf(stderr, "gl_program: refused for \"%s\", not naming \"%s\"\n", why, text);
        abort();
    }
}

static const char es_vs[] = "#version 300 es\n"
                            "in vec4 a_pos;\n"
                            "in vec2 a_uv;\n"
                            "uniform mat4 u_mvp;\n"
                            "out vec2 v_uv;\n"
                            "void main() { v_uv = a_uv; gl_Position = u_mvp * a_pos; }\n";
static const char es_fs[] = "#version 300 es\n"
                            "precision mediump float;\n"
                            "struct S { vec2 a; float b; };\n"
                            "uniform sampler2D u_tex;\n"
                            "uniform vec4 u_tint;\n"
                            "uniform float u_k[3];\n"
                            "uniform S u_s;\n"
                            "layout(std140) uniform Light { vec4 color; vec4 dir; } light;\n"
                            "in vec2 v_uv;\n"
                            "out vec4 o;\n"
                            "void main() { o = texture(u_tex, v_uv) * u_tint * (u_k[0] + u_k[2] + u_s.a.x + u_s.b)"
                            " + light.color; }\n";

int main(void) {
    uint32_t es = snx_gl_context_new(SNX_GL_API_ES, 30, 0), desk = snx_gl_context_new(SNX_GL_API_DESKTOP, 45, 1);
    assert(es && desk);
    snx_gl_program_host(&(SnxGlProgramHost){.program_changed = changed, .say = heard});

    /* shaders: a type GL has not, a GLSL ES pair, a refused one with its log and the line said */
    assert(!snx_gl_create_shader(es, 0x1234) && snx_gl_get_error(es) == SNX_GL_INVALID_ENUM);
    uint32_t vs = shader(es, VERTEX, es_vs), fs = shader(es, FRAGMENT, es_fs);
    assert(vs == 1 && fs == 2 && snx_gl_is_shader(vs) && !snx_gl_is_program(vs));
    assert(iv(es, vs, COMPILE_STATUS) == 1 && iv(es, fs, COMPILE_STATUS) == 1 && iv(es, vs, INFO_LOG_LENGTH) == 0);
    assert(iv(es, vs, SOURCE_LENGTH) == (int32_t)sizeof es_vs && !strcmp(snx_gl_shader_text(es, vs), es_vs));
    uint32_t bad = shader(es, FRAGMENT, "#version 300 es\nprecision mediump float;\nvoid main() { nothing; }\n");
    assert(iv(es, bad, COMPILE_STATUS) == 0 && iv(es, bad, INFO_LOG_LENGTH) > 0);
    assert(!strncmp(snx_gl_shader_info_log(es, bad), "ERROR: 3:", 9));
    assert(!strncmp(said, "a shader does not compile: ERROR: 3:", 36) && !strchr(said, '\n'));
    assert(!snx_gl_get_shaderiv(es, 77, COMPILE_STATUS, &(int32_t){0}) && snx_gl_get_error(es) == SNX_GL_INVALID_VALUE);

    /* a program: glBindAttribLocation's location kept, the others placed; the uniforms by GL's names */
    uint32_t p = snx_gl_create_program(es);
    snx_gl_bind_attrib_location(es, p, 5, "a_uv");
    snx_gl_attach_shader(es, p, vs);
    snx_gl_attach_shader(es, p, fs);
    snx_gl_attach_shader(es, p, bad);
    assert(snx_gl_get_error(es) == SNX_GL_INVALID_OPERATION); /* a second fragment shader */
    snx_gl_link_program(es, p);
    assert(piv(es, p, LINK_STATUS) == 1 && piv(es, p, ACTIVE_ATTRIBUTES) == 2 && piv(es, p, INFO_LOG_LENGTH) == 0);
    assert(snx_gl_attrib_location(es, p, "a_uv") == 5 && snx_gl_attrib_location(es, p, "a_pos") == 0);
    assert(snx_gl_attrib_location(es, p, "a_none") == -1);
    int32_t mvp = snx_gl_uniform_location(es, p, "u_mvp"), tint = snx_gl_uniform_location(es, p, "u_tint");
    int32_t k = snx_gl_uniform_location(es, p, "u_k"), tex = snx_gl_uniform_location(es, p, "u_tex");
    int32_t sa = snx_gl_uniform_location(es, p, "u_s.a"), sb = snx_gl_uniform_location(es, p, "u_s.b");
    assert(mvp >= 0 && tint >= 0 && k >= 0 && tex >= 0 && sa >= 0 && sb >= 0);
    assert(snx_gl_uniform_location(es, p, "u_k[0]") == k && snx_gl_uniform_location(es, p, "u_k[2]") == k + 2);
    assert(snx_gl_uniform_location(es, p, "u_k[3]") == -1 && snx_gl_uniform_location(es, p, "u_none") == -1);
    assert(piv(es, p, ACTIVE_UNIFORMS) == 8); /* u_mvp, u_tint, u_k[0], u_s.a, u_s.b, the block's two, u_tex */
    {
        int32_t count;
        uint32_t type;
        const char* name = snx_gl_active(es, p, 0, 1, &count, &type);
        assert(name && !strcmp(name, "u_mvp") && count == 1 && type == 0x8b5c); /* GL_FLOAT_MAT4 */
    }

    /* the uniforms written and read back, on the program in use */
    snx_gl_use_program(es, p);
    assert(snx_gl_current_program(es) == p);
    {
        const SnxGlProgram* pr = snx_gl_program(p);
        uint32_t serial = pr->default_serial[1];
        const float rgba[4] = {0.25f, 0.5f, 0.75f, 1}, ks[2] = {7, 8}, s_a[2] = {3, 4};
        float out[16];
        int32_t unit = 3, got;
        snx_gl_uniform(es, tint, 1, rgba, 4, 'f', 0);
        snx_gl_get_uniform(es, p, tint, 'f', out);
        assert(!memcmp(out, rgba, sizeof rgba) && pr->default_serial[1] == serial + 1);
        snx_gl_uniform(es, k + 1, 2, ks, 1, 'f', 0); /* u_k[1] and u_k[2] */
        snx_gl_get_uniform(es, p, k + 2, 'f', out);
        assert(out[0] == 8);
        snx_gl_get_uniform(es, p, k, 'f', out);
        assert(out[0] == 0);
        snx_gl_uniform(es, sa, 1, s_a, 2, 'f', 0);
        snx_gl_get_uniform(es, p, sa, 'f', out);
        assert(out[0] == 3 && out[1] == 4);
        snx_gl_uniform(es, tex, 1, &unit, 1, 'i', 0);
        snx_gl_get_uniform(es, p, tex, 'i', &got);
        assert(got == 3);
        snx_gl_uniform(es, tint, 1, ks, 1, 'f', 0); /* a float into a vec4: GL_INVALID_OPERATION, nothing written */
        assert(snx_gl_get_error(es) == SNX_GL_INVALID_OPERATION);
        snx_gl_get_uniform(es, p, tint, 'f', out);
        assert(out[0] == 0.25f);
        snx_gl_uniform(es, -1, 1, rgba, 4, 'f', 0); /* location -1: nothing, no error */
        assert(!snx_gl_get_error(es));
        /* a matrix given by rows (transpose) reads back by columns */
        float rows[16], cols[16];
        for (int i = 0; i < 16; i++) rows[i] = (float)i;
        snx_gl_uniform(es, mvp, 1, rows, 16, 'f', 1);
        snx_gl_get_uniform(es, p, mvp, 'f', cols);
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) assert(cols[c * 4 + r] == rows[r * 4 + c]);
        snx_gl_program_uniform(es, 0, tint, 1, rgba, 4, 'f', 0); /* program 0 is no program */
        assert(snx_gl_get_error(es) == SNX_GL_INVALID_OPERATION);
    }
    /* the block: its index, size and binding point */
    {
        int32_t v[2];
        uint32_t light = snx_gl_uniform_block_index(es, p, "Light");
        assert(light == 0 && piv(es, p, ACTIVE_UNIFORM_BLOCKS) == 1);
        assert(snx_gl_active_uniform_blockiv(es, p, light, UNIFORM_BLOCK_DATA_SIZE, v, 2) == 1 && v[0] == 32);
        snx_gl_uniform_block_binding(es, p, light, 7);
        assert(snx_gl_active_uniform_blockiv(es, p, light, UNIFORM_BLOCK_BINDING, v, 2) == 1 && v[0] == 7);
        snx_gl_uniform_block_binding(es, p, light, 99);
        assert(snx_gl_get_error(es) == SNX_GL_INVALID_VALUE);
    }

    /* a program without a fragment shader does not link: its log says why, and so does the platform */
    uint32_t lone = snx_gl_create_program(es);
    snx_gl_attach_shader(es, lone, vs);
    snx_gl_link_program(es, lone);
    assert(piv(es, lone, LINK_STATUS) == 0 && !strcmp(snx_gl_program_info_log(es, lone), "a program needs a vertex and "
                                                                                         "a fragment shader"));
    assert(!strcmp(said, "a program does not link: a program needs a vertex and a fragment shader"));
    snx_gl_use_program(es, lone);
    assert(snx_gl_get_error(es) == SNX_GL_INVALID_OPERATION && snx_gl_current_program(es) == p);

    /* a desktop context's GLSL 1.20 (the f suffix, a precision statement) and 3.30 core; 1.20 refused in GLSL ES */
    {
        static const char v120[] = "#version 120\nattribute vec4 p;\nuniform mat4 m;\nvarying vec2 t;\n"
                                   "void main() { t = p.xy * 0.5f; gl_Position = m * p; }\n";
        static const char f120[] = "#version 120\nprecision mediump float;\nuniform sampler2D s;\nvarying vec2 t;\n"
                                   "void main() { gl_FragColor = texture2D(s, t) * 2.0f; }\n";
        static const char v330[] = "#version 330 core\nlayout(location = 0) in vec4 p;\nout vec2 t;\n"
                                   "void main() { t = p.xy; gl_Position = p; }\n";
        static const char f330[] = "#version 330 core\nuniform sampler3D v;\nin vec2 t;\nout vec4 o;\n"
                                   "void main() { o = texture(v, t.xyx); }\n";
        uint32_t a = program(desk, shader(desk, VERTEX, v120), shader(desk, FRAGMENT, f120));
        uint32_t b = program(desk, shader(desk, VERTEX, v330), shader(desk, FRAGMENT, f330));
        assert(piv(desk, a, LINK_STATUS) == 1 && piv(desk, b, LINK_STATUS) == 1);
        assert(snx_gl_uniform_location(desk, a, "m") >= 0 && snx_gl_uniform_location(desk, b, "v") >= 0);
        uint32_t no = shader(es, VERTEX, v120);
        assert(iv(es, no, COMPILE_STATUS) == 0 && strstr(snx_gl_shader_info_log(es, no), "is not a GLSL ES version"));
        snx_gl_delete_program(desk, a);
        snx_gl_delete_program(desk, b);
        snx_gl_delete_shader(es, no);
    }

    /* deletes: an attached shader waits for its program; the program in use waits for the next glUseProgram */
    snx_gl_delete_shader(es, vs);
    assert(snx_gl_is_shader(vs) && iv(es, vs, DELETE_STATUS) == 1);
    snx_gl_delete_program(es, p);
    assert(snx_gl_is_program(p) && piv(es, p, DELETE_STATUS) == 1);

    /* a program pipeline of two separable programs: the draw links a hidden program of their stages once */
    {
        uint32_t pv = snx_gl_create_shader(es, VERTEX), pf = snx_gl_create_shader(es, FRAGMENT), pipe = 0;
        memcpy(snx_gl_shader_source(es, pv, strlen(es_vs)), es_vs, strlen(es_vs));
        memcpy(snx_gl_shader_source(es, pf, strlen(es_fs)), es_fs, strlen(es_fs));
        uint32_t sv = snx_gl_create_shader_program(es, pv), sf = snx_gl_create_shader_program(es, pf);
        assert(piv(es, sv, LINK_STATUS) == 1 && piv(es, sf, LINK_STATUS) == 1 && piv(es, sv, 0x8258) == 1);
        snx_gl_gen_pipelines(es, 1, &pipe);
        assert(pipe == 1 && snx_gl_is_pipeline(pipe));
        snx_gl_use_program_stages(es, pipe, 1, sv);
        snx_gl_use_program_stages(es, pipe, 2, sf);
        snx_gl_use_program_stages(es, pipe, 2, lone); /* not linked */
        assert(snx_gl_get_error(es) == SNX_GL_INVALID_OPERATION);
        snx_gl_delete_program(es, lone);
        assert(!snx_gl_is_program(lone) && snx_gl_is_shader(vs)); /* still p's */
        snx_gl_use_program(es, 0);                                /* p goes now, and vs with it */
        assert(!snx_gl_is_program(p) && !snx_gl_is_shader(vs) && changed_last == p);
        snx_gl_bind_pipeline(es, pipe);
        uint32_t hidden = snx_gl_draw_program(es, 0);
        assert(hidden && snx_gl_program(hidden)->hidden && snx_gl_program(hidden)->linked);
        assert(snx_gl_draw_program(es, 0) == hidden);
        snx_gl_delete_pipelines(es, 1, &pipe);
        assert(!snx_gl_is_program(hidden) && !snx_gl_current_pipeline(es));
    }

    /* the arena: one shader's source set many times stays in a small arena, its text whole */
    {
        static char big[4096];
        uint32_t s = snx_gl_create_shader(es, FRAGMENT);
        for (int i = 0; i < 2000; i++) {
            memset(big, 'a' + i % 26, sizeof big - 1);
            memcpy(snx_gl_shader_source(es, s, sizeof big - 1), big, sizeof big - 1);
        }
        assert(!strcmp(snx_gl_shader_text(es, s), big));
        size_t records = 8 + SNX_GL_PROGRAM_NAMES * (sizeof(SnxGlShader) + sizeof(SnxGlProgram)) +
                         SNX_GL_PIPELINES * sizeof(SnxGlPipeline) + 8;
        assert(snx_gl_program_state_bytes() - records <= (64u << 10)); /* the arena kept at its first size */
        snx_gl_delete_shader(es, s);
    }

    /* a save state's copy: loaded back whole, every platform told its programs are stale */
    uint32_t q = program(es, shader(es, VERTEX, es_vs), shader(es, FRAGMENT, es_fs));
    int32_t qtint = snx_gl_uniform_location(es, q, "u_tint");
    const float green[4] = {0, 1, 0, 1};
    snx_gl_use_program(es, q);
    snx_gl_uniform(es, qtint, 1, green, 4, 'f', 0);
    size_t bytes = snx_gl_state_bytes();
    unsigned char *saved = malloc(bytes), *copy = malloc(bytes), *after = malloc(bytes);
    char why[256];
    assert(saved && copy && after);
    snx_gl_state_save(saved);
    assert(!snx_gl_state_check(saved, bytes, why, sizeof why));
    {
        const float red[4] = {1, 0, 0, 1};
        float out[4];
        unsigned before = changes;
        snx_gl_uniform(es, qtint, 1, red, 4, 'f', 0);
        snx_gl_use_program(es, 0);
        snx_gl_delete_program(es, q);
        assert(!snx_gl_is_program(q));
        assert(!snx_gl_state_load(saved, bytes));
        assert(changes - before >= SNX_GL_PROGRAM_NAMES);
        snx_gl_state_save(after);
        assert(!memcmp(saved, after, bytes));
        assert(snx_gl_is_program(q) && snx_gl_current_program(es) == q && piv(es, q, LINK_STATUS) == 1);
        snx_gl_get_uniform(es, q, qtint, 'f', out);
        assert(!memcmp(out, green, sizeof green));
    }
    /* refused copies, each naming what it found */
    {
        size_t records = 16 + (size_t)SNX_GL_CONTEXTS * sizeof(SnxGlContext) + 8; /* the programs' records */
        size_t at_program = records + SNX_GL_PROGRAM_NAMES * sizeof(SnxGlShader) + (q - 1) * sizeof(SnxGlProgram);
        size_t at_shader = records + (snx_gl_program(q)->shaders[0] - 1) * sizeof(SnxGlShader);
        SnxGlShader sh;
        SnxGlProgram pr;
        SnxGlContext c;
        memcpy(copy, saved, bytes);
        memcpy(&sh, copy + at_shader, sizeof sh);
        sh.source.at = 0x7ffffff0u;
        memcpy(copy + at_shader, &sh, sizeof sh);
        refused(copy, bytes, "has the range 2147483632+");
        memcpy(copy, saved, bytes);
        memcpy(&pr, copy + at_program, sizeof pr);
        pr.uniform_count = 100000;
        memcpy(copy + at_program, &pr, sizeof pr);
        refused(copy, bytes, "has its uniforms at");
        memcpy(copy, saved, bytes);
        memcpy(&pr, copy + at_program, sizeof pr);
        pr.shaders[1] = 1000;
        memcpy(copy + at_program, &pr, sizeof pr);
        refused(copy, bytes, "has shader 1000 attached, which is not a shader");
        memcpy(copy, saved, bytes);
        {
            int32_t e = 0x7fff0000; /* uniform 32767 */
            memcpy(&pr, copy + at_program, sizeof pr);
            memcpy(copy +
                       (records + SNX_GL_PROGRAM_NAMES * (sizeof(SnxGlShader) + sizeof(SnxGlProgram)) +
                        SNX_GL_PIPELINES * sizeof(SnxGlPipeline) + 8) +
                       pr.link.at + pr.locations,
                   &e, 4);
            refused(copy, bytes, "has location 0 at uniform 32767");
        }
        memcpy(copy, saved, bytes);
        memcpy(&c, copy + 16 + (es - 1) * sizeof c, sizeof c);
        c.program = 900;
        memcpy(copy + 16 + (es - 1) * sizeof c, &c, sizeof c);
        refused(copy, bytes, "has program 900 and pipeline 0 bound");
        refused(saved, bytes - 4, "this build reads");
    }
    free(saved);
    free(copy);
    free(after);

    /* a platform whose draws bind each stage's resources in its own set (the console's renderer): the default block
       and the blocks from block_base of each stage's set, the samplers from sampler_base, no flip constant; an image
       has no place, and the link says so, naming it */
    {
        static const char vs[] = "#version 300 es\nin vec4 p;\nuniform mat4 m;\nuniform B { vec4 shift; };\n"
                                 "void main() { gl_Position = m * p + shift; }\n";
        static const char fs[] = "#version 300 es\nprecision mediump float;\nuniform sampler2D a;\n"
                                 "uniform lowp sampler2DArray b;\nuniform vec4 tint;\nuniform B { vec4 shift; };\n"
                                 "out vec4 o;\nvoid main() { o = texture(a, gl_FragCoord.xy) + texture(b, vec3(0)) "
                                 "* tint + shift; }\n";
        static const char image_fs[] = "#version 310 es\nprecision mediump float;\n"
                                       "layout(rgba8, binding = 0) uniform readonly highp image2D im;\nout vec4 o;\n"
                                       "void main() { o = imageLoad(im, ivec2(0)); }\n";
        const SnxGlLayout layout = {.set = {0, 1, 0},
                                    .block_base = 0,
                                    .blocks = 17,
                                    .sampler_base = 34,
                                    .samplers = 64,
                                    .sampler_set = {0, 1, 0},
                                    .uniform_blocks = 17};
        uint32_t p, ip;
        const SnxGlProgram* pr;
        snx_gl_program_host(
            &(SnxGlProgramHost){.program_changed = changed, .say = heard, .stage_sets = 1, .layout = layout});
        p = program(es, shader(es, VERTEX, vs), shader(es, FRAGMENT, fs));
        assert(piv(es, p, LINK_STATUS));
        pr = snx_gl_program(p);
        const int32_t plain_uniforms = piv(es, p, ACTIVE_UNIFORMS);
        /* each stage's module: its sets and bindings, read from its decorations */
        for (int stage = 0; stage < 2; stage++) {
            const uint32_t* w = snx_gl_program_spirv(pr, stage);
            uint32_t words = pr->spirv_words[stage], sets = 0, found[64], nfound = 0;
            for (uint32_t i = 5; i < words; i += w[i] >> 16) {
                if ((w[i] & 0xffff) == 71 && w[i + 2] == 34) sets |= 1u << w[i + 3]; /* OpDecorate DescriptorSet */
                if ((w[i] & 0xffff) == 71 && w[i + 2] == 33 && nfound < 64) found[nfound++] = w[i + 3]; /* Binding */
                if (!(w[i] >> 16)) break;
            }
            assert(sets == (stage ? 2u : 1u));
            if (stage == 0)
                assert(nfound == 2 && ((found[0] == 0 && found[1] == 1) || (found[0] == 1 && found[1] == 0)));
            else {
                uint32_t mask = 0;
                for (uint32_t i = 0; i < nfound; i++) mask |= 1u << (found[i] < 32 ? found[i] : found[i] - 32 + 8);
                assert(nfound == 4 && mask == (1u | 2u | 1u << 10 | 1u << 11)); /* 0, 1, 34 and 35 */
            }
        }
        /* no flip constant: the fragment module has no push constant block */
        {
            const uint32_t* w = snx_gl_program_spirv(pr, 1);
            for (uint32_t i = 5; i < pr->spirv_words[1] && (w[i] >> 16); i += w[i] >> 16)
                assert(!((w[i] & 0xffff) == 59 && w[i + 3] == 9)); /* OpVariable PushConstant */
        }
        ip = program(es, shader(es, VERTEX, vs), shader(es, FRAGMENT, image_fs));
        assert(!piv(es, ip, LINK_STATUS));
        assert(strstr(snx_gl_program_info_log(es, ip), "im is an image: this platform's GL layer binds no images yet"));
        /* a renderer's rules: the samplers in sets of their own (5 and 6), blocks from 1 on read as read-only storage
           buffers, and the vertex and fragment stages' uniform ones through windows, their offsets pushed from bytes
           128 and 176; the uniforms still found by the link's reflection */
        {
            SnxGlLayout rules = layout;
            uint32_t bit = 0;
            rules.sampler_set[0] = 5, rules.sampler_set[1] = 6, rules.uniform_blocks = 1;
            rules.window_push[0] = 128, rules.window_push[1] = 176;
            assert(snx_gl_layout_block(&rules, 1, 0, &bit) == SNX_GLSLC_BLOCK_WINDOW && bit == 176 * 8);
            assert(snx_gl_layout_block(&rules, 1, 1, &bit) == SNX_GLSLC_BLOCK_STORAGE);
            rules.uniform_blocks = 17;
            assert(snx_gl_layout_block(&rules, 0, 3, &bit) == SNX_GLSLC_BLOCK_WINDOW && bit == 128 * 8 + 32 + 16);
            assert(snx_gl_layout_block(&rules, 2, 3, &bit) == SNX_GLSLC_BLOCK_UNIFORM); /* compute: no window */
            rules.uniform_blocks = 1;
            snx_gl_program_host(
                &(SnxGlProgramHost){.program_changed = changed, .say = heard, .stage_sets = 1, .layout = rules});
            p = program(es, shader(es, VERTEX, vs), shader(es, FRAGMENT, fs));
            assert(piv(es, p, LINK_STATUS));
            pr = snx_gl_program(p);
            assert(snx_gl_uniform_location(es, p, "tint") >= 0 && snx_gl_uniform_location(es, p, "m") >= 0);
            assert(snx_gl_uniform_block_index(es, p, "B") == 0);
            assert(piv(es, p, ACTIVE_UNIFORMS) == plain_uniforms); /* a window's variable adds none */
            /* the storage block's member and the window-read default block's, each once, at its own offset */
            {
                int32_t block = -2, offset = -1;
                uint32_t shift = snx_gl_uniform_index(es, p, "shift"), tint = snx_gl_uniform_index(es, p, "tint");
                assert(shift != 0xffffffffu && tint != 0xffffffffu);
                assert(snx_gl_active_uniformiv(es, p, shift, 0x8a3a, &block) &&
                       block == 0); /* GL_UNIFORM_BLOCK_INDEX */
                assert(snx_gl_active_uniformiv(es, p, tint, 0x8a3a, &block) && block == -1);
                assert(snx_gl_active_uniformiv(es, p, shift, 0x8a3b, &offset) && offset == 0); /* GL_UNIFORM_OFFSET */
                assert(snx_gl_uniform_index(es, p, "") == 0xffffffffu);
            }
            for (int stage = 0; stage < 2; stage++) {
                const uint32_t* w = snx_gl_program_spirv(pr, stage);
                uint32_t sets = 0, push = 0, storage = 0, offset = 0;
                for (uint32_t i = 5; i < pr->spirv_words[stage] && (w[i] >> 16); i += w[i] >> 16) {
                    if ((w[i] & 0xffff) == 71 && w[i + 2] == 34) sets |= 1u << w[i + 3]; /* DescriptorSet */
                    if ((w[i] & 0xffff) == 59 && w[i + 3] == 9) push++;                  /* OpVariable PushConstant */
                    if ((w[i] & 0xffff) == 59 && w[i + 3] == 12) storage++;              /* StorageBuffer */
                    if ((w[i] & 0xffff) == 72 && w[i + 3] == 35 && w[i + 4] >= 128) offset = w[i + 4];
                }
                assert(push == 1 && storage == 1 && offset == (stage ? 176u : 128u));
                assert(sets == (stage ? (1u << 1 | 1u << 6) : 1u)); /* the vertex stage has no sampler */
            }
        }
        snx_gl_program_host(&(SnxGlProgramHost){.program_changed = changed, .say = heard});
    }
    /* bool uniforms (a LOVE shader's "extern bool"): reported as GL_BOOL and GL_BOOL_VEC3, an array's and a struct
       member's too, though memory keeps them as unsigned integers; set from an integer, read back as 1 */
    {
        static const char vs[] = "#version 300 es\nin vec4 p;\nvoid main() { gl_Position = p; }\n";
        static const char fs[] = "#version 300 es\nprecision mediump float;\nuniform bool shadow;\n"
                                 "uniform bvec3 mask;\nuniform bool flags[2];\nstruct S { bool on; float x; };\n"
                                 "uniform S s;\nuniform uint plain;\nout vec4 o;\n"
                                 "void main() { o = vec4(shadow && mask.y && flags[1] && s.on ? s.x : 0.0, "
                                 "float(plain), 0.0, 1.0); }\n";
        uint32_t p = program(es, shader(es, VERTEX, vs), shader(es, FRAGMENT, fs)),
                 want[5][2] = {{snx_gl_uniform_index(es, p, "shadow"), 0x8b56},
                               {snx_gl_uniform_index(es, p, "mask"), 0x8b58},
                               {snx_gl_uniform_index(es, p, "flags[0]"), 0x8b56},
                               {snx_gl_uniform_index(es, p, "s.on"), 0x8b56},
                               {snx_gl_uniform_index(es, p, "plain"), 0x1405}};
        int32_t one = 5, back = 0, count;
        uint32_t type;
        assert(piv(es, p, LINK_STATUS));
        for (int i = 0; i < 5; i++) {
            assert(want[i][0] != 0xffffffffu && snx_gl_active(es, p, want[i][0], 1, &count, &type));
            if (type != want[i][1]) {
                fprintf(stderr, "gl_program: uniform %d is type 0x%x, not 0x%x\n", i, (unsigned)type, want[i][1]);
                abort();
            }
        }
        snx_gl_use_program(es, p);
        snx_gl_uniform(es, snx_gl_uniform_location(es, p, "shadow"), 1, &one, 1, 'i', 0);
        snx_gl_get_uniform(es, p, snx_gl_uniform_location(es, p, "shadow"), 'i', &back);
        assert(!snx_gl_get_error(es) && back == 1);
    }
    puts("gl_program: ok");
    return 0;
}
