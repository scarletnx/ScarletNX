/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/network/https.h"
#include "common/network/cacert.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HTTPS_DEFAULT_TIMEOUT_S 30
#define HTTPS_DEFAULT_BODY_MAX  (16ull << 20)
#define HTTPS_DEFAULT_REDIRECTS 5

static struct {
    int ready;
    CURLcode global;  /* curl_global_init's */
    const char* file; /* SSL_CERT_FILE's roots, NULL for the bundled ones */
    char roots[1200]; /* which roots, for the log and a refused certificate's message */
} g_https;

const char* snx_https_init(void) {
    const char* file;
    if (g_https.ready) return g_https.roots;
    g_https.global = curl_global_init(CURL_GLOBAL_DEFAULT);
    file = getenv("SSL_CERT_FILE"); /* as OpenSSL's own tools take it: a proxy's root, the tests' */
    if (file && file[0] && strlen(file) < sizeof g_https.roots) {
        memcpy(g_https.roots, file, strlen(file) + 1);
        g_https.file = g_https.roots;
    } else {
        size_t count = 0;
        const char* at = (const char*)snx_cacert_pem;
        while ((at = strstr(at, "-----BEGIN CERTIFICATE-----"))) {
            ++count;
            ++at;
        }
        snprintf(g_https.roots, sizeof g_https.roots, "Mozilla's %zu root certificates of %s, curl's extract", count,
                 snx_cacert_date);
    }
    g_https.ready = 1;
    return g_https.roots;
}

/* the body as it comes, up to the most taken; the caller's progress said after each piece */
typedef struct {
    CURL* curl;
    const SnxHttpsOptions* o;
    char* body;
    size_t size, cap;
    unsigned long long max;
    int over, stopped, no_memory;
} Sink;

static unsigned long long length_said(CURL* curl) {
    curl_off_t n = -1;
    return curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &n) == CURLE_OK && n > 0 ? (unsigned long long)n
                                                                                                : 0;
}
static size_t take(char* p, size_t one, size_t count, void* user) {
    Sink* k = (Sink*)user;
    size_t n = one * count;
    if (k->size + n > k->max) {
        k->over = 1;
        return 0;
    }
    if (k->size + n + 1 > k->cap) {
        size_t cap = k->cap * 2 > k->size + n + 1 ? k->cap * 2 : k->size + n + 1;
        char* more = (char*)realloc(k->body, cap);
        if (!more) {
            k->no_memory = 1;
            return 0;
        }
        k->body = more;
        k->cap = cap;
    }
    memcpy(k->body + k->size, p, n);
    k->size += n;
    k->body[k->size] = 0;
    if (k->o && k->o->progress && k->o->progress(k->o->user, k->size, length_said(k->curl))) {
        k->stopped = 1;
        return 0;
    }
    return n;
}

/* why a transfer failed, by curl's code and what the sink saw, with the values it had */
static void failed(CURL* curl, CURLcode rc, const char* url, const char* detail, const Sink* k, unsigned timeout,
                   char* why, size_t size) {
    char *host = NULL, *port = NULL;
    CURLU* u = curl_url();
    const char* named;
    if (u && !curl_url_set(u, CURLUPART_URL, url, 0)) {
        curl_url_get(u, CURLUPART_HOST, &host, 0);
        curl_url_get(u, CURLUPART_PORT, &port, CURLU_DEFAULT_PORT);
    }
    named = host ? host : url;
    if (k->over && length_said(curl))
        snprintf(why, size, "GET %s: its body is %llu bytes, over the %llu this takes", url, length_said(curl), k->max);
    else if (k->over)
        snprintf(why, size, "GET %s: its body is over the %llu bytes this takes", url, k->max);
    else if (k->stopped)
        snprintf(why, size, "GET %s: stopped, %zu bytes in", url, k->size);
    else if (k->no_memory)
        snprintf(why, size, "GET %s: no memory for %zu bytes of its answer", url, k->size);
    else if (rc == CURLE_COULDNT_RESOLVE_HOST)
        snprintf(why, size, "%s does not resolve: %s", named, detail);
    else if (rc == CURLE_COULDNT_CONNECT)
        snprintf(why, size, "connecting to %s:%s failed: %s", named, port ? port : "?", detail);
    else if (rc == CURLE_PEER_FAILED_VERIFICATION)
        snprintf(why, size, "%s: its certificate is not trusted (%s; roots: %s)", named, detail, g_https.roots);
    else if (rc == CURLE_SSL_CONNECT_ERROR)
        snprintf(why, size, "%s: the TLS handshake failed: %s", named, detail);
    else if (rc == CURLE_OPERATION_TIMEDOUT)
        snprintf(why, size, "GET %s: no whole answer within %u s, %zu bytes in", url, timeout, k->size);
    else if (rc == CURLE_PARTIAL_FILE)
        snprintf(why, size, "GET %s: %zu of its %llu bytes came", url, k->size, length_said(curl));
    else
        snprintf(why, size, "GET %s: %s (curl's error %d)", url, detail, (int)rc);
    curl_free(host);
    curl_free(port);
    curl_url_cleanup(u);
}

