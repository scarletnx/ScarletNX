/* every rule the checker sees, once each, in strict ISO C99; Locks is C11 code it still meets in src/ */
#include <stdlib.h>
typedef unsigned int u32;
typedef unsigned char u8;
typedef struct {
    void* p;
    int is_present;
} Opt;
typedef struct {
    char* name;
    int has_name;
    u32 size;
} Named;
typedef struct {
    _Atomic u32 lock_1;
    _Atomic u32 lock_2;
} Locks;
typedef struct {
    u32 submitted; /* the CPU thread's */
    u32 completed; /* the GPU thread's */
} Queue;
typedef struct {
    u8* in;
    u8* out;
} Buffers;
static u32 g_first;
static u32 g_second;
static Buffers* buffers_new(void) {
    Buffers* b = (Buffers*)calloc(1, sizeof *b);
    if (!b) return NULL;
    b->in = (u8*)calloc(1, 4096);
    b->out = (u8*)malloc(4096);
    return b;
}
static void count(Queue* q) {
    __atomic_fetch_add(&q->submitted, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&q->completed, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&g_first, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_second, 1, __ATOMIC_RELEASE);
}
static _Thread_local u32 t_depth;
static _Thread_local int t_busy; /* a second loose thread-local: one struct holds them */
