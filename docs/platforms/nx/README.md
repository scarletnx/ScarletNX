<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# The Switch (NX)

The Switch runtime (`src/nx/`) plays Switch games. This page lists its layers, with the code and the documents of
each. The documents of this folder describe the parts that only the Switch has. `scarletnx` takes every one of the
[common options](../../user/usage.md#common-options) but `--output`.

## Layers

| Layer | Code | Documents |
| --- | --- | --- |
| Loader | XCI and NSP extraction (`src/nx/runtime/driver/extract.c`), NSO, NRO and NPDM (`src/nx/loader/`) | [Usage](../../user/usage.md), [NRO files](nro.md) |
| CPU | AArch64 compiled ahead of time with LLVM (`src/common/cpu/recompiler/`), 32-bit code through the block compiler (`src/common/cpu/arm/`), a JIT for the code that the AOT build missed, and NCE on AArch64 hosts | [Architecture](../../dev/architecture.md), [CPU plan](../../layers/cpu/plan.md), [NCE](nce.md), [AOT packs](../../user/aot-packs.md) |
| Kernel and HLE | the kernel and the SVCs (`src/nx/hle/kernel/`, `hle/svc/`, `memory/`), the services (`hle/services/`), the file system (`hle/filesystem/`), the hooks (`hle/hook/`) | [IPC interfaces](ipc.md), [Hooks](hooks.md), [Filesystem](filesystem.md), [Networking](networking.md), [Memory accounting](memory-accounting.md) |
| GPU | NVN (`src/nx/gpu/nvn/`) and the Maxwell front end (`src/nx/gpu/maxwell/`), Maxwell shaders to SPIR-V (`gpu/shader/`), the Vulkan renderer (`gpu/vk/`) | [NVN](../../layers/gpu/nvn.md), [GPU-owned guest memory](../../layers/gpu/ownership.md), [ASTC](../../layers/gpu/astc.md), [Block linear](../../layers/gpu/blocklinear.md), [BCn without BCn](../../layers/gpu/bcn.md) |
| Video | host1x, NVDEC with FFmpeg, and VIC (`src/nx/gpu/video/`) | [VIC](../../layers/media/vic.md) |
| Audio | the audio outs, the audio device and the audio renderer's functions, answered in the SDK's own (`src/nx/nn/audio.c`, `audio_renderer.c` and their `.hook`), the renderer itself (`hle/services/audren.c`, `audren_dsp.c`), hwopus (`hle/services/audio.c`) | none yet |
| Input | hid (`hle/services/hid.c`, `hid_touch.c`, `hid_motion.c`), SDL gamepads (`runtime/platform/backend/gamepad.c`), the on-screen controls | [Usage, Playing](../../user/usage.md#playing), [Settings](../../user/settings.md) |
| Platform | the window or headless run, the menu in the game and the shader screen (`src/nx/runtime/platform/`), and the host layer that every driver shares (`src/common/platform/`) | [Architecture](../../dev/architecture.md#runtime-ownership), [the host systems](../../README.md#host-systems) |
