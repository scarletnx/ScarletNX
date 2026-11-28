# Hooks

## Index

* [How a call reaches a hook](#how-a-call-reaches-a-hook)
* [Declaring a hook](#declaring-a-hook)
  * [Function types](#function-types)
* [Versions](#versions)
* [Struct layouts](#struct-layouts)
* [Game patches: hooks by address](#game-patches-hooks-by-address)
* [Modules](#modules)
  * [What a module applies to](#what-a-module-applies-to)
  * [Finding a game's passes](#finding-a-games-passes)
  * [Modules in libraries](#modules-in-libraries)
* [A game that rewrites a hooked function](#a-game-that-rewrites-a-hooked-function)
* [A mod's call of the original](#a-mods-call-of-the-original)
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
  compiles it as a call through the slot (`snx_codegen_routed`), so the function is never inlined into its callers
  either;
- a call from the runtime JIT's code, which calls an AOT function directly only while its slot still holds it
  (`jit_service.c`'s `direct`).

The recompiler routes only the symbols of the route list (`src/nx/loader/hook_routes.c`): each symbol that
`hooks.hook`, `gpu/nvn/nvn.hook` and `gpu/gl/gl.hook` name in any version, and the `route` lines of the functions the
runtime wraps with code of its own (`nvnBootstrapLoader` and `nvnDeviceGetProcAddress`, `nvn_loader.c`; a GL driver's
`eglGetProcAddress`, `gl_census.c`; the Vulkan loader's entry points, `gpu/gvk/gvk.hook`). `hookgen --routes`
generates the list. A direct call to any other function stays a direct call. `SNX_ABI` 15 and 16 first routed every
function
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

`hook_install` (`src/nx/nn/hook.c`) reads the game's exported functions (`kernel_load_symbols`) and puts the
hook's adapter into each matching function's slot, before the game's first thread and the JIT start. The slots never
change while the game runs, except for `hook_restore` putting a hook back after the JIT drops the code of a range
(`jit_code_invalidate`), and a hook the game invalidates
([below](#a-game-that-rewrites-a-hooked-function)). It holds `HOOK_SITES` hooked functions (`hook.h`), as many as a table has entries at most,
and `hook.c` does not build with a table larger than that, so a game that exports each function once gets every hook.
One whose modules export them more than once can run out: the game then stops, naming the function left without its
hook, since that function would run the SDK's own code beside the native hooks of its family, which keep the family's
state, and a service that only the native functions answer would be asked (`tests/hook_sites.c`).

What no hook sees is code the game's own compiler inlined from an SDK header: there is no call to take. A handler
therefore keeps the guest's data as the function does, so inlined copies read what it left.

## Declaring a hook

Hooks are declared in `src/nx/nn/hooks.hook`, one per line, each under the `family` line before it:

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
  of the hook's own: `hook SYMBOL() -> u32 = handler a32 @version(< 1.0.0)`, and several give one group each, any of
  which holds, for SDKs read apart: `a32 @version(< 1.0.0) @version(== 4.4.0)`. The 32-bit SDKs a function was read
  in are not the 64-bit ones, and the runtime's own specs name them on every declaration for a 32-bit game
  (`tests/nn_*.c` check each family's). A 32-bit game of another SDK then has no hook, and stops when its SDK opens
  a service the runtime answers only natively ("Versions", below).
- A line that begins with `a32` declares a hook for a 32-bit game alone: `a32 hook SYMBOL(...) ...`, with a
  `@version` before it (or several, one group each) and `uses` after its handler as any hook. It is for a function whose declaration differs
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
- The handler is `ret handler(CPU* c, args...)`, declared by the generated `nn/hooks.hook.h`. It reads and
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

The Vita's system functions are declared in the same language, by NID (`[0x99ba173e] hook sceIoLseek(...)`), and
`hookgen --vita` generates their adapters and table: [The system's functions by NID](../vita/README.md#the-systems-functions-by-nid).

## Versions

A function's code, and the layout of the structs it works on, change between versions of the SDK that built the
module exporting it. A hook can say which versions it holds for:

```
@version(>= 5.0.0, < 12.0.0)
hook lock(addr m) -> u32 = os_lock_old uses nn::os::MutexType
@version(>= 12.0.0) hook lock(addr m) -> u32 = os_lock uses nn::os::MutexType
```

- `@version(...)` goes on the line before the hook, or at the start of its line. It holds comparisons (`>=`, `>`,
  `<=`, `<`, `==`) joined by commas, all of which must hold, and a key before them: `sdk` when none is given. A
  declaration takes one, but one for a 32-bit game, which takes a group for each 32-bit SDK read apart (above).
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
- `movie` is the version of the movie library, from the label its subsdk module carries
  (`SDK MW+<vendor>+<vendor>SDK_movie-x_y_z`), packed as `sdk`'s. That module has no `nnSdk` label, so its
  functions (NVIDIA's multimedia stack, whose JPEG decoder and encoder `nn/tvmr_jpeg.hook` runs natively) are
  versioned by it. `hooks: module subsdk0, SDK unknown, movie 4.4.1` is its line in the log.
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
  1.0.0`). In a 32-bit game whose SDK no declaration for a 32-bit game was read in, that reason takes the place of the
  advice for a homebrew program (`hook_unread_text`): its SDK and the module it is in, the SDKs the declarations were
  read in (`HOOK_A32_READ_SDKS`, 0.12.16 and 4.4.0), how many of its functions run their own code by family, and what
  to do (`This 32-bit game's SDK is 10.6.0 (module sdk) ...`).

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
in `<game>.c`. No game has one yet: the folder holds only the list of [modules](#modules), `modules.c`.
`src/nx/CMakeLists.txt` collects its `.c` files, and `cmake/hookgen.cmake` its `.hook` files, for the first one:

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

## Modules

A module is one game's patch as one unit (`nn/module.h`): what it applies to, what it hooks on the CPU, and its
handlers, in one file of `src/nx/patches/`. `src/nx/patches/modules.c` lists every module built into the runtime, and
nothing else names one. A module can also be a library of its own, built outside ScarletNX
([below](#modules-in-libraries)), which the runtime adds after them. When the game starts, `hook_install` takes each
module of the list (`hook_modules_select`) and logs whether it applies, and why not (`hooks: module NAME doesn't
apply, and leaves the game as it is: check 0 at main+100 differs at byte 1 (21 there)`). A module of another title
isn't logged.

A module that doesn't apply leaves the game as it is: its hooks aren't installed. `hooks=0` turns every module off,
and `hooks_off` names modules as it names families (a module's name is its family's), so the game's `.ini` turns one
off for that game alone.

```c
typedef struct {
    const char* name;     /* its word in hooks_off and in the log, the family of its address hooks */
    const char* what;     /* what it does, for the log */
    u64 title;            /* the title ID it applies to, 0 any */
    const char* module;   /* the game's module its build ID and checks name: NULL for main */
    const char* build_id; /* that module's build ID, the hex of its first bytes, NULL any */
    const HookModuleCode* code;
    u32 ncode;
} HookModule;
```

### What a module applies to

- **The title**, when `title` isn't 0: the game's title ID (`SnxModule.title_id`).
- **The build** of one of the game's modules, when `build_id` is given: the hex of the first bytes of its build ID, as
  an address hook's key names it. Another version of the game is another build, and gets nothing.
- **Code checks** (`HookModuleCode`): bytes at an offset of one of the game's modules, a byte as two hex digits or
  `??` for any (`"fd 7b bf a9 ?? ?? 00 94"`). Each must lie within that module's code and hold. A check is the gate
  for a module that hooks code it read: a build ID can't tell a patched module (a mod's) from the original.

All of them must hold. A compiled-in module's CPU hooks are then the address hooks of its family, as
[Game patches](#game-patches-hooks-by-address) says: `family NAME` in `src/nx/patches/NAME.hook`, the handlers in its
`.c` file. A library's are its CPU hooks ([below](#cpu-hooks-in-libraries)). A module never writes the game's code,
so it doesn't meet a mod's detours ([below](#a-game-that-rewrites-a-hooked-function)). A 32-bit game gets no module
yet: `hook_install` chooses them for a 64-bit game alone.

`hook_module_applied`, `hook_module_refused` and `hook_module_off` (`tests/hook_install.c` with
`tests/hook_module_plain.c`) check a compiled-in module: one named after a family, its hook installed when its code
check holds, and the function left to its own code when the check fails or `hooks_off` names the module.

### Finding a game's passes

A module that replaces a pass of the game's GPU work (its upscaler, say) hooks the game's function that records that
pass. `gpu_hook_trace=N` (`[Debug]`, `gpu/nvn/nvn_trace.c`) finds the pass and that function: it logs the game's first
N draws and dispatches as its queues replay them. Each dispatch's line has its program's hash (FNV-1a 64 of its code),
its code's size and address, its call site in main (the return address of the game's call that recorded it), grid and
block. An Ounce dispatch's line also has its packed c[0]'s buffers (address and size) and each texture and image
handle with the image it names (address, render target format, size). Each draw's line has its fragment program's
hash, targets and viewport. Each line ends with the records since the one before that aren't state binds or barriers
(their op codes), and the log marks each wait, each end of a submission and each present. `gpu_hook_trace_from` lets
that many presents go by first, to trace a scene the game reaches after its start. `gpu_hook_trace_words` adds words
of one constant buffer of each draw (a stage, a slot, an offset and a count), as floats, to read a camera's matrices
across frames. The pass is read off a frame of that log, with the game's own pictures of its passes (a RenderDoc
capture) to tell their roles, and its call site leads to the function to hook (`tools/disasm.py`). The hashes, call
sites and layouts are the game's, so they stay in the module of that game and in no other file. Off, the replay tests
one flag per record and calls nothing of the trace. `nvn_trace` (`tests/nvn_trace.c`) checks its lines.

### Modules in libraries

A module can be a library built outside ScarletNX, so that a game's facts (its title and build IDs, its functions'
offsets, its layout) stay in a repository of their own. The runtime loads it at start, checks its version, and adds it
after the compiled-in modules (`nn/module_lib.c`). From then on it is a module like the others:
`hook_modules_select` applies or refuses it. Its work is its CPU hooks, functions of the game's code by their offset in
one build, as the [address hooks](#game-patches-hooks-by-address) of `src/nx/patches/` are, whose handlers the runtime
calls with the guest CPU through one adapter of its own ([below](#cpu-hooks-in-libraries)). A handler can record an
upscale into the game's command buffer. The library and its build files are found by `nx/loader/hook_libs.c`, which
the runtime and the recompile share.

**The header.** A library includes one header of ScarletNX, `src/nx/nn/snx_hook_module.h`, with `src/` of a
ScarletNX checkout on its include path (`#include "nx/nn/snx_hook_module.h"`). It names no type of the runtime.
It has its own code checks (`SnxHookCode`), CPU hooks (`SnxHookCpuHook`) and recorded upscale
(`SnxHookRecordedUpscale`). The runtime gives the module a table of functions (`SnxHookHost`, `module_lib.c`): the
log, a setting's value as text, and a handler's registers, guest memory, the function's own code and the upscale it
records.

**The entry point.** The library exports one function, `snx_hook_module(const SnxHookHost* host)`, which the runtime
calls once, before the game's threads. It returns the module (`SnxHookModule`): the ABI version it was built for, its
name, what it does, its title, module and build ID, its code checks and its CPU hooks, which stay where they are for
the run (the runtime never unloads a library it took). It returns NULL to refuse the runtime, after it logs why.

**The version.** The ABI has its own, `SNX_HOOK_ABI_MAJOR` and `SNX_HOOK_ABI_MINOR`, 1.2 now. A new major changes
what one side reads of the other. A new minor only adds: functions at the end of `SnxHookHost`, fields at the end of
`SnxHookModule` and of `SnxHookRecordedUpscale`, which carries its size, and flags. The runtime takes a module of its
own minor or an older one, and refuses one of another major or of a newer minor. In every major, the first fields of
`SnxHookHost` and `SnxHookModule` and the signature of `snx_hook_module` stay as they are, so a module of any version
can be read and refused. A module that needs a function of a newer minor reads `host->abi_minor` and refuses an older
runtime.

**Where.** The `hook_modules_dir` setting names the folder, and when it is empty, `hook_modules/` in the user's data
folder (`~/.local/share/scarletnx/hook_modules/` on Linux). Each `NAME.so` there (`NAME.dll` on Windows,
`NAME.dylib` on macOS) is the module `NAME`, loaded in the names' order. The file's name is the module's (letters a to
z, digits and `_`, at most 63), so `hooks_off` names a library's module as it names a compiled-in one, and the library
of a module that is off is never loaded at start: none of its code runs in the game. `hooks=0` loads none. The
recompile is another process: it opens every library of the folder, an off one too, and calls its entry point to read
its CPU hooks ([below](#cpu-hooks-in-libraries)), but runs no handler.

**The log.** A library taken says `hooks: library PATH: module NAME (hook ABI 1.2), 1 CPU hook`. One refused says
`hooks: library PATH refused: WHY`, and is closed. It is refused when the system can't load it (with the system's
reason), when it has no `snx_hook_module`, when that refuses the runtime, for another major or a newer minor, for a
module named otherwise than its file or as another module, for more than 64 code checks or one without bytes, and for
its CPU hooks' faults ([below](#cpu-hooks-in-libraries)). A library's module that doesn't apply to the game is said as
a compiled-in one is.

#### CPU hooks in libraries

A library's module lists its CPU hooks (`SnxHookCpuHook`: an offset, a name for the log, a handler) in
`SnxHookModule.cpu_hooks`, 1 to 32. They are in the game's module that `SnxHookModule.module` names (main for NULL),
of the build its `build_id` names, which it must give. Each offset is a function's entry, 4-aligned.

- **The call.** Each hook takes its function's dispatch slot (`hook.c`'s `hook_put_libs`), with one adapter for every
  library's hook (`lib_adapter`). A call runs the handler on the game's thread, at the entry, with the registers as
  the call left them (AAPCS64: x0 to x7, v0 to v7, the rest on the stack at sp). The handler reads and writes them
  through the host (`reg`, `set_reg`, `vreg`, `set_vreg`), and guest memory with `read` and `write`, which refuse an
  unmapped byte. When it returns, the function returns to its caller with the registers as the handler left them.
- **The function's own code** runs only when the handler calls `run_original`: then, with the registers as they are
  (an argument the handler changed included), until it returns to the caller's return address. The handler goes on
  after it, with the function's result in x0. Once in a call, as `hook_run_original` does it for a handler of the
  runtime's.
- **Routes.** A hook sees a function's direct calls only when the game's module was recompiled with the function in
  its route list ([How a call reaches a hook](#how-a-call-reaches-a-hook)). The recompile reads the libraries of the
  hook modules folder before the game's modules, with a host that has the log alone (no handler runs), and adds each
  CPU hook as its build ID's bytes and its offset (`snx_hook_route_add_build`). The loader routes it in every module
  whose ID begins with those bytes, under the key of that module's whole ID, `BUILDID:OFFSET` as an address hook's is
  written (`snx_hook_build_key`): so a library may give its whole build ID or its first bytes, and both route the
  same function under the same key. Install a library, then recompile the game. At start, a hook whose key the
  module's route list doesn't name isn't installed, and the log says to recompile: the recompiled code would call that
  function directly, and the hook would see only some of its calls.
- **When a hook isn't installed**, the log says why: its module doesn't apply to the game (title, build ID, code
  checks, as [above](#what-a-module-applies-to)), the game has no module of that name, the offset lies outside its
  code, the key isn't routed, or another hook has the function already. The library is refused at start for CPU hooks
  without a build ID, more than 32, a hook without a name or handler, or an offset that isn't 4-aligned.
- **The game changes the function's first instruction** (a mod's hook): that hook is off from there and the game's
  code runs, as for a hook of the runtime's ([below](#a-game-that-rewrites-a-hooked-function)).

**An upscale a CPU hook records.** `record_upscale(cpu, command_buffer, upscale)`, called from a CPU hook's handler
alone, appends one record (`OP_UPSCALE`, `nvn_cmd_record_upscale`) to the game's command buffer at `command_buffer`,
the guest address of its `NVNcommandBuffer`, which must be recording. Its textures are the game's `NVNtexture` objects
(color and output required, depth, motion vectors, exposure and reactive optional), each with an `NVNtextureView` or
0, snapshotted as every NVN command's are. At replay, in order with the buffer's other commands, the color (read from
its corner at the render size) and the output (written at `out_base`, the output size) go to the renderer's upscaler
as a module's upscale: the `render_upscaler` setting chooses, a temporal upscaler only with depth and motion vectors, a
spatial one or the copy when the flags allow. A `render_max` of 0 is the color's size. Where no upscaler can run, a
filtered copy of the color's render rectangle onto the output's stands in, so the output is always written. The log
names what ran and both surfaces, the first 4 times and at each change
(`nvn: a hook module's recorded upscale ran FSR 1 (EASU, RCAS): color ..., output ...`, or `ran no upscaler, the
filtered copy stands in`). It returns 0, and records nothing, for a command buffer that isn't recording, a color or
output that isn't a live texture, a size of 0 or a call outside a handler: the handler then calls `run_original`, and
the game's own pass runs.

**Motion vectors from depth (1.2).** A game with depth and a camera but no motion vectors of its own can still feed a
temporal upscaler: with `SNX_HOOK_UPSCALE_REPROJECTED_MOTION`, depth given and motion not, the record's `reprojection`
matrix (row-major) takes a render pixel of this frame, as (x, y, depth, 1) with x and y its place in the color's
render rectangle as -1 to 1 (y down) and the depth as its texture stores it, to where it was in the frame before, in
the same terms. It is the frame before's view-projection times the inverse of this frame's, with the picture's
coordinates and the depth's range folded in by the module, which knows its game's conventions. At replay a compute
pass (`gpu/vk/upscaler/reproject.c`) makes the motion vectors from the depth just before the upscaler runs, and the log
says so once (`vulkan: motion vectors from depth for a hook module, up to WxH`). They are the camera's motion alone:
whatever moves by itself gets the camera's motion at its pixels, which a temporal upscaler shows as a short trail. A
record of 1.1's size has no matrix and is taken as before.

**The runtime's jitter (1.2).** A temporal upscaler needs the camera to move by a fraction of a pixel each frame,
which a game without an upscaler of its own doesn't do. With `SNX_HOOK_UPSCALE_RUNTIME_JITTER` and depth given, while
a temporal upscaler takes the module's upscale, the renderer moves the scene's draws itself (`gpu/vk/render.c`): those
that test depth, with a viewport of the render size, into the depth texture given or another one of its size (a game
may draw a depth pass into one image and test a later pass against another). The offset is a sub-pixel one of a
Halton 2, 3 sequence of 8, set after each upscale for the next frame's draws, and the upscaler gets the one this
frame's draws took in place of the record's `jitter`. A spatial upscaler or the copy would show it, so with those the
draws don't move. The log says once that the runtime jitters the scene's draws.

**The display and the resolution multiplier (1.1).** `display_size(out)` gives the display's largest picture as the
game is told it, the output a game sizes its frames for. `resolution_scale(cpu, scale, target, render)`, from a CPU
hook's handler or from the module's load (`snx_hook_module`, with a NULL `cpu`: the settings and `display_size` already
answer there), asks for a resolution multiplier: every render target the renderer makes from then on is the game's
size times `scale`, so a game rendering at `render` reaches the internal resolution target `target` the user picked
(`internal_resolution`). The renderer takes it until it makes its first render target, so a module asks before the
game draws, and only while the user's `resolution` setting is at its default, 1. It returns the multiplier in
effect, and the log says what became of the request (`hooks: module NAME sets the resolution multiplier to 2 (target
3840x2160, the game renders 1920x1080)`, `hooks: a module at its load sets ...` for one asking at its load, or that the
user's setting stays, or that it came too late).

`hook_lib_cpu` (`tests/hook_lib_cpu.c`, libraries from `tests/hook_lib_cpu_mod.c`) checks the recompile's route
entries and their keys (a 16-digit prefix and the whole ID alike), the refusals, a hook the route list leaves out,
and handlers that leave the function's code out, run it around their own work, and record an upscale.
`nvn_commands` checks the record and `nvn_copy_formats` its replay, an upscaler's and the stand-in's.

A module of one game, as a library (each `0` a value read off the game's code, its function found with
[the trace](#finding-a-games-passes)): its CPU hook on the function that records the game's upscaler pass, which
records the runtime's upscale in its place, or runs the game's own code when it can't:

```c
/* one_game.c, in a repository of its own: one game's upscaler replaced by the runtime's */
#include "nx/nn/snx_hook_module.h"

static void upscaler(const SnxHookHost* host, SnxHookCpu* cpu) {
    SnxHookRecordedUpscale u = {0};
    u.size = sizeof u;
    /* the function's arguments: its command buffer, the pass's input and output textures, their sizes */
    uint64_t commands = host->reg(cpu, 0 /* its argument */);
    u.color.texture = host->reg(cpu, 0), u.output.texture = host->reg(cpu, 0);
    u.render[0] = 0, u.render[1] = 0, u.out[0] = 0, u.out[1] = 0;
    u.pre_exposure = 1;
    u.flags = SNX_HOOK_UPSCALE_RANGE_GIVEN | SNX_HOOK_UPSCALE_SPATIAL_OK;
    if (!host->record_upscale(cpu, commands, &u)) host->run_original(cpu);
}
static const SnxHookCpuHook hooks[] = {{0 /* the function's offset in main */, "the game's upscaler", upscaler}};
static const SnxHookModule module = {.abi_major = SNX_HOOK_ABI_MAJOR,
                                     .abi_minor = SNX_HOOK_ABI_MINOR,
                                     .name = "one_game_upscaler",
                                     .what = "the game's upscaler replaced",
                                     .title = 0 /* its title */,
                                     .build_id = "00" /* main's build ID */,
                                     .cpu_hooks = hooks,
                                     .ncpu_hooks = 1};
SNX_HOOK_EXPORT const SnxHookModule* snx_hook_module(const SnxHookHost* host) {
    return host->abi_major == SNX_HOOK_ABI_MAJOR ? &module : NULL;
}
```

and its build, with CMake, as `one_game_upscaler.so`:

```cmake
add_library(one_game_upscaler MODULE one_game.c)
target_include_directories(one_game_upscaler PRIVATE "${SCARLETNX_SOURCE_DIR}/src")
set_target_properties(one_game_upscaler PROPERTIES PREFIX "" SUFFIX "${CMAKE_SHARED_LIBRARY_SUFFIX}"
                      C_VISIBILITY_PRESET hidden)
```

`hook_module_lib` (`tests/hook_module_lib.c`) builds libraries of no game from `tests/hook_module_lib_mod.c`, one for
each case: a folder's library taken, a file that isn't a library passed over, the library `hooks_off` names never
loaded, each refusal above with its reason, the module as the runtime's (title, build ID, code check, CPU hook)
applied to its game and not to another build, and the host's log and setting read. `snx-test-hook-module-lib --load
PATH` loads one library and prints what
the runtime made of it, to check a module built elsewhere without a game. Only Linux runs it yet: the loads on
Windows, macOS and Android are not tested, and Android's shared storage is mounted noexec.

## A game that rewrites a hooked function

No game changes its own SDK while it runs, but a mod's loader (skyline, exlaunch) hooks SDK functions by writing a
branch over their first instruction, and keeps a trampoline: that instruction, then a jump back to the one after it.
When such a function has a native hook, its slot would keep running the handler, and the mod's code would never run.

So when the JIT drops a range's code (`jit_code_invalidate`), `hook_restore` compares the first 4 bytes of each hooked
function in it with the ones `hook_install` saw: one A64 or A32 instruction, or two Thumb ones. Only those 4 bytes,
on purpose: they are what every mod's hook writes over, and a change further into the function is not looked at. A
function whose first 4 bytes changed has its native hook invalidated:

- the hook's adapter (its own host code, which a copy of its address still reaches: an NRO's import bound to it, a
  direct call the JIT compiled) is rewritten into a jump to `hook_invalidated` (`jump.c`, `hook.c`). It is a jump, not
  a call, on every architecture: the sad path gets the arguments the adapter's caller left and returns straight to
  that caller. `hook_invalidated` runs the game's code now at the function (`hook_original`, the JIT's code of it);
- its slot is left empty, so a call through it compiles the game's code;
- a warning names the function, its address, its first instruction before and after, and the handler that answered
  it. A hook whose adapter is the same code (another site of the same entry, or an entry the linker folded into it) is
  named too, and runs its own code from then on.

The game's code then runs as on the console, the SDK's own code included wherever it calls it: a mod's call of the
original function through its trampoline runs the SDK's code of that function, which may reach a service only the
native layer answers, unless an ext_skyline hook takes it ([below](#a-mods-call-of-the-original)).

When the system refuses to make the adapter's code writable (`mprotect`, `VirtualProtect`), the game stops with the
reason and the values, since the mod's code would never run.

The jump, by host architecture, written past the adapter's landing pad (x86_64's `endbr64`, AArch64's `bti`; an
AArch64 `paciasp` becomes `bti c`, as the jump must not sign the return address):

| Host | Jump | Bytes |
| --- | --- | --- |
| x86_64 | `jmp rel32`, or `movabs r11` and `jmp r11` | 5 or 13 |
| AArch64 | `b`, or `ldr x16, #8`, `br x16` and the address | 4 or 16 |
| ppc64 ELFv2 (LE, BE) | `r12` loaded with the global entry (`lis`, `ori`, `sldi`, `oris`, `ori`), `mtctr r12`, `bctr` | 28 |
| ppc64 ELFv1 | `r12` loaded with the descriptor, `ld r0`, `ld r2`, `mtctr r0`, `bctr`, at the descriptor's entry | 36 |
| s390x | `brcl 15`, or `lgrl %r1` of an aligned doubleword and `br %r1` | 6, or up to 22 |
| mips64 (EL, EB) | `$t9` loaded with the address, `jalr $zero, $t9` and the delay slot's `nop` | 32 |
| riscv64 | `auipc t1` and `jalr`, or `auipc`, `ld` of an aligned doubleword and `jalr` | 8, or up to 24 |
| loongarch64 | `pcaddu18i $t0` and `jirl` (128 GiB either way) | 8 |
| ia64 | one MLX bundle, `nop.m 0` and `brl.sptk.few`, at the descriptor's entry | 16 |

It assumes the adapter's code is long enough to hold the jump in the default build. Every adapter calls its handler
and stores its result, so none is that short, though an x86_64 function can be a lone `ret`.
`tests/hook_invalidated.c` runs the jump on x86_64 alone (the test is skipped on another host); `tests/hook_jump.c`
checks every architecture's bytes on any host, against llvm-mc's disassembly and, for IA-64, GNU objdump's (clang has
no IA-64 target yet; llvm-ia64 will take its place). Under NCE the JIT drops no code, so nothing is invalidated
there. A 32-bit game's hooks are not invalidated either: their slots are blocks `hook_restore` does not handle.

## A mod's call of the original

`src/nx/nn/ext_skyline.hooks` holds native hooks for the detours loaders are known to make (skyline's and
Cobalt's on `nn::fs::MountRom`, one game's optimizer mod's on `nn::fs::MountRom` by name alone,
`nn::fs::MountSdCard` and `nn::fs::OpenFile`, skyline's on `nn::ro::Initialize`), each with the handler and
arguments of the function's native hook (`tests/ext_skyline_hooks.py` checks them against it), in a table of its own
(prefix `skyline`, `ext_skyline.c`). They are not installed at the start: once a function's native hook is
invalidated, a dispatch miss at a pc (`rt_miss`, `hook_trampoline`) checks whether the code there is the mod's
trampoline of that function, its first instructions as `hook_install` saw them
(1, or 4 or 5 when the hook was too far for a `b`) followed by a jump back to the one after them (a `b`, or
`ldr x17, #8` and `br x17` of the address, after a `nop` that aligns it), as skyline's and exlaunch's
And64InlineHook writes it. The first time it runs, the hook takes the trampoline's slot, and the mod's call of the
original runs the handler. A handler that hands a case back to the function's own code (`hook_run_original`) runs the
trampoline's code, entered at the trampoline: the SDK's, past the instructions the mod moved. A first instruction that
reads the pc (`adr`, `adrp`, a literal load, a branch) is rewritten in the trampoline, so it is never recognized, and
that function's original stays the SDK's code. A hook of a detour no loader makes is never reached, so the file lists
the known ones only, and its hooks change no route list.

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
(`src/nx/nn/verify.c`) copies the buffers into scratch memory in the kernel region (buffers that overlap or
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
`hooks_off` turns off the families it names (`hooks.hook`'s `family` lines, such as `libc`), and the
[modules](#modules) it names, to tell which. The
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
