/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/apm/apm_controller.cpp, apm_controller.h, mm/mm_u.cpp,
   nfc/nfc_result.h, nfc/nfc_types.h, nfc/common/device_manager.cpp, nfc/nfc_interface.cpp, bcat/bcat_types.h,
   bcat/bcat_result.h, bcat/backend/backend.cpp, bcat/delivery_cache_file_service.cpp,
   aoc/purchase_event_manager.cpp, src/core/file_sys/common_funcs.h, mii/mii_result.h, mii/types/store_data.cpp,
   mii/types/char_info.h, mii/types/char_info.cpp, mii/types/core_data.cpp, mii/mii.cpp, mii/mii_manager.cpp,
   ptm/psm.cpp, lm/lm.cpp, audio/errors.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018, 2019, 2020, 2022, 2023 yuzu Emulator Project (GPL-2.0-or-later)
   and the default Miis and the random Miis' tables (mii_random.inc) from Eden's
   src/core/hle/service/mii/types/raw_data.cpp:
   SPDX-FileCopyrightText: Ryujinx Team and Contributors (MIT) */
/* The smaller services: apm, lm, pctl, friend, ns, prepo, caps, ectx, psm, fatal, csrng, spl, mm, nfp, nfc, bcat,
   aoc, irs, mii, btm and audin, each as a console offline, with no NFC device or microphone, and the add-on
   content the data folder holds */
#include "nx/hle/services/services_int.h"
#include "nx/hle/hle.h"
#include "nx/hle/filesystem/fs.h"
#include <openssl/rand.h>
#include <stdio.h>

#include "hle/services/misc.swipc.inc"

/* ============================================================ apm */
/* nn::apm's sixteen performance configurations, Config1 to Config16: a clock setting for the CPU, GPU and memory */
static const u32 apm_known[] = {0x00010000u, 0x00010001u, 0x00010002u, 0x00020000u, 0x00020001u, 0x00020002u,
                                0x00020003u, 0x00020004u, 0x00020005u, 0x00020006u, 0x92220007u, 0x92220008u,
                                0x92220009u, 0x9222000Au, 0x9222000Bu, 0x9222000Cu};
/* each mode's configuration, as a session or SetCpuBoostMode last set it: g_hle.misc.apm_config */

u32 apm_configuration(u32 mode) { return mode < APM_MODES ? g_hle.misc.apm_config[mode] : APM_CONFIGURATION; }
/* a configuration nn::apm doesn't have, or a mode, is not kept: the clocks stay as they were */
void apm_set_configuration(u32 mode, u32 config) {
    size_t i;
    for (i = 0; i < sizeof apm_known / sizeof apm_known[0]; i++)
        if (apm_known[i] == config) break;
    if (mode >= APM_MODES || i == sizeof apm_known / sizeof apm_known[0]) {
        WARN("apm: performance configuration 0x%08x for mode %u is not one, kept 0x%08x\n", config, mode,
             apm_configuration(mode));
        return;
    }
    g_hle.misc.apm_config[mode] = config;
}
/* nn::oe::CpuBoostMode (am's SetCpuBoostMode): Normal, FastLoad and its third mode give Boost mode Config7, Config13
   and Config15 */
void apm_cpu_boost(u32 boost) {
    static const u32 boosted[] = {0x00020003u, 0x92220009u, 0x9222000Bu};
    if (boost >= sizeof boosted / sizeof boosted[0]) {
        WARN("apm: CPU boost mode %u is not one, Boost mode kept at 0x%08x\n", boost, apm_configuration(1));
        return;
    }
    apm_set_configuration(1, boosted[boost]);
}

static int nn_apm_IManager_OpenSession(SvcObj* self, SvcObj** session) {
    (void)self;
    *session = nn_apm_ISession_new();
    R_SUCCEED();
}
/* Boost when docked, Normal in handheld mode */
static int nn_apm_IManager_GetPerformanceMode(SvcObj* self, nn_apm_PerformanceMode* mode) {
    (void)self;
    *mode = g_settings.tv_mode ? 1 : 0;
    R_SUCCEED();
}
static int nn_apm_ISession_SetPerformanceConfiguration(SvcObj* self, nn_apm_PerformanceMode mode,
                                                       nn_apm_PerformanceConfiguration config) {
    (void)self;
    apm_set_configuration(mode, config);
    R_SUCCEED();
}
static int nn_apm_ISession_GetPerformanceConfiguration(SvcObj* self, nn_apm_PerformanceMode mode,
                                                       nn_apm_PerformanceConfiguration* config) {
    (void)self;
    *config = apm_configuration(mode);
    R_SUCCEED();
}

/* ============================================================ lm */
static int nn_lm_ILogService_OpenLogger(SvcObj* self, u64 pid, SvcObj** logger) {
    (void)self, (void)pid;
    *logger = nn_lm_ILogger_new();
    R_SUCCEED();
}
/* a log packet: a 0x18 byte header (its payload's size at 0x14), then fields of a type byte, a ULEB128 size and
   the data (nn::lm's LogDataChunkKey). The text (2, every such field in order, up to LM_TEXT bytes), with the file
   (4) and line (3) when there are some, goes to the log */
#define LM_TEXT 1000
static int nn_lm_ILogger_Log(SvcObj* self, IpcBuf message) {
    const u8 *p = GP(message.addr), *end;
    const u8* file = NULL;
    char text[LM_TEXT];
    u32 payload, text_n = 0, file_n = 0, line = 0;
    (void)self;
    if (message.size < 0x18) R_SUCCEED();
    payload = snx_ld_le32(p + 0x14);
    end = p + 0x18 + (payload < message.size - 0x18 ? payload : message.size - 0x18);
    p += 0x18;
    while (p < end) {
        u8 type = *p++;
        u64 n = 0;
        int shift = 0;
        while (p < end && shift < 35) {
            n |= (u64)(*p & 0x7f) << shift;
            shift += 7;
            if (!(*p++ & 0x80)) break;
        }
        if (n > (u64)(end - p)) break;
        if (type == 2) {
            u32 k = n < (u64)(LM_TEXT - text_n) ? (u32)n : LM_TEXT - text_n;
            memcpy(text + text_n, p, k);
            text_n += k;
        } else if (type == 4) {
            file = p;
            file_n = (u32)n;
        } else if (type == 3 && n >= 4)
            line = snx_ld_le32(p);
        p += n;
    }
    while (text_n && (text[text_n - 1] == '\n' || !text[text_n - 1])) text_n--;
    if (text_n) {
        if (file_n)
            LOG("[lm] %.*s (%.*s:%u)\n", (int)text_n, text, (int)file_n, (const char*)file, line);
        else
            LOG("[lm] %.*s\n", (int)text_n, text);
    }
    R_SUCCEED();
}

/* ============================================================ pctl: no restrictions set */
static int nn_pctl_detail_ipc_IParentalControlServiceFactory_CreateService(SvcObj* self, u64 pid, SvcObj** service) {
    (void)self, (void)pid;
    *service = nn_pctl_detail_ipc_IParentalControlService_new();
    R_SUCCEED();
}
static int nn_pctl_detail_ipc_IParentalControlServiceFactory_CreateServiceWithoutInitialize(SvcObj* self, u64 pid,
                                                                                         SvcObj** service) {
    return nn_pctl_detail_ipc_IParentalControlServiceFactory_CreateService(self, pid, service);
}
static int nn_pctl_detail_ipc_IParentalControlService_IsStereoVisionPermitted(SvcObj* self, u8* permitted) {
    (void)self;
    *permitted = 1;
    R_SUCCEED();
}
static int nn_pctl_detail_ipc_IParentalControlService_GetSynchronizationEvent(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "pctl synchronization", 0);
}
static int nn_pctl_detail_ipc_IParentalControlService_GetPlayTimerEventToRequestSuspension(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "pctl play timer", 0);
}
static int nn_pctl_detail_ipc_IParentalControlService_GetUnlinkedEvent(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "pctl unlinked", 0);
}

/* ============================================================ friend */
#define FRIEND_NO_NOTIFICATIONS RC(124, 15) /* account's NoNotifications, which friends answers too */

