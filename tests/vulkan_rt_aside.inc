/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A render target whose pixels exist only on the GPU, where the caches would let it go (surface.c rt_give_way): for its
   age (VK_RECLAIM_IDLE, every 600 presents) or under the budget's pressure (VK_RECLAIM_ANY). Here a parked one, as a
   target is once another shape took its address, which nothing kept from its age. It is published as it is parked
   (rt_park_publish), so its pixels are in memory and it goes for its age and comes back from there, unless memory
   can't hold them: a format that is no format of guest memory's (ASIDE_HELD), drawn as RGBA8, is the one that still
   has pixels memory lacks when it is parked. Its pixels: a green picture with a rectangle cleared red over it.
   - Where the GPU shares the system's memory nothing moves: its age leaves it, bit for bit when it holds its address
     again, and the pressure takes it.
   - Where the GPU has memory of its own its image moves into system memory, the pressure leaves it there, and once it
     is used again it goes back into the GPU's memory, bit for bit.
   - Memory the guest wrote while it was set aside, by the CPU or an engine, is newer than it at its next use: it counts
     as loaded again and, as such a format loads nothing, its pixels are undefined. One of a format memory holds, let go
     for its age, is made again at its next use and loads what the guest wrote.
   Lavapipe is of the first kind; the second is had there by taking the device-local flag off its one memory type while
   the target moves out, so that system memory finds a type. A discrete GPU is of the second kind as it is, and of the
   first by its memory model */
#define ASIDE_CPU   (HEAP_BASE + 0x3d000000)
#define ASIDE_GA    0xa90000000ull
#define ASIDE_W     320u
#define ASIDE_H     180u
#define ASIDE_BYTES (ASIDE_W * ASIDE_H * 4u)
#define ASIDE_SPAN  0x50000u /* its guest bytes: 20 GOBs across, 2 blocks of 16 GOBs down */
#define ASIDE_RAW   0xD5u    /* A8B8G8R8: guest memory holds its texels as they are */
#define ASIDE_HELD  0xE1u    /* no format of the table (format.c): drawn as RGBA8, and memory holds none of it */
typedef struct {
    Surf** s;
    u32 n, cap;
    int failed; /* no memory for the list */
} AsideList;
static void aside_collect(void* ctx, const Surf* s) {
    AsideList* l = ctx;
    if (l->failed) return;
    if (l->n == l->cap) {
        Surf** more = realloc(l->s, (l->cap ? 2 * l->cap : 64) * sizeof *l->s);
        if (!more) {
            l->failed = 1;
            return;
        }
        l->s = more;
        l->cap = l->cap ? 2 * l->cap : 64;
    }
    l->s[l->n++] = (Surf*)s;
}
/* t made the least recently used render target, every other one used in a present after its own: the present before
   which the caches' walk takes t alone */
static u32 aside_oldest(Surf* t) {
    AsideList l = {0};
    vk_surf_each(VK_LRU_RT, aside_collect, &l);
    if (l.failed) {
        fprintf(stderr, "render targets set aside: no memory for a list of %u targets\n", l.n);
        abort();
    }
    g_vk_present++;
    vk_surf_touch(t);
    g_vk_present++;
    for (u32 i = 0; i < l.n; i++)
        if (l.s[i] != t) vk_surf_touch(l.s[i]);
    free(l.s);
    return g_vk_present;
}
static u32 aside_targets(void) {
    u32 n, parked;
    u64 bytes;
    vk_surf_stats(VK_LRU_RT, &n, &bytes, &parked);
    return n;
}
/* every pixel of t, a 320 by 180 target, as its image holds them */
static int aside_read(Surf* t, u8* out) {
    VkBuffer buffer;
    vk_render_end();
    u8* data = vk_readback_buffer(ASIDE_BYTES, &buffer);
    CHECK(data);
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, t, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {ASIDE_W, ASIDE_H, 1}};
    rec_CmdCopyImageToBuffer(cb, t->img, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), buffer, 1, &copy);
    vk_finish();
    vk_readback_ready();
    memcpy(out, data, ASIDE_BYTES);
    return 0;
}
/* the 320 by 180 block linear target of format fmt at gva (blocks 16 GOBs high), made from the guest's bytes there,
   then, one memory holds nothing of, all of it cleared green, and a rectangle of it cleared red: the target, held with
   vk_lock */
