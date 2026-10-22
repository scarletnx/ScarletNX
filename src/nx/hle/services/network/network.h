/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The network services, on the stack in src/common/network: bsd (bsd.c), sfdnsres and nsd (dns.c), nifm (nifm.c),
   ssl (ssl.c), ldn (ldn.c) */
#ifndef SNX_HLE_NETWORK_H
#define SNX_HLE_NETWORK_H
#include "nx/hle/services/services_int.h"
#include "common/network/net.h"
#include "common/network/net_room.h"
#include "common/network/net_ldn.h"
#include <time.h>

/* bsd.c */
void network_init(void); /* the stack, with its messages in the log */
SvcObj* new_bsd_u(void);
SvcObj* new_bsd_s(void);
SvcObj* new_bsdcfg(void);
/* the Eden room the console is in (ldn=room), or NULL: its datagram sockets are the game's */
void network_set_room(SnxRoom* room);
SnxRoom* network_room(void);
/* the connection the game is told about: the room's in a room, else the host's unless the network option is off; none
   in airplane mode */
int network_online(void);
int network_interface(SnxNetInterface* out); /* 0 and the host's interface when online */
/* whether a guest may reach addr: with the network option off, loopback only */
int network_reachable(const SnxSockAddr* addr);
/* the socket behind descriptor fd in the table of the bsd session process pid registered (RegisterClient), with a
   reference for the caller, or NULL: what ssl duplicates, as the console's does with bsd:s's DuplicateSocket */
SnxSocket* bsd_client_socket(u64 pid, i32 fd);
/* the address a game's bind to INADDR_ANY binds instead, while ldn gives this console one, and its subnet's broadcast
   address, which a datagram socket bound there keeps hearing; NULL clears it */
void bsd_set_bind_address(const u8* ip, const u8* bcast);

/* dns.c */
SvcObj* new_sfdnsres(void);
SvcObj* new_nsd(void);

/* nifm.c */
SvcObj* new_nifm(void);

/* ssl.c */
SvcObj* new_ssl(void);

/* ldn.c */
SvcObj* new_ldn(void);
/* at exit: every ldn:u closes its network, so the others hear the host or the station go (an Eden in a room only
   hears that, never who left the room) */
void ldn_exit(void);

/* room.c: joins the Eden room room= names when ldn=room, as the user playing (a refusal stops the game), and leaves
   it at exit */
void network_room_init(void);
void network_room_leave(void);

/* ---- HLE state in g_hle (hle.h) */
/* The service objects' data, each in a pool guarded by ipc.c's lock (svcobj_new_data). obj is the object's pool index
   + 1, which ipc.c writes */
/* bsd.c */
#define BSD_MAX_FD 128
typedef struct BsdFd {
    SnxSocket* s;
    SnxSocket* bcast; /* a datagram socket bound to the console's address also hears its subnet's broadcasts through
                         this one, bound to the broadcast address (Linux delivers them to no other); NULL otherwise */
    int flags;        /* F_SETFL's as the guest sent them */
    int exempt;       /* SocketExempt: ShutdownAllSockets leaves it alone */
} BsdFd;
/* one per session (and its clones): Horizon's descriptors belong to the client that registered */
typedef struct Bsd {
    u32 obj;
    PMutex lk;
    BsdFd fd[BSD_MAX_FD];
    int system; /* bsd:s, which may open raw sockets and duplicate descriptors */
} Bsd;
#define BSD_MAX 32 /* bsd:u and bsd:s sessions, 3 KB each: nn::socket::Initialize opens one, and a library may another */
typedef struct {
    /* the Eden room the console joined with ldn=room (room.c) */
    SnxRoom* room;
    time_t iface_when;
    int iface_have;
    SnxNetInterface iface;
    int bind_set;
    u8 bind_ip[4], bind_bcast[4];
    SvcObj* client;
    u64 client_pid;
    Bsd bsds[BSD_MAX];
    u32 bsd_next;
} HleBsd;
/* bsd's host side: locks and condition variables, which a save state never holds (hle.h's g_host) */
typedef struct {
    /* network_interface's host interface, read again after two seconds: when it was read, whether there is one */
    PMutex iface_lk;
    /* bsd_set_bind_address's: whether it is set, the console's address and its subnet's broadcast address */
    PMutex bind_lk;
    /* the session a process registered (RegisterClient), not referenced, and its process */
    PMutex client_lk;
} HostBsd;

