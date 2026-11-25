/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/gvk/gvk.h"
#include "nx/gpu/gvk/gvk_query.h"
#include "nx/gpu/gvk/gvk_rect.h"
#include "nx/nn/hook.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/guest_jit.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/nce/nce.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include <vulkan/vulkan_core.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PAGE = 4096, ROOT = 0x1000, NODE = 0x2000, EXTRA = 0x2040, UNMAPPED = 0x3000 };
static union {
    long double align;
    u8 bytes[4 * PAGE];
} g_guest;
u8* g_mem = g_guest.bytes;
u64 g_as_size = sizeof g_guest.bytes;
MemLayout g_layout = {.end = sizeof g_guest.bytes};
static u32 g_node_perm = PERM_RW;
static u32 g_proc_perm;

u64 plat_vm_page(void) { return PAGE; }

int mem_map(u64 addr, u64 size, u32 state, u32 perm) {
    assert(addr == UNMAPPED && size == PAGE && state == MS_CODE && perm == PERM_RW && !g_proc_perm);
    g_proc_perm = perm;
    return 0;
}
int mem_set_state(u64 addr, u64 size, u32 state, u32 perm) {
    assert(addr == UNMAPPED && size == PAGE && state == MS_CODE && perm == PERM_RX && g_proc_perm == PERM_RW);
    g_proc_perm = perm;
    return 0;
}

int mem_user_mapped(u64 addr, u64 bytes) { return addr >= ROOT && addr < UNMAPPED && bytes <= UNMAPPED - addr; }
void mem_query(u64 addr, MemBlock* out) {
    *out = (MemBlock){.addr = addr & ~(u64)(PAGE - 1), .size = PAGE};
    if (mem_user_mapped(addr, 1)) {
        out->state = MS_NORMAL;
        out->perm = addr < NODE ? PERM_RW : g_node_perm;
    }
    if (addr >= UNMAPPED && addr < UNMAPPED + PAGE && g_proc_perm) {
        out->state = MS_CODE;
        out->perm = g_proc_perm;
    }
}
int mem_user_string(char* dst, u64 addr, u64 cap) {
    for (u64 i = 0; i < cap && mem_user_mapped(addr + i, 1); i++)
        if (!(dst[i] = (char)g_mem[addr + i])) return (int)(i + 1);
    return -1;
}
static char g_said[512]; /* the last line logged */
void logf_(int lvl, const char* fmt, ...) {
    va_list args;
    (void)lvl;
    va_start(args, fmt);
    vsnprintf(g_said, sizeof g_said, fmt, args);
    va_end(args);
}
void fatal(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    abort();
}

static const SnxModInfo g_modules[] = {
    {.name = "application", .base = ROOT, .size = PAGE},
    {.name = "driver", .base = NODE, .size = PAGE},
};
static const SnxModule g_module = {.nmods = 2, .mods = g_modules};
const SnxModule* g_mod = &g_module;
const SnxArmModule* g_arm_mod;
u64 g_code_lo = ROOT, g_code_hi = UNMAPPED;
static _Atomic GuestFn g_slots[(sizeof g_guest.bytes - ROOT) / 4];
_Atomic GuestFn* g_disp = g_slots;
static const struct {
    u64 addr;
    const char* name;
} g_symbols[] = {
    {ROOT + 4, "vkQueueSubmit"},  {NODE + 4, "vkGetInstanceProcAddr"}, {NODE + 6, "vkMisaligned"},
    {NODE + 8, "vkPrivateAlias"}, {NODE + 8, "vkQueueSubmit"},         {NODE + 12, "notVulkan"},
};
enum { PRIVATE_EXPORTS = 520, PRIVATE_START = NODE + 64 };
static char g_private[PRIVATE_EXPORTS][24];
u64 kernel_find_symbol_exact(const char* name) {
    for (size_t i = 0; i < sizeof g_symbols / sizeof *g_symbols; i++)
        if (!strcmp(name, g_symbols[i].name)) return g_symbols[i].addr;
    return 0;
}
int kernel_symbol(u32 i, u64* addr, const char** name) {
    u32 base = sizeof g_symbols / sizeof *g_symbols;
    if (i < base) {
        *addr = g_symbols[i].addr;
        *name = g_symbols[i].name;
    } else if (i - base < PRIVATE_EXPORTS) {
        *addr = PRIVATE_START + 4 * (i - base);
        *name = g_private[i - base];
    } else if (i - base == PRIVATE_EXPORTS) {
        *addr = UNMAPPED;
        *name = "vkCreateInstance";
    } else {
        return 0;
    }
    return 1;
}
int hook_routed(const char* name) {
    return strncmp(name, "vkPrivate", 9) == 0 || !strcmp(name, "vkGetInstanceProcAddr") ||
           !strcmp(name, "vkQueueSubmit");
}
int nce_wanted(void) { return 0; }
static u64 g_nce_refused; /* the function whose entry nce_slot_taken can't divert, 0 none */
int nce_slot_taken(u64 addr, const char* family) {
    (void)family;
    return addr == g_nce_refused ? -1 : 0;
}
void jit_slot_taken(u64 addr, GuestFn fn) {
    (void)addr;
    (void)fn;
}
static void submit(CPU* c, u64 entry) { c->pc = entry; }
static void orig_submit(CPU* c, u64 entry) { c->pc = entry; }
static int g_supported = 1;
GuestFn gvk_api_find(const char* name) {
    return !strcmp(name, "vkQueueSubmit") || !strcmp(name, "vkCreateDebugReportCallbackEXT") ? submit : NULL;
}
int gvk_api_supported(void) { return g_supported; }

