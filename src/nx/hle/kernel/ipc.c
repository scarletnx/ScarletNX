/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* HIPC/CMIF (+domains). Requests are served on the calling thread, no server threads and
   no context switches. */
#include "nx/hle/kernel/kernel_int.h"
#include "nx/runtime/options.h"

/* ============================================================ service objects */
SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*), size_t ds) {
    SvcObj* o = calloc(1, sizeof *o);
    o->name = name;
    o->dispatch = dispatch;
    o->refs = 1;
    o->ptrbuf = 0x8000;
    if (ds) o->data = calloc(1, ds);
    return o;
}
void svcobj_ref(SvcObj* o) { __atomic_add_fetch(&o->refs, 1, __ATOMIC_SEQ_CST); }
void svcobj_unref(SvcObj* o) {
    if (!o) return;
    if (__atomic_sub_fetch(&o->refs, 1, __ATOMIC_SEQ_CST) == 0) {
        if (o->destroy) o->destroy(o);
        free(o->data);
        free(o);
    }
}
static void domain_unref(Domain* d) {
    if (!d) return;
    pm_lock(&d->lk);
    int r = --d->refs;
    pm_unlock(&d->lk);
    if (r) return;
    for (int i = 0; i < d->n; i++)
        if (d->obj[i]) svcobj_unref(d->obj[i]);
    free(d->obj);
    free(d);
}
static void sess_destroy(KObj* k) {
    KSession* s = (KSession*)k;
    svcobj_unref(s->obj);
    domain_unref(s->domain);
    free(k->waiters);
    free(s);
}
static u32 domain_add(Domain* d, SvcObj* o) {
    pm_lock(&d->lk);
    int id = -1;
    for (int i = 1; i < d->n; i++)
        if (!d->obj[i]) {
            id = i;
            break;
        } /* reuse freed ids */
    if (id < 0) {
        if (d->n == d->cap) {
            d->cap = d->cap ? d->cap * 2 : 16;
            d->obj = realloc(d->obj, sizeof(SvcObj*) * d->cap);
        }
        id = d->n++;
    }
    d->obj[id] = o;
    pm_unlock(&d->lk);
    return (u32)id + 1;
}
static KSession* session_alloc(SvcObj* obj) {
    KSession* s = calloc(1, sizeof *s);
    s->o.type = KO_SESSION;
    s->o.destroy = sess_destroy;
    s->o.name = "session";
    s->obj = obj;
    return s;
}
u32 session_new(SvcObj* obj) { return h_new(&session_alloc(obj)->o); }

/* Mesosphere's SendSyncRequestWithUserBuffer: a page-aligned address (InvalidAddress), a nonzero size of whole pages
   (InvalidSize), then a range the kernel can lock for the request (LockForIpcUserBuffer): inside the address space,
   in memory whose state allows IPC user buffers (FlagCanIpcUserBuffer, bit 21), read-write and with no attribute,
   one state and permission across the range (InvalidCurrentMemory otherwise). The runtime doesn't lock the range
   for the request's length, as the kernel does: a guest that unmaps it meanwhile from another thread is a bug the
   console refuses and the runtime doesn't */
u32 ipc_user_buffer(u64 a, u64 size) {
    if (a & 0xFFF) return R_BADADDR;
    if (!size || (size & 0xFFF)) return R_BADSIZE;
    if (a + size < a || a + size > AS_SIZE) return R_BADMEMSTATE;
    u32 state = 0;
    for (u64 end = a + size; a < end;) {
        MemBlock b;
        mem_query(a, &b);
        if (!state) state = b.state; /* the first block's, which every other one has to share (CheckMemoryState) */
        if (!(b.state & (1u << 21)) || b.state != state || b.perm != PERM_RW || b.attr || b.addr + b.size <= a)
            return R_BADMEMSTATE;
        a = b.addr + b.size;
    }
    return 0;
}

/* only warn the first few times per command, some games call them in a loop. Requests come from every guest thread,
   and a service's name goes with the object, so the table keeps a copy under a lock. A name longer than the copy is
   told apart by its first 127 characters (interface names are shorter). A command first seen once 512 are counted
   is never warned */
