<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# NRO implementation notes

## Index

* [Reference](#reference)
* [Validation and limits](#validation-and-limits)
* [Regression tests](#regression-tests)
* [Loading NROs at run time (`ldr:ro`)](#loading-nros-at-run-time-ldrro)
  * [Reference and ABI details](#reference-and-abi-details)
  * [Execution and memory](#execution-and-memory)
  * [NROs made ahead of time](#nros-made-ahead-of-time)
  * [Tests](#tests)

ScarletNX already parsed NRO files and staged optional embedded RomFS data through
`-r`. Direct positional NRO input now bypasses container extraction and goes
straight to recompilation and launch. It always recompiles rather than trusting a
same-stem executable left by another input. The existing mixed NSO/NRO data-cache
check still applies. Use a different output name or remove conflicting cached data.

## Reference

Atmosphère is the only external implementation reference for these changes, pinned to
[`c7e6c35e900b52c364d557bdbfe554c857158d97`](https://github.com/Atmosphere-NX/Atmosphere/tree/c7e6c35e900b52c364d557bdbfe554c857158d97):

- [`NroHeader` in `ro_types.hpp`](https://github.com/Atmosphere-NX/Atmosphere/blob/c7e6c35e900b52c364d557bdbfe554c857158d97/libraries/libstratosphere/include/stratosphere/ro/ro_types.hpp)
  defines the 0x80-byte header, segment offsets, module ID and compression flag.
- [`ValidateNro` in `ro_service_impl.cpp`](https://github.com/Atmosphere-NX/Atmosphere/blob/c7e6c35e900b52c364d557bdbfe554c857158d97/stratosphere/ro/source/impl/ro_service_impl.cpp)
  checks the segment order and contiguous uncompressed layout.

The implementation is written for ScarletNX's existing C loaders. No Atmosphère
source files are vendored.

## Validation and limits

Both loaders require a complete header, a text segment starting at zero that contains
the header, and contiguous text, read-only and data segments covering the declared
image. Calculations use 64-bit values so 32-bit header fields cannot wrap bounds checks.
Compressed NROs are rejected explicitly rather than interpreted as raw instructions.

The runtime checks that the build ID, segment layout and BSS size match the recompiled
descriptor. It copies only the declared image, not trailing assets, and zeroes BSS and
mapping padding. An NRO descriptor selects `exefs/main.nro` or `main.nro`, so an old
NSO named `main` cannot shadow it. As with the existing NSO check, the build ID is not
a cryptographic comparison of every instruction.

Unlike Atmosphère's on-device registration service, ScarletNX retains support for
byte-packed synthetic fixtures rather than requiring page-aligned section sizes.
The standalone NRO workflow does not implement NRR authorization, compressed NRO decompression or a new
homebrew-loader startup ABI. Dynamic loading through the emulated `ldr:ro` service is described
[below](#loading-nros-at-run-time-ldrro). It is separate from this workflow. Existing ASET
parsing is unchanged. Restaging an NRO without embedded RomFS removes the generated
`romfs.bin` from the previous image, without touching saves.

## Regression tests

- `driver_workflow`: lowercase and uppercase extensions, paths with spaces, headless
  forwarding, stale checkpoints, missing input, recompilation failures and retries.
- `loader_native`: asset-free NROs, all truncated header lengths, noncontiguous
  sections and compression rejection, alongside existing asset tests.
- `runtime_nro`: actual runtime loader with synthetic files and memory stubs, both
  staging paths, BSS clearing, asset exclusion, permissions, short files, overflowing
  sizes and descriptor mismatches. Exact-sized allocations allow ASan to detect reads
  beyond a truncated file.
- `native_recompiler`: synthetic NRO through LLVM IR to native module execution,
  plus removal of stale embedded RomFS when an asset-free NRO is staged.

These tests do not establish compatibility with a particular homebrew application or game.

## Loading NROs at run time (`ldr:ro`)

`src/nx/hle/services/ldr.c` implements IRoInterface for `ldr:ro` and `ro:1`:

- 0: MapManualLoadModuleMemory (LoadNro), returning the mapped guest address.
- 1: UnmapManualLoadModuleMemory (UnloadNro), taking **that returned address**, not the source heap address.
- 2 / 3: RegisterModuleInfo / UnregisterModuleInfo (LoadNrr / UnloadNrr).
- 4: RegisterProcessHandle (Initialize), required before loading.
- 10: RegisterProcessModuleInfo, with `ro:1`'s JIT-plugin NRR kind.

### Reference and ABI details

Cross-referenced with Eden at commit `a594c8d60ef79280adc046760561912207e16e17`:

- [ro.cpp](https://git.eden-emu.dev/eden-emu/eden/src/commit/a594c8d60ef79280adc046760561912207e16e17/src/core/hle/service/ro/ro.cpp): commands, process binding, result module 22,
  NRO layout checks, duplicate module IDs, NRR hash authorization and unload-by-base.
- [cmif_serialization.h](https://git.eden-emu.dev/eden-emu/eden/src/commit/a594c8d60ef79280adc046760561912207e16e17/src/core/hle/service/cmif_serialization.h):
  `ClientProcessId` consumes **eight raw bytes**, even though its value comes from the HIPC descriptor.
  Consequently LoadNro's four arguments begin at raw offsets 8, 16, 24 and 32.

This is a C implementation for ScarletNX's single-process memory model, not a copy of Eden's page-table code.
The runtime now injects its process ID into incoming PID descriptors rather than trusting a client placeholder;
its current-process pseudo handle also resolves to a process object for Initialize's copied handle.

An NRR stores SHA-256 hashes of complete, unrelocated NRO images, **not** build IDs. Registration snapshots the hash
list after checking its bounds, header size, program ID and kind. LoadNro checks the mapped image against those
hashes, then rejects an already-loaded module ID. RSA certification/signature verification is not implemented,
matching Eden's HLE registration path. NRR unregistration does not invalidate already-loaded NROs.

### Execution and memory

The service does **not** relocate the NRO or resolve its ELF symbols. The guest `nn::ro` linker does that after
LoadNro returns. It also manages constructors/destructors. Pre-relocating in the service would change bytes the
client expects to receive untouched. The caller's BSS backing is preserved, not silently zeroed.

An NRO the recompiler found in the game's RomFS has its code made ahead of time
([below](#nros-made-ahead-of-time)). The code of any other NRO is compiled by the runtime JIT. When LoadNro maps
such an NRO, its text joins
the JIT's images (`jit_code_added` in `src/nx/runtime/guest_jit.c`, `snx_jit_add_image` in
`src/common/cpu/recompiler/llvm/jit_service.c`). Its pcs lie outside `CPU.disp`, so a call into it misses in
`snx_lookup` and reaches `RtAPI.miss`. Resolution first checks any compatible AOT variants through `variant_resolve`,
then `snx_jit_resolve` reads the JIT's own table for that image without a lock. A resolved call therefore includes
the miss path and applicable variant lookup as well as the JIT-table load. The first call to a function waits for its compile,
as for any code the AOT module lacks. JIT code for the NRO
calls the module's functions directly and other code through the dispatch. UnloadNro, and the end of the session,
remove the image before unmapping its memory: `snx_jit_remove_image` waits for the compiles reading it and forgets
its code, so an NRO mapped at the same place later is compiled afresh. NRO pcs are not written to `.hints`, since the
next run maps the NRO elsewhere. Without the JIT (a system it can't start on), NRO code can't run.

A host `dlopen()` alone cannot execute ARM64 NRO bytes on another host architecture. Nor can an existing fixed-base
ScarletNX AOT module be relocated just by moving its dispatch entries: generated PC-relative addresses and direct
calls use its original guest addresses. The JIT compiles the NRO at the address it is mapped at, and an NRO made
ahead of time is mapped at the one address its code was made for.

### NROs made ahead of time

The recompile of a game finds every `.nro` file of its RomFS (an extracted game's `romfs/` folder, or a dump's RomFS
read in place), and makes each one's code a module of its own (`src/nx/runtime/driver/nros.c`):

- Each NRO is named by the SHA-256 of its image, the hash `ldr:ro` checks against an NRR. Two files with the same
  image are one NRO.
- Each gets one guest address: they lie one after another behind the game's own modules, a guard page before each, in
  the order of their hashes (`snx_nro_slots_place` in `src/nx/loader/loader.c`), so the same set of NROs is laid out
  the same at every recompile. An NRO goes where `ldr:ro` would put the first one it maps anyway.
- The game module tells the runtime where they go: its `snx_nros()` returns the table
  (`src/nx/loader/nro_slots.h`), a symbol with a version of its own beside `snx_module()`, so `SNX_ABI` is unchanged
  and a module without NROs has none.
- Each NRO's code is `<Game>_nro/<sha256>.so` (`.dll` on Windows) beside `<Game>_code.so`: a module like the game's,
  whose descriptor names the NRO as its one module at its address. The modules an earlier recompile left there for
  NROs the game has no more are removed.

A file that is no NRO `ldr:ro` maps (not an NRO, compressed, an image or a BSS that is not whole pages) is said and
left out. An NCE recompile and an iOS pack make none.

At run time `CPU.disp` covers the NROs' addresses too, and the memory there is kept for them: an NRO without a place
is mapped outside it. When LoadNro is given an image whose hash the table has (`ro_load_nro` in
`src/nx/hle/services/ldr.c`):

- The image is mapped at its address, and its module is loaded and checked to be that NRO's for that address
  (the ABI, the address and size, the module ID); its entries go into `CPU.disp` (`jit_nro_added` in
  `src/nx/runtime/guest_jit.c`). Calls into the NRO then find its code in the table, with no miss.
- The module stays loaded when the NRO is unmapped, its entries leave the table, and the next LoadNro of that NRO maps
  it at the same address again.
- The NRO's text joins the JIT's images as well, inside the table (`snx_jit_add_image`): a pc the module has no
  function for (a jump-table case the analysis did not find) is compiled into `CPU.disp` like the game's own.
- When the address is in use (other memory the game put there, or an address space with no code room there), the
  NRO is mapped where there is room, the JIT runs that mapping, and the log names the NRO's hash and both
  addresses. One session cannot map one NRO twice at once: the second LoadNro is refused as already loaded.
- With no module file for it, or one that is refused (another ABI, another NRO or address: said once), the NRO is
  still mapped at its address and the JIT runs it.

The guest patching an NRO's code (IC IVAU) makes the functions compiled from those lines stale, as it does the game
module's; each mapping starts from the module as it was made.

An NRO calls the game's functions, and many of its own, through its PLT: a stub loads the function's address from a
slot the game's linker (`nn::ro`) fills while the game runs, and branches to it. The recompile reads the NRO's PLT
slots (`snx_module_plt` in `src/nx/loader/loader.c`) and binds each stub whose symbol names a function of the NRO
itself, or one that a single module of the game exports and no hook names. Such a stub compares the address it
loaded with the one function it is bound to: equal, it calls that function, the NRO's own as any call inside the
module, the game's through the module's import of it; anything else in the slot (the linker has not bound it yet,
another module defines the symbol first) goes through `CPU.disp` as before. A module's imports are its
`snx_imports()` table (`nro_slots.h`): each pointer starts as the module's own lookup in `CPU.disp`, and the runtime
puts the game module's function there when it has one for the address and its dispatch slot holds that function
(`nro_imports_bind` in `guest_jit.c`); a slot a hook took keeps the call in the table, and a slot given another
function later, as NVN's implementation of a function the game resolves while it runs, takes the import back to the
table (`jit_slot_taken`). A recompile with `--hook-direct-calls` binds none.

What is not made ahead of time: an NRO a mod adds or replaces (its hash is not in the table), an NRO the game takes
from outside its RomFS, and the NROs of a game whose update is recompiled from the update's dump alone, which has
no RomFS without its base.

`mem_map_code` / `mem_unmap_code` in `src/nx/memory/mem.c`:

- Validate the entire source and BSS ranges as unlocked, RW normal memory; reject overlaps and holes.
- Choose a free contiguous code view below the heap with a free guard page on either side.
- Copy the image and BSS, lock the source pages (`PERM_NONE`, locked attribute), then publish the view.
- Expose text RX, rodata R, and data/BSS RW with AliasCode/AliasCodeData states.
- Copy modifications back on unload, restore source permissions, and release the view.
- Do not double-charge the source heap through `g_code_memory`.

These transactions hold the memory lock, including source validation, so heap shrinking and concurrent mappings
cannot consume the same pages. Like the existing MapMemory implementation, this uses copies rather than true
host aliases; source permissions are guest metadata rather than host page protections. Clients must not access
locked source ranges. Validation failures roll back the mapping. A service lock serializes requests, including
cloned sessions, and session destruction releases owned mappings and hash snapshots.

Limits: 64 NROs and 64 NRRs per process (the stock limits; Eden extends these to 256). Compressed NROs are rejected.
Only the current application process is supported. `ro:dmnt` is not part of this implementation.

### Tests

With a configured build:

```sh
cmake --build --preset default --target snx-test-ldr-service snx-test-memory snx-test-runtime-nro \
    snx-test-nro-modules snx-test-runtime-variants snx-test-jit-service
ctest --test-dir build/default -R '^(ldr_service|memory_accounting|runtime_nro|nro_modules|runtime_variants|jit_service)$' \
    --output-on-failure
```

`ldr_service` uses synthetic images and the real service and memory block list. For an NRO made ahead of time it
covers the mapping at its address by its hash, the same address again after an unload, an NRO without a place kept
out of those addresses, an address in use (with the line that says so), a place of another size and one with no code
room. `nro_modules` recompiles a small extracted game and checks the table and each NRO's module, whose code must run
as code at its address and call the game's function and its own through its PLT, directly once bound and through
the table otherwise; `runtime_variants` loads such a module through `guest_jit.c`, and `jit_service` compiles into
the table from an image inside it.
It covers PID placeholders, initialization/handle validation, malformed ranges/headers/hash lists, hash snapshots,
unauthorized/duplicate images, commit failure rollback, permissions, preserved bytes/BSS, no premature
relocations, unload-by-base, copy-back, reloading, session cleanup, simultaneous loads, and both capacity limits.
Running code the module lacks (an NRO's, through `RtAPI.miss`) is `llvm_module`'s.
SHA-256 has independent standard test vectors. Platform backing and IPC transport are test doubles.

Validation in the coding container: these three CTest targets passed using Clang 22. Service/memory tests passed
GCC 14 ASan+UBSan. The runtime translation
units changed here compiled with Clang's missing-prototype/declaration checks. No game dump was supplied, so no
retail-game scene or full Vulkan runtime build was tested.
