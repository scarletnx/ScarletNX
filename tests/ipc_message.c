/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ipc.c's parse() and ipc_handle() driven with raw HIPC messages in guest memory, laid out byte for byte as libnx's
   client builds them: plain, domain and TIPC requests, the control requests, the requests sf refuses with a result
   and the ones the kernel refuses before the server sees them, what a failed command returns, and the crash report's
   ring. The kernel under ipc.c (handles, objects, locks, guest memory, logging) is stood in for here */
#include <stdarg.h>
#include <stddef.h>

#include "nx/hle/kernel/ipc.c"
#include "nx/hle/hle.c"

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "ipc_message.c:%d: check failed: %s\n", __LINE__, #x);                                     \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* ============================================================ the kernel ipc.c calls */
u8* g_mem;
u64 g_as_size = AS_MAX; /* the reservation's size, mem.c's, which this test does not link */
static int test_log; /* SNX_TEST_LOG=1 prints what ipc.c logs */
/* single-threaded: the locks only have to be balanced, which the count checks at the end */
static int test_locks;
void pm_init(PMutex* m) { (void)m; }
void pm_lock(PMutex* m) {
    (void)m;
    test_locks++;
}
void pm_unlock(PMutex* m) {
    (void)m;
    CHECK(test_locks > 0);
    test_locks--;
}
void logf_(int level, const char* format, ...) {
    (void)level;
    if (!test_log) return;
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}
void fatal(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    exit(1);
}
/* the handle table: a handle takes a reference, as h_new does, and an object goes once its last one does */
#define H_BASE 0x100u
static KObj* handles[64];
/* guest memory blocks for ipc_user_buffer (SendSyncRequestWithUserBuffer): what is not listed is free */
static MemBlock test_blocks[16];
static int test_nblocks;
void mem_query(u64 a, MemBlock* o) {
    for (int i = 0; i < test_nblocks; i++)
        if (test_blocks[i].addr <= a && a < test_blocks[i].addr + test_blocks[i].size) {
            *o = test_blocks[i];
            return;
        }
    memset(o, 0, sizeof *o);
    o->addr = a & ~0xFFFull;
    o->size = 0x1000;
    o->state = MS_FREE;
}
void kobj_ref(KObj* o) { o->refs++; }
static void event_destroy(KObj* o);
/* what each type does as its last reference goes, as kernel.c's kobj_destroy */
void kobj_unref(KObj* o) {
    CHECK(o->refs > 0);
    if (--o->refs) return;
    if (o->type == KO_EVENT)
        event_destroy(o);
    else if (o->type == KO_SESSION)
        ksession_destroy(o);
}
u32 h_new(KObj* o) {
    for (u32 i = 0; i < 64; i++)
        if (!handles[i]) {
            kobj_ref(o);
            handles[i] = o;
            return H_BASE + i;
        }
    fatal("the test's handle table is full\n");
}
static KObj* handle_object(u32 h) { return h >= H_BASE && h - H_BASE < 64 ? handles[h - H_BASE] : NULL; }
KObj* h_get_ref(u32 h, int type) {
    KObj* o = handle_object(h);
    if (!o || (type && o->type != type)) return NULL;
    kobj_ref(o);
    return o;
}
int h_close(u32 h) {
    KObj* o = handle_object(h);
    if (!o) return R_BADHANDLE;
    handles[h - H_BASE] = NULL;
    kobj_unref(o);
    return 0;
}
/* the events a failing command hands out */
static int events_live;
static void event_destroy(KObj* o) {
    events_live--;
    free(o);
}
static u32 event_handle(void) {
    KEvent* e = calloc(1, sizeof *e);
    CHECK(e != NULL);
    e->o.type = KO_EVENT;
    events_live++;
    return h_new(&e->o);
}

/* ============================================================ the requests, as libnx lays them out */
/* ported from libnx's nx/include/switch/sf/hipc.h, cmif.h and tipc.h: hipcMakeRequest, hipcMakeBuffer,
   hipcMakeSendStatic, hipcMakeRecvStatic, cmifMakeRequest, cmifMakeControlRequest, cmifMakeCloseRequest,
   tipcMakeRequest and the response parsers, writing the words their bitfields hold.

   Copyright 2017-2018 libnx Authors

   Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby
   granted, provided that the above copyright notice and this permission notice appear in all copies.

   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL
   IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
   INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
   AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
   PERFORMANCE OF THIS SOFTWARE. */
/* the message is little-endian whatever the host's order */
static void put32(u8* p, u32 v) { snx_st_le32(p, v); }
static u32 get32(const u8* p) { return snx_ld_le32(p); }
static u64 get64(const u8* p) { return snx_ld_le64(p); }
typedef struct {
    u32 type, num_send_statics, num_send_buffers, num_recv_buffers, num_exch_buffers, num_data_words, num_recv_statics,
        send_pid, num_copy_handles, num_move_handles;
} HipcMetadata;
typedef struct {
    u8 *send_statics, *send_buffers, *recv_buffers, *exch_buffers, *data_words, *recv_list, *copy_handles,
        *move_handles;
} HipcRequest;
static HipcRequest hipc_make_request(u8* base, HipcMetadata meta) {
    u32 special = meta.send_pid || meta.num_copy_handles || meta.num_move_handles;
    u32 mode = meta.num_recv_statics ? (meta.num_recv_statics != 0xFF ? 2 + meta.num_recv_statics : 2) : 0;
    put32(base, (meta.type & 0xFFFF) | (meta.num_send_statics & 15) << 16 | (meta.num_send_buffers & 15) << 20 |
                    (meta.num_recv_buffers & 15) << 24 | (meta.num_exch_buffers & 15) << 28);
    put32(base + 4, (meta.num_data_words & 0x3FF) | (mode & 15) << 10 | special << 31);
    u8* p = base + 8;
    if (special) {
        put32(p, (meta.send_pid & 1) | (meta.num_copy_handles & 15) << 1 | (meta.num_move_handles & 15) << 5);
        p += 4 + (meta.send_pid ? 8 : 0); /* the pid's slot, which the kernel fills in */
    }
    HipcRequest r = {0};
    r.copy_handles = p, p += 4 * meta.num_copy_handles;
    r.move_handles = p, p += 4 * meta.num_move_handles;
    r.send_statics = p, p += 8 * meta.num_send_statics;
    r.send_buffers = p, p += 12 * meta.num_send_buffers;
    r.recv_buffers = p, p += 12 * meta.num_recv_buffers;
    r.exch_buffers = p, p += 12 * meta.num_exch_buffers;
    r.data_words = p, p += 4 * meta.num_data_words;
    r.recv_list = p;
    return r;
}
static void hipc_buffer(u8* d, u64 addr, u64 size, u32 mode) {
    put32(d, (u32)size);
    put32(d + 4, (u32)addr);
    put32(d + 8, (mode & 3) | (u32)((addr >> 36) & 0x3FFFFF) << 2 | (u32)((size >> 32) & 15) << 24 |
                     (u32)((addr >> 32) & 15) << 28);
}
static void hipc_send_static(u8* d, u64 addr, u64 size, u32 index) {
    put32(d,
          (index & 63) | (u32)((addr >> 36) & 63) << 6 | (u32)((addr >> 32) & 15) << 12 | (u32)(size & 0xFFFF) << 16);
    put32(d + 4, (u32)addr);
}
static void hipc_recv_static(u8* d, u64 addr, u64 size) {
    put32(d, (u32)addr);
    put32(d + 4, (u32)((addr >> 32) & 0xFFFF) | (u32)(size & 0xFFFF) << 16);
}

