/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* How the ASTC textures a GPU decodes are held (gpu/texture/astc_bc.h): astc_mode's Exact (1) as RGBA8, BC7 (2, 3)
   as BC7, Auto (0) as the room in the GPU's memory says as the game runs: BC7 from a trim, Exact again past the most
   room a trim has left. Then when the log suggests BC7: only where Exact holds them as RGBA8, on a GPU that samples BCn
   but not ASTC, with ASTC textures decoded to RGBA8, and the images short of memory for one of its three reasons. Then
   the encoder's queue. The budgets are given here, as vk_mem_use would give them */
#include <assert.h>
#include "nx/gpu/texture/astc_bc.h"
#define MB (1ull << 20)

/* a trim at the caches' use `use` past mark, freeing freed, then the present after it at the use the trim left: what
   Auto holds the next ASTC levels as after the present */
static int trim_then_present(SnxAstcAuto* a, unsigned long long use, unsigned long long freed,
                             unsigned long long mark) {
    const unsigned long long left = use > freed ? use - freed : 0;
    snx_astc_auto_trimmed(a, left, mark);
    snx_astc_auto_room(a, left, mark);
    return a->bc7;
}

int main(void) {
    int why;
    unsigned long long use, freed;
    /* Auto starts Exact, and a present never turns Exact to BC7: only a trim does */
    {
        SnxAstcAuto a = {0, 0, 0};
        assert(!snx_astc_auto_room(&a, 0, 9000 * MB) && !snx_astc_auto_room(&a, 20000 * MB, 9000 * MB));
        assert(!a.bc7 && !a.band && !a.switches);
        /* the first trim, past a 9000 MB mark, leaves the caches at 8000: BC7, and a band of
           the 1000 MB it left under the mark */
        assert(snx_astc_auto_trimmed(&a, 8000 * MB, 9000 * MB) && a.bc7 && a.band == 1000 * MB && a.switches == 1);
        /* another trim leaving less room keeps the band; one leaving more widens it; one leaving the caches past the
           mark (all it could free was not enough) leaves it */
        assert(!snx_astc_auto_trimmed(&a, 8500 * MB, 9000 * MB) && a.band == 1000 * MB && a.switches == 1);
        assert(!snx_astc_auto_trimmed(&a, 7600 * MB, 9000 * MB) && a.band == 1400 * MB);
        assert(!snx_astc_auto_trimmed(&a, 9500 * MB, 9000 * MB) && a.band == 1400 * MB && a.bc7);
        /* presents: BC7 stays with the caches past the mark, at it, and with room up to the band itself */
        assert(!snx_astc_auto_room(&a, 9800 * MB, 9000 * MB) && !snx_astc_auto_room(&a, 9000 * MB, 9000 * MB));
        assert(!snx_astc_auto_room(&a, 7600 * MB, 9000 * MB) && a.bc7 && a.switches == 1);
        /* a byte more room than the band: Exact again */
        assert(snx_astc_auto_room(&a, 7600 * MB - 1, 9000 * MB) && !a.bc7 && a.switches == 2);
        assert(!snx_astc_auto_room(&a, 0, 9000 * MB) && a.switches == 2);
        /* the next trim turns it to BC7 again; the band is the run's, kept */
        assert(snx_astc_auto_trimmed(&a, 8200 * MB, 9000 * MB) && a.bc7 && a.switches == 3 && a.band == 1400 * MB);
        /* the budget moves: a mark of 4500 MB (a budget that fell to 5000) with the caches at 3000 is 1500 MB of room,
           past the band: Exact; the same use under a mark of 9000 is not */
        assert(!snx_astc_auto_room(&a, 7600 * MB, 9000 * MB));
        assert(snx_astc_auto_room(&a, 3000 * MB - 1, 4500 * MB) && !a.bc7);
    }
    /* what a trim leaves: the use it counted (an allocation's 300 MB in it) less what it freed; an allocation that
       found no memory left out, as it comes later; never under nothing */
    assert(snx_astc_trim_left(9500 * MB, 300 * MB, 1500 * MB, 0) == 8000 * MB);
    assert(snx_astc_trim_left(9500 * MB, 300 * MB, 1500 * MB, 1) == 7700 * MB);
    assert(snx_astc_trim_left(9500 * MB, 300 * MB, 9600 * MB, 0) == 0 &&
           snx_astc_trim_left(200 * MB, 300 * MB, 0, 1) == 0);
    /* A trim never brings Exact back by itself: whatever it frees from past the mark, the present after it at the use
       it left finds no more room than the band, which that trim's own room has joined. Nor does a forced one under
       the mark (an image found no memory there), the present seeing the caches without that image */
    for (use = 9000 * MB + 1; use <= 11000 * MB; use += 97 * MB)
        for (freed = 0; freed <= use; freed += 211 * MB) {
            SnxAstcAuto a = {0, 0, 0};
            assert(trim_then_present(&a, use, freed, 9000 * MB) && a.switches == 1);
        }
    for (use = 1000 * MB; use <= 9000 * MB; use += 499 * MB)
        for (freed = 0; freed <= use; freed += 307 * MB) {
            SnxAstcAuto a = {0, 0, 0};
            const unsigned long long left = snx_astc_trim_left(use + 512 * MB, 512 * MB, freed, 1);
            assert(snx_astc_auto_trimmed(&a, left, 9000 * MB) && !snx_astc_auto_room(&a, left, 9000 * MB));
        }
    /* nor does a sawtooth of trims: the caches filling from 7900 MB to past the 9000 MB mark, each trim taking them
       back to 7900 (a trim's own swing), 200 times: one switch. With no band
       (Exact again as soon as the caches are under the mark) each trim made two */
    {
        SnxAstcAuto a = {0, 0, 0}, none = {0, 0, 0};
        for (int i = 0; i < 200; i++) {
            for (use = 7900 * MB; use <= 9000 * MB; use += 100 * MB) {
                snx_astc_auto_room(&a, use, 9000 * MB);
                if (none.bc7 && use < 9000 * MB) none.bc7 = 0, none.switches++;
            }
            snx_astc_auto_trimmed(&a, 7900 * MB, 9000 * MB);
            if (!none.bc7) none.bc7 = 1, none.switches++;
        }
        assert(a.switches == 1 && a.bc7 && a.band == 1100 * MB && none.switches == 399);
        /* the game frees 2 GB (a scene left behind): more room than any trim left, Exact again */
        assert(snx_astc_auto_room(&a, 7900 * MB - 2048 * MB, 9000 * MB) && a.switches == 2);
    }
    /* Exact never, BC7 always (2 between frames, 3 at load), Auto as its state says; past 3 none of them */
    assert(!snx_astc_bc7(1, 0) && !snx_astc_bc7(1, 1));
    assert(snx_astc_bc7(2, 0) && snx_astc_bc7(2, 1));
    assert(!snx_astc_bc7(0, 0) && snx_astc_bc7(0, 1));
    assert(snx_astc_bc7(3, 0) && snx_astc_bc7(3, 1) && !snx_astc_bc7(4, 1));
    for (why = SNX_ASTC_SHORT_NO_MEMORY; why <= SNX_ASTC_SHORT_WORKING_SET; why++) {
        assert(snx_astc_bc_suggest(0, 1, 0, 300 * MB, why));
        assert(!snx_astc_bc_suggest(1, 1, 0, 300 * MB, why)); /* held as BC7 already, or Auto's own */
        assert(!snx_astc_bc_suggest(0, 1, 1, 300 * MB, why)); /* the GPU samples ASTC: nothing to encode again */
        assert(!snx_astc_bc_suggest(0, 0, 0, 300 * MB, why)); /* no BCn: BC7 can't be sampled */
        assert(!snx_astc_bc_suggest(0, 1, 0, 0, why));        /* no ASTC texture decoded: nothing it would hold */
    }
    assert(!snx_astc_bc_suggest(0, 1, 0, 300 * MB, SNX_ASTC_SHORT_NONE)); /* the images had room */
    /* the BC7 encoder's queue: the setting's thousands, else 256 thousand a 2 GiB of the GPU's own memory, from 256
       thousand (a GT 1030's 2 GB, or memory shared with the CPU) to 4 million (an RX 6700 XT's 12 GB gives 1.5) */
    assert(snx_bc7_queue_blocks(512, 12288 * MB, 0) == 512ull << 10);
    assert(snx_bc7_queue_blocks(0, 12288 * MB, 1) == 256ull << 10);
    assert(snx_bc7_queue_blocks(0, 2048 * MB, 0) == 256ull << 10 && snx_bc7_queue_blocks(0, 1024 * MB, 0) == 256ull
                                                                                                                 << 10);
    assert(snx_bc7_queue_blocks(0, 12288 * MB, 0) == 1536ull << 10);
    assert(snx_bc7_queue_blocks(0, 65536 * MB, 0) == 4096ull << 10);
    return 0;
}
