/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/set/settings_server.cpp, settings_types.h, system_settings_server.cpp,
   src/core/file_sys/system_archive/system_version.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018, 2021 yuzu Emulator Project (GPL-2.0-or-later) */
/* set and set:sys: the system's language, region, keyboard, nickname and firmware version */
#include "nx/hle/services/services_int.h"
#include "nx/hle/hle.h"

#include "hle/services/set.swipc.inc"
#include "nx/hle/services/key_code_map.inc"

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
/* each language's nn::settings::KeyboardLayout, set:sys's default for it: 0 Japanese, 1 EnglishUs, 2
   EnglishUsInternational, 3 EnglishUk, 4 French, 5 FrenchCa, 6 Spanish, 7 SpanishLatin, 8 German, 9 Italian, 10
   Portuguese, 11 Russian, 12 Korean, 13 ChineseSimplified, 14 ChineseTraditional */
static const u8 set_layouts[SET_LANGUAGES] = {0, 1, 4, 8, 9, 6, 13, 12, 2, 10, 11, 14, 3, 5, 7, 13, 14, 10};

/* the system settings set:sys keeps for the run: g_hle.set (services_int.h) */
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
    const u8* m = set_languages[set_language(g_settings.language)].key_code_map;
    memcpy(GP(map.addr), m, (size_t)(map.size < 0x1000 ? map.size : 0x1000));
}