static int nn_friends_detail_ipc_IServiceCreator_CreateFriendService(SvcObj* self, SvcObj** service) {
    (void)self;
    *service = nn_friends_detail_ipc_IFriendService_new();
    R_SUCCEED();
}
static int nn_friends_detail_ipc_IServiceCreator_CreateNotificationService(SvcObj* self, const nn_account_Uid* uid,
                                                                         SvcObj** service) {
    (void)self, (void)uid;
    *service = nn_friends_detail_ipc_INotificationService_new();
    R_SUCCEED();
}
static int nn_friends_detail_ipc_IServiceCreator_CreateDaemonSuspendSessionService(SvcObj* self, SvcObj** service) {
    (void)self;
    *service = nn_friends_detail_ipc_IDaemonSuspendSessionService_new();
    R_SUCCEED();
}
static int nn_friends_detail_ipc_IFriendService_GetCompletionEvent(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "friend completion", 0);
}
static int nn_friends_detail_ipc_INotificationService_GetEvent(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "friend notification", 0);
}
static int nn_friends_detail_ipc_INotificationService_Pop(SvcObj* self,
                                                         nn_friends_detail_ipc_SizedNotificationInfo* info) {
    (void)self, (void)info;
    R_THROW(FRIEND_NO_NOTIFICATIONS);
}

/* ============================================================ ns */
static int nn_ns_detail_IServiceGetterInterface_GetDynamicRightsInterface(SvcObj* self, SvcObj** iface) {
    (void)self;
    *iface = nn_ns_detail_IDynamicRightsInterface_new();
    R_SUCCEED();
}
static int nn_ns_detail_IServiceGetterInterface_GetReadOnlyApplicationControlDataInterface(SvcObj* self,
                                                                                          SvcObj** iface) {
    (void)self;
    *iface = nn_ns_detail_IReadOnlyApplicationControlDataInterface_new();
    R_SUCCEED();
}
static int nn_ns_detail_IServiceGetterInterface_GetReadOnlyApplicationRecordInterface(SvcObj* self, SvcObj** iface) {
    (void)self;
    *iface = nn_ns_detail_IReadOnlyApplicationRecordInterface_new();
    R_SUCCEED();
}
static int nn_ns_detail_IServiceGetterInterface_GetECommerceInterface(SvcObj* self, SvcObj** iface) {
    (void)self;
    *iface = nn_ns_detail_IECommerceInterface_new();
    R_SUCCEED();
}
static int nn_ns_detail_IServiceGetterInterface_GetApplicationVersionInterface(SvcObj* self, SvcObj** iface) {
    (void)self;
    *iface = nn_ns_detail_IApplicationVersionInterface_new();
    R_SUCCEED();
}
static int nn_ns_detail_IServiceGetterInterface_GetFactoryResetInterface(SvcObj* self, SvcObj** iface) {
    (void)self;
    *iface = nn_ns_detail_IFactoryResetInterface_new();
    R_SUCCEED();
}
static int nn_ns_detail_IServiceGetterInterface_GetAccountProxyInterface(SvcObj* self, SvcObj** iface) {
    (void)self;
    *iface = nn_ns_detail_IAccountProxyInterface_new();
    R_SUCCEED();
}
static int nn_ns_detail_IServiceGetterInterface_GetApplicationManagerInterface(SvcObj* self, SvcObj** iface) {
    (void)self;
    *iface = nn_ns_detail_IApplicationManagerInterface_new();
    R_SUCCEED();
}
static int nn_ns_detail_IServiceGetterInterface_GetDownloadTaskInterface(SvcObj* self, SvcObj** iface) {
    (void)self;
    *iface = nn_ns_detail_IDownloadTaskInterface_new();
    R_SUCCEED();
}
static int nn_ns_detail_IServiceGetterInterface_GetContentManagementInterface(SvcObj* self, SvcObj** iface) {
    (void)self;
    *iface = nn_ns_detail_IContentManagementInterface_new();
    R_SUCCEED();
}
static int nn_ns_detail_IServiceGetterInterface_GetDocumentInterface(SvcObj* self, SvcObj** iface) {
    (void)self;
    *iface = nn_ns_detail_IDocumentInterface_new();
    R_SUCCEED();
}

/* ============================================================ ectx */
static int nn_err_context_IWriterForApplication_CreateContextRegistrar(SvcObj* self, SvcObj** registrar) {
    (void)self;
    *registrar = nn_err_context_IContextRegistrar_new();
    R_SUCCEED();
}

/* ============================================================ psm: a full battery, on the charger */
static int nn_psm_IPsmServer_GetBatteryChargePercentage(SvcObj* self, u32* percentage) {
    (void)self;
    *percentage = 100;
    R_SUCCEED();
}
static int nn_psm_IPsmServer_GetChargerType(SvcObj* self, nn_psm_ChargerType* charger_type) {
    (void)self;
    *charger_type = 1; /* RegularCharger */
    R_SUCCEED();
}
static int nn_psm_IPsmServer_OpenSession(SvcObj* self, SvcObj** session) {
    (void)self;
    *session = nn_psm_IPsmSession_new();
    R_SUCCEED();
}
static int nn_psm_IPsmSession_BindStateChangeEvent(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "psm state change", 0);
}

/* ============================================================ fatal:u */
/* the result as the console's error screen shows it: 2000 plus the module, and the description */
static void throw_fatal(u32 result, u32 policy, const char* how) {
    fatal("The game stopped with error %04u-%04u (result 0x%x, policy %u%s)", 2000 + (result & 0x1ff),
          (result >> 9) & 0x1fff, result, policy, how);
}
static int nn_fatalsrv_IService_ThrowFatal(SvcObj* self, u32 result, u64 pid) {
    (void)self, (void)pid;
    throw_fatal(result, 0, "");
    R_SUCCEED();
}
static int nn_fatalsrv_IService_ThrowFatalWithPolicy(SvcObj* self, u32 result, u32 policy, u64 pid) {
    (void)self, (void)pid;
    throw_fatal(result, policy, "");
    R_SUCCEED();
}
/* the game's registers when it gave up, into the log first: an AArch64 context has x0 to x28, fp, lr, sp and pc */
static int nn_fatalsrv_IService_ThrowFatalWithCpuContext(SvcObj* self, u32 result, u32 policy, u64 pid,
                                                         IpcBuf context) {
    (void)self, (void)pid;
    if (context.size >= 0x108) {
        u64 r[4];
        memcpy(r, GP(context.addr + 0xe8), sizeof r);
        snx_le64_array(r, 4);
        WARN("fatal: fp %016llx lr %016llx sp %016llx pc %016llx\n", (unsigned long long)r[0],
             (unsigned long long)r[1], (unsigned long long)r[2], (unsigned long long)r[3]);
    }
    throw_fatal(result, policy, ", with its registers in the log");
    R_SUCCEED();
}

/* ============================================================ csrng, spl */
static void random_bytes(IpcBuf data) {
    u8* p = GP(data.addr);
    u64 left = data.size;
    while (left) {
        int n = left > 0x10000 ? 0x10000 : (int)left;
        if (RAND_bytes(p, n) != 1) fatal("The host's random number generator failed");
        p += n;
        left -= (u64)n;
    }
}
static int nn_spl_detail_IRandomInterface_GenerateRandomBytes(SvcObj* self, IpcBuf data) {
    (void)self;
    random_bytes(data);
    R_SUCCEED();
}
static int nn_spl_detail_IGeneralInterface_GenerateRandomBytes(SvcObj* self, IpcBuf data) {
    (void)self;
    random_bytes(data);
    R_SUCCEED();
}

/* ============================================================ mm:u */
/* the clock requests of a session: each Initialize a new one, by request id (from 1) or, in the old commands, by
   module. Setting one keeps its minimum, which is the clock it gets; one never set reads 0. A session's are an Mm
   (services_int.h), guarded by g_host.misc.mm_lock */

