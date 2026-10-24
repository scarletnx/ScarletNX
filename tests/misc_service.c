/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* mm:u and the smaller services (misc.c), hid's IHidServer (hid.c) and acc (acc.c) through their generated
   dispatchers, with the kernel objects they hand out made here and the shared memory a host buffer */
#include "nx/hle/services/services_int.h"
#include <math.h>
#include <stdarg.h>

#include "nx/hle/services/acc.c"
#include "nx/hle/services/users.c"
#include "nx/hle/services/hid.c"
#include "nx/hle/services/misc.c"
#include "nx/hle/hle.c"

u8* g_mem;
const u64 g_user_id[2] = {0x0123456789ABCDEFull, 0xFEDCBA9876543210ull};
char g_exedir[1024];
static unsigned test_checks, test_objects, test_kobjs;
static u32 test_next_handle = 0x100;
u64 plat_ns(void) {
    static u64 now;
    return now += 16666666ull;
}
u64 guest_ns(void) { return plat_ns(); } /* hid's motion steps: no pause here */

static void test_fail(const char* expression, int line) {
    fprintf(stderr, "misc_service.c:%d: check failed: %s\n", line, expression);
    exit(1);
}
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        test_checks++;                                                                                                 \
        if (!(expr)) test_fail(#expr, __LINE__);                                                                       \
    } while (0)

void pm_init(PMutex* m) { (void)m; }
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
/* the data folder: only the add-on images in test_aoc_installed, ids of system/archives/<id>.romfs */
static u64 test_aoc_installed[4];
FILE* fs_open_read(const char* path, u64* size) {
    char want[64];
    for (size_t i = 0; i < 4 && test_aoc_installed[i]; i++) {
        snprintf(want, sizeof want, "system/archives/%016llx.romfs", (unsigned long long)test_aoc_installed[i]);
        if (!strcmp(path, want)) {
            if (size) *size = 0;
            return tmpfile(); /* any open file, which /dev/null is not on Windows: the scan only closes it */
        }
    }
    return NULL;
}
/* the base ID the game's control.nacp numbers its DLCs from (am.c's), 0 for the SDK's default */
static u64 test_aoc_base;
u64 am_aoc_base_id(void) { return test_aoc_base ? test_aoc_base : snx_nacp_aoc_base(NULL, 0, g_hle.kernel.title_id); }
/* a DLC read in place from its dump (fs.c's), 0 for none */
static u64 test_aoc_dump;
int fs_aoc_has(u64 id) { return id && id == test_aoc_dump; }
void logf_(int level, const char* format, ...) {
    (void)level;
    (void)format;
}
void fatal(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    exit(1);
}

SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*)) {
    SvcObj* obj = calloc(1, sizeof *obj);
    CHECK(obj != NULL);
    obj->name = name;
    obj->dispatch = dispatch;
    obj->refs = 1;
    test_objects++;
    return obj;
}
#include "svcobj_data.h"
#include "kobj_ids.h" /* the kernel objects the stubs make, as ids */
void svcobj_unref(SvcObj* obj) {
    if (!obj || --obj->refs) return;
    if (obj->destroy) obj->destroy(obj);
    test_svcobj_data_free(obj);
    free(obj);
    test_objects--;
}
void out_obj(IpcCtx* c, SvcObj* obj) {
    CHECK(c->nout_objs < 8);
    c->out_objs[c->nout_objs++] = obj;
}
void out_copy_h(IpcCtx* c, u32 handle) {
    CHECK(c->nout_copy < 8);
    c->out_copy[c->nout_copy++] = handle;
}
void ipc_out_overflow(IpcCtx* c) {
    (void)c;
    test_fail("IPC output overflow", __LINE__);
}
/* sf's InvalidCmifRequest, for a request that doesn't send what its command declares */
int ipc_bad_request(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return RC(11, 420);
}
/* sf's InvalidNumInObjects, for a domain request with another number of objects */
int ipc_bad_objects(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return RC(10, 235);
}
int stub_ok(SvcObj* o, IpcCtx* c) {
    fprintf(stderr, "unexpected stub: %s command %u\n", o->name, c->cmd);
    test_fail("a command without a handler", __LINE__);
    return -1;
}
int quiet_ok(SvcObj* o, IpcCtx* c) { return stub_ok(o, c); }
void svc_register(const char* name, SvcCtor ctor) {
    (void)name;
    (void)ctor;
}

/* kernel objects: a handle takes a reference, as h_new does, and the test keeps count of what is alive */
void kobj_ref(KObj* o) { o->refs++; }
void kobj_unref(KObj* o) {
    CHECK(o->refs > 0);
    if (--o->refs) return;
    if (o->type == KO_SHMEM) free(((KShmem*)o)->host); /* as kshmem_destroy lets go of its pages */
    free(o);
    test_kobjs--;
}
KEvent* kevent_new(const char* name) {
    KEvent* e = calloc(1, sizeof *e);
    CHECK(e != NULL);
    e->o.type = KO_EVENT;
    kobj_set_name(&e->o, name);
    test_kobjs++;
    return e;
}
void kevent_signal(KEvent* e) { e->signaled = 1; }
KShmem* kshmem_new(u64 size, const char* name) {
    KShmem* s = calloc(1, sizeof *s);
    CHECK(s != NULL);
    s->o.type = KO_SHMEM;
    kobj_set_name(&s->o, name);
    s->size = size;
    s->host = calloc(1, size);
    CHECK(s->host != NULL);
    test_kobjs++;
    return s;
}
u8* kshmem_ptr(KShmem* s) { return s->host; }
/* the handle table: the handle a test reads back names the object it was made for */
static KObj* test_handles[64];
u32 h_new(KObj* o) {
    CHECK(test_next_handle - 0x100 < 64);
    kobj_ref(o);
    test_handles[test_next_handle - 0x100] = o;
    return test_next_handle++;
}
static KObj* handle_object(u32 h) {
    CHECK(h >= 0x100 && h < test_next_handle);
    return test_handles[h - 0x100];
}
static void close_handles(void) {
    for (u32 h = 0x100; h < test_next_handle; h++) kobj_unref(test_handles[h - 0x100]);
    test_next_handle = 0x100;
}
u32 ev_handle(u32 ev) { return h_new(kobj_at(ev)); }
int new_event(u32* event, const char* name, int signaled) {
    KEvent* e = kevent_new(name);
    e->signaled = signaled;
    *event = h_new(&e->o);
    return 0;
}
u32 owned_event(const char* name) {
    KEvent* e = kevent_new(name);
    kobj_ref(&e->o);
    return kobj_id(&e->o);
}

typedef struct {
    IpcCtx c;
    u8 raw[0x60];
} Request;
static IpcCtx* request(Request* r, u32 cmd) {
    memset(r, 0, sizeof *r);
    r->c.cmd = cmd;
    r->c.raw = r->raw;
    r->c.raw_size = sizeof r->raw;
    return &r->c;
}
static void argument32(Request* r, u32 off, u32 v) { snx_st_le32(r->raw + off, v); }
static void argument64(Request* r, u32 off, u64 v) { snx_st_le64(r->raw + off, v); }
static u32 reply32(Request* r, u32 off) {
    CHECK(off + 4 <= r->c.out_size);
    return snx_ld_le32(r->c.out + off); /* the dispatcher writes the reply as the guest reads it */
}
static void call(SvcObj* o, Request* r) { CHECK(o->dispatch(o, &r->c) == 0); }

/* mm:u: Initialize (4) {module, priority, event clear mode} gives a request id. SetAndWait (6) {id, min, max} sets
   its clock, which Get (7) {id} reads back: NvMM reads the decoder's clock back after asking for it */
