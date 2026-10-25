/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Versioned hooks (tests/hook/versions.hook): the table hookgen generates (a symbol's entries by version, a hook split
   at the bounds of the struct variants it uses), the version an SDK label in a module's bytes gives, the entry
   hook_select picks for a module's version and the layout its adapter hands the handler, and the field accessors */
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "hle/hook/hooks.hook.inc"
#include <assert.h>
#include <stdio.h>

static const void* seen_layout;
static const char* seen_handler;

static void saw(const char* handler, const void* layout) {
    seen_handler = handler;
    seen_layout = layout;
}
u64 test_any(CPU* c, const HookLayout_Plain* Plain, u64 p) {
    (void)c;
    saw("test_any", Plain);
    return p + 1;
}
void test_exact(CPU* c) {
    (void)c;
    saw("test_exact", NULL);
}
void test_gap(CPU* c) {
    (void)c;
    saw("test_gap", NULL);
}
u32 os_lock_old(CPU* c, const HookLayout_nn_os_MutexType* nn_os_MutexType, u64 m) {
    (void)c, (void)m;
    saw("os_lock_old", nn_os_MutexType);
    return 1;
}
u32 os_lock(CPU* c, const HookLayout_nn_os_MutexType* nn_os_MutexType, u64 m) {
    /* a handler reads the guest's struct through its layout */
    saw("os_lock", nn_os_MutexType);
    return hook_get_u32(c, nn_os_MutexType->owner, m);
}
void test_nvn(CPU* c, const HookLayout_Plain* Plain, u64 p) {
    (void)c, (void)p;
    saw("test_nvn", Plain);
}
void os_unlock(CPU* c, const HookLayout_nn_os_MutexType* nn_os_MutexType, u64 m) {
    (void)c, (void)m;
    saw("os_unlock", nn_os_MutexType);
}

/* a module's read-only data: the labels among other strings, as an SDK-built module has them */
static size_t module(u8* buf, size_t cap, const char* labels, size_t k) {
    static const char before[] = "assert\0SDK MW+\0SDK MW+Vendor\0SDK MW+Vendor+Vendor_nnSdk-9_9_9\0";
    size_t n = sizeof before - 1;
    assert(n + k + 8 <= cap);
    memcpy(buf, before, n);
    memcpy(buf + n, labels, k);
    memset(buf + n + k, 0, 8);
    return n + k + 8;
}

static u32 sdk_label(const char* labels, size_t k) {
    u8 buf[512];
    size_t n = module(buf, sizeof buf, labels, k);
    return hook_label_version(buf, n, "nnSdk");
}
/* the labels a string literal holds, NULs between them */
#define sdk_of(labels) sdk_label(labels, sizeof labels - 1)

/* the entry hook_select picks for symbol, its handler run through the entry's adapter */
static int pick(const char* symbol, u32 sdk, u32 nvn) {
    static u8 mem[0x40];
    static CPU c;
    u32 versions[HOOK_KEYS] = {0};
    int first, k;
    versions[HOOK_SDK] = sdk;
    versions[HOOK_NVN] = nvn;
    k = hook_select(&hook_table, symbol, versions, &first);
    if (k >= 0) {
        assert(first >= 0 && first <= k && !strcmp(hook_symbol[first], symbol));
        seen_handler = NULL;
        seen_layout = NULL;
        c.mem = mem;
        c.x[0] = 0;
        c.x[30] = 0x81000000;
        hook_fn[k](&c, 0);
        assert(c.pc == 0x81000000 && !strcmp(seen_handler, hook_handler[k]));
    }
    return k;
}

