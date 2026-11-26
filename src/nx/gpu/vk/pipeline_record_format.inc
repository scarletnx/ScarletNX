/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The structures a pipeline record writes (pipeline_record.c) and the replay reads (pipeline_replay.c), as tables of
   their members: each member a line, "name value", in the table's order, a structure's between "{" and "}" lines,
   indented by depth for a reader (the replay reads past the spaces). A pNext chain is a "next Name {" block for each
   structure, its own pNext left out, and "next -" at its end; one the tables do not have is "next unrecorded <sType>".
   Included by those two files only */
#include <stddef.h>

enum {
    RK_U32,        /* uint32_t, an enum, flags, a VkBool32 */
    RK_I32,        /* int32_t */
    RK_F32,        /* float, "%.9g": the same float read back */
    RK_U64,        /* uint64_t (VkPipelineCreateFlags2) */
    RK_SIZE,       /* size_t */
    RK_STR,        /* const char*, the rest of the line; "-" for NULL */
    RK_F32N,       /* float[n] */
    RK_U32N,       /* uint32_t[n] (a VkClearColorValue's words) */
    RK_SUB,        /* a structure held in place, desc's */
    RK_NEXT,       /* the pNext chain */
    RK_PTR,        /* a pointer to one desc, "-" for NULL */
    RK_ARR,        /* count (uint32_t at off2) desc structures, "name N" then N blocks; "name N -" for a NULL pointer */
    RK_U32S,       /* count (uint32_t at off2) uint32_t values on the line, "name N -" for a NULL pointer */
    RK_F32S,       /* the same of floats */
    RK_BYTES,      /* count (size_t at off2) bytes in hex, "name N -" for a NULL pointer */
    RK_STRS,       /* count (uint32_t at off2) strings, a line each */
    RK_MODULE,     /* a VkShaderModule, by its SPIR-V's hash; "-" for none */
    RK_CODE,       /* SPIR-V of size_t bytes at off2, by its hash (VkShaderModuleCreateInfo) */
    RK_LAYOUT,     /* a VkPipelineLayout: its VkPipelineLayoutCreateInfo in a block */
    RK_SETLAYOUTS, /* count (uint32_t at off2) VkDescriptorSetLayouts, each its create info's block */
    RK_SAMPLERS,   /* immutable samplers, as many as the uint32_t at off2, each its create info's block; "-" for none */
    RK_LIBS,       /* count (uint32_t at off2) pipeline libraries, each its VkGraphicsPipelineCreateInfo's block */
    RK_NULLH,      /* a handle the record keeps only as none (a render pass, a base pipeline): "-", else unrecorded */
    RK_FEATS,      /* VkBool32s from off to the structure's end, "0" and "1" in one word */
    RK_MASK,       /* pSampleMask: as many words as the samples at off2 need (32 a word) */
    RK_FEEDBACK1,  /* VkPipelineCreationFeedbackCreateInfo's pPipelineCreationFeedback: written to, so not recorded */
    RK_FEEDBACKN,  /* its stages' feedback, as many as the uint32_t at off2 */
};
typedef struct RecDesc RecDesc;
typedef struct {
    const char* name;
    unsigned char kind;
    unsigned char n;     /* RK_F32N's and RK_U32N's length */
    unsigned short off;  /* the member */
    unsigned short off2; /* its count, or for RK_MASK the samples */
    const RecDesc* desc; /* RK_SUB's, RK_PTR's and RK_ARR's structure */
} RecField;
struct RecDesc {
    const char* name;
    VkStructureType stype; /* 0 for a structure without sType and pNext */
    unsigned short size;
    unsigned char count;
    const RecField* fields;
};
#define RF(T, m, k)       {#m, k, 0, (unsigned short)offsetof(T, m), 0, NULL}
#define RFN(T, m, k, len) {#m, k, len, (unsigned short)offsetof(T, m), 0, NULL}
#define RFC(T, m, c, k)   {#m, k, 0, (unsigned short)offsetof(T, m), (unsigned short)offsetof(T, c), NULL}
#define RFD(T, m, k, d)   {#m, k, 0, (unsigned short)offsetof(T, m), 0, &d}
#define RFCD(T, m, c, d)  {#m, RK_ARR, 0, (unsigned short)offsetof(T, m), (unsigned short)offsetof(T, c), &d}
#define RNEXT(T)          {"next", RK_NEXT, 0, (unsigned short)offsetof(T, pNext), 0, NULL}
#define RDESC(T, st, f)   {#T, st, (unsigned short)sizeof(T), (unsigned char)(sizeof f / sizeof *f), f}

/* ---- structures without sType */
static const RecField f_spec_entry[] = {RF(VkSpecializationMapEntry, constantID, RK_U32),
                                        RF(VkSpecializationMapEntry, offset, RK_U32),
                                        RF(VkSpecializationMapEntry, size, RK_SIZE)};
static const RecDesc d_spec_entry = RDESC(VkSpecializationMapEntry, 0, f_spec_entry);
static const RecField f_spec[] = {RFCD(VkSpecializationInfo, pMapEntries, mapEntryCount, d_spec_entry),
                                  RFC(VkSpecializationInfo, pData, dataSize, RK_BYTES)};
