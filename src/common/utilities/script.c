/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/script.h"
#include "common/utilities/ascii.h"
#include <stdio.h>
#include <stdlib.h>

/* the press at *s into *p, and *s past it and its comma: 0 when it is not ms:hex (or Nf:hex with frames), its buttons
   within buttons */
static int read_press(const char** s, uint64_t buttons, int frames, SnxPress* p) {
    const char* c = *s;
    char* end;
    if (!snx_is_digit((unsigned char)*c)) return 0;
    unsigned long at = strtoul(c, &end, 10);
    c = end;
    *p = (SnxPress){0};
    if (frames && *c == 'f') {
        p->frame = 1;
        c++;
    }
    if (*c++ != ':' || at > UINT32_MAX || !snx_is_hex((unsigned char)*c)) return 0;
    unsigned long long mask = strtoull(c, &end, 16);
    c = end;
    if ((*c && *c != ',') || (mask & ~buttons)) return 0;
    p->at = (uint32_t)at;
    p->mask = (uint64_t)mask;
    *s = *c ? c + 1 : c;
    return 1;
}

size_t snx_script_read(const char* text, uint64_t buttons, int frames, SnxPress** presses, const char** stop) {
    size_t most = 1, count = 0;
    const char* c = text ? text : "";
    *presses = NULL;
    *stop = c;
    if (!*c) return 0;
    for (const char* p = c; *p; ++p) most += *p == ',';
    if (!(*presses = (SnxPress*)calloc(most, sizeof **presses))) return 0;
    while (*c && count < most && read_press(&c, buttons, frames, &(*presses)[count])) ++count;
    *stop = c;
    return count;
}

size_t snx_script_parse(const char* program, const char* text, uint64_t buttons, SnxPress** presses) {
    const char* stop;
    size_t count = snx_script_read(text, buttons, 0, presses, &stop);
    if (text && *text && !*presses)
        fprintf(stderr, "%s: out of memory for the script's presses\n", program);
    else if (*stop)
        fprintf(stderr, "%s: the script stops after %zu presses, at \"%s\": not ms:hex with buttons in 0x%llX\n",
                program, count, stop, (unsigned long long)buttons);
    return count;
}

uint64_t snx_script_held(const SnxPress* presses, size_t count, double ms, uint32_t length) {
    uint64_t held = 0;
    for (size_t i = 0; i < count; ++i)
        if (!presses[i].frame && ms >= presses[i].at && ms - presses[i].at < length) held |= presses[i].mask;
    return held;
}
