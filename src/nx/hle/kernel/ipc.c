/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/ipc_helpers.h, the TIPC reply's header type):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: 2016 Citra Emulator Project */
/* IPC: service objects and sessions, HIPC/CMIF/TIPC requests parsed and answered as the kernel and the system's sf
   framework do (Switchbrew's IPC_Marshalling page for the wire format), domains, and the service registry */
#include "nx/hle/kernel/kernel_int.h"
#include "nx/runtime/options.h"
#include <stdarg.h>

#define SFCI 0x49434653u /* the CmifInHeader's magic */
#define SFCO 0x4F434653u /* the CmifOutHeader's */
/* sf's results (module 10, cmif) and hipc's (module 11) */
#define RC_NOTSUPPORTED     RC(10, 1)
#define RC_BADHEADERSIZE    RC(10, 202)
#define RC_BADINHEADER      RC(10, 211)
#define RC_UNKNOWNCOMMAND   RC(10, 221)
#define RC_BADNUMINOBJECTS  RC(10, 235)
#define RC_TARGETNOTFOUND   RC(10, 261)
#define RC_BADCMIFREQUEST   RC(11, 420)
#define RC_TARGETNOTDOMAIN  RC(11, 491)
#define RC_DOMAINOBJNOTFOUND RC(11, 492)
/* the server's message buffer, which the raw data and every reply fit in */
#define SERVER_MSG 0x100u
/* what QueryPointerBufferSize answers: the size of the server's pointer buffer for X and C descriptors */
#define POINTER_BUFFER 0x8000u
/* IpcCtx.is_domain: a request on a domain session outside a domain message (a control request, answered plainly), and
   one in a domain message (SendMessage), whose reply is a domain one */
enum { IPC_DOMAIN_SESSION = 1, IPC_DOMAIN_MESSAGE = 2 };

static u32 rd32(const u8* p) {
    u32 v;
    memcpy(&v, p, 4);
    return v;
}
static void wr32(u8* p, u32 v) { memcpy(p, &v, 4); }

/* ============================================================ the crash report's ring */
char g_ipc_ring[32][160];
u32 g_ipc_ring_n;
/* each request takes the next entry, served or not; the number it returns names it */
static u32 ring_claim(void) { return __atomic_fetch_add(&g_ipc_ring_n, 1, __ATOMIC_RELAXED); }
/* rewrites entry seq, unless 32 later requests have taken its slot meanwhile (a service that ran long) */
static void ring_set(u32 seq, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void ring_set(u32 seq, const char* fmt, ...) {
    va_list a;
    if (__atomic_load_n(&g_ipc_ring_n, __ATOMIC_RELAXED) - seq > 32) return;
    va_start(a, fmt);
    vsnprintf(g_ipc_ring[seq & 31], sizeof g_ipc_ring[0], fmt, a);
    va_end(a);
}

/* ============================================================ service objects */
/* the object and its data in one allocation, the data 16-aligned */
#define SVCOBJ_HEAD ((sizeof(SvcObj) + 15) & ~(size_t)15)
SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*), size_t datasize) {
    SvcObj* o;
    if (datasize > (size_t)-1 - SVCOBJ_HEAD) fatal("Service object %s too large", name);
    o = calloc(1, SVCOBJ_HEAD + datasize);
    if (!o) fatal("Out of memory for the service object %s", name);
    o->name = name;
    o->dispatch = dispatch;
    o->refs = 1;
    o->data = datasize ? (u8*)o + SVCOBJ_HEAD : NULL;
    return o;
}
void svcobj_ref(SvcObj* o) {
    if (o) __atomic_add_fetch(&o->refs, 1, __ATOMIC_RELAXED);
}
void svcobj_unref(SvcObj* o) {
    if (!o || __atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL)) return;
    if (o->destroy) o->destroy(o);
    free(o);
}

/* ============================================================ domains */
/* A domain's objects are numbered from 1 (id = index + 1); a closed one leaves a hole the next object takes. The
   sessions sharing it (clones) count in refs */