static u32 mm_init(SvcObj* mm, u32 module) {
    Request r;
    request(&r, 4);
    argument32(&r, 0, module);
    argument32(&r, 4, 1);
    argument32(&r, 8, 1);
    call(mm, &r);
    CHECK(r.c.out_size == 4);
    return reply32(&r, 0);
}
static void mm_set_and_wait(SvcObj* mm, u32 cmd, u32 key, u32 min, u32 max) {
    Request r;
    request(&r, cmd);
    argument32(&r, 0, key);
    argument32(&r, 4, min);
    argument32(&r, 8, max);
    call(mm, &r);
    CHECK(r.c.out_size == 0);
}
static u32 mm_get_setting(SvcObj* mm, u32 cmd, u32 key) {
    Request r;
    request(&r, cmd);
    argument32(&r, 0, key);
    call(mm, &r);
    CHECK(r.c.out_size == 4);
    return reply32(&r, 0);
}
static void mm_finalize_key(SvcObj* mm, u32 cmd, u32 key) {
    Request r;
    request(&r, cmd);
    argument32(&r, 0, key);
    call(mm, &r);
    CHECK(r.c.out_size == 0);
}
static void test_mm(void) {
    SvcObj* mm = new_mm();
    u32 nvdec = mm_init(mm, 5), ram = mm_init(mm, 2);
    CHECK(nvdec && ram && nvdec != ram);
    CHECK(mm_get_setting(mm, 7, nvdec) == 0); /* nothing asked for yet */
    mm_set_and_wait(mm, 6, nvdec, 576000000, 716800000);
    mm_set_and_wait(mm, 6, ram, 1600000000, 1600000000);
    CHECK(mm_get_setting(mm, 7, nvdec) == 576000000); /* the minimum asked for */
    CHECK(mm_get_setting(mm, 7, ram) == 1600000000);
    mm_set_and_wait(mm, 6, nvdec, 268800000, 716800000);
    CHECK(mm_get_setting(mm, 7, nvdec) == 268800000 && mm_get_setting(mm, 7, ram) == 1600000000);
    mm_finalize_key(mm, 5, nvdec);
    CHECK(mm_get_setting(mm, 7, nvdec) == 0 && mm_get_setting(mm, 7, ram) == 1600000000);
    mm_set_and_wait(mm, 6, nvdec, 1, 2); /* a finalized request is gone */
    CHECK(mm_get_setting(mm, 7, nvdec) == 0);
    CHECK(mm_init(mm, 5) != nvdec); /* each Initialize is a new request */
    /* the old forms (0 to 3) name the module instead of an id */
    Request r;
    request(&r, 0);
    argument32(&r, 0, 8);
    argument32(&r, 4, 1);
    call(mm, &r);
    CHECK(r.c.out_size == 0);
    mm_set_and_wait(mm, 2, 8, 300000000, 400000000);
    CHECK(mm_get_setting(mm, 3, 8) == 300000000);
    mm_finalize_key(mm, 1, 8);
    CHECK(mm_get_setting(mm, 3, 8) == 0);
    svcobj_unref(mm);
}

/* hid IHidServer: SetSupportedNpadStyleSet (100) {u32 style set, aruid at 8}, GetSupportedNpadStyleSet (101) {aruid}
   and AcquireNpadStyleSetUpdateEventHandle (106) {u32 npad id, aruid at 8, u64 event pointer at 16} */
static u32 no1_style(void) {
    return snx_ld_le32(kshmem_ptr(kshm(g_hle.hid.shm)) + HO_HidSharedMemory_npad + HO_HidNpadInternalState_style_set);
}
static u32 npad_style_at(int id) {
    u8* e = kshmem_ptr(kshm(g_hle.hid.shm)) + HO_HidSharedMemory_npad + id * HS_HidNpadSharedMemoryEntry;
    return snx_ld_le32(e + HO_HidNpadInternalState_style_set);
}
static u64 npad_latest_buttons(int id, u32 lifo_offset) {
    u8* lifo =
        kshmem_ptr(kshm(g_hle.hid.shm)) + HO_HidSharedMemory_npad + id * HS_HidNpadSharedMemoryEntry + lifo_offset;
    u64 tail = snx_ld_le64(lifo + HO_HidCommonLifoHeader_tail);
    return snx_ld_le64(lifo + HO_HidNpadCommonLifo_storage + tail * HS_HidNpadCommonStateAtomicStorage + 8 +
                       HO_HidNpadCommonState_buttons);
}
static void check_npad_latest_sample(int id, u32 lifo_offset, u64 sample) {
    u8* lifo =
        kshmem_ptr(kshm(g_hle.hid.shm)) + HO_HidSharedMemory_npad + id * HS_HidNpadSharedMemoryEntry + lifo_offset;
    u64 tail = snx_ld_le64(lifo + HO_HidCommonLifoHeader_tail), stamp, state_sample;
    u8* entry = lifo + HO_HidNpadCommonLifo_storage + tail * HS_HidNpadCommonStateAtomicStorage;
    stamp = snx_ld_le64(entry);
    state_sample = snx_ld_le64(entry + HO_HidNpadCommonStateAtomicStorage_state);
    CHECK((stamp & 1) == 0 && stamp == sample << 1 && state_sample == sample);
}
static void npad_latest_sticks(int id, u32 lifo_offset, i32 sticks[4]) {
    u8* lifo =
        kshmem_ptr(kshm(g_hle.hid.shm)) + HO_HidSharedMemory_npad + id * HS_HidNpadSharedMemoryEntry + lifo_offset;
    u64 tail = snx_ld_le64(lifo + HO_HidCommonLifoHeader_tail);
    memcpy(sticks,
           lifo + HO_HidNpadCommonLifo_storage + tail * HS_HidNpadCommonStateAtomicStorage + 8 +
               HO_HidNpadCommonState_analog_stick_l,
           16);
    snx_le32_array(sticks, 4);
}
static float npad_latest_direction(int id, u32 lifo_offset, u32 direction_offset) {
    u8* lifo =
        kshmem_ptr(kshm(g_hle.hid.shm)) + HO_HidSharedMemory_npad + id * HS_HidNpadSharedMemoryEntry + lifo_offset;
    u64 tail = snx_ld_le64(lifo + HO_HidCommonLifoHeader_tail);
    u32 bits = snx_ld_le32(lifo + HO_HidNpadSixAxisSensorLifo_storage + tail * HS_HidSixAxisSensorStateAtomicStorage +
                           8 + HO_HidSixAxisSensorState_direction + direction_offset);
    float direction;
    memcpy(&direction, &bits, sizeof direction);
    return direction;
}
static void set_ids(SvcObj* h, const u32* ids, u32 n) {
    Request r;
    request(&r, 102);
    r.c.has_pid = 1;
    argument64(&r, 0, 0x55);
    memcpy(g_mem + 0x3000, ids, n * 4);
    snx_le32_array(g_mem + 0x3000, n);
    r.c.nx = 1;
    r.c.x[0].addr = 0x3000;
    r.c.x[0].size = n * 4;
    call(h, &r);
}
static void joy_assign(SvcObj* h, u32 cmd, u32 id, u32 type) {
    Request r;
    request(&r, cmd);
    r.c.has_pid = 1;
    argument32(&r, 0, id);
    argument64(&r, 8, 0x55);
    if (cmd == 123) argument64(&r, 16, type);
    call(h, &r);
}
static void joy_merge(SvcObj* h, u32 id1, u32 id2) {
    Request r;
    request(&r, 125);
    r.c.has_pid = 1;
    argument32(&r, 0, id1);
    argument32(&r, 4, id2);
    argument64(&r, 8, 0x55);
    call(h, &r);
}
static void joy_hold(SvcObj* h, u64 hold) {
    Request r;
    request(&r, 120);
    r.c.has_pid = 1;
    argument64(&r, 0, 0x55);
    argument64(&r, 8, hold);
    call(h, &r);
}
static u32 get_styles(SvcObj* h) {
    Request r;
    request(&r, 101);
    r.c.has_pid = 1; /* the aruid */
    argument64(&r, 0, 0x55);
    call(h, &r);
    CHECK(r.c.out_size == 4);
    return reply32(&r, 0);
}
static void set_styles(SvcObj* h, u32 styles) {
    Request r;
    request(&r, 100);
    r.c.has_pid = 1; /* the aruid */
    argument32(&r, 0, styles);
    argument64(&r, 8, 0x55);
    call(h, &r);
    CHECK(r.c.out_size == 0);
}
static KEvent* style_event(SvcObj* h, u32 id) {
    Request r;
    request(&r, 106);
    r.c.has_pid = 1; /* the aruid */
    argument32(&r, 0, id);
    argument64(&r, 8, 0x55);
    argument64(&r, 16, 0x1234560);
    call(h, &r);
    CHECK(r.c.out_size == 0 && r.c.nout_copy == 1 && !r.c.nout_move && !r.c.nout_objs);
    KObj* o = handle_object(r.c.out_copy[0]);
    CHECK(o->type == KO_EVENT);
    return (KEvent*)o;
}
static u8 six_axis_at_rest(SvcObj* h, u32 handle) {
    Request r;
    request(&r, 82);
    r.c.has_pid = 1;
    argument32(&r, 0, handle);
    argument64(&r, 8, 0x55);
    call(h, &r);
    CHECK(r.c.out_size == 4);
    return r.c.out[0];
}
/* SendVibrationValue (201) {u32 handle, VibrationValue at 4, aruid at 0x18} and SendVibrationValues (206) {aruid},
   with the handles and the values in two pointer buffers: what the frontend plays, per device */
