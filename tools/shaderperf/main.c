/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx-shaderperf: measures how long a SPIR-V shader (compute, fragment or vertex) takes on the GPU, with every input
   given or made up from its reflection, and compares several shaders against the first. Made to be driven by an
   agent: every part of the environment is an option, and --json gives every number. --help lists the options */
#include "bench.h"
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* one string a section: C99 promises string literals of 4095 characters only */
static const char* const k_help[] = {
    "usage: snx-shaderperf [options] SHADER.spv[#entry] [SHADER2.spv ...]\n"
    "\n",
    "Times SPIR-V shaders on the GPU with GPU timestamps, many submissions, warmup and outlier rejection, and\n"
    "compares the second and later shaders against the first (same inputs, interleaved submissions).\n"
    "Compute, fragment and vertex shaders. Each descriptor, push constant and stage input the shader declares\n"
    "gets a default from the SPIR-V's reflection; options replace any of them. Run --reflect first to see them.\n"
    "\n",
    "Shaders:\n"
    "  --entry NAME              entry point of every shader (or SHADER.spv#NAME)\n"
    "  --vs FILE[#entry]         vertex shader for geometry or fragment shaders, or one --vs for each shader in\n"
    "                            order (default: a built-in one, a full-screen triangle for a fragment shader,\n"
    "                            giving float vec4 varyings at locations 0-7: r,g = screen uv, b = loc/8, a = 1)\n"
    "  --gs FILE[#entry]         geometry shader between them, or one each (default: none)\n"
    "  --fs FILE[#entry]         fragment shader for vertex or geometry shaders, or one each (default: rasterizer\n"
    "                            discard)\n"
    "  --reflect                 print each shader's reflected interface as JSON and exit (no GPU)\n"
    "  --print-vs                print a GLSL full-screen vertex shader that feeds the fragment shader's inputs\n"
    "                            (integer ones too) and exit: glslc -fshader-stage=vert it, then --vs\n"
    "\n",
    "Device:\n"
    "  --list-devices | --info | --list-formats | --list-counters\n"
    "  --device N|NAME           device by index or part of its name (default: first discrete GPU)\n"
    "  --loader PATH             Vulkan loader library (default: the system's)\n"
    "  --validate                Khronos validation layer on (slow: not for timings)\n"
    "  --robust                  robustBufferAccess and robustImageAccess on (off by default)\n"
    "  --driver-cache            keep the driver's shader disk cache (off: honest compile times)\n"
    "  --compute-queue           a compute-only queue family for compute shaders\n"
    "  --ext NAME                enable a device extension (repeatable)\n"
    "\n",
    "Workload:\n"
    "  --size WxH[xD]            compute grid in invocations, or framebuffer (default 1920x1080x1)\n"
    "  --dispatch X,Y,Z          workgroups, in place of --size for compute\n"
    "  --samples N               MSAA samples of the render targets (default 1)\n"
    "  --rt LOC=FORMAT           render target format of an output location (default rgba8_unorm, r..rgba32 int)\n"
    "  --depth FORMAT            a depth attachment; --depth-test, --depth-write turn its test and writes on\n"
    "  --clear                   clear the attachments on each pass (default: don't care)\n"
    "  --blend none|alpha|add    blending on every render target (default none)\n"
    "  --vertices N --instances N  vertices and instances a draw (vertex shaders: 196608 x 1)\n"
    "  --topology points|lines|linestrip|triangles|strip|fan (default triangles)\n"
    "  --index none|seq|random   an index buffer (vertex cache behavior)\n"
    "  --vertex-fill PATTERN     vertex attribute data: noise, gradient, zero, solid:R,G,B,A (default noise)\n"
    "\n",
    "Resources (S.B = descriptor set.binding, or the variable's name):\n"
    "  --bind S.B=K=V,K=V...     what a binding gets. Keys:\n"
    "      images: format=NAME (--list-formats) size=WxH[xD] layers=N mips=N|full view=1d|2d|3d|cube|1darray|\n"
    "              2darray|cubearray tiling=optimal|linear layout=readonly|general samples=N\n"
    "              raw=FILE (bytes as the GPU reads them: level 0 of every layer, then level 1... Block formats\n"
    "              (BCn, ETC2, ASTC) come only this way) | file=IMG.ppm/pgm/pfm (transcoded to plain formats,\n"
    "              mips made by box filter) | pattern=noise|gradient|checker|zero|solid:R,G,B,A (default noise;\n"
    "              random blocks for block formats); scale=X bias=Y applied to file and pattern values\n"
    "      samplers (and combined image samplers): filter=linear|nearest mip=linear|nearest\n"
    "              wrap=repeat|mirror|clamp|border|mirror_clamp aniso=N lod_bias=X min_lod=X max_lod=X\n"
    "              compare=less|lequal|... border=black|white|transparent unnormalized=1\n"
    "      buffers: size=BYTES[K|M|G] elements=N (runtime array elements, or texels) format=NAME (texel buffers)\n"
    "              fill=zero|noise|bytes|seq|ones value=TYPE:V,V,... file=FILE count=N (runtime descriptor arrays)\n"
    "  --push OFF|MEMBER=[TYPE:]V,V...  push constant bytes (TYPE f16 f32 f64 i8-i64 u8-u64 hex; default from the\n"
    "                            member's type)\n"
    "  --push-file FILE          the push constant block's first bytes\n"
    "  --spec ID|NAME=VALUE      a specialization constant (workgroup size ids too)\n"
    "  --buffer-size SIZE        storage buffers with a runtime array, and texel buffers (default 16M)\n"
    "  --runtime-count N         descriptors of a runtime descriptor array (default 1)\n"
    "\n",
    "Timing:\n"
    "  --runs N                  samples (submissions) of each shader (default 30)\n"
    "  --reps N|auto             dispatches or draws in one sample (default auto: --sample-ms of GPU time)\n"
    "  --sample-ms MS            GPU time a sample aims at with --reps auto (default 2)\n"
    "  --warmup N --warmup-ms MS submissions before sampling, at least both (default 10 and 300)\n"
    "  --target-rse X            sample until the median's 95% interval is within X of it (e.g. 0.005)\n"
    "  --min-runs N --max-runs N bounds with --target-rse (default 10 and 1000)\n"
    "  --min-time-ms MS --max-time-ms MS  bounds of the sampling time\n"
    "  --sleep-ms MS             idle between samples\n"
    "  --order interleave|shuffle|sequential  how the samples of several shaders alternate (default interleave)\n"
    "  --overlap                 no barriers between repetitions (throughput of overlapping work)\n"
    "  --outliers iqr|mad|none   outlier rejection (default iqr)\n"
    "  --seed N                  seed of every generated input and of the bootstrap (default 1)\n"
    "  --subgroup-size N --full-subgroups  subgroup size control for compute\n"
    "\n",
    "Queries:\n"
    "  --compile-runs N          pipeline creations to time (default 3)\n"
    "  --no-exec-stats           skip VK_KHR_pipeline_executable_properties (registers, instructions...)\n"
    "  --exec-ir DIR             write the driver's internal representations (disassembly) to DIR\n"
    "  --no-pipeline-stats       skip the pipeline statistics query (invocation counts)\n"
    "  --counters LIST|all       VK_KHR_performance_query counters (names or parts of names), --counter-runs N\n"
    "  --no-calibrate            skip the submit-to-start latency (VK_KHR/EXT_calibrated_timestamps)\n"
    "  --no-smi                  skip nvidia-smi's clocks and temperature before and after\n"
    "  --check                   run each shader once on fresh inputs and hash what it writes (compare outputs)\n"
    "  --dump TARGET=FILE        write a binding (S.B), rtN or depth after --check's run: .ppm/.pfm/.pgm decode,\n"
    "                            other names are raw bytes (FILE-N.ext for the Nth shader of several)\n"
    "\n",
    "Output:\n"
    "  --json[=FILE]             all results as JSON (to stdout with no FILE, the text report goes to stderr)\n"
    "  --csv FILE                every sample: shader,index,ns_per_rep,ns_total,wall_ns,latency_ns\n"
    "  --samples-out             the samples in the JSON too\n"
    "  --quiet                   no text report\n",
    NULL,
};

