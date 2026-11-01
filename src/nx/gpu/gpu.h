/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GPU's host side: guest memory as the GPU sees it, the record of GPU-side writes, the GPU thread's queue, and
   the structures NVN's copies, VIC and NVDEC hand the renderer */
#ifndef SNX_GPU_H
#define SNX_GPU_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/gpu/draw_state.h"

/* a 2D surface, the source or destination of a blit (as the 2D engine's registers 0x80-0x89 and 0x8C-0x95 give it) */
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
    u32 vu; /* input format 0x43 (Y8___U8V8_N420): V in each chroma pair's even byte, U in the odd one */
    VicPlane iy, ic;
} VicSlot;
typedef struct VicJob {
    u32 fmt, sw, sh;
    u32 tx0, ty0, tw, th; /* the target rectangle the background colour fills, in the surface; tw 0: none. The
                             slots lie inside it (vic.c's slot_resolve clips them) */
    u32 bg[4];            /* the background colour: R, G, B, A, 10 bit */
    VicPlane oy, oc;      /* oc.p is NULL when the format has no chroma */
    VicSlot slots[8];
} VicJob;
/* An NVDEC picture for its surfaces (gpu/video/nvdec.c): FFmpeg's planar 4:2:0 rows, w x h of them written into NV12
   surfaces, block linear, at guest CPU addresses luma and chroma: widths pitch_y and pitch_c bytes, the luma surface
   surf_h rows high (the chroma's half of that, rounded up), blocks 2^bh GOBs high. The surfaces' extents (whole GOBs
   wide, whole blocks high) are ext_y and ext_c bytes. Only the picture's bytes are written: in a row's last 16
   bytes, those past its width keep their values. A picture Vulkan Video decoded (the vulkan_video setting) is FFmpeg's
   AVFrame of format AV_PIX_FMT_VULKAN in hw instead, its NV12 planes in a GPU image, and plane is unused */
typedef struct NvdecPic {
    const u8* plane[3]; /* Y, U, V */
    int stride[3];
    u32 w, h;
    u64 luma, chroma, ext_y, ext_c;
    u32 pitch_y, pitch_c, surf_h, bh;
    const void* hw;
} NvdecPic;
/* A launch of the DMA copy engine (NVN's copies, gpu/nvn/nvn_copy.c), for the renderer, which does it on the GPU:
   lines of width elements from src to dst, each side pitch linear (rows pitch bytes apart from addr) or block linear
   (a surface wb bytes wide and h rows high, blocks 2^bh GOBs high and 2^bd deep, slice z, from the origin ox bytes
   in, oy rows down; rows from h on read as zero and take no writes). With remap, an element is ns components of cs
   bytes in and nd out, component k of it from sel[k] (0-3 a source component, 4 constant A, 5 constant B, 6 and 7 the
   destination's own bytes); fill: nothing is read. snap: the source's bytes as the caller read them (pitch linear, its
   span), or NULL */
typedef struct {
    u64 addr;
    int bl;
    u32 pitch, wb, h, z, bh, bd, ox, oy;
} GpuDmaSide;
typedef struct {
    int multi, remap, fill, shuffle, keep;
    u32 cs, ns, nd, sel[4], const_a, const_b;
    u32 width, lines, line_in, line_out; /* elements a line, lines, a line's bytes in and out */
    GpuDmaSide src, dst;
    const u8* snap;
} GpuDmaOp;
/* A 2D blit the renderer does on the GPU: each destination pixel of columns x0 to x1 - 1
   and rows y0 to y1 - 1 the source pixel at column scol[x - x0] and row srow[y - y0] (nearest sampled, the tables
   the engine works out), bpp bytes each. The sides are surface planes as the DMA engine's are (no origin). fmt 0
   copies the pixels' bytes; else both are float formats (GPU_BLIT_FLOAT) and each component is converted */
typedef struct {
    GpuDmaSide src, dst;
    u32 bpp, x0, x1, y0, y1;
    const u32* scol;
    const u32* srow;
    u32 fmt;
} GpuBlit;
/* a blit's fmt between float formats: the source's components (1 to 4) of sb bytes (2 or 4), the destination's (dn
   of db bytes), sx when the source's fourth component is padding. A component the source lacks is 0, its alpha 1.
   bpp is the source's pixel, sn * sb bytes. texconv.glsl's TC_BLIT_FLOAT takes it as it is */
#define GPU_BLIT_FLOAT(sn, sb, dn, db, sx) ((sn) | (sb) << 4 | (dn) << 8 | (db) << 12 | (u32)(sx) << 16)
/* the bytes of a blit's destination pixel */
#define GPU_BLIT_DST_BPP(b)                ((b)->fmt ? ((b)->fmt >> 8 & 15) * ((b)->fmt >> 12 & 15) : (b)->bpp)

/* ---- gpu.c: guest memory through the GPU's address space. A read lands the renderer's pending results over the
   range first, a write lets it land what would overwrite the write later, and records the write (gpu_mem_written).
   Unmapped pages read as zero and take no writes; the int calls return -1 when any page was unmapped */
int gmem_read(u64 gva, void* dst, u64 size);
int gmem_write(u64 gva, const void* src, u64 size);
/* gmem_read in halves, for a copy made of many small runs: the hooks once over the span the runs fall in (gmem_need
   before reading it), then the runs without them. gmem_write_raw writes without the hooks or the record */
void gmem_need(u64 gva, u64 size);
void gmem_read_raw(u64 gva, void* dst, u64 size);
void gmem_write_raw(u64 gva, const void* src, u64 size);
/* a shader program: up to size bytes, stopping at the first page with no guest memory behind it, zeros after */
void gmem_read_code(u64 gva, void* dst, u64 size);
/* the same with nothing landed (memory/own.c): from the first byte the GPU holds on, zeros. How many bytes are the
   CPU's (size when the GPU holds none) */
