/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/apm/apm_controller.cpp, apm_controller.h, mm/mm_u.cpp,
   nfc/nfc_result.h, nfc/nfc_types.h, nfc/common/device_manager.cpp, nfc/nfc_interface.cpp, bcat/bcat_types.h,
   bcat/bcat_result.h, bcat/backend/backend.cpp, bcat/delivery_cache_file_service.cpp,
   aoc/purchase_event_manager.cpp, src/core/file_sys/common_funcs.h, mii/mii_result.h, mii/types/store_data.cpp,
   mii/types/char_info.h, mii/types/char_info.cpp, ptm/psm.cpp, lm/lm.cpp, audio/errors.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018, 2019, 2020, 2022, 2023 yuzu Emulator Project (GPL-2.0-or-later)
   and the default Miis from Eden's src/core/hle/service/mii/types/raw_data.cpp:
   SPDX-FileCopyrightText: Ryujinx Team and Contributors (MIT) */
/* The smaller services: apm, lm, pctl, friend, ns, prepo, caps, ectx, psm, fatal, csrng, spl, mm, nfp, nfc, bcat,
   aoc, irs, mii, ldn, btm and audin, each as a console offline, with no NFC device, microphone or add-on content */
#include "nx/hle/services/services_int.h"
#include <openssl/rand.h>

#include "hle/services/misc.swipc.inc"

/* ============================================================ apm */
#define APM_CONFIGURATION 0x00020003u /* Config7, the default of both modes */

static int nn_apm_IManager_OpenSession(SvcObj* self, SvcObj** session) {
    (void)self;
    *session = nn_apm_ISession_new(0);
    return 0;
}
/* Boost when docked, Normal in handheld mode */
static int nn_apm_IManager_GetPerformanceMode(SvcObj* self, nn_apm_PerformanceMode* mode) {
    (void)self;
    *mode = g_opt_docked ? 1 : 0;
    return 0;
}
static int nn_apm_ISession_GetPerformanceConfiguration(SvcObj* self, nn_apm_PerformanceMode mode,
                                                       nn_apm_PerformanceConfiguration* config) {
    (void)self, (void)mode;
    *config = APM_CONFIGURATION;
    return 0;
}

/* ============================================================ lm */
static int nn_lm_ILogService_OpenLogger(SvcObj* self, u64 pid, SvcObj** logger) {
    (void)self, (void)pid;
    *logger = nn_lm_ILogger_new(0);
    return 0;
}
/* a log packet: a 0x18 byte header (its payload's size at 0x14), then fields of a type byte, a ULEB128 size and
   the data (nn::lm's LogDataChunkKey). The text (2), with the file (4) and line (3) when there are some, goes to
   the log */
static int nn_lm_ILogger_Log(SvcObj* self, IpcBuf message) {
    const u8 *p = GP(message.addr), *end;
    const u8 *text = NULL, *file = NULL;
    u32 payload, text_n = 0, file_n = 0, line = 0;
    (void)self;
    if (message.size < 0x18) return 0;
    memcpy(&payload, p + 0x14, 4);
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
            text = p;
            text_n = (u32)n;
        } else if (type == 4) {
            file = p;
            file_n = (u32)n;
        } else if (type == 3 && n >= 4)
            memcpy(&line, p, 4);
        p += n;
    }
    while (text_n && (text[text_n - 1] == '\n' || !text[text_n - 1])) text_n--;
    if (text_n) {
        if (file_n)
            LOG("[lm] %.*s (%.*s:%u)\n", (int)text_n, (const char*)text, (int)file_n, (const char*)file, line);
        else
            LOG("[lm] %.*s\n", (int)text_n, (const char*)text);
    }
    return 0;
}

