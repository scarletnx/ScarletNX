/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Android's ICU, libicuuc.so and libicui18n.so, as the host's own ICU (hle.h). They are not NDK libraries: an app
   reaches them by dlopen and dlsym, as .NET's System.Globalization.Native does, which looks for u_strlen with each
   version's suffix (u_strlen_76) and then takes every function by the suffix it found. The host's ICU is loaded once,
   and each name of its version is its own function (the name without the suffix where the host's ICU was built
   without them, as FreeBSD's and macOS's are), called with the guest's registers: the guest's pointers are the
   host's, and ICU's C API takes and returns integers, pointers and UDate (a double), which hle_call_wide passes.
   Windows passes the arguments by position, which would need each function's signature, so there it has no ICU */
#include "android/bridge/hle.h"
#include "android/bridge/hle_impl.h"
#include "android/bridge/bridge.h"
#include "common/platform/dl.h"
#include "common/utilities/common.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the versions looked for, newest first: Android 7's ICU is 56; the hosts' today are 7x */
enum { ICU_NEWEST = 99, ICU_OLDEST = 50 };

/* how a function's integer arguments and result differ from a plain int or pointer */
typedef struct {
    const char* name; /* without the version's suffix */
    char kind;        /* 'I', or 'D' for a UDate result */
    uint8_t bools;    /* bit i: the i'th integer argument is a UBool, which an x86-64 callee takes extended to 32
                         bits and an AArch64 caller need not extend */
    bool bool_result; /* a UBool result: the low byte of the register, which a callee need not extend either */
} IcuSignature;
static const IcuSignature signatures[] = {
    {"ucal_getNow", 'D', 0, false},
    {"ucal_getMillis", 'D', 0, false},
    {"ucal_getGregorianChange", 'D', 0, false},
    {"udat_parse", 'D', 0, false},
    {"udat_get2DigitYearStart", 'D', 0, false},
    {"unum_parseDouble", 'D', 0, false},
    {"unum_getDoubleAttribute", 'D', 0, false},
    {"ucal_getKeywordValuesForLocale", 'I', 1 << 2, false},
    {"udat_toPattern", 'I', 1 << 1, false},
    {"udat_applyPattern", 'I', 1 << 1, false},
    {"unum_toPattern", 'I', 1 << 1, false},
    {"unum_applyPattern", 'I', 1 << 1, false},
    {"unorm2_isNormalized", 'I', 0, true},
    {"ucal_inDaylightTime", 'I', 0, true},
    {"ucal_isWeekend", 'I', 0, true},
    {"ucal_equivalentTo", 'I', 0, true},
    {"ucal_getTimeZoneTransitionDate", 'I', 0, true},
};

typedef struct IcuCall {
    struct IcuCall* next;
    HleSymbol symbol; /* the name with its suffix, as the guest asked for it; call with this */
    void* host;
    IcuSignature signature;
} IcuCall;

static struct {
    pthread_once_t once;
    void* uc;   /* the host's libicuuc */
    void* i18n; /* and libicui18n */
    int version;
    bool suffixed; /* whether the host's names have the version's suffix */
    pthread_mutex_t lock;
    IcuCall* calls;
} icu = {.once = PTHREAD_ONCE_INIT, .lock = PTHREAD_MUTEX_INITIALIZER};

#ifndef _WIN32
/* a library of the host's ICU: the name a development package links, else each version's */
static void* open_host(const char* stem) {
    char path[64];
#ifdef __APPLE__
    snprintf(path, sizeof path, "%s.dylib", stem);
#else
    snprintf(path, sizeof path, "%s.so", stem);
#endif
    void* library = plat_dl_open(path);
    for (int v = ICU_NEWEST; !library && v >= ICU_OLDEST; --v) {
#ifdef __APPLE__
        snprintf(path, sizeof path, "%s.%d.dylib", stem, v);
#else
        snprintf(path, sizeof path, "%s.so.%d", stem, v);
#endif
        library = plat_dl_open(path);
    }
    return library;
}
#endif

static void load(void) {
#ifndef _WIN32
    icu.uc = open_host("libicuuc");
    icu.i18n = icu.uc ? open_host("libicui18n") : NULL;
    for (int v = ICU_NEWEST; icu.i18n && !icu.version && v >= ICU_OLDEST; --v) {
        char name[32];
        snprintf(name, sizeof name, "u_strlen_%d", v);
        if (plat_dl_sym(icu.uc, name)) icu.version = v;
    }
    icu.suffixed = icu.version != 0;
    void (*get_version)(uint8_t[4]) = icu.i18n && !icu.version ? (void (*)(uint8_t[4]))plat_dl_sym(icu.uc, "u_getVersion") : NULL;
    if (get_version) {
        uint8_t v[4] = {0};
        get_version(v);
        if (v[0] >= ICU_OLDEST && v[0] <= ICU_NEWEST) icu.version = v[0];
    }
    if (!icu.version) {
        /* none, or one whose version is not known: the guest finds no ICU */
        if (icu.i18n) plat_dl_close(icu.i18n);
        if (icu.uc) plat_dl_close(icu.uc);
        icu.uc = icu.i18n = NULL;
    }
#endif
}

bool hle_icu_available(void) {
    pthread_once(&icu.once, load);
    return icu.version != 0;
}
int hle_icu_version(void) { return hle_icu_available() ? icu.version : 0; }

static void call(CPU* c, void* data) {
    const IcuCall* f = data;
    for (unsigned i = 0; i < 8; ++i)
        if (f->signature.bools >> i & 1) X(i) = (uint8_t)X(i);
    hle_call_wide(c, f->host, f->signature.kind, NULL);
    if (f->signature.bool_result) X(0) = (uint8_t)X(0);
}

const HleSymbol* hle_icu_find(const char* name) {
    if (!hle_icu_available()) return NULL;
    /* only the host's version's names: the guest learns the version from the one u_strlen it finds */
    char suffix[16];
    snprintf(suffix, sizeof suffix, "_%d", icu.version);
    size_t n = strlen(name), s = strlen(suffix);
    if (n <= s || strcmp(name + n - s, suffix)) return NULL;
    pthread_mutex_lock(&icu.lock);
    IcuCall* f = icu.calls;
    while (f && strcmp(f->symbol.name, name)) f = f->next;
    if (!f) {
        char* host_name = icu.suffixed ? snx_format("%s", name) : snx_format("%.*s", (int)(n - s), name);
        void* host = plat_dl_sym(icu.uc, host_name);
        if (!host) host = plat_dl_sym(icu.i18n, host_name);
        free(host_name);
        if (host) {
            f = snx_alloc(sizeof *f);
            f->symbol = (HleSymbol){snx_format("%s", name), call, f};
            f->host = host;
            f->signature = (IcuSignature){NULL, 'I', 0, false};
            for (size_t i = 0; i < sizeof signatures / sizeof *signatures; ++i)
                if (strlen(signatures[i].name) == n - s && !memcmp(signatures[i].name, name, n - s))
                    f->signature = signatures[i];
            f->next = icu.calls;
            icu.calls = f;
        }
    }
    pthread_mutex_unlock(&icu.lock);
    return f ? &f->symbol : NULL;
}

uint64_t hle_icu_symbol(const char* name) {
    const HleSymbol* s = hle_icu_find(name);
    if (!s) return 0;
    char* export = snx_format("libicu::%s", name);
    uint64_t a = bridge_export(export, s->handler, s->data);
    free(export);
    return a;
}
