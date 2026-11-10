/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Vita's imports given memory that isn't the game's: a buffer or a pointer at an address no segment, allocation
   or the runtime's page holds gets the call's error, and the call neither reads nor writes there. Here no data abort
   handler runs (vita_run arms it), so a call that reaches such an address ends this test with the host's fault */
#include "vita/hle/hle.h"
#include "vita/gpu/gxm.h"
#include "common/utilities/common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNMAPPED                              0x10000000u /* no segment (the image has none), allocation or page */
#define SCE_APPUTIL_ERROR_PARAMETER           0x80100600u
#define SCE_NET_ERROR_EFAULT                  0x8041010eu
#define SCE_NET_ERROR_EWOULDBLOCK             0x80410123u
#define SCE_NET_CTL_ERROR_INVALID_ADDR        0x80412107u
#define SCE_NP_ERROR_INVALID_ARGUMENT         0x80550003u
#define SCE_NP_MANAGER_ERROR_INVALID_ARGUMENT 0x80550503u
#define SCE_NP_UTIL_ERROR_INVALID_ARGUMENT    0x80550601u
#define SLOT_PARAM_SIZE                       0x34cu

/* the import name of table called by t with args (r0 to r3, then its stack) */
static uint32_t call(VitaProcess* p, VitaThread* t, const VitaHle* table, const char* name, const uint32_t* args,
                     unsigned count) {
    const VitaHle* h = table;
    while (h->name && strcmp(h->name, name)) ++h;
    assert(h->name);
    for (unsigned i = 0; i < count; ++i)
        if (i < 4)
            t->cpu.r[i] = args[i];
        else
            vita_write32(p, t->cpu.r[13] + (i - 4) * 4, args[i]);
    return h->function(p, t);
}
#define CALL(table, name, ...)                                                                                         \
    call(p, t, table, name, (const uint32_t[]){__VA_ARGS__}, sizeof((const uint32_t[]){__VA_ARGS__}) / sizeof(uint32_t))

/* sceNet's errno, which a failed call sets too */
static uint32_t net_errno(VitaProcess* p, VitaThread* t) { return vita_read32(p, t->tls + VITA_TLS_NET_ERRNO); }
static void sockaddr(VitaProcess* p, uint32_t at, uint16_t port) {
    unsigned char* a = vita_ptr(p, at);
    memset(a, 0, 16);
    a[0] = 16, a[1] = 2;                                           /* its length, AF_INET */
    a[2] = (unsigned char)(port >> 8), a[3] = (unsigned char)port; /* network order */
    a[4] = 127, a[7] = 1;                                          /* 127.0.0.1 */
}
static bool save_file_exists(VitaProcess* p, const char* name) {
    char* path = snx_path_join(p->savedata, name);
    bool there = path && snx_path_exists(path);
    free(path);
    return there;
}

/* SceAppUtil's save data (vita/hle/misc.c): a slot's parameters, the slot and the files given to DataSave, each
   file's buffer and the place for the KiB it needs */
