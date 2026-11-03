/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   gsp::Gpu wire shapes and GX queue formats referenced against Azahar
   core/hle/service/gsp/ and libctru services/gspgpu.c. Bounded model: one
   shared GX/interrupt block per process, four relay threads, a checked GX/LCD
   register file and host-paced 60 Hz display timing. The GX queue's DMA, memory
   fills, display transfers and texture copies run on physical memory through the
   transfer engine (gpu/transfer.c), PICA command lists (SubmitCmdList) through the
   command processor (gpu/pica.c), which draws on the GPU (gpu/vk/render.c).
   The VRAM system area has nothing to save, since no applet here draws. */
#include "3ds/runtime/internal.h"
#include "common/utilities/endian.h"
#include "snx_settings.h"
#include "3ds/services/gsp.h"
#include "3ds/gpu/present.h"
#include "3ds/gpu/pica.h"
#include "3ds/gpu/transfer.h"
#include "3ds/gpu/vk/vk.h"
#include "3ds/gpu/vk/vk_transfer.h"
#include "3ds/gpu/vk/vk_render.h"
#include <stdio.h>
#include "common/utilities/common.h"
#include <stdlib.h>
#include <string.h>
#define GSP_SHARED_SIZE        0x1000u
#define GSP_ERR_INVALID_HANDLE 0xd8e007f7u
#define GSP_ERR_NO_RIGHT       0xd8202a06u
#define GSP_ERR_OOM            0xd86007f3u
#define GSP_ERR_REGS_RANGE     0xe0e02a01u
#define GSP_ERR_REGS_SIZE      0xe0e02becu
#define GSP_ERR_REGS_ALIGN     0xe0e02bf2u
#define GSP_FIRST_INIT         0x2a07u
#define GSP_SAVE_VRAM_ALLOWED  1u /* Azahar RelayEventQueueFlags::AllowSaveVramSysArea (RegisterInterruptRelayQueue) */
#define GSP_VBLANK_NS          UINT64_C(16666667)

static CtrSharedMap* shared_map(SnxCtrProcess* p, uint32_t object) {
    for (unsigned i = 0; i < 8; ++i)
        if (p->shared_maps[i].object == object) return &p->shared_maps[i];
    return NULL;
}
/* GSP_GPU::ProcessPendingInterruptImpl: one relay queue per registered thread in the shared block, a header
   (index, count, error, flags with ignore_pdc in bit 0), the missed PDC0 and PDC1 counters and 0x34 slots at
   0xc, which libctru's popInterrupt reads. A full queue sets the error and drops the interrupt. Vblanks (PDC0,
   PDC1) are dropped when the thread asked to ignore them, and only counted as missed once 0x20 are pending */
static void queue_interrupt(SnxCtrProcess* p, uint32_t thread, uint32_t id) {
    CtrSharedMap* map = shared_map(p, p->gsp.shared_object);
    if (!map) return;
    uint32_t at = map->va + thread * 0x40;
    unsigned char header[4];
    if (snx_ctr_read_ram(p->space, at, header, sizeof header) != SNX_CTR_MEM_OK) return;
    if (id == 2 || id == 3) {
        if (header[3] & 1) return;
        if (header[1] >= 0x20) {
            unsigned char missed[4];
            uint32_t counter = at + (id == 2 ? 4 : 8);
            if (snx_ctr_read_ram(p->space, counter, missed, 4) != SNX_CTR_MEM_OK) return;
            uint32_t n = snx_ld_le32(missed);
            ++n;
            snx_st_le32(missed, n);
            (void)snx_ctr_write_ram(p->space, counter, missed, 4);
            return;
        }
    }
    if (header[1] >= 0x34) {
        header[2] = 1;
        (void)snx_ctr_write_ram(p->space, at + 2, &header[2], 1);
        return;
    }
    unsigned next = (unsigned)(header[0] + header[1]) % 0x34;
    unsigned char slot = (unsigned char)id;
    if (snx_ctr_write_ram(p->space, at + 0xc + next, &slot, 1) != SNX_CTR_MEM_OK) return;
    ++header[1];
    (void)snx_ctr_write_ram(p->space, at + 1, &header[1], 1);
}
static void signal_registered(SnxCtrProcess* p, uint32_t thread) {
    uint32_t id = p->gsp.event_object[thread];
    if (!id) return;
    p->objects[id].data.event.signaled = 1;
    snx_ctr_wake_waiters(p, id);
}
static uint32_t screen_height(uint32_t screen) { return screen ? 320u : 400u; }
static uint32_t bpp_of(uint32_t format) { return format == 0 ? 4u : format == 1 ? 3u : 2u; }
/* A buffer swap, from SetBufferSwap or a shared FrameBufferUpdate block: info is FrameBufferInfo {active_fb,
   address_left, address_right, stride, format, shown_fb, unknown}. active_fb picks the register pair written,
   shown_fb (one bit of the register) the pair the LCD shows from now on. As GPU::SetBufferSwap in Azahar
   video_core/gpu.cpp (GPL-3.0-or-later). The right eye's address is not kept until stereoscopic output is (F4).
   Format and stride are checked when the frame is presented, whichever path set them. */
