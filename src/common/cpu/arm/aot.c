/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/cpu/arm/aot.h"
#include "common/cpu/recompiler/mach/native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct SnxArmAot {
    SnxNativeArm* native;
    size_t made; /* the blocks since the last objects */
    SnxArmAotCount count;
};

/* ---- the count ---- */

void snx_arm_aot_add(SnxArmAotCount* c, const SnxArmAotCount* from) { c->native += from->native; }
void snx_arm_aot_report(const SnxArmAotCount* c, FILE* out) {
    if (c->native && out) fprintf(out, "%s back end: %zu of %zu blocks\n", snx_native_name(), c->native, c->native);
}

/* ---- the maker ---- */

SnxArmAot* snx_arm_aot_new(char* error, size_t error_size) {
    if (!snx_native_arm_available()) {
        if (!snx_native_floor(error, error_size))
            snprintf(error, error_size, "this host's native back end (%s) makes no AArch32 blocks", snx_native_name());
        return NULL;
    }
    SnxArmAot* a = calloc(1, sizeof *a);
    if (a && !(a->native = snx_native_arm_new())) {
        free(a);
        a = NULL;
    }
    if (!a) snprintf(error, error_size, "out of memory for the %s back end's block compiler", snx_native_name());
    return a;
}
void snx_arm_aot_free(SnxArmAot* a) {
    if (!a) return;
    snx_native_arm_free(a->native);
    free(a);
}
const SnxArmAotCount* snx_arm_aot_count(const SnxArmAot* a) { return &a->count; }

int snx_arm_aot_block(SnxArmAot* a, const char* name, const unsigned char* code, size_t count, uint32_t pc, int thumb,
                      const SnxArmCodegenOptions* options, size_t* consumed, char* error, size_t error_size) {
    *consumed = 0;
    if (snx_native_arm_options(a->native, options)) {
        snprintf(error, error_size, "the %s block at 0x%08x: options no block compiler takes", thumb ? "Thumb" : "ARM",
                 (unsigned)pc);
        return -1;
    }
    if (!(thumb ? snx_native_thumb_block(a->native, name, code, count, pc, consumed, NULL, NULL)
                : snx_native_arm_block(a->native, name, code, count, pc, consumed, NULL, NULL))) {
        snprintf(error, error_size, "the %s back end made no %s block at 0x%08x: %s", snx_native_name(),
                 thumb ? "Thumb" : "ARM", (unsigned)pc, snx_native_arm_why(a->native));
        *consumed = 0;
        return -1;
    }
    if (!*consumed || *consumed > count) {
        snprintf(error, error_size, "%s made the %s block at 0x%08x of %zu instructions, of %zu given", snx_native_name(),
                 thumb ? "Thumb" : "ARM", (unsigned)pc, *consumed, count);
        return -1;
    }
    ++a->made;
    ++a->count.native;
    return 0;
}

int snx_arm_aot_objects(SnxArmAot* a, SnxLinkInput* out, char* error, size_t error_size) {
    unsigned char* object = NULL;
    *out = (SnxLinkInput){0};
    if (!a->made) return 0;
    if (snx_native_arm_object(a->native, &object, &out->size, NULL)) {
        snprintf(error, error_size, "%s back end: out of memory for the object of %zu blocks", snx_native_name(),
                 a->made);
        return -1;
    }
    out->data = object;
    out->name = "native blocks";
    a->made = 0;
    return 1;
}