int ipc_warn_once(const char* name, u32 cmd) {
    static struct {
        char n[128];
        u32 cmd, cnt;
    } t[512];
    static int nt;
    static PMutex lk = PMUTEX_INIT;
    pm_lock(&lk);
    int r = 1, i = 0;
    for (; i < nt; i++)
        if (t[i].cmd == cmd && !strncmp(t[i].n, name, sizeof t[i].n - 1)) {
            if (t[i].cnt < 4) t[i].cnt++; /* stops at 4: a count that wrapped would warn again */
            r = t[i].cnt <= 3;
            break;
        }
    if (i == nt && nt < 512) {
        snprintf(t[nt].n, sizeof t[nt].n, "%s", name);
        t[nt].cmd = cmd;
        t[nt].cnt = 1;
        nt++;
    } else if (i == nt) { /* the table is full: a command it can't count isn't warned on every call */
        r = 0;
        WARN_LIMIT(1, "IPC: more than 512 service commands unhandled, the rest aren't logged\n");
    }
    pm_unlock(&lk);
    return r;
}
/* "undeclared", not "stub": a declared @stub is silent, and this success carries none of the command's outputs, so a
   command that gives an object or a handle leaves the SDK holding null (fsp-srv 205 in Metroid Prime 4) */
int stub_ok(SvcObj* o, IpcCtx* c) {
    if (!g_opt_auto_stub_ipc) return ipc_unknown(o, c);
    if (ipc_warn_once(o->name, c->cmd)) LOG("  [undeclared] %s cmd %u -> ok\n", o->name, c->cmd);
    return 0;
}
int quiet_ok(SvcObj* o, IpcCtx* c) {
    (void)o;
    (void)c;
    return 0;
}
void ipc_out_overflow(IpcCtx* c) {
    WARN_LIMIT(1, "IPC: the reply to cmd %u is larger than the message buffer, cut short\n", c->cmd);
}

/* ============================================================ registry */
typedef struct {
    char name[16];
    SvcCtor ctor;
} SvcReg;
static SvcReg regs[256];
static int nregs;
void svc_register(const char* n, SvcCtor c) {
    if (nregs == 256) fatal("too many services registered (%s)", n);
    strncpy(regs[nregs].name, n, 15);
    regs[nregs].ctor = c;
    nregs++;
}
SvcObj* svc_open(const char* name) {
    for (int i = 0; i < nregs; i++)
        if (!strcmp(regs[i].name, name)) return regs[i].ctor();
    return NULL;
}
int svc_registered(const char* name) {
    for (int i = 0; i < nregs; i++)
        if (!strcmp(regs[i].name, name)) return 1;
    return 0;
}

void out_copy_h(IpcCtx* c, u32 h) {
    if (c->nout_copy < 8) c->out_copy[c->nout_copy++] = h;
}
void out_move_h(IpcCtx* c, u32 h) {
    if (c->nout_move < 8) c->out_move[c->nout_move++] = h;
}
void out_obj(IpcCtx* c, SvcObj* o) {
    if (c->nout_objs < 8) c->out_objs[c->nout_objs++] = o;
}
int ipc_bad_request(SvcObj* o, IpcCtx* c) {
    if (ipc_warn_once(o->name, c->cmd))
        WARN("  [bad request] %s cmd %u: pid %d, A %d, B %d, X %d, W %d, copy %d, 0x%x bytes of data is not what it "
             "declares -> InvalidCmifRequest\n",
             o->name, c->cmd, c->has_pid, c->na, c->nb, c->nx, c->nw, c->ncopy, c->raw_size);
    return RC(11, 420); /* sf::hipc InvalidCmifRequest */
}
int ipc_bad_objects(SvcObj* o, IpcCtx* c) {
    if (!c->is_domain) { /* sf's plain processor has no objects to give a command (GetInObjects) */
        if (ipc_warn_once(o->name, c->cmd))
            WARN("  [bad request] %s cmd %u takes objects, on a session that isn't a domain -> NotSupported\n", o->name,
                 c->cmd);
        return RC(10, 1); /* sf NotSupported */
    }
    if (ipc_warn_once(o->name, c->cmd))
        WARN("  [bad request] %s cmd %u: %d objects is not what it declares -> InvalidNumInObjects\n", o->name, c->cmd,
             c->nin_objs);
    return RC(10, 235); /* sf::cmif InvalidNumInObjects */
}
int ipc_unknown(SvcObj* o, IpcCtx* c) {
    if (ipc_warn_once(o->name, c->cmd)) WARN("  [unknown] %s cmd %u -> UnknownCommandId\n", o->name, c->cmd);
    return RC(10, 221); /* sf::cmif UnknownCommandId */
}
/* a failed request returns no data, handles or objects: close and drop what the service put in the reply (the generated
   dispatchers put a command's objects and handles there whatever its result) */
