/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx-sm86: reads a dump of SM86 shader memory (raw bytes, as a game's GPU memory holds them), finds the programs in
   it, and reports the opcodes the decoder knows and the ones it does not, with their counts. With --translate it
   also translates each complete program (sm86_to_spirv), and with --validate it runs spirv-val on the modules.

   Usage: snx-sm86 DUMP [--sph HEADER] [--translate] [--validate] [--out DIR] [--verbose]
   --sph: DUMP is one program that the sm86_dump setting wrote, HEADER its .sph file (a compute one has none)
   The dump is game data: keep it, and what --out writes, out of the repository. */
#include "nx/gpu/shader/sm86.h"
#include "common/utilities/endian.h"
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

#define MAX_PROGRAMS 256

static u8* read_file(const char* path, u32* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    u8* data = NULL;
    long n = fseek(f, 0, SEEK_END) ? -1 : ftell(f);
    if (n > 0 && n < 0x40000000 && !fseek(f, 0, SEEK_SET)) {
        data = (u8*)malloc((size_t)n);
        if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
            free(data);
            data = NULL;
        }
    }
    fclose(f);
    *size = data ? (u32)n : 0;
    return data;
}
static int write_file(const char* path, const void* data, size_t size) {
    FILE* file = fopen(path, "wb");
    int ok = file && fwrite(data, 1, size, file) == size;
    if (file && fclose(file)) ok = 0;
    if (!ok) fprintf(stderr, "cannot write %s\n", path);
    return ok;
}
/* spirv-val on the module at path: 1 when it passes, the first line of its message in msg otherwise */
static int validate(const char* path, char* msg, size_t size) {
    char command[1200];
    snprintf(command, sizeof command, "spirv-val --target-env vulkan1.3 \"%s\" 2>&1", path);
    FILE* proc = popen(command, "r");
    if (!proc) {
        snprintf(msg, size, "cannot run spirv-val");
        return 0;
    }
    msg[0] = 0;
    char line[512];
    while (fgets(line, sizeof line, proc))
        if (!msg[0]) {
            line[strcspn(line, "\r\n")] = 0;
            snprintf(msg, size, "%s", line);
        }
    return pclose(proc) == 0;
}
static const char* stage_name(int stage) {
    static const char* const names[] = {"VS", "TCS", "TES", "GS", "FS", "CS"};
    return stage >= 0 && stage <= ST_CS ? names[stage] : "?";
}

