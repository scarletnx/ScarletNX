/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Virtual textures (vt_sparse): a big ASTC texture the GPU only samples is an image with sparse residency whose
   memory follows what frames sample of it, a device's sparse block (64 KiB) at a time. The image is the texture's as
   ever, in the descriptors as ever: the GPU's own page table maps the blocks, and filters across them.

   The programs say what they sample (shader/vt_sample.c): one quad in SH_VT_QUADS writes a mark, the texture, level,
   layer and place of its sample and whether it found memory, into a table on the device, copied out and cleared each
   present. A sample that finds none reads a coarser level in the same instruction stream, so a block's first frames
   are blurred, never black. Here each mark that landed touches the blocks under it (the present they were last seen
   in), and one that found no memory asks for the blocks it names, with the blocks that cover them in every coarser
   level, so the coarser read finds texels one level away. A texture's wanted blocks are bound and only the new ones
   decoded from guest memory (texture.c vk_tex_vt_load), a bounded number each present; blocks not seen for vt_frames
   presents go, and sooner while the GPU's memory is under pressure. The mip tail is held always.

   A texture a draw binds where its samples don't mark (a vertex or compute program, a fetch, a gather, a view that
   starts past level 0) is held whole from then on, as is one whose misses nothing asked for ends. */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/vt_pages.h"
#include "nx/gpu/shader/shader.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

#define VT_BACKS     3u                         /* the table's copies in flight */
#define VT_SET_SLOTS 32u                        /* the slots of a Maxwell fragment program a mark can name */
#define VT_SETS      (SH_VT_IDS / VT_SET_SLOTS) /* its draws' sets of virtual textures: 0 is none */
#define VT_FLOOR     32u                        /* presents a block stays whatever the pressure */
#define VT_SETTLE    6u   /* presents after a texture's last bind before a miss under held blocks asks for more */
#define VT_STUCK     60u  /* frames of marks in a row with a miss that had nothing to ask for: the texture goes whole */
#define VT_NEW_MAX   512u /* blocks given memory a present: 32 MiB of texels to decode at most */
#define VT_SWEEP     8u   /* presents between the sweeps that let blocks go */
#define VT_SAY       120u /* presents between the log's lines */

#define VT_UNMAPPED ~0u /* seen[b] of a block guest memory doesn't map: asked for, and given no memory */
/* A virtual texture's record. seen[b]: the present block b was last sampled in (or asked for), 0 never. want: the
   blocks its next bind gives memory; held: the ones its last bind gave; fresh: of those, the ones with no texels yet.
   The four arrays are one allocation, seen's */
typedef struct {
    Surf* t; /* NULL: a free record */
    u32 gen;
    VtShape shape;
    u32 nblocks;
    u32* seen;
    u8 *want, *held, *fresh;
    u32 bound_at; /* the present of its last bind that changed something */
    u32 stuck;
    u8 changed, whole, missed, asked;
} VtTex;
typedef struct {
    u32 slot, gen; /* the record (plus one) and its generation */
} VtId;
static struct {
    VtTex* v;
    u32 n, cap, now, turn;
    VkBuffer table, back[VT_BACKS];
    DevMem table_mem, back_mem[VT_BACKS];
    u32* back_ptr[VT_BACKS];
    u64 back_sub[VT_BACKS];
    u32 slot;
    VtId* ids; /* SH_VT_IDS: what each identity of the marks named when a draw last bound it */
    /* the identities that name a virtual texture now, a bit each, as the programs read them after the table: the
       host's copy, staged and copied to the device when it changed */
    u32* named;
    VkBuffer named_stage;
    DevMem named_mem;
    u32* named_ptr;
    int named_dirty;
    /* a Maxwell program's marks name a set of the draw and a slot (identity set << 5 | slot): the virtual textures of
       the draw being set up, a slot each, and the sets given so far by the hash of theirs (0: free) */
    VtId draw[VT_SET_SLOTS];
    u32 draw_any;
    u64 set_key[VT_SETS];
    /* since the log's last line */
    u64 marks, misses, unknown, asked, bound, gone, loads, fails, wholes, deferred, ns, frames, filled;
} g_vt;

