/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_fp_host_lanes_kept and snx_fp_host_lanes_kept_avx (cpu.h) keep what LLVM's preserve_all keeps: called from
   assembly with a pattern in each register that convention keeps, the arguments' among them, every one holds it after
   the call, and the reply and the lanes are snx_fp_host_lanes's. On x86-64 the AVX one keeps ymm0-15 whole, which the
   helper's own calls may clear (a libc memcpy's vzeroupper), and the other xmm0-15; on AArch64 x9-x15 and q8-q31 */
#include "common/cpu/aarch64/cpu.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define EXIT_SKIPPED 77

/* the call's target and arguments, the patterns, and what the registers held after it. The assembly reaches each
   field at its offset (checked in main) */
typedef struct {
    u64 fn, args[7];
    u64 gpr[8], gpr_after[8];
    unsigned char vec[32][32], vec_after[32][32];
    u64 reply;
} Ctx;

#if defined(__x86_64__)
/* SysV: rdi, rsi, rdx, rcx, r8, r9 hold the first six arguments, the stack the seventh, r10 the pattern gpr[4].
   Windows: rcx, rdx, r8, r9 the first four, the stack the rest from its top (LLVM gives a preserve_all call no 32-byte
   home space, and the C thunk, compiled with it, reads them there), r10 gpr[4], rsi gpr[6] and rdi gpr[7]. Each comes
   back in gpr_after at its slot (5 unused). rsp goes past the red zone, 16-aligned at the call, and comes back from
   r12 */
#if defined(_WIN32)
#define SNX_ARGS                                                                                                       \
    "mov 8(%%rbx), %%rcx\n mov 16(%%rbx), %%rdx\n mov 24(%%rbx), %%r8\n mov 32(%%rbx), %%r9\n"                         \
    "mov 112(%%rbx), %%rsi\n mov 120(%%rbx), %%rdi\n sub $32, %%rsp\n"                                                 \
    "mov 40(%%rbx), %%rax\n mov %%rax, (%%rsp)\n mov 48(%%rbx), %%rax\n mov %%rax, 8(%%rsp)\n"                         \
    "mov 56(%%rbx), %%rax\n mov %%rax, 16(%%rsp)\n"
#define SNX_SAVE                                                                                                       \
    "mov %%rcx, 128(%%rbx)\n mov %%rdx, 136(%%rbx)\n mov %%r8, 144(%%rbx)\n mov %%r9, 152(%%rbx)\n"                    \
    "mov %%rsi, 176(%%rbx)\n mov %%rdi, 184(%%rbx)\n"
#else
#define SNX_ARGS                                                                                                       \
    "mov 8(%%rbx), %%rdi\n mov 16(%%rbx), %%rsi\n mov 24(%%rbx), %%rdx\n mov 32(%%rbx), %%rcx\n"                       \
    "mov 40(%%rbx), %%r8\n mov 48(%%rbx), %%r9\n sub $16, %%rsp\n mov 56(%%rbx), %%rax\n mov %%rax, (%%rsp)\n"
#define SNX_SAVE                                                                                                       \
    "mov %%rdi, 128(%%rbx)\n mov %%rsi, 136(%%rbx)\n mov %%rdx, 144(%%rbx)\n mov %%rcx, 152(%%rbx)\n"                  \
    "mov %%r8, 176(%%rbx)\n mov %%r9, 184(%%rbx)\n"
#endif
#define SNX_CALL(LOAD, STORE)                                                                                          \
    __asm__ volatile("mov %%rsp, %%r12\n sub $128, %%rsp\n and $-16, %%rsp\n" LOAD SNX_ARGS                            \
                     "mov 96(%%rbx), %%r10\n call *(%%rbx)\n mov %%rax, 2240(%%rbx)\n" SNX_SAVE                        \
                     "mov %%r10, 160(%%rbx)\n" STORE "mov %%r12, %%rsp\n"                                              \
                     :                                                                                                 \
                     : "b"(t)                                                                                          \
                     : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "xmm0", "xmm1", "xmm2",     \
                       "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13",     \
                       "xmm14", "xmm15", "memory", "cc")