typedef struct {
    u32 object_id, request_id, context, data_size, server_pointer_size, num_in_auto_buffers, num_out_auto_buffers,
        num_in_buffers, num_out_buffers, num_inout_buffers, num_in_pointers, num_out_pointers, num_out_fixed_pointers,
        num_objects, num_handles, send_pid;
} CmifRequestFormat;
typedef struct {
    HipcRequest hipc;
    u8 *domain, *header, *data, *out_pointer_sizes, *objects;
} CmifRequest;
static u8* cmif_aligned_start(u8* data_words, u8* base) { return base + (((data_words - base) + 15) & ~15); }
static CmifRequest cmif_make_request(u8* base, CmifRequestFormat fmt) {
    u32 size = 16; /* the padding the data's alignment takes */
    if (fmt.object_id) size += 16 + fmt.num_objects * 4;
    size += 16 + fmt.data_size;
    size = (size + 1) & ~1u;
    u32 table = size, ntable = fmt.num_out_auto_buffers + fmt.num_out_pointers;
    size += 2 * ntable;
    CmifRequest r = {0};
    r.hipc = hipc_make_request(base, (HipcMetadata){
                                         .type = fmt.context ? 6 : 4,
                                         .num_send_statics = fmt.num_in_auto_buffers + fmt.num_in_pointers,
                                         .num_send_buffers = fmt.num_in_auto_buffers + fmt.num_in_buffers,
                                         .num_recv_buffers = fmt.num_out_auto_buffers + fmt.num_out_buffers,
                                         .num_exch_buffers = fmt.num_inout_buffers,
                                         .num_data_words = (size + 3) / 4,
                                         .num_recv_statics = ntable + fmt.num_out_fixed_pointers,
                                         .send_pid = fmt.send_pid,
                                         .num_copy_handles = fmt.num_handles,
                                     });
    u8* start = cmif_aligned_start(r.hipc.data_words, base);
    r.header = start;
    if (fmt.object_id) {
        u32 payload = 16 + fmt.data_size;
        r.domain = start;
        start[0] = 1; /* SendMessage */
        start[1] = (u8)fmt.num_objects;
        start[2] = (u8)payload, start[3] = (u8)(payload >> 8);
        put32(start + 4, fmt.object_id);
        put32(start + 8, 0);
        put32(start + 12, fmt.context);
        r.header = start + 16;
        r.objects = r.header + payload;
    }
    put32(r.header, 0x49434653); /* SFCI */
    put32(r.header + 4, fmt.context ? 1 : 0);
    put32(r.header + 8, fmt.request_id);
    put32(r.header + 12, fmt.object_id ? 0 : fmt.context);
    r.data = r.header + 16;
    r.out_pointer_sizes = r.hipc.data_words + table;
    return r;
}
static u8* cmif_make_control_request(u8* base, u32 request_id, u32 size) {
    HipcRequest hipc = hipc_make_request(base, (HipcMetadata){.type = 5, .num_data_words = (16 + 16 + size + 3) / 4});
    u8* hdr = cmif_aligned_start(hipc.data_words, base);
    put32(hdr, 0x49434653);
    put32(hdr + 4, 0);
    put32(hdr + 8, request_id);
    put32(hdr + 12, 0);
    return hdr + 16;
}
static void cmif_make_close_request(u8* base, u32 object_id) {
    if (object_id) {
        HipcRequest hipc = hipc_make_request(base, (HipcMetadata){.type = 4, .num_data_words = (16 + 16) / 4});
        u8* d = cmif_aligned_start(hipc.data_words, base);
        memset(d, 0, 16);
        d[0] = 2; /* Close */
        put32(d + 4, object_id);
    } else
        hipc_make_request(base, (HipcMetadata){.type = 2});
}
static u8* tipc_make_request(u8* base, u32 request_id, u32 data_size, u32 send_pid) {
    return hipc_make_request(
               base,
               (HipcMetadata){.type = request_id + 16, .num_data_words = (data_size + 3) / 4, .send_pid = send_pid})
        .data_words;
}

/* the reply: hipcParseResponse, then cmifParseResponse's headers */
typedef struct {
    u32 w0, num_data_words, num_copy_handles, num_move_handles;
    int special;
    u8 *copy_handles, *move_handles, *data_words;
} HipcResponse;
static HipcResponse hipc_parse_response(u8* base) {
    HipcResponse r = {0};
    u32 w1 = get32(base + 4);
    r.w0 = get32(base);
    r.num_data_words = w1 & 0x3FF;
    r.special = (int)(w1 >> 31);
    u8* p = base + 8;
    u32 statics = (r.w0 >> 16) & 15;
    if (r.special) {
        u32 sh = get32(p);
        p += 4 + ((sh & 1) ? 8 : 0);
        r.num_copy_handles = (sh >> 1) & 15;
        r.num_move_handles = (sh >> 5) & 15;
    }
    r.copy_handles = p, p += 4 * r.num_copy_handles;
    r.move_handles = p, p += 4 * r.num_move_handles;
    p += 8 * statics;
    r.data_words = p;
    return r;
}
typedef struct {
    HipcResponse hipc;
    u32 num_out_objects, result, token;
    u8 *data, *objects;
} CmifResponse;
static CmifResponse cmif_parse_response(u8* base, int is_domain, u32 size) {
    CmifResponse r = {0};
    r.hipc = hipc_parse_response(base);
    u8* hdr = cmif_aligned_start(r.hipc.data_words, base);
    if (is_domain) {
        r.num_out_objects = get32(hdr);
        CHECK(!get32(hdr + 4) && !get32(hdr + 8) && !get32(hdr + 12));
        hdr += 16;
        r.objects = hdr + 16 + size;
    }
    CHECK(get32(hdr) == 0x4F434653); /* SFCO */
    CHECK(get32(hdr + 4) == 0);
    r.result = get32(hdr + 8);
    r.token = get32(hdr + 12);
    r.data = hdr + 16;
    return r;
}

/* ============================================================ the service */
static struct {
    int calls;
    SvcObj* self;
    u32 type, cmd, token, raw_size;
    u8 raw[0x100];
    ptrdiff_t raw_off; /* where the raw data starts in the message */
    int has_pid, tipc, is_domain;
    u64 pid;
    int na, nb, nw, nx, nc, ncopy;
    IpcBuf a, b, x, c, c1;
    u32 copy_h, nin_objs, in_objs[8];
    SvcObj* in_obj[8];
} got;
static int objects_live;
static void object_destroy(SvcObj* o) {
    (void)o;
    objects_live--;
}
static int test_dispatch(SvcObj* self, IpcCtx* c);
static SvcObj* test_object(const char* name) {
    SvcObj* o = svcobj_new(name, test_dispatch);
    o->destroy = object_destroy;
    objects_live++;
    return o;
}
#define RC_FAIL RC(5, 7)
/* records what reached it. 1 answers the sum of two words and a constant, 2 fails after putting handles, an object
   and data in the reply, 3 answers an object, 4 answers a missing object, 5 answers 0xD0 bytes, 6 0x100 bytes (more
   than fits), 7 two copy handles and 0xD0 bytes, 8 claims the crash report ring's next 32 entries while it runs */
