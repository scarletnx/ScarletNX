/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Shaders and pipelines. A guest shader is translated directly to SPIR-V and kept as a module.
   Every stage has a descriptor set of the same bindings, one set per stage
   (shader_vk_set). Pipelines are made per PipeKey: the shaders, vertex layout, attachment formats and blend state;
   everything else is dynamic state. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/shader/shader.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/pipeline_file.h"
#include "nx/gpu/vk/internal_spv.h"
#include "common/utilities/build_info.h"
#include "common/utilities/common.h"
#include <sys/stat.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

/* The caches live in shader_cache/ next to the games, shared by all of them, wherever the runtime was started from */
static char g_cache_dir[1100], g_pcache_path[1200], g_pcache_tmp[1230];
static int g_pcache_dirty; /* pipelines made since the cache was last written (atomic: it is saved outside vk_lock) */
static u64 g_pcache_used;  /* plat_ns when a pipeline last went into the cache (atomic) */
static void pcache_note(void) {
    __atomic_store_n(&g_pcache_used, plat_ns(), __ATOMIC_RELAXED);
    __atomic_store_n(&g_pcache_dirty, 1, __ATOMIC_RELAXED);
}

VkPipelineLayout g_vk_playout;
VkDescriptorSetLayout g_vk_dslayout[2][5];
static VkPipelineLayout g_playouts[1u << 5]; /* by the sets with texture arrays, made when first used */
static PMutex g_playout_lock = PMUTEX_INIT;  /* making one: the file's reading thread asks for them too */
u32 g_vk_stage_sets;
static VkPipelineCache g_pcache;
static void pf_start_load(void);
static void pf_merge(void);
static void pf_shader_record(const VkSh* s, const u32* spv, size_t nw);
/* the channel and stage whose bound constant buffers the translator reads, for the vk_shader_get call running */
static const GpuChannel* g_sh_ch;
static int g_sh_bind;

static int sh_cb_read(u32 idx, u32 off, u32* out) {
    if (idx >= 18) return 0;
    u64 a = g_sh_ch->cbind[g_sh_bind][idx];
    u32 sz = g_sh_ch->cbind_size[g_sh_bind][idx];
    u32 limit = sz ? sz : 65536;
    if (!a || limit < 4 || off > limit - 4) return 0;
    gmem_read(a + off, out, 4);
    return 1;
}
static VkCbReader g_rec_read;
static VkCbReads* g_rec;
static int g_rec_failed;
static int rec_read(u32 idx, u32 off, u32* out) {
    int ok = g_rec_read(idx, off, out);
    VkCbReads* reads = g_rec;
    if (reads->n % 16 == 0) {
        VkCbRead* grown = realloc(reads->r, sizeof *grown * (reads->n + 16));
        if (!grown) {
            g_rec_failed = 1;
            return ok;
        }
        reads->r = grown;
    }
    reads->r[reads->n++] = (VkCbRead){idx, off, (u32)ok, ok ? *out : 0};
    return ok;
}
const char* vk_corpus_dir(void) {
    static char dir[sizeof g_exedir + sizeof "captures/shaders"];
    static const char* at = (const char*)-1;
    if (at != (const char*)-1) return at;
    at = getenv("SNX_SHADER_CORPUS");
    if (!at && g_settings.shader_captures) { /* a phone's game has no environment to set */
        snprintf(dir, sizeof dir, "%scaptures", g_exedir);
        plat_mkdir(dir);
        snprintf(dir, sizeof dir, "%scaptures/shaders", g_exedir);
        plat_mkdir(dir);
        at = dir;
    }
    return at;
}

const char* vk_spirv_unavailable(const u32* spv, size_t words) {
    for (size_t at = 5; at < words;) { /* the capabilities come first */
        u32 width = spv[at] >> 16;
        if (!width || at + width > words || (spv[at] & 0xffffu) != 17) break;
        if (width == 2 && spv[at + 1] == 10 && !g_vk.feat.shaderFloat64) return "shaderFloat64 is unavailable";
        if (width == 2 && spv[at + 1] == 11 && !g_vk.feat.shaderInt64) return "shaderInt64 is unavailable";
        if (width == 2 && spv[at + 1] == 9 && !g_shader_float16) return "shaderFloat16 is unavailable";
        if (width == 2 && spv[at + 1] == 2 && !g_vk.feat.geometryShader) return "geometryShader is unavailable";
        if (width == 2 && spv[at + 1] == 3 && !g_vk.feat.tessellationShader) return "tessellationShader is unavailable";
        if (width == 2 && spv[at + 1] == 25 && !g_vk.feat.shaderImageGatherExtended)
            return "shaderImageGatherExtended is unavailable";
        if (width == 2 && spv[at + 1] == 32 && !g_vk.feat.shaderClipDistance)
            return "shaderClipDistance is unavailable";
        if (width == 2 && spv[at + 1] == 35 && !g_vk.feat.sampleRateShading) return "sampleRateShading is unavailable";
        if (width == 2 && spv[at + 1] == 4427 && !g_vk.has_shader_draw_parameters)
            return "shaderDrawParameters is unavailable";
        if (width == 2 && spv[at + 1] == 5055 && !g_shader_clock_scopes) return "shaderClock is unavailable";
        at += width;
    }
    return NULL;
}

u32* vk_translate_spirv(const u8* code, u32 max_bytes, int stage, ShaderInfo* si, size_t* words, VkCbReader read,
                        VkCbReads* reads) {
    int (*old)(u32, u32, u32*) = g_shader_cb_read;
    g_rec_read = read;
    g_rec = reads;
    g_rec_failed = 0;
    g_shader_cb_read = rec_read;
    const VkShaderStageFlagBits stages[] = {VK_SHADER_STAGE_VERTEX_BIT,
                                            VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                            VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                                            VK_SHADER_STAGE_GEOMETRY_BIT,
                                            VK_SHADER_STAGE_FRAGMENT_BIT,
                                            VK_SHADER_STAGE_COMPUTE_BIT};
    VkShaderStageFlagBits shader_stage = stages[stage];
    u32* result = shader_to_spirv(code, max_bytes, stage, si, words, g_vk_flush_denorm, g_vk_preserve_infnan,
                                  g_vk_preserve_infnan64, (g_vk_subgroup_stages & shader_stage) != 0);
    g_shader_cb_read = old;
    g_rec = NULL;
    if (g_rec_failed) {
        free(result);
        return NULL;
    }
    if (result) {
        const char* reason = vk_spirv_unavailable(result, *words);
        /* Texture uploads and render targets are single-sampled. Reject a shader whose descriptor needs MS before
           the draw reaches vk_stage_set's null descriptor path. */
        for (int i = 0; i < si->ntex && !reason; i++)
            if (si->tex[i].flags & 2) reason = "multisample texture binding unavailable";
        if (reason) {
            snprintf(si->first_unsupported, sizeof si->first_unsupported, "%s", reason);
            si->unsupported = 1;
            free(result);
            return NULL;
        }
    }
    return result;
}

/* shader_hash with the jump tables of BRX read through read, as the translation reads them: without a reader the
   length stops at the first EXIT and leaves out code only a jump table reaches */
u64 vk_shader_hash(const u8* code, u32 max_bytes, u32* out_len, VkCbReader read) {
    int (*old)(u32, u32, u32*) = g_shader_cb_read;
    g_shader_cb_read = read;
    u64 h = shader_hash(code, max_bytes, out_len);
    g_shader_cb_read = old;
    return h;
}
int vk_cb_reads_hold(const VkCbReads* reads, VkCbReader read) {
    for (u32 i = 0; i < reads->n; i++) {
        const VkCbRead* c = &reads->r[i];
        u32 v = 0;
        int ok = read(c->idx, c->off, &v);
        if ((u32)ok != c->ok || (ok && v != c->v)) return 0;
    }
    return 1;
}

u32 g_vk_buffer_image_slots;

