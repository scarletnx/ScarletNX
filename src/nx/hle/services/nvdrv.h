/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What nvdrv shares with the GPU, host1x and vi: nvmap handles, the GPU's address space, and the syncpoints */
#ifndef SNX_NVDRV_H
#define SNX_NVDRV_H
#include "nx/runtime/state_fns.h"
#include "common/cpu/aarch64/cpu.h"
#include "nx/runtime/platform/plat.h" /* PMutex, PCond */

/* The syncpoint ids the table holds. An id at or past it would alias a smaller one: it reads 0, never counts as
   reached, and the ioctls refuse it. syncpt_alloc hands out fewer (NVIDIA's stream library has room for 128). Wider
   than both the 128 below and Eden's MaxSyncPoints (192), so an id a game names that syncpt_alloc didn't hand out
   still reads, increments and waits as itself */
#define SYNCPT_IDS 256
/* The ids syncpt_alloc hands out, [SYNCPT_FIRST, SYNCPT_N), 108 of them: GPU channels and host1x's NVDEC and VIC
   channels alike. SYNCPT_N is the 128 NVIDIA's stream library (NvRmStream) sizes its per-syncpoint tables at, since
   NvRmChannelNumSyncPoints in the game's library is a stub that fails: an id of 128 or more crashed the first VIC
   job in NvRmStreamBegin and wrote past the table into the game's heap (host1x once took ids from 200). The first 20
   are never handed out, as the allocator has always started there; nothing in its history says why. Eden's fixed ids
   (12 VIC, 26 and 27 the vblanks, 0x36 NVDEC, 0x37 NVJPG) aren't kept out: nothing here uses a fixed id */
#define SYNCPT_FIRST 20u
#define SYNCPT_N     128u

void nv_init(void);
/* The GSV syncpoint session shares the existing counters and increment-driven event delivery. */
struct SvcObj* nv_syncpoint_new(void);
/* GSV 497's GPU control session shares notification state with its worker. */
struct SvcObj* nv_gpu_new(void);
/* IGraphicsSupervisor 434: lets the worker of the GPU session that made address space id map into it. 0, or the
   kernel's NotFound for an id no session holds */
int nv_gsv_register_as(u32 id);
/* IGraphicsSupervisor 111, 423 and 112: GSV native memory of size bytes as an nvmap handle, backed by the guest
   memory at offset in the GPU-accessible memory parent (worker 101), and freed. 0 or a kernel result */
int nv_native_memory_create(u64 size, u32* memory);
int nv_native_memory_bind(u32 memory, u64 addr, u64 size, u32 parent, u64 offset);
int nv_native_memory_bind_cpu(u32 memory, u64 addr, u64 size, u64 flags, u64 offset);
int nv_native_memory_destroy(u32 memory);
/* a native image's surface, from the SDK's block preference: the extent ((width - 1) | (height - 1) << 16), the pitch
   in 64-byte GOBs, the block height as log2 rows with the GOB's 8 (3 to 8) and the pixel format */
typedef struct {
    u32 extent, pitch_gobs, block_height_log2, pixel_format;
} NvGsvImageShape;
/* IGraphicsSupervisor 429 and 430: count images of size bytes in all over the native memory, and their end. 315 and
   314 (create 0): the image as a native window's alias image pool of count images, and its end */
int nv_native_image_create(u32 memory, u64 size, u32 count, const NvGsvImageShape* shape, u32* image);
int nv_native_image_destroy(u32 image);
int nv_native_image_window_pool(u32 image, u32 count, int create);
/* a native window's image (an alias pool of one): its memory's nvmap handle and its surface. 0, or the kernel's
   NotFound for a handle that is no window image */
int nv_native_window_image(u32 image, u32* memory, NvGsvImageShape* shape);
/* A native image's slice mapped in the supervisor partition. The mapping pins its nvmap backing. */
int nv_native_image_map(u32 address_space, u32 memory, u64 offset, u64 size, u64* address);
int nv_native_image_unmap(u64 address, u64 size);
/* an ioctl on fd that a restored thread makes again (svc_service_retry): the use of the fd the old one counted goes */
void nv_ioctl_abandon(u32 fd);

/* A live, allocated handle's address and size, read together under nvdrv's lock from any thread. 1, or 0 (both
   outputs 0) for a dead handle */
