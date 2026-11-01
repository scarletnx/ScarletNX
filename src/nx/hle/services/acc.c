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
#include "nx/hle/filesystem/fs.h"
#include <openssl/rand.h>
#include <time.h>

#include "hle/services/acc.swipc.inc"

static int acc_profile_dispatch(SvcObj* o, IpcCtx* c);
static SvcObj* acc_with(SvcObj* o, int (*dispatch)(SvcObj*, IpcCtx*));

#define ACC_INVALID_USER_ID RC(124, 20)
#define ACC_INVALID_APPLICATION RC(124, 22)
#define ACC_INVALID_LENGTH  RC(124, 32)
#define ACC_UPDATE_FAILED       RC(124, 100) /* rEden's ResultAccountUpdateFailed: the users could not be written */
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
    *profile = acc_with(acc_for_user(nn_account_profile_IProfile_new_data(g_hle.acc.users, g_hle.acc.user_next,
                                                                          "acc profiles and managers"),
                                     uid),
                        acc_profile_dispatch);
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
    return snx_users_find(((const AccUser*)self->data)->uid); /* NULL for one acc:su deleted since */
}
static void acc_profile_base(SvcObj* self, nn_account_profile_ProfileBase* base) {
    const SnxUser* u = acc_profile_user(self); /* there: Get and GetBase see to it */
    memset(base, 0, sizeof *base);
    acc_uid_put(base->uid.v, ((const AccUser*)self->data)->uid);
    base->last_edit_timestamp = ACC_LAST_EDIT;
    memcpy(base->nickname, u->nickname, strlen(u->nickname)); /* 32 bytes at most: the field's, with no NUL then */
}
/* the user data (icon, colours, Mii) a program stored, users.ini's user_data: zeros, the default icon, without one */
static int nn_account_profile_IProfile_Get(SvcObj* self, IpcBuf user_data, nn_account_profile_ProfileBase* base) {
    const SnxUser* u = acc_profile_user(self);
    R_UNLESS(u, ACC_INVALID_USER_ID); /* deleted since (acc:su 203): rEden answers its profile manager's failure */
    memcpy(GP(user_data.addr), u->user_data,
           (size_t)(user_data.size < sizeof u->user_data ? user_data.size : sizeof u->user_data));
    acc_profile_base(self, base);
    R_SUCCEED();
}
static int nn_account_profile_IProfile_GetBase(SvcObj* self, nn_account_profile_ProfileBase* base) {
    R_UNLESS(acc_profile_user(self), ACC_INVALID_USER_ID);
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
    const SnxUser* u = acc_profile_user(self);
    if (!u || !snx_users_picture(u, size)) *size = acc_picture(); /* a user deleted since: the default, as rEden */
    R_SUCCEED();
}
static int nn_account_profile_IProfile_LoadImage(SvcObj* self, IpcBuf image, u32* size) {
    const SnxUser* u = acc_profile_user(self);
    const unsigned char* jpeg = u ? snx_users_picture(u, size) : NULL;
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

/* ============================================================ IAdministrator: the system manager's license */
static int nn_account_baas_IAdministrator_RefreshNetworkServiceLicenseCacheAsync(SvcObj* self, SvcObj** context) {
    return nn_account_baas_IManagerForSystemService_RefreshNetworkServiceLicenseCacheAsync(self, context);
}
static int nn_account_baas_IAdministrator_GetNetworkServiceLicenseCacheEx(SvcObj* self, u32* license, i64* expiration) {
    return nn_account_baas_IManagerForSystemService_GetNetworkServiceLicenseCacheEx(self, license, expiration);
}
/* no online account behind the user: the pages that need one say so */
static int nn_account_baas_IAdministrator_IsLinkedWithNintendoAccount(SvcObj* self, u8* linked) {
    (void)self;
    *linked = 0;
    R_SUCCEED();
}
/* the online account's page (290, GetRequestForNintendoAccountUserResourceView): not available right now, as rEden */
static int acc_administrator_other(SvcObj* self, IpcCtx* c) {
    if (c->cmd == 290) R_THROW(ACC_UNDER_MAINTENANCE);
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
/* the user's network service account, for the administrator: a user's only */
static int nn_account_IAccountServiceForAdministrator_GetBaasAccountAdministrator(SvcObj* self,
                                                                                  const nn_account_Uid* uid,
                                                                                  SvcObj** administrator) {
    (void)self;
    R_UNLESS(acc_user(uid), ACC_INVALID_USER_ID);
    *administrator = acc_for_user(
        nn_account_baas_IAdministrator_new_data(g_hle.acc.users, g_hle.acc.user_next, "acc profiles and managers"),
        uid);
    R_SUCCEED();
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

/* a user registered (SwitchBrew acc:su 200): a uid of random bits, an RFC 4122 version 4 UUID's as the launcher's
   setup makes them (setup.c's snx_setup_uid), in the users from now and written by Complete */
static int nn_account_IAccountServiceForAdministrator_BeginUserRegistration(SvcObj* self, nn_account_Uid* uid) {
    unsigned char bytes[16];
    char why[256];
    u64 v[2];
    (void)self;
    do {
        if (RAND_bytes(bytes, sizeof bytes) != 1) fatal("The host's random number generator failed");
        v[0] = (snx_ld_be64(bytes) & ~(u64)0xF000) | 0x4000;
        v[1] = (snx_ld_be64(bytes + 8) & ~((u64)3 << 62)) | ((u64)2 << 62);
    } while (snx_users_find(v));
    if (snx_users_begin(v, why, sizeof why)) {
        WARN("acc: no user registered: %s\n", why);
        R_THROW(ACC_UPDATE_FAILED);
    }
    acc_uid_put(uid->v, v);
    R_SUCCEED();
}
/* the user registered, written to users.ini (and those registered before it); one there already changes nothing */
static int nn_account_IAccountServiceForAdministrator_CompleteUserRegistration(SvcObj* self,
                                                                               const nn_account_Uid* uid) {
    const SnxUser* u = acc_user(uid);
    char why[512];
    (void)self;
    R_UNLESS(u, ACC_INVALID_USER_ID);
    if (!g_host.acc.users_dir[0]) {
        WARN("acc: the user registered is not written: there is no users folder\n");
        R_THROW(ACC_UPDATE_FAILED);
    }
    if (snx_users_save(g_host.acc.users_dir, (int)(u - g_users) + 1, why, sizeof why)) {
        WARN("acc: the user registered is not written: %s\n", why);
        R_THROW(ACC_UPDATE_FAILED);
    }
    R_SUCCEED();
}

/* path moved to to; done[] keeps it to be put back */
static int acc_trash_move(const char* path, const char* to, char (*done)[2][FS_TREE_PATH_MAX], int* moved) {
    if (snx_replace(path, to)) return -1;
    snprintf(done[*moved][0], FS_TREE_PATH_MAX, "%s", path);
    snprintf(done[*moved][1], FS_TREE_PATH_MAX, "%s", to);
    ++*moved;
    return 0;
}
/* A user deleted (SwitchBrew acc:su 203), as rEden's Core::Profiles::DeleteUser does it: into the Trash, trash/ of the
   data folder, a folder of its own (user.<uid>.<UTC time>, a number after it rather than over one there): its
   users.ini section, its picture and the folders of its saves this run reaches (fs_user_saves_take), each said where
   it went; then the users after it a place up. The user playing is refused, as rEden refuses it in its frontends; a
   move that fails puts back what moved and deletes nothing */
static int nn_account_IAccountServiceForAdministrator_DeleteUser(SvcObj* self, const nn_account_Uid* uid) {
    const SnxUser* u = acc_user(uid);
    char hex[33], when[32], why[512], picture[FS_TREE_PATH_MAX], saves[2][FS_TREE_PATH_MAX],
        done[3][2][FS_TREE_PATH_MAX];
    char *data, *trash = NULL, *section = NULL, *place;
    time_t now = time(NULL);
    const struct tm* utc = gmtime(&now);
    int number, moved = 0, n, i, rc = ACC_UPDATE_FAILED;
    (void)self;
    R_UNLESS(u, ACC_INVALID_USER_ID);
    number = (int)(u - g_users) + 1;
    if (u == snx_users_selected()) {
        WARN("acc: user %d is not deleted: it is the one playing\n", number);
        R_THROW(ACC_UPDATE_FAILED);
    }
    if (!g_host.acc.users_dir[0]) {
        WARN("acc: user %d is not deleted: there is no users folder\n", number);
        R_THROW(ACC_UPDATE_FAILED);
    }
    snx_users_uid_text(u->uid, hex);
    data = snx_dirname(g_host.acc.users_dir);
    if (!utc || !strftime(when, sizeof when, "%Y%m%d-%H%M%S", utc)) snprintf(when, sizeof when, "%lld", (long long)now);
    for (i = 1; i < 100 && !trash; i++) {
        place = i == 1 ? snx_format("%s/trash/user.%s.%s", data, hex, when)
                       : snx_format("%s/trash/user.%s.%s-%d", data, hex, when, i);
        if (place && !snx_path_exists(place))
            trash = place;
        else
            free(place);
    }
    if (!trash || snx_mkdirs(trash)) {
        WARN("acc: user %d is not deleted: no folder in %s/trash could be made\n", number, data);
        goto done;
    }
    section = snx_format("[user%d]\nuid=%s\nnickname=%s\n", number, hex, u->nickname);
    place = snx_path_join(trash, "users.ini");
    n = !section || !place || snx_write_file(place, section, strlen(section));
    free(place);
    if (n) {
        WARN("acc: user %d is not deleted: %s/users.ini could not be written\n", number, trash);
        goto done;
    }
    snprintf(picture, sizeof picture, "%s/%s.jpg", g_host.acc.users_dir, hex);
    if (snx_is_file(picture)) {
        place = snx_format("%s/%s.jpg", trash, hex);
        n = !place || acc_trash_move(picture, place, done, &moved);
        free(place);
        if (n) {
            WARN("acc: user %d is not deleted: %s could not be moved to %s\n", number, picture, trash);
            goto back;
        }
    }
    n = fs_user_saves_take(u->uid, saves, 2);
    for (i = 0; i < n; i++) {
        place = snx_format("%s/save.%d", trash, i + 1);
        if (!place || acc_trash_move(saves[i], place, done, &moved)) {
            WARN("acc: user %d is not deleted: %s could not be moved to %s; the game sees those saves again at its "
                 "next start\n",
                 number, saves[i], place ? place : trash);
            free(place);
            goto back;
        }
        free(place);
    }
    if (snx_users_remove(g_host.acc.users_dir, number, why, sizeof why)) {
        WARN("acc: user %d is not deleted: %s\n", number, why);
        goto back;
    }
    for (i = 0; i < moved; i++) LOG("acc: %s is in the Trash now, %s\n", done[i][0], done[i][1]);
    LOG("acc: user %d is deleted, into %s\n", number, trash);
    rc = 0;
    goto done;
back:
    for (i = moved - 1; i >= 0; i--)
        if (snx_replace(done[i][1], done[i][0]))
            WARN("acc: %s could not be put back from %s\n", done[i][0], done[i][1]);
done:
    if (rc && trash) { /* the Trash's folder made for it, with what went in it first */
        place = snx_path_join(trash, "users.ini");
        if (place) remove(place);
        free(place);
        snx_remove_empty_dir(trash);
    }
    free(section);
    free(trash);
    free(data);
    return rc;
}
/* A user moved to place position, 0 the first (SwitchBrew acc:su 204), the users between a place over, the one playing
   the same user. A place past the users or a uid no user has changes nothing and succeeds, as rEden's ProfileManager
   SetUserPosition does, said in the log; users.ini that can't be written is a failure */
static int nn_account_IAccountServiceForAdministrator_SetUserPosition(SvcObj* self, u64 position,
                                                                      const nn_account_Uid* uid) {
    const SnxUser* u = acc_user(uid);
    char why[512], hex[33];
    u64 v[2];
    int number;
    (void)self;
    if (!u) {
        acc_uid_get(uid, v);
        snx_users_uid_text(v, hex);
        WARN("acc: no user moved: uid %s is no user's\n", hex);
        R_SUCCEED();
    }
    if (position >= (u64)g_user_count) {
        WARN("acc: no user moved: place %llu is past the %d users\n", (unsigned long long)position, g_user_count);
        R_SUCCEED();
    }
    number = (int)(u - g_users) + 1;
    if (!g_host.acc.users_dir[0] || snx_users_move(g_host.acc.users_dir, number, (int)position + 1, why, sizeof why)) {
        WARN("acc: user %d is not moved to place %llu: %s\n", number, (unsigned long long)position,
             g_host.acc.users_dir[0] ? why : "there is no users folder");
        R_THROW(ACC_UPDATE_FAILED);
    }
    R_SUCCEED();
}
/* a user's profile to change (SwitchBrew acc:su 205): a user's only, a registered one's too */
static int acc_editor_dispatch(SvcObj* o, IpcCtx* c);
static int nn_account_IAccountServiceForAdministrator_GetProfileEditor(SvcObj* self, const nn_account_Uid* uid,
                                                                       SvcObj** editor) {
    (void)self;
    R_UNLESS(acc_user(uid), ACC_INVALID_USER_ID);
    *editor = acc_with(acc_for_user(nn_account_profile_IProfileEditor_new_data(g_hle.acc.users, g_hle.acc.user_next,
                                                                               "acc profiles and managers"),
                                    uid),
                       acc_editor_dispatch);
    R_SUCCEED();
}
/* ============================================================ IProfileEditor: IProfile's, and the profile stored */
static int nn_account_profile_IProfileEditor_Get(SvcObj* self, IpcBuf user_data, nn_account_profile_ProfileBase* base) {
    return nn_account_profile_IProfile_Get(self, user_data, base);
}
static int nn_account_profile_IProfileEditor_GetBase(SvcObj* self, nn_account_profile_ProfileBase* base) {
    return nn_account_profile_IProfile_GetBase(self, base);
}
static int nn_account_profile_IProfileEditor_GetImageSize(SvcObj* self, u32* size) {
    return nn_account_profile_IProfile_GetImageSize(self, size);
}
static int nn_account_profile_IProfileEditor_LoadImage(SvcObj* self, IpcBuf image, u32* size) {
    return nn_account_profile_IProfile_LoadImage(self, image, size);
}
/* The profile a program stores, written as it gives it: the nickname of the base's 32 bytes (strncpy's, as SwitchBrew's
   ProfileBase says), which the service's rule takes (users.c), the UserData's 0x80 bytes, and with an image, the
   picture. A user only registered goes into users.ini with it, as rEden's ProfileManager writes profiles.dat at a
   Store. A base of another user's uid is refused, where rEden takes the uid too: the user would lose its saves and
   picture, which go by uid. The timestamp is not kept: users.ini has no key for it */
static int acc_store(SvcObj* self, const nn_account_profile_ProfileBase* base, IpcBuf user_data, const IpcBuf* image) {
    const AccUser* who = (const AccUser*)self->data;
    const SnxUser* u = snx_users_find(who->uid);
    char nickname[SNX_USERS_NICKNAME + 1], why[512];
    unsigned char data[SNX_USERS_DATA];
    u64 uid[2];
    int number;
    R_UNLESS(u, ACC_INVALID_USER_ID);
    R_UNLESS(user_data.size >= SNX_USERS_DATA, ACC_INVALID_LENGTH);
    R_UNLESS(!image || image->size <= SNX_ACC_PICTURE_MAX, ACC_INVALID_LENGTH);
    acc_uid_get(&base->uid, uid);
    R_UNLESS(uid[0] == who->uid[0] && uid[1] == who->uid[1], ACC_INVALID_USER_ID);
    memcpy(nickname, base->nickname, SNX_USERS_NICKNAME);
    nickname[SNX_USERS_NICKNAME] = 0;
    memcpy(data, GP(user_data.addr), sizeof data);
    number = (int)(u - g_users) + 1;
    if (!g_host.acc.users_dir[0]) {
        WARN("acc: the profile of user %d is not stored: there is no users folder\n", number);
        R_THROW(ACC_UPDATE_FAILED);
    }
    if ((strcmp(nickname, u->nickname) && snx_users_rename(g_host.acc.users_dir, number, nickname, why, sizeof why)) ||
        (memcmp(data, u->user_data, sizeof data) &&
         snx_users_set_data(g_host.acc.users_dir, number, data, why, sizeof why)) ||
        (image && snx_users_set_picture(g_host.acc.users_dir, number, GP(image->addr), (uint32_t)image->size, why,
                                        sizeof why)) ||
        snx_users_save(g_host.acc.users_dir, number, why, sizeof why)) {
        WARN("acc: the profile of user %d is not stored: %s\n", number, why);
        R_THROW(ACC_UPDATE_FAILED);
    }
    R_SUCCEED();
}
static int nn_account_profile_IProfileEditor_Store(SvcObj* self, const nn_account_profile_ProfileBase* base,
                                                   IpcBuf user_data) {
    return acc_store(self, base, user_data, NULL);
}
static int nn_account_profile_IProfileEditor_StoreWithImage(SvcObj* self, const nn_account_profile_ProfileBase* base,
                                                            IpcBuf user_data, IpcBuf image) {
    return acc_store(self, base, user_data, &image);
}

void acc_playing(u64 uid[2], char* nickname, size_t size) {
    const SnxUser* u;
    pm_lock(&g_host.acc.users_lk);
    u = snx_users_selected();
    if (uid) {
        uid[0] = u->uid[0];
        uid[1] = u->uid[1];
    }
    if (size) snprintf(nickname, size, "%s", u->nickname);
    pm_unlock(&g_host.acc.users_lk);
}
/* every command of an object that reads or changes the users, under g_host.acc.users_lk: acc:su changes them while the
   game's threads read them */
static int acc_locked(SvcObj* o, IpcCtx* c, int (*dispatch)(SvcObj*, IpcCtx*)) {
    int rc;
    pm_lock(&g_host.acc.users_lk);
    rc = dispatch(o, c);
    pm_unlock(&g_host.acc.users_lk);
    return rc;
}
static int acc_u0_dispatch(SvcObj* o, IpcCtx* c) {
    return acc_locked(o, c, nn_account_IAccountServiceForApplication_dispatch);
}
static int acc_u1_dispatch(SvcObj* o, IpcCtx* c) {
    return acc_locked(o, c, nn_account_IAccountServiceForSystemService_dispatch);
}
static int acc_su_dispatch(SvcObj* o, IpcCtx* c) {
    return acc_locked(o, c, nn_account_IAccountServiceForAdministrator_dispatch);
}
static int acc_profile_dispatch(SvcObj* o, IpcCtx* c) { return acc_locked(o, c, nn_account_profile_IProfile_dispatch); }
static int acc_editor_dispatch(SvcObj* o, IpcCtx* c) {
    return acc_locked(o, c, nn_account_profile_IProfileEditor_dispatch);
}
static SvcObj* acc_with(SvcObj* o, int (*dispatch)(SvcObj*, IpcCtx*)) {
    o->dispatch = dispatch;
    return o;
}
SvcObj* new_acc(void) { return acc_with(nn_account_IAccountServiceForApplication_new(), acc_u0_dispatch); }
SvcObj* new_acc_system(void) { return acc_with(nn_account_IAccountServiceForSystemService_new(), acc_u1_dispatch); }
SvcObj* new_acc_admin(void) { return acc_with(nn_account_IAccountServiceForAdministrator_new(), acc_su_dispatch); }