static Domain* domain_new(SvcObj* root) {
    Domain* d = calloc(1, sizeof *d);
    if (!d || !(d->obj = calloc(8, sizeof *d->obj))) fatal("Out of memory for a domain");
    d->cap = 8;
    d->obj[0] = root;
    d->n = 1;
    d->refs = 1;
    pm_init(&d->lk);
    return d;
}
static void domain_unref(Domain* d) {
    int i;
    if (!d || __atomic_sub_fetch(&d->refs, 1, __ATOMIC_ACQ_REL)) return;
    for (i = 0; i < d->n; i++) svcobj_unref(d->obj[i]);
    free(d->obj);
    free(d);
}
/* object id, with a reference to drop, or NULL */
static SvcObj* domain_get(Domain* d, u32 id) {
    SvcObj* o = NULL;
    if (!d) return NULL;
    pm_lock(&d->lk);
    if (id && id <= (u32)d->n) {
        o = d->obj[id - 1];
        svcobj_ref(o);
    }
    pm_unlock(&d->lk);
    return o;
}
/* o joins the domain with its reference: its id */
static u32 domain_add(Domain* d, SvcObj* o) {
    int i;
    pm_lock(&d->lk);
    for (i = 0; i < d->n && d->obj[i]; i++) {
    }
    if (i == d->cap) {
        SvcObj** n = realloc(d->obj, sizeof *n * (size_t)d->cap * 2);
        if (!n) fatal("Out of memory for a domain's objects");
        d->obj = n;
        d->cap *= 2;
    }
    if (i == d->n) d->n++;
    d->obj[i] = o;
    pm_unlock(&d->lk);
    return (u32)i + 1;
}
/* object id leaves the domain: its reference, for the caller to drop outside the lock, or NULL */
static SvcObj* domain_take(Domain* d, u32 id) {
    SvcObj* o = NULL;
    pm_lock(&d->lk);
    if (id && id <= (u32)d->n) {
        o = d->obj[id - 1];
        d->obj[id - 1] = NULL;
    }
    pm_unlock(&d->lk);
    return o;
}

/* ============================================================ sessions */
static void session_destroy(KObj* o) {
    KSession* s = (KSession*)o;
    svcobj_unref(s->obj);
    domain_unref(s->domain);
    free(o->waiters);
    free(s);
}
/* a session to obj and domain, whose references it takes: its handle (0 when the table is full) */
static u32 session_make(SvcObj* obj, Domain* d) {
    KSession* s = calloc(1, sizeof *s);
    u32 h;
    if (!s) fatal("Out of memory for a session");
    s->o.type = KO_SESSION;
    s->o.destroy = session_destroy;
    s->o.name = obj ? obj->name : "session";
    s->obj = obj;
    s->domain = d;
    kobj_ref(&s->o); /* so a full handle table destroys it here */
    h = h_new(&s->o);
    kobj_unref(&s->o);
    return h;
}
u32 session_new(SvcObj* obj) { return session_make(obj, NULL); }

/* ============================================================ requests: the kernel's part */
/* the layout of a HIPC message: offsets from its start */
typedef struct {
    u32 data;    /* the data words */
    u32 raw_end; /* their end */
    u32 ndata;   /* how many */
} Hipc;
static int out_of_space(u64 addr, u64 size) { return size && (addr >= AS_SIZE || size > AS_SIZE - addr); }
/* Reads the HIPC headers and descriptors into c, checking what the kernel checks before the server gets the request
   (Mesosphere's order): the message with its receive list in the client's buffer (InvalidCombination; with a
   receive-list offset the raw data isn't counted here), raw data within the server's buffer (MessageTooLarge), no
   move handles (InvalidCombination), every buffer inside the address space (InvalidCurrentMemory), and a receive
   list after the raw data (InvalidCombination, which the console finds at the reply). A header may announce up to 15
   descriptors of a kind: all are read and checked, the first MAX_BUFS kept. 0, or the result in kernel_rc */
