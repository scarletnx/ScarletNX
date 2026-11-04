/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   APT GetLockHandle facts: Azahar service/apt/{apt,applet_manager}.cpp,
   GPL-2.0-or-later, revision recorded in docs/3DS.md. The parameter queue and the library applet's start, as an
   application drives them for the software keyboard and the Mii selector, and the capture block each answers the
   application's Request with (each HLE applet's ReceiveParameterImpl): the same files at ac4a97e. */
#include "3ds/runtime/internal.h"
#include "common/utilities/endian.h"
#include "3ds/memory/ctr_memory.h"
#include "3ds/services/apt.h"
#include "3ds/services/font.h"
#include "3ds/services/mii_selector.h"
#include "3ds/services/swkbd.h"
#include "services/apt.swipc.inc"
#include <stdio.h>
#include <string.h>
#define APT_NO_SHARED_FONT     0xffffffffu /* Azahar's answer (-1) when it has no font to serve */
#define APT_PARAMETER_PRESENT  0xc8a0cc02u /* ParameterPresent, Applet, InvalidState, Status */
#define APT_ALREADY_EXISTS     0xc8a0cffcu /* AlreadyExists, Applet, InvalidState, Status */
#define APT_NO_DATA            0xc8a0cfefu /* NoData, Applet, InvalidState, Status */
#define APT_ANY_LIBRARY        0x400u      /* AnyLibraryApplet, and AnySysLibraryApplet (0x200) */
#define APT_PARAMETER_CAPACITY 0x1000u     /* the most a parameter's buffer holds, as Azahar's receive takes it */
#define APT_CAPTURE_LIMIT      0x200000u   /* the most a Request's capture block may take: both screens, both eyes */
#define APT_CAPTURE_INFO_SIZE  0x20u       /* CaptureBufferInfo, a Request's buffer */
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
    if (p->apt_phase) return APT_ALREADY_EXISTS; /* occupied application slot */
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
    if (app_id != 0x300) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return 0;
    }
    if (!p->apt_phase) return SNX_CTR_IPC_INVALID_STATE;
    /* Record the registered application's startup readiness. This is not a
       kernel wait completion; the queued parameter must still be received.
       Later ones come before each wakeup the application waits for (a library
       applet's), and Azahar answers them with success alone */
    if (p->apt_phase < 2) p->apt_phase = 2;
    return 0;
}
/* the application from its Initialize on, and the library applet APT registered for it; no other applet runs */
static uint32_t snx_ctr_apt_wire_IsRegistered(SnxCtrIpc* c, uint32_t app_id, uint32_t* registered) {
    SnxCtrProcess* p = c->process;
    uint32_t library = p->apt_library;
    if (app_id == 0x300)
        *registered = p->apt_phase != 0;
    else if (app_id == APT_ANY_LIBRARY || app_id == APT_ANY_LIBRARY >> 1)
        *registered = library && (library & 0xf00) == app_id;
    else
        *registered = library && app_id == library;
    return 0;
}
static void parameter_drop(SnxCtrProcess* p) {
    if (p->apt_next.pending && p->apt_next.object) snx_ctr_object_release(p, p->apt_next.object);
    p->apt_next.pending = 0;
    p->apt_next.object = 0;
}
/* CancelParameter: the parameter waiting goes when it is from sender and for receiver, each as asked to check */
static uint32_t snx_ctr_apt_wire_CancelParameter(SnxCtrIpc* c, uint32_t check_sender, uint32_t sender,
                                                 uint32_t check_receiver, uint32_t receiver, uint32_t* cancelled) {
    SnxCtrProcess* p = c->process;
    *cancelled = p->apt_next.pending && (!(check_sender & 0xff) || p->apt_next.sender == sender) &&
                 (!(check_receiver & 0xff) || receiver == 0x300);
    if (*cancelled) parameter_drop(p);
    return 0;
}
/* PrepareToStartLibraryApplet: the software keyboard or the Mii selector, which the runtime plays, registered for the
   application. Any other library applet is one the runtime does not have: the command answers as an unknown one
   does */
