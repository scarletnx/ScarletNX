/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the kernel's files (kernel.c, thread.c, sync.c, debug.c, ipc.c, the SVCs) share among themselves */
#ifndef SNX_KERNEL_INT_H
#define SNX_KERNEL_INT_H
#include "nx/hle/kernel/kernel.h"
#include "nx/hle/kernel/ipc.h"
#include "nx/loader/loader.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/rtapi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the guest thread a host thread runs (thread.c); not in plat.h, which C99 code includes too */
extern _Thread_local void* tls_thread;

/* The kernel's results (module 1), as RC(1, description) gives them */
#define R_OUTOFMEM      0xD001  /* 104 OutOfMemory */
#define R_BADSIZE       0xCA01  /* 101 InvalidSize */
#define R_BADADDR       0xCC01  /* 102 InvalidAddress */
#define R_BADMEMSTATE   0xD401  /* 106 InvalidCurrentMemory */
#define R_BADREGION     0xDC01  /* 110 InvalidMemoryRegion */
#define R_BADPRIO       0xE001  /* 112 InvalidPriority */
#define R_BADCORE       0xE201  /* 113 InvalidCoreId */
#define R_BADHANDLE     0xE401  /* 114 InvalidHandle */
#define R_BADCOMBO      0xE801  /* 116 InvalidCombination */
#define R_TIMEOUT       0xEA01  /* 117 TimedOut */
#define R_CANCELLED     0xEC01  /* 118 Cancelled */
#define R_OUTOFRANGE    0xEE01  /* 119 OutOfRange */
#define R_BADENUM       0xF001  /* 120 InvalidEnumValue */
#define R_NOTFOUND      0xF201  /* 121 NotFound */
#define R_BUSY          0xF401  /* 122 Busy */
#define R_SESSIONCLOSED 0xF601  /* 123 SessionClosed */
#define R_BADSTATE      0xFA01  /* 125 InvalidState */
#define R_LIMIT         0x10801 /* 132 LimitReached */
#define R_MSGTOOLARGE   0x20801 /* 260 MessageTooLarge */
#define R_NOTIMPL       0x4201  /* 33 NotImplemented (not SessionClosed's 0xF601) */

#define HANDLE_WAIT  0x40000000u /* the waiters bit of a mutex word, beside the owner's handle */
#define H_CUR_PROC   0xFFFF8001u /* pseudo-handles */
#define H_CUR_THREAD 0xFFFF8000u

/* one SVC as the trace ring records it: number, result, caller, the first arguments and an output */
typedef struct {
    u32 imm, res;
    u64 lr, a[4], o1;
} SvcRec;

/* ---- kernel.c: handles, waiting, events. _nl: the caller holds g_klock */
KObj* h_get_nl(u32 h);
/* no reference taken: only for an object that is never freed (the resource limit) */
KObj* h_get_type(u32 h, int t);
/* whether o can be waited on (WaitSynchronization), and whether it is signaled: an event while it is set, a thread
   once it has exited, the process never (the game never ends it) */
int kobj_waitable(const KObj* o);
int kobj_signaled(const KObj* o);
/* the last reference went: what each type does then (kobj_unref), and the destroys of the types kernel.c doesn't
   make itself */
void kobj_destroy(KObj* o);
void kthread_destroy(KObj* o);
void ksession_destroy(KObj* o);
void kwevent_destroy(KObj* o);
void kshmem_destroy(KObj* o);
void kcodemem_destroy(KObj* o);
/* wakes the threads whose wait of this kind (1 objects, 5 a pause) names o, under g_klock */
void kobj_wake_nl(KObj* o, int kind);
/* a save state's load: a thread asked to end does, letting go of held (thread.c) */
void kthread_retire_check(PMutex* held);
/* svc.c's side of a restored thread (thread.c's thread_resume_main): the rest of the wait it was blocked in, or its
   way out of an SVC it stopped on the way out of, and what an SVC a service never answered held */
void svc_resume(KThread* t);
void svc_resume_leave(KThread* t);
void svc_service_retry(KThread* t);
/* sync.c's: the rest of a wait that a restored thread was blocked in, the SVC's result and outputs in its registers */
void sync_resume(KThread* t);
int h_close(u32 h);
void waiter_add(KObj* o, KThread* t);
void waiter_del(KObj* o, KThread* t);
void wake(KThread* t, int result);
/* A thread blocked in the kernel waits on its word (kt_word), with no lock held: kt_seq reads it before the thread
   checks what it waits for, and kt_wait blocks while the word still holds that value, so a change made after the check
   ends the wait at once. kt_notify changes t's word and wakes it now; kt_notify_later changes it now and wakes it when
   the caller next lets go of g_klock (klock_unlock) or of core_lk, or waits itself, so that the woken thread doesn't
   wake only to wait for the lock its waker still holds. kt_wait lets go of held (if any) while it waits */
