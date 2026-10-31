/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* acc (acc.c) with the console's users (users.c): users/users.ini, the user the game plays as, their nicknames and
   pictures, and every users.ini that is no list of users refused. A user with no picture of its own gets Wimsy, drawn
   as a 256x256 JPEG of the size GetImageSize tells (through the generated dispatchers), read back by plutovg's
   decoder. SNX_TEST_PICTURE names a file to keep the JPEG in */
#include "nx/hle/services/services_int.h"
#include "common/utilities/common.h"
#include <plutovg.h>
#include <stdarg.h>
#include <stdio.h>

#include "nx/hle/services/acc.c"
#include "nx/hle/services/users.c"
#include "nx/hle/hle.c"

u8* g_mem;
const u64 g_user_id[2] = {0x0123456789ABCDEFull, 0xFEDCBA9876543210ull};
/* acc.c's IsUserAccountSwitchLocked asks am.c, which this test doesn't compile: the NACP field is a switch here */
static int test_account_switch_lock;
int am_user_account_switch_lock(void) { return test_account_switch_lock; }
char g_exedir[1024];
static unsigned test_checks, test_objects;

static void test_fail(const char* expression, int line) {
    fprintf(stderr, "acc_service.c:%d: check failed: %s\n", line, expression);
    exit(1);
}
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        test_checks++;                                                                                                 \
        if (!(expr)) test_fail(#expr, __LINE__);                                                                       \
    } while (0)

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
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
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
int ipc_bad_request(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return RC(11, 420);
}
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
int new_event(u32* event, const char* name, int signaled) {
    (void)name, (void)signaled;
    *event = 0x100;
    return 0;
}

typedef struct {
    IpcCtx c;
    u8 raw[32];
} Request;
static IpcCtx* request(Request* r, u32 cmd) {
    memset(r, 0, sizeof *r);
    r->c.cmd = cmd;
    r->c.raw = r->raw;
    r->c.raw_size = sizeof r->raw;
    return &r->c;
}
static u32 reply32(Request* r, u32 off) {
    CHECK(off + 4 <= r->c.out_size);
    return snx_ld_le32(r->c.out + off); /* the dispatcher writes the reply as the guest reads it */
}
static u64 reply64(Request* r, u32 off) {
    CHECK(off + 8 <= r->c.out_size);
    return snx_ld_le64(r->c.out + off);
}
static void call(SvcObj* o, Request* r) { CHECK(o->dispatch(o, &r->c) == 0); }
/* acc:u0's GetBaasAccountManagerForApplication (101) for the user */
static SvcObj* manager_for(SvcObj* acc, const u64 uid[2]) {
    Request r;
    request(&r, 101);
    acc_uid_put(r.raw, uid);
    call(acc, &r);
    CHECK(r.c.nout_objs == 1);
    return r.c.out_objs[0];
}

/* the frame of a JPEG: its first start of frame (C0 to C2) before the scan, and the components it has */
static int jpeg_frame(const u8* d, u32 size, u32* width, u32* height, u32* components) {
    u32 at = 2;
    while (at + 4 <= size && d[at] == 0xff) {
        u32 marker = d[at + 1], length = (u32)d[at + 2] << 8 | d[at + 3];
        if (marker >= 0xc0 && marker <= 0xc2 && at + 10 <= size) {
            *height = (u32)d[at + 5] << 8 | d[at + 6];
            *width = (u32)d[at + 7] << 8 | d[at + 8];
            *components = d[at + 9];
            return 1;
        }
        if (marker == 0xda || length < 2) return 0;
        at += 2 + length;
    }
    return 0;
}

/* acc:u0's GetProfile (5) of the user, then IProfile's GetImageSize (10) and LoadImage (11) into a buffer of
   0x20000, what a game reserves; and LoadImage into less, which writes what fits and tells the whole size */
