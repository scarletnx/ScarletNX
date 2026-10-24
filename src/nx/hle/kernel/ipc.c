/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/ipc_helpers.h, the TIPC reply's header type):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: 2016 Citra Emulator Project */
/* IPC: service objects and sessions, HIPC/CMIF/TIPC requests parsed and answered as the kernel and the system's sf
   framework do (Switchbrew's IPC_Marshalling page for the wire format), domains, and the service registry */
#include "nx/hle/kernel/kernel_int.h"
#include "nx/hle/hle.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/tracy.h"
#include <assert.h>

#define SFCI                 0x49434653u /* the CmifInHeader's magic */
#define SFCO                 0x4F434653u /* the CmifOutHeader's */
/* sf's results (module 10, cmif) and hipc's (module 11) */
#define RC_NOTSUPPORTED      RC(10, 1)
#define RC_BADHEADERSIZE     RC(10, 202)
#define RC_BADINHEADER       RC(10, 211)
#define RC_UNKNOWNCOMMAND    RC(10, 221)
#define RC_BADNUMINOBJECTS   RC(10, 235)
#define RC_TARGETNOTFOUND    RC(10, 261)
#define RC_BADCMIFREQUEST    RC(11, 420)
#define RC_TARGETNOTDOMAIN   RC(11, 491)
#define RC_DOMAINOBJNOTFOUND RC(11, 492)
/* the server's message buffer, which the raw data and every reply fit in */
#define SERVER_MSG           0x100u
/* what QueryPointerBufferSize answers unless the service sets its own (SvcObj.ptrbuf): the size of the server's
   pointer buffer for X and C descriptors */
#define POINTER_BUFFER       0x8000u
/* IpcCtx.is_domain: a request on a domain session outside a domain message (a control request, answered plainly), and
   one in a domain message (SendMessage), whose reply is a domain one */
enum { IPC_DOMAIN_SESSION = 1, IPC_DOMAIN_MESSAGE = 2 };

/* the message's words are little-endian, as every field of HIPC and CMIF is: the descriptors' packed bitfields are
   taken apart from the converted word */
static u32 rd32(const u8* p) { return snx_ld_le32(p); }
static void wr32(u8* p, u32 v) { snx_st_le32(p, v); }

/* ipc.c's state is g_hle.ipc (ipc.h): the service objects, sessions and domains, each a pool (hle.h), the warned
   pairs, the registry and the crash report's ring */

/* ============================================================ the crash report's ring */
u32 ipc_ring_count(void) { return __atomic_load_n(&g_hle.ipc.ring_n, __ATOMIC_RELAXED); }
void ipc_ring_text(u32 seq, char* out, size_t n) {
    const IpcRec* r = &g_hle.ipc.ring[seq & 31];
    const char* name = r->name ? r->name : "?";
    switch (r->kind) {
        case IPC_REC_RUNNING:
            snprintf(out, n, "%s cmd %u (in %x %x) -> running", name, r->cmd, r->in[0], r->in[1]);
            break;
        case IPC_REC_SERVED:
            snprintf(out, n, "%s cmd %u (in %x %x) -> %x", name, r->cmd, r->in[0], r->in[1], r->rc);
            break;
        case IPC_REC_ANSWERED:
            snprintf(out, n, "%s type %u cmd %u -> %x (not served: %s)", name, r->type, r->cmd, r->rc, r->what);
            break;
        case IPC_REC_CLOSE_OBJECT:
            snprintf(out, n, "%s type %u close of object %u -> 0 (not served)", name, r->type, r->cmd);
            break;
        case IPC_REC_NOT_SESSION: snprintf(out, n, "handle %x -> E401 (not a session)", r->cmd); break;
        case IPC_REC_SESSION_CLOSED: snprintf(out, n, "%s -> F601 (session closed)", name); break;
        case IPC_REC_KERNEL_REFUSED:
            snprintf(out, n, "%s type %u -> %x (not served: the kernel refused it)", name, r->type, r->rc);
            break;
        default: snprintf(out, n, "%s type %u -> F601 (not served, the session is closed)", name, r->type); break;
    }
}
/* each request takes the next entry, served or not; the number it returns names it */
static u32 ring_claim(void) { return __atomic_fetch_add(&g_hle.ipc.ring_n, 1, __ATOMIC_RELAXED); }
/* rewrites entry seq, unless 32 later requests have taken its slot meanwhile (a service that ran long). Each request
   stores its fields, and only a crash report formats them: a text entry cost two vsnprintf calls a request */