int nvmap_get(u32 h, u64* addr, u32* size);
u64 nvmap_addr_by_id(u32 id); /* 0 when no live handle has that id */
/* A handle the runtime makes for guest memory the game gave a front end (nvn's memory pools), as the game's NVN
   driver makes one for each pool: created and allocated at once, with one reference, its number its id. 0 when the
   memory isn't page aligned, is past 4 GiB or no number is left. drop gives its reference back, as FREE does */
u32 nvmap_handle_new(u64 addr, u64 size);
void nvmap_handle_drop(u32 h);

/* The guest address a GPU address maps to, 0 when its page isn't mapped */
u64 gmmu_translate(u64 gva);
/* The bytes from gva on, at most max, that stay contiguous in guest memory, with gva's guest address in *cpu. 0 when
   gva's page isn't mapped */
u64 gmmu_run(u64 gva, u64 max, u64* cpu);
/* Points the pages of [gva, gva + size) at guest memory from cpu on, the renderer told (its mapping bracket): what
   nvhost-as-gpu's MapBufferEx does, and a test without the ioctls */
void gmmu_map(u64 gva, u64 cpu, u64 size);
/* GPU address space for a front end that maps guest memory itself (nvn's memory pools), from the allocator and
   page table nvhost-as-gpu uses: alloc reserves size bytes at the alignment (a power of two, 0 for the allocator's
   64 KiB) and returns their GPU address, 0 when the space is full; map points page-aligned [gva, gva + size) at guest
   memory from cpu on; unmap clears those pages; free clears them and gives the addresses back */
u64 gmmu_space_alloc(u64 size, u64 align);
void gmmu_space_map(u64 gva, u64 cpu, u64 size);
void gmmu_space_unmap(u64 gva, u64 size);
void gmmu_space_free(u64 gva, u64 size);
/* Each GPU range that shows guest bytes of [cpu, cpu + size), clipped to them: fn(ctx, gva, n) for the n bytes at
   gva, each mapped page once. The same guest memory is mapped at several GPU addresses, so what is known of it by one
   address holds at the others, and this finds them all from the guest side. From any thread. fn runs under the
   index's lock: it takes no lock and doesn't call back into nvdrv. The index compares addresses only, as the page
   table holds them: guest memory freed under a mapping still counts here, where gmmu_translate reads it as unmapped */
void gmmu_aliases(u64 cpu, u64 size, void (*fn)(void* ctx, u64 gva, u64 n), void* ctx);
/* The other way: each run of the page table over [gva, gva + size), a range below 2^40, in GPU order and clipped to it:
   fn(ctx, gva, cpu, n) for n bytes at gva showing guest memory from cpu on, whole pages. Addresses only, and under the
   same lock, as gmmu_aliases */
void gmmu_pieces(u64 gva, u64 size, void (*fn)(void* ctx, u64 gva, u64 cpu, u64 n), void* ctx);

/* Every syncpoint user allocates its ids here, so they stay below the 128 the stream library sizes its tables at.
   Running out is fatal. A freed id can be handed out again */
u32 syncpt_alloc(void);
void syncpt_free(u32 id);
/* GSV's shared counter cache retains both transfer memories for the application. Its request is a writable event. */
int nv_syncpoint_cache_bind(u32 rw_handle, u32 ro_handle, u32* request, u32* completed);
void nv_syncpoint_cache_request(u32 event);
u32 syncpt_read(u32 id);
/* Read the native AS ioctl's read-only GPU syncpoint page. 1 when the whole read is in the page, else 0. */
int nv_syncpoint_shim_read(u64 gva, void* dst, u64 size);
void syncpt_incr(u32 id);
void syncpt_add(u32 id, u32 n); /* n increments, one wakeup */
/* syncpt_add for a thread holding the renderer's lock (vk_lock), and only inside such a hold: the event slots waiting
   for the value are signaled by the same thread's syncpt_events, which its outermost vk_unlock calls as it lets the
   lock go (vk_release_events). Signaling takes the kernel's lock, and a thread holding that may wait in a trap on
   GPU-owned memory, which is answered under the renderer's lock (memory/own.c) */