static void vibration(SvcObj* h, u32 handle, float amp_low, float amp_high) {
    Request r;
    float v[4] = {amp_low, 160, amp_high, 320};
    request(&r, 201);
    r.c.has_pid = 1; /* the aruid */
    argument32(&r, 0, handle);
    snx_le32_array(v, 4);
    memcpy(r.raw + 4, v, sizeof v);
    argument64(&r, 0x18, 0x55);
    call(h, &r);
    CHECK(r.c.out_size == 0);
}
static void vibrations(SvcObj* h, const u32* handles, const float (*values)[4], u32 n) {
    Request r;
    request(&r, 206);
    argument64(&r, 0, 0x55);
    memcpy(g_mem + 0x1000, handles, n * 4);
    memcpy(g_mem + 0x2000, values, n * 16);
    snx_le32_array(g_mem + 0x1000, n);
    snx_le32_array(g_mem + 0x2000, n * 4);
    r.c.nx = 2;
    r.c.x[0].addr = 0x1000;
    r.c.x[0].size = n * 4;
    r.c.x[1].addr = 0x2000;
    r.c.x[1].size = n * 16;
    call(h, &r);
    CHECK(r.c.out_size == 0);
}
static void test_vibration(SvcObj* h) {
    /* No1's Pro Controller (style index 3), the left device then the right one */
    static const u32 both[2] = {0x00000003u, 0x00010003u};
    static const float values[2][4] = {{0.25f, 160, 0, 320}, {1.5f, 160, 0.5f, 320}};
    vibration(h, 0x00000003u, 0.5f, 1.0f);
    CHECK(g_hle.hid.pad1.rumble[0] == (32767u | 65535u << 16) && g_hle.hid.pad1.rumble[1] == 0);
    vibration(h, 0x00010020u, 0.0f, 0.25f); /* the handheld's right device */
    CHECK(g_hle.hid.pad1.rumble[1] == 16383u << 16);
    vibration(h, 0x00000003u, -1.0f, NAN); /* none past the ends, nor a NaN */
    CHECK(g_hle.hid.pad1.rumble[0] == 0);
    vibrations(h, both, values, 2);
    CHECK(g_hle.hid.pad1.rumble[0] == 16383u && g_hle.hid.pad1.rumble[1] == (65535u | 32767u << 16));
    vibrations(h, both, values, 0);
    CHECK(g_hle.hid.pad1.rumble[0] == 16383u);
}
static void test_npad_players(SvcObj* h) {
    static const u32 first_ids[] = {0, 1, 2, 3, 0x20};
    static const u32 fewer_ids[] = {0, 1, 3, 0x20};
    static const u32 all_ids[] = {0, 1, 2, 3, 4, 0x20};
    static const u32 no2_only[] = {1};
    u8* e1 = kshmem_ptr(kshm(g_hle.hid.shm)) + HO_HidSharedMemory_npad + HS_HidNpadSharedMemoryEntry;
    u32 assignment, selected_id;
    u64 props;
    i32 sticks[4];
    KEvent* no2 = style_event(h, 1);
    KEvent* no3 = style_event(h, 2);
    g_hle.hid.pad_extra[0].connected = 1;
    g_hle.hid.pad_extra[0].kind = SNX_PAD_JOY_LEFT;
    g_hle.hid.pad_extra[0].buttons = 1ull << 12;
    g_hle.hid.pad_extra[0].lx = 1000;
    g_hle.hid.pad_extra[1].connected = 1;
    g_hle.hid.pad_extra[1].kind = SNX_PAD_JOY_RIGHT;
    g_hle.hid.pad_extra[1].buttons = 1ull << 0;
    g_hle.hid.pad_extra[1].rx = 2000;
    g_hle.hid.pad_extra[2].connected = 1;
    g_hle.hid.pad_extra[2].kind = SNX_PAD_JOY_DUAL;
    g_hle.hid.pad_extra[2].buttons = (1ull << 12) | (1ull << 0);
    g_hle.hid.pad_extra[2].lx = 3000;
    g_hle.hid.pad_extra[2].rx = 4000;
    no2->signaled = no3->signaled = 0;
    hid_update();
    CHECK(npad_style_at(0) == 1 && npad_style_at(1) == 1u << 3 && npad_style_at(2) == 1u << 4 &&
          npad_style_at(3) == 1u << 2);
    /* ControllerSupport selects the first visible player even when No1 is excluded. */
    set_ids(h, no2_only, 1);
    selected_id = 0xFFFFFFFFu;
    CHECK(hid_connected_players(&selected_id) == 1 && selected_id == 1);
    set_ids(h, first_ids, 5);
    CHECK(no2->signaled && no3->signaled);
    CHECK(npad_latest_buttons(1, HO_HidNpadInternalState_joy_left_lifo) == 1ull << 12);
    CHECK(npad_latest_buttons(2, HO_HidNpadInternalState_joy_right_lifo) == 1ull);
    CHECK(npad_latest_buttons(1, HO_HidNpadInternalState_full_key_lifo) == 0);
    npad_latest_sticks(1, HO_HidNpadInternalState_joy_left_lifo, sticks);
    CHECK(sticks[0] == 0 && sticks[1] == -1000);
    npad_latest_sticks(2, HO_HidNpadInternalState_joy_right_lifo, sticks);
    CHECK(sticks[2] == 0 && sticks[3] == 2000);
    /* Replacing a physical controller in No2 resets its sensor orientation before reading the new sample. */
    g_hle.hid.pad_extra[0].source_id = 111;
    g_hle.hid.pad_extra[0].motion[0].valid = 1;
    g_hle.hid.pad_extra[0].motion[0].acceleration[1] = 1.0f;
    hid_update();
    CHECK(fabsf(npad_latest_direction(1, HO_HidNpadInternalState_joy_left_six_axis_sensor_lifo, 28) + 1.0f) < 0.0005f);
    g_hle.hid.pad_extra[0].source_id = 222;
    g_hle.hid.pad_extra[0].motion[0].acceleration[1] = 0.0f;
    g_hle.hid.pad_extra[0].motion[0].acceleration[2] = -1.0f;
    hid_update();
    CHECK(fabsf(npad_latest_direction(1, HO_HidNpadInternalState_joy_left_six_axis_sensor_lifo, 28)) < 0.0005f);
    g_hle.hid.pad_extra[0].motion[0].valid = 0;
    /* upright, a single Joy-Con's header says so (footer JoyLeftVertical, ABXY oriented); held sideways, the other */
    CHECK(e1[HO_HidNpadInternalState_applet_footer_ui_type] == 9);
    props = snx_ld_le64(e1 + HO_HidNpadInternalState_system_properties);
    CHECK((props & (3u << 11)) == 1u << 11 && (props & (1u << 14)));
    joy_hold(h, 1);
    hid_update();
    npad_latest_sticks(1, HO_HidNpadInternalState_joy_left_lifo, sticks);
    CHECK(sticks[0] == 1000 && sticks[1] == 0);
    CHECK(e1[HO_HidNpadInternalState_applet_footer_ui_type] == 8);
    props = snx_ld_le64(e1 + HO_HidNpadInternalState_system_properties);
    CHECK((props & (3u << 11)) == 1u << 12);
    joy_hold(h, 0);
    CHECK(e1[HO_HidNpadInternalState_applet_footer_ui_type] == 9);
    assignment = snx_ld_le32(e1 + HO_HidNpadInternalState_joy_assignment_mode);
    CHECK(assignment == 1);
    set_ids(h, fewer_ids, 4);
    CHECK(npad_style_at(2) == 0 && npad_style_at(1) == 1u << 3);
    set_ids(h, first_ids, 5);
    CHECK(npad_style_at(2) == 1u << 4);
    /* A combined host Joy-Con splits into two guest players, keeping each half's own buttons and motor. */
    joy_assign(h, 123, 3, 0);
    CHECK(npad_style_at(3) == 1u << 2 && npad_style_at(4) == 0); /* no permitted slot for the other half */
    set_ids(h, all_ids, 6);
    joy_assign(h, 124, 3, 0);
    joy_assign(h, 123, 3, 0);
    CHECK(npad_style_at(3) == 1u << 3 && npad_style_at(4) == 1u << 4);
    /* A new dual controller in the same host slot must not inherit the old one's split assignment. */
    g_hle.hid.pad_extra[2].source_id = 333;
    hid_update();
    CHECK(npad_style_at(3) == 1u << 2 && npad_style_at(4) == 0);
    joy_assign(h, 123, 3, 0);
    CHECK(npad_style_at(3) == 1u << 3 && npad_style_at(4) == 1u << 4);
    /* Removing the destination ID must restore the physical pair instead of hiding one half. */
    set_ids(h, first_ids, 5);
    CHECK(npad_style_at(3) == 1u << 2 && npad_style_at(4) == 0);
    set_ids(h, all_ids, 6);
    joy_assign(h, 123, 3, 0);
    CHECK(npad_style_at(3) == 1u << 3 && npad_style_at(4) == 1u << 4);
    hid_update();
    CHECK(npad_latest_buttons(3, HO_HidNpadInternalState_joy_left_lifo) == 1ull << 12);
    CHECK(npad_latest_buttons(4, HO_HidNpadInternalState_joy_right_lifo) == 1ull);
    vibration(h, 0x00010407u, 0.5f, 0); /* virtual No5's right device belongs to physical No4 */
    CHECK(g_hle.hid.pad_extra[2].rumble[1] == 32767u);
    joy_assign(h, 124, 3, 0);
    CHECK(npad_style_at(3) == 1u << 2 && npad_style_at(4) == 0);
    /* Two physical single Joy-Cons can become one JoyDual without mixing player buttons. */
    joy_merge(h, 1, 2);
    CHECK(npad_style_at(1) == 1u << 2 && npad_style_at(2) == 0);
    hid_update();
    CHECK(npad_latest_buttons(1, HO_HidNpadInternalState_joy_dual_lifo) == ((1ull << 12) | 1ull));
    npad_latest_sticks(1, HO_HidNpadInternalState_joy_dual_lifo, sticks);
    CHECK(sticks[0] == 0 && sticks[1] == -1000 && sticks[2] == 0 && sticks[3] == 2000);
    vibration(h, 0x00010105u, 0.25f, 0); /* merged No2's right device goes to physical No3 */
    CHECK(g_hle.hid.pad_extra[1].rumble[1] == 16383u);
    g_hle.hid.pad_extra[0].motion[0].valid = g_hle.hid.pad_extra[1].motion[0].valid = 1;
    g_hle.hid.pad_extra[0].motion[0].acceleration[2] = g_hle.hid.pad_extra[1].motion[0].acceleration[2] = -1;
    g_hle.hid.pad_extra[1].motion[0].angular_velocity[0] = 1;
    CHECK(six_axis_at_rest(h, 0x00000105u) == 1); /* left sensor */
    CHECK(six_axis_at_rest(h, 0x00010105u) == 0); /* right sensor */
    g_hle.hid.pad_extra[0].motion[0].valid = g_hle.hid.pad_extra[1].motion[0].valid = 0;
    /* A new right Joy-Con with the same kind must not inherit the old right Joy-Con's pair. */
    g_hle.hid.pad_extra[1].source_id = 444;
    hid_update();
    CHECK(npad_style_at(1) == 1u << 3 && npad_style_at(2) == 1u << 4);
    joy_merge(h, 1, 2);
    CHECK(npad_style_at(1) == 1u << 2 && npad_style_at(2) == 0);
    /* Reassigning one member to another pair releases the old hidden partner. */
    g_hle.hid.pad_extra[2].kind = SNX_PAD_JOY_RIGHT;
    hid_update();
    joy_merge(h, 1, 3);
    CHECK(npad_style_at(1) == 1u << 2 && npad_style_at(2) == 1u << 4 && npad_style_at(3) == 0);
    joy_merge(h, 1, 2);
    CHECK(npad_style_at(1) == 1u << 2 && npad_style_at(2) == 0 && npad_style_at(3) == 1u << 4);
    g_hle.hid.pad_extra[2].kind = SNX_PAD_JOY_DUAL;
    hid_update();
    /* A replacement in No3's physical slot must not inherit its old Joy-Con pairing. */
    g_hle.hid.pad_extra[1].kind = SNX_PAD_FULL_KEY;
    hid_update();
    CHECK(npad_style_at(1) == 1u << 3 && npad_style_at(2) == 1u);
    g_hle.hid.pad_extra[1].kind = SNX_PAD_JOY_RIGHT;
    hid_update();
    CHECK(npad_style_at(1) == 1u << 3 && npad_style_at(2) == 1u << 4);
    joy_merge(h, 1, 2);
    CHECK(npad_style_at(1) == 1u << 2 && npad_style_at(2) == 0);
    joy_assign(h, 122, 2, 0); /* the hidden secondary can release the pair too */
    CHECK(npad_style_at(1) == 1u << 3 && npad_style_at(2) == 1u << 4);
    joy_merge(h, 1, 2);
    CHECK(npad_style_at(1) == 1u << 2 && npad_style_at(2) == 0);
    joy_assign(h, 122, 1, 0); /* return merged Joy-Cons to their original Npad IDs */
    CHECK(npad_style_at(1) == 1u << 3 && npad_style_at(2) == 1u << 4);
    set_ids(h, first_ids, 5);
}
static void test_hid(void) {
    svc_hid_init();
    SvcObj* h = new_hid();
    for (int i = 0; i < 2; i++) {
        hid_update();
        check_npad_latest_sample(0, HO_HidNpadInternalState_full_key_lifo, g_hle.hid.samp);
    }
    CHECK(npad_style_at(8) == 1u << 1);
    g_settings.tv_mode = 1;
    hid_update();
    CHECK(npad_style_at(8) == 0);
    g_settings.tv_mode = 0;
    hid_update();
    CHECK(npad_style_at(8) == 1u << 1);
    /* the handheld Npad's full-key LIFO carries No1's state too */
    g_hle.hid.pad1.buttons = 1ull << 3;
    hid_update();
    CHECK(npad_latest_buttons(8, HO_HidNpadInternalState_handheld_lifo) == 1ull << 3);
    CHECK(npad_latest_buttons(8, HO_HidNpadInternalState_full_key_lifo) == 1ull << 3);
    g_hle.hid.pad1.buttons = 0;
    CHECK(get_styles(h) == 0x7F); /* every style until the game says which it takes */
    CHECK(no1_style() == 1);      /* No1 is a Pro Controller */
    KEvent* no1 = style_event(h, 0);
    CHECK(no1 == kev(g_hle.hid.style_ev[0]) && no1->signaled); /* signaled at first, so the game reads the styles */
    CHECK(style_event(h, 0x20) == kev(g_hle.hid.style_ev[8]) && style_event(h, 0x10) == kev(g_hle.hid.style_ev[9]));
    CHECK(style_event(h, 3) == kev(g_hle.hid.style_ev[3]));
    no1->signaled = 0;
    set_styles(h, (1u << 1) | (1u << 2)); /* handheld and dual Joy-Con */
    CHECK(get_styles(h) == 6);
    CHECK(no1_style() == 4 && no1->signaled); /* No1 changes to one style, dual Joy-Con, and says so */
    no1->signaled = 0;
    set_styles(h, 1u << 1); /* handheld only: No1 has no style it can take */
    CHECK(get_styles(h) == 2 && no1_style() == 0 && no1->signaled);
    set_styles(h, 0x7F);
    CHECK(get_styles(h) == 0x7F && no1_style() == 1);
    /* Keyboard and scripted No1 input remains usable when slot 0 is an unsupported single Joy-Con. */
    g_hle.hid.pad1.kind = SNX_PAD_JOY_LEFT;
    g_hle.hid.pad1.buttons = 1ull;
    set_styles(h, 1u);
    hid_update();
    CHECK(no1_style() == 1 && npad_latest_buttons(0, HO_HidNpadInternalState_full_key_lifo) == 1ull);
    g_hle.hid.pad1.kind = SNX_PAD_FULL_KEY;
    g_hle.hid.pad1.buttons = 0;
    set_styles(h, 0x7F);
    test_vibration(h);
    test_npad_players(h);
    svcobj_unref(h);
    close_handles();
    /* the service's own references: everything it made goes with them */
    kobj_unref(&kshm(g_hle.hid.shm)->o);
    kobj_unref(&kshm(g_hle.hid.xpad_shm)->o);
    kobj_unref(&kshm(g_hle.hid.six_shm)->o);
    kobj_unref(&kshm(g_hle.hid.joysix_shm)->o);
    for (int i = 0; i < 10; i++) kobj_unref(&kev(g_hle.hid.style_ev[i])->o);
}

