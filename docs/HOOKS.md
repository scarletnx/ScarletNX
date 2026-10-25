# Hooks

A hook runs a function the game exports with a native handler of the runtime's instead of its recompiled code. The
handlers are accelerators: each does what its function is known to do from reading the function's code, on the
guest's own data as the function would leave it. They never copy or reference the SDK. A hook is installed when the
game starts, so adding or changing one needs no recompile.

## How a call reaches a hook

Every call to an exported function goes through its slot in the dispatch table, `g_disp` (`snx_lookup` in `cpu.h`):

- a call into another of the game's modules, through the PLT stub and the GOT the game's own dynamic linker fills
  (the main executable's `memcpy` is the SDK module's);
- a call through a function pointer or a virtual call (`BLR`), NVN's procedure table among them;
- a direct call (`BL`, or a tail call's `B`) within a module: the recompiler compiles a call to an exported function
  as a call through its slot (`snx_codegen_exported`, `ir_codegen.c`'s `call`), so LLVM never inlines one into its
  callers either (`SNX_ABI` 15);
- a call from the runtime JIT's code, which calls an AOT function directly only while its slot still holds it
  (`jit_service.c`'s `direct`).

Calls to the functions a module doesn't export stay direct calls, so they cost what they did. `hook_install`
(`src/nx/hle/hook/hook.c`) reads the game's exported functions (`kernel_load_symbols`) and puts the hook's adapter
into each matching function's slot, before the game's first thread and the JIT start. The slots never change while
the game runs, except for `hook_restore` putting a hook back after the JIT drops the code of a range
(`jit_code_invalidate`).

What no hook sees is code the game's own compiler inlined from an SDK header: there is no call to take. A handler
therefore keeps the guest's data as the function does, so inlined copies read what it left.

## Declaring a hook

Hooks are declared in `src/nx/hle/hook/hooks.hook`, one per line, each under the `family` line before it:

```
family libc
hook memcpy(addr dst, addr src, u64 size) -> addr = hook_memmove
```

- The symbol is the function's name as the game's dynamic symbol table spells it. A C++ function goes by its mangled
  name (`_ZN2nn2os...`). Names are never parsed, demangled or mangled.
- The arguments and result are the function's as C sees them. A C++ method takes `this` as its first argument. The
  types are `void` (a result only), `bool`, `u8` to `u64`, `i8` to `i64`, `addr` (a guest address) and `f32`/`f64`.
  Integers take x0 to x7 and floats v0 to v7 under AAPCS64, counted apart. An argument that would go on the stack is
  refused.
- The handler is `ret handler(CPU* c, args...)`, declared by the generated `hle/hook/hooks.hook.h`. It reads and
  writes guest memory at `c->mem + addr`, and calls the HLE kernel for anything that waits (a guest thread holds its
  emulated core until a kernel wait releases it, so a handler never blocks on a host primitive).

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
  will implement (`gpu/nvn/nvn.hook`). The runtime doesn't read it yet, so it is unknown and no `nvn` hook installs.
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

## Checking a hook

The `hooks` setting (`[Debug]`, on by default) turns every hook off, to tell whether a problem comes from one, and
`hooks_off` turns off the families it names (`hooks.hook`'s `family` lines, such as `libc`), to tell which. The
log names each module's SDK version and each hooked function at startup (`hook: memcpy at ... runs hook_memmove`). A handler that wants to hand a
case back to the function's own code calls `hook_original(addr)` with the CPU as the call left it.