static void drop_outputs(IpcCtx* c) {
    for (int i = 0; i < c->nout_copy; i++) h_close(c->out_copy[i]);
    for (int i = 0; i < c->nout_move; i++) h_close(c->out_move[i]);
    for (int i = 0; i < c->nout_objs; i++) svcobj_unref(c->out_objs[i]);
    c->nout_copy = c->nout_move = c->nout_objs = 0;
    c->out_size = 0;
}

char g_ipc_ring[32][160];
u32 g_ipc_ring_n;
/* the ring's entries are claimed in order, slot seq & 31 for claim seq. A request whose service runs long can find
   its slot claimed again after 32 more, and then leaves the newer entry alone. Best effort, for crash reports: a
   claim and a write 32 claims apart can still meet in one slot, and the text stays bounded */
static u32 g_ipc_ring_seq[32];
static u32 ring_claim(void) {
    u32 seq = __atomic_fetch_add(&g_ipc_ring_n, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&g_ipc_ring_seq[seq & 31], seq, __ATOMIC_RELEASE);
    return seq;
}
static int ring_mine(u32 seq) { return __atomic_load_n(&g_ipc_ring_seq[seq & 31], __ATOMIC_ACQUIRE) == seq; }
/* ============================================================ parsing */
/* the kernel maps or copies a buffer with a size before the server sees the request, so one outside the address space
   fails it. A receive list (C) buffer it only uses at the reply, failing that instead, but services here write C
   buffers straight through GP(), so one is refused at the request too: GP() past the address space is host memory.
   Only the bounds are checked: the console also refuses a buffer in unmapped memory, but looking each up (mem_query)
   would take the memory lock on every request */
static void check_buffer(IpcCtx* c, const IpcBuf* b) {
    if (b->size && (b->addr > AS_SIZE || b->size > AS_SIZE - b->addr)) c->kernel_rc = R_BADMEMSTATE;
}
/* a CmifInHeader sf takes: the SFCI magic, and a version it knows (1 at most) */
static int cmif_in_header(const u8* d) {
    u32 version;
    memcpy(&version, d + 4, 4);
    return !memcmp(d, "SFCI", 4) && version <= 1;
}
/* The counts stored are clamped to the arrays, the pointer still moves past every descriptor the header announces.
   Handles the client sends are read but not taken from its table: services that get one look it up themselves */