/* descriptors of each type in one set: the binding ranges in shader.h */
static VkDescriptorType binding_type(u32 i) {
    if (i == SNX_VKB_SMAP) return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    if (i == SNX_VKB_TEXARRAY || i == SNX_VKB_TEXDEPTH) return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    if (i == SNX_VKB_TEXFUNCS || i == SNX_VKB_TEXMAP) return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    if (i >= SNX_VKB_IMGBUF) return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    return i < SNX_VKB_SSBO  ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
           : i < SNX_VKB_TEX ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
           : i < SNX_VKB_IMG ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
           : i < SNX_VKB_RS  ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                             : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}
void vk_pool_sizes(VkDescriptorPoolSize* sz, u32 sets) {
    static const VkDescriptorType ty[VK_POOL_TYPES] = {
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER};
    int k;
    for (k = 0; k < VK_POOL_TYPES; k++) {
        u32 n = 0;
        u32 i;
        for (i = 0; i < SNX_VKB_END; i++)
            if (binding_type(i) == ty[k]) n += (i == SNX_VKB_TEXARRAY || i == SNX_VKB_TEXDEPTH) ? SNX_TEX_ARRAY : 1;
        sz[k] = (VkDescriptorPoolSize){ty[k], n * sets};
    }
}

VkPipelineLayout vk_playout(u32 arrays) {
    VkPipelineLayout* l = &g_playouts[arrays & ((1u << 5) - 1)];
    VkPipelineLayout made = __atomic_load_n(l, __ATOMIC_ACQUIRE);
    if (made) return made;
    pm_lock(&g_playout_lock);
    if (!*l) {
        VkDescriptorSetLayout sets[5];
        for (u32 s = 0; s < g_vk_stage_sets; s++) sets[s] = g_vk_dslayout[(arrays >> s) & 1][s];
        /* push constants: viewport/depth pc[0..3] at 0, fragment alpha test pc[4..6] and sprite T flip pc[7] at 16 */
        VkPushConstantRange pc = {VK_PUSH_STAGES, 0, 32};
        VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                         .setLayoutCount = g_vk_stage_sets,
                                         .pSetLayouts = sets,
                                         .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &pc};
        if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &pl, NULL, &made))) made = VK_NULL_HANDLE;
        __atomic_store_n(l, made, __ATOMIC_RELEASE);
    }
    made = *l;
    pm_unlock(&g_playout_lock);
    return made;
}

void vk_pipeline_init(void) {
    /* a layout per set, the same bindings, each visible to the stage that uses the set (shader_vk_set): within the
       per-stage limits a device has, which all stages seeing every set would multiply by five */
    static const VkShaderStageFlags vis[5] = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
                                              VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                              VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                                              VK_SHADER_STAGE_GEOMETRY_BIT};
    /* the tessellation and geometry stages' sets come after the first two, where the device binds that many */
    g_vk_stage_sets = g_vk.props.limits.maxBoundDescriptorSets >= 5 ? 5 : 2;
    for (int s = 0; s < 5; s++) {
        VkDescriptorSetLayoutBinding b[SNX_VKB_END];
        u32 count = 0;
        u32 image_slots = g_vk.props.limits.maxPerStageDescriptorStorageImages;
        u32 buffer_slots;
        u32 i;
        if (image_slots > g_vk.props.limits.maxDescriptorSetStorageImages)
            image_slots = g_vk.props.limits.maxDescriptorSetStorageImages;
        /* storage texel buffers count against the storage image limits, so buffer images get what images leave */
        buffer_slots = image_slots;
        if (image_slots > SNX_VKB_RS - SNX_VKB_IMG) image_slots = SNX_VKB_RS - SNX_VKB_IMG;
        buffer_slots -= image_slots;
        if (buffer_slots > SNX_VKB_TEXARRAY - SNX_VKB_IMGBUF) buffer_slots = SNX_VKB_TEXARRAY - SNX_VKB_IMGBUF;
        g_vk_buffer_image_slots = buffer_slots;
        /* A compact array holds the unique handles selected by all indexed tables. */
        if (g_vk.props.limits.maxPerStageDescriptorSamplers < SH_MAXTEX + 2 * SNX_TEX_ARRAY ||
            g_vk.props.limits.maxPerStageDescriptorSampledImages < SH_MAXTEX + 2 * SNX_TEX_ARRAY ||
            g_vk.props.limits.maxDescriptorSetSamplers < 5 * (SH_MAXTEX + 2 * SNX_TEX_ARRAY) ||
            g_vk.props.limits.maxDescriptorSetSampledImages < 5 * (SH_MAXTEX + 2 * SNX_TEX_ARRAY))
            g_shader_texture_arrays = 0;
        for (i = 0; i < SNX_VKB_END; i++) {
            int image = i >= SNX_VKB_IMG && i < SNX_VKB_RS;
            if (image && (s >= 2 || i - SNX_VKB_IMG >= image_slots)) continue; /* compute and fragment */
            if (i >= SNX_VKB_TEXARRAY && !g_shader_texture_arrays) continue;
            if (i >= SNX_VKB_IMGBUF && i < SNX_VKB_TEXARRAY && (s != 0 || i - SNX_VKB_IMGBUF >= buffer_slots)) continue;
            if (i >= SNX_VKB_IMGBUF && i < SNX_VKB_TEXARRAY) image = 1;
            b[count++] = (VkDescriptorSetLayoutBinding){
                .binding = i,
                .descriptorType = binding_type(i),
                .descriptorCount = (i == SNX_VKB_TEXARRAY || i == SNX_VKB_TEXDEPTH) ? SNX_TEX_ARRAY : 1,
                .stageFlags = image ? (s == 0 ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_FRAGMENT_BIT) : vis[s]};
        }
        for (int arrays = 0; arrays < 2; arrays++) {
            u32 n = count;
            while (!arrays && n && b[n - 1].binding >= SNX_VKB_TEXARRAY) n--; /* the arrays are the last bindings */
            VkDescriptorSetLayoutCreateInfo dl = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = n, .pBindings = b};
            if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &dl, NULL, &g_vk_dslayout[arrays][s])))
                fatal("vulkan: cannot create shader descriptor layout");
        }
    }
    {
        VkDescriptorPoolSize n[VK_POOL_TYPES];
        vk_pool_sizes(n, 1);
        const VkPhysicalDeviceLimits* l = &g_vk.props.limits;
        if (n[0].descriptorCount > l->maxPerStageDescriptorUniformBuffers ||
            n[1].descriptorCount > l->maxPerStageDescriptorStorageBuffers ||
            n[2].descriptorCount > l->maxPerStageDescriptorSamplers ||
            n[2].descriptorCount > l->maxPerStageDescriptorSampledImages ||
            n[3].descriptorCount > l->maxPerStageDescriptorStorageImages ||
            n[0].descriptorCount + n[1].descriptorCount + n[2].descriptorCount + n[3].descriptorCount + 8 >
                l->maxPerStageResources)
            WARN("vulkan: a stage's descriptor set (%u uniform and %u storage buffers, %u textures, %u images) is "
                 "past this GPU's per-stage limits (%u, %u, %u, %u): pipelines may fail\n",
                 n[0].descriptorCount, n[1].descriptorCount, n[2].descriptorCount, n[3].descriptorCount,
                 l->maxPerStageDescriptorUniformBuffers, l->maxPerStageDescriptorStorageBuffers,
                 l->maxPerStageDescriptorSamplers, l->maxPerStageDescriptorStorageImages);
    }
    g_vk_playout = vk_playout(0);
    if (!g_vk_playout) fatal("vulkan: cannot create shader pipeline layout");
    snprintf(g_cache_dir, sizeof g_cache_dir, "%sshader_cache", g_cachedir);
    plat_mkdir(g_cache_dir);
    snprintf(g_pcache_path, sizeof g_pcache_path, "%s/vk_pipelines.bin", g_cache_dir);
    /* the temporary name is the process's own: two games running at once would otherwise write the same file */
