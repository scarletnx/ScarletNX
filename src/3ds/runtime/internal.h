/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_PROCESS_INTERNAL_H
#define SNX_CTR_PROCESS_INTERNAL_H
#include "3ds/runtime/ctr_process.h"
#include "3ds/gpu/y2r.h"
#include "3ds/kernel/ctr_kernel.h"
#include "3ds/services/romfs.h"
#define CTR_HANDLES        256u
#define CTR_OBJECTS        256u
#define CTR_GSP_THREADS    4u
#define CTR_PHYS_BASE      0x20000000u
#define CTR_CONFIG         0x1ff80000u
#define CTR_SHARED_PAGE    0x1ff81000u
#define CTR_THREAD_LIMIT   32u                       /* the APPLICATION resource limit's threads */
#define CTR_TLS_BYTES      (CTR_THREAD_LIMIT * 512u) /* one 512-byte TLS slot per thread from SNX_CTR_INITIAL_TLS */
#define CTR_HEAP           0x08000000u
#define CTR_HEAP_END       0x10000000u
#define CTR_LINEAR         0x14000000u
#define CTR_PROCESS_HANDLE 0xffff8001u
#define CTR_THREAD_HANDLE  0xffff8000u
/* Physical page ownership, not a replacement for the checked page controller. */
enum { CTR_UNUSED, CTR_STATIC, CTR_HEAP_PAGE, CTR_LINEAR_PAGE };
#define CTR_PROCESS_ID 0x10u
enum {
    CTR_OBJ_FREE,
    CTR_OBJ_PROCESS,
    CTR_OBJ_THREAD,
    CTR_OBJ_ARBITER,
    CTR_OBJ_SESSION,
    CTR_OBJ_MUTEX,
    CTR_OBJ_EVENT,
    CTR_OBJ_SHARED,
    CTR_OBJ_FILE,
    CTR_OBJ_SEMAPHORE,
    CTR_OBJ_TIMER,
    CTR_OBJ_RESOURCE_LIMIT /* the process's APPLICATION limits, made at its first GetResourceLimit */
};
/* event and timer reset types (Azahar kernel ResetType) */
enum { CTR_RESET_ONESHOT, CTR_RESET_STICKY, CTR_RESET_PULSE };
#define CTR_TIMED_OUT      0x09401bfeu
/* the APPLICATION resource limit (Azahar kernel/resource_limit.cpp): the best priority a thread may have */
#define CTR_PRIORITY_LIMIT 0x18u
/* file.kind */
enum { CTR_FILE_VIEW, CTR_FILE_HOST, CTR_FILE_DIR };
#define CTR_SELF_ARCHIVES 32u
/* FS-private read-only SelfNCCH bindings, never kernel handles. Tokens are
   monotonically issued and never reused; the process owns their RomFS backing. */