void syncpt_add_quiet(u32 id, u32 n);
/* the event slots this thread's syncpt_add_quiet increments reached, signaled: with no renderer's lock held. Nothing
   when there were none since the last call */
void syncpt_events(void);
/* A channel promises the next n increments it will make, in order. Returns the value they reach: the fence the ioctl
   hands back, and what nvhost-ctrl reads as the maximum */
u32 syncpt_promise(u32 id, u32 n);
/* 1 once id reaches thresh (across the 32-bit wrap), 0 when the time runs out. 0 looks once, negative waits for good */
int syncpt_wait(u32 id, u32 thresh, i64 timeout_ns);
/* Runs fn(arg) on the GPU thread after the submissions that promised the fences (id[i], val[i]), i < n <= 4. A fence
   nothing queued has promised is waited for here instead, for at most 1 s, and its id[i] is set to ~0 */
void nv_gpu_call_after(u32 n, u32* id, const u32* val, void (*fn)(void*), void* arg);
/* ---- HLE state in g_hle (hle.h) */
#define NVMAP_HANDLES 8192 /* nvmap's handles, numbered 1..8191 */
#define NV_FDS        256  /* nvdrv's fds, numbered 1..255 */

/* The GPU's page table (gmmu_*), Eden's Tegra::MemoryManager geometry: a 40-bit GPU address space
   (address_space_bits, the characteristics' gpu_va_bit_count 0x28) in 4 KiB pages (page_bits), looked up in two
   levels. A second-level table holds 2^GMMU_L2_BITS page entries (16 MiB of GPU addresses), and the first level the
   2^16 tables the remaining bits name */
#define GMMU_ADDRESS_SPACE_BITS 40
#define GMMU_PAGE_BITS          12
#define GMMU_PAGE_SIZE          (1ull << GMMU_PAGE_BITS)
#define GMMU_PAGE_MASK          (GMMU_PAGE_SIZE - 1)
#define GMMU_L2_BITS            12
#define GMMU_L2_ENTRIES         (1u << GMMU_L2_BITS)
#define GMMU_L1_BITS            (GMMU_ADDRESS_SPACE_BITS - GMMU_PAGE_BITS - GMMU_L2_BITS)
#define NV_GMMU_L1              (1u << GMMU_L1_BITS) /* the first level's entries */
#define GMMU_PAGES              (1ull << (GMMU_ADDRESS_SPACE_BITS - GMMU_PAGE_BITS)) /* pages in the space */

/* ctrl fds with event slots in use at once. Each NvRm user (NVN, a movie player, the video decoder) opens one: games
   hold a handful, one game two */
#define NV_CTRL_TABLES 64
/* a ctrl fd's event slots (Eden's MaxNvEvents) */
#define NV_CTRL_EVENT_SLOTS 64u
/* a channel fd's QueryEvent events, by id: 1 SM exception, 2 SM pause, 3 error notifier (Ryujinx's numbering), 0
   unused */
#define NV_CHANNEL_EVENTS 4u

/* an nvmap handle (nvh, by handle number). CREATE and each FROM_ID take a reference, FREE gives one back; a GPU
   mapping pins it, as the console's nvmap does, and the last reference and pin ends it. allocated: ALLOC gave it guest
   memory at addr. native: GSV native memory (IGraphicsSupervisor 111), which 423 backs and 112 frees */
typedef struct {
    u64 addr;
    u32 size, align, heap, flags;
    u32 id;
    u32 refs, pins;
    int alive, allocated;
    u8 kind, native;
} NvMapH;
/* what MAP_BUFFER_EX mapped, for UNMAP_BUFFER: [gva, gva+size) maps cpu of nvmap handle h. base is the address
   UNMAP_BUFFER names; a modify (NVHOST_AS_MAP_REMAP) or FREE_SPACE splits a mapping into pieces that keep it. va is the
   VA the mapping took from the allocator (0 when the game placed it), given back when it is unmapped. gsv is the GPU
   session state (1 + its index) whose worker mapped it (with h 0), 0 for nvhost-as-gpu's. sparse is the sparse region
   (1 + its index) that a worker's sparse map put it in, with base 0: only that region's maps and its destruction
   remove it */
