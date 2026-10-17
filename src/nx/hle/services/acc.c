/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/acc/acc.cpp, errors.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project (GPL-2.0-or-later) */
/* acc: the console's one user (g_user_id, am.c), open since the game started, with no network service account */
#include "nx/hle/services/services_int.h"

#include "hle/services/acc.swipc.inc"

#define ACC_INVALID_USER_ID RC(124, 20)
#define ACC_NOT_CONNECTED   RC(110, 300) /* nifm's: there is no network to reach the account server */
#define ACC_NICKNAME        "Player"
#define ACC_LAST_EDIT       1546300800 /* the user's profile was last changed on the first day of 2019 */

static int acc_is_user(const nn_account_Uid* uid) { return !memcmp(uid->v, g_user_id, 16); }

/* the user, then zeros (no user) in the rest of the list */
static void acc_list(IpcBuf users) {
    memset(GP(users.addr), 0, (size_t)users.size);
    if (users.size >= 16) memcpy(GP(users.addr), g_user_id, 16);
}

/* an object that keeps the user it was made for */
static SvcObj* acc_for_user(SvcObj* o, const nn_account_Uid* uid) {
    memcpy(o->data, uid->v, 16);
    return o;
}

/* ============================================================ IAccountServiceForApplication */
static int nn_account_IAccountServiceForApplication_GetUserCount(SvcObj* self, i32* count) {
    (void)self;
    *count = 1;
    return 0;
}
static int nn_account_IAccountServiceForApplication_GetUserExistence(SvcObj* self, const nn_account_Uid* uid,
                                                                    u8* exists) {
    (void)self;
    *exists = (u8)acc_is_user(uid);
    return 0;
}
static int nn_account_IAccountServiceForApplication_ListAllUsers(SvcObj* self, IpcBuf users) {
    (void)self;
    acc_list(users);
    return 0;
}
static int nn_account_IAccountServiceForApplication_ListOpenUsers(SvcObj* self, IpcBuf users) {
    (void)self;
    acc_list(users);
    return 0;
}
static int nn_account_IAccountServiceForApplication_GetLastOpenedUser(SvcObj* self, nn_account_Uid* uid) {
    (void)self;
    memcpy(uid->v, g_user_id, 16);
    return 0;
}
static int nn_account_IAccountServiceForApplication_GetProfile(SvcObj* self, const nn_account_Uid* uid,
                                                              SvcObj** profile) {
    (void)self;
    if (!acc_is_user(uid)) return ACC_INVALID_USER_ID;
    *profile = acc_for_user(nn_account_profile_IProfile_new(16), uid);
    return 0;
}
/* with one user, that user, chosen without asking */
static int nn_account_IAccountServiceForApplication_TrySelectUserWithoutInteraction(
    SvcObj* self, u8 is_network_service_account_required, nn_account_Uid* uid) {
    (void)self, (void)is_network_service_account_required;
    memcpy(uid->v, g_user_id, 16);
    return 0;
}
static int nn_account_IAccountServiceForApplication_GetBaasAccountManagerForApplication(SvcObj* self,
                                                                                       const nn_account_Uid* uid,
                                                                                       SvcObj** manager) {
    (void)self;
    if (!acc_is_user(uid)) return ACC_INVALID_USER_ID;
    *manager = acc_for_user(nn_account_baas_IManagerForApplication_new(16), uid);
    return 0;
}
static int nn_account_IAccountServiceForApplication_LoadOpenContext(SvcObj* self, const nn_account_Uid* uid,
                                                                   SvcObj** manager) {
    return nn_account_IAccountServiceForApplication_GetBaasAccountManagerForApplication(self, uid, manager);
}
static int nn_account_IAccountServiceForApplication_ListOpenContextStoredUsers(SvcObj* self, IpcBuf users) {
    (void)self;
    acc_list(users);
    return 0;
}
static int nn_account_IAccountServiceForApplication_ListQualifiedUsers(SvcObj* self, IpcBuf users) {
    (void)self;
    acc_list(users);
    return 0;
}

/* ============================================================ IProfile */
static void acc_profile_base(SvcObj* self, nn_account_profile_ProfileBase* base) {
    memcpy(base->uid.v, self->data, 16);
    base->last_edit_timestamp = ACC_LAST_EDIT;
    memcpy(base->nickname, ACC_NICKNAME, sizeof ACC_NICKNAME);
}
/* the user data (icon, colors) is all zeros: the default icon */
static int nn_account_profile_IProfile_Get(SvcObj* self, IpcBuf user_data, nn_account_profile_ProfileBase* base) {
    memset(GP(user_data.addr), 0, (size_t)user_data.size);
    acc_profile_base(self, base);
    return 0;
}
static int nn_account_profile_IProfile_GetBase(SvcObj* self, nn_account_profile_ProfileBase* base) {
    acc_profile_base(self, base);
    return 0;
}

/* ============================================================ IManagerForApplication */
/* the id the account would have, made from the user's */
static int nn_account_baas_IManagerForApplication_GetAccountId(SvcObj* self, nn_account_NetworkServiceAccountId* id) {
    u64 a, b;
    memcpy(&a, self->data, 8);
    memcpy(&b, (u8*)self->data + 8, 8);
    *id = a ^ b;
    return 0;
}
static int nn_account_baas_IManagerForApplication_EnsureIdTokenCacheAsync(SvcObj* self, SvcObj** context) {
    (void)self;
    *context = nn_account_detail_IAsyncContext_new(0);
    return 0;
}

/* ============================================================ IAsyncContext: done at once, failed */
static int nn_account_detail_IAsyncContext_GetSystemEvent(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "acc async context", 1);
}
static int nn_account_detail_IAsyncContext_HasDone(SvcObj* self, u8* done) {
    (void)self;
    *done = 1;
    return 0;
}
static int nn_account_detail_IAsyncContext_GetResult(SvcObj* self) {
    (void)self;
    return ACC_NOT_CONNECTED;
}

SvcObj* new_acc(void) { return nn_account_IAccountServiceForApplication_new(0); }
