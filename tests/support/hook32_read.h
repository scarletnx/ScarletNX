/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 32-bit SDKs the runtime's declarations for a 32-bit game were read in (nn/hook.h's HOOK_A32_READ_SDKS), each a
   group of its own: 0.12.16, as every version under 1.0.0, and 4.4.0 alone. A family's test checks every entry of a
   32-bit game's table (hookgen --a32-marked, included before this) holds for one of them, and counts each group's */
#ifndef SNX_TEST_HOOK32_READ_H
#define SNX_TEST_HOOK32_READ_H
/* entry i's group: 1 for the first SDK read, 2 for the second, 0 for any other versions */
static inline int hook32_read_group(int i) {
    if (hook32_key[i] != HOOK_SDK) return 0;
    if (hook32_lo[i] == 0 && hook32_hi[i] == 0x10000) return 1;
    if (hook32_lo[i] == 0x40400 && hook32_hi[i] == 0x40401) return 2;
    return 0;
}
/* how many entries are of group g */
static inline int hook32_read_count(int g) {
    int n = 0;
    for (int i = 0; i < HOOK32_COUNT; i++) n += hook32_read_group(i) == g;
    return n;
}
/* the entry of symbol in group g, -1 for none */
static inline int hook32_read_entry(const char* symbol, int g) {
    for (int i = 0; i < HOOK32_COUNT; i++)
        if (!strcmp(hook32_symbol[i], symbol) && hook32_read_group(i) == g) return i;
    return -1;
}
#endif
