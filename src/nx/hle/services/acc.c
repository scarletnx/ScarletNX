/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/acc/acc.cpp, errors.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project (GPL-2.0-or-later) */
/* acc: the console's users (users.c), the one the game plays as open since it started, each with a network service
   account no server stands behind: an id made from the uid, and no id token */
#include "nx/hle/services/services_int.h"
#include "nx/hle/hle.h"
#include "gui/splash.h"

#include "hle/services/acc.swipc.inc"

#define ACC_INVALID_USER_ID RC(124, 20)
#define ACC_INVALID_APPLICATION RC(124, 22)
#define ACC_INVALID_LENGTH  RC(124, 32)
#define ACC_UNDER_MAINTENANCE   RC(124, 5249) /* the online account service is not there: "not available right now" */
#define ACC_THUMBNAIL_SIZE  0x24000      /* a save's thumbnail: 256x144 RGBA */
#define ACC_LAST_EDIT       1546300800   /* the user's profile was last changed on the first day of 2019 */
#define ACC_EMPTY_TOKEN     0x100        /* the zeros LoadIdTokenCache (19.0.0+) answers with no token */

/* a Uid's bytes are its two halves, little-endian as the game keeps them, and users.c's are host numbers */
static void acc_uid_get(const nn_account_Uid* uid, u64 v[2]) {
    v[0] = snx_ld_le64(uid->v);
    v[1] = snx_ld_le64(uid->v + 8);
}
static void acc_uid_put(u8* out, const u64 v[2]) {
    snx_st_le64(out, v[0]);
    snx_st_le64(out + 8, v[1]);
}
static const SnxUser* acc_user(const nn_account_Uid* uid) {
    u64 v[2];
    acc_uid_get(uid, v);
    return snx_users_find(v);
}

/* the users given, in order, then zeros (no user) in the rest of the list */
static void acc_list(IpcBuf users, const SnxUser* first, int count) {
    u64 i;
    memset(GP(users.addr), 0, (size_t)users.size);
    for (i = 0; i < (u64)count && (i + 1) * 16 <= users.size; i++) acc_uid_put(GP(users.addr + i * 16), first[i].uid);
}
static void acc_list_all(IpcBuf users) { acc_list(users, g_users, g_user_count); }
static void acc_list_open(IpcBuf users) { acc_list(users, snx_users_selected(), 1); }

/* an object that keeps the user it was made for, in an AccUser of g_hle.acc's */
static SvcObj* acc_for_user(SvcObj* o, const nn_account_Uid* uid) {
    acc_uid_get(uid, ((AccUser*)o->data)->uid);
    return o;
}

/* ============================================================ IAccountServiceForApplication */
static int nn_account_IAccountServiceForApplication_GetUserCount(SvcObj* self, i32* count) {
    (void)self;
    *count = g_user_count;
    R_SUCCEED();
}
static int nn_account_IAccountServiceForApplication_GetUserExistence(SvcObj* self, const nn_account_Uid* uid,
                                                                     u8* exists) {
    (void)self;
    *exists = acc_user(uid) != NULL;
    R_SUCCEED();
}
static int nn_account_IAccountServiceForApplication_ListAllUsers(SvcObj* self, IpcBuf users) {
    (void)self;
    acc_list_all(users);
    R_SUCCEED();
}
static int nn_account_IAccountServiceForApplication_ListOpenUsers(SvcObj* self, IpcBuf users) {
    (void)self;
    acc_list_open(users);
    R_SUCCEED();
}
static int nn_account_IAccountServiceForApplication_GetLastOpenedUser(SvcObj* self, nn_account_Uid* uid) {
    (void)self;
    acc_uid_put(uid->v, snx_users_selected()->uid);
    R_SUCCEED();
}
static int nn_account_IAccountServiceForApplication_GetProfile(SvcObj* self, const nn_account_Uid* uid,
                                                               SvcObj** profile) {
    (void)self;
    R_UNLESS(acc_user(uid), ACC_INVALID_USER_ID);
    *profile = acc_for_user(nn_account_profile_IProfile_new_data(g_hle.acc.users, g_hle.acc.user_next, "acc profiles and managers"), uid);
    R_SUCCEED();
}
/* both games and applets may register a user, as Eden's ProfileManager::CanSystemRegisterUser answers */
static int nn_account_IAccountServiceForApplication_IsUserRegistrationRequestPermitted(SvcObj* self, u64 pid,
                                                                                      u8* permitted) {
    (void)self, (void)pid;
    *permitted = 1;
    R_SUCCEED();
}
/* with one user, that user, chosen without asking; with more, none, and the game asks the user selector (am.c),
   which answers the user the game plays as */
