/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Graphics pipeline libraries (gpu/vk/pipeline_parts.c), with no GPU: the choice of the library path from the setting
   and what a stand-in device has, each condition that turns it off named in the log line, and the dynamic
   pre-rasterization state taken only where the device has its member, no extension ban holds for it, and the renderer
   uses its feature */
#include "nx/gpu/vk/pipeline_parts.h"
#include <stdio.h>
#include <string.h>

static struct {
    unsigned checks, failures;
    unsigned said;   /* the ban lines of the last vk_gpl_bans */
    char line[2048]; /* the last of them */
} g_test;
#define CHECK(c, what)                                                                                                 \
    do {                                                                                                               \
        g_test.checks++;                                                                                               \
        if (!(c)) {                                                                                                    \
            g_test.failures++;                                                                                         \
            printf("FAIL line %d: %s\n", __LINE__, what);                                                              \
        }                                                                                                              \
    } while (0)

/* the RTX 2080 of this machine: everything */
static VkGplDevice full(void) {
    VkGplDevice d = {.setting = 1,
                     .has_ext = 1,
                     .library = 1,
                     .fast_link = 1,
                     .independent_interpolation = 1,
                     .polygon = 1,
                     .depth_clamp = 1,
                     .provoking = 1,
                     .conservative_mode = 1,
                     .conservative_size = 1,
                     .patch = 1,
                     .depth_range = 1,
                     .depth_clip_control = 1,
                     .has_provoking_last = 1,
                     .has_conservative = 1};
    return d;
}

