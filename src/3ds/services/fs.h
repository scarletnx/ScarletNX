/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_FS_H
#define SNX_CTR_FS_H
#include "3ds/ipc/ctr_ipc.h"
#include "common/filesystem/tree.h"
#include <stddef.h>
void snx_ctr_fs_dispatch(SnxCtrIpc* c);
/* A new file or directory object answered as the request's moved handle (word 3): kind CTR_FILE_*, a FILE* host and
   an FsNode* node it retains. False when the reply already says why not */
/* An ExtSaveData made in the title's tree, a game's own (SD card) or a shared one (NAND): user/, boss/, the 16-byte
   ArchiveFormatInfo as metadata and an icon when icon_size isn't 0. Caller holds the tree's lock */
FsTreeResult snx_ctr_fs_make_extdata(struct SnxCtrProcess* p, bool shared, uint32_t high, uint32_t low,
                                     const unsigned char metadata[16], const void* icon, size_t icon_size);
bool snx_ctr_fs_file_object(SnxCtrIpc* c, uint32_t kind, uint32_t writable, uint32_t fixed, void* host, void* node);
/* the view of the DLC content index of title, opened when the game hasn't yet: 1 + its dlc_views entry, 0 when the
   game has no such content or its RomFS does not parse */
uint32_t snx_ctr_fs_dlc_view(struct SnxCtrProcess* p, uint64_t title, unsigned index);
#endif
