/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Inline keyboard lifecycle and fixed-size SDK replies, with a deterministic host dialog. */
#include "nx/hle/services/services_int.h"
#include <math.h>
#include <stdarg.h>

#include "nx/hle/services/am.c"
#include "nx/hle/hle.c"

u8* g_mem;
static unsigned test_checks, test_objects, test_kobjs;
static u32 test_next_handle = 0x100;
u64 plat_ns(void) {
    static u64 now;
    return now += 16666666ull;
}

static void test_fail(const char* expression, int line) {
    fprintf(stderr, "am_service.c:%d: check failed: %s\n", line, expression);
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
static void close_handles(void) {
    for (u32 h = 0x100; h < test_next_handle; h++) kobj_unref(test_handles[h - 0x100]);
    test_next_handle = 0x100;
}
u32 ev_handle(KEvent* e) { return h_new(&e->o); }
int new_event(u32* event, const char* name, int signaled) {
    KEvent* e = kevent_new(name);
    e->signaled = signaled;
    *event = h_new(&e->o);
    return 0;
}
KEvent* owned_event(const char* name) {
    KEvent* e = kevent_new(name);
    kobj_ref(&e->o);
    return e;
}

void svcobj_ref(SvcObj* o) { o->refs++; }
SvcObj* ipc_in_obj(IpcCtx* c, int index) {
    (void)c;
    (void)index;
    return NULL;
}
void kevent_clear(KEvent* e) { e->signaled = 0; }
KObj* h_get_ref(u32 h, int type) {
    (void)h;
    (void)type;
    return NULL;
}
volatile int g_quit;
void set_language_code(int lang, u8 code[8]) {
    (void)lang;
    memcpy(code, "en-US", 6);
}
static int test_players = 1;
int hid_connected_players(u32* selected_id) {
    *selected_id = 0;
    return test_players;
}
static const char* dialog_text = "Aé😀";
static const char* dialog_then; /* what the dialog answers from its second time on, when set */
static int dialog_cancel, dialog_calls;
static int modern_packet;
int plat_text_input(char* out, size_t cap, const char* initial) {
    (void)initial;
    snprintf(out, cap, "%s", dialog_calls++ && dialog_then ? dialog_then : dialog_text);
    return dialog_cancel;
}
/* a storage's numbers, little-endian as the game has them */
static void put32(u8* dst, u32 v) { snx_st_le32(dst, v); }
static u32 get32(const u8* src) { return snx_ld_le32(src); }
static void send_request(SvcObj* a, u32 request, u64 flags, int utf8) {
    u8 packet[0x4EC] = {0};
    SvcObj* storage;
    put32(packet, request);
    snx_st_le64(packet + 12, flags);
    put32(packet + 4 + (modern_packet ? 0x38 : 0x40), 20);
    packet[4 + (modern_packet ? 0x484 : 0x45C)] = (u8)utf8;
    storage = am_storage_new(modern_packet ? sizeof packet : 0x4A4, packet);
    CHECK(nn_am_service_ILibraryAppletAccessor_PushInteractiveInData(a, storage) == 0);
    svcobj_unref(storage);
}
static SvcObj* reply(SvcObj* a, u32 state, u32 type, u32 size) {
    SvcObj* storage = NULL;
    StorageData* data;
    CHECK(nn_am_service_ILibraryAppletAccessor_PopInteractiveOutData(a, &storage) == 0);
    data = storage->data;
    CHECK(data->size == size);
    CHECK(get32(data->d) == state);
    CHECK(get32(data->d + 4) == type);
    return storage;
}
/* IAudioController: the expected master volumes read back as the game set them, from 0 to 1, on the controller it set
   them on */
static void test_audio_controller(void) {
    SvcObj *c, *other;
    float main_volume = -1.0f, library_volume = -1.0f;
    CHECK(nn_am_service_IApplicationProxy_GetAudioController(NULL, &c) == 0);
    CHECK(nn_am_service_IAudioController_GetMainAppletExpectedMasterVolume(c, &main_volume) == 0 &&
          main_volume == 1.0f);
    CHECK(nn_am_service_IAudioController_SetExpectedMasterVolume(c, 0.5f, 0.25f) == 0);
    CHECK(nn_am_service_IAudioController_GetMainAppletExpectedMasterVolume(c, &main_volume) == 0 &&
          main_volume == 0.5f);
    CHECK(nn_am_service_IAudioController_GetLibraryAppletExpectedMasterVolume(c, &library_volume) == 0 &&
          library_volume == 0.25f);
    CHECK(nn_am_service_IAudioController_ChangeMainAppletMasterVolume(c, 0.75f, 0) == 0);
    CHECK(nn_am_service_IAudioController_GetMainAppletExpectedMasterVolume(c, &main_volume) == 0 &&
          main_volume == 0.75f);
    CHECK(nn_am_service_IAudioController_SetExpectedMasterVolume(c, 1.5f, -1.0f) == 0);
    CHECK(nn_am_service_IAudioController_GetMainAppletExpectedMasterVolume(c, &main_volume) == 0 &&
          main_volume == 1.0f);
    CHECK(nn_am_service_IAudioController_GetLibraryAppletExpectedMasterVolume(c, &library_volume) == 0 &&
          library_volume == 0.0f);
    CHECK(nn_am_service_IApplicationProxy_GetAudioController(NULL, &other) == 0);
    CHECK(nn_am_service_IAudioController_GetLibraryAppletExpectedMasterVolume(other, &library_volume) == 0 &&
          library_volume == 1.0f);
    svcobj_unref(other);
    svcobj_unref(c);
    CHECK(test_objects == 0);
}
/* the user channel: a copy of each storage unpopped, the last one popped first, and ClearUserChannel empties it */
static void test_user_channel(void) {
    SvcObj *a = am_storage_new(4, "abc"), *b = am_storage_new(4, "xyz"), *s = NULL;
    int i;
    CHECK(nn_am_service_IApplicationFunctions_PopLaunchParameter(NULL, 1, &s) == AM_NO_DATA);
    CHECK(nn_am_service_IApplicationFunctions_UnpopToUserChannel(NULL, a) == 0);
    CHECK(nn_am_service_IApplicationFunctions_UnpopToUserChannel(NULL, b) == 0);
    memcpy(am_storage_bytes(a), "zzz", 4);
    CHECK(nn_am_service_IApplicationFunctions_PopLaunchParameter(NULL, 1, &s) == 0);
    CHECK(((StorageData*)s->data)->size == 4 && memcmp(am_storage_bytes(s), "xyz", 4) == 0);
    svcobj_unref(s);
    CHECK(nn_am_service_IApplicationFunctions_PopLaunchParameter(NULL, 1, &s) == 0);
    CHECK(memcmp(am_storage_bytes(s), "abc", 4) == 0);
    svcobj_unref(s);
    CHECK(nn_am_service_IApplicationFunctions_PopLaunchParameter(NULL, 1, &s) == AM_NO_DATA);
    for (i = 0; i < AM_USER_CHANNEL; i++) CHECK(nn_am_service_IApplicationFunctions_UnpopToUserChannel(NULL, a) == 0);
    CHECK(nn_am_service_IApplicationFunctions_UnpopToUserChannel(NULL, a) == AM_OUT_OF_MEMORY);
    CHECK(nn_am_service_IApplicationFunctions_ClearUserChannel(NULL) == 0);
    CHECK(nn_am_service_IApplicationFunctions_PopLaunchParameter(NULL, 1, &s) == AM_NO_DATA);
    svcobj_unref(a);
    svcobj_unref(b);
    CHECK(test_objects == 0);
}
static void test_previous_program(void) {
    i32 index = 0;
    CHECK(nn_am_service_IApplicationFunctions_GetPreviousProgramIndex(NULL, &index) == 0 && index == -1);
}
/* IStorageAccessor's Read: the whole buffer within the storage, or InvalidOffset */
static void test_storage_read(void) {
    static u8 guest[32];
    SvcObj *s = am_storage_new(8, "abcdefg"), *acc = NULL;
    IpcBuf buf = {0, 8, 0};
    g_mem = guest;
    CHECK(nn_am_service_IStorage_Open(s, &acc) == 0);
    CHECK(nn_am_service_IStorageAccessor_Read(acc, 0, buf) == 0 && memcmp(guest, "abcdefg", 8) == 0);
    buf.size = 16;
    CHECK(nn_am_service_IStorageAccessor_Read(acc, 0, buf) == AM_INVALID_OFFSET);
    buf.size = 4;
    CHECK(nn_am_service_IStorageAccessor_Read(acc, 6, buf) == AM_INVALID_OFFSET);
    CHECK(nn_am_service_IStorageAccessor_Read(acc, 4, buf) == 0 && memcmp(guest, "efg", 4) == 0);
    CHECK(nn_am_service_IStorageAccessor_Read(acc, -1, buf) == AM_INVALID_OFFSET);
    g_mem = NULL;
    svcobj_unref(acc);
    svcobj_unref(s);
    CHECK(test_objects == 0);
}
/* VR mode: on and off as the game turns it, by SetVrModeEnabled or by BeginVrModeEx and EndVrModeEx */
static void test_vr_mode(void) {
    u8 on = 0xee;
    CHECK(nn_am_service_ICommonStateGetter_IsVrModeEnabled(NULL, &on) == 0 && on == 0);
    CHECK(nn_am_service_ICommonStateGetter_BeginVrModeEx(NULL) == 0);
    CHECK(nn_am_service_ICommonStateGetter_IsVrModeEnabled(NULL, &on) == 0 && on == 1);
    CHECK(nn_am_service_ICommonStateGetter_EndVrModeEx(NULL) == 0);
    CHECK(nn_am_service_ICommonStateGetter_IsVrModeEnabled(NULL, &on) == 0 && on == 0);
    CHECK(nn_am_service_ICommonStateGetter_SetVrModeEnabled(NULL, 2) == 0);
    CHECK(nn_am_service_ICommonStateGetter_IsVrModeEnabled(NULL, &on) == 0 && on == 1);
    CHECK(nn_am_service_ICommonStateGetter_SetVrModeEnabled(NULL, 0) == 0);
    CHECK(nn_am_service_ICommonStateGetter_IsVrModeEnabled(NULL, &on) == 0 && on == 0);
}
static void test_inline(void) {
    SvcObj* a;
    SvcObj* s;
    LibApplet* la;
    StorageData* data;
    u8 completed;
    u32 handle;
    dialog_cancel = 0;
    CHECK(nn_am_service_ILibraryAppletCreator_CreateLibraryApplet(NULL, 0x11, 3, &a) == 0);
    la = a->data;
    CHECK(nn_am_service_ILibraryAppletAccessor_GetAppletStateChangedEvent(a, &handle) == 0);
    CHECK(!la->state_ev->signaled);
    CHECK(nn_am_service_ILibraryAppletAccessor_IsCompleted(a, &completed) == 0 && !completed);
    send_request(a, 0xA, 1, 0);
    CHECK(la->interactive_ev->signaled);
    svcobj_unref(reply(a, 1, 0, 9));
    CHECK(!la->interactive_ev->signaled);
    CHECK(nn_am_service_ILibraryAppletAccessor_PopInteractiveOutData(a, &s) == RC(128, 2));
    send_request(a, 0xA, 4, 0);
    svcobj_unref(reply(a, 3, 1, 8));
    svcobj_unref(reply(a, 3, 2, 0x404));
    s = reply(a, 3, 5, 0x3F8);
    data = s->data;
    CHECK(get32(data->d + 8 + 0x3EC) == 4);
    CHECK(data->d[8] == 'A' && data->d[10] == 0xE9);
    CHECK(data->d[12] == 0x3D && data->d[13] == 0xD8);
    svcobj_unref(s);
    svcobj_unref(reply(a, 1, 1, 8));
    CHECK(!la->state_ev->signaled);
    send_request(a, 0xA, 4 | 0x20, 1);
    svcobj_unref(reply(a, 3, 1, 8));
    svcobj_unref(reply(a, 3, 7, 0x7EC));
    s = reply(a, 3, 9, 0x7E0);
    CHECK(strcmp((char*)((StorageData*)s->data)->d + 8, dialog_text) == 0);
    svcobj_unref(s);
    svcobj_unref(reply(a, 1, 1, 8));
    dialog_cancel = 1;
    send_request(a, 0xA, 4, 1);
    svcobj_unref(reply(a, 3, 1, 8));
    svcobj_unref(reply(a, 3, 6, 8));
    svcobj_unref(reply(a, 1, 1, 8));
    send_request(a, 4, 0, 0);
    CHECK(la->state_ev->signaled);
    CHECK(nn_am_service_ILibraryAppletAccessor_IsCompleted(a, &completed) == 0 && completed);
    svcobj_unref(a);
    close_handles();
    CHECK(test_objects == 0 && test_kobjs == 0);
}
/* a library applet run to its end: its pushed storages, then what PopOutData gives */
static SvcObj* run_applet(u32 id, SvcObj* const* in, int nin) {
    SvcObj *a, *out = NULL;
    int i;
    CHECK(nn_am_service_ILibraryAppletCreator_CreateLibraryApplet(NULL, id, 0, &a) == 0);
    for (i = 0; i < nin; i++) CHECK(nn_am_service_ILibraryAppletAccessor_PushInData(a, in[i]) == 0);
    CHECK(nn_am_service_ILibraryAppletAccessor_PopOutData(a, &out) == 0 && out);
    CHECK(nn_am_service_ILibraryAppletAccessor_PopOutData(a, &out) == RC(128, 2)); /* given once */
    svcobj_unref(a);
    return out;
}
/* the standard keyboard: SwkbdConfigCommon's limits (max at 0x3ac, min at 0x3b0) and UTF-8 flag (0x3bd), the dialog
   asked again until the text is long enough, and the reply's SwkbdResult and text */
static SvcObj* swkbd(u32 max, u32 min, int utf8) {
    u8 config[0x3e0] = {0};
    SvcObj *in[2], *out;
    put32(config + 0x3ac, max);
    put32(config + 0x3b0, min);
    config[0x3bd] = (u8)utf8;
    in[0] = am_storage_new(0x20, NULL);
    in[1] = am_storage_new(sizeof config, config);
    dialog_calls = 0;
    out = run_applet(0x11, in, 2);
    svcobj_unref(in[0]);
    svcobj_unref(in[1]);
    CHECK(((StorageData*)out->data)->size == 0x7d8);
    return out;
}
/* SetCpuBoostMode hands its mode to apm, and GetCurrentPerformanceConfiguration is apm's for the console's mode */
extern u32 g_test_apm_boost;
static void test_cpu_boost(void) {
    u32 config = 0;
    CHECK(nn_am_service_ICommonStateGetter_SetCpuBoostMode(NULL, 1) == 0 && g_test_apm_boost == 1);
    CHECK(nn_am_service_ICommonStateGetter_GetCurrentPerformanceConfiguration(NULL, &config) == 0 &&
          config == 0x00020003u);
    g_settings.tv_mode = 1;
    CHECK(nn_am_service_ICommonStateGetter_GetCurrentPerformanceConfiguration(NULL, &config) == 0 &&
          config == 0x92220009u);
    g_settings.tv_mode = 0;
}

/* a lock accessor from GetHomeButtonReaderLockAccessor (30) and GetReaderLockAccessorEx (31): its event signaled
   while the lock is free, before anyone took it, and taken by TryLock (1) */
static void test_lock_event(void) {
    SvcObj* accessors[2];
    u8 locked = 0;
    u32 event = 0, i;
    CHECK(nn_am_service_ICommonStateGetter_GetHomeButtonReaderLockAccessor(NULL, &accessors[0]) == 0);
    CHECK(nn_am_service_ICommonStateGetter_GetReaderLockAccessorEx(NULL, 0, &accessors[1]) == 0);
    for (i = 0; i < 2; i++) {
        CHECK(((AmLock*)accessors[i]->data)->event->signaled);
        CHECK(nn_am_service_ILockAccessor_TryLock(accessors[i], 0, &locked, &event) == 0 && locked == 1);
        CHECK(nn_am_service_ILockAccessor_Unlock(accessors[i]) == 0);
        svcobj_unref(accessors[i]);
    }
}

static void test_swkbd(void) {
    SvcObj* out;
    u8* d;
    u16 units[4];
    dialog_cancel = 0;
    dialog_then = NULL;
    /* "Aé😀" is four UTF-16 units: three at most keeps the pair whole, cut before it */
    out = swkbd(3, 0, 0);
    d = ((StorageData*)out->data)->d;
    CHECK(get32(d) == 0 && d[4] == 'A' && d[6] == 0xE9 && d[7] == 0 && d[8] == 0 && d[9] == 0);
    svcobj_unref(out);
    out = swkbd(0, 0, 1);
    CHECK(!strcmp((char*)((StorageData*)out->data)->d + 4, "Aé😀"));
    svcobj_unref(out);
    /* too short for the minimum: asked again, and the second answer is taken */
    dialog_then = "hello";
    out = swkbd(0, 5, 1);
    CHECK(dialog_calls == 2 && get32(((StorageData*)out->data)->d) == 0);
    CHECK(!strcmp((char*)((StorageData*)out->data)->d + 4, "hello"));
    svcobj_unref(out);
    dialog_then = NULL;
    /* a minimum above the maximum is the maximum */
    out = swkbd(2, 9, 0);
    d = ((StorageData*)out->data)->d;
    CHECK(dialog_calls == 1 && get32(d) == 0 && d[4] == 'A' && d[6] == 0xE9 && d[8] == 0);
    svcobj_unref(out);
    /* cancelled */
    dialog_cancel = 1;
    out = swkbd(0, 0, 0);
    CHECK(get32(((StorageData*)out->data)->d) == 1 && ((StorageData*)out->data)->d[4] == 0);
    svcobj_unref(out);
    dialog_cancel = 0;
    /* malformed UTF-8 from the host: an overlong form, a surrogate and a cut sequence, each part U+FFFD */
    CHECK(am_utf8_to_16("\xC0\xAF", units, 4) == 2 && units[0] == 0xFFFD && units[1] == 0xFFFD);
    CHECK(am_utf8_to_16("\xED\xA0\x80", units, 4) == 3 && units[2] == 0xFFFD);
    CHECK(am_utf8_to_16("a\xE2\x82", units, 4) == 2 && units[0] == 'a' && units[1] == 0xFFFD);
    CHECK(am_utf8_to_16("\xF4\x90\x80\x80", units, 4) == 4); /* past U+10FFFF */
    CHECK(am_utf8_to_16("\xF0\x9F\x98\x80", units, 4) == 2 && units[0] == 0xD83D && units[1] == 0xDE00);
    /* a scripted run's text, cancelled when it is shorter than the minimum */
    strcpy(g_settings.keyboard_text, "hi");
    out = swkbd(0, 0, 1);
    CHECK(dialog_calls == 0 && get32(((StorageData*)out->data)->d) == 0);
    CHECK(!strcmp((char*)((StorageData*)out->data)->d + 4, "hi"));
    svcobj_unref(out);
    out = swkbd(0, 3, 1);
    CHECK(get32(((StorageData*)out->data)->d) == 1);
    svcobj_unref(out);
    g_settings.keyboard_text[0] = 0;
}
/* the other applets' outputs: the controller applet's players against the game's minimum, a web applet's return
   value, and zeros from the rest */
/* the offline web applet on page (a WebArgHeader: two entries, the document kind (7) 1, an offline page, and its
   path (6)), run to its end: its WebCommonReturnValue */
static SvcObj* offline_page(const char* page) {
    u8 arg[0x100] = {2, 0, 0, 0, 3, 0, 0, 0, 7, 0, 4, 0, 0, 0, 0, 0, 1, 0, 0, 0, 6, 0};
    SvcObj *in, *out;
    size_t n = strlen(page) + 1;
    arg[22] = (u8)n;
    memcpy(arg + 28, page, n);
    in = am_storage_new(sizeof arg, arg);
    out = run_applet(0x17, &in, 1);
    svcobj_unref(in);
    CHECK(((StorageData*)out->data)->size == 0x1010);
    return out;
}
static void test_applets(void) {
    u8 arg[0x40] = {2};
    SvcObj *in[2], *out;
    in[0] = am_storage_new(0x20, NULL);
    in[1] = am_storage_new(sizeof arg, arg);
    test_players = 1;
    out = run_applet(0x0c, in, 2);
    CHECK(((StorageData*)out->data)->size == 0xc && ((StorageData*)out->data)->d[0] == 1);
    CHECK(get32(((StorageData*)out->data)->d + 8) == 2); /* fewer than two: cancelled */
    svcobj_unref(out);
    test_players = 2;
    out = run_applet(0x0c, in, 2);
    CHECK(((StorageData*)out->data)->d[0] == 2 && get32(((StorageData*)out->data)->d + 8) == 0);
    svcobj_unref(out);
    test_players = 1;
    out = run_applet(0x14, in, 0); /* Shop */
    CHECK(((StorageData*)out->data)->size == 0x1010);
    svcobj_unref(out);
    out = offline_page("pages.htdocs/index.html"); /* answered as its A button: CallbackUrl and the URL */
    CHECK(get32(((StorageData*)out->data)->d) == 3 &&
          !strcmp((const char*)((StorageData*)out->data)->d + 8, "http://localhost/?ret=Yes") &&
          get32(((StorageData*)out->data)->d + 0x1008) == 26);
    svcobj_unref(out);
    out = offline_page("plain.htdocs/index.html"); /* no A button: closed, no URL */
    CHECK(get32(((StorageData*)out->data)->d) == 0 && ((StorageData*)out->data)->d[8] == 0);
    svcobj_unref(out);
    out = offline_page("missing.htdocs/index.html");
    CHECK(get32(((StorageData*)out->data)->d) == 0);
    svcobj_unref(out);
    out = run_applet(0x0e, in, 0); /* the error applet */
    CHECK(((StorageData*)out->data)->size == 0x10 && get32(((StorageData*)out->data)->d) == 0);
    svcobj_unref(out);
    svcobj_unref(in[0]);
    svcobj_unref(in[1]);
    close_handles();
}
/* the events am_init made, which g_hle.am keeps: released before it runs again, which makes new ones */
static void am_events_release(void) {
    kobj_unref(&g_hle.am.msg_event->o);
    kobj_unref(&g_hle.am.sleep_lock_event->o);
    kobj_unref(&g_hle.am.launchable_event->o);
    kobj_unref(&g_hle.am.resolution_event->o);
}
/* am_init: the focus message, then the performance mode's, and the launchable event signaled */
static void test_messages(void) {
    nn_am_AppletMessage m = 0;
    u32 h;
    am_init();
    CHECK(nn_am_service_ICommonStateGetter_ReceiveMessage(NULL, &m) == 0 && m == 15);
    CHECK(g_hle.am.msg_event->signaled);
    CHECK(nn_am_service_ICommonStateGetter_ReceiveMessage(NULL, &m) == 0 && m == 31);
    CHECK(!g_hle.am.msg_event->signaled);
    CHECK(nn_am_service_ICommonStateGetter_ReceiveMessage(NULL, &m) == RC(128, 3));
    CHECK(nn_am_service_ISelfController_GetLibraryAppletLaunchableEvent(NULL, &h) == 0);
    CHECK(test_handles[h - 0x100] == &g_hle.am.launchable_event->o && g_hle.am.launchable_event->signaled);
    close_handles();
    am_events_release();
}
/* the Mii editor (0x12), which a game opens with nn::mii::AppendMii: with no editor here, appending is
   cancelled as when the player backs out, an edit hands the Mii back, and each answer is the size the SDK reads */
static SvcObj* mii_edit(u32 mode, const u8* charinfo, u32 out_size) {
    u8 input[0x100] = {0};
    SvcObj* a;
    SvcObj* storage;
    put32(input, 4); /* version 4 */
    put32(input + 4, mode);
    if (charinfo) memcpy(input + 0xc, charinfo, 0x58);
    CHECK(nn_am_service_ILibraryAppletCreator_CreateLibraryApplet(NULL, 0x12, 0, &a) == 0);
    storage = am_storage_new(sizeof input, input);
    CHECK(nn_am_service_ILibraryAppletAccessor_PushInData(a, storage) == 0);
    svcobj_unref(storage);
    storage = NULL;
    CHECK(nn_am_service_ILibraryAppletAccessor_PopOutData(a, &storage) == 0 && storage);
    CHECK(((StorageData*)storage->data)->size == out_size);
    svcobj_unref(a);
    return storage;
}
static void test_mii_edit(void) {
    u8 charinfo[0x58];
    SvcObj* out;
    StorageData* d;
    int i;
    for (i = 0; i < 0x58; i++) charinfo[i] = (u8)(i + 1);
    out = mii_edit(1, NULL, 0x20); /* AppendMii: cancelled */
    d = out->data;
    CHECK(get32(d->d) == 1 && get32(d->d + 4) == 0);
    svcobj_unref(out);
    out = mii_edit(0, NULL, 0x20); /* ShowMiiEdit: shown */
    CHECK(get32(((StorageData*)out->data)->d) == 0);
    svcobj_unref(out);
    out = mii_edit(5, charinfo, 0x80); /* EditMii: the same Mii back */
    d = out->data;
    CHECK(get32(d->d) == 0 && !memcmp(d->d + 4, charinfo, 0x58));
    svcobj_unref(out);
    out = mii_edit(4, NULL, 0x80); /* CreateMii: cancelled */
    CHECK(get32(((StorageData*)out->data)->d) == 1);
    svcobj_unref(out);
    close_handles();
    CHECK(test_objects == 0 && test_kobjs == 0);
}
/* ICommonStateGetter: 130 queues the start-up logo's message, 95, behind the focus and performance ones at startup;
   1000 to 1002 keep 3D VR mode; 300 says the platform region is Global */
static void test_state_getter(void) {
    u32 message = 0, region = 0;
    u8 vr = 0xee;
    int logo = 0, received = 0;
    am_init();
    CHECK(nn_am_service_ICommonStateGetter_EnableStartupLogoDisappearedMessage(NULL) == 0 && g_hle.am.msg_event->signaled);
    while (received < AM_MESSAGES && nn_am_service_ICommonStateGetter_ReceiveMessage(NULL, &message) == 0) {
        logo += message == AM_MSG_STARTUP_LOGO_GONE;
        received++;
    }
    CHECK(received == 3 && logo == 1 && !g_hle.am.msg_event->signaled);
    CHECK(nn_am_service_ICommonStateGetter_IsVrModeEnabled3d(NULL, &vr) == 0 && vr == 0);
    CHECK(nn_am_service_ICommonStateGetter_BeginVrMode3d(NULL) == 0);
    CHECK(nn_am_service_ICommonStateGetter_IsVrModeEnabled3d(NULL, &vr) == 0 && vr == 1);
    CHECK(nn_am_service_ICommonStateGetter_EndVrMode3d(NULL) == 0);
    CHECK(nn_am_service_ICommonStateGetter_IsVrModeEnabled3d(NULL, &vr) == 0 && vr == 0);
    CHECK(nn_am_service_ICommonStateGetter_GetSettingsPlatformRegion(NULL, &region) == 0 && region == 1);
    am_events_release();
}
/* ISelfController: the idle time detection extension and auto sleep read back as the game set them */
static void test_self_settings(void) {
    u32 extension = 0xee;
    u8 disabled = 0xee;
    CHECK(nn_am_service_ISelfController_GetIdleTimeDetectionExtension(NULL, &extension) == 0 && extension == 0);
    CHECK(nn_am_service_ISelfController_SetIdleTimeDetectionExtension(NULL, 1) == 0);
    CHECK(nn_am_service_ISelfController_GetIdleTimeDetectionExtension(NULL, &extension) == 0 && extension == 1);
    CHECK(nn_am_service_ISelfController_SetIdleTimeDetectionExtension(NULL, 0) == 0);
    CHECK(nn_am_service_ISelfController_GetIdleTimeDetectionExtension(NULL, &extension) == 0 && extension == 0);
    CHECK(nn_am_service_ISelfController_IsAutoSleepDisabled(NULL, &disabled) == 0 && disabled == 0);
    CHECK(nn_am_service_ISelfController_SetAutoSleepDisabled(NULL, 1) == 0);
    CHECK(nn_am_service_ISelfController_IsAutoSleepDisabled(NULL, &disabled) == 0 && disabled == 1);
    CHECK(nn_am_service_ISelfController_SetAutoSleepDisabled(NULL, 0) == 0);
    CHECK(nn_am_service_ISelfController_IsAutoSleepDisabled(NULL, &disabled) == 0 && disabled == 0);
}

int main(void) {
    g_settings.tv_mode = 0; /* handheld, as the test was written for */
    hle_init();
    g_hle.kernel.title_id = 0x0100BF00112C0000ull; /* the title the test was written for */
    test_mii_edit();
    test_audio_controller();
    test_user_channel();
    test_previous_program();
    test_storage_read();
    test_vr_mode();
    test_inline();
    modern_packet = 1;
    test_inline();
    test_swkbd();
    test_lock_event();
    test_cpu_boost();
    test_applets();
    CHECK(test_objects == 0 && test_kobjs == 0);
    test_messages();
    test_state_getter();
    test_self_settings();
    CHECK(test_kobjs == 0);
    printf("am_service: %u checks passed\n", test_checks);
    return 0;
}
