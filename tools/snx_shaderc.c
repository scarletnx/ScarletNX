/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Translate a captured shader corpus directly to SPIR-V and optionally validate the modules. */
#include "nx/gpu/shader/shader.h"
#include "nx/gpu/shader/ampere.h"
#include "nx/gpu/shader/shipped.h"
#include "nx/gpu/maxwell/kepler_compute_regs.h"
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"
#include <sys/stat.h>
#include <zstd.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <process.h>
#define SNX_PID _getpid()
#else
#include <unistd.h>
#define SNX_PID getpid()
#endif

static int write_file(const char* path, const void* data, size_t size) {
    FILE* file = fopen(path, "wb");
    int ok = file && fwrite(data, 1, size, file) == size;
    if (file && fclose(file)) ok = 0;
    if (!ok) fprintf(stderr, "cannot write %s\n", path);
    return ok;
}
static void shell_quote(char* out, size_t size, const char* path) {
#ifdef _WIN32
    snprintf(out, size, "\"%s\"", path);
#else
    size_t n = 0;
    out[n++] = '\'';
    for (; *path && n + 6 < size; path++) {
        if (*path == '\'') {
            memcpy(out + n, "'\\''", 4);
            n += 4;
        } else
            out[n++] = *path;
    }
    out[n++] = '\'';
    out[n] = 0;
#endif
}
/* writes the module to out/<base>.spv, or to a temporary file when only validating, and runs spirv-val on it: 1 when
   it couldn't be written or spirv-val refused it, 0 otherwise (and when neither out nor validate asks for anything) */
static int emit_spirv(u32* spv, size_t words, const char* out, int validate, const char* tmp, const char* base) {
    if (!out && !validate) return 0;
    char output[2200];
    if (out)
        snprintf(output, sizeof output, "%s/%s.spv", out, base);
    else
        snprintf(output, sizeof output, "%s/snx-shaderc-%d-%s.spv", tmp, (int)SNX_PID, base);
    /* the module's words as a .spv file has them, little-endian, so builds on any host diff the same */
    snx_le32_array(spv, words);
    int written = write_file(output, spv, words * sizeof *spv), bad = !written;
    if (written && validate) {
        char quoted[4500], command[4600];
        shell_quote(quoted, sizeof quoted, output);
        snprintf(command, sizeof command, "spirv-val --target-env vulkan1.3 %s 2>&1", quoted);
        FILE* proc = popen(command, "r");
        char message[1024] = "";
        size_t got = proc ? fread(message, 1, sizeof message - 1, proc) : 0;
        message[got] = 0;
        if ((proc ? pclose(proc) : -1) != 0) {
            bad = 1;
            printf("%s: spirv-val: %s\n", base, message);
        }
    }
    if (!out) remove(output);
    return bad;
}

/* ---- --ampere: the Switch 2's native programs, lowered (shader/ampere.c) and translated, with every refusal counted.
   Programs come from shader containers (BFSHA and BNSH, zstd-compressed .zs too: shipped.c finds them) and from the
   runtime's captures (Shader captures setting): captures/native/gfx_*.bin, the 128-byte header then the code, and
   captures/compute/cs_*.bin, the 256-byte QMD then the code. A program found twice (one game ships each container in
   two versions) counts once */
typedef struct {
    char reason[200];
    char example[200];
    u32 count, sites, last_program;
    int local;
} Refusal;
typedef struct {
    const char *out, *tmp, *file;
    const u8* data; /* the file's bytes, which the offsets in the examples count from */
    int validate, verbose, flush, diagnose;
    u64* seen;
    size_t nseen, seen_cap;
    Refusal* refusals;
    size_t nrefusals, refusals_cap;
    u32 programs, duplicates, paired, lowered, translated, refused, untranslated, failed, unchecked, unbounded;
    u32 mesh[2]; /* C797's MESH_SHADER_A/B, with --mesh: a native mesh program's counts and invocations */
    int has_mesh;
    u8 lowered_code[SHADER_AMPERE_LOWERED_MAX];
} Native;

