/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/vk/shader_objects.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "common/gpu/vk/shader_object.h"
#include "common/gpu/vk/vk_common.h"
#include "common/utilities/bits.h"
#include "common/utilities/common.h"
#include "nx/gpu/shader/shader.h"
#include "nx/gpu/vk/descriptor_heaps.h"
#include "nx/gpu/vk/pipeline_parts.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "common/platform/thread.h" /* plat_thread_note */
#include "nx/runtime/platform/tracy.h"

/* A module's SPIR-V, by the module's handle bits, in an open-addressed table that grows (spirv_lock). A module kept
   here lives as long as the run (the translator's and the shader file's, pipeline.c's modinfo_put), unless it is
   forgotten before it is destroyed (vk_so_forget_spirv): a serial for each keep keeps a handle that the driver gives
   again apart, in the stage objects below too */
typedef struct {
    u64 mod;
    u32* words; /* NULL once forgotten (vk_so_forget_spirv): the slot stays, so that probing passes it */
    size_t count;
    u32 serial; /* given at each keep: a handle a destroyed module had names another program */
} SoSpirv;
/* One stage's object, by what its create info is made of (stage_lock): the module, the stage, the set layouts' texture
   array bits (PipeKey.arrays), the fragment specialization (PipeKey.fs_spec), a mesh or task stage's flags (whole
   subgroups, a mesh stage with no task stage) and, with descriptor heaps, the stage's mapping of its set (heaps 1,
   and the constant buffers its set maps by address, PipeKey.cb_addr). state 1 while a thread makes it, 2 once made
   (obj VK_NULL_HANDLE when the device refused it) */
typedef struct SoStage {
    VkShaderModule mod;
    u32 serial; /* of the module's SPIR-V (SoSpirv) */
    VkShaderStageFlagBits stage;
    u32 arrays, spec, heaps, cb_addr;
    VkShaderCreateFlagsEXT flags;
    int state;
    VkShaderEXT obj;
    struct SoStage* next;
} SoStage;
#define SO_STAGEH 4096
/* What the device was made with that a draw with shader objects must set state for (vkCmdDraw's VUIDs), recorded once
   (vk_so_init): the flags in g_vk can change after it, as the tests turn native paths off to check their fallbacks,
   while the device keeps the extension and its rule */
enum {
    SO_TESS = 1u << 0,         /* tessellationShader */
    SO_GEOM = 1u << 1,         /* geometryShader */
    SO_TASK = 1u << 2,         /* the task stage */
    SO_MESH = 1u << 3,         /* the mesh stage */
    SO_CLAMP = 1u << 4,        /* depthClamp */
    SO_CLIP = 1u << 5,         /* VK_EXT_depth_clip_enable */
    SO_PROVOKING = 1u << 6,    /* VK_EXT_provoking_vertex */
    SO_CONSERVATIVE = 1u << 7, /* VK_EXT_conservative_rasterization */
    SO_STREAMS = 1u << 8,      /* geometryStreams of VK_EXT_transform_feedback */
    SO_ALPHA_ONE = 1u << 9,    /* alphaToOne */
    SO_LOCATIONS = 1u << 10,   /* VK_EXT_sample_locations */
    SO_MIXED = 1u << 11,       /* VK_NV_framebuffer_mixed_samples */
    SO_TO_COLOR = 1u << 12,    /* VK_NV_fragment_coverage_to_color */
    SO_LOGIC_OP = 1u << 13,    /* logicOp */
    SO_VRS_NV = 1u << 14,      /* VK_NV_shading_rate_image */
    SO_VRS_KHR = 1u << 15      /* VK_KHR_fragment_shading_rate's attachment */
};
static struct {
    u32 dev; /* SO_* */
    PMutex spirv_lock;
    SoSpirv* spirv;
    size_t spirv_slots, spirv_count;
    u32 spirv_serial;
    u64 spirv_bytes; /* the copies kept, for the stats line (spirv_lock) */
    PMutex stage_lock;
    PCond stage_done;
    int stage_cond_ready;
    SoStage* stages[SO_STAGEH];
    /* the binaries of the objects, and their file (bin_lock): open once vk_so_init read it */
    PMutex bin_lock;
    SnxVkShaderBinaries bin;
    _Atomic int bin_open;
    char bin_path[1200];
    /* atomics: the objects made, those from a binary, and their ns */
    _Atomic u64 made, from_binary, ns;
} g_so = {.spirv_lock = PMUTEX_INIT, .stage_lock = PMUTEX_INIT, .bin_lock = PMUTEX_INIT};

