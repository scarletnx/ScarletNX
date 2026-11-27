/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* This host's native back end (native.h): the code generator whose code this machine runs */
#include "common/cpu/recompiler/mach/native.h"
#include "common/cpu/recompiler/arm64/arm64_codegen.h"
#include "common/cpu/recompiler/arm64/arm64_arm.h"
#include "common/cpu/recompiler/la64/la64_arm.h"
#include "common/cpu/recompiler/la64/la64_codegen.h"
#include "common/cpu/recompiler/mips64/mips64_arm.h"
#include "common/cpu/recompiler/mips64/mips64be_arm.h"
#include "common/cpu/recompiler/mips64/mips64be_codegen.h"
#include "common/cpu/recompiler/ppc64/ppc64be_arm.h"
#include "common/cpu/recompiler/ppc64/ppc64be_codegen.h"
#include "common/cpu/recompiler/s390x/s390x_arm.h"
#include "common/cpu/recompiler/s390x/s390x_codegen.h"
#include "common/cpu/recompiler/mips64/mips64_codegen.h"
#include "common/cpu/recompiler/ppc64/ppc64_arm.h"
#include "common/cpu/recompiler/ppc64/ppc64_codegen.h"
#include "common/cpu/recompiler/rv64/rv64_arm.h"
#include "common/cpu/recompiler/rv64/rv64_codegen.h"
#include "common/cpu/recompiler/x64/x64_arm.h"
#include "common/cpu/recompiler/x64/x64_cache.h"
#include "common/cpu/recompiler/x64/x64_codegen.h"
#include "common/cpu/recompiler/x64/x64_elf.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int snx_native_available(void) {
    return snx_x64_available() || snx_arm64_available() || snx_rv64_available() || snx_ppc64_available() ||
           snx_mips64_available() || snx_la64_available() || snx_ppc64be_available() || snx_mips64be_available() ||
           snx_s390x_available();
}

const char* snx_native_name(void) {
    return snx_x64_available()     ? "x86-64"
           : snx_arm64_available() ? "arm64"
           : snx_rv64_available()  ? "riscv64"
           : snx_ppc64_available() ? "ppc64le"
           : snx_mips64_available() ? "mips64el"
           : snx_la64_available()   ? "loongarch64"
           : snx_ppc64be_available() ? "ppc64"
           : snx_mips64be_available() ? "mips64"
           : snx_s390x_available()    ? "s390x"
                                   : "none";
}
SnxU64 snx_native_reach(const char* triple) {
    if (triple || snx_arm64_available()) return (SnxU64)128 << 20; /* B and BL: a target's back end is arm64's */
    if (snx_ppc64_available() || snx_ppc64be_available()) return (SnxU64)32 << 20; /* b and bl */
    return (SnxU64)2 << 30;
}
unsigned snx_native_features(void) {
    return snx_x64_available() ? snx_x64_features() : snx_arm64_available() ? snx_arm64_features() : 0;
}

/* ---- the hosts below a back end's floor ---- */

