/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The graphics supervisor bootstrap reached through am's GetGraphicsSupervisor. This is a TIPC service: the IPC
   parser subtracts 16 from the message type before dispatch. The syncpoint, GPU and display session handshakes below
   are experimental. They do not yet back the newer console's native GPU memory, worker and layer state. */
#include "nx/hle/services/services_int.h"
#include "nx/hle/hle.h"
#include "nx/gpu/renderer.h"
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
#include "hle/services/gsv.swipc.inc"
#include "hle/services/nvdrv.swipc.h"
#include "nx/hle/services/nvdrv.h"

SvcObj* new_gsv_session(void) {
    return nn_gsv_UserSession_new();
}
static u32 next_native_image_handle = 1;
/* a native image, as CreateNativeImage made it: the memory that backs it and its preference words (the layout
   CalculateNativeImageSize reads: [2] width|height<<16, [4] block height log2, [5] pitch in 64-byte GOBs, [6] format) */
typedef struct {
    u32 memory_handle;
    u32 preference[17];
} GsvNativeImage;
static GsvNativeImage native_images[4096];

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
static u64 native_memory_size[4096];
static u64 native_memory_cpu[4096]; /* the guest CPU address BindNativeMemory binds the memory to, 0 for none */
static u32 next_native_memory_handle = 1;
static int nn_gsv_UserSession_CreateNativeMemory(SvcObj* self, const u8* request, u32* memory_handle) {
    u32 mode;
    u64 size;
    (void)self;
    memcpy(&mode, request, 4);
    memcpy(&size, request + 4, 8);
    R_UNLESS(size && !(size & 0xfff), R_BADSIZE);
    LOG("gsv: CreateNativeMemory(mode %u, size %#llx)\n", mode, (unsigned long long)size);
    R_UNLESS(next_native_memory_handle < sizeof native_memory_size / sizeof native_memory_size[0], R_BADMEMSTATE);
    *memory_handle = next_native_memory_handle++;
    native_memory_size[*memory_handle] = size;
    R_SUCCEED();
}
static int nn_gsv_UserSession_DestroyNativeMemory(SvcObj* self, u32 memory_handle) {
    (void)self;
    if (memory_handle < sizeof native_memory_size / sizeof native_memory_size[0])
        native_memory_size[memory_handle] = 0;
    R_SUCCEED();
}
static int nn_gsv_UserSession_BootNativeMemoryManagement(SvcObj* self, u32 mode) {
    (void)self;
    LOG("gsv: BootNativeMemoryManagement(mode %u)\n", mode);
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
    R_UNLESS(*session, 0xD201); /* OutOfHandles */
    R_SUCCEED();
}
/* the syncpoint cache: the game's request page (RW) and the value page (RO) it polls, bound by command 204 */
static u64 gsv_sync_rw, gsv_sync_ro;
static int nn_gsv_UserSession_BindSyncpointMemory(SvcObj* self, u32 rw, u32 ro, const u8* request,
                                                  u32* request_event, u32* completed_event) {
    KTmem* rwt = (KTmem*)h_get_ref(rw, KO_TMEM);
    KTmem* rot = (KTmem*)h_get_ref(ro, KO_TMEM);
    static const u32 header[8] = {1, 0, 191, 0x40, 0, 0x640, 8, 0x648}; /* Eden's GraphicsSyncpointCache::Header */
    u32 i;
    (void)self, (void)request;
    R_UNLESS(rwt && rot, RC(1, 114)); /* InvalidHandle */
    gsv_sync_rw = rwt->addr;
    gsv_sync_ro = rot->addr;
    kobj_unref(&rwt->o);
    kobj_unref(&rot->o);
    memset(GP(gsv_sync_ro), 0, 0x2000);
    for (i = 0; i < 8; i++) gw32(gsv_sync_ro + i * 4, header[i]);
    for (i = 0; i < 192; i++) gw64(gsv_sync_ro + 0x40 + (u64)i * 8, ~0ull); /* Unallocated */
    LOG("gsv: BindSyncpointMemory(rw %#llx, ro %#llx)\n", (unsigned long long)gsv_sync_rw,
        (unsigned long long)gsv_sync_ro);
    *request_event = 0;
    *completed_event = 0;
    R_UNLESS(!new_event(request_event, "gsv:syncpoint request", 0) &&
                 !new_event(completed_event, "gsv:syncpoint completed", 0), 0xD201);
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
    R_UNLESS(*session, 0xD201);
    R_SUCCEED();
}
static int nn_gsv_UserSession_SetDisplayLayerImage(SvcObj* self, u32 layer_id, u32 image_handle) {
    (void)self;
    R_UNLESS(layer_id && image_handle && image_handle < next_native_image_handle, R_BADMEMSTATE);
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
    R_UNLESS(memory_handle && memory_handle < sizeof native_memory_size / sizeof native_memory_size[0] &&
                 native_memory_size[memory_handle] >= size && size && !(cpu & 0xfff) && !(size & 0xfff) &&
                 cpu < AS_END && size <= AS_END - cpu, R_BADMEMSTATE);
    native_memory_cpu[memory_handle] = cpu;
    LOG("gsv: BindNativeMemory(%u, %#llx, %#llx)\n", memory_handle, (unsigned long long)cpu,
        (unsigned long long)size);
    R_SUCCEED();
}
static int nn_gsv_UserSession_CreateNativeImage(SvcObj* self, const u8* request, u32* image_handle) {
    u32 memory_handle;
    GsvNativeImage* img;
    (void)self;
    memcpy(&memory_handle, request + 0x44, 4);
    R_UNLESS(memory_handle && memory_handle < sizeof native_memory_size / sizeof native_memory_size[0] &&
                 native_memory_size[memory_handle], R_BADMEMSTATE);
    R_UNLESS(next_native_image_handle < sizeof native_images / sizeof native_images[0], R_BADMEMSTATE);
    *image_handle = next_native_image_handle;
    img = &native_images[*image_handle];
    img->memory_handle = memory_handle;
    memcpy(img->preference, request, sizeof img->preference); /* the 17 preference words, host order */
    snx_le32_array(img->preference, 17);
    next_native_image_handle++;
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
       count. This is the firmware layout recovered in Eden's Switch 2 PR #4588, not a fixed display-sized guess. */
    R_UNLESS(p[0] == 0 && (p[1] == 1 || p[1] == 2) && p[3] == 0 &&
                 p[4] >= 3 && p[4] <= 8 && (p[6] == 1 || p[6] == 4) && p[7] == 1 && p[16],
             R_BADSIZE);
    for (u32 i = 8; i < 16; i++) R_UNLESS(p[i] == 0, R_BADSIZE);
    width = (p[2] & 0xffffu) + 1ull;
    height = (p[2] >> 16) + 1ull;
    pitch_gobs = p[5];
    R_UNLESS(pitch_gobs >= (width * 4 + 63) / 64 && pitch_gobs <= 4096, R_BADSIZE);
    block_height = 1ull << p[4];
    aligned_height = (height + block_height - 1) & ~(block_height - 1);
    image_size = pitch_gobs * 64 * aligned_height;
    R_UNLESS(image_size <= (1ull << 48) / p[16], R_BADSIZE);
    *size = image_size * p[16];
    R_SUCCEED();
}
static int nn_gsv_UserSession_CreateGpuSessionWithRemoteMemory(SvcObj* self, u64 version, u32* session) {
    (void)self;
    LOG("gsv: CreateGpuSessionWithRemoteMemory(version %llu)\n", (unsigned long long)version);
    *session = session_new(nn_gsv_GpuSession_new());
    R_UNLESS(*session, 0xD201);
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
    R_UNLESS(*session, 0xD201);
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
typedef struct { u64 start, end; } GsvGpuMemory;
static GsvGpuMemory gpu_memory[4096];
static u32 next_gpu_memory_handle = 1;
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
    R_UNLESS(start <= end && !(start & 0xfff) && (end & 0xfff) == 0xfff && end < AS_END, R_BADADDR);
    end++;
    for (at = start; at < end;) {
        mem_query(at, &block);
        R_UNLESS(block.addr <= at && block.size && block.addr + block.size > at &&
                     block.state != MS_FREE && (block.attr & MA_GPU_SHARABLE), R_BADMEMSTATE);
        at = block.addr + block.size < end ? block.addr + block.size : end;
    }
    R_UNLESS(next_gpu_memory_handle < sizeof gpu_memory / sizeof gpu_memory[0], R_BADMEMSTATE);
    *memory_handle = next_gpu_memory_handle++;
    gpu_memory[*memory_handle] = (GsvGpuMemory){start, end};
    R_SUCCEED();
}
static int nn_gsv_KUserWorkerSession_DestroyGpuMemory(SvcObj* self, u32 memory_handle, u32 event_id) {
    (void)self, (void)event_id;
    if (memory_handle < sizeof gpu_memory / sizeof gpu_memory[0])
        gpu_memory[memory_handle] = (GsvGpuMemory){0, 0};
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
    R_UNLESS(address_space == 1 && memory_handle &&
                 memory_handle < sizeof gpu_memory / sizeof gpu_memory[0] &&
                 gpu_memory[memory_handle].end > gpu_memory[memory_handle].start, R_BADMEMSTATE);
    R_UNLESS(gva <= end && !(gva & 0xfff) && (end & 0xfff) == 0xfff && end < (1ull << GMMU_ADDRESS_SPACE_BITS),
             R_BADADDR);
    size = end - gva + 1;
    R_UNLESS(offset <= gpu_memory[memory_handle].end - gpu_memory[memory_handle].start &&
                 size <= gpu_memory[memory_handle].end - gpu_memory[memory_handle].start - offset, R_BADSIZE);
    cpu = gpu_memory[memory_handle].start + offset;
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
/* ============================================================ the Switch 2 display session (Eden's IVi2Session) */
/* The raw packet of each command is [type, word count, data...]; ScarletNX hands the data words as c->raw, so the token
   is the first two words, and later fields follow at their word offsets (Eden's packet[k] = raw word k - 2). */
#define VI2_DEFAULT_DISPLAY 0x00746c7561666564ull /* "default" */
#define VI2_CLIENTS 16
#define VI2_MOD 9                                   /* nn::vi's result module */
#define VI2_NOT_FOUND RC(VI2_MOD, 7)

typedef struct {
    u64 token;
    u32 slots;                   /* the two slot bits (retire + available notifications) */
    u64 available;               /* the free-slot mask */
    u32 retire_notification;
    u32 available_notification;
    u64 display;                 /* the bound display name, 0 for none */
} Vi2Client;

static u32 vi2_event;            /* the notification event's kobj id */
static u32 vi2_notifications[32]; /* the notification bitmask */
static u32 vi2_display_refs;     /* OpenDisplay references on "default" */
static u32 vi2_endpoint;         /* 0, or the default endpoint's token */
static u32 vi2_group_events[8];  /* the request group events' kobj ids */
static u32 vi2_group_vsync[8];   /* 1 while a group is registered for the display's vsync (kind 0) */
static Vi2Client vi2_clients[VI2_CLIENTS];
static u32 vi2_client_slots;

static Vi2Client* vi2_client(u64 token) {
    u32 i;
    for (i = 0; i < VI2_CLIENTS; i++)
        if (vi2_clients[i].token == token) return &vi2_clients[i];
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
    __atomic_add_fetch(&g_frames, 1, __ATOMIC_RELAXED);
    if (p->client) {
        Vi2Client* c = &vi2_clients[p->client - 1];
        c->available |= 1ull << p->slot;
        vi2_notifications[c->available_notification / 32] |= 1u << (c->available_notification % 32);
        if (vi2_event) ev_signal(vi2_event);
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
    if (!vi2_event) vi2_event = owned_event("vi2:notification");
    *event = ev_handle(vi2_event);
    R_UNLESS(*event, 0xD201);
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_ReleaseNotificationEvent(SvcObj* self, const u8* request) {
    (void)self, (void)request;
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_GetNotifications(SvcObj* self, IpcBuf out) {
    (void)self;
    R_UNLESS(out.size >= sizeof vi2_notifications, RC(VI2_MOD, 6));
    memcpy(GP(out.addr), vi2_notifications, sizeof vi2_notifications);
    memset(vi2_notifications, 0, sizeof vi2_notifications);
    if (vi2_event) ev_clear(vi2_event);
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_CreateRequestGroupEvent(SvcObj* self, const u8* request, u32* event) {
    u32 group = snx_ld_le32(request);
    (void)self;
    R_UNLESS(group < 8, RC(VI2_MOD, 4));
    if (!vi2_group_events[group]) vi2_group_events[group] = owned_event("vi2:request group");
    *event = ev_handle(vi2_group_events[group]);
    R_UNLESS(*event, 0xD201);
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_DuplicateRequestGroupEvent(SvcObj* self, const u8* request, u32 copied, u32* event) {
    u32 group = snx_ld_le32(request);
    (void)self, (void)copied;
    R_UNLESS(group < 8 && vi2_group_events[group], VI2_NOT_FOUND);
    *event = ev_handle(vi2_group_events[group]);
    R_UNLESS(*event, 0xD201);
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_ConfigureRequestGroups(SvcObj* self, const u8* request) {
    (void)self, (void)request;
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_OpenDisplay(SvcObj* self, const u8* request) {
    u64 name = snx_ld_le64(request);
    u32 flags = snx_ld_le32(request + 8);
    (void)self;
    R_UNLESS(name == VI2_DEFAULT_DISPLAY && flags == 1, VI2_NOT_FOUND);
    vi2_display_refs++;
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_CloseDisplay(SvcObj* self, const u8* request) {
    u64 name = snx_ld_le64(request);
    (void)self;
    R_UNLESS(name == VI2_DEFAULT_DISPLAY && vi2_display_refs, VI2_NOT_FOUND);
    vi2_display_refs--;
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_GetHdrStatus(SvcObj* self, const u8* request, u64* format, u32* reserved) {
    u64 name = snx_ld_le64(request);
    (void)self;
    R_UNLESS(name == VI2_DEFAULT_DISPLAY, VI2_NOT_FOUND);
    *format = 1; /* SDR only */
    *reserved = 0;
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_GetDisplayState105(SvcObj* self, const u8* request, u32* value) {
    (void)self, (void)request;
    *value = 0;
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_GetBufferMaxResolution(SvcObj* self, const u8* request, u64* resolution) {
    u64 name = snx_ld_le64(request);
    (void)self;
    R_UNLESS(name == VI2_DEFAULT_DISPLAY, VI2_NOT_FOUND);
    *resolution = 1; /* 1280x720 */
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_GetDisplayState107(SvcObj* self, const u8* request, u64* a, u64* b, u64* c) {
    (void)self, (void)request;
    *a = 1;
    *b = 1;
    *c = 0;
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_OpenEndpoint(SvcObj* self, const u8* request) {
    u64 name = snx_ld_le64(request);
    u64 display_name = snx_ld_le64(request + 8);
    u64 token = snx_ld_le64(request + 16);
    (void)self;
    R_UNLESS(name == VI2_DEFAULT_DISPLAY && display_name == name && vi2_client(token) && !vi2_endpoint,
             VI2_NOT_FOUND);
    vi2_endpoint = (u32)token;
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_CloseEndpoint(SvcObj* self, const u8* request) {
    u64 name = snx_ld_le64(request);
    u64 display_name = snx_ld_le64(request + 8);
    (void)self;
    R_UNLESS(name == VI2_DEFAULT_DISPLAY && display_name == name && vi2_endpoint, VI2_NOT_FOUND);
    vi2_endpoint = 0;
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_RegisterClientState(SvcObj* self, const u8* request) {
    u64 token = snx_ld_le64(request);
    u32 first = snx_ld_le32(request + 8);
    u32 second = snx_ld_le32(request + 12);
    u32 slots, i;
    (void)self;
    R_UNLESS(token && first < 32 && second < 32 && first != second && !vi2_client(token), RC(VI2_MOD, 5));
    slots = (1u << first) | (1u << second);
    R_UNLESS(!(vi2_client_slots & slots), RC(VI2_MOD, 4));
    for (i = 0; i < VI2_CLIENTS; i++) {
        if (!vi2_clients[i].token) {
            vi2_clients[i].token = token;
            vi2_clients[i].slots = slots;
            vi2_clients[i].available = 0x7fff;
            vi2_clients[i].retire_notification = first;
            vi2_clients[i].available_notification = second;
            vi2_client_slots |= slots;
            return 0;
        }
    }
    R_THROW(RC(VI2_MOD, 4));
}
static int nn_vi2_IVi2Session_UnregisterClientState(SvcObj* self, const u8* request) {
    u64 token = snx_ld_le64(request);
    Vi2Client* c = vi2_client(token);
    (void)self;
    R_UNLESS(token && c, VI2_NOT_FOUND);
    vi2_client_slots &= ~c->slots;
    if (vi2_endpoint == token) vi2_endpoint = 0;
    c->token = 0;
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_GetClientAvailability(SvcObj* self, const u8* request, u64* available) {
    u64 token = snx_ld_le64(request);
    Vi2Client* c = vi2_client(token);
    (void)self;
    R_UNLESS(token && c, VI2_NOT_FOUND);
    *available = c->available;
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_CancelClientBuffer(SvcObj* self, const u8* request, u64* available, u64* reserved) {
    u64 token = snx_ld_le64(request);
    u32 slot = snx_ld_le32(request + 8) & 0xff;
    Vi2Client* c = vi2_client(token);
    (void)self;
    R_UNLESS(token && c && slot < 15, VI2_NOT_FOUND);
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
    R_UNLESS(token && c && slot < 15, VI2_NOT_FOUND);
    R_UNLESS(image && image < next_native_image_handle, VI2_NOT_FOUND);
    img = &native_images[image];
    R_UNLESS(img->memory_handle && native_memory_cpu[img->memory_handle], VI2_NOT_FOUND);
    w = (dims & 0xffffu) + 1;
    h = (dims >> 16) + 1;
    R_UNLESS(w && h, VI2_NOT_FOUND);
    c->available &= ~(1ull << slot); /* in use until the fence is reached */
    *available = c->available;
    p = (Vi2Present*)malloc(sizeof *p);
    if (!p) {
        c->available |= 1ull << slot;
        R_THROW(RC(VI2_MOD, 4));
    }
    p->cpu = native_memory_cpu[img->memory_handle];
    p->w = w;
    p->h = h;
    p->pitch = img->preference[5] * 64; /* pitch in 64-byte GOB rows */
    p->bhl = img->preference[4];
    p->client = (u32)(c - vi2_clients) + 1;
    p->slot = slot;
    nv_gpu_call_after(1, &fence_id, &fence_val, vi2_present_cb, p);
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_BindClientDisplay(SvcObj* self, const u8* request) {
    u64 token = snx_ld_le64(request);
    u64 name = snx_ld_le64(request + 8);
    Vi2Client* c = vi2_client(token);
    (void)self;
    R_UNLESS(token && c && name == VI2_DEFAULT_DISPLAY && vi2_display_refs, VI2_NOT_FOUND);
    c->display = name;
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_UnbindClientDisplay(SvcObj* self, const u8* request) {
    u64 token = snx_ld_le64(request);
    Vi2Client* c = vi2_client(token);
    (void)self;
    R_UNLESS(token && c, VI2_NOT_FOUND);
    c->display = 0;
    R_SUCCEED();
}
/* a request group monitor registration: kind 0 (cmd 261) is the display's vsync (signalled each 60 Hz tick); the
   other kinds (263..271) signal their group once, as Eden's RegisterRequestGroupMonitor does. The request is the
   display name and the group index. */
static int vi2_register_monitor(const u8* request, int kind) {
    u64 name = snx_ld_le64(request);
    u32 group = snx_ld_le32(request + 8);
    R_UNLESS(group < 8 && name == VI2_DEFAULT_DISPLAY && vi2_group_events[group], VI2_NOT_FOUND);
    if (kind == 0)
        vi2_group_vsync[group] = 1;
    else
        ev_signal(vi2_group_events[group]);
    R_SUCCEED();
}
static int vi2_unregister_monitor(const u8* request) {
    u64 name = snx_ld_le64(request);
    u32 group = snx_ld_le32(request + 8);
    R_UNLESS(group < 8 && name == VI2_DEFAULT_DISPLAY, VI2_NOT_FOUND);
    vi2_group_vsync[group] = 0;
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_RegisterDisplayEventMonitor(SvcObj* self, const u8* request) {
    (void)self;
    return vi2_register_monitor(request, 0);
}
static int nn_vi2_IVi2Session_UnregisterDisplayEventMonitor(SvcObj* self, const u8* request) {
    (void)self;
    return vi2_unregister_monitor(request);
}
static int nn_vi2_IVi2Session_RegisterDisplayMonitor263(SvcObj* self, const u8* request) {
    (void)self;
    return vi2_register_monitor(request, 1);
}
static int nn_vi2_IVi2Session_UnregisterDisplayMonitor264(SvcObj* self, const u8* request) {
    (void)self;
    return vi2_unregister_monitor(request);
}
static int nn_vi2_IVi2Session_RegisterHdrStatusMonitor(SvcObj* self, const u8* request) {
    (void)self;
    return vi2_register_monitor(request, 2);
}
static int nn_vi2_IVi2Session_UnregisterHdrStatusMonitor(SvcObj* self, const u8* request) {
    (void)self;
    return vi2_unregister_monitor(request);
}
static int nn_vi2_IVi2Session_RegisterDisplayMonitor267(SvcObj* self, const u8* request) {
    (void)self;
    return vi2_register_monitor(request, 3);
}
static int nn_vi2_IVi2Session_UnregisterDisplayMonitor268(SvcObj* self, const u8* request) {
    (void)self;
    return vi2_unregister_monitor(request);
}
static int nn_vi2_IVi2Session_RegisterBufferMaxResolutionMonitor(SvcObj* self, const u8* request) {
    (void)self;
    return vi2_register_monitor(request, 4);
}
static int nn_vi2_IVi2Session_UnregisterBufferMaxResolutionMonitor(SvcObj* self, const u8* request) {
    (void)self;
    return vi2_unregister_monitor(request);
}
static int nn_vi2_IVi2Session_RegisterDisplayMonitor271(SvcObj* self, const u8* request) {
    (void)self;
    return vi2_register_monitor(request, 5);
}
static int nn_vi2_IVi2Session_UnregisterDisplayMonitor272(SvcObj* self, const u8* request) {
    (void)self;
    return vi2_unregister_monitor(request);
}
/* the display's 60 Hz vsync, from the frontend loop (vi_vsync_tick): signals every group registered for it */
void gsv_vsync_tick(void) {
    u32 i;
    for (i = 0; i < 8; i++)
        if (vi2_group_vsync[i] && vi2_group_events[i]) ev_signal(vi2_group_events[i]);
}
static int nn_vi2_IVi2Session_SetDisplayParameter301(SvcObj* self, const u8* request) {
    (void)self, (void)request;
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_SetClientOutputEncoding(SvcObj* self, const u8* request) {
    u64 token = snx_ld_le64(request);
    (void)self;
    R_UNLESS(token && vi2_client(token), VI2_NOT_FOUND);
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_SetClientOutputLevels(SvcObj* self, const u8* request) {
    u64 token = snx_ld_le64(request);
    (void)self;
    R_UNLESS(token && vi2_client(token), VI2_NOT_FOUND);
    R_SUCCEED();
}
static int nn_vi2_IVi2Session_GetDisplayState354(SvcObj* self, const u8* request, u64* value) {
    u64 name = snx_ld_le64(request);
    (void)self;
    R_UNLESS(name == VI2_DEFAULT_DISPLAY, VI2_NOT_FOUND);
    *value = 1;
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
/* the armed syncpoint waits, by slot: the syncpoint and threshold, and the event to signal once it reaches */
#define SYNC_WAITS 63
static struct { u32 id, thresh, event; int armed; } sync_waits[SYNC_WAITS];
void gsv_syncpt_changed(void) {
    u32 i;
    for (i = 0; i < SYNC_WAITS; i++) {
        if (!sync_waits[i].armed) continue;
        if ((i32)(syncpt_read(sync_waits[i].id) - sync_waits[i].thresh) >= 0) {
            LOG("gsv: syncpoint %u reached %u, signaling slot %u\n", sync_waits[i].id,
                syncpt_read(sync_waits[i].id), i);
            sync_waits[i].armed = 0;
            if (sync_waits[i].event) ev_signal(sync_waits[i].event);
        }
    }
    if (gsv_sync_ro) /* the cache's value page: the game polls it directly */
        for (i = 0; i < 192; i++) gw64(gsv_sync_ro + 0x40 + (u64)i * 8, syncpt_read(i));
    if (g_sync_shim_cpu) /* the syncpoint shim: 192 counters, 4 bytes each */
        for (i = 0; i < 192; i++) gw32(g_sync_shim_cpu + (u64)i * 4, syncpt_read(i));
}
static int nn_gsv_SyncptSession_GetEvent(SvcObj* self, u32 slot, u32* event) {
    (void)self;
    R_UNLESS(slot < SYNC_WAITS, 0xD201);
    /* the service keeps the kernel object id to signal it, and hands the game a handle of its own */
    sync_waits[slot].event = owned_event("gsv:syncpoint");
    *event = ev_handle(sync_waits[slot].event);
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
    R_UNLESS(slot < SYNC_WAITS, 0xD201);
    sync_waits[slot].id = ~id;
    sync_waits[slot].thresh = (u32)threshold;
    if (sync_waits[slot].event) ev_clear(sync_waits[slot].event); /* a re-arm clears the previous wait's signal */
    sync_waits[slot].armed = 1;
    LOG("gsv: ArmWait slot %u sp %u thresh %u (now %u)\n", slot, ~id, (u32)threshold, syncpt_read(~id));
    if ((i32)(syncpt_read(sync_waits[slot].id) - sync_waits[slot].thresh) >= 0) {
        /* already there: signal it now, as Eden's PollWaits does, so the game's wait on the event ends */
        sync_waits[slot].armed = 0;
        if (sync_waits[slot].event) ev_signal(sync_waits[slot].event);
    }
    R_SUCCEED();
}
static int nn_gsv_SyncptSession_CancelWait(SvcObj* self, u32 slot) {
    (void)self;
    R_UNLESS(slot < SYNC_WAITS, 0xD201);
    sync_waits[slot].armed = 0;
    R_SUCCEED();
}
static int nn_gsv_UserSession_CreateNvDrv2UserSessionWithRemoteMemory(SvcObj* self, u32* session) {
    (void)self;
    /* INvDrv2User uses the nvdrv command layouts for Open, Ioctl, Close and QueryEvent. Share those device handlers
       until a command or ioctl with different semantics needs its own implementation. */
    *session = session_new(svcobj_new("nn::nvdrv::INvDrv2User", nns_nvdrv_INvDrvServices_dispatch));
    R_UNLESS(*session, 0xD201); /* OutOfHandles */
    R_SUCCEED();
}
