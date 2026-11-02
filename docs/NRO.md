<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# NRO implementation notes

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
homebrew-loader startup ABI. Dynamic loading through the emulated `ldr:ro` service is described in
[LDR_RO.md](LDR_RO.md); it is separate from this workflow. Existing ASET
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