/* ============================================================ pctl: no restrictions set */
static int nn_pctl_detail_ipc_IParentalControlServiceFactory_CreateService(SvcObj* self, u64 pid, SvcObj** service) {
    (void)self, (void)pid;
    *service = nn_pctl_detail_ipc_IParentalControlService_new(0);
    return 0;
}
static int nn_pctl_detail_ipc_IParentalControlServiceFactory_CreateServiceWithoutInitialize(SvcObj* self, u64 pid,
                                                                                         SvcObj** service) {
    return nn_pctl_detail_ipc_IParentalControlServiceFactory_CreateService(self, pid, service);
}
static int nn_pctl_detail_ipc_IParentalControlService_IsStereoVisionPermitted(SvcObj* self, u8* permitted) {
    (void)self;
    *permitted = 1;
    return 0;
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
    *service = nn_friends_detail_ipc_IFriendService_new(0);
    return 0;
}
static int nn_friends_detail_ipc_IServiceCreator_CreateNotificationService(SvcObj* self, const nn_account_Uid* uid,
                                                                         SvcObj** service) {
    (void)self, (void)uid;
    *service = nn_friends_detail_ipc_INotificationService_new(0);
    return 0;
}
static int nn_friends_detail_ipc_IServiceCreator_CreateDaemonSuspendSessionService(SvcObj* self, SvcObj** service) {
    (void)self;
    *service = nn_friends_detail_ipc_IDaemonSuspendSessionService_new(0);
    return 0;
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
    return FRIEND_NO_NOTIFICATIONS;
}

/* ============================================================ ns */
static int nn_ns_detail_IServiceGetterInterface_GetDynamicRightsInterface(SvcObj* self, SvcObj** iface) {
    (void)self;
    *iface = nn_ns_detail_IDynamicRightsInterface_new(0);
    return 0;
}
static int nn_ns_detail_IServiceGetterInterface_GetReadOnlyApplicationControlDataInterface(SvcObj* self,
                                                                                          SvcObj** iface) {
    (void)self;
    *iface = nn_ns_detail_IReadOnlyApplicationControlDataInterface_new(0);
    return 0;
}
static int nn_ns_detail_IServiceGetterInterface_GetReadOnlyApplicationRecordInterface(SvcObj* self, SvcObj** iface) {
    (void)self;
    *iface = nn_ns_detail_IReadOnlyApplicationRecordInterface_new(0);
    return 0;
}
static int nn_ns_detail_IServiceGetterInterface_GetECommerceInterface(SvcObj* self, SvcObj** iface) {
    (void)self;
    *iface = nn_ns_detail_IECommerceInterface_new(0);
    return 0;
}
static int nn_ns_detail_IServiceGetterInterface_GetApplicationVersionInterface(SvcObj* self, SvcObj** iface) {
    (void)self;
    *iface = nn_ns_detail_IApplicationVersionInterface_new(0);
    return 0;
}
static int nn_ns_detail_IServiceGetterInterface_GetFactoryResetInterface(SvcObj* self, SvcObj** iface) {
    (void)self;
    *iface = nn_ns_detail_IFactoryResetInterface_new(0);
    return 0;
}
static int nn_ns_detail_IServiceGetterInterface_GetAccountProxyInterface(SvcObj* self, SvcObj** iface) {
    (void)self;
    *iface = nn_ns_detail_IAccountProxyInterface_new(0);
    return 0;
}
static int nn_ns_detail_IServiceGetterInterface_GetApplicationManagerInterface(SvcObj* self, SvcObj** iface) {
    (void)self;
    *iface = nn_ns_detail_IApplicationManagerInterface_new(0);
    return 0;
}
static int nn_ns_detail_IServiceGetterInterface_GetDownloadTaskInterface(SvcObj* self, SvcObj** iface) {
    (void)self;
    *iface = nn_ns_detail_IDownloadTaskInterface_new(0);
    return 0;
}
static int nn_ns_detail_IServiceGetterInterface_GetContentManagementInterface(SvcObj* self, SvcObj** iface) {
    (void)self;
    *iface = nn_ns_detail_IContentManagementInterface_new(0);
    return 0;
}
static int nn_ns_detail_IServiceGetterInterface_GetDocumentInterface(SvcObj* self, SvcObj** iface) {
    (void)self;
    *iface = nn_ns_detail_IDocumentInterface_new(0);
    return 0;
}

/* ============================================================ ectx */
static int nn_err_context_IWriterForApplication_CreateContextRegistrar(SvcObj* self, SvcObj** registrar) {
    (void)self;
    *registrar = nn_err_context_IContextRegistrar_new(0);
    return 0;
}