u64 gmem_read_code_cpu(u64 gva, void* dst, u64 size);
/* whether a program shader_hash measured len bytes long in a buffer whose bytes from cpu on gmem_read_code_cpu made
   zero may go on into them (size: the buffer's): the scan ends at a zero word, which a control word can precede, so
   one starting within two words of the end may be a zero of ours. Then gmem_read_code reads it with them landed */
int gmem_code_cut(u64 len, u64 cpu, u64 size);
/* the host address of [gva, gva + size) when it is contiguous in guest memory, after landing pending results over
   it; NULL otherwise */
u8* gmem_ptr(u64 gva, u64 size);
/* the same without landing anything: only for comparing a pending result's original guest bytes */
u8* gmem_ptr_untracked(u64 gva, u64 size);
/* the host address of [gva, gva + size) when contiguous, nothing landed and nothing acquired: only to name the range
   (a write-watch scan), never to read or write through, as GPU-owned bytes there are trapped (memory/own.c) */
u8* gmem_host(u64 gva, u64 size);
/* a word of guest memory as a number (little-endian there) */
u32 gmem_r32(u64 gva);
void gmem_w32(u64 gva, u32 v);
/* the byte offset of (x_bytes, y, z) in a block linear surface width_bytes wide and height rows high, with blocks of
   2^bh_log2 GOBs high and 2^bd_log2 deep (the caller shrinks them for small surfaces, as the hardware does). The
   layout is texture/blocklinear.h's */
u64 bl_offset(u32 x_bytes, u32 y, u32 z, u32 width_bytes, u32 height, u32 bh_log2, u32 bd_log2);
u32 gpu_color_bpp(u32 format); /* bytes per pixel of a color format (render targets and 2D surfaces share them) */
void gpu_signal_point(void);   /* the GPU is about to do something the game can observe */
/* a semaphore or report payload the game can poll: it happens once everything before it in the stream is done. data is
   the bytes as guest memory gets them, so its numbers are little-endian already */
void gpu_release_write(u64 gva, const void* data, u32 len);
/* save states (runtime/state.c), with the GPU thread held: the draw count and the calls queued into the state (0, or
   -1). A load reads them first into a staged copy (NULL when the section is damaged), which a refused load frees, and
   puts it in place once the running game is gone, every call it queued dropped */
struct StateIo;
typedef struct GpuStaged GpuStaged;
int gpu_state_save(struct StateIo* io);
GpuStaged* gpu_state_parse(struct StateIo* io);
void gpu_state_install(GpuStaged* s);
void gpu_state_staged_free(GpuStaged* s);

/* ---- writes.c: the record of GPU-side writes. Each write stamps a new generation on the pages it covers, by GPU
   address and by the guest pages behind them */
void gpu_mem_written(u64 gva, u64 size);
void gpu_mem_written_shader(u64 gva, u64 size); /* a shader's result landing in guest memory */
/* guest CPU bytes [cpu, cpu + size) written by an engine that doesn't go through the GPU's address space (VIC's
   picture, published over them, memory/own.c): what is built from them checks them again */
void gpu_mem_written_cpu(u64 cpu, u64 size);
/* fn(cpu, n) for each run of guest memory that a GPU-side write above changes (gpu/vk/uma.c's hazard count) */
void gpu_set_write_watch(void (*fn)(u64 cpu, u64 n));
u64 gpu_mem_generation(void);                   /* the last generation fully published */
void gpu_mem_written_all(void); /* a save state's load: every page written, after anything asked before */
int gpu_mem_dirty_since(u64 since, u64 a, u64 sz); /* a GPU-side write after since touched [a, a + sz) */

/* ---- gpu.c: the GPU thread's queue */
void gpu_init(void);
/* calls fn(arg) on the GPU thread, in order with the other calls */
void gpu_call(void (*fn)(void*), void* arg);
/* a game pause, with no guest thread left to queue anything: the GPU thread runs what is queued up to now, the
   renderer finishes (Renderer.idle), and the thread waits until gpu_release. 0, or -1 when it didn't get there within
   timeout_ms (and won't stop when it does) */
int gpu_hold(int timeout_ms);
void gpu_release(void);
/* the blocking wait the GPU thread is in, for the stall report: what it waits for (a string that lives on) and a number
   that names it, before the wait, and NULL after. Ignored on other threads: 1 on the GPU thread */
int gpu_wait_note(const char* what, u64 value);
/* the stall watchdog's report of what the GPU thread is doing. Any thread */
void gpu_report_stall(void);
/* guest memory [cpu_addr, cpu_addr + len) changed without a write the host's write tracking sees: decommitted pages,
   which now read as zero (memory/mem.c). Caches over that memory check it again. Any thread */
void gpu_mem_changed(u64 cpu_addr, u64 len);
extern void (*g_gpu_mem_changed)(u64 cpu_addr, u64 len); /* the renderer's side of it, set when it starts */
/* the renderer lands pending shader results before the work goes on past them: before a write of memory those
   results overlap (size > 0), and before anything the game can observe (gpu_signal_point, size 0) */
extern void (*g_gpu_before_write)(u64 gva, u64 size);
extern int g_gpu_releases; /* the renderer holds releases waiting to happen (renderer.h release_need) */
/* the renderer's memory lock held across a change of backing. full is for CPU guest pages, whose GPU aliases are
   unknown: gva and size are then the guest range (size ~0: any of it). GPU address space changes give their GPU
   range. Begin is taken before the memory manager's own lock */
extern void (*g_gpu_mapping_begin)(u64 gva, u64 size, int full);
extern void (*g_gpu_mapping_end)(void);
#endif
