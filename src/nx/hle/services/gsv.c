/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The graphics supervisor bootstrap reached through am's GetGraphicsSupervisor. This is a TIPC service: the IPC
   parser subtracts 16 from the message type before dispatch. The syncpoint, GPU and display session handshakes below
   are experimental. They do not yet back the newer console's native GPU memory, worker and layer state. */
#include "nx/hle/services/services_int.h"
#include "nx/hle/hle.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/services/gsv.h"
#include "nx/hle/services/vi.h"
#include "nx/runtime/platform/backend.h"
static int gsv_user_unknown(SvcObj* self, IpcCtx* c) {
    {
        u32 i;
        LOG("gsv: UserSession cmd %u raw size %#x, copy handles %d, buffers %d/%d/%d/%d/%d\n",
            c->cmd, c->raw_size, c->ncopy, c->na, c->nb, c->nw, c->nx, c->nc);
        for (i = 0; i < c->raw_size && i < 0x60; i += 8) {
            u64 word = 0;
            memcpy(&word, c->raw + i, c->raw_size - i < 8 ? c->raw_size - i : 8);
            LOG("gsv:   raw +%#x = %#llx\n", i, (unsigned long long)word);
        }
    }
    return ipc_unknown(self, c);
}
static int gsv_kuser_unknown(SvcObj* self, IpcCtx* c) {
    if (c->cmd == 211) {
        u32 i;
        LOG("gsv: KUserWorker cmd 211 raw size %#x, copy handles %d, buffers %d/%d/%d/%d/%d\n",
            c->raw_size, c->ncopy, c->na, c->nb, c->nw, c->nx, c->nc);
        for (i = 0; i < c->raw_size && i < 0x60; i += 8) {
            u64 word = 0;
            memcpy(&word, c->raw + i, c->raw_size - i < 8 ? c->raw_size - i : 8);
            LOG("gsv:   raw +%#x = %#llx\n", i, (unsigned long long)word);
        }
    }
    return ipc_unknown(self, c);
}
/* The camera context's session, with no camera plugged in (gsv.swipc): every command that gsv.swipc does not declare
   answers the camera service's result of module 452 and description 3, which the SDK's QueryDeviceConnection (29)
   lets pass and one game logs as no camera device found */
static int gsv_camera_none(SvcObj* self, IpcCtx* c) {
    (void)self;
    LOG_LIMIT(16, "gsv: camera session cmd %u, raw size %#x, and no camera is plugged in\n", c->cmd, c->raw_size);
    R_THROW(SNX_RC_CAMERA_DEVICE_NOT_DETECTED);
}
#include "hle/services/gsv.swipc.inc"
#include "hle/services/nvdrv.swipc.h"
#include "nx/hle/services/nvdrv.h"
#include <stdatomic.h>

SvcObj* new_gsv_session(void) {
    return nn_gsv_UserSession_new();
}

static int nn_gsv_UserSession_BootWithRemoteMemory(SvcObj* self, u64 aruid, u64 version, u64 reserved0, u64 reserved1,
                                                   u64 reserved2, u64 usage, u32 process) {
    (void)self, (void)aruid, (void)reserved0, (void)reserved1, (void)reserved2, (void)usage, (void)process;
    LOG("gsv: BootWithRemoteMemory(version %#llx, usage %llu)\n", (unsigned long long)version,
        (unsigned long long)usage);
    R_SUCCEED();
}
static int nn_gsv_UserSession_SetCurrentProcessHandle(SvcObj* self, u32 process) {
    (void)self, (void)process;
    R_SUCCEED();
}
static int nn_gsv_UserSession_CreateContext(SvcObj* self) {
    (void)self;
    R_SUCCEED();
}
/* nn::camera::sdk::CreateCameraContext sends 601, then 602 with a 1, and a game stops if they fail: the context exists
   whether or not a camera does */
static int nn_gsv_UserSession_CreateCameraContext(SvcObj* self) {
    (void)self;
    R_SUCCEED();
}
static int nn_gsv_UserSession_InitializeCameraContext(SvcObj* self, u64 version, u32* session) {
    (void)self, (void)version;
    *session = session_new(nn_gsv_CameraSession_new());
    R_UNLESS(*session, SNX_RC_SVC_OUT_OF_HANDLES); /* OutOfHandles */
    R_SUCCEED();
}
static int nn_gsv_UserSession_DestroyCameraContext(SvcObj* self) {
    (void)self;
    R_SUCCEED();
}
static int nn_gsv_CameraSession_GetEvent(SvcObj* self, u32 id, u32* event) {
    (void)self, (void)id;
    return new_event(event, "gsv camera", 0);
}
/* The device handle of the camera "Default", which the SDK gives back to the session in the calls on that camera.
   The session takes none of them while no camera is plugged in, so its value is the session's own choice */
static int nn_gsv_CameraSession_Initialize(SvcObj* self, const u8* name, u64* device) {
    (void)self;
    LOG("gsv: camera Initialize(\"%.16s\")\n", (const char*)name);
    *device = 0;
    R_SUCCEED();
}
static int nn_gsv_CameraSession_Finalize(SvcObj* self) {
    (void)self;
    R_SUCCEED();
}
static int nn_gsv_UserSession_OpenGraphicsFrameSession(SvcObj* self, u32* session) {
    (void)self;
    *session = session_new(nn_gsv_GraphicsFrameSession_new());
    R_UNLESS(*session, SNX_RC_SVC_OUT_OF_HANDLES); /* OutOfHandles */
    R_SUCCEED();
}
static int nn_gsv_GraphicsFrameSession_Ping(SvcObj* self, u64 token, u64* echo) {
    (void)self;
    *echo = token;
    R_SUCCEED();
}
/* the GPU time the application has used between two points, which nothing here measures: none */
static int nn_gsv_GraphicsFrameSession_QueryApplicationGpuTimeSpan(SvcObj* self, u64 first, u64 second, u64* span) {
    (void)self, (void)first, (void)second;
    *span = 0;
    R_SUCCEED();
}
static int nn_gsv_UserSession_CreateChatContext(SvcObj* self) {
    (void)self;
    R_SUCCEED();
}
static int nn_gsv_UserSession_InitializeChatContext(SvcObj* self, u32* session) {
    (void)self;
    *session = session_new(nn_gsv_ChatSession_new());
    R_UNLESS(*session, SNX_RC_SVC_OUT_OF_HANDLES); /* OutOfHandles */
    R_SUCCEED();
}
static int nn_gsv_UserSession_DestroyChatContext(SvcObj* self) {
    (void)self;
    R_SUCCEED();
}
static int nn_gsv_ChatSession_GetEventId(SvcObj* self, u32* id) {
    (void)self;
    *id = 0;
    R_SUCCEED();
}
static int nn_gsv_ChatSession_GetEvent(SvcObj* self, u32 id, u32* event) {
    (void)self, (void)id;
    return new_event(event, "gsv chat", 0);
}
static int nn_gsv_UserSession_CreateNativeMemory(SvcObj* self, const u8* request, u32* memory_handle) {
    u32 mode;
    u64 size;
    (void)self;
    memcpy(&mode, request, 4);
    memcpy(&size, request + 4, 8);
    R_UNLESS(size && !(size & 0xfff), SNX_RC_SVC_INVALID_SIZE);
    LOG("gsv: CreateNativeMemory(mode %u, size %#llx)\n", mode, (unsigned long long)size);
    R_UNLESS(g_hle.gsv.native_memories_made + 1 < GSV_HANDLES, SNX_RC_SVC_INVALID_CURRENT_MEMORY);
    *memory_handle = ++g_hle.gsv.native_memories_made;
    g_hle.gsv.native_memory_size[*memory_handle] = size;
    R_SUCCEED();
}
static int nn_gsv_UserSession_DestroyNativeMemory(SvcObj* self, u32 memory_handle) {
    (void)self;
    if (memory_handle < GSV_HANDLES) g_hle.gsv.native_memory_size[memory_handle] = 0;
    R_SUCCEED();
}
/* attributes 0: the native memory stays cached, which the runtime's guest memory always is. The answer's word is
   written, not left as the TLS's old bytes, which the SDK would read as attributes */