static u64 mod_bits(VkShaderModule m) { return (u64)(uintptr_t)m; }

/* the slot of mod in the table (spirv_lock): its own, or the empty one where it would go */
static size_t spirv_slot(const SoSpirv* t, size_t slots, u64 mod) {
    size_t i = (size_t)(snx_fnv1a(SNX_FNV_BASIS, &mod, sizeof mod) & (slots - 1));
    while (t[i].mod && t[i].mod != mod) i = (i + 1) & (slots - 1);
    return i;
}
void vk_so_keep_spirv(VkShaderModule m, const u32* words, size_t count) {
    if (!g_vk.has_shader_objects || !m || !words || !count) return;
    u32* copy = malloc(count * sizeof *copy);
    if (!copy) return; /* the module's draws go through pipelines */
    memcpy(copy, words, count * sizeof *copy);
    pm_lock(&g_so.spirv_lock);
    if ((g_so.spirv_count + 1) * 2 > g_so.spirv_slots) {
        size_t slots = g_so.spirv_slots ? g_so.spirv_slots * 2 : 1024;
        SoSpirv* t = calloc(slots, sizeof *t);
        if (!t) {
            pm_unlock(&g_so.spirv_lock);
            free(copy);
            return;
        }
        for (size_t i = 0; i < g_so.spirv_slots; i++)
            if (g_so.spirv[i].mod) t[spirv_slot(t, slots, g_so.spirv[i].mod)] = g_so.spirv[i];
        free(g_so.spirv);
        g_so.spirv = t;
        g_so.spirv_slots = slots;
    }
    SoSpirv* e = &g_so.spirv[spirv_slot(g_so.spirv, g_so.spirv_slots, mod_bits(m))];
    if (!e->mod) g_so.spirv_count++;
    /* a handle again: a module that was destroyed (vk_so_forget_spirv) gave it. Its words stay, as a thread may
       still read them, and its stage objects stay apart by the serial */
    *e = (SoSpirv){mod_bits(m), copy, count, ++g_so.spirv_serial};
    g_so.spirv_bytes += count * sizeof *copy;
    pm_unlock(&g_so.spirv_lock);
}
void vk_so_forget_spirv(VkShaderModule m) {
    pm_lock(&g_so.spirv_lock);
    if (g_so.spirv_slots) {
        SoSpirv* e = &g_so.spirv[spirv_slot(g_so.spirv, g_so.spirv_slots, mod_bits(m))];
        if (e->mod) e->words = NULL; /* the words stay allocated: a thread may still read them */
    }
    pm_unlock(&g_so.spirv_lock);
}
/* m's SPIR-V and its serial: NULL for a module none was kept of, or one forgotten. The words live as long as the
   run */
static const u32* spirv_of(VkShaderModule m, size_t* count, u32* serial) {
    const u32* words = NULL;
    pm_lock(&g_so.spirv_lock);
    if (g_so.spirv_slots) {
        const SoSpirv* e = &g_so.spirv[spirv_slot(g_so.spirv, g_so.spirv_slots, mod_bits(m))];
        if (e->mod && e->words) {
            words = e->words;
            *count = e->count;
            *serial = e->serial;
        }
    }
    pm_unlock(&g_so.spirv_lock);
    return words;
}

int vk_so_takes(const PipeKey* k, VkShaderModule fs) {
    if (!g_vk.has_shader_objects) return 0;
    /* state that is no dynamic state for shader objects (minSampleShading), or that this path does not set yet:
       mixed samples, coverage to color and sample locations. A mesh draw's mesh and task stages are objects where the
       device has the stages */
    if (k->sample_shading || k->coverage.raster_samples || k->coverage_to_color || k->sample_locations.samples)
        return 0;
    if (k->mesh && (!(g_so.dev & SO_MESH) || (k->tcs && !(g_so.dev & SO_TASK)))) return 0;
    if (k->feedback && !g_vk.has_feedback_dynamic) return 0;
    if (k->logic_op && !(g_so.dev & SO_LOGIC_OP)) return 0;
    const VkShaderModule modules[5] = {k->vs, k->tcs, k->tes, k->gs, fs};
    for (int s = 0; s < 5; s++) {
        size_t n;
        u32 serial;
        if (modules[s] && !spirv_of(modules[s], &n, &serial)) return 0;
    }
    return 1;
}

