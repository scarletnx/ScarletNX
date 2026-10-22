/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   ssl, the console's TLS service, with OpenSSL over the game's own sockets. The console's sslsrv is NSS in a process
   of its own: a connection duplicates the descriptor the game hands it out of the game's bsd table
   (SetSocketDescriptor) and runs TLS on that duplicate. Here the duplicate is a reference to the same SnxSocket
   (bsd_client_socket), and OpenSSL reads and writes it through a BIO over the stack's calls, never through a host
   descriptor: the stack keeps those private, and its waits watch the cancel pipe. The BIO never blocks, whatever the
   socket's own mode (the game shares the socket), and a Blocking connection waits in snx_net_poll, as the stack's own
   calls do.

   Layouts from Switchbrew's SSL services page, libnx's ssl.c and the SDK's client templates (a game's SDK). Result
   descriptions from Switchbrew's error codes (module 123) and Ryujinx's ResultCode.cs (NoSocket, InvalidSocket,
   InvalidOption, CertBufferTooSmall, AlreadyInUse); InternalError (999) is Eden's, for an error the console's list
   has no name for. Eden's ssl.cpp forces the verify option to 0 and swaps SetOption's arguments; neither is done
   here. */
#include "nx/hle/services/network/network.h"
#include "nx/hle/hle.h"
#include "hle/services/network/ssl.swipc.inc"
#include <limits.h>
#ifdef _WIN32
/* before OpenSSL's headers, which undefine the names wincrypt.h's macros take (X509_NAME) */
#include <wincrypt.h>
#endif
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#define SSLSRV_RC(desc) RC(123, desc)
enum {
    SSLSRV_INSUFFICIENT_MEMORY = 102,
    SSLSRV_NO_SOCKET = 103,
    SSLSRV_INVALID_SOCKET = 106,
    SSLSRV_INVALID_OPTION = 126,
    SSLSRV_CERT_BUFFER_TOO_SMALL = 202,
    SSLSRV_ALREADY_IN_USE = 203,
    SSLSRV_WOULD_BLOCK = 204,
    SSLSRV_TIMEOUT = 205,
    SSLSRV_IO_ABORTED = 206,
    SSLSRV_NO_CONNECTION = 208,
    SSLSRV_CONNECTION_RESET = 209,
    SSLSRV_CONNECTION_ABORTED = 210,
    SSLSRV_NETWORK_DOWN = 212,
    SSLSRV_CLIENT_PKI_ALREADY_REGISTERED = 215,
    SSLSRV_MAX_SERVER_PKI_REGISTERED = 218,
    SSLSRV_INVALID_CERT_DOMAIN = 301,
    SSLSRV_EXPIRED_CERTIFICATE = 303,
    SSLSRV_REVOKED_CERTIFICATE = 304,
    SSLSRV_BAD_CERTIFICATE = 307,
    SSLSRV_UNKNOWN_CA = 308,
    SSLSRV_INTERNAL_ERROR = 999,
};

enum {
    /* SslVersion: Auto, or the versions allowed, one bit each from TLS 1.0 at bit 3 */
    VERSION_AUTO = 0x1,
    VERSION_TLS10_BIT = 3,
    /* VerifyOption, 3 when a connection is made */
    VERIFY_PEER_CA = 0x1,
    VERIFY_HOST_NAME = 0x2,
    VERIFY_DATE_CHECK = 0x4,
    IO_BLOCKING = 1,
    IO_NONBLOCKING = 2,
    PEV_READ = 0x1,
    PEV_WRITE = 0x2,
    PEV_EXCEPT = 0x4,
    /* ISslConnection's OptionType */
    OPT_DO_NOT_CLOSE_SOCKET = 0,
    OPT_GET_SERVER_CERT_CHAIN = 1,
    OPT_SKIP_DEFAULT_VERIFY = 2,
    OPT_ENABLE_ALPN = 3,
    OPT_COUNT = SSL_OPT_COUNT,
    CONTEXT_CRL_IMPORT_DATE_CHECK = 1,
    CERT_PEM = 1,
    CERT_DER = 2,
    INTERNAL_PKI_DEVICE_CLIENT_CERT = 1,
    RENEGOTIATION_NONE = 0,
    RENEGOTIATION_SECURE = 1,
    ALPN_NO_SUPPORT = 0,
    ALPN_NEGOTIATED = 1,
    ALPN_NO_OVERLAP = 2,
    MAX_CONNECTIONS = SSL_CONTEXT_CONNECTIONS,
    MAX_SERVER_CERTS = 71, /* per ImportServerPki */
    HOST_MAX = SSL_HOST_MAX,
    BLOCKING_MS = 300000,  /* a Blocking connection's timeout, NSS's 5 minutes (libnx) */
    CERT_INFO_SIZE = 0x18, /* BuiltInCertificateInfo {u32 id, u32 status, u64 size, u64 offset} */
    CIPHER_NAME_SIZE = 0x40,
    CIPHER_INFO_SIZE = 0x48, /* SslCipherInfo {char cipher[0x40], char protocol_version[8]} */
};
/* the GetServerCertChain buffer's header: "CertChMN" */
#define CERT_CHAIN_MAGIC 0x4E4D684374726543ull

/* SslContext and SslConn, the objects' data, are in network.h, with the pools */

/* ============================================================ the host's roots, once */
/* The console trusts the CAs of its system archive, the runtime the host's. This OpenSSL is the build's own, whose
   OPENSSLDIR is not the system's, so the default paths find nothing: the roots are read from SSL_CERT_FILE and
   SSL_CERT_DIR when they are set (OpenSSL's variables), else from the first bundle where distributions keep it, else
   the hashed directory /etc/ssl/certs, and on Windows from the system's ROOT store. Each context's store gets them */