static int nn_account_IAccountServiceForApplication_TrySelectUserWithoutInteractionDeprecated(
    SvcObj* self, u8 is_network_service_account_required, nn_account_Uid* uid) {
    (void)self, (void)is_network_service_account_required;
    memset(uid->v, 0, 16);
    if (g_user_count == 1) acc_uid_put(uid->v, g_users[0].uid);
    R_SUCCEED();
}
static int nn_account_IAccountServiceForApplication_TrySelectUserWithoutInteraction(
    SvcObj* self, u8 is_network_service_account_required, nn_account_Uid* uid) {
    return nn_account_IAccountServiceForApplication_TrySelectUserWithoutInteractionDeprecated(
        self, is_network_service_account_required, uid);
}
static int nn_account_IAccountServiceForApplication_GetBaasAccountManagerForApplication(SvcObj* self,
                                                                                        const nn_account_Uid* uid,
                                                                                        SvcObj** manager) {
    (void)self;
    R_UNLESS(acc_user(uid), ACC_INVALID_USER_ID);
    *manager = acc_for_user(nn_account_baas_IManagerForApplication_new_data(g_hle.acc.users, g_hle.acc.user_next,
                                                                                   "acc profiles and managers"), uid);
    R_SUCCEED();
}
static int nn_account_IAccountServiceForApplication_LoadOpenContext(SvcObj* self, const nn_account_Uid* uid,
                                                                    SvcObj** manager) {
    return nn_account_IAccountServiceForApplication_GetBaasAccountManagerForApplication(self, uid, manager);
}
/* none until StoreOpenContext: a program that finds a user here was started by another of its application's, and
   resumes that session instead of showing its title (Eden's GetStoredOpenedUsers) */
static int nn_account_IAccountServiceForApplication_ListOpenContextStoredUsers(SvcObj* self, IpcBuf users) {
    (void)self;
    acc_list(users, snx_users_selected(), g_hle.acc.context_stored ? 1 : 0);
    R_SUCCEED();
}
static int nn_account_IAccountServiceForApplication_ListOpenContextStoredUsersOld(SvcObj* self, IpcBuf users) {
    return nn_account_IAccountServiceForApplication_ListOpenContextStoredUsers(self, users);
}
static int nn_account_IAccountServiceForApplication_ListQualifiedUsers(SvcObj* self, IpcBuf users) {
    (void)self;
    acc_list_all(users);
    R_SUCCEED();
}
/* the game's own control.nacp says whether switching users is locked (Eden reads the same NACP field) */
static int nn_account_IAccountServiceForApplication_IsUserAccountSwitchLocked(SvcObj* self, u8* locked) {
    (void)self;
    *locked = am_user_account_switch_lock() ? 1 : 0;
    R_SUCCEED();
}

/* ============================================================ IProfile */
/* the thumbnail a game stores with a save: for a user, at its size. None is kept here, the saves have no screen to
   show it on */
static int nn_account_IAccountServiceForApplication_StoreSaveDataThumbnail(SvcObj* self, const nn_account_Uid* uid,
                                                                           IpcBuf image) {
    static const nn_account_Uid none;
    (void)self;
    R_UNLESS(memcmp(uid->v, none.v, sizeof none.v), ACC_INVALID_USER_ID);
    R_UNLESS(image.size == ACC_THUMBNAIL_SIZE, ACC_INVALID_LENGTH);
    R_SUCCEED();
}

