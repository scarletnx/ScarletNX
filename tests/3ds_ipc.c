/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/aot/ctr_module.h"
#include "3ds/runtime/internal.h"
#include "3ds/ipc/ctr_ipc.h"
#include "3ds/services/system.h"
#include "snx_settings.h"
#include "support/3ds_guest.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t connect(SnxCtrProcess* p) {
    assert(snx_ctr_write_ram(p->space, 0x300100, "srv:", 5) == SNX_CTR_MEM_OK);
    p->cpu.state.cpu.r[1] = 0x300100;
    assert(snx_ctr_svc(p, 0x2d) == SNX_CTR_SVC_RETURN && !p->cpu.state.cpu.r[0]);
    return p->cpu.state.cpu.r[1];
}
static void close_handle(SnxCtrProcess* p, uint32_t handle) {
    p->cpu.state.cpu.r[0] = handle;
    assert(snx_ctr_svc(p, 0x23) == SNX_CTR_SVC_RETURN && !p->cpu.state.cpu.r[0]);
}
static bool no_mmio(void* context, uint32_t address, void* bytes, size_t size) {
    (void)context;
    (void)address;
    (void)bytes;
    (void)size;
    assert(0 && "IPC must never invoke MMIO");
    return false;
}
static void lookup(SnxCtrProcess* p, uint32_t handle, uint32_t flags) {
    uint32_t args[] = {0x00050100, 0x553a7366, 0x00524553, 7, flags}; /* fs:USER */
    ctr_request(p, args, 5);
    ctr_send(p, handle, SNX_CTR_SVC_RETURN, 0);
}
int main(void) {
    /* the console's answers: an unknown command or service stops, but where the tests below answer it */
    g_settings.ctr_answer_unknown_commands = 0;
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = ctr_image(code, sizeof code, 0x1000);
    for (unsigned model = 0; model < 2; ++model) {
        SnxCtrProcess* p = snx_ctr_process_new(&image, (SnxCtrModel)model);
        assert(p);
        uint32_t* r = p->cpu.state.cpu.r;
        r[1] = CTR_PROCESS_HANDLE;
        assert(snx_ctr_svc(p, 0x35) == SNX_CTR_SVC_RETURN && !r[0] && r[1] == CTR_PROCESS_ID);
        r[1] = CTR_THREAD_HANDLE;
        assert(snx_ctr_svc(p, 0x35) == SNX_CTR_SVC_RETURN && r[0] == 0xd8e007f7 && !r[1]);
        uint32_t srv = connect(p), id = snx_ctr_handle_object(p, srv);
        assert(id && p->objects[id].type == CTR_OBJ_SESSION && ctr_object_count(p) == 3);
        lookup(p, srv, 1);
        assert(ctr_command_word(p, 0) == 0x00050040 && ctr_command_word(p, 1) == 0xd8e06406);
        uint32_t registration[] = {0x00010002, 0x20, 0xdeadbeef};
        ctr_request(p, registration, 3);
        /* Read-only/unmapped/misaligned/MMIO TLS: no handler mutation. */
        assert(snx_ctr_protect(p->space, SNX_CTR_INITIAL_TLS, 4096, 1) == SNX_CTR_MEM_OK);
        ctr_send(p, srv, SNX_CTR_SVC_RETURN, 0xe0e01bf5);
        assert(!p->objects[id].data.session.client_pid && ctr_command_word(p, 0) == registration[0]);
        assert(snx_ctr_protect(p->space, SNX_CTR_INITIAL_TLS, 4096, 3) == SNX_CTR_MEM_OK);
        uint32_t tls_cases[] = {0x70000000, SNX_CTR_INITIAL_TLS + 1, 0xfffffe00, 0x50000000};
        assert(snx_ctr_map(p->space, 0x50000000, 0x10100000, 4096, 3) == SNX_CTR_MEM_OK);
        snx_ctr_memory_mmio(p->memory, NULL, no_mmio, NULL);
        for (unsigned i = 0; i < sizeof tls_cases / sizeof *tls_cases; ++i) {
            p->cpu.state.cpu.tpidruro = tls_cases[i];
            ctr_send(p, srv, SNX_CTR_SVC_RETURN, 0xe0e01bf5);
            assert(!p->objects[id].data.session.client_pid);
        }
        p->cpu.state.cpu.tpidruro = SNX_CTR_INITIAL_TLS;
        const unsigned counts[] = {0, 1, 62, 63};
        for (unsigned a = 0; a < 4; ++a)
            for (unsigned b = 0; b < 4; ++b) {
                uint32_t edge[64] = {0};
                edge[0] = 0x10000 | counts[a] << 6 | counts[b];
                if (counts[a] < 63) edge[1 + counts[a]] = 0x21;
                ctr_request(p, edge, 64);
                ctr_send(p, srv, SNX_CTR_SVC_RETURN, SNX_CTR_IPC_BAD_DESCRIPTOR);
                assert(!p->objects[id].data.session.client_pid && ctr_object_count(p) == 3);
            }
        uint32_t bad_headers[] = {0x00010000, 0x00010040, 0x0001f002, 0x00010fff};
        for (unsigned i = 0; i < sizeof bad_headers / sizeof *bad_headers; ++i) {
            uint32_t bad[] = {bad_headers[i], 0x20, 0};
            ctr_request(p, bad, 3);
            ctr_send(p, srv, SNX_CTR_SVC_RETURN, SNX_CTR_IPC_BAD_DESCRIPTOR);
            assert(ctr_command_word(p, 0) == bad[0] && !p->objects[id].data.session.client_pid);
        }
        uint32_t malformed[] = {0x00010002, 0x21, 0};
        ctr_request(p, malformed, 3);
        ctr_send(p, srv, SNX_CTR_SVC_RETURN, SNX_CTR_IPC_BAD_DESCRIPTOR);
        assert(!p->objects[id].data.session.client_pid);
        uint32_t truncated[] = {0x00010001, 0x20};
        ctr_request(p, truncated, 2);
        ctr_send(p, srv, SNX_CTR_SVC_RETURN, SNX_CTR_IPC_BAD_DESCRIPTOR);
        ctr_request(p, registration, 3);
        ctr_send(p, srv, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 0) == 0x00010040 && !ctr_command_word(p, 1) && !ctr_command_word(p, 2));
        assert(p->objects[id].data.session.client_pid == CTR_PROCESS_ID); /* not forged PID */
        for (unsigned i = 2; i < 64; ++i) assert(!ctr_command_word(p, i));
        lookup(p, srv, 0);
        assert(ctr_command_word(p, 0) == 0x00050042 && !ctr_command_word(p, 1) && ctr_command_word(p, 2) == 0x10);
        uint32_t fs = ctr_command_word(p, 3), fid = snx_ctr_handle_object(p, fs);
        assert(fid && fid != id && p->objects[fid].data.session.service == SNX_CTR_SERVICE_FS);
        assert(ctr_object_count(p) == 4 && !p->objects[fid].data.session.client_pid);
        uint32_t original_init[] = {0x08010002, 0x20, 0xdeadbeef};
        ctr_request(p, original_init, 3);
        ctr_send(p, fs, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 0) == 0x08010040 && !ctr_command_word(p, 1) &&
               p->objects[fid].data.session.client_pid == CTR_PROCESS_ID);
        uint32_t init[] = {0x08610042, 0x11223344, 0x20, 0xffffffff};
        ctr_request(p, init, 4);
        ctr_send(p, fs, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 0) == 0x08610040 && !ctr_command_word(p, 1));
        assert(p->objects[fid].data.session.client_pid == CTR_PROCESS_ID &&
               p->objects[fid].data.session.sdk_version == 0x11223344);
        /* a command the service lacks stops the run, unless the stub list names the service: then it answers
           success with one normal word, as Azahar's ReportUnimplementedFunction does */
        uint32_t unknown[] = {0xbeef0000};
        ctr_request(p, unknown, 1);
        ctr_send(p, fs, SNX_CTR_SVC_UNSUPPORTED, 0);
        assert(ctr_command_word(p, 0) == unknown[0] && ctr_object_count(p) == 4);
        uint32_t mask = 0;
        assert(snx_ctr_ipc_stub_mask("apt,fs", &mask) && !snx_ctr_ipc_stub_mask("fs,nope", &mask));
        /* a file session is fs's; a refused list leaves the mask as it was, and lists add up */
        assert(mask == (1u << SNX_CTR_SERVICE_APT | 1u << SNX_CTR_SERVICE_FS | 1u << SNX_CTR_SERVICE_FILE));
        assert(!snx_ctr_ipc_stub_mask("", &mask) && !snx_ctr_ipc_stub_mask("fs,", &mask) &&
               !snx_ctr_ipc_stub_mask(",fs", &mask) && !snx_ctr_ipc_stub_mask("fs,,apt", &mask));
        assert(snx_ctr_ipc_stub_mask("dsp", &mask) && (mask & 1u << SNX_CTR_SERVICE_DSP) &&
               (mask & 1u << SNX_CTR_SERVICE_APT));
        assert(snx_ctr_ipc_stub_mask("fs", &p->ipc_stubs));
        ctr_request(p, unknown, 1);
        ctr_send(p, fs, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 0) == 0xbeef0040 && !ctr_command_word(p, 1));
        p->ipc_stubs = 0;
        /* Any well-formed descriptor is translated: a mapped buffer the command does not take reaches the handler,
           which refuses its form; one over memory the caller cannot read never does */
        uint32_t buffer[] = {0x08010002, 0x0000100a, 0x300000};
        ctr_request(p, buffer, 3);
        ctr_send(p, fs, SNX_CTR_SVC_RETURN, 0xd9001830u);
        uint32_t unmapped[] = {0x08010002, 0x0000100a, 0x00500000};
        ctr_request(p, unmapped, 3);
        ctr_send(p, fs, SNX_CTR_SVC_RETURN, 0xe0e01bf5u);
        /* PXI buffers (libctru IPC_Desc_PXIBuffer): 6 the service only reads, so read-only code passes; 4 it also
           writes, so the same memory does not */
        uint32_t pxi[] = {0x08010002, 0x00001006, 0x100000};
        ctr_request(p, pxi, 3);
        ctr_send(p, fs, SNX_CTR_SVC_RETURN, 0xd9001830u);
        pxi[1] = 0x00001004;
        ctr_request(p, pxi, 3);
        ctr_send(p, fs, SNX_CTR_SVC_RETURN, 0xe0e01bf5u);
        /* more handles than the runtime translates stop the run instead of inventing an answer */
        uint32_t many[11] = {0xbeef000a, 0x20000000};
        ctr_request(p, many, 11);
        ctr_send(p, fs, SNX_CTR_SVC_UNSUPPORTED, 0);
        /* a moved handle leaves the caller's table once the request is answered (here a stubbed command) */
        uint32_t moved = 0;
        assert(!snx_ctr_object_create(p, CTR_OBJ_EVENT, &moved));
        uint32_t move[] = {0xbeef0002, 0x10, moved};
        assert(snx_ctr_ipc_stub_mask("fs", &p->ipc_stubs));
        ctr_request(p, move, 3);
        ctr_send(p, fs, SNX_CTR_SVC_RETURN, 0);
        assert(!snx_ctr_handle_object(p, moved) && ctr_object_count(p) == 4);
        p->ipc_stubs = 0;
        /* ... and also when the service refuses the request's form */
        assert(!snx_ctr_object_create(p, CTR_OBJ_EVENT, &moved));
        uint32_t refused[] = {0x08010004, 0x20, 0, 0x10, moved};
        ctr_request(p, refused, 5);
        ctr_send(p, fs, SNX_CTR_SVC_RETURN, 0xd9001830u);
        assert(!snx_ctr_handle_object(p, moved) && ctr_object_count(p) == 4);
        close_handle(p, fs);
        ctr_request(p, init, 4);
        ctr_send(p, fs, SNX_CTR_SVC_RETURN, 0xd8e007f7);
        assert(ctr_command_word(p, 0) == init[0] && ctr_object_count(p) == 3);
        uint32_t ports[63];
        for (unsigned i = 0; i < 63; ++i) ports[i] = connect(p);
        r[1] = 0x300100;
        assert(snx_ctr_svc(p, 0x2d) == SNX_CTR_SVC_RETURN && r[0] == 0xd0401834 && !r[1]);
        for (unsigned i = 0; i < 63; ++i) close_handle(p, ports[i]);
        const uint32_t bad_names[] = {0xffffffff, 0x70000000, 0x50000000};
        for (unsigned i = 0; i < 3; ++i) {
            r[1] = bad_names[i];
            assert(snx_ctr_svc(p, 0x2d) == SNX_CTR_SVC_RETURN && r[0] == 0xe0e01bf5 && !r[1] &&
                   ctr_object_count(p) == 3);
        }
        assert(snx_ctr_write_ram(p->space, 0x300100, "other", 6) == SNX_CTR_MEM_OK);
        r[1] = 0x300100;
        assert(snx_ctr_svc(p, 0x2d) == SNX_CTR_SVC_RETURN && r[0] == 0xd88007fa && !r[1]);
        assert(snx_ctr_write_ram(p->space, 0x300100, "abcdefghijkl", 12) == SNX_CTR_MEM_OK);
        r[1] = 0x300100;
        assert(snx_ctr_svc(p, 0x2d) == SNX_CTR_SVC_RETURN && r[0] == 0xe0e0181e && !r[1]);
        /* A duplicated session shares client identity and owns a reference. */
        r[1] = srv;
        assert(snx_ctr_svc(p, 0x27) == SNX_CTR_SVC_RETURN && !r[0]);
        uint32_t duplicate = r[1];
        close_handle(p, srv);
        lookup(p, duplicate, 0);
        assert(!ctr_command_word(p, 1) && snx_ctr_handle_object(p, ctr_command_word(p, 3)));
        close_handle(p, ctr_command_word(p, 3));
        srv = duplicate;
        /* Full handle table: tentative session object is rolled back. */
        uint32_t extras[CTR_HANDLES];
        unsigned count = 0;
        for (;;) {
            r[1] = CTR_PROCESS_HANDLE;
            assert(snx_ctr_svc(p, 0x27) == SNX_CTR_SVC_RETURN);
            if (r[0]) break;
            extras[count++] = r[1];
        }
        assert(count == CTR_HANDLES - 1 && ctr_object_count(p) == 3);
        lookup(p, srv, 0);
        assert(ctr_command_word(p, 0) == 0x00050040 && ctr_command_word(p, 1) == 0xd8600413 &&
               ctr_object_count(p) == 3);
        for (unsigned i = 0; i < count; ++i) close_handle(p, extras[i]);
        uint32_t sessions[32];
        for (unsigned i = 0; i < 32; ++i) {
            lookup(p, srv, 0);
            assert(!ctr_command_word(p, 1));
            sessions[i] = ctr_command_word(p, 3);
        }
        lookup(p, srv, 0);
        assert(ctr_command_word(p, 1) == 0xd0401834 && ctr_object_count(p) == 35);
        for (unsigned i = 0; i < 32; ++i) close_handle(p, sessions[i]);
        uint32_t absent[] = {0x00050100, 0x5353494d, 0x474e49, 7, 1}; /* MISSING, non-waiting */
        ctr_request(p, absent, 5);
        ctr_send(p, srv, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 0) == 0x00050040 && ctr_command_word(p, 1) == 0xd0406401 &&
               !snx_ctr_kernel_blocked(p));
        /* answering unknown commands, the runtime's default (ctr_auto_stub_ipc): a service it does not know is a
           stand-in, each command of which answers success and nothing else, and so is an unknown command of a known
           service, with no stub list */
        g_settings.ctr_answer_unknown_commands = 1;
        absent[4] = 0;
        ctr_request(p, absent, 5);
        ctr_send(p, srv, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 0) == 0x00050042 && !ctr_command_word(p, 1) && ctr_command_word(p, 2) == 0x10 &&
               !snx_ctr_kernel_blocked(p));
        uint32_t stand_in = ctr_command_word(p, 3), command[] = {0x12340080, 7, 8};
        assert(!strcmp(snx_ctr_system_name(p, p->objects[snx_ctr_handle_object(p, stand_in)].data.session.name),
                       "MISSING"));
        ctr_request(p, command, 3);
        ctr_send(p, stand_in, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 0) == 0x12340040 && !ctr_command_word(p, 1) && !ctr_command_word(p, 2));
        close_handle(p, stand_in);
        uint32_t unknown_srv[] = {0x7fff0000};
        ctr_request(p, unknown_srv, 1);
        ctr_send(p, srv, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 0) == 0x7fff0040 && !ctr_command_word(p, 1));
        g_settings.ctr_answer_unknown_commands = 0;
        ctr_request(p, absent, 5);
        ctr_send(p, srv, SNX_CTR_SVC_BLOCKED, 0);
        assert(ctr_command_word(p, 0) == absent[0] && !strcmp(snx_ctr_thread(p)->waiting_service, "MISSING") &&
               snx_ctr_kernel_blocked(p));
        uint64_t executed = 99;
        SnxCtrModule empty = {0};
        assert(snx_ctr_dispatch(&empty, p, 1, &executed) == SNX_CTR_RUN_BLOCKED && !executed);
        assert(snx_ctr_svc(p, 0x23) == SNX_CTR_SVC_BLOCKED && ctr_object_count(p) == 3);
        snx_ctr_process_free(p);
    }
    puts("CTR IPC/session identity, PID replacement, permissions, descriptors, quotas and truthful waits passed");
    return 0;
}
