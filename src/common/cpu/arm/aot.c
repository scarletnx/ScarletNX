/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/cpu/arm/aot.h"
#include "common/cpu/arm/llvm.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/mach/native.h"
#include "common/platform/file.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char snx_arm_aot_none[] = "no native back end on this host";
const char snx_arm_aot_kept[] = "--keep-ir keeps LLVM's IR of every block";

struct SnxArmAot {
    SnxNativeArm* native; /* NULL where every block is LLVM's */
    const char* every;    /* why, where it is */
    SnxIr* ir;            /* the context and target machine of LLVM's blocks, made for the first of them */
    LLVMModuleRef module; /* LLVM's blocks since the last objects, NULL for none */
    size_t made;          /* the native back end's since then */
    const char* keep_ir;
    char opt;
    SnxArmAotCount count;
};

/* ---- the count ---- */

static void refused(SnxArmAotCount* c, const char* why, size_t count, uint32_t lowest) {
    size_t k = 0;
    c->llvm += count;
    while (k < c->nwhys && strcmp(c->whys[k].why, why)) ++k;
    if (k == SNX_ARM_AOT_WHYS) {
        c->unlisted += count;
        return;
    }
    if (k == c->nwhys) {
        c->whys[k].why = why;
        c->whys[k].count = 0;
        c->whys[k].lowest = lowest;
        ++c->nwhys;
    }
    c->whys[k].count += count;
    if (lowest < c->whys[k].lowest) c->whys[k].lowest = lowest;
}
void snx_arm_aot_refused(SnxArmAotCount* c, const char* why, uint32_t key) { refused(c, why, 1, key); }
void snx_arm_aot_add(SnxArmAotCount* c, const SnxArmAotCount* from) {
    c->native += from->native;
    for (size_t k = 0; k < from->nwhys; ++k) refused(c, from->whys[k].why, from->whys[k].count, from->whys[k].lowest);
    c->unlisted += from->unlisted;
    c->llvm += from->unlisted;
}
void snx_arm_aot_report(const SnxArmAotCount* c, const char* program, int quiet) {
    const size_t blocks = c->native + c->llvm;
    if (!blocks) return;
    /* every block LLVM's for the one reason that refuses none */
    const int asked = c->nwhys == 1 && !c->native &&
                      (!strcmp(c->whys[0].why, snx_arm_aot_none) || !strcmp(c->whys[0].why, snx_arm_aot_kept));
    if (!quiet) {
        if (asked)
            printf("LLVM: %zu blocks (%s)\n", blocks, c->whys[0].why);
        else
            printf("%s back end: %zu of %zu blocks%s\n", snx_native_name(), c->native, blocks,
                   c->llvm ? ", LLVM the rest" : "");
    }
    if (asked) return;
    for (size_t k = 0; k < c->nwhys; ++k)
        fprintf(stderr, "%s: %zu block%s made by LLVM, the first the %s one at 0x%08x: %s\n", program, c->whys[k].count,
                c->whys[k].count == 1 ? "" : "s", c->whys[k].lowest & 1 ? "Thumb" : "ARM",
                (unsigned)(c->whys[k].lowest & ~1u), c->whys[k].why);
    if (c->unlisted)
        fprintf(stderr, "%s: %zu more blocks made by LLVM, for reasons past the first %d\n", program, c->unlisted,
                SNX_ARM_AOT_WHYS);
}

/* ---- the maker ---- */

SnxArmAot* snx_arm_aot_new(char opt, const char* keep_ir) {
    SnxArmAot* a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->opt = opt;
    a->keep_ir = keep_ir;
    if (keep_ir)
        a->every = snx_arm_aot_kept;
    else if (!snx_native_arm_available())
        a->every = snx_arm_aot_none;
    else if (!(a->native = snx_native_arm_new())) {
        free(a);
        return NULL;
    }
    return a;
}
void snx_arm_aot_free(SnxArmAot* a) {
    if (!a) return;
    if (a->module) LLVMDisposeModule(a->module);
    snx_ir_free(a->ir);
    snx_native_arm_free(a->native);
    free(a);
}
const SnxArmAotCount* snx_arm_aot_count(const SnxArmAot* a) { return &a->count; }