/* ngc:u's Check (1) and Mask (2) {languages, options} of a text, and ngct:u's Match (0) and Filter (1): nothing found
   or masked, the text back as sent and no byte past the room for it; GetContentVersion (0) 1 */
typedef struct {
    IpcCtx c;
    u8 raw[0x40];
} NgcRequest;
static IpcCtx* ngc_request(NgcRequest* r, u32 cmd) {
    memset(r, 0, sizeof *r);
    r->c.cmd = cmd;
    r->c.raw = r->raw;
    r->c.raw_size = sizeof r->raw;
    return &r->c;
}
static void test_ngc(void) {
    static const char text[] = "edenprobe player";
    SvcObj* ngc = new_ngc();
    SvcObj* ngct = new_ngct();
    NgcRequest r;
    u32 cmd, v;
    memcpy(g_mem + 0x100, text, sizeof text);
    ngc_request(&r, 0);
    CHECK(ngc->dispatch(ngc, &r.c) == 0 && r.c.out_size == 4);
    v = snx_ld_le32(r.c.out);
    CHECK(v == 1);
    for (cmd = 1; cmd <= 5; cmd++) {
        if (cmd == 3) continue;
        ngc_request(&r, cmd);
        snx_st_le32(r.raw, 2); /* American English */
        r.c.na = 1;
        r.c.a[0].addr = 0x100;
        r.c.a[0].size = sizeof text;
        if (cmd == 2 || cmd == 5) {
            memset(g_mem + 0x200, 0xEE, 0x40);
            r.c.nb = 1;
            r.c.b[0].addr = 0x200;
            r.c.b[0].size = sizeof text - 4; /* room for less than the text */
        }
        CHECK(ngc->dispatch(ngc, &r.c) == 0 && r.c.out_size == 4);
        v = snx_ld_le32(r.c.out);
        CHECK(v == 0);
        if (cmd == 2 || cmd == 5)
            CHECK(!memcmp(g_mem + 0x200, text, sizeof text - 4) && g_mem[0x200 + sizeof text - 4] == 0xEE);
    }
    ngc_request(&r, 0);
    r.c.nx = 1;
    r.c.x[0].addr = 0x100;
    r.c.x[0].size = sizeof text;
    CHECK(ngct->dispatch(ngct, &r.c) == 0 && r.c.out_size == 4 && r.c.out[0] == 0);
    ngc_request(&r, 1);
    memset(g_mem + 0x200, 0xEE, 0x40);
    r.c.nx = 1;
    r.c.x[0].addr = 0x100;
    r.c.x[0].size = sizeof text;
    r.c.nc = 1;
    r.c.c[0].addr = 0x200;
    r.c.c[0].size = 0x40;
    CHECK(ngct->dispatch(ngct, &r.c) == 0 && !memcmp(g_mem + 0x200, text, sizeof text) &&
          g_mem[0x200 + sizeof text] == 0xEE);
    svcobj_unref(ngct);
    svcobj_unref(ngc);
}