static int test_dispatch(SvcObj* self, IpcCtx* c) {
    got.calls++;
    got.self = self;
    got.type = c->type, got.cmd = c->cmd, got.token = c->token, got.raw_size = c->raw_size;
    memcpy(got.raw, c->raw, c->raw_size < sizeof got.raw ? c->raw_size : sizeof got.raw);
    got.raw_off = c->raw - c->msg;
    got.has_pid = c->has_pid, got.pid = c->pid, got.tipc = c->tipc, got.is_domain = c->is_domain;
    got.na = c->na, got.nb = c->nb, got.nw = c->nw, got.nx = c->nx, got.nc = c->nc;
    got.ncopy = c->ncopy;
    got.a = c->a[0], got.b = c->b[0], got.x = c->x[0], got.c = c->c[0], got.c1 = c->c[1], got.copy_h = c->copy_h[0];
    got.nin_objs = (u32)c->nin_objs;
    for (int i = 0; i < c->nin_objs; i++) {
        got.in_objs[i] = c->in_objs[i];
        got.in_obj[i] = ipc_in_obj(c, i);
        svcobj_unref(got.in_obj[i]);
    }
    switch (c->cmd) {
        case 1: {
            u32 a, b;
            u64 k = 0x1122334455667788ull;
            ipc_in(c, 0, &a, 4);
            ipc_in(c, 4, &b, 4);
            out32(c, snx_le32(a) + snx_le32(b));
            snx_st_le64(c->out + c->out_size, k);
            c->out_size += 8;
            return 0;
        }
        case 2:
            out_copy_h(c, event_handle());
            out_move_h(c, event_handle());
            out_obj(c, test_object("test:child"));
            out32(c, 0xBAD);
            return RC_FAIL;
        case 3: out_obj(c, test_object("test:child")); return 0;
        case 4: out_obj(c, NULL); return 0;
        case 7:
            out_copy_h(c, event_handle());
            out_copy_h(c, event_handle());
            /* fall through */
        case 5:
        case 6:
            for (u32 i = 0; i < (c->cmd == 6 ? 0x100u : 0xD0u); i += 4) out32(c, 0xA0000000u + i);
            return 0;
        case 8:
            for (int i = 0; i < 32; i++) ring_claim();
            return 0;
        default: return RC(10, 221);
    }
}

/* ============================================================ the cases */
#define MSG  0x1000ull /* the thread's TLS message buffer */
#define UBUF 0x2000ull /* a SendSyncRequestWithUserBuffer buffer, 0x1000 bytes */
static u8* msg(void) { return GP(MSG); }
static void clear(void) {
    memset(GP(MSG), 0, 0x100);
    memset(&got, 0, sizeof got);
}
/* svcSendSyncRequest(WithUserBuffer): its result, what the client's x0 gets */
static u32 send_to(u32 h, u64 addr, u64 size) {
    return ipc_handle(h, addr, size);
}
static u32 send(u32 h) { return send_to(h, MSG, 0x100); }
/* ring entry seq as the crash report prints it */
static const char* ring_text(u32 seq) {
    static char line[200];
    ipc_ring_text(seq, line, sizeof line);
    return line;
}
static const char* last_ring(void) { return ring_text(g_hle.ipc.ring_n - 1); }

/* a plain request with its CmifInHeader, raw data, the reply's words, and the ring entry */
static void plain_requests(u32 h, SvcObj* root) {
    clear();
    CmifRequest r = cmif_make_request(msg(), (CmifRequestFormat){.request_id = 1, .data_size = 8});
    put32(r.data, 0x11223344);
    put32(r.data + 4, 0x01020304);
    CHECK(get32(msg()) == 4 && get32(msg() + 4) == 10); /* 16 padding + 16 header + 8 data */
    u32 ring = g_hle.ipc.ring_n;
    CHECK(send(h) == 0);
    CHECK(got.calls == 1 && got.self == root && got.type == 4 && got.cmd == 1 && got.token == 0);
    CHECK(got.raw_size == 8 && got.raw_off == 32 && get32(got.raw) == 0x11223344 && get32(got.raw + 4) == 0x01020304);
    CHECK(!got.has_pid && !got.tipc && !got.is_domain && !got.na && !got.nb && !got.nx && !got.nc && !got.ncopy);
    /* the reply: no handles, the SFCO header at the 16-aligned start, 12 bytes of data, and 16 bytes for the
       alignment's padding counted in the data words */
    CHECK(get32(msg()) == 0 && get32(msg() + 4) == 11);
    CHECK(get32(msg() + 16) == 0x4F434653 && get32(msg() + 20) == 0 && get32(msg() + 24) == 0 &&
          get32(msg() + 28) == 0);
    CHECK(get32(msg() + 32) == 0x12243648 && get64(msg() + 36) == 0x1122334455667788ull);
    /* the crash report's ring holds the object, the command, the first raw words and the result */
    CHECK(g_hle.ipc.ring_n == ring + 1);
    CHECK(!strcmp(last_ring(), "test:p cmd 1 (in 11223344 1020304) -> 0"));

    /* with a context (type 6, version 1, the token answered back), the pid, a copy handle and each kind of buffer:
       an X, an A, a B with every address and size bit libnx encodes, a C, and its size in the out pointer table */
    clear();
    r = cmif_make_request(msg(), (CmifRequestFormat){.request_id = 1,
                                                     .context = 0x77,
                                                     .data_size = 8,
                                                     .server_pointer_size = 0x8000,
                                                     .num_in_buffers = 1,
                                                     .num_out_buffers = 1,
                                                     .num_in_pointers = 1,
                                                     .num_out_pointers = 1,
                                                     .num_handles = 1,
                                                     .send_pid = 1});
    put32(r.hipc.copy_handles, 0xFFFF8001u);
    put32(msg() + 12, 0xCAFE);                                       /* the client's pid placeholder */
    hipc_send_static(r.hipc.send_statics, 0x1123456789ull, 0x40, 0); /* bits 32-35 and 36 of the address */
    hipc_buffer(r.hipc.send_buffers, 0x3000, 0x100, 0);
    hipc_buffer(r.hipc.recv_buffers, 0x1234567000ull, 0x100000200ull, 1);
    hipc_recv_static(r.hipc.recv_list, 0x4000, 0x80);
    put32(r.data, 5);
    put32(r.data + 4, 6);
    r.out_pointer_sizes[0] = 0x80, r.out_pointer_sizes[1] = 0;
    CHECK(send(h) == 0);
    CHECK(got.calls == 1 && got.type == 6 && got.cmd == 1 && got.token == 0x77);
    CHECK(got.has_pid && got.pid == K_PROCESS_ID && got.ncopy == 1 && got.copy_h == 0xFFFF8001u);
    CHECK(got.nx == 1 && got.x.addr == 0x1123456789ull && got.x.size == 0x40);
    CHECK(got.na == 1 && got.a.addr == 0x3000 && got.a.size == 0x100 && got.a.flags == 0);
    CHECK(got.nb == 1 && got.b.addr == 0x1234567000ull && got.b.size == 0x100000200ull && got.b.flags == 1);
    CHECK(got.nc == 1 && got.c.addr == 0x4000 && got.c.size == 0x80 && !got.nw);
    /* the data starts at 0x40 (header, special header, pid, handle, X, A, B, aligned): sf measures 11 words less 16
       bytes of padding and 16 of header */
    CHECK(got.raw_off == 0x50 && got.raw_size == 12 && get32(got.raw) == 5 && get32(got.raw + 4) == 6);
    CmifResponse resp = cmif_parse_response(msg(), 0, 12);
    CHECK(!resp.hipc.special && resp.result == 0 && resp.token == 0x77 && get32(resp.data) == 11);

    /* an object in the reply of a plain session is a new session's move handle */
    clear();
    cmif_make_request(msg(), (CmifRequestFormat){.request_id = 3});
    CHECK(send(h) == 0 && got.calls == 1 && got.cmd == 3 && got.raw_size == 0);
    resp = cmif_parse_response(msg(), 0, 0);
    CHECK(resp.result == 0 && resp.hipc.special && !resp.hipc.num_copy_handles && resp.hipc.num_move_handles == 1);
    u32 child_h = get32(resp.hipc.move_handles);
    KSession* child = (KSession*)h_get_ref(child_h, KO_SESSION);
    CHECK(child && child->obj && child->obj != root && !strcmp(child->obj->name, "test:child") && !child->domain);
    kobj_unref(&child->o);
    CHECK(h_close(child_h) == 0);

    /* a missing object in the reply is handle 0 */
    clear();
    cmif_make_request(msg(), (CmifRequestFormat){.request_id = 4});
    CHECK(send(h) == 0 && got.cmd == 4);
    resp = cmif_parse_response(msg(), 0, 0);
    CHECK(resp.result == 0 && resp.hipc.num_move_handles == 1 && get32(resp.hipc.move_handles) == 0);
}