static void test_picture(void) {
    SvcObj *acc = new_acc(), *profile;
    Request r;
    u32 size, loaded, width = 0, height = 0, components = 0;
    const u8* picture = g_mem + 0x1000;
    request(&r, 5);
    acc_uid_put(r.raw, g_user_id);
    call(acc, &r);
    CHECK(r.c.nout_objs == 1);
    profile = r.c.out_objs[0];

    request(&r, 10);
    call(profile, &r);
    size = reply32(&r, 0);
    request(&r, 11);
    r.c.nb = 1;
    r.c.b[0].addr = 0x1000;
    r.c.b[0].size = 0x20000;
    call(profile, &r);
    loaded = reply32(&r, 0);
    CHECK(size == loaded && size > 4 && size <= 0x20000);
    CHECK(picture[0] == 0xff && picture[1] == 0xd8 && picture[size - 2] == 0xff && picture[size - 1] == 0xd9);
    CHECK(jpeg_frame(picture, size, &width, &height, &components));
    CHECK(width == 256 && height == 256 && components == 3);
    if (getenv("SNX_TEST_PICTURE")) {
        FILE* f = fopen(getenv("SNX_TEST_PICTURE"), "wb");
        CHECK(f && fwrite(picture, 1, size, f) == size && !fclose(f));
    }
    /* it decodes: white in the corner, around the drawing, and Wimsy in the middle */
    plutovg_surface_t* decoded = plutovg_surface_load_from_image_data(picture, (int)size);
    CHECK(decoded && plutovg_surface_get_width(decoded) == 256 && plutovg_surface_get_height(decoded) == 256);
    const u8* corner = plutovg_surface_get_data(decoded);
    const u8* middle = corner + 128 * plutovg_surface_get_stride(decoded) + 128 * 4;
    CHECK(corner[0] > 240 && corner[1] > 240 && corner[2] > 240);
    CHECK(middle[0] < 200 || middle[1] < 200 || middle[2] < 200);
    plutovg_surface_destroy(decoded);

    memset(g_mem + 0x1000, 0xee, 0x20000);
    request(&r, 11);
    r.c.nb = 1;
    r.c.b[0].addr = 0x1000;
    r.c.b[0].size = 16;
    call(profile, &r);
    CHECK(reply32(&r, 0) == size && picture[0] == 0xff && picture[15] != 0xee && picture[16] == 0xee);

    svcobj_unref(profile);
    svcobj_unref(acc);
}

#define DIR    "acc_service_users"
#define UID1   "0123456789abcdeffedcba9876543210"
#define UID2   "00000000000000020000000000000002"
#define UID3   "0000000000000003000000000000000a"
#define NICK20 "Ab9 x.y_z-0123456789" /* the longest a room takes, with every kind of character */
static const unsigned char jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 1, 2, 3, 0xFF, 0xD9};

static void put(const char* name, const void* data, size_t size) {
    char* path = snx_path_join(DIR, name);
    CHECK(!snx_write_file(path, data, size));
    free(path);
}
static void put_ini(const char* text) { put("users.ini", text, strlen(text)); }
static void gone(const char* name) {
    char* path = snx_path_join(DIR, name);
    remove(path);
    free(path);
}
/* the users.ini text is refused, with why naming what is wrong, and user 1 alone stays */
static void refused(const char* text, int selected, const char* why_has) {
    char why[512];
    put_ini(text);
    CHECK(snx_users_load(DIR, selected, why, sizeof why) == -1);
    if (!strstr(why, why_has)) fprintf(stderr, "why: %s\n", why);
    CHECK(strstr(why, why_has) != NULL);
    CHECK(g_user_count == 1 && !strcmp(g_users[0].nickname, "Player"));
}
static void uid_of(const char* text, nn_account_Uid* uid) {
    unsigned long long hi = 0, lo = 0;
    u64 v[2];
    CHECK(strlen(text) == 32 && sscanf(text, "%16llx%16llx", &hi, &lo) == 2);
    v[0] = hi;
    v[1] = lo;
    acc_uid_put(uid->v, v);
}
/* uid bytes as the game holds them (each half little-endian), against users.c's halves */
static int same_uid(const u8* bytes, const u64 v[2]) {
    u8 b[16];
    acc_uid_put(b, v);
    return !memcmp(bytes, b, 16);
}

static void test_one_user(void) {
    static u8 memory[0x400];
    char why[512], text[33];
    IpcBuf list;
    nn_account_Uid uid;
    i32 count;
    u8 exists;
    g_mem = memory;
    /* no users.ini: user 1 alone, "Player", by the uid this runtime always had */
    CHECK(snx_users_load(DIR, 1, why, sizeof why) == 0);
    CHECK(g_user_count == 1 && !strcmp(g_users[0].nickname, "Player"));
    snx_users_uid_text(g_users[0].uid, text);
    CHECK(!strcmp(text, UID1));
    CHECK(nn_account_IAccountServiceForApplication_GetUserCount(NULL, &count) == 0 && count == 1);
    memset(memory, 0xAA, sizeof memory);
    list.addr = 0x100;
    list.size = 0xA0;
    CHECK(nn_account_IAccountServiceForApplication_ListAllUsers(NULL, list) == 0);
    CHECK(same_uid(memory + 0x100, g_user_id));
    for (count = 16; count < 0xA0; count++) CHECK(memory[0x100 + count] == 0);
    CHECK(memory[0x1A0] == 0xAA);
    /* one user is chosen without asking */
    CHECK(nn_account_IAccountServiceForApplication_TrySelectUserWithoutInteraction(NULL, 0, &uid) == 0);
    CHECK(same_uid(uid.v, g_user_id));
    uid_of(UID2, &uid);
    CHECK(nn_account_IAccountServiceForApplication_GetUserExistence(NULL, &uid, &exists) == 0 && !exists);
    /* without users.ini, user=2 names nobody */
    CHECK(snx_users_load(DIR, 2, why, sizeof why) == -1 && strstr(why, "user=2") && strstr(why, "no users.ini"));
    puts("PASS user 1 alone without users.ini");
}