#if defined(_WIN32)
    unsigned pid = (unsigned)GetCurrentProcessId();
#else
    unsigned pid = (unsigned)getpid();
#endif
    snprintf(g_pcache_tmp, sizeof g_pcache_tmp, "%s.%u.tmp", g_pcache_path, pid);
    /* the driver's compiled pipelines from earlier runs; it ignores data from another device or driver version */
    VkPipelineCacheCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    PFileMap data;
    if (!plat_file_map(&data, g_pcache_path) && data.data) {
        ci.initialDataSize = (size_t)data.size;
        ci.pInitialData = data.data;
    }
    if (!VK_OK(vkCreatePipelineCache(g_vk.dev, &ci, NULL, &g_pcache)) && ci.initialDataSize) {
        ci.initialDataSize = 0;
        ci.pInitialData = NULL;
        VK_OK(vkCreatePipelineCache(g_vk.dev, &ci, NULL, &g_pcache));
    }
    if (ci.initialDataSize) LOG("vulkan: pipeline cache loaded (%zu KB)\n", ci.initialDataSize >> 10);
    plat_file_unmap(&data); /* the cache copied it, and a mapped file can't be replaced on Windows */
    pf_start_load();
}

/* ------------------------------------------------------------ shaders */

static VkSh* g_shh[1024];
/* the shader last found for an address and key, two per set. It is checked against memory once per submit: unchanged
   by write tracking since then, or else by the hash of its code */
typedef struct {
    u64 gva, gva_a, gw;
    u32 mask[20], epoch, wseen;
    int stage;
    VkSh* s;
} ShA;
static ShA g_sha[4096];

static int code_hash(u64 gva, u32 len, u64* hash) {
    u8* p = gmem_ptr(gva, len);
    if (p) {
        *hash = vk_hash(p, len);
        return 1;
    }
    u8* t = malloc(len ? len : 1);
    if (!t) return 0;
    gmem_read(gva, t, len);
    *hash = vk_hash(t, len);
    free(t);
    return 1;
}
static int code_same(const ShA* ca, u64 gva, u32 len, u64 qh) {
    if (!gpu_mem_dirty_since(ca->gw, gva, len) && vk_written_since(gmem_ptr(gva, len), len, ca->wseen) == 0) return 1;
    u64 hash;
    return code_hash(gva, len, &hash) && hash == qh;
}

/* for the stats line: pipelines made, the GPU thread's time waiting for new ones, shaders translated and the time
   their translation and modules took (all on the GPU thread) */
static u64 g_pipes_new, g_sh_new, g_sh_ns;
static u64 g_pipe_wait_ns; /* the GPU thread's and the recording thread's waits, added atomically */
static u64 g_pipe_make_ns; /* the workers' time making pipelines, added atomically */
static u64 g_pipe_hits;    /* pipelines the pipeline cache had */
VkSh* vk_shader_get(const GpuChannel* ch, u64 gva, u64 gva_a, int stage, u64 int_mask, u64 sint_mask, u32 rt_int,
                    u32 rt_sint, u32 clip, u64 dc_color, u32 dc_funcs, u32 unfed, const ShXfb* xfb) {
    pf_merge();
    g_sh_ch = ch;
    g_sh_bind = stage <= ST_FS ? stage : 0;
    u32 mask[20] = {(u32)int_mask,
                    (u32)(int_mask >> 32),
                    (u32)sint_mask,
                    (u32)(sint_mask >> 32),
                    rt_int,
                    rt_sint,
                    clip,
                    (u32)dc_color,
                    (u32)(dc_color >> 32),
                    dc_funcs,
                    unfed};
    if (stage == ST_GS && ch) memcpy(mask + 12, ch->r3d + 0x490, 8 * sizeof(u32));
    /* a capture's layout, in the words only the fragment stage's key uses otherwise */
    if (stage != ST_FS && xfb) {
        u64 xh = vk_hash(xfb, sizeof *xfb);
        mask[7] = (u32)xh | 1;
        mask[8] = (u32)(xh >> 32);
    }
    u32 mix = (u32)vk_hash(mask, 12 * sizeof(u32));
    if (stage == ST_GS) mix ^= (u32)vk_hash(mask + 12, 8 * sizeof(u32));
    /* the set from every bit of the key, mixed by multiplying: programs are 256-byte aligned, and a shift and xor of
       their addresses left most sets unused */
    u64 hk = (gva * 0x9E3779B97F4A7C15ull) ^ (gva_a * 0xC2B2AE3D27D4EB4Full) ^ ((u64)mix << 24) ^ (u64)stage;
    ShA* set = &g_sha[(u32)((hk * 0x9E3779B97F4A7C15ull) >> 52) & 4094];
    ShA* ca = NULL;
    for (int w = 0; w < 2 && !ca; w++)
        if (set[w].s && set[w].gva == gva && set[w].gva_a == gva_a && set[w].stage == stage &&
            !memcmp(set[w].mask, mask, sizeof mask))
            ca = &set[w];
    if (ca) {
        VkSh* s = ca->s;
        int same = ca->epoch == g_vk_epoch ||
                   (code_same(ca, gva, s->clen, s->qh) && (!gva_a || code_same(ca, gva_a, s->clen_a, s->qh_a)));
        if (same && ca->epoch != g_vk_epoch) {
            ca->epoch = g_vk_epoch;
            ca->wseen = vk_ww_now();
            ca->gw = gpu_mem_generation();
        }
        if (same && vk_cb_reads_hold(&s->cbr, sh_cb_read)) return s;
    } else /* the way used longest ago */
        ca = !set[0].s || (set[1].s && (i32)(set[0].epoch - set[1].epoch) <= 0) ? &set[0] : &set[1];
    u32 wseen = vk_ww_now();
    u64 gw = gpu_mem_generation();
    u8* code = malloc(0x10000);
    if (!code) return NULL;
    gmem_read_code(gva, code, 0x10000);
    u32 len, len_a = 0;
    u64 qh, qh_a = 0;
    vk_shader_hash(code, 0x10000, &len, sh_cb_read);
    qh = vk_hash(code, len);
    if (gva_a) { /* VertexA + VertexB: one merged program */
        u8* a = malloc(0x10000);
        if (!a) {
            free(code);
            return NULL;
        }
        gmem_read_code(gva_a, a, 0x10000);
        vk_shader_hash(a, 0x10000, &len_a, sh_cb_read);
        qh_a = vk_hash(a, len_a);
        u8* m = malloc(0x10000);
        if (!m) {
            free(a);
            free(code);
            return NULL;
        }
        /* the merge sizes A and B with shader_hash too */
        int (*old)(u32, u32, u32*) = g_shader_cb_read;
        g_shader_cb_read = sh_cb_read;
        int merged = shader_merge_dual(a, code, m, 0x10000);
        g_shader_cb_read = old;
        if (merged) {
            free(code);
            code = m;
        } else {
            free(m);
            WARN_LIMIT(8, "vulkan: VertexA %llx and VertexB %llx couldn't be merged: VertexB runs alone\n",
                       (unsigned long long)gva_a, (unsigned long long)gva);
        }
        free(a);
    }
    u32 mlen;
    u64 h = vk_shader_hash(code, 0x10000, &mlen, sh_cb_read);
    VkSh* s = NULL;
    /* one made for the same program: its whole code hashes the same (the merged program's, and each part's with its
       length, which ShA's checks compare memory with later), with the same key and constant buffer reads. Shaders of
       earlier runs (pf_merge) are found only so */
    for (VkSh* q = g_shh[h & 1023]; q && !s; q = q->next)
        if (q->hash == h && q->stage == stage && !memcmp(q->mask, mask, sizeof mask) && q->clen == len &&
            q->qh == qh && q->clen_a == len_a && q->qh_a == qh_a && vk_cb_reads_hold(&q->cbr, sh_cb_read))
            s = q;
    if (!s) {
        s = calloc(1, sizeof *s);
        if (!s) {
            free(code);
            return NULL;
        }
        s->hash = h;
        s->stage = stage;
        memcpy(s->mask, mask, sizeof mask);
        s->clen = len;
        s->qh = qh;
        s->clen_a = len_a;
        s->qh_a = qh_a;
        if (stage == ST_FS) {
            s->si.tex_int_mask = int_mask;
            s->si.tex_sint_mask = sint_mask;
            s->si.rt_int_mask = rt_int;
            s->si.rt_sint_mask = rt_sint;
            s->si.tex_dc_color_mask = dc_color;
            s->si.tex_dc_color_funcs = dc_funcs;
            s->si.in_unfed_mask = unfed;
        } else {
            s->si.attr_int_mask = int_mask;
            s->si.attr_sint_mask = sint_mask;
            s->si.rt_int_mask = rt_int; /* the tessellation or geometry primitive */
            s->si.clip_enable = clip;
            memcpy(s->si.gp_skip_mask, mask + 12, sizeof s->si.gp_skip_mask);
            if (xfb) s->si.xfb = *xfb;
        }
        if (vk_corpus_dir()) shader_corpus_save(vk_corpus_dir(), code, mlen, 0x10000, stage, &s->si, h ^ mix);
        u64 t0 = plat_ns();
        size_t nw = 0;
        u32* spv = vk_translate_spirv(code, 0x10000, stage, &s->si, &nw, sh_cb_read, &s->cbr);
        for (int i = 0; i < s->si.ntex; i++) s->indexed |= (s->si.tex[i].flags & SNX_TEX_INDEXED) != 0;
        if (spv && s->si.nimg) {
            u32 image_slots = g_vk.props.limits.maxPerStageDescriptorStorageImages;
            if (image_slots > g_vk.props.limits.maxDescriptorSetStorageImages)
                image_slots = g_vk.props.limits.maxDescriptorSetStorageImages;
            int needs_unformatted = 0;
            for (int i = 0; i < s->si.nimg; i++) needs_unformatted |= s->si.img[i].kind == 0;
            if (stage != ST_FS || s->si.nimg > (int)image_slots ||
                (needs_unformatted && !g_vk.feat.shaderStorageImageWriteWithoutFormat)) {
                free(spv);
                spv = NULL;
                snprintf(s->si.first_unsupported, sizeof s->si.first_unsupported,
                         "fragment storage image feature or descriptor limit");
            }
        }
        if (spv) {
            VkShaderModuleCreateInfo mi = {
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nw * 4, .pCode = spv};
            if (!VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &s->mod)))
                s->failed = 1;
            else
                pf_shader_record(s, spv, nw);
            free(spv);
        } else {
            s->failed = 1;
            WARN("vulkan: shader %016llx (stage %d) not translated: %s\n", (unsigned long long)h, stage,
                 s->si.first_unsupported[0] ? s->si.first_unsupported : "unsupported feature or float controls");
        }
        g_sh_new++;
        g_sh_ns += plat_ns() - t0;
        s->next = g_shh[h & 1023];
        g_shh[h & 1023] = s;
    }
    free(code);
    ca->gva = gva;
    ca->gva_a = gva_a;
    memcpy(ca->mask, mask, sizeof mask);
    ca->stage = stage;
    ca->epoch = g_vk_epoch;
    ca->wseen = wseen;
    ca->gw = gw;
    ca->s = s;
    return s;
}

