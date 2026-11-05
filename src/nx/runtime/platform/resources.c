/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/platform/resources.h"
#include <stdio.h>
#include <string.h>

int threads_line_text(ThreadsWindow* w, const SnxThreadInfo* now, unsigned n, SnxU64 now_ns, char* line, size_t size) {
    struct {
        SnxU64 ns;
        unsigned i;
    } top[3] = {{0}};
    unsigned listed = n < THREADS_SEEN ? n : THREADS_SEEN;
    int said = 0;
    for (unsigned i = 0; i < listed; i++) {
        SnxU64 ns = now[i].cpu_ns;
        int seen = 0;
        for (unsigned k = 0; k < w->n; k++)
            if (w->seen[k].id == now[i].id) {
                if (ns >= w->seen[k].ns) ns -= w->seen[k].ns;
                seen = 1;
                break;
            }
        if (!seen && w->n == THREADS_SEEN) ns = 0; /* it may have run before the window, untold */
        for (int t = 0; t < 3; t++)
            if (ns > top[t].ns) {
                memmove(&top[t + 1], &top[t], (size_t)(2 - t) * sizeof top[0]);
                top[t].ns = ns;
                top[t].i = i;
                break;
            }
    }
    if (w->started && size) {
        int wrote =
            snprintf(line, size,
                     "threads: the most CPU in the last %.1f s, of %u threads:", (double)(now_ns - w->at_ns) / 1e9, n);
        size_t len = wrote > 0 ? (size_t)wrote : 0;
        for (int t = 0; t < 3 && top[t].ns && len < size; t++) {
            wrote = snprintf(line + len, size - len, "%s %s %.2f s", t ? "," : "",
                             now[top[t].i].name[0] ? now[top[t].i].name : "?", (double)top[t].ns / 1e9);
            len += wrote > 0 ? (size_t)wrote : 0;
        }
        if (!top[0].ns && len < size) snprintf(line + len, size - len, " none ran");
        said = 1;
    }
    for (unsigned i = 0; i < listed; i++) {
        w->seen[i].id = now[i].id;
        w->seen[i].ns = now[i].cpu_ns;
    }
    w->n = listed;
    w->at_ns = now_ns;
    w->started = 1;
    return said;
}