static uint32_t snx_ctr_apt_wire_PrepareToStartLibraryApplet(SnxCtrIpc* c, uint32_t app_id) {
    SnxCtrProcess* p = c->process;
    if (!snx_ctr_swkbd_is(app_id) && !snx_ctr_mii_selector_is(app_id)) {
        fprintf(stderr,
                "3ds apt: the game prepares library applet 0x%03x, which the runtime does not have (only the "
                "software keyboard, 0x401, and the Mii selector, 0x402)\n",
                (unsigned)app_id);
        c->state = SNX_CTR_IPC_UNKNOWN;
        return 0;
    }
    if (!p->apt_phase) return SNX_CTR_IPC_INVALID_STATE; /* no application to start it for */
    if (p->apt_next.pending) return APT_PARAMETER_PRESENT;
    if (p->apt_library) return APT_ALREADY_EXISTS;
    p->apt_library = app_id;
    return 0;
}
void snx_ctr_apt_send(struct SnxCtrProcess* p, uint32_t sender, uint32_t signal, uint32_t object,
                      const unsigned char* buffer, uint32_t size) {
    parameter_drop(p);
    p->apt_next.pending = 1;
    p->apt_next.sender = sender;
    p->apt_next.signal = signal;
    p->apt_next.object = object;
    if (object) ++p->objects[object].refs;
    p->apt_next.size = size;
    if (size) memcpy(p->apt_next.buffer, buffer, size);
    p->objects[p->apt_parameter].data.event.signaled = 1;
    snx_ctr_wake_waiters(p, p->apt_parameter);
}
void snx_ctr_apt_library_closed(struct SnxCtrProcess* p) { p->apt_library = 0; }
static uint32_t snx_ctr_apt_wire_GetSharedFont(SnxCtrIpc* c, uint32_t* address, uint32_t* font) {
    return snx_ctr_font_shared(c->process, address, font) ? 0 : APT_NO_SHARED_FONT;
}
static uint32_t snx_ctr_apt_wire_IsNew3DS(SnxCtrIpc* c, uint32_t* is_new) {
    *is_new = snx_ctr_memory_profile(c->process->memory)->model == SNX_CTR_NEW;
    return 0;
}
/* ReceiveParameter and GlanceParameter (glance: the parameter stays): from Initialize on, the initial Wakeup that
   Initialize queued; after it, the parameter waiting for the application, its object moved to it */