static int nn_gsv_UserSession_BootNativeMemoryManagement(SvcObj* self, u64 mode, u64* attributes) {
    (void)self;
    *attributes = 0;
    LOG("gsv: BootNativeMemoryManagement(mode %llu)\n", (unsigned long long)mode);
    R_SUCCEED();
}
/* 0: the SDK boots on without command 120, which the runtime doesn't have (gsv.swipc) */
static int nn_gsv_UserSession_GetPostBootMode(SvcObj* self, u32* mode) {
    (void)self;
    *mode = 0;
    LOG_LIMIT(1, "gsv: GetPostBootMode answers 0: the SDK skips command 120\n");
    R_SUCCEED();
}
static int nn_gsv_UserSession_CreateSyncptContext(SvcObj* self) {
    (void)self;
    R_SUCCEED();
}
static int nn_gsv_UserSession_CreateGraphicsFrameContext(SvcObj* self) {
    (void)self;
    R_SUCCEED();
}
static int nn_gsv_UserSession_InitializeGraphicsFrameContext(SvcObj* self) {
    (void)self;
    R_SUCCEED();
}
static int nn_gsv_UserSession_InitializeSyncptContext(SvcObj* self, u32* session) {
    (void)self;
    *session = session_new(nn_gsv_SyncptSession_new());
    R_UNLESS(*session, SNX_RC_SVC_OUT_OF_HANDLES);
    R_SUCCEED();
}
/* The SDK finds a syncpoint short of a threshold in the value page (TryWaitSyncPoint) and asks for it again: it makes
   the syncpoint's bit in the request page differ from the acknowledge page, the word's bit in their summaries likewise,
   signals the request event and waits for the completed event until the two agree. The value page is kept as syncpoints
   change (gsv_syncpt_changed), so answering is acknowledging. Without an answer the first fence it polled that hadn't
   passed failed: the end this service gave it was not one it could signal. 1 when event is that event */