VkShaderModule vk_module_spv(const u32* spv, size_t nw) {
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nw * 4, .pCode = spv};
    VkShaderModule m = VK_NULL_HANDLE;
    VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &m));
    return m;
}
VkShaderModule vk_module_spv_location(const u32* source, size_t nw, u32 location) {
    u32* spv = malloc(nw * sizeof *spv);
    if (!spv) return VK_NULL_HANDLE;
    memcpy(spv, source, nw * sizeof *spv);
    int found = 0;
    for (size_t i = 5; i < nw;) {
        u32 count = spv[i] >> 16, op = spv[i] & 0xffffu;
        if (!count || i + count > nw) break;
        if (op == 71 && count == 4 && spv[i + 2] == 30) {
            spv[i + 3] = location;
            found++;
        }
        i += count;
    }
    VkShaderModule result = found == 1 ? vk_module_spv(spv, nw) : VK_NULL_HANDLE;
    free(spv);
    return result;
}
/* an empty fragment shader for depth-only passes */
static VkShaderModule null_fs(void) {
    static VkShaderModule m;
    if (!m) m = vk_module_spv(snx_null_fs_spv, sizeof snx_null_fs_spv / sizeof *snx_null_fs_spv);
    return m;
}

/* ------------------------------------------------------------ pipelines */
/* A draw's new pipeline is made on the worker threads (pipeline_threads, vk_pipeline_worker_count): the GPU thread hands
   the recording thread a stand-in for it and decodes on, and the recording thread waits for the pipeline at its bind
   (vk_pipeline_resolve; without that thread, SNX_VK_RECORD=0, the draw waits on the GPU thread). The shader file's
   pipelines (pf_merge) are made on the workers from startup, and a draw that asks for one before then waits for it
   too, at the head of the workers' queue. Skipping a draw whose pipeline was still being made, as Eden's asynchronous
   shaders do, left
   one game's menus over a black backdrop and its loading screen without its map whenever their one draw was
   skipped, and failed 190 of edenprobe's cases: a draw is never skipped for its pipeline. What a worker needs that is
   made lazily, the layout and the empty fragment shader, is made on the GPU thread first */
typedef struct Pipe {
    PipeKey k;
    u64 h;
    VkPipeline p;      /* once ready */
    int ready;         /* set by the worker after p, with release order */
    VkPipelineLayout layout;
    VkShaderModule fs; /* k.fs, or the empty fragment shader */
    int prewarm;       /* one of the file's, made before a draw asked for it */
    int queued;        /* in the workers' queue (g_pj_lock): 1 a draw's, 2 the file's */
    u32 standin;       /* its index + 1 in the stand-ins' table (vk_pipeline_get_deferred), 0 for none yet */
    struct Pipe *next, *job;
} Pipe;
#define PIPEH 4096
static Pipe* g_pipeh[PIPEH];

/* The shaders and pipelines of earlier runs (pipeline_file.c), so that each run doesn't translate the same shaders
   and make the same pipelines again as views come into sight: one game made 440 pipelines and 500 shaders
   every session, each pipeline 3.4 ms on NVIDIA even from the driver's cache and 125 ms without, its draws skipped
   meanwhile. A thread of its own reads the file at startup, makes the modules and queues the pipelines to the workers
   behind any draw's; the first draw puts what it made in the tables (pf_merge), where vk_shader_get finds a shader
   only as it finds one made this run, by its code hashed in guest memory, its key and its constant buffer reads */
static struct {
    PFile file;
    char path[1400];
    int on;          /* the file is used (not with SNX_SHADER_CORPUS, which wants every shader translated) */
    PMutex lock;     /* loaded, and done's */
    PCond done;
    int loaded;      /* the reading thread is done with what follows (lock) */
    int merged;      /* what it made is in g_shh and g_pipeh (vk_lock) */
    VkSh *sh, *sh_tail; /* its shaders, in the file's order, linked by next until merged */
    Pipe* pipes;        /* its pipelines, linked by next until merged */
    u32 nsh, npipe;
    u64 made, hits; /* its pipelines made, and those the pipeline cache had (the workers add, atomically) */
} g_pf;
static void pf_pipe_record(const PipeKey* k);

