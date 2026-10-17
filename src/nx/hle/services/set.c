/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/set/settings_server.cpp, settings_types.h, system_settings_server.cpp,
   src/core/file_sys/system_archive/system_version.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018, 2021 yuzu Emulator Project (GPL-2.0-or-later) */
/* set and set:sys: the system's language, region, keyboard, nickname and firmware version */
#include "nx/hle/services/services_int.h"

#include "hle/services/set.swipc.inc"
#include "nx/hle/services/key_code_map.inc"

#define SET_INVALID_LANGUAGE RC(105, 625)

/* nn::settings::Language, in the order of its values, with the region and keyboard layout that go with each */
static const struct {
    char code[8];
    u8 region; /* nn::settings::system::RegionCode: 0 Japan, 1 USA, 2 Europe, 4 HongKongTaiwanKorea, 5 China */
    const u8* key_code_map;
} set_languages[] = {
    {"ja", 0, KeyCodeMapJapanese},
    {"en-US", 1, KeyCodeMapEnglishUsInternational},
    {"fr", 2, KeyCodeMapFrench},
    {"de", 2, KeyCodeMapGerman},
    {"it", 2, KeyCodeMapItalian},
    {"es", 2, KeyCodeMapSpanish},
    {"zh-CN", 5, KeyCodeMapChineseSimplified},
    {"ko", 4, KeyCodeMapKorean},
    {"nl", 2, KeyCodeMapEnglishUsInternational},
    {"pt", 2, KeyCodeMapPortuguese},
    {"ru", 2, KeyCodeMapRussian},
    {"zh-TW", 4, KeyCodeMapChineseTraditional},
    {"en-GB", 2, KeyCodeMapEnglishUk},
    {"fr-CA", 1, KeyCodeMapFrenchCa},
    {"es-419", 1, KeyCodeMapSpanishLatin},
    {"zh-Hans", 5, KeyCodeMapChineseSimplified},
    {"zh-Hant", 4, KeyCodeMapChineseTraditional},
    {"pt-BR", 1, KeyCodeMapPortuguese},
};
#define SET_LANGUAGES ((int)(sizeof set_languages / sizeof set_languages[0]))
/* GetAvailableLanguageCodes, from before 4.0.0, lists 15 at most */
#define SET_LANGUAGES_V1 15

int set_language_count(void) { return SET_LANGUAGES; }
static int set_language(int lang) { return lang >= 0 && lang < SET_LANGUAGES ? lang : 1; }
void set_language_code(int lang, u8 code[8]) { memcpy(code, set_languages[set_language(lang)].code, 8); }

/* up to max codes into the buffer, as many as fit */
static i32 set_list_languages(IpcBuf codes, int max) {
    int n = (int)(codes.size / 8 < (u64)max ? codes.size / 8 : (u64)max), i;
    for (i = 0; i < n; i++) memcpy(GP(codes.addr + 8 * (u64)i), set_languages[i].code, 8);
    return n;
}
static void set_key_code_map(IpcBuf map) {
    const u8* m = set_languages[set_language(g_opt_lang)].key_code_map;
    memcpy(GP(map.addr), m, (size_t)(map.size < 0x1000 ? map.size : 0x1000));
}