/* the request's end: what it allocated let go, -1 to hand back when the body went too */
static int finish(CURL* curl, struct curl_slist* head, char* body) {
    free(body);
    curl_slist_free_all(head);
    curl_easy_cleanup(curl);
    return -1;
}

int snx_https_get(const char* url, const SnxHttpsOptions* o, SnxHttpsResponse* out, char* why, size_t size) {
    unsigned timeout = o && o->timeout_s ? o->timeout_s : HTTPS_DEFAULT_TIMEOUT_S;
    int redirects = !o || !o->redirects ? HTTPS_DEFAULT_REDIRECTS : o->redirects < 0 ? 0 : o->redirects, hop;
    char accept[320], detail[CURL_ERROR_SIZE];
    struct curl_slist* head = NULL;
    struct curl_blob roots;
    Sink k = {0};
    CURL* curl;
    CURLU* check;
    memset(out, 0, sizeof *out);
    snx_https_init();
    if (strlen(url) >= sizeof out->url) {
        snprintf(why, size, "%.200s...: the URL is over %zu bytes", url, sizeof out->url - 1);
        return -1;
    }
    check = curl_url();
    if ((strncmp(url, "https://", 8) && strncmp(url, "http://", 7)) || !check ||
        curl_url_set(check, CURLUPART_URL, url, 0)) {
        snprintf(why, size, "%s is not a URL this client takes (http:// or https://, a host, a port up to 65535)", url);
        curl_url_cleanup(check);
        return -1;
    }
    curl_url_cleanup(check);
    if (g_https.global != CURLE_OK || !(curl = curl_easy_init())) {
        snprintf(why, size, "GET %s: libcurl did not start (%s)", url,
                 curl_easy_strerror(g_https.global != CURLE_OK ? g_https.global : CURLE_FAILED_INIT));
        return -1;
    }
    snprintf(accept, sizeof accept, "Accept: %s", o && o->accept ? o->accept : "*/*");
    head = curl_slist_append(head, accept);
    memcpy(out->url, url, strlen(url) + 1);
    k.curl = curl;
    k.o = o;
    k.max = o && o->body_max ? o->body_max : HTTPS_DEFAULT_BODY_MAX;
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ScarletNX");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, head);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, detail);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, take);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &k);
    if (g_https.file)
        curl_easy_setopt(curl, CURLOPT_CAINFO, g_https.file);
    else {
        roots.data = (void*)snx_cacert_pem;
        roots.len = snx_cacert_pem_size;
        roots.flags = CURL_BLOB_NOCOPY;
        curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &roots);
    }
    /* the redirects followed here, not by libcurl: past the most, the last answer is given back as it is */
    for (hop = 0;; hop++) {
        CURLcode rc;
        long status = 0;
        char* next = NULL;
        detail[0] = 0;
        k.size = 0;
        curl_easy_setopt(curl, CURLOPT_URL, out->url);
        rc = curl_easy_perform(curl);
        if (rc != CURLE_OK) {
            failed(curl, rc, out->url, detail[0] ? detail : curl_easy_strerror(rc), &k, timeout, why, size);
            return finish(curl, head, k.body);
        }
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &next);
        out->status = (int)status;
        if (!next || hop >= redirects) break;
        if (strlen(next) >= sizeof out->url) {
            snprintf(why, size, "GET %s: its redirect leads to %zu bytes of URL, over %zu", out->url, strlen(next),
                     sizeof out->url - 1);
            return finish(curl, head, k.body);
        }
        memmove(out->url, next, strlen(next) + 1);
    }
    if (!k.body && !(k.body = (char*)calloc(1, 1))) {
        snprintf(why, size, "GET %s: no memory for its empty body", out->url);
        return finish(curl, head, NULL);
    }
    out->body = k.body;
    out->size = k.size;
    curl_slist_free_all(head);
    curl_easy_cleanup(curl);
    return 0;
}
