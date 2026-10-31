/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* common/network/https.c against servers in the test, on 127.0.0.1: a body by Content-Length, chunked and up to the
   close; a redirect to the same origin, one to another and one that loops past the most followed; a body over the
   most taken and one cut short refused with their bytes; a status other than 200 given back with its body; the
   progress said and a stop taken; TLS with a certificate the roots have (SSL_CERT_FILE), for a name and for an
   address, and one they do not have refused saying so */
#include "common/network/https.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>
#define getpid _getpid
typedef SOCKET Sock;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int Sock;
#endif
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

/* a server: its listening socket, its port, and its TLS context (NULL: plain http) */
typedef struct {
    Sock listener;
    unsigned port;
    SSL_CTX* tls;
} Server;
static struct {
    Server plain, other, trusted, untrusted;
    int stops; /* the progress calls before the one that stops */
} g_test;

static void sock_close(Sock s) {
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}
/* a self-signed certificate for localhost and 127.0.0.1, its key in *key */
static X509* make_cert(EVP_PKEY** key) {
    X509V3_CTX v3;
    X509_EXTENSION* san;
    X509_NAME* name;
    X509* cert;
    *key = EVP_EC_gen("P-256");
    assert(*key);
    cert = X509_new();
    assert(cert && X509_set_version(cert, X509_VERSION_3));
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert), -3600);
    X509_gmtime_adj(X509_getm_notAfter(cert), 86400);
    assert(X509_set_pubkey(cert, *key));
    name = X509_NAME_new();
    assert(name && X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char*)"localhost", -1, -1, 0));
    assert(X509_set_subject_name(cert, name) && X509_set_issuer_name(cert, name));
    X509_NAME_free(name);
    X509V3_set_ctx(&v3, cert, cert, NULL, NULL, 0);
    san = X509V3_EXT_conf_nid(NULL, &v3, NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1");
    assert(san && X509_add_ext(cert, san, -1));
    X509_EXTENSION_free(san);
    assert(X509_sign(cert, *key, EVP_sha256()) > 0);
    return cert;
}
static SSL_CTX* tls_of(X509* cert, EVP_PKEY* key) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    assert(ctx && SSL_CTX_use_certificate(ctx, cert) == 1 && SSL_CTX_use_PrivateKey(ctx, key) == 1);
    return ctx;
}
static void listen_on(Server* s, SSL_CTX* tls) {
    struct sockaddr_in a = {0};
    socklen_t len = sizeof a;
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    s->listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(!bind(s->listener, (struct sockaddr*)&a, sizeof a) && !listen(s->listener, 8));
    assert(!getsockname(s->listener, (struct sockaddr*)&a, &len));
    s->port = ntohs(a.sin_port);
    s->tls = tls;
}
/* the answer to path, as raw bytes: n of them in *n */
static const char* answer(const char* path, size_t* n, char* room, size_t size) {
    static const char chunked[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                                  "5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\n\r\n";
    if (!strcmp(path, "/len"))
        snprintf(room, size, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain\r\n\r\nhello");
    else if (!strcmp(path, "/chunk"))
        snprintf(room, size, "%s", chunked);
    else if (!strcmp(path, "/close"))
        snprintf(room, size, "HTTP/1.1 200 OK\r\n\r\nuntil the close");
    else if (!strcmp(path, "/rel"))
        snprintf(room, size, "HTTP/1.1 301 Moved\r\nLocation: /len\r\nContent-Length: 3\r\n\r\nbye");
    else if (!strcmp(path, "/away")) /* to the other server, which has the asset */
        snprintf(room, size, "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:%u/len?x=1\r\n\r\n", g_test.other.port);
    else if (!strcmp(path, "/loop"))
        snprintf(room, size, "HTTP/1.1 302 Found\r\nLocation: loop\r\n\r\n");
    else if (!strcmp(path, "/big"))
        snprintf(room, size, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n%0100d", 0);
    else if (!strcmp(path, "/short"))
        snprintf(room, size, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello");
    else if (!strcmp(path, "/signed")) { /* as a CDN's signed URL: past 2000 bytes */
        int k = snprintf(room, size, "HTTP/1.1 302 Found\r\nLocation: /len?sig=");
        memset(room + k, 'a', 2200);
        snprintf(room + k + 2200, size - (size_t)k - 2200, "\r\n\r\n");
    } else if (!strncmp(path, "/len?sig=aaaa", 13) && strlen(path) == 9 + 2200)
        snprintf(room, size, "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nsigned");
    else if (!strcmp(path, "/len?x=1"))
        snprintf(room, size, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nasset");
    else
        snprintf(room, size, "HTTP/1.1 404 Not Found\r\nContent-Length: 2\r\n\r\nno");
    *n = strlen(room);
    return room;
}
/* each connection: its request's path answered, then closed (Connection: close); until the listener is closed */
#ifdef _WIN32
static DWORD WINAPI serve(void* arg) {
#else
static void* serve(void* arg) {
#endif
    Server* s = (Server*)arg;
    for (;;) {
        char req[8192], room[3000], path[4096];
        size_t n = 0, out = 0;
        const char* reply;
        SSL* ssl = NULL;
        Sock c = accept(s->listener, NULL, NULL);
#ifdef _WIN32
        if (c == INVALID_SOCKET) break;
#else
        if (c < 0) break;
#endif
        if (s->tls) {
            ssl = SSL_new(s->tls);
            SSL_set_fd(ssl, (int)c);
            if (SSL_accept(ssl) != 1) { /* a client that does not trust it goes away */
                SSL_free(ssl);
                ERR_clear_error();
                sock_close(c);
                continue;
            }
        }
        while (n < sizeof req - 1) {
            int r = ssl ? SSL_read(ssl, req + n, (int)(sizeof req - 1 - n))
                        : (int)recv(c, req + n, (int)(sizeof req - 1 - n), 0);
            if (r <= 0) break;
            n += (size_t)r;
            req[n] = 0;
            if (strstr(req, "\r\n\r\n")) break;
        }
        req[n] = 0;
        if (sscanf(req, "GET %4095s HTTP/1.1", path) == 1 && strstr(req, "\r\nUser-Agent: ScarletNX\r\n")) {
            reply = answer(path, &out, room, sizeof room);
            if (ssl)
                SSL_write(ssl, reply, (int)out);
            else
                send(c, reply, (int)out, 0);
        }
        if (ssl) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
        }
        sock_close(c);
    }
    return 0;
}
static void start(Server* s) {
#ifdef _WIN32
    CloseHandle(CreateThread(NULL, 0, serve, s, 0, NULL));
#else
    pthread_t t;
    assert(!pthread_create(&t, NULL, serve, s));
    pthread_detach(t);
#endif
}
static int counting(void* user, unsigned long long got, unsigned long long total) {
    unsigned long long* last = (unsigned long long*)user;
    assert(total == 0 || total >= got);
    last[0] = got;
    last[1] = total;
    return 0;
}
static int stopping(void* user, unsigned long long got, unsigned long long total) {
    (void)user, (void)got, (void)total;
    return g_test.stops-- <= 0;
}
static int get(const char* url, const SnxHttpsOptions* o, SnxHttpsResponse* r, char* why) {
    return snx_https_get(url, o, r, why, 600);
}

int main(void) {
    char url[256], why[600], certs[256];
    SnxHttpsResponse r;
    SnxHttpsOptions o = {0};
    unsigned long long seen[2] = {0, 0};
    EVP_PKEY *key, *other_key;
    X509 *cert = make_cert(&key), *other_cert = make_cert(&other_key);
    FILE* f;
#ifdef _WIN32
    WSADATA wsa;
    assert(!WSAStartup(MAKEWORD(2, 2), &wsa));
#else
    signal(SIGPIPE,
           SIG_IGN); /* a server here writing to a client that hung up (a refused certificate) fails that write */
#endif
    /* the roots: the trusted server's certificate alone, as SSL_CERT_FILE names it */
    snprintf(certs, sizeof certs, "https-test-roots-%d.pem", (int)getpid());
    f = fopen(certs, "w");
    assert(f && PEM_write_X509(f, cert) == 1 && !fclose(f));
#ifdef _WIN32
    _putenv_s("SSL_CERT_FILE", certs);
#else
    setenv("SSL_CERT_FILE", certs, 1);
#endif
    assert(!strcmp(snx_https_init(), certs));
    listen_on(&g_test.plain, NULL);
    listen_on(&g_test.other, NULL);
    listen_on(&g_test.trusted, tls_of(cert, key));
    listen_on(&g_test.untrusted, tls_of(other_cert, other_key));
    start(&g_test.plain);
    start(&g_test.other);
    start(&g_test.trusted);
    start(&g_test.untrusted);
    o.timeout_s = 10;
    /* a body by Content-Length, chunked, and up to the close */
    snprintf(url, sizeof url, "http://127.0.0.1:%u/len", g_test.plain.port);
    o.progress = counting;
    o.user = seen;
    assert(!get(url, &o, &r, why) && r.status == 200 && r.size == 5 && !strcmp(r.body, "hello") && !strcmp(r.url, url));
    assert(seen[0] == 5 && seen[1] == 5); /* the progress, the whole known */
    free(r.body);
    o.progress = NULL;
    snprintf(url, sizeof url, "http://127.0.0.1:%u/chunk", g_test.plain.port);
    assert(!get(url, &o, &r, why) && r.status == 200 && !strcmp(r.body, "hello world") && r.size == 11);
    free(r.body);
    snprintf(url, sizeof url, "http://127.0.0.1:%u/close", g_test.plain.port);
    assert(!get(url, &o, &r, why) && !strcmp(r.body, "until the close"));
    free(r.body);
    /* a status other than 200: given back, with its body */
    snprintf(url, sizeof url, "http://127.0.0.1:%u/missing", g_test.plain.port);
    assert(!get(url, &o, &r, why) && r.status == 404 && !strcmp(r.body, "no"));
    free(r.body);
    /* redirects: to the same origin, to another one (the URL that answered said), and one past the most followed */
    snprintf(url, sizeof url, "http://127.0.0.1:%u/rel", g_test.plain.port);
    assert(!get(url, &o, &r, why) && r.status == 200 && !strcmp(r.body, "hello"));
    free(r.body);
    snprintf(url, sizeof url, "http://127.0.0.1:%u/away", g_test.plain.port);
    assert(!get(url, &o, &r, why) && r.status == 200 && !strcmp(r.body, "asset"));
    snprintf(why, sizeof why, "http://127.0.0.1:%u/len?x=1", g_test.other.port);
    assert(!strcmp(r.url, why));
    free(r.body);
    snprintf(url, sizeof url, "http://127.0.0.1:%u/signed", g_test.plain.port); /* a long redirect, followed whole */
    assert(!get(url, &o, &r, why) && r.status == 200 && !strcmp(r.body, "signed") && strlen(r.url) > 2200);
    free(r.body);
    snprintf(url, sizeof url, "http://127.0.0.1:%u/loop", g_test.plain.port);
    o.redirects = 3;
    assert(!get(url, &o, &r, why) && r.status == 302 && !r.size); /* the fourth 302 comes back as it is */
    free(r.body);
    o.redirects = -1;
    snprintf(url, sizeof url, "http://127.0.0.1:%u/rel", g_test.plain.port);
    assert(!get(url, &o, &r, why) && r.status == 301);
    free(r.body);
    o.redirects = 0;
    /* over the most taken, cut short, stopped: refused, saying the bytes */
    o.body_max = 50;
    snprintf(url, sizeof url, "http://127.0.0.1:%u/big", g_test.plain.port);
    assert(get(url, &o, &r, why) && !r.body && strstr(why, "its body is 100 bytes, over the 50 this takes"));
    o.body_max = 0;
    snprintf(url, sizeof url, "http://127.0.0.1:%u/short", g_test.plain.port);
    assert(get(url, &o, &r, why) && strstr(why, "5 of its 10 bytes came"));
    o.progress = stopping;
    g_test.stops = 0;
    snprintf(url, sizeof url, "http://127.0.0.1:%u/len", g_test.plain.port);
    assert(get(url, &o, &r, why) && strstr(why, ": stopped, 5 bytes in"));
    o.progress = NULL;
    /* TLS: trusted for its name and its address; another certificate refused, saying the roots */
    snprintf(url, sizeof url, "https://localhost:%u/len", g_test.trusted.port);
    assert(!get(url, &o, &r, why) && r.status == 200 && !strcmp(r.body, "hello"));
    free(r.body);
    snprintf(url, sizeof url, "https://127.0.0.1:%u/chunk", g_test.trusted.port);
    assert(!get(url, &o, &r, why) && !strcmp(r.body, "hello world"));
    free(r.body);
    snprintf(url, sizeof url, "https://localhost:%u/len", g_test.untrusted.port);
    assert(get(url, &o, &r, why) && strstr(why, "localhost: its certificate is not trusted (") && strstr(why, certs));
    /* what is no URL, a host that does not resolve, a port nothing listens on */
    assert(get("ftp://x/y", &o, &r, why) && strstr(why, "ftp://x/y is not a URL this client takes"));
    assert(get("http://127.0.0.1:99999/", &o, &r, why) && strstr(why, "is not a URL"));
    assert(get("http://snx-no-such-host.invalid/", &o, &r, why) &&
           strstr(why, "snx-no-such-host.invalid does not resolve: "));
    {
        Server none; /* a port bound and let go, without a listen: nothing answers there */
        listen_on(&none, NULL);
        sock_close(none.listener);
        snprintf(url, sizeof url, "http://127.0.0.1:%u/", none.port);
    }
    assert(get(url, &o, &r, why) && strstr(why, "connecting to 127.0.0.1:"));
    remove(certs);
    puts("https: ok");
    return 0;
}