static void buffer_swap(SnxCtrProcess* p, uint32_t screen, const uint32_t* info) {
    p->gsp.fb_screen[screen].left[info[0] ? 1 : 0] = info[1];
    p->gsp.fb_screen[screen].stride = info[3];
    p->gsp.fb_screen[screen].format = info[4];
    p->gsp.fb_screen[screen].active = info[5] & 1;
    p->gsp.fb_pending[screen] = 1;
}
/* libctru gspPresentBuffer posts framebuffers into the shared FrameBufferUpdate
   blocks (0x200 + thread*0x80 + screen*0x40) instead of calling SetBufferSwap.
   Latch every dirty block at vblank and clear its update flag, as GSP does. */
static void latch_shared_framebuffers(SnxCtrProcess* p) {
    CtrSharedMap* map = p->gsp.shared_object ? shared_map(p, p->gsp.shared_object) : NULL;
    if (!map) return;
    for (uint32_t thread = 0; thread < CTR_GSP_THREADS; ++thread) {
        if (!(p->gsp.thread_mask & (1u << thread))) continue;
        for (uint32_t screen = 0; screen < 2; ++screen) {
            uint32_t base = map->va + 0x200 + thread * 0x80 + screen * 0x40;
            unsigned char header[4];
            if (snx_ctr_read_ram(p->space, base, header, sizeof header) != SNX_CTR_MEM_OK) continue;
            if (!(header[1] & 1)) continue; /* the update flag is bit 0 (libctru gspPresentBuffer) */
            unsigned slot = header[0] & 1;
            unsigned char info[28];
            if (snx_ctr_read_ram(p->space, base + 4 + slot * 28, info, sizeof info) != SNX_CTR_MEM_OK) continue;
            uint32_t words[7];
            for (unsigned i = 0; i < 7; ++i) {
                words[i] = 0;
                for (unsigned b = 0; b < 4; ++b) words[i] |= (uint32_t)info[i * 4 + b] << (b * 8);
            }
            buffer_swap(p, screen, words);
            header[1] = 0; /* applied: the guest may post the next update */
            (void)snx_ctr_write_ram(p->space, base, header, sizeof header);
        }
    }
}
/* Present one latched buffer swap: the left eye of the pair the LCD shows, as the software renderer's
   LoadFBToScreenInfo reads it, from guest RAM through the Vulkan presenter. A swap whose shown pair has no
   address, an unknown format, a stride too short for a row or memory the guest cannot read is dropped: the screen
   keeps its last frame instead of showing stale or invented pixels, and the next swap is tried afresh. */
static void present_screen(SnxCtrProcess* p, uint32_t screen) {
    uint32_t address = p->gsp.fb_screen[screen].left[p->gsp.fb_screen[screen].active];
    uint32_t stride = p->gsp.fb_screen[screen].stride, format = p->gsp.fb_screen[screen].format & 7;
    uint32_t height = screen_height(screen), width = 240;
    p->gsp.fb_pending[screen] = 0;
    if (g_settings.ctr_gpu_trace)
        fprintf(stderr, "3ds gpu trace: screen %u shows 0x%08x format %u stride %u%s\n", (unsigned)screen,
                (unsigned)address, (unsigned)format, (unsigned)stride,
                !address || format > 4 || stride < width * bpp_of(format) || stride > 0x40000 ? ": dropped" : "");
    if (!address || format > 4 || stride < width * bpp_of(format) || stride > 0x40000) return;
    /* the GPU reads it where it is: one physical span, which a framebuffer in VRAM or the linear heap is */
    uint64_t span = (uint64_t)stride * height;
    uint32_t pa = 0;
    for (uint64_t at = 0; at < span + 4096; at += 4096) {
        if (at >= span) at = span - 1; /* the span's last byte, whose page the steps may not reach */
        uint32_t page_pa;
        unsigned permissions;
        if (snx_ctr_query(p->space, address + (uint32_t)at, &page_pa, &permissions) != SNX_CTR_MEM_OK ||
            !(permissions & SNX_CTR_READ) || (at && page_pa != pa + (uint32_t)at))
            return;
        if (!at) pa = page_pa;
        if (at == span - 1) break;
    }
    if (!snx_ctr_present_frame(screen, pa, width, height, format, stride, NULL)) return;
    ++p->gsp.frame_seq;
}
/* Host-paced display timing: one vblank delivers both PDC interrupts to every
   registered relay thread and shows latched frames. Not cycle accurate. */
