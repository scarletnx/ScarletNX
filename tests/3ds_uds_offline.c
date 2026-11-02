/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nwm::UDS with the lan setting: Initialize succeeds without a relay socket, a scan finds no network, a join finds none
   and a hosted network has only its own node. Airplane mode, off and unknown settings refuse with the network error,
   and room without an endpoint fails before any socket. */
#include "3ds/runtime/internal.h"
#include "3ds/services/system.h"
#include "snx_settings.h"
#include "support/3ds_guest.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define BUFFER          0x300000u
#define SHARED          0x301000u
#define NET             0x300900u
#define DATA            0x300b00u
#define UDS_BAD         0xd9001830u
#define UDS_STATE       0xc86113eau
#define UDS_NOT_FOUND   0xc8a113fau
#define UDS_NETWORK     0xc8a113f8u
#define STATUS_IDLE     3
#define STATUS_HOSTING  6
/* the connection status reply: the state, the node id in its low half word and the node count in its low byte */
#define STATUS_STATE    (CTR_COMMAND + 8)
#define STATUS_NODE     (CTR_COMMAND + 16)
#define STATUS_NODES    (CTR_COMMAND + 52)
/* the network description: its channel and its maximum of nodes */
#define NET_CHANNEL     6
#define NET_MAX_NODES   29
#define SCAN_LIMIT      2048u
#define SCAN_DESCRIPTOR (SCAN_LIMIT << 4 | 12)
static void env(const char* key, const char* value) {
#ifdef _WIN32
    int failed = _putenv_s(key, value);
    assert(!failed);
#else
    int failed = setenv(key, value, 1);
    assert(!failed);
#endif
}
/* The result of the transport, before the service replies */
static uint32_t transport(SnxCtrProcess* p, uint32_t handle, const uint32_t* words, unsigned count) {
    ctr_request(p, words, count);
    uint32_t result = 0xffffffffu;
    bool sent = snx_ctr_ipc_send(p, handle, &result) == SNX_CTR_SVC_RETURN;
    assert(sent);
    return result;
}
/* A request that reaches the service: the result is the service's reply */
static uint32_t call(SnxCtrProcess* p, uint32_t handle, const uint32_t* words, unsigned count) {
    uint32_t transported = transport(p, handle, words, count);
    assert(!transported);
    return ctr_word(p, CTR_COMMAND + 4);
}
static void unsupported(SnxCtrProcess* p, uint32_t handle, uint32_t header) {
    ctr_request(p, &header, 1);
    uint32_t result = 0xffffffffu;
    bool sent = snx_ctr_ipc_send(p, handle, &result) == SNX_CTR_SVC_UNSUPPORTED;
    assert(sent);
    assert(!result);
}
static uint32_t own_node(SnxCtrProcess* p) { return ctr_word(p, STATUS_NODE) & 0xffff; }
static uint32_t node_count(SnxCtrProcess* p) { return ctr_word(p, STATUS_NODES) & 0xff; }
/* The application's own shared block, made by SVC 0x1e from a private data page */
static SnxCtrProcess* process(uint32_t* handle, uint32_t* shared) {
    uint8_t code[0x3000] = {0};
    SnxCtrImage image = {.segment = {{0x100000, 1, 16}, {0x200000, 1, 4}, {BUFFER, 1, 4096}},
                         .bss_size = 0x1000,
                         .stack_size = 0x4000,
                         .main_priority = 0x30,
                         .code = code,
                         .code_size = sizeof code};
    SnxCtrProcess* p = snx_ctr_process_new(&image, (SnxCtrModel)0);
    assert(p);
    p->title_id = 0x0004000012345600ull;
    snprintf(p->title, sizeof p->title, "UDS offline fixture");
    uint32_t opened = snx_ctr_system_open(p, (const uint8_t*)"nwm::UDS", 8, handle);
    assert(!opened);
    uint32_t* r = p->cpu.state.cpu.r;
    r[0] = 3; /* everyone else: read-write */
    r[1] = SHARED;
    r[2] = 4096;
    r[3] = 3; /* the application: read-write */
    bool returned = snx_ctr_svc(p, 0x1e) == SNX_CTR_SVC_RETURN;
    assert(returned);
    assert(!r[0] && r[1]);
    *shared = r[1];
    uint32_t id = snx_ctr_handle_object(p, *shared);
    assert(id && p->objects[id].type == CTR_OBJ_SHARED);
    assert(p->objects[id].data.shared.size == 4096);
    assert(p->objects[id].data.shared.address == SHARED);
    assert(p->objects[id].data.shared.permissions == 3);
    assert(p->objects[id].data.shared.other_permissions == 3);
    assert(p->objects[id].data.shared.by_application == 1);
    assert(!p->objects[id].data.shared.physical);
    return p;
}
static void initialize_words(uint32_t words[15], uint32_t shared) {
    memset(words, 0, 15 * sizeof *words);
    words[0] = 0x1b0302; /* command 1b, twelve normal words, two translated */
    words[1] = 4096;
    words[14] = shared; /* after the copied handle descriptor in words[13], which is 0 for one handle */
}
static uint32_t initialize(SnxCtrProcess* p, uint32_t handle, uint32_t shared) {
    uint32_t words[15];
    initialize_words(words, shared);
    return call(p, handle, words, 15);
}
static void scan_words(uint32_t words[21], uint32_t event, uint32_t desc, uint32_t dest, uint32_t limit) {
    memset(words, 0, 21 * sizeof *words);
    words[0] = 0xf0404; /* command f, sixteen normal words, four translated */
    words[1] = limit;
    words[4] = words[5] = UINT32_MAX;
    words[18] = event;
    words[19] = desc;
    words[20] = dest;
}
static uint32_t scan(SnxCtrProcess* p, uint32_t handle, uint32_t event) {
    uint32_t words[21];
    scan_words(words, event, SCAN_DESCRIPTOR, BUFFER, SCAN_LIMIT);
    return call(p, handle, words, 21);
}
static uint32_t status(SnxCtrProcess* p, uint32_t handle) {
    uint32_t words[] = {0xb0000};
    return call(p, handle, words, 1);
}
static uint32_t shutdown_process(SnxCtrProcess* p, uint32_t handle) {
    uint32_t words[] = {0x30000};
    return call(p, handle, words, 1);
}
static uint32_t initialize_ok(SnxCtrProcess* p, uint32_t handle, uint32_t shared, unsigned base) {
    uint32_t reply = initialize(p, handle, shared);
    assert(!reply);
    assert(p->uds);
    assert(ctr_word(p, CTR_COMMAND) == 0x1b0042);
    uint32_t event = ctr_word(p, CTR_COMMAND + 12);
    uint32_t id = snx_ctr_handle_object(p, event);
    assert(event && id && p->objects[id].type == CTR_OBJ_EVENT);
    assert(ctr_object_count(p) == base + 1);
    return event;
}
static void shutdown_ok(SnxCtrProcess* p, uint32_t handle, uint32_t event, unsigned base) {
    uint32_t reply = shutdown_process(p, handle);
    assert(!reply);
    assert(!p->uds);
    assert(ctr_word(p, CTR_COMMAND) == 0x30040);
    uint32_t closed = snx_ctr_handle_close(p, event);
    assert(!closed);
    assert(ctr_object_count(p) == base);
}
static void rejected(SnxCtrProcess* p, uint32_t handle, uint32_t shared, uint32_t expected, unsigned base) {
    uint32_t reply = initialize(p, handle, shared);
    assert(reply == expected);
    assert(!p->uds);
    assert(ctr_word(p, CTR_COMMAND) == 0x1b0040);
    assert(!ctr_word(p, CTR_COMMAND + 12));
    assert(ctr_object_count(p) == base);
}
/* A refused request leaves the block, its ownership and the service state as the kernel made them */
static void assert_shared_untouched(SnxCtrProcess* p, uint32_t shared, uint32_t id, uint32_t refs, unsigned base) {
    assert(snx_ctr_handle_object(p, shared) == id);
    assert(p->objects[id].type == CTR_OBJ_SHARED);
    assert(p->objects[id].data.shared.size == 4096);
    assert(p->objects[id].data.shared.address == SHARED);
    assert(p->objects[id].data.shared.permissions == 3);
    assert(p->objects[id].data.shared.other_permissions == 3);
    assert(p->objects[id].data.shared.by_application == 1);
    assert(!p->objects[id].data.shared.physical);
    assert(p->objects[id].refs == refs);
    assert(!p->uds);
    assert(ctr_word(p, CTR_COMMAND) == 0x1b0040);
    assert(!ctr_word(p, CTR_COMMAND + 12));
    assert(ctr_object_count(p) == base);
}
int main(void) {
    g_settings.airplane_mode = 0;
    snprintf(g_settings.local_wireless, sizeof g_settings.local_wireless, "lan");
    g_settings.eden_room[0] = 0;
    env("SNX_3DS_ROOM", "");
    uint32_t handle, shared;
    SnxCtrProcess* p = process(&handle, &shared);
    uint32_t shared_id = snx_ctr_handle_object(p, shared);
    assert(shared_id && p->objects[shared_id].type == CTR_OBJ_SHARED);
    unsigned base = ctr_object_count(p);
    /* lan initializes with no peer: the status is idle with no node */
    uint32_t event = initialize_ok(p, handle, shared, base);
    uint32_t reply = status(p, handle);
    assert(!reply);
    assert(ctr_word(p, CTR_COMMAND) == 0xb0340);
    assert(ctr_word(p, STATUS_STATE) == STATUS_IDLE);
    assert(!own_node(p));
    assert(!node_count(p));
    reply = scan(p, handle, event);
    assert(!reply);
    assert(ctr_word(p, CTR_COMMAND) == 0xf0042);
    assert(ctr_word(p, CTR_COMMAND + 8) == SCAN_DESCRIPTOR);
    assert(ctr_word(p, CTR_COMMAND + 12) == BUFFER);
    assert(ctr_word(p, BUFFER) == SCAN_LIMIT);
    assert(ctr_word(p, BUFFER + 4) == 12);
    assert(ctr_word(p, BUFFER + 8) == 0);
    /* A descriptor shorter than the list is refused without writing the buffer */
    ctr_put(p, BUFFER, 0xfeedface);
    ctr_put(p, BUFFER + 4, 0x12345678);
    uint32_t malformed[21];
    scan_words(malformed, event, 16 << 4 | 12, BUFFER, SCAN_LIMIT);
    reply = transport(p, handle, malformed, 21);
    assert(reply == UDS_BAD);
    assert(ctr_word(p, BUFFER) == 0xfeedface && ctr_word(p, BUFFER + 4) == 0x12345678);
    reply = scan(p, handle, event);
    assert(p->uds && !reply);
    assert(ctr_word(p, BUFFER + 8) == 0);
    /* commands 0x22 and 0x23 are not implemented */
    unsupported(p, handle, 0x220000);
    unsupported(p, handle, 0x230000);
    /* a second initialization while active leaves the state untouched */
    uint32_t words[15];
    initialize_words(words, shared);
    reply = call(p, handle, words, 15);
    assert(reply == UDS_STATE);
    assert(p->uds && ctr_word(p, CTR_COMMAND + 12) == 0);
    /* no network is discovered, so a join reports not found */
    uint8_t network[264] = {0};
    network[NET_CHANNEL] = 11;
    network[NET_MAX_NODES] = 2;
    bool written = snx_ctr_write_ram(p->space, NET, network, sizeof network) == SNX_CTR_MEM_OK;
    assert(written);
    written = snx_ctr_write_ram(p->space, DATA, "game-pass", 10) == SNX_CTR_MEM_OK;
    assert(written);
    uint32_t joining[] = {0x1e0084, 1, 10, 264 << 14 | 1 << 10 | 2, NET, 10 << 14 | 2, DATA};
    reply = call(p, handle, joining, 7);
    assert(reply == UDS_NOT_FOUND);
    /* a hosted network has its own node and no relay socket, so destroying it sends nothing */
    uint32_t hosting[] = {0x1d0044, 10, 264 << 14 | 1 << 10 | 2, NET, 10 << 14 | 2, DATA};
    reply = call(p, handle, hosting, 6);
    assert(!reply);
    assert(ctr_word(p, CTR_COMMAND) == 0x1d0040);
    reply = status(p, handle);
    assert(!reply);
    assert(ctr_word(p, STATUS_STATE) == STATUS_HOSTING);
    assert(own_node(p) == 1);
    assert(node_count(p) == 1);
    uint32_t destroying[] = {0x80000};
    reply = call(p, handle, destroying, 1);
    assert(!reply);
    assert(ctr_word(p, CTR_COMMAND) == 0x80040);
    reply = status(p, handle);
    assert(!reply);
    assert(ctr_word(p, STATUS_STATE) == STATUS_IDLE);
    shutdown_ok(p, handle, event, base);
    /* a command after shutdown reports the missing service state */
    uint32_t after[21];
    scan_words(after, 0, SCAN_DESCRIPTOR, BUFFER, SCAN_LIMIT);
    reply = call(p, handle, after, 21);
    assert(reply == UDS_STATE);
    /* shutdown and reinitialization leave no object behind */
    event = initialize_ok(p, handle, shared, base);
    shutdown_ok(p, handle, event, base);
    /* airplane mode refuses lan and room, and off and unknown values refuse */
    g_settings.airplane_mode = 1;
    reply = initialize(p, handle, shared);
    assert(reply == UDS_NETWORK && !p->uds);
    snprintf(g_settings.local_wireless, sizeof g_settings.local_wireless, "room");
    reply = initialize(p, handle, shared);
    assert(reply == UDS_NETWORK && !p->uds);
    g_settings.airplane_mode = 0;
    /* room with no endpoint fails before joining or opening a socket */
    rejected(p, handle, shared, UDS_NETWORK, base);
    snprintf(g_settings.local_wireless, sizeof g_settings.local_wireless, "off");
    rejected(p, handle, shared, UDS_NETWORK, base);
    snprintf(g_settings.local_wireless, sizeof g_settings.local_wireless, "wifi");
    rejected(p, handle, shared, UDS_NETWORK, base);
    snprintf(g_settings.local_wireless, sizeof g_settings.local_wireless, "lan");
    /* shared memory that is missing, of the wrong type or advertised at the wrong length is refused before any state
       exists */
    uint32_t missing[15] = {0};
    missing[0] = 0x1b0302;
    missing[1] = 4096;
    missing[13] = 0x20; /* the caller's PID: no handle in the request */
    reply = call(p, handle, missing, 15);
    assert(reply == UDS_BAD);
    assert(!p->uds && ctr_object_count(p) == base);
    uint32_t temporary;
    uint32_t created = snx_ctr_object_create(p, CTR_OBJ_EVENT, &temporary);
    assert(!created);
    initialize_words(words, temporary);
    reply = call(p, handle, words, 15);
    assert(reply == UDS_BAD);
    assert(!p->uds && ctr_object_count(p) == base + 1);
    uint32_t closed = snx_ctr_handle_close(p, temporary);
    assert(!closed);
    assert(ctr_object_count(p) == base);
    uint32_t shared_refs = p->objects[shared_id].refs;
    initialize_words(words, shared);
    words[1] = 4095;
    reply = call(p, handle, words, 15);
    assert(reply == UDS_BAD);
    assert_shared_untouched(p, shared, shared_id, shared_refs, base);
    words[1] = 8192;
    reply = call(p, handle, words, 15);
    assert(reply == UDS_BAD);
    assert_shared_untouched(p, shared, shared_id, shared_refs, base);
    /* a well-formed request of the wrong form is a transport bad descriptor */
    uint32_t wrong_form[14] = {0};
    wrong_form[0] = 0x1b02c2;
    wrong_form[1] = 4096;
    wrong_form[13] = shared;
    reply = transport(p, handle, wrong_form, 14);
    assert(reply == UDS_BAD);
    assert(!p->uds);
    /* the refusals leave the service able to initialize */
    event = initialize_ok(p, handle, shared, base);
    shutdown_ok(p, handle, event, base);
    snx_ctr_process_free(p);
    puts("UDS lan: initialization, scan, join, host, shutdown and refusals passed");
    return 0;
}
