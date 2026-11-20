/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 3DS's side: Nimbus's rewrites, the console's files in Azahar's NAND layout, the LOGIN form read back field by
   field, and the game's authentication data at libctru's offsets */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L
#endif
#include "common/online/ctr.h"
#include "common/online/hosts.h"
#include "online_check.h"
#include <stdint.h>
#include <stdlib.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir(p, 0755)
#endif

static void rewrites(void) {
    char out[256];
    CHECK_INT(snx_online_pretendo_url("https://nasc.nintendowifi.net/ac", out, sizeof out), 27);
    CHECK_STR(out, "https://nasc.pretendo.cc/ac");
    snx_online_pretendo_url("https://nppl.app.nintendo.net/p01/policylist/1/1/UNK", out, sizeof out);
    CHECK_STR(out, "https://nppl.app.pretendo.cc/p01/policylist/1/1/UNK");
    snx_online_pretendo_url("https://3ds1.pokemon-gl.com/api/", out, sizeof out);
    CHECK_STR(out, "https://3ds1.pretendo.cc/api/");
    /* each name's first only, in Nimbus's order */
    snx_online_pretendo_url("https://a.nintendowifi.net/b.nintendo.net/c.nintendo.net", out, sizeof out);
    CHECK_STR(out, "https://a.pretendo.cc/b.pretendo.cc/c.nintendo.net");
    snx_online_pretendo_url("https://example.com/", out, sizeof out);
    CHECK_STR(out, "https://example.com/");
    CHECK_INT(snx_online_pretendo_url("https://example.com/", out, 10), 0);
    CHECK_STR(snx_online_pretendo_host("nncs1.app.nintendowifi.net"), "nncs1.app.pretendo.cc");
    CHECK_STR(snx_online_pretendo_host("nncs2.app.nintendowifi.net"), "nncs2.app.pretendo.cc");
    CHECK_STR(snx_online_pretendo_host("nncs3.app.nintendowifi.net"), "nncs3.app.nintendowifi.net");
}

/* the maker's domains: the domain and names under it, any case, a final dot; a name that only ends alike is not one */
static void maker_hosts(void) {
    CHECK(snx_online_maker_host("nasc.nintendowifi.net"));
    CHECK(snx_online_maker_host("NINTENDO.NET"));
    CHECK(snx_online_maker_host("account.nintendo.net."));
    CHECK(snx_online_maker_host("conntest.nintendowifi.net"));
    CHECK(!snx_online_maker_host("nasc.pretendo.cc"));
    CHECK(!snx_online_maker_host("notnintendo.net"));
    CHECK(!snx_online_maker_host("nintendo.network"));
    CHECK(!snx_online_maker_host(""));
    CHECK(!snx_online_maker_host(NULL));
}

/* a console's three files with known values, in Azahar's layout under folder */
static void make_console(const char* folder, uint8_t lfcs[SNX_ONLINE_CTR_LFCS_SIZE]) {
    static const char* const dirs[] = {"",
                                       "/rw",
                                       "/rw/sys",
                                       "/data",
                                       "/data/00000000000000000000000000000000",
                                       "/data/00000000000000000000000000000000/sysdata",
                                       "/data/00000000000000000000000000000000/sysdata/00010032",
                                       "/data/00000000000000000000000000000000/sysdata/00010032/00000000",
                                       "/data/00000000000000000000000000000000/sysdata/00010032/00000000/2"};
    char path[1024];
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; ++i) {
        snprintf(path, sizeof path, "%s%s", folder, dirs[i]);
        make_dir(path);
    }
    for (int i = 0; i < 0x110; ++i) lfcs[i] = (uint8_t)(i * 7 + 1);
    snprintf(path, sizeof path, "%s/rw/sys/LocalFriendCodeSeed_B", folder);
    FILE* f = fopen(path, "wb");
    fwrite(lfcs, 1, SNX_ONLINE_CTR_LFCS_SIZE, f);
    fclose(f);
    uint8_t info[SNX_ONLINE_CTR_SECURE_INFO_SIZE] = {0};
    info[0x100] = 1; /* USA */
    memcpy(info + 0x102, "CW412345678", 11);
    snprintf(path, sizeof path, "%s/rw/sys/SecureInfo_A", folder);
    f = fopen(path, "wb");
    fwrite(info, 1, sizeof info, f);
    fclose(f);
    uint8_t account[0x60] = {0};
    account[0x10] = 2;
    account[0x14] = 0x78, account[0x15] = 0x56, account[0x16] = 0x34, account[0x17] = 0x12;
    account[0x18] = 0xEF, account[0x1F] = 0x01;
    for (int i = 0; i < 8; ++i) account[0x42 + 2 * i] = (uint8_t)"aB3dEf9H"[i];
    snprintf(path, sizeof path, "%s/data/00000000000000000000000000000000/sysdata/00010032/00000000/2/account", folder);
    f = fopen(path, "wb");
    fwrite(account, 1, sizeof account, f);
    fclose(f);
}