void snx_ctr_gsp_vblank(SnxCtrProcess* p) {
    if (!p->gsp.shared_object) return;
    ++p->gsp.vblank_count;
    /* the next vblank one period on; a display more than a period behind the host (a slow frame) starts again from
       now instead of delivering the missed vblanks back to back, as the DSP's frames and the timers do */
    uint64_t now = snx_ctr_now_ns();
    p->gsp.next_vblank_ns += GSP_VBLANK_NS;
    if (p->gsp.next_vblank_ns + GSP_VBLANK_NS < now) p->gsp.next_vblank_ns = now + GSP_VBLANK_NS;
    latch_shared_framebuffers(p);
    for (uint32_t screen = 0; screen < 2; ++screen)
        if (p->gsp.fb_pending[screen]) present_screen(p, screen);
    for (uint32_t thread = 0; thread < CTR_GSP_THREADS; ++thread) {
        if (!p->gsp.event_object[thread]) continue;
        queue_interrupt(p, thread, 2); /* PDC0 */
        queue_interrupt(p, thread, 3); /* PDC1 */
        signal_registered(p, thread);
    }
}
void snx_ctr_gsp_session_closed(SnxCtrProcess* p, uint32_t session) {
    if (p->gsp.rights_thread == session) p->gsp.rights_thread = UINT32_MAX;
    uint32_t thread = p->objects[session].data.session.gsp_thread;
    if (thread >= CTR_GSP_THREADS) return;
    if (p->gsp.event_object[thread]) {
        uint32_t event = p->gsp.event_object[thread];
        p->gsp.event_object[thread] = 0;
        snx_ctr_object_release(p, event);
    }
    p->gsp.relay_flags[thread] = 0;
    p->gsp.thread_mask &= ~(1u << thread);
}
/* the GPU right: the session that holds it (Azahar GSP_GPU::HasGpuRight) */
static bool has_right(SnxCtrIpc* c) { return c->process->gsp.rights_thread == c->object; }
/* one handle, copied or moved: what Azahar's PopObject takes */
static bool one_handle(const SnxCtrIpc* c, uint32_t word) { return !(c->in[word] & ~0x10u); }
/* WriteHWRegs and WriteHWRegsWithMask (Azahar GSP::WriteHWRegs*): the data's static buffer gives the size (the size
   word is ignored, as on hardware), checked after the right, the address below 0x420000, then at most 0x80 bytes
   in whole words. The runtime models no register a write here would change (framebuffers come from SetBufferSwap
   and the shared FrameBufferUpdate blocks, transfers from GX commands), so the values themselves are not kept. */