u32 kt_seq(const KThread* t);
void kt_notify(KThread* t);
void kt_notify_later(KThread* t);
void kt_flush(void);
void kt_wait(KThread* t, u32 seq, PMutex* held, i64 timeout_ns);
void klock_unlock(void);
void kobj_signal_nl(KObj* o);
int sleep_nl(KThread* t, i64 timeout);

/* ---- thread.c: threads and the guest cores */
int kthread_live_count(void);
/* a thread with no reference: the first handle takes one, as its host thread does from its start to its exit */
KThread* kthread_create(u64 entry, u64 arg, u64 stack_top, int prio, int core);
void kthread_start(KThread* t);
void svc_exit_thread(KThread* t);
void process_exit(int code);
/* gives the core to a waiting thread of higher priority, then waits for it again */
void core_preempt(KThread* t);
/* gives the core to a waiting thread of any priority. migrate 2 may run it on an idle core of its mask instead */
void core_yield(KThread* t, int migrate);
void core_set_prio(KThread* t, int prio); /* the effective priority, reordering the core's wait list */

/* ---- sync.c: mutexes, condition variables, address arbitration, WaitSynchronization */
u32 svc_arbitrate_lock(KThread* t, u32 owner, u64 addr, u32 tag);
u32 svc_arbitrate_unlock(u64 addr);
void prio_set_base_nl(KThread* t, int prio);
u32 svc_wait_cv(KThread* t, u64 maddr, u64 key, u32 tag, i64 timeout);
u32 svc_signal_cv(u64 key, i32 count);
u32 svc_wait_addr(KThread* t, u64 addr, u32 type, i32 value, i64 timeout);
u32 svc_signal_addr(u64 addr, u32 type, i32 value, i32 count);
u32 svc_wait_sync(KThread* t, u64 hptr, u32 n, i64 timeout, u32* out_idx);

/* ---- svc_mem.c: the memory SVCs */
u32 svc_query(u64 info, u64 addr);
u32 svc_map_memory(u64 dst, u64 src, u64 size);
u32 svc_map_process_memory(u64 dst, u32 process, u64 src, u64 size);
u32 svc_unmap_process_memory(u64 dst, u32 process, u64 src, u64 size);
u32 svc_unmap_memory(u64 dst, u64 src, u64 size);
u32 svc_map_shm(u32 h, u64 addr, u64 size, u32 perm);
u32 svc_unmap_shm(u32 h, u64 addr, u64 size);
void tmem_destroy(KObj* o);
u64 mem_total(void);
u64 used_memory(void);
u32 svc_set_heap_size(u64 size);
u32 svc_map_physical(u64 addr, u64 size);
u32 svc_unmap_physical(u64 addr, u64 size);
u32 svc_get_info(u32 id0, u32 h, u64 id1, u64* out);
u32 svc_set_mem_perm(u64 addr, u64 size, u32 perm);
u32 svc_set_mem_attr(u64 addr, u64 size, u32 mask, u32 attr);
/* the new object comes back in *out, for svc.c to make its handle */
u32 svc_tmem_new(u64 addr, u64 size, u32 perm, KTmem** out);
u32 svc_codemem_new(u64 addr, u64 size, KObj** out);
u32 svc_codemem_control(KObj* o, u32 op, u64 addr, u64 size, u32 perm);
/* ---- HLE state in g_hle (hle.h) */
/* The kernel objects each come from a pool (hle.h) guarded by g_host.kernel.pool_lk, a lock of its own that is only
   held to take or free a slot and is taken last, under g_klock or not: an object's last reference may go with g_klock
   held (a mutex waiter's on its owner thread). Ipc.c's sessions are its own pool's */
#define KTHREAD_MAX 256 /* threads not yet freed: the console's 96 live ones (THREAD_MAX), and room for those exited
                           whose handles are still open */
#define KEVENT_MAX  4096 /* readable events: those the game holds handles to (H_MAX), the ends of svcCreateEvent's
                            writable events, and those the services keep without a handle (one or two per interface
                            that has them): four times the handle table */
/* objects that are made with a handle: they outlive their handles only while an SVC in flight on another thread holds
   them, or a service keeps one in the handle's place (am's storage of a transfer memory) */
