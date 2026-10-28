/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Translate a captured shader corpus directly to SPIR-V and optionally validate the modules. */
#include "nx/gpu/shader/shader.h"
#include "common/utilities/endian.h"
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
    int validate = 0, verbose = 0, flush = 1, dispatch_reconverge = 0;
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
        else if (!strcmp(argv[i], "--direct")) { /* accepted for older corpus commands */
        } else if (argv[i][0] != '-' && !dir)
            dir = argv[i];
        else {
            fprintf(stderr, "usage: %s CORPUS_DIR [--out DIR] [--validate] [--verbose] [--no-flush] [--dispatch-reconverge]\n", argv[0]);
            return 2;
        }
    }
    if (!dir) {
        fprintf(stderr, "usage: %s CORPUS_DIR [--out DIR] [--validate] [--verbose] [--no-flush] [--dispatch-reconverge]\n", argv[0]);
        return 2;
    }
    /* Match a renderer with non-uniform sampled image array indexing. Otherwise indexed
       LDC handles look like plain bindless textures and corpus validation misses them. */
    g_shader_texture_arrays = 1;
    /* and VK_KHR_shader_maximal_reconvergence, which NVIDIA, AMD and Intel drivers have: programs whose lanes
       reconverge take structured control flow as on such a device (--dispatch-reconverge keeps their dispatch loop) */
    g_shader_maximal_reconvergence = !dispatch_reconverge;
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
        u32* spv = shader_to_spirv(code, max_bytes, stage, &info, &words, flush, 1, 1, 1);
        free(code);
        if (!spv) {
            unsupported++;
            if (verbose) printf("%s: unsupported: %s\n", base, info.first_unsupported);
            continue;
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