static const SnxUser* acc_profile_user(SvcObj* self) {
    return snx_users_find(((const AccUser*)self->data)->uid); /* GetProfile made it for a user, and users don't go while the game runs */
}
static void acc_profile_base(SvcObj* self, nn_account_profile_ProfileBase* base) {
    const SnxUser* u = acc_profile_user(self);
    memset(base, 0, sizeof *base);
    acc_uid_put(base->uid.v, ((const AccUser*)self->data)->uid);
    base->last_edit_timestamp = ACC_LAST_EDIT;
    memcpy(base->nickname, u->nickname, strlen(u->nickname)); /* 32 bytes at most: the field's, with no NUL then */
}
/* the user data (icon, colors) is all zeros: the default icon */
static int nn_account_profile_IProfile_Get(SvcObj* self, IpcBuf user_data, nn_account_profile_ProfileBase* base) {
    memset(GP(user_data.addr), 0, (size_t)user_data.size);
    acc_profile_base(self, base);
    R_SUCCEED();
}
static int nn_account_profile_IProfile_GetBase(SvcObj* self, nn_account_profile_ProfileBase* base) {
    acc_profile_base(self, base);
    R_SUCCEED();
}
/* the picture of a user with none of its own: Wimsy in Fuddles' colours (dist/splash/wimsy-fuddles.svg, the
   splash's drawing) at 256x256 over white, a JPEG as the console's pictures are, drawn at the first ask and kept */
#define SNX_ACC_PICTURE_SIDE    256
#define SNX_ACC_PICTURE_QUALITY 90

static void acc_picture_write(void* closure, void* data, int size) {
    (void)closure;
    if (size < 0 || (u32)size > SNX_ACC_PICTURE_MAX - g_hle.acc.pic.size) {
        g_hle.acc.pic.overflow = 1;
        return;
    }
    memcpy(g_hle.acc.pic.d + g_hle.acc.pic.size, data, (size_t)size);
    g_hle.acc.pic.size += (u32)size;
}
/* the picture's size, drawing it the first time; 0, said once, when it cannot be drawn */
static u32 acc_picture(void) {
    u32 size;
    pm_lock(&g_host.acc.pic_lk);
    if (!g_hle.acc.pic.made) {
        g_hle.acc.pic.made = 1;
        if (snx_splash_jpeg("wimsy-fuddles", SNX_ACC_PICTURE_SIDE, SNX_ACC_PICTURE_QUALITY, acc_picture_write, NULL) ||
            g_hle.acc.pic.overflow) {
            WARN("acc: the user's picture was not drawn, games get none\n");
            g_hle.acc.pic.size = 0;
        }
    }
    size = g_hle.acc.pic.size;
    pm_unlock(&g_host.acc.pic_lk);
    return size;
}
/* GetImageSize tells the size LoadImage writes, so a game reserves what it gets: the user's own picture
   (users/<uid>.jpg), else Wimsy */
static int nn_account_profile_IProfile_GetImageSize(SvcObj* self, u32* size) {
    if (!snx_users_picture(acc_profile_user(self), size)) *size = acc_picture();
    R_SUCCEED();
}
static int nn_account_profile_IProfile_LoadImage(SvcObj* self, IpcBuf image, u32* size) {
    const unsigned char* jpeg = snx_users_picture(acc_profile_user(self), size);
    if (!jpeg) {
        *size = acc_picture();
        jpeg = g_hle.acc.pic.d;
    }
    memcpy(GP(image.addr), jpeg, (size_t)(image.size < *size ? image.size : *size));
    R_SUCCEED();
}

/* ============================================================ IManagerForApplication */
/* the id the account would have, made from the user's as Eden makes it (Common::UUID::Hash): the uid's first half
   xor its second turned left by one bit */
static int nn_account_baas_IManagerForApplication_GetAccountId(SvcObj* self, nn_account_NetworkServiceAccountId* id) {
    const AccUser* u = (const AccUser*)self->data;
    *id = u->uid[0] ^ (u->uid[1] << 1 | u->uid[1] >> 63);
    R_SUCCEED();
}
static int nn_account_baas_IManagerForApplication_GetNintendoAccountUserResourceCacheForApplication(SvcObj* self,
                                                                                                   IpcBuf base,
                                                                                                   IpcBuf extra,
                                                                                                   u64* id) {
    memset(GP(base.addr), 0, (size_t)base.size);
    memset(GP(extra.addr), 0, (size_t)extra.size);
    /* the online account's id (`NintendoAccountId`), which a console keeps apart from the network service account's:
       the same one here, as there is no account behind either */
    return nn_account_baas_IManagerForApplication_GetAccountId(self, id);
}
/* where an account server would issue the title's token (g_hle.acc.token): there is none to ask, so the token stays
   none and the context succeeds, as rEden's with online off */