static u32 parse_hipc(IpcCtx* c, Hipc* h) {
    const u8* m = c->msg;
    u32 w0 = rd32(m), w1 = rd32(m + 4), off = 8, ncopy = 0, nmove = 0, cmode, rloff, rl, i, nx, na, nb, nw, nc;
    c->type = w0 & 0xFFFF;
    nx = (w0 >> 16) & 15;
    na = (w0 >> 20) & 15;
    nb = (w0 >> 24) & 15;
    nw = w0 >> 28;
    h->ndata = w1 & 0x3FF;
    cmode = (w1 >> 10) & 15;
    rloff = (w1 >> 20) & 0x7FF;
    if (w1 >> 31) {
        u32 sh = rd32(m + 8);
        c->has_pid = (int)(sh & 1);
        ncopy = (sh >> 1) & 15;
        nmove = (sh >> 5) & 15;
        off = 12 + (c->has_pid ? 8u : 0u);
        for (i = 0; i < ncopy; i++)
            if (i < 8) c->copy_h[c->ncopy++] = rd32(m + off + 4 * i);
        off += 4 * (ncopy + nmove);
        if (c->has_pid) c->pid = K_PROCESS_ID; /* the kernel's, whatever the client wrote */
    }
    h->data = off + 8 * nx + 12 * (na + nb + nw);
    h->raw_end = h->data + 4 * h->ndata;
    nc = cmode == 2 ? 1 : cmode > 2 ? cmode - 2 : 0;
    rl = rloff ? rloff * 4 : h->raw_end;
    if ((u64)rl + 8 * (u64)nc > c->msg_size) return c->kernel_rc = R_BADCOMBO;
    if (h->raw_end > SERVER_MSG) return c->kernel_rc = R_MSGTOOLARGE;
    if (nmove) return c->kernel_rc = R_BADCOMBO;
    for (i = 0; i < nx; i++, off += 8) { /* X: the pointer buffer's send statics */
        u32 d0 = rd32(m + off), d1 = rd32(m + off + 4);
        IpcBuf b;
        b.addr = d1 | (u64)((d0 >> 12) & 15) << 32 | (u64)((d0 >> 6) & 7) << 36;
        b.size = d0 >> 16;
        b.flags = (d0 & 63) | ((d0 >> 9) & 7) << 6; /* the static's index */
        if (out_of_space(b.addr, b.size)) return c->kernel_rc = R_BADMEMSTATE;
        if (i < MAX_BUFS) c->x[c->nx++] = b;
    }
    for (i = 0; i < na + nb + nw; i++, off += 12) { /* A, B, W: mapped buffers */
        u32 d0 = rd32(m + off), d1 = rd32(m + off + 4), d2 = rd32(m + off + 8);
        IpcBuf b;
        b.addr = d1 | (u64)(d2 >> 28) << 32 | (u64)((d2 >> 2) & 0x3FFFFF) << 36;
        b.size = d0 | (u64)((d2 >> 24) & 15) << 32;
        b.flags = d2 & 3;
        if (out_of_space(b.addr, b.size)) return c->kernel_rc = R_BADMEMSTATE;
        if (i < na) {
            if (i < MAX_BUFS) c->a[c->na++] = b;
        } else if (i < na + nb) {
            if (i - na < MAX_BUFS) c->b[c->nb++] = b;
        } else if (i - na - nb < MAX_BUFS)
            c->w[c->nw++] = b;
    }
    for (i = 0; i < nc; i++) { /* C: the receive list */
        u32 d0 = rd32(m + rl + 8 * i), d1 = rd32(m + rl + 8 * i + 4);
        IpcBuf b;
        b.addr = d0 | (u64)(d1 & 0xFFFF) << 32;
        b.size = d1 >> 16;
        b.flags = 0;
        if (out_of_space(b.addr, b.size)) return c->kernel_rc = R_BADMEMSTATE;
        if (i < MAX_BUFS) c->c[c->nc++] = b;
    }
    if (nc && rloff && rl < h->raw_end) return c->kernel_rc = R_BADCOMBO;
    return 0;
}
/* why the kernel refused a request, for the log */
static const char* kernel_refusal(u32 rc) {
    switch (rc) {
        case R_BADCOMBO:
            return "a header describing more than the message buffer, a move handle or a misplaced receive list";
        case R_MSGTOOLARGE: return "raw data past the server's 0x100 bytes";
        case R_BADMEMSTATE: return "a buffer outside the address space";
        default: return "?";
    }
}