static int native_seen(Native* n, u64 hash) {
    for (size_t i = 0; i < n->nseen; i++)
        if (n->seen[i] == hash) return 1;
    if (n->nseen == n->seen_cap) {
        size_t cap = n->seen_cap ? n->seen_cap * 2 : 256;
        u64* grown = realloc(n->seen, cap * sizeof *grown);
        if (!grown) return 0;
        n->seen = grown;
        n->seen_cap = cap;
    }
    n->seen[n->nseen++] = hash;
    return 0;
}
/* a refusal by its reason without the offset ("Ampere opcode 819 at +20: x" counts as "Ampere opcode 819: x") */
static void native_refusal(Native* n, const char* stage, const char* why, const char* where, int local) {
    char reason[200];
    const char* at = strstr(why, " at +");
    const char* colon = at ? strchr(at, ':') : NULL;
    if (at && colon)
        snprintf(reason, sizeof reason, "%s: %.*s%s", stage, (int)(at - why), why, colon);
    else
        snprintf(reason, sizeof reason, "%s: %s", stage, why);
    for (size_t i = 0; i < n->nrefusals; i++)
        if (n->refusals[i].local == local && !strcmp(n->refusals[i].reason, reason)) {
            Refusal* r = &n->refusals[i];
            r->sites++;
            if (r->last_program != n->programs) r->count++;
            r->last_program = n->programs;
            return;
        }
    if (n->nrefusals == n->refusals_cap) {
        size_t cap = n->refusals_cap ? n->refusals_cap * 2 : 64;
        Refusal* grown = realloc(n->refusals, cap * sizeof *grown);
        if (!grown) return;
        n->refusals = grown;
        n->refusals_cap = cap;
    }
    Refusal* r = &n->refusals[n->nrefusals++];
    snprintf(r->reason, sizeof r->reason, "%s", reason);
    snprintf(r->example, sizeof r->example, "%s", where);
    r->count = 1;
    r->sites = 1;
    r->last_program = n->programs;
    r->local = local;
}
static int native_by_count(const void* a, const void* b) {
    const Refusal *x = a, *y = b;
    return (x->count < y->count) - (x->count > y->count);
}
typedef struct {
    Native* survey;
    const char* stage;
    size_t offset;
} NativeDiagnostic;
static void native_diagnostic(void* ctx, u32 offset, const char* reason) {
    NativeDiagnostic* d = ctx;
    char where[200];
    snprintf(where, sizeof where, "%s+%zx", d->survey->file, d->offset + offset);
    native_refusal(d->survey, d->stage, reason, where, 1);
    if (d->survey->verbose) printf("%s %s: instruction-local: %s\n", where, d->stage, reason);
}
/* one program: its header (NULL for compute), its code and the bytes readable from there, and a compute program's
   launch description (QMD words as numbers, NULL for graphics), whose workgroup and shared memory sizes the
   translation takes as vk/compute.c's does */
