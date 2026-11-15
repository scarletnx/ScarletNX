/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Whether an NPDM is an Ounce program's. The recompiler (loader.c's SnxNpdm.ounce) and the runtime (main.c's
   g_ounce) read the same NPDM through this one test, so the code the recompiler makes ahead of time and the code the
   runtime JIT makes take the same CPU features */
#ifndef SNX_NX_LOADER_OUNCE_H
#define SNX_NX_LOADER_OUNCE_H

#include "common/cpu/aarch64/a64_decoder.h"
#include "common/utilities/endian.h"
#include <stddef.h>
#include <string.h>

/* An Ounce ACID's signature and key are 0x180 bytes each, where a Switch's are 0x100, so its magic is at 0x300 in
   the ACID, not at 0x200 (Switchbrew's NPDM page). All 18 Switch games on one machine have it at 0x200, and an Ounce
   game at 0x300. A short NPDM, or one with no META, is a Switch program's */
static inline int snx_npdm_ounce(const unsigned char* data, size_t size) {
    if (!data || size < 0x80 || memcmp(data, "META", 4)) return 0;
    size_t acid = snx_ld_le32(data + 0x78), acid_size = snx_ld_le32(data + 0x7c);
    return acid < size && acid_size >= 0x304 && acid_size <= size - acid && !memcmp(data + acid + 0x300, "ACID", 4);
}

/* The CPU features of a program's code, for the recompile and the runtime JIT alike: an Ounce program's Cortex-A78C
   (a64.profile's ounce target), else the Switch's Cortex-A57, with LSE, RCpc and PAuth when the cpu_extensions
   setting is on */
static inline SnxU64 snx_guest_features(int ounce, int cpu_extensions) {
    if (ounce) return SNX_A64_TARGET_OUNCE;
    return cpu_extensions ? SNX_A64_TARGET_SWITCH_EXTENDED : 0;
}

#endif