static void marshal_submit(void) {
    VkSubmitInfo s = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                      .commandBufferCount = 1,
                      .pCommandBuffers = (const VkCommandBuffer*)(uintptr_t)NODE};
    memcpy(GP(ROOT), &s, sizeof s);
    gw64(NODE, 0x12345678);
    const VkSubmitInfo* host = gvk_in(&gvk_t_submit, ROOT, 1);
    assert(host && host->commandBufferCount == 1);
    assert(host->pCommandBuffers == (const void*)GP(NODE));
    assert((uintptr_t)host->pCommandBuffers[0] == 0x12345678);
    assert(!gvk_in(&gvk_t_submit, ROOT, UINT64_MAX));
    assert(!gvk_in(&gvk_t_submit, UNMAPPED, 1));
    gw32(ROOT + offsetof(VkSubmitInfo, commandBufferCount), UINT32_MAX);
    assert(!gvk_in(&gvk_t_submit, ROOT, 1));
    gw32(ROOT + offsetof(VkSubmitInfo, commandBufferCount), 1);
    gw64(ROOT + offsetof(VkSubmitInfo, pCommandBuffers), UNMAPPED);
    assert(!gvk_in(&gvk_t_submit, ROOT, 1));
    gw64(ROOT + offsetof(VkSubmitInfo, pCommandBuffers), 0);
    assert(!gvk_in(&gvk_t_submit, ROOT, 1));
}

static void marshal_chain(void) {
    VkMemoryRequirements2 root = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = (void*)(uintptr_t)NODE};
    VkMemoryDedicatedRequirements ext = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    memcpy(GP(ROOT), &root, sizeof root);
    gw32(NODE, 0x7fffffff);
    gw64(NODE + 8, EXTRA);
    memcpy(GP(EXTRA), &ext, sizeof ext);
    VkMemoryRequirements2* host = gvk_in(&gvk_t_req2, ROOT, 1);
    assert(host && host->pNext);
    VkMemoryDedicatedRequirements* h = host->pNext;
    assert(h->sType == ext.sType && !h->pNext);
    h->requiresDedicatedAllocation = VK_TRUE;
    host->memoryRequirements.size = 256;
    gvk_out(&gvk_t_req2, ROOT, host);
    assert(gr64(ROOT + 8) == NODE && gr64(NODE + 8) == EXTRA && !gr64(EXTRA + 8));
    assert(gr64(ROOT + offsetof(VkMemoryRequirements2, memoryRequirements)) == 256);
    assert(gr32(EXTRA + offsetof(VkMemoryDedicatedRequirements, requiresDedicatedAllocation)) == VK_TRUE);
    gw64(EXTRA + 8, NODE);
    assert(!gvk_in(&gvk_t_req2, ROOT, 1));
    gw64(EXTRA + 8, UNMAPPED);
    assert(!gvk_in(&gvk_t_req2, ROOT, 1));

    g_node_perm = PERM_R;
    assert(!gvk_writable(NODE - 4, 8));
    assert(!gvk_writable(UNMAPPED - 4, 8));
    assert(gvk_writable(ROOT, sizeof root));
    g_node_perm = PERM_RW;
}

