/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The NVN key of versioned hooks (tests/hook/nvn_versions.hook, generated with --prefix nvn as gpu/nvn/nvn.hook is):
   the API version a driver's nvnDeviceGetInteger answers, read from its code (gpu/nvn/nvn_driver.c) as nvn
   reads it at the game's first NVN call, packed as the key takes it, the entry hook_select picks for it, and the
   versions the runtime has measured layouts for */
#include "nx/gpu/nvn/nvn_driver.h"
#include "nx/hle/hook/hook.h"
#include "gpu/nvn/nvn_versions.hook.h"
#include "gpu/nvn/nvn_versions.hook.inc"
#include <assert.h>
#include <stdio.h>

static const char* seen;
i32 acquire_53(CPU* c, u64 w, u64 sync, u64 index) {
    (void)c, (void)w, (void)sync, (void)index;
    seen = "acquire_53";
    return 0;
}
i32 acquire_55(CPU* c, u64 w, u64 sync, u64 index) {
    (void)c, (void)w, (void)sync, (void)index;
    seen = "acquire_55";
    return 0;
}
void queue_finish(CPU* c, u64 q) {
    (void)c, (void)q;
    seen = "queue_finish";
}

/* a driver's nvnDeviceGetInteger as SDK drivers lay it out: a switch on the item through a byte table scaled by 4 from
   an ADR base, its cases setting w0 (items 0 and 1, the API's major and minor version), the rest returning w0 as it
   was. At 0x100, the form whose switch is in the first function it calls (SDK 12 and 20 drivers) */
static u8 image[0x200];
static void put(u32 at, u32 word) { snx_st_le32(image + at, word); }
static void driver(i32 major, i32 minor) {
    u32 i;
    memset(image, 0, sizeof image);
    put(0x00, 0x7101A03F);                   /* cmp w1, #0x68 */
    put(0x04, 0x54000168);                   /* b.hi 0x30 */
    put(0x08, 0x100001C8);                   /* adr x8, 0x40 (the table) */
    put(0x0C, 0x38616909);                   /* ldrb w9, [x8, x1] */
    put(0x10, 0x1000008A);                   /* adr x10, 0x20 (the cases) */
    put(0x14, 0x8B09094A);                   /* add x10, x10, x9, lsl #2 */
    put(0x18, 0xD61F0140);                   /* br x10 */
    put(0x20, 0x52800000 | (u32)major << 5); /* mov w0, #major */
    put(0x24, 0xD65F03C0);                   /* ret */
    put(0x28, 0x52800000 | (u32)minor << 5); /* mov w0, #minor */
    put(0x2C, 0xD65F03C0);                   /* ret */
    put(0x30, 0xD65F03C0);                   /* ret: the default and the other items */
    for (i = 0; i <= 0x68; i++) image[0x40 + i] = i == 0 ? 0 : i == 1 ? 2 : 4;
    put(0x100, 0x97FFFFC0); /* bl 0x0 */
    put(0x104, 0xD65F03C0); /* ret */
}
/* a 32-bit program's driver, in A32: nvnDeviceGetInteger calls the switch, which sets r0 to the default before it
   jumps through a table of words relative to the ADR that names it. Item 0 keeps that default, item 1 sets r0, item 2
   reads the device's state, item 3 keeps the default again */
static void driver32(u32 major, u32 minor) {
    memset(image, 0, sizeof image);
    put(0x00, 0xE92D4010);         /* push {r4, lr} */
    put(0x04, 0xEB000005);         /* bl 0x20 */
    put(0x08, 0xE8BD8010);         /* pop {r4, pc} */
    put(0x20, 0xE92D4070);         /* push {r4, r5, r6, lr} */
    put(0x24, 0xE24DD008);         /* sub sp, sp, #8 */
    put(0x28, 0xE1A05000);         /* mov r5, r0 */
    put(0x2C, 0xE3A00000);         /* mov r0, #0 */
    put(0x30, 0xEB000012);         /* bl 0x80: r0 is unknown after it */
    put(0x34, 0xE3550003);         /* cmp r5, #3 */
    put(0x38, 0x8A00000E);         /* bhi 0x78 */
    put(0x3C, 0xE1A01105);         /* lsl r1, r5, #2 */
    put(0x40, 0xE28F2008);         /* add r2, pc, #8: the table, 0x50 */
    put(0x44, 0xE3A00000 | major); /* mov r0, #major */
    put(0x48, 0xE7911002);         /* ldr r1, [r1, r2] */
    put(0x4C, 0xE081F002);         /* add pc, r1, r2 */
    put(0x50, 0x14);               /* item 0: 0x64 */
    put(0x54, 0x10);               /* item 1: 0x60 */
    put(0x58, 0x1C);               /* item 2: 0x6C */
    put(0x5C, 0x14);               /* item 3: 0x64 */
    put(0x60, 0xE3A00000 | minor); /* mov r0, #minor */
    put(0x64, 0xE28DD008);         /* add sp, sp, #8 */
    put(0x68, 0xE8BD8070);         /* pop {r4, r5, r6, pc} */
    put(0x6C, 0xE5940060);         /* ldr r0, [r4, #0x60] */
    put(0x70, 0xEAFFFFFB);         /* b 0x64 */
    put(0x78, 0xE59D0004);         /* ldr r0, [sp, #4] */
    put(0x7C, 0xEAFFFFF8);         /* b 0x64 */
    put(0x80, 0xE12FFF1E);         /* bx lr */
}