static void save_data(VitaProcess* p, VitaThread* t, uint32_t mem, uint32_t end) {
    const VitaHle* misc = vita_hle_misc;
    uint32_t param = mem, slot = mem + 0x400, files = mem + 0x440, path = mem + 0x500, data = mem + 0x600,
             required = mem + 0x700;
    memset(vita_ptr(p, param), 0, SLOT_PARAM_SIZE);
    assert(CALL(misc, "sceAppUtilSaveDataSlotCreate", 0, UNMAPPED, 0) == SCE_APPUTIL_ERROR_PARAMETER);
    assert(CALL(misc, "sceAppUtilSaveDataSlotCreate", 0, param, 0) == 0);
    assert(CALL(misc, "sceAppUtilSaveDataSlotGetParam", 0, UNMAPPED, 0) == SCE_APPUTIL_ERROR_PARAMETER);
    assert(CALL(misc, "sceAppUtilSaveDataSlotSetParam", 0, UNMAPPED, 0) == SCE_APPUTIL_ERROR_PARAMETER);
    assert(CALL(misc, "sceAppUtilSaveDataSlotGetParam", 0, param, 0) == 0);

    /* one file (0x40 bytes: path, buffer, size, offset at 0x10, mode at 0x18), file.bin, 16 bytes at data,
       truncating (mode 1); the slot (id, parameters) is 0, its parameters the ones it has */
    snprintf(vita_ptr(p, path), 16, "file.bin");
    memcpy(vita_ptr(p, data), "0123456789abcdef", 16);
    memset(vita_ptr(p, files), 0, 0x40);
    vita_write32(p, files, path), vita_write32(p, files + 8, 16), vita_write32(p, files + 0x18, 1);
    vita_write32(p, slot, 0), vita_write32(p, slot + 4, 0);
    /* the file's buffer outside the game's memory, or running past its end: refused, nothing written */
    vita_write32(p, files + 4, UNMAPPED);
    assert(CALL(misc, "sceAppUtilSaveDataDataSave", slot, files, 1, 0, 0) == SCE_APPUTIL_ERROR_PARAMETER);
    vita_write32(p, files + 4, end - 8);
    assert(CALL(misc, "sceAppUtilSaveDataDataSave", slot, files, 1, 0, 0) == SCE_APPUTIL_ERROR_PARAMETER);
    assert(!save_file_exists(p, "file.bin"));
    vita_write32(p, files + 4, data);
    /* the slot, the files (or as many as wrap their size around), its parameters and required: the same */
    assert(CALL(misc, "sceAppUtilSaveDataDataSave", UNMAPPED, files, 1, 0, 0) == SCE_APPUTIL_ERROR_PARAMETER);
    assert(CALL(misc, "sceAppUtilSaveDataDataSave", slot, UNMAPPED, 1, 0, 0) == SCE_APPUTIL_ERROR_PARAMETER);
    assert(CALL(misc, "sceAppUtilSaveDataDataSave", slot, files, 0x04000000u, 0, 0) == SCE_APPUTIL_ERROR_PARAMETER);
    vita_write32(p, slot + 4, UNMAPPED);
    assert(CALL(misc, "sceAppUtilSaveDataDataSave", slot, files, 1, 0, 0) == SCE_APPUTIL_ERROR_PARAMETER);
    vita_write32(p, slot + 4, 0);
    assert(CALL(misc, "sceAppUtilSaveDataDataSave", slot, files, 1, 0, UNMAPPED) == SCE_APPUTIL_ERROR_PARAMETER);
    assert(!save_file_exists(p, "file.bin"));
    /* all of it the game's: the file written, required says there is room */
    vita_write32(p, required, 0xffffffffu);
    assert(CALL(misc, "sceAppUtilSaveDataDataSave", slot, files, 1, 0, required) == 0);
    assert(vita_read32(p, required) == 0);
    char* saved = snx_path_join(p->savedata, "file.bin");
    size_t size = 0;
    unsigned char* bytes = saved ? snx_read_file(saved, &size) : NULL;
    assert(bytes && size == 16 && !memcmp(bytes, "0123456789abcdef", 16));
    free(bytes);
    free(saved);

    /* DataRemove's slot and items (0x2c bytes: the path, the mode) outside the game's memory, or as many items as wrap
       their size around: refused, the file kept */
    uint32_t items = mem + 0x800;
    memset(vita_ptr(p, items), 0, 0x2c);
    vita_write32(p, items, path);
    assert(CALL(misc, "sceAppUtilSaveDataDataRemove", UNMAPPED, items, 1, 0) == SCE_APPUTIL_ERROR_PARAMETER);
    assert(CALL(misc, "sceAppUtilSaveDataDataRemove", slot, UNMAPPED, 1, 0) == SCE_APPUTIL_ERROR_PARAMETER);
    assert(CALL(misc, "sceAppUtilSaveDataDataRemove", slot, items, 0x05d1745eu, 0) == SCE_APPUTIL_ERROR_PARAMETER);
    assert(save_file_exists(p, "file.bin"));
    /* the game's: the file and the slot removed */
    assert(CALL(misc, "sceAppUtilSaveDataDataRemove", slot, items, 1, 0) == 0);
    assert(!save_file_exists(p, "file.bin") && !save_file_exists(p, "sce_sys/slot0.bin"));
}

/* sceNet (vita/hle/net.c): a socket's address read or written, a datagram sent from or received into the game's
   memory, the epoll events, sceNetCtl's and NP's out parameters */