static VtTex* vt_rec(const Surf* t) { return t && t->vt_slot ? &g_vt.v[t->vt_slot - 1] : NULL; }
static int bit(const u8* b, u32 i) { return b[i >> 3] >> (i & 7) & 1; }
static void set(u8* b, u32 i) { b[i >> 3] |= (u8)(1u << (i & 7)); }
static void clear(u8* b, u32 i) { b[i >> 3] &= (u8) ~(1u << (i & 7)); }

int vk_vt_on(void) { return g_shader_vt_address != 0; }
int vk_vt_makes(int holes) { return vk_vt_on() && !(g_shader_vt_parts & (holes ? 24 : 8)); }
u32 vk_vt_phase(void) { return g_vt.now & (SH_VT_QUADS - 1); }

int vk_vt_register(Surf* t) {
    const VkSparseShape* sh = vk_image_sparse_shape(t->img);
    VtShape shape;
    /* levels held always to fall back on (a mip tail, or levels smaller than a block), a level 0 of blocks to page
       (two each way at least), and marks that can name its layers and levels */
    if (!sh || sh->layers > SH_VT_LAYERS || sh->gran.depth != 1 ||
        !vt_shape(&shape, t->w, t->h, sh->layers, sh->levels, sh->gran.width, sh->gran.height, sh->tail_first) ||
        shape.per_layer != sh->per_layer || !shape.tail_first || shape.tail_first >= sh->levels || shape.bx[0] < 2 ||
        shape.by[0] < 2)
        return 0;
    u32 i = 0;
    while (i < g_vt.n && g_vt.v[i].t) i++;
    if (i == g_vt.n) {
        if (g_vt.n == g_vt.cap) {
            const u32 cap = g_vt.cap ? g_vt.cap * 2 : 256;
            VtTex* grown = realloc(g_vt.v, cap * sizeof *grown);
            if (!grown) return 0;
            g_vt.v = grown;
            g_vt.cap = cap;
        }
        g_vt.v[g_vt.n++] = (VtTex){0};
    }
    VtTex* L = &g_vt.v[i];
    const u32 n = shape.layers * shape.per_layer, bytes = n / 8 + 1;
    u32* seen = calloc(1, (size_t)n * sizeof *seen + 3u * bytes);
    if (!seen) return 0;
    *L = (VtTex){.t = t, .gen = L->gen + 1, .shape = shape, .nblocks = n, .seen = seen};
    L->whole = !(g_shader_vt_parts & 4); /* vt_parts without the marks: nothing would ask for its blocks */
    L->want = (u8*)(seen + n);
    L->held = L->want + bytes;
    L->fresh = L->held + bytes;
    t->vt_slot = i + 1;
    return 1;
}
void vk_vt_forget(Surf* t) {
    VtTex* L = vt_rec(t);
    if (!L) return;
    free(L->seen);
    const u32 gen = L->gen;
    *L = (VtTex){.gen = gen};
    t->vt_slot = 0;
}
void vk_vt_want(const Surf* t, u8* bits, u64 nbits) {
    const VtTex* L = vt_rec(t);
    if (!L || L->whole) return;
    for (u64 b = 0; b < nbits; b++)
        if (b >= L->nblocks || (!bit(L->want, (u32)b) && !vt_block_pinned(&L->shape, (u32)b)))
            bits[b >> 3] &= (u8) ~(1u << (b & 7));
}
void vk_vt_bound(const Surf* t, const u8* bits, u64 nbits) {
    VtTex* L = vt_rec(t);
    if (!L) return;
    int changed = 0;
    for (u32 b = 0; b < L->nblocks && b < nbits; b++) {
        const int now = bit(bits, b), was = bit(L->held, b);
        /* asked for where guest memory maps nothing: not asked for again until the texture loads again */
        if (!now && t->vt_mapped && !L->whole && bit(L->want, b) && !vt_block_pinned(&L->shape, b)) {
            clear(L->want, b);
            L->seen[b] = VT_UNMAPPED;
        }
        if (now == was) continue;
        changed = 1;
        if (now) {
            set(L->held, b);
            set(L->fresh, b);
            g_vt.bound++;
        } else {
            clear(L->held, b);
            clear(L->fresh, b); /* a block let go has nothing left to decode */
            g_vt.gone++;
        }
    }
    if (changed) L->bound_at = g_vt.now;
}
const u8* vk_vt_fresh(const Surf* t, u32* nblocks) {
    const VtTex* L = vt_rec(t);
    *nblocks = L ? L->nblocks : 0;
    return L ? L->fresh : NULL;
}
void vk_vt_loaded(const Surf* t) {
    VtTex* L = vt_rec(t);
    if (L) memset(L->fresh, 0, L->nblocks / 8 + 1);
}
void vk_vt_remapped(const Surf* t) {
    VtTex* L = vt_rec(t);
    for (u32 b = 0; L && b < L->nblocks; b++)
        if (L->seen[b] == VT_UNMAPPED) L->seen[b] = 0;
}
void vk_vt_whole(Surf* t, const char* why) {
    VtTex* L = vt_rec(t);
    if (!L || L->whole) return;
    L->whole = 1;
    g_vt.wholes++;
    LOG_LIMIT(64, "vt: a %ux%ux%u texture of %u levels is held whole from now on: %s\n", t->w, t->h, t->layers,
              t->levels, why);
    /* at once: the draw that binds it reads every block. Without memory for it now, at the next present */
    if (!vk_tex_vt_load(t) && t->vt_slot) g_vt.v[t->vt_slot - 1].changed = 1;
}
/* identity id names the virtual texture of record slot (plus one) and generation gen, or with slot 0 none */
static void name(u32 id, u32 slot, u32 gen) {
    id &= SH_VT_IDS - 1;
    g_vt.ids[id] = (VtId){slot, gen};
    const u32 bit = 1u << (id & 31), had = g_vt.named[id >> 5];
    g_vt.named[id >> 5] = slot ? had | bit : had & ~bit;
    if (g_vt.named[id >> 5] != had) g_vt.named_dirty = 1;
}
void vk_vt_bind(u32 id, Surf* t, const char* unmarked) {
    if (!vk_vt_on()) return;
    const VtTex* L = vt_rec(t);
    if (L && !L->whole && unmarked) vk_vt_whole(t, unmarked);
    if (!L || L->whole) { /* no virtual texture, or one that pages nothing: its samples have nothing to mark */
        if (g_vt.ids[id & (SH_VT_IDS - 1)].slot) name(id, 0, 0);
        return;
    }
    name(id, t->vt_slot, L->gen);
}