/* text after out's first *at bytes, *at past it: what doesn't fit is cut */
static void append(char* out, size_t size, size_t* at, const char* text) {
    if (*at < size) snprintf(out + *at, size - *at, "%s", text);
    *at += strlen(text);
}
/* the names of need's features in bits, "A, B and C" */
static void append_features(const SnxNativeFeature* need, size_t count, unsigned bits, char* out, size_t size,
                            size_t* at) {
    size_t left = 0, k;
    for (k = 0; k < count; ++k) left += (need[k].bit & bits) != 0;
    for (k = 0; k < count; ++k) {
        if (!(need[k].bit & bits)) continue;
        append(out, size, at, need[k].name);
        --left;
        if (left) append(out, size, at, left > 1 ? ", " : " and ");
    }
}
int snx_native_lacks(const SnxNativeFeature* need, size_t count, unsigned has, char* why, size_t size) {
    unsigned all = 0;
    size_t at = 0, k;
    if (size) why[0] = 0;
    for (k = 0; k < count; ++k) all |= need[k].bit;
    if (!(all & ~has)) return 0;
    append(why, size, &at, "cpu: needs ");
    append_features(need, count, all & ~has, why, size, &at);
    append(why, size, &at, ", this host has ");
    if (all & has)
        append_features(need, count, all & has, why, size, &at);
    else
        append(why, size, &at, "none");
    return 1;
}
int snx_native_done(const SnxNativeFeature* done, size_t count, unsigned has, char* why, size_t size) {
    unsigned all = 0;
    size_t at = 0, k;
    if (size) why[0] = 0;
    for (k = 0; k < count; ++k) all |= done[k].bit;
    if (!(all & ~has)) return 0;
    append(why, size, &at, "cpu: this host has no ");
    append_features(done, count, all & ~has, why, size, &at);
    append(why, size, &at, ", done by the fallback");
    return 1;
}
int snx_native_floor(char* why, size_t size) { return snx_x64_floor(why, size) || snx_ppc64be_floor(why, size); }
int snx_native_fallback(char* why, size_t size) { return snx_x64_fallback(why, size); }
int snx_native_refused(char* error, size_t size) {
    return !snx_native_available() && snx_native_floor(error, size);
}

int snx_native_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                      unsigned char* made, unsigned char** object, size_t* size, char* error, size_t error_size) {
    if (snx_x64_available())
        return snx_x64_object(codegen, first, last, options, made, object, size, error, error_size);
    if (snx_arm64_available())
        return snx_arm64_object(codegen, first, last, options, snx_arm64_features(), made, object, size, error,
                                error_size);
    if (snx_rv64_available())
        return snx_rv64_object(codegen, first, last, options, made, object, size, error, error_size);
    if (snx_ppc64_available())
        return snx_ppc64_object(codegen, first, last, options, made, object, size, error, error_size);
    if (snx_mips64_available())
        return snx_mips64_object(codegen, first, last, options, made, object, size, error, error_size);
    if (snx_la64_available())
        return snx_la64_object(codegen, first, last, options, made, object, size, error, error_size);
    if (snx_ppc64be_available())
        return snx_ppc64be_object(codegen, first, last, options, made, object, size, error, error_size);
    if (snx_mips64be_available())
        return snx_mips64be_object(codegen, first, last, options, made, object, size, error, error_size);
    if (snx_s390x_available())
        return snx_s390x_object(codegen, first, last, options, made, object, size, error, error_size);
    memset(made, 0, last - first);
    *object = NULL;
    *size = 0;
    return 0;
}

int snx_native_cached_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                             unsigned char* made, unsigned char** object, size_t* size, int* reused, char* error,
                             size_t error_size) {
    if (snx_x64_available())
        return snx_x64_cached_object(codegen, first, last, options, made, object, size, reused, error, error_size);
    *reused = 0;
    return snx_native_object(codegen, first, last, options, made, object, size, error, error_size);
}

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
int snx_native_target(const char* triple) {
#if !defined(_WIN32) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    size_t n = strlen(triple);
    return !strncmp(triple, "arm64-apple-ios", 15) && n >= 4 && !strcmp(triple + n - 4, "-elf");
#else
    (void)triple;
    return 0;
#endif
}

int snx_native_target_object(const char* triple, SnxCodegen* codegen, size_t first, size_t last,
                             const SnxNativeOptions* options, unsigned char* made, unsigned char** object, size_t* size,
                             char* error, size_t error_size) {
    unsigned features = 0; /* the architecture's baseline: the device's CPU is not known here */
    if (!snx_native_target(triple)) {
        snprintf(error, error_size, "no native back end of this build makes the target %s", triple);
        return -1;
    }
#if defined(__APPLE__) && TARGET_OS_IOS && defined(__aarch64__)
    features = snx_arm64_features(); /* the pack is made where it runs */
#endif
    return snx_arm64_object(codegen, first, last, options, features, made, object, size, error, error_size);
}

