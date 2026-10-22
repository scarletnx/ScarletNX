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

## Checking a hook

The `hooks` setting (`[Debug]`, on by default) turns every hook off, to tell whether a problem comes from one, and
`hooks_off` turns off the families it names (`hooks.hook`'s `family` lines, such as `libc`), to tell which. The
log names each hooked function at startup (`hook: memcpy at ... runs hook_memmove`). A handler that wants to hand a
case back to the function's own code calls `hook_original(addr)` with the CPU as the call left it.
