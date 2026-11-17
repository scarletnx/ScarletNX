/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Translate a captured shader corpus directly to SPIR-V and optionally validate the modules. */
#include "nx/gpu/shader/shader.h"
#include "nx/gpu/shader/sm86.h"
#include "common/utilities/endian.h"
#include "common/utilities/spirv_opt.h"
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
int main(int argc, char** argv) {
    const char *dir = NULL, *out = NULL;
    int validate = 0, verbose = 0, flush = SH_FTZ_MODE, dispatch_reconverge = 0, opt = 0;
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
            flush = SH_FTZ_EMULATE;
        else if (!strcmp(argv[i], "--native-flush")) /* a device that flushes by itself (vk/ftz_probe.c): no flush */
            flush = SH_FTZ_NATIVE;
        else if (!strcmp(argv[i], "--opt")) /* the runtime's shared optimizer after the translator (vk_translate_spirv) */
            opt = 1;
        else if (!strcmp(argv[i], "--sm86")) /* an Ounce game's programs, which the SM86 frontend reads */
            g_shader_sm86 = 1;
        else if (!strcmp(argv[i], "--no-doubles")) /* a device without shaderFloat64, which Apple's are */
            g_spirv_translate.float64 = 0;
        else if (!strcmp(argv[i], "--direct")) { /* accepted for older corpus commands */
        } else if (argv[i][0] != '-' && !dir)
            dir = argv[i];
        else {
            fprintf(stderr, "usage: %s CORPUS_DIR [--out DIR] [--validate] [--verbose] [--no-flush | --native-flush] [--opt] [--sm86] [--no-doubles] [--dispatch-reconverge]\n", argv[0]);
            return 2;
        }
    }
    if (!dir) {
        fprintf(stderr, "usage: %s CORPUS_DIR [--out DIR] [--validate] [--verbose] [--no-flush | --native-flush] [--opt] [--sm86] [--no-doubles] [--dispatch-reconverge]\n", argv[0]);
        return 2;
    }
    /* Match a renderer with non-uniform sampled image array indexing. Otherwise indexed
       LDC handles look like plain bindless textures and corpus validation misses them. */
    g_spirv_translate.texture_arrays = 1;
    /* and VK_KHR_shader_maximal_reconvergence, which NVIDIA, AMD and Intel drivers have: programs whose lanes
       reconverge take structured control flow as on such a device (--dispatch-reconverge keeps their dispatch loop) */
    g_spirv_translate.maximal_reconvergence = !dispatch_reconverge;
    DIR* directory = opendir(dir);
    if (!directory) {
        fprintf(stderr, "cannot open %s\n", dir);
        return 2;
    }
    const char* tmp = getenv("TMPDIR");
    if (!tmp) tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMP");
#ifdef _WIN32
    if (!tmp) tmp = ".";
#else
    if (!tmp) tmp = "/tmp";
#endif
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
        /* An Ounce program: the corpus window starts at the program's section, so the program itself is the first
           one found in it. A graphics program's window holds its header first, as the runtime's (pipeline.c's
           sm86_translate) does; a compute program has none */
        u32* spv;
        if (g_shader_sm86) {
            Sm86Program found[1];
            const int graphics = stage != ST_CS;
            if (sm86_find_programs(code, max_bytes, found, 1) != 1 || !found[0].complete ||
                (graphics ? found[0].code == found[0].header : found[0].stage != ST_CS)) {
                free(code);
                unsupported++;
                if (verbose)
                    printf("%s: no complete SM86 %s program in the window\n", base,
                           graphics ? "graphics program with its header" : "compute");
                continue;
            }
            u32 sph[SM86_SPH_BYTES / 4];
            if (graphics)
                for (u32 w = 0; w < SM86_SPH_BYTES / 4; w++) sph[w] = snx_ld_le32(code + found[0].header + 4 * w);
            spv = sm86_to_spirv_ftz(code + found[0].code, found[0].size, stage, graphics ? sph : NULL, &info, &words,
                                    flush); /* size counts from code */
        } else
            spv = shader_to_spirv(code, max_bytes, stage, &info, &words, flush, 1, 1, 1);
        free(code);
        if (!spv) {
            unsupported++;
            if (verbose) printf("%s: unsupported: %s\n", base, info.first_unsupported);
            continue;
        }
        if (opt) {
            words = spv_opt_values(spv, words);
            words = spv_opt_dead_code(spv, words);
        }
        if (out || validate) {
            char output[2200];
            if (out)
                snprintf(output, sizeof output, "%s/%s.spv", out, base);
            else
                snprintf(output, sizeof output, "%s/snx-shaderc-%d-%s.spv", tmp, (int)SNX_PID, base);
            /* the module's words as a .spv file has them, little-endian, so builds on any host diff the same */
            snx_le32_array(spv, words);
            int written = write_file(output, spv, words * sizeof *spv);
            if (written && validate) {
                char quoted[4500], command[4600];
                shell_quote(quoted, sizeof quoted, output);
                snprintf(command, sizeof command, "spirv-val --target-env vulkan1.3 %s 2>&1", quoted);
                FILE* proc = popen(command, "r");
                char message[1024] = "";
                size_t got = proc ? fread(message, 1, sizeof message - 1, proc) : 0;
                message[got] = 0;
                int rc = proc ? pclose(proc) : -1;
                if (rc) {
                    failed++;
                    printf("%s: spirv-val: %s\n", base, message);
                }
            }
            if (!out) remove(output);
            if (!written) failed++;
        }
        free(spv);
    }
    closedir(directory);
    printf("%d shaders: %d translated, %d unsupported, %d failed\n", total, total - unsupported - failed, unsupported,
           failed);
    return failed ? 1 : 0;
}