static uint32_t regs_check(SnxCtrIpc* c, uint32_t size) {
    if (!has_right(c)) return GSP_ERR_NO_RIGHT;
    if ((c->in[1] & 3) || c->in[1] >= 0x420000) return GSP_ERR_REGS_RANGE;
    if (size > 0x80) return GSP_ERR_REGS_SIZE;
    return size & 3 ? GSP_ERR_REGS_ALIGN : 0;
}
static void write_regs(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 2, 2)) return;
    if ((c->in[3] & 0x3fff) != 2) { /* static buffer 0, the data */
        snx_ctr_ipc_bad(c);
        return;
    }
    snx_ctr_ipc_error_reply(c, regs_check(c, c->in[3] >> 14));
}
static void write_regs_mask(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 2, 4)) return;
    /* static buffer 0 the data, 1 the mask (libctru GSPGPU_WriteHWRegsWithMask); a shorter mask reads as zeros */
    if ((c->in[3] & 0x3fff) != 2 || (c->in[5] & 0x3fff) != ((1u << 10) | 2)) {
        snx_ctr_ipc_bad(c);
        return;
    }
    snx_ctr_ipc_error_reply(c, regs_check(c, c->in[3] >> 14));
}
static void set_buffer_swap(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 8, 0)) return;
    SnxCtrProcess* p = c->process;
    /* words: screen, then FrameBufferInfo {active_fb, address_left,
       address_right, stride, format, shown_fb, unknown} */
    uint32_t screen = c->in[1], format = c->in[6] & 7;
    if (!has_right(c)) {
        snx_ctr_ipc_error_reply(c, GSP_ERR_NO_RIGHT);
        return;
    }
    /* Real hardware ignores an invalid screen and still answers success (Azahar gsp_gpu.cpp) */
    if (screen > 1) {
        c->out[0] = (c->in[0] & 0xffff0000u) | 0x40;
        return;
    }
    /* The five PICA framebuffer formats (RGBA8, RGB8, RGB565, RGB5A1, RGBA4) are what the presenter converts; the
       rest stop instead of being displayed with invented colors. */
    if (format > 4) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    buffer_swap(p, screen, &c->in[2]);
    /* A swap schedules the frame for the next real vblank; if display time has
       already passed while the guest ran, service it now. */
    if (p->gsp.shared_object && snx_ctr_now_ns() >= p->gsp.next_vblank_ns) snx_ctr_gsp_vblank(p);
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x40;
}
static void cache_op(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 2, 2)) return;
    /* Host memory is coherent with guest reads; a data cache flush/invalidate
       is a genuine no-op in this model. The process handle must be real. */
    if (!one_handle(c, 3)) {
        snx_ctr_ipc_bad(c);
        return;
    }
    if (c->handle_in[0] != CTR_PROCESS_HANDLE) {
        snx_ctr_ipc_error_reply(c, GSP_ERR_INVALID_HANDLE);
        return;
    }
    if ((uint64_t)c->in[1] + c->in[2] > 0x100000000ull) {
        snx_ctr_ipc_error_reply(c, GSP_ERR_REGS_RANGE);
        return;
    }
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x40;
}
static void register_relay(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 1, 2)) return;
    SnxCtrProcess* p = c->process;
    CtrObject* session = &p->objects[c->object];
    if (!one_handle(c, 2)) {
        snx_ctr_ipc_bad(c);
        return;
    }
    uint32_t event = snx_ctr_handle_object(p, c->handle_in[0]);
    if (!event || p->objects[event].type != CTR_OBJ_EVENT) {
        snx_ctr_ipc_error_reply(c, GSP_ERR_INVALID_HANDLE);
        return;
    }
    if (!p->gsp.shared_object) {
        uint32_t out = 0;
        uint32_t physical = snx_ctr_shared_pages(p, GSP_SHARED_SIZE);
        uint32_t result = physical ? snx_ctr_object_create(p, CTR_OBJ_SHARED, &out) : GSP_ERR_OOM;
        if (result) {
            snx_ctr_ipc_error_reply(c, result);
            return;
        }
        uint32_t id = snx_ctr_handle_object(p, out);
        p->objects[id].data.shared.physical = physical;
        p->objects[id].data.shared.size = GSP_SHARED_SIZE;
        p->objects[id].data.shared.permissions = 3;
        p->gsp.shared_object = id;
        p->gsp.next_vblank_ns = snx_ctr_now_ns() + GSP_VBLANK_NS;
        /* The block keeps its own reference while the process holds it. */
        ++p->objects[id].refs;
    }
    uint32_t thread = session->data.session.gsp_thread;
    if (thread == UINT32_MAX) {
        for (uint32_t i = 0; i < CTR_GSP_THREADS; ++i) {
            if (p->gsp.thread_mask & (1u << i)) continue;
            thread = i;
            p->gsp.thread_mask |= 1u << i;
            break;
        }
        if (thread == UINT32_MAX) {
            snx_ctr_ipc_error_reply(c, GSP_ERR_OOM);
            return;
        }
        session->data.session.gsp_thread = thread;
    }
    if (!p->gsp.event_object[thread]) {
        ++p->objects[event].refs;
        p->gsp.event_object[thread] = event;
        p->gsp.relay_flags[thread] = c->in[1];
    }
    uint32_t shared_handle = 0;
    uint32_t result = snx_ctr_handle_add(p, p->gsp.shared_object, &shared_handle);
    if (result) {
        snx_ctr_ipc_error_reply(c, result);
        return;
    }
    c->out[0] = 0x00130082;
    c->out[1] = p->gsp.first_registration ? 0 : GSP_FIRST_INIT;
    p->gsp.first_registration = 1;
    c->out[2] = thread;
    c->out[3] = 0x10;
    snx_ctr_ipc_output_handle(c, 4, shared_handle);
    if (c->state != SNX_CTR_IPC_RETURN) snx_ctr_ipc_drop_handle(c, shared_handle);
}
static void unregister_relay(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 0, 0)) return;
    SnxCtrProcess* p = c->process;
    uint32_t thread = p->objects[c->object].data.session.gsp_thread;
    if (thread != UINT32_MAX && p->gsp.event_object[thread]) {
        snx_ctr_object_release(p, p->gsp.event_object[thread]);
        p->gsp.event_object[thread] = 0;
    }
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x40;
}
static void acquire(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 1, 2)) return;
    SnxCtrProcess* p = c->process;
    if (!one_handle(c, 2)) {
        snx_ctr_ipc_bad(c);
        return;
    }
    if (c->handle_in[0] != CTR_PROCESS_HANDLE || c->in[1] > 7) {
        snx_ctr_ipc_error_reply(c, c->handle_in[0] != CTR_PROCESS_HANDLE ? GSP_ERR_INVALID_HANDLE : GSP_ERR_REGS_RANGE);
        return;
    }
    /* One bounded right owner: the acquiring session, as on hardware. */
    p->gsp.rights_thread = c->object;
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x40;
}
static void release_right(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 0, 0)) return;
    SnxCtrProcess* p = c->process;
    if (p->gsp.rights_thread != UINT32_MAX && p->gsp.rights_thread != c->object) {
        snx_ctr_ipc_error_reply(c, SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    p->gsp.rights_thread = UINT32_MAX;
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x40;
}
/* ImportDisplayCaptureInfo: the framebuffer each screen shows (address, format and stride, from the last swap).
   The right eye's address is not kept (see buffer_swap), so both addresses are the left eye's, which is what a
   screen with 3D off shows. */
static void import_capture_info(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 0, 0)) return;
    SnxCtrProcess* p = c->process;
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x240;
    for (uint32_t screen = 0; screen < 2; ++screen) {
        uint32_t at = 2 + screen * 4;
        uint32_t address = p->gsp.fb_screen[screen].left[p->gsp.fb_screen[screen].active];
        c->out[at] = address;
        c->out[at + 1] = address;
        c->out[at + 2] = p->gsp.fb_screen[screen].format;
        c->out[at + 3] = p->gsp.fb_screen[screen].stride;
    }
}
/* SaveVramSysArea and RestoreVramSysArea answer only a session that registered with the flag allowing them, as Azahar
   checks. The HLE applets draw nothing, so VRAM stays as the application left it: nothing to copy aside or back.
   The refusal uses GSP_ERR_NO_RIGHT, the result code of a missing GPU right: Azahar's ResultNoSaveVRAMSysAreaPerm
   and ResultNoGpuRight have the same fields. */
