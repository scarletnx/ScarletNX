/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The console's base64, the form a NASC request is, and NASC's replies as Pretendo's NASC writes them (its
   src/services/nasc/routes/ac.ts and nascError in src/util.ts) */
#include "common/online/nasc.h"
#include "online_check.h"
#include <stdint.h>

static void b64n(void) {
    char out[64];
    uint8_t back[64];
    static const uint8_t odd[] = {0xFB, 0xFF};
    CHECK_INT(snx_online_b64n_encode(out, sizeof out, "LOGIN", 5), 8);
    CHECK_STR(out, "TE9HSU4*");
    snx_online_b64n_encode(out, sizeof out, "001", 3);
    CHECK_STR(out, "MDAx");
    snx_online_b64n_encode(out, sizeof out, odd, 2); /* the standard "+/8=" in the console's alphabet */
    CHECK_STR(out, ".-8*");
    CHECK_INT(snx_online_b64n_decode(back, sizeof back, ".-8*", 4), 2);
    CHECK(back[0] == 0xFB && back[1] == 0xFF);
    CHECK_INT(snx_online_b64n_decode(back, sizeof back, "+/8=", 4), 2); /* the standard alphabet, taken too */
    CHECK_INT(snx_online_b64n_decode(back, sizeof back, "TE9HSU4*", 8), 5);
    CHECK(!memcmp(back, "LOGIN", 5));
    CHECK_INT(snx_online_b64n_decode(back, sizeof back, "TE9H!U4*", 8), -1);
    CHECK_INT(snx_online_b64n_decode(back, 2, "TE9HSU4*", 8), -1); /* no room */
    CHECK_INT(snx_online_b64n_encode(out, 8, "LOGIN", 5), 0);      /* no room for the NUL */
    CHECK_INT(snx_online_b64n_encode(out, sizeof out, "", 0), 0);
    CHECK_STR(out, "");
}

static void form(void) {
    char buffer[64];
    SnxOnlineForm f;
    snx_online_form_start(&f, buffer, sizeof buffer);
    snx_online_form_text(&f, "action", "LOGIN");
    snx_online_form_text(&f, "unitcd", "2");
    CHECK(!f.full);
    CHECK_STR(buffer, "action=TE9HSU4*&unitcd=Mg**");
    snx_online_form_start(&f, buffer, 20);
    snx_online_form_text(&f, "action", "LOGIN");
    snx_online_form_text(&f, "unitcd", "2");
    CHECK(f.full);
}

static void login_reply(void) {
    /* LOGIN: locator 1.2.3.4:60000, retry 0, returncd 001, the token as it was made, datetime 2026-11-19 01:30:00 */
    char body[512], locator[64], date[32];
    snx_online_b64n_encode(locator, sizeof locator, "1.2.3.4:60000", 13);
    snx_online_b64n_encode(date, sizeof date, "20261119013000", 14);
    snprintf(body, sizeof body, "locator=%s&retry=MA**&returncd=MDAx&token=QUJDRA**&datetime=%s", locator, date);
    SnxOnlineNascReply r;
    char why[256] = "";
    CHECK_INT(snx_online_nasc_reply_read(body, strlen(body), &r, why, sizeof why), 0);
    CHECK_STR(r.returncd, "001");
    CHECK_INT(snx_online_nasc_result(&r), 1);
    CHECK_INT(r.retry, 0);
    CHECK_STR(r.host, "1.2.3.4");
    CHECK_INT(r.port, 60000);
    CHECK_STR(r.token, "QUJDRA**"); /* the game server hashes this text: it stays as it came */
    CHECK_STR(r.datetime, "20261119013000");
    /* URLSearchParams writes * as %2A where a browser's encoder does: the same reply */
    snprintf(body, sizeof body, "locator=%s&retry=MA%%2A%%2A&returncd=MDAx&token=QUJDRA%%2A%%2A&datetime=%s", locator,
             date);
    CHECK_INT(snx_online_nasc_reply_read(body, strlen(body), &r, why, sizeof why), 0);
    CHECK_STR(r.token, "QUJDRA**");
    CHECK_INT(r.port, 60000);
}

static void error_replies(void) {
    SnxOnlineNascReply r;
    char why[256] = "";
    /* nascError('null'): returncd written as the word null */
    static const char null_body[] = "retry=MQ**&returncd=null&datetime=MjAyNjExMTkwMTMwMDA*";
    CHECK_INT(snx_online_nasc_reply_read(null_body, sizeof null_body - 1, &r, why, sizeof why), 0);
    CHECK_STR(r.returncd, "null");
    CHECK_INT(snx_online_nasc_result(&r), -1);
    CHECK_INT(r.retry, 1);
    /* nascError('110'): no server for the title */
    static const char code_body[] = "retry=MQ**&returncd=MTEw&datetime=MjAyNjExMTkwMTMwMDA*";
    CHECK_INT(snx_online_nasc_reply_read(code_body, sizeof code_body - 1, &r, why, sizeof why), 0);
    CHECK_INT(snx_online_nasc_result(&r), 110);
    /* what is not NASC's says so */
    static const char html[] = "<html>403 Forbidden</html>";
    CHECK_INT(snx_online_nasc_reply_read(html, sizeof html - 1, &r, why, sizeof why), -1);
    CHECK_HAS(why, "no returncd: <html>403");
    static const char bad_locator[] = "returncd=MDAx&locator=MS4yLjMuNA**"; /* "1.2.3.4", no port */
    CHECK_INT(snx_online_nasc_reply_read(bad_locator, sizeof bad_locator - 1, &r, why, sizeof why), -1);
    CHECK_HAS(why, "locator=MS4yLjMuNA**");
}

static void datetime(void) {
    CHECK_INT(snx_online_nex_datetime("20261119013000"),
              (2026ull << 26) | (11ull << 22) | (19ull << 17) | (1ull << 12) | (30ull << 6) | 0);
    CHECK_INT(snx_online_nex_datetime("20261319013000"), 0); /* month 13 */
    CHECK_INT(snx_online_nex_datetime("2026111901300"), 0);
    CHECK_INT(snx_online_nex_datetime(NULL), 0);
}

int main(void) {
    b64n();
    form();
    login_reply();
    error_replies();
    datetime();
    CHECK_DONE();
}
