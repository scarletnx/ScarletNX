/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* shader_cast's cache of the bitcasts between 32-bit types (translate.h) on the emitter alone: what it gives back
   within a block, and that it gives nothing of an earlier block, whose values need not be available in a later one.
   No program is translated: the translator's own values never outlive their block, so no program shows the second */
#include "nx/gpu/shader/translate.h"
#include <stdio.h>

/* 1 for a check that fails, which it names */
static u32 failed(int pass, const char* what) {
    if (!pass) fprintf(stderr, "FAIL: %s\n", what);
    return !pass;
}
int main(void) {
    DirectCtx* c = calloc(1, sizeof *c);
    u32 failures = 0;
    if (!c) return 2;
    spv_emit_init(&c->emit, 64);
    c->u32_type = spv_emit_type_u32(&c->emit);
    c->f32_type = spv_emit_type_f32(&c->emit);
    u32 i32_type = spv_emit_type_i32(&c->emit);
    u32 value = spv_emit_id(&c->emit); /* stands for a u32 value that two blocks could both read */
    spv_emit_label(&c->emit, spv_emit_id(&c->emit));
    failures +=
        failed(shader_cast(c, c->u32_type, value, c->u32_type) == value, "a value cast to its own type is the value");
    u32 as_float = shader_cast(c, c->f32_type, value, c->u32_type);
    failures += failed(as_float != value, "a value cast to another type is a bitcast");
    failures +=
        failed(shader_cast(c, c->f32_type, value, c->u32_type) == as_float, "a value is cast to a type once a block");
    failures += failed(shader_cast(c, c->u32_type, as_float, c->f32_type) == value,
                       "a bitcast cast back is the value it was made of");
    u32 as_int = shader_cast(c, i32_type, as_float, c->f32_type);
    failures += failed(as_int != value && as_int != as_float,
                       "a bitcast cast to a third type is a bitcast, not the first value");
    u32 half = spv_emit_constant_u32(&c->emit, c->u32_type, 0x3f000000u);
    failures +=
        failed(shader_cast(c, c->f32_type, half, c->u32_type) == spv_emit_constant_f32(&c->emit, c->f32_type, 0.5f),
               "a constant cast to a float is the float constant of its bits");
    spv_emit_label(&c->emit, spv_emit_id(&c->emit));
    failures += failed(shader_cast(c, c->f32_type, value, c->u32_type) != as_float,
                       "a value cast in an earlier block is cast again in this one");
    failures += failed(shader_cast(c, c->u32_type, as_float, c->f32_type) != value,
                       "an earlier block's bitcast cast back is a bitcast in this one");
    spv_emit_destroy(&c->emit);
    free(c);
    printf("shader cast cache: %u of 8 checks failed\n", failures);
    return failures != 0;
}
