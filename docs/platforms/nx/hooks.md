# Hooks

## Index

* [How a call reaches a hook](#how-a-call-reaches-a-hook)
* [Declaring a hook](#declaring-a-hook)
  * [Function types](#function-types)
* [Versions](#versions)
* [Struct layouts](#struct-layouts)
* [Game patches: hooks by address](#game-patches-hooks-by-address)
* [Verify mode](#verify-mode)
* [Checking a hook](#checking-a-hook)
* [What the guest profile showed](#what-the-guest-profile-showed)

A hook runs a function the game exports with a native handler of the runtime's instead of its recompiled code. The
handlers are accelerators: each does what its function is known to do from reading the function's code, on the
guest's own data as the function would leave it. They never copy or reference the SDK. A hook is installed when the
game starts, so changing a handler needs no recompile. A hook of a new symbol needs one for the direct calls to its
function (below).

## How a call reaches a hook

Every call to a hooked function goes through its slot in the dispatch table, `g_disp` (`snx_lookup` in `cpu.h`):

- a call into another of the game's modules, through the PLT stub and the GOT the game's own dynamic linker fills
  (the main executable's `memcpy` is the SDK module's). The recompiler binds a stub to the function its symbol names
  when one module alone of the game defines it ([Recompiler](../../dev/architecture.md#recompiler--module-abi)), and a
  stub bound to a function whose symbol a hook names keeps calling through the slot;
- a call through a function pointer or a virtual call (`BLR`), NVN's procedure table among them;
- a direct call (`BL`, or a tail call's `B`) within a module to a function whose symbol a hook names: the recompiler
  compiles it as a call through the slot (`snx_codegen_routed`, `ir_codegen.c`'s `call`), so LLVM never inlines that
  function into its callers either;
- a call from the runtime JIT's code, which calls an AOT function directly only while its slot still holds it
  (`jit_service.c`'s `direct`).

The recompiler routes only the symbols of the route list (`src/nx/loader/hook_routes.c`): each symbol that
`hooks.hook` and `gpu/nvn/nvn.hook` name in any version, and the `route` lines of the functions the runtime wraps with
code of its own (`nvnBootstrapLoader` and `nvnDeviceGetProcAddress`, `nvn_loader.c`). `hookgen --routes` generates
the list. A direct call to any other function stays a direct call. `SNX_ABI` 15 and 16 first routed every function
that a module exports, but some games export nearly all of their main executable: 88% to 98% of its functions in
three games, so that 51% to 64% of all their direct calls became indirect calls (`tools/hook_census.py`). On one of
them, in a gameplay scene, routing only the list's symbols took 9% of the cycles and 3.6% of the instructions that the
main guest thread spends in the recompiled code each frame, and a quarter of the process's mispredicted indirect
branches. The module became 7% smaller. Small accessors that went through their slots are inlined into their callers
again. The frame time did not change there, because the GPU thread set it.

Each module carries the list it was recompiled with (`snx_routes()`, `SnxRoutes` in `module_abi.h`): the hash of the
list and the hash of each symbol. `hook_install` logs the module's list. When it installs a hook whose symbol the
module's list doesn't name, it warns that the hook sees only the calls through function pointers and the PLT stubs
the module did not bind, and that a recompile gives it the direct calls too. The NVN loader (`nvn_loader.c`) does the
same for `nvnBootstrapLoader`. A module made before the list has no `snx_routes`, and routes every function it
exports, so it works with this runtime unchanged.
The list doesn't change `SNX_ABI`: a hook's handler keeps the guest's data as its function does, so a direct call
that runs the function's own code is still correct, only not accelerated.

`hook_install` (`src/nx/hle/hook/hook.c`) reads the game's exported functions (`kernel_load_symbols`) and puts the
hook's adapter into each matching function's slot, before the game's first thread and the JIT start. The slots never
change while the game runs, except for `hook_restore` putting a hook back after the JIT drops the code of a range
(`jit_code_invalidate`). It holds `HOOK_SITES` hooked functions (`hook.h`), as many as a table has entries at most,
and `hook.c` does not build with a table larger than that, so a game that exports each function once gets every hook.
One whose modules export them more than once can run out: the game then stops, naming the function left without its
hook, since that function would run the SDK's own code beside the native hooks of its family, which keep the family's
state, and a service that only the native functions answer would be asked (`tests/hook_sites.c`).

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
  each of the size its name says in the guest (AArch64, LP64), `usize` and `isize` (the guest's `size_t` and
  `ptrdiff_t`), and pointers. Integers take x0 to x7 and floats v0 to v7 under AAPCS64, counted apart. An argument
  that would go on the stack is refused.
- `hookgen --a32` (`snx_hookgen(... A32)`) also generates an adapter for a 32-bit program, a block of its code table
  (`SnxArmBlock`). It reads the arguments where AAPCS32 with VFP arguments puts them: an integer, a pointer, a `usize`
  or an `isize` in a word, a `u64` or an `i64` in an even pair of r0 to r3, floats in s0 to s15 and doubles in d0 to
  d7, and the rest on the stack. It calls the same handler with the thread's `CPU`, and returns to lr in the state lr's
  bit 0 gives. A pointer and a `usize` reach the handler zero-extended, an `isize` sign-extended. The NVN spec uses it:
  `tests/hook_adapters.c` checks the mapping.
- The word `a32` after a hook's handler (after `pure`) says the declaration holds for a 32-bit game too: its
  arguments are the function's under AAPCS32 as written (a `usize` for a `size_t`, never a `u64`), and its handler
  reads nothing whose layout differs in the 32-bit SDK. A hook that `uses` a struct can't be one: a struct's layout
  is one SDK's. A `@version` after that `a32` gives the versions the declaration holds for in a 32-bit game, in place
  of the hook's own: `hook SYMBOL() -> u32 = handler a32 @version(< 1.0.0)`. The 32-bit SDKs a function was read in
  are not the 64-bit ones, and the runtime's own specs name them on every declaration for a 32-bit game
  (`tests/nn_*.c` check each family's). A 32-bit game of another SDK then has no hook, and stops when its SDK opens
  a service the runtime answers only natively ("Versions", below).
- A line that begins with `a32` declares a hook for a 32-bit game alone: `a32 hook SYMBOL(...) ...`, with a
  `@version` before it and `uses` after its handler as any hook. It is for a function whose declaration differs
  there. A 32-bit SDK mangles a `size_t` as `j` where the 64-bit one has `m` (and an `int64_t` as `x` for `l`), so a
  function with one in its signature has another symbol. AAPCS32 returns a class of more than 4 bytes through a
  pointer the caller passes in r0, where AAPCS64 returns one of 8 bytes in x0, so `nn::time`'s `now()` is
  `() -> i64` for a 64-bit game and `a32 hook ...(addr out)` for a 32-bit one, under one symbol. And a struct there
  has its own layout, declared as a struct of another name. A symbol's declarations for a 32-bit game (those marked
  `a32` and those for it alone) are checked against each other, never against the ones that don't hold for it.
- `hookgen --a32-marked` (`snx_hookgen(... A32_MARKED)`) writes a 32-bit game's table after the other,
  `<prefix>32_table` with `<prefix>32_symbol`, `_fn` (the adapters, `SnxArmBlock`s), `_handler`, `_family`, `_key`,
  `_lo`, `_hi` and `_layouts`: the hooks marked `a32` and the ones for a 32-bit game alone, which are in no other
  table and name no route (a 32-bit game's every call goes through its code table). The runtime's own spec is
  generated this way, and for a 32-bit game `hook_install` puts each adapter of that table in its function's slot
  of the code table (`arm_slot`), a versioned one by the SDK label of the module that exports the function, as for
  a 64-bit game; every other SDK function runs its own code there.
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
types of the callbacks and the resolvers, which no hook declares, and `tools/nvn_hash.py` reads the prototypes from
`nvn.hook` (`tools/hook_spec.py`). Each type has the guest's size, whatever the host:

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
  implements (`gpu/nvn/nvn.hook`). It is the game's own driver's: at the game's first
  `nvnBootstrapLoader` call, before it has any NVN function, the driver's lookup finds `nvnDeviceGetInteger` by name
  and its switch's answers for the API's major and minor version are read from its code (`gpu/nvn/nvn_driver.c`).
  Each NVN function the game then resolves takes the declaration that holds for that version, and one declared only
  for other versions stops the game with its name and the version when it is called
  (`tests/nvn_versions.c`). The log says whether the version is one `nvn_api_described` holds
  (`gpu/nvn/nvn_driver.c`: 53.5, 53.13, 53.105, 53.209, 53.311 to 53.314 and 55.8 to 55.15). The runtime takes the
  game's NVN calls for every version.
- A hook is matched per module, against the version of the module that exports its function. An unknown version
  (0) matches only unversioned hooks: a hook that reads the guest's structs never guesses their layout.
- One symbol may be declared several times, each with its own handler, for versions of one key that don't overlap.
  An unversioned declaration is its symbol's only one. hookgen refuses overlaps and mixtures with both lines.
- The table holds each entry's range, sorted by symbol and then by version, and `hook_select` picks the entry whose
  range holds the module's version. When a symbol has entries but none holds, the hook isn't installed and the log
  names the symbol and the version once. A versioned hook says the range it was installed for, once
  (`hook: lock at ... runs os_lock, the hook for sdk >= 12.0.0, with the layouts nn::os::MutexType of line 12`).
- A function left to its own code this way works on the SDK's own state, and reaches the service behind it. Where
  the runtime answers that service only natively, `sm` stops the game when it opens it, and the stop ends with what
  was left: how many symbols have declarations of which none holds for their module's version, and the first one's
  name, module, version and declared versions (`hook_missed_text`: `41 of the SDK functions the runtime has a hook
  for run their own code in this 32-bit game, ...: the first is ... in sdk, of version 3.0.1, declared for sdk <
  1.0.0`).

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

## Game patches: hooks by address

A function that no symbol names, in one build of one game, is hooked by its module's build ID and its offset in that
module. Each game's hooks are a spec of their own in `src/nx/patches/`, `<game>.hook`, with their handlers beside it
in `<game>.c`. No game has one yet, so the folder is not in the tree. `src/nx/CMakeLists.txt` still collects its `.c`
files, and `cmake/hookgen.cmake` its `.hook` files, for the first one:

```
family game_frame_rate
[0123456789abcdef0123456789abcdef01234567:0x1a2b40] hook game_mode_set(u32 mode)
```

- The build ID is the module's, as an IPS patch is named and an IPSwitch patch's `@nsobid-` gives it: the hex of its
  first 8 to 32 bytes, the rest of the ID zeros. The offset is the function's entry from the module's base, as a
  symbol's value would be. A hook of an offset that isn't 4-aligned is refused.
- The name after `hook` is the handler's, as the function has no other, so `= HANDLER` is refused.
- The hook holds for the one build its ID names, so it has no `@version`. A game's other version has another ID: its
  functions move, and it gets no hook until a line of its own names it.
- Its key, `BUILDID:OFFSET` in lower case without the ID's ending zero bytes, `0x` or leading zeros
  (`0123456789abcdef0123456789abcdef01234567:1a2b40`), stands for its symbol: in the table, in the log
  (`hook: 0123...:1a2b40 at 81a7b40 (main+1a2b40) runs game_mode_set`), in `hooks_off` by its family, and in the
  route list. The loader makes each key of a module's own build a function seed and a routed function
  (`route_addresses`, `loader.c`), so the recompiler compiles the function at that entry and calls it through its slot
  from every direct call. Under NCE the slot's stub at the function's first word takes the calls from native code, as
  it does for any hook (`nce_slot_taken`).
- A hook goes on a function's entry, never inside one: a call reaches only an entry's slot.

hookgen reads `hooks.hook` and then each patch into one table, each spec with its own family lines (`cmake/hookgen.cmake`'s
`snx_hook_patches`), so a patch file is added by creating it. Adding one changes the route list, and only a game whose
build a key names compiles anything differently.

A game patch's handler changes what the game does, as an IPS patch would, where `hooks.hook`'s handlers only do what
their functions do. It is written from reading the function's code, works on the guest's data through `hook_host`,
and keeps the game's own code doing the rest: `hook_run_original(c, addr)` runs the function's own code with the CPU as
the call left it, through the dispatch loop until the function returns to the call's return address, so a handler can
change an argument before it and a result after it. `hook_site(key)` gives the address the hook went to, from which a
handler finds its module's base and data.

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

The functions hooked are the running process's (`g_host.hook`): their own code and the adapters are addresses of this
run, so a save state holds none of them, and a state loaded from another run finds this run's hooks in place. The
scratch slots are guest memory, so a state holds them, with their base (`g_hle.hook`), only when the run that saved
it verified: after the load of one that has none, each call is skipped and the log says why (`tests/hook_install.c`).

A function with other side effects (it waits, takes a lock, does I/O or reads the clock) can't run twice, so its hook
isn't `pure` and verify mode skips it: such a hook is verified in tests, on inputs recorded from the game.
`tests/hook_verify.c` checks verify mode itself with stand-ins for the function's own code.

## Checking a hook

The `hooks` setting (`[Debug]`, on by default) turns every hook off, to tell whether a problem comes from one, and
`hooks_off` turns off the families it names (`hooks.hook`'s `family` lines, such as `libc`), to tell which. The
log names each module's SDK version and each hooked function at startup (`hook: memcpy at ... runs hook_memmove`). A handler that wants to hand a
case back to the function's own code calls `hook_original(addr)` with the CPU as the call left it. A hook is on by
default only once it passes verify mode (or, for one that isn't pure, its recorded-input test).

The `diag` family's hook only logs: each of the SDK's NN_ABORT overloads ends in `nn::diag::detail::VAbortImpl`, and its
handler (`diag.c`) writes the condition, the SDK function, its file and line, the Result (and the error code it makes)
and the message's format as the SDK passed them, then runs the function's own code through `hook_original`, which stops
the game as it would (`tests/diag_abort.c`).

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
