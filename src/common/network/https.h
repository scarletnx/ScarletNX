/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An HTTP GET through libcurl (cmake/curl.cmake), TLS by the build's OpenSSL verified against Mozilla's root
   certificates the build carries (cmake/cacert.cmake), as Eden does with its bundled OpenSSL, or the file
   SSL_CERT_FILE names: the launcher's downloads (GPU driver packages, their releases' lists). Redirects followed to any
   origin, as a release's asset is served from another host than its page, nothing secret being sent (no cookie, no
   Authorization); the whole exchange within a time and a size the caller gives */
#ifndef SNX_COMMON_HTTPS_H
#define SNX_COMMON_HTTPS_H
#include <stddef.h>

/* the body's progress: the bytes come so far, and the whole (0 when the server says none); nonzero stops it */
typedef int (*SnxHttpsProgress)(void* user, unsigned long long got, unsigned long long total);
typedef struct {
    const char* accept;          /* the Accept field; NULL: any */
    unsigned timeout_s;          /* each request's whole exchange, a redirect's its own; 0: 30 s */
    unsigned long long body_max; /* the most bytes of the body taken; 0: 16 MiB */
    int redirects;               /* the most followed; 0: 5, -1: none */
    SnxHttpsProgress progress;   /* NULL: none said */
    void* user;
} SnxHttpsOptions;
typedef struct {
    int status; /* the last answer's: a redirect past the most followed comes back as its own */
    char* body; /* its body, a NUL after it: owned, free() */
    size_t size;
    char url[4096]; /* the URL that answered, past the redirects: a CDN's signed one runs past 1500 bytes */
} SnxHttpsResponse;

/* What every call needs, made once before any thread calls: libcurl's global setup. Which roots a transfer trusts,
   for the log */
const char* snx_https_init(void);
/* GET url (https:// or http://): 0 with out filled, whatever its status, else -1 with why saying what failed and the
   values it had (the host, the bytes, the time). out->body is NULL after a failure */
int snx_https_get(const char* url, const SnxHttpsOptions* o, SnxHttpsResponse* out, char* why, size_t size);
#endif