static void marshal_sample_mask(void) {
    u64 mask = UNMAPPED - sizeof(VkSampleMask);
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pSampleMask = (const VkSampleMask*)(uintptr_t)mask,
        .rasterizationSamples = VK_SAMPLE_COUNT_32_BIT,
    };
    VkGraphicsPipelineCreateInfo pipe = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pMultisampleState = (const VkPipelineMultisampleStateCreateInfo*)(uintptr_t)NODE,
    };
    memcpy(GP(ROOT), &pipe, sizeof pipe);
    memcpy(GP(NODE), &ms, sizeof ms);
    gw32(mask, 0x87654321);
    const VkGraphicsPipelineCreateInfo* host = gvk_in(&gvk_t_gpipe, ROOT, 1);
    assert(host && host->pMultisampleState && *host->pMultisampleState->pSampleMask == 0x87654321);
    gw32(NODE + offsetof(VkPipelineMultisampleStateCreateInfo, rasterizationSamples), VK_SAMPLE_COUNT_64_BIT);
    assert(!gvk_in(&gvk_t_gpipe, ROOT, 1));
}

static void install_driver(void) {
    for (u32 i = 0; i < PRIVATE_EXPORTS; i++) snprintf(g_private[i], sizeof *g_private, "vkPrivate%u", i);
    /* with the option off, and for a 32-bit game, the slots and the space stay as they are */
    assert(gvk_proc_space(g_code_hi) == g_code_hi);
    gvk_install();
    assert(!gvk_active());
    g_settings.guest_vulkan = 1;
    g_arm_mod = (const SnxArmModule*)(const void*)&g_module;
    assert(gvk_proc_space(g_code_hi) == g_code_hi);
    gvk_install();
    assert(!gvk_active());
    g_arm_mod = NULL;
    /* past the end of the modules, however small the code is */
    assert(gvk_proc_space(NODE) == UNMAPPED + PAGE);
    g_slots[(NODE + 8 - ROOT) / 4] = orig_submit;
    g_code_hi = gvk_proc_space(g_code_hi);
    gvk_install();
    assert(gvk_active());
    assert(gvk_export("vkQueueSubmit") == NODE + 8);
    assert(g_slots[(NODE + 8 - ROOT) / 4] == submit);
    assert(!gvk_export("vkMisaligned"));
    assert(!g_slots[1] && !g_slots[(NODE + 12 - ROOT) / 4]);
    assert(!gvk_export("vkCreateInstance"));
    assert(gvk_export(g_private[PRIVATE_EXPORTS - 1]) == PRIVATE_START + 4 * (PRIVATE_EXPORTS - 1));
    assert(g_slots[(PRIVATE_START - ROOT) / 4]);
    assert(!gvk_invalidation_slot(NODE + 8));
    g_node_perm = PERM_RX;
    assert(gvk_invalidation_slot(NODE + 8) == submit);
    assert(!gvk_invalidation_slot(ROOT + 4) && !gvk_invalidation_slot(NODE + 12));
    u64 proc = gvk_export("vkCreateDebugReportCallbackEXT");
    assert(proc == UNMAPPED && g_slots[(proc - ROOT) / 4] == submit);
    assert(gvk_invalidation_slot(proc) == submit);
    g_proc_perm = PERM_R;
    assert(!gvk_invalidation_slot(proc));
    assert(!gvk_export("vkDebugReportMessageEXT"));
    g_supported = 1;
    gvk_check_device();
    assert(gvk_active() && g_slots[(NODE + 8 - ROOT) / 4] == submit);
    g_supported = 0;
    gvk_check_device();
    assert(!gvk_active());
    assert(g_slots[(NODE + 8 - ROOT) / 4] == orig_submit);
    assert(!g_slots[(proc - ROOT) / 4]);
    assert(!gvk_invalidation_slot(NODE + 8));

    /* one function that native code keeps: no slot stays taken, so the driver never sees the host's handles */
    g_nce_refused = NODE + 8;
    gvk_install();
    assert(!gvk_active());
    assert(g_slots[(NODE + 8 - ROOT) / 4] == orig_submit);
    assert(!gvk_export("vkQueueSubmit"));
    g_nce_refused = 0;
}

