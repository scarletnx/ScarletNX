/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The WFC services' list, the choice of a DNS server and the DS firmware's connection slot */
#include "common/online/wfc.h"
#include "online_check.h"
#include <stdint.h>

static void presets(void) {
    CHECK_INT(snx_online_wfc_preset_count(), 9);
    CHECK(snx_online_wfc_preset_at(9) == NULL);
    for (size_t i = 0; i < snx_online_wfc_preset_count(); ++i) {
        const SnxOnlineWfcPreset* p = snx_online_wfc_preset_at(i);
        CHECK(snx_online_wfc_preset_find(p->id) == p); /* every id names its own service */
        for (const char* c = p->id; *c; ++c) CHECK((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_');
        CHECK(strcmp(p->id, "off") && strcmp(p->id, "auto") && strcmp(p->id, "custom")); /* no service's */
    }
    const SnxOnlineWfcPreset* kaeru = snx_online_wfc_preset_find("kaeru");
    CHECK(kaeru && kaeru->dns[0] == 178 && kaeru->dns[1] == 62 && kaeru->dns[2] == 43 && kaeru->dns[3] == 212);
    CHECK_STR(snx_online_wfc_preset_at(0)->id, "kaeru"); /* the issue's first, the menus' first */
    CHECK(snx_online_wfc_preset_find("official") == NULL);
    CHECK(snx_online_wfc_preset_find(NULL) == NULL);
}

static void choice(void) {
    uint8_t dns[4] = {9, 9, 9, 9};
    char why[512] = "";
    CHECK_INT(snx_online_wfc_dns("wiimmfi", NULL, dns, why, sizeof why), 1);
    CHECK(dns[0] == 95 && dns[1] == 217 && dns[2] == 77 && dns[3] == 181);
    CHECK_INT(snx_online_wfc_dns("off", NULL, dns, why, sizeof why), 0);
    CHECK_INT(snx_online_wfc_dns("auto", NULL, dns, why, sizeof why), SNX_ONLINE_WFC_AUTO);
    CHECK_INT(snx_online_wfc_dns("custom", "10.0.0.53", dns, why, sizeof why), 1);
    CHECK(dns[0] == 10 && dns[1] == 0 && dns[2] == 0 && dns[3] == 53);
    CHECK_INT(snx_online_wfc_dns("custom", "10.0.0", dns, why, sizeof why), -1);
    CHECK_HAS(why, "\"10.0.0\"");
    CHECK_HAS(why, "no server is set");
    CHECK_INT(snx_online_wfc_dns("custom", "0.0.0.0", dns, why, sizeof why), -1); /* the DS's "from the network" */
    CHECK_HAS(why, "\"0.0.0.0\"");
    CHECK_INT(snx_online_wfc_dns("kaeruwfc", NULL, dns, why, sizeof why), -1);
    CHECK_HAS(why, "\"kaeruwfc\"");
    CHECK_HAS(why, "off, auto, custom, kaeru, wiilink, wiimmfi");
    CHECK_HAS(why, "sudomemo: no server is set");
}

static void ipv4(void) {
    static const struct {
        const char* text;
        int ok;
    } cases[] = {
        {"1.2.3.4", 1},    {"255.255.255.255", 1}, {"0.0.0.0", 1},  {"256.1.1.1", 0}, {"01.2.3.4", 0}, {"1.2.3", 0},
        {"1.2.3.4.5", 0},  {"1..3.4", 0},          {" 1.2.3.4", 0}, {"1.2.3.4 ", 0},  {"1.2.3.-4", 0}, {"", 0},
        {"1234.1.1.1", 0}, {"a.b.c.d", 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        uint8_t out[4];
        if (snx_online_ipv4_parse(cases[i].text, out) != cases[i].ok) {
            fprintf(stderr, "snx_online_ipv4_parse(\"%s\") is not %d\n", cases[i].text, cases[i].ok);
            ++g_failures;
        }
    }
    CHECK(!snx_online_ipv4_parse(NULL, (uint8_t[4]){0}));
}

static void crc(void) {
    static const uint8_t check[] = "123456789";
    CHECK_INT(snx_online_nds_crc16(0, check, 9), 0xBB3D);      /* CRC-16/ARC's check value */
    CHECK_INT(snx_online_nds_crc16(0xFFFF, check, 9), 0x4B37); /* CRC-16/MODBUS's: the user settings' start */
}

static void slot(void) {
    uint8_t s[SNX_ONLINE_NDS_WFC_SLOT_SIZE], dns[4] = {178, 62, 43, 212}, none[4] = {0};
    char why[256] = "";
    SnxOnlineNdsWfcSlot back;
    CHECK_INT(snx_online_nds_wfc_slot(s, "ScarletNX", dns, dns, why, sizeof why), 0);
    /* the fields where a DS reads them (GBATEK: SSID 40h, DNS C8h and CCh, status E7h, SSID length E8h) */
    CHECK(!memcmp(s + 0x40, "ScarletNX", 9) && s[0x49] == 0);
    CHECK(!memcmp(s + 0xC8, dns, 4) && !memcmp(s + 0xCC, dns, 4));
    CHECK_INT(s[0xE7], 0);
    CHECK_INT(s[0xE8], 9);
    CHECK_INT(s[0xEF], 1);
    CHECK_INT(s[0xFE] | s[0xFF] << 8, snx_online_nds_crc16(0, s, 0xFE));
    CHECK_INT(snx_online_nds_wfc_slot_read(s, &back, why, sizeof why), 1);
    CHECK_STR(back.ssid, "ScarletNX");
    CHECK(!memcmp(back.dns1, dns, 4) && !memcmp(back.dns2, dns, 4));
    s[0xC8] ^= 1; /* a byte changed after the CRC */
    CHECK_INT(snx_online_nds_wfc_slot_read(s, &back, why, sizeof why), -1);
    CHECK_HAS(why, "where its bytes give 0x");
    snx_online_nds_wfc_slot_empty(s);
    CHECK_INT(s[0xE7], 0xFF);
    CHECK_INT(snx_online_nds_wfc_slot_read(s, &back, why, sizeof why), 0);
    CHECK_INT(snx_online_nds_wfc_slot(s, "123456789012345678901234567890123", none, none, why, sizeof why), -1);
    CHECK_HAS(why, "has 33 bytes");
    CHECK_INT(snx_online_nds_wfc_slot(s, "", none, none, why, sizeof why), -1);
    CHECK_HAS(why, "has 0 bytes");
}

int main(void) {
    presets();
    choice();
    ipv4();
    crc();
    slot();
    CHECK_DONE();
}