/* the AArch32 blocks' compiler: the host's (x86-64's, arm64's or riscv64's) */
struct SnxNativeArm {
    SnxX64Arm* x64;
    SnxArm64Arm* arm64;
    SnxRv64Arm* rv64;
    SnxPpc64Arm* ppc64;
    SnxMips64Arm* mips64;
    SnxLa64Arm* la64;
    SnxPpc64beArm* ppc64be;
    SnxMips64beArm* mips64be;
    SnxS390xArm* s390x;
};
int snx_native_arm_available(void) {
    return snx_x64_arm_available() || snx_arm64_arm_available() || snx_rv64_arm_available() ||
           snx_ppc64_arm_available() || snx_mips64_arm_available() ||
           snx_la64_arm_available() || snx_ppc64be_arm_available() || snx_mips64be_arm_available() ||
           snx_s390x_arm_available();
}
SnxNativeArm* snx_native_arm_new(void) {
    SnxNativeArm* c;
    int ok;
    if (!snx_native_arm_available() || !(c = (SnxNativeArm*)calloc(1, sizeof *c))) return NULL;
    if (snx_x64_arm_available()) ok = !!(c->x64 = snx_x64_arm_new());
    else if (snx_arm64_arm_available())
        ok = !!(c->arm64 = snx_arm64_arm_new());
    else if (snx_rv64_arm_available())
        ok = !!(c->rv64 = snx_rv64_arm_new());
    else if (snx_ppc64_arm_available())
        ok = !!(c->ppc64 = snx_ppc64_arm_new());
    else if (snx_mips64_arm_available())
        ok = !!(c->mips64 = snx_mips64_arm_new());
    else if (snx_la64_arm_available())
        ok = !!(c->la64 = snx_la64_arm_new());
    else if (snx_ppc64be_arm_available())
        ok = !!(c->ppc64be = snx_ppc64be_arm_new());
    else if (snx_mips64be_arm_available())
        ok = !!(c->mips64be = snx_mips64be_arm_new());
    else
        ok = !!(c->s390x = snx_s390x_arm_new());
    if (!ok) {
        free(c);
        return NULL;
    }
    return c;
}
void snx_native_arm_free(SnxNativeArm* c) {
    if (!c) return;
    snx_x64_arm_free(c->x64);
    snx_arm64_arm_free(c->arm64);
    snx_rv64_arm_free(c->rv64);
    snx_ppc64_arm_free(c->ppc64);
    snx_mips64_arm_free(c->mips64);
    snx_la64_arm_free(c->la64);
    snx_ppc64be_arm_free(c->ppc64be);
    snx_mips64be_arm_free(c->mips64be);
    snx_s390x_arm_free(c->s390x);
    free(c);
}
int snx_native_arm_options(SnxNativeArm* c, const SnxArmCodegenOptions* options) {
    return c->x64        ? snx_x64_arm_options(c->x64, options)
           : c->arm64    ? snx_arm64_arm_options(c->arm64, options)
           : c->rv64     ? snx_rv64_arm_options(c->rv64, options)
           : c->ppc64    ? snx_ppc64_arm_options(c->ppc64, options)
           : c->mips64   ? snx_mips64_arm_options(c->mips64, options)
           : c->la64     ? snx_la64_arm_options(c->la64, options)
           : c->ppc64be  ? snx_ppc64be_arm_options(c->ppc64be, options)
           : c->mips64be ? snx_mips64be_arm_options(c->mips64be, options)
                         : snx_s390x_arm_options(c->s390x, options);
}
int snx_native_arm_block(SnxNativeArm* c, const char* name, const unsigned char* code, size_t count, uint32_t pc,
                         size_t* consumed, SnxNativeArmNext next, void* user) {
    return c->x64     ? snx_x64_arm_block(c->x64, name, code, count, pc, consumed, next, user)
           : c->arm64 ? snx_arm64_arm_block(c->arm64, name, code, count, pc, consumed, next, user)
           : c->rv64  ? snx_rv64_arm_block(c->rv64, name, code, count, pc, consumed, next, user)
           : c->ppc64 ? snx_ppc64_arm_block(c->ppc64, name, code, count, pc, consumed, next, user)
           : c->mips64 ? snx_mips64_arm_block(c->mips64, name, code, count, pc, consumed, next, user)
           : c->la64    ? snx_la64_arm_block(c->la64, name, code, count, pc, consumed, next, user)
           : c->ppc64be ? snx_ppc64be_arm_block(c->ppc64be, name, code, count, pc, consumed, next, user)
           : c->mips64be ? snx_mips64be_arm_block(c->mips64be, name, code, count, pc, consumed, next, user)
                         : snx_s390x_arm_block(c->s390x, name, code, count, pc, consumed, next, user);
}
int snx_native_thumb_block(SnxNativeArm* c, const char* name, const unsigned char* code, size_t count, uint32_t pc,
                           size_t* consumed, SnxNativeArmNext next, void* user) {
    return c->x64        ? snx_x64_arm_thumb_block(c->x64, name, code, count, pc, consumed, next, user)
           : c->arm64    ? snx_arm64_arm_thumb_block(c->arm64, name, code, count, pc, consumed, next, user)
           : c->rv64     ? snx_rv64_arm_thumb_block(c->rv64, name, code, count, pc, consumed, next, user)
           : c->ppc64    ? snx_ppc64_arm_thumb_block(c->ppc64, name, code, count, pc, consumed, next, user)
           : c->mips64   ? snx_mips64_arm_thumb_block(c->mips64, name, code, count, pc, consumed, next, user)
           : c->la64     ? snx_la64_arm_thumb_block(c->la64, name, code, count, pc, consumed, next, user)
           : c->ppc64be  ? snx_ppc64be_arm_thumb_block(c->ppc64be, name, code, count, pc, consumed, next, user)
           : c->mips64be ? snx_mips64be_arm_thumb_block(c->mips64be, name, code, count, pc, consumed, next, user)
                         : snx_s390x_arm_thumb_block(c->s390x, name, code, count, pc, consumed, next, user);
}
const char* snx_native_arm_why(const SnxNativeArm* c) {
    return c->x64     ? snx_x64_arm_why(c->x64)
           : c->arm64 ? snx_arm64_arm_why(c->arm64)
           : c->rv64  ? snx_rv64_arm_why(c->rv64)
           : c->ppc64 ? snx_ppc64_arm_why(c->ppc64)
           : c->mips64 ? snx_mips64_arm_why(c->mips64)
           : c->la64    ? snx_la64_arm_why(c->la64)
           : c->ppc64be ? snx_ppc64be_arm_why(c->ppc64be)
           : c->mips64be ? snx_mips64be_arm_why(c->mips64be)
                         : snx_s390x_arm_why(c->s390x);
}
int snx_native_arm_object(SnxNativeArm* c, unsigned char** object, size_t* size, size_t* blocks) {
    return c->x64     ? snx_x64_arm_object(c->x64, object, size, blocks)
           : c->arm64 ? snx_arm64_arm_object(c->arm64, object, size, blocks)
           : c->rv64  ? snx_rv64_arm_object(c->rv64, object, size, blocks)
           : c->ppc64 ? snx_ppc64_arm_object(c->ppc64, object, size, blocks)
           : c->mips64 ? snx_mips64_arm_object(c->mips64, object, size, blocks)
           : c->la64    ? snx_la64_arm_object(c->la64, object, size, blocks)
           : c->ppc64be ? snx_ppc64be_arm_object(c->ppc64be, object, size, blocks)
           : c->mips64be ? snx_mips64be_arm_object(c->mips64be, object, size, blocks)
                         : snx_s390x_arm_object(c->s390x, object, size, blocks);
}
int snx_native_arm_absorb(SnxNativeArm* c, SnxNativeArm* from) {
    return c->x64     ? snx_x64_arm_absorb(c->x64, from->x64)
           : c->arm64 ? snx_arm64_arm_absorb(c->arm64, from->arm64)
           : c->rv64  ? snx_rv64_arm_absorb(c->rv64, from->rv64)
           : c->ppc64 ? snx_ppc64_arm_absorb(c->ppc64, from->ppc64)
           : c->mips64 ? snx_mips64_arm_absorb(c->mips64, from->mips64)
           : c->la64    ? snx_la64_arm_absorb(c->la64, from->la64)
           : c->ppc64be ? snx_ppc64be_arm_absorb(c->ppc64be, from->ppc64be)
           : c->mips64be ? snx_mips64be_arm_absorb(c->mips64be, from->mips64be)
                         : snx_s390x_arm_absorb(c->s390x, from->s390x);
}
int snx_native_arm_entry(SnxNativeArm* c, uint32_t key) {
    return c->x64 ? snx_x64_arm_entry(c->x64, key)
           : c->arm64 ? snx_arm64_arm_entry(c->arm64, key)
           : c->rv64  ? snx_rv64_arm_entry(c->rv64, key)
           : c->ppc64 ? snx_ppc64_arm_entry(c->ppc64, key)
           : c->mips64 ? snx_mips64_arm_entry(c->mips64, key)
           : c->la64    ? snx_la64_arm_entry(c->la64, key)
           : c->ppc64be ? snx_ppc64be_arm_entry(c->ppc64be, key)
           : c->mips64be ? snx_mips64be_arm_entry(c->mips64be, key)
                         : snx_s390x_arm_entry(c->s390x, key);
}

