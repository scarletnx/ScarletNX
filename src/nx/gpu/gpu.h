/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* GPU state shared between the engines and the backends */
#ifndef SNX_GPU_H
#define SNX_GPU_H
#include "common/cpu/aarch64/cpu.h"
/* a macro's state (mme.c), kept with its channel: a wait (gpu_park) stops the macro after the method that asked for
   it, and it goes on (mme_resume) before the stream's next word */
typedef struct {
    u32 regs[8], carry;
    u32 method, incr; /* where the next send goes, and how far it moves after */
    u32 idx, np, pi;  /* the macro, its parameters (in the channel's mme_params) and the next one to fetch */
    u32 pc, dpc;      /* the next instruction, and where a taken branch goes after its delay slot */
    u32 steps;        /* instructions run, against a macro that never ends */
    int running;      /* started and not ended: a wait stopped it */
    int delayed;      /* a branch was taken: after the next instruction, its delay slot, pc becomes dpc */
    int slot;         /* the next instruction is a delay slot: its exit bit doesn't count */
    int exiting;      /* the macro ends after the next instruction, its exit's delay slot */
} GpuMme;
/* a method batch's place in the stream: one can continue into the next GPFIFO entry */
typedef struct {
    u32 mode, sub, m, cnt, k;
} PbBatch;
typedef struct GpuChannel {
    u32 id;
    u32 sub_class[8];
    u32 puller[0x40];
    u32 r3d[0x1000];       /* 3D engine regs, 0xE00+ are MME macro calls */
    u32 r3d_shadow[0xE00]; /* SET_MME_SHADOW_RAM_CONTROL track/replay state */
    u32 r3d_shadow_mode;
    u64 r3d_counter_base[32]; /* draw-count baselines for estimated Maxwell counter reports */
    u32 r3d_counter_reset_mask;
    u32 rcomp[0x1000];
    u32 r2d[0x400];
    u32 rdma[0x200];
    u32 ri2m[0x80];
    /* inline to memory */
    u8* i2m_buf;
    u32 i2m_buf_cap, i2m_len, i2m_pos, i2m_w, i2m_lines, i2m_pitch, i2m_bh;
    u64 i2m_addr;
    int i2m_linear;
    u32 i2m_bd, i2m_dw, i2m_dh, i2m_x, i2m_y, i2m_z;
    /* macros (MME) */
    PbBatch pb;
    u32 mme_code[0x2000];
    u32 mme_start[0x100];
    u32 mme_upload_ptr, mme_start_ptr;
    u32 mme_params[0x10000];
    u32 mme_nparams;
    int mme_active;
    u32 mme_index;
    /* constant buffer data (3D 0x8E4-0x8F3) a method batch is still sending, written to memory in one go */
    u64 cb_addr;
    u32 cb_n;
    u32 cb_words[256];
    /* the renderer's mirror of the channel's constant buffer bindings (3D CB bind methods): per channel, since
       channels bind their own. The renderer keeps one active set itself, synced from here on each draw */
    u64 vk_cb[5][18];
    u32 vk_cbsize[5][18];
    /* SetRenderEnable (3D 0x554-0x556) or its override (0x651) turned drawing off: draws and clears are skipped */
    int render_off;
    /* the constant buffers bound to each stage (0x904 + 8 * stage), address and size: 3D state of this channel, which
       another channel's binds don't change */
    u64 cbind[5][18];
    u32 cbind_size[5][18];
    u32 instance_count; /* the instance the next draw is, counted by BEGIN's next-instance bit and 0x486 */
    u64 dma_job_id;
    u32 dma_launch_index;
    u32* inline_indices; /* 3D inline index methods, widened to u32 until END draws them */
    u32 inline_n, inline_cap;
    GpuMme mme; /* the macro running, or one a wait stopped after the method that asked for it */
    /* what the stream waits for before its next method (gpu_park): the GPU thread runs other channels' work meanwhile,
       as the GPU's Host unit (PFIFO) switches channels on a semaphore acquire that fails */
    u32 park;      /* GPU_PARK_*, or 0 */
    u32 park_val;  /* the value compared with, or the syncpoint's threshold */
    int park_geq;  /* a semaphore reaches park_val (a wrapping compare) instead of equalling it */
    u64 park_addr; /* the semaphore's GPU address, or the syncpoint id */
    u64 park_end;  /* the plat_ns() past which the stream goes on anyway */
} GpuChannel;
enum {
    GPU_PARK_SEMAPHORE = 1, /* the puller's SemaphoreOperation acquire */
    GPU_PARK_REPORT,        /* the 3D or compute engine's report semaphore acquire */
    GPU_PARK_SYNCPOINT,     /* the puller's syncpoint wait */
    GPU_PARK_FENCE          /* a submission's fence, waited for before its first entry */
};
typedef struct {
    GpuChannel* chan[128]; /* one per syncpoint, and nvdrv hands out fewer than 128 */
    int nchan;
} GpuState;
extern GpuState g_gpu;
/* a 2D engine surface, the source or destination of a blit */
typedef struct {
    u32 format, linear, bh, depth, layer, pitch, width, height;
    u64 addr;
} Surf2D;
/* the VIC composition (gpu/video/vic.c): a surface plane as VIC addresses it (pitch linear rows or block linear
   GOBs), a slot's resolved rectangles and color matrix, and a job's planes and slots. The renderer's vic_compose
   runs the composition as a compute shader (gpu/vk/vic.c); when it returns 0 the caller composes in guest memory */
