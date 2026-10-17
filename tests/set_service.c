/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* set and set:sys replies (set.c): the firmware version's layout and its copy into short buffers, and the
   language codes */
#include "nx/hle/services/services_int.h"
#include <stdarg.h>
#include <stdio.h>

#include "nx/hle/services/set.c"

u8* g_mem;
int g_verbose;
int g_opt_lang = 1;
static unsigned test_checks;

static void test_fail(const char* expression, int line) {
    fprintf(stderr, "set_service.c:%d: check failed: %s\n", line, expression);
    exit(1);
}
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        test_checks++;                                                                                                 \
        if (!(expr)) test_fail(#expr, __LINE__);                                                                       \
    } while (0)

void logf_(int level, const char* format, ...) {
    (void)level;
    (void)format;
}
void fatal(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    exit(1);
}
SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*), size_t size) {
    (void)name, (void)dispatch, (void)size;
    return NULL;
}
void ipc_out_overflow(IpcCtx* c) {
    (void)c;
    test_fail("IPC output overflow", __LINE__);
}
int ipc_bad_request(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return 1;
}
int ipc_bad_objects(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return 1;
}
int stub_ok(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return 0;
}

int main(void) {
    static u8 memory[0x1000];
    IpcBuf buf;
    nn_settings_LanguageCode code;
    i32 count;
    u32 i;
    g_mem = memory;

    /* FirmwareVersion: 12.1.0, revision 1.0, every byte outside the fields zero */
    memset(memory, 0xAA, sizeof memory);
    buf.addr = 0x100;
    buf.size = 0x100;
    CHECK(nn_settings_ISystemSettingsServer_GetFirmwareVersion2(NULL, buf) == 0);
    CHECK(memory[0x100] == 12 && memory[0x101] == 1 && memory[0x102] == 0 && memory[0x103] == 0);
    CHECK(memory[0x104] == 1 && memory[0x105] == 0 && memory[0x106] == 0 && memory[0x107] == 0);
    CHECK(!memcmp(memory + 0x108, "NX", 3));
    for (i = 0x28; i < 0x68; i++) CHECK(memory[0x100 + i] == 0); /* the hash is left empty */
    CHECK(!memcmp(memory + 0x168, "12.1.0", 7));
    CHECK(!memcmp(memory + 0x180, "NintendoSDK Firmware for NX 12.1.0-1.0", 39));
    for (i = 0x80 + 39; i < 0x100; i++) CHECK(memory[0x100 + i] == 0);
    CHECK(memory[0xFF] == 0xAA && memory[0x200] == 0xAA);
    memset(memory, 0xAA, sizeof memory);
    CHECK(nn_settings_ISystemSettingsServer_GetFirmwareVersion(NULL, buf) == 0);
    CHECK(memory[0x100] == 12 && memory[0x101] == 1 && !memcmp(memory + 0x168, "12.1.0", 7));

    /* a short buffer gets only its size */
    memset(memory, 0xAA, sizeof memory);
    buf.size = 6;
    CHECK(nn_settings_ISystemSettingsServer_GetFirmwareVersion2(NULL, buf) == 0);
    CHECK(memory[0x100] == 12 && memory[0x104] == 1 && memory[0x105] == 0 && memory[0x106] == 0xAA);

    /* the 18 languages of 12.1.0: the "2" commands list all of them, the old ones 15 */
    CHECK(set_language_count() == 18);
    CHECK(nn_settings_ISettingsServer_GetAvailableLanguageCodeCount2(NULL, &count) == 0 && count == 18);
    CHECK(nn_settings_ISettingsServer_GetAvailableLanguageCodeCount(NULL, &count) == 0 && count == 15);
    buf.addr = 0x400;
    buf.size = 8 * 32;
    CHECK(nn_settings_ISettingsServer_GetAvailableLanguageCodes2(NULL, buf, &count) == 0 && count == 18);
    CHECK(!memcmp(memory + 0x400 + 17 * 8, "pt-BR\0\0", 8));
    CHECK(nn_settings_ISettingsServer_MakeLanguageCode(NULL, 17, &code) == 0 && !memcmp(code.v, "pt-BR\0\0", 8));
    CHECK(nn_settings_ISettingsServer_MakeLanguageCode(NULL, 18, &code) != 0);
    CHECK(nn_settings_ISettingsServer_GetLanguageCode(NULL, &code) == 0 && !memcmp(code.v, "en-US\0\0", 8));

    printf("PASS set_service (%u checks)\n", test_checks);
    return 0;
}
