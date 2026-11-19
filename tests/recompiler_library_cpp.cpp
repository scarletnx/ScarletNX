/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 the ScarletNX contributors */
/* The recompiler library's header from C++, the way an embedder includes it: with <atomic> after it, and the CPU's
   atomic fields read with the __atomic builtins */
#include "common/cpu/recompiler/snx_recompiler.h"
#include <atomic>
#include <cstdio>

int main() {
    CPU cpu{};
    __atomic_store_n(&cpu.poll, 1u, __ATOMIC_RELAXED);
    const std::atomic<unsigned> poll{__atomic_load_n(&cpu.poll, __ATOMIC_RELAXED)};
    if (poll.load() != 1 || snx_recompiler_abi() != SNX_ABI) {
        std::fprintf(stderr, "recompiler library (C++): the header and the library disagree\n");
        return 1;
    }
    std::puts("recompiler library (C++): ok");
    return 0;
}