/* ============================================================ psm: a full battery, on the charger */
static int nn_psm_IPsmServer_GetBatteryChargePercentage(SvcObj* self, u32* percentage) {
    (void)self;
    *percentage = 100;
    return 0;
}
static int nn_psm_IPsmServer_GetChargerType(SvcObj* self, nn_psm_ChargerType* charger_type) {
    (void)self;
    *charger_type = 1; /* RegularCharger */
    return 0;
}
static int nn_psm_IPsmServer_OpenSession(SvcObj* self, SvcObj** session) {
    (void)self;
    *session = nn_psm_IPsmSession_new(0);
    return 0;
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
    return 0;
}
static int nn_fatalsrv_IService_ThrowFatalWithPolicy(SvcObj* self, u32 result, u32 policy, u64 pid) {
    (void)self, (void)pid;
    throw_fatal(result, policy, "");
    return 0;
}
/* the game's registers when it gave up, into the log first: an AArch64 context has x0 to x28, fp, lr, sp and pc */
static int nn_fatalsrv_IService_ThrowFatalWithCpuContext(SvcObj* self, u32 result, u32 policy, u64 pid,
                                                         IpcBuf context) {
    (void)self, (void)pid;
    if (context.size >= 0x108) {
        u64 r[4];
        memcpy(r, GP(context.addr + 0xe8), sizeof r);
        WARN("fatal: fp %016llx lr %016llx sp %016llx pc %016llx\n", (unsigned long long)r[0],
             (unsigned long long)r[1], (unsigned long long)r[2], (unsigned long long)r[3]);
    }
    throw_fatal(result, policy, ", with its registers in the log");
    return 0;
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
    return 0;
}
static int nn_spl_detail_IGeneralInterface_GenerateRandomBytes(SvcObj* self, IpcBuf data) {
    (void)self;
    random_bytes(data);
    return 0;
}

/* ============================================================ mm:u */
/* the clock requests of a session: each Initialize a new one, by request id (from 1) or, in the old commands, by
   module. Setting one keeps its minimum, which is the clock it gets; one never set reads 0 */
#define MM_REQUESTS 32
typedef struct {
    u32 used, id, module, current;
} MmRequest;
typedef struct {
    MmRequest req[MM_REQUESTS];
    u32 next_id;
} Mm;
static PMutex mm_lock = PMUTEX_INIT;

static MmRequest* mm_find(SvcObj* self, int by_module, u32 key) {
    Mm* mm = (Mm*)self->data;
    int i;
    for (i = 0; i < MM_REQUESTS; i++)
        if (mm->req[i].used && (by_module ? mm->req[i].module : mm->req[i].id) == key) return &mm->req[i];
    return NULL;
}
static u32 mm_initialize(SvcObj* self, u32 module) {
    Mm* mm = (Mm*)self->data;
    u32 id = 0;
    int i;
    pm_lock(&mm_lock);
    for (i = 0; i < MM_REQUESTS && mm->req[i].used; i++) {
    }
    if (i < MM_REQUESTS) {
        id = ++mm->next_id;
        mm->req[i].used = 1;
        mm->req[i].id = id;
        mm->req[i].module = module;
        mm->req[i].current = 0;
    } else
        WARN_LIMIT(4, "mm:u: more than %d clock requests, one is dropped\n", MM_REQUESTS);
    pm_unlock(&mm_lock);
    return id;
}
static void mm_finalize(SvcObj* self, int by_module, u32 key) {
    MmRequest* r;
    pm_lock(&mm_lock);
    r = mm_find(self, by_module, key);
    if (r) r->used = 0;
    pm_unlock(&mm_lock);
}
static void mm_set(SvcObj* self, int by_module, u32 key, u32 min) {
    MmRequest* r;
    pm_lock(&mm_lock);
    r = mm_find(self, by_module, key);
    if (r) r->current = min;
    pm_unlock(&mm_lock);
}
static u32 mm_get(SvcObj* self, int by_module, u32 key) {
    MmRequest* r;
    u32 v;
    pm_lock(&mm_lock);
    r = mm_find(self, by_module, key);
    v = r ? r->current : 0;
    pm_unlock(&mm_lock);
    return v;
}
static int nn_mmnv_IRequest_InitializeOld(SvcObj* self, nn_mmnv_Module module, nn_mmnv_Priority priority,
                                         u32 event_clear_mode) {
    (void)priority, (void)event_clear_mode;
    mm_initialize(self, module);
    return 0;
}
static int nn_mmnv_IRequest_FinalizeOld(SvcObj* self, nn_mmnv_Module module) {
    mm_finalize(self, 1, module);
    return 0;
}
static int nn_mmnv_IRequest_SetAndWaitOld(SvcObj* self, nn_mmnv_Module module, nn_mmnv_Setting min,
                                         nn_mmnv_Setting max) {
    (void)max;
    mm_set(self, 1, module, min);
    return 0;
}
static int nn_mmnv_IRequest_GetOld(SvcObj* self, nn_mmnv_Module module, nn_mmnv_Setting* current) {
    *current = mm_get(self, 1, module);
    return 0;
}
static int nn_mmnv_IRequest_Initialize(SvcObj* self, nn_mmnv_Module module, nn_mmnv_Priority priority,
                                      u32 event_clear_mode, u32* request_id) {
    (void)priority, (void)event_clear_mode;
    *request_id = mm_initialize(self, module);
    return 0;
}
static int nn_mmnv_IRequest_Finalize(SvcObj* self, u32 request_id) {
    mm_finalize(self, 0, request_id);
    return 0;
}
static int nn_mmnv_IRequest_SetAndWait(SvcObj* self, u32 request_id, nn_mmnv_Setting min, nn_mmnv_Setting max) {
    (void)max;
    mm_set(self, 0, request_id, min);
    return 0;
}
static int nn_mmnv_IRequest_Get(SvcObj* self, u32 request_id, nn_mmnv_Setting* current) {
    *current = mm_get(self, 0, request_id);
    return 0;
}

