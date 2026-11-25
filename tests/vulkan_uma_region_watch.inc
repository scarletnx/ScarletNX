/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */

/* The watch of a render target over a dma-buf region (own_watch_in_place, item 8 of docs/dev/draw-cost.md), and what
   comes after it: the pages it let go of and the write tracking, and the publishes over a target it is on.
   vulkan_uma_region_reads.inc has the watch itself. No extracted shaders or game data are used */
#if !defined(_WIN32)
#include <sys/resource.h>
#include <unistd.h>
#define URW_SPAN (20ull * 2 * 512 * 16) /* rt_clear_rect's target: 20 GOBs across, 2 blocks of 16 GOBs down */
/* a window of 2 MiB with a region over it, and the channel its targets are drawn through. region 0: this device has
   none there, and the window is unmapped */
typedef struct {
    GpuChannel* ch;
    u64 gva, cpu;
    int region;
} UrwWindow;
static int urw_close(UrwWindow* w) {
    vk_lock();
    vk_rt_before_write(w->gva, 0x200000, 0); /* the targets go back to memory before it does */
    vk_unlock();
    CHECK(mem_unmap(w->cpu, 0x200000) == 0);
    g_renderer->mem_may_have_changed();
    free(w->ch);
    return 0;
}
static int urw_open(UrwWindow* w, u64 at) {
    VkBuffer r;
    u64 ro;
    *w = (UrwWindow){.gva = URR_GVA + at, .cpu = URR_CPU + at};
    if (!vk_uma() || !own_traps()) {
        printf("skip: the watch over a region needs unified memory and traps\n");
        return 0;
    }
    w->ch = calloc(1, sizeof *w->ch);
    CHECK(w->ch);
    CHECK(mem_map(w->cpu, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(w->gva, w->cpu, 0x200000);
    memset(GP(w->cpu), 0x11, 0x200000);
    g_renderer->mem_may_have_changed();
    vk_lock();
    vk_rt_publish_drawn();                          /* the targets the checks before drew */
    CHECK(vk_buf_get_strict(w->gva, 256, &r, &ro)); /* a region over the window, also where imports are dear */
    vk_finish();
    w->region = vk_uma_region_held(w->cpu, 0x200000);
    vk_unlock();
    if (w->region) return 0;
    printf("skip: no dma-buf region holds the window on this device\n");
    return urw_close(w);
}
/* a thread woken on the first page it opens runs while the landing's thread opens the rest of the watches: they are all
   gone once that thread lets go of vk_lock */
static void urw_opened(void) {
    vk_lock();
    vk_unlock();
}
/* a submission's epoch, the window taken in at once, as by a draw that reads something there: what a scan finds written
   there from now on comes in the next epoch */
static void urw_epoch(const UrwWindow* w) {
    g_renderer->mem_may_have_changed();
    vk_lock();
    vk_ww_take(GP(w->cpu), 0x200000);
    vk_unlock();
}
/* three pixels of the target, left, upper right and lower right, as the CPU reads them in its guest memory at cpu (a
   fault where they are trapped), and as its image at gva holds them */
static const u32 g_urw_at[3][2] = {{3, 5}, {300, 5}, {300, 150}};
static void urw_memory(u64 cpu, u32 px[3]) {
    u8 p[4];
    for (int i = 0; i < 3; i++) {
        memcpy(p, GP(cpu + bl_offset(g_urw_at[i][0] * 4, g_urw_at[i][1], 0, 1280, 180, 4, 0)), 4);
        atomic_signal_fence(memory_order_seq_cst);
        px[i] = snx_ld_le32(p);
    }
}
static int urw_image(u64 gva, u32 px[3]) {
    u8 p[4];
    vk_lock();
    Surf* s = vk_rt_find(gva);
    CHECK(s);
    for (int i = 0; i < 3; i++) {
        CHECK(!read_pixel_at(s, g_urw_at[i][0], g_urw_at[i][1], p));
        px[i] = snx_ld_le32(p);
    }
    vk_unlock();
    return 0;
}

/* A target over a region drawn in three frames: blue whole, red in its left half, green in its lower right corner, the
   last two over what is there. Each frame releases it, waits for the GPU as a game does for its fence, and is read by
   the CPU. No CPU write reaches its memory: the CPU must read every frame's drawing, and the image keep it. The pages
   a watch let go of read as written to the scan, and taken for the CPU's they were newer than the target's next
   drawing, which its publish then left out, in memory and in the image */
enum { URW_EACH, URW_APART, URW_NEIGHBOUR, URW_HOLDER, URW_LETGO, URW_N };
static const char* const g_urw_how[URW_N] = {
    "read after each frame",                       /* the second release on publishes it: the CPU read it lately */
    "read after each frame, the frames far apart", /* watched at each release */
    "the target beside it read, whose watch goes on into this one's",
    "read by the thread that holds vk_lock, which the watch cannot answer",
    "its region let go at a present under the watch",
};
static int urw_frames(int how) {
    const u64 t = 0x90000, beside = 0x40000; /* beside's last page is the one before t's first */
    const float blue[4] = {0, 0, 1, 1}, red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1};
    const float* const color[3] = {blue, red, green};
    const u32 left[4] = {0, 0, 160, 180}, corner[4] = {200, 100, 320, 180};
    const u32 b = 0xffff0000u, r = 0xff0000ffu, g = 0xff00ff00u;
    const u32 want[3][3] = {{b, b, b}, {r, b, b}, {r, b, g}};
    u32 got[3][3], image[3], px[3];
    VkBuffer buf;
    u64 off;
    UrwWindow w;
    CHECK(!urw_open(&w, 0x1400000 + (u64)how * 0x200000));
    if (!w.region) return 0;
    memcpy(got, want, sizeof got); /* the frames the CPU does not read */
    for (int k = 0; k < 3; k++) {
        rt_clear_rect(w.ch, w.gva + t, color[k], k == 1 ? left : k == 2 ? corner : NULL);
        if (how == URW_NEIGHBOUR && !k) rt_clear_rect(w.ch, w.gva + beside, blue, NULL);
        vk_lock();
        vk_rt_publish_drawn(); /* the release */
        vk_finish();           /* the game's wait for it */
        if (!k) CHECK(own_watched(w.cpu + t, URW_SPAN));
        if (!k && how == URW_HOLDER) {
            urw_memory(w.cpu + t, px); /* the bytes from before the drawing: no publish can answer this thread */
            CHECK(px[0] == 0x11111111u && !own_watched(w.cpu + t, URW_SPAN));
            urw_epoch(&w); /* before the lock goes, where the target is watched again */
        }
        if (!k && how == URW_LETGO) { /* unused for longer than UMA_IDLE presents, at a present that looks for such */
            const VkRtWatchStats w0 = g_vk_rt_watch;
            g_vk_present = (g_vk_present / 60 + 12) * 60;
            vk_uma_frame();
            CHECK(!vk_uma_region_held(w.cpu, 0x200000));
            /* published for the move of its pages, which no CPU access asked for: it is not one the CPU reads lately */
            CHECK(g_vk_rt_watch.hits == w0.hits && g_vk_rt_watch.by[VK_PUB_OTHER] == w0.by[VK_PUB_OTHER] + 1);
            Surf* s = vk_rt_find(w.gva + t);
            CHECK(s && !s->cpu_asked);
        }
        vk_unlock();
        urw_epoch(&w);
        if (!k && how == URW_NEIGHBOUR) {
            CHECK(own_watched(w.cpu + t, URW_SPAN));
            urw_memory(w.cpu + beside, px);
            urw_opened();
            CHECK(px[0] == b && !own_trapped(w.cpu + t, URW_SPAN));
        }
        if (how < URW_NEIGHBOUR || (k && how > URW_NEIGHBOUR) || k == 2) urw_memory(w.cpu + t, got[k]);
        urw_opened(); /* the publish has its baseline in this epoch: the next submission of a game comes later */
        g_renderer->mem_may_have_changed();
        g_vk_present += how == URW_APART ? 1300 : 1;
        vk_lock(); /* the region stays in use */
        if (how != URW_LETGO) CHECK(vk_buf_get_strict(w.gva + 0x100000, 256, &buf, &off));
        vk_unlock();
    }
    CHECK(!urw_image(w.gva + t, image));
    const int bad = memcmp(got, want, sizeof got) != 0 || memcmp(image, want[2], sizeof image) != 0;
    if (bad)
        fprintf(stderr,
                "a target over a region, %s: frame 0 reads %08x %08x %08x, frame 1 %08x %08x %08x, frame 2 %08x %08x "
                "%08x, its image %08x %08x %08x; want %08x %08x %08x, %08x %08x %08x and %08x %08x %08x in both\n",
                g_urw_how[how], got[0][0], got[0][1], got[0][2], got[1][0], got[1][1], got[1][2], got[2][0], got[2][1],
                got[2][2], image[0], image[1], image[2], want[0][0], want[0][1], want[0][2], want[1][0], want[1][1],
                want[1][2], want[2][0], want[2][1], want[2][2]);
    CHECK(!urw_close(&w));
    if (!bad)
        printf("in place: a target over a region, %s: every frame's drawing is in memory and in its image\n",
               g_urw_how[how]);
    return bad;
}
static int check_uma_region_watch_frames(void) {
    int bad = 0;
    for (int how = 0; how < URW_N; how++) bad |= urw_frames(how);
    return bad;
}

/* What the write tracking says of a page of a region that a watch trapped and let go: written when the CPU wrote it
   before the watch, or with the access the watch answered, and not when the CPU only read it, when the watch went by
   itself, or when the GPU wrote it in place meanwhile. Each page is taken in, then written as its case has it, then
   asked in the next epoch */
enum { URW_IDLE, URW_READ, URW_GPU, URW_BEFORE, URW_WRITE, URW_WRITES_N };
static int check_uma_region_watch_writes(void) {
    static const char* const how[URW_WRITES_N] = {"let go unread", "read by the CPU", "written in place by the GPU",
                                                  "written by the CPU before the watch",
                                                  "written by the CPU under the watch"};
    static const int want[URW_WRITES_N] = {0, 0, 0, 1, 1};
    int got[URW_WRITES_N], bad = 0;
    UrwWindow w;
    CHECK(!urw_open(&w, 0x1400000 + (u64)URW_N * 0x200000));
    if (!w.region) return 0;
    vk_lock();
    const int tracked = !vk_ww_take(GP(w.cpu), 0x200000);
    const u32 since = vk_ww_now();
    vk_unlock();
    if (!tracked) {
        printf("skip: no write tracking on this host\n");
        return urw_close(&w);
    }
    /* the window's first scan is done: what a scan finds from now on was written since */
    for (int k = 0; k < URW_WRITES_N; k++) {
        const u64 page = w.cpu + 0x100000 + (u64)k * 0x4000;
        volatile u8* p = GP(page);
        if (k == URW_BEFORE) p[0x123] = 0x77;
        vk_lock();
        CHECK(vk_own_watch(page, 0x1000) == 0 && own_watched(page, 0x1000));
        if (k == URW_GPU) {
            VkOwnRes* res = uma_owner(0x1000, 0x42);
            CHECK(res && vk_uma_publish(page, 0x1000, res->buf, 0));
            vk_own_res_unref(res);
            vk_finish();
        }
        if (k == URW_IDLE || k == URW_BEFORE) own_unwatch(page, 0x1000);
        vk_unlock();
        if (k == URW_WRITE)
            p[0x123] = 0x77; /* a fault: the watch goes, then the write */
        else
            CHECK(p[0x123] == (k == URW_BEFORE ? 0x77 : k == URW_GPU ? 0x42 : 0x11));
        urw_opened();
        CHECK(!own_trapped(page, 0x1000));
    }
    g_renderer->mem_may_have_changed();
    vk_lock();
    for (int k = 0; k < URW_WRITES_N; k++)
        got[k] = vk_written_since(GP(w.cpu + 0x100000 + (u64)k * 0x4000), 0x1000, since);
    vk_unlock();
    for (int k = 0; k < URW_WRITES_N; k++)
        if (got[k] != want[k]) {
            fprintf(stderr, "a region's page under a watch, %s: written %d, want %d\n", how[k], got[k], want[k]);
            bad = 1;
        }
    /* and a write after the watch is gone is one, as on any page */
    GP(w.cpu + 0x100000)[0x234] = 0x55;
    g_renderer->mem_may_have_changed();
    vk_lock();
    CHECK(vk_written_since(GP(w.cpu + 0x100000), 0x1000, since) == 1);
    vk_unlock();
    CHECK(!urw_close(&w));
    if (!bad)
        printf("in place: a region's page that a watch let go is written when the CPU wrote it, before the watch, with "
               "the access that ended it and after, and else not\n");
    return bad;
}

/* An engine's result published in place over a target that a release watches in a region (a copy's, or a dispatch's
   storage image) is the GPU thread's, part-way through what it records: it submits nothing and waits for nothing, as
   over memory no target watches. The CPU's access to those pages, with the result not yet submitted, waits for it and
   reads it, and what is drawn there after that is in memory and in the image */
static int check_uma_region_watch_engine(void) {
    /* the one that the CPU reads last: read lately, the target would be published at the release, not watched */
    static const char* const how[3] = {"a result over other memory of the region", "a DMA copy over the target",
                                       "a result over the target"};
    const u64 t = 0x40000, from = 0x100000;
    const float blue[4] = {0, 0, 1, 1}, red[4] = {1, 0, 0, 1};
    const u32 left[4] = {0, 0, 160, 180}, b = 0xffff0000u, r = 0xff0000ffu;
    const u32 want[3] = {r, b, b};
    u32 px[3], image[3];
    int bad = 0;
    UrwWindow w;
    CHECK(!urw_open(&w, 0x1400000 + (u64)(URW_N + 1) * 0x200000));
    if (!w.region) return 0;
    for (int k = 0; k < 3; k++) {
        rt_clear_rect(w.ch, w.gva + t, blue, NULL);
        vk_lock();
        vk_rt_publish_drawn();
        CHECK(own_watched(w.cpu + t, URW_SPAN));
        const u64 sub = vk_submit_number(), waits = vk_uma_out_waits();
        if (k != 1) {
            VkOwnRes* res = uma_owner(0x1000, 0x42);
            CHECK(res);
            vk_own_publish(w.cpu + (k ? t : 0x180000), 0x1000, res, 0);
            vk_own_res_unref(res);
        } else {
            vk_unlock();
            GpuDmaOp o = {.multi = 1, .width = 256, .line_in = 256, .line_out = 256, .lines = 4};
            o.src = pitch_side(w.gva + from, 256);
            o.dst = bl_side(w.gva + t, 1280, 180, 4, 0, 0);
            CHECK(g_renderer->dma_copy(&o));
            vk_lock();
        }
        const u64 submits = vk_submit_number() - sub, waited = vk_uma_out_waits() - waits;
        vk_unlock();
        if (submits || waited) {
            fprintf(stderr,
                    "%s, the target watched in a region: %llu submissions and %llu waits for the GPU in it, "
                    "want none\n",
                    how[k], (unsigned long long)submits, (unsigned long long)waited);
            bad = 1;
        }
        urw_epoch(&w);
        if (k < 2) continue;
        u8 first[64];
        memcpy(first, GP(w.cpu + t), sizeof first); /* a fault: the result is recorded, not submitted */
        atomic_signal_fence(memory_order_seq_cst);
        urw_opened();
        CHECK(!own_trapped(w.cpu + t, URW_SPAN));
        u32 differ = 0;
        for (u32 i = 0; i < sizeof first; i++) differ += first[i] != 0x42;
        if (differ) {
            fprintf(stderr, "%s: the CPU reads %u of its 64 bytes wrong (the first %02x, want 42)\n", how[k], differ,
                    first[0]);
            bad = 1;
        }
        /* the target drawn over it, in another submission: the result's page is the target's again */
        g_renderer->mem_may_have_changed();
        rt_clear_rect(w.ch, w.gva + t, red, left);
        vk_lock();
        vk_rt_publish_drawn();
        vk_finish();
        vk_unlock();
        urw_epoch(&w);
        urw_memory(w.cpu + t, px);
        CHECK(!urw_image(w.gva + t, image));
        if (memcmp(px, want, sizeof px) || memcmp(image, want, sizeof image)) {
            fprintf(
                stderr,
                "a target drawn red on its left after a result over its first page: memory holds %08x %08x %08x, the "
                "image %08x %08x %08x, want %08x %08x %08x in both\n",
                px[0], px[1], px[2], image[0], image[1], image[2], want[0], want[1], want[2]);
            bad = 1;
        }
    }
    CHECK(!urw_close(&w));
    if (!bad)
        printf(
            "in place: an engine's result over a watched target waits for nothing, and the CPU's access waits for it\n");
    return bad;
}
/* The table of imports full (vk_uma_table_max: three, the oldest under a watch): the new range takes the place of the
   oldest one whose pages no watch traps. The oldest of all, under a watch, let go of no room, and the new range was
   copied for as long. All of them under watches: nothing goes, and the new range is copied */
static int check_uma_region_table(void) {
    const float blue[4] = {0, 0, 1, 1};
    const u64 t = 0x40000;
    UrwWindow w[4];
    VkBuffer r;
    u64 ro;
    int held[4];
    for (int k = 0; k < 4; k++) {
        CHECK(!urw_open(&w[k], 0x1400000 + (u64)k * 0x200000));
        if (w[k].region) continue;
        while (k-- > 0) CHECK(!urw_close(&w[k]));
        return 0;
    }
    vk_lock();
    vk_uma_forget_all(); /* the checks before it; the windows are mapped, and none is imported */
    vk_unlock();
    for (int all = 0; all < 2; all++) {
        g_vk_present += 2;
        for (int k = 0; k < 3; k++) { /* w[0] is used first: the least recently used */
            const int watched = !k || all;
            vk_lock();
            CHECK(vk_buf_get_strict(w[k].gva, 256, &r, &ro) && vk_uma_region_held(w[k].cpu, 0x200000));
            vk_unlock();
            if (watched) rt_clear_rect(w[k].ch, w[k].gva + t, blue, NULL);
            vk_lock();
            vk_rt_publish_drawn();
            CHECK(!!own_watched(w[k].cpu + t, URW_SPAN) == watched);
            g_vk_present++;
            vk_unlock();
        }
        vk_lock();
        vk_uma_table_max(3);
        CHECK(vk_buf_get_strict(w[3].gva, 256, &r, &ro)); /* one lookup lets one go, the next makes the new one */
        CHECK(vk_buf_get_strict(w[3].gva, 256, &r, &ro));
        for (int k = 0; k < 4; k++) held[k] = vk_uma_region_held(w[k].cpu, 0x200000);
        vk_uma_table_max(0);
        vk_unlock();
        if (!held[0] || held[1] != all || !held[2] || held[3] == all) {
            fprintf(stderr,
                    "the table of imports full, %s under a watch: held the first window's region %d (want 1), the "
                    "second's %d (want %d), the third's %d (want 1), the new range's %d (want %d)\n",
                    all ? "all" : "the oldest", held[0], held[1], all, held[2], held[3], !all);
            return 1;
        }
        vk_lock();
        for (int k = 0; k < 3; k++) {
            vk_rt_before_write(w[k].gva, 0x200000, 0);
            own_unwatch(w[k].cpu, 0x200000);
        }
        vk_uma_forget_all();
        vk_unlock();
    }
    for (int k = 0; k < 4; k++) CHECK(!urw_close(&w[k]));
    puts("in place: a full table of imports lets go of the oldest one that no watch traps, and of none when all are");
    return 0;
}
/* no room in this process's address space for a copy of 1 MiB or more, so a region does not move back
   (plat_vm_unshare): 0 limits it, 1 puts back what was there */
static struct rlimit g_urw_as;
static int urw_no_room(int back) {
    if (back) return setrlimit(RLIMIT_AS, &g_urw_as);
    FILE* f = fopen("/proc/self/statm", "r");
    unsigned long long pages = 0;
    CHECK(f && fscanf(f, "%llu", &pages) == 1 && !fclose(f) && !getrlimit(RLIMIT_AS, &g_urw_as));
    const struct rlimit r = {(rlim_t)(pages * (unsigned long long)sysconf(_SC_PAGESIZE) + (1ull << 20)),
                             g_urw_as.rlim_max};
    return setrlimit(RLIMIT_AS, &r);
}
/* A region the host has no room to move back (its copy of 2 MiB fails) stays, with its pages the buffer's, which are
   read through the buffer's view after the frames that would have freed it: the import went, and the view was unmapped
   under what own.c reads. Given the room, the next idle present takes it. Where it is two runs (a page moved back, or a
   page unmapped), the first moves and the second can't: it is dead, holds the pages that did not move alone, and a
   draw's lookup does not try it again, which the next present does */
static int check_uma_region_stays(void) {
    UrwWindow w;
    VkBuffer r, r2;
    u64 ro, ro2;
    OwnRange o[4];
    u8 got[64];
    int held, still = 0;
    CHECK(!urw_open(&w, 0x1400000));
    if (!w.region) return 0;
    vk_lock();
    g_vk_present = (g_vk_present / 60 + 12) * 60;
    CHECK(!urw_no_room(0));
    vk_uma_frame(); /* idle: the let-go, which the host refuses */
    CHECK(!urw_no_room(1));
    held = vk_uma_region_held(w.cpu, 0x200000);
    vk_finish();
    for (int k = 0; k < 2 * VK_FRAMES; k++) { /* what a dropped import's buffer waits for */
        vk_frame_next();
        vk_cmd();
        g_vk_present++;
        vk_uma_frame();
    }
    still = held && vk_uma_region_held(w.cpu, 0x200000); /* nothing moved: alive, and not tried until it is idle */
    if (still) {
        CHECK(vk_own_watch(w.cpu + 0x40000, 0x1000) == 0 && own_stage(w.cpu + 0x40000, sizeof got, got, o, 4) == 0);
        own_unwatch(w.cpu + 0x40000, 0x1000);
        for (u32 i = 0; i < sizeof got; i++) CHECK(got[i] == 0x11);
        g_vk_present = (g_vk_present / 60 + 12) * 60;
        vk_uma_frame();
        CHECK(!vk_uma_region_held(w.cpu, 0x200000) && GP(w.cpu + 0x40000)[0] == 0x11);
    }
    vk_unlock();
    if (!still) {
        fprintf(stderr,
                "a region the host has no room to move back: held %d (want 1) after the try, and %d (want 1) after the "
                "frames that follow\n",
                held, still);
        return 1;
    }
    for (int unmapped = 0; unmapped < 2; unmapped++) {
        vk_lock();
        CHECK(vk_buf_get_strict(w.gva, 256, &r, &ro) && vk_uma_region_held(w.cpu, 0x200000));
        if (unmapped) {
            vk_unlock();
            CHECK(!urw_no_room(0));
            CHECK(mem_unmap(w.cpu + 0x8000, 0x1000) == 0); /* the mapping's end lets a dead region go */
            CHECK(!urw_no_room(1));
            g_renderer->mem_may_have_changed();
            vk_lock();
        } else {
            CHECK(mem_unmigrate(w.cpu + 0x8000, 0x1000, NULL) == 0); /* two runs, and the region is not dead */
            g_vk_present = (g_vk_present / 60 + 12) * 60;
            CHECK(!urw_no_room(0));
            vk_uma_frame();
            CHECK(!urw_no_room(1));
        }
        const int left = vk_uma_region_held(w.cpu + 0x100000, 0x1000),
                  went = vk_uma_region_held(w.cpu + 0x1000, 0x1000), back = mem_ext_read(w.cpu + 0x1000, got, 16) != 0;
        const int bound = vk_buf_get_strict(w.gva + 0x100000, 256, &r2, &ro2), kept = bound && r2 == r;
        vk_unlock();
        if (unmapped) CHECK(mem_map(w.cpu + 0x8000, 0x1000, MS_NORMAL, PERM_RW) == 0);
        if (!left || went || !back || !kept) {
            fprintf(stderr,
                    "a region that moved back in part (%s): it holds the pages that did not move %d (want 1) and the "
                    "ones that did %d (want 0), which went back %d (want 1); a draw's lookup leaves it %d (want 1)\n",
                    unmapped ? "a page unmapped" : "two runs", left, went, back, kept);
            return 1;
        }
        vk_lock();
        vk_uma_frame();
        CHECK(!vk_uma_region_held(w.cpu + 0x100000, 0x1000) && GP(w.cpu + 0x100000)[5] == 0x11);
        vk_unlock();
    }
    CHECK(!urw_close(&w));
    puts("in place: a region the host cannot move back stays with its pages, and goes when it can");
    return 0;
}
#endif
