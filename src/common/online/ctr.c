/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/online/ctr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t snx_online_pretendo_url(const char* url, char* out, size_t cap) {
    static const char* const targets[] = {"nintendowifi.net", "nintendo.net", "pokemon-gl.com"};
    static const char replacement[] = "pretendo.cc";
    size_t length = strlen(url);
    if (cap < length + 1) return 0;
    memcpy(out, url, length + 1);
    for (size_t t = 0; t < sizeof targets / sizeof *targets; ++t) {
        char* at = strstr(out, targets[t]);
        size_t from = strlen(targets[t]), to = sizeof replacement - 1;
        if (!at) continue;
        if (length - from + to + 1 > cap) return 0;
        memmove(at + to, at + from, strlen(at + from) + 1);
        memcpy(at, replacement, to);
        length = length - from + to;
    }
    return length;
}

const char* snx_online_pretendo_host(const char* host) {
    if (!strcmp(host, "nncs1.app.nintendowifi.net")) return "nncs1.app.pretendo.cc";
    if (!strcmp(host, "nncs2.app.nintendowifi.net")) return "nncs2.app.pretendo.cc";
    return host;
}

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const uint8_t* p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

int snx_online_ctr_lfcs_read(SnxOnlineCtrConsole* c, const uint8_t* bytes, size_t length, char* why, size_t size) {
    static const uint8_t zero[0x10];
    if (length != SNX_ONLINE_CTR_LFCS_SIZE) {
        snprintf(why, size, "LocalFriendCodeSeed_B has %u bytes, not 0x110", (unsigned)length);
        return -1;
    }
    if (!memcmp(bytes + 0x100, zero, sizeof zero)) {
        snprintf(why, size, "LocalFriendCodeSeed_B's seed is all zeros: not a console's");
        return -1;
    }
    memcpy(c->lfcs, bytes, SNX_ONLINE_CTR_LFCS_SIZE);
    snx_online_ctr_mac(c);
    return 0;
}

int snx_online_ctr_secure_info_read(SnxOnlineCtrConsole* c, const uint8_t* bytes, size_t length, char* why,
                                    size_t size) {
    if (length != SNX_ONLINE_CTR_SECURE_INFO_SIZE) {
        snprintf(why, size, "SecureInfo_A has %u bytes, not 0x111", (unsigned)length);
        return -1;
    }
    size_t n = 0;
    while (n < 15 && bytes[0x102 + n] >= 0x21 && bytes[0x102 + n] <= 0x7E) ++n;
    if (n < 8 || (n < 15 && bytes[0x102 + n])) {
        snprintf(why, size, "SecureInfo_A's serial number at 0x102 is not 8 to 15 letters and digits (%u read)",
                 (unsigned)n);
        return -1;
    }
    memcpy(c->serial, bytes + 0x102, n);
    c->serial[n] = 0;
    c->region = bytes[0x100];
    return 0;
}

int snx_online_ctr_account_read(SnxOnlineCtrConsole* c, const uint8_t* bytes, size_t length, char* why, size_t size) {
    if (length < SNX_ONLINE_CTR_ACCOUNT_SIZE) {
        snprintf(why, size, "the account file has %u bytes, fewer than the 0x54 its uidhmac ends at", (unsigned)length);
        return -1;
    }
    uint32_t pid = le32(bytes + 0x14);
    if (!pid) {
        snprintf(why, size, "the account file's principal ID at 0x14 is 0: the account was never made");
        return -1;
    }
    char hmac[9];
    for (int i = 0; i < 8; ++i) { /* UTF-16 at 0x42, as Nimbus's repair writes it */
        unsigned unit = bytes[0x42 + 2 * i] | (unsigned)bytes[0x43 + 2 * i] << 8;
        if (unit < 0x21 || unit > 0x7E) {
            snprintf(why, size, "the account file's uidhmac at 0x42 has 0x%04X as its letter %d, not a letter", unit,
                     i + 1);
            return -1;
        }
        hmac[i] = (char)unit;
    }
    hmac[8] = 0;
    c->pid = pid;
    c->friend_code = le64(bytes + 0x18);
    memcpy(c->uidhmac, hmac, sizeof hmac);
    return 0;
}

void snx_online_ctr_mac(SnxOnlineCtrConsole* c) {
    c->mac[0] = 0x40, c->mac[1] = 0xF4, c->mac[2] = 0x07;
    c->mac[3] = c->lfcs[0x108] ^ c->lfcs[0x10B];
    c->mac[4] = c->lfcs[0x109] ^ c->lfcs[0x10C];
    c->mac[5] = c->lfcs[0x10A] ^ c->lfcs[0x10D];
}

/* the whole file at path, at most max bytes: its length, or -1 with why */
static long read_file(const char* path, uint8_t* out, size_t max, char* why, size_t size) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        snprintf(why, size, "%s cannot be opened", path);
        return -1;
    }
    size_t n = fread(out, 1, max, f);
    int more = fgetc(f) != EOF, failed = ferror(f);
    fclose(f);
    if (failed || more) {
        snprintf(why, size, failed ? "%s cannot be read" : "%s holds more than %u bytes", path, (unsigned)max);
        return -1;
    }
    return (long)n;
}