void vk_vt_slot(u32 slot, Surf* t, const char* unmarked) {
    const VtTex* L = vt_rec(t);
    if (!L || L->whole) return;
    if (unmarked) {
        vk_vt_whole(t, unmarked);
        return;
    }
    if (slot >= VT_SET_SLOTS) return; /* a sample past the 32 slots marks nothing and `unmarked` says so: no read */
    g_vt.draw[slot] = (VtId){t->vt_slot, L->gen};
    g_vt.draw_any = 1;
}
u32 vk_vt_draw_word(void) {
    const u32 phase = vk_vt_phase();
    if (!g_vt.draw_any) return phase;
    u64 key = 0xcbf29ce484222325ull;
    for (u32 i = 0; i < VT_SET_SLOTS; i++) {
        key = (key ^ g_vt.draw[i].slot) * 0x100000001b3ull;
        key = (key ^ g_vt.draw[i].gen) * 0x100000001b3ull;
    }
    if (!key) key = 1;
    /* the set of this hash, a free one within eight of its place, or its place's taken over (marks in flight that
       name the old one there then name these textures: blocks nobody samples, which go again) */
    const u32 home = 1 + (u32)(key % (VT_SETS - 1));
    u32 set = home;
    for (u32 probe = 0; probe < 8; probe++) {
        const u32 i = 1 + (home - 1 + probe) % (VT_SETS - 1);
        if (g_vt.set_key[i] == key || !g_vt.set_key[i]) {
            set = i;
            break;
        }
    }
    if (g_vt.set_key[set] != key) {
        g_vt.set_key[set] = key;
        for (u32 i = 0; i < VT_SET_SLOTS; i++) name(set * VT_SET_SLOTS + i, g_vt.draw[i].slot, g_vt.draw[i].gen);
    }
    memset(g_vt.draw, 0, sizeof g_vt.draw);
    g_vt.draw_any = 0;
    return phase | set << 4;
}