static void ring_set(u32 seq, u32 kind, const char* name, u32 type, u32 cmd, const u32* in, u32 rc, const char* what) {
    IpcRec* r = &g_hle.ipc.ring[seq & 31];
    if (__atomic_load_n(&g_hle.ipc.ring_n, __ATOMIC_RELAXED) - seq > 32) return;
    r->name = name;
    r->what = what;
    r->kind = kind;
    r->type = type;
    r->cmd = cmd;
    r->in[0] = in ? in[0] : 0;
    r->in[1] = in ? in[1] : 0;
    r->rc = rc;
}

/* ============================================================ service objects */
/* the object from its pool, and its data from the service's pool (when it has some): both under the lock, the data's
   first u32 the object's index plus 1 */
SvcObj* svcobj_new_data(const char* name, int (*dispatch)(SvcObj*, IpcCtx*), void* base, size_t size, u32 count,
                        u32* next, const char* what) {
    SvcObj* o;
    assert(!base || size >= sizeof(u32));
    pm_lock(&g_host.ipc.lk);
    o = (SvcObj*)POOL_TAKE(g_hle.ipc.svcobjs, g_hle.ipc.svcobj_next, "service objects");
    o->name = name;
    o->dispatch = dispatch;
    o->refs = 1;
    o->ptrbuf = POINTER_BUFFER;
    if (base) {
        u32 id = POOL_INDEX(g_hle.ipc.svcobjs, o) + 1;
        o->data = pool_take(base, size, count, next, what);
        o->datasize = (u32)size;
        memcpy(o->data, &id, sizeof id);
    }
    pm_unlock(&g_host.ipc.lk);
    return o;
}
SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*)) {
    return svcobj_new_data(name, dispatch, NULL, 0, 0, NULL, NULL);
}
void svcobj_ref(SvcObj* o) {
    if (o) __atomic_add_fetch(&o->refs, 1, __ATOMIC_RELAXED);
}
void svcobj_unref(SvcObj* o) {
    if (!o || __atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL)) return;
    if (o->destroy) o->destroy(o); /* outside the lock: it may let go of other objects */
    pm_lock(&g_host.ipc.lk);
    if (o->data) pool_put(o->data, o->datasize);
    POOL_PUT(o);
    pm_unlock(&g_host.ipc.lk);
}

/* ============================================================ domains */
static Domain* domain_new(SvcObj* root) {
    Domain* d;
    pm_lock(&g_host.ipc.lk);
    d = (Domain*)POOL_TAKE(g_hle.ipc.domains, g_hle.ipc.domain_next, "domains");
    d->obj[0] = root;
    d->n = 1;
    d->refs = 1;
    pm_unlock(&g_host.ipc.lk);
    return d;
}
static void domain_unref(Domain* d) {
    int i;
    if (!d || __atomic_sub_fetch(&d->refs, 1, __ATOMIC_ACQ_REL)) return;
    for (i = 0; i < d->n; i++) svcobj_unref(d->obj[i]); /* nothing else reaches the domain now */
    pm_lock(&g_host.ipc.lk);
    POOL_PUT(d);
    pm_unlock(&g_host.ipc.lk);
}
/* object id, with a reference to drop, or NULL */
static SvcObj* domain_get(Domain* d, u32 id) {
    SvcObj* o = NULL;
    if (!d) return NULL;
    pm_lock(&g_host.ipc.lk);
    if (id && id <= (u32)d->n) {
        o = d->obj[id - 1];
        svcobj_ref(o);
    }
    pm_unlock(&g_host.ipc.lk);
    return o;
}
/* o joins the domain with its reference: its id */
static u32 domain_add(Domain* d, SvcObj* o) {
    int i;
    pm_lock(&g_host.ipc.lk);
    for (i = 0; i < d->n && d->obj[i]; i++) {}
    assert(i < DOMAIN_OBJECTS);
    if (i == DOMAIN_OBJECTS) fatal("More than %d objects in a domain", DOMAIN_OBJECTS); /* NDEBUG builds */
    if (i == d->n) d->n++;
    d->obj[i] = o;
    pm_unlock(&g_host.ipc.lk);
    return (u32)i + 1;
}
/* object id leaves the domain: its reference, for the caller to drop outside the lock, or NULL */
static SvcObj* domain_take(Domain* d, u32 id) {
    SvcObj* o = NULL;
    pm_lock(&g_host.ipc.lk);
    if (id && id <= (u32)d->n) {
        o = d->obj[id - 1];
        d->obj[id - 1] = NULL;
    }
    pm_unlock(&g_host.ipc.lk);
    return o;
}