/* apm: Get (1) and Set (0) {mode, configuration} on a session, one configuration per mode, Config7 for both at
   first; one that is not a configuration, or a mode past Boost, kept out; SetCpuBoostMode's modes for Boost */
static u32 apm_get(SvcObj* s, u32 mode) {
    Request r;
    request(&r, 1);
    argument32(&r, 0, mode);
    call(s, &r);
    CHECK(r.c.out_size == 4);
    return reply32(&r, 0);
}
static void apm_set(SvcObj* s, u32 mode, u32 config) {
    Request r;
    request(&r, 0);
    argument32(&r, 0, mode);
    argument32(&r, 4, config);
    call(s, &r);
    CHECK(r.c.out_size == 0);
}
static void test_apm(void) {
    SvcObj* apm = new_apm();
    Request r;
    request(&r, 0);
    call(apm, &r);
    CHECK(r.c.nout_objs == 1);
    SvcObj* s = r.c.out_objs[0];
    CHECK(apm_get(s, 0) == 0x00020003u && apm_get(s, 1) == 0x00020003u);
    apm_set(s, 0, 0x00010000u);
    CHECK(apm_get(s, 0) == 0x00010000u && apm_get(s, 1) == 0x00020003u);
    apm_set(s, 0, 0x12345678u);
    apm_set(s, 2, 0x00010001u);
    CHECK(apm_get(s, 0) == 0x00010000u && apm_get(s, 1) == 0x00020003u);
    apm_cpu_boost(1);
    CHECK(apm_get(s, 1) == 0x92220009u && apm_get(s, 0) == 0x00010000u);
    apm_cpu_boost(2);
    CHECK(apm_get(s, 1) == 0x9222000Bu);
    apm_cpu_boost(3);
    CHECK(apm_get(s, 1) == 0x9222000Bu);
    apm_cpu_boost(0);
    apm_set(s, 0, 0x00020003u);
    CHECK(apm_get(s, 1) == 0x00020003u && apm_get(s, 0) == 0x00020003u);
    svcobj_unref(s);
    svcobj_unref(apm);
}