/* SendSyncRequestWithUserBuffer's buffer, as the kernel locks it: page-aligned, whole pages, inside the address space,
   and one block's worth of state and permission across it that allows IPC user buffers (the state's
   FlagCanIpcUserBuffer, Switchbrew's MemoryState), read-write, with no attribute */
#define MS_FLAG_IPC_USER_BUFFER (1u << 21)
u32 ipc_user_buffer(u64 addr, u64 size) {
    MemBlock b;
    u64 a = addr;
    u32 state = 0, perm = 0;
    if (addr & 0xFFF) return R_BADADDR;
    if (!size || (size & 0xFFF)) return R_BADSIZE;
    if (addr >= AS_SIZE || size > AS_SIZE - addr) return R_BADMEMSTATE;
    while (a < addr + size) {
        mem_query(a, &b);
        if (a == addr) {
            state = b.state;
            perm = b.perm;
        }
        if (b.state != state || b.perm != perm || b.attr || !(b.state & MS_FLAG_IPC_USER_BUFFER) || b.perm != PERM_RW ||
            !b.size)
            return R_BADMEMSTATE;
        a = b.addr + b.size;
    }
    return 0;
}

/* ============================================================ requests: sf's part */
/* the CMIF headers of a request (types 4 to 7), as sf measures them: the data region starts at the data words aligned
   to 16 and is 16 bytes shorter than they are. A domain message's CmifInHeader is inside its payload. Refusals go in
   header_rc (before anything else) and domain_rc (once the domain's target is found) */
static void parse_cmif(IpcCtx* c, const Hipc* h, int domain) {
    const u8* d = c->msg + ((h->data + 15) & ~15u);
    i32 avail = (i32)(4 * h->ndata) - 16, payload = avail;
    if (domain) {
        u32 nobj;
        c->is_domain = IPC_DOMAIN_MESSAGE; /* refused or not, the reply is a domain one */
        if (avail < 16) {
            c->header_rc = RC_BADHEADERSIZE;
            return;
        }
        nobj = d[1];
        payload = d[2] | d[3] << 8;
        c->domain_obj = rd32(d + 4);
        c->token = rd32(d + 12);
        if (d[0] == 2) /* Close: nothing more */
            return;
        if (d[0] != 1)
            c->domain_rc = RC_BADINHEADER;
        else if (16 + payload + 4 * (i32)nobj > avail)
            c->domain_rc = RC_BADHEADERSIZE;
        else if (nobj > 8)
            c->domain_rc = RC_BADNUMINOBJECTS;
        else {
            u32 i;
            c->nin_objs = (int)nobj;
            for (i = 0; i < nobj; i++) c->in_objs[i] = rd32(d + 16 + payload + 4 * i);
        }
        d += 16;
        if (c->domain_rc) return;
    }
    if (payload < 16) {
        if (domain)
            c->domain_rc = RC_BADHEADERSIZE;
        else
            c->header_rc = RC_BADHEADERSIZE;
        return;
    }
    if (rd32(d) != SFCI || rd32(d + 4) > 1) {
        if (domain)
            c->domain_rc = RC_BADINHEADER;
        else
            c->header_rc = RC_BADINHEADER;
        return;
    }
    c->cmd = rd32(d + 8);
    if (!domain) c->token = rd32(d + 12);
    c->raw = d + 16;
    c->raw_size = (u32)(payload - 16);
}