static VkPipeline pipe_create(const Pipe* e) {
    const PipeKey* k = &e->k;
    VkPipelineShaderStageCreateInfo st[5];
    VkShaderModule modules[5];
    static const VkShaderStageFlagBits bits[5] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                                  VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                                                  VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
    VkPipelineTessellationStateCreateInfo ts;
    u32 nst = 0, s;
    modules[0] = k->vs;
    modules[1] = k->tcs;
    modules[2] = k->tes;
    modules[3] = k->gs;
    modules[4] = e->fs;
    for (s = 0; s < 5; s++) {
        if (!modules[s]) continue;
        memset(&st[nst], 0, sizeof st[nst]);
        st[nst].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[nst].stage = bits[s];
        st[nst].module = modules[s];
        st[nst].pName = "main";
        nst++;
    }
    memset(&ts, 0, sizeof ts);
    ts.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
    ts.patchControlPoints = k->patch;
    VkVertexInputBindingDescription vb[33];
    VkVertexInputBindingDivisorDescriptionEXT dv[33];
    u32 ndv = 0;
    for (u32 i = 0; i < k->nbind; i++) {
        vb[i] = (VkVertexInputBindingDescription){.binding = i,
                                                  .stride = 0, /* dynamic */
                                                  .inputRate = k->bind[i].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE
                                                                                  : VK_VERTEX_INPUT_RATE_VERTEX};
        if (k->bind[i].divisor > 1 && g_vk.has_divisor)
            dv[ndv++] = (VkVertexInputBindingDivisorDescriptionEXT){i, k->bind[i].divisor};
    }
    VkVertexInputAttributeDescription va[32];
    for (u32 i = 0; i < k->nattr; i++)
        va[i] = (VkVertexInputAttributeDescription){k->attr[i].loc, k->attr[i].binding, (VkFormat)k->attr[i].format,
                                                    k->attr[i].offset};
    VkPipelineVertexInputDivisorStateCreateInfoEXT dvi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
        .vertexBindingDivisorCount = ndv,
        .pVertexBindingDivisors = dv};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .pNext = ndv ? &dvi : NULL,
                                               .vertexBindingDescriptionCount = k->nbind,
                                               .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = k->nattr,
                                               .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = (VkPrimitiveTopology)k->topo_class};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .depthClampEnable = k->depth_clamp,
                                                 .polygonMode = (VkPolygonMode)k->polygon,
                                                 .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8];
    int changed = 0;
    for (u32 i = 0; i < k->ncolor; i++) {
        cba[i] = (VkPipelineColorBlendAttachmentState){.blendEnable = k->blend[i].en,
                                                       .srcColorBlendFactor = (VkBlendFactor)k->blend[i].sc,
                                                       .dstColorBlendFactor = (VkBlendFactor)k->blend[i].dc,
                                                       .colorBlendOp = (VkBlendOp)k->blend[i].cop,
                                                       .srcAlphaBlendFactor = (VkBlendFactor)k->blend[i].sa,
                                                       .dstAlphaBlendFactor = (VkBlendFactor)k->blend[i].da,
                                                       .alphaBlendOp = (VkBlendOp)k->blend[i].aop,
                                                       /* no fragment shader: its outputs would be undefined */
                                                       .colorWriteMask = k->fs ? k->blend[i].mask : 0};
        if (!g_vk.feat.dualSrcBlend) { /* the second source's factors become the first's */
            VkBlendFactor* f[4] = {&cba[i].srcColorBlendFactor, &cba[i].dstColorBlendFactor,
                                   &cba[i].srcAlphaBlendFactor, &cba[i].dstAlphaBlendFactor};
            for (int j = 0; j < 4; j++) {
                VkBlendFactor o = *f[j];
                *f[j] = o == VK_BLEND_FACTOR_SRC1_COLOR             ? VK_BLEND_FACTOR_SRC_COLOR
                        : o == VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR ? VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR
                        : o == VK_BLEND_FACTOR_SRC1_ALPHA           ? VK_BLEND_FACTOR_SRC_ALPHA
                        : o == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
                                                                    : o;
                changed |= *f[j] != o;
            }
        }
    }
    if (!g_vk.feat.independentBlend) /* every attachment blends as the first */
        for (u32 i = 1; i < k->ncolor; i++) {
            changed |= memcmp(&cba[i], &cba[0], sizeof cba[0]) != 0;
            cba[i] = cba[0];
        }
    if (changed) {
        LOG_LIMIT(
            1, "vulkan: blending that needs dual-source or independent blend, which this GPU lacks, is simplified\n");
    }
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = k->ncolor,
                                              .pAttachments = cba};
    VkDynamicState dyn[21 + 2] = {VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,
                                  VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
                                  VK_DYNAMIC_STATE_CULL_MODE,
                                  VK_DYNAMIC_STATE_FRONT_FACE,
                                  VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
                                  VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
                                  VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                                  VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                                  VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE,
                                  VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
                                  VK_DYNAMIC_STATE_STENCIL_OP,
                                  VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
                                  VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
                                  VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
                                  VK_DYNAMIC_STATE_LINE_WIDTH,
                                  VK_DYNAMIC_STATE_DEPTH_BIAS,
                                  VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                  VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                                  VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                  VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                  VK_DYNAMIC_STATE_DEPTH_BOUNDS};
    u32 ndyn = 21; /* the states above, which every pipeline has */
    /* without the feedback loop's dynamic state, a pipeline enables the loop with a flag (k->feedback) instead */
    if (g_vk.has_feedback_dynamic) dyn[ndyn++] = VK_DYNAMIC_STATE_ATTACHMENT_FEEDBACK_LOOP_ENABLE_EXT;
    /* the stride is set by vkCmdBindVertexBuffers2, which a draw without vertex buffers never calls: a pipeline without
       bindings leaves it out */
    if (k->nbind) dyn[ndyn++] = VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE;
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = ndyn,
                                           .pDynamicStates = dyn};
    VkFormat cf[8];
    for (u32 i = 0; i < k->ncolor; i++) cf[i] = (VkFormat)k->color[i];
    /* whether the pipeline cache had it, for the stats line (core in Vulkan 1.3) */
    VkPipelineCreationFeedback made = {0};
    VkPipelineCreationFeedbackCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
                                               .pPipelineCreationFeedback = &made};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .pNext = &fi,
                                        .colorAttachmentCount = k->ncolor,
                                        .pColorAttachmentFormats = cf,
                                        .depthAttachmentFormat = (VkFormat)k->depth,
                                        .stencilAttachmentFormat = (VkFormat)k->stencil};
    VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                       .pNext = &ri,
                                       .flags =
                                           k->feedback ? VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT : 0,
                                       .stageCount = nst,
                                       .pStages = st,
                                       .pVertexInputState = &vi,
                                       .pInputAssemblyState = &ia,
                                       .pTessellationState = k->tes ? &ts : NULL,
                                       .pViewportState = &vp,
                                       .pRasterizationState = &rs,
                                       .pMultisampleState = &ms,
                                       .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb,
                                       .pDynamicState = &dy,
                                       .layout = e->layout};
    VkPipeline p = VK_NULL_HANDLE;
    VK_OK(vkCreateGraphicsPipelines(g_vk.dev, g_pcache, 1, &gi, NULL, &p));
    if (made.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT)
        __atomic_add_fetch(e->prewarm ? &g_pf.hits : &g_pipe_hits, 1, __ATOMIC_RELAXED);
    return p;
}
/* e's pipeline, made here. One a draw asked for is kept in the file (pf_pipe_record): once made when the GPU thread
   makes it (record), and when it queues it for a worker otherwise, as the file's tables are the GPU thread's */
static VkPipeline pipe_make(const Pipe* e, int record) {
    u64 t0 = plat_ns();
    VkPipeline p = pipe_create(e);
    pcache_note();
    if (e->prewarm)
        __atomic_add_fetch(&g_pf.made, 1, __ATOMIC_RELAXED);
    else {
        __atomic_add_fetch(&g_pipe_make_ns, plat_ns() - t0, __ATOMIC_RELAXED);
        if (p && record) pf_pipe_record(&e->k);
    }
    return p;
}

/* the workers' queues, and the wait for one pipeline to be ready. The file's pipelines a draw asked for are made first,
   in the order asked for; the rest of the file's (pf_load_pipe) after */