/* ============================================================ set */
static int nn_settings_ISettingsServer_GetLanguageCode(SvcObj* self, nn_settings_LanguageCode* code) {
    (void)self;
    set_language_code(g_opt_lang, code->v);
    return 0;
}
static int nn_settings_ISettingsServer_GetAvailableLanguageCodes(SvcObj* self, IpcBuf codes, i32* count) {
    (void)self;
    *count = set_list_languages(codes, SET_LANGUAGES_V1);
    return 0;
}
static int nn_settings_ISettingsServer_MakeLanguageCode(SvcObj* self, u32 language, nn_settings_LanguageCode* code) {
    (void)self;
    if (language >= (u32)SET_LANGUAGES) return SET_INVALID_LANGUAGE;
    memcpy(code->v, set_languages[language].code, 8);
    return 0;
}
static int nn_settings_ISettingsServer_GetAvailableLanguageCodeCount(SvcObj* self, i32* count) {
    (void)self;
    *count = SET_LANGUAGES < SET_LANGUAGES_V1 ? SET_LANGUAGES : SET_LANGUAGES_V1;
    return 0;
}
/* the region a console sold with that language comes from */
static int nn_settings_ISettingsServer_GetRegionCode(SvcObj* self, i32* region) {
    (void)self;
    *region = set_languages[set_language(g_opt_lang)].region;
    return 0;
}
static int nn_settings_ISettingsServer_GetAvailableLanguageCodes2(SvcObj* self, IpcBuf codes, i32* count) {
    (void)self;
    *count = set_list_languages(codes, SET_LANGUAGES);
    return 0;
}
static int nn_settings_ISettingsServer_GetAvailableLanguageCodeCount2(SvcObj* self, i32* count) {
    (void)self;
    *count = SET_LANGUAGES;
    return 0;
}
/* the keyboard layout of the language */
static int nn_settings_ISettingsServer_GetKeyCodeMap(SvcObj* self, IpcBuf map) {
    (void)self;
    set_key_code_map(map);
    return 0;
}
static int nn_settings_ISettingsServer_GetKeyCodeMap2(SvcObj* self, IpcBuf map) {
    (void)self;
    set_key_code_map(map);
    return 0;
}
static int nn_settings_ISettingsServer_GetDeviceNickName(SvcObj* self, IpcBuf name) {
    static const char nick[] = "ScarletNX";
    (void)self;
    memset(GP(name.addr), 0, (size_t)name.size);
    memcpy(GP(name.addr), nick, name.size < sizeof nick ? (size_t)name.size : sizeof nick);
    return 0;
}

/* ============================================================ set:sys */
/* nn::settings::system::FirmwareVersion, laid out as the SystemVersion archive (0100000000000809) holds it:
   major, minor and micro at 0..2, the revision's major and minor at 4 and 5, the platform at 0x08, the version
   hash at 0x28 (left empty), the display version at 0x68 and the display title at 0x80. ScarletNX reports 12.1.0,
   the firmware it has always reported, whose languages (no Polish or Thai) are the ones set_languages lists */
static void set_firmware_version(IpcBuf out) {
    u8 v[0x100];
    memset(v, 0, sizeof v);
    v[0] = 12; /* major, minor, micro */
    v[1] = 1;
    v[2] = 0;
    v[4] = 1; /* revision major; the minor (5) is 0 */
    strcpy((char*)v + 0x08, "NX");
    strcpy((char*)v + 0x68, "12.1.0");
    strcpy((char*)v + 0x80, "NintendoSDK Firmware for NX 12.1.0-1.0");
    memcpy(GP(out.addr), v, (size_t)(out.size < sizeof v ? out.size : sizeof v));
}
/* GetFirmwareVersion, the old command, answers the revision's minor number as 0, which it is already */
static int nn_settings_ISystemSettingsServer_GetFirmwareVersion(SvcObj* self, IpcBuf version) {
    (void)self;
    set_firmware_version(version);
    return 0;
}
static int nn_settings_ISystemSettingsServer_GetFirmwareVersion2(SvcObj* self, IpcBuf version) {
    (void)self;
    set_firmware_version(version);
    return 0;
}
/* no setting is kept: the value is empty, as GetSettingsItemValueSize says */
static int nn_settings_ISystemSettingsServer_GetSettingsItemValue(SvcObj* self, IpcBuf name, IpcBuf key, IpcBuf value,
                                                                  u64* size) {
    char n[0x49], k[0x49];
    (void)self, (void)value;
    memset(n, 0, sizeof n);
    memset(k, 0, sizeof k);
    memcpy(n, GP(name.addr), (size_t)(name.size < 0x48 ? name.size : 0x48));
    memcpy(k, GP(key.addr), (size_t)(key.size < 0x48 ? key.size : 0x48));
    LOG_LIMIT(16, "set:sys: GetSettingsItemValue(%s!%s): no such setting here\n", n, k);
    *size = 0;
    return 0;
}

SvcObj* new_set(void) { return nn_settings_ISettingsServer_new(0); }
SvcObj* new_setsys(void) { return nn_settings_ISystemSettingsServer_new(0); }
