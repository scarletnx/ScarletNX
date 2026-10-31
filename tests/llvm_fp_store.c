/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Scalar FP stores preserve exactly their lane's bytes, regardless of NaNs, alignment or other register contents.
   Run every width and source register at O0 and O2. The O0 benchmark emits a run of stores as one guest function. */
#include "llvm_steps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const u32 stores[5] = {0x3D000000, 0x7D000000, 0xBD000000, 0xFD000000, 0x3D800000};

static int mixed(char opt) {
    /* STR S1; FMOV S1,W2; STR S1; MOV V1,V2; STR S1; LDR Q1; STR S1; RET. Every vector-writing path must drop
       the previous scalar snapshot. Each instruction writes and reads the same guest-visible state at O0 and O2. */
    const u32 words[] = {0xBD000001, 0x1E270041, 0xBD000401, 0x4EA21C41,
                         0xBD000801, 0x3DC00401, 0xBD000C01, 0xD65F03C0};
    unsigned char image[sizeof words], leaders[8] = {1}, memory[64], expected[64];
    for (unsigned i = 0; i < 8; i++)
        for (unsigned b = 0; b < 4; b++) image[i * 4 + b] = (unsigned char)(words[i] >> (b * 8));
    SnxCodeImage module = {0};
    module.image = image;
    module.image_size = sizeof image;
    module.base = 0x1000;
    module.text_size = sizeof image;
    SnxFunction function = {0};
    function.start = module.base;
    function.end = module.base + sizeof image;
    function.count = 8;
    function.leaders = leaders;
    function.leader_count = 1;
    SnxCodegen codegen = {0};
    codegen.modules = &module;
    codegen.module_count = 1;
    codegen.functions = &function;
    codegen.function_count = 1;
    LlvmSteps steps;
    char error[1024];
    if (llvm_steps_begin_opt(&steps, 1, 0, opt, error, sizeof error)) return snx_error("%s", error);
    llvm_steps_add_function(&steps, &codegen, &function);
    if (llvm_steps_load(&steps, error, sizeof error)) return snx_error("%s", error);
    for (unsigned sample = 0; sample < 256; sample++) {
        CPU cpu = {0};
        for (unsigned i = 0; i < sizeof memory; i++) memory[i] = (unsigned char)(i * 13 + sample);
        memset(&cpu.v[1], (int)sample, sizeof cpu.v[1]);
        memset(&cpu.v[2], (int)(255 - sample), sizeof cpu.v[2]);
        cpu.mem = memory;
        cpu.x[0] = 3;
        cpu.x[2] = 0xAABBCCDD;
        cpu.fpsr = 0x800009Fu;
        memcpy(expected, memory, sizeof expected);
        memset(expected + 3, (int)sample, 4);
        for (unsigned b = 0; b < 4; b++) expected[7 + b] = (unsigned char)(cpu.x[2] >> (b * 8));
        memset(expected + 11, (int)(255 - sample), 4);
        memcpy(expected + 15, memory + 19, 4);
        GuestFn fn;
        memcpy(&fn, &steps.steps[0], sizeof fn);
        fn(&cpu, module.base);
        if (memcmp(memory, expected, sizeof memory) || memcmp(&cpu.v[1], memory + 19, 16) ||
            cpu.x[0] != 3 || cpu.x[2] != 0xAABBCCDD || cpu.fpsr != 0x800009Fu) {
            fprintf(stderr, "mixed scalar/vector stores differ at O%c sample %u\n", opt, sample);
            llvm_steps_end(&steps);
            return 1;
        }
    }
    llvm_steps_end(&steps);
    printf("Mixed scalar/vector updates at O%c: 256 exact cases passed\n", opt);
    return 0;
}