/* StoreSaveDataThumbnail (110) {uid}: a thumbnail of 0x24000 bytes for the user is taken; one of another size is
   InvalidArrayLength, one for a user id of nought InvalidUserId */
static void test_acc_thumbnail(void) {
    static const u8 nobody[16];
    nn_account_Uid uid;
    IpcBuf image = {0x1000, 0x24000, 0};
    acc_uid_put(uid.v, g_user_id);
    CHECK(nn_account_IAccountServiceForApplication_StoreSaveDataThumbnail(NULL, &uid, image) == 0);
    image.size = 0x1000;
    CHECK(nn_account_IAccountServiceForApplication_StoreSaveDataThumbnail(NULL, &uid, image) == RC(124, 32));
    image.size = 0x24000;
    memcpy(uid.v, nobody, 16);
    CHECK(nn_account_IAccountServiceForApplication_StoreSaveDataThumbnail(NULL, &uid, image) == RC(124, 20));
}

static void test_mii_default(void) {
    SvcObj* service = new_mii();
    Request r;
    request(&r, 0);
    argument32(&r, 0, 0);
    call(service, &r);
    CHECK(r.c.nout_objs == 1);
    SvcObj* db = r.c.out_objs[0];
    request(&r, 22);
    argument32(&r, 0, 1);
    call(db, &r);
    CHECK(r.c.out_size == 0);
    request(&r, 7);
    argument32(&r, 0, 0);
    call(db, &r);
    CHECK(r.c.out_size == 0x58);
    CHECK(r.c.out[0] != 0 && r.c.out[0x10] == 'n' && r.c.out[0x12] == 'o');
    CHECK(r.c.out[0x29] == 64 && r.c.out[0x2a] == 64);
    CHECK(r.c.out[0x31] == 68 && r.c.out[0x34] == 2 && r.c.out[0x27] == 4);
    request(&r, 7);
    argument32(&r, 0, 3);
    call(db, &r);
    CHECK(r.c.out[0x28] == 1 && r.c.out[0x31] == 24);
    request(&r, 7);
    argument32(&r, 0, 6);
    CHECK(db->dispatch(db, &r.c) == RC(126, 1));
    /* the six defaults counted and given by Get, each BuildDefault's build past its random create id */
    request(&r, 2);
    argument32(&r, 0, 2);
    call(db, &r);
    CHECK(reply32(&r, 0) == 6);
    request(&r, 2);
    argument32(&r, 0, 1);
    call(db, &r);
    CHECK(reply32(&r, 0) == 0);
    memset(g_mem + 0x4000, 0xee, 8 * 0x5c);
    request(&r, 3);
    argument32(&r, 0, 3);
    r.c.nb = 1;
    r.c.b[0].addr = 0x4000;
    r.c.b[0].size = 8 * 0x5c;
    call(db, &r);
    CHECK(reply32(&r, 0) == 6);
    for (u32 i = 0; i < 6; i++) {
        u8 built[0x58];
        u32 source;
        request(&r, 7);
        argument32(&r, 0, i);
        call(db, &r);
        memcpy(built, r.c.out, sizeof built);
        CHECK(memcmp(g_mem + 0x4000 + i * 0x5c + 0x10, built + 0x10, 0x48) == 0);
        source = gr32(0x4000 + i * 0x5c + 0x58);
        CHECK(source == 1);
    }
    CHECK(g_mem[0x4000 + 6 * 0x5c] == 0xee);
    request(&r, 4);
    argument32(&r, 0, 2);
    r.c.nb = 1;
    r.c.b[0].addr = 0x5000;
    r.c.b[0].size = 6 * 0x58;
    call(db, &r);
    CHECK(reply32(&r, 0) == 6 && memcmp(g_mem + 0x5000 + 0x58 + 0x10, g_mem + 0x4000 + 0x5c + 0x10, 0x48) == 0);
    request(&r, 3);
    argument32(&r, 0, 2);
    r.c.nb = 1;
    r.c.b[0].addr = 0x4000;
    r.c.b[0].size = 5 * 0x5c;
    CHECK(db->dispatch(db, &r.c) == RC(126, 2));
    request(&r, 1);
    call(db, &r);
    CHECK(reply32(&r, 0) == 0);
    /* the bytes BuildDefault does not set are nought, whatever the CharInfo held */
    nn_mii_CharInfo poisoned;
    memset(&poisoned, 0xee, sizeof poisoned);
    CHECK(nn_mii_detail_IDatabaseService_BuildDefault(NULL, 0, &poisoned) == 0);
    CHECK(poisoned.v[0x11] == 0 && poisoned.v[0x1e] == 0 && poisoned.v[0x25] == 0);
    svcobj_unref(db);
    svcobj_unref(service);
}

/* mm:u past its 32 requests: the id is still the client's, and reads back no clock */
static void test_mm_full(void) {
    SvcObj* mm = new_mm();
    u32 ids[33], i;
    for (i = 0; i < 33; i++) ids[i] = mm_init(mm, 5);
    CHECK(ids[32] && ids[32] != ids[31]);
    mm_set_and_wait(mm, 6, ids[32], 1, 2);
    CHECK(mm_get_setting(mm, 7, ids[32]) == 0);
    mm_set_and_wait(mm, 6, ids[31], 3, 4);
    CHECK(mm_get_setting(mm, 7, ids[31]) == 3);
    svcobj_unref(mm);
}

/* nfp and nfc with no NFC device: the state follows Initialize and Finalize, the availability event is one per
   IUser, and the device commands fail */
