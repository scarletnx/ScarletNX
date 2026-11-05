/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   am:app, as an application asks it about its add-on content, on a console with none installed: the answers of Azahar
   core/hle/service/am/{am.cpp,am_app.cpp} (GPL-2.0-or-later, Copyright 2014-2026 Citra Emulator Project / Azahar
   Emulator Project) at 49a1e8c for a DLC title without a TMD. An application asking for one of its DLC's titles
   is told there is one content info (Azahar's "number of content infos plus one"), lists none, and finds no title
   info; a title ID that is no DLC's is refused as the console refuses it. A DLC the game was given (--dlc) is
   answered from its TMD instead, in libctru's AM_ContentInfo and AM_TitleEntry layouts (24 bytes each). */
#include "3ds/services/am.h"
#include "3ds/runtime/internal.h"
#include "common/utilities/endian.h"
#include <string.h>

#define AM_ENTRY_SIZE 24u

#define AM_TID_HIGH_DLC          0x0004008cu
#define AM_ERR_INVALID_TID       0xe0e0801fu /* InvalidTID (31), AM, InvalidArgument, Usage */
#define AM_ERR_INVALID_TID_LIST  0xe0e0803cu /* InvalidTIDInList (60), AM, InvalidArgument, Usage */
#define AM_ERR_NOT_FOUND         0xd8a083fau /* NotFound, AM, InvalidState, Permanent */

static void reply(SnxCtrIpc* c, uint32_t normal, uint32_t translated, uint32_t result) {
    c->out[0] = (c->in[0] & 0xffff0000u) | normal << 6 | translated;
    c->out[1] = result;
}
/* the mapped buffers of the request, echoed in the reply after its normal words */
static void buffers(SnxCtrIpc* c, uint32_t first, uint32_t count, uint32_t normal) {
    for (uint32_t i = 0; i < count * 2; ++i) c->out[1 + normal + i] = c->in[first + i];
}
/* the DLC title the game was given with this ID, if any */
static bool given(SnxCtrIpc* c, uint64_t title, SnxCtrDlcTitle* out) {
    return c->process->source && snx_ctr_source_dlc_find(c->process->source, title, out);
}
void snx_ctr_am_dispatch(SnxCtrIpc* c) {
    switch (c->in[0] >> 16) {
        case 0x1001: /* GetDLCContentInfoCount(u8 media, u64 title) */
            if (!snx_ctr_ipc_expect(c, 3, 0)) return;
            SnxCtrDlcTitle title;
            reply(c, 2, 0, c->in[3] == AM_TID_HIGH_DLC ? 0 : AM_ERR_INVALID_TID);
            /* the TMD's content count, one without a TMD */
            c->out[2] = given(c, (uint64_t)c->in[3] << 32 | c->in[2], &title) ? title.count : c->in[3] == AM_TID_HIGH_DLC;
            return;
        case 0x1003: /* ListDLCContentInfos(u32 count, u8 media, u64 title, u32 start, buffer out): none */
            if (!snx_ctr_ipc_expect(c, 5, 2)) return;
            SnxCtrDlcTitle listed;
            uint64_t id = (uint64_t)c->in[4] << 32 | c->in[3];
            if (given(c, id, &listed)) {
                uint32_t want = c->in[1], start = c->in[5], room = c->in[6] >> 4, done = 0;
                for (uint32_t i = start; i < listed.count && done < want && (done + 1) * AM_ENTRY_SIZE <= room; ++i) {
                    SnxCtrDlcContent content;
                    unsigned char entry[AM_ENTRY_SIZE] = {0};
                    if (!snx_ctr_source_dlc_content(c->process->source, id, i, &content)) break;
                    snx_st_le16(entry, content.index);
                    snx_st_le16(entry + 2, content.type);
                    snx_st_le32(entry + 4, content.id);
                    snx_st_le64(entry + 8, content.size);
                    entry[16] = 3; /* downloaded and owned */
                    if (snx_ctr_write_ram(c->process->space, c->in[7] + done * AM_ENTRY_SIZE, entry, sizeof entry) !=
                        SNX_CTR_MEM_OK) {
                        c->transport_result = 0xe0e01bf5u;
                        return;
                    }
                    ++done;
                }
                reply(c, 2, 2, 0);
                c->out[2] = done;
                buffers(c, 6, 1, 2);
                return;
            }
            reply(c, 2, 0, c->in[4] == AM_TID_HIGH_DLC ? 0 : AM_ERR_INVALID_TID);
            c->out[2] = 0;
            return;
        case 0x1005: { /* GetDLCTitleInfos(u8 media, u32 count, buffer titles, buffer infos out) */
            if (!snx_ctr_ipc_expect(c, 2, 4)) return;
            uint32_t count = c->in[2], result = count ? AM_ERR_NOT_FOUND : 0;
            bool all_given = count && count <= 256;
            for (uint32_t i = 0; i < count && i < 256; ++i) {
                unsigned char title[8];
                if (snx_ctr_read_ram(c->process->space, c->in[4] + i * 8, title, 8) != SNX_CTR_MEM_OK) {
                    c->transport_result = 0xe0e01bf5u;
                    return;
                }
                uint32_t high = snx_ld_le32(title + 4);
                if (high != AM_TID_HIGH_DLC) {
                    result = AM_ERR_INVALID_TID_LIST;
                    all_given = false;
                    break;
                }
                SnxCtrDlcTitle known;
                all_given = all_given && given(c, snx_ld_le64(title), &known);
            }
            if (all_given) { /* every title is one the game was given: their infos, in the order asked */
                for (uint32_t i = 0; i < count; ++i) {
                    unsigned char id[8], entry[AM_ENTRY_SIZE] = {0};
                    SnxCtrDlcTitle known;
                    (void)snx_ctr_read_ram(c->process->space, c->in[4] + i * 8, id, 8);
                    (void)given(c, snx_ld_le64(id), &known);
                    snx_st_le64(entry, known.title);
                    snx_st_le64(entry + 8, known.size);
                    snx_st_le16(entry + 16, known.version);
                    if (snx_ctr_write_ram(c->process->space, c->in[6] + i * AM_ENTRY_SIZE, entry, sizeof entry) !=
                        SNX_CTR_MEM_OK) {
                        c->transport_result = 0xe0e01bf5u;
                        return;
                    }
                }
                result = 0;
            }
            reply(c, 1, 4, result);
            buffers(c, 3, 2, 1);
            return;
        }
        case 0x1007: /* ListDataTitleTicketInfos(u32 count, u64 title, u32 start, buffer out): none */
            if (!snx_ctr_ipc_expect(c, 4, 2)) return;
            if (c->in[3] != AM_TID_HIGH_DLC) {
                reply(c, 1, 0, AM_ERR_INVALID_TID_LIST);
                return;
            }
            reply(c, 2, 0, 0);
            c->out[2] = 0;
            return;
        default: c->state = SNX_CTR_IPC_UNKNOWN; return;
    }
}