static void check_off(void) {
    static const struct {
        size_t offset;
        const char* says;
    } cases[] = {
        {offsetof(VkGplDevice, setting), "pipeline_libraries=0"},
        {offsetof(VkGplDevice, has_ext), "no VK_EXT_graphics_pipeline_library"},
        {offsetof(VkGplDevice, library), "without graphicsPipelineLibrary"},
        {offsetof(VkGplDevice, fast_link), "without fast linking"},
        {offsetof(VkGplDevice, independent_interpolation), "no independent interpolation"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        VkGplDevice d = full();
        int* field = (int*)((char*)&d + cases[i].offset);
        *field = 0;
        char line[512];
        u32 dyn = ~0u;
        int on = vk_gpl_choose(&d, &dyn, line, sizeof line);
        CHECK(!on, cases[i].says);
        CHECK(dyn == 0, "no dynamic state with the libraries off");
        CHECK(strstr(line, "libraries off") && strstr(line, cases[i].says), line);
    }
}

static void check_dynamic(void) {
    char line[512];
    u32 dyn = 0;
    VkGplDevice d = full();
    CHECK(vk_gpl_choose(&d, &dyn, line, sizeof line), "on with everything");
    CHECK(dyn == (VK_DYN_PR_POLYGON | VK_DYN_PR_DEPTH_CLAMP | VK_DYN_PR_PROVOKING | VK_DYN_PR_CONSERVATIVE |
                  VK_DYN_PR_PATCH | VK_DYN_PR_DEPTH_RANGE),
          "every pre-rasterization state dynamic");
    CHECK(strstr(line, "libraries on") && strstr(line, "polygon mode, depth clamp, provoking vertex, conservative "
                                                       "rasterization, patch control points, depth range"),
          line);
    /* each member alone off leaves its state in the part's key, and the rest dynamic */
    static const struct {
        size_t offset;
        u32 bit;
    } members[] = {
        {offsetof(VkGplDevice, polygon), VK_DYN_PR_POLYGON},
        {offsetof(VkGplDevice, depth_clamp), VK_DYN_PR_DEPTH_CLAMP},
        {offsetof(VkGplDevice, provoking), VK_DYN_PR_PROVOKING},
        {offsetof(VkGplDevice, has_provoking_last), VK_DYN_PR_PROVOKING},
        {offsetof(VkGplDevice, conservative_mode), VK_DYN_PR_CONSERVATIVE},
        {offsetof(VkGplDevice, conservative_size), VK_DYN_PR_CONSERVATIVE},
        {offsetof(VkGplDevice, has_conservative), VK_DYN_PR_CONSERVATIVE},
        {offsetof(VkGplDevice, patch), VK_DYN_PR_PATCH},
        {offsetof(VkGplDevice, depth_range), VK_DYN_PR_DEPTH_RANGE},
        {offsetof(VkGplDevice, depth_clip_control), VK_DYN_PR_DEPTH_RANGE},
    };
    for (size_t i = 0; i < sizeof members / sizeof *members; i++) {
        d = full();
        *(int*)((char*)&d + members[i].offset) = 0;
        CHECK(vk_gpl_choose(&d, &dyn, line, sizeof line), "on without one member");
        CHECK(!(dyn & members[i].bit), "the member's state static");
        CHECK((dyn | members[i].bit) == 63u, "the other states dynamic");
    }
    /* a device without EDS2 and EDS3: libraries with every pre-rasterization state static */
    d = full();
    d.polygon = d.depth_clamp = d.provoking = d.conservative_mode = d.conservative_size = d.patch = 0;
    d.depth_range = 0; /* the depth clip control alone gives no state */
    CHECK(vk_gpl_choose(&d, &dyn, line, sizeof line) && dyn == 0, "on, nothing dynamic");
    CHECK(strstr(line, "dynamic pre-rasterization state: none") != NULL, line);
}

/* Who makes a depth range of -1 to 1, in the log: the device with the state, else the last stage, with the first
   thing the device lacks for it named */
static void check_depth_range(void) {
    char line[512];
    u32 dyn = 0;
    VkGplDevice d = full();
    CHECK(vk_gpl_choose(&d, &dyn, line, sizeof line), "on with everything");
    vk_depth_range_line(&d, dyn, line, sizeof line);
    CHECK(strstr(line, "is the device's (VK_EXT_depth_clip_control, as dynamic state)") != NULL, line);
    static const struct {
        size_t offset;
        const char* says;
    } cases[] = {
        {offsetof(VkGplDevice, depth_clip_control), "(the device has no VK_EXT_depth_clip_control)"},
        {offsetof(VkGplDevice, depth_range), "(the device has no extendedDynamicState3DepthClipNegativeOneToOne)"},
        {offsetof(VkGplDevice, setting), "(graphics pipeline libraries are off, and their dynamic state with them)"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        d = full();
        *(int*)((char*)&d + cases[i].offset) = 0;
        vk_gpl_choose(&d, &dyn, line, sizeof line);
        CHECK(!(dyn & VK_DYN_PR_DEPTH_RANGE), cases[i].says);
        vk_depth_range_line(&d, dyn, line, sizeof line);
        CHECK(strstr(line, "is made by the last stage before rasterization, z = (z + w) / 2") &&
                  strstr(line, cases[i].says),
              line);
    }
}

/* the states a whole pipeline listed before pipeline libraries, in their order (pipe_create): a change of the order is a
   new pipeline cache entry for every pipeline */
static const VkDynamicState g_before[24] = {
    VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT, VK_DYNAMIC_STATE_CULL_MODE,
    VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
    VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE,
    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
    VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
    VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    VK_DYNAMIC_STATE_DEPTH_BOUNDS, VK_DYNAMIC_STATE_ATTACHMENT_FEEDBACK_LOOP_ENABLE_EXT,
    VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT};

static int has(const VkDynamicState* s, u32 n, VkDynamicState want) {
    for (u32 i = 0; i < n; i++)
        if (s[i] == want) return 1;
    return 0;
}

static void check_states(void) {
    VkDynamicState s[VK_PIPE_DYN_MAX];
    /* without libraries: the 21 of every pipeline, then the three the device and the key add, as before */
    for (u32 m = 0; m < 8; m++) {
        PipeDynIn in = {.feedback_dynamic = (int)(m & 1), .stride = (int)(m >> 1 & 1), .vertex_input = (int)(m >> 2)};
        u32 n = vk_pipe_dynamic(VK_PART_ALL, &in, s), at = 0, ok = 1;
        for (u32 i = 0; i < 24; i++) {
            if ((i == 21 && !in.feedback_dynamic) || (i == 22 && !in.stride) || (i == 23 && !in.vertex_input)) continue;
            ok &= at < n && s[at++] == g_before[i];
        }
        CHECK(ok && at == n, "a whole pipeline's states as before");
    }
    /* each pre-rasterization bit adds its states after those; patch control points only with tessellation */
    static const struct {
        u32 bit;
        VkDynamicState state;
    } pr[] = {{VK_DYN_PR_POLYGON, VK_DYNAMIC_STATE_POLYGON_MODE_EXT},
              {VK_DYN_PR_DEPTH_CLAMP, VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT},
              {VK_DYN_PR_PROVOKING, VK_DYNAMIC_STATE_PROVOKING_VERTEX_MODE_EXT},
              {VK_DYN_PR_CONSERVATIVE, VK_DYNAMIC_STATE_CONSERVATIVE_RASTERIZATION_MODE_EXT},
              {VK_DYN_PR_CONSERVATIVE, VK_DYNAMIC_STATE_EXTRA_PRIMITIVE_OVERESTIMATION_SIZE_EXT},
              {VK_DYN_PR_PATCH, VK_DYNAMIC_STATE_PATCH_CONTROL_POINTS_EXT},
              {VK_DYN_PR_DEPTH_RANGE, VK_DYNAMIC_STATE_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE_EXT}};
    for (u32 i = 0; i < sizeof pr / sizeof *pr; i++)
        for (int tess = 0; tess < 2; tess++) {
            PipeDynIn in = {.dyn_pr = pr[i].bit, .tess = tess};
            u32 n = vk_pipe_dynamic(VK_PART_ALL, &in, s);
            int want = pr[i].bit != VK_DYN_PR_PATCH || tess;
            CHECK(has(s, n, pr[i].state) == want, "a pre-rasterization state with its bit");
            CHECK(n == 21u + (want ? (pr[i].bit == VK_DYN_PR_CONSERVATIVE ? 2u : 1u) : 0u), "only that state added");
            CHECK(!want || s[n - 1] == pr[i].state || pr[i].bit == VK_DYN_PR_CONSERVATIVE, "after the others");
        }
    /* the four parts together list what the whole pipeline does, each state once, but the feedback loop's, which the
       fragment shader and fragment output parts both hold */
    PipeDynIn all = {.dyn_pr = 63, .feedback_dynamic = 1, .stride = 1, .vertex_input = 1, .tess = 1};
    VkDynamicState whole[VK_PIPE_DYN_MAX], part[4][VK_PIPE_DYN_MAX];
    u32 nwhole = vk_pipe_dynamic(VK_PART_ALL, &all, whole), np[4];
    for (u32 p = 0; p < 4; p++) np[p] = vk_pipe_dynamic(1u << p, &all, part[p]);
    for (u32 i = 0; i < nwhole; i++) {
        u32 in = 0;
        for (u32 p = 0; p < 4; p++) in += (u32)has(part[p], np[p], whole[i]);
        CHECK(in == (whole[i] == VK_DYNAMIC_STATE_ATTACHMENT_FEEDBACK_LOOP_ENABLE_EXT ? 2u : 1u), "in its part");
    }
    CHECK(np[0] + np[1] + np[2] + np[3] == nwhole + 1, "no state in a part that the whole pipeline lacks");
    /* the parts' states, by the specification's subsets */
    CHECK(has(part[0], np[0], VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY) && has(part[0], np[0], VK_DYNAMIC_STATE_VERTEX_INPUT_EXT),
          "the vertex input part holds the topology and the layout");
    CHECK(has(part[1], np[1], VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT) &&
              has(part[1], np[1], VK_DYNAMIC_STATE_POLYGON_MODE_EXT) &&
              has(part[1], np[1], VK_DYNAMIC_STATE_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE_EXT) &&
              has(part[1], np[1], VK_DYNAMIC_STATE_PATCH_CONTROL_POINTS_EXT),
          "the pre-rasterization part holds the viewports and the rasterization state");
    CHECK(has(part[2], np[2], VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE) && has(part[2], np[2], VK_DYNAMIC_STATE_STENCIL_OP),
          "the fragment shader part holds the depth and stencil tests");
    CHECK(np[3] == 2 && has(part[3], np[3], VK_DYNAMIC_STATE_BLEND_CONSTANTS), "the output part holds the blend constants");
}


/* ---- the key split (vk_pipe_split): a change of a whole pipeline's key changes the keys of the parts that hold that
   state and no other, so a new state of known shaders reuses the parts that compile shaders */
enum { F_VS, F_TCS, F_TES, F_GS, F_PATCH, F_POLYGON, F_CLAMP, F_PROVOKING, F_CONSERVATIVE, F_TOPO, F_VI, F_NCOLOR,
       F_COLOR, F_DEPTH, F_STENCIL, F_BLEND, F_MASK, F_LOGIC, F_SAMPLES, F_SAMPLE_OFF, F_A2C, F_A2O, F_COVERAGE,
       F_ADVANCED, F_SHADING, F_FEEDBACK, F_ARRAYS_VS, F_ARRAYS_FS, F_ARRAYS_GS, F_FS_SPEC, F_FS, F_COUNT };
static const char* const g_fields[F_COUNT] = {
    "vs", "tcs", "tes", "gs", "patch", "polygon", "depth_clamp", "provoking_last", "conservative", "topo_class", "vi",
    "ncolor", "color", "depth", "stencil", "blend", "blend mask", "logic_op", "samples", "sample_off",
    "alpha_to_coverage", "alpha_to_one", "coverage", "advanced blend", "sample_shading", "feedback", "arrays (vertex)",
    "arrays (fragment)", "arrays (geometry)", "fs_spec", "fs"};
static void mutate(PipeKey* k, int f) {
    switch (f) {
    case F_VS: k->vs = (VkShaderModule)(uintptr_t)0x1010; break;
    case F_TCS: k->tcs = (VkShaderModule)(uintptr_t)0x1020; break;
    case F_TES: k->tes = (VkShaderModule)(uintptr_t)0x1030; break;
    case F_GS: k->gs = (VkShaderModule)(uintptr_t)0x1040; break;
    case F_PATCH: k->patch = 4; break;
    case F_POLYGON: k->polygon = VK_POLYGON_MODE_LINE; break;
    case F_CLAMP: k->depth_clamp = 1; break;
    case F_PROVOKING: k->provoking_last = 1; break;
    case F_CONSERVATIVE: k->conservative = 3; break;
    case F_TOPO: k->topo_class = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case F_VI: k->vi.attr[0].format = VK_FORMAT_R8G8B8A8_UNORM; break;
    case F_NCOLOR: k->ncolor = 2; break;
    case F_COLOR: k->color[0] = VK_FORMAT_R16G16B16A16_SFLOAT; break;
    case F_DEPTH: k->depth = VK_FORMAT_D32_SFLOAT; break;
    case F_STENCIL: k->stencil = VK_FORMAT_D24_UNORM_S8_UINT; break;
    case F_BLEND: k->blend[0].en = 1; break;
    case F_MASK: k->blend[0].mask = 7; break;
    case F_LOGIC: k->logic_op = 1 + VK_LOGIC_OP_XOR; break;
    case F_SAMPLES: k->samples = 4; break;
    case F_SAMPLE_OFF: k->sample_off = 2; break;
    case F_A2C: k->alpha_to_coverage = 1; break;
    case F_A2O: k->alpha_to_one = 1; break;
    case F_COVERAGE: k->coverage.raster_samples = 8; break;
    case F_ADVANCED: k->advanced.operation = VK_BLEND_OP_ZERO_EXT; break;
    case F_SHADING: k->sample_shading = 1; break;
    case F_FEEDBACK: k->feedback = 1; break;
    case F_ARRAYS_VS: k->arrays |= 1u; break;
    case F_ARRAYS_FS: k->arrays |= 2u; break;
    case F_ARRAYS_GS: k->arrays |= 16u; break;
    case F_FS_SPEC: k->fs_spec = 3; break;
    default: k->fs = (VkShaderModule)(uintptr_t)0x2020; break;
    }
}
/* the parts (VK_PART_*) whose keys differ between a and b */
static u32 parts_differ(const PipeKey* a, const PipeKey* b, const PipeSplitDevice* d) {
    PipeViKey vi[2];
    PipePrKey pr[2];
    PipeFsKey fs[2];
    PipeFoKey fo[2];
    vk_pipe_split(a, a->fs ? a->fs : (VkShaderModule)(uintptr_t)0x9090, d, &vi[0], &pr[0], &fs[0], &fo[0]);
    vk_pipe_split(b, b->fs ? b->fs : (VkShaderModule)(uintptr_t)0x9090, d, &vi[1], &pr[1], &fs[1], &fo[1]);
    return (memcmp(&vi[0], &vi[1], sizeof *vi) ? VK_PART_VI : 0) | (memcmp(&pr[0], &pr[1], sizeof *pr) ? VK_PART_PR : 0) |
           (memcmp(&fs[0], &fs[1], sizeof *fs) ? VK_PART_FS : 0) | (memcmp(&fo[0], &fo[1], sizeof *fo) ? VK_PART_FO : 0);
}
static void check_split(void) {
    for (int dynamic = 0; dynamic < 2; dynamic++)
        for (int shading = 0; shading < 2; shading++) {
            const PipeSplitDevice d = {.dyn_pr = dynamic ? 63u : 0u, .feedback_dynamic = dynamic};
            PipeKey base;
            memset(&base, 0, sizeof base);
            base.vs = (VkShaderModule)(uintptr_t)0x1000;
            base.fs = (VkShaderModule)(uintptr_t)0x2000;
            base.topo_class = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            base.ncolor = 1;
            base.color[0] = VK_FORMAT_R8G8B8A8_UNORM;
            base.blend[0].mask = 15;
            base.samples = 1;
            base.sample_shading = (u32)shading;
            for (int f = 0; f < F_COUNT; f++) {
                if (shading && f == F_SHADING) continue; /* set already */
                PipeKey k = base;
                mutate(&k, f);
                u32 want;
                switch (f) {
                case F_VS: case F_TCS: case F_TES: case F_GS: case F_ARRAYS_VS: case F_ARRAYS_GS: want = VK_PART_PR; break;
                case F_PATCH: case F_POLYGON: case F_CLAMP: case F_PROVOKING: case F_CONSERVATIVE:
                    want = dynamic ? 0 : VK_PART_PR;
                    break;
                case F_TOPO: case F_VI: want = VK_PART_VI; break;
                case F_SAMPLES: case F_SAMPLE_OFF: case F_A2C: case F_A2O: case F_COVERAGE:
                    want = shading ? VK_PART_FS | VK_PART_FO : VK_PART_FO;
                    break;
                case F_SHADING: want = VK_PART_FS | VK_PART_FO; break;
                case F_FEEDBACK: want = dynamic ? 0 : VK_PART_FS | VK_PART_FO; break;
                case F_ARRAYS_FS: case F_FS_SPEC: case F_FS: want = VK_PART_FS; break;
                default: want = VK_PART_FO; break;
                }
                char what[160];
                snprintf(what, sizeof what, "%s changes the parts that hold it (dynamic %d, sample shading %d)",
                         g_fields[f], dynamic, shading);
                CHECK(parts_differ(&base, &k, &d) == want, what);
            }
            /* each part made from its own key alone (vk_pipe_part_src) splits back into that key */
            for (int f = 0; f < F_COUNT; f++) {
                PipeKey k = base, src;
                mutate(&k, f);
                PipeViKey vi[2];
                PipePrKey pr[2];
                PipeFsKey fs[2];
                PipeFoKey fo[2];
                VkShaderModule m = k.fs ? k.fs : (VkShaderModule)(uintptr_t)0x9090;
                vk_pipe_split(&k, m, &d, &vi[0], &pr[0], &fs[0], &fo[0]);
                vk_pipe_part_src(VK_PART_VI, &vi[0], &src);
                vk_pipe_split(&src, m, &d, &vi[1], &pr[1], &fs[1], &fo[1]);
                CHECK(!memcmp(&vi[0], &vi[1], sizeof *vi), "the vertex input part's own key");
                vk_pipe_part_src(VK_PART_PR, &pr[0], &src);
                vk_pipe_split(&src, m, &d, &vi[1], &pr[1], &fs[1], &fo[1]);
                CHECK(!memcmp(&pr[0], &pr[1], sizeof *pr), "the pre-rasterization part's own key");
                vk_pipe_part_src(VK_PART_FS, &fs[0], &src);
                vk_pipe_split(&src, fs[0].fs, &d, &vi[1], &pr[1], &fs[1], &fo[1]);
                CHECK(!memcmp(&fs[0], &fs[1], sizeof *fs), "the fragment shader part's own key");
                vk_pipe_part_src(VK_PART_FO, &fo[0], &src);
                vk_pipe_split(&src, m, &d, &vi[1], &pr[1], &fs[1], &fo[1]);
                CHECK(!memcmp(&fo[0], &fo[1], sizeof *fo), "the fragment output part's own key");
            }
            /* without a fragment program the write masks are off, so a mask is no output part of its own then */
            PipeKey a = base, b = base;
            a.fs = b.fs = VK_NULL_HANDLE;
            b.blend[0].mask = 1;
            CHECK(parts_differ(&a, &b, &d) == 0, "a write mask without a fragment program is no new part");
        }
}

/* ---- the fragment module's specialization constants in the key (vk_pipe_fs_spec): every state of the constants a
   module declares gives the pipeline those values back, and a constant it does not declare stays out of the key */
static void check_fs_spec(void) {
    for (u32 declared = 0; declared < 1u << SH_SPEC_COUNT; declared++)
        for (u32 state = 0; state < 64; state++) {
            int alpha_test = state & 1, lower_left = state >> 1 & 1, sprite_origin = state >> 2 & 1;
            u32 alpha_func = state >> 3, values[SH_SPEC_COUNT];
            u32 spec = vk_pipe_fs_spec(declared, alpha_test, alpha_func, lower_left, sprite_origin);
            vk_pipe_fs_spec_values(spec, values);
            u32 want[SH_SPEC_COUNT] = {SH_SPEC_ALPHA_ALWAYS, 0, 0};
            if (declared >> SH_SPEC_ALPHA & 1) want[SH_SPEC_ALPHA] = alpha_test ? alpha_func : SH_SPEC_ALPHA_ALWAYS;
            if (declared >> SH_SPEC_LOWER_LEFT & 1) want[SH_SPEC_LOWER_LEFT] = (u32)lower_left;
            if (declared >> SH_SPEC_SPRITE_FLIP & 1) want[SH_SPEC_SPRITE_FLIP] = sprite_origin == lower_left;
            if (memcmp(values, want, sizeof want))
                fprintf(stderr, "constants %x, state %u: the pipeline gives %u %u %u, not %u %u %u\n", declared, state,
                        values[0], values[1], values[2], want[0], want[1], want[2]);
            CHECK(!memcmp(values, want, sizeof want), "a pipeline gives the declared constants the draw's state");
            CHECK(declared || !spec, "a module with no constant takes no pipeline of its own for the state");
        }
    CHECK(!vk_pipe_fs_spec(1u << SH_SPEC_ALPHA, 1, SH_SPEC_ALPHA_ALWAYS, 1, 0) &&
              !vk_pipe_fs_spec(1u << SH_SPEC_ALPHA, 0, 3, 0, 1),
          "the test off and the function always are one pipeline, whatever the origin, for a module with no other");
}

static void say(const char* line, int warn) {
    (void)warn;
    g_test.said++;
    snprintf(g_test.line, sizeof g_test.line, "%s", line);
}

/* The table's bans on a state as dynamic state hold for the choice: a stand-in device's driver and version, the
   members vk_gpl_bans turns off of a device that has them all, each said with its key, and the states the choice
   then leaves in the part's key. A member the device doesn't have is not said */
static void check_bans(void) {
    static const struct {
        const char* what;
        uint32_t driver, version, off; /* VkDriverId, driverVersion, the VK_DYN_PR_* a ban takes */
        const char* key;               /* in the last line said */
    } cases[] = {
        {"RADV 26.2.6", VK_DRIVER_ID_MESA_RADV, VK_MAKE_VERSION(26, 2, 6), 0, NULL},
        {"RADV 22.3.0", VK_DRIVER_ID_MESA_RADV, VK_MAKE_VERSION(22, 3, 0), VK_DYN_PR_DEPTH_CLAMP,
         "extendedDynamicState3DepthClampEnable"},
        {"PowerVR", VK_DRIVER_ID_IMAGINATION_PROPRIETARY, VK_MAKE_VERSION(1, 21, 0),
         VK_DYN_PR_POLYGON | VK_DYN_PR_DEPTH_CLAMP, "extendedDynamicState3DepthClampEnable"},
        {"Intel on Windows", VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS, 101u << 14 | 5000u, VK_DYN_PR_DEPTH_RANGE,
         "extendedDynamicState3DepthClipNegativeOneToOne"},
    };
    char err[512], line[512];
    CHECK(vk_bans_load(snx_vk_extension_bans_text, err, sizeof err), err);
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        VkBanDevice device = {cases[i].what, "", "", cases[i].driver, cases[i].version, 0, SNX_VK_BAN_LINUX};
        vk_bans_device(&device);
        VkGplDevice d = full();
        u32 dyn = 0, lines = 0;
        for (u32 bit = cases[i].off; bit; bit &= bit - 1) lines++;
        g_test.said = 0;
        g_test.line[0] = 0;
        vk_gpl_bans(&d, say);
        CHECK(g_test.said == lines, cases[i].what);
        CHECK(!cases[i].key || strstr(g_test.line, cases[i].key), g_test.line);
        CHECK(vk_gpl_choose(&d, &dyn, line, sizeof line) && dyn == (63u & ~cases[i].off), cases[i].what);
        CHECK(!(cases[i].off & VK_DYN_PR_DEPTH_CLAMP) || !strstr(line, "depth clamp"), line);
        /* the depth range a ban takes is the last stage's, and the log says the ban's line and what it is on */
        vk_depth_range_line(&d, dyn, line, sizeof line);
        CHECK(!(cases[i].off & VK_DYN_PR_DEPTH_RANGE) ||
                  (d.depth_range_ban && strstr(line, "extension_bans.txt:") &&
                   strstr(line, "holds for extendedDynamicState3DepthClipNegativeOneToOne on this driver")),
              line);
        CHECK((cases[i].off & VK_DYN_PR_DEPTH_RANGE) || strstr(line, "is the device's"), line);
        /* without the member there is nothing to turn off, and nothing to say */
        d = full();
        d.polygon = d.depth_clamp = d.depth_range = 0;
        g_test.said = 0;
        vk_gpl_bans(&d, say);
        CHECK(g_test.said == 0, "no line for a member the device has not");
    }
}

int main(void) {
    check_off();
    check_dynamic();
    check_depth_range();
    check_bans();
    check_states();
    check_split();
    check_fs_spec();
    printf("%s: %u checks, %u failed\n", g_test.failures ? "FAIL" : "PASS", g_test.checks, g_test.failures);
    return g_test.failures != 0;
}
