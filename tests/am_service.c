/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Inline keyboard lifecycle and fixed-size SDK replies, with the keyboard's box and its player stood in for. */
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
void pc_init(PCond* c) { (void)c; }
void pc_broadcast(PCond* c) { (void)c; }
/* the log's lines since the last log_has, which looks for one */
static char test_log[16384];
void logf_(int level, const char* format, ...) {
    size_t n = strlen(test_log);
    va_list args;
    (void)level;
    va_start(args, format);
    vsnprintf(test_log + n, sizeof test_log - n, format, args);
    va_end(args);
}
static int log_has(const char* line) {
    int found = strstr(test_log, line) != NULL;
    if (!found) fprintf(stderr, "am_service.c: the log has no \"%s\" in:\n%s", line, test_log);
    test_log[0] = 0;
    return found;
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
    if (o->type == KO_SESSION) svcobj_unref(((KSession*)o)->obj);
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

void svcobj_ref(SvcObj* o) { o->refs++; }
SvcObj* ipc_in_obj(IpcCtx* c, int index) {
    (void)c;
    (void)index;
    return NULL;
}
void kevent_clear(KEvent* e) { e->signaled = 0; }
KObj* h_get_ref(u32 h, int type) {
    if (h == 0xffff8001u && type == KO_PROCESS) {
        kobj_ref(&g_hle.kernel.process);
        return &g_hle.kernel.process;
    }
    if (h >= 0x100 && h < test_next_handle) {
        KObj* o = test_handles[h - 0x100];
        if (o->type == type) {
            kobj_ref(o);
            return o;
        }
    }
    return NULL;
}
u32 session_new(SvcObj* obj) {
    KSession* s = calloc(1, sizeof *s);
    CHECK(s != NULL);
    s->o.type = KO_SESSION;
    s->obj = obj;
    test_kobjs++;
    return h_new(&s->o);
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
/* The keyboard's box, as the backend's stand-in: the last one shown and how many have, the ticket last hidden, and no
   window to show one in when no_window. The player answers a whole applet's box while its thread waits for it
   (kthread_wait): player_text, or a cancel */
static OverlayKeyboard box_shown;
static int box_shows, no_window;
static u32 box_hidden;
static const char* player_text = "Aé😀";
static int player_cancels;
static int modern_packet;
static u32 appear_min; /* the inline keyboard's least, in its appear argument */
int backend_keyboard_show(const OverlayKeyboard* k) {
    if (no_window) return -1;
    box_shown = *k;
    box_shows++;
    return 0;
}
void backend_keyboard_hide(u32 ticket) { box_hidden = ticket; }
static int popped_while_waiting; /* a whole keyboard applet counted as popped while its thread waited */
int kthread_wait(PCond* c, PMutex* m, i64 timeout_ns) {
    int i;
    (void)c, (void)m, (void)timeout_ns;
    for (i = 0; i < AM_APPLETS; i++)
        if (g_hle.am.applets[i].obj && g_hle.am.applets[i].applet_id == 0x11)
            popped_while_waiting |= g_hle.am.applets[i].out_popped;
    am_keyboard_answered(box_shown.ticket, player_cancels, player_text);
    return 0;
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
    put32(packet + 4 + (modern_packet ? 0x3C : 0x44), appear_min);
    packet[4 + (modern_packet ? 0x1C : 0x24)] = 'G'; /* the OK's label */
    packet[4 + (modern_packet ? 0x1E : 0x26)] = 'o';
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
/* the inline keyboard: Appear shows the box with the game's text, most, least and OK label; the replies wait for the
   player, whose text is the changed and decided string in the game's encoding, or a cancel; then it hides */
static void test_inline(void) {
    SvcObj* a;
    SvcObj* s;
    LibApplet* la;
    StorageData* data;
    u8 completed;
    u32 handle;
    int shows = box_shows;
    appear_min = 2;
    CHECK(nn_am_service_ILibraryAppletCreator_CreateLibraryApplet(NULL, 0x11, 3, &a) == 0);
    la = a->data;
    CHECK(nn_am_service_ILibraryAppletAccessor_GetAppletStateChangedEvent(a, &handle) == 0);
    CHECK(!kev(la->state_ev)->signaled);
    CHECK(nn_am_service_ILibraryAppletAccessor_IsCompleted(a, &completed) == 0 && !completed);
    send_request(a, 0xA, 1, 0);
    CHECK(kev(la->interactive_ev)->signaled);
    svcobj_unref(reply(a, 1, 0, 9));
    CHECK(!kev(la->interactive_ev)->signaled);
    CHECK(nn_am_service_ILibraryAppletAccessor_PopInteractiveOutData(a, &s) == RC(128, 2));
    send_request(a, 0xA, 4, 0);
    svcobj_unref(reply(a, 3, 1, 8));
    CHECK(box_shows == shows + 1 && box_shown.max == 20 && box_shown.min == 2 && !strcmp(box_shown.ok, "Go"));
    CHECK(nn_am_service_ILibraryAppletAccessor_PopInteractiveOutData(a, &s) == RC(128, 2)); /* the player types */
    am_keyboard_answered(box_shown.ticket, 0, player_text);
    svcobj_unref(reply(a, 3, 2, 0x404));
    s = reply(a, 3, 5, 0x3F8);
    data = s->data;
    CHECK(get32(data->d + 8 + 0x3EC) == 4);
    CHECK(data->d[8] == 'A' && data->d[10] == 0xE9);
    CHECK(data->d[12] == 0x3D && data->d[13] == 0xD8);
    svcobj_unref(s);
    svcobj_unref(reply(a, 1, 1, 8));
    CHECK(!kev(la->state_ev)->signaled);
    /* the text the game holds is the box's next line; UTF-8 replies */
    send_request(a, 0xA, 4 | 0x20, 1);
    svcobj_unref(reply(a, 3, 1, 8));
    CHECK(!strcmp(box_shown.text, player_text));
    am_keyboard_answered(box_shown.ticket, 0, "hey");
    svcobj_unref(reply(a, 3, 7, 0x7EC));
    s = reply(a, 3, 9, 0x7E0);
    CHECK(strcmp((char*)((StorageData*)s->data)->d + 8, "hey") == 0);
    svcobj_unref(s);
    svcobj_unref(reply(a, 1, 1, 8));
    /* cancelled, with the line the box had, as the backend hands it */
    send_request(a, 0xA, 4, 1);
    svcobj_unref(reply(a, 3, 1, 8));
    am_keyboard_answered(box_shown.ticket, 1, "typed");
    svcobj_unref(reply(a, 3, 6, 8));
    svcobj_unref(reply(a, 1, 1, 8));
    /* Disappear takes the box away, and an answer after it goes nowhere, said */
    send_request(a, 0xA, 4, 1);
    svcobj_unref(reply(a, 3, 1, 8));
    send_request(a, 0xA, 1u << 7, 1);
    svcobj_unref(reply(a, 1, 1, 8));
    CHECK(box_hidden == box_shown.ticket);
    test_log[0] = 0;
    am_keyboard_answered(box_shown.ticket, 0, "late");
    CHECK(nn_am_service_ILibraryAppletAccessor_PopInteractiveOutData(a, &s) == RC(128, 2));
    {
        char line[128];
        snprintf(line, sizeof line,
                 "am: the software keyboard's box %u answered after the game took it away: the game "
                 "has no answer\n",
                 box_shown.ticket);
        CHECK(log_has(line));
    }
    /* Finalize takes a box that shows away */
    send_request(a, 0xA, 4, 1);
    svcobj_unref(reply(a, 3, 1, 8));
    box_hidden = 0;
    send_request(a, 4, 0, 0);
    CHECK(box_hidden == box_shown.ticket);
    CHECK(kev(la->state_ev)->signaled);
    CHECK(nn_am_service_ILibraryAppletAccessor_IsCompleted(a, &completed) == 0 && completed);
    svcobj_unref(a);
    close_handles();
    CHECK(test_objects == 0 && test_kobjs == 0);
    appear_min = 0;
}
/* the inline keyboard answered without the box: swkbd_text first (short of the least: cancelled, said), and with no
   window the game's own text; an applet closed while its box shows takes it away; a save state's load puts back the
   box of a keyboard the state shows */
static void test_inline_unasked(void) {
    SvcObj *a, *b;
    SvcObj* s;
    int shows = box_shows;
    CHECK(nn_am_service_ILibraryAppletCreator_CreateLibraryApplet(NULL, 0x11, 3, &a) == 0);
    send_request(a, 0xA, 1, 1);
    svcobj_unref(reply(a, 1, 0, 9));
    strcpy(g_settings.keyboard_text, "hi");
    send_request(a, 0xA, 4 | 0x20, 1);
    svcobj_unref(reply(a, 3, 1, 8));
    svcobj_unref(reply(a, 3, 7, 0x7EC));
    s = reply(a, 3, 9, 0x7E0);
    CHECK(!strcmp((char*)((StorageData*)s->data)->d + 8, "hi") && box_shows == shows);
    svcobj_unref(s);
    svcobj_unref(reply(a, 1, 1, 8));
    appear_min = 3;
    test_log[0] = 0;
    send_request(a, 0xA, 4, 1);
    svcobj_unref(reply(a, 3, 1, 8));
    svcobj_unref(reply(a, 3, 6, 8));
    svcobj_unref(reply(a, 1, 1, 8));
    CHECK(log_has("am: the software keyboard is cancelled: swkbd_text \"hi\" is 2 characters, the game asks for 3 at "
                  "least\n"));
    appear_min = 0;
    g_settings.keyboard_text[0] = 0;
    /* no window: the game's text ("hi", as the last answer left it), said */
    no_window = 1;
    test_log[0] = 0;
    send_request(a, 0xA, 4, 1);
    svcobj_unref(reply(a, 3, 1, 8));
    svcobj_unref(reply(a, 3, 7, 0x7EC));
    s = reply(a, 3, 9, 0x7E0);
    CHECK(!strcmp((char*)((StorageData*)s->data)->d + 8, "hi") && box_shows == shows);
    CHECK(log_has("am: no window to show the inline keyboard's box in: it answers the game's text \"hi\"\n"));
    svcobj_unref(s);
    svcobj_unref(reply(a, 1, 1, 8));
    no_window = 0;
    /* a load puts the box back for the keyboard that shows, and takes the one before away */
    send_request(a, 0xA, 4, 1);
    svcobj_unref(reply(a, 3, 1, 8));
    box_hidden = 0;
    shows = box_shows;
    {
        u32 before = box_shown.ticket;
        am_state_loaded();
        CHECK(box_hidden == before && box_shows == shows + 1 && box_shown.ticket != before);
        CHECK(!strcmp(box_shown.text, "hi") && box_shown.max == 20);
    }
    am_keyboard_answered(box_shown.ticket, 0, "ok");
    svcobj_unref(reply(a, 3, 7, 0x7EC));
    svcobj_unref(reply(a, 3, 9, 0x7E0));
    svcobj_unref(reply(a, 1, 1, 8));
    /* a second applet's box, then the first closed: the second's stays; the second closed: it goes */
    CHECK(nn_am_service_ILibraryAppletCreator_CreateLibraryApplet(NULL, 0x11, 3, &b) == 0);
    send_request(b, 0xA, 1, 1);
    svcobj_unref(reply(b, 1, 0, 9));
    send_request(b, 0xA, 4, 1);
    svcobj_unref(reply(b, 3, 1, 8));
    box_hidden = 0;
    svcobj_unref(a);
    CHECK(box_hidden == 0);
    svcobj_unref(b);
    CHECK(box_hidden == box_shown.ticket);
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
/* the standard keyboard: SwkbdConfigCommon's texts (the OK's label at 0x4, the header at 0x24, the sub at 0xa6, the
   guide at 0x1a8), kind (0, NumberPad 1), limits (max at 0x3ac, min at 0x3b0), password mode (0x3b4) and UTF-8 flag
   (0x3bd), and the initial text in the work buffer (the third storage, from initial_string_offset 0x3c0 for
   initial_string_length 0x3c4 units); the reply's SwkbdResult and text */
static u32 config_kind, config_password;
static void put16s(u8* at, const char* text) {
    for (; *text; text++, at += 2) at[0] = (u8)*text;
}
static SvcObj* swkbd(u32 max, u32 min, int utf8, const char* initial) {
    u8 config[0x3e0] = {0}, work[0x40] = {0};
    SvcObj *in[3], *out;
    int i, n = 2;
    put32(config, config_kind);
    put16s(config + 0x4, "Send");
    put16s(config + 0x24, "Name");
    put16s(config + 0xa6, "Up to 8");
    put16s(config + 0x1a8, "Your name");
    put32(config + 0x3ac, max);
    put32(config + 0x3b0, min);
    put32(config + 0x3b4, config_password);
    config[0x3bd] = (u8)utf8;
    if (initial) {
        put16s(work + 8, initial);
        put32(config + 0x3c0, 8);
        put32(config + 0x3c4, (u32)strlen(initial));
        n = 3;
    }
    in[0] = am_storage_new(0x20, NULL);
    in[1] = am_storage_new(sizeof config, config);
    in[2] = am_storage_new(sizeof work, work);
    popped_while_waiting = 0;
    out = run_applet(0x11, in, n);
    for (i = 0; i < 3; i++) svcobj_unref(in[i]);
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
        CHECK(kev(((AmLock*)accessors[i]->data)->event)->signaled);
        CHECK(nn_am_service_ILockAccessor_TryLock(accessors[i], 0, &locked, &event) == 0 && locked == 1);
        CHECK(nn_am_service_ILockAccessor_Unlock(accessors[i]) == 0);
        svcobj_unref(accessors[i]);
    }
}

/* the standard keyboard's box, its thread waiting for the player; swkbd_text before it; no window */
static void test_swkbd(void) {
    SvcObj* out;
    u8* d;
    u16 units[4];
    int shows = box_shows;
    player_cancels = 0;
    player_text = "Aé😀";
    /* the box has the game's texts, kind, limits and initial text; "Aé😀" is four UTF-16 units: three at most keeps
       the pair whole, cut before it */
    out = swkbd(3, 0, 0, "Ann");
    CHECK(box_shows == shows + 1 && box_shown.max == 3 && box_shown.min == 0 && !strcmp(box_shown.text, "Ann"));
    CHECK(!strcmp(box_shown.ok, "Send") && !strcmp(box_shown.header, "Name") && !strcmp(box_shown.sub, "Up to 8") &&
          !strcmp(box_shown.guide, "Your name") && !box_shown.password && !box_shown.numbers);
    CHECK(!popped_while_waiting); /* a save state taken while it waits pops it again on load */
    d = ((StorageData*)out->data)->d;
    CHECK(get32(d) == 0 && d[4] == 'A' && d[6] == 0xE9 && d[7] == 0 && d[8] == 0 && d[9] == 0);
    svcobj_unref(out);
    out = swkbd(0, 0, 1, NULL);
    CHECK(!strcmp((char*)((StorageData*)out->data)->d + 4, "Aé😀") && box_shown.max == AM_SWKBD_UNITS);
    CHECK(box_shown.text[0] == 0);
    svcobj_unref(out);
    /* the least is the box's to keep; a least above the most is the most, said */
    out = swkbd(0, 5, 1, NULL);
    CHECK(box_shown.min == 5);
    svcobj_unref(out);
    test_log[0] = 0;
    out = swkbd(2, 9, 0, NULL);
    CHECK(box_shown.min == 2 && box_shown.max == 2);
    CHECK(log_has("am: the software keyboard asks for 9 to 2 characters, 2 it is\n"));
    svcobj_unref(out);
    /* a password, of digits */
    config_kind = 1;
    config_password = 1;
    out = swkbd(0, 0, 0, NULL);
    CHECK(box_shown.numbers && box_shown.password);
    svcobj_unref(out);
    config_kind = config_password = 0;
    /* cancelled */
    player_cancels = 1;
    out = swkbd(0, 0, 0, NULL);
    CHECK(get32(((StorageData*)out->data)->d) == 1 && ((StorageData*)out->data)->d[4] == 0);
    svcobj_unref(out);
    player_cancels = 0;
    /* no window: the game's text, else the user's nickname, each said; cut to the most, and short of the least
       cancelled, as swkbd_text is */
    no_window = 1;
    shows = box_shows;
    test_log[0] = 0;
    out = swkbd(0, 0, 1, "Ann");
    CHECK(get32(((StorageData*)out->data)->d) == 0 && !strcmp((char*)((StorageData*)out->data)->d + 4, "Ann"));
    CHECK(log_has("am: no window to show the software keyboard's box in: it answers the game's text \"Ann\"\n"));
    svcobj_unref(out);
    out = swkbd(0, 0, 1, NULL);
    CHECK(get32(((StorageData*)out->data)->d) == 0 && !strcmp((char*)((StorageData*)out->data)->d + 4, "Nadia"));
    CHECK(log_has("am: no window to show the software keyboard's box in: it answers the user's nickname \"Nadia\"\n"));
    svcobj_unref(out);
    out = swkbd(3, 0, 1, NULL);
    CHECK(get32(((StorageData*)out->data)->d) == 0 && !strcmp((char*)((StorageData*)out->data)->d + 4, "Nad"));
    CHECK(log_has("am: the software keyboard answers \"Nad\" (the user's nickname \"Nadia\", cut to the game's 3 "
                  "characters)\n"));
    svcobj_unref(out);
    out = swkbd(0, 6, 1, NULL);
    CHECK(get32(((StorageData*)out->data)->d) == 1 && box_shows == shows);
    CHECK(log_has("am: the software keyboard is cancelled: the user's nickname \"Nadia\" is 5 characters, the game "
                  "asks for 6 at least\n"));
    svcobj_unref(out);
    no_window = 0;
    /* malformed UTF-8 from the host: an overlong form, a surrogate and a cut sequence, each part U+FFFD */
    CHECK(am_utf8_to_16("\xC0\xAF", units, 4) == 2 && units[0] == 0xFFFD && units[1] == 0xFFFD);
    CHECK(am_utf8_to_16("\xED\xA0\x80", units, 4) == 3 && units[2] == 0xFFFD);
    CHECK(am_utf8_to_16("a\xE2\x82", units, 4) == 2 && units[0] == 'a' && units[1] == 0xFFFD);
    CHECK(am_utf8_to_16("\xF4\x90\x80\x80", units, 4) == 4); /* past U+10FFFF */
    CHECK(am_utf8_to_16("\xF0\x9F\x98\x80", units, 4) == 2 && units[0] == 0xD83D && units[1] == 0xDE00);
    /* a scripted run's text, before the box: cut to the most and cancelled short of the least, each said */
    strcpy(g_settings.keyboard_text, "hi");
    shows = box_shows;
    out = swkbd(0, 0, 1, NULL);
    CHECK(box_shows == shows && get32(((StorageData*)out->data)->d) == 0);
    CHECK(!strcmp((char*)((StorageData*)out->data)->d + 4, "hi"));
    svcobj_unref(out);
    test_log[0] = 0;
    out = swkbd(0, 3, 1, NULL);
    CHECK(get32(((StorageData*)out->data)->d) == 1);
    CHECK(log_has("am: the software keyboard is cancelled: swkbd_text \"hi\" is 2 characters, the game asks for 3 at "
                  "least\n"));
    svcobj_unref(out);
    out = swkbd(1, 0, 1, NULL);
    CHECK(!strcmp((char*)((StorageData*)out->data)->d + 4, "h"));
    CHECK(log_has("am: the software keyboard answers \"h\" (swkbd_text \"hi\", cut to the game's 1 character)\n"));
    svcobj_unref(out);
    g_settings.keyboard_text[0] = 0;
    CHECK(box_shows == shows);
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
    kobj_unref(&kev(g_hle.am.msg_event)->o);
    kobj_unref(&kev(g_hle.am.sleep_lock_event)->o);
    kobj_unref(&kev(g_hle.am.launchable_event)->o);
    kobj_unref(&kev(g_hle.am.resolution_event)->o);
}
/* am_init: the focus message, then the performance mode's, and the launchable event signaled */
static void test_messages(void) {
    nn_am_AppletMessage m = 0;
    u32 h;
    am_init();
    CHECK(nn_am_service_ICommonStateGetter_ReceiveMessage(NULL, &m) == 0 && m == 15);
    CHECK(kev(g_hle.am.msg_event)->signaled);
    CHECK(nn_am_service_ICommonStateGetter_ReceiveMessage(NULL, &m) == 0 && m == 31);
    CHECK(!kev(g_hle.am.msg_event)->signaled);
    CHECK(nn_am_service_ICommonStateGetter_ReceiveMessage(NULL, &m) == RC(128, 3));
    CHECK(nn_am_service_ISelfController_GetLibraryAppletLaunchableEvent(NULL, &h) == 0);
    CHECK(test_handles[h - 0x100] == &kev(g_hle.am.launchable_event)->o && kev(g_hle.am.launchable_event)->signaled);
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
    CHECK(nn_am_service_ICommonStateGetter_EnableStartupLogoDisappearedMessage(NULL) == 0 &&
          kev(g_hle.am.msg_event)->signaled);
    while (received < AM_MESSAGES && nn_am_service_ICommonStateGetter_ReceiveMessage(NULL, &message) == 0) {
        logo += message == AM_MSG_STARTUP_LOGO_GONE;
        received++;
    }
    CHECK(received == 3 && logo == 1 && !kev(g_hle.am.msg_event)->signaled);
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

/* the values these AM commands answer, rather than the zeros an unimplemented command leaves */
static void test_am_command_replies(void) {
    i32 hdcp = 0;
    i64 normal = 0, journal = 0, required = -1;
    u32 media = 0;
    u8 running = 0;
    CHECK(nn_am_service_ICommonStateGetter_GetHdcpAuthenticationState(NULL, &hdcp) == 0 && hdcp == 1);
    CHECK(nn_am_service_IApplicationFunctions_GetSaveDataSizeMax(NULL, &normal, &journal) == 0);
    CHECK(normal == 0xFFFFFFF && journal == 0xFFFFFFF);
    CHECK(nn_am_service_IApplicationFunctions_CreateCacheStorage(NULL, 0, 0, 0, &media, &required) == 0);
    CHECK(media == 1 && required == 0);
    CHECK(nn_am_service_IApplicationFunctions_NotifyRunning(NULL, &running) == 0 && running == 1);
}

/* Every SelfController shares one retained GSV session. Closing guest copies and their SelfControllers must not
   reset the application's bootstrap state. */
static void test_graphics_bootstrap(void) {
    SvcObj *controller, *peer;
    u32 first, second;
    nn_gsv_Bootstrap bootstrap = {.applet_resource_user_id = K_PROCESS_ID, .flags = 0x8000000000000006ull};
    u32 session = 0;
    g_hle.kernel.hw = SNX_HW_SWITCH2;
    CHECK(nn_am_service_IApplicationProxy_GetSelfController(NULL, &controller) == 0);
    CHECK(nn_am_service_IApplicationProxy_GetSelfController(NULL, &peer) == 0);
    CHECK(controller != peer);
    CHECK(nn_am_service_ISelfController_GetGraphicsSupervisor(controller, &first) == 0);
    CHECK(nn_am_service_ISelfController_GetGraphicsSupervisor(peer, &second) == 0);
    KSession* kept = (KSession*)kobj_at(g_hle.am.graphics_session);
    CHECK(kept && kept->o.type == KO_SESSION && kept->o.refs == 3);
    CHECK(test_handles[first - 0x100] == &kept->o && test_handles[second - 0x100] == &kept->o);
    SvcObj* self = kept->obj;
    AmGraphics* state = &g_hle.am.graphics;
    u64 modes = ~0ull;
    CHECK(nn_gsv_IGraphicsSupervisor_ConfigureNativeMemory(self, 2, &modes) == RC(1, 125));
    /* 2880 by 1620 RGBA8 in blocks of 16 GOBs, as one game's window texture asks: rows padded to 1664 */
    nn_gsv_BlockImagePreference image = {.kind = 2, .extent = 2879u | 1619u << 16, .block_height_log2 = 7,
                                         .pitch_gobs = 180, .pixel_format = 1, .block_format = 1};
    u64 size = 0, alignment = 0;
    CHECK(nn_gsv_IGraphicsSupervisor_CalculateRequirements(self, &image, 1, &size, &alignment) == RC(1, 125));
    CHECK(nn_gsv_IGraphicsSupervisor_InitializeFrameContext(self) == RC(1, 125) && !state->frame_phase);
    CHECK(nn_gsv_IGraphicsSupervisor_ActivateFrameContext(self) == RC(1, 125) && !state->frame_phase);
    CHECK(nn_gsv_IGraphicsSupervisor_InitializeGpuContext(self) == RC(1, 125) && !state->gpu_phase);
    CHECK(nn_gsv_IGraphicsSupervisor_ActivateGpuContext(self) == RC(1, 125) && !state->gpu_phase);
    CHECK(nn_gsv_IGraphicsSupervisor_OpenGpuSession(self, 3, &session) == RC(1, 125) && !session);
    CHECK(nn_gsv_IGraphicsSupervisor_OpenGpuSession(self, 4, &session) == RC(1, 125) && !session);
    CHECK(nn_gsv_IGraphicsSupervisor_OpenGpuSession(self, 2, &session) == RC(1, 116) && !session);
    CHECK(nn_gsv_IGraphicsSupervisor_InitializeDisplayContext(self) == RC(1, 125) && !state->display_context);
    CHECK(nn_gsv_IGraphicsSupervisor_OpenDisplaySession(self, 0x300020000ull, &session) == RC(1, 125) && !session);
    CHECK(nn_gsv_IGraphicsSupervisor_CompleteBootstrap(self) == RC(1, 125));
    CHECK(nn_gsv_IGraphicsSupervisor_BindProcess(self, 0xffff8001u) == RC(1, 125));
    CHECK(nn_gsv_IGraphicsSupervisor_Initialize(self, 0, &bootstrap) == RC(1, 114));
    bootstrap.parameter0 = 0x10000;
    CHECK(nn_gsv_IGraphicsSupervisor_Initialize(self, 0xffff8001u, &bootstrap) == RC(1, 33) && !state->phase);
    bootstrap.parameter0 = 0;
    CHECK(nn_gsv_IGraphicsSupervisor_Initialize(self, 0xffff8001u, &bootstrap) == 0);
    CHECK(nn_gsv_IGraphicsSupervisor_Initialize(self, 0xffff8001u, &bootstrap) == RC(1, 125));
    CHECK(nn_gsv_IGraphicsSupervisor_CompleteBootstrap(self) == RC(1, 125));
    CHECK(nn_gsv_IGraphicsSupervisor_GetNvidiaDriver(self, &session) == RC(1, 125) && !session);
    CHECK(nn_gsv_IGraphicsSupervisor_BindProcess(self, 0xffff8001u) == 0);
    CHECK(nn_gsv_IGraphicsSupervisor_CompleteBootstrap(self) == 0);
    CHECK(nn_gsv_IGraphicsSupervisor_ActivateFrameContext(self) == RC(1, 125) && !state->frame_phase);
    CHECK(nn_gsv_IGraphicsSupervisor_InitializeFrameContext(self) == 0 && state->frame_phase == 1);
    CHECK(nn_gsv_IGraphicsSupervisor_ActivateFrameContext(self) == 0 && state->frame_phase == 2);
    CHECK(nn_gsv_IGraphicsSupervisor_InitializeFrameContext(self) == 0 && state->frame_phase == 2);
    CHECK(nn_gsv_IGraphicsSupervisor_ActivateFrameContext(self) == 0 && state->frame_phase == 2);
    CHECK(nn_gsv_IGraphicsSupervisor_ActivateGpuContext(self) == RC(1, 125) && !state->gpu_phase);
    CHECK(nn_gsv_IGraphicsSupervisor_InitializeGpuContext(self) == 0 && state->gpu_phase == 1);
    CHECK(nn_gsv_IGraphicsSupervisor_ActivateGpuContext(self) == 0 && state->gpu_phase == 2);
    CHECK(nn_gsv_IGraphicsSupervisor_InitializeGpuContext(self) == 0 && state->gpu_phase == 2);
    CHECK(nn_gsv_IGraphicsSupervisor_ActivateGpuContext(self) == 0 && state->gpu_phase == 2);
    CHECK(nn_gsv_IGraphicsSupervisor_OpenGpuSession(self, 4, &session) == RC(1, 125) && !session);
    CHECK(nn_gsv_IGraphicsSupervisor_OpenGpuSession(self, 3ull | (1ull << 32), &session) == RC(1, 116));
    /* the display session needs the display context, which 303 ends; its flags are the user library's protocol */
    CHECK(nn_gsv_IGraphicsSupervisor_OpenDisplaySession(self, 0x300010000ull, &session) == RC(1, 125) && !session);
    CHECK(nn_gsv_IGraphicsSupervisor_InitializeDisplayContext(self) == 0 && state->display_context);
    CHECK(nn_gsv_IGraphicsSupervisor_OpenDisplaySession(self, 0x300050000ull, &session) == RC(1, 116) && !session);
    CHECK(nn_gsv_IGraphicsSupervisor_OpenDisplaySession(self, 0x400020000ull, &session) == RC(1, 116) && !session);
    CHECK(nn_gsv_IGraphicsSupervisor_FinalizeDisplayContext(self) == 0 && !state->display_context);
    CHECK(nn_gsv_IGraphicsSupervisor_OpenDisplaySession(self, 0x300020000ull, &session) == RC(1, 125) && !session);
    CHECK(nn_gsv_IGraphicsSupervisor_ConfigureNativeMemory(self, 2, &modes) == 0 && modes == 0);
    CHECK(nn_gsv_IGraphicsSupervisor_ConfigureNativeMemory(self, 1, &modes) == RC(1, 116));
    CHECK(nn_gsv_IGraphicsSupervisor_ConfigureNativeMemory(self, 2ull | (1ull << 32), &modes) == RC(1, 116));
    u32 request = 0, completed = 0;
    CHECK(nn_gsv_IGraphicsSupervisor_BindSyncpointMemory(self, 0, 0x1FF, 0x1000, 0x2000, 0, 0,
                                                       &request, &completed) == RC(1, 116));
    CHECK(nn_gsv_IGraphicsSupervisor_BindSyncpointMemory(self, 1, 0x1FF, 0x1000, 0x2000, 0, 0,
                                                       &request, &completed) == RC(1, 125));
    CHECK(nn_gsv_IGraphicsSupervisor_GetSyncpointSession(self, &session) == RC(1, 125));
    CHECK(nn_gsv_IGraphicsSupervisor_InitializeSyncpointContext(self) == 0);
    CHECK(nn_gsv_IGraphicsSupervisor_InitializeImageFormats(self) == RC(1, 125));
    CHECK(nn_gsv_IGraphicsSupervisor_InitializeImageContext(self) == 0);
    CHECK(nn_gsv_IGraphicsSupervisor_InitializeImageFormats(self) == 0);
    CHECK(nn_gsv_IGraphicsSupervisor_CalculateRequirements(self, &image, 1, &size, &alignment) == 0 &&
          size == 180ull * 64 * 1664 && alignment == 0x1000);
    CHECK(nn_gsv_IGraphicsSupervisor_CalculateRequirements(self, &image, 3, &size, &alignment) == 0 &&
          size == 3 * 180ull * 64 * 1664);
    CHECK(nn_gsv_IGraphicsSupervisor_CalculateNativeImageSize(self, &image, 1, &size) == RC(1, 33));
    image.kind = 1;
    image.extent = 2559u | 1439u << 16;
    image.pitch_gobs = 160;
    CHECK(nn_gsv_IGraphicsSupervisor_CalculateNativeImageSize(self, &image, 1, &size) == 0 &&
          size == 160ull * 64 * 1536);
    image.block_height_log2 = 9; /* 64 GOBs, past the largest block */
    CHECK(nn_gsv_IGraphicsSupervisor_CalculateNativeImageSize(self, &image, 1, &size) == RC(1, 33));
    /* 4096 GOBs by 65536 rows is 2^34 bytes: 2^30 of them would wrap to zero */
    image = (nn_gsv_BlockImagePreference){.kind = 2, .extent = 0xffffffffu, .block_height_log2 = 3, .pitch_gobs = 4096};
    CHECK(nn_gsv_IGraphicsSupervisor_CalculateRequirements(self, &image, 1u << 30, &size, &alignment) == RC(1, 101));
    CHECK(nn_gsv_IGraphicsSupervisor_CalculateRequirements(self, &image, 1u << 29, &size, &alignment) == 0 &&
          size == 1ull << 63);
    svcobj_unref(controller);
    svcobj_unref(peer);
    close_handles();
    CHECK(kept->o.refs == 1);
    CHECK(nn_am_service_IApplicationProxy_GetSelfController(NULL, &controller) == 0);
    CHECK(nn_am_service_ISelfController_GetGraphicsSupervisor(controller, &session) == 0);
    CHECK(test_handles[session - 0x100] == &kept->o && state->phase == 3 &&
          state->frame_phase == 2 && state->gpu_phase == 2);
    CHECK(nn_gsv_IGraphicsSupervisor_Initialize(kept->obj, 0xffff8001u, &bootstrap) == RC(1, 125));
    close_handles();
    svcobj_unref(controller);
    kobj_unref(&kept->o);
    g_hle.am.graphics_session = 0;
    *state = (AmGraphics){0};
    bootstrap.flags = 0x8000000000000001ull;
    CHECK(nn_gsv_IGraphicsSupervisor_Initialize(NULL, 0xffff8001u, &bootstrap) == 0);
    CHECK(nn_gsv_IGraphicsSupervisor_BindProcess(NULL, 0xffff8001u) == 0);
    CHECK(nn_gsv_IGraphicsSupervisor_CompleteBootstrap(NULL) == 0);
    *state = (AmGraphics){0};
    g_hle.kernel.hw = SNX_HW_SWITCH;
    CHECK(g_hle.kernel.process.refs == 1);
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
    test_inline_unasked();
    test_swkbd();
    test_lock_event();
    test_cpu_boost();
    test_am_command_replies();
    test_graphics_bootstrap();
    test_applets();
    CHECK(test_objects == 0 && test_kobjs == 0);
    test_messages();
    test_state_getter();
    test_self_settings();
    CHECK(test_kobjs == 0);
    printf("am_service: %u checks passed\n", test_checks);
    return 0;
}
