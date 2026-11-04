/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* GLES 2 and 3 over Vulkan (src/android/bridge/gles2*.c), drawn on the host's Vulkan (lavapipe) into a pbuffer and
   framebuffer objects, and read back with glReadPixels: a triangle, textures (RGBA, RGB, luminance), blending,
   the depth test, a framebuffer object sampled as a texture, instanced draws with gl_InstanceID and a divisor, a
   uniform block, client arrays and indices, a line loop, glBlitFramebuffer and glCopyTexSubImage2D. The calls go
   through the HLE's handlers with the arguments in a CPU state, as a guest's do. Exits 77 without a Vulkan 1.3
   device */
#include "android/bridge/gles2.h"
#include "android/bridge/hle.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { W = 64, H = 64 };

static struct {
    CPU cpu;
    uint64_t stack[8];
    int failures;
    const char* test;
} g_t;

static const HleSymbol* find(const char* name) {
    const HleSymbol* const tables[] = {hle_gles2, hle_gles2_object, hle_gles2_program, hle_gles2_draw};
    for (size_t t = 0; t < sizeof tables / sizeof *tables; ++t)
        for (const HleSymbol* s = tables[t]; s->name[0]; ++s)
            if (!strcmp(s->name, name)) return s;
    fprintf(stderr, "android_gles2: no function %s\n", name);
    exit(1);
}

/* a call with integer arguments x (the ninth and later on the stack) and floating ones f */
static uint64_t call(const char* name, const uint64_t* x, size_t nx, const float* f, size_t nf) {
    const HleSymbol* s = find(name);
    g_t.cpu = (CPU){0};
    for (size_t i = 0; i < nx; ++i) {
        if (i < 8) g_t.cpu.x[i] = x[i];
        else g_t.stack[i - 8] = x[i];
    }
    g_t.cpu.x[31] = (uint64_t)(uintptr_t)g_t.stack;
    for (size_t i = 0; i < nf; ++i) memcpy(&g_t.cpu.v[i].f[0], &f[i], 4);
    s->handler(&g_t.cpu, s->data);
    return g_t.cpu.x[0];
}
#define P(p) ((uint64_t)(uintptr_t)(p))
#define GL(name, ...) call(name, (uint64_t[]){__VA_ARGS__}, sizeof((uint64_t[]){__VA_ARGS__}) / 8, NULL, 0)
#define GL0(name) call(name, NULL, 0, NULL, 0)
#define GLF(name, ints, ...)                                                                                           \
    call(name, (uint64_t[]){ints}, sizeof((uint64_t[]){ints}) / 8, (float[]){__VA_ARGS__},                            \
         sizeof((float[]){__VA_ARGS__}) / 4)
#define GLF0(name, ...) call(name, NULL, 0, (float[]){__VA_ARGS__}, sizeof((float[]){__VA_ARGS__}) / 4)

enum {
    GL_TRIANGLES = 4, GL_TRIANGLE_STRIP = 5, GL_LINE_LOOP = 2, GL_FLOAT = 0x1406, GL_UNSIGNED_BYTE = 0x1401,
    GL_UNSIGNED_SHORT = 0x1403, GL_RGBA = 0x1908, GL_RGB = 0x1907, GL_LUMINANCE = 0x1909,
    GL_TEXTURE_2D = 0x0de1, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800, GL_NEAREST = 0x2600,
    GL_ARRAY_BUFFER = 0x8892, GL_ELEMENT_ARRAY_BUFFER = 0x8893, GL_UNIFORM_BUFFER = 0x8a11, GL_STATIC_DRAW = 0x88e4,
    GL_VERTEX_SHADER = 0x8b31, GL_FRAGMENT_SHADER = 0x8b30, GL_COMPILE_STATUS = 0x8b81, GL_LINK_STATUS = 0x8b82,
    GL_COLOR_BUFFER_BIT = 0x4000, GL_DEPTH_BUFFER_BIT = 0x100, GL_BLEND = 0x0be2, GL_DEPTH_TEST = 0x0b71,
    GL_SRC_ALPHA = 0x302, GL_ONE_MINUS_SRC_ALPHA = 0x303, GL_LESS = 0x201, GL_FRAMEBUFFER = 0x8d40,
    GL_COLOR_ATTACHMENT0 = 0x8ce0, GL_FRAMEBUFFER_COMPLETE = 0x8cd5, GL_RENDERBUFFER = 0x8d41,
    GL_DEPTH_ATTACHMENT = 0x8d00, GL_DEPTH_COMPONENT16 = 0x81a5, GL_RGBA8 = 0x8058, GL_READ_FRAMEBUFFER = 0x8ca8,
    GL_DRAW_FRAMEBUFFER = 0x8ca9, GL_UNPACK_ALIGNMENT = 0x0cf5, GL_TEXTURE0 = 0x84c0, GL_NO_ERROR = 0,
};