/* the API version nvn reads from the driver at func, 0 when it can't */
static u32 api(u64 func) {
    i32 major, minor;
    if (!nvn_driver_info(image, 0, sizeof image, func, 0, &major) ||
        !nvn_driver_info(image, 0, sizeof image, func, 1, &minor))
        return 0;
    return nvn_api_pack(major, minor);
}
/* the handler nvn takes for symbol at that API version (its adapter run), NULL when none holds */
static const char* pick(const char* symbol, u32 version) {
    static u8 mem[0x40];
    static CPU c;
    u32 versions[HOOK_KEYS] = {0};
    int first, k;
    versions[HOOK_NVN] = version;
    k = hook_select(&nvn_table, symbol, versions, &first);
    if (k < 0) return NULL;
    seen = NULL;
    c.mem = mem;
    c.x[30] = 0x81000000;
    nvn_fn[k](&c, 0);
    assert(c.pc == 0x81000000 && seen && !strcmp(seen, nvn_handler[k]));
    return seen;
}

int main(void) {
    char text[64];
    i32 v;
    u32 v55;
    assert(NVN_COUNT == 3);

    /* the version from the driver's switch, in nvnDeviceGetInteger or in the function it calls first */
    driver(55, 8);
    v55 = api(0x00);
    assert(v55 == (55u << 16 | 8) && api(0x100) == v55);
    assert(nvn_driver_info(image, 0, sizeof image, 0, 5, &v) == 0); /* an item that keeps w0: no default given */
    hook_version_text(text, sizeof text, HOOK_NVN, v55);
    assert(!strcmp(text, "55.8"));
    driver(53, 209);
    assert(api(0x100) == (53u << 16 | 209));
    /* a 32-bit driver's: its default, its constant, not the state it reads or past its bound */
    driver32(52, 32);
    assert(nvn_driver_info32(image, 0, sizeof image, 0, 0, &v) && v == 52);
    assert(nvn_driver_info32(image, 0, sizeof image, 0, 1, &v) && v == 32);
    assert(nvn_driver_info32(image, 0, sizeof image, 0, 3, &v) && v == 52);
    assert(!nvn_driver_info32(image, 0, sizeof image, 0, 2, &v) &&
           !nvn_driver_info32(image, 0, sizeof image, 0, 4, &v));
    assert(!nvn_driver_info32(image, 0, 0x48, 0, 1, &v)); /* the table cut off */
    driver(55, 8);
    /* code that isn't the switch, or a switch cut off by the end of the image: unknown */
    assert(api(0x30) == 0 && nvn_driver_info(image, 0, 0x20, 0, 0, &v) == 0);
    /* out of range for the key */
    assert(nvn_api_pack(0, 5) == 0 && nvn_api_pack(55, 65536) == 0 && nvn_api_pack(-1, 0) == 0);
    assert(nvn_api_pack(65535, 65535) == 0xFFFFFFFFu);

    /* the declaration that holds for the version; the unversioned one for any, unknown included */
    assert(!strcmp(pick("nvnWindowAcquireTexture", v55), "acquire_55"));
    assert(!strcmp(pick("nvnWindowAcquireTexture", 55u << 16 | 15), "acquire_55"));
    assert(!strcmp(pick("nvnWindowAcquireTexture", 53u << 16 | 209), "acquire_53"));
    assert(!pick("nvnWindowAcquireTexture", 56u << 16) && !pick("nvnWindowAcquireTexture", 52u << 16 | 999));
    assert(!pick("nvnWindowAcquireTexture", 0)); /* unknown: a versioned declaration never guesses */
    assert(!strcmp(pick("nvnQueueFinish", 0), "queue_finish") && !strcmp(pick("nvnQueueFinish", v55), "queue_finish"));
    assert(!pick("nvnQueueFenceSync", v55));
    {
        int first, k;
        u32 versions[HOOK_KEYS] = {0};
        versions[HOOK_NVN] = 56u << 16;
        k = hook_select(&nvn_table, "nvnWindowAcquireTexture", versions, &first);
        assert(k == -2 && first >= 0); /* declared, but not for this version: nvn names it so */
        hook_range_text(text, sizeof text, nvn_key[first], nvn_lo[first], nvn_hi[first]);
        assert(!strcmp(text, "nvn >= 53.0, < 55.0"));
    }

    /* exact measured older versions, then the described newer range; no inference for adjacent versions */
    assert(nvn_api_described(55u << 16 | 8) && nvn_api_described(55u << 16 | 12) && nvn_api_described(55u << 16 | 15));
    assert(nvn_api_described(53u << 16 | 209) && nvn_api_described(api(0x100)));
    assert(nvn_api_described(53u << 16 | 5) && nvn_api_described(53u << 16 | 13) && nvn_api_described(53u << 16 | 105));
    assert(nvn_api_described(53u << 16 | 311) && nvn_api_described(53u << 16 | 312) &&
           nvn_api_described(53u << 16 | 313) && nvn_api_described(53u << 16 | 314));
    assert(!nvn_api_described(55u << 16 | 7) && !nvn_api_described(55u << 16 | 16) &&
           !nvn_api_described(53u << 16 | 208) && !nvn_api_described(53u << 16 | 210) &&
           !nvn_api_described(53u << 16 | 310) && !nvn_api_described(53u << 16 | 315) &&
           !nvn_api_described(53u << 16 | 4) && !nvn_api_described(53u << 16 | 6) &&
           !nvn_api_described(53u << 16 | 104) && !nvn_api_described(53u << 16 | 106) &&
           !nvn_api_described(54u << 16) && !nvn_api_described(0));
    nvn_api_described_text(text, sizeof text);
    assert(!strcmp(text, "53.5, 53.13, 53.105, 53.209, 53.311 to 53.314, 55.8 to 55.15"));
    nvn_api_described_text(text, 8); /* cut short, still terminated */
    assert(!strcmp(text, "53.5, 5"));
    puts("nvn versions: ok");
    return 0;
}