typedef struct {
    u64 gva, size, cpu;
    u64 base, va;
    u32 h, gsv, memory; /* memory: GSV registration id, 0 for nvmap mappings */
    u32 sparse;
} GMap;
typedef struct {
    u64 a, e;
} VaRange;
/* a run of the GPU page table: its pages [gpage, gpage + pages) show guest pages from cpage on */
typedef struct {
    u64 gpage, pages, cpage;
} GRun;
/* gmmu_aliases' index of the runs by guest page, built from them on its first query after a change. The guest pages
   where a run starts or ends cut guest memory into nbound - 1 intervals, interval i [bound[i], bound[i + 1]), and
   cover[first[i]] to cover[first[i + 1]] are the runs over all of it, so a query walks the intervals its range
   meets. seen[r] is the query that last reported run r, so a run over several intervals is reported once. One block
   from bound, cap bytes */
typedef struct {
    u64* bound;
    u32* first; /* nbound + 1 entries */
    u32* cover;
    u32* seen;  /* one per run */
    size_t cap;
    u32 nbound, query;
    int stale; /* the runs changed since the index was built */
} GAlias;
/* an nvhost-ctrl event slot. ev is made on first use and nvdrv keeps a reference. An armed slot is signaled once
   syncpoint id reaches thresh; armed changes under nvev_lk */
typedef struct {
    u32 ev; /* its kernel object id */
    u32 id, thresh;
    int armed;
} NvEvent;
/* a ctrl fd's event slots (ctrl_evt, a pool under nvev_lk) */
typedef struct {
    u32 fd; /* 1 + the fd whose slots these are, set while it's taken */
    NvEvent slot[NV_CTRL_EVENT_SLOTS];
} NvEventTable;
#define NV_GSV_SYNCS 16
#define NV_GSV_EVENTS 63
typedef struct {
    u32 obj, count;
    NvEvent events[NV_GSV_EVENTS];
} NvGsvSync;
#define NV_GSV_GPUS 16
#define NV_GSV_GPU_EVENTS 64
/* a GPU session's graphics devices (CreateGraphicsDevice): ids 1 to 255, as the SDK keeps an id in a byte */
#define NV_GSV_GRAPHICS_DEVICES 256
/* Shared by the control session and its worker. refs is also the pool allocation marker, under nvev_lk. */
typedef struct {
    u32 refs;
    u32 events[NV_GSV_GPU_EVENTS]; /* retained KO_EVENT ids */
    u32 graphics[NV_GSV_GRAPHICS_DEVICES / 32]; /* bit i: graphics device i is live, under aslock */
} NvGsvDevices;
/* GPU address spaces (GPU session 33). There is one because the runtime has one GPU page table (gmmu_*). IDs are
   nonzero and never reused within a process, so an old DEV_AS view cannot attach to a later session's address space.
   Under aslock, which comes before nvev_lk. */
#define NV_GSV_ADDRESS_SPACES 1
typedef struct {
    u32 devices;    /* 1 + the owner's shared-state index, 0 while free */
    u32 id;         /* the opaque id given to ALLOC_AS_EX as as_fd */
    int registered; /* IGraphicsSupervisor 434 registered it: its worker can map into it */
} NvGsvAs;
/* The GPU addresses a GSV address space gives the SDK, its user partition [NV_GSV_USER_FIRST, NV_GSV_USER_END): the
   small-page range the Switch 2 driver then asks ALLOC_AS_EX for (0x8000000 up to 768 GiB), as Eglantine answers.
   eden-ns2's goes on to the end of the 40-bit space. Not measured on a console */
#define NV_GSV_USER_FIRST    0x8000000ull
#define NV_GSV_USER_END      0xC000000000ull
/* The syncpoint shim (GetSyncpointShimInfo): the last page of the GPU address space, past every address the SDK
   allocates (Eglantine puts it on the last page of its range), a u32 for each of eden-ns2's 192 syncpoints */
#define NV_SYNCPT_SHIM_GVA   ((1ull << GMMU_ADDRESS_SPACE_BITS) - GMMU_PAGE_SIZE)
#define NV_SYNCPT_SHIM_COUNT 192u
/* GPU-accessible memory a GPU worker registered (worker 101): guest memory with the GpuAccessible attribute, which
   its maps (worker 211) then show at GPU addresses. Its id is 1 + its index. One game holds more than 4096 while it
   loads, and frees few; the console's limit is not known. Under aslock */