typedef struct {
    uint64_t token;
    uint32_t kind; /* 0: SelfNCCH RomFS view (read-only), 1: SDMC host area, 2: SaveData host area (read-write),
                      3: an ExtSaveData's user folder (read-write, its files' sizes fixed) */
    FsNode* root;  /* kind 3: the user folder, retained while the archive is open */
} CtrSelfArchive;
typedef struct {
    SnxArmCPU context; /* inactive architectural prefix only; never the checked adapter */
    uint32_t priority, dead, waiting_object, waiting_address;
    uint32_t core; /* the processor it was created for: 0 the application's, 1 the system's (2 and 3 the New 3DS's) */
    /* Host monotonic deadline, 0 for none. A sleeping thread (no waiting_object) is not runnable before it,
       UINT64_MAX sleeping forever; a waiting thread times out at it (CTR_TIMED_OUT), and waits without one forever */
    uint64_t wake_ns;
    uint32_t yield; /* SleepThread(0): runs again only when no other thread can, at the next scheduling */
    /* Queue order among equal priorities, as Azahar's per-priority ready and wait lists keep it: ready_seq is taken
       each time the thread becomes runnable (a preempted thread keeps its own, so it stays first), wait_seq each time
       it starts waiting */
    uint64_t ready_seq, wait_seq;
    /* WaitSynchronizationN on no handles with a deadline: it wakes there with Timeout and -1 (Azahar's wait-any) */
    uint32_t wait_nothing;
    uint32_t wait_multiple, wait_all, wait_more_count, wait_more[63];
    char waiting_service[9];
} CtrThread;
typedef struct {
    uint32_t type, refs;
    uint32_t from_app; /* made by the application's own SVC, so counted against its resource limit */
    union {
        struct {
            uint32_t service, client_pid, sdk_version, gsp_thread;
            uint32_t name; /* SNX_CTR_SERVICE_SYSTEM's: which of them (snx_ctr_system_name) */
        } session;
        CtrThread thread;
        struct {
            uint32_t closed, kind, writable;
            uint32_t fixed;    /* an ExtSaveData file: Write stops at its end and SetSize changes nothing */
            uint32_t priority; /* SetPriority's, given back by GetPriority */
            uint32_t position; /* CTR_FILE_DIR: the entries Read has given so far */
            void* host;        /* FILE* for CTR_FILE_HOST */
            void* node;        /* FsNode*, retained for the file's lifetime */
        } file;
        struct {
            uint32_t physical, size, permissions;
            /* a block the application made with CreateMemoryBlock: its own range at address (heap-backed, Locked
               while the block lives, physical 0) or a new zeroed one (address 0); permissions are then its own and
               other_permissions everyone else's (0x10000000: don't care) */
            uint32_t address, other_permissions, by_application;
        } shared;
        struct {
            uint32_t owner, depth;
        } mutex;
        struct {
            uint32_t signaled, reset_type;
        } event; /* one-shot, sticky or pulse */
        struct {
            uint32_t signaled, reset_type;
            uint64_t deadline, interval; /* host monotonic ns of the next signal (0: not set), period (0: once) */
        } timer;
        struct {
            uint32_t count, max;
        } semaphore;
    } data;
} CtrObject;
typedef struct {
    uint32_t va, size, object;
} CtrSharedMap;
typedef struct {
    uint32_t generation, object;
} CtrHandle;
/* gsp::Gpu model: one shared GX command/interrupt block, per-registration relay threads and latched buffer swaps.
   Display timing is host-paced 60 Hz, not cycle accurate. */