#define KHANDLED_MAX (H_MAX + KTHREAD_MAX)
#define KSHMEM_MAX   64 /* the services' shared memories (hid, fonts, time, irs: a few, made once each); the game
                           makes none, svcCreateSharedMemory isn't an application's SVC */

/* the handle table's entries (kernel.c) */
typedef struct {
    u32 obj; /* the object's id (kobj_id), 0 for a free entry */
    u16 linear;
    i16 next;
} HEntry;
/* svcCreateEvent's writable end: it signals and clears the readable end it holds a reference to (svc.c) */
typedef struct {
    KObj o;
    u32 r; /* the readable end's id */
} KWEvent;
/* svcCreateCodeMemory's object (svc_mem.c) */
typedef struct {
    KObj o;
    u64 addr, size, owner, slave;
} KCodeMem;
/* one of the game's exported functions, its name kept as its address in guest memory (debug.c) */
typedef struct {
    u64 addr, size, name;
} Sym;

/* kernel.c */
typedef struct {
    u64 title_id;
    u64 sysres_size; /* the npdm's system resource size */
    u64 next_tid;    /* the next thread's id, under g_klock */
    /* the crash report's ring of the last 64 SVCs: svc_ring_n counts every one recorded */
    SvcRec svc_ring[64];
    u32 svc_ring_n;
    /* The handle table: a handle is (linear id << 15) | index, as the console's: the linear id (1 to 0x7FFF, counting
       up and wrapping) tells a stale handle from the one now in its slot, and bits 30 and 31 stay clear, so a handle
       fits the owner field of a mutex word beside its waiters bit. Free entries are a list through next, with -1 at
       its end; entries above htop were never used. Under g_klock */
    HEntry htab[H_MAX];
    int hfree, htop;
    u16 hlinear;
    KObj process; /* the one process, for the pseudo-handle: never destroyed, and never signaled */
    KEvent events[KEVENT_MAX];
    u32 event_next;
} HleKernel;
/* kernel's host side: locks and condition variables, which a save state never holds (hle.h's g_host) */
typedef struct {
    PMutex klock;   /* g_klock: every list a waiter or signaller touches */
    PMutex pool_lk; /* the kernel objects' pools, here and in thread.c, svc.c and svc_mem.c */
} HostKernel;
/* thread.c */
typedef struct {
    /* The threads, under pool_lk. A thread's slot is freed with its last reference: its handles' and the one its host
       thread holds from kthread_start until it has let go of its core in svc_exit_thread */
    KThread threads[KTHREAD_MAX];
    u32 thread_next;
    u64 main_tid;
    int default_core; /* the main thread's, which core -2 names */
    /* A core is held by at most one thread at a time, which runs game code; the others wait for it. Host threads are
       all runnable at once and the host scheduler knows nothing of guest priorities, so the order among the threads of
       one core comes from here: a core that falls free goes to its best waiter (lowest priority value, then the
       earliest), never to whichever host thread wakes first. Every field below and each thread's on_core, core,
       core_next and core_ticket change under core_lk, which is taken after g_klock when both are held (sleep_nl lets
       go of its core with g_klock held), and never the other way round. Two are read without it, with __atomic loads:
       a thread's on_core by the thread itself (only it lets go of a core it holds, and it waits for one handed to it
       on its word), and the heads of core_wait by core_yield */
    u32 core_owner[NCORES];      /* the thread holding each core (kt_at), or 0 */
    u32 core_wait[NCORES];       /* the threads waiting for it, through core_next, in no order */
    int core_best[NCORES];       /* the best priority among them, 64 with none */
    u64 core_ticket;             /* arrival order, for waiters of one priority */
    /* each core's share of the host CPUs (thread.c's core_pin), and whether it was logged */
    int host_cpu[256], host_ncpu;
    int pin_said[NCORES];
    /* a game pause is in force (kernel_hold): set and cleared under g_klock, read with __atomic loads, and every
       holder's poll stays set meanwhile so that its game code reaches a hold point */
    int hold;
    /* the TLS page being handed out, eight threads to a page, and how many have it. Under g_klock */
    u64 tls_page;
    int tls_used;
} HleThread;
/* thread's host side: locks and condition variables, which a save state never holds (hle.h's g_host) */
typedef struct {
    /* each thread's wait word (kt_word), which it blocks on in the kernel's waits: whoever ends the wait changes the
       word, then wakes it (kt_notify) */
    u32 wword[KTHREAD_MAX];
    PMutex core_lk;
    /* for kernel_core_report, under core_lk: when each core's holder took it (plat_ns), and per core the time it was
       held and the time threads waited for it */
    u64 core_since[NCORES], core_held_ns[NCORES], core_wait_ns[NCORES];
    /* a save state's load (thread.c): the guest threads' host threads running, retire asking them to end, and the
       service wait each is in (kthread_wait), where it is woken to */
    int nhost, retire;
    struct {
        PCond* c;
        PMutex* m;
    } waiting_on[KTHREAD_MAX];
    /* the request each thread serves changed what a service holds (kthread_commit), until its SVC leaves */
    u8 committed[KTHREAD_MAX];
} HostThread;
/* thread's thread state, t_hle.thread (hle.h's) */
enum { KT_LATER = 16 };
typedef struct {
    /* the threads this host thread changed the word of with kt_notify_later, to wake once it lets go of its lock: a
       wait's end comes with the lock held, and a thread woken then would only wait again for that lock. Too many for
       the list are woken at once */
    struct {
        u32 n;
        u32 ix[KT_LATER];
    } later;
} TlsThread;
/* sync.c: the threads blocked in each kind of wait, in arrival order, linked by next_waiter (sync.c says how the
   lists are kept: one for mutexes, one per bucket of the word for the others). Under g_host.kernel.klock */
