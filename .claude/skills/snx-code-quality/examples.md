# snx-code-quality: the rules with their wrong and right forms

Every C block here is strict ISO C99 (`-std=c99 -pedantic-errors`). Each wrong/right pair gives the same results
under ASan and UBSan, except where the right form differs on purpose. Read the rule in doubt.

## 1. No state another value already says

**Do:** let the value carry its own presence. A pointer is absent when it is NULL.

**Why:** two fields that say one thing drift apart. When `is_present` and `p` do not agree, every reader must guess
which one is true.

<!-- snippet: flag-wrong -->
```c
typedef struct {
    void* p;
    int is_present;
} Opt;
int opt_present(const Opt* o) { return o->is_present; }
```

<!-- snippet: flag-right -->
```c
typedef struct {
    void* p; /* NULL: not present */
} Opt;
int opt_present(const Opt* o) { return o->p != NULL; }
```

**Does not apply when:**
- NULL is a value of its own ("present and empty").
- Every value of the type is valid (a `u32` offset where 0 is a real offset). Then reserve zero first:
  leave slot or byte 0 unused, so 0 still means none, as in rule 2. Keep a flag only when you cannot reserve a
  value.
- The flag says something else: `has_keys` in `extract.c` says the keys file exists, while `keys`
  always holds a path.

## 2. Indices, not pointer chains

**Do:** keep the objects in one array and link them by index (`u32`), with 0 as none. Slot 0 of the array is
never a real object. Do not use `UINT32_MAX` or -1 for none. With 0, `if (!n)` reads like a null pointer check, a
compare with zero is the cheapest compare, and no index needs an out-of-range case. To sort a list, sort an array of
(key, index) pairs with `qsort`. Do not do a merge sort on `next` pointers.

**Why:** a walk through an array touches memory in order, but a chain of pointers jumps across the heap. An index
stays valid when the code reallocates the array, and when it writes the array out (rule 5). A pointer does neither.

<!-- snippet: indices-wrong -->
```c
typedef struct FsNode {
    struct FsNode* parent;
    int attached;
} FsNode;
typedef struct {
    FsNode* root;
} FsTree;
int belongs_to(const FsTree* tree, const FsNode* n) {
    if (!n || !n->attached) return 0;
    while (n->parent) {
        n = n->parent;
        if (!n->attached) return 0;
    }
    return n == tree->root;
}
```

<!-- snippet: indices-right -->
```c
typedef struct {
    u32 parent; /* 0 at the top */
    int attached;
} FsNode;
typedef struct {
    FsNode* nodes; /* every node of the tree; nodes[0] is none, so index 0 means no node */
    u32 root;
} FsTree;
int belongs_to(const FsTree* tree, u32 n) {
    if (!n || !tree->nodes[n].attached) return 0;
    while (tree->nodes[n].parent) {
        n = tree->nodes[n].parent;
        if (!tree->nodes[n].attached) return 0;
    }
    return n == tree->root;
}
```

**Does not apply when:**
- The code that holds the index cannot get to the array, or the array dies first. An index means nothing
  without its array, so pass the owner along.
- Objects come and go in the middle of the array. Removal leaves a hole (keep a free list) or
  renumbers everything after it (fix every index that pointed there).
- Objects of different owners link to each other, like a thread that waits on several kernel objects.
  Use a pointer there, or an index into one table that they all share.

## 3. One allocation per owner

**Do:** the arrays of an object come from one allocation that offsets divide, and one `free` releases them. When
the sizes are constant, a struct that holds the arrays is the simplest block. When the sizes are available only at
run time, put each array at a multiple of the size of its type. A size is always a multiple of the alignment, and
C99 has no `alignof`.

**Why:** one block keeps an owner's data together and leaves less to fragment. It also turns a
cleanup path with one `free` per array into a single `free`.

<!-- snippet: block-wrong -->
```c
typedef struct {
    Voice* voice;
    Mix* mix;
} Mixer;
int mixer_init(Mixer* m, size_t voices, size_t mixes) {
    m->voice = (Voice*)calloc(voices, sizeof *m->voice);
    m->mix = (Mix*)calloc(mixes, sizeof *m->mix);
    return m->voice && m->mix; /* a failure leaks the one that worked */
}
void mixer_free(Mixer* m) {
    free(m->voice);
    free(m->mix);
}
```

<!-- snippet: block-right -->
```c
typedef struct {
    Voice* voice; /* the block's start: free(voice) frees both */
    Mix* mix;     /* after the voices, at a multiple of sizeof(Mix) */
} Mixer;
int mixer_init(Mixer* m, size_t voices, size_t mixes) {
    size_t mix_at, total;
    char* block;
    if (voices > (SIZE_MAX - sizeof(Mix)) / sizeof(Voice) || mixes > SIZE_MAX / sizeof(Mix)) return 0;
    mix_at = (voices * sizeof(Voice) + sizeof(Mix) - 1) / sizeof(Mix) * sizeof(Mix);
    if (mixes * sizeof(Mix) > SIZE_MAX - mix_at) return 0;
    total = mix_at + mixes * sizeof(Mix);
    block = (char*)calloc(1, total ? total : 1);
    if (!block) return 0;
    m->voice = (Voice*)block;
    m->mix = (Mix*)(block + mix_at);
    return 1;
}
void mixer_free(Mixer* m) { free(m->voice); }
```

`calloc(n, size)` checked each product for overflow. Products and sums of your own do not, hence the checks
against `SIZE_MAX`, which matter when a size comes from the guest.

**Does not apply when:**
- One of the arrays grows on its own. A `realloc` moves the whole block, so give that array a block of
  its own.
- The parts live for different lengths of time.
- A part is large and rarely used: allocate it at its first use.
- What you build starts to look like a general allocator (free lists across owners, size classes). The RFC
  asks for chunks of one allocation, not for a new malloc.