static const RecDesc d_spec = RDESC(VkSpecializationInfo, 0, f_spec);
static const RecField f_vbinding[] = {RF(VkVertexInputBindingDescription, binding, RK_U32),
                                      RF(VkVertexInputBindingDescription, stride, RK_U32),
                                      RF(VkVertexInputBindingDescription, inputRate, RK_U32)};
static const RecDesc d_vbinding = RDESC(VkVertexInputBindingDescription, 0, f_vbinding);
static const RecField f_vattr[] = {
    RF(VkVertexInputAttributeDescription, location, RK_U32), RF(VkVertexInputAttributeDescription, binding, RK_U32),
    RF(VkVertexInputAttributeDescription, format, RK_U32), RF(VkVertexInputAttributeDescription, offset, RK_U32)};
static const RecDesc d_vattr = RDESC(VkVertexInputAttributeDescription, 0, f_vattr);
static const RecField f_viewport[] = {RF(VkViewport, x, RK_F32),        RF(VkViewport, y, RK_F32),
                                      RF(VkViewport, width, RK_F32),    RF(VkViewport, height, RK_F32),
                                      RF(VkViewport, minDepth, RK_F32), RF(VkViewport, maxDepth, RK_F32)};
static const RecDesc d_viewport = RDESC(VkViewport, 0, f_viewport);
static const RecField f_rect[] = {RF(VkRect2D, offset.x, RK_I32), RF(VkRect2D, offset.y, RK_I32),
                                  RF(VkRect2D, extent.width, RK_U32), RF(VkRect2D, extent.height, RK_U32)};
static const RecDesc d_rect = RDESC(VkRect2D, 0, f_rect);
static const RecField f_stencil[] = {RF(VkStencilOpState, failOp, RK_U32),      RF(VkStencilOpState, passOp, RK_U32),
                                     RF(VkStencilOpState, depthFailOp, RK_U32), RF(VkStencilOpState, compareOp, RK_U32),
                                     RF(VkStencilOpState, compareMask, RK_U32), RF(VkStencilOpState, writeMask, RK_U32),
                                     RF(VkStencilOpState, reference, RK_U32)};
static const RecDesc d_stencil = RDESC(VkStencilOpState, 0, f_stencil);
static const RecField f_blend[] = {RF(VkPipelineColorBlendAttachmentState, blendEnable, RK_U32),
                                   RF(VkPipelineColorBlendAttachmentState, srcColorBlendFactor, RK_U32),
                                   RF(VkPipelineColorBlendAttachmentState, dstColorBlendFactor, RK_U32),
                                   RF(VkPipelineColorBlendAttachmentState, colorBlendOp, RK_U32),
                                   RF(VkPipelineColorBlendAttachmentState, srcAlphaBlendFactor, RK_U32),
                                   RF(VkPipelineColorBlendAttachmentState, dstAlphaBlendFactor, RK_U32),
                                   RF(VkPipelineColorBlendAttachmentState, alphaBlendOp, RK_U32),
                                   RF(VkPipelineColorBlendAttachmentState, colorWriteMask, RK_U32)};
static const RecDesc d_blend = RDESC(VkPipelineColorBlendAttachmentState, 0, f_blend);
static const RecField f_push[] = {RF(VkPushConstantRange, stageFlags, RK_U32), RF(VkPushConstantRange, offset, RK_U32),
                                  RF(VkPushConstantRange, size, RK_U32)};
static const RecDesc d_push = RDESC(VkPushConstantRange, 0, f_push);
static const RecField f_set_binding[] = {
    RF(VkDescriptorSetLayoutBinding, binding, RK_U32), RF(VkDescriptorSetLayoutBinding, descriptorType, RK_U32),
    RF(VkDescriptorSetLayoutBinding, descriptorCount, RK_U32), RF(VkDescriptorSetLayoutBinding, stageFlags, RK_U32),
    RFC(VkDescriptorSetLayoutBinding, pImmutableSamplers, descriptorCount, RK_SAMPLERS)};
static const RecDesc d_set_binding = RDESC(VkDescriptorSetLayoutBinding, 0, f_set_binding);
static const RecField f_location[] = {RF(VkSampleLocationEXT, x, RK_F32), RF(VkSampleLocationEXT, y, RK_F32)};
static const RecDesc d_location = RDESC(VkSampleLocationEXT, 0, f_location);
static const RecField f_divisor[] = {RF(VkVertexInputBindingDivisorDescription, binding, RK_U32),
                                     RF(VkVertexInputBindingDivisorDescription, divisor, RK_U32)};
static const RecDesc d_divisor = RDESC(VkVertexInputBindingDivisorDescription, 0, f_divisor);
static const RecField f_mutable_list[] = {
    RFC(VkMutableDescriptorTypeListEXT, pDescriptorTypes, descriptorTypeCount, RK_U32S)};
static const RecDesc d_mutable_list = RDESC(VkMutableDescriptorTypeListEXT, 0, f_mutable_list);
static const RecField f_features10[] = {{"features", RK_FEATS, 0, 0, 0, NULL}};
static const RecDesc d_features10 = RDESC(VkPhysicalDeviceFeatures, 0, f_features10);