/* block b seen now, and the blocks that cover it in the coarser levels: none of them is older than it */
static void touch(VtTex* L, u32 b) {
    for (; b != ~0u && L->seen[b] != g_vt.now && L->seen[b] != VT_UNMAPPED; b = vt_block_parent(&L->shape, b))
        L->seen[b] = g_vt.now;
}
/* block b and the ones that cover it wanted: 1 when any was not */
static int ask(VtTex* L, u32 b) {
    int any = 0;
    for (; b != ~0u; b = vt_block_parent(&L->shape, b)) {
        if (L->seen[b] == VT_UNMAPPED) continue;
        L->seen[b] = g_vt.now;
        if (bit(L->want, b)) continue;
        set(L->want, b);
        any = 1;
        g_vt.asked++;
    }
    if (any) L->changed = 1;
    return any;
}
static int pending(const VtTex* L) {
    for (u32 i = 0; i < L->nblocks / 8 + 1; i++)
        if ((L->want[i] & ~L->held[i]) | L->fresh[i]) return 1;
    return 0;
}
/* One mark that landed. Its blocks with memory are seen now. One that found no memory asks for the blocks under it;
   when those are held and the texture's binds have settled, for the ones a filter reaches from it; when those are held
   too, for the blocks under it one level finer and one coarser (a sampler's bias or clamp, which the mark's level
   doesn't have for a sample at a level the program gives). A miss with nothing left to ask for counts towards holding
   the texture whole */
static void mark_land(u32 w0, u32 w1) {
    const VtId id = g_vt.ids[w0 & (SH_VT_IDS - 1)];
    if (!id.slot || id.slot > g_vt.n) {
        g_vt.unknown++;
        return;
    }
    VtTex* L = &g_vt.v[id.slot - 1];
    if (!L->t || L->gen != id.gen) {
        g_vt.unknown++;
        return;
    }
    if (L->whole) return;
    VtMark m = {.level = w0 >> 20 & 15,
                .layer = w1 >> 20,
                .u = w1 & 1023,
                .v = w1 >> 10 & 1023,
                .uo = (u8)(w0 >> 25 & 3),
                .vo = (u8)(w0 >> 27 & 3),
                .resident = (u8)(w0 >> 24 & 1)};
    static u32 under[VT_MARK_BLOCKS], near[VT_MARK_BLOCKS];
    u32 nu, nn;
    vt_mark_blocks(&L->shape, &m, under, &nu, near, &nn);
    g_vt.marks++;
    for (u32 i = 0; i < nu; i++)
        if (bit(L->held, under[i])) touch(L, under[i]);
    for (u32 i = 0; i < nn; i++)
        if (bit(L->held, near[i])) touch(L, near[i]);
    if (m.resident || m.level >= L->shape.tail_first) return;
    g_vt.misses++;
    int asked = 0;
    for (u32 i = 0; i < nu; i++) asked |= ask(L, under[i]);
    if (asked) {
        L->asked = 1;
        return;
    }
    if (g_vt.now - L->bound_at <= VT_SETTLE || pending(L)) return; /* what it asked for is on its way */
    for (u32 i = 0; i < nn; i++) asked |= ask(L, near[i]);
    /* then the blocks under it in other levels, the nearest first: a level finer, one coarser, up to four finer (an
       anisotropic sample at a level the program computed reads that much finer than the mark's level says) */
    static const int others[5] = {-1, 1, -2, -3, -4};
    for (u32 k = 0; k < 5 && !asked; k++) {
        const int d = others[k];
        if ((d < 0 && m.level < (u32)-d) || m.level + (u32)d >= L->shape.tail_first) continue;
        VtMark o = m;
        o.level = m.level + (u32)d;
        static u32 other[VT_MARK_BLOCKS], far[VT_MARK_BLOCKS];
        u32 no, nf;
        vt_mark_blocks(&L->shape, &o, other, &no, far, &nf);
        for (u32 i = 0; i < no; i++) asked |= ask(L, other[i]);
    }
    if (asked)
        L->asked = 1;
    else
        L->missed = 1;
}
static void table_land(u32 slot) {
    const u32* words = g_vt.back_ptr[slot];
    for (u32 i = 0; i < SH_VT_SLOTS; i++) {
        const u32 w0 = words[2 * i], w1 = words[2 * i + 1];
        if (!(w0 >> 31)) continue;
        g_vt.filled++;
        mark_land(w0, w1);
    }
    g_vt.frames++;
    for (u32 i = 0; i < g_vt.n; i++) {
        VtTex* L = &g_vt.v[i];
        if (!L->t) continue;
        if (L->asked || !L->missed)
            L->stuck = 0;
        else if (++L->stuck > VT_STUCK)
            vk_vt_whole(L->t, "its samples keep finding no memory and name nothing more to load");
        L->asked = L->missed = 0;
    }
}
/* the frame's marks into a free host buffer, the table cleared for the next frame's */
static void table_copy(void) {
    const u32 s = g_vt.slot;
    if (g_vt.back_sub[s]) return; /* none free: the frame's marks stay for the next copy */
    VkCommandBuffer cb = vk_cmd();
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                           .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    rec_CmdPipelineBarrier2(cb, &d);
    VkBufferCopy copy = {0, 0, (u64)SH_VT_SLOTS * 8};
    rec_CmdCopyBuffer(cb, g_vt.table, g_vt.back[s], 1, &copy);
    rec_CmdPipelineBarrier2(cb, &d);
    rec_CmdFillBuffer(cb, g_vt.table, 0, (u64)SH_VT_SLOTS * 8, 0);
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
    mb.dstAccessMask =
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_HOST_READ_BIT;
    rec_CmdPipelineBarrier2(cb, &d);
    g_vt.back_sub[s] = vk_last_submit() + 1;
    g_vt.slot = (s + 1) % VT_BACKS;
}
/* the identities' bits to the device, after the table, when a bind changed one */
static void named_copy(void) {
    if (!g_vt.named_dirty) return;
    g_vt.named_dirty = 0;
    memcpy(g_vt.named_ptr, g_vt.named, SH_VT_IDS / 8);
    VkCommandBuffer cb = vk_cmd();
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT,
                           .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_HOST_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                           .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    rec_CmdPipelineBarrier2(cb, &d);
    VkBufferCopy copy = {0, (u64)SH_VT_SLOTS * 8, SH_VT_IDS / 8};
    rec_CmdCopyBuffer(cb, g_vt.named_stage, g_vt.table, 1, &copy);
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    rec_CmdPipelineBarrier2(cb, &d);
}
/* blocks not seen for vt_frames presents are no longer wanted; while the GPU's memory is under pressure, the oldest
   eighth of those held too, down to the ones seen in the last VT_FLOOR presents */