static SpBench g_bench;

static void print_help(FILE* f) {
    for (size_t i = 0; k_help[i]; i++)
        fputs(k_help[i], f);
}

void sp_warn(SpBench* b, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (b->warning_count == b->warning_cap) {
        b->warning_cap = b->warning_cap ? b->warning_cap * 2 : 16;
        b->warnings = realloc(b->warnings, b->warning_cap * sizeof(char*));
    }
    char* s = malloc(strlen(buf) + 1);
    if (!b->warnings || !s)
        exit(2);
    strcpy(s, buf);
    b->warnings[b->warning_count++] = s;
}

void sp_note_resource(SpBench* b, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (b->resource_count == b->resource_cap) {
        b->resource_cap = b->resource_cap ? b->resource_cap * 2 : 16;
        b->resources = realloc(b->resources, b->resource_cap * sizeof(char*));
    }
    char* s = malloc(strlen(buf) + 1);
    if (!b->resources || !s)
        exit(2);
    strcpy(s, buf);
    b->resources[b->resource_count++] = s;
}

bool sp_parse_size(const char* s, uint64_t* out) {
    char* end;
    double v = strtod(s, &end);
    if (end == s || v < 0)
        return false;
    switch (toupper((unsigned char)*end)) {
    case 'K': v *= 1024; end++; break;
    case 'M': v *= 1024 * 1024; end++; break;
    case 'G': v *= 1024.0 * 1024 * 1024; end++; break;
    default: break;
    }
    if (toupper((unsigned char)*end) == 'B')
        end++;
    if (*end)
        return false;
    *out = (uint64_t)v;
    return true;
}

/* ---- the command line ---- */

static bool split_arg(const char* s, SpArg* out) {
    const char* eq = strchr(s, '=');
    if (!eq || eq == s)
        return false;
    size_t n = (size_t)(eq - s);
    char* t = malloc(n + 1);
    if (!t)
        exit(2);
    memcpy(t, s, n);
    t[n] = 0;
    out->target = t;
    out->value = eq + 1;
    return true;
}

static int usage_error(const char* fmt, const char* arg) {
    fprintf(stderr, "snx-shaderperf: ");
    fprintf(stderr, fmt, arg);
    fprintf(stderr, " (--help)\n");
    return 2;
}

typedef enum { MODE_RUN, MODE_REFLECT, MODE_PRINT_VS, MODE_DEVICES, MODE_INFO, MODE_FORMATS, MODE_COUNTERS } Mode;