static int nn_account_baas_IManagerForApplication_EnsureIdTokenCacheAsync(SvcObj* self, SvcObj** context) {
    (void)self;
    *context = nn_account_detail_IAsyncContext_new();
    R_SUCCEED();
}
/* The cached token, as far as the buffer takes, and zeros to its end: the guest reads it back as a C string, which
   would run into what the buffer held before. With none, 4 answers a run of zeros of that length and 3 writes nothing
   and answers 0, as rEden keeps them apart (acc.h's IdTokenCacheBuffer: a game tells the two apart) */
static void acc_load_token(IpcBuf buffer, int pad_when_empty, u32* size) {
    const char* end = (const char*)memchr(g_hle.acc.token, 0, sizeof g_hle.acc.token);
    size_t n = end ? (size_t)(end - g_hle.acc.token) : sizeof g_hle.acc.token, cap = (size_t)buffer.size;
    if (!n) {
        n = pad_when_empty ? (cap < ACC_EMPTY_TOKEN ? cap : ACC_EMPTY_TOKEN) : 0;
        memset(GP(buffer.addr), 0, n);
        *size = (u32)n;
        return;
    }
    if (n > cap) n = cap;
    memcpy(GP(buffer.addr), g_hle.acc.token, n);
    memset(GP(buffer.addr + n), 0, cap - n);
    *size = (u32)n;
}
static int nn_account_baas_IManagerForApplication_LoadIdTokenCacheDeprecated(SvcObj* self, IpcBuf token, u32* size) {
    (void)self;
    acc_load_token(token, 0, size);
    R_SUCCEED();
}
static int nn_account_baas_IManagerForApplication_LoadIdTokenCache(SvcObj* self, IpcBuf token, u32* size) {
    (void)self;
    acc_load_token(token, 1, size);
    R_SUCCEED();
}
/* what a program does before ExecuteProgram: the open user kept for the next one (Eden's StoreOpenedUsers) */
static int nn_account_baas_IManagerForApplication_StoreOpenContext(SvcObj* self) {
    (void)self;
    g_hle.acc.context_stored = 1;
    R_SUCCEED();
}

/* ============================================================ IManagerForSystemService: the application's */
static int nn_account_baas_IManagerForSystemService_GetAccountId(SvcObj* self, nn_account_NetworkServiceAccountId* id) {
    return nn_account_baas_IManagerForApplication_GetAccountId(self, id);
}
static int nn_account_baas_IManagerForSystemService_EnsureIdTokenCacheAsync(SvcObj* self, SvcObj** context) {
    return nn_account_baas_IManagerForApplication_EnsureIdTokenCacheAsync(self, context);
}
static int nn_account_baas_IManagerForSystemService_LoadIdTokenCacheDeprecated(SvcObj* self, IpcBuf token, u32* size) {
    return nn_account_baas_IManagerForApplication_LoadIdTokenCacheDeprecated(self, token, size);
}
static int nn_account_baas_IManagerForSystemService_LoadIdTokenCache(SvcObj* self, IpcBuf token, u32* size) {
    return nn_account_baas_IManagerForApplication_LoadIdTokenCache(self, token, size);
}
static int nn_account_baas_IManagerForSystemService_RefreshNetworkServiceLicenseCacheAsync(SvcObj* self,
                                                                                           SvcObj** context) {
    (void)self;
    *context = nn_account_detail_IAsyncContext_new();
    R_SUCCEED();
}
/* no account server, so no license (rEden's with online off) */
static int nn_account_baas_IManagerForSystemService_GetNetworkServiceLicenseCacheEx(SvcObj* self, u32* license,
                                                                                    i64* expiration) {
    (void)self;
    *license = 0;
    *expiration = 0;
    R_SUCCEED();
}
/* the online account's user resource (130) and verification URL (133) caches, which the system's account pages read:
   the online account service is not there, which they show as not available right now (rEden's answer) */
static int acc_system_manager_other(SvcObj* self, IpcCtx* c) {
    if (c->cmd == 130 || c->cmd == 133) R_THROW(ACC_UNDER_MAINTENANCE);
    return stub_ok(self, c);
}