int gsv_syncpoint_request(u32 event) {
    u32 i;
    if (!event || event != g_hle.gsv.sync_request_event) return 0;
    for (i = 0; i < 192; i++) gw64(g_hle.gsv.sync_ro + 0x40 + (u64)i * 8, syncpt_read(i));
    for (i = 0; i < 3; i++) gw64(g_hle.gsv.sync_ro + 0x648 + (u64)i * 8, gr64(g_hle.gsv.sync_rw + 8 + (u64)i * 8));
    gw64(g_hle.gsv.sync_ro + 0x640, gr64(g_hle.gsv.sync_rw));
    ev_signal(g_hle.gsv.sync_completed_event);
    return 1;
}
static int nn_gsv_UserSession_BindSyncpointMemory(SvcObj* self, u32 rw, u32 ro, const u8* request,
                                                  u32* request_event, u32* completed_event) {
    KTmem* rwt = (KTmem*)h_get_ref(rw, KO_TMEM);
    KTmem* rot = (KTmem*)h_get_ref(ro, KO_TMEM);
    static const u32 header[8] = {1, 0, 191, 0x40, 0, 0x640, 8, 0x648}; /* Eden's GraphicsSyncpointCache::Header */
    u32 i;
    (void)self, (void)request;
    /* the cache is 0x2000 bytes of the read-only memory, and the request the first 0x20 of the read-write one: a
       memory too short for its part is none to bind */
    if (!rwt || !rot || rot->size < 0x2000 || rwt->size < 0x20) {
        int rc = rwt && rot ? SNX_RC_SVC_INVALID_SIZE : SNX_RC_SVC_INVALID_HANDLE;
        if (rwt) kobj_unref(&rwt->o);
        if (rot) kobj_unref(&rot->o);
        R_THROW(rc);
    }
    g_hle.gsv.sync_rw = rwt->addr;
    g_hle.gsv.sync_ro = rot->addr;
    kobj_unref(&rwt->o);
    kobj_unref(&rot->o);
    memset(GP(g_hle.gsv.sync_ro), 0, 0x2000);
    for (i = 0; i < 8; i++) gw32(g_hle.gsv.sync_ro + i * 4, header[i]);
    for (i = 0; i < 192; i++) gw64(g_hle.gsv.sync_ro + 0x40 + (u64)i * 8, ~0ull); /* Unallocated */
    LOG("gsv: BindSyncpointMemory(rw %#llx, ro %#llx)\n", (unsigned long long)g_hle.gsv.sync_rw,
        (unsigned long long)g_hle.gsv.sync_ro);
    g_hle.gsv.sync_request_event = owned_event("gsv:syncpoint request");
    g_hle.gsv.sync_completed_event = owned_event("gsv:syncpoint completed");
    *request_event = ev_handle(g_hle.gsv.sync_request_event);
    *completed_event = ev_handle(g_hle.gsv.sync_completed_event);
    R_UNLESS(*request_event && *completed_event, SNX_RC_SVC_OUT_OF_HANDLES);
    R_SUCCEED();
}
static int nn_gsv_UserSession_CreateDisplayContext(SvcObj* self) {
    (void)self;
    R_SUCCEED();
}
static int nn_gsv_UserSession_FinalizeDisplayContext(SvcObj* self) {
    (void)self;
    R_SUCCEED();
}
static int nn_gsv_UserSession_InitializeDisplayContext(SvcObj* self, u64 version, u32* session) {
    (void)self;
    LOG("gsv: InitializeDisplayContext(version %#llx)\n", (unsigned long long)version);
    *session = session_new(nn_vi2_IVi2Session_new());
    R_UNLESS(*session, SNX_RC_SVC_OUT_OF_HANDLES);
    R_SUCCEED();
}
static int nn_gsv_UserSession_SetDisplayLayerImage(SvcObj* self, u32 layer_id, u32 image_handle) {
    (void)self;
    R_UNLESS(layer_id && image_handle && image_handle <= g_hle.gsv.native_images_made,
             SNX_RC_SVC_INVALID_CURRENT_MEMORY);
    R_SUCCEED();
}
static int nn_gsv_UserSession_CreateGpuContext(SvcObj* self) {
    (void)self;
    R_SUCCEED();
}
static int nn_gsv_UserSession_InitializeGpuContext(SvcObj* self) {
    (void)self;
    R_SUCCEED();
}
static int nn_gsv_UserSession_BindNativeMemory(SvcObj* self, const u8* request) {
    u32 memory_handle;
    u64 cpu, size;
    (void)self;
    memcpy(&memory_handle, request, 4);
    memcpy(&cpu, request + 4, 8);
    memcpy(&size, request + 12, 8);
    R_UNLESS(memory_handle && memory_handle < GSV_HANDLES && g_hle.gsv.native_memory_size[memory_handle] >= size &&
                 size && !(cpu & 0xfff) && !(size & 0xfff) && cpu < AS_END && size <= AS_END - cpu,
             SNX_RC_SVC_INVALID_CURRENT_MEMORY);
    g_hle.gsv.native_memory_cpu[memory_handle] = cpu;
    LOG("gsv: BindNativeMemory(%u, %#llx, %#llx)\n", memory_handle, (unsigned long long)cpu,
        (unsigned long long)size);
    R_SUCCEED();
}
static int nn_gsv_UserSession_BindNativeMemoryToCpu(SvcObj* self, u32 memory_handle, const u8* address,
                                                    const u8* size, const u8* flags, const u8* offset) {
    u64 cpu = snx_ld_le64(address), bytes = snx_ld_le64(size);
    (void)self;
    R_UNLESS(memory_handle && memory_handle < GSV_HANDLES && g_hle.gsv.native_memory_size[memory_handle] >= bytes &&
                 bytes && !(cpu & 0xfff) && !snx_ld_le64(offset) && mem_mapped_run(cpu, bytes) == bytes,
             SNX_RC_SVC_INVALID_CURRENT_MEMORY);
    g_hle.gsv.native_memory_cpu[memory_handle] = cpu;
    LOG("gsv: BindNativeMemoryToCpu(%u, %#llx, %#llx, flags %#llx)\n", memory_handle, (unsigned long long)cpu,
        (unsigned long long)bytes, (unsigned long long)snx_ld_le64(flags));
    R_SUCCEED();
}
int gsv_native_memory_cpu(u32 memory, u64* cpu, u64* size) {
    if (!memory || memory >= GSV_HANDLES || !g_hle.gsv.native_memory_size[memory] ||
        !g_hle.gsv.native_memory_cpu[memory])
        return 0;
    *cpu = g_hle.gsv.native_memory_cpu[memory];
    *size = g_hle.gsv.native_memory_size[memory];
    return 1;
}
u32 gsv_native_image_reserve(u32 count) {
    u32 first = g_hle.gsv.native_images_made + 1;
    if (!count || count > GSV_HANDLES - first) return 0;
    g_hle.gsv.native_images_made += count;
    return first;
}
/* ---- the video service's commands (native_video.c) */
static int nn_gsv_UserSession_MapNativeImage(SvcObj* self, u32 image, u32 one, u32 address_space, u8 mode, u64* first,
                                             u64* last, nn_gsv_VideoDescriptor* descriptor, u32* flags) {
    (void)self, (void)flags;
    R_UNLESS(one == 1 && (mode == 1 || mode == 3), SNX_RC_SVC_INVALID_SIZE);
    return native_video_gpu_map(image, address_space, first, last, descriptor->v);
}
static int nn_gsv_UserSession_UnmapNativeImage(SvcObj* self, u32 image, u32 address_space, u64 first, u64 last) {
    (void)self;
    return native_video_gpu_unmap(image, address_space, first, last);
}
static int nn_gsv_UserSession_QueryNativeImage(SvcObj* self, u32 image, u64* first, u64* last,
                                               nn_gsv_VideoDescriptor* descriptor, u32* flags) {
    (void)self, (void)first, (void)last, (void)flags;
    return native_video_gpu_query(image, descriptor->v);
}
static int nn_gsv_UserSession_InitializeVideoDecoder(SvcObj* self) {
    (void)self;
    return native_video_service(0, 1);
}
static int nn_gsv_UserSession_FinalizeVideoDecoder(SvcObj* self) {
    (void)self;
    return native_video_service(0, 0);
}
static int nn_gsv_UserSession_OpenVideoDecoderContext(SvcObj* self, u32* context) {
    (void)self;
    return native_video_open(0, context);
}
static int nn_gsv_UserSession_OpenVideoDecoderContextWithFlag(SvcObj* self, u8 flag, u32* context) {
    (void)self;
    LOG("gsv: OpenVideoDecoderContextWithFlag(%u)\n", flag);
    return native_video_open(0, context);
}
static int nn_gsv_UserSession_OpenVideoDecoderContextWithVersion(SvcObj* self, u8 flag, const u8* version,
                                                                 u32* context) {
    (void)self;
    LOG("gsv: OpenVideoDecoderContextWithVersion(%u, version %#llx)\n", flag,
        (unsigned long long)snx_ld_le64(version));
    return native_video_open(0, context);
}
static int nn_gsv_UserSession_InitializeVideoEncoder(SvcObj* self) {
    (void)self;
    return native_video_service(1, 1);
}
static int nn_gsv_UserSession_FinalizeVideoEncoder(SvcObj* self) {
    (void)self;
    return native_video_service(1, 0);
}
static int nn_gsv_UserSession_OpenVideoEncoderContext(SvcObj* self, u32* context) {
    (void)self;
    return native_video_open(1, context);
}
static int nn_gsv_UserSession_CreateVideoImagePool(SvcObj* self, const nn_gsv_VideoImagePreference* preference,
                                                   u32 count, u32 memory_handle, u32* image) {
    u32 words[16];
    (void)self;
    memcpy(words, preference, sizeof words);
    return native_video_pool_create(words, count, memory_handle, image);
}
static int nn_gsv_UserSession_DestroyVideoImagePool(SvcObj* self, u32 image) {
    (void)self;
    return native_video_pool_destroy(image);
}
static int nn_gsv_UserSession_QueryVideoImage(SvcObj* self, u32 image, nn_gsv_VideoDescriptor* descriptor) {
    (void)self;
    return native_video_query(image, descriptor->v);
}
static int nn_gsv_UserSession_CalculateVideoImageRequirements(SvcObj* self,
                                                              const nn_gsv_VideoImagePreference* preference,
                                                              u32 count, u64* size, u64* alignment) {
    u32 words[16];
    (void)self;
    memcpy(words, preference, sizeof words);
    return native_video_requirements(words, count, size, alignment);
}
static int nn_gsv_UserSession_CreateChatImagePool(SvcObj* self, const nn_gsv_VideoImagePreference* preference,
                                                  u32 count, u32 memory_handle, u32* image) {
    return nn_gsv_UserSession_CreateVideoImagePool(self, preference, count, memory_handle, image);
}
static int nn_gsv_UserSession_DestroyChatImagePool(SvcObj* self, u32 image) {
    return nn_gsv_UserSession_DestroyVideoImagePool(self, image);
}
static int nn_gsv_UserSession_CalculateChatImageRequirements(SvcObj* self,
                                                             const nn_gsv_VideoImagePreference* preference, u32 count,
                                                             u64* size, u64* alignment) {
    return nn_gsv_UserSession_CalculateVideoImageRequirements(self, preference, count, size, alignment);
}
static int nn_gsv_UserSession_CreateNativeImage(SvcObj* self, const u8* request, u32* image_handle) {
    u32 memory_handle;
    GsvNativeImage* img;
    (void)self;
    memcpy(&memory_handle, request + 0x44, 4);
    R_UNLESS(memory_handle && memory_handle < GSV_HANDLES && g_hle.gsv.native_memory_size[memory_handle],
             SNX_RC_SVC_INVALID_CURRENT_MEMORY);
    R_UNLESS(g_hle.gsv.native_images_made + 1 < GSV_HANDLES, SNX_RC_SVC_INVALID_CURRENT_MEMORY);
    *image_handle = g_hle.gsv.native_images_made + 1;
    img = &g_hle.gsv.native_images[*image_handle];
    img->memory_handle = memory_handle;
    memcpy(img->preference, request, sizeof img->preference); /* the 17 preference words, host order */
    snx_le32_array(img->preference, 17);
    g_hle.gsv.native_images_made++;
    LOG("gsv: CreateNativeImage(memory %u) -> %u\n", memory_handle, *image_handle);
    R_SUCCEED();
}
static int nn_gsv_UserSession_ExistsAddressSpace(SvcObj* self, u32 address_space) {
    (void)self, (void)address_space;
    R_SUCCEED();
}
static int nn_gsv_UserSession_CalculateNativeImageSize(SvcObj* self, const u8* preference, u64* size) {
    u32 p[17];
    u64 width, height, pitch_gobs, block_height, aligned_height, image_size;
    (void)self;
    memcpy(p, preference, sizeof p);
    /* Native color images use 64-byte GOB rows and a power-of-two pixel block height. The last word is the image
       count. This is the firmware layout recovered in Eden's PR #4588 for the Ounce, not a fixed display-sized guess. */
    R_UNLESS(p[0] == 0 && (p[1] == 1 || p[1] == 2) && p[3] == 0 &&
                 p[4] >= 3 && p[4] <= 8 && (p[6] == 1 || p[6] == 4) && p[7] == 1 && p[16],
             SNX_RC_SVC_INVALID_SIZE);
    for (u32 i = 8; i < 16; i++) R_UNLESS(p[i] == 0, SNX_RC_SVC_INVALID_SIZE);
    width = (p[2] & 0xffffu) + 1ull;
    height = (p[2] >> 16) + 1ull;
    pitch_gobs = p[5];
    R_UNLESS(pitch_gobs >= (width * 4 + 63) / 64 && pitch_gobs <= 4096, SNX_RC_SVC_INVALID_SIZE);
    block_height = 1ull << p[4];
    aligned_height = (height + block_height - 1) & ~(block_height - 1);
    image_size = pitch_gobs * 64 * aligned_height;
    R_UNLESS(image_size <= (1ull << 48) / p[16], SNX_RC_SVC_INVALID_SIZE);
    *size = image_size * p[16];
    R_SUCCEED();
}
static int nn_gsv_UserSession_CreateGpuSessionWithRemoteMemory(SvcObj* self, u64 version, u32* session) {
    (void)self;
    LOG("gsv: CreateGpuSessionWithRemoteMemory(version %llu)\n", (unsigned long long)version);
    *session = session_new(nn_gsv_GpuSession_new());
    R_UNLESS(*session, SNX_RC_SVC_OUT_OF_HANDLES);
    R_SUCCEED();
}
static int nn_gsv_GpuSession_GetEventIdRange(SvcObj* self, u32* min_id, u32* max_id) {
    (void)self;
    *min_id = 0;
    *max_id = 32;
    R_SUCCEED();
}
static int nn_gsv_GpuSession_CreateEvent(SvcObj* self, u32 id, u32* event) {
    (void)self, (void)id;
    return new_event(event, "gsv:gpu event", 1);
}
static int nn_gsv_GpuSession_RequestKUserWorkerSession(SvcObj* self, u32 id) {
    (void)self, (void)id;
    R_SUCCEED();
}
static int nn_gsv_GpuSession_GetKUserWorkerSession(SvcObj* self, u32* session) {
    (void)self;
    *session = session_new(nn_gsv_KUserWorkerSession_new());
    R_UNLESS(*session, SNX_RC_SVC_OUT_OF_HANDLES);
    R_SUCCEED();
}
static int nn_gsv_GpuSession_CreateGraphicsDevice(SvcObj* self, u64 name, u64 flags, u64 sdk_label, u8* device_type) {
    (void)self, (void)flags, (void)sdk_label;
    LOG("gsv: CreateGraphicsDevice(name %#llx)\n", (unsigned long long)name);
    *device_type = 1;
    R_SUCCEED();
}
static int nn_gsv_GpuSession_CreateGpuAddressSpace(SvcObj* self, u32* address_space) {
    (void)self;
    *address_space = 1;
    R_SUCCEED();
}
static int nn_gsv_GpuSession_GetUserPartitionGpuAddress(SvcObj* self, u32 address_space, u64* min_address,
                                                        u64* max_address) {
    (void)self, (void)address_space;
    *min_address = 0x8000000ull;
    *max_address = 0xc000000000ull;
    R_SUCCEED();
}
static int nn_gsv_GpuSession_GetGsvPartitionGpuAddress(SvcObj* self, u32 address_space, u64* min_address,
                                                       u64* max_address) {
    (void)self, (void)address_space;
    *min_address = 0xc000000000ull;
    *max_address = 0x10000000000ull;
    R_SUCCEED();
}
static int nn_gsv_KUserWorkerSession_CreateGpuMemory(SvcObj* self, const u8* request, u32* memory_handle) {
    MemBlock block;
    u64 start, end, options;
    u64 at;
    (void)self;
    memcpy(&start, request, sizeof start);
    memcpy(&end, request + 8, sizeof end);
    memcpy(&options, request + 16, sizeof options);
    LOG("gsv: CreateGpuMemory(%#llx..%#llx, options %#llx)\n", (unsigned long long)start,
        (unsigned long long)end, (unsigned long long)options);
    /* GSV's end address is inclusive. The SDK supplies the last byte of a page. */
    R_UNLESS(start <= end && !(start & 0xfff) && (end & 0xfff) == 0xfff && end < AS_END, SNX_RC_SVC_INVALID_ADDRESS);
    end++;
    for (at = start; at < end;) {
        mem_query(at, &block);
        R_UNLESS(block.addr <= at && block.size && block.addr + block.size > at &&
                     block.state != MS_FREE && (block.attr & MA_GPU_SHARABLE), SNX_RC_SVC_INVALID_CURRENT_MEMORY);
        at = block.addr + block.size < end ? block.addr + block.size : end;
    }
    R_UNLESS(g_hle.gsv.gpu_memories_made + 1 < GSV_HANDLES, SNX_RC_SVC_INVALID_CURRENT_MEMORY);
    *memory_handle = ++g_hle.gsv.gpu_memories_made;
    g_hle.gsv.gpu_memory[*memory_handle] = (GsvGpuMemory){start, end};
    R_SUCCEED();
}
static int nn_gsv_KUserWorkerSession_DestroyGpuMemory(SvcObj* self, u32 memory_handle, u32 event_id) {
    (void)self, (void)event_id;
    if (memory_handle < GSV_HANDLES) g_hle.gsv.gpu_memory[memory_handle] = (GsvGpuMemory){0, 0};
    R_SUCCEED();
}
static int nn_gsv_KUserWorkerSession_NormalMapGpuMemory4K(SvcObj* self, const u8* request) {
    u32 address_space, memory_handle;
    u64 gva, end, offset, size, cpu;
    (void)self;
    memcpy(&address_space, request, 4);
    memcpy(&gva, request + 4, 8);
    memcpy(&end, request + 12, 8);
    memcpy(&memory_handle, request + 20, 4);
    memcpy(&offset, request + 24, 8);
    R_UNLESS(address_space == 1 && memory_handle && memory_handle < GSV_HANDLES &&
                 g_hle.gsv.gpu_memory[memory_handle].end > g_hle.gsv.gpu_memory[memory_handle].start,
             SNX_RC_SVC_INVALID_CURRENT_MEMORY);
    R_UNLESS(gva <= end && !(gva & 0xfff) && (end & 0xfff) == 0xfff && end < (1ull << GMMU_ADDRESS_SPACE_BITS),
             SNX_RC_SVC_INVALID_ADDRESS);
    size = end - gva + 1;
    R_UNLESS(offset <= g_hle.gsv.gpu_memory[memory_handle].end - g_hle.gsv.gpu_memory[memory_handle].start &&
                 size <= g_hle.gsv.gpu_memory[memory_handle].end - g_hle.gsv.gpu_memory[memory_handle].start - offset,
             SNX_RC_SVC_INVALID_SIZE);
    cpu = g_hle.gsv.gpu_memory[memory_handle].start + offset;
    LOG("gsv: map GPU %#llx..%#llx to memory %u at %#llx\n", (unsigned long long)gva,
        (unsigned long long)end, memory_handle, (unsigned long long)cpu);
    pm_lock(&g_host.nvdrv.aslock);
    gmmu_map(gva, cpu, size);
    pm_unlock(&g_host.nvdrv.aslock);
    R_SUCCEED();
}
static int nn_gsv_KUserWorkerSession_NormalModifyGpuMemory4K(SvcObj* self, const u8* request) {
    (void)self, (void)request;
    R_SUCCEED();
}
static int nn_gsv_KUserWorkerSession_NormalUnmapGpuMemory4K(SvcObj* self, const u8* request) {
    (void)self, (void)request;
    R_SUCCEED();
}
static int nn_gsv_KUserWorkerSession_CreateSparseRegion(SvcObj* self, const u8* request) {
    u32 address_space;
    u64 begin, end;
    (void)self;
    memcpy(&address_space, request, 4);
    memcpy(&begin, request + 4, 8);
    memcpy(&end, request + 12, 8);
    LOG("gsv: CreateSparseRegion(as %u, %#llx..%#llx)\n", address_space, (unsigned long long)begin,
        (unsigned long long)end);
    R_SUCCEED();
}
static int nn_gsv_KUserWorkerSession_DestroySparseRegion(SvcObj* self, const u8* request) {
    (void)self, (void)request;
    R_SUCCEED();
}
static int nn_gsv_KUserWorkerSession_ExecuteSparseMapRequests(SvcObj* self, const u8* request, IpcBuf in,
                                                               IpcBuf out, u64* processed) {
    (void)self, (void)request, (void)in;
    if (out.size) memset(GP(out.addr), 0, out.size);
    *processed = 0;
    R_SUCCEED();
}
/* ============================================================ the Ounce display session (Eden's IVi2Session) */
/* The raw packet of each command is [type, word count, data...]; ScarletNX hands the data words as c->raw, so the token
   is the first two words, and later fields follow at their word offsets (Eden's packet[k] = raw word k - 2). */
