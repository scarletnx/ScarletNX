/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/shader/shader.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCHED   0x001F8000FC0007E0ull
#define EXIT    0xE30000000007000Full
#define FMUL    0x5C68000000070000ull
#define IPA     0xE000000000070000ull
#define MUFU    0x5080000000070000ull
#define FFMA    0x5980000000070000ull
#define MOV_C   0x4C98000000070000ull
#define TEXS    0xD800000000070000ull
#define FSETP   0x5BB0000000070000ull
#define FMNMX   0x5C60000000070000ull
#define BRA     0xE24000000007000Full
#define AST     0xEFF0000000070000ull
#define FADD32I 0x0800000000070000ull
#define SYNC    0xF0F800000007000Full
#define VOTE    0x50D8000000070000ull
#define SHFL    0xEF10000000070000ull

static u64 mov32i(u32 reg, u32 value) { return 0x0100000000000000ull | ((u64)value << 20) | 0x71000ull | reg; }

/* code is the program's 64-bit words as host numbers: the translator takes guest memory's bytes, little-endian */
static u32* translate(const u64* code, int stage, ShaderInfo* info, size_t* words) {
    u8 guest[64 * 8];
    for (int i = 0; i < 64; i++) snx_st_le64(guest + i * 8, code[i]);
    return shader_to_spirv(guest, sizeof guest, stage, info, words, 1, 1, 1, 1);
}

static int save(const char* path, u32* spv, size_t words) {
    FILE* out = fopen(path, "wb");
    if (!out) return 0;
    snx_le32_array(spv, words); /* a .spv file is little-endian, the same bytes from any host */
    int ok = fwrite(spv, sizeof *spv, words, out) == words;
    if (fclose(out)) ok = 0;
    return ok;
}

int main(int argc, char** argv) {
    if (argc != 2 && argc != 6) return 2;
    int check_only = argc == 2 && !strcmp(argv[1], "--check-only");
    if (argc == 2 && !check_only) return 2;
    u64 code[64] = {0};
    code[9] = 15; /* SPH word 18: one RGBA fragment output */
    code[3] = 3;  /* SPH byte 0x18: input location 0 is screen linear */
    code[10] = SCHED;
    code[11] = IPA | (0x20ull << 30);    /* r0 = location 0, x */
    code[12] = mov32i(1, 0x3f000000);    /* 0.5 */
    code[13] = MOV_C | (1ull << 39) | 1; /* r1 = c[0][0] */
    code[14] = SCHED;
    code[15] = FMUL | (1ull << 20) | 2;                /* r2 = r0 * r1 */
    code[16] = FFMA | (1ull << 39) | (1ull << 20) | 2; /* r2 = fma(r0, r1, r1) */
    code[17] = MUFU | (2ull << 20) | (1ull << 8) | 3;  /* r3 = exp2(r1) */
    code[18] = SCHED;
    code[19] = FSETP | (9ull << 48) | (7ull << 39) | (1ull << 8) | 7;   /* p0 = r1 < r0 */
    code[20] = TEXS | (1ull << 53) | (255ull << 28) | (1ull << 20) | 3; /* 2D sample to r3 */
    code[21] = (TEXS & ~(7ull << 16)) | (4ull << 53) | (1ull << 36) | (255ull << 28) | (1ull << 20) | 3;
    code[22] = SCHED;
    code[23] = FMNMX | (7ull << 39) | (1ull << 20) | 3; /* min(r0, r1) with NaN handling */
    code[24] = BRA | (16ull << 20);                     /* skip an instruction across a schedule word */
    code[25] = mov32i(3, 0);
    code[26] = SCHED;
    code[27] = EXIT;
    ShaderInfo info = {0};
    size_t words = 0;
    u32* spv = translate(code, ST_FS, &info, &words);
    if (!spv || info.stage != ST_FS || info.sph[18] != 15 || info.cbuf_mask != 1 || info.cbuf_max[0] != 16 ||
        info.ntex != 2 || !info.tex[1].shadow) {
        free(spv);
        return 1;
    }
    int ok = check_only || save(argv[1], spv, words);
    free(spv);
    memset(code, 0, sizeof code);
    code[10] = SCHED;
    code[11] = AST | (0x70ull << 20) | (255ull << 8); /* position.x = r0; W retains its default 1 */
    code[12] = EXIT;
    info = (ShaderInfo){0};
    spv = translate(code, ST_VS, &info, &words);
    if (!spv || info.stage != ST_VS || (!check_only && !save(argv[2], spv, words))) ok = 0;
    free(spv);
    memset(code, 0, sizeof code);
    code[9] = 15;
    code[10] = SCHED;
    /* |r0| + |-1.0|: the register's absolute value needs GLSL.std.450, the immediate's is 1.0 as translated */
    code[11] = FADD32I | (1ull << 54) | (1ull << 57) | (0xBF800000ull << 20);
    code[12] = EXIT;
    info = (ShaderInfo){0};
    spv = translate(code, ST_FS, &info, &words);
    if (!spv || (!check_only && !save(argv[3], spv, words))) ok = 0;
    free(spv);
    code[11] = SYNC;
    info = (ShaderInfo){0};
    spv = translate(code, ST_FS, &info, &words);
    if (!spv || (!check_only && !save(argv[4], spv, words))) ok = 0;
    free(spv);
    memset(code, 0, sizeof code);
    code[10] = SCHED;
    code[11] = VOTE | (7ull << 39); /* ballot P7 and vote all */
    code[12] = SHFL | (1ull << 28) | (1ull << 29) | (31ull << 34) | (1ull << 20) | 1;
    code[13] = EXIT;
    info = (ShaderInfo){0};
    info.cs_local[0] = 32;
    info.cs_local[1] = info.cs_local[2] = 1;
    spv = translate(code, ST_CS, &info, &words);
    if (!spv || (!check_only && !save(argv[5], spv, words))) ok = 0;
    free(spv);
    return ok ? 0 : 1;
}