static void vram_sys_area(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 0, 0)) return;
    SnxCtrProcess* p = c->process;
    uint32_t thread = p->objects[c->object].data.session.gsp_thread;
    if (thread == UINT32_MAX || !(p->gsp.relay_flags[thread] & GSP_SAVE_VRAM_ALLOWED)) {
        snx_ctr_ipc_error_reply(c, GSP_ERR_NO_RIGHT);
        return;
    }
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x40;
}
/* ---- the GX command queue (Azahar GSP_GPU::TriggerCmdReqQueue and VideoCore::GPU::Execute, GPL-3.0-or-later,
   Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project, azahar-emu/azahar a4b335e) ---- */

/* interrupt sources, GSP InterruptId */
enum { GX_PSC0, GX_PSC1, GX_PDC0, GX_PDC1, GX_PPF, GX_P3D, GX_DMA };
#define GX_ERR_INVALID_BUFFER  0xe0e02bf5u
#define GX_ERR_UNKNOWN_COMMAND 0xe0c02bf4u
#define GX_STATUS_STOPPED      0x01u
#define GX_STATUS_FAILED       0x80u
/* the largest span one transfer stages in host memory */
#define GX_STAGE_LIMIT         (32u * 1024 * 1024)

/* GPU::VirtualToPhysicalAddress: the GSP module's own view of the application's buffers. The linear heaps and the
   New 3DS extra RAM must contain the start and the end, VRAM only the start; the ranges' ends are inclusive.
   0 when the buffer is outside GPU-visible memory, the value GSP writes to the registers then. */
static uint32_t gx_physical(uint32_t va, uint32_t size) {
    uint32_t end = va + size;
#define GX_IN(lo, hi) (va >= (lo) && va <= (hi) && end >= (lo) && end <= (hi))
    if (GX_IN(0x14000000u, 0x1c000000u)) return va - 0x14000000u + 0x20000000u;        /* linear heap -> FCRAM */
    if (GX_IN(0x30000000u, 0x40000000u)) return va - 0x30000000u + 0x20000000u;        /* New 3DS linear heap */
    if (va >= 0x1f000000u && va <= 0x1f600000u) return va - 0x1f000000u + 0x18000000u; /* VRAM */
    if (GX_IN(0x1e800000u, 0x1ec00000u)) return va - 0x1e800000u + 0x1f000000u;        /* New 3DS extra RAM */
#undef GX_IN
    return 0;
}

/* a physical address the transfer engine can start at: in a RAM, VRAM or DSP RAM region of the board */
static bool gx_valid(const SnxCtrProcess* p, uint32_t pa) {
    const SnxCtrRegion* r = pa ? snx_ctr_physical_region(snx_ctr_memory_profile(p->memory), pa, 1) : NULL;
    return r && r->kind != SNX_CTR_MMIO;
}

typedef struct {
    unsigned char ids[16];
    unsigned count;
} GxInterrupts;
static void gx_raise(GxInterrupts* irq, unsigned id) {
    if (irq->count < sizeof irq->ids) irq->ids[irq->count++] = (unsigned char)id;
}

/* the outcome of one command: done (with an error GSP stores, or 0), or the run stops here */
typedef enum { GX_DONE, GX_STOP } GxStep;

/* RequestDma: a copy between the application's virtual addresses, which the GSP module makes through the
   application's mapping but as a bus master (no CPU page permission applies: libctru DMAs textures into VRAM the
   CPU maps read-only). Each page is resolved to its physical address and copied through a staging buffer */
