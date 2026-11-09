/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* How a GPU that samples BCn but not ASTC holds the ASTC textures it decodes (astc_mode, settings.txt): Exact (1) as
   RGBA8, BC7 (2) encoded again as BC7 between frames, a quarter of RGBA8's memory, close to the game's blocks but not
   the same, and Auto (0, the default) one of the two by the GPU's memory. Then when the log suggests BC7 to a run that
   holds them as RGBA8 */
#ifndef SNX_TEXTURE_ASTC_BC_H
#define SNX_TEXTURE_ASTC_BC_H

/* Auto takes Exact where the budget of the heap images go to (the driver's, VK_EXT_memory_budget, or vk_budget_mb when
   lower) is this many MiB or more as the device starts, else BC7: one game took 3.9 GB of an RX 6700 XT's 11.8 with
   its ASTC textures as RGBA8, 0.4 more than as BC7, where another ran short of a 5.7 GB budget as RGBA8 and took 2.5 GB
   as BC7 */
#define SNX_ASTC_EXACT_MB 8192ull
/* 1 where astc_mode's Auto holds them as BC7: a budget of budget bytes, under SNX_ASTC_EXACT_MB MiB */
static inline int snx_astc_auto_bc7(unsigned long long budget) { return budget < (SNX_ASTC_EXACT_MB << 20); }
/* 1 where astc_mode mode holds them as BC7 (2 between frames, 3 at load), Auto as auto_bc7 says (snx_astc_auto_bc7 of
   the budget at start) */
static inline int snx_astc_bc7(int mode, int auto_bc7) { return mode == 2 || mode == 3 || (mode == 0 && auto_bc7); }

/* why the images ran short of memory (vk_mem_trim): an image allocation found none, a trim left the caches past their
   budget, or a trim took textures or targets the last two presents used, the working set, which the next frames load
   again: the game held slow by eviction */
enum { SNX_ASTC_SHORT_NONE, SNX_ASTC_SHORT_NO_MEMORY, SNX_ASTC_SHORT_PAST_BUDGET, SNX_ASTC_SHORT_WORKING_SET };
/* 1 where the log suggests astc_mode=2 (BC7): they are held as RGBA8 (bc7 0: Exact, or Auto with the memory for it),
   the GPU samples BCn (bc) but not ASTC (astc), ASTC textures were decoded to RGBA8 (astc_bytes), and the images ran
   short (why) */
static inline int snx_astc_bc_suggest(int bc7, int bc, int astc, unsigned long long astc_bytes, int why) {
    return why != SNX_ASTC_SHORT_NONE && !bc7 && bc && !astc && astc_bytes;
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