static void call(Ctx* t, int bytes) {
    if (bytes == 32)
        SNX_CALL("vmovdqu 192(%%rbx), %%ymm0\n"
                 "vmovdqu 224(%%rbx), %%ymm1\n"
                 "vmovdqu 256(%%rbx), %%ymm2\n"
                 "vmovdqu 288(%%rbx), %%ymm3\n"
                 "vmovdqu 320(%%rbx), %%ymm4\n"
                 "vmovdqu 352(%%rbx), %%ymm5\n"
                 "vmovdqu 384(%%rbx), %%ymm6\n"
                 "vmovdqu 416(%%rbx), %%ymm7\n"
                 "vmovdqu 448(%%rbx), %%ymm8\n"
                 "vmovdqu 480(%%rbx), %%ymm9\n"
                 "vmovdqu 512(%%rbx), %%ymm10\n"
                 "vmovdqu 544(%%rbx), %%ymm11\n"
                 "vmovdqu 576(%%rbx), %%ymm12\n"
                 "vmovdqu 608(%%rbx), %%ymm13\n"
                 "vmovdqu 640(%%rbx), %%ymm14\n"
                 "vmovdqu 672(%%rbx), %%ymm15\n",
                 "vmovdqu %%ymm0, 1216(%%rbx)\n"
                 "vmovdqu %%ymm1, 1248(%%rbx)\n"
                 "vmovdqu %%ymm2, 1280(%%rbx)\n"
                 "vmovdqu %%ymm3, 1312(%%rbx)\n"
                 "vmovdqu %%ymm4, 1344(%%rbx)\n"
                 "vmovdqu %%ymm5, 1376(%%rbx)\n"
                 "vmovdqu %%ymm6, 1408(%%rbx)\n"
                 "vmovdqu %%ymm7, 1440(%%rbx)\n"
                 "vmovdqu %%ymm8, 1472(%%rbx)\n"
                 "vmovdqu %%ymm9, 1504(%%rbx)\n"
                 "vmovdqu %%ymm10, 1536(%%rbx)\n"
                 "vmovdqu %%ymm11, 1568(%%rbx)\n"
                 "vmovdqu %%ymm12, 1600(%%rbx)\n"
                 "vmovdqu %%ymm13, 1632(%%rbx)\n"
                 "vmovdqu %%ymm14, 1664(%%rbx)\n"
                 "vmovdqu %%ymm15, 1696(%%rbx)\n"
                 "vzeroupper\n");
    else
        SNX_CALL("movdqu 192(%%rbx), %%xmm0\n"
                 "movdqu 224(%%rbx), %%xmm1\n"
                 "movdqu 256(%%rbx), %%xmm2\n"
                 "movdqu 288(%%rbx), %%xmm3\n"
                 "movdqu 320(%%rbx), %%xmm4\n"
                 "movdqu 352(%%rbx), %%xmm5\n"
                 "movdqu 384(%%rbx), %%xmm6\n"
                 "movdqu 416(%%rbx), %%xmm7\n"
                 "movdqu 448(%%rbx), %%xmm8\n"
                 "movdqu 480(%%rbx), %%xmm9\n"
                 "movdqu 512(%%rbx), %%xmm10\n"
                 "movdqu 544(%%rbx), %%xmm11\n"
                 "movdqu 576(%%rbx), %%xmm12\n"
                 "movdqu 608(%%rbx), %%xmm13\n"
                 "movdqu 640(%%rbx), %%xmm14\n"
                 "movdqu 672(%%rbx), %%xmm15\n",
                 "movdqu %%xmm0, 1216(%%rbx)\n"
                 "movdqu %%xmm1, 1248(%%rbx)\n"
                 "movdqu %%xmm2, 1280(%%rbx)\n"
                 "movdqu %%xmm3, 1312(%%rbx)\n"
                 "movdqu %%xmm4, 1344(%%rbx)\n"
                 "movdqu %%xmm5, 1376(%%rbx)\n"
                 "movdqu %%xmm6, 1408(%%rbx)\n"
                 "movdqu %%xmm7, 1440(%%rbx)\n"
                 "movdqu %%xmm8, 1472(%%rbx)\n"
                 "movdqu %%xmm9, 1504(%%rbx)\n"
                 "movdqu %%xmm10, 1536(%%rbx)\n"
                 "movdqu %%xmm11, 1568(%%rbx)\n"
                 "movdqu %%xmm12, 1600(%%rbx)\n"
                 "movdqu %%xmm13, 1632(%%rbx)\n"
                 "movdqu %%xmm14, 1664(%%rbx)\n"
                 "movdqu %%xmm15, 1696(%%rbx)\n");
}
/* the registers to compare, by gpr_after slot, and what each held before */
static void expected(const Ctx* t, const char* names[8], u64 want[8]) {
#if defined(_WIN32)
    static const char* const n[8] = {"rcx", "rdx", "r8", "r9", "r10", "", "rsi", "rdi"};
    u64 w[8] = {t->args[0], t->args[1], t->args[2], t->args[3], t->gpr[4], 0, t->gpr[6], t->gpr[7]};
#else
    static const char* const n[8] = {"rdi", "rsi", "rdx", "rcx", "r10", "", "r8", "r9"};
    u64 w[8] = {t->args[0], t->args[1], t->args[2], t->args[3], t->gpr[4], 0, t->args[4], t->args[5]};
#endif
    memcpy(names, n, sizeof n);
    memcpy(want, w, sizeof w);
}
#define SNX_VEC0 0
#define SNX_VECS 16
#elif defined(__aarch64__)
/* x0-x6 hold the arguments, x9-x15 the patterns gpr[0..6] and q8-q31 the patterns vec[8..31]; x20 holds t */
static void call(Ctx* t, int bytes) {
    register Ctx* base __asm__("x20") = t;
    (void)bytes;
    __asm__ volatile("ldr x9, [x20, #64]\n"
                     "ldr x10, [x20, #72]\n"
                     "ldr x11, [x20, #80]\n"
                     "ldr x12, [x20, #88]\n"
                     "ldr x13, [x20, #96]\n"
                     "ldr x14, [x20, #104]\n"
                     "ldr x15, [x20, #112]\n"
                     "ldr q8, [x20, #448]\n"
                     "ldr q9, [x20, #480]\n"
                     "ldr q10, [x20, #512]\n"
                     "ldr q11, [x20, #544]\n"
                     "ldr q12, [x20, #576]\n"
                     "ldr q13, [x20, #608]\n"
                     "ldr q14, [x20, #640]\n"
                     "ldr q15, [x20, #672]\n"
                     "ldr q16, [x20, #704]\n"
                     "ldr q17, [x20, #736]\n"
                     "ldr q18, [x20, #768]\n"
                     "ldr q19, [x20, #800]\n"
                     "ldr q20, [x20, #832]\n"
                     "ldr q21, [x20, #864]\n"
                     "ldr q22, [x20, #896]\n"
                     "ldr q23, [x20, #928]\n"
                     "ldr q24, [x20, #960]\n"
                     "ldr q25, [x20, #992]\n"
                     "ldr q26, [x20, #1024]\n"
                     "ldr q27, [x20, #1056]\n"
                     "ldr q28, [x20, #1088]\n"
                     "ldr q29, [x20, #1120]\n"
                     "ldr q30, [x20, #1152]\n"
                     "ldr q31, [x20, #1184]\n"
                     "ldp x0, x1, [x20, #8]\n ldp x2, x3, [x20, #24]\n ldp x4, x5, [x20, #40]\n ldr x6, [x20, #56]\n"
                     "ldr x16, [x20]\n blr x16\n str x0, [x20, #2240]\n"
                     "str x9, [x20, #128]\n"
                     "str x10, [x20, #136]\n"
                     "str x11, [x20, #144]\n"
                     "str x12, [x20, #152]\n"
                     "str x13, [x20, #160]\n"
                     "str x14, [x20, #168]\n"
                     "str x15, [x20, #176]\n"
                     "str q8, [x20, #1472]\n"
                     "str q9, [x20, #1504]\n"
                     "str q10, [x20, #1536]\n"
                     "str q11, [x20, #1568]\n"
                     "str q12, [x20, #1600]\n"
                     "str q13, [x20, #1632]\n"
                     "str q14, [x20, #1664]\n"
                     "str q15, [x20, #1696]\n"
                     "str q16, [x20, #1728]\n"
                     "str q17, [x20, #1760]\n"
                     "str q18, [x20, #1792]\n"
                     "str q19, [x20, #1824]\n"
                     "str q20, [x20, #1856]\n"
                     "str q21, [x20, #1888]\n"
                     "str q22, [x20, #1920]\n"
                     "str q23, [x20, #1952]\n"
                     "str q24, [x20, #1984]\n"
                     "str q25, [x20, #2016]\n"
                     "str q26, [x20, #2048]\n"
                     "str q27, [x20, #2080]\n"
                     "str q28, [x20, #2112]\n"
                     "str q29, [x20, #2144]\n"
                     "str q30, [x20, #2176]\n"
                     "str q31, [x20, #2208]\n"
                     :
                     : "r"(base)
                     : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14",
                       "x15", "x16", "x17", "x30", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10",
                       "v11", "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24",
                       "v25", "v26", "v27", "v28", "v29", "v30", "v31", "memory", "cc");
}
static void expected(const Ctx* t, const char* names[8], u64 want[8]) {
    static const char* const n[8] = {"x9", "x10", "x11", "x12", "x13", "x14", "x15", ""};
    memcpy(names, n, sizeof n);
    for (int i = 0; i < 8; i++) want[i] = i < 7 ? t->gpr[i] : 0;
}
#define SNX_VEC0 8
#define SNX_VECS 24
#endif

