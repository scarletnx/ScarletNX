/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* hmma_mode (settings.txt): the way an SM86 compute program's HMMA takes off the device's matrix unit, settled at the
   renderer's start, before the first translation and the pipeline file's key (vk_pipeline_init). 1 to 5 is that way.
   Auto (0) reads the way kept for this GPU and driver in the shader cache folder's hmma.<driver>.txt; without one, or
   with one of another driver build, it times the HMMA bench (shader.h) in each way and keeps the fastest
   (shader_hmma_bench_pick). The ways' speeds differ by tens of times between GPUs (settings.txt's numbers), and no
   property of the device tells them. Each way's module is translated as a game's program is (vk_translate_spirv) and
   run here on the compute queue, which nothing else uses yet, as ftz_probe.c does. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/utilities/common.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/gpu/shader/shader.h"
#include "common/gpu/vk/vk_common.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/descriptors.h"

/* the calibrating dispatch's workgroups: an Adreno 610's gather took 684.9 ms for 64 of them, its staging 19.6 */
#define PICK_FIRST_GROUPS 8u
/* the fastest way's dispatch in the timed rounds, in ns: whole waves on every CU of a large GPU for nearly all of it,
   so that a dispatch's start is a small part of it, and the rounds of five ways a fraction of a second */
#define PICK_TARGET_NS    1000000.0
/* the rounds dropped for a size scaled again at most: a dispatch's time grows with its workgroups only once they fill
   the GPU and its clock is up, so a size scaled from a smaller one's time can still fall short */
#define PICK_SIZINGS      4u
#define PICK_MAX_GROUPS   65536u
#define PICK_ROUNDS       4u
/* a way's dispatches in a round, the first untimed: an idle GPU's clock comes up over the first few */
#define PICK_DISPATCHES   6u
/* a way whose calibrating dispatch took this many times the fastest's is not timed further */
#define PICK_DROP         4.0
/* the rounds stop once they have run this long, in ns, past the first kept, and the ways are compared on what they
   have; the modules' translation and pipelines before them are not counted, which a phone's driver takes seconds of */
#define PICK_BUDGET_NS    3000000000ull
/* the input and output addresses cbuf 0 holds: the module reads them, and its buffers are bound at their starts */
#define PICK_VA_IN        0x100000000ull
#define PICK_VA_OUT       0x200000000ull

typedef struct {
    VkBuffer buffer;
    DevMem memory;
    DevMemInfo info;
} Buf;
typedef struct {
    ShaderInfo si;
    VkPipeline pipe;
    VkDescriptorSet set;
    Buf marks;
    double first;                                   /* the calibrating dispatch's ns */
    double ns[PICK_ROUNDS * (PICK_DISPATCHES - 1)]; /* the timed dispatches */
    u32 n;
    char why[192]; /* why it is left out, empty while it is not */
} Way;
typedef struct {
    Way way[SH_HMMA_WAYS];
    u8 code[SH_HMMA_BENCH_BYTES];
    Buf cb, in, out, back;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkDescriptorPool pool;
    VkCommandPool cpool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkQueryPool queries;
    int timed; /* the compute queue has timestamps */
} Bench;

static int no_constants(u32 bank, u32 offset, u32* value) {
    (void)bank, (void)offset;
    *value = 0;
    return 0;
}

static int buf_make(Buf* b, u64 bytes, VkBufferUsageFlags usage, u32 at) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes, .usage = usage};
    DevMemWant want = {.at = at, .mapped = at != DM_DEVICE};
    return VK_OK(vk_buffer_create(&bi, &want, &b->buffer, &b->memory, &b->info));
}
static void buf_free(Buf* b) {
    if (b->buffer) vk_devmem_buffer_destroy(b->buffer, b->memory);
    b->buffer = VK_NULL_HANDLE;
}