static void sweep(void) {
    static u32 hist[VT_AGES];
    memset(hist, 0, sizeof hist);
    u64 held = 0;
    for (u32 i = 0; i < g_vt.n; i++) {
        const VtTex* L = &g_vt.v[i];
        if (!L->t || L->whole) continue;
        vt_age_count(hist, L->seen, L->want, L->nblocks, g_vt.now);
    }
    for (u32 a = 0; a < VT_AGES; a++) held += hist[a];
    u32 cut = vt_age_cut(hist, vk_mem_pressed() ? held / 8 * 7 : held, VT_FLOOR);
    const u32 window = (u32)g_settings.vt_frames;
    if (cut > window) cut = window;
    if (cut < VT_FLOOR) cut = VT_FLOOR;
    for (u32 i = 0; i < g_vt.n; i++) {
        VtTex* L = &g_vt.v[i];
        if (!L->t || L->whole) continue;
        for (u32 b = 0; b < L->nblocks; b++) {
            if (!bit(L->want, b)) continue;
            const u32 age = L->seen[b] && L->seen[b] != VT_UNMAPPED ? g_vt.now - L->seen[b] : VT_AGES - 1;
            if (age < cut) continue;
            clear(L->want, b);
            L->changed = 1;
        }
    }
}
static void say(void) {
    u64 n = 0, whole = 0, whole_bytes = 0, bytes = 0, blocks = 0, all = 0;
    for (u32 i = 0; i < g_vt.n; i++) {
        const VtTex* L = &g_vt.v[i];
        if (!L->t) continue;
        n++;
        bytes += L->t->bytes;
        all += L->nblocks;
        for (u32 b = 0; b < L->nblocks; b++) blocks += (u64)(bit(L->held, b) && !vt_block_pinned(&L->shape, b));
        if (L->whole) whole++, whole_bytes += L->t->bytes;
    }
    const u64 frames = g_vt.frames ? g_vt.frames : 1;
    LOG("vt: %llu virtual textures hold %llu MB, %llu of their %llu blocks; %llu of them whole (%llu MB), %llu turned "
        "whole; in %u presents: %llu marks a frame of marks (%llu of the table's %u slots), %llu of them misses, "
        "%llu naming no texture; %llu blocks asked for, %llu bound, %llu let go, %llu loads, %llu without memory, "
        "%llu put off; %.3f ms of the host a present\n",
        (unsigned long long)n, (unsigned long long)(bytes >> 20), (unsigned long long)blocks, (unsigned long long)all,
        (unsigned long long)whole, (unsigned long long)(whole_bytes >> 20), (unsigned long long)g_vt.wholes, VT_SAY,
        (unsigned long long)(g_vt.marks / frames), (unsigned long long)(g_vt.filled / frames), SH_VT_SLOTS,
        (unsigned long long)(g_vt.misses / frames), (unsigned long long)(g_vt.unknown / frames),
        (unsigned long long)g_vt.asked, (unsigned long long)g_vt.bound, (unsigned long long)g_vt.gone,
        (unsigned long long)g_vt.loads, (unsigned long long)g_vt.fails, (unsigned long long)g_vt.deferred,
        (double)g_vt.ns / 1e6 / VT_SAY);
    g_vt.marks = g_vt.misses = g_vt.unknown = g_vt.asked = g_vt.bound = g_vt.gone = g_vt.loads = g_vt.fails = 0;
    g_vt.wholes = g_vt.deferred = g_vt.ns = g_vt.frames = g_vt.filled = 0;
}
void vk_vt_stats(const Surf* t, VkVtStats* s) {
    *s = (VkVtStats){0};
    for (u32 i = 0; i < g_vt.n; i++) {
        const VtTex* L = &g_vt.v[i];
        if (!L->t || (t && L->t != t)) continue;
        s->textures++;
        s->whole += L->whole;
        s->bytes += L->t->bytes;
        for (u32 b = 0; b < L->nblocks; b++) s->held += (u32)(bit(L->held, b) && !vt_block_pinned(&L->shape, b));
    }
}

