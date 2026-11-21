/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/platform/wait_screen.h"
#include <stdio.h>

int snx_closing_pass(SnxClosing* w, u64 now_ms, int closes, int window_shown) {
    const u64 spent = now_ms > w->began_ms ? now_ms - w->began_ms : 0;
    w->shown = window_shown && spent >= SNX_WAIT_CLOSING_MS;
    if (closes <= 0 || w->stopped_ms) return 0;
    w->stopped_ms = now_ms > w->began_ms ? now_ms : w->began_ms + 1; /* never 0, which is none */
    return 1;
}
void snx_closing_step(const SnxClosing* w, u64 total, u64 packed, SnxPrepareStep* st) {
    const u64 done = packed < total ? packed : total;
    *st = (SnxPrepareStep){0};
    st->label = "SAVING SHADERS";
    st->value = (u32)(done >> 10);
    st->total = (u32)(total >> 10);
    st->kib = 1;
    st->started_ms = w->began_ms;
    st->done = total && done == total;
    snprintf(st->detail, sizeof st->detail, "%.1f of %.1f MiB", (double)done / 1048576.0, (double)total / 1048576.0);
}
void snx_closing_said(const SnxClosing* w, u64 total, u64 kept, char* out, size_t size) {
    const u64 at = w->stopped_ms > w->began_ms ? w->stopped_ms - w->began_ms : 0,
              lost = kept < total ? total - kept : 0;
    snprintf(out, size,
             "a second close stopped the shader cache's save %.1f s in: %.1f of %.1f MiB of this session's "
             "shaders not saved",
             (double)at / 1000.0, (double)lost / 1048576.0, (double)total / 1048576.0);
}