static Surf* aside_draw(GpuChannel* ch, u64 gva, u32 fmt) {
    u32* r = ch->r3d;
    const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1};
    r[0x487] = 1;
    r[0x200] = (u32)(gva >> 32);
    r[0x201] = (u32)gva;
    r[0x202] = ASIDE_W;
    r[0x203] = ASIDE_H;
    r[0x204] = fmt;
    r[0x205] = 4u << 4;
    r[0x206] = 1;
    r[0x207] = 0;
    if (fmt == ASIDE_HELD) {
        memcpy(&r[0x360], green, sizeof green);
        engine_clear(ch, 15 << 2);
    }
    r[0x43E] |= 0x100;
    r[0x380] = 1;
    r[0x381] = 40 | 200u << 16;
    r[0x382] = 20 | 100u << 16;
    memcpy(&r[0x360], red, sizeof red);
    engine_clear(ch, 15 << 2);
    r[0x43E] &= ~0x100u;
    r[0x380] = 0;
    vk_lock();
    vk_render_end();
    return vk_rt_find(gva);
}
/* the target of another shape at gva takes the address, parking t; or t takes it again, the other one parked */
static Surf* aside_park(u64 gva) { return vk_rt(gva, 160, 90, 0xD5, 4u << 4, 0, 0); }
static Surf* aside_use(u64 gva, u32 fmt) { return vk_rt(gva, ASIDE_W, ASIDE_H, fmt, 4u << 4, 0, 0); }
/* the guest bytes of a target: none the same as the one before */
static void aside_bytes(u64 cpu) {
    for (u32 i = 0; i < ASIDE_SPAN; i++) GP(cpu)[i] = (u8)(i * 37 + 11);
}
static int aside_expect(Surf* t, const u8* want, u8* got, const char* what) {
    CHECK(!aside_read(t, got));
    if (!memcmp(got, want, ASIDE_BYTES)) return 0;
    u32 i = 0;
    while (got[i] == want[i]) i++;
    fprintf(stderr, "render target set aside, %s: byte %u of its pixels is %02x, not %02x\n", what, i, got[i], want[i]);
    return 1;
}
static int check_rt_aside(void) {
    const int model = g_vk.mem_model;
    u8 *ref = malloc(ASIDE_BYTES), *got = malloc(ASIDE_BYTES), *flat = malloc(ASIDE_BYTES);
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ref && got && flat && ch);
    CHECK(mem_map(ASIDE_CPU, 2 * 0x80000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(ASIDE_GA, ASIDE_CPU, 2 * 0x80000);
    VkRtSysStats was, now;

    /* the GPU shares the system's memory */
    aside_bytes(ASIDE_CPU);
    g_renderer->mem_may_have_changed();
    Surf* t = aside_draw(ch, ASIDE_GA, ASIDE_HELD);
    CHECK(t && aside_park(ASIDE_GA) != t && (t->gva >> 63));
    CHECK(!aside_read(t, ref));
    static const u8 red[4] = {255, 0, 0, 255};
    static const u8 green[4] = {0, 255, 0, 255};
    CHECK(!memcmp(ref + (u64)(60 * ASIDE_W + 100) * 4, red, 4) && !memcmp(ref, green, 4));
    CHECK(!vk_surf_flush_size(t)); /* memory can't hold its pixels, so its parking published nothing */
    g_vk.mem_model = VK_MEM_UMA;
    u32 before = aside_oldest(t), held = aside_targets();
    vk_rt_sys_stats(&was);
    vk_surf_reclaim(VK_LRU_RT, before, ~0ull, VK_RECLAIM_IDLE);
    vk_rt_sys_stats(&now);
    g_vk.mem_model = model;
    if (aside_targets() != held || !t->aside || t->demoted || now.kept != was.kept + 1 || now.out != was.out) {
        fprintf(stderr,
                "a parked target with pixels memory lacks, unused for its age, where the GPU shares the system's "
                "memory: %u targets held after the age's walk, not %u; set aside %d, in system memory %d\n",
                aside_targets(), held, aside_targets() == held && t->aside, aside_targets() == held && t->demoted);
        return 1;
    }
    if (aside_expect(t, ref, got, "kept for its age")) return 1;
    CHECK(aside_use(ASIDE_GA, ASIDE_HELD) == t && !t->aside);
    vk_rt_sys_stats(&now);
    CHECK(now.reloads == was.reloads);
    if (aside_expect(t, ref, got, "kept for its age, used again")) return 1;
    /* the CPU writes its memory while it is set aside: its next use finds that newer than its pixels, which are
       undefined then, as its format loads nothing */
    vk_unlock();
    CHECK(aside_draw(ch, ASIDE_GA, ASIDE_HELD) == t && aside_park(ASIDE_GA) != t);
    g_vk.mem_model = VK_MEM_UMA;
    vk_surf_reclaim(VK_LRU_RT, aside_oldest(t), ~0ull, VK_RECLAIM_IDLE);
    g_vk.mem_model = model;
    CHECK(t->aside);
    vk_unlock();
    memset(GP(ASIDE_CPU), 0xA5, ASIDE_SPAN);
    g_renderer->mem_may_have_changed(); /* as the runtime says at the guest's next submission */
    vk_lock();
    CHECK(aside_use(ASIDE_GA, ASIDE_HELD) == t);
    vk_rt_sys_stats(&now);
    if (now.reloads != was.reloads + 1 || t->layout != VK_IMAGE_LAYOUT_UNDEFINED) {
        fprintf(stderr,
                "a target set aside whose memory the CPU wrote meanwhile: %llu loaded again at their next use, not "
                "%llu, its pixels defined %d\n",
                (unsigned long long)now.reloads, (unsigned long long)(was.reloads + 1),
                t->layout != VK_IMAGE_LAYOUT_UNDEFINED);
        return 1;
    }
    /* the pressure takes it */
    vk_unlock();
    CHECK(aside_draw(ch, ASIDE_GA, ASIDE_HELD) == t && aside_park(ASIDE_GA) != t);
    g_vk.mem_model = VK_MEM_UMA;
    before = aside_oldest(t);
    held = aside_targets();
    vk_rt_sys_stats(&was);
    vk_surf_reclaim(VK_LRU_RT, before, 1, VK_RECLAIM_ANY);
    vk_rt_sys_stats(&now);
    g_vk.mem_model = model;
    CHECK(aside_targets() == held - 1 && now.lost == was.lost + 1);
    vk_unlock();

    /* a target memory holds, parked as a parked one comes back: it keeps its pixels (surface.c rt_park_publish), and
       its age publishes them and lets it go (rt_give_way), and what the CPU wrote meanwhile is what the target made again
       at its next use holds */
    for (int wrote = 0; wrote < 2; wrote++) {
        aside_bytes(ASIDE_CPU);
        g_renderer->mem_may_have_changed();
        t = aside_draw(ch, ASIDE_GA, ASIDE_RAW);
        CHECK(t && aside_park(ASIDE_GA) != t && (t->gva >> 63) && vk_surf_flush_size(t));
        CHECK(!aside_read(t, ref));
        g_vk.mem_model = VK_MEM_UMA;
        before = aside_oldest(t);
        held = aside_targets();
        vk_rt_sys_stats(&was);
        vk_surf_reclaim(VK_LRU_RT, before, ~0ull, VK_RECLAIM_IDLE);
        vk_rt_sys_stats(&now);
        g_vk.mem_model = model;
        if (aside_targets() != held - 1 || now.kept != was.kept || now.lost != was.lost) {
            fprintf(stderr,
                    "a parked target whose pixels memory has: %u targets held after the age's walk, not %u; set aside "
                    "%llu, gone with its pixels %llu\n",
                    aside_targets(), held - 1, (unsigned long long)(now.kept - was.kept),
                    (unsigned long long)(now.lost - was.lost));
            return 1;
        }
        vk_unlock();
        if (wrote) memset(GP(ASIDE_CPU), 0xA5, ASIDE_SPAN);
        g_renderer->mem_may_have_changed();
        vk_lock();
        Surf* again = aside_use(ASIDE_GA, ASIDE_RAW);
        CHECK(again);
        memset(flat, 0xA5, ASIDE_BYTES);
        if (aside_expect(again, wrote ? flat : ref, got, wrote ? "let go, the CPU wrote its memory" : "let go"))
            return 1;
        vk_unlock();
    }

    /* the GPU has memory of its own */
    const u64 cpu = ASIDE_CPU + 0x80000, gva = ASIDE_GA + 0x80000;
    aside_bytes(cpu);
    g_renderer->mem_may_have_changed();
    t = aside_draw(ch, gva, ASIDE_HELD);
    CHECK(t && aside_park(gva) != t && (t->gva >> 63));
    CHECK(!aside_read(t, ref));
    const u64 device_bytes = t->bytes;
    VkPhysicalDeviceMemoryProperties* mp = (VkPhysicalDeviceMemoryProperties*)vk_devmem_props();
    int sys_type = 0, own_types = 1, round = 0;
    for (u32 k = 0; k < mp->memoryTypeCount; k++)
        if (!(mp->memoryTypes[k].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) own_types = 0;
    for (;; round++) {
        before = aside_oldest(t);
        held = aside_targets();
        vk_rt_sys_stats(&was);
        if (model == VK_MEM_UMA) g_vk.mem_model = VK_MEM_SMALL_BAR;
        for (u32 k = 0; own_types && k < mp->memoryTypeCount; k++)
            mp->memoryTypes[k].propertyFlags &= ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        vk_surf_reclaim(VK_LRU_RT, before, ~0ull, VK_RECLAIM_IDLE);
        DevMemInfo info;
        vk_devmem_info(t->mem, &info);
        sys_type = !(mp->memoryTypes[info.type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        for (u32 k = 0; own_types && k < mp->memoryTypeCount; k++)
            mp->memoryTypes[k].propertyFlags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        g_vk.mem_model = model;
        vk_rt_sys_stats(&now);
        if (aside_targets() != held || !t->demoted || t->bytes || !t->sys_bytes || !sys_type ||
            now.out != was.out + 1 || now.n != was.n + 1) {
            fprintf(stderr,
                    "a parked target with pixels memory lacks, unused for its age, where the GPU has memory of its "
                    "own: %u targets held after the age's walk, not %u; in system memory %d (%llu bytes of the GPU's "
                    "left, %llu of the system's), a memory type of the system's %d\n",
                    aside_targets(), held, aside_targets() == held && t->demoted,
                    (unsigned long long)(aside_targets() == held ? t->bytes : 0),
                    (unsigned long long)(aside_targets() == held ? t->sys_bytes : 0), sys_type);
            return 1;
        }
        if (round) break;
        if (aside_expect(t, ref, got, "in system memory")) return 1;
        /* the pressure leaves it there: it frees none of the GPU's memory */
        before = aside_oldest(t);
        vk_surf_reclaim(VK_LRU_RT, before, 1, VK_RECLAIM_ANY);
        vk_rt_sys_stats(&now);
        CHECK(aside_targets() == held && t->demoted && now.lost == was.lost);
        /* used again: back into the GPU's memory at the present's end */
        CHECK(aside_use(gva, ASIDE_HELD) == t && !t->aside);
        if (aside_expect(t, ref, got, "in system memory, used again")) return 1;
        vk_rt_sys_frame();
        vk_rt_sys_stats(&now);
        if (t->demoted || t->bytes != device_bytes || now.back != was.back + 1 || now.n != was.n) {
            fprintf(stderr,
                    "a target in system memory used again: still there %d, %llu bytes of the GPU's, not %llu; %llu "
                    "moved back, not %llu\n",
                    t->demoted, (unsigned long long)t->bytes, (unsigned long long)device_bytes,
                    (unsigned long long)now.back, (unsigned long long)(was.back + 1));
            return 1;
        }
        if (aside_expect(t, ref, got, "back in the GPU's memory")) return 1;
        /* again, and an engine writes its memory while it is in system memory */
        vk_unlock();
        CHECK(aside_draw(ch, gva, ASIDE_HELD) == t && aside_park(gva) != t);
    }
    vk_unlock();
    memset(GP(cpu), 0x5A, ASIDE_SPAN);
    gpu_mem_written(gva, ASIDE_SPAN);
    g_renderer->mem_may_have_changed();
    vk_lock();
    vk_rt_sys_stats(&was);
    CHECK(aside_use(gva, ASIDE_HELD) == t);
    vk_rt_sys_stats(&now);
    vk_rt_sys_frame();
    if (t->demoted) {
        fprintf(stderr, "a target in system memory used again after an engine wrote its memory: still there\n");
        return 1;
    }
    if (t->layout != VK_IMAGE_LAYOUT_UNDEFINED) {
        fprintf(stderr, "a target in system memory used again after an engine wrote its memory: it kept its pixels\n");
        return 1;
    }
    vk_unlock();
    CHECK(mem_unmap(ASIDE_CPU, 2 * 0x80000) == 0);
    free(ch);
    free(ref);
    free(got);
    free(flat);
    puts("render targets set aside: kept for their age bit for bit, moved into system memory and back where the GPU "
         "has its own, taken by the pressure where it doesn't, undefined where the guest wrote their memory meanwhile, "
         "and one memory holds let go for its age and loaded from it");
    return 0;
}