/* ---- the pipeline's states */
static const RecField f_stage[] = {RNEXT(VkPipelineShaderStageCreateInfo),
                                   RF(VkPipelineShaderStageCreateInfo, flags, RK_U32),
                                   RF(VkPipelineShaderStageCreateInfo, stage, RK_U32),
                                   RF(VkPipelineShaderStageCreateInfo, module, RK_MODULE),
                                   RF(VkPipelineShaderStageCreateInfo, pName, RK_STR),
                                   RFD(VkPipelineShaderStageCreateInfo, pSpecializationInfo, RK_PTR, d_spec)};
static const RecDesc d_stage =
    RDESC(VkPipelineShaderStageCreateInfo, VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, f_stage);
static const RecField f_vertex[] = {
    RNEXT(VkPipelineVertexInputStateCreateInfo), RF(VkPipelineVertexInputStateCreateInfo, flags, RK_U32),
    RFCD(VkPipelineVertexInputStateCreateInfo, pVertexBindingDescriptions, vertexBindingDescriptionCount, d_vbinding),
    RFCD(VkPipelineVertexInputStateCreateInfo, pVertexAttributeDescriptions, vertexAttributeDescriptionCount, d_vattr)};
static const RecDesc d_vertex =
    RDESC(VkPipelineVertexInputStateCreateInfo, VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, f_vertex);
static const RecField f_assembly[] = {RNEXT(VkPipelineInputAssemblyStateCreateInfo),
                                      RF(VkPipelineInputAssemblyStateCreateInfo, flags, RK_U32),
                                      RF(VkPipelineInputAssemblyStateCreateInfo, topology, RK_U32),
                                      RF(VkPipelineInputAssemblyStateCreateInfo, primitiveRestartEnable, RK_U32)};
static const RecDesc d_assembly = RDESC(VkPipelineInputAssemblyStateCreateInfo,
                                        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, f_assembly);
static const RecField f_tess[] = {RNEXT(VkPipelineTessellationStateCreateInfo),
                                  RF(VkPipelineTessellationStateCreateInfo, flags, RK_U32),
                                  RF(VkPipelineTessellationStateCreateInfo, patchControlPoints, RK_U32)};
static const RecDesc d_tess =
    RDESC(VkPipelineTessellationStateCreateInfo, VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO, f_tess);
static const RecField f_view[] = {RNEXT(VkPipelineViewportStateCreateInfo),
                                  RF(VkPipelineViewportStateCreateInfo, flags, RK_U32),
                                  RFCD(VkPipelineViewportStateCreateInfo, pViewports, viewportCount, d_viewport),
                                  RFCD(VkPipelineViewportStateCreateInfo, pScissors, scissorCount, d_rect)};
static const RecDesc d_view =
    RDESC(VkPipelineViewportStateCreateInfo, VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, f_view);
static const RecField f_raster[] = {RNEXT(VkPipelineRasterizationStateCreateInfo),
                                    RF(VkPipelineRasterizationStateCreateInfo, flags, RK_U32),
                                    RF(VkPipelineRasterizationStateCreateInfo, depthClampEnable, RK_U32),
                                    RF(VkPipelineRasterizationStateCreateInfo, rasterizerDiscardEnable, RK_U32),
                                    RF(VkPipelineRasterizationStateCreateInfo, polygonMode, RK_U32),
                                    RF(VkPipelineRasterizationStateCreateInfo, cullMode, RK_U32),
                                    RF(VkPipelineRasterizationStateCreateInfo, frontFace, RK_U32),
                                    RF(VkPipelineRasterizationStateCreateInfo, depthBiasEnable, RK_U32),
                                    RF(VkPipelineRasterizationStateCreateInfo, depthBiasConstantFactor, RK_F32),
                                    RF(VkPipelineRasterizationStateCreateInfo, depthBiasClamp, RK_F32),
                                    RF(VkPipelineRasterizationStateCreateInfo, depthBiasSlopeFactor, RK_F32),
                                    RF(VkPipelineRasterizationStateCreateInfo, lineWidth, RK_F32)};
static const RecDesc d_raster =
    RDESC(VkPipelineRasterizationStateCreateInfo, VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, f_raster);
static const RecField f_multisample[] = {
    RNEXT(VkPipelineMultisampleStateCreateInfo),
    RF(VkPipelineMultisampleStateCreateInfo, flags, RK_U32),
    RF(VkPipelineMultisampleStateCreateInfo, rasterizationSamples, RK_U32),
    RF(VkPipelineMultisampleStateCreateInfo, sampleShadingEnable, RK_U32),
    RF(VkPipelineMultisampleStateCreateInfo, minSampleShading, RK_F32),
    RFC(VkPipelineMultisampleStateCreateInfo, pSampleMask, rasterizationSamples, RK_MASK),
    RF(VkPipelineMultisampleStateCreateInfo, alphaToCoverageEnable, RK_U32),
    RF(VkPipelineMultisampleStateCreateInfo, alphaToOneEnable, RK_U32)};
static const RecDesc d_multisample = RDESC(VkPipelineMultisampleStateCreateInfo,
                                           VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, f_multisample);