static bool gx_resolve(SnxCtrProcess* p, uint32_t va, uint32_t size, unsigned char* buffer, bool write) {
    for (uint32_t off = 0; off < size;) {
        uint32_t at = va + off, pa, n = 4096 - (at & 4095);
        if (n > size - off) n = size - off;
        if (snx_ctr_query(p->space, at, &pa, NULL) != SNX_CTR_MEM_OK) return false;
        SnxCtrMemResult r = write ? snx_ctr_phys_write(p->memory, pa, buffer + off, n)
                                  : snx_ctr_phys_read(p->memory, pa, buffer + off, n);
        if (r != SNX_CTR_MEM_OK) return false;
        off += n;
    }
    return true;
}
static GxStep gx_dma(SnxCtrIpc* c, const uint32_t* a, GxInterrupts* irq) {
    SnxCtrProcess* p = c->process;
    uint32_t source = a[0], dest = a[1], size = a[2];
    if (size > GX_STAGE_LIMIT || (uint64_t)source + size > 0x100000000ull ||
        (uint64_t)dest + size > 0x100000000ull) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return GX_STOP;
    }
    if (size) {
        unsigned char* staged = malloc(size);
        if (!staged) {
            snx_ctr_ipc_error_reply(c, GSP_ERR_OOM);
            return GX_STOP;
        }
        bool ok = gx_resolve(p, source, size, staged, false) && gx_resolve(p, dest, size, staged, true);
        free(staged);
        if (!ok) {
            c->state = SNX_CTR_IPC_FAULT;
            return GX_STOP;
        }
    }
    gx_raise(irq, GX_DMA);
    return GX_DONE;
}

/* MemoryFill: GPU::Execute writes each requested region's registers, GPU::MemoryFill runs it when its control
   word carries the trigger bit, from start to end rounded down to 8 bytes (the registers hold address / 8). With
   both regions requested only the second raises an interrupt, PSC0. The end is an open bound, so it may be the very
   end of its memory region (Azahar's GetPhysicalRef takes a region's end address as valid for this) */
static GxStep gx_fill_region(SnxCtrIpc* c, uint32_t start, uint32_t end, uint32_t value, uint32_t control,
                             int interrupt, GxInterrupts* irq) {
    SnxCtrProcess* p = c->process;
    uint32_t pa_start = start & ~7u, pa_end = end & ~7u;
    if (!(control & 1)) return GX_DONE; /* not triggered: no fill, no interrupt */
    if (pa_end > pa_start && gx_valid(p, pa_start) && gx_valid(p, pa_end - 1)) {
        /* on the GPU, in place (vk/transfer.c) */
        SnxCtrXferResult r = snx_ctr_vk_fill(pa_start, pa_end - pa_start, value, control);
        if (r != SNX_CTR_XFER_OK) { /* the span leaves its region, or there is no device */
            c->state = r == SNX_CTR_XFER_BOUNDS ? SNX_CTR_IPC_FAULT : SNX_CTR_IPC_UNSUPPORTED;
            return GX_STOP;
        }
    }
    /* "It seems that it won't signal interrupt if address_start is zero" */
    if (pa_start && interrupt >= 0) gx_raise(irq, (unsigned)interrupt);
    return GX_DONE;
}
static GxStep gx_fill(SnxCtrIpc* c, const uint32_t* a, uint32_t* error, GxInterrupts* irq) {
    uint32_t start1 = a[0], value1 = a[1], end1 = a[2], start2 = a[3], value2 = a[4], end2 = a[5];
    uint32_t control1 = a[6] & 0xffff, control2 = a[6] >> 16;
    bool both = start1 && start2;
    if (start1) {
        uint32_t pa = start1 < end1 ? gx_physical(start1, end1 - start1) : 0;
        if (!pa) {
            *error = GX_ERR_INVALID_BUFFER;
            return GX_DONE;
        }
        if (gx_fill_region(c, pa, pa + (end1 - start1), value1, control1, both ? -1 : GX_PSC0, irq) == GX_STOP)
            return GX_STOP;
    }
    if (start2) {
        uint32_t pa = start2 < end2 ? gx_physical(start2, end2 - start2) : 0;
        if (!pa) {
            *error = GX_ERR_INVALID_BUFFER;
            return GX_DONE;
        }
        return gx_fill_region(c, pa, pa + (end2 - start2), value2, control2, both ? GX_PSC0 : GX_PSC1, irq);
    }
    return GX_DONE;
}

/* GPU::MemoryTransfer over the transfer registers: a texture copy when the flags say so, a display transfer
   otherwise, then PPF. Addresses outside the board's memory do nothing (the blitter's check); an unimplemented
   mode stops instead of writing something the hardware would not */