static int parse_args(int argc, char** argv, SpConfig* c, Mode* mode) {
    *mode = MODE_RUN;
    c->width = 1920;
    c->height = 1080;
    c->depth = 1;
    c->samples = 1;
    c->topology = "triangles";
    c->instances = 1;
    c->default_buffer = 16u << 20;
    c->runtime_count = 1;
    c->runs = 30;
    c->min_runs = 10;
    c->max_runs = 1000;
    c->warmup = 10;
    c->warmup_ms = 300;
    c->reps_auto = true;
    c->reps = 1;
    c->sample_ms = 2;
    c->outliers = SP_OUTLIERS_IQR;
    c->seed = 1;
    c->pipeline_stats = c->exec_stats = c->smi = c->calibrate = true;
    c->counter_runs = 5;
    c->compile_runs = 3;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
#define FLAG(name) (strcmp(a, name) == 0)
#define VALUE(name) (strcmp(a, name) == 0 && i + 1 < argc ? (v = argv[++i], 1) : 0)
        const char* v = NULL;
        SpArg pa;
        if (FLAG("--help") || FLAG("-h")) {
            print_help(stdout);
            exit(0);
        } else if (FLAG("--reflect")) {
            *mode = MODE_REFLECT;
        } else if (FLAG("--print-vs")) {
            *mode = MODE_PRINT_VS;
        } else if (FLAG("--list-devices")) {
            *mode = MODE_DEVICES;
        } else if (FLAG("--info")) {
            *mode = MODE_INFO;
        } else if (FLAG("--list-formats")) {
            *mode = MODE_FORMATS;
        } else if (FLAG("--list-counters")) {
            *mode = MODE_COUNTERS;
        } else if (VALUE("--entry")) {
            c->entry = v;
        } else if (VALUE("--vs")) {
            if (c->vs_count >= SP_MAX_SHADERS)
                return usage_error("too many --vs: %s", v);
            c->vs[c->vs_count++] = v;
        } else if (VALUE("--gs")) {
            if (c->gs_count >= SP_MAX_SHADERS)
                return usage_error("too many --gs: %s", v);
            c->gs[c->gs_count++] = v;
        } else if (VALUE("--fs")) {
            if (c->fs_count >= SP_MAX_SHADERS)
                return usage_error("too many --fs: %s", v);
            c->fs[c->fs_count++] = v;
        } else if (VALUE("--device")) {
            c->device = v;
        } else if (VALUE("--loader")) {
            c->loader = v;
        } else if (FLAG("--validate")) {
            c->validate = true;
        } else if (FLAG("--robust")) {
            c->robust = true;
        } else if (FLAG("--driver-cache")) {
            c->driver_cache = true;
        } else if (FLAG("--compute-queue")) {
            c->compute_queue = true;
        } else if (VALUE("--ext")) {
            if (c->extension_count < 32)
                c->extensions[c->extension_count++] = v;
        } else if (VALUE("--size")) {
            unsigned w = 0, h = 1, d = 1;
            int n = sscanf(v, "%ux%ux%u", &w, &h, &d);
            if (n < 1 || !w || !h || !d)
                return usage_error("--size %s: WxH or WxHxD", v);
            c->width = w;
            c->height = n >= 2 ? h : 1;
            c->depth = n >= 3 ? d : 1;
        } else if (VALUE("--dispatch")) {
            unsigned x = 0, y = 1, z = 1;
            if (sscanf(v, "%u,%u,%u", &x, &y, &z) < 1 || !x || !y || !z)
                return usage_error("--dispatch %s: X[,Y[,Z]]", v);
            c->dispatch[0] = x;
            c->dispatch[1] = y;
            c->dispatch[2] = z;
            c->have_dispatch = true;
        } else if (VALUE("--samples")) {
            c->samples = (uint32_t)atoi(v);
            if (!c->samples || (c->samples & (c->samples - 1)) || c->samples > 64)
                return usage_error("--samples %s: 1, 2, 4, 8...", v);
        } else if (VALUE("--rt")) {
            if (!split_arg(v, &pa) || atoi(pa.target) < 0 || atoi(pa.target) >= SP_MAX_RTS)
                return usage_error("--rt %s: LOC=FORMAT, LOC 0 to 7", v);
            c->rt_formats[atoi(pa.target)] = pa.value;
        } else if (VALUE("--depth")) {
            c->depth_format = v;
        } else if (FLAG("--depth-test")) {
            c->depth_test = true;
        } else if (FLAG("--depth-write")) {
            c->depth_write = true;
        } else if (FLAG("--clear")) {
            c->clear = true;
        } else if (VALUE("--blend")) {
            c->blend = v;
        } else if (VALUE("--vertices")) {
            c->vertices = (uint32_t)strtoul(v, NULL, 0);
        } else if (VALUE("--instances")) {
            c->instances = (uint32_t)strtoul(v, NULL, 0);
            if (!c->instances)
                return usage_error("--instances %s", v);
        } else if (VALUE("--topology")) {
            c->topology = v;
        } else if (VALUE("--index")) {
            c->index = v;
        } else if (VALUE("--vertex-fill")) {
            c->vertex_fill = v;
        } else if (VALUE("--bind")) {
            if (c->bind_count >= SP_MAX_ARGS || !split_arg(v, &c->binds[c->bind_count]))
                return usage_error("--bind %s: S.B=KEY=VALUE,...", v);
            c->bind_count++;
        } else if (VALUE("--push")) {
            if (c->push_count >= SP_MAX_ARGS || !split_arg(v, &c->pushes[c->push_count]))
                return usage_error("--push %s: OFFSET=VALUES or MEMBER=VALUES", v);
            c->push_count++;
        } else if (VALUE("--push-file")) {
            c->push_file = v;
        } else if (VALUE("--spec")) {
            if (c->spec_count >= SP_MAX_ARGS || !split_arg(v, &c->specs[c->spec_count]))
                return usage_error("--spec %s: ID=VALUE or NAME=VALUE", v);
            c->spec_count++;
        } else if (VALUE("--dump")) {
            if (c->dump_count >= SP_MAX_ARGS || !split_arg(v, &c->dumps[c->dump_count]))
                return usage_error("--dump %s: S.B=FILE, rtN=FILE or depth=FILE", v);
            c->dump_count++;
        } else if (VALUE("--buffer-size")) {
            if (!sp_parse_size(v, &c->default_buffer) || !c->default_buffer)
                return usage_error("--buffer-size %s", v);
        } else if (VALUE("--runtime-count")) {
            c->runtime_count = (uint32_t)atoi(v);
        } else if (VALUE("--runs")) {
            c->runs = (uint32_t)atoi(v);
            if (!c->runs)
                return usage_error("--runs %s", v);
        } else if (VALUE("--min-runs")) {
            c->min_runs = (uint32_t)atoi(v);
        } else if (VALUE("--max-runs")) {
            c->max_runs = (uint32_t)atoi(v);
        } else if (VALUE("--warmup")) {
            c->warmup = (uint32_t)atoi(v);
        } else if (VALUE("--warmup-ms")) {
            c->warmup_ms = atof(v);
        } else if (VALUE("--reps")) {
            c->reps_auto = strcmp(v, "auto") == 0;
            c->reps = (uint32_t)atoi(v);
            if (!c->reps_auto && !c->reps)
                return usage_error("--reps %s: a count or auto", v);
        } else if (VALUE("--sample-ms")) {
            c->sample_ms = atof(v);
        } else if (VALUE("--target-rse")) {
            c->target_rse = atof(v);
        } else if (VALUE("--min-time-ms")) {
            c->min_time_ms = atof(v);
        } else if (VALUE("--max-time-ms")) {
            c->max_time_ms = atof(v);
        } else if (VALUE("--sleep-ms")) {
            c->sleep_ms = atof(v);
        } else if (VALUE("--order")) {
            if (strcmp(v, "interleave") == 0)
                c->order = SP_ORDER_INTERLEAVE;
            else if (strcmp(v, "shuffle") == 0)
                c->order = SP_ORDER_SHUFFLE;
            else if (strcmp(v, "sequential") == 0)
                c->order = SP_ORDER_SEQUENTIAL;
            else
                return usage_error("--order %s: interleave, shuffle or sequential", v);
        } else if (FLAG("--overlap")) {
            c->overlap = true;
        } else if (VALUE("--outliers")) {
            if (strcmp(v, "none") == 0)
                c->outliers = SP_OUTLIERS_NONE;
            else if (strcmp(v, "iqr") == 0)
                c->outliers = SP_OUTLIERS_IQR;
            else if (strcmp(v, "mad") == 0)
                c->outliers = SP_OUTLIERS_MAD;
            else
                return usage_error("--outliers %s: iqr, mad or none", v);
        } else if (VALUE("--seed")) {
            c->seed = strtoull(v, NULL, 0);
        } else if (VALUE("--subgroup-size")) {
            c->subgroup_size = (uint32_t)atoi(v);
        } else if (FLAG("--full-subgroups")) {
            c->full_subgroups = true;
        } else if (VALUE("--compile-runs")) {
            c->compile_runs = (uint32_t)atoi(v);
        } else if (FLAG("--no-exec-stats")) {
            c->exec_stats = false;
        } else if (VALUE("--exec-ir")) {
            c->exec_ir_dir = v;
        } else if (FLAG("--no-pipeline-stats")) {
            c->pipeline_stats = false;
        } else if (VALUE("--counters")) {
            c->counters = v;
        } else if (VALUE("--counter-runs")) {
            c->counter_runs = (uint32_t)atoi(v);
            if (!c->counter_runs)
                c->counter_runs = 1;
        } else if (FLAG("--no-calibrate")) {
            c->calibrate = false;
        } else if (FLAG("--no-smi")) {
            c->smi = false;
        } else if (FLAG("--check")) {
            c->check = true;
        } else if (FLAG("--json")) {
            c->json_path = "-";
        } else if (strncmp(a, "--json=", 7) == 0) {
            c->json_path = a + 7;
        } else if (VALUE("--csv")) {
            c->csv_path = v;
        } else if (FLAG("--samples-out")) {
            c->samples_out = true;
        } else if (FLAG("--quiet")) {
            c->quiet = true;
        } else if (a[0] == '-' && a[1]) {
            return usage_error("unknown option or missing value: %s", a);
        } else {
            if (c->shader_count >= SP_MAX_SHADERS)
                return usage_error("at most 16 shaders: %s", a);
            c->shaders[c->shader_count++] = a;
        }
#undef FLAG
#undef VALUE
    }
    if (c->max_runs < c->runs && c->target_rse <= 0)
        c->max_runs = c->runs;
    if (c->min_runs < 2)
        c->min_runs = 2;
    return -1;
}