typedef struct {
    uint32_t shared_object;              /* CTR_OBJ_SHARED backing the 0x1000 block */
    uint32_t thread_mask, rights_thread; /* the session object holding the GPU right, 0xffffffff: none */
    uint32_t first_registration;         /* one genuine FirstInitialization reply */
    uint32_t lcd_black, frame_seq, vblank_count;
    uint32_t event_object[CTR_GSP_THREADS]; /* registered interrupt events (retained) */
    uint32_t relay_flags[CTR_GSP_THREADS];
    uint32_t fb_pending[2]; /* a swap (SetBufferSwap or a FrameBufferUpdate block) latched since the last present */
    /* each screen's framebuffer_config registers: two address pairs (left/right eye), the one active selects which
       pair the LCD scans out; a swap writes the pair its active_fb names (Azahar video_core/gpu.cpp) */
    struct {
        uint32_t left[2], stride, format, active;
    } fb_screen[2];
    uint64_t next_vblank_ns; /* host monotonic deadline, 60 Hz */
    /* the transfer engine's registers (DisplayTransferConfig), physical addresses: a GX command writes some of them
       and triggers it, the rest keep their last values, as on hardware (gpu/transfer.h) */
    struct {
        uint32_t input, output, output_size, input_size, flags;
        uint32_t copy_size, copy_input, copy_output;
    } transfer;
    struct SnxCtrPica* pica; /* the PICA command processor, made on the first SubmitCmdList (gpu/pica.h) */
} SnxCtrGsp;
/* dsp::DSP: the HLE DSP (audio/dsp.h) and the events the application registered for its interrupts */
typedef struct {
    struct SnxCtrDsp* core;    /* made at the first dsp::DSP session */
    uint32_t interrupt[2 + 8]; /* event objects (retained): interrupts 0 and 1, then one per pipe */
    uint32_t semaphore;        /* the semaphore event (retained), made at its first GetSemaphoreEventHandle */
    uint16_t semaphore_mask;
    uint64_t next_frame_ns; /* host monotonic deadline of the next audio frame */
} SnxCtrDspService;
/* y2r:u: the Y2R engine's configuration as the application set it, and the conversion running (services/y2r.c) */
typedef struct {
    SnxCtrY2rConfig config;
    SnxCtrY2rBuffer streams[SNX_CTR_Y2R_STREAMS]; /* Y, U, V and the output, as SetSending and SetReceiving gave them */
    SnxCtrY2rBuffer yuyv;                         /* SetSendingYUYV's, which the interleaved input reads */
    uint16_t dithering[16];
    uint8_t spacial_dithering, temporal_dithering, transfer_end_interrupt;
    uint32_t event;   /* the transfer end event (retained), made at the first GetTransferEndEvent or DriverInitialize */
    uint64_t done_ns; /* host monotonic end of the conversion running, 0 when none is */
    uint64_t serial;  /* the GPU submission it runs in: it ends no sooner than that is done */
} SnxCtrY2rService;
/* hid:USER's sampling (services/hid.c): each update's next deadline, host monotonic ns (0: not running), the index
   each writes next, how many times the game enabled each sensor, and the circle pad's last two raw positions, which
   each sample averages with */
