/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   APT GetLockHandle facts: Azahar service/apt/{apt,applet_manager}.cpp,
   GPL-2.0-or-later, revision recorded in docs/3DS.md. */
#include "3ds/runtime/internal.h"
#include <stdio.h>
#include <string.h>
static bool preloaded(const SnxCtrProcess* p, uint32_t id);
#include "common/utilities/endian.h"
#include "3ds/services/apt.h"
#include "3ds/services/font.h"
#include <stdlib.h>
#include "services/apt.swipc.inc"
#define APT_NO_SHARED_FONT 0xffffffffu /* Azahar's answer (-1) when it has no font to serve */
static uint32_t snx_ctr_apt_wire_GetLockHandle(SnxCtrIpc* c, uint32_t attributes, uint32_t* corrected, uint32_t* state,
                                               uint32_t* lock) {
    SnxCtrProcess* p = c->process;
    /* Only application callers: no library-position correction or Home Menu. */
    if (attributes) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return 0;
    }
    uint32_t result;
    if (p->apt_lock)
        result = snx_ctr_handle_add(p, p->apt_lock, lock);
    else {
        result = snx_ctr_object_create(p, CTR_OBJ_MUTEX, lock);
        if (!result) {
            p->apt_lock = snx_ctr_handle_object(p, *lock);
            ++p->objects[p->apt_lock].refs; /* manager owns the shared lock */
        }
    }
    if (result) return result;
    *corrected = attributes;
    *state = 0; /* no outstanding library-applet transition */
    return 0;
}
static uint32_t snx_ctr_apt_wire_Initialize(SnxCtrIpc* c, uint32_t app_id, uint32_t attributes, uint32_t* notification,
                                            uint32_t* parameter) {
    SnxCtrProcess* p = c->process;
    if (app_id != 0x300 || attributes) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return 0;
    }
    if (p->apt_phase) return 0xc8a0cffcu; /* occupied application slot */
    uint32_t result = snx_ctr_object_create(p, CTR_OBJ_EVENT, notification);
    if (result) return result;
    result = snx_ctr_object_create(p, CTR_OBJ_EVENT, parameter);
    if (result) return result; /* generated error path closes notification */
    p->apt_notification = snx_ctr_handle_object(p, *notification);
    p->apt_parameter = snx_ctr_handle_object(p, *parameter);
    ++p->objects[p->apt_notification].refs;
    ++p->objects[p->apt_parameter].refs;
    p->apt_phase = 1; /* first application auto-enabled */
    /* APT initializes/enables the first application itself when no applet is
       active, and queues its initial Wakeup. No Home Menu is invented. */
    p->objects[p->apt_parameter].data.event.signaled = 1;
    return 0;
}
static uint32_t snx_ctr_apt_wire_Enable(SnxCtrIpc* c, uint32_t attributes) {
    if (attributes) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return 0;
    }
    /* Initialize auto-enables the first/only application. Re-enabling that
       existing registration is idempotent; no absent applet is registered. */
    return c->process->apt_phase ? 0 : SNX_CTR_IPC_INVALID_STATE;
}
static uint32_t snx_ctr_apt_wire_NotifyToWait(SnxCtrIpc* c, uint32_t app_id) {
    SnxCtrProcess* p = c->process;
    if (app_id != 0x300) { /* a game waiting for a library applet it started, which has already answered */
        if (!preloaded(p, app_id)) c->state = SNX_CTR_IPC_UNSUPPORTED;
        return 0;
    }
    if (!p->apt_phase) return SNX_CTR_IPC_INVALID_STATE;
    if (p->apt_phase > 2) return 0; /* the game waits for its wakeup again, after a library applet it started */
    /* Record the registered application's startup readiness. This is not a
       kernel wait completion; the queued parameter must still be received. */
    p->apt_phase = 2;
    return 0;
}
/* GetTargetPlatform (0x101): 1 for an Old 3DS, 2 for a New 3DS */
static uint32_t snx_ctr_apt_wire_GetTargetPlatform(SnxCtrIpc* c, uint32_t* platform) {
    *platform = snx_ctr_memory_profile(c->process->memory)->model == SNX_CTR_NEW ? 2 : 1;
    return 0;
}
static uint32_t snx_ctr_apt_wire_GetSharedFont(SnxCtrIpc* c, uint32_t* address, uint32_t* font) {
    return snx_ctr_font_shared(c->process, address, font) ? 0 : APT_NO_SHARED_FONT;
}
static uint32_t snx_ctr_apt_wire_IsNew3DS(SnxCtrIpc* c, uint32_t* is_new) {
    *is_new = snx_ctr_memory_profile(c->process->memory)->model == SNX_CTR_NEW;
    return 0;
}
/* ReceiveParameter (0xd) takes the queued parameter; GlanceParameter (0xe) answers the same and leaves it queued */
static void receive_parameter(SnxCtrIpc* c, bool consume) {
    if (!snx_ctr_ipc_expect(c, 2, 0)) return;
    SnxCtrProcess* p = c->process;
    if (c->in[1] != 0x300 || c->in[2] > 4096) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    bool sent = p->apt_message.signal != 0; /* an applet's parameter, not the startup's Wakeup */
    if (!sent && (consume ? p->apt_phase != 2 : p->apt_phase < 1 || p->apt_phase > 2)) {
        snx_ctr_ipc_error_reply(c, p->apt_phase >= 3 ? 0xc8a0cfefu : SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    unsigned char descriptor[8], zero[4096] = {0};
    if (sent) memcpy(zero, p->apt_message.data, p->apt_message.size < c->in[2] ? p->apt_message.size : c->in[2]);
    uint32_t tls = p->cpu.state.cpu.tpidruro;
    if (snx_ctr_read_ram(p->space, tls + 0x180, descriptor, sizeof descriptor) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    uint32_t d = snx_ld_le32(descriptor), address = snx_ld_le32(descriptor + 4), size = c->in[2];
    if ((d & 0x3fff) != 2 || (d >> 14) < size) {
        snx_ctr_ipc_bad(c);
        return;
    }
    /* APT pads its parameter buffer to the requested length. This initial
       Wakeup has no payload or object. Whole-range checked RAM write is atomic
       on a bad address/permission/MMIO; consume only after it succeeds. */
    if (size && snx_ctr_write_ram(p->space, address, zero, size) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    c->out[0] = consume ? 0x000d0104 : 0x000e0104;
    c->out[2] = sent ? p->apt_message.sender : 0; /* none: no Home Menu process is claimed */
    c->out[3] = sent ? p->apt_message.signal : 1; /* else the queued initial Wakeup, implicit in the startup phase */
    c->out[4] = sent ? p->apt_message.size : 0;   /* actual payload size */
    c->out[5] = 0x10;                             /* move descriptor; absent optional object is wire NULL */
    c->out[6] = sent ? p->apt_message.object : 0;
    c->out[7] = (size << 14) | 2; /* static buffer 0 */
    c->out[8] = address;
    if (sent && consume)
        p->apt_message.signal = 0;
    else if (consume)
        p->apt_phase = 3; /* wakeup received, startup transition not yet released */
}
/* AppletUtility: a utility command, its input in static buffer 1, its output (output_size bytes) in the caller's
   receive buffer 0. As Azahar answers them: success, and an output of zeros, but for TryLockTransition (6), which
   says the transitions are locked, there being none that waits. The first UnlockTransition (7) of the startup's
   (0x10) ends the startup transition. SleepIfShellClosed (4) does not sleep: the shell stays open */
static void applet_utility(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 3, 2)) return;
    SnxCtrProcess* p = c->process;
    uint32_t command = c->in[1], input_size = c->in[2], output_size = c->in[3];
    if (input_size > 0x1000 || output_size > 0x1000) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    if (c->in[4] != ((input_size << 14) | 0x402)) {
        snx_ctr_ipc_bad(c);
        return;
    }
    unsigned char input[4] = {0}, descriptor[8];
    if (snx_ctr_read_ram(p->space, c->in[5], input, input_size < 4 ? input_size : 4) != SNX_CTR_MEM_OK ||
        snx_ctr_read_ram(p->space, p->cpu.state.cpu.tpidruro + 0x180, descriptor, 8) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    uint32_t d = snx_ld_le32(descriptor), address = snx_ld_le32(descriptor + 4);
    if ((d & 0x3fff) != 2 || (d >> 14) < output_size) {
        snx_ctr_ipc_bad(c);
        return;
    }
    if (command == 4 && (input[0] || p->shell_closed)) { /* sleep is not modelled */
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    unsigned char out[0x1000] = {0};
    if (command == 6 && output_size) out[0] = 1;
    if (output_size && snx_ctr_write_ram(p->space, address, out, output_size) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    if (command == 7 && input_size == 4 && snx_ld_le32(input) == 0x10 && p->apt_phase == 3) p->apt_phase = 4;
    c->out[0] = 0x004b0082;
    c->out[2] = 0; /* the utility's own result */
    c->out[3] = (output_size << 14) | 2;
    c->out[4] = address;
}
static bool preloaded(const SnxCtrProcess* p, uint32_t id) {
    for (unsigned i = 0; i < sizeof p->apt_preloaded / sizeof *p->apt_preloaded; ++i)
        if (p->apt_preloaded[i] == id) return true;
    return false;
}
/* PreloadLibraryApplet (0x16) and PrepareToStartLibraryApplet (0x18): the applet is ready as soon as it is asked for,
   as Azahar's HLE applets are, and so registered. The first ask for each is reported: no applet runs */
static void ask_library_applet(SnxCtrIpc* c, uint32_t header) {
    if (!snx_ctr_ipc_expect(c, 1, 0)) return;
    SnxCtrProcess* p = c->process;
    uint32_t id = c->in[1];
    for (unsigned i = 0; id && !preloaded(p, id) && i < sizeof p->apt_preloaded / sizeof *p->apt_preloaded; ++i)
        if (!p->apt_preloaded[i]) {
            p->apt_preloaded[i] = id;
            fprintf(stderr, "3ds apt: library applet 0x%x asked for (command 0x%04x); it is only reported ready\n", id,
                    header >> 16);
        }
    c->out[0] = header;
    c->out[1] = 0;
}
/* IsRegistered (0x9): the application itself and each library applet asked for */
static void is_registered(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 1, 0)) return;
    c->out[0] = 0x00090080;
    c->out[1] = 0;
    c->out[2] = c->in[1] == 0x300 || preloaded(c->process, c->in[1]);
}
/* GetAppletInfo (0x6): a preloaded library applet is registered and loaded (a game polls for that after preloading
   it); any other applet answers success with nothing set, as the stub did */
static void get_applet_info(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 1, 0)) return;
    if (!preloaded(c->process, c->in[1])) {
        c->out[0] = 0x00060040;
        c->out[1] = 0;
        return;
    }
    c->out[0] = 0x000601c0;
    c->out[1] = 0;
    c->out[2] = c->out[3] = 0; /* the title: not modelled */
    c->out[4] = 0;             /* the media type */
    c->out[5] = 1;             /* registered */
    c->out[6] = 1;             /* loaded */
    c->out[7] = 0;             /* the attributes */
}
/* The applet's parameter to the game: the next ReceiveParameter answers it, and the parameter event wakes the game's
   thread that waits for one */
static void send_to_game(SnxCtrProcess* p, uint32_t sender, uint32_t signal, const void* data, uint32_t size,
                         uint32_t object) {
    p->apt_message.signal = signal;
    p->apt_message.sender = sender;
    p->apt_message.size = size;
    p->apt_message.object = object;
    memset(p->apt_message.data, 0, sizeof p->apt_message.data);
    if (size) memcpy(p->apt_message.data, data, size);
    if (p->apt_parameter) {
        p->objects[p->apt_parameter].data.event.signaled = 1;
        snx_ctr_wake_waiters(p, p->apt_parameter);
    }
}
/* SendParameter (0xc): the game to an applet. A Request for the capture block, which a library applet answers with
   its screen: a shared block of the size the request asks, blank (Azahar's applet.cpp ReceiveParameterImpl) */
static void send_parameter(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 4, 4)) return;
    SnxCtrProcess* p = c->process;
    uint32_t destination = c->in[2], signal = c->in[3], size = c->in[4];
    c->out[0] = 0x000c0040;
    c->out[1] = 0;
    if (!preloaded(p, destination) || signal != 2) return;
    unsigned char head[4] = {0};
    uint32_t wanted = size >= 4 && snx_ctr_read_ram(p->space, c->in[8], head, 4) == SNX_CTR_MEM_OK ? snx_ld_le32(head) : 0;
    if (!wanted || wanted > 0x400000) wanted = 0x100000;
    uint32_t handle, physical = snx_ctr_shared_pages(p, (wanted + 0xfffu) & ~0xfffu);
    if (!physical || snx_ctr_object_create(p, CTR_OBJ_SHARED, &handle)) {
        c->out[1] = 0xd86007f3u;
        return;
    }
    CtrObject* shared = &p->objects[snx_ctr_handle_object(p, handle)];
    shared->data.shared.physical = physical;
    shared->data.shared.size = (wanted + 0xfffu) & ~0xfffu;
    shared->data.shared.permissions = SNX_CTR_READ | SNX_CTR_WRITE;
    send_to_game(p, destination, 3, NULL, 0, handle); /* Response */
}
/* CRC-16/XMODEM (CCITT polynomial 0x1021, from 0, not reflected), which libctru's miiSelectorChecksumIsValid takes */
static uint16_t crc16(const unsigned char* bytes, uint32_t size) {
    uint16_t crc = 0;
    for (uint32_t i = 0; i < size; ++i) {
        crc ^= (uint16_t)(bytes[i] << 8);
        for (unsigned bit = 0; bit < 8; ++bit) crc = (uint16_t)(crc & 0x8000 ? crc << 1 ^ 0x1021 : crc << 1);
    }
    return crc;
}
/* The Mii the selector answers with, as the console's own picker gave it (Azahar's mii_selector.cpp
   GetStandardMiiResult, GPL-2.0-or-later, from system version 11.8.0): a 0x5c-byte Mii as the system database holds it */