/* the module LLVM's next blocks go in */
static LLVMModuleRef llvm_module(SnxArmAot* a, char* error, size_t error_size) {
    if (a->module) return a->module;
    if (!a->ir) {
        if (snx_ir_init(error, error_size)) return NULL;
        if (!(a->ir = snx_ir_new("arm-aot", a->opt, error, error_size))) return NULL;
    }
    a->module = LLVMModuleCreateWithNameInContext("arm-aot", a->ir->ctx);
    LLVMSetTarget(a->module, LLVMGetTarget(a->ir->mod));
    LLVMSetDataLayout(a->module, LLVMGetDataLayoutStr(a->ir->mod));
    return a->module;
}
static int llvm_block(SnxArmAot* a, const char* name, const unsigned char* code, size_t count, uint32_t pc, int thumb,
                      const SnxArmCodegenOptions* options, size_t* consumed, char* error, size_t error_size) {
    LLVMModuleRef module = llvm_module(a, error, error_size);
    if (!module) return -1;
    /* it reads host integers */
    void* units = malloc((count ? count : 1) * (thumb ? 2 : 4));
    if (!units) {
        snprintf(error, error_size, "out of memory for a block of %zu instructions", count);
        return -1;
    }
    for (size_t i = 0; i < count; ++i)
        if (thumb)
            ((uint16_t*)units)[i] = snx_ld_le16(code + i * 2);
        else
            ((uint32_t*)units)[i] = snx_ld_le32(code + i * 4);
    LLVMValueRef fn = thumb ? snx_thumb_llvm_block_ex(module, name, units, count, pc, consumed, options)
                            : snx_arm_llvm_block_ex(module, name, units, count, pc, consumed, options);
    free(units);
    if (!fn) {
        snprintf(error, error_size, "the block compiler refused the %s block at 0x%08x, of %zu instructions at most",
                 thumb ? "Thumb" : "ARM", (unsigned)pc, count);
        return -1;
    }
    LLVMSetVisibility(fn, LLVMHiddenVisibility);
    return 0;
}
/* a block's length, which the caller goes on from */
static int covers(const char* by, size_t consumed, size_t count, uint32_t pc, int thumb, char* error,
                  size_t error_size) {
    if (consumed && consumed <= count) return 0;
    snprintf(error, error_size, "%s made the %s block at 0x%08x of %zu instructions, of %zu given", by,
             thumb ? "Thumb" : "ARM", (unsigned)pc, consumed, count);
    return -1;
}
int snx_arm_aot_block(SnxArmAot* a, const char* name, const unsigned char* code, size_t count, uint32_t pc, int thumb,
                      const SnxArmCodegenOptions* options, size_t* consumed, char* error, size_t error_size) {
    const char* why = a->every;
    *consumed = 0;
    if (a->native) {
        if (snx_native_arm_options(a->native, options)) {
            snprintf(error, error_size, "options no block compiler takes");
            return -1;
        }
        if (thumb ? snx_native_thumb_block(a->native, name, code, count, pc, consumed, NULL, NULL)
                  : snx_native_arm_block(a->native, name, code, count, pc, consumed, NULL, NULL)) {
            if (covers(snx_native_name(), *consumed, count, pc, thumb, error, error_size)) return -1;
            ++a->made;
            ++a->count.native;
            return 0;
        }
        why = snx_native_arm_why(a->native);
    }
    if (llvm_block(a, name, code, count, pc, thumb, options, consumed, error, error_size) ||
        covers("LLVM", *consumed, count, pc, thumb, error, error_size))
        return -1;
    snx_arm_aot_refused(&a->count, why, pc | (thumb ? 1u : 0u));
    return 0;
}

/* the module's IR as the block compiler made it, before LLVM's passes, as keep_ir/id.ll */
static int save_ir(const SnxArmAot* a, const char* id, char* error, size_t error_size) {
    char *text = LLVMPrintModuleToString(a->module), *file = snx_format("%s.ll", id);
    char* path = file ? snx_path_join(a->keep_ir, file) : NULL;
    int rc = !path || snx_write_file(path, text, strlen(text)) ? -1 : 0;
    if (rc) {
        char why[256];
        snprintf(error, error_size, "cannot save the IR of %s in %s: %s", id, a->keep_ir,
                 snx_os_error(why, sizeof why));
    }
    LLVMDisposeMessage(text);
    free(file);
    free(path);
    return rc;
}
int snx_arm_aot_objects(SnxArmAot* a, const char* id, SnxLinkInput out[2], char* error, size_t error_size) {
    int n = 0;
    out[0] = out[1] = (SnxLinkInput){0};
    if (a->made) {
        unsigned char* object = NULL;
        if (snx_native_arm_object(a->native, &object, &out[0].size, NULL)) {
            snprintf(error, error_size, "%s back end: out of memory for the object of %zu blocks", snx_native_name(),
                     a->made);
            return -1;
        }
        out[0].data = object;
        out[0].name = "native blocks";
        a->made = 0;
        n = 1;
    }
    if (a->module) {
        unsigned char* object = NULL;
        size_t size = 0;
        int rc = a->keep_ir ? save_ir(a, id, error, error_size) : 0;
        if (!rc) rc = snx_ir_module_object(a->ir, a->module, a->opt, &object, &size, error, error_size);
        LLVMDisposeModule(a->module);
        a->module = NULL;
        if (rc) {
            free((void*)out[0].data);
            out[0] = (SnxLinkInput){0};
            return -1;
        }
        out[n].data = object;
        out[n].size = size;
        out[n++].name = "blocks";
    }
    return n;
}
