/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Which frame slot's list a deferred release goes on (gpu/vk/cmd.c's vk_defer_*), and what the release costs: it is
   performed when its slot is recycled, after every command recorded before it is done, and it begins no command
   buffer in a slot in use. A gate semaphore (vulkan_lock_waits.inc) holds the GPU's work back, so a release performed
   before that work is done shows */

/* a release's call: how often it ran, and the newest submission the GPU had done when it did */
typedef struct {
    u32 calls;
    u64 done;
} RsCall;
static void rs_freed(void* arg) {
    RsCall* call = (RsCall*)arg;
    u64 submitted;
    call->calls++;
    vk_submit_progress(&submitted, &call->done);
}
/* something to submit in the current slot */
static int rs_record(void) {
    VkBuffer upload;
    u64 at;
    CHECK(vk_stage_upload(16, 16, &upload, &at));
    return 0;
}
static void rs_said(const char* what, u32 begun, u32 begun_now, u64 last, u64 last_now, const RsCall* call) {
    fprintf(stderr,
            "release %s: %u command buffers begun and %llu submissions made by it, the release performed %u times "
            "with the GPU done up to submission %llu\n",
            what, begun_now - begun, (unsigned long long)(last_now - last), call->calls,
            (unsigned long long)call->done);
}

/* A slot in use with nothing recording (its commands just submitted) takes the release as it is: no command buffer is
   begun for it, so the present that follows submits nothing, and it is performed once that submission, held back
   here, is done */
static int check_release_in_use(void) {
    VkSemaphore gate;
    RsCall call = {0};
    vk_lock();
    vk_finish();
    CHECK(!rs_record());
    CHECK(!lw_gate(&gate));
    vk_submit_recorded(); /* the slot's last submission, held back: its landing finds nothing done */
    u64 held = vk_last_submit();
    u32 begun = vk_cb_begun(), slot = vk_frame_slot();
    vk_defer_call(rs_freed, &call);
    u32 begun_release = vk_cb_begun();
    vk_frame_next(); /* nothing recorded since the submission */
    u32 begun_present = vk_cb_begun();
    u64 last_present = vk_last_submit();
    vk_frame_next(); /* the other slot never began: this one again, fresh */
    RsCall early = call;
    CHECK(!lw_open(gate)); /* first: what holds the GPU's work back would keep the test from ending */
    if (begun_release != begun || begun_present != begun || last_present != held || early.calls)
        rs_said("in a slot in use", begun, begun_present, held, last_present, &early);
    CHECK(begun_release == begun);
    CHECK(begun_present == begun && last_present == held);
    CHECK(!early.calls);
    CHECK(vk_frame_slot() == slot);
    vk_cmd(); /* recycles the slot */
    if (call.calls != 1 || call.done < held)
        rs_said("in a slot in use, at its recycling", begun, begun, held, held, &call);
    CHECK(call.calls == 1 && call.done >= held);
    vk_finish();
    vk_unlock();
    vkDestroySemaphore(g_vk.dev, gate, NULL);
    puts("release in use: a release in a slot with nothing recording begins no command buffer and waits for the "
         "slot's last submission");
    return 0;
}

/* A slot that is recording takes the release too, and it waits for the submission of what is being recorded */
static int check_release_recording(void) {
    VkSemaphore gate;
    RsCall call = {0};
    vk_lock();
    vk_finish();
    CHECK(!rs_record());
    u32 begun = vk_cb_begun(), slot = vk_frame_slot();
    u64 held = vk_submit_number(); /* what is recording, once submitted */
    vk_defer_call(rs_freed, &call);
    u32 begun_release = vk_cb_begun();
    CHECK(!lw_gate(&gate));
    vk_frame_next(); /* submits it, held back */
    u64 last_present = vk_last_submit();
    vk_frame_next();
    RsCall early = call;
    CHECK(!lw_open(gate));
    if (begun_release != begun || last_present != held || early.calls)
        rs_said("in a slot recording", begun, begun_release, held - 1, last_present, &early);
    CHECK(begun_release == begun && last_present == held);
    CHECK(!early.calls);
    CHECK(vk_frame_slot() == slot);
    vk_cmd();
    if (call.calls != 1 || call.done < held)
        rs_said("in a slot recording, at its recycling", begun, begun, held, held, &call);
    CHECK(call.calls == 1 && call.done >= held);
    vk_finish();
    vk_unlock();
    vkDestroySemaphore(g_vk.dev, gate, NULL);
    puts("release recording: a release in a slot that records waits for the submission of what it records");
    return 0;
}

/* A fresh slot still holds its previous use's list: the release starts the slot first (its recycling, and the command
   buffer of its new use), and waits for that use's last submission */
