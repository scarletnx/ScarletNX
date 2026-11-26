/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 the ScarletNX contributors */
/* The recompiler library's header from C++, the way an embedder includes it: with <atomic> after it, and the CPU's
   atomic fields read with the __atomic builtins */
#include "common/cpu/recompiler/snx_recompiler.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <functional>
#include <thread>

static void cpp_miss(CPU* cpu, u64) {
    cpu->x[0] = 17;
}

static void cpp_svc(CPU*, u32) {}
static void cpp_undef(CPU* cpu, u64, u32) { cpu->pc = 0; }
static void cpp_brk(CPU* cpu, u64, u32) { cpu->pc = 0; }

static u64 run_module(struct snx_recompiled* module, RtAPI* rt, u8* memory, u64 entry) {
    CPU cpu{};
    cpu.mem = memory;
    cpu.rt = rt;
    cpu.x[30] = 0;
    cpu.x[31] = 0x1ff0;
    if (snx_recompiled_bind(module, &cpu) != 0) return ~u64{};
    snx_recompiled_run(module, &cpu, entry, 0);
    return cpu.x[0];
}

static bool test_concurrent_updates() {
    constexpr u64 entry = 0x1000;
    constexpr u64 target = entry + 0x40;
    constexpr size_t image_size = static_cast<size_t>(target + 12 - entry);
    constexpr u32 entry_words[] = {0xd2820808, 0xd61f0100}; /* mov x8,#target; br x8 */
    constexpr std::array<u32, 2> version_a{0xd2800020, 0x91002800}; /* mov x0,#1; add x0,x0,#10 */
    constexpr std::array<u32, 2> version_b{0xd2800040, 0x91005000}; /* mov x0,#2; add x0,x0,#20 */
    std::array<u8, 0x2000> memory{};
    auto place = [&](u64 address, const u32* words, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            for (unsigned byte = 0; byte < 4; ++byte) {
                memory[address + i * 4 + byte] = static_cast<u8>(words[i] >> (byte * 8));
            }
        }
    };
    place(entry, entry_words, std::size(entry_words));
    place(target, version_a.data(), version_a.size());
    const u32 ret = 0xd65f03c0;
    place(target + 8, &ret, 1);

    struct snx_recompiler* recompiler = snx_recompiler_new(SNX_RECOMPILER_FLAG_NATIVE);
    if (!recompiler) return false;
    struct snx_recompiled* module = snx_recompile(recompiler, entry, memory.data() + entry, image_size);
    if (!module) {
        snx_recompiler_free(recompiler);
        return false;
    }
    RtAPI rt{};
    snx_runtime_defaults(&rt);
    rt.svc = cpp_svc;
    rt.undef = cpp_undef;
    rt.brk = cpp_brk;

    std::atomic<unsigned> ready{};
    std::atomic<bool> start{};
    std::atomic<bool> updates_ok{true};
    auto update = [&](const std::array<u32, 2>& version) {
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (unsigned i = 0; i < 4000; ++i) {
            if (snx_recompiled_update_code(module, target, version.data(), sizeof(version)) != sizeof(version)) {
                updates_ok.store(false, std::memory_order_relaxed);
                return;
            }
        }
    };
    std::thread first(update, std::cref(version_a));
    std::thread second(update, std::cref(version_b));
    while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    start.store(true, std::memory_order_release);
    first.join();
    second.join();

    snx_recompiled_invalidate(module, target, sizeof(version_a), nullptr, nullptr);
    const u64 result = run_module(module, &rt, memory.data(), entry);
    const bool valid = updates_ok.load(std::memory_order_relaxed) && (result == 11 || result == 22);
    snx_recompiled_free(module);
    snx_recompiler_free(recompiler);
    return valid;
}

int main() {
    CPU cpu{};
    __atomic_store_n(&cpu.poll, 1u, __ATOMIC_RELAXED);
    const std::atomic<unsigned> poll{__atomic_load_n(&cpu.poll, __ATOMIC_RELAXED)};
    if (poll.load() != 1 || snx_recompiler_abi() != SNX_ABI) {
        std::fprintf(stderr, "recompiler library (C++): the header and the library disagree\n");
        return 1;
    }
    RtAPI rt{};
    GuestFn dispatch[1]{};
    rt.miss = cpp_miss;
    cpu.rt = &rt;
    cpu.disp = dispatch;
    cpu.code_lo = 4;
    cpu.code_hi = 8;
    CPU* c = &cpu;
    const u64 target = 4;
    CALLDYN(target);
    if (cpu.x[0] != 17) {
        std::fprintf(stderr, "recompiler library (C++): CALLDYN did not use the miss handler\n");
        return 1;
    }
    if (!test_concurrent_updates()) {
        std::fprintf(stderr, "recompiler library (C++): concurrent code updates produced a mixed image\n");
        return 1;
    }
    std::puts("recompiler library (C++): ok");
    return 0;
}
