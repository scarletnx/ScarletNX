# Runtime NRO loading (`ldr:ro`)

`src/nx/hle/services/ldr.c` implements IRoInterface for `ldr:ro` and `ro:1`:

- 0: MapManualLoadModuleMemory (LoadNro), returning the mapped guest address.
- 1: UnmapManualLoadModuleMemory (UnloadNro), taking **that returned address**, not the source heap address.
- 2 / 3: RegisterModuleInfo / UnregisterModuleInfo (LoadNrr / UnloadNrr).
- 4: RegisterProcessHandle (Initialize), required before loading.
- 10: RegisterProcessModuleInfo, with `ro:1`'s JIT-plugin NRR kind.

## Reference and ABI details

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

## Execution and memory

The service does **not** relocate the NRO or resolve its ELF symbols. The guest `nn::ro` linker does that after
LoadNro returns. It also manages constructors/destructors. Pre-relocating in the service would change bytes the
client expects to receive untouched. The caller's BSS backing is preserved, not silently zeroed.

NRO code is compiled by the runtime JIT. When LoadNro maps an NRO, its text joins
the JIT's images (`jit_code_added` in `src/nx/runtime/guest_jit.c`, `snx_jit_add_image` in
`src/common/cpu/recompiler/llvm/jit_service.c`). Its pcs lie outside `CPU.disp`, so a call into it misses in
`snx_lookup` and reaches `RtAPI.miss`. There `snx_jit_resolve` reads the JIT's own table for that image without a lock:
once a function is compiled, a call costs the miss and that load. The first call to a function waits for its compile,
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

## Tests

With a configured build:

```sh
cmake --build --preset default --target snx-test-ldr-service snx-test-memory snx-test-runtime-nro
ctest --test-dir build/default -R '^(ldr_service|memory_accounting|runtime_nro)$' --output-on-failure
```

`ldr_service` uses synthetic images and the real service, memory block list, and generated AArch64 interpreter.
It covers PID placeholders, initialization/handle validation, malformed ranges/headers/hash lists, hash snapshots,
unauthorized/duplicate images, commit failure rollback, permissions, preserved bytes/BSS, no premature
relocations, unload-by-base, copy-back, reloading, session cleanup, simultaneous loads, and both capacity limits.
It executes a dynamically mapped `mov x0, #42; ret` function, then an NRO-to-AOT-dispatch callback and return.
SHA-256 has independent standard test vectors. Platform backing and IPC transport are test doubles.

Validation in the coding container: these three CTest targets passed using Clang 22; the full interpreter witness
also passed UBSan. Service/memory tests without the interpreter passed GCC 14 ASan+UBSan. The runtime translation
units changed here compiled with Clang's missing-prototype/declaration checks. No game dump was supplied, so no
retail-game scene or full Vulkan runtime build was tested; interpreted NRO performance remains unmeasured.
