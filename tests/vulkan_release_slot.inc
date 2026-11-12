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

/* When an owner's release is performed, told by its buffer: performed, the buffer is back in the owners' pool, which
   hands a new owner the smallest buffer of at least its size. The owner here has a size no other check uses, in a
   buffer made for it, and what the pool held of that size or more is kept out of it until the check ends */
typedef struct {
    VkOwnRes* held[640];
    u32 n;
} RsHeld;
static VkOwnRes* rs_owner(RsHeld* h, u64 size) {
    for (;;) {
        VkOwnRes* r = vk_own_res_new(size);
        if (!r || r->size == size) return r;
        if (h->n == sizeof h->held / sizeof h->held[0]) return NULL;
        h->held[h->n++] = r;
    }
}
static int rs_pooled(RsHeld* h, VkBuffer buf, u64 size) {
    VkOwnRes* r = vk_own_res_new(size);
    int pooled = r && r->buf == buf;
    if (r && h->n < sizeof h->held / sizeof h->held[0]) h->held[h->n++] = r;
    return pooled;
}
static void rs_done(RsHeld* h) {
    for (u32 i = 0; i < h->n; i++) vk_own_res_unref(h->held[i]);
    h->n = 0;
}
/* an owner lands through a queue of its own, so its landing runs with a submission of the graphics queue held back:
   on one queue its copy waits behind that submission */
static int rs_owner_lands_apart(void) { return g_vk.compute && g_vk.compute != g_vk.gfx; }

/* A slot's recycling lands its previous use's results itself, and that landing releases an owner with the slot still
   fresh. The slot is not recycled a second time inside its own recycling, and the release goes on the list of the
   slot that made the newest submission: the other one's, held back here where the owner's landing does not wait
   behind it (rs_owner_lands_apart). It went on the list being freed, and was performed at once, with that submission
   still to run */
static int check_release_recycle_landing(void) {
    RsHeld held = {0};
    const u64 size = 0x3250;
    VkSemaphore gate = VK_NULL_HANDLE, result;
    int apart = rs_owner_lands_apart();
    u64 first, newest;
    if (!own_traps()) return 0;
    gmmu_map(RS_GVA, RS_CPU, 0x2000);
    vk_lock();
    vk_finish();
    VkOwnRes* owner = rs_owner(&held, size);
    CHECK(owner);
    VkBuffer buf = owner->buf;
    CHECK(!rs_result(owner, 1, &result, &first)); /* held back past the other slot's present, which would land it */
    vk_frame_next();                              /* nothing recording: the other slot */
    CHECK(!rs_record());
    if (apart) CHECK(!lw_gate(&gate));
    vk_frame_next(); /* its submission, the newest, held back; the first slot again, fresh, its result still to land */
    newest = vk_last_submit();
    CHECK(!rs_result_go(result, first));
    u32 recycles = vk_frame_recycles();
    vk_cmd(); /* recycles the slot: its landing releases the owner */
    recycles = vk_frame_recycles() - recycles;
    int early = rs_pooled(&held, buf, size);
    if (apart) CHECK(!lw_open(gate));
    if (recycles != 1 || early)
        fprintf(stderr,
                "release in a recycling's landing: %u recyclings where one slot is recycled once, the release %s "
                "before submission %llu was done\n",
                recycles, early ? "performed" : "not performed", (unsigned long long)newest);
    CHECK(recycles == 1);
    CHECK(!early);
    CHECK(!rs_landed());
    vk_frame_next();
    vk_cmd(); /* the other slot recycled, and this one's list freed before it: every slot's releases are performed */
    vk_frame_next();
    vk_cmd();
    CHECK(rs_pooled(&held, buf, size));
    vk_finish();
    rs_done(&held);
    vk_unlock();
    if (apart) vkDestroySemaphore(g_vk.dev, gate, NULL);
    puts("release in a recycling's landing: the slot is recycled once and the release waits for the newest submission");
    return 0;
}

/* A landing asked for with the current slot fresh (a release's gate, a read of guest memory under results) releases an
   owner: the slot is not recycled inside the landing, whose results may be in the rings a recycling hands back, and
   the release is performed once the slots are recycled */
static int check_release_landing_fresh(void) {
    RsHeld held = {0};
    const u64 size = 0x3350;
    if (!own_traps()) return 0;
    gmmu_map(RS_GVA, RS_CPU, 0x2000);
    vk_lock();
    vk_finish();
    VkOwnRes* owner = rs_owner(&held, size);
    CHECK(owner);
    VkBuffer buf = owner->buf;
    u64 result;
    CHECK(!rs_result(owner, 1, NULL, &result));
    vk_frame_next(); /* nothing recording: the other slot, fresh */
    u32 recycles = vk_frame_recycles(), slot = vk_frame_slot();
    vk_land_through(result);
    recycles = vk_frame_recycles() - recycles;
    int early = rs_pooled(&held, buf, size);
    if (recycles || early)
        fprintf(stderr,
                "release in a landing with the slot fresh: %u slots recycled inside the landing, where none is, the "
                "release %s inside it\n",
                recycles, early ? "performed" : "not performed");
    CHECK(!recycles && vk_frame_slot() == slot);
    CHECK(!early);
    CHECK(!rs_landed());
    for (int i = 0; i < VK_FRAMES; i++) {
        vk_cmd();
        vk_frame_next();
    }
    vk_cmd();
    CHECK(rs_pooled(&held, buf, size));
    vk_finish();
    rs_done(&held);
    vk_unlock();
    puts("release in a landing with the slot fresh: the landing recycles no slot");
    return 0;
}