/* ---- JSON ---- */

static void js_str(FILE* f, const char* s) {
    fputc('"', f);
    for (; s && *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\')
            fprintf(f, "\\%c", ch);
        else if (ch == '\n')
            fputs("\\n", f);
        else if (ch == '\t')
            fputs("\\t", f);
        else if (ch < 0x20)
            fprintf(f, "\\u%04x", ch);
        else
            fputc(ch, f);
    }
    fputc('"', f);
}

static void js_num(FILE* f, double v) {
    if (isfinite(v))
        fprintf(f, "%.6g", v);
    else
        fputs("null", f);
}

static void js_stats(FILE* f, const SpStats* s) {
    fprintf(f, "{\"n\": %zu, \"rejected\": %zu", s->n, s->rejected);
    const struct {
        const char* name;
        double v;
    } k[] = {{"median", s->median},   {"median_lo", s->median_lo}, {"median_hi", s->median_hi}, {"mean", s->mean},
             {"mean_lo", s->mean_lo}, {"mean_hi", s->mean_hi},     {"stddev", s->stddev},       {"sem", s->sem},
             {"cv", s->cv},           {"mad", s->mad},             {"min", s->min},             {"p01", s->p01},
             {"p05", s->p05},         {"p25", s->p25},             {"p75", s->p75},             {"p95", s->p95},
             {"p99", s->p99},         {"max", s->max}};
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        fprintf(f, ", \"%s\": ", k[i].name);
        js_num(f, k[i].v);
    }
    fputc('}', f);
}

static void js_varyings(FILE* f, const SrVarying* v, size_t n) {
    fputc('[', f);
    for (size_t i = 0; i < n; i++) {
        fprintf(f, "%s{\"name\": ", i ? ", " : "");
        js_str(f, v[i].name);
        if (v[i].builtin)
            fprintf(f, ", \"location\": null");
        else
            fprintf(f, ", \"location\": %u", v[i].location);
        fprintf(f, ", \"component\": %u, \"type\": \"%s%u\", \"vec\": %u, \"locations\": %u, "
                   "\"flat\": %s, \"builtin\": %s}",
                v[i].component, sr_scalar_name(v[i].scalar), v[i].width, v[i].vec, v[i].locations,
                v[i].flat ? "true" : "false", v[i].builtin ? "true" : "false");
    }
    fputc(']', f);
}

static void js_module(FILE* f, const char* path, const SrModule* m) {
    fprintf(f, "{\"path\": ");
    js_str(f, path);
    fprintf(f, ", \"spirv\": \"%u.%u\", \"instructions\": %u, \"id_bound\": %u, \"entry\": ", (m->version >> 16) & 0xff,
            (m->version >> 8) & 0xff, m->instructions, m->id_bound);
    js_str(f, m->entry.name);
    fprintf(f, ", \"stage\": \"%s\", \"entries\": [", sr_stage_name(m->entry.stage));
    for (size_t i = 0; i < m->entry_count; i++) {
        fprintf(f, "%s{\"name\": ", i ? ", " : "");
        js_str(f, m->entries[i].name);
        fprintf(f, ", \"stage\": \"%s\"}", sr_stage_name(m->entries[i].stage));
    }
    if (m->entry.stage == VK_SHADER_STAGE_GEOMETRY_BIT)
        fprintf(f, "],\n   \"geometry\": {\"invocations\": %u, \"input_vertices\": %u, \"max_vertices\": %u, "
                   "\"output\": \"%s\"}",
                m->gs_invocations, m->gs_input_vertices, m->gs_max_vertices,
                m->gs_output == 0 ? "points" : m->gs_output == 1 ? "line_strip" : "triangle_strip");
    fprintf(f, "%s\n   \"local_size\": [%u, %u, %u], \"local_size_spec_ids\": [",
            m->entry.stage == VK_SHADER_STAGE_GEOMETRY_BIT ? "," : "],", m->local[0], m->local[1], m->local[2]);
    for (int i = 0; i < 3; i++) {
        if (m->local_spec[i] == UINT32_MAX)
            fprintf(f, "%snull", i ? ", " : "");
        else
            fprintf(f, "%s%u", i ? ", " : "", m->local_spec[i]);
    }
    fprintf(f, "],\n   \"bindings\": [");
    for (size_t i = 0; i < m->binding_count; i++) {
        const SrBinding* b = &m->bindings[i];
        fprintf(f, "%s\n    {\"target\": \"%u.%u\", \"name\": ", i ? "," : "", b->set, b->binding);
        js_str(f, b->name);
        fprintf(f, ", \"type\": \"%s\", \"count\": %u, \"writable\": %s", sr_descriptor_name(b->type), b->count,
                b->writable ? "true" : "false");
        if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
            fprintf(f, ", \"block_size\": %u, \"runtime_stride\": %u", b->block_size, b->runtime_stride);
        else if (b->type != VK_DESCRIPTOR_TYPE_SAMPLER) {
            static const char* const dims[] = {"1d", "2d", "3d", "cube", "rect", "buffer", "subpass"};
            const SpFormat* fmt = sp_format_vk(sr_image_format_vk(b->image_format));
            fprintf(f, ", \"dim\": \"%s\", \"arrayed\": %s, \"multisampled\": %s, \"depth\": %s, \"sampled_type\": "
                       "\"%s\", \"format\": \"%s\"",
                    b->dim < 7 ? dims[b->dim] : "?", b->arrayed ? "true" : "false", b->ms ? "true" : "false",
                    b->depth ? "true" : "false", sr_scalar_name(b->sampled_scalar), fmt ? fmt->name : "unknown");
        }
        fputc('}', f);
    }
    fprintf(f, "],\n   \"push_constants\": {\"name\": ");
    js_str(f, m->push_name);
    fprintf(f, ", \"size\": %u, \"members\": [", m->push_size);
    for (size_t i = 0; i < m->push_count; i++) {
        const SrMember* p = &m->push[i];
        fprintf(f, "%s{\"name\": ", i ? ", " : "");
        js_str(f, p->name);
        fprintf(f, ", \"offset\": %u, \"size\": %u, \"type\": \"%s%u\", \"vec\": %u, \"cols\": %u, \"array\": %u, "
                   "\"stride\": %u}",
                p->offset, p->size, sr_scalar_name(p->scalar), p->width, p->vec, p->cols,
                p->array == UINT32_MAX ? 0 : p->array, p->stride);
    }
    fprintf(f, "]},\n   \"spec_constants\": [");
    for (size_t i = 0; i < m->spec_count; i++) {
        const SrSpec* s = &m->specs[i];
        fprintf(f, "%s{\"id\": %u, \"name\": ", i ? ", " : "", s->spec_id);
        js_str(f, s->name);
        fprintf(f, ", \"type\": \"%s%u\", \"default_bits\": %llu}", sr_scalar_name(s->scalar), s->width,
                (unsigned long long)s->value);
    }
    fprintf(f, "],\n   \"inputs\": ");
    js_varyings(f, m->inputs, m->input_count);
    fprintf(f, ",\n   \"outputs\": ");
    js_varyings(f, m->outputs, m->output_count);
    fprintf(f, ",\n   \"capabilities\": [");
    for (size_t i = 0; i < m->capability_count; i++) {
        fprintf(f, "%s", i ? ", " : "");
        js_str(f, m->capabilities[i]);
    }
    fprintf(f, "], \"extensions\": [");
    for (size_t i = 0; i < m->extension_count; i++) {
        fprintf(f, "%s", i ? ", " : "");
        js_str(f, m->extensions[i]);
    }
    fprintf(f, "]}");
}