/* the stages that may come after stage, of those the device has */
static VkShaderStageFlags next_stages(VkShaderStageFlagBits stage) {
    VkShaderStageFlags tess = g_so.dev & SO_TESS ? VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT : 0;
    VkShaderStageFlags geom = g_so.dev & SO_GEOM ? VK_SHADER_STAGE_GEOMETRY_BIT : 0;
    switch (stage) {
    case VK_SHADER_STAGE_VERTEX_BIT: return tess | geom | VK_SHADER_STAGE_FRAGMENT_BIT;
    case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return geom | VK_SHADER_STAGE_FRAGMENT_BIT;
    case VK_SHADER_STAGE_GEOMETRY_BIT: return VK_SHADER_STAGE_FRAGMENT_BIT;
    case VK_SHADER_STAGE_TASK_BIT_EXT: return VK_SHADER_STAGE_MESH_BIT_EXT;
    case VK_SHADER_STAGE_MESH_BIT_EXT: return VK_SHADER_STAGE_FRAGMENT_BIT;
    default: return 0;
    }
}

/* The object of st made, from the binary of an earlier run where the file has one the driver takes, else compiled
   from words and its binary kept. VK_NULL_HANDLE when the device refuses it */
static VkShaderEXT stage_create(const SoStage* st, const u32* words, size_t count, int index) {
    /* vk_playout's sets: with split sets, each stage's textures five after its buffers */
    VkDescriptorSetLayout sets[11];
    u32 nsets = g_spirv_translate.split_sets ? 10 : g_vk_stage_sets;
    for (u32 s = 0; s < nsets; s++) sets[s] = g_vk_dslayout[(st->arrays >> (s % 5)) & 1][s];
    if (g_spirv_translate.tex_table) sets[nsets++] = vk_table_layout(); /* at table_set */
    VkPushConstantRange pc = {VK_PUSH_STAGES, 0, vk_push_bytes()};      /* vk_playout's */
    u32 values[SH_SPEC_COUNT];
    VkSpecializationMapEntry entries[SH_SPEC_COUNT];
    VkSpecializationInfo spec = {0};
    if (st->spec) { /* as pipe_info gives a fragment module its constants */
        vk_pipe_fs_spec_values(st->spec, values);
        for (u32 id = 0; id < SH_SPEC_COUNT; id++) entries[id] = (VkSpecializationMapEntry){id, id * 4u, 4};
        spec = (VkSpecializationInfo){SH_SPEC_COUNT, entries, sizeof values, values};
    }
    VkShaderCreateInfoEXT info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
        .flags = st->stage == VK_SHADER_STAGE_FRAGMENT_BIT && (g_so.dev & SO_VRS_KHR)
                     ? VK_SHADER_CREATE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_EXT
                     : 0,
        .stage = st->stage,
        .nextStage = next_stages(st->stage),
        .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
        .codeSize = count * sizeof *words,
        .pCode = words,
        .pName = "main",
        .setLayoutCount = nsets,
        .pSetLayouts = sets,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pc,
        .pSpecializationInfo = st->spec ? &spec : NULL};
    /* a mesh or task program's lanes are a warp's: its subgroups are 32 invocations, as pipe_info gives a mesh
       pipeline's stages */
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo warp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO,
        .requiredSubgroupSize = 32};
    if (st->stage == VK_SHADER_STAGE_MESH_BIT_EXT || st->stage == VK_SHADER_STAGE_TASK_BIT_EXT) info.pNext = &warp;
    info.flags |= st->flags;
    /* descriptor heaps: the stage's mapping of its set's bindings, as pipe_info gives a pipeline's stage, in place of
       the layouts; push constants are the heaps' push data then (rec_CmdPushConstants) */
    VkDescriptorSetAndBindingMappingEXT maps[SNX_VKB_END];
    VkShaderDescriptorSetAndBindingMappingInfoEXT mapping;
    if (st->heaps) {
        const u32 set = (u32)shader_vk_set(index);
        mapping = (VkShaderDescriptorSetAndBindingMappingInfoEXT){
            .sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT,
            .pNext = info.pNext, /* a mesh stage's required subgroup size */
            .mappingCount = vk_descriptor_heaps_mapping(g_vk_dslayout[(st->arrays >> set) & 1][set], set, maps,
                                                        SNX_VKB_END, st->cb_addr),
            .pMappings = maps};
        info.pNext = &mapping;
        info.flags |= VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT;
        info.setLayoutCount = info.pushConstantRangeCount = 0;
        info.pSetLayouts = NULL;
        info.pPushConstantRanges = NULL;
    }
    /* snx_vk_shader_key keeps no specialization, no set layouts' bindings and no flags: they are mixed in, the
       layouts by what they are made of (g_vk_dslayout_hash), which another build or setting changes */
    u64 key = snx_fnv_step(snx_fnv_step(snx_vk_shader_key(&info), g_vk_dslayout_hash), (u64)st->arrays);
    key = snx_fnv_step(key, info.flags);
    if (st->heaps) key = snx_fnv_step(key, st->cb_addr);
    if (st->spec) key = snx_fnv1a(key, values, sizeof values);
    u64 t0 = plat_ns();
    SNX_ZONE(z, "vk shader object make");
    VkShaderEXT obj = VK_NULL_HANDLE;
    void* binary = NULL;
    size_t size = 0;
    int from_binary = 0;
    pm_lock(&g_so.bin_lock);
    if (atomic_load_explicit(&g_so.bin_open, memory_order_relaxed)) {
        const void* kept = snx_vk_shader_binary(&g_so.bin, key, &size);
        if (kept && (binary = malloc(size)) != NULL) memcpy(binary, kept, size); /* its table may grow meanwhile */
    }
    pm_unlock(&g_so.bin_lock);
    int refused = 0;
    if (binary) {
        VkShaderCreateInfoEXT from = info;
        from.codeType = VK_SHADER_CODE_TYPE_BINARY_EXT;
        from.codeSize = size;
        from.pCode = binary;
        if (vkCreateShadersEXT(g_vk.dev, 1, &from, NULL, &obj) != VK_SUCCESS) obj = VK_NULL_HANDLE;
        else SNX_MEM_ALLOC_D((const void*)(uintptr_t)obj, 0, "shader modules"); /* kept for the run: no destroy site */
        free(binary);
        if (obj) {
            atomic_fetch_add_explicit(&g_so.from_binary, 1, memory_order_relaxed);
            from_binary = 1;
        } else
            refused = 1; /* compiled again below, and its new binary takes the place of this one */
    }
    if (!obj) {
        int entered = snx_vk_compile_enter();
        VkResult r = vkCreateShadersEXT(g_vk.dev, 1, &info, NULL, &obj);
        snx_vk_compile_leave(entered);
        if (r != VK_SUCCESS) {
            SNX_ZONE_TEXT(z, "the device refused it: draws go through a pipeline");
            SNX_ZONE_VALUE(z, 0);
            SNX_ZONE_END(z);
            LOG_LIMIT(16, "vulkan: the device refused a shader object (%s): its draws go through a pipeline\n",
                      snx_vk_result_str(r));
            return VK_NULL_HANDLE;
        }
        SNX_MEM_ALLOC_D((const void*)(uintptr_t)obj, 0, "shader modules"); /* kept for the run: no destroy site */
        /* the driver's binary got outside bin_lock, which the other workers' lookups take */
        size_t got = 0;
        void* data = NULL;
        if (atomic_load_explicit(&g_so.bin_open, memory_order_acquire) &&
            vkGetShaderBinaryDataEXT(g_vk.dev, obj, &got, NULL) == VK_SUCCESS && got && (data = malloc(got)) != NULL &&
            vkGetShaderBinaryDataEXT(g_vk.dev, obj, &got, data) == VK_SUCCESS) {
            pm_lock(&g_so.bin_lock);
            size_t had;
            if (refused || !snx_vk_shader_binary(&g_so.bin, key, &had))
                (void)snx_vk_shader_binaries_put(&g_so.bin, key, data, got);
            pm_unlock(&g_so.bin_lock);
        }
        free(data);
    }
    atomic_fetch_add_explicit(&g_so.made, 1, memory_order_relaxed);
    u64 ns = plat_ns() - t0;
    atomic_fetch_add_explicit(&g_so.ns, ns, memory_order_relaxed);
    SNX_ZONE_TEXT(z, "%s, %llu B",
                  from_binary ? "from a kept binary"
                              : refused ? "binary refused, fresh SPIR-V"
                                        : "no binary, fresh SPIR-V",
                  (unsigned long long)(from_binary ? size : count * sizeof *words));
    SNX_ZONE_VALUE(z, obj != VK_NULL_HANDLE);
    SNX_ZONE_END(z);
    SNX_PLOT("vk pipe shader object ms", (double)ns / 1e6);
    SNX_PLOT("vk pipe shader object from binary", (double)from_binary);
    SNX_PLOT("vk pipe shader object source", (double)(from_binary ? 1 : refused ? 2 : 3)); /* 1 binary, 2 refused, 3 SPIR-V */
    return obj;
}