/* ============================================================ nfp, nfc: no device */
#define NFP_DEVICE_NOT_FOUND RC(115, 64)
#define NFP_NFC_DISABLED     RC(115, 80)
#define NFC_DEVICE_NOT_FOUND RC(127, 64)
#define NFC_NOT_INITIALIZED  RC(127, 77)
#define NFC_FINALIZED        6 /* nn::nfp::DeviceState of a device that isn't there */

/* an IUser's nn::nfc::State in its data: 0 not initialized, 1 initialized */
static u32 nfc_state(SvcObj* self) { return __atomic_load_n((u32*)self->data, __ATOMIC_ACQUIRE); }
static void nfc_set_state(SvcObj* self, u32 state) { __atomic_store_n((u32*)self->data, state, __ATOMIC_RELEASE); }
/* what a command on a device answers: there is none */
static int nfp_device(SvcObj* self) { return nfc_state(self) ? NFP_DEVICE_NOT_FOUND : NFP_NFC_DISABLED; }
static int nfc_device(SvcObj* self) { return nfc_state(self) ? NFC_DEVICE_NOT_FOUND : NFC_NOT_INITIALIZED; }

static int nn_nfp_detail_IUserManager_CreateUserInterface(SvcObj* self, SvcObj** user) {
    (void)self;
    *user = nn_nfp_detail_IUser_new(sizeof(u32));
    return 0;
}
static int nn_nfc_detail_IUserManager_CreateUserInterface(SvcObj* self, SvcObj** user) {
    (void)self;
    *user = nn_nfc_detail_IUser_new(sizeof(u32));
    return 0;
}

static int nn_nfp_detail_IUser_Initialize(SvcObj* self, u64 applet_resource_user_id, u64 pid,
                                         IpcBuf mcu_version_data) {
    (void)applet_resource_user_id, (void)pid, (void)mcu_version_data;
    nfc_set_state(self, 1);
    return 0;
}
static int nn_nfp_detail_IUser_Finalize(SvcObj* self) {
    nfc_set_state(self, 0);
    return 0;
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
    return 0;
}
static int nn_nfp_detail_IUser_GetDeviceState(SvcObj* self, const nn_nfc_DeviceHandle* device_handle, u32* state) {
    (void)self, (void)device_handle;
    *state = NFC_FINALIZED;
    return 0;
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
/* no device comes or goes */
static int nn_nfp_detail_IUser_AttachAvailabilityChangeEvent(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "nfp availability change", 0);
}
static int nn_nfp_detail_IUser_RecreateApplicationArea(SvcObj* self, const nn_nfc_DeviceHandle* device_handle,
                                                      u32 access_id, IpcBuf data) {
    (void)device_handle, (void)access_id, (void)data;
    return nfp_device(self);
}