/* sf's plain processor: a CmifInHeader that the data words, as sf measures them, hold, with its magic and a version
   it knows */
static void plain_refusals(u32 h) {
    /* 7 data words: the header is in the message (bytes 0x10 to 0x20, the message ends at 0x24), but sf takes the
       data as 28 bytes less 16 of padding, 12 */
    clear();
    HipcRequest hr = hipc_make_request(msg(), (HipcMetadata){.type = 4, .num_data_words = 7});
    u8* d = cmif_aligned_start(hr.data_words, msg());
    put32(d, 0x49434653), put32(d + 8, 1);
    CHECK(send(h) == 0 && !got.calls);
    CmifResponse resp = cmif_parse_response(msg(), 0, 0);
    CHECK(resp.result == RC(10, 202) && resp.result == 0x1940A && !resp.hipc.special);
    /* the same with a pid: the data words start at 0x14 and the header at 0x20, inside the message again */
    clear();
    hr = hipc_make_request(msg(), (HipcMetadata){.type = 4, .num_data_words = 7, .send_pid = 1});
    d = cmif_aligned_start(hr.data_words, msg());
    CHECK(d == msg() + 0x20 && hr.data_words + 28 == msg() + 0x30);
    put32(d, 0x49434653), put32(d + 8, 1);
    CHECK(send(h) == 0 && !got.calls && cmif_parse_response(msg(), 0, 0).result == RC(10, 202));
    /* 8 words hold it: a bad magic, then version 2 */
    clear();
    CmifRequest r = cmif_make_request(msg(), (CmifRequestFormat){.request_id = 1});
    put32(r.header, 0x58434653); /* SFCX */
    CHECK(send(h) == 0 && !got.calls);
    CHECK(cmif_parse_response(msg(), 0, 0).result == RC(10, 211));
    CHECK(cmif_parse_response(msg(), 0, 0).result == 0x1A60A);
    clear();
    r = cmif_make_request(msg(), (CmifRequestFormat){.request_id = 1});
    put32(r.header + 4, 2);
    CHECK(send(h) == 0 && !got.calls && cmif_parse_response(msg(), 0, 0).result == RC(10, 211));
}

/* the kernel's refusals: SendSyncRequest returns them and the message stays as the client wrote it */
static void kernel_refusals(u32 h) {
    static u8 before[0x1000];
    /* a header that describes more than the 0x100 byte TLS buffer: InvalidCombination */
    clear();
    hipc_make_request(msg(), (HipcMetadata){.type = 4, .num_data_words = 0x3FF});
    memcpy(before, msg(), 0x100);
    CHECK(send(h) == 0xE801 && !got.calls && !memcmp(before, msg(), 0x100));
    /* a move handle, which a client can't send a server: InvalidCombination, ahead of a B buffer outside the address
       space. The handle stays the client's */
    clear();
    HipcRequest hr = hipc_make_request(
        msg(), (HipcMetadata){.type = 4, .num_recv_buffers = 1, .num_data_words = 8, .num_move_handles = 1});
    put32(hr.move_handles, h);
    hipc_buffer(hr.recv_buffers, 0x7F00000000ull, 0x100, 0);
    memcpy(before, msg(), 0x100);
    CHECK(send(h) == 0xE801 && !got.calls && !memcmp(before, msg(), 0x100));
    /* a close goes through the same checks: refused, and not answered with success */
    clear();
    hr = hipc_make_request(msg(), (HipcMetadata){.type = 2, .num_move_handles = 1});
    put32(hr.move_handles, h);
    CHECK(send(h) == 0xE801);
    /* a B buffer outside the address space, then one that runs past its end: InvalidCurrentMemory */
    clear();
    CmifRequest r = cmif_make_request(msg(), (CmifRequestFormat){.request_id = 1, .num_out_buffers = 1});
    hipc_buffer(r.hipc.recv_buffers, 0x7F00000000ull, 0x100, 0);
    memcpy(before, msg(), 0x100);
    CHECK(send(h) == 0xD401 && !got.calls && !memcmp(before, msg(), 0x100));
    clear();
    r = cmif_make_request(msg(), (CmifRequestFormat){.request_id = 1, .num_out_buffers = 1});
    hipc_buffer(r.hipc.recv_buffers, AS_SIZE - 0x1000, 0x2000, 0);
    CHECK(send(h) == 0xD401 && !got.calls);
    /* through a user buffer of 0x1000 bytes: a 0x118 byte message fits it but not the server's 0x100 byte buffer,
       MessageTooLarge. In the TLS buffer the same message describes more than the buffer */
    memset(GP(UBUF), 0, 0x1000);
    clear();
    r = cmif_make_request(GP(UBUF), (CmifRequestFormat){.request_id = 1, .data_size = 0xF0});
    CHECK(8 + 4 * (get32(GP(UBUF) + 4) & 0x3FF) == 0x118);
    memcpy(before, GP(UBUF), 0x1000);
    CHECK(send_to(h, UBUF, 0x1000) == 0x20801 && !got.calls && !memcmp(before, GP(UBUF), 0x1000));
    memcpy(msg(), GP(UBUF), 0x100);
    CHECK(send(h) == 0xE801 && !got.calls);
    /* a message within 0x100 bytes is served from the user buffer, and answered there */
    memset(GP(UBUF), 0, 0x1000);
    r = cmif_make_request(GP(UBUF), (CmifRequestFormat){.request_id = 1, .data_size = 8});
    put32(r.data, 1), put32(r.data + 4, 2);
    CHECK(send_to(h, UBUF, 0x1000) == 0 && got.calls == 1 && got.raw_size == 8);
    CmifResponse resp = cmif_parse_response(GP(UBUF), 0, 12);
    CHECK(resp.result == 0 && get32(resp.data) == 3);
    /* twelve send statics and ten A buffers, more than the arrays hold: the first MAX_BUFS of each are kept, and every
       one is checked against the address space, past the arrays too */
    for (int bad = 0; bad < 3; bad++) {
        clear();
        r = cmif_make_request(msg(), (CmifRequestFormat){.request_id = 9, .num_in_pointers = 12, .num_in_buffers = 10});
        for (u32 i = 0; i < 12; i++) hipc_send_static(r.hipc.send_statics + 8 * i, 0x10000 + 0x100 * i, 0x10, i);
        for (u32 i = 0; i < 10; i++) hipc_buffer(r.hipc.send_buffers + 12 * i, 0x20000 + 0x100 * i, 0x10, 0);
        if (bad == 1) hipc_send_static(r.hipc.send_statics + 8 * 11, 0x7F00000000ull, 0x10, 11);
        if (bad == 2) hipc_buffer(r.hipc.send_buffers + 12 * 9, 0x7F00000000ull, 0x10, 0);
        if (bad)
            CHECK(send(h) == 0xD401 && !got.calls);
        else
            CHECK(send(h) == 0 && got.calls == 1 && got.nx == MAX_BUFS && got.na == MAX_BUFS && got.x.addr == 0x10000 &&
                  got.a.addr == 0x20000);
    }
    /* a receive list entry outside the address space: InvalidCurrentMemory too */
    clear();
    r = cmif_make_request(msg(), (CmifRequestFormat){.request_id = 9, .num_out_fixed_pointers = 2});
    hipc_recv_static(r.hipc.recv_list, 0x10000, 0x100);
    hipc_recv_static(r.hipc.recv_list + 8, 0x7F00000000ull, 0x100);
    CHECK(send(h) == 0xD401 && !got.calls);
    /* raw data past the server's buffer comes before a move handle: MessageTooLarge */
    memset(GP(UBUF), 0, 0x1000);
    hr = hipc_make_request(GP(UBUF), (HipcMetadata){.type = 4, .num_data_words = 0x40, .num_move_handles = 1});
    put32(hr.move_handles, h);
    CHECK(send_to(h, UBUF, 0x1000) == 0x20801);
    /* a number that isn't a handle, and a handle that isn't a session */
    CHECK(send(0x55) == 0xE401);
    u32 ev = event_handle();
    CHECK(send(ev) == 0xE401 && h_close(ev) == 0);
}

