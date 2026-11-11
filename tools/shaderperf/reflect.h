/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx-shaderperf's SPIR-V reflection (reflect.c): the entry points of a module, and of one entry point the descriptor
   bindings, push constant block, specialization constants, stage inputs and outputs and workgroup size the tool has
   to give it something for. Enough of the module is read for Vulkan shaders as compilers and ScarletNX's translator
   write them, not every corner of the specification */
#ifndef SNX_SHADERPERF_REFLECT_H
#define SNX_SHADERPERF_REFLECT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

/* a scalar's kind: float, signed or unsigned integer, or bool */
enum { SR_FLOAT, SR_SINT, SR_UINT, SR_BOOL, SR_OTHER };

/* A member of a block (a push constant or uniform block), flattened: "pc.scale", "lights[0].color" are not split
   further than the block's own members, an array member keeps its stride */
typedef struct {
    char name[64];
    uint32_t offset, size;
    uint8_t scalar;     /* SR_* of the element */
    uint8_t width;      /* bits of the element's scalar */
    uint8_t vec, cols;  /* components of a vector (1 for a scalar), columns of a matrix (1 otherwise) */
    uint32_t array;     /* elements of an array member, 0 when it is none, UINT32_MAX for a runtime array */
    uint32_t stride;    /* ArrayStride of an array, MatrixStride of a matrix */
} SrMember;

typedef struct {
    uint32_t set, binding;
    VkDescriptorType type;
    uint32_t count;      /* array elements, 0 for a runtime array */
    char name[64];
    /* images and texel buffers */
    uint32_t dim;        /* SPIR-V Dim: 0 1D, 1 2D, 2 3D, 3 Cube, 5 Buffer, 6 SubpassData */
    bool depth, arrayed, ms, writable;
    uint32_t image_format; /* SPIR-V ImageFormat, 0 Unknown */
    uint8_t sampled_scalar; /* SR_* of the sampled type */
    /* buffers: the block's size, with a runtime array's elements counted 0 times, and that array's stride */
    uint32_t block_size, runtime_stride;
} SrBinding;

typedef struct {
    uint32_t location, component;
    uint8_t scalar, width, vec;
    uint32_t locations; /* locations it takes: matrix columns, array elements */
    bool flat, builtin;
    char name[64];
} SrVarying;

typedef struct {
    uint32_t spec_id;
    char name[64];
    uint8_t scalar, width; /* SR_BOOL for OpSpecConstantTrue/False */
    uint64_t value;        /* the default's bits */
} SrSpec;

typedef struct {
    char name[64];
    VkShaderStageFlagBits stage;
} SrEntry;

typedef struct {
    SrEntry* entries;
    size_t entry_count;
    /* of the chosen entry point */
    SrEntry entry;
    SrBinding* bindings;
    size_t binding_count;
    SrMember* push;
    size_t push_count;
    uint32_t push_size; /* bytes of the push constant block, 0 without one */
    char push_name[64];
    SrSpec* specs;
    size_t spec_count;
    SrVarying *inputs, *outputs;
    size_t input_count, output_count;
    uint32_t local[3];       /* workgroup size, 1,1,1 for other stages */
    uint32_t local_spec[3];  /* the SpecId that gives each, UINT32_MAX when a literal does */
    bool depth_replacing, early_tests, origin_lower_left;
    char** capabilities;     /* names of OpCapability values the reflector knows, else "Capability N" */
    size_t capability_count;
    char** extensions;
    size_t extension_count;
    uint32_t version;        /* SPIR-V version word */
    uint32_t id_bound, instructions;
} SrModule;

/* Reflects size bytes of SPIR-V for the entry point named entry (the first one when entry is NULL or empty). False
   with a message in err when the words are not SPIR-V or have no such entry point. The module owns what it holds */
bool sr_reflect(const uint32_t* words, size_t size, const char* entry, SrModule* out, char* err, size_t err_size);
void sr_free(SrModule* m);
/* the Vulkan format a SPIR-V ImageFormat names, VK_FORMAT_UNDEFINED for Unknown */
VkFormat sr_image_format_vk(uint32_t image_format);
const char* sr_stage_name(VkShaderStageFlagBits stage);
const char* sr_descriptor_name(VkDescriptorType type);
const char* sr_scalar_name(uint8_t scalar);
#endif