/* ============================================================ IAsyncContext: done at once, succeeded */
static int nn_account_detail_IAsyncContext_GetSystemEvent(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "acc async context", 1);
}
static int nn_account_detail_IAsyncContext_HasDone(SvcObj* self, u8* done) {
    (void)self;
    *done = 1;
    R_SUCCEED();
}
static int nn_account_detail_IAsyncContext_GetResult(SvcObj* self) {
    (void)self;
    R_SUCCEED();
}

/* ============================================================ IAccountServiceForSystemService: acc:u0's users */
static int nn_account_IAccountServiceForSystemService_GetUserCount(SvcObj* self, i32* count) {
    return nn_account_IAccountServiceForApplication_GetUserCount(self, count);
}
static int nn_account_IAccountServiceForSystemService_GetUserExistence(SvcObj* self, const nn_account_Uid* uid,
                                                                       u8* exists) {
    return nn_account_IAccountServiceForApplication_GetUserExistence(self, uid, exists);
}
static int nn_account_IAccountServiceForSystemService_ListAllUsers(SvcObj* self, IpcBuf users) {
    return nn_account_IAccountServiceForApplication_ListAllUsers(self, users);
}
static int nn_account_IAccountServiceForSystemService_ListOpenUsers(SvcObj* self, IpcBuf users) {
    return nn_account_IAccountServiceForApplication_ListOpenUsers(self, users);
}
static int nn_account_IAccountServiceForSystemService_GetLastOpenedUser(SvcObj* self, nn_account_Uid* uid) {
    return nn_account_IAccountServiceForApplication_GetLastOpenedUser(self, uid);
}
static int nn_account_IAccountServiceForSystemService_GetProfile(SvcObj* self, const nn_account_Uid* uid,
                                                                 SvcObj** profile) {
    return nn_account_IAccountServiceForApplication_GetProfile(self, uid, profile);
}
static int nn_account_IAccountServiceForSystemService_IsUserRegistrationRequestPermitted(SvcObj* self, u64 pid,
                                                                                         u8* permitted) {
    return nn_account_IAccountServiceForApplication_IsUserRegistrationRequestPermitted(self, pid, permitted);
}
static int nn_account_IAccountServiceForSystemService_TrySelectUserWithoutInteractionDeprecated(
    SvcObj* self, u8 is_network_service_account_required, nn_account_Uid* uid) {
    return nn_account_IAccountServiceForApplication_TrySelectUserWithoutInteractionDeprecated(
        self, is_network_service_account_required, uid);
}
static int nn_account_IAccountServiceForSystemService_TrySelectUserWithoutInteraction(
    SvcObj* self, u8 is_network_service_account_required, nn_account_Uid* uid) {
    return nn_account_IAccountServiceForApplication_TrySelectUserWithoutInteraction(
        self, is_network_service_account_required, uid);
}
static int nn_account_IAccountServiceForSystemService_ListOpenContextStoredUsersOld(SvcObj* self, IpcBuf users) {
    return nn_account_IAccountServiceForApplication_ListOpenContextStoredUsersOld(self, users);
}
static int nn_account_IAccountServiceForSystemService_StoreSaveDataThumbnail(SvcObj* self, const nn_account_Uid* uid,
                                                                             IpcBuf image) {
    return nn_account_IAccountServiceForApplication_StoreSaveDataThumbnail(self, uid, image);
}
/* the user's network service account, for the system: a user's only, as acc:u0's for the application */
static int nn_account_IAccountServiceForSystemService_GetBaasAccountManagerForSystemService(SvcObj* self,
                                                                                            const nn_account_Uid* uid,
                                                                                            SvcObj** manager) {
    (void)self;
    R_UNLESS(acc_user(uid), ACC_INVALID_USER_ID);
    *manager = acc_for_user(nn_account_baas_IManagerForSystemService_new_data(g_hle.acc.users, g_hle.acc.user_next,
                                                                              "acc profiles and managers"),
                            uid);
    R_SUCCEED();
}
/* 140 here, 141 in acc:u0 */
static int nn_account_IAccountServiceForSystemService_ListQualifiedUsers(SvcObj* self, IpcBuf users) {
    return nn_account_IAccountServiceForApplication_ListQualifiedUsers(self, users);
}
/* no pin code: the console's users here have none */
static int nn_account_IAccountServiceForSystemService_GetPinCodeLength(SvcObj* self, u32* length) {
    (void)self;
    *length = 0;
    R_SUCCEED();
}