static int cmp_double(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

/* one submission of n dispatches of x's pipeline, groups workgroups each, every one after the one before; their GPU
   times (the host's for the whole submission, split, where the queue has no timestamps) into ns. back: the first and
   last workgroups' outputs copied where the host reads them. 0 with why */
static int run(Bench* b, Way* x, u32 groups, u32 n, double* ns, int back, char* why, size_t wn) {
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (!VK_OK(vkResetCommandPool(g_vk.dev, b->cpool, 0)) || !VK_OK(vkBeginCommandBuffer(b->cmd, &begin))) {
        snprintf(why, wn, "its command buffer could not be begun");
        return 0;
    }
    vkCmdBindPipeline(b->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, x->pipe);
    vkCmdBindDescriptorSets(b->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, b->layout, 0, 1, &x->set, 0, NULL);
    if (b->timed) {
        vkCmdResetQueryPool(b->cmd, b->queries, 0, n + 1);
        vkCmdWriteTimestamp(b->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, b->queries, 0);
    }
    VkMemoryBarrier after = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                             .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                             .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT};
    for (u32 d = 0; d < n; d++) {
        vkCmdDispatch(b->cmd, groups, 1, 1);
        vkCmdPipelineBarrier(b->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &after, 0,
                             NULL, 0, NULL);
        if (b->timed) vkCmdWriteTimestamp(b->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, b->queries, d + 1);
    }
    if (back) {
        u64 group = SH_HMMA_BENCH_LOCAL * 4;
        VkBufferCopy copies[2] = {{0, 0, group}, {(u64)(groups - 1) * group, group, group}};
        vkCmdCopyBuffer(b->cmd, b->out.buffer, b->back.buffer, 2, copies);
        VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(b->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL,
                             0, NULL);
    }
    if (!VK_OK(vkEndCommandBuffer(b->cmd)) || !VK_OK(vkResetFences(g_vk.dev, 1, &b->fence))) {
        snprintf(why, wn, "its command buffer could not be ended");
        return 0;
    }
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &b->cmd};
    u64 t0 = plat_ns();
    vk_queue_lock(g_vk.compute);
    VkResult r = vkQueueSubmit(g_vk.compute, 1, &si, b->fence);
    vk_queue_unlock(g_vk.compute);
    if (!VK_OK(r)) {
        snprintf(why, wn, "its submission failed (%s)", snx_vk_result_str(r));
        return 0;
    }
    /* 10 s: a dispatch the calibration sized for 2 ms that takes this long is a lost device */
    r = vkWaitForFences(g_vk.dev, 1, &b->fence, VK_TRUE, 10000000000ull);
    if (!VK_OK(r)) {
        snprintf(why, wn, "its dispatches did not end in 10 s (%s)", snx_vk_result_str(r));
        return -1;
    }
    double host = (double)(plat_ns() - t0);
    if (b->timed) {
        u64 stamps[PICK_DISPATCHES + 1];
        r = vkGetQueryPoolResults(g_vk.dev, b->queries, 0, n + 1, sizeof stamps, stamps, 8,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        if (!VK_OK(r)) {
            snprintf(why, wn, "its timestamps could not be read (%s)", snx_vk_result_str(r));
            return 0;
        }
        u64 mask = g_vk.compute_timestamp_bits >= 64 ? ~0ull : (1ull << g_vk.compute_timestamp_bits) - 1;
        for (u32 d = 0; d < n; d++)
            ns[d] = (double)((stamps[d + 1] - stamps[d]) & mask) * (double)g_vk.props.limits.timestampPeriod;
    } else
        for (u32 d = 0; d < n; d++) ns[d] = host / n;
    if (back && !VK_OK(vk_devmem_invalidate(b->back.memory, 0, VK_WHOLE_SIZE))) {
        snprintf(why, wn, "its outputs could not be read");
        return 0;
    }
    return 1;
}

