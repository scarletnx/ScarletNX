/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   End-to-end gsp::Gpu/Vulkan test: real service requests, shared GX queue,
   checked register writes, memory fills and a real Vulkan presentation of a
   guest framebuffer to a PPM snapshot. No GPU emulation is faked; when no
   Vulkan device exists the presentation half stops (checked here explicitly). */
#include "3ds/runtime/internal.h"
#include "3ds/ipc/ctr_ipc.h"
#include "3ds/services/gsp.h"
#include "3ds/gpu/present.h"
#include "common/filesystem/host.h"
#include "common/utilities/common.h"
#include "support/3ds_guest.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QUEUE 0x14000000u
static uint64_t fnv(const unsigned char* bytes, size_t n) {
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}
/* PPM pixels of the landscape screen a width x height RGBA framebuffer shows: the saved image has height columns
   and width rows, screen (x, y) being framebuffer row x, pixel width - 1 - y (libctru's console addressing). */
static void ppm_pixel(const unsigned char* ppm, size_t size, unsigned width, unsigned height, unsigned x, unsigned y,
                      unsigned char out[3]) {
    const char* header = "P6\n400 240\n255\n";
    (void)width;
    (void)height;
    size_t at = strlen(header) + ((size_t)y * 400 + x) * 3;
    assert(at + 3 <= size);
    memcpy(out, ppm + at, 3);
}
/* one pixel of a saved top-screen snapshot, frame-0-<index>.ppm */
static void frame_pixel(const char* dir, unsigned index, unsigned x, unsigned y, unsigned char out[3]) {
    char path[1024];
    snprintf(path, sizeof path, "%s/frame-0-%02u.ppm", dir, index);
    FILE* f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* ppm = malloc((size_t)length);
    assert(ppm && length > 0 && fread(ppm, 1, (size_t)length, f) == (size_t)length);
    fclose(f);
    ppm_pixel(ppm, (size_t)length, 240, 400, x, y, out);
    free(ppm);
}
/* a whole RGBA8 (A, B, G, R) top framebuffer of one colour */
static void fill_rgba8(SnxCtrProcess* p, uint32_t address, unsigned char r, unsigned char g, unsigned char b) {
    unsigned char* pixels = malloc(240u * 400 * 4);
    assert(pixels);
    for (unsigned i = 0; i < 240u * 400; ++i) {
        pixels[i * 4 + 0] = 255;
        pixels[i * 4 + 1] = b;
        pixels[i * 4 + 2] = g;
        pixels[i * 4 + 3] = r;
    }
    assert(snx_ctr_write_ram(p->space, address, pixels, 240u * 400 * 4) == SNX_CTR_MEM_OK);
    free(pixels);
}
int main(int argc, char** argv) {
    assert(argc == 2);

    (void)plat_mkdir(argv[1]);
    /* a fresh frame sequence every run */
    for (unsigned screen = 0; screen < 2; ++screen)
        for (unsigned i = 0; i < 8; ++i) {
            char stale[1024];
            snprintf(stale, sizeof stale, "%s/frame-%u-%02u.ppm", argv[1], screen, i);
            (void)remove(stale);
        }
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = ctr_image(code, sizeof code, 0x3000);
    for (unsigned model = 0; model < 2; ++model) {
        SnxCtrProcess* p = snx_ctr_process_new(&image, (SnxCtrModel)model);
        assert(p);
        if (!snx_ctr_present_init(p->memory, argv[1])) {
            assert(!model);
            printf("no Vulkan device: presentation checks skipped\n");
            snx_ctr_process_free(p);
            return 77;
        }
        uint32_t gsp;
        assert(!snx_ctr_session_create(p, SNX_CTR_SERVICE_GSP, &gsp));
        uint32_t event = 0;
        assert(!snx_ctr_object_create(p, CTR_OBJ_EVENT, &event));
        /* libctru's gspInit takes the GPU right before it registers its relay queue, and keeps it */
        assert(p->gsp.rights_thread == UINT32_MAX);
        uint32_t acquire[] = {0x00160042, 0, 0, CTR_PROCESS_HANDLE};
        ctr_call(p, gsp, acquire, 4, SNX_CTR_SVC_RETURN, 0);
        assert(!ctr_command_word(p, 1));
        uint32_t register_relay[] = {0x00130042, 1, 0, event};
        ctr_call(p, gsp, register_relay, 4, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 0) == 0x00130082 && ctr_command_word(p, 1) == 0x2a07 &&
               ctr_command_word(p, 2) == 0 && ctr_command_word(p, 3) == 0x10);
        assert(p->gsp.rights_thread == snx_ctr_handle_object(p, gsp));
        uint32_t shared = ctr_command_word(p, 4);
        assert(snx_ctr_handle_object(p, shared) && !snx_ctr_handle_object(p, shared ^ 1u));
        /* A second registration reports success, not another first-init. */
        ctr_call(p, gsp, register_relay, 4, SNX_CTR_SVC_RETURN, 0);
        assert(!ctr_command_word(p, 1));
        /* MapMemoryBlock: the 0x1000 GX block at a free mappable address. */
        p->cpu.state.cpu.r[0] = shared;
        p->cpu.state.cpu.r[1] = QUEUE;
        p->cpu.state.cpu.r[2] = 3;
        p->cpu.state.cpu.r[3] = 0x10000000;
        assert(snx_ctr_svc(p, 0x1f) == SNX_CTR_SVC_RETURN && !p->cpu.state.cpu.r[0]);
        /* QueryMemory reports the registered mapping as shared, in registers as libctru's svcQueryMemory reads
           them: base r1, size r2, permissions r3, state r4, page flags r5 */
        uint32_t* q = p->cpu.state.cpu.r;
        q[0] = 0x300000;
        q[1] = 0x300010;
        q[2] = QUEUE + 0x10;
        q[5] = 0xdeadbeef;
        assert(snx_ctr_svc(p, 0x02) == SNX_CTR_SVC_RETURN && !q[0]);
        assert(q[1] == QUEUE && q[2] == 0x1000 && q[3] == 3 && q[4] == 6 && !q[5]);
        q[2] = 0x100000; /* the code segment: Code (4), read and execute */
        assert(snx_ctr_svc(p, 0x02) == SNX_CTR_SVC_RETURN && !q[0] && q[1] == 0x100000 && q[3] == 5 && q[4] == 4);
        q[2] = 0x1ff81000; /* the shared page: Shared, read-only */
        assert(snx_ctr_svc(p, 0x02) == SNX_CTR_SVC_RETURN && !q[0] && q[3] == 1 && q[4] == 6);
        /* GPU right + process handle check. */
        ctr_call(p, gsp, acquire, 4, SNX_CTR_SVC_RETURN, 0);
        assert(!ctr_command_word(p, 1));
        acquire[3] = 0x1234;
        ctr_call(p, gsp, acquire, 4, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 1) == 0xd8e007f7);
        acquire[3] = CTR_PROCESS_HANDLE;
        /* Register writes as libctru's GSPGPU_WriteHWRegs and WriteHWRegsWithMask send them, checked as Azahar
           checks them: the right, then an address below 0x420000, then at most 0x80 bytes (the static buffer's
           size, not the size word), then whole words */
        uint32_t hw[] = {0x00010082, 0x400468, 8, (8u << 14) | 2, 0x300200};
        ctr_put(p, 0x300200, 0x01c501c1);
        ctr_put(p, 0x300204, 0x000001c1);
        ctr_call(p, gsp, hw, 5, SNX_CTR_SVC_RETURN, 0);
        assert(!ctr_command_word(p, 1));
        hw[2] = 0;
        ctr_call(p, gsp, hw, 5, SNX_CTR_SVC_RETURN, 0);
        assert(!ctr_command_word(p, 1));
        hw[1] = 0x420000;
        ctr_call(p, gsp, hw, 5, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 1) == 0xe0e02a01);
        hw[1] = 0x40046a;
        ctr_call(p, gsp, hw, 5, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 1) == 0xe0e02a01);
        hw[1] = 0x400468;
        hw[3] = (0x84u << 14) | 2;
        ctr_call(p, gsp, hw, 5, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 1) == 0xe0e02bec);
        hw[3] = (6u << 14) | 2;
        ctr_call(p, gsp, hw, 5, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 1) == 0xe0e02bf2);
        hw[3] = (8u << 14) | 2;
        /* the mask is static buffer 1, and may be shorter than the data */
        uint32_t masked[] = {0x00020084, 0x400468, 8, (8u << 14) | 2, 0x300200, (4u << 14) | (1u << 10) | 2, 0x300200};
        ctr_call(p, gsp, masked, 7, SNX_CTR_SVC_RETURN, 0);
        assert(!ctr_command_word(p, 1));
        masked[5] = (8u << 14) | (2u << 10) | 2;
        ctr_call(p, gsp, masked, 7, SNX_CTR_SVC_RETURN, SNX_CTR_IPC_BAD_DESCRIPTOR);
        masked[5] = (4u << 14) | (1u << 10) | 2;
        /* without the GPU right, writes and buffer swaps fail with GX's NoPermission */
        uint32_t release[] = {0x00170000};
        ctr_call(p, gsp, release, 1, SNX_CTR_SVC_RETURN, 0);
        assert(!ctr_command_word(p, 1));
        ctr_call(p, gsp, hw, 5, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 1) == 0xd8202a06);
        ctr_call(p, gsp, masked, 7, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 1) == 0xd8202a06);
        uint32_t no_right_swap[] = {0x00050200, 0, 0, 0x300200, 0x300200, 960, 0, 0, 0};
        ctr_call(p, gsp, no_right_swap, 9, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 1) == 0xd8202a06);
        ctr_call(p, gsp, acquire, 4, SNX_CTR_SVC_RETURN, 0);
        assert(!ctr_command_word(p, 1));
        /* the process handle must come in a handle descriptor */
        acquire[2] = 0x20;
        ctr_call(p, gsp, acquire, 4, SNX_CTR_SVC_RETURN, SNX_CTR_IPC_BAD_DESCRIPTOR);
        acquire[2] = 0;
        /* MemoryFill through the real shared GX queue. */
        uint32_t target = 0;
        assert(!snx_ctr_kernel_memory(p, 0x10003, 0, 0, 0x1000, 3, &target) && target);
        ctr_put(p, QUEUE + 0x800, 0x100); /* one command queued */
        ctr_put(p, QUEUE + 0x820, 2);     /* MemoryFill */
        ctr_put(p, QUEUE + 0x824, target);
        ctr_put(p, QUEUE + 0x828, 0x11223344);
        ctr_put(p, QUEUE + 0x82c, target + 0x1000);
        ctr_put(p, QUEUE + 0x830, 0);
        ctr_put(p, QUEUE + 0x834, 0);
        ctr_put(p, QUEUE + 0x838, 0);
        ctr_put(p, QUEUE + 0x83c, 0x0201); /* control1: GX_FILL_TRIGGER | GX_FILL_32BIT_DEPTH, as libctru sets it */
        ctr_put(p, QUEUE, 0);              /* relay queue of thread 0: empty */
        uint32_t trigger[] = {0x000c0000};
        ctr_call(p, gsp, trigger, 1, SNX_CTR_SVC_RETURN, 0);
        assert(!ctr_command_word(p, 1));
        for (uint32_t at = target; at < target + 0x1000; at += 4) {
            unsigned char b[4];
            assert(snx_ctr_read_ram(p->space, at, b, 4) == SNX_CTR_MEM_OK);
            assert(b[0] == 0x44 && b[1] == 0x33 && b[2] == 0x22 && b[3] == 0x11);
        }
        /* one region filled: exactly one PSC0 in the relay queue, and the GX queue drained */
        assert((ctr_word(p, QUEUE) & 0xffff) == 0x0100 && (ctr_word(p, QUEUE + 12) & 0xff) == 0);
        assert((ctr_word(p, QUEUE + 0x800) & 0xff00) == 0);
        /* an untriggered fill (control without bit 0) writes nothing and raises nothing */
        ctr_put(p, QUEUE, 0);
        ctr_put(p, QUEUE + 0x800, 0x100 | 1); /* index 1, one command */
        ctr_put(p, QUEUE + 0x840, 2);
        ctr_put(p, QUEUE + 0x844, target);
        ctr_put(p, QUEUE + 0x848, 0x55555555);
        ctr_put(p, QUEUE + 0x84c, target + 0x100);
        ctr_put(p, QUEUE + 0x850, 0);
        ctr_put(p, QUEUE + 0x854, 0);
        ctr_put(p, QUEUE + 0x858, 0);
        ctr_put(p, QUEUE + 0x85c, 0x0200);
        ctr_call(p, gsp, trigger, 1, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_word(p, target) == 0x11223344 && (ctr_word(p, QUEUE) & 0xff00) == 0);
        /* DisplayTransfer of an 8x8 tiled RGBA8 image from the linear heap into VRAM (0x1f000000, physical
           0x18000000) as linear RGB8. The GPU writes physical memory, the process need not map VRAM */
        {
            static const unsigned row7[8] = {0, 1, 4, 5, 16, 17, 20, 21}; /* the Morton tile's bottom row */
            unsigned char px[4] = {0xff, 0x80, 0, 0}, out[8 * 8 * 3];
            for (unsigned y = 0; y < 8; ++y)
                for (unsigned x = 0; x < 8; ++x) {
                    /* texel index of (x, y): the bottom row's pattern, plus 2 for odd y, 8 for y&2, 32 for y&4 */
                    unsigned t = row7[x] + ((y & 1) ? 2 : 0) + ((y & 2) ? 8 : 0) + ((y & 4) ? 32 : 0);
                    px[2] = (unsigned char)y;
                    px[3] = (unsigned char)x;
                    assert(snx_ctr_write_ram(p->space, target + 0x200 + t * 4, px, 4) == SNX_CTR_MEM_OK);
                }
            unsigned char zero[8 * 8 * 3] = {0};
            assert(snx_ctr_phys_write(p->memory, 0x18000000u, zero, sizeof zero) == SNX_CTR_MEM_OK);
            ctr_put(p, QUEUE, 0);
            ctr_put(p, QUEUE + 0x800, 0x100 | 2);
            ctr_put(p, QUEUE + 0x860, 3);
            ctr_put(p, QUEUE + 0x864, target + 0x200);
            ctr_put(p, QUEUE + 0x868, 0x1f000000u);
            ctr_put(p, QUEUE + 0x86c, 8 | (8u << 16));
            ctr_put(p, QUEUE + 0x870, 8 | (8u << 16));
            ctr_put(p, QUEUE + 0x874, (0u << 8) | (1u << 12)); /* RGBA8 in, RGB8 out, tiled to linear */
            ctr_call(p, gsp, trigger, 1, SNX_CTR_SVC_RETURN, 0);
            assert(!ctr_command_word(p, 1));
            assert(snx_ctr_phys_read(p->memory, 0x18000000u, out, sizeof out) == SNX_CTR_MEM_OK);
            for (unsigned y = 0; y < 8; ++y)
                for (unsigned x = 0; x < 8; ++x) {
                    const unsigned char* q = out + (y * 8 + x) * 3;
                    assert(q[0] == 0x80 && q[1] == y && q[2] == x);
                }
            /* PPF, once */
            assert((ctr_word(p, QUEUE) & 0xffff) == 0x0100 && (ctr_word(p, QUEUE + 12) & 0xff) == 4);
        }
        /* TextureCopy with an input gap, then an invalid fill and an unknown command: GSP stores the errors, sets
           the FAILED status and keeps going */
        {
            unsigned char line[64];
            for (unsigned i = 0; i < 64; ++i) line[i] = (unsigned char)i;
            assert(snx_ctr_write_ram(p->space, target + 0x400, line, 64) == SNX_CTR_MEM_OK);
            ctr_put(p, QUEUE, 0);
            ctr_put(p, QUEUE + 0x800, 0x300 | 3); /* three commands from index 3 */
            ctr_put(p, QUEUE + 0x880, 4);
            ctr_put(p, QUEUE + 0x884, target + 0x400);
            ctr_put(p, QUEUE + 0x888, target + 0x600);
            ctr_put(p, QUEUE + 0x88c, 32);
            ctr_put(p, QUEUE + 0x890, 1 | (1u << 16)); /* 16-byte lines, 16-byte gaps */
            ctr_put(p, QUEUE + 0x894, 0);
            ctr_put(p, QUEUE + 0x898, 8); /* the texture copy flag */
            ctr_put(p, QUEUE + 0x8a0, 2); /* MemoryFill outside GPU-visible memory */
            ctr_put(p, QUEUE + 0x8a4, 0x00100000);
            ctr_put(p, QUEUE + 0x8a8, 0);
            ctr_put(p, QUEUE + 0x8ac, 0x00100100);
            ctr_put(p, QUEUE + 0x8c0, 7); /* no such command */
            ctr_call(p, gsp, trigger, 1, SNX_CTR_SVC_RETURN, 0);
            assert(!ctr_command_word(p, 1));
            unsigned char copied[32];
            assert(snx_ctr_read_ram(p->space, target + 0x600, copied, 32) == SNX_CTR_MEM_OK);
            for (unsigned i = 0; i < 32; ++i) assert(copied[i] == (i < 16 ? i : i + 16));
            uint32_t header = ctr_word(p, QUEUE + 0x800);
            assert((header & 0xff) == 6 && ((header >> 8) & 0xff) == 0 && ((header >> 16) & 0xff) == 0x80);
            assert(ctr_word(p, QUEUE + 0x804) == 0xe0c02bf4u); /* the last error: the unknown command's */
            assert((ctr_word(p, QUEUE) & 0xffff) == 0x0100 &&
                   (ctr_word(p, QUEUE + 12) & 0xff) == 4); /* only the copy's PPF */
            ctr_put(p, QUEUE + 0x800, 0);                  /* status cleared for what follows */
        }
        /* SubmitCmdList: a list in the linear heap whose interrupt request matches irq_compare's low byte raises
           P3D, then a list outside GPU-visible memory raises it without running */
        ctr_put(p, target + 0x800, 0x78);
        ctr_put(p, target + 0x804, 0x000f0010u); /* irq_request, all bytes */
        ctr_put(p, QUEUE, 0);
        ctr_put(p, QUEUE + 0x800, 0x200);
        ctr_put(p, QUEUE + 0x820, 1);
        ctr_put(p, QUEUE + 0x824, target + 0x800);
        ctr_put(p, QUEUE + 0x828, 8);
        ctr_put(p, QUEUE + 0x840, 1);
        ctr_put(p, QUEUE + 0x844, 0x00100000);
        ctr_put(p, QUEUE + 0x848, 8);
        ctr_call(p, gsp, trigger, 1, SNX_CTR_SVC_RETURN, 0);
        assert(!ctr_command_word(p, 1));
        assert((ctr_word(p, QUEUE + 0x800) & 0xffff) == 2);
        assert((ctr_word(p, QUEUE) & 0xffff) == 0x0200 && (ctr_word(p, QUEUE + 12) & 0xffff) == 0x0505);
        /* a draw needing a part not ported yet (a geometry shader) stops the run */
        ctr_put(p, target + 0x800, 2);
        ctr_put(p, target + 0x804, 0x000f0229u); /* use_gs */
        ctr_put(p, target + 0x808, 1);
        ctr_put(p, target + 0x80c, 0x000f022eu); /* trigger_draw */
        ctr_put(p, QUEUE + 0x800, 0x100);
        ctr_put(p, QUEUE + 0x820, 1);
        ctr_put(p, QUEUE + 0x828, 16);
        ctr_call(p, gsp, trigger, 1, SNX_CTR_SVC_UNSUPPORTED, 0);
        ctr_put(p, QUEUE + 0x800, 0);
        /* vblanks reach the relay queue's slots at 0xc (libctru's popInterrupt), are dropped with ignore_pdc set,
           and only counted as missed once 0x20 interrupts are pending */
        ctr_put(p, QUEUE, 0);
        snx_ctr_gsp_vblank(p);
        assert((ctr_word(p, QUEUE) & 0xffff) == 0x0200 && (ctr_word(p, QUEUE + 12) & 0xffff) == 0x0302);
        ctr_put(p, QUEUE, 0x01000000);
        snx_ctr_gsp_vblank(p);
        assert((ctr_word(p, QUEUE) & 0xffff) == 0);
        ctr_put(p, QUEUE, 0x2000);
        ctr_put(p, QUEUE + 4, 0);
        ctr_put(p, QUEUE + 8, 0);
        snx_ctr_gsp_vblank(p);
        assert(ctr_word(p, QUEUE + 4) == 1 && ctr_word(p, QUEUE + 8) == 1 && (ctr_word(p, QUEUE) & 0xffff) == 0x2000);
        ctr_put(p, QUEUE, 0);
        ctr_put(p, QUEUE + 4, 0);
        ctr_put(p, QUEUE + 8, 0);
        /* SetBufferSwap: a real RGBA8 framebuffer with a distinctive pattern. */
        uint32_t fb = 0;
        assert(!snx_ctr_kernel_memory(p, 0x10003, 0, 0, 0x60000, 3, &fb) && fb);
        unsigned char* pattern = malloc(240u * 400 * 4);
        assert(pattern);
        for (unsigned y = 0; y < 400; ++y)
            for (unsigned x = 0; x < 240; ++x) {
                unsigned char* px = pattern + ((size_t)y * 240 + x) * 4;
                /* RGBA8 is stored A, B, G, R */
                px[3] = (unsigned char)x;
                px[2] = (unsigned char)y;
                px[1] = (unsigned char)(model + 1);
                px[0] = 255;
            }
        assert(snx_ctr_write_ram(p->space, fb, pattern, 240u * 400 * 4) == SNX_CTR_MEM_OK);
        uint32_t swap[] = {0x00050200, 0, 0, fb, fb, 960, 0, 0, 0};
        ctr_call(p, gsp, swap, 9, SNX_CTR_SVC_RETURN, 0);
        assert(!ctr_command_word(p, 1));
        snx_ctr_gsp_vblank(p);
        if (!model) {
            /* The saved frame is the real Vulkan readback, rotated like the
               console screen: 400x240 landscape. Corners come from the exact
               framebuffer pattern. */
            char path[1024];
            snprintf(path, sizeof path, "%s/frame-0-00.ppm", argv[1]);
            FILE* f = fopen(path, "rb");
            assert(f);
            fseek(f, 0, SEEK_END);
            long length = ftell(f);
            fseek(f, 0, SEEK_SET);
            unsigned char* ppm = malloc((size_t)length);
            assert(ppm && length > 0 && fread(ppm, 1, (size_t)length, f) == (size_t)length);
            fclose(f);
            const char* header = "P6\n400 240\n255\n";
            assert(!memcmp(ppm, header, strlen(header)));
            unsigned char rgb[3];
            ppm_pixel(ppm, (size_t)length, 240, 400, 0, 0, rgb); /* screen top-left: row 0, pixel 239 */
            assert(rgb[0] == 239 && rgb[1] == 0 && rgb[2] == 1);
            ppm_pixel(ppm, (size_t)length, 240, 400, 399, 0, rgb); /* top-right: row 399, pixel 239 */
            assert(rgb[0] == 239 && rgb[1] == 143 && rgb[2] == 1);
            ppm_pixel(ppm, (size_t)length, 240, 400, 0, 239, rgb); /* bottom-left: row 0, pixel 0 */
            assert(rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 1);
            free(ppm);
            /* The libctru path posts framebuffers through the shared
               FrameBufferUpdate blocks; vblank must latch and show those too. */
            for (unsigned i = 0; i < 240u * 400; ++i) {
                unsigned char* px = pattern + (size_t)i * 4;
                px[3] = 0x21;
                px[2] = 0x43;
                px[1] = 0x65;
            }
            assert(snx_ctr_write_ram(p->space, fb, pattern, 240u * 400 * 4) == SNX_CTR_MEM_OK);
            uint32_t block = QUEUE + 0x200;
            ctr_put(p, block + 0, 0x100); /* slot 0 selected, update pending */
            ctr_put(p, block + 4, 0);     /* active_framebuf */
            ctr_put(p, block + 8, fb);    /* framebuf0_vaddr */
            ctr_put(p, block + 12, fb);
            ctr_put(p, block + 16, 960);
            ctr_put(p, block + 20, 0x100); /* format 0, linear */
            ctr_put(p, block + 24, 0);
            ctr_put(p, block + 28, 0);
            snx_ctr_gsp_vblank(p);
            assert(ctr_word(p, block + 0) == 0); /* applied */
            snprintf(path, sizeof path, "%s/frame-0-01.ppm", argv[1]);
            f = fopen(path, "rb");
            assert(f);
            fseek(f, 0, SEEK_END);
            length = ftell(f);
            fseek(f, 0, SEEK_SET);
            ppm = malloc((size_t)length);
            assert(ppm && length > 0 && fread(ppm, 1, (size_t)length, f) == (size_t)length);
            fclose(f);
            ppm_pixel(ppm, (size_t)length, 240, 400, 100, 100, rgb);
            assert(rgb[0] == 0x21 && rgb[1] == 0x43 && rgb[2] == 0x65);
            free(ppm);
            /* The LCD shows the left eye of the register pair shown_fb selects, whatever pair a swap writes:
               active_fb 1 writes pair 2 (left fb2, right fb, 3D off) and shows it; a later swap into pair 1 with
               shown_fb still 1 changes nothing on screen (Azahar video_core/gpu.cpp, LoadFBToScreenInfo). */
            uint32_t fb2 = 0;
            assert(!snx_ctr_kernel_memory(p, 0x10003, 0, 0, 0x60000, 3, &fb2) && fb2);
            fill_rgba8(p, fb, 0x10, 0x20, 0x30);
            fill_rgba8(p, fb2, 0x40, 0x50, 0x60);
            uint32_t pair2[] = {0x00050200, 0, 1, fb2, fb, 960, 0, 1, 0};
            ctr_call(p, gsp, pair2, 9, SNX_CTR_SVC_RETURN, 0);
            assert(!ctr_command_word(p, 1));
            snx_ctr_gsp_vblank(p);
            frame_pixel(argv[1], 2, 200, 120, rgb);
            assert(rgb[0] == 0x40 && rgb[1] == 0x50 && rgb[2] == 0x60);
            uint32_t pair1[] = {0x00050200, 0, 0, fb, fb, 960, 0, 1, 0};
            ctr_call(p, gsp, pair1, 9, SNX_CTR_SVC_RETURN, 0);
            assert(!ctr_command_word(p, 1));
            snx_ctr_gsp_vblank(p);
            frame_pixel(argv[1], 3, 200, 120, rgb);
            assert(rgb[0] == 0x40 && rgb[1] == 0x50 && rgb[2] == 0x60);
            /* shown_fb is one bit of the register: 2 shows pair 1 */
            uint32_t shown2[] = {0x00050200, 0, 0, fb, fb, 960, 0, 2, 0};
            ctr_call(p, gsp, shown2, 9, SNX_CTR_SVC_RETURN, 0);
            assert(!ctr_command_word(p, 1));
            snx_ctr_gsp_vblank(p);
            frame_pixel(argv[1], 4, 200, 120, rgb);
            assert(rgb[0] == 0x10 && rgb[1] == 0x20 && rgb[2] == 0x30);
            /* RGB5A1 and RGBA4 framebuffers are accepted and presented, and an invalid screen is ignored with
               success */
            uint32_t rgba4_swap[] = {0x00050200, 0, 0, fb, fb, 480, 4, 0, 0};
            ctr_call(p, gsp, rgba4_swap, 9, SNX_CTR_SVC_RETURN, 0);
            assert(!ctr_command_word(p, 1));
            snx_ctr_gsp_vblank(p);
            frame_pixel(argv[1], 5, 200, 120, rgb);
            uint32_t bad_screen[] = {0x00050200, 2, 0, fb, fb, 960, 0, 0, 0};
            ctr_call(p, gsp, bad_screen, 9, SNX_CTR_SVC_RETURN, 0);
            assert(!ctr_command_word(p, 1));
            /* Presenter contract: the framebuffer is read where the guest has it (here VRAM's start), decoded on the
               device, and the hash covers exactly the decoded pixels */
            const uint32_t vram = 0x18000000u;
            unsigned short low[2] = {0xffffu, 0xf800u};
            unsigned char raw[4];
            memcpy(raw, low, sizeof raw);
            uint64_t hash = 0;
            assert(snx_ctr_phys_write(p->memory, vram, raw, sizeof raw) == SNX_CTR_MEM_OK);
            assert(snx_ctr_present_frame(1, vram, 2, 1, 2, 4, &hash));
            const unsigned char expect[8] = {255, 255, 255, 255, 255, 0, 0, 255};
            assert(hash == fnv(expect, sizeof expect));
            /* RGB8 (libctru BGR8_OES: B,G,R bytes) */
            unsigned char bgr[3] = {0x10, 0x20, 0x30};
            uint64_t hash2 = 0;
            assert(snx_ctr_phys_write(p->memory, vram, bgr, sizeof bgr) == SNX_CTR_MEM_OK);
            assert(snx_ctr_present_frame(1, vram, 1, 1, 1, 3, &hash2));
            const unsigned char expect2[4] = {0x30, 0x20, 0x10, 255};
            assert(hash2 == fnv(expect2, sizeof expect2));
            /* RGBA8 (A,B,G,R bytes) and RGBA4 */
            unsigned char abgr[4] = {0x40, 0x30, 0x20, 0x10}, rgba4[2] = {0x8c, 0x1f};
            uint64_t hash3 = 0, hash4 = 0;
            assert(snx_ctr_phys_write(p->memory, vram, abgr, sizeof abgr) == SNX_CTR_MEM_OK);
            assert(snx_ctr_present_frame(1, vram, 1, 1, 0, 4, &hash3));
            const unsigned char expect3[4] = {0x10, 0x20, 0x30, 0x40};
            assert(hash3 == fnv(expect3, sizeof expect3));
            assert(snx_ctr_phys_write(p->memory, vram, rgba4, sizeof rgba4) == SNX_CTR_MEM_OK);
            assert(snx_ctr_present_frame(1, vram, 1, 1, 4, 2, &hash4)); /* 0x1f8c: R 1, G 15, B 8, A 12 */
            const unsigned char expect4[4] = {0x11, 0xff, 0x88, 0xcc};
            assert(hash4 == fnv(expect4, sizeof expect4));
        }
        free(pattern);
        /* WaitSynchronization1 on the relay event, not signaled: the thread blocks, and the scheduler's next
           vblank wakes it with success (the only thread, so it sleeps the host until then). */
        p->objects[snx_ctr_handle_object(p, event)].data.event.signaled = 0;
        p->cpu.state.cpu.r[0] = event;
        p->cpu.state.cpu.r[1] = 0;
        p->cpu.state.cpu.r[2] = UINT32_MAX;
        p->cpu.state.cpu.r[3] = UINT32_MAX;
        uint32_t vblanks = p->gsp.vblank_count;
        assert(snx_ctr_svc(p, 0x24) == SNX_CTR_SVC_BLOCKED);
        p->cpu.state.cpu.r[0] = 0xdeadbeef;
        assert(snx_ctr_schedule(p) && !p->cpu.state.cpu.r[0] && p->gsp.vblank_count > vblanks);
        snx_ctr_present_free();
        snx_ctr_process_free(p);
    }
    printf("GSP relay/GX queue/register file and Vulkan presentation passed\n");
    return 0;
}