/* ============================================================ IAccountServiceForAdministrator: acc:u1's and more */
static int nn_account_IAccountServiceForAdministrator_GetUserCount(SvcObj* self, i32* count) {
    return nn_account_IAccountServiceForApplication_GetUserCount(self, count);
}
static int nn_account_IAccountServiceForAdministrator_GetUserExistence(SvcObj* self, const nn_account_Uid* uid,
                                                                       u8* exists) {
    return nn_account_IAccountServiceForApplication_GetUserExistence(self, uid, exists);
}
static int nn_account_IAccountServiceForAdministrator_ListAllUsers(SvcObj* self, IpcBuf users) {
    return nn_account_IAccountServiceForApplication_ListAllUsers(self, users);
}
static int nn_account_IAccountServiceForAdministrator_ListOpenUsers(SvcObj* self, IpcBuf users) {
    return nn_account_IAccountServiceForApplication_ListOpenUsers(self, users);
}
static int nn_account_IAccountServiceForAdministrator_GetLastOpenedUser(SvcObj* self, nn_account_Uid* uid) {
    return nn_account_IAccountServiceForApplication_GetLastOpenedUser(self, uid);
}
static int nn_account_IAccountServiceForAdministrator_GetProfile(SvcObj* self, const nn_account_Uid* uid,
                                                                 SvcObj** profile) {
    return nn_account_IAccountServiceForApplication_GetProfile(self, uid, profile);
}
static int nn_account_IAccountServiceForAdministrator_IsUserRegistrationRequestPermitted(SvcObj* self, u64 pid,
                                                                                         u8* permitted) {
    return nn_account_IAccountServiceForApplication_IsUserRegistrationRequestPermitted(self, pid, permitted);
}
static int nn_account_IAccountServiceForAdministrator_TrySelectUserWithoutInteractionDeprecated(
    SvcObj* self, u8 is_network_service_account_required, nn_account_Uid* uid) {
    return nn_account_IAccountServiceForApplication_TrySelectUserWithoutInteractionDeprecated(
        self, is_network_service_account_required, uid);
}
static int nn_account_IAccountServiceForAdministrator_TrySelectUserWithoutInteraction(
    SvcObj* self, u8 is_network_service_account_required, nn_account_Uid* uid) {
    return nn_account_IAccountServiceForApplication_TrySelectUserWithoutInteraction(
        self, is_network_service_account_required, uid);
}
static int nn_account_IAccountServiceForAdministrator_ListOpenContextStoredUsersOld(SvcObj* self, IpcBuf users) {
    return nn_account_IAccountServiceForApplication_ListOpenContextStoredUsersOld(self, users);
}
static int nn_account_IAccountServiceForAdministrator_ListQualifiedUsers(SvcObj* self, IpcBuf users) {
    return nn_account_IAccountServiceForApplication_ListQualifiedUsers(self, users);
}
static int nn_account_IAccountServiceForAdministrator_GetBaasAccountManagerForSystemService(SvcObj* self,
                                                                                            const nn_account_Uid* uid,
                                                                                            SvcObj** manager) {
    return nn_account_IAccountServiceForSystemService_GetBaasAccountManagerForSystemService(self, uid, manager);
}
/* the system names the application the thumbnail is of: none is no application's */
static int nn_account_IAccountServiceForAdministrator_StoreSaveDataThumbnail(SvcObj* self, const nn_account_Uid* uid,
                                                                             u64 application_id, IpcBuf image) {
    R_UNLESS(application_id, ACC_INVALID_APPLICATION);
    return nn_account_IAccountServiceForApplication_StoreSaveDataThumbnail(self, uid, image);
}
static int nn_account_IAccountServiceForAdministrator_GetPinCodeLength(SvcObj* self, u32* length) {
    return nn_account_IAccountServiceForSystemService_GetPinCodeLength(self, length);
}

SvcObj* new_acc(void) { return nn_account_IAccountServiceForApplication_new(); }
SvcObj* new_acc_system(void) { return nn_account_IAccountServiceForSystemService_new(); }
SvcObj* new_acc_admin(void) { return nn_account_IAccountServiceForAdministrator_new(); }
