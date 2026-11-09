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

NRO code is compiled by the runtime JIT. When LoadNro maps an NRO, its text joins
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
calls use its original guest addresses. The JIT compiles the NRO at the address it is mapped at, so neither is needed.

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
cmake --build --preset default --target snx-test-ldr-service snx-test-memory snx-test-runtime-nro
ctest --test-dir build/default -R '^(ldr_service|memory_accounting|runtime_nro)$' --output-on-failure
```

`ldr_service` uses synthetic images and the real service and memory block list.
It covers PID placeholders, initialization/handle validation, malformed ranges/headers/hash lists, hash snapshots,
unauthorized/duplicate images, commit failure rollback, permissions, preserved bytes/BSS, no premature
relocations, unload-by-base, copy-back, reloading, session cleanup, simultaneous loads, and both capacity limits.
Running code the module lacks (an NRO's, through `RtAPI.miss`) is `llvm_module`'s.
SHA-256 has independent standard test vectors. Platform backing and IPC transport are test doubles.

Validation in the coding container: these three CTest targets passed using Clang 22. Service/memory tests passed
GCC 14 ASan+UBSan. The runtime translation
units changed here compiled with Clang's missing-prototype/declaration checks. No game dump was supplied, so no
retail-game scene or full Vulkan runtime build was tested.
