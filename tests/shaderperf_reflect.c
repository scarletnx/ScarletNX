/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* shaderperf's reflection (tools/shaderperf/reflect.c) of a module whose spec constant has an id past the module's id
   bound: the first pass leaves such an id out, and the second read its table at the id as given, far past it. The
   module is the header, an entry point and that constant; it reflects with no spec constant, and nothing past the
   table is read */
#include "reflect.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    static const uint32_t module[] = {
        0x07230203,    0x00010300, 0,           4,          0, /* the magic, SPIR-V 1.3, an id bound of 4 */
        5u << 16 | 15, 5,          1,           0x6e69616d, 0, /* OpEntryPoint GLCompute %1 "main" */
        4u << 16 | 50, 2,          0x7ffffff0u, 7};            /* OpSpecConstant %2 %0x7ffffff0 7 */
    SrModule m;
    char error[256] = "";
    memset(&m, 0, sizeof m);
    bool ok = sr_reflect(module, sizeof module, "main", &m, error, sizeof error);
    if (!ok) fprintf(stderr, "shaderperf_reflect: %s\n", error);
    assert(ok && m.spec_count == 0 && m.id_bound == 4);
    sr_free(&m);
    puts("shaderperf_reflect: ok");
    return 0;
}