/* ============================================================ replies */
void out_copy_h(IpcCtx* c, u32 h) {
    if (c->nout_copy < 8) {
        c->out_copy[c->nout_copy++] = h;
        return;
    }
    WARN("ipc: more than 8 copy handles in a reply, one dropped\n");
    h_close(h);
}
void out_move_h(IpcCtx* c, u32 h) {
    if (c->nout_move < 8) {
        c->out_move[c->nout_move++] = h;
        return;
    }
    WARN("ipc: more than 8 move handles in a reply, one dropped\n");
    h_close(h);
}
void out_obj(IpcCtx* c, SvcObj* o) {
    if (c->nout_objs < 8) {
        c->out_objs[c->nout_objs++] = o;
        return;
    }
    WARN("ipc: more than 8 objects in a reply, one dropped\n");
    svcobj_unref(o);
}
void ipc_out_overflow(IpcCtx* c) {
    WARN_LIMIT(1, "ipc: %s cmd %u: the reply doesn't fit its 0x100 bytes, the rest is dropped\n",
               c->sess && c->sess->obj ? c->sess->obj->name : "?", c->cmd);
}
/* a failed request returns no handles, objects or data: the ones its handler made are closed and dropped. Called with
   no lock held, as closing a session's last handle destroys its service */
static void drop_outputs(IpcCtx* c) {
    int i;
    for (i = 0; i < c->nout_copy; i++)
        if (c->out_copy[i]) h_close(c->out_copy[i]);
    for (i = 0; i < c->nout_move; i++)
        if (c->out_move[i]) h_close(c->out_move[i]);
    for (i = 0; i < c->nout_objs; i++) svcobj_unref(c->out_objs[i]);
    c->nout_copy = c->nout_move = c->nout_objs = 0;
    c->out_size = 0;
}
/* Writes the reply over the request: the HIPC header, the handles (on a plain session an object is a new session's
   move handle, ahead of the declared ones), then for CMIF the data words at the next 16-byte boundary (counted with 16
   bytes of padding, as sf counts them), a domain header in a domain message's reply, the CmifOutHeader, the data and
   a domain message's object ids; for TIPC the result and the data right after the handles, under the request's type.
   What doesn't fit the server's 0x100 bytes is cut, and logged */
static void respond(IpcCtx* c) {
    u8* m = c->msg;
    int dom = c->is_domain == IPC_DOMAIN_MESSAGE, i, nmove = 0;
    u32 moves[16], ids[8], nids = 0, off, start, words, limit, hdrs = 16 + (dom ? 16u : 0u), size;
    if (c->rc) drop_outputs(c);
    for (i = 0; i < c->nout_objs; i++) {
        SvcObj* o = c->out_objs[i];
        if (dom)
            ids[nids++] = o ? domain_add(c->sess->domain, o) : 0;
        else
            moves[nmove++] = o ? session_new(o) : 0;
    }
    for (i = 0; i < c->nout_move; i++) moves[nmove++] = c->out_move[i];
    while (nmove > 15) { /* the special header counts 15 at most */
        WARN_LIMIT(4, "ipc: more than 15 move handles in a reply, the rest dropped\n");
        if (moves[--nmove]) h_close(moves[nmove]);
    }
    off = 8;
    if (c->nout_copy || nmove) off = 12 + 4 * (u32)(c->nout_copy + nmove);
    start = c->tipc ? off : (off + 15) & ~15u;
    limit = SERVER_MSG - start - (c->tipc ? 4 : hdrs) - 4 * nids;
    size = (c->out_size + 3) & ~3u;
    if (size > sizeof c->out) size = sizeof c->out;
    if (size > limit) {
        WARN_LIMIT(8, "ipc: %s cmd %u answers 0x%x bytes, more than its reply holds: cut to 0x%x\n",
                   c->sess && c->sess->obj ? c->sess->obj->name : "?", c->cmd, size, limit);
        size = limit;
    }
    words = c->tipc ? (4 + size) / 4 : (16 + hdrs + size + 4 * nids) / 4;
    wr32(m, c->tipc ? c->type : 0);
    wr32(m + 4, words | (off > 8 ? 0x80000000u : 0u));
    if (off > 8) {
        wr32(m + 8, (u32)c->nout_copy << 1 | (u32)nmove << 5);
        for (i = 0; i < c->nout_copy; i++) wr32(m + 12 + 4 * i, c->out_copy[i]);
        for (i = 0; i < nmove; i++) wr32(m + 12 + 4 * (c->nout_copy + i), moves[i]);
    }
    if (c->tipc) {
        wr32(m + start, c->rc);
        memcpy(m + start + 4, c->out, size);
        return;
    }
    memset(m + off, 0, start - off);
    if (dom) {
        wr32(m + start, nids);
        memset(m + start + 4, 0, 12);
        start += 16;
    }
    wr32(m + start, SFCO);
    wr32(m + start + 4, 0);
    wr32(m + start + 8, c->rc);
    wr32(m + start + 12, c->token);
    memcpy(m + start + 16, c->out, size);
    for (i = 0; i < (int)nids; i++) wr32(m + start + 16 + size + 4 * (u32)i, ids[i]);
}

