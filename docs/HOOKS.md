# Hooks

A hook runs a function the game exports with a native handler of the runtime's instead of its recompiled code. The
handlers are accelerators: each does what its function is known to do from reading the function's code, on the
guest's own data as the function would leave it. They never copy or reference the SDK. A hook is installed when the
game starts, so changing a handler needs no recompile. A hook of a new symbol needs one for the direct calls to its
function (below).

## How a call reaches a hook

Every call to a hooked function goes through its slot in the dispatch table, `g_disp` (`snx_lookup` in `cpu.h`):

- a call into another of the game's modules, through the PLT stub and the GOT the game's own dynamic linker fills
  (the main executable's `memcpy` is the SDK module's);
- a call through a function pointer or a virtual call (`BLR`), NVN's procedure table among them;
- a direct call (`BL`, or a tail call's `B`) within a module to a function whose symbol a hook names: the recompiler
  compiles it as a call through the slot (`snx_codegen_routed`, `ir_codegen.c`'s `call`), so LLVM never inlines that
  function into its callers either;
- a call from the runtime JIT's code, which calls an AOT function directly only while its slot still holds it
  (`jit_service.c`'s `direct`).

The recompiler routes only the symbols of the route list (`src/nx/loader/hook_routes.c`): each symbol that
`hooks.hook` and `gpu/nvn/nvn.hook` name in any version, and the `route` lines of the functions the runtime wraps with
code of its own (`nvnBootstrapLoader` and `nvnDeviceGetProcAddress`, `nvn_direct.c`). `hookgen --routes` generates
the list. A direct call to any other function stays a direct call. `SNX_ABI` 15 and 16 first routed every function
that a module exports, but some games export nearly all of their main executable: 88% to 98% of its functions in
three games, so that 51% to 64% of all their direct calls became indirect calls (`tools/hook_census.py`). On one of
them, in a gameplay scene, routing only the list's symbols took 9% of the cycles and 3.6% of the instructions that the
main guest thread spends in the recompiled code each frame, and a quarter of the process's mispredicted indirect
branches. The module became 7% smaller. Small accessors that went through their slots are inlined into their callers
again. The frame time did not change there, because the GPU thread set it.

Each module carries the list it was recompiled with (`snx_routes()`, `SnxRoutes` in `module_abi.h`): the hash of the
list and the hash of each symbol. `hook_install` logs the module's list. When it installs a hook whose symbol the
module's list doesn't name, it warns that the hook sees only the calls through the PLT and function pointers, and
that a recompile gives it the direct calls too. `nvn_direct` does the same for `nvnBootstrapLoader`. A module made
before the list has no `snx_routes`, and routes every function it exports, so it works with this runtime unchanged.
The list doesn't change `SNX_ABI`: a hook's handler keeps the guest's data as its function does, so a direct call
that runs the function's own code is still correct, only not accelerated.

`hook_install` (`src/nx/hle/hook/hook.c`) reads the game's exported functions (`kernel_load_symbols`) and puts the
hook's adapter into each matching function's slot, before the game's first thread and the JIT start. The slots never
change while the game runs, except for `hook_restore` putting a hook back after the JIT drops the code of a range
(`jit_code_invalidate`).

What no hook sees is code the game's own compiler inlined from an SDK header: there is no call to take. A handler
therefore keeps the guest's data as the function does, so inlined copies read what it left.

## Declaring a hook

Hooks are declared in `src/nx/hle/hook/hooks.hook`, one per line, each under the `family` line before it:

```
family libc
hook memcpy(void* dst, const void* src, u64 size) -> void* = hook_memmove
```

- The symbol is the function's name as the game's dynamic symbol table spells it. A C++ function goes by its mangled
  name (`_ZN2nn2os...`). Names are never parsed, demangled or mangled.
- The arguments and result are the function's as C sees them. A C++ method takes `this` as its first argument. The
  types are `void` (a result only), `bool`, `u8` to `u64`, `i8` to `i64`, `addr` (a guest address) and `f32`/`f64`,
  each of the size its name says in the guest (AArch64, LP64), and pointers. Integers take x0 to x7 and floats v0 to
  v7 under AAPCS64, counted apart. An argument that would go on the stack is refused.
- A pointer is a type followed by `*`, each `*` optionally followed by `const`, after an optional `const`:
  `const NVNtexture* const* textures`. It points to `void`, `char`, one of the types above, or a type of the C header
  that the spec's `include` line names (`include "nx/gpu/nvn/nvn.h"`, found in the folders that `-I` gives hookgen).
  hookgen reads the names that the header's typedefs declare, and refuses a name it doesn't declare. A type of the
  header is passed by value only when it is an enum (an `int` under AAPCS64, so an `i32`) or a pointer
  (`typedef void* NVNnativeWindow`, or a function pointer type, so an `addr`). The language has no arrays: an array
  argument is a pointer to its first element. A `const` without a `*` after it is refused, as it says nothing the
  caller sees.
- A pointer is a guest address. The adapter reads it from its x register as a `u64`, and the handler takes it as a
  `u64`, as an `addr`: the type only says what it points to, for the function types below and for the tools that
  read the spec (`tools/hook_spec.py`). No handler can take it for a host pointer.
- The handler is `ret handler(CPU* c, args...)`, declared by the generated `hle/hook/hooks.hook.h`. It reads and
  writes guest memory at `c->mem + addr`, and calls the HLE kernel for anything that waits (a guest thread holds its
  emulated core until a kernel wait releases it, so a handler never blocks on a host primitive).
- `= HANDLER` names the handler. Without it, the handler is the symbol with a `_` before its first capital:
  `hook nvnDeviceFinalize(NVNdevice* d)` runs `nvn_DeviceFinalize`. The symbol must have a part in lower case before
  that capital (`memcpy` and `_ZN...` don't, so they name their handlers). An `= HANDLER` that names the handler the
  symbol gives already is refused, so that each name is written once.

### Function types

With an `include` line, the generated header also includes that header, and gives each hooked function its type as
the guest calls it, `PFN<SYMBOL in capitals>PROC`:

```
hook nvnTextureWriteTexelsStrided(const NVNtexture* t, const NVNtextureView* v, const NVNcopyRegion* region, const void* src, i64 row, i64 image)
typedef void (*PFNNVNTEXTUREWRITETEXELSSTRIDEDPROC)(const NVNtexture* t, const NVNtextureView* v, const NVNcopyRegion* region, const void* src, int64_t row, int64_t image);
```

`gpu/nvn/nvn.hook` is thus the one prototype of each NVN function the runtime implements: `nvn.h` keeps only the
types of the callbacks and the resolvers, which no hook declares, and `tools/nvn_verify_kinds.py` and
`tools/nvn_hash.py` read the prototypes from `nvn.hook` (`tools/hook_spec.py`). Each type has the guest's size,
whatever the host:

- An integer is the `<stdint.h>` type of its size (`int32_t`, `int64_t`), and `bool` a `uint8_t` (NVN's
  `NVNboolean`), never a host type that changes with the host: `long` is 32 bits on Windows, and a `size_t` or a
  `ptrdiff_t` is as wide as the host's pointers. A `ptrdiff_t` of the guest is an `i64` in the spec, and a `size_t` a
  `u64`.
- A pointer is the host pointer to the type it points to, which the reader needs. The guest's pointers are 8 bytes,
  and so are the host's on every host ScarletNX builds for: the generated header asserts it (`nvn_pointer_size`).
- An enum stays an enum. AAPCS64 makes it an `int` when its values fit in one, and each of NVN's has a
  `0x7FFFFFFF` member to make it 32 bits.

Nothing calls through these types: a guest function runs at a guest address, through its dispatch slot. All the
declarations of one symbol take the same types, as it has one function type. `tests/nvn_header.c` checks some of
them against prototypes in NVN's own types, with `-pedantic-errors`.

`tools/hookgen` generates the adapters (a `GuestFn` that unpacks the registers, calls the handler, puts the result in
x0 or v0 and returns to x30) and the table sorted by symbol. `tests/hook_adapters.c` checks the register mapping,
and the `hookgen_refuses_*` tests the declarations it refuses.

## Versions

A function's code, and the layout of the structs it works on, change between versions of the SDK that built the
module exporting it. A hook can say which versions it holds for:

```
@version(>= 5.0.0, < 12.0.0)
hook lock(addr m) -> u32 = os_lock_old uses nn::os::MutexType
@version(>= 12.0.0) hook lock(addr m) -> u32 = os_lock uses nn::os::MutexType
```

- `@version(...)` goes on the line before the hook, or at the start of its line. It holds comparisons (`>=`, `>`,
  `<=`, `<`, `==`) joined by commas, all of which must hold, and a key before them: `sdk` when none is given.
- `sdk` is the module's SDK version. An SDK-built module carries middleware labels in its read-only data, of the
  form `SDK MW+<vendor>+<vendor>SDK_nnSdk-<x>_<y>_<z>` (and `..._NVN-...` for the NVN library). `hook_install`
  reads each module's when the game starts (`hook_label_version`, packed `x << 16 | y << 8 | z`, each part at most
  255) and logs it once per module (`hooks: module sdk, SDK 20.5.22`). In the games looked at so far only the `sdk`
  module carries one: `main`, `rtld` and the `subsdk` modules have none, so their version is unknown.
- `nvn` is the NVN API version (`major.minor`, packed `major << 16 | minor`), for the NVN functions the runtime
  implements under `nvn_direct` (`gpu/nvn/nvn.hook`). It is the game's own driver's: at the game's first
  `nvnBootstrapLoader` call, before it has any NVN function, the driver's lookup finds `nvnDeviceGetInteger` by name
  and its switch's answers for the API's major and minor version are read from its code (`gpu/nvn/nvn_driver.c`).
  Each NVN function the game then resolves takes the declaration that holds for that version, and one declared only
  for other versions stops the game with its name and the version when it is called
  (`tests/nvn_versions.c`). `nvn_direct=2` (auto) takes the game's NVN calls only for the versions
  `nvn_api_described` holds (`gpu/nvn/nvn_driver.c`: 53.209 and 55.8 to 55.15), and leaves the others to the driver.
- A hook is matched per module, against the version of the module that exports its function. An unknown version
  (0) matches only unversioned hooks: a hook that reads the guest's structs never guesses their layout.
- One symbol may be declared several times, each with its own handler, for versions of one key that don't overlap.
  An unversioned declaration is its symbol's only one. hookgen refuses overlaps and mixtures with both lines.
- The table holds each entry's range, sorted by symbol and then by version, and `hook_select` picks the entry whose
  range holds the module's version. When a symbol has entries but none holds, the hook isn't installed and the log
  names the symbol and the version once. A versioned hook says the range it was installed for, once
  (`hook: lock at ... runs os_lock, the hook for sdk >= 12.0.0, with the layouts nn::os::MutexType of line 12`).

## Struct layouts

The structs a handler reads in guest memory are declared beside the hooks, each version's layout apart:

```
struct nn::os::MutexType @version(< 9.0.0) {
    u8 state @0x0
    u32 owner @0x4
} size 0x10
struct nn::os::MutexType @version(>= 9.0.0) { u8 state @0x0; u32 owner @0x8 } size 0x20
```

- A field is a type (those of the arguments, but `void`; `bool` is a byte and `addr` 8 bytes), a name and its
  offset. Fields are separated by line ends or semicolons, and the struct's size follows its closing brace. A field
  that runs past the size is refused.
- Every declaration of one struct has the same fields, of the same types, at offsets of their own, for versions
  that don't overlap. A struct without `@version` holds for every version and is its name's only declaration.
- hookgen generates a layout type per struct, `HookLayout_nn_os_MutexType` (`::` becomes `_`): its `size`, and each
  field as a `HookField_<type>` holding its offset. The variants are a table of these, by version.
- A hook whose handler reads a struct says so after the handler, `= handler uses nn::os::MutexType` (several are
  separated by commas). The handler takes a pointer to the layout after the CPU,
  `u32 os_lock(CPU* c, const HookLayout_nn_os_MutexType* nn_os_MutexType, u64 m)`, the variant for the module the
  hook was installed in.
- hookgen splits a hook's versions at the bounds of the variants it uses, into one entry each, so an entry's adapter
  passes its variant as a constant and the handler costs no lookup. The versions no variant covers have no entry:
  the hook isn't installed for a module of such a version. A hook none of whose versions a variant covers is
  refused.
- A handler reads and writes the struct's fields through the layout with `hook_get_<type>(c, L->field, addr)` and
  `hook_put_<type>(c, L->field, addr, value)` (`hook.h`), little-endian whatever the host's order. The field types
  differ per type, so `hook_get_u32` doesn't take a `u8` field. An atomic access takes
  `c->mem + addr + L->field.offset` itself.

`tests/hook_versions.c` checks the entries hookgen makes of `tests/hook/versions.hook`, the labels, the entry and
layout picked for a module's version (none, below and between ranges, the bounds of each comparison), and the
accessors.

## Verify mode

A handler has to do what its function does, and verify mode checks that it does, call by call, against the
function's own recompiled code. The two can't both run on the game's live memory, since a side effect would happen
twice, so a hook says what its function touches:

```
hook memcpy(void* dst out[size], const void* src in[size], u64 size) -> void* = hook_memmove pure
```

- `pure` (after the handler and its `uses`) says the function depends on nothing but its arguments, the memory they
  point to and constant data, and changes nothing but its result and the memory its buffers name.
- A buffer is an `addr` or pointer argument followed by its use, `in[...]` (read), `out[...]` (written) or `inout[...]`, and its
  length in bytes: a number, or an integer argument's name times an optional number, plus an optional number
  (`in[count * 4 + 8]`). A hook has at most four, and only a pure hook has any.

The `hooks_verify` setting (`[Debug]`) names the families whose pure hooks are checked. Their slots then hold
verify mode's check (`verify_fn` in `hook.c`) in place of the adapter. For each call, `hook_verify_call`
(`src/nx/hle/hook/verify.c`) copies the buffers into scratch memory in the kernel region (buffers that overlap or
touch share one copy, so a memmove's overlap stays one, and each copy keeps its buffer's address modulo 64, so the
function takes the path it takes for that alignment), runs the function's own code with the buffer arguments pointing
at the copies (and through the dispatch loop when the code leaves the function early), puts the registers back, runs
the handler on the live memory and compares:

- the result: as many bits of x0 as its type has (a narrow result's upper bits are no caller's), whether a bool is 0,
  lane 0 of v0 for a float, and an address with a pointer into a copy taken as the same pointer into the live buffer;
- the callee-saved registers, x19 to x29, sp, and the low halves of v8 to v15;
- every byte of every buffer.

The log names each call that differs (the first 16 of each function), with its caller and arguments, and counts the
calls that agree (the first, then at 1024 and each power of 4 after it). A call whose buffers don't fit in its scratch
slot (4 MiB, 16 slots taken without waiting, since a guest thread can lose its core inside the function's code) runs
the handler alone and is counted as skipped. The cumulative FP exception flags (FPSR) aren't compared: no handler
sets them. Verify mode is slow (each call runs twice and copies its buffers), for checking a hook, never for playing.

A function with other side effects (it waits, takes a lock, does I/O or reads the clock) can't run twice, so its hook
isn't `pure` and verify mode skips it: such a hook is verified in tests, on inputs recorded from the game.
`tests/hook_verify.c` checks verify mode itself with stand-ins for the function's own code.

## Checking a hook

The `hooks` setting (`[Debug]`, on by default) turns every hook off, to tell whether a problem comes from one, and
`hooks_off` turns off the families it names (`hooks.hook`'s `family` lines, such as `libc`), to tell which. The
log names each module's SDK version and each hooked function at startup (`hook: memcpy at ... runs hook_memmove`). A handler that wants to hand a
case back to the function's own code calls `hook_original(addr)` with the CPU as the call left it. A hook is on by
default only once it passes verify mode (or, for one that isn't pure, its recorded-input test).

## What the guest profile showed

An accelerator is added only when its function shows up in a guest profile of CPU time by SDK function. The first
profile (perf `cycles:u` on two games built with SDK 12.3.9 and 20.5.22, during loading, a title screen and
gameplay, each recompiled function's samples named by the game's symbols) gave these shares of the time spent in
guest code:

| Family | Share of guest time |
| --- | --- |
| nn::os | 3.8% to 6.3% in gameplay, 4.9% loading, 15.6% on one game's title screen |
| math (`sinf`, `cosf`, `powf` and their kernels) | 0.5% to 1.1% |
| the C library's string functions left unhooked (`strcmp`, `strlen`, `memcmp`) | 0.3% to 0.8% |
| allocators (`nn::mem`, `nn::lmem`, the SDK's heap) | 0.02% to 0.6% |
| compression and hashing (zlib, LZ4, zstd, CRC) | under 0.01% |

Most of nn::os's share is the internal critical section under every mutex (`Enter`, `Leave`,
`IsLockedByCurrentThread`) and, on the title screen, a loop polling `GetSystemTick` and `ConvertToTimeSpan`. Neither
is guest code a handler can make cheaper. Half of `Enter`'s and `Leave`'s samples land on the compare-and-swap that
the recompiler makes of their exclusive store, and a native handler, which has to make the same compare-and-swap on
the same word to keep the lock's protocol, spent 78% of its own time there: in gameplay the hooked pair cost the same
per lock taken as the recompiled code, within the runs' noise. A tick-polling loop waits for time to pass, however
fast it polls. The math functions differ between the two SDKs (one has FreeBSD's `__sindf` kernels, the other its
own `sinf`), so each needs a bit-exact port per version for a gain under 0.2% of all CPU time, and the string
functions are word-at-a-time loops the recompiled code already runs well. None of these got a hook.