typedef struct VicPlane {
    u8* p;   /* the plane's bytes in guest memory */
    u64 lim; /* bytes p may touch */
    u32 pitch, h, blk, bh;
} VicPlane;
typedef struct VicSlot {
    int on;                               /* the slot composes */
    u32 sx0, sy0, dx0, dy0, w, h;         /* the resolved rectangles: source start, destination start and size */
    int mat[3][4], mat_shift, mat_enable; /* the color matrix, out = (R*c0 + G*c1 + B*c2) >> shift, + c3, >> 8 */
    u32 alpha, clamp_lo, clamp_hi;        /* the planar alpha and the soft clamp range (10 bit) */
    VicPlane iy, ic;
} VicSlot;
typedef struct VicJob {
    u32 fmt, sw, sh;
    VicPlane oy, oc; /* oc.p is NULL when the format has no chroma */
    VicSlot slots[8];
} VicJob;
int gmem_read(u64 gva, void* dst, u64 size);
/* gmem_read and gmem_write in halves, for a copy of many small runs (a block linear row's 16 byte runs): the
   renderer's hooks once over the span the runs fall in (gmem_need before reading it, gmem_before_write before writing
   it and gpu_mem_written after), then the runs themselves without them. The hooks look through every render target */
void gmem_need(u64 gva, u64 size);
void gmem_read_raw(u64 gva, void* dst, u64 size);
void gmem_before_write(u64 gva, u64 size);
void gmem_write_raw(u64 gva, const void* src, u64 size);
void gmem_read_code(u64 gva, void* dst, u64 size); /* a shader program: stops where guest memory does */
int gmem_write(u64 gva, const void* src, u64 size);
u8* gmem_ptr(u64 gva, u64 size);
/* Only for comparing a pending GPU result's original guest bytes: does not land pending GPU writes. */
u8* gmem_ptr_untracked(u64 gva, u64 size);
u32 gmem_r32(u64 gva);
void gmem_w32(u64 gva, u32 v);
u64 bl_offset(u32 x_bytes, u32 y, u32 z, u32 width_bytes, u32 height, u32 bh_log2, u32 bd_log2);
void deswizzle(u8* dst, u32 dst_pitch, const u8* src, u32 w_bytes, u32 h, u32 bh_log2);
void swizzle(u8* dst, const u8* src, u32 src_pitch, u32 w_bytes, u32 h, u32 bh_log2);
void i2m_launch(GpuChannel* ch, u32* regs, int base);
void i2m_data(GpuChannel* ch, u32 v);
void maxwell3d_method(GpuChannel* ch, u32 m, u32 v, int last);
void compute_method(GpuChannel* ch, u32 m, u32 v, int last);
void kepler_i2m_method(GpuChannel* ch, u32 m, u32 v);
void fermi2d_method(GpuChannel* ch, u32 m, u32 v);
void dma_method(GpuChannel* ch, u32 m, u32 v);
/* the linear source of a LAUNCH_DMA (v) with registers r, read now for the channel's copy to use when it runs: called
   in the channel's submit order, for every launch the stream will run (gpu_submit) */
void dma_snap_launch(u32 channel, u64 job_id, u32 launch_index, const u32* r, u32 v);
void dma_snap_finish(u32 channel, u64 job_id);
void dma_snap_reset(u32 channel); /* the channel closed: what it queued goes */
void gpu_mem_written(u64 gva, u64 size);
void gpu_mem_written_shader(u64 gva, u64 size); /* shader writeback tracks both GPU VA and guest backing */
void gpu_signal_point(void);                    /* the GPU is about to do something the game can observe */
u32 gpu_color_bpp(u32 format); /* bytes per pixel of a color format (render targets and 2D surfaces share them) */

