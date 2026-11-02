/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/shader/ampere.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "ampere_shader:%d: %s\n", __LINE__, #x);                                                   \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)
static void instruction(u8* out, u32 op, u32 dst, u32 a, u32 value, u32 flags) {
    snx_st_le32(out, op | 7u << 12 | dst << 16 | a << 24);
    snx_st_le32(out + 4, value);
    snx_st_le32(out + 8, flags);
    snx_st_le32(out + 12, 0x000fe200);
}
typedef struct {
    u32 count, offsets[8];
} Diagnostics;
static void diagnostic(void* ctx, u32 offset, const char* reason) {
    Diagnostics* d = ctx;
    if (d->count < 8) d->offsets[d->count] = offset;
    ++d->count;
    if (!strstr(reason, "Ampere opcode")) abort();
}
int main(void) {
    u8 source[256] = {0}, out[1024];
    /* Native register, carry and instruction boundaries must fail closed, not run a different Maxwell operation. */
    static const struct {
        u32 op, dst, a, value, flags;
    } invalid[] = {
        {0xfff, 0, 0, 0, 0},
        {0x424, 240, 255, 1, 0x078e00ff},  /* neither layout may overlap its uniform or scratch bank */
        {0xab9, 56, 0, 16u << 6, 5u << 9}, /* uniform bank past its reserved range */
        {0xab9, 1, 0, 16u << 6, 5u << 9},  /* misaligned uniform pair */
        {0xc10, 203, 255, 8, 0x0fffe0ff},  /* wide vectors cannot also use UR8 */
        {0x23a, 203, 0, 0, 0},             /* wide vectors cannot share the matrix scratch */
        {0x326, 0, 0, 255, 0x000e2020},    /* offset sampling is not centroid sampling */
        {0x326, 0, 0, 255, 0x000e8020},    /* unimplemented interpolation frequency */
        {0x986, 0, 0, 1, 0x0810ab00},      /* misaligned store data pair */
        {0x981, 1, 0, 0, 0x081eab00},      /* misaligned load data pair */
        {0x981, 0, 1, 0, 0x0c1ea900},      /* misaligned 64-bit address offset */
        {0x981, 0, 0, 56, 0x081ea900},     /* uniform base outside its bank */
        {0x981, 0, 0, 0, 0x081ea908},      /* constant-false load predicate is not supported */
        {0x981, 0, 0, 0, 0x081e4900},      /* unsupported memory ordering */
        {0x306, 0, 0, 1u << 28, 0x00201000}, /* U32 has no byte selector */
        {0x306, 0, 0, 2u << 28, 0x00101000}, /* U16 has two halves, not four */
        {0x306, 0, 0, 0, 0x00005000},        /* unimplemented I2F rounding */
        {0x386, 0, 0, 0, 0x0010af00},       /* store width 7 is not a 128-bit store */
        {0xa02, 0, 0, 18u << 22, 0xf00},    /* banks 18..23 are not root tables */
        {0xa02, 0, 0, 23u << 22, 0xf00},
        {0xa02, 0, 0, 24u << 22 | 256u << 6, 0xf00}, /* outside the root table */
        {0x811, 0, 0, 0, 0x078e01ff},      /* LEA negation must not be discarded */
        {0x94d, 0, 0, 0, 0x03900000},      /* extra exit predicate */
        {0x947, 0, 0, 4, 0x03800000},      /* a branch into the middle of an instruction */
        {0x947, 0, 0, 32, 0x03800000},     /* branch outside the input */
        {0x941, 0, 0, 0, 0x03800000},      /* no active reconvergence token */
        {0x944, 0, 0, 0, 0x03c00000},      /* ordinary calls still require return-link translation */
        {0x948, 0, 0, 0x7fffffff, 0x03800000}, /* a partial mask is not a full-warp barrier */
        {0x948, 1, 0, 0xffffffff, 0x03800000}, /* reserved barrier register fields */
        {0x81a, 0, 0, 33, 0x200},             /* SGXT has at most 32 source bits */
        {0x232, 0, 0, 0, 0x400},              /* unimplemented F32 source selector */
        {0x59f, 0, 4, 0x60000c08, 0x0010e400}, /* U64 cannot be reduced as one U32 texel */
        {0x59f, 0, 4, 0xa0000c08, 0x0010e000}, /* a 3D surface needs a third coordinate */
    };
    for (u32 i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
        instruction(source, invalid[i].op, invalid[i].dst, invalid[i].a, invalid[i].value, invalid[i].flags);
        instruction(source + 16, 0x94d, 0, 0, 0, 0x03800000);
        ShaderInfo si = {0};
        CHECK(!shader_ampere_lower(source, 32, out, sizeof out, &si, NULL) && si.unsupported);
    }
    /* An unknown instruction must not hide a later unsupported modifier, or make valid intervening arithmetic
       look unsupported. A flow instruction is counted as unchecked, not diagnosed out of context. */
    instruction(source, 0xfff, 0, 0, 0, 0);
    instruction(source + 16, 0x802, 0, 0, 7, 0xf00);
    instruction(source + 32, 0x306, 0, 0, 0, 0x5000);
    instruction(source + 48, 0x941, 0, 0, 0, 0x03800000);
    instruction(source + 64, 0xfff, 0, 0, 0, 0);
    instruction(source + 80, 0x94d, 0, 0, 0, 0x03800000);
    Diagnostics diagnostics = {0};
    CHECK(shader_ampere_diagnose(source, 96, diagnostic, &diagnostics) == 1);
    CHECK(diagnostics.count == 3 && diagnostics.offsets[0] == 0 &&
          diagnostics.offsets[1] == 32 && diagnostics.offsets[2] == 64);
    ShaderInfo first = {0};
    CHECK(!shader_ampere_lower(source, 96, out, sizeof out, &first, NULL) &&
          strstr(first.first_unsupported, "opcode fff at +0"));
    CHECK(shader_ampere_diagnose(source, 80, diagnostic, &diagnostics) == ~0u && diagnostics.count == 3);
    instruction(source, 0x947, 0, 0, 32, 0x03800000);
    instruction(source + 16, 0x94d, 0, 0, 0, 0x03800000);
    instruction(source + 64, 0x94d, 0, 0, 0, 0x03800000);
    diagnostics = (Diagnostics){0};
    CHECK(shader_ampere_diagnose(source, 80, diagnostic, &diagnostics) == 2);
    CHECK(diagnostics.count == 1 && diagnostics.offsets[0] == 32);
    /* A leaf that changes the guest's return address cannot use an unrelated implicit Maxwell return frame. */
    instruction(source, 0x802, 0, 0, 32, 0xf00);
    instruction(source + 16, 0x944, 0, 0, 16, 0x03c00000);
    instruction(source + 32, 0x94d, 0, 0, 0, 0x03800000);
    instruction(source + 48, 0x802, 0, 0, 99, 0xf00);
    instruction(source + 64, 0x202, 18, 0, 0, 0xf00);
    instruction(source + 80, 0x802, 19, 0, 0, 0xf00);
    instruction(source + 96, 0x950, 0, 18, 0xffffff90, 0x03c3ffff);
    ShaderInfo bad_link = {0};
    CHECK(!shader_ampere_lower(source, 112, out, sizeof out, &bad_link, NULL) && bad_link.unsupported);
    /* IMAD moves also stage native return pairs. The zero high word need not be adjacent to RET, but must be
       preserved and must really be zero. */
    instruction(source + 48, 0x424, 19, 255, 0, 0x078e00ff);
    for (u32 at = 64; at < 176; at += 16) instruction(source + at, 0x918, 0, 0, 0, 0);
    instruction(source + 176, 0x224, 18, 255, 255, 0x078e0000);
    instruction(source + 192, 0x950, 0, 18, 0u - 208, 0x03c3ffff);
    bad_link = (ShaderInfo){0};
    CHECK(shader_ampere_lower(source, 208, out, sizeof out, &bad_link, NULL));
    instruction(source + 48, 0x424, 19, 255, 1, 0x078e00ff);
    bad_link = (ShaderInfo){0};
    CHECK(!shader_ampere_lower(source, 208, out, sizeof out, &bad_link, NULL) && bad_link.unsupported);
    instruction(source, 0x424, 0, 255, 0xfedcba98, 0x078e00ff);
    instruction(source + 16, 0x94d, 0, 0, 0, 0x03800000);
    ShaderInfo si = {0};
    CHECK(!shader_ampere_lower(source, 31, out, sizeof out, &si, NULL) && si.unsupported);
    si = (ShaderInfo){0};
    CHECK(!shader_ampere_lower(source, 32, out, 95, &si, NULL) && si.unsupported);
    si = (ShaderInfo){0};
    CHECK(!shader_ampere_lower(source, 16, out, sizeof out, &si, NULL) && si.unsupported);
    /* BRA.U: bit 0 of the offset word is the uniformity hint NVIDIA's compiler sets, not part of the displacement,
       which counts from bit 2 (bit 34 of the instruction). Read as an offset, it put the target mid-instruction. */
    u8 branch[48];
    instruction(branch, 0x947, 0, 0, 16 | 1, 0x03800000);
    instruction(branch + 16, 0x802, 0, 0, 1, 0xf00);
    instruction(branch + 32, 0x94d, 0, 0, 0, 0x03800000);
    si = (ShaderInfo){0};
    CHECK(shader_ampere_lower(branch, sizeof branch, out, sizeof out, &si, NULL) && !si.unsupported);
    /* TEX with an explicit LOD and a depth reference (scalar layout: R4, R5, then R6 the LOD, R7 the reference).
       Maxwell's TEX.B reads them after the handle in R252: the LOD in R253, the reference in R254, its DC bit 50 set. */
    u8 shadow[48];
    instruction(shadow, 0xb60, 0, 4, 0x30c01006, 0x018e41ff);
    instruction(shadow + 16, 0x94d, 0, 0, 0, 0x03800000);
    si = (ShaderInfo){0};
    u32 shadow_bytes = shader_ampere_lower(shadow, 32, out, sizeof out, &si, NULL);
    CHECK(shadow_bytes && !si.unsupported);
    int lod_staged = 0, reference_staged = 0, compared = 0;
    for (u32 at = 80; at + 8 <= shadow_bytes; at += 8) {
        u64 word = snx_ld_le64(out + at);
        if ((word >> 51) == (0x5c98078000000000ull >> 51) && (word >> 20 & 255) == 6) lod_staged = (word & 255) == 253;
        if ((word >> 51) == (0x5c98078000000000ull >> 51) && (word >> 20 & 255) == 7)
            reference_staged = (word & 255) == 254;
        if ((word >> 54) == (0xde80000000000000ull >> 54)) compared = (word >> 50 & 1) && (word >> 20 & 255) == 252;
    }
    CHECK(lod_staged && reference_staged && compared);
    /* R254 holds the uniform predicates while a program uses them: that placement must then be refused */
    instruction(shadow, 0x88c, 0, 2, 0, 0x0bf02070);
    instruction(shadow + 16, 0xb60, 0, 4, 0x30c01006, 0x018e41ff);
    instruction(shadow + 32, 0x94d, 0, 0, 0, 0x03800000);
    si = (ShaderInfo){0};
    CHECK(!shader_ampere_lower(shadow, 48, out, sizeof out, &si, NULL) && si.unsupported &&
          strstr(si.first_unsupported, "opcode b60"));
    /* A valid terminal program must also pass the actual SPIR-V frontend, not just the encoder's own checks. */
    si = (ShaderInfo){.cs_local = {1, 1, 1}};
    u32 consumed = 0;
    u32 bytes = shader_ampere_lower(source, sizeof source, out, sizeof out, &si, &consumed);
    CHECK(consumed == 32);
    size_t words = 0;
    u32* spirv = bytes ? shader_to_spirv(out, bytes, ST_CS, &si, &words, 0, 1, 0, 0) : NULL;
    CHECK(spirv && !si.unsupported);
    free(spirv);
    /* Native warp barriers require a 32-lane subgroup in compute and fragment programs, not a workgroup barrier.
       A 64-lane physical subgroup cannot stand in for two independent native warps. */
    instruction(source, 0x948, 0, 0, 0xffffffff, 0x03800000);
    instruction(source + 16, 0x94d, 0, 0, 0, 0x03800000);
    si = (ShaderInfo){0};
    bytes = shader_ampere_lower(source, 32, out, sizeof out, &si, NULL);
    CHECK(bytes);
    for (u32 stage = ST_FS; stage <= ST_CS; ++stage)
        for (u32 caps = 0; caps < 4; ++caps)
            for (u32 native = 0; native < 2; ++native) {
                snx_st_le32(out, native ? 0x81 : 0x61);
                si = (ShaderInfo){.cs_local = {64, 1, 1}};
                spirv = shader_to_spirv(out, bytes, stage, &si, &words, 0, 1, 0, caps);
                CHECK(native && caps == 3 ? spirv != NULL : spirv == NULL && si.unsupported);
                free(spirv);
            }
    /* Native root banks do not extend Maxwell's constant banks or compute's parameter banks. */
    instruction(source, 0xa02, 0, 0, 24u << 22, 0xf00);
    instruction(source + 16, 0x94d, 0, 0, 0, 0x03800000);
    si = (ShaderInfo){0};
    bytes = shader_ampere_lower(source, 32, out, sizeof out, &si, NULL);
    CHECK(bytes);
    for (u32 native = 0; native < 2; ++native) {
        snx_st_le32(out, native ? 0x81 : 0x61);
        si = (ShaderInfo){0};
        spirv = shader_to_spirv(out, bytes, ST_FS, &si, &words, 0, 1, 0, 0);
        CHECK(native ? spirv != NULL : spirv == NULL && si.unsupported);
        free(spirv);
    }
    si = (ShaderInfo){.cs_local = {1, 1, 1}};
    spirv = shader_to_spirv(out, bytes, ST_CS, &si, &words, 0, 1, 0, 0);
    CHECK(!spirv && si.unsupported);
    /* The native VPRS selector is not a Maxwell output, a fragment output, or a vector store. */
    instruction(source, 0x322, 0, 255, 0x5cff, 255);
    instruction(source + 16, 0x94d, 0, 0, 0, 0x03800000);
    si = (ShaderInfo){0};
    bytes = shader_ampere_lower(source, 32, out, sizeof out, &si, NULL);
    CHECK(bytes);
    for (u32 native = 0; native < 2; ++native) {
        snx_st_le32(out, native ? 0x81 : 0x61);
        for (int stage = ST_VS; stage <= ST_FS; stage += ST_FS) {
            si = (ShaderInfo){0};
            spirv = shader_to_spirv(out, bytes, stage, &si, &words, 0, 1, 0, 0);
            CHECK(native && stage == ST_VS ? spirv && si.writes_vprs : !spirv && si.unsupported);
            free(spirv);
        }
    }
    instruction(source, 0x322, 0, 255, 0x5c00, 0x4ff);
    si = (ShaderInfo){0};
    bytes = shader_ampere_lower(source, 32, out, sizeof out, &si, NULL);
    CHECK(bytes);
    spirv = shader_to_spirv(out, bytes, ST_VS, &si, &words, 0, 1, 0, 0);
    CHECK(!spirv && si.unsupported);
    /* A passthrough geometry program's exit hook: LEPC, its offset, the 64-bit add and BMOV.64 ATEXIT_PC, the main
       EXIT, then OUT.FINAL, BMOV.64 ATEXIT_PC, 0 and EXIT past it. Only LEPC's address reaches the hook, so the read
       extent must take it in, and the hook must match for the LEPC to lower. */
    u8 hooked[0x90] = {0};
    instruction(hooked, 0x34e, 0, 0, 0, 0);
    instruction(hooked + 0x10, 0x802, 3, 0, 0x60, 0xf00);
    instruction(hooked + 0x20, 0x210, 0, 0, 3, 0x07f1e0ff);
    instruction(hooked + 0x30, 0x224, 1, 255, 255, 0x000e0401);
    instruction(hooked + 0x40, 0x357, 0, 0, 0, 0);
    instruction(hooked + 0x50, 0x94d, 0, 0, 0, 0x03800000);
    instruction(hooked + 0x60, 0x324, 255, 0, 255, 0);
    instruction(hooked + 0x70, 0x957, 0, 0, 0, 0);
    instruction(hooked + 0x80, 0x94d, 0, 0, 0, 0x03800000);
    CHECK(shader_ampere_read_extent(hooked, 0x60) == sizeof hooked);
    CHECK(shader_ampere_read_extent(hooked, sizeof hooked) == sizeof hooked);
    consumed = 0;
    si = (ShaderInfo){0};
    CHECK(shader_ampere_lower(hooked, sizeof hooked, out, sizeof out, &si, &consumed) && consumed == sizeof hooked);
    instruction(hooked + 0x70, 0x918, 0, 0, 0, 0); /* not the hook's form: the LEPC has another reader */
    si = (ShaderInfo){0};
    CHECK(!shader_ampere_lower(hooked, sizeof hooked, out, sizeof out, &si, NULL) && si.unsupported);
    /* A forward reconvergence target beyond the old 64 KiB window must request more code, not accept an early
       EXIT or report a malformed branch. Leaf returns beyond that boundary need the same bounded read. */
    u32 far = 0x40020, span = far + 16;
    u8* long_source = calloc(1, span);
    CHECK(long_source);
    instruction(long_source, 0x945, 0, 0, far - 16, 0x03800000);
    instruction(long_source + 16, 0x94d, 0, 0, 0, 0x03800000);
    instruction(long_source + far, 0x94d, 0, 0, 0, 0x03800000);
    CHECK(shader_ampere_read_extent(long_source, 0x10000) == span);
    CHECK(shader_ampere_read_extent(long_source, span) == span);
    instruction(long_source, 0x945, 0, 0, far - 12, 0x03800000);
    CHECK(shader_ampere_read_extent(long_source, 0x10000) == 0x10000); /* malformed, not a larger read */
    instruction(long_source, 0x944, 0, 0, far - 16, 0x03c00000);
    instruction(long_source + far, 0x950, 0, 18, 0u - span, 0x03c3ffff);
    CHECK(shader_ampere_read_extent(long_source, 0x10000) == span);
    CHECK(shader_ampere_read_extent(long_source, span) == span);
    /* Lowered code can exceed the ordinary 128 KiB scan window too. Keep the far reconvergence target in both
       the hash extent and the SPIR-V program, rather than truncating it into a control-stack refusal. */
    for (u32 at = 0; at < span; at += 16) instruction(long_source + at, 0x918, 0, 0, 0, 0);
    instruction(long_source, 0x945, 0, 0, far - 16, 0x03800000);
    instruction(long_source + 16, 0x947, 0, 0, far - 48, 0x03800000);
    instruction(long_source + 32, 0x94d, 0, 0, 0, 0x03800000);
    instruction(long_source + far - 16, 0x941, 0, 0, 0, 0x03800000);
    instruction(long_source + far, 0x94d, 0, 0, 0, 0x03800000);
    u8* long_out = malloc(SHADER_AMPERE_LOWERED_MAX);
    CHECK(long_out);
    si = (ShaderInfo){0};
    bytes = shader_ampere_lower(long_source, span, long_out, SHADER_AMPERE_LOWERED_MAX, &si, &consumed);
    CHECK(bytes > 0x20000 && consumed == span);
    u32 hashed = 0;
    shader_hash(long_out, bytes, &hashed);
    CHECK(hashed == bytes);
    si = (ShaderInfo){.cs_local = {1, 1, 1}};
    spirv = shader_to_spirv(long_out, bytes, ST_CS, &si, &words, 0, 1, 0, 0);
    CHECK(spirv && !si.unsupported);
    free(spirv);
    free(long_out);
    free(long_source);
    puts("Ampere shader: unsupported forms refused and terminal program translated");
    return 0;
}