static int check_release_fresh(void) {
    VkSemaphore gate;
    RsCall call = {0};
    vk_lock();
    vk_finish();
    vk_frame_next(); /* nothing recording: the next slot, fresh */
    u32 begun = vk_cb_begun(), slot = vk_frame_slot();
    vk_defer_call(rs_freed, &call);
    u32 begun_release = vk_cb_begun();
    u64 held = vk_submit_number();
    CHECK(!lw_gate(&gate));
    vk_frame_next();
    u64 last_present = vk_last_submit();
    vk_frame_next();
    RsCall early = call;
    CHECK(!lw_open(gate));
    if (begun_release != begun + 1 || last_present != held || early.calls)
        rs_said("in a fresh slot", begun, begun_release, held - 1, last_present, &early);
    CHECK(begun_release == begun + 1 && last_present == held);
    CHECK(!early.calls);
    CHECK(vk_frame_slot() == slot);
    vk_cmd();
    if (call.calls != 1 || call.done < held)
        rs_said("in a fresh slot, at its recycling", begun, begun, held, held, &call);
    CHECK(call.calls == 1 && call.done >= held);
    vk_finish();
    vk_unlock();
    vkDestroySemaphore(g_vk.dev, gate, NULL);
    puts("release fresh: a release in a fresh slot starts the slot and waits for its new use's last submission");
    return 0;
}

/* The landing of a present's submission releases an owner: a storage result whose submission the GPU finished after
   its own landing lands at the present, over a GPU-owned range nothing else holds, so the landing lets the owner go.
   The present costs what it costs when the owner is still held elsewhere and nothing is released: the same command
   buffers begun, the same submissions made. The release began one in the slot the present was leaving, which stayed
   open when the frames came round to it (commands past the recording thread) until the present submitted it as well */
#define RS_CPU (HEAP_BASE + 0x7f0000)
#define RS_GVA 0xad00000ull
/* A storage result over an owner's bytes, its submission done and still to land: owner holds 0x22 and is published
   over 256 bytes of the page, which the range holds alone when release is set; a draw's stores (0x44) over every byte
   of it are submitted, held back until that submission's own landing has found it not done, or with held, until the
   caller lets *held go (rs_result_go) */
static int rs_result_go(VkSemaphore gate, u64 submission) {
    CHECK(!lw_open(gate));
    vk_wait_submit(submission);
    vkDestroySemaphore(g_vk.dev, gate, NULL);
    CHECK(own_lookup(RS_CPU + 64, 256, NULL, 0) == 1); /* done, and still to land */
    return 0;
}
static int rs_result(VkOwnRes* owner, int release, VkSemaphore* held, u64* submission) {
    VkSemaphore gate;
    VkBuffer output;
    u64 at;
    memset(GP(RS_CPU), 0x11, 0x1000);
    CHECK(!owned_test_fill(owner->buf, 0, 256, 0x22));
    vk_own_publish_owned(RS_CPU + 64, 256, owner, 0);
    if (release) vk_own_res_unref(owner);
    /* not whole pages: a binding of whole pages is kept on the GPU, where nothing lands at its submission's end */
    CHECK(vk_buf_copy_writable(RS_GVA + 32, 0x200, &output, &at, 1));
    vk_buf_writable_commit();
    CHECK(!owned_test_fill(output, at + 32, 256, 0x44));
    CHECK(!lw_gate(&gate));
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    if (submission) *submission = vk_last_submit();
    if (!held) return rs_result_go(gate, vk_last_submit());
    *held = gate;
    return 0;
}
/* the result landed: the range is the CPU's again, with the stores over the owner's bytes */
static int rs_landed(void) {
    CHECK(own_lookup(RS_CPU + 64, 256, NULL, 0) == 0);
    for (u32 i = 0; i < 0x1000; i++) CHECK(GP(RS_CPU)[i] == (i >= 64 && i < 320 ? 0x44 : 0x11));
    return 0;
}

/* The landing of a present's submission releases an owner: a storage result whose submission the GPU finished after
   its own landing lands at the present, over a GPU-owned range nothing else holds, so the landing lets the owner go.
   The present costs what it costs when the owner is still held elsewhere and nothing is released: the same command
   buffers begun, the same submissions made. The release began one in the slot the present was leaving, which stayed
   open when the frames came round to it (commands past the recording thread) until the present submitted it as well */
static int rs_present(int release, u32* begun, u64* submits) {
    VkOwnRes* owner = vk_own_res_new(256);
    CHECK(owner);
    CHECK(!rs_result(owner, release, NULL, NULL));
    CHECK(!rs_record());
    u32 slot = vk_frame_slot();
    *begun = vk_cb_begun();
    *submits = vk_last_submit();
    vk_frame_next();
    *begun = vk_cb_begun() - *begun;
    *submits = vk_last_submit() - *submits;
    CHECK(vk_frame_slot() != slot);
    CHECK(!rs_landed());
    if (!release) vk_own_res_unref(owner);
    vk_finish();
    return 0;
}
static int check_release_present_landing(void) {
    u32 begun[2];
    u64 submits[2];
    if (!own_traps()) return 0;
    gmmu_map(RS_GVA, RS_CPU, 0x2000);
    vk_lock();
    vk_finish();
    for (int release = 0; release < 2; release++) CHECK(!rs_present(release, &begun[release], &submits[release]));
    vk_unlock();
    if (begun[1] != begun[0] || submits[1] != submits[0])
        fprintf(stderr,
                "release in a present's landing: the present began %u command buffers and made %llu submissions, where "
                "the same present with nothing to release begins %u and makes %llu\n",
                begun[1], (unsigned long long)submits[1], begun[0], (unsigned long long)submits[0]);
    CHECK(begun[1] == begun[0] && submits[1] == submits[0]);
    puts("release in a present's landing: an owner released there begins no command buffer and adds no submission");
    return 0;
}