#if defined(__x86_64__) || defined(__aarch64__)
/* the file's state: the call's context, too large for the stack frame the assembly moves, and the checks failed */
static struct {
    Ctx t;
    int failures;
} g_kept;

/* one call of fn through call() with desc: its reply and lanes are snx_fp_host_lanes's, and every kept register
   holds its pattern (bytes of each vector register: 16, or 32 for ymm) */
static void check_desc(const char* name, u64 fn, int bytes, u32 desc) {
    Ctx* t = &g_kept.t;
    float x[4] = {1.5f, -2.0f, 3.25f, 1e-40f}, y[4] = {0.5f, 4.0f, -1.0f, 2.0f}, want[4], got[4];
    /* QC and IDC: the reply carries the FPSR it was given, so a seventh argument read from anywhere else shows */
    u32 fpsr = 0x08000080u;
    u32 reply = snx_fp_host_lanes(want, x, y, y, desc, 0, fpsr);
    memset(t, 0, sizeof *t);
    for (int i = 0; i < 8; i++) t->gpr[i] = 0x1111111111111111ull * (u64)(i + 1) ^ 0x0123456789abcdefull;
    for (int r = 0; r < 32; r++)
        for (int b = 0; b < 32; b++) t->vec[r][b] = (unsigned char)(r * 37 + b * 11 + 5);
    t->fn = fn;
    t->args[0] = (u64)(size_t)got, t->args[1] = (u64)(size_t)x, t->args[2] = (u64)(size_t)y,
    t->args[3] = (u64)(size_t)y;
    t->args[4] = desc, t->args[6] = fpsr;
    call(t, bytes);
    if ((u32)t->reply != reply || memcmp(got, want, sizeof got)) {
        fprintf(stderr, "FAIL: %s, desc %#x, replied %#x with lane 0 %08x; snx_fp_host_lanes %#x with %08x\n", name,
                desc, (u32)t->reply, ((const u32*)(const void*)got)[0], reply, ((const u32*)(const void*)want)[0]);
        g_kept.failures++;
    }
    const char* names[8];
    u64 before[8];
    expected(t, names, before);
    for (int i = 0; i < 8; i++)
        if (names[i][0] && t->gpr_after[i] != before[i]) {
            fprintf(stderr, "FAIL: %s, desc %#x: %s is %#llx after the call, %#llx before\n", name, desc, names[i],
                    (unsigned long long)t->gpr_after[i], (unsigned long long)before[i]);
            g_kept.failures++;
        }
    for (int r = SNX_VEC0; r < SNX_VEC0 + SNX_VECS; r++)
        if (memcmp(t->vec_after[r], t->vec[r], (size_t)bytes)) {
            fprintf(stderr, "FAIL: %s, desc %#x: vector register %d differs after the call in its first %d bytes\n",
                    name, desc, r, bytes);
            g_kept.failures++;
        }
}
/* the helper's binary op 0 on four singles (its four-lane path) and on two doubles (lane by lane: its calls there write
   the argument registers, which a thunk that did not give them back would leave changed) */