/* the object of mod at stage (index: its slot in vk_so_make, an ST_* stage), made here unless a thread makes it
   already, then waited for */
static VkShaderEXT stage_get(VkShaderModule mod, VkShaderStageFlagBits stage, int index, u32 arrays, u32 spec,
                             VkShaderCreateFlagsEXT flags, u32 heaps, u32 cb_addr) {
    size_t count = 0;
    u32 serial = 0;
    const u32* words = spirv_of(mod, &count, &serial);
    if (!words) return VK_NULL_HANDLE;
    u64 h = snx_fnv_step(snx_fnv_step(snx_fnv_step(snx_fnv_step(SNX_FNV_BASIS, mod_bits(mod)), serial), stage),
                         (u64)arrays << 32 | spec);
    h = snx_fnv_step(snx_fnv_step(h, flags), (u64)cb_addr << 1 | heaps);
    u32 b = (u32)(h & (SO_STAGEH - 1));
    pm_lock(&g_so.stage_lock);
    if (!g_so.stage_cond_ready) {
        pc_init(&g_so.stage_done);
        g_so.stage_cond_ready = 1;
    }
    SoStage* st = g_so.stages[b];
    while (st && (st->mod != mod || st->serial != serial || st->stage != stage || st->arrays != arrays ||
                  st->spec != spec || st->flags != flags || st->heaps != heaps || st->cb_addr != cb_addr))
        st = st->next;
    if (!st) {
        st = calloc(1, sizeof *st);
        if (!st) {
            pm_unlock(&g_so.stage_lock);
            return VK_NULL_HANDLE;
        }
        *st = (SoStage){mod, serial, stage, arrays, spec, heaps, cb_addr, flags, 0, VK_NULL_HANDLE, g_so.stages[b]};
        g_so.stages[b] = st;
    }
    while (st->state == 1) pc_wait(&g_so.stage_done, &g_so.stage_lock, -1);
    if (st->state == 2) {
        VkShaderEXT obj = st->obj;
        pm_unlock(&g_so.stage_lock);
        return obj;
    }
    st->state = 1;
    pm_unlock(&g_so.stage_lock);
    vk_so_note(mod, stage);
    VkShaderEXT obj = stage_create(st, words, count, index);
    plat_thread_note_clear();
    pm_lock(&g_so.stage_lock);
    st->obj = obj;
    st->state = 2;
    pc_broadcast(&g_so.stage_done);
    pm_unlock(&g_so.stage_lock);
    return obj;
}