static void net(VitaProcess* p, VitaThread* t, uint32_t mem, uint32_t end) {
    const VitaHle* n = vita_hle_net;
    uint32_t to = mem + 0x1000, from = mem + 0x1010, length = mem + 0x1020, value = mem + 0x1030, data = mem + 0x1100,
             got = mem + 0x1200, event = mem + 0x1300, events = mem + 0x1400, id = mem + 0x1500;
    uint32_t receiver = CALL(n, "sceNetSocket", 0, 2, 2, 0), sender = CALL(n, "sceNetSocket", 0, 2, 2, 0);
    assert(receiver && receiver < 0x80000000u && sender && sender < 0x80000000u && receiver != sender);
    sockaddr(p, to, 5000);
    assert(CALL(n, "sceNetBind", receiver, UNMAPPED, 16) == SCE_NET_ERROR_EFAULT && net_errno(p, t) == 14);
    assert(CALL(n, "sceNetBind", receiver, to, 16) == 0);

    /* sent from outside the game's memory, or to an address there: EFAULT, nothing queued */
    assert(CALL(n, "sceNetSendto", sender, UNMAPPED, 8, 0, to, 16) == SCE_NET_ERROR_EFAULT && net_errno(p, t) == 14);
    assert(CALL(n, "sceNetSendto", sender, data, 8, 0, UNMAPPED, 16) == SCE_NET_ERROR_EFAULT);
    assert(CALL(n, "sceNetRecvfrom", receiver, got, 8, 0x80, 0, 0) == SCE_NET_ERROR_EWOULDBLOCK); /* none queued */
    memcpy(vita_ptr(p, data), "datagram", 8);
    assert(CALL(n, "sceNetSendto", sender, data, 8, 0, to, 16) == 8);
    /* received into memory that isn't the game's, or with the sender's address or its length there: EFAULT, and the
       datagram stays for the next call */
    vita_write32(p, length, 16);
    assert(CALL(n, "sceNetRecvfrom", receiver, UNMAPPED, 8, 0, from, length) == SCE_NET_ERROR_EFAULT);
    assert(CALL(n, "sceNetRecvfrom", receiver, got, 8, 0, UNMAPPED, length) == SCE_NET_ERROR_EFAULT);
    assert(CALL(n, "sceNetRecvfrom", receiver, got, 8, 0, from, UNMAPPED) == SCE_NET_ERROR_EFAULT);
    assert(CALL(n, "sceNetRecvfrom", receiver, got, 8, 0, from, length) == 8);
    assert(!memcmp(vita_ptr(p, got), "datagram", 8) && vita_read32(p, length) == 16);

    /* the socket's name, its options and a peer, read or written outside the game's memory */
    assert(CALL(n, "sceNetGetsockname", receiver, UNMAPPED, length) == SCE_NET_ERROR_EFAULT);
    assert(CALL(n, "sceNetGetsockname", receiver, from, UNMAPPED) == SCE_NET_ERROR_EFAULT);
    assert(CALL(n, "sceNetSetsockopt", receiver, 0xffff, 0x1100, UNMAPPED, 4) == SCE_NET_ERROR_EFAULT);
    assert(CALL(n, "sceNetGetsockopt", receiver, 0xffff, 0x1008, UNMAPPED, length) == SCE_NET_ERROR_EFAULT);
    assert(CALL(n, "sceNetGetsockopt", receiver, 0xffff, 0x1008, value, UNMAPPED) == SCE_NET_ERROR_EFAULT);
    assert(CALL(n, "sceNetGetsockopt", receiver, 0xffff, 0x1008, value, length) == 0 && vita_read32(p, value) == 2);
    assert(CALL(n, "sceNetConnect", sender, UNMAPPED, 16) == SCE_NET_ERROR_EFAULT);

    /* an epoll's event and the events it returns; the sender is always ready to send (EPOLLOUT) */
    uint32_t e = CALL(n, "sceNetEpollCreate", 0, 0);
    memset(vita_ptr(p, event), 0, 0x20);
    vita_write32(p, event, 2);
    assert(CALL(n, "sceNetEpollControl", e, 1, sender, UNMAPPED) == SCE_NET_ERROR_EFAULT);
    assert(CALL(n, "sceNetEpollControl", e, 1, sender, event) == 0);
    assert(CALL(n, "sceNetEpollWait", e, UNMAPPED, 1, 0) == SCE_NET_ERROR_EFAULT);
    assert(CALL(n, "sceNetEpollWait", e, events, 1, 0) == 1 && vita_read32(p, events) == 2);
    /* two ready, and as many events asked as wrap their size around, with room for one: EFAULT */
    assert(CALL(n, "sceNetEpollControl", e, 1, receiver, event) == 0);
    assert(CALL(n, "sceNetEpollWait", e, end - 0x20, 0x08000000u, 0) == SCE_NET_ERROR_EFAULT);

    /* sceNetCtl's and NP's out parameters, and the NP IDs compared */
    assert(CALL(n, "sceNetCtlInetGetState", UNMAPPED) == SCE_NET_CTL_ERROR_INVALID_ADDR);
    assert(CALL(n, "sceNetCtlInetGetResult", 0, UNMAPPED) == SCE_NET_CTL_ERROR_INVALID_ADDR);
    assert(CALL(n, "sceNetCtlInetRegisterCallback", mem, 0, UNMAPPED) == SCE_NET_CTL_ERROR_INVALID_ADDR);
    assert(CALL(n, "sceNpGetServiceState", UNMAPPED) == SCE_NP_ERROR_INVALID_ARGUMENT);
    assert(CALL(n, "sceNpManagerGetNpId", UNMAPPED) == SCE_NP_MANAGER_ERROR_INVALID_ARGUMENT);
    assert(CALL(n, "sceNpManagerGetNpId", id) == 0);
    assert(CALL(n, "sceNpCmpNpId", UNMAPPED, id) == SCE_NP_UTIL_ERROR_INVALID_ARGUMENT);
    assert(CALL(n, "sceNpCmpNpId", id, UNMAPPED) == SCE_NP_UTIL_ERROR_INVALID_ARGUMENT);
    assert(CALL(n, "sceNpCmpNpId", id, id) == 0);
}