int main(int argc, char** argv) {
    const char *path = NULL, *out = NULL, *sph_path = NULL;
    int translate = 0, check = 0, verbose = 0, list = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(argv[i], "--sph") && i + 1 < argc)
            sph_path = argv[++i];
        else if (!strcmp(argv[i], "--translate"))
            translate = 1;
        else if (!strcmp(argv[i], "--validate"))
            translate = check = 1;
        else if (!strcmp(argv[i], "--verbose"))
            verbose = 1;
        else if (!strcmp(argv[i], "--list"))
            list = 1;
        else if (argv[i][0] != '-' && !path)
            path = argv[i];
        else {
            fprintf(stderr, "usage: %s DUMP [--translate] [--validate] [--out DIR] [--verbose]\n", argv[0]);
            return 2;
        }
    }
    if (!path) {
        fprintf(stderr, "usage: %s DUMP [--translate] [--validate] [--out DIR] [--verbose]\n", argv[0]);
        return 2;
    }
    u32 size = 0;
    u8* data = read_file(path, &size);
    if (!data) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    Sm86Program* progs = (Sm86Program*)calloc(MAX_PROGRAMS, sizeof *progs);
    u32 known[SM86__COUNT] = {0};
    u32* unknown = (u32*)calloc(4096, sizeof *unknown);
    if (!progs || !unknown) return 1;
    u32 dumped_sph[32] = {0};
    int count;
    if (sph_path) {
        /* a program that the sm86_dump setting wrote: its code alone, and its header beside it */
        u32 sph_size = 0;
        u8* sph_bytes = read_file(sph_path, &sph_size);
        if (!sph_bytes || sph_size < SM86_SPH_BYTES) {
            fprintf(stderr, "cannot read the header %s\n", sph_path);
            return 1;
        }
        for (int k = 0; k < 32; k++) dumped_sph[k] = snx_ld_le32(sph_bytes + 4 * k);
        free(sph_bytes);
        progs[0].header = progs[0].code = 0;
        progs[0].size = size & ~15u;
        progs[0].stage = sm86_sph_stage(dumped_sph[0]);
        progs[0].complete = 1;
        count = progs[0].stage >= 0 ? 1 : 0;
    } else
        count = sm86_find_programs(data, size, progs, MAX_PROGRAMS);
    int complete = 0, translated = 0, valid = 0;
    for (int p = 0; p < count; p++) {
        const Sm86Program* pr = &progs[p];
        u32 n = pr->size / 16, unknown_here = 0;
        for (u32 i = 0; i < n; i++) {
            Sm86Insn in;
            const u8* at = data + pr->code + 16 * i;
            int ok = sm86_decode(snx_ld_le64(at), snx_ld_le64(at + 8), &in);
            if (list) {
                /* the instruction: its index, guard, name and form, destination and sources, and its words */
                static const char kinds[] = "-RUIC";
                printf("  %4u %s%c%u %-14s f%u d%-3u", i, in.pred_not ? "!" : " ", in.pred == 7 ? 'T' : 'P', in.pred,
                       sm86_ops[in.op].name, in.form, in.dst);
                const Sm86Src* s3[3] = {&in.a, &in.b, &in.c};
                for (int k = 0; k < 3; k++)
                    if (s3[k]->kind == SM86_SRC_CBUF)
                        printf(" c[%u][0x%x]", s3[k]->bank, s3[k]->value);
                    else if (s3[k]->kind)
                        printf(" %c%s%x", kinds[s3[k]->kind], s3[k]->neg ? "-" : "",
                               s3[k]->kind == SM86_SRC_IMM ? s3[k]->value : s3[k]->reg);
                printf("  %016llx %016llx\n", (unsigned long long)in.lo, (unsigned long long)in.hi);
            }
            if (ok)
                known[in.op]++;
            else {
                unknown[in.lo & 0xfff]++;
                unknown_here++;
            }
        }
        complete += pr->complete;
        printf("program %d: %s at 0x%x, code 0x%x, %u instructions%s%s\n", p, stage_name(pr->stage), pr->header,
               pr->code, n, pr->complete ? "" : " (the dump ends before its end)",
               unknown_here ? ", with unknown opcodes" : "");
        if (!translate || !pr->complete) continue;
        ShaderInfo si = {0};
        u32 sph[32] = {0};
        if (sph_path)
            memcpy(sph, dumped_sph, sizeof sph);
        else if (pr->stage != ST_CS)
            for (int k = 0; k < 32; k++) sph[k] = snx_ld_le32(data + pr->header + 4 * k);
        size_t words = 0;
        u32* spv = sm86_to_spirv(data + pr->code, pr->size, pr->stage, pr->stage != ST_CS ? sph : NULL, &si, &words);
        if (!spv) {
            printf("  not translated: %s\n", si.first_unsupported);
            continue;
        }
        translated++;
        char file[1024];
        if (out)
            snprintf(file, sizeof file, "%s/program%02d_%s.spv", out, p, stage_name(pr->stage));
        else
            snprintf(file, sizeof file, "snx-sm86-%d-%d.spv", (int)SNX_PID, p);
        int wrote = write_file(file, spv, words * 4);
        free(spv);
        if (verbose) {
            printf("  %zu words, cbufs 0x%x, textures %d, global buffers %d\n", words, si.cbuf_mask, si.ntex, si.ngbuf);
            for (int k = 0; k < 18; k++)
                if (si.cbuf_mask & (1u << k)) printf("    cbuf %d: %u bytes read\n", k, si.cbuf_max[k]);
            for (int k = 0; k < si.ntex; k++)
                printf("    texture %d: handle c[%u][0x%x]\n", k, si.tex[k].bl_cbuf, si.tex[k].cbuf_off * 4);
            for (int k = 0; k < si.ngbuf; k++)
                printf("    global buffer %d: base c[%u][0x%x]%s\n", k, SH_GBUF_BANK(si.gbuf_cbuf_off[k]),
                       SH_GBUF_OFFSET(si.gbuf_cbuf_off[k]), si.gbuf_wmask >> k & 1 ? ", written" : "");
        }
        if (check && wrote) {
            char msg[512];
            if (validate(file, msg, sizeof msg)) {
                valid++;
                printf("  spirv-val: pass\n");
            } else
                printf("  spirv-val: FAIL: %s\n", msg);
        }
        if (!out && wrote) remove(file);
    }
    printf("\n%d programs, %d complete", count, complete);
    if (translate) printf(", %d translated", translated);
    if (check) printf(", %d pass spirv-val", valid);
    printf("\n\nknown opcodes:\n");
    for (int k = 1; k < SM86__COUNT; k++)
        if (known[k]) printf("  %-22s %6u\n", sm86_ops[k].name, known[k]);
    u32 kinds = 0;
    for (u32 k = 0; k < 4096; k++) kinds += unknown[k] != 0;
    printf("unknown opcodes (bits 0-11): %u\n", kinds);
    for (u32 k = 0; k < 4096; k++)
        if (unknown[k]) printf("  0x%03x %6u\n", k, unknown[k]);
    free(unknown);
    free(progs);
    free(data);
    return 0;
}