static void check(const char* name, u64 fn, int bytes) {
    check_desc(name, fn, bytes, 0u | 4u << 4 | 4u << 8);
    check_desc(name, fn, bytes, 0u | 8u << 4 | 2u << 8);
}
#endif

int main(void) {
    if (offsetof(Ctx, args) != 8 || offsetof(Ctx, gpr) != 64 || offsetof(Ctx, gpr_after) != 128 ||
        offsetof(Ctx, vec) != 192 || offsetof(Ctx, vec_after) != 1216 || offsetof(Ctx, reply) != 2240) {
        fprintf(stderr, "FAIL: Ctx's layout isn't the one the assembly reads\n");
        return 1;
    }
#if defined(__x86_64__)
    check("snx_fp_host_lanes_kept", (u64)(size_t)snx_fp_host_lanes_kept, 16);
    if (__builtin_cpu_supports("avx"))
        check("snx_fp_host_lanes_kept_avx", (u64)(size_t)snx_fp_host_lanes_kept_avx, 32);
    else
        printf("no AVX here: snx_fp_host_lanes_kept_avx is not called\n");
#elif defined(__aarch64__)
    check("snx_fp_host_lanes_kept", (u64)(size_t)snx_fp_host_lanes_kept, 16);
#else
    printf("skipped: preserve_all thunks exist on x86-64 and AArch64 only\n");
    return EXIT_SKIPPED;
#endif
#if defined(__x86_64__) || defined(__aarch64__)
    if (g_kept.failures) {
        fprintf(stderr, "%d checks failed\n", g_kept.failures);
        return 1;
    }
    printf("fp_lanes_kept: the registers preserve_all keeps come back, and the reply and lanes are the helper's\n");
    return 0;
#endif
}
