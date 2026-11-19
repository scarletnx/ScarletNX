/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 3DS on Pretendo Network: the console's own files a NASC login needs, the login's form, and the addresses
   Pretendo's Nimbus patches give the console's system modules (its patches/ in github.com/PretendoNetwork/Nimbus):
   the friends module's NASC URL, the account module's base URL, http:C's host rewrite and the socket module's NAT
   check hosts */
#ifndef SNX_ONLINE_CTR_H
#define SNX_ONLINE_CTR_H
#include "common/online/nasc.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SNX_ONLINE_PRETENDO_NASC_URL    "https://nasc.pretendo.cc/ac/"
#define SNX_ONLINE_PRETENDO_ACCOUNT_URL "https://account.pretendo.cc/v1/api/"

/* http:C's rewrite with a Pretendo account: the first nintendowifi.net, then the first nintendo.net, then the first
   pokemon-gl.com in url each become pretendo.cc, in that order, as Nimbus's patch does. The rewritten URL's length,
   or 0 when cap is short */
size_t snx_online_pretendo_url(const char* url, char* out, size_t cap);
/* The socket module's rewrite: nncs1.app.nintendowifi.net and nncs2.app.nintendowifi.net, the NAT check servers,
   become nncs1.app.pretendo.cc and nncs2.app.pretendo.cc; any other host stays */
const char* snx_online_pretendo_host(const char* host);

/* The console's files, as Azahar keeps them in its NAND folder: rw/sys/LocalFriendCodeSeed_B, rw/sys/SecureInfo_A,
   and the friends module's save (system save 00010032) at data/<32 zeros>/sysdata/00010032/00000000/, whose
   folder N holds local account N's account file: 1 the console's first account, 2 the one Nimbus makes for Pretendo */
#define SNX_ONLINE_CTR_LFCS_SIZE        0x110u /* the seed's signature (0x100) and its body (0x10) */
#define SNX_ONLINE_CTR_SECURE_INFO_SIZE 0x111u
#define SNX_ONLINE_CTR_ACCOUNT_SIZE     0x54u /* the account file's bytes this reads, up to the uidhmac's end */
#define SNX_ONLINE_CTR_PRETENDO_ACCOUNT 2u

typedef struct SnxOnlineCtrConsole {
    uint8_t lfcs[SNX_ONLINE_CTR_LFCS_SIZE]; /* LocalFriendCodeSeed_B, sent whole as NASC's fcdcert */
    char serial[16];                        /* SecureInfo_A's serial number */
    uint8_t region;                         /* SecureInfo_A's region */
    uint32_t pid;                           /* the account's principal ID */
    uint64_t friend_code;                   /* its local friend code */
    char uidhmac[9];                        /* the uidhmac the account's server gave it, sent with every login */
    uint8_t mac[6];                         /* the Wi-Fi's: one with a vendor prefix of the console's, kept per seed */
} SnxOnlineCtrConsole;

/* The parts, from their bytes: 0, or -1 with why saying what is wrong and the value read */
int snx_online_ctr_lfcs_read(SnxOnlineCtrConsole* c, const uint8_t* bytes, size_t length, char* why, size_t size);
int snx_online_ctr_secure_info_read(SnxOnlineCtrConsole* c, const uint8_t* bytes, size_t length, char* why,
                                    size_t size);
int snx_online_ctr_account_read(SnxOnlineCtrConsole* c, const uint8_t* bytes, size_t length, char* why, size_t size);
/* The three files from nand, local account `account`: 0, or -1 with why naming the file it could not take */
int snx_online_ctr_console_load(SnxOnlineCtrConsole* c, const char* nand, unsigned account, char* why, size_t size);
/* the Wi-Fi address kept for a seed: 40:F4:07 (a console vendor prefix NASC takes) and three bytes of its seed */
void snx_online_ctr_mac(SnxOnlineCtrConsole* c);

/* What the game asks of RequestGameAuthentication, and what the console knows of it */
typedef struct SnxOnlineCtrGame {
    uint32_t game_server_id; /* the request's server ID, NASC's gameid */
    uint64_t title_id;
    uint16_t title_version;
    char product_code[5]; /* the last four letters of the NCCH's product code */
    char maker_code[3];   /* the NCCH's maker code */
    uint8_t sdk_major, sdk_minor;
    uint16_t ingamesn[12]; /* the game's nickname for the player, UTF-16, NUL ended: 11 at most */
    uint8_t media_type;    /* 0 system, 1 digital, 2 card (NASC then wants the card's romid) */
} SnxOnlineCtrGame;
typedef struct SnxOnlineCtrPlace {
    uint8_t language;     /* the console's language code */
    uint16_t fpd_version; /* the friends module's version, NASC's fpdver */
    char devtime[13];     /* the console's local time, %y%m%d%H%M%S */
    uint16_t devname[11]; /* the console's user name, UTF-16, NUL ended */
} SnxOnlineCtrPlace;

/* The LOGIN form a friends module posts for the game: 0 with the form in out (a NUL after it), -1 with why */
int snx_online_nasc_login_form(char* out, size_t cap, const SnxOnlineCtrConsole* c, const SnxOnlineCtrGame* g,
                               const SnxOnlineCtrPlace* p, char* why, size_t size);

/* What the friends module gives the game after a LOGIN (GetGameAuthenticationData's 0x138 bytes, libctru's
   GameAuthenticationData): the NASC result, the HTTP status, the server, its port, the token and the server's time
   as NEX's DateTime, little endian */
#define SNX_ONLINE_CTR_GAME_AUTH_SIZE 0x138u
void snx_online_ctr_game_auth(uint8_t out[SNX_ONLINE_CTR_GAME_AUTH_SIZE], const SnxOnlineNascReply* reply,
                              int http_status);

#ifdef __cplusplus
}
#endif
#endif