static MmRequest* mm_find(SvcObj* self, int by_module, u32 key) {
    Mm* mm = (Mm*)self->data;
    int i;
    for (i = 0; i < MM_REQUESTS; i++)
        if (mm->req[i].used && (by_module ? mm->req[i].module : mm->req[i].id) == key) return &mm->req[i];
    return NULL;
}
static u32 mm_initialize(SvcObj* self, u32 module) {
    Mm* mm = (Mm*)self->data;
    u32 id;
    int i;
    pm_lock(&g_host.misc.mm_lock);
    id = ++mm->next_id; /* the client gets its id even when the request can't be kept: it reads back 0 */
    for (i = 0; i < MM_REQUESTS && mm->req[i].used; i++) {
    }
    if (i < MM_REQUESTS) {
        mm->req[i].used = 1;
        mm->req[i].id = id;
        mm->req[i].module = module;
        mm->req[i].current = 0;
    } else
        WARN_LIMIT(4, "mm:u: more than %d clock requests, request %u isn't kept\n", MM_REQUESTS, id);
    pm_unlock(&g_host.misc.mm_lock);
    return id;
}
static void mm_finalize(SvcObj* self, int by_module, u32 key) {
    MmRequest* r;
    pm_lock(&g_host.misc.mm_lock);
    r = mm_find(self, by_module, key);
    if (r) r->used = 0;
    pm_unlock(&g_host.misc.mm_lock);
}
static void mm_set(SvcObj* self, int by_module, u32 key, u32 min) {
    MmRequest* r;
    pm_lock(&g_host.misc.mm_lock);
    r = mm_find(self, by_module, key);
    if (r) r->current = min;
    pm_unlock(&g_host.misc.mm_lock);
}
static u32 mm_get(SvcObj* self, int by_module, u32 key) {
    MmRequest* r;
    u32 v;
    pm_lock(&g_host.misc.mm_lock);
    r = mm_find(self, by_module, key);
    v = r ? r->current : 0;
    pm_unlock(&g_host.misc.mm_lock);
    return v;
}
static int nn_mmnv_IRequest_InitializeOld(SvcObj* self, nn_mmnv_Module module, nn_mmnv_Priority priority,
                                         u32 event_clear_mode) {
    (void)priority, (void)event_clear_mode;
    mm_initialize(self, module);
    R_SUCCEED();
}
static int nn_mmnv_IRequest_FinalizeOld(SvcObj* self, nn_mmnv_Module module) {
    mm_finalize(self, 1, module);
    R_SUCCEED();
}
static int nn_mmnv_IRequest_SetAndWaitOld(SvcObj* self, nn_mmnv_Module module, nn_mmnv_Setting min,
                                         nn_mmnv_Setting max) {
    (void)max;
    mm_set(self, 1, module, min);
    R_SUCCEED();
}
static int nn_mmnv_IRequest_GetOld(SvcObj* self, nn_mmnv_Module module, nn_mmnv_Setting* current) {
    *current = mm_get(self, 1, module);
    R_SUCCEED();
}
static int nn_mmnv_IRequest_Initialize(SvcObj* self, nn_mmnv_Module module, nn_mmnv_Priority priority,
                                      u32 event_clear_mode, u32* request_id) {
    (void)priority, (void)event_clear_mode;
    *request_id = mm_initialize(self, module);
    R_SUCCEED();
}
static int nn_mmnv_IRequest_Finalize(SvcObj* self, u32 request_id) {
    mm_finalize(self, 0, request_id);
    R_SUCCEED();
}
static int nn_mmnv_IRequest_SetAndWait(SvcObj* self, u32 request_id, nn_mmnv_Setting min, nn_mmnv_Setting max) {
    (void)max;
    mm_set(self, 0, request_id, min);
    R_SUCCEED();
}
static int nn_mmnv_IRequest_Get(SvcObj* self, u32 request_id, nn_mmnv_Setting* current) {
    *current = mm_get(self, 0, request_id);
    R_SUCCEED();
}

/* ============================================================ nfp, nfc: no device */
#define NFP_DEVICE_NOT_FOUND RC(115, 64)
#define NFP_NFC_DISABLED     RC(115, 80)
#define NFC_DEVICE_NOT_FOUND RC(127, 64)
#define NFC_NOT_INITIALIZED  RC(127, 77)
#define NFC_FINALIZED        6 /* nn::nfp::DeviceState of a device that isn't there */

/* an IUser's NfcUser (services_int.h): its nn::nfc::State, and the one availability-change event it hands out */
static u32 nfc_state(SvcObj* self) { return __atomic_load_n(&((NfcUser*)self->data)->state, __ATOMIC_ACQUIRE); }
static void nfc_set_state(SvcObj* self, u32 state) {
    __atomic_store_n(&((NfcUser*)self->data)->state, state, __ATOMIC_RELEASE);
}
static void nfc_user_destroy(SvcObj* o) { kobj_unref(kobj_at(((NfcUser*)o->data)->availability)); }
static SvcObj* nfc_user(SvcObj* o, const char* name) {
    ((NfcUser*)o->data)->availability = owned_event(name);
    o->destroy = nfc_user_destroy;
    return o;
}
/* what a command on a device answers: there is none */
static int nfp_device(SvcObj* self) { return nfc_state(self) ? NFP_DEVICE_NOT_FOUND : NFP_NFC_DISABLED; }
static int nfc_device(SvcObj* self) { return nfc_state(self) ? NFC_DEVICE_NOT_FOUND : NFC_NOT_INITIALIZED; }

static int nn_nfp_detail_IUserManager_CreateUserInterface(SvcObj* self, SvcObj** user) {
    (void)self;
    *user = nfc_user(nn_nfp_detail_IUser_new_data(g_hle.misc.nfc_users, g_hle.misc.nfc_user_next, "nfp and nfc users"), "nfp availability change");
    R_SUCCEED();
}
static int nn_nfc_detail_IUserManager_CreateUserInterface(SvcObj* self, SvcObj** user) {
    (void)self;
    *user = nfc_user(nn_nfc_detail_IUser_new_data(g_hle.misc.nfc_users, g_hle.misc.nfc_user_next, "nfp and nfc users"), "nfc availability change");
    R_SUCCEED();
}

static int nn_nfp_detail_IUser_Initialize(SvcObj* self, u64 applet_resource_user_id, u64 pid,
                                         IpcBuf mcu_version_data) {
    (void)applet_resource_user_id, (void)pid, (void)mcu_version_data;
    nfc_set_state(self, 1);
    R_SUCCEED();
}
static int nn_nfp_detail_IUser_Finalize(SvcObj* self) {
    nfc_set_state(self, 0);
    R_SUCCEED();
}
static int nn_nfp_detail_IUser_ListDevices(SvcObj* self, IpcBuf devices, i32* count) {
    (void)devices, (void)count;
    return nfp_device(self);
}
static int nn_nfp_detail_IUser_StartDetection(SvcObj* self, const nn_nfc_DeviceHandle* device_handle) {
    (void)device_handle;
    return nfp_device(self);
}
static int nn_nfp_detail_IUser_StopDetection(SvcObj* self, const nn_nfc_DeviceHandle* device_handle) {
    (void)device_handle;
    return nfp_device(self);
}
static int nn_nfp_detail_IUser_Mount(SvcObj* self, const nn_nfc_DeviceHandle* device_handle, u32 model_type,
                                    u32 mount_target) {
    (void)device_handle, (void)model_type, (void)mount_target;
    return nfp_device(self);
}
static int nn_nfp_detail_IUser_Unmount(SvcObj* self, const nn_nfc_DeviceHandle* device_handle) {
    (void)device_handle;
    return nfp_device(self);
}
static int nn_nfp_detail_IUser_OpenApplicationArea(SvcObj* self, const nn_nfc_DeviceHandle* device_handle,
                                                  u32 access_id) {
    (void)device_handle, (void)access_id;
    return nfp_device(self);
}
static int nn_nfp_detail_IUser_GetApplicationArea(SvcObj* self, const nn_nfc_DeviceHandle* device_handle, IpcBuf data,
                                                 u32* size) {
    (void)device_handle, (void)data, (void)size;
    return nfp_device(self);
}
static int nn_nfp_detail_IUser_SetApplicationArea(SvcObj* self, const nn_nfc_DeviceHandle* device_handle,
                                                 IpcBuf data) {
    (void)device_handle, (void)data;
    return nfp_device(self);
}
static int nn_nfp_detail_IUser_Flush(SvcObj* self, const nn_nfc_DeviceHandle* device_handle) {
    (void)device_handle;
    return nfp_device(self);
}
static int nn_nfp_detail_IUser_Restore(SvcObj* self, const nn_nfc_DeviceHandle* device_handle) {
    (void)device_handle;
    return nfp_device(self);
}
static int nn_nfp_detail_IUser_CreateApplicationArea(SvcObj* self, const nn_nfc_DeviceHandle* device_handle,
                                                    u32 access_id, IpcBuf data) {
    (void)device_handle, (void)access_id, (void)data;
    return nfp_device(self);
}
static int nn_nfp_detail_IUser_GetTagInfo(SvcObj* self, const nn_nfc_DeviceHandle* device_handle, IpcBuf info) {
    (void)device_handle, (void)info;
    return nfp_device(self);
}
static int nn_nfp_detail_IUser_GetRegisterInfo(SvcObj* self, const nn_nfc_DeviceHandle* device_handle, IpcBuf info) {
    (void)device_handle, (void)info;
    return nfp_device(self);
}
static int nn_nfp_detail_IUser_GetCommonInfo(SvcObj* self, const nn_nfc_DeviceHandle* device_handle, IpcBuf info) {
    (void)device_handle, (void)info;
    return nfp_device(self);
}
static int nn_nfp_detail_IUser_GetModelInfo(SvcObj* self, const nn_nfc_DeviceHandle* device_handle, IpcBuf info) {
    (void)device_handle, (void)info;
    return nfp_device(self);
}
static int nn_nfp_detail_IUser_AttachActivateEvent(SvcObj* self, const nn_nfc_DeviceHandle* device_handle,
                                                  u32* event) {
    (void)device_handle, (void)event;
    return nfp_device(self);
}
static int nn_nfp_detail_IUser_AttachDeactivateEvent(SvcObj* self, const nn_nfc_DeviceHandle* device_handle,
                                                    u32* event) {
    (void)device_handle, (void)event;
    return nfp_device(self);
}
static int nn_nfp_detail_IUser_GetState(SvcObj* self, u32* state) {
    *state = nfc_state(self);
    R_SUCCEED();
}
static int nn_nfp_detail_IUser_GetDeviceState(SvcObj* self, const nn_nfc_DeviceHandle* device_handle, u32* state) {
    (void)self, (void)device_handle;
    *state = NFC_FINALIZED;
    R_SUCCEED();
}
static int nn_nfp_detail_IUser_GetNpadId(SvcObj* self, const nn_nfc_DeviceHandle* device_handle, u32* npad_id) {
    (void)device_handle, (void)npad_id;
    return nfp_device(self);
}
static int nn_nfp_detail_IUser_GetApplicationAreaSize(SvcObj* self, const nn_nfc_DeviceHandle* device_handle,
                                                     u32* size) {
    (void)device_handle, (void)size;
    return nfp_device(self);
}
static int nn_nfp_detail_IUser_AttachAvailabilityChangeEvent(SvcObj* self, u32* event) {
    *event = ev_handle(((NfcUser*)self->data)->availability);
    R_SUCCEED();
}
static int nn_nfp_detail_IUser_RecreateApplicationArea(SvcObj* self, const nn_nfc_DeviceHandle* device_handle,
                                                      u32 access_id, IpcBuf data) {
    (void)device_handle, (void)access_id, (void)data;
    return nfp_device(self);
}
static int nn_nfp_detail_IUser_StartDetectionWithFilter(SvcObj* self, const nn_nfc_DeviceHandle* device_handle) {
    return nn_nfp_detail_IUser_StartDetection(self, device_handle);
}