/* way w's module and pipeline, translated as a game's program is with w's flags, and its descriptors; 0 with x->why */
static int way_make(Bench* b, Way* x, int w, const u8* code, u32 bytes) {
    memset(&x->si, 0, sizeof x->si);
    x->si.cs_local[0] = (int)SH_HMMA_BENCH_LOCAL, x->si.cs_local[1] = x->si.cs_local[2] = 1;
    x->si.shared_size = (int)SH_HMMA_BENCH_SHARED;
    shader_hmma_take(w, (int)g_vk.props.limits.maxComputeSharedMemorySize);
    /* the way a program takes where the matrix unit does not: the bench's would take that unit, as its blocks are
       whole warps */
    int coop = g_spirv_translate.hmma_coop;
    u32 shapes = g_spirv_translate.hmma_coop_shapes;
    g_spirv_translate.hmma_coop = 0, g_spirv_translate.hmma_coop_shapes = 0;
    size_t words = 0;
    VkCbReads reads = {0};
    u32* spv = vk_translate_spirv(code, bytes, ST_CS, &x->si, &words, no_constants, &reads);
    g_spirv_translate.hmma_coop = coop, g_spirv_translate.hmma_coop_shapes = shapes;
    free(reads.r);
    if (!spv || x->si.unsupported) {
        snprintf(x->why, sizeof x->why, "not translated: %s",
                 x->si.first_unsupported[0] ? x->si.first_unsupported : "no reason given");
        free(spv);
        return 0;
    }
    /* the bindings set up here: cbuf 0, the input and the output, and the write log where the stores keep one */
    if (x->si.cbuf_mask != 1u || x->si.ngbuf != 2 || x->si.ntex || x->si.nimg || x->si.root_max || x->si.shared_spill) {
        snprintf(x->why, sizeof x->why,
                 "its module reads more than the bench binds (cbufs %#x, %d global buffers, %d textures, %d images, "
                 "root %u, spill %d)",
                 x->si.cbuf_mask, x->si.ngbuf, x->si.ntex, x->si.nimg, x->si.root_max, x->si.shared_spill);
        free(spv);
        return 0;
    }
    /* subgroups of 32, whole, as compute.c makes the pipelines of the programs that read other lanes */
    int warp = g_vk.has_cs_warp32;
    VkResult r = snx_vk_compute_pipeline_flags(g_vk.dev, VK_NULL_HANDLE, b->layout, spv, words * 4, warp ? 32 : 0, warp,
                                               0, &x->pipe);
    free(spv);
    if (!VK_OK(r)) {
        snprintf(x->why, sizeof x->why, "its pipeline was not made (%s)", snx_vk_result_str(r));
        return 0;
    }
    if (x->si.gbuf_marks &&
        !buf_make(&x->marks, (u64)x->si.nmarks * SH_WLOG_PART * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, DM_DEVICE)) {
        snprintf(x->why, sizeof x->why, "its write log's buffer was not made");
        return 0;
    }
    VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = b->pool,
                                       .descriptorSetCount = 1,
                                       .pSetLayouts = &b->dsl};
    if (!VK_OK(vkAllocateDescriptorSets(g_vk.dev, &dai, &x->set))) {
        snprintf(x->why, sizeof x->why, "its descriptor set was not made");
        return 0;
    }
    VkDescriptorBufferInfo infos[4] = {{b->cb.buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[4] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = x->set,
                                       .dstBinding = SNX_VKB_CB,
                                       .descriptorCount = 1,
                                       .descriptorType = vk_cbuf_type(0),
                                       .pBufferInfo = &infos[0]}};
    u32 nw = 1;
    for (int k = 0; k < 2; k++, nw++) {
        infos[nw] = (VkDescriptorBufferInfo){
            x->si.gbuf_cbuf_off[k] == SH_HMMA_BENCH_CB_OUT ? b->out.buffer : b->in.buffer, 0, VK_WHOLE_SIZE};
        writes[nw] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                            .dstSet = x->set,
                                            .dstBinding = SNX_VKB_SSBO + (u32)k,
                                            .descriptorCount = 1,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                            .pBufferInfo = &infos[nw]};
    }
    if (x->si.gbuf_marks) {
        infos[nw] = (VkDescriptorBufferInfo){x->marks.buffer, 0, VK_WHOLE_SIZE};
        writes[nw] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                            .dstSet = x->set,
                                            .dstBinding = SNX_VKB_WPAGES,
                                            .descriptorCount = 1,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                            .pBufferInfo = &infos[nw]};
        nw++;
    }
    vkUpdateDescriptorSets(g_vk.dev, nw, writes, 0, NULL);
    return 1;
}

/* what the bench needs besides each way's: cbuf 0, the input, the output, the copy the host reads, a layout of the
   bindings the module reads, a command buffer, a fence and the timestamps; 0 with why */
