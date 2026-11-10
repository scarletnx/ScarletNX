/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The quit key asks twice in every console's window (the Switch's hotkey_quit, the 3DS', the Vita's and the GBA's
   Escape): a first press warns, and a second within SNX_QUIT_AGAIN_NS of it quits, so a stray press cannot end the
   game */
#ifndef SNX_QUIT_KEY_H
#define SNX_QUIT_KEY_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define SNX_QUIT_AGAIN_NS 3000000000ull
/* a press at now_ns, on a clock in nanoseconds: 1 when it quits; else 0, and *by (0 before any press) is when a second
   must come by */
static inline int snx_quit_press(uint64_t* by, uint64_t now_ns) {
    if (now_ns < *by) return 1;
    *by = now_ns + SNX_QUIT_AGAIN_NS;
    return 0;
}
/* a first press's warning, with the key as the window names it, into out (n bytes) */
static inline void snx_quit_warning(char* out, size_t n, const char* key) {
    snprintf(out, n, "Press %s again within %llu seconds to quit", key,
             (unsigned long long)(SNX_QUIT_AGAIN_NS / 1000000000ull));
}
#endif