/* the receive list (C descriptors), as Mesosphere's ReceiveMessage places and measures it: after the raw data, or at
   the word the header's recv_list_offset (w1 bits 20-30) names. libnx leaves the offset 0, so the test sets it by
   hand. The whole message with its receive list has to fit the client's buffer and the list can't start inside the
   raw data (InvalidCombination: the console finds that one at the reply, the runtime after the request's descriptors
   and before the handler), and only the raw data has to fit the server's 0x100 bytes (MessageTooLarge) */
static void set_recv_list_offset(u8* base, u32 word) {
    put32(base + 4, (get32(base + 4) & ~(0x7FFu << 20)) | word << 20);
}
/* SendSyncRequestWithUserBuffer's buffer, as Mesosphere checks it: page-aligned, whole pages, then a range it can
   lock (memory that allows IPC user buffers, read-write, no attribute) */
static void user_buffers(void) {
    static const MemBlock blocks[] = {
        {0x10000, 0x4000, MS_NORMAL, PERM_RW, 0, 0, 0, 0}, /* heap */
        {0x14000, 0x1000, MS_CODE, PERM_RW, 0, 0, 0, 0},   /* code, RW as the loader first maps it: no user buffers */
        {0x15000, 0x1000, MS_NORMAL, PERM_R, 0, 0, 0, 0},  /* read-only */
        {0x16000, 0x1000, MS_NORMAL, PERM_RW, 1, 0, 0, 0}, /* locked */
        {0x17000, 0x1000, MS_NORMAL, PERM_RW, 0, 0, 0, 0}, /* heap, then module data: each takes them, not as one */
        {0x18000, 0x1000, MS_CODEDATA, PERM_RW, 0, 0, 0, 0},
        {0x19000, 0x1000, MS_NORMAL, PERM_RW, 0, 0, 0, 0}, /* read-write, then read-only: not one permission */
        {0x1A000, 0x1000, MS_NORMAL, PERM_R, 0, 0, 0, 0},
        {0x1B000, 0x1000, MS_NORMAL, PERM_RW, 0, 0, 0, 0}, /* then a locked block */
        {0x1C000, 0x1000, MS_NORMAL, PERM_RW, 1, 0, 0, 0},
        {0x1D000, 0x1000, MS_NORMAL, PERM_RW | PERM_DONTCARE, 0, 0, 0, 0}, /* RW with more bits: not read-write */
    };
    memcpy(test_blocks, blocks, sizeof blocks);
    test_nblocks = 11;
    CHECK(ipc_user_buffer(0x10000, 0x1000) == 0);
    CHECK(ipc_user_buffer(0x10000, 0x4000) == 0);
    CHECK(ipc_user_buffer(0x10800, 0x1000) == 0xCC01); /* InvalidAddress */
    CHECK(ipc_user_buffer(0x10000, 0) == 0xCA01);      /* InvalidSize */
    CHECK(ipc_user_buffer(0x10000, 0x800) == 0xCA01);
    CHECK(ipc_user_buffer(0x13000, 0x2000) == 0xD401); /* into code: InvalidCurrentMemory */
    CHECK(ipc_user_buffer(0x14000, 0x1000) == 0xD401); /* code, read-write: its state takes no user buffer */
    CHECK(ipc_user_buffer(0x17000, 0x1000) == 0 && ipc_user_buffer(0x18000, 0x1000) == 0);
    CHECK(ipc_user_buffer(0x17000, 0x2000) == 0xD401); /* two states in one range */
    CHECK(ipc_user_buffer(0x19000, 0x1000) == 0 && ipc_user_buffer(0x19000, 0x2000) == 0xD401);
    CHECK(ipc_user_buffer(0x1B000, 0x1000) == 0 && ipc_user_buffer(0x1B000, 0x2000) == 0xD401);
    CHECK(ipc_user_buffer(0x1D000, 0x1000) == 0xD401);
    CHECK(ipc_user_buffer(0x15000, 0x1000) == 0xD401);
    CHECK(ipc_user_buffer(0x16000, 0x1000) == 0xD401);
    CHECK(ipc_user_buffer(0x20000, 0x1000) == 0xD401); /* free */
    CHECK(ipc_user_buffer(AS_SIZE - 0x1000, 0x2000) == 0xD401);
    CHECK(ipc_user_buffer(~0xFFFull, 0x2000) == 0xD401); /* wraps */
    test_nblocks = 0;
}
static void receive_lists(u32 h, SvcObj* root) {
    static u8 before[0x1000];
    /* through the user buffer: raw data ending at 0xF8 (60 data words), two C entries at 0xF8 and 0x100, past the
       server's 0x100 bytes. Served, with both decoded, the second's address past 4 GiB */
    memset(GP(UBUF), 0, 0x1000);
    clear();
    CmifRequest r = cmif_make_request(
        GP(UBUF), (CmifRequestFormat){.request_id = 1, .data_size = 0xD0, .num_out_fixed_pointers = 2});
    CHECK(r.hipc.recv_list == GP(UBUF) + 0xF8 && (get32(GP(UBUF) + 4) >> 10 & 15) == 4);
    put32(r.data, 1), put32(r.data + 4, 2);
    hipc_recv_static(r.hipc.recv_list, 0x5000, 0x40);
    hipc_recv_static(r.hipc.recv_list + 8, 0x123456000ull, 0x1000);
    CHECK(send_to(h, UBUF, 0x1000) == 0);
    CHECK(got.calls == 1 && got.self == root && got.cmd == 1 && got.raw_size == 0xD0);
    CHECK(got.nc == 2 && got.c.addr == 0x5000 && got.c.size == 0x40);
    CHECK(got.c1.addr == 0x123456000ull && got.c1.size == 0x1000);
    CmifResponse resp = cmif_parse_response(GP(UBUF), 0, 12);
    CHECK(resp.result == 0 && get32(resp.data) == 3);

    /* a plain request with 8 bytes of data ends its raw data at word 12 (0x30), and has one C entry */
    for (u32 word = 8; word <= 0x3F; word++) {
        clear();
        r = cmif_make_request(msg(), (CmifRequestFormat){.request_id = 1, .data_size = 8, .num_out_fixed_pointers = 1});
        CHECK(r.hipc.recv_list == msg() + 0x30);
        put32(r.data, 3), put32(r.data + 4, 4);
        set_recv_list_offset(msg(), word);
        /* where the list would be without the offset, and where the offset puts it */
        hipc_recv_static(msg() + 0x30, 0xDEAD000, 0x10);
        if (word * 4 + 8 <= 0x100) hipc_recv_static(msg() + word * 4, 0x6000 + word, 0x20);
        memcpy(before, msg(), 0x100);
        u32 rc = send(h);
        if (word < 12) { /* inside the raw data */
            CHECK(rc == 0xE801 && !got.calls && !memcmp(before, msg(), 0x100));
        } else if (word * 4 + 8 > 0x100) { /* the list runs past the client's buffer */
            CHECK(word == 0x3F && rc == 0xE801 && !got.calls && !memcmp(before, msg(), 0x100));
        } else { /* served, its C buffer read where the offset says */
            CHECK(rc == 0 && got.calls == 1 && got.raw_size == 8 && get32(got.raw) == 3);
            CHECK(got.nc == 1 && got.c.addr == 0x6000 + word && got.c.size == 0x20);
            CHECK(cmif_parse_response(msg(), 0, 12).result == 0);
        }
    }

    /* in TLS: raw data past the server's 0x100 bytes, with the receive list inside it so the message fits the
       client's buffer: MessageTooLarge, which the console answers before it would look at the receive list */
    clear();
    r = cmif_make_request(msg(), (CmifRequestFormat){.request_id = 1, .data_size = 0xF0, .num_out_fixed_pointers = 1});
    set_recv_list_offset(msg(), 4);
    hipc_recv_static(msg() + 16, 0x6000, 0x20);
    memcpy(before, msg(), 0x100);
    CHECK(send(h) == 0x20801 && !got.calls && !memcmp(before, msg(), 0x100));
    /* a receive list inside the raw data and a B buffer outside the address space: the buffer's InvalidCurrentMemory,
       which the console finds first (the receive list only at the reply) */
    clear();
    r = cmif_make_request(
        msg(), (CmifRequestFormat){.request_id = 1, .data_size = 8, .num_out_buffers = 1, .num_out_fixed_pointers = 1});
    hipc_buffer(r.hipc.recv_buffers, 0x7F00000000ull, 0x100, 0);
    set_recv_list_offset(msg(), 8);
    memcpy(before, msg(), 0x100);
    CHECK(send(h) == 0xD401 && !got.calls && !memcmp(before, msg(), 0x100));
    /* the same with the buffer inside: the receive list's InvalidCombination */
    hipc_buffer(r.hipc.recv_buffers, 0x5000, 0x100, 0);
    memcpy(before, msg(), 0x100);
    CHECK(send(h) == 0xE801 && !got.calls && !memcmp(before, msg(), 0x100));
}