static int bench_open(Bench* b, char* why, size_t n) {
    static const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    u64 out_bytes = (u64)PICK_MAX_GROUPS * SH_HMMA_BENCH_LOCAL * 4;
    if (!buf_make(&b->cb, 256,
                  vk_cbuf_type(0) == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : storage,
                  DM_CACHED) ||
        !buf_make(&b->in, SH_HMMA_BENCH_INPUT * 4, storage, DM_CACHED) ||
        !buf_make(&b->out, out_bytes, storage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, DM_DEVICE) ||
        !buf_make(&b->back, 2 * SH_HMMA_BENCH_LOCAL * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, DM_CACHED)) {
        snprintf(why, n, "its buffers were not made (%llu MiB of output)", (unsigned long long)(out_bytes >> 20));
        return 0;
    }
    u32* cb = b->cb.info.ptr;
    memset(cb, 0, 256);
    cb[SH_HMMA_BENCH_CB_IN / 4] = (u32)PICK_VA_IN, cb[SH_HMMA_BENCH_CB_IN / 4 + 1] = (u32)(PICK_VA_IN >> 32);
    cb[SH_HMMA_BENCH_CB_OUT / 4] = (u32)PICK_VA_OUT, cb[SH_HMMA_BENCH_CB_OUT / 4 + 1] = (u32)(PICK_VA_OUT >> 32);
    shader_hmma_bench_inputs(b->in.info.ptr);
    if (!VK_OK(vk_devmem_flush(b->cb.memory, 0, VK_WHOLE_SIZE)) ||
        !VK_OK(vk_devmem_flush(b->in.memory, 0, VK_WHOLE_SIZE))) {
        snprintf(why, n, "its inputs could not be written");
        return 0;
    }
    VkDescriptorSetLayoutBinding bindings[4] = {
        {SNX_VKB_CB, vk_cbuf_type(0), 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {SNX_VKB_SSBO, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {SNX_VKB_SSBO + 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {SNX_VKB_WPAGES, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dl = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = bindings};
    /* the push constants compute.c's layout has, which a module may declare */
    VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 80};
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, SH_HMMA_WAYS},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * SH_HMMA_WAYS}};
    VkDescriptorPoolCreateInfo dp = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .maxSets = SH_HMMA_WAYS,
                                     .poolSizeCount = 2,
                                     .pPoolSizes = sizes};
    VkCommandPoolCreateInfo cp = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                  .queueFamilyIndex = g_vk.compute_family};
    VkFenceCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &dl, NULL, &b->dsl))) {
        snprintf(why, n, "its descriptor set layout was not made");
        return 0;
    }
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &b->dsl,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &pc};
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &pl, NULL, &b->layout)) ||
        !VK_OK(vkCreateDescriptorPool(g_vk.dev, &dp, NULL, &b->pool)) ||
        !VK_OK(vkCreateCommandPool(g_vk.dev, &cp, NULL, &b->cpool)) ||
        !VK_OK(vkCreateFence(g_vk.dev, &fc, NULL, &b->fence))) {
        snprintf(why, n, "its pipeline layout, pools or fence were not made");
        return 0;
    }
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = b->cpool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1};
    if (!VK_OK(vkAllocateCommandBuffers(g_vk.dev, &ca, &b->cmd))) {
        snprintf(why, n, "its command buffer was not made");
        return 0;
    }
    b->timed = g_vk.compute_timestamp_bits != 0 && g_vk.props.limits.timestampPeriod > 0;
    VkQueryPoolCreateInfo qc = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                .queryCount = PICK_DISPATCHES + 1};
    if (b->timed && !VK_OK(vkCreateQueryPool(g_vk.dev, &qc, NULL, &b->queries))) b->timed = 0;
    return 1;
}
static void bench_close(Bench* b) {
    for (int w = 0; w < SH_HMMA_WAYS; w++) {
        if (b->way[w].pipe) vkDestroyPipeline(g_vk.dev, b->way[w].pipe, NULL);
        buf_free(&b->way[w].marks);
    }
    if (b->queries) vkDestroyQueryPool(g_vk.dev, b->queries, NULL);
    if (b->fence) vkDestroyFence(g_vk.dev, b->fence, NULL);
    if (b->cpool) vkDestroyCommandPool(g_vk.dev, b->cpool, NULL);
    if (b->pool) vkDestroyDescriptorPool(g_vk.dev, b->pool, NULL);
    if (b->layout) vkDestroyPipelineLayout(g_vk.dev, b->layout, NULL);
    if (b->dsl) vkDestroyDescriptorSetLayout(g_vk.dev, b->dsl, NULL);
    buf_free(&b->cb);
    buf_free(&b->in);
    buf_free(&b->out);
    buf_free(&b->back);
}