/* the form's field key decoded from the console's base64 into out: its length, or -1 when the form has none */
static long field(const char* form, const char* key, uint8_t* out, size_t cap) {
    size_t k = strlen(key);
    for (const char* at = form; at && *at; at = strchr(at, '&') ? strchr(at, '&') + 1 : NULL) {
        if (strncmp(at, key, k) || at[k] != '=') continue;
        const char *value = at + k + 1, *end = strchr(value, '&');
        long n = snx_online_b64n_decode(out, cap - 1, value, end ? (size_t)(end - value) : strlen(value));
        if (n >= 0) out[n] = 0;
        return n;
    }
    return -1;
}

static void console(const char* folder) {
    SnxOnlineCtrConsole c;
    uint8_t lfcs[SNX_ONLINE_CTR_LFCS_SIZE];
    char why[1024] = "";
    make_console(folder, lfcs);
    CHECK_INT(snx_online_ctr_console_load(&c, folder, SNX_ONLINE_CTR_PRETENDO_ACCOUNT, why, sizeof why), 0);
    CHECK(!memcmp(c.lfcs, lfcs, sizeof lfcs));
    CHECK_STR(c.serial, "CW412345678");
    CHECK_INT(c.region, 1);
    CHECK_INT(c.pid, 0x12345678);
    CHECK_INT(c.friend_code, 0x01000000000000EFull);
    CHECK_STR(c.uidhmac, "aB3dEf9H");
    CHECK(c.mac[0] == 0x40 && c.mac[1] == 0xF4 && c.mac[2] == 0x07);
    /* local account 1 is not there: the path says which */
    CHECK_INT(snx_online_ctr_console_load(&c, folder, 1, why, sizeof why), -1);
    CHECK_HAS(why, "00010032/00000000/1/account");
    CHECK_HAS(why, "cannot be opened");

    /* LOGIN, read back as a NASC server reads it */
    SnxOnlineCtrConsole ok;
    snx_online_ctr_console_load(&ok, folder, SNX_ONLINE_CTR_PRETENDO_ACCOUNT, why, sizeof why);
    SnxOnlineCtrGame g = {0};
    SnxOnlineCtrPlace p = {0};
    g.game_server_id = 0x00001234;
    g.title_id = 0x0004000000ABCD00ull;
    g.title_version = 0x0410;
    memcpy(g.product_code, "ABCE", 5);
    memcpy(g.maker_code, "01", 3);
    g.media_type = 1;
    g.ingamesn[0] = 'R', g.ingamesn[1] = 'e', g.ingamesn[2] = 'd';
    p.language = 1;
    p.fpd_version = 0x000F;
    memcpy(p.devtime, "261119013000", 13);
    p.devname[0] = 'S', p.devname[1] = 'N', p.devname[2] = 'X';
    char form[4096];
    uint8_t v[512];
    CHECK_INT(snx_online_nasc_login_form(form, sizeof form, &ok, &g, &p, why, sizeof why), 0);
    CHECK_INT(field(form, "action", v, sizeof v), 5);
    CHECK_STR((char*)v, "LOGIN");
    field(form, "gameid", v, sizeof v);
    CHECK_STR((char*)v, "00001234");
    field(form, "titleid", v, sizeof v);
    CHECK_STR((char*)v, "0004000000ABCD00");
    field(form, "gamever", v, sizeof v);
    CHECK_STR((char*)v, "0410");
    field(form, "sdkver", v, sizeof v);
    CHECK_STR((char*)v, "000000");
    field(form, "userid", v, sizeof v);
    CHECK_STR((char*)v, "305419896");
    field(form, "uidhmac", v, sizeof v);
    CHECK_STR((char*)v, "aB3dEf9H");
    field(form, "csnum", v, sizeof v);
    CHECK_STR((char*)v, "CW412345678");
    field(form, "servertype", v, sizeof v);
    CHECK_STR((char*)v, "L1");
    field(form, "unitcd", v, sizeof v);
    CHECK_STR((char*)v, "2");
    field(form, "lang", v, sizeof v);
    CHECK_STR((char*)v, "01");
    field(form, "region", v, sizeof v);
    CHECK_STR((char*)v, "01");
    field(form, "macadr", v, sizeof v);
    CHECK_INT(strlen((char*)v), 12);
    CHECK(!strncmp((char*)v, "40f407", 6));
    CHECK_INT(field(form, "fcdcert", v, sizeof v), 0x110);
    CHECK(!memcmp(v, lfcs, 0x110));
    CHECK_INT(field(form, "ingamesn", v, sizeof v), 6);
    CHECK(v[0] == 'R' && v[1] == 0 && v[4] == 'd' && v[5] == 0);
    CHECK_INT(field(form, "devname", v, sizeof v), 6);
    CHECK_INT(field(form, "passwd", v, sizeof v), -1); /* a login sends no password */
    CHECK_INT(snx_online_nasc_login_form(form, 100, &ok, &g, &p, why, sizeof why), -1);
    CHECK_HAS(why, "does not fit in 100 bytes");
    SnxOnlineCtrConsole empty = {0};
    CHECK_INT(snx_online_nasc_login_form(form, sizeof form, &empty, &g, &p, why, sizeof why), -1);
    CHECK_HAS(why, "principal ID 0");
}