static int nn_nfc_detail_IUser_Initialize(SvcObj* self, u64 applet_resource_user_id, u64 pid,
                                         IpcBuf mcu_version_data) {
    (void)applet_resource_user_id, (void)pid, (void)mcu_version_data;
    nfc_set_state(self, 1);
    R_SUCCEED();
}
static int nn_nfc_detail_IUser_Finalize(SvcObj* self) {
    nfc_set_state(self, 0);
    R_SUCCEED();
}
static int nn_nfc_detail_IUser_GetState(SvcObj* self, u32* state) {
    *state = nfc_state(self);
    R_SUCCEED();
}
/* NFC is switched on in the settings; there is just nothing to read with */
static int nn_nfc_detail_IUser_IsNfcEnabled(SvcObj* self, u8* enabled) {
    (void)self;
    *enabled = 1;
    R_SUCCEED();
}
static int nn_nfc_detail_IUser_InitializeOld(SvcObj* self, u64 applet_resource_user_id, u64 pid,
                                            IpcBuf mcu_version_data) {
    return nn_nfc_detail_IUser_Initialize(self, applet_resource_user_id, pid, mcu_version_data);
}
static int nn_nfc_detail_IUser_FinalizeOld(SvcObj* self) { return nn_nfc_detail_IUser_Finalize(self); }
static int nn_nfc_detail_IUser_GetStateOld(SvcObj* self, u32* state) { return nn_nfc_detail_IUser_GetState(self, state); }
static int nn_nfc_detail_IUser_IsNfcEnabledOld(SvcObj* self, u8* enabled) {
    return nn_nfc_detail_IUser_IsNfcEnabled(self, enabled);
}
static int nn_nfc_detail_IUser_ListDevices(SvcObj* self, IpcBuf devices, i32* count) {
    (void)devices, (void)count;
    return nfc_device(self);
}
static int nn_nfc_detail_IUser_GetDeviceState(SvcObj* self, const nn_nfc_DeviceHandle* device_handle, u32* state) {
    (void)self, (void)device_handle;
    *state = NFC_FINALIZED;
    R_SUCCEED();
}
static int nn_nfc_detail_IUser_GetNpadId(SvcObj* self, const nn_nfc_DeviceHandle* device_handle, u32* npad_id) {
    (void)device_handle, (void)npad_id;
    return nfc_device(self);
}
static int nn_nfc_detail_IUser_AttachAvailabilityChangeEvent(SvcObj* self, u32* event) {
    *event = ev_handle(((NfcUser*)self->data)->availability);
    R_SUCCEED();
}
static int nn_nfc_detail_IUser_StartDetection(SvcObj* self, const nn_nfc_DeviceHandle* device_handle, u32 protocol) {
    (void)device_handle, (void)protocol;
    return nfc_device(self);
}
static int nn_nfc_detail_IUser_StopDetection(SvcObj* self, const nn_nfc_DeviceHandle* device_handle) {
    (void)device_handle;
    return nfc_device(self);
}
static int nn_nfc_detail_IUser_GetTagInfo(SvcObj* self, const nn_nfc_DeviceHandle* device_handle, IpcBuf info) {
    (void)device_handle, (void)info;
    return nfc_device(self);
}
static int nn_nfc_detail_IUser_AttachActivateEvent(SvcObj* self, const nn_nfc_DeviceHandle* device_handle,
                                                  u32* event) {
    (void)device_handle, (void)event;
    return nfc_device(self);
}
static int nn_nfc_detail_IUser_AttachDeactivateEvent(SvcObj* self, const nn_nfc_DeviceHandle* device_handle,
                                                    u32* event) {
    (void)device_handle, (void)event;
    return nfc_device(self);
}

/* ============================================================ bcat: synchronized at once, the cache empty */
#define BCAT_FAILED_OPEN_ENTITY RC(122, 2)
#define BCAT_DONE               9 /* nn::bcat::DeliveryCacheProgressStatus */

static int nn_bcat_detail_ipc_IServiceCreator_CreateBcatService(SvcObj* self, u64 pid, SvcObj** service) {
    (void)self, (void)pid;
    *service = nn_bcat_detail_ipc_IBcatService_new();
    R_SUCCEED();
}
static int nn_bcat_detail_ipc_IServiceCreator_CreateDeliveryCacheStorageService(SvcObj* self, u64 pid,
                                                                               SvcObj** service) {
    (void)self, (void)pid;
    *service = nn_bcat_detail_ipc_IDeliveryCacheStorageService_new();
    R_SUCCEED();
}
static int nn_bcat_detail_ipc_IServiceCreator_CreateDeliveryCacheStorageServiceWithApplicationId(
    SvcObj* self, nn_ApplicationId application_id, SvcObj** service) {
    (void)self, (void)application_id;
    *service = nn_bcat_detail_ipc_IDeliveryCacheStorageService_new();
    R_SUCCEED();
}
static int bcat_progress(SvcObj** progress) {
    *progress = nn_bcat_detail_ipc_IDeliveryCacheProgressService_new();
    R_SUCCEED();
}
static int nn_bcat_detail_ipc_IBcatService_RequestSyncDeliveryCache(SvcObj* self, SvcObj** progress) {
    (void)self;
    return bcat_progress(progress);
}
static int nn_bcat_detail_ipc_IBcatService_RequestSyncDeliveryCacheWithDirectoryName(
    SvcObj* self, const nn_bcat_DirectoryName* directory_name, SvcObj** progress) {
    (void)self, (void)directory_name;
    return bcat_progress(progress);
}
static int nn_bcat_detail_ipc_IBcatService_RequestSyncDeliveryCacheWithApplicationId(SvcObj* self,
                                                                                    nn_ApplicationId application_id,
                                                                                    u32 unknown, SvcObj** progress) {
    (void)self, (void)application_id, (void)unknown;
    return bcat_progress(progress);
}
static int nn_bcat_detail_ipc_IBcatService_RequestSyncDeliveryCacheWithApplicationIdAndDirectoryName(
    SvcObj* self, nn_ApplicationId application_id, u32 unknown, const nn_bcat_DirectoryName* directory_name,
    SvcObj** progress) {
    (void)self, (void)application_id, (void)unknown, (void)directory_name;
    return bcat_progress(progress);
}
static int nn_bcat_detail_ipc_IBcatService_GetDeliveryCacheStorageUpdateNotifier(SvcObj* self,
                                                                               nn_ApplicationId application_id,
                                                                               SvcObj** notifier) {
    (void)self, (void)application_id;
    *notifier = nn_bcat_detail_ipc_INotifierService_new();
    R_SUCCEED();
}
static int nn_bcat_detail_ipc_IBcatService_RequestSuspendDeliveryTask(SvcObj* self, nn_ApplicationId application_id,
                                                                    SvcObj** suspension) {
    (void)self, (void)application_id;
    *suspension = nn_bcat_detail_ipc_IDeliveryTaskSuspensionService_new();
    R_SUCCEED();
}
/* the sync is done before the game looks */
static int nn_bcat_detail_ipc_IDeliveryCacheProgressService_GetEvent(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "bcat progress", 1);
}
/* DeliveryCacheProgressImpl: status Done, result success, nothing downloaded */
static int nn_bcat_detail_ipc_IDeliveryCacheProgressService_GetImpl(SvcObj* self, IpcBuf impl) {
    (void)self;
    memset(GP(impl.addr), 0, (size_t)impl.size);
    if (impl.size >= 4) gw32(impl.addr, (u32)BCAT_DONE);
    R_SUCCEED();
}
static int nn_bcat_detail_ipc_IDeliveryCacheStorageService_CreateFileService(SvcObj* self, SvcObj** file) {
    (void)self;
    *file = nn_bcat_detail_ipc_IDeliveryCacheFileService_new();
    R_SUCCEED();
}
static int nn_bcat_detail_ipc_IDeliveryCacheStorageService_CreateDirectoryService(SvcObj* self, SvcObj** directory) {
    (void)self;
    *directory = nn_bcat_detail_ipc_IDeliveryCacheDirectoryService_new();
    R_SUCCEED();
}
static int nn_bcat_detail_ipc_IDeliveryCacheFileService_Open(SvcObj* self, const nn_bcat_DirectoryName* directory_name,
                                                            const nn_bcat_FileName* file_name) {
    (void)self, (void)directory_name, (void)file_name;
    R_THROW(BCAT_FAILED_OPEN_ENTITY);
}
static int nn_bcat_detail_ipc_INotifierService_GetEvent(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "bcat storage update", 0);
}

