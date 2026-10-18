/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* am.c with a control.nacp: the desired language against its supported-language flags, its display version and
   save data sizes; then am_service.c's checks, whose stand-ins this reuses */
#define main am_service_main
#include "am_service.c"
#undef main

extern const u8* g_test_nacp;
extern u64 g_test_nacp_size;

static void nacp_reset(void) {
    free(am.nacp);
    am.nacp = NULL;
    am.nacp_read = 0;
}

int main(void) {
    static u8 nacp[0x4000];
    u32 flags;
    u64 v;
    nn_oe_DisplayVersion version;
    i64 normal = 0, journal = 0;
    /* the system's language when the game has it (flags 0: every one), else the first fallback ns tries */
    CHECK(am_desired_language(1, 0) == 1);           /* American English */
    CHECK(am_desired_language(0, 1u << 2) == 0);     /* Japanese, which the game has */
    CHECK(am_desired_language(0, 1u << 0) == 1);     /* Japanese falls back to American English */
    CHECK(am_desired_language(12, 1u << 0) == 1);    /* British English to American */
    CHECK(am_desired_language(2, 1u << 9) == 13);    /* French to Canadian French */
    CHECK(am_desired_language(17, 1u << 10) == 9);   /* Brazilian Portuguese to Portuguese */
    CHECK(am_desired_language(15, 1u << 13) == 16);  /* Simplified Chinese to Traditional (zh-Hant) */
    CHECK(am_desired_language(6, 1u << 14) == 6);    /* zh-CN keeps its own code */
    CHECK(am_desired_language(1, 1u << 16) == 1);    /* only Polish, which has no code here: the system's */
    CHECK(am_desired_language(99, 1u << 2) == 0);    /* an index out of range is American English's list */

    /* GetDisplayVersion from the NACP; directory-backed save capacity is synthetic. */
    memcpy(nacp + 0x3060, "2.0.0", 6);
    flags = 1u << 2;
    memcpy(nacp + 0x302c, &flags, 4);
    v = 0x4000000;
    memcpy(nacp + 0x3080, &v, 8);
    v = 0x1000000;
    memcpy(nacp + 0x3088, &v, 8);
    v = 0x200000;
    memcpy(nacp + 0x3090, &v, 8);
    g_test_nacp = nacp;
    g_test_nacp_size = sizeof nacp;
    CHECK(nn_am_service_IApplicationFunctions_GetDisplayVersion(NULL, &version) == 0);
    CHECK(!strcmp((char*)version.v, "2.0.0"));
    CHECK(nn_am_service_IApplicationFunctions_GetSaveDataSize(NULL, 1, NULL, &normal, &journal) == 0);
    CHECK(normal == 1073741824 && journal == 1073741824);
    CHECK(nn_am_service_IApplicationFunctions_GetSaveDataSize(NULL, 3, NULL, &normal, &journal) == 0);
    CHECK(normal == 1073741824 && journal == 1073741824);
    nacp_reset();
    g_test_nacp = NULL;
    CHECK(nn_am_service_IApplicationFunctions_GetDisplayVersion(NULL, &version) == 0);
    CHECK(!strcmp((char*)version.v, "1.0.0"));
    nacp_reset();
    return am_service_main();
}