/* the pixel at x, y (GL's, from the bottom left) of the read framebuffer */
static void pixel(int x, int y, unsigned char out[4]) {
    GL("glReadPixels", (uint64_t)x, (uint64_t)y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, P(out));
}
static void expect(int x, int y, int r, int g, int b, int a, const char* what) {
    unsigned char p[4];
    pixel(x, y, p);
    int want[4] = {r, g, b, a};
    for (int i = 0; i < 4; ++i)
        if (abs((int)p[i] - want[i]) > 3) {
            fprintf(stderr, "android_gles2: %s: %s: pixel %d,%d is %u %u %u %u, want %d %d %d %d\n", g_t.test, what, x,
                    y, p[0], p[1], p[2], p[3], r, g, b, a);
            g_t.failures++;
            return;
        }
}
static void no_error(const char* where) {
    uint64_t e = GL0("glGetError");
    if (e != GL_NO_ERROR) {
        fprintf(stderr, "android_gles2: %s: %s: GL error 0x%llx\n", g_t.test, where, (unsigned long long)e);
        g_t.failures++;
    }
}

static unsigned shader(unsigned type, const char* source) {
    unsigned s = (unsigned)GL("glCreateShader", type);
    uint64_t strings[1] = {P(source)};
    GL("glShaderSource", s, 1, P(strings), 0);
    GL("glCompileShader", s);
    int32_t ok = 0;
    GL("glGetShaderiv", s, GL_COMPILE_STATUS, P(&ok));
    if (!ok) {
        char log[512] = {0};
        GL("glGetShaderInfoLog", s, sizeof log, 0, P(log));
        fprintf(stderr, "android_gles2: %s: a shader does not compile: %s\n", g_t.test, log);
        exit(1);
    }
    return s;
}
/* a program of two stages; position at attribute location 0 when bind */
static unsigned program(const char* vs, const char* fs) {
    unsigned p = (unsigned)GL0("glCreateProgram");
    GL("glAttachShader", p, shader(GL_VERTEX_SHADER, vs));
    GL("glAttachShader", p, shader(GL_FRAGMENT_SHADER, fs));
    GL("glBindAttribLocation", p, 0, P("position"));
    GL("glLinkProgram", p);
    int32_t ok = 0;
    GL("glGetProgramiv", p, GL_LINK_STATUS, P(&ok));
    if (!ok) {
        char log[512] = {0};
        GL("glGetProgramInfoLog", p, sizeof log, 0, P(log));
        fprintf(stderr, "android_gles2: %s: a program does not link: %s\n", g_t.test, log);
        exit(1);
    }
    GL("glUseProgram", p);
    return p;
}
static unsigned buffer(unsigned target, const void* data, size_t size) {
    uint32_t b = 0;
    GL("glGenBuffers", 1, P(&b));
    GL("glBindBuffer", target, b);
    GL("glBufferData", target, size, P(data), GL_STATIC_DRAW);
    return b;
}
static unsigned texture(int w, int h, unsigned format, const void* pixels) {
    uint32_t t = 0;
    GL("glGenTextures", 1, P(&t));
    GL("glBindTexture", GL_TEXTURE_2D, t);
    GL("glTexParameteri", GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    GL("glTexParameteri", GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GL("glTexImage2D", GL_TEXTURE_2D, 0, format, (uint64_t)w, (uint64_t)h, 0, format, GL_UNSIGNED_BYTE, P(pixels));
    return t;
}
static void clear(float r, float g, float b, float a) {
    GLF0("glClearColor", r, g, b, a);
    GL("glClear", GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static const char* const plain_vs = "#version 300 es\nin vec4 position;\nvoid main() { gl_Position = position; }\n";
static const char* const uniform_fs = "#version 300 es\nprecision mediump float;\nuniform vec4 color;\n"
                                      "out vec4 frag;\nvoid main() { frag = color; }\n";

/* a red triangle over the bottom left half, on blue: GL's origin is the bottom left */
static void triangle(void) {
    g_t.test = "triangle";
    unsigned p = program(plain_vs, uniform_fs);
    static const float tri[] = {-1, -1, 1, -1, -1, 1};
    buffer(GL_ARRAY_BUFFER, tri, sizeof tri);
    GL("glVertexAttribPointer", 0, 2, GL_FLOAT, 0, 0, 0);
    GL("glEnableVertexAttribArray", 0);
    GLF("glUniform4f", GL("glGetUniformLocation", p, P("color")), 1, 0, 0, 1);
    clear(0, 0, 1, 1);
    GL("glDrawArrays", GL_TRIANGLES, 0, 3);
    no_error("draw");
    expect(4, 4, 255, 0, 0, 255, "inside");
    expect(W - 4, H - 4, 0, 0, 255, 255, "outside");
    /* the scissor keeps the clear to its rectangle */
    GL("glEnable", 0x0c11);
    GL("glScissor", 0, 0, 8, 8);
    clear(0, 1, 0, 1);
    GL("glDisable", 0x0c11);
    expect(2, 2, 0, 255, 0, 255, "scissored clear");
    expect(20, 2, 255, 0, 0, 255, "outside the scissor");
}

static const char* const textured_vs =
    "attribute vec4 position;\nvarying vec2 uv;\n"
    "void main() { uv = position.xy * 0.5 + 0.5; gl_Position = position; }\n";
static const char* const textured_fs = "precision mediump float;\nvarying vec2 uv;\nuniform sampler2D tex;\n"
                                       "void main() { gl_FragColor = texture2D(tex, uv); }\n";
static const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};

/* a 2x2 texture over the window: its row 0 at the bottom, as GL has it; then RGB and luminance texels */
static void textures(void) {
    g_t.test = "textures";
    unsigned p = program(textured_vs, textured_fs);
    buffer(GL_ARRAY_BUFFER, quad, sizeof quad);
    GL("glVertexAttribPointer", 0, 2, GL_FLOAT, 0, 0, 0);
    GL("glEnableVertexAttribArray", 0);
    static const unsigned char rgba[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    GL("glActiveTexture", GL_TEXTURE0 + 3);
    texture(2, 2, GL_RGBA, rgba);
    GL("glUniform1i", GL("glGetUniformLocation", p, P("tex")), 3);
    GL("glDrawArrays", GL_TRIANGLE_STRIP, 0, 4);
    no_error("rgba");
    expect(8, 8, 255, 0, 0, 255, "texel 0,0");
    expect(W - 8, 8, 0, 255, 0, 255, "texel 1,0");
    expect(8, H - 8, 0, 0, 255, 255, "texel 0,1");
    expect(W - 8, H - 8, 255, 255, 255, 255, "texel 1,1");
    /* 3-byte texels, rows at an alignment of 1 */
    static const unsigned char rgb[12] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120};
    GL("glPixelStorei", GL_UNPACK_ALIGNMENT, 1);
    texture(2, 2, GL_RGB, rgb);
    GL("glDrawArrays", GL_TRIANGLE_STRIP, 0, 4);
    no_error("rgb");
    expect(8, 8, 10, 20, 30, 255, "rgb texel 0,0");
    expect(W - 8, H - 8, 100, 110, 120, 255, "rgb texel 1,1");
    static const unsigned char lum[4] = {50, 100, 150, 200};
    texture(2, 2, GL_LUMINANCE, lum);
    GL("glDrawArrays", GL_TRIANGLE_STRIP, 0, 4);
    expect(W - 8, 8, 100, 100, 100, 255, "luminance texel 1,0");
    GL("glPixelStorei", GL_UNPACK_ALIGNMENT, 4);
    /* glTexSubImage2D into the luminance texture's texel 0,1 */
    static const unsigned char one = 250;
    GL("glTexSubImage2D", GL_TEXTURE_2D, 0, 0, 1, 1, 1, GL_LUMINANCE, GL_UNSIGNED_BYTE, P(&one));
    GL("glDrawArrays", GL_TRIANGLE_STRIP, 0, 4);
    expect(8, H - 8, 250, 250, 250, 255, "sub image");
    GL("glActiveTexture", GL_TEXTURE0);
}

/* half-transparent green over red: their mean */
static void blend(void) {
    g_t.test = "blend";
    unsigned p = program(plain_vs, uniform_fs);
    buffer(GL_ARRAY_BUFFER, quad, sizeof quad);
    GL("glVertexAttribPointer", 0, 2, GL_FLOAT, 0, 0, 0);
    GL("glEnableVertexAttribArray", 0);
    clear(1, 0, 0, 1);
    GL("glEnable", GL_BLEND);
    GL("glBlendFunc", GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    GLF("glUniform4f", GL("glGetUniformLocation", p, P("color")), 0, 1, 0, 0.5f);
    GL("glDrawArrays", GL_TRIANGLE_STRIP, 0, 4);
    GL("glDisable", GL_BLEND);
    expect(W / 2, H / 2, 128, 128, 0, 191, "blended");
}

/* a near quad, then a far one with GL_LESS: the near one stays */
static void depth(void) {
    g_t.test = "depth";
    const char* vs = "#version 300 es\nin vec4 position;\nuniform float z;\n"
                     "void main() { gl_Position = vec4(position.xy, z, 1.0); }\n";
    unsigned p = program(vs, uniform_fs);
    buffer(GL_ARRAY_BUFFER, quad, sizeof quad);
    GL("glVertexAttribPointer", 0, 2, GL_FLOAT, 0, 0, 0);
    GL("glEnableVertexAttribArray", 0);
    GL("glEnable", GL_DEPTH_TEST);
    GL("glDepthFunc", GL_LESS);
    clear(0, 0, 0, 1);
    int64_t z = (int64_t)GL("glGetUniformLocation", p, P("z")), color = (int64_t)GL("glGetUniformLocation", p, P("color"));
    GLF("glUniform1f", (uint64_t)z, -0.5f);
    GLF("glUniform4f", (uint64_t)color, 0, 1, 0, 1);
    GL("glDrawArrays", GL_TRIANGLE_STRIP, 0, 4);
    GLF("glUniform1f", (uint64_t)z, 0.5f);
    GLF("glUniform4f", (uint64_t)color, 1, 0, 0, 1);
    GL("glDrawArrays", GL_TRIANGLE_STRIP, 0, 4);
    expect(W / 2, H / 2, 0, 255, 0, 255, "the near quad");
    /* GL's depth -1 to 1: z of 0.9 is nearer than 0.95 */
    clear(0, 0, 0, 1);
    GLF("glUniform1f", (uint64_t)z, 0.95f);
    GLF("glUniform4f", (uint64_t)color, 1, 0, 0, 1);
    GL("glDrawArrays", GL_TRIANGLE_STRIP, 0, 4);
    GLF("glUniform1f", (uint64_t)z, 0.9f);
    GLF("glUniform4f", (uint64_t)color, 0, 0, 1, 1);
    GL("glDrawArrays", GL_TRIANGLE_STRIP, 0, 4);
    expect(W / 2, H / 2, 0, 0, 255, 255, "the nearer of two far quads");
    GL("glDisable", GL_DEPTH_TEST);
}

/* a framebuffer object: red over its bottom half, green on top, then sampled over the window, upright */
static void fbo(void) {
    g_t.test = "fbo";
    uint32_t fb = 0, rb = 0;
    unsigned t = texture(32, 32, GL_RGBA, NULL);
    GL("glGenFramebuffers", 1, P(&fb));
    GL("glBindFramebuffer", GL_FRAMEBUFFER, fb);
    GL("glFramebufferTexture2D", GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    GL("glGenRenderbuffers", 1, P(&rb));
    GL("glBindRenderbuffer", GL_RENDERBUFFER, rb);
    GL("glRenderbufferStorage", GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, 32, 32);
    GL("glFramebufferRenderbuffer", GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb);
    if (GL("glCheckFramebufferStatus", GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "android_gles2: fbo: incomplete\n");
        g_t.failures++;
        return;
    }
    GL("glViewport", 0, 0, 32, 32);
    unsigned p = program(plain_vs, uniform_fs);
    static const float bottom[] = {-1, -1, 1, -1, -1, 0, 1, 0};
    buffer(GL_ARRAY_BUFFER, bottom, sizeof bottom);
    GL("glVertexAttribPointer", 0, 2, GL_FLOAT, 0, 0, 0);
    GL("glEnableVertexAttribArray", 0);
    clear(0, 1, 0, 1);
    GLF("glUniform4f", GL("glGetUniformLocation", p, P("color")), 1, 0, 0, 1);
    GL("glDrawArrays", GL_TRIANGLE_STRIP, 0, 4);
    /* the framebuffer read back: its own bottom is red */
    expect(4, 4, 255, 0, 0, 255, "fbo bottom");
    expect(4, 28, 0, 255, 0, 255, "fbo top");
    /* a triangle's winding is GL's in the framebuffer too: culling the back faces keeps a counterclockwise one */
    GL("glEnable", 0x0b44);
    clear(0, 0, 0, 1);
    static const float ccw[] = {-1, -1, 1, -1, -1, 1};
    buffer(GL_ARRAY_BUFFER, ccw, sizeof ccw);
    GL("glVertexAttribPointer", 0, 2, GL_FLOAT, 0, 0, 0);
    GL("glDrawArrays", GL_TRIANGLES, 0, 3);
    GL("glDisable", 0x0b44);
    expect(4, 4, 255, 0, 0, 255, "a front face drawn in the fbo");
    /* again for the sampling below */
    clear(0, 1, 0, 1);
    buffer(GL_ARRAY_BUFFER, bottom, sizeof bottom);
    GL("glVertexAttribPointer", 0, 2, GL_FLOAT, 0, 0, 0);
    GL("glDrawArrays", GL_TRIANGLE_STRIP, 0, 4);
    GL("glBindFramebuffer", GL_FRAMEBUFFER, 0);
    GL("glViewport", 0, 0, W, H);
    p = program(textured_vs, textured_fs);
    GL("glUniform1i", GL("glGetUniformLocation", p, P("tex")), 0);
    buffer(GL_ARRAY_BUFFER, quad, sizeof quad);
    GL("glVertexAttribPointer", 0, 2, GL_FLOAT, 0, 0, 0);
    GL("glBindTexture", GL_TEXTURE_2D, t);
    GL("glDrawArrays", GL_TRIANGLE_STRIP, 0, 4);
    no_error("sampled");
    expect(W / 2, 8, 255, 0, 0, 255, "the window's bottom");
    expect(W / 2, H - 8, 0, 255, 0, 255, "the window's top");
    /* a window's triangle with back faces culled: counterclockwise is front there as well */
    GL("glEnable", 0x0b44);
    p = program(plain_vs, uniform_fs);
    GLF("glUniform4f", GL("glGetUniformLocation", p, P("color")), 0, 0, 1, 1);
    buffer(GL_ARRAY_BUFFER, ccw, sizeof ccw);
    GL("glVertexAttribPointer", 0, 2, GL_FLOAT, 0, 0, 0);
    GL("glDrawArrays", GL_TRIANGLES, 0, 3);
    GL("glDisable", 0x0b44);
    expect(4, 4, 0, 0, 255, 255, "a front face drawn in the window");
    /* glBlitFramebuffer of the fbo's bottom left quarter onto the window's top right, and glCopyTexSubImage2D */
    GL("glBindFramebuffer", GL_READ_FRAMEBUFFER, fb);
    GL("glBindFramebuffer", GL_DRAW_FRAMEBUFFER, 0);
    GL("glBlitFramebuffer", 0, 0, 16, 16, W / 2, H / 2, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    GL("glBindFramebuffer", GL_FRAMEBUFFER, 0);
    no_error("blit");
    expect(W - 4, H - 4, 255, 0, 0, 255, "blitted");
    GL("glBindTexture", GL_TEXTURE_2D, t);
    GL("glCopyTexSubImage2D", GL_TEXTURE_2D, 0, 0, 16, 0, 0, 16, 16);
    GL("glBindFramebuffer", GL_FRAMEBUFFER, fb);
    expect(4, 20, 0, 0, 255, 255, "copied into the texture");
    GL("glBindFramebuffer", GL_FRAMEBUFFER, 0);
}

/* four quads, each at its gl_InstanceID's place, colored by a per-instance attribute */
static void instanced(void) {
    g_t.test = "instanced";
    const char* vs = "#version 300 es\nlayout(location = 0) in vec2 position;\nin vec4 tint;\nout vec4 c;\n"
                     "void main() { vec2 at = vec2(float(gl_InstanceID % 2), float(gl_InstanceID / 2));\n"
                     "  gl_Position = vec4(position * 0.5 + at - 0.5, 0.0, 1.0); c = tint; }\n";
    const char* fs = "#version 300 es\nprecision mediump float;\nin vec4 c;\nout vec4 frag;\n"
                     "void main() { frag = c; }\n";
    unsigned p = program(vs, fs);
    static const float small[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    static const unsigned char tints[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255};
    buffer(GL_ARRAY_BUFFER, small, sizeof small);
    GL("glVertexAttribPointer", 0, 2, GL_FLOAT, 0, 0, 0);
    GL("glEnableVertexAttribArray", 0);
    int64_t tint = (int64_t)GL("glGetAttribLocation", p, P("tint"));
    buffer(GL_ARRAY_BUFFER, tints, sizeof tints);
    GL("glVertexAttribPointer", (uint64_t)tint, 4, GL_UNSIGNED_BYTE, 1, 0, 0);
    GL("glEnableVertexAttribArray", (uint64_t)tint);
    GL("glVertexAttribDivisor", (uint64_t)tint, 1);
    clear(0, 0, 0, 1);
    GL("glDrawArraysInstanced", GL_TRIANGLE_STRIP, 0, 4, 4);
    no_error("draw");
    expect(W / 4, H / 4, 255, 0, 0, 255, "instance 0");
    expect(3 * W / 4, H / 4, 0, 255, 0, 255, "instance 1");
    expect(W / 4, 3 * H / 4, 0, 0, 255, 255, "instance 2");
    expect(3 * W / 4, 3 * H / 4, 255, 255, 0, 255, "instance 3");
    GL("glVertexAttribDivisor", (uint64_t)tint, 0);
    GL("glDisableVertexAttribArray", (uint64_t)tint);
}

/* a uniform block's color, std140, from a buffer at binding point 2 */
static void uniform_block(void) {
    g_t.test = "uniform block";
    const char* fs = "#version 300 es\nprecision mediump float;\nlayout(std140) uniform Colors { vec4 unused; "
                     "vec4 color; };\nout vec4 frag;\nvoid main() { frag = color; }\n";
    unsigned p = program(plain_vs, fs);
    uint64_t index = GL("glGetUniformBlockIndex", p, P("Colors"));
    int32_t size = 0;
    GL("glGetActiveUniformBlockiv", p, index, 0x8a40, P(&size));
    if (index == 0xffffffffu || size != 32) {
        fprintf(stderr, "android_gles2: uniform block: index %llu, size %d\n", (unsigned long long)index, size);
        g_t.failures++;
        return;
    }
    GL("glUniformBlockBinding", p, index, 2);
    static const float colors[8] = {1, 1, 1, 1, 0, 1, 1, 1};
    unsigned b = buffer(GL_UNIFORM_BUFFER, colors, sizeof colors);
    GL("glBindBufferBase", GL_UNIFORM_BUFFER, 2, b);
    buffer(GL_ARRAY_BUFFER, quad, sizeof quad);
    GL("glVertexAttribPointer", 0, 2, GL_FLOAT, 0, 0, 0);
    GL("glEnableVertexAttribArray", 0);
    GL("glDrawArrays", GL_TRIANGLE_STRIP, 0, 4);
    no_error("draw");
    expect(W / 2, H / 2, 0, 255, 255, 255, "the block's color");
    /* the buffer written again: the next draw reads the new color, the first one kept the old */
    static const float yellow[4] = {1, 1, 0, 1};
    GL("glBindBuffer", GL_UNIFORM_BUFFER, b);
    GL("glBufferSubData", GL_UNIFORM_BUFFER, 16, 16, P(yellow));
    GL("glDrawArrays", GL_TRIANGLE_STRIP, 0, 4);
    expect(W / 2, H / 2, 255, 255, 0, 255, "the block's new color");
}

/* client arrays without buffers, indices from the client, a line loop, and arrays of uniforms */
static void client_arrays(void) {
    g_t.test = "client arrays";
    const char* fs = "#version 300 es\nprecision mediump float;\nuniform vec4 colors[3];\nuniform int which;\n"
                     "out vec4 frag;\nvoid main() { frag = colors[which]; }\n";
    unsigned p = program(plain_vs, fs);
    GL("glBindBuffer", GL_ARRAY_BUFFER, 0);
    static const float verts[] = {9, 9, -1, -1, 1, -1, -1, 1, 1, 1};
    GL("glVertexAttribPointer", 0, 2, GL_FLOAT, 0, 0, P(verts));
    GL("glEnableVertexAttribArray", 0);
    static const float colors[12] = {1, 0, 0, 1, 0, 1, 0, 1, 1, 0, 1, 1};
    GL("glUniform4fv", GL("glGetUniformLocation", p, P("colors")), 3, P(colors));
    GL("glUniform1i", GL("glGetUniformLocation", p, P("which")), 2);
    clear(0, 0, 0, 1);
    static const uint16_t indices[6] = {1, 2, 3, 3, 2, 4};
    GL("glBindBuffer", GL_ELEMENT_ARRAY_BUFFER, 0);
    GL("glDrawElements", GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, P(indices));
    no_error("elements");
    expect(W / 2, H / 2, 255, 0, 255, 255, "colors[2]");
    GLF("glUniform4f", GL("glGetUniformLocation", p, P("colors[1]")), 0, 0, 1, 1);
    GL("glUniform1i", GL("glGetUniformLocation", p, P("which")), 1);
    clear(0, 0, 0, 1);
    static const float loop[] = {-0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f};
    GL("glVertexAttribPointer", 0, 2, GL_FLOAT, 0, 0, P(loop));
    GL("glDrawArrays", GL_LINE_LOOP, 0, 4);
    no_error("line loop");
    /* the closing line, from the last vertex back to the first: the left edge */
    unsigned char px[4] = {0};
    bool left = false, right = false;
    for (int dx = -2; dx <= 2 && !left; ++dx) {
        pixel(W / 4 + dx, H / 2, px);
        left = px[2] > 200;
    }
    for (int dx = -2; dx <= 2 && !right; ++dx) {
        pixel(3 * W / 4 + dx, H / 2, px);
        right = px[2] > 200;
    }
    if (!left || !right) {
        fprintf(stderr, "android_gles2: client arrays: the line loop's edges: left %d right %d\n", left, right);
        g_t.failures++;
    }
}

/* the red channel at x, y, within 40 of a value */
static void expect_red(int x, int y, int red, const char* what) {
    unsigned char p[4];
    pixel(x, y, p);
    if (abs((int)p[0] - red) > 40) {
        fprintf(stderr, "android_gles2: %s: %s: red %u at %d,%d, want about %d\n", g_t.test, what, p[0], x, y, red);
        g_t.failures++;
    }
}

/* gl_FragCoord from the bottom left, and gl_PointCoord's t from the top, in the window and in a framebuffer */
static void window_coords(void) {
    g_t.test = "window coordinates";
    const char* fs = "#version 300 es\nprecision highp float;\nuniform float half_height;\nout vec4 frag;\n"
                     "void main() { frag = vec4(gl_FragCoord.y < half_height ? 1.0 : 0.0, 0.0, 0.0, 1.0); }\n";
    unsigned p = program(plain_vs, fs);
    buffer(GL_ARRAY_BUFFER, quad, sizeof quad);
    GL("glVertexAttribPointer", 0, 2, GL_FLOAT, 0, 0, 0);
    GL("glEnableVertexAttribArray", 0);
    GLF("glUniform1f", GL("glGetUniformLocation", p, P("half_height")), H / 2.0f);
    GL("glDrawArrays", GL_TRIANGLE_STRIP, 0, 4);
    expect(4, 4, 255, 0, 0, 255, "the window's bottom");
    expect(4, H - 4, 0, 0, 0, 255, "the window's top");
    uint32_t fb = 0;
    unsigned t = texture(32, 32, GL_RGBA, NULL);
    GL("glGenFramebuffers", 1, P(&fb));
    GL("glBindFramebuffer", GL_FRAMEBUFFER, fb);
    GL("glFramebufferTexture2D", GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    GL("glViewport", 0, 0, 32, 32);
    GLF("glUniform1f", GL("glGetUniformLocation", p, P("half_height")), 16.0f);
    GL("glDrawArrays", GL_TRIANGLE_STRIP, 0, 4);
    expect(4, 4, 255, 0, 0, 255, "the framebuffer's bottom");
    expect(4, 28, 0, 0, 0, 255, "the framebuffer's top");
    const char* vs = "#version 300 es\nvoid main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); gl_PointSize = 16.0; }\n";
    const char* points = "#version 300 es\nprecision highp float;\nout vec4 frag;\n"
                         "void main() { frag = vec4(gl_PointCoord.y, 0.0, 0.0, 1.0); }\n";
    program(vs, points);
    clear(0, 0, 0, 1);
    GL("glDrawArrays", 0, 0, 1);
    expect_red(16, 22, 50, "a point's top in the framebuffer");
    expect_red(16, 9, 205, "a point's bottom in the framebuffer");
    GL("glBindFramebuffer", GL_FRAMEBUFFER, 0);
    GL("glViewport", 0, 0, W, H);
    clear(0, 0, 0, 1);
    GL("glDrawArrays", 0, 0, 1);
    expect_red(W / 2, H / 2 + 6, 50, "a point's top in the window");
    expect_red(W / 2, H / 2 - 7, 205, "a point's bottom in the window");
    no_error("points");
}

static unsigned compute_program(const char* source) {
    unsigned p = (unsigned)GL0("glCreateProgram");
    GL("glAttachShader", p, shader(0x91b9, source));
    GL("glLinkProgram", p);
    int32_t ok = 0;
    GL("glGetProgramiv", p, GL_LINK_STATUS, P(&ok));
    if (!ok) {
        char log[512] = {0};
        GL("glGetProgramInfoLog", p, sizeof log, 0, P(log));
        fprintf(stderr, "android_gles2: %s: a compute program does not link: %s\n", g_t.test, log);
        exit(1);
    }
    GL("glUseProgram", p);
    return p;
}

/* GLES 3.1: a compute shader writes a storage buffer (read back mapped) and an image (sampled by a draw), by a
   direct and an indirect dispatch; an indirect draw */
static void compute(void) {
    g_t.test = "compute";
    const char* cs = "#version 310 es\nlayout(local_size_x = 4) in;\n"
                     "layout(std430, binding = 1) buffer Data { uint scale; uint v[]; } data;\n"
                     "void main() { data.v[gl_GlobalInvocationID.x] = gl_GlobalInvocationID.x * data.scale; }\n";
    unsigned p = compute_program(cs);
    uint32_t init[17] = {3};
    unsigned b = buffer(0x90d2, init, sizeof init);
    GL("glBindBufferBase", 0x90d2, 1, b);
    GL("glDispatchCompute", 4, 1, 1);
    no_error("dispatch");
    GL("glMemoryBarrier", 0x2000); /* GL_BUFFER_UPDATE_BARRIER_BIT */
    const uint32_t* v = (const uint32_t*)(uintptr_t)GL("glMapBufferRange", 0x90d2, 0, sizeof init, 1);
    if (!v || v[1] != 0 || v[2] != 3 || v[16] != 45) {
        fprintf(stderr, "android_gles2: compute: the buffer reads %u %u %u\n", v ? v[1] : 0, v ? v[2] : 0,
                v ? v[16] : 0);
        g_t.failures++;
    }
    GL("glUnmapBuffer", 0x90d2);
    uint64_t block = GL("glGetProgramResourceIndex", p, 0x92e6, P("Data"));
    int32_t binding = -1;
    uint32_t prop = 0x9302;
    GL("glGetProgramResourceiv", p, 0x92e6, block, 1, P(&prop), 1, 0, P(&binding));
    if (block == 0xffffffffu || binding != 1) {
        fprintf(stderr, "android_gles2: compute: the block's index %llu and binding %d\n", (unsigned long long)block,
                binding);
        g_t.failures++;
    }
    /* an image written by an indirect dispatch, then sampled over the window */
    const char* image = "#version 310 es\nlayout(local_size_x = 8, local_size_y = 8) in;\n"
                        "layout(rgba8, binding = 2) writeonly uniform highp image2D img;\n"
                        "void main() { ivec2 at = ivec2(gl_GlobalInvocationID.xy);\n"
                        "  imageStore(img, at, at.y < 8 ? vec4(1.0, 0.5, 0.0, 1.0) : vec4(0.0, 0.0, 1.0, 1.0)); }\n";
    compute_program(image);
    uint32_t t = 0;
    GL("glGenTextures", 1, P(&t));
    GL("glBindTexture", GL_TEXTURE_2D, t);
    GL("glTexStorage2D", GL_TEXTURE_2D, 1, GL_RGBA8, 16, 16);
    GL("glTexParameteri", GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    GL("glTexParameteri", GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GL("glBindImageTexture", 2, t, 0, 0, 0, 0x88b9, GL_RGBA8); /* GL_WRITE_ONLY */
    static const uint32_t groups[3] = {2, 2, 1};
    unsigned ib = buffer(0x90ee, groups, sizeof groups);
    GL("glBindBuffer", 0x90ee, ib);
    GL("glDispatchComputeIndirect", 0);
    no_error("indirect dispatch");
    unsigned q = program(textured_vs, textured_fs);
    GL("glUniform1i", GL("glGetUniformLocation", q, P("tex")), 0);
    buffer(GL_ARRAY_BUFFER, quad, sizeof quad);
    GL("glVertexAttribPointer", 0, 2, GL_FLOAT, 0, 0, 0);
    GL("glEnableVertexAttribArray", 0);
    /* glDrawArraysIndirect: count 4, one instance, from 0 */
    static const uint32_t command[4] = {4, 1, 0, 0};
    unsigned db = buffer(0x8f3f, command, sizeof command);
    GL("glBindBuffer", 0x8f3f, db);
    GL("glDrawArraysIndirect", GL_TRIANGLE_STRIP, 0);
    no_error("indirect draw");
    expect(W / 2, 8, 255, 128, 0, 255, "the image's row 0, at the bottom");
    expect(W / 2, H - 8, 0, 0, 255, 255, "the image's top");
}

/* GLES 3.1's vertex bindings: a position and a color interleaved in one buffer, by their relative offsets */
static void vertex_bindings(void) {
    g_t.test = "vertex bindings";
    const char* vs = "#version 310 es\nlayout(location = 0) in vec2 position;\nlayout(location = 3) in vec4 tint;\n"
                     "out vec4 c;\nvoid main() { gl_Position = vec4(position, 0.0, 1.0); c = tint; }\n";
    const char* fs = "#version 310 es\nprecision mediump float;\nin vec4 c;\nout vec4 frag;\n"
                     "void main() { frag = c; }\n";
    program(vs, fs);
    struct {
        float x, y;
        unsigned char rgba[4];
    } vertices[4] = {{-1, -1, {0, 255, 128, 255}}, {1, -1, {0, 255, 128, 255}}, {-1, 1, {0, 255, 128, 255}},
                     {1, 1, {0, 255, 128, 255}}};
    uint32_t vao = 0;
    GL("glGenVertexArrays", 1, P(&vao));
    GL("glBindVertexArray", vao);
    unsigned b = buffer(GL_ARRAY_BUFFER, vertices, sizeof vertices);
    GL("glBindVertexBuffer", 5, b, 0, sizeof vertices[0]);
    GL("glVertexAttribFormat", 0, 2, GL_FLOAT, 0, 0);
    GL("glVertexAttribFormat", 3, 4, GL_UNSIGNED_BYTE, 1, 8);
    GL("glVertexAttribBinding", 0, 5);
    GL("glVertexAttribBinding", 3, 5);
    GL("glEnableVertexAttribArray", 0);
    GL("glEnableVertexAttribArray", 3);
    clear(0, 0, 0, 1);
    GL("glDrawArrays", GL_TRIANGLE_STRIP, 0, 4);
    no_error("draw");
    expect(W / 2, H / 2, 0, 255, 128, 255, "the interleaved color");
    GL("glBindVertexArray", 0);
}

/* the queries of state and of the program */
static void queries(void) {
    g_t.test = "queries";
    int32_t v = 0;
    GL("glGetIntegerv", 0x821b, P(&v));
    if (v != 3) fprintf(stderr, "android_gles2: queries: GL_MAJOR_VERSION %d\n", v), g_t.failures++;
    GL("glGetIntegerv", 0x0ba2, P((int32_t[4]){0}));
    unsigned p = program(textured_vs, textured_fs);
    int32_t count = 0, size = 0;
    uint32_t type = 0;
    char name[32] = {0};
    GL("glGetProgramiv", p, 0x8b86, P(&count));
    GL("glGetActiveUniform", p, 0, sizeof name, 0, P(&size), P(&type), P(name));
    if (count != 1 || strcmp(name, "tex") || type != 0x8b5e)
        fprintf(stderr, "android_gles2: queries: %d uniforms, the first %s of 0x%x\n", count, name, type), g_t.failures++;
    no_error("queries");
}

int main(void) {
    Gles2Context* gl = gles2_context_new(3);
    GlesSurface* s = gles_pbuffer_surface(W, H);
    if (!s || !g_gles_vk.v13) {
        printf("android_gles2: no Vulkan 1.3 device: skipped\n");
        return 77;
    }
    gles_surface_depth(s, 24, 8);
    gles2_make_current(gl, s, s);
    GL("glViewport", 0, 0, W, H);
    triangle();
    textures();
    blend();
    depth();
    fbo();
    instanced();
    uniform_block();
    client_arrays();
    window_coords();
    compute();
    vertex_bindings();
    queries();
    gles2_make_current(NULL, NULL, NULL);
    gles2_context_free(gl);
    gles_surface_free(s);
    gles2_shutdown(); /* every Vulkan object gone before the device, which the validation layer checks */
    if (g_t.failures) {
        fprintf(stderr, "android_gles2: %d checks failed\n", g_t.failures);
        return 1;
    }
    printf("android_gles2: a triangle, textures, blending, depth, a framebuffer object, instanced draws, a uniform "
           "block, client arrays and a line loop drawn on %s\n",
           g_gles_vk.props.deviceName);
    return 0;
}
