/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The HMMA bench (shader.h): its SM86 program, which hmma_mode's Auto times in each way an HMMA takes off the
   device's matrix unit (vk/hmma_pick.c) and tests/shader_sm86_gpu.c's SNX_HMMA_BENCH times by hand; the rule that
   picks a way from those times; and the text of the file that keeps the pick for a device and driver. */
#include "nx/gpu/shader/shader.h"
#include <stdio.h>
#include <string.h>

/* an instruction's 128 bits, set a field at a time by the rules of Mesa's NAK encoder (sm70_encode.rs) */
typedef struct {
    u64 w[2];
} Word;
static void set(Word* x, unsigned lo, unsigned n, u64 v) {
    for (unsigned i = 0; i < n; i++) {
        unsigned bit = lo + i;
        u64 m = 1ull << (bit & 63);
        if (v >> i & 1)
            x->w[bit >> 6] |= m;
        else
            x->w[bit >> 6] &= ~m;
    }
}
/* the 12-bit opcode on PT, with no barrier to set or wait on */
static Word fix(u32 opcode) {
    Word x = {{0, 0}};
    set(&x, 0, 12, opcode);
    set(&x, 12, 3, 7);
    set(&x, 110, 3, 7);
    set(&x, 113, 3, 7);
    return x;
}
/* an ALU instruction: its 9-bit opcode, form, destination and source a */
static Word alu(u32 op9, u32 form, u32 dst, u32 a) {
    Word x = fix(0);
    set(&x, 0, 9, op9);
    set(&x, 9, 3, form);
    set(&x, 16, 8, dst);
    set(&x, 24, 8, a);
    return x;
}
/* the second source a register at 32 (form 1), the third RZ */
static Word rr(u32 op9, u32 dst, u32 a, u32 b) {
    Word x = alu(op9, 1, dst, a);
    set(&x, 32, 8, b);
    set(&x, 64, 8, 255);
    return x;
}
/* an immediate second source (form 4), the third the register c */
static Word ri(u32 op9, u32 dst, u32 a, u32 imm, u32 c) {
    Word x = alu(op9, 4, dst, a);
    set(&x, 32, 32, imm);
    set(&x, 64, 8, c);
    return x;
}
static void put(u8* code, u32* n, Word x) {
    for (int k = 0; k < 8; k++) {
        code[16 * *n + (u32)k] = (u8)(x.w[0] >> (8 * k));
        code[16 * *n + 8 + (u32)k] = (u8)(x.w[1] >> (8 * k));
    }
    ++*n;
}

u32 shader_hmma_bench_program(u8* code) {
    u32 n = 0;
    memset(code, 0, SH_HMMA_BENCH_BYTES);
    Word w = fix(0x919); /* S2R R0, TID.X */
    set(&w, 72, 8, 0x21);
    put(code, &n, w);
    w = fix(0x919); /* S2R R1, CTAID.X */
    set(&w, 16, 8, 1);
    set(&w, 72, 8, 0x25);
    put(code, &n, w);
    put(code, &n, ri(0x024, 2, 1, SH_HMMA_BENCH_LOCAL, 0)); /* IMAD R2, R1, 64, R0 */
    for (u32 k = 0; k < 2; k++) {
        w = fix(0xab9); /* ULDC.64 UR4 + 2k, c[0][0x20 + 8k] */
        set(&w, 16, 6, 4 + 2 * k);
        set(&w, 38, 16, SH_HMMA_BENCH_CB_IN + 8 * k);
        set(&w, 54, 5, 0);
        set(&w, 73, 3, 5);
        put(code, &n, w);
    }
    w = rr(0x011, 3, 0, 255); /* LEA R3, R0, RZ, 6: the lane's 64 bytes of inputs, the same in every workgroup */
    set(&w, 75, 5, 6);
    set(&w, 81, 3, 7);
    put(code, &n, w);
    for (u32 k = 0; k < 4; k++) {
        w = fix(0x981); /* LDG.E.128 R8 + 4k, [R3.U32 + UR4 + 16k] */
        set(&w, 16, 8, 8 + 4 * k);
        set(&w, 24, 8, 3);
        set(&w, 32, 6, 4);
        set(&w, 40, 24, 16 * k);
        set(&w, 72, 1, 1);
        set(&w, 73, 3, 6);
        set(&w, 81, 3, 7);
        set(&w, 84, 3, 1);
        set(&w, 91, 1, 1);
        put(code, &n, w);
    }
    put(code, &n, ri(0x024, 7, 0, 16, 255)); /* IMAD R7, R0, 16, RZ */
    for (u32 q = 0; q < 4; q++) {
        w = fix(0x388); /* STS.128 [R7 + 1024q], R8 + 4q: four sets of rows */
        set(&w, 24, 8, 7);
        set(&w, 32, 8, 8 + 4 * q);
        set(&w, 40, 24, 1024 * q);
        set(&w, 73, 3, 6);
        put(code, &n, w);
    }
    put(code, &n, fix(0xb1d)); /* BAR.SYNC 0 */
    for (u32 step = 0; step < SH_HMMA_BENCH_STEPS; step++) {
        for (u32 m = 0; m < 4; m++) { /* A0 R24, A1 R28 (LDSM.4); B0-B1 R32, B2-B3 R36 (LDSM.T.4) */
            w = fix(0x83b);
            set(&w, 16, 8, 24 + 4 * m);
            set(&w, 24, 8, 7);
            set(&w, 40, 24, 1024 * ((step + m) % 4));
            set(&w, 72, 2, 2);
            set(&w, 78, 1, m >= 2);
            put(code, &n, w);
        }
        for (u32 i = 0; i < 2; i++)
            for (u32 j = 0; j < 4; j++) { /* HMMA.16816.F32 R40 + 4(4i + j), A_i, B_j, the same or RZ */
                u32 d = 40 + 4 * (4 * i + j);
                w = fix(0x23c);
                set(&w, 16, 8, d);
                set(&w, 24, 8, 24 + 4 * i);
                set(&w, 32, 8, 32 + 2 * j);
                set(&w, 64, 8, step ? d : 255);
                set(&w, 72, 19, 0x18);
                put(code, &n, w);
            }
    }
    put(code, &n, rr(0x021, 72, 40, 41)); /* FADD R72, R40, R41, then R72 plus each of R42 to R71 */
    for (u32 r = 42; r < 72; r++) put(code, &n, rr(0x021, 72, 72, r));
    put(code, &n, ri(0x024, 6, 2, 4, 255)); /* IMAD R6, R2, 4, RZ */
    w = fix(0x986);                         /* STG.E [R6.U32 + UR6], R72 */
    set(&w, 24, 8, 6);
    set(&w, 32, 8, 72);
    set(&w, 64, 6, 6);
    set(&w, 72, 1, 1);
    set(&w, 73, 3, 4);
    set(&w, 84, 3, 1);
    set(&w, 91, 1, 1);
    put(code, &n, w);
    w = fix(0x94d); /* EXIT */
    set(&w, 87, 3, 7);
    put(code, &n, w);
    w = fix(0x947); /* BRA to itself, which ends a program */
    set(&w, 87, 3, 7);
    set(&w, 34, 48, (u64)-4);
    put(code, &n, w);
    return 16 * n;
}