/* ============================================================ sessions */
void ksession_destroy(KObj* o) {
    KSession* s = (KSession*)o;
    svcobj_unref(s->obj);
    domain_unref(s->domain);
    pm_lock(&g_host.ipc.lk);
    POOL_PUT(s);
    pm_unlock(&g_host.ipc.lk);
}
/* a session to obj and domain, whose references it takes: its handle (0 when the table is full) */
static u32 session_make(SvcObj* obj, Domain* d) {
    KSession* s;
    u32 h;
    pm_lock(&g_host.ipc.lk);
    s = (KSession*)POOL_TAKE(g_hle.ipc.sessions, g_hle.ipc.session_next, "sessions");
    s->o.type = KO_SESSION;
    kobj_set_name(&s->o, obj ? obj->name : "session");
    s->obj = obj;
    s->domain = d;
    pm_unlock(&g_host.ipc.lk);
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
    R_SUCCEED();
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
    R_UNLESS(!(addr & 0xFFF), R_BADADDR);
    R_UNLESS(size && !(size & 0xFFF), R_BADSIZE);
    R_UNLESS(addr < AS_SIZE && size <= AS_SIZE - addr, R_BADMEMSTATE);
    while (a < addr + size) {
        mem_query(a, &b);
        if (a == addr) {
            state = b.state;
            perm = b.perm;
        }
        if (b.state != state || b.perm != perm || b.attr || !(b.state & MS_FLAG_IPC_USER_BUFFER) || b.perm != PERM_RW ||
            !b.size)
            R_THROW(R_BADMEMSTATE);
        a = b.addr + b.size;
    }
    R_SUCCEED();
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
    SNX_ZONE_TV(z_ipc, "ipc", c->cmd, "%s cmd %u", o->name, c->cmd);
    ipc_in(c, 0, &in[0], 4);
    ipc_in(c, 4, &in[1], 4);
    snx_le32_array(in, 2);
    ring_set(seq, IPC_REC_RUNNING, o->name, c->type, c->cmd, in, 0, NULL);
    LOGV("IPC %s cmd %u\n", o->name, c->cmd);
    c->rc = (u32)o->dispatch(o, c);
    ring_set(seq, IPC_REC_SERVED, o->name, c->type, c->cmd, in, c->rc, NULL);
    respond(c);
}
/* a request sf answers without the service: a refusal, or a control request */
static void answer(IpcCtx* c, u32 rc, u32 seq, const char* name, const char* what) {
    c->rc = rc;
    ring_set(seq, IPC_REC_ANSWERED, name, c->type, c->cmd, NULL, rc, what);
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
            R_SUCCEED();
        case 1: {
            SvcObj* o;
            R_UNLESS(d, RC_TARGETNOTDOMAIN);
            ipc_in(c, 0, &id, 4);
            o = domain_get(d, snx_le32(id));
            R_UNLESS(o, RC_DOMAINOBJNOTFOUND);
            out_move_h(c, session_new(o));
            R_SUCCEED();
        }
        case 2:
        case 4:
            svcobj_ref(s->obj);
            if (d) __atomic_add_fetch(&d->refs, 1, __ATOMIC_RELAXED);
            out_move_h(c, session_make(s->obj, d));
            R_SUCCEED();
        case 3: out32(c, s->obj ? s->obj->ptrbuf : POINTER_BUFFER); R_SUCCEED();
        default:
            if (ipc_warn_once("control", c->cmd))
                WARN("ipc: %s: control request %u, UnknownCommandId\n", s->obj ? s->obj->name : "session", c->cmd);
            R_THROW(RC_UNKNOWNCOMMAND);
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
        ring_set(seq, IPC_REC_CLOSE_OBJECT, name, c->type, c->domain_obj, NULL, 0, NULL);
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
u32 ipc_handle(u32 handle, u64 msg_addr, u64 msg_size) {
    IpcCtx c;
    Hipc h;
    u32 seq = ring_claim();
    KSession* s = (KSession*)h_get_ref(handle, KO_SESSION);
    KThread* t = kthread_cur();
    Domain* d;
    const char* name;
    u32 rc = 0;
    if (t && s) t->req_session = kobj_id(&s->o);
    if (!s) {
        ring_set(seq, IPC_REC_NOT_SESSION, NULL, 0, handle, NULL, R_BADHANDLE, NULL);
        WARN_LIMIT(8, "ipc: a request on handle %x, which isn't a session: InvalidHandle\n", handle);
        return R_BADHANDLE;
    }
    name = s->obj ? s->obj->name : "session";
    if (__atomic_load_n(&s->closed, __ATOMIC_ACQUIRE)) {
        ring_set(seq, IPC_REC_SESSION_CLOSED, name, 0, 0, NULL, R_SESSIONCLOSED, NULL);
        kobj_unref(&s->o);
        return R_SESSIONCLOSED;
    }
    memset(&c, 0, sizeof c);
    c.msg = GP(msg_addr);
    c.msg_size = msg_size;
    c.sess = s;
    if (parse_hipc(&c, &h)) {
        ring_set(seq, IPC_REC_KERNEL_REFUSED, name, c.type, 0, NULL, c.kernel_rc, NULL);
        WARN_LIMIT(8, "ipc: %s: the kernel refuses a request (%x): %s\n", name, c.kernel_rc,
                   kernel_refusal(c.kernel_rc));
        rc = c.kernel_rc;
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
        ring_set(seq, IPC_REC_TYPE_CLOSED, name, c.type, 0, NULL, R_SESSIONCLOSED, NULL);
        if (c.type != 2 && c.type != 15)
            WARN_LIMIT(4, "ipc: %s: message type %u, which sf doesn't serve: the session is closed\n", name, c.type);
        rc = R_SESSIONCLOSED;
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
    if (t) t->req_session = 0;
    kobj_unref(&s->o);
    return rc;
}

/* ============================================================ what dispatchers answer */
/* A game that polls a command nothing implements would fill the log with it: each pair is reported the first time
   only. Past WARN_PAIRS pairs the table stops growing and new pairs go unreported, which the log is told once */
int ipc_warn_once(const char* name, u32 cmd) {
    char key[128];
    u32 h = 2166136261u, i;
    size_t k;
    int report = 0, full = 0;
    snprintf(key, sizeof key, "%s", name && *name ? name : "?");
    for (k = 0; key[k]; k++) h = (h ^ (u8)key[k]) * 16777619u; /* FNV-1a over the name, then the command */
    h = (h ^ cmd) * 16777619u;
    pm_lock(&g_host.ipc.lk);
    for (i = h % WARN_SLOTS;; i = (i + 1) % WARN_SLOTS) {
        if (!g_hle.ipc.seen[i].name[0]) break;
        if (g_hle.ipc.seen[i].cmd == cmd && !strcmp(g_hle.ipc.seen[i].name, key)) break;
    }
    if (!g_hle.ipc.seen[i].name[0]) {
        if (g_hle.ipc.nseen < WARN_PAIRS) {
            memcpy(g_hle.ipc.seen[i].name, key, sizeof key);
            g_hle.ipc.seen[i].cmd = cmd;
            g_hle.ipc.nseen++;
            report = 1;
        } else if (!g_hle.ipc.warn_full) {
            g_hle.ipc.warn_full = 1;
            full = 1;
        }
    }
    pm_unlock(&g_host.ipc.lk);
    if (full) WARN("ipc: %d unimplemented commands reported, further ones are not\n", WARN_PAIRS);
    return report;
}
/* "undeclared", not "stub": a success carrying none of the command's outputs. The SDK still reads them from the reply,
   past what respond writes, so they are zeroed here: a count, an object or a handle reads as none, not as the request's
   bytes left in the client's buffer (a game took a device count of "SFCI" from IAudioDevice cmd 14 and walked off its
   stack) */
int stub_ok(SvcObj* o, IpcCtx* c) {
    if (!g_settings.answer_unknown_commands) return ipc_unknown(o, c);
    if (ipc_warn_once(o->name, c->cmd)) WARN("  [undeclared] %s cmd %u -> ok\n", o->name, c->cmd);
    memset(c->msg, 0, c->msg_size < SERVER_MSG ? (size_t)c->msg_size : SERVER_MSG);
    R_SUCCEED();
}
int quiet_ok(SvcObj* o, IpcCtx* c) {
    (void)o;
    (void)c;
    R_SUCCEED();
}
int ipc_unknown(SvcObj* o, IpcCtx* c) {
    if (ipc_warn_once(o->name, c->cmd)) WARN("  [unknown] %s cmd %u -> UnknownCommandId\n", o->name, c->cmd);
    R_THROW((int)RC_UNKNOWNCOMMAND);
}
int ipc_bad_request(SvcObj* o, IpcCtx* c) {
    WARN("  [bad request] %s cmd %u: pid %d, A %d, B %d, X %d, W %d, copy handles %d, raw 0x%x bytes%s\n", o->name,
         c->cmd, c->has_pid, c->na, c->nb, c->nx, c->nw, c->ncopy, c->raw_size, c->tipc ? " (TIPC)" : "");
    R_THROW((int)RC_BADCMIFREQUEST);
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
/* g_hle.ipc.reg, under g_host.ipc.lk: a slot array whose live entries have a name (an all-zero one is empty), never
   freed. A constructor runs outside the lock: it makes a service object, which takes it */
static int reg_find_nl(const char* name) {
    int i;
    for (i = 0; i < SVCREG_MAX; i++)
        if (g_hle.ipc.reg[i].name && !strcmp(g_hle.ipc.reg[i].name, name)) return i;
    return -1;
}
void svc_register(const char* name, SvcCtor ctor) {
    SvcReg* r;
    int i;
    pm_lock(&g_host.ipc.lk);
    i = reg_find_nl(name);
    if (i >= 0)
        r = &g_hle.ipc.reg[i];
    else {
        r = (SvcReg*)POOL_TAKE(g_hle.ipc.reg, g_hle.ipc.reg_next, "service names");
        r->name = name;
    }
    r->ctor = ctor;
    pm_unlock(&g_host.ipc.lk);
}
SvcObj* svc_open(const char* name) {
    SvcCtor ctor = NULL;
    int i;
    pm_lock(&g_host.ipc.lk);
    i = reg_find_nl(name);
    if (i >= 0) ctor = g_hle.ipc.reg[i].ctor;
    pm_unlock(&g_host.ipc.lk);
    return ctor ? ctor() : NULL;
}
int svc_registered(const char* name) {
    int r;
    pm_lock(&g_host.ipc.lk);
    r = reg_find_nl(name) >= 0;
    pm_unlock(&g_host.ipc.lk);
    return r;
}