/* reads and reflects "path[#entry]" (entry else); false with the reason printed */
static bool reflect_file(const char* spec, const char* entry, SrModule* m) {
    char path[1024], err[256];
    snprintf(path, sizeof path, "%s", spec);
    char* hash = strrchr(path, '#');
    if (hash) {
        *hash = 0;
        entry = hash + 1;
    }
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "snx-shaderperf: cannot read %s\n", path);
        return false;
    }
    size_t cap = 1 << 16, n = 0;
    uint8_t* buf = malloc(cap);
    while (buf) {
        n += fread(buf + n, 1, cap - n, f);
        if (n < cap)
            break;
        uint8_t* nb = realloc(buf, cap *= 2);
        if (!nb)
            free(buf);
        buf = nb;
    }
    fclose(f);
    uint32_t* words = buf ? malloc(n + 4) : NULL;
    if (words)
        memcpy(words, buf, n);
    free(buf);
    bool ok = words && sr_reflect(words, n, entry, m, err, sizeof err);
    if (!ok)
        fprintf(stderr, "snx-shaderperf: %s: %s\n", path, words ? err : "out of memory");
    free(words);
    return ok;
}

static int reflect_only(const SpConfig* c) {
    printf("[");
    int status = 0;
    for (size_t i = 0, shown = 0; i < c->shader_count; i++) {
        SrModule m;
        if (!reflect_file(c->shaders[i], c->entry, &m)) {
            status = 1;
            continue;
        }
        printf("%s\n  ", shown++ ? "," : "");
        js_module(stdout, c->shaders[i], &m);
        sr_free(&m);
    }
    printf("\n]\n");
    return status;
}

/* GLSL of a full-screen triangle that writes each of the fragment shader's inputs: floats (u, v, location / 8, 1),
   integers (u * 255, v * 255, location, 1), flat */
static int print_vs(const SpConfig* c) {
    SrModule m;
    if (!reflect_file(c->shaders[0], c->entry, &m))
        return 1;
    if (m.entry.stage != VK_SHADER_STAGE_FRAGMENT_BIT) {
        fprintf(stderr, "snx-shaderperf: --print-vs wants a fragment shader, %s is a %s one\n", c->shaders[0],
                sr_stage_name(m.entry.stage));
        sr_free(&m);
        return 1;
    }
    printf("#version 450\n// a full-screen triangle for %s (snx-shaderperf --print-vs); draw 3 vertices\n",
           c->shaders[0]);
    int status = 0;
    for (size_t i = 0; i < m.input_count; i++) {
        const SrVarying* v = &m.inputs[i];
        if (v->builtin)
            continue;
        const char* base = v->scalar == SR_UINT ? "uint" : v->scalar == SR_SINT ? "int" : "float";
        const char* vec = v->scalar == SR_UINT ? "uvec" : v->scalar == SR_SINT ? "ivec" : "vec";
        char type[16];
        if (v->vec > 1)
            snprintf(type, sizeof type, "%s%u", vec, v->vec);
        else
            snprintf(type, sizeof type, "%s", base);
        if (v->width != 32 || v->locations != 1 || v->scalar == SR_BOOL || v->scalar == SR_OTHER) {
            printf("// location %u (%s): %s%u x%u over %u locations is not written here: add it by hand\n",
                   v->location, v->name, sr_scalar_name(v->scalar), v->width, v->vec, v->locations);
            status = 1;
            continue;
        }
        printf("layout(location = %u, component = %u) %sout %s o%zu;\n", v->location, v->component,
               v->scalar == SR_FLOAT ? "" : "flat ", type, i);
    }
    printf("void main() {\n    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
           "    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);\n");
    static const char* const sw[] = {"", ".x", ".xy", ".xyz", ""};
    for (size_t i = 0; i < m.input_count; i++) {
        const SrVarying* v = &m.inputs[i];
        if (v->builtin || v->width != 32 || v->locations != 1 || v->scalar == SR_BOOL || v->scalar == SR_OTHER)
            continue;
        const char* sel = v->vec <= 4 ? sw[v->vec] : "";
        if (v->scalar == SR_FLOAT)
            printf("    o%zu = vec4(uv, %u.0 / 8.0, 1.0)%s;\n", i, v->location, sel);
        else
            printf("    o%zu = %svec4(uv * 255.0, %u, 1)%s;\n", i, v->scalar == SR_UINT ? "u" : "i", v->location,
                   sel);
    }
    printf("}\n");
    sr_free(&m);
    return status;
}

/* ---- results ---- */

static const char* verdict(const SpCompare* c) {
    bool sig = c->mwu_p < 0.01 && (c->ratio_hi < 1 || c->ratio_lo > 1);
    if (!sig)
        return "no significant difference";
    return c->ratio < 1 ? "faster" : "slower";
}