typedef struct {
    uint64_t next_pad_ns, next_accelerometer_ns, next_gyroscope_ns;
    uint32_t pad_index, touch_index, accelerometer_index, gyroscope_index;
    uint32_t accelerometer_users, gyroscope_users;
    int32_t circle_history[2][2];
} SnxCtrHid;
struct SnxCtrProcess {
    SnxCtrRomfs* romfs; /* runtime-owned immutable host filesystem view */
    FsTree* tree;       /* the shared NX-style tree: romfs/exefs read-only, sdmc read-write */
    FsNode* sd;         /* the "sdmc" subtree root (read-write) */
    FsNode* save;       /* the "savedata" subtree root (read-write), NULL until the game formats its save data */
    SnxCtrMemory* memory;
    SnxCtrAddressSpace* space;
    SnxCtrCpu cpu;
    uint32_t physical_next, physical_end, app_limit, physical_shared;
    CtrSharedMap shared_maps[8]; /* MapMemoryBlock registrations, kernel ordered */
    /* ControlMemory MAP pairs: target mirrors the heap pages at source; both stay out of FREE and PROTECT until the
       matching UNMAP (Azahar's Aliased and Alias memory states) */
    struct {
        uint32_t target, source, size;
    } aliases[16];
    CtrObject objects[CTR_OBJECTS];
    CtrHandle handles[CTR_HANDLES];
    uint32_t last_svc, current_thread;
    /* the cores take turns on the one host thread: the one the next scheduling serves, and each one's thread last run */
    uint32_t next_core, core_thread[4];
    uint32_t last_ipc_service, last_ipc_header;
    uint32_t apt_lock;         /* manager-owned object index; independent of client sessions */
    uint32_t srv_notification; /* srv:'s notification semaphore (retained), made at the first EnableNotification */
    uint32_t apt_phase, apt_notification, apt_parameter;
    uint32_t hid_objects[6]; /* the shared memory, then the events: pad 1, pad 2, accelerometer, gyroscope, debug pad */
    SnxCtrHid hid;
    uint32_t shell_closed; /* virtual console starts with its shell open */
    double clock_origin;   /* host monotonic origin for the base ARM11 tick domain */
    CtrSelfArchive self_archives[CTR_SELF_ARCHIVES];
    uint64_t archive_sequence;
    uint32_t fs_priority; /* fs:USER's SetPriority, UINT32_MAX until one */
    SnxCtrGsp gsp;
    /* ldr:ro's static module (services/ldr.c): where Initialize mapped the CRS and the buffer it was mapped from, 0
       until it did */
    uint32_t ldr_crs, ldr_crs_size, ldr_buffer;
    /* the loaded CROs, in a list of their own because the guest can rewrite the headers that would also link them */
    struct SnxCtrLdrModule* ldr_modules;
    size_t ldr_count, ldr_capacity;
    /* the open transaction that journals the writes of one ldr:ro command (services/cro.c), NULL between commands */
    struct SnxCtrCroTransaction* cro_transaction;
    /* one bit per 128 bytes of the static text (aot/dispatch.c): set where an AOT block may hold a word a CRO's
       relocation rewrote, and allocated by the first such rewrite */
    uint32_t* aot_stale;
    SnxCtrDspService dsp;
    struct SnxCtrUds* uds;
    SnxCtrY2rService y2r;
    uint64_t svc_count, svc_counts[256]; /* all, and each SVC's */
    uint64_t idle_ns, idle_waits;        /* the host time the scheduler slept with no thread to run, and how often */
    /* the codeset GetProcessInfo describes: segments as loaded, BSS, title and name (ExHeader) */
    SnxCtrSegment segment[3];
    uint32_t bss_size, stack_size; /* the main thread's stack, page-rounded, below SNX_CTR_STACK_TOP */
    uint64_t title_id;
    char name[8];
    char title[192]; /* the SMDH's short title (English, else the first language that has one), UTF-8; "" without */
    uint32_t region_lockout; /* the SMDH's regions the title runs in, bit per CFG region (0 JPN ... 6 TWN), 0 unknown */
    uint32_t resource_limit; /* CTR_OBJ_RESOURCE_LIMIT object, 0 until the first GetResourceLimit */
    uint32_t ipc_stubs;      /* services whose unknown commands answer success (bit per SNX_CTR_SERVICE_*), with
                                ctr_auto_stub_ipc off */
    /* the services the runtime has no model of and does not know, which the game asked for: each a stand-in
       session's name (services/system.c) */
    char stand_ins[16][9];
    uint32_t stand_in_count;
    /* the stubbed commands already reported, service << 16 | command, so each is logged once */
    uint32_t stub_logged[256], stub_logged_count;
    uint64_t time_update_ns; /* host monotonic deadline of the shared page's next hourly time entry */
    uint64_t sched_seq;      /* the last ready_seq/wait_seq handed out */
    /* the block at pc the static module lacks, compiled while the game runs (aot/jit.c) when the driver sets it: NULL
       when it cannot be. stale drops the one compiled before, whose code changed */
    SnxArmBlock (*compile)(void* compiler, uint32_t pc, bool thumb, bool stale);
    void* compiler;
    /* the blocks compiled so far dropped (aot/jit.c), when the driver sets it: a CRO loaded or unloaded at an address
       would otherwise run the blocks a previous occupant left there. NULL in a build without the run-time compiler */
    void (*flush)(void* compiler);
    /* set by flush, and run by the dispatcher before its next block */
    void (*reclaim)(void* compiler);
    unsigned char physical_owner[];
};
CtrThread* snx_ctr_thread(SnxCtrProcess* p);
/* The static text in [address, address + size) was rewritten: the AOT blocks that can hold any of it stop being used.
   False when the bit map cannot be allocated. */
bool snx_ctr_aot_mark_stale(SnxCtrProcess* p, uint32_t address, uint32_t size);
uint32_t snx_ctr_thread_create(SnxCtrProcess* p, const uint32_t* registers, uint32_t* out);
/* snx_ctr_schedule's answer: no thread will ever run again, a thread runs now, or the host slept to a vblank or an
   audio frame that woke nobody (the caller can look at its window, then schedule again) */
