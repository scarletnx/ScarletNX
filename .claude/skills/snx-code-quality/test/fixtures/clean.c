/* near misses: none of these is a candidate; PaddedLocks is C11 code the checker still meets in src/ */
#include <stdalign.h>
#include <stdlib.h>
typedef unsigned int u32;
typedef struct {
    int has_divisor; /* a feature, no pointer of that name */
    int is_present;  /* two pointers: which one would it describe? */
    void* a;
    void* b;
} Features;
typedef struct {
    _Atomic u32 lock_1;
    alignas(64) _Atomic u32 lock_2;
} PaddedLocks;
typedef struct {
    u32 submitted;
    char pad[64 - sizeof(u32)]; /* completed on a line of its own */
    u32 completed;
} PaddedQueue;
typedef struct {
    u32 count;
    u32 only;
} OneAtomic;
typedef struct {
    u32 slots[16]; /* each slot an atomic: an array is not checked */
    u32 head;      /* one thread's, an index only */
    u32 tail;
} Ring;
extern u32 g_elsewhere_1;
extern u32 g_elsewhere_2;
static u32 g_alone;
static const u32 g_table[2] = {1, 2}; /* a table, not state */
typedef struct {
    u32* in;
} One;
static const char* two_owners(One* a, One* b) {
    a->in = (u32*)calloc(1, 4);
    b->in = (u32*)calloc(1, 4);
    /* a->x = calloc(1, 4096); a->y = calloc(1, 4096); in a comment */
    return "a->x = malloc(1); a->y = malloc(1);";
}
static void touch(PaddedQueue* q, OneAtomic* o, Ring* r) {
    __atomic_store_n(&r->slots[r->head % 16], 1, __ATOMIC_RELEASE);
    __atomic_fetch_add(&r->tail, 1, __ATOMIC_RELEASE);
    __atomic_fetch_add(&q->submitted, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&q->completed, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&o->only, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_alone, __atomic_load_n(&g_elsewhere_1, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
    (void)__atomic_load_n(&g_elsewhere_2, __ATOMIC_ACQUIRE);
}
static _Thread_local struct { /* one thread-local for the file thread state */
    u32 depth;
    int busy;
} t_clean;