static int nn_nfc_detail_IUser_Initialize(SvcObj* self, u64 applet_resource_user_id, u64 pid,
                                         IpcBuf mcu_version_data) {
    (void)applet_resource_user_id, (void)pid, (void)mcu_version_data;
    nfc_set_state(self, 1);
    return 0;
}
static int nn_nfc_detail_IUser_Finalize(SvcObj* self) {
    nfc_set_state(self, 0);
    return 0;
}
static int nn_nfc_detail_IUser_GetState(SvcObj* self, u32* state) {
    *state = nfc_state(self);
    return 0;
}
/* NFC is switched on in the settings; there is just nothing to read with */
static int nn_nfc_detail_IUser_IsNfcEnabled(SvcObj* self, u8* enabled) {
    (void)self;
    *enabled = 1;
    return 0;
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
    return 0;
}
static int nn_nfc_detail_IUser_GetNpadId(SvcObj* self, const nn_nfc_DeviceHandle* device_handle, u32* npad_id) {
    (void)device_handle, (void)npad_id;
    return nfc_device(self);
}
static int nn_nfc_detail_IUser_AttachAvailabilityChangeEvent(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "nfc availability change", 0);
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
    *service = nn_bcat_detail_ipc_IBcatService_new(0);
    return 0;
}
static int nn_bcat_detail_ipc_IServiceCreator_CreateDeliveryCacheStorageService(SvcObj* self, u64 pid,
                                                                               SvcObj** service) {
    (void)self, (void)pid;
    *service = nn_bcat_detail_ipc_IDeliveryCacheStorageService_new(0);
    return 0;
}
static int nn_bcat_detail_ipc_IServiceCreator_CreateDeliveryCacheStorageServiceWithApplicationId(
    SvcObj* self, nn_ApplicationId application_id, SvcObj** service) {
    (void)self, (void)application_id;
    *service = nn_bcat_detail_ipc_IDeliveryCacheStorageService_new(0);
    return 0;
}
static int bcat_progress(SvcObj** progress) {
    *progress = nn_bcat_detail_ipc_IDeliveryCacheProgressService_new(0);
    return 0;
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
    *notifier = nn_bcat_detail_ipc_INotifierService_new(0);
    return 0;
}
static int nn_bcat_detail_ipc_IBcatService_RequestSuspendDeliveryTask(SvcObj* self, nn_ApplicationId application_id,
                                                                    SvcObj** suspension) {
    (void)self, (void)application_id;
    *suspension = nn_bcat_detail_ipc_IDeliveryTaskSuspensionService_new(0);
    return 0;
}
/* the sync is done before the game looks */
static int nn_bcat_detail_ipc_IDeliveryCacheProgressService_GetEvent(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "bcat progress", 1);
}
/* DeliveryCacheProgressImpl: status Done, result success, nothing downloaded */
static int nn_bcat_detail_ipc_IDeliveryCacheProgressService_GetImpl(SvcObj* self, IpcBuf impl) {
    i32 status = BCAT_DONE;
    (void)self;
    memset(GP(impl.addr), 0, (size_t)impl.size);
    if (impl.size >= 4) memcpy(GP(impl.addr), &status, 4);
    return 0;
}
static int nn_bcat_detail_ipc_IDeliveryCacheStorageService_CreateFileService(SvcObj* self, SvcObj** file) {
    (void)self;
    *file = nn_bcat_detail_ipc_IDeliveryCacheFileService_new(0);
    return 0;
}
static int nn_bcat_detail_ipc_IDeliveryCacheStorageService_CreateDirectoryService(SvcObj* self, SvcObj** directory) {
    (void)self;
    *directory = nn_bcat_detail_ipc_IDeliveryCacheDirectoryService_new(0);
    return 0;
}
static int nn_bcat_detail_ipc_IDeliveryCacheFileService_Open(SvcObj* self, const nn_bcat_DirectoryName* directory_name,
                                                            const nn_bcat_FileName* file_name) {
    (void)self, (void)directory_name, (void)file_name;
    return BCAT_FAILED_OPEN_ENTITY;
}
static int nn_bcat_detail_ipc_INotifierService_GetEvent(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "bcat storage update", 0);
}

/* ============================================================ aoc:u: no add-on content */
#define AOC_NO_PURCHASED_PRODUCT RC(164, 400) /* nim's shop: NoPurchasedProductInfoAvailable */
#define AOC_NOT_CONNECTED        RC(110, 300) /* nifm's: the token needs the network */