static void native_program(Native* n, const u8* header, const u8* code, size_t avail, int stage, size_t offset,
                           const u32* qmd) {
    u32 bytes = (u32)(avail < SHADER_AMPERE_NATIVE_MAX ? avail : SHADER_AMPERE_NATIVE_MAX) & ~15u;
    /* --mesh: a native program of C797's mesh slots is translated as the task in the vertex slot (its SPH has no
       output map) or the mesh program in the tessellation slot (a tessellation init program with one) */
    int task = 0, mesh_stage = 0;
    if (n->has_mesh && header) {
        /* a mesh slot's program touches the stage buffer (ISBE); without an output map it is the task, with one the
           mesh program the vertex slot holds */
        u32 shader = snx_ld_le32(header) >> 10 & 0xf, map = 0;
        int isbe = 0;
        for (u32 i = 12; i < 18; ++i) map |= snx_ld_le32(header + i * 4);
        for (u32 at = 0; at + 16 <= bytes && !isbe; at += 16) {
            u32 op = snx_ld_le32(code + at) & 0xfff;
            isbe = op == 0x923 || op == 0x927;
        }
        if (isbe && shader == 3) {
            stage = ST_TES;
            mesh_stage = 1;
        } else if (isbe && shader == 1) {
            task = map == 0;
            stage = ST_VS;
            mesh_stage = 1;
        }
    }
    /* Include forward branch bodies and called leaves after an early EXIT in the program's identity. */
    u32 end = shader_ampere_read_extent(code, bytes);
    if (end > bytes) end = bytes;
    u64 hash = snx_fnv1a(header ? snx_fnv1a(SNX_FNV_BASIS, header, SHIPPED_NATIVE_HEADER) : SNX_FNV_BASIS, code, end);
    hash = snx_fnv_step(hash, (u64)(i64)stage);
    if (native_seen(n, hash)) {
        n->duplicates++;
        return;
    }
    n->programs++;
    char where[200], base[64];
    snprintf(where, sizeof where, "%s+%zx", n->file, offset);
    snprintf(base, sizeof base, "%016llx_%d", (unsigned long long)hash, stage);
    const char* name = shipped_stage_name(stage);
    ShaderInfo info = {0};
    memset(n->lowered_code, 0, sizeof n->lowered_code);
    u32 size = shader_ampere_lower(code, bytes, n->lowered_code, sizeof n->lowered_code, &info, NULL);
    if (n->diagnose) {
        /* Reuse the lowerer's main-body extent. Leaf linkage and whole-program checks remain fail-fast. */
        NativeDiagnostic diagnostic = {n, name, offset};
        u32 unchecked = shader_ampere_diagnose(code, bytes, native_diagnostic, &diagnostic);
        if (unchecked == ~0u) n->unbounded++;
        else n->unchecked += unchecked;
    }
    if (!size) {
        n->refused++;
        native_refusal(n, name, info.first_unsupported, where, 0);
        if (n->verbose) printf("%s %s: lowering: %s\n", where, name, info.first_unsupported);
        return;
    }
    n->lowered++;
    /* a VertexA runs merged with the VertexB a draw binds beside it, which a container doesn't say */
    if (stage == SHIPPED_VERTEX_A) {
        n->paired++;
        return;
    }
    /* graphics: the first 80 bytes of the native header stand where the Maxwell header was (vk/pipeline.c) */
    if (header) memcpy(n->lowered_code, header, 80);
    ShaderInfo si = {0};
    if (mesh_stage) {
        si.mesh[0] = n->mesh[0];
        si.mesh[1] = n->mesh[1];
        si.task = (u8)(stage == ST_VS);
        si.mesh_task = (u8)(stage == ST_TES);
    }
    if (stage == ST_GS) si.rt_int_mask = 3; /* a container doesn't name the draw's input primitive: triangles */
    if (qmd) {
        si.cs_local[0] = qmd_block_dim_x(qmd);
        si.cs_local[1] = qmd_block_dim_y(qmd);
        si.cs_local[2] = qmd_block_dim_z(qmd);
        si.shared_size = (int)qmd_shared_alloc(qmd);
    }
    size_t words = 0;
    u32* spv = shader_to_spirv(n->lowered_code, size, stage, &si, &words, n->flush, 1, 1,
                              SH_SUBGROUP_OPERATIONS | SH_SUBGROUP_32);
    if (!spv) {
        n->untranslated++;
        native_refusal(n, name, si.first_unsupported, where, 0);
        if (n->verbose) printf("%s %s: translation: %s\n", where, name, si.first_unsupported);
        return;
    }
    n->translated++;
    n->failed += (u32)emit_spirv(spv, words, n->out, n->validate, n->tmp, base);
    free(spv);
}
static int native_each(void* ctx, const u8* header, size_t avail, int stage) {
    Native* n = ctx;
    native_program(n, header, header + SHIPPED_NATIVE_HEADER, avail - SHIPPED_NATIVE_HEADER, stage,
                   (size_t)(header - n->data), NULL);
    return 0;
}
static int ends_with(const char* s, const char* end) {
    size_t a = strlen(s), b = strlen(end);
    return a >= b && !strcmp(s + a - b, end);
}
/* the file's bytes, decompressed when its name ends in .zs: malloc'd, or NULL */
static u8* native_read(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    u8* data = NULL;
    size_t n = 0, cap = 0, got;
    do {
        if (n == cap) {
            cap = cap ? cap * 2 : 1u << 20;
            u8* grown = realloc(data, cap);
            if (!grown) break;
            data = grown;
        }
        got = fread(data + n, 1, cap - n, f);
        n += got;
    } while (got);
    fclose(f);
    if (!ends_with(path, ".zs") || !data) {
        *size = n;
        return data;
    }
    ZSTD_DCtx* z = ZSTD_createDCtx();
    u8* plain = NULL;
    size_t made = 0, room = 0, ret = 1;
    ZSTD_inBuffer in = {data, n, 0};
    /* frame after frame until the input ends: complete when it ends where a frame does (ret 0) */
    while (z && !ZSTD_isError(ret)) {
        if (made == room) {
            room = room ? room * 2 : n * 4 + (1u << 20);
            u8* grown = realloc(plain, room);
            if (!grown) break;
            plain = grown;
        }
        ZSTD_outBuffer o = {plain, room, made};
        ret = ZSTD_decompressStream(z, &o, &in);
        made = o.pos;
        if (in.pos == in.size && (ret == 0 || o.pos < o.size)) break; /* the end, or input that ends mid-frame */
    }
    ZSTD_freeDCtx(z);
    free(data);
    if (!z || ZSTD_isError(ret) || ret != 0) {
        fprintf(stderr, "%s: not a whole zstd stream\n", path);
        free(plain);
        return NULL;
    }
    *size = made;
    return plain;
}
static void native_file(Native* n, const char* path) {
    const char* name = strrchr(path, '/');
    name = name ? name + 1 : path;
    int capture_cs = !strncmp(name, "cs_", 3) && ends_with(name, ".bin");
    int capture_gfx = !strncmp(name, "gfx_", 4) && ends_with(name, ".bin");
    if (!capture_cs && !capture_gfx && !strstr(name, ".bfsha") && !strstr(name, ".bnsh")) return;
    size_t size = 0;
    u8* d = native_read(path, &size);
    if (!d) {
        n->failed++;
        fprintf(stderr, "%s: unreadable\n", path);
        return;
    }
    n->file = path;
    n->data = d;
    if (capture_cs && size > 256) {
        u32 qmd[KEPLER_COMPUTE_QMD_WORDS];
        for (u32 i = 0; i < KEPLER_COMPUTE_QMD_WORDS; i++) qmd[i] = snx_ld_le32(d + i * 4);
        native_program(n, NULL, d + 256, size - 256, ST_CS, 256, qmd);
    } else if (capture_gfx && size > SHIPPED_NATIVE_HEADER && shipped_native_stage(d) != -1)
        native_program(n, d, d + SHIPPED_NATIVE_HEADER, size - SHIPPED_NATIVE_HEADER, shipped_native_stage(d), 0,
                       NULL);
    else if (!capture_cs && !capture_gfx)
        shipped_native_programs(d, size, native_each, n);
    free(d);
}
static void native_walk(Native* n, const char* path) {
    struct stat st;
    if (stat(path, &st)) {
        fprintf(stderr, "cannot open %s\n", path);
        n->failed++;
        return;
    }
    if (!S_ISDIR(st.st_mode)) {
        native_file(n, path);
        return;
    }
    DIR* dir = opendir(path);
    if (!dir) return;
    struct dirent* e;
    while ((e = readdir(dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[4096];
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        native_walk(n, child);
    }
    closedir(dir);
}
static int native_main(char** paths, int npaths, const char* out, int validate, int verbose, int flush,
                       const char* tmp, int diagnose, const u32 mesh[2], int has_mesh) {
    Native* n = calloc(1, sizeof *n);
    if (!n) return 2;
    *n = (Native){.out = out, .tmp = tmp, .validate = validate, .verbose = verbose, .flush = flush, .diagnose = diagnose};
    n->has_mesh = has_mesh;
    if (mesh) {
        n->mesh[0] = mesh[0];
        n->mesh[1] = mesh[1];
    }
    for (int i = 0; i < npaths; i++) native_walk(n, paths[i]);
    printf("%u native programs (%u duplicates not counted): %u lowered, %u translated, %u paired VertexA not translated "
           "alone; %u refused by the lowering, %u by the translator, %u failed\n",
           n->programs, n->duplicates, n->lowered, n->translated, n->paired, n->refused, n->untranslated, n->failed);
    if (n->nrefusals) {
        qsort(n->refusals, n->nrefusals, sizeof *n->refusals, native_by_count);
        printf("refusals, each program's first, the most common first:\n");
        for (size_t i = 0; i < n->nrefusals; i++)
            if (!n->refusals[i].local)
                printf("%6u  %s  (%s)\n", n->refusals[i].count, n->refusals[i].reason, n->refusals[i].example);
        if (diagnose) {
            printf("instruction-local refusals in bounded main bodies (programs, sites), not a translation proof:\n");
            for (size_t i = 0; i < n->nrefusals; i++)
                if (n->refusals[i].local)
                    printf("%6u %7u  %s  (%s)\n", n->refusals[i].count, n->refusals[i].sites,
                           n->refusals[i].reason, n->refusals[i].example);
        }
    }
    if (diagnose)
        printf("%u flow/link sites not checked locally, %u programs without a bounded main body; "
               "leaf bodies not scanned, full-program checks remain fail-fast\n", n->unchecked, n->unbounded);
    int rc = n->refused || n->untranslated || n->failed ? 1 : 0;
    free(n->seen);
    free(n->refusals);
    free(n);
    return rc;
}
static const char USAGE[] = "usage: %s CORPUS_DIR [--out DIR] [--validate] [--verbose] [--no-flush] [--dispatch-reconverge]\n"
                             "       %s --ampere PATH... [--diagnose] [--out DIR] [--validate] [--verbose] [--no-flush]\n"
                             "--ampere: the Switch 2's native programs in shader containers (.bfsha, .bnsh, .zs too) and\n"
                             "in the runtime's captures (gfx_*.bin, cs_*.bin), in the files and folders named; exits 1\n"
                             "when any program can't be lowered or translated, and lists why, the most common first\n"
                             "--mesh=A,B: with --ampere, translate C797's mesh programs with MESH_SHADER_A and _B (hex):\n"
                             "a tessellation init program with no output map as the task, one with a map as the mesh\n"
                             "--diagnose: also check instruction-local forms in bounded main bodies, without executing\n"
                             "partial programs; leaf bodies are not scanned, flow/link and SPIR-V checks stay fail-fast\n";
int main(int argc, char** argv) {
    const char* out = NULL;
    int validate = 0, verbose = 0, flush = 1, dispatch_reconverge = 0, ampere = 0, diagnose = 0, npaths = 0;
    u32 mesh_words[2] = {0, 0};
    int has_mesh_word = 0;
    char** paths = calloc((size_t)argc, sizeof *paths);
    if (!paths) return 2;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(argv[i], "--validate"))
            validate = 1;
        else if (!strcmp(argv[i], "--verbose"))
            verbose = 1;
        else if (!strcmp(argv[i], "--dispatch-reconverge"))
            dispatch_reconverge = 1;
        else if (!strcmp(argv[i], "--no-flush")) /* a device without DenormFlushToZero: FTZ done per instruction */
            flush = 0;
        else if (!strcmp(argv[i], "--ampere"))
            ampere = 1;
        else if (!strncmp(argv[i], "--mesh=", 7)) {
            char* end = NULL;
            mesh_words[0] = (u32)strtoul(argv[i] + 7, &end, 0);
            if (end && *end == ',') mesh_words[1] = (u32)strtoul(end + 1, NULL, 0);
            has_mesh_word = 1;
        }
        else if (!strcmp(argv[i], "--diagnose"))
            diagnose = 1;
        else if (!strcmp(argv[i], "--direct")) { /* accepted for older corpus commands */
        } else if (argv[i][0] != '-')
            paths[npaths++] = argv[i];
        else
            npaths = -1;
        if (npaths < 0) break;
    }
    if (npaths <= 0 || (!ampere && (npaths != 1 || diagnose))) {
        fprintf(stderr, USAGE, argv[0], argv[0]);
        free(paths);
        return 2;
    }
    /* Match a renderer with non-uniform sampled image array indexing. Otherwise indexed
       LDC handles look like plain bindless textures and corpus validation misses them. */
    g_shader_texture_arrays = 1;
    /* and VK_KHR_shader_maximal_reconvergence, which NVIDIA, AMD and Intel drivers have: programs whose lanes
       reconverge take structured control flow as on such a device (--dispatch-reconverge keeps their dispatch loop) */
    g_shader_maximal_reconvergence = !dispatch_reconverge;
    const char* tmp = getenv("TMPDIR");
    if (!tmp) tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMP");
#ifdef _WIN32
    if (!tmp) tmp = ".";
#else
    if (!tmp) tmp = "/tmp";
#endif
    if (ampere) {
        int rc = native_main(paths, npaths, out, validate, verbose, flush, tmp, diagnose,
                             has_mesh_word ? mesh_words : NULL, has_mesh_word);
        free(paths);
        return rc;
    }
    const char* dir = paths[0];
    free(paths);
    DIR* directory = opendir(dir);
    if (!directory) {
        fprintf(stderr, "cannot open %s\n", dir);
        return 2;
    }
    int total = 0, failed = 0, unsupported = 0;
    struct dirent* entry;
    while ((entry = readdir(directory))) {
        size_t len = strlen(entry->d_name);
        if (len < 6 || strcmp(entry->d_name + len - 5, ".snxs")) continue;
        total++;
        char path[2200], base[1024];
        snprintf(path, sizeof path, "%s/%s", dir, entry->d_name);
        snprintf(base, sizeof base, "%.*s", (int)(len - 5), entry->d_name);
        u32 max_bytes;
        int stage;
        ShaderInfo info;
        u8* code = shader_corpus_load(path, &max_bytes, &stage, &info);
        if (!code) {
            failed++;
            if (verbose) printf("%s: unreadable corpus entry\n", base);
            continue;
        }
        size_t words = 0;
        if (verbose) {
            u32 hlen = 0;
            printf("%s: hash %016llx stage %d\n", base, (unsigned long long)shader_hash(code, max_bytes, &hlen), stage);
        }
        u32* spv = shader_to_spirv(code, max_bytes, stage, &info, &words, flush, 1, 1,
                                  SH_SUBGROUP_OPERATIONS | SH_SUBGROUP_32);
        free(code);
        if (!spv) {
            unsupported++;
            if (verbose) printf("%s: unsupported: %s\n", base, info.first_unsupported);
            continue;
        }
        failed += emit_spirv(spv, words, out, validate, tmp, base);
        free(spv);
    }
    closedir(directory);
    printf("%d shaders: %d translated, %d unsupported, %d failed\n", total, total - unsupported - failed, unsupported,
           failed);
    return failed ? 1 : 0;
}