/* the largest relative difference between two runs' copied outputs, NaN where one is not a number */
static double outputs_apart(const u32* a, const u32* b) {
    double worst = 0;
    for (u32 i = 0; i < 2 * SH_HMMA_BENCH_LOCAL; i++) {
        float x, y;
        memcpy(&x, &a[i], 4);
        memcpy(&y, &b[i], 4);
        double d = fabs((double)x - (double)y) / (fabs((double)x) > 1 ? fabs((double)x) : 1);
        if (!(d <= worst)) worst = d;
    }
    return worst;
}

/* Times each way (us[way], 0 for one left out, said in the log); 0 with why where the bench could not run, -1 where
   the device was lost in it */
static int bench_run(int rule, int half, double* us, u64* took, char* why, size_t n) {
    u32 ref[2 * SH_HMMA_BENCH_LOCAL];
    u64 t0 = plat_ns();
    int ok = 1, ref_way = 0;
    for (int w = 0; w < SH_HMMA_WAYS; w++) us[w] = 0;
    *took = 0;
    Bench* b = calloc(1, sizeof *b); /* its ways' ShaderInfo are large for a stack */
    if (!b) {
        snprintf(why, n, "no memory for it (%zu bytes)", sizeof *b);
        return 0;
    }
    u32 bytes = shader_hmma_bench_program(b->code);
    if (!bench_open(b, why, n)) ok = 0;
    /* the rule's way first: the others' outputs are compared with the first that ran, which is what the device ran
       before the bench where it ran */
    int order[SH_HMMA_WAYS - 1], nord = 0;
    order[nord++] = rule;
    for (int w = SH_HMMA_GATHER; w < SH_HMMA_WAYS; w++)
        if (w != rule) order[nord++] = w;
    double best = 0;
    for (int k = 0; k < nord && ok > 0; k++) {
        int w = order[k];
        Way* x = &b->way[w];
        if (half && (w == SH_HMMA_ASTAT || w == SH_HMMA_ASTAT_STAGED)) {
            snprintf(x->why, sizeof x->why, "hmma_f16's half sums take the place of A-stationary's");
            continue;
        }
        if (!way_make(b, x, w, b->code, bytes)) continue;
        double ns[2];
        int r = run(b, x, PICK_FIRST_GROUPS, 2, ns, 1, x->why, sizeof x->why);
        if (r < 0) {
            snprintf(why, n, "the %s way's %s", g_shader_hmma_names[w], x->why);
            ok = -1;
            break;
        }
        if (!r) continue;
        x->first = ns[1];
        if (!ref_way) {
            memcpy(ref, b->back.info.ptr, sizeof ref);
            ref_way = w;
        } else {
            double apart = outputs_apart(ref, b->back.info.ptr);
            if (!(apart <= 1e-4)) {
                snprintf(x->why, sizeof x->why, "its outputs differ from the %s way's by %.3g of their size",
                         g_shader_hmma_names[ref_way], apart);
                continue;
            }
        }
        if (!best || x->first < best) best = x->first;
    }
    shader_hmma_take(rule, (int)g_vk.props.limits.maxComputeSharedMemorySize);
    for (int w = SH_HMMA_GATHER; w < SH_HMMA_WAYS && ok > 0; w++) {
        Way* x = &b->way[w];
        if (x->pipe && !x->why[0] && x->first > PICK_DROP * best)
            snprintf(x->why, sizeof x->why, "its first dispatch took %.0f us, over %.0f times the fastest's %.0f",
                     x->first / 1000, PICK_DROP, best / 1000);
    }
    u32 groups = PICK_FIRST_GROUPS;
    if (ok > 0 && best > 0) {
        double want = PICK_FIRST_GROUPS * PICK_TARGET_NS / best;
        groups = want >= PICK_MAX_GROUPS ? PICK_MAX_GROUPS : want <= PICK_FIRST_GROUPS ? PICK_FIRST_GROUPS : (u32)want;
    }
    /* the rounds: the ways in turn, the order reversed every other round, as the clock still climbs in the first. The
       calibrating workgroups leave most of a large GPU idle, so their time is mostly a dispatch's start and the size
       scaled from it falls short of the target there (an RX 6700 XT's dispatches ran 0.1 to 0.2 ms), and a sizing
       dispatch of its own runs at an idle GPU's clock: while a round's fastest way takes under half the target, the
       size is scaled again from that round's times, which are dropped */
    u32 rounds = 0, passes = 0, resized = 0;
    u64 t_rounds = plat_ns();
    for (; rounds < PICK_ROUNDS && ok > 0 && best > 0 && (!rounds || plat_ns() - t_rounds < PICK_BUDGET_NS); passes++) {
        double fast = 0;
        for (int k = 0; k < SH_HMMA_WAYS - 1 && ok > 0; k++) {
            int w = passes & 1 ? SH_HMMA_WAYS - 1 - k : SH_HMMA_GATHER + k;
            Way* x = &b->way[w];
            double ns[PICK_DISPATCHES];
            if (!x->pipe || x->why[0]) continue;
            int r = run(b, x, groups, PICK_DISPATCHES, ns, 0, x->why, sizeof x->why);
            if (r < 0) {
                snprintf(why, n, "the %s way's %s", g_shader_hmma_names[w], x->why);
                ok = -1;
            } else if (r) {
                for (u32 d = 1; d < PICK_DISPATCHES; d++) x->ns[x->n++] = ns[d];
                qsort(ns + 1, PICK_DISPATCHES - 1, sizeof *ns, cmp_double);
                double mid = ns[1 + (PICK_DISPATCHES - 1) / 2];
                if (!fast || mid < fast) fast = mid;
            }
        }
        if (ok > 0 && fast > 0 && fast < PICK_TARGET_NS / 2 && groups < PICK_MAX_GROUPS && resized < PICK_SIZINGS &&
            plat_ns() - t_rounds < PICK_BUDGET_NS) {
            double want = groups * PICK_TARGET_NS / fast;
            groups = want >= PICK_MAX_GROUPS ? PICK_MAX_GROUPS : (u32)want;
            resized++;
            for (int w = SH_HMMA_GATHER; w < SH_HMMA_WAYS; w++) b->way[w].n = 0;
        } else
            rounds++;
    }
    if (ok > 0 && !best) {
        snprintf(why, n, "no way ran");
        ok = 0;
    }
    for (int w = SH_HMMA_GATHER; w < SH_HMMA_WAYS && ok > 0; w++) {
        Way* x = &b->way[w];
        if (x->why[0] || !x->n) continue;
        qsort(x->ns, x->n, sizeof *x->ns, cmp_double);
        us[w] = x->ns[x->n / 2] / 1000;
    }
    char line[1024];
    size_t at = 0;
    for (int w = SH_HMMA_GATHER; w < SH_HMMA_WAYS; w++)
        if (b->way[w].why[0] && at < sizeof line)
            at += (size_t)snprintf(line + at, sizeof line - at, "%s%s: %s", at ? "; " : "", g_shader_hmma_names[w],
                                   b->way[w].why);
    if (at) LOG("vulkan: the HMMA bench leaves out %s\n", line);
    if (ok > 0)
        LOG("vulkan: the HMMA bench ran %u workgroups a dispatch, %u rounds of %u dispatches a way (and %u at fewer "
            "workgroups, dropped), timed by %s\n",
            groups, rounds, PICK_DISPATCHES - 1, resized,
            b->timed ? "the GPU's timestamps" : "the host's clock (the compute queue has no timestamps)");
    /* a device lost in the bench keeps what it was using: nothing more is asked of it here */
    if (ok >= 0) {
        bench_close(b);
        free(b);
    }
    *took = plat_ns() - t0;
    return ok;
}