static STACK_OF(X509) * load_bundle(const char* path) {
    X509_STORE* st = X509_STORE_new();
    STACK_OF(X509)* certs = NULL;
    if (st && X509_STORE_load_file(st, path) == 1) certs = X509_STORE_get1_all_certs(st);
    X509_STORE_free(st);
    if (certs && !sk_X509_num(certs)) {
        sk_X509_pop_free(certs, X509_free);
        certs = NULL;
    }
    return certs;
}
/* the host's own roots, when the environment names none. Returns where they came from */
#ifdef _WIN32
static const char* system_roots(void) {
    HCERTSTORE h = CertOpenSystemStoreW(0, L"ROOT");
    PCCERT_CONTEXT cc = NULL;
    if (!h) return "nowhere";
    g_hle.ssl.roots = sk_X509_new_null();
    while (g_hle.ssl.roots && (cc = CertEnumCertificatesInStore(h, cc)) != NULL) {
        const unsigned char* p = cc->pbCertEncoded;
        X509* x = d2i_X509(NULL, &p, (long)cc->cbCertEncoded);
        if (x && !sk_X509_push(g_hle.ssl.roots, x)) X509_free(x);
    }
    CertCloseStore(h, 0);
    return "the system's ROOT store";
}
#else
static const char* system_roots(void) {
    static const char* const bundles[] = {
        "/etc/ssl/certs/ca-certificates.crt",     /* Debian, Ubuntu, Arch, Gentoo */
        "/etc/pki/tls/certs/ca-bundle.crt",       /* Fedora, RHEL */
        "/etc/ssl/ca-bundle.pem",                 /* openSUSE */
        "/etc/ssl/cert.pem",                      /* Alpine, macOS, OpenBSD */
        "/usr/local/share/certs/ca-root-nss.crt", /* FreeBSD */
    };
    size_t i;
    for (i = 0; i < sizeof bundles / sizeof bundles[0]; i++)
        if ((g_hle.ssl.roots = load_bundle(bundles[i])) != NULL) return bundles[i];
    strcpy(g_hle.ssl.roots_dir, "/etc/ssl/certs");
    return g_hle.ssl.roots_dir;
}
#endif

static int bio_write(BIO* b, const char* data, size_t size, size_t* written);
static int bio_read(BIO* b, char* data, size_t size, size_t* got);
static long bio_ctrl(BIO* b, int cmd, long num, void* ptr);
static int bio_create(BIO* b);
static void ssl_init(void) {
    const char *file, *dir, *from = "nowhere";
    pm_lock(&g_host.ssl.init_lk);
    if (g_hle.ssl.init_done) {
        pm_unlock(&g_host.ssl.init_lk);
        return;
    }
    file = getenv("SSL_CERT_FILE");
    dir = getenv("SSL_CERT_DIR");
    if (file && *file && (g_hle.ssl.roots = load_bundle(file)) != NULL) from = file;
    if (dir && *dir && strlen(dir) < sizeof g_hle.ssl.roots_dir) {
        strcpy(g_hle.ssl.roots_dir, dir);
        from = dir;
    }
    if (!(file && *file) && !g_hle.ssl.roots_dir[0]) from = system_roots();
    ERR_clear_error();
    LOG("ssl: root certificates from %s (%d read)\n", from, g_hle.ssl.roots ? sk_X509_num(g_hle.ssl.roots) : 0);
    g_hle.ssl.bio = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "snx socket");
    if (!g_hle.ssl.bio || !BIO_meth_set_write_ex(g_hle.ssl.bio, bio_write) || !BIO_meth_set_read_ex(g_hle.ssl.bio, bio_read) ||
        !BIO_meth_set_ctrl(g_hle.ssl.bio, bio_ctrl) || !BIO_meth_set_create(g_hle.ssl.bio, bio_create))
        fatal("ssl: OpenSSL can't make a BIO method\n");
    g_hle.ssl.init_done = 1;
    pm_unlock(&g_host.ssl.init_lk);
}
/* a context's store: the roots, with any certificate in it an anchor (PARTIAL_CHAIN), since the console's
   ImportServerPki takes CAs and servers' own certificates alike */
static X509_STORE* roots_store(void) {
    X509_STORE* st = X509_STORE_new();
    int i;
    if (!st) return NULL;
    for (i = 0; i < sk_X509_num(g_hle.ssl.roots); i++) X509_STORE_add_cert(st, sk_X509_value(g_hle.ssl.roots, i));
    if (g_hle.ssl.roots_dir[0]) X509_STORE_load_path(st, g_hle.ssl.roots_dir);
    X509_STORE_set_flags(st, X509_V_FLAG_PARTIAL_CHAIN);
    ERR_clear_error();
    return st;
}

/* ============================================================ results */
static int errno_result(int e) {
    switch (e) {
        /* NSPR maps EPIPE to PR_CONNECT_RESET_ERROR */
        case SNX_ECONNRESET:
        case SNX_EPIPE: return SSLSRV_RC(SSLSRV_CONNECTION_RESET);
        case SNX_ECONNABORTED: return SSLSRV_RC(SSLSRV_CONNECTION_ABORTED);
        case SNX_ETIMEDOUT: return SSLSRV_RC(SSLSRV_TIMEOUT);
        case SNX_EINTR: return SSLSRV_RC(SSLSRV_IO_ABORTED); /* the stack was cancelled: the game is stopping */
        case SNX_ENOTCONN: return SSLSRV_RC(SSLSRV_NO_CONNECTION);
        case SNX_ENETDOWN:
        case SNX_ENETUNREACH:
        case SNX_EHOSTUNREACH: return SSLSRV_RC(SSLSRV_NETWORK_DOWN);
        case SNX_ENOMEM: return SSLSRV_RC(SSLSRV_INSUFFICIENT_MEMORY);
        default: return SSLSRV_RC(SSLSRV_INTERNAL_ERROR);
    }
}
/* a fatal alert from the server: Switchbrew's ResultSslAlert* run 1501 to 1530 in the order of TLS's alerts, without
   SSL 3's no_certificate (41) */
