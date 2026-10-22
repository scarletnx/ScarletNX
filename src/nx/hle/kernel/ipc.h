/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* HIPC/CMIF requests as the services see them: the parsed request and its reply (IpcCtx), service objects, sessions
   and domains, and the service registry. ipc.c parses and answers; the dispatchers tools/swipc generates call the
   inline helpers below, so their names and signatures are part of the generator's output */
#ifndef SNX_IPC_H
#define SNX_IPC_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/hle/result.h" /* RC, RC_OK and the R_ helpers */
#include "common/utilities/endian.h"
#include <string.h>

#define MAX_BUFS 8 /* descriptors of one kind a request may carry */

/* a buffer descriptor: A (send), B (receive), W (exchange), X (pointer) or C (receive list) */
typedef struct {
    u64 addr, size;
    u32 flags;
} IpcBuf;

struct SvcObj;

/* One request, parsed from the client's message (TLS's 0x100 bytes, or SendSyncRequestWithUserBuffer's buffer), and
   the reply the handler builds */
/* an entry of the crash report's ring of requests (ipc.c): what became of one, formatted only when a report prints it.
   name is the service object's, which lives as long as the process (sm.c keeps the names of its stand-ins), and what
   a literal */
enum {
    IPC_REC_RUNNING,
    IPC_REC_SERVED,
    IPC_REC_ANSWERED,       /* sf answered it without the service, for the reason in what */
    IPC_REC_CLOSE_OBJECT,   /* a domain's Close, the object in cmd */
    IPC_REC_NOT_SESSION,    /* the handle in cmd */
    IPC_REC_SESSION_CLOSED,
    IPC_REC_KERNEL_REFUSED, /* the kernel's result in rc */
    IPC_REC_TYPE_CLOSED     /* a message type sf doesn't serve closed the session */
};
typedef struct {
    const char *name, *what;
    u32 kind, type, cmd, in[2], rc;
} IpcRec;

typedef struct IpcCtx {
    u8* msg;
    u64 msg_size;
    u32 type;
    u32 cmd;
    u32 token;
    const u8* raw; /* the arguments after the CMIF header */
    u32 raw_size;
    u64 pid;
    int has_pid;
    u32 copy_h[8]; /* a request carries copy handles only: the kernel refuses move handles */
    int ncopy;
    IpcBuf a[MAX_BUFS], b[MAX_BUFS], w[MAX_BUFS], x[MAX_BUFS], c[MAX_BUFS];
    int na, nb, nw, nx, nc;
    /* refusals, decided while parsing: kernel_rc as the kernel answers before any server sees the request, header_rc
       (sf's header checks) at once, domain_rc (sf's domain checks) once the target object is found */
    u32 kernel_rc;
    u32 domain_rc;
    u32 header_rc;
    int is_domain;
    u32 domain_obj;
    u32 in_objs[8];
    int nin_objs;
    int tipc;
    /* the reply */
    u32 rc;
    u8 out[0x100]; /* the raw data; writes are bounded by its sizeof */
    u32 out_size;
    u32 out_copy[8], out_move[8];
    int nout_copy, nout_move;
    struct SvcObj* out_objs[8];
    int nout_objs;
    struct KSession* sess;
} IpcCtx;

/* An object a session or a domain entry refers to: a service, or an interface one of its commands returned */
typedef struct SvcObj {
    const char* name;
    int (*dispatch)(struct SvcObj* self, IpcCtx* ctx);
    void (*destroy)(struct SvcObj* self);
    int refs;
    u32 ptrbuf; /* the server's pointer buffer size, which QueryPointerBufferSize answers (0x8000 unless set) */
    void* data;   /* a slot of its service's pool in g_hle (svcobj_new_data), or NULL */
    u32 datasize; /* the slot's size, zeroed as the object goes */
    u64 u[8];
} SvcObj;

typedef struct KSession {
    KObj o; /* first, as in every kernel object */
    SvcObj* obj;
    /* NULL until ConvertCurrentObjectToDomain, then set once with an atomic store that other threads' requests load,
       and shared by the session's clones */
    struct Domain* domain;
    int closed; /* the server closed it: requests answer R_SESSIONCLOSED */
} KSession;

/* A domain's objects, numbered from 1 (id = index + 1): a closed one leaves a hole the next object takes, as sf
   numbers them. The sessions sharing it (clones) count in refs. ipc.c's pool lock guards obj and n */
