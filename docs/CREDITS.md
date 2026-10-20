<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Licenses and credits

ScarletNX is [GPL-3.0-or-later](../LICENSE).
Generated modules include runtime ABI code. Game assets, keys and firmware are excluded.

| Component | License |
| --- | --- |
| FFmpeg | LGPL-2.1-or-later |
| libvpx's VP9 test vectors (the WebM project's test data, downloaded for the `nvdec_vp9_*` tests, not shipped) | BSD-3-Clause |
| jsmolka's gba-tests (test ROMs, downloaded for the `agb_gba_*` tests, not shipped) | MIT |
| hactool / cJSON / ConvertUTF (`src/nx/runtime/driver/hactool`, with ScarletNX's changes) | ISC / MIT / Unicode license |
| LibHac storage code, ported into hactool (`src/nx/runtime/driver/hactool/storage.c`) | MIT |
| OpenSSL | Apache 2.0 |
| Open Sans 1.101 (the shared fonts' stand-in, `cmake/shared_fonts.cmake`) | Apache 2.0 ([text](../dist/licenses/OpenSans/LICENSE)) |
| yuzu's controller button glyphs (`font_nintendo_extended.cpp`, as Eden carries it) | GPL-2.0-or-later |
| LZ4 (the library, `lib/`) | BSD-2-Clause |
| Vulkan-Headers | Apache 2.0 |
| volk / Vulkan Memory Allocator | MIT |
| SDL3 | zlib |
| ENet / zstd (an Eden room's connection and its datagrams, `src/common/network/net_room.c`) | MIT / BSD-3-Clause |
| lunasvg / plutovg (the splash's SVGs and the user's picture) | MIT / MIT, with FreeType's rasterizer and stroker (FreeType License) and stb (MIT or public domain) |
| GUI VGA BIOS font (SeaBIOS `vgafont16`, originally Joseph Gil's font collection) | Public domain |
| GUI Latin letters past CP437 (GNU Unifont 18.0.01's 8x16 glyphs, `src/gui/unifont-latin.hex`, fitted to the VGA font) | GPL-2.0-or-later with the GNU font embedding exception, or OFL-1.1 |

The GUI fonts' notices and pinned sources are preserved in
[`src/gui/FONT-NOTICE.txt`](../src/gui/FONT-NOTICE.txt).

Code ported into the repository, and assets taken from other projects. In these places, the files whose header names
a source keep that source's copyright lines and license, most with the revision they came from; the drawables of
`dist/controls/` have theirs in [its README](../dist/controls/README.md):

| Where | From | License |
| --- | --- | --- |
| `src/nx/runtime/driver/hactool/storage.c` and `storage.h` (the storages added to hactool) | LibHac, as Ryubing maintains it | MIT |
| `src/3ds/gpu/`, `src/3ds/audio/`, `src/3ds/services/dsp.c` | Azahar (Citra) | GPL-2.0-or-later |
| `src/3ds/gpu/shader.c` and `shader.h` (the instruction fields) | nihstro | BSD-3-Clause |
| `src/common/cpu/` (the recompiler's instruction semantics: `a64.c`, `a64_templates.inc`, the LLVM emitters derived from them, the A32/T32 block compiler) | QEMU's `target/arm/tcg/` | LGPL-2.1-or-later |
| `src/common/cpu/aarch64/` (`cpu.h`, `cpu_helpers.c` and `helpers/`, the generated code's FP, SIMD, integer and crypto helpers) | QEMU's `target/arm/` and its softfloat, with the SoftFloat-2a notice | LGPL-2.1-or-later, SoftFloat-2a, BSD |
| `src/nx/gpu/gpu.c`, `src/nx/gpu/maxwell/` (the DMA pusher and puller, the 3D, macro, DMA, 2D and compute engines) | Eden and yuzu (`video_core/`) | GPL-3.0-or-later, GPL-2.0-or-later |
| `src/nx/gpu/shader/` (`shader.c`, `decode.c`, and `maxwell_opcodes.def`, the encodings `tools/shaderdec` generates the decoder from) | Eden and yuzu (`shader_recompiler/frontend/maxwell/`) | GPL-3.0-or-later, GPL-2.0-or-later |
| `src/nx/gpu/video/` (`host1x.c`, `nvdec.c`, `vic.c`) | Eden and yuzu (`video_core/host1x/`, the nvdrv host1x devices), with Ryujinx's CDMA pusher as Eden carries it | GPL-3.0-or-later, GPL-2.0-or-later, MIT |
| `src/nx/loader/loader.c` (the defaults for a program without an NPDM) | Eden and yuzu (`file_sys/program_metadata.cpp`) | GPL-3.0-or-later, GPL-2.0-or-later |
| `src/nx/hle/kernel/`, `src/nx/hle/svc/`, `src/nx/memory/`, `src/nx/hle/services/`, `src/nx/hle/filesystem/` | Eden and yuzu | GPL-3.0-or-later, GPL-2.0-or-later |
| `src/nx/hle/svc/hos.svc` (the SVC declarations and type sizes) and `tools/svcgen/svcgen.c` (their parsing and register allocation) | Eden and yuzu (`tools/svc_generator.py`) | GPL-3.0-or-later, GPL-2.0-or-later |
| `src/nx/hle/services/misc.c` (the six default Miis, from Eden's `mii/types/raw_data.cpp`) | Ryujinx, as Eden carries it | MIT ([text](../dist/licenses/Ryujinx/LICENSE.txt)) |
| `src/common/network/`, `src/nx/hle/services/network/` | Eden and yuzu | GPL-3.0-or-later, GPL-2.0-or-later |
| `src/common/network/net_room.c` (an Eden room's protocol) | Eden, yuzu and Citra (`src/network/room.cpp`, `room_member.cpp`, `packet.cpp`) | GPL-3.0-or-later, GPL-2.0-or-later |
| `src/nx/gpu/video/vp9_rebuild.cpp`, `src/nx/gpu/video/vic.comp` and `vic_compose.glsl` (compiled into SPIR-V at build time) | Eden and yuzu | GPL-3.0-or-later, GPL-2.0-or-later |
| `src/nx/gpu/texture/astc.comp` (the ASTC decoder shader, compiled into `astc_spv.h`) | Eden and yuzu (`astc_decoder.comp`) | GPL-3.0-or-later, GPL-2.0-or-later |
| `src/nx/gpu/texture/astc.c` and `astc.h` (the CPU ASTC decoder the tests check the shader against) | Eden and yuzu (`textures/astc.cpp`), with the University of North Carolina at Chapel Hill's FasTC decoder in it | GPL-3.0-or-later, GPL-2.0-or-later, Apache 2.0 |
| `tests/ipc_message.c` (the HIPC, CMIF and TIPC request builders) | libnx | ISC |
| `src/gui/i18n.c` (`snx_mo_lookup`, the reader of the menu's catalogs; notice in [`src/gui/MUSL-NOTICE.txt`](../src/gui/MUSL-NOTICE.txt)) | musl (`src/locale/__mo_lookup.c`) | MIT |
| `dist/controls/*.xml`, and `controls.png` made from them | yuzu's Android overlay, as Eden and rEden carry it | GPL-3.0-or-later |

Dynarmic (0BSD) and its GPL-3.0-or-later fork were consulted for the recompiler's instruction semantics. No code was
ported from them.

[Nexium](https://nexium-emu.org/) (MIT) is credited for its research and development, which SwitchRecomp, the
project ScarletNX started from, took as its basis.

Installed license texts: `share/licenses/scarletnx/`. AppImage system-library licenses
and package manifest: `share/licenses/`. Component license files contain the full terms.

Community credits: [README](../README.md#special-thanks).

Wimsy ([SVG metadata](../dist/wimsy.svg)): drawn by Liz, GPL-3.0-or-later.
The splash's Fuddles ([SVG metadata](../dist/splash/wimsy-fuddles.svg)): Wimsy with Fuddles' tails, circuits and
colours, by AO55 after Liz's Wimsy, GPL-3.0-or-later.