typedef enum { SNX_CTR_SCHED_BLOCKED, SNX_CTR_SCHED_RUN, SNX_CTR_SCHED_IDLE } SnxCtrSchedule;
SnxCtrSchedule snx_ctr_schedule(SnxCtrProcess* p);
/* a thread becomes runnable: it goes behind the runnable threads of its priority */
void snx_ctr_thread_ready(SnxCtrProcess* p, uint32_t thread);
/* the priority a thread runs at: its own, or a better one inherited from the waiters of mutexes it holds */
uint32_t snx_ctr_thread_priority(SnxCtrProcess* p, uint32_t thread);
/* every object's effective priority at once (CTR_OBJECTS entries; 64 for what is not a thread) */
void snx_ctr_thread_priorities(SnxCtrProcess* p, uint32_t* priority);
/* an object type a thread can wait on */
bool snx_ctr_waitable(uint32_t type);
/* host monotonic nanoseconds, the clock every deadline is on */
uint64_t snx_ctr_now_ns(void);
SnxCtrSvcResult snx_ctr_wait_many(SnxCtrProcess* p, uint32_t* registers);
void snx_ctr_wake_waiters(SnxCtrProcess* p, uint32_t object);
/* The current thread's wait or sleep deadline from an SVC's s64 nanoseconds (low, high words): -1 is none, another
   negative value is already due */
void snx_ctr_thread_deadline(SnxCtrProcess* p, uint32_t low, uint32_t high);
/* ExitThread: the current thread ends; SNX_CTR_SVC_EXIT when it was the process's last */
SnxCtrSvcResult snx_ctr_thread_exit(SnxCtrProcess* p);
/* A timer reaching its deadline (or set with no initial delay): signaled, waiters woken, rescheduled by its interval */
void snx_ctr_timer_signal(SnxCtrProcess* p, uint32_t object, uint64_t now_ns);
void snx_ctr_object_release(SnxCtrProcess* p, uint32_t object);
void snx_ctr_thread_block(SnxCtrProcess* p, uint32_t object, uint32_t address);
uint32_t snx_ctr_handle_object(SnxCtrProcess* p, uint32_t handle);
uint32_t snx_ctr_handle_add(SnxCtrProcess* p, uint32_t object, uint32_t* out);
uint32_t snx_ctr_object_create(SnxCtrProcess* p, uint32_t type, uint32_t* out);
uint32_t snx_ctr_handle_close(SnxCtrProcess* p, uint32_t handle);
uint32_t snx_ctr_session_create(SnxCtrProcess* p, uint32_t service, uint32_t* out);
/* The shared page's time entry, written at process creation and every hour since (the scheduler calls it) */
void snx_ctr_shared_page_time(SnxCtrProcess* p);
/* HLE shared pages live past the application pool but inside board FCRAM;
   each call reserves the next page-aligned block, or returns 0. */
uint32_t snx_ctr_shared_pages(SnxCtrProcess* p, uint32_t size);
uint32_t snx_ctr_named_connect(SnxCtrProcess* p, uint32_t name_address, uint32_t* out);
SnxCtrSvcResult snx_ctr_ipc_send(SnxCtrProcess* p, uint32_t handle, uint32_t* transport_result);
/* SVC helpers use only RAM transactions, never MMIO or a flat pointer. */
uint32_t snx_ctr_kernel_memory(SnxCtrProcess* p, uint32_t operation, uint32_t va, uint32_t source, uint32_t size,
                               uint32_t permissions, uint32_t* output);
/* Loader aliases use private heap or data backing, and the loader registers their lifetime before it returns. */
bool snx_ctr_kernel_loader_map(SnxCtrProcess* p, uint32_t target, uint32_t source, uint32_t size);
/* Private read-write application data, possibly unaligned, with no outstanding alias or service owner. */
bool snx_ctr_kernel_private(SnxCtrProcess* p, uint32_t address, uint32_t size);
#endif