static int alert_result(int alert) {
    static const u8 alerts[] = {0,  10, 20, 21, 22, 30, 40, 41, 42, 43,  44,  45,  46,  47,  48,
                                49, 50, 51, 60, 70, 71, 80, 86, 90, 100, 110, 111, 112, 113, 114};
    int i;
    for (i = 0; i < (int)sizeof alerts; i++)
        if (alerts[i] == alert && alert != 41) return SSLSRV_RC(1501 + i);
    return SSLSRV_RC(SSLSRV_INTERNAL_ERROR);
}
/* the result of a TLS call that failed for another reason than a socket that isn't ready */
static int conn_error(SslConn* c, int ssl_error) {
    char text[160];
    if (c->verify_error) return c->verify_error;
    if (c->sock_err) return errno_result(c->sock_err);
    if (c->alert >= 0) return alert_result(c->alert);
    ERR_error_string_n(ERR_peek_last_error(), text, sizeof text);
    LOG_LIMIT(8, "ssl: OpenSSL error %d (%s)\n", ssl_error, text);
    if (ssl_error == SSL_ERROR_ZERO_RETURN || ssl_error == SSL_ERROR_SYSCALL)
        return SSLSRV_RC(SSLSRV_CONNECTION_ABORTED);
    return SSLSRV_RC(SSLSRV_INTERNAL_ERROR);
}

/* ============================================================ the socket under OpenSSL */
/* The calls never wait (DONTWAIT): a socket that isn't ready is a retry for OpenSSL, and conn_io waits for it */
static int bio_write(BIO* b, const char* data, size_t size, size_t* written) {
    SslConn* c = BIO_get_data(b);
    int64_t r = snx_socket_send(c->s, data, size, SNX_MSG_DONTWAIT, NULL);
    BIO_clear_retry_flags(b);
    if (r >= 0) {
        *written = (size_t)r;
        return 1;
    }
    if (r == -SNX_EAGAIN)
        BIO_set_retry_write(b);
    else
        c->sock_err = (int)-r;
    return 0;
}
/* the server closing the connection is an end of file for OpenSSL. A read takes it as the end of the data
   (SSL_OP_IGNORE_UNEXPECTED_EOF), and a handshake fails with it: ConnectionAborted */
static int bio_read(BIO* b, char* data, size_t size, size_t* got) {
    SslConn* c = BIO_get_data(b);
    int64_t r = snx_socket_recv(c->s, data, size, SNX_MSG_DONTWAIT, NULL, NULL);
    BIO_clear_retry_flags(b);
    if (r > 0) {
        *got = (size_t)r;
        return 1;
    }
    if (r == -SNX_EAGAIN)
        BIO_set_retry_read(b);
    else
        c->sock_err = r ? (int)-r : SNX_ECONNABORTED;
    return 0;
}
static long bio_ctrl(BIO* b, int cmd, long num, void* ptr) {
    (void)b;
    (void)num;
    (void)ptr;
    return cmd == BIO_CTRL_FLUSH;
}
static int bio_create(BIO* b) {
    BIO_set_init(b, 1);
    return 1;
}

/* ============================================================ verification */
/* Each VerifyOption bit decides a kind of failure: PeerCa the chain, HostName the name, DateCheck the dates. An
   error whose bit is clear is let through, and the first one that isn't is kept for GetVerifyCertError */
static int verify_cb(int ok, X509_STORE_CTX* store) {
    SSL* ssl = X509_STORE_CTX_get_ex_data(store, SSL_get_ex_data_X509_STORE_CTX_idx());
    SslConn* c = SSL_get_app_data(ssl);
    u32 bit = VERIFY_PEER_CA;
    int rc = SSLSRV_BAD_CERTIFICATE;
    if (ok) return 1;
    switch (X509_STORE_CTX_get_error(store)) {
        case X509_V_ERR_HOSTNAME_MISMATCH:
        case X509_V_ERR_IP_ADDRESS_MISMATCH:
            bit = VERIFY_HOST_NAME;
            rc = SSLSRV_INVALID_CERT_DOMAIN;
            break;
        case X509_V_ERR_CERT_HAS_EXPIRED:
        case X509_V_ERR_CERT_NOT_YET_VALID:
            bit = VERIFY_DATE_CHECK;
            rc = SSLSRV_EXPIRED_CERTIFICATE;
            break;
        case X509_V_ERR_CERT_REVOKED: rc = SSLSRV_REVOKED_CERTIFICATE; break;
        case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
        case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
        case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
        case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
        case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:
        case X509_V_ERR_CERT_UNTRUSTED: rc = SSLSRV_UNKNOWN_CA; break;
        default: break;
    }
    if (!(c->verify & bit)) return 1;
    if (!c->verify_error) c->verify_error = SSLSRV_RC(rc);
    return 0;
}
static void info_cb(const SSL* ssl, int where, int ret) {
    SslConn* c = SSL_get_app_data(ssl);
    if ((where & SSL_CB_READ_ALERT) == SSL_CB_READ_ALERT && (ret >> 8) == SSL3_AL_FATAL) c->alert = ret & 0xFF;
}

/* ============================================================ TLS calls */
/* before the handshake: what the context holds now (its store and client certificate), the name, and the options */
static void conn_prepare(SslConn* c) {
    SslContext* x = c->context->data;
    int ip = 0;
    pm_lock(&x->lk);
    SSL_set1_verify_cert_store(c->ssl, x->store);
    if (x->client_cert) SSL_use_cert_and_key(c->ssl, x->client_cert, x->client_key, x->client_chain, 1);
    pm_unlock(&x->lk);
    if (c->host[0]) {
        ip = SSL_set1_ipaddr(c->ssl, c->host);
        /* no server name for an address (RFC 6066) */
        if (!ip) {
            SSL_set_tlsext_host_name(c->ssl, c->host);
            SSL_set1_dnsname(c->ssl, c->host);
        }
    }
    if (c->renegotiation == RENEGOTIATION_NONE) SSL_set_options(c->ssl, SSL_OP_NO_RENEGOTIATION);
    if (!c->option[OPT_ENABLE_ALPN]) SSL_set_alpn_protos(c->ssl, NULL, 0);
    ERR_clear_error();
}
enum { OP_HANDSHAKE, OP_READ, OP_PEEK, OP_WRITE };
/* One TLS call, until it is done, or in NonBlocking mode until the socket isn't ready (WouldBlock), or in Blocking
   mode until the 5 minutes run out (Timeout). A read that finds the connection closed reads 0 bytes */