/* ListOpenContextStoredUsers holds what StoreOpenContext kept for the next program: nothing for a game the system
   started, which a game with several programs reads as "show the title", not "resume the program before" */
static void test_open_context(void) {
    static u8 memory[0x400];
    char why[512];
    IpcBuf list;
    nn_account_Uid uid;
    SvcObj* manager = NULL;
    int i;
    g_mem = memory;
    CHECK(snx_users_load(DIR, 1, why, sizeof why) == 0);
    memset(memory, 0xAA, sizeof memory);
    list.addr = 0x100;
    list.size = 0xA0;
    CHECK(nn_account_IAccountServiceForApplication_ListOpenContextStoredUsers(NULL, list) == 0);
    for (i = 0; i < 0xA0; i++) CHECK(memory[0x100 + i] == 0);
    CHECK(memory[0x1A0] == 0xAA);
    /* a program about to ExecuteProgram keeps its open user */
    acc_uid_put(uid.v, snx_users_selected()->uid);
    CHECK(nn_account_IAccountServiceForApplication_GetBaasAccountManagerForApplication(NULL, &uid, &manager) == 0);
    CHECK(nn_account_baas_IManagerForApplication_StoreOpenContext(manager) == 0);
    svcobj_unref(manager);
    memset(memory, 0xAA, sizeof memory);
    CHECK(nn_account_IAccountServiceForApplication_ListOpenContextStoredUsers(NULL, list) == 0);
    CHECK(same_uid(memory + 0x100, snx_users_selected()->uid));
    for (i = 16; i < 0xA0; i++) CHECK(memory[0x100 + i] == 0);
    /* 5.0.0's command 60 lists the same */
    {
        SvcObj* acc = new_acc();
        Request r;
        memset(memory, 0xAA, sizeof memory);
        request(&r, 60);
        r.c.nc = 1;
        r.c.c[0] = list;
        call(acc, &r);
        CHECK(same_uid(memory + 0x100, snx_users_selected()->uid) && memory[0x110] == 0 && memory[0x1A0] == 0xAA);
        svcobj_unref(acc);
    }
    g_hle.acc.context_stored = 0;
    puts("PASS no stored open context until StoreOpenContext, then the open user, at 131 and 5.0.0's 60");
}

/* the user's network service account id, made from its uid as Eden makes it (Common::UUID::Hash), from GetAccountId
   (1) and GetNintendoAccountUserResourceCacheForApplication (130) */
static void test_account_id(void) {
    static u8 memory[0x400];
    char why[512];
    SvcObj *acc = new_acc(), *manager;
    Request r;
    g_mem = memory;
    CHECK(snx_users_load(DIR, 1, why, sizeof why) == 0);
    manager = manager_for(acc, g_user_id);
    request(&r, 1);
    call(manager, &r);
    CHECK(reply64(&r, 0) == 0xFC9A30576503A9CEull);
    request(&r, 130);
    r.c.nc = 1;
    r.c.c[0].addr = 0x100;
    r.c.c[0].size = 0x68;
    r.c.nb = 1;
    r.c.b[0].addr = 0x200;
    r.c.b[0].size = 0x40;
    call(manager, &r);
    CHECK(reply64(&r, 0) == 0xFC9A30576503A9CEull);
    svcobj_unref(manager);
    svcobj_unref(acc);
    puts("PASS the network service account id, as Eden makes it from the uid");
}

/* LoadIdTokenCacheDeprecated (3) or LoadIdTokenCache (4) into a buffer of size at 0x100 */
static void token_request(Request* r, u32 cmd, u64 size) {
    request(r, cmd);
    r->c.nb = 1;
    r->c.b[0].addr = 0x100;
    r->c.b[0].size = size;
}
/* the id token: with none, 3 writes nothing and answers 0, and 4 (19.0.0+) answers a run of 0x100 zeros, as much of it
   as the buffer takes; a token is written as far as the buffer takes, zeros after it to the buffer's end, and its
   length answered */