static const RecField f_depth[] = {RNEXT(VkPipelineDepthStencilStateCreateInfo),
                                   RF(VkPipelineDepthStencilStateCreateInfo, flags, RK_U32),
                                   RF(VkPipelineDepthStencilStateCreateInfo, depthTestEnable, RK_U32),
                                   RF(VkPipelineDepthStencilStateCreateInfo, depthWriteEnable, RK_U32),
                                   RF(VkPipelineDepthStencilStateCreateInfo, depthCompareOp, RK_U32),
                                   RF(VkPipelineDepthStencilStateCreateInfo, depthBoundsTestEnable, RK_U32),
                                   RF(VkPipelineDepthStencilStateCreateInfo, stencilTestEnable, RK_U32),
                                   RFD(VkPipelineDepthStencilStateCreateInfo, front, RK_SUB, d_stencil),
                                   RFD(VkPipelineDepthStencilStateCreateInfo, back, RK_SUB, d_stencil),
                                   RF(VkPipelineDepthStencilStateCreateInfo, minDepthBounds, RK_F32),
                                   RF(VkPipelineDepthStencilStateCreateInfo, maxDepthBounds, RK_F32)};
static const RecDesc d_depth =
    RDESC(VkPipelineDepthStencilStateCreateInfo, VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, f_depth);
static const RecField f_colorblend[] = {
    RNEXT(VkPipelineColorBlendStateCreateInfo),
    RF(VkPipelineColorBlendStateCreateInfo, flags, RK_U32),
    RF(VkPipelineColorBlendStateCreateInfo, logicOpEnable, RK_U32),
    RF(VkPipelineColorBlendStateCreateInfo, logicOp, RK_U32),
    RFCD(VkPipelineColorBlendStateCreateInfo, pAttachments, attachmentCount, d_blend),
    RFN(VkPipelineColorBlendStateCreateInfo, blendConstants, RK_F32N, 4)};
static const RecDesc d_colorblend =
    RDESC(VkPipelineColorBlendStateCreateInfo, VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, f_colorblend);
static const RecField f_dynamic[] = {RNEXT(VkPipelineDynamicStateCreateInfo),
                                     RF(VkPipelineDynamicStateCreateInfo, flags, RK_U32),
                                     RFC(VkPipelineDynamicStateCreateInfo, pDynamicStates, dynamicStateCount, RK_U32S)};
static const RecDesc d_dynamic =
    RDESC(VkPipelineDynamicStateCreateInfo, VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, f_dynamic);
static const RecField f_graphics[] = {RNEXT(VkGraphicsPipelineCreateInfo),
                                      RF(VkGraphicsPipelineCreateInfo, flags, RK_U32),
                                      RFCD(VkGraphicsPipelineCreateInfo, pStages, stageCount, d_stage),
                                      RFD(VkGraphicsPipelineCreateInfo, pVertexInputState, RK_PTR, d_vertex),
                                      RFD(VkGraphicsPipelineCreateInfo, pInputAssemblyState, RK_PTR, d_assembly),
                                      RFD(VkGraphicsPipelineCreateInfo, pTessellationState, RK_PTR, d_tess),
                                      RFD(VkGraphicsPipelineCreateInfo, pViewportState, RK_PTR, d_view),
                                      RFD(VkGraphicsPipelineCreateInfo, pRasterizationState, RK_PTR, d_raster),
                                      RFD(VkGraphicsPipelineCreateInfo, pMultisampleState, RK_PTR, d_multisample),
                                      RFD(VkGraphicsPipelineCreateInfo, pDepthStencilState, RK_PTR, d_depth),
                                      RFD(VkGraphicsPipelineCreateInfo, pColorBlendState, RK_PTR, d_colorblend),
                                      RFD(VkGraphicsPipelineCreateInfo, pDynamicState, RK_PTR, d_dynamic),
                                      RF(VkGraphicsPipelineCreateInfo, layout, RK_LAYOUT),
                                      RF(VkGraphicsPipelineCreateInfo, renderPass, RK_NULLH),
                                      RF(VkGraphicsPipelineCreateInfo, subpass, RK_U32),
                                      RF(VkGraphicsPipelineCreateInfo, basePipelineHandle, RK_NULLH),
                                      RF(VkGraphicsPipelineCreateInfo, basePipelineIndex, RK_I32)};
static const RecDesc d_graphics =
    RDESC(VkGraphicsPipelineCreateInfo, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, f_graphics);

/* ---- the objects a pipeline names */
static const RecField f_sampler[] = {RNEXT(VkSamplerCreateInfo),
                                     RF(VkSamplerCreateInfo, flags, RK_U32),
                                     RF(VkSamplerCreateInfo, magFilter, RK_U32),
                                     RF(VkSamplerCreateInfo, minFilter, RK_U32),
                                     RF(VkSamplerCreateInfo, mipmapMode, RK_U32),
                                     RF(VkSamplerCreateInfo, addressModeU, RK_U32),
                                     RF(VkSamplerCreateInfo, addressModeV, RK_U32),
                                     RF(VkSamplerCreateInfo, addressModeW, RK_U32),
                                     RF(VkSamplerCreateInfo, mipLodBias, RK_F32),
                                     RF(VkSamplerCreateInfo, anisotropyEnable, RK_U32),
                                     RF(VkSamplerCreateInfo, maxAnisotropy, RK_F32),
                                     RF(VkSamplerCreateInfo, compareEnable, RK_U32),
                                     RF(VkSamplerCreateInfo, compareOp, RK_U32),
                                     RF(VkSamplerCreateInfo, minLod, RK_F32),
                                     RF(VkSamplerCreateInfo, maxLod, RK_F32),
                                     RF(VkSamplerCreateInfo, borderColor, RK_U32),
                                     RF(VkSamplerCreateInfo, unnormalizedCoordinates, RK_U32)};
