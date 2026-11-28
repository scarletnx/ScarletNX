/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* How a GPU that samples BCn but not ASTC holds the ASTC textures it decodes (astc_textures, settings.txt): BC7 (0, the
   default) shown at once as RGBA8 and encoded again as BC7 between frames, a quarter of RGBA8's memory, close to the
   game's blocks but not the same; Exact (1) as RGBA8; BC7 at load (2) encoded as each texture loads. Then when the log
   suggests BC7 to a run that holds them as RGBA8, and from what size a level held as BC7 at load is encoded quickly */
#ifndef SNX_TEXTURE_ASTC_BC_H
#define SNX_TEXTURE_ASTC_BC_H

/* 1 where astc_textures mode holds them as BC7: 0 between frames, 2 at load */
static inline int snx_astc_bc7(int mode) { return mode == 0 || mode == 2; }

/* why the images ran short of memory (vk_mem_trim): an image allocation found none, a trim left the caches past their
   budget, or a trim took textures or targets the last two presents used, the working set, which the next frames load
   again: the game held slow by eviction */
enum { SNX_ASTC_SHORT_NONE, SNX_ASTC_SHORT_NO_MEMORY, SNX_ASTC_SHORT_PAST_BUDGET, SNX_ASTC_SHORT_WORKING_SET };
/* 1 where the log suggests astc_textures=0 (Approximate): they are held as RGBA8 by Exact (bc7 0: astc_textures=1), the GPU
   samples BCn (bc) but not ASTC (astc), ASTC textures were decoded to RGBA8 (astc_bytes), and the images ran short
   (why) */
static inline int snx_astc_bc_suggest(int bc7, int bc, int astc, unsigned long long astc_bytes, int why) {
    return why != SNX_ASTC_SHORT_NONE && !bc7 && bc && !astc && astc_bytes;
}

/* A level held as BC7 at load is encoded quickly (astc.c's quick build) from astc_quick_mtexels megatexels, every layer
   counted, at the start; the BC7 encoder's fast build timed on this GPU (ns of its time over the texels of the
   dispatches timed) then sets the size. The setting stands for a time: the fast build's 25 ns a texel on the Radeon
   890M where it was measured (d8c8bd08e: two 1365x1365x90 arrays in 10.1 s), so 8 is a 0.21 s encode. Once the
   dispatches timed here cover as many texels as the start, the size is the one this GPU encodes in that time, at the
   cost a texel measured over the game's own blocks (65 to 1400 megatexels a second on one GPU, a game's normal maps to
   its albedo, astc.c): bigger on a faster GPU, smaller on a slower one. The size in texels, 0 (never) for a setting of
   0 */
#define SNX_ASTC_QUICK_REF_NS       25.0
/* The dispatches timed for it: the fast build's of a megatexel or more, the unit the setting counts in, and as big as
   the strips a very large level is encoded in. A smaller one is the latency of its slowest blocks more than the
   encoder's throughput: on an RX 6700 XT a 256x256 texture's nine levels took 4.42 ms, 50 ns a texel, where a
   1024x1024 level of the same blocks took 4.83 ms, 4.61 ns a texel; timing them all, one game's run read 36.80 ns, a
   GPU slower than the 890M's 25 */
#define SNX_ASTC_QUICK_TIMED_TEXELS (1ull << 20)
static inline unsigned long long snx_astc_quick_texels(int mtexels, unsigned long long ns, unsigned long long texels) {
    const unsigned long long start = mtexels > 0 ? (unsigned long long)mtexels << 20 : 0;
    double size;
    if (!start || !ns || texels < start) return start;
    size = (double)start * (double)texels * SNX_ASTC_QUICK_REF_NS / (double)ns;
    return size >= 9.0e18 ? 9000000000000000000ull : size < 1.0 ? 1ull : (unsigned long long)(size + 0.5);
}

/* The blocks the BC7 encoder may have queued on the GPU before an upload waits for it to do them (bc7_queue, in
   thousands; 0 is Auto): Auto gives 256 thousand for every 2 GiB of the GPU's own memory, heap bytes, from 256
   thousand to 4 million, and 256 thousand to a GPU that shares the CPU's memory (uma). A GT 1030 (2 GB) lost the
   device past 256 thousand, a second of its encoder; an RX 6700 XT (12 GB) encodes about 10 million a second, and
   waited 2 s a minute at 256 thousand in one game's loads */
#define SNX_BC7_QUEUE_MIN (256ull << 10)
#define SNX_BC7_QUEUE_MAX (4096ull << 10)
static inline unsigned long long snx_bc7_queue_blocks(int setting, unsigned long long heap, int uma) {
    unsigned long long n;
    if (setting > 0) return (unsigned long long)setting << 10;
    if (uma) return SNX_BC7_QUEUE_MIN;
    n = SNX_BC7_QUEUE_MIN * (heap / (2ull << 30));
    return n < SNX_BC7_QUEUE_MIN ? SNX_BC7_QUEUE_MIN : n > SNX_BC7_QUEUE_MAX ? SNX_BC7_QUEUE_MAX : n;
}

#endif