#define VI2_DEFAULT_DISPLAY 0x00746c7561666564ull /* "default" */

/* The displays a game can name: the console's, and the three for GameShare's guests, which the SDK opens when a game
   asks for individual displays (nn::streamplay::CreateIndividualDisplays), whether anyone ever joins or not */
static const u64 vi2_display_names[] = {
    VI2_DEFAULT_DISPLAY,
    0x0031726579616c70ull, /* "player1" */
    0x0032726579616c70ull, /* "player2" */
    0x0033726579616c70ull, /* "player3" */
};
static_assert(sizeof vi2_display_names / sizeof vi2_display_names[0] == VI2_DISPLAYS, "gsv.h holds a display each");

static Vi2Display* vi2_display(u64 name) {
    u32 i;
    for (i = 0; i < VI2_DISPLAYS; i++)
        if (vi2_display_names[i] == name) return &g_hle.gsv.vi2_displays[i];
    return NULL;
}
static Vi2Client* vi2_client(u64 token) {
    u32 i;
    for (i = 0; i < VI2_CLIENTS; i++)
        if (g_hle.gsv.vi2_clients[i].token == token) return &g_hle.gsv.vi2_clients[i];
    return NULL;
}

/* a queued present, shown on the GPU thread once its fence is reached */
typedef struct {
    u64 cpu;
    u32 w, h, pitch, bhl;
    u32 client; /* vi2_clients index + 1, or 0 for none */
    u32 slot;
} Vi2Present;
static void vi2_present_cb(void* arg) {
    Vi2Present* p = (Vi2Present*)arg;
    i32 crop[4] = {0, 0, (i32)p->w, (i32)p->h};
    g_renderer->present_buffer(p->cpu, p->w, p->h, p->pitch, 3 /* block linear */, p->bhl, 1 /* RGBA8888 */, crop, 0,
                               0, 0, 0);
    atomic_fetch_add_explicit(&g_frames, 1, memory_order_relaxed);
    if (p->client) {
        Vi2Client* c = &g_hle.gsv.vi2_clients[p->client - 1];
        pm_lock(&g_host.gsv.lock);
        c->available |= 1ull << p->slot;
        g_hle.gsv.vi2_notifications[c->available_notification / 32] |= 1u << (c->available_notification % 32);
        if (g_hle.gsv.vi2_event) ev_signal(g_hle.gsv.vi2_event);
        pm_unlock(&g_host.gsv.lock);
    }
    free(p);
}

