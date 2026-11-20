/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VITA_USSE_H
#define SNX_VITA_USSE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A GXP program (the USSE code the SGX543 runs, and its parameters) as a SPIR-V shader (usse.c), as Vita3K's shader
   recompiler makes one, through the shared SPIR-V writer (common/utilities/spirv_emit.h).

   The interface the renderer binds: set 0, binding 0 the vertex program's uniform buffers and binding 1 the fragment
   program's (storage buffers of 32-bit words, laid out as UsseShader.buffers says), bindings 2 to 17 the fragment
   program's textures (combined image samplers, by sampler index). A vertex program's attributes are inputs at the
   locations UsseShader.attribute_location gives; its outputs and a fragment program's inputs meet at fixed locations
   (color0 1, color1 2, fog 3, texcoord0 to 9 4 to 13); a fragment program writes location 0. Push constants (20 bytes,
   the vertex stage's): the viewport's state, UssePush */
#define USSE_ATTRIBUTES 16u
#define USSE_BUFFERS    16u
#define USSE_TEXTURES   16u
#define USSE_TEXTURE_BINDING 2u
#define USSE_COLOR_BINDING   (USSE_TEXTURE_BINDING + USSE_TEXTURES) /* the pixel's color, an input attachment */

typedef struct {
    float viewport_flag; /* 1: positions are clip space (the viewport is enabled), 0: window coordinates */
    float screen_width, screen_height;
    float z_offset, z_scale; /* the viewport's depth transform, applied in the shader */
} UssePush;

typedef struct {
    /* vertex: the vertex program's attributes (SceGxmVertexAttribute: the resource index each feeds, its format,
       SceGxmAttributeFormat, and component count) */
    struct {
        uint16_t reg;
        uint8_t format, components;
    } attributes[USSE_ATTRIBUTES];
    unsigned attribute_count;
    /* fragment: each sampler's texture format (SceGxmTextureFormat), which says what a texture read gives */
    uint32_t texture_formats[USSE_TEXTURES];
} UsseHints;

typedef struct {
    uint32_t* words;
    size_t count;
    bool fragment;
    /* vertex: the input location of each of the hints' attributes, -1 for one the program doesn't read */
    int attribute_location[USSE_ATTRIBUTES];
    /* the uniform buffers the program reads, in its storage buffer: the container (0 to 13, 14 the default one), where
       its words start there and how many */
    struct {
        uint32_t container, offset, words;
    } buffers[USSE_BUFFERS];
    unsigned buffer_count;
    uint32_t buffer_words; /* the storage buffer's size */
    uint32_t sampler_mask; /* the textures it samples */
    bool reads_color;      /* a fragment program that starts with its pixel's color in O0 (FRAGCOLOR_USED) */
} UsseShader;

/* program's bytes (size of them) to a shader: false with the reason in error (an instruction not translated yet).
   preserve_infnan declares SPV_KHR_float_controls' SignedZeroInfNanPreserve for 32-bit floats (for a device with
   shaderSignedZeroInfNanPreserveFloat32): without it the driver may assume no infinities, NaNs or signed zeros, and
   fold away what the SGX computes with them (a log2 of zero's -inf, a reciprocal of zero's +inf) */
bool usse_translate(const unsigned char* program, size_t size, const UsseHints* hints, bool preserve_infnan,
                    UsseShader* out, char* error, size_t error_size);
void usse_shader_free(UsseShader* shader);

/* The shader corpus (the vita_shader_corpus setting): a program and the hints it was translated with, written to
   folder as <hash>.gxp ("SNXGXP1", the hints, the program), once each. read gives a record's program (the caller's to
   free) and hints. False when it can't */
bool usse_corpus_write(const char* folder, const unsigned char* program, size_t size, const UsseHints* hints);
bool usse_corpus_read(const char* path, unsigned char** program, size_t* size, UsseHints* hints);
/* Each instruction of program, its pattern's name and whether the translator has it (name NULL for an encoding no
   pattern matches), for the corpus tool's survey */
void usse_survey(const unsigned char* program, size_t size, void (*each)(void* context, const char* name, bool known),
                 void* context);
#endif