static void marshal_optional_one(void) {
    /* Uninitialized / unmapped pInheritanceInfo and pTessellationState must be cleared to NULL, not fail gvk_in. */
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pInheritanceInfo = (const VkCommandBufferInheritanceInfo*)(uintptr_t)UNMAPPED,
    };
    memcpy(GP(ROOT), &bi, sizeof bi);
    const VkCommandBufferBeginInfo* hbi = gvk_in(&gvk_t_begin, ROOT, 1);
    assert(hbi && !hbi->pInheritanceInfo);

    VkGraphicsPipelineCreateInfo pipe = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pTessellationState = (const VkPipelineTessellationStateCreateInfo*)(uintptr_t)UNMAPPED,
    };
    memcpy(GP(ROOT), &pipe, sizeof pipe);
    const VkGraphicsPipelineCreateInfo* hp = gvk_in(&gvk_t_gpipe, ROOT, 1);
    assert(hp && !hp->pTessellationState);
}

static void marshal_ignored_state(void) {
    /* rasterizerDiscardEnable: the four states Vulkan ignores can be any pointer, or zeros that no state is */
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .rasterizerDiscardEnable = VK_TRUE};
    VkPipelineMultisampleStateCreateInfo zeros = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pipe = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pRasterizationState = (const VkPipelineRasterizationStateCreateInfo*)(uintptr_t)NODE,
        .pMultisampleState = (const VkPipelineMultisampleStateCreateInfo*)(uintptr_t)(UNMAPPED + 64),
        .pColorBlendState = (const VkPipelineColorBlendStateCreateInfo*)(uintptr_t)(UNMAPPED + 64)};
    memcpy(GP(ROOT), &pipe, sizeof pipe);
    memcpy(GP(NODE), &rs, sizeof rs);
    const VkGraphicsPipelineCreateInfo* h = gvk_in(&gvk_t_gpipe, ROOT, 1);
    assert(h && h->pRasterizationState && !h->pMultisampleState && !h->pColorBlendState);
    pipe.pMultisampleState = (const VkPipelineMultisampleStateCreateInfo*)(uintptr_t)(NODE + 0x200);
    memcpy(GP(ROOT), &pipe, sizeof pipe);
    memcpy(GP(NODE + 0x200), &zeros, sizeof zeros);
    assert(gvk_in(&gvk_t_gpipe, ROOT, 1));
}

static void marshal_core_chains(void) {
    /* structures of core 1.1 that no feature guards: the host is given them */
    VkPipelineTessellationDomainOriginStateCreateInfo origin = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO,
        .domainOrigin = VK_TESSELLATION_DOMAIN_ORIGIN_LOWER_LEFT};
    VkPipelineTessellationStateCreateInfo tess = {.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
                                                  .pNext = (const void*)(uintptr_t)EXTRA,
                                                  .patchControlPoints = 3};
    VkGraphicsPipelineCreateInfo pipe = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                         .pTessellationState =
                                             (const VkPipelineTessellationStateCreateInfo*)(uintptr_t)NODE};
    memcpy(GP(ROOT), &pipe, sizeof pipe);
    memcpy(GP(NODE), &tess, sizeof tess);
    memcpy(GP(EXTRA), &origin, sizeof origin);
    const VkGraphicsPipelineCreateInfo* h = gvk_in(&gvk_t_gpipe, ROOT, 1);
    const VkPipelineTessellationDomainOriginStateCreateInfo* o =
        h && h->pTessellationState ? h->pTessellationState->pNext : NULL;
    assert(o && o->sType == origin.sType && o->domainOrigin == VK_TESSELLATION_DOMAIN_ORIGIN_LOWER_LEFT);
    VkInputAttachmentAspectReference ref = {1, 2, VK_IMAGE_ASPECT_DEPTH_BIT};
    VkRenderPassInputAttachmentAspectCreateInfo aspects = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO,
        .aspectReferenceCount = 1,
        .pAspectReferences = (const VkInputAttachmentAspectReference*)(uintptr_t)(NODE + 0x100)};
    VkRenderPassCreateInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .pNext = (const void*)(uintptr_t)EXTRA};
    memcpy(GP(ROOT), &pass, sizeof pass);
    memcpy(GP(EXTRA), &aspects, sizeof aspects);
    memcpy(GP(NODE + 0x100), &ref, sizeof ref);
    const VkRenderPassCreateInfo* p = gvk_in(&gvk_t_rpass, ROOT, 1);
    const VkRenderPassInputAttachmentAspectCreateInfo* a = p ? p->pNext : NULL;
    assert(a && a->aspectReferenceCount == 1 && a->pAspectReferences == (const void*)GP(NODE + 0x100) &&
           a->pAspectReferences[0].inputAttachmentIndex == 2);
}