/* ============================================================ set */
static int nn_settings_ISettingsServer_GetLanguageCode(SvcObj* self, nn_settings_LanguageCode* code) {
    (void)self;
    set_language_code(g_settings.language, code->v);
    R_SUCCEED();
}
static int nn_settings_ISettingsServer_GetAvailableLanguageCodes(SvcObj* self, IpcBuf codes, i32* count) {
    (void)self;
    *count = set_list_languages(codes, SET_LANGUAGES_V1);
    R_SUCCEED();
}
static int nn_settings_ISettingsServer_MakeLanguageCode(SvcObj* self, u32 language, nn_settings_LanguageCode* code) {
    (void)self;
    R_UNLESS(language < (u32)SET_LANGUAGES, SNX_RC_SETTINGS_LANGUAGE_OUT_OF_RANGE);
    memcpy(code->v, set_languages[language].code, 8);
    R_SUCCEED();
}
static int nn_settings_ISettingsServer_GetAvailableLanguageCodeCount(SvcObj* self, i32* count) {
    (void)self;
    *count = SET_LANGUAGES < SET_LANGUAGES_V1 ? SET_LANGUAGES : SET_LANGUAGES_V1;
    R_SUCCEED();
}
/* the region a console sold with that language comes from */
static int nn_settings_ISettingsServer_GetRegionCode(SvcObj* self, i32* region) {
    (void)self;
    *region = set_languages[set_language(g_settings.language)].region;
    R_SUCCEED();
}
static int nn_settings_ISettingsServer_GetAvailableLanguageCodes2(SvcObj* self, IpcBuf codes, i32* count) {
    (void)self;
    *count = set_list_languages(codes, SET_LANGUAGES);
    R_SUCCEED();
}
static int nn_settings_ISettingsServer_GetAvailableLanguageCodeCount2(SvcObj* self, i32* count) {
    (void)self;
    *count = SET_LANGUAGES;
    R_SUCCEED();
}
/* the keyboard layout of the language */
static int nn_settings_ISettingsServer_GetKeyCodeMap(SvcObj* self, IpcBuf map) {
    (void)self;
    set_key_code_map(map);
    R_SUCCEED();
}
static int nn_settings_ISettingsServer_GetKeyCodeMap2(SvcObj* self, IpcBuf map) {
    (void)self;
    set_key_code_map(map);
    R_SUCCEED();
}
/* the kiosk flag, set:sys's */
static int nn_settings_ISettingsServer_GetQuestFlag(SvcObj* self, u8* quest) {
    (void)self;
    *quest = g_hle.set.quest;
    R_SUCCEED();
}
static int nn_settings_ISettingsServer_GetDeviceNickName(SvcObj* self, IpcBuf name) {
    static const char nick[] = "ScarletNX";
    (void)self;
    if (!name.size) R_SUCCEED();
    /* cut to leave room for the NUL */
    memset(GP(name.addr), 0, (size_t)name.size);
    memcpy(GP(name.addr), nick, name.size <= sizeof nick - 1 ? (size_t)name.size - 1 : sizeof nick - 1);
    R_SUCCEED();
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
    R_SUCCEED();
}
static int nn_settings_ISystemSettingsServer_GetFirmwareVersion2(SvcObj* self, IpcBuf version) {
    (void)self;
    set_firmware_version(version);
    R_SUCCEED();
}
static int nn_settings_ISystemSettingsServer_GetLockScreenFlag(SvcObj* self, u8* flag) {
    (void)self;
    *flag = g_hle.set.lock_screen;
    R_SUCCEED();
}
static int nn_settings_ISystemSettingsServer_SetLockScreenFlag(SvcObj* self, u8 flag) {
    (void)self;
    g_hle.set.lock_screen = flag != 0;
    R_SUCCEED();
}
static int nn_settings_ISystemSettingsServer_GetColorSetId(SvcObj* self, i32* id) {
    (void)self;
    *id = g_hle.set.color_set;
    R_SUCCEED();
}
static int nn_settings_ISystemSettingsServer_SetColorSetId(SvcObj* self, i32 id) {
    (void)self;
    g_hle.set.color_set = id;
    R_SUCCEED();
}
static int nn_settings_ISystemSettingsServer_GetVibrationMasterVolume(SvcObj* self, float* volume) {
    (void)self;
    *volume = g_hle.set.vibration;
    R_SUCCEED();
}
static int nn_settings_ISystemSettingsServer_SetVibrationMasterVolume(SvcObj* self, float volume) {
    (void)self;
    g_hle.set.vibration = volume;
    R_SUCCEED();
}
static int nn_settings_ISystemSettingsServer_GetSpeakerAutoMuteFlag(SvcObj* self, u8* flag) {
    (void)self;
    *flag = g_hle.set.speaker_auto_mute;
    R_SUCCEED();
}
static int nn_settings_ISystemSettingsServer_SetSpeakerAutoMuteFlag(SvcObj* self, u8 flag) {
    (void)self;
    g_hle.set.speaker_auto_mute = flag != 0;
    R_SUCCEED();
}
static int nn_settings_ISystemSettingsServer_GetQuestFlag(SvcObj* self, u8* flag) {
    (void)self;
    *flag = g_hle.set.quest;
    R_SUCCEED();
}
static int nn_settings_ISystemSettingsServer_SetQuestFlag(SvcObj* self, u8 flag) {
    (void)self;
    g_hle.set.quest = flag != 0;
    R_SUCCEED();
}
static int nn_settings_ISystemSettingsServer_GetBatteryPercentageFlag(SvcObj* self, u8* flag) {
    (void)self;
    *flag = g_hle.set.battery_percentage;
    R_SUCCEED();
}
static int nn_settings_ISystemSettingsServer_SetBatteryPercentageFlag(SvcObj* self, u8 flag) {
    (void)self;
    g_hle.set.battery_percentage = flag != 0;
    R_SUCCEED();
}
static int nn_settings_ISystemSettingsServer_GetKeyboardLayout(SvcObj* self, u32* layout) {
    (void)self;
    *layout = g_hle.set.layout_set ? g_hle.set.keyboard_layout : set_layouts[set_language(g_settings.language)];
    R_SUCCEED();
}
static int nn_settings_ISystemSettingsServer_SetKeyboardLayout(SvcObj* self, u32 layout) {
    (void)self;
    g_hle.set.keyboard_layout = layout;
    g_hle.set.layout_set = 1;
    R_SUCCEED();
}

/* no setting is kept: the value is empty, as GetSettingsItemValueSize says */
static int nn_settings_ISystemSettingsServer_GetSettingsItemValue(SvcObj* self, IpcBuf name, IpcBuf key, IpcBuf value,
                                                                  u64* size) {
    char n[0x49], k[0x49];
    (void)self;
    memset(GP(value.addr), 0, (size_t)value.size);
    memset(n, 0, sizeof n);
    memset(k, 0, sizeof k);
    memcpy(n, GP(name.addr), (size_t)(name.size < 0x48 ? name.size : 0x48));
    memcpy(k, GP(key.addr), (size_t)(key.size < 0x48 ? key.size : 0x48));
    LOG_LIMIT(16, "set:sys: GetSettingsItemValue(%s!%s): no such setting here\n", n, k);
    *size = 0;
    R_SUCCEED();
}

SvcObj* new_set(void) { return nn_settings_ISettingsServer_new(); }
SvcObj* new_setsys(void) { return nn_settings_ISystemSettingsServer_new(); }