static int correctness(char opt) {
    char error[1024];
    LlvmSteps steps;
    if (llvm_steps_begin_opt(&steps, 1, 0, opt, error, sizeof error)) return snx_error("%s", error);
    for (unsigned size = 0; size < 5; size++)
        for (unsigned reg = 0; reg < 32; reg++) llvm_steps_add(&steps, stores[size] | reg, 0x1000, 0);
    if (llvm_steps_load(&steps, error, sizeof error)) return snx_error("%s", error);
    unsigned checks = 0;
    for (unsigned size = 0; size < 5; size++)
        for (unsigned reg = 0; reg < 32; reg++)
            for (unsigned alignment = 0; alignment < 16; alignment++)
                for (unsigned pattern = 0; pattern < 4; pattern++) {
                    unsigned char memory[96], expected[96], lane[16];
                    CPU cpu = {0};
                    for (unsigned i = 0; i < 16; i++) lane[i] = (unsigned char)(i * 73 + pattern * 127);
                    memset(memory, 0xA5, sizeof memory);
                    memcpy(expected, memory, sizeof memory);
                    /* CPU's vector bytes are in guest order, even on a big-endian host. */
                    memset(cpu.v, 0xC3, sizeof cpu.v);
                    memcpy(&cpu.v[reg], lane, sizeof lane);
                    cpu.mem = memory;
                    cpu.x[0] = 32 + alignment;
                    cpu.fpcr = pattern * 0x4100000u;
                    cpu.fpsr = 0x800009Fu;
                    CPU before = cpu;
                    memcpy(expected + cpu.x[0], lane, 1u << size);
                    u64 next = steps.steps[size * 32 + reg](&cpu);
                    if (next != 0x1004 || memcmp(memory, expected, sizeof memory) ||
                        memcmp(cpu.x, before.x, sizeof cpu.x) || memcmp(cpu.v, before.v, sizeof cpu.v) ||
                        cpu.fpcr != before.fpcr || cpu.fpsr != before.fpsr) {
                        fprintf(stderr, "FP store differs at O%c size %u reg %u align %u pattern %u\n",
                                opt, size, reg, alignment, pattern);
                        llvm_steps_end(&steps);
                        return 1;
                    }
                    checks++;
                }
    llvm_steps_end(&steps);
    printf("FP stores at O%c: %u exact byte/register cases passed\n", opt, checks);
    return 0;
}

static int benchmark(unsigned size) {
    if (size > 4) return 2;
    unsigned char image[260] = {0}, memory[2048] = {0};
    unsigned char leaders[65] = {1};
    for (unsigned i = 0; i < 65; i++) {
        u32 word = i < 64 ? stores[size] | 1 | (i << 10) : 0xD65F03C0;
        for (unsigned b = 0; b < 4; b++) image[i * 4 + b] = (unsigned char)(word >> (b * 8));
    }
    SnxCodeImage module = {0};
    module.image = image;
    module.image_size = sizeof image;
    module.base = 0x1000;
    module.text_size = sizeof image;
    SnxFunction function = {0};
    function.start = module.base;
    function.end = module.base + sizeof image;
    function.count = 65;
    function.leaders = leaders;
    function.leader_count = 1;
    SnxCodegen codegen = {0};
    codegen.modules = &module;
    codegen.module_count = 1;
    codegen.functions = &function;
    codegen.function_count = 1;
    char error[1024];
    LlvmSteps steps;
    if (llvm_steps_begin_opt(&steps, 1, 0, '0', error, sizeof error)) return snx_error("%s", error);
    llvm_steps_add_function(&steps, &codegen, &function);
    if (llvm_steps_load(&steps, error, sizeof error)) return snx_error("%s", error);
    CPU cpu = {0};
    cpu.mem = memory;
    cpu.x[0] = 3;
    memset(&cpu.v[1], 0xA7, sizeof cpu.v[1]);
    GuestFn fn;
    memcpy(&fn, &steps.steps[0], sizeof fn);
    double start = snx_seconds();
    for (unsigned i = 0; i < 1000000; i++) fn(&cpu, module.base);
    double elapsed = snx_seconds() - start;
    for (unsigned i = 0; i < (64u << size); i++)
        if (memory[3 + i] != 0xA7) return 1;
    if (memory[2] || memory[3 + (64u << size)]) return 1;
    llvm_steps_end(&steps);
    printf("{\"width_bits\":%u,\"store_count\":64000000,\"seconds\":%.9f,\"verified\":true}\n", 8u << size, elapsed);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && !strcmp(argv[1], "--bench")) return benchmark((unsigned)strtoul(argv[2], NULL, 10));
    return correctness('0') || correctness('2') || mixed('0') || mixed('2');
}