int snx_online_ctr_console_load(SnxOnlineCtrConsole* c, const char* nand, unsigned account, char* why, size_t size) {
    char path[4096], what[512];
    uint8_t bytes[0x200];
    long n;
    memset(c, 0, sizeof *c);
    snprintf(path, sizeof path, "%s/rw/sys/LocalFriendCodeSeed_B", nand);
    if ((n = read_file(path, bytes, sizeof bytes, what, sizeof what)) < 0 ||
        snx_online_ctr_lfcs_read(c, bytes, (size_t)n, what, sizeof what))
        goto failed;
    snprintf(path, sizeof path, "%s/rw/sys/SecureInfo_A", nand);
    if ((n = read_file(path, bytes, sizeof bytes, what, sizeof what)) < 0 ||
        snx_online_ctr_secure_info_read(c, bytes, (size_t)n, what, sizeof what))
        goto failed;
    snprintf(path, sizeof path, "%s/data/00000000000000000000000000000000/sysdata/00010032/00000000/%u/account", nand,
             account);
    if ((n = read_file(path, bytes, sizeof bytes, what, sizeof what)) < 0 ||
        snx_online_ctr_account_read(c, bytes, (size_t)n, what, sizeof what))
        goto failed;
    return 0;
failed:
    snprintf(why, size, "%s (the file read: %s)", what, path);
    return -1;
}

/* UTF-16 units up to the NUL (max at most), as little endian bytes into out: their count */
static size_t utf16_bytes(const uint16_t* units, size_t max, uint8_t* out) {
    size_t n = 0;
    while (n < max && units[n]) {
        out[2 * n] = (uint8_t)units[n];
        out[2 * n + 1] = (uint8_t)(units[n] >> 8);
        ++n;
    }
    return 2 * n;
}

int snx_online_nasc_login_form(char* out, size_t cap, const SnxOnlineCtrConsole* c, const SnxOnlineCtrGame* g,
                               const SnxOnlineCtrPlace* p, char* why, size_t size) {
    SnxOnlineForm f;
    char text[64];
    uint8_t name[32];
    if (!c->pid || !c->uidhmac[0]) {
        snprintf(why, size, "the console has no account to log in with (principal ID %u)", (unsigned)c->pid);
        return -1;
    }
    snx_online_form_start(&f, out, cap);
    snprintf(text, sizeof text, "%08X", (unsigned)g->game_server_id);
    snx_online_form_text(&f, "gameid", text);
    snprintf(text, sizeof text, "%03u%03u", g->sdk_major, g->sdk_minor);
    snx_online_form_text(&f, "sdkver", text);
    snprintf(text, sizeof text, "%08X%08X", (unsigned)(g->title_id >> 32), (unsigned)g->title_id);
    snx_online_form_text(&f, "titleid", text);
    snx_online_form_text(&f, "gamecd", g->product_code);
    snprintf(text, sizeof text, "%04X", g->title_version);
    snx_online_form_text(&f, "gamever", text);
    snprintf(text, sizeof text, "%u", g->media_type);
    snx_online_form_text(&f, "mediatype", text);
    snx_online_form_text(&f, "makercd", g->maker_code);
    snx_online_form_text(&f, "unitcd", "2");
    snprintf(text, sizeof text, "%02x%02x%02x%02x%02x%02x", c->mac[0], c->mac[1], c->mac[2], c->mac[3], c->mac[4],
             c->mac[5]);
    snx_online_form_text(&f, "macadr", text);
    snx_online_form_text(&f, "bssid", text); /* the access point's: the console's own network here, no access point */
    snx_online_form_text(&f, "apinfo", "01:0000000000");
    snx_online_form_put(&f, "fcdcert", c->lfcs, sizeof c->lfcs);
    snx_online_form_put(&f, "devname", name, utf16_bytes(p->devname, 10, name));
    snx_online_form_text(&f, "servertype", "L1");
    snprintf(text, sizeof text, "%04X", p->fpd_version);
    snx_online_form_text(&f, "fpdver", text);
    snx_online_form_text(&f, "devtime", p->devtime);
    snprintf(text, sizeof text, "%02X", p->language);
    snx_online_form_text(&f, "lang", text);
    snprintf(text, sizeof text, "%02X", c->region);
    snx_online_form_text(&f, "region", text);
    snx_online_form_text(&f, "csnum", c->serial);
    snx_online_form_text(&f, "uidhmac", c->uidhmac);
    snprintf(text, sizeof text, "%u", (unsigned)c->pid);
    snx_online_form_text(&f, "userid", text);
    snx_online_form_text(&f, "action", "LOGIN");
    snx_online_form_put(&f, "ingamesn", name, utf16_bytes(g->ingamesn, 11, name));
    if (f.full) {
        snprintf(why, size, "the LOGIN form does not fit in %u bytes", (unsigned)cap);
        return -1;
    }
    return 0;
}

static void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> 8 * i);
}

void snx_online_ctr_game_auth(uint8_t out[SNX_ONLINE_CTR_GAME_AUTH_SIZE], const SnxOnlineNascReply* reply,
                              int http_status) {
    int result = snx_online_nasc_result(reply);
    uint64_t time = snx_online_nex_datetime(reply->datetime);
    memset(out, 0, SNX_ONLINE_CTR_GAME_AUTH_SIZE);
    put32(out, (uint32_t)(result < 0 ? 0 : result));
    put32(out + 0x04, (uint32_t)http_status);
    strncpy((char*)out + 0x08, reply->host, 0x1F);
    out[0x28] = (uint8_t)reply->port;
    out[0x29] = (uint8_t)(reply->port >> 8);
    strncpy((char*)out + 0x30, reply->token, 0xFF);
    put32(out + 0x130, (uint32_t)time);
    put32(out + 0x134, (uint32_t)(time >> 32));
}