void shader_hmma_bench_inputs(u32* in) {
    u32 seed = 0x2545f491u;
    for (u32 k = 0; k < SH_HMMA_BENCH_INPUT; k++) {
        u32 h[2];
        for (u32 j = 0; j < 2; j++) {
            seed = seed * 1664525u + 1013904223u;
            h[j] = (seed >> 31) << 15 | (14u + (seed >> 20 & 1)) << 10 | (seed >> 8 & 0x3ffu);
        }
        in[k] = h[0] | h[1] << 16;
    }
}

int shader_hmma_bench_pick(const double* us, int rule) {
    int best = 0;
    for (int w = SH_HMMA_GATHER; w < SH_HMMA_WAYS; w++)
        if (us[w] > 0 && (!best || us[w] < us[best])) best = w;
    if (!best) return rule;
    if (rule > SH_HMMA_AUTO && rule < SH_HMMA_WAYS && us[rule] > 0 &&
        us[best] * (1.0 + SH_HMMA_PICK_MARGIN / 100.0) >= us[rule])
        return rule;
    return best;
}

size_t shader_hmma_bench_format(char* out, size_t n, const char* key, int way, const double* us, const char* note) {
    if (way <= SH_HMMA_AUTO || way >= SH_HMMA_WAYS) return 0;
    size_t at = 0;
    int w = snprintf(out, n,
                     "# hmma_mode=0 (Auto): the way the HMMA bench measured fastest on this GPU and driver, in "
                     "microseconds a dispatch (0: not measured); delete the file to measure again\nkey %s\nway %s\nus",
                     key, g_shader_hmma_names[way]);
    if (w < 0 || (size_t)w >= n) return 0;
    at = (size_t)w;
    for (int k = SH_HMMA_GATHER; k < SH_HMMA_WAYS; k++) {
        w = snprintf(out + at, n - at, " %s=%.1f", g_shader_hmma_names[k], us[k] > 0 ? us[k] : 0.0);
        if (w < 0 || (size_t)w >= n - at) return 0;
        at += (size_t)w;
    }
    w = snprintf(out + at, n - at, "\nnote %s\n", note && *note ? note : "-");
    if (w < 0 || (size_t)w >= n - at) return 0;
    return at + (size_t)w;
}

/* the rest of the line that starts with word and a space, its length in *len (a CR before the line's end left out, as
   an editor on Windows may write one); NULL where no line does */
static const char* line_of(const char* text, const char* word, size_t* len) {
    size_t wl = strlen(word);
    for (const char* p = text; p && *p;) {
        const char* end = strchr(p, '\n');
        size_t l = end ? (size_t)(end - p) : strlen(p);
        if (l && p[l - 1] == '\r') l--;
        if (l > wl && !strncmp(p, word, wl) && p[wl] == ' ') {
            *len = l - wl - 1;
            return p + wl + 1;
        }
        p = end ? end + 1 : NULL;
    }
    return NULL;
}

int shader_hmma_bench_parse(const char* text, const char* key, char* why, size_t n) {
    size_t len;
    const char* have = line_of(text, "key", &len);
    if (!have) {
        snprintf(why, n, "it has no key line");
        return SH_HMMA_AUTO;
    }
    if (len != strlen(key) || strncmp(have, key, len)) {
        snprintf(why, n, "its key is %.*s, and this GPU and driver's %s", (int)len, have, key);
        return SH_HMMA_AUTO;
    }
    const char* way = line_of(text, "way", &len);
    if (!way) {
        snprintf(why, n, "it has no way line");
        return SH_HMMA_AUTO;
    }
    for (int w = SH_HMMA_GATHER; w < SH_HMMA_WAYS; w++)
        if (len == strlen(g_shader_hmma_names[w]) && !strncmp(way, g_shader_hmma_names[w], len)) return w;
    snprintf(why, n, "its way %.*s is none of gather, pairs, staged, astat, astat-staged", (int)len, way);
    return SH_HMMA_AUTO;
}