static void scaled_rect(void) {
    /* ucas: w * f passes INT32_MAX at w = 2147483647, f = 2, which Vulkan refuses of a scissor */
    VkRect2D all = gvk_rect_at((VkRect2D){{0, 0}, {INT32_MAX, INT32_MAX}}, 2);
    assert(all.offset.x == 0 && all.extent.width == INT32_MAX && all.extent.height == INT32_MAX);
    VkRect2D from = gvk_rect_at((VkRect2D){{100, 200}, {INT32_MAX - 100, INT32_MAX - 200}}, 4);
    assert(from.offset.x == 400 && (int64_t)from.offset.x + from.extent.width == INT32_MAX);
    assert(from.offset.y == 800 && (int64_t)from.offset.y + from.extent.height == INT32_MAX);
    VkRect2D part = gvk_rect_at((VkRect2D){{-3, 5}, {7, 11}}, 8);
    assert(part.offset.x == -24 && part.offset.y == 40 && part.extent.width == 56 && part.extent.height == 88);
}

/* what isn't given to the host is said, with the structure and its values */
static void marshal_said(void) {
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 3};
    g_said[0] = 0;
    memcpy(GP(ROOT), &submit, sizeof submit);
    gw32(ROOT, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
    assert(!gvk_in(&gvk_t_submit, ROOT, 1));
    assert(strstr(g_said, "VkSubmitInfo 0 of 1 at 1000 has the sType 8, not 4"));
    gw32(ROOT, VK_STRUCTURE_TYPE_SUBMIT_INFO);
    g_said[0] = 0;
    assert(!gvk_in(&gvk_t_submit, ROOT, 1));
    assert(strstr(g_said, "VkSubmitInfo has 3 elements at 0 by the pointer at its byte 48"));
    VkWriteDescriptorSet w[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstBinding = 5,
         .descriptorCount = 2,
         .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
         .pImageInfo = (const void*)GP(ROOT),
         .pBufferInfo = (const void*)GP(ROOT),
         .pTexelBufferView = (const void*)GP(ROOT)},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstBinding = 7,
         .descriptorCount = 4,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = (const void*)GP(ROOT)},
    };
    assert(gvk_writes_take(w, 2));
    assert(w[0].pImageInfo && !w[0].pBufferInfo && !w[0].pTexelBufferView);
    assert(!w[1].pImageInfo && w[1].pBufferInfo && !w[1].pTexelBufferView);
    w[1].pBufferInfo = NULL;
    g_said[0] = 0;
    assert(!gvk_writes_take(w, 2));
    assert(strstr(g_said, "write 1 of 2 of descriptors (binding 7, type 7, 4 of them) has no pBufferInfo"));
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    assert(!gvk_writes_take(w, 2) && strstr(g_said, "type 4, 4 of them) has no pTexelBufferView"));
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    assert(!gvk_writes_take(w, 2) && strstr(g_said, "type 10, 4 of them) has no pImageInfo"));
    w[1].descriptorCount = 0; /* a write of no descriptors reads none of its arrays */
    w[1].pTexelBufferView = (const void*)GP(ROOT);
    assert(gvk_writes_take(w, 2) && !w[1].pTexelBufferView);
}