/* the module descriptors' functions of the RISC machines (mach/risc_desc.inc, in their code generators) */
#if !defined(_WIN32)
extern const SnxNativeDesc snx_rv64_desc, snx_ppc64_desc, snx_ppc64be_desc, snx_mips64_desc, snx_mips64be_desc,
    snx_la64_desc, snx_s390x_desc;
static const SnxNativeDesc* const g_descs[] = {&snx_rv64_desc,   &snx_ppc64_desc, &snx_ppc64be_desc, &snx_mips64_desc,
                                               &snx_mips64be_desc, &snx_la64_desc, &snx_s390x_desc};
const SnxNativeDesc* snx_native_desc(void) {
    if (snx_x64_available() || snx_arm64_available()) return NULL;
    return snx_native_desc_named(snx_native_name());
}
const SnxNativeDesc* snx_native_desc_named(const char* name) {
    for (size_t k = 0; k < sizeof g_descs / sizeof *g_descs; ++k)
        if (!strcmp(g_descs[k]->name, name)) return g_descs[k];
    return NULL;
}
#else
const SnxNativeDesc* snx_native_desc(void) { return NULL; }
const SnxNativeDesc* snx_native_desc_named(const char* name) {
    (void)name;
    return NULL;
}
#endif

/* <elf.h>'s, where the system has it */
enum { NATIVE_EM_AARCH64 = 183, NATIVE_R_X86_64_64 = 1, NATIVE_R_AARCH64_ABS64 = 257 };
SnxXElf* snx_native_data(unsigned* pointer) {
    const SnxNativeDesc* d = snx_native_desc();
    SnxXElf* o = snx_native_available() ? snx_xelf_new() : NULL;
    if (!o) return NULL;
    if (d) {
        snx_xelf_set_machine(o, d->em);
        snx_xelf_set_flags(o, d->elf_flags);
        snx_xelf_set_big(o, d->big);
        *pointer = d->abs64;
    } else if (snx_x64_available()) {
        if (snx_x64_win64()) snx_xelf_set_coff(o, 1);
        *pointer = NATIVE_R_X86_64_64; /* a COFF object's too */
    } else {
        snx_xelf_set_machine(o, NATIVE_EM_AARCH64);
        if (snx_arm64_windows()) snx_xelf_set_coff(o, 1);
        *pointer = NATIVE_R_AARCH64_ABS64; /* a COFF object's too */
    }
    return o;
}