static GxStep gx_transfer(SnxCtrIpc* c, GxInterrupts* irq) {
    SnxCtrProcess* p = c->process;
    SnxCtrGsp* g = &p->gsp;
    bool copy = (g->transfer.flags & SNX_CTR_XFER_TEXCOPY) != 0;
    SnxCtrDisplayTransfer display = {g->transfer.input_size, g->transfer.output_size, g->transfer.flags};
    SnxCtrTextureCopy texture = {g->transfer.copy_size, g->transfer.copy_input, g->transfer.copy_output};
    if (gx_valid(p, g->transfer.input) && gx_valid(p, g->transfer.output)) {
        /* on the GPU, in place (vk/transfer.c) */
        SnxCtrXferResult r = copy ? snx_ctr_vk_texture_copy(&texture, g->transfer.input, g->transfer.output)
                                  : snx_ctr_vk_display_transfer(&display, g->transfer.input, g->transfer.output);
        if (r == SNX_CTR_XFER_UNSUPPORTED) {
            c->state = SNX_CTR_IPC_UNSUPPORTED;
            return GX_STOP;
        }
        if (r == SNX_CTR_XFER_BOUNDS) { /* a span leaving its memory region */
            c->state = SNX_CTR_IPC_FAULT;
            return GX_STOP;
        }
    }
    gx_raise(irq, GX_PPF);
    return GX_DONE;
}

/* SubmitCmdList (GPU::Execute): GSP writes the list's physical address (0 outside GPU-visible memory) and size to
   the first command buffer channel and triggers it. A list GPU::SubmitCmdList finds outside memory raises P3D. The
   list's own interrupt requests raise P3D as it runs. A draw needing a part not ported yet stops the run */
static void gx_p3d(void* context) { gx_raise((GxInterrupts*)context, GX_P3D); }
static GxStep gx_submit(SnxCtrIpc* c, const uint32_t* a, GxInterrupts* irq) {
    SnxCtrGsp* g = &c->process->gsp;
    SnxCtrPicaHost host = {irq, gx_p3d, snx_ctr_vk_draw, snx_ctr_vk_vertex, snx_ctr_vk_vertices_end};
    uint32_t pa = gx_physical(a[0], a[1]);
    if (!g->pica) {
        g->pica = (SnxCtrPica*)malloc(sizeof *g->pica);
        if (!g->pica) {
            c->state = SNX_CTR_IPC_UNSUPPORTED;
            return GX_STOP;
        }
        snx_ctr_pica_init(g->pica);
    }
    g->pica->regs[SNX_PICA_CMDBUF_ADDR] = (pa >> 3) & 0x0fffffffu;
    g->pica->regs[SNX_PICA_CMDBUF_SIZE] = (a[1] >> 3) & 0xfffffu;
    g->pica->regs[SNX_PICA_CMDBUF_TRIGGER] = 1;
    SnxCtrPicaResult r = SNX_CTR_PICA_OK;
    if (pa)
        r = snx_ctr_pica_run(g->pica, c->process->memory, g->pica->regs[SNX_PICA_CMDBUF_ADDR] * 8,
                             g->pica->regs[SNX_PICA_CMDBUF_SIZE] * 8, &host);
    else
        gx_raise(irq, GX_P3D);
    g->pica->regs[SNX_PICA_CMDBUF_TRIGGER] = 0;
    if (r == SNX_CTR_PICA_OK) return GX_DONE;
    fprintf(stderr, "3ds gpu: the command list at 0x%08x stopped at register 0x%03x: %s\n", pa, g->pica->stop_register,
            r == SNX_CTR_PICA_FAULT ? "it jumps outside memory"
            : g->pica->stop_reason  ? g->pica->stop_reason
                                    : "it is larger than the staging bound");
    c->state = SNX_CTR_IPC_UNSUPPORTED;
    return GX_STOP;
}

/* TriggerCmdReqQueue: the right holder's queue in the shared block, a 0x20-byte header (index, count, status,
   should_stop, the last error) and 15 commands of 0x20 bytes. Each command is taken off the queue before it runs; a
   failing one leaves its error and the FAILED status and the queue goes on; its stop flag stops the queue after it.
   Interrupts go to the right holder's relay queue as the commands raise them. A command the runtime cannot finish
   (a PICA draw) stops the run */