static void parse(IpcCtx* c) {
    u8* m = c->msg;
    u32 w0, w1;
    memcpy(&w0, m, 4);
    memcpy(&w1, m + 4, 4);
    c->type = w0 & 0xFFFF;
    u32 nx = (w0 >> 16) & 15, na = (w0 >> 20) & 15, nb = (w0 >> 24) & 15, nw = (w0 >> 28) & 15;
    u32 ndata = w1 & 0x3FF, cflags = (w1 >> 10) & 15, recv_off = (w1 >> 20) & 0x7FF; /* the receive list's word */
    int special = (w1 >> 31) & 1;
    u32 nc = cflags == 0 ? 0 : cflags == 1 ? 0 : cflags == 2 ? 1 : cflags - 2;
    u8* p = m + 8;
    {
        /* the kernel copies the message the header describes into the server's 0x100 byte buffer before the server
           reads any of it (Mesosphere's ReceiveMessage). A header that describes more than the client's buffer, its
           receive list included, fails the request (InvalidCombination), and raw data that ends past the server's
           buffer too (MessageTooLarge: a user buffer's request, or one in TLS whose receive list sits inside its
           raw data). Then move handles, which a client can't send a server (InvalidCombination). A receive list
           inside the raw data is refused after the descriptors, below */
        u32 sh = 0, raw_end, all;
        if (special) memcpy(&sh, p, 4);
        raw_end = 8 + (special ? 4 + ((sh & 1) ? 8 : 0) + 4 * (((sh >> 1) & 15) + ((sh >> 5) & 15)) : 0) + 8 * nx +
                  12 * (na + nb + nw) + 4 * ndata;
        all = (recv_off ? recv_off * 4 : raw_end) + 8 * nc;
        if (all > c->msg_size)
            c->kernel_rc = R_BADCOMBO;
        else if (raw_end > 0x100)
            c->kernel_rc = R_MSGTOOLARGE;
        else if ((sh >> 5) & 15)
            c->kernel_rc = R_BADCOMBO;
        if (c->kernel_rc) return;
    }
    if (special) {
        u32 sh;
        memcpy(&sh, p, 4);
        p += 4;
        c->has_pid = sh & 1;
        u32 ncopy = (sh >> 1) & 15; /* and no move handles, refused above */
        if (c->has_pid) {
            /* HIPC replaces the client placeholder with the sender's real process id. */
            c->pid = K_PROCESS_ID;
            p += 8;
        }
        for (u32 i = 0; i < ncopy; i++, p += 4)
            if (i < 8) memcpy(&c->copy_h[i], p, 4);
        c->ncopy = ncopy < 8 ? (int)ncopy : 8;
    }
    for (u32 i = 0; i < nx; i++, p += 8) { /* each is checked, the ones past the arrays too */
        u32 a, b;
        IpcBuf x;
        memcpy(&a, p, 4);
        memcpy(&b, p + 4, 4);
        x.addr = b | ((u64)((a >> 12) & 15) << 32) | ((u64)((a >> 6) & 7) << 36);
        x.size = a >> 16;
        x.flags = 0;
        check_buffer(c, &x);
        if (i < MAX_BUFS) c->x[i] = x;
    }
    c->nx = nx < MAX_BUFS ? (int)nx : MAX_BUFS;
    IpcBuf* lists[3] = {c->a, c->b, c->w};
    u32 cnt[3] = {na, nb, nw};
    for (int k = 0; k < 3; k++)
        for (u32 i = 0; i < cnt[k]; i++, p += 12) {
            u32 s, a, f;
            IpcBuf d;
            memcpy(&s, p, 4);
            memcpy(&a, p + 4, 4);
            memcpy(&f, p + 8, 4);
            d.addr = a | ((u64)((f >> 28) & 15) << 32) | ((u64)((f >> 2) & 7) << 36);
            d.size = s | ((u64)((f >> 24) & 15) << 32);
            d.flags = f & 3;
            check_buffer(c, &d);
            if (i < MAX_BUFS) lists[k][i] = d;
        }
    c->na = na < MAX_BUFS ? (int)na : MAX_BUFS;
    c->nb = nb < MAX_BUFS ? (int)nb : MAX_BUFS;
    c->nw = nw < MAX_BUFS ? (int)nw : MAX_BUFS;
    /* a receive list inside the raw data is InvalidCombination: the console finds it at the reply (SendMessage, the
       client being the destination then), after the descriptors above, which the runtime refuses first too */
    if (c->kernel_rc) return;
    if (recv_off && recv_off * 4 < (u32)(p - m) + 4 * ndata) {
        c->kernel_rc = R_BADCOMBO;
        return;
    }
    /* raw data is 16-aligned from the message start */
    u8* raw = m + (((p - m) + 15) & ~15);
    /* C descriptors follow the data, or start where the header says */
    u8* cp = recv_off ? m + recv_off * 4 : p + ndata * 4;
    c->nc = nc < MAX_BUFS ? (int)nc : MAX_BUFS;
    for (u32 i = 0; i < nc; i++, cp += 8) {
        u32 a, b;
        IpcBuf r;
        memcpy(&a, cp, 4);
        memcpy(&b, cp + 4, 4);
        r.addr = a | ((u64)(b & 0xFFFF) << 32);
        r.size = b >> 16;
        r.flags = 0;
        check_buffer(c, &r);
        if (i < MAX_BUFS) c->c[i] = r;
    }
    /* the data words end the request (inside the message: the size check above) */
    u8* end = p + ndata * 4;
    /* TIPC (newer SDKs): type is 16+cmd, payload right after the header, no SFCI */
    if (c->type >= 16) {
        c->tipc = 1;
        c->cmd = c->type - 16;
        c->raw = p;
        c->raw_size = (u32)(end - p);
        return;
    }
    /* CMIF: sf's data region starts at the data words aligned to 16 and is 16 bytes shorter than they are, whatever
       the alignment took (sf's DispatchRequest), so a request is measured as sf measures it. Fewer than 4 data
       words answer InvalidHeaderSize below, where Atmosphere answers InvalidRequestSize: no SDK sends one */
    end = ndata * 4 >= 16 ? raw + ndata * 4 - 16 : raw;
    u8* d = raw;
    u32 dlen_max = 0x100; /* a domain message says how long its CMIF part is */
    c->is_domain = 0;
    if (__atomic_load_n(&c->sess->domain, __ATOMIC_ACQUIRE) && (c->type == 4 || c->type == 6)) {
        if (end < d + 16) {   /* sf's first check, InvalidHeaderSize: nothing of the header is read */
            c->is_domain = 2; /* the client reads a domain reply */
            c->header_rc = RC(10, 202);
            return;
        }
        u8 dcmd = d[0];
        u8 nobj = d[1];
        u16 dlen;
        memcpy(&dlen, d + 2, 2);
        memcpy(&c->domain_obj, d + 4, 4);
        c->is_domain = 1 + dcmd; /* 2 = request, 3 = close, anything else is refused */
        /* sf's checks of a request's domain header (sf_cmif_domain_service_object.cpp): its data and object ids
           within the message's data, 8 objects at most */
        u32 avail = end > d + 16 ? (u32)(end - (d + 16)) : 0;
        if ((u32)dlen + nobj * 4u > avail)
            c->domain_rc = RC(10, 202); /* InvalidHeaderSize */
        else if (nobj > 8)
            c->domain_rc = RC(10, 235); /* InvalidNumInObjects */
        else {
            c->nin_objs = nobj;
            for (int i = 0; i < c->nin_objs; i++) memcpy(&c->in_objs[i], d + 16 + dlen + i * 4, 4);
        }
        dlen_max = dlen > 16 ? dlen - 16u : 0; /* the CmifInHeader, then the arguments: the object ids come after */
        d += 16;
        /* the object's CmifInHeader is checked once the target is found: it has to be there, with its magic */
        if (!c->domain_rc && dlen < 16) c->domain_rc = RC(10, 202);          /* InvalidHeaderSize */
        if (!c->domain_rc && !cmif_in_header(d)) c->domain_rc = RC(10, 211); /* InvalidInHeader */
    } else if (end < d + 16 || !cmif_in_header(d)) {                         /* a plain request's CmifInHeader */
        c->header_rc = end < d + 16 ? RC(10, 202) /* InvalidHeaderSize */ : RC(10, 211) /* InvalidInHeader */;
        return;
    }
    memcpy(&c->cmd, d + 8, 4);
    memcpy(&c->token, d + 12, 4);
    c->raw = d + 16;
    c->raw_size = end > c->raw ? (u32)(end - c->raw) : 0;
    if (c->raw_size > dlen_max) c->raw_size = dlen_max;
}

