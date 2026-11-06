/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Queue A must not copy the old middle buffer past an event. Queue B replaces that buffer, then signals after
   the real GPU copy. The result checks ordering and GPU-owned source reuse together. */
static int check_nvn_event_order(void) {
    const u64 gpu = 0x4ec000000ull, cpu = HEAP_BASE + 0x780000;
    gmmu_map(gpu, cpu, 0x10000);
    u32 old[16], updated[16], actual[16];
    for (u32 i = 0; i < 16; i++) {
        old[i] = 0x12340000u + i;
        updated[i] = 0xabcd0000u + i;
    }
    CHECK(!gmem_write(gpu, old, sizeof old));
    CHECK(!gmem_write(gpu + 0x100, updated, sizeof updated));
    memset(GP(cpu + 0x200), 0, 0x300);
    u64 a = cpu + 0x1000, b = cpu + 0x2000;
    NvnRecHdr copy = {OP_COPY_B2B, 8 + sizeof(NvnPCopyB2B)};
    NvnPCopyB2B transfer = {gpu, gpu + 0x200, sizeof old};
    memcpy(GP(a), &copy, sizeof copy);
    memcpy(GP(a + 8), &transfer, sizeof transfer);
    NvnRecHdr wait = {OP_WAIT_EVENT, 8 + sizeof(NvnPWaitEvent)};
    NvnPWaitEvent event = {gpu + 0x400, NVN_EVENT_WAIT_MODE_EQUAL, 1};
    memcpy(GP(a + copy.size), &wait, sizeof wait);
    memcpy(GP(a + copy.size + 8), &event, sizeof event);
    NvnRecHdr last_copy = copy;
    last_copy.op |= NVN_REC_LAST;
    transfer = (NvnPCopyB2B){gpu + 0x200, gpu + 0x300, sizeof old};
    memcpy(GP(a + copy.size + wait.size), &last_copy, sizeof last_copy);
    memcpy(GP(a + copy.size + wait.size + 8), &transfer, sizeof transfer);
    transfer = (NvnPCopyB2B){gpu + 0x100, gpu + 0x200, sizeof old};
    memcpy(GP(b), &copy, sizeof copy);
    memcpy(GP(b + 8), &transfer, sizeof transfer);
    NvnRecHdr signal = {OP_SIGNAL_EVENT | NVN_REC_LAST, 8 + sizeof(NvnPSignalEvent)};
    NvnPSignalEvent value = {gpu + 0x400, NVN_EVENT_SIGNAL_MODE_WRITE, 1};
    memcpy(GP(b + copy.size), &signal, sizeof signal);
    memcpy(GP(b + copy.size + 8), &value, sizeof value);
    u32 qa = nvn_replay_queue(), qb = nvn_replay_queue(), shown = 0;
    u64 ready = 0;
    memcpy(GP(cpu + 0x800), &a, sizeof a);
    nvn_replay_submit(qa, 1, cpu + 0x800, &ready, &shown);
    memcpy(GP(cpu + 0x800), &b, sizeof b);
    nvn_replay_submit(qb, 1, cpu + 0x800, &ready, &shown);
    nvn_replay_finish(qa);
    CHECK(!gmem_read(gpu + 0x300, actual, sizeof actual));
    CHECK(!memcmp(actual, updated, sizeof actual));
    nvn_replay_queue_free(qa);
    nvn_replay_queue_free(qb);
    puts("PASS NVN event waits: another queue signals after GPU copy and dependent bytes are exact=1");
    return 0;
}