/* the time in a fitting unit */
static void fmt_time(char* out, size_t size, double ns) {
    if (!isfinite(ns))
        snprintf(out, size, "n/a");
    else if (ns >= 1e9)
        snprintf(out, size, "%.4g s", ns / 1e9);
    else if (ns >= 1e6)
        snprintf(out, size, "%.4g ms", ns / 1e6);
    else if (ns >= 1e3)
        snprintf(out, size, "%.4g us", ns / 1e3);
    else
        snprintf(out, size, "%.4g ns", ns);
}

static bool outputs_match(const SpShader* a, const SpShader* b) {
    if (a->hash_count != b->hash_count)
        return false;
    for (size_t i = 0; i < a->hash_count; i++)
        if (a->hashes[i].hash != b->hashes[i].hash)
            return false;
    return true;
}

static const char* stage_unit(VkShaderStageFlagBits stage, bool plural) {
    if (stage == VK_SHADER_STAGE_COMPUTE_BIT)
        return plural ? "invocations" : "invocation";
    if (stage == VK_SHADER_STAGE_FRAGMENT_BIT)
        return plural ? "pixels" : "pixel";
    return plural ? "vertices" : "vertex";
}

static void text_report(FILE* f, const SpBench* b, const SpCompare* cmp) {
    const SpConfig* c = &b->cfg;
    const SpDeviceInfo* d = &b->dev;
    fprintf(f, "device: %s, %s %s (Vulkan %s), timestamps %g ns x %u bits\n", d->name, d->driver, d->driver_info,
            d->api, d->timestamp_period, d->timestamp_bits);
    if (b->smi_before[0])
        fprintf(f, "nvidia-smi (sm, mem, max sm, temp, pstate, power, util): before %s | after %s\n", b->smi_before,
                b->smi_after);
    VkShaderStageFlagBits stage = b->shaders[0].mod.entry.stage;
    if (stage == VK_SHADER_STAGE_COMPUTE_BIT)
        fprintf(f, "workload: compute, %u x %u x %u workgroups of %u x %u x %u\n", b->shaders[0].dispatch[0],
                b->shaders[0].dispatch[1], b->shaders[0].dispatch[2], b->shaders[0].mod.local[0],
                b->shaders[0].mod.local[1], b->shaders[0].mod.local[2]);
    else if (stage == VK_SHADER_STAGE_FRAGMENT_BIT)
        fprintf(f, "workload: fragment, %u x %u framebuffer, %u sample%s, %s vertex shader%s\n", c->width, c->height,
                c->samples, c->samples > 1 ? "s" : "",
                c->vs_count > 1 ? "one --vs each" : c->vs_count ? c->vs[0] : "built-in full-screen",
                c->gs_count > 1 ? ", one --gs each" : c->gs_count ? ", a --gs" : "");
        else
        fprintf(f, "workload: vertex, %u vertices x %u instances, %s\n", c->vertices, c->instances,
                c->fs_count > 1 ? "one --fs each" : c->fs_count ? c->fs[0] : "rasterizer discard");
    fprintf(f, "sampling: %u repetitions a sample%s, %s barriers between them, outliers %s, %.0f ms in all\n", b->reps,
            c->reps_auto ? " (auto)" : "", c->overlap ? "no" : "with",
            c->outliers == SP_OUTLIERS_IQR ? "iqr" : c->outliers == SP_OUTLIERS_MAD ? "mad" : "kept", b->elapsed_ms);
    fprintf(f, "resources:\n");
    for (size_t i = 0; i < b->resource_count; i++)
        fprintf(f, "  %s\n", b->resources[i]);
    for (size_t v = 0; v < b->shader_count; v++) {
        const SpShader* sh = &b->shaders[v];
        const SpStats* s = &sh->time;
        char med[32], lo[32], hi[32], mn[32], mx[32], p95[32], sd[32];
        fmt_time(med, sizeof med, s->median);
        fmt_time(lo, sizeof lo, s->median_lo);
        fmt_time(hi, sizeof hi, s->median_hi);
        fmt_time(mn, sizeof mn, s->min);
        fmt_time(mx, sizeof mx, s->max);
        fmt_time(p95, sizeof p95, s->p95);
        fmt_time(sd, sizeof sd, s->stddev);
        fprintf(f, "\n[%zu] %s (%s, entry %s)\n", v, sh->label, sr_stage_name(sh->mod.entry.stage),
                sh->mod.entry.name);
        fprintf(f, "  time:       median %s (95%% CI %s .. %s), min %s, p95 %s, max %s, stddev %s, cv %.2f%%\n", med,
                lo, hi, mn, p95, mx, sd, s->cv * 100);
        fprintf(f, "              %zu samples, %zu outliers left out\n", s->n, s->rejected);
        if (sh->invocations && s->median > 0)
            fprintf(f, "  throughput: %.4g ns per %s, %.4g G%s/s (%llu a repetition)\n",
                    s->median / (double)sh->invocations, stage_unit(sh->mod.entry.stage, false),
                    (double)sh->invocations / s->median, stage_unit(sh->mod.entry.stage, true),
                    (unsigned long long)sh->invocations);
        char w[32], l[32];
        fmt_time(w, sizeof w, sh->wall_stats.median);
        fmt_time(l, sizeof l, sh->latency_stats.median);
        fprintf(f, "  submission: CPU submit to fence %s, submit to GPU start %s (medians)\n", w, l);
        fprintf(f, "  compile:    %.3f ms median of %zu (min %.3f)\n", sh->compile_stats.median, sh->compile_stats.n,
                sh->compile_stats.min);
        for (size_t e = 0; e < sh->exec_count; e++) {
            const SpExec* x = &sh->execs[e];
            fprintf(f, "  executable %s (subgroup %u):", x->name, x->subgroup_size);
            for (size_t k = 0; k < x->stat_count; k++)
                fprintf(f, "%s %s=%s", k ? "," : "", x->stats[k].name, x->stats[k].value);
            fputc('\n', f);
        }
        if (sh->pipeline_stat_count) {
            fprintf(f, "  pipeline statistics a repetition:");
            for (size_t k = 0; k < sh->pipeline_stat_count; k++)
                if (sh->pipeline_stats[k])
                    fprintf(f, " %s=%llu", sh->stat_names[k], (unsigned long long)sh->pipeline_stats[k]);
            fputc('\n', f);
        }
        for (size_t k = 0; k < sh->counter_count; k++)
            fprintf(f, "  counter %s: median %.6g %s (%zu runs of %u repetitions)\n", sh->counters[k].name,
                    sh->counters[k].stats.median, sh->counters[k].unit, sh->counters[k].stats.n, b->reps);
        for (size_t k = 0; k < sh->hash_count; k++)
            fprintf(f, "  output %s: %016llx (%zu bytes)\n", sh->hashes[k].target,
                    (unsigned long long)sh->hashes[k].hash, sh->hashes[k].bytes);
    }
    if (b->shader_count > 1) {
        fprintf(f, "\ncompared with [0] %s (ratio of medians, bootstrap 95%% CI):\n", b->shaders[0].label);
        for (size_t v = 1; v < b->shader_count; v++) {
            const SpCompare* x = &cmp[v];
            fprintf(f, "  [%zu] %s: %.4fx (%.4f .. %.4f), %s; Mann-Whitney p=%.3g, Welch p=%.3g, P(slower)=%.2f",
                    v, b->shaders[v].label, x->ratio, x->ratio_lo, x->ratio_hi, verdict(x), x->mwu_p, x->welch_p,
                    x->cles);
            if (b->shaders[v].hash_count)
                fprintf(f, ", outputs %s", outputs_match(&b->shaders[0], &b->shaders[v]) ? "match" : "DIFFER");
            fputc('\n', f);
        }
    }
    for (size_t i = 0; i < b->warning_count; i++)
        fprintf(f, "warning: %s\n", b->warnings[i]);
}