/* TIPC: type 16 + the command, the raw data right after the headers, the pid only in the special header */
static void tipc_request(u32 h, SvcObj* root) {
    clear();
    u8* data = tipc_make_request(msg(), 1, 8, 1);
    put32(msg() + 12, 0xCAFE);
    put32(data, 0xAABBCCDD), put32(data + 4, 0x11111111);
    CHECK(data == msg() + 20 && get32(msg()) == 17 && get32(msg() + 4) == (2u | 0x80000000u));
    CHECK(send(h) == 0);
    CHECK(got.calls == 1 && got.self == root && got.tipc && got.type == 17 && got.cmd == 1);
    CHECK(got.raw_off == 20 && got.raw_size == 8 && get32(got.raw) == 0xAABBCCDD);
    CHECK(got.has_pid && got.pid == K_PROCESS_ID && !got.ncopy);
    /* tipcParseResponse: the result in the first data word, then the data */
    HipcResponse resp = hipc_parse_response(msg());
    CHECK(!resp.special && resp.num_data_words == 4 && resp.data_words == msg() + 8);
    CHECK(get32(resp.data_words) == 0 && get32(resp.data_words + 4) == 0xBBCCDDEE &&
          get64(resp.data_words + 8) == 0x1122334455667788ull);
}

/* a failed command returns no handles, objects or data: the ones it made are closed and dropped */
static void failed_command(u32 h, int domain) {
    int events = events_live, objects = objects_live;
    clear();
    cmif_make_request(msg(), (CmifRequestFormat){.object_id = domain ? 1 : 0, .request_id = 2});
    u32 ring = g_hle.ipc.ring_n;
    CHECK(send(h) == 0 && got.calls == 1 && got.cmd == 2);
    CHECK(events_live == events && objects_live == objects);
    for (u32 i = 0; i < 64; i++) CHECK(!handles[i] || handles[i]->type == KO_SESSION);
    CHECK(get32(msg() + 4) == (domain ? 12u : 8u)); /* no special header, the padding's 16, the headers, no data */
    CmifResponse resp = cmif_parse_response(msg(), domain, 0);
    CHECK(resp.result == RC_FAIL && !resp.num_out_objects && !get32(resp.data));
    CHECK(g_hle.ipc.ring_n == ring + 1 && !strcmp(last_ring(), "test:p cmd 2 (in 0 0) -> e05"));
}

/* a domain request's header (sf_cmif_domain_service_object.cpp): InvalidHeaderSize before the target is looked up,
   TargetNotFound, then the sizes, the number of objects and the object's CmifInHeader */