static void marshal_edges(void) {
    /* the second of two structures is looked at as the first is, and a count that wraps is none */
    VkSubmitInfo submits[2] = {{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO},
                               {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1}};
    memcpy(GP(ROOT), submits, sizeof submits);
    assert(gvk_in(&gvk_t_submit, ROOT, 1) && !gvk_in(&gvk_t_submit, ROOT, 2));
    assert(!gvk_in(&gvk_t_submit, ROOT, UINT64_MAX / sizeof *submits + 2));
    assert(!gvk_writable(ROOT, UINT64_MAX));

    /* a code size of 8 bytes: what isn't all mapped, and none at all, is refused */
    VkShaderModuleCreateInfo code = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                     .codeSize = 64,
                                     .pCode = (const uint32_t*)(uintptr_t)NODE};
    memcpy(GP(ROOT), &code, sizeof code);
    const VkShaderModuleCreateInfo* shader = gvk_in(&gvk_t_shader, ROOT, 1);
    assert(shader && shader->codeSize == 64 && shader->pCode == (const void*)GP(NODE));
    gw64(ROOT + offsetof(VkShaderModuleCreateInfo, codeSize), 2 * PAGE);
    assert(!gvk_in(&gvk_t_shader, ROOT, 1));
    gw64(ROOT + offsetof(VkShaderModuleCreateInfo, codeSize), 4);
    gw64(ROOT + offsetof(VkShaderModuleCreateInfo, pCode), 0);
    assert(!gvk_in(&gvk_t_shader, ROOT, 1));

    /* a structure inside another: its name is copied, and one that isn't a string is refused */
    const u64 name = NODE + 0x300;
    VkComputePipelineCreateInfo compute = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pName = (const char*)(uintptr_t)name}};
    memcpy(GP(name), "main", 5);
    memcpy(GP(ROOT), &compute, sizeof compute);
    const VkComputePipelineCreateInfo* pipe = gvk_in(&gvk_t_cpipe, ROOT, 1);
    assert(pipe && pipe->stage.pName != (const char*)(uintptr_t)name && !strcmp(pipe->stage.pName, "main"));
    gw64(ROOT + offsetof(VkComputePipelineCreateInfo, stage) + offsetof(VkPipelineShaderStageCreateInfo, pName),
         UNMAPPED);
    assert(!gvk_in(&gvk_t_cpipe, ROOT, 1));
    memset(GP(name), 'x', 0x300);
    gw64(ROOT + offsetof(VkComputePipelineCreateInfo, stage) + offsetof(VkPipelineShaderStageCreateInfo, pName), name);
    assert(!gvk_in(&gvk_t_cpipe, ROOT, 1));

    /* a sample count that is none, and a mask of the one word that one sample takes */
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .pSampleMask = (const VkSampleMask*)(uintptr_t)(NODE + 0x400),
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkGraphicsPipelineCreateInfo graphics = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                             .pMultisampleState =
                                                 (const VkPipelineMultisampleStateCreateInfo*)(uintptr_t)NODE};
    memcpy(GP(NODE), &ms, sizeof ms);
    memcpy(GP(ROOT), &graphics, sizeof graphics);
    const VkGraphicsPipelineCreateInfo* g = gvk_in(&gvk_t_gpipe, ROOT, 1);
    assert(g && g->pMultisampleState->pSampleMask);
    gw32(NODE + offsetof(VkPipelineMultisampleStateCreateInfo, rasterizationSamples), 3);
    assert(!gvk_in(&gvk_t_gpipe, ROOT, 1));

    /* an optional pointer of another type, or not all mapped, is none */
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                      .pInheritanceInfo = (const VkCommandBufferInheritanceInfo*)(uintptr_t)NODE};
    memcpy(GP(ROOT), &begin, sizeof begin);
    gw32(NODE, VK_STRUCTURE_TYPE_SUBMIT_INFO);
    g_said[0] = 0; /* and not a fault to say: Vulkan ignores it */
    const VkCommandBufferBeginInfo* b = gvk_in(&gvk_t_begin, ROOT, 1);
    assert(b && !b->pInheritanceInfo && !g_said[0]);
    gw32(UNMAPPED - 16, VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO);
    gw64(ROOT + offsetof(VkCommandBufferBeginInfo, pInheritanceInfo), UNMAPPED - 16);
    b = gvk_in(&gvk_t_begin, ROOT, 1);
    assert(b && !b->pInheritanceInfo && !g_said[0]);
}

