/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Generator-only ISO C fixture. Fake resource accounting is deliberately local
   to this fixture; tests/3ds_ipc.c separately executes real kernel sessions. */
#include "tests/ctr_swipc.swipc.inc"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned calls, owned;
void snx_ctr_ipc_bad(SnxCtrIpc* c) { c->transport_result = SNX_CTR_IPC_BAD_DESCRIPTOR; }
int snx_ctr_ipc_expect(SnxCtrIpc* c, uint32_t normal, uint32_t translated) {
    if (((c->in[0] >> 6) & 63) != normal || (c->in[0] & 63) != translated) {
        snx_ctr_ipc_bad(c);
        return 0;
    }
    return 1;
}
void snx_ctr_ipc_get_bytes(SnxCtrIpc* c, uint32_t first, uint8_t* b, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; ++i) b[i] = (uint8_t)(c->in[first + i / 4] >> (i % 4 * 8));
}
void snx_ctr_ipc_put_bytes(SnxCtrIpc* c, uint32_t first, const uint8_t* b, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; ++i) c->out[first + i / 4] |= (uint32_t)b[i] << (i % 4 * 8);
}
void snx_ctr_ipc_error_reply(SnxCtrIpc* c, uint32_t result) {
    if (c->transport_result || c->state != SNX_CTR_IPC_RETURN) return;
    memset(c->out, 0, sizeof c->out);
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x40;
    c->out[1] = result;
}
void snx_ctr_ipc_drop_handle(SnxCtrIpc* c, uint32_t handle) {
    (void)c;
    if (handle) {
        assert(handle == 0x222 && owned == 1);
        --owned;
    }
}
void snx_ctr_ipc_output_handle(SnxCtrIpc* c, uint32_t word, uint32_t handle) {
    if (!handle) {
        c->state = SNX_CTR_IPC_FAULT;
        return;
    }
    assert(handle == 0x222 && owned == 1);
    c->out[word] = handle;
}
static uint32_t test_ctr_RoundTrip(SnxCtrIpc* c, uint32_t tag, uint64_t value, const uint8_t* name, uint32_t process,
                                   uint64_t* echoed, uint8_t* copy, uint32_t* identity) {
    (void)c;
    ++calls;
    assert(tag == 0x89abcdef && value == (0xfedcba98ull << 32 | 0x76543210));
    assert(!memcmp(name, "12345678", 8) && process == 0x10);
    *echoed = value;
    memcpy(copy, name, 8);
    *identity = process;
    return 0;
}
static uint32_t test_ctr_Owned(SnxCtrIpc* c, uint32_t outcome, uint32_t* object) {
    ++calls;
    if (outcome == 2) return 0; /* Deliberately broken handler: must not publish null. */
    assert(!owned);
    ++owned;
    *object = 0x222;
    if (outcome == 3) c->state = SNX_CTR_IPC_UNSUPPORTED;
    if (outcome == 4) snx_ctr_ipc_bad(c);
    return outcome == 1 ? 0x12345678 : 0;
}
int main(void) {
    SnxCtrIpc c;
    unsigned i;
    memset(&c, 0, sizeof c);
    c.caller_pid = 0x10;
    c.in[0] = 0x00100142;
    c.in[1] = 0x89abcdef;
    c.in[2] = 0x76543210;
    c.in[3] = 0xfedcba98;
    c.in[4] = 0x34333231;
    c.in[5] = 0x38373635;
    c.in[6] = 0x20;
    c.in[7] = 0xdeadbeef;
    test_ctr_dispatch(&c);
    assert(calls == 1 && !c.transport_result && c.state == SNX_CTR_IPC_RETURN);
    assert(c.out[0] == 0x00100180 && !c.out[1] && c.out[2] == c.in[2] && c.out[3] == c.in[3]);
    assert(c.out[4] == c.in[4] && c.out[5] == c.in[5] && c.out[6] == 0x10);
    calls = 0;
    c.in[6] = 0x21;
    test_ctr_dispatch(&c);
    assert(!calls && c.transport_result == SNX_CTR_IPC_BAD_DESCRIPTOR);
    for (i = 0; i < 5; ++i) {
        memset(&c, 0, sizeof c);
        c.in[0] = 0x00110040;
        c.in[1] = i;
        test_ctr_dispatch(&c);
        if (!i) {
            assert(c.out[0] == 0x00110042 && !c.out[1] && !c.out[2] && c.out[3] == 0x222 && owned == 1);
            snx_ctr_ipc_drop_handle(&c, c.out[3]);
        } else if (i == 1)
            assert(c.out[0] == 0x00110040 && c.out[1] == 0x12345678 && !owned);
        else if (i == 4)
            assert(!owned && c.transport_result == SNX_CTR_IPC_BAD_DESCRIPTOR && !c.out[0]);
        else
            assert(!owned && c.state == (i == 2 ? SNX_CTR_IPC_FAULT : SNX_CTR_IPC_UNSUPPORTED));
    }
    memset(&c, 0, sizeof c);
    c.in[0] = 0xbeef0000;
    calls = 0;
    test_ctr_dispatch(&c);
    assert(!calls && !owned && c.state == SNX_CTR_IPC_UNKNOWN && !c.out[0]); /* no such command */
    puts("CTR generated wire ordering, 32-bit caller PID, replies and owned-output rollback passed (ISO C fixture)");
    return 0;
}
