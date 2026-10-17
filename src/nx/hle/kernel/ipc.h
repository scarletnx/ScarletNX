/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* IPC (ipc.c): service objects, sessions, the request and reply helpers, the service registry */
#ifndef SNX_IPC_H
#define SNX_IPC_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/hle/kernel/kernel.h"
#include <string.h>

#define MAX_BUFS 8
typedef struct {
    u64 addr, size;
    u32 flags;
} IpcBuf;
struct SvcObj;
typedef struct IpcCtx {
    u8* msg;
    u64 msg_size;
    u32 type;
    u32 cmd;
    u32 token;
    const u8* raw;
    u32 raw_size; /* raw args after the CMIF header */
    u64 pid;
    int has_pid;
    u32 copy_h[8]; /* a request has no move handles: the kernel refuses those (parse) */
    int ncopy;
    IpcBuf a[MAX_BUFS], b[MAX_BUFS], w[MAX_BUFS], x[MAX_BUFS], c[MAX_BUFS];
    int na, nb, nw, nx, nc;
    u32 kernel_rc; /* the kernel fails the request before the server sees it: a header that describes more than the
                      client's buffer, a receive list inside the raw data or a move handle (InvalidCombination), raw
                      data past the server's 0x100 bytes (MessageTooLarge), a buffer outside the address space */
    u32 domain_rc; /* sf refuses a domain request whose headers don't fit its data, with more than 8 objects or a
                         bad CmifInHeader, once the target is found */
    u32 header_rc; /* sf refuses a request whose headers don't fit its data, or a bad CmifInHeader, at once */
    int is_domain;
    u32 domain_obj;
    u32 in_objs[8];
    int nin_objs;
    int tipc;
    /* reply */
    u32 rc;
    u8 out[0x100];
    u32 out_size;
    u32 out_copy[8], out_move[8];
    int nout_copy, nout_move;
    struct SvcObj* out_objs[8];
    int nout_objs;
    struct KSession* sess;
} IpcCtx;

typedef struct SvcObj {
    const char* name;
    int (*dispatch)(struct SvcObj* self, IpcCtx* ctx);
    void (*destroy)(struct SvcObj* self);
    int refs;
    /* its server's pointer buffer, which QueryPointerBufferSize answers: 0x8000 unless the service sets another. libnx
       sends an AutoSelect buffer through the pointer buffer when it fits there, and mapped otherwise */
    u32 ptrbuf;
    void* data;
    u64 u[8];
} SvcObj;

typedef struct KSession {
    KObj o;
    SvcObj* obj;
    /* NULL until ConvertCurrentObjectToDomain, set once (atomically: another thread's request reads it), and shared
       by cloned sessions, same as HOS */
    struct Domain* domain;
    int closed; /* the server closed it (a Close, or a message type sf refuses): requests answer SessionClosed */
} KSession;
typedef struct Domain {
    SvcObj** obj;
    int n, cap, refs;
    PMutex lk;
} Domain;

SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*), size_t datasize);
void svcobj_ref(SvcObj* o);
void svcobj_unref(SvcObj* o);
u32 session_new(SvcObj* obj);
/* SendSyncRequestWithUserBuffer's buffer, checked as the kernel does: 0, or the result the SVC answers */
u32 ipc_user_buffer(u64 addr, u64 size);
/* a request in the client's message buffer, msg_size bytes (TLS's 0x100, or SendSyncRequestWithUserBuffer's) */
void ipc_handle(KThread* t, u32 handle, u64 msg_addr, u64 msg_size);
/* building a reply by hand (the control requests, a service's fallback). What doesn't fit in out[] is dropped, and
   logged once */
void ipc_out_overflow(IpcCtx* c);
static inline int out_fits(IpcCtx* c, u32 n) {
    if (n <= sizeof c->out - c->out_size) return 1;
    ipc_out_overflow(c);
    return 0;
}
static inline void out32(IpcCtx* c, u32 v) {
    if (!out_fits(c, 4)) return;
    memcpy(c->out + c->out_size, &v, 4);
    c->out_size += 4;
}
void out_copy_h(IpcCtx* c, u32 h);
void out_move_h(IpcCtx* c, u32 h);
void out_obj(IpcCtx* c, SvcObj* o);
/* what the dispatchers generated from the .swipc files use (tools/swipc/swipc.c). The generator places every output
   inside out[]. A request shorter than its command's raw data is refused (ipc_counts), so ipc_in's zeros only matter
   to a dispatcher written by hand */