static const RecDesc d_sampler = RDESC(VkSamplerCreateInfo, VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, f_sampler);
static const RecField f_set_layout[] = {RNEXT(VkDescriptorSetLayoutCreateInfo),
                                        RF(VkDescriptorSetLayoutCreateInfo, flags, RK_U32),
                                        RFCD(VkDescriptorSetLayoutCreateInfo, pBindings, bindingCount, d_set_binding)};
static const RecDesc d_set_layout =
    RDESC(VkDescriptorSetLayoutCreateInfo, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, f_set_layout);
static const RecField f_layout[] = {
    RNEXT(VkPipelineLayoutCreateInfo), RF(VkPipelineLayoutCreateInfo, flags, RK_U32),
    RFC(VkPipelineLayoutCreateInfo, pSetLayouts, setLayoutCount, RK_SETLAYOUTS),
    RFCD(VkPipelineLayoutCreateInfo, pPushConstantRanges, pushConstantRangeCount, d_push)};
static const RecDesc d_layout =
    RDESC(VkPipelineLayoutCreateInfo, VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, f_layout);
static const RecField f_queue[] = {RNEXT(VkDeviceQueueCreateInfo), RF(VkDeviceQueueCreateInfo, flags, RK_U32),
                                   RF(VkDeviceQueueCreateInfo, queueFamilyIndex, RK_U32),
                                   RFC(VkDeviceQueueCreateInfo, pQueuePriorities, queueCount, RK_F32S)};
static const RecDesc d_queue = RDESC(VkDeviceQueueCreateInfo, VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, f_queue);
static const RecField f_device[] = {RNEXT(VkDeviceCreateInfo),
                                    RF(VkDeviceCreateInfo, flags, RK_U32),
                                    RFCD(VkDeviceCreateInfo, pQueueCreateInfos, queueCreateInfoCount, d_queue),
                                    RFC(VkDeviceCreateInfo, ppEnabledLayerNames, enabledLayerCount, RK_STRS),
                                    RFC(VkDeviceCreateInfo, ppEnabledExtensionNames, enabledExtensionCount, RK_STRS),
                                    RFD(VkDeviceCreateInfo, pEnabledFeatures, RK_PTR, d_features10)};
static const RecDesc d_device = RDESC(VkDeviceCreateInfo, VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, f_device);

/* ---- what pNext chains carry */
#define RNEXT_DESC(T, st, ...)                                                                                         \
    static const RecField f_##T[] = {__VA_ARGS__};                                                                     \
    static const RecDesc d_##T = RDESC(T, st, f_##T)
RNEXT_DESC(VkPipelineRenderingCreateInfo, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
           RF(VkPipelineRenderingCreateInfo, viewMask, RK_U32),
           RFC(VkPipelineRenderingCreateInfo, pColorAttachmentFormats, colorAttachmentCount, RK_U32S),
           RF(VkPipelineRenderingCreateInfo, depthAttachmentFormat, RK_U32),
           RF(VkPipelineRenderingCreateInfo, stencilAttachmentFormat, RK_U32));
RNEXT_DESC(VkPipelineRobustnessCreateInfo, VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO,
           RF(VkPipelineRobustnessCreateInfo, storageBuffers, RK_U32),
           RF(VkPipelineRobustnessCreateInfo, uniformBuffers, RK_U32),
           RF(VkPipelineRobustnessCreateInfo, vertexInputs, RK_U32),
           RF(VkPipelineRobustnessCreateInfo, images, RK_U32));
RNEXT_DESC(VkAttachmentSampleCountInfoAMD, VK_STRUCTURE_TYPE_ATTACHMENT_SAMPLE_COUNT_INFO_AMD,
           RFC(VkAttachmentSampleCountInfoAMD, pColorAttachmentSamples, colorAttachmentCount, RK_U32S),
           RF(VkAttachmentSampleCountInfoAMD, depthStencilAttachmentSamples, RK_U32));
RNEXT_DESC(VkGraphicsPipelineLibraryCreateInfoEXT, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
           RF(VkGraphicsPipelineLibraryCreateInfoEXT, flags, RK_U32));
RNEXT_DESC(VkPipelineLibraryCreateInfoKHR, VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
           RFC(VkPipelineLibraryCreateInfoKHR, pLibraries, libraryCount, RK_LIBS));
RNEXT_DESC(VkPipelineCoverageToColorStateCreateInfoNV,
           VK_STRUCTURE_TYPE_PIPELINE_COVERAGE_TO_COLOR_STATE_CREATE_INFO_NV,
           RF(VkPipelineCoverageToColorStateCreateInfoNV, flags, RK_U32),
           RF(VkPipelineCoverageToColorStateCreateInfoNV, coverageToColorEnable, RK_U32),
           RF(VkPipelineCoverageToColorStateCreateInfoNV, coverageToColorLocation, RK_U32));