#define DOMAIN_OBJECTS 1024
typedef struct Domain {
    SvcObj* obj[DOMAIN_OBJECTS];
    int n; /* the slots ever taken: obj[n] and those after it are free */
    int refs;
} Domain;

typedef SvcObj* (*SvcCtor)(void);

SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*));
/* an object whose data is a slot of a pool in g_hle (hle.h), count slots of size bytes at base, searched from *next.
   The data's type starts with a u32 the object's pool index plus 1 is written to (non-zero, so the slot is taken, as
   hle.h's pools require), and the pool is guarded by ipc.c's lock: the object takes the slot zeroed but for it, and
   zeroes it as it goes, after its destroy */
SvcObj* svcobj_new_data(const char* name, int (*dispatch)(SvcObj*, IpcCtx*), void* base, size_t size, u32 count,
                        u32* next, const char* what);
#define SVCOBJ_NEW_DATA(name, dispatch, arr, next, what)                                                               \
    svcobj_new_data((name), (dispatch), (arr), sizeof *(arr), (u32)(sizeof(arr) / sizeof *(arr)), &(next), (what))
void svcobj_ref(SvcObj* o);
void svcobj_unref(SvcObj* o);
u32 session_new(SvcObj* obj);
/* SendSyncRequestWithUserBuffer's buffer checked as the kernel locks it: 0, or the SVC's result */
u32 ipc_user_buffer(u64 addr, u64 size);
/* SendSyncRequest(WithUserBuffer)'s request on handle, served on t: the SVC's result */
u32 ipc_handle(u32 handle, u64 msg_addr, u64 msg_size);
void ipc_out_overflow(IpcCtx* c); /* a reply that didn't fit: logged once */
void out_copy_h(IpcCtx* c, u32 h);
void out_move_h(IpcCtx* c, u32 h);
void out_obj(IpcCtx* c, SvcObj* o);
/* the answers to a request a command can't take, each logged: sf's InvalidCmifRequest, and InvalidNumInObjects (or
   NotSupported for objects sent on a session that isn't a domain) */
int ipc_bad_request(SvcObj* o, IpcCtx* c);
int ipc_bad_objects(SvcObj* o, IpcCtx* c);
SvcObj* ipc_in_obj(IpcCtx* c, int i); /* the request's object i, with a reference the caller drops */
/* for commands nothing implements: success logged, success silent, and sf's UnknownCommandId logged */
int stub_ok(SvcObj* o, IpcCtx* c);
int quiet_ok(SvcObj* o, IpcCtx* c);
int ipc_unknown(SvcObj* o, IpcCtx* c);
/* 1 the first time a (service, command) pair comes, among the first WARN_PAIRS pairs (names told apart by their
   first 127 characters), else 0 */
int ipc_warn_once(const char* name, u32 cmd);
void svc_register(const char* name, SvcCtor ctor);
SvcObj* svc_open(const char* name);
int svc_registered(const char* name);

/* 1 when n more bytes fit in the reply's raw data, else logs the overflow and returns 0 */
static inline int out_fits(IpcCtx* c, u32 n) {
    if (c->out_size <= sizeof c->out && n <= sizeof c->out - c->out_size) return 1;
    ipc_out_overflow(c);
    return 0;
}
/* appends v, a host number, to the reply's raw data (little-endian) when it fits; dropped (and logged) otherwise */
static inline void out32(IpcCtx* c, u32 v) {
    if (!out_fits(c, 4)) return;
    snx_st_le32(c->out + c->out_size, v);
    c->out_size += 4;
}
/* n bytes of the request's raw data at off, zeros when they are not all there. The bytes are the guest's
   (little-endian): the generated dispatchers convert what they read with ipc_in, and what they write with ipc_out, to
   and from the host's order, so a handler's raw arguments and outputs are host numbers. Buffers stay guest bytes */