static void ngs_template1(VitaProcess* p, VitaThread* t, uint32_t mem) {
    const VitaHle* n = vita_hle_ngs;
    uint32_t system = mem + 0x2000, params = mem + 0x2100, description = mem + 0x2200;
    uint32_t required = mem + 0x2300, buffer_info = mem + 0x2400, rack_out = mem + 0x2500;
    uint32_t patch_info = mem + 0x2600, patch_out = mem + 0x2700, voice_info = mem + 0x2800;
    uint32_t matrix = mem + 0x2900, delivery = mem + 0x2a00;
    uint32_t rack_memory = mem + 0x3000, rack_size = 0x40 + 2 * 0x440;
    uint32_t voice0 = rack_memory + 0x40, voice1 = voice0 + 0x440;
    uint32_t template = CALL(n, "sceNgsVoiceDefGetTemplate1");

    vita_write32(p, params + 8, 256);
    vita_write32(p, params + 12, 48000);
    assert(CALL(n, "sceNgsSystemInit", system, 0x1000, params, mem + 0x2204) == 0);
    assert(vita_read32(p, mem + 0x2204) == system);
    vita_write32(p, description, template);
    vita_write32(p, description + 4, 2);
    vita_write32(p, description + 8, 2);
    assert(CALL(n, "sceNgsRackGetRequiredMemorySize", system, description, required) == 0);
    assert(vita_read32(p, required) == rack_size);
    vita_write32(p, buffer_info, rack_memory);
    vita_write32(p, buffer_info + 4, rack_size);
    assert(CALL(n, "sceNgsRackInit", system, buffer_info, description, rack_out) == 0);
    assert(vita_read32(p, rack_out) == rack_memory);
    assert(CALL(n, "sceNgsRackGetVoiceHandle", rack_memory, 0, patch_out) == 0);
    assert(vita_read32(p, patch_out) == voice0);
    assert(CALL(n, "sceNgsRackGetVoiceHandle", rack_memory, 1, patch_out) == 0);
    assert(vita_read32(p, patch_out) == voice1);

    assert(CALL(n, "sceNgsVoiceLockParams", voice0, 9, 0, buffer_info) == 0);
    assert(vita_read32(p, buffer_info) == voice0 + 0x40 + 9 * 0x60);
    assert(vita_read32(p, buffer_info + 4) == 0x60);
    assert(CALL(n, "sceNgsVoiceGetInfo", voice0, voice_info) == 0);
    assert(vita_read32(p, voice_info + 4) == 10);
    assert(vita_read32(p, voice_info + 12) == 4);

    vita_write32(p, patch_info, voice0);
    vita_write32(p, patch_info + 4, 3);
    vita_write32(p, patch_info + 8, UINT32_MAX);
    vita_write32(p, patch_info + 12, voice1);
    vita_write32(p, patch_info + 16, 1);
    assert(CALL(n, "sceNgsPatchCreateRouting", patch_info, patch_out) == 0);
    assert(vita_read32(p, patch_out) == voice0 + 0x400 + 3 * 4 * 4);
    vita_write32(p, matrix, 0x3f800000u);
    vita_write32(p, matrix + 4, 0);
    vita_write32(p, matrix + 8, 0);
    vita_write32(p, matrix + 12, 0x3f800000u);
    assert(CALL(n, "sceNgsVoicePatchSetVolumesMatrix", vita_read32(p, patch_out), matrix) == 0);
    assert(CALL(n, "sceNgsPatchGetInfo", vita_read32(p, patch_out), voice_info, delivery) == 0);
    assert(vita_read32(p, voice_info) == 2 && vita_read32(p, voice_info + 4) == 2);
    assert(vita_read32(p, voice_info + 8) == 0x3f800000u && vita_read32(p, voice_info + 12) == 0);
    assert(vita_read32(p, voice_info + 16) == 0 && vita_read32(p, voice_info + 20) == 0x3f800000u);
    assert(vita_read32(p, delivery) == voice0 && vita_read32(p, delivery + 4) == 3);
    assert(vita_read32(p, delivery + 8) == 0 && vita_read32(p, delivery + 12) == voice1);
    assert(vita_read32(p, delivery + 16) == 1);
    assert(CALL(n, "sceNgsPatchRemoveRouting", vita_read32(p, patch_out)) == 0);

    uint32_t player_params = voice0 + 0x40, samples = mem + 0x2b00, user_data = mem + 0x2b20;
    assert(CALL(n, "sceNgsVoiceSetModuleCallback", voice0, 10, VITA_RUNTIME_PAGE + 0x20, user_data) ==
           0x804a0002u);
    assert(CALL(n, "sceNgsVoiceSetModuleCallback", voice0, 0, VITA_RUNTIME_PAGE + 0x20, user_data) == 0);
    vita_write32(p, player_params, 0x01015ce6u);
    vita_write32(p, player_params + 8, samples);
    vita_write32(p, player_params + 12, 4);
    vita_write32(p, player_params + 16, 0xffff0000u);
    vita_write32(p, player_params + 0x38, 0x473b8000u);
    vita_write32(p, player_params + 0x3c, 0x3f800000u);
    vita_write32(p, player_params + 0x4c, 2);
    vita_write32(p, samples, 0x40004000u);
    assert(CALL(n, "sceNgsVoicePlay", voice0) == 0);
    assert(CALL(n, "sceNgsSystemUpdate", system) == 0 && t->call.redirected);
    uint32_t callback_info = t->cpu.r[0];
    assert(vita_read32(p, callback_info) == voice0);
    assert(vita_read32(p, callback_info + 4) == rack_memory);
    assert(vita_read32(p, callback_info + 8) == 0x5ce6u);
    assert(vita_read32(p, callback_info + 12) == 0);
    assert(vita_read32(p, callback_info + 16) == 0 && vita_read32(p, callback_info + 20) == 0);
    assert(vita_read32(p, callback_info + 24) == user_data);
    vita_hle_call_returned(p, t);
    assert(!t->call.active);
    assert(CALL(n, "sceNgsVoiceGetInfo", voice0, voice_info) == 0);
    assert(vita_read32(p, voice_info) == 0);
    assert(CALL(n, "sceNgsSystemRelease", system) == 0);
}