/* ============================================================ aoc:u: the add-on content in system/archives */
#define AOC_NO_PURCHASED_PRODUCT RC(164, 400) /* nim's shop: NoPurchasedProductInfoAvailable */
#define AOC_NOT_CONNECTED        RC(110, 300) /* nifm's: the token needs the network */

/* the running game's from its control.nacp (am_aoc_base_id), another application's as the SDK numbers them */
static u64 aoc_base_id(u64 application_id) {
    return application_id == g_hle.kernel.title_id ? am_aoc_base_id() : snx_nacp_aoc_base(NULL, 0, application_id);
}
/* am's ApplicationRecordNotFound, for a list asked from past its end */
#define SNX_AOC_NO_RECORD RC(128, 37)
/* An add-on content is installed when its RomFS image is in the data folder as system/archives/<data id>.romfs, the
   file OpenDataStorageByDataId opens, or is a DLC read in place from its dump (fs_aoc_has): data id = base id +
   index, index 1 to 2047. Scanned once: the list does not
   change while the game runs (g_hle.misc.aoc_index, AOC_MAX at most) */
static void aoc_scan(void) {
    u64 base = aoc_base_id(g_hle.kernel.title_id);
    char path[64];
    FILE* f;
    i32 i;
    pm_lock(&g_host.misc.aoc_lock);
    if (!g_hle.misc.aoc_scanned) {
        for (i = 1; i <= AOC_MAX; i++) {
            snprintf(path, sizeof path, "system/archives/%016llx.romfs", (unsigned long long)(base + (u64)i));
            f = fs_aoc_has(base + (u64)i) ? NULL : fs_open_read(path, NULL);
            if (!f && !fs_aoc_has(base + (u64)i)) continue;
            if (f) fclose(f);
            g_hle.misc.aoc_index[g_hle.misc.aoc_count++] = i;
            LOG("aoc: add-on content %d (%016llx) installed\n", (int)i, (unsigned long long)(base + (u64)i));
        }
        g_hle.misc.aoc_scanned = 1;
    }
    pm_unlock(&g_host.misc.aoc_lock);
}
/* the running game's; another application has none here */
static i32 aoc_count_of(u64 application_id) {
    if (application_id != g_hle.kernel.title_id) return 0;
    aoc_scan();
    return g_hle.misc.aoc_count;
}
/* the indices from offset, as many as count and the buffer hold; from past the end (the offset read unsigned, as the
   console reads it) no record */
static int aoc_list(i32 offset, i32 count, u64 application_id, IpcBuf indices, i32* out_count) {
    i32 total = aoc_count_of(application_id), n;
    R_UNLESS((u32)offset <= (u32)total, SNX_AOC_NO_RECORD);
    if ((u32)count > indices.size / 4) count = (i32)(indices.size / 4);
    for (n = 0; n < count && offset + n < total; n++) gw32(indices.addr + 4 * (u64)n, (u32)g_hle.misc.aoc_index[offset + n]);
    *out_count = n;
    R_SUCCEED();
}
static int nn_aocsrv_detail_IAddOnContentManager_CountAddOnContentByApplicationId(SvcObj* self,
                                                                                  nn_ncm_ApplicationId application_id,
                                                                                  i32* count) {
    (void)self;
    *count = aoc_count_of(application_id);
    R_SUCCEED();
}
static int nn_aocsrv_detail_IAddOnContentManager_ListAddOnContentByApplicationId(SvcObj* self, i32 offset, i32 count,
                                                                                 nn_ncm_ApplicationId application_id,
                                                                                 IpcBuf indices, i32* out_count) {
    (void)self;
    return aoc_list(offset, count, application_id, indices, out_count);
}
static int nn_aocsrv_detail_IAddOnContentManager_CountAddOnContent(SvcObj* self, u64 pid, i32* count) {
    (void)self, (void)pid;
    *count = aoc_count_of(g_hle.kernel.title_id);
    R_SUCCEED();
}
static int nn_aocsrv_detail_IAddOnContentManager_ListAddOnContent(SvcObj* self, i32 offset, i32 count, u64 pid,
                                                                  IpcBuf indices, i32* out_count) {
    (void)self, (void)pid;
    return aoc_list(offset, count, g_hle.kernel.title_id, indices, out_count);
}
/* the image is in place already: nothing to download or mount */
static int
nn_aocsrv_detail_IAddOnContentManager_PrepareAddOnContentByApplicationId(SvcObj* self, i32 index,
                                                                         nn_ncm_ApplicationId application_id) {
    (void)self, (void)index, (void)application_id;
    R_SUCCEED();
}
static int nn_aocsrv_detail_IAddOnContentManager_PrepareAddOnContent(SvcObj* self, i32 index, u64 pid) {
    (void)self, (void)index, (void)pid;
    R_SUCCEED();
}
static int nn_aocsrv_detail_IAddOnContentManager_GetAddOnContentBaseIdByApplicationId(
    SvcObj* self, nn_ncm_ApplicationId application_id, u64* base_id) {
    (void)self;
    *base_id = aoc_base_id(application_id);
    R_SUCCEED();
}
static int nn_aocsrv_detail_IAddOnContentManager_GetAddOnContentBaseId(SvcObj* self, u64 pid, u64* base_id) {
    (void)self, (void)pid;
    *base_id = aoc_base_id(g_hle.kernel.title_id);
    R_SUCCEED();
}
static int nn_aocsrv_detail_IAddOnContentManager_GetAddOnContentListChangedEvent(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "aoc list changed", 0);
}
static int nn_aocsrv_detail_IAddOnContentManager_GetAddOnContentListChangedEventWithProcessId(SvcObj* self, u64 pid,
                                                                                            u32* event) {
    (void)pid;
    return nn_aocsrv_detail_IAddOnContentManager_GetAddOnContentListChangedEvent(self, event);
}
static int nn_aocsrv_detail_IAddOnContentManager_CreateEcPurchasedEventManager(SvcObj* self, SvcObj** manager) {
    (void)self;
    *manager = nn_ec_IPurchaseEventManager_new();
    R_SUCCEED();
}
static int nn_aocsrv_detail_IAddOnContentManager_CreatePermanentEcPurchasedEventManager(SvcObj* self, u64 pid,
                                                                                       SvcObj** manager) {
    (void)pid;
    return nn_aocsrv_detail_IAddOnContentManager_CreateEcPurchasedEventManager(self, manager);
}
static int nn_aocsrv_detail_IAddOnContentManager_CreateContentsServiceManager(SvcObj* self, SvcObj** manager) {
    (void)self;
    *manager = nn_ec_IContentsServiceManager_new();
    R_SUCCEED();
}
static int nn_ec_IContentsServiceManager_RequestContentsAuthorizationToken(SvcObj* self, const u8* nonce, u64 pid,
                                                                          IpcBuf data, u32* event, SvcObj** async) {
    (void)self, (void)nonce, (void)pid, (void)data, (void)event, (void)async;
    R_THROW(AOC_NOT_CONNECTED);
}
static int nn_ec_IPurchaseEventManager_GetPurchasedEventReadableHandle(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "aoc purchased", 0);
}
static int nn_ec_IPurchaseEventManager_PopPurchasedProductInfo(SvcObj* self,
                                                              nn_ec_srv_detail_PurchasedProductInfo* info) {
    (void)self, (void)info;
    R_THROW(AOC_NO_PURCHASED_PRODUCT);
}
static int nn_ec_IPurchaseEventManager_PopPurchasedProductInfoWithUid(SvcObj* self, const nn_account_Uid* uid,
                                                                     nn_ec_srv_detail_PurchasedProductInfo* info) {
    (void)self, (void)uid, (void)info;
    R_THROW(AOC_NO_PURCHASED_PRODUCT);
}

