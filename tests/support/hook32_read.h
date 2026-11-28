/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 32-bit SDKs the runtime's declarations for a 32-bit game were read in (nn/hook.h's HOOK_A32_READ_SDKS), and the
   versions around them each family's declarations hold for by inference. A family's test checks the entries of a
   32-bit game's table (hookgen --a32-marked, included before this) hold for the versions its .hook says and for none
   past them */
#ifndef SNX_TEST_HOOK32_READ_H
#define SNX_TEST_HOOK32_READ_H
#define HOOK32_SDK_0  0x000c10u /* 0.12.16 */
#define HOOK32_SDK_4  0x040400u /* 4.4.0 */
#define HOOK32_SDK_10 0x0a0600u /* 10.6.0 */
#define HOOK32_SDK_11 0x0b0000u /* 11.0.0, past the newest read */
/* does entry i hold for the SDK version v */
static inline int hook32_holds(int i, u32 v) {
    return hook32_key[i] == HOOK_SDK && hook32_lo[i] <= v && v < hook32_hi[i];
}
/* how many entries hold for v */
static inline int hook32_count_at(u32 v) {
    int n = 0;
    for (int i = 0; i < HOOK32_COUNT; i++) n += hook32_holds(i, v);
    return n;
}
/* the entry of symbol that holds for v, -1 for none */
static inline int hook32_entry_at(const char* symbol, u32 v) {
    for (int i = 0; i < HOOK32_COUNT; i++)
        if (!strcmp(hook32_symbol[i], symbol) && hook32_holds(i, v)) return i;
    return -1;
}
/* does every entry hold for exactly the versions below hi (the families read in all three SDKs) */
static inline int hook32_all_below(u32 hi) {
    for (int i = 0; i < HOOK32_COUNT; i++)
        if (hook32_key[i] != HOOK_SDK || hook32_lo[i] != 0 || hook32_hi[i] != hi) return 0;
    return 1;
}
#endif