/* vt_sparse or vk_gpu_profile: every 600 presents, what the textures' loads moved in them: the guest bytes staged for
   the GPU, the bytes the ASTC decoder made RGBA8 and the bytes its encoders made BC7, each a present, so a run with
   virtual textures and one without say the same things */
static void traffic_say(void) {
    static u64 last[4], presents;
    if (!g_settings.vt_sparse && !g_settings.gpu_profile) return;
    if (++presents % 600) return;
    u64 uploads, levels, hits, rgba8, texels, now[4];
    vk_tex_stats(&uploads, &now[0]);
    vk_astc_stats(&levels, &hits, &now[2], &rgba8);
    vk_tex_vt_stats(&now[3], &texels);
    now[1] = vk_astc_rgba8_bytes();
    LOG("vulkan: texture traffic over 600 presents: %llu KB of guest bytes staged a present, %llu KB of ASTC decoded "
        "to RGBA8 and %llu KB encoded as BC7 a present; %llu virtual texture blocks decoded\n",
        (unsigned long long)((now[0] - last[0]) / 600 >> 10), (unsigned long long)((now[1] - last[1]) / 600 >> 10),
        (unsigned long long)((now[2] - last[2]) / 600 >> 10), (unsigned long long)(now[3] - last[3]));
    memcpy(last, now, sizeof last);
}
void vk_vt_frame(void) {
    traffic_say();
    if (!vk_vt_on()) return;
    const u64 t0 = plat_ns();
    g_vt.now++;
    u64 done = 0;
    if (VK_OK(vkGetSemaphoreCounterValue(g_vk.dev, vk_gfx_timeline(), &done)))
        for (u32 i = 0; i < VT_BACKS; i++)
            if (g_vt.back_sub[i] && g_vt.back_sub[i] <= done) {
                table_land(i);
                g_vt.back_sub[i] = 0;
            }
    if (g_vt.now % VT_SWEEP == 0) sweep();
    /* the textures whose wanted blocks changed, bound and their new blocks decoded: VT_NEW_MAX new blocks a present,
       starting after the texture the last present ended at */
    u32 spent = 0;
    for (u32 k = 0; k < g_vt.n; k++) {
        const u32 i = (g_vt.turn + k) % g_vt.n;
        VtTex* L = &g_vt.v[i];
        if (!L->t || !L->changed) continue;
        u32 fresh = 0;
        for (u32 b = 0; b < L->nblocks; b++)
            fresh += (u32)(L->whole ? !bit(L->held, b) : bit(L->want, b) && !bit(L->held, b));
        if (spent && spent + fresh > VT_NEW_MAX) {
            g_vt.deferred++;
            continue;
        }
        spent += fresh;
        L->changed = 0;
        g_vt.loads++;
        Surf* t = L->t;
        if (!vk_tex_vt_load(t)) {
            g_vt.fails++;
            if (t->vt_slot) g_vt.v[t->vt_slot - 1].changed = 1; /* tried again at the next present */
        }
        if (spent >= VT_NEW_MAX) {
            g_vt.turn = i + 1;
            break;
        }
    }
    table_copy();
    named_copy();
    g_vt.ns += plat_ns() - t0;
    if (g_vt.now % VT_SAY == 0) say();
}