static inline void ipc_in(IpcCtx* c, u32 off, void* dst, u32 n) {
    if (!n) return;
    if (off > c->raw_size || n > c->raw_size - off)
        memset(dst, 0, n);
    else
        memcpy(dst, c->raw + off, n);
}
/* n bytes into the reply at off. Unchecked: the generator lays out a command's outputs inside out */
static inline void ipc_out(IpcCtx* c, u32 off, const void* src, u32 n) {
    if (n) memcpy(c->out + off, src, n);
}
/* list[i] of a list of n, or an empty buffer past its end */
static inline IpcBuf ipc_buf(const IpcBuf* list, int n, int i) {
    IpcBuf none;
    if (i >= 0 && i < n) return list[i];
    memset(&none, 0, sizeof none);
    return none;
}
/* an AutoSelect buffer: the mapped descriptor when its address isn't 0, whatever its size, else the pointer one */
static inline IpcBuf ipc_buf_auto(const IpcBuf* map, int nmap, int i, const IpcBuf* ptr, int nptr, int j) {
    IpcBuf m = ipc_buf(map, nmap, i);
    return m.addr ? m : ipc_buf(ptr, nptr, j);
}
/* 1 when every X descriptor past the first nx is empty (libnx sends the unused half of an AutoSelect buffer) */
static inline int ipc_x_extra_empty(const IpcCtx* c, int nx) {
    int k;
    for (k = nx < 0 ? 0 : nx; k < c->nx && k < MAX_BUFS; k++)
        if (c->x[k].size) return 0;
    return 1;
}
/* sf's check of a request against the command's declaration: the pid flag, the A and B counts exactly, at least nx X
   descriptors with any more empty, no W, the copy handles exactly, and enough raw data (tipc_raw for a TIPC request,
   whose pid has no raw slot). C descriptors aren't checked */
static inline int ipc_counts(const IpcCtx* c, int pid, int na, int nb, int nx, int ncopy, u32 raw, u32 tipc_raw) {
    return (c->has_pid != 0) == (pid != 0) && c->na == na && c->nb == nb && c->nx >= nx && ipc_x_extra_empty(c, nx) &&
           c->nw == 0 && c->ncopy == ncopy && c->raw_size >= (c->tipc ? tipc_raw : raw);
}
/* a domain request carries exactly the objects the command declares */
static inline int ipc_objects(const IpcCtx* c, int nobj) { return !c->is_domain || c->nin_objs == nobj; }

/* the last 32 requests, for crash reports: how many there have been, and request seq's entry (seq & 31) as text */
u32 ipc_ring_count(void);
void ipc_ring_text(u32 seq, char* out, size_t n);
/* ---- HLE state in g_hle (hle.h) */
/* Service objects, sessions and domains each live in a pool (hle.h) */
#define SVCOBJ_MAX  8192 /* sessions' objects, domains', and those services keep (am's storages) */
#define SESSION_MAX (H_MAX + THREAD_MAX + 1) /* each is made with a handle, and only a request in flight outlives it */
#define DOMAIN_MAX  64 /* one per session that called ConvertCurrentObjectToDomain, shared by its clones */
#define WARN_PAIRS  512 /* the (service, command) pairs ipc_warn_once reports */
#define WARN_SLOTS  (2 * WARN_PAIRS) /* its hash table, kept half empty so a lookup ends at a free slot soon */
/* the names sm hands out: the runtime registers about a hundred (services.c), and 256 leaves room for more */
#define SVCREG_MAX  256

/* a name sm hands out, with the constructor of a new object for every session */
typedef struct {
    const char* name;
    SvcCtor ctor;
} SvcReg;

/* ipc.c's. lk guards the pools (taking and freeing, and every domain's objects), every service's pool of object data
   (svcobj_new_data), the warned pairs and the registry: none of them is held long */
typedef struct {
    SvcObj svcobjs[SVCOBJ_MAX];
    KSession sessions[SESSION_MAX];
    Domain domains[DOMAIN_MAX];
    u32 svcobj_next, session_next, domain_next;
    /* ipc_warn_once's reported (service, command) pairs, open addressing on a hash of both. A slot is free while its
       name is empty (names compare as their first 127 characters; an empty one is kept as "?") */
    struct {
        char name[128];
        u32 cmd;
    } seen[WARN_SLOTS];
    int nseen, warn_full; /* pairs reported; whether the log was told the table is full */
    /* the service registry, a slot array: a live entry has a name, and none is ever freed. Registering more asserts */
    SvcReg reg[SVCREG_MAX];
    u32 reg_next;
    /* the crash report's ring of the last 32 requests: ring_n counts every request, on every thread */
    IpcRec ring[32];
    char apart[64];
    u32 ring_n;
} HleIpc;
/* ipc's host side: locks and condition variables, which a save state never holds (hle.h's g_host) */
typedef struct {
    PMutex lk;
} HostIpc;
#endif