#define NV_GSV_MEMORIES      65536
typedef struct {
    u64 addr, size;
    u32 devices; /* 1 + the owner's shared-state index, 0 while free */
} NvGsvMemory;
/* Sparse regions that a GPU worker reserved (worker 214): GPU addresses without memory, whose pages the worker's
   sparse maps (216) point at registered memory or at nothing again, until the worker frees the region (215). Nothing
   else maps into a region. Its id is 1 + its index. Under aslock */
#define NV_GSV_SPARSE_REGIONS 1024
typedef struct {
    u64 first, size;
    u32 devices; /* 1 + the owner's shared-state index, 0 while free */
} NvGsvSparse;
/* GSV native images (IGraphicsSupervisor 429): each holds a reference on its native memory. An image array takes
   count consecutive handles from handle on (the SDK's CalculateNativeImageHandle adds the index). Handles are nonzero
   and never reused. window_pool: 315 made the image a native window's alias image pool, until 314. Under nv.lock */
#define NV_GSV_IMAGES 256
typedef struct {
    u32 handle, count; /* handle 0 while free */
    u32 memory;        /* the native memory's nvmap handle */
    int window_pool;
    NvGsvImageShape shape;
} NvGsvImage;
typedef struct {
    u32 obj, devices; /* 1 + shared-state pool index */
    u32 worker;       /* retained KO_SESSION id */
} NvGsvGpu;
typedef struct {
    u32 obj, devices;
} NvGsvWorker;
/* an fd (nvf, by number) */
typedef struct {
    int dev;       /* a DEV_* value, DEV_NONE while the entry is free */
    u32 syncpt;    /* DEV_GPU: the channel's syncpoint, 0 until its first use */
    u64 user_data; /* DEV_GPU: SET_USER_DATA's value, for GET_USER_DATA */
    u32 tsg, subcontext; /* DEV_GPU: TSG fd (0 when unbound) and its subcontext */
    u64 tsg_id; /* DEV_TSG: lifetime identity, never reused when its fd closes */
    /* DEV_AS of a Switch 2 driver: the small-page view [view_first, view_end) of view_as. Bounds remain after that
       AS is destroyed, but requests fail because view_as is no longer live. All zero before ALLOC_AS_EX. */
    u64 view_first, view_end;
    u32 view_as;
    /* QueryEvent's events of a channel (NV_CHANNEL_EVENTS), made on first query and never signaled: nothing here
       raises them */
    u32 ev[NV_CHANNEL_EVENTS];
    struct H1Ch* h1x; /* DEV_NVDEC, DEV_VIC: the fd's host1x channel (host1x_open) */
    /* the ioctls running on the fd. Close during one only marks it closed, and the last ioctl to leave releases it:
       the entry and its channel stay while an ioctl uses them, and Open doesn't hand the fd out again meanwhile */
    u32 users;
    int closed;
} NvFile;
/* a (device, ioctl) pair logged already */
typedef struct {
    int dev;
    u32 cmd;
} NvSeen;
#define NV_GSV_COUNTERS 192
typedef struct {
    u32 rw, ro; /* retained KO_TMEM ids */
    u32 request, completed; /* retained readable KO_EVENT ids, under nvev_lk */
    u64 previous[NV_GSV_COUNTERS]; /* last allocated observation, extended across 32-bit counter wrap */
} NvGsvCache;
/* nv_init sets the two fields that don't start at 0 (nvh_next, big_page_bits) and the locks */
typedef struct {
    NvMapH nvh[NVMAP_HANDLES];
    u32 nvh_next; /* handles below this have been handed out */
    /* freed handles, handed out again before nvh_next grows: movies create and free buffers for every clip, and a
       handle past the table would be written outside it */
    u32 nvh_free[NVMAP_HANDLES];
    u32 nvh_nfree;
    u64* gmmu[NV_GMMU_L1]; /* gmmu[i] -> GMMU_L2_ENTRIES page entries (nvdrv.c's GMMU_PTE_*) */
    GMap* gmaps;
    u32 ngmaps, capgmaps;
    /* the GPU page table again as runs, sorted by GPU page and disjoint, changed with it (gmmu_map, gmmu_unmap: every
       mapping, MAP_BUFFER_EX's, REMAP's and FREE_SPACE's, goes through them), and their index by guest page. Both
       under alias_lock, which is taken last: inside aslock and the renderer's mapping bracket */
    GRun* gruns;
    u32 ngruns, capgruns;
    GAlias alias;
    u32 big_page_bits; /* 64K (AS_BIG_PAGE_BITS_64K) until ALLOC_AS_EX asks otherwise */
    VaRange* vafree;
    u32 nvafree, capvafree;
    int va_ready; /* the free ranges hold the whole region at first (va_init) */
    u32 syncpt[SYNCPT_IDS];
    u32 syncpt_max[SYNCPT_IDS];
    u8 syncpt_used[SYNCPT_N];
    u8 syncpt_gpu[SYNCPT_N]; /* a GPU channel's: submissions promise its increments (syncpt_max), the GPU thread makes
                                them (sync_lk) */
    /* each ctrl fd's event slots, a table of ctrl_evt taken on first use and given back with the fd, under
       nvev_lk: 1 + its index, 0 for none. As on the console, the slots are the fd's (Ryujinx's NvHostCtrlDeviceFile
       keeps them per open). A game's GPU fence waits (one fd) and its movie player (another) both use slot 0,
       and with one table between them a thread resetting its event took the other's wake, which then waited for
       good */
    u32 ctrl_ev[NV_FDS];
    NvEventTable ctrl_evt[NV_CTRL_TABLES];
    u32 ctrl_evt_next;
    NvGsvSync gsv_sync[NV_GSV_SYNCS];
    u32 gsv_sync_next;
    u32 gsv_sync_active; /* event-delivery membership under nvev_lk, separate from IPC pool allocation */
    NvGsvCache gsv_cache;
    NvGsvGpu gsv_gpu[NV_GSV_GPUS];
    u32 gsv_gpu_next;
    NvGsvWorker gsv_workers[NV_GSV_GPUS];
    u32 gsv_worker_next;
    NvGsvDevices gsv_devices[NV_GSV_GPUS];
    u32 gsv_devices_next;
    NvGsvAs gsv_as[NV_GSV_ADDRESS_SPACES];
    u32 gsv_as_last; /* last id issued, never wrapped or reused */
    NvGsvMemory gsv_mem[NV_GSV_MEMORIES];
    u32 gsv_mem_next; /* where the search for a free registration starts */
    NvGsvSparse gsv_sparse[NV_GSV_SPARSE_REGIONS];
    NvGsvImage gsv_images[NV_GSV_IMAGES];
    u32 gsv_image_last; /* the last image handle issued */
    /* a game thread arms a slot while GPU and video threads run nv_events_check after their increments: syncpt_add's
       at once, syncpt_add_quiet's at the outermost vk_unlock, after vk_lock goes (syncpt_events). Arming clears the
       event first and does both in one hold: an increment between a separate arm and clear would signal and then
       lose its wakeup, leaving the game waiting for good. A mutex, not a spin: kevent_signal runs under it */
    NvFile nvf[NV_FDS];
    u64 tsg_last;
    NvSeen seen[256];
    u32 nseen;
} HleNvdrv;
/* nvdrv's host side: locks and condition variables, which a save state never holds (hle.h's g_host) */
typedef struct {
    PMutex lock;
    PMutex aslock; /* held across ioctl_as: the mappings below and the VA allocator */
    PMutex alias_lock;
    PCond sync_cv;
    PMutex sync_lk;
    PMutex submit_lk; /* a submission's promise and its place in the GPU queue, in the same order */
    PMutex nvev_lk;
    /* nv_first_use's (device, ioctl) pairs, under seen_lock */
    PMutex seen_lock;
} HostNvdrv;
/* nvdrv's thread state, t_hle.nvdrv (hle.h's) */
typedef struct {
    /* this thread's syncpt_add_quiet moved a syncpoint: its syncpt_events checks the event slots where syncpt_add would
       have, so the same thread takes the kernel's lock, after vk_lock goes */
    int events_due;
} TlsNvdrv;
extern const StateFnTable g_nvdrv_state_fns; /* the functions a save state may hold of nvdrv.c */

#endif