RNEXT_DESC(VkPipelineCoverageModulationStateCreateInfoNV,
           VK_STRUCTURE_TYPE_PIPELINE_COVERAGE_MODULATION_STATE_CREATE_INFO_NV,
           RF(VkPipelineCoverageModulationStateCreateInfoNV, flags, RK_U32),
           RF(VkPipelineCoverageModulationStateCreateInfoNV, coverageModulationMode, RK_U32),
           RF(VkPipelineCoverageModulationStateCreateInfoNV, coverageModulationTableEnable, RK_U32),
           RFC(VkPipelineCoverageModulationStateCreateInfoNV, pCoverageModulationTable, coverageModulationTableCount,
               RK_F32S));
RNEXT_DESC(VkSampleLocationsInfoEXT, VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT, RNEXT(VkSampleLocationsInfoEXT),
           RF(VkSampleLocationsInfoEXT, sampleLocationsPerPixel, RK_U32),
           RF(VkSampleLocationsInfoEXT, sampleLocationGridSize.width, RK_U32),
           RF(VkSampleLocationsInfoEXT, sampleLocationGridSize.height, RK_U32),
           RFCD(VkSampleLocationsInfoEXT, pSampleLocations, sampleLocationsCount, d_location));
RNEXT_DESC(VkPipelineSampleLocationsStateCreateInfoEXT,
           VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT,
           RF(VkPipelineSampleLocationsStateCreateInfoEXT, sampleLocationsEnable, RK_U32),
           RFD(VkPipelineSampleLocationsStateCreateInfoEXT, sampleLocationsInfo, RK_SUB, d_VkSampleLocationsInfoEXT));
RNEXT_DESC(VkPipelineColorBlendAdvancedStateCreateInfoEXT,
           VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_ADVANCED_STATE_CREATE_INFO_EXT,
           RF(VkPipelineColorBlendAdvancedStateCreateInfoEXT, srcPremultiplied, RK_U32),
           RF(VkPipelineColorBlendAdvancedStateCreateInfoEXT, dstPremultiplied, RK_U32),
           RF(VkPipelineColorBlendAdvancedStateCreateInfoEXT, blendOverlap, RK_U32));
RNEXT_DESC(VkPipelineRasterizationProvokingVertexStateCreateInfoEXT,
           VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT,
           RF(VkPipelineRasterizationProvokingVertexStateCreateInfoEXT, provokingVertexMode, RK_U32));
RNEXT_DESC(VkPipelineRasterizationDepthClipStateCreateInfoEXT,
           VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT,
           RF(VkPipelineRasterizationDepthClipStateCreateInfoEXT, flags, RK_U32),
           RF(VkPipelineRasterizationDepthClipStateCreateInfoEXT, depthClipEnable, RK_U32));
RNEXT_DESC(VkPipelineRasterizationConservativeStateCreateInfoEXT,
           VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT,
           RF(VkPipelineRasterizationConservativeStateCreateInfoEXT, flags, RK_U32),
           RF(VkPipelineRasterizationConservativeStateCreateInfoEXT, conservativeRasterizationMode, RK_U32),
           RF(VkPipelineRasterizationConservativeStateCreateInfoEXT, extraPrimitiveOverestimationSize, RK_F32));
RNEXT_DESC(VkPipelineVertexInputDivisorStateCreateInfo,
           VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO,
           RFCD(VkPipelineVertexInputDivisorStateCreateInfo, pVertexBindingDivisors, vertexBindingDivisorCount,
                d_divisor));
RNEXT_DESC(VkPipelineShaderStageRequiredSubgroupSizeCreateInfo,
           VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO,
           RF(VkPipelineShaderStageRequiredSubgroupSizeCreateInfo, requiredSubgroupSize, RK_U32));
RNEXT_DESC(VkPipelineCreateFlags2CreateInfo, VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
           RF(VkPipelineCreateFlags2CreateInfo, flags, RK_U64));
RNEXT_DESC(VkPipelineCreationFeedbackCreateInfo, VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
           RF(VkPipelineCreationFeedbackCreateInfo, pPipelineCreationFeedback, RK_FEEDBACK1),
           RF(VkPipelineCreationFeedbackCreateInfo, pipelineStageCreationFeedbackCount, RK_U32),
           RFC(VkPipelineCreationFeedbackCreateInfo, pPipelineStageCreationFeedbacks,
               pipelineStageCreationFeedbackCount, RK_FEEDBACKN));
RNEXT_DESC(VkShaderModuleCreateInfo, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
           RF(VkShaderModuleCreateInfo, flags, RK_U32), RFC(VkShaderModuleCreateInfo, pCode, codeSize, RK_CODE));
RNEXT_DESC(VkDescriptorSetLayoutBindingFlagsCreateInfo,
           VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
           RFC(VkDescriptorSetLayoutBindingFlagsCreateInfo, pBindingFlags, bindingCount, RK_U32S));