static void chain_edges(void) {
    /* two known structures are linked in order, and the last one ends the chain */
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .pNext = (void*)(uintptr_t)(NODE + 0x100)};
    VkPipelineRasterizationDepthClipStateCreateInfoEXT clip = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT,
        .pNext = (void*)(uintptr_t)(NODE + 0x180)};
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT provoking = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT,
        .pNext = (void*)(uintptr_t)(NODE + 0x200)};
    VkGraphicsPipelineCreateInfo graphics = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                             .pRasterizationState =
                                                 (const VkPipelineRasterizationStateCreateInfo*)(uintptr_t)NODE};
    memcpy(GP(ROOT), &graphics, sizeof graphics);
    memcpy(GP(NODE), &raster, sizeof raster);
    memcpy(GP(NODE + 0x100), &clip, sizeof clip);
    memcpy(GP(NODE + 0x180), &provoking, sizeof provoking);
    gw32(NODE + 0x200, 0x7ffffffe); /* a structure gvk doesn't know, after them */
    gw64(NODE + 0x208, 0);
    const VkGraphicsPipelineCreateInfo* g = gvk_in(&gvk_t_gpipe, ROOT, 1);
    assert(g && g->pRasterizationState);
    const VkPipelineRasterizationDepthClipStateCreateInfoEXT* c = g->pRasterizationState->pNext;
    assert(c && c->sType == clip.sType);
    const VkPipelineRasterizationProvokingVertexStateCreateInfoEXT* p = c->pNext;
    assert(p && p->sType == provoking.sType && !p->pNext);

    /* a structure another chain takes is left out of this one */
    VkSamplerReductionModeCreateInfo reduction = {.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
    VkBufferCreateInfo buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = (void*)(uintptr_t)NODE};
    memcpy(GP(NODE), &reduction, sizeof reduction);
    memcpy(GP(ROOT), &buffer, sizeof buffer);
    const VkBufferCreateInfo* hb = gvk_in(&gvk_t_buffer, ROOT, 1);
    assert(hb && !hb->pNext);

    /* a structure twice, a chain that comes back, one of more than its hops, and one whose last structure isn't all
       mapped are refused, saying which */
    VkMemoryRequirements2 req = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = (void*)(uintptr_t)NODE};
    VkMemoryDedicatedRequirements ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
                                         .pNext = (void*)(uintptr_t)EXTRA};
    memcpy(GP(ROOT), &req, sizeof req);
    memcpy(GP(NODE), &ded, sizeof ded);
    ded.pNext = NULL;
    memcpy(GP(EXTRA), &ded, sizeof ded);
    assert(!gvk_in(&gvk_t_req2, ROOT, 1) && strstr(g_said, "has two VkMemoryDedicatedRequirements"));
    gw64(NODE + 8, NODE);
    assert(!gvk_in(&gvk_t_req2, ROOT, 1) && strstr(g_said, "comes back to 2000"));
    for (u64 k = 0; k < 64; k++) {
        gw32(NODE + 0x400 + 16 * k, 0x7ffffffd);
        gw64(NODE + 0x408 + 16 * k, k < 63 ? NODE + 0x410 + 16 * k : 0);
    }
    gw64(ROOT + 8, NODE + 0x400);
    const VkMemoryRequirements2* r = gvk_in(&gvk_t_req2, ROOT, 1);
    assert(r && !r->pNext);
    gw64(NODE + 0x408 + 16 * 63, NODE + 0x3f0);
    gw32(NODE + 0x3f0, 0x7ffffffd);
    gw64(NODE + 0x3f8, 0);
    assert(!gvk_in(&gvk_t_req2, ROOT, 1) && strstr(g_said, "longer than 64"));
    gw64(ROOT + 8, UNMAPPED - 16);
    gw32(UNMAPPED - 16, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS);
    gw64(UNMAPPED - 8, 0);
    assert(!gvk_in(&gvk_t_req2, ROOT, 1) && strstr(g_said, "isn't all mapped"));
}

static void scratch_pieces(void) {
    u8* piece[6];
    const size_t size[6] = {1, 1, 700000, 700000, 1 << 21, 33};
    for (u32 i = 0; i < 6; i++) {
        piece[i] = gvk_scratch(size[i]);
        assert(!((uintptr_t)piece[i] & 15));
        memset(piece[i], (int)i + 1, size[i]);
    }
    for (u32 i = 0; i < 6; i++)
        for (size_t k = 0; k < size[i]; k += size[i] > 4096 ? 4096 : 1) assert(piece[i][k] == i + 1);
    gvk_scratch_free();
}

