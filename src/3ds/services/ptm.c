/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/runtime/internal.h"
#include "3ds/services/fs.h"
#include "3ds/services/ptm.h"
#include "services/ptm.swipc.inc"
#include <stdio.h>
#include <string.h>
void snx_ctr_ptm_system_data(SnxCtrProcess* p) {
    /* Azahar's PTM: the archive formatted with an empty ArchiveFormatInfo, then gamecoin.dat holding its
       default_game_coin (magic 0x4F00, 42 coins, none today, no steps, 2014-12-29; 3dbrew's GameCoin layout) */
    static const unsigned char metadata[16] = {0};
    static const unsigned char coins[20] = {0x00, 0x4f, 0, 0, 42, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xde, 0x07, 12, 29};
    static const char path[] = "nand/data/00000000000000000000000000000000/extdata/00048000/f000000b/user/gamecoin.dat";
    pm_lock(&p->tree->lock);
    FsNode* node = NULL;
    if (fs_tree_lookup(p->tree, NULL, path, &node) == FS_TREE_NOTFOUND &&
        !snx_ctr_fs_make_extdata(p, true, 0xf0000000u, 0xf000000bu, metadata, NULL, 0) &&
        !fs_tree_create(p->tree, NULL, path, 0, sizeof coins, &node)) {
        FILE* f = fs_tree_open(p->tree, node, "r+b");
        if (f) {
            (void)fwrite(coins, sizeof coins, 1, f);
            fclose(f);
        }
    }
    pm_unlock(&p->tree->lock);
}
static uint32_t snx_ctr_ptm_CheckNew3DS(SnxCtrIpc* c, uint32_t* is_new) {
    *is_new = snx_ctr_memory_profile(c->process->memory)->model == SNX_CTR_NEW;
    return 0;
}
static uint32_t snx_ctr_ptm_ConfigureNew3DSCPU(SnxCtrIpc* c, uint32_t configuration) {
    /* A New 3DS's CPU modes: bit 0 its L2 cache, bit 1 its 804 MHz clock (3dbrew, ExHeader Flag1, which NS hands
       here). The game's code runs natively at the host's speed and the tick counter keeps its 268 MHz on the console
       too, so both are taken with nothing to change, and said once. An Old 3DS has neither, and another bit is unknown:
       those stop */
    bool new_3ds = snx_ctr_memory_profile(c->process->memory)->model == SNX_CTR_NEW;
    if (configuration && (!new_3ds || configuration > 3)) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return 0;
    }
    if (configuration && !c->process->cpu_configured) {
        printf("New 3DS CPU mode asked for:%s%s (the game runs at the host's speed)\n",
               configuration & 2 ? " 804 MHz" : "", configuration & 1 ? " L2 cache" : "");
        c->process->cpu_configured = 1;
    }
    return 0;
}