void vk_hmma_pick(const char* cache_dir, const char* driver_tag) {
    int shared = (int)g_vk.props.limits.maxComputeSharedMemorySize, mode = g_settings.hmma_mode;
    int subgroups = (g_vk_subgroup_stages & VK_SHADER_STAGE_COMPUTE_BIT) != 0;
    char way[256], why[256];
    if (!subgroups) return; /* no program reads other lanes: no way of HMMA's runs (shader_hmma_rule) */
    if (mode > SH_HMMA_AUTO && mode < SH_HMMA_WAYS) {
        shader_hmma_take(mode, shared);
        shader_hmma_way(way, sizeof way);
        LOG("vulkan: an SM86 compute program's HMMA: %s (hmma_mode=%d, %s; the device's rules take %s)\n", way, mode,
            g_shader_hmma_names[mode], g_shader_hmma_names[g_vk.hmma_rule]);
        return;
    }
    /* Auto: the rules' way stands until the bench or its file says another */
    shader_hmma_take(g_vk.hmma_rule, shared);
    int half = shader_hmma_f16_unused() == NULL && g_spirv_translate.hmma_f16;
    if (!g_shader_sm86) return; /* only an Ounce game's programs have HMMAs */
    char path[1024], key[160], uuid[2 * VK_UUID_SIZE + 1];
    for (int i = 0; i < VK_UUID_SIZE; i++) snprintf(uuid + 2 * i, 3, "%02x", g_vk.props.pipelineCacheUUID[i]);
    snprintf(key, sizeof key, "bench=%d driver=%08x uuid=%s hmma_f16=%d", SH_HMMA_BENCH_VERSION,
             (unsigned)g_vk.props.driverVersion, uuid, half);
    snx_vk_driver_path(path, sizeof path, cache_dir, "hmma", driver_tag, ".txt");
    snx_path_native(path);
    size_t size = 0;
    char* text = (char*)snx_read_file(path, &size);
    if (text) {
        char* z = realloc(text, size + 1); /* the parse reads a string */
        if (z) {
            text = z;
            text[size] = 0;
            int kept = shader_hmma_bench_parse(text, key, why, sizeof why);
            free(text);
            if (kept) {
                shader_hmma_take(kept, shared);
                shader_hmma_way(way, sizeof way);
                LOG("vulkan: an SM86 compute program's HMMA: %s (hmma_mode=0, Auto: %s, the HMMA bench's pick in %s)\n",
                    way, g_shader_hmma_names[kept], path);
                return;
            }
            LOG("vulkan: %s is not this GPU and driver's HMMA bench (%s): the bench runs again\n", path, why);
        } else
            free(text);
    }
    double us[SH_HMMA_WAYS];
    u64 took = 0;
    int ran = bench_run(g_vk.hmma_rule, half, us, &took, why, sizeof why);
    int pick = ran > 0 ? shader_hmma_bench_pick(us, g_vk.hmma_rule) : g_vk.hmma_rule;
    shader_hmma_take(pick, shared);
    shader_hmma_way(way, sizeof way);
    if (ran <= 0) {
        WARN("vulkan: the HMMA bench could not run (%s): an SM86 compute program's HMMA is %s, the device's rules' %s "
             "(hmma_mode=0, Auto)\n",
             why, way, g_shader_hmma_names[pick]);
        return; /* no file: the next start runs it again, and says so again */
    }
    char times[512];
    size_t at = 0;
    for (int w = SH_HMMA_GATHER; w < SH_HMMA_WAYS && at < sizeof times; w++)
        at +=
            (size_t)snprintf(times + at, sizeof times - at, "%s%s %.1f", at ? ", " : "", g_shader_hmma_names[w], us[w]);
    LOG("vulkan: the HMMA bench took %.0f ms at this first start of the GPU and driver: %s us a dispatch (0: left "
        "out); an SM86 compute program's HMMA: %s (hmma_mode=0, Auto: %s, the rules' %s)\n",
        (double)took / 1e6, times, way, g_shader_hmma_names[pick], g_shader_hmma_names[g_vk.hmma_rule]);
    char file[1024], note[160];
    snprintf(note, sizeof note, "the bench took %.0f ms; the device's rules take %s", (double)took / 1e6,
             g_shader_hmma_names[g_vk.hmma_rule]);
    size_t len = shader_hmma_bench_format(file, sizeof file, key, pick, us, note);
    if (!len || snx_vk_write_replace(path, file, len, why, sizeof why))
        WARN("vulkan: %s was not written (%s): the HMMA bench runs again at the next start\n", path,
             len ? why : "its text is too long");
}
