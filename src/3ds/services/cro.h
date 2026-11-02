/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Port of Azahar service/ldr_ro/cro_helper.h at a4b335e.
   Copyright 2016 Citra Emulator Project
   Licensed under GPLv2 or any later version. */
#ifndef SNX_CTR_CRO_H
#define SNX_CTR_CRO_H
#include <stdbool.h>
#include <stdint.h>
struct SnxCtrProcess;
/* Loader-only API: validate the complete source before begin. Keep all affected views mapped until good() succeeds.
   After unmapping a committed view, call finish immediately. No guest instruction runs during a transaction. */
bool snx_ctr_cro_begin(struct SnxCtrProcess* p, uint32_t module, uint32_t size, uint32_t buffer);
bool snx_ctr_cro_good(struct SnxCtrProcess* p);
bool snx_ctr_cro_finish(struct SnxCtrProcess* p, bool commit);
bool snx_ctr_cro_write(struct SnxCtrProcess* p, uint32_t address, const void* data, uint32_t size);
bool snx_ctr_cro_validate(struct SnxCtrProcess* p, uint32_t module, uint32_t size, bool crs);
/* The link of a CRO that LoadCRO or LoadCRO_New is loading. It needs the transaction that snx_ctr_cro_begin opened. */
bool snx_ctr_cro_link_load(struct SnxCtrProcess* p, uint32_t module, uint32_t crs, bool new_command);
/* A CRO (or the static module's CRS) at module_address, as Azahar's cro_helper.cpp rebases, links, unlinks,
   registers and fixes it. All addresses are guest virtual addresses, and the header starts at module_address + 0x80. */
bool snx_ctr_cro_rebase(struct SnxCtrProcess* p, uint32_t module, uint32_t size, uint32_t data, uint32_t data_size,
                        uint32_t bss, uint32_t bss_size, uint32_t crs);
/* the static module's CRS: its own segments already hold absolute addresses, so only its header and tables are
   rebased (Azahar's Rebase with is_crs true) */
bool snx_ctr_cro_rebase_crs(struct SnxCtrProcess* p, uint32_t module, uint32_t size);
bool snx_ctr_cro_link(struct SnxCtrProcess* p, uint32_t module, uint32_t crs);
bool snx_ctr_cro_unlink(struct SnxCtrProcess* p, uint32_t module, uint32_t crs);
void snx_ctr_cro_register(struct SnxCtrProcess* p, uint32_t module, uint32_t crs, bool auto_link);
void snx_ctr_cro_unregister(struct SnxCtrProcess* p, uint32_t module, uint32_t crs);
/* Azahar's ClearRelocations on a level-0 CRO, then Unrebase. CRS segments are already absolute and stay that way.
   Fixed CROs retain FIXD and their discarded tables; this does not reconstruct an original file byte for byte. */
bool snx_ctr_cro_unrebase(struct SnxCtrProcess* p, uint32_t module, bool crs);
/* Zeroes what a fix level leaves out, marks the header FIXD and returns the page-aligned size the module keeps. */
uint32_t snx_ctr_cro_fix(struct SnxCtrProcess* p, uint32_t module, uint32_t level);
/* the file offset and size of the .data image in the header of the CRO at module */
void snx_ctr_cro_data(struct SnxCtrProcess* p, uint32_t module, uint32_t* offset, uint32_t* size);
/* The pages of the code segment, where the process may execute it: size zero when the module has no code. False when
   those pages would end past the address space. */
bool snx_ctr_cro_code(struct SnxCtrProcess* p, uint32_t module, uint32_t* begin, uint32_t* size);
#endif