static void json_report(FILE* f, const SpBench* b, const SpCompare* cmp) {
    const SpConfig* c = &b->cfg;
    const SpDeviceInfo* d = &b->dev;
    fprintf(f, "{\n \"tool\": \"snx-shaderperf\", \"version\": 1,\n \"device\": {\"name\": ");
    js_str(f, d->name);
    fprintf(f, ", \"vendor\": %u, \"device_id\": %u, \"api\": \"%s\", \"driver\": ", d->vendor, d->device_id, d->api);
    js_str(f, d->driver);
    fprintf(f, ", \"driver_info\": ");
    js_str(f, d->driver_info);
    fprintf(f, ", \"driver_version\": \"%s\", \"timestamp_period_ns\": %g, \"timestamp_bits\": %u, "
               "\"subgroup_size\": %u, \"performance_query\": %s, \"pipeline_executable_properties\": %s, "
               "\"calibrated_timestamps\": %s, \"time_domains\": \"%s\"},\n",
            d->driver_version, d->timestamp_period, d->timestamp_bits, d->subgroup_size,
            d->perf_query ? "true" : "false", d->exec_props ? "true" : "false", d->calibrated ? "true" : "false",
            d->time_domains);
    fprintf(f, " \"nvidia_smi\": {\"fields\": \"clocks.sm,clocks.mem,clocks.max.sm,temperature.gpu,pstate,power.draw,"
               "utilization.gpu\", \"before\": ");
    js_str(f, b->smi_before);
    fprintf(f, ", \"after\": ");
    js_str(f, b->smi_after);
    fprintf(f, "},\n \"config\": {\"size\": [%u, %u, %u], \"samples\": %u, \"vertices\": %u, \"instances\": %u, "
               "\"reps\": %u, \"reps_auto\": %s, \"sample_ms\": %g, \"runs\": %u, \"warmup\": %u, \"warmup_ms\": %g, "
               "\"target_rse\": %g, \"overlap\": %s, \"order\": \"%s\", \"outliers\": \"%s\", \"seed\": %llu, "
               "\"robust\": %s, \"driver_cache\": %s, \"elapsed_ms\": %.1f},\n",
            c->width, c->height, c->depth, c->samples, c->vertices, c->instances, b->reps,
            c->reps_auto ? "true" : "false", c->sample_ms, c->runs, c->warmup, c->warmup_ms, c->target_rse,
            c->overlap ? "true" : "false",
            c->order == SP_ORDER_SHUFFLE ? "shuffle" : c->order == SP_ORDER_SEQUENTIAL ? "sequential" : "interleave",
            c->outliers == SP_OUTLIERS_IQR ? "iqr" : c->outliers == SP_OUTLIERS_MAD ? "mad" : "none",
            (unsigned long long)c->seed, c->robust ? "true" : "false", c->driver_cache ? "true" : "false",
            b->elapsed_ms);
    fprintf(f, " \"resources\": [");
    for (size_t i = 0; i < b->resource_count; i++) {
        fprintf(f, "%s\n  ", i ? "," : "");
        js_str(f, b->resources[i]);
    }
    fprintf(f, "],\n \"shaders\": [");
    for (size_t v = 0; v < b->shader_count; v++) {
        const SpShader* sh = &b->shaders[v];
        fprintf(f, "%s\n  {\"index\": %zu, \"label\": ", v ? "," : "", v);
        js_str(f, sh->label);
        fprintf(f, ", \"stage\": \"%s\", \"entry\": ", sr_stage_name(sh->mod.entry.stage));
        js_str(f, sh->mod.entry.name);
        fprintf(f, ", \"path\": ");
        js_str(f, sh->path);
        fprintf(f, ",\n   \"dispatch\": [%u, %u, %u], \"local_size\": [%u, %u, %u], \"invocations_per_rep\": %llu,\n",
                sh->dispatch[0], sh->dispatch[1], sh->dispatch[2], sh->mod.local[0], sh->mod.local[1],
                sh->mod.local[2], (unsigned long long)sh->invocations);
        fprintf(f, "   \"time_ns_per_rep\": ");
        js_stats(f, &sh->time);
        fprintf(f, ",\n   \"ns_per_invocation\": ");
        js_num(f, sh->invocations ? sh->time.median / (double)sh->invocations : NAN);
        fprintf(f, ", \"ginvocations_per_s\": ");
        js_num(f, sh->time.median > 0 ? (double)sh->invocations / sh->time.median : NAN);
        fprintf(f, ",\n   \"time_ns_per_sample\": ");
        js_stats(f, &sh->total);
        fprintf(f, ",\n   \"wall_ns_per_sample\": ");
        js_stats(f, &sh->wall_stats);
        fprintf(f, ",\n   \"submit_latency_ns\": ");
        js_stats(f, &sh->latency_stats);
        fprintf(f, ",\n   \"compile_ms\": ");
        js_stats(f, &sh->compile_stats);
        fprintf(f, ",\n   \"executables\": [");
        for (size_t e = 0; e < sh->exec_count; e++) {
            const SpExec* x = &sh->execs[e];
            fprintf(f, "%s{\"name\": ", e ? ", " : "");
            js_str(f, x->name);
            fprintf(f, ", \"description\": ");
            js_str(f, x->desc);
            fprintf(f, ", \"subgroup_size\": %u, \"statistics\": {", x->subgroup_size);
            for (size_t k = 0; k < x->stat_count; k++) {
                fprintf(f, "%s", k ? ", " : "");
                js_str(f, x->stats[k].name);
                fprintf(f, ": ");
                const char* val = x->stats[k].value;
                bool num = strcmp(val, "true") == 0 || strcmp(val, "false") == 0 || strspn(val, "-0123456789.e+") == strlen(val);
                if (num)
                    fputs(val, f);
                else
                    js_str(f, val);
            }
            fprintf(f, "}}");
        }
        fprintf(f, "],\n   \"pipeline_statistics_per_rep\": {");
        for (size_t k = 0; k < sh->pipeline_stat_count; k++)
            fprintf(f, "%s\"%s\": %llu", k ? ", " : "", sh->stat_names[k], (unsigned long long)sh->pipeline_stats[k]);
        fprintf(f, "},\n   \"counters\": [");
        for (size_t k = 0; k < sh->counter_count; k++) {
            fprintf(f, "%s{\"name\": ", k ? ", " : "");
            js_str(f, sh->counters[k].name);
            fprintf(f, ", \"category\": ");
            js_str(f, sh->counters[k].category);
            fprintf(f, ", \"unit\": \"%s\", \"per_sample\": ", sh->counters[k].unit);
            js_stats(f, &sh->counters[k].stats);
            fputc('}', f);
        }
        fprintf(f, "],\n   \"outputs\": {");
        for (size_t k = 0; k < sh->hash_count; k++)
            fprintf(f, "%s\"%s\": {\"fnv1a64\": \"%016llx\", \"bytes\": %zu}", k ? ", " : "", sh->hashes[k].target,
                    (unsigned long long)sh->hashes[k].hash, sh->hashes[k].bytes);
        fprintf(f, "}");
        if (c->samples_out) {
            fprintf(f, ",\n   \"samples_ns_per_rep\": [");
            for (size_t k = 0; k < sh->sample_count; k++) {
                fprintf(f, "%s", k ? ", " : "");
                js_num(f, sh->samples[k]);
            }
            fprintf(f, "]");
        }
        fprintf(f, "}");
    }
    fprintf(f, "],\n \"comparisons\": [");
    for (size_t v = 1; v < b->shader_count; v++) {
        const SpCompare* x = &cmp[v];
        fprintf(f, "%s\n  {\"shader\": %zu, \"baseline\": 0, \"ratio_of_medians\": ", v > 1 ? "," : "", v);
        js_num(f, x->ratio);
        fprintf(f, ", \"ratio_lo\": ");
        js_num(f, x->ratio_lo);
        fprintf(f, ", \"ratio_hi\": ");
        js_num(f, x->ratio_hi);
        fprintf(f, ", \"ratio_of_means\": ");
        js_num(f, x->mean_ratio);
        fprintf(f, ", \"mann_whitney_p\": ");
        js_num(f, x->mwu_p);
        fprintf(f, ", \"welch_p\": ");
        js_num(f, x->welch_p);
        fprintf(f, ", \"p_slower\": ");
        js_num(f, x->cles);
        fprintf(f, ", \"verdict\": \"%s\"", verdict(x));
        if (b->shaders[v].hash_count)
            fprintf(f, ", \"outputs_match\": %s", outputs_match(&b->shaders[0], &b->shaders[v]) ? "true" : "false");
        fputc('}', f);
    }
    fprintf(f, "],\n \"warnings\": [");
    for (size_t i = 0; i < b->warning_count; i++) {
        fprintf(f, "%s", i ? ", " : "");
        js_str(f, b->warnings[i]);
    }
    fprintf(f, "]\n}\n");
}