static int nn_vi2_IVi2Session_GetNotificationLimit(SvcObj* self, u32* limit) {
    (void)self;
    *limit = 0;
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_GetSessionLimit(SvcObj* self, u32* limit) {
    (void)self;
    *limit = 0;
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_GetRequestGroupLimit(SvcObj* self, u32* limit) {
    (void)self;
    *limit = 7;
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_GetNotificationEvent(SvcObj* self, const u8* request, u32* event) {
    (void)self, (void)request;
    if (!g_hle.gsv.vi2_event) g_hle.gsv.vi2_event = owned_event("vi2:notification");
    *event = ev_handle(g_hle.gsv.vi2_event);
    R_UNLESS(*event, SNX_RC_SVC_OUT_OF_HANDLES);
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_ReleaseNotificationEvent(SvcObj* self, const u8* request) {
    (void)self, (void)request;
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_GetNotifications(SvcObj* self, IpcBuf out) {
    (void)self;
    R_UNLESS(out.size >= sizeof g_hle.gsv.vi2_notifications, SNX_RC_VI2_BUFFER_TOO_SMALL);
    pm_lock(&g_host.gsv.lock);
    memcpy(GP(out.addr), g_hle.gsv.vi2_notifications, sizeof g_hle.gsv.vi2_notifications);
    memset(g_hle.gsv.vi2_notifications, 0, sizeof g_hle.gsv.vi2_notifications);
    if (g_hle.gsv.vi2_event) ev_clear(g_hle.gsv.vi2_event);
    pm_unlock(&g_host.gsv.lock);
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_CreateRequestGroupEvent(SvcObj* self, const u8* request, u32* event) {
    u32 group = snx_ld_le32(request);
    (void)self;
    R_UNLESS(group < 8, SNX_RC_VI2_OUT_OF_RESOURCE);
    if (!g_hle.gsv.vi2_group_events[group]) g_hle.gsv.vi2_group_events[group] = owned_event("vi2:request group");
    *event = ev_handle(g_hle.gsv.vi2_group_events[group]);
    R_UNLESS(*event, SNX_RC_SVC_OUT_OF_HANDLES);
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_DuplicateRequestGroupEvent(SvcObj* self, const u8* request, u32 copied, u32* event) {
    u32 group = snx_ld_le32(request);
    (void)self, (void)copied;
    R_UNLESS(group < 8 && g_hle.gsv.vi2_group_events[group], SNX_RC_VI2_NOT_FOUND);
    *event = ev_handle(g_hle.gsv.vi2_group_events[group]);
    R_UNLESS(*event, SNX_RC_SVC_OUT_OF_HANDLES);
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_ConfigureRequestGroups(SvcObj* self, const u8* request) {
    (void)self, (void)request;
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_OpenDisplay(SvcObj* self, const u8* request) {
    u64 name = snx_ld_le64(request);
    u32 flags = snx_ld_le32(request + 8);
    Vi2Display* d = vi2_display(name);
    (void)self;
    /* the SDK asks with flags 1 for the console's display and 4 for a guest's */
    R_UNLESS(d && (name != VI2_DEFAULT_DISPLAY || flags == 1), SNX_RC_VI2_NOT_FOUND);
    d->refs++;
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_CloseDisplay(SvcObj* self, const u8* request) {
    Vi2Display* d = vi2_display(snx_ld_le64(request));
    (void)self;
    R_UNLESS(d && d->refs, SNX_RC_VI2_NOT_FOUND);
    d->refs--;
    R_SUCCEED();
}
/* What a game is told of the display's HDR, as the renderer has it (renderer.h hdr_display): 1 with the peak and the
   paper white in nits, each of which the SDK reads as 16 bits; 0 with neither for an SDR display */
static int vi2_hdr(u32* peak, u32* paper_white) {
    *peak = *paper_white = 0;
    return g_renderer && g_renderer->hdr_display && g_renderer->hdr_display(peak, paper_white);
}
/* The SDK (nn::vi2's QueryDisplayHdrStatus) takes format 1 as SDR and 2 as HDR, and any other as an error; the 16
   bits after it are the display's maximum luminance in nits, which a game clamps its own peak to */
static int nn_vi2_IVi2Session_GetHdrStatus(SvcObj* self, const u8* request, u64* format, u32* peak) {
    u64 name = snx_ld_le64(request);
    u32 paper_white;
    (void)self;
    R_UNLESS(vi2_display(name), SNX_RC_VI2_NOT_FOUND);
    *format = vi2_hdr(peak, &paper_white) ? 2 : 1;
    R_SUCCEED();
}
/* nn::vi2's QueryDisplayHdrPaperWhite: the light of the display's SDR white in nits, 16 bits, which a game asks for
   after the status. 0 for an SDR display, as before a game could be told of an HDR one */
static int nn_vi2_IVi2Session_GetHdrPaperWhite(SvcObj* self, const u8* request, u32* paper_white) {
    u64 name = snx_ld_le64(request);
    u32 peak;
    (void)self;
    R_UNLESS(vi2_display(name), SNX_RC_VI2_NOT_FOUND);
    vi2_hdr(&peak, paper_white);
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_GetBufferMaxResolution(SvcObj* self, const u8* request, u64* resolution) {
    u64 name = snx_ld_le64(request);
    (void)self;
    R_UNLESS(vi2_display(name), SNX_RC_VI2_NOT_FOUND);
    /* One bit for the largest picture the display takes: 1 = 1280x720, 2 = 1920x1080, 4 = 2560x1440, 8 = 3840x2160
       (one game sizes its upscaler's output and its window's crop by it: 1280x720, 1920x1080 and 2560x1440 for the
       first three, its own most for 8, and 1280x720 for any other value). In its dock the console is on a TV, taken
       as a 3840x2160 one; in the hand its own screen is 1920x1080 */
    *resolution = snx_tv_mode() ? 8 : 2;
    R_SUCCEED();
}
/* The display's refresh rates, as the SDK codes them: 1 for 60 Hz, 2 for 120 Hz, each also its bit in the
   rate sets (107, 354), and 1 << 63 for VRR. A game turns a rate on or off (301), the display then runs at the
   fastest one that is on and the TV takes, and the game reads it back (103, 354) once the refresh rate event says it
   changed. One game turns 120 Hz on for a part made for a 120 Hz TV and checks that the display then runs at 120 Hz */
#define VI2_RATE_60  1ull
#define VI2_RATE_120 2ull

/* The rates the TV in the dock takes, as tv_refresh_rate says: 60 Hz, and 120 Hz with 2, or with 1 where the window's
   display refreshes at 119 Hz or more (119.88 Hz counts). Never VRR: the runtime paces no VRR */
static u64 vi2_rates_offered(void) {
    static int told;
    u64 ns = backend_display_refresh_ns();
    int on = g_settings.tv_refresh_rate == 2 || (g_settings.tv_refresh_rate == 1 && ns && ns <= 1000000000u / 119);
    if (!told++)
        LOG("gsv: the TV takes 60 Hz%s (tv_refresh_rate=%d, the window's display at %.2f Hz)\n",
            on ? " and 120 Hz" : "", g_settings.tv_refresh_rate, ns ? 1e9 / (double)ns : 0.0);
    return on ? VI2_RATE_60 | VI2_RATE_120 : VI2_RATE_60;
}
/* the console's display runs at 120 Hz or VI_REFRESH_HZ (vi.h), which present.c and nvn_queue.c pace by; a guest's
   display of GameShare at 60 Hz */
static u64 vi2_rate_now(u64 name) {
    return name == VI2_DEFAULT_DISPLAY && vi_refresh_hz() == 120 ? VI2_RATE_120 : VI2_RATE_60;
}
static int nn_vi2_IVi2Session_GetDisplayRefreshRate(SvcObj* self, const u8* request, u64* rate) {
    u64 name = snx_ld_le64(request);
    (void)self;
    R_UNLESS(vi2_display(name), SNX_RC_VI2_NOT_FOUND);
    *rate = vi2_rate_now(name);
    R_SUCCEED();
}
/* the rate as its bit: the same values as the codes for 60 and 120 Hz, and bit 63 (VRR on) never set */
static int nn_vi2_IVi2Session_GetDisplayRefreshRateBits(SvcObj* self, const u8* request, u64* rate) {
    return nn_vi2_IVi2Session_GetDisplayRefreshRate(self, request, rate);
}
/* the HDR formats' set is 1, as before anything read it */
static int nn_vi2_IVi2Session_GetDisplayModeCapability(SvcObj* self, const u8* request, u64* refresh_rates,
                                                       u64* hdr_formats, u64* refresh_rates2) {
    (void)self, (void)request;
    *refresh_rates = vi2_rates_offered();
    *hdr_formats = 1;
    *refresh_rates2 = 0;
    R_SUCCEED();
}
/* The value's bits turn the mask's rates on, its clear bits turn them off. 60 Hz stays on: the SDK stops a game that
   turns it off. A change of the console's display's rate signals the groups registered for the refresh rate event */
static int nn_vi2_IVi2Session_SetDisplayRefreshRateCapability(SvcObj* self, const u8* request) {
    u64 name = snx_ld_le64(request), value = snx_ld_le64(request + 8), mask = snx_ld_le64(request + 16);
    u32 hz, i;
    (void)self;
    R_UNLESS(vi2_display(name), SNX_RC_VI2_NOT_FOUND);
    if (name != VI2_DEFAULT_DISPLAY) R_SUCCEED();
    g_hle.gsv.vi2_rates_on = ((g_hle.gsv.vi2_rates_on & ~mask) | (value & mask)) | VI2_RATE_60;
    hz = (g_hle.gsv.vi2_rates_on & vi2_rates_offered() & VI2_RATE_120) ? 120 : VI_REFRESH_HZ;
    if ((g_hle.gsv.vi2_rates_on & VI2_RATE_120) && hz != 120)
        LOG_LIMIT(4, "gsv: the game turned 120 Hz on, which the TV doesn't take: the display stays at 60 Hz\n");
    if (hz == vi_refresh_hz()) R_SUCCEED();
    LOG("gsv: the display runs at %u Hz, as the game turned 120 Hz %s\n", hz, hz == 120 ? "on" : "off");
    atomic_store_explicit(&g_vi_refresh_hz, (int)hz, memory_order_relaxed);
    for (i = 0; i < 8; i++)
        if (g_hle.gsv.vi2_group_refresh[i] && g_hle.gsv.vi2_group_events[i]) ev_signal(g_hle.gsv.vi2_group_events[i]);
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_OpenEndpoint(SvcObj* self, const u8* request) {
    /* the display, then the endpoint's name: every display's is "default" too */
    Vi2Display* d = vi2_display(snx_ld_le64(request));
    u64 endpoint = snx_ld_le64(request + 8);
    u64 token = snx_ld_le64(request + 16);
    (void)self;
    R_UNLESS(endpoint == VI2_DEFAULT_DISPLAY && d && vi2_client(token) && !d->endpoint, SNX_RC_VI2_NOT_FOUND);
    d->endpoint = (u32)token;
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_CloseEndpoint(SvcObj* self, const u8* request) {
    Vi2Display* d = vi2_display(snx_ld_le64(request));
    u64 endpoint = snx_ld_le64(request + 8);
    (void)self;
    R_UNLESS(endpoint == VI2_DEFAULT_DISPLAY && d && d->endpoint, SNX_RC_VI2_NOT_FOUND);
    d->endpoint = 0;
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_RegisterClientState(SvcObj* self, const u8* request) {
    u64 token = snx_ld_le64(request);
    u32 first = snx_ld_le32(request + 8);
    u32 second = snx_ld_le32(request + 12);
    u32 slots, i;
    (void)self;
    R_UNLESS(token && first < 32 && second < 32 && first != second && !vi2_client(token), SNX_RC_VI2_INVALID_ARGUMENT);
    slots = (1u << first) | (1u << second);
    R_UNLESS(!(g_hle.gsv.vi2_client_slots & slots), SNX_RC_VI2_OUT_OF_RESOURCE);
    for (i = 0; i < VI2_CLIENTS; i++) {
        if (!g_hle.gsv.vi2_clients[i].token) {
            g_hle.gsv.vi2_clients[i].token = token;
            g_hle.gsv.vi2_clients[i].slots = slots;
            g_hle.gsv.vi2_clients[i].available = 0x7fff;
            g_hle.gsv.vi2_clients[i].retire_notification = first;
            g_hle.gsv.vi2_clients[i].available_notification = second;
            g_hle.gsv.vi2_client_slots |= slots;
            return 0;
        }
    }
    R_THROW(SNX_RC_VI2_OUT_OF_RESOURCE);
}
static int nn_vi2_IVi2Session_UnregisterClientState(SvcObj* self, const u8* request) {
    u64 token = snx_ld_le64(request);
    Vi2Client* c = vi2_client(token);
    u32 i;
    (void)self;
    R_UNLESS(token && c, SNX_RC_VI2_NOT_FOUND);
    g_hle.gsv.vi2_client_slots &= ~c->slots;
    for (i = 0; i < VI2_DISPLAYS; i++)
        if (g_hle.gsv.vi2_displays[i].endpoint == (u32)token) /* as OpenEndpoint keeps it: its low 32 bits */
            g_hle.gsv.vi2_displays[i].endpoint = 0;
    c->token = 0;
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_GetClientAvailability(SvcObj* self, const u8* request, u64* available) {
    u64 token = snx_ld_le64(request);
    Vi2Client* c = vi2_client(token);
    (void)self;
    R_UNLESS(token && c, SNX_RC_VI2_NOT_FOUND);
    *available = c->available;
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_CancelClientBuffer(SvcObj* self, const u8* request, u64* available, u64* reserved) {
    u64 token = snx_ld_le64(request);
    u32 slot = snx_ld_le32(request + 8) & 0xff;
    Vi2Client* c = vi2_client(token);
    (void)self;
    R_UNLESS(token && c && slot < 15, SNX_RC_VI2_NOT_FOUND);
    c->available |= 1ull << slot;
    *available = c->available;
    *reserved = 0;
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_PresentClientBuffer(SvcObj* self, const u8* request, u64* available, u64* reserved) {
    u64 token = snx_ld_le64(request);
    u32 slot = snx_ld_le32(request + 8) & 0xff;
    u32 image = snx_ld_le32(request + 12);
    u32 dims = snx_ld_le32(request + 20);
    u32 fence_id = ~snx_ld_le32(request + 0x50);
    u32 fence_val = snx_ld_le32(request + 0x58);
    Vi2Client* c = vi2_client(token);
    GsvNativeImage* img;
    Vi2Present* p;
    u32 w, h;
    (void)self;
    *available = 0;
    *reserved = 0;
    R_UNLESS(token && c && slot < 15, SNX_RC_VI2_NOT_FOUND);
    R_UNLESS(image && image <= g_hle.gsv.native_images_made, SNX_RC_VI2_NOT_FOUND);
    img = &g_hle.gsv.native_images[image];
    R_UNLESS(img->memory_handle && g_hle.gsv.native_memory_cpu[img->memory_handle], SNX_RC_VI2_NOT_FOUND);
    w = (dims & 0xffffu) + 1;
    h = (dims >> 16) + 1;
    R_UNLESS(w && h, SNX_RC_VI2_NOT_FOUND);
    c->available &= ~(1ull << slot); /* in use until the fence is reached */
    *available = c->available;
    p = (Vi2Present*)malloc(sizeof *p);
    if (!p) {
        c->available |= 1ull << slot;
        R_THROW(SNX_RC_VI2_OUT_OF_RESOURCE);
    }
    p->cpu = g_hle.gsv.native_memory_cpu[img->memory_handle];
    p->w = w;
    p->h = h;
    p->pitch = img->preference[5] * 64; /* pitch in 64-byte GOB rows */
    p->bhl = img->preference[4];
    p->client = (u32)(c - g_hle.gsv.vi2_clients) + 1;
    p->slot = slot;
    nv_gpu_call_after(1, &fence_id, &fence_val, vi2_present_cb, p);
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_BindClientDisplay(SvcObj* self, const u8* request) {
    u64 token = snx_ld_le64(request);
    u64 name = snx_ld_le64(request + 8);
    Vi2Client* c = vi2_client(token);
    Vi2Display* d = vi2_display(name);
    (void)self;
    R_UNLESS(token && c && d && d->refs, SNX_RC_VI2_NOT_FOUND);
    c->display = name;
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_UnbindClientDisplay(SvcObj* self, const u8* request) {
    u64 token = snx_ld_le64(request);
    Vi2Client* c = vi2_client(token);
    (void)self;
    R_UNLESS(token && c, SNX_RC_VI2_NOT_FOUND);
    c->display = 0;
    R_SUCCEED();
}
/* a request group monitor registration: kind 0 (cmd 261) is the display's vsync (signalled each 60 Hz tick); the
   other kinds (263..271) signal their group once, as Eden's RegisterRequestGroupMonitor does. The request is the
   display name and the group index. */
static int vi2_register_monitor(const u8* request, int kind) {
    u64 name = snx_ld_le64(request);
    u32 group = snx_ld_le32(request + 8);
    R_UNLESS(group < 8 && vi2_display(name) && g_hle.gsv.vi2_group_events[group], SNX_RC_VI2_NOT_FOUND);
    if (kind == 0)
        g_hle.gsv.vi2_group_vsync[group] = 1;
    else
        ev_signal(g_hle.gsv.vi2_group_events[group]);
    if (kind == 1) g_hle.gsv.vi2_group_refresh[group] = 1;
    if (kind == 2) g_hle.gsv.vi2_group_hdr[group] = 1;
    R_SUCCEED();
}
static int vi2_unregister_monitor(const u8* request, int kind) {
    u64 name = snx_ld_le64(request);
    u32 group = snx_ld_le32(request + 8);
    R_UNLESS(group < 8 && vi2_display(name), SNX_RC_VI2_NOT_FOUND);
    g_hle.gsv.vi2_group_vsync[group] = 0;
    if (kind == 1) g_hle.gsv.vi2_group_refresh[group] = 0;
    if (kind == 2) g_hle.gsv.vi2_group_hdr[group] = 0;
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_RegisterDisplayEventMonitor(SvcObj* self, const u8* request) {
    (void)self;
    return vi2_register_monitor(request, 0);
}
static int nn_vi2_IVi2Session_UnregisterDisplayEventMonitor(SvcObj* self, const u8* request) {
    (void)self;
    return vi2_unregister_monitor(request, 0);
}
static int nn_vi2_IVi2Session_RegisterRefreshRateMonitor(SvcObj* self, const u8* request) {
    (void)self;
    return vi2_register_monitor(request, 1);
}
static int nn_vi2_IVi2Session_UnregisterRefreshRateMonitor(SvcObj* self, const u8* request) {
    (void)self;
    return vi2_unregister_monitor(request, 1);
}
static int nn_vi2_IVi2Session_RegisterHdrStatusMonitor(SvcObj* self, const u8* request) {
    (void)self;
    return vi2_register_monitor(request, 2);
}
static int nn_vi2_IVi2Session_UnregisterHdrStatusMonitor(SvcObj* self, const u8* request) {
    (void)self;
    return vi2_unregister_monitor(request, 2);
}
static int nn_vi2_IVi2Session_RegisterDisplayMonitor267(SvcObj* self, const u8* request) {
    (void)self;
    return vi2_register_monitor(request, 3);
}
static int nn_vi2_IVi2Session_UnregisterDisplayMonitor268(SvcObj* self, const u8* request) {
    (void)self;
    return vi2_unregister_monitor(request, 3);
}
static int nn_vi2_IVi2Session_RegisterBufferMaxResolutionMonitor(SvcObj* self, const u8* request) {
    (void)self;
    return vi2_register_monitor(request, 4);
}
static int nn_vi2_IVi2Session_UnregisterBufferMaxResolutionMonitor(SvcObj* self, const u8* request) {
    (void)self;
    return vi2_unregister_monitor(request, 4);
}
static int nn_vi2_IVi2Session_RegisterDisplayMonitor271(SvcObj* self, const u8* request) {
    (void)self;
    return vi2_register_monitor(request, 5);
}
static int nn_vi2_IVi2Session_UnregisterDisplayMonitor272(SvcObj* self, const u8* request) {
    (void)self;
    return vi2_unregister_monitor(request, 5);
}
/* the display's 60 Hz vsync, from the frontend loop (vi_vsync_tick): signals every group registered for it, and the
   groups registered for the HDR status when the display's changed (the window on another display, or its HDR turned
   on or off), as the console's does when a TV is plugged in: the SDK asks for the status again only after its event */
void gsv_vsync_tick(void) {
    u32 i, peak, paper_white;
    int hdr = vi2_hdr(&peak, &paper_white);
    for (i = 0; i < 8; i++)
        if (g_hle.gsv.vi2_group_vsync[i] && g_hle.gsv.vi2_group_events[i]) ev_signal(g_hle.gsv.vi2_group_events[i]);
    if ((u32)hdr + 1 == g_hle.gsv.vi2_hdr_told) return;
    for (i = 0; i < 8 && g_hle.gsv.vi2_hdr_told; i++)
        if (g_hle.gsv.vi2_group_hdr[i] && g_hle.gsv.vi2_group_events[i]) ev_signal(g_hle.gsv.vi2_group_events[i]);
    g_hle.gsv.vi2_hdr_told = (u32)hdr + 1;
}
static int nn_vi2_IVi2Session_SetClientOutputEncoding(SvcObj* self, const u8* request) {
    u64 token = snx_ld_le64(request);
    (void)self;
    R_UNLESS(token && vi2_client(token), SNX_RC_VI2_NOT_FOUND);
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_SetClientOutputLevels(SvcObj* self, const u8* request) {
    u64 token = snx_ld_le64(request);
    (void)self;
    R_UNLESS(token && vi2_client(token), SNX_RC_VI2_NOT_FOUND);
    R_SUCCEED();
}
static int nn_gsv_SyncptSession_Ping(SvcObj* self, u64 tick, u64* out_tick) {
    (void)self;
    *out_tick = tick;
    R_SUCCEED();
}
static int nn_gsv_SyncptSession_AllocateSyncpoint(SvcObj* self, u32* id) {
    (void)self;
    *id = syncpt_alloc();
    R_SUCCEED();
}
static int nn_gsv_SyncptSession_SetSyncpointLimit(SvcObj* self, u32 id) {
    (void)self, (void)id;
    R_SUCCEED();
}
/* The armed syncpoint waits, by slot, under g_host.gsv.lock: a guest's thread arms and cancels them while the GPU's
   and the video's move the syncpoints. An arm that set its wait while a syncpoint moved was seen by neither side
   without it, and a wait armed at the threshold stayed unsignaled. The events are signaled under it too, so that a
   signal decided for the wait before never lands after a new arm cleared the event */
void gsv_syncpt_changed(void) {
    u32 i;
    pm_lock(&g_host.gsv.lock);
    for (i = 0; i < GSV_SYNC_WAITS; i++) {
        if (!g_hle.gsv.sync_waits[i].armed) continue;
        if ((i32)(syncpt_read(g_hle.gsv.sync_waits[i].id) - g_hle.gsv.sync_waits[i].thresh) >= 0) {
            LOGV("gsv: syncpoint %u reached %u, signaling slot %u\n", g_hle.gsv.sync_waits[i].id,
                 syncpt_read(g_hle.gsv.sync_waits[i].id), i);
            g_hle.gsv.sync_waits[i].armed = 0;
            if (g_hle.gsv.sync_waits[i].event) ev_signal(g_hle.gsv.sync_waits[i].event);
        }
    }
    pm_unlock(&g_host.gsv.lock);
    if (g_hle.gsv.sync_ro) /* the cache's value page: the game polls it directly */
        for (i = 0; i < 192; i++) gw64(g_hle.gsv.sync_ro + 0x40 + (u64)i * 8, syncpt_read(i));
    if (g_sync_shim_cpu) /* the syncpoint shim: 192 counters, 4 bytes each */
        for (i = 0; i < 192; i++) gw32(g_sync_shim_cpu + (u64)i * 4, syncpt_read(i));
}
static int nn_gsv_SyncptSession_GetEvent(SvcObj* self, u32 slot, u32* event) {
    (void)self;
    R_UNLESS(slot < GSV_SYNC_WAITS, SNX_RC_SVC_OUT_OF_HANDLES);
    /* the service keeps the kernel object id to signal it, and hands the game a handle of its own */
    pm_lock(&g_host.gsv.lock);
    g_hle.gsv.sync_waits[slot].event = owned_event("gsv:syncpoint");
    *event = ev_handle(g_hle.gsv.sync_waits[slot].event);
    pm_unlock(&g_host.gsv.lock);
    R_UNLESS(*event, SNX_RC_SVC_OUT_OF_HANDLES);
    R_SUCCEED();
}
static int nn_gsv_SyncptSession_QueryCounter(SvcObj* self, u32 id, u64* value, u32* reserved) {
    (void)self;
    *value = syncpt_read(~id); /* the SDK negates the syncpoint id in the packet */
    *reserved = 0;
    R_SUCCEED();
}
static int nn_gsv_SyncptSession_ArmWait(SvcObj* self, const u8* request) {
    u32 slot, id;
    u64 threshold;
    (void)self;
    memcpy(&slot, request, 4);
    memcpy(&id, request + 4, 4);
    memcpy(&threshold, request + 8, 8);
    R_UNLESS(slot < GSV_SYNC_WAITS, SNX_RC_SVC_OUT_OF_HANDLES);
    pm_lock(&g_host.gsv.lock);
    g_hle.gsv.sync_waits[slot].id = ~id;
    g_hle.gsv.sync_waits[slot].thresh = (u32)threshold;
    if (g_hle.gsv.sync_waits[slot].event)
        ev_clear(g_hle.gsv.sync_waits[slot].event); /* a re-arm clears the previous wait's signal */
    g_hle.gsv.sync_waits[slot].armed = 1;
    LOGV("gsv: ArmWait slot %u sp %u thresh %u (now %u)\n", slot, ~id, (u32)threshold, syncpt_read(~id));
    if ((i32)(syncpt_read(g_hle.gsv.sync_waits[slot].id) - g_hle.gsv.sync_waits[slot].thresh) >= 0) {
        /* already there: signal it now, as Eden's PollWaits does, so the game's wait on the event ends */
        g_hle.gsv.sync_waits[slot].armed = 0;
        if (g_hle.gsv.sync_waits[slot].event) ev_signal(g_hle.gsv.sync_waits[slot].event);
    }
    pm_unlock(&g_host.gsv.lock);
    R_SUCCEED();
}
/* The wait ends with the slot's event signaled, armed or not: the SDK's CancelSyncPointEvent (and
   DestroySyncPointEvent) waits on the event after 202, also for a new slot that was never armed (the video decoder's
   first wait on an image) */
static int nn_gsv_SyncptSession_CancelWait(SvcObj* self, u32 slot) {
    (void)self;
    R_UNLESS(slot < GSV_SYNC_WAITS, SNX_RC_SVC_OUT_OF_HANDLES);
    pm_lock(&g_host.gsv.lock);
    g_hle.gsv.sync_waits[slot].armed = 0;
    if (g_hle.gsv.sync_waits[slot].event) ev_signal(g_hle.gsv.sync_waits[slot].event);
    pm_unlock(&g_host.gsv.lock);
    R_SUCCEED();
}
static int nn_gsv_UserSession_CreateNvDrv2UserSessionWithRemoteMemory(SvcObj* self, u32* session) {
    (void)self;
    /* INvDrv2User uses the nvdrv command layouts for Open, Ioctl, Close and QueryEvent. Share those device handlers
       until a command or ioctl with different semantics needs its own implementation. */
    *session = session_new(svcobj_new("nn::nvdrv::INvDrv2User", nns_nvdrv_INvDrvServices_dispatch));
    R_UNLESS(*session, SNX_RC_SVC_OUT_OF_HANDLES);
    R_SUCCEED();
}