/* ============================================================ irs: a shared memory no camera writes to */
#define IRS_SHM_SIZE 0x8000
static int nn_irsensor_IIrSensorServer_GetIrsensorSharedMemoryHandle(SvcObj* self, u64 aruid, u32* shared_memory) {
    (void)self, (void)aruid;
    pm_lock(&g_host.misc.irs_lock);
    if (!g_hle.misc.irs_shm) {
        g_hle.misc.irs_shm = kobj_id(&kshmem_new(IRS_SHM_SIZE, "irs")->o);
        kobj_ref(kobj_at(g_hle.misc.irs_shm)); /* the service's, for as long as the process runs */
    }
    pm_unlock(&g_host.misc.irs_lock);
    *shared_memory = h_new(kobj_at(g_hle.misc.irs_shm));
    R_SUCCEED();
}

/* ============================================================ mii: the six default Miis */
#define MII_INVALID_ARGUMENT      RC(126, 1)
#define MII_INVALID_ARGUMENT_SIZE RC(126, 2)
#define MII_SOURCE_DEFAULT        2 /* nn::mii::SourceFlag::Default */
#define MII_NOT_FOUND             RC(126, 4)

/* nn::mii's DefaultMii, the fields in this order (colors in the 3DS's numbering): face type, color, wrinkle, makeup;
   hair type, color, flip; eye type, color, scale, aspect, rotate, x, y; eyebrow type, color, scale, aspect, rotate,
   x, y; nose type, scale, y; mouth type, color, scale, aspect, y; mustache type, beard type, beard color, mustache
   scale, mustache y; glasses type, color, scale, y; mole type, scale, x, y; height, weight, gender, favorite color,
   region move, font region, type. Each is called "no name" */
#define MII_FIELDS 49
static const u8 mii_defaults[6][MII_FIELDS] = {
    {0, 4, 0, 0, 68, 0, 0, 2, 0, 4, 3, 4, 2, 12, 6, 0, 4, 3, 6, 2, 10, 1, 4, 9, 23,
     0, 4, 3, 13, 0, 0, 0, 4, 10, 0, 0, 4, 10, 0, 4, 2, 20, 64, 64, 0, 4, 0, 0, 0},
    {0, 0, 0, 0, 55, 6, 0, 2, 4, 4, 3, 4, 2, 12, 6, 6, 4, 3, 6, 2, 10, 1, 4, 9, 23,
     0, 4, 3, 13, 0, 0, 0, 4, 10, 0, 0, 4, 10, 0, 4, 2, 20, 64, 64, 0, 5, 0, 0, 0},
    {0, 1, 0, 0, 33, 1, 0, 2, 0, 4, 3, 4, 2, 12, 6, 1, 4, 3, 6, 2, 10, 1, 4, 9, 23,
     0, 4, 3, 13, 0, 0, 0, 4, 10, 0, 0, 4, 10, 0, 4, 2, 20, 64, 64, 0, 0, 0, 0, 0},
    {0, 2, 0, 0, 24, 0, 0, 4, 0, 4, 3, 3, 2, 12, 0, 0, 4, 3, 6, 2, 10, 1, 4, 9, 23,
     0, 4, 3, 13, 0, 0, 0, 4, 10, 0, 0, 4, 10, 0, 4, 2, 20, 64, 64, 1, 2, 0, 0, 0},
    {0, 0, 0, 0, 14, 7, 0, 4, 5, 4, 3, 3, 2, 12, 0, 7, 4, 3, 6, 2, 10, 1, 4, 9, 23,
     0, 4, 3, 13, 0, 0, 0, 4, 10, 0, 0, 4, 10, 0, 4, 2, 20, 64, 64, 1, 6, 0, 0, 0},
    {0, 0, 0, 0, 12, 1, 0, 4, 0, 4, 3, 3, 2, 12, 0, 1, 4, 3, 6, 2, 10, 1, 4, 9, 23,
     0, 4, 3, 13, 0, 0, 0, 4, 10, 0, 0, 4, 10, 0, 4, 2, 20, 64, 64, 1, 7, 0, 0, 0},
};
/* where each field goes in nn::mii::CharInfo, and the table that turns its 3DS color into the Switch's (0 none) */
static const u8 mii_char_offset[MII_FIELDS] = {
    0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d,
    0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4c, 0x4b, 0x4a, 0x4d, 0x4e,
    0x4f, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x29, 0x2a, 0x28, 0x27, 0x2c, 0x26, 0x2b};