/* ============================================================ reply */
static void respond(IpcCtx* c) {
    u8* m = c->msg;
    memset(m, 0, 0x100);
    int dom = c->is_domain != 0 && !c->tipc;
    u32 nhandles = c->nout_copy + c->nout_move;
    if (!dom) nhandles += c->nout_objs; /* out objects become new session handles */
    u32 w0 = 0;
    u8* p = m + 8;
    u32 special = (c->nout_copy || c->nout_move || (!dom && c->nout_objs)) ? 1 : 0;
    u32 hdr_words = 2 + (special ? 1 + nhandles : 0);
    u32 pad_bytes = ((hdr_words * 4 + 15) & ~15) - hdr_words * 4;
    /* SFCO(16) + payload; domains add a 16 byte header and object ids */
    u32 payload = (c->out_size + 3) & ~3u;
    /* what fits the message buffer the reply goes back in: a reply built by hand that doesn't is cut, and logged */
    u32 room = (u32)(c->msg_size < 0x100 ? c->msg_size : 0x100) - hdr_words * 4 -
               (c->tipc ? 4 : pad_bytes + 16 + (dom ? 16 + c->nout_objs * 4 : 0)); /* TIPC's data isn't aligned */
    if (payload > room) {
        ipc_out_overflow(c);
        payload = room & ~3u;
    }
    u32 data_bytes = 16 + payload + (dom ? 16 + c->nout_objs * 4 : 0);
    /* the data words count 16 bytes for the alignment's padding, whatever it took, as libnx counts a request's: the
       SDK's client aborts on a reply whose data words come to more than its 0x100 byte buffer, and a domain reply
       with the 0xD0 bytes of output swipc allows comes to 0x100 exactly */
    u32 ndata = (16 + data_bytes + 3) / 4;
    memcpy(m, &w0, 4);
    u32 w1 = ndata | (special ? 0x80000000u : 0);
    memcpy(m + 4, &w1, 4);
    if (special) {
        u32 nmove = c->nout_move + (dom ? 0 : c->nout_objs);
        u32 sh = (c->nout_copy << 1) | (nmove << 5);
        memcpy(p, &sh, 4);
        p += 4;
        for (int i = 0; i < c->nout_copy; i++) {
            memcpy(p, &c->out_copy[i], 4);
            p += 4;
        }
        /* the out objects' sessions come first among the move handles, as sf writes them: a client that gets both
           reads the object's handle at index 0. A missing object is handle 0 */
        if (!dom)
            for (int i = 0; i < c->nout_objs; i++) {
                u32 h = c->out_objs[i] ? session_new(c->out_objs[i]) : 0;
                memcpy(p, &h, 4);
                p += 4;
            }
        for (int i = 0; i < c->nout_move; i++) {
            memcpy(p, &c->out_move[i], 4);
            p += 4;
        }
    }
    if (c->tipc) { /* TIPC reply: result + payload after the header */
        w1 = (1 + payload / 4) | (special ? 0x80000000u : 0);
        memcpy(m + 4, &w1, 4);
        memcpy(p, &c->rc, 4);
        memcpy(p + 4, c->out, payload);
        return;
    }
    u8* d = m + (((p - m) + 15) & ~15);
    if (dom) {
        u32 n = c->nout_objs;
        memcpy(d, &n, 4);
        d += 16;
    }
    u32 magic = 0x4F434653;
    memcpy(d, &magic, 4);
    u32 ver = 0;
    memcpy(d + 4, &ver, 4);
    memcpy(d + 8, &c->rc, 4);
    memcpy(d + 12, &c->token, 4);
    memcpy(d + 16, c->out, payload);
    if (dom) {
        u8* o = d + 16 + payload;
        for (int i = 0; i < c->nout_objs; i++) {
            u32 id =
                c->out_objs[i] ? domain_add(__atomic_load_n(&c->sess->domain, __ATOMIC_ACQUIRE), c->out_objs[i]) : 0;
            memcpy(o + i * 4, &id, 4);
        }
    }
}