static inline void ipc_in(IpcCtx* c, u32 off, void* dst, u32 n) {
    if (off + n <= c->raw_size)
        memcpy(dst, c->raw + off, n);
    else
        memset(dst, 0, n);
}
static inline void ipc_out(IpcCtx* c, u32 off, const void* src, u32 n) { memcpy(c->out + off, src, n); }
static inline IpcBuf ipc_buf(const IpcBuf* list, int n, int i) {
    IpcBuf none = {0, 0, 0};
    return i < n ? list[i] : none;
}
/* an AutoSelect buffer: the client sends it mapped (A or B) or through the pointer buffer (X or C), and the other
   descriptor empty. A mapped descriptor with an address is taken whatever its size, as Atmosphere's sf does */
static inline IpcBuf ipc_buf_auto(const IpcBuf* map, int nmap, int i, const IpcBuf* ptr, int nptr, int j) {
    IpcBuf b = ipc_buf(map, nmap, i);
    return b.addr ? b : ipc_buf(ptr, nptr, j);
}
/* whether the X descriptors past the first nx carry nothing: the empty half of an AutoSelect buffer that libnx sends
   beside the mapped one a command takes, which the console takes (edenprobe's nsd Resolve) */
static inline int ipc_x_extra_empty(const IpcCtx* c, int nx) {
    for (int i = nx; i < c->nx; i++)
        if (c->x[i].size) return 0;
    return 1;
}
/* whether a request sends what its command declares, as sf's HipcCommandProcessor checks it: the pid flag, the A, B
   and X descriptors (an AutoSelect buffer counts in both of its lists, and an X descriptor past them must be empty),
   no exchange buffer, the copy handles, and at least the command's raw data: raw bytes, or tipc_raw for a TIPC
   request, whose pid has no raw slot. C descriptors aren't checked */
static inline int ipc_counts(const IpcCtx* c, int pid, int na, int nb, int nx, int ncopy, u32 raw, u32 tipc_raw) {
    return c->has_pid == pid && c->na == na && c->nb == nb && c->nx >= nx && ipc_x_extra_empty(c, nx) && !c->nw &&
           c->ncopy == ncopy && c->raw_size >= (c->tipc ? tipc_raw : raw);
}
/* and a domain request, the objects it declares (sf's DomainServiceObjectProcessor, which checks them first) */
static inline int ipc_objects(const IpcCtx* c, int nobj) { return !c->is_domain || c->nin_objs == nobj; }
/* The refusals below (and the header ones in ipc.c) answer the result in a reply. sf closes the session instead, and
   the client's SendSyncRequest gets SessionClosed: the runtime keeps the session and replies, because its own
   declarations can be wrong where the console's aren't, and a game may survive an error result but not a dead
   session. The log shows each one */
int ipc_bad_request(SvcObj* o, IpcCtx* c); /* sf's InvalidCmifRequest for a request that doesn't, logged */
/* a domain request with another number of objects (InvalidNumInObjects), or objects a plain session can't send
   (NotSupported), logged */
int ipc_bad_objects(SvcObj* o, IpcCtx* c);
SvcObj* ipc_in_obj(IpcCtx* c, int i); /* the request's object i (domains only), with a reference to drop */
#define RC(mod, desc) ((mod) | ((desc) << 9))
#define RC_OK         0
int stub_ok(SvcObj* o, IpcCtx* c);     /* success for a command a service doesn't know, logged */
int quiet_ok(SvcObj* o, IpcCtx* c);    /* the same, silent */
int ipc_unknown(SvcObj* o, IpcCtx* c); /* what the console answers it: sf's UnknownCommandId, logged */
/* 1 the first three times a service command is seen unhandled (0 for a new one once 512 are counted) */
int ipc_warn_once(const char* name, u32 cmd);
extern char g_ipc_ring[32][160]; /* the last requests, for crash reports */
extern u32 g_ipc_ring_n;

/* service registry */
typedef SvcObj* (*SvcCtor)(void);
void svc_register(const char* name, SvcCtor ctor);
SvcObj* svc_open(const char* name);
int svc_registered(const char* name);

#endif