## 4. Atomics on cache lines of their own

**Do:** when different threads write two atomics, give each its own cache line. Put a `char` array between them.
This also applies to a lock and the data next to it. Atomics are plain integers that the code reaches through the
`__atomic_*` builtins, as `src/` does, not C11's `_Atomic`. Two definitions next to each other at file scope also put
them together. Keep them in one padded struct instead.

**Why:** a core writes a whole 64-byte line. Two threads that write two variables in the same line take that line
from each other on every write. This is as slow as if they wrote one shared variable. This is false sharing.

<!-- snippet: sharing-wrong -->
```c
typedef struct {
    u32 submitted; /* the CPU thread's, through __atomic_* */
    u32 completed; /* the GPU thread's */
} Queue;
```

<!-- snippet: sharing-right -->
```c
#define SNX_CACHE_LINE 64 /* x86-64 */
typedef struct {
    u32 submitted; /* the CPU thread's, through __atomic_* */
    char apart[SNX_CACHE_LINE - sizeof(u32)];
    u32 completed; /* the GPU thread's */
} Queue;
/* C99 has no static assert: an array of negative size if they would share a line */
typedef char snx_queue_apart[offsetof(Queue, completed) - offsetof(Queue, submitted) >= SNX_CACHE_LINE ? 1 : -1];
```

**Does not apply when:**
- One thread writes both, or they are flags that threads read often and write rarely. Then the padding buys
  nothing.
- There are many of them. Each padded atomic costs a full line, and in an array of per-object atomics
  that adds up.

The padding keeps the two apart from each other, not from an allocation next to them: `malloc` aligns to 16, not
64. When that matters, pad before the first one too.

## 5. No stored pointers in data

**Do:** refer to the parts of a data structure by index or by offset from its start, not by address. A data
structure is a table, a tree, or anything that the code could save or share. Native byte order and widths are fine.
The point is that the block means the same at each place that the code copies it to.

**Why:** an address is valid only in this process and at this moment. You can copy data with offsets, write it out,
read it back or map it elsewhere, and it needs no fix-up.

<!-- snippet: serial-wrong -->
```c
typedef struct Node {
    const char* name;    /* a string somewhere on the heap */
    struct Node* parent; /* an address: wrong in a copy, meaningless once written out */
} Node;
```

<!-- snippet: serial-right -->
```c
typedef struct {
    u32 name;   /* offset of its NUL-terminated name in the names that follow the nodes */
    u32 parent; /* index in the nodes, 0 at the top: nodes[0] is none */
} Node;
typedef struct {
    u32 count; /* then Node[count], then the names: one block, copied or written out as is */
} NodeTable;
const Node* table_node(const NodeTable* t, u32 i) { return (const Node*)(t + 1) + i; }
const char* table_name(const NodeTable* t, u32 i) {
    return (const char*)((const Node*)(t + 1) + t->count) + table_node(t, i)->name;
}
```

**Does not apply when:**
- The member is a handle to something outside the data: `FILE*`, `SDL_Window*`, a Vulkan object, a
  function pointer. Keep those in a runtime struct beside the data, not inside it.
- The code never copies, writes out or shares the structure, and a pointer is plainly simpler. The words of
  the RFC are "unless absolutely necessary", so say why in a comment.
- The access is hot enough that an add per access shows up in a measurement. Measure first.

## 6. One global per file

**Do:** keep the mutable state of a file in one struct, and define one global of it, `g_<file>`. The global is
static unless another file needs it. Function-local `static`s are globals too and go in it. Declare what another file
reads one time, in the header next to the file. That is the type of the struct with the global, or a function that
answers it. Atomics that different threads write still get a cache line each (rule 4), with padding inside the
struct.

In `src/nx/hle/`, the struct is a member of `g_hle` (`hle/hle.h`), not a global of the file. Its type and its
initializer macro (`HLE_IPC_INIT`) are in the header beside the file. `hle.c` defines `g_hle` with them, and the code
reads `g_hle.ipc.lk`.

The objects that the file makes and frees are pools in that struct, not `malloc`s. A pool is a static array with a
bound. The code takes an object with `POOL_TAKE` under the lock that guards the pool. It makes the object non-zero
before it releases the lock. It frees the object with `POOL_PUT`.

**Why:** loose globals scatter the state of one subsystem over a file, each with its own name, initialization and
lock. Thus nothing shows what belongs together or what a lock guards. One struct is one place to read, reset in a
test, dump in a crash report or move into a context later.

<!-- snippet: globals-wrong -->
```c
static Session g_sessions[SESSION_MAX];
static u32 g_session_next;
static PMutex g_pool_lk = PMUTEX_INIT;
u32 g_ring_n; /* read by debug.c through an extern of its own */
static int warn_once(u32 cmd) {
    static u32 seen[512]; /* a global no one sees */
    ...
}
```

<!-- snippet: globals-right -->
```c
static struct {
    PMutex lk; /* guards sessions and next */
    Session sessions[SESSION_MAX];
    u32 next;
    u32 seen[512]; /* warn_once's */
    char apart[SNX_CACHE_LINE];
    u32 ring_n; /* every thread's, through __atomic_* */
} g_ipc = {PMUTEX_INIT};
u32 ipc_ring_count(void) { return __atomic_load_n(&g_ipc.ring_n, __ATOMIC_RELAXED); } /* debug.c's */
```

**Does not apply when:**
- The object is `const`: a table is data, not state.
- The variable is `_Thread_local`: each thread has its own, so it cannot live in the shared struct. It goes in the
  file's one thread-local struct, `t_<file>`, instead (`t_hle` and `t_nvn` for those subsystems).
- A third-party file (`.cache/cpm`), which nobody edits.