static void unlisted_names(void) {
    static const VkExtensionProperties listed[] = {{"VK_KHR_swapchain", 70}, {"VK_KHR_maintenance1", 2}};
    char name[VK_MAX_EXTENSION_NAME_SIZE];
    const u64 a = NODE + 0x100, b = NODE + 0x120, longer = NODE + 0x200;
    memcpy(GP(a), "VK_KHR_maintenance1", 20);
    memcpy(GP(b), "VK_KHR_maintenance2", 20);
    memset(GP(longer), 'x', VK_MAX_EXTENSION_NAME_SIZE);
    memcpy(GP(longer + VK_MAX_EXTENSION_NAME_SIZE), "", 1);
#define UNLISTED(N) gvk_unlisted(ROOT, N, listed[0].extensionName, sizeof *listed, 2, name, sizeof name)
    gw64(ROOT, a);
    gw64(ROOT + 8, b);
    assert(!UNLISTED(0) && !name[0]);
    assert(!UNLISTED(1) && !name[0]);
    assert(UNLISTED(2) && !strcmp(name, "VK_KHR_maintenance2"));
    gw64(ROOT, b); /* the first one that isn't, of two that aren't */
    assert(UNLISTED(2) && !strcmp(name, "VK_KHR_maintenance2"));
    gw64(ROOT, 0); /* no string: an empty name */
    assert(UNLISTED(1) && !name[0]);
    gw64(ROOT, UNMAPPED);
    assert(UNLISTED(1) && !name[0]);
    gw64(ROOT, longer); /* past the size of a name */
    assert(UNLISTED(1) && !name[0]);
    gw64(ROOT, a);
    assert(gvk_unlisted(UNMAPPED - 8, 2, listed[0].extensionName, sizeof *listed, 2, name, sizeof name) && !name[0]);
    assert(gvk_unlisted(0x7ffffff000ull, 1, listed[0].extensionName, sizeof *listed, 2, name, sizeof name) && !name[0]);
#undef UNLISTED
}

static void copies_depth(void) {
    VkBufferImageCopy r[2] = {{.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT}},
                              {.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT}}};
    assert(!gvk_copies_depth(r, 2) && !gvk_copies_depth(NULL, 2));
    r[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    assert(gvk_copies_depth(r, 2) && !gvk_copies_depth(r, 1));
    r[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    assert(gvk_copies_depth(r, 2));
}

static void query_bytes(void) {
    /* ucas: two queries 8 bytes apart write 8 + 8, past the 8 bytes a game gave */
    assert(gvk_query_bytes(2, 1, VK_QUERY_RESULT_64_BIT, 8) == 16);
    assert(gvk_query_bytes(1, 1, VK_QUERY_RESULT_64_BIT, 1u << 20) == 8);
    assert(gvk_query_bytes(3, 1, 0, 16) == 36);
    assert(gvk_query_bytes(2, 1, VK_QUERY_RESULT_WITH_AVAILABILITY_BIT, 8) == 16);
    assert(gvk_query_bytes(2, 11, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT, 96) == 192);
    assert(gvk_query_bytes(0, 1, VK_QUERY_RESULT_64_BIT, 8) == 0);
    /* a stride of which n - 1 pass 64 bits */
    assert(gvk_query_bytes(3, 1, VK_QUERY_RESULT_64_BIT, UINT64_MAX / 2 + 1) == UINT64_MAX);
    assert(gvk_query_bytes(2, 1, VK_QUERY_RESULT_64_BIT, UINT64_MAX - 8) == UINT64_MAX);
    assert(gvk_query_bytes(2, 1, VK_QUERY_RESULT_64_BIT, UINT64_MAX - 7) == UINT64_MAX);
}

int main(void) {
    marshal_submit();
    gvk_scratch_reset();
    marshal_chain();
    gvk_scratch_reset();
    marshal_sample_mask();
    gvk_scratch_reset();
    marshal_optional_one();
    gvk_scratch_reset();
    marshal_ignored_state();
    gvk_scratch_reset();
    marshal_core_chains();
    gvk_scratch_free();
    scaled_rect();
    marshal_said();
    gvk_scratch_free();
    marshal_edges();
    gvk_scratch_free();
    chain_edges();
    gvk_scratch_free();
    scratch_pieces();
    query_bytes();
    copies_depth();
    unlisted_names();
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
    install_driver();
#endif
    return 0;
}