static void test_id_token(void) {
    static u8 memory[0x400];
    static const char token[] = "eyJhbGciOiJSUzI1NiJ9";
    char why[512];
    SvcObj *acc = new_acc(), *manager;
    Request r;
    u32 cmd;
    int i;
    g_mem = memory;
    CHECK(snx_users_load(DIR, 1, why, sizeof why) == 0);
    manager = manager_for(acc, g_user_id);
    memset(memory, 0xAA, sizeof memory);
    token_request(&r, 3, 0x200);
    call(manager, &r);
    CHECK(reply32(&r, 0) == 0 && memory[0x100] == 0xAA && memory[0x2FF] == 0xAA);
    token_request(&r, 4, 0x200);
    call(manager, &r);
    CHECK(reply32(&r, 0) == 0x100);
    for (i = 0; i < 0x100; i++) CHECK(memory[0x100 + i] == 0);
    CHECK(memory[0x200] == 0xAA);
    memset(memory, 0xAA, sizeof memory);
    token_request(&r, 4, 0x40);
    call(manager, &r);
    CHECK(reply32(&r, 0) == 0x40 && memory[0x13F] == 0 && memory[0x140] == 0xAA);
    /* a token, which both commands answer alike */
    memcpy(g_hle.acc.token, token, sizeof token);
    for (cmd = 3; cmd <= 4; cmd++) {
        memset(memory, 0xAA, sizeof memory);
        token_request(&r, cmd, 0x40);
        call(manager, &r);
        CHECK(reply32(&r, 0) == sizeof token - 1 && !memcmp(memory + 0x100, token, sizeof token - 1));
        for (i = (int)sizeof token - 1; i < 0x40; i++) CHECK(memory[0x100 + i] == 0);
        CHECK(memory[0x140] == 0xAA);
    }
    /* longer than the buffer: as far as it takes */
    memset(memory, 0xAA, sizeof memory);
    token_request(&r, 3, 8);
    call(manager, &r);
    CHECK(reply32(&r, 0) == 8 && !memcmp(memory + 0x100, token, 8) && memory[0x108] == 0xAA);
    memset(g_hle.acc.token, 0, sizeof g_hle.acc.token);
    svcobj_unref(manager);
    svcobj_unref(acc);
    puts("PASS the id token: none, as 3 and 4 answer it apart, and one as far as the buffer takes");
}

/* EnsureIdTokenCacheAsync (2): its context is done at once, its event signaled, and it succeeded, with no token to
   load after it; Cancel then changes nothing */
static void test_ensure_id_token(void) {
    static u8 memory[0x400];
    char why[512];
    SvcObj *acc = new_acc(), *manager, *context;
    Request r;
    g_mem = memory;
    CHECK(snx_users_load(DIR, 1, why, sizeof why) == 0);
    manager = manager_for(acc, g_user_id);
    request(&r, 2);
    call(manager, &r);
    CHECK(r.c.nout_objs == 1);
    context = r.c.out_objs[0];
    request(&r, 0);
    call(context, &r);
    CHECK(r.c.nout_copy == 1);
    request(&r, 2);
    call(context, &r);
    CHECK(r.c.out_size >= 1 && r.c.out[0] == 1);
    request(&r, 3);
    CHECK(context->dispatch(context, &r.c) == 0);
    request(&r, 1);
    call(context, &r);
    request(&r, 3);
    CHECK(context->dispatch(context, &r.c) == 0);
    memset(memory, 0xAA, sizeof memory);
    token_request(&r, 3, 0x40);
    call(manager, &r);
    CHECK(reply32(&r, 0) == 0 && memory[0x100] == 0xAA);
    svcobj_unref(context);
    svcobj_unref(manager);
    svcobj_unref(acc);
    puts("PASS EnsureIdTokenCacheAsync done at once and succeeded, with no token");
}

/* InitializeApplicationInfoV2, which Tears of the Kingdom and Super Mario Bros. Wonder call at boot (13.0.0+): a
   pid in, nothing out, as on the console */
static void test_initialize_application_info_v2(void) {
    SvcObj* acc = new_acc();
    Request r;
    IpcCtx* c = request(&r, 160);
    c->has_pid = 1;
    call(acc, &r);
    CHECK(c->out_size == 0 && c->nout_objs == 0 && c->nout_copy == 0);
    svcobj_unref(acc);
}