static int conn_io(SslConn* c, int op, void* data, size_t size, size_t* done) {
    u64 start = plat_ns();
    *done = 0;
    R_UNLESS(c->s, SSLSRV_RC(SSLSRV_NO_SOCKET));
    if (op != OP_HANDSHAKE && !size) R_SUCCEED();
    if (SSL_in_before(c->ssl)) conn_prepare(c);
    for (;;) {
        SnxPollEntry e;
        u64 elapsed;
        int r, err;
        ERR_clear_error();
        c->sock_err = 0;
        c->alert = -1;
        switch (op) {
            case OP_HANDSHAKE: r = SSL_do_handshake(c->ssl); break;
            case OP_READ: r = SSL_read_ex(c->ssl, data, size, done); break;
            case OP_PEEK: r = SSL_peek_ex(c->ssl, data, size, done); break;
            default: r = SSL_write_ex(c->ssl, data, size, done); break;
        }
        if (r == 1) R_SUCCEED();
        err = SSL_get_error(c->ssl, r);
        if (err == SSL_ERROR_ZERO_RETURN && op != OP_HANDSHAKE) R_SUCCEED();
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) return conn_error(c, err);
        R_UNLESS(c->io_mode == IO_BLOCKING, SSLSRV_RC(SSLSRV_WOULD_BLOCK));
        elapsed = (plat_ns() - start) / 1000000;
        R_UNLESS(elapsed < BLOCKING_MS, SSLSRV_RC(SSLSRV_TIMEOUT));
        e.socket = c->s;
        e.host_fd = -1;
        e.events = err == SSL_ERROR_WANT_READ ? SNX_POLLIN : SNX_POLLOUT;
        e.revents = 0;
        r = snx_net_poll(&e, 1, (int)(BLOCKING_MS - elapsed));
        if (r < 0) return errno_result(-r);
    }
}
/* DoHandshakeGetServerCert's buffer: the server's certificate, or with GetServerCertChain the chain it sent behind
   a header {u64 magic, u32 count, u32 pad} and an entry {u32 size, u32 offset} each. None when PeerCa isn't verified
   (libnx). Returns the size, written to out unless it is NULL, and the number of certificates in *count */
static u32 server_certs(SslConn* c, u8* out, u32* count) {
    STACK_OF(X509)* chain = SSL_get_peer_cert_chain(c->ssl);
    int i, n = chain ? sk_X509_num(chain) : 0;
    u32 at;
    *count = 0;
    if (!(c->verify & VERIFY_PEER_CA) || n <= 0 || !SSL_is_init_finished(c->ssl)) return 0;
    if (!c->option[OPT_GET_SERVER_CERT_CHAIN]) {
        int len = i2d_X509(sk_X509_value(chain, 0), NULL);
        if (len <= 0) return 0;
        if (out) i2d_X509(sk_X509_value(chain, 0), &out);
        *count = 1;
        return (u32)len;
    }
    at = 16 + 8 * (u32)n;
    for (i = 0; i < n; i++) {
        X509* x = sk_X509_value(chain, i);
        int len = i2d_X509(x, NULL);
        if (len < 0) len = 0;
        if (out) {
            u8* p = out + at;
            snx_st_le32(out + 16 + 8 * i, (u32)len);
            snx_st_le32(out + 20 + 8 * i, at);
            if (len) i2d_X509(x, &p);
        }
        at += (u32)len;
    }
    if (out) {
        snx_st_le64(out, CERT_CHAIN_MAGIC);
        snx_st_le32(out + 8, (u32)n);
        snx_st_le32(out + 12, 0);
    }
    *count = (u32)n;
    return at;
}

/* ============================================================ ISslConnection */
/* The game's descriptor, from the table of the bsd session its process registered. The SDK keeps the descriptor
   this answers and closes it when the connection goes (nn::ssl::Connection::Destroy): the input, so the game's
   descriptor goes and the connection's duplicate stays, or -1 with DoNotCloseSocket, and the game keeps both */
