/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* NASC, the service a 3DS's friends module asks for a game server's address and a token to log in to it with: an
   HTTP POST of a form whose every value is in the console's base64 (the alphabet's + / = written . - *), and a form
   back. The fields are the ones Pretendo Network's NASC (its account server's src/services/nasc) reads and writes */
#ifndef SNX_ONLINE_NASC_H
#define SNX_ONLINE_NASC_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The console's base64 of length bytes into out with a NUL: its length, or 0 when cap is short (an empty input's is 0
   too, with out "") */
size_t snx_online_b64n_encode(char* out, size_t cap, const void* data, size_t length);
/* The bytes of text (length bytes) into out: their count, or -1 for text that is not the console's base64 or a cap
   too short. The standard alphabet's + / = are taken too */
long snx_online_b64n_decode(uint8_t* out, size_t cap, const char* text, size_t length);

/* A form being written: key=value fields, & between, each value the console's base64 of its bytes */
typedef struct SnxOnlineForm {
    char* text;
    size_t length, cap;
    int full; /* a field did not fit: the form is unusable */
} SnxOnlineForm;
void snx_online_form_start(SnxOnlineForm* f, char* buffer, size_t cap);
void snx_online_form_put(SnxOnlineForm* f, const char* key, const void* value, size_t length);
void snx_online_form_text(SnxOnlineForm* f, const char* key, const char* value); /* a C string's bytes */

/* A NASC reply */
typedef struct SnxOnlineNascReply {
    char returncd[8];  /* "001" for a LOGIN that worked, "007" for a SVCLOC, "null" when the server named none */
    int retry;         /* 1 when the console should ask again later */
    char host[64];     /* the locator's address, LOGIN's */
    uint16_t port;     /* the locator's port */
    char token[512];   /* the token, LOGIN's, as it came: the game server checks this text */
    char datetime[16]; /* the server's time, YYYYMMDDHHMMSS */
    char service_token[512], service_host[128]; /* SVCLOC's */
    uint8_t status_data;
} SnxOnlineNascReply;
/* Reads a NASC reply body: 0 with out filled, or -1 with why naming the field that is wrong */
int snx_online_nasc_reply_read(const char* body, size_t length, SnxOnlineNascReply* out, char* why, size_t size);
/* returncd as a number: 1 for "001"; -1 for "null" or a code that is not a number */
int snx_online_nasc_result(const SnxOnlineNascReply* reply);

/* NEX's DateTime of a YYYYMMDDHHMMSS time: year << 26 | month << 22 | day << 17 | hour << 12 | minute << 6 | second.
   0 for a text that is not one */
uint64_t snx_online_nex_datetime(const char* yyyymmddhhmmss);

#ifdef __cplusplus
}
#endif
#endif