static SvcObj* domain_get(KSession* s, u32 id) {
    Domain* d = __atomic_load_n(&s->domain, __ATOMIC_ACQUIRE);
    if (!d) return NULL;
    pm_lock(&d->lk);
    SvcObj* o = (id && id <= (u32)d->n) ? d->obj[id - 1] : NULL;
    if (o) svcobj_ref(o);
    pm_unlock(&d->lk);
    return o;
}
SvcObj* ipc_in_obj(IpcCtx* c, int i) {
    if (i >= c->nin_objs) return NULL;
    return domain_get(c->sess, c->in_objs[i]);
}

/* the control requests (type 5 and 7) */
static void control(IpcCtx* c, KSession* s) {
    switch (c->cmd) {
        case 0: { /* ConvertCurrentObjectToDomain */
            if (!__atomic_load_n(&s->domain, __ATOMIC_ACQUIRE)) {
                /* filled in before it's published: another thread's request on the session may read it at once.
                   Two converts at once keep the first domain */
                Domain *d = calloc(1, sizeof *d), *none = NULL;
                pm_init(&d->lk);
                d->refs = 1;
                svcobj_ref(s->obj);
                domain_add(d, s->obj);
                if (!__atomic_compare_exchange_n(&s->domain, &none, d, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                    domain_unref(d);
            }
            out32(c, 1);
            break;
        }
        case 1: { /* CopyFromCurrentDomain {u32 object id}: a session of the domain's object, moved to the client */
            u32 id;
            ipc_in(c, 0, &id, 4);
            if (!__atomic_load_n(&s->domain, __ATOMIC_ACQUIRE)) {
                c->rc = RC(11, 491); /* sf::hipc TargetNotDomain */
                break;
            }
            SvcObj* o = domain_get(s, id); /* its reference goes to the new session */
            if (!o) {
                c->rc = RC(11, 492); /* sf::hipc DomainObjectNotFound */
                break;
            }
            out_move_h(c, session_new(o));
            break;
        }
        case 2:
        case 4: { /* CloneCurrentObject(Ex) */
            svcobj_ref(s->obj);
            KSession* ns = session_alloc(s->obj);
            Domain* d = __atomic_load_n(&s->domain, __ATOMIC_ACQUIRE);
            if (d) {
                pm_lock(&d->lk);
                d->refs++;
                pm_unlock(&d->lk);
                ns->domain = d;
            }
            out_move_h(c, h_new(&ns->o));
            break;
        }
        case 3: out32(c, s->obj->ptrbuf); break; /* QueryPointerBufferSize */
        default:
            if (ipc_warn_once("IPC control", c->cmd)) WARN("IPC: control command %u not handled\n", c->cmd);
            c->rc = RC(10, 221); /* sf::cmif UnknownCommandId */
            break;
    }
}

void ipc_handle(KThread* t, u32 handle, u64 msg_addr, u64 msg_size) {
    CPU* cpu = &t->cpu;
    /* the ref keeps the session alive while the request runs, if another thread closes the handle meanwhile */
    KSession* s = (KSession*)h_get_ref(handle, KO_SESSION);
    if (!s) {
        WARN_LIMIT(8, "SendSyncRequest to invalid handle %x (pc=%llx)\n", handle, (unsigned long long)cpu->pc);
        cpu->x[0] = R_BADHANDLE;
        u32 seq = ring_claim();
        snprintf(g_ipc_ring[seq & 31], sizeof g_ipc_ring[0], "handle %x -> %x (not a session)", handle, R_BADHANDLE);
        return;
    }
    if (__atomic_load_n(&s->closed, __ATOMIC_ACQUIRE)) { /* the server closed it: every request fails */
        cpu->x[0] = R_SESSIONCLOSED;
        u32 seq = ring_claim();
        snprintf(g_ipc_ring[seq & 31], sizeof g_ipc_ring[0], "%s -> %x (session closed)", s->obj->name,
                 R_SESSIONCLOSED);
        kobj_unref(&s->o);
        return;
    }
    IpcCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    IpcCtx* c = &ctx;
    const char* name = s->obj->name; /* for the ring: the object served, once it's known */
    int served = 0;                  /* a service ran it, and wrote its own ring entry */
    SvcObj* held = NULL;             /* domain_get's reference to the target, dropped at the end */
    c->msg = GP(msg_addr);
    c->msg_size = msg_size;
    c->sess = s;
    parse(c);
    cpu->x[0] = 0;
    if (c->kernel_rc) { /* the client's SendSyncRequest fails, and the server never sees the request, a close too */
        WARN_LIMIT(8, "IPC: request to %s refused by the kernel (%x): %s\n", s->obj->name, c->kernel_rc,
                   c->kernel_rc == R_BADCOMBO
                       ? "its header describes more than its buffer, a receive list inside its data, or a move handle"
                   : c->kernel_rc == R_MSGTOOLARGE ? "the message is larger than the server's buffer"
                                                   : "a buffer is outside the address space");
        cpu->x[0] = c->kernel_rc;
        goto out;
    }
    /* a Close (2, or 15 for TIPC), or a type sf doesn't serve (0, 1, 3, 8 to 14): sf closes the session without a
       reply (Atmosphere's ServerSessionManager), and the client's SendSyncRequest and any later one get SessionClosed.
       The SDK sends none of them: it closes with svcCloseHandle and sends only types 6 and 7 */
    if (c->type != 4 && c->type != 5 && c->type != 6 && c->type != 7 && c->type < 16) {
        if (c->type != 2 && c->type != 15)
            WARN_LIMIT(4, "IPC: request to %s of message type %u, which sf doesn't serve: session closed\n", name,
                       c->type);
        __atomic_store_n(&s->closed, 1, __ATOMIC_RELEASE);
        cpu->x[0] = R_SESSIONCLOSED;
        goto out;
    }
    if (c->header_rc) { /* the control requests' header too */
        WARN_LIMIT(8, "IPC: request to %s with a broken header -> %x\n", s->obj->name, c->header_rc);
        c->rc = c->header_rc;
        respond(c);
        goto out;
    }
    if (c->type == 5 || c->type == 7) { /* answered plainly on a domain session too: parse left is_domain 0 */
        control(c, s);
        respond(c);
        goto out;
    }
    SvcObj* obj = s->obj;

    if (c->is_domain) {
        if (c->is_domain == 3) { /* close a domain object */
            Domain* d = __atomic_load_n(&s->domain, __ATOMIC_ACQUIRE);
            SvcObj* old = NULL;
            if (d) {
                pm_lock(&d->lk);
                if (c->domain_obj && c->domain_obj <= (u32)d->n) {
                    old = d->obj[c->domain_obj - 1];
                    d->obj[c->domain_obj - 1] = NULL;
                }
                pm_unlock(&d->lk);
            }
            if (old) svcobj_unref(old);
            c->is_domain = 1;
            respond(c);
            goto out;
        }
        if (c->is_domain != 2) { /* neither SendMessage nor Close */
            WARN_LIMIT(8, "IPC: domain request type %u to %s\n", c->is_domain - 1, name);
            c->rc = RC(10, 211); /* sf::cmif InvalidInHeader */
            respond(c);
            goto out;
        }
        obj = held = domain_get(s, c->domain_obj);
        if (!obj) {
            WARN_LIMIT(8, "IPC: domain object %u of %s isn't there\n", c->domain_obj, name);
            c->rc = RC(10, 261); /* sf::cmif TargetNotFound */
            respond(c);
            goto out;
        }
        name = obj->name;
        if (c->domain_rc) { /* after the target, as sf checks the header's sizes */
            WARN_LIMIT(8, "IPC: domain request to object %u (%s): bad header (%x)\n", c->domain_obj, name,
                       c->domain_rc);
            c->rc = c->domain_rc;
            respond(c);
            goto out;
        }
    }
    LOGV("IPC %s cmd %u\n", obj->name, c->cmd);
    {
        /* in the ring before it runs, so a crash or a hang in the service shows the request it was serving */
        u32 in0, in1;
        ipc_in(c, 0, &in0, 4);
        ipc_in(c, 4, &in1, 4);
        u32 seq = ring_claim();
        snprintf(g_ipc_ring[seq & 31], sizeof g_ipc_ring[0], "%s cmd %u (in %x %x) -> running", name, c->cmd, in0, in1);
        c->rc = (u32)obj->dispatch(obj, c);
        if (ring_mine(seq))
            snprintf(g_ipc_ring[seq & 31], sizeof g_ipc_ring[0], "%s cmd %u (in %x %x) -> %x", name, c->cmd, in0, in1,
                     c->rc);
        served = 1;
    }
    if (c->rc) drop_outputs(c);
    respond(c);
out:
    if (!served) { /* refused by the kernel or sf, or a control request or a close */
        u32 rc = (u32)cpu->x[0] ? (u32)cpu->x[0] : c->rc, seq = ring_claim();
        snprintf(g_ipc_ring[seq & 31], sizeof g_ipc_ring[0], "%s type %u cmd %u -> %x (not served)", name, c->type,
                 c->cmd, rc);
    }
    if (held) svcobj_unref(held); /* after the ring: name may live in the object */
    kobj_unref(&s->o);
}