/* the add-on contents' ids start past the application's: its base id with 0x1000 added */
static u64 aoc_base_id(u64 application_id) { return (application_id & 0xFFFFFFFFFFFFE000ull) + 0x1000; }
static int nn_aocsrv_detail_IAddOnContentManager_GetAddOnContentBaseIdByApplicationId(
    SvcObj* self, nn_ncm_ApplicationId application_id, u64* base_id) {
    (void)self;
    *base_id = aoc_base_id(application_id);
    return 0;
}
static int nn_aocsrv_detail_IAddOnContentManager_GetAddOnContentBaseId(SvcObj* self, u64 pid, u64* base_id) {
    (void)self, (void)pid;
    *base_id = aoc_base_id(g_title_id);
    return 0;
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
    *manager = nn_ec_IPurchaseEventManager_new(0);
    return 0;
}
static int nn_aocsrv_detail_IAddOnContentManager_CreatePermanentEcPurchasedEventManager(SvcObj* self, u64 pid,
                                                                                       SvcObj** manager) {
    (void)pid;
    return nn_aocsrv_detail_IAddOnContentManager_CreateEcPurchasedEventManager(self, manager);
}
static int nn_aocsrv_detail_IAddOnContentManager_CreateContentsServiceManager(SvcObj* self, SvcObj** manager) {
    (void)self;
    *manager = nn_ec_IContentsServiceManager_new(0);
    return 0;
}
static int nn_ec_IContentsServiceManager_RequestContentsAuthorizationToken(SvcObj* self, const u8* nonce, u64 pid,
                                                                          IpcBuf data, u32* event, SvcObj** async) {
    (void)self, (void)nonce, (void)pid, (void)data, (void)event, (void)async;
    return AOC_NOT_CONNECTED;
}
static int nn_ec_IPurchaseEventManager_GetPurchasedEventReadableHandle(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "aoc purchased", 0);
}
static int nn_ec_IPurchaseEventManager_PopPurchasedProductInfo(SvcObj* self,
                                                              nn_ec_srv_detail_PurchasedProductInfo* info) {
    (void)self, (void)info;
    return AOC_NO_PURCHASED_PRODUCT;
}
static int nn_ec_IPurchaseEventManager_PopPurchasedProductInfoWithUid(SvcObj* self, const nn_account_Uid* uid,
                                                                     nn_ec_srv_detail_PurchasedProductInfo* info) {
    (void)self, (void)uid, (void)info;
    return AOC_NO_PURCHASED_PRODUCT;
}

/* ============================================================ irs: a shared memory no camera writes to */
#define IRS_SHM_SIZE 0x8000
static int nn_irsensor_IIrSensorServer_GetIrsensorSharedMemoryHandle(SvcObj* self, u64 aruid, u32* shared_memory) {
    static KShmem* shm;
    static PMutex lock = PMUTEX_INIT;
    (void)self, (void)aruid;
    pm_lock(&lock);
    if (!shm) {
        shm = kshmem_new(IRS_SHM_SIZE, "irs");
        kobj_ref(&shm->o); /* the service's, for as long as the process runs */
    }
    pm_unlock(&lock);
    *shared_memory = h_new(&shm->o);
    return 0;
}

/* ============================================================ mii: the six default Miis */
#define MII_INVALID_ARGUMENT RC(126, 1)

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
    *database = nn_mii_detail_IDatabaseService_new(0);
    return 0;
}
static int nn_mii_detail_IDatabaseService_BuildDefault(SvcObj* self, u32 index, nn_mii_CharInfo* info) {
    static const char name[] = "no name";
    u8* c = info->v;
    int i;
    (void)self;
    if (index >= 6) return MII_INVALID_ARGUMENT;
    /* a new random create id (a version 4 UUID); never zero in its first byte, so no check of it reads it as the
       invalid all-zero id */
    do {
        if (RAND_bytes(c, 16) != 1) fatal("The host's random number generator failed");
    } while (!c[0]);
    c[6] = (u8)(0x40 | (c[6] & 0x0f));
    c[8] = (u8)(0x80 | (c[8] & 0x3f));
    for (i = 0; name[i]; i++) c[0x10 + 2 * i] = (u8)name[i]; /* UTF-16 */
    for (i = 0; i < MII_FIELDS; i++) c[mii_char_offset[i]] = mii_color(mii_color_kind[i], mii_defaults[index][i]);
    return 0;
}
static int nn_mii_detail_IDatabaseService_SetInterfaceVersion(SvcObj* self, u32 version) {
    (void)self, (void)version;
    return 0;
}