/* ============================================================ serving */
/* the request goes to o's dispatcher, and its result into the reply */
static void serve(IpcCtx* c, SvcObj* o, u32 seq) {
    u32 in[2];
    ipc_in(c, 0, &in[0], 4);
    ipc_in(c, 4, &in[1], 4);
    ring_set(seq, "%s cmd %u (in %x %x) -> running", o->name, c->cmd, in[0], in[1]);
    LOGV("IPC %s cmd %u\n", o->name, c->cmd);
    c->rc = (u32)o->dispatch(o, c);
    ring_set(seq, "%s cmd %u (in %x %x) -> %x", o->name, c->cmd, in[0], in[1], c->rc);
    respond(c);
}
/* a request sf answers without the service: a refusal, or a control request */
static void answer(IpcCtx* c, u32 rc, u32 seq, const char* name, const char* what) {
    c->rc = rc;
    ring_set(seq, "%s type %u cmd %u -> %x (not served: %s)", name, c->type, c->cmd, rc, what);
    if (rc && strcmp(what, "control")) WARN_LIMIT(16, "ipc: %s: a request refused (%x)\n", name, rc);
    respond(c);
}
/* The control requests (types 5 and 7), which sf serves itself and answers plainly on any session:
   ConvertCurrentObjectToDomain, CopyFromCurrentDomain, CloneCurrentObject, QueryPointerBufferSize and
   CloneCurrentObjectEx */
