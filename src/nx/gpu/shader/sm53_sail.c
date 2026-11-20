/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SM53's instructions emitted from Sail: sailgen --gpu writes sm53_sail.inc from sail/sm53.sail and the families'
   sail/sm53_<family>.sail, with their back ends over translate.h's accessors (sail/sm53_spirv.h), which this file
   compiles. to_spirv hands each instruction to sm53_sail_emit before its families' emitters */
#include "nx/gpu/shader/translate.h"
#include <stdarg.h>
#include <stdio.h>

#include "sm53_sail.inc"

int sm53_sail_emit(const Tr* t, u32 i, u64 w, int op, u32 d, u32 a, u32 b) {
    SgG g;
    sg53_begin(&g, t, i, w, op, d, a, b);
    return sg53_emit(&g, op);
}

int sm53_sail_takes(int op) { return sg53_takes(op); }