static int nn_ssl_sf_ISslConnection_SetSocketDescriptor(SvcObj* self, i32 fd, i32* out_fd) {
    SslConn* c = self->data;
    SslContext* x = c->context->data;
    SnxSockAddr peer;
    SnxSocket* s;
    BIO* bio;
    R_UNLESS(!c->s, SSLSRV_RC(SSLSRV_ALREADY_IN_USE));
    s = bsd_client_socket(x->pid, fd);
    R_UNLESS(s, SSLSRV_RC(SSLSRV_INVALID_SOCKET));
    /* the console asks the duplicate for its peer, and fails without one */
    if (snx_socket_peer(s, &peer)) {
        snx_socket_unref(s);
        R_THROW(SSLSRV_RC(SSLSRV_NO_CONNECTION));
    }
    bio = BIO_new(g_hle.ssl.bio);
    if (!bio) {
        snx_socket_unref(s);
        R_THROW(SSLSRV_RC(SSLSRV_INSUFFICIENT_MEMORY));
    }
    BIO_set_data(bio, c);
    SSL_set_bio(c->ssl, bio, bio);
    c->s = s;
    c->fd = fd;
    *out_fd = c->option[OPT_DO_NOT_CLOSE_SOCKET] ? -1 : fd;
    LOGV("ssl: SetSocketDescriptor(%d)\n", fd);
    R_SUCCEED();
}
/* up to HOST_MAX characters, to the first NUL */
static int nn_ssl_sf_ISslConnection_SetHostName(SvcObj* self, IpcBuf name) {
    SslConn* c = self->data;
    size_t n = name.addr ? (size_t)name.size : 0;
    const char* p = n ? (const char*)GP(name.addr) : "";
    const char* nul = memchr(p, 0, n);
    if (nul) n = (size_t)(nul - p);
    if (n > HOST_MAX) n = HOST_MAX;
    memcpy(c->host, p, n);
    c->host[n] = 0;
    LOGV("ssl: SetHostName(%s)\n", c->host);
    R_SUCCEED();
}
static int nn_ssl_sf_ISslConnection_SetVerifyOption(SvcObj* self, nn_ssl_sf_VerifyOption option) {
    SslConn* c = self->data;
    c->verify = option;
    R_SUCCEED();
}
static int nn_ssl_sf_ISslConnection_SetIoMode(SvcObj* self, nn_ssl_sf_IoMode mode) {
    SslConn* c = self->data;
    R_UNLESS(c->s, SSLSRV_RC(SSLSRV_NO_SOCKET));
    if (mode != IO_BLOCKING && mode != IO_NONBLOCKING) R_THROW(SSLSRV_RC(SSLSRV_INVALID_OPTION));
    c->io_mode = mode;
    R_SUCCEED();
}
static int nn_ssl_sf_ISslConnection_GetSocketDescriptor(SvcObj* self, i32* fd) {
    SslConn* c = self->data;
    R_UNLESS(c->s, SSLSRV_RC(SSLSRV_NO_SOCKET));
    *fd = c->fd;
    R_SUCCEED();
}
/* what fits of the name and its NUL, and the name's length */
static int nn_ssl_sf_ISslConnection_GetHostName(SvcObj* self, IpcBuf name, u32* size) {
    SslConn* c = self->data;
    size_t n = strlen(c->host);
    if (name.addr && name.size) {
        size_t k = n < name.size ? n : (size_t)name.size - 1;
        memcpy(GP(name.addr), c->host, k);
        ((u8*)GP(name.addr))[k] = 0;
    }
    *size = (u32)n;
    R_SUCCEED();
}
static int nn_ssl_sf_ISslConnection_GetVerifyOption(SvcObj* self, nn_ssl_sf_VerifyOption* option) {
    *option = ((SslConn*)self->data)->verify;
    R_SUCCEED();
}
static int nn_ssl_sf_ISslConnection_GetIoMode(SvcObj* self, nn_ssl_sf_IoMode* mode) {
    *mode = ((SslConn*)self->data)->io_mode;
    R_SUCCEED();
}
static int nn_ssl_sf_ISslConnection_DoHandshake(SvcObj* self) {
    SslConn* c = self->data;
    size_t none;
    int r = conn_io(c, OP_HANDSHAKE, NULL, 0, &none);
    LOGV("ssl: DoHandshake(%s) -> %#x\n", c->host, (unsigned)r);
    return r;
}
static int nn_ssl_sf_ISslConnection_DoHandshakeGetServerCert(SvcObj* self, IpcBuf certificates, u32* size, u32* count) {
    SslConn* c = self->data;
    u32 n;
    R_TRY(nn_ssl_sf_ISslConnection_DoHandshake(self));
    n = server_certs(c, NULL, count);
    R_UNLESS(n <= (certificates.addr ? certificates.size : 0), SSLSRV_RC(SSLSRV_CERT_BUFFER_TOO_SMALL));
    if (n) server_certs(c, GP(certificates.addr), count);
    *size = n;
    R_SUCCEED();
}
static int nn_ssl_sf_ISslConnection_Read(SvcObj* self, IpcBuf data, u32* size) {
    size_t n;
    int r = conn_io(self->data, OP_READ, GP(data.addr), data.addr ? (size_t)data.size : 0, &n);
    *size = (u32)n;
    return r;
}
/* Blocking mode writes everything, NonBlocking what the socket takes. A write cut short answers what went out */
static int nn_ssl_sf_ISslConnection_Write(SvcObj* self, IpcBuf data, u32* size) {
    SslConn* c = self->data;
    size_t total = 0, want = data.addr ? (size_t)data.size : 0, n;
    int r;
    do {
        r = conn_io(c, OP_WRITE, GP(data.addr) + total, want - total, &n);
        total += n;
    } while (!r && total < want && c->io_mode == IO_BLOCKING);
    if (r && !total) return r;
    *size = (u32)total;
    R_SUCCEED();
}
static int nn_ssl_sf_ISslConnection_Pending(SvcObj* self, i32* size) {
    SslConn* c = self->data;
    R_UNLESS(c->s, SSLSRV_RC(SSLSRV_NO_SOCKET));
    *size = SSL_pending(c->ssl);
    R_SUCCEED();
}
static int nn_ssl_sf_ISslConnection_Peek(SvcObj* self, IpcBuf data, u32* size) {
    size_t n;
    int r = conn_io(self->data, OP_PEEK, GP(data.addr), data.addr ? (size_t)data.size : 0, &n);
    *size = (u32)n;
    return r;
}
/* Read is ready with decrypted data waiting or the socket readable, Write with the socket writable. Nothing ready in
   the time answers no events */