static PMutex g_pj_lock = PMUTEX_INIT;
static PCond g_pj_work, g_pj_done;
static Pipe *g_pj_head, *g_pj_tail; /* a draw's */
static Pipe *g_pw_head, *g_pw_tail; /* the file's */
static int g_pj_started, g_pj_threads;
static void pipe_worker(void* arg) {
    (void)arg;
    for (;;) {
        Pipe* e;
        VkPipeline p;
        pm_lock(&g_pj_lock);
        while (!g_pj_head && !g_pw_head) pc_wait(&g_pj_work, &g_pj_lock, -1);
        if (g_pj_head) {
            e = g_pj_head;
            g_pj_head = e->job;
            if (!g_pj_head) g_pj_tail = NULL;
        } else {
            e = g_pw_head;
            g_pw_head = e->job;
            if (!g_pw_head) g_pw_tail = NULL;
        }
        e->queued = 0;
        pm_unlock(&g_pj_lock);
        p = pipe_make(e, 0);
        pm_lock(&g_pj_lock);
        e->p = p;
        __atomic_store_n(&e->ready, 1, __ATOMIC_RELEASE);
        pc_broadcast(&g_pj_done);
        pm_unlock(&g_pj_lock);
    }
}
int vk_pipeline_worker_count(int setting, int cpus) {
    int n = cpus / 4;
    if (setting > 0) return setting;
    /* a quarter of the CPUs, two to six: the driver's compile is the CPU's work, which the game's threads need too,
       and a first run's bursts of new pipelines go to them side by side (vk_pipeline_get_deferred) */
    if (n < 2) n = 2;
    if (n > 6) n = 6;
    return n;
}
/* the workers, started the first time one is needed: how many there are (g_pj_lock) */
static int pipe_workers(void) {
    if (!g_pj_started) {
        int ids[256], n = vk_pipeline_worker_count(g_settings.pipeline_threads, plat_cpus(ids, 256));
        g_pj_started = 1;
        pc_init(&g_pj_work);
        pc_init(&g_pj_done);
        for (int i = 0; i < n; i++)
            if (!plat_thread(pipe_worker, NULL, (size_t)1 << 20)) g_pj_threads++;
    }
    return g_pj_threads;
}
/* e at the end of a queue (g_pj_lock): 1 a draw's, 2 the file's */
static void pipe_queue(Pipe* e, int queue) {
    Pipe **head = queue == 1 ? &g_pj_head : &g_pw_head, **tail = queue == 1 ? &g_pj_tail : &g_pw_tail;
    e->job = NULL;
    if (*tail)
        (*tail)->job = e;
    else
        *head = e;
    *tail = e;
    e->queued = queue;
    pc_signal(&g_pj_work);
}
/* a pipeline of the file that a draw asks for before a worker took it goes to the draws' queue: 1 when it did */
static int pipe_promote(Pipe* e) {
    int moved = 0;
    pm_lock(&g_pj_lock);
    if (e->queued == 2) {
        Pipe *q, *before = NULL;
        for (q = g_pw_head; q != e; q = q->job) before = q;
        if (before)
            before->job = e->job;
        else
            g_pw_head = e->job;
        if (g_pw_tail == e) g_pw_tail = before;
        pipe_queue(e, 1);
        moved = 1;
    }
    pm_unlock(&g_pj_lock);
    return moved;
}
/* e's pipeline once ready, waiting at most ns for it (a negative ns: until it is) */
static VkPipeline pipe_wait(Pipe* e, i64 ns) {
    u64 until = ns < 0 ? 0 : plat_ns() + (u64)ns;
    pm_lock(&g_pj_lock);
    while (!e->ready) {
        u64 now = plat_ns();
        if (ns >= 0 && now >= until) break;
        pc_wait(&g_pj_done, &g_pj_lock, ns < 0 ? -1 : (i64)(until - now));
    }
    pm_unlock(&g_pj_lock);
    return __atomic_load_n(&e->ready, __ATOMIC_ACQUIRE) ? e->p : VK_NULL_HANDLE;
}
/* Stand-ins for pipelines still being made, which the GPU thread hands the recording thread in their place
   (vk_pipeline_get_deferred): PIPE_STANDIN in the top bits, which no handle has (record.c's REC_STANDIN tells the same
   of descriptor sets), and the pipeline's index in this table, pages the GPU thread adds before a stand-in naming
   them goes through the ring. Pipelines live as long as the run, so a stand-in does too */
#define PIPE_STANDIN   0x5EC6000000000000ull
#define PSTAND_PAGE    1024
static Pipe** g_pstand[1024];
static u32 g_npstand;
static VkPipeline pipe_standin(Pipe* p) {
    if (!p->standin) {
        u32 page = g_npstand / PSTAND_PAGE;
        if (page >= sizeof g_pstand / sizeof *g_pstand) return VK_NULL_HANDLE;
        if (!g_pstand[page] && !(g_pstand[page] = calloc(PSTAND_PAGE, sizeof **g_pstand))) return VK_NULL_HANDLE;
        g_pstand[page][g_npstand % PSTAND_PAGE] = p;
        p->standin = ++g_npstand;
    }
    return (VkPipeline)(uintptr_t)(PIPE_STANDIN | p->standin);
}
/* the GPU thread's wait for p, made by a worker */
static VkPipeline pipe_wait_here(Pipe* p) {
    u64 t0 = plat_ns();
    gpu_wait_note("a pipeline being made", p->h);
    VkPipeline got = pipe_wait(p, -1);
    gpu_wait_note(NULL, 0);
    __atomic_add_fetch(&g_pipe_wait_ns, plat_ns() - t0, __ATOMIC_RELAXED);
    return got;
}
/* k's entry, found or added (NULL with no memory): *found says whether it was there */
static Pipe* pipe_entry(const PipeKey* k, int* found) {
    pf_merge();
    u64 h = vk_hash(k, sizeof *k);
    u32 b = (u32)(h ^ (h >> 32)) & (PIPEH - 1);
    for (Pipe* p = g_pipeh[b]; p; p = p->next)
        if (p->h == h && !memcmp(&p->k, k, sizeof *k)) {
            *found = 1;
            return p;
        }
    Pipe* p = calloc(1, sizeof *p);
    *found = 0;
    if (!p) return NULL;
    p->k = *k;
    p->h = h;
    p->layout = vk_playout(k->arrays);
    p->fs = k->fs ? k->fs : null_fs();
    p->next = g_pipeh[b];
    g_pipeh[b] = p;
    g_pipes_new++;
    return p;
}
/* the pipeline for k: made here when new, waited for when one of the file's is still being made */
VkPipeline vk_pipeline_get(const PipeKey* k) {
    int found;
    Pipe* p = pipe_entry(k, &found);
    if (!p) return VK_NULL_HANDLE;
    if (found) {
        if (__atomic_load_n(&p->ready, __ATOMIC_ACQUIRE)) return p->p;
        if (p->prewarm) pipe_promote(p);
        return pipe_wait_here(p);
    }
    u64 t0 = plat_ns();
    p->p = pipe_make(p, 1);
    __atomic_store_n(&p->ready, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_pipe_wait_ns, plat_ns() - t0, __ATOMIC_RELAXED);
    return p->p;
}
/* A draw's pipeline when its commands go through the recording thread: the pipeline if it is ready, otherwise a
   stand-in, with the pipeline made on a worker in the order draws asked for them. The GPU thread goes on with the
   frame and the recording thread waits for each pipeline when it reaches its bind (vk_pipeline_resolve). A burst of
   new pipelines (a first run makes hundreds as views come into sight, 87 ms each on one driver without its cache)
   is made by the workers side by side while the frame is decoded, where the GPU thread made them one after another
   and stopped the game for 45 s once. No draw is skipped for them */