static void test_nfp(void) {
    SvcObj *manager = new_nfp(), *user = NULL, *nfc_manager = new_nfc(), *nfc = NULL;
    IpcBuf none = {0, 0, 0};
    nn_nfc_DeviceHandle device;
    u32 state = 9, a, b;
    i32 count;
    memset(&device, 0, sizeof device);
    CHECK(nn_nfp_detail_IUserManager_CreateUserInterface(manager, &user) == 0 && user);
    CHECK(nn_nfp_detail_IUser_GetState(user, &state) == 0 && state == 0);
    CHECK(nn_nfp_detail_IUser_ListDevices(user, none, &count) == RC(115, 80));
    CHECK(nn_nfp_detail_IUser_Initialize(user, 0, 0, none) == 0);
    CHECK(nn_nfp_detail_IUser_GetState(user, &state) == 0 && state == 1);
    CHECK(nn_nfp_detail_IUser_ListDevices(user, none, &count) == RC(115, 64));
    CHECK(nn_nfp_detail_IUser_StartDetection(user, &device) == RC(115, 64));
    CHECK(nn_nfp_detail_IUser_GetDeviceState(user, &device, &state) == 0 && state == 6);
    CHECK(nn_nfp_detail_IUser_AttachAvailabilityChangeEvent(user, &a) == 0);
    CHECK(nn_nfp_detail_IUser_AttachAvailabilityChangeEvent(user, &b) == 0);
    CHECK(handle_object(a) == handle_object(b) && !((KEvent*)handle_object(a))->signaled);
    CHECK(nn_nfp_detail_IUser_Finalize(user) == 0);
    CHECK(nn_nfp_detail_IUser_GetState(user, &state) == 0 && state == 0);
    CHECK(nn_nfc_detail_IUserManager_CreateUserInterface(nfc_manager, &nfc) == 0 && nfc);
    CHECK(nn_nfc_detail_IUser_ListDevices(nfc, none, &count) == RC(127, 77));
    CHECK(nn_nfc_detail_IUser_InitializeOld(nfc, 0, 0, none) == 0);
    CHECK(nn_nfc_detail_IUser_GetState(nfc, &state) == 0 && state == 1);
    CHECK(nn_nfc_detail_IUser_ListDevices(nfc, none, &count) == RC(127, 64));
    CHECK(nn_nfc_detail_IUser_AttachAvailabilityChangeEvent(nfc, &a) == 0);
    CHECK(nn_nfc_detail_IUser_AttachAvailabilityChangeEvent(nfc, &b) == 0 && handle_object(a) == handle_object(b));
    close_handles();
    svcobj_unref(nfc);
    svcobj_unref(nfc_manager);
    svcobj_unref(user);
    svcobj_unref(manager);
}

/* acc's TrySelectUserWithoutInteraction: 51, and 52, which a game built for 19.0.0 sends instead, both pick the one
   user, as users.c has it without users.ini {is_network_service_account_required} */
static void test_acc_select(void) {
    SvcObj* acc = new_acc();
    Request r;
    char why[256];
    CHECK(snx_users_load("no-users-here", 1, why, sizeof why) == 0 && g_user_count == 1);
    for (u32 cmd = 51; cmd <= 52; cmd++) {
        request(&r, cmd);
        call(acc, &r);
        u8 uid[16];
        acc_uid_put(uid, g_user_id);
        CHECK(r.c.out_size == 16 && !memcmp(r.c.out, uid, 16));
    }
    svcobj_unref(acc);
}

/* nfp's StartDetectionWithFilter (25, 23.0.0+) answers what StartDetection (3) does for the same device, before and
   after Initialize {aruid, pid, buffer}; the filter word after the device is not read */
static void test_nfp_filter(void) {
    SvcObj* manager = new_nfp();
    Request r;
    request(&r, 0);
    call(manager, &r);
    CHECK(r.c.nout_objs == 1);
    SvcObj* user = r.c.out_objs[0];
    for (int initialized = 0; initialized < 2; initialized++) {
        if (initialized) {
            request(&r, 0);
            r.c.has_pid = 1;
            r.c.na = 1;
            call(user, &r);
        }
        int answers[2];
        for (int i = 0; i < 2; i++) {
            request(&r, i ? 25 : 3);
            argument32(&r, 0, 0x20);
            argument32(&r, 8, 0xFFFFFFFFu);
            answers[i] = user->dispatch(user, &r.c);
        }
        CHECK(answers[0] == answers[1] && answers[0] == (initialized ? NFP_DEVICE_NOT_FOUND : NFP_NFC_DISABLED));
    }
    svcobj_unref(user);
    svcobj_unref(manager);
}

/* aoc:u's ListAddOnContent (3) {offset, count, pid} with no add-on content: from the end (0) nothing, from past it
   (1, or a negative offset, which the console reads unsigned) am's ApplicationRecordNotFound */
static void test_aoc_list(void) {
    static const u32 offsets[] = {0, 1, 0xFFFFFFFFu};
    SvcObj* aoc = new_aoc();
    Request r;
    for (u32 i = 0; i < 3; i++) {
        request(&r, 3);
        argument32(&r, 0, offsets[i]);
        argument32(&r, 4, 16);
        r.c.has_pid = 1;
        r.c.nb = 1;
        if (i) {
            CHECK(aoc->dispatch(aoc, &r.c) == RC(128, 37));
            continue;
        }
        call(aoc, &r);
        CHECK(r.c.out_size == 4 && reply32(&r, 0) == 0);
    }
    svcobj_unref(aoc);
}

/* aoc:u with the add-on images 1 and 3 in the data folder: CountAddOnContent (2) and the game's own
   CountAddOnContentByApplicationId (0) say 2, another application's 0; ListAddOnContent (3) and
   ListAddOnContentByApplicationId (1) {offset, count, ...} give the indices from offset, as many as count and the
   buffer hold, nothing from the end and ApplicationRecordNotFound past it; PrepareAddOnContent (7) succeeds */
static void test_aoc_content(void) {
    static const struct {
        u32 cmd, offset, count, room, rc, n, first;
    } lists[] = {
        {3, 0, 16, 16, 0, 2, 1}, {3, 1, 16, 16, 0, 1, 3},           {3, 0, 1, 16, 0, 1, 1},
        {3, 0, 16, 1, 0, 1, 1},  {3, 2, 16, 16, 0, 0, 0},           {3, 3, 16, 16, RC(128, 37), 0, 0},
        {1, 0, 16, 16, 0, 2, 1}, {1, 3, 16, 16, RC(128, 37), 0, 0},
    };
    const u64 base = aoc_base_id(g_hle.kernel.title_id), other = 0x0100000000020000ull;
    SvcObj* aoc = new_aoc();
    Request r;
    u32 v;
    test_aoc_installed[0] = base + 1;
    test_aoc_installed[1] = base + 3;
    g_hle.misc.aoc_scanned = g_hle.misc.aoc_count = 0; /* test_aoc_list scanned an empty folder */
    request(&r, 2);
    r.c.has_pid = 1;
    call(aoc, &r);
    CHECK(reply32(&r, 0) == 2);
    for (u32 i = 0; i < 2; i++) {
        request(&r, 0);
        argument64(&r, 0, i ? other : g_hle.kernel.title_id);
        call(aoc, &r);
        CHECK(reply32(&r, 0) == (i ? 0u : 2u));
    }
    for (u32 i = 0; i < sizeof lists / sizeof lists[0]; i++) {
        request(&r, lists[i].cmd);
        argument32(&r, 0, lists[i].offset);
        argument32(&r, 4, lists[i].count);
        if (lists[i].cmd == 1) argument64(&r, 8, g_hle.kernel.title_id);
        r.c.has_pid = lists[i].cmd == 3;
        r.c.nb = 1;
        r.c.b[0].addr = 0x4000;
        r.c.b[0].size = lists[i].room * 4;
        memset(g_mem + 0x4000, 0xAA, 64);
        CHECK((u32)aoc->dispatch(aoc, &r.c) == lists[i].rc);
        if (lists[i].rc) continue;
        CHECK(reply32(&r, 0) == lists[i].n);
        v = gr32(0x4000);
        CHECK(!lists[i].n || v == lists[i].first);
    }
    request(&r, 7);
    argument32(&r, 0, 1);
    r.c.has_pid = 1;
    call(aoc, &r);
    /* a DLC read in place from its dump counts as installed, beside one in system/archives */
    test_aoc_installed[1] = 0;
    test_aoc_dump = base + 2;
    g_hle.misc.aoc_scanned = g_hle.misc.aoc_count = 0;
    request(&r, 2);
    r.c.has_pid = 1;
    call(aoc, &r);
    CHECK(reply32(&r, 0) == 2);
    request(&r, 3);
    argument32(&r, 0, 1);
    argument32(&r, 4, 1);
    r.c.has_pid = 1;
    r.c.nb = 1;
    r.c.b[0].addr = 0x4000;
    r.c.b[0].size = 4;
    call(aoc, &r);
    v = gr32(0x4000);
    CHECK(reply32(&r, 0) == 1 && v == 2);
    test_aoc_dump = 0;
    /* a game whose control.nacp numbers its DLCs from a base ID of its own, not its ID's 0x1000: its DLCs counted from
       that base, and that base given */
    test_aoc_base = 0x0100ABCD5555A000ull;
    test_aoc_installed[0] = test_aoc_base + 1;
    g_hle.misc.aoc_scanned = g_hle.misc.aoc_count = 0;
    request(&r, 2);
    r.c.has_pid = 1;
    call(aoc, &r);
    CHECK(reply32(&r, 0) == 1);
    request(&r, 5);
    r.c.has_pid = 1;
    call(aoc, &r);
    {
        u64 got;
        got = snx_ld_le64(r.c.out);
        CHECK(got == test_aoc_base);
    }
    test_aoc_base = 0;
    svcobj_unref(aoc);
    memset(test_aoc_installed, 0, sizeof test_aoc_installed);
    g_hle.misc.aoc_scanned = g_hle.misc.aoc_count = 0;
}

