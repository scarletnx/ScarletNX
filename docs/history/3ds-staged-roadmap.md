<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# The 3DS staged roadmap

## Index

* [2. CIA extraction, memory controller and static execution](#2-cia-extraction-memory-controller-and-static-execution)
* [3. Basic HLE OS and a SwIPC-derived generator](#3-basic-hle-os-and-a-swipc-derived-generator)
* [4. Audio DSP](#4-audio-dsp)
* [5. Video graphics](#5-video-graphics)
* [6. HID and remaining peripherals](#6-hid-and-remaining-peripherals)

*Snapshot: this is the first plan of the 3DS driver, in stages 2 to 6. The parity roadmap in
[3DS](../platforms/3ds/README.md#parity-roadmap-azahar) replaced it, and this page is not kept up to date.*

Each stage should be one independently reviewable commit/diff, with all earlier
stages as its base. Stubs must return explicit unsupported errors, not pretend
that an unimplemented service/device succeeded.

## 2. CIA extraction, memory controller and static execution

The extraction and independent memory-controller subset is implemented as 2a.
Stage 2b1 supplies ARM11 integer profile selection and checked-memory integration.
Stage 2b2 adds module ABI/loading, bounded exhaustive ARM/Thumb static dispatch,
ExHeader process/stack/TLS setup and the asset-free extract/AOT/run witness.
Stage 2b3 removes the tiny-image restriction with bounded chunked static CFG
compilation and passes the external SuperTux first-SVC native bootstrap gate.
Stage 2b4 adds kernel-bootstrap objects, configuration, checked allocations and
serialized ARM11 exclusives. That checkpoint reached ConnectToPort before `main`;
initial 3a passed `srv:` registration and stopped at absent APT:S. Initial 3b
passes the bounded APT startup/thread/HID path and reaches main. The minimal
shared-backed FS increment opens RomFS. Scalar VFPv2 and its offline CFG
fallthrough pass `gettimeofday`, and the archive timestamp control was answered
later, before the parity roadmap took over.
**Stage 2 is not complete**: broader discovery/ISA coverage, full VFP, concurrent
reservation/scheduling semantics, validated resume/cache and wider runtime
validation remain pending.

Reference Azahar `src/core/file_sys/{cia_container,ticket,title_metadata,
ncch_container,romfs_reader}.*`, `src/core/hle/service/am/am.cpp` (installation),
`src/core/loader/ncch.*`, `src/core/memory.*` and kernel process/VM code.

- CIA is an installable container, not directly executable code. Validate all
  section sizes/alignment, signature-dependent ticket/TMD body offsets, content
  bitmap/indices and hashes; select the application NCCH, not the first content
  unconditionally. Distinguish updates/DLC, absent contents and encrypted layers.
- Implement the ARM11/memory/module prerequisites above. Split NCCH ExHeader,
  ExeFS `.code` (including BLZ), RomFS, segment permissions, BSS and TLS. Preserve
  metadata needed for process setup. Fail closed for unsupported crypto/formats;
  accept user-supplied keys only, never distribute keys/firmware.
- Always materialize extracted content into `work/3ds/<title>/...`, using the
  shared NX-style host filesystem utilities. Extract RomFS safely into a tree;
  no mount-on-demand of the original CIA. Reject traversal, duplicates, malformed
  offsets, oversized/decompression-bomb inputs and symlink escapes.
- Pipeline: **extract -> recompile -> run**. Reuse stage supervision and shared
  FS/tree logic, not NX's NSO/A64 loader. Publish staged extraction/compiler
  outputs atomically. Cache identity includes input hashes, keys/configuration
  identity without storing secrets, ISA, module ABI and compiler version; never
  reuse an incomplete build. Launch only compatible native modules.
- Gate: synthetic CIA/NCCH/RomFS fixtures, malformed/truncated/overflow cases,
  memory permissions/alias/MMIO tests, ARM/Thumb native witness, resume/cache
  invalidation and an asset-free end-to-end extract/AOT/run test.

## 3. Basic HLE OS and a SwIPC-derived generator

Reference Azahar kernel/SVC/IPC code and `src/core/hle/service/`. Use
`tools/swipc/swipc.c` as the parser/schema/codegen basis, with a **separate CTR
wire adapter**: 3DS TLS command words, normal/translation counts, handles,
static/mapped buffers and response layout are not Switch HIPC/CMIF.

Initial 3a implements a strict generated subset, real session lifetimes, client
registration/lookup and FS caller/SDK association. Initial 3b adds the observed
APT startup, native threads and bounded PTM/HID resources, not a full applet or
input subsystem. The minimal FS increment reuses NX's common indexed/mapped
host backing for read-only SelfNCCH access. A synthetic native CIA reads and
resolves a real extracted file; the full stage/OS/filesystem witness below
remains unfinished.

- Threads, handles, events, waits, scheduling, SVCs, TLS, shared memory and result
  codes; checked guest buffers and handle lifetimes at every IPC boundary.
- Start with `srv:`, `fs:USER`, `APT:S`/`APT:U`, configuration/time and resource services;
  implement shared-memory/event plumbing needed by later DSP/GSP/HID stages.
- Gate: generated dispatch against hand-encoded requests, malformed descriptors,
  unknown commands, handle transfer/close, IPC-buffer bounds, blocking/wakeup,
  process exit and a small homebrew OS/filesystem witness.

## 4. Audio DSP

Reference `src/audio_core/{dsp_interface.*,hle/*}` and
`src/core/hle/service/{dsp,csnd}/`. Port HLE DSP shared memory, voices, decoding,
resampling, mixing/effects, pipes/semaphores and interrupt events; adapt output to
ScarletNX's host audio boundary. Do not substitute silence for implemented DSP.
LLE/firmware behavior needs a separately licensed/dependency-audited integration.
Gate: asset-free PCM/ADPCM decoding/mixing golden tests, pipe/event lifecycle,
ring-buffer wrap, sample-rate timing and deterministic offline rendering.

## 5. Video graphics

Reference Azahar GSP/GPU/LCD, `src/video_core/pica/`, shader and software renderer
first, then Vulkan as an acceleration path. PICA200 is **not Maxwell**.
Implement GSP shared queues/interrupts, GPU command lists, physical-memory access,
DMA/fills/display transfers, texture formats, shader behavior, framebuffer layout
and top/bottom screen presentation. Integrate renderer cache invalidation with
CPU/DSP memory writes rather than copying the NX GPU frontend.
Gate: deterministic software-rendered golden scenes, GPU command/memory bounds,
transfer/coherency tests, two-screen layouts, then Vulkan comparisons.

## 6. HID and remaining peripherals

Reference `src/core/hle/service/{hid,ir,cam,mic,nfc,ptm,mcu,qtm}/` and the Azahar
frontend/input interfaces. Wire host input to 3DS shared-memory sample rings,
pad/circle-pad/touch, accelerometer/gyro, IR/New 3DS inputs and events with correct
timestamps and screen-coordinate transforms. Camera/microphone/NFC and lid/power
policies require explicit support or documented unsupported results; never claim
that every peripheral is wired when only button input works.
Gate: injected input/touch tests, ring wrap/events, disconnect/reconnect, New/Old
model differences, camera/mic bounds and end-to-end homebrew interaction.
