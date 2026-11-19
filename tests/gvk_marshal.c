/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/gvk/gvk.h"
#include "nx/hle/hook/hook.h"
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
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    (void)fmt;
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
    {ROOT + 4, "vkQueueSubmit"},  {NODE + 4, "vkGetInstanceProcAddr"},
    {NODE + 8, "vkPrivateAlias"}, {NODE + 8, "vkQueueSubmit"},
    {NODE + 12, "notVulkan"},
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
    g_settings.guest_vulkan = 1;
    g_slots[(NODE + 8 - ROOT) / 4] = orig_submit;
    g_code_hi = gvk_proc_space(g_code_hi);
    gvk_install();
    assert(gvk_active());
    assert(gvk_export("vkQueueSubmit") == NODE + 8);
    assert(g_slots[(NODE + 8 - ROOT) / 4] == submit);
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

int main(void) {
    marshal_submit();
    gvk_scratch_reset();
    marshal_chain();
    gvk_scratch_reset();
    marshal_sample_mask();
    gvk_scratch_reset();
    marshal_optional_one();
    gvk_scratch_free();
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
    install_driver();
#endif
    return 0;
}