static void standard_mii(unsigned char mii[0x5c]) {
    static const unsigned char head[0x1a] = {0x03, 0x00, 0x10, 0x30, 0x0a, 0x85, 0xc8, 0x00, 0xb3, 0xb6, 0x85, 0xd2, 0xe4,
                                             0x1e, 0x39, 0x98, 0x40, 0xf4, 0x07, 0xb7, 0x37, 0x10, 0x00, 0x00, 0x00, 0xa6};
    static const unsigned char look[0x1e] = {0x40, 0x40, 0x00, 0x00, 0x21, 0x01, 0x18, 0x44, 0x68, 0x02,
                                             0x14, 0x46, 0x34, 0x26, 0x12, 0x81, 0x68, 0x17, 0x00, 0x0d,
                                             0x29, 0x00, 0x52, 0x00, 0x50, 0x48};
    memset(mii, 0, 0x5c);
    memcpy(mii, head, sizeof head);
    static const unsigned char name[] = {'M', 0, 'i', 0, 'i', 0}; /* UTF-16, ten characters of room */
    memcpy(mii + 0x1a, name, sizeof name);
    memcpy(mii + 0x2e, look, 26);
}
/* StartLibraryApplet (0x1e): runs the applet and ends it at once, with its result for the game as a WakeupByExit. Only
   the Mii selector has one, and it answers with that standard Mii (MiiSelectorReturn: a Mii chosen, no guest, the
   checksum of the Mii and the two bytes after it, big-endian) */