static void receive_parameter(SnxCtrIpc* c, int glance) {
    if (!snx_ctr_ipc_expect(c, 2, 0)) return;
    SnxCtrProcess* p = c->process;
    if (c->in[1] != 0x300 || c->in[2] > APT_PARAMETER_CAPACITY) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    int startup = p->apt_phase == 1 || p->apt_phase == 2;
    if (!startup && (p->apt_phase < 3 || !p->apt_next.pending)) {
        snx_ctr_ipc_error_reply(c, p->apt_phase >= 3 ? APT_NO_DATA : SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    unsigned char descriptor[8], buffer[APT_PARAMETER_CAPACITY] = {0};
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
    /* the initial Wakeup has no payload or object */
    uint32_t sender = 0, signal = 1, actual = 0, object = startup ? 0 : p->apt_next.object, handle = 0;
    if (!startup) {
        sender = p->apt_next.sender;
        signal = p->apt_next.signal;
        actual = p->apt_next.size < size ? p->apt_next.size : size;
        memcpy(buffer, p->apt_next.buffer, actual);
    }
    /* APT pads its parameter buffer to the requested length. Whole-range
       checked RAM write is atomic on a bad address/permission/MMIO; consume
       only after it succeeds. */
    if (size && snx_ctr_write_ram(p->space, address, buffer, size) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    if (object) {
        uint32_t result = snx_ctr_handle_add(p, object, &handle);
        if (result) {
            snx_ctr_ipc_error_reply(c, result);
            return;
        }
    }
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x104;
    c->out[2] = sender; /* the initial Wakeup's: none, no Home Menu process is claimed */
    c->out[3] = signal;
    c->out[4] = actual;
    c->out[5] = 0x10; /* move descriptor; absent optional object is wire NULL */
    c->out[6] = handle;
    c->out[7] = (size << 14) | 2; /* static buffer 0 */
    c->out[8] = address;
    if (glance) return;
    if (startup)
        p->apt_phase = 3; /* wakeup received, startup transition not yet released */
    else
        parameter_drop(p); /* the object's reference went to the handle */
}
/* a static buffer of the request at word i (its descriptor, then its address): the bytes, at most cap. 0 on success,
   else the transport's result */
static uint32_t static_input(SnxCtrIpc* c, unsigned i, uint32_t size, unsigned char* out, uint32_t cap) {
    if ((c->in[i] & 0x3fff) != 2 || (c->in[i] >> 14) != size || size > cap) return SNX_CTR_IPC_BAD_DESCRIPTOR;
    if (size && snx_ctr_read_ram(c->process->space, c->in[i + 1], out, size) != SNX_CTR_MEM_OK) return 0xe0e01bf5u;
    return 0;
}
/* the one handle a request copies at word i: its object, 0 for none (the zero handle), CTR_OBJECTS for a bad one */
static uint32_t copied_object(SnxCtrIpc* c, unsigned i) {
    if (c->in[i] != 0) return CTR_OBJECTS; /* one copied handle */
    if (!c->in[i + 1]) return 0;
    uint32_t id = snx_ctr_handle_object(c->process, c->in[i + 1]);
    return id ? id : CTR_OBJECTS;
}
/* the capture block the application copies its screens into for the library applet to show: one, made at the first
   Request and again when a later one asks for more. 0 when there is no memory left for it */
static uint32_t capture_block(SnxCtrProcess* p, uint32_t size) {
    uint32_t pages = (size + 4095) & ~4095u, handle, physical;
    if (p->apt_capture && p->objects[p->apt_capture].data.shared.size >= pages) return p->apt_capture;
    if (snx_ctr_object_create(p, CTR_OBJ_SHARED, &handle)) return 0;
    if (!(physical = snx_ctr_shared_pages(p, pages))) {
        (void)snx_ctr_handle_close(p, handle);
        return 0;
    }
    uint32_t id = snx_ctr_handle_object(p, handle);
    CtrObject* o = &p->objects[id];
    o->data.shared.physical = physical;
    o->data.shared.size = pages;
    o->data.shared.permissions = SNX_CTR_READ | SNX_CTR_WRITE;
    /* Azahar's CreateSharedMemoryForApplet makes the block read-write for its owner and for the others, at a base
       address of its own: the application maps it where it chooses, with other permissions that cover the block's
       (SharedMemory::Map) */
    o->data.shared.other_permissions = SNX_CTR_READ | SNX_CTR_WRITE;
    ++o->refs; /* APT keeps it for the applets */
    (void)snx_ctr_handle_close(p, handle);
    if (p->apt_capture) snx_ctr_object_release(p, p->apt_capture);
    p->apt_capture = id;
    return id;
}
/* SendParameter: source, destination, signal, size, a copied handle and the buffer. To the library applet APT
   registered: a Request for the capture block is answered with it (the Response), as each applet of the runtime
   answers it; any other parameter is the applet's to take. Any other destination is one the runtime does not
   deliver to */
static void send_parameter(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 4, 4)) return;
    SnxCtrProcess* p = c->process;
    uint32_t destination = c->in[2], signal = c->in[3], size = c->in[4], object = copied_object(c, 5);
    unsigned char buffer[APT_PARAMETER_CAPACITY];
    if (!p->apt_library || destination != p->apt_library) {
        c->state = SNX_CTR_IPC_UNKNOWN;
        return;
    }
    if (object == CTR_OBJECTS) {
        snx_ctr_ipc_bad(c);
        return;
    }
    uint32_t transport = static_input(c, 7, size, buffer, sizeof buffer);
    if (transport) {
        c->transport_result = transport;
        return;
    }
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x40;
    c->out[1] = p->apt_next.pending ? APT_PARAMETER_PRESENT : 0;
    if (c->out[1]) return;
    if (signal == SNX_CTR_APT_REQUEST && size == APT_CAPTURE_INFO_SIZE && snx_ld_le32(buffer) &&
        snx_ld_le32(buffer) <= APT_CAPTURE_LIMIT) {
        uint32_t block = capture_block(p, snx_ld_le32(buffer));
        if (!block) {
            fprintf(stderr, "3ds apt: no memory left for library applet 0x%03x's %u-byte capture block\n",
                    (unsigned)p->apt_library, (unsigned)snx_ld_le32(buffer));
            c->state = SNX_CTR_IPC_UNSUPPORTED;
            return;
        }
        snx_ctr_apt_send(p, p->apt_library, SNX_CTR_APT_RESPONSE, block, NULL, 0);
    } else if (snx_ctr_swkbd_is(p->apt_library))
        snx_ctr_swkbd_parameter(c, signal, buffer, size, object);
    else {
        /* the Mii selector answers as soon as it starts: a Request is all it takes (Azahar's ReceiveParameterImpl) */
        fprintf(stderr,
                "3ds apt: the game sends the Mii selector signal %u with %u bytes, which it does not take (a "
                "Request with a 32-byte capture info)\n",
                (unsigned)signal, (unsigned)size);
        c->state = SNX_CTR_IPC_UNSUPPORTED;
    }
}
/* StartLibraryApplet: the applet APT registered, its parameter and a copied handle: the software keyboard's
   configuration and the block its text goes in, or the Mii selector's configuration (it uses no object, as Azahar's
   takes none) */
static void start_library_applet(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 2, 4)) return;
    SnxCtrProcess* p = c->process;
    uint32_t app_id = c->in[1], size = c->in[2], object = copied_object(c, 3);
    unsigned char buffer[APT_PARAMETER_CAPACITY];
    if (!p->apt_library || app_id != p->apt_library) {
        c->state = SNX_CTR_IPC_UNKNOWN;
        return;
    }
    if (object == CTR_OBJECTS) {
        snx_ctr_ipc_bad(c);
        return;
    }
    uint32_t transport = static_input(c, 5, size, buffer, sizeof buffer);
    if (transport) {
        c->transport_result = transport;
        return;
    }
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x40;
    c->out[1] = p->apt_next.pending ? APT_PARAMETER_PRESENT : 0;
    if (c->out[1]) return;
    if (snx_ctr_swkbd_is(app_id))
        snx_ctr_swkbd_start(c, buffer, size, object);
    else
        snx_ctr_mii_selector_start(c, buffer, size);
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
void snx_ctr_apt_dispatch(SnxCtrIpc* c) {
    switch (c->in[0] >> 16) {
        case 0x0c: send_parameter(c); break;
        case 0x0d:
        case 0x0e: receive_parameter(c, c->in[0] >> 16 == 0x0e); break;
        case 0x1e: start_library_applet(c); break;
        case 0x4b: applet_utility(c); break;
        default: snx_ctr_apt_wire_dispatch(c); break;
    }
}