RNEXT_DESC(VkMutableDescriptorTypeCreateInfoEXT, VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT,
           RFCD(VkMutableDescriptorTypeCreateInfoEXT, pMutableDescriptorTypeLists, mutableDescriptorTypeListCount,
                d_mutable_list));
RNEXT_DESC(VkSamplerReductionModeCreateInfo, VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
           RF(VkSamplerReductionModeCreateInfo, reductionMode, RK_U32));
RNEXT_DESC(VkSamplerCustomBorderColorCreateInfoEXT, VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT,
           RFN(VkSamplerCustomBorderColorCreateInfoEXT, customBorderColor, RK_U32N, 4),
           RF(VkSamplerCustomBorderColorCreateInfoEXT, format, RK_U32));
RNEXT_DESC(VkDeviceDiagnosticsConfigCreateInfoNV, VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV,
           RF(VkDeviceDiagnosticsConfigCreateInfoNV, flags, RK_U32));
RNEXT_DESC(VkDeviceQueueGlobalPriorityCreateInfo, VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO,
           RF(VkDeviceQueueGlobalPriorityCreateInfo, globalPriority, RK_U32));

/* the device's features structures: VkBool32s after sType and pNext */
static const RecField f_features[] = {{"features", RK_FEATS, 0, (unsigned short)sizeof(VkBaseOutStructure), 0, NULL}};
#define FEAT(st, T) {#T, VK_STRUCTURE_TYPE_##st, (unsigned short)sizeof(T), 1, f_features}
static const RecDesc d_features[] = {
    FEAT(PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_FEATURES_EXT,
         VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT),
    FEAT(PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT,
         VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT),
    FEAT(PHYSICAL_DEVICE_BLEND_OPERATION_ADVANCED_FEATURES_EXT, VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT),
    FEAT(PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT, VkPhysicalDeviceConditionalRenderingFeaturesEXT),
    FEAT(PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR, VkPhysicalDeviceCooperativeMatrixFeaturesKHR),
    FEAT(PHYSICAL_DEVICE_COPY_MEMORY_INDIRECT_FEATURES_KHR, VkPhysicalDeviceCopyMemoryIndirectFeaturesKHR),
    FEAT(PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, VkPhysicalDeviceCustomBorderColorFeaturesEXT),
    FEAT(PHYSICAL_DEVICE_DEPTH_CLAMP_CONTROL_FEATURES_EXT, VkPhysicalDeviceDepthClampControlFeaturesEXT),
    FEAT(PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT, VkPhysicalDeviceDepthClipControlFeaturesEXT),
    FEAT(PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT, VkPhysicalDeviceDepthClipEnableFeaturesEXT),
    FEAT(PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT, VkPhysicalDeviceDescriptorBufferFeaturesEXT),
    FEAT(PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT, VkPhysicalDeviceDescriptorHeapFeaturesEXT),
    FEAT(PHYSICAL_DEVICE_DIAGNOSTICS_CONFIG_FEATURES_NV, VkPhysicalDeviceDiagnosticsConfigFeaturesNV),
    FEAT(PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, VkPhysicalDeviceDynamicRenderingFeatures),
    FEAT(PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR, VkPhysicalDeviceDynamicRenderingLocalReadFeatures),
    FEAT(PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT,
         VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT),
    FEAT(PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT, VkPhysicalDeviceExtendedDynamicState2FeaturesEXT),
    FEAT(PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT, VkPhysicalDeviceExtendedDynamicState3FeaturesEXT),
    FEAT(PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT, VkPhysicalDeviceExtendedDynamicStateFeaturesEXT),
    FEAT(PHYSICAL_DEVICE_EXTENDED_FLAGS_FEATURES_KHR, VkPhysicalDeviceExtendedFlagsFeaturesKHR),
    FEAT(PHYSICAL_DEVICE_FAULT_FEATURES_EXT, VkPhysicalDeviceFaultFeaturesEXT),
    FEAT(PHYSICAL_DEVICE_FEATURES_2, VkPhysicalDeviceFeatures2),
    FEAT(PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR,
         VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR),
    FEAT(PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT, VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT),
    FEAT(PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR, VkPhysicalDeviceFragmentShadingRateFeaturesKHR),
    FEAT(PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT),
    FEAT(PHYSICAL_DEVICE_IMAGE_VIEW_MIN_LOD_FEATURES_EXT, VkPhysicalDeviceImageViewMinLodFeaturesEXT),
    FEAT(PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_KHR, VkPhysicalDeviceIndexTypeUint8Features),
    FEAT(PHYSICAL_DEVICE_LEGACY_VERTEX_ATTRIBUTES_FEATURES_EXT, VkPhysicalDeviceLegacyVertexAttributesFeaturesEXT),
    FEAT(PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES, VkPhysicalDeviceMaintenance4Features),
    FEAT(PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, VkPhysicalDeviceMaintenance5Features),
    FEAT(PHYSICAL_DEVICE_MAINTENANCE_8_FEATURES_KHR, VkPhysicalDeviceMaintenance8FeaturesKHR),
    FEAT(PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT, VkPhysicalDeviceMapMemoryPlacedFeaturesEXT),
    FEAT(PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT, VkPhysicalDeviceMemoryPriorityFeaturesEXT),
    FEAT(PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT, VkPhysicalDeviceMeshShaderFeaturesEXT),
    FEAT(PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT, VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT),
    FEAT(PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT,
         VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT),
    FEAT(PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR,
         VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR),
    FEAT(PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT, VkPhysicalDevicePipelineRobustnessFeatures),
    FEAT(PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR, VkPhysicalDevicePresentId2FeaturesKHR),
    FEAT(PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, VkPhysicalDevicePresentIdFeaturesKHR),
    FEAT(PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT, VkPhysicalDevicePresentTimingFeaturesEXT),
    FEAT(PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR, VkPhysicalDevicePresentWait2FeaturesKHR),
    FEAT(PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR, VkPhysicalDevicePresentWaitFeaturesKHR),
    FEAT(PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT,
         VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT),
    FEAT(PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT, VkPhysicalDeviceProvokingVertexFeaturesEXT),
    FEAT(PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, VkPhysicalDeviceRobustness2FeaturesKHR),
    FEAT(PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT, VkPhysicalDeviceShaderAtomicFloatFeaturesEXT),
    FEAT(PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR, VkPhysicalDeviceShaderClockFeaturesKHR),
    FEAT(PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES,
         VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures),
    FEAT(PHYSICAL_DEVICE_SHADER_MAXIMAL_RECONVERGENCE_FEATURES_KHR,
         VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR),
    FEAT(PHYSICAL_DEVICE_SHADER_MIXED_FLOAT_DOT_PRODUCT_FEATURES_VALVE,
         VkPhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE),
    FEAT(PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT, VkPhysicalDeviceShaderObjectFeaturesEXT),
    FEAT(PHYSICAL_DEVICE_SHADING_RATE_IMAGE_FEATURES_NV, VkPhysicalDeviceShadingRateImageFeaturesNV),
    FEAT(PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES, VkPhysicalDeviceSubgroupSizeControlFeatures),
    FEAT(PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, VkPhysicalDeviceSynchronization2Features),
    FEAT(PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, VkPhysicalDeviceTransformFeedbackFeaturesEXT),
    FEAT(PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR, VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR),
    FEAT(PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT, VkPhysicalDeviceVertexAttributeDivisorFeatures),
    FEAT(PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT, VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT),
    FEAT(PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, VkPhysicalDeviceVulkan11Features),
    FEAT(PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, VkPhysicalDeviceVulkan12Features),
    FEAT(PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, VkPhysicalDeviceVulkan13Features)};