static int table_buffer(VkBuffer* b, DevMem* m, u32 at, u64 size, VkBufferUsageFlags usage, u32** ptr) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = size,
                             .usage = usage,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    DevMemWant want = {
        .at = at, .mapped = at != DM_DEVICE, .need = at != DM_DEVICE ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0};
    DevMemInfo info;
    if (!VK_OK(vk_devmem_buffer(&bi, &want, b, m, &info))) return 0;
    if (ptr) *ptr = info.ptr;
    return 1;
}
void vk_vt_init(void) {
    if (!g_settings.vt_sparse) return;
    const char* lacks = !g_vk.sparse                         ? "sparse binding on the graphics queue"
                        : !g_vk.feat.sparseResidencyImage2D  ? "sparse residency of 2D images"
                        : !g_vk.feat.shaderResourceResidency ? "shaderResourceResidency"
                                                             : NULL;
    if (lacks) {
        LOG("vt: vt_sparse=1 asks for virtual textures, and the device has no %s: textures are held whole, as with "
            "vt_sparse=0\n",
            lacks);
        return;
    }
    const u64 table = (u64)SH_VT_SLOTS * 8, named = SH_VT_IDS / 8;
    g_vt.ids = calloc(SH_VT_IDS, sizeof *g_vt.ids);
    g_vt.named = calloc(SH_VT_IDS / 32, sizeof *g_vt.named);
    int ok = g_vt.ids && g_vt.named &&
             table_buffer(&g_vt.table, &g_vt.table_mem, DM_DEVICE, table + named,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          NULL) &&
             table_buffer(&g_vt.named_stage, &g_vt.named_mem, DM_CACHED, named, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          &g_vt.named_ptr) &&
             g_vt.named_ptr;
    for (u32 i = 0; ok && i < VT_BACKS; i++)
        ok = table_buffer(&g_vt.back[i], &g_vt.back_mem[i], DM_CACHED, table, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          &g_vt.back_ptr[i]) &&
             g_vt.back_ptr[i];
    if (!ok) fatal("vt: no memory for the table of the samples' marks (%u KiB)", (u32)((table + named) >> 10));
    g_vt.named_dirty = 1; /* the device's copy starts as the host's: no identity named */
    VkBufferDeviceAddressInfo ai = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = g_vt.table};
    g_shader_vt_address = vkGetBufferDeviceAddress(g_vk.dev, &ai);
    g_shader_vt_parts = (u32)g_settings.vt_parts;
    if (g_shader_vt_parts != 7)
        LOG("vt: vt_parts=%u: a measurement, the samples translated without %s%s%s; without the marks every virtual "
            "texture is held whole%s\n",
            g_shader_vt_parts, g_shader_vt_parts & 1 ? "" : "the sparse sample, ",
            g_shader_vt_parts & 2 ? "" : "the coarser read, ", g_shader_vt_parts & 4 ? "" : "the marks",
            g_shader_vt_parts & 8    ? "; no texture is made virtual"
            : g_shader_vt_parts & 16 ? "; no texture mapped in part is made virtual"
                                     : "");
    LOG("vt: virtual textures on: ASTC textures of 1 MiB and more that programs only sample take memory a %u KiB "
        "block at a time, where frames sample them; the marks' table of %u KiB at device address %llx; a block goes "
        "%d presents after it was last sampled\n",
        64u, (u32)((table + named) >> 10), (unsigned long long)g_shader_vt_address, g_settings.vt_frames);
}