int vk_so_make(const PipeKey* k, VkShaderModule fs, VkShaderEXT out[5]) {
    static const VkShaderStageFlagBits bits[5] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                                  VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                                                  VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
    /* a mesh draw's mesh and task programs, in the vertex and tessellation control slots (pipe_info's mesh_bits) */
    static const VkShaderStageFlagBits mesh_bits[5] = {VK_SHADER_STAGE_MESH_BIT_EXT, VK_SHADER_STAGE_TASK_BIT_EXT, 0, 0,
                                                       VK_SHADER_STAGE_FRAGMENT_BIT};
    const VkShaderModule modules[5] = {k->vs, k->tcs, k->tes, k->gs, fs};
    u32 arrays = k->arrays & ((1u << g_vk_stage_sets) - 1);
    for (int s = 0; s < 5; s++) {
        out[s] = VK_NULL_HANDLE;
        VkShaderCreateFlagsEXT flags = 0;
        if (k->mesh && s < 2) {
            /* a workgroup of fewer invocations than a warp is one subgroup, its lanes the first ones */
            if (k->mesh & PIPE_MESH_WHOLE(s ? ST_TS : ST_MS)) flags |= VK_SHADER_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT;
            if (!s && !k->tcs && (g_so.dev & SO_TASK)) flags |= VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT;
        }
        const u32 cb_addr = k->heaps ? k->cb_addr[shader_vk_set(s)] : 0; /* the slots are ST_VS to ST_FS */
        if (modules[s] && !(out[s] = stage_get(modules[s], k->mesh ? mesh_bits[s] : bits[s], s, arrays,
                                               s == 4 ? k->fs_spec : 0, flags, k->heaps != 0, cb_addr)))
            return 0;
    }
    return 1;
}