static void ngs_sparse_input_clear(VitaProcess* p, VitaThread* t, uint32_t mem) {
    const VitaHle* n = vita_hle_ngs;
    uint32_t system = mem + 0x2000, params = mem + 0x2100, description = mem + 0x2200;
    uint32_t buffer_info = mem + 0x2300, rack_out = mem + 0x2310, voice_out = mem + 0x2320;
    uint32_t patch_info = mem + 0x2400, patch_out = mem + 0x2420, matrix = mem + 0x2440;
    uint32_t source_rack = mem + 0x3000, master_rack = mem + 0x4000, rack_size = 0x40 + 0x400;
    uint32_t source_voice = source_rack + 0x40, master_voice = master_rack + 0x40;
    uint32_t player_params = source_voice + 0x40, samples = mem + 0x5000, output = mem + 0x6000;

    vita_write32(p, params + 8, 256);
    vita_write32(p, params + 12, 48000);
    assert(CALL(n, "sceNgsSystemInit", system, 0x1000, params, mem + 0x2210) == 0);

    vita_write32(p, description, CALL(n, "sceNgsVoiceDefGetSimpleVoice"));
    vita_write32(p, description + 4, 1);
    vita_write32(p, description + 8, 2);
    vita_write32(p, buffer_info, source_rack);
    vita_write32(p, buffer_info + 4, rack_size);
    assert(CALL(n, "sceNgsRackInit", system, buffer_info, description, rack_out) == 0);
    assert(CALL(n, "sceNgsRackGetVoiceHandle", source_rack, 0, voice_out) == 0);
    assert(vita_read32(p, voice_out) == source_voice);

    vita_write32(p, description, CALL(n, "sceNgsVoiceDefGetMasterBuss"));
    vita_write32(p, buffer_info, master_rack);
    assert(CALL(n, "sceNgsRackInit", system, buffer_info, description, rack_out) == 0);
    assert(CALL(n, "sceNgsRackGetVoiceHandle", master_rack, 0, voice_out) == 0);
    assert(vita_read32(p, voice_out) == master_voice);

    vita_write32(p, patch_info, source_voice);
    vita_write32(p, patch_info + 4, 0);
    vita_write32(p, patch_info + 8, UINT32_MAX);
    vita_write32(p, patch_info + 12, master_voice);
    vita_write32(p, patch_info + 16, UINT32_MAX);
    assert(CALL(n, "sceNgsPatchCreateRouting", patch_info, patch_out) == 0);
    vita_write32(p, matrix, 0x3f800000u);
    vita_write32(p, matrix + 4, 0);
    vita_write32(p, matrix + 8, 0);
    vita_write32(p, matrix + 12, 0x3f800000u);
    assert(CALL(n, "sceNgsVoicePatchSetVolumesMatrix", vita_read32(p, patch_out), matrix) == 0);

    assert(CALL(n, "sceNgsVoiceLockParams", source_voice, 0, 0, buffer_info) == 0);
    player_params = vita_read32(p, buffer_info);
    vita_write32(p, player_params, 0x01015ce6u);
    vita_write32(p, player_params + 8, samples);
    vita_write32(p, player_params + 12, 256 * 4);
    vita_write32(p, player_params + 16, 1);
    vita_write32(p, player_params + 0x38, 0x473b8000u);
    vita_write32(p, player_params + 0x3c, 0x3f800000u);
    vita_write32(p, player_params + 0x4c, 2);
    for (unsigned i = 0; i < 256; ++i) {
        vita_write16(p, samples + i * 4, 8192);
        vita_write16(p, samples + i * 4 + 2, 8192);
    }
    assert(CALL(n, "sceNgsVoiceSetModuleCallback", source_voice, 0, VITA_RUNTIME_PAGE + 0x20, 0) == 0);
    assert(CALL(n, "sceNgsVoicePlay", source_voice) == 0);
    assert(CALL(n, "sceNgsVoicePlay", master_voice) == 0);
    assert(CALL(n, "sceNgsSystemUpdate", system) == 0 && !t->call.redirected);
    assert(CALL(n, "sceNgsVoiceGetStateData", master_voice, 1, output, 256 * 4) == 0);
    assert(vita_read16(p, output + 10 * 4) == 8192);

    assert(CALL(n, "sceNgsVoiceKill", master_voice) == 0);
    assert(CALL(n, "sceNgsSystemUpdate", system) == 0 && t->call.redirected);
    assert(CALL(n, "sceNgsVoicePlay", master_voice) == 0);
    vita_hle_call_returned(p, t);
    assert(!t->call.active);
    assert(CALL(n, "sceNgsVoiceGetStateData", master_voice, 1, output, 256 * 4) == 0);
    assert(vita_read16(p, output + 10 * 4) == 8192);
    assert(CALL(n, "sceNgsSystemRelease", system) == 0);
}