static int nn_ssl_sf_ISslConnection_Poll(SvcObj* self, nn_ssl_sf_PollEvent events, u32 timeout_ms,
                                         nn_ssl_sf_PollEvent* revents) {
    SslConn* c = self->data;
    SnxPollEntry e;
    int pending, r;
    R_UNLESS(c->s, SSLSRV_RC(SSLSRV_NO_SOCKET));
    pending = (events & PEV_READ) && SSL_pending(c->ssl) > 0;
    e.socket = c->s;
    e.host_fd = -1;
    e.events = (uint16_t)((events & PEV_READ ? SNX_POLLIN : 0) | (events & PEV_WRITE ? SNX_POLLOUT : 0) |
                          (events & PEV_EXCEPT ? SNX_POLLPRI : 0));
    e.revents = 0;
    r = snx_net_poll(&e, 1, pending ? 0 : timeout_ms > INT_MAX ? INT_MAX : (int)timeout_ms);
    if (r < 0) return errno_result(-r);
    if ((events & PEV_READ) && (pending || (e.revents & (SNX_POLLIN | SNX_POLLHUP | SNX_POLLERR))))
        *revents |= PEV_READ;
    if ((events & PEV_WRITE) && (e.revents & (SNX_POLLOUT | SNX_POLLERR))) *revents |= PEV_WRITE;
    if ((events & PEV_EXCEPT) && (e.revents & (SNX_POLLPRI | SNX_POLLERR | SNX_POLLHUP))) *revents |= PEV_EXCEPT;
    R_SUCCEED();
}
/* the failed certificate check's result, once */
static int nn_ssl_sf_ISslConnection_GetVerifyCertError(SvcObj* self) {
    SslConn* c = self->data;
    int r = c->verify_error;
    c->verify_error = 0;
    return r;
}
static int nn_ssl_sf_ISslConnection_GetNeededServerCertBufferSize(SvcObj* self, u32* size) {
    u32 count;
    *size = server_certs(self->data, NULL, &count);
    R_SUCCEED();
}
static int nn_ssl_sf_ISslConnection_SetSessionCacheMode(SvcObj* self, nn_ssl_sf_SessionCacheMode mode) {
    ((SslConn*)self->data)->cache_mode = mode;
    R_SUCCEED();
}
static int nn_ssl_sf_ISslConnection_GetSessionCacheMode(SvcObj* self, nn_ssl_sf_SessionCacheMode* mode) {
    *mode = ((SslConn*)self->data)->cache_mode;
    R_SUCCEED();
}
static int nn_ssl_sf_ISslConnection_SetRenegotiationMode(SvcObj* self, nn_ssl_sf_RenegotiationMode mode) {
    ((SslConn*)self->data)->renegotiation = mode;
    R_SUCCEED();
}
static int nn_ssl_sf_ISslConnection_GetRenegotiationMode(SvcObj* self, nn_ssl_sf_RenegotiationMode* mode) {
    *mode = ((SslConn*)self->data)->renegotiation;
    R_SUCCEED();
}
/* DoNotCloseSocket only before SetSocketDescriptor (Switchbrew) */
static int nn_ssl_sf_ISslConnection_SetOption(SvcObj* self, u8 value, nn_ssl_sf_OptionType option) {
    SslConn* c = self->data;
    if (option >= OPT_COUNT || (option == OPT_DO_NOT_CLOSE_SOCKET && c->s)) R_THROW(SSLSRV_RC(SSLSRV_INVALID_OPTION));
    c->option[option] = value != 0;
    R_SUCCEED();
}
static int nn_ssl_sf_ISslConnection_GetOption(SvcObj* self, nn_ssl_sf_OptionType option, u8* value) {
    SslConn* c = self->data;
    R_UNLESS(option < OPT_COUNT, SSLSRV_RC(SSLSRV_INVALID_OPTION));
    *value = c->option[option];
    R_SUCCEED();
}
/* the cipher's IANA name, as NSS names them, and OpenSSL's name for the version ("TLSv1.3") */
static int nn_ssl_sf_ISslConnection_GetCipherInfo(SvcObj* self, u32 version, IpcBuf info) {
    SslConn* c = self->data;
    const SSL_CIPHER* cipher = SSL_get_current_cipher(c->ssl);
    char out[CIPHER_INFO_SIZE];
    R_UNLESS(c->s, SSLSRV_RC(SSLSRV_NO_SOCKET));
    memset(out, 0, sizeof out);
    if (cipher) {
        const char* name = SSL_CIPHER_standard_name(cipher);
        strncpy(out, name ? name : SSL_CIPHER_get_name(cipher), CIPHER_NAME_SIZE - 1);
        strncpy(out + CIPHER_NAME_SIZE, SSL_get_version(c->ssl), CIPHER_INFO_SIZE - CIPHER_NAME_SIZE - 1);
    }
    if (info.addr) memcpy(GP(info.addr), out, info.size < sizeof out ? (size_t)info.size : sizeof out);
    R_SUCCEED();
}
/* {u8 size, the name} each, TLS's own format: used when EnableAlpn is set at the handshake */
static int nn_ssl_sf_ISslConnection_SetNextAlpnProto(SvcObj* self, IpcBuf protocols) {
    SslConn* c = self->data;
    const u8* p = protocols.addr ? GP(protocols.addr) : NULL;
    u64 at = 0, n = protocols.addr ? protocols.size : 0;
    R_UNLESS(c->s, SSLSRV_RC(SSLSRV_NO_SOCKET));
    R_UNLESS(n >= 2 && n <= UINT_MAX, SSLSRV_RC(SSLSRV_INVALID_OPTION));
    while (at < n) {
        R_UNLESS(p[at] && p[at] <= n - at - 1, SSLSRV_RC(SSLSRV_INVALID_OPTION));
        at += 1 + (u64)p[at];
    }
    return SSL_set_alpn_protos(c->ssl, p, (unsigned)n) ? SSLSRV_RC(SSLSRV_INSUFFICIENT_MEMORY) : 0;
}
static int nn_ssl_sf_ISslConnection_GetNextAlpnProto(SvcObj* self, IpcBuf protocol, nn_ssl_sf_AlpnProtoState* state,
                                                     u32* size) {
    SslConn* c = self->data;
    const unsigned char* p = NULL;
    unsigned n = 0;
    R_UNLESS(c->s, SSLSRV_RC(SSLSRV_NO_SOCKET));
    SSL_get0_alpn_selected(c->ssl, &p, &n);
    if (n) {
        *state = ALPN_NEGOTIATED;
        if (protocol.addr && protocol.size) {
            size_t k = n < protocol.size ? n : (size_t)protocol.size - 1;
            memcpy(GP(protocol.addr), p, k);
            ((u8*)GP(protocol.addr))[k] = 0;
        }
    } else
        *state = c->option[OPT_ENABLE_ALPN] && SSL_is_init_finished(c->ssl) ? ALPN_NO_OVERLAP : ALPN_NO_SUPPORT;
    *size = n;
    R_SUCCEED();
}

static int conn_disp(SvcObj* o, IpcCtx* ipc) {
    SslConn* c = o->data;
    int r;
    pm_lock(&c->lk);
    r = nn_ssl_sf_ISslConnection_dispatch(o, ipc);
    pm_unlock(&c->lk);
    return r;
}
/* the close_notify, without waiting for the server's */
static void conn_destroy(SvcObj* o) {
    SslConn* c = o->data;
    SslContext* x = c->context->data;
    if (c->s && SSL_is_init_finished(c->ssl)) SSL_shutdown(c->ssl);
    SSL_free(c->ssl);
    ERR_clear_error();
    snx_socket_unref(c->s);
    pm_lock(&x->lk);
    x->connections--;
    pm_unlock(&x->lk);
    svcobj_unref(c->context);
}