/* the two acc replies: users may register, and the NACP's switch lock is what the game said */
static void test_account_command_replies(void) {
    SvcObj* acc = new_acc();
    Request r;
    IpcCtx* c;
    c = request(&r, 50);
    c->has_pid = 1;
    call(acc, &r);
    CHECK(c->out_size >= 1 && c->out[0] == 1);
    test_account_switch_lock = 0;
    c = request(&r, 150);
    call(acc, &r);
    CHECK(c->out_size >= 1 && c->out[0] == 0);
    test_account_switch_lock = 1;
    c = request(&r, 150);
    call(acc, &r);
    CHECK(c->out_size >= 1 && c->out[0] == 1);
    svcobj_unref(acc);
}

static void test_users(void) {
    static u8 memory[0x400];
    char why[512];
    IpcBuf list, image;
    nn_account_Uid uid, got;
    nn_account_profile_ProfileBase base;
    SvcObj* profile;
    i32 count;
    u32 size;
    u8 exists;
    g_mem = memory;
    put_ini("; the console's users\n"
            "[user1]\n"
            "nickname=Aogo\n"
            "[user2]\n"
            "uid=" UID2 "\n"
            "nickname=" NICK20 "\n"
            "[user3]\n"
            "uid=" UID3 "\n"
            "nickname=Afugo\n");
    put(UID2 ".jpg", jpeg, sizeof jpeg);
    CHECK(snx_users_load(DIR, 2, why, sizeof why) == 0);
    CHECK(g_user_count == 3 && !strcmp(g_users[0].nickname, "Aogo") && !strcmp(g_users[2].nickname, "Afugo"));
    CHECK(nn_account_IAccountServiceForApplication_GetUserCount(NULL, &count) == 0 && count == 3);
    /* every user in the console's order */
    memset(memory, 0xAA, sizeof memory);
    list.addr = 0x100;
    list.size = 0xA0;
    CHECK(nn_account_IAccountServiceForApplication_ListAllUsers(NULL, list) == 0);
    CHECK(same_uid(memory + 0x100, g_user_id));
    uid_of(UID2, &uid);
    CHECK(!memcmp(memory + 0x110, uid.v, 16));
    uid_of(UID3, &uid);
    CHECK(!memcmp(memory + 0x120, uid.v, 16));
    CHECK(memory[0x130] == 0 && memory[0x19F] == 0);
    /* a list two users long takes two */
    memset(memory, 0xAA, sizeof memory);
    list.size = 0x20;
    CHECK(nn_account_IAccountServiceForApplication_ListQualifiedUsers(NULL, list) == 0);
    uid_of(UID2, &uid);
    CHECK(!memcmp(memory + 0x110, uid.v, 16) && memory[0x120] == 0xAA);
    /* the game plays as user 2: the one open, the last opened */
    memset(memory, 0xAA, sizeof memory);
    list.size = 0xA0;
    CHECK(nn_account_IAccountServiceForApplication_ListOpenUsers(NULL, list) == 0);
    CHECK(!memcmp(memory + 0x100, uid.v, 16) && memory[0x110] == 0 && memory[0x19F] == 0);
    CHECK(nn_account_IAccountServiceForApplication_GetLastOpenedUser(NULL, &got) == 0 && !memcmp(got.v, uid.v, 16));
    CHECK(same_uid(uid.v, snx_users_selected()->uid));
    /* with three users nobody is chosen without asking: the game asks the user selector (am.c) */
    memset(&got, 0xAA, sizeof got);
    CHECK(nn_account_IAccountServiceForApplication_TrySelectUserWithoutInteraction(NULL, 1, &got) == 0);
    for (count = 0; count < 16; count++) CHECK(got.v[count] == 0);
    CHECK(nn_account_IAccountServiceForApplication_GetUserExistence(NULL, &uid, &exists) == 0 && exists);
    /* user 2's profile: its nickname, the longest there is, its picture */
    profile = NULL;
    CHECK(nn_account_IAccountServiceForApplication_GetProfile(NULL, &uid, &profile) == 0 && profile);
    memset(&base, 0xAA, sizeof base);
    CHECK(nn_account_profile_IProfile_GetBase(profile, &base) == 0);
    CHECK(!memcmp(base.uid.v, uid.v, 16) && !memcmp(base.nickname, NICK20, 20) && base.nickname[20] == 0);
    CHECK(nn_account_profile_IProfile_GetImageSize(profile, &size) == 0 && size == sizeof jpeg);
    memset(memory, 0xAA, sizeof memory);
    image.addr = 0x200;
    image.size = 0x100;
    CHECK(nn_account_profile_IProfile_LoadImage(profile, image, &size) == 0 && size == sizeof jpeg);
    CHECK(!memcmp(memory + 0x200, jpeg, sizeof jpeg) && memory[0x200 + sizeof jpeg] == 0xAA);
    /* a buffer shorter than the picture takes its start, and hears the whole size */
    memset(memory, 0xAA, sizeof memory);
    image.size = 4;
    CHECK(nn_account_profile_IProfile_LoadImage(profile, image, &size) == 0 && size == sizeof jpeg);
    CHECK(!memcmp(memory + 0x200, jpeg, 4) && memory[0x204] == 0xAA);
    svcobj_unref(profile);
    /* user 3: its own nickname, ending in zeros, and no picture of its own: Wimsy */
    uid_of(UID3, &uid);
    CHECK(nn_account_IAccountServiceForApplication_GetProfile(NULL, &uid, &profile) == 0);
    memset(&base, 0xAA, sizeof base);
    CHECK(nn_account_profile_IProfile_GetBase(profile, &base) == 0);
    CHECK(!memcmp(base.nickname, "Afugo", 6) && base.nickname[31] == 0);
    CHECK(nn_account_profile_IProfile_GetImageSize(profile, &size) == 0 && size == acc_picture() && size > sizeof jpeg);
    svcobj_unref(profile);
    /* a uid that is no user's has no profile */
    uid_of("00000000000000040000000000000004", &uid);
    CHECK(nn_account_IAccountServiceForApplication_GetProfile(NULL, &uid, &profile) == ACC_INVALID_USER_ID);
    gone(UID2 ".jpg");
    puts("PASS three users: the list, the open user, the selection, nicknames and pictures");
}

