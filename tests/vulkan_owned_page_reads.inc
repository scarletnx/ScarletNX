/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Guest memory the renderer reads with its lock held, on a page the GPU owns bytes of (a result kept on the GPU, as a
   dispatch's storage buffer is: compute.c res_publish). The CPU touching a trapped page there can't be answered and ends
   the process (memory/own.c own_fault): a read takes the owned bytes from their owner on the GPU, the CPU's on such a
   page through the pool's mirror, and lands nothing. The NVN replay reads a recording without that lock: a fault there
   lands what the GPU holds of the page. No extracted shaders or game data are used */

/* the memory of the draws' and the word's cases, mapped by check_owned_page_reads */
#define OPR_GVA 0x8da000000ull
#define OPR_CPU (HEAP_BASE + 0x11f00000)

/* A draw's constant buffer. Two buffers: one whose bound bytes hold an owner's 16, one with an owner on its page past
   the bytes the draw reads. A vertex program stores c[0][0x100] where the descriptor at c[0][0x130] points */

/* the vertex program at the code's base: c[0][0x100] stored through c[0][0x130]'s descriptor, one point at the origin */
static int cbown_program(void) {
    const u64 ops[] = {
        0x4C98078004C70006ull,       /* MOV R6, c[0x0][0x130] */
        0x4C98078004D70007ull,       /* MOV R7, c[0x0][0x134] */
        0x4C9807800407000Aull,       /* MOV R10, c[0x0][0x100] */
        0xEEDC20000007060Aull,       /* STG.E [R6], R10 */
        storage_mov(16, 0),          /* the position: x */
        storage_mov(17, 0),          /* y */
        storage_mov(18, 0),          /* z */
        storage_mov(19, 0x3f800000), /* w */
        0xEFF1FF800707FF10ull,       /* AST a[0x70], R16..R19 */
        0xE30000000007000Full,       /* EXIT */
    };
    return mesh_program(OPR_GVA, ops, sizeof ops / sizeof *ops, 0);
}

/* a draw reading the buffer at cb (an offset in the mapped area), whose word at 0x100 the CPU wrote as cpu_word, with
   an owner of 16 bytes of fill at own: 0 when it stored want, the owner is still there and nothing landed */
static int cbown_case(GpuChannel* channel, u32 cb, u32 own, u32 cpu_word, u8 fill, u32 want, const char* what) {
    const u64 result = OPR_GVA + 0x5000;
    const u32 desc[3] = {(u32)result, (u32)(result >> 32), 4};
    u8 bytes[4] = {0};
    CHECK(!gmem_write(result, bytes, sizeof bytes));
    CHECK(!storage_write_words(OPR_GVA + cb + 0x100, &cpu_word, 1));
    CHECK(!storage_write_words(OPR_GVA + cb + 0x130, desc, 3));
    channel->ds.bind.cb_addr[0][0] = OPR_GVA + cb;
    channel->ds.bind.cb_size[0][0] = 0x200;
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    vk_lock();
    VkOwnRes* owner = vk_own_res_new(16, VK_OWN_BY_DRAW);
    CHECK(owner && !owned_test_fill(owner->buf, 0, 16, fill));
    vk_own_publish_owned(OPR_CPU + own, 16, owner, 0);
    CHECK(own_lookup(OPR_CPU + own, 16, NULL, 0) == 1);
    u64 lands = own_landings(), drawn = g_vk_drawn, skipped = g_vk_skipped;
    DrawCall call = {.topology = 0, .count = 1, .instances = 1};
    vk_render_draw(&channel->ds, &call, &channel->owner);
    vk_finish();
    int drew = g_vk_drawn == drawn + 1 && g_vk_skipped == skipped;
    int kept = own_lookup(OPR_CPU + own, 16, NULL, 0) == 1, landed = own_landings() != lands;
    vk_unlock();
    CHECK(!gmem_read(result, bytes, sizeof bytes));
    u32 got = snx_ld_le32(bytes);
    if (!drew || got != want || !kept || landed)
        fprintf(stderr, "constant buffer, %s: drawn %d, stored %08x (expected %08x), owner %s, %s\n", what, drew, got,
                want, kept ? "kept" : "gone", landed ? "landed" : "nothing landed");
    CHECK(drew && got == want && kept && !landed);
    vk_lock();
    own_acquire(OPR_CPU + own, 16);
    vk_own_res_unref(owner);
    vk_unlock();
    return 0;
}

static int owned_constant_buffers(void) {
    if (!g_vk.feat.vertexPipelineStoresAndAtomics) {
        puts("owned page reads: no constant buffer case, as the device has no stores in vertex programs");
        return 0;
    }
    const u64 fragment[] = {0xE30000000007000Full};
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    CHECK(cbown_program());
    CHECK(mesh_program(OPR_GVA + 0x2000, fragment, 1, 1));
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 154;
    u32* r = channel->r3d;
    r[0x582] = (u32)(OPR_GVA >> 32);
    r[0x583] = (u32)OPR_GVA;
    r[0x850] = 1;
    r[0x851] = 0x2000;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 0;
    all_samples(r);
    memcpy(r + 0x280, transform, sizeof transform);
    channel->ds.owner = channel->id;
    CHECK(!cbown_case(channel, 0x3000, 0x3100, 0x11111111, 0x5a, 0x5a5a5a5a, "an owner in the bytes it binds"));
    CHECK(!cbown_case(channel, 0x6000, 0x6200, 0x22222222, 0x6b, 0x22222222, "an owner past them on their page"));
    channel->ds.bind.cb_addr[0][0] = 0;
    channel->ds.bind.cb_size[0][0] = 0;
    /* A fragment program's lookup folds the words of its constant buffers that the CPU holds (gpu/vk/cb_fold.h): of
       a buffer with an owner's bytes it reads none, so nothing lands, the owner keeps them and the word stays a load.
       The owner's bytes are zeros, a word that would fold */
    const u64 loads[] = {0x4C98078004070000ull, 0xE30000000007000Full}; /* MOV R0, c[0x0][0x100] */
    CHECK(mesh_program(OPR_GVA + 0x2800, loads, 2, 1));
    channel->ds.bind.cb_addr[4][0] = OPR_GVA + 0x3000;
    channel->ds.bind.cb_size[4][0] = 0x200;
    vk_lock();
    VkOwnRes* owner = vk_own_res_new(16, VK_OWN_BY_DRAW);
    CHECK(owner && !owned_test_fill(owner->buf, 0, 16, 0));
    vk_own_publish_owned(OPR_CPU + 0x3100, 16, owner, 0);
    CHECK(own_lookup(OPR_CPU + 0x3100, 16, NULL, 0) == 1);
    u64 lands = own_landings();
    const VkSh* folded =
        vk_shader_get(&channel->ds, OPR_GVA + 0x2800, 0, ST_FS, 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0, 0, 0);
    int kept = own_lookup(OPR_CPU + 0x3100, 16, NULL, 0) == 1, landed = own_landings() != lands;
    own_acquire(OPR_CPU + 0x3100, 16);
    vk_own_res_unref(owner);
    vk_unlock();
    CHECK(folded && !folded->failed && folded->nfold == 0 && folded->cbr.n == 1 && kept && !landed);
    channel->ds.bind.cb_addr[4][0] = 0;
    channel->ds.bind.cb_size[4][0] = 0;
    free(channel);
    puts("owned page reads: a draw's constant buffers took an owner's bytes and the CPU's beside another");
    return 0;
}

/* A word the GPU front end reads (gmem_r32: a semaphore's, a texture handle in a constant buffer), on a page an owner
   holds 16 other bytes of: the CPU's word, and the owner stays. Its direct read takes the word in place when nothing is
   pending anywhere, as in a run of this check alone, which reads it first */
static int owned_word(void) {
    const u64 gva = OPR_GVA + 0x8000, cpu = OPR_CPU + 0x8000;
    const u32 word = 0x3c3c5a5a;
    u8 bytes[4];
    snx_st_le32(bytes, word);
    CHECK(!gmem_write(gva + 0x10, bytes, sizeof bytes));
    vk_lock();
    VkOwnRes* owner = vk_own_res_new(16, VK_OWN_BY_DRAW);
    CHECK(owner && !owned_test_fill(owner->buf, 0, 16, 0x77));
    vk_own_publish_owned(cpu + 0x800, 16, owner, 0);
    CHECK(own_lookup(cpu + 0x800, 16, NULL, 0) == 1);
    const int direct = !g_cs_any && !g_gfx_any && !g_gfx_img_any && !g_uma_any && !g_gpu_releases;
    u64 lands = own_landings();
    u32 got = gmem_r32(gva + 0x10);
    int kept = own_lookup(cpu + 0x800, 16, NULL, 0) == 1, landed = own_landings() != lands;
    own_acquire(cpu + 0x800, 16);
    vk_own_res_unref(owner);
    vk_unlock();
    if (got != word || !kept || landed)
        fprintf(stderr, "owned page reads, a word: %08x (expected %08x), owner %s, %s\n", got, word,
                kept ? "kept" : "gone", landed ? "landed" : "nothing landed");
    CHECK(got == word && kept && !landed);
    printf("owned page reads: a word beside an owner read as the CPU's, %s\n",
           direct ? "where the direct read could take it" : "with results pending (the general path)");
    return 0;
}

/* An ASTC level's BC7 key (texture.c astc_content), hashed from the guest's bytes as the texture goes up, where the GPU
   samples BCn and not ASTC: an owner of 16 bytes at the start of the page the texture begins 512 bytes into. Its first
   level goes up unkeyed (its bytes aren't hashed), a later one on pages of its own is keyed, and the owner stays */
/* the texture's memory and address in vulkan_astc_later.inc's layout, apart from its cases' */
#define OPR_ASTC_INDEX 12
static int owned_astc_key(void) {
    static const AlCase c = {"ASTC 8x8 256x256 beside an owner", NVN_FORMAT_RGBA_ASTC_8x8, 256, 256, 1, 0};
    const int had = g_settings.astc_mode;
    AlTex x;
    memset(&x, 0, sizeof x);
    x.c = &c;
    const u64 base = AL_CPU + OPR_ASTC_INDEX * AL_SPAN, gbase = AL_GVA + OPR_ASTC_INDEX * AL_GVA_STEP;
    x.cpu = base + 0x200;
    x.rng = 0x9e3779b9u;
    x.tex.d.target = NVN_TEXTURE_TARGET_2D;
    x.tex.d.format = c.format;
    x.tex.d.levels = (u8)(32u - snx_clz32(c.w));
    x.tex.d.width = c.w;
    x.tex.d.height = c.h;
    x.tex.d.depth = 1;
    x.tex.gpu = gbase + 0x200;
    nvn_tex_layout(&x.tex.d, &x.lay);
    g_settings.astc_mode = 2;
    CHECK(nvn_tic_encode(&x.tex, NULL, 0, 0, x.tic));
    const VkFmt f = vk_tic_format(x.tic[0], 0);
    if (f.conv != VKC_ASTC || f.vk != VK_FORMAT_BC7_UNORM_BLOCK) {
        printf("owned page reads: no ASTC key case, as the GPU %s\n",
               f.conv != VKC_ASTC ? "samples ASTC as it is" : "holds no ASTC texture as BC7");
        g_settings.astc_mode = had;
        return 0;
    }
    const u64 own = base; /* before the texture's bytes, on their first page */
    CHECK(x.lay.size <= AL_SPAN - 0x200 && !mem_map(base, AL_SPAN, MS_NORMAL, PERM_RW));
    al_fill(&x);
    gmmu_map(gbase, base, AL_SPAN);
    later_drain(); /* what the checks before left to the encoder */
    char cache[sizeof g_cachedir + 40];
    al_cache_path(cache, sizeof cache, "vulkan_headless_owned_reads.vkbc7");
    CHECK(!al_cache_open(cache)); /* the keys are made only where a cache takes the levels */
    vk_lock();
    VkOwnRes* owner = vk_own_res_new(16, VK_OWN_BY_DRAW);
    CHECK(owner && !owned_test_fill(owner->buf, 0, 16, 0x3c));
    vk_own_publish_owned(own, 16, owner, 0);
    CHECK(own_lookup(own, 16, NULL, 0) == 1);
    u64 lands = own_landings();
    Surf* t = vk_tex_from_tic(x.tic);
    const u32 all = (1u << x.tex.d.levels) - 1, keyed = t ? t->bc7.keyed : 0;
    int made = t && t->upload_valid, kept = own_lookup(own, 16, NULL, 0) == 1, landed = own_landings() != lands;
    own_acquire(own, 16);
    vk_own_res_unref(owner);
    vk_unlock();
    if (!made || (keyed & 1) || !keyed || !kept || landed)
        fprintf(stderr, "owned page reads, %s: made %d, levels keyed %#x of %#x, owner %s, %s\n", c.name, made, keyed,
                all, kept ? "kept" : "gone", landed ? "landed" : "nothing landed");
    later_drain();
    vk_bc7_cache_close();
    remove(cache);
    gmmu_space_unmap(gbase, AL_SPAN);
    mem_unmap(base, AL_SPAN);
    g_settings.astc_mode = had;
    CHECK(made && !(keyed & 1) && keyed && kept && !landed);
    printf("owned page reads: an ASTC texture went up beside an owner, levels %#x of %#x keyed\n", keyed, all);
    return 0;
}

/* An NVN recording on a page an owner takes 16 bytes of after the submission and before the GPU thread replays it (a
   copy's result beside the game's command memory). The replay reads the record there without the renderer's lock, so
   the trap lands the owner and the record runs: its event is written, the owner's bytes are in guest memory */
static int g_opr_go; /* atomic: the replay's lane waits behind opr_gate while it is 0 */
static int opr_gate(void* arg) {
    (void)arg;
    return __atomic_load_n(&g_opr_go, __ATOMIC_ACQUIRE);
}
static int owned_nvn_record(void) {
    const u64 rec = OPR_CPU + 0xa000, handles = OPR_CPU + 0xb000, own = rec + 0x800, event = OPR_GVA + 0xc000;
    const u8 fill = 0x4d;
    u8 bytes[16];
    memset(bytes, 0, sizeof bytes);
    CHECK(!gmem_write(event, bytes, 4));
    const NvnRecHdr signal = {OP_SIGNAL_EVENT | NVN_REC_LAST, 8 + sizeof(NvnPSignalEvent)};
    const NvnPSignalEvent value = {event, NVN_EVENT_SIGNAL_MODE_WRITE, 0x2468ace0};
    memcpy(GP(rec), &signal, sizeof signal);
    memcpy(GP(rec + 8), &value, sizeof value);
    memcpy(GP(handles), &rec, sizeof rec);
    u32 q = nvn_replay_queue(), shown = 0;
    u64 ready = 0;
    __atomic_store_n(&g_opr_go, 0, __ATOMIC_RELEASE);
    gpu_call_poll(opr_gate, NULL, q - 1); /* the queue's lane (nvn_replay.c): its submission waits behind it */
    nvn_replay_submit(q, 1, handles, &ready, &shown);
    vk_lock();
    VkOwnRes* owner = vk_own_res_new(16, VK_OWN_BY_DRAW);
    CHECK(owner && !owned_test_fill(owner->buf, 0, 16, fill));
    vk_own_publish_owned(own, 16, owner, 0);
    CHECK(own_lookup(own, 16, NULL, 0) == 1);
    vk_unlock();
    u64 lands = own_landings();
    __atomic_store_n(&g_opr_go, 1, __ATOMIC_RELEASE);
    gpu_kick();
    nvn_replay_finish(q);
    int kept = own_lookup(own, 16, NULL, 0) != 0, landed = own_landings() != lands;
    CHECK(!gmem_read(event, bytes, 4));
    u32 written = snx_ld_le32(bytes);
    CHECK(!gmem_read(OPR_GVA + 0xa800, bytes, sizeof bytes));
    int same = 1;
    for (u32 i = 0; i < sizeof bytes; i++) same &= bytes[i] == fill;
    nvn_replay_queue_free(q);
    vk_lock();
    own_acquire(own, 16);
    vk_own_res_unref(owner);
    vk_unlock();
    if (written != 0x2468ace0 || kept || !landed || !same)
        fprintf(stderr, "owned page reads, an NVN record: event %08x (expected 2468ace0), owner %s, %s, its bytes %s\n",
                written, kept ? "kept" : "gone", landed ? "landed" : "nothing landed", same ? "there" : "not there");
    CHECK(written == 0x2468ace0 && !kept && landed && same);
    puts("owned page reads: an NVN record replayed off a page an owner took after its submission, which landed");
    return 0;
}

static int check_owned_page_reads(void) {
    if (!own_traps()) {
        puts("SKIP owned page reads: the host has no memory traps, so nothing is owned");
        return 0;
    }
    CHECK(!mem_map(OPR_CPU, 0x10000, MS_NORMAL, PERM_RW));
    gmmu_map(OPR_GVA, OPR_CPU, 0x10000);
    CHECK(!owned_word());
    CHECK(!owned_astc_key());
    CHECK(!owned_constant_buffers());
    CHECK(!owned_nvn_record());
    printf("PASS owned page reads, with %s guest buffers: none landed under the lock, the NVN replay's page did\n",
           vk_uma() ? "unified" : "copied");
    return 0;
}