/* ============================================================ ISslContext */
static int nn_ssl_sf_ISslContext_SetOption(SvcObj* self, nn_ssl_sf_ContextOption option, i32 value) {
    SslContext* x = self->data;
    R_UNLESS(option == CONTEXT_CRL_IMPORT_DATE_CHECK, SSLSRV_RC(SSLSRV_INVALID_OPTION));
    x->crl_date_check = value;
    R_SUCCEED();
}
static int nn_ssl_sf_ISslContext_GetOption(SvcObj* self, nn_ssl_sf_ContextOption option, i32* value) {
    SslContext* x = self->data;
    R_UNLESS(option == CONTEXT_CRL_IMPORT_DATE_CHECK, SSLSRV_RC(SSLSRV_INVALID_OPTION));
    *value = x->crl_date_check;
    R_SUCCEED();
}
/* The limit's result is not known: the console's list of results has none for it, InsufficientMemory is the nearest */
static int nn_ssl_sf_ISslContext_CreateConnection(SvcObj* self, SvcObj** connection) {
    SslContext* x = self->data;
    SvcObj* o;
    SslConn* c;
    SSL* ssl;
    R_UNLESS(x->connections < MAX_CONNECTIONS, SSLSRV_RC(SSLSRV_INSUFFICIENT_MEMORY));
    ssl = SSL_new(x->ctx);
    R_UNLESS(ssl, SSLSRV_RC(SSLSRV_INSUFFICIENT_MEMORY));
    o = SVCOBJ_NEW_DATA("nn::ssl::sf::ISslConnection", conn_disp, g_hle.ssl.conns, g_hle.ssl.conn_next,
                        "ssl connections");
    c = o->data;
    pm_init(&c->lk);
    c->ssl = ssl;
    c->fd = -1;
    c->verify = VERIFY_PEER_CA | VERIFY_HOST_NAME;
    c->io_mode = IO_BLOCKING;
    c->renegotiation = RENEGOTIATION_SECURE;
    c->alert = -1;
    SSL_set_app_data(ssl, c);
    SSL_set_info_callback(ssl, info_cb);
    SSL_set_connect_state(ssl);
    svcobj_ref(self);
    c->context = self;
    x->connections++;
    o->destroy = conn_destroy;
    *connection = o;
    R_SUCCEED();
}
static int nn_ssl_sf_ISslContext_GetConnectionCount(SvcObj* self, u32* count) {
    *count = ((SslContext*)self->data)->connections;
    R_SUCCEED();
}
/* PEM or DER, one certificate or more: the console checks the format and reads either (Switchbrew) */
static STACK_OF(X509) * read_certs(const u8* p, size_t n) {
    STACK_OF(X509)* certs = sk_X509_new_null();
    const char* pem = "-----BEGIN";
    size_t i, k = strlen(pem);
    int is_pem = 0;
    if (!certs) return NULL;
    for (i = 0; i + k <= n && !is_pem; i++) is_pem = !memcmp(p + i, pem, k);
    if (is_pem) {
        BIO* b = BIO_new_mem_buf(p, n > INT_MAX ? INT_MAX : (int)n);
        X509* x;
        while (b && (x = PEM_read_bio_X509(b, NULL, NULL, NULL)) != NULL)
            if (!sk_X509_push(certs, x)) X509_free(x);
        BIO_free(b);
    } else {
        const unsigned char* at = p;
        while (at < p + n) {
            X509* x = d2i_X509(NULL, &at, (long)(p + n - at));
            if (!x) break;
            if (!sk_X509_push(certs, x)) X509_free(x);
        }
    }
    ERR_clear_error();
    return certs;
}
static int nn_ssl_sf_ISslContext_ImportServerPki(SvcObj* self, nn_ssl_sf_CertificateFormat format, IpcBuf certificates,
                                                 u64* id) {
    SslContext* x = self->data;
    STACK_OF(X509) * certs;
    int i, n;
    if (format != CERT_PEM && format != CERT_DER) R_THROW(SSLSRV_RC(SSLSRV_INVALID_OPTION));
    certs = read_certs(certificates.addr ? GP(certificates.addr) : NULL, certificates.addr ? certificates.size : 0);
    R_UNLESS(certs, SSLSRV_RC(SSLSRV_INSUFFICIENT_MEMORY));
    n = sk_X509_num(certs);
    if (!n || n > MAX_SERVER_CERTS) {
        sk_X509_pop_free(certs, X509_free);
        R_THROW(SSLSRV_RC(n ? SSLSRV_MAX_SERVER_PKI_REGISTERED : SSLSRV_BAD_CERTIFICATE));
    }
    for (i = 0; i < n; i++) X509_STORE_add_cert(x->store, sk_X509_value(certs, i));
    sk_X509_pop_free(certs, X509_free);
    ERR_clear_error();
    *id = ++x->last_id;
    LOG("ssl: ImportServerPki: %d certificates, id %llu\n", n, (unsigned long long)*id);
    R_SUCCEED();
}
/* PKCS#12, with an optional password */
static int nn_ssl_sf_ISslContext_ImportClientPki(SvcObj* self, IpcBuf pkcs12, IpcBuf password, u64* id) {
    SslContext* x = self->data;
    const unsigned char* p = pkcs12.addr ? GP(pkcs12.addr) : NULL;
    size_t pn = password.addr ? (size_t)password.size : 0;
    char* pass;
    PKCS12* p12;
    int ok;
    R_UNLESS(!x->client_id, SSLSRV_RC(SSLSRV_CLIENT_PKI_ALREADY_REGISTERED));
    p12 = p ? d2i_PKCS12(NULL, &p, pkcs12.size > LONG_MAX ? LONG_MAX : (long)pkcs12.size) : NULL;
    pass = malloc(pn + 1);
    if (!pass) {
        PKCS12_free(p12);
        R_THROW(SSLSRV_RC(SSLSRV_INSUFFICIENT_MEMORY));
    }
    if (pn) memcpy(pass, GP(password.addr), pn);
    pass[pn] = 0;
    ok = p12 && PKCS12_parse(p12, pass, &x->client_key, &x->client_cert, &x->client_chain) && x->client_cert;
    OPENSSL_cleanse(pass, pn + 1);
    free(pass);
    PKCS12_free(p12);
    ERR_clear_error();
    if (!ok) {
        EVP_PKEY_free(x->client_key);
        X509_free(x->client_cert);
        sk_X509_pop_free(x->client_chain, X509_free);
        x->client_key = NULL;
        x->client_cert = NULL;
        x->client_chain = NULL;
        R_THROW(SSLSRV_RC(SSLSRV_BAD_CERTIFICATE));
    }
    *id = x->client_id = ++x->last_id;
    R_SUCCEED();
}
/* the console's device certificate, which the runtime doesn't have: the slot is taken, with no certificate in it */
static int nn_ssl_sf_ISslContext_RegisterInternalPki(SvcObj* self, nn_ssl_sf_InternalPki pki, u64* id) {
    SslContext* x = self->data;
    R_UNLESS(pki == INTERNAL_PKI_DEVICE_CLIENT_CERT, SSLSRV_RC(SSLSRV_INVALID_OPTION));
    R_UNLESS(!x->client_id, SSLSRV_RC(SSLSRV_CLIENT_PKI_ALREADY_REGISTERED));
    LOG("ssl: RegisterInternalPki: no device certificate, connections go without a client certificate\n");
    *id = x->client_id = ++x->last_id;
    R_SUCCEED();
}
static int ctx_disp(SvcObj* o, IpcCtx* ipc) {
    SslContext* x = o->data;
    int r;
    pm_lock(&x->lk);
    r = nn_ssl_sf_ISslContext_dispatch(o, ipc);
    pm_unlock(&x->lk);
    return r;
}
static void ctx_destroy(SvcObj* o) {
    SslContext* x = o->data;
    SSL_CTX_free(x->ctx);
    X509_STORE_free(x->store);
    EVP_PKEY_free(x->client_key);
    X509_free(x->client_cert);
    sk_X509_pop_free(x->client_chain, X509_free);
    __atomic_sub_fetch(&g_hle.ssl.contexts, 1, __ATOMIC_RELAXED);
}