void vk_so_bind(VkCommandBuffer cb, const PipeKey* k, const VkShaderEXT sh[5]) {
    /* a mesh draw: its mesh and task objects (sh[0], sh[1]), and no vertex stage */
    VkShaderStageFlagBits stages[7] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
    VkShaderEXT objects[7] = {k->mesh ? VK_NULL_HANDLE : sh[0], sh[4]};
    u32 n = 2;
    if (g_so.dev & SO_TESS) {
        stages[n] = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        objects[n++] = k->mesh ? VK_NULL_HANDLE : sh[1];
        stages[n] = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        objects[n++] = sh[2];
    }
    if (g_so.dev & SO_GEOM) {
        stages[n] = VK_SHADER_STAGE_GEOMETRY_BIT;
        objects[n++] = sh[3];
    }
    if (g_so.dev & SO_TASK) { /* a pipeline's draws had none: stages a draw does not use are bound empty */
        stages[n] = VK_SHADER_STAGE_TASK_BIT_EXT;
        objects[n++] = k->mesh ? sh[1] : VK_NULL_HANDLE;
    }
    if (g_so.dev & SO_MESH) {
        stages[n] = VK_SHADER_STAGE_MESH_BIT_EXT;
        objects[n++] = k->mesh ? sh[0] : VK_NULL_HANDLE;
    }
    vkCmdBindShadersEXT(cb, n, stages, objects);

    /* the pre-rasterization state of the key, which render.c's pr_dynamic sets again after the bind where
       g_vk.dyn_pr has it, with the depth range of the draw */
    vkCmdSetPolygonModeEXT(cb, (VkPolygonMode)k->polygon);
    if (g_so.dev & SO_CLAMP) vkCmdSetDepthClampEnableEXT(cb, k->depth_clamp ? VK_TRUE : VK_FALSE);
    if (g_so.dev & SO_CLIP) vkCmdSetDepthClipEnableEXT(cb, k->depth_clip == 2 ? VK_TRUE : VK_FALSE);
    if (g_so.dev & SO_PROVOKING)
        vkCmdSetProvokingVertexModeEXT(cb, k->provoking_last ? VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT
                                                             : VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT);
    if (g_so.dev & SO_CONSERVATIVE) {
        vkCmdSetConservativeRasterizationModeEXT(cb, k->conservative ? VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT
                                                                     : VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT);
        vkCmdSetExtraPrimitiveOverestimationSizeEXT(cb, vk_conservative_extra(k->conservative));
    }
    if (k->tes) {
        vkCmdSetPatchControlPointsEXT(cb, k->patch);
        /* a pipeline's, without VkPipelineTessellationDomainOriginStateCreateInfo */
        vkCmdSetTessellationDomainOriginEXT(cb, VK_TESSELLATION_DOMAIN_ORIGIN_UPPER_LEFT);
    }
    if (g_so.dev & SO_STREAMS) vkCmdSetRasterizationStreamEXT(cb, 0);

    /* multisampling: the targets' samples, the ones the mask leaves, alpha to coverage and to one */
    VkSampleCountFlagBits samples = k->samples ? (VkSampleCountFlagBits)k->samples : VK_SAMPLE_COUNT_1_BIT;
    VkSampleMask mask = ~k->sample_off;
    vkCmdSetRasterizationSamplesEXT(cb, samples);
    vkCmdSetSampleMaskEXT(cb, samples, &mask);
    vkCmdSetAlphaToCoverageEnableEXT(cb, k->alpha_to_coverage ? VK_TRUE : VK_FALSE);
    if (g_so.dev & SO_ALPHA_ONE) vkCmdSetAlphaToOneEnableEXT(cb, k->alpha_to_one ? VK_TRUE : VK_FALSE);
    if (g_so.dev & SO_LOCATIONS) vkCmdSetSampleLocationsEnableEXT(cb, VK_FALSE);
    if (g_so.dev & SO_MIXED) {
        vkCmdSetCoverageModulationModeNV(cb, VK_COVERAGE_MODULATION_MODE_NONE_NV);
        vkCmdSetCoverageModulationTableEnableNV(cb, VK_FALSE);
    }
    if (g_so.dev & SO_TO_COLOR) vkCmdSetCoverageToColorEnableNV(cb, VK_FALSE);

    /* the color targets' blending and write masks, as pipe_info gives them (vk_pipe_blend), or a logic operation. A
       key whose blending is dynamic state has the draw's set by the draw after the bind (render.c) */
    if (k->ncolor && !vk_pipe_blend_dynamic(k)) {
        VkPipelineColorBlendAttachmentState cba[8];
        vk_pipe_blend(k, cba);
        VkBool32 enable[8];
        VkColorComponentFlags write[8];
        VkColorBlendEquationEXT eq[8];
        VkColorBlendAdvancedEXT adv[8];
        for (u32 i = 0; i < k->ncolor; i++) {
            enable[i] = cba[i].blendEnable;
            write[i] = cba[i].colorWriteMask;
            eq[i] = (VkColorBlendEquationEXT){cba[i].srcColorBlendFactor, cba[i].dstColorBlendFactor,
                                              cba[i].colorBlendOp,        cba[i].srcAlphaBlendFactor,
                                              cba[i].dstAlphaBlendFactor, cba[i].alphaBlendOp};
            adv[i] = (VkColorBlendAdvancedEXT){.advancedBlendOp = (VkBlendOp)k->advanced.operation,
                                               .srcPremultiplied = k->advanced.src_premultiplied,
                                               .dstPremultiplied = VK_TRUE,
                                               .blendOverlap = (VkBlendOverlapEXT)k->advanced.overlap};
        }
        vkCmdSetColorBlendEnableEXT(cb, 0, k->ncolor, enable);
        if (k->advanced.operation)
            vkCmdSetColorBlendAdvancedEXT(cb, 0, k->ncolor, adv);
        else
            vkCmdSetColorBlendEquationEXT(cb, 0, k->ncolor, eq);
        vkCmdSetColorWriteMaskEXT(cb, 0, k->ncolor, write);
    }
    if (g_so.dev & SO_LOGIC_OP) {
        vkCmdSetLogicOpEnableEXT(cb, k->logic_op ? VK_TRUE : VK_FALSE);
        if (k->logic_op) vkCmdSetLogicOpEXT(cb, (VkLogicOp)(k->logic_op - 1));
    }

    /* the shading rate image a pipeline read (vk_vrs_pipeline): its palettes are set by the draws */
    if (g_so.dev & SO_VRS_NV) {
        vkCmdSetShadingRateImageEnableNV(cb, VK_TRUE);
        vkCmdSetCoarseSampleOrderNV(cb, VK_COARSE_SAMPLE_ORDER_TYPE_DEFAULT_NV, 0, NULL); /* a pipeline's default */
    } else if (g_so.dev & SO_VRS_KHR) {
        VkExtent2D one = {1, 1};
        VkFragmentShadingRateCombinerOpKHR ops[2] = {VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
                                                     VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR};
        vkCmdSetFragmentShadingRateKHR(cb, &one, ops);
    }
}