/* ============================================================ ldn */
static int nn_ldn_detail_IUserServiceCreator_CreateUserLocalCommunicationService(SvcObj* self, SvcObj** service) {
    (void)self;
    *service = nn_ldn_detail_IUserLocalCommunicationService_new(sizeof(u32));
    return 0;
}
/* nn::ldn::State: 0 none, 1 initialized */
static int nn_ldn_detail_IUserLocalCommunicationService_GetState(SvcObj* self, u32* state) {
    *state = __atomic_load_n((u32*)self->data, __ATOMIC_ACQUIRE);
    return 0;
}
static int nn_ldn_detail_IUserLocalCommunicationService_AttachStateChangeEvent(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "ldn state change", 0);
}
static int nn_ldn_detail_IUserLocalCommunicationService_Initialize(SvcObj* self, u64 pid) {
    (void)pid;
    __atomic_store_n((u32*)self->data, 1u, __ATOMIC_RELEASE);
    return 0;
}
static int nn_ldn_detail_IUserLocalCommunicationService_Finalize(SvcObj* self) {
    __atomic_store_n((u32*)self->data, 0u, __ATOMIC_RELEASE);
    return 0;
}
static int nn_ldn_detail_IUserLocalCommunicationService_InitializeWithVersion(SvcObj* self, i32 version, u64 pid) {
    (void)version;
    return nn_ldn_detail_IUserLocalCommunicationService_Initialize(self, pid);
}

/* ============================================================ btm */
static int nn_btm_IBtmUser_GetCore(SvcObj* self, SvcObj** core) {
    (void)self;
    *core = nn_btm_IBtmUserCore_new(0);
    return 0;
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
    return AUDIO_NOT_FOUND;
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

/* ============================================================ the services' objects */
SvcObj* new_apm(void) { return nn_apm_IManager_new(0); }
SvcObj* new_lm(void) { return nn_lm_ILogService_new(0); }
SvcObj* new_pctl(void) { return nn_pctl_detail_ipc_IParentalControlServiceFactory_new(0); }
SvcObj* new_friend(void) { return nn_friends_detail_ipc_IServiceCreator_new(0); }
SvcObj* new_ns(void) { return nn_ns_detail_IServiceGetterInterface_new(0); }
SvcObj* new_prepo(void) { return nn_prepo_detail_ipc_IPrepoService_new(0); }
SvcObj* new_psm(void) { return nn_psm_IPsmServer_new(0); }
SvcObj* new_fatal(void) { return nn_fatalsrv_IService_new(0); }
SvcObj* new_csrng(void) { return nn_spl_detail_IRandomInterface_new(0); }
SvcObj* new_mm(void) { return nn_mmnv_IRequest_new(sizeof(Mm)); }
SvcObj* new_nfp(void) { return nn_nfp_detail_IUserManager_new(0); }
SvcObj* new_nfc(void) { return nn_nfc_detail_IUserManager_new(0); }
SvcObj* new_caps(void) { return nn_capsrv_sf_IAlbumApplicationService_new(0); }
SvcObj* new_bcat(void) { return nn_bcat_detail_ipc_IServiceCreator_new(0); }
SvcObj* new_aoc(void) { return nn_aocsrv_detail_IAddOnContentManager_new(0); }
SvcObj* new_irs(void) { return nn_irsensor_IIrSensorServer_new(0); }
SvcObj* new_mii(void) { return nn_mii_detail_IStaticService_new(0); }
SvcObj* new_ldn(void) { return nn_ldn_detail_IUserServiceCreator_new(0); }
SvcObj* new_ectx(void) { return nn_err_context_IWriterForApplication_new(0); }
SvcObj* new_spl(void) { return nn_spl_detail_IGeneralInterface_new(0); }
SvcObj* new_btmu(void) { return nn_btm_IBtmUser_new(0); }
SvcObj* new_audin(void) { return nn_audio_detail_IAudioInManager_new(0); }