/* A landing that records: a storage result lands over the bytes of a render target whose pixels memory has not got,
   and they are written back first (commands recorded, submitted and waited for inside the landing). With the slot
   fresh, inside its own recycling or in a landing asked for before the frame began, they go after the slot's previous
   use, and the slot is recycled once, when its frame begins */
static int check_landing_records_fresh(void) {
    const u64 cpu = RS_CPU + 0x1000, gva = RS_GVA + 0x1000;
    static const float color[2][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}};
    if (vk_uma()) { /* the release checks above record in their landings there: an owner lands on the one queue */
        puts("landing that records: not run on unified memory, where a storage result is written in place");
        return 0;
    }
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    u32* r = ch->r3d;
    gmmu_map(RS_GVA, RS_CPU, 0x2000);
    r[0x487] = 1;
    r[0x201] = (u32)gva;
    r[0x202] = 64; /* 16 RGBA pixels, pitch linear */
    r[0x203] = 16;
    r[0x204] = 0xD5; /* A8B8G8R8 */
    r[0x205] = 0x1000;
    r[0x206] = 1;
    for (int asked = 0; asked < 2; asked++) { /* 0: in the slot's recycling; 1: in a landing asked for */
        const char* where = asked ? "with the slot fresh" : "in a slot's recycling";
        VkSemaphore gate;
        VkBuffer output;
        u64 at;
        vk_lock();
        vk_finish();
        memset(GP(cpu), 0, 0x1000);
        memcpy(&r[0x360], color[asked], sizeof color[asked]);
        engine_clear(ch, 15 << 2);
        vk_render_end();
        Surf* target = vk_rt_find(gva);
        u64 pixels = vk_surf_flush_size(target);
        CHECK(target && pixels && pixels < 0x800);
        /* the target's bytes and more, not whole pages: the binding takes the pixels on the GPU, the target keeps
           them, and the result lands at its submission's end */
        CHECK(vk_buf_copy_writable(gva, 0x800, &output, &at, 1));
        vk_buf_writable_commit();
        CHECK(vk_surf_flush_size(target) == pixels);
        CHECK(!owned_test_fill(output, at, 0x800, 0x44));
        CHECK(!lw_gate(&gate));
        vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE); /* held back until its own landing has found it not done */
        u64 result = vk_last_submit();
        CHECK(!lw_open(gate));
        vk_wait_submit(result);
        vkDestroySemaphore(g_vk.dev, gate, NULL);
        vk_frame_next();             /* nothing recording: the other slot */
        if (!asked) vk_frame_next(); /* and the result's slot again, fresh */
        CHECK(vk_surf_flush_size(target) == pixels && vk_last_submit() == result);
        u32 recycles = vk_frame_recycles(), expected = asked ? 0 : 1;
        if (asked)
            vk_land_through(result);
        else
            vk_cmd();
        recycles = vk_frame_recycles() - recycles;
        u64 submits = vk_last_submit() - result, left = vk_surf_flush_size(target);
        if (recycles != expected || !submits || left)
            fprintf(stderr,
                    "a landing that records %s: %u slots recycled where %u are, %llu submissions made inside it and "
                    "%llu bytes of the target left to write back\n",
                    where, recycles, expected, (unsigned long long)submits, (unsigned long long)left);
        CHECK(submits && !left); /* the write-back ran inside the landing */
        CHECK(recycles == expected);
        if (asked) {
            recycles = vk_frame_recycles();
            vk_cmd(); /* the frame begins: the slot's one recycling */
            CHECK(vk_frame_recycles() == recycles + 1);
        }
        vk_finish();
        vk_unlock();
        /* the pixels written back first: the result's bytes go over the bytes they left as the result found them */
        for (u32 i = 0; i < 0x1000; i++) {
            u8 want = i >= 0x800 ? 0 : i < pixels && (i % 4 == 3 || i % 4 == (u32)asked) ? 255 : 0x44;
            if (GP(cpu)[i] != want) {
                fprintf(stderr, "a landing that records %s: byte %#x of the page is %02x, expected %02x\n", where, i,
                        GP(cpu)[i], want);
                return 1;
            }
        }
    }
    free(ch);
    puts("landing that records: with the slot fresh, its commands go after the slot's previous use");
    return 0;
}