static void domain_requests(u32 h, SvcObj* root) {
    /* ConvertCurrentObjectToDomain answers the session's object as id 1, in a plain reply */
    clear();
    cmif_make_control_request(msg(), 0, 0);
    CHECK(get32(msg()) == 5 && get32(msg() + 4) == 8);
    CHECK(send(h) == 0 && !got.calls);
    CmifResponse resp = cmif_parse_response(msg(), 0, 4);
    CHECK(resp.result == 0 && get32(resp.data) == 1);
    KSession* s = (KSession*)handle_object(h);
    CHECK(s->domain && s->domain->n == 1 && s->domain->obj[0] == root);
    /* QueryPointerBufferSize, still a control request answered plainly */
    clear();
    cmif_make_control_request(msg(), 3, 0);
    CHECK(send(h) == 0);
    resp = cmif_parse_response(msg(), 0, 4);
    CHECK(resp.result == 0 && get32(resp.data) == 0x8000);
    /* a control command sf doesn't have: UnknownCommandId */
    clear();
    cmif_make_control_request(msg(), 9, 0);
    CHECK(send(h) == 0 && cmif_parse_response(msg(), 0, 0).result == RC(10, 221));

    /* a request to object 1 with two in objects, both object 1 */
    clear();
    CmifRequest r = cmif_make_request(
        msg(), (CmifRequestFormat){.object_id = 1, .request_id = 1, .data_size = 8, .num_objects = 2});
    put32(r.data, 7), put32(r.data + 4, 8);
    put32(r.objects, 1), put32(r.objects + 4, 1);
    CHECK(get32(msg() + 4) == 16 && r.domain[2] == 24); /* 16 + domain header 16 + 8 of objects + 16 + 8 */
    CHECK(send(h) == 0);
    CHECK(got.calls == 1 && got.self == root && got.cmd == 1 && got.is_domain == 2 && got.raw_size == 8);
    CHECK(got.raw_off == 48 && get32(got.raw) == 7 && get32(got.raw + 4) == 8);
    CHECK(got.nin_objs == 2 && got.in_objs[0] == 1 && got.in_objs[1] == 1 && got.in_obj[0] == root &&
          got.in_obj[1] == root);
    CHECK(get32(msg() + 4) == 15); /* 16 for the padding, the domain and SFCO headers, 12 of data */
    resp = cmif_parse_response(msg(), 1, 12);
    CHECK(resp.result == 0 && !resp.num_out_objects && get32(resp.data) == 15);
    CHECK(!strcmp(last_ring(), "test:p cmd 1 (in 7 8) -> 0"));

    /* an object out of a domain request is the domain's next id, which then takes requests */
    clear();
    cmif_make_request(msg(), (CmifRequestFormat){.object_id = 1, .request_id = 3});
    CHECK(send(h) == 0 && got.calls == 1 && got.cmd == 3);
    resp = cmif_parse_response(msg(), 1, 0);
    CHECK(resp.result == 0 && resp.num_out_objects == 1 && !resp.hipc.special && get32(resp.objects) == 2);
    SvcObj* child = s->domain->obj[1];
    CHECK(child && child != root);
    clear();
    r = cmif_make_request(msg(), (CmifRequestFormat){.object_id = 2, .request_id = 1, .data_size = 8});
    CHECK(send(h) == 0 && got.calls == 1 && got.self == child && got.nin_objs == 0);
    CHECK(!strcmp(last_ring(), "test:child cmd 1 (in 0 0) -> 0"));
    /* libnx's Close, data_size 0: the object goes, and the answer is 0 */
    int objects = objects_live;
    clear();
    cmif_make_close_request(msg(), 2);
    CHECK(get32(msg() + 4) == 8 && msg()[16] == 2 && get32(msg() + 20) == 2 && !msg()[18] && !msg()[19]);
    CHECK(send(h) == 0 && !got.calls && objects_live == objects - 1 && !s->domain->obj[1]);
    resp = cmif_parse_response(msg(), 1, 0);
    CHECK(resp.result == 0 && !resp.num_out_objects);
    /* the next object out takes the freed id 2 again, and closing it frees it */
    clear();
    cmif_make_request(msg(), (CmifRequestFormat){.object_id = 1, .request_id = 3});
    CHECK(send(h) == 0 && get32(cmif_parse_response(msg(), 1, 0).objects) == 2 && s->domain->obj[1]);
    clear();
    cmif_make_close_request(msg(), 2);
    CHECK(send(h) == 0 && !s->domain->obj[1] && objects_live == objects - 1);
    /* a missing object out of a domain request is id 0 */
    clear();
    cmif_make_request(msg(), (CmifRequestFormat){.object_id = 1, .request_id = 4});
    CHECK(send(h) == 0);
    resp = cmif_parse_response(msg(), 1, 0);
    CHECK(resp.result == 0 && resp.num_out_objects == 1 && get32(resp.objects) == 0);
    /* the closed id, and one never given: TargetNotFound */
    for (u32 id = 2; id <= 9; id += 7) {
        clear();
        cmif_make_request(msg(), (CmifRequestFormat){.object_id = id, .request_id = 1, .data_size = 8});
        CHECK(send(h) == 0 && !got.calls);
        resp = cmif_parse_response(msg(), 1, 0);
        CHECK(resp.result == RC(10, 261) && resp.result == 0x20A0A);
    }

    /* a domain header the data doesn't hold (6 words: 24 bytes less 16 of padding): InvalidHeaderSize, though its
       object doesn't exist, and the reply is a domain one */
    clear();
    HipcRequest hr = hipc_make_request(msg(), (HipcMetadata){.type = 4, .num_data_words = 6});
    u8* d = cmif_aligned_start(hr.data_words, msg());
    d[0] = 1, put32(d + 4, 9);
    CHECK(send(h) == 0 && !got.calls && cmif_parse_response(msg(), 1, 0).result == RC(10, 202));
    /* its data and objects past the data words: one object counted after 24 bytes of data, with the target there
       (InvalidHeaderSize) and without it (TargetNotFound comes first) */
    for (u32 id = 1; id <= 9; id += 8) {
        clear();
        r = cmif_make_request(msg(), (CmifRequestFormat){.object_id = id, .request_id = 1, .data_size = 8});
        r.domain[1] = 1;
        CHECK(send(h) == 0 && !got.calls);
        CHECK(cmif_parse_response(msg(), 1, 0).result == (id == 1 ? RC(10, 202) : RC(10, 261)));
    }
    /* 9 objects, all within the data: InvalidNumInObjects */
    clear();
    r = cmif_make_request(msg(),
                          (CmifRequestFormat){.object_id = 1, .request_id = 1, .data_size = 8, .num_objects = 9});
    for (int i = 0; i < 9; i++) put32(r.objects + 4 * i, 1);
    CHECK(send(h) == 0 && !got.calls);
    CHECK(cmif_parse_response(msg(), 1, 0).result == RC(10, 235));
    CHECK(cmif_parse_response(msg(), 1, 0).result == 0x1D60A);
    /* the object's CmifInHeader: shorter than 16 bytes, a bad magic, version 2 */
    clear();
    r = cmif_make_request(msg(), (CmifRequestFormat){.object_id = 1, .request_id = 1, .data_size = 8});
    r.domain[2] = 8;
    CHECK(send(h) == 0 && !got.calls && cmif_parse_response(msg(), 1, 0).result == RC(10, 202));
    clear();
    r = cmif_make_request(msg(), (CmifRequestFormat){.object_id = 1, .request_id = 1, .data_size = 8});
    put32(r.header, 0x58434653);
    CHECK(send(h) == 0 && !got.calls && cmif_parse_response(msg(), 1, 0).result == RC(10, 211));
    clear();
    r = cmif_make_request(msg(), (CmifRequestFormat){.object_id = 1, .request_id = 1, .data_size = 8});
    put32(r.header + 4, 2);
    CHECK(send(h) == 0 && !got.calls && cmif_parse_response(msg(), 1, 0).result == RC(10, 211));
    /* a domain request type that is neither SendMessage nor Close: InvalidInHeader */
    clear();
    r = cmif_make_request(msg(), (CmifRequestFormat){.object_id = 1, .request_id = 1, .data_size = 8});
    r.domain[0] = 3;
    CHECK(send(h) == 0 && !got.calls && cmif_parse_response(msg(), 1, 0).result == RC(10, 211));
}

/* the reply's data words, as the SDK's client checks them: no more than its 0x100 byte buffer, the largest replies
   swipc allows exactly at it, and a reply built by hand that doesn't fit cut to what does */
static void reply_sizes(void) {
    u32 h = session_new(test_object("test:r"));
    clear();
    cmif_make_request(msg(), (CmifRequestFormat){.request_id = 5});
    CHECK(send(h) == 0 && (get32(msg() + 4) & 0x3FF) == 0x3C); /* 16 + 16 + 0xD0 */
    CmifResponse resp = cmif_parse_response(msg(), 0, 0xD0);
    CHECK(get32(resp.data) == 0xA0000000u && get32(resp.data + 0xCC) == 0xA00000CCu);
    /* 0x100 bytes in out[]: 0xE0 fit after the headers, the data words come to 0x100 */
    clear();
    cmif_make_request(msg(), (CmifRequestFormat){.request_id = 6});
    CHECK(send(h) == 0 && (get32(msg() + 4) & 0x3FF) == 0x40);
    resp = cmif_parse_response(msg(), 0, 0xE0);
    CHECK(get32(resp.data + 0xDC) == 0xA00000DCu && resp.data + 0xE0 == msg() + 0x100);
    /* two copy handles: 12 bytes of padding, the data's end exactly at the buffer's */
    clear();
    cmif_make_request(msg(), (CmifRequestFormat){.request_id = 7});
    CHECK(send(h) == 0 && (get32(msg() + 4) & 0x3FF) == 0x3C);
    resp = cmif_parse_response(msg(), 0, 0xD0);
    CHECK(resp.hipc.num_copy_handles == 2 && resp.data + 0xD0 == msg() + 0x100);
    CHECK(get32(resp.data + 0xCC) == 0xA00000CCu);
    CHECK(h_close(get32(resp.hipc.copy_handles)) == 0 && h_close(get32(resp.hipc.copy_handles + 4)) == 0);
    /* the largest domain reply swipc allows: 0xD0 bytes, data words of 0x100 */
    clear();
    cmif_make_control_request(msg(), 0, 0);
    CHECK(send(h) == 0);
    clear();
    cmif_make_request(msg(), (CmifRequestFormat){.object_id = 1, .request_id = 5});
    CHECK(send(h) == 0 && (get32(msg() + 4) & 0x3FF) == 0x40);
    CHECK(get32(cmif_parse_response(msg(), 1, 0xD0).data + 0xCC) == 0xA00000CCu);
    CHECK(h_close(h) == 0);
}

/* the crash report's ring: one entry a request, served or not, and a request whose service runs long leaves alone the
   entry that took its slot meanwhile */
static void ring_entries(u32 h) {
    u32 ring = g_hle.ipc.ring_n;
    CHECK(send(0x55) == 0xE401 && g_hle.ipc.ring_n == ring + 1 && strstr(last_ring(), "not a session"));
    clear();
    hipc_make_request(msg(), (HipcMetadata){.type = 4, .num_data_words = 0x3FF}); /* the kernel refuses it */
    CHECK(send(h) == 0xE801 && g_hle.ipc.ring_n == ring + 2 && strstr(last_ring(), "not served"));
    clear();
    cmif_make_control_request(msg(), 3, 0);
    CHECK(send(h) == 0 && g_hle.ipc.ring_n == ring + 3 && strstr(last_ring(), "not served"));
    clear();
    cmif_make_request(msg(), (CmifRequestFormat){.request_id = 8});
    u32 slot = g_hle.ipc.ring_n & 31;
    CHECK(send(h) == 0 && g_hle.ipc.ring_n == ring + 3 + 33);
    CHECK(strstr(ring_text(slot), "cmd 8") && strstr(ring_text(slot), "-> running")); /* not rewritten */
}