/* gpu.c: GPFIFO submission, and the GPU thread's queue */
void gpu_init(void);
/* queues GPFIFO entries for the GPU thread, which waits for the fence (wait_id ~0: none) as the stream's first
   method, runs them on channel (the channel's syncpoint id), then increments the syncpoint incs times, after which it
   should be at expect (the fence nvdrv promised). The entries are u64s at any alignment. A channel's jobs run in
   order, and another channel's may run before them while they wait (gpu_park) */
void gpu_submit(const void* entries, u32 n, u32 channel, u32 incs, u32 wait_id, u32 wait_val, u32 expect);
/* calls fn(arg) on the GPU thread in order with the other calls, once the jobs queued before it on channels
   chans[0..nchans) (n at most 4) are done. Other channels' jobs, queued before or after it, don't hold it back */
void gpu_call(void (*fn)(void*), void* arg, const u32* chans, u32 nchans);
void gpu_channel_close(u32 channel);  /* nvdrv closed the channel: a new one with the same id starts from reset */
void gpu_release_retire(u32 channel); /* complete pending syncpoint releases before nvdrv reuses its id */
/* the channel's stream waits for the u32 at GPU address addr (a syncpoint's id for the syncpoint kinds) to equal val,
   or reach it (geq), before its next method. Met already, nothing waits. Otherwise the method that asked returns and
   the stream stops after it (a macro after the method too, mme_resume), and the GPU thread goes on with other
   channels until it's met */
void gpu_park(GpuChannel* ch, u32 kind, u64 addr, u32 val, int geq);
/* something a parked channel may wait for changed (a syncpoint increment, on the GPU thread or off it): look again.
   Any thread */
void gpu_kick(void);
extern volatile u64 g_gpu_submits;
/* guest memory [cpu_addr, cpu_addr + len) changed without a write the host's write tracking sees: decommitted pages,
   which now read as zero (kernel/mem.c). Caches over that memory check it again. Any thread */
void gpu_mem_changed(u64 cpu_addr, u64 len);
extern void (*g_gpu_mem_changed)(u64 cpu_addr, u64 len); /* the renderer's side of it, set when it starts */
/* the renderer lands pending shader results before the stream goes on past them: before a write of
   memory those results overlap (size > 0), and before anything the game can observe (gpu_signal_point, size 0) */
extern void (*g_gpu_before_write)(u64 gva, u64 size);
extern int g_gpu_releases; /* the renderer holds releases waiting to happen (renderer.h release_need) */
/* a semaphore or report payload the game can poll, and a syncpoint increment of the stream: they happen once
   everything before them in the stream is done */
void gpu_release_write(u64 gva, const void* data, u32 len);
/* Hold the renderer's memory lock across a backing change. full is for CPU guest pages, whose GPU aliases are
   unknown; GPU-VA changes supply the affected range. Begin precedes the memory manager's own lock. */
extern void (*g_gpu_mapping_begin)(u64 gva, u64 size, int full);
extern void (*g_gpu_mapping_end)(void);

/* engines.c: the 3D engine's methods and its report semaphores */
u64 gpu_ticks(void);
/* mme.c: a macro a wait stopped (ch->mme.running) goes on, until it ends or waits again */
void mme_resume(GpuChannel* ch);
void r3d_pending_flush(void); /* instanced draws are merged into one while they come in, this sends it now */
int r3d_pending_on(void);

/* maxwell3d.c: the 3D engine's side of the renderer */
void r3d_clear(GpuChannel* ch, u32 v);
/* a clear is cut to scissor 0 when CLEAR_FLAGS (3D 0x43E) has SCISSOR, bit 8, and that scissor is enabled. Bit 4,
   CLEAR_RECT, is another bound (nouveau's names for the bits; Eden reads the scissor from bit 8 too) */
static inline int r3d_clear_scissored(const u32* r) { return (r[0x43E] & 0x100) && (r[0x380] & 1); }
void r3d_draw_texture(GpuChannel* ch); /* DRAW_TEXTURE (3D 0x420-0x42B) */
void r3d_draw(GpuChannel* ch, int indexed, u32 topology, u32 first, u32 count, u32 base_vertex, u32 base_instance,
              u32 instances);
void r3d_cb_bind(GpuChannel* ch, int stage, u32 v); /* records the binding in ch->cbind */
void r3d_const_upload(GpuChannel* ch, u32 v, int last);
void r3d_const_flush(GpuChannel* ch); /* writes the constant buffer data r3d_const_upload is holding */
int r3d_blit(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
             double sdy);
u64 gpu_mem_generation(void); /* the last fully published GPU-side write generation */
int gpu_mem_dirty_since(u64 since, u64 a, u64 sz);
#endif