/* user 1 has the uid users.ini gives it (a setup's random one), and the fixed one only when it gives none */
static void test_first_uid(void) {
    static u8 memory[0x400];
    char why[512], text[33];
    IpcBuf list;
    nn_account_Uid uid;
    uint64_t v[2];
    g_mem = memory;
    put_ini("[user1]\nuid=" UID3 "\nnickname=Aogo\n[user2]\nuid=" UID1 "\nnickname=Xuji\n");
    CHECK(snx_users_load(DIR, 1, why, sizeof why) == 0 && g_user_count == 2);
    snx_users_uid_text(g_users[0].uid, text);
    CHECK(!strcmp(text, UID3) && !strcmp(g_users[0].nickname, "Aogo"));
    snx_users_uid_text(g_users[1].uid, text);
    CHECK(!strcmp(text, UID1)); /* the fixed uid is any user's to have */
    memset(memory, 0xAA, sizeof memory);
    list.addr = 0x100;
    list.size = 0x20;
    CHECK(nn_account_IAccountServiceForApplication_ListAllUsers(NULL, list) == 0);
    uid_of(UID3, &uid);
    CHECK(!memcmp(memory + 0x100, uid.v, 16) && same_uid(memory + 0x110, g_user_id));
    CHECK(nn_account_IAccountServiceForApplication_GetLastOpenedUser(NULL, &uid) == 0);
    acc_uid_get(&uid, v);
    snx_users_uid_text(v, text);
    CHECK(!strcmp(text, UID3));
    /* a [user1] that names no uid has the fixed one */
    put_ini("[user1]\nnickname=Aogo\n");
    CHECK(snx_users_load(DIR, 1, why, sizeof why) == 0 && g_user_count == 1);
    snx_users_uid_text(g_users[0].uid, text);
    CHECK(!strcmp(text, UID1) && !strcmp(g_users[0].nickname, "Aogo"));
    puts("PASS user 1's uid from users.ini, the fixed one without");
}