/* ldn.c */
typedef struct Ldn {
    u32 obj;
    PMutex lk;
    u32 next; /* the next ldn:u in HleLdn's list, its index + 1, or 0 */
    SnxLdn* lan;
    u32 ev;
    u32 state;
    i16 reason;
    u8 ip[4], mask[4];
} Ldn;
#define LDN_MAX 8 /* IUserLocalCommunicationServices: a game makes one for its local play */
typedef struct {
    u32 ldns;
    Ldn ldn[LDN_MAX];
    u32 ldn_next;
} HleLdn;
/* ldn's host side: locks and condition variables, which a save state never holds (hle.h's g_host) */
typedef struct {
    /* every ldn:u, for ldn_exit to close their networks: the first's index + 1, linked through next */
    PMutex ldns_lk;
} HostLdn;

/* nifm.c: IRequest's data, the two events (u[0] holds the state) */
typedef struct {
    u32 obj;
    u32 ev[2];
} NifmRequest;
#define NIFM_REQUEST_MAX 64 /* IRequests: the SDK makes one per network use it submits, and closes it after */
typedef struct {
    NifmRequest reqs[NIFM_REQUEST_MAX];
    u32 req_next;
} HleNifm;

/* ssl.c, with OpenSSL's types by their struct tags (SSL_CTX, X509_STORE, EVP_PKEY, X509, STACK_OF(X509), SSL,
   BIO_METHOD), whose headers only ssl.c includes */
typedef struct SslContext {
    u32 obj;
    PMutex lk;
    struct ssl_ctx_st* ctx;
    struct x509_store_st* store; /* the host's roots, and what ImportServerPki added: what connections verify against */
    struct evp_pkey_st* client_key;
    struct x509_st* client_cert;
    struct stack_st_X509* client_chain;
    u64 client_id; /* ImportClientPki's or RegisterInternalPki's id, 0 before: a context takes one */
    u64 last_id;
    u64 pid; /* the process whose bsd descriptors the connections take */
    i32 crl_date_check;
    u32 connections;
} SslContext;
#define SSL_HOST_MAX 0xFF /* SetHostName's buffer, at most (libnx) */
#define SSL_OPT_COUNT 4   /* ISslConnection's OptionTypes */
typedef struct SslConn {
    u32 obj;
    PMutex lk; /* OpenSSL takes one thread at a time on an SSL: conn_disp holds it around every command */
    struct ssl_st* ssl;
    SnxSocket* s;      /* the duplicate of the game's socket, NULL before SetSocketDescriptor */
    SvcObj* context;   /* the ISslContext, referenced */
    i32 fd;            /* the game's descriptor, for GetSocketDescriptor */
    u32 verify;        /* VerifyOption */
    u32 io_mode;       /* IoMode */
    u32 cache_mode;    /* SessionCacheMode, kept for Get: no session is resumed */
    u32 renegotiation; /* RenegotiationMode */
    int verify_error;  /* the Result of the failed certificate check, until GetVerifyCertError */
    int sock_err;      /* the stack's error (SNX_E*) under the last TLS call */
    int alert;         /* the fatal alert the server sent during it, -1 for none */
    u8 option[SSL_OPT_COUNT];
    char host[SSL_HOST_MAX + 1];
} SslConn;
#define SSL_CONTEXT_MAX 32 /* ISslContexts: a game keeps one or two (its own, its network library's) */
#define SSL_CONTEXT_CONNECTIONS 8 /* per context: Switchbrew, from ISslContextForSystem's CreateConnectionEx allowing 10 */
#define SSL_CONN_MAX (SSL_CONTEXT_MAX * SSL_CONTEXT_CONNECTIONS) /* ISslConnections: as many as the contexts take */
typedef struct {
    int contexts; /* GetContextCount */
    int init_done;
    struct stack_st_X509* roots; /* from a bundle file or the Windows store */
    char roots_dir[4096];        /* a hashed directory (or a list), which OpenSSL reads as it needs */
    struct bio_method_st* bio;
    SslContext ctxs[SSL_CONTEXT_MAX];
    SslConn conns[SSL_CONN_MAX];
    u32 ctx_next, conn_next;
} HleSsl;
/* ssl's host side: locks and condition variables, which a save state never holds (hle.h's g_host) */
typedef struct {
    /* the host's roots, read once under init_lk */
    PMutex init_lk;
} HostSsl;
#endif