void vk_so_init(void) {
    if (!g_vk.has_shader_objects) return;
    g_so.dev = (g_vk.feat.tessellationShader ? SO_TESS : 0) | (g_vk.feat.geometryShader ? SO_GEOM : 0) |
               (g_vk.has_task ? SO_TASK : 0) | (g_vk.has_mesh ? SO_MESH : 0) | (g_vk.feat.depthClamp ? SO_CLAMP : 0) |
               (g_vk.has_depth_clip_enable ? SO_CLIP : 0) | (g_vk.has_provoking_last ? SO_PROVOKING : 0) |
               (g_vk.has_conservative ? SO_CONSERVATIVE : 0) | (g_vk.has_geometry_streams ? SO_STREAMS : 0) |
               (g_vk.feat.alphaToOne ? SO_ALPHA_ONE : 0) | (g_vk.sample_locations.available ? SO_LOCATIONS : 0) |
               (g_vk.has_mixed_samples ? SO_MIXED : 0) | (g_vk.has_coverage_to_color ? SO_TO_COLOR : 0) |
               (g_vk.feat.logicOp ? SO_LOGIC_OP : 0) | (g_vk.vrs == VK_VRS_NV ? SO_VRS_NV : 0) |
               (g_vk.vrs == VK_VRS_KHR ? SO_VRS_KHR : 0);
}
void vk_so_open(const char* dir, const char* tag) {
    if (!g_vk.has_shader_objects || !g_settings.vulkan_pipeline_cache) return;
    snx_vk_driver_path(g_so.bin_path, sizeof g_so.bin_path, dir, "vk_shaders", tag, ".bin");
    snx_path_native(g_so.bin_path);
    /* The file is of this device and driver (their uuid and version), and of this build, translator setup and
       robustness (the shader file's key, vk_pipeline_file_key) and set layouts (g_vk_dslayout_hash), mixed into the
       uuid it carries. A change of any of them makes every key of the old file one that no draw asks for again: the
       file is not read, and the next save writes this run's binaries alone, so the file does not grow with each
       build */
    char key[2048];
    size_t guest = 0;
    vk_pipeline_file_key(key, sizeof key, &guest);
    u64 tag64 = snx_fnv_step(snx_fnv1a(SNX_FNV_BASIS, key, strlen(key)), g_vk_dslayout_hash);
    u8 uuid[VK_UUID_SIZE];
    memcpy(uuid, g_vk.so_binary_uuid, VK_UUID_SIZE);
    for (int i = 0; i < 8; i++) uuid[i] ^= (u8)(tag64 >> (8 * i));
    size_t loaded = 0;
    pm_lock(&g_so.bin_lock);
    snx_vk_shader_binaries_init(&g_so.bin, uuid, g_vk.so_binary_version);
    snx_vk_shader_binaries_load(&g_so.bin, g_so.bin_path, &loaded);
    atomic_store_explicit(&g_so.bin_open, 1, memory_order_release);
    pm_unlock(&g_so.bin_lock);
    if (loaded) LOG("vulkan: shader object binaries loaded (%zu KB)\n", loaded >> 10);
}
void vk_so_save(void) {
    pm_lock(&g_so.bin_lock);
    if (atomic_load_explicit(&g_so.bin_open, memory_order_relaxed) && g_so.bin.dirty) {
        size_t saved;
        char why[512];
        if (snx_vk_shader_binaries_save(&g_so.bin, g_so.bin_path, &saved, why, sizeof why))
            WARN("vulkan: the shader object binaries could not be written to %s: %s\n", g_so.bin_path, why);
    }
    pm_unlock(&g_so.bin_lock);
}

void vk_so_stats(u64* made, u64* from_binary, u64* ms, u64* spirv_mb) {
    pm_lock(&g_so.spirv_lock);
    *spirv_mb = g_so.spirv_bytes >> 20;
    pm_unlock(&g_so.spirv_lock);
    *made = atomic_load_explicit(&g_so.made, memory_order_relaxed);
    *from_binary = atomic_load_explicit(&g_so.from_binary, memory_order_relaxed);
    *ms = atomic_load_explicit(&g_so.ns, memory_order_relaxed) / 1000000;
}