#undef FEAT

/* every structure a pNext chain may carry */
static const RecDesc* const rec_next_list[] = {&d_VkPipelineRenderingCreateInfo,
                                               &d_VkPipelineRobustnessCreateInfo,
                                               &d_VkAttachmentSampleCountInfoAMD,
                                               &d_VkGraphicsPipelineLibraryCreateInfoEXT,
                                               &d_VkPipelineLibraryCreateInfoKHR,
                                               &d_VkPipelineCoverageToColorStateCreateInfoNV,
                                               &d_VkPipelineCoverageModulationStateCreateInfoNV,
                                               &d_VkPipelineSampleLocationsStateCreateInfoEXT,
                                               &d_VkPipelineColorBlendAdvancedStateCreateInfoEXT,
                                               &d_VkPipelineRasterizationProvokingVertexStateCreateInfoEXT,
                                               &d_VkPipelineRasterizationDepthClipStateCreateInfoEXT,
                                               &d_VkPipelineRasterizationConservativeStateCreateInfoEXT,
                                               &d_VkPipelineVertexInputDivisorStateCreateInfo,
                                               &d_VkPipelineShaderStageRequiredSubgroupSizeCreateInfo,
                                               &d_VkPipelineCreateFlags2CreateInfo,
                                               &d_VkPipelineCreationFeedbackCreateInfo,
                                               &d_VkShaderModuleCreateInfo,
                                               &d_VkDescriptorSetLayoutBindingFlagsCreateInfo,
                                               &d_VkMutableDescriptorTypeCreateInfoEXT,
                                               &d_VkSamplerReductionModeCreateInfo,
                                               &d_VkSamplerCustomBorderColorCreateInfoEXT,
                                               &d_VkDeviceDiagnosticsConfigCreateInfoNV,
                                               &d_VkDeviceQueueGlobalPriorityCreateInfo};
/* the chain's structure of sType, else NULL */
static const RecDesc* rec_next_desc(VkStructureType stype) {
    for (size_t i = 0; i < sizeof rec_next_list / sizeof *rec_next_list; i++)
        if (rec_next_list[i]->stype == stype) return rec_next_list[i];
    for (size_t i = 0; i < sizeof d_features / sizeof *d_features; i++)
        if (d_features[i].stype == stype) return &d_features[i];
    return NULL;
}
/* the same by its name, as a record writes it */
static const RecDesc* rec_next_named(const char* name) {
    for (size_t i = 0; i < sizeof rec_next_list / sizeof *rec_next_list; i++)
        if (!strcmp(rec_next_list[i]->name, name)) return rec_next_list[i];
    for (size_t i = 0; i < sizeof d_features / sizeof *d_features; i++)
        if (!strcmp(d_features[i].name, name)) return &d_features[i];
    return NULL;
}