static void gxm_program_metadata(VitaProcess* p, VitaThread* t, uint32_t mem) {
    const VitaHle* gxm = vita_hle_gxm;
    uint32_t parameter = mem + 0x8000, patcher_out = mem + 0x8020, program = mem + 0x8040,
             fragment_out = mem + 0x8060;
    unsigned char* bytes = vita_ptr(p, parameter);
    memset(bytes, 0, 16);
    bytes[5] = 3;
    vita_write32(p, parameter + 8, 7);
    assert(CALL(gxm, "sceGxmProgramParameterGetComponentCount", parameter) == 3);
    assert(CALL(gxm, "sceGxmProgramParameterGetArraySize", parameter) == 7);

    assert(CALL(gxm, "sceGxmShaderPatcherCreate", mem, patcher_out) == 0);
    uint32_t patcher = vita_read32(p, patcher_out);
    assert(CALL(gxm, "sceGxmShaderPatcherCreateFragmentProgram", patcher, program, 0, 0, 0, 0, fragment_out) == 0);
    uint32_t fragment = vita_read32(p, fragment_out);
    assert(CALL(gxm, "sceGxmShaderPatcherAddRefFragmentProgram", patcher, fragment) == 0);
    uint32_t slot = (fragment - GXM_HANDLE_FRAGMENT) / GXM_PROGRAM_HANDLE;
    assert(slot < GXM_PROGRAMS && p->gxm->fragment[slot].references == 2);
}
static void gxm_texture_transfers(VitaProcess* p, VitaThread* t, uint32_t mem) {
    const VitaHle* gxm = vita_hle_gxm;
    uint32_t texture = mem + 0x8080;
    assert(CALL(gxm, "sceGxmTextureInitCube", texture, mem + 0x9000, 0, 64, 32, 1) == 0);
    GxmTexture cube = {{vita_read32(p, texture), vita_read32(p, texture + 4), vita_read32(p, texture + 8),
                        vita_read32(p, texture + 12)}};
    assert(gxm_texture_type(&cube) == GXM_TEXTURE_CUBE);
    assert(CALL(gxm, "sceGxmTextureInitCube", texture, mem + 0x9000, 0, 63, 32, 1) == 0x805b0005u);
    assert(CALL(gxm, "sceGxmTextureInitCube", texture, mem + 0x9000, 0, 64, 0, 1) == 0x805b0005u);
    assert(CALL(gxm, "sceGxmTextureInitCube", 0, mem + 0x9000, 0, 64, 32, 1) == 0x805b0004u);

    /* Unsupported transfer modes fail explicitly; finish remains safe without a renderer. */
    assert(CALL(gxm, "sceGxmTransferCopy", 1, 1, 0, 0, 1, 0, 0, mem, 0, 0, 4, 0, 0, mem + 4, 0, 0, 4, 0, 0, 0) ==
           0x805b0009u);
    assert(CALL(gxm, "sceGxmTransferCopy", 0, 1, 0, 0, 0, 0, 0, mem, 0, 0, 4, 0, 0, mem + 4, 0, 0, 4, 0, 0, 0) ==
           0);
    assert(CALL(gxm, "sceGxmTransferFinish") == 0);
}