VkPipeline vk_pipeline_get_deferred(const PipeKey* k) {
    if (!vk_rec_threaded()) return vk_pipeline_get(k);
    int found;
    Pipe* p = pipe_entry(k, &found);
    if (!p) return VK_NULL_HANDLE;
    if (found) {
        if (__atomic_load_n(&p->ready, __ATOMIC_ACQUIRE)) return p->p;
        if (p->prewarm) pipe_promote(p);
    } else {
        int queued = 0;
        pf_pipe_record(&p->k);
        pm_lock(&g_pj_lock);
        if (pipe_workers()) {
            pipe_queue(p, 1);
            queued = 1;
        }
        pm_unlock(&g_pj_lock);
        if (!queued) { /* no worker: made here */
            u64 t0 = plat_ns();
            p->p = pipe_make(p, 0);
            __atomic_store_n(&p->ready, 1, __ATOMIC_RELEASE);
            __atomic_add_fetch(&g_pipe_wait_ns, plat_ns() - t0, __ATOMIC_RELAXED);
            return p->p;
        }
    }
    VkPipeline s = pipe_standin(p);
    return s ? s : pipe_wait_here(p);
}
/* on the recording thread: the pipeline a handle from vk_pipeline_get_deferred names, waited for while a worker makes
   it. VK_NULL_HANDLE when it couldn't be made */
VkPipeline vk_pipeline_resolve(VkPipeline h) {
    u64 v = (u64)(uintptr_t)h;
    if ((v & ~(u64)0xFFFFFFFFull) != PIPE_STANDIN) return h;
    u32 i = (u32)v - 1;
    Pipe* p = g_pstand[i / PSTAND_PAGE][i % PSTAND_PAGE];
    if (__atomic_load_n(&p->ready, __ATOMIC_ACQUIRE)) return p->p;
    u64 t0 = plat_ns();
    VkPipeline got = pipe_wait(p, -1);
    __atomic_add_fetch(&g_pipe_wait_ns, plat_ns() - t0, __ATOMIC_RELAXED);
    return got;
}
void vk_pipeline_stats(u64* pipes, u64* hits, u64* make_ms, u64* wait_ms, u64* shaders, u64* shader_ms) {
    *pipes = g_pipes_new;
    *hits = __atomic_load_n(&g_pipe_hits, __ATOMIC_RELAXED);
    *make_ms = __atomic_load_n(&g_pipe_make_ns, __ATOMIC_RELAXED) / 1000000;
    *wait_ms = __atomic_load_n(&g_pipe_wait_ns, __ATOMIC_RELAXED) / 1000000;
    *shaders = g_sh_new;
    *shader_ms = g_sh_ns / 1000000;
}

VkPipelineCache vk_pipeline_cache(void) {
    pcache_note();
    return g_pcache;
}
/* writes the pipeline cache when pipelines were made since the last write. Asked every few seconds and at exit (the
   renderer's save), outside vk_lock: the driver synchronizes the cache itself. The driver holds the cache's lock while
   it copies the cache out, so a pipeline made meanwhile waits for the whole copy, and the copies came every five seconds
   while pipelines were being made: a pipeline once waited 3.2 s with recording stopped behind it. So the asking every
   few seconds writes it only after PCACHE_QUIET_NS without a new pipeline, and at exit it is written regardless. Saves
   one at a time, so the one at exit waits for one in progress to finish its file before the process ends */
#define PCACHE_QUIET_NS 20000000000ull
static PMutex g_psave = PMUTEX_INIT;
void vk_pipeline_save(int at_exit) {
    if (!at_exit && plat_ns() - __atomic_load_n(&g_pcache_used, __ATOMIC_RELAXED) < PCACHE_QUIET_NS) return;
    pm_lock(&g_psave);
    if (!g_pcache || !__atomic_exchange_n(&g_pcache_dirty, 0, __ATOMIC_RELAXED)) {
        pm_unlock(&g_psave);
        return;
    }
    size_t n = 0;
    void* d = NULL;
    if (vkGetPipelineCacheData(g_vk.dev, g_pcache, &n, NULL) == VK_SUCCESS && n) d = malloc(n);
    if (d && vkGetPipelineCacheData(g_vk.dev, g_pcache, &n, d) == VK_SUCCESS) {
        FILE* f = fopen(g_pcache_tmp, "wb");
        if (f) {
            int ok = fwrite(d, 1, n, f) == n;
            if (fclose(f)) ok = 0;
            if (ok)
                plat_replace_file(g_pcache_tmp, g_pcache_path); /* the old cache stays whole until this */
            else
                remove(g_pcache_tmp);
        }
    }
    free(d);
    pm_unlock(&g_psave);
}

/* ------------------------------------------------------------ the shaders and pipelines of earlier runs */
/* a module's handle as the file knows it, and back (a pointer, or a 64-bit integer on 32-bit hosts) */
static u64 mod_bits(VkShaderModule m) {
    u64 v = 0;
    memcpy(&v, &m, sizeof m);
    return v;
}
static VkShaderModule mod_handle(u64 v) {
    VkShaderModule m = VK_NULL_HANDLE;
    memcpy(&m, &v, sizeof m);
    return m;
}
typedef char pf_cbr_words[sizeof(VkCbRead) == 16 ? 1 : -1]; /* the file's reads are VkCbRead's four words */

/* s, made this run from spv: kept for the next one */
static void pf_shader_record(const VkSh* s, const u32* spv, size_t nw) {
    PfShader r;
    if (!g_pf.on || nw > 0xFFFFFFFFu) return;
    memset(&r, 0, sizeof r);
    r.hash = s->hash;
    r.qh = s->qh;
    r.qh_a = s->qh_a;
    r.stage = (u32)s->stage;
    r.clen = s->clen;
    r.clen_a = s->clen_a;
    r.ncbr = s->cbr.n;
    r.nspv = (u32)nw;
    memcpy(r.mask, s->mask, sizeof r.mask);
    memcpy(&r.si, &s->si, sizeof r.si);
    pf_shader(&g_pf.file, mod_bits(s->mod), &r, (const u32*)(const void*)s->cbr.r, spv);
}
/* a draw's pipeline, made: kept for the next run, its modules by their shaders' records (any thread) */
static void pf_pipe_record(const PipeKey* k) {
    PipeKey c;
    u64 m[5];
    if (!g_pf.on) return;
    m[0] = mod_bits(k->vs);
    m[1] = mod_bits(k->tcs);
    m[2] = mod_bits(k->tes);
    m[3] = mod_bits(k->gs);
    m[4] = mod_bits(k->fs);
    memcpy(&c, k, sizeof c);
    c.vs = c.tcs = c.tes = c.gs = c.fs = VK_NULL_HANDLE;
    pf_pipe(&g_pf.file, m, &c, sizeof c);
}