/* nn::mii::detail::CharInfoRaw::Verify as the SDK checks a CharInfo (a game's SDK, 9c068c8): a create id
   not all zeros with the RFC 4122 variant, a nickname of 1 to 10 characters ending in zeros, and each field in its
   range. A game builds no Mii from a CharInfo that fails it */
static int mii_valid(const u8* c) {
    static const u8 max[49] = {3, 11, 1,  0x7f, 0x7f, 1,  3,  11, 9,  11, 11, 131, 99, 1,  59, 99, 7,
                               6, 7,  12, 18,   23,   99, 8,  6,  11, 12, 15, 17,  8,  18, 35, 99, 8,
                               6, 18, 99, 5,    5,    8,  16, 19, 99, 7,  20, 1,   8,  16, 31};
    int i, zero = 1, end = 0;
    for (i = 0; i < 16; i++)
        if (c[i]) zero = 0;
    if (zero || (c[8] & 0xc0) != 0x80) return 0;
    if (!c[0x10] && !c[0x11]) return 0;
    for (i = 0; i < 11; i++) {
        int ch = c[0x10 + 2 * i] | c[0x11 + 2 * i] << 8;
        if (!ch)
            end = 1;
        else if (end || i == 10)
            return 0;
    }
    for (i = 0; i < 49; i++)
        if (c[0x26 + i] > max[i]) return 0;
    return 1;
}

/* BuildRandom (6): a valid Mii every time, of the age, gender and race asked (or any for All, 3, 2, 3), laid out as
   Eden's CoreData::BuildRandom and CharInfo::SetFromStoreData make it. A game may ask for 13 at its title, and
   build its Mii characters from them */
static void test_mii_random(void) {
    SvcObj* service = new_mii();
    Request r;
    u8 last[16] = {0};
    int differ = 0, female = 0, male = 0;
    request(&r, 0);
    call(service, &r);
    SvcObj* db = r.c.out_objs[0];
    for (u32 age = 0; age <= 3; age++)
        for (u32 gender = 0; gender <= 2; gender++)
            for (u32 race = 0; race <= 3; race++)
                for (int n = 0; n < 40; n++) {
                    request(&r, 6);
                    argument32(&r, 0, age);
                    argument32(&r, 4, gender);
                    argument32(&r, 8, race);
                    call(db, &r);
                    const u8* c = r.c.out;
                    CHECK(r.c.out_size == 0x58 && mii_valid(c));
                    if (gender < 2) CHECK(c[0x28] == gender);
                    female += c[0x28] == 1, male += c[0x28] == 0;
                    u32 axis = c[0x3a] - 12u; /* the eyes' height is the face's offset plus 12 */
                    CHECK(axis <= 3 && (axis == 0 || (c[0x28] == 1 && age != 1 && age != 2)));
                    if (race < 3) CHECK(c[0x41] == axis + (race == 2 ? 6 : 7) + 3); /* eyebrows: CoreData's + 3 */
                    CHECK(c[0x43] == (c[0x28] ? 3 : 4) && c[0x44] == axis + 9);     /* the nose */
                    CHECK(c[0x3c] == c[0x32] && c[0x4a] == c[0x32]); /* eyebrows and beard the hair's color */
                    CHECK(c[0x50] == 8 && c[0x56] == 20 && c[0x29] == 64 && c[0x2a] == 64);
                    if (c[0x28] == 1) CHECK(c[0x4b] == 0 && c[0x4c] == 0); /* no beard nor mustache */
                    if (memcmp(last, c, 16)) differ++;
                    memcpy(last, c, 16);
                }
    CHECK(differ == 4 * 3 * 4 * 40 && female > 300 && male > 300);
    /* an age, gender or race past All is InvalidArgument */
    request(&r, 6);
    argument32(&r, 0, 4);
    CHECK(db->dispatch(db, &r.c) == RC(126, 1));
    argument32(&r, 0, 0);
    argument32(&r, 4, 3);
    CHECK(db->dispatch(db, &r.c) == RC(126, 1));
    argument32(&r, 4, 0);
    argument32(&r, 8, 4);
    CHECK(db->dispatch(db, &r.c) == RC(126, 1));
    svcobj_unref(db);
    svcobj_unref(service);
}

/* UpdateLatest (5) and GetIndex (21) of a Mii: NotFound, whatever the source, as the empty database holds none. A game
   that asks UpdateLatest for the Mii its save holds at boot got an empty CharInfo from a success, built no Mii from it,
   and crashed */
static void test_mii_latest(void) {
    SvcObj* service = new_mii();
    Request r;
    u8 mii[0x58];
    request(&r, 0);
    call(service, &r);
    SvcObj* db = r.c.out_objs[0];
    request(&r, 7);
    argument32(&r, 0, 0);
    call(db, &r);
    memcpy(mii, r.c.out, sizeof mii);
    CHECK(mii_valid(mii));
    for (u32 source = 0; source <= 3; source++) {
        request(&r, 5);
        memcpy(r.raw, mii, sizeof mii);
        argument32(&r, 0x58, source);
        CHECK(db->dispatch(db, &r.c) == RC(126, 4));
        CHECK(r.c.out_size == 0);
    }
    request(&r, 21);
    memcpy(r.raw, mii, sizeof mii);
    CHECK(db->dispatch(db, &r.c) == RC(126, 4));
    CHECK(r.c.out_size == 0);
    svcobj_unref(db);
    svcobj_unref(service);
}


int main(void) {
    g_settings.tv_mode = 0; /* handheld, as the test was written for */
    hle_init();
    g_hle.kernel.title_id = 0x0100BF00112C0000ull; /* the title the test was written for */
    g_mem = calloc(1, 0x10000);
    CHECK(g_mem != NULL);
    test_mm();
    test_hid();
    test_mii_default();
    test_acc_thumbnail();
    test_apm();
    test_ngc();
    test_mm_full();
    test_nfp();
    test_acc_select();
    test_nfp_filter();
    test_aoc_list();
    test_aoc_content();
    test_mii_random();
    test_mii_latest();
    CHECK(test_objects == 0 && test_kobjs == 0);
    free(g_mem);
    printf("misc_service: %u checks passed\n", test_checks);
    return 0;
}