static u32 control(IpcCtx* c, KSession* s) {
    Domain* d = __atomic_load_n(&s->domain, __ATOMIC_ACQUIRE);
    u32 id = 0;
    switch (c->cmd) {
        case 0:
            if (!d) {
                Domain* n;
                svcobj_ref(s->obj);
                n = domain_new(s->obj);
                if (!__atomic_compare_exchange_n(&s->domain, &d, n, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                    domain_unref(n); /* another thread converted it first */
            }
            out32(c, 1); /* the session's object is the domain's first */
            return 0;
        case 1: {
            SvcObj* o;
            if (!d) return RC_TARGETNOTDOMAIN;
            ipc_in(c, 0, &id, 4);
            o = domain_get(d, id);
            if (!o) return RC_DOMAINOBJNOTFOUND;
            out_move_h(c, session_new(o));
            return 0;
        }
        case 2:
        case 4:
            svcobj_ref(s->obj);
            if (d) __atomic_add_fetch(&d->refs, 1, __ATOMIC_RELAXED);
            out_move_h(c, session_make(s->obj, d));
            return 0;
        case 3: out32(c, POINTER_BUFFER); return 0;
        default:
            if (ipc_warn_once("control", c->cmd))
                WARN("ipc: %s: control request %u, UnknownCommandId\n", s->obj ? s->obj->name : "session", c->cmd);
            return RC_UNKNOWNCOMMAND;
    }
}
/* a request in a domain message: Close, or the target object's command */
static void serve_domain(IpcCtx* c, Domain* d, u32 seq, const char* name) {
    SvcObj* o = domain_get(d, c->domain_obj);
    if (!o) {
        answer(c, RC_TARGETNOTFOUND, seq, name, "object");
        return;
    }
    if (c->domain_rc) {
        svcobj_unref(o);
        answer(c, c->domain_rc, seq, name, "object");
        return;
    }
    if (!c->raw) { /* Close, the one domain message parse_cmif finds no CmifInHeader in */
        svcobj_unref(o);
        svcobj_unref(domain_take(d, c->domain_obj));
        ring_set(seq, "%s type %u close of object %u -> 0 (not served)", name, c->type, c->domain_obj);
        c->rc = 0;
        respond(c);
        return;
    }
    serve(c, o, seq);
    svcobj_unref(o);
}

/* svcSendSyncRequest(WithUserBuffer): the request in the client's message buffer, served on the calling thread, which
   waits for its reply as it would on the console. The kernel's refusals return their result with the message
   untouched; a message type sf doesn't serve, a Close among them, closes the session */
void ipc_handle(KThread* t, u32 handle, u64 msg_addr, u64 msg_size) {
    IpcCtx c;
    Hipc h;
    u32 seq = ring_claim();
    KSession* s = (KSession*)h_get_ref(handle, KO_SESSION);
    Domain* d;
    const char* name;
    if (!s) {
        ring_set(seq, "handle %x -> E401 (not a session)", handle);
        WARN_LIMIT(8, "ipc: a request on handle %x, which isn't a session: InvalidHandle\n", handle);
        t->cpu.x[0] = R_BADHANDLE;
        return;
    }
    name = s->obj ? s->obj->name : "session";
    t->cpu.x[0] = 0;
    if (__atomic_load_n(&s->closed, __ATOMIC_ACQUIRE)) {
        ring_set(seq, "%s -> F601 (session closed)", name);
        t->cpu.x[0] = R_SESSIONCLOSED;
        kobj_unref(&s->o);
        return;
    }
    memset(&c, 0, sizeof c);
    c.msg = GP(msg_addr);
    c.msg_size = msg_size;
    c.sess = s;
    if (parse_hipc(&c, &h)) {
        ring_set(seq, "%s type %u -> %x (not served: the kernel refused it)", name, c.type, c.kernel_rc);
        WARN_LIMIT(8, "ipc: %s: the kernel refuses a request (%x): %s\n", name, c.kernel_rc, kernel_refusal(c.kernel_rc));
        t->cpu.x[0] = c.kernel_rc;
    } else if (c.type >= 16) { /* TIPC: the command in the type, the raw data right after the headers */
        c.tipc = 1;
        c.cmd = c.type - 16;
        c.raw = c.msg + h.data;
        c.raw_size = 4 * h.ndata;
        if (s->obj)
            serve(&c, s->obj, seq);
        else
            answer(&c, RC_UNKNOWNCOMMAND, seq, name, "command");
    } else if (c.type < 4 || c.type > 7) {
        __atomic_store_n(&s->closed, 1, __ATOMIC_RELEASE);
        ring_set(seq, "%s type %u -> F601 (not served, the session is closed)", name, c.type);
        if (c.type != 2 && c.type != 15)
            WARN_LIMIT(4, "ipc: %s: message type %u, which sf doesn't serve: the session is closed\n", name, c.type);
        t->cpu.x[0] = R_SESSIONCLOSED;
    } else {
        int ctl = c.type == 5 || c.type == 7;
        d = __atomic_load_n(&s->domain, __ATOMIC_ACQUIRE);
        parse_cmif(&c, &h, d && !ctl);
        if (d && ctl) c.is_domain = IPC_DOMAIN_SESSION;
        if (c.header_rc)
            answer(&c, c.header_rc, seq, name, "request");
        else if (ctl)
            answer(&c, s->obj ? control(&c, s) : RC_UNKNOWNCOMMAND, seq, name, "control");
        else if (d)
            serve_domain(&c, d, seq, name);
        else if (s->obj)
            serve(&c, s->obj, seq);
        else
            answer(&c, RC_UNKNOWNCOMMAND, seq, name, "command");
    }
    kobj_unref(&s->o);
}

/* ============================================================ what dispatchers answer */
/* the count stops at 4, past the three warnings; once the table is full a new pair is never warned of, which is said
   once */
int ipc_warn_once(const char* name, u32 cmd) {
    static struct {
        char name[128];
        u32 cmd;
        int n;
    } seen[512];
    static int nseen;
    static PMutex lk = PMUTEX_INIT;
    int i, r = 0;
    pm_lock(&lk);
    for (i = 0; i < nseen && (seen[i].cmd != cmd || strncmp(seen[i].name, name, 127)); i++) {
    }
    if (i == nseen && nseen < 512) {
        strncpy(seen[i].name, name, 127);
        seen[i].cmd = cmd;
        nseen++;
    }
    if (i < nseen) {
        r = seen[i].n < 3;
        if (seen[i].n < 4) seen[i].n++;
    }
    pm_unlock(&lk);
    if (i == 512) WARN_LIMIT(1, "ipc: 512 service commands warned of, no more are\n");
    return r;
}
int stub_ok(SvcObj* o, IpcCtx* c) {
    if (!g_opt_auto_stub_ipc) return ipc_unknown(o, c);
    if (ipc_warn_once(o->name, c->cmd)) WARN("  [undeclared] %s cmd %u -> ok\n", o->name, c->cmd);
    return 0;
}
int quiet_ok(SvcObj* o, IpcCtx* c) {
    (void)o;
    (void)c;
    return 0;
}
int ipc_unknown(SvcObj* o, IpcCtx* c) {
    if (ipc_warn_once(o->name, c->cmd)) WARN("  [unknown] %s cmd %u -> UnknownCommandId\n", o->name, c->cmd);
    return (int)RC_UNKNOWNCOMMAND;
}
int ipc_bad_request(SvcObj* o, IpcCtx* c) {
    WARN("  [bad request] %s cmd %u: pid %d, A %d, B %d, X %d, W %d, copy handles %d, raw 0x%x bytes%s\n", o->name,
         c->cmd, c->has_pid, c->na, c->nb, c->nx, c->nw, c->ncopy, c->raw_size, c->tipc ? " (TIPC)" : "");
    return (int)RC_BADCMIFREQUEST;
}
int ipc_bad_objects(SvcObj* o, IpcCtx* c) {
    WARN("  [bad request] %s cmd %u: %d objects%s\n", o->name, c->cmd, c->nin_objs,
         c->is_domain ? "" : " on a session that isn't a domain");
    return (int)(c->is_domain ? RC_BADNUMINOBJECTS : RC_NOTSUPPORTED);
}
SvcObj* ipc_in_obj(IpcCtx* c, int i) {
    if (c->is_domain != IPC_DOMAIN_MESSAGE || i < 0 || i >= c->nin_objs || !c->sess) return NULL;
    return domain_get(__atomic_load_n(&c->sess->domain, __ATOMIC_ACQUIRE), c->in_objs[i]);
}

/* ============================================================ the service registry */
/* the names sm hands out, each with the constructor of a new object for every session */
typedef struct {
    const char* name;
    SvcCtor ctor;
} SvcReg;
static SvcReg* g_reg;
static int g_nreg, g_capreg;
static PMutex g_reg_lk = PMUTEX_INIT;
static int reg_find_nl(const char* name) {
    int i;
    for (i = 0; i < g_nreg; i++)
        if (!strcmp(g_reg[i].name, name)) return i;
    return -1;
}
void svc_register(const char* name, SvcCtor ctor) {
    int i;
    pm_lock(&g_reg_lk);
    i = reg_find_nl(name);
    if (i < 0) {
        if (g_nreg == g_capreg) {
            int cap = g_capreg ? g_capreg * 2 : 64;
            SvcReg* n = realloc(g_reg, sizeof *n * (size_t)cap);
            if (!n) fatal("Out of memory for the service registry");
            g_reg = n;
            g_capreg = cap;
        }
        i = g_nreg++;
        g_reg[i].name = name;
    }
    g_reg[i].ctor = ctor;
    pm_unlock(&g_reg_lk);
}
SvcObj* svc_open(const char* name) {
    SvcCtor ctor = NULL;
    int i;
    pm_lock(&g_reg_lk);
    i = reg_find_nl(name);
    if (i >= 0) ctor = g_reg[i].ctor;
    pm_unlock(&g_reg_lk);
    return ctor ? ctor() : NULL;
}
int svc_registered(const char* name) {
    int r;
    pm_lock(&g_reg_lk);
    r = reg_find_nl(name) >= 0;
    pm_unlock(&g_reg_lk);
    return r;
}
