/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VITA_PROCESS_H
#define SNX_VITA_PROCESS_H
#include "common/cpu/arm/arm.h"
#include "vita/aot/module.h"
#include "vita/loader/image.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The guest's address space: every 32-bit address, a host reservation the guest's addresses are offsets into (the
   block compiler's flat memory, SnxArmCPU.mem), pages made accessible as the game allocates them. Where things go:
   the runtime's own page of code (the trampolines below) and the threads' TLS, the image at VITA_LOAD_BASE, the
   memory blocks the game allocates (user memory above it, the GPU's CDRAM in its own range) */
#define VITA_SPACE         (1ull << 32)
#define VITA_RUNTIME_PAGE  0x80000000u
#define VITA_TLS_BASE      0x80010000u
#define VITA_TLS_SIZE      0x1000u /* each thread's: sceKernelGetTLSAddr's slots, then the TLS register's */
#define VITA_TLS_ERRNO     0xffcu  /* in each thread's: its errno (SceLibc's _sceLibcErrnoLoc) */
#define VITA_TLS_NET_ERRNO 0xff8u  /* in each thread's: SceNet's errno (sceNetErrnoLoc) */
#define VITA_USER_BASE     0x82000000u
#define VITA_USER_END      0xa0000000u
#define VITA_CDRAM_BASE    0x60000000u
#define VITA_CDRAM_END     0x70000000u
#define VITA_THREADS       64u
#define VITA_OBJECTS       1024u
#define VITA_FILES         128u
#define VITA_ALLOCATIONS   1024u
#define VITA_FIBERS        128u

/* The runtime's SVCs (immediates from VITA_SVC_RUNTIME, below which an SVC is an import's), from its trampolines at
   VITA_RUNTIME_PAGE: a thread's entry returning (lr), a display queue entry's callback being due and returning */
enum {
    VITA_SVC_THREAD_RETURN = VITA_SVC_RUNTIME,
    VITA_SVC_DISPLAY_NEXT,
    VITA_SVC_DISPLAY_DONE,
    VITA_SVC_FIBER_RETURN,
    VITA_SVC_CALL_RETURN,
};
/* the trampolines' addresses, A32 code */
#define VITA_THREAD_RETURN   (VITA_RUNTIME_PAGE + 0x00u)
#define VITA_DISPLAY_LOOP    (VITA_RUNTIME_PAGE + 0x10u)
#define VITA_RETURN          (VITA_RUNTIME_PAGE + 0x20u) /* bx lr: a function that does nothing */
#define VITA_FIBER_RETURN    (VITA_RUNTIME_PAGE + 0x24u) /* a fiber's entry returning (its lr) */
#define VITA_CALL_RETURN     (VITA_RUNTIME_PAGE + 0x2cu) /* a guest function an import called returning (its lr) */

/* SceKernel's error codes (psp2/kernel/error.h), and errno's as the libraries return them (0x80010000 | errno) */
#define SCE_OK                              0
#define SCE_KERNEL_ERROR_ERROR              0x80020001u
#define SCE_KERNEL_ERROR_INVALID_ARGUMENT   0x80020005u
#define SCE_KERNEL_ERROR_ILLEGAL_PRIORITY   0x80028023u
#define SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID  0x80028021u
#define SCE_KERNEL_ERROR_ILLEGAL_TYPE       0x8002000cu
#define SCE_KERNEL_ERROR_NO_MEMORY          0x8002710bu
#define SCE_KERNEL_ERROR_UNKNOWN_UID        0x80028001u
#define SCE_KERNEL_ERROR_WAIT_TIMEOUT       0x80028005u
#define SCE_KERNEL_ERROR_WAIT_CANCEL        0x80028007u
#define SCE_KERNEL_ERROR_NOT_DORMANT        0x80028028u
#define SCE_KERNEL_ERROR_LW_MUTEX_UNLOCKED /* LW_MUTEX_UNLOCK_UDF */  0x80028184u
#define SCE_KERNEL_ERROR_LW_MUTEX_NOT_OWNED 0x80028186u
#define SCE_KERNEL_ERROR_MUTEX_UNLOCKED /* MUTEX_UNLOCK_UDF */     0x80028144u
#define SCE_KERNEL_ERROR_MUTEX_NOT_OWNED    0x80028146u
#define SCE_KERNEL_ERROR_MUTEX_RECURSIVE    0x80028142u
#define SCE_KERNEL_ERROR_SEMA_ZERO          0x80028102u
#define SCE_KERNEL_ERROR_SEMA_OVF           0x80028103u
#define SCE_KERNEL_ERROR_EVF_COND           0x800280e3u
#define SCE_ERRNO(e)                        (0x80010000u | (e))

enum { VITA_THREAD_FREE, VITA_THREAD_DORMANT, VITA_THREAD_READY, VITA_THREAD_WAITING, VITA_THREAD_DEAD };
/* What a waiting thread waits for. The scheduler asks vita_wait_try whether it is met each time it looks for a
   thread to run, as there are few threads; a met wait sets the thread's r0 and makes it ready */
enum {
    VITA_WAIT_DELAY,      /* until the deadline */
    VITA_WAIT_SEMA,       /* object, a = the count it takes */
    VITA_WAIT_MUTEX,      /* object, a = the count it locks with */
    VITA_WAIT_THREAD_END, /* object (the thread), out = its exit status's address */
    VITA_WAIT_EVENT_FLAG, /* object, a = pattern, b = mode, out = the result's address */
    VITA_WAIT_VBLANK,     /* a = the vblank count it waits past; b set: r0 kept, else 0 */
    VITA_WAIT_AUDIO,      /* until the deadline: an audio port's buffer played */
    VITA_WAIT_DISPLAY,    /* the display queue's thread, for its next entry (vita/hle/gxm.c) */
    VITA_WAIT_GXM,        /* object-less: the GPU's queue drained (vita/hle/gxm.c) */
    VITA_WAIT_COND,       /* object, a = the mutex it relocks */
    VITA_WAIT_RWLOCK,     /* object, a = 1 to write, 0 to read */
    VITA_WAIT_SIGNAL,     /* until sceKernelSendSignal sets the thread's signal_pending */
};
typedef struct {
    uint32_t kind, object, a, b, out;
    uint32_t timeout;  /* the guest's SceUInt32* timeout in microseconds, written with what is left; 0 for none */
    uint64_t deadline; /* host monotonic ns, 0 for none */
} VitaWait;
struct VitaProcess;
struct VitaThread;
typedef struct {
    uint32_t lr, sp;
    uint32_t (*next)(struct VitaProcess* p, struct VitaThread* t, void* context);
    void* context;
} VitaCallFrame;
typedef struct VitaThread {
    SnxArmCPU cpu;
    uint32_t state, uid;
    char name[32];
    uint32_t entry, priority, attr, stack, stack_size, stack_block, tls;
    /* the module's thread-local data for this thread (VitaImage.tls_*), made at its first use; 0 until then */
    uint32_t module_tls;
    /* the fiber it runs (an index in VitaProcess.fibers plus 1, 0 for its own code), and its own registers meanwhile,
       with where sceFiberRun writes what the fiber returns to it */
    uint32_t fiber, fiber_out;
    SnxArmCPU own;
    /* a guest function called by an import, with nested guest calls supported */
    struct {
        bool active, redirected, in_callback;
        uint32_t lr, sp;
        uint32_t (*next)(struct VitaProcess* p, struct VitaThread* t, void* context);
        void* context;
        VitaCallFrame stack[16];
        unsigned depth;
    } call;
    int32_t exit_status;
    /* sceKernelSendSignal's flag: set until the thread's next sceKernelWaitSignal takes it */
    bool signal_pending;
    VitaWait wait;
    uint64_t seq; /* when it last ran: the scheduler takes the ready thread that waited longest */
} VitaThread;

enum {
    VITA_OBJ_FREE,
    VITA_OBJ_THREAD,
    VITA_OBJ_SEMA,
    VITA_OBJ_MUTEX,   /* LwMutexes too: their work area's first word holds the UID */
    VITA_OBJ_EVENT_FLAG,
    VITA_OBJ_MEMBLOCK,
    VITA_OBJ_COND,
    VITA_OBJ_RWLOCK,
};
typedef struct {
    uint32_t type;
    char name[32];
    union {
        uint32_t thread; /* the index in threads */
        struct {
            int32_t count, max;
            uint32_t attr;
        } sema;
        struct {
            uint32_t owner; /* the owning thread's UID, 0 for none */
            int32_t count;
            uint32_t attr, work; /* work: an LwMutex's work area, 0 for a mutex */
        } mutex;
        struct {
            uint32_t pattern, attr;
        } event_flag;
        struct {
            uint32_t address, size, type;
        } memblock;
        struct {
            uint32_t mutex; /* the mutex's UID */
        } cond;
        struct {
            uint32_t writer; /* the writing thread's UID, 0 for none */
            int32_t writes, reads; /* the writer's locks (recursive ones), the readers' */
            uint32_t attr;
        } rwlock;
    } u;
} VitaObject;

typedef struct {
    uint32_t base, end, next; /* next: the lowest address that may be free */
} VitaRegion;
typedef struct {
    uint32_t address, size; /* size 0: free slot */
} VitaAllocation;

typedef struct VitaFile {
    void* host;   /* FILE*, or the directory's listing (VitaDirectory) */
    int directory;
} VitaFile;

/* A SceFiber (vita/hle/fiber.c): its registers while it doesn't run */
typedef struct {
    uint32_t address; /* the game's SceFiber, 0 for a free slot */
    uint32_t entry, argument, context, context_size;
    uint32_t running;   /* the thread running it's UID, 0 for none */
    uint32_t started;   /* its registers hold where it stopped */
    uint32_t in_out;    /* where sceFiberReturnToThread writes what the next sceFiberRun brings */
    SnxArmCPU cpu;
} VitaFiber;

struct VitaGxm;
struct VitaAudio;
struct VitaMp4;
struct VitaNet;
struct VitaTrophy;
struct VitaVideodec;
struct VitaAudiodec;
struct VitaNgs;
typedef struct VitaProcess {
    unsigned char* mem; /* the reservation */
    VitaImage image;
    const VitaModule* module;
    char* folder;  /* the game's folder */
    char* app;     /* app0: in it */
    char* patch;   /* the update's files, which app0: shows over app's */
    char* ux0;     /* ux0: in it */
    char* savedata; /* savedata0:, the game's save data: ux0/user/00/savedata/<its title ID>, as the console keeps it */
    char title_id[16]; /* from its param.sfo (the update's when it has one), else the folder's name */
    VitaThread threads[VITA_THREADS];
    VitaObject objects[VITA_OBJECTS];
    VitaFile files[VITA_FILES];
    VitaAllocation allocations[VITA_ALLOCATIONS];
    VitaRegion user, cdram;
    VitaThread* current;
    uint64_t seq;
    uint64_t start_ns;
    /* the display: the framebuffer sceDisplaySetFrameBuf last set, the vblanks so far and the next one's time */
    struct {
        uint32_t base, pitch, format, width, height;
        uint64_t vblanks, next_ns;
    } display;
    struct VitaGxm* gxm;
    struct VitaAudio* audio;
    /* the movie libraries (vita/hle/mp4.c, videodec.c, audiodec.c), made at their first use */
    struct VitaMp4* mp4;
    struct VitaVideodec* videodec;
    struct VitaAudiodec* audiodec;
    /* SceNgs's voices (vita/hle/ngs.c), made at sceNgsSystemInit */
    struct VitaNgs* ngs;
    bool exited;
    int32_t exit_code;
    /* the run-time compiler: blocks the module lacks (vita/runtime/jit.c), NULL without one */
    struct VitaJit* jit;
    uint64_t hle_calls;
    /* SceLibc's (vita/hle/libc.c): malloc's heap and its data (the ctype tables, gmtime's result), made at their first
       use; rand's state */
    struct {
        uint32_t heap, data, rand;
    } libc;
    VitaFiber fibers[VITA_FIBERS];
    /* the common dialog open (vita/hle/dialog.c): which one (0 for none), its status and the result it ends with */
    struct {
        uint32_t kind, status, result;
        /* the save data dialog's: the phase it is in, the mode it shows, the slot and the game's pointer for its result */
        uint32_t substatus, mode, slot, userdata;
    } dialog;
    /* the network's state (vita/hle/net.c), made at its first use */
    struct VitaNet* net;
    /* the trophies (vita/hle/trophy.c), made at their first use */
    struct VitaTrophy* trophy;
    /* each import's function (vita/hle/hle.c), looked up at its first call; unknown_logged: reported as missing */
    const void** hle;
    uint32_t* unknown_logged;
} VitaProcess;

/* The process for image: its memory, the image in it, the runtime's page, the main thread ready at module_start.
   folder is the game's (app0: is folder/app, ux0: folder/ux0). NULL with error filled */
VitaProcess* vita_process_new(const VitaImage* image, const char* folder, char* error, size_t error_size);
void vita_process_free(VitaProcess* p);

/* guest memory: host pointers (the caller keeps within what the guest has mapped), and accessible ranges */
static inline void* vita_ptr(VitaProcess* p, uint32_t address) { return p->mem + address; }
uint16_t vita_read16(VitaProcess* p, uint32_t address);
uint32_t vita_read32(VitaProcess* p, uint32_t address);
void vita_write16(VitaProcess* p, uint32_t address, uint16_t value);
void vita_write32(VitaProcess* p, uint32_t address, uint32_t value);
void vita_write64(VitaProcess* p, uint32_t address, uint64_t value);
/* a NUL-terminated guest string copied into out (truncated), false when address is 0 */
bool vita_string(VitaProcess* p, uint32_t address, char* out, size_t size);
bool vita_mapped(VitaProcess* p, uint32_t address, uint32_t size);
/* size bytes of new memory in region, aligned, accessible and zeroed: its address, 0 when there is no room */
uint32_t vita_alloc(VitaProcess* p, VitaRegion* region, uint32_t size, uint32_t align);
void vita_free(VitaProcess* p, uint32_t address);

/* objects: a UID for a new object of type (with its name), the object of a UID of type, the UID's release */
uint32_t vita_object_new(VitaProcess* p, uint32_t type, const char* name, VitaObject** out);
VitaObject* vita_object(VitaProcess* p, uint32_t uid, uint32_t type);
void vita_object_delete(VitaProcess* p, uint32_t uid);

/* A dormant thread running entry with r0 and r1 when started, on a new stack: its UID, or an error (negative) */
int32_t vita_thread_new(VitaProcess* p, const char* name, uint32_t entry, uint32_t priority, uint32_t stack_size,
                        uint32_t attr);
VitaThread* vita_thread(VitaProcess* p, uint32_t uid);
void vita_thread_start(VitaProcess* p, VitaThread* t, uint32_t r0, uint32_t r1);
/* the current thread waits: it stops running, and its wait is met later (vita_wait_try) */
void vita_wait(VitaProcess* p, VitaWait wait);
/* the current thread's deadline from a guest SceUInt32* timeout in microseconds (0: none) */
uint64_t vita_deadline(VitaProcess* p, uint32_t timeout);
uint64_t vita_now_ns(void);
/* the thread ends with status (its waiters are told by their wait) */
void vita_thread_end(VitaProcess* p, VitaThread* t, int32_t status);
bool vita_in_module_start(const VitaThread* t);

/* Runs the game until it exits, its window closes or seconds pass (0: no limit): 0, or 1 after a fault (reported) */
int vita_run(VitaProcess* p, double seconds);
#endif