static void start_library_applet(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 2, 4)) return;
    SnxCtrProcess* p = c->process;
    uint32_t id = c->in[1];
    c->out[0] = 0x001e0040;
    c->out[1] = 0;
    if (id != 0x402) {
        fprintf(stderr, "3ds apt: library applet 0x%x is not emulated; the game waits for it\n", id);
        return;
    }
    unsigned char result[0x84] = {0};
    snx_st_le32(result + 8, 0xffffffffu);
    standard_mii(result + 0x0c);
    uint16_t checksum = crc16(result + 0x0c, 0x5e);
    result[0x6a] = (unsigned char)(checksum >> 8);
    result[0x6b] = (unsigned char)checksum;
    send_to_game(p, id, 10, result, sizeof result, 0);
}
void snx_ctr_apt_dispatch(SnxCtrIpc* c) {
    if (c->in[0] >> 16 == 0xd || c->in[0] >> 16 == 0xe)
        receive_parameter(c, c->in[0] >> 16 == 0xd);
    else if (c->in[0] >> 16 == 0x4b)
        applet_utility(c);
    else if (c->in[0] >> 16 == 0x16 || c->in[0] >> 16 == 0x18)
        ask_library_applet(c, c->in[0] >> 16 == 0x16 ? 0x00160040 : 0x00180040);
    else if (c->in[0] >> 16 == 0x9)
        is_registered(c);
    else if (c->in[0] >> 16 == 0xc)
        send_parameter(c);
    else if (c->in[0] >> 16 == 0x1e)
        start_library_applet(c);
    else if (c->in[0] >> 16 == 0x6)
        get_applet_info(c);
    else
        snx_ctr_apt_wire_dispatch(c);
}