/* ============================================================ ISslService */
/* The versions the context allows, TLS 1.0 to 1.3 for Auto. OpenSSL's default security level refuses TLS 1.0 and
   1.1, which the console speaks: a context that allows nothing newer gets level 0 */
static void set_versions(SSL_CTX* ctx, u32 version) {
    static const int tls[4] = {TLS1_VERSION, TLS1_1_VERSION, TLS1_2_VERSION, TLS1_3_VERSION};
    int i, lo = 0, hi = 0;
    for (i = 0; i < 4; i++)
        if (version >> (VERSION_TLS10_BIT + i) & 1) {
            if (!lo) lo = tls[i];
            hi = tls[i];
        }
    if ((version & VERSION_AUTO) || !lo) {
        lo = TLS1_VERSION;
        hi = TLS1_3_VERSION;
    }
    SSL_CTX_set_min_proto_version(ctx, lo);
    SSL_CTX_set_max_proto_version(ctx, hi);
    if (hi < TLS1_2_VERSION) SSL_CTX_set_security_level(ctx, 0);
}
static int nn_ssl_sf_ISslService_CreateContext(SvcObj* self, nn_ssl_sf_SslVersion ssl_version, u64 pid,
                                               SvcObj** context) {
    SSL_CTX* ctx;
    X509_STORE* store;
    SvcObj* o;
    SslContext* x;
    ssl_init();
    ctx = SSL_CTX_new(TLS_client_method());
    store = roots_store();
    if (!ctx || !store) {
        SSL_CTX_free(ctx);
        X509_STORE_free(store);
        ERR_clear_error();
        R_THROW(SSLSRV_RC(SSLSRV_INSUFFICIENT_MEMORY));
    }
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    SSL_CTX_set_options(ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, verify_cb);
    set_versions(ctx, ssl_version);
    o = SVCOBJ_NEW_DATA("nn::ssl::sf::ISslContext", ctx_disp, g_hle.ssl.ctxs, g_hle.ssl.ctx_next, "ssl contexts");
    x = o->data;
    pm_init(&x->lk);
    x->ctx = ctx;
    x->store = store;
    x->pid = pid;
    x->crl_date_check = 1; /* libnx: 1 when the context is made */
    o->destroy = ctx_destroy;
    __atomic_add_fetch(&g_hle.ssl.contexts, 1, __ATOMIC_RELAXED);
    LOG("ssl: CreateContext(version %#x)\n", (unsigned)ssl_version);
    *context = o;
    R_SUCCEED();
}
static int nn_ssl_sf_ISslService_GetContextCount(SvcObj* self, u32* count) {
    *count = (u32)__atomic_load_n(&g_hle.ssl.contexts, __ATOMIC_RELAXED);
    R_SUCCEED();
}
/* No system archive, so no built-in certificates: the buffer holds just the terminating entry {-1, -1, 0, 0} */
static int nn_ssl_sf_ISslService_GetCertificates(SvcObj* self, IpcBuf certificates, IpcBuf ids, u32* count) {
    u32 end[6] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0, 0, 0};
    R_UNLESS(certificates.addr && certificates.size >= CERT_INFO_SIZE, SSLSRV_RC(SSLSRV_CERT_BUFFER_TOO_SMALL));
    memcpy(GP(certificates.addr), end, CERT_INFO_SIZE);
    *count = 0;
    R_SUCCEED();
}
static int nn_ssl_sf_ISslService_GetCertificateBufSize(SvcObj* self, IpcBuf ids, u32* size) {
    *size = CERT_INFO_SIZE;
    R_SUCCEED();
}
SvcObj* new_ssl(void) { return nn_ssl_sf_ISslService_new(); }
/* the functions a save state may hold of this file (state_fns.h) */
STATE_FN_TABLE(g_ssl_state_fns,
               STATE_FN("ssl", conn_destroy),
               STATE_FN("ssl", ctx_destroy),
               STATE_FN("ssl", conn_disp),
               STATE_FN("ssl", ctx_disp));