enum { MII_FACE = 1, MII_HAIR, MII_EYE, MII_MOUTH, MII_GLASS };
static const u8 mii_color_kind[MII_FIELDS] = {
    0, MII_FACE, 0, 0, 0, MII_HAIR, 0, 0, MII_EYE, 0, 0, 0, 0, 0, 0, MII_HAIR, 0, 0, 0, 0, 0, 0, 0, 0, 0, MII_MOUTH,
    0, 0, 0, 0, 0, MII_HAIR, 0, 0, 0, MII_GLASS, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static u8 mii_color(int kind, u8 v) {
    static const u8 face[8] = {0x0, 0x1, 0x2, 0x3, 0x4, 0x5};
    static const u8 hair[8] = {0x8, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7};
    static const u8 eye[6] = {0x8, 0x9, 0xa, 0xb, 0xc, 0xd};
    static const u8 mouth[5] = {0x13, 0x14, 0x15, 0x16, 0x17};
    static const u8 glass[7] = {0x8, 0xe, 0xf, 0x10, 0x11, 0x12, 0x0};
    switch (kind) {
        case MII_FACE: return v < 8 ? face[v] : 0;
        case MII_HAIR: return v < 8 ? hair[v] : 0;
        case MII_EYE: return v < 6 ? eye[v] : 0;
        case MII_MOUTH: return v < 5 ? mouth[v] : 0;
        case MII_GLASS: return v < 7 ? glass[v] : 0;
    }
    return v;
}

static int nn_mii_detail_IStaticService_GetDatabaseService(SvcObj* self, i32 key_code, SvcObj** database) {
    (void)self, (void)key_code;
    *database = nn_mii_detail_IDatabaseService_new();
    R_SUCCEED();
}
static int nn_mii_detail_IDatabaseService_BuildDefault(SvcObj* self, u32 index, nn_mii_CharInfo* info) {
    static const char name[] = "no name";
    u8* c = info->v;
    int i;
    (void)self;
    R_UNLESS(index < 6, MII_INVALID_ARGUMENT);
    memset(c, 0, sizeof info->v); /* what is not built is nought: Get builds over the last one, not a zeroed reply */
    /* a new random create id (a version 4 UUID); never zero in its first byte, so no check of it reads it as the
       invalid all-zero id */
    do {
        if (RAND_bytes(c, 16) != 1) fatal("The host's random number generator failed");
    } while (!c[0]);
    c[6] = (u8)(0x40 | (c[6] & 0x0f));
    c[8] = (u8)(0x80 | (c[8] & 0x3f));
    for (i = 0; name[i]; i++) c[0x10 + 2 * i] = (u8)name[i]; /* UTF-16 */
    for (i = 0; i < MII_FIELDS; i++) c[mii_char_offset[i]] = mii_color(mii_color_kind[i], mii_defaults[index][i]);
    R_SUCCEED();
}
static int nn_mii_detail_IDatabaseService_GetCount(SvcObj* self, i32 source_flag, i32* count) {
    (void)self;
    *count = source_flag & MII_SOURCE_DEFAULT ? 6 : 0;
    R_SUCCEED();
}
/* the six default Miis in index order, each built anew as Eden gives them, into elements of size bytes: the CharInfo,
   then for Get its source (1, Default). A buffer with no room for the next one is InvalidArgumentSize */
static int mii_give_defaults(i32 source_flag, IpcBuf out, u32 size, i32* count) {
    nn_mii_CharInfo info;
    u32 i;
    *count = 0;
    if (!(source_flag & MII_SOURCE_DEFAULT)) R_SUCCEED();
    for (i = 0; i < 6; i++) {
        if (out.size < (u64)(i + 1) * size) R_THROW(MII_INVALID_ARGUMENT_SIZE);
        nn_mii_detail_IDatabaseService_BuildDefault(NULL, i, &info);
        memcpy(GP(out.addr + (u64)i * size), info.v, sizeof info.v);
        if (size > sizeof info.v) gw32(out.addr + (u64)i * size + sizeof info.v, 1);
        (*count)++;
    }
    R_SUCCEED();
}
static int nn_mii_detail_IDatabaseService_Get(SvcObj* self, i32 source_flag, IpcBuf elements, i32* count) {
    (void)self;
    return mii_give_defaults(source_flag, elements, 0x5c, count);
}
static int nn_mii_detail_IDatabaseService_Get1(SvcObj* self, i32 source_flag, IpcBuf infos, i32* count) {
    (void)self;
    return mii_give_defaults(source_flag, infos, 0x58, count);
}
/* a Mii's newest version and its place in the database, which has no Miis: NotFound, and the game keeps the Mii it has
   (a game may ask both for the Mii its save holds, and take a successful UpdateLatest's as that Mii) */
static int nn_mii_detail_IDatabaseService_UpdateLatest(SvcObj* self, const nn_mii_CharInfo* info, i32 source_flag,
                                                       nn_mii_CharInfo* latest) {
    (void)self, (void)info, (void)source_flag, (void)latest;
    R_THROW(MII_NOT_FOUND);
}
static int nn_mii_detail_IDatabaseService_GetIndex(SvcObj* self, const nn_mii_CharInfo* info, i32* index) {
    (void)self, (void)info, (void)index;
    R_THROW(MII_NOT_FOUND);
}
/* ------------------------------------------------------------ BuildRandom */
#include "nx/hle/services/mii_random.inc"
#define MII_AGE_ALL    3 /* Young, Normal, Old, All */
#define MII_GENDER_ALL 2 /* Male, Female, All */
#define MII_RACE_ALL   3 /* Black, White, Asian, All */
/* the Ver3 to Switch colors of CoreData's hair (eyebrows, beard), eyes, mouth and glasses */
static const u8 mii_ver3_hair[8] = {0x8, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7};
static const u8 mii_ver3_eye[6] = {0x8, 0x9, 0xa, 0xb, 0xc, 0xd};
static const u8 mii_ver3_mouth[5] = {0x13, 0x14, 0x15, 0x16, 0x17};
static const u8 mii_eye_rotate[62] = {3, 4, 4, 4, 3, 4, 4, 4, 3, 4, 4, 4, 4, 3, 3, 4, 4, 4, 3, 3, 4,
                                      3, 4, 3, 3, 4, 3, 4, 4, 3, 4, 4, 4, 3, 3, 3, 4, 4, 3, 3, 3, 4,
                                      4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 3, 4, 4, 3, 4, 4};
static const u8 mii_eyebrow_rotate[24] = {6, 6, 5, 7, 6, 7, 6, 7, 4, 7, 6, 8, 5, 5, 6, 6, 7, 7, 6, 6, 5, 6, 7, 5};
/* 0 to max, both included, as nn::mii's random values (MiiUtil::GetRandomValue) */
static u32 mii_random(u32 max) {
    u32 v;
    if (RAND_bytes((u8*)&v, sizeof v) != 1) fatal("The host's random number generator failed");
    return (u32)((u64)v * ((u64)max + 1) >> 32);
}
/* one of a table row's values. Eden draws the index from 0 to the count, both included, so one draw in count + 1
   reads the zero past the row's values; here it is one of them */
static u8 mii_pick(const u8* row) { return row[1 + mii_random(row[0] - 1u)]; }
/* a random Mii of that age, gender and race (any where it says All), as Eden's CoreData::BuildRandom makes one and
   CharInfo::SetFromStoreData lays it out */
static int nn_mii_detail_IDatabaseService_BuildRandom(SvcObj* self, u32 age, u32 gender, u32 race,
                                                      nn_mii_CharInfo* info) {
    static const char name[] = "no name";
    u8 f[MII_FIELDS], *c = info->v;
    u32 axis_y = 0, index, r, eyebrow_y, flag;
    int i;
    (void)self;
    if (age > MII_AGE_ALL || gender > MII_GENDER_ALL || race > MII_RACE_ALL) R_THROW(MII_INVALID_ARGUMENT);
    if (gender == MII_GENDER_ALL) gender = mii_random(1);
    if (age == MII_AGE_ALL) {
        r = mii_random(10);
        age = r >= 8 ? 2 : r >= 4 ? 1 : 0;
    }
    if (race == MII_RACE_ALL) {
        r = mii_random(10);
        race = r >= 8 ? 0 : r >= 4 ? 1 : 2;
    }
    memset(f, 0, sizeof f);
    if (gender == 1 && age == 0) axis_y = mii_random(3);
    index = 3 * age + 9 * gender + race;
    f[0] = mii_pick(mii_faceline[index]);
    f[1] = mii_pick(mii_faceline_color[3 * gender + race]);
    f[2] = mii_pick(mii_faceline_wrinkle[index]);
    f[3] = mii_pick(mii_faceline_makeup[index]);
    f[4] = mii_pick(mii_hair_type[index]);
    f[5] = mii_ver3_hair[mii_pick(mii_hair_color[3 * race + age]) & 7];
    f[6] = (u8)mii_random(1);
    f[7] = mii_pick(mii_eye_type[index]);
    f[8] = mii_ver3_eye[mii_pick(mii_eye_color[race]) % 6];
    f[9] = 4;
    f[10] = 3;
    f[11] = (u8)((32 - mii_eye_rotate[gender ? 4 : 2] + (gender ? 3 : 4)) - (32 - mii_eye_rotate[f[7] % 62]));
    f[12] = 2;
    f[13] = (u8)(axis_y + 12);
    f[14] = mii_pick(mii_eyebrow_type[index]);
    f[15] = f[5];
    f[16] = 4;
    f[17] = 3;
    f[18] = (u8)((32 - mii_eyebrow_rotate[race == 2 ? 6 : 0] + 6) - (32 - mii_eyebrow_rotate[f[14] % 24]));
    f[19] = 2;
    eyebrow_y = race == 2 ? 6 : 7;
    f[20] = (u8)(axis_y + eyebrow_y + 3); /* CharInfo's eyebrow height is the CoreData's plus 3 */
    f[21] = mii_pick(mii_nose_type[index]);
    f[22] = gender ? 3 : 4;
    f[23] = (u8)(axis_y + 9);
    f[24] = mii_pick(mii_mouth_type[index]);
    f[25] = mii_ver3_mouth[gender ? mii_random(4) : 0];
    f[26] = 4;
    f[27] = 3;
    f[28] = (u8)(axis_y + 13);
    f[31] = f[5]; /* the beard's color is the hair's */
    f[32] = 4;
    if (!gender && age && mii_random(10) < 2) {
        flag = mii_random(3); /* beard 1, mustache 2 */
        f[30] = flag & 1 ? (u8)(1 + mii_random(4)) : 0;
        f[29] = flag & 2 ? (u8)(1 + mii_random(4)) : 0;
        f[33] = 10;
    } else
        f[33] = (u8)(axis_y + 10);
    r = mii_random(100);
    for (i = 0; r < mii_glass_type[age][1 + i];)
        if (++i >= mii_glass_type[age][0]) {
            i = 0;
            break;
        }
    f[34] = (u8)i;
    f[35] = 8; /* the Ver3 glasses color 0 */
    f[36] = 4;
    f[37] = (u8)(axis_y + 10);
    f[39] = 4;
    f[40] = 2;
    f[41] = 20;
    f[42] = 64;
    f[43] = 64;
    f[44] = (u8)gender;
    f[45] = (u8)mii_random(11);
    memset(c, 0, sizeof info->v);
    do {
        if (RAND_bytes(c, 16) != 1) fatal("The host's random number generator failed");
    } while (!c[0]);
    c[6] = (u8)(0x40 | (c[6] & 0x0f));
    c[8] = (u8)(0x80 | (c[8] & 0x3f));
    for (i = 0; name[i]; i++) c[0x10 + 2 * i] = (u8)name[i];
    for (i = 0; i < MII_FIELDS; i++) c[mii_char_offset[i]] = f[i];
    R_SUCCEED();
}
static int nn_mii_detail_IDatabaseService_SetInterfaceVersion(SvcObj* self, u32 version) {
    (void)self, (void)version;
    R_SUCCEED();
}

/* ============================================================ btm */
static int nn_btm_IBtmUser_GetCore(SvcObj* self, SvcObj** core) {
    (void)self;
    *core = nn_btm_IBtmUserCore_new();
    R_SUCCEED();
}
static int btm_event(u8* success, u32* event, const char* name) {
    *success = 1;
    return new_event(event, name, 0);
}
static int nn_btm_IBtmUserCore_AcquireBleScanEvent(SvcObj* self, u8* success, u32* event) {
    (void)self;
    return btm_event(success, event, "btm BLE scan");
}
static int nn_btm_IBtmUserCore_AcquireBleConnectionEvent(SvcObj* self, u8* success, u32* event) {
    (void)self;
    return btm_event(success, event, "btm BLE connection");
}
static int nn_btm_IBtmUserCore_AcquireBlePairingEvent(SvcObj* self, u8* success, u32* event) {
    (void)self;
    return btm_event(success, event, "btm BLE pairing");
}
static int nn_btm_IBtmUserCore_AcquireBleServiceDiscoveryEvent(SvcObj* self, u8* success, u32* event) {
    (void)self;
    return btm_event(success, event, "btm BLE service discovery");
}
static int nn_btm_IBtmUserCore_AcquireBleMtuConfigEvent(SvcObj* self, u8* success, u32* event) {
    (void)self;
    return btm_event(success, event, "btm BLE MTU config");
}

/* ============================================================ audin: no microphone to open */
#define AUDIO_NOT_FOUND RC(153, 1)
static int nn_audio_detail_IAudioInManager_OpenAudioIn(SvcObj* self, const nn_audio_AudioInParameter* params,
                                                      u64 aruid, u32 process, IpcBuf name, IpcBuf name_out,
                                                      u32* sample_rate, u32* channel_count, u32* sample_format,
                                                      u32* state, SvcObj** in) {
    (void)self, (void)params, (void)aruid, (void)process, (void)name, (void)name_out;
    (void)sample_rate, (void)channel_count, (void)sample_format, (void)state, (void)in;
    R_THROW(AUDIO_NOT_FOUND);
}
static int nn_audio_detail_IAudioInManager_OpenAudioInAuto(SvcObj* self, const nn_audio_AudioInParameter* params,
                                                          u64 aruid, u32 process, IpcBuf name, IpcBuf name_out,
                                                          u32* sample_rate, u32* channel_count, u32* sample_format,
                                                          u32* state, SvcObj** in) {
    return nn_audio_detail_IAudioInManager_OpenAudioIn(self, params, aruid, process, name, name_out, sample_rate,
                                                       channel_count, sample_format, state, in);
}
static int nn_audio_detail_IAudioInManager_OpenAudioInProtocolSpecified(
    SvcObj* self, const nn_audio_AudioInProtocol* protocol, const nn_audio_AudioInParameter* params, u64 aruid,
    u32 process, IpcBuf name, IpcBuf name_out, u32* sample_rate, u32* channel_count, u32* sample_format, u32* state,
    SvcObj** in) {
    (void)protocol;
    return nn_audio_detail_IAudioInManager_OpenAudioIn(self, params, aruid, process, name, name_out, sample_rate,
                                                       channel_count, sample_format, state, in);
}

/* ============================================================ ngc:u and ngct:u: no word lists */
/* the lists' version: there are none here, and reden answers 1 for its own empty ones */
#define NGC_CONTENT_VERSION 1u
/* a text back as it was sent, as much of it as the buffer for it holds */
static void ngc_back(IpcBuf text, IpcBuf out) {
    u64 n = text.size < out.size ? text.size : out.size;
    if (n) memmove(GP(out.addr), GP(text.addr), (size_t)n);
}
static int nn_ngc_detail_IService_GetContentVersion(SvcObj* self, u32* version) {
    (void)self;
    *version = NGC_CONTENT_VERSION;
    R_SUCCEED();
}
static int nn_ngc_detail_IService_Check(SvcObj* self, u32 languages, const nn_ngc_detail_ProfanityFilterOption* option,
                                        IpcBuf text, u32* found) {
    (void)self, (void)languages, (void)option, (void)text;
    *found = 0;
    R_SUCCEED();
}
static int nn_ngc_detail_IService_Mask(SvcObj* self, u32 languages, const nn_ngc_detail_ProfanityFilterOption* option,
                                       IpcBuf text, IpcBuf masked, u32* count) {
    (void)self, (void)languages, (void)option;
    ngc_back(text, masked);
    *count = 0;
    R_SUCCEED();
}
static int nn_ngc_detail_IService_Reload(SvcObj* self) {
    (void)self;
    R_SUCCEED();
}
static int nn_ngc_detail_IService_Check2(SvcObj* self, u32 languages, const nn_ngc_detail_ProfanityFilterOption* option,
                                         IpcBuf text, u32* found) {
    return nn_ngc_detail_IService_Check(self, languages, option, text, found);
}
static int nn_ngc_detail_IService_Mask2(SvcObj* self, u32 languages, const nn_ngc_detail_ProfanityFilterOption* option,
                                        IpcBuf text, IpcBuf masked, u32* count) {
    return nn_ngc_detail_IService_Mask(self, languages, option, text, masked, count);
}
static int nn_ngct_detail_IService_Match(SvcObj* self, IpcBuf text, u8* matched) {
    (void)self, (void)text;
    *matched = 0;
    R_SUCCEED();
}
static int nn_ngct_detail_IService_Filter(SvcObj* self, IpcBuf text, IpcBuf filtered) {
    (void)self;
    ngc_back(text, filtered);
    R_SUCCEED();
}

/* ============================================================ the services' objects */
SvcObj* new_apm(void) { return nn_apm_IManager_new(); }
SvcObj* new_lm(void) { return nn_lm_ILogService_new(); }
SvcObj* new_pctl(void) { return nn_pctl_detail_ipc_IParentalControlServiceFactory_new(); }
SvcObj* new_friend(void) { return nn_friends_detail_ipc_IServiceCreator_new(); }
SvcObj* new_ns(void) { return nn_ns_detail_IServiceGetterInterface_new(); }
SvcObj* new_prepo(void) { return nn_prepo_detail_ipc_IPrepoService_new(); }
SvcObj* new_psm(void) { return nn_psm_IPsmServer_new(); }
SvcObj* new_fatal(void) { return nn_fatalsrv_IService_new(); }
SvcObj* new_csrng(void) { return nn_spl_detail_IRandomInterface_new(); }
SvcObj* new_mm(void) { return nn_mmnv_IRequest_new_data(g_hle.misc.mms, g_hle.misc.mm_next, "mm:u sessions"); }
SvcObj* new_nfp(void) { return nn_nfp_detail_IUserManager_new(); }
SvcObj* new_nfc(void) { return nn_nfc_detail_IUserManager_new(); }
SvcObj* new_caps(void) { return nn_capsrv_sf_IAlbumApplicationService_new(); }
SvcObj* new_bcat(void) { return nn_bcat_detail_ipc_IServiceCreator_new(); }
SvcObj* new_aoc(void) { return nn_aocsrv_detail_IAddOnContentManager_new(); }
SvcObj* new_irs(void) { return nn_irsensor_IIrSensorServer_new(); }
SvcObj* new_mii(void) { return nn_mii_detail_IStaticService_new(); }
SvcObj* new_ectx(void) { return nn_err_context_IWriterForApplication_new(); }
SvcObj* new_spl(void) { return nn_spl_detail_IGeneralInterface_new(); }
SvcObj* new_btmu(void) { return nn_btm_IBtmUser_new(); }
SvcObj* new_audin(void) { return nn_audio_detail_IAudioInManager_new(); }
SvcObj* new_ngc(void) { return nn_ngc_detail_IService_new(); }
SvcObj* new_ngct(void) { return nn_ngct_detail_IService_new(); }