/* auto_stub_ipc: a command no interface declares answers success, or the console's UnknownCommandId when it's off */
static void undeclared_commands(void) {
    SvcObj o;
    IpcCtx c;
    memset(&o, 0, sizeof o);
    memset(&c, 0, sizeof c);
    o.name = "test:p";
    c.cmd = 77;
    c.msg = msg(); /* the request's message, as ipc_handle gives it: the answer is written over it */
    c.msg_size = 0x100;
    memset(c.msg, 0xA5, c.msg_size);
    CHECK(stub_ok(&o, &c) == 0 && c.out_size == 0 && !c.msg[0] && !c.msg[0xFF]);
    g_settings.answer_unknown_commands = 0;
    CHECK(stub_ok(&o, &c) == (int)RC(10, 221));
    g_settings.answer_unknown_commands = 1;
}
/* ipc_warn_once: one warning per service and command, a name told apart by its first 127 characters, and nothing
   new once WARN_PAIRS pairs are taken. Run last: it fills the table */
static void warn_once(void) {
    char a[200], b[200];
    memset(a, 'a', 199), a[199] = 0;
    memcpy(b, a, sizeof b), b[150] = 'b'; /* differs past the 127th character */
    CHECK(ipc_warn_once("warn:x", 5) == 1);
    CHECK(ipc_warn_once("warn:x", 5) == 0 && ipc_warn_once("warn:x", 5) == 0);
    CHECK(ipc_warn_once("warn:x", 6) == 1); /* another command counts on its own */
    CHECK(ipc_warn_once("warn:y", 5) == 1); /* and another service */
    CHECK(ipc_warn_once(a, 1) == 1 && ipc_warn_once(b, 1) == 0); /* the same 127 characters */
    for (int i = 0; i < 100000; i++) ipc_warn_once("warn:x", 7); /* repeats stay quiet and take no room */
    CHECK(ipc_warn_once("warn:x", 7) == 0);
    for (u32 cmd = 1000; cmd < 1600; cmd++) ipc_warn_once("warn:fill", cmd);
    CHECK(ipc_warn_once("warn:new", 1) == 0); /* the table is full */
    CHECK(ipc_warn_once("warn:x", 5) == 0 && ipc_warn_once("warn:fill", 1000) == 0); /* old pairs are still found */
}

/* the control requests on a domain session: CloneCurrentObject shares the domain, closing the clone and the original
   in either order lets go of it once; CopyFromCurrentDomain gives an object's own session */
static void clones(void) {
    int objects = objects_live;
    SvcObj* root = test_object("test:c");
    u32 h = session_new(root);
    clear();
    cmif_make_control_request(msg(), 0, 0);
    CHECK(send(h) == 0);
    Domain* d = ((KSession*)handle_object(h))->domain;
    CHECK(d && d->refs == 1);
    for (int order = 0; order < 2; order++) {
        clear();
        cmif_make_control_request(msg(), order ? 4 : 2, order ? 4 : 0); /* CloneCurrentObjectEx takes a tag */
        CHECK(send(h) == 0);
        CmifResponse resp = cmif_parse_response(msg(), 0, 0);
        CHECK(resp.result == 0 && resp.hipc.num_move_handles == 1);
        u32 ch = get32(resp.hipc.move_handles);
        KSession* clone = (KSession*)handle_object(ch);
        CHECK(clone && clone->domain == d && clone->obj == root && d->refs == 2);
        /* the clone serves the domain's objects */
        clear();
        cmif_make_request(msg(), (CmifRequestFormat){.object_id = 1, .request_id = 1, .data_size = 8});
        CHECK(send(ch) == 0 && got.self == root);
        if (order) { /* the original first: the clone keeps the domain */
            CHECK(h_close(h) == 0 && d->refs == 1 && objects_live == objects + 1);
            h = ch;
        } else {
            CHECK(h_close(ch) == 0 && d->refs == 1);
        }
    }
    /* CopyFromCurrentDomain: object 1's own session; an id the domain doesn't have, DomainObjectNotFound */
    clear();
    put32(cmif_make_control_request(msg(), 1, 4), 1);
    CHECK(send(h) == 0);
    CmifResponse resp = cmif_parse_response(msg(), 0, 0);
    CHECK(resp.result == 0 && resp.hipc.num_move_handles == 1);
    u32 own = get32(resp.hipc.move_handles);
    KSession* os = (KSession*)handle_object(own);
    CHECK(os && os->obj == root && !os->domain);
    CHECK(h_close(own) == 0);
    clear();
    put32(cmif_make_control_request(msg(), 1, 4), 9);
    CHECK(send(h) == 0 && cmif_parse_response(msg(), 0, 0).result == 0x3D80B);
    CHECK(h_close(h) == 0 && objects_live == objects);
    /* on a session that isn't a domain: TargetNotDomain */
    root = test_object("test:c");
    h = session_new(root);
    clear();
    put32(cmif_make_control_request(msg(), 1, 4), 1);
    CHECK(send(h) == 0 && cmif_parse_response(msg(), 0, 0).result == 0x3D60B);
    CHECK(h_close(h) == 0 && objects_live == objects);
}

/* what sf closes the session for: a message type it doesn't serve, a TIPC close, a CMIF close. The client's
   SendSyncRequest and every later one get SessionClosed, and the crash report's ring shows them */
static void session_closes(void) {
    for (u32 type = 0; type < 16; type++) {
        if (type >= 4 && type <= 7) continue;
        int objects = objects_live;
        u32 h = session_new(test_object("test:x"));
        static u8 before[0x100];
        clear();
        hipc_make_request(msg(), (HipcMetadata){.type = type, .num_data_words = 8});
        memcpy(before, msg(), 0x100);
        u32 ring = g_hle.ipc.ring_n;
        CHECK(send(h) == 0xF601 && !got.calls && ((KSession*)handle_object(h))->closed);
        CHECK(!memcmp(before, msg(), 0x100)); /* no reply */
        CHECK(g_hle.ipc.ring_n == ring + 1 && strstr(last_ring(), "not served"));
        clear();
        cmif_make_request(msg(), (CmifRequestFormat){.request_id = 1, .data_size = 8});
        memcpy(before, msg(), 0x100);
        CHECK(send(h) == 0xF601 && !got.calls && !memcmp(before, msg(), 0x100));
        CHECK(g_hle.ipc.ring_n == ring + 2 && strstr(last_ring(), "session closed"));
        CHECK(h_close(h) == 0 && objects_live == objects);
    }
}


int main(void) {
    hle_init();
    user_buffers();
    test_log = getenv("SNX_TEST_LOG") != NULL;
    g_mem = calloc(1, 0x10000);
    CHECK(g_mem != NULL);

    undeclared_commands();
    reply_sizes();
    SvcObj* root = test_object("test:p");
    u32 h = session_new(root);
    plain_requests(h, root);
    ring_entries(h);
    plain_refusals(h);
    kernel_refusals(h);
    receive_lists(h, root);
    tipc_request(h, root);
    failed_command(h, 0);
    domain_requests(h, root);
    failed_command(h, 1);
    clones();
    session_closes();
    /* a plain Close (type 2): sf closes the session, and the client then closes its handle */
    clear();
    cmif_make_close_request(msg(), 0);
    CHECK(send(h) == 0xF601 && !got.calls && handle_object(h));
    warn_once();

    /* closing the session lets go of the domain and every object */
    CHECK(h_close(h) == 0);
    for (u32 i = 0; i < 64; i++) CHECK(!handles[i]);
    CHECK(objects_live == 0 && events_live == 0 && test_locks == 0);
    free(g_mem);
    printf("ipc_message: ok\n");
    return 0;
}