/* on the reading thread: a shader of the file, made into a VkSh as vk_shader_get makes one, but without translating */
static u64 pf_load_shader(void* ctx, const PfShader* r, const u32* cbr, const u32* spv) {
    VkSh* s;
    (void)ctx;
    if (r->si.ntex < 0 || r->si.ntex > SH_MAXTEX || vk_spirv_unavailable(spv, r->nspv)) return 0;
    s = calloc(1, sizeof *s);
    if (!s) return 0;
    if (r->ncbr) {
        s->cbr.r = malloc(r->ncbr * sizeof *s->cbr.r);
        if (!s->cbr.r) {
            free(s);
            return 0;
        }
        memcpy(s->cbr.r, cbr, r->ncbr * sizeof *s->cbr.r);
        s->cbr.n = r->ncbr;
    }
    s->hash = r->hash;
    s->stage = (int)r->stage;
    memcpy(s->mask, r->mask, sizeof s->mask);
    memcpy(&s->si, &r->si, sizeof s->si);
    s->clen = r->clen;
    s->qh = r->qh;
    s->clen_a = r->clen_a;
    s->qh_a = r->qh_a;
    for (int i = 0; i < s->si.ntex; i++) s->indexed |= (s->si.tex[i].flags & SNX_TEX_INDEXED) != 0;
    s->mod = vk_module_spv(spv, r->nspv);
    if (!s->mod) {
        free(s->cbr.r);
        free(s);
        return 0;
    }
    if (g_pf.sh_tail)
        g_pf.sh_tail->next = s;
    else
        g_pf.sh = s;
    g_pf.sh_tail = s;
    g_pf.nsh++;
    return mod_bits(s->mod);
}
/* on the reading thread: a pipeline of the file, whose shaders were made, queued to the workers behind the draws' */
static void pf_load_pipe(void* ctx, const u64 m[5], const void* key, u32 keylen) {
    Pipe* p;
    (void)ctx;
    if (keylen != sizeof p->k || !m[0]) return;
    p = calloc(1, sizeof *p);
    if (!p) return;
    memcpy(&p->k, key, sizeof p->k);
    if (p->k.nattr > 32 || p->k.nbind > 33 || p->k.ncolor > 8) { /* pipe_create's arrays */
        free(p);
        return;
    }
    p->k.vs = mod_handle(m[0]);
    p->k.tcs = mod_handle(m[1]);
    p->k.tes = mod_handle(m[2]);
    p->k.gs = mod_handle(m[3]);
    p->k.fs = mod_handle(m[4]);
    p->h = vk_hash(&p->k, sizeof p->k);
    for (Pipe* q = g_pf.pipes; q; q = q->next) /* the file has it twice */
        if (q->h == p->h && !memcmp(&q->k, &p->k, sizeof p->k)) {
            free(p);
            return;
        }
    p->layout = vk_playout(p->k.arrays);
    p->fs = p->k.fs ? p->k.fs : null_fs();
    p->prewarm = 1;
    pm_lock(&g_pj_lock);
    if (!pipe_workers()) { /* made when a draw asks for it, then */
        pm_unlock(&g_pj_lock);
        free(p);
        return;
    }
    pipe_queue(p, 2);
    pm_unlock(&g_pj_lock);
    p->next = g_pf.pipes; /* the workers don't use next */
    g_pf.pipes = p;
    g_pf.npipe++;
}

/* The file's key: whatever changes the SPIR-V translation makes or what a PipeKey means. The build (commit, compiler,
   host), and for a tree with changes, which has one commit for every build of it, the runtime's own file; the device
   and its driver; the features and limits the translator, vk_shader_get and pipe_create consult; the sizes of what
   records hold whole */
static void pf_key(char* out, size_t n) {
    const VkPhysicalDeviceProperties* d = &g_vk.props;
    const char* commit = snx_build_commit();
    size_t len = strlen(commit);
    char exe[64] = "", uuid[33];
    u32 caps[] = {(u32)g_vk.has_divisor,
                  (u32)g_vk.has_shader_draw_parameters,
                  (u32)g_vk.has_feedback,
                  (u32)g_vk.has_feedback_dynamic,
                  (u32)g_shader_texture_arrays,
                  (u32)g_shader_stage_point_size,
                  (u32)g_shader_output_layer,
                  (u32)g_shader_geometry,
                  (u32)g_shader_float16,
                  (u32)g_shader_tessellation,
                  (u32)g_shader_clock_scopes,
                  (u32)g_vk_flush_denorm,
                  (u32)g_vk_preserve_infnan,
                  (u32)g_vk_preserve_infnan64,
                  (u32)g_vk_subgroup_stages,
                  g_vk_stage_sets,
                  g_vk_buffer_image_slots,
                  d->limits.maxPerStageDescriptorStorageImages,
                  d->limits.maxDescriptorSetStorageImages};
    /* a tree with changes builds under one commit name: the runtime's own bytes tell those builds apart. Not its
       modification time, which a copy of the same runtime (each game has one) changes */
    if (len >= 6 && !strcmp(commit + len - 6, "-dirty")) {
        char* self = snx_executable("scarletnx");
        PFileMap m;
        if (self && !plat_file_map(&m, self) && m.data) {
            snprintf(exe, sizeof exe, " exe=%016llx", (unsigned long long)vk_hash(m.data, (size_t)m.size));
            plat_file_unmap(&m);
        }
        free(self);
    }
    for (int i = 0; i < 16; i++) snprintf(uuid + 2 * i, 3, "%02x", d->pipelineCacheUUID[i]);
    snprintf(out, n, "%s%s gpu=%04x:%04x driver=%u api=%u uuid=%s features=%016llx caps=%016llx sizes=%u,%u,%u",
             snx_build_info(), exe, d->vendorID, d->deviceID, d->driverVersion, d->apiVersion, uuid,
             (unsigned long long)vk_hash(&g_vk.feat, sizeof g_vk.feat), (unsigned long long)vk_hash(caps, sizeof caps),
             (unsigned)sizeof(ShaderInfo), (unsigned)sizeof(PipeKey), (unsigned)sizeof(PfShader));
}

static void pf_load(void* arg) {
    PfLoad load = {pf_load_shader, pf_load_pipe, NULL};
    char key[1024];
    u32 loaded[2];
    u64 t0 = plat_ns();
    (void)arg;
    pf_key(key, sizeof key);
    pf_open(&g_pf.file, g_pf.path, key, &load, loaded);
    LOG("vulkan: %u shaders and %u pipelines of earlier runs loaded from %s in %llu ms, the pipelines to be made in "
        "the background\n",
        g_pf.nsh, g_pf.npipe, g_pf.path, (unsigned long long)((plat_ns() - t0) / 1000000));
    pm_lock(&g_pf.lock);
    g_pf.loaded = 1;
    pc_broadcast(&g_pf.done);
    pm_unlock(&g_pf.lock);
}
/* at the end of vk_pipeline_init: the file is read on a thread of its own, so startup doesn't wait for it */
static void pf_start_load(void) {
    pm_init(&g_pf.file.lock);
    pm_init(&g_pf.lock);
    pc_init(&g_pf.done);
    if (getenv("SNX_SHADER_CORPUS")) {
        LOG("vulkan: SNX_SHADER_CORPUS is set, so every shader is translated and none is kept for the next run\n");
        return;
    }
    g_pf.on = 1;
    null_fs(); /* made here, before the reading thread uses it */
    snprintf(g_pf.path, sizeof g_pf.path, "%s/%s.vkshaders", g_cache_dir, g_exename);
    if (plat_thread(pf_load, NULL, (size_t)1 << 20)) pf_load(NULL); /* no thread: read here */
}
/* the reading thread's shaders and pipelines into the tables, waiting for it the first time a draw needs them
   (vk_lock) */
static void pf_merge(void) {
    VkSh* s;
    Pipe* p;
    if (g_pf.merged) return;
    g_pf.merged = 1;
    if (!g_pf.on) return;
    pm_lock(&g_pf.lock);
    while (!g_pf.loaded) pc_wait(&g_pf.done, &g_pf.lock, -1);
    pm_unlock(&g_pf.lock);
    /* in the file's order, so the last made is found first, as in the run that made them */
    while ((s = g_pf.sh) != NULL) {
        g_pf.sh = s->next;
        s->next = g_shh[s->hash & 1023];
        g_shh[s->hash & 1023] = s;
    }
    g_pf.sh_tail = NULL;
    while ((p = g_pf.pipes) != NULL) {
        u32 b = (u32)(p->h ^ (p->h >> 32)) & (PIPEH - 1);
        g_pf.pipes = p->next;
        p->next = g_pipeh[b];
        g_pipeh[b] = p;
    }
}
void vk_pipeline_prewarm_stats(u64* made, u64* hits, u64* left) {
    *made = __atomic_load_n(&g_pf.made, __ATOMIC_RELAXED);
    *hits = __atomic_load_n(&g_pf.hits, __ATOMIC_RELAXED);
    *left = g_pf.merged && g_pf.npipe > *made ? g_pf.npipe - *made : 0; /* npipe is settled once merged */
}