static void trigger_queue(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 0, 0)) return;
    SnxCtrProcess* p = c->process;
    CtrSharedMap* map = p->gsp.shared_object ? shared_map(p, p->gsp.shared_object) : NULL;
    if (p->gsp.rights_thread == UINT32_MAX) {
        c->out[0] = (c->in[0] & 0xffff0000u) | 0x40; /* succeeds with no active queue */
        return;
    }
    uint32_t thread = p->objects[p->gsp.rights_thread].data.session.gsp_thread;
    if (!map || thread == UINT32_MAX || thread >= CTR_GSP_THREADS) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    unsigned char queue[0x200];
    uint32_t at = map->va + 0x800 + thread * 0x200, last_error = 0;
    if (snx_ctr_read_ram(p->space, at, queue, sizeof queue) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    memcpy(&last_error, queue + 4, 4);
    GxInterrupts irq = {{0}, 0};
    bool stopped = false;
    unsigned index = queue[0], pending = queue[1];
    while (pending) {
        if (queue[3]) { /* should_stop */
            queue[2] = GX_STATUS_STOPPED;
            break;
        }
        if (queue[2] == GX_STATUS_STOPPED) break;
        const unsigned char* cmd = queue + 0x20 + (index % 15) * 0x20;
        uint32_t id = cmd[0], args[7], error = 0;
        memcpy(args, cmd + 4, sizeof args);
        index = (index + 1) % 15;
        --pending;
        GxStep step = GX_DONE;
        SnxCtrGsp* g = &p->gsp;
        if (g_settings.ctr_gpu_trace) fprintf(stderr, "3ds gpu trace: GX command 0x%02x\n", (unsigned)id);
        switch (id) {
            case 0: step = gx_dma(c, args, &irq); break;
            case 1: step = gx_submit(c, args, &irq); break;
            case 2: step = gx_fill(c, args, &error, &irq); break;
            case 3: /* DisplayTransfer: the addresses must be GPU visible at their first byte */
                g->transfer.input = gx_physical(args[0], 1) & ~7u;
                g->transfer.output = gx_physical(args[1], 1) & ~7u;
                g->transfer.input_size = args[2];
                g->transfer.output_size = args[3];
                g->transfer.flags = args[4];
                step = gx_transfer(c, &irq);
                break;
            case 4: /* TextureCopy: the addresses must hold the whole size */
                g->transfer.input = gx_physical(args[0], args[2]) & ~7u;
                g->transfer.output = gx_physical(args[1], args[2]) & ~7u;
                g->transfer.copy_size = args[2];
                g->transfer.copy_input = args[3];
                g->transfer.copy_output = args[4];
                g->transfer.flags = args[5];
                step = gx_transfer(c, &irq);
                break;
            case 5: break; /* CacheFlush: host memory is coherent with the guest's view */
            default: error = GX_ERR_UNKNOWN_COMMAND; break;
        }
        if (step == GX_STOP) {
            stopped = true;
            break;
        }
        if (error) {
            last_error = error;
            queue[2] = GX_STATUS_FAILED;
        }
        if (cmd[2]) queue[2] = GX_STATUS_STOPPED; /* the command's stop flag */
    }
    /* The GPU's work submitted, not waited for, and the interrupts raised now: the GPU runs beside the guest, which
       write tracking keeps from seeing it unfinished (what it writes traps until it is done, what it reads traps
       writes until then). Not at the work's end, as the hardware raises them: the DSP and the vblanks keep the host's
       time, so a GPU slower than the console's would starve the guest's frame of its audio */
    snx_ctr_vk_submit();
    queue[0] = (unsigned char)index;
    queue[1] = (unsigned char)pending;
    memcpy(queue + 4, &last_error, 4);
    if (snx_ctr_write_ram(p->space, at, queue, 8) != SNX_CTR_MEM_OK) {
        c->state = SNX_CTR_IPC_FAULT;
        return;
    }
    for (unsigned i = 0; i < irq.count; ++i) queue_interrupt(p, thread, irq.ids[i]);
    if (irq.count) signal_registered(p, thread);
    if (!stopped) c->out[0] = (c->in[0] & 0xffff0000u) | 0x40;
}
void snx_ctr_gsp_dispatch(SnxCtrIpc* c) {
    switch (c->in[0] >> 16) {
        case 0x001: write_regs(c); break;
        case 0x002: write_regs_mask(c); break;
        case 0x005: set_buffer_swap(c); break;
        case 0x008:
        case 0x009: cache_op(c); break;
        case 0x00b:
            if (!snx_ctr_ipc_expect(c, 1, 0) || c->in[1] > 1) {
                if (c->state != SNX_CTR_IPC_RETURN) return;
                snx_ctr_ipc_error_reply(c, GSP_ERR_REGS_RANGE);
                return;
            }
            c->process->gsp.lcd_black = c->in[1];
            c->out[0] = (c->in[0] & 0xffff0000u) | 0x40;
            break;
        case 0x00c: trigger_queue(c); break;
        case 0x013: register_relay(c); break;
        case 0x014: unregister_relay(c); break;
        case 0x015:
        case 0x016: acquire(c); break;
        case 0x017: release_right(c); break;
        case 0x018: import_capture_info(c); break;
        case 0x019:
        case 0x01a: vram_sys_area(c); break;
        default: c->state = SNX_CTR_IPC_UNKNOWN; break;
    }
}