#define WAIT_BUCKETS 256
typedef struct {
    u32 mwait;                      /* wait_kind 2: on the mutex at wait_addr, for lock_owner to hand it over */
    u32 cvwait[WAIT_BUCKETS];       /* 3: on the condition variable wait_key, to take the mutex at wait_addr */
    u32 arbwait[WAIT_BUCKETS];      /* 4: on the address wait_addr */
    u32 bad_waits;                  /* the bad handles waited on, logged the first 20 times and every 65536th after */
} HleSync;
/* debug.c */
typedef struct {
    Sym* syms; /* sorted by address once the modules are loaded */
    u32 nsyms;
    /* the svcSleepThread count of each thread slot's thread at the last dump, from 0 again when a new thread (tid)
       takes the slot */
    struct {
        u64 tid, n;
    } sleeps[KTHREAD_MAX];
} HleDebug;
/* svc.c */
typedef struct {
    KObj reslimit; /* the process's resource limit, for GetInfo 9: one object, never destroyed */
    KWEvent wevents[KHANDLED_MAX];
    u32 wevent_next;
} HleSvc;
/* svcMapProcessMemory's view of code at dst, of size bytes of src, and src as it was mapped (snap, size bytes; NULL
   when it could not be kept): what a line the guest flushes (svc_code_line) is compared with */
typedef struct {
    u64 dst, src, size;
    u8* snap;
} KCodeView;
#define KCODEVIEW_MAX 64 /* the views a mod keeps at once: exlaunch's are its module's text and its hook pools */
/* svcMapMemory's view of src at dst, size bytes: src stays locked until svcUnmapMemory, so the view is the owner of
   that lock. A view is live while dst is still mapped in its state (the stack state, or normal memory in the alias
   region); one whose dst is gone owns nothing */
typedef struct {
    u64 dst, src, size;
} KMapView;
#define KMAPVIEW_MAX 256 /* the live threads' stacks (KTHREAD_MAX), and the fibers' and mods' views */
/* svc_mem.c */
typedef struct {
    /* the cores and priorities the npdm's kernel capabilities allow (svcGetInfo 0 and 1) */
    u64 core_mask, prio_mask;
    KShmem shmems[KSHMEM_MAX];
    KTmem tmems[KHANDLED_MAX];
    KCodeMem codemems[KHANDLED_MAX];
    u32 shmem_next, tmem_next, codemem_next;
    /* RandomEntropy's four words, and whether they are made */
    u64 entropy[4];
    int entropy_made;
    u64 local_region; /* UserExceptionContextAddress's page, 0 until made */
    KCodeView views[KCODEVIEW_MAX]; /* under g_host.svcmem.views_lk: a free one has size 0 */
    KMapView maps[KMAPVIEW_MAX]; /* under g_host.svcmem.map_lk: a free one has size 0 */
    u64 fit; /* memory_fit's: the most the game is told, measured as it starts (mem_fit); 0 for the console's */
} HleSvcMem;
/* svc_mem's host side: the views' lock, which a save state never holds (hle.h's g_host) */
typedef struct {
    PMutex views_lk;
    /* MapMemory, UnmapMemory and the attribute and permission changes, one at a time as the console's page table lock
       makes them: taken before views_lk and mem.c's locks, never after them. Guards g_hle.svcmem.maps too */
    PMutex map_lk;
} HostSvcMem;
/* until the npdm says (main.c): an application's */
#endif