static bool write_csv(const SpBench* b, const char* path) {
    FILE* f = fopen(path, "w");
    if (!f)
        return false;
    fprintf(f, "shader,index,ns_per_rep,ns_total,wall_ns,latency_ns\n");
    for (size_t v = 0; v < b->shader_count; v++) {
        const SpShader* sh = &b->shaders[v];
        for (size_t k = 0; k < sh->sample_count; k++) {
            fprintf(f, "%zu,%zu,%.3f,%.3f,%.3f,", v, k, sh->samples[k], sh->totals[k], sh->wall[k]);
            if (isnan(sh->latency[k]))
                fprintf(f, "\n");
            else
                fprintf(f, "%.3f\n", sh->latency[k]);
        }
    }
    return fclose(f) == 0;
}

int main(int argc, char** argv) {
    SpBench* b = &g_bench;
    Mode mode;
    int status = parse_args(argc, argv, &b->cfg, &mode);
    if (status >= 0)
        return status;
    switch (mode) {
    case MODE_DEVICES: return sp_gpu_list_devices(&b->cfg);
    case MODE_INFO: return sp_gpu_info(b);
    case MODE_FORMATS: return sp_gpu_list_formats(b);
    case MODE_COUNTERS: return sp_gpu_list_counters(b);
    default: break;
    }
    if (!b->cfg.shader_count) {
        print_help(stderr);
        return 2;
    }
    if (mode == MODE_REFLECT)
        return reflect_only(&b->cfg);
    if (mode == MODE_PRINT_VS)
        return print_vs(&b->cfg);
    if (!sp_gpu_run(b)) {
        for (size_t i = 0; i < b->warning_count; i++)
            fprintf(stderr, "warning: %s\n", b->warnings[i]);
        return 1;
    }
    SpCompare cmp[SP_MAX_SHADERS];
    memset(cmp, 0, sizeof cmp);
    for (size_t v = 1; v < b->shader_count; v++) {
        /* the kept samples of each, as the summaries keep them */
        const SpShader *a = &b->shaders[0], *s = &b->shaders[v];
        double* xa = malloc((a->sample_count + 1) * sizeof *xa);
        double* xb = malloc((s->sample_count + 1) * sizeof *xb);
        if (!xa || !xb)
            return 2;
        memcpy(xa, a->samples, a->sample_count * sizeof *xa);
        memcpy(xb, s->samples, s->sample_count * sizeof *xb);
        size_t na = sp_stats_keep(xa, a->sample_count, b->cfg.outliers);
        size_t nb = sp_stats_keep(xb, s->sample_count, b->cfg.outliers);
        sp_compare(xa, na, xb, nb, 2000, b->cfg.seed, &cmp[v]);
        free(xa);
        free(xb);
    }
    bool json_stdout = b->cfg.json_path && strcmp(b->cfg.json_path, "-") == 0;
    if (!b->cfg.quiet)
        text_report(json_stdout ? stderr : stdout, b, cmp);
    if (b->cfg.json_path) {
        FILE* f = json_stdout ? stdout : fopen(b->cfg.json_path, "w");
        if (!f) {
            fprintf(stderr, "snx-shaderperf: cannot write %s\n", b->cfg.json_path);
            return 1;
        }
        json_report(f, b, cmp);
        if (!json_stdout && fclose(f) != 0)
            return 1;
    }
    if (b->cfg.csv_path && !write_csv(b, b->cfg.csv_path)) {
        fprintf(stderr, "snx-shaderperf: cannot write %s\n", b->cfg.csv_path);
        return 1;
    }
    return 0;
}
