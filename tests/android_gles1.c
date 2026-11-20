/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* GLES 1.1's texture names (src/android/bridge/hle_gles.c): an app binds any name it likes, one glGenTextures never
   gave it too, and the table of them is indexed by the name. A name past the table's room is refused with
   GL_OUT_OF_MEMORY and the texture before it stays bound; the largest names, where the name plus the table's slack
   passed 32 bits, among them. The calls go through the HLE's handlers with the arguments in a CPU state, as a
   guest's do, on a context with no surface: nothing here draws */
#include "android/bridge/hle.h"
#include "android/bridge/hle_gles.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { GL_TEXTURE_2D = 0x0de1, GL_NO_ERROR = 0, GL_OUT_OF_MEMORY = 0x0505 };

static uint64_t gl(const char* name, uint64_t a, uint64_t b) {
    CPU cpu = {0};
    const HleSymbol* s = hle_gles;
    while (s->name[0] && strcmp(s->name, name)) ++s;
    if (!s->name[0]) {
        fprintf(stderr, "android_gles1: no function %s\n", name);
        exit(1);
    }
    cpu.x[0] = a, cpu.x[1] = b;
    s->handler(&cpu, s->data);
    return cpu.x[0];
}

int main(void) {
    static const uint32_t past[] = {0xffffffffu, 0xffffffc0u, 0xffffffbfu, 0x7fffffffu, 1u << 20};
    uint32_t made[2] = {0, 0};
    GlesContext* context = gles_context_new();
    assert(context);
    gles_make_current(context, NULL);
    /* a name never generated, past the names made so far: bound, and the table grows to it */
    gl("glBindTexture", GL_TEXTURE_2D, 70);
    assert(gl("glGetError", 0, 0) == GL_NO_ERROR);
    for (size_t i = 0; i < sizeof past / sizeof *past; ++i) {
        gl("glBindTexture", GL_TEXTURE_2D, past[i]);
        assert(gl("glGetError", 0, 0) == GL_OUT_OF_MEMORY);
    }
    /* the table is as it was: the lowest free names are still given, and bind */
    gl("glGenTextures", 2, (uint64_t)(uintptr_t)made);
    assert(made[0] == 1 && made[1] == 2 && gl("glGetError", 0, 0) == GL_NO_ERROR);
    gl("glBindTexture", GL_TEXTURE_2D, made[1]);
    assert(gl("glGetError", 0, 0) == GL_NO_ERROR);
    gles_make_current(NULL, NULL);
    gles_context_free(context);
    return 0;
}
