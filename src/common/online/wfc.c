/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/online/wfc.h"
#include <stdio.h>
#include <string.h>

/* The replacement services the DS community keeps, in the order issue scarletnx/ScarletNX#2307 lists them. Each
   server answered nas.nintendowifi.net with a WFC server when README.md here was written
   (tests/online_presets_live.c asks them again); a service that stops answering leaves this list */
static const SnxOnlineWfcPreset presets[] = {
    {"kaeru", "Kaeru WFC", {178, 62, 43, 212}},          {"wiilink", "WiiLink", {167, 235, 229, 36}},
    {"wiimmfi", "Wiimmfi", {95, 217, 77, 181}},          {"wiilink_wfc", "WiiLink WFC", {5, 161, 56, 11}},
    {"altwfc", "AltWFC", {172, 104, 88, 237}},           {"newwfc", "NewWFC", {89, 117, 58, 143}},
    {"skytemple", "SkyTemple WFC", {91, 107, 226, 151}}, {"diyfi", "DIYFI", {3, 22, 166, 69}},
    {"sudomemo", "Sudomemo", {104, 248, 0, 110}},
};
#define PRESET_COUNT (sizeof presets / sizeof *presets)

size_t snx_online_wfc_preset_count(void) { return PRESET_COUNT; }
const SnxOnlineWfcPreset* snx_online_wfc_preset_at(size_t index) {
    return index < PRESET_COUNT ? &presets[index] : NULL;
}
const SnxOnlineWfcPreset* snx_online_wfc_preset_find(const char* id) {
    for (size_t i = 0; id && i < PRESET_COUNT; ++i)
        if (!strcmp(presets[i].id, id)) return &presets[i];
    return NULL;
}

int snx_online_ipv4_parse(const char* text, uint8_t out[4]) {
    uint8_t got[4];
    const char* p = text;
    if (!p) return 0;
    for (int part = 0; part < 4; ++part) {
        unsigned value = 0, digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 4) value = value * 10 + (unsigned)(*p++ - '0'), ++digits;
        if (!digits || digits > 3 || value > 255 || (digits > 1 && p[-(int)digits] == '0')) return 0;
        got[part] = (uint8_t)value;
        if (part < 3 && *p++ != '.') return 0;
    }
    if (*p) return 0;
    memcpy(out, got, 4);
    return 1;
}

int snx_online_wfc_dns(const char* choice, const char* custom, uint8_t dns[4], char* why, size_t size) {
    const SnxOnlineWfcPreset* preset = snx_online_wfc_preset_find(choice);
    if (preset) {
        memcpy(dns, preset->dns, 4);
        return 1;
    }
    if (choice && !strcmp(choice, "off")) return 0;
    if (choice && !strcmp(choice, "auto")) return SNX_ONLINE_WFC_AUTO;
    if (choice && !strcmp(choice, "custom")) {
        uint8_t got[4];
        static const uint8_t none[4], all[4] = {255, 255, 255, 255};
        if (!snx_online_ipv4_parse(custom, got) || !memcmp(got, none, 4) || !memcmp(got, all, 4)) {
            snprintf(why, size,
                     "the custom DNS server \"%s\" is not an IPv4 address such as 178.62.43.212: no "
                     "server is set",
                     custom ? custom : "");
            return -1;
        }
        memcpy(dns, got, 4);
        return 1;
    }
    char ids[256] = "";
    for (size_t i = 0; i < PRESET_COUNT; ++i)
        snprintf(ids + strlen(ids), sizeof ids - strlen(ids), "%s, ", presets[i].id);
    snprintf(why, size, "\"%s\" is none of off, auto, custom, %.*s: no server is set", choice ? choice : "",
             (int)(strlen(ids) > 2 ? strlen(ids) - 2 : 0), ids);
    return -1;
}

uint16_t snx_online_nds_crc16(uint16_t crc, const uint8_t* bytes, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) crc = (uint16_t)(crc & 1 ? crc >> 1 ^ 0xA001 : crc >> 1);
    }
    return crc;
}

/* a slot's fields (melonDS's Firmware::WifiAccessPoint, GBATEK's table) */
enum {
    SLOT_SSID = 0x40,
    SLOT_DNS1 = 0xC8,
    SLOT_DNS2 = 0xCC,
    SLOT_WEP_MODE = 0xE6,
    SLOT_STATUS = 0xE7,
    SLOT_SSID_LENGTH = 0xE8,
    SLOT_CONFIGURED = 0xEF,
    SLOT_CRC = 0xFE,
};

static void slot_crc(uint8_t slot[SNX_ONLINE_NDS_WFC_SLOT_SIZE]) {
    uint16_t crc = snx_online_nds_crc16(0, slot, SLOT_CRC);
    slot[SLOT_CRC] = (uint8_t)crc;
    slot[SLOT_CRC + 1] = (uint8_t)(crc >> 8);
}

int snx_online_nds_wfc_slot(uint8_t slot[SNX_ONLINE_NDS_WFC_SLOT_SIZE], const char* ssid, const uint8_t dns1[4],
                            const uint8_t dns2[4], char* why, size_t size) {
    size_t length = ssid ? strlen(ssid) : 0;
    if (!length || length > 32) {
        snprintf(why, size, "the network's name \"%s\" has %u bytes, not 1 to 32", ssid ? ssid : "", (unsigned)length);
        return -1;
    }
    memset(slot, 0, SNX_ONLINE_NDS_WFC_SLOT_SIZE);
    memcpy(slot + SLOT_SSID, ssid, length);
    memcpy(slot + SLOT_DNS1, dns1, 4);
    memcpy(slot + SLOT_DNS2, dns2, 4);
    slot[SLOT_WEP_MODE] = 0; /* open: no WEP */
    slot[SLOT_STATUS] = 0;   /* normal, set by hand rather than by AOSS */
    slot[SLOT_SSID_LENGTH] = (uint8_t)length;
    slot[SLOT_CONFIGURED] = 1;
    slot_crc(slot);
    return 0;
}

void snx_online_nds_wfc_slot_empty(uint8_t slot[SNX_ONLINE_NDS_WFC_SLOT_SIZE]) {
    memset(slot, 0, SNX_ONLINE_NDS_WFC_SLOT_SIZE);
    slot[SLOT_STATUS] = 0xFF;
    slot_crc(slot);
}

int snx_online_nds_wfc_slot_read(const uint8_t slot[SNX_ONLINE_NDS_WFC_SLOT_SIZE], SnxOnlineNdsWfcSlot* out, char* why,
                                 size_t size) {
    uint16_t crc = snx_online_nds_crc16(0, slot, SLOT_CRC), kept = (uint16_t)(slot[SLOT_CRC] | slot[SLOT_CRC + 1] << 8);
    if (crc != kept) {
        snprintf(why, size, "the slot's CRC is 0x%04X where its bytes give 0x%04X", kept, crc);
        return -1;
    }
    if (slot[SLOT_STATUS] == 0xFF) return 0;
    size_t length = slot[SLOT_SSID_LENGTH] > 32 ? 32 : slot[SLOT_SSID_LENGTH];
    memcpy(out->ssid, slot + SLOT_SSID, length);
    out->ssid[length] = 0;
    memcpy(out->dns1, slot + SLOT_DNS1, 4);
    memcpy(out->dns2, slot + SLOT_DNS2, 4);
    return 1;
}