int main(void) {
    static u8 mem[0x100];
    static CPU c;
    char text[64];
    int i, k, first;
    u32 none[HOOK_KEYS] = {0};

    /* the table: by symbol, then by version; lock's first range split at the struct's 9.0.0 */
    assert(HOOK_COUNT == 10 && HOOK_FAMILIES == 2);
    for (i = 1; i < HOOK_COUNT; i++) {
        int order = strcmp(hook_symbol[i - 1], hook_symbol[i]);
        assert(order < 0 || (order == 0 && hook_key[i - 1] == hook_key[i] && hook_hi[i - 1] <= hook_lo[i]));
    }
    assert(hook_table.count == HOOK_COUNT && hook_table.symbol == hook_symbol);

    /* the layouts: the fields in another order the second time, each at its own offset */
    assert(hook_layout_nn_os_MutexType[0].size == 0x10 && hook_layout_nn_os_MutexType[0].state.offset == 0 &&
           hook_layout_nn_os_MutexType[0].owner.offset == 4);
    assert(hook_layout_nn_os_MutexType[1].size == 0x20 && hook_layout_nn_os_MutexType[1].state.offset == 0 &&
           hook_layout_nn_os_MutexType[1].owner.offset == 8);
    assert(hook_layout_Plain[0].size == 0x10 && hook_layout_Plain[0].a.offset == 0 &&
           hook_layout_Plain[0].b.offset == 8 && hook_layout_Plain[0].on.offset == 10 &&
           hook_layout_Plain[0].f.offset == 12);

    /* labels: the first whole one for the library asked; a vendor part not ending in SDK, a truncated version or a
       part past 255 isn't one */
    assert(sdk_of("SDK MW+Vendor+VendorSdk_nnSdk-12_3_9-Release") == 0x0C0309);
    assert(sdk_of("SDK MW+Vendor+VendorSDK_nnSdk-20_5_22-Release") == 0x140516);
    assert(sdk_of("SDK MW+Vendor+VendorSDK_NVN-20_5_22-Release") == 0);
    assert(sdk_of("SDK MW+Vendor+Vendor_nnSdk-1_2_3\0SDK MW+Vendor+VendorSdk_nnSdk-8_1_0") == 0x080100);
    assert(sdk_of("SDK MW+Vendor+VendorSdk_nnSdk-12_3") == 0);
    assert(sdk_of("SDK MW+Vendor+VendorSdk_nnSdk-256_0_0") == 0);
    assert(sdk_of("SDK MW+Vendor+VendorSdk_nnSdk-0_12_16-Release") == 0x000C10);
    assert(sdk_of("") == 0);
    {
        u8 buf[128];
        static const char nvn[] = "SDK MW+Vendor+VendorSDK_NVN-13_3_13-Release";
        size_t n = module(buf, sizeof buf, nvn, sizeof nvn - 1);
        assert(hook_label_version(buf, n, "NVN") == 0x0D030D);
        /* a label cut off at the end of the bytes */
        assert(hook_label_version((const u8*)"SDK MW+Vendor+VendorSdk_nnSdk-12_3_9", 30, "nnSdk") == 0);
    }

    /* the entry for a module's version, and the layout variant its adapter passes */
    k = pick("lock", sdk_of("SDK MW+Vendor+VendorSdk_nnSdk-12_3_9-Release"), 0);
    assert(k >= 0 && !strcmp(hook_handler[k], "os_lock") && seen_layout == &hook_layout_nn_os_MutexType[1]);
    k = pick("lock", sdk_of("SDK MW+Vendor+VendorSdk_nnSdk-9_0_0-Release"), 0);
    assert(k >= 0 && !strcmp(hook_handler[k], "os_lock_old") && seen_layout == &hook_layout_nn_os_MutexType[1]);
    k = pick("lock", sdk_of("SDK MW+Vendor+VendorSdk_nnSdk-8_1_0-Release"), 0);
    assert(k >= 0 && !strcmp(hook_handler[k], "os_lock_old") && seen_layout == &hook_layout_nn_os_MutexType[0]);
    hook_range_text(text, sizeof text, hook_key[k], hook_lo[k], hook_hi[k]);
    assert(!strcmp(text, "sdk >= 5.0.0, < 9.0.0"));
    assert(!strcmp(hook_layouts[k], "nn::os::MutexType of line 7"));
    /* below every range, or a module without a label: not installed */
    assert(pick("lock", 0x040000, 0) == -2);
    assert(hook_select(&hook_table, "lock", none, &first) == -2 && first >= 0 && !strcmp(hook_symbol[first], "lock"));
    /* unversioned, but its struct isn't: a variant per version, none for an unknown one */
    assert(pick("unlock", 0x010000, 0) >= 0 && seen_layout == &hook_layout_nn_os_MutexType[0]);
    assert(pick("unlock", 0x140516, 0) >= 0 && seen_layout == &hook_layout_nn_os_MutexType[1]);
    assert(pick("unlock", 0, 0) == -2);
    /* unversioned with an unversioned struct: every module */
    k = pick("any", 0, 0);
    assert(k >= 0 && seen_layout == &hook_layout_Plain[0] && !strcmp(hook_layouts[k], "Plain of line 23"));
    hook_range_text(text, sizeof text, hook_key[k], hook_lo[k], hook_hi[k]);
    assert(!strcmp(text, "any version"));
    /* ==, a gap between two ranges, and the bounds of > and <= */
    k = pick("exact", 0x140516, 0);
    assert(k >= 0);
    hook_range_text(text, sizeof text, hook_key[k], hook_lo[k], hook_hi[k]);
    assert(!strcmp(text, "sdk == 20.5.22"));
    assert(pick("exact", 0x140517, 0) == -2);
    assert(pick("gap", 0x010000, 0) == -2 && pick("gap", 0x010001, 0) >= 0 && pick("gap", 0x030000, 0) >= 0);
    assert(pick("gap", 0x030001, 0) == -2 && pick("gap", 0x040000, 0) == -2 && pick("gap", 0x050000, 0) >= 0);
    /* the NVN key: unknown (0) until the runtime reads the API version */
    assert(pick("nvn_only", 0x140516, 0) == -2);
    k = pick("nvn_only", 0, 55u << 16);
    assert(k >= 0 && seen_layout == &hook_layout_Plain[0]);
    hook_range_text(text, sizeof text, hook_key[k], hook_lo[k], hook_hi[k]);
    assert(!strcmp(text, "nvn >= 55.0"));
    /* a symbol no entry names */
    assert(hook_select(&hook_table, "missing", none, &first) == -1 && first == -1);
    assert(hook_select(&hook_table, "zzz", none, &first) == -1 && hook_select(&hook_table, "a", none, &first) == -1);
    hook_version_text(text, sizeof text, HOOK_SDK, 0x140516);
    assert(!strcmp(text, "20.5.22"));
    hook_version_text(text, sizeof text, HOOK_SDK, 0);
    assert(!strcmp(text, "unknown"));

    /* the accessors: guest bytes, little-endian, at the layout's offsets */
    c.mem = mem;
    memset(mem, 0, sizeof mem);
    mem[0x48] = 0x78, mem[0x49] = 0x56, mem[0x4A] = 0x34, mem[0x4B] = 0x12;
    k = pick("lock", 0x0C0000, 0);
    c.x[0] = 0x40;
    c.x[30] = 0x81000000;
    hook_fn[k](&c, 0);
    assert(c.x[0] == 0x12345678);
    hook_put_u8(&c, hook_layout_nn_os_MutexType[1].state, 0x40, 0xAB);
    assert(mem[0x40] == 0xAB && hook_get_u8(&c, hook_layout_nn_os_MutexType[1].state, 0x40) == 0xAB);
    hook_put_i16(&c, hook_layout_Plain[0].b, 0x80, -2);
    assert(mem[0x88] == 0xFE && mem[0x89] == 0xFF && hook_get_i16(&c, hook_layout_Plain[0].b, 0x80) == -2);
    hook_put_bool(&c, hook_layout_Plain[0].on, 0x80, 7);
    assert(mem[0x8A] == 1 && hook_get_bool(&c, hook_layout_Plain[0].on, 0x80));
    hook_put_f32(&c, hook_layout_Plain[0].f, 0x80, 1.5f);
    assert(mem[0x8F] == 0x3F && mem[0x8E] == 0xC0 && hook_get_f32(&c, hook_layout_Plain[0].f, 0x80) == 1.5f);
    hook_put_u64(&c, hook_layout_Plain[0].a, 0x80, 0x0102030405060708ull);
    assert(mem[0x80] == 8 && mem[0x87] == 1 && hook_get_u64(&c, hook_layout_Plain[0].a, 0x80) == 0x0102030405060708ull);
    puts("hook versions: ok");
    return 0;
}