int main(void) {
    char *root = snx_temp_dir(NULL), *folder = root ? snx_path_join(root, "game") : NULL, error[256] = {0};
    assert(folder && !snx_mkdirs(folder));
    VitaImage image = {0};
    VitaProcess* p = vita_process_new(&image, folder, error, sizeof error);
    if (!p) fprintf(stderr, "vita_process_new: %s\n", error);
    assert(p);
    VitaThread* t = &p->threads[0];
    assert(t->state == VITA_THREAD_READY);
    p->current = t;
    t->cpu.r[13] -= 0x100; /* room for the stack's arguments */
    /* the game's memory: one block, the last made, so the page past its end is no one's */
    uint32_t mem = vita_alloc(p, &p->user, 0x10000, 0x1000), end = mem + 0x10000;
    assert(mem && vita_mapped(p, mem, 0x10000) && !vita_mapped(p, end - 8, 16) && !vita_mapped(p, UNMAPPED, 1));
    save_data(p, t, mem, end);
    net(p, t, mem, end);
    ngs_template1(p, t, mem);
    ngs_sparse_input_clear(p, t, mem);
    gxm_program_metadata(p, t, mem);
    gxm_texture_transfers(p, t, mem);
    vita_process_free(p);
    assert(!snx_remove_tree(root));
    free(folder);
    free(root);
    puts("vita_hle_memory: ok");
    return 0;
}