static void parts(void) {
    SnxOnlineCtrConsole c = {0};
    uint8_t bytes[0x200] = {0};
    char why[256] = "";
    CHECK_INT(snx_online_ctr_lfcs_read(&c, bytes, 0x10F, why, sizeof why), -1);
    CHECK_HAS(why, "has 271 bytes");
    CHECK_INT(snx_online_ctr_lfcs_read(&c, bytes, 0x110, why, sizeof why), -1);
    CHECK_HAS(why, "all zeros");
    CHECK_INT(snx_online_ctr_secure_info_read(&c, bytes, 0x111, why, sizeof why), -1);
    CHECK_HAS(why, "(0 read)");
    CHECK_INT(snx_online_ctr_account_read(&c, bytes, 0x40, why, sizeof why), -1);
    CHECK_HAS(why, "has 64 bytes");
    bytes[0x14] = 1;
    CHECK_INT(snx_online_ctr_account_read(&c, bytes, 0x54, why, sizeof why), -1);
    CHECK_HAS(why, "0x0000 as its letter 1");
}

static void game_auth(void) {
    SnxOnlineNascReply r;
    uint8_t out[SNX_ONLINE_CTR_GAME_AUTH_SIZE];
    char why[128];
    static const char body[] = "locator=MTAuMC4wLjI6NjAwMDA*&retry=MA**&returncd=MDAx&token=VE9LRU4*&datetime="
                               "MjAyNjExMTkwMTMwMDA*";
    CHECK_INT(snx_online_nasc_reply_read(body, sizeof body - 1, &r, why, sizeof why), 0);
    CHECK_STR(r.host, "10.0.0.2");
    snx_online_ctr_game_auth(out, &r, 200);
    CHECK_INT(out[0], 1);
    CHECK_INT(out[4] | out[5] << 8, 200);
    CHECK_STR((char*)out + 0x08, "10.0.0.2");
    CHECK_INT(out[0x28] | out[0x29] << 8, 60000);
    CHECK_STR((char*)out + 0x30, "VE9LRU4*");
    uint64_t time = 0;
    for (int i = 7; i >= 0; --i) time = time << 8 | out[0x130 + i];
    CHECK_INT(time, snx_online_nex_datetime("20261119013000"));
}

int main(int argc, char** argv) {
    rewrites();
    maker_hosts();
    console(argc > 1 ? argv[1] : "ctr_console");
    parts();
    game_auth();
    CHECK_DONE();
}