/* the file read back as text, NULL when it is not there */
static char* got(const char* name) {
    char* path = snx_path_join(DIR, name);
    size_t size;
    char* text = snx_is_file(path) ? (char*)snx_read_file(path, &size) : NULL;
    free(path);
    return text;
}
/* users.ini started over with one user: the one before kept as users.ini.old, nothing written for what is refused */
static void test_start(void) {
    static const uint64_t three[2] = {3, 10}, two[2] = {2, 2}, zero[2] = {0, 0};
    char why[512], text[33], *before, *now, *old;
    gone("users.ini");
    gone("users.ini.old");
    CHECK(snx_users_start(DIR, three, "Ada", why, sizeof why) == -1 && strstr(why, "nickname Ada is not 4 to 20"));
    CHECK(snx_users_start(DIR, zero, "Aogo", why, sizeof why) == -1 && strstr(why, "zero"));
    CHECK(!got("users.ini") && !got("users.ini.old") && !got("users.ini.new"));
    /* none before: the new one alone, read as user 1 of that uid */
    CHECK(snx_users_start(DIR, three, NICK20, why, sizeof why) == 0 && !got("users.ini.old"));
    CHECK(snx_users_load(DIR, 1, why, sizeof why) == 0 && g_user_count == 1 && !strcmp(g_users[0].nickname, NICK20));
    snx_users_uid_text(g_users[0].uid, text);
    CHECK(!strcmp(text, UID3));
    /* three users before: they go to users.ini.old as they were */
    put_ini("[user1]\nnickname=Aogo\n[user2]\nuid=" UID2 "\nnickname=Xuji\n[user3]\nuid=" UID3 "\nnickname=Afugo\n");
    before = got("users.ini");
    CHECK(snx_users_start(DIR, two, "Afugo", why, sizeof why) == 0);
    old = got("users.ini.old");
    now = got("users.ini");
    CHECK(old && before && !strcmp(old, before) && now && strstr(now, "[user1]\nuid=" UID2 "\nnickname=Afugo\n"));
    CHECK(!strstr(now, "[user2]") && !got("users.ini.new"));
    CHECK(snx_users_load(DIR, 1, why, sizeof why) == 0 && g_user_count == 1 && !strcmp(g_users[0].nickname, "Afugo"));
    free(old);
    free(before);
    /* again: users.ini.old is the one it replaces */
    CHECK(snx_users_start(DIR, three, "Aogo", why, sizeof why) == 0);
    old = got("users.ini.old");
    CHECK(old && !strcmp(old, now));
    free(old);
    free(now);
    gone("users.ini.old");
    puts("PASS users.ini started over with one user, the one before kept as users.ini.old");
}

/* a user's nickname changed in users.ini, its other lines as they were: a comment, the uid, a key after it; a section
   with none gets one; a nickname the rule refuses, or no such user, changes nothing; with no users.ini, user 1 starts
   one */
static void test_rename(void) {
    char why[512], *now;
    put_ini("; mine\n[user1]\nuid=" UID2 "\nnickname = Aogo\n\n[user2]\nuid=" UID3 "\n  nickname=Xuji\n");
    CHECK(snx_users_load(DIR, 1, why, sizeof why) == 0 && g_user_count == 2);
    CHECK(snx_users_rename(DIR, 2, "Xuji Two", why, sizeof why) == 0 && !strcmp(g_users[1].nickname, "Xuji Two"));
    now = got("users.ini");
    CHECK(now &&
          !strcmp(now, "; mine\n[user1]\nuid=" UID2 "\nnickname = Aogo\n\n[user2]\nuid=" UID3 "\nnickname=Xuji Two\n"));
    free(now);
    CHECK(snx_users_rename(DIR, 1, "Aogo.55", why, sizeof why) == 0);
    now = got("users.ini");
    CHECK(now && strstr(now, "[user1]\nuid=" UID2 "\nnickname=Aogo.55\n\n[user2]") && !got("users.ini.new"));
    free(now);
    CHECK(snx_users_load(DIR, 1, why, sizeof why) == 0 && !strcmp(g_users[0].nickname, "Aogo.55") &&
          !strcmp(g_users[1].nickname, "Xuji Two"));
    CHECK(snx_users_rename(DIR, 2, "Ada", why, sizeof why) == -1 && strstr(why, "nickname Ada is not 4 to 20"));
    CHECK(snx_users_rename(DIR, 3, "Afugo", why, sizeof why) == -1 && strstr(why, "there is no user 3: 2 users"));
    now = got("users.ini");
    CHECK(now && strstr(now, "nickname=Xuji Two\n")); /* as it was */
    free(now);
    /* a [user1] whose nickname line is missing at the file's end, no line's end after it */
    put_ini("[user1]");
    CHECK(snx_users_load(DIR, 1, why, sizeof why) == 0 && g_user_count == 1);
    CHECK(snx_users_rename(DIR, 1, "Afugo", why, sizeof why) == 0);
    now = got("users.ini");
    CHECK(now && !strcmp(now, "[user1]\nnickname=Afugo\n"));
    free(now);
    gone("users.ini");
    CHECK(snx_users_load(DIR, 1, why, sizeof why) == 0 && g_user_count == 1);
    CHECK(snx_users_rename(DIR, 1, "Wimsy", why, sizeof why) == 0);
    now = got("users.ini");
    CHECK(now && strstr(now, "[user1]\nuid=0123456789abcdeffedcba9876543210\nnickname=Wimsy\n"));
    free(now);
    gone("users.ini");
    gone("users.ini.old");
    puts("PASS a user renamed in users.ini, its other lines as they were");
}

static void test_refused(void) {
    char why[512];
    refused("[user1]\nnickname=Aogo\ncolour=red\n", 1, "colour is not a setting");
    refused("[user1]\nuid=" UID2 "\nnickname=Aogo\n[user2]\nuid=" UID2 "\nnickname=Xuji\n", 1,
            "[user2] has the uid of [user1]");
    refused("[user1]\nnickname=Aogo\n[user2]\nuid=" UID1 "\nnickname=Xuji\n", 1, "[user2] has the uid of [user1]");
    refused("[user1]\n[user3]\nuid=" UID3 "\nnickname=Afugo\n", 1, "in order");
    refused("[user1]\n[user2]\nuid=" UID2 "\nnickname=Xuji\n[user2]\nuid=" UID3 "\nnickname=Afugo\n", 1, "in order");
    refused("[user1]\n[user2]\nnickname=Xuji\n", 1, "[user2] has no uid");
    refused("[user1]\n[user2]\nuid=" UID2 "\n", 1, "[user2] has no nickname");
    refused("[user1]\n[user2]\nuid=" UID2 "\nnickname=Xuji\n[user3]\nuid=" UID2 "\nnickname=Afugo\n", 1,
            "[user3] has the uid of [user2]");
    refused("[user1]\n[user2]\nuid=00000000000000000000000000000000\nnickname=Xuji\n", 1, "nor zero");
    refused("[user1]\n[user2]\nuid=0123\nnickname=Xuji\n", 1, "not 32 hex digits");
    /* a nickname a room wouldn't take: too long, too short, empty, a letter outside ASCII, a sign, a tab */
    refused("[user1]\nnickname=" NICK20 "x\n", 1, "nickname=" NICK20 "x is not 4 to 20 letters");
    refused("[user1]\nnickname=Ada\n", 1, "nickname=Ada is not 4 to 20 letters");
    refused("[user1]\nnickname=\n", 1, "nickname= is not 4 to 20 letters");
    refused("[user1]\nnickname=Jo\xc3\xa3o\n", 1, "is not 4 to 20 letters");
    refused("[user1]\nnickname=Ada/Bob\n", 1, "is not 4 to 20 letters");
    refused("[user1]\nnickname=Ada\tBob\n", 1, "is not 4 to 20 letters");
    refused("[user1]\npassword=hunter2\n", 1, "is not a setting: uid, nickname");
    refused("[user1]\n[user2]\nuid=" UID2 "\nnickname=Xuji\n", 3, "user=3");
    refused("nickname=Aogo\n[user1]\n", 1, "before [user1]");
    refused("[players]\nnickname=Aogo\n", 1, "[user1] to [user8]");
    refused("[user1]\nnickname Aogo\n", 1, "neither");
    refused("; nothing\n", 1, "no [user1]");
    refused("[user1\n", 1, "no ]");
    /* a picture that is no JPEG, or too big */
    put_ini("[user1]\n");
    put(UID1 ".jpg", "GIF89a", 6);
    CHECK(snx_users_load(DIR, 1, why, sizeof why) == -1 && strstr(why, "not a JPEG"));
    {
        unsigned char* big = (unsigned char*)calloc(1, 0x20001);
        big[0] = 0xFF, big[1] = 0xD8, big[0x1FFFF] = 0xFF, big[0x20000] = 0xD9;
        put(UID1 ".jpg", big, 0x20001);
        CHECK(snx_users_load(DIR, 1, why, sizeof why) == -1 && strstr(why, "not a JPEG"));
        big[0x1FFFF - 1] = 0xFF, big[0x1FFFF] = 0xD9;
        put(UID1 ".jpg", big, 0x20000);
        CHECK(snx_users_load(DIR, 1, why, sizeof why) == 0 && g_users[0].picture_size == 0x20000);
        free(big);
    }
    gone(UID1 ".jpg");
    /* a [user1] with nothing in it is the first user as it always was */
    put_ini("[user1]\n");
    CHECK(snx_users_load(DIR, 1, why, sizeof why) == 0 && g_user_count == 1 && !strcmp(g_users[0].nickname, "Player"));
    puts("PASS every users.ini that is no list of users refused");
}

void pm_init(PMutex* m) { (void)m; }

int main(void) {
    hle_init();
    snx_remove_tree(DIR);
    CHECK(!snx_mkdirs(DIR));
    test_one_user();
    test_open_context();
    test_account_id();
    test_id_token();
    test_ensure_id_token();
    g_mem = calloc(1, 0x30000); /* user 1 alone, with no picture of its own */
    CHECK(g_mem != NULL);
    test_picture();
    free(g_mem);
    test_initialize_application_info_v2();
    test_account_command_replies();
    test_users();
    test_first_uid();
    test_start();
    test_rename();
    test_refused();
    gone("users.ini");
    snx_remove_tree(DIR);
    CHECK(test_objects == 0);
    printf("acc_service: %u checks passed\n", test_checks);
    return 0;
}
